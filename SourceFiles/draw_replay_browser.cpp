#include "pch.h"
#include "draw_replay_browser.h"
#include "ReplayLibrary.h"
#include "GuiGlobalConstants.h"
#include "TextureCache.h"
#include "GuildCapeCache.h"
#include "SkillDatabase.h"
#include "MatchRatings.h"
#include "MatchNotes.h"
#include "Net/HttpClient.h"
#include <algorithm>
#include <set>
#include <unordered_map>
#include <thread>

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
                                       const std::vector<int>& skills)
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

    // Attributes: 0 attributes, 0 bits per value
    pushBits(0, 4);  // number of attributes
    pushBits(0, 4);  // bits per attribute value

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

static const char* GetProfessionIconFile(int id)
{
    switch (id)
    {
    case 1:  return "[1] - Warrior.png";
    case 2:  return "[2] - Ranger.png";
    case 3:  return "[3] - Monk.png";
    case 4:  return "[4] - Necromancer.png";
    case 5:  return "[5] - Mesmer.png";
    case 6:  return "[6] - Elementalist.png";
    case 7:  return "[7] - Assassin.png";
    case 8:  return "[8] - Ritualist.png";
    case 9:  return "[9] - Paragon.png";
    case 10: return "[10] - Dervish.png";
    default: return nullptr;
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

// ─── Texture path resolvers ─────────────────────────────────────────────────

static std::string g_textureBasePath;

static void EnsureTextureBasePath()
{
    if (!g_textureBasePath.empty()) return;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Walk up from exe directory to find the Textures folder.
    // Exe is typically at <project>/x64/Release/GuildWarsObserver.exe
    // while Textures lives at <project>/Textures/
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures"))
        {
            g_textureBasePath = dir.string();
            return;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    // Fallback to exe directory
    g_textureBasePath = std::filesystem::path(exePath).parent_path().string();
}

static ImTextureID GetProfessionIcon(int profId)
{
    const char* file = GetProfessionIconFile(profId);
    if (!file) return nullptr;
    EnsureTextureBasePath();
    std::string path = g_textureBasePath + "\\Textures\\Professions_Icons\\" + file;
    return GetTextureCache().GetTexture(path);
}

// A slow breathing value in [0,1], for drawing the eye to a control without moving it.
// Driven off the shared clock so every pulsing control beats together rather than drifting
// apart, which is what makes two of them read as one deliberate cue.
static float BrowserPulse()
{
    constexpr float kPeriodSeconds = 1.9f;
    return 0.5f + 0.5f * sinf((float)ImGui::GetTime() * (2.0f * IM_PI / kPeriodSeconds));
}

// A soft halo just outside mn..mx, brightest at the pulse peak. Three rings with a falling
// alpha rather than one, so it reads as a glow instead of a second hard border.
static void DrawPulseGlow(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
                          ImU32 rgb, float rounding, float strength = 1.0f)
{
    const float p = BrowserPulse();
    for (int i = 0; i < 3; i++)
    {
        const float grow = 1.0f + i * 2.0f;
        const float a = (0.40f - i * 0.11f) * p * strength;
        if (a <= 0.0f) continue;
        const ImU32 col = (rgb & ~IM_COL32_A_MASK)
                        | ((ImU32)(a * 255.0f) << IM_COL32_A_SHIFT);
        dl->AddRect(ImVec2(mn.x - grow, mn.y - grow), ImVec2(mx.x + grow, mx.y + grow),
                    col, rounding + grow, 0, 1.5f);
    }
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
    const std::string path = g_textureBasePath + "\\Textures\\DDS\\ATEXDXT5\\texture_337393.dds";
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
    std::string path = g_textureBasePath + "\\Textures\\Guild_Halls\\" + file;
    return GetTextureCache().GetTexture(path);
}

static ImTextureID GetCupIcon()
{
    EnsureTextureBasePath();
    std::string path = g_textureBasePath + "\\Textures\\Game_UI\\cup.webp";
    return GetTextureCache().GetTexture(path);
}

static ImTextureID GetFluxIcon()
{
    EnsureTextureBasePath();
    std::string path = g_textureBasePath + "\\Textures\\Skill_Icons\\PvP_Flair.png";
    return GetTextureCache().GetTexture(path);
}

static ImTextureID GetGuildHallIcon()
{
    EnsureTextureBasePath();
    std::string path = g_textureBasePath + "\\Textures\\Game_UI\\GuildHallIcon.png";
    return GetTextureCache().GetTexture(path);
}

static ImTextureID GetDurationIcon()
{
    EnsureTextureBasePath();
    std::string path = g_textureBasePath + "\\Textures\\Game_UI\\Skill Description\\activation.png";
    return GetTextureCache().GetTexture(path);
}

static ImTextureID GetArenaIcon()
{
    EnsureTextureBasePath();
    std::string path = g_textureBasePath + "\\Textures\\Game_UI\\ArenaIcon.png";
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
        const std::string dir = g_textureBasePath + "\\Textures\\Match library\\";
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
        auto root = std::filesystem::path(g_textureBasePath) / "Textures" / "CapeAssets";
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
    std::string folder = g_textureBasePath + "\\Textures\\Skill_Icons";
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

static ImTextureID GetSkillIcon(int skillId)
{
    EnsureSkillIconIndex();
    auto it = g_skillIconIndex.find(skillId);
    if (it == g_skillIconIndex.end()) return nullptr;
    return GetTextureCache().GetTexture(it->second);
}

// ─── Skill tooltip helper ────────────────────────────────────────────────────

static ImTextureID GetSkillDescIcon(const char* filename)
{
    EnsureTextureBasePath();
    std::string path = g_textureBasePath + "\\Textures\\Game_UI\\Skill Description\\" + filename;
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
    const SkillInfo* si = view ? view->Get(skillId) : GetSkillDatabase().Get(skillId);
    if (!si) return;

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(340.0f);

    // Skill icon + name header
    ImTextureID icon = GetSkillIcon(skillId);
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

static ImVec4 kColorBg, kColorPanel, kColorPanelLight, kColorBorder;
static ImVec4 kColorAccent, kColorAccentDim;
static ImVec4 kColorText, kColorTextDim;
// Between the two: for the list's supporting columns, which should sit back from the guild
// names without receding as far as a disabled label.
static ImVec4 kColorTextMuted;
static ImVec4 kColorSelected, kColorHover;

// Color interpolation helper
static ImU32 LerpColor(ImU32 a, ImU32 b, float t) {
    float r = ((a >> 0) & 0xFF) * (1 - t) + ((b >> 0) & 0xFF) * t;
    float g = ((a >> 8) & 0xFF) * (1 - t) + ((b >> 8) & 0xFF) * t;
    float bl = ((a >> 16) & 0xFF) * (1 - t) + ((b >> 16) & 0xFF) * t;
    float al = ((a >> 24) & 0xFF) * (1 - t) + ((b >> 24) & 0xFF) * t;
    return IM_COL32((int)r, (int)g, (int)bl, (int)al);
}

// Card gallery visual hierarchy
static ImU32 kCardMapName, kCardDate, kCardDuration;
static ImU32 kCardGuildName, kCardGuildTag, kCardVS;
static ImU32 kCardTeamLabel, kCardProfSig, kCardBuildName, kCardViewDetails;
static ImU32 kCardMatchupBg, kCardMatchupRule;

// Theme-specific inline colors for PushGlassTheme
struct BrowserThemeColors {
    ImVec4 popupBg, frameBg, frameBgHov, frameBgAct;
    ImVec4 titleBgAct, scrollBg, scrollGrab, scrollGrabHov;
    ImVec4 button, buttonHov;
    ImVec4 tableHeaderBg, tableBorderStrong, tableBorderLight, tableRowBgAlt;
    ImVec4 rowHoverBg, rowSelectedBg;
    ImU32 splitterIdle, splitterActive, splitterHover;
    ImVec4 detailPanelBg;
    ImVec4 replayBtnBg, replayBtnHov, replayBtnAct;
    ImVec4 compTextCol;
    ImVec4 cardBg, cardBgSel, cardBorderSel, cardBorderIdle;
    // Netflix card
    ImU32 cardGradientBot, cardGradientMid;
    ImU32 cardHoverBorder;
    ImU32 cardFallbackBg;
};
static BrowserThemeColors s_themeColors;

static int s_appliedTheme = -1;

static void ApplyBrowserTheme(int theme)
{
    if (theme == s_appliedTheme) return;
    s_appliedTheme = theme;

    if (theme == 1) // Watchtower Dashboard - zinc bg, amber accent (#f59e0b)
    {
        kColorBg         = ImVec4(0.094f, 0.094f, 0.106f, 1.00f); // #18181b zinc-950
        kColorPanel      = ImVec4(0.094f, 0.094f, 0.106f, 0.55f); // surface 1
        kColorPanelLight = ImVec4(0.153f, 0.153f, 0.165f, 0.40f); // surface raised
        kColorBorder     = ImVec4(0.247f, 0.247f, 0.275f, 0.45f); // border soft
        kColorAccent     = ImVec4(0.961f, 0.620f, 0.043f, 1.00f); // #f59e0b amber
        kColorAccentDim  = ImVec4(0.961f, 0.620f, 0.043f, 0.50f); // amber dimmed
        kColorText       = ImVec4(0.894f, 0.894f, 0.906f, 1.00f); // #e4e4e7 zinc-200
        kColorTextDim    = ImVec4(0.631f, 0.631f, 0.667f, 1.00f); // #a1a1aa zinc-400
        kColorTextMuted  = ImVec4(0.789f, 0.789f, 0.810f, 1.00f); // #c9c9cf
        kColorSelected   = ImVec4(0.961f, 0.620f, 0.043f, 0.15f); // amber tint
        kColorHover      = ImVec4(0.961f, 0.620f, 0.043f, 0.10f); // amber tint

        kCardMapName     = IM_COL32(161, 161, 170, 255); // zinc-400
        kCardDate        = IM_COL32(161, 161, 170, 255); // zinc-400 (was zinc-500)
        kCardDuration    = IM_COL32(113, 113, 122, 255); // zinc-500 full alpha (was zinc-600 @70%)
        kCardGuildName   = IM_COL32(228, 228, 231, 255); // zinc-200
        kCardGuildTag    = IM_COL32(245, 158,  11, 180); // amber-500 @70% (was zinc-400)
        kCardVS          = IM_COL32(113, 113, 122, 255); // zinc-500 (was zinc-600)
        kCardTeamLabel   = IM_COL32(113, 113, 122, 255); // zinc-500 (was zinc-600)
        kCardProfSig     = IM_COL32(161, 161, 170, 255); // zinc-400
        kCardBuildName   = IM_COL32(245, 158, 11, 255);  // #f59e0b amber
        kCardViewDetails = IM_COL32(161, 161, 170, 255); // zinc-400 (was zinc-500)
        kCardMatchupBg   = IM_COL32(18,  18,  20, 255);  // slightly darker than base
        kCardMatchupRule = IM_COL32(245, 158, 11,  25);  // subtle amber rule

        s_themeColors.popupBg       = ImVec4(0.094f, 0.094f, 0.106f, 0.95f);
        s_themeColors.frameBg       = ImVec4(0.094f, 0.094f, 0.106f, 0.55f);
        s_themeColors.frameBgHov    = ImVec4(0.153f, 0.153f, 0.165f, 0.60f);
        s_themeColors.frameBgAct    = ImVec4(0.153f, 0.153f, 0.165f, 0.80f);
        s_themeColors.titleBgAct    = ImVec4(0.094f, 0.094f, 0.106f, 0.95f);
        s_themeColors.scrollBg      = ImVec4(0.07f, 0.07f, 0.08f, 0.50f);
        s_themeColors.scrollGrab    = ImVec4(0.247f, 0.247f, 0.275f, 0.60f);
        s_themeColors.scrollGrabHov = ImVec4(0.322f, 0.322f, 0.357f, 0.70f);
        s_themeColors.button        = ImVec4(0.153f, 0.153f, 0.165f, 0.40f);
        s_themeColors.buttonHov     = ImVec4(0.961f, 0.620f, 0.043f, 0.15f);
        s_themeColors.tableHeaderBg = ImVec4(0.12f, 0.12f, 0.13f, 0.45f);
        s_themeColors.tableBorderStrong = ImVec4(0.247f, 0.247f, 0.275f, 0.65f);
        s_themeColors.tableBorderLight  = ImVec4(0.247f, 0.247f, 0.275f, 0.30f);
        s_themeColors.tableRowBgAlt = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
        s_themeColors.rowHoverBg    = ImVec4(0.340f, 0.340f, 0.360f, 0.90f);
        s_themeColors.rowSelectedBg = ImVec4(0.961f, 0.620f, 0.043f, 0.20f);
        s_themeColors.splitterIdle  = IM_COL32(63, 63, 70, 115);    // border soft
        s_themeColors.splitterActive = IM_COL32(245, 158, 11, 255); // amber
        s_themeColors.splitterHover = IM_COL32(251, 191, 36, 180);  // amber hover
        s_themeColors.detailPanelBg = ImVec4(0.08f, 0.08f, 0.09f, 0.90f);
        s_themeColors.replayBtnBg   = ImVec4(0.094f, 0.094f, 0.106f, 1.0f);
        s_themeColors.replayBtnHov  = ImVec4(0.153f, 0.153f, 0.165f, 1.0f);
        s_themeColors.replayBtnAct  = ImVec4(0.12f, 0.12f, 0.13f, 1.0f);
        s_themeColors.compTextCol   = ImVec4(0.631f, 0.631f, 0.667f, 1.f); // zinc-400
        s_themeColors.cardBg        = ImVec4(0.094f, 0.094f, 0.106f, 0.55f);
        s_themeColors.cardBgSel     = ImVec4(0.961f, 0.620f, 0.043f, 0.08f);
        s_themeColors.cardBorderSel = ImVec4(0.961f, 0.620f, 0.043f, 1.0f); // amber
        s_themeColors.cardBorderIdle = ImVec4(0.247f, 0.247f, 0.275f, 0.45f);
        s_themeColors.cardGradientBot  = IM_COL32(14, 14, 16, 240);
        s_themeColors.cardGradientMid  = IM_COL32(14, 14, 16, 0);
        s_themeColors.cardHoverBorder  = IM_COL32(245, 158, 11, 180);
        s_themeColors.cardFallbackBg   = IM_COL32(22, 22, 26, 255);
    }
    else // Theme 0: GW Observer - warm near-black, desaturated gold
    {
        kColorBg         = ImVec4(0.075f, 0.075f, 0.075f, 1.00f); // #131313
        kColorPanel      = ImVec4(0.102f, 0.102f, 0.102f, 1.00f); // #1A1A1A
        kColorPanelLight = ImVec4(0.141f, 0.133f, 0.125f, 1.00f); // #242220
        kColorBorder     = ImVec4(0.239f, 0.227f, 0.200f, 1.00f); // #3D3A33
        kColorAccent     = ImVec4(0.878f, 0.710f, 0.388f, 1.00f); // #E0B563
        kColorAccentDim  = ImVec4(0.878f, 0.710f, 0.388f, 0.70f);
        kColorText       = ImVec4(0.941f, 0.937f, 0.914f, 1.00f); // #F0EFE9
        kColorTextDim    = ImVec4(0.612f, 0.580f, 0.533f, 1.00f); // #9C9488
        kColorTextMuted  = ImVec4(0.809f, 0.794f, 0.762f, 1.00f); // #CFCAC2
        kColorSelected   = ImVec4(0.228f, 0.185f, 0.101f, 0.90f);
        kColorHover      = ImVec4(0.202f, 0.163f, 0.089f, 0.70f);

        kCardMapName     = IM_COL32(175, 172, 165, 255);
        kCardDate        = IM_COL32(130, 127, 120, 255);
        kCardDuration    = IM_COL32(110, 107, 100, 180);
        kCardGuildName   = IM_COL32(240, 236, 225, 255);
        kCardGuildTag    = IM_COL32(190, 185, 170, 255); // warm silver
        kCardVS          = IM_COL32(120, 105,  75, 255);
        kCardTeamLabel   = IM_COL32(100,  97,  90, 255);
        kCardProfSig     = IM_COL32(160, 155, 145, 255);
        kCardBuildName   = IM_COL32(210, 185, 120, 255);
        kCardViewDetails = IM_COL32(140, 137, 130, 255);
        kCardMatchupBg   = IM_COL32( 14,  13,  10, 255);
        kCardMatchupRule = IM_COL32(196, 169, 106,  30);

        s_themeColors.popupBg       = ImVec4(0.055f, 0.055f, 0.055f, 0.97f);
        s_themeColors.frameBg       = ImVec4(0.078f, 0.078f, 0.078f, 0.85f); // #141414 well
        s_themeColors.frameBgHov    = ImVec4(0.110f, 0.106f, 0.098f, 0.90f);
        s_themeColors.frameBgAct    = ImVec4(0.145f, 0.133f, 0.108f, 0.95f);
        s_themeColors.titleBgAct    = ImVec4(0.08f, 0.07f, 0.06f, 0.95f);
        s_themeColors.scrollBg      = ImVec4(0.05f, 0.04f, 0.03f, 0.50f);
        s_themeColors.scrollGrab    = ImVec4(0.28f, 0.24f, 0.16f, 0.60f);
        s_themeColors.scrollGrabHov = ImVec4(0.38f, 0.32f, 0.22f, 0.70f);
        s_themeColors.button        = ImVec4(0.14f, 0.12f, 0.08f, 0.80f);
        s_themeColors.buttonHov     = ImVec4(0.25f, 0.22f, 0.14f, 0.80f);
        s_themeColors.tableHeaderBg = ImVec4(0.16f, 0.14f, 0.09f, 0.45f);
        s_themeColors.tableBorderStrong = ImVec4(0.20f, 0.17f, 0.10f, 1.00f);
        s_themeColors.tableBorderLight  = ImVec4(0.25f, 0.22f, 0.15f, 0.40f);
        s_themeColors.tableRowBgAlt = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
        s_themeColors.rowHoverBg    = ImVec4(0.340f, 0.300f, 0.220f, 0.90f);
        s_themeColors.rowSelectedBg = ImVec4(0.878f, 0.710f, 0.388f, 0.22f);
        s_themeColors.splitterIdle  = IM_COL32(46, 40, 30, 255);
        s_themeColors.splitterActive = IM_COL32(196, 169, 106, 255);
        s_themeColors.splitterHover = IM_COL32(196, 169, 106, 140);
        s_themeColors.detailPanelBg = ImVec4(0.07f, 0.065f, 0.05f, 0.90f);
        s_themeColors.replayBtnBg   = ImVec4(0.09f, 0.08f, 0.06f, 1.0f);
        s_themeColors.replayBtnHov  = ImVec4(0.16f, 0.14f, 0.10f, 1.0f);
        s_themeColors.replayBtnAct  = ImVec4(0.12f, 0.10f, 0.07f, 1.0f);
        s_themeColors.compTextCol   = ImVec4(0.65f, 0.58f, 0.42f, 1.f);
        s_themeColors.cardBg        = ImVec4(0.09f, 0.08f, 0.06f, 1.0f);
        s_themeColors.cardBgSel     = ImVec4(0.11f, 0.10f, 0.07f, 1.0f);
        s_themeColors.cardBorderSel = ImVec4(0.769f, 0.663f, 0.416f, 1.0f);
        s_themeColors.cardBorderIdle = ImVec4(0.18f, 0.15f, 0.10f, 1.0f);
        s_themeColors.cardGradientBot  = IM_COL32(10, 9, 7, 240);
        s_themeColors.cardGradientMid  = IM_COL32(10, 9, 7, 0);
        s_themeColors.cardHoverBorder  = IM_COL32(196, 169, 106, 180);
        s_themeColors.cardFallbackBg   = IM_COL32(25, 22, 18, 255);
    }
}

// Vertical splitter (drag left/right to resize columns). Returns true while dragging.
static bool VSplitter(const char* id, float height, float thickness = 6.0f)
{
    ImGui::SameLine(0, 0);
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(thickness, height));
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();

    ImU32 col = s_themeColors.splitterIdle;
    if (active)       col = s_themeColors.splitterActive;
    else if (hovered) col = s_themeColors.splitterHover;

    float lineX = cursor.x + thickness * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(lineX, cursor.y + 4.0f),
        ImVec2(lineX, cursor.y + height - 4.0f),
        col, 2.0f);

    if (hovered || active)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    ImGui::SameLine(0, 0);
    return active;
}

// Horizontal splitter (drag up/down to resize rows). Returns true while dragging.
static bool HSplitter(const char* id, float width, float thickness = 6.0f)
{
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, thickness));
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();

    ImU32 col = s_themeColors.splitterIdle;
    if (active)       col = s_themeColors.splitterActive;
    else if (hovered) col = s_themeColors.splitterHover;

    float lineY = cursor.y + thickness * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(cursor.x + 8.0f, lineY),
        ImVec2(cursor.x + width - 8.0f, lineY),
        col, 2.0f);

    if (hovered || active)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    return active;
}

