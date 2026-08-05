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

namespace {

// Gain by position within a burst of sounds that land together. Layers played at equal volume
// simply sum, so one four-layer event hits far harder than a one-layer event for no musical
// reason; the first layer carries the sound and the rest fill it out. Shared by the skill cues
// and the StoC effects, which layer just as heavily.
constexpr float kLayerGains[] = { 1.0f, 0.55f, 0.35f, 0.22f };
constexpr int   kLayerGainCount = static_cast<int>(std::size(kLayerGains));

// Binary search on an agent's snapshots for the weapon_type in effect at time `t`. Same lookup
// shape as the position lookup below, just reading a different field. Returns 0 (no match /
// unarmed) if the agent or its snapshots aren't known.
uint16_t FindWeaponTypeAt(const AgentReplayData& ard, float t)
{
    if (ard.snapshots.empty()) return 0;
    auto& snaps = ard.snapshots;
    int idx = 0;
    if (t >= snaps.back().time)
        idx = static_cast<int>(snaps.size()) - 1;
    else if (t > snaps.front().time) {
        int lo = 0, hi = static_cast<int>(snaps.size()) - 1;
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            if (snaps[mid].time <= t) lo = mid; else hi = mid - 1;
        }
        idx = lo;
    }
    return snaps[idx].weapon_type;
}

// Maps a captured sound_events.txt entry to a CategorySettings.ini-derived playback category.
// The capture side only records the coarse SND_TYPE (0-4); anything finer is derived here:
//   - Effects-type sounds correlated to a skill (cause_skill_id != 0) -> SkillCue
//   - Effects-type sounds correlated to a plain attack (cause_agent_id != 0, cause_skill_id==0)
//     -> the specific hit category, via the causing agent's weapon_type at the event's timestamp
//       (GWCA weapon_type: 1=bow,2=axe,3=hammer,4=daggers,5=scythe,6=spear,7=sword,10/12/14=
//       wand/staff, matching arrhit/axehit/hamhit/daghit/scyhit/sprhit/swohit/maghit 1:1)
//   - Uncorrelated Effects-type sounds -> Footstep (the dominant uncorrelated case in practice)
SoundLogCategory ClassifySoundEvent(const SoundLogEvent& ev, const ReplayContext& ctx,
                                    const std::unordered_set<uint32_t>& attackSoundIds)
{
    switch (ev.sound_type) {
        case 0: return SoundLogCategory::Background;
        case 2: return SoundLogCategory::Ui;
        case 3: return SoundLogCategory::Music;
        case 4: return SoundLogCategory::Dialog;
        default: break; // sound_type == 1 (Effects), the dominant/most-granular bucket
    }

    if (ev.cause_skill_id != 0)
        return SoundLogCategory::SkillEffect;   // suppressed at playback; skills come from JSON

    if (ev.cause_agent_id != 0) {
        auto it = ctx.agents.find(ev.cause_agent_id);
        if (it != ctx.agents.end()) {
            switch (FindWeaponTypeAt(it->second, ev.time)) {
                case 1:  return SoundLogCategory::HitBow;
                case 2:  return SoundLogCategory::HitAxe;
                case 3:  return SoundLogCategory::HitHammer;
                case 4:  return SoundLogCategory::HitDagger;
                case 5:  return SoundLogCategory::HitScythe;
                case 6:  return SoundLogCategory::HitSpear;
                case 7:  return SoundLogCategory::HitSword;
                case 10: case 12: case 14: return SoundLogCategory::HitMagic;
                default: return SoundLogCategory::HitMiss;
            }
        }
        return SoundLogCategory::HitMiss;
    }

    // Uncorrelated. The recorder only fills cause_agent_id when it can attribute the sound, and it
    // fails to do so for most combat audio - around 70% of the attack sounds in a match arrive
    // with no agent at all. Falling through to Footstep would put them on the ambient fader with
    // a footstep's volume and a 500-unit radius, which is why turning "Attacks" down barely
    // changed the mix. If this exact sound is heard elsewhere in the same match attributed to an
    // attack, treat it as one.
    if (attackSoundIds.count(ev.file_id))
        return SoundLogCategory::HitMiss;   // "attack with no discernible weapon"

    return SoundLogCategory::Footstep;
}

} // namespace

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

    m_skillSoundTable = std::make_unique<SkillSoundTable>();
    m_skillSoundTable->Load(GetSkillSoundsJsonPath().string());

    m_audioInitialized = true;
    OutputDebugStringA("[Audio] Engine ready\n");
}


