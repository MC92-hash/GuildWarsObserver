#include "pch.h"
#include "TeamColors.h"
#include "ReplayWindow.h"
#include "MatchStats.h"
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


void ReplayWindow::BuildTimelineData()
{
    if (m_timeline.computed) return;
    m_timeline.computed = true;

    float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
    int totalSec = static_cast<int>(std::ceil(maxT));

    m_timeline.redHealth.resize(totalSec + 1, 100.f);
    m_timeline.blueHealth.resize(totalSec + 1, 100.f);

    auto sampleTeamHealth = [&](const std::vector<int>& ids, std::vector<float>& out) {
        for (int s = 0; s <= totalSec; ++s)
        {
            float t = static_cast<float>(s);
            float sumHp = 0.f, sumMaxHp = 0.f;
            for (int id : ids)
            {
                auto it = m_replayCtx.agents.find(id);
                if (it == m_replayCtx.agents.end()) continue;
                const auto& ard = it->second;
                if (ard.type != AgentType::Player) continue;
                const AgentSnapshot* snap = FindSnapshotAtTime(ard, t);
                if (!snap) continue;
                float mhp = static_cast<float>(snap->max_hp > 0 ? snap->max_hp : 480);
                sumMaxHp += mhp;
                if (snap->is_dead)
                    sumHp += 0.f;
                else
                    sumHp += snap->health_pct * mhp;
            }
            out[s] = (sumMaxHp > 0.f) ? (sumHp / sumMaxHp * 100.f) : 0.f;
        }
    };

    sampleTeamHealth(m_team1PlayerIds, m_timeline.blueHealth);
    sampleTeamHealth(m_team2PlayerIds, m_timeline.redHealth);

    auto teamForAgent = [&](int agentId) -> int {
        for (int id : m_team1PlayerIds) if (id == agentId) return 1;
        for (int id : m_team2PlayerIds) if (id == agentId) return 2;
        return 0;
    };

    // Deaths & resurrections from snapshot transitions
    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Player) continue;
        int team = teamForAgent(agentId);
        for (size_t si = 1; si < ard.snapshots.size(); ++si)
        {
            const auto& prev = ard.snapshots[si - 1];
            const auto& cur  = ard.snapshots[si];
            if (cur.is_dead && !prev.is_dead)
            {
                TimelineEvent ev;
                ev.time = cur.time;
                ev.type = TimelineEventType::Death;
                ev.agentId = agentId;
                ev.teamId = team;
                ev.professionId = ard.primaryProf;
                ev.label = ard.playerName.empty() ? ard.partyBarLabel : ard.playerName;
                m_timeline.events.push_back(ev);
            }
            if (!cur.is_dead && prev.is_dead)
            {
                TimelineEvent ev;
                ev.time = cur.time;
                ev.type = TimelineEventType::Resurrection;
                ev.agentId = agentId;
                ev.teamId = team;
                ev.professionId = ard.primaryProf;
                ev.label = ard.playerName.empty() ? ard.partyBarLabel : ard.playerName;
                m_timeline.events.push_back(ev);
            }
        }
    }

    // Jumbo events
    auto jumboTeamId = [](int pv) -> int {
        return (pv == 1635021873) ? 1 : (pv == 1635021874) ? 2 : 0;
    };

    for (auto& ev : m_replayCtx.stocData.jumbo)
    {
        TimelineEvent te;
        te.time = ev.time;
        te.teamId = jumboTeamId(ev.party_value);

        if (ev.message == "CAPTURED_TOWER") {
            te.type = TimelineEventType::FlagCapture;
            te.label = Team::Name(te.teamId) + std::string(" captured tower");
        }
        else if (ev.message == "MORALE_BOOST") {
            te.type = TimelineEventType::MoraleBoost;
            te.label = Team::Name(te.teamId) + std::string(" morale boost");
        }
        else if (ev.message == "GUILD_LORD_UNDER_ATTACK" || ev.message == "BASE_UNDER_ATTACK") {
            te.type = TimelineEventType::LordAttacked;
            te.label = Team::Name(te.teamId) + std::string(" lord under attack");
        }
        else if (ev.message == "VICTORY" || ev.message == "FLAWLESS_VICTORY") {
            te.type = TimelineEventType::Victory;
            te.label = Team::Name(te.teamId) + std::string(" victory!");
        }
        else if (ev.message == "CAPTURED_SHRINE") {
            te.type = TimelineEventType::ShrineCaptured;
            te.label = Team::Name(te.teamId) + std::string(" captured shrine");
        }
        else if (ev.message == "NEUTRALIZED_SHRINE") {
            // party_value identifies the losing team — invert for display
            if (te.teamId > 0) te.teamId = Team::Other(te.teamId);
            te.type = TimelineEventType::ShrineNeutralized;
            te.label = Team::Name(te.teamId) + std::string(" neutralized shrine");
        }
        else continue;

        m_timeline.events.push_back(te);
    }

    // Add flag return events from the flag timeline
    if (m_flagTimelineBuilt && m_flagTimeline.valid) {
        for (auto& ev : m_flagTimeline.allEvents) {
            if (ev.eventType != FlagTimelineEventType::Return) continue;
            TimelineEvent te;
            te.time = ev.time;
            te.type = TimelineEventType::FlagReturn;
            te.teamId = (ev.flagTeam == FlagTeam::Red) ? Team::Red : Team::Blue;
            te.label = "Flag returned";
            m_timeline.events.push_back(std::move(te));
        }

        // Add obelisk capture events
        for (auto& sc : m_flagTimeline.obelisk.events) {
            if (sc.owner == StandOwner::Neutral) continue;
            TimelineEvent te;
            te.time = sc.time;
            te.type = TimelineEventType::ObeliskCapture;
            te.teamId = (sc.owner == StandOwner::Red) ? Team::Red : Team::Blue;
            te.label = Team::Name(te.teamId) + std::string(" captured obelisk");
            m_timeline.events.push_back(std::move(te));
        }
    }

    // Catapult levers (Warrior's Isle). Read straight from the map object stream
    // rather than m_catapultStates, which is built on its own schedule.
    if (m_replayCtx.mapId == kWarriorsIsleMapId)
    {
        for (auto& moe : m_replayCtx.stocData.mapObject)
        {
            if (moe.isState) continue;
            if (moe.animation_stage != 2) continue;

            // Reloads are frequent and say little on their own, so only the repair
            // and the shots earn a marker here.
            CatapultState cs = CatapultState::Unknown;
            if (moe.animation_type == 2)       cs = CatapultState::Repaired;
            else if (moe.animation_type == 12) cs = CatapultState::Fired;
            if (cs == CatapultState::Unknown) continue;

            TimelineEvent te;
            te.time           = moe.time;
            te.type           = TimelineEventType::Catapult;
            te.catapultState  = cs;
            te.label          = CatapultStateName(cs);
            m_timeline.events.push_back(std::move(te));
        }
    }

    std::sort(m_timeline.events.begin(), m_timeline.events.end(),
        [](const TimelineEvent& a, const TimelineEvent& b) { return a.time < b.time; });
}


