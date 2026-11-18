#include "pch.h"
#include "CommsAudio.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

// XAudio2 requires _WIN32_WINNT >= 0x0602 (as SpatialAudioEngine.h does it).
#pragma push_macro("_WIN32_WINNT")
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <xaudio2.h>
#pragma pop_macro("_WIN32_WINNT")

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <json.hpp>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace Comms
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

double SecondsSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

unsigned WorkerCount()
{
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(hw / 2, 1u, 6u);
}

// Runs body(i) for i in [0, n) on a few threads.
template <typename F>
void ParallelFor(size_t n, F&& body)
{
    const unsigned workers = static_cast<unsigned>(std::min<size_t>(WorkerCount(), n));
    if (workers <= 1) { for (size_t i = 0; i < n; ++i) body(i); return; }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; ++w)
        pool.emplace_back([&] { for (size_t i; (i = next.fetch_add(1)) < n;) body(i); });
    for (auto& t : pool) t.join();
}

// In-place radix-2 FFT; a.size() must be a power of two.
template <typename T>
void Fft(std::vector<std::complex<T>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    std::vector<std::complex<T>> tw;
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const size_t half = len / 2;
        tw.resize(half);
        const double ang = 2.0 * kPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        for (size_t k = 0; k < half; ++k)
            tw[k] = std::complex<T>(static_cast<T>(std::cos(ang * k)), static_cast<T>(std::sin(ang * k)));
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < half; ++k)
            {
                const std::complex<T> u = a[i + k];
                const std::complex<T> v = a[i + k + half] * tw[k];
                a[i + k] = u + v;
                a[i + k + half] = u - v;
            }
    }
    if (inverse)
        for (auto& x : a) x /= static_cast<T>(n);
}

size_t NextPow2(size_t n)
{
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// Centred moving average of v over `radius` values each side (prefix sums).
std::vector<float> MovingAverage(const std::vector<float>& v, int radius)
{
    const size_t n = v.size();
    std::vector<double> pre(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) pre[i + 1] = pre[i] + v[i];
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i)
    {
        const size_t lo = i >= static_cast<size_t>(radius) ? i - radius : 0;
        const size_t hi = std::min(n, i + radius + 1);
        out[i] = static_cast<float>((pre[hi] - pre[lo]) / static_cast<double>(hi - lo));
    }
    return out;
}

// The same shaping for the match's envelope and the file's: what stands above its last second,
// at a level relative to its last ten seconds (a loud stretch of talk or a big fight does not
// outweigh the rest), clipped, then zero-mean for the correlation.
void Normalise(std::vector<float>& v)
{
    if (v.empty()) return;
    const std::vector<float> slow = MovingAverage(v, kEnvelopeRate / 2);
    for (size_t i = 0; i < v.size(); ++i) v[i] = std::max(0.f, v[i] - slow[i]);

    std::vector<float> sq(v.size());
    double total = 0.0;
    for (size_t i = 0; i < v.size(); ++i) { sq[i] = v[i] * v[i]; total += sq[i]; }
    const float globalRms = static_cast<float>(std::sqrt(total / static_cast<double>(v.size())));
    const std::vector<float> ms = MovingAverage(sq, kEnvelopeRate * 5);
    const float floorRms = std::max(1e-9f, 0.25f * globalRms);
    double mean = 0.0;
    for (size_t i = 0; i < v.size(); ++i)
    {
        v[i] = std::min(8.f, v[i] / (std::sqrt(ms[i]) + floorRms));
        mean += v[i];
    }
    mean /= static_cast<double>(v.size());
    for (auto& x : v) x -= static_cast<float>(mean);
}

