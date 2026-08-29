#include "pch.h"
#include "SkillDatabase.h"
#include <json.hpp>
#include <fstream>
#include <algorithm>
#include <unordered_set>
#include <tuple>
#include <filesystem>
#include <regex>
#include <cctype>
#include <cstring>

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
            si.is_pvp = val.value("is_pvp", false);
            si.pvp_split = val.value("pvp_split", false);
            si.split_id = val.value("split_id", 0);
        }
    }

    // Parse scales from skill descriptions and classify deduction usability.
    for (auto& [id, si] : m_skills)
    {
        ParseScalesFromDescription(si);
        ClassifyDeductionUsability(si);
    }

    // Index the splits the other way round for ResolveBaseSkillId.
    m_pvpToBase.clear();
    for (const auto& [id, si] : m_skills)
        if (si.pvp_split && si.split_id > 0)
            m_pvpToBase[si.split_id] = id;

    m_loaded = !m_skills.empty();
    if (m_loaded)
        m_baseView = std::make_shared<const std::unordered_map<int, SkillInfo>>(m_skills);

#ifdef _DEBUG
    if (m_loaded)
    {
        int total = 0, dmg = 0, heal = 0, steal = 0, loss = 0, dfConf = 0;
        for (const auto& [id, si] : m_skills)
        {
            if (!si.deductionUsable) continue;
            ++total;
            switch (si.deductionKind) {
            case SkillScaleKind::Damage:    ++dmg;   break;
            case SkillScaleKind::Heal:      ++heal;  break;
            case SkillScaleKind::LifeSteal: ++steal; break;
            case SkillScaleKind::LifeLoss:  ++loss;  break;
            default: break;
            }
            if (si.dfConfounded) ++dfConf;
        }
        char buf[256];
        snprintf(buf, sizeof(buf),
            "[AttrDeduce] usable=%d (Damage %d, Heal %d, LifeSteal %d, LifeLoss %d) dfConfounded=%d\n",
            total, dmg, heal, steal, loss, dfConf);
        OutputDebugStringA(buf);

        static const int kSpotIds[] = { 23, 69, 153, 156, 110, 3058, 1233, 1234,
                                        287, 3232, 281, 286, 229, 102, 42, 915 };
        for (int sid : kSpotIds)
        {
            auto it = m_skills.find(sid);
            if (it == m_skills.end()) continue;
            const SkillInfo& si = it->second;
            const char* kindName = "None";
            switch (si.deductionKind) {
            case SkillScaleKind::Damage:    kindName = "Damage";    break;
            case SkillScaleKind::Heal:      kindName = "Heal";      break;
            case SkillScaleKind::LifeSteal: kindName = "LifeSteal"; break;
            case SkillScaleKind::LifeLoss:  kindName = "LifeLoss";  break;
            default: break;
            }
            snprintf(buf, sizeof(buf),
                "[AttrDeduce]   id=%d %-24s usable=%d kind=%-9s v0=%.0f v15=%.0f df=%d\n",
                sid, si.name.c_str(), si.deductionUsable ? 1 : 0,
                kindName, si.dedV0, si.dedV15, si.dfConfounded ? 1 : 0);
            OutputDebugStringA(buf);
        }
    }
#endif

    return m_loaded;
}

