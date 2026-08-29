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


void ReplayWindow::DrawAgentOverlay()
{
    if (!m_showAgentOverlay) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    Camera* cam = m_mapRenderer->GetCamera();
    XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    auto vp = m_deviceResources->GetScreenViewport();
    float vpW = vp.Width, vpH = vp.Height;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float dotRadius = 6.f;
    const float labelOffY = 8.f;
    const MapTransform& t = m_replayCtx.mapTransform;

    const bool canClickAgents = !ImGui::GetIO().WantCaptureMouse
                                && !m_rightMouseDown
                                && !m_annotationMgr.IsDrawModeActive();
    const ImVec2 mousePos = ImGui::GetIO().MousePos;
    const float clickRadius = 14.f;
    m_hoveredAgentId = -1;

    struct TopViewTooltipCandidate { float rectMinX, rectMinY, rectMaxX, rectMaxY; std::string name; float dist; };
    std::vector<TopViewTooltipCandidate> topViewTooltipCandidates;

    // Map boundary clamping: use terrain bounds if available.
    // Bounds are in GWMB mesh coordinates (post-transform), so we clamp after
    // applying the map transform.
    Terrain* terrain = m_mapRenderer->GetTerrain();
    bool hasBounds = (terrain != nullptr);
    float bMinX = 0, bMaxX = 0, bMinZ = 0, bMaxZ = 0;
    if (hasBounds) {
        bMinX = terrain->m_bounds.map_min_x;
        bMaxX = terrain->m_bounds.map_max_x;
        bMinZ = terrain->m_bounds.map_min_z;
        bMaxZ = terrain->m_bounds.map_max_z;
    }

    // Optional: draw origin axes
    if (m_showMapOriginAxes)
    {
        const float axisLen = 2000.f;
        struct { XMFLOAT3 end; ImU32 col; } axes[] = {
            { { axisLen, 0, 0 }, IM_COL32(255, 60, 60, 200) },
            { { 0, axisLen, 0 }, IM_COL32(60, 255, 60, 200) },
            { { 0, 0, axisLen }, IM_COL32(60, 100, 255, 200) },
        };
        float ox, oy;
        if (ProjectToScreen(viewProj, vpW, vpH, { 0, 0, 0 }, ox, oy))
        {
            for (auto& a : axes) {
                float ax, ay;
                if (ProjectToScreen(viewProj, vpW, vpH, a.end, ax, ay))
                    dl->AddLine(ImVec2(ox, oy), ImVec2(ax, ay), a.col, 2.f);
            }
        }
    }

    const InterpolationSettings& is = m_replayCtx.interpSettings;

    // --- Spirit overlap pass: determine which spirits are hidden ---
    // Group spirits by (team, model_id). Within each group, only the newest
    // spirit is unconditionally visible; older ones are hidden if they are
    // within 2.7 × the spirit's danger-zone radius of the newest.
    {
        struct SpiritEntry { int agentId; float spawnTime; float px, py; };
        // Key: (teamId << 32) | modelId
        std::unordered_map<uint64_t, std::vector<SpiritEntry>> groups;

        for (int id : m_spiritIds)
        {
            auto& ard = m_replayCtx.agents[id];
            ard.overlapHidden     = false;
            ard.overlapIsNewest   = false;
            ard.overlapDistNewest = 0.f;
            ard.overlapThreshold  = 0.f;

            if (ard.snapshots.empty()) continue;
            if (m_debugTimeline < ard.snapshots.front().time ||
                m_debugTimeline > ard.snapshots.back().time)
                continue;

            float sx, sy, sz;
            SnapAgentPosition(ard, m_debugTimeline, sx, sy, sz);

            uint64_t key = (static_cast<uint64_t>(ard.teamId) << 32) | ard.modelId;
            groups[key].push_back({ id, ard.snapshots.front().time, sx, sy });
        }

        for (auto& [key, entries] : groups)
        {
            if (entries.size() <= 1) {
                if (!entries.empty()) {
                    auto& a = m_replayCtx.agents[entries[0].agentId];
                    a.overlapIsNewest  = true;
                    a.overlapThreshold = GetSpiritOverwriteDist(a.modelId);
                }
                continue;
            }

            // Sort newest first (highest spawnTime)
            std::sort(entries.begin(), entries.end(),
                      [](const SpiritEntry& a, const SpiritEntry& b) {
                          return a.spawnTime > b.spawnTime;
                      });

            auto& newest = entries[0];
            auto& newestArd = m_replayCtx.agents[newest.agentId];
            float threshold = GetSpiritOverwriteDist(newestArd.modelId);

            newestArd.overlapIsNewest  = true;
            newestArd.overlapThreshold = threshold;

            for (size_t i = 1; i < entries.size(); ++i)
            {
                auto& e = entries[i];
                auto& a = m_replayCtx.agents[e.agentId];
                float dx = e.px - newest.px;
                float dy = e.py - newest.py;
                float dist = sqrtf(dx * dx + dy * dy);

                a.overlapThreshold  = threshold;
                a.overlapDistNewest = dist;
                a.overlapHidden     = (dist < threshold);
            }
        }
    }

    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;

        if (ard.type == AgentType::Flag) continue;

        // Skip empty agents (no model, no gadget) and bodyguard model 166
        {
            const auto& fs = ard.snapshots.front();
            if (fs.model_id == 0 && fs.gadget_id == 0) continue;
            if (fs.model_id == 166) continue;
        }

        if (m_fogPerspective > 0 && ard.teamId != m_fogPerspective && IsAgentInFog(agentId))
        {
            if (!m_fogGhostMode) continue;
        }

        // Spirits only exist within their snapshot time range
        if (ard.type == AgentType::Spirit)
        {
            if (m_debugTimeline < ard.snapshots.front().time ||
                m_debugTimeline > ard.snapshots.back().time)
                continue;
        }

        // Items: use lifecycle AGENT_REMOVE time for precise visibility cutoff
        if (ard.type == AgentType::Item)
        {
            if (m_debugTimeline < ard.snapshots.front().time)
                continue;
            auto rmIt = m_itemRemoveTime.find(agentId);
            float removeT;
            if (rmIt != m_itemRemoveTime.end())
                removeT = rmIt->second;
            else if (m_replayCtx.stocLoaded)
                removeT = FLT_MAX;
            else
                removeT = ard.snapshots.back().time;
            if (m_debugTimeline > removeT)
                continue;
        }

        // Spirit visibility: hide overwritten, dead, or not-alive spirits immediately
        if (ard.type == AgentType::Spirit)
        {
            if (ard.overlapHidden) continue;
            if (ard.isDeadAtTime(m_debugTimeline) || !ard.isAliveAtTime(m_debugTimeline))
                continue;
        }

        // Summoned minions (e.g. Bone Horror): the name/overlay only appears
        // while the agent exists and is alive (same rule as its 3D model).
        if (ard.type == AgentType::NPC && IsNpcHiddenWhenDead(ard.modelId)
            && !ard.isMinionVisibleAtTime(m_debugTimeline))
            continue;

        float sx, sy, sz;
        InterpolateAgentPosition(ard, m_debugTimeline, is, sx, sy, sz);

        // Optional: show raw axis-remapped position (no transform) — from calibration panel
        if (m_showRawPositions) {
            XMFLOAT3 rawPos = { sx, sz, sy };
            float rsx, rsy;
            if (ProjectToScreen(viewProj, vpW, vpH, rawPos, rsx, rsy))
                dl->AddCircle(ImVec2(rsx, rsy), 3.f, IM_COL32(255, 255, 0, 120), 0, 1.f);
        }

        // Optional: show raw snapshot position as grey dot (from interp panel)
        float rawScrX = 0.f, rawScrY = 0.f;
        bool rawOnScreen = false;
        if (is.showRawSnapshots && ard.type != AgentType::Flag && ard.type != AgentType::Spirit)
        {
            float rx, ry, rz;
            SnapAgentPosition(ard, m_debugTimeline, rx, ry, rz);
            XMFLOAT3 rawPos = ApplyMapTransformToPos(rx, ry, rz, t);
            rawOnScreen = ProjectToScreen(viewProj, vpW, vpH, rawPos, rawScrX, rawScrY);
            if (rawOnScreen)
                dl->AddCircleFilled(ImVec2(rawScrX, rawScrY), 3.f, IM_COL32(160, 160, 160, 180));
        }

        // Optional: draw MOVE_TO_POINT anchors (yellow diamond + line from snap)
        if (is.showMoveAnchors && !ard.moveEvents.empty() &&
            ard.type != AgentType::Flag && ard.type != AgentType::Spirit)
        {
            int moveIdx = FindMoveEventIndex(ard.moveEvents, m_debugTimeline);
            if (moveIdx >= 0) {
                auto& move = ard.moveEvents[moveIdx];
                XMFLOAT3 mpos = ApplyMapTransformToPos(move.targetX, move.targetY, 0.f, t);
                float msx, msy;
                if (ProjectToScreen(viewProj, vpW, vpH, mpos, msx, msy)) {
                    dl->AddCircleFilled(ImVec2(msx, msy), 4.f, IM_COL32(255, 255, 0, 200));
                    float snapScrX, snapScrY;
                    float rx, ry, rz;
                    SnapAgentPosition(ard, m_debugTimeline, rx, ry, rz);
                    XMFLOAT3 spos = ApplyMapTransformToPos(rx, ry, rz, t);
                    if (ProjectToScreen(viewProj, vpW, vpH, spos, snapScrX, snapScrY))
                        dl->AddLine(ImVec2(snapScrX, snapScrY), ImVec2(msx, msy),
                                    IM_COL32(255, 255, 0, 100), 1.f);
                }
            }
        }

        XMFLOAT3 pos = ApplyMapTransformToPos(sx, sy, sz, t);

        // Clamp to map boundaries to prevent out-of-bounds drift
        if (hasBounds) {
            if (pos.x < bMinX) pos.x = bMinX;
            if (pos.x > bMaxX) pos.x = bMaxX;
            if (pos.z < bMinZ) pos.z = bMinZ;
            if (pos.z > bMaxZ) pos.z = bMaxZ;
        }

        float scrX, scrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, pos, scrX, scrY)) continue;

        // Debug line between raw snapshot and interpolated position
        if (is.showInterpolatedLine && rawOnScreen &&
            ard.type != AgentType::Flag && ard.type != AgentType::Spirit)
            dl->AddLine(ImVec2(rawScrX, rawScrY), ImVec2(scrX, scrY),
                        IM_COL32(255, 255, 255, 80), 1.f);

        bool casting = ard.isCastingAtTime(m_debugTimeline);
        bool dead    = ard.isDeadAtTime(m_debugTimeline);

        // Determine if this agent has a 3D representation or needs a 2D dot
        bool is3DAgent = (ard.type == AgentType::Player || ard.type == AgentType::NPC);
        bool showDot = !is3DAgent;

        // Agents with 3D models don't need dots
        if (m_useAgentModels && m_agentMeshIds.count(agentId))
            showDot = false;

        // For players/NPCs, check LOD: if Dot mode, use stylized profession icon for players
        if (is3DAgent && ard.currentLOD == 0)
            showDot = true;

        // For players with 3D representation (not dot LOD), draw profession icon just above model head
        if (ard.type == AgentType::Player && !showDot && ard.primaryProf >= 1)
        {
            float modelTopY = AgentModelTopY(agentId, ard, pos.y, m_debugTimeline);
            if (modelTopY <= pos.y)
                modelTopY = pos.y + 120.f;

            XMFLOAT3 topPos = { pos.x, modelTopY, pos.z };
            float topScrX, topScrY;
            if (ProjectToScreen(viewProj, vpW, vpH, topPos, topScrX, topScrY))
            {
                ID3D11Device* dev = m_deviceResources->GetD3DDevice();
                ImTextureID profTex = LoadProfIcon(dev, ard.primaryProf);
                if (profTex)
                {
                    float iconSz = std::clamp(vpH * 0.020f, 12.f, 20.f);
                    constexpr float kScreenPad = 4.f;
                    float iconBottom = topScrY - kScreenPad;
                    ImVec2 iconTL(topScrX - iconSz * 0.5f, iconBottom - iconSz);
                    ImVec2 iconBR(topScrX + iconSz * 0.5f, iconBottom);
                    dl->AddImage(profTex, iconTL, iconBR);
                }
            }
        }

        // Floating skill icon + cast bar above agent (players/NPCs, alive only)
        if (m_showSkillIcons && !dead && (ard.type == AgentType::Player || ard.type == AgentType::NPC))
        {
            auto sv = ard.skillVisualAtTime(m_debugTimeline);
            if (sv.skillId > 0 && sv.alpha > 0.f)
            {
                EnsureSkillIconIndex();
                ID3D11Device* dev = m_deviceResources->GetD3DDevice();
                ImTextureID skillTex = LoadSkillIcon(this, dev, sv.skillId,
                                                     m_skillIconIndex, m_skillIconCache);
                constexpr float SKILL_SIDE_OFFSET = 55.f;
                constexpr float SKILL_UP_OFFSET   = 40.f;
                XMFLOAT3 skillPos = { pos.x + SKILL_SIDE_OFFSET, pos.y + SKILL_UP_OFFSET, pos.z };
                float skX, skY;
                if (ProjectToScreen(viewProj, vpW, vpH, skillPos, skX, skY))
                {
                    float dpiScale = std::max(1.f, vpH / 1080.f);
                    ImU8 alpha = (ImU8)(sv.alpha * 255.f);
                    float iconSz = std::clamp(vpH * 0.028f, 20.f, 32.f);

                    // Skill icon
                    if (skillTex)
                    {
                        ImVec2 iconTL(skX - iconSz * 0.5f, skY - iconSz * 0.5f);
                        ImVec2 iconBR(skX + iconSz * 0.5f, skY + iconSz * 0.5f);
                        dl->AddImage(skillTex, iconTL, iconBR,
                                     ImVec2(0, 0), ImVec2(1, 1),
                                     IM_COL32(255, 255, 255, alpha));
                    }

                    // Cast bar (non-instant skills: casting, cancelled, interrupted, or just completed)
                    bool showBar = sv.isCasting || sv.cancelled || sv.interrupted;
                    if (showBar)
                    {
                        float barW = iconSz * 1.6f;
                        float barH = 6.f  * dpiScale;
                        float gap  = 2.f  * dpiScale;

                        ImVec2 barMin(skX - barW * 0.5f, skY + iconSz * 0.5f + gap);
                        ImVec2 barMax(barMin.x + barW, barMin.y + barH);
                        float pct   = sv.progress;
                        float midY  = barMin.y + barH * 0.5f;

                        // Background: procedural vertical gradient (black→gray→black)
                        {
                            ImU32 bgD = IM_COL32(0, 0, 0, alpha);
                            ImU32 bgM = IM_COL32(36, 36, 36, alpha);
                            dl->AddRectFilledMultiColor(barMin, ImVec2(barMax.x, midY),
                                bgD, bgD, bgM, bgM);
                            dl->AddRectFilledMultiColor(ImVec2(barMin.x, midY), barMax,
                                bgM, bgM, bgD, bgD);
                        }

                        // Fill: procedural horizontal gradient + vertical vignette
                        // Green = success, Yellow/orange = cancelled, Purple = interrupted
                        float fillW = barW * pct;
                        if (pct > 0.005f)
                        {
                            static const GradStop sGreenH[] = {
                                { 0.000f,  10, 10, 10 }, { 0.200f,  26, 58, 10 },
                                { 0.400f,  64,176, 32 }, { 0.600f, 168,240, 80 },
                                { 0.800f, 200,255,112 }, { 1.000f, 144,224, 64 }
                            };
                            static const GradStop sOrangeH[] = {
                                { 0.000f,  10,  8,  0 }, { 0.143f,  58, 30,  0 },
                                { 0.286f, 122, 58,  0 }, { 0.429f, 192, 96,  0 },
                                { 0.571f, 232,144, 16 }, { 0.714f, 255,184, 32 },
                                { 0.857f, 255,208, 64 }, { 1.000f, 232,160, 16 }
                            };
                            static const GradStop sPurpleH[] = {
                                { 0.000f,  10, 10, 10 }, { 0.300f, 120, 32,192 },
                                { 0.600f, 224,160,255 }, { 1.000f, 160, 80,224 }
                            };
                            const GradStop* hS;
                            int nH;
                            float topV, botV;
                            if (sv.interrupted) {
                                hS = sPurpleH; nH = 4; topV = 0.58f; botV = 0.52f;
                            } else if (sv.cancelled) {
                                hS = sOrangeH; nH = 8; topV = 0.58f; botV = 0.52f;
                            } else {
                                hS = sGreenH; nH = 6; topV = 0.55f; botV = 0.50f;
                            }

                            int nSegs = std::clamp((int)(fillW / 3.f), 4, 24);
                            for (int si = 0; si < nSegs; ++si)
                            {
                                float u0 = (float)si / nSegs;
                                float u1 = (float)(si + 1) / nSegs;
                                float r0, g0, b0, r1, g1, b1;
                                SampleGradient(hS, nH, u0 * pct, r0, g0, b0);
                                SampleGradient(hS, nH, u1 * pct, r1, g1, b1);

                                float x0 = barMin.x + fillW * u0;
                                float x1 = barMin.x + fillW * u1;

                                auto vig = [&](float r, float g, float b, float d) -> ImU32 {
                                    float m = 1.f - d;
                                    return IM_COL32((ImU8)(r * m), (ImU8)(g * m), (ImU8)(b * m), alpha);
                                };
                                ImU32 tl = vig(r0,g0,b0, topV);
                                ImU32 tr = vig(r1,g1,b1, topV);
                                ImU32 ml = IM_COL32((ImU8)r0,(ImU8)g0,(ImU8)b0, alpha);
                                ImU32 mr = IM_COL32((ImU8)r1,(ImU8)g1,(ImU8)b1, alpha);
                                ImU32 bl = vig(r0,g0,b0, botV);
                                ImU32 br = vig(r1,g1,b1, botV);

                                dl->AddRectFilledMultiColor(
                                    ImVec2(x0, barMin.y), ImVec2(x1, midY),
                                    tl, tr, mr, ml);
                                dl->AddRectFilledMultiColor(
                                    ImVec2(x0, midY), ImVec2(x1, barMax.y),
                                    ml, mr, br, bl);
                            }
                        }

                        // Outer glow at leading edge
                        float fillX = barMin.x + fillW;
                        if (pct > 0.01f)
                        {
                            float gw = 6.f * dpiScale;
                            ImU8 glA1 = (ImU8)(140 * sv.alpha);
                            ImU8 glA2 = (ImU8)( 60 * sv.alpha);
                            ImU32 gc1, gc2;
                            if (sv.interrupted)
                            {
                                gc1 = IM_COL32(128, 48,192, glA1);
                                gc2 = IM_COL32(128, 48,192, glA2);
                            }
                            else if (sv.cancelled)
                            {
                                gc1 = IM_COL32(192,120,  0, glA1);
                                gc2 = IM_COL32(192,120,  0, glA2);
                            }
                            else
                            {
                                gc1 = IM_COL32( 96,208, 32, glA1);
                                gc2 = IM_COL32( 96,208, 32, glA2);
                            }
                            dl->AddRectFilled(
                                ImVec2(fillX - gw * 0.5f, barMin.y),
                                ImVec2(fillX + gw * 0.5f, barMax.y), gc1);
                            dl->AddRectFilled(
                                ImVec2(fillX - gw, barMin.y - 1.f * dpiScale),
                                ImVec2(fillX + gw, barMax.y + 1.f * dpiScale), gc2);
                        }
                    }
                }
            }
        }


        bool usedProfIcon = false;
        if (showDot)
        {
            if (ard.type == AgentType::Player && ard.primaryProf >= 1)
            {
                ID3D11Device* dev = m_deviceResources->GetD3DDevice();
                AgentIconState iconState = AgentIconState::Alive;
                if (dead)
                    iconState = AgentIconState::Dead;
                else if (ard.knockdownTiltAtTime(m_debugTimeline) > 0.f)
                    iconState = AgentIconState::Knockdown;

                ImTextureID stylTex = LoadProfStylized(dev, ard.primaryProf,
                                                       ard.teamId, iconState);
                if (stylTex)
                {
                    float iconSz = std::clamp(vpH * 0.020f, 12.f, 22.f);
                    ImVec2 iconTL(scrX - iconSz * 0.5f, scrY - iconSz * 0.5f);
                    ImVec2 iconBR(scrX + iconSz * 0.5f, scrY + iconSz * 0.5f);
                    dl->AddImage(stylTex, iconTL, iconBR);
                    usedProfIcon = true;
                }
            }
            if (!usedProfIcon)
            {
                bool isSpecialGadget = (ard.categoryName == "Repair Kit" ||
                                        ard.categoryName == "Tower Flag Stand" ||
                                        ard.categoryName == "Obelisk Flag Stand" ||
                                        ard.categoryName == "Resurrection Shrine" ||
                                        ard.categoryName == "Dwarven Resurrection Shrine" ||
                                        ard.categoryName == "Southern Health Shrine" ||
                                        ard.categoryName == "Gate lever");
                ImU32 dotColor;
                if (isSpecialGadget)
                    dotColor = IM_COL32(220, 200, 120, 255);
                else if (ard.type == AgentType::Spirit)
                    dotColor = IsNatureRitual(ard.modelId)
                        ? IM_COL32(0x80, 0xFF, 0x80, 0xFF)
                        : GetAgentTeamColor(ard.teamId);
                else if (ard.type == AgentType::Item)
                    dotColor = IM_COL32(0xFF, 0xA5, 0x00, 0xFF);
                else
                    dotColor = GetAgentTeamColor(ard.teamId);
                dl->AddCircleFilled(ImVec2(scrX, scrY), dotRadius, dotColor);
                dl->AddCircle(ImVec2(scrX, scrY), dotRadius, IM_COL32(0, 0, 0, 180), 0, 1.5f);
            }
        }

        // Dead freeze indicator: black X over the dot (dot-mode agents only, skip if prof icon used)
        if (showDot && !usedProfIcon && is.showDeadFreeze && dead &&
            ard.type != AgentType::Flag && ard.type != AgentType::Spirit)
        {
            float r = dotRadius + 2.f;
            dl->AddLine(ImVec2(scrX - r, scrY - r), ImVec2(scrX + r, scrY + r),
                        IM_COL32(0, 0, 0, 240), 2.f);
            dl->AddLine(ImVec2(scrX + r, scrY - r), ImVec2(scrX - r, scrY + r),
                        IM_COL32(0, 0, 0, 240), 2.f);
        }

        // Casting freeze indicator: purple ring (dot-mode agents only)
        if (showDot && is.showCastingFreeze && casting && !dead &&
            ard.type != AgentType::Flag && ard.type != AgentType::Spirit)
        {
            dl->AddCircle(ImVec2(scrX, scrY), dotRadius + 3.f,
                          IM_COL32(180, 60, 255, 220), 0, 2.f);
        }

        // Neon-green dashed ring + glow at cylinder base for followed agent (counter-clockwise spin)
        if (m_cameraMode == CameraMode::FollowAgent && agentId == m_followedAgentId)
        {
            constexpr int   NUM_DASHES = 16;
            constexpr float DASH_FRAC  = 0.70f;   // 70% visible, 30% gap
            constexpr float CYL_R      = 30.f;
            constexpr float RING_R     = CYL_R + 4.f;
            constexpr float PI2        = 6.2831853f;
            constexpr int   PTS_PER_DASH = 8;      // smooth arc per dash
            const ImU32 ringCol = IM_COL32(0, 255, 120, 255);
            const ImU32 glowCol = IM_COL32(0, 255, 120, 80);

            float baseY = pos.y + 0.05f;
            float spinOffset = -fmodf((float)ImGui::GetTime() / 15.f, 1.f) * PI2;
            float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 2.5f);
            float pulseR = RING_R + pulse * 3.f;
            ImU8  pulseA = (ImU8)(200 + (int)(55.f * pulse));

            auto projectRingPt = [&](float angle, float r, float& ox, float& oy) -> bool {
                XMFLOAT3 wp = { pos.x + r * cosf(angle), baseY, pos.z + r * sinf(angle) };
                return ProjectToScreen(viewProj, vpW, vpH, wp, ox, oy);
            };

            // Glow layers (pulsating)
            for (int g = 1; g <= 3; ++g)
            {
                float gr = pulseR + g * 3.f;
                ImU32 gc = IM_COL32(0, 255, 120, (ImU8)((int)(80 * (0.5f + 0.5f * pulse)) / g));
                float prevX, prevY;
                bool first = true;
                for (int i = 0; i <= 64; ++i)
                {
                    float a = (float(i) / 64) * PI2;
                    float rx, ry;
                    if (!projectRingPt(a, gr, rx, ry)) { first = true; continue; }
                    if (!first)
                        dl->AddLine(ImVec2(prevX, prevY), ImVec2(rx, ry), gc, 2.f);
                    prevX = rx; prevY = ry;
                    first = false;
                }
            }

            // Dashed ring with counter-clockwise rotation (pulsating)
            ImU32 pulseRingCol = IM_COL32(0, 255, 120, pulseA);
            float dashArc = (PI2 / NUM_DASHES) * DASH_FRAC;
            float segStep = PI2 / NUM_DASHES;
            for (int d = 0; d < NUM_DASHES; ++d)
            {
                float dashStart = spinOffset + d * segStep;
                float prevX, prevY;
                bool first = true;
                for (int p = 0; p <= PTS_PER_DASH; ++p)
                {
                    float a = dashStart + (float(p) / PTS_PER_DASH) * dashArc;
                    float rx, ry;
                    if (!projectRingPt(a, pulseR, rx, ry)) { first = true; continue; }
                    if (!first)
                        dl->AddLine(ImVec2(prevX, prevY), ImVec2(rx, ry), pulseRingCol, 2.5f);
                    prevX = rx; prevY = ry;
                    first = false;
                }
            }
        }

        // Hover detection + click-to-follow
        if (canClickAgents)
        {
            bool hit = false;

            if (!showDot && is3DAgent)
            {
                constexpr float HOVER_H = 120.f;
                XMFLOAT3 topPos = { pos.x, pos.y + HOVER_H, pos.z };
                float topScrX, topScrY;
                if (ProjectToScreen(viewProj, vpW, vpH, topPos, topScrX, topScrY))
                {
                    float minY = std::min(topScrY, scrY);
                    float maxY = std::max(topScrY, scrY);
                    float height = maxY - minY;
                    float halfW = std::max(height * 0.35f, clickRadius);
                    hit = (mousePos.x >= scrX - halfW && mousePos.x <= scrX + halfW &&
                           mousePos.y >= minY - 4.f   && mousePos.y <= maxY + 4.f);
                }
            }
            else
            {
                float dx = mousePos.x - scrX;
                float dy = mousePos.y - scrY;
                hit = (dx * dx + dy * dy <= clickRadius * clickRadius);
            }

            if (hit)
            {
                m_hoveredAgentId = agentId;

                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    EnterFollowMode(agentId);
                    OpenPlayerInfoPanel(agentId);
                    if (m_showRangeRings)
                        m_ringAgentFilter = (m_ringAgentFilter == agentId) ? -1 : agentId;
                    if (m_fogPerspective > 0)
                        m_fogPlayerAgent = (m_fogPlayerAgent == agentId) ? -1 : agentId;
                }
            }
        }

        if (m_show3DLabels && !m_hiddenNameAgents.count(agentId))
        {
            std::string label = GetAgentLabel(ard);
            ImVec2 textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, label.c_str());
            float lx = scrX - textSize.x * 0.5f;
            float ly = scrY + dotRadius + labelOffY;

            bool isSpecialLabel = (ard.categoryName == "Repair Kit" ||
                                   ard.categoryName == "Tower Flag Stand" ||
                                   ard.categoryName == "Obelisk Flag Stand" ||
                                   ard.categoryName == "Resurrection Shrine" ||
                                   ard.categoryName == "Dwarven Resurrection Shrine" ||
                                   ard.categoryName == "Southern Health Shrine" ||
                                   ard.categoryName == "Gate lever");
            float pad = 2.f;
            if (isSpecialLabel)
            {
                dl->AddRectFilled(ImVec2(lx - pad, ly - pad),
                                  ImVec2(lx + textSize.x + pad, ly + textSize.y + pad),
                                  IM_COL32(0, 0, 0, 13), 3.f);
                dl->AddText(ImVec2(lx, ly), IM_COL32(245, 228, 180, 255), label.c_str());
            }
            else
            {
                dl->AddRectFilled(ImVec2(lx - pad, ly - pad),
                                  ImVec2(lx + textSize.x + pad, ly + textSize.y + pad),
                                  IM_COL32(0, 0, 0, 25), 3.f);
                ImU32 labelCol;
                if (ard.teamId == 1)      labelCol = IM_COL32(0xFF, 0x99, 0x9A, 0xE6);
                else if (ard.teamId == 2) labelCol = IM_COL32(0x99, 0xCB, 0xFD, 0xE6);
                else                      labelCol = IM_COL32(255, 255, 255, 230);
                dl->AddText(ImVec2(lx + 1.f, ly + 1.f), IM_COL32(0, 0, 0, 200), label.c_str());
                dl->AddText(ImVec2(lx, ly), labelCol, label.c_str());

                if (agentId == m_hoveredAgentId)
                {
                    float ulY = ly + textSize.y - 2.f;
                    dl->AddLine(ImVec2(lx, ulY), ImVec2(lx + textSize.x, ulY), labelCol, 1.f);
                }
            }
        }

        // Top View: collect tooltip hit candidates (skip when names already visible)
        if (m_topViewActive && !m_show3DLabels)
        {
            const float pad = 4.f;
            float hitSz = showDot ? std::clamp(vpH * 0.020f, 12.f, 22.f) : 24.f;
            float rMinX = scrX - hitSz * 0.5f - pad;
            float rMinY = scrY - hitSz * 0.5f - pad;
            float rMaxX = scrX + hitSz * 0.5f + pad;
            float rMaxY = scrY + hitSz * 0.5f + pad;
            XMFLOAT3 camP = cam->GetPosition3f();
            float dx = pos.x - camP.x, dy = pos.y - camP.y, dz = pos.z - camP.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            topViewTooltipCandidates.push_back({ rMinX, rMinY, rMaxX, rMaxY, GetAgentLabel(ard), dist });
        }
    }

    // Top View: draw agent name tooltip on hover (skip when names already visible)
    if (m_topViewActive && !m_show3DLabels && !topViewTooltipCandidates.empty())
    {
        const float mx = mousePos.x, my = mousePos.y;
        int bestIdx = -1;
        float bestDist = FLT_MAX;
        for (size_t i = 0; i < topViewTooltipCandidates.size(); ++i)
        {
            const auto& c = topViewTooltipCandidates[i];
            if (mx >= c.rectMinX && mx <= c.rectMaxX && my >= c.rectMinY && my <= c.rectMaxY
                && c.dist < bestDist)
            {
                bestDist = c.dist;
                bestIdx = (int)i;
            }
        }
        if (bestIdx >= 0)
        {
            const auto& c = topViewTooltipCandidates[bestIdx];
            float iconCx = (c.rectMinX + c.rectMaxX) * 0.5f;
            float iconCy = (c.rectMinY + c.rectMaxY) * 0.5f;
            const float hitPad = 4.f;
            float iconTop = c.rectMinY + hitPad;
            float iconBottom = c.rectMaxY - hitPad;

            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const char* text = c.name.c_str();
            ImVec2 textSize = font->CalcTextSizeA(11.f, FLT_MAX, 0.f, text);
            const float tPadH = 4.f, tPadV = 3.f;
            float tw = textSize.x + tPadH * 2.f;
            float th = textSize.y + tPadV * 2.f;
            const float gap = 8.f;

            float tx = iconCx - tw * 0.5f;
            float ty;
            if (iconTop - gap - th >= 0.f)
                ty = iconTop - gap - th;
            else
                ty = iconBottom + gap;
            if (tx + tw > vpW) tx = vpW - tw;
            if (tx < 0.f) tx = 0.f;
            if (ty + th > vpH) ty = vpH - th;
            if (ty < 0.f) ty = 0.f;

            ImVec2 tMin(tx, ty);
            ImVec2 tMax(tx + tw, ty + th);
            fg->AddRectFilled(tMin, tMax, IM_COL32(14, 20, 26, (int)(0.88f * 255)));
            fg->AddRect(tMin, tMax, IM_COL32(255, 255, 255, (int)(0.12f * 255)), 4.f);
            fg->AddText(font, 11.f, ImVec2(tx + tPadH, ty + tPadV), IM_COL32(0xf0, 0xf0, 0xf0, 255), text);
        }
    }
}