static int PushGlassTheme()
{
    int count = 0;
    auto Push = [&](ImGuiCol idx, const ImVec4& col) { ImGui::PushStyleColor(idx, col); count++; };

    Push(ImGuiCol_WindowBg,           kColorBg);
    Push(ImGuiCol_ChildBg,            kColorPanel);
    Push(ImGuiCol_PopupBg,            s_themeColors.popupBg);
    Push(ImGuiCol_Border,             kColorBorder);
    Push(ImGuiCol_FrameBg,            s_themeColors.frameBg);
    Push(ImGuiCol_FrameBgHovered,     s_themeColors.frameBgHov);
    Push(ImGuiCol_FrameBgActive,      s_themeColors.frameBgAct);
    Push(ImGuiCol_TitleBg,            kColorBg);
    Push(ImGuiCol_TitleBgActive,      s_themeColors.titleBgAct);
    Push(ImGuiCol_ScrollbarBg,        s_themeColors.scrollBg);
    Push(ImGuiCol_ScrollbarGrab,      s_themeColors.scrollGrab);
    Push(ImGuiCol_ScrollbarGrabHovered, s_themeColors.scrollGrabHov);
    Push(ImGuiCol_ScrollbarGrabActive,  kColorAccentDim);
    Push(ImGuiCol_Header,             kColorSelected);
    Push(ImGuiCol_HeaderHovered,      kColorHover);
    Push(ImGuiCol_HeaderActive,       kColorSelected);
    Push(ImGuiCol_Button,             s_themeColors.button);
    Push(ImGuiCol_ButtonHovered,      s_themeColors.buttonHov);
    Push(ImGuiCol_ButtonActive,       kColorAccentDim);
    Push(ImGuiCol_Separator,          kColorBorder);
    Push(ImGuiCol_Text,               kColorText);
    Push(ImGuiCol_TextDisabled,       kColorTextDim);
    Push(ImGuiCol_TableHeaderBg,      s_themeColors.tableHeaderBg);
    Push(ImGuiCol_TableBorderStrong,  s_themeColors.tableBorderStrong);
    Push(ImGuiCol_TableBorderLight,   s_themeColors.tableBorderLight);
    Push(ImGuiCol_TableRowBg,         ImVec4(0.00f, 0.00f, 0.00f, 0.00f));
    Push(ImGuiCol_TableRowBgAlt,      s_themeColors.tableRowBgAlt);

    return count;
}

