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

const SkillInfo* SkillDatabaseView::Get(int skillId) const
{
    if (!m_data) return nullptr;
    auto it = m_data->find(skillId);
    if (it == m_data->end()) return nullptr;
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

            // Look up the current (latest) SkillInfo as the base to copy from
            auto baseIt = m_skills.find(skillId);
            SkillInfo old = (baseIt != m_skills.end()) ? baseIt->second : SkillInfo{};
            old.id = skillId;

            // Override with the old values stored in the patch
            if (val.contains("energy"))      old.energy      = val["energy"].get<int>();
            if (val.contains("activation"))  old.activation  = val["activation"].get<float>();
            if (val.contains("recharge"))    old.recharge    = val["recharge"].get<float>();
            if (val.contains("adrenaline"))  old.adrenaline  = val["adrenaline"].get<int>();
            if (val.contains("sacrifice"))   old.sacrifice   = val["sacrifice"].get<int>();
            if (val.contains("upkeep"))      old.upkeep      = val["upkeep"].get<int>();
            if (val.contains("overcast"))    old.overcast    = val["overcast"].get<int>();
            if (val.contains("is_elite"))    old.is_elite    = val["is_elite"].get<bool>();
            if (val.contains("type"))        old.type        = val["type"].get<int>();
            if (val.contains("profession"))  old.profession  = val["profession"].get<int>();
            if (val.contains("attribute"))   old.attribute   = val["attribute"].get<int>();
            if (val.contains("name"))        old.name        = val["name"].get<std::string>();
            if (val.contains("description")) old.description = val["description"].get<std::string>();
            if (val.contains("concise"))     old.concise     = val["concise"].get<std::string>();

            ParseScalesFromDescription(old);
            ClassifyDeductionUsability(old);
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

    // Apply patches dated AFTER the replay date in reverse (newest first so
    // multiple patches to the same skill accumulate correctly)
    for (int i = static_cast<int>(m_patches.size()) - 1; i >= 0; --i)
    {
        const SkillPatch& patch = m_patches[i];
        if (patch.dateKey <= dateKey) continue; // patch is not after replay date

        for (const auto& [skillId, oldInfo] : patch.overrides)
            (*data)[skillId] = oldInfo;
    }

    auto constData = std::shared_ptr<const std::unordered_map<int, SkillInfo>>(std::move(data));
    m_viewCache[dateKey] = constData;
    return SkillDatabaseView(std::move(constData));
}

SkillDatabaseView SkillDatabase::GetBaseView() const
{
    return SkillDatabaseView(m_baseView);
}

