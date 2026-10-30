#include "pch.h"
#include "draw_replay_browser.h"
#include "RunLog.h"
#include "ReplayLibrary.h"
#include "GuiGlobalConstants.h"
#include "TextureCache.h"
#include "GuildCapeCache.h"
#include "SkillDatabase.h"
#include "MatchRatings.h"
#include "MatchNotes.h"
#include "TeamColors.h"
#include "ReplayHotkeys.h"
#include "CatapultLeverState.h"
#include "BattleLedger.h"
#include "EquipmentData.h"
#include "BitmapFont.h"
#include "Net/HttpClient.h"
#include <algorithm>
#include <set>
#include <unordered_map>
#include <thread>
#include <fstream>
#include <array>
#include <map>
#include <unordered_set>
#include <json.hpp>
#include "MatchStats.h"

#include "ui/ui_kit.h"

// ─── The shared UI kit ───────────────────────────────────────────────
//
// The chrome this file used to own privately - the palette, the theme push, the splitters, the
// responsive breakpoints, the little drawing helpers - now lives in SourceFiles/ui/ui_kit.h, so
// that a second screen can be built out of the same parts instead of copying their styles. The
// bodies moved unchanged; the names are pulled back into this file's scope here, so every call
// site below reads and draws exactly as it did before.
using ui::kColorBg;
using ui::kColorPanel;
using ui::kColorPanelLight;
using ui::kColorBorder;
using ui::kColorAccent;
using ui::kColorAccentDim;
using ui::kColorText;
using ui::kColorTextDim;
using ui::kColorTextMuted;
using ui::kColorSelected;
using ui::kColorHover;
using ui::kCardMapName;
using ui::kCardDate;
using ui::kCardDuration;
using ui::kCardGuildName;
using ui::kCardGuildTag;
using ui::kCardVS;
using ui::kCardTeamLabel;
using ui::kCardProfSig;
using ui::kCardBuildName;
using ui::kCardViewDetails;
using ui::kCardMatchupBg;
using ui::kCardMatchupRule;
using ui::LerpColor;
using ui::BrowserThemeColors;
using ui::s_themeColors;
using ui::ApplyBrowserTheme;
using ui::PushGlassTheme;
using ui::PopGlassTheme;
using ui::VSplitter;
using ui::HSplitter;
using ui::BrowserPulse;
using ui::DrawPulseGlow;
using ui::DrawPillBadge;
using ui::ToLower;
using ui::FuzzyMatch;
using ui::TruncateToWidth;
using ui::ComboFromVec;
using ui::PushDropdownArrowStyle;
using ui::PopDropdownArrowStyle;
using ui::HighlightedSelectable;
using ui::LayoutMode;
using ui::ComputeLayout;
using ui::ResponsiveSizes;
using ui::GetSizes;
using ui::GetProfessionIconFile;
using ui::EnsureTextureBasePath;
using ui::GetProfessionIcon;


// ─── Build composition system ────────────────────────────────────────────────

static const char* GetProfAbbrev(int id)
{
    switch (id) {
    case 1: return "W"; case 2: return "R"; case 3: return "Mo"; case 4: return "N";
    case 5: return "Me"; case 6: return "E"; case 7: return "A"; case 8: return "Rt";
    case 9: return "P"; case 10: return "D"; default: return "?";
    }
}

struct SkillCountReq { int skillId; int minPlayers; };

struct BuildDef {
    std::string name;
    std::map<std::string, int> professions;    // exact prof counts (empty = skip check)
    std::map<std::string, int> minProfessions; // minimum prof counts (at least N)
    std::vector<int> keySkills;                // any-of: at least 1 player uses one
    std::vector<SkillCountReq> skillCounts;    // each: at least N players use skill
};

static std::vector<BuildDef> s_buildDefs;
static bool s_buildDefsLoaded = false;
// Bumped whenever s_buildDefs changes. ComputeTeamBuild's answer depends on the definitions,
// so the cached per-match builds have to be thrown away when they are edited or reloaded.
static int  s_buildDefsGen = 0;

// ReplayLibrary's generation, sampled once per frame in draw_replay_browser. Every cache in
// this file that is index-aligned with the match list keys off it.
//
// The vector's address and size are not enough on their own: RescanDiff replaces entries in
// place when a match's cloud status flips, and re-sorts the whole vector afterwards, so an
// unchanged address and count can still mean every index now points at a different match.
static int s_libraryGeneration = -1;
static std::filesystem::path s_buildDefsPath; // resolved local path for saving

static void ParseBuildDefs(const nlohmann::json& j, std::vector<BuildDef>& out)
{
    if (!j.contains("builds") || !j["builds"].is_array()) return;
    for (auto& b : j["builds"])
    {
        BuildDef def;
        def.name = b.value("name", "");
        if (b.contains("professions") && b["professions"].is_object())
            for (auto& [k, v] : b["professions"].items())
                def.professions[k] = v.get<int>();
        if (b.contains("min_professions") && b["min_professions"].is_object())
            for (auto& [k, v] : b["min_professions"].items())
                def.minProfessions[k] = v.get<int>();
        if (b.contains("key_skills") && b["key_skills"].is_array())
            for (auto& s : b["key_skills"])
                def.keySkills.push_back(s.get<int>());
        if (b.contains("skill_counts") && b["skill_counts"].is_array())
            for (auto& sc : b["skill_counts"])
                def.skillCounts.push_back({ sc.value("skill", 0), sc.value("min", 1) });
        if (!def.name.empty())
            out.push_back(std::move(def));
    }
}

static nlohmann::json SerializeBuildDefs(const std::vector<BuildDef>& defs)
{
    nlohmann::json j;
    j["builds"] = nlohmann::json::array();
    for (const auto& def : defs)
    {
        nlohmann::json b;
        b["name"] = def.name;
        if (!def.professions.empty()) b["professions"] = def.professions;
        if (!def.minProfessions.empty()) b["min_professions"] = def.minProfessions;
        if (!def.keySkills.empty()) b["key_skills"] = def.keySkills;
        if (!def.skillCounts.empty())
        {
            b["skill_counts"] = nlohmann::json::array();
            for (const auto& sc : def.skillCounts)
                b["skill_counts"].push_back({{"skill", sc.skillId}, {"min", sc.minPlayers}});
        }
        j["builds"].push_back(b);
    }
    return j;
}

static void SaveBuildDefs()
{
    if (s_buildDefsPath.empty()) return;
    auto settingsDir = s_buildDefsPath.parent_path();
    if (!std::filesystem::exists(settingsDir))
        std::filesystem::create_directories(settingsDir);
    s_buildDefsGen++;
    std::ofstream f(s_buildDefsPath);
    if (!f.is_open()) return;
    f << SerializeBuildDefs(s_buildDefs).dump(2) << "\n";
}

static void PushBuildsToCloud()
{
    if (s_buildDefsPath.empty()) return;
    // Find the scripts directory (search up from exe)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        auto script = dir / "scripts" / "push_builds.py";
        if (std::filesystem::exists(script))
        {
            std::string cmd = "python \"" + script.string() + "\" \"" + s_buildDefsPath.string() + "\"";
            if (!GuiGlobalConstants::contributor_key.empty())
            {
                const auto& key = GuiGlobalConstants::contributor_key;
                bool safe = std::all_of(key.begin(), key.end(), [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
                });
                if (safe)
                    cmd += " --key \"" + key + "\"";
            }
            // Fire and forget (async would be better but this is rare)
            std::thread([cmd]() { std::system(cmd.c_str()); }).detach();
            return;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
}

static void LoadBuildDefs()
{
    if (s_buildDefsLoaded) return;
    s_buildDefsLoaded = true;

    // 1. Try fetching from cloud
    try {
        HttpClient http;
        std::wstring host(GuiGlobalConstants::cloud_storage_host.begin(),
                          GuiGlobalConstants::cloud_storage_host.end());
        http.SetBaseUrl(host, true);
        auto resp = http.Get(L"/builds.json");
        if (resp.IsOk() && !resp.body.empty())
        {
            std::string body(resp.body.begin(), resp.body.end());
            auto j = nlohmann::json::parse(body, nullptr, false);
            if (!j.is_discarded())
                ParseBuildDefs(j, s_buildDefs);
        }
    } catch (...) {}

    // 2. Resolve local path and merge local builds
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();

    for (int i = 0; i < 5; i++)
    {
        auto p = dir / "settings" / "builds.json";
        if (std::filesystem::exists(p))
        {
            s_buildDefsPath = p;
            std::ifstream f(p);
            if (f.is_open())
            {
                try {
                    nlohmann::json j;
                    f >> j;
                    // Merge: add local defs not already present (by name)
                    std::set<std::string> existingNames;
                    for (const auto& d : s_buildDefs)
                        existingNames.insert(d.name);

                    std::vector<BuildDef> localDefs;
                    ParseBuildDefs(j, localDefs);
                    for (auto& ld : localDefs)
                    {
                        if (!existingNames.count(ld.name))
                            s_buildDefs.push_back(std::move(ld));
                    }
                } catch (...) {}
            }
            return;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }

    // Fallback: use exe-adjacent settings dir
    s_buildDefsPath = std::filesystem::path(exePath).parent_path() / "settings" / "builds.json";
}

static std::string ComputeProfSignature(const std::map<std::string, int>& counts)
{
    // Sort by abbreviation alphabetically, format as "2A/2E/1Me/2Mo/1W"
    std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end());
    std::string sig;
    for (auto& [abbr, cnt] : sorted)
    {
        if (!sig.empty()) sig += "/";
        sig += std::to_string(cnt) + abbr;
    }
    return sig;
}

static std::string ComputeTeamBuild(const PartyMeta& party)
{
    LoadBuildDefs();

    // Count primary professions and per-player skill sets
    std::map<std::string, int> profCounts;
    std::vector<std::set<int>> playerSkills;
    for (const auto& p : party.players)
    {
        if (p.primary >= 1 && p.primary <= 10)
            profCounts[GetProfAbbrev(p.primary)]++;
        playerSkills.emplace_back(p.used_skills.begin(), p.used_skills.end());
    }

    if (profCounts.empty()) return "";

    // Try matching against build definitions
    for (const auto& def : s_buildDefs)
    {
        // Profession check (skip if empty — pure skill-based match)
        if (!def.professions.empty() && def.professions != profCounts) continue;

        // Minimum profession check (at least N of each listed prof)
        if (!def.minProfessions.empty())
        {
            bool met = true;
            for (const auto& [abbr, minCnt] : def.minProfessions)
            {
                auto it = profCounts.find(abbr);
                if (it == profCounts.end() || it->second < minCnt) { met = false; break; }
            }
            if (!met) continue;
        }

        // key_skills: at least 1 player uses any of these
        if (!def.keySkills.empty())
        {
            bool hasKey = false;
            for (const auto& ps : playerSkills)
                for (int sk : def.keySkills)
                    if (ps.count(sk)) { hasKey = true; break; }
            if (!hasKey) continue;
        }

        // skill_counts: for each entry, at least N players must use that skill
        if (!def.skillCounts.empty())
        {
            bool allMet = true;
            for (const auto& sc : def.skillCounts)
            {
                int count = 0;
                for (const auto& ps : playerSkills)
                    if (ps.count(sc.skillId)) count++;
                if (count < sc.minPlayers) { allMet = false; break; }
            }
            if (!allMet) continue;
        }

        return def.name;
    }

    // Fallback: profession signature
    return ComputeProfSignature(profCounts);
}

// ─── GW1 skill template encoder ─────────────────────────────────────────────

static std::string EncodeSkillTemplate(int primary, int secondary,
                                       const std::vector<int>& skills,
                                       const std::vector<std::pair<int, int>>& attributes = {})
{
    // GW1 template code format: variable-length bitstream, base64 encoded.
    // Reference: https://wiki.guildwars.com/wiki/Skill_template_format
    std::vector<bool> bits;

    auto pushBits = [&](int value, int count) {
        for (int i = 0; i < count; i++)
            bits.push_back((value >> i) & 1);
    };

    // Header: type 14 (skill template), version 0
    pushBits(14, 4);  // template type = skill bar
    pushBits(0, 4);   // version

    // Profession IDs: need enough bits for max ID (10 = Dervish)
    int profBits = 4; // 4 bits covers 0-15
    pushBits(profBits - 4, 2); // bits-per-prof minus 4, encoded in 2 bits (0 = 4 bits)
    pushBits(primary, profBits);
    pushBits(secondary, profBits);

    // Attributes: count, bits per attribute id minus 4, then (id, rank) pairs, rank in 4 bits.
    // The ranks are the points bought, without runes or headgear, as the game's template holds them.
    const int attrCount = std::min((int)attributes.size(), 15);
    int maxAttr = 0;
    for (int i = 0; i < attrCount; i++) maxAttr = std::max(maxAttr, attributes[i].first);
    int attrBits = 4;
    while ((1 << attrBits) <= maxAttr) attrBits++;
    pushBits(attrCount, 4);
    pushBits(attrBits - 4, 4);
    for (int i = 0; i < attrCount; i++)
    {
        pushBits(attributes[i].first, attrBits);
        pushBits(std::clamp(attributes[i].second, 0, 12), 4);
    }

    // Skills: need enough bits for max skill ID
    int maxSkill = 0;
    for (int s : skills)
        if (s > maxSkill) maxSkill = s;

    int skillBits = 8;
    if (maxSkill > 0)
    {
        skillBits = 0;
        int tmp = maxSkill;
        while (tmp > 0) { skillBits++; tmp >>= 1; }
        skillBits = std::max(skillBits, 8);   // the field holds bits minus 8; never below
    }

    pushBits(skillBits - 8, 4); // bits-per-skill minus 8, encoded in 4 bits

    // 8 skill slots
    for (int i = 0; i < 8; i++)
        pushBits(i < (int)skills.size() ? skills[i] : 0, skillBits);

    // Pad to multiple of 6 bits for base64
    while (bits.size() % 6 != 0)
        bits.push_back(false);

    // Base64 encode (GW1 uses a custom alphabet)
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    for (size_t i = 0; i < bits.size(); i += 6)
    {
        int val = 0;
        for (int b = 0; b < 6; b++)
            if (bits[i + b]) val |= (1 << b);
        result += alphabet[val];
    }
    return result;
}

// ─── Profession helpers ──────────────────────────────────────────────────────

static const char* GetProfessionName(int id)
{
    switch (id)
    {
    case 1:  return "Warrior";
    case 2:  return "Ranger";
    case 3:  return "Monk";
    case 4:  return "Necromancer";
    case 5:  return "Mesmer";
    case 6:  return "Elementalist";
    case 7:  return "Assassin";
    case 8:  return "Ritualist";
    case 9:  return "Paragon";
    case 10: return "Dervish";
    default: return "";
    }
}

// Abbreviation -> profession id
static int ProfAbbrevToId(const std::string& abbr)
{
    if (abbr == "W")  return 1;  if (abbr == "R")  return 2;
    if (abbr == "Mo") return 3;  if (abbr == "N")  return 4;
    if (abbr == "Me") return 5;  if (abbr == "E")  return 6;
    if (abbr == "A")  return 7;  if (abbr == "Rt") return 8;
    if (abbr == "P")  return 9;  if (abbr == "D")  return 10;
    return 0;
}

// Display order: melees first, then casters
static int ProfDisplayOrder(const std::string& abbr)
{
    static const char* order[] = { "W","D","A","R","P","E","N","Me","Mo","Rt" };
    for (int i = 0; i < 10; i++) if (abbr == order[i]) return i;
    return 99;
}

struct CompToken { int profId; int count; std::string abbr; };

static std::vector<CompToken> ParseCompString(const std::string& sig)
{
    std::vector<CompToken> tokens;
    size_t pos = 0;
    while (pos < sig.size())
    {
        // Parse count
        int count = 0;
        while (pos < sig.size() && sig[pos] >= '0' && sig[pos] <= '9')
            count = count * 10 + (sig[pos++] - '0');
        // Parse abbreviation
        std::string abbr;
        while (pos < sig.size() && sig[pos] != '/')
            abbr += sig[pos++];
        if (pos < sig.size() && sig[pos] == '/') pos++;
        if (!abbr.empty())
            tokens.push_back({ ProfAbbrevToId(abbr), count > 0 ? count : 1, abbr });
    }
    std::sort(tokens.begin(), tokens.end(),
        [](const CompToken& a, const CompToken& b) { return ProfDisplayOrder(a.abbr) < ProfDisplayOrder(b.abbr); });
    return tokens;
}

// Rank badge styling for the tier ramp. Order: C > A > B > Scrim
struct RankStyle { ImU32 bg; ImU32 fg; ImU32 border; };
static RankStyle GetRankStyle(const std::string& occasion)
{
    if (occasion == "C AT" || occasion == "mAT Playoffs")
        return { IM_COL32(245, 158, 11, 230), IM_COL32(24, 24, 27, 255), 0 }; // filled amber
    if (occasion == "A AT")
        return { 0, IM_COL32(245, 158, 11, 255), IM_COL32(245, 158, 11, 255) }; // amber outline
    if (occasion == "B AT")
        return { IM_COL32(245, 158, 11, 33), IM_COL32(224, 162, 78, 255), 0 }; // faint tint
    // Scrim / unknown
    return { 0, IM_COL32(113, 113, 122, 255), IM_COL32(63, 63, 70, 255) }; // zinc outline
}

// ─── Map helpers ─────────────────────────────────────────────────────────────

static const char* GetMapName(int mapId)
{
    switch (mapId)
    {
    case 7: case 171:   return "Warrior's Isle";
    case 8: case 172:   return "Hunter's Isle";   
    case 9: case 173:   return "Wizard's Isle";
    case 52: case 167:  return "Burning Isle";
    case 68: case 170:  return "Frozen Isle";
    case 69: case 174:  return "Nomad's Isle";
    case 70: case 168:  return "Druid's Isle";
    case 71: case 175:  return "Isle of the Dead";
    case 360: case 358: return "Isle of Meditation";
    case 361: case 355: return "Isle of Weeping Stone";
    case 362: case 356: return "Isle of Jade";
    case 363: case 357: return "Imperial Isle";
    case 531: case 533: return "Uncharted Isle";
    case 532: case 534: return "Isle of Wurms";
    case 537: case 541: return "Corrupted Isle";
    case 540: case 542: return "Isle of Solitude";
    default:            return nullptr;
    }
}

static const char* GetMapIconFile(int mapId)
{
    switch (mapId)
    {
    case 7: case 171:   return "Warrior's_Isle.jpg";
    case 8: case 172:   return "Hunters_Isle_icon.jpg";
    case 9: case 173:   return "Wizards_Isle_icon.jpg";
    case 52: case 167:  return "Burning_Isle_icon.jpg";
    case 68: case 170:  return "Frozen_Isle.jpg";
    case 69: case 174:  return "Nomads_Isle.jpg";
    case 70: case 168:  return "Druids_Isle_icon.jpg";
    case 71: case 175:  return "Isle_of_the_Dead_icon.jpg";
    case 360: case 358: return "Isle_of_Meditation_icon.jpg";
    case 361: case 355: return "Isle_of_Weeping_Stone_icon.jpg";
    case 362: case 356: return "Isle_of_Jade.jpg";
    case 363: case 357: return "Imperial_Isle.jpg";
    case 531: case 533: return "Uncharted_Isle.jpg";
    case 532: case 534: return "Isle_of_Wurms_icon.jpg";
    case 537: case 541: return "Corrupted_Isle_icon.jpg";
    case 540: case 542: return "Isle_of_Solitude.jpg";
    default:            return nullptr;
    }
}

// ─── Guild/party helpers ─────────────────────────────────────────────────────

struct GuildLabel
{
    std::string name;
    std::string tag;
    std::string display;  // "Name [Tag]"
    int rank = 0;
};

static void ParseFolderTags(const std::string& folderName, std::string& tag1, std::string& tag2)
{
    auto vs = folderName.find("]vs[");
    if (vs != std::string::npos) {
        auto open1 = folderName.rfind('[', vs);
        auto close2 = folderName.find(']', vs + 4);
        if (open1 != std::string::npos && close2 != std::string::npos) {
            tag1 = folderName.substr(open1 + 1, vs - open1 - 1);
            tag2 = folderName.substr(vs + 4, close2 - (vs + 4));
        }
    }
}

static GuildLabel GetPartyGuild(const MatchMeta& m, const std::string& partyId,
                                const std::string& folderTag = "")
{
    GuildLabel result;

    // Prefer folder-name tag (authoritative from GW match list)
    if (!folderTag.empty()) {
        for (const auto& [id, gm] : m.guilds) {
            if (gm.tag == folderTag) {
                result.name = gm.name;
                result.tag = gm.tag;
                result.rank = gm.rank;
                result.display = result.name + " [" + result.tag + "]";
                return result;
            }
        }
    }

    auto pit = m.parties.find(partyId);
    if (pit == m.parties.end() || pit->second.players.empty())
    {
        // No player data (e.g. cloud-only match) — try guild lookup directly by party ID
        auto git = m.guilds.find(partyId);
        if (git != m.guilds.end() && !git->second.name.empty())
        {
            result.name = git->second.name;
            result.tag = git->second.tag;
            result.rank = git->second.rank;
            result.display = result.name + " [" + result.tag + "]";
        }
        else
        {
            result.display = "?";
        }
        return result;
    }

    std::map<int, int> guildCounts;
    for (const auto& p : pit->second.players)
        if (p.guild_id > 0) guildCounts[p.guild_id]++;

    int bestGuildId = 0, bestCount = 0;
    for (const auto& [gid, cnt] : guildCounts)
        if (cnt > bestCount) { bestGuildId = gid; bestCount = cnt; }

    if (bestGuildId == 0)
    {
        // No guild_id on players (e.g. cloud-only metadata) — fall back to guild by party ID
        auto git = m.guilds.find(partyId);
        if (git != m.guilds.end() && !git->second.name.empty())
        {
            result.name = git->second.name;
            result.tag = git->second.tag;
            result.rank = git->second.rank;
            result.display = result.name + " [" + result.tag + "]";
        }
        else
        {
            result.display = "Unknown";
        }
        return result;
    }

    auto guildIdStr = std::to_string(bestGuildId);
    auto git = m.guilds.find(guildIdStr);
    if (git != m.guilds.end())
    {
        result.name = git->second.name;
        result.tag = git->second.tag;
        result.rank = git->second.rank;
        result.display = result.name + " [" + result.tag + "]";
    }
    else
    {
        result.display = "Guild #" + guildIdStr;
    }
    return result;
}



// The refresh control, drawn with a Guild Wars UI badge instead of the word.
//
// The atlas is 128x64 and holds the badge twice, side by side: a banked copy on the left and
// a lit one on the right. That is exactly the idle/hover pair, so both states come from the
// same texture with no tinting.
//
// highlight: new matches are waiting, so the badge is lit and outlined without a hover.
// Returns true on click.

// Source rects in the atlas. Centred on the badge's solid disc rather than on its alpha
// bounding box: the art carries a soft glow that reaches further right and further down, so a
// tight box sits the badge low and right of the frame it is drawn in. Square, so the button
// is square too.
inline constexpr float kRefreshAtlasW = 128.0f, kRefreshAtlasH = 64.0f;
inline constexpr float kRefreshSrcY0 = 3.0f, kRefreshSrcY1 = 55.0f;
inline constexpr float kRefreshDimX0 = 5.0f,  kRefreshDimX1 = 57.0f;
inline constexpr float kRefreshLitX0 = 69.0f, kRefreshLitX1 = 121.0f;
inline constexpr float kRefreshDrawH = 30.0f;
inline constexpr float kRefreshPad = 4.0f;

// Total height of the control, so the labels beside it can be centred against it.
inline float RefreshIconButtonHeight() { return kRefreshDrawH + kRefreshPad * 2.0f; }

static bool DrawRefreshIconButton(const char* id, bool highlight)
{
    EnsureTextureBasePath();
    const std::string path = ui::TextureBasePath() + "\\Textures\\DDS\\ATEXDXT5\\texture_337393.dds";
    ImTextureID tex = GetTextureCache().GetTexture(path);

    // No atlas: the word still has to be clickable.
    if (!tex)
        return ImGui::SmallButton("Refresh");

    const ImVec2 uvDim0(kRefreshDimX0 / kRefreshAtlasW, kRefreshSrcY0 / kRefreshAtlasH);
    const ImVec2 uvDim1(kRefreshDimX1 / kRefreshAtlasW, kRefreshSrcY1 / kRefreshAtlasH);
    const ImVec2 uvLit0(kRefreshLitX0 / kRefreshAtlasW, kRefreshSrcY0 / kRefreshAtlasH);
    const ImVec2 uvLit1(kRefreshLitX1 / kRefreshAtlasW, kRefreshSrcY1 / kRefreshAtlasH);

    const float sz = kRefreshDrawH;
    const ImVec2 pad(kRefreshPad, kRefreshPad);
    constexpr float kRound = 5.0f;

    // Chrome at rest, not just on hover: with a fully transparent frame the badge read as
    // decoration rather than something to click.
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1, 1, 1, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.18f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.26f));
    ImGui::PushStyleColor(ImGuiCol_Border, highlight ? ImVec4(0.18f, 0.72f, 0.35f, 1.0f)
                                                     : ImVec4(1, 1, 1, 0.16f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,    pad);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, highlight ? 2.0f : 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,   kRound);

    const bool clicked = ImGui::ImageButton(id, tex, ImVec2(sz, sz), uvDim0, uvDim1);
    const bool hovered = ImGui::IsItemHovered();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);

    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float hoverGrow = hovered ? 3.0f : 0.0f;

    // Breathing halo so the control is noticed without being hunted for. Stronger once new
    // matches are waiting, which is when it actually wants pressing.
    DrawPulseGlow(dl, ImVec2(mn.x - hoverGrow, mn.y - hoverGrow),
                  ImVec2(mx.x + hoverGrow, mx.y + hoverGrow),
                  highlight ? IM_COL32(46, 184, 90, 255) : IM_COL32(245, 190, 90, 255),
                  kRound, highlight ? 1.0f : 0.65f);

    // Swap in the lit copy by drawing it over the one already submitted; hover state is only
    // known after the item exists, and the alternative is a frame of lag.
    if (hovered || highlight)
        dl->AddImage(tex, ImVec2(mn.x + pad.x - hoverGrow, mn.y + pad.y - hoverGrow),
                     ImVec2(mn.x + pad.x + sz + hoverGrow, mn.y + pad.y + sz + hoverGrow),
                     uvLit0, uvLit1);

    // A gold ring on hover, so the feedback carries at a glance.
    if (hovered)
        dl->AddRect(ImVec2(mn.x - hoverGrow, mn.y - hoverGrow),
                    ImVec2(mx.x + hoverGrow, mx.y + hoverGrow),
                    IM_COL32(245, 190, 90, 255), kRound + hoverGrow, 0, 2.0f);

    return clicked;
}

static ImTextureID GetMapIcon(int mapId)
{
    const char* file = GetMapIconFile(mapId);
    if (!file) return nullptr;
    EnsureTextureBasePath();
    std::string path = ui::TextureBasePath() + "\\Textures\\Guild_Halls\\" + file;
    return GetTextureCache().GetTexture(path);
}

static ImTextureID GetCupIcon()
{
    EnsureTextureBasePath();
    std::string path = ui::TextureBasePath() + "\\Textures\\Game_UI\\cup.webp";
    return GetTextureCache().GetTexture(path);
}

static ImTextureID GetFluxIcon()
{
    EnsureTextureBasePath();
    std::string path = ui::TextureBasePath() + "\\Textures\\Skill_Icons\\PvP_Flair.png";
    return GetTextureCache().GetTexture(path);
}

static ImTextureID GetGuildHallIcon()
{
    EnsureTextureBasePath();
    std::string path = ui::TextureBasePath() + "\\Textures\\Game_UI\\GuildHallIcon.png";
    return GetTextureCache().GetTexture(path);
}

static ImTextureID GetDurationIcon()
{
    EnsureTextureBasePath();
    std::string path = ui::TextureBasePath() + "\\Textures\\Game_UI\\Skill Description\\activation.png";
    return GetTextureCache().GetTexture(path);
}

static ImTextureID GetArenaIcon()
{
    EnsureTextureBasePath();
    std::string path = ui::TextureBasePath() + "\\Textures\\Game_UI\\ArenaIcon.png";
    return GetTextureCache().GetTexture(path);
}

// Stat icons, by base name.
//
// Prefers the background-removed art where it has been exported, and falls back to the
// original plate otherwise, so an icon that has not been cut out yet still shows. Which one
// won is remembered per name: a missing file is not cached by the texture cache, so without
// this the fallbacks would re-probe the filesystem on every frame.
static ImTextureID GetStatIcon(const char* baseName)
{
    EnsureTextureBasePath();

    static std::unordered_map<std::string, std::string> s_resolved;
    auto it = s_resolved.find(baseName);
    if (it == s_resolved.end())
    {
        const std::string dir = ui::TextureBasePath() + "\\Textures\\Match library\\";
        std::string cut   = dir + baseName + "-removebg-preview.png";
        std::string plain = dir + baseName + ".png";
        std::error_code ec;
        it = s_resolved.emplace(baseName,
                 std::filesystem::exists(cut, ec) ? std::move(cut) : std::move(plain)).first;
    }
    return GetTextureCache().GetTexture(it->second);
}

// ─── Guild capes ──────────────────────────────────────────────────
//
// Composed from the cape fields infos.json already carries, the same way the replay loading
// screen builds them. Banners are cached by guild tag, so each is composed once per session
// however many matches that guild appears in.

static GuildCapeCache& BrowserCapeCache()
{
    static GuildCapeCache cache;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        EnsureTextureBasePath();
        auto root = std::filesystem::path(ui::TextureBasePath()) / "Textures" / "CapeAssets";
        std::error_code ec;
        if (std::filesystem::exists(root, ec))
            cache.Init(GetTextureCache().Device(), root);
    }
    return cache;
}

// The cape for one side of a match, or nullptr when the guild is unknown or has none.
// Matched by tag, which is what GetPartyGuild already resolved from the folder name.
//
// A match that has not been downloaded is built from the cloud index, which carries only each
// guild's name and tag - so its CapeData arrives default-constructed. Composing from that
// would not merely draw a blank banner: the cache is keyed by tag alone, so it would store the
// blank under that guild and keep serving it afterwards, including for downloaded matches that
// do have the real thing. All seven fields zero means "not recorded", so stop before caching.
static ImTextureID GetGuildCape(const MatchMeta& m, const GuildLabel& label)
{
    if (label.tag.empty()) return nullptr;
    auto& cache = BrowserCapeCache();
    if (!cache.IsReady()) return nullptr;

    for (const auto& [id, gm] : m.guilds)
    {
        if (gm.tag != label.tag) continue;
        const CapeData& c = gm.cape;
        const bool recorded = c.bg_color || c.detail_color || c.emblem_color
                           || c.shape || c.detail || c.emblem || c.trim;
        return recorded ? cache.GetOrCreate(gm.tag, c) : nullptr;
    }
    return nullptr;
}

// ─── Skill icon lookup ──────────────────────────────────────────────────────

static std::unordered_map<int, std::string> g_skillIconIndex;
static bool g_skillIconIndexBuilt = false;

static void EnsureSkillIconIndex()
{
    if (g_skillIconIndexBuilt) return;
    g_skillIconIndexBuilt = true;

    EnsureTextureBasePath();
    std::string folder = ui::TextureBasePath() + "\\Textures\\Skill_Icons";
    if (!std::filesystem::exists(folder)) return;

    for (const auto& entry : std::filesystem::directory_iterator(folder))
    {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        // Pattern: [ID] - Name.jpg
        if (name.size() < 4 || name[0] != '[') continue;
        size_t closeBracket = name.find(']', 1);
        if (closeBracket == std::string::npos) continue;
        int skillId = 0;
        try { skillId = std::stoi(name.substr(1, closeBracket - 1)); }
        catch (...) { continue; }
        g_skillIconIndex[skillId] = entry.path().string();
    }
}

static ImTextureID GetSkillIcon(int skillId, const SkillDatabaseView* view = nullptr)
{
    if (view && view->IsUnresolvedHistoricalId(skillId)) return nullptr;
    EnsureSkillIconIndex();
    auto it = g_skillIconIndex.find(skillId);
    if (it == g_skillIconIndex.end()) return nullptr;
    return GetTextureCache().GetTexture(it->second);
}

// ─── Skill tooltip helper ────────────────────────────────────────────────────

static ImTextureID GetSkillDescIcon(const char* filename)
{
    EnsureTextureBasePath();
    std::string path = ui::TextureBasePath() + "\\Textures\\Game_UI\\Skill Description\\" + filename;
    return GetTextureCache().GetTexture(path);
}

static void DrawCostIcon(const char* iconFile, const char* valueFmt, float val, bool& hasCost)
{
    if (val <= 0) return;
    ImTextureID tex = GetSkillDescIcon(iconFile);
    if (!tex) return;

    if (hasCost) ImGui::SameLine(0, 10);
    const float h = 16.0f;
    ImGui::Image(tex, ImVec2(h, h));
    ImGui::SameLine(0, 3);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.0f);
    ImGui::Text(valueFmt, val);
    hasCost = true;
}

static void DrawCostIconInt(const char* iconFile, const char* valueFmt, int val, bool& hasCost)
{
    if (val <= 0) return;
    ImTextureID tex = GetSkillDescIcon(iconFile);
    if (!tex) return;

    if (hasCost) ImGui::SameLine(0, 10);
    const float h = 16.0f;
    ImGui::Image(tex, ImVec2(h, h));
    ImGui::SameLine(0, 3);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.0f);
    ImGui::Text(valueFmt, val);
    hasCost = true;
}

static void DrawSkillTooltip(int skillId, const SkillDatabaseView* view = nullptr)
{
    if (view && view->IsUnresolvedHistoricalId(skillId))
    {
        ImGui::BeginTooltip();
        ImGui::Text("Unknown historical skill (ID %d)", skillId);
        ImGui::PushTextWrapPos(340.0f);
        ImGui::TextUnformatted("This older recording contains an unresolved skill ID. "
            "Its original identity is not available in the current skill table.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        return;
    }
    const SkillInfo* si = view ? view->Get(skillId) : GetSkillDatabase().Get(skillId);
    if (!si) return;

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(340.0f);

    // Skill icon + name header
    ImTextureID icon = GetSkillIcon(skillId, view);
    if (icon)
    {
        ImGui::Image(icon, ImVec2(40, 40));
        ImGui::SameLine();
    }

    ImGui::BeginGroup();
    if (si->is_elite)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
        ImGui::Text("{Elite} %s", si->name.c_str());
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::TextUnformatted(si->name.c_str());
        ImGui::PopStyleColor();
    }

    const char* typeName = SkillDatabase::GetTypeName(si->type);
    const char* attrName = SkillDatabase::GetAttributeName(si->attribute);
    if (typeName[0] || attrName[0])
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        if (attrName[0])
            ImGui::Text("%s  (%s)", typeName, attrName);
        else
            ImGui::TextUnformatted(typeName);
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Cost bar with icons
    {
        bool hasCost = false;
        DrawCostIconInt("energy.png",     "%d",   si->energy,     hasCost);
        DrawCostIconInt("adrenaline.png",  "%d",   si->adrenaline, hasCost);
        DrawCostIconInt("sacrifice.png",  "%d%%",  si->sacrifice,  hasCost);
        DrawCostIconInt("overcast.png",   "%d",    si->overcast,   hasCost);

        if (si->upkeep < 0)
        {
            int upkeepVal = -si->upkeep;
            DrawCostIconInt("upkeep.png", "%d", upkeepVal, hasCost);
        }

        if (si->activation > 0)
            DrawCostIcon("activation.png", SkillTimeFormat(si->activation), si->activation, hasCost);

        if (si->recharge > 0)
            DrawCostIcon("recharge.png", SkillTimeFormat(si->recharge), si->recharge, hasCost);
    }

    ImGui::Spacing();

    // Description — strip custom HTML tags
    std::string desc = si->description;
    size_t pos;
    while ((pos = desc.find('<')) != std::string::npos)
    {
        size_t end = desc.find('>', pos);
        if (end == std::string::npos) break;
        desc.erase(pos, end - pos + 1);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.82f, 0.75f, 1.0f));
    ImGui::TextWrapped("%s", desc.c_str());
    ImGui::PopStyleColor();

    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// ─── Theme: Glass-Dark style push/pop ────────────────────────────────────────

// ─── Themeable color palette ────────────────────────────────────────────────


// ─── Flux description lookup ─────────────────────────────────────────────────

struct FluxTableEntry { const char* name; const char* desc; };
static const FluxTableEntry kFluxTable[] = {
    { "Odran's Razor",          "PvP combat is unmodified." },
    { "Amateur Hour",           "Your secondary profession skills deal 30% more damage to foes with that primary profession." },
    { "Hidden Talent",          "You have a +2 bonus to all of the secondary attributes of your secondary profession." },
    { "There Can Be Only One",  "You deal +30% damage to foes of the same primary profession. Each time you kill one of these foes, you regain all Health and Energy and receive a 5% morale boost." },
    { "Meek Shall Inherit",     "If you do not equip an elite skill, you have +2 to all attributes, +2 Health regeneration, and +1 Energy regeneration." },
    { "Jack of All Trades",     "If your attributes are all between 8-11 before buffs, your skills deal 15% additional damage, activate 25% faster, and cost 20% less Energy." },
    { "Chain Combo",            "Gain a stacking 5% damage bonus (max 30%) whenever you use a skill of a different attribute than the last skill used. Bonus resets if your next skill has the same attribute." },
    { "Xinrae's Revenge",       "Whenever you successfully activate a skill, it is disabled (3 seconds) for all party and opposing party members in the area who have it on their skill bars." },
    { "Like a Boss",            "Kill the boss (or any player if there is no boss); now you're the boss: -20 armor, +33% attack speed, +33% movement speed, -33% skill activation time, +3 Health regeneration, and +1 Energy regeneration. If you die, you're not the boss anymore. If a player killed you, they're the boss now." },
    { "Minion Apocalypse",      "Each player death deals 50 damage to all nearby creatures and spawns a masterless bone horror (level 20)." },
    { "All In",                 "If all your skills use one attribute, gain +3 Health regeneration and +100 max Health; your skills also cost 25% less Energy." },
    { "Parting Gift",           "If you die, you drop a bundle on the ground that grants bonuses to whoever picks it up." },
};

const char* GetFluxDescription(const std::string& fluxName)
{
    for (const auto& entry : kFluxTable)
        if (fluxName == entry.name)
            return entry.desc;

    return nullptr;
}

static std::vector<std::string> GetAllKnownFluxNames()
{
    std::vector<std::string> names;
    for (const auto& entry : kFluxTable)
        names.push_back(entry.name);
    return names;
}

static void DrawFluxWithTooltip(const std::string& flux, float infoIconH)
{
    ImTextureID fluxIco = GetFluxIcon();
    if (fluxIco)
    {
        ImGui::Image(fluxIco, ImVec2(infoIconH, infoIconH));
        if (ImGui::IsItemHovered())
        {
            const char* desc = GetFluxDescription(flux);
            if (desc)
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(320.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
                ImGui::TextUnformatted(flux.c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.82f, 0.75f, 1.0f));
                ImGui::TextWrapped("%s", desc);
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        ImGui::SameLine(0, 4);
    }
    ImFont* boldFnt = GuiGlobalConstants::boldFont;
    if (boldFnt) ImGui::PushFont(boldFnt);
    ImGui::TextColored(kColorTextDim, "%s", flux.c_str());
    if (boldFnt) ImGui::PopFont();

    // Dashed underline beneath the flux name
    {
        ImVec2 tMin = ImGui::GetItemRectMin();
        ImVec2 tMax = ImGui::GetItemRectMax();
        float y = tMax.y + 1.0f;
        float dashLen = 4.0f, gapLen = 3.0f;
        ImU32 dashCol = ImGui::GetColorU32(ImVec4(0.35f, 0.35f, 0.35f, 0.60f));
        for (float x = tMin.x; x < tMax.x; x += dashLen + gapLen)
        {
            float x2 = x + dashLen;
            if (x2 > tMax.x) x2 = tMax.x;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x, y), ImVec2(x2, y), dashCol, 1.0f);
        }
    }

    if (ImGui::IsItemHovered())
    {
        const char* desc = GetFluxDescription(flux);
        if (desc)
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(320.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
            ImGui::TextUnformatted(flux.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.82f, 0.75f, 1.0f));
            ImGui::TextWrapped("%s", desc);
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

// ─── Responsive layout system ────────────────────────────────────────────────


// ─── Filter state ────────────────────────────────────────────────────────────

struct DateVal { int day = 0, month = 0, year = 0; };

struct BrowserState
{
    // Global search with debounce
    char  searchBuf[256] = "";
    float lastSearchEditTime = -1.0f;
    char  searchDebounced[256] = "";

    // Multi-select filter selections
    std::set<std::string> selectedSearchTerms;
    int  searchMatchMode = 0;   // 0 = All (every chip must match), 1 = Any
    std::set<std::string> selectedMaps;
    std::set<std::string> selectedFluxes;
    std::set<std::string> selectedOccasions;

    // Auto-complete search buffers
    char mapSearchBuf[128] = "";
    char fluxSearchBuf[128] = "";

    // Date range
    DateVal dateFrom;
    DateVal dateTo;
    int calBrowseFromMonth = 0, calBrowseFromYear = 0;
    int calBrowseToMonth = 0, calBrowseToYear = 0;

    // Filter option lists (built from match data)
    std::vector<std::string> mapNames;
    std::vector<std::string> guildNames;
    std::vector<std::string> fluxNames;
    std::vector<std::string> occasionNames;

    // Global search auto-complete data
    std::vector<std::string> allTeams;
    std::vector<std::string> allPlayers;
    std::vector<std::string> allTags;

    int  selectedMatchIndex = -1;

    // Match Details tabs. The tab is kept when another match is selected, so a reader
    // comparing several matches in Stats stays in Stats.
    int   detailTab = 0;               // DetailTab
    bool  detailMaximised = false;     // detail panel takes the whole Library
    float detailIntervalFrom = 0.0f;   // shared interval, as fractions of the match
    float detailIntervalTo   = 1.0f;
    int   detailIntervalDrag = -1;     // 0 = start handle, 1 = end handle
    float detailChartAnchor  = -1.0f;  // where a drag across the timeline chart began
    float detailPlayhead     = 0.0f;   // Map tab, seconds on the match clock
    bool  detailPlaying      = false;
    float detailPlaySpeed    = 1.0f;
    bool  detailShowTrails   = false;
    bool  detailShowNames    = false;
    float detailMapZoom      = 1.0f;   // Map tab view: 1 = whole map, up to 8
    float detailMapCenterU   = 0.5f;   // centre of the view, 0..1 across the picture
    float detailMapCenterV   = 0.5f;

    int  sortColumn = 0;
    bool sortAscending = false;

    int  minRatingFilter = 0;

    // Build composition filter
    std::set<std::string> selectedBuilds;
    char buildSearchBuf[128] = {};
    std::vector<std::string> buildNames;

    // Skill filter
    std::set<int> selectedSkills;           // canonical skill IDs
    char skillSearchBuf[128] = {};
    int  skillMatchMode = 0;                // 0 = All, 1 = Any
    int  skillScope     = 2;                // 0 = Same player, 1 = Same team, 2 = Either team

    // Matchup filter (guild A vs guild B, side-agnostic)
    char matchupBufA[128] = {};
    char matchupBufB[128] = {};
    std::string matchupDisplayA, matchupNameA, matchupTagA;
    std::string matchupDisplayB, matchupNameB, matchupTagB;

    // Which multi-select filter currently has its "browse all" list open ("" = none)
    std::string browseOpenId;

    bool filtersBuilt = false;
    int  lastMatchCount = -1;
    int  lastLibraryGen = -1;

    // Responsive layout state
    LayoutMode layout = LayoutMode::Full;
    LayoutMode prevLayout = LayoutMode::Full;
    bool sidebarExpanded = true;
    bool mobileShowDetail = false;
    bool statsExpanded = true;

    // User-resizable splitter state (pixels, <=0 means use default)
    float userFilterW = -1.0f;
    float userTopRowH = -1.0f;

    // New-match highlight flash (folder_path -> highlight start time)
    std::unordered_map<std::string, float> highlightStartTimes;

    // Inline notes editor state
    std::string browserNoteBuffer;
    std::string browserNoteMatchId;

    // Notification bar
    int   notifyNewCount = 0;
    std::string notifyMatchName;
    float notifyStartTime = -1.f;
    bool  notifyDismissed = false;

    // Card Gallery state
    bool cardGalleryMode = false;   // false = table, true = card gallery
    int  galleryColumns  = 3;       // 2, 3, or 4
    int  cardStyle       = 0;       // 0 = Classic, 1 = Visual

    // Netflix card hover animation
    int   hoveredCardIdx = -1;
    float hoverStartTime = -1.0f;
    ImVec2 hoveredCardScreenMin;
    ImVec2 hoveredCardScreenMax;
    float  hoveredCardWidth = 0.0f;

    // Tournament Prep state
    bool tournamentMode = false;
    char tournamentGuildBuf[128] = {};       // search input
    std::string tournamentGuildDisplay;      // selected guild "Name [Tag]"
    std::string tournamentGuildTag;          // tag for matching
    std::string tournamentGuildName;         // name for matching
    std::set<std::string> tournamentMaps;    // optional map filter
    char tournamentMapBuf[128] = {};
    int  tournamentSortColumn = 2;           // default sort by times played
    bool tournamentSortAsc = false;
    int  tournamentSelectedBuild = -1;       // drill-down row index (builds table)
    int  tournamentSelectedLostTo = -1;      // drill-down row index (lost-to table)
    char tournamentLostToSearch[128] = {};   // search filter for lost-to table
};

static BrowserState s_state;

// Tournament Prep cache (forward-declared, populated in AggregateTournamentBuilds)
static std::vector<struct TournamentBuildStats> s_tournamentCache;
static std::string s_tournamentCacheKey;
static int s_tournamentCacheMatchCount = -1;
static int s_tournamentCacheGeneration = -1;

// Build naming popup state
static struct {
    char nameBuf[128] = {};
    std::string profSig;
    std::map<std::string, int> profCounts;
    int existingDefIdx = -1;
} s_buildNaming;

static void NotifyNewMatches(int count, const std::string& matchName)
{
    s_state.notifyNewCount = count;
    s_state.notifyMatchName = matchName;
    s_state.notifyStartTime = (float)ImGui::GetTime();
    s_state.notifyDismissed = false;
}


static bool ParseDateStr(const char* buf, int& day, int& month, int& year)
{
    if (!buf || buf[0] == '\0') return false;
    int d = 0, m = 0, y = 0;
    if (sscanf(buf, "%d/%d/%d", &d, &m, &y) == 3)
    {
        if (d >= 1 && d <= 31 && m >= 1 && m <= 12 && y >= 1000 && y <= 9999)
        { day = d; month = m; year = y; return true; }
    }
    d = m = y = 0;
    if (sscanf(buf, "%d-%d-%d", &y, &m, &d) == 3)
    {
        if (d >= 1 && d <= 31 && m >= 1 && m <= 12 && y >= 1000 && y <= 9999)
        { day = d; month = m; year = y; return true; }
    }
    return false;
}

static int CompareDate(int d1, int m1, int y1, int d2, int m2, int y2)
{
    if (y1 != y2) return y1 < y2 ? -1 : 1;
    if (m1 != m2) return m1 < m2 ? -1 : 1;
    if (d1 != d2) return d1 < d2 ? -1 : 1;
    return 0;
}

static std::string SanitizePlayerName(const std::string& name)
{
    if (name.size() < 4) return name;
    if (name.back() != ')') return name;
    size_t open = name.rfind('(');
    if (open == std::string::npos || open < 1) return name;
    bool allDigits = true;
    for (size_t i = open + 1; i + 1 < name.size(); i++)
        if (!isdigit((unsigned char)name[i])) { allDigits = false; break; }
    if (!allDigits || open + 1 >= name.size() - 1) return name;
    size_t trim = open;
    if (trim > 0 && name[trim - 1] == ' ') trim--;
    return name.substr(0, trim);
}

static int DaysInMonth(int month, int year)
{
    static const int d[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month < 1 || month > 12) return 30;
    int n = d[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) n = 29;
    return n;
}

static int DayOfWeek(int day, int month, int year)
{
    static int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    int y = year;
    if (month < 3) y--;
    return (y + y/4 - y/100 + y/400 + t[month-1] + day) % 7;
}

static const char* kMonthNames[] = {
    "January","February","March","April","May","June",
    "July","August","September","October","November","December"
};

static bool DateValValid(const DateVal& d) { return d.day > 0 && d.month > 0 && d.year > 0; }

// ─── Skill filter index ──────────────────────────────────────────────────────

struct SkillOption
{
    int         id = 0;          // canonical skill ID
    std::string name;
    std::string nameLower;
    bool        isElite = false;
    int         profession = 0;
};

// One entry per match, index-aligned with ReplayLibrary::GetMatches().
struct MatchSkillIndex
{
    std::vector<std::vector<int>> teamPlayers[2]; // canonical IDs per player, per team
    std::vector<int>              teamUnion[2];   // sorted, de-duplicated
    std::vector<int>              allUnion;       // sorted, de-duplicated
};

static std::vector<SkillOption>     s_skillOptions;    // sorted by display name
static std::vector<MatchSkillIndex> s_matchSkillIndex;
static std::unordered_map<int, int> s_skillCanonical;  // raw ID -> canonical ID

static int CanonicalSkillId(int rawId)
{
    auto it = s_skillCanonical.find(rawId);
    return it == s_skillCanonical.end() ? rawId : it->second;
}

// PvP/PvE skill splits share a name but carry different IDs; collapse each such
// group onto its lowest ID so one chip matches every variant.
static void BuildSkillIndex(const std::vector<MatchMeta>& matches)
{
    s_skillOptions.clear();
    s_matchSkillIndex.clear();
    s_skillCanonical.clear();

    std::set<int> rawIds;
    for (const auto& m : matches)
        for (const auto& [pid, party] : m.parties)
            for (const auto& p : party.players)
                for (int sk : p.used_skills)
                    if (sk > 0 && !IsUnresolvedHistoricalSkillId(sk,
                            m.year * 10000 + m.month * 100 + m.day)) rawIds.insert(sk);

    if (rawIds.empty()) return;

    const SkillDatabase& db = GetSkillDatabase();

    // Group by skill name; the lowest ID of a group becomes its canonical ID.
    std::map<std::string, std::vector<int>> byName;
    for (int id : rawIds)
    {
        const SkillInfo* si = db.Get(id);
        std::string key = (si && !si->name.empty())
            ? ToLower(si->name)
            : ("#" + std::to_string(id));
        byName[key].push_back(id);
    }

    for (auto& [key, ids] : byName)
    {
        int canonical = *std::min_element(ids.begin(), ids.end());
        for (int id : ids) s_skillCanonical[id] = canonical;

        const SkillInfo* si = db.Get(canonical);
        if (!si || si->name.empty()) continue;  // unnamed skills stay unselectable

        SkillOption opt;
        opt.id         = canonical;
        opt.name       = si->name;
        opt.nameLower  = ToLower(si->name);
        opt.isElite    = si->is_elite;
        opt.profession = si->profession;
        s_skillOptions.push_back(std::move(opt));
    }

    std::sort(s_skillOptions.begin(), s_skillOptions.end(),
        [](const SkillOption& a, const SkillOption& b) { return a.nameLower < b.nameLower; });

    // Per-match canonical skill sets, so filtering never touches the raw metadata.
    s_matchSkillIndex.resize(matches.size());
    for (size_t mi = 0; mi < matches.size(); mi++)
    {
        const auto& m = matches[mi];
        MatchSkillIndex& idx = s_matchSkillIndex[mi];

        for (int team = 0; team < 2; team++)
        {
            auto pit = m.parties.find(team == 0 ? "1" : "2");
            if (pit == m.parties.end()) continue;

            for (const auto& p : pit->second.players)
            {
                std::vector<int> skills;
                skills.reserve(p.used_skills.size());
                for (int sk : p.used_skills)
                    if (sk > 0 && !IsUnresolvedHistoricalSkillId(sk,
                            m.year * 10000 + m.month * 100 + m.day))
                        skills.push_back(CanonicalSkillId(sk));
                std::sort(skills.begin(), skills.end());
                skills.erase(std::unique(skills.begin(), skills.end()), skills.end());
                if (skills.empty()) continue;

                idx.teamUnion[team].insert(idx.teamUnion[team].end(), skills.begin(), skills.end());
                idx.teamPlayers[team].push_back(std::move(skills));
            }

            auto& tu = idx.teamUnion[team];
            std::sort(tu.begin(), tu.end());
            tu.erase(std::unique(tu.begin(), tu.end()), tu.end());
            idx.allUnion.insert(idx.allUnion.end(), tu.begin(), tu.end());
        }

        std::sort(idx.allUnion.begin(), idx.allUnion.end());
        idx.allUnion.erase(std::unique(idx.allUnion.begin(), idx.allUnion.end()),
                           idx.allUnion.end());
    }
}

static bool HasSkill(const std::vector<int>& sortedSkills, int id)
{
    return std::binary_search(sortedSkills.begin(), sortedSkills.end(), id);
}

// Match/Any x Same player / Same team / Either team.
static bool MatchPassesSkillFilter(size_t matchIndex)
{
    if (s_state.selectedSkills.empty()) return true;
    if (matchIndex >= s_matchSkillIndex.size()) return false;

    const MatchSkillIndex& idx = s_matchSkillIndex[matchIndex];
    const bool matchAll = (s_state.skillMatchMode == 0);

    auto setSatisfies = [&](const std::vector<int>& skills) {
        if (matchAll)
        {
            for (int want : s_state.selectedSkills)
                if (!HasSkill(skills, want)) return false;
            return true;
        }
        for (int want : s_state.selectedSkills)
            if (HasSkill(skills, want)) return true;
        return false;
    };

    switch (s_state.skillScope)
    {
    case 0: // Same player
        for (int team = 0; team < 2; team++)
            for (const auto& bar : idx.teamPlayers[team])
                if (setSatisfies(bar)) return true;
        return false;

    case 1: // Same team
        for (int team = 0; team < 2; team++)
            if (setSatisfies(idx.teamUnion[team])) return true;
        return false;

    default: // Either team — anywhere in the match
        return setSatisfies(idx.allUnion);
    }
}

static void BuildFilterLists(const std::vector<MatchMeta>& matches)
{
    if (g_invalidateFilters)
    {
        s_state.filtersBuilt = false;
        g_invalidateFilters = false;
        s_tournamentCacheKey.clear();
    }
    // The count alone is not enough. s_matchSkillIndex is index-aligned with the match list,
    // and a rescan that adds nothing still re-sorts it, which would leave every skill-filter
    // lookup pointing at a different match than the one it describes.
    if (s_state.filtersBuilt && s_state.lastMatchCount == (int)matches.size()
        && s_state.lastLibraryGen == s_libraryGeneration)
        return;
    s_state.lastLibraryGen = s_libraryGeneration;

    std::set<std::string> maps, guilds, fluxes, occasions;
    std::set<std::string> teams, players, tags;

    // Seed with every known flux so the filter can offer all of them, even
    // ones that have not shown up in the currently loaded matches yet.
    for (auto& fn : GetAllKnownFluxNames()) fluxes.insert(fn);

    for (const auto& m : matches)
    {
        const char* mn = GetMapName(m.map_id);
        if (mn) maps.insert(mn);
        else    maps.insert("Map " + std::to_string(m.map_id));

        if (!m.flux.empty())     fluxes.insert(m.flux);
        if (!m.occasion.empty()) occasions.insert(m.occasion);

        for (const auto& [gid, g] : m.guilds)
        {
            if (!g.name.empty())
            {
                std::string display = g.name + " [" + g.tag + "]";
                guilds.insert(display);
                teams.insert(display);
                if (!g.tag.empty()) tags.insert("[" + g.tag + "]");
            }
        }

        for (const auto& [pid, party] : m.parties)
            for (const auto& p : party.players)
                if (!p.encoded_name.empty())
                    players.insert(SanitizePlayerName(p.encoded_name));
    }

    auto ToVec = [](const std::set<std::string>& s) {
        std::vector<std::string> v;
        v.push_back("All");
        for (const auto& e : s) v.push_back(e);
        return v;
    };

    s_state.mapNames      = ToVec(maps);
    s_state.guildNames    = ToVec(guilds);
    s_state.fluxNames     = ToVec(fluxes);
    s_state.occasionNames = ToVec(occasions);
    s_state.allTeams.assign(teams.begin(), teams.end());
    s_state.allPlayers.assign(players.begin(), players.end());
    s_state.allTags.assign(tags.begin(), tags.end());

    // Collect all build compositions
    {
        std::set<std::string> builds;
        for (const auto& m : matches)
        {
            for (const auto& [pid, party] : m.parties)
            {
                std::string b = ComputeTeamBuild(party);
                if (!b.empty()) builds.insert(b);
            }
        }
        s_state.buildNames = ToVec(builds);
    }

    BuildSkillIndex(matches);

    s_state.filtersBuilt = true;
    s_state.lastMatchCount = (int)matches.size();
}


// ─── Auto-complete & chip helpers ────────────────────────────────────────────



enum class MsRowStyle { Plain, Flux };


// Flux dropdown row: icon + name + a one-line preview of the effect, full text on hover.
static bool DrawFluxRow(const std::string& name, const char* query,
                        const char* uid, bool fuzzy)
{
    const float iconH = 18.0f;
    const float lineH = ImGui::GetTextLineHeight();
    const char* desc  = GetFluxDescription(name);
    const float rowH  = desc ? (lineH * 2.0f + 3.0f) : std::max(iconH, lineH);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Selectable((std::string("##fxr_") + uid).c_str(),
                                     false, 0, ImVec2(0, rowH));
    bool hovered = ImGui::IsItemHovered();
    float rowRight = ImGui::GetItemRectMax().x;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = pos.x + 4.0f;

    if (ImTextureID ico = GetFluxIcon())
    {
        dl->AddImage(ico, ImVec2(x, pos.y + 1.0f), ImVec2(x + iconH, pos.y + 1.0f + iconH));
        x += iconH + 6.0f;
    }

    dl->AddText(ImVec2(x, pos.y), ImGui::GetColorU32(kColorText), name.c_str());
    if (desc)
    {
        std::string line = TruncateToWidth(desc, rowRight - x - 6.0f);
        dl->AddText(ImVec2(x, pos.y + lineH + 2.0f),
                    ImGui::GetColorU32(kColorTextDim), line.c_str());
    }

    if (hovered && desc)
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(320.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.82f, 0.75f, 1.0f));
        ImGui::TextWrapped("%s", desc);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    (void)query; (void)fuzzy;
    return clicked;
}

static bool DrawMultiSelectFilter(
    const char* label, const char* hint,
    char* searchBuf, size_t searchBufSize,
    const std::vector<std::string>& allItems,
    std::set<std::string>& selectedItems,
    const char* id, bool useFuzzy,
    MsRowStyle rowStyle = MsRowStyle::Plain)
{
    bool changed = false;

    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    if (!selectedItems.empty())
    {
        const float chipGap = 4.0f;
        float wrapW = ImGui::GetContentRegionAvail().x;
        float lineX = 0.0f;

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 2));

        for (auto it = selectedItems.begin(); it != selectedItems.end(); )
        {
            std::string chipText = *it + "  x";
            std::string chipId = chipText + "##chip_" + id + "_" + *it;

            ImVec2 ts = ImGui::CalcTextSize(chipText.c_str());
            float chipW = ts.x + 16.0f;

            if (lineX > 0.0f && lineX + chipGap + chipW > wrapW)
                lineX = 0.0f;
            else if (lineX > 0.0f)
                ImGui::SameLine(0, chipGap);

            ImGui::PushStyleColor(ImGuiCol_Button, kColorSelected);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.15f, 0.10f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.50f, 0.18f, 0.12f, 1.00f));

            bool erase = ImGui::Button(chipId.c_str());
            lineX += (lineX > 0.0f ? chipGap : 0.0f) + ImGui::GetItemRectSize().x;
            ImGui::PopStyleColor(3);

            if (rowStyle == MsRowStyle::Flux && ImGui::IsItemHovered())
            {
                if (const char* d = GetFluxDescription(*it))
                {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(320.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.82f, 0.75f, 1.0f));
                    ImGui::TextWrapped("%s", d);
                    ImGui::PopStyleColor();
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }

            if (erase)
            {
                it = selectedItems.erase(it);
                changed = true;
            }
            else
                ++it;
        }

        ImGui::PopStyleVar(2);

        ImGui::SameLine(0, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorHover);
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccentDim);
        if (ImGui::SmallButton((std::string("Clear##clr_") + id).c_str()))
        {
            selectedItems.clear();
            changed = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        ImGui::Spacing();
    }

    // Search field + "browse all" arrow
    const float arrowW = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(-(arrowW + 4.0f));
    ImGui::InputTextWithHint(
        (std::string("##ms_") + id).c_str(),
        hint, searchBuf, searchBufSize);
    ImVec2 inputMin = ImGui::GetItemRectMin();
    ImVec2 inputMax = ImGui::GetItemRectMax();
    bool inputActive = ImGui::IsItemActive();

    ImGui::SameLine(0, 2.0f);
    bool browsing = (s_state.browseOpenId == id);
    PushDropdownArrowStyle();
    bool arrowClicked = ImGui::ArrowButton((std::string("##msarrow_") + id).c_str(),
                           browsing ? ImGuiDir_Up : ImGuiDir_Down);
    PopDropdownArrowStyle();
    if (arrowClicked)
    {
        s_state.browseOpenId = browsing ? std::string() : std::string(id);
        browsing = !browsing;
        searchBuf[0] = '\0';
    }
    bool arrowHovered = ImGui::IsItemHovered();
    if (arrowHovered && !browsing)
        ImGui::SetTooltip("Show all %s options", label);
    float rowRight = ImGui::GetItemRectMax().x;

    const bool hasQuery = (searchBuf[0] != '\0');
    if (!hasQuery && !browsing)
        return changed;

    std::string query(searchBuf);
    std::vector<const std::string*> suggestions;
    for (size_t k = 1; k < allItems.size(); k++)
    {
        const auto& item = allItems[k];
        if (selectedItems.count(item)) continue;
        bool match = !hasQuery || (useFuzzy
            ? FuzzyMatch(query, item)
            : (ToLower(item).find(ToLower(query)) != std::string::npos));
        if (match) suggestions.push_back(&item);
    }

    if (suggestions.empty() && !browsing)
        return changed;

    float dropW = rowRight - inputMin.x;
    float rowH  = (rowStyle == MsRowStyle::Flux)
        ? ImGui::GetTextLineHeight() * 2.0f + 3.0f + ImGui::GetStyle().ItemSpacing.y
        : ImGui::GetTextLineHeightWithSpacing();
    float maxH  = std::min(browsing ? 300.0f : 200.0f,
                           std::max(1.0f, (float)suggestions.size()) * rowH + 12.0f);

    ImGui::SetNextWindowPos(ImVec2(inputMin.x, inputMax.y));
    ImGui::SetNextWindowSizeConstraints(ImVec2(dropW, 0), ImVec2(dropW, maxH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.11f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

    bool ddHovered = false;
    std::string ddWinId = std::string("##ddw_") + id;
    if (ImGui::Begin(ddWinId.c_str(), nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ddHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                           ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (suggestions.empty())
            ImGui::TextColored(kColorTextDim, "No match");

        for (size_t si = 0; si < suggestions.size(); si++)
        {
            std::string selId = std::string(id) + "_" + std::to_string(si);
            bool picked = (rowStyle == MsRowStyle::Flux)
                ? DrawFluxRow(*suggestions[si], searchBuf, selId.c_str(), useFuzzy)
                : HighlightedSelectable(suggestions[si]->c_str(),
                                        searchBuf, selId.c_str(), useFuzzy);
            if (picked)
            {
                selectedItems.insert(*suggestions[si]);
                searchBuf[0] = '\0';
                changed = true;
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    // Dismiss the browse list on a click that lands outside it
    if (browsing && !ddHovered && !arrowHovered && !inputActive &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        s_state.browseOpenId.clear();

    return changed;
}

static void DrawGlobalSearchAutoComplete(ImVec2 inputMin, ImVec2 inputMax)
{
    const char* query = s_state.searchBuf;
    if (!query || query[0] == '\0') return;

    std::string qLow = ToLower(query);

    struct SuggGroup { const char* cat; std::vector<const std::string*> items; };
    SuggGroup groups[3] = { {"Teams", {}}, {"Players", {}}, {"Guild Tags", {}} };

    for (const auto& t : s_state.allTeams)
        if (ToLower(t).find(qLow) != std::string::npos)
            groups[0].items.push_back(&t);
    for (const auto& p : s_state.allPlayers)
        if (ToLower(p).find(qLow) != std::string::npos)
            groups[1].items.push_back(&p);
    for (const auto& g : s_state.allTags)
        if (ToLower(g).find(qLow) != std::string::npos)
            groups[2].items.push_back(&g);

    int total = 0;
    for (auto& g : groups) total += (int)g.items.size();
    if (total == 0) return;

    const int maxPerGroup = 5;
    float lineH = ImGui::GetTextLineHeightWithSpacing();
    int visCount = 0;
    for (auto& g : groups)
        if (!g.items.empty())
            visCount += 1 + (int)std::min((int)g.items.size(), maxPerGroup);

    float dropW = inputMax.x - inputMin.x;
    float maxH = std::min(280.0f, visCount * lineH + 8.0f);

    ImGui::SetNextWindowPos(ImVec2(inputMin.x, inputMax.y));
    ImGui::SetNextWindowSizeConstraints(ImVec2(dropW, 0), ImVec2(dropW, maxH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.11f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

    if (ImGui::Begin("##search_ac_win", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize))
    {
        int idx = 0;
        for (auto& g : groups)
        {
            if (g.items.empty()) continue;
            ImGui::PushStyleColor(ImGuiCol_Text, kColorAccentDim);
            ImGui::TextUnformatted(g.cat);
            ImGui::PopStyleColor();
            ImGui::Separator();

            int shown = std::min((int)g.items.size(), maxPerGroup);
            for (int i = 0; i < shown; i++)
            {
                std::string uid = std::string("ac_") + std::to_string(idx++);
                if (HighlightedSelectable(g.items[i]->c_str(), query,
                        uid.c_str(), false))
                {
                    s_state.selectedSearchTerms.insert(*g.items[i]);
                    s_state.searchBuf[0] = '\0';
                    s_state.searchDebounced[0] = '\0';
                    s_state.lastSearchEditTime = -1.0f;
                }
            }
            if ((int)g.items.size() > maxPerGroup)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
                ImGui::Text("  ... %d more", (int)g.items.size() - maxPerGroup);
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

static bool DrawCalendarPicker(const char* id, DateVal& date, int& browseMonth, int& browseYear)
{
    bool changed = false;

    char displayBuf[32];
    if (date.day > 0)
        snprintf(displayBuf, sizeof(displayBuf), "%02d/%02d/%04d", date.day, date.month, date.year);
    else
        snprintf(displayBuf, sizeof(displayBuf), "Select date...");

    std::string btnId = std::string(displayBuf) + "###dp_" + id;
    std::string popupId = std::string("##cal_") + id;

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    if (ImGui::Button(btnId.c_str()))
    {
        if (date.day > 0)
        { browseMonth = date.month; browseYear = date.year; }
        else
        {
            time_t now = time(nullptr);
            struct tm lt;
#ifdef _WIN32
            localtime_s(&lt, &now);
#else
            localtime_r(&now, &lt);
#endif
            browseMonth = lt.tm_mon + 1;
            browseYear  = lt.tm_year + 1900;
        }
        ImGui::OpenPopup(popupId.c_str());
    }
    ImGui::PopStyleVar();

    static const float kCellW = 30.0f;
    static const float kCellPad = 2.0f;
    float gridW = 7 * kCellW + 6 * (kCellPad * 2);
    float popupW = gridW + 16.0f;

    ImGui::SetNextWindowSizeConstraints(ImVec2(popupW, 0), ImVec2(popupW, 450.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.11f, 0.11f, 0.11f, 0.98f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);

    if (ImGui::BeginPopup(popupId.c_str()))
    {
        float navW = ImGui::GetContentRegionAvail().x;

        if (ImGui::SmallButton("<<##yr_prev"))
            browseYear--;
        ImGui::SameLine(0, 2);
        if (ImGui::ArrowButton("##prev_mo", ImGuiDir_Left))
        { browseMonth--; if (browseMonth < 1) { browseMonth = 12; browseYear--; } }
        ImGui::SameLine();

        char hdr[64];
        snprintf(hdr, sizeof(hdr), "%s %d", kMonthNames[browseMonth - 1], browseYear);
        float hdrW = ImGui::CalcTextSize(hdr).x;
        float arrowBtnW = ImGui::GetFrameHeight();
        float smallBtnW = ImGui::CalcTextSize(">>").x + ImGui::GetStyle().FramePadding.x * 2;
        float rightGroupW = arrowBtnW + 2.0f + smallBtnW;
        float centerX = (navW - hdrW) * 0.5f;
        ImGui::SameLine(8.0f + centerX);
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
        ImGui::TextUnformatted(hdr);
        ImGui::PopStyleColor();

        ImGui::SameLine(navW - rightGroupW + 8.0f);
        if (ImGui::ArrowButton("##next_mo", ImGuiDir_Right))
        { browseMonth++; if (browseMonth > 12) { browseMonth = 1; browseYear++; } }
        ImGui::SameLine(0, 2);
        if (ImGui::SmallButton(">>##yr_next"))
            browseYear++;

        ImGui::Spacing();

        static const char* dayHdr[] = {"Mo","Tu","We","Th","Fr","Sa","Su"};
        float cellW = kCellW;

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(kCellPad, kCellPad));
        if (ImGui::BeginTable("##cal_tbl", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX))
        {
            for (int c = 0; c < 7; c++)
                ImGui::TableSetupColumn(dayHdr[c], ImGuiTableColumnFlags_WidthFixed, cellW);

            ImGui::TableNextRow();
            for (int c = 0; c < 7; c++)
            {
                ImGui::TableSetColumnIndex(c);
                ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
                ImGui::TextUnformatted(dayHdr[c]);
                ImGui::PopStyleColor();
            }

            int firstDow = DayOfWeek(1, browseMonth, browseYear);
            int startCol = (firstDow == 0) ? 6 : firstDow - 1;
            int numDays = DaysInMonth(browseMonth, browseYear);
            int curRow = -1;

            for (int day = 1; day <= numDays; day++)
            {
                int col = (startCol + day - 1) % 7;
                int row = (startCol + day - 1) / 7;
                if (row != curRow) { ImGui::TableNextRow(); curRow = row; }
                ImGui::TableSetColumnIndex(col);

                bool sel = (date.day == day && date.month == browseMonth && date.year == browseYear);
                if (sel)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, kColorAccent);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorAccent);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorHover);
                    ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
                }

                char dayStr[8];
                snprintf(dayStr, sizeof(dayStr), "%d##d%d", day, day);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
                if (ImGui::Button(dayStr, ImVec2(cellW, cellW)))
                {
                    date = { day, browseMonth, browseYear };
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar(); // CellPadding

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    return changed;
}

static int GetOccasionOrder(const std::string& occasion);  // defined with the sort helpers

// ─── Skill filter ────────────────────────────────────────────────────────────

// Selected skills show as icons only; the dropdown shows icon + name.
static void DrawSkillFilter()
{
    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::TextUnformatted("Skills");
    ImGui::PopStyleColor();

    if (s_skillOptions.empty())
    {
        ImGui::TextColored(kColorTextDim, "No skill data loaded");
        return;
    }

    if (!s_state.selectedSkills.empty())
    {
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorHover);
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccentDim);
        if (ImGui::SmallButton("Clear##clr_skills"))
            s_state.selectedSkills.clear();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        const float chipSz = 28.0f;
        const float chipGap = 4.0f;
        float wrapW = ImGui::GetContentRegionAvail().x;
        float lineX = 0.0f;
        int removeId = -1;

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
        for (int skillId : s_state.selectedSkills)
        {
            float chipW = chipSz + 4.0f;
            if (lineX > 0.0f && lineX + chipGap + chipW > wrapW)
                lineX = 0.0f;
            else if (lineX > 0.0f)
                ImGui::SameLine(0, chipGap);

            std::string btnId = "##skchip_" + std::to_string(skillId);
            ImTextureID tex = GetSkillIcon(skillId);
            bool clicked = tex
                ? ImGui::ImageButton(btnId.c_str(), tex, ImVec2(chipSz, chipSz))
                : ImGui::Button(btnId.c_str(), ImVec2(chipSz + 4.0f, chipSz + 4.0f));

            if (ImGui::IsItemHovered())
            {
                DrawSkillTooltip(skillId);
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (clicked) removeId = skillId;

            lineX += (lineX > 0.0f ? chipGap : 0.0f) + ImGui::GetItemRectSize().x;
        }
        ImGui::PopStyleVar();

        if (removeId >= 0) s_state.selectedSkills.erase(removeId);
        ImGui::Spacing();
    }

    // Match mode + scope
    if (!s_state.selectedSkills.empty())
    {
        ImGui::Spacing();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 2));

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kColorTextDim, "Match");
        ImGui::SameLine(0, 6);
        const char* modes[2] = { "All", "Any" };
        for (int i = 0; i < 2; i++)
        {
            if (i) ImGui::SameLine(0, 2);
            bool on = (s_state.skillMatchMode == i);
            ImGui::PushStyleColor(ImGuiCol_Button, on ? kColorSelected : s_themeColors.frameBg);
            ImGui::PushStyleColor(ImGuiCol_Text, on ? kColorAccent : kColorTextDim);
            if (ImGui::SmallButton((std::string(modes[i]) + "##skmode" + std::to_string(i)).c_str()))
                s_state.skillMatchMode = i;
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(i == 0
                    ? "Every selected skill must be present"
                    : "At least one selected skill must be present");
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kColorTextDim, "Scope");
        ImGui::SameLine(0, 6);
        const char* scopes[3] = { "Player", "Team", "Match" };
        const char* scopeTips[3] = {
            "One player's skill bar must satisfy the selection",
            "One team must satisfy the selection (across its 8 bars)",
            "Anywhere in the match, either team"
        };
        for (int i = 0; i < 3; i++)
        {
            if (i) ImGui::SameLine(0, 2);
            bool on = (s_state.skillScope == i);
            ImGui::PushStyleColor(ImGuiCol_Button, on ? kColorSelected : s_themeColors.frameBg);
            ImGui::PushStyleColor(ImGuiCol_Text, on ? kColorAccent : kColorTextDim);
            if (ImGui::SmallButton((std::string(scopes[i]) + "##skscope" + std::to_string(i)).c_str()))
                s_state.skillScope = i;
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", scopeTips[i]);
        }

        ImGui::PopStyleVar();
    }

    // Search field + browse arrow
    const float arrowW = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(-(arrowW + 4.0f));
    ImGui::InputTextWithHint("##ms_skill", "Search skills...",
        s_state.skillSearchBuf, sizeof(s_state.skillSearchBuf));
    ImVec2 inputMin = ImGui::GetItemRectMin();
    ImVec2 inputMax = ImGui::GetItemRectMax();
    bool inputActive = ImGui::IsItemActive();

    ImGui::SameLine(0, 2.0f);
    bool browsing = (s_state.browseOpenId == "skill");
    PushDropdownArrowStyle();
    bool arrowClicked = ImGui::ArrowButton("##msarrow_skill", browsing ? ImGuiDir_Up : ImGuiDir_Down);
    PopDropdownArrowStyle();
    if (arrowClicked)
    {
        s_state.browseOpenId = browsing ? std::string() : std::string("skill");
        browsing = !browsing;
        s_state.skillSearchBuf[0] = '\0';
    }
    bool arrowHovered = ImGui::IsItemHovered();
    if (arrowHovered && !browsing)
        ImGui::SetTooltip("Show all skills used in the library");
    float rowRight = ImGui::GetItemRectMax().x;

    const bool hasQuery = (s_state.skillSearchBuf[0] != '\0');
    if (!hasQuery && !browsing) return;

    std::string qLow = ToLower(std::string(s_state.skillSearchBuf));
    std::vector<const SkillOption*> suggestions;
    for (const auto& opt : s_skillOptions)
    {
        if (s_state.selectedSkills.count(opt.id)) continue;
        if (hasQuery && opt.nameLower.find(qLow) == std::string::npos) continue;
        suggestions.push_back(&opt);
    }

    if (suggestions.empty() && !browsing) return;

    const float iconH = 22.0f;
    float rowH  = std::max(iconH + 2.0f, ImGui::GetTextLineHeightWithSpacing());
    float dropW = std::max(rowRight - inputMin.x, 240.0f);
    float maxH  = std::min(320.0f, std::max(1.0f, (float)suggestions.size()) * rowH + 12.0f);

    ImGui::SetNextWindowPos(ImVec2(inputMin.x, inputMax.y));
    ImGui::SetNextWindowSizeConstraints(ImVec2(dropW, 0), ImVec2(dropW, maxH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.11f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

    bool ddHovered = false;
    if (ImGui::Begin("##ddw_skill", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ddHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                           ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (suggestions.empty())
            ImGui::TextColored(kColorTextDim, "No skill matches");

        ImGuiListClipper clipper;
        clipper.Begin((int)suggestions.size(), rowH);
        int pickedId = -1;
        while (clipper.Step())
        {
            for (int si = clipper.DisplayStart; si < clipper.DisplayEnd; si++)
            {
                const SkillOption* opt = suggestions[si];
                ImVec2 pos = ImGui::GetCursorScreenPos();

                if (ImGui::Selectable(("##skrow_" + std::to_string(opt->id)).c_str(),
                                      false, 0, ImVec2(0, rowH)))
                    pickedId = opt->id;
                bool hovered = ImGui::IsItemHovered();

                ImDrawList* dl = ImGui::GetWindowDrawList();
                float x = pos.x + 4.0f;
                if (ImTextureID tex = GetSkillIcon(opt->id))
                {
                    dl->AddImage(tex, ImVec2(x, pos.y + 1.0f),
                                 ImVec2(x + iconH, pos.y + 1.0f + iconH));
                }
                x += iconH + 6.0f;

                ImU32 nameCol = opt->isElite
                    ? ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.3f, 1.0f))
                    : ImGui::GetColorU32(kColorText);
                float ty = pos.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
                dl->AddText(ImVec2(x, ty), nameCol, opt->name.c_str());

                if (hovered) DrawSkillTooltip(opt->id);
            }
        }
        clipper.End();

        if (pickedId >= 0)
        {
            s_state.selectedSkills.insert(pickedId);
            s_state.skillSearchBuf[0] = '\0';
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    if (browsing && !ddHovered && !arrowHovered && !inputActive &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        s_state.browseOpenId.clear();
}

// ─── Occasion filter (two-level tree) ────────────────────────────────────────

// Occasion strings come straight from infos.json, so groups are derived by shape.
// Matching is per word, not by substring: "Automated Tournament" contains "mat" but
// is not an mAT stage.
static const char* OccasionGroupOf(const std::string& occasion)
{
    std::vector<std::string> words;
    std::string cur;
    for (char c : occasion)
    {
        if (isalnum((unsigned char)c))
            cur += (char)tolower((unsigned char)c);
        else if (!cur.empty())
        {
            words.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) words.push_back(cur);

    auto has = [&words](const char* w) {
        return std::find(words.begin(), words.end(), w) != words.end();
    };

    if (has("mat"))                          return "Monthly AT (mAT)";
    if (has("scrim") || has("scrimmage"))    return "Scrimmage";
    if (has("at") || has("tournament"))      return "Automated Tournament";
    return "Other";
}

static ImVec4 WithMinAlpha(ImVec4 c, float minA) { c.w = std::max(c.w, minA); return c; }

static void PushTreeCheckboxStyle()
{
    // Per-checkbox fill/border (on vs. off vs. mixed) is pushed separately
    // via PushCheckboxState below, since Dear ImGui's FrameBg alone doesn't
    // vary with checked state. This just sets up hover/active feedback and
    // shape - rounding, a visible border, and a slightly tighter box than
    // the rest of the UI's frame widgets.
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, WithMinAlpha(kColorHover,    0.85f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  WithMinAlpha(kColorSelected, 0.90f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
}

static void PopTreeCheckboxStyle()
{
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

enum class CheckboxVisual { Off, Mixed, On };

// Solid accent fill + dark mark when fully checked; a muted well when not;
// mixed keeps the well fill but picks up the accent border/mark so a
// partially-selected group still reads as "something in here is on".
static void PushCheckboxState(CheckboxVisual v)
{
    static const ImVec4 kOffBorder = ImVec4(0.361f, 0.337f, 0.282f, 0.85f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,   v == CheckboxVisual::On ? kColorAccent : kColorPanelLight);
    ImGui::PushStyleColor(ImGuiCol_Border,    v == CheckboxVisual::Off ? kOffBorder : kColorAccent);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, v == CheckboxVisual::On ? kColorBg : kColorAccent);
}

static void PopCheckboxState()
{
    ImGui::PopStyleColor(3);
}

static void DrawOccasionTreeFilter(const std::unordered_map<std::string, int>& occasionCounts)
{
    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::TextUnformatted("Occasion");
    ImGui::PopStyleColor();

    if (!s_state.selectedOccasions.empty())
    {
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorHover);
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccentDim);
        if (ImGui::SmallButton("Clear##clr_occ"))
            s_state.selectedOccasions.clear();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }

    // Fixed group order keeps the tree stable regardless of what the library holds
    static const char* kGroupOrder[] = {
        "Monthly AT (mAT)", "Automated Tournament", "Scrimmage", "Other"
    };

    std::map<std::string, std::vector<const std::string*>> groups;
    for (size_t i = 1; i < s_state.occasionNames.size(); i++)
    {
        const std::string& occ = s_state.occasionNames[i];
        groups[OccasionGroupOf(occ)].push_back(&occ);
    }

    for (const char* groupName : kGroupOrder)
    {
        auto git = groups.find(groupName);
        if (git == groups.end() || git->second.empty()) continue;

        // Order stages within a group the same way the Date column sorts them
        std::vector<const std::string*> items = git->second;
        std::sort(items.begin(), items.end(),
            [](const std::string* a, const std::string* b) {
                int oa = GetOccasionOrder(*a), ob = GetOccasionOrder(*b);
                if (oa != ob) return oa < ob;
                return *a < *b;
            });

        int selCount = 0;
        int groupMatchCount = 0;
        for (const auto* occ : items)
        {
            if (s_state.selectedOccasions.count(*occ)) selCount++;
            auto cit = occasionCounts.find(*occ);
            if (cit != occasionCounts.end()) groupMatchCount += cit->second;
        }

        bool allOn  = (selCount == (int)items.size());
        bool someOn = (selCount > 0 && !allOn);

        ImGui::PushID(groupName);
        PushTreeCheckboxStyle();

        if (someOn)
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        PushCheckboxState(allOn ? CheckboxVisual::On : someOn ? CheckboxVisual::Mixed : CheckboxVisual::Off);
        bool parentState = allOn;
        if (ImGui::Checkbox("##grp", &parentState))
        {
            for (const auto* occ : items)
            {
                if (parentState) s_state.selectedOccasions.insert(*occ);
                else             s_state.selectedOccasions.erase(*occ);
            }
        }
        PopCheckboxState();
        if (someOn)
            ImGui::PopItemFlag();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s all %d occasions in this group",
                              allOn ? "Deselect" : "Select", (int)items.size());

        ImGui::SameLine(0, 4);
        ImGui::PushStyleColor(ImGuiCol_Text,
            (allOn || someOn) ? kColorAccent : kColorText);
        bool open = ImGui::TreeNodeEx("##grpnode",
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding |
            ImGuiTreeNodeFlags_DefaultOpen,
            "%s (%d)", groupName, groupMatchCount);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%d match%s in this group under the current filters",
                              groupMatchCount, groupMatchCount == 1 ? "" : "es");

        if (open)
        {
            for (const auto* occ : items)
            {
                bool on = s_state.selectedOccasions.count(*occ) > 0;
                auto cit = occasionCounts.find(*occ);
                int leafCount = (cit != occasionCounts.end()) ? cit->second : 0;

                std::string label = *occ + "  (" + std::to_string(leafCount) + ")";
                if (leafCount == 0) ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
                PushCheckboxState(on ? CheckboxVisual::On : CheckboxVisual::Off);
                bool changed = ImGui::Checkbox(label.c_str(), &on);
                PopCheckboxState();
                if (leafCount == 0) ImGui::PopStyleColor();

                if (changed)
                {
                    if (on) s_state.selectedOccasions.insert(*occ);
                    else    s_state.selectedOccasions.erase(*occ);
                }
            }
            ImGui::TreePop();
        }

        PopTreeCheckboxStyle();
        ImGui::PopID();
    }
}

// ─── Matchup filter (guild A vs guild B) ─────────────────────────────────────

static void SplitGuildDisplay(const std::string& display,
                              std::string& outName, std::string& outTag)
{
    auto open = display.rfind('[');
    auto close = display.rfind(']');
    if (open != std::string::npos && close != std::string::npos && close > open)
    {
        outName = display.substr(0, open);
        while (!outName.empty() && outName.back() == ' ') outName.pop_back();
        outTag = display.substr(open + 1, close - open - 1);
    }
    else
    {
        outName = display;
        outTag.clear();
    }
}

// One autocomplete slot; returns true when the selection changed.
static bool DrawMatchupSlot(const char* slotId, const char* hint,
                            char* buf, size_t bufSize,
                            std::string& display, std::string& name, std::string& tag)
{
    bool changed = false;
    const bool slotFilled = !display.empty();

    const float clearW = slotFilled ? (ImGui::GetFrameHeight() + 4.0f) : 0.0f;
    ImGui::SetNextItemWidth(clearW > 0.0f ? -clearW : -1.0f);
    bool enterPressed = ImGui::InputTextWithHint(
        (std::string("##mu_") + slotId).c_str(), hint, buf, bufSize,
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImVec2 inputMin = ImGui::GetItemRectMin();
    ImVec2 inputMax = ImGui::GetItemRectMax();

    if (slotFilled)
    {
        ImGui::SameLine(0, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorHover);
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccentDim);
        if (ImGui::Button((std::string("x##muclr_") + slotId).c_str(),
                          ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
        {
            buf[0] = '\0';
            display.clear(); name.clear(); tag.clear();
            changed = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }

    std::string qLow = ToLower(std::string(buf));
    if (qLow.empty() || s_state.guildNames.size() <= 1)
        return changed;

    // Already showing the exact selection — no need to re-open the list
    if (!display.empty() && ToLower(display) == qLow)
        return changed;

    std::vector<const std::string*> hits;
    for (size_t i = 1; i < s_state.guildNames.size() && hits.size() < 40; i++)
        if (ToLower(s_state.guildNames[i]).find(qLow) != std::string::npos)
            hits.push_back(&s_state.guildNames[i]);

    if (hits.empty()) return changed;

    auto pick = [&](const std::string& picked) {
        display = picked;
        SplitGuildDisplay(display, name, tag);
        snprintf(buf, bufSize, "%s", display.c_str());
        changed = true;
    };

    if (enterPressed)
    {
        pick(*hits[0]);
        return changed;
    }

    float dropW = inputMax.x - inputMin.x;
    float maxH  = std::min(220.0f, hits.size() * ImGui::GetTextLineHeightWithSpacing() + 8.0f);

    ImGui::SetNextWindowPos(ImVec2(inputMin.x, inputMax.y));
    ImGui::SetNextWindowSizeConstraints(ImVec2(dropW, 0), ImVec2(dropW, maxH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.11f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

    if (ImGui::Begin((std::string("##ddw_mu_") + slotId).c_str(), nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize))
    {
        for (size_t i = 0; i < hits.size(); i++)
        {
            std::string uid = std::string("mu_") + slotId + "_" + std::to_string(i);
            if (HighlightedSelectable(hits[i]->c_str(), buf, uid.c_str(), false))
                pick(*hits[i]);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    return changed;
}

static bool MatchupSlotSet(const std::string& name, const std::string& tag)
{
    return !name.empty() || !tag.empty();
}

static void DrawMatchupFilter()
{
    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::TextUnformatted("Matchup");
    ImGui::PopStyleColor();

    bool anySet = MatchupSlotSet(s_state.matchupNameA, s_state.matchupTagA) ||
                  MatchupSlotSet(s_state.matchupNameB, s_state.matchupTagB);

    if (anySet)
    {
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorHover);
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccentDim);
        if (ImGui::SmallButton("Clear##clr_matchup"))
        {
            s_state.matchupBufA[0] = '\0';
            s_state.matchupBufB[0] = '\0';
            s_state.matchupDisplayA.clear(); s_state.matchupNameA.clear(); s_state.matchupTagA.clear();
            s_state.matchupDisplayB.clear(); s_state.matchupNameB.clear(); s_state.matchupTagB.clear();
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }

    DrawMatchupSlot("a", "Team A...", s_state.matchupBufA, sizeof(s_state.matchupBufA),
                    s_state.matchupDisplayA, s_state.matchupNameA, s_state.matchupTagA);

    // "vs" divider
    ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
    ImGui::TextUnformatted("vs");
    ImGui::PopStyleColor();

    DrawMatchupSlot("b", "Team B...", s_state.matchupBufB, sizeof(s_state.matchupBufB),
                    s_state.matchupDisplayB, s_state.matchupNameB, s_state.matchupTagB);
}

static void DrawDateRangeFilter()
{
    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::TextUnformatted("Date Range");
    ImGui::PopStyleColor();

    if (DateValValid(s_state.dateFrom) || DateValValid(s_state.dateTo))
    {
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorHover);
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccentDim);
        if (ImGui::SmallButton("Reset##date_reset"))
        {
            s_state.dateFrom = {};
            s_state.dateTo = {};
        }
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
    }

    float labelCol = ImGui::CalcTextSize("From:").x + 8.0f;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("From:");
    ImGui::SameLine(labelCol);
    bool fromChanged = DrawCalendarPicker("from", s_state.dateFrom,
        s_state.calBrowseFromMonth, s_state.calBrowseFromYear);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("To:");
    ImGui::SameLine(labelCol);
    bool toChanged = DrawCalendarPicker("to", s_state.dateTo,
        s_state.calBrowseToMonth, s_state.calBrowseToYear);

    if (fromChanged && DateValValid(s_state.dateFrom) && DateValValid(s_state.dateTo))
    {
        if (CompareDate(s_state.dateFrom.day, s_state.dateFrom.month, s_state.dateFrom.year,
                        s_state.dateTo.day, s_state.dateTo.month, s_state.dateTo.year) > 0)
            s_state.dateTo = {};
    }
    if (toChanged && DateValValid(s_state.dateFrom) && DateValValid(s_state.dateTo))
    {
        if (CompareDate(s_state.dateFrom.day, s_state.dateFrom.month, s_state.dateFrom.year,
                        s_state.dateTo.day, s_state.dateTo.month, s_state.dateTo.year) > 0)
            s_state.dateFrom = {};
    }
}

// ─── Sort helpers ────────────────────────────────────────────────────────────

static int GetOccasionOrder(const std::string& occasion)
{
    if (occasion == "A AT")                return 0;
    if (occasion == "B AT")                return 1;
    if (occasion == "C AT")                return 2;
    if (occasion == "Swiss-Rounds mAT")    return 3;
    if (occasion == "mAT Playoffs")        return 4;
    if (occasion == "mAT Quarterfinals")   return 5;
    if (occasion == "mAT Semifinals")      return 6;
    if (occasion == "mAT Finals")          return 7;
    return 99;
}

static int GetMapRotationOrder(int month, const std::string& mapName)
{
    static const char* rotations[12][5] = {
        // January
        {"Isle of Weeping Stone", "Uncharted Isle", "Druid's Isle", "Burning Isle", "Warrior's Isle"},
        // February
        {"Isle of Wurms", "Isle of Jade", "Isle of Meditation", "Imperial Isle", "Druid's Isle"},
        // March
        {"Burning Isle", "Frozen Isle", "Warrior's Isle", "Isle of Solitude", "Uncharted Isle"},
        // April
        {"Isle of the Dead", "Isle of Solitude", "Imperial Isle", "Isle of Wurms", "Isle of Jade"},
        // May
        {"Isle of Wurms", "Imperial Isle", "Isle of Meditation", "Warrior's Isle", "Frozen Isle"},
        // June
        {"Isle of Weeping Stone", "Isle of Jade", "Warrior's Isle", "Uncharted Isle", "Imperial Isle"},
        // July
        {"Burning Isle", "Isle of Wurms", "Uncharted Isle", "Nomad's Isle", "Isle of Solitude"},
        // August
        {"Isle of the Dead", "Warrior's Isle", "Corrupted Isle", "Frozen Isle", "Isle of Meditation"},
        // September
        {"Isle of Solitude", "Druid's Isle", "Corrupted Isle", "Isle of Weeping Stone", "Uncharted Isle"},
        // October
        {"Isle of Meditation", "Uncharted Isle", "Isle of Jade", "Isle of Solitude", "Isle of the Dead"},
        // November
        {"Corrupted Isle", "Imperial Isle", "Nomad's Isle", "Isle of Meditation", "Isle of Jade"},
        // December
        {"Burning Isle", "Druid's Isle", "Warrior's Isle", "Uncharted Isle", "Frozen Isle"},
    };

    if (month < 1 || month > 12) return 99;
    const char** rot = rotations[month - 1];
    for (int i = 0; i < 5; i++)
        if (mapName == rot[i]) return i;
    return 99;
}

// ─── Card Gallery helpers ────────────────────────────────────────────────────

struct OccasionStyle {
    ImVec4 textColor;
    ImVec4 bgColor;
    const char* shortLabel;
};

static OccasionStyle GetOccasionStyle(const std::string& occasion)
{
    if (occasion == "C AT")
        return { ImVec4(0.973f, 0.443f, 0.443f, 1.0f),
                 ImVec4(0.937f, 0.267f, 0.267f, 0.2f), "C AT" };
    if (occasion == "B AT")
        return { ImVec4(0.753f, 0.522f, 0.988f, 1.0f),
                 ImVec4(0.659f, 0.333f, 0.969f, 0.2f), "B AT" };
    if (occasion == "A AT")
        return { ImVec4(0.784f, 0.608f, 0.235f, 1.0f),
                 ImVec4(0.784f, 0.608f, 0.235f, 0.2f), "A AT" };
    if (occasion.find("mAT") != std::string::npos ||
        occasion.find("Swiss") != std::string::npos)
        return { ImVec4(0.294f, 0.855f, 0.498f, 1.0f),
                 ImVec4(0.133f, 0.773f, 0.369f, 0.2f), occasion.c_str() };
    if (occasion.find("Scrim") != std::string::npos ||
        occasion.find("scrim") != std::string::npos)
        return { ImVec4(0.980f, 0.800f, 0.082f, 1.0f),
                 ImVec4(0.918f, 0.702f, 0.031f, 0.2f), "Scrim" };
    if (occasion.find("Unranked") != std::string::npos)
        return { ImVec4(0.431f, 0.659f, 0.996f, 1.0f),
                 ImVec4(0.431f, 0.659f, 0.996f, 0.2f), "Unranked AT" };
    return { kColorTextDim,
             ImVec4(0.5f, 0.5f, 0.5f, 0.15f), occasion.c_str() };
}

static void DrawOccasionBadge(ImDrawList* dl, const std::string& occasion, ImVec2 pos)
{
    if (occasion.empty()) return;
    auto style = GetOccasionStyle(occasion);
    ImFont* font = ImGui::GetFont();
    float fontSize = font->FontSize * 0.82f;
    ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, style.shortLabel);
    float padX = 8.f, padY = 3.f;
    ImVec2 mn(pos.x, pos.y);
    ImVec2 mx(pos.x + textSz.x + padX * 2, pos.y + textSz.y + padY * 2);
    dl->AddRectFilled(mn, mx, ImGui::GetColorU32(style.bgColor), 4.f);
    dl->AddText(font, fontSize, ImVec2(pos.x + padX, pos.y + padY),
                ImGui::GetColorU32(style.textColor), style.shortLabel);
}


static const char* MonthAbbrev(int month)
{
    static const char* months[] = {
        "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    if (month >= 1 && month <= 12) return months[month];
    return "";
}

// ─── Tournament Prep aggregation ────────────────────────────────────────────

struct TournamentBuildStats
{
    std::string buildName;
    std::string profSignature;
    std::map<std::string, int> profCounts;
    int timesPlayed = 0;
    int wins = 0;
    int losses = 0;
    float winPct = 0.0f;
    // No stored match references - drill-down recomputes on the fly
};

// Resolve a guild ID to a party ID ("1" or "2") by matching players' guild_id
static std::string ResolveGuildToParty(const MatchMeta& m, int guildId)
{
    for (const auto& [partyKey, party] : m.parties)
        for (const auto& p : party.players)
            if (p.guild_id == guildId) return partyKey;
    return "";
}

// Returns party ID ("1" or "2") if the guild is in the match, or "" if not found
static std::string FindGuildParty(const MatchMeta& m,
    const std::string& guildTag, const std::string& guildName)
{
    // Only check the two competing team guilds (keys "1" and "2").
    // Other guild entries are guest players' home guilds and should be ignored.
    for (const char* teamKey : {"1", "2"})
    {
        auto it = m.guilds.find(teamKey);
        if (it == m.guilds.end()) continue;
        const auto& gm = it->second;

        if (!guildTag.empty() && gm.tag == guildTag)
            return teamKey;
        if (!guildName.empty() && ToLower(gm.name) == ToLower(guildName))
            return teamKey;
    }

    // Folder tag fallback - only used when guilds map has no data at all
    // (e.g. cloud-only match without guild metadata)
    if (m.guilds.empty())
    {
        std::string ft1, ft2;
        ParseFolderTags(m.folder_name, ft1, ft2);
        if (!guildTag.empty())
        {
            if (ft1 == guildTag) return "1";
            if (ft2 == guildTag) return "2";
        }
    }

    return "";
}

// Scout Mode's filter window: the map chips in its own toolbar, plus the
// sidebar's date range.
//
// The date range used to be silently ignored here. DrawReplayBrowser passes
// this aggregation the UNFILTERED match list (the filtered one backs the
// table), so the calendar reached every view except the one where "what have
// they run lately" is the actual question -- a guild's numbers always spanned
// the whole archive no matter what the sidebar said.
static bool ScoutWindowAllows(const MatchMeta& m)
{
    if (!s_state.tournamentMaps.empty())
    {
        const char* mn = GetMapName(m.map_id);
        std::string mapName = mn ? mn : ("Map " + std::to_string(m.map_id));
        if (s_state.tournamentMaps.find(mapName) == s_state.tournamentMaps.end())
            return false;
    }
    if (DateValValid(s_state.dateFrom) &&
        CompareDate(m.day, m.month, m.year,
            s_state.dateFrom.day, s_state.dateFrom.month, s_state.dateFrom.year) < 0)
        return false;
    if (DateValValid(s_state.dateTo) &&
        CompareDate(m.day, m.month, m.year,
            s_state.dateTo.day, s_state.dateTo.month, s_state.dateTo.year) > 0)
        return false;
    return true;
}

static std::vector<TournamentBuildStats> AggregateTournamentBuilds(
    const std::vector<MatchMeta>& matches)
{
    std::map<std::string, TournamentBuildStats> grouped;

    for (int i = 0; i < (int)matches.size(); i++)
    {
        const auto& m = matches[i];

        if (!ScoutWindowAllows(m)) continue;

        std::string partyId = FindGuildParty(m, s_state.tournamentGuildTag,
                                              s_state.tournamentGuildName);
        if (partyId.empty()) continue;

        // Compute build for this guild's party
        auto pit = m.parties.find(partyId);
        std::string buildName;
        std::string profSig;
        std::map<std::string, int> profCounts;

        if (pit != m.parties.end() && !pit->second.players.empty())
        {
            buildName = ComputeTeamBuild(pit->second);
            for (const auto& p : pit->second.players)
                if (p.primary >= 1 && p.primary <= 10)
                    profCounts[GetProfAbbrev(p.primary)]++;
            profSig = ComputeProfSignature(profCounts);
        }

        // Skip matches with no player data (cloud-only without detail)
        if (buildName.empty()) continue;
        if (profSig.empty()) profSig = buildName;

        auto& stats = grouped[buildName];
        stats.buildName = buildName;
        stats.profSignature = profSig;
        stats.profCounts = profCounts;
        stats.timesPlayed++;

        bool won = (std::to_string(m.winner_party_id) == partyId);
        if (won) stats.wins++;
        else stats.losses++;
    }

    std::vector<TournamentBuildStats> result;
    result.reserve(grouped.size());
    for (auto& [key, val] : grouped)
    {
        val.winPct = val.timesPlayed > 0 ? (val.wins / (float)val.timesPlayed * 100.0f) : 0.0f;
        result.push_back(std::move(val));
    }

    // Default sort: most played first, then by win% descending
    std::sort(result.begin(), result.end(),
        [](const TournamentBuildStats& a, const TournamentBuildStats& b) {
            if (a.timesPlayed != b.timesPlayed) return a.timesPlayed > b.timesPlayed;
            return a.winPct > b.winPct;
        });

    return result;
}

// Aggregate builds that BEAT the selected guild (opponent builds in losses)
static std::vector<TournamentBuildStats> AggregateLostToBuilds(
    const std::vector<MatchMeta>& matches)
{
    std::map<std::string, TournamentBuildStats> grouped;

    for (int i = 0; i < (int)matches.size(); i++)
    {
        const auto& m = matches[i];

        if (!ScoutWindowAllows(m)) continue;

        std::string partyId = FindGuildParty(m, s_state.tournamentGuildTag,
                                              s_state.tournamentGuildName);
        if (partyId.empty()) continue;

        bool won = (std::to_string(m.winner_party_id) == partyId);
        if (won) continue; // only interested in losses

        // Get the opponent's build
        std::string oppPartyId = (partyId == "1") ? "2" : "1";
        auto pit = m.parties.find(oppPartyId);
        std::string buildName;
        std::string profSig;
        std::map<std::string, int> profCounts;

        if (pit != m.parties.end() && !pit->second.players.empty())
        {
            buildName = ComputeTeamBuild(pit->second);
            for (const auto& p : pit->second.players)
                if (p.primary >= 1 && p.primary <= 10)
                    profCounts[GetProfAbbrev(p.primary)]++;
            profSig = ComputeProfSignature(profCounts);
        }

        if (buildName.empty()) continue;
        if (profSig.empty()) profSig = buildName;

        auto& stats = grouped[buildName];
        stats.buildName = buildName;
        stats.profSignature = profSig;
        stats.profCounts = profCounts;
        stats.timesPlayed++;
        stats.wins++; // from the opponent's perspective, these are wins
    }

    std::vector<TournamentBuildStats> result;
    result.reserve(grouped.size());
    for (auto& [key, val] : grouped)
    {
        val.winPct = 100.0f; // all entries are wins for the opponent
        result.push_back(std::move(val));
    }

    std::sort(result.begin(), result.end(),
        [](const TournamentBuildStats& a, const TournamentBuildStats& b) {
            return a.timesPlayed > b.timesPlayed;
        });

    return result;
}

static std::vector<TournamentBuildStats> s_lostToCache;

static const std::vector<TournamentBuildStats>& GetTournamentStats(
    const std::vector<MatchMeta>& matches, int generation)
{
    std::string key = s_state.tournamentGuildTag + "|" + s_state.tournamentGuildName + "|";
    for (const auto& m : s_state.tournamentMaps) key += m + ",";
    // The date range is part of the answer, so it has to be part of the key --
    // otherwise moving the calendar reuses the previous window's numbers.
    key += "|" + std::to_string(s_state.dateFrom.year * 10000
                              + s_state.dateFrom.month * 100 + s_state.dateFrom.day)
         + "|" + std::to_string(s_state.dateTo.year * 10000
                              + s_state.dateTo.month * 100 + s_state.dateTo.day);

    if (key == s_tournamentCacheKey &&
        s_tournamentCacheMatchCount == (int)matches.size() &&
        s_tournamentCacheGeneration == generation)
        return s_tournamentCache;

    s_tournamentCache = AggregateTournamentBuilds(matches);
    s_lostToCache = AggregateLostToBuilds(matches);
    s_tournamentCacheKey = key;
    s_tournamentCacheMatchCount = (int)matches.size();
    s_tournamentCacheGeneration = generation;
    return s_tournamentCache;
}

// ─── Match filtering ─────────────────────────────────────────────────────────

struct FilteredMatch
{
    int originalIndex;
    const MatchMeta* meta;
    GuildLabel guild1;
    GuildLabel guild2;
    std::string mapName;
    std::string build1;
    std::string build2;
    std::string profSig1;
    std::string profSig2;
    std::map<std::string, int> profCounts1;
    std::map<std::string, int> profCounts2;
};


// ─── Per-match derived data ─────────────────────────────────────────────────
//
// Everything here is a pure function of one match plus the build definitions: the map name,
// the two guild labels, the two team builds and their profession signatures. None of it
// depends on the filters or on the frame, but computing it is by far the most expensive part
// of building the list - ComputeTeamBuild alone allocates a std::set of skills per player and
// a profession map per team, twice per match.
//
// It used to run for every match on every frame. Across a 2500-match library that is tens of
// thousands of heap allocations a frame, which is what made the browser window slow, left the
// row highlight trailing the mouse and set the cursor flickering between shapes.
struct MatchDerived
{
    GuildLabel  guild1, guild2;
    std::string mapName;
    std::string build1, build2;
    std::string profSig1, profSig2;
    std::map<std::string, int> profCounts1, profCounts2;
};

static std::vector<MatchDerived> s_matchDerived;
// The match vector's identity, not just its size: a rescan can reallocate it, and every
// FilteredMatch::meta points into it.
static const MatchMeta* s_derivedMatchesData = nullptr;
static size_t           s_derivedMatchCount  = 0;
static int              s_derivedBuildGen    = -1;
static int              s_derivedLibraryGen  = -1;

static void EnsureMatchDerived(const std::vector<MatchMeta>& matches)
{
    // Resolve the definitions before sampling the generation: the first ComputeTeamBuild would
    // otherwise load them midway through the rebuild and leave the cache stamped with the
    // pre-load generation.
    LoadBuildDefs();

    if (s_derivedMatchesData == matches.data() &&
        s_derivedMatchCount  == matches.size() &&
        s_derivedBuildGen    == s_buildDefsGen &&
        s_derivedLibraryGen  == s_libraryGeneration)
        return;

    s_matchDerived.clear();
    s_matchDerived.resize(matches.size());

    for (size_t i = 0; i < matches.size(); i++)
    {
        const auto& m = matches[i];
        MatchDerived& d = s_matchDerived[i];

        const char* mn = GetMapName(m.map_id);
        d.mapName = mn ? mn : ("Map " + std::to_string(m.map_id));

        std::string ft1, ft2;
        ParseFolderTags(m.folder_name, ft1, ft2);
        d.guild1 = GetPartyGuild(m, "1", ft1);
        d.guild2 = GetPartyGuild(m, "2", ft2);

        auto side = [&](const char* partyId, std::string& build,
                        std::map<std::string, int>& counts, std::string& sig) {
            auto it = m.parties.find(partyId);
            if (it == m.parties.end()) return;
            for (const auto& p : it->second.players)
                if (p.primary >= 1 && p.primary <= 10) counts[GetProfAbbrev(p.primary)]++;
            build = ComputeTeamBuild(it->second);
            sig   = ComputeProfSignature(counts);
        };
        side("1", d.build1, d.profCounts1, d.profSig1);
        side("2", d.build2, d.profCounts2, d.profSig2);
    }

    s_derivedMatchesData = matches.data();
    s_derivedMatchCount  = matches.size();
    s_derivedBuildGen    = s_buildDefsGen;
    s_derivedLibraryGen  = s_libraryGeneration;
}

static std::vector<FilteredMatch> BuildFilteredMatches(const std::vector<MatchMeta>& matches)
{
    EnsureMatchDerived(matches);

    std::vector<FilteredMatch> result;
    result.reserve(matches.size());

    std::string searchLower = ToLower(std::string(s_state.searchDebounced));

    for (int i = 0; i < (int)matches.size(); i++)
    {
        const auto& m = matches[i];
        const MatchDerived& d = s_matchDerived[i];

        // Map filter (multi-select)
        const std::string& mapName = d.mapName;

        if (!s_state.selectedMaps.empty())
        {
            if (s_state.selectedMaps.find(mapName) == s_state.selectedMaps.end())
                continue;
        }

        // Occasion filter (multi-select)
        if (!s_state.selectedOccasions.empty())
        {
            if (s_state.selectedOccasions.find(m.occasion) == s_state.selectedOccasions.end())
                continue;
        }

        // Flux filter (multi-select)
        if (!s_state.selectedFluxes.empty())
        {
            if (s_state.selectedFluxes.find(m.flux) == s_state.selectedFluxes.end())
                continue;
        }

        // Skill filter (pre-indexed, so this is a handful of binary searches)
        if (!MatchPassesSkillFilter((size_t)i))
            continue;

        // Date range filter
        if (DateValValid(s_state.dateFrom))
        {
            if (CompareDate(m.day, m.month, m.year,
                s_state.dateFrom.day, s_state.dateFrom.month, s_state.dateFrom.year) < 0)
                continue;
        }
        if (DateValValid(s_state.dateTo))
        {
            if (CompareDate(m.day, m.month, m.year,
                s_state.dateTo.day, s_state.dateTo.month, s_state.dateTo.year) > 0)
                continue;
        }

        // Rating filter
        if (s_state.minRatingFilter > 0)
        {
            int r = MatchRatings::Get().GetRating(m.folder_name);
            if (r < s_state.minRatingFilter)
                continue;
        }

        const GuildLabel& g1 = d.guild1;
        const GuildLabel& g2 = d.guild2;

        // Matchup filter — side-agnostic; with only one slot filled it degrades
        // to "this guild played in the match".
        {
            bool hasA = MatchupSlotSet(s_state.matchupNameA, s_state.matchupTagA);
            bool hasB = MatchupSlotSet(s_state.matchupNameB, s_state.matchupTagB);
            if (hasA || hasB)
            {
                auto sideIs = [](const GuildLabel& g,
                                 const std::string& name, const std::string& tag) {
                    if (name.empty() && tag.empty()) return true;   // unset slot matches anything
                    if (!tag.empty() && ToLower(g.tag) == ToLower(tag)) return true;
                    if (!name.empty() && ToLower(g.name) == ToLower(name)) return true;
                    return false;
                };
                bool ok =
                    (sideIs(g1, s_state.matchupNameA, s_state.matchupTagA) &&
                     sideIs(g2, s_state.matchupNameB, s_state.matchupTagB)) ||
                    (sideIs(g2, s_state.matchupNameA, s_state.matchupTagA) &&
                     sideIs(g1, s_state.matchupNameB, s_state.matchupTagB));
                if (!ok) continue;
            }
        }

        // Search chips. All by default, so stacking two players asks for matches they both
        // played in; Any widens it back to either of them.
        if (!s_state.selectedSearchTerms.empty())
        {
            const bool matchAll = (s_state.searchMatchMode == 0);
            bool verdict = matchAll;
            for (const auto& term : s_state.selectedSearchTerms)
            {
                std::string tLow = ToLower(term);
                bool found = false;
                found |= ToLower(g1.name).find(tLow) != std::string::npos;
                found |= ToLower(g1.tag).find(tLow) != std::string::npos;
                found |= ToLower(g1.display).find(tLow) != std::string::npos;
                found |= ToLower(g2.name).find(tLow) != std::string::npos;
                found |= ToLower(g2.tag).find(tLow) != std::string::npos;
                found |= ToLower(g2.display).find(tLow) != std::string::npos;
                for (const auto& [pid, party] : m.parties)
                {
                    for (const auto& p : party.players)
                        if (ToLower(p.encoded_name).find(tLow) != std::string::npos)
                        { found = true; break; }
                    if (found) break;
                }
                if (matchAll && !found) { verdict = false; break; }
                if (!matchAll && found) { verdict = true;  break; }
            }
            if (!verdict) continue;
        }
        else if (!searchLower.empty())
        {
            bool found = false;
            found |= ToLower(g1.name).find(searchLower) != std::string::npos;
            found |= ToLower(g1.tag).find(searchLower) != std::string::npos;
            found |= ToLower(g1.display).find(searchLower) != std::string::npos;
            found |= ToLower(g2.name).find(searchLower) != std::string::npos;
            found |= ToLower(g2.tag).find(searchLower) != std::string::npos;
            found |= ToLower(g2.display).find(searchLower) != std::string::npos;

            for (const auto& [pid, party] : m.parties)
            {
                for (const auto& p : party.players)
                {
                    if (ToLower(p.encoded_name).find(searchLower) != std::string::npos)
                    { found = true; break; }
                }
                if (found) break;
            }
            if (!found) continue;
        }

        // Build filter (multi-select)
        if (!s_state.selectedBuilds.empty())
        {
            bool match = s_state.selectedBuilds.count(d.build1)
                      || s_state.selectedBuilds.count(d.build2);
            if (!match) continue;
        }

        FilteredMatch fm;
        fm.originalIndex = i;
        fm.meta = &m;
        fm.guild1 = d.guild1;
        fm.guild2 = d.guild2;
        fm.mapName = d.mapName;
        fm.build1 = d.build1;
        fm.build2 = d.build2;
        fm.profCounts1 = d.profCounts1;
        fm.profCounts2 = d.profCounts2;
        fm.profSig1 = d.profSig1;
        fm.profSig2 = d.profSig2;
        result.push_back(std::move(fm));
    }

    // Sort
    if (!result.empty())
    {
        auto cmp = [](const FilteredMatch& a, const FilteredMatch& b, int col, bool asc) -> bool {
            int r = 0;
            switch (col)
            {
            case 0: // Date → Occasion → Map rotation
            {
                if (a.meta->year != b.meta->year)       r = a.meta->year - b.meta->year;
                else if (a.meta->month != b.meta->month) r = a.meta->month - b.meta->month;
                else if (a.meta->day != b.meta->day)     r = a.meta->day - b.meta->day;
                else {
                    int oa = GetOccasionOrder(a.meta->occasion);
                    int ob = GetOccasionOrder(b.meta->occasion);
                    if (oa != ob) r = oa - ob;
                    else {
                        int ma = GetMapRotationOrder(a.meta->month, a.mapName);
                        int mb = GetMapRotationOrder(b.meta->month, b.mapName);
                        if (ma != mb) r = ma - mb;
                        else r = a.meta->folder_name.compare(b.meta->folder_name);
                    }
                }
                break;
            }
            // These indices are the table's logical column order; see MatchCol in
            // DrawMatchListTable. The two Composition columns are NoSort, so they never
            // reach here.
            case 1: r = a.meta->occasion.compare(b.meta->occasion); break;
            case 2: r = a.mapName.compare(b.mapName); break;
            case 3: r = a.guild1.display.compare(b.guild1.display); break;
            case 5: r = a.guild2.display.compare(b.guild2.display); break;
            case 7: r = a.build1.compare(b.build1); break;
            case 8: r = a.build2.compare(b.build2); break;
            case 9: r = MatchRatings::Get().GetRating(a.meta->folder_name)
                      - MatchRatings::Get().GetRating(b.meta->folder_name); break;
            default: break;
            }
            return asc ? (r < 0) : (r > 0);
        };

        int col = s_state.sortColumn;
        bool asc = s_state.sortAscending;
        std::stable_sort(result.begin(), result.end(),
            [&](const FilteredMatch& a, const FilteredMatch& b) { return cmp(a, b, col, asc); });
    }

    return result;
}

// ─── Filtered-list cache ────────────────────────────────────────────────────
//
// The filtered, sorted list is rebuilt only when something it depends on changes. Everything
// BuildFilteredMatches reads has to be folded into the signature below; a field left out here
// is a filter that silently stops responding, so add to it whenever a filter is added.
//
// The match vector's address is part of the signature as well as its size, because every
// FilteredMatch::meta points into it and a rescan can move it.
static uint64_t FilterSignature(const std::vector<MatchMeta>& matches)
{
    uint64_t h = 1469598103934665603ull;   // FNV-1a
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    auto mixStr = [&](const std::string& s) {
        for (unsigned char c : s) mix(c);
        mix(0x5eed);
    };
    auto mixStrSet = [&](const std::set<std::string>& set) {
        mix(set.size());
        for (const auto& e : set) mixStr(e);
    };

    mix(reinterpret_cast<uintptr_t>(matches.data()));
    mix(matches.size());
    mix((uint64_t)s_libraryGeneration);
    mix((uint64_t)s_buildDefsGen);
    mix(MatchRatings::Get().Version());

    mixStrSet(s_state.selectedMaps);
    mixStrSet(s_state.selectedOccasions);
    mixStrSet(s_state.selectedFluxes);
    mixStrSet(s_state.selectedBuilds);
    mixStrSet(s_state.selectedSearchTerms);
    mix((uint64_t)s_state.searchMatchMode);
    mixStr(std::string(s_state.searchDebounced));

    mix(s_state.selectedSkills.size());
    for (int sk : s_state.selectedSkills) mix((uint64_t)sk);
    mix((uint64_t)s_state.skillMatchMode);
    mix((uint64_t)s_state.skillScope);

    mix((uint64_t)s_state.dateFrom.day);   mix((uint64_t)s_state.dateFrom.month);
    mix((uint64_t)s_state.dateFrom.year);
    mix((uint64_t)s_state.dateTo.day);     mix((uint64_t)s_state.dateTo.month);
    mix((uint64_t)s_state.dateTo.year);

    mix((uint64_t)s_state.minRatingFilter);
    mixStr(s_state.matchupNameA); mixStr(s_state.matchupTagA);
    mixStr(s_state.matchupNameB); mixStr(s_state.matchupTagB);

    mix((uint64_t)s_state.sortColumn);
    mix(s_state.sortAscending ? 1u : 0u);
    return h;
}

static const std::vector<FilteredMatch>& FilterMatches(const std::vector<MatchMeta>& matches)
{
    static std::vector<FilteredMatch> s_cache;
    static uint64_t s_cacheSig = 0;
    static bool     s_cacheValid = false;

    const uint64_t sig = FilterSignature(matches);
    if (!s_cacheValid || sig != s_cacheSig)
    {
        s_cache = BuildFilteredMatches(matches);
        s_cacheSig = sig;
        s_cacheValid = true;
    }
    return s_cache;
}


// For each occasion string, count matches that pass every OTHER active filter
// (map, flux, skill, date, rating, matchup, search, build) - the Occasion
// filter itself is deliberately skipped so the tree can show "how many
// matches would this bucket add", independent of what is already ticked.
static std::unordered_map<std::string, int> ComputeOccasionMatchCounts(
    const std::vector<MatchMeta>& matches)
{
    std::unordered_map<std::string, int> counts;

    std::string searchLower = ToLower(std::string(s_state.searchDebounced));
    bool hasMatchup = MatchupSlotSet(s_state.matchupNameA, s_state.matchupTagA) ||
                       MatchupSlotSet(s_state.matchupNameB, s_state.matchupTagB);
    bool needBuild = !s_state.selectedBuilds.empty();

    EnsureMatchDerived(matches);

    for (int i = 0; i < (int)matches.size(); i++)
    {
        const auto& m = matches[i];
        const MatchDerived& d = s_matchDerived[i];

        if (!s_state.selectedMaps.empty() &&
            s_state.selectedMaps.find(d.mapName) == s_state.selectedMaps.end())
            continue;

        if (!s_state.selectedFluxes.empty() &&
            s_state.selectedFluxes.find(m.flux) == s_state.selectedFluxes.end())
            continue;

        if (!MatchPassesSkillFilter((size_t)i))
            continue;

        if (DateValValid(s_state.dateFrom) &&
            CompareDate(m.day, m.month, m.year,
                s_state.dateFrom.day, s_state.dateFrom.month, s_state.dateFrom.year) < 0)
            continue;
        if (DateValValid(s_state.dateTo) &&
            CompareDate(m.day, m.month, m.year,
                s_state.dateTo.day, s_state.dateTo.month, s_state.dateTo.year) > 0)
            continue;

        if (s_state.minRatingFilter > 0 &&
            MatchRatings::Get().GetRating(m.folder_name) < s_state.minRatingFilter)
            continue;

        const GuildLabel& g1 = d.guild1;
        const GuildLabel& g2 = d.guild2;

        if (hasMatchup)
        {
            auto sideIs = [](const GuildLabel& g,
                             const std::string& name, const std::string& tag) {
                if (name.empty() && tag.empty()) return true;
                if (!tag.empty() && ToLower(g.tag) == ToLower(tag)) return true;
                if (!name.empty() && ToLower(g.name) == ToLower(name)) return true;
                return false;
            };
            bool ok =
                (sideIs(g1, s_state.matchupNameA, s_state.matchupTagA) &&
                 sideIs(g2, s_state.matchupNameB, s_state.matchupTagB)) ||
                (sideIs(g2, s_state.matchupNameA, s_state.matchupTagA) &&
                 sideIs(g1, s_state.matchupNameB, s_state.matchupTagB));
            if (!ok) continue;
        }

        // Same rule as FilterMatches; the two must agree or the counts lie.
        if (!s_state.selectedSearchTerms.empty())
        {
            const bool matchAll = (s_state.searchMatchMode == 0);
            bool verdict = matchAll;
            for (const auto& term : s_state.selectedSearchTerms)
            {
                std::string tLow = ToLower(term);
                bool found = false;
                found |= ToLower(g1.name).find(tLow) != std::string::npos;
                found |= ToLower(g1.tag).find(tLow) != std::string::npos;
                found |= ToLower(g1.display).find(tLow) != std::string::npos;
                found |= ToLower(g2.name).find(tLow) != std::string::npos;
                found |= ToLower(g2.tag).find(tLow) != std::string::npos;
                found |= ToLower(g2.display).find(tLow) != std::string::npos;
                for (const auto& [pid, party] : m.parties)
                {
                    for (const auto& p : party.players)
                        if (ToLower(p.encoded_name).find(tLow) != std::string::npos)
                        { found = true; break; }
                    if (found) break;
                }
                if (matchAll && !found) { verdict = false; break; }
                if (!matchAll && found) { verdict = true;  break; }
            }
            if (!verdict) continue;
        }
        else if (!searchLower.empty())
        {
            bool found = false;
            found |= ToLower(g1.name).find(searchLower) != std::string::npos;
            found |= ToLower(g1.tag).find(searchLower) != std::string::npos;
            found |= ToLower(g1.display).find(searchLower) != std::string::npos;
            found |= ToLower(g2.name).find(searchLower) != std::string::npos;
            found |= ToLower(g2.tag).find(searchLower) != std::string::npos;
            found |= ToLower(g2.display).find(searchLower) != std::string::npos;
            for (const auto& [pid, party] : m.parties)
            {
                for (const auto& p : party.players)
                    if (ToLower(p.encoded_name).find(searchLower) != std::string::npos)
                    { found = true; break; }
                if (found) break;
            }
            if (!found) continue;
        }

        if (needBuild)
        {
            if (!s_state.selectedBuilds.count(d.build1) &&
                !s_state.selectedBuilds.count(d.build2))
                continue;
        }

        counts[m.occasion]++;
    }

    return counts;
}

// ─── Left filter panel ──────────────────────────────────────────────────────

static int CountActiveFilters()
{
    int n = 0;
    if (!s_state.selectedSearchTerms.empty() || s_state.searchDebounced[0] != '\0') n++;
    if (!s_state.selectedMaps.empty()) n++;
    if (!s_state.selectedFluxes.empty()) n++;
    if (!s_state.selectedOccasions.empty()) n++;
    if (!s_state.selectedBuilds.empty()) n++;
    if (!s_state.selectedSkills.empty()) n++;
    if (MatchupSlotSet(s_state.matchupNameA, s_state.matchupTagA) ||
        MatchupSlotSet(s_state.matchupNameB, s_state.matchupTagB)) n++;
    if (DateValValid(s_state.dateFrom) || DateValValid(s_state.dateTo)) n++;
    if (s_state.minRatingFilter > 0) n++;
    if (s_state.tournamentMode) n++;
    return n;
}

static void DrawFilterPanelCollapsed(const std::vector<MatchMeta>& matches, float panelH)
{
    const float sp = 8.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp, sp));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);

    ImGui::BeginChild("##filter_collapsed", ImVec2(40, panelH), ImGuiChildFlags_Border);

    if (ImGui::Button(">", ImVec2(24, 24)))
        s_state.sidebarExpanded = true;

    ImGui::Spacing();

    int active = CountActiveFilters();
    if (active > 0)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
        ImGui::Text("%d", active);
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
    ImGui::Text("%d", (int)matches.size());
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

static void DrawFilterPanelExpanded(const std::vector<MatchMeta>& matches, float panelH, float filterW)
{
    const float sp = 8.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp + 4, sp + 4));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);

    ImGui::BeginChild("##filter_panel", ImVec2(filterW, panelH), ImGuiChildFlags_Border);

    if (ImGui::Button("<", ImVec2(24, 24)))
        s_state.sidebarExpanded = false;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
    ImGui::TextUnformatted("FILTERS");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Clear all filters ──
    //
    // Red only while something is actually filtered: with an empty sidebar the button does
    // nothing, and a permanent red block would be the loudest thing on screen for no reason.
    // A low-alpha tint rather than a solid fill, which is how the accent is used elsewhere.
    const bool anyFilters = CountActiveFilters() > 0;

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    if (anyFilters)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.86f, 0.31f, 0.29f, 0.16f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.31f, 0.29f, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.86f, 0.31f, 0.29f, 0.42f));
        ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.86f, 0.36f, 0.33f, 0.50f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.96f, 0.65f, 0.61f, 1.00f));
    }
    if (ImGui::Button("Clear All Filters", ImVec2(-1, 0)))
    {
        s_state.searchBuf[0] = '\0';
        s_state.searchDebounced[0] = '\0';
        s_state.lastSearchEditTime = -1.0f;
        s_state.selectedSearchTerms.clear();
        s_state.selectedMaps.clear();
        s_state.selectedFluxes.clear();
        s_state.selectedOccasions.clear();
        s_state.selectedBuilds.clear();
        s_state.selectedSkills.clear();
        s_state.mapSearchBuf[0] = '\0';
        s_state.fluxSearchBuf[0] = '\0';
        s_state.buildSearchBuf[0] = '\0';
        s_state.skillSearchBuf[0] = '\0';
        s_state.skillMatchMode = 0;
        s_state.skillScope = 2;
        s_state.browseOpenId.clear();
        s_state.matchupBufA[0] = '\0';
        s_state.matchupBufB[0] = '\0';
        s_state.matchupDisplayA.clear();
        s_state.matchupNameA.clear();
        s_state.matchupTagA.clear();
        s_state.matchupDisplayB.clear();
        s_state.matchupNameB.clear();
        s_state.matchupTagB.clear();
        s_state.dateFrom = {};
        s_state.dateTo = {};
        s_state.calBrowseFromMonth = 0; s_state.calBrowseFromYear = 0;
        s_state.calBrowseToMonth = 0; s_state.calBrowseToYear = 0;
        s_state.minRatingFilter = 0;
        s_state.tournamentMode = false;
        s_state.tournamentGuildBuf[0] = '\0';
        s_state.tournamentGuildDisplay.clear();
        s_state.tournamentGuildTag.clear();
        s_state.tournamentGuildName.clear();
        s_state.tournamentMaps.clear();
        s_state.tournamentMapBuf[0] = '\0';
        s_state.tournamentSelectedBuild = -1;
        s_state.tournamentSelectedLostTo = -1;
        s_state.tournamentLostToSearch[0] = '\0';
    }
    if (anyFilters)
    {
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar();
    }
    ImGui::PopStyleVar();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Global search with auto-complete chips ──
    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::TextUnformatted("Search");
    ImGui::PopStyleColor();

    if (!s_state.selectedSearchTerms.empty())
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
        if (ImGui::SmallButton("Clear##search_clear"))
            s_state.selectedSearchTerms.clear();
        ImGui::PopStyleColor();

        // Only means anything with two or more chips, so it stays out of the way until then.
        if (s_state.selectedSearchTerms.size() > 1)
        {
            ImGui::SameLine(0, 10);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 1));
            const char* modes[2] = { "All", "Any" };
            for (int i = 0; i < 2; i++)
            {
                if (i) ImGui::SameLine(0, 3);
                const bool on = (s_state.searchMatchMode == i);
                ImGui::PushStyleColor(ImGuiCol_Button, on ? kColorSelected : ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text,   on ? kColorAccent : kColorTextDim);
                if (ImGui::SmallButton((std::string(modes[i]) + "##srchmode").c_str()))
                    s_state.searchMatchMode = i;
                ImGui::PopStyleColor(2);
            }
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("All: matches featuring every term.\nAny: matches featuring at least one.");
        }

        float maxLineX = ImGui::GetContentRegionMax().x;
        float lineX = 0.0f;
        std::string toRemove;

        for (const auto& term : s_state.selectedSearchTerms)
        {
            std::string chipLabel = term + "  x##srm_" + term;
            ImVec2 chipSz = ImGui::CalcTextSize(chipLabel.c_str());
            chipSz.x += 16.0f;
            chipSz.y += 6.0f;

            if (lineX > 0.0f && lineX + chipSz.x + 4.0f < maxLineX)
                ImGui::SameLine(0, 4.0f);
            else
                lineX = 0.0f;

            ImGui::PushStyleColor(ImGuiCol_Button, kColorSelected);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorHover);
            ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
            if (ImGui::SmallButton(chipLabel.c_str()))
                toRemove = term;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);

            lineX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        }

        if (!toRemove.empty())
            s_state.selectedSearchTerms.erase(toRemove);
    }

    ImGui::SetNextItemWidth(-1);
    bool searchEdited = ImGui::InputTextWithHint("##search",
        "Player, guild, tag...", s_state.searchBuf, sizeof(s_state.searchBuf));
    ImVec2 searchMin = ImGui::GetItemRectMin();
    ImVec2 searchMax = ImGui::GetItemRectMax();
    if (searchEdited)
        s_state.lastSearchEditTime = (float)ImGui::GetTime();

    if (s_state.searchBuf[0] != '\0' &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
    {
        s_state.selectedSearchTerms.insert(std::string(s_state.searchBuf));
        s_state.searchBuf[0] = '\0';
        s_state.searchDebounced[0] = '\0';
        s_state.lastSearchEditTime = -1.0f;
    }

    if (s_state.searchBuf[0] != '\0')
        DrawGlobalSearchAutoComplete(searchMin, searchMax);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Skill filter ──
    DrawSkillFilter();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Matchup filter (guild A vs guild B) ──
    DrawMatchupFilter();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Map filter (multi-select) ──
    DrawMultiSelectFilter("Map", "Search maps...",
        s_state.mapSearchBuf, sizeof(s_state.mapSearchBuf),
        s_state.mapNames, s_state.selectedMaps, "map", false);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Flux filter (multi-select, fuzzy) ──
    DrawMultiSelectFilter("Flux", "Search flux...",
        s_state.fluxSearchBuf, sizeof(s_state.fluxSearchBuf),
        s_state.fluxNames, s_state.selectedFluxes, "flux", true,
        MsRowStyle::Flux);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Occasion filter (two-level tree: group + individual stages) ──
    DrawOccasionTreeFilter(ComputeOccasionMatchCounts(matches));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Build composition filter (multi-select) ──
    DrawMultiSelectFilter("Build", "Search builds...",
        s_state.buildSearchBuf, sizeof(s_state.buildSearchBuf),
        s_state.buildNames, s_state.selectedBuilds, "build", true);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Date range filter ──
    DrawDateRangeFilter();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Min Rating filter ──
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::TextUnformatted("Min Rating");
    ImGui::PopStyleColor();
    if (s_state.minRatingFilter > 0)
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
        if (ImGui::SmallButton("Clear##rating_clear"))
            s_state.minRatingFilter = 0;
        ImGui::PopStyleColor();
    }
    {
        int res = DrawStarRating("##minRating", s_state.minRatingFilter);
        if (res > 0) s_state.minRatingFilter = res;
        else if (res == -1) s_state.minRatingFilter = 0;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
    ImGui::Text("%d matches loaded", (int)matches.size());
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

static void DrawFilterPanel(const std::vector<MatchMeta>& matches, float panelH = 0, float filterW = 0)
{
    if (s_state.sidebarExpanded)
        DrawFilterPanelExpanded(matches, panelH, filterW);
    else
        DrawFilterPanelCollapsed(matches, panelH);
}

// ─── Team colour ribbon ──────────────────────────────────────────────────────
//
// Every match here is one side against the other, but nothing in the library said which guild
// held Blue and which held Red - you had to open the replay to find out. A thin bar in the
// side's own colour, beside the guild name, says it without spending a word or a line: the
// same mark on the gallery card, the compact card list and both detail headers. The match
// table is left alone - its TEAM 1 / TEAM 2 column headings already say it, once, for every
// row at the same time.
//
// Party "1" is Blue and party "2" is Red. That mapping lives in TeamColors.h, so a slot is
// what gets passed around here and the two colours are never written against a literal 1 or 2.
static constexpr float kTeamRibbonW = 3.0f;

// Deliberately not theme colours. A side's colour is the game's own and has to read the same
// in every theme; these are the saturated pair the minimap and the morale graph already use.
static ImU32 TeamRibbonColor(int slot01)
{
    return Team::IndexIsRed(slot01) ? IM_COL32(0xFF, 0x6B, 0x6B, 0xFF)
                                    : IM_COL32(0x4A, 0xA8, 0xFF, 0xFF);
}

// A rounded bar with a faint halo, so it marks the name it stands beside rather than reading
// as a table rule or a panel border.
static void DrawTeamRibbon(ImDrawList* dl, ImVec2 topLeft, float h, int slot01)
{
    const ImU32 col  = TeamRibbonColor(slot01);
    const ImU32 halo = (col & ~(0xFFu << IM_COL32_A_SHIFT)) | (44u << IM_COL32_A_SHIFT);
    const ImVec2 br(topLeft.x + kTeamRibbonW, topLeft.y + h);
    dl->AddRectFilled(ImVec2(topLeft.x - 1.5f, topLeft.y - 1.5f),
                      ImVec2(br.x + 1.5f, br.y + 1.5f), halo, (kTeamRibbonW + 3.0f) * 0.5f);
    dl->AddRectFilled(topLeft, br, col, kTeamRibbonW * 0.5f);
}

// The same ribbon as a laid-out item, for the rows built out of ImGui widgets rather than
// draw calls. It stands a little taller than the text but claims only the line's own height,
// so putting one in front of a name never grows the row.
static void TeamRibbonItem(float h, int slot01, bool withTooltip)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float lineH = ImGui::GetTextLineHeight();
    DrawTeamRibbon(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y - (h - lineH) * 0.5f), h, slot01);
    ImGui::Dummy(ImVec2(kTeamRibbonW, lineH));
    if (withTooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s team", Team::Name(Team::FromIndex01(slot01)));
}

// ─── Match list: card mode for mobile ────────────────────────────────────────

static void DrawMatchCards(const std::vector<FilteredMatch>& filtered,
                           const ResponsiveSizes& sz)
{
    ImTextureID cupTex = GetCupIcon();

    for (int row = 0; row < (int)filtered.size(); row++)
    {
        const auto& fm = filtered[row];
        const auto& m = *fm.meta;
        bool isSelected = (s_state.selectedMatchIndex == fm.originalIndex);

        ImGui::PushID(fm.originalIndex);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, isSelected ? kColorSelected : kColorPanelLight);

        float cardH = 72.0f;
        ImGui::BeginChild(("##card_" + std::to_string(fm.originalIndex)).c_str(),
            ImVec2(-1, cardH), ImGuiChildFlags_Border);

        char dateBuf[16];
        snprintf(dateBuf, sizeof(dateBuf), "%04d/%02d/%02d", m.year, m.month, m.day);
        ImGui::TextColored(kColorTextDim, "%s  |  %s", dateBuf, fm.mapName.c_str());

        if (!m.occasion.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(kColorTextDim, " |  %s", m.occasion.c_str());
        }

        // Team line
        TeamRibbonItem(ImGui::GetTextLineHeight() + 2.0f, 0, false);
        ImGui::SameLine(0, 6);
        ImGui::TextUnformatted(fm.guild1.display.c_str());
        if (m.winner_party_id == 1 && cupTex)
        {
            ImGui::SameLine(0, 4);
            ImGui::Image(cupTex, ImVec2(sz.cupIcon, sz.cupIcon));
        }
        ImGui::SameLine(0, 8);
        ImGui::TextColored(kColorTextDim, "vs");
        ImGui::SameLine(0, 8);
        TeamRibbonItem(ImGui::GetTextLineHeight() + 2.0f, 1, false);
        ImGui::SameLine(0, 6);
        ImGui::TextUnformatted(fm.guild2.display.c_str());
        if (m.winner_party_id == 2 && cupTex)
        {
            ImGui::SameLine(0, 4);
            ImGui::Image(cupTex, ImVec2(sz.cupIcon, sz.cupIcon));
        }

        // Click to select
        ImGui::SetCursorPos(ImVec2(0, 0));
        if (ImGui::InvisibleButton("##sel", ImVec2(-1, cardH)))
        {
            if (isSelected)
                s_state.selectedMatchIndex = -1;
            else
            {
                s_state.selectedMatchIndex = fm.originalIndex;
                s_state.mobileShowDetail = true;
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

// ─── Card Gallery ────────────────────────────────────────────────────────────

// Forward declaration (defined later in file)
static void DrawMatchDetailPanel(const MatchMeta& m, bool fillRemaining = false);

// ─── Handoff card: renders one team side (left or right) ────────────────────

static float DrawTeamSide(ImDrawList* dl, const FilteredMatch& fm, bool isTeam2,
                           float areaX, float areaY, float areaW, float maxH,
                           bool won, const std::string& profSig, const std::string& buildName)
{
    ImFont* font = ImGui::GetFont();
    ImFont* boldFnt = GuiGlobalConstants::boldFont;
    ImFont* mono = GuiGlobalConstants::monoFont ? GuiGlobalConstants::monoFont : font;
    ImFont* monoBold = GuiGlobalConstants::monoBoldFont ? GuiGlobalConstants::monoBoldFont : (boldFnt ? boldFnt : font);
    float curY = areaY;

    const auto& guild = isTeam2 ? fm.guild2 : fm.guild1;
    ImU32 nameCol = won ? IM_COL32(251, 191, 36, 255) : IM_COL32(161, 161, 170, 255);
    ImFont* teamNameFont = (won && boldFnt) ? boldFnt : font;
    ImU32 tagCol = IM_COL32(245, 158, 11, 184); // amber @72%

    // Winner tint background
    if (won)
    {
        dl->AddRectFilledMultiColor(
            ImVec2(areaX, areaY), ImVec2(areaX + areaW, areaY + maxH),
            IM_COL32(245, 158, 11, 25), IM_COL32(245, 158, 11, 25),
            IM_COL32(245, 158, 11, 8), IM_COL32(245, 158, 11, 8));
    }

    // Guild name: 16px (§3b revised), tag: 15.5px mono
    float nameSz = 16.f;
    float tagSz  = 15.5f;
    {
        char tagBuf[32] = "";
        if (!guild.tag.empty()) snprintf(tagBuf, sizeof(tagBuf), "[%s]", guild.tag.c_str());

        // The ribbon rides the outer edge of the team's own half, so the two colours frame
        // the matchup instead of crowding the VS rule down the middle. It spans the name and
        // the tag together - a mark for the team, not for one line of it.
        const int   slot01  = isTeam2 ? 1 : 0;
        const float ribbonH = nameSz + tagSz + 2.f;
        const float inset   = kTeamRibbonW + 7.f;
        DrawTeamRibbon(dl, ImVec2(isTeam2 ? (areaX + areaW - kTeamRibbonW) : areaX, curY + 1.f),
                       ribbonH, slot01);

        if (isTeam2)
        {
            float x = areaX + areaW - inset;
            dl->PushClipRect(ImVec2(areaX, curY), ImVec2(x, curY + nameSz + tagSz + 4.f), true);
            ImVec2 nSz = teamNameFont->CalcTextSizeA(nameSz, FLT_MAX, 0.f, guild.name.c_str());
            dl->AddText(teamNameFont, nameSz, ImVec2(std::max(areaX, x - nSz.x), curY), nameCol, guild.name.c_str());
            if (tagBuf[0])
            {
                ImVec2 tSz = mono->CalcTextSizeA(tagSz, FLT_MAX, 0.f, tagBuf);
                dl->AddText(mono, tagSz, ImVec2(std::max(areaX, x - tSz.x), curY + nameSz + 1.f), tagCol, tagBuf);
            }
            dl->PopClipRect();
        }
        else
        {
            const float textX = areaX + inset;
            dl->PushClipRect(ImVec2(textX, curY), ImVec2(areaX + areaW, curY + nameSz + tagSz + 4.f), true);
            dl->AddText(teamNameFont, nameSz, ImVec2(textX, curY), nameCol, guild.name.c_str());
            if (tagBuf[0])
                dl->AddText(mono, tagSz, ImVec2(textX, curY + nameSz + 1.f), tagCol, tagBuf);
            dl->PopClipRect();
        }
    }
    curY += nameSz + tagSz + 5.f;

    // Profession icons (20px) + comp count (14px mono bold)
    {
        auto tokens = ParseCompString(profSig);
        float iconSz = 20.f;
        float compFontSz = 14.f;
        float gap = 10.f;

        float totalW = 0;
        for (auto& t : tokens)
        {
            char countBuf[8]; snprintf(countBuf, sizeof(countBuf), "%d", t.count);
            totalW += iconSz + 3.f + monoBold->CalcTextSizeA(compFontSz, FLT_MAX, 0.f, countBuf).x + gap;
        }
        if (!tokens.empty()) totalW -= gap;

        float startX = isTeam2 ? (areaX + areaW - std::max(0.f, totalW)) : areaX;
        float x = startX;
        for (auto& t : tokens)
        {
            ImTextureID ico = GetProfessionIcon(t.profId);
            if (ico)
                dl->AddImage(ico, ImVec2(x, curY), ImVec2(x + iconSz, curY + iconSz));
            x += iconSz + 3.f;
            char countBuf[8]; snprintf(countBuf, sizeof(countBuf), "%d", t.count);
            dl->AddText(monoBold, compFontSz, ImVec2(x, curY + 2.f),
                        IM_COL32(212, 212, 216, 255), countBuf);
            x += monoBold->CalcTextSizeA(compFontSz, FLT_MAX, 0.f, countBuf).x + gap;
        }
        curY += iconSz + 3.f;
    }

    // Strategy pill: 13.5px mono (§3b revised)
    bool hasBuild = !buildName.empty() && buildName != profSig;
    if (hasBuild)
    {
        float pillSz = 13.5f;
        ImVec2 pillTextSz = mono->CalcTextSizeA(pillSz, FLT_MAX, 0.f, buildName.c_str());
        float pillW = pillTextSz.x + 16.f;
        float pillH = pillTextSz.y + 4.f;
        float pillX = isTeam2 ? (areaX + areaW - pillW) : areaX;

        dl->AddRect(ImVec2(pillX, curY), ImVec2(pillX + pillW, curY + pillH),
                    IM_COL32(245, 158, 11, 100), 3.f);
        dl->AddText(mono, pillSz, ImVec2(pillX + 8.f, curY + 2.f),
                    IM_COL32(245, 158, 11, 255), buildName.c_str());
        curY += pillH + 2.f;
    }

    return curY - areaY;
}

static void DrawGalleryCard(const FilteredMatch& fm, float cardWidth, bool isSelected)
{
    const auto& m = *fm.meta;
    const float cardH = 215.f;
    const float pad = 12.f;

    ImGui::PushID(fm.originalIndex);

    ImVec4 borderCol = isSelected ? s_themeColors.cardBorderSel : s_themeColors.cardBorderIdle;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.110f, 0.110f, 0.122f, 1.0f)); // #1c1c1f
    ImGui::PushStyleColor(ImGuiCol_Border, borderCol);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);

    ImGui::BeginChild(("##gcard" + std::to_string(fm.originalIndex)).c_str(),
                      ImVec2(cardWidth, cardH), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    ImFont* font = ImGui::GetFont();
    ImFont* boldFnt = GuiGlobalConstants::boldFont;
    ImFont* nameFont = boldFnt ? boldFnt : font;
    ImFont* mono = GuiGlobalConstants::monoFont ? GuiGlobalConstants::monoFont : font;
    ImFont* monoBold = GuiGlobalConstants::monoBoldFont ? GuiGlobalConstants::monoBoldFont : nameFont;
    float rightEdge = winPos.x + cardWidth - pad;

    // ---- Card head: dark inset with map + metadata + rank badge ----
    float thumbW = 135.f, thumbH = 86.f; // handoff: 135x86
    float headH = thumbH + 16.f; // 8px padding top+bottom
    {
        ImVec2 headMin(winPos.x, winPos.y);
        ImVec2 headMax(winPos.x + cardWidth, winPos.y + headH);
        dl->AddRectFilled(headMin, headMax, IM_COL32(22, 22, 24, 255), 8.f,
                          ImDrawFlags_RoundCornersTop); // #161618

        float thumbX = winPos.x + pad, thumbY = winPos.y + 8.f;
        ImTextureID mapThumb = GetMapIcon(m.map_id);
        if (mapThumb)
        {
            dl->AddImageRounded(mapThumb,
                ImVec2(thumbX, thumbY), ImVec2(thumbX + thumbW, thumbY + thumbH),
                ImVec2(0, 0), ImVec2(1, 1),
                IM_COL32(255, 255, 255, 255), 6.f);
            dl->AddRect(ImVec2(thumbX, thumbY), ImVec2(thumbX + thumbW, thumbY + thumbH),
                        IM_COL32(63, 63, 70, 255), 6.f, 0, 3.f); // 3px border #3f3f46
        }
        else
        {
            dl->AddRectFilled(ImVec2(thumbX, thumbY), ImVec2(thumbX + thumbW, thumbY + thumbH),
                              IM_COL32(38, 38, 42, 255), 6.f); // bg #26262a
            dl->AddRect(ImVec2(thumbX, thumbY), ImVec2(thumbX + thumbW, thumbY + thumbH),
                        IM_COL32(63, 63, 70, 255), 6.f, 0, 3.f);
        }

        // Map name: 17px weight 600 #d4d4d8
        float textX = thumbX + thumbW + 10.f;
        dl->PushClipRect(ImVec2(textX, winPos.y), ImVec2(rightEdge - 80.f, winPos.y + headH), true);
        dl->AddText(nameFont, 17.f, ImVec2(textX, thumbY + 2.f),
                    IM_COL32(212, 212, 216, 255), fm.mapName.c_str());
        dl->PopClipRect();

        // Date: 16px mono #a1a1aa
        char dateBuf[32];
        snprintf(dateBuf, sizeof(dateBuf), "%s %d, %04d", MonthAbbrev(m.month), m.day, m.year);
        dl->AddText(mono, 16.f, ImVec2(textX, thumbY + 24.f),
                    IM_COL32(161, 161, 170, 255), dateBuf);

        // Duration + rank badge — same row, vertically centered in head, right-aligned
        // HTML: margin-left:auto; display:flex; align-items:center; gap:8px
        {
            float rowY = thumbY + (thumbH - 20.f) * 0.5f; // vertically center ~20px content in thumb area
            float rx = rightEdge;

            // Rank badge first (rightmost)
            if (!m.occasion.empty())
            {
                auto rs = GetRankStyle(m.occasion);
                ImVec2 badgeSz = monoBold->CalcTextSizeA(12.f, FLT_MAX, 0.f, m.occasion.c_str());
                float bw = badgeSz.x + 14.f, bh = badgeSz.y + 6.f;
                float bx = rx - bw, by = rowY + (20.f - bh) * 0.5f;
                if (rs.bg) dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), rs.bg, 4.f);
                if (rs.border) dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), rs.border, 4.f);
                dl->AddText(monoBold, 12.f, ImVec2(bx + 7.f, by + 3.f), rs.fg, m.occasion.c_str());
                rx = bx - 8.f; // gap:8px per HTML
            }

            // Duration (left of badge, same row) — 18px
            if (!m.match_duration.empty())
            {
                ImVec2 durSz = mono->CalcTextSizeA(18.f, FLT_MAX, 0.f, m.match_duration.c_str());
                float dx = rx - durSz.x;
                dl->AddText(mono, 18.f, ImVec2(dx, rowY),
                            IM_COL32(212, 212, 216, 255), m.match_duration.c_str());
                dl->AddText(font, 18.f, ImVec2(dx - 20.f, rowY),
                            IM_COL32(245, 158, 11, 255), "\xe2\x97\xb7");
            }
        }

        // Bottom border of head
        dl->AddLine(ImVec2(winPos.x, winPos.y + headH),
                    ImVec2(winPos.x + cardWidth, winPos.y + headH),
                    IM_COL32(33, 33, 36, 255));
    }

    // ---- Matchup body: left team | VS | right team ----
    float footerH = 24.f;
    float bodyY = winPos.y + headH + 1.f;
    float bodyH = cardH - headH - footerH - 1.f; // content-driven now (195 - 102 - 24 - 1 = 68px)
    {
        float vsCenterX = winPos.x + cardWidth * 0.5f;
        float vsW = 20.f;
        float teamW = (cardWidth - vsW) * 0.5f - pad - 12.f; // 12px right padding for comp overflow

        // VS divider: absolute positioning, not proportional
        float vsX = vsCenterX;
        float vsMidY = bodyY + bodyH * 0.5f;
        dl->AddLine(ImVec2(vsX, bodyY + 4.f), ImVec2(vsX, vsMidY - 12.f),
                    IM_COL32(39, 39, 42, 255));
        float vsFontSz = 15.f; // §3b revised: 15px mono bold
        ImVec2 vsSz = monoBold->CalcTextSizeA(vsFontSz, FLT_MAX, 0.f, "VS");
        dl->AddText(monoBold, vsFontSz,
                    ImVec2(vsX - vsSz.x * 0.5f, vsMidY - vsSz.y * 0.5f),
                    IM_COL32(245, 158, 11, 255), "VS");
        dl->AddLine(ImVec2(vsX, vsMidY + 12.f), ImVec2(vsX, bodyY + bodyH - 4.f),
                    IM_COL32(39, 39, 42, 255));

        bool won1 = (m.winner_party_id == 1);
        bool won2 = (m.winner_party_id == 2);

        // Left team - top-aligned, no centering
        DrawTeamSide(dl, fm, false,
            winPos.x + pad, bodyY + 4.f, teamW, bodyH - 8.f,
            won1, fm.profSig1, fm.build1);

        // Right team - top-aligned, no centering
        DrawTeamSide(dl, fm, true,
            vsCenterX + vsW * 0.5f, bodyY + 4.f, teamW, bodyH - 8.f,
            won2, fm.profSig2, fm.build2);
    }

    // ---- Footer ----
    {
        float footerY = winPos.y + cardH - footerH;
        dl->AddLine(ImVec2(winPos.x, footerY),
                    ImVec2(winPos.x + cardWidth, footerY),
                    IM_COL32(33, 33, 36, 255));

        float detailSz = 16.f; // §3b revised: 16px
        const char* detailText = "Details >";
        ImVec2 dtSz = font->CalcTextSizeA(detailSz, FLT_MAX, 0.f, detailText);
        dl->AddText(font, detailSz,
                    ImVec2(rightEdge - dtSz.x, footerY + 5.f),
                    IM_COL32(161, 161, 170, 255), detailText);
    }

    // ---- Click overlay ----
    ImGui::SetCursorPos(ImVec2(0, 0));
    if (ImGui::InvisibleButton("##sel", ImVec2(cardWidth, cardH)))
    {
        if (isSelected)
            s_state.selectedMatchIndex = -1;
        else
            s_state.selectedMatchIndex = fm.originalIndex;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !g_cloudDownloadInProgress)
    {
        s_state.selectedMatchIndex = fm.originalIndex;
        g_pendingReplay.requested = true;
        g_pendingReplay.match = m;
    }
    if (ImGui::IsItemHovered() && !isSelected)
    {
        ImVec2 mn = winPos;
        ImVec2 mx(winPos.x + cardWidth, winPos.y + cardH);
        dl->AddRectFilled(mn, mx, IM_COL32(245, 158, 11, 6), 8.f);
        dl->AddRect(mn, mx, IM_COL32(245, 158, 11, 60), 8.f, 0, 1.5f);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

// ─── Card gallery detail panel (handoff v2 design) ──────────────────────────

static void DrawGalleryDetailTeam(const MatchMeta& m, const std::string& partyId,
                                   const GuildLabel& guild, bool isWinner, float panelW)
{
    ImFont* font = ImGui::GetFont();
    ImFont* boldFnt = GuiGlobalConstants::boldFont;
    ImFont* nameFont = boldFnt ? boldFnt : font;
    ImFont* mono = GuiGlobalConstants::monoFont ? GuiGlobalConstants::monoFont : font;

    // Winner amber gradient bg
    if (isWinner)
    {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
            p0, ImVec2(p0.x + panelW, p0.y + 80),
            IM_COL32(245, 158, 11, 18), IM_COL32(245, 158, 11, 18),
            IM_COL32(245, 158, 11, 5), IM_COL32(245, 158, 11, 5));
    }

    // Team name + [tag] + WON chip, led by the side's colour
    TeamRibbonItem(ImGui::GetTextLineHeight() + 3.0f, (partyId == "2") ? 1 : 0, true);
    ImGui::SameLine(0, 8);
    if (boldFnt) ImGui::PushFont(boldFnt);
    ImGui::TextColored(isWinner ? ImVec4(0.984f, 0.749f, 0.141f, 1.f) // #fbbf24
                                : ImVec4(0.831f, 0.831f, 0.847f, 1.f), // #d4d4d8
                       "%s", guild.display.c_str());
    if (boldFnt) ImGui::PopFont();
    if (isWinner)
    {
        ImGui::SameLine(0, 8);
        ImVec2 cp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(cp, ImVec2(cp.x + 30, cp.y + 14),
            IM_COL32(245, 158, 11, 255), 3.f);
        if (boldFnt) ImGui::PushFont(boldFnt);
        ImGui::GetWindowDrawList()->AddText(nameFont, 9.f,
            ImVec2(cp.x + 4, cp.y + 2), IM_COL32(24, 24, 27, 255), "WON");
        if (boldFnt) ImGui::PopFont();
        ImGui::Dummy(ImVec2(30, 14));
    }
    ImGui::Spacing();

    auto pit = m.parties.find(partyId);
    if (pit == m.parties.end()) { ImGui::TextColored(ImVec4(0.443f, 0.443f, 0.478f, 1.f), "No player data"); return; }

    // Sort players
    std::vector<const PlayerMeta*> sorted;
    for (const auto& p : pit->second.players) sorted.push_back(&p);
    std::sort(sorted.begin(), sorted.end(),
        [](const PlayerMeta* a, const PlayerMeta* b) { return a->player_number < b->player_number; });

    // Compute totals
    int totK = 0, totD = 0, totDmg = 0, totInt = 0, totCnc = 0, totSkl = 0;
    bool totalAvailable[] = { true, true, true, true, true, true };
    for (auto* p : sorted) {
        totK += p->kills; totD += p->deaths; totDmg += p->total_damage;
        totInt += p->interrupted_count; totCnc += p->cancelled_skills_count;
        totSkl += p->skills_finished;
        totalAvailable[3] &= (p->preview_stats_available & PreviewInterrupted) != 0;
        totalAvailable[4] &= (p->preview_stats_available & PreviewCancelledSkills) != 0;
        totalAvailable[5] &= (p->preview_stats_available & PreviewSkillsFinished) != 0;
    }

    struct SC { const char* label; const char* tooltip; int total; };
    SC cols[] = { {"K","Kills",totK}, {"D","Deaths",totD}, {"DMG","Damage",totDmg},
                  {"INT","Interrupts",totInt}, {"CNC","Cancelled",totCnc}, {"SKL","Skills Used",totSkl} };

    // Use ImGui table for proper column alignment (same approach as table view)
    float availW = ImGui::GetContentRegionAvail().x;
    float iconSize = 22.f;
    float skillIconSize = 24.f;
    bool showSkills = (availW >= 500.f);
    float nameColW = showSkills ? std::max(80.f, availW * 0.16f) : std::max(80.f, availW * 0.28f);
    float statColW = 38.f;
    float dmgColW = 46.f;
    float copyBtnW = 20.f;

    int numCols = 3 + (showSkills ? 1 : 0) + 1 + 6; // prof, name, copy, [skills], spacer, 6 stats

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3, 2));

    if (ImGui::BeginTable("##detailTeam", numCols,
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Prof", ImGuiTableColumnFlags_WidthFixed, 40.f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, nameColW);
        ImGui::TableSetupColumn("##Copy", ImGuiTableColumnFlags_WidthFixed, copyBtnW);
        if (showSkills)
            ImGui::TableSetupColumn("Skills", ImGuiTableColumnFlags_WidthFixed, 8 * (skillIconSize + 2) + 8.f);
        ImGui::TableSetupColumn("##sp", ImGuiTableColumnFlags_WidthFixed, 6.f);
        const char* statHdrs[] = { "K", "D", "DMG", "INT", "CNC", "SKL" };
        const char* statTips[] = { "Kills", "Deaths", "Damage Dealt", "Interrupts", "Cancelled Skills", "Skills Used" };
        float statWs[] = { statColW, statColW, dmgColW, statColW, statColW, statColW };
        for (int si = 0; si < 6; si++)
            ImGui::TableSetupColumn(statHdrs[si], ImGuiTableColumnFlags_WidthFixed, statWs[si]);

        // Header with totals
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); // prof
        ImGui::TableNextColumn(); // name
        ImGui::TableNextColumn(); // copy
        if (showSkills) ImGui::TableNextColumn(); // skills
        ImGui::TableNextColumn(); // spacer
        int totals[] = { totK, totD, totDmg, totInt, totCnc, totSkl };
        for (int si = 0; si < 6; si++)
        {
            ImGui::TableNextColumn();
            // Label
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.443f, 0.443f, 0.478f, 1.f));
            float tw1 = ImGui::CalcTextSize(statHdrs[si]).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (statWs[si] - tw1) * 0.5f);
            ImGui::TextUnformatted(statHdrs[si]);
            ImGui::PopStyleColor();
            // Total
            char totBuf[16];
            if (!totalAvailable[si]) snprintf(totBuf, sizeof(totBuf), "-");
            else if (totals[si] >= 1000) snprintf(totBuf, sizeof(totBuf), "%.1fk", totals[si] / 1000.f);
            else snprintf(totBuf, sizeof(totBuf), "%d", totals[si]);
            ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
            if (boldFnt) ImGui::PushFont(boldFnt);
            float tw2 = ImGui::CalcTextSize(totBuf).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (statWs[si] - tw2) * 0.5f);
            ImGui::TextUnformatted(totBuf);
            if (boldFnt) ImGui::PopFont();
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", statTips[si]);
        }

        // Player rows
        for (const auto* pp : sorted)
        {
            const auto& p = *pp;
            ImGui::TableNextRow();
            ImGui::PushID(p.player_number);

            // Prof icons
            ImGui::TableNextColumn();
            {
                ImTextureID priIco = GetProfessionIcon(p.primary);
                ImTextureID secIco = GetProfessionIcon(p.secondary);
                ImVec2 sp = ImGui::GetCursorScreenPos();
                if (priIco)
                    ImGui::GetWindowDrawList()->AddImage(priIco, sp, ImVec2(sp.x + 22, sp.y + 22));
                if (secIco)
                    ImGui::GetWindowDrawList()->AddImage(secIco,
                        ImVec2(sp.x + 23, sp.y + 4), ImVec2(sp.x + 38, sp.y + 19));
                ImGui::Dummy(ImVec2(40, skillIconSize));
            }

            // Player name
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.encoded_name.c_str());

            // Copy build
            ImGui::TableNextColumn();
            {
                ImVec2 btnPos = ImGui::GetCursorScreenPos();
                std::string btnId = "##cp_" + std::to_string(p.player_number);
                if (ImGui::InvisibleButton(btnId.c_str(), ImVec2(16, skillIconSize)))
                {
                    std::string code = p.skill_template_code;
                    if (code.empty() && !p.used_skills.empty())
                        code = EncodeSkillTemplate(p.primary, p.secondary, p.used_skills);
                    if (!code.empty())
                    {
                        std::string link = "[" + p.encoded_name + "-Skills;" + code + "]";
                        ImGui::SetClipboardText(link.c_str());
                    }
                }
                ImDrawList* bdl = ImGui::GetWindowDrawList();
                float ix = btnPos.x + 1, iy = btnPos.y + 4;
                ImU32 icol = ImGui::IsItemHovered() ? IM_COL32(245, 158, 11, 255) : IM_COL32(113, 113, 122, 255);
                bdl->AddRect(ImVec2(ix + 3, iy), ImVec2(ix + 12, iy + 11), icol, 1.f, 0, 1.2f);
                bdl->AddRect(ImVec2(ix, iy + 3), ImVec2(ix + 9, iy + 14), icol, 1.f, 0, 1.2f);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy build template");
            }

            // Skill bar
            if (showSkills)
            {
                ImGui::TableNextColumn();
                ImVec2 sp = ImGui::GetCursorScreenPos();
                for (int si = 0; si < 8 && si < (int)p.used_skills.size(); si++)
                {
                    float sx = sp.x + si * (skillIconSize + 2);
                    ImTextureID skillTex = IsUnresolvedHistoricalSkillId(p.used_skills[si],
                        m.year * 10000 + m.month * 100 + m.day)
                        ? nullptr : GetSkillIcon(p.used_skills[si]);
                    if (skillTex)
                        ImGui::GetWindowDrawList()->AddImage(skillTex,
                            ImVec2(sx, sp.y), ImVec2(sx + skillIconSize, sp.y + skillIconSize));
                    else
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            ImVec2(sx, sp.y), ImVec2(sx + skillIconSize, sp.y + skillIconSize),
                            IM_COL32(42, 42, 46, 255), 3.f);
                }
                ImGui::Dummy(ImVec2(8 * (skillIconSize + 2), skillIconSize));
            }

            ImGui::TableNextColumn(); // spacer

            // Stats
            int playerStats[] = { p.kills, p.deaths, p.total_damage,
                                  p.interrupted_count, p.cancelled_skills_count, p.skills_finished };
            bool playerStatAvailable[] = {
                true, true, true,
                (p.preview_stats_available & PreviewInterrupted) != 0,
                (p.preview_stats_available & PreviewCancelledSkills) != 0,
                (p.preview_stats_available & PreviewSkillsFinished) != 0
            };
            for (int si = 0; si < 6; si++)
            {
                ImGui::TableNextColumn();
                char buf[16];
                if (!playerStatAvailable[si])
                    snprintf(buf, sizeof(buf), "-");
                else if (playerStats[si] >= 1000)
                    snprintf(buf, sizeof(buf), "%.1fk", playerStats[si] / 1000.f);
                else
                    snprintf(buf, sizeof(buf), "%d", playerStats[si]);
                float tw = ImGui::CalcTextSize(buf).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (statWs[si] - tw) * 0.5f);
                bool isKD = (si == 0 || si == 1) && playerStats[si] > 0;
                if (playerStats[si] == 0)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.357f, 0.357f, 0.380f, 1.f));
                else if (isKD)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.831f, 0.831f, 0.847f, 1.f));
                else
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.631f, 0.631f, 0.667f, 1.f));
                ImGui::TextUnformatted(buf);
                ImGui::PopStyleColor();
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

static void DrawGalleryDetailPanel(const MatchMeta& m)
{
    ImFont* font = ImGui::GetFont();
    ImFont* boldFnt = GuiGlobalConstants::boldFont;
    ImFont* nameFont = boldFnt ? boldFnt : font;
    const auto sz = GetSizes(s_state.layout);

    // ---- Top bar: MATCH DETAILS + REPLAY MATCH + close ----
    if (boldFnt) ImGui::PushFont(boldFnt);
    ImGui::TextColored(ImVec4(0.961f, 0.620f, 0.043f, 1.f), "MATCH DETAILS");
    if (boldFnt) ImGui::PopFont();

    float contentMaxX = ImGui::GetContentRegionMax().x + ImGui::GetWindowPos().x;
    float lineY = ImGui::GetCursorScreenPos().y - ImGui::GetTextLineHeightWithSpacing();

    // Replay Match button
    {
        ImGui::SameLine();
        float btnW = 120.f, btnH = 24.f;
        float closeW = 24.f;
        ImGui::SetCursorScreenPos(ImVec2(contentMaxX - closeW - 10 - btnW, lineY + 2));

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.961f, 0.620f, 0.043f, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.961f, 0.620f, 0.043f, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.961f, 0.620f, 0.043f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.984f, 0.749f, 0.141f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        if (ImGui::Button("\xe2\x96\xb6 REPLAY", ImVec2(btnW, btnH)) && !g_cloudDownloadInProgress)
        {
            g_pendingReplay.requested = true;
            g_pendingReplay.match = m;
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
    }

    // Close button
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(contentMaxX - 22, lineY + 2));
    if (ImGui::SmallButton("X##detail_close"))
    {
        s_state.selectedMatchIndex = -1;
        s_state.mobileShowDetail = false;
    }
    ImGui::Separator();

    // ---- Sidebar + two team panels ----
    std::string ft1, ft2;
    ParseFolderTags(m.folder_name, ft1, ft2);
    GuildLabel g1 = GetPartyGuild(m, "1", ft1);
    GuildLabel g2 = GetPartyGuild(m, "2", ft2);

    float availW = ImGui::GetContentRegionAvail().x;
    bool stackVertical = (availW < 700.f);
    float sidebarW = stackVertical ? 0.f : std::min(200.f, availW * 0.18f);
    float teamsW = availW - sidebarW - (stackVertical ? 0.f : 16.f);
    float teamW = (teamsW - 8.f) * 0.5f;

    // Compact info bar when stacked (replaces full sidebar)
    if (stackVertical)
    {
        const char* mapName = GetMapName(m.map_id);
        char dateBuf[16];
        snprintf(dateBuf, sizeof(dateBuf), "%04d/%02d/%02d", m.year, m.month, m.day);

        ImGui::TextColored(kColorAccent, "%s", mapName ? mapName : "Unknown");
        ImGui::SameLine(0, 16);
        ImGui::TextColored(kColorTextDim, "%s", dateBuf);
        if (!m.occasion.empty()) { ImGui::SameLine(0, 16); ImGui::TextColored(kColorTextDim, "%s", m.occasion.c_str()); }
        if (!m.match_duration.empty()) { ImGui::SameLine(0, 16); ImGui::TextColored(kColorText, "%s", m.match_duration.c_str()); }
        ImGui::Separator();
    }

    // Full sidebar (only when wide enough)
    if (!stackVertical)
    {
    ImGui::BeginChild("##detail_sidebar", ImVec2(sidebarW, 0));
    {
        // Map image
        float mapH = sidebarW * 0.7f;
        ImTextureID mapIcon = GetMapIcon(m.map_id);
        if (mapIcon)
            ImGui::Image(mapIcon, ImVec2(sidebarW - 8, mapH));
        else
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.149f, 0.149f, 0.165f, 1.f));
            ImGui::BeginChild("##mapph", ImVec2(sidebarW - 8, mapH), ImGuiChildFlags_Border);
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        const char* mapName = GetMapName(m.map_id);
        char dateBuf[16];
        snprintf(dateBuf, sizeof(dateBuf), "%04d/%02d/%02d", m.year, m.month, m.day);

        ImGui::TextColored(ImVec4(0.831f, 0.831f, 0.847f, 1.f), "%s", dateBuf);
        ImGui::TextColored(ImVec4(0.961f, 0.620f, 0.043f, 1.f), "\xe2\x97\x86 %s", mapName ? mapName : "Unknown");
        if (!m.occasion.empty())
            ImGui::TextColored(ImVec4(0.631f, 0.631f, 0.667f, 1.f), "\xe2\x9a\x91 %s", m.occasion.c_str());
        if (!m.match_duration.empty())
            ImGui::TextColored(ImVec4(0.831f, 0.831f, 0.847f, 1.f), "\xe2\x97\xb7 %s", m.match_duration.c_str());
        if (!m.flux.empty())
            DrawFluxWithTooltip(m.flux, 16.f);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Rating
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.443f, 0.443f, 0.478f, 1.f));
        ImGui::TextUnformatted("RATING");
        ImGui::PopStyleColor();
        {
            int cur = MatchRatings::Get().GetRating(m.folder_name);
            int res = DrawStarRating("##detailRate", cur);
            if (res > 0) MatchRatings::Get().SetRating(m.folder_name, res);
            else if (res == -1) MatchRatings::Get().SetRating(m.folder_name, 0);
        }

        ImGui::Spacing();

        // Notes
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.443f, 0.443f, 0.478f, 1.f));
        ImGui::TextUnformatted("NOTES");
        ImGui::PopStyleColor();
        {
            if (s_state.browserNoteMatchId != m.folder_name)
            {
                s_state.browserNoteBuffer = MatchNotes::Get().GetNote(m.folder_name);
                s_state.browserNoteMatchId = m.folder_name;
            }
            float editH = ImGui::GetTextLineHeight() * 3 + ImGui::GetStyle().FramePadding.y * 2;
            if (ImGui::InputTextMultiline("##detailNotes", &s_state.browserNoteBuffer,
                                          ImVec2(sidebarW - 8, editH)))
                MatchNotes::Get().SetNote(m.folder_name, s_state.browserNoteBuffer);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Lord Damage
        ImGui::TextColored(ImVec4(0.961f, 0.620f, 0.043f, 1.f), "LORD DAMAGE");
        if (!m.lord_damage.has_data)
            ImGui::TextColored(ImVec4(0.443f, 0.443f, 0.478f, 1.f), "No lord damage recorded.");
        else
        {
            char buf1[32], buf2[32];
            snprintf(buf1, sizeof(buf1), "%ld", m.lord_damage.total_lord_damage_blue);
            snprintf(buf2, sizeof(buf2), "%ld", m.lord_damage.total_lord_damage_red);
            ImGui::TextColored(ImVec4(0.831f, 0.831f, 0.847f, 1.f), "%s: %s", g1.tag.empty() ? "Team 1" : g1.tag.c_str(), buf1);
            ImGui::TextColored(ImVec4(0.831f, 0.831f, 0.847f, 1.f), "%s: %s", g2.tag.empty() ? "Team 2" : g2.tag.c_str(), buf2);
        }
    }
    ImGui::EndChild();
    } // end if (!stackVertical) sidebar

    if (!stackVertical)
    {
        ImGui::SameLine(0, 8);

        // Team 1
        ImGui::BeginChild("##detail_team1", ImVec2(teamW, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        DrawGalleryDetailTeam(m, "1", g1, m.winner_party_id == 1, teamW);
        ImGui::EndChild();

        ImGui::SameLine(0, 8);

        // Team 2
        ImGui::BeginChild("##detail_team2", ImVec2(teamW, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        DrawGalleryDetailTeam(m, "2", g2, m.winner_party_id == 2, teamW);
        ImGui::EndChild();
    }
    else
    {
        // Stacked vertical for narrow panels
        ImGui::Spacing();
        DrawGalleryDetailTeam(m, "1", g1, m.winner_party_id == 1, availW);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        DrawGalleryDetailTeam(m, "2", g2, m.winner_party_id == 2, availW);
    }
}

// ─── "N new matches added", beside the refresh button ─────────────────── [2026-09-30] ──
//
// What the last rescan brought in, as a pill right after the refresh button it belongs to (it was a
// bar across the top of the screen). It leaves by itself after a few seconds, fading, or at once
// from its x. Drawn straight after the refresh button: it centres itself on that item.
static void DrawNewMatchesPill()
{
    if (s_state.notifyDismissed || s_state.notifyStartTime < 0.f || s_state.notifyNewCount <= 0)
        return;

    const float elapsed = (float)ImGui::GetTime() - s_state.notifyStartTime;
    constexpr float kAutoDismissTime = 4.f;
    constexpr float kFadeDuration = 0.3f;
    if (elapsed > kAutoDismissTime + kFadeDuration)
    {
        s_state.notifyNewCount = 0;
        return;
    }
    const float alpha = elapsed > kAutoDismissTime ? 1.f - (elapsed - kAutoDismissTime) / kFadeDuration : 1.f;

    char text[256];
    if (s_state.notifyNewCount == 1 && !s_state.notifyMatchName.empty())
    {
        std::string truncated = s_state.notifyMatchName;
        if (truncated.size() > 50) truncated = truncated.substr(0, 47) + "...";
        snprintf(text, sizeof(text), "New match added: %s", truncated.c_str());
    }
    else if (s_state.notifyNewCount == 1)
        snprintf(text, sizeof(text), "1 new match added");
    else
        snprintf(text, sizeof(text), "%d new matches added", s_state.notifyNewCount);

    const ImVec2 refMn = ImGui::GetItemRectMin();
    const ImVec2 refMx = ImGui::GetItemRectMax();
    const float lineH = ImGui::GetTextLineHeight();
    const float pillH = lineH + 10.f;
    const float padX = 10.f;
    const float closeW = lineH + 8.f;
    const float textW = ImGui::CalcTextSize(text).x;

    ImGui::SameLine(0, 10);
    const ImVec2 at(ImGui::GetCursorScreenPos().x, std::floor((refMn.y + refMx.y - pillH) * 0.5f));
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy(ImVec2(padX + textW + 4.f, pillH));
    ImGui::SameLine(0, 0);
    const bool dismiss = ImGui::InvisibleButton("##new_matches_dismiss", ImVec2(closeW, pillH));
    const bool closeHovered = ImGui::IsItemHovered();
    if (closeHovered)
        ImGui::SetTooltip("Dismiss");

    const ImVec2 mx(at.x + padX + textW + 4.f + closeW, at.y + pillH);
    const auto A = [alpha](int a) { return (int)(a * alpha); };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, mx, IM_COL32(0x2A, 0x2A, 0x1C, A(255)), 4.f);
    dl->AddRect(at, mx, IM_COL32(0x7A, 0x9A, 0x50, A(255)), 4.f);
    dl->AddText(ImVec2(at.x + padX, at.y + (pillH - lineH) * 0.5f), IM_COL32(0xB8, 0xDC, 0x90, A(255)), text);
    const float cx = mx.x - closeW * 0.5f - 2.f;
    const float cy = at.y + pillH * 0.5f;
    const float r = 3.5f;
    const ImU32 xCol = closeHovered ? IM_COL32(0xF0, 0xE8, 0xD6, A(255)) : IM_COL32(0xA8, 0xA0, 0x90, A(255));
    dl->AddLine(ImVec2(cx - r, cy - r), ImVec2(cx + r, cy + r), xCol, 1.5f);
    dl->AddLine(ImVec2(cx - r, cy + r), ImVec2(cx + r, cy - r), xCol, 1.5f);

    if (dismiss)
        s_state.notifyDismissed = true;
}

static void DrawGalleryTopBar(int matchCount, bool hideSortAndCount = false)
{
    ImFont* boldFnt = GuiGlobalConstants::boldFont;

    if (!hideSortAndCount)
    {
        // Match count
        if (boldFnt) ImGui::PushFont(boldFnt);
        ImGui::TextColored(kColorAccent, "MATCHES");
        if (boldFnt) ImGui::PopFont();
        ImGui::SameLine(0, 4);
        ImGui::TextColored(kColorTextDim, "(%d)", matchCount);
    }

    // Sort combo (hidden in tournament mode)
    if (!hideSortAndCount)
    {
        ImGui::SameLine(0, 16);
        ImGui::SetNextItemWidth(160.f);
        const char* sortLabels[] = { "Newest First", "Oldest First", "Highest Rating", "Map Name" };
        int sortIdx = 0;
        if (s_state.sortColumn == 0 && !s_state.sortAscending) sortIdx = 0;
        else if (s_state.sortColumn == 0 && s_state.sortAscending) sortIdx = 1;
        else if (s_state.sortColumn == 7) sortIdx = 2;
        else if (s_state.sortColumn == 2) sortIdx = 3;

        if (ImGui::Combo("##gallery_sort", &sortIdx, sortLabels, 4))
        {
            switch (sortIdx) {
                case 0: s_state.sortColumn = 0; s_state.sortAscending = false; break;
                case 1: s_state.sortColumn = 0; s_state.sortAscending = true;  break;
                case 2: s_state.sortColumn = 7; s_state.sortAscending = false; break;
                case 3: s_state.sortColumn = 2; s_state.sortAscending = true;  break;
            }
        }
    }

    // View toggle: Table | Cards. The Library's own views: Scout is a tool of its own on the
    // application ribbon [2026-09-30], and has neither.
    if (!s_state.tournamentMode)
    {
        ImGui::SameLine(0, 16);
        ImGui::TextColored(kColorTextDim, "|");
        ImGui::SameLine(0, 8);

        if (ui::SegmentedButton("Table", !s_state.cardGalleryMode))
        {
            s_state.cardGalleryMode = false;
            GuiGlobalConstants::replay_card_gallery_mode = 0;
            GuiGlobalConstants::SaveSettings();
        }
        ImGui::SameLine(0, 2);
        if (ui::SegmentedButton("Cards", s_state.cardGalleryMode))
        {
            s_state.cardGalleryMode = true;
            GuiGlobalConstants::replay_card_gallery_mode = 1;
            GuiGlobalConstants::SaveSettings();
        }
    }

    // ── Opponent guild search + map filter (inline in top bar when Scout is active) ──
    if (s_state.tournamentMode)
    {
        // First on the bar when the count and the sort are hidden (Scout's own panel).
        if (!hideSortAndCount)
        {
            ImGui::SameLine(0, 16);
            ImGui::TextColored(kColorTextDim, "|");
            ImGui::SameLine(0, 8);
        }

        // Opponent guild search with autocomplete
        ImGui::SetNextItemWidth(220.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.10f, 0.12f, 0.8f));
        ImGui::InputTextWithHint("##prep_guild_search",
            "Search opponent guild...",
            s_state.tournamentGuildBuf, sizeof(s_state.tournamentGuildBuf));
        ImVec2 guildInputMin = ImGui::GetItemRectMin();
        ImVec2 guildInputMax = ImGui::GetItemRectMax();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        // Autocomplete popup (shown whenever buffer has text)
        std::string guildSearchLower = ToLower(std::string(s_state.tournamentGuildBuf));
        if (!guildSearchLower.empty() && s_state.guildNames.size() > 1)
        {
            ImGui::SetNextWindowPos(ImVec2(guildInputMin.x, guildInputMax.y));
            ImGui::SetNextWindowSizeConstraints(ImVec2(280, 0), ImVec2(400, 250));
            if (ImGui::Begin("##guild_autocomplete", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize))
            {
                int shown = 0;
                for (int i = 1; i < (int)s_state.guildNames.size() && shown < 12; i++)
                {
                    const auto& gn = s_state.guildNames[i];
                    if (ToLower(gn).find(guildSearchLower) == std::string::npos)
                        continue;

                    if (ImGui::Selectable(gn.c_str()))
                    {
                        // Parse "Name [Tag]" into parts
                        s_state.tournamentGuildDisplay = gn;
                        auto bracketPos = gn.rfind('[');
                        if (bracketPos != std::string::npos)
                        {
                            s_state.tournamentGuildName = gn.substr(0, bracketPos);
                            while (!s_state.tournamentGuildName.empty() &&
                                   s_state.tournamentGuildName.back() == ' ')
                                s_state.tournamentGuildName.pop_back();
                            auto endBracket = gn.rfind(']');
                            if (endBracket != std::string::npos && endBracket > bracketPos)
                                s_state.tournamentGuildTag = gn.substr(bracketPos + 1,
                                    endBracket - bracketPos - 1);
                        }
                        else
                        {
                            s_state.tournamentGuildName = gn;
                            s_state.tournamentGuildTag = gn;
                        }
                        snprintf(s_state.tournamentGuildBuf,
                                 sizeof(s_state.tournamentGuildBuf), "%s", gn.c_str());
                        s_state.tournamentSelectedBuild = -1;
                        s_state.tournamentSelectedLostTo = -1;
                        s_tournamentCacheKey.clear();
                    }
                    shown++;
                }
                if (shown == 0)
                    ImGui::TextColored(ImVec4(kColorTextDim), "No guilds match");
            }
            ImGui::End();
        }

        // Selected guild badge (shown when a guild is selected)
        if (!s_state.tournamentGuildTag.empty())
        {
            ImGui::SameLine(0, 8);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.961f, 0.620f, 0.043f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.961f, 0.620f, 0.043f, 1.0f));
            std::string badgeLabel = "[" + s_state.tournamentGuildTag + "] x";
            if (ImGui::SmallButton(badgeLabel.c_str()))
            {
                s_state.tournamentGuildBuf[0] = '\0';
                s_state.tournamentGuildDisplay.clear();
                s_state.tournamentGuildTag.clear();
                s_state.tournamentGuildName.clear();
                s_state.tournamentSelectedBuild = -1;
                s_state.tournamentSelectedLostTo = -1;
                s_tournamentCacheKey.clear();
            }
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();
        }

        // Map filter
        ImGui::SameLine(0, 12);
        ImGui::SetNextItemWidth(160.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.10f, 0.12f, 0.8f));
        ImGui::InputTextWithHint("##prep_map_filter",
            "Filter map...",
            s_state.tournamentMapBuf, sizeof(s_state.tournamentMapBuf));
        ImVec2 mapInputMin = ImGui::GetItemRectMin();
        ImVec2 mapInputMax = ImGui::GetItemRectMax();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        // Map autocomplete popup (shown whenever buffer has text)
        std::string mapSearchLower = ToLower(std::string(s_state.tournamentMapBuf));
        if (!mapSearchLower.empty() && s_state.mapNames.size() > 1)
        {
            ImGui::SetNextWindowPos(ImVec2(mapInputMin.x, mapInputMax.y));
            ImGui::SetNextWindowSizeConstraints(ImVec2(200, 0), ImVec2(320, 200));
            if (ImGui::Begin("##map_autocomplete", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize))
            {
                int shown = 0;
                for (int i = 1; i < (int)s_state.mapNames.size() && shown < 10; i++)
                {
                    const auto& mn = s_state.mapNames[i];
                    if (ToLower(mn).find(mapSearchLower) == std::string::npos)
                        continue;

                    bool alreadySelected = s_state.tournamentMaps.count(mn) > 0;
                    if (alreadySelected)
                        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);

                    if (ImGui::Selectable(mn.c_str()))
                    {
                        if (alreadySelected)
                            s_state.tournamentMaps.erase(mn);
                        else
                            s_state.tournamentMaps.insert(mn);
                        s_state.tournamentMapBuf[0] = '\0';
                        s_tournamentCacheKey.clear();
                        s_state.tournamentSelectedBuild = -1;
                        s_state.tournamentSelectedLostTo = -1;
                    }

                    if (alreadySelected)
                        ImGui::PopStyleColor();
                    shown++;
                }
                if (shown == 0)
                    ImGui::TextColored(ImVec4(kColorTextDim), "No maps match");
            }
            ImGui::End();
        }

        // Map filter badges
        if (!s_state.tournamentMaps.empty())
        {
            for (auto it = s_state.tournamentMaps.begin(); it != s_state.tournamentMaps.end(); )
            {
                ImGui::SameLine(0, 4);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.12f, 0.08f, 0.80f));
                ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
                std::string mapBadge = *it + " x";
                if (ImGui::SmallButton(mapBadge.c_str()))
                {
                    it = s_state.tournamentMaps.erase(it);
                    s_tournamentCacheKey.clear();
                    s_state.tournamentSelectedBuild = -1;
                    s_state.tournamentSelectedLostTo = -1;
                }
                else
                {
                    ++it;
                }
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar();
            }
        }
    }

    // Refresh — outlined when new matches are waiting
    ImGui::SameLine(0, 16);
    if (DrawRefreshIconButton("##refresh_gallery", g_refreshHint))
    {
        g_refreshMatchIndex = true;
        g_refreshHint = false;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(g_refreshHint ? "Refresh for latest matches"
                                        : "Refresh - re-fetch match list from cloud");
    DrawNewMatchesPill();
}

// ─── Tournament Prep stats panel ────────────────────────────────────────────

static void DrawTournamentStatsPanel(const std::vector<MatchMeta>& matches, float listH, int generation)
{
    ImFont* font = ImGui::GetFont();
    ImFont* boldFnt = GuiGlobalConstants::boldFont;
    ImFont* monoBold = GuiGlobalConstants::monoBoldFont ? GuiGlobalConstants::monoBoldFont : (boldFnt ? boldFnt : font);

    ImGui::SameLine(0, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);

    float h = listH > 0 ? listH : ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##tournament_panel", ImVec2(-1, h), ImGuiChildFlags_Border);

    // Reuse the same toolbar so Table/Cards/Prep are always visible
    DrawGalleryTopBar(0, true);
    ImGui::Separator();
    ImGui::Spacing();

    // If no guild selected yet, show a prompt
    if (s_state.tournamentGuildTag.empty())
    {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
        if (boldFnt) ImGui::PushFont(boldFnt);
        ImGui::TextUnformatted("SCOUT");
        if (boldFnt) ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(kColorTextDim),
            "Search for an opponent guild in the toolbar above to begin scouting.");
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        return;
    }

    const auto& stats = GetTournamentStats(matches, generation);

    // ── Header ──
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
        if (boldFnt) ImGui::PushFont(boldFnt);
        ImGui::TextUnformatted("SCOUT");
        if (boldFnt) ImGui::PopFont();
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::TextColored(ImVec4(kColorText), "- %s", s_state.tournamentGuildDisplay.c_str());

        // Summary line
        int totalWins = 0, totalLosses = 0;
        for (const auto& s : stats) { totalWins += s.wins; totalLosses += s.losses; }
        int totalMatches = totalWins + totalLosses;
        float overallPct = totalMatches > 0 ? (totalWins / (float)totalMatches * 100.0f) : 0.0f;

        ImGui::TextColored(ImVec4(kColorTextDim),
            "%d matches | %d builds | Record: %d-%d (%.0f%%)",
            totalMatches, (int)stats.size(), totalWins, totalLosses, overallPct);

        // Map filter badge
        if (!s_state.tournamentMaps.empty())
        {
            ImGui::SameLine();
            for (const auto& m : s_state.tournamentMaps)
            {
                ImGui::SameLine(0, 4.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.12f, 0.08f, 0.80f));
                ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
                ImGui::SmallButton(m.c_str());
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    if (stats.empty())
    {
        ImGui::TextColored(ImVec4(kColorTextDim),
            "No matches found for %s", s_state.tournamentGuildDisplay.c_str());
        if (!s_state.tournamentMaps.empty())
            ImGui::TextColored(ImVec4(kColorTextDim), "Try removing the map filter.");
    }
    else
    {
        // ── Build stats table ──
        float availH = ImGui::GetContentRegionAvail().y;
        bool hasBuildDrillDown = (s_state.tournamentSelectedBuild >= 0 &&
                             s_state.tournamentSelectedBuild < (int)stats.size());
        bool hasLostToDrillDown = (s_state.tournamentSelectedLostTo >= 0 &&
                             s_state.tournamentSelectedLostTo < (int)s_lostToCache.size());
        bool hasDrillDown = hasBuildDrillDown || hasLostToDrillDown;
        bool hasLostTo = !s_lostToCache.empty();
        float tableH;
        if (hasDrillDown)
            tableH = availH * 0.30f;
        else if (hasLostTo)
            tableH = availH * 0.55f;
        else
            tableH = availH;

        ImGuiTableFlags tblFlags =
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Sortable |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;

        if (ImGui::BeginTable("##tournament_builds", 5, tblFlags, ImVec2(0, tableH)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Build", ImGuiTableColumnFlags_DefaultSort, 0.30f);
            ImGui::TableSetupColumn("Composition", ImGuiTableColumnFlags_NoSort, 0.30f);
            ImGui::TableSetupColumn("Played", ImGuiTableColumnFlags_DefaultSort, 0.12f);
            ImGui::TableSetupColumn("W-L", ImGuiTableColumnFlags_DefaultSort, 0.12f);
            ImGui::TableSetupColumn("Win %", ImGuiTableColumnFlags_DefaultSort, 0.16f);
            ImGui::TableHeadersRow();

            // Handle sorting
            if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
            {
                if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0)
                {
                    s_state.tournamentSortColumn = sortSpecs->Specs[0].ColumnIndex;
                    s_state.tournamentSortAsc = (sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
                    sortSpecs->SpecsDirty = false;

                    // We need a mutable copy for sorting
                    s_tournamentCache = s_tournamentCache; // already mutable
                    auto& cache = s_tournamentCache;
                    int col = s_state.tournamentSortColumn;
                    bool asc = s_state.tournamentSortAsc;
                    std::sort(cache.begin(), cache.end(),
                        [col, asc](const TournamentBuildStats& a, const TournamentBuildStats& b) {
                            int cmp = 0;
                            switch (col) {
                            case 0: cmp = a.buildName.compare(b.buildName); break;
                            case 2: cmp = a.timesPlayed - b.timesPlayed; break;
                            case 3: cmp = a.wins - b.wins; break;
                            case 4: cmp = (a.winPct < b.winPct) ? -1 : (a.winPct > b.winPct ? 1 : 0); break;
                            default: cmp = a.timesPlayed - b.timesPlayed; break;
                            }
                            return asc ? cmp < 0 : cmp > 0;
                        });
                }
            }

            ImGuiListClipper clipper;
            clipper.Begin((int)stats.size());
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    const auto& bs = stats[row];
                    bool isSelected = (s_state.tournamentSelectedBuild == row);

                    ImGui::TableNextRow();

                    // Build name
                    ImGui::TableNextColumn();
                    ImGui::PushID(row);

                    if (isSelected)
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(kColorAccent.x, kColorAccent.y, kColorAccent.z, 0.20f));

                    if (ImGui::Selectable("##trow", isSelected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    {
                        s_state.tournamentSelectedBuild = isSelected ? -1 : row;
                        s_state.tournamentSelectedLostTo = -1; // clear other drill-down
                    }

                    if (isSelected)
                        ImGui::PopStyleColor();

                    ImGui::SameLine();
                    bool hasBuildName = (bs.buildName != bs.profSignature && bs.buildName != "Unknown");
                    if (hasBuildName && boldFnt)
                    {
                        ImGui::PushFont(boldFnt);
                        ImGui::TextUnformatted(bs.buildName.c_str());
                        ImGui::PopFont();
                    }
                    else
                    {
                        ImGui::TextUnformatted(bs.buildName.c_str());
                    }

                    // Composition (profession icons)
                    ImGui::TableNextColumn();
                    {
                        auto tokens = ParseCompString(bs.profSignature);
                        float iconSz = 18.f;
                        float gap = 6.f;
                        ImVec2 startPos = ImGui::GetCursorScreenPos();
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        float x = startPos.x;
                        float y = startPos.y;
                        for (auto& t : tokens)
                        {
                            ImTextureID ico = GetProfessionIcon(t.profId);
                            if (ico)
                                dl->AddImage(ico, ImVec2(x, y), ImVec2(x + iconSz, y + iconSz));
                            x += iconSz + 2.f;
                            char countBuf[8]; snprintf(countBuf, sizeof(countBuf), "%d", t.count);
                            dl->AddText(monoBold, 13.f, ImVec2(x, y + 2.f),
                                        IM_COL32(212, 212, 216, 255), countBuf);
                            x += monoBold->CalcTextSizeA(13.f, FLT_MAX, 0.f, countBuf).x + gap;
                        }
                        ImGui::Dummy(ImVec2(x - startPos.x, iconSz));
                    }

                    // Played
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", bs.timesPlayed);

                    // W-L
                    ImGui::TableNextColumn();
                    ImGui::Text("%d-%d", bs.wins, bs.losses);

                    // Win %
                    ImGui::TableNextColumn();
                    {
                        ImU32 pctCol;
                        if (bs.winPct >= 60.f)
                            pctCol = IM_COL32(76, 175, 80, 255);
                        else if (bs.winPct >= 40.f)
                            pctCol = IM_COL32(245, 158, 11, 255);
                        else
                            pctCol = IM_COL32(229, 57, 53, 255);

                        ImGui::Text("%.0f%%", bs.winPct);
                        ImGui::SameLine(0, 8.0f);

                        // Win rate bar
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        float barW = 50.0f, barH = 10.0f;
                        ImVec2 barMin = ImGui::GetCursorScreenPos();
                        barMin.y += 4.0f;
                        ImVec2 barMax(barMin.x + barW, barMin.y + barH);
                        dl->AddRectFilled(barMin, barMax, IM_COL32(40, 40, 40, 200), 2.0f);
                        float fillW = barW * (bs.winPct / 100.0f);
                        dl->AddRectFilled(barMin, ImVec2(barMin.x + fillW, barMax.y), pctCol, 2.0f);
                        ImGui::Dummy(ImVec2(barW, barH));
                    }

                    ImGui::PopID();
                }
            }
            clipper.End();
            ImGui::EndTable();
        }

        // ── "Lost To" table: builds that beat this guild ──
        if (hasLostTo)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
            if (boldFnt) ImGui::PushFont(boldFnt);
            ImGui::TextUnformatted("LOST AGAINST");
            if (boldFnt) ImGui::PopFont();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(kColorTextDim), "- builds that beat %s (%d)",
                               s_state.tournamentGuildDisplay.c_str(),
                               (int)s_lostToCache.size());

            // Search filter
            ImGui::SameLine(0, 12);
            ImGui::SetNextItemWidth(180.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.10f, 0.12f, 0.8f));
            ImGui::InputTextWithHint("##lostto_search", "Search builds...",
                                     s_state.tournamentLostToSearch,
                                     sizeof(s_state.tournamentLostToSearch));
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::Spacing();

            std::string lostToSearchLower = ToLower(std::string(s_state.tournamentLostToSearch));

            float lostH = hasDrillDown
                ? ImGui::GetContentRegionAvail().y * 0.45f
                : ImGui::GetContentRegionAvail().y;

            ImGuiTableFlags ltFlags =
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Sortable |
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;

            if (ImGui::BeginTable("##tournament_lostto", 3, ltFlags, ImVec2(0, lostH)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Build", ImGuiTableColumnFlags_DefaultSort, 0.40f);
                ImGui::TableSetupColumn("Composition", ImGuiTableColumnFlags_NoSort, 0.35f);
                ImGui::TableSetupColumn("Times", ImGuiTableColumnFlags_DefaultSort, 0.25f);
                ImGui::TableHeadersRow();

                for (int row = 0; row < (int)s_lostToCache.size(); row++)
                {
                    const auto& lt = s_lostToCache[row];

                    // Apply search filter
                    if (!lostToSearchLower.empty())
                    {
                        if (ToLower(lt.buildName).find(lostToSearchLower) == std::string::npos &&
                            ToLower(lt.profSignature).find(lostToSearchLower) == std::string::npos)
                            continue;
                    }

                    bool isSelected = (s_state.tournamentSelectedLostTo == row);

                    ImGui::TableNextRow();

                    // Build name
                    ImGui::TableNextColumn();
                    ImGui::PushID(row + 10000); // offset to avoid ID collisions

                    if (isSelected)
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.90f, 0.22f, 0.21f, 0.20f));

                    if (ImGui::Selectable("##lt_row", isSelected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    {
                        s_state.tournamentSelectedLostTo = isSelected ? -1 : row;
                        s_state.tournamentSelectedBuild = -1; // clear other drill-down
                    }

                    if (isSelected)
                        ImGui::PopStyleColor();

                    ImGui::SameLine();
                    bool hasBuild = (lt.buildName != lt.profSignature);
                    if (hasBuild && boldFnt)
                    {
                        ImGui::PushFont(boldFnt);
                        ImGui::TextUnformatted(lt.buildName.c_str());
                        ImGui::PopFont();
                    }
                    else
                    {
                        ImGui::TextUnformatted(lt.buildName.c_str());
                    }

                    // Composition
                    ImGui::TableNextColumn();
                    {
                        auto tokens = ParseCompString(lt.profSignature);
                        float iconSz = 18.f;
                        float gap = 6.f;
                        ImVec2 startPos = ImGui::GetCursorScreenPos();
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        float x = startPos.x;
                        float y = startPos.y;
                        for (auto& t : tokens)
                        {
                            ImTextureID ico = GetProfessionIcon(t.profId);
                            if (ico)
                                dl->AddImage(ico, ImVec2(x, y), ImVec2(x + iconSz, y + iconSz));
                            x += iconSz + 2.f;
                            char countBuf[8]; snprintf(countBuf, sizeof(countBuf), "%d", t.count);
                            dl->AddText(monoBold, 13.f, ImVec2(x, y + 2.f),
                                        IM_COL32(212, 212, 216, 255), countBuf);
                            x += monoBold->CalcTextSizeA(13.f, FLT_MAX, 0.f, countBuf).x + gap;
                        }
                        ImGui::Dummy(ImVec2(x - startPos.x, iconSz));
                    }

                    // Times
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0.90f, 0.22f, 0.21f, 1.0f), "%d", lt.timesPlayed);

                    ImGui::PopID();
                }

                ImGui::EndTable();
            }
        }

        // ── Drill-down: matches for selected build or selected lost-to ──
        const TournamentBuildStats* drillDownBuild = nullptr;
        const char* drillDownLabel = nullptr;
        bool drillDownIsLoss = false;

        if (s_state.tournamentSelectedBuild >= 0 &&
            s_state.tournamentSelectedBuild < (int)stats.size())
        {
            drillDownBuild = &stats[s_state.tournamentSelectedBuild];
            drillDownLabel = "Matches playing";
        }
        else if (s_state.tournamentSelectedLostTo >= 0 &&
                 s_state.tournamentSelectedLostTo < (int)s_lostToCache.size())
        {
            drillDownBuild = &s_lostToCache[s_state.tournamentSelectedLostTo];
            drillDownLabel = "Losses against";
            drillDownIsLoss = true;
        }

        if (drillDownBuild)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, drillDownIsLoss
                ? ImVec4(0.90f, 0.22f, 0.21f, 1.0f) : kColorAccent);
            ImGui::Text("%s \"%s\" (%d)", drillDownLabel,
                        drillDownBuild->buildName.c_str(),
                        drillDownBuild->timesPlayed);
            ImGui::PopStyleColor();
            ImGui::Spacing();

            ImGuiTableFlags ddFlags =
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;

            float ddH = ImGui::GetContentRegionAvail().y;

            if (ImGui::BeginTable("##tournament_dd", 5, ddFlags, ImVec2(0, ddH)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Date", 0, 0.20f);
                ImGui::TableSetupColumn("Map", 0, 0.22f);
                ImGui::TableSetupColumn(drillDownIsLoss ? "Lost To" : "Opponent", 0, 0.30f);
                ImGui::TableSetupColumn("Result", 0, 0.12f);
                ImGui::TableSetupColumn("Duration", 0, 0.16f);
                ImGui::TableHeadersRow();

                // Recompute matching games on the fly from the current matches vector
                const std::string& targetBuild = drillDownBuild->buildName;
                for (int mi = 0; mi < (int)matches.size(); mi++)
                {
                    const auto& m = matches[mi];

                    // Apply same map filter as the aggregation
                    if (!s_state.tournamentMaps.empty())
                    {
                        const char* mnf = GetMapName(m.map_id);
                        std::string mapNameF = mnf ? mnf : ("Map " + std::to_string(m.map_id));
                        if (s_state.tournamentMaps.find(mapNameF) == s_state.tournamentMaps.end())
                            continue;
                    }

                    std::string partyId = FindGuildParty(m, s_state.tournamentGuildTag,
                                                         s_state.tournamentGuildName);
                    if (partyId.empty()) continue;

                    bool won = (std::to_string(m.winner_party_id) == partyId);

                    // For losses drill-down: only show losses where opponent ran this build
                    // For builds drill-down: only show matches where our guild ran this build
                    std::string checkPartyId = drillDownIsLoss
                        ? ((partyId == "1") ? "2" : "1")  // opponent's party
                        : partyId;                          // our party

                    if (drillDownIsLoss && won) continue; // only losses

                    auto pit = m.parties.find(checkPartyId);
                    if (pit == m.parties.end() || pit->second.players.empty()) continue;

                    std::string buildName = ComputeTeamBuild(pit->second);
                    if (buildName != targetBuild) continue;

                    std::string opponentPartyId = (partyId == "1") ? "2" : "1";

                    // Opponent guild label
                    std::string ft1, ft2;
                    ParseFolderTags(m.folder_name, ft1, ft2);
                    std::string oppFolderTag = (opponentPartyId == "1") ? ft1 : ft2;
                    GuildLabel opponent = GetPartyGuild(m, opponentPartyId, oppFolderTag);

                    ImGui::TableNextRow();

                    // Date
                    ImGui::TableNextColumn();
                    ImGui::PushID(mi);
                    bool ddSelected = (s_state.selectedMatchIndex == mi);
                    if (ImGui::Selectable("##dd_row", ddSelected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        if (ImGui::IsMouseDoubleClicked(0) && !g_cloudDownloadInProgress)
                        {
                            s_state.selectedMatchIndex = mi;
                            s_state.tournamentMode = false;
                            g_pendingReplay.requested = true;
                            g_pendingReplay.match = m;
                        }
                        else
                        {
                            s_state.selectedMatchIndex = ddSelected ? -1 : mi;
                        }
                    }
                    ImGui::SameLine();
                    if (m.month >= 1 && m.month <= 12)
                        ImGui::Text("%s %d, %d", MonthAbbrev(m.month), m.day, m.year);
                    else
                        ImGui::Text("%02d/%02d/%d", m.day, m.month, m.year);

                    // Map
                    ImGui::TableNextColumn();
                    {
                        const char* mn = GetMapName(m.map_id);
                        ImGui::TextUnformatted(mn ? mn : "Unknown");
                    }

                    // Opponent
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(opponent.display.c_str());

                    // Result
                    ImGui::TableNextColumn();
                    if (won)
                        ImGui::TextColored(ImVec4(0.30f, 0.69f, 0.31f, 1.0f), "Win");
                    else
                        ImGui::TextColored(ImVec4(0.90f, 0.22f, 0.21f, 1.0f), "Loss");

                    // Duration
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(m.match_duration.c_str());

                    ImGui::PopID();
                }

                ImGui::EndTable();
            }
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

static void DrawCardGallery(const std::vector<FilteredMatch>& filtered,
                            const std::vector<MatchMeta>& allMatches,
                            float listH)
{
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);

    bool hasDetail = (s_state.selectedMatchIndex >= 0 &&
                      s_state.selectedMatchIndex < (int)allMatches.size());
    float spacing = 8.0f;
    float outerAvail = ImGui::GetContentRegionAvail().x;
    float detailPanelW = hasDetail ? std::clamp(outerAvail * 0.38f, 480.0f, 700.0f) : 0.0f;
    float gridOuterW = outerAvail - detailPanelW - (hasDetail ? spacing : 0.0f);

    // Card grid area
    ImGui::BeginChild("##card_gallery_area", ImVec2(gridOuterW, listH), ImGuiChildFlags_Border);

    DrawGalleryTopBar((int)filtered.size());
    ImGui::Separator();

    // Scrollable grid
    {
        int cols = 3; // fixed 3-column grid
        float gridSpacing = 12.0f;
        float availW = ImGui::GetContentRegionAvail().x;
        float cardWidth = (availW - gridSpacing * (cols - 1)) / cols;
        if (cardWidth < 200.f) cardWidth = 200.f;
        float cardH = 215.f;
        float rowH = cardH + gridSpacing;
        int totalRows = ((int)filtered.size() + cols - 1) / cols;

        ImGuiListClipper clipper;
        clipper.Begin(totalRows, rowH);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
            {
                for (int col = 0; col < cols; col++)
                {
                    int idx = row * cols + col;
                    if (idx >= (int)filtered.size()) break;

                    if (col > 0) ImGui::SameLine(0, gridSpacing);

                    const auto& fm = filtered[idx];
                    bool isSel = (s_state.selectedMatchIndex == fm.originalIndex);
                    DrawGalleryCard(fm, cardWidth, isSel);
                }
            }
        }
    }

    ImGui::EndChild();

    // Side detail panel
    if (hasDetail)
    {
        ImGui::SameLine(0, spacing);

        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(s_themeColors.detailPanelBg.x, s_themeColors.detailPanelBg.y, s_themeColors.detailPanelBg.z, 0.95f));

        ImGui::BeginChild("##gallery_detail_side", ImVec2(detailPanelW, listH), ImGuiChildFlags_Border);

        DrawGalleryDetailPanel(allMatches[s_state.selectedMatchIndex]);

        ImGui::EndChild();

        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// ─── Match list table ────────────────────────────────────────────────────────

// Shared with the row loop, which needs the height to centre the cell.
static constexpr float kCompIconSz  = 18.0f;
static constexpr float kCompIconGap = 3.0f;

// A team's composition: one profession symbol per player, melee first.
//
// The signature is grouped ("2W/1R/2Mo"), so each token is repeated for its count rather than
// printed as a number - eight players read as eight icons.
static void DrawCompositionIcons(const std::string& profSig)
{
    if (profSig.empty()) { ImGui::TextDisabled("-"); return; }

    const auto tokens = ParseCompString(profSig);
    const float iconSz = kCompIconSz;
    const float gap = kCompIconGap;

    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = start.x;

    for (const auto& t : tokens)
    {
        ImTextureID ico = GetProfessionIcon(t.profId);
        if (!ico) continue;
        for (int n = 0; n < t.count; n++)
        {
            dl->AddImage(ico, ImVec2(x, start.y), ImVec2(x + iconSz, start.y + iconSz));
            x += iconSz + gap;
        }
    }

    // Claim the row height, and give the cell something to hover for the text form.
    ImGui::Dummy(ImVec2(std::max(1.f, x - start.x), iconSz));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", profSig.c_str());
}

static void DrawMatchListTable(const std::vector<FilteredMatch>& filtered,
                               const std::vector<MatchMeta>& allMatches,
                               float listH = 0)
{
    const auto mode = s_state.layout;
    const auto sz = GetSizes(mode);

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);

    ImGui::BeginChild("##match_list_area", ImVec2(0, listH), ImGuiChildFlags_Border);

    // Header with back button in mobile detail mode
    if (mode == LayoutMode::Mobile && s_state.mobileShowDetail &&
        s_state.selectedMatchIndex >= 0 && s_state.selectedMatchIndex < (int)allMatches.size())
    {
        if (ImGui::Button("< Back to list"))
            s_state.mobileShowDetail = false;
        ImGui::Separator();
    }

    // Mobile: show detail view instead of list when a match is selected
    if (mode == LayoutMode::Mobile && s_state.mobileShowDetail &&
        s_state.selectedMatchIndex >= 0 && s_state.selectedMatchIndex < (int)allMatches.size())
    {
        // Detail content rendered inline (handled below after EndChild)
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        return;
    }

    // The refresh badge is the tallest thing on this row, so the row grows to it and every
    // other control is centred against that height instead of sitting on the top edge.
    const float rowTopY = ImGui::GetCursorPosY();
    const float topRowH = std::max(RefreshIconButtonHeight(), ImGui::GetFrameHeight());
    auto CentreOnRow = [&](float itemH) {
        ImGui::SetCursorPosY(rowTopY + (topRowH - itemH) * 0.5f);
    };

    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    CentreOnRow(ImGui::GetTextLineHeight());
    ImGui::Text("MATCHES  (%d)", (int)filtered.size());
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 12);
    CentreOnRow(RefreshIconButtonHeight());
    // Refresh — outlined when new matches are waiting
    if (DrawRefreshIconButton("##refresh_list", g_refreshHint))
    {
        g_refreshMatchIndex = true;
        g_refreshHint = false;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(g_refreshHint ? "Refresh for latest matches"
                                        : "Refresh - re-fetch match list from cloud");
    DrawNewMatchesPill();
    ImGui::Separator();

    // Card mode for mobile
    if (mode == LayoutMode::Mobile)
    {
        DrawMatchCards(filtered, sz);
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        return;
    }

    // Table mode for wider layouts
    float tableW = ImGui::GetContentRegionAvail().x;
    float dateW = 100.0f;
    float occasionW = (mode == LayoutMode::Narrow) ? 100.0f : 130.0f;
    float ratingW = 72.0f;
    float compW = 80.0f;          // team build name
    float compIconW = 8 * (kCompIconSz + kCompIconGap) + 4.0f;   // a full team of eight
    float mapW = std::max(90.0f, tableW * 0.13f);
    float notesColW = 20.0f;
    float teamW = std::clamp((tableW - dateW - mapW - occasionW - ratingW
                              - compW * 2 - compIconW * 2 - notesColW) * 0.5f, 100.0f, 250.0f);

    ImGuiTableFlags tableFlags =
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;

    // Wider gutters stand in for the column rules that used to separate the cells.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10.0f, 2.0f));

    // Column order is the logical order the cells below address by index, so the cells can
    // stay in the order that reads best in code while the table shows them in this one.
    // The sort comparator switches on the same indices - keep the two in step.
    enum MatchCol {
        Col_Date = 0, Col_Occasion, Col_Map,
        Col_Team1, Col_Comp1, Col_Team2, Col_Comp2,
        Col_Build1, Col_Build2, Col_Rating, Col_Notes, Col_Count
    };

    if (ImGui::BeginTable("##match_table", Col_Count, tableFlags))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("DATE",          ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, dateW);
        ImGui::TableSetupColumn("OCCASION",      ImGuiTableColumnFlags_WidthFixed, occasionW);
        ImGui::TableSetupColumn("MAP",           ImGuiTableColumnFlags_WidthFixed, mapW);
        ImGui::TableSetupColumn("TEAM 1",        ImGuiTableColumnFlags_WidthFixed, teamW);
        // Icons carry no ordering a click on the header could express, so they do not sort.
        ImGui::TableSetupColumn("COMPOSITION 1", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, compIconW);
        ImGui::TableSetupColumn("TEAM 2",        ImGuiTableColumnFlags_WidthFixed, teamW);
        ImGui::TableSetupColumn("COMPOSITION 2", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, compIconW);
        ImGui::TableSetupColumn("TEAM BUILD 1",  ImGuiTableColumnFlags_WidthFixed, compW);
        ImGui::TableSetupColumn("TEAM BUILD 2",  ImGuiTableColumnFlags_WidthFixed, compW);
        ImGui::TableSetupColumn("RATING",        ImGuiTableColumnFlags_WidthFixed, ratingW);
        ImGui::TableSetupColumn("##notes",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoReorder, notesColW);

        // Headers sit back as labels over the list rather than announcing a grid.
        ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();

        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0)
            {
                s_state.sortColumn = sortSpecs->Specs[0].ColumnIndex;
                s_state.sortAscending = (sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
                sortSpecs->SpecsDirty = false;
            }
        }

        ImTextureID cupTex = GetCupIcon();

        // Rows are given room to breathe rather than packed to the text height. Scaled off
        // the font so it follows the font-size setting instead of fixing a pixel count.
        const float textH = ImGui::GetTextLineHeight();
        const float rowH  = textH + 12.0f;

        // ImGui puts cell content at the top of the row, which in a tall row leaves the text
        // sitting on a band of whitespace. Nudge each cell down to sit on the row's centre.
        auto CenterCell = [&](float contentH) {
            const float off = (rowH - contentH) * 0.5f;
            if (off > 0.f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + off);
        };

        // ComputeTeamBuild falls back to the profession signature when no definition matches,
        // so an unnamed build prints the Composition column beside it a second time, in text.
        // A dash instead leaves only the builds somebody has actually named on screen.
        auto DrawGuild = [&](const GuildLabel& g) {
            if (GuiGlobalConstants::boldFont) ImGui::PushFont(GuiGlobalConstants::boldFont);
            ImGui::TextUnformatted(g.name.empty() ? g.display.c_str() : g.name.c_str());
            if (GuiGlobalConstants::boldFont) ImGui::PopFont();
            if (!g.name.empty() && !g.tag.empty())
            {
                ImGui::SameLine(0, 4);
                ImGui::TextColored(kColorTextDim, "[%s]", g.tag.c_str());
            }
        };

        auto DrawBuildName = [&](const std::string& build, const std::string& profSig) {
            if (build.empty() || build == profSig) { ImGui::TextDisabled("-"); return; }
            ImGui::PushStyleColor(ImGuiCol_Text, s_themeColors.compTextCol);
            ImGui::TextUnformatted(build.c_str());
            ImGui::PopStyleColor();
        };

        ImGuiListClipper clipper;
        clipper.Begin((int)filtered.size());
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
            {
                const auto& fm = filtered[row];
                const auto& m = *fm.meta;
                bool isSelected = (s_state.selectedMatchIndex == fm.originalIndex);

                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
                ImGui::PushID(fm.originalIndex);

                // Gold highlight flash for newly added matches
                auto hlIt = s_state.highlightStartTimes.find(m.folder_path);
                if (hlIt != s_state.highlightStartTimes.end())
                {
                    float age = (float)ImGui::GetTime() - hlIt->second;
                    float hlAlpha = std::max(0.f, 1.f - age / 2.f) * 0.15f;
                    if (hlAlpha > 0.f)
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                            IM_COL32(212, 160, 32, (int)(hlAlpha * 255)));
                }

                ImGui::TableSetColumnIndex(Col_Date);

                // Status icon and date are painted over the selectable below, so the
                // selectable carries no label and only provides the row's hit box.
                ImVec2 cellStart = ImGui::GetCursorScreenPos();
                float iconW = ImGui::CalcTextSize("\xe2\x98\x81").x + 4.f;

                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, s_themeColors.rowHoverBg);
                ImGui::PushStyleColor(ImGuiCol_Header,        s_themeColors.rowSelectedBg);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  s_themeColors.rowSelectedBg);
                // An explicit height, because a Selectable sizes itself to its text and would
                // otherwise paint a highlight band thinner than the row it belongs to.
                bool rowClicked = ImGui::Selectable("##row", isSelected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap
                    | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowH));
                ImGui::PopStyleColor(3);
                if (rowClicked)
                {
                    if (ImGui::IsMouseDoubleClicked(0) && !g_cloudDownloadInProgress)
                    {
                        s_state.selectedMatchIndex = fm.originalIndex;
                        g_pendingReplay.requested = true;
                        g_pendingReplay.match = m;
                    }
                    else
                    {
                        s_state.selectedMatchIndex = isSelected ? -1 : fm.originalIndex;
                    }
                }
                if (ImGui::IsItemHovered() && m.is_cloud_only)
                    ImGui::SetTooltip("Cloud only - will download when opened");
                if (ImGui::IsItemFocused() && !isSelected)
                    s_state.selectedMatchIndex = fm.originalIndex;

                // Draw status icon on top of the selectable
                {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const char* icon;
                    ImU32 color;
                    if (m.is_cloud_only) {
                        icon = "\xe2\x98\x81"; // ☁
                        color = IM_COL32(90, 160, 220, 230);
                    } else {
                        icon = "\xe2\x9c\x94"; // ✔
                        color = IM_COL32(70, 190, 95, 230);
                    }
                    const float cy = cellStart.y + (rowH - textH) * 0.5f;
                    dl->AddText(ImVec2(cellStart.x, cy), color, icon);

                    // Every row carries its own date. Blanking the repeats left ragged holes
                    // down the column, which reads as broken rather than grouped, and it fell
                    // apart under any sort but this one. The day banding above does that job.
                    char dateBuf[16];
                    snprintf(dateBuf, sizeof(dateBuf), "%04d/%02d/%02d",
                             m.year, m.month, m.day);
                    dl->AddText(ImVec2(cellStart.x + iconW, cy),
                                ImGui::GetColorU32(kColorTextMuted), dateBuf);
                }

                ImGui::TableSetColumnIndex(Col_Occasion);
                CenterCell(textH);
                ImGui::TextColored(kColorTextMuted, "%s", m.occasion.c_str());

                ImGui::TableSetColumnIndex(Col_Map);
                CenterCell(textH);
                ImGui::TextColored(kColorTextMuted, "%s", fm.mapName.c_str());

                ImGui::TableSetColumnIndex(Col_Team1);
                CenterCell(textH);
                DrawGuild(fm.guild1);
                if (m.winner_party_id == 1 && cupTex)
                {
                    ImGui::SameLine();
                    ImGui::Image(cupTex, ImVec2(sz.cupIcon, sz.cupIcon));
                }

                ImGui::TableSetColumnIndex(Col_Comp1);
                CenterCell(kCompIconSz);
                DrawCompositionIcons(fm.profSig1);

                // Team build 1
                ImGui::TableSetColumnIndex(Col_Build1);
                CenterCell(textH);
                DrawBuildName(fm.build1, fm.profSig1);
                if (ImGui::IsItemHovered() && !fm.profSig1.empty())
                    ImGui::SetTooltip("%s", fm.profSig1.c_str());
                if (GuiGlobalConstants::IsDeveloperMode() && !GuiGlobalConstants::contributor_key.empty() && ImGui::BeginPopupContextItem(("##nc1_" + std::to_string(fm.originalIndex)).c_str()))
                {
                    // Initialize popup state
                    if (s_buildNaming.profSig != fm.profSig1)
                    {
                        s_buildNaming.profSig = fm.profSig1;
                        s_buildNaming.profCounts = fm.profCounts1;
                        s_buildNaming.existingDefIdx = -1;
                        // Find existing def with same exact professions (no skill constraints)
                        for (int di = 0; di < (int)s_buildDefs.size(); di++)
                        {
                            if (s_buildDefs[di].professions == fm.profCounts1
                                && s_buildDefs[di].keySkills.empty()
                                && s_buildDefs[di].skillCounts.empty()
                                && s_buildDefs[di].minProfessions.empty())
                            {
                                s_buildNaming.existingDefIdx = di;
                                break;
                            }
                        }
                        std::string pre = (s_buildNaming.existingDefIdx >= 0)
                            ? s_buildDefs[s_buildNaming.existingDefIdx].name : "";
                        size_t len = std::min(pre.size(), sizeof(s_buildNaming.nameBuf) - 1);
                        memcpy(s_buildNaming.nameBuf, pre.c_str(), len);
                        s_buildNaming.nameBuf[len] = '\0';
                    }
                    ImGui::TextColored(ImVec4(0.90f, 0.76f, 0.30f, 1.f), "%s", fm.profSig1.c_str());
                    ImGui::Separator();
                    bool confirmed = ImGui::InputText("##bname", s_buildNaming.nameBuf,
                        sizeof(s_buildNaming.nameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
                    if (ImGui::Button("Save") || confirmed)
                    {
                        if (s_buildNaming.nameBuf[0] != '\0')
                        {
                            if (s_buildNaming.existingDefIdx >= 0)
                                s_buildDefs[s_buildNaming.existingDefIdx].name = s_buildNaming.nameBuf;
                            else
                            {
                                BuildDef def;
                                def.name = s_buildNaming.nameBuf;
                                def.professions = s_buildNaming.profCounts;
                                s_buildDefs.push_back(std::move(def));
                            }
                            SaveBuildDefs();
                            PushBuildsToCloud();
                            s_state.filtersBuilt = false;
                        }
                        s_buildNaming.profSig.clear();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel"))
                    {
                        s_buildNaming.profSig.clear();
                        ImGui::CloseCurrentPopup();
                    }
                    if (s_buildNaming.existingDefIdx >= 0)
                    {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 0.8f));
                        if (ImGui::Button("Delete"))
                        {
                            s_buildDefs.erase(s_buildDefs.begin() + s_buildNaming.existingDefIdx);
                            SaveBuildDefs();
                            PushBuildsToCloud();
                            s_state.filtersBuilt = false;
                            s_buildNaming.profSig.clear();
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::PopStyleColor();
                    }
                    ImGui::EndPopup();
                }

                ImGui::TableSetColumnIndex(Col_Team2);
                CenterCell(textH);
                DrawGuild(fm.guild2);
                if (m.winner_party_id == 2 && cupTex)
                {
                    ImGui::SameLine();
                    ImGui::Image(cupTex, ImVec2(sz.cupIcon, sz.cupIcon));
                }

                ImGui::TableSetColumnIndex(Col_Comp2);
                CenterCell(kCompIconSz);
                DrawCompositionIcons(fm.profSig2);

                // Team build 2
                ImGui::TableSetColumnIndex(Col_Build2);
                CenterCell(textH);
                DrawBuildName(fm.build2, fm.profSig2);
                if (ImGui::IsItemHovered() && !fm.profSig2.empty())
                    ImGui::SetTooltip("%s", fm.profSig2.c_str());
                if (GuiGlobalConstants::IsDeveloperMode() && !GuiGlobalConstants::contributor_key.empty() && ImGui::BeginPopupContextItem(("##nc2_" + std::to_string(fm.originalIndex)).c_str()))
                {
                    if (s_buildNaming.profSig != fm.profSig2)
                    {
                        s_buildNaming.profSig = fm.profSig2;
                        s_buildNaming.profCounts = fm.profCounts2;
                        s_buildNaming.existingDefIdx = -1;
                        for (int di = 0; di < (int)s_buildDefs.size(); di++)
                        {
                            if (s_buildDefs[di].professions == fm.profCounts2
                                && s_buildDefs[di].keySkills.empty()
                                && s_buildDefs[di].skillCounts.empty()
                                && s_buildDefs[di].minProfessions.empty())
                            {
                                s_buildNaming.existingDefIdx = di;
                                break;
                            }
                        }
                        std::string pre = (s_buildNaming.existingDefIdx >= 0)
                            ? s_buildDefs[s_buildNaming.existingDefIdx].name : "";
                        size_t len = std::min(pre.size(), sizeof(s_buildNaming.nameBuf) - 1);
                        memcpy(s_buildNaming.nameBuf, pre.c_str(), len);
                        s_buildNaming.nameBuf[len] = '\0';
                    }
                    ImGui::TextColored(ImVec4(0.90f, 0.76f, 0.30f, 1.f), "%s", fm.profSig2.c_str());
                    ImGui::Separator();
                    bool confirmed = ImGui::InputText("##bname2", s_buildNaming.nameBuf,
                        sizeof(s_buildNaming.nameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
                    if (ImGui::Button("Save") || confirmed)
                    {
                        if (s_buildNaming.nameBuf[0] != '\0')
                        {
                            if (s_buildNaming.existingDefIdx >= 0)
                                s_buildDefs[s_buildNaming.existingDefIdx].name = s_buildNaming.nameBuf;
                            else
                            {
                                BuildDef def;
                                def.name = s_buildNaming.nameBuf;
                                def.professions = s_buildNaming.profCounts;
                                s_buildDefs.push_back(std::move(def));
                            }
                            SaveBuildDefs();
                            PushBuildsToCloud();
                            s_state.filtersBuilt = false;
                        }
                        s_buildNaming.profSig.clear();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel"))
                    {
                        s_buildNaming.profSig.clear();
                        ImGui::CloseCurrentPopup();
                    }
                    if (s_buildNaming.existingDefIdx >= 0)
                    {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 0.8f));
                        if (ImGui::Button("Delete##2"))
                        {
                            s_buildDefs.erase(s_buildDefs.begin() + s_buildNaming.existingDefIdx);
                            SaveBuildDefs();
                            PushBuildsToCloud();
                            s_state.filtersBuilt = false;
                            s_buildNaming.profSig.clear();
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::PopStyleColor();
                    }
                    ImGui::EndPopup();
                }

                // Rating column
                ImGui::TableSetColumnIndex(Col_Rating);
                CenterCell(textH);
                {
                    int cur = MatchRatings::Get().GetRating(m.folder_name);
                    int res = DrawStarRating(("##rate" + std::to_string(fm.originalIndex)).c_str(),
                                             cur, false, /*hideWhenEmpty=*/true);
                    if (res > 0) MatchRatings::Get().SetRating(m.folder_name, res);
                    else if (res == -1) MatchRatings::Get().SetRating(m.folder_name, 0);
                }

                // Notes indicator
                ImGui::TableSetColumnIndex(Col_Notes);
                CenterCell(textH);
                if (MatchNotes::Get().HasNote(m.folder_name))
                {
                    ImGui::TextColored(ImVec4(0.90f, 0.75f, 0.25f, 0.85f), "\xe2\x9c\x8e");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Has notes");
                }

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();   // CellPadding, pushed before BeginTable

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// ─── Team composition panel ──────────────────────────────────────────────────

// Draws `label` as a link into the search filter: underlined and lit in the accent while
// hovered, with the hand cursor, so it reads as something to click rather than a label that
// happens to react. Returns true on click.
//
// The hover repaint overdraws the same string at the same position rather than colouring it
// up front, because hover is only known once the item exists.
// Clicking one of these changes the list behind the panel, but the panel itself does not
// move, so without an acknowledgement the click reads as having done nothing. Keyed by the
// label, which is unique enough among guild and player names on screen at once.
static std::unordered_map<std::string, float> s_linkFlashTimes;
constexpr float kLinkFlashSeconds = 0.45f;

static bool DrawFilterLink(const char* label, const ImVec4& baseCol)
{
    const float now = (float)ImGui::GetTime();

    ImGui::PushStyleColor(ImGuiCol_Text, baseCol);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 lit = ImGui::GetColorU32(kColorAccent);

    auto plate = [&](float alpha, float dy) {
        ImVec4 c = kColorAccent; c.w = alpha;
        dl->AddRectFilled(ImVec2(mn.x - 3.0f, mn.y - 2.0f), ImVec2(mx.x + 3.0f, mx.y + 2.0f),
                          ImGui::GetColorU32(c), 3.0f);
        // Repainted over the plate rather than under it, since the label is already down.
        dl->AddText(ImVec2(mn.x, mn.y + dy), lit, label);
    };

    // Fading confirmation of a click that has already landed.
    auto flash = s_linkFlashTimes.find(label);
    if (flash != s_linkFlashTimes.end())
    {
        const float age = now - flash->second;
        if (age >= kLinkFlashSeconds) s_linkFlashTimes.erase(flash);
        else plate(0.40f * (1.0f - age / kLinkFlashSeconds), 0.0f);
    }

    if (!ImGui::IsItemHovered()) return false;

    // Held: pressed plate and a one-pixel push, so the click has a beginning as well as an end.
    const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (held) plate(0.28f, 1.0f);
    else      dl->AddText(mn, lit, label);

    dl->AddLine(ImVec2(mn.x, mx.y - 1.0f), ImVec2(mx.x, mx.y - 1.0f), lit, 1.0f);
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        s_linkFlashTimes[label] = now;
        return true;
    }
    return false;
}

// Add one guild or player to the search without disturbing what is already there.
//
// The typed box is cleared, since it is a second and independent filter that would otherwise
// keep narrowing on top of the chips.
//
// Note the chips are OR-ed by FilterMatches, so several players widen the result to matches
// featuring ANY of them, not all.
static void AddSearchTerm(const std::string& term)
{
    if (term.empty()) return;
    s_state.selectedSearchTerms.insert(term);
    s_state.searchBuf[0] = '\0';
    s_state.searchDebounced[0] = '\0';
    s_state.lastSearchEditTime = -1.0f;
}

// Narrow the library to exactly one guild or player, dropping whatever was filtered before.
static void FilterBySearchTerm(const std::string& term)
{
    if (term.empty()) return;
    s_state.selectedSearchTerms.clear();
    s_state.selectedSearchTerms.insert(term);
    s_state.searchBuf[0] = '\0';
    s_state.searchDebounced[0] = '\0';
    s_state.lastSearchEditTime = -1.0f;
}

// The skill data as it was on the day of the match.
static const SkillDatabaseView& GetMatchSkillView(const MatchMeta& m)
{
    static std::unordered_map<int, SkillDatabaseView> s_matchViewCache;
    int dateKey = m.year * 10000 + m.month * 100 + m.day;
    auto [cacheIt, inserted] = s_matchViewCache.try_emplace(dateKey);
    if (inserted)
        cacheIt->second = GetSkillDatabase().GetView(m.year, m.month, m.day);
    return cacheIt->second;
}

// A player's bar, as the Overview shows it: hover for the skill, click to filter by it.
// One solved attribute (match_analysis.json "attributes", AttributeModel::WriteJson): the
// effective rank the packets measure, and what of it the rune and the headgear added.
struct SolvedAttribute
{
    int id = 0, lo = 0, hi = 0, best = 0;
    bool budgetOnly = true;   // never seen in use: only a ceiling from the 200 points
    int rune = 0;             // 0..3
    bool headgear = false;
    int Base() const { return std::clamp(best - rune - (headgear ? 1 : 0), 0, 12); }
};

// What the Overview knows about the match beyond the index: the solved builds, once the match
// analysis has run. Null while it is downloading or reading.
struct OverviewBuilds
{
    const std::unordered_map<std::string, std::vector<SolvedAttribute>>* builds = nullptr;
    const char* pending = nullptr;   // what is happening instead, shown where the attributes go
};

static void DrawPlayerSkillRow(const PlayerMeta& p, const SkillDatabaseView& matchView, float skillIconSize)
{
    if (p.used_skills.empty()) return;
    auto sortedSkills = matchView.SortSkillsForDisplay(p.used_skills, p.primary, p.secondary);
    for (int ski = 0; ski < (int)sortedSkills.size(); ski++)
    {
        if (ski > 0) ImGui::SameLine(0, 2);
        int skillId = sortedSkills[ski];
        ImTextureID skillTex = GetSkillIcon(skillId, &matchView);
        if (skillTex)
        {
            ImGui::Image(skillTex, ImVec2(skillIconSize, skillIconSize));
            const bool picked = s_state.selectedSkills.count(skillId) > 0;
            if (picked)
            {
                // Already in the filter: ring it so the panel and this list
                // agree about what is selected.
                ImGui::GetWindowDrawList()->AddRect(
                    ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                    ImGui::GetColorU32(kColorAccent), 2.0f, 0, 2.0f);
            }

            if (ImGui::IsItemHovered())
            {
                // Lift the hovered icon a little. Drawn on the foreground
                // list because the grown edges would otherwise be clipped
                // by the table cell the row of icons sits in.
                const float grow = skillIconSize * 0.16f;
                const ImVec2 mn = ImGui::GetItemRectMin();
                const ImVec2 mx = ImGui::GetItemRectMax();
                ImGui::GetForegroundDrawList()->AddImage(
                    skillTex,
                    ImVec2(mn.x - grow, mn.y - grow),
                    ImVec2(mx.x + grow, mx.y + grow));

                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                DrawSkillTooltip(skillId, &matchView);

                // Toggle, not just add: without it the only way to undo a
                // mis-click is to go and find the chip in the filter panel.
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    if (picked) s_state.selectedSkills.erase(skillId);
                    else        s_state.selectedSkills.insert(skillId);
                }
            }
        }
        else
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(pos,
                ImVec2(pos.x + skillIconSize, pos.y + skillIconSize),
                IM_COL32(42, 42, 46, 255), 2.f);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(pos.x + skillIconSize * 0.3f, pos.y),
                IM_COL32(180, 180, 180, 255), "?");
            ImGui::Dummy(ImVec2(skillIconSize, skillIconSize));
            if (ImGui::IsItemHovered()) DrawSkillTooltip(skillId, &matchView);
        }
    }
}

// The primary profession's own attribute, which a template window lists first.
static int PrimaryAttributeOf(int primaryProf)
{
    switch (primaryProf)
    {
    case 1:  return 17;  // Warrior       Strength
    case 2:  return 23;  // Ranger        Expertise
    case 3:  return 16;  // Monk          Divine Favor
    case 4:  return 6;   // Necromancer   Soul Reaping
    case 5:  return 0;   // Mesmer        Fast Casting
    case 6:  return 12;  // Elementalist  Energy Storage
    case 7:  return 35;  // Assassin      Critical Strikes
    case 8:  return 36;  // Ritualist     Spawning Power
    case 9:  return 40;  // Paragon       Leadership
    case 10: return 44;  // Dervish       Mysticism
    default: return -1;
    }
}

// The attributes worth a line: seen in use and above zero, primary profession first with its own
// attribute at the top, each group strongest first - the order the replay's Character panel uses.
// A rank nothing was observed on is only a ceiling the 200 points leave, so it is left out here.
static std::vector<const SolvedAttribute*> ShownAttributes(const std::vector<SolvedAttribute>& build, int primaryProf)
{
    std::vector<const SolvedAttribute*> out;
    for (const auto& a : build)
        if (!a.budgetOnly && a.hi > 0) out.push_back(&a);
    const int primaryAttr = PrimaryAttributeOf(primaryProf);
    std::sort(out.begin(), out.end(), [&](const SolvedAttribute* a, const SolvedAttribute* b) {
        const auto key = [&](const SolvedAttribute* x) {
            const int group = SkillDatabase::GetProfessionForAttribute(x->id) == primaryProf ? 0 : 1;
            return std::tuple{ group, x->id == primaryAttr ? 0 : 1, -x->best, x->id };
        };
        return key(a) < key(b);
    });
    return out;
}

// The player's template code: the solved ranks when the match has been read, the skills alone
// otherwise (the recorder's own code when it has one).
static std::string PlayerTemplateCode(const PlayerMeta& p, const std::vector<SolvedAttribute>* build)
{
    if (build && !build->empty())
    {
        std::vector<std::pair<int, int>> attrs;
        for (const SolvedAttribute* a : ShownAttributes(*build, p.primary))
            if (a->Base() > 0) attrs.push_back({ a->id, a->Base() });
        if (!attrs.empty())
            return EncodeSkillTemplate(p.primary, p.secondary, p.used_skills, attrs);
    }
    return p.skill_template_code.empty() ? EncodeSkillTemplate(p.primary, p.secondary, p.used_skills)
                                         : p.skill_template_code;
}

// The two-sheet copy icon, turning gold and showing a tick for a moment after a click. Returns
// true when clicked.
static bool DrawCopyIcon(const char* id, float btnSize, float vertOff, float feedbackAt)
{
    const float now = (float)ImGui::GetTime();
    const bool showFeedback = feedbackAt >= 0.0f && (now - feedbackAt < 1.5f);

    const ImVec2 startCursor = ImGui::GetCursorScreenPos();
    const ImVec2 cursor(startCursor.x, startCursor.y + vertOff);
    ImGui::SetCursorScreenPos(cursor);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(btnSize, btnSize));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float feedbackAge = showFeedback ? (now - feedbackAt) : 99.0f;
    float goldBlend = 0.0f;
    if (showFeedback)
        goldBlend = (feedbackAge < 0.3f) ? 1.0f : std::max(0.0f, 1.0f - (feedbackAge - 0.3f) / 1.2f);

    auto lerpCol = [](ImU32 a, ImU32 b, float t) -> ImU32 {
        float r = ((a >> 0) & 0xFF) * (1 - t) + ((b >> 0) & 0xFF) * t;
        float g = ((a >> 8) & 0xFF) * (1 - t) + ((b >> 8) & 0xFF) * t;
        float bl = ((a >> 16) & 0xFF) * (1 - t) + ((b >> 16) & 0xFF) * t;
        float al = ((a >> 24) & 0xFF) * (1 - t) + ((b >> 24) & 0xFF) * t;
        return IM_COL32((int)r, (int)g, (int)bl, (int)al);
    };

    const ImU32 baseCol = hovered ? IM_COL32(190, 190, 190, 230) : IM_COL32(130, 130, 130, 160);
    const ImU32 iconCol = lerpCol(baseCol, IM_COL32(225, 190, 80, 255), goldBlend);

    const float m = btnSize * 0.12f;
    const float cx = cursor.x + m, cy = cursor.y + m;
    const float cw = btnSize - 2 * m, ch = btnSize - 2 * m;
    const float t = std::max(1.0f, btnSize * 0.08f);

    if (showFeedback && feedbackAge < 0.4f)
    {
        const float mx = cursor.x + btnSize * 0.5f, my = cursor.y + btnSize * 0.5f;
        const float s = btnSize * 0.22f;
        dl->AddLine(ImVec2(mx - s, my), ImVec2(mx - s * 0.3f, my + s * 0.7f), iconCol, t * 1.5f);
        dl->AddLine(ImVec2(mx - s * 0.3f, my + s * 0.7f), ImVec2(mx + s, my - s * 0.5f), iconCol, t * 1.5f);
    }
    else
    {
        const float off = cw * 0.2f, rw = cw - off, rh = ch - off, r = 1.5f;
        dl->AddRect(ImVec2(cx + off, cy), ImVec2(cx + off + rw, cy + rh), iconCol, r, 0, t);
        dl->AddRectFilled(ImVec2(cx, cy + off), ImVec2(cx + rw, cy + off + rh), IM_COL32(30, 30, 30, 200), r);
        dl->AddRect(ImVec2(cx, cy + off), ImVec2(cx + rw, cy + off + rh), iconCol, r, 0, t);
    }
    return clicked;
}

// The colours of the replay's Character panel split (ReplayWindow_CharacterPanel.cpp): a
// near-white rank, the rune's own number in its rarity's colour, the headgear's in dim gold.
static constexpr ImU32 kAttrExact  = IM_COL32(232, 236, 242, 255);
static constexpr ImU32 kAttrRanged = IM_COL32(196, 202, 210, 255);
static constexpr ImU32 kAttrMarker = IM_COL32(176, 148, 74, 255);

static ImU32 RuneTierColour(int tier)
{
    const Equipment::Rgb c = Equipment::RarityColor(tier >= 3 ? Equipment::Rarity::Gold
                                                    : tier == 2 ? Equipment::Rarity::Purple
                                                                : Equipment::Rarity::Blue);
    return IM_COL32(c.r, c.g, c.b, 255);
}

// The Overview's one pinned build card: which match and player, and where it was opened.
struct PinnedBuildCard
{
    std::string folder;   // MatchMeta::folder_name
    std::string key;      // "party:number"
    ImVec2 pos;
};
static PinnedBuildCard s_pinnedCard;

// A player's build as the game's template window shows it: professions and name, the skill bar,
// then each attribute with its rank in a frame, how the armour made it up ("10+2+1", the rune in
// its rarity's colour), and the full name. Drawn into the current tooltip or window.
static void DrawBuildCard(const PlayerMeta& p, const std::string& name,
                          const std::vector<SolvedAttribute>* build, const char* pending,
                          const SkillDatabaseView& matchView, float skillIconSize, bool pinned)
{
    const float profIcon = ImGui::GetTextLineHeight() + 4.0f;
    ImFont* bold = GuiGlobalConstants::boldFont;

    // Header: the two profession icons, the name, the professions spelled out.
    for (int prof : { p.primary, p.secondary })
    {
        if (ImTextureID ico = GetProfessionIcon(prof)) ImGui::Image(ico, ImVec2(profIcon, profIcon));
        else                                          ImGui::Dummy(ImVec2(profIcon, profIcon));
        ImGui::SameLine(0, 2);
    }
    ImGui::SameLine(0, 8);
    {
        const float y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + (profIcon - ImGui::GetTextLineHeight()) * 0.5f);
        if (bold) ImGui::PushFont(bold);
        ImGui::TextColored(kColorText, "%s", name.c_str());
        if (bold) ImGui::PopFont();
        ImGui::SameLine(0, 10);
        ImGui::SetCursorPosY(y + (profIcon - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextColored(kColorTextDim, "%s / %s", GetProfessionName(p.primary), GetProfessionName(p.secondary));
    }

    // The bar, as plain icons: the card is for reading, the list row is where skills are picked.
    if (!p.used_skills.empty())
    {
        ImGui::Spacing();
        const auto sortedSkills = matchView.SortSkillsForDisplay(p.used_skills, p.primary, p.secondary);
        for (size_t i = 0; i < sortedSkills.size(); i++)
        {
            if (i > 0) ImGui::SameLine(0, 2);
            if (ImTextureID tex = GetSkillIcon(sortedSkills[i], &matchView))
                ImGui::Image(tex, ImVec2(skillIconSize, skillIconSize));
            else
                ImGui::Dummy(ImVec2(skillIconSize, skillIconSize));
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!build)
        ImGui::TextColored(kColorTextDim, "%s", pending ? pending : "Attributes unavailable");
    else if (const auto shown = ShownAttributes(*build, p.primary); shown.empty())
        ImGui::TextColored(kColorTextDim, "No attributes seen in use");
    else
    {
        if (bold) ImGui::PushFont(bold);
        const float boxW = ImGui::CalcTextSize("~00").x + 12.0f;
        if (bold) ImGui::PopFont();
        const float splitW = ImGui::CalcTextSize("~00+0+0").x + 10.0f;
        const float lineH = ImGui::GetTextLineHeight();
        const ImU32 frame = IM_COL32(120, 128, 140, 190);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        for (const SolvedAttribute* ap : shown)
        {
            const SolvedAttribute& a = *ap;
            const bool exact = (a.lo == a.hi);
            const ImU32 colour = exact ? kAttrExact : kAttrRanged;
            const ImVec2 o = ImGui::GetCursorScreenPos();

            dl->AddRect(ImVec2(o.x, o.y - 2.0f), ImVec2(o.x + boxW, o.y + lineH + 2.0f), frame, 2.0f);
            char rank[16];
            snprintf(rank, sizeof(rank), exact ? "%d" : "~%d", a.best);
            if (bold) ImGui::PushFont(bold);
            dl->AddText(ImVec2(o.x + (boxW - ImGui::CalcTextSize(rank).x) * 0.5f, o.y), colour, rank);
            if (bold) ImGui::PopFont();

            // Nothing to spell out when the points bought the whole rank.
            float x = o.x + boxW + 8.0f;
            if (a.rune > 0 || a.headgear)
            {
                const std::string base = (exact ? "" : "~") + std::to_string(a.Base());
                dl->AddText(ImVec2(x, o.y), colour, base.c_str());
                x += ImGui::CalcTextSize(base.c_str()).x;
                if (a.rune > 0)
                {
                    const std::string r = "+" + std::to_string(a.rune);
                    dl->AddText(ImVec2(x, o.y), RuneTierColour(a.rune), r.c_str());
                    x += ImGui::CalcTextSize(r.c_str()).x;
                }
                if (a.headgear)
                    dl->AddText(ImVec2(x, o.y), kAttrMarker, "+1");
            }

            ImGui::SetCursorScreenPos(ImVec2(o.x + boxW + 8.0f + splitW, o.y));
            ImGui::TextColored(kColorText, "%s", SkillDatabase::GetAttributeName(a.id));
            if (!exact)
            {
                ImGui::SameLine(0, 8);
                ImGui::TextColored(kColorTextDim, "(%d-%d)", a.lo, a.hi);
            }
            ImGui::Dummy(ImVec2(0, 2));
        }
    }

    ImGui::Spacing();
    ImGui::TextColored(kColorTextDim, pinned ? "Click the profession icon again to close"
                                             : "Click the profession icon to keep this open");
    ImGui::TextColored(kColorTextDim, "Click the name to show every match with this player");
}

static void DrawTeamComposition(const MatchMeta& m, const std::string& partyId,
                                const GuildLabel& guild, bool isWinner,
                                const ResponsiveSizes& sz, const OverviewBuilds& ob)
{
    const auto& matchView = GetMatchSkillView(m);

    const float iconSize = sz.profIcon;
    const float skillIconSize = sz.skillIcon;

    // Party "1" is Blue and party "2" is Red; see TeamColors.h.
    const int teamSlot = (partyId == "2") ? 1 : 0;

    static std::unordered_map<std::string, float> s_copyFeedbackTimes;
    auto feedbackAt = [](const std::string& key) {
        auto it = s_copyFeedbackTimes.find(key);
        return it == s_copyFeedbackTimes.end() ? -1.0f : it->second;
    };

    ImGui::BeginGroup();

    auto pit = m.parties.find(partyId);
    if (pit == m.parties.end())
    {
        ImGui::TextColored(kColorTextDim, "No player data");
        ImGui::EndGroup();
        return;
    }

    std::vector<const PlayerMeta*> sorted;
    sorted.reserve(pit->second.players.size());
    for (const auto& p : pit->second.players)
        sorted.push_back(&p);
    std::sort(sorted.begin(), sorted.end(),
        [](const PlayerMeta* a, const PlayerMeta* b) { return a->player_number < b->player_number; });

    auto keyOf = [&](const PlayerMeta& p) { return partyId + ":" + std::to_string(p.player_number); };
    auto buildOf = [&](const PlayerMeta& p) -> const std::vector<SolvedAttribute>* {
        if (!ob.builds) return nullptr;
        auto it = ob.builds->find(keyOf(p));
        return it == ob.builds->end() ? nullptr : &it->second;
    };

    static const ImVec4 kColorStatText = ImVec4(0.490f, 0.490f, 0.494f, 1.0f); // #7D7D7E

    const float availW = ImGui::GetContentRegionAvail().x;
    const float headerH = 34.0f;
    const bool showSkills = (s_state.layout != LayoutMode::Mobile && s_state.layout != LayoutMode::Narrow
                             && availW >= 500.0f);
    const float nameColW = showSkills ? std::max(80.0f, availW * 0.18f) : std::max(80.0f, availW * 0.30f);
    const float copyBtnW = skillIconSize * 0.45f + 4.0f;
    const float skillsW = 8 * (skillIconSize + 2) + 16.0f;
    const int numCols = 3 + (showSkills ? 1 : 0) + 1;

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3, 2));
    ImGui::PushStyleColor(ImGuiCol_Text, kColorStatText);

    if (ImGui::BeginTable(("##team_" + partyId).c_str(), numCols,
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Pri",  ImGuiTableColumnFlags_WidthFixed, iconSize);
        ImGui::TableSetupColumn("Sec",  ImGuiTableColumnFlags_WidthFixed, iconSize + 2);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, nameColW);
        if (showSkills)
            ImGui::TableSetupColumn("Skills", ImGuiTableColumnFlags_WidthFixed, skillsW);
        ImGui::TableSetupColumn("##Copy", ImGuiTableColumnFlags_WidthFixed, copyBtnW);
        const int copyCol = numCols - 1;

        // Header row: the guild's identity on the left, the whole team's copy on the right.
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            // The name is drawn from the first column but is far wider than it, so the clip is
            // widened across the empty cells to its right rather than cut at the Name column.
            const ImVec2 clipMin = ImGui::GetCursorScreenPos();
            const float clipW = iconSize + (iconSize + 2.0f) + nameColW + (showSkills ? skillsW : 0.0f);
            ImGui::PushClipRect(clipMin, ImVec2(clipMin.x + clipW, clipMin.y + headerH + 8.0f), false);

            ImFont* bold = GuiGlobalConstants::boldFont;
            if (bold) ImGui::PushFont(bold);

            const float rowTopY = ImGui::GetCursorPosY();
            auto CentreOnRow = [&](float itemH) {
                ImGui::SetCursorPosY(rowTopY + (headerH - itemH) * 0.5f);
            };
            const float lineH = ImGui::GetTextLineHeight();
            constexpr float kNameScale = 1.15f;

            // The side's colour opens the header, ahead of even the trophy: which half of the
            // map a team held is the one thing you cannot read off the names.
            CentreOnRow(lineH);
            TeamRibbonItem(headerH * 0.72f, teamSlot, true);
            ImGui::SameLine(0, 10);

            // The art is 64x109, so the width follows the height or the cup comes out squashed.
            if (isWinner)
            {
                if (ImTextureID cup = GetCupIcon())
                {
                    const float trophyH = headerH;
                    CentreOnRow(trophyH);
                    ImGui::Image(cup, ImVec2(trophyH * (64.0f / 109.0f), trophyH));
                    ImGui::SameLine(0, 12);
                }
            }

            if (guild.rank > 0)
            {
                CentreOnRow(lineH);
                ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
                ImGui::Text("#%d", guild.rank);
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 6);
            }

            CentreOnRow(lineH * kNameScale);
            ImGui::SetWindowFontScale(kNameScale);
            if (DrawFilterLink(guild.display.c_str(), isWinner ? kColorAccent : kColorText))
                FilterBySearchTerm(guild.display);
            const bool guildHovered = ImGui::IsItemHovered();
            ImGui::SetWindowFontScale(1.0f);

            if (bold) ImGui::PopFont();
            ImGui::PopClipRect();

            if (guildHovered)
                ImGui::SetTooltip("Show every match for %s", guild.display.c_str());

            // Every player's template, one chat link a line: a whole team to paste or keep.
            ImGui::TableSetColumnIndex(copyCol);
            const std::string teamKey = "team_" + partyId;
            const float btnSize = std::round(skillIconSize * 0.45f);
            if (DrawCopyIcon(("###cpyteam_" + partyId).c_str(), btnSize, (headerH - btnSize) * 0.5f,
                             feedbackAt(teamKey)))
            {
                std::string all;
                for (const auto* pp : sorted)
                {
                    if (pp->used_skills.empty() && pp->skill_template_code.empty()) continue;
                    const std::string name = pp->encoded_name.empty() ? "(unnamed)" : SanitizePlayerName(pp->encoded_name);
                    all += "[" + name + "-Skills;" + PlayerTemplateCode(*pp, buildOf(*pp)) + "]\n";
                }
                ImGui::SetClipboardText(all.c_str());
                s_copyFeedbackTimes[teamKey] = (float)ImGui::GetTime();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
                ImGui::TextUnformatted("Copy the whole team's templates");
                ImGui::PopStyleColor();
                ImGui::TextColored(kColorTextDim, ob.builds ? "Skills and solved attributes, one player a line"
                                                             : "Skills only until the match has been read");
                ImGui::EndTooltip();
            }
        }

        // Player rows
        for (const auto* pp : sorted)
        {
            const auto& p = *pp;
            const auto* build = buildOf(p);
            const std::string key = keyOf(p);
            const bool isPinned = (s_pinnedCard.folder == m.folder_name && s_pinnedCard.key == key);
            std::string cleanName = p.encoded_name.empty() ? "(unnamed)" : SanitizePlayerName(p.encoded_name);
            ImGui::TableNextRow();

            const float rowH = std::max(skillIconSize, std::max(iconSize, ImGui::GetTextLineHeight()));
            const float profPad = (rowH - iconSize) * 0.5f;
            const float textPad = (rowH - ImGui::GetTextLineHeight()) * 0.5f;

            // The profession icons open the build card on hover and pin it on a click.
            bool showCard = false;
            for (int prof : { p.primary, p.secondary })
            {
                ImGui::TableNextColumn();
                if (profPad > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + profPad);
                ImTextureID ico = GetProfessionIcon(prof);
                if (ico) ImGui::Image(ico, ImVec2(iconSize, iconSize));
                else     ImGui::Dummy(ImVec2(iconSize, iconSize));
                if (ImGui::IsItemHovered())
                {
                    showCard = true;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        if (isPinned) s_pinnedCard = PinnedBuildCard();
                        else s_pinnedCard = { m.folder_name, key, ImVec2(ImGui::GetMousePos().x + 16.0f, ImGui::GetMousePos().y + 8.0f) };
                    }
                }
            }
            if (isPinned)
                ImGui::GetWindowDrawList()->AddRect(
                    ImVec2(ImGui::GetItemRectMin().x - iconSize - 4.0f, ImGui::GetItemRectMin().y - 1.0f),
                    ImVec2(ImGui::GetItemRectMax().x + 1.0f, ImGui::GetItemRectMax().y + 1.0f),
                    ImGui::GetColorU32(kColorAccent), 3.0f);

            ImGui::TableNextColumn();
            if (textPad > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textPad);
            if (DrawFilterLink(cleanName.c_str(), kColorText))
            {
                if (ImGui::GetIO().KeyCtrl) AddSearchTerm(cleanName);
                else                        FilterBySearchTerm(cleanName);
            }
            if (ImGui::IsItemHovered()) showCard = true;

            if (showCard && !isPinned)
            {
                ImGui::BeginTooltip();
                DrawBuildCard(p, cleanName, build, ob.pending, matchView, skillIconSize, false);
                ImGui::EndTooltip();
            }

            if (showSkills)
            {
                ImGui::TableNextColumn();
                DrawPlayerSkillRow(p, matchView, skillIconSize);
            }

            ImGui::TableNextColumn();
            if (!p.used_skills.empty() || !p.skill_template_code.empty())
            {
                const std::string tmplKey = partyId + "_" + std::to_string(p.player_number);
                const float btnSize = std::round(skillIconSize * 0.45f);
                const float fb = feedbackAt(tmplKey);
                if (DrawCopyIcon(("###cpytmpl_" + tmplKey).c_str(), btnSize, (rowH - btnSize) * 0.5f, fb))
                {
                    const std::string chatLink = "[" + cleanName + "-Skills;" + PlayerTemplateCode(p, build) + "]";
                    ImGui::SetClipboardText(chatLink.c_str());
                    s_copyFeedbackTimes[tmplKey] = (float)ImGui::GetTime();
                }
                if (ImGui::IsItemHovered())
                {
                    const bool copied = fb >= 0.0f && (float)ImGui::GetTime() - fb < 1.5f;
                    ImGui::BeginTooltip();
                    ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
                    ImGui::TextUnformatted(copied ? "Copied!" : "Copy template code");
                    ImGui::PopStyleColor();
                    if (!copied)
                        ImGui::TextColored(kColorTextDim, build ? "Skills and the solved attributes (points only, no runes)"
                                                                : "Skills only until the match has been read");
                    ImGui::EndTooltip();
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor(); // kColorStatText
    ImGui::PopStyleVar();

    ImGui::EndGroup();

    // The pinned card stays where it was opened, in a window of its own, until its icon is
    // clicked again, its close button is pressed or another match is selected.
    if (s_pinnedCard.folder == m.folder_name)
        for (const auto* pp : sorted)
        {
            if (keyOf(*pp) != s_pinnedCard.key) continue;
            const std::string cleanName = pp->encoded_name.empty() ? "(unnamed)" : SanitizePlayerName(pp->encoded_name);
            ImGui::SetNextWindowPos(s_pinnedCard.pos, ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
            ImGui::PushStyleColor(ImGuiCol_Border, kColorAccent);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
            if (ImGui::Begin("Build##pinned_build_card", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoMove))
            {
                DrawBuildCard(*pp, cleanName, buildOf(*pp), ob.pending, matchView, skillIconSize, true);
                if (ImGui::SmallButton("Close")) s_pinnedCard = PinnedBuildCard();
            }
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }
}

// ─── Bottom match detail panel ───────────────────────────────────────────────

// The Overview tab: today's panel - map, match facts, notes, lord damage, both teams.
static void DrawMatchOverview(const MatchMeta& m, const OverviewBuilds& ob)
{
    const auto sz = GetSizes(s_state.layout);
    const float sp = sz.spacing;

    std::string ft1, ft2;
    ParseFolderTags(m.folder_name, ft1, ft2);
    GuildLabel g1 = GetPartyGuild(m, "1", ft1);
    GuildLabel g2 = GetPartyGuild(m, "2", ft2);

    float availWidth = ImGui::GetContentRegionAvail().x;
    const float icoH = 16.0f;
    bool stackVertical = (availWidth < 700.0f);

    // ── Left column: map image + match metadata ──
    float mapAreaW = stackVertical ? std::min(sz.mapImg, availWidth * 0.25f) : std::max(120.0f, availWidth * 0.10f);
    float mapImgSize = mapAreaW - sp;

    ImGui::BeginGroup();
    {
        const ImVec2 mapPos = ImGui::GetCursorScreenPos();
        ImTextureID mapIcon = GetMapIcon(m.map_id);
        if (mapIcon)
            ImGui::Image(mapIcon, ImVec2(mapImgSize, mapImgSize));
        else
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(kColorPanel.x, kColorPanel.y, kColorPanel.z, 0.80f));
            ImGui::BeginChild("##map_placeholder", ImVec2(mapImgSize, mapImgSize), ImGuiChildFlags_Border);
            ImGui::SetCursorPos(ImVec2(mapImgSize * 0.15f, mapImgSize * 0.4f));
            ImGui::TextColored(kColorTextDim, "No map");
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        // Keep each cape inside its team's corner of the map, without consuming
        // metadata space. Draw directly so the layout cursor stays below the map.
        if (mapImgSize > 0.0f)
        {
            const float uiScale = ImGui::GetFontSize() / 16.0f;
            const float inset = std::min(6.0f * uiScale, mapImgSize * 0.04f);
            const float capeH = std::min(96.0f * uiScale, mapImgSize * 0.45f);
            const float capeW = capeH * 0.5f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            auto drawCape = [&](ImTextureID tex, const GuildLabel& label, bool right) {
                if (!tex) return;
                const ImVec2 pos(mapPos.x + (right ? mapImgSize - inset - capeW : inset),
                                 mapPos.y + (right ? mapImgSize - inset - capeH : inset));
                const ImVec2 end(pos.x + capeW, pos.y + capeH);
                const float backingPad = inset * 0.5f;
                dl->PushClipRect(mapPos, ImVec2(mapPos.x + mapImgSize, mapPos.y + mapImgSize), true);
                dl->AddRectFilled(ImVec2(pos.x - backingPad, pos.y - backingPad),
                                  ImVec2(end.x + backingPad, end.y + backingPad),
                                  IM_COL32(0, 0, 0, 90), backingPad);
                dl->AddImage(tex, pos, end);
                dl->PopClipRect();

                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                    ImGui::IsMouseHoveringRect(pos, end))
                {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 280.0f * uiScale);
                    ImGui::TextUnformatted(label.display.empty()
                        ? (right ? "Team 2" : "Team 1") : label.display.c_str());
                    ImGui::PopTextWrapPos();
                    // Reserve a square at least as wide as the guild label, then
                    // center the preview in both directions without stretching it.
                    const float previewSide = std::max(192.0f * uiScale, ImGui::GetItemRectSize().x);
                    const ImVec2 previewSize(96.0f * uiScale, 192.0f * uiScale);
                    const ImVec2 previewOrigin = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(previewSide, previewSide));
                    const ImVec2 previewPos(previewOrigin.x + (previewSide - previewSize.x) * 0.5f,
                                            previewOrigin.y + (previewSide - previewSize.y) * 0.5f);
                    ImGui::GetWindowDrawList()->AddImage(tex, previewPos,
                        ImVec2(previewPos.x + previewSize.x, previewPos.y + previewSize.y));
                    ImGui::EndTooltip();
                }
            };
            drawCape(GetGuildCape(m, g1), g1, false);
            drawCape(GetGuildCape(m, g2), g2, true);
        }

        ImGui::Spacing();

        ImFont* bold = GuiGlobalConstants::boldFont;

        // Long map and Flux names stay in the sidebar and grow downward.
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + mapImgSize);

        const char* mapName = GetMapName(m.map_id);
        char dateBuf[16];
        snprintf(dateBuf, sizeof(dateBuf), "%04d/%02d/%02d", m.year, m.month, m.day);

        if (bold) ImGui::PushFont(bold);
        ImGui::TextColored(kColorText, "%s", dateBuf);
        if (bold) ImGui::PopFont();

        ImTextureID ghIcon = GetGuildHallIcon();
        if (ghIcon) { ImGui::Image(ghIcon, ImVec2(icoH, icoH)); ImGui::SameLine(0, 4); }
        if (bold) ImGui::PushFont(bold);
        ImGui::TextColored(kColorAccent, "%s", mapName ? mapName : "Unknown Map");
        if (bold) ImGui::PopFont();

        if (!m.occasion.empty())
        {
            ImTextureID arenaIco = GetArenaIcon();
            if (arenaIco) { ImGui::Image(arenaIco, ImVec2(icoH, icoH)); ImGui::SameLine(0, 4); }
            if (bold) ImGui::PushFont(bold);
            ImGui::TextColored(ImVec4(0.75f, 0.73f, 0.68f, 1.0f), "%s", m.occasion.c_str());
            if (bold) ImGui::PopFont();
        }
        if (!m.match_duration.empty())
        {
            ImTextureID durIco = GetDurationIcon();
            if (durIco) { ImGui::Image(durIco, ImVec2(icoH, icoH)); ImGui::SameLine(0, 4); }
            if (bold) ImGui::PushFont(bold);
            ImGui::TextColored(ImVec4(0.75f, 0.73f, 0.68f, 1.0f), "%s", m.match_duration.c_str());
            if (bold) ImGui::PopFont();
        }
        if (!m.flux.empty())
            DrawFluxWithTooltip(m.flux, icoH);
        ImGui::PopTextWrapPos();

        // ── Rating ──
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
        ImGui::TextUnformatted("Rating");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        {
            int cur = MatchRatings::Get().GetRating(m.folder_name);
            int res = DrawStarRating("##detailRating", cur);
            if (res > 0) MatchRatings::Get().SetRating(m.folder_name, res);
            else if (res == -1) MatchRatings::Get().SetRating(m.folder_name, 0);
        }

        // ── Notes ──
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
        ImGui::TextUnformatted("Notes");
        ImGui::PopStyleColor();
        {
            if (s_state.browserNoteMatchId != m.folder_name)
            {
                s_state.browserNoteBuffer  = MatchNotes::Get().GetNote(m.folder_name);
                s_state.browserNoteMatchId = m.folder_name;
            }
            float editH = ImGui::GetTextLineHeight() * 4 + ImGui::GetStyle().FramePadding.y * 2;
            if (ImGui::InputTextMultiline("##detailNotes", &s_state.browserNoteBuffer,
                                          ImVec2(mapAreaW, editH)))
            {
                MatchNotes::Get().SetNote(m.folder_name, s_state.browserNoteBuffer);
            }
        }

        // ── Lord Damage summary ──
        {
            const auto& ld = m.lord_damage;
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
            ImGui::TextUnformatted("LORD DAMAGE");
            ImGui::PopStyleColor();

            if (!ld.has_data)
            {
                ImGui::TextColored(kColorTextDim, "No lord damage recorded.");
            }
            else
            {
                const ImVec4 colBlue(0.40f, 0.65f, 1.00f, 1.00f);
                const ImVec4 colRed(1.00f, 0.40f, 0.40f, 1.00f);

                std::string labelBlue = g1.tag.empty() ? "Team 1" : "[" + g1.tag + "]";
                std::string labelRed  = g2.tag.empty() ? "Team 2" : "[" + g2.tag + "]";

                float labelW1 = ImGui::CalcTextSize(labelBlue.c_str()).x;
                float labelW2 = ImGui::CalcTextSize(labelRed.c_str()).x;
                float labelColW = (std::max)(labelW1, labelW2) + 6.0f;

                char valBuf1[32], valBuf2[32];
                snprintf(valBuf1, sizeof(valBuf1), "%ld", ld.total_lord_damage_blue);
                snprintf(valBuf2, sizeof(valBuf2), "%ld", ld.total_lord_damage_red);
                float valW1 = ImGui::CalcTextSize(valBuf1).x;
                float valW2 = ImGui::CalcTextSize(valBuf2).x;
                float valColW = (std::max)(valW1, valW2) + 6.0f;

                long maxDmg = (std::max)(ld.total_lord_damage_blue, ld.total_lord_damage_red);
                if (maxDmg <= 0) maxDmg = 1;

                float totalW = mapAreaW;
                float barMaxW = totalW - labelColW - valColW - 8.0f;
                if (barMaxW < 20.0f) barMaxW = 20.0f;
                float barH = ImGui::GetFontSize() + 2.0f;
                float rowH = barH + 3.0f;

                auto DrawDmgRow = [&](const std::string& label, long val, const char* valStr, const ImVec4& barCol)
                {
                    float frac = (maxDmg > 0) ? static_cast<float>(val) / static_cast<float>(maxDmg) : 0.f;
                    ImVec2 rowStart = ImGui::GetCursorScreenPos();
                    float y0 = rowStart.y;

                    if (bold) ImGui::PushFont(bold);
                    ImGui::SetCursorScreenPos(ImVec2(rowStart.x, y0 + 1.0f));
                    ImGui::TextColored(kColorText, "%s", label.c_str());
                    if (bold) ImGui::PopFont();

                    float barX = rowStart.x + labelColW + 2.0f;
                    float barFillW = barMaxW * frac;
                    if (val > 0 && barFillW < 4.0f) barFillW = 4.0f;

                    ImGui::GetWindowDrawList()->AddRectFilled(
                        ImVec2(barX, y0),
                        ImVec2(barX + barFillW, y0 + barH),
                        ImGui::GetColorU32(ImVec4(barCol.x, barCol.y, barCol.z, 0.55f)),
                        2.0f);

                    float numX = rowStart.x + labelColW + barMaxW + 6.0f;
                    if (bold) ImGui::PushFont(bold);
                    ImGui::SetCursorScreenPos(ImVec2(numX, y0 + 1.0f));
                    ImGui::TextColored(kColorText, "%s", valStr);
                    if (bold) ImGui::PopFont();

                    ImGui::SetCursorScreenPos(ImVec2(rowStart.x, y0 + rowH));
                };

                DrawDmgRow(labelBlue, ld.total_lord_damage_blue, valBuf1, colBlue);
                DrawDmgRow(labelRed, ld.total_lord_damage_red, valBuf2, colRed);
            }
        }
    }
    ImGui::EndGroup();

    ImGui::SameLine(0, sp * 2);

    // ── Right side: teams ──
    float teamsW = ImGui::GetContentRegionAvail().x;

    if (stackVertical)
    {
        ImGui::BeginGroup();
        DrawTeamComposition(m, "1", g1, m.winner_party_id == 1, sz, ob);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        DrawTeamComposition(m, "2", g2, m.winner_party_id == 2, sz, ob);
        ImGui::EndGroup();
    }
    else
    {
        float teamW = (teamsW - sp) * 0.5f;

        ImGui::BeginChild("##team1_detail", ImVec2(teamW, 0));
        DrawTeamComposition(m, "1", g1, m.winner_party_id == 1, sz, ob);
        ImGui::EndChild();

        ImGui::SameLine(0, sp);

        ImGui::BeginChild("##team2_detail", ImVec2(teamW, 0));
        DrawTeamComposition(m, "2", g2, m.winner_party_id == 2, sz, ob);
        ImGui::EndChild();
    }
}

// ─── Match detail tabs ───────────────────────────────────────────────────────
// Overview is today's panel. Stats, Graph and Map share one interval, picked on the
// timeline strip along the bottom of the panel. Their content is not built yet: the
// shell exists first so the navigation can be judged on its own.

enum DetailTab { DetailTab_Overview, DetailTab_Stats, DetailTab_Graph, DetailTab_Map, DetailTab_Count };

static const char* const kDetailTabNames[DetailTab_Count] = { "Overview", "Stats", "Graph", "Map" };

// "mm:ss" or "h:mm:ss" -> seconds; 0 when the field is empty or unreadable.
static int ParseDurationSeconds(const std::string& text)
{
    int total = 0, part = 0;
    bool any = false;
    for (char c : text)
    {
        if (c >= '0' && c <= '9') { part = part * 10 + (c - '0'); any = true; }
        else if (c == ':') { total = total * 60 + part; part = 0; }
        else return 0;
    }
    return any ? total * 60 + part : 0;
}

static void FormatClock(int seconds, char* buf, size_t n)
{
    const int a = std::abs(seconds);
    snprintf(buf, n, "%s%d:%02d", seconds < 0 ? "-" : "", a / 60, a % 60);
}

// Framed buttons on the header line. The frame is drawn a few pixels outside the item,
// so the buttons read as buttons without making the header line taller.
static constexpr float kDetailBtnPadX = 10.0f;
static constexpr float kDetailBtnPadY = 3.0f;

static float DetailHeaderButtonWidth(const char* label, float leadW)
{
    ImFont* bold = GuiGlobalConstants::boldFont;
    if (bold) ImGui::PushFont(bold);
    const float textW = ImGui::CalcTextSize(label).x;
    if (bold) ImGui::PopFont();
    return leadW + textW + kDetailBtnPadX * 2.0f;
}

// leadW reserves room left of the label for an icon the caller draws.
static bool DetailHeaderButton(const char* id, const char* label, bool selected, float leadW = 0.0f)
{
    ImFont* bold = GuiGlobalConstants::boldFont;
    const float w = DetailHeaderButtonWidth(label, leadW);
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();

    const ImVec4 bg = selected ? s_themeColors.replayBtnBg
                    : active   ? s_themeColors.replayBtnAct
                    : hovered  ? s_themeColors.replayBtnHov
                               : ImVec4(kColorPanel.x, kColorPanel.y, kColorPanel.z, 0.90f);
    const ImVec4 border = selected ? kColorAccent
                        : hovered  ? ImVec4(kColorAccent.x, kColorAccent.y, kColorAccent.z, 0.60f)
                                   : kColorBorder;
    const ImVec4 text = selected ? kColorAccent : (hovered ? ImVec4(1, 1, 1, 1) : kColorText);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(pos.x, pos.y - kDetailBtnPadY), b(pos.x + w, pos.y + h + kDetailBtnPadY);
    dl->AddRectFilled(a, b, ImGui::GetColorU32(bg), 4.0f);
    dl->AddRect(a, b, ImGui::GetColorU32(border), 4.0f, 0, selected ? 1.5f : 1.0f);
    dl->AddText(bold ? bold : ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(pos.x + kDetailBtnPadX + leadW, pos.y + (active ? 1.0f : 0.0f)),
                ImGui::GetColorU32(text), label);
    return clicked;
}

static void DrawDetailTabs()
{
    for (int t = 0; t < DetailTab_Count; ++t)
    {
        if (t == DetailTab_Graph) continue;      // not built yet; hidden until it is
        ImGui::SameLine(0, t == 0 ? 24.0f : 6.0f);
        ImGui::PushID(t);
        if (DetailHeaderButton("##detail_tab", kDetailTabNames[t], s_state.detailTab == t))
            s_state.detailTab = t;
        ImGui::PopID();
    }
}

static const char* DetailMaximiseLabel()
{
    return s_state.detailMaximised ? "Show list" : "Expand";
}

// Expand gives the details the whole Library; Show list brings the filters and the match
// list back. The chevron points the way the panel's top edge moves.
static void DrawDetailMaximiseButton(ImVec2 pos)
{
    const float chevW = 10.0f;
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = DetailHeaderButton("##detail_maximise", DetailMaximiseLabel(), false, chevW + 6.0f);
    const bool hovered = ImGui::IsItemHovered();
    if (clicked)
        s_state.detailMaximised = !s_state.detailMaximised;
    if (hovered)
        ImGui::SetTooltip(s_state.detailMaximised
            ? "Bring back the filters and the match list"
            : "Hide the match list and give these details the whole Library");

    const ImVec2 r = ImGui::GetItemRectMin();
    const float h = ImGui::GetTextLineHeight();
    const float cx = r.x + kDetailBtnPadX + chevW * 0.5f;
    const float cy = r.y + h * 0.5f;
    const float dy = (s_state.detailMaximised ? 1.0f : -1.0f) * chevW * 0.25f;
    const ImU32 col = ImGui::GetColorU32(hovered ? ImVec4(1, 1, 1, 1) : kColorText);
    const ImVec2 pts[3] = { ImVec2(cx - chevW * 0.5f, cy - dy), ImVec2(cx, cy + dy),
                            ImVec2(cx + chevW * 0.5f, cy - dy) };
    ImGui::GetWindowDrawList()->AddPolyline(pts, 3, col, 0, 2.0f);
}

// ─── Match analysis job ──────────────────────────────────────────────────────
// What the analysis tabs read about the selected match. Opening one of them on a match makes it
// local (downloading it when it is cloud-only, through the replay button's own download), then
// runs `GuildWarsObserver.exe --export-match-analysis` on it in the background. The export
// writes match_analysis.json beside the recording, so a second visit reads that file at once.
// One match at a time: selecting another one stops the export still running for the last.

static constexpr int kMatchAnalysisVersion = 12;   // ReplayWindow::ExportMatchAnalysis kVersion

enum class AnalysisEvent { Death, FlagCapture, MoraleBoost, Victory, ShrineCaptured, ObeliskCapture };

struct MatchAnalysisEvent
{
    float t = 0.0f;        // seconds on the displayed match clock
    AnalysisEvent type = AnalysisEvent::Death;
    int team = 0;          // 1 or 2; 0 when the event names no team
    int prof = 0;          // the dead player's primary profession, for deaths
    int agent = 0;         // the agent it is about (the dead player)
    std::string label;
};

struct MatchAnalysisPlayer
{
    int agent = 0;
    int team = 0;
    int prof = 0;
    std::string name;
    std::vector<int> pos;   // per second of the recording: recorded x, y, z, state (0 alive, 1 dead, 2 absent)
    bool lord = false;      // NPCs: a guild lord
};

struct MatchAnalysisLandmark
{
    std::string kind;       // "lord", "flag_stand", "obelisk_stand"
    int team = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// A state that changes over the match, as [clock time, value] changes in time order.
struct MatchAnalysisSteps
{
    std::vector<std::pair<float, int>> steps;
    int At(float t, int fallback = 0) const
    {
        int v = fallback;
        for (const auto& [st, sv] : steps) { if (st > t) break; v = sv; }
        return v;
    }
};

struct MatchAnalysisObjects
{
    MatchAnalysisSteps standOwner, obeliskOwner, shrineOwner;   // 0 none, 1 blue, 2 red
    bool hasStand = false, hasObelisk = false, hasShrine = false;
    std::vector<MatchAnalysisPlayer> flags;     // team, pos (state 0 on a stand, 1 carried, 2 loose)
    std::vector<MatchAnalysisPlayer> bundles;   // name "seed" / "kit", pos as flags
    std::vector<std::pair<float, std::pair<int, int>>> doors;   // time, (door type, open)
    struct Catapult { float x = 0, y = 0, z = 0, repaired = 0; MatchAnalysisSteps state; };
    std::vector<Catapult> catapults;
};

struct MatchAnalysisJob
{
    enum class State { Idle, Downloading, Analysing, Ready, Failed };
    State state = State::Idle;
    std::string folderName;
    HANDLE process = nullptr;
    std::string error;
    std::vector<MatchAnalysisEvent> events;
    std::vector<unsigned char> healthTeam1, healthTeam2;   // percent, per second of the recording
    int gateSeconds = 0;   // recording seconds before the gate opens (0:00); shown as -1:00 .. 0:00
    std::vector<MatchAnalysisPlayer> npcs;                 // team, name, pos as for players
    MatchAnalysisObjects objects;
    std::vector<MatchAnalysisPlayer> players;
    std::vector<MatchAnalysisLandmark> landmarks;
    int mapId = -1;
    BattleLedger::Data ledger;    // the Damage & Healing lines, times on the match clock
    MatchStats::Records stats;    // the Stats tab's extra columns, times on the match clock
    std::unordered_map<std::string, std::vector<SolvedAttribute>> builds;   // "party:number"
};

static MatchAnalysisJob s_analysis;

static void StopAnalysisProcess()
{
    if (!s_analysis.process) return;
    // Still running means its match is no longer selected. Its output would be valid, but a
    // reader clicking through the list would otherwise leave one export per match behind.
    if (WaitForSingleObject(s_analysis.process, 0) == WAIT_TIMEOUT)
        TerminateProcess(s_analysis.process, 1);
    CloseHandle(s_analysis.process);
    s_analysis.process = nullptr;
}

// The ledger section (ReplayWindow::ExportMatchAnalysis): flat arrays, times in milliseconds on
// the match clock. Player names are the recording's own, not the party bar's.
static void ReadLedger(const nlohmann::json& lj, MatchAnalysisJob& job)
{
    BattleLedger::Data& d = job.ledger;
    d = BattleLedger::Data();
    auto I = [](const nlohmann::json& a, size_t i) { return i < a.size() && a[i].is_number() ? a[i].get<int>() : 0; };
    auto T = [&](const nlohmann::json& a, size_t i) { return (float)I(a, i) / 1000.0f; };
    for (const auto& v : lj.value("players", nlohmann::json::array()))
        if (v.is_number()) d.players.push_back(v.get<int>());
    for (const auto& a : lj.value("agents", nlohmann::json::array()))
    {
        if (!a.is_array() || a.size() < 6) continue;
        BattleLedger::Agent ag;
        ag.name = a[1].is_string() ? a[1].get<std::string>() : std::string();
        ag.team = I(a, 2); ag.prof = I(a, 3); ag.sec = I(a, 4); ag.lord = I(a, 5) != 0;
        d.agents[I(a, 0)] = std::move(ag);
    }
    for (const auto& pl : job.players)
        if (auto it = d.agents.find(pl.agent); it != d.agents.end() && !pl.name.empty())
            it->second.name = SanitizePlayerName(pl.name);
    for (const auto& a : lj.value("lines", nlohmann::json::array()))
    {
        if (!a.is_array() || a.size() < 10) continue;
        BattleLedger::Line ln;
        ln.t = T(a, 0); ln.kind = (BattleLedger::Kind)I(a, 1); ln.src = I(a, 2); ln.dst = I(a, 3);
        ln.amount = I(a, 4); ln.skillId = I(a, 5); ln.rowKey = I(a, 6);
        ln.weaponHit = (I(a, 7) & 1) != 0; ln.estimated = (I(a, 7) & 2) != 0;
        ln.count = I(a, 8); ln.preventGroup = (uint8_t)I(a, 9);
        d.lines.push_back(ln);
    }
    for (const auto& a : lj.value("ints", nlohmann::json::array()))
    {
        if (!a.is_array() || a.size() < 8) continue;
        BattleLedger::Interrupt li;
        li.t = T(a, 0); li.src = I(a, 1); li.dst = I(a, 2); li.skillId = I(a, 3);
        li.outcome = (BattleLedger::Interrupt::Outcome)I(a, 4); li.lostSkill = I(a, 5);
        li.afterMs = I(a, 6); li.reflexMs = I(a, 7);
        d.ints.push_back(li);
    }
    for (const auto& a : lj.value("uses", nlohmann::json::array()))
    {
        if (!a.is_array() || a.size() < 6) continue;
        d.uses[I(a, 0)].push_back({ I(a, 3), I(a, 4), T(a, 1), T(a, 2), (I(a, 5) & 1) != 0, (I(a, 5) & 2) != 0 });
    }
    const auto guilds = lj.value("guilds", nlohmann::json::array()), tags = lj.value("tags", nlohmann::json::array());
    for (int i = 0; i < 2; ++i)
    {
        if (i < (int)guilds.size() && guilds[i].is_string()) d.guild[i] = guilds[i].get<std::string>();
        if (i < (int)tags.size() && tags[i].is_string()) d.tag[i] = tags[i].get<std::string>();
    }
}

static bool LoadMatchAnalysis(const std::filesystem::path& file, MatchAnalysisJob& job)
{
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    const auto root = nlohmann::json::parse(f, nullptr, false);
    if (root.is_discarded() || root.value("version", 0) != kMatchAnalysisVersion) return false;

    static const std::pair<const char*, AnalysisEvent> kTypes[] = {
        { "death",           AnalysisEvent::Death },
        { "flag_capture",    AnalysisEvent::FlagCapture },
        { "morale_boost",    AnalysisEvent::MoraleBoost },
        { "victory",         AnalysisEvent::Victory },
        { "shrine_captured", AnalysisEvent::ShrineCaptured },
        { "obelisk_capture", AnalysisEvent::ObeliskCapture },
    };
    auto& out = job.events;
    out.clear();
    auto readCurve = [&](const char* key, std::vector<unsigned char>& curve) {
        curve.clear();
        for (const auto& v : root.value(key, nlohmann::json::array()))
            curve.push_back((unsigned char)std::clamp(v.is_number() ? v.get<int>() : 0, 0, 100));
    };
    readCurve("health_team1", job.healthTeam1);
    readCurve("health_team2", job.healthTeam2);
    job.mapId = root.value("map_id", -1);
    job.gateSeconds = std::max(0, root.value("gate_seconds", 0));
    auto readAgents = [&](const char* key, std::vector<MatchAnalysisPlayer>& out) {
        out.clear();
        for (const auto& pj : root.value(key, nlohmann::json::array()))
        {
            MatchAnalysisPlayer pl;
            pl.agent = pj.value("agent", 0);
            pl.team = pj.value("team", 0);
            pl.prof = pj.value("prof", 0);
            pl.lord = pj.value("lord", false);
            pl.name = pj.value("name", std::string());
            for (const auto& v : pj.value("pos", nlohmann::json::array()))
                pl.pos.push_back(v.is_number() ? v.get<int>() : 0);
            out.push_back(std::move(pl));
        }
    };
    readAgents("players", job.players);
    readAgents("npcs", job.npcs);

    auto& ob = job.objects;
    ob = MatchAnalysisObjects();
    const auto objs = root.value("objects", nlohmann::json::object());
    auto readSteps = [&](const char* key, MatchAnalysisSteps& out) {
        if (!objs.contains(key)) return false;
        for (const auto& st : objs[key])
            if (st.is_array() && st.size() == 2)
                out.steps.push_back({ st[0].get<float>(), st[1].get<int>() });
        return true;
    };
    ob.hasStand   = readSteps("stand_owner", ob.standOwner);
    ob.hasObelisk = readSteps("obelisk_owner", ob.obeliskOwner);
    ob.hasShrine  = readSteps("shrine_owner", ob.shrineOwner);
    auto readTracks = [&](const char* key, std::vector<MatchAnalysisPlayer>& out) {
        for (const auto& fj : objs.value(key, nlohmann::json::array()))
        {
            MatchAnalysisPlayer f;
            f.team = fj.value("team", 0);
            f.name = fj.value("type", std::string());
            for (const auto& v : fj.value("pos", nlohmann::json::array()))
                f.pos.push_back(v.is_number() ? v.get<int>() : 0);
            out.push_back(std::move(f));
        }
    };
    readTracks("flags", ob.flags);
    readTracks("bundles", ob.bundles);
    for (const auto& d : objs.value("doors", nlohmann::json::array()))
        if (d.is_array() && d.size() == 3)
            ob.doors.push_back({ d[0].get<float>(), { d[1].get<int>(), d[2].get<int>() } });
    for (const auto& cj : objs.value("catapults", nlohmann::json::array()))
    {
        MatchAnalysisObjects::Catapult c;
        c.x = cj.value("x", 0.0f); c.y = cj.value("y", 0.0f); c.z = cj.value("z", 0.0f);
        c.repaired = cj.value("repaired", 0.0f);
        for (const auto& st : cj.value("states", nlohmann::json::array()))
            if (st.is_array() && st.size() == 2)
                c.state.steps.push_back({ st[0].get<float>(), st[1].get<int>() });
        ob.catapults.push_back(std::move(c));
    }
    job.landmarks.clear();
    for (const auto& lj : root.value("landmarks", nlohmann::json::array()))
    {
        MatchAnalysisLandmark lm;
        lm.kind = lj.value("kind", std::string());
        lm.team = lj.value("team", 0);
        lm.x = lj.value("x", 0.0f);
        lm.y = lj.value("y", 0.0f);
        lm.z = lj.value("z", 0.0f);
        job.landmarks.push_back(std::move(lm));
    }
    ReadLedger(root.value("ledger", nlohmann::json::object()), job);
    job.builds.clear();
    if (root.contains("attributes") && root["attributes"].is_object())
    {
        const nlohmann::json players = root["attributes"].value("players", nlohmann::json::object());
        for (const auto& [key, pj] : players.items())
        {
            const nlohmann::json attrs = pj.value("attributes", nlohmann::json::object());
            const nlohmann::json runes = pj.value("runes", nlohmann::json::object());
            const int head = pj.value("headgear", -1);
            auto& build = job.builds[key];
            for (const auto& [idStr, aj] : attrs.items())
            {
                SolvedAttribute a;
                a.id = std::atoi(idStr.c_str());
                a.lo = aj.value("lo", 0); a.hi = aj.value("hi", 0); a.best = aj.value("best", 0);
                a.budgetOnly = aj.value("budget_only", true);
                a.rune = runes.value(idStr, 0);
                a.headgear = (head == a.id);
                build.push_back(a);
            }
        }
    }
    // The extra columns (ReplayWindow::ExportMatchAnalysis "stats"): [ms, agent, value, dur ms, other, skill, flag].
    // Held in a variable: items() over value()'s temporary would iterate a destroyed copy.
    job.stats.clear();
    const nlohmann::json statsJson = root.value("stats", nlohmann::json::object());
    for (const auto& [key, list] : statsJson.items())
    {
        auto& recs = job.stats[key];
        for (const auto& a : list)
        {
            if (!a.is_array() || a.size() < 7) continue;
            MatchStats::Record r;
            r.t = a[0].get<float>() / 1000.0f; r.agent = a[1].get<int>(); r.value = a[2].get<float>();
            r.dur = a[3].get<float>() / 1000.0f; r.other = a[4].get<int>(); r.skill = a[5].get<int>(); r.flag = a[6].get<int>();
            recs.push_back(r);
        }
    }
    for (const auto& e : root.value("events", nlohmann::json::array()))
    {
        const std::string type = e.value("type", std::string());
        // Resurrections, lord alarms, flag returns and catapult shots are left out: on a strip
        // this small they would bury the deaths and captures a reader picks a fight by.
        for (const auto& [name, kind] : kTypes)
        {
            if (type != name) continue;
            MatchAnalysisEvent ev;
            ev.t = e.value("t", 0.0f);
            ev.type = kind;
            ev.team = e.value("team", 0);
            ev.prof = e.value("prof", 0);
            ev.agent = e.value("agent", 0);
            ev.label = e.value("label", std::string());
            out.push_back(std::move(ev));
            break;
        }
    }
    return true;
}

static bool StartAnalysisProcess(const std::filesystem::path& folder)
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;

    std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" --export-match-analysis \""
                     + folder.wstring() + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, std::filesystem::path(exePath).parent_path().c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    s_analysis.process = pi.hProcess;
    return true;
}

static void ForgetMapPicture();   // Map tab, below

// Advances the job for the selected match by one step. Called every frame an analysis tab
// is showing.
static void UpdateMatchAnalysis(const MatchMeta& m)
{
    using State = MatchAnalysisJob::State;
    if (s_analysis.folderName != m.folder_name)
    {
        StopAnalysisProcess();
        s_analysis = MatchAnalysisJob();
        s_analysis.folderName = m.folder_name;
        ForgetMapPicture();
        s_state.detailPlaying = false;
    }

    switch (s_analysis.state)
    {
    case State::Idle:
    {
        if (m.is_cloud_only)
        {
            // The replay button's download is single-slot: wait for one already running.
            if (g_cloudDownloadInProgress || g_pendingReplay.requested) return;
            g_pendingReplay.requested = true;
            g_pendingReplay.match = m;
            g_pendingReplay.openReplay = false;
            s_analysis.state = State::Downloading;
            return;
        }
        const std::filesystem::path folder(m.folder_path);
        if (LoadMatchAnalysis(folder / "match_analysis.json", s_analysis))
        {
            s_analysis.state = State::Ready;
            return;
        }
        if (StartAnalysisProcess(folder))
            s_analysis.state = State::Analysing;
        else
        {
            s_analysis.error = "Could not start the match reader.";
            s_analysis.state = State::Failed;
        }
        return;
    }
    case State::Downloading:
        // The rescan after the download replaces this match's entry with the local one.
        if (!m.is_cloud_only)
            s_analysis.state = State::Idle;
        else if (!g_cloudDownloadInProgress && !g_pendingReplay.requested)
        {
            s_analysis.error = "The download did not complete.";
            s_analysis.state = State::Failed;
        }
        return;
    case State::Analysing:
    {
        if (WaitForSingleObject(s_analysis.process, 0) == WAIT_TIMEOUT) return;
        DWORD code = 1;
        GetExitCodeProcess(s_analysis.process, &code);
        CloseHandle(s_analysis.process);
        s_analysis.process = nullptr;
        if (code == 0 && LoadMatchAnalysis(std::filesystem::path(m.folder_path) / "match_analysis.json",
                                           s_analysis))
            s_analysis.state = State::Ready;
        else
        {
            RunLog::Line("library: match analysis failed for %s (exit code %lu)",
                         m.folder_name.c_str(), code);
            s_analysis.error = code == 5 ? "This recording holds no combat to read."
                                         : "The match could not be read.";
            s_analysis.state = State::Failed;
        }
        return;
    }
    case State::Ready:
    case State::Failed:
        return;
    }
}

static void DrawDetailTabPlaceholder(int tab)
{
    static const char* const kWhat[DetailTab_Count] = {
        "",
        "Every player against the stats you pick, for the selected interval.",
        "One stat over time: a line per player, per skill or per team.",
        "The replay minimap, played over the selected interval.",
    };
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImFont* bold = GuiGlobalConstants::boldFont;

    if (bold) ImGui::PushFont(bold);
    const ImVec2 titleSz = ImGui::CalcTextSize(kDetailTabNames[tab]);
    if (bold) ImGui::PopFont();
    const ImVec2 lineSz = ImGui::CalcTextSize(kWhat[tab]);

    // Where the match data stands; the tab itself is not built yet.
    using State = MatchAnalysisJob::State;
    char note[160];
    switch (s_analysis.state)
    {
    case State::Downloading: snprintf(note, sizeof(note), "Downloading the match..."); break;
    case State::Analysing:   snprintf(note, sizeof(note), "Reading the match..."); break;
    case State::Ready:       snprintf(note, sizeof(note), "%d match events read. Not built yet.",
                                      (int)s_analysis.events.size()); break;
    case State::Failed:      snprintf(note, sizeof(note), "%s", s_analysis.error.c_str()); break;
    default:                 snprintf(note, sizeof(note), "Not built yet."); break;
    }
    const ImVec2 noteSz = ImGui::CalcTextSize(note);
    const bool failed = (s_analysis.state == State::Failed);
    const float blockH = titleSz.y + lineSz.y + noteSz.y + 8.0f + (failed ? noteSz.y + 10.0f : 0.0f);

    const ImVec2 origin = ImGui::GetCursorPos();
    const float top = origin.y + std::max(0.0f, (avail.y - blockH) * 0.5f);

    ImGui::SetCursorPos(ImVec2(origin.x + std::max(0.0f, (avail.x - titleSz.x) * 0.5f), top));
    if (bold) ImGui::PushFont(bold);
    ImGui::TextColored(kColorAccent, "%s", kDetailTabNames[tab]);
    if (bold) ImGui::PopFont();
    ImGui::SetCursorPosX(origin.x + std::max(0.0f, (avail.x - lineSz.x) * 0.5f));
    ImGui::TextColored(kColorText, "%s", kWhat[tab]);
    ImGui::SetCursorPosX(origin.x + std::max(0.0f, (avail.x - noteSz.x) * 0.5f));
    ImGui::TextColored(failed ? ImVec4(1.0f, 0.45f, 0.40f, 1.0f) : kColorTextDim, "%s", note);
    if (failed)
    {
        const float retryW = ImGui::CalcTextSize("Retry").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(origin.x + std::max(0.0f, (avail.x - retryW) * 0.5f));
        if (ImGui::Button("Retry"))
            s_analysis.state = State::Idle;
    }
}

static ImTextureID GetOthersUiIcon(const char* file)
{
    EnsureTextureBasePath();
    return GetTextureCache().GetTexture(ui::TextureBasePath() + "\\Textures\\Others_UI\\" + file);
}

// The interval every analysis tab reads, on a small copy of the replay's Event Timeline: both
// teams' health over the match, deaths as profession icons on their team's curve, flag captures,
// morale boosts and the victory in a row along the bottom. Drag across the chart to pick a fight,
// or move the green (start) and red (end) handles on the track under it. Outside the interval
// the chart is dimmed.
static void DrawDetailTimeline(const MatchMeta& m, float height)
{
    using State = MatchAnalysisJob::State;
    const bool ready = s_analysis.state == State::Ready && s_analysis.folderName == m.folder_name;
    // The span runs from the start of the recording (-1:00, the minute before the gate opens)
    // to the end of the match; the interval handles are fractions of it.
    const int pre = ready ? s_analysis.gateSeconds : 0;
    int duration = ParseDurationSeconds(m.match_duration);
    if (duration <= 0 && ready)
        duration = (int)std::max(s_analysis.healthTeam1.size(), s_analysis.healthTeam2.size()) - pre;
    const int span = std::max(1, duration + pre);
    float& from = s_state.detailIntervalFrom;
    float& to   = s_state.detailIntervalTo;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    ImFont* bold = GuiGlobalConstants::boldFont;
    const float fs = ImGui::GetFontSize();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    // Same palette as the replay's Event Timeline.
    const ImU32 cChartBg  = IM_COL32( 10,  14,  18, 166);
    const ImU32 cBorderHi = IM_COL32(200, 168,  75,  82);
    const ImU32 cGrid     = IM_COL32(255, 255, 255,   8);
    const ImU32 cTextDim  = IM_COL32(120, 108,  80, 255);
    const ImU32 cBlue     = IM_COL32( 74, 144, 216, 255);
    const ImU32 cBlueFill = IM_COL32( 74, 144, 216,  18);
    const ImU32 cRed      = IM_COL32(208,  72,  72, 255);
    const ImU32 cRedFill  = IM_COL32(208,  72,  72,  18);
    const ImU32 cStart    = IM_COL32( 64, 200,  96, 255);
    const ImU32 cEnd      = IM_COL32(220,  72,  72, 255);

    // ── Layout: controls on the left, chart + track + clock on the right ──
    const float leftW   = 150.0f;
    const float x0      = p0.x + leftW;
    const float x1      = p0.x + w - 4.0f;
    const float chartW  = std::max(40.0f, x1 - x0);
    const float labelFs = fs * 0.75f;
    const float chartY0 = p0.y + 6.0f;
    const float trackH  = 16.0f;
    const float chartY1 = p0.y + height - trackH - labelFs - 4.0f;
    const float chartH  = std::max(20.0f, chartY1 - chartY0);
    const float trackY  = chartY1 + trackH * 0.5f;
    auto xAt = [&](float frac) { return x0 + chartW * std::clamp(frac, 0.0f, 1.0f); };

    // ── Left column: title, interval, Full match ──
    char fromBuf[16], toBuf[16], lenBuf[16];
    const int fromS = (int)std::lround(from * span) - pre;
    const int toS   = (int)std::lround(to * span) - pre;
    FormatClock(fromS, fromBuf, sizeof(fromBuf));
    FormatClock(toS, toBuf, sizeof(toBuf));
    FormatClock(toS - fromS, lenBuf, sizeof(lenBuf));
    {
        float y = chartY0;
        if (bold) ImGui::PushFont(bold);
        dl->AddText(ImVec2(p0.x, y), ImGui::GetColorU32(kColorAccent), "TIMELINE");
        if (bold) ImGui::PopFont();
        y += fs + 8.0f;
        if (duration > 0)
        {
            char span[48];
            snprintf(span, sizeof(span), "%s - %s", fromBuf, toBuf);
            dl->AddText(ImVec2(p0.x, y), ImGui::GetColorU32(kColorText), span);
            y += fs + 2.0f;
            char len[32];
            snprintf(len, sizeof(len), "%s selected", lenBuf);
            dl->AddText(ImVec2(p0.x, y), ImGui::GetColorU32(kColorTextDim), len);
            y += fs + 8.0f;
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x, y));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 3.0f));
        if (ImGui::Button("Full match")) { from = 0.0f; to = 1.0f; }
        ImGui::PopStyleVar();
    }

    // ── Chart background, grid ──
    dl->AddRectFilled(ImVec2(x0, chartY0), ImVec2(x1, chartY1), cChartBg, 4.0f);
    dl->AddRect(ImVec2(x0, chartY0), ImVec2(x1, chartY1), cBorderHi, 4.0f);
    for (int pct = 25; pct < 100; pct += 25)
    {
        const float y = chartY1 - pct / 100.0f * chartH;
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), cGrid);
    }

    if (!ready || duration <= 0)
    {
        const char* msg = s_analysis.state == State::Downloading ? "Downloading the match..."
                        : s_analysis.state == State::Analysing   ? "Reading the match..."
                        : s_analysis.state == State::Failed      ? s_analysis.error.c_str()
                        : duration <= 0                          ? "Match length unknown"
                                                                 : "";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(x0 + (chartW - ts.x) * 0.5f, chartY0 + (chartH - ts.y) * 0.5f), cTextDim, msg);
    }

    // ── Health curves ──
    dl->PushClipRect(ImVec2(x0, chartY0), ImVec2(x1, chartY1), true);
    auto healthY = [&](const std::vector<unsigned char>& curve, float t) {
        if (curve.empty()) return chartY1 - chartH * 0.5f;
        const int s = std::clamp((int)(t + pre), 0, (int)curve.size() - 1);
        return chartY1 - curve[s] / 100.0f * chartH;
    };
    if (ready && duration > 0)
    {
        auto drawCurve = [&](const std::vector<unsigned char>& curve, ImU32 line, ImU32 fill) {
            for (size_t s = 0; s + 1 < curve.size(); ++s)
            {
                const float ax = xAt((float)s / span), bx = xAt((float)(s + 1) / span);
                const float ay = chartY1 - curve[s] / 100.0f * chartH;
                const float by = chartY1 - curve[s + 1] / 100.0f * chartH;
                const ImVec2 quad[4] = { ImVec2(ax, ay), ImVec2(bx, by), ImVec2(bx, chartY1), ImVec2(ax, chartY1) };
                dl->AddConvexPolyFilled(quad, 4, fill);
                dl->AddLine(ImVec2(ax, ay), ImVec2(bx, by), line, 1.0f);
            }
        };
        drawCurve(s_analysis.healthTeam2, cRed, cRedFill);
        drawCurve(s_analysis.healthTeam1, cBlue, cBlueFill);
    }

    // ── Event markers ──
    const float iconSz = std::clamp(chartH * 0.22f, 14.0f, 22.0f);
    const float bottomRowY = chartY1 - iconSz * 0.5f - 2.0f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const MatchAnalysisEvent* hovered = nullptr;
    if (ready && duration > 0)
    {
        struct Placed { const MatchAnalysisEvent* e; float x, y; };
        std::vector<Placed> placed;
        placed.reserve(s_analysis.events.size());
        for (const auto& e : s_analysis.events)
        {
            const float x = xAt((e.t + pre) / span);
            float y = bottomRowY;
            if (e.type == AnalysisEvent::Death)
            {
                // On the curve of the team that lost the player; deaths that would overlap
                // stack downwards, as in the replay.
                y = healthY(e.team == 2 ? s_analysis.healthTeam2 : s_analysis.healthTeam1, e.t);
                y = std::max(y, chartY0 + iconSz * 0.5f + 1.0f);
                int stack = 0;
                for (const auto& p : placed)
                    if (p.e->type == AnalysisEvent::Death && std::fabs(p.x - x) < iconSz + 2.0f) stack++;
                y = std::min(y + stack * (iconSz + 2.0f), bottomRowY - iconSz - 2.0f);
            }
            else if (e.type == AnalysisEvent::MoraleBoost)
            {
                // Lifted when a capture shares its spot on the bottom row.
                for (const auto& p : placed)
                    if (p.e->type != AnalysisEvent::Death && std::fabs(p.x - x) < iconSz) { y -= iconSz + 2.0f; break; }
            }
            placed.push_back({ &e, x, y });
        }

        for (const auto& p : placed)
        {
            const auto& e = *p.e;
            float half = iconSz * 0.5f;
            const bool over = std::fabs(mouse.x - p.x) <= half + 2.0f && std::fabs(mouse.y - p.y) <= half + 2.0f;
            if (over) { hovered = &e; half *= 1.3f; }
            const ImVec2 a(p.x - half, p.y - half), b(p.x + half, p.y + half);
            const ImU32 teamCol = e.team == 2 ? cRed : cBlue;

            ImTextureID tex = nullptr;
            switch (e.type)
            {
            case AnalysisEvent::Death:
                dl->AddRectFilled(a, b, IM_COL32(10, 10, 10, 230), 2.0f);
                if (ImTextureID prof = GetProfessionIcon(e.prof)) dl->AddImage(prof, a, b);
                dl->AddLine(ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, b.y - 2), IM_COL32(220, 40, 40, 120), 1.5f);
                dl->AddLine(ImVec2(b.x - 2, a.y + 2), ImVec2(a.x + 2, b.y - 2), IM_COL32(220, 40, 40, 120), 1.5f);
                dl->AddRect(a, b, teamCol, 2.0f, 0, 2.0f);
                continue;
            case AnalysisEvent::FlagCapture:
                tex = GetOthersUiIcon(e.team == 2 ? "Red_flag_waving.svg.png" : "Blue_flag_waving.svg.png"); break;
            case AnalysisEvent::MoraleBoost:
                tex = GetOthersUiIcon(e.team == 2 ? "redmorale.png" : "bluemorale.png"); break;
            case AnalysisEvent::Victory:
                tex = GetOthersUiIcon("cup.webp"); break;
            case AnalysisEvent::ShrineCaptured:
                tex = GetOthersUiIcon("Health_Shrine_Bonus.jpg"); break;
            case AnalysisEvent::ObeliskCapture:
                tex = GetOthersUiIcon("Obelisk_Lightning.jpg"); break;
            }
            if (tex) dl->AddImage(tex, a, b);
            else     dl->AddCircleFilled(ImVec2(p.x, p.y), half * 0.6f, teamCol);
            if (e.type == AnalysisEvent::ShrineCaptured || e.type == AnalysisEvent::ObeliskCapture)
                dl->AddRect(a, b, teamCol, 2.0f, 0, 1.5f);
        }
    }

    // ── The interval: dim outside it, mark its edges ──
    const float xFrom = xAt(from), xTo = xAt(to);
    const ImU32 cDim = IM_COL32(0, 0, 0, 120);
    if (xFrom > x0) dl->AddRectFilled(ImVec2(x0, chartY0), ImVec2(xFrom, chartY1), cDim);
    if (xTo < x1)   dl->AddRectFilled(ImVec2(xTo, chartY0), ImVec2(x1, chartY1), cDim);
    dl->PopClipRect();
    if (from > 0.0f) dl->AddLine(ImVec2(xFrom, chartY0), ImVec2(xFrom, chartY1), cStart, 1.5f);
    if (to < 1.0f)   dl->AddLine(ImVec2(xTo, chartY0), ImVec2(xTo, chartY1), cEnd, 1.5f);
    if (s_state.detailTab == DetailTab_Map && ready && duration > 0)
    {
        const float xp = xAt((s_state.detailPlayhead + pre) / span);
        dl->AddLine(ImVec2(xp, chartY0), ImVec2(xp, chartY1 + trackH * 0.5f), IM_COL32(255, 215, 100, 220), 2.0f);
        // A knob on top, as on the replay's scrub track, so the line reads as something to grab.
        dl->AddCircleFilled(ImVec2(xp, chartY0 + 4.0f), 5.0f, IM_COL32(226, 194, 106, 255));
        dl->AddCircle(ImVec2(xp, chartY0 + 4.0f), 5.0f, IM_COL32(255, 255, 220, 100), 0, 1.5f);
    }

    // ── Track + clock ──
    dl->AddRectFilled(ImVec2(x0, trackY - 2.0f), ImVec2(x1, trackY + 2.0f),
                      ImGui::GetColorU32(ImVec4(kColorPanel.x, kColorPanel.y, kColorPanel.z, 1.0f)), 2.0f);
    dl->AddRectFilled(ImVec2(xFrom, trackY - 3.0f), ImVec2(xTo, trackY + 3.0f),
                      ImGui::GetColorU32(ImVec4(kColorAccent.x, kColorAccent.y, kColorAccent.z, 0.70f)), 2.0f);
    if (duration > 0)
    {
        const int step = duration < 600 ? 60 : duration < 1500 ? 120 : 300;
        for (int s = pre > 0 ? -pre : 0; s <= duration; s = s < 0 ? 0 : s + step)
        {
            const float x = xAt((float)(s + pre) / span);
            char clock[16];
            FormatClock(s, clock, sizeof(clock));
            const ImVec2 ts = font->CalcTextSizeA(labelFs, FLT_MAX, 0.0f, clock);
            const float lx = std::clamp(x - ts.x * 0.5f, x0, x1 - ts.x);
            dl->AddText(font, labelFs, ImVec2(lx, chartY1 + trackH + 1.0f), cTextDim, clock);
        }
    }
    auto drawHandle = [&](float x, ImU32 col) {
        dl->AddTriangleFilled(ImVec2(x - 6.0f, trackY - 8.0f), ImVec2(x + 6.0f, trackY - 8.0f),
                              ImVec2(x, trackY + 1.0f), col);
    };
    drawHandle(xFrom, cStart);
    drawHandle(xTo, cEnd);

    // ── Input ──
    const float minSpan = duration > 0 ? std::min(1.0f, 5.0f / span) : 0.01f;
    const float mouseFrac = std::clamp((mouse.x - x0) / chartW, 0.0f, 1.0f);

    // Drag across the chart: a new interval from the press to the mouse. On the Map tab the
    // chart is also the scrubber: a click puts the playhead there, and the playhead line itself
    // can be dragged.
    const bool scrubs = s_state.detailTab == DetailTab_Map && ready && duration > 0;
    const float xPlay = xAt((s_state.detailPlayhead + pre) / span);
    static bool s_chartDragged = false, s_playheadDrag = false;
    auto setPlayhead = [&](float frac) {
        s_state.detailPlayhead = std::clamp(frac, from, to) * span - pre;
    };
    ImGui::SetCursorScreenPos(ImVec2(x0, chartY0));
    ImGui::InvisibleButton("##detail_chart", ImVec2(chartW, chartH));
    if (ImGui::IsItemActivated())
    {
        s_state.detailChartAnchor = mouseFrac;
        s_chartDragged = false;
        s_playheadDrag = scrubs && std::fabs(mouse.x - xPlay) <= 5.0f;
    }
    if (ImGui::IsItemActive() && s_playheadDrag)
        setPlayhead(mouseFrac);
    else if (ImGui::IsItemActive() && s_state.detailChartAnchor >= 0.0f
        && (s_chartDragged || std::fabs(mouseFrac - s_state.detailChartAnchor) * chartW > 3.0f))
    {
        s_chartDragged = true;
        from = std::min(s_state.detailChartAnchor, mouseFrac);
        to   = std::max(std::max(s_state.detailChartAnchor, mouseFrac), from + minSpan);
        to   = std::min(1.0f, to);
    }
    if (ImGui::IsItemDeactivated())
    {
        if (scrubs && !s_chartDragged && !s_playheadDrag)
            setPlayhead(mouseFrac);
        s_state.detailChartAnchor = -1.0f;
        s_playheadDrag = false;
    }
    if (scrubs && (s_playheadDrag || (ImGui::IsItemHovered() && std::fabs(mouse.x - xPlay) <= 5.0f)))
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    const bool chartActive = ImGui::IsItemActive();
    const bool chartHovered = ImGui::IsItemHovered();

    // The track: a press picks the nearer handle, which then follows the mouse.
    ImGui::SetCursorScreenPos(ImVec2(x0 - 8.0f, chartY1));
    ImGui::InvisibleButton("##detail_interval", ImVec2(chartW + 16.0f, trackH));
    if (ImGui::IsItemActivated())
        s_state.detailIntervalDrag = (std::fabs(mouseFrac - from) <= std::fabs(mouseFrac - to)) ? 0 : 1;
    if (ImGui::IsItemActive() && s_state.detailIntervalDrag >= 0)
    {
        if (s_state.detailIntervalDrag == 0) from = std::max(0.0f, std::min(mouseFrac, to - minSpan));
        else                                 to   = std::min(1.0f, std::max(mouseFrac, from + minSpan));
    }
    if (ImGui::IsItemDeactivated())
        s_state.detailIntervalDrag = -1;
    const bool trackActive = ImGui::IsItemActive();
    const bool trackHovered = ImGui::IsItemHovered();

    // ── Tooltips ──
    if (chartActive && s_playheadDrag)
    {
        char clock[16];
        FormatClock((int)std::floor(s_state.detailPlayhead), clock, sizeof(clock));
        ImGui::SetTooltip("%s", clock);
    }
    else if ((chartActive && s_chartDragged) || trackActive)
        ImGui::SetTooltip("%s - %s  (%s)", fromBuf, toBuf, lenBuf);
    else if (hovered)
    {
        char clock[16];
        FormatClock((int)hovered->t, clock, sizeof(clock));
        const ImVec4 col = hovered->team == 2 ? ImVec4(1.00f, 0.42f, 0.42f, 1.0f)
                         : hovered->team == 1 ? ImVec4(0.45f, 0.70f, 1.00f, 1.0f) : kColorAccent;
        ImGui::BeginTooltip();
        ImGui::TextColored(kColorTextDim, "%s", clock);
        ImGui::SameLine();
        if (hovered->type == AnalysisEvent::Death)
            ImGui::TextColored(col, "%s died", hovered->label.c_str());
        else
            ImGui::TextColored(col, "%s", hovered->label.c_str());
        ImGui::EndTooltip();
    }
    else if (chartHovered && duration > 0)
    {
        char clock[16];
        FormatClock((int)std::lround(mouseFrac * span) - pre, clock, sizeof(clock));
        ImGui::SetTooltip(scrubs ? "%s\nClick to move the playhead, drag to select a fight"
                                 : "%s\nDrag to select a fight", clock);
    }
    else if (trackHovered)
        ImGui::SetTooltip("Drag a handle to move the start or the end");

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + height));
}


// ─── Map tab ─────────────────────────────────────────────────────────────────
// The match from above over the selected interval: the map's top-down picture (shipped in
// Textures/Map_Pictures, made with the GWO_MAP_PICTURES batch and cropped to the map; a map
// missing from that set is captured into the user folder by the first replay of it -
// ReplayWindow::CaptureTopDownMapImage), every player drawn as the replay minimap draws them,
// with the path they took since the start of the interval, guild lords and flag stands.
// Plays inside the interval, on the replay's own keys (ReplayHotkeys); the playhead shows on
// the timeline too.

struct MapPicture
{
    int mapId = -1;
    bool loaded = false;           // the picture and its placement were read
    double checkedAt = 0.0;        // when a missing picture was last looked for
    std::string png;
    float u[4] = {}, v[4] = {};    // recorded position -> 0..1 across the picture
    float aspect = 1.0f;           // width / height
    struct Gate { int door = 0; float u = 0, v = 0; bool bridge = false; };
    std::vector<Gate> gates;       // lever gates, Meditation gates, vine bridges
};

static MapPicture s_mapPicture;

static const MapPicture& GetMapPicture(int mapId)
{
    // A missing picture is looked for again every few seconds: a replay opened meanwhile may
    // have just captured it.
    const double now = ImGui::GetTime();
    if (s_mapPicture.mapId == mapId && (s_mapPicture.loaded || now - s_mapPicture.checkedAt < 2.0))
        return s_mapPicture;
    s_mapPicture = MapPicture();
    s_mapPicture.mapId = mapId;
    s_mapPicture.checkedAt = now;

    // Shipped pictures are JPEG (the batch's PNGs, converted to keep the download small); a
    // picture captured on the user's machine stays PNG.
    const std::string name = std::to_string(mapId);
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(ui::TextureBasePath()) / L"Textures" / L"Map_Pictures";
    std::filesystem::path png = dir / (name + ".jpg");
    if (!std::filesystem::exists(png, ec))
    {
        dir = GuiGlobalConstants::GetUserDataDir() / L"MapImages";
        png = dir / (name + ".png");
    }
    std::ifstream f(dir / (name + ".json"), std::ios::binary);
    if (!f || !std::filesystem::exists(png, ec)) return s_mapPicture;
    const auto j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.contains("u") || !j.contains("v")) return s_mapPicture;
    for (int i = 0; i < 4; ++i)
    {
        s_mapPicture.u[i] = j["u"][i].get<float>();
        s_mapPicture.v[i] = j["v"][i].get<float>();
    }
    for (const auto& g : j.value("gates", nlohmann::json::array()))
        s_mapPicture.gates.push_back({ g.value("door", 0), g.value("u", 0.0f), g.value("v", 0.0f),
                                       g.value("kind", std::string()) == "bridge" });
    const float w = j.value("width", 1024.0f), h = j.value("height", 1024.0f);
    s_mapPicture.aspect = h > 0.0f ? w / h : 1.0f;
    s_mapPicture.png = png.string();
    s_mapPicture.loaded = true;
    return s_mapPicture;
}

// Forget the cached lookup, so a picture captured since (by a replay) is found.
static void ForgetMapPicture() { s_mapPicture.mapId = -1; }

static constexpr int kMapPlaySpeeds[] = { 1, 2, 4, 8, 16 };

// The replay's transport keys, read from the same bindings (Preferences > Hotkeys).
static void HandleMapTabHotkeys(float fromT, float toT)
{
    if (ImGui::GetIO().WantTextInput) return;
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;

    const auto& hk = ReplayHotkeys::Get();
    float& t = s_state.detailPlayhead;
    auto pressed = [](int key, bool repeat = false) {
        return key > 0 && ImGui::IsKeyPressed((ImGuiKey)key, repeat);
    };

    if (pressed(hk.playPause))
    {
        if (!s_state.detailPlaying && t >= toT - 0.01f) t = fromT;
        s_state.detailPlaying = !s_state.detailPlaying;
    }

    // As in the replay: the 5 s keys, unless bound to an arrow; arrows step 1 s and repeat.
    const bool rewIsArrow = hk.rewind5s == ImGuiKey_LeftArrow || hk.rewind5s == ImGuiKey_RightArrow;
    const bool fwdIsArrow = hk.forward5s == ImGuiKey_LeftArrow || hk.forward5s == ImGuiKey_RightArrow;
    if (!rewIsArrow && pressed(hk.rewind5s))  t -= 5.0f;
    if (!fwdIsArrow && pressed(hk.forward5s)) t += 5.0f;
    if (pressed(ImGuiKey_LeftArrow, true))  t -= 1.0f;
    if (pressed(ImGuiKey_RightArrow, true)) t += 1.0f;
    t = std::clamp(t, fromT, toT);

    const int step = (pressed(hk.speedUp) ? 1 : 0) - (pressed(hk.speedDown) ? 1 : 0);
    if (step != 0)
    {
        const int n = (int)std::size(kMapPlaySpeeds);
        int i = 0;
        while (i < n - 1 && kMapPlaySpeeds[i] < (int)s_state.detailPlaySpeed) ++i;
        s_state.detailPlaySpeed = (float)kMapPlaySpeeds[std::clamp(i + step, 0, n - 1)];
    }
}

// The Map tab's play bar: the replay window's transport row (ReplayWindow_TimelineController.cpp),
// its glass panel and buttons, over the interval. The timeline under it is the scrub track.
static constexpr float kMapPlayBarH = 44.0f;

static void DrawMapPlayBar(ImVec2 pos, float width, float fromT, float toT, float& t)
{
    const float barH = kMapPlayBarH, PAD = 12.f;
    const float BTN_H = 30.f, PLAY_SZ = 36.f, BGAP = 2.f, GDIV = 10.f;
    const float d2r = IM_PI / 180.f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = font->FontSize;
    const float fsSm = fs * 0.82f;

    // The replay bar's dark-glass gold palette.
    const ImU32 cGlass      = IM_COL32( 18,  16,  10, 150);
    const ImU32 cBorder     = IM_COL32(160, 120,  40,  46);
    const ImU32 cBorderHi   = IM_COL32(200, 168,  75,  82);
    const ImU32 cBorderGlow = IM_COL32(200, 168,  75, 140);
    const ImU32 cGold       = IM_COL32(200, 168,  75, 255);
    const ImU32 cGoldBright = IM_COL32(226, 194, 106, 255);
    const ImU32 cGoldFill   = IM_COL32(200, 168,  75,  20);
    const ImU32 cText       = IM_COL32(232, 223, 200, 255);
    const ImU32 cTextMid    = IM_COL32(154, 138, 104, 255);
    const ImU32 cTextDim    = IM_COL32(120, 108,  80, 255);
    const ImU32 cDanger     = IM_COL32(192,  80,  74, 255);
    const ImU32 cGreen      = IM_COL32( 90, 170, 120, 255);
    const ImU32 cGreenDim   = IM_COL32( 90, 170, 120,  80);

    const float px = pos.x, py = pos.y, pw = width, ph = barH;
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph), IM_COL32(8, 9, 12, 140), 8.f);
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph), cGlass, 8.f);
    dl->AddRect(ImVec2(px, py), ImVec2(px + pw, py + ph), cBorderHi, 8.f);
    {
        const float sx = px + 12.f, sw = pw - 24.f, mx = sx + sw * 0.5f;
        dl->AddRectFilledMultiColor(ImVec2(sx, py), ImVec2(mx, py + 1.f),
            IM_COL32(200,168,75, 0), IM_COL32(200,168,75,100), IM_COL32(200,168,75,100), IM_COL32(200,168,75, 0));
        dl->AddRectFilledMultiColor(ImVec2(mx, py), ImVec2(sx + sw, py + 1.f),
            IM_COL32(200,168,75,100), IM_COL32(200,168,75, 0), IM_COL32(200,168,75, 0), IM_COL32(200,168,75,100));
    }

    const float x0 = px + PAD;
    bool& playing = s_state.detailPlaying;

    // ── One row: status chip and clock left, transport centred, toggles after it, speed right ──
    const float cy = py + (barH - BTN_H) * 0.5f;
    {
        char curBuf[16], totBuf[16];
        FormatClock((int)std::floor(t), curBuf, sizeof(curBuf));
        FormatClock((int)std::floor(toT), totBuf, sizeof(totBuf));
        const float cx0 = x0 + 66.f + 10.f;
        const float ty = cy + (BTN_H - fs) * 0.5f;
        dl->AddText(font, fs, ImVec2(cx0, ty), cText, curBuf);
        const float curW = font->CalcTextSizeA(fs, FLT_MAX, 0.f, curBuf).x;
        const float sepY = ty + (fs - fsSm) * 0.5f;
        dl->AddText(font, fsSm, ImVec2(cx0 + curW + 2.f, sepY), cBorderHi, " / ");
        const float sepW = font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, " / ").x;
        dl->AddText(font, fsSm, ImVec2(cx0 + curW + 2.f + sepW, sepY), cTextMid, totBuf);
    }
    auto FillBtn = [&](float bx, float by, float bw, float bh, bool hov, ImU32 bg0, ImU32 bdr0) {
        const ImU32 bg = hov ? cGoldFill : bg0;
        const ImU32 bdr = hov ? cBorderHi : bdr0;
        if ((bg >> 24) > 0) dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), bg, 5.f);
        if ((bdr >> 24) > 0) dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), bdr, 5.f);
    };
    auto Divider = [&](float dx) {
        dl->AddLine(ImVec2(dx, cy + 3.f), ImVec2(dx, cy + BTN_H - 3.f), cBorder);
    };

    {
        const float chipW = 66.f;
        dl->AddRectFilled(ImVec2(x0, cy), ImVec2(x0 + chipW, cy + BTN_H), IM_COL32(0,0,0,76), 5.f);
        dl->AddRect(ImVec2(x0, cy), ImVec2(x0 + chipW, cy + BTN_H), cBorder, 5.f);
        const bool blinkOn = ((int)(ImGui::GetTime() / 0.65) % 2 == 0);
        dl->AddCircleFilled(ImVec2(x0 + 8.f, cy + BTN_H * 0.5f), 2.5f,
                            playing ? (blinkOn ? cGreen : cGreenDim) : cDanger);
        dl->PushClipRect(ImVec2(x0, cy), ImVec2(x0 + chipW, cy + BTN_H), true);
        dl->AddText(font, fsSm, ImVec2(x0 + 16.f, cy + (BTN_H - fsSm) * 0.5f), cTextDim,
                    playing ? "PLAYING" : "STOPPED");
        dl->PopClipRect();
    }

    const float bk30W = 40.f, stepW = 34.f, stopW = 24.f, toggleW = 58.f;
    const float transportW = bk30W + BGAP + stepW + BGAP + stepW + GDIV + PLAY_SZ + BGAP + stopW
                           + GDIV + stepW + BGAP + stepW + BGAP + bk30W;
    float cx = std::max(x0 + 180.f, px + (pw - transportW) * 0.5f);

    auto DrawStepBtn = [&](const char* id, float bw, const char* label, float stepSec, bool forward) {
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::InvisibleButton(id, ImVec2(bw, BTN_H));
        const bool hov = ImGui::IsItemHovered();
        FillBtn(cx, cy, bw, BTN_H, hov, 0, 0);
        const ImU32 fg = hov ? cGoldBright : cTextMid;
        const float thick = 1.3f, rad = 4.f;
        if (!forward)
        {
            const float icx = cx + bw - 10.f, icy = cy + BTN_H * 0.5f;
            dl->PathArcTo(ImVec2(icx, icy), rad, 270.f * d2r, (270.f + 300.f) * d2r, 24);
            dl->PathStroke(fg, false, thick);
            const float tipX = icx + rad - 0.5f, tipY = icy - 1.f;
            dl->AddLine(ImVec2(tipX, tipY), ImVec2(tipX - 2.5f, tipY - 2.f), fg, thick);
            dl->AddLine(ImVec2(tipX, tipY), ImVec2(tipX + 0.8f, tipY - 2.5f), fg, thick);
            dl->AddText(font, fsSm, ImVec2(cx + 4.f, cy + (BTN_H - fsSm) * 0.5f), fg, label);
        }
        else
        {
            const float icx = cx + 10.f, icy = cy + BTN_H * 0.5f;
            dl->PathArcTo(ImVec2(icx, icy), rad, 30.f * d2r, (30.f + 300.f) * d2r, 24);
            dl->PathStroke(fg, false, thick);
            const float tipX = icx - rad + 0.5f, tipY = icy - 1.f;
            dl->AddLine(ImVec2(tipX, tipY), ImVec2(tipX + 2.5f, tipY - 2.f), fg, thick);
            dl->AddLine(ImVec2(tipX, tipY), ImVec2(tipX - 0.8f, tipY - 2.5f), fg, thick);
            dl->AddText(font, fsSm, ImVec2(cx + 16.f, cy + (BTN_H - fsSm) * 0.5f), fg, label);
        }
        if (ImGui::IsItemClicked()) t = std::clamp(t + stepSec, fromT, toT);
        if (hov) ImGui::SetTooltip(forward ? "Forward %s" : "Back %s", label);
        cx += bw + BGAP;
    };

    DrawStepBtn("##map_bk30", bk30W, "30s", -30.f, false);
    DrawStepBtn("##map_bk5",  stepW, "5s",  -5.f,  false);
    DrawStepBtn("##map_bk1",  stepW, "1s",  -1.f,  false);
    cx += GDIV;
    Divider(cx - GDIV * 0.5f);

    {
        const float plaY = cy - (PLAY_SZ - BTN_H) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(cx, plaY));
        ImGui::InvisibleButton("##map_play", ImVec2(PLAY_SZ, PLAY_SZ));
        const bool hov = ImGui::IsItemHovered();
        dl->AddRectFilled(ImVec2(cx, plaY), ImVec2(cx + PLAY_SZ, plaY + PLAY_SZ),
                          hov ? IM_COL32(200,168,75, 33) : IM_COL32(200,168,75, 18), 6.f);
        dl->AddRect(ImVec2(cx, plaY), ImVec2(cx + PLAY_SZ, plaY + PLAY_SZ), hov ? cBorderGlow : cBorderHi, 6.f);
        const ImU32 fg = hov ? cGoldBright : cGold;
        const float pcx = cx + PLAY_SZ * 0.5f, pcy = plaY + PLAY_SZ * 0.5f;
        if (!playing)
            dl->AddTriangleFilled(ImVec2(pcx - 4.f, pcy - 5.f), ImVec2(pcx + 5.f, pcy), ImVec2(pcx - 4.f, pcy + 5.f), fg);
        else
        {
            dl->AddRectFilled(ImVec2(pcx - 4.5f, pcy - 5.f), ImVec2(pcx - 1.5f, pcy + 5.f), fg);
            dl->AddRectFilled(ImVec2(pcx + 1.5f, pcy - 5.f), ImVec2(pcx + 4.5f, pcy + 5.f), fg);
        }
        if (ImGui::IsItemClicked())
        {
            if (!playing && t >= toT - 0.01f) t = fromT;
            playing = !playing;
        }
        if (hov) ImGui::SetTooltip(playing ? "Pause" : "Play");
        cx += PLAY_SZ + BGAP;
    }
    {
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::InvisibleButton("##map_stop", ImVec2(stopW, BTN_H));
        const bool hov = ImGui::IsItemHovered();
        FillBtn(cx, cy, stopW, BTN_H, hov, 0, hov ? IM_COL32(192,80,74,100) : IM_COL32(0,0,0,0));
        const float sq = 8.f;
        const float scx = cx + (stopW - sq) * 0.5f, scy = cy + (BTN_H - sq) * 0.5f;
        dl->AddRectFilled(ImVec2(scx, scy), ImVec2(scx + sq, scy + sq), hov ? cDanger : cTextMid);
        if (ImGui::IsItemClicked()) { t = fromT; playing = false; }
        if (hov) ImGui::SetTooltip("Stop (back to the start of the interval)");
        cx += stopW + BGAP;
    }
    cx += GDIV;
    Divider(cx - GDIV * 0.5f);

    DrawStepBtn("##map_fw1",  stepW, "1s",  +1.f,  true);
    DrawStepBtn("##map_fw5",  stepW, "5s",  +5.f,  true);
    DrawStepBtn("##map_fw30", bk30W, "30s", +30.f, true);

    // Trails and Names: toggles in the Loop button's style.
    auto DrawToggle = [&](const char* id, const char* label, bool& on, const char* tip) {
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::InvisibleButton(id, ImVec2(toggleW, BTN_H));
        const bool hov = ImGui::IsItemHovered();
        FillBtn(cx, cy, toggleW, BTN_H, hov, on ? cGoldFill : 0, on ? cBorderHi : 0);
        const ImU32 fg = (hov || on) ? cGoldBright : cTextMid;
        dl->AddCircleFilled(ImVec2(cx + 9.f, cy + BTN_H * 0.5f), 3.f, on ? cGreen : cTextDim);
        dl->AddText(font, fsSm, ImVec2(cx + 17.f, cy + (BTN_H - fsSm) * 0.5f), fg, label);
        if (ImGui::IsItemClicked()) on = !on;
        if (hov) ImGui::SetTooltip("%s: %s", tip, on ? "ON" : "OFF");
        cx += toggleW + BGAP;
    };
    cx += GDIV;
    Divider(cx - GDIV * 0.5f);
    DrawToggle("##map_trails", "Trails", s_state.detailShowTrails, "Where each player went since the start of the interval");
    DrawToggle("##map_names",  "Names",  s_state.detailShowNames,  "Player names on the map");

    // ── Speed (right-aligned) ──
    {
        const float speedW = 54.f;
        const float speedX = px + pw - PAD - speedW;
        dl->AddText(font, fsSm, ImVec2(speedX - 8.f - font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, "SPEED").x,
                                       cy + (BTN_H - fsSm) * 0.5f + 1.f), cTextDim, "SPEED");
        ImGui::SetCursorScreenPos(ImVec2(speedX, cy));
        ImGui::InvisibleButton("##map_speed", ImVec2(speedW, BTN_H));
        const bool hov = ImGui::IsItemHovered();
        FillBtn(speedX, cy, speedW, BTN_H, hov, 0, cBorderHi);
        const ImU32 sfg = hov ? cGoldBright : cTextMid;
        char sLbl[8];
        snprintf(sLbl, sizeof(sLbl), "%dx", (int)s_state.detailPlaySpeed);
        const ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, sLbl);
        const float tx = speedX + (speedW - tsz.x - 8.f) * 0.5f, ty = cy + (BTN_H - fs) * 0.5f;
        dl->AddText(font, fs, ImVec2(tx, ty), sfg, sLbl);
        const float arX = tx + tsz.x + 3.f, arY = ty + fs * 0.35f;
        dl->AddTriangleFilled(ImVec2(arX, arY), ImVec2(arX + 4.f, arY), ImVec2(arX + 2.f, arY + 3.f), sfg);
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("##map_speed_menu");

        const int speedCount = (int)std::size(kMapPlaySpeeds);
        const float popupH = speedCount * ImGui::GetFrameHeightWithSpacing() + 8.f;
        ImGui::SetNextWindowPos(ImVec2(speedX, cy - popupH));
        ImGui::SetNextWindowSizeConstraints(ImVec2(speedW, 0), ImVec2(speedW * 2.f, FLT_MAX));
        ImGui::PushStyleColor(ImGuiCol_PopupBg,       ImVec4(0.07f, 0.06f, 0.04f, 0.97f));
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.78f, 0.66f, 0.29f, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.78f, 0.66f, 0.29f, 0.20f));
        if (ImGui::BeginPopup("##map_speed_menu"))
        {
            // Fastest first, as in the replay: the menu opens upwards.
            for (int i = speedCount - 1; i >= 0; --i)
            {
                char l[8];
                snprintf(l, sizeof(l), "%dx", kMapPlaySpeeds[i]);
                if (ImGui::Selectable(l, (int)s_state.detailPlaySpeed == kMapPlaySpeeds[i]))
                    s_state.detailPlaySpeed = (float)kMapPlaySpeeds[i];
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(3);
        if (hov) ImGui::SetTooltip("Playback speed");
    }
}

// The team columns beside the map: the guild line, then a row per player this far apart.
static constexpr float kMapTeamRowGap = 3.0f;
static float MapTeamHeaderH() { return ImGui::GetTextLineHeight() + 10.0f; }

// The widest player name of the match, for the name column beside the map.
static float MapTeamNameWidth(const MatchMeta& m)
{
    float w = 0.0f;
    for (const auto& [id, party] : m.parties)
        for (const auto& p : party.players)
            w = std::max(w, ImGui::CalcTextSize(
                (p.encoded_name.empty() ? std::string("(unnamed)") : SanitizePlayerName(p.encoded_name)).c_str()).x);
    return w;
}

// One team beside the map: its guild, then one row per player - professions, name, then the
// bar as the Overview shows it. A player dead at the playhead is dimmed.
static void DrawMapTeamColumn(const MatchMeta& m, const char* partyId, ImVec2 pos, ImVec2 size,
                              float skillSz, float nameW, float t)
{
    auto pit = m.parties.find(partyId);
    if (pit == m.parties.end()) return;
    std::vector<const PlayerMeta*> sorted;
    for (const auto& p : pit->second.players) sorted.push_back(&p);
    std::sort(sorted.begin(), sorted.end(),
        [](const PlayerMeta* a, const PlayerMeta* b) { return a->player_number < b->player_number; });
    const auto& matchView = GetMatchSkillView(m);
    const bool red = std::string(partyId) == "2";

    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    if (ImGui::BeginChild(red ? "##map_team_red" : "##map_team_blue", size, false, ImGuiWindowFlags_NoScrollbar))
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float lineH = ImGui::GetTextLineHeight();

        // Header: the side's ribbon, the cup for the winner, the guild.
        {
            std::string ft1, ft2;
            ParseFolderTags(m.folder_name, ft1, ft2);
            const GuildLabel guild = GetPartyGuild(m, partyId, red ? ft2 : ft1);
            const bool won = m.winner_party_id == (red ? 2 : 1);
            TeamRibbonItem(lineH + 2.0f, red ? 1 : 0, true);
            ImGui::SameLine(0, 8);
            if (won)
                if (ImTextureID cup = GetCupIcon())
                {
                    ImGui::Image(cup, ImVec2((lineH + 2.0f) * (64.0f / 109.0f), lineH + 2.0f));
                    ImGui::SameLine(0, 6);
                }
            ImFont* bold = GuiGlobalConstants::boldFont;
            if (bold) ImGui::PushFont(bold);
            ImGui::TextColored(won ? kColorAccent : kColorText, "%s", guild.display.c_str());
            if (bold) ImGui::PopFont();
        }
        // Rows at fixed offsets, the ones DrawMapTab sizes the icons by: the item spacing of the
        // normal flow would add up to half a row over eight players.
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + MapTeamHeaderH()));

        for (const auto* pp : sorted)
        {
            const auto& p = *pp;
            const std::string name = p.encoded_name.empty() ? "(unnamed)" : SanitizePlayerName(p.encoded_name);

            // Dead at the playhead, by the analysis' position track.
            bool dead = false;
            for (const auto& ap : s_analysis.players)
                if (ap.name == name)
                {
                    const int idx = (int)std::floor(t) + s_analysis.gateSeconds;
                    if (idx >= 0 && (size_t)idx * 4 + 3 < ap.pos.size()) dead = ap.pos[(size_t)idx * 4 + 3] == 1;
                    break;
                }

            // One row, everything centred on the bar's height.
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const float rowH = std::max(skillSz, lineH);
            const float profSz = std::min(lineH, rowH);
            const float profY = rowMin.y + (rowH - profSz) * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, profY));
            if (ImTextureID pri = GetProfessionIcon(p.primary)) ImGui::Image(pri, ImVec2(profSz, profSz));
            else ImGui::Dummy(ImVec2(profSz, profSz));
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x + profSz + 1.0f, profY));
            if (ImTextureID sec = GetProfessionIcon(p.secondary)) ImGui::Image(sec, ImVec2(profSz, profSz));
            else ImGui::Dummy(ImVec2(profSz, profSz));
            const float nameX = rowMin.x + 2.0f * profSz + 7.0f;
            ImGui::SetCursorScreenPos(ImVec2(nameX, rowMin.y + (rowH - lineH) * 0.5f));
            ImGui::TextColored(dead ? kColorTextDim : kColorText, "%s", name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s / %s%s", GetProfessionName(p.primary), GetProfessionName(p.secondary),
                                  dead ? "\nDead at this moment" : "");
            ImGui::SetCursorScreenPos(ImVec2(nameX + nameW + 10.0f, rowMin.y + (rowH - skillSz) * 0.5f));
            DrawPlayerSkillRow(p, matchView, skillSz);
            if (dead)
                dl->AddRectFilled(rowMin, ImVec2(rowMin.x + size.x, rowMin.y + rowH), IM_COL32(0, 0, 0, 120));
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMin.y + rowH + kMapTeamRowGap));
        }
        ImGui::Dummy(ImVec2(0, 0));   // an item after the last SetCursorScreenPos, as ImGui asks
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ─── Stats tab ───────────────────────────────────────────────────────────────
// Two views of the same numbers over the timeline's interval, swapped with the VIEW pills:
// Table, every player against the columns picked, and Bars, the replay's Damage & Healing panel
// (BattleLedgerView.cpp) drawn from the ledger match_analysis.json carries. Clicking a table cell
// opens Bars on that player and the cell's tab.

std::filesystem::path FindTexturesDDSDir();   // ReplayWindow.cpp

namespace
{
    enum StatCol { SC_Dealt, SC_Taken, SC_Healed, SC_Received, SC_Prevented, SC_Kills, SC_Deaths, SC_Ints,
                   SC_IntsRecv, SC_Skills, SC_Cancelled, SC_Dead, SC_Distance,
                   SC_DmgAttack, SC_DmgSkill, SC_DmgHex, SC_DmgCond, SC_DmgMinion,
                   SC_DmgBleed, SC_DmgBurn, SC_DmgPoison, SC_DmgDisease, SC_Sacrifice, SC_SelfCond, SC_Count };
    // name: the column's id in MatchStats::Layout() ("base:<name>"); help: its plain meaning;
    // lowerBetter: a smaller number is the better play (the header marks it).
    struct StatColInfo { const char* name; const char* label; const char* head; int ledgerTab; const char* help; bool lowerBetter; };
    const StatColInfo kStatCols[SC_Count] = {
        { "dealt",     "Damage dealt",        "Damage",      BattleLedger::kDealt,     "All the damage this player caused: attacks, skills, hexes, conditions and minions. Degeneration is included: the Health lost to it is split among the conditions and hexes active at the time, each part credited to whoever applied it. Own sacrifices are left out.", false },
        { "taken",     "Damage taken",        "Taken",       BattleLedger::kTaken,     "Damage taken from foes and the map, degeneration included. The player's own sacrifices are apart (Health sacrificed).", true },
        { "healed",    "Healing done",        "Healing",     BattleLedger::kHealed,    "Healing given, the player's own included.", false },
        { "received",  "Healing received",    "Healed",      BattleLedger::kReceived,  "Healing received, from anyone and the map.", false },
        { "prevented", "Damage prevented",    "Prevented",   BattleLedger::kPrevented, "Damage the player's protections spared, some of it estimated.", false },
        { "kills",     "Kills",               "Kills",       BattleLedger::kDealt,     "Foes whose last hit before dying (within 5 s) was this player's.", false },
        { "deaths",    "Deaths",              "Deaths",      BattleLedger::kTaken,     "Times the player died.", true },
        { "ints",      "Interrupts landed",   "Interrupts",  BattleLedger::kInts,      "Foe casts this player interrupted.", false },
        { "intsrecv",  "Interrupted",         "Interrupted", BattleLedger::kIntsRecv,  "Casts of this player that a foe interrupted.", true },
        { "skills",    "Skills used",         "Skills",      BattleLedger::kDealt,     "Skills used, interrupted ones included, cancelled ones left out.", false },
        { "cancelled", "Skills cancelled",    "Cancelled",   BattleLedger::kIntsRecv,  "Casts the player stopped themselves.", true },
        { "dead",      "Time dead",           "Dead",        BattleLedger::kTaken,     "Seconds spent dead. Per minute: the share of the interval.", true },
        { "distance",  "Distance",            "Distance",    BattleLedger::kDealt,     "Ground covered while alive, in game units.", false },
        { "dmg_attack", "Attack damage",      "Attacks",     BattleLedger::kDealt,     "Damage dealt by weapon hits and attack skills.", false },
        { "dmg_skill",  "Skill damage",       "Skills dmg",  BattleLedger::kDealt,     "Damage dealt by spells and other non-attack skills (hexes apart), and damage whose skill is unknown.", false },
        { "dmg_hex",    "Hex damage",         "Hexes",       BattleLedger::kDealt,     "Damage dealt by hexes: their hits (Empathy, Spiteful Spirit...) and their degeneration.", false },
        { "dmg_cond",   "Condition damage",   "Conditions",  BattleLedger::kDealt,     "Degeneration from the conditions this player applied: Bleeding, Burning, Poison, Disease.", false },
        { "dmg_minion", "Minion, spirit & pet damage", "Summons", BattleLedger::kDealt, "Damage dealt by this player's minions, spirits (Bloodsong...) and pet.", false },
        { "dmg_bleed",  "Bleeding damage",    "Bleeding",    BattleLedger::kDealt,     "Degeneration from the Bleeding this player applied.", false },
        { "dmg_burn",   "Burning damage",     "Burning",     BattleLedger::kDealt,     "Degeneration from the Burning this player applied.", false },
        { "dmg_poison", "Poison damage",      "Poison",      BattleLedger::kDealt,     "Degeneration from the Poison this player applied.", false },
        { "dmg_disease","Disease damage",     "Disease",     BattleLedger::kDealt,     "Degeneration from the Disease this player applied.", false },
        { "sacrifice",  "Health sacrificed",  "Sacrificed",  BattleLedger::kTaken,     "Health this player paid for their own skills (sacrifices). Not part of Damage taken.", false },
        { "selfcond",   "Own conditions caught", "Own condi",  BattleLedger::kTaken,     "Health this player lost to a condition from their own skill that came back to them: Mystic Corruption puts Disease on adjacent foes, Disease spreads, and the Dervish catches it. Their own doing, so it is not part of Damage taken.", true },
    };

    // A column is one of the fixed ones above (0 .. SC_Count - 1) or a stat of the MatchStats
    // catalogue (kExtraBase + its index), whose records match_analysis.json carries.
    constexpr int kExtraBase = 100;
    const MatchStats::StatDef* ExtraDef(int c)
    {
        const auto& cat = MatchStats::Catalog();
        return c >= kExtraBase && c - kExtraBase < (int)cat.size() ? &cat[c - kExtraBase] : nullptr;
    }
    const char* ColLabel(int c) { const auto* d = ExtraDef(c); return d ? d->label : kStatCols[c].label; }
    const char* ColHead(int c)  { const auto* d = ExtraDef(c); return d ? d->head : kStatCols[c].head; }
    bool ColLowerBetter(int c)  { const auto* d = ExtraDef(c); return d ? d->lowerBetter : kStatCols[c].lowerBetter; }

    // A MatchStats::Layout() key's column: "base:<name>" a fixed one, else the catalogue stat; -1 unknown.
    int LayoutCol(const char* key)
    {
        if (std::strncmp(key, "base:", 5) == 0)
        {
            for (int c = 0; c < SC_Count; ++c) if (std::strcmp(key + 5, kStatCols[c].name) == 0) return c;
            return -1;
        }
        const auto& cat = MatchStats::Catalog();
        for (size_t i = 0; i < cat.size(); ++i) if (std::strcmp(cat[i].key, key) == 0) return kExtraBase + (int)i;
        return -1;
    }

    // Typical skills behind a column, shown as icons in its tooltip so the meaning is plain to see.
    // Ids checked against skilldesc-en.json; the conditions are the client's condition "skills"
    // (478 Bleeding ... 486 Weakness), which have icons but no skill data.
    struct ExampleSkill { int id; const char* name; };
    using Examples = std::vector<ExampleSkill>;
    const Examples& ColExamples(int c)
    {
        static const Examples kHeals   = { { 2061, "Patient Spirit" }, { 1257, "Spirit Light Weapon" }, { 292, "Infuse Health" }, { 1118, "Healing Burst" } };
        static const Examples kProts   = { { 245, "Protective Spirit" }, { 258, "Guardian" }, { 307, "Reversal of Fortune" }, { 299, "Shielding Hands" }, { 1114, "Spirit Bond" } };
        static const Examples kRupts   = { { 426, "Savage Shot" }, { 399, "Distracting Shot" }, { 25, "Power Drain" }, { 61, "Leech Signet" } };
        static const Examples kKds     = { { 2011, "Grapple" }, { 226, "Mind Shock" }, { 332, "Bull's Strike" }, { 869, "\"Coward!\"" }, { 162, "Gale" } };
        static const Examples kStab    = { { 2063, "Aura of Stability" }, { 938, "Ward of Stability" }, { 1572, "\"Brace Yourself!\"" }, { 371, "Balanced Stance" }, { 1514, "Fleeting Stability" } };
        static const Examples kDrains  = { { 42, "Energy Burn" }, { 49, "Mind Wrack" }, { 39, "Energy Surge" }, { 366, "\"Fear Me!\"" } };
        static const Examples kCondRm  = { { 275, "Mend Condition" }, { 276, "Restore Condition" }, { 311, "Draw Conditions" }, { 1401, "Mending Touch" } };
        static const Examples kHexRm   = { { 2003, "Cure Hex" }, { 295, "Purge Signet" }, { 1692, "Divert Hexes" }, { 302, "Smite Hex" } };
        static const Examples kHexes   = { { 26, "Empathy" }, { 30, "Diversion" }, { 52, "Panic" }, { 31, "Conjure Phantasm" } };
        static const Examples kConds   = { { 481, "Crippled" }, { 479, "Blind" }, { 486, "Weakness" }, { 485, "Dazed" }, { 478, "Bleeding" } };
        static const Examples kRes     = { { 1481, "Death Pact Signet" }, { 2, "Resurrection Signet" }, { 791, "Flesh of My Flesh" } };
        static const std::unordered_map<std::string, Examples> byKey = {
            { "base:dealt",      { { 338, "Eviscerate" }, { 1227, "Essence Strike" }, { 26, "Empathy" }, { 478, "Bleeding" }, { 1253, "Bloodsong" } } },
            { "base:dmg_attack", { { 338, "Eviscerate" }, { 336, "Executioner's Strike" }, { 352, "Crushing Blow" }, { 907, "Dragon Slash" } } },
            { "base:dmg_skill",  { { 1227, "Essence Strike" }, { 133, "Dark Pact" }, { 1662, "Mind Blast" }, { 205, "Lightning Surge" } } },
            { "base:dmg_hex",    { { 26, "Empathy" }, { 121, "Spiteful Spirit" }, { 31, "Conjure Phantasm" }, { 109, "Life Siphon" } } },
            { "base:dmg_cond",   { { 478, "Bleeding" }, { 480, "Burning" }, { 484, "Poison" }, { 483, "Disease" } } },
            { "base:dmg_minion", { { 84, "Animate Bone Fiend" }, { 832, "Animate Flesh Golem" }, { 1253, "Bloodsong" }, { 411, "Charm Animal" } } },
            { "base:dmg_bleed",  { { 478, "Bleeding" } } },
            { "base:dmg_burn",   { { 480, "Burning" } } },
            { "base:dmg_poison", { { 484, "Poison" } } },
            { "base:dmg_disease",{ { 483, "Disease" } } },
            { "cond_inflicted_crippled",   { { 481, "Crippled" } } },
            { "cond_inflicted_dazed",      { { 485, "Dazed" } } },
            { "cond_inflicted_weakness",   { { 486, "Weakness" } } },
            { "cond_inflicted_cracked",    { { 2077, "Cracked Armor" } } },
            { "cond_inflicted_blind",      { { 479, "Blind" } } },
            { "cond_inflicted_deep_wound", { { 482, "Deep Wound" } } },
            { "base:sacrifice",  { { 133, "Dark Pact" }, { 791, "Flesh of My Flesh" }, { 119, "Blood is Power" }, { 146, "Offering of Blood" } } },
            { "base:selfcond",   { { 1755, "Mystic Corruption" }, { 483, "Disease" } } },
            { "base:healed", kHeals }, { "heal_effective", kHeals }, { "heal_overheal", kHeals },
            { "base:prevented", kProts }, { "spike_reaction", kProts },
            { "base:ints", kRupts }, { "base:intsrecv", kRupts }, { "int_accuracy", kRupts }, { "int_reaction", kRupts }, { "int_fakes", kRupts },
            { "kd_dealt", kKds }, { "kd_success", kKds }, { "kd_taken", kKds },
            { "kd_blocked", kStab }, { "kd_prevent_reaction", kStab },
            { "en_denied", kDrains }, { "en_lost_foes", kDrains },
            { "ench_removed", { { 141, "Rend Enchantments" }, { 69, "Shatter Enchantment" }, { 143, "Strip Enchantment" } } },
            { "cond_inflicted_total", kConds }, { "cond_time", kConds },
            { "hex_time", kHexes },
            { "cond_removed", kCondRm }, { "cond_removal_reaction", kCondRm },
            { "hex_removed", kHexRm }, { "hex_removal_reaction", kHexRm },
            { "act_res_done", kRes }, { "act_res_reaction", kRes },
            { "en_given", { { 1782, "\"The Power Is Yours!\"" } } },
            { "kd_coward_dodge", { { 869, "\"Coward!\"" } } },
        };
        static const Examples none;
        const auto* d = ExtraDef(c);
        auto it = byKey.find(d ? std::string(d->key) : std::string("base:") + kStatCols[c].name);
        return it == byKey.end() ? none : it->second;
    }

    // A column's tooltip: what it means in plain words, "Lower is better" when it is, typical
    // skills as icons, the method dimmed below (catalogue stats), then the caller's own hint.
    void ColTooltip(int c, const char* hint = nullptr, bool withName = false)
    {
        const auto* d = ExtraDef(c);
        const ImVec4 dim(0.55f, 0.57f, 0.60f, 1.0f);
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        if (withName) ImGui::TextColored(ImVec4(0.88f, 0.75f, 0.31f, 1.0f), "%s", ColLabel(c));
        ImGui::TextUnformatted(d ? (d->meaning[0] ? d->meaning : d->help) : kStatCols[c].help);
        if (ColLowerBetter(c)) ImGui::TextColored(ImVec4(0.90f, 0.62f, 0.45f, 1.0f), "Lower is better.");
        if (const Examples& ex = ColExamples(c); !ex.empty())
        {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::TextColored(dim, "Typical skills:");
            const float sz = ImGui::GetFontSize() * 2.2f;
            for (size_t i = 0; i < ex.size(); ++i)
            {
                if (i) ImGui::SameLine(0.0f, 4.0f);
                if (ImTextureID t = GetSkillIcon(ex[i].id)) ImGui::Image(t, ImVec2(sz, sz));
                else ImGui::Dummy(ImVec2(sz, sz));
            }
        }
        if (d && d->meaning[0] && d->meaning != d->help)
        {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::TextColored(dim, "How it is measured: %s", d->help);
        }
        if (d && d->estimated) ImGui::TextColored(dim, "~ = estimated from a model.");
        if (hint) ImGui::TextColored(ImVec4(0.67f, 0.69f, 0.72f, 1.0f), "%s", hint);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    struct StatsTabState
    {
        bool bars = false;
        bool perMinute = false;
        bool on[SC_Count] = { true, true, true, true, true, true, true, true, false, true, false, false, false };
        std::vector<int> extra;           // the catalogue's columns picked, in the order picked (kExtraBase + index)
        char search[64] = {};             // the More stats picker's keywords
        int  sort = SC_Dealt;
        BattleLedger::ViewState view;
        std::string folder;               // the match the view state belongs to
    };
    StatsTabState s_stats;

    // The Library's icons and digits for the shared view (the main window's device).
    class LibraryLedgerHost : public BattleLedger::Host
    {
    public:
        explicit LibraryLedgerHost(const SkillDatabaseView& view) : m_view(view) {}
        ImTextureID SkillIcon(int skillId) override { return GetSkillIcon(skillId, &m_view); }
        ImTextureID ProfIcon(int prof) override { return GetProfessionIcon(prof); }
        ImTextureID UiIcon(const char* file) override
        {
            EnsureTextureBasePath();
            return GetTextureCache().GetTexture(ui::TextureBasePath() + "\\Textures\\Others_UI\\" + file);
        }
        std::string SkillName(int skillId) override
        {
            const SkillInfo* si = m_view.Get(skillId);
            return si ? si->name : "Skill " + std::to_string(skillId);
        }
        const BitmapFont* Digits(int m) override
        {
            static BitmapFont dmg, heal, energy;
            if (!dmg.loaded && GetTextureCache().Device())
            {
                const auto dir = FindTexturesDDSDir();
                if (!dir.empty())
                {
                    dmg.Load(GetTextureCache().Device(), (dir / L"GW.EXE_0x753F0FB5.dds").c_str());
                    heal.Load(GetTextureCache().Device(), (dir / L"GW.EXE_0xA0629E7F.dds").c_str());
                    energy.Load(GetTextureCache().Device(), (dir / L"texture_265569.dds").c_str());
                }
                dmg.loaded = heal.loaded = energy.loaded = true;
            }
            using namespace BattleLedger;
            const BitmapFont* f = m == kDealt || m == kTaken ? &dmg : m == kInts || m == kIntsRecv ? &energy : &heal;
            return f->srv.Get() ? f : nullptr;
        }
    private:
        const SkillDatabaseView& m_view;
    };

    // Every column's number per player over [r0, r1] on the match clock.
    std::unordered_map<int, std::array<double, SC_Count>> StatValues(const MatchAnalysisJob& job, float r0, float r1,
                                                                    const SkillDatabaseView& skills)
    {
        const BattleLedger::Data& d = job.ledger;
        std::unordered_map<int, std::array<double, SC_Count>> out;
        for (int pid : d.players) out[pid].fill(0.0);
        const auto tot = BattleLedger::SumTotals(d, r0, r1);
        for (auto& [pid, v] : out)
        {
            auto it = tot.find(pid);
            if (it == tot.end()) continue;
            const auto& t = it->second.v;
            v[SC_Dealt]     = (double)t[BattleLedger::kDealt];
            v[SC_Taken]     = (double)t[BattleLedger::kTaken];
            v[SC_Healed]    = (double)t[BattleLedger::kHealed];
            v[SC_Received]  = (double)t[BattleLedger::kReceived];
            v[SC_Prevented] = (double)t[BattleLedger::kPrevented];
            v[SC_Ints]      = (double)t[BattleLedger::kInts];
            v[SC_IntsRecv]  = (double)t[BattleLedger::kIntsRecv];
        }
        // Damage dealt by kind, the same lines Damage dealt sums, so the five always add up to it.
        for (const BattleLedger::Line& ln : d.lines)
        {
            if (ln.t < r0) continue;
            if (ln.t > r1) break;
            if (BattleLedger::LineMetricGiver(ln.kind) != BattleLedger::kDealt) continue;
            auto it = out.find(ln.src);
            if (it == out.end()) continue;
            int col = SC_DmgSkill;
            if (ln.rowKey == BattleLedger::kRowMinions || ln.rowKey == BattleLedger::kRowPets) col = SC_DmgMinion;
            else if (BattleLedger::IsDegenRow(ln.rowKey) || ln.rowKey == BattleLedger::kRowDegenUnknown)
            {
                col = SC_DmgCond;
                // Each damaging condition also on its own column (the client's condition ids).
                switch (BattleLedger::IsDegenRow(ln.rowKey) ? BattleLedger::DegenCondition(ln.rowKey) : 0)
                {
                case 478: it->second[SC_DmgBleed]   += ln.amount; break;
                case 480: it->second[SC_DmgBurn]    += ln.amount; break;
                case 484: it->second[SC_DmgPoison]  += ln.amount; break;
                case 483: it->second[SC_DmgDisease] += ln.amount; break;
                default: break;
                }
            }
            else if (ln.weaponHit) col = SC_DmgAttack;
            else if (const SkillInfo* si = ln.skillId ? skills.Get(ln.skillId) : nullptr)
                col = SkillDatabase::IsWeaponAttack(si->type) ? SC_DmgAttack
                    : si->type == 24                    ? SC_DmgHex
                    : si->type == 18 || si->type == 19  ? SC_DmgMinion     // a spirit's attack (Bloodsong)
                                                        : SC_DmgSkill;
            it->second[col] += ln.amount;
        }
        // Sacrifices out of Damage taken, on their own column.
        for (const BattleLedger::Line& ln : d.lines)
        {
            if (ln.t < r0) continue;
            if (ln.t > r1) break;
            if (ln.kind != BattleLedger::Kind::SelfCost) continue;
            auto it = out.find(ln.dst);
            if (it == out.end()) continue;
            it->second[BattleLedger::IsDegenRow(ln.rowKey) ? SC_SelfCond : SC_Sacrifice] += ln.amount;
            it->second[SC_Taken] -= ln.amount;
        }
        // Deaths, and the kill: the last foe player's hit within 5 s before it.
        auto teamOf = [&](int id) { auto it = d.agents.find(id); return it == d.agents.end() ? 0 : it->second.team; };
        for (const auto& e : job.events)
        {
            if (e.type != AnalysisEvent::Death || e.t < r0 || e.t > r1 || !out.count(e.agent)) continue;
            out[e.agent][SC_Deaths] += 1.0;
            int killer = 0;
            for (const auto& ln : d.lines)
            {
                if (ln.t > e.t) break;
                if (ln.t < e.t - 5.0f || ln.dst != e.agent || ln.kind != BattleLedger::Kind::Damage) continue;
                killer = ln.src;
            }
            if (killer && out.count(killer) && teamOf(killer) != teamOf(e.agent)) out[killer][SC_Kills] += 1.0;
        }
        for (const auto& [pid, uses] : d.uses)
        {
            auto it = out.find(pid);
            if (it == out.end()) continue;
            for (const auto& u : uses)
            {
                if (u.start < r0 || u.start > r1) continue;
                if (u.cancelled && !u.interrupted) it->second[SC_Cancelled] += 1.0;
                else it->second[SC_Skills] += 1.0;
            }
        }
        // Time dead and distance from the per-second track, which starts gateSeconds before 0:00.
        const int pre = job.gateSeconds;
        for (const auto& pl : job.players)
        {
            auto it = out.find(pl.agent);
            if (it == out.end()) continue;
            const int n = (int)pl.pos.size() / 4;
            if (n == 0) continue;
            const int a = std::clamp((int)std::ceil(r0) + pre, 0, n - 1), b = std::clamp((int)std::floor(r1) + pre, 0, n - 1);
            for (int i = a; i <= b; ++i)
            {
                if (pl.pos[i * 4 + 3] == 1) it->second[SC_Dead] += 1.0;
                if (i > a && pl.pos[i * 4 + 3] == 0 && pl.pos[(i - 1) * 4 + 3] == 0)
                {
                    const double dx = pl.pos[i * 4] - pl.pos[(i - 1) * 4], dy = pl.pos[i * 4 + 1] - pl.pos[(i - 1) * 4 + 1];
                    it->second[SC_Distance] += std::sqrt(dx * dx + dy * dy);
                }
            }
        }
        return out;
    }

    bool IsCountCol(int col)
    {
        return col == SC_Kills || col == SC_Deaths || col == SC_Ints || col == SC_IntsRecv || col == SC_Skills || col == SC_Cancelled;
    }

    std::string StatText(double v, int col, bool perMinute)
    {
        char b[32];
        if (col == SC_Dead && perMinute) { if (v < 0.5) return "-"; snprintf(b, sizeof b, "%d%%", (int)std::lround(v)); return b; }
        if (v < 0.05) return "-";
        if (col == SC_Dead) { snprintf(b, sizeof b, "%d s", (int)std::lround(v)); return b; }
        if (perMinute && IsCountCol(col)) { snprintf(b, sizeof b, "%.1f", v); return b; }
        const long long n = std::llround(v);
        std::string raw = std::to_string(n), s;
        for (int i = (int)raw.size() - 1, k = 0; i >= 0; --i, ++k)
        {
            if (k && k % 3 == 0) s.insert(s.begin(), ',');
            s.insert(s.begin(), raw[i]);
        }
        return s;
    }
}

namespace
{
    // One player's cell of a catalogue column over [r0, r1]: `v` is what sorts and ranks (already
    // per minute when asked), num / den a ratio's counts, n how many records a median rests on.
    struct ExtraCell { double v = 0.0, num = 0.0, den = 0.0; int n = 0; };

    double Median(std::vector<float> v)
    {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        const size_t h = v.size() / 2;
        return v.size() % 2 ? v[h] : 0.5 * (v[h - 1] + v[h]);
    }

    std::unordered_map<int, ExtraCell> ExtraValues(const MatchAnalysisJob& job, const MatchStats::StatDef& def,
                                                   float r0, float r1, bool perMinute)
    {
        using MatchStats::Agg;
        std::unordered_map<int, ExtraCell> out;
        auto recsOf = [&](const char* key) -> const std::vector<MatchStats::Record>* {
            if (!key) return nullptr;
            auto it = job.stats.find(key);
            return it == job.stats.end() ? nullptr : &it->second;
        };
        auto inside = [&](const MatchStats::Record& r) { return r.t >= r0 && r.t <= r1; };
        const float minutes = std::max(1.0f / 60.0f, (r1 - r0) / 60.0f);
        if (def.agg == Agg::Ratio)
        {
            if (const auto* num = recsOf(def.num)) for (const auto& r : *num) if (inside(r)) out[r.agent].num += 1.0;
            if (const auto* den = recsOf(def.den)) for (const auto& r : *den) if (inside(r)) out[r.agent].den += 1.0;
            for (auto& [pid, c] : out) c.v = c.den > 0.0 ? c.num / c.den : 0.0;
            return out;
        }
        const auto* recs = recsOf(def.key);
        if (!recs) return out;
        std::unordered_map<int, std::vector<float>> values;
        for (const auto& r : *recs)
        {
            if (def.agg == Agg::Time)
            {
                const double a = std::max<double>(r.t, r0), b = std::min<double>(r.t + r.dur, r1);
                if (b > a) out[r.agent].v += b - a;
                continue;
            }
            if (!inside(r)) continue;
            ExtraCell& c = out[r.agent];
            ++c.n;
            if (def.agg == Agg::Count) c.v += 1.0;
            else if (def.agg == Agg::Sum) c.v += r.value;
            else values[r.agent].push_back(r.value);
        }
        if (def.agg == Agg::Median)
            for (auto& [pid, list] : values) out[pid].v = Median(list);
        if (perMinute)
            for (auto& [pid, c] : out)
            {
                if (def.agg == Agg::Time) c.v = c.v / std::max(1.0f, r1 - r0) * 100.0;
                else if (def.agg == Agg::Count || def.agg == Agg::Sum) c.v /= minutes;
            }
        return out;
    }

    std::string ExtraText(const ExtraCell& c, const MatchStats::StatDef& def, bool perMinute)
    {
        using MatchStats::Agg;
        char b[48];
        const char* est = def.estimated ? "~" : "";
        switch (def.agg)
        {
        case Agg::Ratio:
            if (c.den <= 0.0) return "-";
            snprintf(b, sizeof b, "%d/%d  %d%%", (int)c.num, (int)c.den, (int)std::lround(c.v * 100.0));
            return b;
        case Agg::Median:
            if (c.n == 0) return "-";
            snprintf(b, sizeof b, "%s%.2f s", est, c.v);
            return b;
        case Agg::Time:
            if (c.v < 0.5) return "-";
            snprintf(b, sizeof b, perMinute ? "%s%d%%" : "%s%d s", est, (int)std::lround(c.v));
            return b;
        case Agg::Count:
            if (c.v < 0.05) return "-";
            if (perMinute) { snprintf(b, sizeof b, "%s%.1f", est, c.v); return b; }
            snprintf(b, sizeof b, "%s%d", est, (int)std::lround(c.v));
            return b;
        case Agg::Sum:
            break;
        }
        if (c.v < 0.05) return "-";
        const long long n = std::llround(c.v);
        std::string raw = std::to_string(n), s;
        for (int i = (int)raw.size() - 1, k = 0; i >= 0; --i, ++k)
        {
            if (k && k % 3 == 0) s.insert(s.begin(), ',');
            s.insert(s.begin(), raw[i]);
        }
        return est + s;
    }

    ImU32 ExtraHue(MatchStats::Hue h)
    {
        switch (h)
        {
        case MatchStats::Hue::Damage:    return IM_COL32(226, 80, 63, 0);
        case MatchStats::Hue::Healing:   return IM_COL32(108, 196, 86, 0);
        case MatchStats::Hue::Prevented: return IM_COL32(230, 200, 70, 0);
        case MatchStats::Hue::Interrupt: return IM_COL32(170, 110, 220, 0);
        case MatchStats::Hue::Energy:    return IM_COL32(80, 150, 235, 0);
        case MatchStats::Hue::Control:   return IM_COL32(232, 140, 60, 0);
        default:                         return IM_COL32(190, 196, 206, 0);
        }
    }

    // Favourite stats: starred in the picker, shown as columns every time, kept between sessions in
    // stats_favourites.txt beside gui_settings.ini (one stat key per line, in the order starred).
    std::vector<int> s_favs;          // kExtraBase + catalogue index
    bool s_favsLoaded = false;

    std::filesystem::path FavouritesFile() { return GuiGlobalConstants::GetSettingsFilePath().parent_path() / "stats_favourites.txt"; }

    // A fixed column is kept as "base:<name>". Once one fixed column is starred, the starred ones
    // replace the default set (the others start unticked).
    void LoadFavourites()
    {
        if (s_favsLoaded) return;
        s_favsLoaded = true;
        std::ifstream f(FavouritesFile());
        std::string key;
        bool anyBase = false;
        while (std::getline(f, key))
        {
            while (!key.empty() && (key.back() == '\r' || key.back() == ' ')) key.pop_back();
            if (key.empty()) continue;
            const int c = LayoutCol(key.c_str());
            if (c < 0 || std::find(s_favs.begin(), s_favs.end(), c) != s_favs.end()) continue;
            s_favs.push_back(c);
            anyBase |= c < SC_Count;
        }
        if (anyBase)
            for (bool& on : s_stats.on) on = false;
    }

    void SaveFavourites()
    {
        std::ofstream f(FavouritesFile(), std::ios::trunc);
        for (int c : s_favs)
        {
            if (c < SC_Count) f << "base:" << kStatCols[c].name << "\n";
            else if (const auto* d = ExtraDef(c)) f << d->key << "\n";
        }
    }

    bool IsFavourite(int c) { return std::find(s_favs.begin(), s_favs.end(), c) != s_favs.end(); }
    // A fixed column is on screen when ticked or starred.
    bool BaseShown(int c) { return s_stats.on[c] || IsFavourite(c); }

    // Starring keeps the column for good; unstarring leaves it shown for this session.
    void ToggleFavourite(int c)
    {
        auto it = std::find(s_favs.begin(), s_favs.end(), c);
        if (c < SC_Count)
        {
            // A fixed column: unstarring leaves it ticked for this session.
            if (it != s_favs.end()) { s_favs.erase(it); s_stats.on[c] = true; }
            else s_favs.push_back(c);
            SaveFavourites();
            return;
        }
        auto ex = std::find(s_stats.extra.begin(), s_stats.extra.end(), c);
        if (it != s_favs.end()) { s_favs.erase(it); if (ex == s_stats.extra.end()) s_stats.extra.push_back(c); }
        else { s_favs.push_back(c); if (ex != s_stats.extra.end()) s_stats.extra.erase(ex); }
        SaveFavourites();
    }

    // The catalogue's columns on screen: the favourites, then this session's picks.
    std::vector<int> ShownExtras()
    {
        std::vector<int> v;
        for (int c : s_favs) if (ExtraDef(c)) v.push_back(c);
        for (int c : s_stats.extra) if (ExtraDef(c) && !IsFavourite(c)) v.push_back(c);
        return v;
    }

    // A five-pointed star (the app font has no star glyph): filled gold, or an outline.
    void DrawStar(ImDrawList* dl, ImVec2 c, float r, bool filled, ImU32 col)
    {
        ImVec2 pts[10];
        for (int i = 0; i < 10; ++i)
        {
            const float a = -1.5707963f + i * 0.6283185f, rr = (i & 1) ? r * 0.45f : r;
            pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
        }
        if (filled)
            for (int i = 0; i < 10; ++i) dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], col);
        else
            dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.2f);
    }

    // A star button the size of a text line; true when clicked.
    bool StarButton(bool on)
    {
        const float h = ImGui::GetFrameHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##star", ImVec2(h, h));
        const bool hov = ImGui::IsItemHovered();
        DrawStar(ImGui::GetWindowDrawList(), ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.36f, on || hov,
                 on ? IM_COL32(240, 200, 80, 255) : hov ? IM_COL32(240, 200, 80, 140) : IM_COL32(150, 156, 165, 200));
        return clicked;
    }

    // Every keyword of the picker's search appears in the stat's label, head, group or help.
    bool MatchesSearch(const MatchStats::StatDef& d, const char* query)
    {
        auto lower = [](std::string s) { for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; };
        const std::string hay = lower(std::string(d.label) + " " + d.head + " " + d.group + " " + d.sub + " " + d.meaning + " " + d.help);
        std::string q = lower(query), word;
        for (size_t i = 0; i <= q.size(); ++i)
        {
            if (i == q.size() || q[i] == ' ')
            {
                if (!word.empty() && hay.find(word) == std::string::npos) return false;
                word.clear();
            }
            else word += q[i];
        }
        return true;
    }
}

// The Stats tab's columns panel (left of the table). Its own function, never inlined: inside DrawStatsTab's large
// frame MSVC once emitted an aligned SSE load (movdqa) to an 8-aligned stack slot here, and opening
// the picker crashed reading 0xFFFFFFFFFFFFFFFF (2026-10-30).
static __declspec(noinline) void DrawStatsPicker()
{
    // Clear all: every ticked column off (and the search); starred ones stay, they are kept on purpose.
    const float clearW = ImGui::CalcTextSize("Clear all").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearW - 6.0f);
    ImGui::InputTextWithHint("##search", "Search stats (kd, energy, flag...)", s_stats.search, sizeof s_stats.search);
    ImGui::SameLine(0.0f, 6.0f);
    if (ImGui::Button("Clear all"))
    {
        s_stats.extra.clear();
        for (bool& on : s_stats.on) on = false;
        s_stats.search[0] = 0;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Untick every column. Starred columns stay: unstar them to remove them.");
    ImGui::TextColored(ImVec4(0.59f, 0.61f, 0.65f, 1.0f), "Tick to show a column; star to keep it every time.");
    ImGui::Separator();
    ImGui::BeginChild("##picker_list", ImVec2(0.0f, 0.0f), false);
    const auto& cat = MatchStats::Catalog();
    int toggleFav = -1;
    // One stat's line: star, tick box, label, its short column name dimmed.
    auto row = [&](int c, const MatchStats::StatDef& d, const char* idSuffix) {
        ImGui::PushID(c);
        ImGui::PushID(idSuffix);
        const bool fav = IsFavourite(c);
        if (StarButton(fav)) toggleFav = c;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(fav ? "Favourite: shown every time. Click to unstar." : "Star: show this column every time.");
        ImGui::SameLine(0.0f, 2.0f);
        bool on = fav || std::find(s_stats.extra.begin(), s_stats.extra.end(), c) != s_stats.extra.end();
        if (fav) ImGui::BeginDisabled();
        if (ImGui::Checkbox(d.label, &on))
        {
            if (on) s_stats.extra.push_back(c);
            else s_stats.extra.erase(std::find(s_stats.extra.begin(), s_stats.extra.end(), c));
        }
        if (fav) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ColTooltip(c);
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.47f, 0.50f, 1.0f), "%s", d.head);
        ImGui::PopID();
        ImGui::PopID();
    };
    const bool searching = s_stats.search[0] != '\0';
    if (!searching && !s_favs.empty())
    {
        ImGui::TextColored(ImVec4(0.94f, 0.78f, 0.31f, 1.0f), "Favourites");
    }
    // A fixed column's line: star, tick box, label, its short name dimmed, like a stat's.
    auto baseRow = [&](int c, const char* idSuffix) {
        ImGui::PushID(c);
        ImGui::PushID(idSuffix);
        const bool fav = IsFavourite(c);
        if (StarButton(fav)) toggleFav = c;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(fav ? "Favourite: shown every time. Click to unstar." : "Star: show this column every time.");
        ImGui::SameLine(0.0f, 2.0f);
        bool on = BaseShown(c);
        if (fav) ImGui::BeginDisabled();
        if (ImGui::Checkbox(kStatCols[c].label, &on)) s_stats.on[c] = on;
        if (fav) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ColTooltip(c);
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.47f, 0.50f, 1.0f), "%s", kStatCols[c].head);
        ImGui::PopID();
        ImGui::PopID();
    };
    if (!searching && !s_favs.empty())
        for (int c : s_favs)
        {
            if (c < SC_Count) baseRow(c, "fav");
            else if (const auto* d = ExtraDef(c)) row(c, *d, "fav");
        }
    auto baseMatches = [&](int c, const MatchStats::LayoutEntry& e) {
        MatchStats::StatDef probe;
        probe.label = kStatCols[c].label; probe.head = kStatCols[c].head; probe.help = kStatCols[c].help;
        probe.group = e.group; probe.sub = e.sub;
        return MatchesSearch(probe, s_stats.search);
    };
    // The tree: group, sub-group (unless it repeats the group's name), then its stats.
    std::string group, sub;
    bool any = false;
    for (const auto& e : MatchStats::Layout())
    {
        const int c = LayoutCol(e.key);
        if (c < 0) continue;
        const auto* d = ExtraDef(c);
        if (d ? !MatchesSearch(*d, s_stats.search) : !baseMatches(c, e)) continue;
        any = true;
        if (group != e.group)
        {
            group = e.group; sub.clear();
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::PushFont(GuiGlobalConstants::boldFont ? GuiGlobalConstants::boldFont : ImGui::GetFont());
            ImGui::TextColored(ImVec4(0.94f, 0.78f, 0.31f, 1.0f), "%s", group.c_str());
            ImGui::PopFont();
        }
        if (sub != e.sub)
        {
            sub = e.sub;
            if (sub != group) ImGui::TextColored(ImVec4(0.72f, 0.64f, 0.42f, 1.0f), "  %s", sub.c_str());
        }
        ImGui::Indent(10.0f);
        if (d) row(c, *d, "all"); else baseRow(c, "all");
        ImGui::Unindent(10.0f);
    }
    // The detailed stats left out of the tree, found by search only.
    if (searching)
    {
        bool header = false;
        for (size_t i = 0; i < cat.size(); ++i)
        {
            const auto& d = cat[i];
            if (!d.hidden || !MatchesSearch(d, s_stats.search)) continue;
            if (!header)
            {
                header = true;
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                ImGui::TextColored(ImVec4(0.59f, 0.61f, 0.65f, 1.0f), "Detailed (search only)");
            }
            any = true;
            ImGui::Indent(10.0f);
            row(kExtraBase + (int)i, d, "all");
            ImGui::Unindent(10.0f);
        }
    }
    if (!any) ImGui::TextColored(ImVec4(0.59f, 0.61f, 0.65f, 1.0f), "%s", cat.empty() ? "No extra stats yet." : "No stat matches.");
    if (toggleFav >= 0) ToggleFavourite(toggleFav);
    ImGui::EndChild();
}

// A Stats table player's build card as a tooltip (the Overview's: skill bar, then attributes). The
// ledger knows players by name, so the match's PlayerMeta is found by its sanitized name.
static __declspec(noinline) void StatsPlayerTooltip(const MatchMeta& m, const std::string& name)
{
    for (const auto& [partyId, party] : m.parties)
        for (const auto& p : party.players)
        {
            if (p.encoded_name.empty() || SanitizePlayerName(p.encoded_name) != name) continue;
            const std::vector<SolvedAttribute>* build = nullptr;
            if (s_analysis.folderName == m.folder_name)
                if (auto it = s_analysis.builds.find(partyId + ":" + std::to_string(p.player_number)); it != s_analysis.builds.end())
                    build = &it->second;
            ImGui::BeginTooltip();
            DrawBuildCard(p, name, build, nullptr, GetMatchSkillView(m), 32.0f, false);
            ImGui::EndTooltip();
            return;
        }
}

static void DrawStatsTable(const MatchMeta& m, float r0, float r1)
{
    const MatchAnalysisJob& job = s_analysis;
    const BattleLedger::Data& d = job.ledger;
    const float minutes = std::max(1.0f / 60.0f, (r1 - r0) / 60.0f);
    auto raw = StatValues(job, r0, r1, GetMatchSkillView(m));
    std::vector<int> cols;
    for (int c = 0; c < SC_Count; ++c) if (BaseShown(c)) cols.push_back(c);
    for (int c : ShownExtras()) cols.push_back(c);
    std::unordered_map<int, std::unordered_map<int, ExtraCell>> extraCells;
    for (int c : cols)
        if (const auto* def = ExtraDef(c)) extraCells[c] = ExtraValues(job, *def, r0, r1, s_stats.perMinute);
    auto shown = [&](int pid, int c) -> double {
        if (ExtraDef(c))
        {
            const auto& cells = extraCells[c];
            auto it = cells.find(pid);
            return it == cells.end() ? 0.0 : it->second.v;
        }
        const double v = raw[pid][c];
        if (!s_stats.perMinute) return v;
        return c == SC_Dead ? v / std::max(1.0f, r1 - r0) * 100.0 : v / minutes;
    };
    // Whether a player has a value at all in a column (a median with no case, a ratio with no try).
    auto present = [&](int pid, int c) -> bool {
        const auto* def = ExtraDef(c);
        if (!def) return shown(pid, c) > 0.0;
        const auto& cells = extraCells[c];
        auto it = cells.find(pid);
        if (it == cells.end()) return false;
        if (def->agg == MatchStats::Agg::Median) return it->second.n > 0;
        if (def->agg == MatchStats::Agg::Ratio) return it->second.den > 0.0;
        return it->second.v > 0.0;
    };
    // Better first: the larger number, but the quicker one for a median (reactions, timings).
    auto better = [&](int a, int b, int c) {
        const auto* def = ExtraDef(c);
        if (def && def->agg == MatchStats::Agg::Median)
        {
            const bool pa = present(a, c), pb = present(b, c);
            if (pa != pb) return pa;
            return shown(a, c) < shown(b, c);
        }
        return shown(a, c) > shown(b, c);
    };
    auto cellText = [&](int pid, int c) -> std::string {
        if (const auto* def = ExtraDef(c))
        {
            const auto& cells = extraCells[c];
            auto it = cells.find(pid);
            return it == cells.end() ? std::string("-") : ExtraText(it->second, *def, s_stats.perMinute);
        }
        return StatText(shown(pid, c), c, s_stats.perMinute);
    };
    if (std::find(cols.begin(), cols.end(), s_stats.sort) == cols.end() && !cols.empty()) s_stats.sort = cols.front();

    const float lineH = ImGui::GetTextLineHeight();
    const float rowH = std::max(lineH + 8.0f, 24.0f), iconS = lineH + 2.0f;
    float nameW = 120.0f;
    for (int pid : d.players)
        if (auto it = d.agents.find(pid); it != d.agents.end())
            nameW = std::max(nameW, ImGui::CalcTextSize(it->second.name.c_str()).x);
    const float nameColW = iconS * 2.0f + 10.0f + nameW + 16.0f;
    float colW = 70.0f;
    for (int c : cols) colW = std::max(colW, ImGui::CalcTextSize(ColHead(c)).x + 26.0f);
    // Each column at the common width, a ratio's wider when "123/456  100%" needs it.
    std::vector<float> colX, colWs;
    float tableW = nameColW;
    for (int c : cols)
    {
        float w = colW;
        if (const auto* def = ExtraDef(c); def && def->agg == MatchStats::Agg::Ratio)
            w = std::max(w, ImGui::CalcTextSize("888/888  100%").x + 18.0f);
        colX.push_back(tableW);
        colWs.push_back(w);
        tableW += w;
    }

    // Each kind of figure in the colours of Bars and the client's digits: damage red, healing
    // green, prevented gold, interrupts purple; counts neutral.
    auto hue = [](int c) -> ImU32 {
        if (const auto* def = ExtraDef(c)) return ExtraHue(def->hue);
        switch (c)
        {
        case SC_Dealt: case SC_Taken:       return IM_COL32(226, 80, 63, 0);
        case SC_Healed: case SC_Received:   return IM_COL32(108, 196, 86, 0);
        case SC_Prevented:                  return IM_COL32(230, 200, 70, 0);
        case SC_Ints: case SC_IntsRecv:     return IM_COL32(170, 110, 220, 0);
        default:                            return IM_COL32(190, 196, 206, 0);
        }
    };
    ImFont* bold = GuiGlobalConstants::boldFont ? GuiGlobalConstants::boldFont : ImGui::GetFont();
    const float fs = ImGui::GetFontSize();

    ImGui::BeginChild("##stats_table", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 cDim = IM_COL32(150, 156, 165, 255), cGold = IM_COL32(225, 190, 80, 255);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    float y = o.y;

    // Header: click a column to sort both teams by it.
    dl->AddText(ImVec2(o.x + 6.0f, y + (rowH - lineH) * 0.5f), cDim, "PLAYER");
    for (size_t k = 0; k < cols.size(); ++k)
    {
        const int c = cols[k];
        const float x = o.x + colX[k], cw = colWs[k];
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(c);
        if (ImGui::InvisibleButton("##sort", ImVec2(cw - 2.0f, rowH))) s_stats.sort = c;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) ColTooltip(c, "Click to sort.", true);
        const bool sorted = s_stats.sort == c;
        const float tw = ImGui::CalcTextSize(ColHead(c)).x;
        const float tx = x + cw - 14.0f - tw;
        dl->AddText(ImVec2(tx, y + (rowH - lineH) * 0.5f), sorted ? cGold : hov ? IM_COL32(216, 220, 226, 255) : cDim, ColHead(c));
        if (ColLowerBetter(c))
        {
            // "Lower is better": a small hollow down arrow after the name (the sort mark is filled, before it).
            const float ax = tx + tw + 2.0f, ay = y + (rowH - lineH) * 0.5f;
            dl->AddTriangle(ImVec2(ax, ay + 1.0f), ImVec2(ax + 6.0f, ay + 1.0f), ImVec2(ax + 3.0f, ay + 5.5f), IM_COL32(230, 158, 115, 220), 1.0f);
        }
        if (sorted)
        {
            const float cy = y + rowH * 0.5f;     // a small down triangle before the name
            dl->AddTriangleFilled(ImVec2(tx - 11.0f, cy - 2.0f), ImVec2(tx - 4.0f, cy - 2.0f), ImVec2(tx - 7.5f, cy + 2.5f), cGold);
            dl->AddLine(ImVec2(x + 4.0f, y + rowH - 1.0f), ImVec2(x + cw - 6.0f, y + rowH - 1.0f), cGold, 1.5f);
        }
    }
    y += rowH + 2.0f;

    for (int side = 0; side < 2; ++side)
    {
        const uint8_t team = Team::FromIndex01(side);
        const ImU32 teamCol = Team::IsBlue(team) ? IM_COL32(74, 200, 255, 255) : IM_COL32(255, 107, 107, 255);
        std::vector<int> rows;
        for (int pid : d.players)
            if (auto it = d.agents.find(pid); it != d.agents.end() && it->second.team == team) rows.push_back(pid);
        std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) { return better(a, b, s_stats.sort); });

        // Team heading over a team-coloured rule.
        dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + tableW, y), (teamCol & 0x00FFFFFF) | 0xB0000000, 2.0f);
        const std::string guild = !d.guild[side].empty() ? d.guild[side] : Team::IsBlue(team) ? "Blue team" : "Red team";
        dl->AddText(bold, fs, ImVec2(o.x + 6.0f, y + (rowH - lineH) * 0.5f), teamCol, guild.c_str());
        y += rowH;

        // The team's standouts per column: first, second and third (nonzero only, ties share).
        std::unordered_map<int, std::unordered_map<int, int>> rank;
        for (int c : cols)
        {
            std::vector<int> order;
            for (int pid : rows) if (present(pid, c)) order.push_back(pid);
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return better(a, b, c); });
            for (size_t i = 0; i < order.size() && i < 3; ++i)
                rank[c][order[i]] = i > 0 && shown(order[i], c) == shown(order[i - 1], c) ? rank[c][order[i - 1]] : (int)i + 1;
        }

        int stripe = 0;
        for (int pid : rows)
        {
            // Faint stripes, and the whole row lit under the mouse to follow one player across.
            const ImVec2 ra(o.x, y), rb(o.x + tableW, y + rowH);
            if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ra, rb))
                dl->AddRectFilled(ra, rb, IM_COL32(255, 255, 255, 16), 2.0f);
            else if (stripe & 1)
                dl->AddRectFilled(ra, rb, IM_COL32(255, 255, 255, 6), 2.0f);
            ++stripe;
            const BattleLedger::Agent& ag = d.agents.at(pid);
            float x = o.x + 6.0f;
            if (ImTextureID t = ag.prof ? GetProfessionIcon(ag.prof) : nullptr)
                dl->AddImage(t, ImVec2(x, y + (rowH - iconS) * 0.5f), ImVec2(x + iconS, y + (rowH + iconS) * 0.5f));
            x += iconS + 2.0f;
            if (ImTextureID t = ag.sec ? GetProfessionIcon(ag.sec) : nullptr)
                dl->AddImage(t, ImVec2(x, y + (rowH - iconS) * 0.5f), ImVec2(x + iconS, y + (rowH + iconS) * 0.5f),
                             ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 170));
            x += iconS + 8.0f;
            dl->AddText(ImVec2(x, y + (rowH - lineH) * 0.5f), teamCol, ag.name.c_str());
            if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ra, ImVec2(o.x + nameColW, rb.y)))
                StatsPlayerTooltip(m, ag.name);
            for (size_t k = 0; k < cols.size(); ++k)
            {
                const int c = cols[k];
                const ImVec2 a(o.x + colX[k], y + 1.0f), b(a.x + colWs[k] - 2.0f, y + rowH - 1.0f);
                ImGui::SetCursorScreenPos(a);
                ImGui::PushID(pid * 32 + c);
                const bool clicked = ImGui::InvisibleButton("##cell", ImVec2(b.x - a.x, b.y - a.y));
                const bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                // Only the team's standouts are filled, in the column's colour: the first strongly
                // with a bold number, the second and third lightly; the rest are plain numbers.
                auto rit = rank[c].find(pid);
                const int place = rit == rank[c].end() ? 0 : rit->second;
                if (place) dl->AddRectFilled(a, b, hue(c) | ((ImU32)(place == 1 ? 105 : 38) << 24), 2.0f);
                if (hov) dl->AddRect(a, b, IM_COL32(225, 190, 80, 220), 2.0f);
                const std::string s = cellText(pid, c);
                ImFont* f = place == 1 ? bold : ImGui::GetFont();
                const float tw = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, s.c_str()).x;
                dl->AddText(f, fs, ImVec2(b.x - 8.0f - tw, y + (rowH - lineH) * 0.5f),
                            s == "-" ? IM_COL32(90, 88, 82, 255) : place == 1 ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 205, 212, 255),
                            s.c_str());
                if (hov && ExtraDef(c)) ImGui::SetTooltip("%s - %s: %s\n%s", ag.name.c_str(), ColLabel(c), s.c_str(),
                                                          ExtraDef(c)->meaning[0] ? ExtraDef(c)->meaning : ExtraDef(c)->help);
                else if (hov) ImGui::SetTooltip("%s - %s: %s\nClick for the breakdown (Bars, %s).", ag.name.c_str(), kStatCols[c].label,
                                                s.c_str(), BattleLedger::MetricName(kStatCols[c].ledgerTab));
                if (clicked && !ExtraDef(c))
                {
                    s_stats.bars = true;
                    s_stats.view.metric = kStatCols[c].ledgerTab;
                    s_stats.view.selPlayer = pid;
                    s_stats.view.matrix = false;
                    s_stats.view.openRows.clear();
                }
            }
            dl->AddLine(ImVec2(o.x, y + rowH), ImVec2(o.x + tableW, y + rowH), IM_COL32(255, 255, 255, 8));
            y += rowH;
        }
        // Team total (time dead per minute: the team's average share), on a faint band of the
        // team's colour under a rule.
        dl->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + tableW, y + rowH), (teamCol & 0x00FFFFFF) | 0x16000000, 2.0f);
        dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + tableW, y), (teamCol & 0x00FFFFFF) | 0x60000000, 1.0f);
        dl->AddText(bold, fs, ImVec2(o.x + 6.0f, y + (rowH - lineH) * 0.5f), cGold, "Team total");
        for (size_t k = 0; k < cols.size(); ++k)
        {
            const int c = cols[k];
            std::string s;
            if (const auto* def = ExtraDef(c))
            {
                // The team's own cell: counts and times summed (a time per minute: the average
                // share), a ratio over the summed tries, a median over every case of the team.
                ExtraCell tc;
                const auto& cells = extraCells[c];
                for (int pid : rows)
                    if (auto it = cells.find(pid); it != cells.end())
                    { tc.v += it->second.v; tc.num += it->second.num; tc.den += it->second.den; tc.n += it->second.n; }
                if (def->agg == MatchStats::Agg::Ratio) tc.v = tc.den > 0.0 ? tc.num / tc.den : 0.0;
                if (def->agg == MatchStats::Agg::Time && s_stats.perMinute && !rows.empty()) tc.v /= rows.size();
                if (def->agg == MatchStats::Agg::Median)
                {
                    std::vector<float> all;
                    auto it = job.stats.find(def->key);
                    std::unordered_set<int> mine(rows.begin(), rows.end());
                    if (it != job.stats.end())
                        for (const auto& r : it->second)
                            if (r.t >= r0 && r.t <= r1 && mine.count(r.agent)) all.push_back(r.value);
                    tc.n = (int)all.size();
                    tc.v = Median(std::move(all));
                }
                s = ExtraText(tc, *def, s_stats.perMinute);
            }
            else
            {
                double sum = 0.0;
                for (int pid : rows) sum += shown(pid, c);
                if (c == SC_Dead && s_stats.perMinute && !rows.empty()) sum /= rows.size();
                s = StatText(sum, c, s_stats.perMinute);
            }
            const float tw = bold->CalcTextSizeA(fs, FLT_MAX, 0.0f, s.c_str()).x;
            dl->AddText(bold, fs, ImVec2(o.x + colX[k] + colWs[k] - 10.0f - tw, y + (rowH - lineH) * 0.5f), cGold, s.c_str());
        }
        y += rowH;
        // What the map did to this team: already inside the players' Taken and Healed, shown apart
        // on two rows, the map's own effects (catapults, coral, acid, Miasma, the healing water)
        // and the flux (Minion Apocalypse).
        for (int group = 0; group < 2; ++group)
        {
            const bool fluxRow = group == 1;
            double mapV[SC_Count] = {};
            std::map<int, long long> byEffect[2];         // 0 damage taken, 1 healing received
            std::unordered_set<int> mine(rows.begin(), rows.end());
            for (const auto& ln : d.lines)
            {
                if (ln.t < r0) continue;
                if (ln.t > r1) break;
                if (!BattleLedger::IsMapRow(ln.rowKey) || !mine.count(ln.dst)) continue;
                if ((ln.rowKey == BattleLedger::kRowFlux) != fluxRow) continue;
                const int m = BattleLedger::LineMetricTaker(ln.kind);
                if (m == BattleLedger::kTaken)         { mapV[SC_Taken] += ln.amount;    byEffect[0][ln.rowKey] += ln.amount; }
                else if (m == BattleLedger::kReceived) { mapV[SC_Received] += ln.amount; byEffect[1][ln.rowKey] += ln.amount; }
            }
            const bool showTaken = BaseShown(SC_Taken) && mapV[SC_Taken] > 0.0;
            const bool showHealed = BaseShown(SC_Received) && mapV[SC_Received] > 0.0;
            if (showTaken || showHealed)
            {
                if (s_stats.perMinute) { mapV[SC_Taken] /= minutes; mapV[SC_Received] /= minutes; }
                // One icon per effect that struck (or healed) the team, then the label.
                LibraryLedgerHost host(GetMatchSkillView(m));
                std::set<int> keys;
                for (const auto& list : byEffect) for (const auto& [key, amount] : list) keys.insert(key);
                float ix = o.x + 6.0f;
                for (int key : keys)
                {
                    const ImVec2 a(ix, y + (rowH - iconS) * 0.5f), b(ix + iconS, y + (rowH + iconS) * 0.5f);
                    if (ImTextureID ic = BattleLedger::MapRowIcon(host, key))
                        dl->AddImageRounded(ic, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 2.0f);
                    if (ImGui::IsMouseHoveringRect(a, b)) ImGui::SetTooltip("%s", BattleLedger::MapRowName(key));
                    ix += iconS + 2.0f;
                }
                const float smallFs = fs * 0.9f;
                dl->AddText(ImGui::GetFont(), smallFs, ImVec2(ix + 6.0f, y + (rowH - smallFs) * 0.5f), IM_COL32(140, 146, 154, 255),
                            fluxRow ? "Flux" : "Map effects");
                for (size_t k = 0; k < cols.size(); ++k)
                {
                    const int c = cols[k];
                    if (c != SC_Taken && c != SC_Received) continue;
                    const ImVec2 a(o.x + colX[k], y + 1.0f), b(a.x + colWs[k] - 2.0f, y + rowH - 1.0f);
                    const std::string s = StatText(mapV[c], c, s_stats.perMinute);
                    const float tw = ImGui::GetFont()->CalcTextSizeA(smallFs, FLT_MAX, 0.0f, s.c_str()).x;
                    dl->AddText(ImGui::GetFont(), smallFs, ImVec2(b.x - 8.0f - tw, y + (rowH - smallFs) * 0.5f),
                                s == "-" ? IM_COL32(90, 88, 82, 255) : IM_COL32(140, 146, 154, 255), s.c_str());
                    const auto& list = byEffect[c == SC_Taken ? 0 : 1];
                    if (!list.empty() && ImGui::IsMouseHoveringRect(a, b))
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(fluxRow ? "Damage taken from the flux"
                                             : c == SC_Taken ? "Damage taken from the map" : "Healing received from the map");
                        for (const auto& [key, amount] : list)
                            ImGui::Text("%s  %s", BattleLedger::MapRowName(key),
                                        StatText(s_stats.perMinute ? amount / minutes : (double)amount, c, s_stats.perMinute).c_str());
                        ImGui::EndTooltip();
                    }
                }
                dl->AddLine(ImVec2(o.x, y + rowH), ImVec2(o.x + tableW, y + rowH), IM_COL32(255, 255, 255, 8));
                y += rowH;
            }
        }
        y += 10.0f;
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, y));
    ImGui::Dummy(ImVec2(tableW, 0.0f));
    ImGui::EndChild();
}

static void DrawStatsTab(const MatchMeta& m)
{
    const MatchAnalysisJob& job = s_analysis;
    if (s_stats.folder != m.folder_name) { s_stats.view = BattleLedger::ViewState(); s_stats.folder = m.folder_name; }

    // The interval on the match clock, as the Map tab reads it.
    const int pre = job.gateSeconds;
    const int seconds = job.players.empty() ? 0 : (int)(job.players.front().pos.size() / 4) - 1;
    const int duration = std::max(1, ParseDurationSeconds(m.match_duration) > 0
                                     ? ParseDurationSeconds(m.match_duration) : seconds - pre);
    const int span = duration + pre;
    const float r0 = s_state.detailIntervalFrom * span - pre;
    const float r1 = s_state.detailIntervalTo * span - pre;
    char a[16], b[16];
    FormatClock((int)std::floor(r0), a, sizeof a);
    FormatClock((int)std::ceil(r1), b, sizeof b);
    const bool full = s_state.detailIntervalFrom <= 0.0f && s_state.detailIntervalTo >= 1.0f;
    const std::string stretch = std::string(full ? "Full match " : "Selection ") + a + " - " + b;

    if (job.ledger.players.empty())
    {
        ImGui::TextColored(kColorText, "This match file has no Damage & Healing data.");
        return;
    }

    // ── VIEW pills, then the view's own choices ──
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.59f, 0.61f, 0.65f, 1.0f), "VIEW");
    ImGui::SameLine(0.0f, 8.0f);
    if (BattleLedger::Pill("Table", !s_stats.bars)) s_stats.bars = false;
    ImGui::SameLine(0.0f, 4.0f);
    if (BattleLedger::Pill("Bars", s_stats.bars)) s_stats.bars = true;
    if (!s_stats.bars)
    {
        // Total / Per minute at the right of the VIEW row; under it the columns panel on the left
        // (every stat by role, as tall as the tab) and the table beside it.
        LoadFavourites();
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        const float segW = ImGui::CalcTextSize("Total").x + ImGui::CalcTextSize("Per minute").x + 52.0f;
        ImGui::SameLine(0.0f, 0.0f);
        const float rowY = ImGui::GetCursorScreenPos().y;
        ImGui::SetCursorScreenPos(ImVec2(right - segW, rowY));
        if (BattleLedger::Pill("Total", !s_stats.perMinute)) s_stats.perMinute = false;
        ImGui::SameLine(0.0f, 4.0f);
        if (BattleLedger::Pill("Per minute", s_stats.perMinute)) s_stats.perMinute = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Divided by the interval's length.");
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        const float panelW = std::min(320.0f, ImGui::GetContentRegionAvail().x * 0.35f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 1.0f, 1.0f, 0.025f));
        if (ImGui::BeginChild("##stats_columns", ImVec2(panelW, 0.0f), false))
            DrawStatsPicker();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 12.0f);
        DrawStatsTable(m, r0, r1);
        return;
    }

    LibraryLedgerHost host(GetMatchSkillView(m));
    ImGui::SameLine(0.0f, 18.0f);
    ImGui::TextColored(ImVec4(0.67f, 0.69f, 0.72f, 1.0f), "Counting %s, the timeline's interval", stretch.c_str());
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    BattleLedger::DrawMetricTabs(host, s_stats.view);
    ImGui::BeginChild("##stats_bars", ImVec2(0, 0), false);
    BattleLedger::DrawBody(job.ledger, host, s_stats.view, r0, r1, stretch);
    ImGui::EndChild();
}

static void DrawMapTab(const MatchMeta& m)
{
    const auto& job = s_analysis;
    const int pre = job.gateSeconds;
    const int seconds = job.players.empty() ? 0 : (int)(job.players.front().pos.size() / 4) - 1;
    const int duration = std::max(1, ParseDurationSeconds(m.match_duration) > 0
                                     ? ParseDurationSeconds(m.match_duration) : seconds - pre);
    const int span = duration + pre;
    // Seconds on the match clock (negative before the gate); positions are stored per second of
    // the recording, so the index of clock time c is c + pre.
    const float fromT = s_state.detailIntervalFrom * span - pre;
    const float toT   = s_state.detailIntervalTo * span - pre;
    float& t = s_state.detailPlayhead;

    // Playback inside the interval; a playhead left outside it (the interval moved) starts over.
    if (t < fromT || t > toT) t = fromT;
    HandleMapTabHotkeys(fromT, toT);
    if (s_state.detailPlaying)
    {
        t += ImGui::GetIO().DeltaTime * s_state.detailPlaySpeed;
        if (t >= toT) { t = toT; s_state.detailPlaying = false; }
    }

    // ── The map area: the picture's own shape, as large as the tab allows above the play bar ──
    EnsureTextureBasePath();
    const MapPicture& pic = GetMapPicture(job.mapId >= 0 ? job.mapId : m.map_id);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float aspect = pic.loaded ? pic.aspect : 1.0f;
    const float areaH = std::max(80.0f, avail.y - 4.0f - kMapPlayBarH - 4.0f);
    // Each team's bars beside the map, Blue on the left and Red on the right, when there is
    // room for them and a map of a useful size.
    // One row per player: the bar's icons are as large as eight rows under the guild allow, then
    // smaller if the width runs short; below 16 px the columns are left out.
    const float lineH = ImGui::GetTextLineHeight();
    const float nameW = MapTeamNameWidth(m);
    const float colGap = 16.0f, mapMinW = 300.0f;
    const float rowFixedW = 2.0f * lineH + 7.0f + nameW + 10.0f;
    float skillSz = std::clamp((areaH - MapTeamHeaderH()) / 8.0f - kMapTeamRowGap, 16.0f, 40.0f);
    const float skillByWidth = ((avail.x - mapMinW) * 0.5f - colGap - rowFixedW) / 8.0f - 2.0f;
    skillSz = std::floor(std::min(skillSz, skillByWidth));
    const bool showTeams = skillSz >= 16.0f;
    const float colW = rowFixedW + 8.0f * (skillSz + 2.0f);
    const float mapMaxW = showTeams ? avail.x - 2.0f * (colW + colGap) : avail.x;
    float drawH = areaH;
    float drawW = drawH * aspect;
    if (drawW > mapMaxW) { drawW = mapMaxW; drawH = drawW / aspect; }
    const ImVec2 areaAt = ImGui::GetCursorScreenPos();
    const ImVec2 o(areaAt.x + std::max(0.0f, (avail.x - drawW) * 0.5f), areaAt.y + 2.0f);
    const ImVec2 e(o.x + drawW, o.y + drawH);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (showTeams)
    {
        DrawMapTeamColumn(m, "1", ImVec2(o.x - colGap - colW, o.y), ImVec2(colW, areaH), skillSz, nameW, t);
        DrawMapTeamColumn(m, "2", ImVec2(e.x + colGap, o.y), ImVec2(colW, areaH), skillSz, nameW, t);
        ImGui::SetCursorScreenPos(areaAt);
    }
    ImGui::Dummy(ImVec2(avail.x, std::max(drawH, showTeams ? areaH : 0.0f) + 4.0f));
    {
        const ImVec2 barAt = ImGui::GetCursorScreenPos();
        DrawMapPlayBar(ImVec2(barAt.x, barAt.y + 4.0f), avail.x, fromT, toT, t);
        ImGui::SetCursorScreenPos(barAt);
        ImGui::Dummy(ImVec2(avail.x, kMapPlayBarH + 4.0f));
    }
    const ImVec2 afterMap = ImGui::GetCursorScreenPos();

    // Zoom and pan. The view is the part of the picture between uv0 and uv1.
    float& zoom = s_state.detailMapZoom;
    float& cu = s_state.detailMapCenterU;
    float& cv = s_state.detailMapCenterV;
    ImGui::SetCursorScreenPos(o);
    ImGui::InvisibleButton("##map_view", ImVec2(drawW, drawH));
    const bool viewHovered = ImGui::IsItemHovered();
    {
        ImGuiIO& io = ImGui::GetIO();
        const float halfU = 0.5f / zoom, halfV = 0.5f / zoom;
        if (viewHovered && io.MouseWheel != 0.0f)
        {
            // Keep the point under the cursor where it is.
            const float mu = cu - halfU + (io.MousePos.x - o.x) / drawW * 2.0f * halfU;
            const float mv = cv - halfV + (io.MousePos.y - o.y) / drawH * 2.0f * halfV;
            const float newZoom = std::clamp(zoom * (io.MouseWheel > 0.0f ? 1.25f : 0.8f), 1.0f, 8.0f);
            cu = mu + (cu - mu) * zoom / newZoom;
            cv = mv + (cv - mv) * zoom / newZoom;
            zoom = newZoom;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
        {
            cu -= io.MouseDelta.x / drawW / zoom;
            cv -= io.MouseDelta.y / drawH / zoom;
        }
        if (viewHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            zoom = 1.0f; cu = 0.5f; cv = 0.5f;
        }
        const float h = 0.5f / zoom;
        cu = std::clamp(cu, h, 1.0f - h);
        cv = std::clamp(cv, h, 1.0f - h);
    }
    const ImVec2 uv0(cu - 0.5f / zoom, cv - 0.5f / zoom), uv1(cu + 0.5f / zoom, cv + 0.5f / zoom);
    ImGui::SetCursorScreenPos(afterMap);

    if (pic.loaded)
    {
        if (ImTextureID tex = GetTextureCache().GetTexture(pic.png))
            dl->AddImage(tex, o, e, uv0, uv1);
    }
    else
        dl->AddRect(o, e, ImGui::GetColorU32(kColorBorder), 4.0f);

    // Without a picture the positions still show, fitted to the area the players covered, on
    // the two widest axes of the recording.
    float fitMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, fitMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    int axisA = 0, axisB = 2;
    if (!pic.loaded)
    {
        for (const auto& p : job.players)
            for (size_t i = 0; i + 3 < p.pos.size(); i += 4)
                for (int a = 0; a < 3; ++a)
                {
                    fitMin[a] = std::min(fitMin[a], (float)p.pos[i + a]);
                    fitMax[a] = std::max(fitMax[a], (float)p.pos[i + a]);
                }
        int order[3] = { 0, 1, 2 };
        std::sort(order, order + 3, [&](int a, int b) { return fitMax[a] - fitMin[a] > fitMax[b] - fitMin[b]; });
        axisA = std::min(order[0], order[1]);
        axisB = std::max(order[0], order[1]);
        const char* note = "No picture of this map yet: opening one replay on it captures one.";
        dl->AddText(ImVec2(o.x + 8.0f, o.y + 6.0f), ImGui::GetColorU32(kColorTextDim), note);
    }
    auto place = [&](float x, float y, float z) -> ImVec2 {
        float u, v;
        if (pic.loaded)
        {
            u = pic.u[0] * x + pic.u[1] * y + pic.u[2] * z + pic.u[3];
            v = pic.v[0] * x + pic.v[1] * y + pic.v[2] * z + pic.v[3];
        }
        else
        {
            const float c[3] = { x, y, z };
            const float span = std::max(fitMax[axisA] - fitMin[axisA], fitMax[axisB] - fitMin[axisB]) * 1.1f;
            u = 0.5f + (c[axisA] - (fitMin[axisA] + fitMax[axisA]) * 0.5f) / std::max(1.0f, span);
            v = 0.5f - (c[axisB] - (fitMin[axisB] + fitMax[axisB]) * 0.5f) / std::max(1.0f, span);
        }
        return ImVec2(o.x + (u - uv0.x) / (uv1.x - uv0.x) * drawW,
                      o.y + (v - uv0.y) / (uv1.y - uv0.y) * drawH);
    };

    // The replay minimap's colours and marker sizes (ReplayWindow_Minimap.cpp).
    const ImU32 kBlueTeam = IM_COL32(0x99, 0xCB, 0xFD, 0xFF);
    const ImU32 kRedTeam  = IM_COL32(0xFF, 0x99, 0x9A, 0xFF);
    const ImU32 kGold     = IM_COL32(230, 195, 50, 255);
    const float playerR   = 8.0f;    // max(dotRadius + 3.5, 8) for a player
    ImFont* font = ImGui::GetFont();
    const float labelFs = font->FontSize * 0.85f;

    dl->PushClipRect(o, e, true);

    // ── Map objects, by the replay minimap's rules (ReplayWindow_Minimap.cpp) ──
    const auto& ob = job.objects;
    std::string objectHover;            // label of the object under the cursor
    ImVec2 objectHoverAt;
    float objectHoverD2 = FLT_MAX;
    auto npcIcon = [](const char* file) {
        return GetTextureCache().GetTexture(ui::TextureBasePath() + "\\Textures\\NPC\\" + file);
    };
    auto hoverTest = [&](ImVec2 c, float pick, const std::string& label) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const float d2 = (mp.x - c.x) * (mp.x - c.x) + (mp.y - c.y) * (mp.y - c.y);
        if (d2 <= pick * pick && d2 < objectHoverD2) { objectHoverD2 = d2; objectHover = label; objectHoverAt = c; }
    };
    auto iconAt = [&](ImTextureID tex, ImVec2 c, float half) {
        if (tex) dl->AddImage(tex, ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half));
        else
        {
            dl->AddCircleFilled(c, half * 0.5f + 1.0f, IM_COL32(0, 0, 0, 180));
            dl->AddCircleFilled(c, half * 0.5f, kGold);
        }
    };
    auto ownerFile = [](int owner, const char* red, const char* blue, const char* grey) {
        return owner == 2 ? red : owner == 1 ? blue : grey;
    };
    for (const auto& lm : job.landmarks)
    {
        if (lm.kind == "lord") continue;   // drawn with the NPCs
        const ImVec2 c = place(lm.x, lm.y, lm.z);
        if (lm.kind == "flag_stand" || lm.kind == "obelisk_stand")
        {
            // The stand shows who holds it: a red, blue or grey flag.
            const bool obelisk = lm.kind == "obelisk_stand";
            const int owner = obelisk ? ob.obeliskOwner.At(t) : ob.standOwner.At(t);
            iconAt(npcIcon(ownerFile(owner, "RedFlag.png", "BlueFlag.png", "GreyFlag.png")), c, 11.0f);
            hoverTest(c, 15.0f, obelisk ? "Obelisk Flag Stand" : "Tower Flag Stand");
        }
        else if (lm.kind == "health_shrine")
        {
            const int owner = ob.shrineOwner.At(t);
            iconAt(npcIcon(ownerFile(owner, "RedAnkh.png", "BlueAnkh.png", "GreyAnkh.png")), c, 11.0f);
            hoverTest(c, 15.0f, "Southern Health Shrine");
        }
        else if (lm.kind == "res_shrine")
        {
            const ImU32 col = lm.team == 2 ? kRedTeam : lm.team == 1 ? kBlueTeam : kGold;
            dl->AddCircleFilled(c, 4.5f, IM_COL32(0, 0, 0, 180));
            dl->AddCircleFilled(c, 3.5f, col);
            hoverTest(c, 12.0f, "Resurrection Shrine");
        }
    }

    // Gates and vine bridges: the open or grown marker, the closed marker otherwise.
    if (pic.loaded)
    {
        bool open[32] = {};
        for (const auto& [dt, door] : ob.doors)
        {
            if (dt > t) break;
            if (door.first > 0 && door.first < 32) open[door.first] = door.second != 0;
        }
        for (const auto& g : pic.gates)
        {
            const ImVec2 c(o.x + (g.u - uv0.x) / (uv1.x - uv0.x) * drawW,
                           o.y + (g.v - uv0.y) / (uv1.y - uv0.y) * drawH);
            const bool isOpen = g.door > 0 && g.door < 32 && open[g.door];
            iconAt(npcIcon(isOpen ? "texture_193320.dds" : "texture_191464.dds"), c, 16.0f);
            hoverTest(c, 18.0f, g.bridge ? (isOpen ? "Vine Bridge (Grown)" : "Vine Bridge") : "Gate");
        }
    }

    // Catapult levers (Warrior's Isle): once repaired, the border follows the firing cycle.
    for (const auto& cp : ob.catapults)
    {
        if (t < cp.repaired) continue;
        const ImVec2 c = place(cp.x, cp.y, cp.z);
        const CatapultState st = (CatapultState)cp.state.At(t, (int)CatapultState::Repaired);
        const float lh = 14.0f, bw = 2.0f;
        const ImVec2 outMin(c.x - lh, c.y - lh), outMax(c.x + lh, c.y + lh);
        const ImVec2 inMin(outMin.x + bw, outMin.y + bw), inMax(outMax.x - bw, outMax.y - bw);
        dl->AddRectFilled(outMin, outMax, CatapultStateBorderColor(st), 3.0f);
        dl->AddRectFilled(inMin, inMax, IM_COL32(10, 10, 10, 220), 2.0f);
        if (ImTextureID lever = npcIcon("Lever.png")) dl->AddImage(lever, inMin, inMax);
        hoverTest(c, 16.0f, std::string("Catapult - ") + CatapultStateName(st));
    }

    // ── NPCs: the minimap's small team dots, the guild lords larger; the dead not shown ──
    const ImVec2 mouseNpc = ImGui::GetIO().MousePos;
    const MatchAnalysisPlayer* hoveredNpc = nullptr;
    ImVec2 hoveredNpcAt;
    float hoveredNpcD2 = 8.0f * 8.0f;
    for (const auto& npc : job.npcs)
    {
        const int n = (int)npc.pos.size() / 4;
        if (n == 0) continue;
        const int s = std::clamp((int)std::lround(t + pre), 0, n - 1);
        if (npc.pos[s * 4 + 3] != 0) continue;
        const ImVec2 c = place((float)npc.pos[s * 4], (float)npc.pos[s * 4 + 1], (float)npc.pos[s * 4 + 2]);
        const float r = npc.lord ? 6.0f : 3.0f;
        const ImU32 col = npc.team == 2 ? kRedTeam : npc.team == 1 ? kBlueTeam : IM_COL32(255, 220, 100, 200);
        dl->AddCircleFilled(c, r + 1.0f, IM_COL32(0, 0, 0, 180));
        dl->AddCircleFilled(c, r, col);
        const float d2 = (mouseNpc.x - c.x) * (mouseNpc.x - c.x) + (mouseNpc.y - c.y) * (mouseNpc.y - c.y);
        if (d2 < hoveredNpcD2) { hoveredNpcD2 = d2; hoveredNpc = &npc; hoveredNpcAt = c; }
    }

    // ── Trails ──
    if (s_state.detailShowTrails)
    {
        for (const auto& p : job.players)
        {
            const int n = (int)p.pos.size() / 4;
            if (n == 0) continue;
            auto at = [&](int s) { s = std::clamp(s, 0, n - 1); return place((float)p.pos[s * 4], (float)p.pos[s * 4 + 1], (float)p.pos[s * 4 + 2]); };
            const ImU32 col = p.team == 2 ? kRedTeam : kBlueTeam;
            const int s0 = std::clamp((int)(fromT + pre), 0, n - 1);
            const int s1 = std::clamp((int)(t + pre), 0, n - 1);
            ImVec2 prev = at(s0);
            for (int s = s0 + 1; s <= s1; ++s)
            {
                const ImVec2 cur = at(s);
                // Older stretches fade, so where they are heading reads first.
                const float age = (float)(s1 - s) / std::max(1.0f, (float)(s1 - s0));
                const int alpha = (int)(170 - 120 * age);
                dl->AddLine(prev, cur, (col & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT), 2.0f);
                prev = cur;
            }
        }
    }

    // ── Players: profession icon on a black disc with a team ring; the dead greyed ──
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const MatchAnalysisPlayer* hovered = nullptr;
    ImVec2 hoveredAt;
    float hoveredD2 = 12.0f * 12.0f;
    for (int pass = 0; pass < 2; ++pass)   // the dead first, so the living draw over them
    {
        for (const auto& p : job.players)
        {
            const int n = (int)p.pos.size() / 4;
            if (n == 0) continue;
            auto at = [&](int s) { s = std::clamp(s, 0, n - 1); return place((float)p.pos[s * 4], (float)p.pos[s * 4 + 1], (float)p.pos[s * 4 + 2]); };

            // Between two recorded seconds the marker moves in a straight line.
            const float tp = t + pre;
            const int sa = std::clamp((int)std::floor(tp), 0, n - 1), sb = std::min(sa + 1, n - 1);
            const float f = std::clamp(tp - (float)sa, 0.0f, 1.0f);
            const ImVec2 a = at(sa), b = at(sb);
            const ImVec2 c(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f);
            const bool dead = p.pos[sa * 4 + 3] != 0;
            if (dead != (pass == 0)) continue;

            const ImU32 teamCol = p.team == 2 ? kRedTeam : kBlueTeam;
            ImTextureID prof = GetProfessionIcon(p.prof);
            const ImVec2 i0(c.x - playerR, c.y - playerR), i1(c.x + playerR, c.y + playerR);
            if (dead)
            {
                if (prof)
                {
                    dl->AddCircleFilled(c, playerR + 1.5f, IM_COL32(0, 0, 0, 205));
                    dl->AddImage(prof, i0, i1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(130, 130, 130, 180));
                    dl->AddCircle(c, playerR + 1.5f, p.team == 2 ? IM_COL32(140, 90, 90, 200)
                                                                 : IM_COL32(90, 110, 140, 200), 0, 1.6f);
                }
                else
                {
                    const float cr = 4.0f;
                    dl->AddLine(ImVec2(c.x - cr, c.y - cr), ImVec2(c.x + cr, c.y + cr), IM_COL32(0, 0, 0, 180), 2.5f);
                    dl->AddLine(ImVec2(c.x + cr, c.y - cr), ImVec2(c.x - cr, c.y + cr), IM_COL32(0, 0, 0, 180), 2.5f);
                    dl->AddLine(ImVec2(c.x - cr, c.y - cr), ImVec2(c.x + cr, c.y + cr), teamCol, 1.6f);
                    dl->AddLine(ImVec2(c.x + cr, c.y - cr), ImVec2(c.x - cr, c.y + cr), teamCol, 1.6f);
                }
            }
            else if (prof)
            {
                dl->AddCircleFilled(c, playerR + 1.5f, IM_COL32(0, 0, 0, 205));
                dl->AddImage(prof, i0, i1);
                dl->AddCircle(c, playerR + 1.5f, teamCol, 0, 1.6f);
            }
            else
            {
                dl->AddCircleFilled(c, 5.5f, IM_COL32(0, 0, 0, 180));
                dl->AddCircleFilled(c, 4.5f, teamCol);
            }

            if (s_state.detailShowNames)
            {
                const ImVec2 ts = font->CalcTextSizeA(labelFs, FLT_MAX, 0.0f, p.name.c_str());
                const ImVec2 lp(c.x - ts.x * 0.5f, c.y + playerR + 2.0f);
                const ImU32 labelCol = (teamCol & ~IM_COL32_A_MASK) | (0xE6u << IM_COL32_A_SHIFT);
                dl->AddText(font, labelFs, ImVec2(lp.x + 1.0f, lp.y + 1.0f), IM_COL32(0, 0, 0, 200), p.name.c_str());
                dl->AddText(font, labelFs, lp, labelCol, p.name.c_str());
            }

            const float d2 = (mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y);
            if (d2 < hoveredD2) { hoveredD2 = d2; hovered = &p; hoveredAt = c; }
        }
    }

    // ── Each team's flag off its stand, vine seeds and repair kits ──
    {
        const int s = std::max(0, (int)std::lround(t + pre));
        auto drawTrack = [&](const MatchAnalysisPlayer& tr, ImTextureID tex, ImU32 fallback) {
            const int n = (int)tr.pos.size() / 4;
            if (n == 0) return;
            const int i = std::min(s, n - 1);
            const int state = tr.pos[i * 4 + 3];
            if (state == 0) return;
            ImVec2 c = place((float)tr.pos[i * 4], (float)tr.pos[i * 4 + 1], (float)tr.pos[i * 4 + 2]);
            // Beside the carrier's marker when carried.
            if (state == 1) { c.x += 8.0f; c.y -= 8.0f; }
            const float h = 9.0f;
            if (tex) dl->AddImage(tex, ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h));
            else
            {
                dl->AddCircleFilled(c, 4.0f, IM_COL32(0, 0, 0, 180));
                dl->AddCircleFilled(c, 3.0f, fallback);
            }
        };
        for (const auto& f : ob.flags)
            drawTrack(f, GetOthersUiIcon(f.team == 2 ? "Red_flag_waving.svg.png" : "Blue_flag_waving.svg.png"),
                      f.team == 2 ? kRedTeam : kBlueTeam);
        for (const auto& b : ob.bundles)
        {
            const bool seed = b.name == "seed";
            drawTrack(b, npcIcon(seed ? "Vine Seed.png" : "RepairKit.png"),
                      seed ? IM_COL32(0x7C, 0xC8, 0x4A, 255) : IM_COL32(0xD2, 0xB4, 0x78, 255));
        }
    }

    if (!hovered && !hoveredNpc && !objectHover.empty() && ImGui::IsMouseHoveringRect(o, e))
    {
        const ImVec2 ts = font->CalcTextSizeA(labelFs, FLT_MAX, 0.0f, objectHover.c_str());
        const ImVec2 lp(objectHoverAt.x - ts.x * 0.5f, objectHoverAt.y + 14.0f);
        dl->AddRectFilled(ImVec2(lp.x - 4.0f, lp.y - 2.0f), ImVec2(lp.x + ts.x + 4.0f, lp.y + ts.y + 2.0f),
                          IM_COL32(0, 0, 0, 200), 3.0f);
        dl->AddText(font, labelFs, lp, IM_COL32(255, 255, 255, 240), objectHover.c_str());
    }

    // Hover label, as on the minimap: the name under the marker. NPCs always; players when the
    // names are hidden.
    if (!hovered && hoveredNpc && ImGui::IsMouseHoveringRect(o, e))
    {
        const ImVec2 ts = font->CalcTextSizeA(labelFs, FLT_MAX, 0.0f, hoveredNpc->name.c_str());
        const ImVec2 lp(hoveredNpcAt.x - ts.x * 0.5f, hoveredNpcAt.y + 8.0f);
        dl->AddRectFilled(ImVec2(lp.x - 4.0f, lp.y - 2.0f), ImVec2(lp.x + ts.x + 4.0f, lp.y + ts.y + 2.0f),
                          IM_COL32(0, 0, 0, 200), 3.0f);
        dl->AddText(font, labelFs, lp, IM_COL32(255, 255, 255, 240), hoveredNpc->name.c_str());
    }
    if (hovered && !s_state.detailShowNames && ImGui::IsMouseHoveringRect(o, e))
    {
        const ImU32 teamCol = hovered->team == 2 ? kRedTeam : kBlueTeam;
        const ImVec2 ts = font->CalcTextSizeA(labelFs, FLT_MAX, 0.0f, hovered->name.c_str());
        const ImVec2 lp(hoveredAt.x - ts.x * 0.5f, hoveredAt.y + playerR + 3.0f);
        dl->AddRectFilled(ImVec2(lp.x - 4.0f, lp.y - 2.0f), ImVec2(lp.x + ts.x + 4.0f, lp.y + ts.y + 2.0f),
                          IM_COL32(0, 0, 0, 200), 3.0f);
        dl->AddText(font, labelFs, lp, teamCol, hovered->name.c_str());
    }
    dl->PopClipRect();
}

static void DrawMatchDetailPanel(const MatchMeta& m, bool fillRemaining)
{
    const auto mode = s_state.layout;
    const auto sz = GetSizes(mode);
    const float sp = sz.spacing;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp + 4, sp + 2));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, s_themeColors.detailPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);

    ImVec2 detailSize = fillRemaining ? ImVec2(0, 0) : ImVec2(0, 0);
    ImGui::BeginChild("##match_detail", detailSize, ImGuiChildFlags_Border);

    // Header
    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::TextUnformatted("MATCH DETAILS");
    ImGui::PopStyleColor();
    DrawDetailTabs();

    float contentMaxX = ImGui::GetWindowContentRegionMax().x + ImGui::GetWindowPos().x;
    float contentW = contentMaxX - (ImGui::GetWindowContentRegionMin().x + ImGui::GetWindowPos().x);
    float lineY = ImGui::GetCursorScreenPos().y - ImGui::GetTextLineHeightWithSpacing();
    bool narrowHeader = (contentW < 320.0f);
    float replayBtnX = contentMaxX;

    // Replay Match button (left of close)
    {
        ImFont* semibold = GuiGlobalConstants::boldFont;
        if (semibold) ImGui::PushFont(semibold);

        const char* playIcon = "\xE2\x96\xB6";  // UTF-8 U+25B6 (▶)
        float fontSize = ImGui::GetFontSize();
        float iconFontSz = fontSize * 1.6f;

        float padX = 10.0f, padY = 6.0f;
        float iconW = fontSize * 1.2f;

        float btnW, btnH;
        if (narrowHeader)
        {
            btnW = iconW + padX * 2.0f;
        }
        else
        {
            ImVec2 labelSz = ImGui::CalcTextSize("REPLAY MATCH");
            btnW = iconW + 4.0f + labelSz.x + padX * 2.0f;
        }
        btnH = fontSize + padY * 2.0f;

        // Grown on hover. Safe to grow the frame itself here, unlike the refresh badge:
        // this button is placed absolutely against the panel's right edge, so nothing sits
        // beside it to be pushed around. Hover is last frame's, which at this size and
        // frame rate is not perceptible.
        static bool s_replayHot = false;
        if (s_replayHot) { btnW *= 1.06f; btnH *= 1.06f; }

        float closeW = 24.0f;
        float gap = 22.0f;
        float btnX = contentMaxX - closeW - gap - btnW;
        float btnY = lineY + (ImGui::GetTextLineHeightWithSpacing() - btnH) * 0.5f;
        replayBtnX = btnX;

        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(btnX, btnY));

        const ImVec4 colBtnBg      = s_themeColors.replayBtnBg;
        const ImVec4 colBtnHover   = s_themeColors.replayBtnHov;
        const ImVec4 colBtnActive  = s_themeColors.replayBtnAct;
        const ImVec4 colBorder     = kColorAccent;

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
        ImGui::PushStyleColor(ImGuiCol_Button,        colBtnBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  colBtnHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   colBtnActive);
        ImGui::PushStyleColor(ImGuiCol_Border,         colBorder);
        ImGui::PushStyleColor(ImGuiCol_Text,           kColorText);

        if (g_cloudDownloadInProgress)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
            ImGui::Button("###replay_btn", ImVec2(btnW, btnH));
            ImGui::PopStyleVar();
        }
        else
        {
            ImGui::Button("###replay_btn", ImVec2(btnW, btnH));
        }
        bool clicked = ImGui::IsItemClicked() && !g_cloudDownloadInProgress;
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();

        if (clicked)
        {
            g_pendingReplay.requested = true;
            g_pendingReplay.match = m;
        }

        s_replayHot = hovered;

        ImVec2 rMin = ImGui::GetItemRectMin();
        ImVec2 rMax = ImGui::GetItemRectMax();

        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* drawFont = semibold ? semibold : ImGui::GetFont();

        // Held back while a download has the button disabled: pulsing there would be
        // advertising an action that cannot currently be taken.
        if (!g_cloudDownloadInProgress)
            DrawPulseGlow(dl, rMin, rMax, ImGui::GetColorU32(kColorAccent), 5.0f);

        // Visual states: icon and text colors react to hover/press
        ImVec4 colGreen    (0.0f,  1.0f, 0.4f, 1.0f);
        ImVec4 colTextDraw = kColorText;

        if (active)
        {
            colGreen    = ImVec4(0.0f, 0.85f, 0.35f, 1.0f);
            colTextDraw = ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
        }
        else if (hovered)
        {
            colGreen    = ImVec4(0.15f, 1.0f, 0.55f, 1.0f);
            colTextDraw = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        }

        // Content offset: slight push on press
        float pushY = active ? 1.0f : 0.0f;

        // Draw ▶ icon
        {
            float iconX = rMin.x + padX;
            float iconY = rMin.y + (btnH - iconFontSz) * 0.5f + pushY;
            dl->AddText(drawFont, iconFontSz,
                ImVec2(iconX, iconY),
                ImGui::GetColorU32(colGreen),
                playIcon);
        }

        // Draw "REPLAY MATCH" text
        if (!narrowHeader)
        {
            float textX = rMin.x + padX + iconW + 4.0f;
            float textY = rMin.y + (btnH - fontSize) * 0.5f + pushY;
            dl->AddText(drawFont, fontSize,
                ImVec2(textX, textY),
                ImGui::GetColorU32(colTextDraw),
                "REPLAY MATCH");
        }

        if (semibold) ImGui::PopFont();

        // Hover: subtle accent glow border
        if (hovered && !active)
        {
            ImU32 glowCol = ImGui::GetColorU32(ImVec4(0.847f, 0.659f, 0.290f, 0.45f));
            dl->AddRect(rMin, rMax, glowCol, 5.0f, 0, 1.5f);
        }

        // Pressed: bright accent border
        if (active)
        {
            ImU32 brightGold = ImGui::GetColorU32(ImVec4(0.847f, 0.659f, 0.290f, 0.90f));
            dl->AddRect(rMin, rMax, brightGold, 5.0f, 0, 2.5f);
        }

        if (narrowHeader && hovered)
            ImGui::SetTooltip("Replay Match");
    }

    // Close button (rightmost)
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(contentMaxX - 20.0f, lineY + 1.0f));
    if (ImGui::SmallButton("X"))
    {
        s_state.selectedMatchIndex = -1;
        s_state.mobileShowDetail = false;
        s_state.detailMaximised = false;
    }

    // Maximise (left of Replay Match). Not offered on mobile, where the details
    // already replace the list.
    if (s_state.layout != LayoutMode::Mobile)
    {
        const float w = DetailHeaderButtonWidth(DetailMaximiseLabel(), 16.0f);
        DrawDetailMaximiseButton(ImVec2(replayBtnX - 16.0f - w, lineY));
    }
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowContentRegionMin().x + ImGui::GetWindowPos().x,
                                     lineY + ImGui::GetTextLineHeightWithSpacing()));
    ImGui::Separator();
    ImGui::Spacing();

    if (s_state.detailTab == DetailTab_Overview)
    {
        // The attributes need the match read, as the other tabs do: selecting a match starts it.
        UpdateMatchAnalysis(m);
        OverviewBuilds ob;
        using State = MatchAnalysisJob::State;
        if (s_analysis.folderName == m.folder_name && s_analysis.state == State::Ready)
        {
            ob.builds = &s_analysis.builds;
        }
        else
            ob.pending = s_analysis.state == State::Downloading ? "Downloading the match..."
                       : s_analysis.state == State::Failed      ? "Attributes unavailable"
                                                                : "Solving attributes...";
        DrawMatchOverview(m, ob);
    }
    else
    {
        UpdateMatchAnalysis(m);
        const float timelineH = std::max(140.0f, ImGui::GetTextLineHeight() * 8.5f);
        ImGui::BeginChild("##detail_tab_body", ImVec2(0, -timelineH));
        const bool analysed = s_analysis.state == MatchAnalysisJob::State::Ready && s_analysis.folderName == m.folder_name;
        if (s_state.detailTab == DetailTab_Map && analysed)
            DrawMapTab(m);
        else if (s_state.detailTab == DetailTab_Stats && analysed)
            DrawStatsTab(m);
        else
            DrawDetailTabPlaceholder(s_state.detailTab);
        ImGui::EndChild();
        DrawDetailTimeline(m, timelineH);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// ─── Main entry point ────────────────────────────────────────────────────────

bool replay_browser_scout_mode()
{
    return s_state.tournamentMode;
}

void replay_browser_set_scout_mode(bool on)
{
    s_state.tournamentMode = on;
}

void draw_replay_browser(ReplayLibrary& library)
{
    // Process new matches from RescanDiff — populate highlights and notification
    if (!library.GetNewMatchFolders().empty())
    {
        float now = (float)ImGui::GetTime();
        int count = (int)library.GetNewMatchFolders().size();

        for (auto& fp : library.GetNewMatchFolders())
            s_state.highlightStartTimes[fp] = now;

        // Build notification match name from the first new match
        std::string matchName;
        if (count == 1)
        {
            for (auto& m : library.GetMatches())
            {
                if (m.folder_path == library.GetNewMatchFolders()[0])
                {
                    // Try to build "Team1 vs Team2"
                    auto it1 = m.guilds.find("1");
                    auto it2 = m.guilds.find("2");
                    if (it1 != m.guilds.end() && it2 != m.guilds.end())
                        matchName = it1->second.name + " vs " + it2->second.name;
                    break;
                }
            }
        }

        NotifyNewMatches(count, matchName);
        library.ClearNewMatchFolders();
    }

    if (!library.IsLoaded() || library.GetMatches().empty())
    {
        // NOTHING IS DRAWN HERE, and a frame with nothing in it is the bare clear colour
        // with only the menu bar and any toast on top - which is what "the window came up
        // blank" means. Say so, once per state change, with the reason, so a log answers it.
        static int s_reported = -1;
        const int state = library.IsLoaded() ? 1 : 0;
        if (s_reported != state)
        {
            s_reported = state;
            RunLog::Line("replay browser: drawing nothing - the library is %s",
                         library.IsLoaded() ? "loaded but holds no matches"
                                            : "not scanned yet");
        }
        return;
    }

    {
        // ...and say when it recovers, so the log shows how long the blank frames lasted.
        static bool s_announced = false;
        if (!s_announced)
        {
            s_announced = true;
            RunLog::Line("replay browser: first frame drawn with %d match(es)",
                         (int)library.GetMatches().size());
        }
    }

    // Card gallery always uses Watchtower theme; table view uses user's choice
    int themeToApply = s_state.cardGalleryMode ? 1 : GuiGlobalConstants::replay_browser_theme;
    ApplyBrowserTheme(themeToApply);

    const auto& matches = library.GetMatches();
    s_libraryGeneration = library.GetGeneration();
    BuildFilterLists(matches);

    // Sync card gallery settings from global constants (may be changed via Settings window)
    s_state.cardGalleryMode = (GuiGlobalConstants::replay_card_gallery_mode != 0);
    s_state.galleryColumns = std::clamp(GuiGlobalConstants::replay_gallery_columns, 2, 4);
    (void)GuiGlobalConstants::replay_card_style; // unused, kept for settings compat

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 displaySize = io.DisplaySize;
    float totalW = displaySize.x;
    const float sp = 8.0f;

    // Compute responsive layout mode
    s_state.prevLayout = s_state.layout;
    s_state.layout = ComputeLayout(totalW);

    // Auto-collapse sidebar when crossing into narrow/mobile
    if (s_state.layout != s_state.prevLayout)
    {
        if (s_state.layout == LayoutMode::Narrow || s_state.layout == LayoutMode::Mobile)
            s_state.sidebarExpanded = false;
        else
            s_state.sidebarExpanded = true;
    }

    // Reset mobile detail view when crossing out of mobile
    if (s_state.layout != LayoutMode::Mobile)
        s_state.mobileShowDetail = false;

    ImGui::SetNextWindowPos(ImVec2(0, GuiGlobalConstants::ContentTop()), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(totalW, displaySize.y - GuiGlobalConstants::ContentTop()), ImGuiCond_Always);

    int themeColors = PushGlassTheme();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp, sp));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (!ImGui::Begin("##replay_browser", nullptr, flags))
    {
        ImGui::End();
        ImGui::PopStyleVar(4);
        PopGlassTheme(themeColors);
        return;
    }


    // Debounce global search (200ms)
    if (s_state.lastSearchEditTime >= 0.0f &&
        (float)ImGui::GetTime() - s_state.lastSearchEditTime >= 0.2f)
    {
        snprintf(s_state.searchDebounced, sizeof(s_state.searchDebounced),
                 "%s", s_state.searchBuf);
        s_state.lastSearchEditTime = -1.0f;
    }

    // Expire old highlight flashes (2000ms)
    {
        float now = (float)ImGui::GetTime();
        for (auto it = s_state.highlightStartTimes.begin(); it != s_state.highlightStartTimes.end();)
        {
            if (now - it->second > 2.f)
                it = s_state.highlightStartTimes.erase(it);
            else
                ++it;
        }
    }

    const auto& filtered = FilterMatches(matches);

    // Helper lambda: re-check selection validity (can change mid-frame via clicks)
    auto validSelection = [&]() {
        return s_state.selectedMatchIndex >= 0 && s_state.selectedMatchIndex < (int)matches.size();
    };

    if (s_state.layout == LayoutMode::Mobile && s_state.mobileShowDetail && validSelection())
    {
        // Mobile: detail replaces the list
        DrawFilterPanel(matches);
        ImGui::SameLine(0, sp);
        ImGui::BeginGroup();
        {
            DrawMatchListTable(filtered, matches);
            if (validSelection())
                DrawMatchDetailPanel(matches[s_state.selectedMatchIndex], true);
        }
        ImGui::EndGroup();
    }
    else if (s_state.detailMaximised && validSelection() && !s_state.cardGalleryMode
             && !s_state.tournamentMode)
    {
        // Maximised: the details take the whole Library until restored or closed.
        DrawMatchDetailPanel(matches[s_state.selectedMatchIndex], true);
    }
    else
    {
        // Two-row layout with user-resizable splitters:
        //   Top row:    Filter (left) | VSplitter | Match List (right)
        //   HSplitter   (full width, only when detail is open)
        //   Bottom row: Match Details (full width)

        const float splitterThick = 6.0f;
        float availW = ImGui::GetContentRegionAvail().x;
        float totalH = ImGui::GetContentRegionAvail().y;
        bool inTournament = s_state.tournamentMode;
        bool hasDetail = validSelection() && s_state.layout != LayoutMode::Mobile
                         && !s_state.cardGalleryMode && !inTournament;

        // ── Filter width (horizontal splitter state) ──
        const float filterMinW = 220.0f;
        const float filterMaxW = availW * 0.40f;

        if (s_state.userFilterW <= 0)
        {
            if (GuiGlobalConstants::replay_filter_width > 0)
                s_state.userFilterW = (float)GuiGlobalConstants::replay_filter_width;
            else
                s_state.userFilterW = std::max(filterMinW, availW * 0.18f);
        }
        s_state.userFilterW = std::clamp(s_state.userFilterW, filterMinW, filterMaxW);

        float filterW = s_state.sidebarExpanded ? s_state.userFilterW : 40.0f;

        // ── Top row height (vertical splitter state) ──
        float topRowH = 0;
        if (hasDetail)
        {
            const float listMinH  = 200.0f;
            const float listMaxH  = totalH * 0.90f;
            const float detailMinH = 50.0f;

            if (s_state.userTopRowH <= 0)
            {
                if (GuiGlobalConstants::replay_list_height > 0)
                    s_state.userTopRowH = (float)GuiGlobalConstants::replay_list_height;
                else
                    s_state.userTopRowH = totalH * 0.40f;
            }

            float maxTopRow = totalH - detailMinH - splitterThick;
            topRowH = std::clamp(s_state.userTopRowH, listMinH, std::min(listMaxH, maxTopRow));
            s_state.userTopRowH = topRowH;
        }

        float splitterH = (topRowH > 0) ? topRowH : totalH;

        // ── Draw top row ──
        DrawFilterPanel(matches, topRowH, filterW);

        if (s_state.sidebarExpanded)
        {
            if (VSplitter("##v_splitter", splitterH, splitterThick))
            {
                s_state.userFilterW += ImGui::GetIO().MouseDelta.x;
                s_state.userFilterW = std::clamp(s_state.userFilterW, filterMinW, filterMaxW);
            }
            if (ImGui::IsItemDeactivated())
                GuiGlobalConstants::SaveSettings();
        }
        else
        {
            ImGui::SameLine(0, sp);
        }

        GuiGlobalConstants::replay_filter_width = (int)s_state.userFilterW;

        if (s_state.tournamentMode)
            DrawTournamentStatsPanel(matches, topRowH, library.GetGeneration());
        else if (s_state.cardGalleryMode && s_state.layout != LayoutMode::Mobile)
            DrawCardGallery(filtered, matches, topRowH);
        else
            DrawMatchListTable(filtered, matches, topRowH);

        // ── Horizontal splitter + Detail (table mode only) ──
        if (hasDetail && !s_state.cardGalleryMode)
        {
            if (HSplitter("##h_splitter", availW, splitterThick))
            {
                s_state.userTopRowH += ImGui::GetIO().MouseDelta.y;
                float maxTopRow = totalH - 50.0f - splitterThick;
                s_state.userTopRowH = std::clamp(s_state.userTopRowH, 200.0f,
                    std::min(totalH * 0.90f, maxTopRow));
            }
            if (ImGui::IsItemDeactivated())
                GuiGlobalConstants::SaveSettings();

            GuiGlobalConstants::replay_list_height = (int)s_state.userTopRowH;

            if (validSelection())
                DrawMatchDetailPanel(matches[s_state.selectedMatchIndex]);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(4);
    PopGlassTheme(themeColors);
}
