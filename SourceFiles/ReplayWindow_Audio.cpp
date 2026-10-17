#include "pch.h"
#include "ReplayWindow.h"
#include "AssetBlacklist.h"
#include "MatchRatings.h"
#include "MatchNotes.h"
#include "MatchBookmarks.h"
#include "AgentSnapshotParser.h"
#include "StoCParser.h"
#include "SkillDatabase.h"
#include "DXMathHelpers.h"
#include "FontConfig.h"
#include "GuiGlobalConstants.h"
#include "MapBrowser.h"
#include "TextureCache.h"
#include "CursorSystem.h"
#include "SpatialAudioEngine.h"
#include "SoundCache.h"
#include "SkillSoundTable.h"
#include "Parsers/BB9AnimationParser.h"
#include "Parsers/FileReferenceParser.h"
#include "ReplayWindow_Internal.h"
#include "../ThirdParty/nanosvg/nanosvg.h"
#include "../ThirdParty/nanosvg/nanosvgrast.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <algorithm>
#include <numeric>
#include <json.hpp>
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// Extracted from ReplayWindow.cpp (partial-class split). These remain
// ReplayWindow:: member functions; only their definitions live here.
// ---------------------------------------------------------------------------

static std::filesystem::path GetSkillSoundsJsonPath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    return dir / "settings" / "skill_sounds.json";
}

void ReplayWindow::InitAudioEngine()
{
    if (m_audioInitialized || !m_datManager) return;

    m_audioEngine = std::make_unique<SpatialAudioEngine>();
    if (!m_audioEngine->Init(m_datManager, m_hashIndex)) {
        OutputDebugStringA("[Audio] SpatialAudioEngine init failed\n");
        m_audioEngine.reset();
        return;
    }

    // A hidden verification run (GWO_AUDIO_MUTE set): silent output, the levels left as they are.
    if (GetEnvironmentVariableA("GWO_AUDIO_MUTE", nullptr, 0) > 0)
        m_audioEngine->SetOutputMuted(true);

    m_skillSoundTable = std::make_unique<SkillSoundTable>();
    m_skillSoundTable->Load(GetSkillSoundsJsonPath().string());

    m_audioInitialized = true;
    OutputDebugStringA("[Audio] Engine ready\n");
}