// ---- The file's onset envelope: spectral flux above 1.5 kHz ---------------------------------
std::vector<float> ComputeOnset(const std::vector<int16_t>& pcm, uint32_t rate, const std::atomic<bool>& cancel)
{
    const size_t hop = std::max<size_t>(1, rate / kEnvelopeRate);
    const size_t frames = pcm.size() / hop;
    size_t win = 256;
    while (win < static_cast<size_t>(rate * 0.016)) win <<= 1;
    std::vector<float> hann(win);
    for (size_t i = 0; i < win; ++i) hann[i] = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * kPi * i / win));

    const double binHz = static_cast<double>(rate) / win;
    const size_t kLo = std::max<size_t>(1, static_cast<size_t>(1500.0 / binHz));
    const size_t kHi = std::min(win / 2, static_cast<size_t>(12000.0 / binHz));
    const size_t bins = kHi > kLo ? kHi - kLo : 1;
    const float scale = 2000.f / (static_cast<float>(win) * 32768.f);

    // Log magnitudes per frame (only the band), then the flux against the frame two back, the
    // neighbouring bins included (a held or gliding tone - a voice, music - gives little flux).
    // Chunked, each chunk with its own two frames of history, so memory stays small.
    std::vector<float> onset(frames, 0.f);
    const size_t chunk = 4096;
    const size_t chunks = (frames + chunk - 1) / chunk;
    ParallelFor(chunks, [&](size_t c) {
        if (cancel.load(std::memory_order_relaxed)) return;
        std::vector<std::complex<float>> buf(win);
        const size_t f1 = std::min(frames, (c + 1) * chunk);
        const size_t fStart = c * chunk >= 2 ? c * chunk - 2 : 0;
        std::vector<float> logmag((f1 - fStart) * bins, 0.f);
        for (size_t f = fStart; f < f1; ++f)
        {
            const size_t centre = f * hop;
            for (size_t i = 0; i < win; ++i)
            {
                const ptrdiff_t s = static_cast<ptrdiff_t>(centre + i) - static_cast<ptrdiff_t>(win / 2);
                const float x = (s >= 0 && static_cast<size_t>(s) < pcm.size()) ? pcm[s] : 0.f;
                buf[i] = std::complex<float>(x * hann[i], 0.f);
            }
            Fft(buf, false);
            float* row = &logmag[(f - fStart) * bins];
            for (size_t b = 0; b < bins; ++b)
                row[b] = std::log1p(std::abs(buf[kLo + b]) * scale);
        }
        for (size_t f = std::max<size_t>(2, c * chunk); f < f1; ++f)
        {
            const float* cur = &logmag[(f - fStart) * bins];
            const float* ref = &logmag[(f - 2 - fStart) * bins];
            float sum = 0.f;
            for (size_t b = 0; b < bins; ++b)
            {
                float r = ref[b];
                if (b > 0) r = std::max(r, ref[b - 1]);
                if (b + 1 < bins) r = std::max(r, ref[b + 1]);
                sum += std::max(0.f, cur[b] - r);
            }
            onset[f] = sum;
        }
    });
    Normalise(onset);
    return onset;
}

void BuildPeaks(Track& t)
{
    const size_t hop = std::max<size_t>(1, t.sampleRate / kEnvelopeRate);
    const size_t n = (t.pcm.size() + hop - 1) / hop;
    t.peakMin.assign(n, 0);
    t.peakMax.assign(n, 0);
    t.rms.assign(n, 0);
    int absMax = 1;
    for (size_t i = 0; i < n; ++i)
    {
        const size_t a = i * hop, b = std::min(t.pcm.size(), a + hop);
        int16_t lo = 0, hi = 0;
        double sq = 0.0;
        for (size_t s = a; s < b; ++s)
        {
            lo = std::min(lo, t.pcm[s]);
            hi = std::max(hi, t.pcm[s]);
            sq += static_cast<double>(t.pcm[s]) * t.pcm[s];
        }
        t.peakMin[i] = lo;
        t.peakMax[i] = hi;
        t.rms[i] = static_cast<uint16_t>(std::min(32767.0, std::sqrt(sq / static_cast<double>(std::max<size_t>(1, b - a)))));
        absMax = std::max({absMax, static_cast<int>(hi), -static_cast<int>(lo)});
    }
    t.peakAbs = static_cast<int16_t>(std::min(32767, absMax));
    std::vector<int16_t> mags(n);
    for (size_t i = 0; i < n; ++i) mags[i] = std::max<int16_t>(t.peakMax[i], static_cast<int16_t>(-std::max<int>(t.peakMin[i], -32767)));
    if (!mags.empty())
    {
        const size_t k = std::min(mags.size() - 1, static_cast<size_t>(mags.size() * 0.99));
        std::nth_element(mags.begin(), mags.begin() + k, mags.end());
        t.displayRef = std::max(64.f, static_cast<float>(mags[k]));
        std::vector<uint16_t> r = t.rms;
        const size_t k10 = r.size() / 10, k50 = r.size() / 2;
        std::nth_element(r.begin(), r.begin() + k10, r.end());
        t.rmsFloor = static_cast<float>(r[k10]);
        std::nth_element(r.begin(), r.begin() + k50, r.end());
        t.rmsMid = static_cast<float>(r[k50]);
    }
}