// ---------------------------------------------------------------------------
// Skill Lasers: animated dashed line from caster → target
// ---------------------------------------------------------------------------

void ReplayWindow::DrawSkillLasers()
{
    if (!m_showSkillLasers) return;
    if (!m_skillUseTimelineBuilt) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    Camera* cam = m_mapRenderer->GetCamera();
    XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    auto vp = m_deviceResources->GetScreenViewport();
    float vpW = vp.Width, vpH = vp.Height;
    const auto& t = m_replayCtx.mapTransform;

    const InterpolationSettings& is = m_replayCtx.interpSettings;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float dpi = std::max(1.f, vpH / 1080.f);
    float curTime = (float)ImGui::GetTime();

    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Player && ard.type != AgentType::NPC) continue;
        if (ard.isDeadAtTime(m_debugTimeline)) continue;
        if (!LaserCasterVisible(agentId, ard)) continue;

        auto laser = ard.skillLaserAtTime(m_debugTimeline);
        if (laser.targetId <= 0 || laser.alpha <= 0.f) continue;

        auto tit = m_replayCtx.agents.find(laser.targetId);
        if (tit == m_replayCtx.agents.end()) continue;
        auto& targ = tit->second;

        // Caster position
        float cx, cy, cz;
        InterpolateAgentPosition(ard, m_debugTimeline, is, cx, cy, cz);
        XMFLOAT3 casterWorld = ApplyMapTransformToPos(cx, cy, cz, t);
        casterWorld.y += 60.f;

        float cScrX, cScrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, casterWorld, cScrX, cScrY)) continue;

        // Target position
        float txp, typ, tzp;
        InterpolateAgentPosition(targ, m_debugTimeline, is, txp, typ, tzp);
        XMFLOAT3 targetWorld = ApplyMapTransformToPos(txp, typ, tzp, t);
        targetWorld.y += 60.f;

        float tScrX, tScrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, targetWorld, tScrX, tScrY)) continue;

        // LOD check: skip if caster is far and in dot LOD
        if (m_uiLayout.lodEnabled && ard.currentLOD == 0)
        {
            XMFLOAT3 camPos;
            XMStoreFloat3(&camPos, cam->GetPosition());
            float dx = casterWorld.x - camPos.x;
            float dy = casterWorld.y - camPos.y;
            float dz2 = casterWorld.z - camPos.z;
            float dist = sqrtf(dx * dx + dy * dy + dz2 * dz2);
            if (dist > m_uiLayout.lodDotDist * 1.5f) continue;
        }

        // Determine ally vs enemy
        bool isEnemy = (ard.teamId != targ.teamId);
        ImU32 laserCol, glowCol;
        int lR, lG, lB;
        if (isEnemy)
        {
            lR = 255; lG = 60; lB = 60;
            laserCol = IM_COL32(255, 60, 60, (ImU8)(255 * laser.alpha));
            glowCol  = IM_COL32(255, 60, 60, (ImU8)(120 * laser.alpha));
        }
        else
        {
            lR = 60; lG = 255; lB = 120;
            laserCol = IM_COL32(60, 255, 120, (ImU8)(255 * laser.alpha));
            glowCol  = IM_COL32(60, 255, 120, (ImU8)(120 * laser.alpha));
        }

        ImVec2 A(cScrX, cScrY), B(tScrX, tScrY);
        float lineLen = sqrtf((B.x - A.x) * (B.x - A.x) + (B.y - A.y) * (B.y - A.y));
        if (lineLen < 2.f) continue;

        ImVec2 dir((B.x - A.x) / lineLen, (B.y - A.y) / lineLen);

        // Glow layers (continuous line behind dashes)
        for (int g = 1; g <= 3; ++g)
        {
            float thick = (1.0f + g * 0.8f) * dpi;
            ImU32 gc = IM_COL32(lR, lG, lB, (ImU8)(std::max(0.f, 30.f / g * laser.alpha)));
            dl->AddLine(A, B, gc, thick);
        }

        // Flowing dashes (caster → target direction)
        float dashLen = 14.f * dpi;
        float gapLen  = 8.f * dpi;
        float period  = dashLen + gapLen;
        float anim    = fmodf(curTime * 0.8f, 1.f);
        float offset  = anim * period;

        for (float d = offset - period; d < lineLen; d += period)
        {
            float s0 = std::clamp(d, 0.f, lineLen);
            float s1 = std::clamp(d + dashLen, 0.f, lineLen);
            if (s1 - s0 < 1.f) continue;
            ImVec2 p0(A.x + dir.x * s0, A.y + dir.y * s0);
            ImVec2 p1(A.x + dir.x * s1, A.y + dir.y * s1);
            dl->AddLine(p0, p1, laserCol, 1.2f * dpi);
        }

        // Arrowhead at target
        float arrowSz = 10.f * dpi;
        ImVec2 perp(-dir.y, dir.x);
        ImVec2 tip = B;
        ImVec2 left(B.x - dir.x * arrowSz + perp.x * arrowSz * 0.6f,
                     B.y - dir.y * arrowSz + perp.y * arrowSz * 0.6f);
        ImVec2 right(B.x - dir.x * arrowSz - perp.x * arrowSz * 0.6f,
                      B.y - dir.y * arrowSz - perp.y * arrowSz * 0.6f);
        dl->AddTriangleFilled(tip, left, right, laserCol);

        // Breathing target highlight ring at cylinder base (3D projected)
        constexpr float CYL_R       = 30.f;
        constexpr float RING_R      = CYL_R + 5.f;
        constexpr int   RING_SEGS   = 48;
        constexpr float DASH_RATIO  = 0.35f;
        constexpr float PI2         = 6.2831853f;

        float pulse = 0.5f + 0.5f * sinf(curTime * 4.f);
        ImU32 ringCol = IM_COL32(
            (int)(lR * (0.7f + 0.3f * pulse)),
            (int)(lG * (0.7f + 0.3f * pulse)),
            (int)(lB * (0.7f + 0.3f * pulse)),
            (ImU8)(220 * laser.alpha));

        // Base of the cylinder (targetWorld.y was offset +60, remove it and add small lift)
        float baseY = targetWorld.y - 60.f + 0.05f;

        for (int i = 0; i < RING_SEGS; ++i)
        {
            float a0 = (float(i) / RING_SEGS) * PI2;
            float a1 = (float(i + 1) / RING_SEGS) * PI2;
            float mid = (a0 + a1) * 0.5f;
            float sa = a0 + (mid - a0) * (1.f - DASH_RATIO);
            float ea = mid + (a1 - mid) * DASH_RATIO;

            XMFLOAT3 wp0 = { targetWorld.x + RING_R * cosf(sa), baseY, targetWorld.z + RING_R * sinf(sa) };
            XMFLOAT3 wp1 = { targetWorld.x + RING_R * cosf(ea), baseY, targetWorld.z + RING_R * sinf(ea) };
            float sx0, sy0, sx1, sy1;
            if (ProjectToScreen(viewProj, vpW, vpH, wp0, sx0, sy0) &&
                ProjectToScreen(viewProj, vpW, vpH, wp1, sx1, sy1))
            {
                dl->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), ringCol, 1.5f * dpi);
            }
        }
    }
}


