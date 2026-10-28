#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "SkillDatabase.h"
#include "MaxHpSolver.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

// ---------------------------------------------------------------------------
// Which skill each damage and heal packet came from. The rules live in DamageAttribution
// (private); this file hands them the window's data, groups the packets into the floating numbers
// the pops draw, and writes a developer dump.
// ---------------------------------------------------------------------------

void ReplayWindow::BuildDamageAttribution() const
{
    using namespace DamageAttribution;
    const StoCData& stoc = m_replayCtx.stocData;

    Inputs in;
    in.mapId = m_replayCtx.mapId;
    in.packets.reserve(stoc.combat.size());
    for (const CombatEvent& ce : stoc.combat)
    {
        Packet p;
        p.time       = ce.time;
        p.caster     = ce.caster_id;
        p.target     = ce.target_id;
        p.value      = ce.value;
        p.damageType = ce.damage_type;
        p.kind = ce.type == "DAMAGE"       ? PacketKind::Damage
               : ce.type == "HEAL"         ? PacketKind::Heal
               : ce.type == "INTERRUPTED"  ? PacketKind::Interrupted
               : ce.type == "KNOCKED_DOWN" ? PacketKind::KnockedDown
                                           : PacketKind::Other;
        // The number the floater shows for it, so the rules can read a skill's scale.
        if (p.kind == PacketKind::Damage || p.kind == PacketKind::Heal)
        {
            if (const uint32_t mhp = PacketMaxHp(ce.target_id, ce.time, ce.value))
            {
                p.amount = (int)std::lround(std::fabs(ce.value) * (double)mhp);
                p.maxHp = (int)mhp;
            }
            if (p.kind == PacketKind::Heal)
                if (auto ait = m_replayCtx.agents.find(ce.target_id); ait != m_replayCtx.agents.end())
                    if (const AgentSnapshot* s = FindSnapshotAtTime(ait->second, ce.time))
                        p.deepWound = s->has_deep_wound;
        }
        in.packets.push_back(p);
    }
    for (const auto& [agentId, ard] : m_replayCtx.agents)
        for (const SkillUseEvent& su : ard.skillUseHistory)
            in.uses.push_back({ agentId, su.targetId, su.skillId, su.endTime, su.wasCancelled });
    for (const BasicAttackEvent& b : stoc.basicAttack)
        if (b.type == "ATTACK_FINISHED" || b.type == "ATTACK_STARTED")
            in.attacks.push_back({ b.time, b.caster_id, b.target_id, b.type == "ATTACK_FINISHED" });
    for (const SkillDamageEvent& t : stoc.skillDamage)
        in.tags.push_back({ t.time, t.victimId, t.skillId });
    for (const ProjectileEvent& s : stoc.projectiles)
        in.shots.push_back({ s.time, s.sourceId, s.flight });
    for (const LifecycleEvent& l : stoc.lifecycle)
    {
        // The lifecycle carries the agent's incarnation (a reused id split into a synthetic one,
        // SplitRecycledAgents); the combat packets carry the recorded id. Give the rules the
        // recorded one: a spawn's removal is then the first of that id after it.
        int raw = l.agent_id;
        if (auto ait = m_replayCtx.agents.find(l.agent_id); ait != m_replayCtx.agents.end() && ait->second.originalAgentId >= 0)
            raw = ait->second.originalAgentId;
        if (l.isAdd) in.spawns.push_back({ l.time, l.agent_type, raw });
        else         in.removes.push_back({ l.time, raw });
    }
    // The recording names the flux when the recorder saw it; the month decides otherwise.
    AttributeModel::Flux flux = AttributeModel::FluxFromName(m_matchMeta.flux);
    if (flux == AttributeModel::Flux::None && m_matchMeta.month > 0)
        flux = AttributeModel::FluxForMonth(m_matchMeta.month);
    in.minionApocalypse = flux == AttributeModel::Flux::MinionApocalypse;

    std::unordered_map<int, SkillFacts> facts;
    in.skill = [&](int skillId) -> const SkillFacts* {
        auto it = facts.find(skillId);
        if (it != facts.end()) return &it->second;
        const SkillInfo* si = m_skillView.Get(skillId);
        if (!si) return nullptr;
        return &facts.emplace(skillId, SkillFacts{ si->type, si->profession, si->name, si->description, si->sacrifice, si->attribute }).first->second;
    };

    // Agents by their recorded id (combat packets use it): a reused id's incarnations share it.
    std::unordered_map<int, int> teamOfRaw;
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        const int raw = ard.originalAgentId >= 0 ? ard.originalAgentId : id;
        if (ard.teamId) teamOfRaw[raw] = ard.teamId;
    }
    in.team = [teamOfRaw](int agent) -> int {
        auto it = teamOfRaw.find(agent);
        return it == teamOfRaw.end() ? 0 : it->second;
    };
    in.rank = [this](int agentId, int attribute) -> int {
        auto it = m_attrProfiles.find(agentId);
        if (it == m_attrProfiles.end()) return -1;
        const AttributeModel::AttributeRange* a = AttributeModel::SolvedRange(&it->second, attribute);
        return a ? a->best : -1;
    };
    // When each agent died: his snapshot turning dead or to 0 Health.
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        const int raw = ard.originalAgentId >= 0 ? ard.originalAgentId : id;
        bool wasDead = true;
        for (const AgentSnapshot& sn : ard.snapshots)
        {
            const bool dead = sn.is_dead || sn.health_pct <= 0.f;
            if (dead && !wasDead) in.deaths.push_back({ sn.time, raw });
            wasDead = dead;
        }
    }

    const EffectTimeline::Table* effects = EffectTimelineTable();
    if (effects)
        in.effectsOn = [effects](int agent, float t, std::vector<int>& out) {
            for (const EffectTimeline::Effect* e : effects->ActiveAt(agent, t)) out.push_back(e->skillId);
        };

    m_damageAttribution = Build(in);
    m_damagePops.clear();
    m_damageAttributionBuilt = true;

    // Developer dump, the way the energy model writes its own: %TEMP%\gwo_damage_<match>.txt
    // whenever GWO_DAMAGE_DEBUG is set. One line per damage or heal packet.
    char flag[16] = {};
    char tempDir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("GWO_DAMAGE_DEBUG", flag, (DWORD)sizeof(flag)) == 0 ||
        GetTempPathA(MAX_PATH, tempDir) == 0)
        return;
    std::string folder = m_replayCtx.matchFolderPath.filename().string();
    if (folder.empty()) folder = "replay";
    for (char& c : folder)
        if (!std::isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '_';
    FILE* f = nullptr;
    if (fopen_s(&f, (std::string(tempDir) + "gwo_damage_" + folder + ".txt").c_str(), "w") != 0 || !f) return;
    fprintf(f, "# index\ttime\ttype\tcause\tvictim\tvalue\tdamage_type\tskill\tname\trule\tweapon_hit\tamount\tmax_hp_fix\tmax_hp\n");
    for (size_t i = 0; i < stoc.combat.size(); ++i)
    {
        const CombatEvent& ce = stoc.combat[i];
        if (!ce.IsDamageOrHeal()) continue;
        const Result& r = m_damageAttribution.packets[i];
        const SkillInfo* si = r.skillId ? m_skillView.Get(r.skillId) : nullptr;
        const char* name = si ? si->name.c_str()
                         : r.skillId == kSpikedCoral      ? "Spiked Coral"
                         : r.skillId == kAcidTrap         ? "Acid Trap"
                         : r.skillId == kMinionApocalypse ? "Minion Apocalypse"
                         : r.skillId == 657               ? "Life Draining"
                         : r.skillId ? "?" : "-";
        fprintf(f, "%zu\t%.3f\t%s\t%d\t%d\t%.6f\t%d\t%d\t%s\t%s\t%d\t%d\t%u\t%d\n", i, ce.time, ce.type.c_str(),
                ce.caster_id, ce.target_id, ce.value, ce.damage_type, r.skillId,
                name, RuleName(r.rule), r.weaponHit ? 1 : 0,
                in.packets[i].amount, m_damageAttribution.maxHpFix[i], in.packets[i].maxHp);
    }
    fclose(f);
}