// ---------------------------------------------------------------------------
// ExportMatchAnalysis - the Library's cached copy of the event timeline
//
// Written next to the recording by `--export-match-analysis`, so the Library can show deaths,
// flag captures and the rest on its interval strip without opening the replay. Times are on the
// DISPLAYED clock, the one the Library's match length is on: 0:00 is the gate opening, and the
// minute before it (gate_seconds) is kept, as the replay shows it, with negative times.
// Bump kVersion whenever the content changes; the Library re-runs the export on a mismatch.
// ---------------------------------------------------------------------------
bool ReplayWindow::ExportMatchAnalysis(const std::filesystem::path& outPath) const
{
    static constexpr int kVersion = 8;

    auto typeName = [](TimelineEventType t) -> const char* {
        switch (t)
        {
        case TimelineEventType::Death:             return "death";
        case TimelineEventType::Resurrection:      return "resurrection";
        case TimelineEventType::FlagCapture:       return "flag_capture";
        case TimelineEventType::FlagReturn:        return "flag_return";
        case TimelineEventType::MoraleBoost:       return "morale_boost";
        case TimelineEventType::LordAttacked:      return "lord_attacked";
        case TimelineEventType::Victory:           return "victory";
        case TimelineEventType::ShrineCaptured:    return "shrine_captured";
        case TimelineEventType::ShrineNeutralized: return "shrine_neutralized";
        case TimelineEventType::ObeliskCapture:    return "obelisk_capture";
        case TimelineEventType::Catapult:          return "catapult";
        }
        return "other";
    };

    nlohmann::json events = nlohmann::json::array();
    for (const auto& e : m_timeline.events)
    {
        events.push_back({
            { "t",     e.time - m_displayTimeOffset },
            { "type",  typeName(e.type) },
            { "team",  e.teamId },
            { "agent", e.agentId },
            { "prof",  e.professionId },
            { "label", e.label },
        });
    }

    // Team health per second of the recording, the gate minute included (0-100, whole percent):
    // the curves the timeline draws. Team 1 is blue, team 2 red, as in BuildTimelineData.
    const int gate = std::max(0, (int)std::lround(m_displayTimeOffset));
    auto healthCurve = [](const std::vector<float>& perSecond) {
        nlohmann::json out = nlohmann::json::array();
        for (size_t s = 0; s < perSecond.size(); ++s)
            out.push_back((int)std::lround(std::clamp(perSecond[s], 0.f, 100.f)));
        return out;
    };

    // Positions once per second of the recording, from its start (raw snapshot coordinates, before
    // the map transform), flattened as x, y, z, state: 0 alive, 1 dead, 2 not there (before the
    // agent appears or after it leaves). The Library's Map tab places them on the map's top-down
    // picture (Textures/Map_Pictures, made by CaptureTopDownMapImage).
    const int seconds = std::max(0, (int)std::ceil(m_replayCtx.maxReplayTime));
    auto track = [&](const AgentReplayData& ard) {
        std::vector<int> pos;
        pos.reserve((size_t)(seconds + 1) * 4);
        const float first = ard.snapshots.front().time, last = ard.snapshots.back().time;
        for (int s = 0; s <= seconds; ++s)
        {
            const float t = (float)s;
            float x, y, z;
            InterpolateAgentPosition(ard, t, m_replayCtx.interpSettings, x, y, z);
            const AgentSnapshot* snap = FindSnapshotAtTime(ard, t);
            pos.push_back((int)std::lround(x));
            pos.push_back((int)std::lround(y));
            pos.push_back((int)std::lround(z));
            pos.push_back(ard.type != AgentType::Player && (t < first || t > last) ? 2
                          : snap && snap->is_dead ? 1 : 0);
        }
        return pos;
    };
    nlohmann::json players = nlohmann::json::array();
    auto addTeam = [&](const std::vector<int>& ids, int team) {
        for (int id : ids)
        {
            auto it = m_replayCtx.agents.find(id);
            if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) continue;
            const AgentReplayData& ard = it->second;
            std::vector<int> pos = track(ard);
            players.push_back({
                { "agent", id },
                { "team",  team },
                { "prof",  ard.primaryProf },
                { "name",  ard.playerName.empty() ? ard.partyBarLabel : ard.playerName },
                { "pos",   std::move(pos) },
            });
        }
    };
    addTeam(m_team1PlayerIds, 1);
    addTeam(m_team2PlayerIds, 2);

    // NPCs, as the replay minimap shows them: guild lords, guards, archers, bodyguards...
    nlohmann::json npcs = nlohmann::json::array();
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::NPC || ard.snapshots.empty()) continue;
        npcs.push_back({
            { "team", (int)ard.teamId },
            { "lord", ard.categoryName.find("Guild Lord") != std::string::npos },
            { "name", GetAgentLabel(ard) },
            { "pos",  track(ard) },
        });
    }

    // Fixed landmarks for the Map tab: guild lords and flag stands, where they first stood.
    nlohmann::json landmarks = nlohmann::json::array();
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;
        const char* kind = nullptr;
        if (ard.categoryName.find("Guild Lord") != std::string::npos)  kind = "lord";
        else if (ard.categoryName == "Tower Flag Stand")                kind = "flag_stand";
        else if (ard.type == AgentType::ObeliskFlagStand
                 || ard.categoryName == "Obelisk Flag Stand")           kind = "obelisk_stand";
        if (!kind) continue;
        const auto& s0 = ard.snapshots.front();
        landmarks.push_back({ { "kind", kind }, { "team", (int)ard.teamId },
                              { "x", (int)std::lround(s0.x) }, { "y", (int)std::lround(s0.y) },
                              { "z", (int)std::lround(s0.z) } });
    }

    // ── Map objects, by the replay minimap's rules (ReplayWindow_Minimap.cpp) ──
    // Each state is sampled once per second of the recording and stored as its changes:
    // [display time, value]. Positions are raw snapshot coordinates.
    const float dispOff = m_displayTimeOffset;
    auto changes = [&](auto valueAt) {
        nlohmann::json out = nlohmann::json::array();
        int last = INT_MIN;
        for (int s = 0; s <= seconds; ++s)
        {
            const int v = valueAt((float)s);
            if (v != last) { out.push_back({ s - dispOff, v }); last = v; }
        }
        return out;
    };
    auto ownerTeam = [](StandOwner o) { return o == StandOwner::Red ? (int)Team::Red
                                             : o == StandOwner::Blue ? (int)Team::Blue : 0; };
    nlohmann::json objects = nlohmann::json::object();

    // Flag stand and obelisk stand: who holds them.
    if (m_flagTimelineBuilt && m_flagTimeline.valid)
    {
        objects["stand_owner"] = changes([&](float t) { return ownerTeam(m_flagTimeline.stand.ownerAtTime(t)); });
        if (m_flagTimeline.obelisk.standAgentId >= 0)
            objects["obelisk_owner"] = changes([&](float t) { return ownerTeam(m_flagTimeline.obelisk.ownerAtTime(t)); });

        // Each team's flag while it is off its stand: x, y, z, state (0 hidden - on a stand,
        // 1 carried - at its carrier, 2 at its base or on the ground).
        nlohmann::json flags = nlohmann::json::array();
        for (int ti = 0; ti < 2; ++ti)
        {
            const auto& ft = m_flagTimeline.teams[ti];
            if (ft.events.empty()) continue;
            std::vector<int> pos;
            pos.reserve((size_t)(seconds + 1) * 4);
            for (int s = 0; s <= seconds; ++s)
            {
                const float t = (float)s;
                const FlagLocation loc = ft.locationAtTime(t);
                float x = 0, y = 0, z = 0;
                int state = 0;
                if (loc != FlagLocation::Stand)
                {
                    const int carrier = loc == FlagLocation::Carried ? ft.carrierAtTime(t) : -1;
                    auto cit = carrier >= 0 ? m_replayCtx.agents.find(carrier) : m_replayCtx.agents.end();
                    if (cit != m_replayCtx.agents.end() && !cit->second.snapshots.empty())
                        InterpolateAgentPosition(cit->second, t, m_replayCtx.interpSettings, x, y, z);
                    else
                        ft.positionAtTime(t, x, y, z);
                    state = loc == FlagLocation::Carried ? 1 : 2;
                }
                pos.push_back((int)std::lround(x)); pos.push_back((int)std::lround(y));
                pos.push_back((int)std::lround(z)); pos.push_back(state);
            }
            flags.push_back({ { "team", Team::IndexIsRed(ti) ? (int)Team::Red : (int)Team::Blue },
                              { "pos", std::move(pos) } });
        }
        objects["flags"] = std::move(flags);

        // Vine seeds and repair kits: gone once spent; not shown at an unresolved spawn.
        nlohmann::json bundles = nlohmann::json::array();
        for (const auto& bundle : m_flagTimeline.bundles)
        {
            if (bundle.events.empty()) continue;
            if (bundle.type != BundleType::VineSeed && bundle.type != BundleType::RepairKit) continue;
            std::vector<int> pos;
            pos.reserve((size_t)(seconds + 1) * 4);
            for (int s = 0; s <= seconds; ++s)
            {
                const float t = (float)s;
                const BundleLocation loc = bundle.locationAtTime(t);
                float x = 0, y = 0, z = 0;
                int state = 0;
                if (loc != BundleLocation::Consumed && !(loc == BundleLocation::Base && !bundle.spawnResolved))
                {
                    const int carrier = loc == BundleLocation::Carried ? bundle.carrierAtTime(t) : -1;
                    auto cit = carrier >= 0 ? m_replayCtx.agents.find(carrier) : m_replayCtx.agents.end();
                    if (cit != m_replayCtx.agents.end() && !cit->second.snapshots.empty())
                        InterpolateAgentPosition(cit->second, t, m_replayCtx.interpSettings, x, y, z);
                    else
                        bundle.positionAtTime(t, x, y, z);
                    state = loc == BundleLocation::Carried ? 1 : 2;
                }
                pos.push_back((int)std::lround(x)); pos.push_back((int)std::lround(y));
                pos.push_back((int)std::lround(z)); pos.push_back(state);
            }
            bundles.push_back({ { "type", bundle.type == BundleType::VineSeed ? "seed" : "kit" },
                                { "pos", std::move(pos) } });
        }
        objects["bundles"] = std::move(bundles);
    }

    // Isle of Wurms southern health shrine: its owner.
    if (!m_wurmsShrineSamples.empty())
    {
        const int lastIdx = (int)m_wurmsShrineSamples.size() - 1;
        objects["shrine_owner"] = changes([&](float t) {
            return (int)m_wurmsShrineSamples[std::clamp((int)(t / m_wurmsShrineSampleDt), 0, lastIdx)].ownerTeam;
        });
    }

    // Gates, doors and vine bridges: open (1) or closed (0) per door type, through the same rule
    // that animates them in the replay (DoorStatesAt). Their positions come with the map picture.
    {
        nlohmann::json doors = nlohmann::json::array();
        bool prev[kDoorTypeCount];
        bool cur[kDoorTypeCount];
        for (int s = 0; s <= seconds; ++s)
        {
            DoorStatesAt((float)s, cur);
            for (int d = 1; d < kDoorTypeCount; ++d)
                if (s == 0 || cur[d] != prev[d])
                    doors.push_back({ s - dispOff, d, cur[d] ? 1 : 0 });
            std::copy(std::begin(cur), std::end(cur), std::begin(prev));
        }
        objects["doors"] = std::move(doors);
    }

    // Warrior's Isle catapult levers: shown once repaired, the border following the firing cycle.
    if (m_replayCtx.mapId == kWarriorsIsleMapId)
    {
        nlohmann::json catapults = nlohmann::json::array();
        for (const auto& cl : m_catapultLevers)
        {
            auto csIt = m_catapultStates.find(cl.objectId);
            nlohmann::json states = nlohmann::json::array();
            int last = -1;
            // Loading is derived from the time since the load, so sample finer than a second.
            for (float t = 0.f; t <= (float)seconds; t += 0.25f)
            {
                const int st = (int)(csIt != m_catapultStates.end() ? csIt->second.GetState(t) : CatapultState::Repaired);
                if (st != last) { states.push_back({ t - dispOff, st }); last = st; }
            }
            catapults.push_back({ { "x", (int)std::lround(cl.x) }, { "y", (int)std::lround(cl.y) },
                                  { "z", (int)std::lround(cl.z) }, { "repaired", cl.repairedTime - dispOff },
                                  { "states", std::move(states) } });
        }
        objects["catapults"] = std::move(catapults);
    }

    // Shrines as fixed landmarks: the southern health shrine, and resurrection shrines with
    // the team of the nearest guild lord (as the minimap attributes them).
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;
        const auto& s0 = ard.snapshots.front();
        const bool health = ard.categoryName == "Southern Health Shrine";
        const bool res = ard.categoryName == "Resurrection Shrine";
        if (!health && !res) continue;
        int team = 0;
        if (res)
        {
            float best = FLT_MAX;
            for (const auto& [lid, lord] : m_replayCtx.agents)
            {
                if (lord.snapshots.empty() || lord.categoryName.find("Guild Lord") == std::string::npos) continue;
                const auto& l0 = lord.snapshots.front();
                const float dx = l0.x - s0.x, dy = l0.y - s0.y, dz = l0.z - s0.z;
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < best) { best = d2; team = (int)lord.teamId; }
            }
        }
        landmarks.push_back({ { "kind", health ? "health_shrine" : "res_shrine" }, { "team", team },
                              { "x", (int)std::lround(s0.x) }, { "y", (int)std::lround(s0.y) },
                              { "z", (int)std::lround(s0.z) } });
    }

    // The Damage & Healing ledger (ReplayWindow_BattleLedger.cpp), for the Library's Stats tab: its
    // lines, interrupt attempts and the players' skill uses, times in milliseconds on the displayed
    // clock, and every agent they name. Flat arrays: a match holds tens of thousands of lines.
    if (!m_ledgerBuilt || (!m_ledgerUsedAttributes && m_attributesDeduced)) BuildBattleLedger();
    nlohmann::json ledger = nlohmann::json::object();
    {
        const BattleLedger::Data& d = m_ledgerData;
        auto ms = [&](float t) { return (int)std::lround((t - dispOff) * 1000.f); };
        std::unordered_set<int> named(d.players.begin(), d.players.end());
        nlohmann::json lines = nlohmann::json::array();
        for (const auto& ln : d.lines)
        {
            lines.push_back({ ms(ln.t), (int)ln.kind, ln.src, ln.dst, ln.amount, ln.skillId, ln.rowKey,
                              (ln.weaponHit ? 1 : 0) | (ln.estimated ? 2 : 0), ln.count, (int)ln.preventGroup });
            named.insert(ln.src); named.insert(ln.dst);
        }
        nlohmann::json ints = nlohmann::json::array();
        for (const auto& li : d.ints)
        {
            ints.push_back({ ms(li.t), li.src, li.dst, li.skillId, (int)li.outcome, li.lostSkill, li.afterMs, li.reflexMs });
            named.insert(li.src); named.insert(li.dst);
        }
        nlohmann::json uses = nlohmann::json::array();
        for (const auto& [pid, list] : d.uses)
            for (const auto& u : list)
            {
                uses.push_back({ pid, ms(u.start), ms(u.end), u.skillId, u.target, (u.interrupted ? 1 : 0) | (u.cancelled ? 2 : 0) });
                named.insert(u.target);
            }
        nlohmann::json agents = nlohmann::json::array();
        for (int id : named)
            if (auto it = d.agents.find(id); it != d.agents.end())
                agents.push_back({ id, it->second.name, it->second.team, it->second.prof, it->second.sec, it->second.lord ? 1 : 0 });
        ledger = { { "players", d.players }, { "agents", std::move(agents) }, { "lines", std::move(lines) },
                   { "ints", std::move(ints) }, { "uses", std::move(uses) },
                   { "guilds", { d.guild[0], d.guild[1] } }, { "tags", { d.tag[0], d.tag[1] } } };
    }

    // The Stats tab's extra columns (MatchStats): per stat key, its records as flat arrays
    // [ms, agent, value, dur ms, other, skill, flag], times on the displayed clock.
    nlohmann::json stats = nlohmann::json::object();
    {
        auto ms = [&](float t) { return (int)std::lround((t - dispOff) * 1000.f); };
        MatchStats::Inputs in;
        FillMatchStatsInputs(in);
        for (const auto& [key, recs] : MatchStats::Build(in))
        {
            nlohmann::json list = nlohmann::json::array();
            for (const auto& r : recs)
                list.push_back({ ms(r.t), r.agent, std::round(r.value * 1000.f) / 1000.f, (int)std::lround(r.dur * 1000.f),
                                 r.other, r.skill, r.flag });
            stats[key] = std::move(list);
        }
    }

    // The solved builds, the --export-attributes file verbatim: the Library's Overview shows each
    // player's attributes and copies them into the template. null when nothing was solved.
    nlohmann::json attributes = nullptr;
    {
        std::filesystem::path attrTmp = outPath;
        attrTmp += ".attributes.tmp";
        if (ExportAttributes(attrTmp))
        {
            std::ifstream in(attrTmp, std::ios::binary);
            attributes = nlohmann::json::parse(in, nullptr, false);
            if (attributes.is_discarded()) attributes = nullptr;
        }
        std::error_code ec;
        std::filesystem::remove(attrTmp, ec);
    }

    nlohmann::json root = {
        { "version",        kVersion },
        { "attributes",     std::move(attributes) },
        { "display_offset", m_displayTimeOffset },
        { "gate_seconds",   gate },
        { "replay_length",  m_replayCtx.maxReplayTime },
        { "map_id",         m_replayCtx.mapId },
        { "events",         std::move(events) },
        { "health_team1",   healthCurve(m_timeline.blueHealth) },
        { "health_team2",   healthCurve(m_timeline.redHealth) },
        { "players",        std::move(players) },
        { "npcs",           std::move(npcs) },
        { "landmarks",      std::move(landmarks) },
        { "objects",        std::move(objects) },
        { "ledger",         std::move(ledger) },
        { "stats",          std::move(stats) },
    };

    // Written beside and renamed over, so a reader never sees half a file.
    std::filesystem::path tmp = outPath;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << root.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, outPath, ec);
    return !ec;
}