void ReplayWindow::UpdateAudioPlayback(float currentTime, float dt)
{
    if (!m_audioInitialized || !m_audioEngine || !m_audioEnabled) return;

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
    // cursor. This must happen before the early return below - by the time execution would
    // reach a same-named check after that return, `seeked` can no longer be true (this frame
    // already returns), so a check placed after it would be dead code. Reset against
    // currentTime (the seek target), not the old position.
    bool seeked = (m_audioLastTime > currentTime + 0.01f) ||
                  (currentTime - m_audioLastTime > 2.0f);
    if (seeked) {
        m_audioEngine->StopAll();

        auto& soundEvents = m_replayCtx.stocData.soundEvents;
        size_t lo = 0, hi = soundEvents.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (soundEvents[mid].time <= currentTime) lo = mid + 1; else hi = mid;
        }
        m_audioSoundEventCursor = lo;

        // Same no-backfill-on-seek behavior for the skill timeline.
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

    if (prevTime < 0.f || seeked) return;

    // Real captured audio (StoC/sound_events.txt) for everything except skills: Background, UI,
    // Music, Dialog, attack-hits and footsteps. Skill cues are deliberately dropped here and
    // driven from skill_sounds.json instead (below) - the recorder only wrote a line for skills
    // cast within earshot of its own camera, so using it for skills gives sound to a handful of
    // casts near the recording player and silence for the rest of the map.
    auto& soundEvents = m_replayCtx.stocData.soundEvents;
    const auto& is = m_replayCtx.interpSettings;
    const auto& mapTransform = m_replayCtx.mapTransform;

    // Whoever the viewer is "being". Camera follow is the strong signal; a manual selection is
    // the fallback so clicking an agent also focuses their audio without forcing follow mode.
    const int focusAgentId = (m_followedAgentId > 0) ? m_followedAgentId : m_selectedAgentId;

    if (!m_audioStocAnalysisBuilt)
        BuildStocSoundAnalysis();

    while (m_audioSoundEventCursor < soundEvents.size()) {
        const auto& ev = soundEvents[m_audioSoundEventCursor];
        if (ev.time > currentTime) break;

        if (ev.time > prevTime) {
            const float layerGain = (m_audioSoundEventCursor < m_audioStocGain.size())
                                  ? m_audioStocGain[m_audioSoundEventCursor] : 1.0f;
            const SoundLogCategory category =
                ClassifySoundEvent(ev, m_replayCtx, m_audioAttackSoundIds);
            if (layerGain > 0.f && !IsSkillCategory(category)) {
                // The recorder writes positions in GWCA game space (x, y, z_height). The listener
                // lives in render space (x, height, y) after the map transform, so these have to
                // go through exactly the same conversion the agents and the skill sounds do -
                // otherwise the horizontal Y is treated as a height and the offset/rotation/scale
                // are missing entirely, putting every emitter somewhere arbitrary relative to the
                // camera. This was inert while the falloff reached 35000 units and everything
                // played at full volume regardless; it only became audible once distance mattered.
                XMFLOAT3 world = ApplyMapTransformToPos(ev.x, ev.y, ev.z, mapTransform);
                // Attack hits carry the agent that caused them, so the followed agent's own
                // swings (and the hits landing on them) get the player column too.
                const bool focused = (focusAgentId > 0) && (ev.cause_agent_id == focusAgentId);
                m_audioEngine->PostFileId(ev.file_id, category, world.x, world.y, world.z,
                                          ev.positional, layerGain, focused);
            }
        }

        m_audioSoundEventCursor++;
    }

    // Skill sounds, replayed from the captured per-skill timelines in skill_sounds.json. Each
    // entry is one layer of one cast, already resolved to an absolute time; the emitter's
    // position is sampled at the moment it fires so a moving caster/target pans correctly.
    if (!m_audioSkillTimelineBuilt)
        BuildSkillSoundTimeline();

    while (m_audioSkillCursor < m_audioSkillTimeline.size()) {
        const auto& s = m_audioSkillTimeline[m_audioSkillCursor];
        if (s.time > currentTime) break;

        if (s.time > prevTime) {
            auto it = m_replayCtx.agents.find(s.agentId);
            if (it != m_replayCtx.agents.end()) {
                float x, y, z;
                InterpolateAgentPosition(it->second, s.time, is, x, y, z);
                XMFLOAT3 world = ApplyMapTransformToPos(x, y, z, mapTransform);
                // Sounds the followed agent casts or receives take the louder, separately
                // budgeted player column - the perspective the viewer is actually watching from.
                const bool focused = (focusAgentId > 0) &&
                                     (s.casterId == focusAgentId || s.targetId == focusAgentId);
                m_audioEngine->PostFileId(s.fileId, s.category, world.x, world.y, world.z,
                                          /*positional=*/true, s.gain, focused);
            }
        }

        m_audioSkillCursor++;
    }
}

