#include "pch.h"
#include "FlagTimelineBuilder.h"
#include <algorithm>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Query functions
// ---------------------------------------------------------------------------

static int BinarySearchLatest(const auto& events, float t)
{
    if (events.empty()) return -1;
    if (t < events.front().time) return -1;
    int lo = 0, hi = static_cast<int>(events.size()) - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (events[mid].time <= t) lo = mid; else hi = mid - 1;
    }
    return lo;
}

FlagLocation FlagTeamTimeline::locationAtTime(float t) const
{
    int idx = BinarySearchLatest(events, t);
    if (idx < 0) return FlagLocation::Base;
    return events[idx].newLocation;
}

int FlagTeamTimeline::carrierAtTime(float t) const
{
    int idx = BinarySearchLatest(events, t);
    if (idx < 0) return -1;
    return events[idx].carrierAgentId;
}

void FlagTeamTimeline::positionAtTime(float t, float& outX, float& outY, float& outZ) const
{
    int idx = BinarySearchLatest(events, t);
    if (idx < 0) {
        outX = spawnX; outY = spawnY; outZ = spawnZ;
        return;
    }
    outX = events[idx].x;
    outY = events[idx].y;
    outZ = events[idx].z;
}

StandOwner StandTimeline::ownerAtTime(float t) const
{
    int idx = BinarySearchLatest(events, t);
    if (idx < 0) return StandOwner::Neutral;
    return events[idx].owner;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr uint32_t kBlueExtraId = 59808;
static constexpr uint32_t kRedExtraId  = 57400;

static constexpr int kTeamCodeBlue = 20;
static constexpr int kTeamCodeRed  = 21;

static FlagTeam TeamFromExtraId(uint32_t extraId)
{
    return (extraId == kRedExtraId) ? FlagTeam::Red : FlagTeam::Blue;
}

static int TeamIndex(FlagTeam t) { return static_cast<int>(t); }

static int ResolveTeamFromItemMap(int itemId, const std::unordered_map<int, FlagTeam>& itemTeamMap)
{
    auto it = itemTeamMap.find(itemId);
    if (it != itemTeamMap.end()) return TeamIndex(it->second);
    return -1;
}

static int ResolveTeamFromPlayer(int playerAgentId, const std::unordered_map<int, AgentReplayData>* agents)
{
    if (!agents) return -1;
    auto it = agents->find(playerAgentId);
    if (it == agents->end()) return -1;
    if (it->second.teamId == 1) return 0;
    if (it->second.teamId == 2) return 1;
    return -1;
}

static int ResolveTeamFromCode(int teamCode)
{
    if (teamCode == kTeamCodeBlue) return 0;
    if (teamCode == kTeamCodeRed)  return 1;
    return -1;
}

static bool GetAgentPosition(int agentId, const std::unordered_map<int, AgentReplayData>* agents,
                             float time, float& outX, float& outY, float& outZ)
{
    if (!agents) return false;
    auto it = agents->find(agentId);
    if (it == agents->end() || it->second.snapshots.empty()) return false;
    const auto& snaps = it->second.snapshots;

    if (snaps.size() == 1 || time <= snaps.front().time) {
        outX = snaps.front().x; outY = snaps.front().y; outZ = snaps.front().z;
        return true;
    }
    if (time >= snaps.back().time) {
        outX = snaps.back().x; outY = snaps.back().y; outZ = snaps.back().z;
        return true;
    }

    int lo = 0, hi = static_cast<int>(snaps.size()) - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (snaps[mid].time <= time) lo = mid; else hi = mid - 1;
    }
    outX = snaps[lo].x; outY = snaps[lo].y; outZ = snaps[lo].z;
    return true;
}

static bool GetAgentFirstPos(int agentId, const std::unordered_map<int, AgentReplayData>* agents,
                             float& outX, float& outY, float& outZ)
{
    if (!agents) return false;
    auto it = agents->find(agentId);
    if (it == agents->end() || it->second.snapshots.empty()) return false;
    outX = it->second.snapshots.front().x;
    outY = it->second.snapshots.front().y;
    outZ = it->second.snapshots.front().z;
    return true;
}

