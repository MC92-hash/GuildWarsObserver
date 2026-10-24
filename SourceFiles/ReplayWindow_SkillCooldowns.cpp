#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "SkillDatabase.h"
#include "EffectTimeline.h"
#include "InterruptCauses.h"
#include "DiversionTrigger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>

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
    // Arcane Mimicry (it swaps the slot for another skill). Echo and Arcane Echo swap theirs too:
    // which copy a slot holds, and which uses went to it, is EchoSlots' (private).

    // Skills that lock themselves longer for each effect they removed, counted on the effect bar's
    // timeline. Shatter Storm is disabled 7 s per enchantment (a disable: no recharge modifier
    // touches it); Withdraw Hexes recharges 2 s longer per hex. Audited 2026-10-11 on 125 Shatter
    // Storms followed by a recast: six recasts came within 1 s of the predicted end (one at
    // 0.07 s), and none came earlier once a morale boost (it clears disables, see below) and the
    // effect timeline's Reversal of Fortune end were accounted for. The caster's next attempt at the skill proves it was ready, so the lock never outlasts it.
    constexpr int kShatterStorm = 933, kWithdrawHexes = 942;
    constexpr float kShatterStormPerEnchantment = 7.f, kWithdrawHexesPerHex = 2.f;

    // The effects of `kind` this use of `skill` took: removed by it, from this caster, as it landed.
    int RemovedByUse(const EffectTimeline::Table* fx, const SkillDatabaseView& sdb, int casterId,
                     const SkillUseEvent& ev, int skill, EffectTimeline::Kind kind)
    {
        if (!fx) return 0;
        const float landed = ev.isInstant ? ev.startTime : ev.endTime;
        int n = 0;
        for (const auto& [agentId, list] : fx->byAgent)
            for (const EffectTimeline::Effect& e : list)
                if (e.kind == kind && e.removedByCaster == casterId && sdb.ResolvePvpSkillId(e.removedBy) == skill &&
                    e.end >= ev.startTime - 0.05f && e.end <= landed + 1.f)
                    ++n;
        return n;
    }

    // When the caster next tried the skill after this use, or -1.
    float NextAttempt(const AgentReplayData& caster, size_t index, const SkillDatabaseView& sdb)
    {
        const auto& h = caster.skillUseHistory;
        const int sid = sdb.ResolvePvpSkillId(h[index].skillId);
        const float landed = h[index].isInstant ? h[index].startTime : h[index].endTime;
        for (size_t j = index + 1; j < h.size(); ++j)
            if (sdb.ResolvePvpSkillId(h[j].skillId) == sid && h[j].startTime > landed) return h[j].startTime;
        return -1.f;
    }

    constexpr int kAuspiciousIncantation = 930;
    constexpr int kOathShot = 405, kNeedlingShot = 1197, kWordOfCensure = 1129;
    // Keystone Signet: "All of your signets except Keystone Signet are recharged." Measured
    // 2026-10-16 over the library (5 casters): 151 signets came back before their recharge, every
    // one with a Keystone landing in between. The wiki adds that it brings back a Resurrection
    // Signet only when something else disabled it, never after its use.
    constexpr int kKeystoneSignet = 63;
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
    if (it != m_attrProfiles.end())
        if (const AttributeModel::AttributeRange* a = AttributeModel::SolvedRange(&it->second, attribute))
            return a->best;
    return kFallbackRank;
}

// The ranks a skill read for its user at `usedAt`. `usedAt` is the instant the skill landed: which
// attribute it reads can depend on what else its user had up then, and the attribute model says
// which (AttributeModel::SkillAttributeAt). The solved range where the solve observed the attribute
// (`best` its answer); elsewhere the budget's bounds, or 0..16, with `best` the fallback rank.
// Weakness takes 1 off every rank while it lasts (the effect bar's timeline); an attribute boost
// (Glyph of Elemental Power's +2 to the elemental attributes) adds its amount, the larger one to
// `hi` where the recording cannot tell whether it was still up.
ReplayWindow::RankBand ReplayWindow::SkillRankBandAt(int skillId, int userAgentId, float usedAt) const
{
    RankBand band;
    const SkillInfo* si = m_skillView.Get(skillId);
    if (!si) return band;
    auto bit = m_attrProfiles.find(userAgentId);
    const int attribute = AttributeModel::SkillAttributeAt(
        bit != m_attrProfiles.end() ? &bit->second : nullptr, *si, usedAt);
    band.best = AttributeRankFor(userAgentId, attribute);
    if (bit != m_attrProfiles.end()) {
        if (const AttributeModel::AttributeRange* s = AttributeModel::SolvedRange(&bit->second, attribute)) {
            band.lo = s->lo;
            band.hi = s->hi;
            band.solved = true;
        } else if (auto a = bit->second.attributes.find(attribute); a != bit->second.attributes.end()) {
            band.lo = a->second.lo;
            band.hi = a->second.hi;
        }
    }
    band.lo = std::min(band.lo, band.best);
    band.hi = std::max(band.hi, band.best);
    if (const EffectTimeline::Table* fx = EffectTimelineTable(); fx && fx->WeaknessAt(userAgentId, usedAt) == 1) {
        band.lo = std::max(0, band.lo - 1);
        band.hi = std::max(0, band.hi - 1);
        band.best = std::max(0, band.best - 1);
    }
    if (m_attrBoosts) {
        static thread_local AttributeModel::Inputs in;
        in.boosts = m_attrBoosts.get();
        int boostLo = 0, boostHi = 0;
        if (AttributeModel::AttributeBoostAt(in, userAgentId, attribute, usedAt, boostLo, boostHi)) {
            band.lo += boostLo;
            band.best += boostLo;
            band.hi += boostHi;
        }
    }
    return band;
}

