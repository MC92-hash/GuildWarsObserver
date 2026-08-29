#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

// Energy = the caster gains Energy, EnergyLoss = a foe loses it. They are separate kinds
// because the two arrive on opposite sides of the energy stream: a gain moves the caster's own
// bar, a loss moves the victim's, and only the sign of the sample tells them apart.
enum class SkillScaleKind : uint8_t
{
    None, Damage, Heal, LifeSteal, LifeLoss, Duration, Energy, EnergyLoss,
    Level,  // "Create a level 1...10 spirit", "animate a level 1...17 bone horror"
    Cap     // "(maximum 30...120 holy damage)" - a ceiling on a payout, never the payout
};

struct SkillScale
{
    SkillScaleKind kind = SkillScaleKind::None;
    float v0 = 0;
    float v15 = 0;
    float multiplier = 1.f;
};

struct SkillInfo
{
    int id = 0;
    std::string name;
    std::string description;
    std::string concise;

    int profession = 0;
    int attribute = 0;
    int type = 0;
    int campaign = 0;
    bool is_elite = false;

    int energy = 0;
    float activation = 0;
    float recharge = 0;
    int adrenaline = 0;
    int sacrifice = 0;
    int upkeep = 0;
    int overcast = 0;

    bool is_pvp = false;
    bool pvp_split = false;
    int  split_id = 0;

    std::vector<SkillScale> scales;

    // Auto-derived attribute-deduction classification (filled by
    // ClassifyDeductionUsability, run right after ParseScalesFromDescription).
    bool deductionUsable = false;              // safe to use for rank deduction
    bool dfConfounded = false;                 // single-target Monk heal (Divine Favor bonus applies)
    SkillScaleKind deductionKind = SkillScaleKind::None;
    float dedV0 = 0;                           // usable scale endpoints (rank 0 / rank 15)
    float dedV15 = 0;

    // A second Heal scale the first one can be added to. Word of Healing pays 5...100, or that
    // plus 30...115 when the target is below half health, and nothing in the recording says
    // which - so the observation is matched against bp1(r) OR bp1(r) + bp2(r) and the rank set
    // carries the ambiguity instead of the skill being thrown away for having two scales.
    bool  dedTwoScale = false;
    float dedV0b = 0;
    float dedV15b = 0;

    // bp1 + n * bp2 rather than bp1 or bp1 + bp2. Wastrel's Demise pays its scale once per
    // second and adds another copy of the second scale for every second the hex has run, so
    // n is 0..4 and nothing in the recording says which second a tick came from.
    int   dedPairRepeat = 0;

    // The packet is a WHOLE MULTIPLE of the breakpoint: Restore Condition heals once per
    // condition it removed, Mending Touch once per condition of two, Feast of Souls once per
    // spirit destroyed. The count is in the description and never in the recording, so the
    // deducer tries k = 1..dedPerCount and takes the first that lands on a breakpoint. Trying
    // the smallest first makes the rule purely additive: a packet the single-multiple rule
    // already explains is read exactly as it was before.
    int   dedPerCount = 0;

    // Armour reduces this packet (it arrives as a damage type other than 55), so no single hit
    // measures the rank. The largest hit of the match is a FLOOR - a level 20 player has AL >= 60
    // on every piece, so the breakpoint is the most the skill can ever have paid.
    bool  dedArmourRespecting = false;

    // "(maximum X...Y)" - the ceiling on a payout that is otherwise a flat amount times a count
    // the recording cannot see. Read as the payout it is the model's worst false ruler (Signet
    // of Mystic Wrath pays 25 per enchantment and was being measured against its own cap table),
    // so it is parsed apart from the payout scales and yields a floor: observed <= cap(rank).
    bool  dedCap = false;
    float dedCapV0 = 0;
    float dedCapV15 = 0;

    // The same classification for the energy stream, kept apart from the health one because a
    // skill can be a clean ruler on both at once: Drain Enchantment scales its heal AND its
    // energy with Inspiration, and rejecting it for having two scales would throw away both.
    bool energyUsable = false;
    SkillScaleKind energyKind = SkillScaleKind::None;  // Energy or EnergyLoss
    float enV0 = 0;
    float enV15 = 0;

    // The energy arrives when the effect ENDS, not when the skill is cast (Signet of Recall,
    // Renewing Surge, Zealous Renewal). By then the recorder's 2 s attribution window has
    // closed, so the sample carries skill 0 and has to be matched against the caster's bar.
    bool energyDelayed = false;
};