static void PopGlassTheme(int count)
{
    ImGui::PopStyleColor(count);
}

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

static const char* GetFluxDescription(const std::string& fluxName)
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

enum class LayoutMode { Full, Compact, Narrow, Mobile };

static LayoutMode ComputeLayout(float windowWidth)
{
    if (windowWidth > 1600.0f) return LayoutMode::Full;
    if (windowWidth > 1200.0f) return LayoutMode::Compact;
    if (windowWidth > 800.0f)  return LayoutMode::Narrow;
    return LayoutMode::Mobile;
}

struct ResponsiveSizes
{
    float profIcon;
    float skillIcon;
    float cupIcon;
    float mapImg;
    float spacing;      // base spacing unit (multiples of 8)
};

static ResponsiveSizes GetSizes(LayoutMode mode)
{
    switch (mode)
    {
    case LayoutMode::Full:    return { 22.0f, 36.0f, 14.0f, 140.0f, 8.0f };
    case LayoutMode::Compact: return { 20.0f, 30.0f, 13.0f, 120.0f, 8.0f };
    case LayoutMode::Narrow:  return { 18.0f, 26.0f, 12.0f, 100.0f, 8.0f };
    case LayoutMode::Mobile:  return { 18.0f, 24.0f, 12.0f, 80.0f,  8.0f };
    }
    return { 22.0f, 36.0f, 14.0f, 140.0f, 8.0f };
}

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

