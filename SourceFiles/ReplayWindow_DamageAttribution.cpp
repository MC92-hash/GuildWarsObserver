#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "SkillDatabase.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

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

    std::unordered_map<int, SkillFacts> facts;
    in.skill = [&](int skillId) -> const SkillFacts* {
        auto it = facts.find(skillId);
        if (it != facts.end()) return &it->second;
        const SkillInfo* si = m_skillView.Get(skillId);
        if (!si) return nullptr;
        return &facts.emplace(skillId, SkillFacts{ si->type, si->profession, si->name, si->description }).first->second;
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
    fprintf(f, "# index\ttime\ttype\tcause\tvictim\tvalue\tdamage_type\tskill\tname\trule\tweapon_hit\n");
    for (size_t i = 0; i < stoc.combat.size(); ++i)
    {
        const CombatEvent& ce = stoc.combat[i];
        if (!ce.IsDamageOrHeal()) continue;
        const Result& r = m_damageAttribution.packets[i];
        const SkillInfo* si = r.skillId ? m_skillView.Get(r.skillId) : nullptr;
        fprintf(f, "%zu\t%.3f\t%s\t%d\t%d\t%.6f\t%d\t%d\t%s\t%s\t%d\n", i, ce.time, ce.type.c_str(),
                ce.caster_id, ce.target_id, ce.value, ce.damage_type, r.skillId,
                si ? si->name.c_str() : (r.skillId ? "?" : "-"), RuleName(r.rule), r.weaponHit ? 1 : 0);
    }
    fclose(f);
}

const DamageAttribution::Table* ReplayWindow::DamageAttributionTable() const
{
    if (!m_replayCtx.stocLoaded || !m_skillUseTimelineBuilt) return nullptr;
    if (!m_damageAttributionBuilt) BuildDamageAttribution();
    return &m_damageAttribution;
}

const std::vector<ReplayWindow::DamagePop>& ReplayWindow::DamagePopsOn(int agentId) const
{
    static const std::vector<DamagePop> kNone;
    const DamageAttribution::Table* table = DamageAttributionTable();
    if (!table) return kNone;
    auto cached = m_damagePops.find(agentId);
    if (cached != m_damagePops.end()) return cached->second;

    std::vector<DamagePop>& pops = m_damagePops[agentId];
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
        p.weaponHit = r.weaponHit && !(ce.damage_type == 55 && !p.heal);
        p.value    = ce.value;
        pops.push_back(p);
    }
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
