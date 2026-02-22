#pragma once
#include <string>
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
};

class SkillDatabase
{
public:
    bool Load(const std::string& dataDir);
    const SkillInfo* Get(int skillId) const;
    bool IsLoaded() const { return m_loaded; }

    static const char* GetTypeName(int typeId);
    static const char* GetAttributeName(int attrId);

private:
    std::unordered_map<int, SkillInfo> m_skills;
    bool m_loaded = false;
};

SkillDatabase& GetSkillDatabase();