std::string HrText(HRESULT hr) { return std::format("0x{:08X}", static_cast<unsigned>(hr)); }

constexpr const char* kCannotRead = "This file can't be read. In OBS: File > Remux Recordings to mp4";

bool Decode(LoadJob& job, Track& out)
{
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(job.path.c_str(), nullptr, &reader);
    if (FAILED(hr)) { job.error = kCannotRead; job.detail = "open " + HrText(hr); return false; }

    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
    if (FAILED(hr)) { job.error = "This file has no audio track."; job.detail = "no audio stream " + HrText(hr); return false; }

    // Mono 16-bit at kTrackRate when the decoder offers it; its own PCM otherwise (mixed down and
    // brought down to kTrackRate below).
    auto makeType = [](bool full, bool asFloat) {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, asFloat ? MFAudioFormat_Float : MFAudioFormat_PCM);
        if (full)
        {
            t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
            t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kTrackRate);
            t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
            t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kTrackRate * 2);
            t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        }
        return t;
    };
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    hr = reader->SetCurrentMediaType(stream, nullptr, makeType(true, false).Get());
    if (FAILED(hr)) hr = reader->SetCurrentMediaType(stream, nullptr, makeType(false, false).Get());
    if (FAILED(hr)) hr = reader->SetCurrentMediaType(stream, nullptr, makeType(false, true).Get());
    if (FAILED(hr)) { job.error = kCannotRead; job.detail = "no PCM output " + HrText(hr); return false; }

    UINT32 channels = 1, rate = kTrackRate, bits = 16;
    bool isFloat = false;
    auto readType = [&]() {
        ComPtr<IMFMediaType> cur;
        if (FAILED(reader->GetCurrentMediaType(stream, &cur))) return;
        cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        cur->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        GUID sub{};
        cur->GetGUID(MF_MT_SUBTYPE, &sub);
        isFloat = sub == MFAudioFormat_Float;
        channels = std::max(1u, channels);
    };
    readType();
    if (rate == 0) { job.error = kCannotRead; job.detail = "no sample rate"; return false; }
    // The track's rate: the file's, at most kTrackRate (linear interpolation; a voice has little
    // above 16 kHz to fold back). Fixed once chosen, whatever the stream does later.
    const uint32_t outRate = std::min<uint32_t>(rate, kTrackRate);
    out.sampleRate = outRate;
    double phase = 0.0;      // position of the next output sample, in input samples since `prev`
    double prev = 0.0;

    double duration = 0.0;
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)))
        duration = static_cast<double>(var.uhVal.QuadPart) / 1e7;
    PropVariantClear(&var);
    if (duration > 0.0) out.pcm.reserve(static_cast<size_t>(duration * outRate) + outRate);

    for (;;)
    {
        if (job.cancel.load(std::memory_order_relaxed)) { job.detail = "cancelled"; return false; }
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(stream, 0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr)) { job.error = kCannotRead; job.detail = "decode " + HrText(hr); return false; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) readType();
        if (!sample) continue;

        // A gap in the stream (the first sample late, a dropped stretch) stays silence, so the
        // file's clock keeps its place.
        const double at = static_cast<double>(ts) / 1e7 * outRate;
        if (at > static_cast<double>(out.pcm.size()) + outRate * 0.05 && at - out.pcm.size() < outRate * 600.0)
            out.pcm.resize(static_cast<size_t>(at), 0);

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&data, nullptr, &len))) continue;
        const UINT32 bytes = std::max(1u, bits / 8);
        const size_t frameCount = len / (bytes * channels);
        for (size_t f = 0; f < frameCount; ++f)
        {
            double acc = 0.0;
            for (UINT32 c = 0; c < channels; ++c)
            {
                const BYTE* p = data + (f * channels + c) * bytes;
                if (isFloat && bytes == 4) { float v; memcpy(&v, p, 4); acc += v * 32767.0; }
                else if (bytes == 2) { int16_t v; memcpy(&v, p, 2); acc += v; }
                else if (bytes == 3) { const int v = (p[0] << 8 | p[1] << 16 | p[2] << 24) >> 16; acc += v; }
                else if (bytes == 4) { int32_t v; memcpy(&v, p, 4); acc += v / 65536.0; }
                else if (bytes == 1) { acc += (static_cast<int>(p[0]) - 128) * 256.0; }
            }
            acc /= channels;
            if (rate == outRate)
                out.pcm.push_back(static_cast<int16_t>(std::clamp(acc, -32768.0, 32767.0)));
            else
            {
                // Output samples between `prev` (input 0) and `acc` (input 1).
                const double step = static_cast<double>(rate) / outRate;
                while (phase <= 1.0)
                {
                    const double v = prev + (acc - prev) * phase;
                    out.pcm.push_back(static_cast<int16_t>(std::clamp(v, -32768.0, 32767.0)));
                    phase += step;
                }
                phase -= 1.0;
                prev = acc;
            }
        }
        buf->Unlock();
        if (duration > 0.0)
            job.progress.store(static_cast<float>(std::min(1.0, out.pcm.size() / (duration * outRate))) * 0.8f,
                               std::memory_order_relaxed);
    }
    if (out.pcm.size() < outRate / 2) { job.error = "This file has no sound in it."; job.detail = "no samples"; return false; }
    return true;
}
} // namespace

