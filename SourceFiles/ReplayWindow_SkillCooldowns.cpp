#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "SkillDatabase.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// Skill cooldowns and disables -- one model for every skill bar in the app.
//
// A slot runs two clocks (see SkillCooldowns.h). The recharge is read off the player's own last
// use; the disables are imposed by skills, and those are folded once per replay from the rule
// tables below, which transcribe the wiki's list of disabling skills (wiki: Disable). Every rule
// names the measured stream it rides on; a rule whose condition the recording cannot see (did the
// arrow hit, was the shooter Overcast) is either left out or states the assumption it makes.
// ---------------------------------------------------------------------------

namespace
{
    // "Disabled for an additional X seconds" locks the skill for its recharge PLUS X, not the
    // longer of the two. Measured over the archive (2026-10-05, 248 millisecond-stamped matches):
    // after Distracting Shot the victim's next use of the skill sits at R + 20 at every recharge
    // R -- a 20 s skill is back at 40.02 s, never at 20 -- in 454 of 457 cases; Power Lock and
    // Complicate agree. Skills worded without "additional" lock for X alone.
    constexpr bool kAdditionalAddsToRecharge = true;

    constexpr float kAreaRange   = 322.f;   // "in the area"
    constexpr float kQzRange     = 2512.f;  // spirit range
    constexpr int   kFallbackRank = 12;     // no evidence: the most a player can buy

    constexpr int kResurrectionSignet = 2;
    constexpr uint32_t kQzSpiritModelId = 2937;

    // Attributes, in the client's numbering (GWCA Constants::Attribute).
    constexpr int kAttrSmitingPrayers    = 14;
    constexpr int kAttrProtectionPrayers = 15;
    constexpr int kAttrHealingPrayers    = 13;
    constexpr int kAttrDaggerMastery     = 29;

    // Professions, in SkillInfo::profession numbering.
    constexpr int kProfMesmer   = 5;
    constexpr int kProfAssassin = 7;
    constexpr int kProfDervish  = 10;

    // The skill type ids of the skill data (SkillDatabase::GetTypeName).
    constexpr int kTypeLeadAttack = 5, kTypeOffhandAttack = 6, kTypeDualAttack = 7;
    constexpr int kTypeSpearAttack = 12, kTypeChant = 13, kTypePreparation = 17;
    constexpr int kTypeSignet = 21, kTypeStance = 29;

    // The index-th "A...B second" range of a description: the endpoints at rank 0 and rank 15.
    // Read from the match-dated description, so a balance change is honoured per replay.
    bool DescRange(const std::string& d, int index, float& v0, float& v15)
    {
        int n = 0;
        for (size_t pos = d.find("..."); pos != std::string::npos; pos = d.find("...", pos + 3)) {
            size_t b = pos;
            while (b > 0 && std::isdigit(static_cast<unsigned char>(d[b - 1]))) --b;
            size_t e = pos + 3, e2 = e;
            while (e2 < d.size() && std::isdigit(static_cast<unsigned char>(d[e2]))) ++e2;
            if (b == pos || e2 == e || d.compare(e2, 7, " second") != 0) continue;
            if (n++ != index) continue;
            v0  = std::stof(d.substr(b, pos - b));
            v15 = std::stof(d.substr(e, e2 - e));
            return true;
        }
        return false;
    }

    float AtRank(float v0, float v15, int rank)
    {
        return std::round(v0 + (v15 - v0) * static_cast<float>(rank) / 15.f);
    }

    bool IsAttack(const SkillInfo* si) { return si && SkillDatabase::IsWeaponAttack(si->type); }