static bool FindLifecycleAddPos(int agentId, float time,
                                const std::vector<LifecycleEvent>* lifecycle,
                                float& outX, float& outY)
{
    if (!lifecycle) return false;
    for (auto& ev : *lifecycle) {
        if (ev.isAdd && ev.agent_id == agentId && std::abs(ev.time - time) < 1.0f) {
            outX = ev.x;
            outY = ev.y;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

FlagTimeline FlagTimelineBuilder::Build(const Input& input)
{
    FlagTimeline result;
    result.valid = false;

    if (!input.flagEvents || input.flagEvents->empty())
        return result;

    const auto& fe = *input.flagEvents;

    // Phase 1: Collect all flag item_ids (team map built incrementally in Phase 4)
    std::unordered_map<int, FlagTeam> itemTeamMap;
    for (auto& e : fe.items)
        result.allFlagItemIds.insert(e.item_id);

    // Phase 2: Resolve base spawn positions
    // Strategy: for each team's first FLAG_ITEM, find its world agent via
    // lifecycle AGENT_ADD, then read the spawn coordinates. Multiple fallbacks.
    bool spawnResolved[2] = { false, false };

    for (auto& e : fe.items) {
        int ti = -1;
        if (e.extra_id == kBlueExtraId) ti = 0;
        else if (e.extra_id == kRedExtraId) ti = 1;
        if (ti < 0) continue;
        if (spawnResolved[ti]) continue;

        // Try 1: lifecycle AGENT_ADD with flag type_code near this time
        if (input.lifecycle) {
            for (auto& lc : *input.lifecycle) {
                if (!lc.isAdd) continue;
                if (std::abs(lc.time - e.time) > 2.0f) continue;
                if (IsFlagItemId(input.mapId, static_cast<uint32_t>(lc.type_code))) {
                    result.teams[ti].spawnX = lc.x;
                    result.teams[ti].spawnY = lc.y;
                    result.teams[ti].spawnZ = 0;
                    float ax, ay, az;
                    if (GetAgentFirstPos(lc.agent_id, input.agents, ax, ay, az))
                        result.teams[ti].spawnZ = az;
                    spawnResolved[ti] = true;
                    break;
                }
            }
        }

        // Try 2: any lifecycle AGENT_ADD near FLAG_ITEM time whose agent has
        // a matching item_id in its snapshots (catches non-standard type_codes)
        if (!spawnResolved[ti] && input.lifecycle && input.agents) {
            for (auto& lc : *input.lifecycle) {
                if (!lc.isAdd) continue;
                if (std::abs(lc.time - e.time) > 2.0f) continue;
                auto ait = input.agents->find(lc.agent_id);
                if (ait == input.agents->end() || ait->second.snapshots.empty()) continue;
                if (ait->second.snapshots.front().item_id == static_cast<uint32_t>(e.item_id)) {
                    result.teams[ti].spawnX = lc.x;
                    result.teams[ti].spawnY = lc.y;
                    result.teams[ti].spawnZ = ait->second.snapshots.front().z;
                    spawnResolved[ti] = true;
                    break;
                }
            }
        }

        // Try 3: scan agent snapshots for an agent whose item_id matches
        if (!spawnResolved[ti] && input.agents) {
            for (auto& [aid, ard] : *input.agents) {
                if (ard.snapshots.empty()) continue;
                if (ard.snapshots.front().item_id == static_cast<uint32_t>(e.item_id)) {
                    result.teams[ti].spawnX = ard.snapshots.front().x;
                    result.teams[ti].spawnY = ard.snapshots.front().y;
                    result.teams[ti].spawnZ = ard.snapshots.front().z;
                    spawnResolved[ti] = true;
                    break;
                }
            }
        }

        // Try 4: use item_id directly as agent_id (initial flag agent_id == item_id)
        if (!spawnResolved[ti]) {
            float ax, ay, az;
            if (GetAgentFirstPos(e.item_id, input.agents, ax, ay, az)) {
                result.teams[ti].spawnX = ax;
                result.teams[ti].spawnY = ay;
                result.teams[ti].spawnZ = az;
                spawnResolved[ti] = true;
            }
        }
    }

    // Phase 3: Resolve flagstand position and agent ID
    // The tower flagstand is identified by value=120000 (morale timer) in FLAG_STAND events.
    // Other stand_agent_ids (e.g. 1, 2) are neutral flag spawn points, not the tower.
    int towerStandId = -1;
    for (auto& e : fe.stands) {
        if (e.value == 120000) { towerStandId = e.stand_agent_id; break; }
    }
    // Fallback: find a "Tower Flag Stand" gadget in the agents map
    if (towerStandId < 0 && input.agents) {
        for (auto& [aid, ard] : *input.agents) {
            if (ard.categoryName == "Tower Flag Stand" && !ard.snapshots.empty()) {
                towerStandId = aid;
                break;
            }
        }
    }
    if (towerStandId >= 0) {
        result.stand.standAgentId = towerStandId;
        float sx, sy, sz;
        if (GetAgentFirstPos(towerStandId, input.agents, sx, sy, sz)) {
            result.stand.standX = sx;
            result.stand.standY = sy;
            result.stand.standZ = sz;
        }
    }

    // Phase 4: Merge and process events chronologically
    struct RawEvent {
        float time;
        int   code;
        int   index;
        int   priority;
    };

    std::vector<RawEvent> merged;
    merged.reserve(fe.totalCount());

    auto priorityForCode = [](int code) -> int {
        switch (code) {
        case 3: return 0;  // ITEM
        case 4: return 1;  // STAND
        case 2: return 2;  // STATE
        case 5: return 3;  // SPAWN
        case 0: return 4;  // PICKUP
        case 1: return 5;  // DROP
        case 6: return 6;  // ANNOUNCE
        default: return 99;
        }
    };

    for (int i = 0; i < static_cast<int>(fe.pickups.size()); i++)
        merged.push_back({ fe.pickups[i].time, 0, i, priorityForCode(0) });
    for (int i = 0; i < static_cast<int>(fe.drops.size()); i++)
        merged.push_back({ fe.drops[i].time, 1, i, priorityForCode(1) });
    for (int i = 0; i < static_cast<int>(fe.states.size()); i++)
        merged.push_back({ fe.states[i].time, 2, i, priorityForCode(2) });
    for (int i = 0; i < static_cast<int>(fe.items.size()); i++)
        merged.push_back({ fe.items[i].time, 3, i, priorityForCode(3) });
    for (int i = 0; i < static_cast<int>(fe.stands.size()); i++)
        merged.push_back({ fe.stands[i].time, 4, i, priorityForCode(4) });
    for (int i = 0; i < static_cast<int>(fe.spawns.size()); i++)
        merged.push_back({ fe.spawns[i].time, 5, i, priorityForCode(5) });
    for (int i = 0; i < static_cast<int>(fe.announces.size()); i++)
        merged.push_back({ fe.announces[i].time, 6, i, priorityForCode(6) });

    std::stable_sort(merged.begin(), merged.end(), [](const RawEvent& a, const RawEvent& b) {
        if (a.time != b.time) return a.time < b.time;
        return a.priority < b.priority;
    });

    int   carrier[2]      = { -1, -1 };
    float carrierSince[2] = { 0.f, 0.f };
    int   lastItemId[2]   = { -1, -1 };
    float lastGroundPos[2][3] = { {0,0,0}, {0,0,0} };

    struct SpawnInfo { int objectId; float time; };
    std::unordered_map<int, SpawnInfo> recentSpawns;

    for (auto& raw : merged)
    {
        switch (raw.code)
        {
        case 3: // FLAG_ITEM
        {
            auto& e = fe.items[raw.index];
            if (e.extra_id == kBlueExtraId || e.extra_id == kRedExtraId)
                itemTeamMap[e.item_id] = TeamFromExtraId(e.extra_id);
            break;
        }

        case 5: // FLAG_SPAWN
        {
            auto& e = fe.spawns[raw.index];
            recentSpawns[e.agent_id] = { e.object_id, e.time };
            break;
        }

        case 0: // FLAG_PICKUP
        {
            auto& e = fe.pickups[raw.index];
            int ti = ResolveTeamFromPlayer(e.player_agent_id, input.agents);
            if (ti < 0) ti = ResolveTeamFromItemMap(e.item_id, itemTeamMap);
            if (ti < 0) ti = ResolveTeamFromCode(e.team_code);
            if (ti < 0) break;

            // If this team already has a carrier, synthesize a DROP.
            // Find the actual drop moment by scanning the carrier's snapshots for
            // the weapon_type transition (0 = holding bundle -> non-zero = dropped).
            // Only scan within the current carry period (since carrierSince).
            if (carrier[ti] >= 0) {
                float dx = 0, dy = 0, dz = 0;
                float dropTime = e.time - 0.001f;
                bool dropPosFound = false;
                float scanLowerBound = carrierSince[ti];

                if (input.agents) {
                    auto cit = input.agents->find(carrier[ti]);
                    if (cit != input.agents->end() && !cit->second.snapshots.empty()) {
                        const auto& snaps = cit->second.snapshots;
                        int idx = static_cast<int>(snaps.size()) - 1;
                        for (int k = idx; k >= 0; --k) {
                            if (snaps[k].time <= e.time) { idx = k; break; }
                        }
                        for (int k = idx; k >= 1; --k) {
                            if (snaps[k].time < scanLowerBound) break;
                            if (snaps[k].weapon_type != 0 && snaps[k - 1].weapon_type == 0) {
                                dx = snaps[k].x;
                                dy = snaps[k].y;
                                dz = snaps[k].z;
                                dropTime = snaps[k].time;
                                dropPosFound = true;
                                break;
                            }
                            if (snaps[k].is_dead && !snaps[k - 1].is_dead) {
                                dx = snaps[k].x;
                                dy = snaps[k].y;
                                dz = snaps[k].z;
                                dropTime = snaps[k].time;
                                dropPosFound = true;
                                break;
                            }
                        }
                    }
                }
                if (!dropPosFound)
                    GetAgentPosition(carrier[ti], input.agents, e.time, dx, dy, dz);

                FlagTimelineEvent dropEv;
                dropEv.time           = dropTime;
                dropEv.flagTeam       = static_cast<FlagTeam>(ti);
                dropEv.eventType      = FlagTimelineEventType::Drop;
                dropEv.newLocation    = FlagLocation::Ground;
                dropEv.actorAgentId   = carrier[ti];
                dropEv.carrierAgentId = -1;
                dropEv.x = dx; dropEv.y = dy; dropEv.z = dz;
                result.teams[ti].events.push_back(dropEv);
                lastGroundPos[ti][0] = dx; lastGroundPos[ti][1] = dy; lastGroundPos[ti][2] = dz;
                carrier[ti] = -1;
            }

            float px = 0, py = 0, pz = 0;
            GetAgentPosition(e.player_agent_id, input.agents, e.time, px, py, pz);

            FlagTimelineEvent fte;
            fte.time           = e.time;
            fte.flagTeam       = static_cast<FlagTeam>(ti);
            fte.eventType      = FlagTimelineEventType::Pickup;
            fte.newLocation    = FlagLocation::Carried;
            fte.actorAgentId   = e.player_agent_id;
            fte.carrierAgentId = e.player_agent_id;
            fte.x = px; fte.y = py; fte.z = pz;
            result.teams[ti].events.push_back(fte);

            carrier[ti] = e.player_agent_id;
            carrierSince[ti] = e.time;
            lastItemId[ti] = e.item_id;
            // Clear stale SPAWN data from this pickup so a same-time DROP
            // doesn't use the pickup's object_id for position resolution
            recentSpawns.erase(e.player_agent_id);
            break;
        }

        case 1: // FLAG_DROP
        {
            auto& e = fe.drops[raw.index];
            int ti = ResolveTeamFromPlayer(e.player_agent_id, input.agents);
            if (ti < 0) ti = ResolveTeamFromCode(e.team_code);
            if (ti < 0) {
                for (int t = 0; t < 2; t++) {
                    if (carrier[t] == e.player_agent_id) { ti = t; break; }
                }
            }
            if (ti < 0) break;

            // Skip spurious drops: the GW server sometimes fires a FLAG_DROP
            // shortly after a PICKUP even though the player keeps holding the
            // flag. Validate using agent snapshot: if weapon_type is still 0
            // (holding bundle) ~1s after the alleged drop, it's bogus.
            if (carrier[ti] == e.player_agent_id && input.agents) {
                auto cit = input.agents->find(e.player_agent_id);
                if (cit != input.agents->end() && !cit->second.snapshots.empty()) {
                    const auto& snaps = cit->second.snapshots;
                    float checkTime = e.time + 1.0f;
                    const AgentSnapshot* checkSnap = nullptr;
                    for (int k = static_cast<int>(snaps.size()) - 1; k >= 0; --k) {
                        if (snaps[k].time <= checkTime) { checkSnap = &snaps[k]; break; }
                    }
                    if (checkSnap && checkSnap->time > e.time && checkSnap->weapon_type == 0)
                        break;
                }
            }

            float dx = 0, dy = 0, dz = 0;
            bool posFound = false;

            auto spawnIt = recentSpawns.find(e.player_agent_id);
            if (spawnIt != recentSpawns.end() && std::abs(spawnIt->second.time - e.time) < 1.0f) {
                int objId = spawnIt->second.objectId;
                float lx, ly;
                if (FindLifecycleAddPos(objId, e.time, input.lifecycle, lx, ly)) {
                    dx = lx; dy = ly;
                    float az;
                    if (GetAgentFirstPos(objId, input.agents, lx, ly, az))
                        dz = az;
                    posFound = true;
                }
                if (!posFound) {
                    posFound = GetAgentFirstPos(objId, input.agents, dx, dy, dz);
                }
            }
            if (!posFound) {
                posFound = GetAgentPosition(e.player_agent_id, input.agents, e.time, dx, dy, dz);
            }

            FlagTimelineEvent fte;
            fte.time           = e.time;
            fte.flagTeam       = static_cast<FlagTeam>(ti);
            fte.eventType      = FlagTimelineEventType::Drop;
            fte.newLocation    = FlagLocation::Ground;
            fte.actorAgentId   = e.player_agent_id;
            fte.carrierAgentId = -1;
            fte.x = dx; fte.y = dy; fte.z = dz;

            if (spawnIt != recentSpawns.end() && std::abs(spawnIt->second.time - e.time) < 1.0f)
                fte.flagWorldAgentId = spawnIt->second.objectId;

            result.teams[ti].events.push_back(fte);
            lastGroundPos[ti][0] = dx; lastGroundPos[ti][1] = dy; lastGroundPos[ti][2] = dz;
            carrier[ti] = -1;
            break;
        }

        case 6: // FLAG_ANNOUNCE
        {
            auto& e = fe.announces[raw.index];
            if (e.action == 1) // STICK
            {
                int ti = (e.team == 1) ? 0 : (e.team == 2) ? 1 : -1;
                if (ti < 0) break;

                FlagTimelineEvent stickEv;
                stickEv.time           = e.time;
                stickEv.flagTeam       = static_cast<FlagTeam>(ti);
                stickEv.eventType      = FlagTimelineEventType::Stick;
                stickEv.newLocation    = FlagLocation::Stand;
                stickEv.actorAgentId   = carrier[ti];
                stickEv.carrierAgentId = -1;
                stickEv.x = result.stand.standX;
                stickEv.y = result.stand.standY;
                stickEv.z = result.stand.standZ;
                stickEv.standAgentId = result.stand.standAgentId;
                result.teams[ti].events.push_back(stickEv);

                FlagTimelineEvent spawnEv;
                spawnEv.time        = e.time + 0.001f;
                spawnEv.flagTeam    = static_cast<FlagTeam>(ti);
                spawnEv.eventType   = FlagTimelineEventType::Spawn;
                spawnEv.newLocation = FlagLocation::Base;
                spawnEv.x = result.teams[ti].spawnX;
                spawnEv.y = result.teams[ti].spawnY;
                spawnEv.z = result.teams[ti].spawnZ;
                result.teams[ti].events.push_back(spawnEv);

                StandControlEvent sc;
                sc.time          = e.time;
                sc.owner         = (ti == 0) ? StandOwner::Blue : StandOwner::Red;
                sc.standAgentId  = result.stand.standAgentId;
                sc.moraleExpiry  = e.time + 120.f;
                result.stand.events.push_back(sc);

                carrier[ti] = -1;
            }
            else if (e.action == 0) // RETURN
            {
                int returnTeam = (e.team == 1) ? 0 : (e.team == 2) ? 1 : -1;
                if (returnTeam < 0) break;
                int flagTeam = 1 - returnTeam;

                // If the flag team still has a carrier, the DROP event was
                // missing from the raw data. Synthesize one using the
                // carrier's weapon_type transition or death.
                if (carrier[flagTeam] >= 0) {
                    float dx = 0, dy = 0, dz = 0;
                    float dropTime = e.time - 0.001f;
                    bool dropPosFound = false;
                    float scanLower = carrierSince[flagTeam];

                    if (input.agents) {
                        auto cit = input.agents->find(carrier[flagTeam]);
                        if (cit != input.agents->end() && !cit->second.snapshots.empty()) {
                            const auto& snaps = cit->second.snapshots;
                            int idx = static_cast<int>(snaps.size()) - 1;
                            for (int k = idx; k >= 0; --k) {
                                if (snaps[k].time <= e.time) { idx = k; break; }
                            }
                            for (int k = idx; k >= 1; --k) {
                                if (snaps[k].time < scanLower) break;
                                if (snaps[k].weapon_type != 0 && snaps[k - 1].weapon_type == 0) {
                                    dx = snaps[k].x; dy = snaps[k].y; dz = snaps[k].z;
                                    dropTime = snaps[k].time;
                                    dropPosFound = true;
                                    break;
                                }
                                if (snaps[k].is_dead && !snaps[k - 1].is_dead) {
                                    dx = snaps[k].x; dy = snaps[k].y; dz = snaps[k].z;
                                    dropTime = snaps[k].time;
                                    dropPosFound = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (!dropPosFound)
                        GetAgentPosition(carrier[flagTeam], input.agents, e.time, dx, dy, dz);

                    FlagTimelineEvent dropEv;
                    dropEv.time           = dropTime;
                    dropEv.flagTeam       = static_cast<FlagTeam>(flagTeam);
                    dropEv.eventType      = FlagTimelineEventType::Drop;
                    dropEv.newLocation    = FlagLocation::Ground;
                    dropEv.actorAgentId   = carrier[flagTeam];
                    dropEv.carrierAgentId = -1;
                    dropEv.x = dx; dropEv.y = dy; dropEv.z = dz;
                    result.teams[flagTeam].events.push_back(dropEv);
                    lastGroundPos[flagTeam][0] = dx;
                    lastGroundPos[flagTeam][1] = dy;
                    lastGroundPos[flagTeam][2] = dz;
                }

                // Detect which opposing player performed the return by checking
                // animation_code and proximity to the flag ground position
                static constexpr uint32_t kReturnAnimCodes[] = {
                    3002646805u, 3002646795u, 3002646789u
                };
                static constexpr float kReturnProxDistSq = 200.f * 200.f;
                int returnActorId = -1;

                float gx = lastGroundPos[flagTeam][0];
                float gy = lastGroundPos[flagTeam][1];

                if (input.agents && (gx != 0.f || gy != 0.f)) {
                    uint8_t returnTeamId = (returnTeam == 0) ? 1 : 2;
                    float tLo = e.time - 1.0f;
                    float tHi = e.time + 1.0f;

                    for (auto& [aid, ard] : *input.agents) {
                        if (ard.teamId != returnTeamId) continue;
                        if (ard.snapshots.empty()) continue;
                        if (ard.type != AgentType::Player) continue;

                        const auto& snaps = ard.snapshots;
                        bool found = false;
                        for (int k = 0; k < static_cast<int>(snaps.size()); ++k) {
                            if (snaps[k].time < tLo) continue;
                            if (snaps[k].time > tHi) break;

                            bool animMatch = false;
                            for (uint32_t ac : kReturnAnimCodes) {
                                if (snaps[k].animation_code == ac) { animMatch = true; break; }
                            }
                            if (!animMatch) continue;

                            float pdx = snaps[k].x - gx;
                            float pdy = snaps[k].y - gy;
                            if (pdx * pdx + pdy * pdy <= kReturnProxDistSq) {
                                returnActorId = aid;
                                found = true;
                                break;
                            }
                        }
                        if (found) break;
                    }
                }

                FlagTimelineEvent retEv;
                retEv.time        = e.time;
                retEv.flagTeam    = static_cast<FlagTeam>(flagTeam);
                retEv.eventType   = FlagTimelineEventType::Return;
                retEv.newLocation = FlagLocation::Base;
                retEv.actorAgentId = returnActorId;
                retEv.x = result.teams[flagTeam].spawnX;
                retEv.y = result.teams[flagTeam].spawnY;
                retEv.z = result.teams[flagTeam].spawnZ;
                result.teams[flagTeam].events.push_back(retEv);

                carrier[flagTeam] = -1;
            }
            break;
        }

        default:
            break;
        }
    }

    // Phase 5: Build stand control from MAP_OBJECT overcap events
    if (input.mapObject && result.stand.standAgentId >= 0) {
        uint32_t standObjId = static_cast<uint32_t>(result.stand.standAgentId);
        for (auto& mo : *input.mapObject) {
            if (mo.isState || mo.object_id != standObjId) continue;
            if (mo.animation_stage == 2) {
                bool alreadyHandled = false;
                for (auto& sc : result.stand.events) {
                    if (std::abs(sc.time - mo.time) < 0.5f) { alreadyHandled = true; break; }
                }
                if (!alreadyHandled) {
                    StandControlEvent sc;
                    sc.time  = mo.time;
                    sc.owner = StandOwner::Neutral;
                    sc.standAgentId = result.stand.standAgentId;
                    result.stand.events.push_back(sc);
                }
            }
        }
        std::stable_sort(result.stand.events.begin(), result.stand.events.end(),
            [](const StandControlEvent& a, const StandControlEvent& b) { return a.time < b.time; });
    }

    // Phase 6: Insert initial spawn events
    // Every team that has any events must start with a Spawn. Use the first
    // FLAG_ITEM time if available, otherwise time=0.
    for (int ti = 0; ti < 2; ti++) {
        result.teams[ti].team = static_cast<FlagTeam>(ti);

        if (result.teams[ti].events.empty()) continue;

        bool hasEarlySpawn = false;
        for (auto& ev : result.teams[ti].events) {
            if (ev.eventType == FlagTimelineEventType::Spawn) {
                hasEarlySpawn = true;
                break;
            }
        }
        if (!hasEarlySpawn) {
            // Find the earliest FLAG_ITEM time for this team to use as spawn time
            float spawnTime = 0.f;
            FlagTeam ft = static_cast<FlagTeam>(ti);
            for (auto& e : fe.items) {
                auto it = itemTeamMap.find(e.item_id);
                if (it != itemTeamMap.end() && it->second == ft) {
                    spawnTime = e.time;
                    break;
                }
            }

            FlagTimelineEvent initEv;
            initEv.time        = spawnTime;
            initEv.flagTeam    = ft;
            initEv.eventType   = FlagTimelineEventType::Spawn;
            initEv.newLocation = FlagLocation::Base;
            initEv.x = result.teams[ti].spawnX;
            initEv.y = result.teams[ti].spawnY;
            initEv.z = result.teams[ti].spawnZ;
            result.teams[ti].events.insert(result.teams[ti].events.begin(), initEv);
        }

        std::stable_sort(result.teams[ti].events.begin(), result.teams[ti].events.end(),
            [](const FlagTimelineEvent& a, const FlagTimelineEvent& b) { return a.time < b.time; });
    }

    // Build merged allEvents
    for (int ti = 0; ti < 2; ti++)
        for (auto& ev : result.teams[ti].events)
            result.allEvents.push_back(ev);
    std::stable_sort(result.allEvents.begin(), result.allEvents.end(),
        [](const FlagTimelineEvent& a, const FlagTimelineEvent& b) { return a.time < b.time; });

    result.valid = true;
    return result;
}