// ---------------------------------------------------------------------------
// ClassifyRangeContext
//
// What kind of magnitude an "X...Y" range names, decided from the words around it. Shared by
// ParseScalesFromDescription and ClassifyDeductionUsability so the two can never drift: they
// answer different questions about the same ranges, and the moment their context rules differ
// a skill can be scaled one way and deduced another.
//
// Returns None for a range the surrounding text does not explain (a recharge, a chance, a
// number of foes), which the callers drop.
// ---------------------------------------------------------------------------
static SkillScaleKind ClassifyRangeContext(const std::string& text,
                                           size_t matchStart, size_t matchEnd)
{
    auto toLower = [](const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return out;
    };
    auto ltrim = [](const std::string& s) {
        size_t i = 0;
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
        return s.substr(i);
    };
    auto startsWith = [](const std::string& s, const char* pfx) {
        size_t n = std::strlen(pfx);
        return s.size() >= n && s.compare(0, n, pfx) == 0;
    };
    auto contains = [](const std::string& s, const char* needle) {
        return s.find(needle) != std::string::npos;
    };
    auto endsWith = [](const std::string& s, const char* sfx) {
        size_t n = std::strlen(sfx);
        return s.size() >= n && s.compare(s.size() - n, n, sfx) == 0;
    };
    // "damage" possibly preceded by a single qualifier word ("cold damage").
    static const std::regex kDamageRe(R"(^\s*(?:[a-z]+\s+)?damage)");

    // Percentages are ratios, not absolute magnitudes.
    if (matchEnd < text.size() && text[matchEnd] == '%')
        return SkillScaleKind::None;

    // ~40 chars of context on each side, lower-cased.
    const size_t beforeStart = (matchStart > 40) ? matchStart - 40 : 0;
    const std::string before = toLower(text.substr(beforeStart, matchStart - beforeStart));
    const std::string after =
        toLower(text.substr(matchEnd, std::min<size_t>(40, text.size() - matchEnd)));
    const std::string afterTrim = ltrim(after);

    if (startsWith(afterTrim, "second"))
        return SkillScaleKind::Duration;   // "for X...Y second(s)" / "X...Y seconds"

    if (startsWith(afterTrim, "health"))
    {
        // "maximum Health" is a ceiling on a pool, not a magnitude any packet carries.
        if (endsWith(before, "maximum ")) return SkillScaleKind::None;
        if (contains(before, "steal")) return SkillScaleKind::LifeSteal;
        if (contains(before, "lose") || contains(before, "sacrifice"))
            return SkillScaleKind::LifeLoss;
        if (contains(before, "heal") || contains(before, "gain") || contains(before, "regain"))
            return SkillScaleKind::Heal;
        return SkillScaleKind::None;
    }

    if (startsWith(afterTrim, "energy"))
    {
        // "Energy regeneration" is a pip rate, not a magnitude: nothing in the energy
        // stream ever equals it. Without this guard "The Power Is Yours!" (1...2 Energy
        // regeneration) would read as a 1...2 energy gain.
        if (startsWith(afterTrim, "energy regeneration")) return SkillScaleKind::None;
        if (contains(before, "lose")) return SkillScaleKind::EnergyLoss;   // covers "loses"
        if (contains(before, "gain")) return SkillScaleKind::Energy;       // covers "gains"
        return SkillScaleKind::None;
    }

    // Heals that name no unit after the number. "Target ally is healed for 20...120." and
    // "Heal target ally for 15...60 points." are the exact wording of half the Monk bar -
    // Healing Burst, Signet of Rejuvenation, Shield Guardian, Wielder's Boon, Death's Charge -
    // and the missing word "Health" is the only reason the rules above never saw them.
    {
        const std::string tail12 = before.size() <= 12 ? before : before.substr(before.size() - 12);
        const std::string tail24 = before.size() <= 24 ? before : before.substr(before.size() - 24);
        if (contains(tail12, "healed for"))
            return SkillScaleKind::Heal;
        // "Heal target ally for" also opens sentences that go on to name something else, so the
        // number has to be the end of the clause or carry a health unit.
        if (contains(tail24, "heal target ally for") &&
            (afterTrim.empty() || afterTrim[0] == '.' || startsWith(afterTrim, "points")))
            return SkillScaleKind::Heal;
    }

    if (std::regex_search(afterTrim, kDamageRe))
        return SkillScaleKind::Damage;

    return SkillScaleKind::None;
}

void SkillDatabase::ParseScalesFromDescription(SkillInfo& si)
{
    si.scales.clear();

    // Prefer the full description; fall back to the concise form.
    const std::string& text = !si.description.empty() ? si.description : si.concise;
    if (text.empty()) return;

    // Matches two-value ranges such as "82...172". GW descriptions never use a
    // three-value form, so a single pattern suffices.
    static const std::regex kRangeRe(R"((\d+)\.\.\.(\d+))");

    auto begin = std::sregex_iterator(text.begin(), text.end(), kRangeRe);
    auto end   = std::sregex_iterator();

    for (auto it = begin; it != end; ++it)
    {
        const std::smatch& m = *it;

        int v0 = 0, v15 = 0;
        try {
            v0  = std::stoi(m[1].str());
            v15 = std::stoi(m[2].str());
        } catch (...) {
            continue;
        }

        const size_t matchStart = (size_t)m.position(0);
        const size_t matchEnd   = matchStart + (size_t)m.length(0);

        const SkillScaleKind kind = ClassifyRangeContext(text, matchStart, matchEnd);
        if (kind == SkillScaleKind::None)
            continue; // unmatched context -> drop

        SkillScale sc;
        sc.kind = kind;
        sc.v0 = (float)v0;
        sc.v15 = (float)v15;
        sc.multiplier = 1.f;
        si.scales.push_back(sc);
    }
}