    bool Covers(const SkillDisable& d, int sid, const SkillInfo* si)
    {
        switch (d.scope) {
        case DisableScope::Skill:        return sid == d.skillId;
        case DisableScope::All:          return true;
        case DisableScope::AllExcept:    return sid != d.skillId;
        case DisableScope::Attribute:    return si && si->attribute == d.param;
        case DisableScope::Elite:        return si && si->is_elite;
        case DisableScope::Attacks:      return IsAttack(si);
        case DisableScope::OtherAttacks: return sid != d.skillId && IsAttack(si);
        case DisableScope::NonAttacks:   return si && !IsAttack(si);
        case DisableScope::OtherNonAttacks: return sid != d.skillId && si && !IsAttack(si);
        case DisableScope::Signets:      return si && si->type == kTypeSignet;
        case DisableScope::Spells:       return si && SkillDatabase::IsSpellType(si->type);
        case DisableScope::StancesAndEnchantments:
            return si && (si->type == kTypeStance || SkillDatabase::IsEnchantmentType(si->type));
        case DisableScope::NonProfession: return si && si->profession != d.param;
        case DisableScope::NonProfessionNonSignet:
            return si && si->profession != d.param && si->type != kTypeSignet;
        case DisableScope::OtherAttributeSkills:
            return sid != d.skillId && si && si->attribute == d.param;
        case DisableScope::NonDaggerAttacks:
            return IsAttack(si) && si->attribute != kAttrDaggerMastery &&
                   si->type != kTypeLeadAttack && si->type != kTypeOffhandAttack && si->type != kTypeDualAttack;
        case DisableScope::NonSpearAttacks:
            return IsAttack(si) && si->type != kTypeSpearAttack;
        }
        return false;
    }

    // --- A. Interrupts that lock the interrupted skill --------------------------------------
    enum class Needs : uint8_t { Any, SpellOrChant, Spell, Attack };
    struct InterruptRule
    {
        int   skillId;
        Needs needs;
        bool  additional;       // "for an additional X seconds"
        int   rangeIndex;       // -1: `fixed`
        float fixed;
        DisableScope scope;     // Skill; Power Block: Attribute; Disarm: Attacks
        bool  area;             // Complicate: every foe in the area as well
        bool  perSignet;        // Signet of Distraction: X per signet on the caster's bar
        bool  casterEnchanted;  // Lyssa's Assault
    };
    // Not here, because the recording cannot see their condition: Distracting Strike (the target
    // must have Cracked Armor -- conditions are one flag), Rust (the caster must be Overcast) and
    // Shield Bash (a block, not an interrupt).
    constexpr InterruptRule kInterruptRules[] = {
        {    5, Needs::SpellOrChant, false,  0,  0.f, DisableScope::Attribute, false, false, false }, // Power Block
        { 1994, Needs::SpellOrChant, true,   0,  0.f, DisableScope::Skill,     false, false, false }, // Power Lock
        {  932, Needs::Any,          true,   0,  0.f, DisableScope::Skill,     true,  false, false }, // Complicate
        { 1053, Needs::Any,          true,   0,  0.f, DisableScope::Skill,     false, false, false }, // Psychic Distraction
        { 1350, Needs::Any,          false,  0,  0.f, DisableScope::Skill,     false, false, false }, // Simple Thievery
        { 1992, Needs::Spell,        false,  0,  0.f, DisableScope::Skill,     false, true,  false }, // Signet of Distraction
        { 1025, Needs::Spell,        false,  0,  0.f, DisableScope::Skill,     false, false, false }, // Disrupting Stab
        { 1538, Needs::Any,          true,   0,  0.f, DisableScope::Skill,     false, false, true  }, // Lyssa's Assault
        { 2066, Needs::Attack,       false,  0,  0.f, DisableScope::Attacks,   false, false, false }, // Disarm
        {  399, Needs::Any,          true,  -1, 20.f, DisableScope::Skill,     false, false, false }, // Distracting Shot
        {  340, Needs::Any,          true,  -1, 20.f, DisableScope::Skill,     false, false, false }, // Disrupting Chop
        {  445, Needs::Any,          true,  -1, 20.f, DisableScope::Skill,     false, false, false }, // Disrupting Lunge
        { 1726, Needs::Spell,        true,  -1, 10.f, DisableScope::Skill,     false, false, false }, // Magebane Shot
    };