// ---------------------------------------------------------------------------
// Skill Laser filter panel — team / profession / per-caster toggles.
// Styling mirrors DrawRangeRingToolbar() so the two overlays feel like a pair.
// ---------------------------------------------------------------------------

namespace
{
    const char* LaserProfName(int id)
    {
        switch (id)
        {
        case 1:  return "Warrior";
        case 2:  return "Ranger";
        case 3:  return "Monk";
        case 4:  return "Necromancer";
        case 5:  return "Mesmer";
        case 6:  return "Elementalist";
        case 7:  return "Assassin";
        case 8:  return "Ritualist";
        case 9:  return "Paragon";
        case 10: return "Dervish";
        default: return "No Profession";
        }
    }

    // Short form keeps the pill grid narrow enough for two rows of five.
    const char* LaserProfAbbrev(int id)
    {
        switch (id)
        {
        case 1:  return "W";
        case 2:  return "R";
        case 3:  return "Mo";
        case 4:  return "N";
        case 5:  return "Me";
        case 6:  return "E";
        case 7:  return "A";
        case 8:  return "Rt";
        case 9:  return "P";
        case 10: return "D";
        default: return "--";
        }
    }
}

bool ReplayWindow::LaserCasterVisible(int agentId, const AgentReplayData& ard) const
{
    if (m_laserHiddenAgents.count(agentId)) return false;

    if (ard.teamId == 1 && !m_laserShowRed)  return false;
    if (ard.teamId == 2 && !m_laserShowBlue) return false;

    int prof = (ard.primaryProf > 0 && ard.primaryProf < kLaserProfCount)
                 ? ard.primaryProf : 0;
    return m_laserProf[prof];
}

