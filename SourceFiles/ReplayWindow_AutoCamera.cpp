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


// ---------------------------------------------------------------------------
// Follow-agent camera
// ---------------------------------------------------------------------------

void ReplayWindow::EnterFollowMode(int agentId)
{
    auto it = m_replayCtx.agents.find(agentId);
    if (it == m_replayCtx.agents.end()) return;

    const auto& ard = it->second;
    if (ard.snapshots.empty()) return;

    bool wasFollowing = (m_cameraMode == CameraMode::FollowAgent && m_followedAgentId >= 0);

    if (wasFollowing && m_followedAgentId != agentId)
    {
        // Switching targets: pan the orbit center from old agent to new.
        // Orbit distance, yaw, and pitch stay exactly as the user set them.
        m_followTransFromAgentId = m_followedAgentId;
        m_followTransActive      = true;
        m_followTransElapsed     = 0.f;
    }
    else if (!wasFollowing)
    {
        // Entering follow mode from Free camera: adopt current distance.
        Camera* cam = m_mapRenderer->GetCamera();
        XMFLOAT3 camPos = cam->GetPosition3f();

        float sx, sy, sz;
        InterpolateAgentPosition(ard, m_debugTimeline, m_replayCtx.interpSettings, sx, sy, sz);
        XMFLOAT3 agentWorld = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);

        float dx = camPos.x - agentWorld.x;
        float dy = camPos.y - agentWorld.y;
        float dz = camPos.z - agentWorld.z;
        float distToAgent = sqrtf(dx * dx + dy * dy + dz * dz);

        m_followDist       = distToAgent;
        m_followDistTarget = distToAgent;
        m_followYaw        = atan2f(dx, dz);
        m_followPitch      = (distToAgent > 0.001f)
            ? asinf(std::clamp(dy / distToAgent, -1.f, 1.f)) : 0.3f;

        m_followTransActive = false;
    }

    m_followedAgentId = agentId;
    m_cameraMode = CameraMode::FollowAgent;
    m_mapRenderer->m_disableMovementInput = true;
    m_leftClickPending = false;
}

void ReplayWindow::ExitFollowMode()
{
    m_cameraMode = CameraMode::Free;
    m_followedAgentId = -1;
    m_followTransActive = false;
    m_mapRenderer->m_disableMovementInput = false;
}

// ---------------------------------------------------------------------------
// Top View mode
// ---------------------------------------------------------------------------

XMFLOAT3 ReplayWindow::ComputeTopViewPosition() const
{
    Terrain* terrain = m_mapRenderer ? m_mapRenderer->GetTerrain() : nullptr;
    if (!terrain)
        return XMFLOAT3(0.f, 5000.f, 0.f);

    const auto& b = terrain->m_bounds;
    float cx = (b.map_min_x + b.map_max_x) * 0.5f;
    float cz = (b.map_min_z + b.map_max_z) * 0.5f;

    float mapW = b.map_max_x - b.map_min_x;
    float mapH = b.map_max_z - b.map_min_z;
    if (mapW < 1.f) mapW = 10000.f;
    if (mapH < 1.f) mapH = 10000.f;

    // Add 5% padding
    mapW *= 1.10f;
    mapH *= 1.10f;

    Camera* cam = m_mapRenderer->GetCamera();
    float fovY  = cam->GetFovY();
    float aspect = cam->GetAspectRatio();
    if (fovY <= 0.f)  fovY  = 50.f * XM_PI / 180.f;
    if (aspect <= 0.f) aspect = 16.f / 9.f;

    float halfFovY = fovY * 0.5f;
    float halfFovX = atanf(tanf(halfFovY) * aspect);

    float hFromW = (mapW * 0.5f) / tanf(halfFovX);
    float hFromH = (mapH * 0.5f) / tanf(halfFovY);
    float height = std::max(hFromW, hFromH);

    // Ensure minimum height and add vertical offset above terrain
    float maxTerrainY = std::max(std::abs(b.map_min_y), std::abs(b.map_max_y));
    height = std::max(height, 2000.f) + maxTerrainY;

    return XMFLOAT3(cx, height, cz);
}