// The duration a skill grants at its user's rank: the index-th "A...B second" range of its
// description, or `fixed` when the rule names one.
float ReplayWindow::SkillSecondsAtRank(int skillId, int userAgentId, int rangeIndex, float fixed,
                                       float usedAt) const
{
    if (rangeIndex < 0) return fixed;
    const SkillInfo* si = m_skillView.Get(skillId);
    float v0 = 0.f, v15 = 0.f;
    if (!si || !DescRange(si->description, rangeIndex, v0, v15)) return fixed;
    return AtRank(v0, v15, SkillRankBandAt(skillId, userAgentId, usedAt).best);
}

// The same range read at the two ends of the rank band. False when the description has no such range.
bool ReplayWindow::SkillSecondsBand(int skillId, int userAgentId, int rangeIndex, float usedAt,
                                    float& lo, float& hi) const
{
    const SkillInfo* si = m_skillView.Get(skillId);
    float v0 = 0.f, v15 = 0.f;
    if (rangeIndex < 0 || !si || !DescRange(si->description, rangeIndex, v0, v15)) return false;
    const RankBand band = SkillRankBandAt(skillId, userAgentId, usedAt);
    lo = AtRank(v0, v15, band.lo);
    hi = AtRank(v0, v15, band.hi);
    if (lo > hi) std::swap(lo, hi);
    return true;
}

// The solved range a skill's description reads for its user at `usedAt`, and which attribute that
// is: the skill's own, or the one the attribute model says it scaled on then (SkillAttributeAt).
// nullptr when the tool never solved that attribute for this player.
const AttributeModel::AttributeRange* ReplayWindow::SkillRankAt(int skillId, int userAgentId, float usedAt,
                                                                int* attribute) const
{
    const SkillInfo* si = m_skillView.Get(skillId);
    if (attribute) *attribute = si ? si->attribute : -1;
    auto bit = m_attrProfiles.find(userAgentId);
    if (!si || bit == m_attrProfiles.end()) return nullptr;
    const int used = AttributeModel::SkillAttributeAt(&bit->second, *si, usedAt);
    if (attribute) *attribute = used;
    return AttributeModel::SolvedRange(&bit->second, used);
}