// ---------------------------------------------------------------------------
// DrawEventTimeline — full-width panel above playback bar
// ---------------------------------------------------------------------------
void ReplayWindow::DrawEventTimeline()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float vpW = vp->Size.x;
    const float vpH = vp->Size.y;

    // Republished every frame: viewport bottom means "nothing here", so a HUD reading it can
    // just take the minimum against whatever else it has to clear.
    m_eventTimelineTopY = vp->Pos.y + vpH;

    if (!m_showEventTimeline || !m_timeline.computed) return;

    const float playbarH = 76.f;
    const float panelH   = 160.f;
    const float topPad   = 6.f;
    const float scrubH   = 16.f;
    const float chartH   = panelH - topPad - scrubH;

    // Match playback bar's internal layout for horizontal alignment
    const float PAD    = 12.f;
    const float timeW  = 80.f;
    const float badgeW = 64.f;

    float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
    auto* dev = m_deviceResources->GetD3DDevice();

    ImVec2 panelPos(vp->Pos.x, vp->Pos.y + vpH - playbarH - panelH);
    m_eventTimelineTopY = panelPos.y;
    ImGui::SetNextWindowPos(panelPos);
    ImGui::SetNextWindowSize(ImVec2(vpW, panelH));

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar      | ImGuiWindowFlags_NoResize       |
        ImGuiWindowFlags_NoMove          | ImGuiWindowFlags_NoScrollbar    |
        ImGuiWindowFlags_NoCollapse      | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground    | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (!ImGui::Begin("##EventTimeline", nullptr, kFlags))
    {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::PopStyleVar();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 O = ImGui::GetWindowPos();
    ImFont* font = ImGui::GetFont();
    const float fs = font->FontSize;

    // ── Palette ──────────────────────────────────────────────────────────
    const ImU32 cBg         = IM_COL32( 10,  14,  18, 166);
    const ImU32 cGlass      = IM_COL32( 18,  16,  10, 100);
    const ImU32 cBorderHi   = IM_COL32(200, 168,  75,  82);
    const ImU32 cText       = IM_COL32(232, 223, 200, 255);
    const ImU32 cTextDim    = IM_COL32(120, 108,  80, 255);
    const ImU32 cGrid       = IM_COL32(255, 255, 255,   8);
    const ImU32 cBlue       = IM_COL32( 74, 144, 216, 255);
    const ImU32 cBlueFill   = IM_COL32( 74, 144, 216,  18);
    const ImU32 cRed        = IM_COL32(208,  72,  72, 255);
    const ImU32 cRedFill    = IM_COL32(208,  72,  72,  18);
    const ImU32 cPlayhead   = IM_COL32(255, 215, 100, 200);

    // ── Background ──────────────────────────────────────────────────────
    dl->AddRectFilled(O, ImVec2(O.x + vpW, O.y + panelH), cBg, 0.f);
    {
        float px = O.x + 2.f, py = O.y + 2.f;
        float pw = vpW - 4.f, ph = panelH - 4.f;
        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph), cGlass, 6.f);
        dl->AddRect(ImVec2(px, py), ImVec2(px + pw, py + ph), cBorderHi, 6.f);
    }

    // ── Chart area — aligned to playback bar's scrubber track ───────────
    const float glassX = O.x + 2.f;
    const float glassW = vpW - 4.f;
    const float chartX0 = glassX + PAD + timeW + 6.f;
    const float chartX1 = glassX + glassW - PAD - badgeW - 6.f;
    const float chartY0 = O.y + topPad;
    const float chartY1 = chartY0 + chartH;
    const float chartW  = chartX1 - chartX0;
    const float scrubY0 = chartY1;
    const float scrubY1 = scrubY0 + scrubH;

    // ── Close button (top-right X) ──────────────────────────────────────
    {
        const float btnSz = 18.f;
        const float margin = 6.f;
        ImVec2 btnMin(O.x + vpW - margin - btnSz, O.y + margin);
        ImVec2 btnMax(btnMin.x + btnSz, btnMin.y + btnSz);
        ImVec2 center((btnMin.x + btnMax.x) * 0.5f, (btnMin.y + btnMax.y) * 0.5f);

        bool btnHov = ImGui::IsMouseHoveringRect(btnMin, btnMax);
        ImU32 btnBg  = btnHov ? IM_COL32(200, 60, 60, 200) : IM_COL32(255, 255, 255, 20);
        ImU32 xCol   = btnHov ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 168, 75, 180);

        dl->AddRectFilled(btnMin, btnMax, btnBg, 3.f);
        dl->AddRect(btnMin, btnMax, IM_COL32(200, 168, 75, 80), 3.f);
        float cr = 5.f;
        dl->AddLine(ImVec2(center.x - cr, center.y - cr),
                    ImVec2(center.x + cr, center.y + cr), xCol, 1.5f);
        dl->AddLine(ImVec2(center.x + cr, center.y - cr),
                    ImVec2(center.x - cr, center.y + cr), xCol, 1.5f);

        if (btnHov && ImGui::IsMouseClicked(0))
            m_showEventTimeline = false;
    }

    // ── Filter pills — vertical left column ─────────────────────────────
    {
        const float iconH   = 16.f;
        const float pillH   = 20.f;
        const float pillGap = 3.f;
        const float pillW   = chartX0 - O.x - 34.f;
        float lx = O.x + 6.f;
        float ly = O.y + 6.f;
        const float fsPill = fs * 1.0f;

        auto FilterPill = [&](const char* label, bool& active, ImTextureID miniTex, ImU32 fallbackCol) {
            ImVec2 p0(lx, ly);
            ImVec2 p1(lx + pillW, ly + pillH);

            bool hovered = ImGui::IsMouseHoveringRect(p0, p1);
            if (hovered && ImGui::IsMouseClicked(0))
                active = !active;

            ImU32 bg     = active ? IM_COL32(47, 36, 15, 220) : IM_COL32(20, 20, 20, 150);
            ImU32 border = active ? IM_COL32(255, 215, 100, 255) : IM_COL32(255, 255, 255, 30);
            ImU32 text   = active ? IM_COL32(255, 232, 176, 255) : IM_COL32(120, 120, 120, 255);

            if (hovered) bg = IM_COL32(60, 50, 20, 220);

            dl->AddRectFilled(p0, p1, bg, 4.f);
            dl->AddRect(p0, p1, border, 4.f);

            float iy = ly + (pillH - iconH) * 0.5f;
            if (miniTex)
                dl->AddImage(miniTex, ImVec2(lx + 5.f, iy), ImVec2(lx + 5.f + iconH, iy + iconH));
            else
                dl->AddCircleFilled(ImVec2(lx + 5.f + iconH * 0.5f, ly + pillH * 0.5f), 4.f, fallbackCol);

            dl->AddText(font, fsPill,
                ImVec2(lx + 5.f + iconH + 4.f, ly + (pillH - fsPill) * 0.5f), text, label);

            ly += pillH + pillGap;
        };

        ImTextureID deathTex  = LoadProfIcon(dev, 1);
        ImTextureID flagTex   = LoadFlagIcon(dev, "Blue_flag_waving.svg.png");
        ImTextureID moraleTex = LoadFlagIcon(dev, "bluemorale.png");
        ImTextureID lordTex   = LoadFlagIcon(dev, "blueguildlord.png");

        FilterPill("Death",   m_tlFilterDeath,  deathTex,  IM_COL32(208,  72,  72, 255));
        FilterPill("Flag",    m_tlFilterFlag,   flagTex,   IM_COL32(255, 200,  60, 255));
        FilterPill("Morale",  m_tlFilterMorale, moraleTex, IM_COL32(212, 160,  32, 255));
        FilterPill("Lord",    m_tlFilterLord,   lordTex,   IM_COL32(255,  90,  90, 255));

        if (IsIsleOfWurmsMap(m_replayCtx.mapId)) {
            ImTextureID shrineTex = LoadFlagIcon(dev, "Health_Shrine_Bonus.jpg");
            FilterPill("Shrine",  m_tlFilterShrine, shrineTex, IM_COL32(220, 200, 120, 255));
        }

        if (m_flagTimeline.obelisk.standAgentId >= 0) {
            ImTextureID obeliskTex = LoadFlagIcon(dev, "Obelisk_Lightning.jpg");
            FilterPill("Obelisk", m_tlFilterObelisk, obeliskTex, IM_COL32(180, 140, 255, 255));
        }

        if (m_replayCtx.mapId == kWarriorsIsleMapId) {
            ImTextureID leverTex = LoadNPCIcon(dev, "Lever.png");
            FilterPill("Catapult", m_tlFilterCatapult, leverTex, IM_COL32(240, 170, 40, 255));
        }
    }

    // ── Clip to chart area ──────────────────────────────────────────────
    dl->PushClipRect(ImVec2(chartX0, chartY0), ImVec2(chartX1, chartY1), true);

    // ── Horizontal gridlines only (no vertical lines) ─────────────────
    for (int pct = 25; pct <= 100; pct += 25)
    {
        float y = chartY1 - (pct / 100.f) * chartH;
        dl->AddLine(ImVec2(chartX0, y), ImVec2(chartX1, y), cGrid);
    }

    float interval = maxT < 300.f ? 30.f : maxT < 600.f ? 60.f : 120.f;

    // ── Y-axis labels ───────────────────────────────────────────────────
    dl->PopClipRect();
    for (int pct = 25; pct <= 100; pct += 25)
    {
        float y = chartY1 - (pct / 100.f) * chartH;
        char buf[8]; snprintf(buf, sizeof(buf), "%d%%", pct);
        ImVec2 ts = font->CalcTextSizeA(fs * 0.72f, FLT_MAX, 0.f, buf);
        dl->AddText(font, fs * 0.72f,
            ImVec2(chartX0 - ts.x - 4.f, y - ts.y * 0.5f), cTextDim, buf);
    }

    // ── X-axis labels (placed below the scrubber) ───────────────────────
    for (float t = 0.f; t <= maxT; t += interval)
    {
        float x = chartX0 + (t / maxT) * chartW;
        float dispT = t - m_displayTimeOffset;
        int absSec = static_cast<int>(std::abs(dispT));
        int mins = absSec / 60;
        int secs = absSec % 60;
        char buf[16];
        if (dispT < 0.f)
            snprintf(buf, sizeof(buf), "-%d:%02d", mins, secs);
        else
            snprintf(buf, sizeof(buf), "%d:%02d", mins, secs);
        ImVec2 ts = font->CalcTextSizeA(fs * 0.72f, FLT_MAX, 0.f, buf);
        dl->AddText(font, fs * 0.72f,
            ImVec2(x - ts.x * 0.5f, scrubY1 + 1.f), cTextDim, buf);
    }

    dl->PushClipRect(ImVec2(chartX0, chartY0), ImVec2(chartX1, chartY1), true);

    // ── Health curves (smooth linear interpolation with fill) ───────────
    auto drawCurve = [&](const std::vector<float>& data, ImU32 lineCol, ImU32 fillCol) {
        if (data.size() < 2) return;
        int n = static_cast<int>(data.size());

        // Filled area: build triangle strip using AddConvexPoly in segments
        for (int i = 0; i < n - 1; ++i)
        {
            float t0 = static_cast<float>(i);
            float t1 = static_cast<float>(i + 1);
            float x0 = chartX0 + (t0 / maxT) * chartW;
            float x1 = chartX0 + (t1 / maxT) * chartW;
            float y0 = chartY1 - (data[i] / 100.f) * chartH;
            float y1 = chartY1 - (data[i + 1] / 100.f) * chartH;

            ImVec2 quad[4] = {
                ImVec2(x0, y0), ImVec2(x1, y1),
                ImVec2(x1, chartY1), ImVec2(x0, chartY1)
            };
            dl->AddConvexPolyFilled(quad, 4, fillCol);
        }

        // Smooth line
        for (int i = 0; i < n - 1; ++i)
        {
            float t0 = static_cast<float>(i);
            float t1 = static_cast<float>(i + 1);
            float x0 = chartX0 + (t0 / maxT) * chartW;
            float x1 = chartX0 + (t1 / maxT) * chartW;
            float y0 = chartY1 - (data[i] / 100.f) * chartH;
            float y1 = chartY1 - (data[i + 1] / 100.f) * chartH;
            dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), lineCol, 1.f);
        }
    };

    drawCurve(m_timeline.redHealth, cRed, cRedFill);
    drawCurve(m_timeline.blueHealth,  cBlue,  cBlueFill);

    // ── Event markers ───────────────────────────────────────────────────
    auto isEventVisible = [&](const TimelineEvent& e) -> bool {
        switch (e.type) {
        case TimelineEventType::Death:        return m_tlFilterDeath;
        case TimelineEventType::Resurrection: return false;
        case TimelineEventType::FlagCapture:  return m_tlFilterFlag;
        case TimelineEventType::FlagReturn:   return m_tlFilterFlag;
        case TimelineEventType::MoraleBoost:  return m_tlFilterMorale;
        case TimelineEventType::LordAttacked:      return m_tlFilterLord;
        case TimelineEventType::Victory:            return false;
        case TimelineEventType::ShrineCaptured:     return m_tlFilterShrine;
        case TimelineEventType::ShrineNeutralized:  return m_tlFilterShrine;
        case TimelineEventType::ObeliskCapture:     return m_tlFilterObelisk;
        case TimelineEventType::Catapult:           return m_tlFilterCatapult;
        }
        return true;
    };

    // Bottom event row: flags + morale pinned here, 20px from chart bottom
    const float iconSz       = 22.f;
    const float bottomRowY   = chartY1 - 10.f; // center of bottom row
    const float stackStep    = iconSz + 2.f;

    struct MarkerPos { float x, y; int idx; };
    std::vector<MarkerPos> markers;

    for (int i = 0; i < static_cast<int>(m_timeline.events.size()); ++i)
    {
        const auto& e = m_timeline.events[i];
        if (!isEventVisible(e)) continue;

        float ex = chartX0 + (e.time / maxT) * chartW;
        float ey;

        if (e.type == TimelineEventType::FlagCapture || e.type == TimelineEventType::FlagReturn
            || e.type == TimelineEventType::ShrineCaptured || e.type == TimelineEventType::ShrineNeutralized
            || e.type == TimelineEventType::ObeliskCapture || e.type == TimelineEventType::Catapult)
        {
            ey = bottomRowY;
        }
        else if (e.type == TimelineEventType::MoraleBoost)
        {
            ey = bottomRowY;
            // Collision avoidance: if a flag capture within 3s, offset morale 20px up
            for (const auto& other : m_timeline.events)
            {
                if ((other.type == TimelineEventType::FlagCapture
                     || other.type == TimelineEventType::ShrineCaptured
                     || other.type == TimelineEventType::ShrineNeutralized
                     || other.type == TimelineEventType::ObeliskCapture
                     || other.type == TimelineEventType::Catapult) &&
                    isEventVisible(other) &&
                    std::abs(e.time - other.time) <= 3.f)
                {
                    ey = bottomRowY - 20.f;
                    break;
                }
            }
        }
        else
        {
            int sec = std::clamp(static_cast<int>(e.time), 0,
                static_cast<int>(m_timeline.redHealth.size()) - 1);
            const auto& curve = Team::IsRed(e.teamId) ? m_timeline.redHealth : m_timeline.blueHealth;
            float healthPct = (sec < static_cast<int>(curve.size())) ? curve[sec] : 50.f;
            ey = chartY1 - (healthPct / 100.f) * chartH;
            float minY = chartY0 + iconSz * 0.5f + 1.f;
            if (ey < minY) ey = minY;
        }

        markers.push_back({ ex, ey, i });
    }

    // Overlap stacking for curve-based markers (death, lord) — push downward
    auto isBottomRowEvent = [](TimelineEventType t) {
        return t == TimelineEventType::FlagCapture
            || t == TimelineEventType::FlagReturn
            || t == TimelineEventType::MoraleBoost
            || t == TimelineEventType::ShrineCaptured
            || t == TimelineEventType::ShrineNeutralized
            || t == TimelineEventType::ObeliskCapture
            || t == TimelineEventType::Catapult;
    };

    for (size_t mi = 0; mi < markers.size(); ++mi)
    {
        const auto& e = m_timeline.events[markers[mi].idx];
        if (isBottomRowEvent(e.type))
            continue;

        int stackCount = 0;
        for (size_t mj = 0; mj < mi; ++mj)
        {
            const auto& eo = m_timeline.events[markers[mj].idx];
            if (isBottomRowEvent(eo.type))
                continue;

            if (std::abs(markers[mi].x - markers[mj].x) < stackStep)
                stackCount++;
        }
        if (stackCount > 0)
        {
            markers[mi].y += stackCount * stackStep;
            if (markers[mi].y + iconSz * 0.5f > chartY1 - 22.f)
                markers[mi].y = chartY1 - 22.f - iconSz * 0.5f;
        }
    }

    // Draw markers with actual texture icons
    ImVec2 mousePos = ImGui::GetMousePos();
    int hoveredMarker = -1;

    for (auto& mp : markers)
    {
        const auto& e = m_timeline.events[mp.idx];
        float half = iconSz * 0.5f;
        ImVec2 iconMin(mp.x - half, mp.y - half);
        ImVec2 iconMax(mp.x + half, mp.y + half);

        bool mHovered = (mousePos.x >= iconMin.x - 2.f && mousePos.x <= iconMax.x + 2.f &&
                         mousePos.y >= iconMin.y - 2.f && mousePos.y <= iconMax.y + 2.f);
        if (mHovered) {
            hoveredMarker = mp.idx;
            iconMin = ImVec2(mp.x - half * 1.3f, mp.y - half * 1.3f);
            iconMax = ImVec2(mp.x + half * 1.3f, mp.y + half * 1.3f);
        }

        ImU32 teamBorderCol = Team::IsRed(e.teamId)
            ? IM_COL32(208, 72, 72, 255)   // #d04848
            : IM_COL32( 74,144,216, 255);   // #4a90d8

        switch (e.type) {
        case TimelineEventType::Death: {
            dl->AddRectFilled(iconMin, iconMax,
                              IM_COL32(10, 10, 10, 230), 2.f);
            dl->AddRect(iconMin, iconMax,
                        teamBorderCol, 2.f, 0, 2.f);
            ImTextureID profTex = LoadProfIcon(dev, e.professionId);
            if (profTex)
                dl->AddImage(profTex, iconMin, iconMax);
            else {
                dl->AddText(font, fs * 0.65f,
                    ImVec2(mp.x - 3.f, mp.y - fs * 0.35f),
                    IM_COL32(255, 255, 255, 255), "X");
            }
            // Subtle red cross overlay
            {
                const ImU32 crossCol = IM_COL32(220, 40, 40, 120);
                dl->AddLine(ImVec2(iconMin.x + 2.f, iconMin.y + 2.f),
                            ImVec2(iconMax.x - 2.f, iconMax.y - 2.f), crossCol, 1.5f);
                dl->AddLine(ImVec2(iconMax.x - 2.f, iconMin.y + 2.f),
                            ImVec2(iconMin.x + 2.f, iconMax.y - 2.f), crossCol, 1.5f);
            }
            break;
        }
        case TimelineEventType::FlagCapture: {
            const char* flagFile = Team::IsRed(e.teamId) ? "Red_flag_waving.svg.png" : "Blue_flag_waving.svg.png";
            ImTextureID tex = LoadFlagIcon(dev, flagFile);
            if (tex)
                dl->AddImage(tex, iconMin, iconMax);
            else {
                dl->AddText(font, fs * 0.65f,
                    ImVec2(mp.x - 3.f, mp.y - fs * 0.35f),
                    IM_COL32(255, 255, 255, 255), "F");
            }
            break;
        }
        case TimelineEventType::FlagReturn: {
            const char* flagFile = Team::IsRed(e.teamId) ? "Red_flag_waving.svg.png" : "Blue_flag_waving.svg.png";
            ImTextureID tex = LoadFlagIcon(dev, flagFile);
            if (tex)
                dl->AddImage(tex, iconMin, iconMax);
            else {
                dl->AddText(font, fs * 0.65f,
                    ImVec2(mp.x - 3.f, mp.y - fs * 0.35f),
                    IM_COL32(255, 255, 255, 255), "R");
            }
            // Green left-pointing arrow overlay
            float arrowX = iconMin.x - 2.f;
            float arrowCY = (iconMin.y + iconMax.y) * 0.5f;
            float arrowH = (iconMax.y - iconMin.y) * 0.35f;
            dl->AddTriangleFilled(
                ImVec2(arrowX - 6.f, arrowCY),
                ImVec2(arrowX, arrowCY - arrowH),
                ImVec2(arrowX, arrowCY + arrowH),
                IM_COL32(40, 200, 40, 220));
            dl->AddLine(ImVec2(arrowX, arrowCY), ImVec2(arrowX + 5.f, arrowCY),
                        IM_COL32(40, 200, 40, 220), 2.f);
            break;
        }
        case TimelineEventType::MoraleBoost: {
            const char* moraleFile = Team::IsRed(e.teamId) ? "redmorale.png" : "bluemorale.png";
            ImTextureID tex = LoadFlagIcon(dev, moraleFile);
            if (tex)
                dl->AddImage(tex, iconMin, iconMax);
            else {
                dl->AddText(font, fs * 0.65f,
                    ImVec2(mp.x - 3.f, mp.y - fs * 0.35f),
                    IM_COL32(255, 255, 255, 255), "M");
            }
            break;
        }
        case TimelineEventType::LordAttacked: {
            const char* lordFile = Team::IsRed(e.teamId) ? "redguildlord.png" : "blueguildlord.png";
            ImTextureID tex = LoadFlagIcon(dev, lordFile);
            if (tex)
                dl->AddImage(tex, iconMin, iconMax);
            else {
                dl->AddText(font, fs * 0.65f,
                    ImVec2(mp.x - 3.f, mp.y - fs * 0.35f),
                    IM_COL32(255, 255, 255, 255), "!");
            }
            break;
        }
        case TimelineEventType::ShrineCaptured: {
            ImTextureID tex = LoadFlagIcon(dev, "Health_Shrine_Bonus.jpg");
            ImU32 borderCol = Team::IsRed(e.teamId)
                ? IM_COL32(208, 72, 72, 255)
                : IM_COL32(74, 144, 216, 255);
            dl->AddRectFilled(iconMin, iconMax, IM_COL32(10, 10, 10, 200), 2.f);
            dl->AddRect(iconMin, iconMax, borderCol, 2.f, 0, 2.f);
            if (tex)
                dl->AddImage(tex, iconMin, iconMax);
            else
                dl->AddText(font, fs * 0.65f,
                    ImVec2(mp.x - 3.f, mp.y - fs * 0.35f),
                    IM_COL32(255, 255, 255, 255), "S");
            break;
        }
        case TimelineEventType::ShrineNeutralized: {
            ImTextureID tex = LoadFlagIcon(dev, "Health_Shrine_Bonus.jpg");
            dl->AddRectFilled(iconMin, iconMax, IM_COL32(10, 10, 10, 200), 2.f);
            dl->AddRect(iconMin, iconMax, IM_COL32(180, 180, 180, 200), 2.f, 0, 2.f);
            if (tex)
            {
                dl->AddImage(tex, iconMin, iconMax);
                dl->AddRectFilled(iconMin, iconMax, IM_COL32(60, 60, 60, 120), 2.f);
            }
            else
                dl->AddText(font, fs * 0.65f,
                    ImVec2(mp.x - 3.f, mp.y - fs * 0.35f),
                    IM_COL32(180, 180, 180, 255), "N");
            break;
        }
        case TimelineEventType::Catapult: {
            ImTextureID tex = LoadNPCIcon(dev, "Lever.png");
            ImU32 borderCol = CatapultStateBorderColor(e.catapultState);
            constexpr float bw = 2.f;
            dl->AddRectFilled(iconMin, iconMax, borderCol, 2.f);
            ImVec2 imgMin(iconMin.x + bw, iconMin.y + bw);
            ImVec2 imgMax(iconMax.x - bw, iconMax.y - bw);
            dl->AddRectFilled(imgMin, imgMax, IM_COL32(10, 10, 10, 220), 1.f);
            if (tex)
                dl->AddImage(tex, imgMin, imgMax);
            else
                dl->AddText(font, fs * 0.65f,
                    ImVec2(mp.x - 3.f, mp.y - fs * 0.35f),
                    IM_COL32(255, 255, 255, 255), "C");
            break;
        }
        case TimelineEventType::ObeliskCapture: {
            ImTextureID tex = LoadFlagIcon(dev, "Obelisk_Lightning.jpg");
            ImU32 borderCol = Team::IsRed(e.teamId)
                ? IM_COL32(255, 80, 70, 255)
                : IM_COL32(80, 160, 255, 255);
            constexpr float bw = 2.f;
            dl->AddRectFilled(iconMin, iconMax, borderCol, 2.f);
            ImVec2 imgMin(iconMin.x + bw, iconMin.y + bw);
            ImVec2 imgMax(iconMax.x - bw, iconMax.y - bw);
            if (tex)
                dl->AddImage(tex, imgMin, imgMax);
            else
                dl->AddText(font, fs * 0.65f,
                    ImVec2(mp.x - 3.f, mp.y - fs * 0.35f),
                    IM_COL32(255, 255, 255, 255), "O");
            break;
        }
        default: break;
        }
    }

    // ── Playhead (main chart) ───────────────────────────────────────────
    {
        float phX = chartX0 + (m_debugTimeline / maxT) * chartW;
        dl->AddLine(ImVec2(phX, chartY0), ImVec2(phX, chartY1), cPlayhead, 1.5f);
        dl->AddTriangleFilled(
            ImVec2(phX - 4.f, chartY0),
            ImVec2(phX + 4.f, chartY0),
            ImVec2(phX,       chartY0 + 6.f), cPlayhead);
    }

    // ── Hover crosshair + tooltip ───────────────────────────────────────
    bool chartHovered = (mousePos.x >= chartX0 && mousePos.x <= chartX1 &&
                         mousePos.y >= chartY0 && mousePos.y <= chartY1);
    if (chartHovered)
    {
        dl->AddLine(ImVec2(mousePos.x, chartY0), ImVec2(mousePos.x, chartY1),
            IM_COL32(255, 255, 255, 40), 1.f);

        float hoverT = ((mousePos.x - chartX0) / chartW) * maxT;
        int sec = std::clamp(static_cast<int>(hoverT), 0,
            static_cast<int>(m_timeline.redHealth.size()) - 1);
        float rh = m_timeline.redHealth[sec];
        float bh = m_timeline.blueHealth[sec];
        float dispHT = hoverT - m_displayTimeOffset;
        int absHT = static_cast<int>(std::abs(dispHT));
        int mins = absHT / 60;
        int secs = absHT % 60;

        dl->PopClipRect();

        char ttBuf[128];
        if (dispHT < 0.f)
            snprintf(ttBuf, sizeof(ttBuf), "-%d:%02d  Red: %.0f%%  Blue: %.0f%%", mins, secs, rh, bh);
        else
            snprintf(ttBuf, sizeof(ttBuf), "%d:%02d  Red: %.0f%%  Blue: %.0f%%", mins, secs, rh, bh);
        ImVec2 ttSz = font->CalcTextSizeA(fs * 0.78f, FLT_MAX, 0.f, ttBuf);
        float ttX = std::clamp(mousePos.x - ttSz.x * 0.5f, chartX0, chartX1 - ttSz.x - 8.f);
        float ttY = mousePos.y - ttSz.y - 10.f;
        if (ttY < chartY0) ttY = mousePos.y + 14.f;

        dl->AddRectFilled(
            ImVec2(ttX - 4.f, ttY - 2.f),
            ImVec2(ttX + ttSz.x + 4.f, ttY + ttSz.y + 2.f),
            IM_COL32(14, 16, 20, 230), 4.f);
        dl->AddRect(
            ImVec2(ttX - 4.f, ttY - 2.f),
            ImVec2(ttX + ttSz.x + 4.f, ttY + ttSz.y + 2.f),
            IM_COL32(200, 168, 75, 80), 4.f);
        dl->AddText(font, fs * 0.78f, ImVec2(ttX, ttY), cText, ttBuf);

        dl->PushClipRect(ImVec2(chartX0, chartY0), ImVec2(chartX1, chartY1), true);
    }

    // ── Marker tooltip ──────────────────────────────────────────────────
    if (hoveredMarker >= 0)
    {
        const auto& e = m_timeline.events[hoveredMarker];
        dl->PopClipRect();

        const char* typeName = "";
        switch (e.type) {
        case TimelineEventType::Death:        typeName = "Death"; break;
        case TimelineEventType::FlagCapture:  typeName = "Flag Capture"; break;
        case TimelineEventType::FlagReturn:   typeName = "Flag Return"; break;
        case TimelineEventType::MoraleBoost:       typeName = "Morale Boost"; break;
        case TimelineEventType::LordAttacked:      typeName = "Lord Attacked"; break;
        case TimelineEventType::ShrineCaptured:    typeName = "Shrine Captured"; break;
        case TimelineEventType::ShrineNeutralized: typeName = "Shrine Neutralized"; break;
        case TimelineEventType::ObeliskCapture:    typeName = "Obelisk Capture"; break;
        case TimelineEventType::Catapult:          typeName = "Catapult"; break;
        default: break;
        }

        float dispET = e.time - m_displayTimeOffset;
        int absET = static_cast<int>(std::abs(dispET));
        int mins = absET / 60;
        int secs = absET % 60;
        const char* sign = (dispET < 0.f) ? "-" : "";
        char ttBuf[256];
        if (!e.label.empty())
            snprintf(ttBuf, sizeof(ttBuf), "[%s%d:%02d] %s - %s", sign, mins, secs, typeName, e.label.c_str());
        else
            snprintf(ttBuf, sizeof(ttBuf), "[%s%d:%02d] %s", sign, mins, secs, typeName);

        ImVec2 ttSz = font->CalcTextSizeA(fs * 0.78f, FLT_MAX, 0.f, ttBuf);
        float ttX = std::clamp(mousePos.x - ttSz.x * 0.5f, chartX0, chartX1 - ttSz.x - 8.f);
        float ttY = mousePos.y - ttSz.y - 22.f;
        if (ttY < chartY0) ttY = mousePos.y + 14.f;

        dl->AddRectFilled(
            ImVec2(ttX - 4.f, ttY - 2.f),
            ImVec2(ttX + ttSz.x + 4.f, ttY + ttSz.y + 2.f),
            IM_COL32(14, 16, 20, 240), 4.f);
        dl->AddRect(
            ImVec2(ttX - 4.f, ttY - 2.f),
            ImVec2(ttX + ttSz.x + 4.f, ttY + ttSz.y + 2.f),
            IM_COL32(200, 168, 75, 100), 4.f);
        dl->AddText(font, fs * 0.78f, ImVec2(ttX, ttY), cText, ttBuf);

        if (ImGui::IsMouseClicked(0))
            m_debugTimeline = std::clamp(e.time, 0.f, maxT);

        dl->PushClipRect(ImVec2(chartX0, chartY0), ImVec2(chartX1, chartY1), true);
    }

    dl->PopClipRect();

    // ── Scrubber strip ──────────────────────────────────────────────────
    dl->AddLine(ImVec2(chartX0, scrubY0), ImVec2(chartX1, scrubY0),
        IM_COL32(200, 168, 75, 40), 1.f);
    dl->PushClipRect(ImVec2(chartX0, scrubY0 + 1.f), ImVec2(chartX1, scrubY1), true);
    dl->AddRectFilled(ImVec2(chartX0, scrubY0 + 1.f), ImVec2(chartX1, scrubY1),
        IM_COL32(255, 255, 255, 6));

    // Mini health curves in scrubber (smooth)
    {
        int n = static_cast<int>(m_timeline.redHealth.size());
        for (int i = 0; i < n - 1; ++i)
        {
            float t0 = static_cast<float>(i);
            float t1 = static_cast<float>(i + 1);
            float x0 = chartX0 + (t0 / maxT) * chartW;
            float x1 = chartX0 + (t1 / maxT) * chartW;

            float ry0 = scrubY1 - (m_timeline.redHealth[i] / 100.f) * scrubH;
            float ry1 = scrubY1 - (m_timeline.redHealth[i + 1] / 100.f) * scrubH;
            dl->AddLine(ImVec2(x0, ry0), ImVec2(x1, ry1), IM_COL32(255, 107, 107, 100), 1.f);

            float by0 = scrubY1 - (m_timeline.blueHealth[i] / 100.f) * scrubH;
            float by1 = scrubY1 - (m_timeline.blueHealth[i + 1] / 100.f) * scrubH;
            dl->AddLine(ImVec2(x0, by0), ImVec2(x1, by1), IM_COL32(74, 200, 255, 100), 1.f);
        }
    }

    // Scrubber playhead
    {
        float phX = chartX0 + (m_debugTimeline / maxT) * chartW;
        dl->AddLine(ImVec2(phX, scrubY0), ImVec2(phX, scrubY1), cPlayhead, 2.f);
    }

    dl->PopClipRect();

    // Unified click-drag scrubbing for both chart and scrubber areas
    bool anyTimelineHovered = (mousePos.x >= chartX0 && mousePos.x <= chartX1 &&
                               mousePos.y >= chartY0 && mousePos.y <= scrubY1);
    static bool timelineDragging = false;
    if (anyTimelineHovered && ImGui::IsMouseClicked(0) && hoveredMarker < 0)
        timelineDragging = true;
    if (!ImGui::IsMouseDown(0))
        timelineDragging = false;
    if (timelineDragging)
    {
        float t = ((mousePos.x - chartX0) / chartW) * maxT;
        m_debugTimeline = std::clamp(t, 0.f, maxT);
    }

    // ── Bottom border shimmer ───────────────────────────────────────────
    {
        float sx = O.x + 12.f, sw = vpW - 24.f;
        float sy = O.y + panelH - 1.f;
        float mx = sx + sw * 0.5f;
        dl->AddRectFilledMultiColor(
            ImVec2(sx, sy), ImVec2(mx, sy + 1.f),
            IM_COL32(200,168,75, 0), IM_COL32(200,168,75,100),
            IM_COL32(200,168,75,100), IM_COL32(200,168,75, 0));
        dl->AddRectFilledMultiColor(
            ImVec2(mx, sy), ImVec2(sx + sw, sy + 1.f),
            IM_COL32(200,168,75,100), IM_COL32(200,168,75, 0),
            IM_COL32(200,168,75, 0), IM_COL32(200,168,75,100));
    }

    ImGui::End();
}