// One-off pass over this match's StoC sound events. Two products, both of which the skill path
// already had and the StoC path did not.
void ReplayWindow::BuildStocSoundAnalysis()
{
    const auto& events = m_replayCtx.stocData.soundEvents;
    m_audioStocAnalysisBuilt = true;
    m_audioAttackSoundIds.clear();
    m_audioStocGain.assign(events.size(), 1.0f);
    if (events.empty()) return;

    // 1. Which sounds are attacks? A row with a causing agent but no skill is an attack by
    //    construction, so its file id is an attack sound wherever else it appears in this match.
    for (const auto& ev : events) {
        if (ev.sound_type == 1 && ev.cause_agent_id != 0 && ev.cause_skill_id == 0)
            m_audioAttackSoundIds.insert(ev.file_id);
    }

    // 2. Duck simultaneous layers and drop exact repeats. A third of effect events fire two or
    //    more distinct sounds from one point at one instant (occasionally sixteen); played flat
    //    they simply sum, which is most of why impacts feel oversized next to casts.
    constexpr float kSameInstant = 0.03f;   // within this, sounds belong to one event
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].sound_type != 1) continue;

        int   layer = 0;
        bool  duplicate = false;
        for (size_t j = i; j-- > 0; ) {
            if (events[i].time - events[j].time > kSameInstant) break;
            if (events[j].sound_type != 1) continue;
            if (events[j].x != events[i].x || events[j].y != events[i].y || events[j].z != events[i].z)
                continue;   // different emitter, genuinely a separate sound
            if (events[j].file_id == events[i].file_id) { duplicate = true; break; }
            ++layer;
        }

        m_audioStocGain[i] = duplicate
            ? 0.0f
            : kLayerGains[std::min<int>(layer, kLayerGainCount - 1)];
    }

    OutputDebugStringA(std::format("[Audio] StoC analysis: {} attack sounds identified\n",
                                   m_audioAttackSoundIds.size()).c_str());
}

