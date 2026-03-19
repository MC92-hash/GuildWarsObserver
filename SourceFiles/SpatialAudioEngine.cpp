#include "pch.h"
#include "SpatialAudioEngine.h"
#include "SoundCache.h"
#include "SkillSoundTable.h"
#include "DATManager.h"

#include <cmath>
#include <algorithm>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <Shlwapi.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "Shlwapi.lib")

using namespace DirectX;

// ── Helpers ────────────────────────────────────────────────────────────────

static void ZeroEmitter(X3DAUDIO_EMITTER& e)
{
    memset(&e, 0, sizeof(e));
    e.ChannelCount = 1;
    e.CurveDistanceScaler = 1.0f;
    e.DopplerScaler = 0.0f;
    e.InnerRadius = 0.0f;
    e.InnerRadiusAngle = 0.0f;
}

// ── SpatialAudioEngine ────────────────────────────────────────────────────

SpatialAudioEngine::SpatialAudioEngine()
{
    m_callback.engine = this;
}

SpatialAudioEngine::~SpatialAudioEngine()
{
    Shutdown();
}

bool SpatialAudioEngine::Init(DATManager* datMgr,
                               const std::unordered_map<int, std::vector<int>>* hashIndex,
                               const std::string& skillSoundsJsonPath)
{
    if (m_initialized) return true;

    // Init Media Foundation (for MP3 decoding)
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        OutputDebugStringA("[SpatialAudio] MFStartup failed\n");
        return false;
    }

    // Create XAudio2
    hr = XAudio2Create(&m_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        OutputDebugStringA("[SpatialAudio] XAudio2Create failed\n");
        MFShutdown();
        return false;
    }

    hr = m_xaudio->CreateMasteringVoice(&m_masterVoice);
    if (FAILED(hr)) {
        OutputDebugStringA("[SpatialAudio] CreateMasteringVoice failed\n");
        m_xaudio->Release();
        m_xaudio = nullptr;
        MFShutdown();
        return false;
    }

    // Get channel mask for X3DAudio
    XAUDIO2_VOICE_DETAILS details;
    m_masterVoice->GetVoiceDetails(&details);
    m_masterChannels = details.InputChannels;

    m_masterVoice->GetChannelMask(&m_channelMask);
    X3DAudioInitialize(m_channelMask, X3DAUDIO_SPEED_OF_SOUND, m_x3d);

    // Setup listener defaults
    memset(&m_listener, 0, sizeof(m_listener));
    m_listener.OrientFront = { 0.f, 0.f, 1.f };
    m_listener.OrientTop   = { 0.f, 1.f, 0.f };

    // Smooth logarithmic-style distance curve — audible from far away, gentle falloff
    m_curvePoints[0] = { 0.00f, 1.00f };   // at emitter: full volume
    m_curvePoints[1] = { 0.05f, 0.90f };   // very near: barely reduced
    m_curvePoints[2] = { 0.15f, 0.60f };   // nearby: still prominent
    m_curvePoints[3] = { 0.35f, 0.30f };   // mid-range: noticeable
    m_curvePoints[4] = { 0.65f, 0.10f };   // far: ambient-level
    m_curvePoints[5] = { 1.00f, 0.00f };   // max range: silent
    m_distanceCurve.pPoints = m_curvePoints;
    m_distanceCurve.PointCount = 6;

    // Load skill sound table
    m_skillTable = std::make_unique<SkillSoundTable>();
    m_skillTable->Load(skillSoundsJsonPath);

    // Init sound cache with shared hash index (same one used for textures/models)
    m_soundCache = std::make_unique<SoundCache>();
    m_soundCache->Init(datMgr, hashIndex);

    m_initialized = true;
    OutputDebugStringA(std::format("[SpatialAudio] Initialized with {} output channels\n", m_masterChannels).c_str());
    return true;
}

