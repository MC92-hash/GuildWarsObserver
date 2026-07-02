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
// Bundle carry timeline — correlate lifecycle events with weapon_type transitions
// to build per-player intervals of what bundle they are holding.
// ---------------------------------------------------------------------------

void ReplayWindow::BuildBundleCarryTimeline()
{
    m_bundleCarryBuilt = true;
    m_bundleCarry.clear();
    m_itemRemoveTime.clear();
    m_leverCaps.clear();
    m_vineBridgeEvents.clear();

    const int mapId = m_replayCtx.mapId;

    // Index lifecycle AGENT_REMOVE events by agent_id for quick lookup.
    // When an item agent is removed, it was picked up by a nearby player.
    // We also track AGENT_ADD events to know item spawn/respawn positions.
    struct ItemLifecycle {
        float addTime    = -1.f;
        float removeTime = -1.f;
        float addX = 0, addY = 0;
        uint32_t agentType = 0;
        int      typeCode  = 0;
    };
    // agent_id -> list of incarnations (add/remove pairs)
    std::unordered_map<int, std::vector<ItemLifecycle>> itemLifecycles;

    for (auto& ev : m_replayCtx.stocData.lifecycle)
    {
        if (ev.isAdd)
        {
            ItemLifecycle il;
            il.addTime   = ev.time;
            il.addX      = ev.x;
            il.addY      = ev.y;
            il.agentType = ev.agent_type;
            il.typeCode  = ev.type_code;
            itemLifecycles[ev.agent_id].push_back(il);
        }
        else
        {
            auto& vec = itemLifecycles[ev.agent_id];
            if (!vec.empty() && vec.back().removeTime < 0)
                vec.back().removeTime = ev.time;
        }
    }

    // Build m_itemRemoveTime from lifecycle AGENT_REMOVE events for item agents
    for (auto& [agentId, lifecycles] : itemLifecycles)
    {
        for (auto& lc : lifecycles)
        {
            if (lc.removeTime >= 0)
                m_itemRemoveTime[agentId] = lc.removeTime;
        }
    }

    // Build item_id -> BundleType map from known item agents
    std::unordered_map<uint32_t, BundleType> itemIdBundleType;
    for (auto& [id, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Flag && ard.type != AgentType::Item) continue;
        if (ard.snapshots.empty()) continue;
        uint32_t iid = ard.snapshots.front().item_id;
        if (iid == 0) continue;
        BundleType bt = LookupBundleType(mapId, iid);
        if (bt != BundleType::Unknown)
            itemIdBundleType[iid] = bt;
    }

    // Scan all players for weapon_type transitions: non-0 -> 0 = pickup, 0 -> non-0 = drop/consume
    auto allPlayers = m_team1PlayerIds;
    allPlayers.insert(allPlayers.end(), m_team2PlayerIds.begin(), m_team2PlayerIds.end());

    for (int pid : allPlayers)
    {
        auto it = m_replayCtx.agents.find(pid);
        if (it == m_replayCtx.agents.end()) continue;
        const auto& ard = it->second;
        const auto& snaps = ard.snapshots;
        if (snaps.size() < 2) continue;

        for (size_t si = 1; si < snaps.size(); si++)
        {
            const auto& prev = snaps[si - 1];
            const auto& cur  = snaps[si];

            // Pickup: weapon_type was non-0, now 0 (and weapon_item_type == 46)
            if (prev.weapon_type != 0 && cur.weapon_type == 0 && cur.weapon_item_type == 46)
            {
                BundleCarryInterval bci;
                bci.startTime = cur.time;

                // Try to classify: look at which item agent disappeared around this time
                BundleType classified = BundleType::Unknown;
                float bestDist = FLT_MAX;
                int bestItemAgent = -1;

                for (auto& [itemAgentId, lifecycles] : itemLifecycles)
                {
                    for (auto& lc : lifecycles)
                    {
                        if (lc.removeTime < 0) continue;
                        if (std::abs(lc.removeTime - cur.time) > 2.f) continue;
                        float dx = lc.addX - cur.x;
                        float dy = lc.addY - cur.y;
                        float dsq = dx * dx + dy * dy;
                        if (dsq < bestDist)
                        {
                            bestDist = dsq;
                            bestItemAgent = itemAgentId;
                        }
                    }
                }

                // If we found a lifecycle match, look up the item_id from snapshot data
                if (bestItemAgent >= 0)
                {
                    auto ait = m_replayCtx.agents.find(bestItemAgent);
                    if (ait != m_replayCtx.agents.end() && !ait->second.snapshots.empty())
                    {
                        uint32_t iid = ait->second.snapshots.front().item_id;
                        auto btIt = itemIdBundleType.find(iid);
                        if (btIt != itemIdBundleType.end())
                            classified = btIt->second;
                    }
                    bci.itemAgentId = bestItemAgent;
                }

                // Fallback: if we're on a map with no vine seeds and no repair kits, it's a flag
                if (classified == BundleType::Unknown)
                    classified = BundleType::Flag;

                bci.type = classified;

                // Find the end: scan forward for weapon_type going back to non-0
                for (size_t ej = si + 1; ej < snaps.size(); ej++)
                {
                    if (snaps[ej].weapon_type != 0)
                    {
                        bci.endTime = snaps[ej].time;
                        break;
                    }
                }

                m_bundleCarry[pid].push_back(bci);
            }
        }
    }

    // Process MAP_OBJECT events for lever caps and vine bridge activations
    for (auto& moe : m_replayCtx.stocData.mapObject)
    {
        if (moe.isState) continue;

        // Lever cap: animation_type=2, animation_stage=2
        if (moe.animation_type == 2 && moe.animation_stage == 2)
        {
            LeverCapEvent lce;
            lce.time     = moe.time;
            lce.objectId = moe.object_id;

            // Find the player who just dropped the repair kit at this time
            for (int pid : allPlayers)
            {
                auto& intervals = m_bundleCarry[pid];
                for (auto& bci : intervals)
                {
                    if (bci.type != BundleType::RepairKit) continue;
                    if (std::abs(bci.endTime - moe.time) > 2.f) continue;
                    auto pit = m_replayCtx.agents.find(pid);
                    if (pit == m_replayCtx.agents.end()) continue;
                    const auto& psnap = *FindSnapshotAtTime(pit->second, bci.endTime);
                    lce.x = psnap.x;
                    lce.y = psnap.y;
                    lce.z = psnap.z;
                    lce.teamIdx = (pit->second.teamId == 1) ? 0 : 1;
                    break;
                }
                if (lce.teamIdx >= 0) break;
            }

            m_leverCaps.push_back(lce);
        }

        // Vine bridge activation: animation_type=16, animation_stage=2
        if (moe.animation_type == 16 && moe.animation_stage == 2)
        {
            VineBridgeEvent vbe;
            vbe.time     = moe.time;
            vbe.objectId = moe.object_id;

            for (int pid : allPlayers)
            {
                auto& intervals = m_bundleCarry[pid];
                for (auto& bci : intervals)
                {
                    if (bci.type != BundleType::VineSeed) continue;
                    if (std::abs(bci.endTime - moe.time) > 2.f) continue;
                    auto pit = m_replayCtx.agents.find(pid);
                    if (pit == m_replayCtx.agents.end()) continue;
                    const auto& psnap = *FindSnapshotAtTime(pit->second, bci.endTime);
                    vbe.x = psnap.x;
                    vbe.y = psnap.y;
                    vbe.z = psnap.z;
                    vbe.teamIdx = (pit->second.teamId == 1) ? 0 : 1;
                    break;
                }
                if (vbe.teamIdx >= 0) break;
            }

            m_vineBridgeEvents.push_back(vbe);
        }
    }

    // Build catapult lever state machines (Warrior's Isle only)
    m_catapultStates.clear();
    if (m_replayCtx.mapId == kWarriorsIsleMapId)
    {
        for (auto& moe : m_replayCtx.stocData.mapObject)
        {
            if (moe.isState) continue;
            if (moe.animation_stage != 2) continue;

            CatapultState cs = CatapultState::Unknown;
            if (moe.animation_type == 2)
                cs = CatapultState::Repaired;
            else if (moe.animation_type == 17)
                cs = CatapultState::Loaded;
            else if (moe.animation_type == 12)
                cs = CatapultState::Fired;

            if (cs != CatapultState::Unknown)
                m_catapultStates[moe.object_id].AddEvent(moe.time, cs);
        }

        for (auto& [objId, state] : m_catapultStates)
            state.Finalize();
    }
}


