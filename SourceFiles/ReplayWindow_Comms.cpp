#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "CommsAudio.h"
#include "RunLog.h"
#include "UrlProtocol.h"
#include "SpatialAudioEngine.h"
#include <imgui_internal.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <thread>

// =================================================================================================
// COMMS AUDIO IN THE REPLAY - the module is CommsAudio.h.
//
// A file dropped onto the replay window (WM_DROPFILES) is decoded on a worker thread, synced
// against the match's sound events on another, and played at match time + offset by its own
// XAudio2 voice, which follows play / pause / scrub / speed (up to 2x; silent above it). The lane
// under the play bar shows it as a clip on the match's timeline, its offset, and its menu; the Sync
// editor over the play bar is a two-track timeline in match time where the clip slides into place.
//
// Remembered per match folder in settings/comms_audio.json (on this PC only); a quiet test instance
// (GWO_INSTANCE_TAG) keeps a file of its own, comms_audio_<tag>.json.
// =================================================================================================

namespace Comms
{
namespace
{
double NowSeconds() { return GetTickCount64() / 1000.0; }

// The strip's messages: shown this long, then faded out over kStatusFade.
constexpr double kStatusShow = 4.0, kStatusFade = 0.6;

// The master slider's curve, as SpatialAudioEngine.cpp shapes it (kMasterCurveExponent).
float MasterGainOf(float slider) { return std::pow(std::clamp(slider, 0.f, 1.f), 2.36f); }

std::filesystem::path StorePath()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring name = L"comms_audio";
    wchar_t tag[64] = {};
    if (UrlProtocol::QuietTestInstance() && GetEnvironmentVariableW(L"GWO_INSTANCE_TAG", tag, 64) > 0)
        name += std::wstring(L"_") + tag;
    return std::filesystem::path(exe).parent_path() / "settings" / (name + L".json");
}

struct DialogState
{
    std::atomic<bool> done{false};
    bool ok = false;
    std::wstring path;
};

// The shell's Open dialog needs a single-threaded apartment (the UI thread is multithreaded), so
// it runs on a short-lived thread of its own, as the clip's Save dialog does.
void RunOpenDialog(HWND owner, std::shared_ptr<DialogState> st)
{
    using Microsoft::WRL::ComPtr;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
    {
        {
            ComPtr<IFileOpenDialog> dlg;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
            {
                const COMDLG_FILTERSPEC types[] = {
                    {L"Audio and video", L"*.mp3;*.m4a;*.aac;*.wav;*.wma;*.flac;*.ogg;*.opus;*.mp4;*.mov;*.mkv;*.webm;*.wmv"},
                    {L"All files", L"*.*"},
                };
                dlg->SetFileTypes(2, types);
                dlg->SetTitle(L"Comms recording");
                DWORD opts = 0;
                if (SUCCEEDED(dlg->GetOptions(&opts)))
                    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
                ComPtr<IShellItem> item;
                PWSTR p = nullptr;
                if (SUCCEEDED(dlg->Show(owner)) && SUCCEEDED(dlg->GetResult(&item)) &&
                    SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p)
                {
                    st->path = p;
                    st->ok = true;
                }
                if (p) CoTaskMemFree(p);
            }
        }
        CoUninitialize();
    }
    st->done.store(true, std::memory_order_release);
}

// ---- Self-test ------------------------------------------------------------------------------
struct SelfTest
{
    bool on = false;
    std::string mode = "sync";   // sync | hard | nogame | reattach | file
    double offset = 47.3;
    std::filesystem::path dir;
    std::wstring file;           // mode file: the recording to attach (GWO_COMMS_SELFTEST_FILE)
    std::vector<std::string> folders;
};

const SelfTest& SelfTestCfg()
{
    static const SelfTest cfg = [] {
        SelfTest t;
        char buf[4096] = {};
        if (GetEnvironmentVariableA("GWO_COMMS_SELFTEST", buf, sizeof(buf)) == 0) return t;
        t.on = true;
        std::string v = buf;
        size_t a = 0;
        while (a <= v.size())
        {
            size_t b = v.find(';', a);
            if (b == std::string::npos) b = v.size();
            std::string item = v.substr(a, b - a);
            if (!item.empty()) t.folders.push_back(std::filesystem::path(item).filename().string());
            a = b + 1;
        }
        char m[64] = {};
        if (GetEnvironmentVariableA("GWO_COMMS_SELFTEST_MODE", m, sizeof(m)) > 0) t.mode = m;
        char o[64] = {};
        if (GetEnvironmentVariableA("GWO_COMMS_SELFTEST_OFFSET", o, sizeof(o)) > 0) t.offset = std::strtod(o, nullptr);
        wchar_t d[MAX_PATH] = {};
        wchar_t f[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"GWO_COMMS_SELFTEST_FILE", f, MAX_PATH) > 0) t.file = f;
        if (GetEnvironmentVariableW(L"GWO_COMMS_SELFTEST_DIR", d, MAX_PATH) > 0) t.dir = d;
        else
        {
            GetTempPathW(MAX_PATH, d);
            t.dir = d;
        }
        return t;
    }();
    return cfg;
}

struct SynthJob
{
    std::atomic<bool> done{false};
    bool ok = false;
    std::filesystem::path path;
    std::string info;
};

// A recording to sync without a recording: the match's sounds as heard by `listener` (a falloff
// and a loudness of its own, not the sync's; a third of them missing, jittered by 20 ms and late by
// 40 ms), under two people talking 12 dB louder (voiced syllables through moving formants, and
// fricatives - transients the sync has to see through), over a noise floor (`hard`: 60 % missing,
// the talk 24 dB louder). Without game sounds
// (`withGame` false): only the talk, the floor and keyboard clicks at random.
void Synthesise(const GameModel& m, int listener, double offset, bool withGame, bool hard, uint32_t seed, SynthJob& job)
{
    constexpr uint32_t kRate = 24000;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> U(0.f, 1.f);
    std::normal_distribution<float> N(0.f, 1.f);

    const double fileDur = m.duration + offset + 20.0;
    if (fileDur < 60.0) { job.info = "too short"; job.done = true; return; }
    const size_t len = static_cast<size_t>(fileDur * kRate);

    std::vector<float> game(len, 0.f), talk(len, 0.f);
    const Listener* L = (listener >= 0 && listener < static_cast<int>(m.listeners.size())) ? &m.listeners[listener] : nullptr;
    size_t placed = 0;
    if (withGame)
    {
        for (const GameEvent& e : m.events)
        {
            float w = e.weight;
            if (e.a == -1) {}
            else if (L && (e.a == L->agentId || e.b == L->agentId)) {}
            else if (L && e.hasPos && !L->x.empty())
            {
                const size_t i = std::min(L->x.size() - 1, static_cast<size_t>(e.time * L->rate));
                const float d = std::hypot(e.x - L->x[i], e.y - L->y[i]);
                w *= 0.5f * std::pow(std::max(0.f, 1.f - d / 2500.f), 1.5f);
            }
            else w *= 0.15f;
            if (w <= 0.01f || U(rng) < (hard ? 0.6f : 0.35f)) continue;
            const double t = e.time + offset + 0.04 + 0.02 * N(rng);
            if (t < 0.0 || t >= fileDur - 1.0) continue;
            const size_t s0 = static_cast<size_t>(t * kRate);
            const float amp = 0.3f * w * (0.6f + 0.8f * U(rng));
            const int kind = static_cast<int>(U(rng) * 3.f);
            const float tau = kind == 0 ? 0.03f : kind == 1 ? 0.15f : 0.10f;
            const float cut = 0.05f + 0.6f * U(rng);
            const size_t n = static_cast<size_t>(std::min(0.6f, tau * 5.f) * kRate);
            float lp = 0.f;
            for (size_t k = 0; k < n && s0 + k < len; ++k)
            {
                const float tt = static_cast<float>(k) / kRate;
                float env = std::exp(-tt / tau);
                if (kind == 1) env *= std::min(1.f, tt / 0.01f);
                if (kind == 2) env *= std::min(1.f, tt / 0.04f);
                const float x = N(rng);
                lp += cut * (x - lp);
                const float y = kind == 0 ? x - lp : lp;
                game[s0 + k] += amp * env * y;
            }
            ++placed;
        }
    }

    // Two speakers taking turns.
    auto resonate = [](float x, float f, float bw, float& y1, float& y2) {
        const float r = std::exp(-3.14159f * bw / kRate);
        const float c = 2.f * r * std::cos(2.f * 3.14159f * f / kRate);
        const float y = (1.f - r) * x + c * y1 - r * r * y2;
        y2 = y1;
        y1 = y;
        return y;
    };
    double t = 1.0;
    int speaker = 0;
    while (t < fileDur - 2.0)
    {
        const double spurt = 1.5 + 5.5 * U(rng);
        const float f0 = (speaker == 0 ? 110.f : 190.f) * (0.92f + 0.16f * U(rng));
        double st = t;
        float a1 = 0, a2 = 0, b1 = 0, b2 = 0, hp = 0;
        while (st < t + spurt && st < fileDur - 1.0)
        {
            const double syl = 0.12 + 0.16 * U(rng);
            const float amp = 0.4f + 0.6f * U(rng);
            const float F1 = 300.f + 600.f * U(rng), F2 = 900.f + 1600.f * U(rng);
            const bool fric = U(rng) < 0.3f;
            const size_t s0 = static_cast<size_t>(st * kRate), n = static_cast<size_t>(syl * kRate);
            float phase = 0.f;
            for (size_t k = 0; k < n && s0 + k < len; ++k)
            {
                const float u = static_cast<float>(k) / n;
                const float env = std::sin(3.14159f * u);
                phase += f0 / kRate;
                float src = 0.f;
                if (phase >= 1.f) { phase -= 1.f; src = 1.f; }
                src += 0.05f * N(rng);
                float v = resonate(src, F1, 80.f, a1, a2) + 0.6f * resonate(src, F2, 120.f, b1, b2);
                if (fric && u < 0.35f)
                {
                    const float x = N(rng);
                    hp += 0.5f * (x - hp);
                    v += 0.25f * (x - hp) * (1.f - u / 0.35f);
                }
                talk[s0 + k] += amp * env * v;
            }
            st += syl + 0.02 + 0.06 * U(rng);
        }
        t += spurt + 0.2 + 2.8 * U(rng);
        speaker ^= 1;
    }

    auto rms = [&](const std::vector<float>& v) {
        double s = 0;
        for (float x : v) s += double(x) * x;
        return std::sqrt(s / std::max<size_t>(1, v.size()));
    };
    const double gameRms = rms(game), talkRms = rms(talk);
    const double talkGain = talkRms > 0 ? (withGame && gameRms > 0 ? (hard ? 16.0 : 4.0) * gameRms : 0.08) / talkRms : 0.0;
    std::vector<float> mix(len);
    float lp = 0.f;
    const float floorAmp = static_cast<float>(withGame ? gameRms * 0.1 : 0.004);
    for (size_t i = 0; i < len; ++i)
    {
        lp += 0.2f * (N(rng) - lp);
        mix[i] = game[i] + static_cast<float>(talkGain) * talk[i] + floorAmp * lp;
    }
    if (!withGame)
    {
        // Keyboard and mouse clicks, about two a second.
        for (double c = 0.5; c < fileDur - 1.0; c += -std::log(std::max(1e-6f, U(rng))) / 2.0)
        {
            const size_t s0 = static_cast<size_t>(c * kRate);
            const float amp = 0.05f * (0.5f + U(rng));
            for (size_t k = 0; k < kRate / 200 && s0 + k < len; ++k)
                mix[s0 + k] += amp * N(rng) * std::exp(-static_cast<float>(k) / (kRate / 1000.f));
        }
    }
    float peak = 1e-6f;
    for (float x : mix) peak = std::max(peak, std::fabs(x));
    for (auto& x : mix) x *= 0.9f / peak;

    job.ok = WriteWavMonoAsStereo(job.path, mix, kRate);
    job.info = std::format("{:.1f} min, {} game sounds placed, speech/game rms {:.1f} dB", fileDur / 60.0, placed,
                           gameRms > 0 ? 20.0 * std::log10(talkGain * talkRms / gameRms) : 0.0);
    job.done.store(true, std::memory_order_release);
}
} // namespace

std::filesystem::path DefaultStorePath() { return StorePath(); }

struct LaneState
{
    enum class Phase { None, Loading, Ready, Failed, Missing };
    Phase phase = Phase::None;
    std::wstring path;
    std::string name;
    uint64_t size = 0;
    int64_t writeTime = 0;
    std::shared_ptr<Track> track;
    double offset = 0.0;
    float volume = 1.f;
    bool muted = false;
    bool restored = false;       // the offset came with it: no auto sync

    std::shared_ptr<LoadJob> load;
    std::thread loadThread;
    std::shared_ptr<SyncJob> sync;
    std::thread syncThread;
    bool syncPending = false;
    double syncWantedAt = 0.0;
    bool hasSync = false;
    SyncResult lastSync;

    std::string status;
    bool statusError = false;
    double statusUntil = 0.0;    // 0 = until replaced (a failed load's message, in its own phase)
    double statusSetAt = 0.0;    // over the waveform (Ready): a toast, kStatusShow s then a fade

    bool storeChecked = false;
    bool dirty = false;
    std::wstring pendingDrop;

    Player player;
    bool noDevice = false;
    int reseeks = 0;
    int driftFrames = 0;
    double driftSum = 0.0, driftMax = 0.0;
    int driftCount = 0;

    // The lane's drag: 0 none, 1 the clip, 2 a scrub.
    int laneDrag = 0;
    bool dragMoved = false;
    float dragX0 = 0.f;
    double dragOffset0 = 0.0;
    // The offset moved (a drag, a nudge, a snap): the voice re-seeks at once while playing.
    bool seekNow = false;
    double lastSeekAt = 0.0;

    std::shared_ptr<DialogState> dialog;
    bool dialogKeepsOffset = false;

    // The Sync editor: a two-track timeline in match time (the Match track, the Comms clip).
    bool syncWin = false;
    bool syncWinShown = false;              // brought to the front once when it opens
    double view0 = 0.0, view1 = 0.0;        // the replay-time span the editor shows
    bool viewSet = false;
    int edDrag = 0;                         // 0 none, 1 the clip, 2 a scrub, 3 a pan
    float panX0 = 0.f;
    double panView0 = 0.0, panView1 = 0.0;
    bool edHot = false;                     // hovered or focused: the arrows nudge the clip
    bool syncFromDrop = false;              // the running sync came with the file, not the button
    bool gameMuted = false;                 // the Match track's mute (the replay's sound, editor open)
    bool gameMuteApplied = false;
    std::vector<float> activity;            // the match's sound activity, kEnvelopeRate per second
    bool activityBuilt = false;
    // Self-test: a clip drag driven by code (the pointer stays where the owner left it).
    bool simDrag = false;
    double simTarget = 0.0;                 // the replay time the clip start is pulled to
    float simMiss = 0.f;                    // ...missing it by this many pixels (inside the magnet)
    std::string snapName;                   // what the clip snapped to on this frame's drag
    bool phSticky = false;                  // the scrubbed playhead holds a snapped time...
    double phSnapT = 0.0;                   // ...this one (replay time)
    bool mixerForced = false;               // self-test: the play bar's mixer held open for its picture
    std::string hoverSim;                   // self-test: the play bar button hovered by code
    bool menuForced = false;                // self-test: the strip's menu held open for its picture
    int hoverIdx = 0;
    bool simScrub = false;                  // self-test: a scrub driven by code, to simScrubT + simMiss px
    double simScrubT = 0.0;

    // Self-test
    int stStage = 0;
    double stAt = 0.0;
    std::shared_ptr<SynthJob> synth;
    std::thread synthThread;
    double stExpected = 0.0;
    double stSavedOffset = 0.0;
    bool stParty1 = true, stParty2 = true, stCombatLog = false;
    bool stDone = false;
    bool shotPending = false;
    std::filesystem::path shotPath;