void ReplayWindow::EnterTopView()
{
    if (m_topViewActive) return;

    Camera* cam = m_mapRenderer->GetCamera();

    // Save current camera state
    m_tvSavedPos   = cam->GetPosition3f();
    m_tvSavedYaw   = cam->GetYaw();
    m_tvSavedPitch = cam->GetPitch();
    m_tvSavedCamMode     = m_cameraMode;
    m_tvSavedFollowId    = m_followedAgentId;
    m_tvSavedFollowDist  = m_followDist;
    m_tvSavedFollowYaw   = m_followYaw;
    m_tvSavedFollowPitch = m_followPitch;
    m_tvSavedFogPerspective = m_fogPerspective;
    m_tvSavedSkillIcons   = m_showSkillIcons;
    m_tvSavedSkillLasers  = m_showSkillLasers;
    m_tvSavedLodEnabled   = m_uiLayout.lodEnabled;
    m_tvSavedShow3DLabels = m_show3DLabels;
    m_tvSavedNamePanel    = m_showNameFilterPanel;

    // Pause auto camera if active
    m_tvSavedAutoCam = m_autoCameraEnabled;
    if (m_autoCameraEnabled)
        m_autoCameraEnabled = false;

    // Exit follow mode so we have direct camera control
    if (m_cameraMode == CameraMode::FollowAgent)
        ExitFollowMode();

    m_topViewActive = true;
    m_mapRenderer->m_disableMovementInput = true;

    // Suppress fog
    if (m_fogPerspective > 0)
    {
        m_fogLastActive = m_fogPerspective;
        m_fogPerspective = 0;
    }

    // Override display settings for top view
    m_showSkillIcons      = false;
    m_showSkillLasers     = false;
    m_uiLayout.lodEnabled = true;
    m_show3DLabels        = false;
    m_showNameFilterPanel = true;

    // Set up transition
    m_topViewTransitioning = true;
    m_topViewTransTimer    = 0.f;
    m_tvTransFrom      = cam->GetPosition3f();
    m_tvTransFromYaw   = cam->GetYaw();
    m_tvTransFromPitch = cam->GetPitch();
    m_tvTransTo        = ComputeTopViewPosition();
    m_tvTransToYaw     = 0.f;
    m_tvTransToPitch   = -XM_PIDIV2 + 0.001f;  // Looking straight down
}

void ReplayWindow::ExitTopView()
{
    if (!m_topViewActive) return;

    Camera* cam = m_mapRenderer->GetCamera();

    // Set up transition from current position back to saved
    m_topViewTransitioning = true;
    m_topViewTransTimer    = 0.f;
    m_tvTransFrom      = cam->GetPosition3f();
    m_tvTransFromYaw   = cam->GetYaw();
    m_tvTransFromPitch = cam->GetPitch();
    m_tvTransTo        = m_tvSavedPos;
    m_tvTransToYaw     = m_tvSavedYaw;
    m_tvTransToPitch   = m_tvSavedPitch;

    m_topViewActive = false;
    m_mapRenderer->m_disableMovementInput = false;

    // Restore display settings
    m_fogPerspective      = m_tvSavedFogPerspective;
    m_showSkillIcons      = m_tvSavedSkillIcons;
    m_showSkillLasers     = m_tvSavedSkillLasers;
    m_uiLayout.lodEnabled = m_tvSavedLodEnabled;
    m_show3DLabels        = m_tvSavedShow3DLabels;
    m_showNameFilterPanel = m_tvSavedNamePanel;

    // Restore follow mode if was active
    if (m_tvSavedCamMode == CameraMode::FollowAgent && m_tvSavedFollowId >= 0)
    {
        EnterFollowMode(m_tvSavedFollowId);
        m_followDist       = m_tvSavedFollowDist;
        m_followDistTarget = m_tvSavedFollowDist;
        m_followYaw        = m_tvSavedFollowYaw;
        m_followPitch      = m_tvSavedFollowPitch;
    }

    // Restore auto camera
    if (m_tvSavedAutoCam)
    {
        m_autoCameraEnabled = true;
        m_autoCamState = AutoCameraState{};
    }
}

void ReplayWindow::UpdateTopViewTransition(float dt)
{
    if (!m_topViewTransitioning) return;

    m_topViewTransTimer += dt;
    float t = std::clamp(m_topViewTransTimer / m_topViewTransDuration, 0.f, 1.f);

    // Ease-in-out cubic
    float ease = t < 0.5f
        ? 4.f * t * t * t
        : 1.f - 0.5f * powf(-2.f * t + 2.f, 3.f);

    Camera* cam = m_mapRenderer->GetCamera();

    // Lerp position
    float lx = m_tvTransFrom.x + (m_tvTransTo.x - m_tvTransFrom.x) * ease;
    float ly = m_tvTransFrom.y + (m_tvTransTo.y - m_tvTransFrom.y) * ease;
    float lz = m_tvTransFrom.z + (m_tvTransTo.z - m_tvTransFrom.z) * ease;
    cam->SetPosition(lx, ly, lz);

    // Lerp orientation (short-path yaw interpolation)
    float dyaw = m_tvTransToYaw - m_tvTransFromYaw;
    while (dyaw >  XM_PI) dyaw -= XM_2PI;
    while (dyaw < -XM_PI) dyaw += XM_2PI;
    float curYaw   = m_tvTransFromYaw + dyaw * ease;
    float curPitch = m_tvTransFromPitch + (m_tvTransToPitch - m_tvTransFromPitch) * ease;
    cam->SetOrientation(curPitch, curYaw);

    if (t >= 1.f)
    {
        m_topViewTransitioning = false;
        // Snap to exact target values
        cam->SetPosition(m_tvTransTo.x, m_tvTransTo.y, m_tvTransTo.z);
        cam->SetOrientation(m_tvTransToPitch, m_tvTransToYaw);
    }
}

