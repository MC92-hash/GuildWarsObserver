#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "SkillDatabase.h"
#include <algorithm>
#include <cctype>
#include <cmath>
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
            in.uses.push_back({ agentId, su.targetId > 0 ? su.targetId : agentId, su.skillId, su.startTime, su.endTime, su.wasCancelled || su.wasInterrupted });
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
        p.value  = ce.value;
        p.type   = ce.damage_type;
        p.kind = ce.type == "DAMAGE"       ? PacketKind::Damage
               : ce.type == "HEAL"         ? PacketKind::Heal
               : ce.type == "KNOCKED_DOWN" ? PacketKind::KnockedDown
               : ce.type == "INTERRUPTED"  ? PacketKind::Interrupted
                                           : PacketKind::Other;
        // INTERRUPTED: the parser keeps the victim as caster_id, the interrupter as target_id.
        if (p.kind == PacketKind::Interrupted) { p.cause = ce.target_id; p.target = ce.caster_id; p.value = 0.f; }
        in.packets.push_back(p);
    }
    for (const BasicAttackEvent& a : stoc.basicAttack)
    {
        const bool finished = a.type == "ATTACK_FINISHED";
        if (!finished && a.type != "ATTACK_STARTED") continue;
        in.attacks.push_back({ a.time, a.caster_id, a.target_id, finished });
    }
    std::stable_sort(in.attacks.begin(), in.attacks.end(),
                     [](const Attack& a, const Attack& b) { return a.time < b.time; });
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
        const AttributeModel::AttributeRange* a = AttributeModel::SolvedRange(&it->second, attribute);
        return a ? a->best : -1;
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

    // Removal-scaled disable audit: %TEMP%\gwo_removal_audit_<match>.txt. Shatter Storm is
    // disabled 7 s per enchantment it removed, Withdraw Hexes recharges 2 s longer per hex; the
    // caster's next use of the same skill can come no earlier than that.
    struct Scaled { int skill; Kind kind; float perItem; };
    static const Scaled kScaled[] = { { 933, Kind::Enchantment, 7.f }, { 942, Kind::Hex, 2.f } };
    if (fopen_s(&f, (std::string(tempDir) + "gwo_removal_audit_" + folder + ".txt").c_str(), "w") != 0 || !f) return;
    fprintf(f, "# skill\tcaster\ttarget\tstart\tcast_end\tresult\tremoved\tmaintained\tborderline\tbase_recharge"
               "\tpredicted_ready\tnext_start\tslack\timplied_max\tboost\titems\n");
    if (!m_moraleTimelineBuilt) BuildMoraleTimelines();
    for (const auto& [casterId, ard] : m_replayCtx.agents)
    {
        const auto& hist = ard.skillUseHistory;
        for (size_t i = 0; i < hist.size(); ++i)
        {
            const SkillUseEvent& ev = hist[i];
            const int sid = m_skillView.ResolvePvpSkillId(ev.skillId);
            const Scaled* sc = nullptr;
            for (const Scaled& s : kScaled) if (s.skill == sid) sc = &s;
            if (!sc) continue;
            const float castEnd = ev.isInstant ? ev.startTime : ev.endTime;
            const char* result = ev.wasInterrupted ? "interrupted" : ev.wasCancelled ? "cancelled" : "landed";
            const SkillInfo* si = m_skillView.Get(sid);
            const float base = si ? si->recharge : 0.f;

            // What the timeline says this cast took: every effect of the kind removed by this skill
            // from this caster, ending around the cast's end.
            int removed = 0, maintained = 0, borderline = 0;
            std::string items;
            for (const auto& [agentId, list] : m_effectTimeline.byAgent)
                for (const Effect& e : list)
                {
                    if (e.kind != sc->kind) continue;
                    const bool mine = m_skillView.ResolvePvpSkillId(e.removedBy) == sid && e.removedByCaster == casterId
                                   && e.end >= ev.startTime - 0.05f && e.end <= castEnd + 1.0f;
                    // Ran out on its own just before or after the landing: the count hangs on its estimate.
                    const bool closeCall = !mine && agentId == ev.targetId && e.end >= castEnd - 1.5f && e.end <= castEnd + 0.5f
                                   && e.start < castEnd;
                    if (!mine && !closeCall) continue;
                    if (mine) { ++removed; if (!e.hasDuration) ++maintained; }
                    else ++borderline;
                    const SkillInfo* es = m_skillView.Get(e.skillId);
                    char buf[200];
                    snprintf(buf, sizeof(buf), "%s%s[%d %s c%d %.2f-%.2f %s exp%.2f]", items.empty() ? "" : " ",
                             mine ? "" : "~", agentId, es ? es->name.c_str() : "?", e.caster, e.start, e.end,
                             EndSourceName(e.endSource), e.expectedEnd);
                    items += buf;
                }

            // The next attempt at the same skill: any start proves it was ready then.
            float next = -1.f;
            for (size_t j = i + 1; j < hist.size(); ++j)
                if (m_skillView.ResolvePvpSkillId(hist[j].skillId) == sid && hist[j].startTime > castEnd)
                { next = hist[j].startTime; break; }
            const bool landed = !ev.wasInterrupted && !ev.wasCancelled;
            const float predicted = castEnd + base + (landed ? sc->perItem * removed : 0.f);
            const float slack = next >= 0.f ? next - predicted : 0.f;
            const int impliedMax = next >= 0.f ? (int)std::floor((next - castEnd - base + 0.25f) / sc->perItem) : -1;
            // A morale boost between the cast and the recast recharged everything: no bound then.
            int boost = 0;
            if (auto bit = m_moraleBoosts.find(static_cast<int>(ard.teamId)); bit != m_moraleBoosts.end())
                for (float bt : bit->second) if (bt > castEnd && (next < 0.f || bt < next)) boost = 1;
            fprintf(f, "%s\t%d\t%d\t%.3f\t%.3f\t%s\t%d\t%d\t%d\t%.1f\t%.3f\t%.3f\t%.3f\t%d\t%d\t%s\n",
                    si ? si->name.c_str() : "?", casterId, ev.targetId, ev.startTime, castEnd, result, removed,
                    maintained, borderline, base, predicted, next, next >= 0.f ? slack : 0.f, impliedMax, boost, items.c_str());
        }
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