    void SetStatus(std::string s, bool error, double seconds)
    {
        status = std::move(s);
        statusError = error;
        statusSetAt = NowSeconds();
        statusUntil = seconds > 0.0 ? statusSetAt + seconds : 0.0;
    }
    void CancelJobs()
    {
        if (load) load->cancel = true;
        if (sync) sync->cancel = true;
        if (loadThread.joinable()) loadThread.join();
        if (syncThread.joinable()) syncThread.join();
        load.reset();
        sync.reset();
    }
    ~LaneState()
    {
        CancelJobs();
        if (synthThread.joinable()) synthThread.join();
        player.Shutdown();
    }
};
} // namespace Comms

using Comms::LaneState;
using Phase = Comms::LaneState::Phase;

namespace
{
constexpr float kCommsLaneH = 32.f;
}

// Always shown in a replay, empty or not: with nothing attached it invites a drop.
bool ReplayWindow::CommsLaneVisible() const
{
    return !m_liveMode;
}

bool ReplayWindow::CommsAttached() const
{
    return !m_liveMode && m_comms && m_comms->phase != Phase::None;
}

float ReplayWindow::PlayBarHeight() const
{
    return 76.f + (CommsLaneVisible() ? kCommsLaneH : 0.f);
}

bool ReplayWindow::CommsSelfTestDone() const
{
    const auto& cfg = Comms::SelfTestCfg();
    if (!cfg.on) return true;
    if (!cfg.folders.empty() && cfg.folders.front() != "1" &&
        std::find(cfg.folders.begin(), cfg.folders.end(), m_matchMeta.folder_name) == cfg.folders.end())
        return true;
    return m_comms && m_comms->stDone;
}

// ---------------------------------------------------------------------------
// Attaching
// ---------------------------------------------------------------------------

void ReplayWindow::OnCommsDrop(void* hdrop)
{
    HDROP drop = static_cast<HDROP>(hdrop);
    std::wstring path;
    if (DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) > 0)
    {
        const UINT n = DragQueryFileW(drop, 0, nullptr, 0);
        path.resize(n + 1);
        DragQueryFileW(drop, 0, path.data(), n + 1);
        path.resize(n);
    }
    DragFinish(drop);
    if (path.empty() || m_liveMode) return;
    if (!m_comms) m_comms = std::make_shared<LaneState>();
    if (m_loadingPhase != LoadingPhase::Ready)
    {
        m_comms->pendingDrop = path;
        return;
    }
    CommsAttachFile(path, false, 0.0);
}

void ReplayWindow::CommsAttachFile(const std::wstring& path, bool restored, double offset)
{
    if (!m_comms) m_comms = std::make_shared<LaneState>();
    auto& c = *m_comms;
    c.CancelJobs();
    c.player.Stop();
    c.path = path;
    c.name = Comms::ToUtf8(std::filesystem::path(path).filename().wstring());
    c.track.reset();
    c.phase = Phase::Loading;
    c.restored = restored;
    c.offset = restored ? offset : 0.0;
    c.hasSync = false;
    c.syncPending = false;
    c.syncWin = false;
    c.reseeks = 0;
    c.size = 0;
    c.writeTime = 0;
    Comms::FileStamp(path, c.size, c.writeTime);
    c.SetStatus("", false, 0.0);

    c.load = std::make_shared<Comms::LoadJob>();
    c.load->path = path;
    c.loadThread = std::thread([job = c.load] { Comms::RunLoad(*job); });
    RunLog::Line("comms: loading '%s' (%.1f MB)%s", Comms::ToUtf8(path).c_str(), c.size / 1048576.0,
                 restored ? " - remembered for this match" : "");
}

void ReplayWindow::SaveCommsEntry()
{
    if (!m_comms || m_comms->phase != Phase::Ready) return;
    auto& c = *m_comms;
    Comms::Saved e;
    e.path = c.path;
    e.size = c.size;
    e.writeTime = c.writeTime;
    e.offset = c.offset;
    e.volume = c.volume;
    e.muted = c.muted;
    Comms::SaveEntry(Comms::StorePath(), m_matchMeta.folder_name, e);
    c.dirty = false;
}

// ---------------------------------------------------------------------------
// The match's sounds, for the sync
// ---------------------------------------------------------------------------

