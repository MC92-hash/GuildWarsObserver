#pragma once

#include <cstdint>
#include <array>
#include <vector>
#include <mutex>
#include <atomic>
#include <queue>
#include <thread>
#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <DirectXMath.h>
// XAudio2 requires _WIN32_WINNT >= 0x0602 (Windows 8)
#pragma push_macro("_WIN32_WINNT")
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <xaudio2.h>
#include <x3daudio.h>
#pragma pop_macro("_WIN32_WINNT")

struct AudioBuffer;
class SoundCache;
class DATManager;

// Category classification for a captured StoC/sound_events.txt event, mirroring the game's own
// CategorySettings.ini (extracted from Gw.dat) closely enough to reuse its per-category
// Volume/MaxSounds values. The capture side only records the coarse SND_TYPE (0-4), so anything
// finer is derived at playback time (see ReplayWindow_Audio.cpp's classification: weapon_type
// of the correlated cause_agent_id for attack-hit sounds, cause_skill_id presence for skill
// cues, and "Footstep" as the fallback for uncorrelated Effects-type sounds).
enum class SoundLogCategory : uint8_t {
    Background,   // SND_TYPE 0
    Ui,           // SND_TYPE 2
    Music,        // SND_TYPE 3
    Dialog,       // SND_TYPE 4
    HitBow,       // weapon_type 1
    HitAxe,       // weapon_type 2
    HitHammer,    // weapon_type 3
    HitDagger,    // weapon_type 4
    HitScythe,    // weapon_type 5
    HitSpear,     // weapon_type 6
    HitSword,     // weapon_type 7
    HitMagic,     // weapon_type 10/12/14 (wand/staff)
    HitMiss,      // no weapon_type match / attack sound with no discernible weapon
    // Skill audio, split the way CategorySettings.ini splits it rather than lumped into one
    // bucket. A single cast layers several of these at once, and the game gives each layer its
    // own volume and its own concurrency budget - which is what keeps a busy fight from turning
    // into mush. SkillWarmup in particular is 10x quieter than the rest ([wrmups] Volume=-2000).
    SkillWarmup,  // [wrmups] the channeling/wind-up layer at the start of a cast
    SkillEffect,  // [skills] the primary skill sound
    SkillExtra,   // [skillx] secondary layer stacked on the primary
    SkillWoosh,   // [woosh]  motion/whoosh layer
    Footstep,     // Effects-type sound with no correlation (footsteps/ambient loops)
    // Categories a recorded sound PROGRAM names itself (its BUS), which the old SND_TYPE-only
    // classification had no slot for. The private replay sound schedule maps each bus onto one of
    // these and carries the exact CategorySettings.ini level in the per-sound gain.
    Vocal,        // [plyvoc]/[monvoc] the characters' own grunts and shouts (0 mB in the world)
    AttackWoosh,  // [woosh]/[pwoosh] a weapon's swing, a bow's release, an arrow going past
    AttackOther,  // [effects]/[default]/[comfol] combat foley a program files under no hit category
    Prop,         // [smprop]/[mdprop]/[lgprop]/[ambone] the map's props and one-shot ambiences
    EventCue,     // [ui] played in the world (MODE3D): the interrupt / cancel cue and its kind
    Count
};

// True for every category driven by skill_sounds.json rather than StoC.
constexpr bool IsSkillCategory(SoundLogCategory c)
{
    return c == SoundLogCategory::SkillWarmup || c == SoundLogCategory::SkillEffect ||
           c == SoundLogCategory::SkillExtra  || c == SoundLogCategory::SkillWoosh;
}