// ---- Loading --------------------------------------------------------------------------------
void RunLoad(LoadJob& job)
{
    const auto t0 = std::chrono::steady_clock::now();
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool mf = SUCCEEDED(MFStartup(MF_VERSION));
    auto track = std::make_shared<Track>();
    if (!mf) { job.error = kCannotRead; job.detail = "MFStartup failed"; }
    else if (Decode(job, *track))
    {
        BuildPeaks(*track);
        job.progress.store(0.85f);
        track->onset = ComputeOnset(track->pcm, track->sampleRate, job.cancel);
        if (!job.cancel.load())
        {
            job.track = std::move(track);
            job.ok = true;
        }
    }
    if (mf) MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    job.progress.store(1.f);
    job.seconds = SecondsSince(t0);
    job.done.store(true, std::memory_order_release);
}

// ---- The match's envelope -------------------------------------------------------------------
namespace
{
// How loud a sound `d` units from a listener is to him (the client's own falloff is per
// category; one curve is close enough for timing).
float Falloff(float d)
{
    if (d < 300.f) return 1.f;
    if (d > 3500.f) return 0.f;
    const float u = (d - 300.f) / 700.f;
    return 1.f / (1.f + u * u);
}
}

std::vector<float> BuildGameEnvelope(const GameModel& model, int listener)
{
    const size_t n = static_cast<size_t>(std::max(0.f, model.duration) * kEnvelopeRate) + 2;
    std::vector<float> env(n, 0.f);
    const Listener* L = (listener >= 0 && listener < static_cast<int>(model.listeners.size()))
        ? &model.listeners[listener] : nullptr;
    for (const GameEvent& e : model.events)
    {
        if (e.time < 0.f) continue;
        float w = e.weight;
        if (L && e.a != -1)   // a == -1: a sound every player hears (the victory fanfare)
        {
            if (e.a == L->agentId || e.b == L->agentId) w *= 2.f;
            else if (e.hasPos && !L->x.empty())
            {
                const size_t i = std::min(L->x.size() - 1, static_cast<size_t>(e.time * L->rate + 0.5f));
                const float dx = e.x - L->x[i], dy = e.y - L->y[i];
                w *= Falloff(std::sqrt(dx * dx + dy * dy));
            }
            else w *= 0.3f;
        }
        if (w <= 0.f) continue;
        const float pos = e.time * kEnvelopeRate;
        const size_t i0 = static_cast<size_t>(pos);
        if (i0 + 1 >= n) continue;
        const float frac = pos - static_cast<float>(i0);
        env[i0] += w * (1.f - frac);
        env[i0 + 1] += w * frac;
    }
    // A sound is heard over a few tens of milliseconds, and the packet is not the sound to the
    // frame: a short Gaussian (sigma 15 ms) gives the correlation that tolerance.
    constexpr int kR = 4;
    float kernel[2 * kR + 1];
    for (int k = -kR; k <= kR; ++k) kernel[k + kR] = std::exp(-0.5f * (k / 1.5f) * (k / 1.5f));
    std::vector<float> smooth(n, 0.f);
    for (size_t i = 0; i < n; ++i)
    {
        if (env[i] == 0.f) continue;
        for (int k = -kR; k <= kR; ++k)
        {
            const ptrdiff_t j = static_cast<ptrdiff_t>(i) + k;
            if (j >= 0 && static_cast<size_t>(j) < n) smooth[j] += env[i] * kernel[k + kR];
        }
    }
    for (auto& v : smooth) v = std::log1p(v);
    Normalise(smooth);
    return smooth;
}