void ReplayWindow::DrawSkillLaserPanel()
{
    if (!m_showLaserPanel) return;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,      ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.f, 0.f), ImVec2(vp->Size.x, vp->Size.y));
    ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
    m_panelLayout.ApplyPosition("skill_lasers");
    if (ImGui::Begin("Skill Lasers", &m_showLaserPanel,
        ImGuiWindowFlags_AlwaysAutoResize))
    {
        m_panelLayout.TrackWindow("skill_lasers");
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, vp->Pos.x, vp->Pos.x + vp->Size.x - sz.x);
        float cy = std::clamp(pos.y, vp->Pos.y, vp->Pos.y + vp->Size.y - sz.y);
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));

        auto LaserPill = [](const char* label, bool active) -> bool {
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
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(5);
            return clicked;
        };

        auto TeamPill = [](const char* label, bool active, int team) -> bool {
            ImVec4 bg, tx, hov, bdr;
            if (active) {
                if (team == 1) {
                    bg  = ImVec4(0.25f, 0.06f, 0.06f, 1.f);
                    tx  = ImVec4(1.f, 0.42f, 0.42f, 1.f);
                    hov = ImVec4(0.30f, 0.10f, 0.10f, 1.f);
                    bdr = ImVec4(1.f, 0.42f, 0.42f, 0.85f);
                } else {
                    bg  = ImVec4(0.05f, 0.12f, 0.25f, 1.f);
                    tx  = ImVec4(0.29f, 0.78f, 1.f, 1.f);
                    hov = ImVec4(0.08f, 0.16f, 0.30f, 1.f);
                    bdr = ImVec4(0.29f, 0.78f, 1.f, 0.85f);
                }
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
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(5);
            return clicked;
        };

        // Master switch for the overlay itself; the filters below stay editable
        // while it is off so a setup can be prepared before switching on.
        ImGui::Checkbox("Show Skill Lasers", &m_showSkillLasers);

        ImGui::Separator();

        // --- Professions ---
        ImGui::TextDisabled("Professions");
        if (LaserPill("None##LaserProf", false))
            for (int i = 0; i < kLaserProfCount; ++i) m_laserProf[i] = false;
        ImGui::SameLine();
        if (LaserPill("All##LaserProf", false))
            for (int i = 0; i < kLaserProfCount; ++i) m_laserProf[i] = true;

        ID3D11Device* dev = m_deviceResources->GetD3DDevice();
        const float iconSz = 16.f;

        // Profession toggle drawn as the game's own profession icon. Active
        // keeps the gold frame used by the pills; inactive dims the art so the
        // row still reads as a set of toggles. Falls back to the abbreviation
        // pill when the icon texture is unavailable.
        const float profBtnSz = ImGui::GetTextLineHeight() + 4.f;
        auto ProfIconToggle = [&](int p) -> bool
        {
            const bool  active  = m_laserProf[p];
            ImTextureID profTex = (p > 0) ? LoadProfIcon(dev, p) : nullptr;
            if (!profTex)
                return LaserPill(LaserProfAbbrev(p), active);

            ImVec4 bg  = active ? ImVec4(0.18f, 0.14f, 0.05f, 1.f)
                                : ImVec4(1.f, 1.f, 1.f, 0.05f);
            ImVec4 bdr = active ? ImVec4(1.f, 0.84f, 0.39f, 0.85f)
                                : ImVec4(1.f, 1.f, 1.f, 0.08f);
            ImVec4 tint = active ? ImVec4(1.f, 1.f, 1.f, 1.f)
                                 : ImVec4(1.f, 1.f, 1.f, 0.32f);

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_Border, bdr);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 3));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            bool clicked = ImGui::ImageButton("##prof", profTex,
                                              ImVec2(profBtnSz, profBtnSz),
                                              ImVec2(0, 0), ImVec2(1, 1), bg, tint);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(4);
            return clicked;
        };

        // Ids 1..10 in two rows of five, then the catch-all slot 0.
        for (int p = 1; p < kLaserProfCount; ++p)
        {
            if (p != 1 && p != 6) ImGui::SameLine(0.f, 3.f);

            ImGui::PushID(1000 + p);
            if (ProfIconToggle(p))
                m_laserProf[p] = !m_laserProf[p];
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", LaserProfName(p));
            ImGui::PopID();
        }

        ImGui::PushID(1000);
        if (LaserPill("Other", m_laserProf[0]))
            m_laserProf[0] = !m_laserProf[0];
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Casters with no known profession (NPCs, henchmen)");
        ImGui::PopID();

        ImGui::Separator();

        // --- Teams ---
        ImGui::TextDisabled("Teams");
        if (TeamPill("Red##Laser", m_laserShowRed, 1))
            m_laserShowRed = !m_laserShowRed;
        ImGui::SameLine();
        if (TeamPill("Blue##Laser", m_laserShowBlue, 2))
            m_laserShowBlue = !m_laserShowBlue;

        ImGui::Separator();

        // --- Individual casters ---
        if (ImGui::SmallButton("Show All##Lasers"))
            m_laserHiddenAgents.clear();
        ImGui::SameLine();
        if (ImGui::SmallButton("Hide All##Lasers"))
        {
            for (int id : m_playerIds)
                m_laserHiddenAgents.insert(id);
        }

        auto DrawTeamLasers = [&](const char* header, const std::vector<int>& ids,
                                  bool teamEnabled)
        {
            if (!teamEnabled) ImGui::BeginDisabled();
            if (ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (int id : ids)
                {
                    auto it = m_replayCtx.agents.find(id);
                    if (it == m_replayCtx.agents.end()) continue;
                    const auto& ard = it->second;

                    bool visible = !m_laserHiddenAgents.count(id);

                    ImTextureID profTex = (ard.primaryProf > 0)
                        ? LoadProfIcon(dev, ard.primaryProf) : nullptr;
                    if (profTex)
                    {
                        ImGui::Image(profTex, ImVec2(iconSz, iconSz));
                        ImGui::SameLine(0, 4);
                    }

                    if (ImGui::Checkbox(("##laser_" + std::to_string(id)).c_str(), &visible))
                    {
                        if (visible)
                            m_laserHiddenAgents.erase(id);
                        else
                            m_laserHiddenAgents.insert(id);
                    }
                    ImGui::SameLine(0, 4);

                    // Greyed out when the profession filter already hides this
                    // caster, so the checkbox state is not misread as "showing".
                    int prof = (ard.primaryProf > 0 && ard.primaryProf < kLaserProfCount)
                                 ? ard.primaryProf : 0;
                    std::string label = ard.playerName.empty()
                        ? ard.categoryName : ard.playerName;
                    if (!m_laserProf[prof])
                        ImGui::TextDisabled("%s", label.c_str());
                    else
                        ImGui::TextUnformatted(label.c_str());
                }
                ImGui::TreePop();
            }
            if (!teamEnabled) ImGui::EndDisabled();
        };

        // Side by side: two eight-player rosters stacked would make the panel
        // taller than most screens.
        if (ImGui::BeginTable("##LaserTeams", 2,
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_NoSavedSettings))
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawTeamLasers("Red Team",  m_team1PlayerIds, m_laserShowRed);
            ImGui::TableSetColumnIndex(1);
            DrawTeamLasers("Blue Team", m_team2PlayerIds, m_laserShowBlue);
            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
}