void SpatialAudioEngine::Shutdown()
{
    if (!m_initialized) return;

    StopAll();

    // Destroy voices
    for (auto& slot : m_voicePool) {
        if (slot.voice) {
            slot.voice->DestroyVoice();
            slot.voice = nullptr;
        }
        slot.inUse = false;
    }

    if (m_masterVoice) {
        m_masterVoice->DestroyVoice();
        m_masterVoice = nullptr;
    }
    if (m_xaudio) {
        m_xaudio->Release();
        m_xaudio = nullptr;
    }

    m_soundCache.reset();
    m_skillTable.reset();
    m_initialized = false;

    MFShutdown();
}

void SpatialAudioEngine::UpdateListener(const XMFLOAT3& pos,
                                         const XMFLOAT3& front,
                                         const XMFLOAT3& up,
                                         float deltaTime)
{
    if (!m_initialized) return;

    // Compute listener velocity
    if (m_hasLastListenerPos && deltaTime > 0.0001f) {
        m_listener.Velocity.x = (pos.x - m_lastListenerPos.x) / deltaTime;
        m_listener.Velocity.y = (pos.y - m_lastListenerPos.y) / deltaTime;
        m_listener.Velocity.z = (pos.z - m_lastListenerPos.z) / deltaTime;
    } else {
        m_listener.Velocity = { 0, 0, 0 };
    }

    m_listener.Position    = { pos.x, pos.y, pos.z };
    m_listener.OrientFront = { front.x, front.y, front.z };
    m_listener.OrientTop   = { up.x, up.y, up.z };
    m_lastListenerPos = pos;
    m_hasLastListenerPos = true;

    // Apply master volume
    if (m_masterVoice)
        m_masterVoice->SetVolume(m_config.master_volume * m_config.sfx_volume);

    // Update 3D for all active voices
    std::lock_guard<std::mutex> lock(m_voiceMutex);
    for (auto& slot : m_voicePool) {
        if (!slot.inUse || !slot.voice) continue;

        slot.emitter.Position = { slot.emitterX, slot.emitterY, slot.emitterZ };
        slot.emitter.pVolumeCurve = &m_distanceCurve;
        slot.emitter.CurveDistanceScaler = m_config.curve_distance_scaler * m_config.max_audible_radius;

        slot.dspSettings.SrcChannelCount = 1;
        slot.dspSettings.DstChannelCount = m_masterChannels;
        slot.dspSettings.pMatrixCoefficients = slot.matrixCoeffs;

        X3DAudioCalculate(m_x3d, &m_listener, &slot.emitter,
            X3DAUDIO_CALCULATE_MATRIX | X3DAUDIO_CALCULATE_DOPPLER | X3DAUDIO_CALCULATE_LPF_DIRECT,
            &slot.dspSettings);

        // Front-hemisphere attenuation: mute sounds behind the camera,
        // smooth fade through a ~60-degree transition zone at the sides.
        float dx = slot.emitterX - m_listener.Position.x;
        float dz = slot.emitterZ - m_listener.Position.z;
        float dist2d = sqrtf(dx * dx + dz * dz);
        float dotFront = 0.f;
        if (dist2d > 0.01f)
            dotFront = (dx * m_listener.OrientFront.x + dz * m_listener.OrientFront.z) / dist2d;
        // dotFront: 1.0 = directly in front, 0.0 = 90 degrees, -1.0 = behind
        // Map to volume: full at >30deg forward, fade to 0 at >30deg behind
        float frontGain = std::clamp((dotFront + 0.5f) / 1.0f, 0.0f, 1.0f);
        frontGain *= frontGain; // quadratic fade for smoother transition

        for (UINT32 ch = 0; ch < m_masterChannels; ch++)
            slot.matrixCoeffs[ch] *= frontGain;

        slot.voice->SetOutputMatrix(m_masterVoice, 1, m_masterChannels, slot.matrixCoeffs);
        slot.voice->SetFrequencyRatio(slot.dspSettings.DopplerFactor);

        // Apply low-pass filter for sounds approaching the side/rear
        float lpfCoeff = slot.dspSettings.LPFDirectCoefficient;
        XAUDIO2_FILTER_PARAMETERS filterParams;
        filterParams.Type = LowPassFilter;
        filterParams.Frequency = 2.0f * sinf(X3DAUDIO_PI / 6.0f * lpfCoeff);
        filterParams.OneOverQ = 1.0f;
        slot.voice->SetFilterParameters(&filterParams);

        // Check if voice has finished playing
        XAUDIO2_VOICE_STATE state;
        slot.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0) {
            slot.voice->Stop();
            slot.inUse = false;
            slot.pcmOwned.clear();
        }
    }
}