std::vector<float> BuildGameActivity(const GameModel& model)
{
    std::vector<float> env = BuildGameEnvelope(model, -1);
    for (auto& v : env) v = std::max(0.f, v);
    env = MovingAverage(env, kEnvelopeRate * 15 / 100);
    if (env.empty()) return env;
    std::vector<float> tmp = env;
    const size_t k = std::min(tmp.size() - 1, static_cast<size_t>(tmp.size() * 0.99));
    std::nth_element(tmp.begin(), tmp.begin() + k, tmp.end());
    const float ref = std::max(1e-6f, tmp[k]);
    for (auto& v : env) v = std::min(1.5f, v / ref);
    return env;
}

// ---- The correlation ------------------------------------------------------------------------
void RunSync(SyncJob& job)
{
    const auto t0 = std::chrono::steady_clock::now();
    SyncResult& r = job.result;
    auto finish = [&] {
        r.seconds = SecondsSince(t0);
        job.done.store(true, std::memory_order_release);
    };
    if (!job.track || job.track->onset.empty() || job.model.duration <= 1.f)
    {
        r.why = "nothing to correlate";
        finish();
        return;
    }

    const std::vector<float>& f = job.track->onset;
    const size_t Lf = f.size();
    const size_t Lg = static_cast<size_t>(job.model.duration * kEnvelopeRate) + 2;
    const size_t N = NextPow2(Lf + Lg);
    // Enough overlap to mean something: half the shorter of the two, between 10 s and 60 s.
    const double minOverlap = std::clamp(0.5 * static_cast<double>(std::min(Lf, Lg)), 10.0 * kEnvelopeRate, 60.0 * kEnvelopeRate);

    std::vector<std::complex<double>> F(N);
    for (size_t i = 0; i < Lf; ++i) F[i] = f[i];
    Fft(F, false);
    job.progress.store(0.1f);

    const int candidates = 1 + static_cast<int>(job.model.listeners.size());
    struct Cand { double z = 0, ratio = 0, lag = 0; bool ok = false; };
    std::vector<Cand> res(candidates);
    std::atomic<int> finished{0};

    ParallelFor(static_cast<size_t>(candidates), [&](size_t ci) {
        if (job.cancel.load(std::memory_order_relaxed)) return;
        const std::vector<float> g = BuildGameEnvelope(job.model, static_cast<int>(ci) - 1);
        std::vector<std::complex<double>> G(N);
        for (size_t i = 0; i < std::min(g.size(), N); ++i) G[i] = g[i];
        Fft(G, false);
        for (size_t i = 0; i < N; ++i) G[i] = std::conj(G[i]) * F[i];
        Fft(G, true);

        // c[k] = sum_t g[t] f[t + k]; k < 0 wraps to N + k. Scaled by the overlap's square root,
        // so every lag has the same noise.
        const ptrdiff_t kMin = -static_cast<ptrdiff_t>(Lg) + 1, kMax = static_cast<ptrdiff_t>(Lf) - 1;
        std::vector<float> score;
        std::vector<ptrdiff_t> lags;
        score.reserve(Lf + Lg);
        lags.reserve(Lf + Lg);
        for (ptrdiff_t k = kMin; k <= kMax; ++k)
        {
            const double overlap = static_cast<double>(std::min<ptrdiff_t>(static_cast<ptrdiff_t>(Lg), static_cast<ptrdiff_t>(Lf) - k) -
                                                       std::max<ptrdiff_t>(0, -k));
            if (overlap < minOverlap) continue;
            const size_t idx = k >= 0 ? static_cast<size_t>(k) : N - static_cast<size_t>(-k);
            score.push_back(static_cast<float>(G[idx].real() / std::sqrt(overlap)));
            lags.push_back(k);
        }
        Cand c;
        if (score.size() > 200)
        {
            std::vector<float> tmp;
            tmp.reserve(score.size() / 3 + 1);
            for (size_t i = 0; i < score.size(); i += 3) tmp.push_back(score[i]);
            std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
            const float med = tmp[tmp.size() / 2];
            for (auto& x : tmp) x = std::fabs(x - med);
            std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
            const double sigma = std::max(1e-12, 1.4826 * tmp[tmp.size() / 2]);

            size_t best = 0;
            for (size_t i = 1; i < score.size(); ++i) if (score[i] > score[best]) best = i;
            float second = -1e30f;
            for (size_t i = 0; i < score.size(); ++i)
                if (std::abs(lags[i] - lags[best]) > kEnvelopeRate && score[i] > second) second = score[i];

            // Parabolic refinement between the neighbouring lags.
            double lag = static_cast<double>(lags[best]);
            if (best > 0 && best + 1 < score.size() && lags[best - 1] == lags[best] - 1 && lags[best + 1] == lags[best] + 1)
            {
                const double a = score[best - 1], b = score[best], cc = score[best + 1];
                const double den = a - 2 * b + cc;
                if (std::fabs(den) > 1e-12) lag += std::clamp(0.5 * (a - cc) / den, -0.5, 0.5);
            }
            c.z = (score[best] - med) / sigma;
            const double z2 = (second - med) / sigma;
            c.ratio = z2 > 0.0 ? c.z / z2 : 99.0;
            c.lag = lag;
            c.ok = true;
        }
        res[ci] = c;
        const int done = ++finished;
        job.progress.store(0.1f + 0.9f * done / candidates);
    });

    if (job.cancel.load())
    {
        r.why = "cancelled";
        finish();
        return;
    }
    int bestCand = -1;
    for (int i = 0; i < candidates; ++i)
        if (res[i].ok && (bestCand < 0 || res[i].z > res[bestCand].z)) bestCand = i;
    if (bestCand < 0)
    {
        r.why = "the file and the match overlap too little";
        finish();
        return;
    }
    const Cand& b = res[bestCand];
    r.ok = true;
    r.offset = b.lag / kEnvelopeRate;
    r.z = b.z;
    r.ratio = b.ratio;
    r.listener = bestCand - 1;
    r.confidence = std::clamp((b.z - 6.0) / (2.0 * (kConfidentZ - 6.0)), 0.0, 1.0) *
                   std::clamp((b.ratio - 1.0) / (2.0 * (kConfidentRatio - 1.0)), 0.0, 1.0);
    // A tall peak, or a less tall one that stands well clear of every other lag.
    r.confident = (b.z >= kConfidentZ && b.ratio >= kConfidentRatio) ||
                  (b.z >= kConfidentZClear && b.ratio >= kConfidentRatioClear);
    finish();
}