void ReplayWindow::BuildCommsGameModel(Comms::GameModel& out) const
{
    const auto t0 = std::chrono::steady_clock::now();
    out = {};
    out.duration = m_replayCtx.maxReplayTime;
    const auto& sd = m_replayCtx.stocData;
    const auto& is = m_replayCtx.interpSettings;

    auto posOf = [&](int id, float t, float& x, float& y) {
        const auto it = m_replayCtx.agents.find(id);
        if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) return false;
        float z = 0.f;
        InterpolateAgentPosition(it->second, t, is, x, y, z);
        return true;
    };
    auto add = [&](float t, float w, int emitter, int a, int b) {
        Comms::GameEvent e;
        e.time = t;
        e.weight = w;
        e.a = a;
        e.b = b;
        e.hasPos = emitter > 0 && posOf(emitter, t, e.x, e.y);
        out.events.push_back(e);
    };

    // Rough loudness per kind: a cast's start and its release, an attack's swing and its hit, the
    // interrupt cue, a knock-down, a death, the end of the match.
    for (const auto& e : sd.skill)
    {
        float w = 0.f;
        if (e.type == "SKILL_ACTIVATED" || e.type == "INSTANT_SKILL_USED") w = 1.f;
        else if (e.type == "SKILL_FINISHED") w = 0.7f;
        else if (e.type == "SKILL_STOPPED") w = 0.3f;
        if (w > 0.f) add(e.time, w, e.caster_id, e.caster_id, e.target_id);
    }
    for (const auto& e : sd.attackSkill)
        if (e.type == "ATTACK_SKILL_ACTIVATED") add(e.time, 0.8f, e.caster_id, e.caster_id, e.target_id);
    for (const auto& e : sd.basicAttack)
    {
        if (e.type == "ATTACK_STARTED") add(e.time, 0.35f, e.caster_id, e.caster_id, e.target_id);
        else if (e.type == "ATTACK_FINISHED") add(e.time, 0.3f, e.caster_id, e.caster_id, e.target_id);
    }
    for (const auto& e : sd.combat)
    {
        if (e.type == "DAMAGE") add(e.time + 0.067f, 0.6f, e.target_id, e.caster_id, e.target_id);
        else if (e.type == "INTERRUPTED") add(e.time, 0.8f, e.target_id, e.caster_id, e.target_id);
        else if (e.type == "KNOCKED_DOWN") add(e.time, 0.6f, e.target_id, e.caster_id, e.target_id);
    }
    // What the recording client itself heard (effects only; positions as recorded).
    for (const auto& e : sd.soundEvents)
    {
        if (e.sound_type != 1 || !e.positional) continue;
        Comms::GameEvent g;
        g.time = e.time;
        g.weight = 0.5f;
        g.x = e.x;
        g.y = e.y;
        g.hasPos = true;
        g.a = e.cause_agent_id;
        out.events.push_back(g);
    }
    for (const auto& e : m_timeline.events)
    {
        if (e.type == TimelineEventType::Death) add(e.time, 1.5f, e.agentId, e.agentId, 0);
        else if (e.type == TimelineEventType::Victory) add(e.time, 2.f, 0, -1, 0);
    }

    for (int id : m_playerIds)
    {
        const auto it = m_replayCtx.agents.find(id);
        if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) continue;
        Comms::Listener L;
        L.agentId = id;
        const size_t n = static_cast<size_t>(out.duration * L.rate) + 1;
        L.x.resize(n);
        L.y.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            float z = 0.f;
            InterpolateAgentPosition(it->second, static_cast<float>(i) / L.rate, is, L.x[i], L.y[i], z);
        }
        out.listeners.push_back(std::move(L));
    }
    RunLog::Line("comms: the match's sound model - %zu events, %zu listeners, %.1f min, built in %.0f ms",
                 out.events.size(), out.listeners.size(), out.duration / 60.0,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void ReplayWindow::StartCommsSync()
{
    if (!m_comms || !m_comms->track) return;
    auto& c = *m_comms;
    if (c.sync)
    {
        c.sync->cancel = true;
        if (c.syncThread.joinable()) c.syncThread.join();
        c.sync.reset();
    }
    auto job = std::make_shared<Comms::SyncJob>();
    BuildCommsGameModel(job->model);
    job->track = c.track;
    c.sync = job;
    c.syncThread = std::thread([job] { Comms::RunSync(*job); });
    c.syncFromDrop = c.syncPending;
    c.syncPending = false;
    c.SetStatus("Syncing the voice comms with the match...", false, 0.0);
}

// ---------------------------------------------------------------------------
// Every frame
// ---------------------------------------------------------------------------

void ReplayWindow::UpdateComms()
{
    if (m_liveMode || m_loadingPhase != LoadingPhase::Ready) return;
    if (!m_comms) m_comms = std::make_shared<LaneState>();
    auto& c = *m_comms;
    const double now = Comms::NowSeconds();

    // The match remembers a file: attach it again, or say it is gone.
    if (!c.storeChecked && !m_matchMeta.folder_name.empty())
    {
        c.storeChecked = true;
        Comms::Saved saved;
        if (c.phase == Phase::None && c.pendingDrop.empty() &&
            Comms::LoadSaved(Comms::StorePath(), m_matchMeta.folder_name, saved))
        {
            uint64_t size = 0;
            int64_t wt = 0;
            const bool exists = Comms::FileStamp(saved.path, size, wt);
            if (exists && (saved.size == 0 || size == saved.size))
            {
                CommsAttachFile(saved.path, true, saved.offset);
                c.volume = saved.volume;
                c.muted = saved.muted;
            }
            else
            {
                c.phase = Phase::Missing;
                c.path = saved.path;
                c.name = Comms::ToUtf8(std::filesystem::path(saved.path).filename().wstring());
                c.offset = saved.offset;
                c.volume = saved.volume;
                c.muted = saved.muted;
                RunLog::Line("comms: file not found '%s'%s", Comms::ToUtf8(saved.path).c_str(),
                             exists ? " (its size changed)" : "");
            }
        }
    }
    if (!c.pendingDrop.empty())
    {
        const std::wstring p = std::move(c.pendingDrop);
        c.pendingDrop.clear();
        CommsAttachFile(p, false, 0.0);
    }

    // The Locate / Replace dialog.
    if (c.dialog && c.dialog->done.load(std::memory_order_acquire))
    {
        const auto d = c.dialog;
        c.dialog.reset();
        if (d->ok) CommsAttachFile(d->path, c.dialogKeepsOffset, c.offset);
    }

    // The load.
    if (c.load && c.load->done.load(std::memory_order_acquire))
    {
        if (c.loadThread.joinable()) c.loadThread.join();
        const auto job = c.load;
        c.load.reset();
        if (job->ok)
        {
            c.track = job->track;
            c.phase = Phase::Ready;
            RunLog::Line("comms: loaded '%s' - %.1f min at %u Hz, decoded and analysed in %.2f s",
                         c.name.c_str(), c.track->Duration() / 60.0, c.track->sampleRate, job->seconds);
            if (c.restored)
            {
                RunLog::Line("comms: reattached '%s' at offset %+.3f s", c.name.c_str(), c.offset);
                c.SetStatus("Voice comms attached (" + Comms::FormatOffset(c.offset) + ")", false, 5.0);
            }
            else
            {
                c.syncPending = true;
                c.syncWantedAt = now;
                c.SetStatus("Syncing the voice comms with the match...", false, 0.0);
            }
            SaveCommsEntry();
        }
        else
        {
            c.phase = Phase::Failed;
            c.SetStatus(job->error.empty() ? "This file can't be read." : job->error, true, 0.0);
            RunLog::Line("comms: FAILED '%s' - %s (%s)", c.name.c_str(), job->error.c_str(), job->detail.c_str());
        }
    }

    // The sync, once the match's events are in (the event timeline gives the deaths).
    if (c.syncPending && c.track && !c.sync && m_agentsClassified && m_replayCtx.stocLoaded &&
        (m_timeline.computed || now - c.syncWantedAt > 20.0))
        StartCommsSync();
    if (c.sync && c.sync->done.load(std::memory_order_acquire))
    {
        if (c.syncThread.joinable()) c.syncThread.join();
        const Comms::SyncResult r = c.sync->result;
        c.sync.reset();
        c.hasSync = true;
        c.lastSync = r;
        if (r.ok && r.confident)
        {
            c.offset = r.offset;
            c.seekNow = true;
            c.SetStatus("Voice comms synced automatically (" + Comms::FormatOffset(c.offset) + ")", false, 8.0);
        }
        else
        {
            // Not applied, and not offered: a guess this unsure is more often wrong than right, and
            // people line the comms up by ear anyway. A file just dropped starts at MATCH 0:00 and
            // the editor opens for the player to line it up.
            if (c.syncFromDrop)
            {
                c.offset = -static_cast<double>(m_displayTimeOffset);
                c.syncWin = true;
                c.viewSet = false;
            }
            c.SetStatus(c.track && c.track->Duration() < 30.0
                            ? "This recording is too short to sync automatically - line it up in the Sync editor"
                            : "Couldn't sync the voice comms automatically - line them up in the Sync editor",
                        true, 0.0);
        }
        c.syncFromDrop = false;
        RunLog::Line("comms: synced '%s' - offset %+.3f s, z %.1f, ratio %.2f, confidence %.2f (%s), listener %d, %.2f s%s%s",
                     c.name.c_str(), r.offset, r.z, r.ratio, r.confidence, r.confident ? "applied" : "LOW",
                     r.listener, r.seconds, r.ok ? "" : " - ", r.ok ? "" : r.why.c_str());
        SaveCommsEntry();
    }
    // Over the waveform every message is a toast; a failed load keeps its message (its own phase,
    // with Choose another / Close).
    if ((c.statusUntil > 0.0 && now > c.statusUntil) ||
        (c.phase == Phase::Ready && !c.status.empty() && now - c.statusSetAt > Comms::kStatusShow + Comms::kStatusFade))
        c.SetStatus("", false, 0.0);

    // The Sync editor's Match track: the match's sound activity, built once (a few ms).
    if (c.syncWin && !c.activityBuilt && m_agentsClassified && m_replayCtx.stocLoaded)
    {
        c.activityBuilt = true;
        Comms::GameModel model;
        BuildCommsGameModel(model);
        model.listeners.clear();
        c.activity = Comms::BuildGameActivity(model);
    }
    // The Match track's mute: the game's sound off while the editor is open (an engine already
    // muted by someone else is left alone, and given back as it was).
    if (m_audioEngine)
    {
        const bool want = c.gameMuted && c.syncWin && c.phase == Phase::Ready;
        if (want && !c.gameMuteApplied && !m_audioEngine->IsOutputMuted())
        {
            m_audioEngine->SetOutputMuted(true);
            c.gameMuteApplied = true;
        }
        else if (!want && c.gameMuteApplied)
        {
            m_audioEngine->SetOutputMuted(false);
            c.gameMuteApplied = false;
        }
    }

    // Playback: comms position = match time + offset.
    if (c.phase == Phase::Ready && c.track)
    {
        const float speed = m_replayCtx.playbackSpeed;
        const bool want = m_replayCtx.isPlaying && !IsClipRendering() && speed > 0.f && speed <= 2.001f && !c.noDevice;
        const double filePos = static_cast<double>(m_debugTimeline) + c.offset;
        const bool inRange = filePos >= 0.0 && filePos < c.track->Duration() - 0.05;
        if (!want || !inRange)
        {
            if (c.player.Active()) c.player.Stop();
            c.driftFrames = 0;
            c.seekNow = false;
        }
        else
        {
            float vol = 0.f;
            // The editor's Match mute silences the game's engine, not the comms.
            const bool engineMuted = m_audioEngine && m_audioEngine->IsOutputMuted() && !c.gameMuteApplied;
            if (!c.muted && m_audioEnabled && !engineMuted)
                vol = c.volume * Comms::MasterGainOf(m_audioEngine ? m_audioEngine->GetConfig().master_volume : 0.7f);
            const size_t startSample = static_cast<size_t>(filePos * c.track->sampleRate);
            if (!c.player.Active() || c.player.Finished())
            {
                if (!c.player.Start(c.track, startSample, speed, vol))
                {
                    c.noDevice = true;
                    RunLog::Line("comms: no audio output - the comms stay silent");
                }
                c.driftFrames = 0;
                c.seekNow = false;
            }
            else
            {
                const double drift = c.player.Position() - filePos;
                c.driftSum += std::fabs(drift);
                c.driftMax = std::max(c.driftMax, std::fabs(drift));
                ++c.driftCount;
                // A jump (a scrub, a step) restarts at once; a small slip only once it has held
                // for a few frames, so one late frame does not cut the voice.
                c.driftFrames = std::fabs(drift) > 0.08 ? c.driftFrames + 1 : 0;
                // The clip was moved (dragged, nudged): heard at once, but at most ten times a
                // second while a drag keeps moving it.
                const bool moved = c.seekNow && std::fabs(drift) > 0.01 && now - c.lastSeekAt > 0.1;
                if (std::fabs(drift) > 0.25 || c.driftFrames >= 4 || moved)
                {
                    c.player.Start(c.track, startSample, speed, vol);
                    ++c.reseeks;
                    c.driftFrames = 0;
                    c.lastSeekAt = now;
                    c.seekNow = false;
                }
            }
            c.player.SetRatio(speed);
            c.player.SetVolume(vol);
        }
    }
    else if (c.player.Active())
        c.player.Stop();

    ProcessCommsSelfTest();
}

// ---------------------------------------------------------------------------
// Drawing: one waveform renderer for the lane and the Sync editor
// ---------------------------------------------------------------------------

namespace
{
// A neutral editor palette (Premiere / Resolve): grey panels, a green audio clip, a slate match
// clip, a coral playhead. Gold only for the editor's title.
const ImU32 kClipFill    = IM_COL32( 47, 107,  79, 245);   // #2f6b4f
const ImU32 kClipBand    = IM_COL32( 64, 138, 103, 250);
const ImU32 kClipBorder  = IM_COL32(110, 190, 150, 200);
const ImU32 kClipHot     = IM_COL32(200, 250, 222, 255);
const ImU32 kWavePeak    = IM_COL32(127, 214, 166, 230);   // #7fd6a6
const ImU32 kWaveRms     = IM_COL32(196, 244, 218, 245);
const ImU32 kMatchFill   = IM_COL32( 40,  62,  92, 245);
const ImU32 kMatchBand   = IM_COL32( 58,  88, 126, 250);
const ImU32 kMatchBorder = IM_COL32(110, 150, 200, 200);
const ImU32 kMatchWave   = IM_COL32(150, 194, 240, 220);
const ImU32 kPlayhead    = IM_COL32(255,  92,  80, 255);   // coral
const ImU32 kSnapLine    = IM_COL32(120, 205, 255, 235);
const ImU32 kEdPanel     = IM_COL32( 38,  38,  41, 250);
const ImU32 kEdLane      = IM_COL32( 52,  52,  56, 255);
const ImU32 kEdRuler     = IM_COL32( 78,  78,  84, 255);
const ImU32 kEdHeader    = IM_COL32( 58,  58,  63, 255);
const ImU32 kEdEdge      = IM_COL32( 24,  24,  26, 255);
const ImU32 kEdText      = IM_COL32(242, 242, 242, 255);
const ImU32 kEdText2     = IM_COL32(178, 178, 184, 255);
const ImU32 kEdText3     = IM_COL32(140, 140, 146, 255);

// The recording's waveform over the columns [x0, x1): file time `fileAtX0` at x0, `secPerPx` a
// column. Zoomed in, the true waveform as an audio editor draws it: linear min/max (the outline)
// and RMS (the bright core), from the 10 ms peak table or from the samples once zoomed past it.
// Zoomed out, every column holds seconds of sound and its loudest peak always reaches the top, so a
// long or loud recording became a slab: there each column shows its average peak instead, spread
// between the quietest and loudest columns in view (2nd to 98th percentile), and the core keeps its
// RMS share. The two are blended over the zoom, so it never jumps.
void DrawWave(ImDrawList* dl, const Comms::Track& tr, float x0, float x1, double fileAtX0, double secPerPx,
              float midY, float half, ImU32 peakCol, ImU32 rmsCol)
{
    const double dur = tr.Duration();
    const size_t nb = tr.peakMax.size();
    if (nb == 0 || secPerPx <= 0.0 || half < 1.f) return;
    const double binsPerPx = secPerPx * Comms::kEnvelopeRate;
    const double rate = static_cast<double>(tr.sampleRate);
    const float ref = tr.displayRef;
    const float wOut = static_cast<float>(std::clamp((binsPerPx - 2.0) / 40.0, 0.0, 1.0));

    struct Col { float x, up, dn, r, avgPeak, rms; };
    static thread_local std::vector<Col> cols;
    cols.clear();
    for (float x = std::floor(x0); x < x1; x += 1.f)
    {
        double t0 = fileAtX0 + (x - x0) * secPerPx, t1 = t0 + secPerPx;
        if (t1 <= 0.0 || t0 >= dur) continue;
        t0 = std::max(0.0, t0);
        t1 = std::min(dur, t1);
        int lo = 0, hi = 0;
        double sq = 0.0, peakSum = 0.0;
        size_t n = 0;
        if (binsPerPx < 1.0)
        {
            const size_t s0 = static_cast<size_t>(t0 * rate);
            const size_t s1 = std::min(tr.pcm.size(), std::max(s0 + 1, static_cast<size_t>(t1 * rate)));
            for (size_t s = s0; s < s1; ++s)
            {
                const int v = tr.pcm[s];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                sq += static_cast<double>(v) * v;
                ++n;
            }
            peakSum = std::max(hi, -lo) * static_cast<double>(n);
        }
        else
        {
            const size_t b0 = static_cast<size_t>(t0 * Comms::kEnvelopeRate);
            const size_t b1 = std::min(nb, std::max(b0 + 1, static_cast<size_t>(t1 * Comms::kEnvelopeRate)));
            for (size_t b = b0; b < b1; ++b)
            {
                lo = std::min<int>(lo, tr.peakMin[b]);
                hi = std::max<int>(hi, tr.peakMax[b]);
                peakSum += std::max<int>(tr.peakMax[b], -static_cast<int>(tr.peakMin[b]));
                const double r = b < tr.rms.size() ? tr.rms[b] : 0.0;
                sq += r * r;
                ++n;
            }
        }
        if (n == 0) continue;
        Col c;
        c.x = x;
        c.rms = static_cast<float>(std::sqrt(sq / static_cast<double>(n)));
        c.avgPeak = static_cast<float>(peakSum / static_cast<double>(n));
        c.up = Comms::WaveHeight(static_cast<float>(hi), ref);
        c.dn = Comms::WaveHeight(static_cast<float>(-lo), ref);
        c.r = Comms::WaveHeight(c.rms, ref);
        cols.push_back(c);
    }
    if (cols.empty()) return;

    if (wOut > 0.f)
    {
        static thread_local std::vector<float> vals, ratios;
        vals.clear();
        ratios.clear();
        for (const Col& c : cols)
        {
            vals.push_back(c.avgPeak);
            ratios.push_back(c.avgPeak > 1.f ? c.rms / c.avgPeak : 0.f);
        }
        auto pct = [](std::vector<float>& v, double p) {
            const size_t k = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return v[k];
        };
        const float lo = pct(vals, 0.02), hi = std::max(pct(vals, 0.98), lo * 1.05f + 64.f);
        const float medRatio = std::max(0.05f, pct(ratios, 0.5));
        for (Col& c : cols)
        {
            const float u = std::clamp((c.avgPeak - lo) / (hi - lo), 0.f, 1.f);
            const float e = 0.12f + 0.73f * std::pow(u, 0.8f);
            const float share = c.avgPeak > 1.f ? c.rms / c.avgPeak : 0.f;
            const float rx = std::min(e, e * 0.55f * share / medRatio);
            c.up += (e - c.up) * wOut;
            c.dn += (e - c.dn) * wOut;
            c.r += (rx - c.r) * wOut;
        }
    }
    for (const Col& c : cols)
    {
        const float up = std::max(0.5f, c.up * half), dn = std::max(0.5f, c.dn * half);
        dl->AddRectFilled(ImVec2(c.x, midY - up), ImVec2(c.x + 1.f, midY + dn), peakCol);
        const float r = c.r * half;
        if (r >= 0.5f) dl->AddRectFilled(ImVec2(c.x, midY - r), ImVec2(c.x + 1.f, midY + r), rmsCol);
    }
}

// The match's sound activity (Comms::BuildGameActivity) drawn the same way, in replay time.
void DrawActivity(ImDrawList* dl, const std::vector<float>& act, float x0, float x1, double tAtX0, double secPerPx,
                  float midY, float half, ImU32 col)
{
    const size_t n = act.size();
    if (n == 0 || secPerPx <= 0.0 || half < 1.f) return;
    const double len = static_cast<double>(n) / Comms::kEnvelopeRate;
    for (float x = std::floor(x0); x < x1; x += 1.f)
    {
        double t0 = tAtX0 + (x - x0) * secPerPx, t1 = t0 + secPerPx;
        if (t1 <= 0.0 || t0 >= len) continue;
        t0 = std::max(0.0, t0);
        const size_t b0 = static_cast<size_t>(t0 * Comms::kEnvelopeRate);
        const size_t b1 = std::min(n, std::max(b0 + 1, static_cast<size_t>(t1 * Comms::kEnvelopeRate)));
        float mx = 0.f;
        double sum = 0.0;
        for (size_t b = b0; b < b1; ++b) { mx = std::max(mx, act[b]); sum += act[b]; }
        // Zoomed out, a column holds many sounds: their density reads better than the loudest.
        const float mean = static_cast<float>(sum / static_cast<double>(b1 - b0));
        const float v = (b1 - b0) <= 4 ? mx : 0.3f * mx + 1.2f * mean;
        const float h = std::max(0.5f, std::min(1.f, 0.85f * std::max(0.f, v)) * half);
        dl->AddRectFilled(ImVec2(x, midY - h), ImVec2(x + 1.f, midY + h), col);
    }
}

void DrawSpeaker(ImDrawList* dl, ImVec2 c, float s, bool on, ImU32 col)
{
    const float ox = c.x - 5.5f * s, oy = c.y, th = 1.3f * s;
    dl->AddRectFilled(ImVec2(ox, oy - 2.f * s), ImVec2(ox + 3.2f * s, oy + 2.f * s), col);
    dl->AddTriangleFilled(ImVec2(ox + 7.2f * s, oy - 4.6f * s), ImVec2(ox + 7.2f * s, oy + 4.6f * s), ImVec2(ox + 1.6f * s, oy), col);
    if (on)
    {
        dl->PathArcTo(ImVec2(ox + 7.2f * s, oy), 3.4f * s, -58.f * IM_PI / 180.f, 58.f * IM_PI / 180.f, 12);
        dl->PathStroke(col, false, th);
    }
    else
        dl->AddLine(ImVec2(ox, oy - 5.5f * s), ImVec2(ox + 11.f * s, oy + 5.5f * s), col, th);
}

// "1:47.3", "-0:05.0"
std::string ClockTenths(double s)
{
    const bool neg = s < -0.05;
    const long long t10 = static_cast<long long>(std::llround(std::fabs(s) * 10.0));
    return std::format("{}{}:{:02}.{}", neg ? "-" : "", t10 / 600, (t10 / 10) % 60, t10 % 10);
}

// Where the recording starts against MATCH 0:00 (`startDisplay`: its start on the match's clock).
std::string StartWords(double startDisplay)
{
    if (std::fabs(startDisplay) < 0.05) return "Recording starts at the match start";
    const std::string t = ClockTenths(std::fabs(startDisplay));
    return startDisplay < 0.0 ? "Recording starts " + t + " before the match"
                              : "Recording starts " + t + " after the match start";
}

std::string Ellipsize(ImFont* font, float size, const std::string& s, float maxW)
{
    if (maxW <= 0.f) return {};
    if (font->CalcTextSizeA(size, FLT_MAX, 0.f, s.c_str()).x <= maxW) return s;
    std::string t = s;
    while (!t.empty())
    {
        t.pop_back();
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();   // a whole UTF-8 character
        std::string base = t;   // no "...." or "x ...": the cut never ends on a dot or a space
        while (!base.empty() && (base.back() == '.' || base.back() == ' ')) base.pop_back();
        const std::string e = base + "...";
        if (font->CalcTextSizeA(size, FLT_MAX, 0.f, e.c_str()).x <= maxW) return e;
    }
    return {};
}

// The editor's magnet: what the clip's start snaps to (replay time).
struct SnapTarget { double t; const char* name; };
std::vector<SnapTarget> SnapTargets(double dto, double playhead)
{
    std::vector<SnapTarget> v;
    v.push_back({dto, "the match start"});
    v.push_back({playhead, "the playhead"});
    if (dto > 0.5) v.push_back({0.0, "the replay start"});
    return v;
}
double SnapStart(double start, double pxPerSec, const std::vector<SnapTarget>& targets, float magnetPx, std::string& name)
{
    double best = start, bestPx = magnetPx;
    for (const SnapTarget& s : targets)
    {
        const double px = std::fabs(s.t - start) * pxPerSec;
        if (px <= bestPx) { bestPx = px; best = s.t; name = s.name; }
    }
    return best;
}

// A small tooltip box drawn by hand (the pointer may be anywhere: the self-test drives the drag).
void TipBox(ImDrawList* dl, ImFont* font, float fs, float x, float yBottom, float minX, float maxX, const std::string& text)
{
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, text.c_str());
    const float w = ts.x + 12.f, h = ts.y + 8.f;
    const float bx = std::clamp(x - w * 0.5f, minX, std::max(minX, maxX - w));
    const ImVec2 a(bx, yBottom - h), b(bx + w, yBottom);
    dl->AddRectFilled(a, b, IM_COL32(12, 11, 8, 245), 4.f);
    dl->AddRect(a, b, IM_COL32(226, 194, 106, 200), 4.f);
    dl->AddText(font, fs, ImVec2(a.x + 6.f, a.y + 4.f), IM_COL32(240, 228, 196, 255), text.c_str());
}
} // namespace

// ---------------------------------------------------------------------------
// The lane under the play bar
// ---------------------------------------------------------------------------

void ReplayWindow::ForgetComms(const char* why)
{
    if (!m_comms) return;
    auto& c = *m_comms;
    c.CancelJobs();
    c.player.Stop();
    Comms::ForgetEntry(Comms::StorePath(), m_matchMeta.folder_name);
    RunLog::Line("comms: %s '%s'", why, c.name.c_str());
    c.phase = Phase::None;
    c.track.reset();
    c.path.clear();
    c.name.clear();
    c.offset = 0.0;
    c.hasSync = false;
    c.syncWin = false;
    c.edDrag = 0;
    c.laneDrag = 0;
    c.SetStatus("", false, 0.0);
}

bool ReplayWindow::CommsEditorOwnsKeys() const
{
    return m_comms && m_comms->syncWin && m_comms->edHot;
}

