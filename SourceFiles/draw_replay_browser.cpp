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
    int rank = 0;
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

static ImTextureID GetStatIcon(const char* filename)
{
    EnsureTextureBasePath();
    std::string path = g_textureBasePath + "\\Textures\\Others_UI\\" + filename;
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

static const ImVec4 kColorBg         = ImVec4(0.102f, 0.102f, 0.102f, 1.00f); // #1A1A1A
static const ImVec4 kColorPanel      = ImVec4(0.125f, 0.125f, 0.125f, 1.00f); // #202020
static const ImVec4 kColorPanelLight = ImVec4(0.157f, 0.157f, 0.157f, 1.00f); // #282828
static const ImVec4 kColorBorder     = ImVec4(0.165f, 0.165f, 0.165f, 1.00f); // #2A2A2A
static const ImVec4 kColorAccent     = ImVec4(0.784f, 0.608f, 0.235f, 1.00f); // #C89B3C
static const ImVec4 kColorAccentDim  = ImVec4(0.588f, 0.456f, 0.176f, 0.70f);
static const ImVec4 kColorText       = ImVec4(0.902f, 0.902f, 0.902f, 1.00f); // #E6E6E6
static const ImVec4 kColorTextDim    = ImVec4(0.600f, 0.600f, 0.600f, 1.00f);
static const ImVec4 kColorSelected   = ImVec4(0.200f, 0.180f, 0.140f, 0.90f);
static const ImVec4 kColorHover      = ImVec4(0.180f, 0.165f, 0.140f, 0.70f);

// Vertical splitter (drag left/right to resize columns). Returns true while dragging.
static bool VSplitter(const char* id, float height, float thickness = 6.0f)
{
    ImGui::SameLine(0, 0);
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(thickness, height));
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();

    ImU32 col = IM_COL32(42, 42, 42, 255);          // kColorBorder
    if (active)       col = IM_COL32(200, 155, 60, 255); // kColorAccent
    else if (hovered) col = IM_COL32(200, 155, 60, 140);

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

    ImU32 col = IM_COL32(42, 42, 42, 255);
    if (active)       col = IM_COL32(200, 155, 60, 255);
    else if (hovered) col = IM_COL32(200, 155, 60, 140);

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

// ─── Flux description lookup ─────────────────────────────────────────────────

static const char* GetFluxDescription(const std::string& fluxName)
{
    static const struct { const char* name; const char* desc; } kFluxTable[] = {
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

    for (const auto& entry : kFluxTable)
        if (fluxName == entry.name)
            return entry.desc;

    return nullptr;
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
    std::set<std::string> selectedMaps;
    std::set<std::string> selectedFluxes;
    std::set<std::string> selectedOccasions;

    // Auto-complete search buffers
    char mapSearchBuf[128] = "";
    char fluxSearchBuf[128] = "";
    char occasionSearchBuf[128] = "";

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

    bool filtersBuilt = false;
    int  lastMatchCount = -1;

    // Responsive layout state
    LayoutMode layout = LayoutMode::Full;
    LayoutMode prevLayout = LayoutMode::Full;
    bool sidebarExpanded = true;
    bool mobileShowDetail = false;
    bool statsExpanded = true;

    // User-resizable splitter state (pixels, <=0 means use default)
    float userFilterW = -1.0f;
    float userTopRowH = -1.0f;
};

static BrowserState s_state;

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

static void BuildFilterLists(const std::vector<MatchMeta>& matches)
{
    if (s_state.filtersBuilt && s_state.lastMatchCount == (int)matches.size())
        return;

    std::set<std::string> maps, guilds, fluxes, occasions;
    std::set<std::string> teams, players, tags;

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

static bool DrawMultiSelectFilter(
    const char* label, const char* hint,
    char* searchBuf, size_t searchBufSize,
    const std::vector<std::string>& allItems,
    std::set<std::string>& selectedItems,
    const char* id, bool useFuzzy)
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

            if (ImGui::Button(chipId.c_str()))
            {
                it = selectedItems.erase(it);
                changed = true;
            }
            else
                ++it;

            lineX += (lineX > 0.0f ? chipGap : 0.0f) + ImGui::GetItemRectSize().x;
            ImGui::PopStyleColor(3);
        }

        ImGui::PopStyleVar(2);

        ImGui::SameLine(0, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorHover);
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccentDim);
        if (ImGui::SmallButton((std::string("Clear##clr_") + id).c_str()))
        {
            selectedItems.clear();
            changed = true;
        }
        ImGui::PopStyleColor(3);
        ImGui::Spacing();
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint(
        (std::string("##ms_") + id).c_str(),
        hint, searchBuf, searchBufSize);

    if (searchBuf[0] != '\0')
    {
        ImVec2 inputMin = ImGui::GetItemRectMin();
        ImVec2 inputMax = ImGui::GetItemRectMax();

        std::string query(searchBuf);
        std::vector<const std::string*> suggestions;

        for (size_t i = 1; i < allItems.size(); i++)
        {
            const auto& item = allItems[i];
            if (selectedItems.count(item)) continue;
            bool match = useFuzzy
                ? FuzzyMatch(query, item)
                : (ToLower(item).find(ToLower(query)) != std::string::npos);
            if (match) suggestions.push_back(&item);
        }

        if (!suggestions.empty())
        {
            float dropW = inputMax.x - inputMin.x;
            float itemH = ImGui::GetTextLineHeightWithSpacing();
            float maxH = std::min(200.0f, (float)suggestions.size() * itemH + 8.0f);

            ImGui::SetNextWindowPos(ImVec2(inputMin.x, inputMax.y));
            ImGui::SetNextWindowSizeConstraints(ImVec2(dropW, 0), ImVec2(dropW, maxH));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.11f, 0.11f, 0.98f));
            ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);

            std::string ddWinId = std::string("##ddw_") + id;
            if (ImGui::Begin(ddWinId.c_str(), nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize))
            {
                for (size_t si = 0; si < suggestions.size(); si++)
                {
                    std::string selId = std::string(id) + "_" + std::to_string(si);
                    if (HighlightedSelectable(suggestions[si]->c_str(),
                            searchBuf, selId.c_str(), useFuzzy))
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
        }
    }

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