void ReplayWindow::EnsureCastBarTextures()
{
    if (m_castBarBgTex) return;
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    if (!dev) return;

    constexpr int H = 64;
    constexpr int W = 512;
    m_castBarTexH = H;

    // Background: symmetric black vignette (1xN, vertical only)
    static const GradStop bgStops[] = {
        { 0.00f,  0,  0,  0 },
        { 0.25f, 18, 18, 18 },
        { 0.50f, 36, 36, 36 },
        { 0.75f, 18, 18, 18 },
        { 1.00f,  0,  0,  0 }
    };
    m_castBarBgTex = BuildGradientTex1xN(dev, H, bgStops, 5);

    // Green casting fill (horizontal gradient + vertical vignette 55%/50%)
    static const GradStop greenH[] = {
        { 0.000f,  10, 10, 10 },
        { 0.200f,  26, 58, 10 },
        { 0.400f,  64,176, 32 },
        { 0.600f, 168,240, 80 },
        { 0.800f, 200,255,112 },
        { 1.000f, 144,224, 64 }
    };
    m_castBarFillTex = BuildCastBarFillTex2D(dev, W, H, greenH, 6, 0.55f, 0.50f);

    // Orange cancelled fill (horizontal gradient + vertical vignette 58%/52%)
    static const GradStop orangeH[] = {
        { 0.000f,  10,  8,  0 },
        { 0.143f,  58, 30,  0 },
        { 0.286f, 122, 58,  0 },
        { 0.429f, 192, 96,  0 },
        { 0.571f, 232,144, 16 },
        { 0.714f, 255,184, 32 },
        { 0.857f, 255,208, 64 },
        { 1.000f, 232,160, 16 }
    };
    m_castBarCancelTex = BuildCastBarFillTex2D(dev, W, H, orangeH, 8, 0.58f, 0.52f);
}