// The play bar's mixer, opened from its speaker while voice comms are attached: the game's sound
// and the comms, each with its mute and its volume. The same state the play bar's slider, the
// Comms strip, the Sync editor and comms_audio.json use. Its bottom-left corner at the anchor.
void ReplayWindow::DrawVolumeMixer(const char* popupId, float anchorX, float anchorY)
{
    if (!m_comms) return;
    auto& c = *m_comms;
    if (c.mixerForced && !ImGui::IsPopupOpen(popupId)) ImGui::OpenPopup(popupId);
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    // Over the speaker, kept inside the window (the speaker sits near its right edge).
    constexpr float kSliderW = 170.f, kPad = 12.f;
    const float estW = kPad * 2.f + font->CalcTextSizeA(fs, FLT_MAX, 0.f, "Comms").x + 14.f + ImGui::GetFrameHeight() + 8.f + kSliderW + 8.f + font->CalcTextSizeA(fs, FLT_MAX, 0.f, "100%").x;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float px = std::min(anchorX - 10.f, vp->Pos.x + vp->Size.x - 8.f - estW);
    ImGui::SetNextWindowPos(ImVec2(px, anchorY), ImGuiCond_Always, ImVec2(0.f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPad, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 8.f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.06f, 0.04f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.78f, 0.66f, 0.29f, 0.40f));
    const bool open = ImGui::BeginPopup(popupId);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
    if (!open) return;
    ImGui::PushStyleColor(ImGuiCol_FrameBg,          ImVec4(0.f, 0.f, 0.f, 0.45f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   ImVec4(0.78f, 0.66f, 0.29f, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    ImVec4(0.78f, 0.66f, 0.29f, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ImVec4(0.78f, 0.66f, 0.29f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.89f, 0.76f, 0.42f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text,             ImVec4(0.91f, 0.87f, 0.78f, 1.f));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float labelW = font->CalcTextSizeA(fs, FLT_MAX, 0.f, "Comms").x + 14.f;
    AudioConfig* acfg = m_audioEngine ? &m_audioEngine->GetConfig() : nullptr;

    auto Row = [&](const char* label, const char* id, bool& muted, float& level, const char* tipOn, const char* tipOff) {
        bool changed = false;
        const float rowH = ImGui::GetFrameHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(font, fs, ImVec2(p.x, p.y + (rowH - fs) * 0.5f), IM_COL32(232, 223, 200, 255), label);
        ImGui::SetCursorScreenPos(ImVec2(p.x + labelW, p.y));
        ImGui::PushID(id);
        ImGui::InvisibleButton("##mute", ImVec2(rowH, rowH));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) { muted = !muted; changed = true; }
        if (hov || muted)
            dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                              muted ? IM_COL32(192, 80, 74, 60) : IM_COL32(255, 244, 222, 24), 6.f);
        DrawSpeaker(dl, ImVec2(p.x + labelW + rowH * 0.5f, p.y + rowH * 0.5f), 1.f, !muted,
                    muted ? IM_COL32(230, 130, 110, 255) : IM_COL32(226, 194, 106, 255));
        if (hov) ImGui::SetTooltip("%s", muted ? tipOff : tipOn);
        ImGui::SameLine();
        // The play bar's own slider: a thin track and a round knob, not ImGui's frame.
        const ImVec2 t0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##level", ImVec2(kSliderW, rowH));
        const bool hovT = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        const float kr = 6.f, k0 = t0.x + kr, k1 = t0.x + kSliderW - kr, ty = t0.y + rowH * 0.5f;
        if (held)
        {
            const float nv = std::clamp((ImGui::GetIO().MousePos.x - k0) / (k1 - k0), 0.f, 1.f);
            if (nv != level) { level = nv; changed = true; }
        }
        const float frac = muted ? 0.f : level;
        dl->AddLine(ImVec2(k0, ty), ImVec2(k1, ty), IM_COL32(120, 108, 80, 255), 3.f);
        if (frac > 0.f) dl->AddLine(ImVec2(k0, ty), ImVec2(k0 + (k1 - k0) * frac, ty), IM_COL32(226, 194, 106, 255), 3.f);
        dl->AddCircleFilled(ImVec2(k0 + (k1 - k0) * frac, ty), (hovT || held) ? 6.f : 5.f,
                            (hovT || held) ? IM_COL32(248, 242, 230, 255) : IM_COL32(226, 194, 106, 255));
        ImGui::SameLine();
        const std::string pct = std::format("{:.0f}%", level * 100.f);
        const float pw = font->CalcTextSizeA(fs, FLT_MAX, 0.f, "100%").x;
        const ImVec2 tp = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(pw, rowH));
        const float tw = font->CalcTextSizeA(fs, FLT_MAX, 0.f, pct.c_str()).x;
        dl->AddText(font, fs, ImVec2(tp.x + pw - tw, tp.y + (rowH - fs) * 0.5f),
                    muted ? IM_COL32(120, 108, 80, 255) : IM_COL32(200, 190, 165, 255), pct.c_str());
        ImGui::PopID();
        return changed;
    };

    ImGui::TextDisabled("Volume");
    {
        bool muted = !m_audioEnabled;
        float level = acfg ? acfg->master_volume : 0.f;
        const float before = level;
        if (Row("Game", "game", muted, level, "Game sound: mute", "Game sound: unmute"))
        {
            m_audioEnabled = !muted;
            if (acfg)
            {
                acfg->master_volume = level;
                if (level != before) m_audioEnabled = true;   // moving the slider means you want to hear it
            }
        }
    }
    {
        bool muted = c.muted;
        float level = c.volume;
        const float before = level;
        if (Row("Comms", "comms", muted, level, "Voice comms: mute", "Voice comms: unmute"))
        {
            c.muted = muted;
            c.volume = level;
            if (level != before) c.muted = false;
            c.dirty = true;
        }
    }
    ImGui::PopStyleColor(6);
    ImGui::EndPopup();
}

void ReplayWindow::DrawCommsLane(float laneX, float laneW, float trackX, float trackW, float laneY, float laneH, float maxT)
{
    if (!CommsLaneVisible()) return;
    if (!m_comms) m_comms = std::make_shared<LaneState>();
    auto& c = *m_comms;
    const float sf = 1.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = font->FontSize;
    const float fsSm = fs * 0.82f;
    const ImGuiIO& io = ImGui::GetIO();

    const ImU32 cBorder     = IM_COL32(160, 120,  40,  46);
    const ImU32 cBorderHi   = IM_COL32(200, 168,  75,  82);
    const ImU32 cBorderGlow = IM_COL32(200, 168,  75, 140);
    const ImU32 cGoldBright = IM_COL32(226, 194, 106, 255);
    const ImU32 cText       = IM_COL32(232, 223, 200, 255);
    const ImU32 cTextMid    = IM_COL32(154, 138, 104, 255);
    const ImU32 cTextDim    = IM_COL32(120, 108,  80, 255);
    const ImU32 cDanger     = IM_COL32(192,  80,  74, 255);

    const float y0 = laneY, y1 = laneY + laneH, midY = (y0 + y1) * 0.5f;

    // A small pill button; true when clicked.
    auto Pill = [&](const char* id, const char* label, float x, float w) {
        const float h = laneH - 8.f * sf, py = y0 + 4.f * sf;
        ImGui::SetCursorScreenPos(ImVec2(x, py));
        ImGui::InvisibleButton(id, ImVec2(w, h));
        const bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(ImVec2(x, py), ImVec2(x + w, py + h), hov ? IM_COL32(200, 168, 75, 34) : IM_COL32(0, 0, 0, 76), h * 0.5f);
        dl->AddRect(ImVec2(x, py), ImVec2(x + w, py + h), hov ? cBorderGlow : cBorderHi, h * 0.5f);
        const ImVec2 ts = font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, label);
        dl->AddText(font, fsSm, ImVec2(x + (w - ts.x) * 0.5f, py + (h - fsSm) * 0.5f), hov ? cGoldBright : cTextMid, label);
        return ImGui::IsItemClicked();
    };
    auto PillW = [&](const char* label) { return font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, label).x + 18.f * sf; };
    auto OpenDialog = [&](bool keepOffset) {
        if (c.dialog) return;
        c.dialog = std::make_shared<Comms::DialogState>();
        c.dialogKeepsOffset = keepOffset;
        m_replayCtx.isPlaying = false;
        std::thread(Comms::RunOpenDialog, m_hwnd, c.dialog).detach();
    };

    // ── Left column: the speaker (a plain mute; the volume is in the play bar's mixer) and the
    // lane's name. Nothing attached: the name alone. ──
    {
        const float iw = 20.f * sf;
        if (c.phase != Phase::None)
        {
            ImGui::SetCursorScreenPos(ImVec2(laneX, y0));
            ImGui::InvisibleButton("##CommsMute", ImVec2(iw, laneH));
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) { c.muted = !c.muted; c.dirty = true; }
            DrawSpeaker(dl, ImVec2(laneX + 9.5f * sf, midY), sf, !c.muted, (hov || !c.muted) ? cGoldBright : cTextMid);
            if (hov) ImGui::SetTooltip(c.muted ? "Voice comms: unmute" : "Voice comms: mute");
        }
        dl->AddText(font, fsSm, ImVec2(laneX + iw + 4.f * sf, midY - fsSm * 0.5f), cTextDim, "Comms");
    }

    // ── The track: the recording as one clip on the match's timeline ─────
    const float tx0 = trackX, tx1 = trackX + trackW;
    dl->AddRectFilled(ImVec2(tx0, y0), ImVec2(tx1, y1), IM_COL32(0, 0, 0, 70), 3.f);
    dl->AddRect(ImVec2(tx0, y0), ImVec2(tx1, y1), cBorder, 3.f);
    const float now = m_debugTimeline;

    if (c.phase == Phase::Ready && c.track && !c.track->peakMax.empty())
    {
        const Comms::Track& tr = *c.track;
        const double dur = tr.Duration();
        const double secPerPx = maxT / std::max(1.f, trackW);
        auto X = [&](double t) { return tx0 + static_cast<float>(t / maxT) * trackW; };
        const float cx0 = X(-c.offset), cx1 = X(dur - c.offset);
        const float nowX = tx0 + trackW * std::clamp(now / maxT, 0.f, 1.f);
        const bool hotClip = c.laneDrag == 1;

        dl->PushClipRect(ImVec2(tx0, y0), ImVec2(tx1, y1), true);
        const ImVec2 ca(std::max(cx0, tx0 - 4.f), y0 + 2.f), cb(std::min(cx1, tx1 + 4.f), y1 - 2.f);
        if (cb.x > ca.x)
        {
            dl->AddRectFilled(ca, cb, kClipFill, 3.f);
            DrawWave(dl, tr, tx0, tx1, c.offset, secPerPx, midY, laneH * 0.5f - 4.f * sf, kWavePeak, kWaveRms);
            dl->AddRect(ca, cb, hotClip ? kClipHot : kClipBorder, 3.f, 0, hotClip ? 1.5f : 1.f);
        }
        // Where the recording starts and ends on the match's clock, when that is inside the match.
        // Outside the clip, in the empty part of the track, when there is room for them.
        auto Tag = [&](float x, const char* text, bool leftOf) {
            const ImVec2 ts = font->CalcTextSizeA(fsSm * 0.9f, FLT_MAX, 0.f, text);
            const float lx = leftOf ? x - ts.x - 6.f : x + 6.f;
            if (lx < tx0 + 4.f || lx + ts.x > tx1 - 4.f) return;
            dl->AddText(font, fsSm * 0.9f, ImVec2(lx, midY - ts.y * 0.5f), cTextMid, text);
        };
        const bool wide = cx1 - cx0 > 40.f;
        if (cx0 > tx0 && cx0 < tx1 && wide) Tag(cx0, "rec start", true);
        if (cx1 > tx0 && cx1 < tx1 && wide) Tag(cx1, "rec end", false);
        // A recording much shorter than the match is a few pixels wide here: name its length.
        if (cx1 - cx0 < 40.f && cx1 > tx0 && cx0 < tx1)
        {
            const std::string len = Comms::ClockText(dur) + " recording";
            dl->AddText(font, fsSm, ImVec2(std::min(cx1 + 6.f, tx1 - 90.f), midY - fsSm * 0.5f), cGoldBright, len.c_str());
        }
        dl->AddLine(ImVec2(nowX, y0 + 1.f), ImVec2(nowX, y1 - 1.f), kPlayhead, 1.f);
        dl->PopClipRect();

        // Drag the clip to slide it (the editor's magnet, Alt: none); click or drag elsewhere to
        // seek; right-click for the menu.
        ImGui::SetCursorScreenPos(ImVec2(tx0, y0));
        ImGui::InvisibleButton("##CommsWave", ImVec2(trackW, laneH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const float mx = io.MousePos.x;
        const bool overClip = mx >= cx0 - 3.f && mx <= cx1 + 3.f;
        if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            c.laneDrag = overClip ? 1 : 2;
            c.dragMoved = false;
            c.dragX0 = mx;
            c.dragOffset0 = c.offset;
        }
        const bool held = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (c.laneDrag == 1 && held)
        {
            if (std::fabs(mx - c.dragX0) > 3.f && !c.dragMoved) { c.dragMoved = true; c.SetStatus("", false, 0.0); }
            if (c.dragMoved)
            {
                double start = -c.dragOffset0 + (mx - c.dragX0) * secPerPx;
                std::string snap;
                if (!io.KeyAlt)
                    start = SnapStart(start, 1.0 / secPerPx, SnapTargets(m_displayTimeOffset, m_debugTimeline), 8.f, snap);
                if (start != -c.offset) { c.offset = -start; c.seekNow = true; }
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                std::string tip = StartWords(-c.offset - m_displayTimeOffset);
                if (!snap.empty()) tip += "  (snapped to " + snap + ")";
                TipBox(ImGui::GetForegroundDrawList(), font, fsSm, X(-c.offset), y0 - 4.f, laneX, laneX + laneW, tip);
            }
        }
        else if (c.laneDrag == 2 && held)
            m_debugTimeline = std::clamp(static_cast<float>((mx - tx0) / trackW) * maxT, 0.f, maxT);
        if (c.laneDrag != 0 && !held)
        {
            if (c.laneDrag == 1 && c.dragMoved)
            {
                if (c.offset != c.dragOffset0)
                {
                    c.dirty = true;
                    c.SetStatus("", false, 0.0);
                    RunLog::Line("comms: clip dragged on the lane - offset %+.3f s", c.offset);
                }
            }
            else if (c.laneDrag == 1)
                m_debugTimeline = std::clamp(static_cast<float>((c.dragX0 - tx0) / trackW) * maxT, 0.f, maxT);
            c.laneDrag = 0;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##CommsMenu");
        if (ImGui::IsItemHovered() && c.laneDrag == 0)
        {
            if (overClip)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s\nDrag to line it up with the match (Alt: no snap). Click to seek. Right-click: menu.", c.name.c_str());
            }
        }
    }
    else if (c.phase == Phase::None)
    {
        // Nothing attached: a faint invitation to drop a file here (the whole window takes the
        // drop), or to click for the Open dialog.
        ImGui::SetCursorScreenPos(ImVec2(tx0, y0));
        ImGui::InvisibleButton("##CommsEmpty", ImVec2(trackW, laneH));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) OpenDialog(false);
        // Only the formats checked to load (GWO_COMMS_SELFTEST_MODE=probe, 2026-11-18): OGG failed
        // (Media Foundation has no Ogg reader by default); FLAC, WMA and MKV were not tested.
        const char* hint = "Drag and drop a voice file here (MP3, WAV, M4A, MP4, MOV, WEBM)";
        const float hw = font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, hint).x;
        dl->PushClipRect(ImVec2(tx0, y0), ImVec2(tx1, y1), true);
        dl->AddText(font, fsSm, ImVec2(std::max(tx0 + 8.f * sf, (tx0 + tx1 - hw) * 0.5f), midY - fsSm * 0.5f),
                    hov ? cTextMid : IM_COL32(154, 138, 104, 150), hint);
        dl->PopClipRect();
        if (hov)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Drop an audio or video file on the replay, or click to choose one.\n"
                              "It stays on this PC and is lined up with the match automatically.");
        }
    }
    else if (c.phase == Phase::Loading)
    {
        const float p = c.load ? c.load->progress.load() : 0.f;
        dl->AddRectFilled(ImVec2(tx0, y0), ImVec2(tx0 + trackW * p, y1), IM_COL32(200, 168, 75, 30), 3.f);
        const std::string s = std::format("Loading {}... {:.0f}%", c.name, p * 100.f);
        dl->PushClipRect(ImVec2(tx0, y0), ImVec2(tx1, y1), true);
        const float lw = font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, s.c_str()).x;
        dl->AddText(font, fsSm, ImVec2(std::max(tx0 + 8.f * sf, (tx0 + tx1 - lw) * 0.5f), midY - fsSm * 0.5f), cTextMid, s.c_str());
        dl->PopClipRect();
    }
    else if (c.phase == Phase::Missing || c.phase == Phase::Failed)
    {
        const bool missing = c.phase == Phase::Missing;
        const std::string s = missing ? "Voice comms file not found: " + c.name : c.status;
        const char* a = missing ? "Locate..." : "Choose another...";
        const char* b = missing ? "Forget" : "Close";
        const float wb = PillW(b), wa = PillW(a);
        // Centred in the track: the party windows often cover the ends of the bar.
        const float textW = font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, s.c_str()).x;
        const float total = textW + 12.f * sf + wa + 6.f * sf + wb;
        const float sx = std::max(tx0 + 8.f * sf, (tx0 + tx1 - total) * 0.5f);
        const float ax = std::min(sx + textW + 12.f * sf, tx1 - 10.f * sf - wa - wb), bx = ax + wa + 6.f * sf;
        dl->PushClipRect(ImVec2(tx0, y0), ImVec2(ax - 4.f * sf, y1), true);
        dl->AddText(font, fsSm, ImVec2(sx, midY - fsSm * 0.5f), missing ? cTextMid : cDanger, s.c_str());
        dl->PopClipRect();
        if (Pill("##CommsA", a, ax, wa)) OpenDialog(missing);
        if (Pill("##CommsB", b, bx, wb)) ForgetComms(missing ? "forgotten" : "closed");
    }

    // The status line over the waveform (synced, couldn't sync, syncing...): a toast. It shows for
    // Comms::kStatusShow seconds and fades out (UpdateComms clears it); moving or nudging the clip
    // or opening the editor clears it at once. Only the sync's live progress stays while it runs.
    if (c.phase == Phase::Ready)
    {
        std::string s = c.status;
        float alpha = 1.f;
        if (c.sync) s = std::format("Syncing the voice comms with the match... {:.0f}%", c.sync->progress.load() * 100.f);
        else if (!s.empty())
        {
            double end = c.statusSetAt + Comms::kStatusShow + Comms::kStatusFade;
            if (c.statusUntil > 0.0) end = std::min(end, c.statusUntil);
            alpha = static_cast<float>(std::clamp((end - Comms::NowSeconds()) / Comms::kStatusFade, 0.0, 1.0));
        }
        if (!s.empty() && c.laneDrag != 1 && alpha > 0.f)
        {
            auto A = [&](ImU32 col) { return (col & 0x00FFFFFF) | (static_cast<ImU32>(((col >> 24) & 0xFF) * alpha) << 24); };
            const ImVec2 ts = font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, s.c_str());
            const float sx = std::max(tx0 + 6.f * sf, (tx0 + tx1 - ts.x) * 0.5f), sy = midY - fsSm * 0.5f;
            dl->AddRectFilled(ImVec2(sx - 3.f, sy - 1.f), ImVec2(sx + ts.x + 3.f, sy + fsSm + 1.f), A(IM_COL32(8, 9, 12, 200)), 3.f);
            dl->AddText(font, fsSm, ImVec2(sx, sy), A(c.statusError ? IM_COL32(230, 160, 90, 255) : cText), s.c_str());
        }
    }

    // ── Right column: the offset badge, which opens the Sync editor ──────
    if (c.phase != Phase::None)
    {
        const float bx = tx1 + 6.f * sf, bw = laneX + laneW - bx;
        const float bh = laneH - 8.f * sf, by = y0 + 4.f * sf;
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        ImGui::InvisibleButton("##CommsOffset", ImVec2(std::max(10.f, bw), bh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked() && c.phase == Phase::Ready) { c.syncWin = !c.syncWin; c.viewSet = false; c.SetStatus("", false, 0.0); }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##CommsMenu");
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), hov || c.syncWin ? IM_COL32(200, 168, 75, 34) : IM_COL32(0, 0, 0, 100), 3.f);
        dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), hov || c.syncWin ? cBorderGlow : cBorder, 3.f);
        const std::string off = Comms::FormatOffset(c.offset);
        const ImVec2 ts = font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, off.c_str());
        dl->AddText(font, fsSm, ImVec2(bx + (bw - ts.x) * 0.5f, by + (bh - fsSm) * 0.5f), hov ? cGoldBright : cTextMid, off.c_str());
        if (hov) ImGui::SetTooltip("Comms offset - click to open the Sync editor; right-click for volume and more");
    }

    // The strip's menu (right-click): the file, then what can be done with it. Styled as the play
    // bar's Share menu: padded, bordered, rounded, each row a full-width target with the bar's
    // hover. The volume lives in the play bar's mixer, the mute on this strip's speaker.
    if (c.menuForced && !ImGui::IsPopupOpen("##CommsMenu")) ImGui::OpenPopup("##CommsMenu");
    if (c.menuForced)   // the self-test's picture: over the strip's right end
        ImGui::SetNextWindowPos(ImVec2(laneX + laneW, y0 - 4.f), ImGuiCond_Always, ImVec2(1.f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 2.f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.06f, 0.04f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.78f, 0.66f, 0.29f, 0.40f));
    const bool menu = ImGui::BeginPopup("##CommsMenu");
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
    if (menu)
    {
        const float u = std::max(0.85f, fs / 15.f);
        const float maxNameW = 320.f * u;
        const std::string sub = c.track ? Comms::ClockText(c.track->Duration()) + " \xC2\xB7 stays on this PC"
                                        : std::string("stays on this PC");
        // The menu's width: the name (up to its cap), the sub line, the longest row.
        const float rowTextX = 26.f * u;
        float menuW = std::max({font->CalcTextSizeA(fs, FLT_MAX, 0.f, c.name.c_str()).x,
                                font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, sub.c_str()).x,
                                rowTextX + font->CalcTextSizeA(fs, FLT_MAX, 0.f, "Remove voice comms").x + 12.f * u});
        menuW = std::clamp(menuW, 200.f * u, maxNameW);
        ImDrawList* pdl = ImGui::GetWindowDrawList();

        // Header: the name (ellipsized, the full name and path on hover), and its length.
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const std::string shown = Ellipsize(font, fs, c.name, menuW);
            ImGui::Dummy(ImVec2(menuW, fs + 2.f * u));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n%s", c.name.c_str(), Comms::ToUtf8(c.path).c_str());
            pdl->AddText(font, fs, ImVec2(p.x, p.y), IM_COL32(242, 238, 228, 255), shown.c_str());
            const ImVec2 q = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(menuW, fsSm + 2.f * u));
            pdl->AddText(font, fsSm, ImVec2(q.x, q.y), IM_COL32(150, 142, 126, 255), sub.c_str());
        }
        auto Separator = [&] {
            ImGui::Dummy(ImVec2(menuW, 4.f * u));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            pdl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + menuW, p.y), IM_COL32(200, 168, 75, 60), 1.f);
            ImGui::Dummy(ImVec2(menuW, 4.f * u));
        };
        // A row: icon, label, the bar's soft hover, the whole width clickable.
        enum class Icon { Wave, Folder, NoEntry };
        auto Row = [&](const char* id, const char* label, Icon icon, bool danger, bool enabled) {
            const float h = fs + 10.f * u;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (!enabled) ImGui::BeginDisabled();
            ImGui::InvisibleButton(id, ImVec2(menuW, h));
            const bool hov = enabled && ImGui::IsItemHovered();
            const bool clicked = enabled && ImGui::IsItemClicked();
            if (!enabled) ImGui::EndDisabled();
            m_playBarHot.push_back({id, ImVec4(p.x, p.y, p.x + menuW, p.y + h)});
            if (hov)
                pdl->AddRectFilled(ImVec2(p.x - 4.f * u, p.y), ImVec2(p.x + menuW + 4.f * u, p.y + h),
                                   danger ? IM_COL32(200, 80, 70, 40) : IM_COL32(255, 244, 222, 24), 4.f);
            const ImU32 fg = !enabled ? IM_COL32(110, 104, 92, 255)
                           : danger   ? (hov ? IM_COL32(245, 150, 138, 255) : IM_COL32(214, 112, 100, 255))
                                      : (hov ? IM_COL32(248, 242, 230, 255) : IM_COL32(222, 212, 188, 255));
            const ImVec2 ic(p.x + 9.f * u, p.y + h * 0.5f);
            const float s = u;
            if (icon == Icon::Wave)
            {
                // A small waveform: the editor's clip.
                const float hs[] = {2.f, 5.f, 3.5f, 6.5f, 4.f, 2.5f};
                for (int k = 0; k < 6; ++k)
                {
                    const float x = ic.x - 6.f * s + k * 2.4f * s;
                    pdl->AddLine(ImVec2(x, ic.y - hs[k] * s), ImVec2(x, ic.y + hs[k] * s), fg, 1.4f * s);
                }
            }
            else if (icon == Icon::Folder)
            {
                pdl->AddRect(ImVec2(ic.x - 6.5f * s, ic.y - 3.5f * s), ImVec2(ic.x + 6.5f * s, ic.y + 5.f * s), fg, 1.5f * s, 0, 1.3f * s);
                pdl->AddLine(ImVec2(ic.x - 6.5f * s, ic.y - 3.5f * s), ImVec2(ic.x - 5.f * s, ic.y - 6.f * s), fg, 1.3f * s);
                pdl->AddLine(ImVec2(ic.x - 5.f * s, ic.y - 6.f * s), ImVec2(ic.x - 1.f * s, ic.y - 6.f * s), fg, 1.3f * s);
                pdl->AddLine(ImVec2(ic.x - 1.f * s, ic.y - 6.f * s), ImVec2(ic.x + 0.5f * s, ic.y - 3.5f * s), fg, 1.3f * s);
            }
            else
            {
                // The editor's Remove icon: the red no-entry sign of Toolbar/confirm_yesno.png.
                float tw = 0.f, th = 0.f;
                ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;
                ImTextureID tex = dev ? LoadRibbonArt(dev, "Toolbar\\confirm_yesno.png", &tw, &th) : ImTextureID{};
                const float r = 7.5f * s;
                if (tex && tw > 0.f && th > 0.f)
                {
                    const float gx = hov ? 32.f : 0.f, gy = 32.f;
                    pdl->AddImage(tex, ImVec2(ic.x - r, ic.y - r), ImVec2(ic.x + r, ic.y + r),
                                  ImVec2(gx / tw, gy / th), ImVec2((gx + 32.f) / tw, (gy + 32.f) / th));
                }
                else
                    pdl->AddCircle(ic, r * 0.8f, fg, 16, 1.5f * s);
            }
            pdl->AddText(font, fs, ImVec2(p.x + rowTextX, p.y + (h - fs) * 0.5f), fg, label);
            return clicked;
        };

        Separator();
        if (Row("##CmOpen", "Open sync editor", Icon::Wave, false, c.phase == Phase::Ready))
        {
            c.syncWin = true;
            c.SetStatus("", false, 0.0);
            c.viewSet = false;
            ImGui::CloseCurrentPopup();
        }
        if (Row("##CmReplace", "Replace file...", Icon::Folder, false, true))
        {
            OpenDialog(false);
            ImGui::CloseCurrentPopup();
        }
        Separator();
        if (Row("##CmRemove", "Remove voice comms", Icon::NoEntry, true, true))
        {
            ImGui::CloseCurrentPopup();
            ForgetComms("removed");
        }
        ImGui::EndPopup();
    }

    DrawCommsEditor(maxT);

    // DEV TOOL (the comms self-test): the pointer over a play bar or editor button for the next
    // frame, by ImGui's own input queue (the real cursor never moves); "__away" takes it off.
    if (!c.hoverSim.empty())
    {
        ImGuiIO& sio = ImGui::GetIO();
        if (c.hoverSim == "__away")
        {
            sio.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            c.hoverSim.clear();
        }
        else
            for (const auto& h : m_playBarHot)
                if (h.first == c.hoverSim)
                    sio.AddMousePosEvent((h.second.x + h.second.z) * 0.5f, (h.second.y + h.second.w) * 0.5f);
    }

    if (c.dirty && c.laneDrag == 0 && c.edDrag == 0 && !ImGui::IsAnyItemActive()) SaveCommsEntry();
}