    // --- B/C/D. Disables a successful use imposes, on its target or on its user -------------
    enum class Who : uint8_t { User, Target };
    struct UseRule
    {
        int   skillId;
        Who   who;
        DisableScope scope;
        int   param;            // attribute or profession, per scope
        int   rangeIndex;       // -1: `fixed`
        float fixed;
        int   delayIndex;       // -1: applies at once; else it starts that range later (Spell Shield)
    };
    constexpr UseRule kUseRules[] = {
        // On the target
        {   29, Who::Target, DisableScope::All,   0,  0,  0.f, -1 },   // Blackout
        {   62, Who::Target, DisableScope::Elite, 0,  0,  0.f, -1 },   // Signet of Humility
        {  306, Who::Target, DisableScope::All,   0,  0,  0.f, -1 },   // Rebirth (an ally)
        // On the user's other skills
        {   29, Who::User, DisableScope::All,                    0,               -1,  5.f, -1 }, // Blackout
        { 1053, Who::User, DisableScope::AllExcept,              0,               -1,  8.f, -1 }, // Psychic Distraction
        {   62, Who::User, DisableScope::NonProfession,          kProfMesmer,     -1,  4.f, -1 }, // Signet of Humility
        { 3188, Who::User, DisableScope::NonProfessionNonSignet, kProfMesmer,     -1, 10.f, -1 }, // Unnatural Signet (PvP)
        {  572, Who::User, DisableScope::Attacks,                0,               -1, 10.f, -1 }, // Deadly Paradox
        {  454, Who::User, DisableScope::NonAttacks,             0,               -1,  5.f, -1 }, // Tiger's Fury
        {  908, Who::User, DisableScope::NonAttacks,             0,               -1,  5.f, -1 }, // Marauder's Shot (assumes it hit)
        {  990, Who::User, DisableScope::OtherNonAttacks,        0,                0,  0.f, -1 }, // Expunge Enchantments
        {  909, Who::User, DisableScope::OtherAttacks,           0,                0,  0.f, -1 }, // Focused Shot
        { 1197, Who::User, DisableScope::OtherAttacks,           0,               -1,  2.f, -1 }, // Needling Shot
        { 1644, Who::User, DisableScope::NonDaggerAttacks,       0,               -1, 10.f, -1 }, // Wastrel's Collapse
        { 1605, Who::User, DisableScope::NonSpearAttacks,        0,               -1,  3.f, -1 }, // Wild Throw
        { 1642, Who::User, DisableScope::NonProfession,          kProfAssassin,   -1,  3.f, -1 }, // Hidden Caltrops
        {  570, Who::User, DisableScope::NonProfession,          kProfAssassin,   -1,  5.f, -1 }, // Mark of Insecurity
        { 1407, Who::User, DisableScope::Signets,                0,               -1, 12.f, -1 }, // Lion's Comfort
        {  801, Who::User, DisableScope::Spells,                 0,               -1, 15.f, -1 }, // Shroud of Silence
        { 1121, Who::User, DisableScope::OtherAttributeSkills,   kAttrHealingPrayers, 0, 0.f, -1 }, // Gift of Health
        {  269, Who::User, DisableScope::Attribute,              kAttrProtectionPrayers, -1, 5.f, -1 }, // Mark of Protection
        { 1118, Who::User, DisableScope::Attribute,              kAttrSmitingPrayers, -1, 20.f, -1 }, // Healing Burst
        { 2871, Who::User, DisableScope::Attribute,              kAttrSmitingPrayers, -1, 20.f, -1 }, // Light of Deliverance (PvP)
        {  266, Who::User, DisableScope::Attribute,              kAttrSmitingPrayers, -1, 20.f, -1 }, // Peace and Harmony
        { 3448, Who::User, DisableScope::Attribute,              kAttrSmitingPrayers, -1, 20.f, -1 }, // Peace and Harmony (PvP)
        { 1650, Who::User, DisableScope::Attacks,                0,               -1,  1.f, -1 }, // Shadow Walk
        { 1650, Who::User, DisableScope::StancesAndEnchantments, 0,               -1, 10.f, -1 }, // Shadow Walk
        {  957, Who::User, DisableScope::All,                    0,                1,  0.f,  0 }, // Spell Shield, when it ends
        // Skills that lock themselves past their recharge
        { 1518, Who::User, DisableScope::Skill, 0, -1, 45.f, -1 },  // Avatar of Balthazar
        { 1519, Who::User, DisableScope::Skill, 0, -1, 45.f, -1 },  // Avatar of Dwayna
        { 3270, Who::User, DisableScope::Skill, 0, -1, 45.f, -1 },  // Avatar of Dwayna (PvP)
        { 1520, Who::User, DisableScope::Skill, 0, -1, 45.f, -1 },  // Avatar of Grenth
        { 1521, Who::User, DisableScope::Skill, 0, -1, 45.f, -1 },  // Avatar of Lyssa
        { 1522, Who::User, DisableScope::Skill, 0, -1, 45.f, -1 },  // Avatar of Melandru
        { 3271, Who::User, DisableScope::Skill, 0, -1, 45.f, -1 },  // Avatar of Melandru (PvP)
        {  239, Who::User, DisableScope::Skill, 0, -1, 20.f, -1 },  // Ward Against Harm
        { 2806, Who::User, DisableScope::Skill, 0, -1, 30.f, -1 },  // Ward Against Harm (PvP)
        { 1093, Who::User, DisableScope::Skill, 0, -1, 20.f, -1 },  // Teinai's Heat
    };
    // Not modelled: Recall (the disable starts when the upkeep is dropped, which is not recorded),
    // Withdraw Hexes and Shatter Storm (they scale with hexes / enchantments removed), Echo,
    // Arcane Echo and Arcane Mimicry (they swap the slot for another skill).

