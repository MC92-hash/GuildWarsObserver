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
    js << "{\"v\":1,\"folder\":\"" << Esc(m_matchMeta.folder_name) << "\",\"end\":" << Num(end);

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
    js << "]}";

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
