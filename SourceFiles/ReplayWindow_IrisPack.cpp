#include "pch.h"
#include "ReplayWindow.h"
#include "IrisPack.h"
#include "build_config.h"

#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

// The Iris pack: what the in-game HUD (Iris, a GWToolbox plugin) shows over a match the client is
// observing - the values only this analysis can produce. Everything the client already receives
// (positions, health fraction, skill activations) stays out of it.
//
// Time is the recording's own clock, seconds of instance time, which the client's
// GW::Map::GetInstanceTime() matches to the sample (measured 2026-10-22). Agents are keyed by agent id,
// which the observed instance and the recording share. Version 1 layout:
//
//   v, folder, end            pack version, recording folder, last recorded second
//   players[]                 {id, party, number, name, primary, secondary}
//   attributes                the solved builds, exactly what --export-attributes writes
//   max_hp{id: [[t, hp, est]]}            change points, sampled every second
//   energy{id: {e[], lo[], hi[], max[]}}  one value per second from t = 0; lo/hi = the model's band
//   effects{id: [[skill, start, end, caster, exact, kind]]}
//   morale{id: [[t, morale]]}             step function
//   lords[]                   {team, id, max_hp, hp: [[t, frac]], hits: [[t, caster, dmg]]}
//   event_types[], events[]   [[t, type, agent, team, label]]
//
// Version 2 adds (WriteIrisPackV2):
//   offset                    instance seconds before GW's "Time elapsed" 0:00
//   deaths[]                  [t, victim, killer, npc] - every death, players and NPCs; the recording names
//                             no killer, so `killer` is whoever last damaged the victim in the 5 s before
//                             (0 when nobody did); npc = 1 for a non-player
//   npcs{id: [model, team, "name"]}   the NPCs that appear in deaths[]
//   skillbars{id: [8 skill ids]}      in the Observer's display order
//   recharges{id: {skill: [[from, to, state]]}}  spans when a skill was not ready, sampled every 0.5 s;
//                             state 1 recharging, 2 disabled, 3 spent
//   weapons{id: [[t, main, offhand, "main name", "offhand name"]]}  change points of the set in hand
//   output{id: {dmg: [], heal: []}}   cumulative damage dealt / healing done, one value per 2 s from t = 0
//   bundles[]                 [t, item, type, location, carrier] - vine seeds and repair kits; type 2
//                             repair kit, 3 vine seed; location 0 base, 1 carried, 2 ground, 3 used
namespace
{
    std::string Esc(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 2);
        for (const char c : s)
        {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", c);
            else out += c;
        }
        return out;
    }

    std::string Num(float v) { return std::format("{:.2f}", v); }
}