void ReplayWindow::UpdateFollowCamera(float dt)
{
    if (m_cameraMode != CameraMode::FollowAgent) return;

    auto it = m_replayCtx.agents.find(m_followedAgentId);
    if (it == m_replayCtx.agents.end()) { ExitFollowMode(); return; }

    const auto& ard = it->second;
    if (ard.snapshots.empty()) { ExitFollowMode(); return; }

    if (ard.isDeadAtTime(m_debugTimeline) &&
        m_debugTimeline > ard.snapshots.back().time)
    {
        ExitFollowMode();
        return;
    }

    // Smooth zoom toward target distance
    float t = 1.0f - expf(-kFollowLerpSpeed * dt);
    m_followDist += (m_followDistTarget - m_followDist) * t;
    m_followDist = std::clamp(m_followDist, kFollowMinDist, kFollowMaxDist);

    // New agent (target) world position
    float sx, sy, sz;
    InterpolateAgentPosition(ard, m_debugTimeline, m_replayCtx.interpSettings, sx, sy, sz);
    XMFLOAT3 newCenter = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);

    // Orbit center: blend from old agent to new agent during transition
    XMFLOAT3 center = newCenter;

    if (m_followTransActive)
    {
        m_followTransElapsed += dt;
        float u = std::clamp(m_followTransElapsed / kFollowTransDuration, 0.f, 1.f);

        // Smooth-step (cubic Hermite) for a gentle, broadcast-camera pan
        float ease = u * u * (3.f - 2.f * u);

        if (u >= 1.f)
        {
            m_followTransActive = false;
        }
        else
        {
            auto itOld = m_replayCtx.agents.find(m_followTransFromAgentId);
            if (itOld != m_replayCtx.agents.end() && !itOld->second.snapshots.empty())
            {
                float ox, oy, oz;
                InterpolateAgentPosition(itOld->second, m_debugTimeline,
                    m_replayCtx.interpSettings, ox, oy, oz);
                XMFLOAT3 oldCenter = ApplyMapTransformToPos(ox, oy, oz, m_replayCtx.mapTransform);

                center.x = oldCenter.x + (newCenter.x - oldCenter.x) * ease;
                center.y = oldCenter.y + (newCenter.y - oldCenter.y) * ease;
                center.z = oldCenter.z + (newCenter.z - oldCenter.z) * ease;
            }
        }
    }

    // Spherical offset from orbit center
    float cosP = cosf(m_followPitch);
    float offX = m_followDist * sinf(m_followYaw) * cosP;
    float offY = m_followDist * sinf(m_followPitch);
    float offZ = m_followDist * cosf(m_followYaw) * cosP;

    Camera* cam = m_mapRenderer->GetCamera();
    cam->SetPosition(center.x + offX, center.y + offY, center.z + offZ);
    cam->SetOrientation(-m_followPitch, m_followYaw + XM_PI);
}

// ---------------------------------------------------------------------------
// Auto Camera system
// ---------------------------------------------------------------------------

static const int kRezSkillIds[] = { 2, 305, 306, 314, 315, 791, 1263, 1481, 1778 };

// Returns projected HP as fraction 0.0–1.0, and optionally the damage rate
float ReplayWindow::EstimateProjectedHp(const AgentReplayData& ard, float time, float lookahead) const
{
    return EstimateProjectedHpEx(ard, time, lookahead, nullptr);
}

float ReplayWindow::EstimateProjectedHpEx(const AgentReplayData& ard, float time, float lookahead, float* outDmgRate) const
{
    const auto& combatEvents = m_replayCtx.stocData.combat;
    float totalDmgFrac = 0.f;
    const float window = 3.f;
    for (const auto& ce : combatEvents)
    {
        if (ce.target_id != ard.agent_id) continue;
        if (ce.type != "DAMAGE") continue;
        if (ce.time > time || ce.time < time - window) continue;
        if (ce.value < 0.f)
            totalDmgFrac += -ce.value;
    }
    float dmgRate = totalDmgFrac / window;
    if (outDmgRate) *outDmgRate = dmgRate;

    const AgentSnapshot* snap = FindSnapshotAtTime(ard, time);
    if (!snap) return 1.f;

    float projected = snap->health_pct - dmgRate * lookahead;
    return std::clamp(projected, 0.f, 1.f);
}

