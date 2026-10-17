#pragma once
#include <cstdint>
#include <string>
#include <vector>

// One skill slot's availability at a moment of the replay, as the game would show it.
//
// Two clocks run on a slot and the later one governs. RECHARGE starts at the skill's last use and
// is shortened by recharge modifiers (Quickening Zephyr, Serpent's Quickness, ...). A DISABLE is
// imposed from outside the recharge -- an interrupt that locks the skill, a skill that shuts off
// its user's other skills, a skill that locks itself -- and, per the game's own rule, recharge
// modifiers never touch it and two disables do not add up: the longest wins. A morale boost
// recharges everything and so clears both.
struct SkillCooldownState
{
    enum class State : uint8_t
    {
        Ready,
        Recharging,  // the recharge clock governs
        Disabled,    // a disable outlasts the recharge
        Spent,       // Resurrection Signet after its one use: back only on a morale boost
    };

    int   skillId = 0;
    int   shownSkillId = 0;            // what the slot shows: the copy while Echo / Arcane Echo holds one
    bool  castingNow = false;          // a cast on this slot is running at the moment asked
    State state   = State::Ready;

    // The governing countdown, for the sweep and the number.
    float remaining = 0.f;
    float total     = 0.f;

    // The recharge clock alone. rechargeTotal already carries the modifiers.
    float rechargeRemaining = 0.f;
    float rechargeTotal     = 0.f;
    float rechargeMult      = 1.f;
    std::string modifierText;          // "QZ x0.50 + SQ x0.67", empty when nothing applies

    // The longest disable alone.
    float disableRemaining   = 0.f;
    float disableTotal       = 0.f;
    int   disableSourceSkill = 0;      // the skill that imposed it
    int   disableSourceAgent = -1;     // who used that skill (may be the slot's owner)
    // Where the longest disable's length rests on a rank the solve did not pin (Diversion at the
    // caster's Domination Magic): the remaining time at the band's two ends. Equal when exact.
    float disableRemainingLo = 0.f;
    float disableRemainingHi = 0.f;
};

// What a disable covers on the victim's bar. Every scope but Skill is a filter over the bar.
enum class DisableScope : uint8_t
{
    Skill,              // exactly `skillId`
    All,
    AllExcept,          // every skill but `skillId`
    Attribute,          // every skill of attribute `param`
    Elite,
    Attacks,
    OtherAttacks,       // attack skills but `skillId`
    NonAttacks,
    OtherNonAttacks,    // non-attack skills but `skillId`
    Signets,
    Spells,
    StancesAndEnchantments,
    NonProfession,      // skills not of profession `param`
    NonProfessionNonSignet,
    OtherAttributeSkills, // skills of attribute `param` but `skillId`
    NonDaggerAttacks,
    NonSpearAttacks,
};

struct SkillDisable
{
    float start = 0.f;
    float end   = 0.f;
    DisableScope scope = DisableScope::Skill;
    int   skillId = 0;        // PvP-resolved
    int   param   = 0;
    int   sourceSkill = 0;
    int   sourceAgent = -1;
    // A length read off a rank band (see SkillCooldownState): the ends at its two edges, `end`
    // being the one shown. 0 when the length is exact.
    float endLo = 0.f;
    float endHi = 0.f;
    // The end the rule itself gives, before any later evidence shortened the one shown (the
    // victim's next use proves the skill ready). The cooldown dump checks the rule against it.
    float ruleEnd = 0.f;
    // The victim's use it lands on (an index into his skillUseHistory), -1 when it lands on no one
    // use. With an Echo copy of the skill on his bar, only that use's slot is locked.
    int   victimUse = -1;
};

// A skill that recharges others: Oath Shot recharges every skill but itself, Needling Shot
// recharges itself, Keystone Signet every signet but itself. Exactly one of the two ids is set.
struct RechargeReset
{
    float time = 0.f;
    int   exceptSkill = 0;   // every skill but this one (PvP-resolved)
    int   onlySkill   = 0;   // only this one (PvP-resolved)
    bool  signetsOnly = false;     // of those, only the signets
    bool  clearsDisables = false;  // it also ends the disables running on them then
};