// Answers as the PvP half too, for the same reason as the view above. This is the undated
// accessor, used where no replay date is in hand, so it reads the current split rather than the
// one that was live on a match day.
const SkillInfo* SkillDatabase::Get(int skillId) const
{
    auto it = m_skills.find(skillId);
    if (it == m_skills.end()) return nullptr;
    if (it->second.pvp_split && it->second.split_id > 0)
    {
        auto pvp = m_skills.find(it->second.split_id);
        if (pvp != m_skills.end()) return &pvp->second;
    }
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

bool SkillDatabase::IsMartialProfession(int profId)
{
    return profId == 1  // Warrior
        || profId == 2  // Ranger
        || profId == 7  // Assassin
        || profId == 9  // Paragon
        || profId == 10; // Dervish
}

bool SkillDatabase::IsWeaponAttack(int typeId)
{
    return (typeId >= 2 && typeId <= 12) || typeId == 31;
}

bool SkillDatabase::IsSpellType(int typeId)
{
    return (typeId >= 22 && typeId <= 28) || typeId == 33 || typeId == 34;
}

bool SkillDatabase::IsEnchantmentType(int typeId)
{
    return typeId == 23 || typeId == 33 || typeId == 34;
}

bool SkillDatabase::IsResurrectionSkill(int skillId)
{
    static const std::unordered_set<int> kResSkills = {
        2,    // Resurrection Signet
        1795, // Rebirth
        2917, // Flesh of My Flesh
        4399, // Resurrection Chant
        5413, // Death Pact Signet
        6841, // Sunspear Rebirth Signet
        8029, // Flesh of My Flesh (PvP)
        8059, // Death Pact Signet (PvP)
    };
    return kResSkills.count(skillId) > 0;
}

int SkillDatabase::GetProfessionForAttribute(int attrId)
{
    if (attrId >= 0  && attrId <= 3)  return 5;  // Mesmer
    if (attrId >= 4  && attrId <= 7)  return 4;  // Necromancer
    if (attrId >= 8  && attrId <= 12) return 6;  // Elementalist
    if (attrId >= 13 && attrId <= 16) return 3;  // Monk
    if (attrId >= 17 && attrId <= 21) return 1;  // Warrior
    if (attrId >= 22 && attrId <= 25) return 2;  // Ranger
    if (attrId >= 29 && attrId <= 31) return 7;  // Assassin
    if (attrId >= 32 && attrId <= 34) return 8;  // Ritualist
    if (attrId == 35)                 return 7;  // Assassin (Critical Strikes)
    if (attrId == 36)                 return 8;  // Ritualist (Spawning Power)
    if (attrId >= 37 && attrId <= 40) return 9;  // Paragon
    if (attrId >= 41 && attrId <= 44) return 10; // Dervish
    return 0; // No Attribute / special title tracks
}

std::vector<int> SkillDatabase::SortSkillsForDisplay(
    const std::vector<int>& skillIds, int primaryProf, int secondaryProf) const
{
    if (skillIds.size() <= 1) return skillIds;

    bool martial = IsMartialProfession(primaryProf);

    struct SortKey {
        int bucket       = 99;
        int typeOrder    = 0;   // weapon attack sub-type for chain ordering (Lead→Off-Hand→Dual)
        int attrGroup    = 999;
        int skillId      = 0;
        std::string name;
    };

    std::vector<std::pair<int, SortKey>> keyed;
    keyed.reserve(skillIds.size());

    for (int sid : skillIds)
    {
        SortKey k;
        k.skillId = sid;

        const SkillInfo* si = Get(sid);
        if (!si) { keyed.push_back({ sid, k }); continue; }

        k.name = si->name;
        int attrProf = GetProfessionForAttribute(si->attribute);
        bool isPrimary   = (attrProf == primaryProf) || (si->profession == primaryProf);
        bool isSecondary = (attrProf == secondaryProf) || (si->profession == secondaryProf);
        k.attrGroup = si->attribute;

        // Martial buckets:  0 Elite | 1 Weapon attacks | 2 Primary skills | 3 Stances |
        //                   4 Spells | 5 Other | 6 Secondary | 7 Res
        // Caster buckets:   0 Elite | 1 Primary (by attr) | 2 Other | 3 Secondary |
        //                   4 Stances | 5 Res

        if (IsResurrectionSkill(sid))
        {
            k.bucket = martial ? 7 : 5;
        }
        else if (si->is_elite)
        {
            k.bucket = 0;
        }
        else if (si->type == 29) // Stances always in the stance bucket regardless of profession
        {
            k.bucket = martial ? 3 : 4;
        }
        else if (isSecondary && !isPrimary)
        {
            k.bucket = martial ? 6 : 3;
        }
        else if (martial)
        {
            if (IsWeaponAttack(si->type))
            {
                k.bucket = 1;
                k.typeOrder = si->type; // Lead(5)→Off-Hand(6)→Dual(7) etc.
            }
            else if (isPrimary)
                k.bucket = 2;
            else if (IsSpellType(si->type))
                k.bucket = 4;
            else
                k.bucket = 5;
        }
        else // caster — all primary skills grouped by attribute, no separate enchantment bucket
        {
            if (isPrimary)
                k.bucket = 1;
            else
                k.bucket = 2;
        }

        keyed.push_back({ sid, k });
    }

    std::stable_sort(keyed.begin(), keyed.end(),
        [](const auto& a, const auto& b)
        {
            if (a.second.bucket != b.second.bucket)
                return a.second.bucket < b.second.bucket;
            if (a.second.typeOrder != b.second.typeOrder)
                return a.second.typeOrder < b.second.typeOrder;
            if (a.second.attrGroup != b.second.attrGroup)
                return a.second.attrGroup < b.second.attrGroup;
            if (a.second.skillId != b.second.skillId)
                return a.second.skillId < b.second.skillId;
            return a.second.name < b.second.name;
        });

    std::vector<int> result;
    result.reserve(keyed.size());
    for (auto& [sid, k] : keyed)
        result.push_back(sid);
    return result;
}

int SkillDatabase::ResolvePvpSkillId(int skillId) const
{
    auto it = m_skills.find(skillId);
    if (it == m_skills.end()) return skillId;
    const SkillInfo& si = it->second;
    if (si.pvp_split && si.split_id > 0)
        return si.split_id;
    return skillId;
}

// ---------------------------------------------------------------------------
// SkillDatabaseView
// ---------------------------------------------------------------------------

// A skill that has a PvP split answers as its PvP half.
//
// This tool only ever shows GvG, so the split is the skill as every match it loads actually
// played it. The two halves are not a rounding apart: PvE Aegis blocks attacks for the party
// while PvP Aegis makes one ally untargetable by spells, so serving the PvE entry is not an
// imprecise answer but the wrong skill.
//
// It resolves here rather than at each call site because the two id forms arrive from different
// places and only one of them was ever wrong. The StoC stream already names the PvP id, so casts,
// the combat log and the analytics were right; a player's bar in infos.json names the PvE id, and
// every place that reads that bar was showing PvE text for a PvP match. Putting the rule in the
// accessor makes it hold for readers nobody has thought of yet.
//
// Date correctness comes for free: this reads the view's own data, so a replay from before a split
// existed sees pvp_split false on that day's entry and keeps the only version there was. Callers
// that need the id rather than the entry still have ResolvePvpSkillId, and resolving twice is
// harmless because a PvP entry never carries a split of its own.
const SkillInfo* SkillDatabaseView::Get(int skillId) const
{
    if (!m_data) return nullptr;
    auto it = m_data->find(skillId);
    if (it == m_data->end()) return nullptr;
    if (it->second.pvp_split && it->second.split_id > 0)
    {
        auto pvp = m_data->find(it->second.split_id);
        if (pvp != m_data->end()) return &pvp->second;
    }
    return &it->second;
}

std::vector<int> SkillDatabaseView::SortSkillsForDisplay(
    const std::vector<int>& skillIds, int primaryProf, int secondaryProf) const
{
    if (skillIds.size() <= 1) return skillIds;

    bool martial = SkillDatabase::IsMartialProfession(primaryProf);

    struct SortKey {
        int bucket       = 99;
        int typeOrder    = 0;
        int attrGroup    = 999;
        int skillId      = 0;
        std::string name;
    };

    std::vector<std::pair<int, SortKey>> keyed;
    keyed.reserve(skillIds.size());

    for (int sid : skillIds)
    {
        SortKey k;
        k.skillId = sid;

        const SkillInfo* si = Get(sid);
        if (!si) { keyed.push_back({ sid, k }); continue; }

        k.name = si->name;
        int attrProf = SkillDatabase::GetProfessionForAttribute(si->attribute);
        bool isPrimary   = (attrProf == primaryProf) || (si->profession == primaryProf);
        bool isSecondary = (attrProf == secondaryProf) || (si->profession == secondaryProf);
        k.attrGroup = si->attribute;

        if (SkillDatabase::IsResurrectionSkill(sid))
        {
            k.bucket = martial ? 7 : 5;
        }
        else if (si->is_elite)
        {
            k.bucket = 0;
        }
        else if (si->type == 29)
        {
            k.bucket = martial ? 3 : 4;
        }
        else if (isSecondary && !isPrimary)
        {
            k.bucket = martial ? 6 : 3;
        }
        else if (martial)
        {
            if (SkillDatabase::IsWeaponAttack(si->type))
            {
                k.bucket = 1;
                k.typeOrder = si->type;
            }
            else if (isPrimary)
                k.bucket = 2;
            else if (SkillDatabase::IsSpellType(si->type))
                k.bucket = 4;
            else
                k.bucket = 5;
        }
        else
        {
            if (isPrimary)
                k.bucket = 1;
            else
                k.bucket = 2;
        }

        keyed.push_back({ sid, k });
    }

    std::stable_sort(keyed.begin(), keyed.end(),
        [](const auto& a, const auto& b)
        {
            if (a.second.bucket != b.second.bucket)
                return a.second.bucket < b.second.bucket;
            if (a.second.typeOrder != b.second.typeOrder)
                return a.second.typeOrder < b.second.typeOrder;
            if (a.second.attrGroup != b.second.attrGroup)
                return a.second.attrGroup < b.second.attrGroup;
            if (a.second.skillId != b.second.skillId)
                return a.second.skillId < b.second.skillId;
            return a.second.name < b.second.name;
        });

    std::vector<int> result;
    result.reserve(keyed.size());
    for (auto& [sid, k] : keyed)
        result.push_back(sid);
    return result;
}

int SkillDatabase::ResolveBaseSkillId(int skillId) const
{
    auto it = m_pvpToBase.find(skillId);
    return (it != m_pvpToBase.end()) ? it->second : skillId;
}

int SkillDatabaseView::ResolvePvpSkillId(int skillId) const
{
    if (!m_data) return skillId;
    auto it = m_data->find(skillId);
    if (it == m_data->end()) return skillId;
    const SkillInfo& si = it->second;
    if (si.pvp_split && si.split_id > 0)
        return si.split_id;
    return skillId;
}

// ---------------------------------------------------------------------------
// SkillDatabase patch loading and versioned views
// ---------------------------------------------------------------------------

// Assign one field from a patch entry onto `old`. Returns false for a key the
// loader does not understand, so the caller can leave it out of the field list
// rather than recording a field it never actually applied.
static bool AssignPatchField(SkillInfo& old, const std::string& field, const json& val)
{
    if (field == "energy")           old.energy      = val.get<int>();
    else if (field == "activation")  old.activation  = val.get<float>();
    else if (field == "recharge")    old.recharge    = val.get<float>();
    else if (field == "adrenaline")  old.adrenaline  = val.get<int>();
    else if (field == "sacrifice")   old.sacrifice   = val.get<int>();
    else if (field == "upkeep")      old.upkeep      = val.get<int>();
    else if (field == "overcast")    old.overcast    = val.get<int>();
    else if (field == "is_elite")    old.is_elite    = val.get<bool>();
    else if (field == "type")        old.type        = val.get<int>();
    else if (field == "profession")  old.profession  = val.get<int>();
    else if (field == "attribute")   old.attribute   = val.get<int>();
    // A skill split for PvP part-way through the tool's history had no split before that
    // date, and Get/GetView follow pvp_split to answer as the PvP half. Without these two a
    // replay predating the split renders the PvP numbers for a skill that had none.
    else if (field == "pvp_split")   old.pvp_split   = val.get<bool>();
    else if (field == "split_id")    old.split_id    = val.get<int>();
    else if (field == "name")        old.name        = val.get<std::string>();
    else if (field == "description") old.description = val.get<std::string>();
    else if (field == "concise")     old.concise     = val.get<std::string>();
    else return false;
    return true;
}

// Copy one already-parsed field from `src` onto `dst`. The counterpart to
// AssignPatchField, used when merging a patch onto an accumulating view.
static void CopyPatchField(SkillInfo& dst, const std::string& field, const SkillInfo& src)
{
    if (field == "energy")           dst.energy      = src.energy;
    else if (field == "activation")  dst.activation  = src.activation;
    else if (field == "recharge")    dst.recharge    = src.recharge;
    else if (field == "adrenaline")  dst.adrenaline  = src.adrenaline;
    else if (field == "sacrifice")   dst.sacrifice   = src.sacrifice;
    else if (field == "upkeep")      dst.upkeep      = src.upkeep;
    else if (field == "overcast")    dst.overcast    = src.overcast;
    else if (field == "is_elite")    dst.is_elite    = src.is_elite;
    else if (field == "type")        dst.type        = src.type;
    else if (field == "profession")  dst.profession  = src.profession;
    else if (field == "attribute")   dst.attribute   = src.attribute;
    else if (field == "pvp_split")   dst.pvp_split   = src.pvp_split;
    else if (field == "split_id")    dst.split_id    = src.split_id;
    else if (field == "name")        dst.name        = src.name;
    else if (field == "description") dst.description = src.description;
    else if (field == "concise")     dst.concise     = src.concise;
}

void SkillDatabase::LoadPatches(const std::string& dataDir)
{
    namespace fs = std::filesystem;

    m_patches.clear();
    m_viewCache.clear();

    fs::path patchDir = fs::path(dataDir) / "skillpatches";
    if (!fs::exists(patchDir) || !fs::is_directory(patchDir))
        return;

    for (const auto& entry : fs::directory_iterator(patchDir))
    {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".json") continue;

        // Expect filename YYYY-MM-DD.json
        std::string stem = path.stem().string();
        if (stem.size() != 10 || stem[4] != '-' || stem[7] != '-') continue;

        int y = 0, m = 0, d = 0;
        try {
            y = std::stoi(stem.substr(0, 4));
            m = std::stoi(stem.substr(5, 2));
            d = std::stoi(stem.substr(8, 2));
        } catch (...) { continue; }

        if (y <= 0 || m < 1 || m > 12 || d < 1 || d > 31) continue;

        std::ifstream f(path);
        if (!f.is_open()) continue;

        json j = json::parse(f, nullptr, false, true);
        if (j.is_discarded()) continue;
        if (!j.contains("skills")) continue;

        SkillPatch patch;
        patch.dateKey = y * 10000 + m * 100 + d;

        for (auto& [key, val] : j["skills"].items())
        {
            int skillId = 0;
            try { skillId = std::stoi(key); } catch (...) { continue; }

            if (!val.is_object()) continue;

            // Look up the current (latest) SkillInfo as the base to copy from
            auto baseIt = m_skills.find(skillId);
            SkillInfo old = (baseIt != m_skills.end()) ? baseIt->second : SkillInfo{};
            old.id = skillId;

            // Override with the old values stored in the patch, remembering which
            // fields this patch actually names -- GetView needs that to merge one
            // patch onto another without clobbering fields it never touched.
            std::vector<std::string> named;
            named.reserve(val.size());
            for (auto& [field, fieldVal] : val.items())
            {
                if (AssignPatchField(old, field, fieldVal))
                    named.push_back(field);
            }

            ParseScalesFromDescription(old);
            ClassifyDeductionUsability(old);
            patch.fields[skillId] = std::move(named);
            patch.overrides[skillId] = std::move(old);
        }

        m_patches.push_back(std::move(patch));
    }

    std::sort(m_patches.begin(), m_patches.end(),
        [](const SkillPatch& a, const SkillPatch& b) { return a.dateKey < b.dateKey; });
}

