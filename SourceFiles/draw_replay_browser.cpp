#include "pch.h"
#include "draw_replay_browser.h"
#include "ReplayLibrary.h"
#include "GuiGlobalConstants.h"
#include "TextureCache.h"
#include "SkillDatabase.h"
#include <algorithm>
#include <set>

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
};

static GuildLabel GetPartyGuild(const MatchMeta& m, const std::string& partyId)
{
    GuildLabel result;
    auto pit = m.parties.find(partyId);
    if (pit == m.parties.end()) { result.display = "?"; return result; }

    std::map<int, int> guildCounts;
    for (const auto& p : pit->second.players)
        if (p.guild_id > 0) guildCounts[p.guild_id]++;

    int bestGuildId = 0, bestCount = 0;
    for (const auto& [gid, cnt] : guildCounts)
        if (cnt > bestCount) { bestGuildId = gid; bestCount = cnt; }

    if (bestGuildId == 0) { result.display = "Unknown"; return result; }

    auto guildIdStr = std::to_string(bestGuildId);
    auto git = m.guilds.find(guildIdStr);
    if (git != m.guilds.end())
    {
        result.name = git->second.name;
        result.tag = git->second.tag;
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

static void DrawSkillTooltip(int skillId)
{
    const SkillInfo* si = GetSkillDatabase().Get(skillId);
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
        {
            if (si->activation == (int)si->activation)
                DrawCostIcon("activation.png", "%.0f", si->activation, hasCost);
            else
                DrawCostIcon("activation.png", "%.1f", si->activation, hasCost);
        }

        if (si->recharge > 0)
        {
            if (si->recharge == (int)si->recharge)
                DrawCostIcon("recharge.png", "%.0f", si->recharge, hasCost);
            else
                DrawCostIcon("recharge.png", "%.1f", si->recharge, hasCost);
        }
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

static const ImVec4 kColorBg         = ImVec4(0.06f, 0.06f, 0.08f, 0.92f);
static const ImVec4 kColorPanel      = ImVec4(0.10f, 0.10f, 0.13f, 0.85f);
static const ImVec4 kColorPanelLight = ImVec4(0.14f, 0.14f, 0.18f, 0.80f);
static const ImVec4 kColorBorder     = ImVec4(0.30f, 0.28f, 0.22f, 0.60f);
static const ImVec4 kColorAccent     = ImVec4(0.77f, 0.64f, 0.35f, 1.00f);
static const ImVec4 kColorAccentDim  = ImVec4(0.55f, 0.46f, 0.25f, 0.70f);
static const ImVec4 kColorText       = ImVec4(0.88f, 0.86f, 0.78f, 1.00f);
static const ImVec4 kColorTextDim    = ImVec4(0.60f, 0.58f, 0.52f, 1.00f);
static const ImVec4 kColorSelected   = ImVec4(0.24f, 0.22f, 0.16f, 0.90f);
static const ImVec4 kColorHover      = ImVec4(0.20f, 0.18f, 0.14f, 0.70f);

static int PushGlassTheme()
{
    int count = 0;
    auto Push = [&](ImGuiCol idx, const ImVec4& col) { ImGui::PushStyleColor(idx, col); count++; };

    Push(ImGuiCol_WindowBg,           kColorBg);
    Push(ImGuiCol_ChildBg,            kColorPanel);
    Push(ImGuiCol_PopupBg,            ImVec4(0.08f, 0.08f, 0.10f, 0.95f));
    Push(ImGuiCol_Border,             kColorBorder);
    Push(ImGuiCol_FrameBg,            ImVec4(0.12f, 0.12f, 0.15f, 0.70f));
    Push(ImGuiCol_FrameBgHovered,     ImVec4(0.18f, 0.17f, 0.14f, 0.70f));
    Push(ImGuiCol_FrameBgActive,      ImVec4(0.22f, 0.20f, 0.16f, 0.80f));
    Push(ImGuiCol_TitleBg,            kColorBg);
    Push(ImGuiCol_TitleBgActive,      ImVec4(0.10f, 0.10f, 0.12f, 0.95f));
    Push(ImGuiCol_ScrollbarBg,        ImVec4(0.06f, 0.06f, 0.08f, 0.50f));
    Push(ImGuiCol_ScrollbarGrab,      ImVec4(0.28f, 0.26f, 0.22f, 0.60f));
    Push(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.38f, 0.34f, 0.28f, 0.70f));
    Push(ImGuiCol_ScrollbarGrabActive,  kColorAccentDim);
    Push(ImGuiCol_Header,             kColorSelected);
    Push(ImGuiCol_HeaderHovered,      kColorHover);
    Push(ImGuiCol_HeaderActive,       kColorSelected);
    Push(ImGuiCol_Button,             ImVec4(0.16f, 0.15f, 0.13f, 0.80f));
    Push(ImGuiCol_ButtonHovered,      ImVec4(0.28f, 0.25f, 0.20f, 0.80f));
    Push(ImGuiCol_ButtonActive,       kColorAccentDim);
    Push(ImGuiCol_Separator,          kColorBorder);
    Push(ImGuiCol_Text,               kColorText);
    Push(ImGuiCol_TextDisabled,       kColorTextDim);
    Push(ImGuiCol_TableHeaderBg,      ImVec4(0.12f, 0.11f, 0.10f, 0.90f));
    Push(ImGuiCol_TableBorderStrong,  kColorBorder);
    Push(ImGuiCol_TableBorderLight,   ImVec4(0.22f, 0.20f, 0.18f, 0.40f));
    Push(ImGuiCol_TableRowBg,         ImVec4(0.00f, 0.00f, 0.00f, 0.00f));
    Push(ImGuiCol_TableRowBgAlt,      ImVec4(0.10f, 0.10f, 0.10f, 0.20f));

    return count;
}

static void PopGlassTheme(int count)
{
    ImGui::PopStyleColor(count);
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
    case LayoutMode::Full:    return { 22.0f, 30.0f, 14.0f, 140.0f, 8.0f };
    case LayoutMode::Compact: return { 20.0f, 24.0f, 13.0f, 120.0f, 8.0f };
    case LayoutMode::Narrow:  return { 18.0f, 22.0f, 12.0f, 100.0f, 8.0f };
    case LayoutMode::Mobile:  return { 18.0f, 20.0f, 12.0f, 80.0f,  8.0f };
    }
    return { 22.0f, 30.0f, 14.0f, 140.0f, 8.0f };
}

// ─── Filter state ────────────────────────────────────────────────────────────

struct BrowserState
{
    char searchBuf[256] = "";
    int  selectedMapFilter = 0;
    int  selectedGuildFilter = 0;
    int  selectedFluxFilter = 0;
    int  selectedOccasionFilter = 0;

    std::vector<std::string> mapNames;
    std::vector<std::string> guildNames;
    std::vector<std::string> fluxNames;
    std::vector<std::string> occasionNames;

    int  selectedMatchIndex = -1;
    int  sortColumn = 0;
    bool sortAscending = false;

    bool filtersBuilt = false;
    int  lastMatchCount = -1;

    // Responsive layout state
    LayoutMode layout = LayoutMode::Full;
    LayoutMode prevLayout = LayoutMode::Full;
    bool sidebarExpanded = true;
    bool mobileShowDetail = false;
};

static BrowserState s_state;

static std::string ToLower(const std::string& s)
{
    std::string r = s;
    for (auto& c : r) c = (char)tolower((unsigned char)c);
    return r;
}

static void BuildFilterLists(const std::vector<MatchMeta>& matches)
{
    if (s_state.filtersBuilt && s_state.lastMatchCount == (int)matches.size())
        return;

    std::set<std::string> maps, guilds, fluxes, occasions;
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
                guilds.insert(g.name + " [" + g.tag + "]");
        }
    }

    auto ToVec = [](const std::set<std::string>& s) {
        std::vector<std::string> v;
        v.push_back("All");
        for (const auto& e : s) v.push_back(e);
        return v;
    };

    s_state.mapNames = ToVec(maps);
    s_state.guildNames = ToVec(guilds);
    s_state.fluxNames = ToVec(fluxes);
    s_state.occasionNames = ToVec(occasions);
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

