#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "SkillDatabase.h"
#include "MatchStats.h"

// The Library Stats tab's extra columns: the replay's models handed to the private MatchStats
// families, whose records ExportMatchAnalysis writes into match_analysis.json ("stats").
// MatchStats.h is included only here, by the export and by the Library, so a new input does not
// rebuild every file that includes ReplayWindow.h.

void ReplayWindow::FillMatchStatsInputs(MatchStats::Inputs& in) const
{
    in.stoc = &m_replayCtx.stocData;
    in.agents = &m_replayCtx.agents;
    in.skills = &m_skillView;
    in.team1 = m_team1PlayerIds;
    in.team2 = m_team2PlayerIds;

    // Agents by their recorded id (combat packets use it): a reused id's incarnations share it.
    std::unordered_map<int, int> teamOfRaw;
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        const int raw = ard.originalAgentId >= 0 ? ard.originalAgentId : id;
        if (ard.teamId) { teamOfRaw[raw] = ard.teamId; teamOfRaw[id] = ard.teamId; }
    }
    in.team = [teamOfRaw](int agent) -> int {
        auto it = teamOfRaw.find(agent);
        return it == teamOfRaw.end() ? 0 : it->second;
    };
    in.position = [this](int agentId, float t, float& x, float& y) -> bool {
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return false;
        const AgentSnapshot* s = FindSnapshotAtTime(it->second, t);
        if (!s || s->is_dead) return false;
        x = s->x;
        y = s->y;
        return true;
    };
    in.rank = [this](int agentId, int attribute) -> int {
        auto it = m_attrProfiles.find(agentId);
        if (it == m_attrProfiles.end()) return -1;
        const AttributeModel::AttributeRange* a = AttributeModel::SolvedRange(&it->second, attribute);
        return a ? a->best : -1;
    };
    in.effects = EffectTimelineTable();
    in.energy = [this](int agentId) { return EnergyTrackFor(agentId); };
    if (!m_ledgerBuilt || (!m_ledgerUsedAttributes && m_attributesDeduced)) BuildBattleLedger();
    in.ledger = &m_ledgerData;
    in.flags = m_flagTimelineBuilt && m_flagTimeline.valid ? &m_flagTimeline : nullptr;
    in.matchStart = m_displayTimeOffset;
    in.matchEnd = m_replayCtx.maxReplayTime;

    // ── Inputs a family adds go below, each in a block named after its file ──

    // MatchStats_Interrupts.cpp
    // The Piano Roll's spikes. BuildSpikes only fills the roll's cache, so a const export may build it.
    if (!m_spikesBuilt) const_cast<ReplayWindow*>(this)->BuildSpikes();
    auto toSpikeCast = [this](const CastRef& r) {
        return MatchStats::Inputs::SpikeCast{ r.casterId, m_skillView.ResolvePvpSkillId(r.ev->skillId), r.ev->startTime, r.ev->endTime };
    };
    for (const SpikeInfo& s : m_spikes)
    {
        MatchStats::Inputs::Spike sp;
        sp.target = s.target; sp.cue = s.cue; sp.firstHit = s.firstHit; sp.lastHit = s.lastHit;
        for (const CastRef& r : s.hits)    sp.hits.push_back(toSpikeCast(r));
        for (const CastRef& r : s.answers) sp.answers.push_back(toSpikeCast(r));
        in.spikes.push_back(std::move(sp));
    }
}