    constexpr int kArcaneThievery = 81, kArcaneLarceny = 1062;
    constexpr int kAuspiciousIncantation = 930;
    constexpr int kOathShot = 405, kNeedlingShot = 1197, kWordOfCensure = 1129;
    constexpr float kXinraeSeconds = 3.f;

    // Recharge modifiers, all "for A...B seconds" at the user's rank.
    constexpr int kSerpentsQuickness = 456, kPracticedStance = 449, kDeadlyParadox = 572;
    constexpr int kAvatarOfLyssa = 1521, kLyssasHaste = 1512, kLyssasHastePvp = 3348;

    float CastEnd(const SkillUseEvent& ev) { return ev.isInstant ? ev.startTime : ev.endTime; }
    bool  Succeeded(const SkillUseEvent& ev) { return !ev.wasCancelled && !ev.wasInterrupted; }
}

int ReplayWindow::AttributeRankFor(int agentId, int attribute) const
{
    auto it = m_attrProfiles.find(agentId);
    if (it != m_attrProfiles.end()) {
        auto a = it->second.attributes.find(attribute);
        if (a != it->second.attributes.end() && !a->second.budgetOnly) return a->second.best;
    }
    return kFallbackRank;
}

// The duration a skill grants at its user's rank: the index-th "A...B second" range of its
// description, or `fixed` when the rule names one.
float ReplayWindow::SkillSecondsAtRank(int skillId, int userAgentId, int rangeIndex, float fixed) const
{
    if (rangeIndex < 0) return fixed;
    const SkillInfo* si = m_skillView.Get(skillId);
    float v0 = 0.f, v15 = 0.f;
    if (!si || !DescRange(si->description, rangeIndex, v0, v15)) return fixed;
    return AtRank(v0, v15, AttributeRankFor(userAgentId, si->attribute));
}