void ReplayWindow::EnsureSkillIconIndex()
{
    if (m_skillIconIndexBuilt) return;
    m_skillIconIndexBuilt = true;

    // Primary folder: Textures/Skill_Icons/ with "[ID] - Name.jpg" naming
    auto folder = GetSkillIconsBasePath();
    if (std::filesystem::exists(folder))
    {
        for (const auto& entry : std::filesystem::directory_iterator(folder))
        {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            if (name.size() < 4 || name[0] != '[') continue;
            size_t closeBracket = name.find(']', 1);
            if (closeBracket == std::string::npos) continue;
            int skillId = 0;
            try { skillId = std::stoi(name.substr(1, closeBracket - 1)); }
            catch (...) { continue; }
            m_skillIconIndex[skillId] = entry.path().string();
        }
    }

    // Fallback folder: Textures/skills/ with "{id}.jpg" naming
    wchar_t exeBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    auto exeDir = std::filesystem::path(exeBuf).parent_path();
    for (auto dir = exeDir; ; dir = dir.parent_path())
    {
        auto alt = dir / "Textures" / "skills";
        if (std::filesystem::exists(alt))
        {
            for (const auto& entry : std::filesystem::directory_iterator(alt))
            {
                if (!entry.is_regular_file()) continue;
                auto stem = entry.path().stem().string();
                int skillId = 0;
                try { skillId = std::stoi(stem); }
                catch (...) { continue; }
                if (m_skillIconIndex.find(skillId) == m_skillIconIndex.end())
                    m_skillIconIndex[skillId] = entry.path().string();
            }
            break;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
    }
}


// ---------------------------------------------------------------------------
// Name Filter — per-agent name visibility toggles
// ---------------------------------------------------------------------------

void ReplayWindow::DrawNameFilterPanel()
{
    if (!m_showNameFilterPanel || !m_agentsClassified) return;

    ImGuiIO& io = ImGui::GetIO();
    float vpW = io.DisplaySize.x;
    float vpH = io.DisplaySize.y;

    ImGui::SetNextWindowSizeConstraints(ImVec2(220.f, 140.f), ImVec2(vpW, vpH));
    if (m_panelLayout.HasSavedSize("agent_names"))
        m_panelLayout.ApplySize("agent_names");
    else
        ImGui::SetNextWindowSize(ImVec2(260.f, 380.f), ImGuiCond_FirstUseEver);
    m_panelLayout.ApplyPosition("agent_names");

    ImGui::PushStyleColor(ImGuiCol_WindowBg,             ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,              ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,        ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,               ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,            ImVec4(0.90f, 0.76f, 0.30f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,    6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,  1.f);

    if (!ImGui::Begin("Agent Names", &m_showNameFilterPanel))
    {
        m_panelLayout.TrackWindow("agent_names");
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
        return;
    }

    m_panelLayout.TrackWindow("agent_names");

    {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, 0.f, std::max(0.f, vpW - sz.x));
        float cy = std::clamp(pos.y, 0.f, std::max(0.f, vpH - sz.y));
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));
    }

    // Master toggle
    ImGui::Checkbox("Show Names", &m_show3DLabels);
    ImGui::Separator();

    // Per-player filter (only meaningful when names are on)
    bool dimmed = !m_show3DLabels;
    if (dimmed)
        ImGui::BeginDisabled();

    // Show All / Hide All
    if (ImGui::SmallButton("Show All"))
        m_hiddenNameAgents.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Hide All"))
    {
        for (int id : m_playerIds)
            m_hiddenNameAgents.insert(id);
    }

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    const float iconSz = 16.f;

    auto DrawTeam = [&](const char* header, const std::vector<int>& ids)
    {
        if (ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (int id : ids)
            {
                auto it = m_replayCtx.agents.find(id);
                if (it == m_replayCtx.agents.end()) continue;
                const auto& ard = it->second;

                bool visible = !m_hiddenNameAgents.count(id);

                ImTextureID profTex = (ard.primaryProf > 0)
                    ? LoadProfIcon(dev, ard.primaryProf) : nullptr;
                if (profTex)
                {
                    ImGui::Image(profTex, ImVec2(iconSz, iconSz));
                    ImGui::SameLine(0, 4);
                }

                if (ImGui::Checkbox(("##name_" + std::to_string(id)).c_str(), &visible))
                {
                    if (visible)
                        m_hiddenNameAgents.erase(id);
                    else
                        m_hiddenNameAgents.insert(id);
                }
                ImGui::SameLine(0, 4);
                std::string label = ard.playerName.empty()
                    ? ard.categoryName : ard.playerName;
                ImGui::TextUnformatted(label.c_str());
            }
            ImGui::TreePop();
        }
    };

    DrawTeam("Team 1", m_team1PlayerIds);
    DrawTeam("Team 2", m_team2PlayerIds);

    if (dimmed)
        ImGui::EndDisabled();

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
}
