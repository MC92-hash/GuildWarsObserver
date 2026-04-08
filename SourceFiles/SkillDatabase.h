#pragma once
#include <string>
#include <vector>
#include <unordered_map>

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

private:
    std::unordered_map<int, SkillInfo> m_skills;
    bool m_loaded = false;
};

SkillDatabase& GetSkillDatabase();