// ─── Match filtering ─────────────────────────────────────────────────────────

struct FilteredMatch
{
    int originalIndex;
    const MatchMeta* meta;
    GuildLabel guild1;
    GuildLabel guild2;
    std::string mapName;
};

static std::vector<FilteredMatch> FilterMatches(const std::vector<MatchMeta>& matches)
{
    std::vector<FilteredMatch> result;
    result.reserve(matches.size());

    std::string searchLower = ToLower(std::string(s_state.searchBuf));

    for (int i = 0; i < (int)matches.size(); i++)
    {
        const auto& m = matches[i];

        // Map filter
        const char* mn = GetMapName(m.map_id);
        std::string mapName = mn ? mn : ("Map " + std::to_string(m.map_id));

        if (s_state.selectedMapFilter > 0 && s_state.selectedMapFilter < (int)s_state.mapNames.size())
        {
            if (mapName != s_state.mapNames[s_state.selectedMapFilter])
                continue;
        }

        // Occasion filter
        if (s_state.selectedOccasionFilter > 0 && s_state.selectedOccasionFilter < (int)s_state.occasionNames.size())
        {
            if (m.occasion != s_state.occasionNames[s_state.selectedOccasionFilter])
                continue;
        }

        // Flux filter
        if (s_state.selectedFluxFilter > 0 && s_state.selectedFluxFilter < (int)s_state.fluxNames.size())
        {
            if (m.flux != s_state.fluxNames[s_state.selectedFluxFilter])
                continue;
        }

        GuildLabel g1 = GetPartyGuild(m, "1");
        GuildLabel g2 = GetPartyGuild(m, "2");

        // Guild filter
        if (s_state.selectedGuildFilter > 0 && s_state.selectedGuildFilter < (int)s_state.guildNames.size())
        {
            const auto& gf = s_state.guildNames[s_state.selectedGuildFilter];
            if (g1.display != gf && g2.display != gf)
                continue;
        }

        // Search filter (player name, guild name, tag)
        if (!searchLower.empty())
        {
            bool found = false;
            found |= ToLower(g1.name).find(searchLower) != std::string::npos;
            found |= ToLower(g1.tag).find(searchLower) != std::string::npos;
            found |= ToLower(g2.name).find(searchLower) != std::string::npos;
            found |= ToLower(g2.tag).find(searchLower) != std::string::npos;

            for (const auto& [pid, party] : m.parties)
            {
                for (const auto& p : party.players)
                {
                    if (ToLower(p.encoded_name).find(searchLower) != std::string::npos)
                    {
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
            if (!found) continue;
        }

        FilteredMatch fm;
        fm.originalIndex = i;
        fm.meta = &m;
        fm.guild1 = g1;
        fm.guild2 = g2;
        fm.mapName = mapName;
        result.push_back(fm);
    }

    // Sort
    if (!result.empty())
    {
        auto cmp = [](const FilteredMatch& a, const FilteredMatch& b, int col, bool asc) -> bool {
            int r = 0;
            switch (col)
            {
            case 0: // Date — tiebreak by folder name (contains timestamp)
            {
                if (a.meta->year != b.meta->year)       r = a.meta->year - b.meta->year;
                else if (a.meta->month != b.meta->month) r = a.meta->month - b.meta->month;
                else if (a.meta->day != b.meta->day)     r = a.meta->day - b.meta->day;
                else r = a.meta->folder_name.compare(b.meta->folder_name);
                break;
            }
            case 1: r = a.mapName.compare(b.mapName); break;
            case 2: r = a.meta->occasion.compare(b.meta->occasion); break;
            case 3: r = a.guild1.display.compare(b.guild1.display); break;
            case 4: r = a.guild2.display.compare(b.guild2.display); break;
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

// ─── Left filter panel ──────────────────────────────────────────────────────

static int CountActiveFilters()
{
    int n = 0;
    if (s_state.searchBuf[0] != '\0') n++;
    if (s_state.selectedMapFilter > 0) n++;
    if (s_state.selectedGuildFilter > 0) n++;
    if (s_state.selectedFluxFilter > 0) n++;
    if (s_state.selectedOccasionFilter > 0) n++;
    return n;
}

static void DrawFilterPanelCollapsed(const std::vector<MatchMeta>& matches)
{
    const float sp = 8.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp, sp));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);

    ImGui::BeginChild("##filter_collapsed", ImVec2(40, 0), ImGuiChildFlags_Border);

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

static void DrawFilterPanelExpanded(const std::vector<MatchMeta>& matches)
{
    const float sp = 8.0f;
    const float minW = 220.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp + 4, sp + 4));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);

    float filterW = std::max(minW, ImGui::GetContentRegionAvail().x * 0.15f);
    ImGui::BeginChild("##filter_panel", ImVec2(filterW, 0), ImGuiChildFlags_Border);

    // Header with collapse toggle
    if (ImGui::Button("<", ImVec2(24, 24)))
        s_state.sidebarExpanded = false;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::TextUnformatted("FILTERS");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("Search");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##search", "Player, guild, tag...", s_state.searchBuf, sizeof(s_state.searchBuf));
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextUnformatted("Map");
    ImGui::SetNextItemWidth(-1);
    ComboFromVec("##map_filter", s_state.selectedMapFilter, s_state.mapNames);
    ImGui::Spacing();

    ImGui::TextUnformatted("Guild");
    ImGui::SetNextItemWidth(-1);
    ComboFromVec("##guild_filter", s_state.selectedGuildFilter, s_state.guildNames);
    ImGui::Spacing();

    ImGui::TextUnformatted("Flux");
    ImGui::SetNextItemWidth(-1);
    ComboFromVec("##flux_filter", s_state.selectedFluxFilter, s_state.fluxNames);
    ImGui::Spacing();

    ImGui::TextUnformatted("Occasion");
    ImGui::SetNextItemWidth(-1);
    ComboFromVec("##occasion_filter", s_state.selectedOccasionFilter, s_state.occasionNames);
    ImGui::Spacing();
    ImGui::Spacing();

    if (ImGui::Button("Clear Filters", ImVec2(-1, 0)))
    {
        s_state.searchBuf[0] = '\0';
        s_state.selectedMapFilter = 0;
        s_state.selectedGuildFilter = 0;
        s_state.selectedFluxFilter = 0;
        s_state.selectedOccasionFilter = 0;
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

static void DrawFilterPanel(const std::vector<MatchMeta>& matches)
{
    if (s_state.sidebarExpanded)
        DrawFilterPanelExpanded(matches);
    else
        DrawFilterPanelCollapsed(matches);
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

// ─── Match list table ────────────────────────────────────────────────────────

static void DrawMatchListTable(const std::vector<FilteredMatch>& filtered,
                               const std::vector<MatchMeta>& allMatches)
{
    const auto mode = s_state.layout;
    const auto sz = GetSizes(mode);
    const float sp = sz.spacing;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kColorPanel);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);

    float panelTotalH = ImGui::GetContentRegionAvail().y;
    bool hasDetail = (s_state.selectedMatchIndex >= 0) && (mode != LayoutMode::Mobile);
    float detailHeight = hasDetail ? std::max(200.0f, panelTotalH * 0.45f) : 0.0f;
    float listHeight = -detailHeight;
    if (detailHeight > 0) listHeight -= sp;

    ImGui::BeginChild("##match_list_area", ImVec2(0, listHeight), ImGuiChildFlags_Border);

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

    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::Text("MATCHES  (%d)", (int)filtered.size());
    ImGui::PopStyleColor();
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
    float dateW = 82.0f;
    float occasionW = (mode == LayoutMode::Narrow) ? 100.0f : 130.0f;
    float mapW = std::max(90.0f, tableW * 0.13f);
    float teamW = std::max(100.0f, (tableW - dateW - mapW - occasionW) * 0.5f);

    ImGuiTableFlags tableFlags =
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable |
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;

    if (ImGui::BeginTable("##match_table", 5, tableFlags))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Date",     ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, dateW);
        ImGui::TableSetupColumn("Map",      ImGuiTableColumnFlags_WidthFixed, mapW);
        ImGui::TableSetupColumn("Occasion", ImGuiTableColumnFlags_WidthFixed, occasionW);
        ImGui::TableSetupColumn("Team 1",   ImGuiTableColumnFlags_WidthFixed, teamW);
        ImGui::TableSetupColumn("Team 2",   ImGuiTableColumnFlags_WidthFixed, teamW);
        ImGui::TableHeadersRow();

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

        ImGuiListClipper clipper;
        clipper.Begin((int)filtered.size());
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
            {
                const auto& fm = filtered[row];
                const auto& m = *fm.meta;
                bool isSelected = (s_state.selectedMatchIndex == fm.originalIndex);

                ImGui::TableNextRow();
                ImGui::PushID(fm.originalIndex);

                ImGui::TableNextColumn();
                char dateBuf[16];
                snprintf(dateBuf, sizeof(dateBuf), "%04d/%02d/%02d", m.year, m.month, m.day);
                if (ImGui::Selectable(dateBuf, isSelected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    s_state.selectedMatchIndex = isSelected ? -1 : fm.originalIndex;
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(fm.mapName.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.occasion.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(fm.guild1.display.c_str());
                if (m.winner_party_id == 1 && cupTex)
                {
                    ImGui::SameLine();
                    ImGui::Image(cupTex, ImVec2(sz.cupIcon, sz.cupIcon));
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(fm.guild2.display.c_str());
                if (m.winner_party_id == 2 && cupTex)
                {
                    ImGui::SameLine();
                    ImGui::Image(cupTex, ImVec2(sz.cupIcon, sz.cupIcon));
                }

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// ─── Team composition panel ──────────────────────────────────────────────────

static void DrawTeamComposition(const MatchMeta& m, const std::string& partyId,
                                const GuildLabel& guild, bool isWinner,
                                const ResponsiveSizes& sz)
{
    const float iconSize = sz.profIcon;
    const float skillIconSize = sz.skillIcon;
    const float smallIconSize = sz.cupIcon + 2.0f;

    ImGui::BeginGroup();

    ImGui::PushStyleColor(ImGuiCol_Text, isWinner ? kColorAccent : kColorText);
    ImGui::TextUnformatted(guild.display.c_str());
    ImGui::PopStyleColor();

    if (isWinner)
    {
        ImGui::SameLine();
        ImTextureID cup = GetCupIcon();
        if (cup) ImGui::Image(cup, ImVec2(smallIconSize, smallIconSize));
    }

    ImGui::Spacing();

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

    const float availW = ImGui::GetContentRegionAvail().x;
    const float nameColW = std::max(100.0f, availW * 0.22f);
    const float kdColW = 44.0f;
    bool showSkills = (s_state.layout != LayoutMode::Mobile);
    int numCols = showSkills ? 5 : 4;

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(2, 2));
    if (ImGui::BeginTable(("##team_" + partyId).c_str(), numCols,
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX))
    {
        ImGui::TableSetupColumn("Pri",  ImGuiTableColumnFlags_WidthFixed, iconSize + 1);
        ImGui::TableSetupColumn("Sec",  ImGuiTableColumnFlags_WidthFixed, iconSize + 3);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, nameColW);
        if (showSkills)
            ImGui::TableSetupColumn("Skills", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("K/D",  ImGuiTableColumnFlags_WidthFixed, kdColW);

        for (const auto* pp : sorted)
        {
            const auto& p = *pp;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            ImTextureID priIcon = GetProfessionIcon(p.primary);
            if (priIcon) ImGui::Image(priIcon, ImVec2(iconSize, iconSize));
            else         ImGui::Dummy(ImVec2(iconSize, iconSize));

            ImGui::TableNextColumn();
            ImTextureID secIcon = GetProfessionIcon(p.secondary);
            if (secIcon) ImGui::Image(secIcon, ImVec2(iconSize, iconSize));
            else         ImGui::Dummy(ImVec2(iconSize, iconSize));

            ImGui::TableNextColumn();
            const char* name = p.encoded_name.empty() ? "(unnamed)" : p.encoded_name.c_str();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
            ImGui::TextUnformatted(name);

            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("%s / %s", GetProfessionName(p.primary), GetProfessionName(p.secondary));
                ImGui::Text("Damage: %d | Kills: %d | Deaths: %d", p.total_damage, p.kills, p.deaths);
                if (!p.skill_template_code.empty())
                    ImGui::Text("Template: %s", p.skill_template_code.c_str());
                ImGui::EndTooltip();
            }

            if (showSkills)
            {
                ImGui::TableNextColumn();
                if (!p.used_skills.empty())
                {
                    for (int ski = 0; ski < (int)p.used_skills.size(); ski++)
                    {
                        if (ski > 0) ImGui::SameLine(0, 2);
                        int skillId = p.used_skills[ski];
                        ImTextureID skillTex = GetSkillIcon(skillId);
                        if (skillTex)
                        {
                            ImGui::Image(skillTex, ImVec2(skillIconSize, skillIconSize));
                            if (ImGui::IsItemHovered())
                                DrawSkillTooltip(skillId);
                        }
                        else
                            ImGui::Dummy(ImVec2(skillIconSize, skillIconSize));
                    }
                }
            }

            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
            ImGui::Text("%d/%d", p.kills, p.deaths);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    ImGui::EndGroup();
}

// ─── Bottom match detail panel ───────────────────────────────────────────────

static void DrawMatchDetailPanel(const MatchMeta& m, bool fillRemaining = false)
{
    const auto mode = s_state.layout;
    const auto sz = GetSizes(mode);
    const float sp = sz.spacing;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp + 4, sp + 2));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.10f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);

    ImVec2 detailSize = fillRemaining ? ImVec2(0, 0) : ImVec2(0, 0);
    ImGui::BeginChild("##match_detail", detailSize, ImGuiChildFlags_Border);

    // Header
    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::TextUnformatted("MATCH DETAILS");
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
    if (ImGui::SmallButton("X"))
    {
        s_state.selectedMatchIndex = -1;
        s_state.mobileShowDetail = false;
    }
    ImGui::Separator();
    ImGui::Spacing();

    GuildLabel g1 = GetPartyGuild(m, "1");
    GuildLabel g2 = GetPartyGuild(m, "2");

    float availWidth = ImGui::GetContentRegionAvail().x;

    // Vertical stacking for narrow/mobile, horizontal for wider
    bool stackVertical = (availWidth < 700.0f);

    if (stackVertical)
    {
        // Map info at top
        float mapImgSize = std::min(sz.mapImg, availWidth * 0.3f);
        ImTextureID mapIcon = GetMapIcon(m.map_id);
        if (mapIcon)
        {
            ImGui::Image(mapIcon, ImVec2(mapImgSize, mapImgSize));
            ImGui::SameLine(0, sp * 2);
        }

        ImGui::BeginGroup();
        {
            const char* mapName = GetMapName(m.map_id);
            char dateBuf[16];
            snprintf(dateBuf, sizeof(dateBuf), "%04d/%02d/%02d", m.year, m.month, m.day);
            ImGui::TextColored(kColorText, "%s", dateBuf);
            ImGui::TextColored(kColorAccent, "%s", mapName ? mapName : "Unknown Map");
            if (!m.occasion.empty())
                ImGui::TextColored(kColorTextDim, "%s", m.occasion.c_str());
            if (!m.match_duration.empty())
                ImGui::TextColored(kColorTextDim, "Duration: %s", m.match_duration.c_str());
            if (!m.flux.empty())
                ImGui::TextColored(kColorTextDim, "Flux: %s", m.flux.c_str());
        }
        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Teams stacked
        DrawTeamComposition(m, "1", g1, m.winner_party_id == 1, sz);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        DrawTeamComposition(m, "2", g2, m.winner_party_id == 2, sz);
    }
    else
    {
        // Horizontal: Map | Team1 | Team2
        float mapAreaWidth = std::max(140.0f, availWidth * 0.12f);
        float teamAreaWidth = (availWidth - mapAreaWidth - sp * 4) * 0.5f;

        ImGui::BeginGroup();
        {
            float mapImgSize = mapAreaWidth - sp * 2;
            ImTextureID mapIcon = GetMapIcon(m.map_id);
            if (mapIcon)
                ImGui::Image(mapIcon, ImVec2(mapImgSize, mapImgSize));
            else
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.18f, 0.80f));
                ImGui::BeginChild("##map_placeholder", ImVec2(mapImgSize, mapImgSize), ImGuiChildFlags_Border);
                ImGui::SetCursorPos(ImVec2(mapImgSize * 0.2f, mapImgSize * 0.43f));
                ImGui::TextColored(kColorTextDim, "No map");
                ImGui::EndChild();
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();

            const char* mapName = GetMapName(m.map_id);
            char dateBuf[16];
            snprintf(dateBuf, sizeof(dateBuf), "%04d/%02d/%02d", m.year, m.month, m.day);
            ImGui::TextColored(kColorText, "%s", dateBuf);
            ImGui::TextColored(kColorAccent, "%s", mapName ? mapName : "Unknown Map");
            if (!m.occasion.empty())
                ImGui::TextColored(kColorTextDim, "%s", m.occasion.c_str());
            if (!m.match_duration.empty())
                ImGui::TextColored(kColorTextDim, "Duration: %s", m.match_duration.c_str());
            if (!m.flux.empty())
                ImGui::TextColored(kColorTextDim, "Flux: %s", m.flux.c_str());
        }
        ImGui::EndGroup();

        ImGui::SameLine(0, sp * 2);

        ImGui::BeginChild("##team1_detail", ImVec2(teamAreaWidth, 0));
        DrawTeamComposition(m, "1", g1, m.winner_party_id == 1, sz);
        ImGui::EndChild();

        ImGui::SameLine(0, sp);

        ImGui::BeginChild("##team2_detail", ImVec2(teamAreaWidth, 0));
        DrawTeamComposition(m, "2", g2, m.winner_party_id == 2, sz);
        ImGui::EndChild();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// ─── Main entry point ────────────────────────────────────────────────────────

void draw_replay_browser(ReplayLibrary& library)
{
    if (!GuiGlobalConstants::is_replay_browser_open)
        return;

    if (!library.IsLoaded() || library.GetMatches().empty())
        return;

    const auto& matches = library.GetMatches();
    BuildFilterLists(matches);

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

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    if (!ImGui::Begin("##replay_browser", nullptr, flags))
    {
        ImGui::End();
        ImGui::PopStyleVar(3);
        PopGlassTheme(themeColors);
        return;
    }

    auto filtered = FilterMatches(matches);

    // Helper lambda: re-check selection validity (can change mid-frame via clicks)
    auto validSelection = [&]() {
        return s_state.selectedMatchIndex >= 0 && s_state.selectedMatchIndex < (int)matches.size();
    };

    // Mobile: detail replaces the list
    if (s_state.layout == LayoutMode::Mobile && s_state.mobileShowDetail && validSelection())
    {
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
        // Standard layout: sidebar + (list + detail)
        DrawFilterPanel(matches);
        ImGui::SameLine(0, sp);

        ImGui::BeginGroup();
        {
            DrawMatchListTable(filtered, matches);

            if (validSelection() && s_state.layout != LayoutMode::Mobile)
            {
                ImGui::Spacing();
                DrawMatchDetailPanel(matches[s_state.selectedMatchIndex]);
            }
        }
        ImGui::EndGroup();
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    PopGlassTheme(themeColors);
}
