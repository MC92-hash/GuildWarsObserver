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


// A filled disc whose colour is interpolated from the centre outwards, for glows
// that read as a wash of light rather than as drawn rings. ImDrawList has no
// gradient primitive, so the fan is written by hand: one centre vertex in the
// inner colour surrounded by rim vertices in the outer one.
static void AddRadialGlow(ImDrawList* dl, ImVec2 center, float radius,
                          ImU32 innerCol, ImU32 outerCol)
{
    if (radius <= 0.f) return;

    constexpr int kSegments = 48;
    const ImVec2 uv = dl->_Data->TexUvWhitePixel;

    dl->PrimReserve(kSegments * 3, kSegments + 1);
    const unsigned int base = dl->_VtxCurrentIdx;

    dl->PrimWriteVtx(center, uv, innerCol);
    for (int i = 0; i < kSegments; i++)
    {
        float a = (static_cast<float>(i) / kSegments) * 2.f * IM_PI;
        dl->PrimWriteVtx(ImVec2(center.x + cosf(a) * radius,
                                center.y + sinf(a) * radius), uv, outerCol);
    }
    for (int i = 0; i < kSegments; i++)
    {
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
        dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + ((i + 1) % kSegments)));
    }
}


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

    // Build item_id -> BundleType map from known item agents. Recorded against
    // the moment each agent appeared, since an id can serve a bundle and then a
    // flag within the same match.
    std::unordered_map<uint32_t, BundleType> itemIdBundleType;
    for (auto& [id, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Flag && ard.type != AgentType::Item) continue;
        if (ard.snapshots.empty()) continue;
        const auto& s0 = ard.snapshots.front();
        if (s0.item_id == 0) continue;
        BundleType bt = m_flagItems.Classify(s0.item_id, s0.time);
        if (bt != BundleType::Unknown)
            itemIdBundleType[s0.item_id] = bt;
    }

    // FLAG_PICKUP packets name the item that was picked up, which is the only
    // authoritative source. Index them per player so a carry interval can be
    // classified from the packet rather than guessed from lifecycle proximity.
    std::unordered_map<int, std::vector<std::pair<float, int>>> pickupsByPlayer;
    for (auto& pe : m_replayCtx.stocData.flagEvents.pickups)
        pickupsByPlayer[pe.player_agent_id].emplace_back(pe.time, pe.item_id);
    for (auto& [pid, vec] : pickupsByPlayer)
        std::sort(vec.begin(), vec.end());

    // Nearest pickup packet from this player within a couple of seconds of the
    // weapon_type transition that opened the carry interval.
    auto classifyFromPacket = [&](int pid, float t) -> BundleType {
        auto pit = pickupsByPlayer.find(pid);
        if (pit == pickupsByPlayer.end()) return BundleType::Unknown;
        float bestDt = 2.5f;
        int   bestItem = 0;
        float bestTime = t;
        for (auto& [ptime, itemId] : pit->second)
        {
            float dt = std::abs(ptime - t);
            if (dt > bestDt) continue;
            bestDt = dt;
            bestItem = itemId;
            bestTime = ptime;
        }
        if (bestItem == 0) return BundleType::Unknown;
        return m_flagItems.Classify(static_cast<uint32_t>(bestItem), bestTime);
    };

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

                // Prefer the item id the server reported; fall back to correlating
                // the item agent that disappeared around this time.
                BundleType classified = classifyFromPacket(pid, cur.time);
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
                    if (classified == BundleType::Unknown)
                    {
                        auto ait = m_replayCtx.agents.find(bestItemAgent);
                        if (ait != m_replayCtx.agents.end() && !ait->second.snapshots.empty())
                        {
                            uint32_t iid = ait->second.snapshots.front().item_id;
                            auto btIt = itemIdBundleType.find(iid);
                            if (btIt != itemIdBundleType.end())
                                classified = btIt->second;
                        }
                    }
                    bci.itemAgentId = bestItemAgent;
                }

                // Last resort: no packet and no lifecycle match. Most maps only have
                // flags, so that is the safest assumption.
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

    // The forward scan above only closes a carry when the weapon field is seen
    // going back to a real weapon, and that transition is missing whenever the
    // bundle is consumed rather than dropped (a planted seed, a capped lever),
    // when the player dies holding it, or when the recording simply ends first.
    // Those intervals keep endTime = FLT_MAX, which not only shows the bundle for
    // the rest of the replay but also hides every later carry by that player,
    // since GetPlayerBundleType returns the first interval that matches. Close
    // each interval against whatever end signal is actually available.

    float replayEnd = 0.f;
    for (auto& [aid, ard] : m_replayCtx.agents)
        if (!ard.snapshots.empty())
            replayEnd = std::max(replayEnd, ard.snapshots.back().time);

    // Seeds and repair kits cannot be tracked through the weapon field at all. One
    // dropped and picked straight back up never produces the non-0 weapon snapshot
    // the scan above needs to see the second pickup, so that carry goes missing
    // entirely, and spending one on a bridge or a catapult is invisible for the
    // same reason. Their timelines are built from the bundle's own world agents
    // plus the map object events, so they already hold every carry span with the
    // right carrier and the right boundaries. Take those carries from there instead
    // of guessing, which also keeps the carried icon in sync with the ground
    // marker: both then switch on the same event rather than on two independently
    // sampled signals.
    if (!m_flagTimeline.bundles.empty())
    {
        for (auto& [pid, intervals] : m_bundleCarry)
            intervals.erase(std::remove_if(intervals.begin(), intervals.end(),
                [](const BundleCarryInterval& b) {
                    return b.type == BundleType::VineSeed || b.type == BundleType::RepairKit;
                }),
                intervals.end());

        for (auto& bundle : m_flagTimeline.bundles)
        {
            for (size_t i = 0; i < bundle.events.size(); i++)
            {
                const auto& ev = bundle.events[i];
                if (ev.newLocation != BundleLocation::Carried) continue;
                if (ev.carrierAgentId < 0) continue;

                BundleCarryInterval bci;
                bci.type      = bundle.type;
                bci.startTime = ev.time;
                bci.endTime   = (i + 1 < bundle.events.size()) ? bundle.events[i + 1].time
                                                               : replayEnd;
                // The ground agent it was taken from, so callers can still tie the
                // carry back to the item it came from.
                if (i > 0) bci.itemAgentId = bundle.events[i - 1].worldAgentId;

                m_bundleCarry[ev.carrierAgentId].push_back(bci);
            }
        }
    }

    for (auto& [pid, intervals] : m_bundleCarry)
    {
        std::sort(intervals.begin(), intervals.end(),
            [](const BundleCarryInterval& a, const BundleCarryInterval& b) {
                return a.startTime < b.startTime;
            });

        auto ait = m_replayCtx.agents.find(pid);

        for (size_t i = 0; i < intervals.size(); i++)
        {
            auto& bci = intervals[i];

            // Picking something else up proves the earlier carry was over.
            if (i + 1 < intervals.size())
                bci.endTime = std::min(bci.endTime, intervals[i + 1].startTime);

            // Dying drops whatever was being held. Seed and repair kit spans
            // already come from the bundle's own agents, which record the death
            // drop themselves, so second-guessing them here would only cut a
            // carry short.
            bool fromTimeline = (bci.type == BundleType::VineSeed ||
                                 bci.type == BundleType::RepairKit);
            if (!fromTimeline && ait != m_replayCtx.agents.end())
            {
                for (auto& s : ait->second.snapshots)
                {
                    if (s.time <= bci.startTime) continue;
                    if (s.time >= bci.endTime) break;
                    if (s.is_dead) { bci.endTime = s.time; break; }
                }
            }

            if (bci.endTime > replayEnd)
                bci.endTime = replayEnd;
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

        ResolveCatapultLevers();
    }

    // Bundle maps are the ones where a carry can end without a weapon transition,
    // so dump the resolved intervals there to make a bad clamp traceable.
    if (!m_flagTimeline.bundles.empty())
    {
        std::ofstream dbg("door_debug.log", std::ios::app);
        for (auto& [pid, intervals] : m_bundleCarry)
            for (auto& bci : intervals)
                dbg << "[BundleCarry] player=" << pid
                    << " type=" << static_cast<int>(bci.type)
                    << " " << bci.startTime << ".." << bci.endTime
                    << " itemAgent=" << bci.itemAgentId << "\n";
    }
}