void SpatialAudioEngine::Post(const SoundEvent& evt, float agentX, float agentY, float agentZ)
{
    if (!m_initialized) return;
    if (!m_skillTable || !m_soundCache) return;

    m_debugStats.eventsPosted++;

    const SkillSoundEntry* entry = m_skillTable->Get(evt.skill_id);
    if (!entry) {
        if (m_warnedSkillIds.insert(evt.skill_id).second)
            OutputDebugStringA(std::format("[SpatialAudio] No sound mapping for skill {}\n", evt.skill_id).c_str());
        return;
    }

    m_debugStats.lastSkillId = evt.skill_id;
    m_debugStats.lastSkillName = entry->name;

    // Determine which file list to play based on the event category
    const std::vector<uint32_t>* fileIds = nullptr;
    if (evt.category == SoundCategory::SKILL_CAST)
        fileIds = &entry->caster_sounds;
    else if (evt.category == SoundCategory::HIT)
        fileIds = &entry->target_sounds;

    if (!fileIds || fileIds->empty()) return;

    OutputDebugStringA(std::format("[SpatialAudio] Post: skill={} '{}' cat={} files={} pos=({:.0f},{:.0f},{:.0f})\n",
        evt.skill_id, entry->name, (int)evt.category, fileIds->size(), agentX, agentY, agentZ).c_str());

    int soundsThisPost = 0;
    for (uint32_t fileId : *fileIds) {
        const AudioBuffer* buf = m_soundCache->Get(fileId);
        if (!buf) {
            OutputDebugStringA(std::format("[SpatialAudio]   fileId {} -> cache miss/fail\n", fileId).c_str());
            continue;
        }

        VoiceSlot* slot = AcquireVoice();
        if (!slot) {
            OutputDebugStringA("[SpatialAudio] Voice pool exhausted\n");
            break;
        }

        slot->emitterX = agentX;
        slot->emitterY = agentY;
        slot->emitterZ = agentZ;

        SubmitBuffer(slot, buf, evt.volume);
        soundsThisPost++;
    }
    m_debugStats.soundsPlayed += soundsThisPost;
}

void SpatialAudioEngine::StopAll()
{
    std::lock_guard<std::mutex> lock(m_voiceMutex);
    for (auto& slot : m_voicePool) {
        if (slot.inUse && slot.voice) {
            slot.voice->Stop();
            slot.voice->FlushSourceBuffers();
            slot.inUse = false;
            slot.pcmOwned.clear();
        }
    }
}

SpatialAudioEngine::VoiceSlot* SpatialAudioEngine::AcquireVoice()
{
    std::lock_guard<std::mutex> lock(m_voiceMutex);
    for (auto& slot : m_voicePool) {
        if (!slot.inUse) {
            slot.inUse = true;
            ZeroEmitter(slot.emitter);
            memset(slot.matrixCoeffs, 0, sizeof(slot.matrixCoeffs));
            memset(&slot.dspSettings, 0, sizeof(slot.dspSettings));
            return &slot;
        }
    }

    // Steal the first slot (oldest) as fallback
    auto& slot = m_voicePool[0];
    if (slot.voice) {
        slot.voice->Stop();
        slot.voice->FlushSourceBuffers();
    }
    slot.inUse = true;
    slot.pcmOwned.clear();
    ZeroEmitter(slot.emitter);
    memset(slot.matrixCoeffs, 0, sizeof(slot.matrixCoeffs));
    memset(&slot.dspSettings, 0, sizeof(slot.dspSettings));
    return &slot;
}