// ---- Playback -------------------------------------------------------------------------------
Player::~Player() { Shutdown(); }

void Player::Shutdown()
{
    Stop();
    if (m_master) { m_master->DestroyVoice(); m_master = nullptr; }
    if (m_xaudio) { m_xaudio->Release(); m_xaudio = nullptr; }
}

bool Player::EnsureEngine()
{
    if (m_xaudio) return true;
    if (m_engineFailed) return false;
    if (FAILED(XAudio2Create(&m_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)))
    {
        m_xaudio = nullptr;
        m_engineFailed = true;
        return false;
    }
    if (FAILED(m_xaudio->CreateMasteringVoice(&m_master)))
    {
        m_master = nullptr;
        m_xaudio->Release();
        m_xaudio = nullptr;
        m_engineFailed = true;
        return false;
    }
    return true;
}

bool Player::Start(const std::shared_ptr<const Track>& track, size_t startSample, float ratio, float volume)
{
    Stop();
    if (!track || track->pcm.empty() || startSample >= track->pcm.size() || !EnsureEngine()) return false;

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = track->sampleRate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = track->sampleRate * 2;
    if (FAILED(m_xaudio->CreateSourceVoice(&m_voice, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO)))
    {
        m_voice = nullptr;
        return false;
    }
    XAUDIO2_BUFFER buf{};
    buf.Flags = XAUDIO2_END_OF_STREAM;
    buf.AudioBytes = static_cast<UINT32>(std::min<size_t>(track->pcm.size() * 2, XAUDIO2_MAX_BUFFER_BYTES));
    buf.pAudioData = reinterpret_cast<const BYTE*>(track->pcm.data());
    buf.PlayBegin = static_cast<UINT32>(startSample);
    buf.PlayLength = 0;
    if (FAILED(m_voice->SubmitSourceBuffer(&buf)))
    {
        m_voice->DestroyVoice();
        m_voice = nullptr;
        return false;
    }
    m_track = track;
    m_start = startSample;
    m_ratio = std::clamp(ratio, XAUDIO2_MIN_FREQ_RATIO, XAUDIO2_DEFAULT_FREQ_RATIO);
    m_volume = volume;
    m_voice->SetFrequencyRatio(m_ratio);
    m_voice->SetVolume(m_volume);
    m_voice->Start(0);
    return true;
}