void ReplayWindow::BuildSkillDisables() const
{
    m_skillDisables.clear();
    m_rechargeResets.clear();
    m_skillDisablesUsedAttributes = m_attributesDeduced;

    const auto& sdb = m_skillView;
    auto resolve = [&](int sid) { return sdb.ResolvePvpSkillId(sid); };
    auto add = [&](int victim, const SkillDisable& d) {
        if (d.end > d.start) m_skillDisables[victim].push_back(d);
    };
    auto distance = [&](const AgentReplayData& a, const AgentReplayData& b, float t) {
        float ax, ay, az, bx, by, bz;
        InterpolateAgentPosition(a, t, m_replayCtx.interpSettings, ax, ay, az);
        InterpolateAgentPosition(b, t, m_replayCtx.interpSettings, bx, by, bz);
        return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
    };
    auto onBar = [&](int agentId, int resolvedSid) {
        for (int s : SkillBarForAgent(agentId)) if (resolve(s) == resolvedSid) return true;
        return false;
    };

    for (const auto& [casterId, caster] : m_replayCtx.agents) {
        if (caster.type != AgentType::Player) continue;

        for (size_t ei = 0; ei < caster.skillUseHistory.size(); ++ei) {
            const SkillUseEvent& ev = caster.skillUseHistory[ei];
            if (!Succeeded(ev)) continue;
            const int   rid = resolve(ev.skillId);
            const float t0  = CastEnd(ev);

            // A. Interrupts. The victim's cast that was cut short is the one that ended as this
            // skill landed: at once for a spell, within the flight or swing for an attack.
            for (const auto& rule : kInterruptRules) {
                if (resolve(rule.skillId) != rid) continue;
                if (rule.casterEnchanted) {
                    const AgentSnapshot* snap = FindSnapshotAtTime(caster, t0);
                    if (!snap || !snap->has_enchantment) continue;
                }
                auto vit = m_replayCtx.agents.find(ev.targetId);
                if (vit == m_replayCtx.agents.end()) continue;
                const AgentReplayData& victim = vit->second;

                const SkillInfo* ruleInfo = sdb.Get(rule.skillId);
                const bool slow = IsAttack(ruleInfo) || (ruleInfo && ruleInfo->type == 11);
                const float lo = ev.startTime - 0.1f;
                const float hi = std::max(ev.startTime, t0) + (slow ? 1.5f : 0.35f);

                const SkillUseEvent* cut = nullptr;
                for (const auto& vev : victim.skillUseHistory) {
                    if (!vev.wasInterrupted || vev.endTime < lo || vev.endTime > hi) continue;
                    if (!cut || std::abs(vev.endTime - t0) < std::abs(cut->endTime - t0)) cut = &vev;
                }
                if (!cut) continue;

                const SkillInfo* cutInfo = sdb.Get(cut->skillId);
                if (rule.needs == Needs::SpellOrChant &&
                    !(cutInfo && (SkillDatabase::IsSpellType(cutInfo->type) || cutInfo->type == kTypeChant))) continue;
                if (rule.needs == Needs::Spell && !(cutInfo && SkillDatabase::IsSpellType(cutInfo->type))) continue;
                if (rule.needs == Needs::Attack && !IsAttack(cutInfo)) continue;

                float secs = SkillSecondsAtRank(rule.skillId, casterId, rule.rangeIndex, rule.fixed);
                if (rule.perSignet) {
                    int signets = 0;
                    for (int s : SkillBarForAgent(casterId))
                        if (const SkillInfo* bi = sdb.Get(s); bi && bi->type == kTypeSignet) ++signets;
                    secs *= static_cast<float>(std::max(1, signets));
                }
                const float tCut = cut->endTime;
                float end = tCut + secs;
                if (rule.additional && kAdditionalAddsToRecharge && cutInfo) end += cutInfo->recharge;

                SkillDisable d;
                d.start = tCut; d.end = end;
                d.scope = rule.scope;
                d.skillId = resolve(cut->skillId);
                d.sourceSkill = rule.skillId; d.sourceAgent = casterId;
                if (rule.scope == DisableScope::Attribute) {
                    // Power Block: the skill, and every skill of its attribute. A skill with no
                    // attribute has no siblings.
                    if (!cutInfo || cutInfo->attribute >= 101) d.scope = DisableScope::Skill;
                    else d.param = cutInfo->attribute;
                }
                add(vit->first, d);

                if (rule.area) {
                    for (const auto& [otherId, other] : m_replayCtx.agents) {
                        if (otherId == vit->first || other.type != AgentType::Player) continue;
                        if (other.teamId != victim.teamId || !other.isAliveAtTime(tCut)) continue;
                        if (distance(victim, other, tCut) > kAreaRange) continue;
                        add(otherId, d);
                    }
                }
            }

            // B/C/D. Disables a successful use imposes.
            for (const auto& rule : kUseRules) {
                if (rule.skillId != ev.skillId && resolve(rule.skillId) != rid) continue;
                int victim = casterId;
                if (rule.who == Who::Target) {
                    if (ev.targetId <= 0 || ev.targetId == casterId) continue;
                    if (m_replayCtx.agents.find(ev.targetId) == m_replayCtx.agents.end()) continue;
                    victim = ev.targetId;
                }
                float start = t0;
                if (rule.delayIndex >= 0)
                    start += SkillSecondsAtRank(rule.skillId, casterId, rule.delayIndex, 0.f);
                SkillDisable d;
                d.start = start;
                d.end   = start + SkillSecondsAtRank(rule.skillId, casterId, rule.rangeIndex, rule.fixed);
                d.scope = rule.scope;
                d.skillId = rid;
                d.param = rule.param;
                d.sourceSkill = ev.skillId; d.sourceAgent = casterId;
                add(victim, d);
            }

            // Arcane Thievery / Larceny take ONE random spell for the duration. Which one is only
            // known if the thief casts it: a spell that is not on the thief's own bar, and is on
            // the victim's, used inside the window.
            if (rid == resolve(kArcaneThievery) || rid == resolve(kArcaneLarceny)) {
                const float dur = SkillSecondsAtRank(ev.skillId, casterId, 0, 0.f);
                if (ev.targetId > 0 && dur > 0.f) {
                    for (size_t j = ei + 1; j < caster.skillUseHistory.size(); ++j) {
                        const auto& nev = caster.skillUseHistory[j];
                        if (nev.startTime > t0 + dur) break;
                        const int nid = resolve(nev.skillId);
                        if (onBar(casterId, nid) || !onBar(ev.targetId, nid)) continue;
                        SkillDisable d;
                        d.start = t0; d.end = t0 + dur;
                        d.scope = DisableScope::Skill; d.skillId = nid;
                        d.sourceSkill = ev.skillId; d.sourceAgent = casterId;
                        add(ev.targetId, d);
                        break;
                    }
                }
            }

            // Auspicious Incantation: the next spell its user casts within 20 seconds is disabled
            // for an additional X seconds.
            if (rid == resolve(kAuspiciousIncantation)) {
                const float secs = SkillSecondsAtRank(ev.skillId, casterId, 0, 0.f);
                for (size_t j = ei + 1; j < caster.skillUseHistory.size(); ++j) {
                    const auto& nev = caster.skillUseHistory[j];
                    if (nev.startTime > t0 + 20.f) break;
                    if (!Succeeded(nev)) continue;
                    const SkillInfo* ni = sdb.Get(nev.skillId);
                    if (!ni || !SkillDatabase::IsSpellType(ni->type)) continue;
                    SkillDisable d;
                    d.start = CastEnd(nev);
                    d.end   = d.start + secs + (kAdditionalAddsToRecharge ? ni->recharge : 0.f);
                    d.scope = DisableScope::Skill; d.skillId = resolve(nev.skillId);
                    d.sourceSkill = ev.skillId; d.sourceAgent = casterId;
                    add(casterId, d);
                    break;
                }
            }

            // Skills that recharge others. Oath Shot is read as a hit (it only misses half the
            // time at Expertise 7 or less, and a Ranger running it has more); Needling Shot
            // recharges itself when it strikes a foe below half health, which the recording shows.
            if (rid == resolve(kOathShot))
                m_rechargeResets[casterId].push_back({ t0, resolve(kOathShot), 0 });
            if (rid == resolve(kNeedlingShot)) {
                auto tit = m_replayCtx.agents.find(ev.targetId);
                if (tit != m_replayCtx.agents.end() && tit->second.healthPctAtTime(t0) < 0.5f)
                    m_rechargeResets[casterId].push_back({ t0 + 0.01f, 0, rid });
            }
        }
    }

    // Xinrae's Revenge: every skill a player activates is disabled for 3 seconds for everyone
    // in the area, of either party, who has it on their bar -- but not for the player who used it.
    AttributeModel::Flux flux = AttributeModel::FluxFromName(m_matchMeta.flux);
    if (flux == AttributeModel::Flux::None && m_matchMeta.month > 0)
        flux = AttributeModel::FluxForMonth(m_matchMeta.month);
    if (flux == AttributeModel::Flux::XinraesRevenge) {
        for (const auto& [userId, user] : m_replayCtx.agents) {
            if (user.type != AgentType::Player) continue;
            for (const auto& ev : user.skillUseHistory) {
                if (!Succeeded(ev)) continue;
                const int rid = resolve(ev.skillId);
                const float t0 = CastEnd(ev);
                for (const auto& [otherId, other] : m_replayCtx.agents) {
                    if (otherId == userId || other.type != AgentType::Player) continue;
                    if (!onBar(otherId, rid) || distance(user, other, t0) > kAreaRange) continue;
                    SkillDisable d;
                    d.start = t0; d.end = t0 + kXinraeSeconds;
                    d.scope = DisableScope::Skill; d.skillId = rid;
                    d.sourceSkill = ev.skillId; d.sourceAgent = userId;
                    add(otherId, d);
                }
            }
        }
    }

    m_skillDisablesBuilt = true;
}

