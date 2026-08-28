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
    None, Damage, Heal, LifeSteal, LifeLoss, Duration, Energy, EnergyLoss
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

    static void ParseScalesFromDescription(SkillInfo& si);
    static void ClassifyDeductionUsability(SkillInfo& si);

    void LoadPatches(const std::string& dataDir);
    SkillDatabaseView GetView(int year, int month, int day);
    SkillDatabaseView GetBaseView() const;

private:
    std::unordered_map<int, SkillInfo> m_skills;
    bool m_loaded = false;

    struct SkillPatch {
        int dateKey = 0; // YYYYMMDD
        std::unordered_map<int, SkillInfo> overrides; // skill ID -> old SkillInfo values
        std::unordered_map<int, std::pair<std::string, std::string>> descOverrides; // skill ID -> {old description, old concise}
    };
    std::vector<SkillPatch> m_patches; // sorted by dateKey ascending
    std::unordered_map<int, std::shared_ptr<const std::unordered_map<int, SkillInfo>>> m_viewCache;
    std::shared_ptr<const std::unordered_map<int, SkillInfo>> m_baseView;
};

SkillDatabase& GetSkillDatabase();
