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


void ReplayWindow::InitAudioEngine()
{
    if (m_audioInitialized || !m_datManager) return;

    m_audioEngine = std::make_unique<SpatialAudioEngine>();
    auto jsonPath = GetSkillSoundsFilePath();
    if (!std::filesystem::exists(jsonPath)) {
        OutputDebugStringA(("[Audio] skill_sounds.json not found at: " + jsonPath.string() + "\n").c_str());
        return;
    }

    if (!m_audioEngine->Init(m_datManager, m_hashIndex, jsonPath.string())) {
        OutputDebugStringA("[Audio] SpatialAudioEngine init failed\n");
        m_audioEngine.reset();
        return;
    }

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
        XMMATRIX view = cam->GetView();

        // Project camera forward onto the horizontal XZ plane so that
        // screen-space left/right maps to speaker left/right regardless of camera pitch.
        XMFLOAT4X4 v4;
        XMStoreFloat4x4(&v4, view);
        XMFLOAT3 flatFront = { -v4._31, 0.0f, -v4._33 };
        float len = sqrtf(flatFront.x * flatFront.x + flatFront.z * flatFront.z);
        if (len > 0.001f) { flatFront.x /= len; flatFront.z /= len; }
        else { flatFront = { 0.f, 0.f, 1.f }; }
        XMFLOAT3 up = { 0.f, 1.f, 0.f };

        m_audioEngine->UpdateListener(pos, flatFront, up, dt);
    }

    // Detect scrub/seek: if timeline jumped backwards or forward significantly, reset cursors
    bool seeked = (m_audioLastTime > currentTime + 0.01f) ||
                  (currentTime - m_audioLastTime > 2.0f);
    if (seeked) {
        m_audioSkillCursor.clear();
        m_audioEngine->StopAll();
        // m_targetOrderCursor is reset via binary search in the target loop below
    }

    float prevTime = m_audioLastTime;
    m_audioLastTime = currentTime;

    if (prevTime < 0.f || seeked) return;

    // Helper: find agent position at a given time via binary search on snapshots
    auto findAgentPos = [](const AgentReplayData& ard, float t, float& ox, float& oy, float& oz) {
        ox = oy = oz = 0.f;
        if (ard.snapshots.empty()) return;
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
        ox = snaps[idx].x;
        oy = snaps[idx].y;
        oz = snaps[idx].z;
    };

    // --- Caster sounds: trigger at cast startTime, positioned at caster ---
    for (auto& [agentId, ard] : m_replayCtx.agents) {
        if (ard.skillUseHistory.empty()) continue;

        auto [it, inserted] = m_audioSkillCursor.try_emplace(agentId, 0);
        size_t& cursor = it->second;

        if (inserted) {
            size_t lo = 0, hi = ard.skillUseHistory.size();
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (ard.skillUseHistory[mid].startTime <= prevTime)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            cursor = lo;
        }

        while (cursor < ard.skillUseHistory.size()) {
            const auto& ev = ard.skillUseHistory[cursor];
            if (ev.startTime > currentTime) break;

            if (ev.startTime > prevTime) {
                float ax, ay, az;
                findAgentPos(ard, ev.startTime, ax, ay, az);

                SoundEvent snd;
                snd.category = SoundCategory::SKILL_CAST;
                snd.skill_id = static_cast<uint32_t>(ev.skillId);
                snd.source_agent_id = agentId;
                m_audioEngine->Post(snd, ax, ay, az);
            }
            cursor++;
        }
    }

    // --- Target sounds: trigger at cast endTime, positioned at target agent ---
    // Build a global timeline of (casterId, eventIdx) sorted by endTime once.
    // Includes all non-cancelled events (self-target uses caster position).
    if (!m_targetOrderBuilt && m_skillUseTimelineBuilt) {
        m_targetEventOrder.clear();
        for (auto& [agentId, ard] : m_replayCtx.agents) {
            for (size_t i = 0; i < ard.skillUseHistory.size(); i++) {
                const auto& ev = ard.skillUseHistory[i];
                if (!ev.wasCancelled)
                    m_targetEventOrder.push_back({ agentId, i });
            }
        }
        std::sort(m_targetEventOrder.begin(), m_targetEventOrder.end(),
            [this](const auto& a, const auto& b) {
                float ea = m_replayCtx.agents.at(a.first).skillUseHistory[a.second].endTime;
                float eb = m_replayCtx.agents.at(b.first).skillUseHistory[b.second].endTime;
                return ea < eb;
            });
        m_targetOrderCursor = 0;
        m_targetOrderBuilt = true;
    }

    if (m_targetOrderBuilt) {
        if (seeked) {
            size_t lo = 0, hi = m_targetEventOrder.size();
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                auto& p = m_targetEventOrder[mid];
                float et = m_replayCtx.agents.at(p.first).skillUseHistory[p.second].endTime;
                if (et <= prevTime) lo = mid + 1; else hi = mid;
            }
            m_targetOrderCursor = lo;
        }

        while (m_targetOrderCursor < m_targetEventOrder.size()) {
            auto& [casterId, evIdx] = m_targetEventOrder[m_targetOrderCursor];
            const auto& ev = m_replayCtx.agents.at(casterId).skillUseHistory[evIdx];
            if (ev.endTime > currentTime) break;

            if (ev.endTime > prevTime) {
                // Resolve position: use target agent if available, otherwise caster
                float tx, ty, tz;
                int targetId = ev.targetId;
                auto tit = (targetId >= 0) ? m_replayCtx.agents.find(targetId) : m_replayCtx.agents.end();
                if (tit != m_replayCtx.agents.end()) {
                    findAgentPos(tit->second, ev.endTime, tx, ty, tz);
                } else {
                    // Self-target or unknown target: use caster position
                    auto cit = m_replayCtx.agents.find(casterId);
                    if (cit != m_replayCtx.agents.end())
                        findAgentPos(cit->second, ev.endTime, tx, ty, tz);
                    else
                        tx = ty = tz = 0.f;
                }

                SoundEvent snd;
                snd.category = SoundCategory::HIT;
                snd.skill_id = static_cast<uint32_t>(ev.skillId);
                snd.source_agent_id = casterId;
                m_audioEngine->Post(snd, tx, ty, tz);
            }
            m_targetOrderCursor++;
        }
    }
}