static void DrawDateRangeFilter()
{
    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::TextUnformatted("Date Range");
    ImGui::PopStyleColor();

    if (DateValValid(s_state.dateFrom) || DateValValid(s_state.dateTo))
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorHover);
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccentDim);
        if (ImGui::SmallButton("Reset##date_reset"))
        {
            s_state.dateFrom = {};
            s_state.dateTo = {};
        }
        ImGui::PopStyleColor(3);
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

    std::string searchLower = ToLower(std::string(s_state.searchDebounced));

    for (int i = 0; i < (int)matches.size(); i++)
    {
        const auto& m = matches[i];

        // Map filter (multi-select)
        const char* mn = GetMapName(m.map_id);
        std::string mapName = mn ? mn : ("Map " + std::to_string(m.map_id));

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

        GuildLabel g1 = GetPartyGuild(m, "1");
        GuildLabel g2 = GetPartyGuild(m, "2");

        // Search filter — chip-based (OR logic) or text-based fallback
        if (!s_state.selectedSearchTerms.empty())
        {
            bool anyMatch = false;
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
                    {
                        if (ToLower(p.encoded_name).find(tLow) != std::string::npos)
                        { found = true; break; }
                    }
                    if (found) break;
                }
                if (found) { anyMatch = true; break; }
            }
            if (!anyMatch) continue;
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
            case 1: r = a.meta->occasion.compare(b.meta->occasion); break;
            case 2: r = a.mapName.compare(b.mapName); break;
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
    if (!s_state.selectedSearchTerms.empty() || s_state.searchDebounced[0] != '\0') n++;
    if (!s_state.selectedMaps.empty()) n++;
    if (!s_state.selectedFluxes.empty()) n++;
    if (!s_state.selectedOccasions.empty()) n++;
    if (DateValValid(s_state.dateFrom) || DateValValid(s_state.dateTo)) n++;
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
    ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    ImGui::TextUnformatted("FILTERS");
    ImGui::PopStyleColor();
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
        s_state.fluxNames, s_state.selectedFluxes, "flux", true);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Occasion filter (multi-select) ──
    DrawMultiSelectFilter("Occasion", "Search occasions...",
        s_state.occasionSearchBuf, sizeof(s_state.occasionSearchBuf),
        s_state.occasionNames, s_state.selectedOccasions, "occasion", false);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Date range filter ──
    DrawDateRangeFilter();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Clear all filters ──
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    if (ImGui::Button("Clear All Filters", ImVec2(-1, 0)))
    {
        s_state.searchBuf[0] = '\0';
        s_state.searchDebounced[0] = '\0';
        s_state.lastSearchEditTime = -1.0f;
        s_state.selectedSearchTerms.clear();
        s_state.selectedMaps.clear();
        s_state.selectedFluxes.clear();
        s_state.selectedOccasions.clear();
        s_state.mapSearchBuf[0] = '\0';
        s_state.fluxSearchBuf[0] = '\0';
        s_state.occasionSearchBuf[0] = '\0';
        s_state.dateFrom = {};
        s_state.dateTo = {};
        s_state.calBrowseFromMonth = 0; s_state.calBrowseFromYear = 0;
        s_state.calBrowseToMonth = 0; s_state.calBrowseToYear = 0;
    }
    ImGui::PopStyleVar();

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

