#include "pch.h"
#include "SpatialAudioEngine.h"
#include "SoundCache.h"
#include "DATManager.h"

#include <cmath>
#include <algorithm>
#include <fstream>
#include <json.hpp>
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

// ── Category profiles ─────────────────────────────────────────────────────
// Derived from CategorySettings.ini (extracted from Gw.dat). Volume there is a base attenuation
// in millibels (mB, 1/100 dB); gainLinear = 10^(mB/2000) converts that to a linear multiplier.
// maxSounds is the category's concurrent-instance cap at the client's max audio quality setting
// (MinSounds/MaxSounds interpolate by quality in-game; GW Observer has no such setting, so the
// max-quality value is used - most permissive, matching "assume good hardware"). Values are the
// non-"p"-prefixed (third-person/world) variant, since replay is never any one player's own
// first-person audio. Categories the capture side can't distinguish (SkillCue blends
// skillb/skills/skillx/woosh; Footstep uses "feet") are reasonable single-representative picks,
// not exact per-sub-category fidelity - see SOUND_RECORDING_PLAYBACK_PLAN.md.
// audibleRadius is the distance (in GW units) at which the category falls to silence, and is also
// the cull distance - past it a sound is never voiced at all, so it cannot steal a slot from
// nearer action. Values are anchored to the game's own distance vocabulary, the same constants the
// range rings use: Touch 144, Nearby 240, In Area 322, Earshot 1000, Cast Range 1248,
// Passive 2512, Compass 5020. CategorySettings.ini has no distance data of its own (it only
// separates 3D behaviour coarsely via 3DMode), so these are chosen per category by what the sound
// means: feet stay local, impacts carry to about where you can see them, big spell payoffs carry
// furthest, and wind-ups stay intimate so they do not smear across the map.
// Every category exists twice in CategorySettings.ini: a world variant and a "p"-prefixed player
// variant used for the sounds the local player themselves makes or receives. The player variant is
// 1-17 dB louder (a wind-up is -2000 out in the world but -300 for you) and has a much smaller
// MaxSounds, because there is only ever one player - which also means the player's audio has its
// own budget and can never be starved by a crowded fight. Following an agent in a replay puts the
// viewer in exactly that position, so the followed agent's audio uses the player column.
struct CategoryProfile {
    float gainLinear;        // world  (e.g. [skills])
    int   maxSounds;
    float audibleRadius;
    float playerGainLinear;  // player (e.g. [pskills])
    int   playerMaxSounds;
};

static float MillibelToLinear(float mB) { return powf(10.0f, mB / 2000.0f); }

// Shapes the master slider: gain = position^kMasterCurveExponent, chosen so 60% on the slider
// lands on 30% gain. Loudness perception is roughly logarithmic, so a raw linear slider spends
// most of its travel in a range that all sounds equally loud; this pushes the usable range out.
// (A flat headroom multiplier used to sit here instead - it is gone because the per-category
// CategorySettings.ini gains below are the correct place for that attenuation, and stacking both
// simply attenuated everything twice.)
static constexpr float kMasterCurveExponent = 2.36f;

static float MasterGain(float sliderPosition)
{
    return powf(std::clamp(sliderPosition, 0.0f, 1.0f), kMasterCurveExponent);
}

// Fraction of full volume a positional sound keeps when it is directly behind the listener.
// 0.0 would mute the rear hemisphere entirely (the old behaviour).
static constexpr float kRearGainFloor = 0.35f;