// Beside the damage dump: %TEMP%\gwo_combatlog_<match>.txt, one line per damage or heal row of the
// combat log, with the packet it read, so its numbers can be compared with the floaters'.
void ReplayWindow::WriteCombatLogValueDump() const
{
    char flag[16] = {};
    char tempDir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("GWO_DAMAGE_DEBUG", flag, (DWORD)sizeof(flag)) == 0 ||
        GetTempPathA(MAX_PATH, tempDir) == 0)
        return;
    std::string folder = m_replayCtx.matchFolderPath.filename().string();
    if (folder.empty()) folder = "replay";
    for (char& c : folder)
        if (!std::isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '_';
    FILE* f = nullptr;
    if (fopen_s(&f, (std::string(tempDir) + "gwo_combatlog_" + folder + ".txt").c_str(), "w") != 0 || !f) return;
    fprintf(f, "# time\tcategory\tcaster\ttarget\tskill\tvalue\tvalue_abs\tpacket\n");
    for (const CombatLogRow& row : m_combatLog)
        if (row.category == CombatLogCategory::Damage || row.category == CombatLogCategory::Heal)
            fprintf(f, "%.3f\t%s\t%d\t%d\t%d\t%.6f\t%d\t%d\n", row.time,
                    row.category == CombatLogCategory::Damage ? "DAMAGE" : "HEAL", row.casterId, row.targetId,
                    row.skillId, row.valuePct, row.valueAbs, row.combatIndex);
    fclose(f);
}

