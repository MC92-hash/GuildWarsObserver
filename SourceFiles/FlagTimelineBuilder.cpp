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

BundleLocation BundleTimeline::locationAtTime(float t) const
{
    int idx = BinarySearchLatest(events, t);
    if (idx < 0) return BundleLocation::Base;
    return events[idx].newLocation;
}

int BundleTimeline::carrierAtTime(float t) const
{
    int idx = BinarySearchLatest(events, t);
    if (idx < 0) return -1;
    return events[idx].carrierAgentId;
}

void BundleTimeline::positionAtTime(float t, float& outX, float& outY, float& outZ) const
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

static constexpr int kTeamCodeRed  = 20;
static constexpr int kTeamCodeBlue = 21;

static FlagTeam TeamFromExtraId(uint32_t extraId)
{
    return (extraId == kRedExtraId) ? FlagTeam::Blue : FlagTeam::Red;
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
    if (teamCode == kTeamCodeRed)  return 0;
    if (teamCode == kTeamCodeBlue) return 1;
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

    // Item ids are recycled between flags and the map's own bundle, so every
    // "what is this item" question below has to carry the time it is asked about.
    FlagItemRegistry localRegistry;
    if (!input.flagItems) localRegistry.Build(fe.items, input.mapId);
    const FlagItemRegistry& itemReg = input.flagItems ? *input.flagItems : localRegistry;

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
                if (itemReg.IsFlagAt(static_cast<uint32_t>(lc.type_code), lc.time)) {
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

    // Phase 3b: Resolve obelisk flagstand position and agent ID
    int obeliskStandId = -1;
    if (input.agents) {
        for (auto& [aid, ard] : *input.agents) {
            if (ard.categoryName == "Obelisk Flag Stand" && !ard.snapshots.empty()) {
                obeliskStandId = aid;
                break;
            }
        }
    }
    if (obeliskStandId >= 0) {
        result.obelisk.standAgentId = obeliskStandId;
        float sx, sy, sz;
        if (GetAgentFirstPos(obeliskStandId, input.agents, sx, sy, sz)) {
            result.obelisk.standX = sx;
            result.obelisk.standY = sy;
            result.obelisk.standZ = sz;
        }
    }

    // Phase 3c: Resolve non-flag bundles (vine seeds, repair kits). They travel
    // through the same pickup/drop packets as flags, so they have to be
    // reconstructed here or they would be mistaken for flag carries.
    //
    // Which ids belong to bundles has to be read off the world agents rather than
    // a table: the server recycles ids, so the same number can be a repair kit for
    // the first minutes of a match and a respawned flag afterwards. An agent is a
    // bundle when its id was not a declared flag at the moment the agent appeared.
    BundleType mapBundle = MapBundleType(input.mapId);
    if (mapBundle != BundleType::Unknown && input.agents)
    {
        struct Candidate { int itemId; float firstSeen; float x, y, z; };
        std::vector<Candidate> found;

        for (auto& [aid, ard] : *input.agents) {
            if (ard.snapshots.empty()) continue;
            const auto& s0 = ard.snapshots.front();
            if (s0.item_id == 0) continue;
            if (itemReg.IsFlagAt(s0.item_id, s0.time)) continue;

            int itemId = static_cast<int>(s0.item_id);
            auto it = std::find_if(found.begin(), found.end(),
                [&](const Candidate& c) { return c.itemId == itemId; });
            if (it == found.end())
                found.push_back({ itemId, s0.time, s0.x, s0.y, s0.z });
            else if (s0.time < it->firstSeen)
                *it = { itemId, s0.time, s0.x, s0.y, s0.z };
        }

        std::sort(found.begin(), found.end(),
            [](const Candidate& a, const Candidate& b) { return a.itemId < b.itemId; });

        for (auto& c : found) {
            BundleTimeline bt;
            bt.itemId = c.itemId;
            bt.type   = mapBundle;
            bt.spawnX = c.x; bt.spawnY = c.y; bt.spawnZ = c.z;
            bt.spawnResolved = true;
            result.bundles.push_back(bt);
        }
    }

    // Phase 3d: Build each bundle's timeline from its own world agents. A bundle
    // only has an agent while it is lying on the ground, so each agent's snapshot
    // span is one "on the ground" interval and the gaps between them are carries.
    // This is the same data Phase 3c reads, which avoids depending on lifecycle
    // agent ids: those are rewritten by SplitRecycledAgents before the builder ever
    // sees them. The flag packets cannot drive this: FLAG_DROP is never sent for
    // bundles and the weapon_type transitions the flag machine relies on are not
    // always recorded.
    if (!result.bundles.empty() && input.agents)
    {
        // Snapshots stop at the end of the recording, so a bundle still lying on the
        // ground then must not be turned into a carry.
        float replayEnd = 0.f;
        for (auto& [aid, ard] : *input.agents) {
            if (ard.snapshots.empty()) continue;
            replayEnd = std::max(replayEnd, ard.snapshots.back().time);
        }

        // A ground span ends for two very different reasons: somebody took the
        // bundle, or the recording simply stopped hearing about it because no
        // observed player was near enough. Only the first sends AGENT_REMOVE, and
        // reading the second as a pickup glues the bundle to a player who never
        // touched it for the rest of the match.
        bool haveLifecycle = input.lifecycle && !input.lifecycle->empty();

        auto findRemoveTime = [&](int agentId, float notBefore) -> float {
            if (!haveLifecycle) return -1.f;
            for (auto& lc : *input.lifecycle) {
                if (lc.isAdd) continue;
                if (lc.agent_id != agentId) continue;
                if (lc.time < notBefore) continue;
                return lc.time;
            }
            return -1.f;
        };

        // A bundle is consumed rather than merely dropped when the map object it
        // feeds reacts: a vine bridge growing (animation_type 16) for a seed, a
        // catapult being repaired (animation_type 2) for a repair kit. A seed goes
        // into the bridge the instant it leaves the ground, but a repair kit is
        // carried to the catapult, so the reaction can come a minute after the
        // pickup and the search has to run forward rather than around the pickup.
        // Each object is fed by exactly one bundle, so claim the event to stop two
        // bundles both matching it. The events show up in either stream depending
        // on the map, so both are searched.
        auto consumerType = [](BundleType bt) {
            return (bt == BundleType::VineSeed) ? 16 : 2;
        };
        std::vector<char> doorClaimed, objClaimed;
        if (input.doorEvents) doorClaimed.assign(input.doorEvents->size(), 0);
        if (input.mapObject)  objClaimed.assign(input.mapObject->size(), 0);

        // Earliest unclaimed reaction at or after `from`, or -1 if there is none.
        auto claimConsumerAfter = [&](float from, int animType) -> float {
            float best = -1.f;
            std::vector<char>* bestClaimed = nullptr;
            size_t bestIdx = 0;

            auto scan = [&](auto* list, std::vector<char>& claimed) {
                if (!list) return;
                for (size_t i = 0; i < list->size(); ++i) {
                    const auto& ev = (*list)[i];
                    if (claimed[i]) continue;
                    if (ev.isState) continue;
                    if (ev.animation_type != animType) continue;
                    if (ev.animation_stage != 2) continue;
                    if (ev.time < from - 1.0f) continue;
                    if (best >= 0.f && ev.time >= best) continue;
                    best = ev.time;
                    bestClaimed = &claimed;
                    bestIdx = i;
                }
            };
            scan(input.doorEvents, doorClaimed);
            scan(input.mapObject, objClaimed);

            if (bestClaimed) (*bestClaimed)[bestIdx] = 1;
            return best;
        };

        // A player already holding a flag has no free hands, so they cannot be the
        // one who took the bundle. Naming one anyway is the most damaging mistake
        // available here, because a bundle carry suppresses that player's flag drop
        // synthesis and would leave their flag stuck to them for the rest of the
        // match. Derive the flag holds from the packets so the guess below can rule
        // those players out.
        struct HoldSpan { float start, end; };
        std::unordered_map<int, std::vector<HoldSpan>> flagHolds;
        for (auto& pe : fe.pickups)
        {
            if (!itemReg.IsFlagAt(static_cast<uint32_t>(pe.item_id), pe.time))
                continue;

            float end = FLT_MAX;
            for (auto& other : fe.pickups)
                if (other.player_agent_id == pe.player_agent_id && other.time > pe.time)
                    end = std::min(end, other.time);
            for (auto& de : fe.drops)
                if (de.player_agent_id == pe.player_agent_id && de.time > pe.time)
                    end = std::min(end, de.time);
            // Hands free again, when the recording caught it.
            if (input.agents) {
                auto hit = input.agents->find(pe.player_agent_id);
                if (hit != input.agents->end()) {
                    for (auto& s : hit->second.snapshots) {
                        if (s.time <= pe.time) continue;
                        if (s.weapon_type != 0) { end = std::min(end, s.time); break; }
                    }
                }
            }
            flagHolds[pe.player_agent_id].push_back({ pe.time, end });
        }

        auto holdsFlagAt = [&](int pid, float t) -> bool {
            auto hit = flagHolds.find(pid);
            if (hit == flagHolds.end()) return false;
            for (auto& h : hit->second)
                if (t >= h.start && t < h.end) return true;
            return false;
        };

        // Whoever took the bundle: the pickup packet names them, otherwise fall back
        // to the closest player, which covers pickups the server did not announce.
        // The two are not equally trustworthy, so say which one this was and let
        // callers decide how much weight to give it.
        struct Taker { int id = -1; bool confirmed = false; };

        auto resolveTaker = [&](int itemId, float t, float gx, float gy) -> Taker {
            int   packetTaker = -1;
            float packetDt    = 2.0f;
            for (auto& pe : fe.pickups) {
                if (pe.item_id != itemId) continue;
                float dt = std::abs(pe.time - t);
                if (dt > packetDt) continue;
                packetDt    = dt;
                packetTaker = pe.player_agent_id;
            }
            if (packetTaker >= 0) return { packetTaker, true };

            int   best = -1;
            float bestSq = 600.f * 600.f;
            for (auto& [aid, ard] : *input.agents) {
                if (ard.type != AgentType::Player) continue;
                if (holdsFlagAt(aid, t)) continue;
                float px, py, pz;
                if (!GetAgentPosition(aid, input.agents, t, px, py, pz)) continue;
                float dx = px - gx, dy = py - gy;
                float d2 = dx * dx + dy * dy;
                if (d2 >= bestSq) continue;
                bestSq = d2;
                best = aid;
            }
            return { best, false };
        };

        struct GroundSpan { float start, end, x, y, z; int agentId; };

        // The last span of each bundle, resolved after the main loop. Whether a
        // bundle was spent on its map object depends on which reaction is still
        // unclaimed, so the bundle that left the ground latest gets first choice —
        // otherwise an early pickup can steal a reaction that belongs to a later one.
        struct FinalDeparture {
            size_t bundleIndex;
            float  time;
            float  x, y, z;
        };
        std::vector<FinalDeparture> finals;

        for (size_t bi = 0; bi < result.bundles.size(); ++bi)
        {
            auto& bundle = result.bundles[bi];
            std::vector<GroundSpan> spans;
            for (auto& [aid, ard] : *input.agents) {
                if (ard.snapshots.empty()) continue;
                const auto& s0 = ard.snapshots.front();
                if (static_cast<int>(s0.item_id) != bundle.itemId) continue;
                // Once this id has been handed to a flag it is no longer this
                // bundle, however many times it lands on the ground afterwards.
                if (itemReg.IsFlagAt(s0.item_id, s0.time)) continue;
                spans.push_back({ s0.time, ard.snapshots.back().time,
                                  s0.x, s0.y, s0.z, aid });
            }
            std::sort(spans.begin(), spans.end(),
                [](const GroundSpan& a, const GroundSpan& b) { return a.start < b.start; });

            if (spans.empty()) {
                // Nothing to go on. Fall back to a single Base event so the bundle
                // at least shows where Phase 3c found it.
                if (!bundle.spawnResolved) continue;
                BundleTimelineEvent ev;
                ev.newLocation = BundleLocation::Base;
                ev.x = bundle.spawnX; ev.y = bundle.spawnY; ev.z = bundle.spawnZ;
                bundle.events.push_back(ev);
                continue;
            }

            bundle.spawnX = spans.front().x;
            bundle.spawnY = spans.front().y;
            bundle.spawnZ = spans.front().z;
            bundle.spawnResolved = true;

            for (size_t i = 0; i < spans.size(); ++i)
            {
                const auto& sp = spans[i];
                bool isLast = (i + 1 == spans.size());

                BundleTimelineEvent onGround;
                onGround.time         = sp.start;
                onGround.newLocation  = (i == 0) ? BundleLocation::Base
                                                 : BundleLocation::Ground;
                onGround.x = sp.x; onGround.y = sp.y; onGround.z = sp.z;
                onGround.worldAgentId = sp.agentId;
                bundle.events.push_back(onGround);

                // Departure is the removal, not the last snapshot: the two differ
                // by a moment when the bundle is taken, and the removal is missing
                // altogether when the span merely fell out of recording range.
                float departTime = findRemoveTime(sp.agentId, sp.start);
                if (departTime < 0.f) {
                    if (haveLifecycle) continue;
                    // No lifecycle to consult; the old heuristic is all there is.
                    if (isLast && sp.end >= replayEnd - 2.0f) continue;
                    departTime = sp.end;
                }

                if (isLast) {
                    finals.push_back({ bi, departTime, sp.x, sp.y, sp.z });
                    continue;
                }

                Taker taker = resolveTaker(bundle.itemId, departTime, sp.x, sp.y);
                BundleTimelineEvent left;
                left.time = departTime;
                left.x = sp.x; left.y = sp.y; left.z = sp.z;
                left.newLocation      = BundleLocation::Carried;
                left.actorAgentId     = taker.id;
                left.carrierAgentId   = taker.id;
                left.carrierConfirmed = taker.confirmed;
                bundle.events.push_back(left);
            }
        }

        std::sort(finals.begin(), finals.end(),
            [](const FinalDeparture& a, const FinalDeparture& b) { return a.time > b.time; });

        for (auto& f : finals)
        {
            auto& bundle = result.bundles[f.bundleIndex];
            float consumeAt = claimConsumerAfter(f.time, consumerType(bundle.type));

            // Spent the instant it left the ground — a seed dropped straight into
            // a vine bridge never passes through anyone's hands.
            constexpr float kImmediate = 2.5f;
            bool immediate = (consumeAt >= 0.f && consumeAt <= f.time + kImmediate);

            if (!immediate) {
                Taker taker = resolveTaker(bundle.itemId, f.time, f.x, f.y);
                BundleTimelineEvent carried;
                carried.time = f.time;
                carried.x = f.x; carried.y = f.y; carried.z = f.z;
                carried.newLocation      = BundleLocation::Carried;
                carried.actorAgentId     = taker.id;
                carried.carrierAgentId   = taker.id;
                carried.carrierConfirmed = taker.confirmed;
                bundle.events.push_back(carried);
            }

            if (consumeAt >= 0.f) {
                BundleTimelineEvent spent;
                spent.time = std::max(consumeAt, f.time);
                spent.x = f.x; spent.y = f.y; spent.z = f.z;
                spent.newLocation = BundleLocation::Consumed;
                bundle.events.push_back(spent);
            }
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

    // The bundle timelines are already complete, so "is this player holding a seed
    // or a repair kit right now" is just a query. FLAG_DROP carries no item id and
    // the weapon_type transitions the flag machine scans for are ambiguous when a
    // player has held both kinds of bundle, so every such site consults this first.
    auto bundleCarriedBy = [&](int playerId, float t) -> bool {
        if (playerId < 0) return false;
        // The packets that ask this question arrive a few milliseconds before the
        // snapshot stream the bundle timeline is built from catches up, so look
        // slightly ahead rather than demanding the two agree to the millisecond.
        const float q = t + 0.5f;
        for (auto& bundle : result.bundles) {
            const BundleTimelineEvent* cur = nullptr;
            for (auto& ev : bundle.events) {
                if (ev.time > q) break;
                cur = &ev;
            }
            if (!cur) continue;
            if (cur->newLocation != BundleLocation::Carried) continue;
            if (cur->carrierAgentId != playerId) continue;
            // An inferred carrier is enough to draw the bundle on someone, but not
            // to silence this player's flag handling: if the guess is wrong, the
            // flag would never come off them again. A missed suppression only costs
            // a synthesized drop, which later events can still correct.
            if (!cur->carrierConfirmed) continue;
            return true;
        }
        return false;
    };

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

            // The server reuses this packet for every bundle, not just flags.
            // Route non-flag bundles away from the flag state machine, otherwise
            // carrying one shows up as carrying a flag. Maps with no bundle of
            // their own keep the old behaviour and treat everything as a flag.
            BundleType pickedType = itemReg.Classify(static_cast<uint32_t>(e.item_id), e.time);

            // Seeds and repair kits already have their own timelines from Phase 3d.
            if (pickedType == BundleType::VineSeed || pickedType == BundleType::RepairKit)
            {
                recentSpawns.erase(e.player_agent_id);
                break;
            }

            int ti = ResolveTeamFromPlayer(e.player_agent_id, input.agents);
            if (ti < 0) ti = ResolveTeamFromItemMap(e.item_id, itemTeamMap);
            if (ti < 0) ti = ResolveTeamFromCode(e.team_code);
            if (ti < 0) break;

            // If this team already has a carrier, synthesize a DROP.
            // Find the actual drop moment by scanning the carrier's snapshots for
            // the weapon_type transition (0 = holding bundle -> non-zero = dropped).
            // Only scan within the current carry period (since carrierSince).
            // Skip the scan if that player is holding a bundle now: the transition
            // would belong to the bundle, not to this flag.
            if (carrier[ti] >= 0 && !bundleCarriedBy(carrier[ti], e.time)) {
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

            // This packet does not say what was dropped. If the player is carrying a
            // seed or a repair kit, that is what is being put down, and Phase 3d has
            // already recorded it.
            if (bundleCarriedBy(e.player_agent_id, e.time))
                break;

            int ti = ResolveTeamFromPlayer(e.player_agent_id, input.agents);
            if (ti < 0) ti = ResolveTeamFromCode(e.team_code);
            if (ti < 0) {
                for (int t = 0; t < 2; t++) {
                    if (carrier[t] == e.player_agent_id) { ti = t; break; }
                }
            }
            if (ti < 0) break;

            // You cannot drop a flag you are not holding. The server sends this
            // packet to whoever picked up a repair kit as well, and taking that at
            // face value teleports the real carrier's flag to wherever the kit lay.
            if (carrier[ti] >= 0 && carrier[ti] != e.player_agent_id) break;

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
                sc.owner         = (ti == 0) ? StandOwner::Red : StandOwner::Blue;
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
                // carrier's weapon_type transition or death. Skip the scan when
                // that player is now carrying a bundle, since the transition would
                // then belong to the bundle.
                if (carrier[flagTeam] >= 0 && !bundleCarriedBy(carrier[flagTeam], e.time)) {
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

        case 4: // FLAG_STAND — detect obelisk captures
        {
            if (obeliskStandId < 0) break;
            auto& e = fe.stands[raw.index];
            if (e.stand_agent_id != obeliskStandId) break;
            if (e.sub_field != 0 || e.value != 4) break;

            // Determine capturing team from the concurrent FLAG_ITEM event
            // (more reliable than carrier check when both teams carry simultaneously).
            int ti = -1;
            for (auto& fi : fe.items) {
                if (std::abs(fi.time - e.time) > 0.01f) continue;
                if (fi.extra_id == kBlueExtraId)      { ti = 0; break; }
                else if (fi.extra_id == kRedExtraId)   { ti = 1; break; }
            }

            // Fallback: check which team currently has a carrier
            if (ti < 0) {
                for (int t = 0; t < 2; t++) {
                    if (carrier[t] >= 0) { ti = t; break; }
                }
            }
            if (ti < 0) break;

            FlagTimelineEvent stickEv;
            stickEv.time           = e.time;
            stickEv.flagTeam       = static_cast<FlagTeam>(ti);
            stickEv.eventType      = FlagTimelineEventType::Stick;
            stickEv.newLocation    = FlagLocation::Stand;
            stickEv.actorAgentId   = carrier[ti];
            stickEv.carrierAgentId = -1;
            stickEv.x = result.obelisk.standX;
            stickEv.y = result.obelisk.standY;
            stickEv.z = result.obelisk.standZ;
            stickEv.standAgentId = obeliskStandId;
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
            sc.time         = e.time;
            sc.owner        = (ti == 0) ? StandOwner::Red : StandOwner::Blue;
            sc.standAgentId = obeliskStandId;
            sc.moraleExpiry = 0.f;
            result.obelisk.events.push_back(sc);

            carrier[ti] = -1;
            break;
        }

        default:
            break;
        }
    }

    // Phase 4b: Synthesize drops for carriers still active after all events.
    // The GW server sometimes omits FLAG_DROP packets. If no subsequent pickup
    // or return cleared the carrier, scan snapshots for the weapon_type
    // transition (0 -> non-zero) or death to find the actual drop moment.
    for (int ti = 0; ti < 2; ti++) {
        if (carrier[ti] < 0 || !input.agents) continue;

        // If this player has since picked up a bundle, the transition below would
        // describe the bundle instead of the flag.
        if (bundleCarriedBy(carrier[ti], carrierSince[ti])) continue;

        auto cit = input.agents->find(carrier[ti]);
        if (cit == input.agents->end() || cit->second.snapshots.empty()) continue;

        const auto& snaps = cit->second.snapshots;
        float scanLower = carrierSince[ti];
        float dropTime = -1.f;
        float dx = 0, dy = 0, dz = 0;

        for (int k = 1; k < static_cast<int>(snaps.size()); ++k) {
            if (snaps[k].time < scanLower) continue;
            if (snaps[k].weapon_type != 0 && snaps[k - 1].weapon_type == 0) {
                dropTime = snaps[k].time;
                dx = snaps[k].x; dy = snaps[k].y; dz = snaps[k].z;
                break;
            }
            if (snaps[k].is_dead && !snaps[k - 1].is_dead) {
                dropTime = snaps[k].time;
                dx = snaps[k].x; dy = snaps[k].y; dz = snaps[k].z;
                break;
            }
        }

        if (dropTime >= 0.f) {
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

    // Phase 5b: Build obelisk stand control from MAP_OBJECT overcap events
    if (input.mapObject && result.obelisk.standAgentId >= 0) {
        uint32_t obeliskObjId = static_cast<uint32_t>(result.obelisk.standAgentId);
        for (auto& mo : *input.mapObject) {
            if (mo.isState || mo.object_id != obeliskObjId) continue;
            if (mo.animation_stage == 2) {
                bool alreadyHandled = false;
                for (auto& sc : result.obelisk.events) {
                    if (std::abs(sc.time - mo.time) < 0.5f) { alreadyHandled = true; break; }
                }
                if (!alreadyHandled) {
                    StandControlEvent sc;
                    sc.time  = mo.time;
                    sc.owner = StandOwner::Neutral;
                    sc.standAgentId = result.obelisk.standAgentId;
                    result.obelisk.events.push_back(sc);
                }
            }
        }
        std::stable_sort(result.obelisk.events.begin(), result.obelisk.events.end(),
            [](const StandControlEvent& a, const StandControlEvent& b) { return a.time < b.time; });
    }

    // Phase 6: Insert initial spawn events
    // Every team whose flag exists must start with a Spawn. Use the first
    // FLAG_ITEM time if available, otherwise time=0.
    for (int ti = 0; ti < 2; ti++) {
        result.teams[ti].team = static_cast<FlagTeam>(ti);

        // A flag nobody ever touches produces no events at all, but it still needs
        // its Spawn so consumers know it is sitting at its base. Phase 2 only sets
        // spawnResolved when a real world agent carried the flag's item id, so this
        // cannot invent a flag on a map that has none.
        if (result.teams[ti].events.empty() && !spawnResolved[ti]) continue;

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

    for (auto& bundle : result.bundles)
        std::stable_sort(bundle.events.begin(), bundle.events.end(),
            [](const BundleTimelineEvent& a, const BundleTimelineEvent& b) { return a.time < b.time; });

    // Build merged allEvents
    for (int ti = 0; ti < 2; ti++)
        for (auto& ev : result.teams[ti].events)
            result.allEvents.push_back(ev);
    std::stable_sort(result.allEvents.begin(), result.allEvents.end(),
        [](const FlagTimelineEvent& a, const FlagTimelineEvent& b) { return a.time < b.time; });

    result.valid = true;
    return result;
}