// Flattens every cast in every agent's skillUseHistory into one time-sorted list of individual
// sound layers. Done once (skillUseHistory is fully built before playback), so the per-frame cost
// is just a cursor walk and a seek is a binary search - no per-cast scheduling state to keep
// consistent across pause/scrub.
void ReplayWindow::BuildSkillSoundTimeline()
{
    // skillUseHistory is filled in a later phase than audio init; building before it exists would
    // cache an empty timeline permanently, so stay unbuilt and retry on a later frame.
    if (!m_skillUseTimelineBuilt) return;

    m_audioSkillTimeline.clear();
    m_audioSkillCursor = 0;
    m_audioSkillTimelineBuilt = true;

    if (!m_skillSoundTable || !m_skillSoundTable->IsLoaded()) return;

    // An interrupted/cancelled cast still plays whatever layers had already fired, but none of
    // the ones scheduled past the point it actually ended (no impact sound for a Meteor that got
    // dazed off). The grace keeps the ~0ms "cast begins" layer for a near-instant interrupt.
    constexpr float kTruncationGrace = 0.05f;

    // A cue this close to cast start is the wind-up rather than the effect (the capture shows the
    // layers clustering at ~0ms and again around cast completion, split almost exactly 50/50).
    constexpr float kWarmupWindow = 0.10f;
    // Below this a skill is effectively instant, so it has no wind-up phase to speak of.
    constexpr float kInstantCastEpsilon = 0.05f;

    // Cues within this of each other are one layered sound rather than separate events.
    constexpr float kLayerClusterWindow = 0.10f;

    for (const auto& [agentId, ard] : m_replayCtx.agents) {
        if (ard.type != AgentType::Player && ard.type != AgentType::NPC) continue;

        for (const auto& use : ard.skillUseHistory) {
            const SkillSoundEntry* entry = m_skillSoundTable->Get(static_cast<uint32_t>(use.skillId));
            if (!entry) continue;

            const bool truncated = use.wasCancelled || use.wasInterrupted;
            const float castActual = use.endTime - use.startTime;

            // Spread the simultaneous layers of one cast across the three "effect" categories the
            // way the game does, so three stacked layers cost one voice in each of three separate
            // budgets instead of three in a single one. Without this the layers of a couple of
            // casts alone saturate the category and everything after them is dropped.
            int layerIndex = 0;

            auto emit = [&](const std::vector<SkillSoundCue>& cues, int emitterId) {
                float clusterStart = -1e9f;   // start of the current burst of simultaneous cues
                int   inCluster    = 0;

                for (const auto& cue : cues) {
                    if (truncated && cue.offset > castActual + kTruncationGrace)
                        break; // cues are offset-sorted, so everything after this is later too

                    SoundLogCategory category;
                    if (entry->castDuration > kInstantCastEpsilon && cue.offset < kWarmupWindow) {
                        // Fires as the cast begins on a skill that actually has a cast time: this
                        // is the wind-up, which the game keeps far quieter than the payoff.
                        category = SoundLogCategory::SkillWarmup;
                    }
                    else {
                        switch (layerIndex++ % 3) {
                            case 0:  category = SoundLogCategory::SkillEffect; break;
                            case 1:  category = SoundLogCategory::SkillExtra;  break;
                            default: category = SoundLogCategory::SkillWoosh;  break;
                        }
                    }

                    // Layers landing together are one composite sound, not several events. Played
                    // at equal volume they simply sum, so a 4-layer instant (every Dervish flash
                    // enchantment) hits far harder than a 1-layer one for no musical reason. Duck
                    // each additional layer of a burst so the first reads as the sound and the
                    // rest fill it out.
                    if (cue.offset - clusterStart > kLayerClusterWindow) {
                        clusterStart = cue.offset;
                        inCluster    = 0;
                    }
                    const float gain = kLayerGains[std::min<int>(inCluster, kLayerGainCount - 1)];
                    ++inCluster;

                    m_audioSkillTimeline.push_back({ use.startTime + cue.offset, cue.file_id,
                                                     emitterId, agentId, use.targetId,
                                                     category, gain });
                }
            };

            emit(entry->casterCues, agentId);
            // targetId <= 0 means self/no target (the many self-cast heals and enchantments),
            // in which case the target layers belong on the caster.
            emit(entry->targetCues, (use.targetId > 0) ? use.targetId : agentId);
        }
    }

    std::sort(m_audioSkillTimeline.begin(), m_audioSkillTimeline.end(),
              [](const ScheduledSkillSound& a, const ScheduledSkillSound& b) { return a.time < b.time; });

    OutputDebugStringA(std::format("[Audio] Skill sound timeline: {} scheduled sounds\n",
                                   m_audioSkillTimeline.size()).c_str());
}