void Player::Stop()
{
    if (m_voice)
    {
        m_voice->Stop(0);
        m_voice->DestroyVoice();
        m_voice = nullptr;
    }
    m_track.reset();
}

double Player::Position() const
{
    if (!m_voice || !m_track) return 0.0;
    XAUDIO2_VOICE_STATE st{};
    m_voice->GetState(&st, 0);
    return static_cast<double>(m_start + st.SamplesPlayed) / m_track->sampleRate;
}

bool Player::Finished() const
{
    if (!m_voice) return true;
    XAUDIO2_VOICE_STATE st{};
    m_voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return st.BuffersQueued == 0;
}

void Player::SetRatio(float ratio)
{
    ratio = std::clamp(ratio, XAUDIO2_MIN_FREQ_RATIO, XAUDIO2_DEFAULT_FREQ_RATIO);
    if (m_voice && ratio != m_ratio) { m_voice->SetFrequencyRatio(ratio); m_ratio = ratio; }
}

void Player::SetVolume(float volume)
{
    if (m_voice && volume != m_volume) { m_voice->SetVolume(volume); m_volume = volume; }
}

// ---- Memory ---------------------------------------------------------------------------------
std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string FormatOffset(double seconds)
{
    const double r = std::round(seconds * 10.0) / 10.0;
    return std::format("{}{:.1f} s", r < 0.0 ? "-" : "+", std::fabs(r));
}

std::string ClockText(double seconds)
{
    const bool neg = seconds < 0.0;
    long long t = static_cast<long long>(std::floor(std::fabs(seconds) + 0.5));
    const long long h = t / 3600, m = (t / 60) % 60, sec = t % 60;
    if (h > 0) return std::format("{}{}:{:02}:{:02}", neg ? "-" : "", h, m, sec);
    return std::format("{}{}:{:02}", neg ? "-" : "", m, sec);
}

namespace
{
nlohmann::json ReadStore(const std::filesystem::path& store)
{
    std::ifstream in(store);
    if (!in.is_open()) return nlohmann::json::object();
    try
    {
        nlohmann::json j;
        in >> j;
        if (j.is_object()) return j;
    }
    catch (...) {}
    return nlohmann::json::object();
}

void WriteStore(const std::filesystem::path& store, const nlohmann::json& j)
{
    std::error_code ec;
    std::filesystem::create_directories(store.parent_path(), ec);
    std::ofstream out(store);
    if (out.is_open()) out << j.dump(2) << "\n";
}
}

bool LoadSaved(const std::filesystem::path& store, const std::string& folder, Saved& out)
{
    const nlohmann::json j = ReadStore(store);
    const auto it = j.find(folder);
    if (it == j.end() || !it->is_object()) return false;
    try
    {
        out.path = FromUtf8(it->value("path", std::string()));
        out.size = it->value("size", uint64_t{0});
        out.writeTime = it->value("write_time", int64_t{0});
        out.offset = it->value("offset", 0.0);
        out.volume = std::clamp(it->value("volume", 1.f), 0.f, 1.f);
        out.muted = it->value("muted", false);
    }
    catch (...) { return false; }
    return !out.path.empty();
}

void SaveEntry(const std::filesystem::path& store, const std::string& folder, const Saved& e)
{
    nlohmann::json j = ReadStore(store);
    j[folder] = {
        {"path", ToUtf8(e.path)}, {"size", e.size}, {"write_time", e.writeTime},
        {"offset", std::round(e.offset * 1000.0) / 1000.0}, {"volume", e.volume}, {"muted", e.muted},
    };
    WriteStore(store, j);
    InvalidateBadges();
}