// ---------------------------------------------------------------------------
// Query the bundle type a player is carrying at a given time
// ---------------------------------------------------------------------------

BundleType ReplayWindow::GetPlayerBundleType(int agentId, float time) const
{
    auto it = m_bundleCarry.find(agentId);
    if (it == m_bundleCarry.end()) return BundleType::Unknown;

    for (auto& bci : it->second)
    {
        if (time >= bci.startTime && time < bci.endTime)
            return bci.type;
    }
    return BundleType::Unknown;
}


// ---------------------------------------------------------------------------
// Render carried bundle items (repair kits, vine seeds) and capped indicators
// ---------------------------------------------------------------------------

void ReplayWindow::DrawBundleItems()
{
    if (!m_bundleCarryBuilt) return;
    if (!m_agentsClassified) return;

    const auto& t = m_replayCtx.mapTransform;
    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;

    auto* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x;
    float vpH = vp->Size.y;

    XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();

    const float iconSz = std::clamp(vpH * 0.035f, 18.f, 32.f);

    // 1. Draw carried repair kits and vine seeds above the carrier
    auto allPlayers = m_team1PlayerIds;
    allPlayers.insert(allPlayers.end(), m_team2PlayerIds.begin(), m_team2PlayerIds.end());

    for (int pid : allPlayers)
    {
        BundleType bt = GetPlayerBundleType(pid, m_debugTimeline);
        if (bt != BundleType::RepairKit && bt != BundleType::VineSeed) continue;

        auto it = m_replayCtx.agents.find(pid);
        if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) continue;

        float cx, cy, cz;
        InterpolateAgentPosition(it->second, m_debugTimeline, m_replayCtx.interpSettings, cx, cy, cz);

        XMFLOAT3 pos = ApplyMapTransformToPos(cx, cy, cz, t);
        float scrX, scrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, pos, scrX, scrY)) continue;

        const char* iconFile = (bt == BundleType::RepairKit) ? "RepairKit.png" : "Vine Seed.png";
        ImTextureID tex = LoadNPCIcon(dev, iconFile);
        if (!tex) continue;

        float offsetY = iconSz * 0.8f;
        ImVec2 iconTL(scrX - iconSz * 0.5f, scrY - offsetY - iconSz);
        ImVec2 iconBR(iconTL.x + iconSz, iconTL.y + iconSz);
        dl->AddImage(tex, iconTL, iconBR);

        const char* label = (bt == BundleType::RepairKit) ? "Repair Kit" : "Vine Seed";
        ImFont* font = ImGui::GetFont();
        ImVec2 textSz = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, label);
        float tx = scrX - textSz.x * 0.5f;
        float ty = iconBR.y + 2.f;
        dl->AddText(ImVec2(tx + 1.f, ty + 1.f), IM_COL32(0, 0, 0, 200), label);
        dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 230), label);
    }

    // 2. Draw capped lever indicators (permanent after capping)
    for (auto& lc : m_leverCaps)
    {
        if (m_debugTimeline < lc.time) continue;
        if (lc.x == 0 && lc.y == 0) continue;

        ImTextureID tex = LoadNPCIcon(dev, "Lever.png");
        if (!tex) continue;

        XMFLOAT3 pos = ApplyMapTransformToPos(lc.x, lc.y, lc.z, t);
        float scrX, scrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, pos, scrX, scrY)) continue;

        float leverSz = iconSz * 1.2f;
        ImVec2 iconTL(scrX - leverSz * 0.5f, scrY - leverSz);
        ImVec2 iconBR(iconTL.x + leverSz, iconTL.y + leverSz);
        dl->AddImage(tex, iconTL, iconBR);

        auto csIt = m_catapultStates.find(lc.objectId);
        if (csIt != m_catapultStates.end() && m_replayCtx.mapId == kWarriorsIsleMapId)
        {
            const CatapultLeverState& cls = csIt->second;
            std::string labelStr = cls.GetLabelText(m_debugTimeline);
            ImU32 labelCol       = cls.GetLabelColor(m_debugTimeline);
            float glowOpacity    = cls.GetGlowOpacity(m_debugTimeline);
            ImU32 glowCol        = cls.GetGlowColor(m_debugTimeline);
            CatapultState cState = cls.GetState(m_debugTimeline);

            // Glow rings (concentric circles around the icon center)
            if (glowOpacity > 0.01f)
            {
                float cx = (iconTL.x + iconBR.x) * 0.5f;
                float cy = (iconTL.y + iconBR.y) * 0.5f;
                uint8_t gr = (glowCol >> IM_COL32_R_SHIFT) & 0xFF;
                uint8_t gg = (glowCol >> IM_COL32_G_SHIFT) & 0xFF;
                uint8_t gb = (glowCol >> IM_COL32_B_SHIFT) & 0xFF;

                float r1 = leverSz * 0.6f;
                float r2 = leverSz * 0.9f;
                float r3 = leverSz * 1.2f;
                dl->AddCircle(ImVec2(cx, cy), r1,
                    IM_COL32(gr, gg, gb, (uint8_t)(glowOpacity * 255)), 0, 2.0f);
                dl->AddCircle(ImVec2(cx, cy), r2,
                    IM_COL32(gr, gg, gb, (uint8_t)(glowOpacity * 0.5f * 255)), 0, 1.5f);
                dl->AddCircle(ImVec2(cx, cy), r3,
                    IM_COL32(gr, gg, gb, (uint8_t)(glowOpacity * 0.25f * 255)), 0, 1.0f);
            }

            ImFont* font = ImGui::GetFont();
            float fontSize = font->FontSize;
            if (cState == CatapultState::Impact)
                fontSize += 2.f;

            ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, labelStr.c_str());
            float tx = scrX - textSz.x * 0.5f;
            float ty = iconBR.y + 2.f;
            dl->AddText(font, fontSize, ImVec2(tx + 1.f, ty + 1.f),
                        IM_COL32(0, 0, 0, 200), labelStr.c_str());
            dl->AddText(font, fontSize, ImVec2(tx, ty), labelCol, labelStr.c_str());
        }
        else
        {
            const char* label = "Lever (Capped)";
            ImFont* font = ImGui::GetFont();
            ImVec2 textSz = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, label);
            float tx = scrX - textSz.x * 0.5f;
            float ty = iconBR.y + 2.f;
            dl->AddText(ImVec2(tx + 1.f, ty + 1.f), IM_COL32(0, 0, 0, 200), label);
            dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 230), label);
        }
    }

    // 3. Draw activated vine bridge indicators
    for (auto& vb : m_vineBridgeEvents)
    {
        if (m_debugTimeline < vb.time) continue;
        if (vb.x == 0 && vb.y == 0) continue;

        ImTextureID tex = LoadNPCIcon(dev, "Vine Seed.png");
        if (!tex) continue;

        XMFLOAT3 pos = ApplyMapTransformToPos(vb.x, vb.y, vb.z, t);
        float scrX, scrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, pos, scrX, scrY)) continue;

        float bridgeSz = iconSz * 1.2f;
        ImVec2 iconTL(scrX - bridgeSz * 0.5f, scrY - bridgeSz);
        ImVec2 iconBR(iconTL.x + bridgeSz, iconTL.y + bridgeSz);
        dl->AddImage(tex, iconTL, iconBR);

        const char* label = "Bridge (Grown)";
        ImFont* font = ImGui::GetFont();
        ImVec2 textSz = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, label);
        float tx = scrX - textSz.x * 0.5f;
        float ty = iconBR.y + 2.f;
        dl->AddText(ImVec2(tx + 1.f, ty + 1.f), IM_COL32(0, 0, 0, 200), label);
        dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 230), label);
    }
}