// ---------------------------------------------------------------------------
// Tie each repaired catapult to the lever gadget standing at it
//
// The map object events only carry an object id and the repair events only carry
// the position of the player who applied the kit, so the two are joined by taking
// the lever gadget nearest that player.
// ---------------------------------------------------------------------------

void ReplayWindow::ResolveCatapultLevers()
{
    m_catapultLevers.clear();
    m_catapultLeversResolved = true;

    if (m_replayCtx.mapId != kWarriorsIsleMapId) return;

    struct Gadget { int id; float x, y, z; };
    std::vector<Gadget> gadgets;
    for (auto& [aid, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;
        if (ard.categoryName != "Gate lever") continue;
        gadgets.push_back({ aid, ard.snapshots[0].x, ard.snapshots[0].y, ard.snapshots[0].z });
    }
    if (gadgets.empty()) return;

    for (auto& lc : m_leverCaps)
    {
        int   bestId = -1;
        float bestD2 = FLT_MAX;
        const Gadget* best = nullptr;
        for (auto& g : gadgets)
        {
            float dx = g.x - lc.x, dy = g.y - lc.y;
            float d2 = dx * dx + dy * dy;
            if (d2 < bestD2) { bestD2 = d2; bestId = g.id; best = &g; }
        }
        if (!best) continue;

        // Already recorded from an earlier repair of the same catapult.
        bool seen = false;
        for (auto& cl : m_catapultLevers)
            if (cl.agentId == bestId) { seen = true; break; }
        if (seen) continue;

        CatapultLever cl;
        cl.objectId     = lc.objectId;
        cl.agentId      = bestId;
        cl.x = best->x; cl.y = best->y; cl.z = best->z;
        cl.repairedTime = lc.time;
        m_catapultLevers.push_back(std::move(cl));
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
// Icon for the bundle a player is holding
// ---------------------------------------------------------------------------

// The party bars and the focused-player HUD each have a single square slot for
// this, and a player can only hold one bundle at a time, so flags, vine seeds and
// repair kits share it. Flags win the tie: their timeline is the more trustworthy
// of the two, so if both somehow claim the same player the flag is what gets shown.
ImTextureID ReplayWindow::CarriedBundleIcon(ID3D11Device* device, int agentId) const
{
    if (!m_flagTimelineBuilt || !m_flagTimeline.valid) return nullptr;

    for (int ti = 0; ti < 2; ti++)
    {
        if (m_flagTimeline.teams[ti].carrierAtTime(m_debugTimeline) != agentId) continue;
        return LoadFlagIcon(device, (ti == 0) ? "Red_flag_waving.svg.png"
                                              : "Blue_flag_waving.svg.png");
    }

    switch (GetPlayerBundleType(agentId, m_debugTimeline))
    {
    case BundleType::VineSeed:  return LoadNPCIcon(device, "Vine Seed.png");
    case BundleType::RepairKit: return LoadNPCIcon(device, "RepairKit.png");
    default: break;
    }

    return nullptr;
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

        // Once the lever has a model standing at it, the marker belongs on the
        // lever itself rather than on the spot the repairing player happened to
        // occupy, and the flat icon would only sit in front of the model.
        float mx = lc.x, my = lc.y, mz = lc.z;
        bool hasModel = false;
        for (auto& cl : m_catapultLevers)
        {
            if (cl.objectId != lc.objectId || cl.meshIds.empty()) continue;
            mx = cl.x; my = cl.y; mz = cl.z;
            hasModel = true;
            break;
        }

        ImTextureID tex = hasModel ? nullptr : LoadNPCIcon(dev, "Lever.png");
        if (!hasModel && !tex) continue;

        XMFLOAT3 pos = ApplyMapTransformToPos(mx, my, mz, t);
        float scrX, scrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, pos, scrX, scrY)) continue;

        float leverSz = iconSz * 1.2f;
        ImVec2 iconTL(scrX - leverSz * 0.5f, scrY - leverSz);
        ImVec2 iconBR(iconTL.x + leverSz, iconTL.y + leverSz);

        auto csIt = m_catapultStates.find(lc.objectId);
        if (csIt != m_catapultStates.end() && m_replayCtx.mapId == kWarriorsIsleMapId)
        {
            const CatapultLeverState& cls = csIt->second;
            std::string labelStr = cls.GetLabelText(m_debugTimeline);
            ImU32 labelCol       = cls.GetLabelColor(m_debugTimeline);
            float glowOpacity    = cls.GetGlowOpacity(m_debugTimeline);
            ImU32 glowCol        = cls.GetGlowColor(m_debugTimeline);
            CatapultState cState = cls.GetState(m_debugTimeline);

            // Soft halo behind the marker: a wide faint wash with a tighter core
            // over it, each fading to nothing at its rim.
            if (glowOpacity > 0.01f)
            {
                ImVec2 gc((iconTL.x + iconBR.x) * 0.5f, (iconTL.y + iconBR.y) * 0.5f);
                uint8_t gr = (glowCol >> IM_COL32_R_SHIFT) & 0xFF;
                uint8_t gg = (glowCol >> IM_COL32_G_SHIFT) & 0xFF;
                uint8_t gb = (glowCol >> IM_COL32_B_SHIFT) & 0xFF;
                ImU32 clear = IM_COL32(gr, gg, gb, 0);

                AddRadialGlow(dl, gc, leverSz * 2.0f,
                    IM_COL32(gr, gg, gb, (uint8_t)(glowOpacity * 70.f)), clear);
                AddRadialGlow(dl, gc, leverSz * 1.1f,
                    IM_COL32(gr, gg, gb, (uint8_t)(glowOpacity * 130.f)), clear);
            }

            if (tex)
                dl->AddImage(tex, iconTL, iconBR);

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
            if (tex)
                dl->AddImage(tex, iconTL, iconBR);

            const char* label = "Lever (Capped)";
            ImFont* font = ImGui::GetFont();
            ImVec2 textSz = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, label);
            float tx = scrX - textSz.x * 0.5f;
            float ty = iconBR.y + 2.f;
            dl->AddText(ImVec2(tx + 1.f, ty + 1.f), IM_COL32(0, 0, 0, 200), label);
            dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 230), label);
        }
    }

    // A grown bridge gets no world marker: the bridge model itself is visible, and
    // the minimap door icon carries the state.
}