// ─── Match list table ────────────────────────────────────────────────────────

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
    float teamW = std::clamp((tableW - dateW - mapW - occasionW) * 0.5f, 100.0f, 250.0f);

    ImGuiTableFlags tableFlags =
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;

    if (ImGui::BeginTable("##match_table", 5, tableFlags))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Date",     ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed, dateW);
        ImGui::TableSetupColumn("Occasion", ImGuiTableColumnFlags_WidthFixed, occasionW);
        ImGui::TableSetupColumn("Map",      ImGuiTableColumnFlags_WidthFixed, mapW);
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
                if (ImGui::IsItemFocused() && !isSelected)
                    s_state.selectedMatchIndex = fm.originalIndex;

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.occasion.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(fm.mapName.c_str());

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

    // Team name header (full width, bold font)
    {
        ImFont* bold = GuiGlobalConstants::boldFont;
        if (bold) ImGui::PushFont(bold);

        if (guild.rank > 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kColorTextDim);
            ImGui::Text("#%d", guild.rank);
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 6);
        }

        ImGui::PushStyleColor(ImGuiCol_Text, isWinner ? kColorAccent : kColorText);
        ImGui::TextUnformatted(guild.display.c_str());
        ImGui::PopStyleColor();

        if (isWinner)
        {
            ImGui::SameLine(0, 4);
            ImTextureID cup = GetCupIcon();
            if (cup) ImGui::Image(cup, ImVec2(smallIconSize, smallIconSize));
        }

        if (bold) ImGui::PopFont();
    }

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
    const float nameColW = std::max(80.0f, availW * 0.18f);
    const float statColW = 34.0f;
    const float dmgColW = 44.0f;
    const float statIconSz = 20.0f;
    bool showSkills = (s_state.layout != LayoutMode::Mobile && s_state.layout != LayoutMode::Narrow);

    struct StatCol { const char* hdr; const char* tooltip; const char* iconFile; float w; };
    StatCol statCols[] = {
        { "K",   "Kills",            "damagedone3.jpg",   statColW },
        { "D",   "Deaths",           "death2.jpg",      statColW },
        { "DMG", "Damage Dealt",     "kill2.png", dmgColW  },
        { "INT", "Interrupts",       "interrupts2.jpg", statColW },
        { "CNC", "Cancelled Skills", "cancel2.png",     statColW },
        { "SKL", "Skill Count",      "skillcount2.jpg", statColW },
    };
    const int numStats = 6;

    int totals[6] = {};
    for (const auto* pp : sorted)
    {
        totals[0] += pp->kills;
        totals[1] += pp->deaths;
        totals[2] += pp->total_damage;
        totals[3] += pp->interrupted_count;
        totals[4] += pp->cancelled_skills_count;
        totals[5] += pp->skills_finished;
    }

    float skillsNeeded = showSkills ? (8 * (skillIconSize + 2) + 16.0f) : 0.0f;
    float statsNeeded = 0.0f;
    for (int si = 0; si < numStats; si++) statsNeeded += statCols[si].w + 6.0f;
    float fixedNeeded = iconSize + (iconSize + 2) + nameColW + skillsNeeded;
    bool statsOverflow = (fixedNeeded + statsNeeded) > availW;

    bool showStats = true;
    if (statsOverflow)
        showStats = s_state.statsExpanded;

    int fixedCols = 3;
    int numCols = fixedCols + (showSkills ? 1 : 0) + (showStats ? numStats : 0);

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
        if (showStats)
            for (int si = 0; si < numStats; si++)
                ImGui::TableSetupColumn(statCols[si].hdr, ImGuiTableColumnFlags_WidthFixed, statCols[si].w);

        // Stat icons header row
        if (showStats)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); // Pri
            ImGui::TableNextColumn(); // Sec
            ImGui::TableNextColumn(); // Name
            if (showSkills) ImGui::TableNextColumn(); // Skills

            for (int si = 0; si < numStats; si++)
            {
                ImGui::TableNextColumn();
                ImTextureID ico = GetStatIcon(statCols[si].iconFile);
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

            for (int si = 0; si < numStats; si++)
            {
                ImGui::TableNextColumn();
                char buf[16]; snprintf(buf, sizeof(buf), "%d", totals[si]);
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
            ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
            ImGui::TextUnformatted(cleanName.c_str());
            ImGui::PopStyleColor();

            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
                ImGui::Text("%s / %s", GetProfessionName(p.primary), GetProfessionName(p.secondary));
                if (!p.skill_template_code.empty())
                    ImGui::Text("Template: %s", p.skill_template_code.c_str());
                ImGui::PopStyleColor();
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

            if (showStats)
            {
                int statValues[] = {
                    p.kills, p.deaths, p.total_damage,
                    p.interrupted_count, p.cancelled_skills_count, p.skills_finished
                };

                for (int si = 0; si < numStats; si++)
                {
                    ImGui::TableNextColumn();
                    if (textPad > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textPad);
                    char buf[16]; snprintf(buf, sizeof(buf), "%d", statValues[si]);
                    float tw = ImGui::CalcTextSize(buf).x;
                    float padX = (statCols[si].w - tw) * 0.5f;
                    if (padX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
                    ImGui::TextUnformatted(buf);
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

        float closeW = 24.0f;
        float gap = 22.0f;
        float btnX = contentMaxX - closeW - gap - btnW;
        float btnY = lineY + (ImGui::GetTextLineHeightWithSpacing() - btnH) * 0.5f;

        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(btnX, btnY));

        const ImVec4 colBtnBg      (0.102f, 0.102f, 0.102f, 1.0f);
        const ImVec4 colBtnHover   (0.165f, 0.165f, 0.165f, 1.0f);
        const ImVec4 colBtnActive  (0.067f, 0.067f, 0.067f, 1.0f);
        const ImVec4 colBorder     = kColorAccent;
        const ImVec4 colGreen      (0.0f,   1.0f,   0.4f,   1.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
        ImGui::PushStyleColor(ImGuiCol_Button,        colBtnBg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  colBtnHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   colBtnActive);
        ImGui::PushStyleColor(ImGuiCol_Border,         colBorder);
        ImGui::PushStyleColor(ImGuiCol_Text,           kColorText);

        bool clicked = ImGui::Button("###replay_btn", ImVec2(btnW, btnH));
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();

        if (clicked)
        {
            g_pendingReplay.requested = true;
            g_pendingReplay.match = m;
        }

        ImVec2 rMin = ImGui::GetItemRectMin();
        ImVec2 rMax = ImGui::GetItemRectMax();

        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);

        // Draw ▶ icon (larger) in green
        {
            float iconX = rMin.x + padX;
            float iconY = rMin.y + (btnH - iconFontSz) * 0.5f;
            ImGui::GetWindowDrawList()->AddText(
                semibold ? semibold : ImGui::GetFont(),
                iconFontSz,
                ImVec2(iconX, iconY),
                ImGui::GetColorU32(colGreen),
                playIcon);
        }

        // Draw "REPLAY MATCH" text
        if (!narrowHeader)
        {
            float textX = rMin.x + padX + iconW + 4.0f;
            float textY = rMin.y + (btnH - fontSize) * 0.5f;
            ImGui::GetWindowDrawList()->AddText(
                semibold ? semibold : ImGui::GetFont(),
                fontSize,
                ImVec2(textX, textY),
                ImGui::GetColorU32(kColorText),
                "REPLAY MATCH");
        }

        if (semibold) ImGui::PopFont();

        // Pressed: brighter gold border
        if (active)
        {
            ImU32 brightGold = ImGui::GetColorU32(ImVec4(0.847f, 0.659f, 0.290f, 0.90f));
            ImGui::GetWindowDrawList()->AddRect(
                rMin, rMax, brightGold, 5.0f, 0, 2.0f);
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

    GuildLabel g1 = GetPartyGuild(m, "1");
    GuildLabel g2 = GetPartyGuild(m, "2");

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
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.18f, 0.80f));
            ImGui::BeginChild("##map_placeholder", ImVec2(mapImgSize, mapImgSize), ImGuiChildFlags_Border);
            ImGui::SetCursorPos(ImVec2(mapImgSize * 0.15f, mapImgSize * 0.4f));
            ImGui::TextColored(kColorTextDim, "No map");
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();

        ImFont* bold = GuiGlobalConstants::boldFont;

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
            ImGui::TextColored(kColorTextDim, "%s", m.occasion.c_str());
            if (bold) ImGui::PopFont();
        }
        if (!m.match_duration.empty())
        {
            ImTextureID durIco = GetDurationIcon();
            if (durIco) { ImGui::Image(durIco, ImVec2(icoH, icoH)); ImGui::SameLine(0, 4); }
            if (bold) ImGui::PushFont(bold);
            ImGui::TextColored(kColorTextDim, "%s", m.match_duration.c_str());
            if (bold) ImGui::PopFont();
        }
        if (!m.flux.empty())
            DrawFluxWithTooltip(m.flux, icoH);

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

    // Debounce global search (200ms)
    if (s_state.lastSearchEditTime >= 0.0f &&
        (float)ImGui::GetTime() - s_state.lastSearchEditTime >= 0.2f)
    {
        snprintf(s_state.searchDebounced, sizeof(s_state.searchDebounced),
                 "%s", s_state.searchBuf);
        s_state.lastSearchEditTime = -1.0f;
    }

    auto filtered = FilterMatches(matches);

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
        bool hasDetail = validSelection() && s_state.layout != LayoutMode::Mobile;

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
            {
                GuiGlobalConstants::replay_filter_width = (int)s_state.userFilterW;
                GuiGlobalConstants::SaveSettings();
            }
        }
        else
        {
            ImGui::SameLine(0, sp);
        }

        DrawMatchListTable(filtered, matches, topRowH);

        // ── Horizontal splitter + Detail ──
        if (hasDetail)
        {
            if (HSplitter("##h_splitter", availW, splitterThick))
            {
                s_state.userTopRowH += ImGui::GetIO().MouseDelta.y;
                float maxTopRow = totalH - 50.0f - splitterThick;
                s_state.userTopRowH = std::clamp(s_state.userTopRowH, 200.0f,
                    std::min(totalH * 0.90f, maxTopRow));
            }
            if (ImGui::IsItemDeactivated())
            {
                GuiGlobalConstants::replay_list_height = (int)s_state.userTopRowH;
                GuiGlobalConstants::SaveSettings();
            }

            if (validSelection())
                DrawMatchDetailPanel(matches[s_state.selectedMatchIndex]);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    PopGlassTheme(themeColors);
}