// Developer dump (GWO_ATTR_DEBUG): for every player whose skills change attribute over the match,
// the rank and the tooltip's green numbers of each such skill around every span the model gives.
void ReplayWindow::WriteSkillRankDebug(const std::string& path) const
{
    std::ofstream out(path);
    if (!out) return;
    for (const auto& [agentId, build] : m_attrProfiles) {
        if (build.symbolicCelerity.empty()) continue;
        std::vector<float> times;
        for (const auto& s : build.symbolicCelerity) {
            out << std::format("agent {} span {:.2f} .. {:.2f} (sure {:.2f}, maybe {:.2f})\n",
                               agentId, s.start, s.estimate, s.sure, s.maybe);
            for (float t : { s.start - 5.f, s.start + 0.5f, 0.5f * (s.start + s.estimate),
                             s.estimate - 0.5f, s.estimate + 5.f })
                times.push_back(t);
        }
        for (int skillId : SkillBarForAgent(agentId)) {
            const SkillInfo* si = m_skillView.Get(skillId);
            if (!si) continue;
            bool moves = false;
            for (float t : times) {
                int a = si->attribute;
                (void)SkillRankAt(skillId, agentId, t, &a);
                moves |= a != si->attribute;
            }
            if (!moves) continue;
            out << "  " << si->name << "\n";
            for (float t : times) {
                int a = si->attribute;
                const AttributeModel::AttributeRange* r = SkillRankAt(skillId, agentId, t, &a);
                const char* name = SkillDatabase::GetAttributeName(a);
                std::string text;
                for (const auto& run : BuildSkillTextRuns(*si, r, 0u, 1u))
                    text += run.colour == 1u ? "[" + run.text + "]" : run.text;
                for (char& c : text) if (c == '\n') c = ' ';
                out << std::format("    t {:8.2f}  {} {}  {}\n", t, name ? name : "?",
                                   r ? (r->lo == r->hi ? std::to_string(r->lo)
                                                       : std::format("{}-{}", r->lo, r->hi)) : "unsolved",
                                   text);
            }
        }
    }
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

    // Echo / Arcane Echo: which uses went to the copy in their slot (EchoSlots, private).
    m_echoSlots.clear();
    {
        const EffectTimeline::Table* fx = EffectTimelineTable();
        for (const auto& [id, ard] : m_replayCtx.agents) {
            if (ard.type != AgentType::Player) continue;
            EchoSlots::Agent a = EchoSlots::Build(id, ard, sdb, fx);
            if (!a.windows.empty()) m_echoSlots.emplace(id, std::move(a));
        }
    }
    // The caster's next try at the skill of use `index` on the same slot (an Echo copy and the
    // original are two slots), or -1.
    auto nextOnSlot = [&](int agentId, const AgentReplayData& ard, size_t index) {
        auto eit = m_echoSlots.find(agentId);
        if (eit == m_echoSlots.end()) return NextAttempt(ard, index, sdb);
        const auto& h = ard.skillUseHistory;
        const int sid = resolve(h[index].skillId), slot = eit->second.copyOf[index];
        for (size_t j = index + 1; j < h.size(); ++j)
            if (resolve(h[j].skillId) == sid && eit->second.copyOf[j] == slot && h[j].startTime > CastEnd(h[index]))
                return h[j].startTime;
        return -1.f;
    };

    // Which use caused each interrupt: only that one applies its "if it interrupts" effect.
    const InterruptCauses::Table causes = InterruptCauses::Build(m_replayCtx.agents, m_replayCtx.stocData, sdb);

    // Arcane Thievery / Larceny / Simple Thievery: what each steal took, and which of the thief's
    // uses went to it (ThieverySlots, private). The target's spell is disabled for the steal's
    // full length; Simple Thievery's lock is its interrupt rule's (kInterruptRules).
    {
        ThieverySlots::Inputs ti;
        ti.agents = &m_replayCtx.agents;
        ti.sdb = &sdb;
        ti.causes = &causes;
        ti.bar = [&](int agent) { return SkillBarForAgent(agent); };
        ti.seconds = [&](int agent, int sid, float at) { return SkillSecondsAtRank(sid, agent, 0, 0.f, at); };
        m_thievery = ThieverySlots::Build(ti);
        for (const ThieverySlots::Steal& s : m_thievery.steals) {
            if (s.how == ThieverySlots::How::Interrupt || s.stolen <= 0) continue;
            SkillDisable d;
            d.start = s.start;
            d.end = s.targetEnd;
            d.ruleEnd = d.end;
            float lo = 0.f, hi = 0.f;
            if (SkillSecondsBand(s.thiefSkill, s.thief, 0, s.start, lo, hi) && hi > lo) {
                d.endLo = s.start + lo;
                d.endHi = s.start + hi;
            }
            d.scope = DisableScope::Skill; d.skillId = s.stolen;
            d.sourceSkill = s.thiefSkill; d.sourceAgent = s.thief;
            add(s.target, d);
        }
    }

    for (const auto& [casterId, caster] : m_replayCtx.agents) {
        if (caster.type != AgentType::Player) continue;

        for (size_t ei = 0; ei < caster.skillUseHistory.size(); ++ei) {
            const SkillUseEvent& ev = caster.skillUseHistory[ei];
            if (!Succeeded(ev)) continue;
            const int   rid = resolve(ev.skillId);
            const float t0  = CastEnd(ev);

            // A. Interrupts. The victim's cast that was cut short is the one this use interrupted
            // (InterruptCauses); a use that interrupted nothing disables nothing.
            const InterruptCauses::Cause* cause = causes.ForUse(casterId, static_cast<int>(ei));
            for (const auto& rule : kInterruptRules) {
                if (resolve(rule.skillId) != rid) continue;
                if (!cause || cause->victimUse < 0) continue;
                if (rule.casterEnchanted) {
                    const AgentSnapshot* snap = FindSnapshotAtTime(caster, t0);
                    if (!snap || !snap->has_enchantment) continue;
                }
                auto vit = m_replayCtx.agents.find(cause->victim);
                if (vit == m_replayCtx.agents.end()) continue;
                const AgentReplayData& victim = vit->second;
                const SkillUseEvent* cut = &victim.skillUseHistory[cause->victimUse];

                const SkillInfo* cutInfo = sdb.Get(cut->skillId);
                if (rule.needs == Needs::SpellOrChant &&
                    !(cutInfo && (SkillDatabase::IsSpellType(cutInfo->type) || cutInfo->type == kTypeChant))) continue;
                if (rule.needs == Needs::Spell && !(cutInfo && SkillDatabase::IsSpellType(cutInfo->type))) continue;
                if (rule.needs == Needs::Attack && !IsAttack(cutInfo)) continue;

                float secs = SkillSecondsAtRank(rule.skillId, casterId, rule.rangeIndex, rule.fixed, t0);
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
                d.victimUse = cause->victimUse;
                if (rule.scope == DisableScope::Attribute) {
                    // Power Block: the skill, and every skill of its attribute. A skill with no
                    // attribute has no siblings.
                    if (!cutInfo || cutInfo->attribute >= 101) d.scope = DisableScope::Skill;
                    else d.param = cutInfo->attribute;
                }
                add(vit->first, d);

                if (rule.area) {
                    d.victimUse = -1;   // the others' copies of the skill, whichever slot
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
                    start += SkillSecondsAtRank(rule.skillId, casterId, rule.delayIndex, 0.f, t0);
                SkillDisable d;
                d.start = start;
                d.end   = start + SkillSecondsAtRank(rule.skillId, casterId, rule.rangeIndex, rule.fixed, t0);
                d.scope = rule.scope;
                d.skillId = rid;
                d.param = rule.param;
                d.sourceSkill = ev.skillId; d.sourceAgent = casterId;
                add(victim, d);
            }

            // Shatter Storm: disabled 7 s for each enchantment it removed.
            if (rid == kShatterStorm) {
                const int n = RemovedByUse(EffectTimelineTable(), sdb, casterId, ev, kShatterStorm,
                                           EffectTimeline::Kind::Enchantment);
                SkillDisable d;
                d.start = t0;
                d.end   = t0 + kShatterStormPerEnchantment * n;
                if (const float next = nextOnSlot(casterId, caster, ei); next >= 0.f) d.end = std::min(d.end, next);
                d.scope = DisableScope::Skill; d.skillId = rid;
                d.sourceSkill = ev.skillId; d.sourceAgent = casterId;
                d.victimUse = static_cast<int>(ei);
                add(casterId, d);
            }

            // Auspicious Incantation: the next spell its user casts within 20 seconds is disabled
            // for an additional X seconds.
            if (rid == resolve(kAuspiciousIncantation)) {
                const float secs = SkillSecondsAtRank(ev.skillId, casterId, 0, 0.f, t0);
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
                    d.victimUse = static_cast<int>(j);
                    add(casterId, d);
                    break;
                }
            }

            // Diversion: for 6 seconds, the next skill its target uses takes an additional X seconds
            // to recharge, X at the caster's Domination Magic. Which use it lands on, and when it was
            // taken off first, is DiversionTrigger's (private); the lock is R + X from that use's
            // completion. X is read at the caster's rank as it landed (Weakness, boosts); where the
            // solve left a band, its two ends ride along for the tooltip. The victim's next try at
            // the skill proves it was ready, so the lock shown never outlasts it.
            if (rid == resolve(DiversionTrigger::kDiversion) && ev.targetId > 0 && ev.targetId != casterId) {
                const DiversionTrigger::Result dv = DiversionTrigger::Find(m_replayCtx.agents, sdb, ev.targetId, t0);
                if (dv.Locks()) {
                    const SkillInfo* vi = sdb.Get(dv.trigger->skillId);
                    const float recharge = kAdditionalAddsToRecharge && vi ? vi->recharge : 0.f;
                    float lo = 0.f, hi = 0.f;
                    SkillDisable d;
                    d.start = dv.done;
                    d.end   = dv.done + SkillSecondsAtRank(ev.skillId, casterId, 0, 0.f, t0) + recharge;
                    d.ruleEnd = d.end;
                    if (SkillSecondsBand(ev.skillId, casterId, 0, t0, lo, hi) && hi > lo) {
                        d.endLo = dv.done + lo + recharge;
                        d.endHi = dv.done + hi + recharge;
                    }
                    const AgentReplayData& victim = m_replayCtx.agents.at(ev.targetId);
                    d.victimUse = static_cast<int>(dv.trigger - victim.skillUseHistory.data());
                    // The victim's next try on the same slot (an Echo copy of it is another slot).
                    const float next = m_echoSlots.count(ev.targetId)
                                     ? nextOnSlot(ev.targetId, victim, static_cast<size_t>(d.victimUse)) : dv.next;
                    if (next > 0.f) d.end = std::min(d.end, next);
                    d.scope = DisableScope::Skill; d.skillId = dv.skill;
                    d.sourceSkill = ev.skillId; d.sourceAgent = casterId;
                    add(ev.targetId, d);
                }
            }

            // Skills that recharge others. Oath Shot is read as a hit (it only misses half the
            // time at Expertise 7 or less, and a Ranger running it has more); Needling Shot
            // recharges itself when it strikes a foe below half health, which the recording shows.
            if (rid == resolve(kOathShot))
                m_rechargeResets[casterId].push_back({ t0, resolve(kOathShot), 0 });
            if (rid == resolve(kKeystoneSignet))
                m_rechargeResets[casterId].push_back({ t0, rid, 0, true, true });
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

// Developer dump (GWO_ATTR_DEBUG): every interrupt and the use that caused it, then every disable
// an interrupt laid, with the victim's first use of the disabled skill inside it - a contradiction
// unless a morale boost of his party came between.
void ReplayWindow::WriteInterruptDebug(const std::string& path) const
{
    if (!m_moraleTimelineBuilt) BuildMoraleTimelines();
    if (!m_skillDisablesBuilt || (!m_skillDisablesUsedAttributes && m_attributesDeduced))
        BuildSkillDisables();
    std::ofstream out(path);
    if (!out) return;

    const auto& sdb = m_skillView;
    auto resolve = [&](int sid) { return sdb.ResolvePvpSkillId(sid); };
    auto name = [&](int id) { const SkillInfo* si = sdb.Get(id); return si ? si->name : std::to_string(id); };
    const InterruptCauses::Table causes = InterruptCauses::Build(m_replayCtx.agents, m_replayCtx.stocData, sdb);
    for (const InterruptCauses::Cause& c : causes.causes) {
        std::string cut = "-";
        if (auto it = m_replayCtx.agents.find(c.victim); it != m_replayCtx.agents.end() && c.victimUse >= 0)
            cut = name(it->second.skillUseHistory[c.victimUse].skillId);
        out << std::format("cause\t{:.3f}\t{}\t{}\t{}\t{}\t{:+.3f}\n", c.time, c.victim, cut, c.caster,
                           name(c.skillId), c.landed - c.time);
    }

    // The first use of the skill inside the lock, and whether a morale boost explains it.
    auto check = [&](int victimId, const SkillDisable& d) -> std::string {
        auto vit = m_replayCtx.agents.find(victimId);
        if (vit == m_replayCtx.agents.end()) return "-";
        for (const auto& u : vit->second.skillUseHistory) {
            if (resolve(u.skillId) != d.skillId || u.startTime <= d.start + 0.05f) continue;
            if (u.startTime >= d.end - 0.25f) return std::format("next +{:.2f}", u.startTime - d.start);
            bool boost = false;
            if (auto bit = m_moraleBoosts.find(static_cast<int>(vit->second.teamId)); bit != m_moraleBoosts.end())
                for (float bt : bit->second) boost |= bt >= d.start && bt <= u.startTime;
            return std::format("{} +{:.2f}", boost ? "used-after-boost" : "USED-INSIDE", u.startTime - d.start);
        }
        return "never";
    };
    for (const auto& [victimId, list] : m_skillDisables)
        for (const SkillDisable& d : list) {
            bool interrupt = false;
            for (const auto& r : kInterruptRules) interrupt |= resolve(r.skillId) == resolve(d.sourceSkill);
            if (!interrupt) continue;
            out << std::format("disable\t{:.3f}\t{}\t{}\t{}\t{}\t{:.2f}\t{}\n", d.start, victimId, name(d.skillId),
                               d.sourceAgent, name(d.sourceSkill), d.end - d.start, check(victimId, d));
        }
}

// Developer dump (GWO_ATTR_DEBUG): %TEMP%\gwo_cooldowns_<match>.txt. The cooldown model against
// what the players did:
//   used  a use (a cast started, stopped ones too) while the model showed the slot disabled,
//         recharging or spent by more than 0.25 s - a contradiction;
//   fast  a skill back before its recharge (the replay fits that recharge to the next use, so the
//         bar shows it) and what explains it: a morale boost, a recharge reset (Keystone Signet,
//         Oath Shot, Needling Shot), a recharge modifier - or UNEXPLAINED;
//   lock  a disable whose rule end was later shortened by the victim's next use (Diversion): the
//         rule's length, its rank band, that next use, and EARLY when it came before the rule's end.
void ReplayWindow::WriteCooldownDebug(const std::string& path) const
{
    if (!m_moraleTimelineBuilt) BuildMoraleTimelines();
    if (!m_skillDisablesBuilt || (!m_skillDisablesUsedAttributes && m_attributesDeduced))
        BuildSkillDisables();
    std::ofstream out(path);
    if (!out) return;

    const auto& sdb = m_skillView;
    auto resolve = [&](int sid) { return sdb.ResolvePvpSkillId(sid); };
    auto name = [&](int id) { const SkillInfo* si = sdb.Get(id); return si ? si->name : std::to_string(id); };
    auto boostBetween = [&](const AgentReplayData& ard, float a, float b) {
        if (auto bit = m_moraleBoosts.find(static_cast<int>(ard.teamId)); bit != m_moraleBoosts.end())
            for (float bt : bit->second) if (bt > a && bt <= b) return true;
        return false;
    };

    std::vector<int> ids;
    for (const auto& [id, ard] : m_replayCtx.agents) if (ard.type == AgentType::Player) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        const AgentReplayData& ard = m_replayCtx.agents.at(id);
        const auto& h = ard.skillUseHistory;
        const EchoSlots::Agent* echo = nullptr;
        if (auto eit = m_echoSlots.find(id); eit != m_echoSlots.end()) echo = &eit->second;
        if (echo)
            for (size_t w = 0; w < echo->windows.size(); ++w) {
                const EchoSlots::Window& ew = echo->windows[w];
                int onCopy = 0, onOriginal = 0;
                for (size_t i = 0; i < h.size(); ++i) {
                    if (resolve(h[i].skillId) != ew.copied || static_cast<int>(i) == ew.captureUse) continue;
                    if (echo->copyOf[i] == static_cast<int>(w)) ++onCopy;
                    else if (h[i].startTime >= ew.capture && h[i].startTime < ew.close) ++onOriginal;
                }
                out << std::format("echo\t{:.3f}\t{}\t{}\t{}\t{:.2f}\t{:.2f}\t{}\t{}\t{}\n", ew.open, id, name(ew.echoSkill),
                                   name(ew.copied), ew.capture - ew.open, ew.close - ew.capture, ew.closedBy,
                                   onCopy, onOriginal);
                // The uses around it, each with the slot it went to (c: the copy, s: its own).
                for (size_t i = 0; i < h.size(); ++i) {
                    if (h[i].startTime < ew.open - 3.f || h[i].startTime > ew.close + 15.f) continue;
                    out << std::format("echouse\t{:.3f}\t{}\t{:.3f}\t{}\t{}\t{}\n", ew.open, id, h[i].startTime,
                                       name(h[i].skillId), echo->copyOf[i] == static_cast<int>(w) ? "c" : "s",
                                       h[i].wasInterrupted ? "int" : h[i].wasCancelled ? "cancel" : "ok");
                }
            }
        for (size_t i = 0; i < h.size(); ++i) {
            const SkillUseEvent& u = h[i];
            // A use of an Echo copy is checked on the Echo slot, as the copy stood.
            int slotSkill = u.skillId;
            float at = u.startTime - 0.02f;
            if (echo && echo->OnCopy(i)) {
                const EchoSlots::Window& ew = echo->windows[echo->copyOf[i]];
                slotSkill = ew.echoSkill;
                at = std::max(at, ew.capture);
            }
            const SkillCooldownState cd = ComputeSkillCooldowns(ard, { slotSkill }, at).front();
            const char* state = cd.state == SkillCooldownState::State::Disabled   ? "DISABLED"
                              : cd.state == SkillCooldownState::State::Recharging ? "RECHARGING"
                              : cd.state == SkillCooldownState::State::Spent      ? "SPENT" : nullptr;
            if (state && (cd.remaining > 0.25f || cd.state == SkillCooldownState::State::Spent))
                out << std::format("used\t{:.3f}\t{}\t{}\t{}\t{:.2f}\t{}\t{}\n", u.startTime, id, name(u.skillId), state,
                                   cd.remaining, cd.disableSourceSkill ? name(cd.disableSourceSkill) : "-",
                                   cd.disableSourceAgent);

            // A fast recast, read per slot (an Echo copy's use is no early recast of the original).
            if (u.wasCancelled && !u.wasInterrupted) continue;
            const SkillInfo* si = sdb.Get(u.skillId);
            const float end = CastEnd(u), normal = si ? si->recharge : 0.f;
            const float recharge = echo ? echo->recharge[i] : u.rechargeDuration;
            if (!(normal > 0.f && recharge < normal - 0.5f)) continue;
            const float next = end + recharge;
            std::string why;
            if (boostBetween(ard, end, next + 0.05f)) why = "boost";
            if (why.empty()) {
                if (auto rit = m_rechargeResets.find(id); rit != m_rechargeResets.end())
                    for (const RechargeReset& r : rit->second) {
                        if (r.time <= end || r.time > next + 0.05f) continue;
                        const int sid = resolve(u.skillId);
                        if (r.signetsOnly && !(si && si->type == kTypeSignet)) continue;
                        if (r.onlySkill ? r.onlySkill == sid : r.exceptSkill != sid) { why = "reset"; break; }
                    }
            }
            if (why.empty()) {
                float mult = 1.f;
                std::string text;
                for (float at : { end + 0.01f, 0.5f * (end + next), next - 0.02f }) {
                    const SkillCooldownState m = ComputeSkillCooldowns(ard, { u.skillId }, at).front();
                    if (m.rechargeMult < mult) { mult = m.rechargeMult; text = m.modifierText; }
                }
                if (mult < 1.f && recharge >= normal * mult - 0.5f) why = "mod " + text;
            }
            out << std::format("fast\t{:.3f}\t{}\t{}\t{:.2f}\t{:.0f}\t{}\n", end, id, name(u.skillId),
                               recharge, normal, why.empty() ? "UNEXPLAINED" : why);
        }
    }

    // Thievery steals: what each took, how it was read, what elimination and the inference alone
    // would have said where the thief's cast named it, and whether the target used it meanwhile.
    auto howName = [](ThieverySlots::How h) {
        switch (h) {
        case ThieverySlots::How::Cast:          return "cast";
        case ThieverySlots::How::OnlyCandidate: return "only";
        case ThieverySlots::How::Inferred:      return "inferred";
        case ThieverySlots::How::Interrupt:     return "interrupt";
        default:                                return "unknown";
        }
    };
    auto nameOr = [&](int sid) { return sid > 0 ? name(sid) : std::string("?"); };
    for (size_t si = 0; si < m_thievery.steals.size(); ++si) {
        const ThieverySlots::Steal& s = m_thievery.steals[si];
        int uses = 0, targetUses = 0;
        const auto& th = m_replayCtx.agents.at(s.thief).skillUseHistory;
        for (size_t j = 0; j < th.size(); ++j) uses += m_thievery.StealOf(s.thief, j) == static_cast<int>(si);
        if (s.stolen > 0)
            for (const auto& v : m_replayCtx.agents.at(s.target).skillUseHistory)
                targetUses += resolve(v.skillId) == s.stolen && v.startTime > s.start + 0.05f && v.startTime < s.targetEnd;
        out << std::format("steal\t{:.3f}\t{}\t{}\t{}\t{:.2f}\t{:.2f}\t{}\t{}\t{}\t{}\t{}\t{}\n", s.start, s.thief,
                           name(s.thiefSkill), s.target, s.targetEnd - s.start, s.end - s.start, nameOr(s.stolen),
                           howName(s.how), nameOr(s.blind), howName(s.blindHow), uses, targetUses);
    }
    for (int id : ids)
        if (m_thievery.onlyStolen.count(id))
            out << std::format("bar\t{}\t{}\t{}\n", id, SkillBarForAgent(id).size(), SkillBarDisplayOrder(id).size());

    for (int id : ids) {
        auto dit = m_skillDisables.find(id);
        if (dit == m_skillDisables.end()) continue;
        const AgentReplayData& victim = m_replayCtx.agents.at(id);
        for (const SkillDisable& d : dit->second) {
            if (d.ruleEnd <= 0.f) continue;
            float next = -1.f;
            for (const auto& v : victim.skillUseHistory)
                if (resolve(v.skillId) == d.skillId && v.startTime > d.start + 0.05f) { next = v.startTime; break; }
            const bool boost = next > 0.f && boostBetween(victim, d.start, next);
            const char* verdict = next < 0.f ? "never" : next >= d.ruleEnd - 0.25f ? "ok" : boost ? "after-boost" : "EARLY";
            out << std::format("lock\t{:.3f}\t{}\t{}\t{}\t{}\t{:.2f}\t{:.2f}\t{:.2f}\t{}\t{}\n", d.start, id, name(d.skillId),
                               name(d.sourceSkill), d.sourceAgent, d.ruleEnd - d.start,
                               d.endHi > 0.f ? d.endLo - d.start : d.ruleEnd - d.start,
                               d.endHi > 0.f ? d.endHi - d.start : d.ruleEnd - d.start,
                               next > 0.f ? std::format("{:.2f}", next - d.start) : "-", verdict);
        }
    }
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
            return t < t0 + SkillSecondsAtRank(ev.skillId, ard.agent_id, rangeIndex, fallback, t0);
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

    // Echo / Arcane Echo: a copy in their slot is a slot of its own (EchoSlots, private).
    const EchoSlots::Agent* echo = nullptr;
    if (auto eit = m_echoSlots.find(ard.agent_id); eit != m_echoSlots.end()) echo = &eit->second;
    const auto& hist = ard.skillUseHistory;

    std::vector<SkillCooldownState> out;
    out.reserve(skillIds.size());
    for (int rawSid : skillIds) {
        SkillCooldownState cd;
        cd.skillId = rawSid;
        cd.shownSkillId = rawSid;
        // While Echo holds a copy the slot IS that skill, and only the uses that went to the copy
        // are its own; every other slot leaves those uses out.
        const int slotSid = resolve(rawSid);
        const int echoSkill = slotSid == resolve(EchoSlots::kEcho)       ? EchoSlots::kEcho
                            : slotSid == resolve(EchoSlots::kArcaneEcho) ? EchoSlots::kArcaneEcho : 0;
        const EchoSlots::Window* held = echo && echoSkill ? echo->HeldAt(echoSkill, t) : nullptr;
        const int heldIndex = held ? static_cast<int>(held - echo->windows.data()) : -1;
        if (held) cd.shownSkillId = held->copied;
        // A thievery holding a steal is the stolen skill the same way (ThieverySlots).
        const int thiefSkill = slotSid == resolve(ThieverySlots::kArcaneThievery) ? ThieverySlots::kArcaneThievery
                             : slotSid == resolve(ThieverySlots::kArcaneLarceny)  ? ThieverySlots::kArcaneLarceny
                             : slotSid == resolve(ThieverySlots::kSimpleThievery) ? ThieverySlots::kSimpleThievery : 0;
        const ThieverySlots::Steal* steal = thiefSkill ? m_thievery.HeldAt(ard.agent_id, thiefSkill, t) : nullptr;
        const int stealIndex = steal ? static_cast<int>(steal - m_thievery.steals.data()) : -1;
        if (steal) {
            std::string from = "?";
            if (auto vit = m_replayCtx.agents.find(steal->target); vit != m_replayCtx.agents.end()) from = vit->second.playerName;
            char buf[160];
            if (steal->stolen <= 0) {
                // Taken, but the recording does not say what: a neutral slot until it comes back.
                snprintf(buf, sizeof(buf), "Stolen from %s: unknown spell, %.0fs left", from.c_str(), std::ceil(steal->end - t));
                cd.shownSkillId = SkillCooldownState::kUnknownSkill;
                cd.slotNote = buf;
                out.push_back(std::move(cd));
                continue;
            }
            snprintf(buf, sizeof(buf), "Stolen from %s%s, %.0fs left", from.c_str(),
                     steal->how == ThieverySlots::How::Inferred ? " (inferred: the spell left unused)" : "",
                     std::ceil(steal->end - t));
            cd.shownSkillId = steal->stolen;
            cd.slotNote = buf;
        }
        const int sid = held ? held->copied : steal ? steal->stolen : slotSid;
        const SkillInfo* si = sdb.Get(sid);
        auto mine = [&](size_t i) {
            return (!echo || echo->copyOf[i] == heldIndex) && m_thievery.StealOf(ard.agent_id, i) == stealIndex;
        };

        // --- Recharge: the last use, cleared by a morale boost or a skill that recharges it ---
        float rechargeStart = -1.f, rechargeDur = 0.f;
        float rechargeCap = -1.f;   // the caster's next try at the skill, where one bounds the recharge
        bool spent = false;
        const SkillUseEvent* last = nullptr;
        size_t lastIndex = 0;
        for (size_t i = 0; i < hist.size(); ++i) {
            const SkillUseEvent& ev = hist[i];
            if (resolve(ev.skillId) != sid || !mine(i)) continue;
            if (!ev.isInstant && ev.startTime <= t && ev.endTime > t) cd.castingNow = true;
            // A cast the player let go of starts no recharge; an interrupted one (also recorded as
            // stopped) recharges in full, with any interrupt disable laid on top.
            if ((ev.wasCancelled && !ev.wasInterrupted) || ev.endTime > t) continue;
            last = &ev;
            lastIndex = i;
        }
        if (last) {
            rechargeStart = CastEnd(*last);
            if (sid == kResurrectionSignet) spent = true;
            else rechargeDur = echo ? echo->recharge[lastIndex] : last->rechargeDuration;
            // Echo back from a copy recharges from the copy's end, in full; so does a thievery back
            // from its steal (only Simple Thievery has a recharge to run).
            if (echoSkill && !held && echo)
                if (const EchoSlots::Window* w = echo->LastClosedBy(echoSkill, t);
                    w && w->echoUse == static_cast<int>(lastIndex)) {
                    rechargeStart = w->close;
                    rechargeDur = si ? si->recharge : 0.f;
                }
            if (thiefSkill && !steal)
                if (const ThieverySlots::Steal* s = m_thievery.LastEndedBy(ard.agent_id, thiefSkill, t);
                    s && s->use == static_cast<int>(lastIndex)) {
                    rechargeStart = s->end;
                    rechargeDur = si ? si->recharge : 0.f;
                }
            if (sid == resolve(kWordOfCensure) && !last->wasFastRecast) {
                auto tit = m_replayCtx.agents.find(last->targetId);
                if (tit != m_replayCtx.agents.end() && tit->second.healthPctAtTime(rechargeStart) < 0.5f)
                    rechargeDur += 20.f;
            }
            // Withdraw Hexes: 2 s more for each hex it removed, never past the caster's next try.
            if (sid == kWithdrawHexes && Succeeded(*last) && !last->wasFastRecast) {
                const size_t index = static_cast<size_t>(last - ard.skillUseHistory.data());
                rechargeDur += kWithdrawHexesPerHex * RemovedByUse(EffectTimelineTable(), sdb, ard.agent_id, *last,
                                                                   kWithdrawHexes, EffectTimeline::Kind::Hex);
                rechargeCap = NextAttempt(ard, index, sdb);
            }
        }
        if (rechargeStart >= 0.f && lastBoost > rechargeStart) { spent = false; rechargeDur = 0.f; }
        auto resetHits = [&](const RechargeReset& r) {
            if (r.signetsOnly && !(si && si->type == kTypeSignet)) return false;
            return r.onlySkill ? r.onlySkill == sid : r.exceptSkill != sid;
        };
        if (resets && rechargeStart >= 0.f) {
            for (const auto& r : *resets) {
                if (r.time <= rechargeStart || r.time > t || !resetHits(r)) continue;
                // A used Resurrection Signet comes back on a morale boost only (Keystone: wiki).
                if (r.signetsOnly && spent) continue;
                spent = false; rechargeDur = 0.f;
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
            if (rechargeCap > rechargeStart)
                cd.rechargeTotal = std::min(cd.rechargeTotal, rechargeCap - rechargeStart);
            cd.rechargeRemaining = std::max(0.f, rechargeStart + cd.rechargeTotal - t);
        }

        // --- Disables: the longest one running now, untouched by recharge modifiers ---
        if (disables) {
            for (const auto& d : *disables) {
                if (d.start > t || d.end <= t || d.start < lastBoost) continue;
                if (!Covers(d, sid, si)) continue;
                // A lock on one use of the skill holds the slot that use went to (Echo copies).
                if (d.scope == DisableScope::Skill && d.victimUse >= 0 &&
                    static_cast<size_t>(d.victimUse) < hist.size() && !mine(static_cast<size_t>(d.victimUse))) continue;
                // Recharged since by a skill that also ends disables (Keystone Signet).
                bool cleared = false;
                if (resets)
                    for (const auto& r : *resets)
                        cleared |= r.clearsDisables && r.time > d.start && r.time <= t && resetHits(r);
                if (cleared) continue;
                const float rem = d.end - t;
                if (rem > cd.disableRemaining) {
                    cd.disableRemaining   = rem;
                    cd.disableTotal       = d.end - d.start;
                    cd.disableSourceSkill = d.sourceSkill;
                    cd.disableSourceAgent = d.sourceAgent;
                    // The band never runs past the end shown (the victim's next use proved it over).
                    cd.disableRemainingLo = d.endHi > 0.f ? std::max(0.f, std::min(d.endLo, d.end) - t) : rem;
                    cd.disableRemainingHi = d.endHi > 0.f ? std::max(0.f, std::min(d.endHi, d.end) - t) : rem;
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

    // A skill the player only ever cast as a thievery's steal is not on his bar.
    if (!m_skillDisablesBuilt || (!m_skillDisablesUsedAttributes && m_attributesDeduced))
        BuildSkillDisables();
    std::vector<int> ids;
    std::unordered_set<int> placed;
    for (int sid : SkillBarForAgent(agentId)) {
        const int r = sdb.ResolvePvpSkillId(sid);
        if (r > 0 && !m_thievery.OnlyStolen(agentId, r) && placed.insert(r).second) ids.push_back(r);
    }
    // History is in start order, so the first sighting of a skill comes first.
    for (const auto& ev : ard.skillUseHistory) {
        const int r = sdb.ResolvePvpSkillId(ev.skillId);
        if (r > 0 && !m_thievery.OnlyStolen(agentId, r) && placed.insert(r).second) ids.push_back(r);
    }
    if (ids.size() > 1)
        ids = sdb.SortSkillsForDisplay(ids, ard.primaryProf, ard.secondaryProf);
    return ids;
}