bool ReplayWindow::WillAgentDie(const AgentReplayData& ard, float time, float lookahead) const
{
    const auto& snaps = ard.snapshots;
    if (snaps.empty()) return false;

    // Binary search to find the first snapshot at or after 'time'
    auto it = std::lower_bound(snaps.begin(), snaps.end(), time,
        [](const AgentSnapshot& s, float t) { return s.time < t; });

    float deadline = time + lookahead;
    for (; it != snaps.end() && it->time <= deadline; ++it)
    {
        if (it->is_dead) return true;
    }
    return false;
}


bool ReplayWindow::IsAgentTakingDamage(int agentId, float time, float window) const
{
    for (const auto& ce : m_replayCtx.stocData.combat)
    {
        if (ce.target_id != agentId) continue;
        if (ce.type != "DAMAGE") continue;
        if (ce.time <= time && ce.time >= time - window)
            return true;
    }
    return false;
}

bool ReplayWindow::IsAgentIsolated(const AgentReplayData& ard, float time, float radius) const
{
    const AgentSnapshot* snap = FindSnapshotAtTime(ard, time);
    if (!snap) return false;

    float ax = snap->x, ay = snap->y;
    float radiusSq = radius * radius;
    const auto& teammates = (ard.teamId == 1) ? m_team1PlayerIds : m_team2PlayerIds;

    int nearbyCount = 0;
    for (int pid : teammates)
    {
        if (pid == ard.agent_id) continue;
        auto it = m_replayCtx.agents.find(pid);
        if (it == m_replayCtx.agents.end()) continue;
        const AgentSnapshot* ps = FindSnapshotAtTime(it->second, time);
        if (!ps || ps->is_dead) continue;
        float dx = ps->x - ax, dy = ps->y - ay;
        if (dx * dx + dy * dy < radiusSq) nearbyCount++;
    }
    return nearbyCount == 0;
}

bool ReplayWindow::IsAgentCastingRez(const AgentReplayData& ard, float time) const
{
    for (const auto& su : ard.skillUseHistory)
    {
        if (su.wasCancelled) continue;
        if (time < su.startTime || time > su.endTime) continue;
        for (int rid : kRezSkillIds)
        {
            if (su.skillId == rid) return true;
        }
    }
    return false;
}