std::vector<SkillCooldownState> ReplayWindow::ComputeSkillCooldowns(
    const AgentReplayData& ard, const std::vector<int>& skillIds, float t) const
{
    if (!m_moraleTimelineBuilt) BuildMoraleTimelines();
    if (!m_skillDisablesBuilt || (!m_skillDisablesUsedAttributes && m_attributesDeduced))
        BuildSkillDisables();

    const auto& sdb = m_skillView;
    auto resolve = [&](int sid) { return sdb.ResolvePvpSkillId(sid); };

    // A morale boost recharges every skill: nothing that began before the last one survives it.
    float lastBoost = -1.f;
    if (auto bit = m_moraleBoosts.find(static_cast<int>(ard.teamId)); bit != m_moraleBoosts.end())
        for (float bt : bit->second) if (bt <= t) lastBoost = std::max(lastBoost, bt);

    // --- Recharge modifiers active now ---
    struct Modifier { float mult; const char* name; int only; };   // only: 0 all, else a filter
    enum : int { kAll = 0, kPreparations, kAssassin, kDervishEnchantments };
    std::vector<Modifier> mods;

    bool qz = false;
    {
        float px, py, pz;
        InterpolateAgentPosition(ard, t, m_replayCtx.interpSettings, px, py, pz);
        for (const auto& [sid, spirit] : m_replayCtx.agents) {
            if (spirit.modelId != kQzSpiritModelId) continue;
            if (spirit.isDeadAtTime(t) || !spirit.isAliveAtTime(t)) continue;
            float sx, sy, sz;
            InterpolateAgentPosition(spirit, t, m_replayCtx.interpSettings, sx, sy, sz);
            if (std::hypot(px - sx, py - sy) <= kQzRange) { qz = true; break; }
        }
    }
    // A modifier counts once however many casts of it overlap (Practiced Stance outlasts its own
    // recharge, so two can be running at once).
    auto addMod = [&](float mult, const char* name, int only) {
        for (const auto& m : mods) if (std::strcmp(m.name, name) == 0) return;
        mods.push_back({ mult, name, only });
    };
    if (qz) addMod(0.50f, "QZ", kAll);

    for (const auto& ev : ard.skillUseHistory) {
        if (ev.wasCancelled) continue;
        const float t0 = CastEnd(ev);
        if (t0 > t) continue;
        const int rid = resolve(ev.skillId);
        auto activeFor = [&](int rangeIndex, float fallback) {
            return t < t0 + SkillSecondsAtRank(ev.skillId, ard.agent_id, rangeIndex, fallback);
        };
        if (rid == resolve(kSerpentsQuickness) && activeFor(0, 20.f) && ard.healthPctAtTime(t) >= 0.5f)
            addMod(0.67f, "SQ", kAll);
        else if (rid == resolve(kPracticedStance) && activeFor(0, 20.f))
            addMod(0.50f, "PS", kPreparations);
        else if (rid == resolve(kDeadlyParadox) && activeFor(0, 10.f))
            addMod(0.67f, "DP", kAssassin);
        else if (rid == resolve(kAvatarOfLyssa) && activeFor(0, 45.f))
            addMod(0.50f, "AoL", kDervishEnchantments);
        else if ((rid == resolve(kLyssasHaste) || rid == resolve(kLyssasHastePvp)) && activeFor(0, 10.f))
            addMod(0.67f, "LH", kDervishEnchantments);
    }
    auto modApplies = [&](const Modifier& m, const SkillInfo* si) {
        switch (m.only) {
        case kAll:                 return true;
        case kPreparations:        return si && si->type == kTypePreparation;
        case kAssassin:            return si && si->profession == kProfAssassin;
        case kDervishEnchantments: return si && si->profession == kProfDervish && SkillDatabase::IsEnchantmentType(si->type);
        }
        return false;
    };

    const std::vector<SkillDisable>* disables = nullptr;
    if (auto dit = m_skillDisables.find(ard.agent_id); dit != m_skillDisables.end()) disables = &dit->second;
    const std::vector<RechargeReset>* resets = nullptr;
    if (auto rit = m_rechargeResets.find(ard.agent_id); rit != m_rechargeResets.end()) resets = &rit->second;

    std::vector<SkillCooldownState> out;
    out.reserve(skillIds.size());
    for (int rawSid : skillIds) {
        SkillCooldownState cd;
        cd.skillId = rawSid;
        const int sid = resolve(rawSid);
        const SkillInfo* si = sdb.Get(sid);

        // --- Recharge: the last use, cleared by a morale boost or a skill that recharges it ---
        float rechargeStart = -1.f, rechargeDur = 0.f;
        bool spent = false;
        const SkillUseEvent* last = nullptr;
        for (const auto& ev : ard.skillUseHistory) {
            // A cast the player let go of starts no recharge; an interrupted one (also recorded as
            // stopped) recharges in full, with any interrupt disable laid on top.
            if ((ev.wasCancelled && !ev.wasInterrupted) || ev.endTime > t || resolve(ev.skillId) != sid) continue;
            last = &ev;
        }
        if (last) {
            rechargeStart = CastEnd(*last);
            if (sid == kResurrectionSignet) spent = true;
            else rechargeDur = last->rechargeDuration;
            if (sid == resolve(kWordOfCensure) && !last->wasFastRecast) {
                auto tit = m_replayCtx.agents.find(last->targetId);
                if (tit != m_replayCtx.agents.end() && tit->second.healthPctAtTime(rechargeStart) < 0.5f)
                    rechargeDur += 20.f;
            }
        }
        if (rechargeStart >= 0.f && lastBoost > rechargeStart) { spent = false; rechargeDur = 0.f; }
        if (resets && rechargeStart >= 0.f) {
            for (const auto& r : *resets) {
                if (r.time <= rechargeStart || r.time > t) continue;
                const bool hits = r.onlySkill ? r.onlySkill == sid : r.exceptSkill != sid;
                if (hits) { spent = false; rechargeDur = 0.f; }
            }
        }

        float mult = 1.f;
        for (const auto& m : mods) {
            if (!modApplies(m, si)) continue;
            mult *= m.mult;
            if (!cd.modifierText.empty()) cd.modifierText += " + ";
            char buf[32];
            snprintf(buf, sizeof(buf), "%s x%.2f", m.name, m.mult);
            cd.modifierText += buf;
        }
        cd.rechargeMult = mult;
        if (rechargeDur > 0.f) {
            cd.rechargeTotal = rechargeDur * mult;
            cd.rechargeRemaining = std::max(0.f, rechargeStart + cd.rechargeTotal - t);
        }

        // --- Disables: the longest one running now, untouched by recharge modifiers ---
        if (disables) {
            for (const auto& d : *disables) {
                if (d.start > t || d.end <= t || d.start < lastBoost) continue;
                if (!Covers(d, sid, si)) continue;
                const float rem = d.end - t;
                if (rem > cd.disableRemaining) {
                    cd.disableRemaining   = rem;
                    cd.disableTotal       = d.end - d.start;
                    cd.disableSourceSkill = d.sourceSkill;
                    cd.disableSourceAgent = d.sourceAgent;
                }
            }
        }

        if (spent) {
            cd.state = SkillCooldownState::State::Spent;
        } else if (cd.disableRemaining > cd.rechargeRemaining) {
            cd.state = SkillCooldownState::State::Disabled;
            cd.remaining = cd.disableRemaining;
            cd.total = cd.disableTotal;
        } else if (cd.rechargeRemaining > 0.f) {
            cd.state = SkillCooldownState::State::Recharging;
            cd.remaining = cd.rechargeRemaining;
            cd.total = cd.rechargeTotal;
        }
        out.push_back(std::move(cd));
    }
    return out;
}

std::vector<int> ReplayWindow::SkillBarDisplayOrder(int agentId) const
{
    auto it = m_replayCtx.agents.find(agentId);
    if (it == m_replayCtx.agents.end()) return {};
    const AgentReplayData& ard = it->second;
    const auto& sdb = m_skillView;

    std::vector<int> ids;
    std::unordered_set<int> placed;
    for (int sid : SkillBarForAgent(agentId)) {
        const int r = sdb.ResolvePvpSkillId(sid);
        if (r > 0 && placed.insert(r).second) ids.push_back(r);
    }
    // History is in start order, so the first sighting of a skill comes first.
    for (const auto& ev : ard.skillUseHistory) {
        const int r = sdb.ResolvePvpSkillId(ev.skillId);
        if (r > 0 && placed.insert(r).second) ids.push_back(r);
    }
    if (ids.size() > 1)
        ids = sdb.SortSkillsForDisplay(ids, ard.primaryProf, ard.secondaryProf);
    return ids;
}