// The printf format that states a cast or recharge time exactly.
//
// Guild Wars counts these in quarters of a second, and "%.1f" cannot hold a quarter: it renders
// 0.75 as "0.8" and 0.25 as "0.2". That is 267 of the 299 skills with a fractional time showing a
// number the game never uses. So pick the shortest format that loses nothing, whole seconds
// plain, halves to one decimal and quarters to two, rather than a fixed width that has to round.
inline const char* SkillTimeFormat(float seconds)
{
    if (seconds == (float)(int)seconds)               return "%.0f";
    if (seconds * 2.f == (float)(int)(seconds * 2.f)) return "%.1f";
    return "%.2f";
}

class SkillDatabaseView
{
public:
    SkillDatabaseView() = default;
    explicit SkillDatabaseView(std::shared_ptr<const std::unordered_map<int, SkillInfo>> data)
        : m_data(std::move(data)) {}

    const SkillInfo* Get(int skillId) const;
    bool IsLoaded() const { return m_data && !m_data->empty(); }

    template<typename Fn>
    void ForEachSkill(Fn&& fn) const {
        if (!m_data) return;
        for (const auto& [id, info] : *m_data)
            fn(info);
    }

    std::vector<int> SortSkillsForDisplay(const std::vector<int>& skillIds,
                                           int primaryProf, int secondaryProf) const;
    int ResolvePvpSkillId(int skillId) const;

private:
    std::shared_ptr<const std::unordered_map<int, SkillInfo>> m_data;
};

class SkillDatabase
{
public:
    bool Load(const std::string& dataDir);
    const SkillInfo* Get(int skillId) const;
    bool IsLoaded() const { return m_loaded; }

    template<typename Fn>
    void ForEachSkill(Fn&& fn) const { for (auto& [id, si] : m_skills) fn(si); }

    static const char* GetTypeName(int typeId);
    static const char* GetAttributeName(int attrId);

    static bool IsMartialProfession(int profId);
    static bool IsWeaponAttack(int typeId);
    static bool IsSpellType(int typeId);
    static bool IsEnchantmentType(int typeId);
    static bool IsResurrectionSkill(int skillId);
    static int  GetProfessionForAttribute(int attrId);

    std::vector<int> SortSkillsForDisplay(const std::vector<int>& skillIds,
                                          int primaryProf, int secondaryProf) const;

    int ResolvePvpSkillId(int skillId) const;

    // The reverse direction: the PvE original a PvP-only id was split from, or the id itself
    // when it is not the PvP half of anything. Only the PvE row carries the link (so Get can
    // follow it to answer as the PvP half), so walking back needs an index rather than a field.
    int ResolveBaseSkillId(int skillId) const;

    static void ParseScalesFromDescription(SkillInfo& si);
    static void ClassifyDeductionUsability(SkillInfo& si);

    void LoadPatches(const std::string& dataDir);
    SkillDatabaseView GetView(int year, int month, int day);
    SkillDatabaseView GetBaseView() const;

private:
    std::unordered_map<int, SkillInfo> m_skills;
    std::unordered_map<int, int> m_pvpToBase; // PvP-only skill id -> the PvE id it was split from
    bool m_loaded = false;

    struct SkillPatch {
        int dateKey = 0; // YYYYMMDD
        std::unordered_map<int, SkillInfo> overrides; // skill ID -> old SkillInfo values
        // Which fields each patch actually names. `overrides` is a whole SkillInfo
        // copied from the current data with those fields replaced, so without this
        // list there is no way to tell a patched value from an unpatched one -- and
        // applying the whole struct lets an older patch undo a newer one's changes
        // to a field it never mentioned. See GetView.
        std::unordered_map<int, std::vector<std::string>> fields; // skill ID -> patched field names
        std::unordered_map<int, std::pair<std::string, std::string>> descOverrides; // skill ID -> {old description, old concise}
    };
    std::vector<SkillPatch> m_patches; // sorted by dateKey ascending
    std::unordered_map<int, std::shared_ptr<const std::unordered_map<int, SkillInfo>>> m_viewCache;
    std::shared_ptr<const std::unordered_map<int, SkillInfo>> m_baseView;
};

SkillDatabase& GetSkillDatabase();
