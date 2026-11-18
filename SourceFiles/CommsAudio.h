#pragma once

// =================================================================================================
// COMMS AUDIO - a team's voice recording played beside its replay (ReplayWindow_Comms.cpp).
//
// The player drops an audio or video file (an OBS recording, a Discord recording) onto an open
// replay. This module is the part with no replay in it:
//
//   * LOADING. Media Foundation's Source Reader decodes the file's first audio stream to mono
//     16-bit PCM at kTrackRate (its own resampler and down-mixer), on a worker thread. Beside the
//     PCM it keeps the waveform's min/max peaks and the file's onset envelope, both at
//     kEnvelopeRate per second.
//   * SYNC. Most recordings carry the game's own sounds. The match's sound events (casts, attacks,
//     hits, deaths) make an onset envelope of the match per candidate listener (everything, and
//     each player with the sounds near him louder); the file's envelope is spectral flux in the
//     high bands, where voices weigh least. An FFT cross-correlation over every offset with enough
//     overlap finds the best lag; the peak against the noise of all the others is the confidence.
//   * PLAYBACK. One XAudio2 source voice on an engine of its own, started at a sample position and
//     re-seeked by the caller when it drifts.
//   * MEMORY. Per match, on this PC only: the file's path, size and write time, the offset, the
//     volume and the mute, in a small JSON file under settings/.
//
// Nothing here opens a network connection: the file never leaves the PC.
// =================================================================================================

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace Comms
{
constexpr int kEnvelopeRate = 100;       // peaks and onset values per second
constexpr uint32_t kTrackRate = 32000;   // the decoded PCM's rate (mono)

// A decoded file.
struct Track
{
    uint32_t sampleRate = 0;
    std::vector<int16_t> pcm;                 // mono
    std::vector<int16_t> peakMin, peakMax;    // kEnvelopeRate per second
    std::vector<uint16_t> rms;                // kEnvelopeRate per second: the waveform's inner band
    std::vector<float> onset;                 // kEnvelopeRate per second, normalised
    int16_t peakAbs = 1;                      // the loudest sample, for drawing
    // The level a waveform is drawn against: the 99th percentile of the peaks, so one click or a
    // clipped shout does not flatten everything else (WaveHeight).
    float displayRef = 1.f;
    // The 10th percentile and the median of the RMS table, for the log.
    float rmsFloor = 0.f;
    float rmsMid = 0.f;
    double Duration() const { return sampleRate ? static_cast<double>(pcm.size()) / sampleRate : 0.0; }
};

// One load, run on a worker thread (RunLoad), polled by the window.
struct LoadJob
{
    std::wstring path;
    std::atomic<bool> cancel{false};
    std::atomic<float> progress{0.f};   // 0..1
    std::atomic<bool> done{false};
    bool ok = false;
    std::string error;    // for the player
    std::string detail;   // for the log
    std::shared_ptr<Track> track;
    double seconds = 0.0;
};
void RunLoad(LoadJob& job);

// How tall to draw a level of `v` (0..1 of the half-height), as an audio editor does: linear
// amplitude against the track's display level, which sits at 85 %, with a light curve (power 0.8)
// so quiet talk still shows; clamped. Decibels made every music file a solid slab.
inline float WaveHeight(float v, float ref)
{
    if (v <= 0.f) return 0.f;
    const float h = 0.85f * std::pow(v / (ref > 1.f ? ref : 1.f), 0.8f);
    return h > 1.f ? 1.f : h;
}

// The match's sound events and its possible listeners.
struct GameEvent
{
    float time = 0.f;
    float weight = 1.f;
    float x = 0.f, y = 0.f;
    bool hasPos = false;
    int a = 0, b = 0;   // the agents involved (caster / target): a listener's own sounds are louder;
                        // a == -1: heard by every player at full weight
};
struct Listener
{
    int agentId = 0;
    float rate = 4.f;            // positions per second, from t = 0
    std::vector<float> x, y;
};
struct GameModel
{
    float duration = 0.f;
    std::vector<GameEvent> events;
    std::vector<Listener> listeners;
};

// The match's onset envelope heard from listener `listener` (-1 = every sound at full weight),
// normalised the way the file's is. kEnvelopeRate values per second.
std::vector<float> BuildGameEnvelope(const GameModel& model, int listener);
// The same envelope (every sound, full weight) shaped for drawing: positive, smoothed over 0.15 s,
// its 99th percentile at 1. What the Sync editor's Match track shows to line the comms up against.
std::vector<float> BuildGameActivity(const GameModel& model);

struct SyncResult
{
    bool ok = false;
    double offset = 0.0;       // file position = match time + offset
    double z = 0.0;            // the peak above the correlation's noise, in robust sigmas
    double ratio = 0.0;        // the peak's z over the next best peak's (more than 1 s away)
    double confidence = 0.0;   // 0..1
    bool confident = false;
    int listener = -1;         // which candidate won (-1 = everything)
    double seconds = 0.0;
    std::string why;           // when !ok
};
struct SyncJob
{
    GameModel model;
    std::shared_ptr<const Track> track;
    std::atomic<bool> cancel{false};
    std::atomic<float> progress{0.f};
    std::atomic<bool> done{false};
    SyncResult result;
};
void RunSync(SyncJob& job);

// The thresholds a sync must pass to be applied without asking (calibrated on the self-test).
constexpr double kConfidentZ = 9.0;
constexpr double kConfidentRatio = 1.25;
// ...or a lower peak far clear of the next one. Files with no game sound in them measured z 5.9-7.1
// and ratio 1.01-1.21 (self-test, 2026-11-18).
constexpr double kConfidentZClear = 7.5;
constexpr double kConfidentRatioClear = 1.5;

// One voice, its own XAudio2 engine.
class Player
{
public:
    Player() = default;
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Starts `track` at `startSample`. False when there is no audio device.
    bool Start(const std::shared_ptr<const Track>& track, size_t startSample, float ratio, float volume);
    void Stop();
    bool Active() const { return m_voice != nullptr; }
    // Where the voice is in the file, in seconds (valid while Active()).
    double Position() const;
    // True once the voice has played to the end of the file.
    bool Finished() const;
    void SetRatio(float ratio);
    void SetVolume(float volume);
    void Shutdown();

private:
    bool EnsureEngine();
    IXAudio2* m_xaudio = nullptr;
    IXAudio2MasteringVoice* m_master = nullptr;
    IXAudio2SourceVoice* m_voice = nullptr;
    std::shared_ptr<const Track> m_track;
    size_t m_start = 0;
    float m_ratio = 1.f, m_volume = 1.f;
    bool m_engineFailed = false;
};

// ---- What a match remembers -----------------------------------------------------------------
struct Saved
{
    std::wstring path;
    uint64_t size = 0;
    int64_t writeTime = 0;
    double offset = 0.0;
    float volume = 1.f;
    bool muted = false;
};
bool LoadSaved(const std::filesystem::path& store, const std::string& folder, Saved& out);
void SaveEntry(const std::filesystem::path& store, const std::string& folder, const Saved& entry);
void ForgetEntry(const std::filesystem::path& store, const std::string& folder);
// The store this instance uses (settings/comms_audio.json; a quiet test instance's own copy).
// Defined in ReplayWindow_Comms.cpp.
std::filesystem::path DefaultStorePath();

// What the Library shows for a match: a recording remembered for it, and whether that file is
// still on disk. Cached: the store is re-read when SaveEntry / ForgetEntry change it, when its
// stamp changes (checked every 2 s), and at least every 30 s; never per row or per frame.
struct Badge
{
    bool present = false;
    bool exists = false;
    std::string name;   // the file's name
    std::string path;   // UTF-8
};
Badge BadgeFor(const std::string& folder);
uint64_t BadgeVersion();   // changes whenever the cache was reloaded
void InvalidateBadges();

// The file's size and last write time; false when it does not exist.
bool FileStamp(const std::wstring& path, uint64_t& size, int64_t& writeTime);

std::string ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);
std::string FormatOffset(double seconds);   // "+12.4 s"
std::string ClockText(double seconds);      // "7:50", "1:02:03", "-0:05"

// ---- Self-test ------------------------------------------------------------------------------
// Writes a 16-bit PCM wav (interleaved when channels > 1), streamed from float samples in -1..1.
bool WriteWavMonoAsStereo(const std::filesystem::path& path, const std::vector<float>& mono, uint32_t rate);
} // namespace Comms