static const CategoryProfile& GetCategoryProfile(SoundLogCategory category)
{
    static const CategoryProfile kProfiles[static_cast<size_t>(SoundLogCategory::Count)] = {
        //                 world gain                cap   radius     player gain              cap
        /* Background */ { MillibelToLinear(-100.f), 20,   5020.f, MillibelToLinear(-100.f),  20 },
        /* Ui         */ { MillibelToLinear(0.f),    16,      0.f, MillibelToLinear(0.f),     16 },
        /* Music      */ { MillibelToLinear(-500.f),  1,      0.f, MillibelToLinear(-500.f),   1 },
        /* Dialog     */ { MillibelToLinear(0.f),     4,   1248.f, MillibelToLinear(0.f),      4 },
        /* HitBow     */ { MillibelToLinear(-300.f),  4,   1248.f, MillibelToLinear(0.f),      2 }, // arrhit/parrhit
        /* HitAxe     */ { MillibelToLinear(-450.f), 16,   1248.f, MillibelToLinear(0.f),      4 }, // axehit/paxehit
        /* HitHammer  */ { MillibelToLinear(-600.f),  8,   1248.f, MillibelToLinear(-200.f),   4 }, // hamhit/phamhit
        /* HitDagger  */ { MillibelToLinear(-400.f), 16,   1000.f, MillibelToLinear(200.f),    4 }, // daghit/pdaghit
        /* HitScythe  */ { MillibelToLinear(-400.f),  8,   1248.f, MillibelToLinear(0.f),      4 }, // scyhit/pscyhit
        /* HitSpear   */ { MillibelToLinear(-400.f),  8,   1248.f, MillibelToLinear(0.f),      4 }, // sprhit/psprhit
        /* HitSword   */ { MillibelToLinear(-750.f), 16,   1248.f, MillibelToLinear(-350.f),   4 }, // swohit/pswohit
        /* HitMagic   */ { MillibelToLinear(-500.f), 16,   1248.f, MillibelToLinear(-400.f),   4 }, // maghit/pmaghit
        /* HitMiss    */ { MillibelToLinear(-500.f),  8,   1000.f, MillibelToLinear(-100.f),   3 }, // miss/pmiss
        // Skill layers, each taken verbatim from its own CategorySettings.ini section instead of
        // sharing one averaged bucket. The warmup layer being 10x quieter than the rest is the
        // single biggest difference between "a fight" and "a wall of noise": roughly half of all
        // captured skill cues are warmups, and the game plays them at -2000mB.
        // The wind-up gap is the widest in the whole table: -2000 out in the world against -300
        // for the followed player. That single pairing is what makes a followed caster's spells
        // announce themselves while the rest of the map stays a texture.
        /* SkillWarmup*/ { MillibelToLinear(-2000.f), 4,   1000.f, MillibelToLinear(-300.f),   2 }, // wrmups/pwrmups
        /* SkillEffect*/ { MillibelToLinear(-600.f),  8,   2512.f, MillibelToLinear(0.f),      4 }, // skills/pskills
        /* SkillExtra */ { MillibelToLinear(-600.f),  8,   1600.f, MillibelToLinear(-50.f),    4 }, // skillx/pskillx
        /* SkillWoosh */ { MillibelToLinear(-700.f), 12,   1248.f, MillibelToLinear(-200.f),   2 }, // woosh/pwoosh
        /* Footstep   */ { MillibelToLinear(-1500.f),16,    500.f, MillibelToLinear(-1200.f),  2 }, // feet/pfeet
    };
    return kProfiles[static_cast<size_t>(category)];
}

// User-adjustable per-SND_TYPE slider (Sound FX panel) covering the category. All Effects-type
// sub-categories (hits, skill cues, footsteps) share the single "Effects" slider, matching the
// granularity the panel exposes - CategorySettings.ini's own per-category Volume (baked into
// GetCategoryProfile above) is what distinguishes them from each other underneath that.
static float TypeVolumeMultiplier(SoundLogCategory category, const AudioConfig& cfg)
{
    switch (category) {
        case SoundLogCategory::Background: return cfg.background_volume;
        case SoundLogCategory::Ui:         return cfg.ui_volume;
        case SoundLogCategory::Music:      return cfg.music_volume;
        case SoundLogCategory::Dialog:     return cfg.dialog_volume;
        case SoundLogCategory::Footstep:   return cfg.ambient_volume;
        default:
            return IsSkillCategory(category) ? cfg.skills_volume   // Skill*
                                             : cfg.attacks_volume; // Hit*
    }
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

bool SpatialAudioEngine::Init(DATManager* datMgr, const std::unordered_map<int, std::vector<int>>* hashIndex)
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

    RebuildDistanceCurve();

    // Init sound cache with shared hash index (same one used for textures/models)
    m_soundCache = std::make_unique<SoundCache>();
    m_soundCache->Init(datMgr, hashIndex);

    LoadSoundOverrides();

    m_initialized = true;
    OutputDebugStringA(std::format("[SpatialAudio] Initialized with {} output channels\n", m_masterChannels).c_str());
    return true;
}