struct AudioConfig {
    // The master slider is not a straight multiplier - it is shaped by kMasterCurveExponent in
    // SpatialAudioEngine.cpp so that 60% on the slider yields 30% gain, putting the useful range
    // where it is actually adjusted instead of bunching everything up near the top.
    float master_volume          = 0.7f;
    // Per-SND_TYPE multipliers (user-adjustable, mirrors the game's own Master/Background/
    // Effects/UI/Music/Dialog volume sliders). These stay linear - they are applied on top of
    // the fixed per-category gain CategorySettings.ini bakes in (see GetCategoryProfile()).
    // Levels are given as the position their slider reads, converted through the panel's
    // reference mapping - not as raw gains. Background reads 55% against a 0.01 midpoint.
    float background_volume      = 0.0158f;
    // "Effects" is split into three faders rather than one. Auto-attacks fire an order of
    // magnitude more often than skills do, so at any shared setting they bury the casts - and no
    // single slider can fix that, it needs independent balance. Attacks default lower for that
    // reason; this is a deliberate departure from the game's own mix, because a replay is watched
    // to read what people cast, not to feel like standing in the middle of the fight.
    float skills_volume          = 0.95f;  // Skill* categories
    // Raised from 0.02 when the recorded attack sounds started to play (2026-10-17): before, the
    // recording's weapon swings and hits were sound programs the replay could not read, so the
    // fader was tuned on the few MP3 children that slipped through.
    // 0.30 -> 0.60 (2026-10-17, owner: "no attack sounds"): measured on Warrior's Isle 07.18, 4:30-5:30,
    // followed agent, hits started at a mean level of 0.009 against 0.05 for skills (-15 dB).
    float attacks_volume         = 0.60f;  // Hit* / AttackWoosh / AttackOther categories
    float ambient_volume         = 0.30f;  // footsteps / uncorrelated effects / map props
    // The characters' vocals (attack grunts, cast shouts, pain): the client plays them at full
    // category volume in the world, and they fire with nearly every attack - their own fader.
    float voices_volume          = 0.50f;
    float ui_volume              = 0.02f;
    float music_volume           = 0.0956f;  // reads 70% against a 0.02 midpoint
    float dialog_volume          = 0.00f;
    // Multiplies every category's audible radius, so the whole soundscape can be opened up or
    // tightened by ear without re-tuning each category. 1.0 = the tuned per-category radii.
    float hearing_distance_scale = 1.20f;

    // Weight applied to the vertical component of emitter-to-listener distance.
    //
    // A replay is watched from above, so camera height dominates the 3D distance: from 2500 units
    // up, an agent 300 units away horizontally - directly below the camera - computes as ~2500
    // units away and lands at the silent end of the falloff curve. Everything ends up equidistant
    // and quiet at once, which both crushes the levels and destroys the spatial separation that
    // makes a fight readable. Compressing the vertical axis restores the horizontal geometry the
    // viewer is actually looking at. 1.0 = true 3D distance.
    float vertical_distance_scale = 0.05f;

    // --- Distance falloff shape -------------------------------------------------------------
    // The curve used to be nine hand-placed points that followed no particular law, which made it
    // impossible to say whether it was linear, logarithmic or something else - and impossible to
    // adjust coherently. It is now generated from the inverse distance law games and DirectSound3D
    // both use, so it has a shape that can be reasoned about and only two things to tune.

    // Fraction of the audible radius that stays at full volume. This is the "reference distance"
    // of the standard model: without it the curve starts falling the instant a sound leaves the
    // listener, which is what made close range feel abrupt.
    float near_field_fraction    = 0.12f;

    // How sharply it falls beyond the near field. 1.0 is the physical inverse distance law
    // (-6 dB per doubling); lower carries further, higher drops away faster.
    float distance_rolloff       = 0.66f;
    // Debug: mirror the stereo image. Present because left/right can end up inverted through the
    // listener basis, and hearing it is the only practical way to confirm which way is correct.
    bool  swap_stereo            = false;
};

class SpatialAudioEngine {
public:
    SpatialAudioEngine();
    ~SpatialAudioEngine();

    // Noncopyable
    SpatialAudioEngine(const SpatialAudioEngine&) = delete;
    SpatialAudioEngine& operator=(const SpatialAudioEngine&) = delete;

    bool Init(DATManager* datMgr, const std::unordered_map<int, std::vector<int>>* hashIndex);
    void Shutdown();

    // Called every frame from the main loop
    void UpdateListener(const DirectX::XMFLOAT3& pos,
                        const DirectX::XMFLOAT3& front,
                        const DirectX::XMFLOAT3& up,
                        float deltaTime);

