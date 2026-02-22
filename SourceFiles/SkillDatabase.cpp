#include "pch.h"
#include "SkillDatabase.h"
#include <json.hpp>
#include <fstream>

using json = nlohmann::json;

static SkillDatabase g_skillDatabase;

SkillDatabase& GetSkillDatabase()
{
    return g_skillDatabase;
}

bool SkillDatabase::Load(const std::string& dataDir)
{
    std::string descPath = dataDir + "\\skilldesc-en.json";
    std::string dataPath = dataDir + "\\skilldata.json";

    // Load descriptions (name + description + concise)
    {
        std::ifstream f(descPath);
        if (!f.is_open()) return false;

        json j = json::parse(f, nullptr, false, true);
        if (j.is_discarded() || !j.contains("skilldesc")) return false;

        for (auto& [key, val] : j["skilldesc"].items())
        {
            int id = val.value("id", 0);
            SkillInfo& si = m_skills[id];
            si.id = id;
            si.name = val.value("name", std::string());
            si.description = val.value("description", std::string());
            si.concise = val.value("concise", std::string());
        }
    }

    // Load skill data (costs, type, profession, etc.)
    {
        std::ifstream f(dataPath);
        if (!f.is_open()) return false;

        json j = json::parse(f, nullptr, false, true);
        if (j.is_discarded() || !j.contains("skilldata")) return false;

        for (auto& [key, val] : j["skilldata"].items())
        {
            int id = val.value("id", 0);
            auto it = m_skills.find(id);
            if (it == m_skills.end()) continue;

            SkillInfo& si = it->second;
            si.profession = val.value("profession", 0);
            si.attribute = val.value("attribute", 0);
            si.type = val.value("type", 0);
            si.campaign = val.value("campaign", 0);
            si.is_elite = val.value("is_elite", false);
            si.energy = val.value("energy", 0);
            si.activation = val.value("activation", 0.0f);
            si.recharge = val.value("recharge", 0.0f);
            si.adrenaline = val.value("adrenaline", 0);
            si.sacrifice = val.value("sacrifice", 0);
            si.upkeep = val.value("upkeep", 0);
            si.overcast = val.value("overcast", 0);
        }
    }

    m_loaded = !m_skills.empty();
    return m_loaded;
}

const SkillInfo* SkillDatabase::Get(int skillId) const
{
    auto it = m_skills.find(skillId);
    if (it == m_skills.end()) return nullptr;
    return &it->second;
}

const char* SkillDatabase::GetTypeName(int typeId)
{
    switch (typeId)
    {
    case 0:  return "Not a Skill";
    case 1:  return "Skill";
    case 2:  return "Bow Attack";
    case 3:  return "Melee Attack";
    case 4:  return "Axe Attack";
    case 5:  return "Lead Attack";
    case 6:  return "Off-Hand Attack";
    case 7:  return "Dual Attack";
    case 8:  return "Hammer Attack";
    case 9:  return "Scythe Attack";
    case 10: return "Sword Attack";
    case 11: return "Pet Attack";
    case 12: return "Spear Attack";
    case 13: return "Chant";
    case 14: return "Echo";
    case 15: return "Form";
    case 16: return "Glyph";
    case 17: return "Preparation";
    case 18: return "Binding Ritual";
    case 19: return "Nature Ritual";
    case 20: return "Shout";
    case 21: return "Signet";
    case 22: return "Spell";
    case 23: return "Enchantment Spell";
    case 24: return "Hex Spell";
    case 25: return "Item Spell";
    case 26: return "Ward Spell";
    case 27: return "Weapon Spell";
    case 28: return "Well Spell";
    case 29: return "Stance";
    case 30: return "Trap";
    case 31: return "Ranged Attack";
    case 32: return "Ebon Vanguard Ritual";
    case 33: return "Flash Enchantment";
    case 34: return "Double Enchantment";
    default: return "Skill";
    }
}

const char* SkillDatabase::GetAttributeName(int attrId)
{
    switch (attrId)
    {
    // Mesmer
    case 0:  return "Fast Casting";
    case 1:  return "Illusion Magic";
    case 2:  return "Domination Magic";
    case 3:  return "Inspiration Magic";
    // Necromancer
    case 4:  return "Blood Magic";
    case 5:  return "Death Magic";
    case 6:  return "Soul Reaping";
    case 7:  return "Curses";
    // Elementalist
    case 8:  return "Air Magic";
    case 9:  return "Earth Magic";
    case 10: return "Fire Magic";
    case 11: return "Water Magic";
    case 12: return "Energy Storage";
    // Monk
    case 13: return "Healing Prayers";
    case 14: return "Smiting Prayers";
    case 15: return "Protection Prayers";
    case 16: return "Divine Favor";
    // Warrior
    case 17: return "Strength";
    case 18: return "Axe Mastery";
    case 19: return "Hammer Mastery";
    case 20: return "Swordsmanship";
    case 21: return "Tactics";
    // Ranger
    case 22: return "Beast Mastery";
    case 23: return "Expertise";
    case 24: return "Wilderness Survival";
    case 25: return "Marksmanship";
    // Assassin
    case 29: return "Dagger Mastery";
    case 30: return "Deadly Arts";
    case 31: return "Shadow Arts";
    // Ritualist
    case 32: return "Communing";
    case 33: return "Restoration Magic";
    case 34: return "Channeling Magic";
    // Assassin (primary)
    case 35: return "Critical Strikes";
    // Ritualist (primary)
    case 36: return "Spawning Power";
    // Paragon
    case 37: return "Spear Mastery";
    case 38: return "Command";
    case 39: return "Motivation";
    case 40: return "Leadership";
    // Dervish
    case 41: return "Scythe Mastery";
    case 42: return "Wind Prayers";
    case 43: return "Earth Prayers";
    case 44: return "Mysticism";
    // Special
    case 101: return "No Attribute";
    case 102: return "Sunspear";
    case 103: return "Lightbringer";
    case 104: return "Luxon";
    case 105: return "Kurzick";
    case 106: return "Asura";
    case 107: return "Deldrimor";
    case 108: return "Ebon Vanguard";
    case 109: return "Norn";
    default:  return "";
    }
}