float SpatialAudioEngine::DistanceGain(float normalizedDistance) const
{
    const float x = std::clamp(normalizedDistance, 0.0f, 1.0f);

    // Reference distance: full volume inside it. Clamped away from 0 because the inverse law
    // divides by it, and away from 1 so there is always some falloff left to hear.
    const float ref = std::clamp(m_config.near_field_fraction, 0.001f, 0.90f);
    if (x <= ref) return 1.0f;

    const float rolloff = std::max(0.01f, m_config.distance_rolloff);

    // Inverse distance law: gain = ref / (ref + rolloff * (d - ref)). This is the model
    // DirectSound3D and OpenAL use, and it is logarithmic in dB - a fixed drop per doubling of
    // distance rather than a straight line - which is how loudness is actually perceived.
    auto inverse = [ref, rolloff](float d) { return ref / (ref + rolloff * (d - ref)); };

    // The bare law never reaches zero, so a sound would still be audible at the cull radius and
    // cut off abruptly there. Rescaling by the value at the edge slides the whole curve down so
    // it arrives at silence exactly at the radius, keeping the shape but removing the step.
    const float atEdge = inverse(1.0f);
    return std::clamp((inverse(x) - atEdge) / (1.0f - atEdge), 0.0f, 1.0f);
}

void SpatialAudioEngine::RebuildDistanceCurve()
{
    for (int i = 0; i < kCurvePointCount; ++i) {
        const float x = static_cast<float>(i) / static_cast<float>(kCurvePointCount - 1);
        m_curvePoints[i].Distance   = x;
        m_curvePoints[i].DSPSetting = DistanceGain(x);
    }
    m_distanceCurve.pPoints    = m_curvePoints;
    m_distanceCurve.PointCount = kCurvePointCount;
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

    // Keep the falloff in step with the sliders. 33 evaluations of a divide - far cheaper than
    // tracking which control changed and risking a stale curve.
    RebuildDistanceCurve();

    // Apply master volume
    if (m_masterVoice)
        m_masterVoice->SetVolume(MasterGain(m_config.master_volume));

    // Update 3D for all active voices
    std::lock_guard<std::mutex> lock(m_voiceMutex);
    for (auto& slot : m_voicePool) {
        if (!slot.inUse || !slot.voice) continue;

        // Re-apply category/type volume every frame rather than only at voice start, so moving
        // a Sound FX slider while a sound is already playing takes effect immediately instead of
        // only on the next sound that starts. baseGainLinear already carries the world-vs-player
        // choice made when the voice was acquired.
        slot.voice->SetVolume(slot.baseGainLinear * TypeVolumeMultiplier(slot.category, m_config));

        if (slot.positional) {
            // Squash the emitter toward the listener's own height before X3DAudio sees it, so
            // camera altitude stops dominating the distance. Done here rather than by editing the
            // curve because it must affect the panning geometry too: a fight below an elevated
            // camera should pan across the stereo field, not collapse to "far away and centred".
            const float ey = m_listener.Position.y +
                             (slot.emitterY - m_listener.Position.y) * m_config.vertical_distance_scale;
            slot.emitter.Position = { slot.emitterX, ey, slot.emitterZ };
            slot.emitter.pVolumeCurve = &m_distanceCurve;
            // X3DAudio normalises distance by CurveDistanceScaler, so this must BE the radius at
            // which the category goes silent. It used to be curve_distance_scaler(14) *
            // max_audible_radius(2500) = 35000 units - several map widths - which is why a sound
            // at the edge of the compass still played at ~62% and everything blurred together.
            slot.emitter.CurveDistanceScaler =
                std::max(1.0f, GetCategoryProfile(slot.category).audibleRadius *
                               m_config.hearing_distance_scale);

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
            // Map to volume: full at >30deg forward, falling off towards the rear.
            float frontGain = std::clamp((dotFront + 0.5f) / 1.0f, 0.0f, 1.0f);
            frontGain *= frontGain; // quadratic fade for smoother transition
            // Floor it rather than muting outright: a spike going off behind the camera should
            // still be heard (quieter, and panned by the X3DAudio matrix), not vanish. Fully
            // silencing the rear hemisphere is what made skill casts seem to only exist while
            // the camera happened to face them.
            frontGain = kRearGainFloor + (1.0f - kRearGainFloor) * frontGain;

            for (UINT32 ch = 0; ch < m_masterChannels; ch++)
                slot.matrixCoeffs[ch] *= frontGain;

            if (m_config.swap_stereo && m_masterChannels >= 2)
                std::swap(slot.matrixCoeffs[0], slot.matrixCoeffs[1]);

            slot.voice->SetOutputMatrix(m_masterVoice, 1, m_masterChannels, slot.matrixCoeffs);
            slot.voice->SetFrequencyRatio(slot.dspSettings.DopplerFactor);

            // Apply low-pass filter for sounds approaching the side/rear
            float lpfCoeff = slot.dspSettings.LPFDirectCoefficient;
            XAUDIO2_FILTER_PARAMETERS filterParams;
            filterParams.Type = LowPassFilter;
            filterParams.Frequency = 2.0f * sinf(X3DAUDIO_PI / 6.0f * lpfCoeff);
            filterParams.OneOverQ = 1.0f;
            slot.voice->SetFilterParameters(&filterParams);
        }
        else {
            // Non-spatial (music/dialog/UI, per the recorder's own Positional flag): flat
            // equal-power matrix, no distance/direction attenuation, no Doppler/LPF.
            float coeff = (m_masterChannels > 0) ? 1.0f / sqrtf(static_cast<float>(m_masterChannels)) : 1.0f;
            for (UINT32 ch = 0; ch < m_masterChannels; ch++)
                slot.matrixCoeffs[ch] = coeff;
            slot.voice->SetOutputMatrix(m_masterVoice, 1, m_masterChannels, slot.matrixCoeffs);
        }

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

int SpatialAudioEngine::CountActiveVoicesInCategory(SoundLogCategory category, bool focusedPlayer) const
{
    int count = 0;
    for (const auto& slot : m_voicePool) {
        if (slot.inUse && slot.category == category && slot.focusedPlayer == focusedPlayer)
            count++;
    }
    return count;
}

void SpatialAudioEngine::PostFileId(uint32_t fileId, SoundLogCategory category, float x, float y, float z,
                                    bool positional, float gainScale, bool focusedPlayer)
{
    if (!m_initialized || !m_soundCache) return;

    m_debugStats.eventsPosted++;

    // Per-sample override (settings/sound_overrides.json). A muted sample is dropped here so it
    // never occupies a voice slot.
    if (!m_soundOverrides.empty()) {
        auto ov = m_soundOverrides.find(fileId);
        if (ov != m_soundOverrides.end()) {
            if (ov->second <= 0.0f) return;
            gainScale *= ov->second;
        }
    }

    const auto& profile = GetCategoryProfile(category);

    // A zero radius means the category is 2D (CategorySettings.ini 3DMode=0: ui, music). The
    // recording still flags most of those events positional, but honouring that would run them
    // through a distance curve with no meaningful radius and silence them outright.
    if (profile.audibleRadius <= 0.f)
        positional = false;

    // Out of earshot: drop it before it can take a voice. Playing something inaudible is not just
    // wasted work - it occupies one of the category's limited slots and can push a sound the
    // viewer would actually have heard past the cap.
    if (positional) {
        const float radius = profile.audibleRadius * m_config.hearing_distance_scale;
        const float dx = x - m_listener.Position.x;
        const float dy = (y - m_listener.Position.y) * m_config.vertical_distance_scale;
        const float dz = z - m_listener.Position.z;
        if ((dx * dx + dy * dy + dz * dz) > (radius * radius)) {
            m_debugStats.culledByDistance++;
            return;
        }
    }

    // Mirror the game's own per-category polyphony cap (CategorySettings.ini MaxSounds): if
    // this category is already saturated, drop the new instance rather than voice it - the same
    // throttling a crowded real fight would apply, just replicated here instead of (or in
    // addition to) whatever already happened upstream at capture time.
    const int cap = focusedPlayer ? profile.playerMaxSounds : profile.maxSounds;
    std::unique_lock lock(m_voiceMutex);
    if (CountActiveVoicesInCategory(category, focusedPlayer) >= cap) {
        m_debugStats.droppedAtCap++;
        return;
    }
    lock.unlock();

    const AudioBuffer* buf = m_soundCache->Get(fileId);
    if (!buf) return;

    VoiceSlot* slot = AcquireVoice();
    if (!slot) return;

    slot->positional = positional;
    slot->category = category;
    slot->focusedPlayer = focusedPlayer;
    slot->emitterX = x;
    slot->emitterY = y;
    slot->emitterZ = z;

    slot->baseGainLinear = (focusedPlayer ? profile.playerGainLinear : profile.gainLinear) * gainScale;
    SubmitBuffer(slot, buf, slot->baseGainLinear * TypeVolumeMultiplier(category, m_config));
    m_lastPlayedFileId = fileId;
    m_debugStats.soundsPlayed++;
}

void SpatialAudioEngine::LoadSoundOverrides()
{
    m_soundOverrides.clear();

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto path = std::filesystem::path(exePath).parent_path() / "settings" / "sound_overrides.json";

    std::ifstream f(path);
    if (!f.is_open()) return;   // optional file - absence just means no overrides

    try {
        nlohmann::json j;
        f >> j;
        for (auto& [key, val] : j.items()) {
            if (key.empty() || key[0] == '_' || !val.is_object() || !val.contains("gain"))
                continue;   // "_comment" / "_candidates" and other prose entries
            if (!val["gain"].is_number()) continue;
            try {
                m_soundOverrides[static_cast<uint32_t>(std::stoul(key))] =
                    std::max(0.0f, val["gain"].get<float>());
            }
            catch (const std::exception&) { /* non-numeric key */ }
        }
        OutputDebugStringA(std::format("[SpatialAudio] {} sound overrides loaded\n",
                                       m_soundOverrides.size()).c_str());
    }
    catch (const std::exception& ex) {
        OutputDebugStringA(std::format("[SpatialAudio] sound_overrides.json parse error: {}\n",
                                       ex.what()).c_str());
    }
}

void SpatialAudioEngine::PlayDirectionalTest(TestDirection dir)
{
    if (!m_initialized) return;

    // Reuse whatever last played so the probe can't be silent for a bad file id; 8901 (a common
    // skill layer) is only the fallback for a session where nothing has played yet.
    const uint32_t fileId = m_lastPlayedFileId ? m_lastPlayedFileId : 8901u;

    const XMFLOAT3 pos   = { m_listener.Position.x, m_listener.Position.y, m_listener.Position.z };
    const XMFLOAT3 front = { m_listener.OrientFront.x, m_listener.OrientFront.y, m_listener.OrientFront.z };
    const XMFLOAT3 up    = { m_listener.OrientTop.x, m_listener.OrientTop.y, m_listener.OrientTop.z };

    // X3DAudio is left-handed, so right = up x front.
    XMVECTOR vFront = XMVector3Normalize(XMLoadFloat3(&front));
    XMVECTOR vUp    = XMVector3Normalize(XMLoadFloat3(&up));
    XMVECTOR vRight = XMVector3Normalize(XMVector3Cross(vUp, vFront));

    XMVECTOR offset{};
    switch (dir) {
        case TestDirection::Left:  offset = XMVectorNegate(vRight); break;
        case TestDirection::Right: offset = vRight;                 break;
        case TestDirection::Front: offset = vFront;                 break;
        case TestDirection::Back:  offset = XMVectorNegate(vFront); break;
    }

    // Close enough to stay well inside the distance curve, far enough to pan hard.
    constexpr float kTestDistance = 300.0f;
    XMFLOAT3 emitter;
    XMStoreFloat3(&emitter, XMVectorAdd(XMLoadFloat3(&pos), XMVectorScale(offset, kTestDistance)));

    PostFileId(fileId, SoundLogCategory::SkillEffect, emitter.x, emitter.y, emitter.z, true);
}

void SpatialAudioEngine::SetPaused(bool paused)
{
    std::lock_guard<std::mutex> lock(m_voiceMutex);
    for (auto& slot : m_voicePool) {
        if (!slot.inUse || !slot.voice) continue;
        if (paused) slot.voice->Stop();
        else slot.voice->Start();
    }
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
            slot.positional = true;
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
    slot.positional = true;
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

    if (m_soundCache) {
        m_debugStats.hashNotFound  = m_soundCache->GetHashNotFound();
        m_debugStats.notSoundType  = m_soundCache->GetNotSoundType();
    }
    return m_debugStats;
}