// ---------------------------------------------------------------------------
// The Sync editor: two tracks on one time axis in MATCH time, as a video editor's sequence. The
// Match track is the reference (the match's sound activity); the Comms clip slides on it.
// ---------------------------------------------------------------------------

void ReplayWindow::DrawCommsEditor(float maxTf)
{
    auto& c = *m_comms;
    if (!(c.syncWin && c.phase == Phase::Ready && c.track))
    {
        c.syncWinShown = false;
        c.edHot = false;
        c.edDrag = 0;
        return;
    }
    const Comms::Track& tr = *c.track;
    const double dur = tr.Duration(), maxT = std::max(1.f, maxTf), dto = m_displayTimeOffset;
    const ImGuiIO& io = ImGui::GetIO();
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float fsSm = fs * 0.84f;
    const float u = std::max(0.85f, fs / 15.f);   // the UI scale, from the font
    const ImU32 cGold    = IM_COL32(226, 194, 106, 255);
    const ImU32 cTextDim = kEdText3;
    const ImU32 cIcon    = IM_COL32(225, 225, 230, 255);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float winW = std::clamp(vp->Size.x * 0.76f, std::min(560.f * u, vp->Size.x - 16.f), vp->Size.x - 16.f);
    const float pad = 8.f * u;
    const float titleH = ImGui::GetFrameHeight();   // the panel's own title bar, as the others have
    const float readH = fs + 6.f * u;
    const float rulerH = 18.f * u, matchH = 50.f * u, commsH = 68.f * u, gap = 4.f * u;
    const float playSz = 34.f * u, iconSz = 28.f * u;
    const float winH = titleH + pad + readH + 4.f * u + rulerH + matchH + gap + commsH + 8.f * u + playSz + pad;

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y - PlayBarHeight() - 8.f * u),
                            ImGuiCond_Always, ImVec2(0.5f, 1.f));
    ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
    // In front of the party windows and the rest when it opens.
    if (!c.syncWinShown) { ImGui::SetNextWindowFocus(); c.syncWinShown = true; }
    // The frame of the app's floating panels (Combat Log, Skill Analytics, Morale, Lord Damage...):
    // their dark slate background, title bar, thin brown-gold border and 6 px rounding. The tracks
    // inside keep the editor's own colours.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,      ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    constexpr int kColours = 4, kVars = 3;
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoNav;
    bool open = true;
    bool close = false;
    if (ImGui::Begin("Sync voice comms###CommsSync", &open, kFlags))
    {
        // Over the party windows and the panels, as the clip panel keeps itself.
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        c.edHot = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
                  ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || c.edDrag != 0;

        const ImVec2 w0 = ImGui::GetWindowPos();
        const float L = w0.x + pad, R = w0.x + winW - pad;
        const float rowY = w0.y + titleH + pad;
        const float hdrW = 150.f * u;
        const float ax0 = L + hdrW + 6.f * u, ax1 = R, areaW = std::max(50.f, ax1 - ax0);
        const float rY0 = rowY + readH + 4.f * u, rY1 = rY0 + rulerH;
        const float mY0 = rY1, mY1 = mY0 + matchH;
        const float cY0 = mY1 + gap, cY1 = cY0 + commsH;
        const float bY0 = cY1 + 8.f * u;   // the bottom row: transport, hint, Remove / Done

        // ── The top row: where the recording starts, live while it moves ──
        {
            const std::string words = StartWords(-c.offset - dto);
            wdl->AddText(font, fs, ImVec2(L, rowY + (readH - fs) * 0.5f), IM_COL32(232, 223, 200, 255),
                         Ellipsize(font, fs, words, R - L).c_str());
        }

        // ── The bottom row: [Play] 2:32 / 28:06 · hint · [Remove] [Done] ──
        bool remove = false;
        {
            // Play / Pause: the play bar's own button, a gold framed square, bigger than the rest.
            ImGui::SetCursorScreenPos(ImVec2(L, bY0));
            ImGui::InvisibleButton("##EdPlay", ImVec2(playSz, playSz));
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) m_replayCtx.isPlaying = !m_replayCtx.isPlaying;
            const ImVec2 a(L, bY0), b(L + playSz, bY0 + playSz);
            wdl->AddRectFilled(a, b, IM_COL32(200, 168, 75, 18), 6.f);
            if (hov) wdl->AddRectFilled(a, b, IM_COL32(255, 244, 222, 24), 6.f);
            wdl->AddRect(a, b, IM_COL32(200, 168, 75, 82), 6.f);
            const ImU32 fg = hov ? IM_COL32(248, 242, 230, 255) : IM_COL32(200, 168, 75, 255);
            const float pcx = L + playSz * 0.5f, pcy = bY0 + playSz * 0.5f, s = u;
            if (!m_replayCtx.isPlaying)
                wdl->AddTriangleFilled(ImVec2(pcx - 4.f * s, pcy - 5.f * s), ImVec2(pcx + 5.f * s, pcy), ImVec2(pcx - 4.f * s, pcy + 5.f * s), fg);
            else
            {
                wdl->AddRectFilled(ImVec2(pcx - 4.5f * s, pcy - 5.f * s), ImVec2(pcx - 1.5f * s, pcy + 5.f * s), fg);
                wdl->AddRectFilled(ImVec2(pcx + 1.5f * s, pcy - 5.f * s), ImVec2(pcx + 4.5f * s, pcy + 5.f * s), fg);
            }
            if (hov) ImGui::SetTooltip(m_replayCtx.isPlaying ? "Pause (Space)" : "Play the replay with the voice comms (Space)");

            // The match clock beside it.
            const std::string clock = Comms::ClockText(m_debugTimeline - dto) + " / " + Comms::ClockText(maxT - dto);
            wdl->AddText(font, fs, ImVec2(L + playSz + 8.f * u, bY0 + (playSz - fs) * 0.5f), IM_COL32(232, 223, 200, 255), clock.c_str());

            // Remove and Done: the icons of the bookmark dialog's Cancel and Add (the red no-entry
            // sign and the green tick, Toolbar/confirm_yesno.png, lit on hover) in its button frame.
            auto IconButton = [&](const char* id, float x, bool negative, const char* tip) {
                const float y = bY0 + (playSz - iconSz) * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(x, y));
                ImGui::InvisibleButton(id, ImVec2(iconSz, iconSz));
                const bool h = ImGui::IsItemHovered();
                const bool clicked = ImGui::IsItemClicked();
                m_playBarHot.push_back({id, ImVec4(x, y, x + iconSz, y + iconSz)});
                const ImVec2 p0(x, y), p1(x + iconSz, y + iconSz);
                wdl->AddRectFilled(p0, p1, h ? IM_COL32(46, 46, 56, 230) : IM_COL32(31, 31, 38, 230), 6.f);
                wdl->AddRect(p0, p1, negative ? IM_COL32(209, 71, 71, 217) : IM_COL32(77, 184, 89, 217), 6.f);
                float tw = 0.f, th = 0.f;
                ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;
                ImTextureID tex = dev ? LoadRibbonArt(dev, "Toolbar\\confirm_yesno.png", &tw, &th) : ImTextureID{};
                const ImVec2 ic(x + iconSz * 0.5f, y + iconSz * 0.5f);
                const float r = iconSz * 0.5f - 5.f * u;
                if (tex && tw > 0.f && th > 0.f)
                {
                    const float gx = h ? 32.f : 0.f, gy = negative ? 32.f : 0.f;
                    wdl->AddImage(tex, ImVec2(ic.x - r, ic.y - r), ImVec2(ic.x + r, ic.y + r),
                                  ImVec2(gx / tw, gy / th), ImVec2((gx + 32.f) / tw, (gy + 32.f) / th));
                }
                else
                    wdl->AddCircleFilled(ic, r * 0.7f, negative ? IM_COL32(209, 71, 71, 255) : IM_COL32(77, 184, 89, 255), 20);
                if (h) ImGui::SetTooltip("%s", tip);
                return clicked;
            };
            const float doneX = R - iconSz, remX = doneX - 6.f * u - iconSz;
            remove = IconButton("##EdRemove", remX, true, "Remove the voice comms");
            if (IconButton("##EdDone", doneX, false, "Done")) close = true;

            // The hint, between the clock and the buttons.
            const float hx = std::max(ax0, L + playSz + 8.f * u + font->CalcTextSizeA(fs, FLT_MAX, 0.f, clock.c_str()).x + 16.f * u);
            const std::string hint = Ellipsize(font, fsSm,
                "Drag the clip \xC2\xB7 arrows to nudge (Shift: 1 s) \xC2\xB7 Alt: no snap \xC2\xB7 wheel: zoom \xC2\xB7 "
                "right drag: pan \xC2\xB7 double-click the ruler: fit", remX - 12.f * u - hx);
            wdl->AddText(font, fsSm, ImVec2(hx, bY0 + (playSz - fsSm) * 0.5f), cTextDim, hint.c_str());
            if (dur < 60.0)
            {
                const std::string warn = "This recording is only " + Comms::ClockText(dur) + " long";
                wdl->AddText(font, fsSm, ImVec2(hx, bY0 + playSz - fsSm + 2.f * u), IM_COL32(230, 160, 90, 255), warn.c_str());
            }
        }
        if (remove)
        {
            // As a bookmark's delete: at once, no confirmation (a dropped file comes back by
            // dropping it again).
            ForgetComms("removed");
            ImGui::End();
            ImGui::PopStyleColor(kColours);
            ImGui::PopStyleVar(kVars);
            return;
        }

        // ── The view: replay time, shown as match time (0:00 = the match start) ──
        const double clipStart = -c.offset, clipEnd = dur - c.offset;
        const double fit0 = std::min(0.0, clipStart), fit1 = std::max(maxT, clipEnd);
        const double fitPad = 0.03 * (fit1 - fit0);
        if (!c.viewSet)
        {
            c.view0 = fit0 - fitPad;
            c.view1 = fit1 + fitPad;
            c.viewSet = true;
        }
        auto ClampView = [&] {
            const double lim0 = fit0 - 0.5 * (fit1 - fit0), lim1 = fit1 + 0.5 * (fit1 - fit0);
            const double w = std::clamp(c.view1 - c.view0, 1.0, lim1 - lim0);
            c.view0 = std::clamp(c.view0, lim0, lim1 - w);
            c.view1 = c.view0 + w;
        };
        ClampView();
        const double span = c.view1 - c.view0;
        const double secPerPx = span / areaW;
        auto X = [&](double t) { return ax0 + static_cast<float>((t - c.view0) / span) * areaW; };
        auto T = [&](float x) { return c.view0 + (x - ax0) / areaW * span; };

        // ── Track headers ──
        auto Header = [&](const char* id, float y0, float y1, bool& muted, const char* tip) {
            wdl->AddRectFilled(ImVec2(L, y0), ImVec2(L + hdrW, y1), kEdHeader, 4.f * u);
            wdl->AddRect(ImVec2(L, y0), ImVec2(L + hdrW, y1), kEdEdge, 4.f * u);
            const float bs = 22.f * u, by = y0 + 5.f * u;
            ImGui::SetCursorScreenPos(ImVec2(L + 5.f * u, by));
            ImGui::InvisibleButton(id, ImVec2(bs, bs));
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) { muted = !muted; c.dirty = true; }
            if (hov || muted)
                wdl->AddRectFilled(ImVec2(L + 5.f * u, by), ImVec2(L + 5.f * u + bs, by + bs),
                                   muted ? IM_COL32(200, 70, 64, 120) : IM_COL32(255, 255, 255, 30), 3.f * u);
            DrawSpeaker(wdl, ImVec2(L + 5.f * u + bs * 0.5f, by + bs * 0.5f), u, !muted, muted ? IM_COL32(255, 150, 140, 255) : cIcon);
            if (hov) ImGui::SetTooltip("%s", tip);
            return L + 5.f * u + bs + 6.f * u;   // where the name goes
        };
        {
            const float nx = Header("##EdGameMute", mY0, mY1, c.gameMuted,
                                    c.gameMuted ? "Unmute the game's sound" : "Mute the game's sound while lining up");
            wdl->AddText(font, fs, ImVec2(nx, mY0 + 5.f * u + (22.f * u - fs) * 0.5f), kEdText, "Match");
            wdl->AddText(font, fsSm, ImVec2(L + 8.f * u, mY1 - fsSm - 5.f * u), cTextDim, "Game sound");
        }
        {
            bool muted = c.muted;
            const float nx = Header("##EdCommsMute", cY0, cY1, muted, c.muted ? "Voice comms: unmute" : "Voice comms: mute");
            c.muted = muted;
            wdl->AddText(font, fs, ImVec2(nx, cY0 + 5.f * u + (22.f * u - fs) * 0.5f), kEdText, "Comms");
            // The file's name under it (its volume is in the play bar's mixer).
            const std::string nm = Ellipsize(font, fsSm, c.name, hdrW - 14.f * u);
            wdl->AddText(font, fsSm, ImVec2(L + 8.f * u, cY1 - fsSm - 6.f * u), cTextDim, nm.c_str());
        }

        // ── The ruler ──
        // Labels every `step`; minor ticks on the round grid the playhead snaps to (whole seconds
        // zoomed in, 5 s / 10 s / 30 s / 1 min zoomed out).
        static const double kSteps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
        double step = kSteps[std::size(kSteps) - 1];
        for (double s : kSteps)
            if (s / secPerPx >= 72.0 * u) { step = s; break; }
        static const double kGrid[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800};
        double grid = step;
        for (double g : kGrid)
            if (g / secPerPx >= 12.0 * u) { grid = std::min(g, step); break; }
        wdl->AddRectFilled(ImVec2(ax0, rY0), ImVec2(ax1, rY1), kEdRuler, 3.f * u, ImDrawFlags_RoundCornersTop);
        wdl->AddLine(ImVec2(ax0, rY1 - 0.5f), ImVec2(ax1, rY1 - 0.5f), kEdEdge, 1.f);
        {
            wdl->PushClipRect(ImVec2(ax0, rY0), ImVec2(ax1, cY1), true);
            if (grid < step)
                for (double m = std::ceil((c.view0 - dto) / grid) * grid; m + dto <= c.view1; m += grid)
                {
                    const float x = X(m + dto);
                    wdl->AddLine(ImVec2(x, rY1 - 5.f * u), ImVec2(x, rY1), IM_COL32(205, 205, 212, 150), 1.f);
                }
            for (double m = std::ceil((c.view0 - dto) / step) * step; m + dto <= c.view1; m += step)
            {
                const float x = X(m + dto);
                const bool zero = std::fabs(m) < step * 0.01;
                wdl->AddLine(ImVec2(x, rY0 + 3.f * u), ImVec2(x, rY1), zero ? cGold : IM_COL32(225, 225, 230, 220), 1.f);
                const std::string lbl = step < 1.0 ? ClockTenths(m) : Comms::ClockText(m);
                if (x + 3.f * u + font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, lbl.c_str()).x < ax1 - 2.f * u)
                    wdl->AddText(font, fsSm, ImVec2(x + 3.f * u, rY0 + (rulerH - fsSm) * 0.5f), zero ? cGold : kEdText, lbl.c_str());
            }
            wdl->PopClipRect();
        }

        // ── The tracks: lighter lanes, a dark seam between them ──
        wdl->AddRectFilled(ImVec2(ax0, mY0), ImVec2(ax1, mY1), kEdLane);
        wdl->AddRectFilled(ImVec2(ax0, cY0), ImVec2(ax1, cY1), kEdLane);
        wdl->AddRectFilled(ImVec2(ax0, mY1), ImVec2(ax1, cY0), kEdEdge);
        wdl->PushClipRect(ImVec2(ax0, rY0), ImVec2(ax1, cY1), true);
        const float bandH = fsSm + 4.f * u;
        auto ClipLabel = [&](float x0, float x1, float y, const std::string& text, ImU32 col) {
            // The label stays readable while the clip's start is scrolled off the left.
            const float lx = std::max(x0, ax0) + 5.f * u;
            const float room = std::min(x1, ax1) - 5.f * u - lx;
            const std::string s = Ellipsize(font, fsSm, text, room);
            if (!s.empty()) wdl->AddText(font, fsSm, ImVec2(lx, y + 2.f * u), col, s.c_str());
        };
        // The Match clip: the whole replay, the pre-match minute a shade darker.
        {
            const float x0 = X(0.0), x1 = X(maxT);
            const ImVec2 a(x0, mY0 + 2.f * u), b(x1, mY1 - 2.f * u);
            wdl->AddRectFilled(a, b, kMatchFill, 4.f * u);
            wdl->AddRectFilled(a, ImVec2(x1, a.y + bandH), kMatchBand, 4.f * u, ImDrawFlags_RoundCornersTop);
            if (dto > 0.5) wdl->AddRectFilled(ImVec2(x0, a.y + bandH), ImVec2(X(dto), b.y), IM_COL32(0, 0, 0, 70));
            const float wy0 = a.y + bandH, wy1 = b.y;
            DrawActivity(wdl, c.activity, std::max(ax0, x0), std::min(ax1, x1), T(std::max(ax0, x0)), secPerPx,
                         (wy0 + wy1) * 0.5f, (wy1 - wy0) * 0.5f - 2.f * u, kMatchWave);
            wdl->AddRect(a, b, kMatchBorder, 4.f * u);
            const char* map = GetMapNameForLoading(m_matchMeta.map_id);
            const std::string label = std::string(map && *map ? map : "Match") + "   " + Comms::ClockText(maxT - dto);
            ClipLabel(x0, x1, a.y, label, kEdText);
        }
        // The Comms clip.
        const float cx0 = X(clipStart), cx1 = X(clipEnd);
        const ImVec2 mp = io.MousePos;
        const bool inComms = mp.y >= cY0 && mp.y <= cY1 && mp.x >= ax0 && mp.x <= ax1;
        const bool overClip = inComms && mp.x >= cx0 - 3.f * u && mp.x <= cx1 + 3.f * u;
        {
            const bool hot = c.edDrag == 1 || (overClip && c.edDrag == 0);
            const ImVec2 a(cx0, cY0 + 2.f * u), b(cx1, cY1 - 2.f * u);
            wdl->AddRectFilled(a, b, kClipFill, 4.f * u);
            wdl->AddRectFilled(a, ImVec2(b.x, a.y + bandH), kClipBand, 4.f * u, ImDrawFlags_RoundCornersTop);
            const float wy0 = a.y + bandH + 1.f * u, wy1 = b.y - 1.f * u;
            DrawWave(wdl, tr, std::max(ax0, cx0), std::min(ax1, cx1), T(std::max(ax0, cx0)) + c.offset, secPerPx,
                     (wy0 + wy1) * 0.5f, (wy1 - wy0) * 0.5f, kWavePeak, kWaveRms);
            wdl->AddRect(a, b, hot ? kClipHot : kClipBorder, 4.f * u, 0, hot ? 2.f : 1.f);
            ClipLabel(cx0, cx1, a.y, c.name + "   " + Comms::ClockText(dur), kEdText);
        }
        // MATCH 0:00, faint, through both tracks: what the clip is lined up against.
        {
            const float zx = X(dto);
            for (float y = mY0; y < cY1; y += 6.f * u)
                wdl->AddLine(ImVec2(zx, y), ImVec2(zx, std::min(cY1, y + 3.f * u)), IM_COL32(235, 235, 240, 110), 1.f);
        }
        wdl->PopClipRect();
        // The playhead is drawn after the mouse (below), so a scrub's snap shows on the same frame.

        // ── The mouse: drag the clip; click / drag elsewhere to scrub; right or middle drag pans;
        // the wheel zooms around the pointer (Shift: pans). ──
        ImGui::SetCursorScreenPos(ImVec2(ax0, rY0));
        ImGui::InvisibleButton("##EdArea", ImVec2(areaW, cY1 - rY0),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        if (ImGui::IsItemActivated())
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                c.edDrag = overClip ? 1 : 2;
                c.phSticky = false;
                c.dragX0 = mp.x;
                c.dragOffset0 = c.offset;
                c.dragMoved = false;
            }
            else
            {
                c.edDrag = 3;
                c.panX0 = mp.x;
                c.panView0 = c.view0;
                c.panView1 = c.view1;
            }
        }
        // Self-test: a drag of the clip driven by code, from its middle to `simTarget` + `simMiss` px.
        if (c.simDrag && c.edDrag == 0)
        {
            c.edDrag = 1;
            c.dragX0 = std::clamp((cx0 + cx1) * 0.5f, ax0 + 10.f, ax1 - 10.f);
            c.dragOffset0 = c.offset;
            c.dragMoved = false;
        }
        // ...and a scrub driven by code, to `simScrubT` + `simMiss` px (the sticky playhead).
        if (c.simScrub && c.edDrag == 0)
        {
            c.edDrag = 2;
            c.phSticky = false;
        }
        const float mx = c.simDrag ? c.dragX0 + (X(c.simTarget) - X(-c.dragOffset0)) + c.simMiss * u : mp.x;
        const bool held = active || (c.edDrag == 1 && c.simDrag) || (c.edDrag == 2 && c.simScrub);
        c.snapName.clear();
        if (c.edDrag == 1 && held)
        {
            if (std::fabs(mx - c.dragX0) > 2.f && !c.dragMoved) { c.dragMoved = true; c.SetStatus("", false, 0.0); }
            if (c.dragMoved)
            {
                double start = -c.dragOffset0 + (mx - c.dragX0) * secPerPx;
                if (!io.KeyAlt)
                    start = SnapStart(start, 1.0 / secPerPx, SnapTargets(dto, m_debugTimeline), 8.f * u, c.snapName);
                if (start != -c.offset) { c.offset = -start; c.seekNow = true; }
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
        }
        else if (c.edDrag == 2 && held)
        {
            // The sticky playhead: within 6 px of a round time (the ruler's minor grid), MATCH 0:00,
            // the comms clip's ends or the match end, it lands there, and holds there until the
            // pointer leaves that radius. Alt: no snap.
            const float sx = c.simScrub ? X(c.simScrubT) + c.simMiss * u : mp.x;
            double t = T(sx);
            const float radius = 6.f * u;
            if (io.KeyAlt)
                c.phSticky = false;
            else
            {
                if (c.phSticky && std::fabs(X(c.phSnapT) - sx) > radius) c.phSticky = false;
                if (!c.phSticky)
                {
                    double best = t;
                    float bestPx = radius;
                    bool found = false;
                    auto consider = [&](double s) {
                        const float d = std::fabs(X(s) - sx);
                        if (d <= bestPx) { bestPx = d; best = s; found = true; }
                    };
                    consider(std::round((t - dto) / grid) * grid + dto);
                    consider(dto);
                    consider(clipStart);
                    consider(clipEnd);
                    consider(maxT);
                    if (found) { c.phSticky = true; c.phSnapT = best; }
                }
                if (c.phSticky) t = c.phSnapT;
            }
            m_debugTimeline = static_cast<float>(std::clamp(t, 0.0, maxT));
        }
        else if (c.edDrag == 3 && held)
        {
            const double d = (mp.x - c.panX0) / areaW * (c.panView1 - c.panView0);
            c.view0 = c.panView0 - d;
            c.view1 = c.panView1 - d;
            ClampView();
        }
        if (c.edDrag != 0 && !held)
        {
            if (c.edDrag == 1 && c.dragMoved && c.offset != c.dragOffset0)
            {
                c.dirty = true;
                c.SetStatus("", false, 0.0);
                RunLog::Line("comms: clip dragged in the Sync editor - offset %+.3f s", c.offset);
            }
            else if (c.edDrag == 1 && !c.dragMoved)
                m_debugTimeline = static_cast<float>(std::clamp(T(c.dragX0), 0.0, maxT));
            c.edDrag = 0;
        }
        if (hovered && c.edDrag == 0 && overClip) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        // Double-click on the ruler: fit the whole match and the whole recording again.
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && mp.y >= rY0 && mp.y <= rY1)
            c.viewSet = false;
        if (hovered && (io.MouseWheel != 0.f || io.MouseWheelH != 0.f))
        {
            if (io.KeyShift || io.MouseWheelH != 0.f)
            {
                const float w = io.MouseWheelH != 0.f ? -io.MouseWheelH : io.MouseWheel;
                const double d = -w * 0.15 * span;
                c.view0 += d;
                c.view1 += d;
            }
            else
            {
                const double tm = T(mp.x);
                const double w = span * std::pow(0.8, static_cast<double>(io.MouseWheel));
                c.view0 = tm - (tm - c.view0) * (w / span);
                c.view1 = c.view0 + w;
            }
            ClampView();
        }

        // The playhead: a coral line through the ruler and both tracks, with a grab handle on the
        // ruler; while scrubbing, its time in a small label on top.
        {
            const float px = X(m_debugTimeline);
            const bool scrubbing = c.edDrag == 2;
            const float hw = 6.f * u, hh = rulerH * 0.7f;
            wdl->PushClipRect(ImVec2(ax0 - hw, rY0), ImVec2(ax1 + hw, cY1), true);
            wdl->AddLine(ImVec2(px, rY0), ImVec2(px, cY1), kPlayhead, scrubbing ? 2.f : 1.5f);
            const ImVec2 head[5] = {ImVec2(px - hw, rY0 + 1.f), ImVec2(px + hw, rY0 + 1.f), ImVec2(px + hw, rY0 + hh - 4.f * u),
                                    ImVec2(px, rY0 + hh + 1.f * u), ImVec2(px - hw, rY0 + hh - 4.f * u)};
            wdl->AddConvexPolyFilled(head, 5, kPlayhead);
            wdl->PopClipRect();
            if (hovered && c.edDrag == 0 && mp.y >= rY0 && mp.y <= rY1 && std::fabs(mp.x - px) <= hw + 2.f * u)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (scrubbing && px >= ax0 - 1.f && px <= ax1 + 1.f)
            {
                const double m = m_debugTimeline - dto;
                const bool whole = std::fabs(m - std::round(m)) < 0.05;
                const std::string lbl = whole ? Comms::ClockText(m) : ClockTenths(m);
                const ImVec2 ts = font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, lbl.c_str());
                const float bw = ts.x + 10.f * u, bh = ts.y + 4.f * u;
                const float bx = std::clamp(px - bw * 0.5f, w0.x + 2.f, w0.x + winW - 2.f - bw);
                const ImVec2 a(bx, rY0 - bh - 1.f * u), b(bx + bw, rY0 - 1.f * u);
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                fg->AddRectFilled(a, b, kPlayhead, 3.f * u);
                fg->AddText(font, fsSm, ImVec2(a.x + 5.f * u, a.y + 2.f * u), IM_COL32(255, 255, 255, 255), lbl.c_str());
            }
        }

        // While the clip moves: the snap line and where the recording now starts.
        if (c.edDrag == 1 && c.dragMoved)
        {
            const float sx = X(-c.offset);
            if (!c.snapName.empty())
                wdl->AddLine(ImVec2(sx, rY0), ImVec2(sx, cY1), kSnapLine, 2.f);
            std::string tip = StartWords(-c.offset - dto);
            if (!c.snapName.empty()) tip += "  (snapped to " + c.snapName + ")";
            TipBox(ImGui::GetForegroundDrawList(), font, fsSm, sx, cY0 - 3.f * u, ax0, ax1, tip);
        }

        // ── The keys: arrows nudge the clip (Shift: 1 s). Space plays, as everywhere. ──
        if (c.edHot && !io.WantTextInput)
        {
            const int dir = (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) ? 1 : 0);
            if (dir != 0)
            {
                c.offset -= dir * (io.KeyShift ? 1.0 : 0.1);
                c.seekNow = true;
                c.dirty = true;
                c.SetStatus("", false, 0.0);
            }
        }

    }
    ImGui::End();
    ImGui::PopStyleColor(kColours);
    ImGui::PopStyleVar(kVars);
    if (close || !open)
    {
        c.syncWin = false;
        c.syncWinShown = false;
        c.edHot = false;
        c.edDrag = 0;
    }
}