void ForgetEntry(const std::filesystem::path& store, const std::string& folder)
{
    nlohmann::json j = ReadStore(store);
    if (j.erase(folder) > 0) WriteStore(store, j);
    InvalidateBadges();
}

// ---- The Library's badges ---------------------------------------------------------------------
namespace
{
std::mutex g_badgeMutex;
std::unordered_map<std::string, Badge> g_badges;
bool g_badgesValid = false;
double g_badgesLoadedAt = 0.0, g_badgesCheckedAt = 0.0;
int64_t g_storeStamp = 0;
uint64_t g_badgeVersion = 0;   // +1 with every reload (the Library re-sorts its comms column)

int64_t StoreStamp(const std::filesystem::path& store)
{
    uint64_t size = 0;
    int64_t wt = 0;
    return FileStamp(store.wstring(), size, wt) ? wt ^ static_cast<int64_t>(size) : 0;
}
}

void InvalidateBadges()
{
    std::lock_guard<std::mutex> lock(g_badgeMutex);
    g_badgesValid = false;
}

Badge BadgeFor(const std::string& folder)
{
    std::lock_guard<std::mutex> lock(g_badgeMutex);
    const double now = GetTickCount64() / 1000.0;
    const std::filesystem::path store = DefaultStorePath();
    // Another window (or instance) may have written the store: one stat every 2 s says so. The
    // recordings' existence is re-checked with every reload, and at least every 30 s.
    if (g_badgesValid && now - g_badgesCheckedAt > 2.0)
    {
        g_badgesCheckedAt = now;
        if (StoreStamp(store) != g_storeStamp || now - g_badgesLoadedAt > 30.0) g_badgesValid = false;
    }
    if (!g_badgesValid)
    {
        g_badges.clear();
        const nlohmann::json j = ReadStore(store);
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            if (!it->is_object()) continue;
            Badge b;
            b.present = true;
            b.path = it->value("path", std::string());
            const std::wstring wp = FromUtf8(b.path);
            b.name = ToUtf8(std::filesystem::path(wp).filename().wstring());
            uint64_t size = 0;
            int64_t wt = 0;
            b.exists = !wp.empty() && FileStamp(wp, size, wt);
            g_badges[it.key()] = std::move(b);
        }
        g_storeStamp = StoreStamp(store);
        ++g_badgeVersion;
        g_badgesValid = true;
        g_badgesLoadedAt = g_badgesCheckedAt = now;
    }
    const auto it = g_badges.find(folder);
    return it != g_badges.end() ? it->second : Badge{};
}

uint64_t BadgeVersion()
{
    BadgeFor(std::string());   // refreshes the cache when it is due
    std::lock_guard<std::mutex> lock(g_badgeMutex);
    return g_badgeVersion;
}

bool FileStamp(const std::wstring& path, uint64_t& size, int64_t& writeTime)
{
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return false;
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    writeTime = static_cast<int64_t>((static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) |
                                     fa.ftLastWriteTime.dwLowDateTime);
    return true;
}

// ---- Self-test ------------------------------------------------------------------------------
bool WriteWavMonoAsStereo(const std::filesystem::path& path, const std::vector<float>& mono, uint32_t rate)
{
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    const uint16_t channels = 2, bits = 16;
    const uint32_t dataBytes = static_cast<uint32_t>(mono.size() * channels * 2);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4); u32(36 + dataBytes); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(1); u16(channels); u32(rate); u32(rate * channels * 2); u16(channels * 2); u16(bits);
    out.write("data", 4); u32(dataBytes);
    std::vector<int16_t> block;
    constexpr size_t kBlock = 65536;
    for (size_t i = 0; i < mono.size(); i += kBlock)
    {
        const size_t n = std::min(kBlock, mono.size() - i);
        block.resize(n * 2);
        for (size_t k = 0; k < n; ++k)
        {
            const int16_t v = static_cast<int16_t>(std::clamp(mono[i + k], -1.f, 1.f) * 32767.f);
            block[2 * k] = v;
            block[2 * k + 1] = v;
        }
        out.write(reinterpret_cast<const char*>(block.data()), static_cast<std::streamsize>(block.size() * 2));
    }
    return out.good();
}
} // namespace Comms