void ReplayWindow::UpdateAutoCamera(float dt)
{
    if (!m_autoCameraEnabled) return;
    if (!m_agentsClassified) return;

    auto& st = m_autoCamState;
    auto& cfg = m_autoCamCfg;

    // Only advance timers while the replay is playing
    if (m_replayCtx.isPlaying)
    {
        st.dwellTimer += dt;
        st.switchCooldown = std::max(0.f, st.switchCooldown - dt);
    }

    float now = m_debugTimeline;

    m_autoCamDebug.clear();

    int bestAgent = -1;
    int bestScore = 0;
    std::string bestReason;

    // Re-score the current target so hysteresis uses live score, not stale priority
    int currentTargetLiveScore = 0;

    auto scoreAgent = [&](int pid, const AgentReplayData& ard) {
        const AgentSnapshot* snap = FindSnapshotAtTime(ard, now);

        AutoCamDebugEntry dbg;
        dbg.agentId = pid;
        dbg.name = ard.partyBarLabel.empty() ? std::to_string(pid) : ard.partyBarLabel;

        if (!snap) {
            dbg.disqualified = true;
            dbg.disqualReason = "No snapshot";
            m_autoCamDebug.push_back(std::move(dbg));
            return;
        }
        if (snap->is_dead) {
            dbg.hpPct = 0.f;
            dbg.disqualified = true;
            dbg.disqualReason = "Dead";
            m_autoCamDebug.push_back(std::move(dbg));
            return;
        }

        dbg.hpPct = snap->health_pct;

        int score = 0;
        std::string reason;

        // Use actual future snapshot for projected HP
        const AgentSnapshot* futureSnap = FindSnapshotAtTime(ard, now + cfg.lookaheadSec);
        float futureHp = futureSnap ? futureSnap->health_pct : snap->health_pct;
        dbg.projectedHp = futureHp;
        dbg.dmgRate = (futureSnap && futureSnap->time > snap->time)
            ? std::max(0.f, snap->health_pct - futureHp) / (futureSnap->time - snap->time)
            : 0.f;

        // Death imminent: check actual future snapshots for a real death
        if (cfg.focusDeath && WillAgentDie(ard, now, cfg.lookaheadSec))
        {
            score = std::max(score, 1000);
            reason = std::format("Death imminent (HP {:.0f}%)", snap->health_pct * 100.f);
        }

        // Low HP: use actual future HP from replay data
        if (cfg.focusLowHp && futureHp < cfg.hpThreshold &&
            futureHp < snap->health_pct)
        {
            score = std::max(score, 700);
            if (reason.empty()) reason = std::format("Low HP ({:.0f}% -> {:.0f}%)", snap->health_pct * 100.f, futureHp * 100.f);
        }

        if (cfg.focusRez && IsAgentCastingRez(ard, now))
        {
            score = std::max(score, 500);
            if (reason.empty()) reason = "Casting resurrection";
        }

        if (cfg.focusIsolated && IsAgentIsolated(ard, now) && IsAgentTakingDamage(pid, now))
        {
            score = std::max(score, 400);
            if (reason.empty()) reason = "Isolated + taking damage";
        }

        if (cfg.focusFlagCarry && snap->weapon_type == 0)
        {
            BundleType bt = GetPlayerBundleType(pid, now);
            if (bt == BundleType::Flag || bt == BundleType::Unknown)
            {
                score = std::max(score, 200);
                if (reason.empty()) reason = "Carrying flag";
            }
        }

        dbg.score = score;
        dbg.reason = reason;
        m_autoCamDebug.push_back(std::move(dbg));

        if (pid == st.currentTarget)
            currentTargetLiveScore = score;

        if (score > bestScore)
        {
            bestScore = score;
            bestAgent = pid;
            bestReason = reason;
        }
    };

    for (int pid : m_team1PlayerIds)
    {
        auto it = m_replayCtx.agents.find(pid);
        if (it != m_replayCtx.agents.end())
            scoreAgent(pid, it->second);
    }
    for (int pid : m_team2PlayerIds)
    {
        auto it = m_replayCtx.agents.find(pid);
        if (it != m_replayCtx.agents.end())
            scoreAgent(pid, it->second);
    }

    // Score guild lords
    if (cfg.focusLord)
    {
        for (int nid : m_npcIds)
        {
            auto it = m_replayCtx.agents.find(nid);
            if (it == m_replayCtx.agents.end()) continue;
            if (it->second.categoryName != "Guild Lord") continue;
            const AgentSnapshot* snap = FindSnapshotAtTime(it->second, now);
            if (!snap || snap->is_dead) continue;
            if (IsAgentTakingDamage(nid, now))
            {
                int lordScore = 700;
                AutoCamDebugEntry dbg;
                dbg.agentId = nid;
                dbg.name = "Guild Lord";
                dbg.hpPct = snap->health_pct;
                dbg.score = lordScore;
                dbg.reason = "Guild Lord under attack";
                m_autoCamDebug.push_back(std::move(dbg));

                if (nid == st.currentTarget)
                    currentTargetLiveScore = lordScore;

                if (lordScore > bestScore)
                {
                    bestScore = lordScore;
                    bestAgent = nid;
                    bestReason = "Guild Lord under attack";
                }
            }
        }
    }

    // Score flag pickup
    if (cfg.focusFlag && m_flagTimelineBuilt && m_flagTimeline.valid)
    {
        for (int ti = 0; ti < 2; ++ti)
        {
            for (const auto& fe : m_flagTimeline.teams[ti].events)
            {
                if (fe.newLocation == FlagLocation::Carried &&
                    fe.carrierAgentId >= 0 &&
                    std::abs(fe.time - now) < 2.f)
                {
                    if (500 > bestScore)
                    {
                        bestScore = 500;
                        bestAgent = fe.carrierAgentId;
                        bestReason = "Flag pickup";
                    }
                }
            }
        }
    }

    if (bestAgent < 0) return;

    // Switch decision — use live score for current target (not stale priority)
    bool shouldSwitch = false;
    if (st.currentTarget < 0)
    {
        shouldSwitch = true;
    }
    else if (bestScore >= 900 && bestAgent != st.currentTarget)
    {
        // Priority 1 events bypass both dwell and hysteresis
        shouldSwitch = true;
    }
    else if (st.dwellTimer < cfg.minDwellTime)
    {
        // Still dwelling, no switch for non-P1
    }
    else if (st.switchCooldown <= 0.f && bestAgent != st.currentTarget)
    {
        float refScore = static_cast<float>(std::max(currentTargetLiveScore, 1));
        if (static_cast<float>(bestScore) > refScore * 1.10f)
            shouldSwitch = true;
    }

    if (shouldSwitch && bestAgent != st.currentTarget)
    {
        Camera* cam = m_mapRenderer->GetCamera();
        st.lerpFrom = cam->GetPosition3f();
        st.lerpActive = true;
        st.lerpElapsed = 0.f;
        st.lerpDuration = (bestScore >= 900) ? 1.2f : 2.5f;

        st.currentTarget = bestAgent;
        st.currentPriority = bestScore;
        st.currentReason = bestReason;
        st.dwellTimer = 0.f;
        st.switchCooldown = 1.f;

        if (m_cameraMode != CameraMode::FollowAgent || m_followedAgentId != bestAgent)
        {
            EnterFollowMode(bestAgent);
        }
    }

    // Smooth camera lerp transition
    if (st.lerpActive && m_cameraMode == CameraMode::FollowAgent)
    {
        if (m_replayCtx.isPlaying)
            st.lerpElapsed += dt;
        float t = std::clamp(st.lerpElapsed / st.lerpDuration, 0.f, 1.f);
        // Ease-in-out quintic for a smoother, more cinematic camera glide
        float ease = t < 0.5f
            ? 16.f * t * t * t * t * t
            : 1.f - 0.5f * powf(-2.f * t + 2.f, 5.f);

        if (t >= 1.f)
        {
            st.lerpActive = false;
        }
        else
        {
            auto it = m_replayCtx.agents.find(m_followedAgentId);
            if (it != m_replayCtx.agents.end())
            {
                float sx, sy, sz;
                InterpolateAgentPosition(it->second, m_debugTimeline,
                    m_replayCtx.interpSettings, sx, sy, sz);
                XMFLOAT3 agentWorld = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);

                float cosP = cosf(m_followPitch);
                float offX = m_followDist * sinf(m_followYaw) * cosP;
                float offY = m_followDist * sinf(m_followPitch);
                float offZ = m_followDist * cosf(m_followYaw) * cosP;

                XMFLOAT3 targetPos(agentWorld.x + offX, agentWorld.y + offY, agentWorld.z + offZ);

                float lx = st.lerpFrom.x + (targetPos.x - st.lerpFrom.x) * ease;
                float ly = st.lerpFrom.y + (targetPos.y - st.lerpFrom.y) * ease;
                float lz = st.lerpFrom.z + (targetPos.z - st.lerpFrom.z) * ease;

                Camera* cam = m_mapRenderer->GetCamera();
                cam->SetPosition(lx, ly, lz);
            }
        }
    }
}