const DamageAttribution::Table* ReplayWindow::DamageAttributionTable() const
{
    if (!m_replayCtx.stocLoaded || !m_skillUseTimelineBuilt) return nullptr;
    if (!m_damageAttributionBuilt) BuildDamageAttribution();
    return &m_damageAttribution;
}

// The vampiric weapon mod is the hidden skill 657, Life Draining. Its icon (Gw.dat 383607, read from
// the skill table at Gw.exe 0x98B4B0 + 657 * 0xA4, +0x90) ships as Textures/Skill_Icons/[657].
static constexpr int kLifeDrainingIcon = 657;

const std::vector<ReplayWindow::DamagePop>& ReplayWindow::DamagePopsOn(int agentId) const
{
    static const std::vector<DamagePop> kNone;
    const DamageAttribution::Table* table = DamageAttributionTable();
    if (!table) return kNone;
    auto cached = m_damagePops.find(agentId);
    if (cached != m_damagePops.end()) return cached->second;

    std::vector<DamagePop>& pops = m_damagePops[agentId];
    std::vector<int> damageTypes;   // per pop, -1 for a heal
    const auto& combat = m_replayCtx.stocData.combat;
    for (size_t i = 0; i < combat.size(); ++i)
    {
        const CombatEvent& ce = combat[i];
        if (ce.target_id != agentId || !ce.IsDamageOrHeal()) continue;
        const DamageAttribution::Result& r = table->packets[i];
        DamagePop p;
        p.time     = ce.time;
        p.casterId = ce.caster_id;
        p.skillId  = r.skillId;
        p.heal     = ce.type == "HEAL";
        // A weapon's vampiric part is life stolen, not an attack: its number has no attack icon.
        // Nor is the attacker's own heal from a steal (Avatar of Grenth, Grenth's Aura, a vampiric
        // weapon): it lands in the hit's tick and the attribution calls it a weapon hit, which drew
        // it in the damage digits.
        p.weaponHit = r.weaponHit && !p.heal && ce.damage_type != 55;
        // It shows the client's own icon for it instead: the hidden skill Life Draining's.
        if (r.weaponHit && !p.heal && ce.damage_type == 55) p.skillId = kLifeDrainingIcon;
        p.value    = ce.value;
        p.maxHpFix = i < table->maxHpFix.size() ? table->maxHpFix[i] : 0;
        pops.push_back(p);
        damageTypes.push_back(p.heal ? -1 : (int)ce.damage_type);
    }
    // One icon per hit. An attack skill under a steal (Avatar of Grenth, Grenth's Aura, a vampiric
    // weapon) arrives as several damage packets in one tick - the hit and its stolen parts - and the
    // attribution names the skill on each. When a stolen part (type 55) is among them, the skill
    // shows once, beside the hit; the stolen numbers come alone - with Life Draining's icon when the
    // skill is an attack, whose steal is the weapon's.
    struct Group { size_t holder = SIZE_MAX; bool steal = false; };
    std::map<std::tuple<int, long, int>, Group> groups;   // (cause, tick ms, skill)
    auto keyOf = [&](size_t k) { return std::make_tuple(pops[k].casterId, std::lround(pops[k].time * 1000.f), pops[k].skillId); };
    for (size_t k = 0; k < pops.size(); ++k)
    {
        if (pops[k].heal || pops[k].skillId <= 0 || pops[k].skillId == kLifeDrainingIcon) continue;
        Group& g = groups[keyOf(k)];
        const bool stolen = damageTypes[k] == 55;
        if (stolen) g.steal = true;
        if (g.holder == SIZE_MAX || (damageTypes[g.holder] == 55 && !stolen)) g.holder = k;
    }
    for (size_t k = 0; k < pops.size(); ++k)
    {
        if (pops[k].heal || pops[k].skillId <= 0 || pops[k].skillId == kLifeDrainingIcon) continue;
        const Group& g = groups[keyOf(k)];
        if (!g.steal || g.holder == k) continue;
        const SkillInfo* si = m_skillView.Get(pops[k].skillId);
        const bool attack = si && ((si->type >= 2 && si->type <= 12) || si->type == 31);
        pops[k].skillId = (attack && damageTypes[k] == 55) ? kLifeDrainingIcon : 0;
    }
    AppendCatapultHits(agentId, pops);
    std::stable_sort(pops.begin(), pops.end(), [](const DamagePop& a, const DamagePop& b) { return a.time < b.time; });
    return pops;
}

std::string ReplayWindow::DamagePopLabel(const DamagePop& pop, uint32_t maxHp)
{
    auto amount = [&](float fraction) {
        const int raw = maxHp > 0 ? (int)std::lround(std::fabs(fraction) * maxHp) : 0;
        return raw > 0 ? std::format("{}", raw) : std::format("{:.0f}%", std::fabs(fraction) * 100.f);
    };
    const char* sign = pop.heal ? "+" : "-";
    return std::format("{}{}", sign, amount(pop.value));
}