bool ReplayWindow::ExportIrisPack(const std::filesystem::path& outPath) const
{
    const float end = m_replayCtx.maxReplayTime;
    if (end <= 0.f) return false;

    std::vector<int> playerIds;
    std::ostringstream js;
    js << "{\"v\":2,\"folder\":\"" << Esc(m_matchMeta.folder_name) << "\",\"end\":" << Num(end);

    js << ",\"players\":[";
    for (const auto& [partyId, party] : m_matchMeta.parties)
        for (const PlayerMeta& pm : party.players)
        {
            if (pm.id <= 0) continue;
            js << (playerIds.empty() ? "" : ",") << "{\"id\":" << pm.id << ",\"party\":\"" << Esc(partyId)
               << "\",\"number\":" << pm.player_number << ",\"name\":\"" << Esc(pm.encoded_name)
               << "\",\"primary\":" << pm.primary << ",\"secondary\":" << pm.secondary << "}";
            playerIds.push_back(pm.id);
        }
    js << "]";
    if (playerIds.empty()) return false;

    // The attributes object is the --export-attributes file verbatim, so Iris and the website agree.
    {
        const auto tmp = std::filesystem::path(outPath).replace_extension(".attributes.tmp");
        std::string attributes = "null";
        if (ExportAttributes(tmp))
        {
            std::ifstream in(tmp, std::ios::binary);
            attributes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        js << ",\"attributes\":" << attributes;
    }

    const int seconds = static_cast<int>(std::ceil(end));

    js << ",\"max_hp\":{";
    bool first = true;
    for (const int id : playerIds)
    {
        const auto it = m_replayCtx.agents.find(id);
        if (it == m_replayCtx.agents.end()) continue;
        js << (first ? "" : ",") << "\"" << id << "\":[";
        first = false;
        uint32_t last = UINT32_MAX;
        bool lastEst = false, any = false;
        for (int s = 0; s <= seconds; s++)
        {
            const MaxHpSample m = ResolveMaxHp(it->second, static_cast<float>(s));
            if (m.value == last && m.estimated == lastEst) continue;
            js << (any ? "," : "") << "[" << s << "," << m.value << "," << (m.estimated ? 1 : 0) << "]";
            last = m.value;
            lastEst = m.estimated;
            any = true;
        }
        js << "]";
    }
    js << "}";

    js << ",\"energy\":{";
    first = true;
    for (const int id : playerIds)
    {
        const EnergyModel::Track* track = EnergyTrackFor(id);
        if (!track || track->samples.empty()) continue;
        std::ostringstream e, lo, hi, mx;
        int lastMax = INT_MIN;
        bool anyMax = false;
        for (int s = 0; s <= seconds; s++)
        {
            const EnergyModel::Sample x = track->At(static_cast<float>(s));
            const char* sep = s ? "," : "";
            e << sep << std::lround(std::max(0.f, x.energy));
            lo << sep << std::lround(std::max(0.f, x.energy - x.down));
            hi << sep << std::lround(std::max(0.f, x.energy + x.up));
            const int m = static_cast<int>(std::lround(x.maxEnergy));
            if (m != lastMax)
            {
                mx << (anyMax ? "," : "") << "[" << s << "," << m << "]";
                lastMax = m;
                anyMax = true;
            }
        }
        js << (first ? "" : ",") << "\"" << id << "\":{\"e\":[" << e.str() << "],\"lo\":[" << lo.str()
           << "],\"hi\":[" << hi.str() << "],\"max\":[" << mx.str() << "]}";
        first = false;
    }
    js << "}";

    js << ",\"effects\":{";
    first = true;
    if (const EffectTimeline::Table* table = EffectTimelineTable())
        for (const int id : playerIds)
        {
            const auto it = table->byAgent.find(id);
            if (it == table->byAgent.end() || it->second.empty()) continue;
            js << (first ? "" : ",") << "\"" << id << "\":[";
            first = false;
            bool any = false;
            for (const EffectTimeline::Effect& fx : it->second)
            {
                js << (any ? "," : "") << "[" << fx.skillId << "," << Num(fx.start) << "," << Num(fx.end) << ","
                   << fx.caster << "," << (fx.endExact ? 1 : 0) << "," << static_cast<int>(fx.kind) << "]";
                any = true;
            }
            js << "]";
        }
    js << "}";

    js << ",\"morale\":{";
    first = true;
    for (const int id : playerIds)
    {
        const auto it = m_moraleTimeline.find(id);
        if (it == m_moraleTimeline.end() || it->second.empty()) continue;
        js << (first ? "" : ",") << "\"" << id << "\":[";
        first = false;
        for (size_t i = 0; i < it->second.size(); i++)
            js << (i ? "," : "") << "[" << Num(it->second[i].first) << "," << it->second[i].second << "]";
        js << "]";
    }
    js << "}";

    js << ",\"lords\":[";
    first = true;
    for (int team = 0; team < 2; team++)
    {
        const LordDamageData& lord = m_lordDmg[team];
        if (lord.lordAgentId < 0) continue;
        js << (first ? "" : ",") << "{\"team\":" << (team + 1) << ",\"id\":" << lord.lordAgentId
           << ",\"max_hp\":" << lord.lordMaxHp << ",\"hp\":[";
        first = false;
        if (const auto it = m_replayCtx.agents.find(lord.lordAgentId); it != m_replayCtx.agents.end())
        {
            float last = -1.f;
            bool any = false;
            for (const AgentSnapshot& snap : it->second.snapshots)
            {
                if (std::fabs(snap.health_pct - last) < 0.001f) continue;
                js << (any ? "," : "") << "[" << Num(snap.time) << "," << std::format("{:.3f}", snap.health_pct) << "]";
                last = snap.health_pct;
                any = true;
            }
        }
        js << "],\"hits\":[";
        for (size_t i = 0; i < lord.hits.size(); i++)
            js << (i ? "," : "") << "[" << Num(lord.hits[i].time) << "," << lord.hits[i].casterId << "," << lord.hits[i].rawDmg << "]";
        js << "]}";
    }
    js << "]";

    // Indices into event_types; the order is TimelineEventType's.
    js << ",\"event_types\":[\"death\",\"resurrection\",\"flag_capture\",\"flag_return\",\"morale_boost\","
          "\"lord_attacked\",\"victory\",\"shrine_captured\",\"shrine_neutralized\",\"obelisk_capture\",\"catapult\"]";
    js << ",\"events\":[";
    for (size_t i = 0; i < m_timeline.events.size(); i++)
    {
        const TimelineEvent& ev = m_timeline.events[i];
        js << (i ? "," : "") << "[" << Num(ev.time) << "," << static_cast<int>(ev.type) << "," << ev.agentId << ","
           << ev.teamId << ",\"" << Esc(ev.label) << "\"]";
    }
    js << "]";

    WriteIrisPackV2(js, playerIds);
    js << "}";

    std::vector<uint8_t> sealed;
    if (!IrisPack::Seal(js.str(), sealed)) return false;

    // A readable copy beside the pack, for checking the export by eye. Developer builds only: the
    // published pack is the sealed one.
    if (GWO_DEVELOPER)
    {
        std::ofstream plain(std::filesystem::path(outPath).replace_extension(".json"), std::ios::binary);
        plain << js.str();
    }

    std::ofstream out(outPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(sealed.data()), static_cast<std::streamsize>(sealed.size()));
    return static_cast<bool>(out);
}

void ReplayWindow::WriteIrisPackV2(std::ostringstream& js, const std::vector<int>& playerIds) const
{
    const float end = m_replayCtx.maxReplayTime;
    js << ",\"offset\":" << Num(m_displayTimeOffset);

    // Deaths: every alive -> dead turn in an agent's snapshots, players and NPCs. The recording names no
    // killer; the last damage on the victim in the 5 s before the death is the estimate.
    const auto killerOf = [this](const int victim, const float t) {
        int killer = 0;
        for (const CombatLogRow& row : m_combatLog)
        {
            if (row.time > t + 0.25f) break;
            if (row.category == CombatLogCategory::Damage && row.targetId == victim && row.casterId != victim && row.time >= t - 5.f)
                killer = row.casterId;
        }
        return killer;
    };
    std::vector<int> npcs;
    js << ",\"deaths\":[";
    bool first = true;
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        const bool player = ard.type == AgentType::Player;
        if (!player && !(ard.type == AgentType::NPC && LookupNpcName(ard.modelId))) continue;
        bool wasDead = false;
        for (const AgentSnapshot& snap : ard.snapshots)
        {
            if (snap.is_dead && !wasDead)
            {
                js << (first ? "" : ",") << "[" << Num(snap.time) << "," << id << "," << killerOf(id, snap.time) << "," << (player ? 0 : 1) << "]";
                first = false;
                if (!player && std::find(npcs.begin(), npcs.end(), id) == npcs.end()) npcs.push_back(id);
            }
            wasDead = snap.is_dead;
        }
    }
    js << "],\"npcs\":{";
    for (size_t i = 0; i < npcs.size(); i++)
    {
        const AgentReplayData& ard = m_replayCtx.agents.at(npcs[i]);
        js << (i ? "," : "") << "\"" << npcs[i] << "\":[" << ard.modelId << "," << static_cast<int>(ard.teamId) << ",\"" << Esc(LookupNpcName(ard.modelId)) << "\"]";
    }
    js << "}";

    // Skill bars and the spans each skill was not ready, from the cooldown model sampled every 0.5 s.
    js << ",\"skillbars\":{";
    first = true;
    for (const int id : playerIds)
    {
        const std::vector<int> bar = SkillBarDisplayOrder(id);
        js << (first ? "" : ",") << "\"" << id << "\":[";
        for (size_t i = 0; i < bar.size(); i++) js << (i ? "," : "") << bar[i];
        js << "]";
        first = false;
    }
    js << "},\"recharges\":{";
    first = true;
    for (const int id : playerIds)
    {
        const auto it = m_replayCtx.agents.find(id);
        if (it == m_replayCtx.agents.end()) continue;
        const std::vector<int> bar = SkillBarDisplayOrder(id);
        std::vector<std::vector<std::array<float, 3>>> spans(bar.size());
        std::vector<int> open(bar.size(), 0); // state of the running span, 0 = none
        for (float t = 0.f; t <= end; t += 0.5f)
        {
            const std::vector<SkillCooldownState> states = ComputeSkillCooldowns(it->second, bar, t);
            for (size_t s = 0; s < bar.size() && s < states.size(); s++)
            {
                const int state = static_cast<int>(states[s].state); // 0 ready, 1 recharging, 2 disabled, 3 spent
                if (state == open[s]) continue;
                if (open[s]) spans[s].back()[1] = t;
                if (state) spans[s].push_back({ t, end, static_cast<float>(state) });
                open[s] = state;
            }
        }
        js << (first ? "" : ",") << "\"" << id << "\":{";
        first = false;
        bool firstSkill = true;
        for (size_t s = 0; s < bar.size(); s++)
        {
            if (spans[s].empty()) continue;
            js << (firstSkill ? "" : ",") << "\"" << bar[s] << "\":[";
            firstSkill = false;
            for (size_t k = 0; k < spans[s].size(); k++)
                js << (k ? "," : "") << "[" << Num(spans[s][k][0]) << "," << Num(spans[s][k][1]) << "," << static_cast<int>(spans[s][k][2]) << "]";
            js << "]";
        }
        js << "}";
    }
    js << "}";

    // Weapon sets: the change points of the set in hand, with the items' names when the recording has them.
    const auto itemName = [this](const uint16_t item, const float t) -> std::string {
        if (!item) return {};
        const auto* def = m_replayCtx.stocData.equipment.FindByAgentItemId(item, t);
        return def ? def->name : std::string();
    };
    js << ",\"weapons\":{";
    first = true;
    for (const int id : playerIds)
    {
        const auto it = m_replayCtx.agents.find(id);
        if (it == m_replayCtx.agents.end()) continue;
        js << (first ? "" : ",") << "\"" << id << "\":[";
        first = false;
        uint64_t last = UINT64_MAX;
        bool any = false;
        for (const AgentSnapshot& snap : it->second.snapshots)
        {
            const uint64_t key = AgentReplayData::WeaponSetKey(snap);
            if (key == last) continue;
            last = key;
            js << (any ? "," : "") << "[" << Num(snap.time) << "," << snap.weapon_item_id << "," << snap.offhand_item_id << ",\""
               << Esc(itemName(snap.weapon_item_id, snap.time)) << "\",\"" << Esc(itemName(snap.offhand_item_id, snap.time)) << "\"]";
            any = true;
        }
        js << "]";
    }
    js << "}";

    // Damage dealt and healing done, cumulative, one value per 2 s.
    const int steps = static_cast<int>(std::ceil(end / 2.f)) + 1;
    std::unordered_map<int, std::pair<std::vector<int>, std::vector<int>>> output;
    for (const int id : playerIds) output[id] = { std::vector<int>(steps, 0), std::vector<int>(steps, 0) };
    for (const CombatLogRow& row : m_combatLog)
    {
        const bool dmg = row.category == CombatLogCategory::Damage, heal = row.category == CombatLogCategory::Heal;
        const auto o = output.find(row.casterId);
        if ((!dmg && !heal) || o == output.end() || row.valueAbs <= 0 || (dmg && row.targetId == row.casterId)) continue;
        const int step = std::clamp(static_cast<int>(std::ceil(row.time / 2.f)), 0, steps - 1);
        (dmg ? o->second.first : o->second.second)[step] += row.valueAbs;
    }
    js << ",\"output\":{";
    first = true;
    for (auto& [id, series] : output)
    {
        for (auto* v : { &series.first, &series.second })
            for (int i = 1; i < steps; i++) (*v)[i] += (*v)[i - 1];
        js << (first ? "" : ",") << "\"" << id << "\":{\"dmg\":[";
        first = false;
        for (int i = 0; i < steps; i++) js << (i ? "," : "") << series.first[i];
        js << "],\"heal\":[";
        for (int i = 0; i < steps; i++) js << (i ? "," : "") << series.second[i];
        js << "]}";
    }
    js << "}";

    // Vine seeds and repair kits.
    js << ",\"bundles\":[";
    first = true;
    for (const BundleTimeline& b : m_flagTimeline.bundles)
        for (const BundleTimelineEvent& e : b.events)
        {
            js << (first ? "" : ",") << "[" << Num(e.time) << "," << b.itemId << "," << static_cast<int>(b.type) << ","
               << static_cast<int>(e.newLocation) << "," << e.carrierAgentId << "]";
            first = false;
        }
    js << "]";
}