// ---------------------------------------------------------------------------
// DEV TOOL (GWO_COMMS_SELFTEST): a synthetic recording of this match through the real loader,
// auto sync and playback, reported in the RunLog. See Comms::Synthesise.
//   GWO_COMMS_SELFTEST=<match folder>[;<match folder>...]   (opened by the batch, MapBrowser.cpp)
//   GWO_COMMS_SELFTEST_MODE=sync | hard | nogame | reattach | file (default sync)
//   GWO_COMMS_SELFTEST_OFFSET=<seconds>                      (default 47.3)
//   GWO_COMMS_SELFTEST_FILE=<a real recording>               (mode file: attached at the offset)
//   GWO_COMMS_SELFTEST_DIR=<folder for the wav and the pictures>   (default %TEMP%)
// Pictures: <folder>.comms_editor.png (the Sync editor), .comms_drag.png (a clip drag driven by
// code, 5 px short of the playhead: the magnet), .comms_lane.png (the lane alone).
// ---------------------------------------------------------------------------

bool ReplayWindow::CommsShotPending() const { return m_comms && m_comms->shotPending; }

void ReplayWindow::SaveCommsShot()
{
    if (!m_comms) return;
    m_comms->shotPending = false;
    DirectX::ScratchImage shot;
    if (FAILED(DirectX::CaptureTexture(m_deviceResources->GetD3DDevice(), m_deviceResources->GetD3DDeviceContext(),
                                       m_deviceResources->GetRenderTarget(), shot)))
        return;
    DirectX::SaveToWICFile(*shot.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE,
                           DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), m_comms->shotPath.c_str());
    RunLog::Line("comms-selftest: picture '%s'", m_comms->shotPath.string().c_str());
}