void ReplayWindow::LoadAutoCamSettings()
{
    auto& cfg = m_autoCamCfg;
    cfg.lookaheadSec = GuiGlobalConstants::autocam_lookahead;
    cfg.hpThreshold  = GuiGlobalConstants::autocam_hp_thresh / 100.f;
    cfg.minDwellTime = GuiGlobalConstants::autocam_dwell;
    cfg.focusDeath     = GuiGlobalConstants::autocam_death;
    cfg.focusLowHp     = GuiGlobalConstants::autocam_low_hp;
    cfg.focusLord      = GuiGlobalConstants::autocam_lord;
    cfg.focusFlag      = GuiGlobalConstants::autocam_flag;
    cfg.focusRez       = GuiGlobalConstants::autocam_rez;
    cfg.focusIsolated  = GuiGlobalConstants::autocam_isolated;
    cfg.focusFlagCarry = GuiGlobalConstants::autocam_flag_carry;
}

void ReplayWindow::SaveAutoCamSettings()
{
    auto& cfg = m_autoCamCfg;
    GuiGlobalConstants::autocam_lookahead  = cfg.lookaheadSec;
    GuiGlobalConstants::autocam_hp_thresh  = static_cast<int>(cfg.hpThreshold * 100.f);
    GuiGlobalConstants::autocam_dwell      = cfg.minDwellTime;
    GuiGlobalConstants::autocam_death      = cfg.focusDeath;
    GuiGlobalConstants::autocam_low_hp     = cfg.focusLowHp;
    GuiGlobalConstants::autocam_lord       = cfg.focusLord;
    GuiGlobalConstants::autocam_flag       = cfg.focusFlag;
    GuiGlobalConstants::autocam_rez        = cfg.focusRez;
    GuiGlobalConstants::autocam_isolated   = cfg.focusIsolated;
    GuiGlobalConstants::autocam_flag_carry = cfg.focusFlagCarry;
    GuiGlobalConstants::SaveSettings();
}