void SpatialAudioEngine::ReleaseVoice(VoiceSlot* slot)
{
    std::lock_guard<std::mutex> lock(m_voiceMutex);
    if (slot) {
        slot->inUse = false;
        slot->pcmOwned.clear();
    }
}

void SpatialAudioEngine::SubmitBuffer(VoiceSlot* slot, const AudioBuffer* buf, float volume)
{
    if (!buf || !buf->valid || buf->pcmData.empty()) {
        slot->inUse = false;
        return;
    }

    uint32_t sampleRate = buf->sampleRate;
    uint16_t channels = buf->channels;
    uint16_t bits = buf->bitsPerSample;

    // If sampleRate==0, it's raw MP3 that needs decoding
    if (sampleRate == 0) {
        std::vector<uint8_t> decoded;
        if (!DecodeMp3ToWav(buf->pcmData.data(), static_cast<int>(buf->pcmData.size()),
                            decoded, sampleRate, channels)) {
            slot->inUse = false;
            return;
        }
        slot->pcmOwned = std::move(decoded);
        bits = 16;
    }
    else {
        slot->pcmOwned = buf->pcmData; // copy PCM
    }

    if (slot->pcmOwned.empty() || sampleRate == 0) {
        slot->inUse = false;
        return;
    }

    // Force mono for 3D audio (X3DAudio works best with mono sources)
    uint16_t voiceChannels = 1;
    if (channels == 2 && bits == 16) {
        // Downmix stereo to mono
        size_t sampleCount = slot->pcmOwned.size() / 4; // 2 channels * 2 bytes
        std::vector<uint8_t> mono(sampleCount * 2);
        const int16_t* src = reinterpret_cast<const int16_t*>(slot->pcmOwned.data());
        int16_t* dst = reinterpret_cast<int16_t*>(mono.data());
        for (size_t i = 0; i < sampleCount; i++) {
            int32_t l = src[i * 2];
            int32_t r = src[i * 2 + 1];
            dst[i] = static_cast<int16_t>((l + r) / 2);
        }
        slot->pcmOwned = std::move(mono);
        channels = 1;
    }

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = sampleRate;
    wfx.wBitsPerSample  = bits;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    // Create or recreate source voice if format changed
    if (slot->voice) {
        slot->voice->DestroyVoice();
        slot->voice = nullptr;
    }

    HRESULT hr = m_xaudio->CreateSourceVoice(&slot->voice, &wfx, XAUDIO2_VOICE_USEFILTER, 2.0f, &m_callback);
    if (FAILED(hr)) {
        OutputDebugStringA("[SpatialAudio] CreateSourceVoice failed\n");
        slot->inUse = false;
        return;
    }

    XAUDIO2_BUFFER xbuf{};
    xbuf.AudioBytes = static_cast<UINT32>(slot->pcmOwned.size());
    xbuf.pAudioData = slot->pcmOwned.data();
    xbuf.Flags      = XAUDIO2_END_OF_STREAM;
    xbuf.pContext   = slot;

    hr = slot->voice->SubmitSourceBuffer(&xbuf);
    if (FAILED(hr)) {
        OutputDebugStringA("[SpatialAudio] SubmitSourceBuffer failed\n");
        slot->voice->DestroyVoice();
        slot->voice = nullptr;
        slot->inUse = false;
        return;
    }

    slot->voice->SetVolume(volume);
    slot->voice->Start();
}