    // Fire-and-forget: play an exact File ID (from a captured StoC/sound_events.txt entry) at a
    // known world position. `category` selects the CategorySettings.ini-derived base volume and
    // concurrent-voice cap (mirrors the real client's per-category polyphony throttling - if the
    // category is already at its cap, this call is dropped rather than voiced, same as the game
    // itself would do in a crowded moment). When positional is false, x/y/z are ignored and the
    // sound plays flat/centered (music, dialog, UI - anything the recorder captured non-spatial).
    // gainScale lets the caller attenuate one instance without touching the category - used to
    // duck the stacked simultaneous layers of a single cast so the cast reads as one sound.
    // focusedPlayer selects CategorySettings.ini's "p" column: the louder, separately-budgeted
    // variant the game uses for sounds the local player makes or receives. Set it for the agent
    // the camera is following.
    // pitch is a playback-rate multiplier (1 = as stored; XAudio2 allows up to 2 here).
    void PostFileId(uint32_t fileId, SoundLogCategory category, float x, float y, float z,
                    bool positional, float gainScale = 1.0f, bool focusedPlayer = false,
                    float pitch = 1.0f);

    // Debug: play a sound at a fixed distance directly to the listener's left/right/front/back,
    // derived from the listener basis actually in use. Lets the stereo image be verified by ear
    // instead of by reading matrix maths. Reuses the last sound that played so the test is never
    // silent because of a missing file id.
    enum class TestDirection { Left, Right, Front, Back };
    void PlayDirectionalTest(TestDirection dir);

    // Gain at a normalised distance: 0 = at the emitter, 1 = at the category's audible radius.
    // Single source of truth for the falloff - the X3DAudio curve is sampled from this and the
    // Sound FX panel plots it, so the drawn curve is exactly the one being heard.
    float DistanceGain(float normalizedDistance) const;

    // Stop all playing sounds
    void StopAll();

    // Pause/resume all currently-playing voices in place (XAudio2 has no concept of the
    // replay's own play/pause state, so already-triggered sounds would otherwise keep playing
    // straight through a pause). Unlike StopAll(), this doesn't discard queued buffers - a
    // paused voice resumes from where it left off, not from the start.
    void SetPaused(bool paused);

    AudioConfig& GetConfig() { return m_config; }
    const AudioConfig& GetConfig() const { return m_config; }

    bool IsInitialized() const { return m_initialized; }

    // Debug statistics
    struct DebugStats {
        int eventsPosted    = 0;
        int soundsPlayed    = 0;
        int droppedAtCap    = 0;   // events skipped because their category was already at MaxSounds
        int culledByDistance = 0;  // events skipped for being beyond the category's audible radius
        int cacheHits       = 0;
        int cacheMisses     = 0;
        int hashNotFound    = 0;
        int notSoundType    = 0;
        int decodeFailures  = 0;
        int voicesActive    = 0;
        int hashIndexSize   = 0;
        float panLeft  = 0.f;
        float panRight = 0.f;
    };
    DebugStats GetDebugStats() const;

    // Per category, since the last reset: what was posted and what became of it. `estimatedGain`
    // is the level a started sound begins at: category x fader x per-sound gain x master x the
    // distance curve x the rear floor (the same factors UpdateListener applies).
    struct CategoryStats {
        int posted = 0, culled = 0, capped = 0, unreadable = 0, started = 0, audible = 0;
        double gainSum = 0.0;
        float gainMax = 0.f;
    };
    const CategoryStats& GetCategoryStats(SoundLogCategory c) const { return m_categoryStats[static_cast<size_t>(c)]; }
    void ResetCategoryStats() { for (auto& s : m_categoryStats) s = CategoryStats{}; }
    // Silences the output without touching the levels (a hidden verification run).
    void SetOutputMuted(bool muted) { m_outputMuted = muted; }
    const SoundCache* GetSoundCache() const { return m_soundCache.get(); }

private:
    // Sized above the sum of the busy categories' caps: when the pool runs dry AcquireVoice
    // steals the oldest slot mid-playback, which is audible as a clipped sound.
    static constexpr int VOICE_POOL_SIZE = 96;