void ReplayWindow::ProcessCommsSelfTest()
{
    const auto& cfg = Comms::SelfTestCfg();
    if (!cfg.on || !m_comms || m_comms->stDone) return;
    if (CommsSelfTestDone()) return;   // not this window's match
    auto& c = *m_comms;
    const double now = Comms::NowSeconds();
    const std::string& folder = m_matchMeta.folder_name;

    auto speedTo = [&](int idx) {
        m_replayCtx.speedIndex = idx;
        m_replayCtx.playbackSpeed = kReplaySpeeds[idx];
    };
    auto driftReset = [&] { c.reseeks = 0; c.driftSum = 0; c.driftMax = 0; c.driftCount = 0; };
    auto driftLine = [&](const char* what) {
        RunLog::Line("comms-selftest: playback %s - voice %s, %d reseek(s), drift mean %.1f ms max %.1f ms over %d frames",
                     what, c.player.Active() ? "playing" : "stopped", c.reseeks,
                     c.driftCount ? 1000.0 * c.driftSum / c.driftCount : 0.0, 1000.0 * c.driftMax, c.driftCount);
    };

    // Off every monitor, visible, unfocused (as the clip self-test does): a real frame to take.
    auto place = [&](int w, int h) {
        RECT r = {0, 0, w, h};
        AdjustWindowRect(&r, GetWindowLong(m_hwnd, GWL_STYLE), FALSE);
        SetWindowPos(m_hwnd, HWND_BOTTOM, 20000, 20000, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    };
    auto shot = [&](const char* name, int next) {
        c.shotPath = cfg.dir / (folder + name);
        c.shotPending = true;
        c.stAt = now;
        c.stStage = next;
    };
    auto shotDone = [&] { return !c.shotPending || now - c.stAt > 10.0; };

    switch (c.stStage)
    {
    // ---- playbar: the play bar empty and with comms, at three widths, the mixer, the Share menu
    case 40: if (now - c.stAt < 2.5) return; shot(".pb_1280_empty.png", 41); return;
    case 41: if (!shotDone()) return; place(1920, 1080); c.stAt = now; c.stStage = 42; return;
    case 42: if (now - c.stAt < 2.0) return; shot(".pb_1920_empty.png", 43); return;
    case 43:
        if (!shotDone()) return;
        if (cfg.file.empty()) { RunLog::Line("comms-selftest: RESULT playbar - GWO_COMMS_SELFTEST_FILE not set"); c.stDone = true; return; }
        CommsAttachFile(cfg.file, true, cfg.offset);
        place(1280, 720);
        c.stAt = now;
        c.stStage = 44;
        return;
    case 44:
        if (c.phase == Phase::Loading || now - c.stAt < 2.5) return;
        RunLog::Line("comms-selftest: RESULT playbar - comms %s", c.phase == Phase::Ready ? "attached" : "FAILED");
        shot(".pb_1280_comms.png", 45);
        return;
    case 45: if (!shotDone()) return; place(1920, 1080); c.stAt = now; c.stStage = 46; return;
    case 46: if (now - c.stAt < 2.0) return; shot(".pb_1920_comms.png", 47); return;
    case 47: if (!shotDone()) return; place(2560, 1440); c.stAt = now; c.stStage = 48; return;
    case 48: if (now - c.stAt < 2.0) return; shot(".pb_2560_comms.png", 49); return;
    case 49: if (!shotDone()) return; place(1280, 720); c.hoverIdx = 0; c.stAt = now; c.stStage = 60; return;
    case 60:   // each play bar button hovered by code, one picture each
    {
        static const char* kHover[] = {"##Bk5", "##PlayPause", "##Stop", "##Mute", "##Highlights", "##Speed", "##Share", "##ShareMore"};
        if (now - c.stAt < 1.5 && c.hoverIdx == 0) return;
        if (c.hoverIdx >= static_cast<int>(std::size(kHover)))
        {
            c.hoverSim = "__away";
            c.mixerForced = true;
            c.stAt = now;
            c.stStage = 50;
            return;
        }
        c.hoverSim = kHover[c.hoverIdx];
        c.stAt = now;
        c.stStage = 61;
        return;
    }
    case 61:
        if (now - c.stAt < 0.8) return;
        shot((".pb_hover_" + c.hoverSim.substr(2) + ".png").c_str(), 62);
        return;
    case 62: if (!shotDone()) return; ++c.hoverIdx; c.stAt = now; c.stStage = 60; return;
    case 50: if (now - c.stAt < 2.0) return; shot(".pb_mixer.png", 51); return;
    case 51: if (!shotDone()) return; c.mixerForced = false; m_shareMenuForced = true; c.stAt = now; c.stStage = 52; return;
    case 52: if (now - c.stAt < 1.0) return; shot(".pb_share_menu.png", 53); return;
    case 53:
        if (!shotDone()) return;
        m_shareMenuForced = false;
        m_showTeam1Party = c.stParty1;
        m_showTeam2Party = c.stParty2;
        c.stDone = true;
        return;
    // ---- menu
    case 90:
        if (c.phase == Phase::Loading || now - c.stAt < 1.5) return;
        place(1280, 720);   // again: the first resize, while the window opens, does not hold
        c.menuForced = true;
        c.stAt = now;
        c.stStage = 91;
        return;
    case 91: if (now - c.stAt < 1.5) return; shot(".cm_menu.png", 92); return;
    case 92: if (!shotDone()) return; c.hoverSim = "##CmOpen"; c.stAt = now; c.stStage = 93; return;
    case 93: if (now - c.stAt < 0.8) return; shot(".cm_hover_open.png", 94); return;
    case 94: if (!shotDone()) return; c.hoverSim = "##CmRemove"; c.stAt = now; c.stStage = 95; return;
    case 95: if (now - c.stAt < 0.8) return; shot(".cm_hover_remove.png", 96); return;
    case 96:
        if (!shotDone()) return;
        c.hoverSim = "__away";
        c.menuForced = false;
        m_showTeam1Party = c.stParty1;
        m_showTeam2Party = c.stParty2;
        c.stDone = true;
        return;
    // ---- editorui
    case 70:
        if (c.phase == Phase::Loading || now - c.stAt < 1.0) return;
        if (c.phase != Phase::Ready) { RunLog::Line("comms-selftest: RESULT editorui - FAILED to load"); c.stDone = true; return; }
        c.syncWin = true;
        c.viewSet = false;
        m_replayCtx.isPlaying = false;
        place(1920, 1080);   // again: the first resize, while the window opens, does not hold
        c.stAt = now;
        c.stStage = 71;
        return;
    case 71: if (now - c.stAt < 2.0) return; shot(".ed_1920.png", 72); return;
    case 72: if (!shotDone()) return; place(2560, 1440); c.viewSet = false; c.stAt = now; c.stStage = 73; return;
    case 73: if (now - c.stAt < 2.0) return; shot(".ed_2560.png", 74); return;
    case 74: if (!shotDone()) return; place(1920, 1080); c.viewSet = false; c.hoverSim = "##EdRemove"; c.stAt = now; c.stStage = 75; return;
    case 75: if (now - c.stAt < 1.5) return; shot(".ed_hover_remove.png", 76); return;
    case 76: if (!shotDone()) return; c.hoverSim = "##EdDone"; c.stAt = now; c.stStage = 77; return;
    case 77: if (now - c.stAt < 0.8) return; shot(".ed_hover_done.png", 78); return;
    case 78:
        if (!shotDone()) return;
        c.hoverSim = "__away";
        c.stCombatLog = m_showCombatLog;
        m_showCombatLog = true;
        c.stAt = now;
        c.stStage = 79;
        return;
    case 79: if (now - c.stAt < 1.5) return; shot(".ed_vs_combatlog.png", 80); return;
    case 80:
        if (!shotDone()) return;
        m_showCombatLog = c.stCombatLog;
        m_showTeam1Party = c.stParty1;
        m_showTeam2Party = c.stParty2;
        c.syncWin = false;
        c.stDone = true;
        return;
    case 0:
        if (!c.storeChecked || !m_agentsClassified || !m_replayCtx.stocLoaded || !m_timeline.computed) return;
        if (cfg.mode == "reattach") { c.stStage = 10; return; }
        if (cfg.mode == "probe")
        {
            // Which files the loader can read: GWO_COMMS_PROBE=<file>|<file>|... each decoded in
            // turn (on this thread: a test), the outcome logged.
            static wchar_t list[8192] = {};
            if (GetEnvironmentVariableW(L"GWO_COMMS_PROBE", list, 8192) > 0)
            {
                std::wstring v = list;
                for (size_t a = 0; a <= v.size();)
                {
                    size_t b = v.find(L'|', a);
                    if (b == std::wstring::npos) b = v.size();
                    const std::wstring p = v.substr(a, b - a);
                    a = b + 1;
                    if (p.empty()) continue;
                    Comms::LoadJob job;
                    job.path = p;
                    Comms::RunLoad(job);
                    RunLog::Line("comms-selftest: PROBE '%s' - %s, %.1f s of audio at %u Hz%s%s",
                                 Comms::ToUtf8(std::filesystem::path(p).extension().wstring()).c_str(),
                                 job.ok ? "OK" : "FAILED", job.ok && job.track ? job.track->Duration() : 0.0,
                                 job.ok && job.track ? job.track->sampleRate : 0u, job.ok ? "" : " - ",
                                 job.ok ? "" : job.detail.c_str());
                }
            }
            c.stDone = true;
            return;
        }
        if (cfg.mode == "menu")
        {
            // The Comms strip's right-click menu, open and with rows hovered.
            if (cfg.file.empty()) { RunLog::Line("comms-selftest: RESULT menu - GWO_COMMS_SELFTEST_FILE not set"); c.stDone = true; return; }
            CommsAttachFile(cfg.file, true, cfg.offset);
            c.stParty1 = m_showTeam1Party;
            c.stParty2 = m_showTeam2Party;
            m_showTeam1Party = m_showTeam2Party = false;
            place(1280, 720);
            c.stAt = now;
            c.stStage = 90;
            return;
        }
        if (cfg.mode == "editorui")
        {
            // The Sync editor at 1920 and 2560, its icon buttons hovered, and beside the Combat
            // Log, the panel whose frame it now shares.
            if (cfg.file.empty()) { RunLog::Line("comms-selftest: RESULT editorui - GWO_COMMS_SELFTEST_FILE not set"); c.stDone = true; return; }
            CommsAttachFile(cfg.file, true, cfg.offset);
            c.stParty1 = m_showTeam1Party;
            c.stParty2 = m_showTeam2Party;
            m_showTeam1Party = m_showTeam2Party = false;
            m_debugTimeline = std::clamp(m_displayTimeOffset + 152.f, 0.f, m_replayCtx.maxReplayTime);
            place(1920, 1080);
            c.stAt = now;
            c.stStage = 70;
            return;
        }
        if (cfg.mode == "playbar")
        {
            if (c.phase != Phase::None) ForgetComms("cleared by the self-test");
            // The party windows sit over the bar's ends at 1280: out of the way for the pictures
            // (given back at the end).
            c.stParty1 = m_showTeam1Party;
            c.stParty2 = m_showTeam2Party;
            m_showTeam1Party = m_showTeam2Party = false;
            place(1280, 720);
            c.stAt = now;
            c.stStage = 40;
            return;
        }
        if (cfg.mode == "file")
        {
            if (cfg.file.empty()) { RunLog::Line("comms-selftest: RESULT file - GWO_COMMS_SELFTEST_FILE not set"); c.stDone = true; return; }
            CommsAttachFile(cfg.file, true, cfg.offset);
            c.stStage = 20;
            return;
        }
        {
            auto job = std::make_shared<Comms::SynthJob>();
            const bool withGame = cfg.mode != "nogame";
            job->path = cfg.dir / (folder + (withGame ? ".comms_test.wav" : ".comms_nogame.wav"));
            Comms::GameModel model;
            BuildCommsGameModel(model);
            // The recording player: the first of the first team.
            int listener = 0;
            if (!m_team1PlayerIds.empty())
                for (size_t i = 0; i < model.listeners.size(); ++i)
                    if (model.listeners[i].agentId == m_team1PlayerIds.front()) listener = static_cast<int>(i);
            c.stExpected = cfg.offset;
            const uint32_t seed = static_cast<uint32_t>(std::hash<std::string>{}(folder));
            c.synth = job;
            const bool hard = cfg.mode == "hard";
            c.synthThread = std::thread([job, model = std::move(model), listener, offset = cfg.offset, withGame, hard, seed] {
                Comms::Synthesise(model, listener, offset, withGame, hard, seed, *job);
            });
            RunLog::Line("comms-selftest: '%s' mode %s, synthesising with offset %+.3f s (listener %d)",
                         folder.c_str(), cfg.mode.c_str(), cfg.offset, listener);
            c.stStage = 1;
        }
        return;
    case 1:
        if (!c.synth || !c.synth->done.load(std::memory_order_acquire)) return;
        if (c.synthThread.joinable()) c.synthThread.join();
        RunLog::Line("comms-selftest: wrote '%s' - %s", c.synth->path.string().c_str(), c.synth->info.c_str());
        if (!c.synth->ok) { c.stDone = true; return; }
        CommsAttachFile(c.synth->path.wstring(), false, 0.0);
        c.stAt = now;
        c.stStage = 2;
        return;
    case 2:
        if (c.phase == Phase::Failed) { RunLog::Line("comms-selftest: RESULT load failed"); c.stDone = true; return; }
        if (!c.hasSync) { if (now - c.stAt > 300.0) { RunLog::Line("comms-selftest: RESULT timed out"); c.stDone = true; } return; }
        {
            const auto& r = c.lastSync;
            const bool expectSync = cfg.mode != "nogame";
            RunLog::Line("comms-selftest: RESULT '%s' mode %s expected %+.3f found %+.3f error %+.3f s z %.1f ratio %.2f confidence %.2f %s listener %d (sync %.2f s)%s",
                         folder.c_str(), cfg.mode.c_str(), expectSync ? c.stExpected : 0.0, r.offset,
                         expectSync ? r.offset - c.stExpected : 0.0, r.z, r.ratio, r.confidence,
                         r.confident ? "CONFIDENT" : "low", r.listener, r.seconds,
                         expectSync ? (r.confident && std::fabs(r.offset - c.stExpected) < 0.1 ? " PASS" : " FAIL")
                                    : (r.confident ? " FAIL (confident on no game sound)" : " PASS"));
        }
        // Playback: 1x, 2x, then 4x (silent).
        if (cfg.mode == "nogame") c.offset = c.stExpected;
        m_debugTimeline = std::clamp(static_cast<float>(std::max(60.0, -c.offset + 60.0)), 0.f, m_replayCtx.maxReplayTime);
        speedTo(3);
        m_replayCtx.isPlaying = true;
        driftReset();
        c.stAt = now;
        c.stStage = 3;
        return;
    case 3:
        if (now - c.stAt < 4.0) return;
        driftLine("1x");
        speedTo(5);
        driftReset();
        c.stAt = now;
        c.stStage = 4;
        return;
    case 4:
        if (now - c.stAt < 4.0) return;
        driftLine("2x");
        speedTo(6);
        driftReset();
        c.stAt = now;
        c.stStage = 5;
        return;
    case 5:
        if (now - c.stAt < 1.0) return;
        driftLine("4x (expected stopped)");
        // A scrub while playing at 1x: the voice must follow at once.
        speedTo(3);
        driftReset();
        m_debugTimeline = std::min(m_replayCtx.maxReplayTime - 5.f, m_debugTimeline + 120.f);
        c.stAt = now;
        c.stStage = 6;
        return;
    case 6:
        if (now - c.stAt < 2.0) return;
        if (cfg.mode != "file") driftLine("1x after a 120 s jump");
        m_replayCtx.isPlaying = false;
        {
            // Off every monitor, visible, unfocused (as the clip self-test does): a real frame to take.
            RECT r = {0, 0, 1280, 720};
            AdjustWindowRect(&r, GetWindowLong(m_hwnd, GWL_STYLE), FALSE);
            SetWindowPos(m_hwnd, HWND_BOTTOM, 20000, 20000, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        }
        // The Sync editor as the player sees it, fitted.
        c.syncWin = true;
        c.viewSet = false;
        c.stAt = now;
        c.stStage = 7;
        return;
    case 7:   // the editor
        if (now - c.stAt < 2.5) return;
        c.shotPath = cfg.dir / (folder + ".comms_editor.png");
        c.shotPending = true;
        c.stAt = now;
        c.stStage = 8;
        return;
    case 8:   // ...then a drag of its clip, driven by code, ending 5 px short of the playhead
        if (c.shotPending && now - c.stAt < 10.0) return;
        c.stSavedOffset = c.offset;
        c.simTarget = m_debugTimeline;
        c.simMiss = -5.f;
        c.simDrag = true;
        c.stAt = now;
        c.stStage = 9;
        return;
    case 9:
        if (now - c.stAt < 0.6) return;
        RunLog::Line("comms-selftest: drag - clip start pulled to %.3f s (5 px short), offset %+.3f s (start %.3f), snapped to '%s'",
                     c.simTarget, c.offset, -c.offset, c.snapName.c_str());
        c.shotPath = cfg.dir / (folder + ".comms_drag.png");
        c.shotPending = true;
        c.stAt = now;
        c.stStage = 12;
        return;
    case 12:   // the drag ends, the offset is put back; zoomed in on 12 s around the playhead
        if (c.shotPending && now - c.stAt < 10.0) return;
        c.simDrag = false;
        c.offset = c.stSavedOffset;
        c.dirty = true;
        c.view0 = m_debugTimeline - 6.0;
        c.view1 = m_debugTimeline + 6.0;
        // ...mid-scrub: pulled 3 px past a whole second, where the sticky playhead must land on it.
        c.simScrubT = m_displayTimeOffset + std::round(m_debugTimeline - m_displayTimeOffset) + 2.0;
        c.simMiss = 3.f;
        c.simScrub = true;
        c.stAt = now;
        c.stStage = 15;
        return;
    case 15:
        if (now - c.stAt < 1.0) return;
        c.shotPath = cfg.dir / (folder + ".comms_zoom.png");
        c.shotPending = true;
        c.stAt = now;
        c.stStage = 16;
        return;
    case 16:   // the editor closes: the lane alone, wider (at 1280 the party windows cover most of it)
        if (c.shotPending && now - c.stAt < 10.0) return;
        RunLog::Line("comms-selftest: scrub - pulled to %.3f s + 3 px, playhead at %.3f s (match %s), %s",
                     c.simScrubT, m_debugTimeline, ClockTenths(m_debugTimeline - m_displayTimeOffset).c_str(),
                     c.phSticky ? "SNAPPED" : "not snapped");
        c.simScrub = false;
        c.syncWin = false;
        {
            RECT r = {0, 0, 1920, 1080};
            AdjustWindowRect(&r, GetWindowLong(m_hwnd, GWL_STYLE), FALSE);
            SetWindowPos(m_hwnd, HWND_BOTTOM, 20000, 20000, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
        }
        c.stAt = now;
        c.stStage = 13;
        return;
    case 13:
        if (now - c.stAt < 1.5) return;
        c.shotPath = cfg.dir / (folder + ".comms_lane.png");
        c.shotPending = true;
        c.stAt = now;
        c.stStage = 14;
        return;
    case 14:
        if (c.shotPending && now - c.stAt < 10.0) return;
        c.stDone = true;
        return;
    case 10:   // reattach: what came back for this match
        if (c.phase == Phase::Loading) return;
        RunLog::Line("comms-selftest: RESULT reattach '%s' - %s, file '%s', offset %+.3f s, volume %.2f, muted %d",
                     folder.c_str(),
                     c.phase == Phase::Ready ? "REATTACHED" : c.phase == Phase::Missing ? "NOT FOUND" :
                     c.phase == Phase::Failed ? "FAILED" : "nothing remembered",
                     c.name.c_str(), c.offset, c.volume, c.muted ? 1 : 0);
        c.stStage = 11;
        return;
    case 11:   // ...and a picture of the lane as it came back
    {
        RECT r = {0, 0, 1920, 1080};
        AdjustWindowRect(&r, GetWindowLong(m_hwnd, GWL_STYLE), FALSE);
        SetWindowPos(m_hwnd, HWND_BOTTOM, 20000, 20000, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        c.stAt = now;
        c.stStage = 13;
        return;
    }
    case 20:   // file: a real recording (GWO_COMMS_SELFTEST_FILE) at GWO_COMMS_SELFTEST_OFFSET, no sync
        if (c.phase == Phase::Loading) return;
        if (c.phase != Phase::Ready) { RunLog::Line("comms-selftest: RESULT file FAILED to load"); c.stDone = true; return; }
        RunLog::Line("comms-selftest: RESULT file '%s' loaded - %.1f min, offset %+.3f s, display ref %.0f, rms floor %.0f, rms median %.0f",
                     c.name.c_str(), c.track->Duration() / 60.0, c.offset, c.track->displayRef, c.track->rmsFloor, c.track->rmsMid);
        m_debugTimeline = std::clamp(m_displayTimeOffset + 150.f, 0.f, m_replayCtx.maxReplayTime);
        c.stAt = now - 2.0;
        c.stStage = 6;
        return;
    default:
        return;
    }
}