static std::string ToLower(const std::string& s)
{
    std::string r = s;
    for (auto& c : r) c = (char)tolower((unsigned char)c);
    return r;
}

static bool FuzzyMatch(const std::string& query, const std::string& target)
{
    std::string qLow = ToLower(query);
    std::string tLow = ToLower(target);
    size_t qi = 0;
    for (size_t ti = 0; ti < tLow.size() && qi < qLow.size(); ti++)
        if (tLow[ti] == qLow[qi]) qi++;
    return qi == qLow.size();
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
                    if (sk > 0) rawIds.insert(sk);

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
                    if (sk > 0) skills.push_back(CanonicalSkillId(sk));
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

static bool ComboFromVec(const char* label, int& current, const std::vector<std::string>& items)
{
    if (items.empty()) return false;
    if (current >= (int)items.size()) current = 0;
    const char* preview = items[current].c_str();

    bool changed = false;
    if (ImGui::BeginCombo(label, preview))
    {
        for (int i = 0; i < (int)items.size(); i++)
        {
            bool selected = (i == current);
            if (ImGui::Selectable(items[i].c_str(), selected))
            {
                current = i;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// ─── Auto-complete & chip helpers ────────────────────────────────────────────

// Make the "browse all" arrow button read as part of its adjoining search
// field (same frame colors) instead of the generic, mismatched button chrome
// it would otherwise inherit.
static void PushDropdownArrowStyle()
{
    ImGui::PushStyleColor(ImGuiCol_Button,        s_themeColors.frameBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, s_themeColors.frameBgHov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  s_themeColors.frameBgAct);
    ImGui::PushStyleColor(ImGuiCol_Text,          kColorTextDim);
}

static void PopDropdownArrowStyle()
{
    ImGui::PopStyleColor(4);
}

static bool HighlightedSelectable(const char* text, const char* query,
                                  const char* uid, bool fuzzy)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float lineH = ImGui::GetTextLineHeightWithSpacing();

    std::string hiddenId = std::string("##hl_") + uid;
    bool clicked = ImGui::Selectable(hiddenId.c_str(), false);

    float x = pos.x + 4.0f;
    float y = pos.y + (lineH - ImGui::GetTextLineHeight()) * 0.5f;
    ImU32 colNorm = ImGui::GetColorU32(kColorText);
    ImU32 colHL   = ImGui::GetColorU32(kColorAccent);

    if (!query || query[0] == '\0')
    {
        dl->AddText(ImVec2(x, y), colNorm, text);
        return clicked;
    }

    if (fuzzy)
    {
        std::string qLow = ToLower(query);
        std::string tLow = ToLower(text);
        size_t qi = 0;
        for (size_t ti = 0; ti < tLow.size(); ti++)
        {
            bool m = (qi < qLow.size() && tLow[ti] == qLow[qi]);
            if (m) qi++;
            char c[2] = { text[ti], '\0' };
            dl->AddText(ImVec2(x, y), m ? colHL : colNorm, c);
            x += ImGui::CalcTextSize(c).x;
        }
    }
    else
    {
        std::string tLow = ToLower(text);
        std::string qLow = ToLower(query);
        size_t mpos = tLow.find(qLow);
        if (mpos == std::string::npos)
        {
            dl->AddText(ImVec2(x, y), colNorm, text);
        }
        else
        {
            size_t qLen = strlen(query);
            if (mpos > 0)
            {
                dl->AddText(ImVec2(x, y), colNorm, text, text + mpos);
                x += ImGui::CalcTextSize(text, text + mpos).x;
            }
            dl->AddText(ImVec2(x, y), colHL, text + mpos, text + mpos + qLen);
            x += ImGui::CalcTextSize(text + mpos, text + mpos + qLen).x;
            if (text[mpos + qLen] != '\0')
                dl->AddText(ImVec2(x, y), colNorm, text + mpos + qLen);
        }
    }
    return clicked;
}

enum class MsRowStyle { Plain, Flux };

static std::string TruncateToWidth(const std::string& text, float maxW)
{
    if (maxW <= 0.0f) return std::string();
    if (ImGui::CalcTextSize(text.c_str()).x <= maxW) return text;

    const float ellW = ImGui::CalcTextSize("...").x;
    size_t n = text.size();
    while (n > 0 &&
           ImGui::CalcTextSize(text.c_str(), text.c_str() + n).x + ellW > maxW)
        n--;
    return text.substr(0, n) + "...";
}

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

static void DrawPillBadge(ImDrawList* dl, ImFont* font, float fontSize,
                          const char* text, ImVec2 pos, ImU32 textCol,
                          ImU32 bgCol = IM_COL32(255, 255, 255, 25), float rounding = 10.f)
{
    ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
    float padX = 6.0f, padY = 2.0f;
    ImVec2 mn(pos.x, pos.y);
    ImVec2 mx(pos.x + textSz.x + padX * 2, pos.y + textSz.y + padY * 2);
    dl->AddRectFilled(mn, mx, bgCol, rounding);
    dl->AddText(font, fontSize, ImVec2(pos.x + padX, pos.y + padY), textCol, text);
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
        ImGui::TextUnformatted(fm.guild1.display.c_str());
        if (m.winner_party_id == 1 && cupTex)
        {
            ImGui::SameLine(0, 4);
            ImGui::Image(cupTex, ImVec2(sz.cupIcon, sz.cupIcon));
        }
        ImGui::SameLine(0, 8);
        ImGui::TextColored(kColorTextDim, "vs");
        ImGui::SameLine(0, 8);
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

        if (isTeam2)
        {
            float x = areaX + areaW;
            dl->PushClipRect(ImVec2(areaX, curY), ImVec2(areaX + areaW, curY + nameSz + tagSz + 4.f), true);
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
            dl->PushClipRect(ImVec2(areaX, curY), ImVec2(areaX + areaW, curY + nameSz + tagSz + 4.f), true);
            dl->AddText(teamNameFont, nameSz, ImVec2(areaX, curY), nameCol, guild.name.c_str());
            if (tagBuf[0])
                dl->AddText(mono, tagSz, ImVec2(areaX, curY + nameSz + 1.f), tagCol, tagBuf);
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

    // Team name + [tag] + WON chip
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
                    ImTextureID skillTex = GetSkillIcon(p.used_skills[si]);
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

    // View toggle: Table | Cards | Prep
    ImGui::SameLine(0, 16);
    ImGui::TextColored(kColorTextDim, "|");
    ImGui::SameLine(0, 8);

    auto ViewBtn = [](const char* label, bool active) -> bool {
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.431f, 0.659f, 0.996f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.431f, 0.659f, 0.996f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.431f, 0.659f, 0.996f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.15f, 0.7f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.56f, 0.63f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 3));
        bool clicked = ImGui::Button(label);
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
        return clicked;
    };

    if (ViewBtn("Table", !s_state.cardGalleryMode && !s_state.tournamentMode))
    {
        s_state.cardGalleryMode = false;
        s_state.tournamentMode = false;
        GuiGlobalConstants::replay_card_gallery_mode = 0;
        GuiGlobalConstants::SaveSettings();
    }
    ImGui::SameLine(0, 2);
    if (ViewBtn("Cards", s_state.cardGalleryMode && !s_state.tournamentMode))
    {
        s_state.cardGalleryMode = true;
        s_state.tournamentMode = false;
        GuiGlobalConstants::replay_card_gallery_mode = 1;
        GuiGlobalConstants::SaveSettings();
    }
    ImGui::SameLine(0, 2);
    {
        // Tournament Prep button - amber accent when active
        bool prepActive = s_state.tournamentMode;
        if (prepActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.961f, 0.620f, 0.043f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.961f, 0.620f, 0.043f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.961f, 0.620f, 0.043f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.15f, 0.7f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.56f, 0.63f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 3));
        if (ImGui::Button("Scout"))
        {
            s_state.tournamentMode = !s_state.tournamentMode;
        }
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scout - what they run, and what beats them");
    }

    // ── Opponent guild search + map filter (inline in top bar when Scout is active) ──
    if (s_state.tournamentMode)
    {
        ImGui::SameLine(0, 16);
        ImGui::TextColored(kColorTextDim, "|");
        ImGui::SameLine(0, 8);

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
    constexpr float btnPadY = 2.0f;
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

    // Prep button in table header
    ImGui::SameLine(0, 8);
    CentreOnRow(ImGui::GetTextLineHeight() + btnPadY * 2.0f + 2.0f);   // +border
    {
        bool prepActive = s_state.tournamentMode;
        if (prepActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.961f, 0.620f, 0.043f, 0.15f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.961f, 0.620f, 0.043f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.961f, 0.620f, 0.043f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.15f, 0.7f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.56f, 0.63f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, btnPadY));
        if (ImGui::Button("Scout"))
        {
            s_state.tournamentMode = !s_state.tournamentMode;
        }
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scout - what they run, and what beats them");
    }
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

static void DrawTeamComposition(const MatchMeta& m, const std::string& partyId,
                                const GuildLabel& guild, bool isWinner,
                                const ResponsiveSizes& sz)
{
    // Use skill data matching this match's date
    static std::unordered_map<int, SkillDatabaseView> s_matchViewCache;
    int dateKey = m.year * 10000 + m.month * 100 + m.day;
    auto [cacheIt, inserted] = s_matchViewCache.try_emplace(dateKey);
    if (inserted)
        cacheIt->second = GetSkillDatabase().GetView(m.year, m.month, m.day);
    const auto& matchView = cacheIt->second;

    const float iconSize = sz.profIcon;
    const float skillIconSize = sz.skillIcon;
    const float smallIconSize = sz.cupIcon + 2.0f;

    static std::unordered_map<std::string, float> s_copyFeedbackTimes;

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

    static const ImVec4 kColorStatText = ImVec4(0.490f, 0.490f, 0.494f, 1.0f); // #7D7D7E

    const float availW = ImGui::GetContentRegionAvail().x;
    const float statColW = 42.0f;
    const float dmgColW = 50.0f;
    const float statIconSz = 34.0f;
    bool showSkills = (s_state.layout != LayoutMode::Mobile && s_state.layout != LayoutMode::Narrow
                       && availW >= 500.0f);
    const float nameColW = showSkills
        ? std::max(80.0f, availW * 0.18f)
        : std::max(80.0f, availW * 0.30f);

    struct StatCol { const char* hdr; const char* tooltip; const char* iconBase; float w; };
    // The hdr strings are the fallback shown when an icon fails to load.
    StatCol statCols[] = {
        { "K",   "Kills",                "Kills",       statColW },
        { "D",   "Deaths",               "Deaths",      statColW },
        { "DMG", "Damage Dealt",         "Dmg_Out",     dmgColW  },
        { "DMR", "Damage Received",      "Dmg_In",      dmgColW  },
        { "HEL", "Healing Done",         "Heal_Out",    dmgColW  },
        { "HLR", "Healing Received",     "Heal_In",     dmgColW  },
        { "INT", "Interrupts Received",  "Rupt_In",     statColW },
        { "CNC", "Cancelled Skills",     "Cancel",      statColW },
        { "SKL", "Skill Count",          "Skill_Count", statColW },
    };
    const int numStats = static_cast<int>(std::size(statCols));

    // Read a column off a player. The column order lives in exactly one place this way,
    // instead of in a header list, a totals loop and a per-row list that could drift apart.
    auto StatOf = [](const PlayerMeta& q, int si) -> int {
        switch (si) {
        case 0:  return q.kills;
        case 1:  return q.deaths;
        case 2:  return q.total_damage;
        case 3:  return q.total_damage_received;
        case 4:  return q.total_healing_dealt;
        case 5:  return q.total_healing_received;
        case 6:  return q.interrupted_count;
        case 7:  return q.cancelled_skills_count;
        default: return q.skills_finished;
        }
    };

    auto StatAvailable = [](const PlayerMeta& q, int si) -> bool {
        switch (si) {
        case 3:  return (q.preview_stats_available & PreviewDamageReceived) != 0;
        case 4:  return (q.preview_stats_available & PreviewHealingDealt) != 0;
        case 5:  return (q.preview_stats_available & PreviewHealingReceived) != 0;
        case 6:  return (q.preview_stats_available & PreviewInterrupted) != 0;
        case 7:  return (q.preview_stats_available & PreviewCancelledSkills) != 0;
        case 8:  return (q.preview_stats_available & PreviewSkillsFinished) != 0;
        default: return true;
        }
    };

    int totals[std::size(statCols)] = {};
    bool totalsAvailable[std::size(statCols)];
    std::fill(std::begin(totalsAvailable), std::end(totalsAvailable), true);
    for (const auto* pp : sorted)
        for (int si = 0; si < numStats; si++)
        {
            totals[si] += StatOf(*pp, si);
            totalsAvailable[si] &= StatAvailable(*pp, si);
        }

    float skillsNeeded = showSkills ? (8 * (skillIconSize + 2) + 16.0f) : 0.0f;
    float statsNeeded = 0.0f;
    for (int si = 0; si < numStats; si++) statsNeeded += statCols[si].w + 6.0f;
    float copyBtnW = skillIconSize * 0.45f + 4.0f;
    float fixedNeeded = iconSize + (iconSize + 2) + nameColW + copyBtnW + skillsNeeded;
    bool statsOverflow = (fixedNeeded + statsNeeded) > availW;

    bool showStats = true;
    if (statsOverflow)
        showStats = s_state.statsExpanded;

    int fixedCols = 3;
    int numCols = fixedCols + (showSkills ? 1 : 0) + 1 + (showStats ? numStats : 0);

    if (statsOverflow)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorHover);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        const char* lbl = s_state.statsExpanded ? "Stats <" : "Stats >";
        if (ImGui::SmallButton((std::string(lbl) + "###stattgl_" + partyId).c_str()))
            s_state.statsExpanded = !s_state.statsExpanded;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3, 2));
    ImGui::PushStyleColor(ImGuiCol_Text, kColorStatText);

    if (ImGui::BeginTable(("##team_" + partyId).c_str(), numCols,
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX))
    {
        ImGui::TableSetupScrollFreeze(fixedCols, 0);

        ImGui::TableSetupColumn("Pri",  ImGuiTableColumnFlags_WidthFixed, iconSize);
        ImGui::TableSetupColumn("Sec",  ImGuiTableColumnFlags_WidthFixed, iconSize + 2);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, nameColW);
        if (showSkills)
            ImGui::TableSetupColumn("Skills", ImGuiTableColumnFlags_WidthFixed,
                8 * (skillIconSize + 2) + 16.0f);
        ImGui::TableSetupColumn("##Copy", ImGuiTableColumnFlags_WidthFixed, copyBtnW);
        if (showStats)
            for (int si = 0; si < numStats; si++)
                ImGui::TableSetupColumn(statCols[si].hdr, ImGuiTableColumnFlags_WidthFixed, statCols[si].w);

        // Header row: the guild's identity on the left, the stat icons on the right.
        // Folding the two together reclaims the separate title line the guild used to have.
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            // The name is drawn from the first column but is far wider than it, so the clip
            // is widened across the cells to its right - all empty on this row - rather than
            // letting a long guild name be cut at the Name column's edge.
            const ImVec2 clipMin = ImGui::GetCursorScreenPos();
            const float clipW = iconSize + (iconSize + 2.0f) + nameColW
                              + skillsNeeded + copyBtnW;
            ImGui::PushClipRect(clipMin,
                                ImVec2(clipMin.x + clipW, clipMin.y + statIconSz + 8.0f), false);

            ImFont* bold = GuiGlobalConstants::boldFont;
            if (bold) ImGui::PushFont(bold);

            // Everything on this row lines up on the stat icons' centre rather than their top.
            const float rowTopY = ImGui::GetCursorPosY();
            auto CentreOnIcons = [&](float itemH) {
                ImGui::SetCursorPosY(rowTopY + (statIconSz - itemH) * 0.5f);
            };
            const float lineH = ImGui::GetTextLineHeight();
            constexpr float kNameScale = 1.15f;

            // The trophy leads the winner rather than trailing the name: at a glance the eye
            // finds the marker before it has read either guild. The art is 64x109, so the
            // width follows the height or the cup comes out squashed.
            if (isWinner)
            {
                if (ImTextureID cup = GetCupIcon())
                {
                    const float trophyH = statIconSz;
                    const float trophyW = trophyH * (64.0f / 109.0f);
                    CentreOnIcons(trophyH);
                    ImGui::Image(cup, ImVec2(trophyW, trophyH));
                    ImGui::SameLine(0, 12);
                }
            }

            if (guild.rank > 0)
            {
                CentreOnIcons(lineH);
                ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
                ImGui::Text("#%d", guild.rank);
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 6);
            }

            CentreOnIcons(lineH * kNameScale);
            ImGui::SetWindowFontScale(kNameScale);
            if (DrawFilterLink(guild.display.c_str(), isWinner ? kColorAccent : kColorText))
                FilterBySearchTerm(guild.display);
            const bool guildHovered = ImGui::IsItemHovered();
            ImGui::SetWindowFontScale(1.0f);

            if (bold) ImGui::PopFont();
            ImGui::PopClipRect();

            if (guildHovered)
                ImGui::SetTooltip("Show every match for %s", guild.display.c_str());
        }

        if (showStats)
        {
            const int firstStatCol = 3 + (showSkills ? 1 : 0) + 1;
            for (int si = 0; si < numStats; si++)
            {
                ImGui::TableSetColumnIndex(firstStatCol + si);
                ImTextureID ico = GetStatIcon(statCols[si].iconBase);
                if (ico)
                {
                    float padX = (statCols[si].w - statIconSz) * 0.5f;
                    if (padX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
                    ImGui::Image(ico, ImVec2(statIconSz, statIconSz));
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
                    ImGui::TextUnformatted(statCols[si].hdr);
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(statCols[si].tooltip);
                    ImGui::EndTooltip();
                }
            }
        }

        // Summary (totals) row — bold
        if (showStats)
        {
            ImFont* bold = GuiGlobalConstants::boldFont;
            if (bold) ImGui::PushFont(bold);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            if (showSkills) ImGui::TableNextColumn();
            ImGui::TableNextColumn(); // Copy

            for (int si = 0; si < numStats; si++)
            {
                ImGui::TableNextColumn();
                char buf[16];
                if (!totalsAvailable[si]) snprintf(buf, sizeof(buf), "-");
                else snprintf(buf, sizeof(buf), "%d", totals[si]);
                float tw = ImGui::CalcTextSize(buf).x;
                float padX = (statCols[si].w - tw) * 0.5f;
                if (padX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
                ImGui::TextUnformatted(buf);
            }

            if (bold) ImGui::PopFont();
        }

        // Player rows
        for (const auto* pp : sorted)
        {
            const auto& p = *pp;
            ImGui::TableNextRow();

            float rowH = std::max(skillIconSize, std::max(iconSize, ImGui::GetTextLineHeight()));
            float profPad = (rowH - iconSize) * 0.5f;
            float textPad = (rowH - ImGui::GetTextLineHeight()) * 0.5f;

            ImGui::TableNextColumn();
            if (profPad > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + profPad);
            ImTextureID priIcon = GetProfessionIcon(p.primary);
            if (priIcon) ImGui::Image(priIcon, ImVec2(iconSize, iconSize));
            else         ImGui::Dummy(ImVec2(iconSize, iconSize));

            ImGui::TableNextColumn();
            if (profPad > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + profPad);
            ImTextureID secIcon = GetProfessionIcon(p.secondary);
            if (secIcon) ImGui::Image(secIcon, ImVec2(iconSize, iconSize));
            else         ImGui::Dummy(ImVec2(iconSize, iconSize));

            ImGui::TableNextColumn();
            std::string cleanName = p.encoded_name.empty() ? "(unnamed)" : SanitizePlayerName(p.encoded_name);
            if (textPad > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textPad);
            if (DrawFilterLink(cleanName.c_str(), kColorText))
            {
                if (ImGui::GetIO().KeyCtrl) AddSearchTerm(cleanName);
                else                        FilterBySearchTerm(cleanName);
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
                ImGui::Text("%s / %s", GetProfessionName(p.primary), GetProfessionName(p.secondary));
                if (!p.skill_template_code.empty())
                    ImGui::Text("Template: %s", p.skill_template_code.c_str());
                ImGui::PopStyleColor();
                ImGui::TextColored(kColorTextDim, "Click to show every match with this player");
                ImGui::TextColored(kColorTextDim, "Ctrl+click to add them to the current filter");
                ImGui::EndTooltip();
            }

            if (showSkills)
            {
                ImGui::TableNextColumn();
                if (!p.used_skills.empty())
                {
                    auto sortedSkills = matchView.SortSkillsForDisplay(
                        p.used_skills, p.primary, p.secondary);
                    for (int ski = 0; ski < (int)sortedSkills.size(); ski++)
                    {
                        if (ski > 0) ImGui::SameLine(0, 2);
                        int skillId = sortedSkills[ski];
                        ImTextureID skillTex = GetSkillIcon(skillId);
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
                            ImGui::Dummy(ImVec2(skillIconSize, skillIconSize));
                    }
                }
            }

            // Copy template button column
            ImGui::TableNextColumn();
            if (!p.used_skills.empty() || !p.skill_template_code.empty())
            {
                std::string tmplKey = partyId + "_" + std::to_string(p.player_number);

                float now = (float)ImGui::GetTime();
                auto feedbackIt = s_copyFeedbackTimes.find(tmplKey);
                bool showFeedback = (feedbackIt != s_copyFeedbackTimes.end())
                                 && (now - feedbackIt->second < 1.5f);

                float btnSize = std::round(skillIconSize * 0.45f);
                float rowH = std::max(skillIconSize, std::max(sz.profIcon, ImGui::GetTextLineHeight()));
                float vertOff = (rowH - btnSize) * 0.5f;

                ImVec2 startCursor = ImGui::GetCursorScreenPos();
                ImVec2 cursor = ImVec2(startCursor.x, startCursor.y + vertOff);

                std::string btnId = "###cpytmpl_" + tmplKey;
                ImGui::SetCursorScreenPos(cursor);
                if (ImGui::InvisibleButton(btnId.c_str(), ImVec2(btnSize, btnSize)))
                {
                    std::string code = p.skill_template_code;
                    if (code.empty())
                        code = EncodeSkillTemplate(p.primary, p.secondary, p.used_skills);
                    std::string chatLink = "[" + cleanName + "-Skills;" + code + "]";
                    ImGui::SetClipboardText(chatLink.c_str());
                    s_copyFeedbackTimes[tmplKey] = now;
                }
                bool hovered = ImGui::IsItemHovered();

                ImDrawList* dl = ImGui::GetWindowDrawList();

                float feedbackAge = showFeedback ? (now - feedbackIt->second) : 99.0f;
                float goldBlend = 0.0f;
                if (showFeedback)
                    goldBlend = (feedbackAge < 0.3f)
                        ? 1.0f
                        : std::max(0.0f, 1.0f - (feedbackAge - 0.3f) / 1.2f);

                auto lerpCol = [](ImU32 a, ImU32 b, float t) -> ImU32 {
                    float r = ((a >> 0) & 0xFF) * (1 - t) + ((b >> 0) & 0xFF) * t;
                    float g = ((a >> 8) & 0xFF) * (1 - t) + ((b >> 8) & 0xFF) * t;
                    float bl = ((a >> 16) & 0xFF) * (1 - t) + ((b >> 16) & 0xFF) * t;
                    float al = ((a >> 24) & 0xFF) * (1 - t) + ((b >> 24) & 0xFF) * t;
                    return IM_COL32((int)r, (int)g, (int)bl, (int)al);
                };

                ImU32 baseCol = hovered
                    ? IM_COL32(190, 190, 190, 230)
                    : IM_COL32(130, 130, 130, 160);
                ImU32 goldCol = IM_COL32(225, 190, 80, 255);
                ImU32 iconCol = lerpCol(baseCol, goldCol, goldBlend);

                float m = btnSize * 0.12f;
                float cx = cursor.x + m;
                float cy = cursor.y + m;
                float cw = btnSize - 2 * m;
                float ch = btnSize - 2 * m;
                float t = std::max(1.0f, btnSize * 0.08f);

                if (showFeedback && feedbackAge < 0.4f)
                {
                    float mx = cursor.x + btnSize * 0.5f;
                    float my = cursor.y + btnSize * 0.5f;
                    float s = btnSize * 0.22f;
                    dl->AddLine(ImVec2(mx - s, my), ImVec2(mx - s * 0.3f, my + s * 0.7f), iconCol, t * 1.5f);
                    dl->AddLine(ImVec2(mx - s * 0.3f, my + s * 0.7f), ImVec2(mx + s, my - s * 0.5f), iconCol, t * 1.5f);
                }
                else
                {
                    float off = cw * 0.2f;
                    float rw = cw - off;
                    float rh = ch - off;
                    float r = 1.5f;
                    dl->AddRect(ImVec2(cx + off, cy), ImVec2(cx + off + rw, cy + rh), iconCol, r, 0, t);
                    dl->AddRectFilled(ImVec2(cx, cy + off), ImVec2(cx + rw, cy + off + rh), IM_COL32(30, 30, 30, 200), r);
                    dl->AddRect(ImVec2(cx, cy + off), ImVec2(cx + rw, cy + off + rh), iconCol, r, 0, t);
                }

                if (hovered)
                {
                    ImGui::BeginTooltip();
                    ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
                    ImGui::TextUnformatted(showFeedback ? "Copied!" : "Copy template code");
                    ImGui::PopStyleColor();
                    ImGui::EndTooltip();
                }
            }

            if (showStats)
            {
                for (int si = 0; si < numStats; si++)
                {
                    const int value = StatOf(p, si);

                    ImGui::TableNextColumn();
                    if (textPad > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textPad);
                    char buf[16];
                    if (!StatAvailable(p, si)) snprintf(buf, sizeof(buf), "-");
                    else snprintf(buf, sizeof(buf), "%d", value);
                    float tw = ImGui::CalcTextSize(buf).x;
                    float padX = (statCols[si].w - tw) * 0.5f;
                    if (padX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
                    ImGui::TextUnformatted(buf);

                    // A bare number says little on its own. Placing it against the rest of
                    // the team is the reading you actually want, and it is only computed
                    // for the one cell under the cursor.
                    if (ImGui::IsItemHovered())
                    {
                        int better = 0, equal = 0;
                        for (const auto* q : sorted)
                        {
                            const int v = StatOf(*q, si);
                            if (v > value) better++;
                            else if (v == value) equal++;
                        }
                        const int rank = better + 1;
                        const int teamTotal = totals[si];

                        ImFont* tipBold = GuiGlobalConstants::boldFont;
                        ImGui::BeginTooltip();
                        if (tipBold) ImGui::PushFont(tipBold);
                        ImGui::TextUnformatted(statCols[si].tooltip);
                        if (tipBold) ImGui::PopFont();
                        ImGui::Separator();
                        ImGui::Text("%d", value);
                        if (teamTotal > 0)
                            ImGui::TextColored(kColorTextDim, "%.0f%% of team (%d)",
                                               value * 100.0f / teamTotal, teamTotal);
                        // Ties share a place, so say so rather than implying an order the
                        // numbers do not support.
                        if (equal > 1)
                            ImGui::TextColored(kColorTextDim, "joint #%d of %d",
                                               rank, (int)sorted.size());
                        else
                            ImGui::TextColored(kColorTextDim, "#%d of %d",
                                               rank, (int)sorted.size());
                        ImGui::EndTooltip();
                    }
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleColor(); // kColorStatText
    ImGui::PopStyleVar();

    ImGui::EndGroup();
}

// ─── Bottom match detail panel ───────────────────────────────────────────────

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

    float contentMaxX = ImGui::GetWindowContentRegionMax().x + ImGui::GetWindowPos().x;
    float contentW = contentMaxX - (ImGui::GetWindowContentRegionMin().x + ImGui::GetWindowPos().x);
    float lineY = ImGui::GetCursorScreenPos().y - ImGui::GetTextLineHeightWithSpacing();
    bool narrowHeader = (contentW < 320.0f);

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
    }
    ImGui::Separator();
    ImGui::Spacing();

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

        ImGui::Spacing();

        ImFont* bold = GuiGlobalConstants::boldFont;

        // The metadata lines are narrow, so the two capes hang in the space beside them
        // rather than taking a row of their own. Their top is remembered here and the
        // cursor is put back below the taller of the two blocks afterwards.
        const float metaTopY = ImGui::GetCursorPosY();

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

        // ── Guild capes, in the gap to the right of the metadata ──
        {
            const float metaBottomY = ImGui::GetCursorPosY();

            ImTextureID cape1 = GetGuildCape(m, g1);
            ImTextureID cape2 = GetGuildCape(m, g2);
            if (cape1 || cape2)
            {
                // Banners are composed 128x256, so height is twice width. Sized to the
                // metadata block it sits beside, and clamped so it neither vanishes on a
                // narrow panel nor outgrows the map image above it.
                const float gap = 6.0f;
                float capeH = std::clamp(metaBottomY - metaTopY, 44.0f, 96.0f);
                float capeW = capeH * 0.5f;
                float needed = capeW * 2.0f + gap;

                // Only worth doing if the capes fit without crowding the text.
                if (needed < mapAreaW * 0.75f)
                {
                    float x = mapAreaW - needed;
                    auto drawCape = [&](ImTextureID tex, const GuildLabel& label, float cx) {
                        if (!tex) return;
                        ImGui::SetCursorPos(ImVec2(cx, metaTopY));
                        ImGui::Image(tex, ImVec2(capeW, capeH));
                        if (ImGui::IsItemHovered() && !label.display.empty())
                            ImGui::SetTooltip("%s", label.display.c_str());
                    };
                    drawCape(cape1, g1, x);
                    drawCape(cape2, g2, x + capeW + gap);

                    ImGui::SetCursorPosY(std::max(metaBottomY, metaTopY + capeH));
                }
            }
        }

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

                std::string labelRed  = g1.tag.empty() ? "Team 1" : "[" + g1.tag + "]";
                std::string labelBlue = g2.tag.empty() ? "Team 2" : "[" + g2.tag + "]";

                float labelW1 = ImGui::CalcTextSize(labelRed.c_str()).x;
                float labelW2 = ImGui::CalcTextSize(labelBlue.c_str()).x;
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

                DrawDmgRow(labelRed, ld.total_lord_damage_blue, valBuf1, colRed);
                DrawDmgRow(labelBlue, ld.total_lord_damage_red, valBuf2, colBlue);
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
        DrawTeamComposition(m, "1", g1, m.winner_party_id == 1, sz);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        DrawTeamComposition(m, "2", g2, m.winner_party_id == 2, sz);
        ImGui::EndGroup();
    }
    else
    {
        float teamW = (teamsW - sp) * 0.5f;

        ImGui::BeginChild("##team1_detail", ImVec2(teamW, 0));
        DrawTeamComposition(m, "1", g1, m.winner_party_id == 1, sz);
        ImGui::EndChild();

        ImGui::SameLine(0, sp);

        ImGui::BeginChild("##team2_detail", ImVec2(teamW, 0));
        DrawTeamComposition(m, "2", g2, m.winner_party_id == 2, sz);
        ImGui::EndChild();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// ─── Notification bar for new matches ─────────────────────────────────────────

static void DrawNotificationBar()
{
    if (s_state.notifyDismissed || s_state.notifyStartTime < 0.f || s_state.notifyNewCount <= 0)
        return;

    float elapsed = (float)ImGui::GetTime() - s_state.notifyStartTime;
    constexpr float kAutoDissmissTime = 4.f;
    constexpr float kFadeDuration = 0.3f;

    if (elapsed > kAutoDissmissTime + kFadeDuration)
    {
        s_state.notifyNewCount = 0;
        return;
    }

    float alpha = 1.f;
    if (elapsed > kAutoDissmissTime)
        alpha = 1.f - (elapsed - kAutoDissmissTime) / kFadeDuration;

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

    float barH = 28.f;
    float availW = ImGui::GetContentRegionAvail().x;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();

    dl->AddRectFilled(p, ImVec2(p.x + availW, p.y + barH),
        IM_COL32(212, 160, 32, (int)(30 * alpha)));
    dl->AddLine(p, ImVec2(p.x, p.y + barH),
        IM_COL32(212, 160, 32, (int)(255 * alpha)), 2.f);

    ImGui::SetCursorScreenPos(ImVec2(p.x + 10.f, p.y + (barH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 0.85f * alpha));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();

    // Dismiss button
    float btnW = ImGui::CalcTextSize("X").x + 12.f;
    ImGui::SetCursorScreenPos(ImVec2(p.x + availW - btnW - 4.f, p.y + (barH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 0.5f * alpha));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.1f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.2f));
    if (ImGui::SmallButton("X##notif_dismiss"))
        s_state.notifyDismissed = true;
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + barH + 2.f));
}

// ─── Main entry point ────────────────────────────────────────────────────────

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
        return;

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

    ImGui::SetNextWindowPos(ImVec2(0, GuiGlobalConstants::menu_bar_height), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(totalW, displaySize.y - GuiGlobalConstants::menu_bar_height), ImGuiCond_Always);

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

    DrawNotificationBar();

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