SkillDatabaseView SkillDatabase::GetView(int year, int month, int day)
{
    int dateKey = year * 10000 + month * 100 + day;

    auto cacheIt = m_viewCache.find(dateKey);
    if (cacheIt != m_viewCache.end())
        return SkillDatabaseView(cacheIt->second);

    // Start with a copy of the latest data
    auto data = std::make_shared<std::unordered_map<int, SkillInfo>>(m_skills);

    // Apply patches dated AFTER the replay date in reverse, newest first, so
    // multiple patches to the same skill accumulate.
    //
    // Merge field by field rather than assigning the whole SkillInfo. Each
    // patch's `overrides` entry is a copy of the CURRENT skill with only that
    // patch's fields rolled back, so assigning it wholesale would also write
    // today's values for every field the patch does not mention -- undoing a
    // newer patch that did change one of them. That is not hypothetical: with
    // the June and August 2026 patches loaded, four skills (Black Spider
    // Strike, Mirage Cloak, Mighty Throw, Disrupting Chop) were each patched in
    // both, on partly different fields, and a pre-June replay rendered the
    // post-August value for the field only August named.
    for (int i = static_cast<int>(m_patches.size()) - 1; i >= 0; --i)
    {
        const SkillPatch& patch = m_patches[i];
        if (patch.dateKey <= dateKey) continue; // patch is not after replay date

        for (const auto& [skillId, oldInfo] : patch.overrides)
        {
            auto fieldsIt = patch.fields.find(skillId);
            auto cur = data->find(skillId);

            // No entry to merge onto, or a patch written before field tracking
            // existed: fall back to the whole-struct behaviour.
            if (cur == data->end() || fieldsIt == patch.fields.end())
            {
                (*data)[skillId] = oldInfo;
                continue;
            }

            for (const std::string& field : fieldsIt->second)
                CopyPatchField(cur->second, field, oldInfo);

            // Scales are derived from the description, so re-derive whenever a
            // patch may have changed it.
            ParseScalesFromDescription(cur->second);
            ClassifyDeductionUsability(cur->second);
        }
    }

    auto constData = std::shared_ptr<const std::unordered_map<int, SkillInfo>>(std::move(data));
    m_viewCache[dateKey] = constData;
    return SkillDatabaseView(std::move(constData));
}

SkillDatabaseView SkillDatabase::GetBaseView() const
{
    return SkillDatabaseView(m_baseView);
}