bool SpatialAudioEngine::DecodeMp3ToWav(const uint8_t* mp3Data, int mp3Size,
                                          std::vector<uint8_t>& pcmOut,
                                          uint32_t& sampleRate, uint16_t& channels)
{
    pcmOut.clear();
    sampleRate = 44100;
    channels = 1;

    // Use SHCreateMemStream → MFCreateMFByteStreamOnStream for proper MF integration
    IStream* memStream = SHCreateMemStream(mp3Data, static_cast<UINT>(mp3Size));
    if (!memStream) {
        OutputDebugStringA("[SpatialAudio] SHCreateMemStream failed\n");
        m_debugStats.decodeFailures++;
        return false;
    }

    IMFByteStream* mfByteStream = nullptr;
    HRESULT hr = MFCreateMFByteStreamOnStream(memStream, &mfByteStream);
    memStream->Release();
    if (FAILED(hr) || !mfByteStream) {
        OutputDebugStringA(std::format("[SpatialAudio] MFCreateMFByteStreamOnStream failed: 0x{:08X}\n", (unsigned)hr).c_str());
        m_debugStats.decodeFailures++;
        return false;
    }

    // Set content type attribute so MF knows this is MP3
    IMFAttributes* bsAttrs = nullptr;
    hr = mfByteStream->QueryInterface(IID_PPV_ARGS(&bsAttrs));
    if (SUCCEEDED(hr) && bsAttrs) {
        bsAttrs->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"audio/mpeg");
        bsAttrs->Release();
    }

    IMFSourceReader* reader = nullptr;
    hr = MFCreateSourceReaderFromByteStream(mfByteStream, nullptr, &reader);
    if (FAILED(hr) || !reader) {
        OutputDebugStringA(std::format("[SpatialAudio] MFCreateSourceReaderFromByteStream failed: 0x{:08X}\n", (unsigned)hr).c_str());
        mfByteStream->Release();
        m_debugStats.decodeFailures++;
        return false;
    }

    // Configure output as PCM
    IMFMediaType* pcmType = nullptr;
    MFCreateMediaType(&pcmType);
    pcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcmType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pcmType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pcmType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    pcmType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);

    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pcmType);
    pcmType->Release();
    if (FAILED(hr)) {
        OutputDebugStringA(std::format("[SpatialAudio] SetCurrentMediaType failed: 0x{:08X}\n", (unsigned)hr).c_str());
        reader->Release();
        mfByteStream->Release();
        m_debugStats.decodeFailures++;
        return false;
    }

    // Read all samples
    pcmOut.reserve(mp3Size * 4);
    for (;;) {
        IMFSample* sample = nullptr;
        DWORD flags = 0;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            if (sample) sample->Release();
            break;
        }
        if (!sample) continue;

        IMFMediaBuffer* mbuf = nullptr;
        sample->ConvertToContiguousBuffer(&mbuf);
        if (mbuf) {
            BYTE* data = nullptr;
            DWORD len = 0;
            mbuf->Lock(&data, nullptr, &len);
            pcmOut.insert(pcmOut.end(), data, data + len);
            mbuf->Unlock();
            mbuf->Release();
        }
        sample->Release();
    }

    reader->Release();
    mfByteStream->Release();

    sampleRate = 44100;
    channels = 1;

    if (pcmOut.empty()) {
        OutputDebugStringA("[SpatialAudio] MP3 decode produced 0 bytes\n");
        m_debugStats.decodeFailures++;
        return false;
    }

    return true;
}

void SpatialAudioEngine::VoiceCallback::OnBufferEnd(void* ctx)
{
    (void)ctx;
}

SpatialAudioEngine::DebugStats SpatialAudioEngine::GetDebugStats() const
{
    m_debugStats.voicesActive = 0;
    m_debugStats.panLeft = 0.f;
    m_debugStats.panRight = 0.f;
    for (auto& slot : m_voicePool) {
        if (slot.inUse) {
            m_debugStats.voicesActive++;
            // Grab L/R coefficients from the last active voice (stereo: ch0=L, ch1=R)
            if (m_masterChannels >= 2) {
                m_debugStats.panLeft  = slot.matrixCoeffs[0];
                m_debugStats.panRight = slot.matrixCoeffs[1];
            }
        }
    }

    if (m_skillTable)
        m_debugStats.skillTableSize = m_skillTable->IsLoaded() ? 1 : 0;
    if (m_soundCache) {
        m_debugStats.hashNotFound  = m_soundCache->GetHashNotFound();
        m_debugStats.notSoundType  = m_soundCache->GetNotSoundType();
    }
    return m_debugStats;
}