// ---------------------------------------------------------------------------
// ClassifyEnergyScale
//
// The energy half of ClassifyDeductionUsability, split out so the two streams read as two
// independent decisions rather than one tangled one.
//
// Three differences from the health rules, each for a reason:
//   * no minimum span. Energy ranges are small by nature (Chaos Storm loses 0...2), and unlike
//     health there is no regeneration pip that could masquerade as a breakpoint - the stream
//     carries effect gains and losses only, never a rate.
//   * no weapon-attack veto. Auspicious Blow (id 905) is a Hammer Attack whose "+5...20 damage"
//     is unusable and whose "3...8 Energy" is exact; vetoing the skill would lose the good half.
//   * the "for each" veto looks at the energy CLAUSE, not the whole sentence. Energy Burn
//     (id 42) reads "loses 1...10 Energy and takes 9 damage for each point of Energy lost":
//     the multiplier governs the damage, the energy loss is the total. Cutting the clause at
//     the first " and " keeps id 42 usable while still vetoing "gain 1...3 Energy for each ...".
// ---------------------------------------------------------------------------
static void ClassifyEnergyScale(SkillInfo& si, const std::string& text, SkillScaleKind kind,
                                float v0, float v15, size_t rangeStart, size_t rangeEnd)
{
    if (v15 == v0) return;   // degenerate, cannot rank-match
    if (SkillDatabase::GetProfessionForAttribute(si.attribute) == 0) return;

    auto toLower = [](const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return out;
    };

    const size_t dot = text.find('.', rangeEnd);
    const size_t sentEnd = (dot == std::string::npos) ? text.size() : dot;

    std::string clause = toLower(text.substr(rangeEnd, sentEnd - rangeEnd));
    const size_t andPos = clause.find(" and ");
    if (andPos != std::string::npos) clause.erase(andPos);
    if (clause.find("for each") != std::string::npos ||
        clause.find("for every") != std::string::npos)
        return;

    si.energyUsable = true;
    si.energyKind = kind;
    si.enV0 = v0;
    si.enV15 = v15;

    // Delayed payout: the energy arrives when an effect the skill created ends, which is long
    // past the recorder's 2 s attribution window. The wording is "When this hex ends" (Renewing
    // Surge), "When this effect ends" (Signet of Recall) or "If this enchantment ends
    // prematurely" (Zealous Renewal), so both openings count.
    size_t sentStart = text.rfind('.', rangeStart);
    sentStart = (sentStart == std::string::npos) ? 0 : sentStart + 1;
    const std::string sentence = toLower(text.substr(sentStart, sentEnd - sentStart));
    if (sentence.find("ends") != std::string::npos &&
        (sentence.find("when this") != std::string::npos ||
         sentence.find("if this") != std::string::npos))
        si.energyDelayed = true;
}