void ReplayWindow::UpdateAudioPlayback(float currentTime, float dt)
{
    // A clip renders faster than real time and the GIF has no sound: stay quiet meanwhile.
    if (!m_audioInitialized || !m_audioEngine || !m_audioEnabled || IsClipRendering()) return;

    // Update listener from camera
    Camera* cam = m_mapRenderer ? m_mapRenderer->GetCamera() : nullptr;
    if (cam) {
        XMFLOAT3 pos = cam->GetPosition3f();

        // Take the camera's own look vector rather than reconstructing one from the view matrix.
        // The previous code read { -view._31, 0, -view._33 }, which mixes components of two
        // different basis vectors: in a row-major view matrix the third row is
        // (Right.z, Up.z, Look.z), so that expression built (-Right.z, 0, -Look.z). The X term
        // coincidentally matched Look.x, but the Z term came out negated - mirroring the front
        // vector while up stayed (0,1,0), which flips the handedness of X3DAudio's
        // right = up x front and swaps the stereo image.
        XMFLOAT3 look = cam->GetLook3f();

        // Flatten onto the horizontal plane so screen-space left/right maps to speaker
        // left/right regardless of how far the camera is pitched down.
        XMFLOAT3 flatFront = { look.x, 0.0f, look.z };
        float len = sqrtf(flatFront.x * flatFront.x + flatFront.z * flatFront.z);
        if (len > 0.001f) { flatFront.x /= len; flatFront.z /= len; }
        else { flatFront = { 0.f, 0.f, 1.f }; }   // camera looking straight down
        XMFLOAT3 up = { 0.f, 1.f, 0.f };

        // Following an agent, the viewer is that character: the ears go where it stands, as the
        // game's are on the player, and the camera keeps only the orientation. From the camera,
        // 900 units behind and above, its own swings and hits sat half-way to their silence
        // radius (measured -15 dB) while the wider skill radius barely felt it.
        if (m_followedAgentId > 0) {
            auto fit = m_replayCtx.agents.find(m_followedAgentId);
            if (fit != m_replayCtx.agents.end() && !fit->second.snapshots.empty()) {
                float ax, ay, az;
                InterpolateAgentPosition(fit->second, currentTime, m_replayCtx.interpSettings, ax, ay, az);
                pos = ApplyMapTransformToPos(ax, ay, az, m_replayCtx.mapTransform);
            }
        }

        m_audioEngine->UpdateListener(pos, flatFront, up, dt);
    }

    // Pause/resume: XAudio2 has no idea the replay itself paused, so already-triggered voices
    // would otherwise keep playing straight through it. Only act on the transition (not every
    // frame) so a held pause doesn't repeatedly call Stop() on already-stopped voices.
    const bool isPlaying = m_replayCtx.isPlaying;
    if (isPlaying != m_audioWasPlaying) {
        m_audioEngine->SetPaused(!isPlaying);
        m_audioWasPlaying = isPlaying;
    }

    // Detect scrub/seek: if timeline jumped backwards or forward significantly, reset the
    // cursors to the seek target (no backfill of what was skipped).
    bool seeked = (m_audioLastTime > currentTime + 0.01f) ||
                  (currentTime - m_audioLastTime > 2.0f);
    if (seeked) {
        m_audioEngine->StopAll();

        if (!m_audioSkillTimelineBuilt)
            BuildSkillSoundTimeline();
        size_t klo = 0, khi = m_audioSkillTimeline.size();
        while (klo < khi) {
            size_t mid = klo + (khi - klo) / 2;
            if (m_audioSkillTimeline[mid].time <= currentTime) klo = mid + 1; else khi = mid;
        }
        m_audioSkillCursor = klo;
    }

    float prevTime = m_audioLastTime;
    m_audioLastTime = currentTime;

    if (!m_audioSkillTimelineBuilt)
        BuildSkillSoundTimeline();

    // The recording's own sounds and the scheduled skill / effect / attack sounds (private).
    StepReplaySounds(prevTime, currentTime, seeked || prevTime < 0.f);

    if (prevTime < 0.f || seeked) return;

    const auto& is = m_replayCtx.interpSettings;
    const auto& mapTransform = m_replayCtx.mapTransform;

    // Whoever the viewer is "being". Camera follow is the strong signal; a manual selection is
    // the fallback so clicking an agent also focuses their audio without forcing follow mode.
    const int focusAgentId = (m_followedAgentId > 0) ? m_followedAgentId : m_selectedAgentId;

    // The Boss's sound and the world's sounds; the emitter's position is sampled at the moment it
    // fires so a moving holder pans correctly.
    while (m_audioSkillCursor < m_audioSkillTimeline.size()) {
        const auto& s = m_audioSkillTimeline[m_audioSkillCursor];
        if (s.time > currentTime) break;

        if (s.time > prevTime && s.atPoint) {
            m_audioEngine->PostFileId(s.fileId, s.category, s.point.x, s.point.y, s.point.z,
                                      /*positional=*/true, s.gain, /*focusedPlayer=*/false, s.pitch);
        }
        else if (s.time > prevTime) {
            auto it = m_replayCtx.agents.find(s.agentId);
            if (it != m_replayCtx.agents.end()) {
                float x, y, z;
                InterpolateAgentPosition(it->second, s.time, is, x, y, z);
                XMFLOAT3 world = ApplyMapTransformToPos(x, y, z, mapTransform);
                const bool focused = (focusAgentId > 0) &&
                                     (s.casterId == focusAgentId || s.targetId == focusAgentId);
                m_audioEngine->PostFileId(s.fileId, s.category, world.x, world.y, world.z,
                                          /*positional=*/true, s.gain, focused);
            }
        }

        m_audioSkillCursor++;
    }
}

// The Boss's sound and the world's sounds, flattened and sorted once. The skill casts are the
// private schedule's (StepReplaySounds), which builds once this timeline exists.
void ReplayWindow::BuildSkillSoundTimeline()
{
    // skillUseHistory is filled in a later phase than audio init; building before it exists would
    // cache an empty timeline permanently, so stay unbuilt and retry on a later frame.
    if (!m_skillUseTimelineBuilt) return;
    // The Boss claims are scheduled into the same list, so wait for them too.
    if (!m_bossTimelineBuilt) return;
    // ...and the world's own sounds, which need the map's machines resolved.
    if (!m_bundleCarryBuilt) return;

    m_audioSkillTimeline.clear();
    m_audioSkillCursor = 0;
    m_audioSkillTimelineBuilt = true;

    // The Boss: the game plays its sound on the new holder about a second after the shout.
    // Scheduled like a skill cue, so it follows the holder wherever the recording camera was.
    for (const auto& claim : m_bossClaims)
        m_audioSkillTimeline.push_back({ claim.time + kBossSoundDelay, kBossSoundFileId,
                                         claim.agentId, claim.agentId, 0,
                                         SoundLogCategory::SkillEffect, 1.0f });

    AppendWorldSounds();

    std::sort(m_audioSkillTimeline.begin(), m_audioSkillTimeline.end(),
              [](const ScheduledSkillSound& a, const ScheduledSkillSound& b) { return a.time < b.time; });
}