void ReplayWindow::DrawAutoCameraPanel()
{
    if (!m_showAutoCameraPanel) return;

    auto& cfg = m_autoCamCfg;
    auto& st  = m_autoCamState;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,      ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.10f, 0.10f, 0.12f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.15f, 0.14f, 0.12f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,     ImVec4(0.78f, 0.65f, 0.29f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,      ImVec4(1.f, 0.84f, 0.39f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Header,         ImVec4(0.18f, 0.14f, 0.05f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.23f, 0.19f, 0.08f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);

    auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.f, 0.f), ImVec2(vp->Size.x, vp->Size.y));
    ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
    m_panelLayout.ApplyPosition("auto_camera");

    if (!ImGui::Begin("Auto Camera", &m_showAutoCameraPanel,
        ImGuiWindowFlags_AlwaysAutoResize))
    {
        m_panelLayout.TrackWindow("auto_camera");
        ImGui::End();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(11);
        return;
    }

    m_panelLayout.TrackWindow("auto_camera");

    {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, vp->Pos.x, vp->Pos.x + vp->Size.x - sz.x);
        float cy = std::clamp(pos.y, vp->Pos.y, vp->Pos.y + vp->Size.y - sz.y);
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));
    }

    const ImVec4 goldText(0.78f, 0.72f, 0.55f, 1.f);
    const ImVec4 accentText(1.f, 0.91f, 0.69f, 1.f);
    const ImVec4 dimText(0.60f, 0.64f, 0.69f, 1.f);

    bool wasEnabled = m_autoCameraEnabled;
    ImGui::Checkbox("Enable Auto Camera (A)", &m_autoCameraEnabled);
    if (m_autoCameraEnabled && !wasEnabled)
        st = AutoCameraState{};
    if (!m_autoCameraEnabled && wasEnabled)
        ExitFollowMode();

    if (m_topViewActive && m_tvSavedAutoCam)
        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), "Paused (Top View active)");

    // ── Focus pills ──────────────────────────────────────────────────────
    ImGui::Separator();
    ImGui::TextColored(goldText, "Focus");

    auto FocusPill = [&](const char* label, bool& active) {
        ImVec4 bg, tx, hov, bdr;
        if (active) {
            bg  = ImVec4(0.18f, 0.14f, 0.05f, 1.f);
            tx  = ImVec4(1.f, 0.91f, 0.69f, 1.f);
            hov = ImVec4(0.23f, 0.19f, 0.08f, 1.f);
            bdr = ImVec4(1.f, 0.84f, 0.39f, 0.85f);
        } else {
            bg  = ImVec4(1.f, 1.f, 1.f, 0.05f);
            tx  = ImVec4(0.60f, 0.64f, 0.69f, 1.f);
            hov = ImVec4(1.f, 1.f, 1.f, 0.12f);
            bdr = ImVec4(1.f, 1.f, 1.f, 0.08f);
        }
        ImGui::PushStyleColor(ImGuiCol_Button, bg);
        ImGui::PushStyleColor(ImGuiCol_Text, tx);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            ImVec4(bg.x * 0.85f, bg.y * 0.85f, bg.z * 0.85f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border, bdr);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        if (ImGui::Button(label))
            active = !active;
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);
    };

    FocusPill("Death", cfg.focusDeath);
    ImGui::SameLine();
    FocusPill("Low HP", cfg.focusLowHp);
    ImGui::SameLine();
    FocusPill("Guild Lord", cfg.focusLord);
    ImGui::SameLine();
    FocusPill("Flag", cfg.focusFlag);

    FocusPill("Resurrection", cfg.focusRez);
    ImGui::SameLine();
    FocusPill("Isolated", cfg.focusIsolated);
    ImGui::SameLine();
    FocusPill("Flag Carrier", cfg.focusFlagCarry);

    // ── Settings (collapsible) ───────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Settings"))
    {
        const char* lookaheadLabels[] = { "2s", "3s", "5s", "10s" };
        const float lookaheadValues[] = { 2.f, 3.f, 5.f, 10.f };
        int lookaheadIdx = 1;
        for (int i = 0; i < 4; ++i) if (cfg.lookaheadSec == lookaheadValues[i]) lookaheadIdx = i;
        if (ImGui::Combo("Lookahead", &lookaheadIdx, lookaheadLabels, 4))
            cfg.lookaheadSec = lookaheadValues[lookaheadIdx];

        float hpPct = cfg.hpThreshold * 100.f;
        if (ImGui::SliderFloat("HP Threshold", &hpPct, 10.f, 80.f, "%.0f%%"))
            cfg.hpThreshold = hpPct / 100.f;

        ImGui::SliderFloat("Min Dwell", &cfg.minDwellTime, 1.f, 8.f, "%.1fs");
    }

    (void)accentText;
    (void)dimText;

    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(11);
}