    struct VoiceSlot {
        IXAudio2SourceVoice* voice = nullptr;
        bool                  inUse = false;
        bool                  positional = true; // false => flat/centered, skip 3D emitter math
        SoundLogCategory       category = SoundLogCategory::Footstep;
        bool                  focusedPlayer = false; // uses the "p" column and its own voice budget
        float                 pitch = 1.0f;          // playback rate, kept under the Doppler factor
        float                 startGain = 0.0f;      // the estimated level it started at (CategoryStats), for stealing
        float                 baseGainLinear = 1.0f; // CategorySettings.ini gain; combined with the
                                                        // live per-type slider every frame (see UpdateListener)
        float                 emitterX = 0, emitterY = 0, emitterZ = 0;
        X3DAUDIO_EMITTER      emitter{};
        X3DAUDIO_DSP_SETTINGS dspSettings{};
        float                 matrixCoeffs[8]{}; // up to 7.1
        std::vector<uint8_t>  pcmOwned;          // keep PCM alive while playing
    };

    // Voice callback to detect end of buffer
    class VoiceCallback : public IXAudio2VoiceCallback {
    public:
        VoiceCallback() = default;
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
        void STDMETHODCALLTYPE OnBufferEnd(void* ctx) override;
        void STDMETHODCALLTYPE OnBufferStart(void*) override {}
        void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}

        SpatialAudioEngine* engine = nullptr;
    };

    VoiceSlot* AcquireVoice();
    void ReleaseVoice(VoiceSlot* slot);
    void SubmitBuffer(VoiceSlot* slot, const AudioBuffer* buf, float volume);
    // MP3s decoded once and kept (the recorded sound programs replay the same few hundred files
    // thousands of times a match; decoding at every play stalled the frame). Keyed by the cache's
    // buffer, which lives as long as the SoundCache; bounded by kDecodedBudgetBytes.
    struct DecodedPcm {
        std::vector<uint8_t> pcm;
        uint32_t sampleRate = 0;
        uint16_t channels = 1;
    };
    std::unordered_map<const AudioBuffer*, DecodedPcm> m_decoded;
    size_t m_decodedBytes = 0;
    static constexpr size_t kDecodedBudgetBytes = 128u * 1024u * 1024u;
    bool DecodeMp3ToWav(const uint8_t* mp3Data, int mp3Size,
                        std::vector<uint8_t>& pcmOut,
                        uint32_t& sampleRate, uint16_t& channels);
    // Counted per perspective: the player variant has its own (small) cap in the ini, so its
    // voices must not be pooled with the world's or the crowd would starve the followed agent.
    int CountActiveVoicesInCategory(SoundLogCategory category, bool focusedPlayer) const;

    IXAudio2*              m_xaudio = nullptr;
    IXAudio2MasteringVoice* m_masterVoice = nullptr;
    X3DAUDIO_HANDLE        m_x3d{};
    X3DAUDIO_LISTENER      m_listener{};
    DWORD                  m_channelMask = 0;
    UINT32                 m_masterChannels = 0;

    std::array<VoiceSlot, VOICE_POOL_SIZE> m_voicePool;
    VoiceCallback          m_callback;
    std::mutex             m_voiceMutex;

    std::unique_ptr<SoundCache> m_soundCache;

    // Per-file-id linear gain overrides from settings/sound_overrides.json. Individual samples
    // that are fine once but grate when a profession retriggers them every cast get tamed here
    // rather than by dragging a whole category down. A gain of 0 drops the sound entirely.
    void LoadSoundOverrides();
    std::unordered_map<uint32_t, float> m_soundOverrides;

    AudioConfig m_config;
    bool        m_initialized = false;
    uint32_t    m_lastPlayedFileId = 0;   // fallback source for PlayDirectionalTest
    DirectX::XMFLOAT3 m_lastListenerPos{0,0,0};
    bool        m_hasLastListenerPos = false;

    // Re-sample m_curvePoints from DistanceGain(). Cheap enough to run every frame, which keeps
    // the curve in step with the sliders with no dirty flag to get out of sync.
    void RebuildDistanceCurve();
    static constexpr int kCurvePointCount = 33;
    X3DAUDIO_DISTANCE_CURVE_POINT m_curvePoints[kCurvePointCount];
    X3DAUDIO_DISTANCE_CURVE       m_distanceCurve;

    mutable DebugStats m_debugStats;
    std::array<CategoryStats, static_cast<size_t>(SoundLogCategory::Count)> m_categoryStats{};
    bool m_outputMuted = false;
};
