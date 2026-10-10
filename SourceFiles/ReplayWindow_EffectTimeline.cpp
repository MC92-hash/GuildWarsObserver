#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "SkillDatabase.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

// ---------------------------------------------------------------------------
// The skill effects on each player. The rules live in EffectTimeline (private); this file hands
// them the window's data and writes a developer dump.
// ---------------------------------------------------------------------------

void ReplayWindow::BuildEffectTimeline() const
{
    using namespace EffectTimeline;
    const StoCData& stoc = m_replayCtx.stocData;

    Inputs in;
    in.matchEnd = m_replayCtx.maxReplayTime;
    in.visuals.reserve(stoc.visualEffects.size());
    for (const VisualEffectEvent& v : stoc.visualEffects)
    {
        Visual x;
        x.time  = v.time;
        x.agent = v.agentId;
        x.value = v.value;
        x.other = v.otherId;
        switch (v.kind)
        {
        case VisualEffectEvent::Kind::Add:      x.kind = VisualKind::Add; break;
        case VisualEffectEvent::Kind::Remove:   x.kind = VisualKind::Remove; break;
        case VisualEffectEvent::Kind::OnAgent:  x.kind = VisualKind::OnAgent; break;
        case VisualEffectEvent::Kind::OnTarget: x.kind = VisualKind::OnTarget; break;
        case VisualEffectEvent::Kind::State:    x.kind = VisualKind::State; break;
        }
        in.visuals.push_back(x);
    }
    for (const auto& [agentId, ard] : m_replayCtx.agents)
    {
        for (const SkillUseEvent& su : ard.skillUseHistory)
            in.uses.push_back({ agentId, su.targetId > 0 ? su.targetId : agentId, su.skillId, su.startTime, su.endTime, su.wasCancelled });
        // Avatar forms: from one change into an avatar model to the next change.
        for (size_t i = 0; i < ard.modelChanges.size(); ++i)
        {
            const ModelChangeEvent& mc = ard.modelChanges[i];
            if (LookupAvatarFileHash(mc.modelId) == 0) continue;
            const bool ended = i + 1 < ard.modelChanges.size();
            in.forms.push_back({ agentId, static_cast<int>(mc.modelId), mc.time,
                                 ended ? ard.modelChanges[i + 1].time : m_replayCtx.maxReplayTime, ended });
        }
        float last = -1.f;
        for (const AgentSnapshot& s : ard.snapshots)
        {
            if (s.attack_speed_modifier == last) continue;
            last = s.attack_speed_modifier;
            in.attackSpeed.push_back({ s.time, agentId, s.attack_speed_modifier });
        }
    }
    std::stable_sort(in.attackSpeed.begin(), in.attackSpeed.end(),
                     [](const AttackSpeed& a, const AttackSpeed& b) { return a.time < b.time; });
    for (const CombatEvent& ce : stoc.combat)
    {
        Packet p;
        p.time   = ce.time;
        p.cause  = ce.caster_id;
        p.target = ce.target_id;
        p.kind = ce.type == "DAMAGE"       ? PacketKind::Damage
               : ce.type == "HEAL"         ? PacketKind::Heal
               : ce.type == "KNOCKED_DOWN" ? PacketKind::KnockedDown
                                           : PacketKind::Other;
        in.packets.push_back(p);
    }
    in.position = [this](int agentId, float t, float& x, float& y) -> bool {
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return false;
        const AgentSnapshot* s = FindSnapshotAtTime(it->second, t);
        if (!s || s->is_dead) return false;
        x = s->x;
        y = s->y;
        return true;
    };
    in.partyOf = [this](int agentId) -> std::vector<int> {
        std::vector<int> party;
        auto self = m_replayCtx.agents.find(agentId);
        if (self == m_replayCtx.agents.end()) return party;
        for (const auto& [id, ard] : m_replayCtx.agents)
            if (ard.type == AgentType::Player && ard.teamId == self->second.teamId) party.push_back(id);
        return party;
    };
    in.state = [this](int agentId, float t, AgentState& out) -> bool {
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return false;
        const AgentSnapshot* s = FindSnapshotAtTime(it->second, t);
        if (!s) return false;
        out.x       = s->x;
        out.y       = s->y;
        out.health  = s->health_pct;
        out.hpSlope = s->hp_pips;
        out.maxHp   = it->second.effectiveMaxHpAtTime(t);
        if (out.maxHp == 0) out.maxHp = s->max_hp;
        out.alive   = s->is_alive && !s->is_dead;
        return true;
    };
    in.mapId = m_replayCtx.mapId;
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        if (ard.type == AgentType::Player) { in.players.push_back(id); continue; }
        if (ard.type != AgentType::Spirit || ard.spiritSkillId <= 0) continue;
        // One entry per life: an agent id is reused, by another spirit as often as not. A spirit
        // is written seconds apart, so it lives until the sample that says otherwise.
        Spirit sp;
        sp.agent   = id;
        sp.skillId = ard.spiritSkillId;
        sp.range   = GetSpiritRange(ard.modelId);
        bool any = false;
        for (const AgentSnapshot& s : ard.snapshots)
        {
            if (!s.is_alive || s.is_dead || s.model_id != ard.modelId)
            {
                if (any) { sp.died = s.time; in.spirits.push_back(sp); }
                any = false;
                continue;
            }
            if (!any) { sp.born = s.time; sp.team = ard.teamId; any = true; }
            sp.died = s.time;
            if (s.team_id != 0) sp.team = s.team_id;
        }
        if (any) in.spirits.push_back(sp);
    }
    std::sort(in.players.begin(), in.players.end());
    in.team = [this](int agentId) -> int {
        auto it = m_replayCtx.agents.find(agentId);
        return it == m_replayCtx.agents.end() ? 0 : it->second.teamId;
    };
    std::unordered_map<int, SkillFacts> facts;
    in.skill = [&](int skillId) -> const SkillFacts* {
        auto it = facts.find(skillId);
        if (it != facts.end()) return &it->second;
        const SkillInfo* si = m_skillView.Get(skillId);
        if (!si) return nullptr;
        return &facts.emplace(skillId, SkillFacts{ si->type, si->profession, si->attribute, si->upkeep, si->name, si->concise, si->recharge }).first->second;
    };
    in.rank = [this](int agentId, int attribute) -> int {
        auto it = m_attrProfiles.find(agentId);
        if (it == m_attrProfiles.end()) return -1;
        auto a = it->second.attributes.find(attribute);
        if (a == it->second.attributes.end() || a->second.budgetOnly) return -1;
        return a->second.best;
    };

    in.skillHealth = &m_skillHealthSpans;
    in.stoc = &stoc;
    in.agentData = [this](int agentId) -> const AgentReplayData* {
        auto it = m_replayCtx.agents.find(agentId);
        return it == m_replayCtx.agents.end() ? nullptr : &it->second;
    };

    m_effectTimeline = Build(in);
    m_effectTimelineBuilt = true;
    m_effectTimelineUsedAttributes = m_attributesDeduced;

    // Developer dump: %TEMP%\gwo_effects_<match>.txt whenever GWO_EFFECT_DEBUG is set.
    char flag[16] = {};
    char tempDir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("GWO_EFFECT_DEBUG", flag, (DWORD)sizeof(flag)) == 0 ||
        GetTempPathA(MAX_PATH, tempDir) == 0)
        return;
    std::string folder = m_replayCtx.matchFolderPath.filename().string();
    if (folder.empty()) folder = "replay";
    for (char& c : folder)
        if (!std::isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '_';
    FILE* f = nullptr;
    if (fopen_s(&f, (std::string(tempDir) + "gwo_effects_" + folder + ".txt").c_str(), "w") != 0 || !f) return;
    fprintf(f, "# agent\tkind\tskill\tname\tcause\tcaster\tstart\tend\texact\tnatural\tsource\texpected_end\tremoved_by\n");
    static const char* kKind[] = { "enchantment", "hex", "condition", "stance", "weapon spell", "form",
                                   "ritual", "other", "terrain" };
    std::vector<int> agents;
    for (const auto& [agentId, list] : m_effectTimeline.byAgent) agents.push_back(agentId);
    std::sort(agents.begin(), agents.end());
    for (int agentId : agents)
        for (const Effect& e : m_effectTimeline.byAgent.at(agentId))
        {
            const SkillInfo* si = m_skillView.Get(e.skillId);
            fprintf(f, "%d\t%s\t%d\t%s\t%d\t%d\t%.3f\t%.3f\t%d\t%d\t%s\t%.3f\t%d\n", e.agent, kKind[(int)e.kind], e.skillId,
                    e.label ? e.label : si ? si->name.c_str() : "?", e.causeSkill, e.caster, e.start, e.end, e.endExact ? 1 : 0,
                    e.natural ? 1 : 0, EndSourceName(e.endSource), e.expectedEnd, e.removedBy);
        }
    fclose(f);
}

const EffectTimeline::Table* ReplayWindow::EffectTimelineTable() const
{
    if (!m_replayCtx.stocLoaded || !m_skillUseTimelineBuilt || !m_agentsClassified) return nullptr;
    if (!m_effectTimelineBuilt || (!m_effectTimelineUsedAttributes && m_attributesDeduced))
        BuildEffectTimeline();
    return &m_effectTimeline;
}
