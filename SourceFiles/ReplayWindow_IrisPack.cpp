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
//   energy{id: {pts: [[t, energy, band low, band high, max after overcast, overcast, pips]]}}  points the bar
//                             runs on from at pips / 3 per second, as the client's bar does (older packs: no pips,
//                             whole-number steps; before 2026-10-23: e/lo/hi per second + max change points)
//   effects{id: [[skill, start, end, caster, exact, kind, removed_by, removed_by_caster]]}  removed_by: the skill that
//                             took it off at `end` (0 unknown or a natural end)
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
//   weapon_sets{id: [{main, off, cat, main_type, off_type, bundle, bundle_name, main_icon, off_icon, main_tip, off_tip}]}
//                             the Observer's set list; *_icon = the item's Gw.dat icon file (its skin);
//                             *_tip = [[kind, text, condition, "RRGGBB"]], kind 0 name 1 stat 2 mod 3 muted
//   pips{id: [[t, health arrows, energy arrows]]}  change points, signed, as the Observer counts them
//   casts{id: [[start, end, target, skill, instant]]}  targeted casts of players and NPCs, for the skill lasers
//   bundles[]                 [t, item, type, location, carrier] - vine seeds and repair kits; type 2
//                             repair kit, 3 vine seed; location 0 base, 1 carried, 2 ground, 3 used
//   flag_moves[]              [t, kind, agent, flag team, bundle] - kind 0 picked up, 1 dropped; flag team 1 blue,
//                             2 red (0 for a bundle); bundle 0 a flag, 1 repair kit, 2 vine seed
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
        // As the client carries a bar: a value and the regeneration it runs at, the bar filling at pips / 3 per
        // second from there. [t, energy, band low, band high, max after overcast, overcast, pips], written whenever
        // the model leaves the line the last point draws (a spend, a gain, a pip change) or max/overcast change.
        // Iris advances from the last point at that rate - the model's own rate, nothing interpolated
        // (owner, 2026-10-22: the bar moves smoothly in game, the number is the floor).
        std::ostringstream pts;
        float lt = 0.f, le = 0.f, llo = 0.f, lhi = 0.f;
        int lmax = INT_MIN, loc = INT_MIN, lpips = INT_MIN;
        const auto ahead = [&](const float from, const float dt, const float cap) {
            const float v = std::min(cap, from + static_cast<float>(lpips) / 3.f * dt);
            return from >= 0.f ? std::max(0.f, v) : v;
        };
        bool any = false;
        for (size_t i = 0; i < track->samples.size(); i++)
        {
            const EnergyModel::Sample& x = track->samples[i];
            const float t = static_cast<float>(i) * track->dt;
            const int mx = static_cast<int>(std::lround(x.maxEnergy)), oc = static_cast<int>(std::lround(x.overcast));
            const float cap = static_cast<float>(mx);
            const float e = std::min(cap, x.energy), lo = std::clamp(x.energy - x.down, 0.f, cap), hi = std::clamp(x.energy + x.up, 0.f, cap);
            const int pips = x.dead ? 0 : x.pips;
            if (any && mx == lmax && oc == loc && pips == lpips)
            {
                const float dt = t - lt;
                if (std::abs(ahead(le, dt, cap) - e) < 0.05f && std::abs(ahead(llo, dt, cap) - lo) < 0.05f &&
                    std::abs(ahead(lhi, dt, cap) - hi) < 0.05f)
                    continue;
            }
            pts << (any ? "," : "") << "[" << Num(t) << "," << Num(e) << "," << Num(lo) << "," << Num(hi) << "," << mx << "," << oc << "," << pips << "]";
            lt = t, le = e, llo = lo, lhi = hi, lmax = mx, loc = oc, lpips = pips;
            any = true;
        }
        js << (first ? "" : ",") << "\"" << id << "\":{\"pts\":[" << pts.str() << "]}";
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
                   << fx.caster << "," << (fx.endExact ? 1 : 0) << "," << static_cast<int>(fx.kind) << "," << fx.removedBy << ","
                   << fx.removedByCaster << "]";
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
        // GW's tower announcement names no player; the flag timeline does: the carrier who planted the flag (Stick),
        // or the player who touched a dropped flag home (Return). The nearest such event within 5 s.
        int agent = ev.agentId;
        if (agent <= 0 && m_flagTimelineBuilt && m_flagTimeline.valid &&
            (ev.type == TimelineEventType::FlagCapture || ev.type == TimelineEventType::FlagReturn ||
             ev.type == TimelineEventType::ObeliskCapture))
        {
            const auto wanted = ev.type == TimelineEventType::FlagReturn ? FlagTimelineEventType::Return : FlagTimelineEventType::Stick;
            float best = 5.f;
            for (const FlagTimelineEvent& f : m_flagTimeline.allEvents)
            {
                const int who = f.carrierAgentId >= 0 ? f.carrierAgentId : f.actorAgentId;
                if (f.eventType != wanted || who < 0 || std::abs(f.time - ev.time) >= best) continue;
                best = std::abs(f.time - ev.time);
                agent = who;
            }
        }
        js << (i ? "," : "") << "[" << Num(ev.time) << "," << static_cast<int>(ev.type) << "," << agent << ","
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

    // The Observer's own weapon-set list (BuildWeaponSets: momentary swaps dropped, sorted by slot) with each
    // item's categories and its finished tooltip - name in its rarity colour, damage/armour line, every mod
    // with its condition - exactly as DrawWeaponSetTooltip shows it.
    const auto tooltipOf = [this](const uint16_t itemId) {
        std::ostringstream t;
        t << "[";
        const auto& equipment = m_replayCtx.stocData.equipment;
        const Equipment::ItemDef* item = (equipment.loaded && itemId) ? equipment.FindByAgentItemId(itemId) : nullptr;
        if (item)
        {
            const auto tip = Equipment::BuildTooltip(*item);
            const auto rgb = [](const Equipment::Rgb c) { return std::format("\"{:02X}{:02X}{:02X}\"", c.r, c.g, c.b); };
            bool firstLine = true;
            for (const auto& line : tip.lines)
            {
                using Kind = Equipment::TooltipLine::Kind;
                const std::string color = line.kind == Kind::Name ? rgb(Equipment::RarityColor(tip.rarity))
                                        : line.kind == Kind::Stat ? rgb(Equipment::kStatColor)
                                        : line.kind == Kind::Mod  ? rgb(Equipment::ModTextColor(tip.rarity))
                                                                  : std::string("\"7C7E7E\"");
                t << (firstLine ? "" : ",") << "[" << static_cast<int>(line.kind) << ",\"" << Esc(line.text) << "\",\""
                  << Esc(line.condition) << "\"," << color << "]";
                firstLine = false;
            }
        }
        t << "]";
        return t.str();
    };
    // The item's Gw.dat icon file: the real skin, which the in-game client can decode (toolbox LoadItemImage).
    const auto iconOf = [this](const uint16_t itemId) -> uint32_t {
        const auto& equipment = m_replayCtx.stocData.equipment;
        const Equipment::ItemDef* item = (equipment.loaded && itemId) ? equipment.FindByAgentItemId(itemId) : nullptr;
        return item ? (item->iconFileId ? item->iconFileId : item->modelFileId) : 0u;
    };
    js << ",\"weapon_sets\":{";
    first = true;
    for (const int id : playerIds)
    {
        PlayerWeaponSets sets;
        BuildWeaponSets(id, sets);
        js << (first ? "" : ",") << "\"" << id << "\":[";
        first = false;
        for (size_t i = 0; i < sets.sets.size(); i++)
        {
            const WeaponSetEntry& w = sets.sets[i];
            js << (i ? "," : "") << "{\"main\":" << w.mainId << ",\"off\":" << w.offId << ",\"cat\":" << w.weapCat << ",\"main_type\":"
               << static_cast<int>(w.mainType) << ",\"off_type\":" << static_cast<int>(w.offType) << ",\"bundle\":" << static_cast<int>(w.bundleType)
               << ",\"bundle_name\":\"" << Esc(w.bundleType != BundleType::Unknown ? BundleTypeName(w.bundleType) : "") << "\""
               << ",\"main_icon\":" << iconOf(w.mainId) << ",\"off_icon\":" << iconOf(w.offId)
               << ",\"main_tip\":" << tooltipOf(w.mainId) << ",\"off_tip\":" << tooltipOf(w.offId) << "}";
        }
        js << "]";
    }
    js << "}";

    // Regeneration arrows: health from the recording (the observing client does not get them), energy from
    // the model; change points [t, health arrows, energy arrows], arrows as the Observer counts them.
    js << ",\"pips\":{";
    first = true;
    for (const int id : playerIds)
    {
        const auto it = m_replayCtx.agents.find(id);
        if (it == m_replayCtx.agents.end()) continue;
        const EnergyModel::Track* track = EnergyTrackFor(id);
        js << (first ? "" : ",") << "\"" << id << "\":[";
        first = false;
        int lastHp = 1000, lastEn = 1000;
        bool any = false;
        for (float t = 0.f; t <= end; t += 0.5f)
        {
            const int snap = it->second.snapshotIndexAtTime(t);
            int hpPips = 0;
            if (snap >= 0)
            {
                const AgentSnapshot& s = it->second.snapshots[snap];
                const uint32_t maxHp = ResolveMaxHp(it->second, t).value;
                if (!s.is_dead && maxHp)
                    hpPips = std::clamp(static_cast<int>(std::lround(s.hp_pips * static_cast<float>(maxHp) / 2.f)), -10, 10);
            }
            const int enPips = track ? static_cast<int>(track->At(t).pips) : 0;
            if (hpPips == lastHp && enPips == lastEn) continue;
            js << (any ? "," : "") << "[" << Num(t) << "," << hpPips << "," << enPips << "]";
            lastHp = hpPips;
            lastEn = enPips;
            any = true;
        }
        js << "]";
    }
    js << "}";

    // Casts with a target, for the skill lasers (the Observer's skillLaserAtTime reads the same history):
    // players and NPCs, [start, end, target, skill, instant].
    js << ",\"casts\":{";
    first = true;
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Player && ard.type != AgentType::NPC) continue;
        bool any = false;
        for (const SkillUseEvent& ev : ard.skillUseHistory)
        {
            if (ev.targetId <= 0 || ev.targetId == id) continue;
            if (!any) js << (first ? "" : ",") << "\"" << id << "\":[";
            js << (any ? "," : "") << "[" << Num(ev.startTime) << "," << Num(ev.endTime) << "," << ev.targetId << "," << ev.skillId << ","
               << (ev.isInstant ? 1 : 0) << "]";
            any = true;
            first = false;
        }
        if (any) js << "]";
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

    // Who picked up or dropped a flag, a vine seed or a repair kit: the Observer's on-screen announcements
    // (BuildFlagMessages), for Iris's chat. A bundle's drop names the player of the carry it ends.
    struct Move { float t; int kind, agent, flagTeam, bundle; };
    std::vector<Move> moves;
    if (m_flagTimeline.valid)
    {
        for (const FlagTimelineEvent& ev : m_flagTimeline.allEvents)
            if ((ev.eventType == FlagTimelineEventType::Pickup || ev.eventType == FlagTimelineEventType::Drop) && ev.actorAgentId >= 0)
                moves.push_back({ev.time, ev.eventType == FlagTimelineEventType::Pickup ? 0 : 1, ev.actorAgentId, ev.flagTeam == FlagTeam::Red ? 2 : 1, 0});
        for (const BundleTimeline& b : m_flagTimeline.bundles)
        {
            int carrier = -1;
            for (const BundleTimelineEvent& e : b.events)
            {
                if (e.newLocation == BundleLocation::Carried && e.carrierAgentId >= 0)
                {
                    carrier = e.carrierAgentId;
                    moves.push_back({e.time, 0, carrier, 0, b.type == BundleType::RepairKit ? 1 : 2});
                }
                else
                {
                    if (e.newLocation == BundleLocation::Ground && carrier >= 0)
                        moves.push_back({e.time, 1, carrier, 0, b.type == BundleType::RepairKit ? 1 : 2});
                    carrier = -1;
                }
            }
        }
        std::sort(moves.begin(), moves.end(), [](const Move& a, const Move& b) { return a.t < b.t; });
    }
    js << ",\"flag_moves\":[";
    for (size_t i = 0; i < moves.size(); i++)
        js << (i ? "," : "") << "[" << Num(moves[i].t) << "," << moves[i].kind << "," << moves[i].agent << "," << moves[i].flagTeam << ","
           << moves[i].bundle << "]";
    js << "]";
}