void ReplayWindow::DrawAutoCameraDebugPanel()
{
    if (!m_autoCamShowDebug) return;

    auto& st = m_autoCamState;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));

    ImGui::SetNextWindowSize(ImVec2(520, 340), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Auto Camera Debug", &m_autoCamShowDebug))
    {
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(4);
        return;
    }

    auto& cfg = m_autoCamCfg;
    const ImVec4 accentText(1.f, 0.91f, 0.69f, 1.f);
    const ImVec4 dimText(0.60f, 0.64f, 0.69f, 1.f);
    const ImVec4 goldText(0.78f, 0.72f, 0.55f, 1.f);

    // ── Status ──────────────────────────────────────────────────────────
    ImGui::TextColored(goldText, "Status");
    if (m_autoCameraEnabled && st.currentTarget >= 0)
    {
        auto it = m_replayCtx.agents.find(st.currentTarget);
        if (it != m_replayCtx.agents.end())
        {
            const auto& ard = it->second;
            const AgentSnapshot* snap = FindSnapshotAtTime(ard, m_debugTimeline);
            std::string name = ard.partyBarLabel.empty() ? ard.playerName : ard.partyBarLabel;
            ImGui::TextColored(accentText, "Watching: %s", name.c_str());
            if (snap)
            {
                float pct = snap->health_pct;
                ImGui::ProgressBar(pct, ImVec2(-1, 14),
                    std::format("{:.0f}%", pct * 100.f).c_str());
            }
            ImGui::TextColored(ImVec4(0.8f, 0.7f, 0.3f, 1.f), "Reason: %s", st.currentReason.c_str());
            float remaining = std::max(0.f, cfg.minDwellTime - st.dwellTimer);
            ImGui::TextColored(dimText, "Next switch: in %.1fs", remaining);
        }
    }
    else if (m_autoCameraEnabled)
        ImGui::TextDisabled("No target selected");
    else
        ImGui::TextDisabled("Auto camera OFF");

    ImGui::Separator();
    ImGui::TextColored(accentText, "Agent Scores (%d agents)", (int)m_autoCamDebug.size());

    if (ImGui::BeginTable("##acdbg", 6,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
        ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.f)))
    {
        ImGui::TableSetupColumn("Agent", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("HP%",   ImGuiTableColumnFlags_WidthFixed, 44.f);
        ImGui::TableSetupColumn("Dmg/s", ImGuiTableColumnFlags_WidthFixed, 50.f);
        ImGui::TableSetupColumn("Proj",  ImGuiTableColumnFlags_WidthFixed, 44.f);
        ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 44.f);
        ImGui::TableSetupColumn("Info",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        auto sorted = m_autoCamDebug;
        std::sort(sorted.begin(), sorted.end(),
            [](const AutoCamDebugEntry& a, const AutoCamDebugEntry& b) { return a.score > b.score; });

        for (const auto& e : sorted)
        {
            ImGui::TableNextRow();

            bool isCurrent = (e.agentId == st.currentTarget);
            if (isCurrent)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.f, 0.3f, 1.f));

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(e.name.c_str());

            ImGui::TableSetColumnIndex(1);
            if (e.disqualified)
                ImGui::TextDisabled("--");
            else
                ImGui::Text("%.0f", e.hpPct * 100.f);

            ImGui::TableSetColumnIndex(2);
            if (e.disqualified)
                ImGui::TextDisabled("--");
            else
                ImGui::Text("%.1f%%", e.dmgRate * 100.f);

            ImGui::TableSetColumnIndex(3);
            if (e.disqualified)
                ImGui::TextDisabled("--");
            else
            {
                if (e.score >= 1000)
                    ImGui::TextColored(ImVec4(1.f, 0.2f, 0.2f, 1.f), "DIES");
                else
                    ImGui::Text("%.0f%%", e.projectedHp * 100.f);
            }

            ImGui::TableSetColumnIndex(4);
            if (e.score >= 900)
                ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "%d", e.score);
            else if (e.score > 0)
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "%d", e.score);
            else
                ImGui::TextDisabled("0");

            ImGui::TableSetColumnIndex(5);
            if (e.disqualified)
                ImGui::TextDisabled("%s", e.disqualReason.c_str());
            else if (!e.reason.empty())
                ImGui::TextUnformatted(e.reason.c_str());

            if (isCurrent)
                ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }

    ImGui::TextColored(dimText,
        "Dwell: %.2f  Cooldown: %.2f  Hysteresis: 1.10x",
        st.dwellTimer, st.switchCooldown);

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
}