// ---------------------------------------------------------------------------
// ClassifyDeductionUsability
//
// Auto-derives the set of skills whose live description yields a single clean,
// attribute-scaled magnitude that the AttributeDeducer can invert into a rank.
// This replaces the former hand-curated 8-skill whitelist.
//
// A skill is usable only if its description parses (through the SAME
// ClassifyRangeContext that ParseScalesFromDescription uses) to EXACTLY ONE
// scale of a HEALTH kind (Damage / Heal / LifeSteal / LifeLoss); Duration
// scales are ignored.
//
// The one exception is a pair of Heal scales, which is not two magnitudes but
// one payout with a conditional half: Word of Healing (id 282) heals 5...100,
// or that plus 30...115 below 50% health, and the recording never says which
// branch fired. Both scales are the skill's own attribute, so the pair still
// measures one rank and dedTwoScale tells the deducer to accept bp1(r) or
// bp1(r) + bp2(r).
//
// Energy scales are classified on their own track (energyUsable / energyKind /
// enV0 / enV15) and deliberately do NOT count towards the "exactly one" rule
// above, because the two streams never collide: a health packet is read from
// combat_events, an energy delta from energy_events. Drain Enchantment (id 68)
// is the case that matters - 8...17 Energy AND 40...120 Health, both scaled by
// Inspiration - and counting its energy scale as a second magnitude would throw
// away a heal the combat log can read perfectly well.
// Multi-scale skills (e.g. Shadow Strike id 102: Damage + LifeSteal) are
// ambiguous and dropped, as are per-instance multiplier skills ("N damage for
// each ...", e.g. Desecrate Enchantments id 112).
//
// The "for each" / "for every" veto MUST be sentence-bounded: it looks only from
// the end of the matched range to the next '.'. A fixed character window would
// wrongly veto Mend Body and Soul (id 1234) - "healed for 20...115 Health. That
// ally loses one condition for each spirit ..." - whose "for each" lives in the
// following sentence. Id 1234 must stay usable (Heal 20..115); id 112 must not.
//
// Divine Favor: single-target Monk heals gain a flat bonus (~3.2 * DF rank) on
// top of the spell's own heal. These are FLAGGED (dfConfounded) rather than
// excluded so the deducer can jointly solve Divine Favor + Healing/Protection.
// Party-wide heals (Heal Party id 287) get NO Divine Favor bonus and are
// detected via the word "party", leaving them clean.
//
// Runtime notes handled by the deducer, not here:
//   - Damage rows are only accepted when armor-ignoring (damageType == 55), so
//     elemental skills like Lightning Orb (id 229) classify as Damage but
//     contribute zero accepted observations.
//   - Weapon attacks ("+10...40 damage") are vetoed here via IsWeaponAttack plus
//     a '+'-prefix guard.
// ---------------------------------------------------------------------------
void SkillDatabase::ClassifyDeductionUsability(SkillInfo& si)
{
    si.deductionUsable = false;
    si.dfConfounded = false;
    si.deductionKind = SkillScaleKind::None;
    si.dedV0 = 0.f;
    si.dedV15 = 0.f;
    si.dedTwoScale = false;
    si.dedV0b = 0.f;
    si.dedV15b = 0.f;
    si.energyUsable = false;
    si.energyKind = SkillScaleKind::None;
    si.enV0 = 0.f;
    si.enV15 = 0.f;
    si.energyDelayed = false;

    // Same source-text selection as ParseScalesFromDescription.
    const std::string& text = !si.description.empty() ? si.description : si.concise;
    if (text.empty()) return;

    auto toLower = [](const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return out;
    };
    auto contains = [](const std::string& s, const char* needle) {
        return s.find(needle) != std::string::npos;
    };

    static const std::regex kRangeRe(R"((\d+)\.\.\.(\d+))");

    // Health-kind and energy-kind matches with their text positions (needed for the
    // sentence-bounded and '+'-prefix vetoes below). Kept in two lists so each stream is
    // classified against its own "exactly one scale" rule.
    struct UsableMatch {
        SkillScaleKind kind = SkillScaleKind::None;
        float v0 = 0, v15 = 0;
        size_t start = 0, end = 0;
    };
    std::vector<UsableMatch> matches;
    std::vector<UsableMatch> energyMatches;

    auto begin = std::sregex_iterator(text.begin(), text.end(), kRangeRe);
    auto end   = std::sregex_iterator();
    for (auto it = begin; it != end; ++it)
    {
        const std::smatch& m = *it;

        int v0 = 0, v15 = 0;
        try {
            v0  = std::stoi(m[1].str());
            v15 = std::stoi(m[2].str());
        } catch (...) {
            continue;
        }

        size_t matchStart = (size_t)m.position(0);
        size_t matchEnd   = matchStart + (size_t)m.length(0);

        const SkillScaleKind kind = ClassifyRangeContext(text, matchStart, matchEnd);

        if (kind == SkillScaleKind::None) continue; // unmatched context
        if (kind == SkillScaleKind::Duration) continue; // durations are not consumable

        UsableMatch um;
        um.kind = kind;
        um.v0 = (float)v0;
        um.v15 = (float)v15;
        um.start = matchStart;
        um.end = matchEnd;

        if (kind == SkillScaleKind::Energy || kind == SkillScaleKind::EnergyLoss)
            energyMatches.push_back(um);
        else
            matches.push_back(um);
    }

    // The energy stream is classified from its own single-scale rule, independently of whether
    // the health path below accepts or rejects this skill.
    if (energyMatches.size() == 1)
    {
        const UsableMatch& em = energyMatches.front();
        ClassifyEnergyScale(si, text, em.kind, em.v0, em.v15, em.start, em.end);
    }

    // Rule (a): one health-kind scale, or two Heal scales that read as one payout.
    //
    // Two Heals are not two magnitudes competing for the same packet: they are the same packet
    // under a condition the recording cannot see. Word of Healing heals 5...100, or that plus
    // 30...115 when the target is below half health; Signet of Rejuvenation, Wielder's Boon and
    // Spirit Light Weapon are shaped the same way. Both scales belong to the skill's own
    // attribute, so the pair still measures one rank - the deducer accepts bp1(r) or
    // bp1(r) + bp2(r) and keeps the union. Any other multi-scale skill stays ambiguous.
    const bool twoHeals = (matches.size() == 2 &&
                           matches[0].kind == SkillScaleKind::Heal &&
                           matches[1].kind == SkillScaleKind::Heal);
    if (matches.size() != 1 && !twoHeals) return;
    const UsableMatch& um = matches.front();

    // Rule (b): meaningful per-rank step.
    //
    // 15 points over 15 ranks is a step of one, which is what separates a real breakpoint table
    // from a health regeneration pip that happens to sit on a number. Life steal is the
    // exception the model has to make: Avatar of Grenth steals 0...12 per scythe hit and never
    // arrives as a regeneration tick, so its 0.8-per-rank step is coarse rather than fake.
    // Adjacent ranks collide there, which the deducer answers by emitting the whole colliding
    // set at half weight rather than by pretending the skill said nothing.
    const float minSpan = (um.kind == SkillScaleKind::LifeSteal) ? 8.f : 15.f;
    if (std::fabs(um.v15 - um.v0) < minSpan) return;

    // Rule (c): attribute must map to a real profession (drops No Attribute and
    // title tracks).
    if (GetProfessionForAttribute(si.attribute) == 0) return;

    // Rule (d): weapon-attack veto.
    if (IsWeaponAttack(si.type)) return;

    // Rules (e) and (f), applied to EVERY scale that will be consumed - both halves of a
    // two-Heal skill, not just the first. Leader's Comfort ("You gain 30...75 Health. For each
    // ally within earshot, you also gain +10...20 Health") is the case that needs it: its
    // second scale is a per-instance multiplier AND a '+' bonus, and letting it through would
    // invent a payout of 30...75 plus 10...20 that the game never pays in one packet.
    auto passesClauseVetoes = [&](const UsableMatch& match) {
        // (e) per-instance multiplier veto, SENTENCE-BOUNDED.
        const size_t dot = text.find('.', match.end);
        const size_t sentEnd = (dot == std::string::npos) ? text.size() : dot;
        const std::string sentence = toLower(text.substr(match.end, sentEnd - match.end));
        if (contains(sentence, "for each") || contains(sentence, "for every")) return false;
        // (f) '+'-prefix veto (weapon-style additive bonuses).
        if (match.start > 0 && text[match.start - 1] == '+') return false;
        return true;
    };
    for (const UsableMatch& match : matches)
        if (!passesClauseVetoes(match)) return;

    // Passed all vetoes.
    si.deductionUsable = true;
    si.deductionKind = um.kind;
    si.dedV0 = um.v0;
    si.dedV15 = um.v15;
    if (twoHeals)
    {
        si.dedTwoScale = true;
        si.dedV0b = matches[1].v0;
        si.dedV15b = matches[1].v15;
    }

    // Rule (g): Divine Favor confound flag (NOT an exclusion). A single-target
    // Monk heal is confounded; a party-wide heal is not.
    if (um.kind == SkillScaleKind::Heal && si.profession == 3)
    {
        std::string lowText = toLower(text);
        if (!contains(lowText, "party"))
            si.dfConfounded = true;
    }
}
