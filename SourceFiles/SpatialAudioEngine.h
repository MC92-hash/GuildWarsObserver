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
class SkillSoundTable;
class DATManager;

enum class SoundCategory : uint8_t {
    SKILL_CAST,
    FOOTSTEP,   // reserved
    HIT,        // reserved
    DEATH       // reserved
};

struct SoundEvent {
    SoundCategory category = SoundCategory::SKILL_CAST;
    uint32_t      skill_id = 0;
    int           source_agent_id = -1;
    float         volume = 1.0f;
    float         pitch  = 0.0f;  // semitone offset (unused for now)
};

struct AudioConfig {
    float master_volume          = 1.0f;
    float sfx_volume             = 1.0f;
    float curve_distance_scaler  = 14.0f;
    float max_audible_radius     = 2500.0f;
};

class SpatialAudioEngine {
public:
    SpatialAudioEngine();
    ~SpatialAudioEngine();

    // Noncopyable
    SpatialAudioEngine(const SpatialAudioEngine&) = delete;
    SpatialAudioEngine& operator=(const SpatialAudioEngine&) = delete;

    bool Init(DATManager* datMgr,
              const std::unordered_map<int, std::vector<int>>* hashIndex,
              const std::string& skillSoundsJsonPath);
    void Shutdown();

    // Called every frame from the main loop
    void UpdateListener(const DirectX::XMFLOAT3& pos,
                        const DirectX::XMFLOAT3& front,
                        const DirectX::XMFLOAT3& up,
                        float deltaTime);

    // Fire-and-forget: post a sound event.
    // agentX/Y/Z: world position of the emitter at the time of posting.
    void Post(const SoundEvent& evt, float agentX, float agentY, float agentZ);

    // Stop all playing sounds
    void StopAll();

    AudioConfig& GetConfig() { return m_config; }
    const AudioConfig& GetConfig() const { return m_config; }

    bool IsInitialized() const { return m_initialized; }

    // Debug statistics
    struct DebugStats {
        int eventsPosted    = 0;
        int soundsPlayed    = 0;
        int cacheHits       = 0;
        int cacheMisses     = 0;
        int hashNotFound    = 0;
        int notSoundType    = 0;
        int decodeFailures  = 0;
        int voicesActive    = 0;
        int skillTableSize  = 0;
        int hashIndexSize   = 0;
        uint32_t lastSkillId = 0;
        std::string lastSkillName;
        float panLeft  = 0.f;
        float panRight = 0.f;
    };
    DebugStats GetDebugStats() const;
    const SoundCache* GetSoundCache() const { return m_soundCache.get(); }

private:
    static constexpr int VOICE_POOL_SIZE = 32;

    struct VoiceSlot {
        IXAudio2SourceVoice* voice = nullptr;
        bool                  inUse = false;
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
    bool DecodeMp3ToWav(const uint8_t* mp3Data, int mp3Size,
                        std::vector<uint8_t>& pcmOut,
                        uint32_t& sampleRate, uint16_t& channels);

    IXAudio2*              m_xaudio = nullptr;
    IXAudio2MasteringVoice* m_masterVoice = nullptr;
    X3DAUDIO_HANDLE        m_x3d{};
    X3DAUDIO_LISTENER      m_listener{};
    DWORD                  m_channelMask = 0;
    UINT32                 m_masterChannels = 0;

    std::array<VoiceSlot, VOICE_POOL_SIZE> m_voicePool;
    VoiceCallback          m_callback;
    std::mutex             m_voiceMutex;

    std::unique_ptr<SoundCache>      m_soundCache;
    std::unique_ptr<SkillSoundTable> m_skillTable;

    AudioConfig m_config;
    bool        m_initialized = false;
    DirectX::XMFLOAT3 m_lastListenerPos{0,0,0};
    bool        m_hasLastListenerPos = false;

    X3DAUDIO_DISTANCE_CURVE_POINT m_curvePoints[6];
    X3DAUDIO_DISTANCE_CURVE       m_distanceCurve;

    // Track which skill_ids had no entry, to warn only once
    std::unordered_set<uint32_t> m_warnedSkillIds;

    mutable DebugStats m_debugStats;
};
