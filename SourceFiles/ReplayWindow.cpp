#include "pch.h"
#include "ReplayWindow.h"
#include "AssetBlacklist.h"
#include "MatchRatings.h"
#include "MatchNotes.h"
#include "MatchBookmarks.h"
#include "AgentSnapshotParser.h"
#include "StoCParser.h"
#include "SkillDatabase.h"
#include "DXMathHelpers.h"
#include "FontConfig.h"
#include "GuiGlobalConstants.h"
#include "MapBrowser.h"
#include "TextureCache.h"
#include "CursorSystem.h"
#include "SpatialAudioEngine.h"
#include "SoundCache.h"
#include "Parsers/BB9AnimationParser.h"
#include "Parsers/FileReferenceParser.h"
#include "ReplayWindow_Internal.h"

#define NANOSVG_IMPLEMENTATION
#include "../ThirdParty/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../ThirdParty/nanosvg/nanosvgrast.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <algorithm>
#include <numeric>
#include <json.hpp>
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

static MapTransform LoadMapTransform(int mapId, bool* found = nullptr);

// ---------------------------------------------------------------------------
// Hotkey persistence (singleton, JSON)
// ---------------------------------------------------------------------------

static std::filesystem::path GetHotkeysFilePath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    auto settingsDir = dir / "settings";
    if (!std::filesystem::exists(settingsDir))
        std::filesystem::create_directories(settingsDir);
    return settingsDir / "hotkeys.json";
}

ReplayHotkeys& ReplayHotkeys::Get()
{
    static ReplayHotkeys instance;
    static bool loaded = false;
    if (!loaded) { instance.Load(); loaded = true; }
    return instance;
}

UINT ReplayHotkeys::ImGuiKeyToVK(int imguiKey)
{
    if (imguiKey >= ImGuiKey_A && imguiKey <= ImGuiKey_Z)
        return 'A' + (imguiKey - ImGuiKey_A);
    if (imguiKey >= ImGuiKey_0 && imguiKey <= ImGuiKey_9)
        return '0' + (imguiKey - ImGuiKey_0);
    switch (imguiKey)
    {
    case ImGuiKey_Space:       return VK_SPACE;
    case ImGuiKey_LeftArrow:   return VK_LEFT;
    case ImGuiKey_RightArrow:  return VK_RIGHT;
    case ImGuiKey_UpArrow:     return VK_UP;
    case ImGuiKey_DownArrow:   return VK_DOWN;
    case ImGuiKey_Tab:         return VK_TAB;
    case ImGuiKey_Escape:      return VK_ESCAPE;
    case ImGuiKey_Enter:       return VK_RETURN;
    case ImGuiKey_Backspace:   return VK_BACK;
    case ImGuiKey_Delete:      return VK_DELETE;
    case ImGuiKey_Insert:      return VK_INSERT;
    case ImGuiKey_Home:        return VK_HOME;
    case ImGuiKey_End:         return VK_END;
    case ImGuiKey_PageUp:      return VK_PRIOR;
    case ImGuiKey_PageDown:    return VK_NEXT;
    case ImGuiKey_MouseRight:  return VK_RBUTTON;
    case ImGuiKey_MouseMiddle: return VK_MBUTTON;
    case ImGuiKey_MouseX1:     return VK_XBUTTON1;
    case ImGuiKey_MouseX2:     return VK_XBUTTON2;
    default:                   return 0;
    }
}

bool ReplayHotkeys::IsValidBindableKey(int k)
{
    // Keyboard keys
    if (k >= ImGuiKey_NamedKey_BEGIN && k < ImGuiKey_MouseLeft)
        return true;
    // Mouse buttons (right, middle, X1, X2) — but not left-click or wheel
    if (k == ImGuiKey_MouseRight || k == ImGuiKey_MouseMiddle ||
        k == ImGuiKey_MouseX1    || k == ImGuiKey_MouseX2)
        return true;
    return false;
}

void ReplayHotkeys::Save() const
{
    auto path = GetHotkeysFilePath();
    std::ofstream f(path);
    if (!f.is_open()) return;
    f << "{\n"
      << "  \"rewind5s\": "            << rewind5s            << ",\n"
      << "  \"forward5s\": "           << forward5s           << ",\n"
      << "  \"playPause\": "           << playPause           << ",\n"
      << "  \"toggleRangeRings\": "    << toggleRangeRings    << ",\n"
      << "  \"toggleMoralePanel\": "   << toggleMoralePanel   << ",\n"
      << "  \"toggleEventTimeline\": " << toggleEventTimeline << ",\n"
      << "  \"toggleLordDamage\": "    << toggleLordDamage    << ",\n"
      << "  \"toggleAutoCamera\": "    << toggleAutoCamera    << ",\n"
      << "  \"toggleFogOfWar\": "      << toggleFogOfWar      << ",\n"
      << "  \"toggleTopView\": "       << toggleTopView       << ",\n"
      << "  \"togglePianoRoll\": "     << togglePianoRoll     << ",\n"
      << "  \"toggleHeatmap\": "       << toggleHeatmap       << ",\n"
      << "  \"addBookmark\": "         << addBookmark         << ",\n"
      << "  \"exitFollowMode\": "      << exitFollowMode      << ",\n"
      << "  \"camForward\": "          << camForward          << ",\n"
      << "  \"camBackward\": "         << camBackward         << ",\n"
      << "  \"camStrafeLeft\": "       << camStrafeLeft       << ",\n"
      << "  \"camStrafeRight\": "      << camStrafeRight      << ",\n"
      << "  \"invertMouseX\": "        << (invertMouseX ? 1 : 0) << ",\n"
      << "  \"invertMouseY\": "        << (invertMouseY ? 1 : 0) << "\n"
      << "}\n";
}

void ReplayHotkeys::Load()
{
    auto path = GetHotkeysFilePath();
    std::ifstream f(path);
    if (!f.is_open()) return;
    try {
        nlohmann::json j;
        f >> j;

        // Helper: only accept valid bindable keys (keyboard + mouse buttons, not wheel)
        auto readKey = [&](const char* name, int& field) {
            if (!j.contains(name)) return;
            int v = j[name].get<int>();
            if (IsValidBindableKey(v))
                field = v;
        };

        readKey("rewind5s",            rewind5s);
        readKey("forward5s",           forward5s);
        readKey("playPause",           playPause);
        readKey("toggleRangeRings",    toggleRangeRings);
        readKey("toggleMoralePanel",   toggleMoralePanel);
        readKey("toggleEventTimeline", toggleEventTimeline);
        readKey("toggleLordDamage",    toggleLordDamage);
        readKey("toggleAutoCamera",    toggleAutoCamera);
        readKey("toggleFogOfWar",      toggleFogOfWar);
        readKey("toggleTopView",       toggleTopView);
        readKey("togglePianoRoll",     togglePianoRoll);
        readKey("toggleHeatmap",       toggleHeatmap);
        readKey("addBookmark",         addBookmark);
        readKey("exitFollowMode",      exitFollowMode);
        readKey("camForward",          camForward);
        readKey("camBackward",         camBackward);
        readKey("camStrafeLeft",       camStrafeLeft);
        readKey("camStrafeRight",      camStrafeRight);
        if (j.contains("invertMouseX")) invertMouseX = j["invertMouseX"].get<int>() != 0;
        if (j.contains("invertMouseY")) invertMouseY = j["invertMouseY"].get<int>() != 0;
    } catch (...) {
        // File corrupted — reset everything to defaults
        *this = ReplayHotkeys{};
    }
}

// ---------------------------------------------------------------------------
// Panel layout registration — maps stable keys to m_show* pointers
// ---------------------------------------------------------------------------

void ReplayWindow::RegisterPanelLayout()
{
    if (m_panelLayoutRegistered) return;
    m_panelLayoutRegistered = true;

    // trackPosition=true for movable panels, false for fixed-position panels
    m_panelLayout.RegisterPanel("team1_party",     "Team 1 Party",         &m_showTeam1Party,       true,  true);
    m_panelLayout.RegisterPanel("team2_party",     "Team 2 Party",         &m_showTeam2Party,       true,  true);
    m_panelLayout.RegisterPanel("morale",          "Morale",               &m_showMoralePanel,      false, true);
    m_panelLayout.RegisterPanel("lord_damage",     "Lord Damage",          &m_showLordDamagePanel,  false, true);
    m_panelLayout.RegisterPanel("event_timeline",  "Event Timeline",       &m_showEventTimeline,    false, false);
    m_panelLayout.RegisterPanel("range_rings",     "Range Rings",          &m_showRangeRings,       false, true);
    m_panelLayout.RegisterPanel("auto_camera",     "Auto Camera",          &m_showAutoCameraPanel,  false, true);
    m_panelLayout.RegisterPanel("combat_log",      "Combat Log",           &m_showCombatLog,        false, true);
    m_panelLayout.RegisterPanel("skill_analytics", "Skill Analytics",      &m_showSkillAnalytics,   false, true);
    m_panelLayout.RegisterPanel("piano_roll",      "Piano Roll",           &m_showPianoRoll,        false, true);
    m_panelLayout.RegisterPanel("heatmap",         "Heatmap",              &m_heatmapSettings.show, false, true);
    m_panelLayout.RegisterPanel("agent_names",     "Agent Names",          &m_showNameFilterPanel,  false, true);
    m_panelLayout.RegisterPanel("split_camera",    "Split Camera",         &m_pipEnabled,           false, true);
    m_panelLayout.RegisterPanel("notepad",         "Match Notepad",        &m_showNotepad,          false, true);
}

uint64_t ReplayWindow::ComputePanelStateHash() const
{
    uint64_t h = 0;
    auto hashBool = [&](bool v) { h = h * 131 + (v ? 1u : 0u); };
    auto hashInt  = [&](int v)  { h = h * 131 + static_cast<uint64_t>(static_cast<uint32_t>(v)); };

    hashBool(m_alliesOpenTeam1);
    hashBool(m_alliesOpenTeam2);
    for (int i = 0; i < kRingTypeCount; i++) hashBool(m_ringType[i]);
    hashBool(m_ringShowBlue);
    hashBool(m_ringShowRed);
    hashBool(m_clFilterDamage);
    hashBool(m_clFilterHeals);
    hashBool(m_clFilterSkills);
    hashBool(m_clFilterInterrupt);
    hashBool(m_clFilterCancel);
    hashBool(m_clFilterDeaths);
    hashBool(m_clFilterAttacks);
    hashBool(m_clFilterJumbo);
    hashBool(m_clAutoScroll);
    hashBool(m_tlFilterDeath);
    hashBool(m_tlFilterFlag);
    hashBool(m_tlFilterMorale);
    hashBool(m_tlFilterLord);
    hashBool(m_tlFilterShrine);
    hashBool(m_tlFilterObelisk);
    hashInt(m_pianoRollZoomIdx);
    hashBool(m_pianoRollTeam1Open);
    hashBool(m_pianoRollTeam2Open);
    hashInt(m_fogPerspective);
    hashBool(m_fogGhostMode);
    hashInt(m_fogLastActive);
    hashBool(m_showDamageMeter);
    hashBool(m_showHealMeter);
    return h;
}

// ---------------------------------------------------------------------------
// UI Layout persistence (JSON)
// ---------------------------------------------------------------------------

std::filesystem::path GetUILayoutFilePath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    auto settingsDir = dir / "settings";
    if (!std::filesystem::exists(settingsDir))
        std::filesystem::create_directories(settingsDir);
    return settingsDir / "ui_layout.json";
}

void ReplayWindow::SaveUILayout()
{
    m_panelLayout.SyncVisibilityFromPointers();

    auto path = GetUILayoutFilePath();
    std::ofstream f(path);
    if (!f.is_open()) return;

    nlohmann::json j;
    j["useCustom"]    = m_uiLayout.useCustom;
    j["jumboX"]       = m_uiLayout.jumboX;
    j["jumboY"]       = m_uiLayout.jumboY;
    j["moBlueX"]      = m_uiLayout.moBlueX;
    j["moBlueY"]      = m_uiLayout.moBlueY;
    j["moRedX"]       = m_uiLayout.moRedX;
    j["moRedY"]       = m_uiLayout.moRedY;
    j["timerX"]       = m_uiLayout.timerX;
    j["timerY"]       = m_uiLayout.timerY;
    j["teamColorSwapped"] = true;
    j["lodEnabled"]   = m_uiLayout.lodEnabled;
    j["lodDotDist"]   = m_uiLayout.lodDotDist;
    j["lodPillarDist"] = m_uiLayout.lodPillarDist;

    nlohmann::json panels;
    m_panelLayout.SaveToJson(panels);
    j["panels"] = panels;

    // Per-panel filter/state persistence
    nlohmann::json ps;

    // Party window allies expanded state
    ps["alliesOpenTeam1"] = m_alliesOpenTeam1;
    ps["alliesOpenTeam2"] = m_alliesOpenTeam2;

    // Range Rings selections
    {
        nlohmann::json rt = nlohmann::json::array();
        for (int i = 0; i < kRingTypeCount; i++) rt.push_back(m_ringType[i]);
        ps["ringTypes"]    = rt;
    }
    ps["ringShowBlue"]     = m_ringShowBlue;
    ps["ringShowRed"]      = m_ringShowRed;

    // Combat Log filters
    ps["clFilterDamage"]    = m_clFilterDamage;
    ps["clFilterHeals"]     = m_clFilterHeals;
    ps["clFilterSkills"]    = m_clFilterSkills;
    ps["clFilterInterrupt"] = m_clFilterInterrupt;
    ps["clFilterCancel"]    = m_clFilterCancel;
    ps["clFilterDeaths"]    = m_clFilterDeaths;
    ps["clFilterAttacks"]   = m_clFilterAttacks;
    ps["clFilterJumbo"]     = m_clFilterJumbo;
    ps["clAutoScroll"]      = m_clAutoScroll;

    // Event Timeline filters
    ps["tlFilterDeath"]       = m_tlFilterDeath;
    ps["tlFilterFlag"]        = m_tlFilterFlag;
    ps["tlFilterMorale"]      = m_tlFilterMorale;
    ps["tlFilterLord"]        = m_tlFilterLord;
    ps["tlFilterShrine"]      = m_tlFilterShrine;
    ps["tlFilterObelisk"]     = m_tlFilterObelisk;

    // Piano Roll state
    ps["pianoRollZoomIdx"]    = m_pianoRollZoomIdx;
    ps["pianoRollTeam1Open"]  = m_pianoRollTeam1Open;
    ps["pianoRollTeam2Open"]  = m_pianoRollTeam2Open;

    // Fog of War state
    ps["fogPerspective"]      = m_fogPerspective;
    ps["fogGhostMode"]        = m_fogGhostMode;
    ps["fogLastActive"]       = m_fogLastActive;

    // Damage / Heal meter toggles
    ps["showDamageMeter"]     = m_showDamageMeter;
    ps["showHealMeter"]       = m_showHealMeter;

    // Split Camera state
    ps["pipFollowDist"]       = m_pipFollowDist;
    ps["pipFollowYaw"]        = m_pipFollowYaw;
    ps["pipFollowPitch"]      = m_pipFollowPitch;
    ps["pipManualAgent"]      = m_pipManualAgent;

    j["panelState"] = ps;

    f << j.dump(2) << "\n";

    m_panelLayout.ClearDirty();
}

void ReplayWindow::LoadUILayout()
{
    RegisterPanelLayout();

    auto path = GetUILayoutFilePath();
    std::ifstream f(path);
    if (!f.is_open()) return;
    try {
        nlohmann::json j;
        f >> j;
        if (j.contains("useCustom")) m_uiLayout.useCustom = j["useCustom"].get<bool>();
        if (j.contains("jumboX"))    m_uiLayout.jumboX   = j["jumboX"].get<float>();
        if (j.contains("jumboY"))    m_uiLayout.jumboY   = j["jumboY"].get<float>();
        if (j.contains("moBlueX"))   m_uiLayout.moBlueX  = j["moBlueX"].get<float>();
        if (j.contains("moBlueY"))   m_uiLayout.moBlueY  = j["moBlueY"].get<float>();
        if (j.contains("moRedX"))    m_uiLayout.moRedX   = j["moRedX"].get<float>();
        if (j.contains("moRedY"))    m_uiLayout.moRedY   = j["moRedY"].get<float>();
        if (j.contains("timerX"))    m_uiLayout.timerX   = j["timerX"].get<float>();
        if (j.contains("timerY"))    m_uiLayout.timerY   = j["timerY"].get<float>();

        if (!j.contains("teamColorSwapped"))
        {
            std::swap(m_uiLayout.moBlueX, m_uiLayout.moRedX);
            std::swap(m_uiLayout.moBlueY, m_uiLayout.moRedY);
        }
        if (j.contains("lodEnabled"))    m_uiLayout.lodEnabled    = j["lodEnabled"].get<bool>();
        if (j.contains("lodDotDist"))    m_uiLayout.lodDotDist    = j["lodDotDist"].get<float>();
        if (j.contains("lodPillarDist")) m_uiLayout.lodPillarDist = j["lodPillarDist"].get<float>();

        if (j.contains("panels"))
        {
            m_panelLayout.LoadFromJson(j["panels"]);
            m_panelLayout.SyncVisibilityToPointers();
        }

        if (j.contains("panelState"))
        {
            auto& ps = j["panelState"];
            auto bv = [&](const char* k, bool& dst)  { if (ps.contains(k)) dst = ps[k].get<bool>(); };
            auto iv = [&](const char* k, int& dst)   { if (ps.contains(k)) dst = ps[k].get<int>(); };
            auto fv = [&](const char* k, float& dst) { if (ps.contains(k)) dst = ps[k].get<float>(); };

            // Party window allies expanded state
            bv("alliesOpenTeam1", m_alliesOpenTeam1);
            bv("alliesOpenTeam2", m_alliesOpenTeam2);

            // Range Rings selections
            if (ps.contains("ringTypes") && ps["ringTypes"].is_array())
            {
                auto& rt = ps["ringTypes"];
                for (int i = 0; i < kRingTypeCount && i < (int)rt.size(); i++)
                    m_ringType[i] = rt[i].get<bool>();
            }
            bv("ringShowBlue", m_ringShowBlue);
            bv("ringShowRed",  m_ringShowRed);

            if (!j.contains("teamColorSwapped"))
            {
                std::swap(m_ringShowBlue, m_ringShowRed);
            }

            // Combat Log filters
            bv("clFilterDamage",    m_clFilterDamage);
            bv("clFilterHeals",     m_clFilterHeals);
            bv("clFilterSkills",    m_clFilterSkills);
            bv("clFilterInterrupt", m_clFilterInterrupt);
            bv("clFilterCancel",    m_clFilterCancel);
            bv("clFilterDeaths",    m_clFilterDeaths);
            bv("clFilterAttacks",   m_clFilterAttacks);
            bv("clFilterJumbo",     m_clFilterJumbo);
            bv("clAutoScroll",      m_clAutoScroll);

            // Event Timeline filters
            bv("tlFilterDeath",      m_tlFilterDeath);
            bv("tlFilterFlag",       m_tlFilterFlag);
            bv("tlFilterMorale",     m_tlFilterMorale);
            bv("tlFilterLord",       m_tlFilterLord);
            bv("tlFilterShrine",     m_tlFilterShrine);
            bv("tlFilterObelisk",    m_tlFilterObelisk);

            // Piano Roll state
            iv("pianoRollZoomIdx",   m_pianoRollZoomIdx);
            bv("pianoRollTeam1Open", m_pianoRollTeam1Open);
            bv("pianoRollTeam2Open", m_pianoRollTeam2Open);

            // Fog of War state
            iv("fogPerspective",  m_fogPerspective);
            bv("fogGhostMode",   m_fogGhostMode);
            iv("fogLastActive",  m_fogLastActive);

            // Damage / Heal meter toggles
            bv("showDamageMeter", m_showDamageMeter);
            bv("showHealMeter",   m_showHealMeter);

            // Split Camera state
            fv("pipFollowDist",  m_pipFollowDist);
            fv("pipFollowYaw",   m_pipFollowYaw);
            fv("pipFollowPitch", m_pipFollowPitch);
            iv("pipManualAgent", m_pipManualAgent);
        }
    } catch (...) {}

    m_panelLayout.ClearDirty();
    m_lastPanelStateHash = ComputePanelStateHash();
}

// ---------------------------------------------------------------------------

bool ReplayWindow::s_classRegistered = false;

// ---------------------------------------------------------------------------
// Inline HLSL for the 2D loading overlay
// ---------------------------------------------------------------------------

static const char kOverlayHLSL[] = R"(
struct VS_IN  { float2 pos : POSITION; float4 col : COLOR; };
struct VS_OUT { float4 pos : SV_Position; float4 col : COLOR; };

VS_OUT VSMain(VS_IN i) {
    VS_OUT o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.col = i.col;
    return o;
}

float4 PSMain(VS_OUT i) : SV_Target { return i.col; }
)";

// ---------------------------------------------------------------------------
// Window class registration
// ---------------------------------------------------------------------------

bool ReplayWindow::RegisterWindowClass(HINSTANCE hInstance)
{
    if (s_classRegistered) return true;

    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(WNDCLASSEXW);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = ReplayWindow::WndProc;
    wcex.hInstance     = hInstance;
    wcex.hIcon         = LoadIconW(hInstance, L"IDI_ICON");
    wcex.hCursor       = nullptr;
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wcex.lpszClassName = kWindowClassName;
    wcex.hIconSm       = LoadIconW(hInstance, L"IDI_ICON");

    if (!RegisterClassExW(&wcex))
        return false;

    s_classRegistered = true;
    return true;
}

// ---------------------------------------------------------------------------
// Build the window title
// ---------------------------------------------------------------------------

static std::wstring BuildWindowTitle(const MatchMeta& match)
{
    // Parse authoritative guild tags from folder name
    std::string ft1, ft2;
    {
        auto vs = match.folder_name.find("]vs[");
        if (vs != std::string::npos) {
            auto open1 = match.folder_name.rfind('[', vs);
            auto close2 = match.folder_name.find(']', vs + 4);
            if (open1 != std::string::npos && close2 != std::string::npos) {
                ft1 = match.folder_name.substr(open1 + 1, vs - open1 - 1);
                ft2 = match.folder_name.substr(vs + 4, close2 - (vs + 4));
            }
        }
    }

    auto findGuildByTag = [&](const std::string& tag) -> const GuildMeta* {
        if (tag.empty()) return nullptr;
        for (const auto& [id, gm] : match.guilds)
            if (gm.tag == tag) return &gm;
        return nullptr;
    };

    auto getGuildLabel = [&](const std::string& partyId, const std::string& folderTag) -> std::pair<std::string, std::string>
    {
        auto* fg = findGuildByTag(folderTag);
        if (fg) return { fg->name, fg->tag };

        auto pit = match.parties.find(partyId);
        if (pit == match.parties.end()) return { "Unknown", "?" };

        std::map<int, int> guildCounts;
        for (const auto& p : pit->second.players)
            if (p.guild_id > 0) guildCounts[p.guild_id]++;

        int bestGuildId = 0, bestCount = 0;
        for (const auto& [gid, cnt] : guildCounts)
            if (cnt > bestCount) { bestGuildId = gid; bestCount = cnt; }

        if (bestGuildId == 0) return { "Unknown", "?" };

        auto git = match.guilds.find(std::to_string(bestGuildId));
        if (git != match.guilds.end())
            return { git->second.name, git->second.tag };

        return { "Guild #" + std::to_string(bestGuildId), "?" };
    };

    auto [name1, tag1] = getGuildLabel("1", ft1);
    auto [name2, tag2] = getGuildLabel("2", ft2);

    std::string title = std::format("Guild Wars Observer - {:04d}/{:02d}/{:02d} {} [{}] vs {} [{}]",
        match.year, match.month, match.day,
        name1, tag1, name2, tag2);

    return std::wstring(title.begin(), title.end());
}

// ---------------------------------------------------------------------------
// Spatial Audio
// ---------------------------------------------------------------------------

std::filesystem::path GetSkillSoundsFilePath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    auto settingsDir = dir / "settings";
    return settingsDir / "skill_sounds.json";
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

ReplayWindow* ReplayWindow::Create(HINSTANCE hInstance, const MatchMeta& match,
                                    DATManager* sharedDatManager,
                                    const std::unordered_map<int, std::vector<int>>& hashIndex)
{
    auto* rw = new ReplayWindow();
    rw->m_matchMeta   = match;
    rw->m_datManager   = sharedDatManager;
    rw->m_hashIndex    = &hashIndex;

    // Parse authoritative guild tags from folder name early,
    // before the loading screen tries to resolve capes/headers.
    {
        const auto& fn = match.folder_name;
        auto vs = fn.find("]vs[");
        if (vs != std::string::npos) {
            auto open1 = fn.rfind('[', vs);
            auto close2 = fn.find(']', vs + 4);
            if (open1 != std::string::npos && close2 != std::string::npos) {
                rw->m_folderTag1 = fn.substr(open1 + 1, vs - open1 - 1);
                rw->m_folderTag2 = fn.substr(vs + 4, close2 - (vs + 4));
            }
        }
    }

    rw->m_replayCtx.mapId       = match.map_id;
    rw->m_replayCtx.datMapId    = GetDatMapId(match.map_id);
    rw->m_replayCtx.matchFolderPath = match.folder_path;

    if (!RegisterWindowClass(hInstance))
    {
        delete rw;
        return nullptr;
    }

    std::wstring title = BuildWindowTitle(match);
    if (!rw->InitWindow(hInstance, L"Loading... " + title))
    {
        delete rw;
        return nullptr;
    }

    if (!rw->InitGraphics())
    {
        DestroyWindow(rw->m_hwnd);
        delete rw;
        return nullptr;
    }

    if (!rw->InitLoadingOverlay())
    {
        DestroyWindow(rw->m_hwnd);
        delete rw;
        return nullptr;
    }

    rw->m_alive = true;
    rw->m_useAgentModels = GuiGlobalConstants::use_3d_agent_models;
    rw->m_loadingPhase = LoadingPhase::Validate;

    // Restore persisted bookmarks for this match
    {
        auto& saved = MatchBookmarks::Get().GetBookmarks(match.folder_name);
        rw->m_annotationMgr.bookmarks.reserve(saved.size());
        for (auto& e : saved)
        {
            AnnotationManager::Bookmark bk;
            bk.timestamp_ms = e.time_ms;
            bk.title        = e.title;
            rw->m_annotationMgr.bookmarks.push_back(std::move(bk));
        }
        if (!rw->m_annotationMgr.bookmarks.empty())
            rw->m_annotationMgr.bookmarks_visible = true;
    }

    // Persist bookmarks whenever they change
    std::string folderName = match.folder_name;
    rw->m_annotationMgr.onBookmarksChanged = [folderName, rw]()
    {
        std::vector<MatchBookmarks::Entry> entries;
        entries.reserve(rw->m_annotationMgr.bookmarks.size());
        for (auto& bk : rw->m_annotationMgr.bookmarks)
            entries.push_back({ bk.timestamp_ms, bk.title });
        MatchBookmarks::Get().SetBookmarks(folderName, entries);
    };

    ShowWindow(rw->m_hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(rw->m_hwnd);
    return rw;
}

// ---------------------------------------------------------------------------

ReplayWindow::~ReplayWindow()
{
    if (m_agentModelLoadThread.joinable())
        m_agentModelLoadThread.join();
    if (m_audioEngine) m_audioEngine->Shutdown();
    ShutdownImGui();
    if (m_hwnd)
        SetWindowLongPtr(m_hwnd, GWLP_USERDATA, 0);
}

// ---------------------------------------------------------------------------
// Window creation
// ---------------------------------------------------------------------------

bool ReplayWindow::InitWindow(HINSTANCE hInstance, const std::wstring& title)
{
    int w = 1280, h = 720;
    RECT rc = { 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(0, kWindowClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!m_hwnd) return false;

    SetWindowLongPtr(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

// ---------------------------------------------------------------------------
// Graphics init
// ---------------------------------------------------------------------------

bool ReplayWindow::InitGraphics()
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    int width  = rc.right  - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0) width = 1280;
    if (height <= 0) height = 720;

    m_deviceResources = std::make_unique<DX::DeviceResources>(DXGI_FORMAT_R8G8B8A8_UNORM);
    m_deviceResources->SetWindow(m_hwnd, width, height);
    m_deviceResources->CreateDeviceResources();
    m_deviceResources->CreateWindowSizeDependentResources();

    m_inputManager = std::make_unique<InputManager>(m_hwnd);

    m_mapRenderer = std::make_unique<MapRenderer>(
        m_deviceResources->GetD3DDevice(),
        m_deviceResources->GetD3DDeviceContext(),
        m_inputManager.get());
    m_mapRenderer->Initialize(static_cast<float>(width), static_cast<float>(height),
        GuiGlobalConstants::ClampReplayCameraFovDegrees(GuiGlobalConstants::replay_camera_fov_degrees));

    m_deviceResources->RegisterDeviceNotify(this);
    return true;
}

// ---------------------------------------------------------------------------
// Loading overlay GPU resources (simple 2D colored quad shader)
// ---------------------------------------------------------------------------

bool ReplayWindow::InitLoadingOverlay()
{
    auto* device = m_deviceResources->GetD3DDevice();
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif

    // Compile vertex shader
    ComPtr<ID3DBlob> vsBlob, errBlob;
    HRESULT hr = D3DCompile(kOverlayHLSL, sizeof(kOverlayHLSL), nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", flags, 0, vsBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
        m_overlayVS.GetAddressOf());
    if (FAILED(hr)) return false;

    // Input layout
    D3D11_INPUT_ELEMENT_DESC ilDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device->CreateInputLayout(ilDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        m_overlayIL.GetAddressOf());
    if (FAILED(hr)) return false;

    // Compile pixel shader
    ComPtr<ID3DBlob> psBlob;
    hr = D3DCompile(kOverlayHLSL, sizeof(kOverlayHLSL), nullptr, nullptr, nullptr,
        "PSMain", "ps_5_0", flags, 0, psBlob.GetAddressOf(), errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
        m_overlayPS.GetAddressOf());
    if (FAILED(hr)) return false;

    // Dynamic vertex buffer (enough for bar background + bar fill = 12 vertices)
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(OverlayVertex) * 12;
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&vbDesc, nullptr, m_overlayVB.GetAddressOf());
    if (FAILED(hr)) return false;

    // Depth stencil state: no depth test
    D3D11_DEPTH_STENCIL_DESC dssDesc = {};
    dssDesc.DepthEnable = FALSE;
    dssDesc.StencilEnable = FALSE;
    hr = device->CreateDepthStencilState(&dssDesc, m_overlayDSS.GetAddressOf());
    if (FAILED(hr)) return false;

    // Rasterizer state: no culling
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rsDesc, m_overlayRS.GetAddressOf());
    if (FAILED(hr)) return false;

    // Blend state: opaque
    D3D11_BLEND_DESC bsDesc = {};
    bsDesc.RenderTarget[0].BlendEnable = FALSE;
    bsDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&bsDesc, m_overlayBS.GetAddressOf());
    if (FAILED(hr)) return false;

    // Blend state: additive (ONE/ONE) for shrine beam glow
    D3D11_BLEND_DESC abDesc = {};
    abDesc.RenderTarget[0].BlendEnable = TRUE;
    abDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    abDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    abDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    abDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    abDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    abDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    abDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&abDesc, m_additiveBS.GetAddressOf());
    if (FAILED(hr)) return false;

    return true;
}

// ---------------------------------------------------------------------------
// ImGui init / shutdown (private context for this window)
// ---------------------------------------------------------------------------

std::string GetReplayFontBasePath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "Fonts"))
            return (dir / "Textures" / "Fonts").string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return "";
}

void ReplayWindow::InitImGui()
{
    if (m_imguiInitialized) return;

    ImGuiContext* prevCtx = ImGui::GetCurrentContext();

    m_imguiContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_imguiContext);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;

    // Load the same font as the main UI
    float fontSize = GuiGlobalConstants::saved_font_size;
    int fontIdx = GuiGlobalConstants::saved_font_index;
    if (fontIdx < 0 || fontIdx >= g_fontTableCount) fontIdx = 2;
    const FontEntry& fe = g_fontTable[fontIdx];
    bool fontLoaded = false;

    if (fe.fileName)
    {
        std::string fullPath;
        if (fe.isSystemFont)
            fullPath = std::string("C:\\Windows\\Fonts\\") + fe.fileName;
        else
        {
            std::string base = GetReplayFontBasePath();
            if (!base.empty()) fullPath = base + "\\" + fe.fileName;
        }
        if (!fullPath.empty() && std::filesystem::exists(fullPath))
        {
            io.Fonts->AddFontFromFileTTF(fullPath.c_str(), fontSize);
            fontLoaded = true;
        }
    }
    if (!fontLoaded)
        io.Fonts->AddFontDefault();

    // Merge symbol glyphs so arrows (U+2190-21FF) and misc symbols render
    {
        ImFontConfig mergeConfig;
        mergeConfig.MergeMode = true;
        mergeConfig.PixelSnapH = true;
        static const ImWchar symbolRanges[] = {
            0x2190, 0x21FF,   // Arrows (includes → U+2192)
            0x2500, 0x257F,   // Box Drawing
            0x25A0, 0x25FF,   // Geometric Shapes
            0x2600, 0x26FF,   // Miscellaneous Symbols (includes ⚔ U+2694)
            0, 0
        };
        const char* symbolFonts[] = {
            "C:\\Windows\\Fonts\\seguisym.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\arial.ttf",
        };
        for (const char* path : symbolFonts)
        {
            if (std::filesystem::exists(path))
            {
                io.Fonts->AddFontFromFileTTF(path, fontSize, &mergeConfig, symbolRanges);
                break;
            }
        }
    }

    // Load Lato fonts for scene overlays (timer, jumbo, morale)
    {
        std::string base = GetReplayFontBasePath();
        if (!base.empty())
        {
            std::string latoRegPath = base + "\\Lato-Regular.ttf";
            std::string latoBoldPath = base + "\\Lato-Bold.ttf";
            if (std::filesystem::exists(latoRegPath))
                m_latoRegular = io.Fonts->AddFontFromFileTTF(latoRegPath.c_str(), 19.f);
            if (std::filesystem::exists(latoBoldPath))
            {
                m_latoBold    = io.Fonts->AddFontFromFileTTF(latoBoldPath.c_str(), 18.f);
                m_latoBoldBig = io.Fonts->AddFontFromFileTTF(latoBoldPath.c_str(), 40.f);
            }
        }
    }

    io.Fonts->Build();

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_deviceResources->GetD3DDevice(),
                        m_deviceResources->GetD3DDeviceContext());

    m_imguiInitialized = true;
    LoadUILayout();
    LoadHeatmapSettings();

    ImGui::SetCurrentContext(prevCtx);
}

void ReplayWindow::ShutdownImGui()
{
    if (!m_imguiInitialized) return;

    ImGuiContext* prevCtx = ImGui::GetCurrentContext();
    bool needRestore = (prevCtx != m_imguiContext);

    ImGui::SetCurrentContext(m_imguiContext);

    SaveUILayout();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(m_imguiContext);
    m_imguiContext = nullptr;
    m_imguiInitialized = false;

    if (needRestore)
        ImGui::SetCurrentContext(prevCtx);
    else
        ImGui::SetCurrentContext(nullptr);
}

// ---------------------------------------------------------------------------
// Loading phase: Validate
// ---------------------------------------------------------------------------

void ReplayWindow::StepValidate()
{
    if (!m_totalLoadTimerStarted) {
        m_totalLoadStartTime = LoadClock::now();
        m_totalLoadTimerStarted = true;
    }
    m_phaseStartTime = LoadClock::now();

    if (!m_datManager || !m_hashIndex)
    {
        m_errorMsg = "gw.dat is missing or unreadable.";
        m_loadingPhase = LoadingPhase::Error;
        return;
    }

    if (m_datManager->m_initialization_state < InitializationState::IndexReady)
    {
        m_errorMsg = "gw.dat is still loading. Please wait and try again.";
        m_loadingPhase = LoadingPhase::Error;
        return;
    }

    uint32_t datFileHash = m_replayCtx.datMapId;
    if (datFileHash == 0)
    {
        m_errorMsg = std::format("Unknown map_id {}. No dat mapping available.", m_replayCtx.mapId);
        m_loadingPhase = LoadingPhase::Error;
        return;
    }

    auto it = m_hashIndex->find(static_cast<int>(datFileHash));
    if (it == m_hashIndex->end() || it->second.empty())
    {
        m_errorMsg = std::format("Unable to load map for map_id {} (dat ID 0x{:X}).",
                                 m_replayCtx.mapId, datFileHash);
        m_loadingPhase = LoadingPhase::Error;
        return;
    }

    // Launch async agent snapshot parsing in parallel with map loading
    if (!m_replayCtx.agentParseProgress)
    {
        m_replayCtx.agentParseProgress = std::make_shared<AgentParseProgress>();
        LaunchAgentSnapshotParsing(m_replayCtx.matchFolderPath,
                                   m_replayCtx.agentParseProgress);
    }

    // Launch async StoC event parsing in parallel
    if (!m_replayCtx.stocParseProgress)
    {
        m_replayCtx.stocParseProgress = std::make_shared<StoCParseProgress>();
        LaunchStoCParsing(m_replayCtx.matchFolderPath, m_replayCtx.stocParseProgress);
    }

    m_loadTiming.validateSec = std::chrono::duration<double>(LoadClock::now() - m_phaseStartTime).count();
    OutputDebugStringA(std::format("[ReplayLoad] Validate: {:.3f}s\n", m_loadTiming.validateSec).c_str());

    m_loadingPhase = LoadingPhase::Init;
}

// ---------------------------------------------------------------------------
// Loading phase: Init (parse map, terrain, env, sky, water, fog)
// ---------------------------------------------------------------------------

void ReplayWindow::StepLoadInit()
{
    m_phaseStartTime = LoadClock::now();

    uint32_t datFileHash = m_replayCtx.datMapId;
    auto it = m_hashIndex->find(static_cast<int>(datFileHash));
    int mftIndex = it->second.at(0);

    m_mapFile = m_datManager->parse_ffna_map_file(mftIndex);

    if (m_mapFile.terrain_chunk.terrain_heightmap.empty() ||
        m_mapFile.terrain_chunk.terrain_heightmap.size() !=
        m_mapFile.terrain_chunk.terrain_x_dims * m_mapFile.terrain_chunk.terrain_y_dims)
    {
        m_errorMsg = std::format("Unable to load map for map_id {} (dat ID 0x{:X}). Terrain data missing.",
                                 m_replayCtx.mapId, datFileHash);
        m_loadingPhase = LoadingPhase::Error;
        return;
    }

    auto* map_renderer = m_mapRenderer.get();
    map_renderer->GetTextureManager()->Clear();
    map_renderer->ClearSceneForModeSwitch();
    map_renderer->SetShouldRenderShadowsForModels(true);

    // --- Environment setup (lighting, sky, fog, water) ---
    const auto& envChunk = m_mapFile.environment_info_chunk;
    const EnvSubChunk8* env8 = envChunk.env_sub_chunk8.empty() ? nullptr : &envChunk.env_sub_chunk8[0];

    PerSkyCB sky_cb = map_renderer->GetPerSkyCB();

    // Brightness/saturation
    {
        float brightness = 1.0f, saturation = 1.0f, bias_add = 0.0f;
        if (!envChunk.env_sub_chunk1.empty()) {
            size_t idx = (env8 && env8->sky_settings_index < envChunk.env_sub_chunk1.size())
                ? env8->sky_settings_index : 0u;
            const auto& sub1 = envChunk.env_sub_chunk1[idx];
            brightness = std::clamp(sub1.sky_brightness_maybe / 128.0f, 0.0f, 2.0f);
            saturation = std::clamp(sub1.sky_saturaion_maybe / 128.0f, 0.0f, 2.0f);
        }
        if (env8) {
            bias_add = (static_cast<int>(env8->sky_brightness_bias) - 128) / 128.0f;
            bias_add = std::clamp(bias_add * 0.15f, -0.25f, 0.25f);
        }
        sky_cb.color_params = XMFLOAT4(brightness, saturation, bias_add, 0.0f);
    }

    // Lighting
    if (!envChunk.env_sub_chunk3.empty()) {
        size_t idx = (env8 && env8->lighting_settings_index < envChunk.env_sub_chunk3.size())
            ? env8->lighting_settings_index : 0u;
        const auto& sub3 = envChunk.env_sub_chunk3[idx];
        float light_div = 2.0f;
        float ambient_intensity = sub3.ambient_intensity / 255.0f;
        float diffuse_intensity = sub3.sun_intensity / 255.0f;

        DirectionalLight dl = map_renderer->GetDirectionalLight();
        dl.ambient.x = sub3.ambient_red / (255.0f * light_div);
        dl.ambient.y = sub3.ambient_green / (255.0f * light_div);
        dl.ambient.z = sub3.ambient_blue / (255.0f * light_div);
        dl.diffuse.x = sub3.sun_red / (255.0f * light_div);
        dl.diffuse.y = sub3.sun_green / (255.0f * light_div);
        dl.diffuse.z = sub3.sun_blue / (255.0f * light_div);

        auto ahls = RGBAtoHSL(dl.ambient);
        auto dhls = RGBAtoHSL(dl.diffuse);
        ahls.z = std::max(ambient_intensity * 0.9f, 0.7f);
        dhls.z = std::max(diffuse_intensity * 0.9f, 0.5f);
        dl.ambient = HSLtoRGBA(ahls);
        dl.diffuse = HSLtoRGBA(dhls);
        map_renderer->SetDirectionalLight(dl);
    }

    // Sky texture settings
    uint16_t sky_bg_idx = 0xFFFF;
    uint16_t sky_clouds0_idx = 0xFFFF, sky_clouds1_idx = 0xFFFF;
    uint16_t sky_sun_idx = 0xFFFF;
    uint16_t water_color_idx = 0xFFFF, water_distort_idx = 0xFFFF;

    const uint16_t selSkyTexIdx   = env8 ? env8->sky_texture_settings_index : 0u;
    const uint16_t selWaterIdx    = env8 ? env8->water_settings_index : 0u;
    const uint16_t selWindIdx     = env8 ? env8->wind_settings_index : 0u;

    if (!envChunk.env_sub_chunk5.empty()) {
        size_t si = (selSkyTexIdx < envChunk.env_sub_chunk5.size()) ? selSkyTexIdx : 0u;
        const auto& sub5 = envChunk.env_sub_chunk5[si];
        sky_bg_idx      = sub5.sky_background_texture_index;
        sky_clouds0_idx = sub5.sky_clouds_texture_index0;
        sky_clouds1_idx = sub5.sky_clouds_texture_index1;
        sky_sun_idx     = sub5.sky_sun_texture_index;

        const float uv_scale = std::max(1.0f, std::round(sub5.unknown0 / 32.0f));
        const float kDenom = 16777216.0f;
        const float scroll_u = static_cast<float>(sub5.unknown1) / kDenom;
        const float scroll_v = static_cast<float>(sub5.unknown2) / kDenom;
        const float sun_scale = sub5.unknown3 / 255.0f;
        const float sun_disk_radius = (0.01f + sun_scale * 0.05f) * 3.0f;
        sky_cb.cloud0_params = XMFLOAT4(uv_scale, scroll_u, scroll_v, 1.0f);
        sky_cb.cloud1_params = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        sky_cb.sun_params.x = sun_disk_radius;
    }
    else if (!envChunk.env_sub_chunk5_other.empty()) {
        size_t si = (selSkyTexIdx < envChunk.env_sub_chunk5_other.size()) ? selSkyTexIdx : 0u;
        const auto& sub5 = envChunk.env_sub_chunk5_other[si];
        sky_bg_idx      = sub5.sky_background_texture_index;
        sky_clouds0_idx = sub5.sky_clouds_texture_index0;
        sky_clouds1_idx = sub5.sky_clouds_texture_index1;
        sky_sun_idx     = sub5.sky_sun_texture_index;

        uint8_t scale_byte = 0; int16_t s0 = 0, s1 = 0; uint8_t sun_byte = 0;
        std::memcpy(&scale_byte, &sub5.unknown[0], 1);
        std::memcpy(&s0, &sub5.unknown[1], 2);
        std::memcpy(&s1, &sub5.unknown[3], 2);
        std::memcpy(&sun_byte, &sub5.unknown[5], 1);
        const float uv_scale = std::max(1.0f, std::round(scale_byte / 32.0f));
        const float kDenom = 16777216.0f;
        sky_cb.cloud0_params = XMFLOAT4(uv_scale, s0 / kDenom, s1 / kDenom, 1.0f);
        sky_cb.cloud1_params = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        sky_cb.sun_params.x  = (0.01f + sun_byte / 255.0f * 0.05f) * 3.0f;
    }
    map_renderer->SetPerSkyCB(sky_cb);

    // Water settings
    if (!envChunk.env_sub_chunk6.empty()) {
        size_t wi = (selWaterIdx < envChunk.env_sub_chunk6.size()) ? selWaterIdx : 0u;
        water_color_idx   = envChunk.env_sub_chunk6[wi].water_color_texture_index;
        water_distort_idx = envChunk.env_sub_chunk6[wi].water_distortion_texture_index;
    }

    // Helper to load a texture from the dat by env filename index
    const auto& envFilenames = m_mapFile.environment_info_filenames_chunk;
    auto loadEnvTexture = [&](uint16_t filenameIndex) -> ID3D11ShaderResourceView*
    {
        if (filenameIndex >= envFilenames.filenames.size()) return nullptr;
        const auto& fn = envFilenames.filenames[filenameIndex];
        auto decoded = decode_filename(fn.filename.id0, fn.filename.id1);
        auto mit = m_hashIndex->find(decoded);
        if (mit == m_hashIndex->end()) return nullptr;
        int ti = mit->second.at(0);
        m_datManager->EnsureTypeClassified(ti);
        auto type = m_datManager->get_MFT()[ti].type;
        int texId = -1;
        if (type == DDS) {
            auto ddsData = m_datManager->parse_dds_file(ti);
            DatTexture dt;
            auto hr = map_renderer->GetTextureManager()->CreateTextureFromDDSInMemory(
                ddsData.data(), ddsData.size(), &texId, &dt.width, &dt.height, dt.rgba_data, decoded);
            if (SUCCEEDED(hr) && texId >= 0)
                return map_renderer->GetTextureManager()->GetTexture(texId);
        }
        else {
            DatTexture dt = m_datManager->parse_ffna_texture_file(ti);
            if (dt.width > 0 && dt.height > 0) {
                auto hr = map_renderer->GetTextureManager()->CreateTextureFromRGBA(
                    dt.width, dt.height, dt.rgba_data.data(), &texId, decoded);
                if (SUCCEEDED(hr) && texId >= 0)
                    return map_renderer->GetTextureManager()->GetTexture(texId);
            }
        }
        return nullptr;
    };

    // --- Sky mesh ---
    std::vector<uint16_t> skyIndices{ sky_bg_idx, sky_clouds0_idx, sky_clouds1_idx, sky_sun_idx };
    std::vector<ID3D11ShaderResourceView*> skyTextures(skyIndices.size(), nullptr);
    for (size_t i = 0; i < skyIndices.size(); i++)
        skyTextures[i] = loadEnvTexture(skyIndices[i]);

    if (!skyTextures.empty()) {
        int skyMeshId = map_renderer->GetMeshManager()->AddGwSkyCylinder(67723.75f / 2.0f, 33941.0f);
        map_renderer->SetSkyMeshId(skyMeshId);
        map_renderer->GetMeshManager()->SetMeshShouldCull(skyMeshId, false);
        map_renderer->SetMeshShouldRender(skyMeshId, false);

        const auto& mapBounds = m_mapFile.map_info_chunk.map_bounds;
        float cx = (mapBounds.map_min_x + mapBounds.map_max_x) / 2.0f;
        float cz = (mapBounds.map_min_z + mapBounds.map_max_z) / 2.0f;

        XMFLOAT4X4 skyWorld;
        XMStoreFloat4x4(&skyWorld, XMMatrixTranslation(cx, map_renderer->GetSkyHeight(), cz));
        PerObjectCB skyObj;
        skyObj.world = skyWorld;
        for (int i = 0; i < (int)skyTextures.size(); i++) {
            skyObj.texture_indices[i / 4][i % 4] = 0;
            skyObj.texture_types[i / 4][i % 4] = skyTextures[i] == nullptr ? 0xFF : 0;
        }
        skyObj.num_uv_texture_pairs = (uint32_t)skyTextures.size();
        map_renderer->GetMeshManager()->UpdateMeshPerObjectData(skyMeshId, skyObj);
        map_renderer->GetMeshManager()->SetTexturesForMesh(skyMeshId, skyTextures, 3);
    }

    // --- Water mesh ---
    std::vector<uint16_t> waterTexIndices{ water_color_idx, water_distort_idx };
    std::vector<ID3D11ShaderResourceView*> waterTextures(waterTexIndices.size(), nullptr);
    for (size_t i = 0; i < waterTexIndices.size(); i++)
        waterTextures[i] = loadEnvTexture(waterTexIndices[i]);

    if (!waterTextures.empty()) {
        int waterMeshId = map_renderer->GetMeshManager()->AddGwSkyCircle(70000, PixelShaderType::Water);
        map_renderer->SetWaterMeshId(waterMeshId);
        map_renderer->GetMeshManager()->SetMeshShouldCull(waterMeshId, false);
        map_renderer->SetMeshShouldRender(waterMeshId, false);

        const auto& mapBounds = m_mapFile.map_info_chunk.map_bounds;
        float cx = (mapBounds.map_min_x + mapBounds.map_max_x) / 2.0f;
        float cz = (mapBounds.map_min_z + mapBounds.map_max_z) / 2.0f;

        XMFLOAT4X4 waterWorld;
        XMStoreFloat4x4(&waterWorld, XMMatrixTranslation(cx, 0, cz));
        PerObjectCB waterObj;
        waterObj.world = waterWorld;
        for (int i = 0; i < (int)waterTextures.size(); i++) {
            waterObj.texture_indices[i / 4][i % 4] = 0;
            waterObj.texture_types[i / 4][i % 4] = waterTextures[i] == nullptr ? 0xFF : 0;
        }
        waterObj.num_uv_texture_pairs = (uint32_t)waterTextures.size();
        map_renderer->GetMeshManager()->UpdateMeshPerObjectData(waterMeshId, waterObj);
        map_renderer->GetMeshManager()->SetTexturesForMesh(waterMeshId, waterTextures, 0);
        map_renderer->GetMeshManager()->SetTexturesForMesh(
            waterMeshId, { map_renderer->GetWaterFresnelLUTSRV() }, 3);
    }

    // --- Terrain textures ---
    auto& terrainTexNames = m_mapFile.terrain_texture_filenames.array;
    std::vector<DatTexture> terrainDatTextures;
    for (size_t i = 0; i < terrainTexNames.size(); i++)
    {
        auto decoded = decode_filename(terrainTexNames[i].filename.id0, terrainTexNames[i].filename.id1);
        if (decoded == 0x25e09 || decoded == 0x00028615 || decoded == 0x46db6)
            continue;

        auto mit = m_hashIndex->find(decoded);
        if (mit != m_hashIndex->end()) {
            DatTexture dt = m_datManager->parse_ffna_texture_file(mit->second.at(0));
            if (dt.width > 0 && dt.height > 0)
                terrainDatTextures.push_back(dt);
        }
    }

    if (terrainDatTextures.empty())
    {
        m_errorMsg = std::format("Unable to load map for map_id {} (dat ID 0x{:X}). No terrain textures.",
                                 m_replayCtx.mapId, datFileHash);
        m_loadingPhase = LoadingPhase::Error;
        return;
    }

    std::vector<void*> rawPtrs;
    for (auto& dt : terrainDatTextures)
        rawPtrs.push_back(dt.rgba_data.data());

    const auto terrainTexId = map_renderer->GetTextureManager()->AddTextureArray(
        rawPtrs, terrainDatTextures[0].width, terrainDatTextures[0].height,
        DXGI_FORMAT_B8G8R8A8_UNORM, static_cast<int>(datFileHash), true);

    // --- Terrain mesh ---
    auto terrain = std::make_unique<Terrain>(
        m_mapFile.terrain_chunk.terrain_x_dims,
        m_mapFile.terrain_chunk.terrain_y_dims,
        m_mapFile.terrain_chunk.terrain_heightmap,
        m_mapFile.terrain_chunk.terrain_texture_indices_maybe,
        m_mapFile.terrain_chunk.terrain_shadow_map,
        m_mapFile.map_info_chunk.map_bounds);
    map_renderer->SetTerrain(terrain.get(), terrainTexId);

    // Water properties
    if (!envChunk.env_sub_chunk6.empty()) {
        size_t wi = (selWaterIdx < envChunk.env_sub_chunk6.size()) ? selWaterIdx : 0u;
        const EnvSubChunk7* wind = nullptr;
        if (!envChunk.env_sub_chunk7.empty()) {
            size_t wii = (selWindIdx < envChunk.env_sub_chunk7.size()) ? selWindIdx : 0u;
            wind = &envChunk.env_sub_chunk7[wii];
        }
        map_renderer->UpdateWaterProperties(envChunk.env_sub_chunk6[wi], wind);
    }

    // Cloud mesh
    if (!envFilenames.filenames.empty()) {
        auto* cloudTex = loadEnvTexture(0);
        if (cloudTex) {
            int cloudId = map_renderer->GetMeshManager()->AddGwSkyCircle(100000.0f);
            map_renderer->SetCloudsMeshId(cloudId);
            map_renderer->GetMeshManager()->SetMeshShouldCull(cloudId, true);
            map_renderer->SetMeshShouldRender(cloudId, false);

            float cx = (terrain->m_bounds.map_min_x + terrain->m_bounds.map_max_x) / 2.0f;
            float cz = (terrain->m_bounds.map_min_z + terrain->m_bounds.map_max_z) / 2.0f;

            XMFLOAT4X4 cWorld;
            XMStoreFloat4x4(&cWorld, XMMatrixTranslation(cx, terrain->m_bounds.map_max_y + 2400, cz));
            PerObjectCB cObj;
            cObj.world = cWorld;
            cObj.texture_indices[0][0] = 0;
            cObj.texture_types[0][0] = 0;
            cObj.num_uv_texture_pairs = 1;
            map_renderer->GetMeshManager()->UpdateMeshPerObjectData(cloudId, cObj);
            map_renderer->GetMeshManager()->SetTexturesForMesh(cloudId, { cloudTex }, 3);
        }
    }

    // Fog and clear color
    if (!envChunk.env_sub_chunk2.empty()) {
        size_t fi = (env8 && env8->fog_settings_index < envChunk.env_sub_chunk2.size())
            ? env8->fog_settings_index : 0u;
        const auto& sub2 = envChunk.env_sub_chunk2[fi];
        XMFLOAT4 clearColor{
            sub2.fog_red / 255.0f, sub2.fog_green / 255.0f, sub2.fog_blue / 255.0f, 1.0f
        };
        float fogStart = static_cast<float>(sub2.fog_distance_start);
        float fogEndRaw = static_cast<float>(sub2.fog_distance_end);
        float fogEnd = (fogEndRaw > fogStart + 1.0f) ? fogEndRaw : (fogStart + 1.0f);
        map_renderer->SetFogStart(fogStart);
        map_renderer->SetFogEnd(fogEnd);
        map_renderer->SetFogStartY(static_cast<float>(sub2.fog_z_start_maybe));
        map_renderer->SetFogEndY(static_cast<float>(sub2.fog_z_end_maybe));
        map_renderer->SetClearColor(clearColor);
    }
    map_renderer->SetSkyHeight(0);

    m_terrain = std::move(terrain);

    // Prepare for prop loading phases
    m_totalPropFilenames = static_cast<int>(m_mapFile.prop_filenames_chunk.array.size()
        + m_mapFile.more_filnames_chunk.array.size());
    m_totalPropInstances = static_cast<int>(m_mapFile.props_info_chunk.prop_array.props_info.size());
    m_propModelLoadIndex = 0;
    m_propPlaceIndex = 0;
    m_propModelFiles.clear();
    m_propModelFiles.reserve(m_totalPropFilenames);
    m_propModelFileHashes.clear();
    m_propModelFileHashes.reserve(m_totalPropFilenames);
    m_meshIdToPropIndex.clear();

    AssetBlacklist::Get().LoadForMap(m_replayCtx.mapId);

    m_loadTiming.initSec = std::chrono::duration<double>(LoadClock::now() - m_phaseStartTime).count();
    OutputDebugStringA(std::format("[ReplayLoad] Init: {:.3f}s\n", m_loadTiming.initSec).c_str());

    m_loadProgress = 0.05f;
    m_loadingPhase = LoadingPhase::PropModels;
    m_phaseStartTime = LoadClock::now();
}

// ---------------------------------------------------------------------------
// Loading phase: parse prop model files from DAT (batched)
// ---------------------------------------------------------------------------

void ReplayWindow::StepLoadPropModels()
{
    const auto& propFN = m_mapFile.prop_filenames_chunk.array;
    const auto& moreFN = m_mapFile.more_filnames_chunk.array;
    int total = m_totalPropFilenames;

    auto frameStart = LoadClock::now();
    int processed = 0;

    while (m_propModelLoadIndex < total)
    {
        int i = m_propModelLoadIndex;
        const auto& fn = (i < (int)propFN.size())
            ? propFN[i]
            : moreFN[i - (int)propFN.size()];

        auto decoded = decode_filename(fn.filename.id0, fn.filename.id1);
        auto mit = m_hashIndex->find(decoded);
        if (mit != m_hashIndex->end()) {
            m_datManager->EnsureTypeClassified(mit->second.at(0));
            auto type = m_datManager->get_MFT()[mit->second.at(0)].type;
            if (type == FFNA_Type2) {
                m_propModelFiles.emplace_back(m_datManager->parse_ffna_model_file(mit->second.at(0)));
                m_propModelFileHashes.push_back(static_cast<uint32_t>(decoded));
            }
        }

        m_propModelLoadIndex++;
        processed++;

        if (processed >= kPropModelBatchSize) {
            auto elapsed = std::chrono::duration<float, std::milli>(LoadClock::now() - frameStart).count();
            if (elapsed > kLoadFrameBudgetMs)
                break;
        }
    }

    if (total > 0)
        m_loadProgress = 0.05f + 0.25f * (static_cast<float>(m_propModelLoadIndex) / total);

    if (m_propModelLoadIndex >= total)
    {
        m_loadTiming.propModelSec += std::chrono::duration<double>(LoadClock::now() - m_phaseStartTime).count();
        OutputDebugStringA(std::format("[ReplayLoad] PropModels: {:.3f}s\n", m_loadTiming.propModelSec).c_str());

        m_loadProgress = 0.30f;
        m_loadingPhase = LoadingPhase::PlaceProps;
        m_phaseStartTime = LoadClock::now();
    }
}

// ---------------------------------------------------------------------------
// Loading phase: place prop instances (batched)
// ---------------------------------------------------------------------------

void ReplayWindow::StepPlaceProps()
{
    auto* map_renderer = m_mapRenderer.get();
    const auto& propsInfo = m_mapFile.props_info_chunk.prop_array.props_info;
    int total = m_totalPropInstances;

    auto frameStart = LoadClock::now();
    int processed = 0;

    while (m_propPlaceIndex < total)
    {
        int i = m_propPlaceIndex++;
        processed++;
        PropInfo prop_info = propsInfo[i];

        do {
            if (prop_info.filename_index >= m_propModelFiles.size()) break;
            auto* modelFilePtr = std::get_if<FFNA_ModelFile>(&m_propModelFiles[prop_info.filename_index]);
            if (!modelFilePtr || !modelFilePtr->parsed_correctly) break;

            const auto& geom = modelFilePtr->geometry_chunk;
            std::vector<Mesh> propMeshes;
            for (size_t j = 0; j < geom.models.size(); j++)
            {
                AMAT_file amat;
                if (!modelFilePtr->AMAT_filenames_chunk.texture_filenames.empty()) {
                    int subIdx = geom.models[j].unknown;
                    if (!geom.tex_and_vertex_shader_struct.uts0.empty())
                        subIdx %= (int)geom.tex_and_vertex_shader_struct.uts0.size();
                    const auto& uts1 = geom.uts1[subIdx % geom.uts1.size()];
                    int amatIdx = ((uts1.some_flags0 >> 8) & 0xFF) % (int)modelFilePtr->AMAT_filenames_chunk.texture_filenames.size();
                    auto amatFn = modelFilePtr->AMAT_filenames_chunk.texture_filenames[amatIdx];
                    auto amatHash = decode_filename(amatFn.id0, amatFn.id1);
                    auto aIt = m_hashIndex->find(amatHash);
                    if (aIt != m_hashIndex->end())
                        amat = m_datManager->parse_amat_file(aIt->second.at(0));
                }
                Mesh mesh = modelFilePtr->GetMesh((int)j, amat);
                if (mesh.indices.size() % 3 == 0)
                    propMeshes.push_back(mesh);
            }
            if (propMeshes.empty()) break;

            // Load textures for this model
            std::vector<int> textureIds;
            if (modelFilePtr->textures_parsed_correctly) {
                for (size_t t = 0; t < modelFilePtr->texture_filenames_chunk.texture_filenames.size(); t++) {
                    auto tf = modelFilePtr->texture_filenames_chunk.texture_filenames[t];
                    auto decoded = decode_filename(tf.id0, tf.id1);
                    int texId = map_renderer->GetTextureManager()->GetTextureIdByHash(decoded);
                    if (texId >= 0) { textureIds.push_back(texId); continue; }
                    auto mit = m_hashIndex->find(decoded);
                    if (mit != m_hashIndex->end()) {
                        DatTexture dt = m_datManager->parse_ffna_texture_file(mit->second.at(0));
                        if (dt.width > 0 && dt.height > 0) {
                            map_renderer->GetTextureManager()->CreateTextureFromRGBA(
                                dt.width, dt.height, dt.rgba_data.data(), &texId, decoded);
                        }
                        textureIds.push_back(texId);
                    }
                }
            }

            // Remap per-mesh texture indices
            std::vector<std::vector<int>> perMeshTexIds(propMeshes.size());
            for (size_t k = 0; k < propMeshes.size(); k++) {
                std::vector<uint8_t> remappedIndices;
                for (size_t ti = 0; ti < propMeshes[k].tex_indices.size(); ti++) {
                    int idx = std::min((int)propMeshes[k].tex_indices[ti], (int)textureIds.size() - 1);
                    if (idx >= 0 && idx < (int)textureIds.size()) {
                        perMeshTexIds[k].push_back(textureIds[idx]);
                        remappedIndices.push_back((uint8_t)ti);
                    }
                }
                propMeshes[k].tex_indices = remappedIndices;
            }

            // Build per-object constant buffers with correct transform
            std::vector<PerObjectCB> perObjectCBs(propMeshes.size());
            for (size_t j = 0; j < propMeshes.size(); j++) {
                XMFLOAT3 translation(prop_info.x, prop_info.y, prop_info.z);
                XMFLOAT3 vec1{ prop_info.f4, -prop_info.f6, prop_info.f5 };
                XMFLOAT3 vec2{ prop_info.sin_angle, -prop_info.f9, prop_info.cos_angle };

                XMVECTOR v2 = XMLoadFloat3(&vec1);
                XMVECTOR v3 = XMLoadFloat3(&vec2);
                XMVECTOR v1 = XMVector3Cross(v3, v2);
                v1 = XMVector3Normalize(v1);
                v2 = XMVector3Normalize(v2);
                v3 = XMVector3Normalize(v3);

                auto rotation_matrix = XMMATRIX(
                    -XMVectorGetX(v1), -XMVectorGetY(v1),  XMVectorGetZ(v1), 0.0f,
                     XMVectorGetX(v2),  XMVectorGetY(v2),  XMVectorGetZ(v2), 0.0f,
                    -XMVectorGetX(v3), -XMVectorGetY(v3),  XMVectorGetZ(v3), 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f);

                float scale = prop_info.scaling_factor;
                XMMATRIX scaling_matrix = XMMatrixScaling(scale, scale, scale);
                XMMATRIX translation_matrix = XMMatrixTranslationFromVector(XMLoadFloat3(&translation));
                XMMATRIX transform = scaling_matrix * XMMatrixTranspose(rotation_matrix) * translation_matrix;
                XMStoreFloat4x4(&perObjectCBs[j].world, transform);

                auto& mesh = propMeshes[j];
                if (mesh.uv_coord_indices.size() == mesh.tex_indices.size() &&
                    mesh.uv_coord_indices.size() < MAX_NUM_TEX_INDICES &&
                    modelFilePtr->textures_parsed_correctly) {
                    perObjectCBs[j].num_uv_texture_pairs = (uint32_t)mesh.uv_coord_indices.size();
                    for (size_t k = 0; k < mesh.uv_coord_indices.size(); k++) {
                        perObjectCBs[j].uv_indices[k / 4][k % 4] = (uint32_t)mesh.uv_coord_indices[k];
                        perObjectCBs[j].texture_indices[k / 4][k % 4] = (uint32_t)mesh.tex_indices[k];
                        perObjectCBs[j].blend_flags[k / 4][k % 4] = (uint32_t)mesh.blend_flags[k];
                        perObjectCBs[j].texture_types[k / 4][k % 4] = (uint32_t)mesh.texture_types[k];
                    }
                }
            }

            auto pst = geom.unknown_tex_stuff1.empty() ? PixelShaderType::OldModel : PixelShaderType::NewModel;
            auto meshIds = map_renderer->AddProp(propMeshes, perObjectCBs, (uint32_t)i, pst);

            if (modelFilePtr->textures_parsed_correctly) {
                for (size_t l = 0; l < meshIds.size() && l < perMeshTexIds.size(); l++) {
                    map_renderer->GetMeshManager()->SetTexturesForMesh(
                        meshIds[l], map_renderer->GetTextureManager()->GetTextures(perMeshTexIds[l]), 3);
                }
            }

            if (prop_info.filename_index < m_propModelFileHashes.size())
            {
                uint32_t fileHash = m_propModelFileHashes[prop_info.filename_index];
                uint32_t segHash = 0;
                size_t   segFallback = SIZE_MAX;

                if (m_replayCtx.datMapId == 0x334A2 && fileHash == 0x2E13A)
                {
                    segHash = 0x0000000;
                    segFallback = 1;
                }
                else if (m_replayCtx.datMapId == 0x26625 && fileHash == 0x285EA)
                {
                    segHash = 0x35E6AE29;
                    segFallback = 1;
                }

                if (segFallback != SIZE_MAX)
                {
                    SetupAnimatedProp(i, *modelFilePtr, fileHash,
                                      propMeshes, perObjectCBs, meshIds,
                                      perMeshTexIds, pst, segHash, segFallback);
                }

                // Imperial Isle: hide submesh 0 for gate frame props (only show submesh 1)
                if (m_replayCtx.datMapId == 0x28736 && fileHash == 0x2D74A &&
                    meshIds.size() > 1)
                {
                    map_renderer->GetMeshManager()->SetMeshShouldRender(meshIds[0], false);
                }

                if (m_replayCtx.datMapId == 0x28784)
                {
                    uint8_t doorType = 0;
                    if (fileHash == 0x2873C || fileHash == 0x2873B || fileHash == 0x2646B)
                        doorType = 1;
                    else if (fileHash == 0x1F1EE)
                        doorType = 2;

                    if (doorType != 0)
                    {
                        SetupAnimatedProp(i, *modelFilePtr, fileHash,
                                          propMeshes, perObjectCBs, meshIds,
                                          perMeshTexIds, pst, 0x303419C9, 2);

                        auto& animProps = m_mapRenderer->GetAnimatedProps();
                        if (!animProps.empty())
                        {
                            auto& last = animProps.back();
                            last.doorType = doorType;
                            last.openSegmentIndex  = SIZE_MAX;
                            last.closeSegmentIndex = SIZE_MAX;

                            if (last.clip)
                            {
                                const auto& segs = last.clip->animationSegments;
                                for (size_t s = 0; s < segs.size(); s++)
                                {
                                    if (segs[s].hash == 0x303419C9) last.openSegmentIndex  = s;
                                    if (segs[s].hash == 0x31D3EDC8) last.closeSegmentIndex = s;
                                }
                                if (last.openSegmentIndex == SIZE_MAX)  last.openSegmentIndex  = 2;
                                if (last.closeSegmentIndex == SIZE_MAX) last.closeSegmentIndex = 3;
                            }

                            last.controller->SetSegment(last.closeSegmentIndex);
                            last.controller->SetLooping(false);
                            last.controller->SetTime(
                                static_cast<float>(last.clip->animationSegments[last.closeSegmentIndex].endTime));
                            last.controller->Pause();

                            if (doorType == 2)
                            {
                                for (int skip = static_cast<int>(last.meshes.size()) - 1; skip >= 0; skip--)
                                {
                                    if (skip == 3 || skip == 4)
                                    {
                                        last.meshes.erase(last.meshes.begin() + skip);
                                        if (skip < static_cast<int>(last.perObjectCBs.size()))
                                            last.perObjectCBs.erase(last.perObjectCBs.begin() + skip);
                                        if (skip < static_cast<int>(last.staticMeshIds.size()))
                                            m_mapRenderer->GetMeshManager()->SetMeshShouldRender(
                                                last.staticMeshIds[skip], true);
                                    }
                                }
                            }

                            m_doorAnimPropCount++;
                            OutputDebugStringA(
                                std::format("[DoorAnim] Prop {} set up as door type {} (hash 0x{:X})\n",
                                            i, doorType, fileHash).c_str());
                        }
                    }
                }

                // Imperial Isle doors
                if (m_replayCtx.datMapId == 0x28736)
                {
                    bool isImperialDoor =
                        fileHash == 0x2865D || fileHash == 0x2865B || fileHash == 0x28699;

                    if (isImperialDoor && fileHash != 0x28699)
                    {
                        SetupAnimatedProp(i, *modelFilePtr, fileHash,
                                          propMeshes, perObjectCBs, meshIds,
                                          perMeshTexIds, pst, 0x303419C9, 2);

                        auto& animProps = m_mapRenderer->GetAnimatedProps();
                        if (!animProps.empty())
                        {
                            auto& last = animProps.back();
                            last.doorType = (fileHash == 0x2865B) ? 4 : 3;
                            last.openSegmentIndex  = SIZE_MAX;
                            last.closeSegmentIndex = SIZE_MAX;

                            if (last.clip)
                            {
                                const auto& segs = last.clip->animationSegments;
                                for (size_t s = 0; s < segs.size(); s++)
                                {
                                    if (segs[s].hash == 0x303419C9) last.openSegmentIndex = s;
                                }
                                if (last.openSegmentIndex == SIZE_MAX) last.openSegmentIndex = 2;
                            }

                            last.controller->SetSegment(last.openSegmentIndex);
                            last.controller->SetTime(
                                static_cast<float>(last.clip->animationSegments[last.openSegmentIndex].startTime));
                            last.controller->SetLooping(false);
                            last.controller->Pause();

                            // submesh hiding for 0x2865B disabled for testing
                            // if (fileHash == 0x2865B)
                            // {
                            //     if (last.meshes.size() > 4) last.meshes[4] = nullptr;
                            //     if (last.meshes.size() > 3) last.meshes[3] = nullptr;
                            // }

                            m_doorAnimPropCount++;
                            {
                                std::ofstream dbg("door_debug.log", std::ios::app);
                                dbg << "[DoorAnim] Imperial prop " << i
                                    << " door type " << static_cast<int>(last.doorType)
                                    << " (hash 0x" << std::hex << fileHash << std::dec << ")\n";
                            }
                        }
                    }
                    else if (fileHash == 0x28699)
                    {
                        // Inline animated prop creation with bone-locking for submeshes 0-5
                        auto* device = m_deviceResources->GetD3DDevice();
                        auto mit = m_hashIndex->find(static_cast<int>(fileHash));
                        if (device && mit != m_hashIndex->end() && !mit->second.empty())
                        {
                            int mftIndex = mit->second.at(0);
                            uint8_t* animFileData = m_datManager->read_file(mftIndex);
                            if (animFileData)
                            {
                                size_t animFileSize = m_datManager->get_MFT()[mftIndex].uncompressedSize;
                                auto clipOpt = GW::Parsers::ParseAnimationFromFile(animFileData, animFileSize);
                                delete[] animFileData;

                                if (clipOpt && clipOpt->IsValid())
                                {
                                    auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*clipOpt));
                                    clip->BuildAnimationGroups();

                                    const auto& segments = clip->animationSegments;
                                    if (segments.size() >= 2)
                                    {
                                        size_t openSeg = SIZE_MAX;
                                        for (size_t s = 0; s < segments.size(); s++)
                                        {
                                            if (segments[s].hash == 0x303419C9) openSeg = s;
                                        }
                                        if (openSeg == SIZE_MAX) openSeg = 2;

                                        auto controller = std::make_shared<GW::Animation::AnimationController>();
                                        controller->Initialize(clip);
                                        controller->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
                                        controller->SetSegment(openSeg);
                                        controller->SetLooping(false);
                                        controller->SetPlaybackSpeed(100000.0f);
                                        controller->SetTime(static_cast<float>(segments[openSeg].startTime));
                                        controller->Pause();

                                        const auto& geomModels = modelFilePtr->geometry_chunk.models;
                                        size_t boneCount = clip->boneTracks.size();

                                        // First pass: build skinned verts for all submeshes
                                        // and find the center X of panel vertices (submeshes 6-7)
                                        struct SubmeshSkinData {
                                            std::vector<SkinnedGWVertex> verts;
                                        };
                                        std::vector<SubmeshSkinData> allSkinData(propMeshes.size());

                                        float panelMinX =  FLT_MAX, panelMaxX = -FLT_MAX;
                                        float panelMinZ =  FLT_MAX, panelMaxZ = -FLT_MAX;

                                        for (size_t j = 0; j < propMeshes.size(); j++)
                                        {
                                            const auto& mesh = propMeshes[j];
                                            AnimationPanelState::SubmeshBoneData boneData;
                                            std::vector<uint32_t> vertexBoneGroups;
                                            if (j < geomModels.size())
                                            {
                                                const auto& geomModel = geomModels[j];
                                                boneData = AnimationPanelState::ExtractBoneData(
                                                    geomModel.extra_data, geomModel.u0, geomModel.u1);
                                                vertexBoneGroups.reserve(geomModel.vertices.size());
                                                for (const auto& mv : geomModel.vertices)
                                                    vertexBoneGroups.push_back(mv.group);
                                            }

                                            allSkinData[j].verts = AnimationPanelState::CreateSkinnedVertices(
                                                mesh, boneData, vertexBoneGroups, boneCount,
                                                clip->hierarchyMode, j);

                                            if (j >= 6)
                                            {
                                                for (const auto& sv : allSkinData[j].verts)
                                                {
                                                    panelMinX = std::min(panelMinX, sv.position.x);
                                                    panelMaxX = std::max(panelMaxX, sv.position.x);
                                                    panelMinZ = std::min(panelMinZ, sv.position.z);
                                                    panelMaxZ = std::max(panelMaxZ, sv.position.z);
                                                }
                                            }
                                        }

                                        float extentX = panelMaxX - panelMinX;
                                        float extentZ = panelMaxZ - panelMinZ;
                                        bool splitOnX = (extentX >= extentZ);
                                        float splitCenter = splitOnX
                                            ? (panelMinX + panelMaxX) * 0.5f
                                            : (panelMinZ + panelMaxZ) * 0.5f;

                                        // Debug log to file
                                        {
                                            std::ofstream dbg("door_debug.log", std::ios::app);
                                            dbg << "\n=== 0x28699 prop " << i << " ===\n";
                                            dbg << "boneCount=" << boneCount
                                                << "  outputBoneCount=" << clip->GetOutputBoneCount()
                                                << "  hierarchyMode=" << static_cast<int>(clip->hierarchyMode) << "\n";
                                            dbg << "panelX=[" << panelMinX << ", " << panelMaxX
                                                << "]  panelZ=[" << panelMinZ << ", " << panelMaxZ << "]\n";
                                            dbg << "splitAxis=" << (splitOnX ? "X" : "Z")
                                                << "  splitCenter=" << splitCenter << "\n";

                                            dbg << "  -- Bone bind positions (basePosition) --\n";
                                            for (size_t b = 0; b < boneCount; b++)
                                            {
                                                const auto& bp = clip->boneTracks[b].basePosition;
                                                int32_t parent = (b < clip->boneParents.size()) ? clip->boneParents[b] : -1;
                                                dbg << "  bone " << b << " : pos=("
                                                    << bp.x << ", " << bp.y << ", " << bp.z
                                                    << ") parent=" << parent << "\n";
                                            }

                                            dbg << "  -- Bone skinning matrices at segment start (closed) --\n";
                                            const auto& matrices = controller->GetBoneMatrices();
                                            for (size_t b = 0; b < std::min(matrices.size(), (size_t)10); b++)
                                            {
                                                const auto& m = matrices[b];
                                                dbg << "  bone " << b << " 4x4:\n";
                                                dbg << "    [" << m._11 << ", " << m._12 << ", " << m._13 << ", " << m._14 << "]\n";
                                                dbg << "    [" << m._21 << ", " << m._22 << ", " << m._23 << ", " << m._24 << "]\n";
                                                dbg << "    [" << m._31 << ", " << m._32 << ", " << m._33 << ", " << m._34 << "]\n";
                                                dbg << "    [" << m._41 << ", " << m._42 << ", " << m._43 << ", " << m._44 << "]\n";
                                            }

                                            // Also evaluate at segment end (open state)
                                            controller->SetTime(static_cast<float>(segments[openSeg].endTime));
                                            const auto& openMatrices = controller->GetBoneMatrices();
                                            dbg << "  -- Bone skinning matrices at segment end (open) --\n";
                                            for (size_t b = 0; b < std::min(openMatrices.size(), (size_t)10); b++)
                                            {
                                                const auto& m = openMatrices[b];
                                                dbg << "  bone " << b << " 4x4:\n";
                                                dbg << "    [" << m._11 << ", " << m._12 << ", " << m._13 << ", " << m._14 << "]\n";
                                                dbg << "    [" << m._21 << ", " << m._22 << ", " << m._23 << ", " << m._24 << "]\n";
                                                dbg << "    [" << m._31 << ", " << m._32 << ", " << m._33 << ", " << m._34 << "]\n";
                                                dbg << "    [" << m._41 << ", " << m._42 << ", " << m._43 << ", " << m._44 << "]\n";
                                            }
                                            // Reset back to start
                                            controller->SetTime(static_cast<float>(segments[openSeg].startTime));

                                            for (size_t j = 0; j < allSkinData.size(); j++)
                                            {
                                                std::set<uint32_t> uniqueBones;
                                                for (const auto& sv : allSkinData[j].verts)
                                                    uniqueBones.insert(sv.boneIndices[0]);
                                                std::string boneList;
                                                for (uint32_t b : uniqueBones)
                                                    boneList += std::to_string(b) + " ";
                                                dbg << "  submesh " << j << " : "
                                                    << allSkinData[j].verts.size() << " verts, bones: [" << boneList << "]\n";
                                            }
                                        }

                                        // Second pass: apply bone overrides and create meshes
                                        std::vector<std::shared_ptr<AnimatedMeshInstance>> animatedMeshes;
                                        int rightCount = 0, leftCount = 0;
                                        for (size_t j = 0; j < propMeshes.size(); j++)
                                        {
                                            auto& skinnedVerts = allSkinData[j].verts;
                                            std::vector<uint32_t> meshIndices = propMeshes[j].indices;

                                            if (j <= 5)
                                            {
                                                for (auto& sv : skinnedVerts)
                                                    sv.SetSingleBone(static_cast<uint32_t>(boneCount));
                                            }
                                            else
                                            {
                                                size_t numTris = meshIndices.size() / 3;

                                                std::vector<bool> triIsLeft(numTris);
                                                for (size_t t = 0; t < numTris; t++)
                                                {
                                                    uint32_t i0 = meshIndices[t*3], i1 = meshIndices[t*3+1], i2 = meshIndices[t*3+2];
                                                    float centroid = splitOnX
                                                        ? (skinnedVerts[i0].position.x + skinnedVerts[i1].position.x + skinnedVerts[i2].position.x) / 3.0f
                                                        : (skinnedVerts[i0].position.z + skinnedVerts[i1].position.z + skinnedVerts[i2].position.z) / 3.0f;
                                                    triIsLeft[t] = (centroid <= splitCenter);
                                                }

                                                enum VertSide : uint8_t { NONE=0, RIGHT_ONLY=1, LEFT_ONLY=2, SHARED=3 };
                                                std::vector<uint8_t> vertSide(skinnedVerts.size(), NONE);
                                                for (size_t t = 0; t < numTris; t++)
                                                {
                                                    uint8_t side = triIsLeft[t] ? LEFT_ONLY : RIGHT_ONLY;
                                                    vertSide[meshIndices[t*3]]   |= side;
                                                    vertSide[meshIndices[t*3+1]] |= side;
                                                    vertSide[meshIndices[t*3+2]] |= side;
                                                }

                                                std::unordered_map<uint32_t, uint32_t> leftCopyMap;
                                                size_t origSize = skinnedVerts.size();
                                                for (uint32_t v = 0; v < static_cast<uint32_t>(origSize); v++)
                                                {
                                                    if (vertSide[v] == SHARED)
                                                    {
                                                        uint32_t newIdx = static_cast<uint32_t>(skinnedVerts.size());
                                                        skinnedVerts.push_back(skinnedVerts[v]);
                                                        leftCopyMap[v] = newIdx;
                                                    }
                                                }

                                                for (size_t t = 0; t < numTris; t++)
                                                {
                                                    if (triIsLeft[t])
                                                    {
                                                        for (int k = 0; k < 3; k++)
                                                        {
                                                            auto it = leftCopyMap.find(meshIndices[t*3+k]);
                                                            if (it != leftCopyMap.end())
                                                                meshIndices[t*3+k] = it->second;
                                                        }
                                                    }
                                                }

                                                for (uint32_t v = 0; v < static_cast<uint32_t>(skinnedVerts.size()); v++)
                                                {
                                                    bool isLeft = false;
                                                    if (v < origSize)
                                                        isLeft = (vertSide[v] == LEFT_ONLY);
                                                    else
                                                        isLeft = true;

                                                    if (isLeft)
                                                    {
                                                        uint32_t origBone = skinnedVerts[v].boneIndices[0];
                                                        if (origBone == 0 || origBone == 7)
                                                            skinnedVerts[v].SetSingleBone(9);
                                                        else if (origBone == 8)
                                                            skinnedVerts[v].SetSingleBone(11);
                                                        else
                                                            skinnedVerts[v].SetSingleBone(9);
                                                        leftCount++;
                                                    }
                                                    else
                                                    {
                                                        uint32_t origBone = skinnedVerts[v].boneIndices[0];
                                                        if (origBone == 7)
                                                            skinnedVerts[v].SetSingleBone(0);
                                                        rightCount++;
                                                    }
                                                }
                                            }

                                            auto animMesh = std::make_shared<AnimatedMeshInstance>(
                                                device, skinnedVerts, meshIndices, static_cast<int>(j));

                                            if (j < perMeshTexIds.size())
                                            {
                                                auto texSRVs = m_mapRenderer->GetTextureManager()->GetTextures(perMeshTexIds[j]);
                                                animMesh->SetTextures(texSRVs, 3);
                                            }

                                            animMesh->SetPerObjectData(perObjectCBs[j]);
                                            animatedMeshes.push_back(std::move(animMesh));
                                        }

                                        {
                                            std::ofstream dbg("door_debug.log", std::ios::app);
                                            dbg << "  panel triangle-split: leftVerts=" << leftCount
                                                << " rightVerts=" << rightCount << "\n";
                                            for (size_t j = 6; j < allSkinData.size(); j++)
                                            {
                                                std::map<uint32_t, int> boneCounts;
                                                for (const auto& sv : allSkinData[j].verts)
                                                    boneCounts[sv.boneIndices[0]]++;
                                                std::string detail;
                                                for (const auto& [b, c] : boneCounts)
                                                    detail += "bone" + std::to_string(b) + "=" + std::to_string(c) + " ";
                                                dbg << "  submesh " << j << " verts=" << allSkinData[j].verts.size()
                                                    << " after remap: " << detail << "\n";
                                            }
                                        }

                                        MapAnimatedProp prop;
                                        prop.controller     = controller;
                                        prop.clip           = clip;
                                        prop.meshes         = std::move(animatedMeshes);
                                        prop.perObjectCBs   = perObjectCBs;
                                        prop.staticMeshIds  = meshIds;
                                        prop.pixelShaderType = pst;
                                        prop.doorType        = 4;
                                        prop.openSegmentIndex  = openSeg;
                                        prop.closeSegmentIndex = SIZE_MAX;
                                        prop.mirrorBonePairs = {{0, 9}, {8, 11}};
                                        prop.doubleSided = true;

                                        m_mapRenderer->AddAnimatedProp(std::move(prop));
                                        m_doorAnimPropCount++;
                                        {
                                            std::ofstream dbg("door_debug.log", std::ios::app);
                                            dbg << "[DoorAnim] Imperial prop " << i
                                                << " door 0x28699 type 4 (position-split panels, mirror bone 0->9)\n";
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            for (int mid : meshIds)
                m_meshIdToPropIndex[mid] = static_cast<uint32_t>(i);

            auto& bl = AssetBlacklist::Get();
            uint32_t propIdx = static_cast<uint32_t>(i);

            if (bl.IsHidden(propIdx))
            {
                for (int mid : meshIds)
                    map_renderer->GetMeshManager()->SetMeshShouldRender(mid, false);
            }

            if (bl.HasAlphaOverride(propIdx))
            {
                float alpha = bl.GetAlpha(propIdx);
                for (int mid : meshIds)
                {
                    auto data = map_renderer->GetMeshManager()->GetMeshPerObjectData(mid);
                    if (data.has_value()) {
                        auto updated = data.value();
                        updated.mesh_alpha = alpha;
                        map_renderer->GetMeshManager()->UpdateMeshPerObjectData(mid, updated);
                    }
                }
            }
        } while (false);

        if (processed >= kPropPlaceBatchSize) {
            auto elapsed = std::chrono::duration<float, std::milli>(LoadClock::now() - frameStart).count();
            if (elapsed > kLoadFrameBudgetMs)
                break;
        }
    }

    if (total > 0) {
        float propRange = m_useAgentModels ? 0.60f : 0.70f;
        m_loadProgress = 0.30f + propRange * (static_cast<float>(m_propPlaceIndex) / total);
    }

    if (m_propPlaceIndex >= total)
    {
        if (!m_loadTiming.placePropsLogged) {
            m_loadTiming.placePropSec = std::chrono::duration<double>(LoadClock::now() - m_phaseStartTime).count();
            OutputDebugStringA(std::format("[ReplayLoad] PlaceProps: {:.3f}s\n", m_loadTiming.placePropSec).c_str());

            if (m_replayCtx.datMapId == 0x28784 || m_replayCtx.datMapId == 0x28736)
            {
                const char* mapLabel = (m_replayCtx.datMapId == 0x28784)
                    ? "Isle of Meditation" : "Imperial Isle";
                OutputDebugStringA(std::format(
                    "[DoorAnim] {}: {} door animated props created\n",
                    mapLabel, m_doorAnimPropCount).c_str());

                std::unordered_map<uint32_t, int> hashCounts;
                for (size_t h = 0; h < m_propModelFileHashes.size(); h++)
                    hashCounts[m_propModelFileHashes[h]]++;
                for (const auto& [hash, count] : hashCounts)
                    OutputDebugStringA(std::format(
                        "[DoorAnim]   model hash 0x{:X} x{}\n", hash, count).c_str());
            }

            m_loadTiming.placePropsLogged = true;
        }

        // Fire-and-forget: start async agent model loading, but don't block.
        // The background thread will continue during FadingOut and Ready phases.
        if (m_useAgentModels && !m_agentModelsLoaded && !m_agentModelsLoading) {
            if (m_agentsClassified)
                LoadAgentModelsAsync();
        }

        m_replayCtx.mapLoaded = true;
        m_loadProgress = 1.0f;
        m_loadingPhase = LoadingPhase::FadingOut;

        if (m_totalLoadTimerStarted) {
            m_loadTiming.totalSec = std::chrono::duration<double>(LoadClock::now() - m_totalLoadStartTime).count();
            OutputDebugStringA(std::format(
                "[ReplayLoad] TOTAL: {:.3f}s  (validate={:.3f} init={:.3f} propModel={:.3f} "
                "placeProps={:.3f})\n",
                m_loadTiming.totalSec,
                m_loadTiming.validateSec, m_loadTiming.initSec,
                m_loadTiming.propModelSec, m_loadTiming.placePropSec).c_str());
        }

        SetWindowTextW(m_hwnd, BuildWindowTitle(m_matchMeta).c_str());
    }
}

// ---------------------------------------------------------------------------
// Tick / Update / Render
// ---------------------------------------------------------------------------

void ReplayWindow::Tick()
{
    if (!m_alive) return;

    // Poll async agent snapshot parsing
    PollAgentParseCompletion(m_replayCtx);

    // Poll async StoC event parsing
    PollStoCParseCompletion(m_replayCtx);

    // Once both parsers are done, consolidate maxReplayTime from all sources
    if (m_replayCtx.agentsLoaded && m_replayCtx.stocLoaded && !m_replayTimeConsolidated)
    {
        m_replayTimeConsolidated = true;

        float maxT = m_replayCtx.maxReplayTime;

        // Official match end from infos.json
        if (m_matchMeta.match_end_time_ms > 0)
            maxT = std::max(maxT, m_matchMeta.match_end_time_ms / 1000.f);

        // Max StoC event timestamps
        auto& sd = m_replayCtx.stocData;
        if (!sd.combat.empty())
            maxT = std::max(maxT, sd.combat.back().time);
        if (!sd.skill.empty())
            maxT = std::max(maxT, sd.skill.back().time);
        if (!sd.attackSkill.empty())
            maxT = std::max(maxT, sd.attackSkill.back().time);
        if (!sd.lordDamage.empty())
            maxT = std::max(maxT, sd.lordDamage.back().time);
        if (!sd.jumbo.empty())
            maxT = std::max(maxT, sd.jumbo.back().time);

        m_replayCtx.maxReplayTime = maxT;

        // Compute display offset so displayed time matches match_duration
        // (match_duration excludes the ~60s GvG grace period)
        m_displayTimeOffset = 0.f;
        if (!m_matchMeta.match_duration.empty())
        {
            float displayDur = 0.f;
            int mm = 0, ss = 0;
            if (sscanf(m_matchMeta.match_duration.c_str(), "%d:%d", &mm, &ss) == 2)
                displayDur = static_cast<float>(mm * 60 + ss);
            if (displayDur > 0.f && maxT > displayDur)
                m_displayTimeOffset = maxT - displayDur;
        }
    }

    // Once both agents and StoC are loaded, split recycled agent IDs and classify
    if (m_replayCtx.agentsLoaded && m_replayCtx.stocLoaded
        && !m_agentsClassified && !m_replayCtx.agents.empty())
    {
        // Build set of flag agent IDs to skip during splitting (flags are handled
        // by FlagTimelineBuilder via StoC events, not by agent incarnation tracking)
        std::unordered_set<int> flagAgentSkipIds;
        {
            std::unordered_set<uint32_t> flagItemIds;
            for (auto& e : m_replayCtx.stocData.flagEvents.items)
                flagItemIds.insert(static_cast<uint32_t>(e.item_id));
            for (auto& [id, ard] : m_replayCtx.agents) {
                if (ard.snapshots.empty()) continue;
                uint32_t iid = ard.snapshots.front().item_id;
                if (iid != 0 && (IsFlagItemId(m_replayCtx.mapId, iid) || flagItemIds.count(iid)))
                    flagAgentSkipIds.insert(id);
            }
        }
        SplitRecycledAgents(m_replayCtx.agents, m_replayCtx.stocData.lifecycle, &flagAgentSkipIds);

        // Build incarnation reverse map before classification so we can remap events
        m_incarnationMap.clear();
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            if (ard.originalAgentId >= 0)
                m_incarnationMap[ard.originalAgentId].push_back(id);
        }

        // Remap lifecycle events from original agent_ids to synthetic incarnations
        for (auto& ev : m_replayCtx.stocData.lifecycle)
        {
            int resolved = ResolveAgentAtTime(ev.agent_id, ev.time);
            if (resolved != ev.agent_id)
                ev.agent_id = resolved;
        }

        ClassifyAgents(m_replayCtx.agents, m_matchMeta, m_replayCtx.mapId);

        // Re-classify flag agents using FLAG_ITEM data (catches dynamic item_ids
        // not in the hardcoded IsFlagItemId table, e.g. respawned flags)
        if (!m_replayCtx.stocData.flagEvents.items.empty()) {
            std::unordered_set<uint32_t> flagItemIds;
            for (auto& e : m_replayCtx.stocData.flagEvents.items)
                flagItemIds.insert(static_cast<uint32_t>(e.item_id));
            for (auto& [id, ard] : m_replayCtx.agents) {
                if (ard.type != AgentType::Unknown) continue;
                if (ard.snapshots.empty()) continue;
                if (flagItemIds.count(ard.snapshots.front().item_id)) {
                    ard.type = AgentType::Flag;
                    ard.categoryName = "Flag";
                }
            }
        }

        m_sortedAgentIds.reserve(m_replayCtx.agents.size());
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            m_sortedAgentIds.push_back(id);
            switch (ard.type) {
            case AgentType::Player:  m_playerIds.push_back(id);  break;
            case AgentType::NPC:     m_npcIds.push_back(id);     break;
            case AgentType::Gadget:            m_gadgetIds.push_back(id);  break;
            case AgentType::ObeliskFlagStand: m_gadgetIds.push_back(id);  break;
            case AgentType::Flag:              m_flagIds.push_back(id);    break;
            case AgentType::Spirit:            m_spiritIds.push_back(id);  break;
            case AgentType::Item:              m_itemIds.push_back(id);    break;
            default:                           m_unknownIds.push_back(id);  break;
            }
        }
        std::sort(m_sortedAgentIds.begin(), m_sortedAgentIds.end());
        std::sort(m_playerIds.begin(),  m_playerIds.end());
        std::sort(m_npcIds.begin(),     m_npcIds.end());
        std::sort(m_gadgetIds.begin(),  m_gadgetIds.end());
        std::sort(m_flagIds.begin(),    m_flagIds.end());
        std::sort(m_spiritIds.begin(),  m_spiritIds.end());
        std::sort(m_itemIds.begin(),    m_itemIds.end());
        std::sort(m_unknownIds.begin(), m_unknownIds.end());

        // Build modelId -> PlayerMeta* lookup for player metadata
        std::unordered_map<uint32_t, const PlayerMeta*> modelToPlayer;
        for (auto& [partyId, party] : m_matchMeta.parties)
            for (auto& p : party.players)
                modelToPlayer[static_cast<uint32_t>(p.model_id)] = &p;

        auto ProfShort = [](int id) -> const char* {
            switch (id) {
            case 1: return "W"; case 2: return "R"; case 3: return "Mo";
            case 4: return "N"; case 5: return "Me"; case 6: return "E";
            case 7: return "A"; case 8: return "Rt"; case 9: return "P";
            case 10: return "D"; default: return "?";
            }
        };

        for (int id : m_playerIds)
        {
            auto& ard = m_replayCtx.agents[id];
            auto it = modelToPlayer.find(ard.modelId);
            if (it != modelToPlayer.end())
            {
                const PlayerMeta* pm = it->second;
                ard.playerNumber  = pm->player_number;
                ard.primaryProf   = pm->primary;
                ard.secondaryProf = pm->secondary;
                ard.playerLevel   = pm->level;
                ard.isFemale      = (pm->gender == "Female");

                char buf[256];
                snprintf(buf, sizeof(buf), "%s/%s%d %s",
                         ProfShort(pm->primary), ProfShort(pm->secondary),
                         pm->level, ard.playerName.c_str());
                ard.partyBarLabel = buf;
            }
            else
            {
                ard.partyBarLabel = ard.playerName;
            }

            if (ard.teamId == 1)
                m_team1PlayerIds.push_back(id);
            else if (ard.teamId == 2)
                m_team2PlayerIds.push_back(id);
        }

        auto sortByPlayerNum = [&](std::vector<int>& ids) {
            std::sort(ids.begin(), ids.end(), [&](int a, int b) {
                return m_replayCtx.agents[a].playerNumber
                     < m_replayCtx.agents[b].playerNumber;
            });
        };
        sortByPlayerNum(m_team1PlayerIds);
        sortByPlayerNum(m_team2PlayerIds);

        // Parse guild tags from folder name: "..._[tag1]vs[tag2]"
        {
            const auto& fn = m_matchMeta.folder_name;
            auto vs = fn.find("]vs[");
            if (vs != std::string::npos) {
                auto open1 = fn.rfind('[', vs);
                auto close2 = fn.find(']', vs + 4);
                if (open1 != std::string::npos && close2 != std::string::npos) {
                    m_folderTag1 = fn.substr(open1 + 1, vs - open1 - 1);
                    m_folderTag2 = fn.substr(vs + 4, close2 - (vs + 4));
                }
            }
        }

        // Find guild by tag (from folder name)
        auto FindGuildByTag = [&](const std::string& tag) -> const GuildMeta* {
            if (tag.empty()) return nullptr;
            for (const auto& [id, gm] : m_matchMeta.guilds)
                if (gm.tag == tag) return &gm;
            return nullptr;
        };

        // Build guild header strings
        // Prefer folder-name tags (authoritative from GW match list) over
        // the majority-of-players heuristic which fails when guests outnumber
        // the home guild's own members.
        auto BuildGuildHeader = [&](const std::string& partyId, const std::string& folderTag) -> std::string {
            // Try folder-name tag first
            auto* fg = FindGuildByTag(folderTag);
            if (fg) return fg->name + " [" + fg->tag + "]";

            // Fallback: majority heuristic
            auto pit = m_matchMeta.parties.find(partyId);
            if (pit == m_matchMeta.parties.end()) return "Unknown";
            std::map<int, int> guildCounts;
            for (auto& p : pit->second.players)
                if (p.guild_id > 0) guildCounts[p.guild_id]++;
            int bestGuildId = 0, bestCount = 0;
            for (auto& [gid, cnt] : guildCounts)
                if (cnt > bestCount) { bestGuildId = gid; bestCount = cnt; }
            if (bestGuildId == 0) return "Unknown";
            auto git = m_matchMeta.guilds.find(std::to_string(bestGuildId));
            if (git != m_matchMeta.guilds.end())
                return git->second.name + " [" + git->second.tag + "]";
            return "Unknown";
        };
        m_team1GuildHeader = BuildGuildHeader("1", m_folderTag1);
        m_team2GuildHeader = BuildGuildHeader("2", m_folderTag2);

        // Build NPC + Spirit team lists for Allies section
        auto NpcSortOrder = [](const std::string& cat) -> int {
            if (cat == "Guild Lord")    return 0;
            if (cat == "Bodyguard")     return 1;
            if (cat == "Knight")        return 2;
            if (cat == "Archer")        return 3;
            if (cat == "Footman")       return 4;
            return 5; // Pets, Spirits, other NPCs
        };

        for (int id : m_npcIds)
        {
            auto& ard = m_replayCtx.agents[id];
            ard.partyBarLabel = ard.categoryName;
            if (ard.teamId == 1)      m_team1NpcIds.push_back(id);
            else if (ard.teamId == 2) m_team2NpcIds.push_back(id);
        }
        for (int id : m_spiritIds)
        {
            auto& ard = m_replayCtx.agents[id];
            ard.partyBarLabel = ard.categoryName;
            if (ard.teamId == 1)      m_team1NpcIds.push_back(id);
            else if (ard.teamId == 2) m_team2NpcIds.push_back(id);
        }

        auto sortNpcs = [&](std::vector<int>& ids) {
            std::sort(ids.begin(), ids.end(), [&](int a, int b) {
                auto& aa = m_replayCtx.agents[a];
                auto& bb = m_replayCtx.agents[b];
                int oa = NpcSortOrder(aa.categoryName);
                int ob = NpcSortOrder(bb.categoryName);
                if (oa != ob) return oa < ob;
                return aa.agent_id < bb.agent_id;
            });
        };
        sortNpcs(m_team1NpcIds);
        sortNpcs(m_team2NpcIds);

        // Isle of Wurms: South Health Shrine gadget + CAPTURED_SHRINE jumbo timeline
        m_wurmsSouthShrineAgentId = -1;
        m_wurmsShrineCaptureEvents.clear();
        m_wurmsShrineSamples.clear();
        if (IsIsleOfWurmsMap(m_replayCtx.mapId))
        {
            for (int gid : m_gadgetIds)
            {
                auto itg = m_replayCtx.agents.find(gid);
                if (itg == m_replayCtx.agents.end()) continue;
                const auto& gard = itg->second;
                if (gard.snapshots.empty()) continue;
                const uint32_t gd = gard.snapshots.front().gadget_id;
                if (gd == 5988u || gard.categoryName == "Southern Health Shrine")
                {
                    m_wurmsSouthShrineAgentId = gid;
                    break;
                }
            }
            if (m_replayCtx.stocLoaded)
            {
                for (auto& jev : m_replayCtx.stocData.jumbo)
                {
                    if (jev.message == "CAPTURED_SHRINE")
                    {
                        int tm = 0;
                        if (jev.party_value == 1635021873) tm = 1;
                        else if (jev.party_value == 1635021874) tm = 2;
                        if (tm != 0)
                            m_wurmsShrineCaptureEvents.push_back({ jev.time, tm });
                    }
                    else if (jev.message == "NEUTRALIZED_SHRINE")
                    {
                        m_wurmsShrineCaptureEvents.push_back({ jev.time, 0 });
                    }
                }
                std::sort(m_wurmsShrineCaptureEvents.begin(), m_wurmsShrineCaptureEvents.end(),
                          [](const std::pair<float, int>& a, const std::pair<float, int>& b) {
                              return a.first < b.first;
                          });
            }
            PrecomputeShrineTimeline();
        }

        m_agentsClassified = true;
        LoadAutoCamSettings();
    }

    // Once both agents and StoC data are loaded, distribute MOVE_TO_POINT
    // events from the global StoC list into per-agent moveEvents vectors.
    if (m_agentsClassified && m_replayCtx.stocLoaded && !m_moveEventsBuilt)
    {
        for (auto& ev : m_replayCtx.stocData.agentMovement)
        {
            int resolvedId = ResolveAgentAtTime(ev.agent_id, ev.time);
            auto it = m_replayCtx.agents.find(resolvedId);
            if (it != m_replayCtx.agents.end())
            {
                it->second.moveEvents.push_back(
                    MoveToPointEvent{ ev.time, ev.x, ev.y });
            }
        }
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            std::sort(ard.moveEvents.begin(), ard.moveEvents.end(),
                      [](const MoveToPointEvent& a, const MoveToPointEvent& b) {
                          return a.time < b.time;
                      });
        }
        m_moveEventsBuilt = true;
    }

    // Build per-agent casting intervals from StoC skill/attack-skill events.
    // A SKILL_ACTIVATED opens an interval; SKILL_FINISHED / SKILL_STOPPED closes it.
    // INSTANT_SKILL_USED has no cast time so we skip it.
    if (m_agentsClassified && m_replayCtx.stocLoaded && !m_castIntervalsBuilt)
    {
        // Track open (unfinished) casts per caster_id
        std::unordered_map<int, CastInterval> openCasts;

        auto processStart = [&](int casterId, float time, int skillId) {
            openCasts[casterId] = CastInterval{ time, time, skillId };
        };
        auto processEnd = [&](int casterId, float time) {
            auto oc = openCasts.find(casterId);
            if (oc != openCasts.end()) {
                oc->second.end = time;
                int resolvedId = ResolveAgentAtTime(casterId, oc->second.start);
                auto it = m_replayCtx.agents.find(resolvedId);
                if (it != m_replayCtx.agents.end())
                    it->second.castHistory.push_back(oc->second);
                openCasts.erase(oc);
            }
        };

        for (auto& ev : m_replayCtx.stocData.skill)
        {
            if (ev.type == "SKILL_ACTIVATED")
                processStart(ev.caster_id, ev.time, ev.skill_id);
            else if (ev.type == "SKILL_FINISHED" || ev.type == "SKILL_STOPPED")
                processEnd(ev.caster_id, ev.time);
        }

        {
            auto& skillDb = GetSkillDatabase();
            auto implicitClose = [&](int casterId) {
                auto oc = openCasts.find(casterId);
                if (oc == openCasts.end()) return;
                const SkillInfo* si = skillDb.Get(oc->second.skillId);
                float act = (si && si->activation > 0.01f) ? si->activation : 0.f;
                oc->second.end = oc->second.start + act;
                int resolvedId = ResolveAgentAtTime(casterId, oc->second.start);
                auto it = m_replayCtx.agents.find(resolvedId);
                if (it != m_replayCtx.agents.end())
                    it->second.castHistory.push_back(oc->second);
                openCasts.erase(oc);
            };

            for (auto& ev : m_replayCtx.stocData.attackSkill)
            {
                if (ev.type == "ATTACK_SKILL_ACTIVATED")
                {
                    implicitClose(ev.caster_id);
                    processStart(ev.caster_id, ev.time, ev.skill_id);
                }
                else if (ev.type == "ATTACK_SKILL_FINISHED" || ev.type == "ATTACK_SKILL_STOPPED")
                    processEnd(ev.caster_id, ev.time);
            }
            // Flush remaining orphans
            std::vector<int> orphanIds;
            for (auto& [id, ci] : openCasts) orphanIds.push_back(id);
            for (int id : orphanIds) implicitClose(id);
            openCasts.clear();
        }

        // Sort each agent's cast history by start time
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            std::sort(ard.castHistory.begin(), ard.castHistory.end(),
                      [](const CastInterval& a, const CastInterval& b) {
                          return a.start < b.start;
                      });
        }
        m_castIntervalsBuilt = true;
    }

    // Build per-agent skill use timeline for icon display + laser lines
    if (m_castIntervalsBuilt && !m_skillUseTimelineBuilt)
    {
        auto resolveTarget = [](int tid, int cid) { return (tid == 0) ? cid : tid; };

        // Track open casts to pair ACTIVATED→FINISHED with their target
        struct OpenCast { float start; int skillId; int targetId; };
        std::unordered_map<int, OpenCast> openCasts;

        // 1) Process StoC skill events (activated, instant, finished, stopped)
        for (auto& ev : m_replayCtx.stocData.skill)
        {
            if (ev.type == "SKILL_ACTIVATED" && ev.skill_id > 0)
            {
                int tid = resolveTarget(ev.target_id, ev.caster_id);
                openCasts[ev.caster_id] = { ev.time, ev.skill_id, tid };
            }
            else if (ev.type == "SKILL_FINISHED" || ev.type == "SKILL_STOPPED")
            {
                auto oc = openCasts.find(ev.caster_id);
                if (oc != openCasts.end())
                {
                    bool stopped = (ev.type == "SKILL_STOPPED");
                    auto it = m_replayCtx.agents.find(ev.caster_id);
                    if (it != m_replayCtx.agents.end())
                        it->second.skillUseHistory.push_back(
                            { oc->second.start, ev.time, 0.f, oc->second.skillId,
                              oc->second.targetId, false, stopped });
                    openCasts.erase(oc);
                }
            }
            else if (ev.type == "INSTANT_SKILL_USED" && ev.skill_id > 0)
            {
                int tid = resolveTarget(ev.target_id, ev.caster_id);
                auto it = m_replayCtx.agents.find(ev.caster_id);
                if (it != m_replayCtx.agents.end())
                    it->second.skillUseHistory.push_back(
                        { ev.time, ev.time, 0.f, ev.skill_id, tid, true, false });
            }
        }

        // 2) Process StoC attack skill events
        openCasts.clear();
        {
            auto& skillDb = GetSkillDatabase();
            auto implicitCloseSkillUse = [&](int casterId) {
                auto oc = openCasts.find(casterId);
                if (oc == openCasts.end()) return;
                const SkillInfo* si = skillDb.Get(oc->second.skillId);
                float act = (si && si->activation > 0.01f) ? si->activation : 0.f;
                float endT = oc->second.start + act;
                bool isInstant = (act < 0.01f);
                auto it = m_replayCtx.agents.find(casterId);
                if (it != m_replayCtx.agents.end())
                    it->second.skillUseHistory.push_back(
                        { oc->second.start, endT, 0.f, oc->second.skillId,
                          oc->second.targetId, isInstant, false });
                openCasts.erase(oc);
            };

            for (auto& ev : m_replayCtx.stocData.attackSkill)
            {
                if (ev.type == "ATTACK_SKILL_ACTIVATED" && ev.skill_id > 0)
                {
                    implicitCloseSkillUse(ev.caster_id);
                    int tid = resolveTarget(ev.target_id, ev.caster_id);
                    openCasts[ev.caster_id] = { ev.time, ev.skill_id, tid };
                }
                else if (ev.type == "ATTACK_SKILL_FINISHED" || ev.type == "ATTACK_SKILL_STOPPED")
                {
                    bool stopped = (ev.type == "ATTACK_SKILL_STOPPED");
                    auto oc = openCasts.find(ev.caster_id);
                    if (oc != openCasts.end())
                    {
                        auto it = m_replayCtx.agents.find(ev.caster_id);
                        if (it != m_replayCtx.agents.end())
                            it->second.skillUseHistory.push_back(
                                { oc->second.start, ev.time, 0.f, oc->second.skillId,
                                  oc->second.targetId, false, stopped });
                        openCasts.erase(oc);
                    }
                }
            }
            // Flush remaining orphans using activation time from DB
            std::vector<int> orphanIds;
            for (auto& [id, oc] : openCasts) orphanIds.push_back(id);
            for (int id : orphanIds) implicitCloseSkillUse(id);
            openCasts.clear();
        }

        // 3) Sort each agent's timeline by startTime
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            std::sort(ard.skillUseHistory.begin(), ard.skillUseHistory.end(),
                      [](const SkillUseEvent& a, const SkillUseEvent& b) {
                          return a.startTime < b.startTime;
                      });
        }

        // 4) Compute fullCastDuration per skillId from successful (non-cancelled) casts
        std::unordered_map<int, float> skillFullDur;
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            for (auto& ev : ard.skillUseHistory)
            {
                if (ev.isInstant || ev.wasCancelled) continue;
                float d = ev.endTime - ev.startTime;
                if (d > 0.001f)
                {
                    auto it = skillFullDur.find(ev.skillId);
                    if (it == skillFullDur.end() || d > it->second)
                        skillFullDur[ev.skillId] = d;
                }
            }
        }
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            for (auto& ev : ard.skillUseHistory)
            {
                if (ev.isInstant) continue;
                auto it = skillFullDur.find(ev.skillId);
                if (it != skillFullDur.end())
                    ev.fullCastDuration = it->second;
                else
                    ev.fullCastDuration = ev.endTime - ev.startTime;
            }
        }

        // 5) Match INTERRUPTED combat events to cancelled SkillUseEvents
        for (const auto& ce : m_replayCtx.stocData.combat)
        {
            if (ce.type != "INTERRUPTED") continue;
            int victimId = ce.caster_id;
            int intSkillId = (int)ce.value;
            auto ait = m_replayCtx.agents.find(victimId);
            if (ait == m_replayCtx.agents.end()) continue;
            auto& hist = ait->second.skillUseHistory;
            // Binary search for the closest cancelled event before this interrupt time
            int lo2 = 0, hi2 = (int)hist.size() - 1, best2 = -1;
            while (lo2 <= hi2) {
                int mid2 = lo2 + (hi2 - lo2) / 2;
                if (hist[mid2].endTime <= ce.time + 0.5f) { best2 = mid2; lo2 = mid2 + 1; }
                else hi2 = mid2 - 1;
            }
            for (int i = best2; i >= 0; --i)
            {
                float dt = ce.time - hist[i].endTime;
                if (dt > 3.0f) break;
                if (dt < -0.5f) continue;
                if (!hist[i].wasCancelled || hist[i].wasInterrupted) continue;
                if (intSkillId > 0 && hist[i].skillId != intSkillId) continue;
                hist[i].wasInterrupted = true;
                break;
            }
        }

        // 6) Precompute recharge durations per cast event (fast recast detection)
        // NOTE: Fast recast detection may produce false positives if recharge-reduction
        // skills are active (e.g. Quickening Zephyr). In standard GvG this is rare.
        // Known limitation — no fix planned.
        {
            auto& skillDb = GetSkillDatabase();
            for (auto& [id, ard] : m_replayCtx.agents)
            {
                // Group events by skill ID
                std::unordered_map<int, std::vector<int>> skillEventIndices;
                for (int i = 0; i < (int)ard.skillUseHistory.size(); i++)
                {
                    auto& ev = ard.skillUseHistory[i];
                    if (ev.wasCancelled) continue;
                    skillEventIndices[ev.skillId].push_back(i);
                }

                for (auto& [sid, indices] : skillEventIndices)
                {
                    const SkillInfo* si = skillDb.IsLoaded() ? skillDb.Get(sid) : nullptr;
                    float normalRecharge = si ? si->recharge : 0.f;

                    for (int j = 0; j < (int)indices.size(); j++)
                    {
                        auto& ev = ard.skillUseHistory[indices[j]];
                        float castEnd = ev.isInstant ? ev.startTime : ev.endTime;

                        if (j + 1 < (int)indices.size())
                        {
                            auto& nextEv = ard.skillUseHistory[indices[j + 1]];
                            float expectedNext = castEnd + normalRecharge;
                            if (nextEv.startTime < expectedNext - 0.5f && normalRecharge > 0.f)
                            {
                                ev.wasFastRecast = true;
                                ev.rechargeDuration = nextEv.startTime - castEnd;
                            }
                            else
                            {
                                ev.rechargeDuration = normalRecharge;
                            }
                        }
                        else
                        {
                            ev.rechargeDuration = normalRecharge;
                        }
                    }
                }
            }
        }

        // Sort each agent's skillUseHistory by startTime so binary searches
        // and cursor-based audio scanning work correctly.
        for (auto& [id, ard] : m_replayCtx.agents) {
            std::sort(ard.skillUseHistory.begin(), ard.skillUseHistory.end(),
                [](const SkillUseEvent& a, const SkillUseEvent& b) {
                    return a.startTime < b.startTime;
                });
        }

        m_skillUseTimelineBuilt = true;
    }

    // Build knockdown intervals from snapshot is_knocked transitions
    if (m_agentsClassified && !m_knockdownIntervalsBuilt)
    {
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            ard.knockdownIntervals.clear();
            bool wasKnocked = false;
            float kdStart = 0.f;
            for (auto& snap : ard.snapshots)
            {
                if (snap.is_knocked && !wasKnocked)
                    kdStart = snap.time;
                else if (!snap.is_knocked && wasKnocked)
                    ard.knockdownIntervals.push_back({ kdStart, snap.time });
                wasKnocked = snap.is_knocked;
            }
            if (wasKnocked && !ard.snapshots.empty())
                ard.knockdownIntervals.push_back({ kdStart, ard.snapshots.back().time });
        }
        m_knockdownIntervalsBuilt = true;
    }

    // Build combat log from merged StoC streams
    if (m_skillUseTimelineBuilt && m_replayCtx.stocLoaded && !m_combatLogBuilt)
    {
        m_combatLog.clear();

        auto findMaxHp = [&](int agentId, float t) -> uint32_t {
            auto it = m_replayCtx.agents.find(agentId);
            if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) return 0;
            auto& snaps = it->second.snapshots;

            // Binary search for the last snapshot at or before t
            int idx = 0;
            if (t >= snaps.back().time) {
                idx = (int)snaps.size() - 1;
            } else if (t > snaps.front().time) {
                int lo = 0, hi = (int)snaps.size() - 1;
                while (lo < hi) { int mid = lo + (hi - lo + 1) / 2; if (snaps[mid].time <= t) lo = mid; else hi = mid - 1; }
                idx = lo;
            }

            if (snaps[idx].max_hp > 0) return snaps[idx].max_hp;

            // max_hp unknown at this time — scan forward for the earliest known value
            for (int i = idx + 1; i < (int)snaps.size(); ++i)
            {
                if (snaps[i].max_hp > 0) return snaps[i].max_hp;
            }
            // Also scan backward in case only earlier snapshots have it
            for (int i = idx - 1; i >= 0; --i)
            {
                if (snaps[i].max_hp > 0) return snaps[i].max_hp;
            }
            return 0;
        };

        // Pass 1: skill events -> primary rows
        {
            struct OpenCast { float start; int skillId; int targetId; };
            std::unordered_map<int, OpenCast> open;

            for (auto& ev : m_replayCtx.stocData.skill)
            {
                if (ev.type == "SKILL_ACTIVATED" && ev.skill_id > 0)
                {
                    open[ev.caster_id] = { ev.time, ev.skill_id, ev.target_id };
                }
                else if (ev.type == "SKILL_FINISHED" || ev.type == "SKILL_STOPPED")
                {
                    auto oc = open.find(ev.caster_id);
                    if (oc != open.end())
                    {
                        CombatLogRow r;
                        r.time = oc->second.start;
                        r.casterId = ev.caster_id;
                        r.targetId = oc->second.targetId;
                        r.skillId = oc->second.skillId;
                        r.cancelled = (ev.type == "SKILL_STOPPED");
                        r.category = CombatLogCategory::Skill;
                        m_combatLog.push_back(std::move(r));
                        open.erase(oc);
                    }
                }
                else if (ev.type == "INSTANT_SKILL_USED" && ev.skill_id > 0)
                {
                    CombatLogRow r;
                    r.time = ev.time;
                    r.casterId = ev.caster_id;
                    r.targetId = ev.target_id;
                    r.skillId = ev.skill_id;
                    r.category = CombatLogCategory::Skill;
                    m_combatLog.push_back(std::move(r));
                }
            }

            open.clear();
            for (auto& ev : m_replayCtx.stocData.attackSkill)
            {
                if (ev.type == "ATTACK_SKILL_ACTIVATED" && ev.skill_id > 0)
                {
                    // Close prior open cast for this caster (ranged attack skills
                    // often have no FINISHED event).
                    auto oc = open.find(ev.caster_id);
                    if (oc != open.end())
                    {
                        CombatLogRow r;
                        r.time = oc->second.start;
                        r.casterId = ev.caster_id;
                        r.targetId = oc->second.targetId;
                        r.skillId = oc->second.skillId;
                        r.category = CombatLogCategory::Skill;
                        m_combatLog.push_back(std::move(r));
                        open.erase(oc);
                    }
                    open[ev.caster_id] = { ev.time, ev.skill_id, ev.target_id };
                }
                else if (ev.type == "ATTACK_SKILL_FINISHED" || ev.type == "ATTACK_SKILL_STOPPED")
                {
                    auto oc = open.find(ev.caster_id);
                    if (oc != open.end())
                    {
                        CombatLogRow r;
                        r.time = oc->second.start;
                        r.casterId = ev.caster_id;
                        r.targetId = oc->second.targetId;
                        r.skillId = oc->second.skillId;
                        r.cancelled = (ev.type == "ATTACK_SKILL_STOPPED");
                        r.category = CombatLogCategory::Skill;
                        m_combatLog.push_back(std::move(r));
                        open.erase(oc);
                    }
                }
            }
            // Close remaining orphan attack skill casts
            for (auto& [casterId, oc] : open)
            {
                CombatLogRow r;
                r.time = oc.start;
                r.casterId = casterId;
                r.targetId = oc.targetId;
                r.skillId = oc.skillId;
                r.category = CombatLogCategory::Skill;
                m_combatLog.push_back(std::move(r));
            }
            open.clear();
        }

        // Sort skill rows from Pass 1 so backward search reliably finds
        // the most recent cast when merging damage events.
        std::sort(m_combatLog.begin(), m_combatLog.end(),
            [](const CombatLogRow& a, const CombatLogRow& b) { return a.time < b.time; });

        // Pass 2: combat events -> enrich skill rows or create standalone rows
        {
            std::vector<bool> matched(m_combatLog.size(), false);
            const int skillCount = (int)m_combatLog.size();

            for (auto& ce : m_replayCtx.stocData.combat)
            {
                if (ce.IsDamageOrHeal())
                {
                    int bestIdx = -1;
                    float bestDt = 99.f;

                    for (int i = skillCount - 1; i >= 0; --i)
                    {
                        if (matched[i]) continue;
                        auto& r = m_combatLog[i];
                        if (r.category != CombatLogCategory::Skill) continue;
                        if (r.cancelled) continue;
                        if (r.casterId != ce.caster_id) continue;

                        bool targetMatch = (r.targetId == ce.target_id);
                        bool targetOpen  = (r.targetId <= 0 || r.targetId == r.casterId);
                        if (!targetMatch && !targetOpen) continue;

                        float dt = ce.time - r.time;
                        if (dt < -0.1f || dt > 1.5f) continue;

                        if (targetMatch && dt < bestDt) {
                            bestDt = dt;
                            bestIdx = i;
                        } else if (bestIdx < 0 && targetOpen && dt < bestDt) {
                            bestDt = dt;
                            bestIdx = i;
                        }
                    }

                    if (bestIdx >= 0)
                    {
                        auto& r = m_combatLog[bestIdx];
                        if (r.targetId <= 0 || r.targetId == r.casterId)
                            r.targetId = ce.target_id;
                        r.valuePct = ce.value;
                        uint32_t mhp = findMaxHp(ce.target_id, ce.time);
                        r.valueAbs = (mhp > 0) ? (int)(ce.value * mhp) : 0;
                        r.category = (ce.value < 0.f) ? CombatLogCategory::Damage
                                                      : CombatLogCategory::Heal;
                        matched[bestIdx] = true;
                    }
                    else
                    {
                        CombatLogRow r;
                        r.time = ce.time;
                        r.casterId = ce.caster_id;
                        r.targetId = ce.target_id;
                        r.valuePct = ce.value;
                        uint32_t mhp = findMaxHp(ce.target_id, ce.time);
                        r.valueAbs = (mhp > 0) ? (int)(ce.value * mhp) : 0;
                        r.category = (ce.value < 0.f) ? CombatLogCategory::Damage
                                                      : CombatLogCategory::Heal;
                        m_combatLog.push_back(std::move(r));
                    }
                }
                else if (ce.type == "INTERRUPTED")
                {
                    // ce.caster_id = the victim (whose cast was interrupted)
                    // ce.target_id = the interrupter
                    // ce.value     = skill ID of the interrupted spell
                    int interruptedSkillId = (int)ce.value;
                    bool merged = false;
                    bool passedNewerCast = false;
                    for (int i = skillCount - 1; i >= 0; --i)
                    {
                        auto& r = m_combatLog[i];
                        float dt = ce.time - r.time;
                        if (dt > 4.0f) break;  // too old, stop scanning

                        if (r.casterId != ce.caster_id) continue;
                        if (r.category != CombatLogCategory::Skill) continue;

                        // If we already passed a newer cast from this caster
                        // (cancelled or not), any older cancelled row is stale.
                        if (passedNewerCast) continue;

                        if (!r.cancelled || r.interrupted) {
                            passedNewerCast = true;
                            continue;
                        }

                        if (interruptedSkillId > 0 && r.skillId != interruptedSkillId) {
                            passedNewerCast = true;
                            continue;
                        }

                        if (dt < -0.2f || dt > 3.0f) continue;

                        r.interrupted = true;
                        r.interrupterId = ce.target_id;
                        merged = true;
                        break;
                    }
                    if (!merged)
                    {
                        CombatLogRow r;
                        r.time = ce.time;
                        r.casterId = ce.caster_id;
                        r.targetId = ce.target_id;
                        r.skillId = interruptedSkillId;
                        r.category = CombatLogCategory::Interrupt;
                        m_combatLog.push_back(std::move(r));
                    }
                }
                else if (ce.type == "KNOCKED_DOWN")
                {
                    CombatLogRow r;
                    r.time = ce.time;
                    r.casterId = ce.caster_id;
                    r.targetId = ce.target_id;
                    r.category = CombatLogCategory::KnockDown;
                    m_combatLog.push_back(std::move(r));
                }
                else
                {
                    CombatLogRow r;
                    r.time = ce.time;
                    r.casterId = ce.caster_id;
                    r.targetId = ce.target_id;
                    r.category = CombatLogCategory::Other;
                    r.eventType = ce.type;
                    m_combatLog.push_back(std::move(r));
                }
            }
        }

        // Pass 3: basic attacks
        for (auto& ev : m_replayCtx.stocData.basicAttack)
        {
            if (ev.type == "ATTACK_STARTED")
            {
                CombatLogRow r;
                r.time = ev.time;
                r.casterId = ev.caster_id;
                r.targetId = ev.target_id;
                r.category = CombatLogCategory::BasicAttack;
                m_combatLog.push_back(std::move(r));
            }
        }

        // Pass 4: death events from snapshot is_dead transitions
        for (auto& [agentId, ard] : m_replayCtx.agents)
        {
            if (ard.type != AgentType::Player) continue;
            for (size_t si = 1; si < ard.snapshots.size(); ++si)
            {
                if (ard.snapshots[si].is_dead && !ard.snapshots[si - 1].is_dead)
                {
                    CombatLogRow r;
                    r.time = ard.snapshots[si].time;
                    r.casterId = agentId;
                    r.category = CombatLogCategory::Death;
                    m_combatLog.push_back(std::move(r));
                }
            }
        }

        // Pass 5: jumbo messages (flag captures, morale boosts, etc.)
        for (auto& ev : m_replayCtx.stocData.jumbo)
        {
            CombatLogRow r;
            r.time = ev.time;
            int jTeam = (ev.party_value == 1635021873) ? 1
                      : (ev.party_value == 1635021874) ? 2 : 0;
            // NEUTRALIZED_SHRINE party_value identifies the team that lost
            // ownership, not the team that performed the neutralize — invert.
            if (ev.message == "NEUTRALIZED_SHRINE" && jTeam > 0)
                jTeam = (jTeam == 1) ? 2 : 1;
            r.jumboTeam = jTeam;
            r.eventType = ev.message;
            r.category = CombatLogCategory::Jumbo;
            m_combatLog.push_back(std::move(r));
        }

        std::sort(m_combatLog.begin(), m_combatLog.end(),
            [](const CombatLogRow& a, const CombatLogRow& b) { return a.time < b.time; });
        m_combatLogBuilt = true;
    }

    // Build flag timeline from StoC flag_events.txt (before BuildTimelineData
    // so flag return events can be included in the UI timeline)
    if (m_agentsClassified && m_replayCtx.stocLoaded && !m_flagTimelineBuilt)
        BuildFlagTimeline();

    if (m_combatLogBuilt && !m_timeline.computed)
        BuildTimelineData();

    if (m_combatLogBuilt && !m_lordDamageBuilt)
        BuildLordDamageData();

    if (m_agentsClassified && m_replayCtx.stocLoaded && !m_bundleCarryBuilt)
        BuildBundleCarryTimeline();

    // Auto-load saved calibration transform for this map, or fall back to
    // WebGL-derived defaults if no saved data exists.
    if (!m_calibrationLoaded && m_replayCtx.mapLoaded)
    {
        bool found = false;
        MapTransform saved = LoadMapTransform(m_replayCtx.mapId, &found);
        m_replayCtx.mapTransform = found ? saved : GetDefaultMapTransform();
        m_calibrationLoaded = true;
    }

    // Set up obelisk flag stand 3D model once timeline + calibration are ready
    if (m_flagTimelineBuilt && m_calibrationLoaded &&
        m_loadingPhase == LoadingPhase::Ready && !m_obeliskModelLoaded)
        SetupObeliskFlagStand();

    // Set up tower flag stand 3D model once timeline + calibration are ready
    if (m_flagTimelineBuilt && m_calibrationLoaded &&
        m_loadingPhase == LoadingPhase::Ready && !m_towerModelLoaded)
        SetupTowerFlagStand();

    // Set up gate lock animated levers (Isle of Meditation)
    if (m_flagTimelineBuilt && m_calibrationLoaded &&
        m_loadingPhase == LoadingPhase::Ready && !m_gateLockModelsLoaded)
        SetupGateLockProps();

    if (m_flagTimelineBuilt && m_calibrationLoaded &&
        m_loadingPhase == LoadingPhase::Ready && !m_imperialGateLockLoaded)
        SetupImperialGateLockProps();

    m_timer.Tick([this]()
    {
        if (m_loadingPhase == LoadingPhase::Ready)
            Update(m_timer.GetElapsedSeconds() * 1000.0);
    });

    // Playback engine: advance timeline when playing
    if (m_replayCtx.isPlaying && m_loadingPhase == LoadingPhase::Ready)
    {
        float dt = static_cast<float>(m_timer.GetElapsedSeconds());
        float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
        m_debugTimeline += dt * m_replayCtx.playbackSpeed;

        if (m_debugTimeline >= maxT)
        {
            if (m_replayCtx.loopPlayback) {
                m_debugTimeline = 0.f;
            } else {
                m_debugTimeline = maxT;
                m_replayCtx.isPlaying = false;
            }
        }
    }

    switch (m_loadingPhase)
    {
    case LoadingPhase::Validate:
        StepValidate();
        RenderLoadingScreen();
        break;

    case LoadingPhase::Init:
        StepLoadInit();
        RenderLoadingScreen();
        break;

    case LoadingPhase::PropModels:
        StepLoadPropModels();
        RenderLoadingScreen();
        break;

    case LoadingPhase::PlaceProps:
        StepPlaceProps();
        RenderLoadingScreen();
        break;

    case LoadingPhase::FadingOut:
        ProgressiveAgentModelPump();
        RenderLoadingScreen();
        break;

    case LoadingPhase::Ready:
        ProgressiveAgentModelPump();
        Render();
        break;

    case LoadingPhase::Error:
        SetWindowTextA(m_hwnd, ("Replay - ERROR: " + m_errorMsg).c_str());
        RenderLoadingScreen();
        break;
    }
}

void ReplayWindow::Update(double elapsedMs)
{
    float dt = static_cast<float>(elapsedMs / 1000.0);
    if (m_inputManager)
        m_inputManager->SetSuppressKeyPolling(m_imguiWantTextInput || m_clSkillSearchFocused);
    UpdateTopViewTransition(dt);
    if (!m_topViewActive)
        UpdateAutoCamera(dt);
    if (!m_topViewActive && !m_topViewTransitioning)
        UpdateFollowCamera(dt);
    m_mapRenderer->m_replayPlaybackSpeed = m_replayCtx.playbackSpeed;
    m_mapRenderer->Update(dt);

    UpdateDoorAnimations();
    UpdateObeliskFlagStand();
    UpdateTowerFlagStand();

    if (m_pipEnabled)
        UpdatePiPTarget();

    UpdateAudioPlayback(m_debugTimeline, dt);
}

void ReplayWindow::Render()
{
    ++m_frameCount;

    if (m_pipEnabled && m_pipResourcesReady && m_pipTargetAgent >= 0)
        RenderPiP();

    Clear();

    auto* pickingRTV = m_assetSelectionEnabled
        ? m_deviceResources->GetPickingRenderTargetView() : nullptr;

    m_mapRenderer->Render(
        m_deviceResources->GetRenderTargetView(),
        pickingRTV,
        m_deviceResources->GetDepthStencilView());

    // Picking readback: resolve and copy the picking RT to CPU-accessible staging
    if (m_assetSelectionEnabled)
    {
        auto* ctx = m_deviceResources->GetD3DDeviceContext();
        if (m_deviceResources->GetMsaaLevelIndex() > 0) {
            ctx->ResolveSubresource(
                m_deviceResources->GetPickingNonMsaaTexture(), 0,
                m_deviceResources->GetPickingRenderTarget(), 0,
                m_deviceResources->GetBackBufferFormat());
            ctx->CopyResource(
                m_deviceResources->GetPickingStagingTexture(),
                m_deviceResources->GetPickingNonMsaaTexture());
        } else {
            ctx->CopyResource(
                m_deviceResources->GetPickingStagingTexture(),
                m_deviceResources->GetPickingRenderTarget());
        }

        POINT cursor;
        GetCursorPos(&cursor);
        ScreenToClient(m_hwnd, &cursor);
        m_hoveredPropMeshId = m_mapRenderer->GetObjectId(
            m_deviceResources->GetPickingStagingTexture(), cursor.x, cursor.y);
    }

    DrawHeatmapOverlay();

    DrawFogOfWar();

    DrawAgentModels();
    DrawSkinnedAgentModels();

    DrawAgentCylinders();

    DrawImGuiOverlay();

    m_deviceResources->Present();
}

// ---------------------------------------------------------------------------
// Map name helpers
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// ImGui overlay: menu bar + debug windows
// ---------------------------------------------------------------------------

void ReplayWindow::DrawImGuiOverlay()
{
    if (!m_imguiInitialized)
        InitImGui();

    ImGuiContext* prevCtx = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_imguiContext);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    ImGui::NewFrame();

    // Top menu bar
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::BeginMenu("Preferences"))
            {
                if (ImGui::MenuItem("Shortcuts"))
                    m_showShortcutPreferences = true;
                if (ImGui::MenuItem("Interface"))
                    m_showInterfacePrefs = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Rate Match"))
            {
                int cur = MatchRatings::Get().GetRating(m_matchMeta.folder_name);
                for (int i = 1; i <= 5; i++)
                {
                    char label[16];
                    snprintf(label, sizeof(label), "%d Star%s", i, i > 1 ? "s" : "");
                    if (ImGui::MenuItem(label, nullptr, cur == i))
                        MatchRatings::Get().SetRating(m_matchMeta.folder_name, (cur == i) ? 0 : i);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear Rating", nullptr, false, cur > 0))
                    MatchRatings::Get().SetRating(m_matchMeta.folder_name, 0);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Close Replay"))
            {
                PostMessage(m_hwnd, WM_CLOSE, 0, 0);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Agent Overlay", nullptr, &m_showAgentOverlay);

            if (ImGui::MenuItem("Stylized Agent Icons",
                                nullptr, m_uiLayout.lodEnabled))
            {
                m_uiLayout.lodEnabled = !m_uiLayout.lodEnabled;
                SaveUILayout();
            }

            ImGui::Separator();
            ImGui::MenuItem("Skill Icons", nullptr, &m_showSkillIcons);
            ImGui::MenuItem("Skill Lasers", nullptr, &m_showSkillLasers);

            {
                const auto& hk = ReplayHotkeys::Get();
                auto kn = [](int k) { return ImGui::GetKeyName((ImGuiKey)k); };

                ImGui::MenuItem(std::format("Range Rings ({})", kn(hk.toggleRangeRings)).c_str(), nullptr, &m_showRangeRings);
                {
                    bool fogOn = (m_fogPerspective > 0);
                    if (ImGui::MenuItem(std::format("Fog of War ({})", kn(hk.toggleFogOfWar)).c_str(), nullptr, &fogOn)) {
                        if (fogOn) m_fogPerspective = m_fogLastActive;
                        else { m_fogLastActive = m_fogPerspective; m_fogPerspective = 0; m_fogPlayerAgent = -1; }
                    }
                }
                ImGui::MenuItem(std::format("Morale ({})", kn(hk.toggleMoralePanel)).c_str(), nullptr, &m_showMoralePanel);
                ImGui::MenuItem(std::format("Lord Damage ({})", kn(hk.toggleLordDamage)).c_str(), nullptr, &m_showLordDamagePanel);
                ImGui::MenuItem(std::format("Event Timeline ({})", kn(hk.toggleEventTimeline)).c_str(), nullptr, &m_showEventTimeline);
                ImGui::MenuItem(std::format("Auto Camera ({})", kn(hk.toggleAutoCamera)).c_str(), nullptr, &m_showAutoCameraPanel);
                {
                    bool tvOn = m_topViewActive;
                    if (ImGui::MenuItem(std::format("Top View ({})", kn(hk.toggleTopView)).c_str(), nullptr, &tvOn)) {
                        if (tvOn) EnterTopView(); else ExitTopView();
                    }
                }
                ImGui::MenuItem("Team 1 Party", nullptr, &m_showTeam1Party);
                ImGui::MenuItem("Team 2 Party", nullptr, &m_showTeam2Party);
                ImGui::MenuItem(std::format("Piano Roll ({})", kn(hk.togglePianoRoll)).c_str(), nullptr, &m_showPianoRoll);
                ImGui::Separator();

                ImGui::MenuItem(std::format("Heatmap ({})", kn(hk.toggleHeatmap)).c_str(), nullptr, &m_heatmapSettings.show);
            }

            ImGui::Separator();
            ImGui::MenuItem("Combat Log", nullptr, &m_showCombatLog);
            ImGui::MenuItem("Skill Analytics (BETA)", nullptr, &m_showSkillAnalytics);
            ImGui::MenuItem("Agent Names", nullptr, &m_showNameFilterPanel);
            ImGui::MenuItem("Split Camera", nullptr, &m_pipEnabled);
            ImGui::Separator();
            ImGui::MenuItem("Sound FX (BETA)", nullptr, &m_audioEnabled);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug"))
        {
            if (GuiGlobalConstants::IsDeveloperMode())
            {
                ImGui::MenuItem("Agent Data", nullptr, &m_showAgentDataWindow);
                ImGui::MenuItem("Map Calibration", nullptr, &m_showMapCalibrationWindow);
                ImGui::MenuItem("Interpolation", nullptr, &m_showInterpolationWindow);
                ImGui::MenuItem("StoC Events", nullptr, &m_showStoCWindow);
                ImGui::MenuItem("Auto Camera Debug", nullptr, &m_autoCamShowDebug);
            }
            ImGui::MenuItem("Audio Debug", nullptr, &m_showAudioDebug);
            if (GuiGlobalConstants::IsDeveloperMode())
            {
                ImGui::MenuItem("Flag Timeline", nullptr, &m_showFlagDebugWindow);
                ImGui::MenuItem("Assets", nullptr, &m_showAssetInspector);
                ImGui::MenuItem("Agent 3D Models", nullptr, &m_showAgentModelWindow);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools"))
        {
            ImGui::MenuItem("Drawing toolbar", nullptr, &m_annotationMgr.toolbar_visible);
            ImGui::MenuItem("Bookmarks",       nullptr, &m_annotationMgr.bookmarks_visible);
            ImGui::MenuItem("Notepad",         nullptr, &m_showNotepad);
            ImGui::EndMenu();
        }

        if (m_replayCtx.agentParseProgress && !m_replayCtx.agentsLoaded)
        {
            int done = m_replayCtx.agentParseProgress->files_done.load();
            int total = m_replayCtx.agentParseProgress->files_total.load();
            auto label = std::format("  Parsing agents... {}/{}", done, total);
            ImGui::TextDisabled("%s", label.c_str());
        }
        if (m_replayCtx.stocParseProgress && !m_replayCtx.stocLoaded)
        {
            int done = m_replayCtx.stocParseProgress->files_done.load();
            int total = m_replayCtx.stocParseProgress->files_total.load();
            auto label = std::format("  Parsing StoC... {}/{}", done, total);
            ImGui::TextDisabled("%s", label.c_str());
        }

        ImGui::EndMainMenuBar();
    }

    DrawTimelineController();
    DrawAgentModelLoadingBanner();
    DrawEventTimeline();

    if (m_showAgentDataWindow)
        DrawAgentDataWindow();

    if (m_showMapCalibrationWindow)
        DrawMapCalibrationWindow();

    if (m_showInterpolationWindow)
        DrawInterpolationWindow();

    if (m_showStoCWindow)
        DrawStoCWindow();

    if (m_showFlagDebugWindow)
        DrawFlagDebugWindow();

    if (m_showAssetInspector)
        DrawAssetInspectorWindow();

    if (m_showAgentModelWindow)
    {
        ImGui::SetNextWindowSize(ImVec2(300, 160), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Agent 3D Models", &m_showAgentModelWindow))
        {
            bool prevToggle = m_useAgentModels;
            ImGui::Checkbox("Enable 3D Agent Models", &m_useAgentModels);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Replace NPC/spirit cylinders with actual GW models");
            if (m_useAgentModels != prevToggle)
                GuiGlobalConstants::use_3d_agent_models = m_useAgentModels;

            if (m_useAgentModels && !m_agentModelsLoaded)
                LoadAgentModels();

            if (m_useAgentModels && !prevToggle && m_agentModelsLoaded)
            {
                // Turning on: meshes will be shown by DrawAgentModels()
            }
            if (!m_useAgentModels && prevToggle && m_agentModelsLoaded)
            {
                // Turning off: hide all agent model meshes
                auto* meshMgr = m_mapRenderer->GetMeshManager();
                for (auto& [agentId, meshIds] : m_agentMeshIds)
                    for (int mid : meshIds)
                        meshMgr->SetMeshShouldRender(mid, false);
            }

            ImGui::SliderFloat("Model Scale", &m_agentModelScale, 0.1f, 5.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Scale factor for agent 3D models");

            ImGui::Separator();
            if (m_agentModelsLoaded) {
                ImGui::Text("Loaded: %d unique models, %d agents",
                    (int)m_agentModelTemplates.size(), (int)m_agentMeshIds.size());

                if (ImGui::TreeNode("Agent Model Diagnostics"))
                {
                    if (ImGui::BeginTable("AgentModelDiag", 7,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                        ImVec2(0, 300)))
                    {
                        ImGui::TableSetupColumn("ID",      ImGuiTableColumnFlags_WidthFixed, 35);
                        ImGui::TableSetupColumn("Type",    ImGuiTableColumnFlags_WidthFixed, 55);
                        ImGui::TableSetupColumn("Mdl",     ImGuiTableColumnFlags_WidthFixed, 35);
                        ImGui::TableSetupColumn("Tm",      ImGuiTableColumnFlags_WidthFixed, 25);
                        ImGui::TableSetupColumn("Hash",    ImGuiTableColumnFlags_WidthFixed, 65);
                        ImGui::TableSetupColumn("Mesh#",   ImGuiTableColumnFlags_WidthFixed, 40);
                        ImGui::TableSetupColumn("Render Status", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        for (auto& [agentId, ard] : m_replayCtx.agents)
                        {
                            if (ard.type != AgentType::Player && ard.type != AgentType::NPC
                                && ard.type != AgentType::Spirit && ard.type != AgentType::Unknown)
                                continue;
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", agentId);
                            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", AgentTypeName(ard.type));
                            ImGui::TableSetColumnIndex(2); ImGui::Text("%u", ard.modelId);
                            ImGui::TableSetColumnIndex(3); ImGui::Text("%u", ard.teamId);
                            auto cacheIt = m_agentFileHashCache.find(agentId);
                            ImGui::TableSetColumnIndex(4);
                            if (cacheIt != m_agentFileHashCache.end())
                                ImGui::Text("0x%X", cacheIt->second);
                            else
                                ImGui::TextDisabled("none");
                            auto meshIt = m_agentMeshIds.find(agentId);
                            ImGui::TableSetColumnIndex(5);
                            if (meshIt != m_agentMeshIds.end())
                                ImGui::Text("%d", (int)meshIt->second.size());
                            else
                                ImGui::TextDisabled("-");
                            ImGui::TableSetColumnIndex(6);
                            auto statusIt = m_agentModelRenderStatus.find(agentId);
                            if (statusIt != m_agentModelRenderStatus.end())
                                ImGui::TextWrapped("%s", statusIt->second.c_str());
                            else
                                ImGui::TextDisabled("n/a");
                        }
                        ImGui::EndTable();
                    }

                    if (ImGui::TreeNode("Model 174 agents (all types)"))
                    {
                        for (auto& [agentId, ard] : m_replayCtx.agents)
                        {
                            if (ard.modelId != 174) continue;
                            ImGui::Text("Agent %d: type=%s team=%u amt=0x%X",
                                        agentId, AgentTypeName(ard.type), ard.teamId, ard.agentModelType);
                        }
                        ImGui::TreePop();
                    }

                    ImGui::TreePop();
                }
            } else if (m_useAgentModels) {
                ImGui::TextDisabled("Loading models...");
            } else {
                ImGui::TextDisabled("Enable to load models from DAT");
            }
        }
        ImGui::End();
    }

    if (m_showShortcutPreferences)
    {
        ImGui::OpenPopup("Shortcut Preferences");
        m_showShortcutPreferences = false;
    }
    DrawShortcutPreferences();

    DrawInterfacePreferences();

    DrawPartyWindows();
    DrawCombatLog();
    DrawSkillAnalyticsPanel();
    DrawSkillAnalyticsPlayerPopups();
    DrawPlayerInfoPanel();
    DrawPianoRollPanel();
    DrawNotepad();
    DrawNameFilterPanel();
    DrawPiPPanel();

    {
        auto lutGetter = [this](HeatmapPalette p) -> ID3D11ShaderResourceView* {
            return m_heatmapRenderer.GetLutSRV(p);
        };

        // Heatmap floating panel
        if (m_heatmapSettings.show)
        {
            std::vector<AgentMenuEntry> hmAgents;
            if (m_agentsClassified)
            {
                auto* dev = m_deviceResources->GetD3DDevice();
                for (int id : m_playerIds)
                {
                    auto it = m_replayCtx.agents.find(id);
                    if (it == m_replayCtx.agents.end()) continue;
                    const auto& ard = it->second;
                    std::string label = ard.playerName.empty()
                        ? ard.categoryName : ard.playerName;
                    ImTextureID icon = (ard.primaryProf > 0)
                        ? LoadProfIcon(dev, ard.primaryProf) : nullptr;
                    hmAgents.push_back({ id, label, ard.teamId, icon });
                }
            }
            size_t prevLayerCount = m_heatmapSettings.layers.size();
            m_panelLayout.ApplyPosition("heatmap");
            bool hmChanged = DrawHeatmapPanel(m_heatmapSettings, hmAgents, lutGetter);
            m_panelLayout.TrackWindowByName("heatmap", "Heatmap");
            if (hmChanged)
            {
                size_t newCount = m_heatmapSettings.layers.size();
                if (newCount != prevLayerCount)
                {
                    m_heatmapAccumulator.EnsureLayerCount(newCount);
                    if (m_heatmapInitialized)
                        m_heatmapRenderer.EnsureLayerTextures(
                            m_deviceResources->GetD3DDevice(), newCount);
                }
                m_heatmapAccumulator.MarkAllLayersDirty();
                ResolveHeatmapLayers();
                SaveHeatmapSettings();
            }
        }

        DrawHeatmapLegend(m_heatmapSettings, lutGetter);
    }

    {
        Camera* cam = m_mapRenderer->GetCamera();
        XMMATRIX annVP = cam->GetView() * cam->GetProj();
        auto annViewport = m_deviceResources->GetScreenViewport();
        m_annotationMgr.Update(m_terrain.get(), annVP,
                               annViewport.Width, annViewport.Height,
                               cam->GetPosition3f(), m_debugTimeline);
    }
    m_annotationMgr.RenderToolbar();
    {
        Camera* cam = m_mapRenderer->GetCamera();
        XMMATRIX annVP = cam->GetView() * cam->GetProj();
        auto annViewport = m_deviceResources->GetScreenViewport();
        m_annotationMgr.RenderDrawings(annVP, annViewport.Width,
                                       annViewport.Height, m_debugTimeline);
    }

    // Audio debug panel
    if (m_showAudioDebug && m_audioEngine) {
        ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Audio Debug", &m_showAudioDebug)) {
            auto stats = m_audioEngine->GetDebugStats();
            ImGui::Text("Engine initialized: %s", m_audioInitialized ? "YES" : "NO");
            ImGui::Text("Audio enabled:      %s", m_audioEnabled ? "YES" : "NO");
            ImGui::Text("MFT size:           %d", m_datManager ? m_datManager->get_num_files() : -1);
            if (m_datManager) {
                auto wpath = m_datManager->get_filepath();
                std::string path8(wpath.begin(), wpath.end());
                ImGui::TextWrapped("DAT: %s", path8.c_str());
            }
            ImGui::Separator();
            ImGui::Text("Events posted:      %d", stats.eventsPosted);
            ImGui::Text("Sounds played:      %d", stats.soundsPlayed);
            ImGui::Text("Voices active:      %d", stats.voicesActive);
            if (stats.voicesActive > 0) {
                float total = stats.panLeft + stats.panRight;
                float pct = total > 0.001f ? stats.panLeft / total : 0.5f;
                const char* dir = (pct > 0.55f) ? "LEFT" : (pct < 0.45f) ? "RIGHT" : "CENTER";
                ImGui::Text("Pan L:%.2f R:%.2f  [%s]", stats.panLeft, stats.panRight, dir);
            }
            ImGui::Separator();
            ImGui::Text("ID out of range:    %d", stats.hashNotFound);
            ImGui::Text("Not SOUND type:     %d", stats.notSoundType);
            ImGui::Text("Decode failures:    %d", stats.decodeFailures);
            if (auto* sc = m_audioEngine->GetSoundCache()) {
                auto& ll = sc->GetLastLoad();
                ImGui::Separator();
                ImGui::Text("Hash index entries: %d", sc->GetHashIndexSize());
                ImGui::Text("Last fileId (json): %u", ll.fileId);
                ImGui::Text("Resolved MFT idx:   %u", ll.resolvedIndex);
                ImGui::Text("Resolve method:     %s", ll.resolveMethod.c_str());
                ImGui::Text("Data size:          %d", ll.dataSize);
                ImGui::Text("MFT type:           %d", ll.mftType);
                ImGui::Text("Magic bytes:        %02X %02X %02X %02X",
                    ll.magic[0], ll.magic[1], ll.magic[2], ll.magic[3]);
                ImGui::Text("Load ok:            %s", ll.loaded ? "YES" : "NO");
                if (!ll.rejectReason.empty())
                    ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Reject: %s", ll.rejectReason.c_str());
            }
            ImGui::Separator();
            ImGui::Text("Last skill: %u '%s'", stats.lastSkillId, stats.lastSkillName.c_str());
            ImGui::Text("Timeline: %.2f  LastAudio: %.2f", m_debugTimeline, m_audioLastTime);
            ImGui::Separator();
            auto& cfg = m_audioEngine->GetConfig();
            ImGui::SliderFloat("Master Vol", &cfg.master_volume, 0.f, 1.f);
            ImGui::SliderFloat("SFX Vol", &cfg.sfx_volume, 0.f, 1.f);
            ImGui::SliderFloat("Dist Scale", &cfg.curve_distance_scaler, 1.f, 50.f);
        }
        ImGui::End();
    }
    else if (m_showAudioDebug && !m_audioEngine) {
        if (ImGui::Begin("Audio Debug", &m_showAudioDebug)) {
            ImGui::Text("Audio engine not created");
            ImGui::Text("Initialized: %s", m_audioInitialized ? "YES" : "NO");
        }
        ImGui::End();
    }

    DrawAgentOverlay();
    DrawFlags();
    DrawBundleItems();
    DrawSkillLasers();
    UpdateIncomingEffects();
    RenderIncomingEffects();
    UpdateSpeechBubbles();
    RenderSpeechBubbles();
    DrawFollowedAgentHUD();
    DrawRangeRings();
    DrawSpiritRanges();
    DrawWurmsShrineCaptureRadius();
    DrawRangeRingToolbar();
    DrawFogOfWarToolbar();
    DrawMoralePanel();
    DrawLordDamagePanel();
    DrawAutoCameraPanel();
    DrawAutoCameraDebugPanel();

    DrawMatchTimer();
    DrawFlagEventMessages();
    DrawJumboMessages();
    DrawMoraleBoostTimers();

    // Asset selection consumes the left-click before it becomes a pan
    if (m_leftClickPending && m_assetSelectionEnabled && !ImGui::GetIO().WantCaptureMouse)
    {
        m_leftClickPending = false;
        auto it = m_meshIdToPropIndex.find(m_hoveredPropMeshId);
        int newProp = (it != m_meshIdToPropIndex.end())
            ? static_cast<int>(it->second) : -1;
        SetPickedProp(newProp);
    }

    // Commit deferred left-click to pan if no agent was clicked
    if (m_leftClickPending && !m_annotationMgr.draw_mode_active)
    {
        m_leftClickPending = false;
        m_leftMouseDown = true;
        ClosePlayerInfoPanel();
        if (!m_rightMouseDown)
        {
            ShowCursor(FALSE);
            SetCapture(m_hwnd);
        }
        else
        {
            ClipCursor(nullptr);
            ShowCursor(FALSE);
        }
    }

    // Keyboard shortcuts — uses GetAsyncKeyState directly so hotkeys work even
    // without Win32 keyboard focus.  Only active when our process is in the
    // foreground and no text input / annotation mode is active.
    {
        DWORD fgPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
        bool processActive = (fgPid == GetCurrentProcessId());

        if (processActive && !ImGui::GetIO().WantTextInput && !m_clSkillSearchFocused
            && !m_annotationMgr.draw_mode_active)
        {
            const auto& hk = ReplayHotkeys::Get();

            if (HotkeyPressed(hk.exitFollowMode) && m_cameraMode == CameraMode::FollowAgent)
                ExitFollowMode();

            float maxT = std::max(1.f, m_replayCtx.maxReplayTime);

            // Configurable 5s hotkeys (skip if bound to arrow keys — arrows are handled below)
            bool rew5isArrow = (hk.rewind5s  == ImGuiKey_LeftArrow  || hk.rewind5s  == ImGuiKey_RightArrow);
            bool fwd5isArrow = (hk.forward5s == ImGuiKey_LeftArrow  || hk.forward5s == ImGuiKey_RightArrow);
            if (!rew5isArrow && HotkeyPressed(hk.rewind5s))
                m_debugTimeline = std::max(0.f, m_debugTimeline - 5.f);
            if (!fwd5isArrow && HotkeyPressed(hk.forward5s))
                m_debugTimeline = std::min(maxT, m_debugTimeline + 5.f);

            // Arrow keys: tap = ±1s, hold = continuous ±1s at repeat rate
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))
                m_debugTimeline = std::max(0.f, m_debugTimeline - 1.f);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true))
                m_debugTimeline = std::min(maxT, m_debugTimeline + 1.f);

            if (HotkeyPressed(hk.playPause))
                m_replayCtx.isPlaying = !m_replayCtx.isPlaying;

            if (HotkeyPressed(hk.toggleRangeRings))
            {
                m_showRangeRings = !m_showRangeRings;
                if (!m_showRangeRings)
                {
                    m_ringAgentFilter = -1;
                    m_ringHiddenAgents.clear();
                }
            }

            if (HotkeyPressed(hk.toggleMoralePanel))
                m_showMoralePanel = !m_showMoralePanel;

            if (HotkeyPressed(hk.toggleEventTimeline))
                m_showEventTimeline = !m_showEventTimeline;

            if (HotkeyPressed(hk.toggleLordDamage))
                m_showLordDamagePanel = !m_showLordDamagePanel;

            if (HotkeyPressed(hk.toggleAutoCamera))
            {
                m_autoCameraEnabled = !m_autoCameraEnabled;
                if (m_autoCameraEnabled)
                    m_autoCamState = AutoCameraState{};
                else
                    ExitFollowMode();
            }

            if (HotkeyPressed(hk.toggleFogOfWar) && !m_topViewActive)
            {
                if (m_fogPerspective > 0) {
                    m_fogLastActive = m_fogPerspective;
                    m_fogPerspective = 0;
                    m_fogPlayerAgent = -1;
                } else {
                    m_fogPerspective = m_fogLastActive;
                }
            }

            if (HotkeyPressed(hk.toggleTopView))
            {
                if (m_topViewActive)
                    ExitTopView();
                else
                    EnterTopView();
            }

            if (HotkeyPressed(hk.togglePianoRoll))
                m_showPianoRoll = !m_showPianoRoll;

            if (HotkeyPressed(hk.toggleHeatmap))
            {
                m_heatmapSettings.show = !m_heatmapSettings.show;
                SaveHeatmapSettings();
            }

            if (HotkeyPressed(hk.addBookmark))
                m_annotationMgr.BeginAddBookmark();
        }
    }

    // Sync panel visibility and auto-save layout if anything changed
    m_panelLayout.SyncVisibilityFromPointers();
    uint64_t stateHash = ComputePanelStateHash();
    bool stateChanged = (stateHash != m_lastPanelStateHash);
    if (stateChanged)
        m_lastPanelStateHash = stateHash;
    if (m_panelLayout.IsDirty() || stateChanged)
        SaveUILayout();

    // Determine cursor mode, then apply drag overrides before committing
    UpdateCursorMode();
    if (m_leftMouseDown)
        g_CurrentCursor = CursorMode::Hidden;
    else if (m_rightMouseDown)
        g_CurrentCursor = CursorMode::Precision;
    ApplyCursor();

    // Cache for next Update() — suppresses WASD camera polling while a text widget is active
    m_imguiWantTextInput = ImGui::GetIO().WantTextInput;

    // Draw post-loading match info overlay (synced to replay timeline)
    if (m_matchOverlayActive)
    {
        constexpr float kOverlayVisibleSec = 2.0f;
        constexpr float kOverlayFadeSec    = 2.0f;
        constexpr float kOverlayTotalSec   = kOverlayVisibleSec + kOverlayFadeSec;
        float overlayElapsed = m_debugTimeline - m_matchOverlayStartTime;
        if (overlayElapsed < 0.f)
            overlayElapsed = 0.f;
        if (overlayElapsed < kOverlayTotalSec)
        {
            float fadeT = (overlayElapsed <= kOverlayVisibleSec)
                ? 1.f
                : 1.f - std::clamp((overlayElapsed - kOverlayVisibleSec) / kOverlayFadeSec, 0.f, 1.f);
            ImDrawList* fgDl = ImGui::GetForegroundDrawList();
            ImVec2 disp = ImGui::GetIO().DisplaySize;
            DrawMatchInfoOverlay(fgDl, disp, fadeT);
        }
    }

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    ImGui::SetCurrentContext(prevCtx);
}

// ---------------------------------------------------------------------------
// Map calibration transform: save / load
// ---------------------------------------------------------------------------

static constexpr const char* kCalibrationFile = "map_transforms.txt";

void SaveMapTransform(int mapId, const MapTransform& t)
{
    std::map<int, MapTransform> all;
    {
        std::ifstream in(kCalibrationFile);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            int id;
            float ox, oy, oz, sx, sy, sz, rot;
            int fx, fy, fz, syz, sxz, sxy;
            if (sscanf_s(line.c_str(), "%d %f %f %f %f %f %f %f %d %d %d %d %d %d",
                         &id, &ox, &oy, &oz, &sx, &sy, &sz, &rot,
                         &fx, &fy, &fz, &syz, &sxz, &sxy) == 14)
            {
                all[id] = { ox, oy, oz, sx, sy, sz, rot,
                            fx != 0, fy != 0, fz != 0,
                            syz != 0, sxz != 0, sxy != 0 };
            }
        }
    }
    all[mapId] = t;
    {
        std::ofstream out(kCalibrationFile);
        out << "# map_id offX offY offZ scaleX scaleY scaleZ rotation flipX flipY flipZ swapYZ swapXZ swapXY\n";
        for (auto& [id, mt] : all)
        {
            char buf[512];
            snprintf(buf, sizeof(buf),
                     "%d %.4f %.4f %.4f %.6f %.6f %.6f %.4f %d %d %d %d %d %d\n",
                     id, mt.offsetX, mt.offsetY, mt.offsetZ,
                     mt.scaleX, mt.scaleY, mt.scaleZ, mt.rotationDegrees,
                     mt.flipX ? 1 : 0, mt.flipY ? 1 : 0, mt.flipZ ? 1 : 0,
                     mt.swapYZ ? 1 : 0, mt.swapXZ ? 1 : 0, mt.swapXY ? 1 : 0);
            out << buf;
        }
    }
}

static MapTransform LoadMapTransform(int mapId, bool* found)
{
    if (found) *found = false;
    std::ifstream in(kCalibrationFile);
    if (!in.is_open()) return {};
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        int id;
        float ox, oy, oz, sx, sy, sz, rot;
        int fx, fy, fz, syz, sxz, sxy;
        if (sscanf_s(line.c_str(), "%d %f %f %f %f %f %f %f %d %d %d %d %d %d",
                     &id, &ox, &oy, &oz, &sx, &sy, &sz, &rot,
                     &fx, &fy, &fz, &syz, &sxz, &sxy) == 14)
        {
            if (id == mapId) {
                if (found) *found = true;
                return { ox, oy, oz, sx, sy, sz, rot,
                         fx != 0, fy != 0, fz != 0,
                         syz != 0, sxz != 0, sxy != 0 };
            }
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Agent Overlay: full calibration transform pipeline + rendering
// ---------------------------------------------------------------------------

ImU32 GetAgentTeamColor(uint8_t teamId)
{
    switch (teamId) {
    case 1:  return IM_COL32(0xFF, 0x4A, 0x4A, 0xFF);
    case 2:  return IM_COL32(0x2A, 0x8C, 0xFF, 0xFF);
    default: return IM_COL32(0xAA, 0xAA, 0xAA, 0xFF);
    }
}

// Binary search: find index of last snapshot with time <= t
int FindSnapshotIndex(const std::vector<AgentSnapshot>& snaps, float t)
{
    int lo = 0, hi = static_cast<int>(snaps.size()) - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (snaps[mid].time <= t) lo = mid; else hi = mid - 1;
    }
    return lo;
}

// Snap to the nearest snapshot <= t (no blending). Used for flags and
// when interpolation is disabled.
void SnapAgentPosition(const AgentReplayData& ard, float t,
                              float& outX, float& outY, float& outZ)
{
    const auto& snaps = ard.snapshots;
    if (snaps.empty()) { outX = outY = outZ = 0.f; return; }
    if (t <= snaps.front().time) {
        outX = snaps.front().x; outY = snaps.front().y; outZ = snaps.front().z; return;
    }
    if (t >= snaps.back().time) {
        outX = snaps.back().x; outY = snaps.back().y; outZ = snaps.back().z; return;
    }
    int idx = FindSnapshotIndex(snaps, t);
    outX = snaps[idx].x; outY = snaps[idx].y; outZ = snaps[idx].z;
}

// Stationary threshold: if both bracketing snapshots are within this distance
// per axis (game units), skip interpolation to avoid micro-jitter from data noise.
// Matches the RAW_COORDINATE_EPSILON from the working website implementation.
static constexpr float kStationaryEpsilon = 1.0f;

// Original linear interpolation (legacy behavior).
static void LinearInterpolatePosition(const AgentReplayData& ard, float t,
                                      float& outX, float& outY, float& outZ)
{
    const auto& snaps = ard.snapshots;
    if (snaps.empty()) { outX = outY = outZ = 0.f; return; }
    if (t <= snaps.front().time) {
        outX = snaps.front().x; outY = snaps.front().y; outZ = snaps.front().z; return;
    }
    if (t >= snaps.back().time) {
        outX = snaps.back().x; outY = snaps.back().y; outZ = snaps.back().z; return;
    }
    int lo = FindSnapshotIndex(snaps, t);
    auto& s0 = snaps[lo];
    if (lo + 1 < static_cast<int>(snaps.size())) {
        auto& s1 = snaps[lo + 1];

        if (fabsf(s1.x - s0.x) <= kStationaryEpsilon &&
            fabsf(s1.y - s0.y) <= kStationaryEpsilon &&
            fabsf(s1.z - s0.z) <= kStationaryEpsilon) {
            outX = s0.x; outY = s0.y; outZ = s0.z;
            return;
        }

        float dt = s1.time - s0.time;
        float a = (dt > 0.001f) ? (t - s0.time) / dt : 0.f;
        outX = s0.x + (s1.x - s0.x) * a;
        outY = s0.y + (s1.y - s0.y) * a;
        outZ = s0.z + (s1.z - s0.z) * a;
    } else {
        outX = s0.x; outY = s0.y; outZ = s0.z;
    }
}

// Find the last MOVE_TO_POINT event at or before time t (binary search).
// Returns -1 if none exists.
int FindMoveEventIndex(const std::vector<MoveToPointEvent>& moves, float t)
{
    if (moves.empty()) return -1;
    int lo = 0, hi = static_cast<int>(moves.size()) - 1, result = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (moves[mid].time <= t) { result = mid; lo = mid + 1; }
        else { hi = mid - 1; }
    }
    return result;
}

// Improved interpolation: MOVE_TO_POINT aware.
//   - For small gaps (<= gapThreshold): pure linear lerp
//   - For large gaps: use MOVE_TO_POINT target as movement direction anchor
//     blended with linear interpolation via velocityInfluence slider.
//   - If no MOVE_TO_POINT exists for the interval, pure linear interpolation.
static void ImprovedInterpolatePosition(const AgentReplayData& ard, float t,
                                        const InterpolationSettings& s,
                                        float& outX, float& outY, float& outZ)
{
    const auto& snaps = ard.snapshots;
    if (snaps.empty()) { outX = outY = outZ = 0.f; return; }
    if (t <= snaps.front().time) {
        outX = snaps.front().x; outY = snaps.front().y; outZ = snaps.front().z; return;
    }
    if (t >= snaps.back().time) {
        outX = snaps.back().x; outY = snaps.back().y; outZ = snaps.back().z; return;
    }

    int lo = FindSnapshotIndex(snaps, t);
    auto& prev = snaps[lo];

    if (lo + 1 >= static_cast<int>(snaps.size())) {
        outX = prev.x; outY = prev.y; outZ = prev.z;
        return;
    }
    auto& next = snaps[lo + 1];

    if (fabsf(next.x - prev.x) <= kStationaryEpsilon &&
        fabsf(next.y - prev.y) <= kStationaryEpsilon &&
        fabsf(next.z - prev.z) <= kStationaryEpsilon) {
        outX = prev.x; outY = prev.y; outZ = prev.z;
        return;
    }

    float gap = next.time - prev.time;
    float alpha = (gap > 0.001f) ? (t - prev.time) / gap : 0.f;

    // Base linear interpolation
    float lx = prev.x + (next.x - prev.x) * alpha;
    float ly = prev.y + (next.y - prev.y) * alpha;
    float lz = prev.z + (next.z - prev.z) * alpha;

    // MOVE_TO_POINT directional prediction for large gaps
    if (gap > s.gapThreshold && s.velocityInfluence > 0.f)
    {
        int moveIdx = FindMoveEventIndex(ard.moveEvents, t);
        if (moveIdx >= 0)
        {
            auto& move = ard.moveEvents[moveIdx];

            // Direction from previous snapshot position toward the MOVE_TO_POINT target
            float dx = move.targetX - prev.x;
            float dy = move.targetY - prev.y;
            float dist = sqrtf(dx * dx + dy * dy);

            if (dist > 1.f)
            {
                // Estimate speed from the distance between the two bracketing snapshots
                float speed = sqrtf((next.x - prev.x) * (next.x - prev.x) +
                                    (next.y - prev.y) * (next.y - prev.y)) / gap;

                float dirX = dx / dist;
                float dirY = dy / dist;
                float dt = t - prev.time;

                float px = prev.x + dirX * speed * dt;
                float py = prev.y + dirY * speed * dt;
                float pz = prev.z;

                float beta = (gap - s.gapThreshold) / 1.0f;
                if (beta > 1.f) beta = 1.f;
                beta *= s.velocityInfluence;

                outX = lx + (px - lx) * beta;
                outY = ly + (py - ly) * beta;
                outZ = lz + (pz - lz) * beta;
                return;
            }
        }
        // No applicable MOVE_TO_POINT — fall through to pure linear
    }

    outX = lx;
    outY = ly;
    outZ = lz;
}

// Internal dispatch: run the active interpolation mode (or snap when disabled).
static void DispatchInterpolation(const AgentReplayData& ard, float t,
                                  const InterpolationSettings& is,
                                  float& outX, float& outY, float& outZ)
{
    if (!is.enabled) {
        SnapAgentPosition(ard, t, outX, outY, outZ);
        return;
    }
    if (is.mode == InterpolationMode::OriginalLinear)
        LinearInterpolatePosition(ard, t, outX, outY, outZ);
    else
        ImprovedInterpolatePosition(ard, t, is, outX, outY, outZ);
}

// Unified entry point: routes through flag snap / disabled snap /
// original linear / improved, based on agent type and settings.
//
// Casting freeze is intentionally NOT applied here.  The snapshot data
// already reflects the game's movement freeze during casts — consecutive
// snapshots during a cast have nearly identical positions, so the
// stationary-detection epsilon in the lerp functions keeps the agent
// still without introducing a position discontinuity at cast boundaries.
//
// Death freeze interpolates to the moment the agent died instead of
// snapping to a raw snapshot, avoiding a teleport at the alive→dead edge.
void InterpolateAgentPosition(const AgentReplayData& ard, float t,
                                     const InterpolationSettings& is,
                                     float& outX, float& outY, float& outZ)
{
    // Flags and Spirits never interpolate — snap to nearest recorded position
    if (ard.type == AgentType::Flag || ard.type == AgentType::Spirit) {
        SnapAgentPosition(ard, t, outX, outY, outZ);
        return;
    }

    // Death freeze: interpolate to the moment of death so there is no
    // position jump at the alive→dead boundary.
    if (ard.isDeadAtTime(t)) {
        float deathT = ard.deathTransitionTime(t);
        DispatchInterpolation(ard, deathT, is, outX, outY, outZ);
        return;
    }

    DispatchInterpolation(ard, t, is, outX, outY, outZ);
}

std::string GetAgentLabel(const AgentReplayData& ard)
{
    switch (ard.type) {
    case AgentType::Player:
        if (!ard.guildTag.empty())
            return ard.playerName + " [" + ard.guildTag + "]";
        return ard.playerName;
    case AgentType::NPC:               return ard.categoryName;
    case AgentType::Gadget:            return ard.categoryName;
    case AgentType::ObeliskFlagStand:  return ard.categoryName;
    case AgentType::Flag:              return "Flag";
    case AgentType::Spirit:            return ard.categoryName;
    case AgentType::Item:              return ard.categoryName;
    default:                           return std::format("Agent {}", ard.agent_id);
    }
}

XMFLOAT3 ApplyMapTransformToPos(float snapX, float snapY, float snapZ,
                                        const MapTransform& t)
{
    // 0. Base axis remap: GWCA (x,y,z_height) → GWMB (x, z_height, y)
    float px = snapX;
    float py = snapZ;
    float pz = snapY;

    // 1. Axis swaps
    if (t.swapYZ) { float tmp = py; py = pz; pz = tmp; }
    if (t.swapXZ) { float tmp = px; px = pz; pz = tmp; }
    if (t.swapXY) { float tmp = px; px = py; py = tmp; }

    // 2. Axis flips
    if (t.flipX) px = -px;
    if (t.flipY) py = -py;
    if (t.flipZ) pz = -pz;

    // 3. Rotation around Y axis
    if (t.rotationDegrees != 0.f) {
        float rad = t.rotationDegrees * (XM_PI / 180.f);
        float c = cosf(rad), s = sinf(rad);
        float rx = px * c - pz * s;
        float rz = px * s + pz * c;
        px = rx;
        pz = rz;
    }

    // 4. Offset
    px += t.offsetX;
    py += t.offsetY;
    pz += t.offsetZ;

    // 5. Scale
    px *= t.scaleX;
    py *= t.scaleY;
    pz *= t.scaleZ;

    return { px, py, pz };
}

bool ProjectToScreen(XMMATRIX viewProj, float vpW, float vpH,
                             const XMFLOAT3& worldPos, float& scrX, float& scrY)
{
    XMVECTOR clip = XMVector4Transform(
        XMVectorSet(worldPos.x, worldPos.y, worldPos.z, 1.f), viewProj);
    float w = XMVectorGetW(clip);
    if (w <= 0.001f) return false;
    float ndcX = XMVectorGetX(clip) / w;
    float ndcY = XMVectorGetY(clip) / w;
    scrX = (ndcX + 1.f) * 0.5f * vpW;
    scrY = (1.f - ndcY) * 0.5f * vpH;
    return (scrX > -200.f && scrX < vpW + 200.f &&
            scrY > -200.f && scrY < vpH + 200.f);
}


// ---------------------------------------------------------------------------
// Cylinder agent marker — 3D renderer
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Main thread: create GPU resources in batches (called per frame)
// Uses the EXACT same texture creation → remap → AddProp flow as the
// working synchronous LoadAgentModels to avoid the color regression.
// ---------------------------------------------------------------------------


// Forward declarations for icon loaders (defined later)

// Gradient helpers (used by cast bar rendering + texture building)
float LerpGradChannel(float a, float b, float t) { return a + (b - a) * t; }
void SampleGradient(const GradStop* stops, int n, float t,
                            float& outR, float& outG, float& outB)
{
    const GradStop* a = &stops[0];
    const GradStop* b = &stops[n - 1];
    for (int i = 0; i < n - 1; ++i)
    {
        if (t >= stops[i].pos && t <= stops[i + 1].pos)
        { a = &stops[i]; b = &stops[i + 1]; break; }
    }
    float lt = (b->pos - a->pos > 0.0001f) ? (t - a->pos) / (b->pos - a->pos) : 0.f;
    outR = LerpGradChannel(a->r, b->r, lt);
    outG = LerpGradChannel(a->g, b->g, lt);
    outB = LerpGradChannel(a->b, b->b, lt);
}

// ---------------------------------------------------------------------------
// Flag Debug Window — shows reconstructed FlagTimeline
// ---------------------------------------------------------------------------

const char* FlagLocationName(FlagLocation loc)
{
    switch (loc) {
    case FlagLocation::Base:    return "Base";
    case FlagLocation::Carried: return "Carried";
    case FlagLocation::Ground:  return "Ground";
    case FlagLocation::Stand:   return "Stand";
    }
    return "?";
}

const char* FlagEventTypeName(FlagTimelineEventType t)
{
    switch (t) {
    case FlagTimelineEventType::Spawn:       return "Spawn";
    case FlagTimelineEventType::Pickup:      return "Pickup";
    case FlagTimelineEventType::Drop:        return "Drop";
    case FlagTimelineEventType::Stick:       return "Stick";
    case FlagTimelineEventType::Return:      return "Return";
    case FlagTimelineEventType::GroundSpawn: return "GroundSpawn";
    }
    return "?";
}

const char* StandOwnerName(StandOwner o)
{
    switch (o) {
    case StandOwner::Neutral: return "Neutral";
    case StandOwner::Red:    return "Red";
    case StandOwner::Blue:     return "Blue";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// ResolveAgentAtTime — find the correct incarnation entry for a given agent_id
// and timestamp, accounting for agent ID recycling.
// ---------------------------------------------------------------------------

int ReplayWindow::ResolveAgentAtTime(int agentId, float time) const
{
    constexpr float kTol = 0.5f;

    auto it = m_replayCtx.agents.find(agentId);
    if (it != m_replayCtx.agents.end())
    {
        const auto& ard = it->second;
        if (ard.lifecycleStart >= 0)
        {
            float t0 = ard.lifecycleStart - kTol;
            float t1 = (ard.lifecycleEnd >= 0) ? ard.lifecycleEnd + kTol : FLT_MAX;
            if (time >= t0 && time <= t1)
                return agentId;
        }
        else if (!ard.snapshots.empty())
        {
            float t0 = ard.snapshots.front().time - kTol;
            float t1 = ard.snapshots.back().time + kTol;
            if (time >= t0 && time <= t1)
                return agentId;
        }
    }

    auto incIt = m_incarnationMap.find(agentId);
    if (incIt != m_incarnationMap.end())
    {
        for (int sid : incIt->second)
        {
            auto sit = m_replayCtx.agents.find(sid);
            if (sit == m_replayCtx.agents.end()) continue;
            const auto& sard = sit->second;
            if (sard.lifecycleStart >= 0)
            {
                float t0 = sard.lifecycleStart - kTol;
                float t1 = (sard.lifecycleEnd >= 0) ? sard.lifecycleEnd + kTol : FLT_MAX;
                if (time >= t0 && time <= t1)
                    return sid;
            }
            else if (!sard.snapshots.empty())
            {
                float t0 = sard.snapshots.front().time - kTol;
                float t1 = sard.snapshots.back().time + kTol;
                if (time >= t0 && time <= t1)
                    return sid;
            }
        }
    }

    return agentId;
}

// ---------------------------------------------------------------------------
// Range Ring Rendering
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Scene overlays: Match Timer, Jumbo Messages, Morale Boost Timers
// ---------------------------------------------------------------------------

static float sVw(float pct) { return ImGui::GetMainViewport()->Size.x * pct; }
static float sVh(float pct) { return ImGui::GetMainViewport()->Size.y * pct; }

void FormatMMSS(char* buf, size_t bufSz, float seconds)
{
    int s = std::max(0, static_cast<int>(seconds));
    int m = s / 60;
    int ss = s % 60;
    snprintf(buf, bufSz, "%02d:%02d", m, ss);
}

static int JumboPartyToTeam(int partyValue)
{
    if (partyValue == 1635021873) return 1;
    if (partyValue == 1635021874) return 2;
    return 0;
}

const char* JumboMessageDisplayText(const std::string& msgType, int team)
{
    const char* side = (team == 1) ? "Red" : "Blue";
    static char buf[128];
    if      (msgType == "BASE_UNDER_ATTACK")       snprintf(buf, sizeof(buf), "%s Base Under Attack",       side);
    else if (msgType == "GUILD_LORD_UNDER_ATTACK")  snprintf(buf, sizeof(buf), "%s Guild Lord Under Attack",  side);
    else if (msgType == "CAPTURED_SHRINE")          snprintf(buf, sizeof(buf), "%s Captured Shrine",          side);
    else if (msgType == "NEUTRALIZED_SHRINE")       snprintf(buf, sizeof(buf), "%s neutralized Health Shrine", side);
    else if (msgType == "CAPTURED_TOWER")           snprintf(buf, sizeof(buf), "%s Captured Tower",           side);
    else if (msgType == "PARTY_DEFEATED")           snprintf(buf, sizeof(buf), "%s Party Defeated",           side);
    else if (msgType == "MORALE_BOOST")             snprintf(buf, sizeof(buf), "%s Morale Boost",             side);
    else if (msgType == "VICTORY")                  snprintf(buf, sizeof(buf), "%s Victory!",                 side);
    else if (msgType == "FLAWLESS_VICTORY")         snprintf(buf, sizeof(buf), "%s Flawless Victory!",        side);
    else                                            snprintf(buf, sizeof(buf), "%s %s",                       side, msgType.c_str());
    return buf;
}

// Helper: handle drag mode for an overlay element.
// Returns true if currently being dragged; updates the fraction-based position.
bool ReplayWindow::HandleOverlayDrag(int elementIdx, float* fracX, float* fracY,
                                      ImVec2 boxTL, ImVec2 boxBR)
{
    if (m_draggingUIElement != elementIdx) return false;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRect(ImVec2(boxTL.x - 2, boxTL.y - 2), ImVec2(boxBR.x + 2, boxBR.y + 2),
                IM_COL32(0xF5, 0xE4, 0x5A, 180), 4.f, 0, 2.f);
    dl->AddRectFilled(boxTL, boxBR, IM_COL32(0xF5, 0xE4, 0x5A, 30), 4.f);

    const ImGuiViewport* vp = ImGui::GetMainViewport();

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        *fracX += delta.x / vp->Size.x;
        *fracY += delta.y / vp->Size.y;
        *fracX = std::clamp(*fracX, 0.f, 1.f);
        *fracY = std::clamp(*fracY, 0.f, 1.f);
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        m_draggingUIElement = -1;
        SaveUILayout();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Timeline profession icon loader (Textures/professions/{id}.png — same as website)
// ---------------------------------------------------------------------------
ImTextureID LoadProfIconTimeline(ID3D11Device* device, int profId)
{
    if (profId < 1 || profId > 10) return nullptr;

    static ID3D11Device* s_dev = nullptr;
    static std::unordered_map<int, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_dev) { s_cache.clear(); s_dev = device; }

    auto it = s_cache.find(profId);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    static std::filesystem::path basePath;
    if (basePath.empty())
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto dir = std::filesystem::path(exePath).parent_path();
        for (int i = 0; i < 5; i++)
        {
            if (std::filesystem::exists(dir / "Textures" / "professions"))
            {
                basePath = dir / "Textures" / "professions";
                break;
            }
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
        if (basePath.empty())
            basePath = std::filesystem::path(exePath).parent_path() / "Textures" / "professions";
    }

    char fn[16]; snprintf(fn, sizeof(fn), "%d.png", profId);
    auto fullPath = basePath / fn;
    if (!std::filesystem::exists(fullPath)) return nullptr;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&texDesc, &initData, &tex);
    if (FAILED(hr)) return nullptr;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (FAILED(hr)) return nullptr;

    s_cache[profId] = srv;
    return (ImTextureID)srv.Get();
}

// ---------------------------------------------------------------------------
// NPC icon loader (Textures/NPC subfolder)
// ---------------------------------------------------------------------------
std::filesystem::path GetNPCIconBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "NPC"))
        {
            cached = dir / "Textures" / "NPC";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "NPC";
    return cached;
}

ImTextureID LoadNPCIcon(ID3D11Device* device, const char* filename)
{
    static ID3D11Device* s_cachedDevice = nullptr;
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_cachedDevice) { s_cache.clear(); s_cachedDevice = device; }

    auto it = s_cache.find(filename);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    auto fullPath = GetNPCIconBasePath() / std::filesystem::path(filename);
    if (!std::filesystem::exists(fullPath)) return nullptr;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&texDesc, &initData, &tex);
    if (FAILED(hr)) return nullptr;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (FAILED(hr)) return nullptr;

    s_cache[filename] = srv;
    return (ImTextureID)srv.Get();
}


ImTextureID LoadSkillIconFile(ID3D11Device* device, const char* filename)
{
    static ID3D11Device* s_cachedDevice = nullptr;
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_cachedDevice) { s_cache.clear(); s_cachedDevice = device; }

    auto it = s_cache.find(filename);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    auto fullPath = GetSkillIconsBasePath() / std::filesystem::path(filename);
    if (!std::filesystem::exists(fullPath)) return nullptr;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&texDesc, &initData, &tex);
    if (FAILED(hr)) return nullptr;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (FAILED(hr)) return nullptr;

    s_cache[filename] = srv;
    return (ImTextureID)srv.Get();
}

void ReplayWindow::DrawMatchTimer()
{
    ImFont* font = m_latoRegular ? m_latoRegular : ImGui::GetFont();
    float fontSize = font->FontSize;

    float curTime = m_debugTimeline;
    float matchTime = curTime - m_matchStartOffset;

    bool isCountdown = matchTime < 0.f;
    float absTime = fabsf(matchTime);
    FormatMMSS(m_timerBuf, sizeof(m_timerBuf), absTime);

    const char* label = isCountdown ? "Time to start:" : "Time Elapsed:";

    float labelFs = fontSize;
    float timeFs  = fontSize;

    ImVec2 labelSize = font->CalcTextSizeA(labelFs, FLT_MAX, 0.f, label);
    ImVec2 timeSize  = font->CalcTextSizeA(timeFs, FLT_MAX, 0.f, m_timerBuf);
    float boxW = std::max(labelSize.x, timeSize.x);
    float lineH = labelFs + 2.f;
    float boxH = lineH * 2.f;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float posX = m_uiLayout.useCustom ? m_uiLayout.timerX : 0.50f;
    float posY = m_uiLayout.useCustom ? m_uiLayout.timerY : 0.12f;
    float cx   = vp->Pos.x + vp->Size.x * posX;
    float topY = vp->Pos.y + vp->Size.y * posY;

    float padX, padY;
    if (isCountdown) { padX = 12.f; padY = 10.f; }
    else             { padX = 8.f;  padY = 4.f; }

    ImVec2 boxTL(cx - boxW * 0.5f - padX, topY);
    ImVec2 boxBR(cx + boxW * 0.5f + padX, topY + boxH + padY * 2.f);

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    if (isCountdown)
    {
        dl->AddRectFilled(boxTL, boxBR, IM_COL32(0, 0, 0, 204), 8.f);
        dl->AddRect(boxTL, boxBR, IM_COL32(0xB7, 0xB8, 0xB3, 0xFF), 8.f, 0, 1.f);
    }

    ImU32 goldCol = IM_COL32(0xF5, 0xE4, 0xB4, 0xFF);
    ImU32 shA     = IM_COL32(0, 0, 0, 204);
    ImU32 shB     = IM_COL32(0, 0, 0, 230);

    auto drawShadowedText = [&](ImFont* f, float fs, ImVec2 pos, ImU32 col, const char* txt)
    {
        dl->AddText(f, fs, ImVec2(pos.x, pos.y + 1), shA, txt);
        dl->AddText(f, fs, ImVec2(pos.x, pos.y + 1), shB, txt);
        dl->AddText(f, fs, pos, col, txt);
    };

    float labelX = cx - labelSize.x * 0.5f;
    float labelY = boxTL.y + padY;
    drawShadowedText(font, labelFs, ImVec2(labelX, labelY), goldCol, label);

    float timeX = cx - timeSize.x * 0.5f;
    float timeY = labelY + lineH;
    drawShadowedText(font, timeFs, ImVec2(timeX, timeY), goldCol, m_timerBuf);

    HandleOverlayDrag(3, &m_uiLayout.timerX, &m_uiLayout.timerY, boxTL, boxBR);
}

void ReplayWindow::DrawJumboMessages()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float posX = m_uiLayout.useCustom ? m_uiLayout.jumboX : 0.50f;
    float posY = m_uiLayout.useCustom ? m_uiLayout.jumboY : 0.30f;

    // Show drag preview even without an active jumbo message
    if (m_draggingUIElement == 0)
    {
        ImFont* font = m_latoBoldBig ? m_latoBoldBig : ImGui::GetFont();
        float fontSize = font->FontSize;
        const char* preview = "Red Captured Tower";
        ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, preview);

        float cx = vp->Pos.x + vp->Size.x * posX;
        float ty = vp->Pos.y + vp->Size.y * posY;
        float tx = cx - textSize.x * 0.5f;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddText(font, fontSize, ImVec2(tx, ty + 1), IM_COL32(0, 0, 0, 150), preview);
        dl->AddText(font, fontSize, ImVec2(tx, ty), IM_COL32(0xFF, 0x99, 0x9A, 180), preview);

        ImVec2 boxTL(tx - 4, ty - 4);
        ImVec2 boxBR(tx + textSize.x + 4, ty + textSize.y + 4);
        HandleOverlayDrag(0, &m_uiLayout.jumboX, &m_uiLayout.jumboY, boxTL, boxBR);
        return;
    }

    if (!m_replayCtx.stocLoaded) return;

    const auto& jumbos = m_replayCtx.stocData.jumbo;
    if (jumbos.empty()) return;

    float curTime = m_debugTimeline;

    const JumboMessageEvent* best = nullptr;
    float bestAge = 999.f;
    for (auto& ev : jumbos)
    {
        float age = curTime - ev.time;
        if (age < 0.f || age > 6.f) continue;
        if (!best || ev.time > best->time)
        {
            best = &ev;
            bestAge = age;
        }
    }

    if (!best) return;

    float alpha = 1.f;
    if (bestAge > 5.f)
        alpha = std::clamp(6.f - bestAge, 0.f, 1.f);
    if (alpha <= 0.f) return;

    int team = JumboPartyToTeam(best->party_value);
    const bool isNeutralizedShrine = (best->message == "NEUTRALIZED_SHRINE");
    // NEUTRALIZED_SHRINE party_value identifies the losing team — invert for display
    if (isNeutralizedShrine && team > 0)
        team = (team == 1) ? 2 : 1;
    const char* text = JumboMessageDisplayText(best->message, team);

    int a = static_cast<int>(alpha * 255);
    ImU32 teamCol;
    if (isNeutralizedShrine)    teamCol = IM_COL32(0xBB, 0xBB, 0xBB, a);
    else if (team == 1)         teamCol = IM_COL32(0xFF, 0x99, 0x9A, a);
    else if (team == 2)         teamCol = IM_COL32(0x99, 0xCB, 0xFD, a);
    else                        teamCol = IM_COL32(0xFF, 0xFF, 0xFF, a);

    ImFont* font = m_latoBoldBig ? m_latoBoldBig : ImGui::GetFont();
    float fontSize = font->FontSize;
    ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, text);

    float cx = vp->Pos.x + vp->Size.x * posX;
    float topY = vp->Pos.y + vp->Size.y * posY;

    float tx = cx - textSize.x * 0.5f;
    float ty = topY;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(alpha * 230));
    dl->AddText(font, fontSize, ImVec2(tx, ty + 1), shadow, text);
    dl->AddText(font, fontSize, ImVec2(tx, ty), teamCol, text);
}

// ---------------------------------------------------------------------------
// Timeline Controller — fixed bottom playback bar
// Styled to match the GW Observer design system:
//   bg1 #111213  bg2 #161718  bg3 #1c1d1e  bg4 #212324
//   line #252627  line2 #2e2f30  line3 #3a3b3c
//   t1 #e2e3e4  t2 #909294  t3 #55575a  t4 #363739
//   acc #4d8ef0
// ---------------------------------------------------------------------------

std::string GetSvgIconBasePath()
{
    static std::string cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "timebar_UI"))
        {
            cached = (dir / "Textures" / "timebar_UI").string();
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path().string();
    return cached;
}

// Rasterize an SVG to a white-on-transparent RGBA texture for use on a dark bar.
// Cached after first load.
static std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> s_svgIconCache;
static ID3D11Device* s_svgIconCacheDevice = nullptr;

ImTextureID LoadSvgIcon(ID3D11Device* device, const char* filename, int rasterSize)
{
    // Invalidate cache when the device changes (new replay window)
    if (device != s_svgIconCacheDevice)
    {
        s_svgIconCache.clear();
        s_svgIconCacheDevice = device;
    }

    auto it = s_svgIconCache.find(filename);
    if (it != s_svgIconCache.end())
        return (ImTextureID)it->second.Get();

    if (!device) return nullptr;

    std::string fullPath = GetSvgIconBasePath() + "\\" + filename;
    if (!std::filesystem::exists(fullPath)) return nullptr;

    NSVGimage* svg = nsvgParseFromFile(fullPath.c_str(), "px", 96.0f);
    if (!svg) return nullptr;

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(svg); return nullptr; }

    float scale = (float)rasterSize / std::max(svg->width, svg->height);
    int w = rasterSize;
    int h = rasterSize;

    std::vector<uint8_t> rgba(w * h * 4, 0);
    nsvgRasterize(rast, svg, 0, 0, scale, rgba.data(), w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);

    // Recolor: any visible pixel → white, preserving its alpha.
    // The original SVG fill is dark (#1C274C), but we want white icons on dark bg.
    for (int i = 0; i < w * h; i++)
    {
        uint8_t a = rgba[i * 4 + 3];
        if (a > 0)
        {
            rgba[i * 4 + 0] = 255;
            rgba[i * 4 + 1] = 255;
            rgba[i * 4 + 2] = 255;
        }
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width     = w;
    texDesc.Height    = h;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage     = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem     = rgba.data();
    initData.SysMemPitch = w * 4;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, &srv);
    if (FAILED(hr)) return nullptr;

    s_svgIconCache[filename].Attach(srv);
    return (ImTextureID)srv;
}

void FormatTime(float seconds, char* buf, size_t bufSize)
{
    int totalSec = static_cast<int>(seconds);
    bool neg = totalSec < 0;
    if (neg) totalSec = -totalSec;
    int m = totalSec / 60;
    int s = totalSec % 60;
    if (neg)
        snprintf(buf, bufSize, "-%02d:%02d", m, s);
    else
        snprintf(buf, bufSize, "%02d:%02d", m, s);
}

// ---------------------------------------------------------------------------
// Debug window: Agent Data Viewer
// ---------------------------------------------------------------------------

const char* GetWeaponTypeName(uint16_t wt)
{
    switch (wt) {
    case 1:  return "Bow";
    case 2:  return "Axe";
    case 3:  return "Hammer";
    case 4:  return "Daggers";
    case 5:  return "Scythe";
    case 6:  return "Spear";
    case 7:  return "Sword";
    case 8:  return "Staff";
    case 10: return "Wand";
    default: return "Unknown";
    }
}

const char* GetTeamName(uint8_t tid)
{
    switch (tid) {
    case 0: return "None";
    case 1: return "Red";
    case 2: return "Blue";
    case 3: return "Yellow";
    default: return "?";
    }
}

const char* GetDaggerStatusName(uint8_t ds)
{
    switch (ds) {
    case 0: return "None";
    case 1: return "Lead";
    case 2: return "Offhand";
    case 3: return "Dual";
    default: return "?";
    }
}

const AgentSnapshot* FindSnapshotAtTime(const AgentReplayData& ard, float t)
{
    if (ard.snapshots.empty()) return nullptr;
    // Binary search for the last snapshot with time <= t
    int lo = 0, hi = static_cast<int>(ard.snapshots.size()) - 1;
    if (t < ard.snapshots[0].time) return &ard.snapshots[0];
    if (t >= ard.snapshots.back().time) return &ard.snapshots.back();
    while (lo < hi)
    {
        int mid = lo + (hi - lo + 1) / 2;
        if (ard.snapshots[mid].time <= t)
            lo = mid;
        else
            hi = mid - 1;
    }
    return &ard.snapshots[lo];
}

// ---------------------------------------------------------------------------
// Party Windows (Phase 5+6) — dockable health bar panels
// ---------------------------------------------------------------------------



void DrawGradientRect(ImDrawList* dl, ImVec2 tl, ImVec2 br, const Gradient5& g)
{
    float h = br.y - tl.y;
    float segH = h * 0.25f;
    for (int i = 0; i < 4; ++i)
    {
        float y0 = tl.y + segH * i;
        float y1 = (i == 3) ? br.y : (y0 + segH);
        dl->AddRectFilledMultiColor(
            ImVec2(tl.x, y0), ImVec2(br.x, y1),
            g.c[i], g.c[i], g.c[i + 1], g.c[i + 1]);
    }
}

std::filesystem::path GetGameUIBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "Game_UI"))
        {
            cached = dir / "Textures" / "Game_UI";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "Game_UI";
    return cached;
}

std::filesystem::path GetWeaponsBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "Weapons"))
        {
            cached = dir / "Textures" / "Weapons";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "Weapons";
    return cached;
}

ImTextureID LoadWeaponTexture(ID3D11Device* device, const char* filename)
{
    static ID3D11Device* s_cachedDevice = nullptr;
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_cachedDevice) { s_cache.clear(); s_cachedDevice = device; }

    auto it = s_cache.find(filename);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    auto fullPath = GetWeaponsBasePath() / filename;
    if (!std::filesystem::exists(fullPath)) return nullptr;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    s_cache[filename] = srv;
    return (ImTextureID)srv.Get();
}

// Resolve weapon textures using (weapon_type, weapon_item_type) lookup table.
// weapon_type: broad category (0=flag/bundle, 1=bow, 2=axe, ... 8-14=caster)
// weapon_item_type values:
//   12  = wand/focus pair (main + offhand focus)
//   24  = melee or wand + shield
//   46  = two-handed weapon, staff, or single weapon
//         (bow, hammer, daggers, scythe, staff, flag)
//   Any other value: treat as 46 (single/two-handed), log warning if unexpected
// teamId: 1=red, 2=blue (for flag textures)

const char* GetShieldTexture(int primaryProf)
{
    if (primaryProf == 1)  return "GW.EXE_0x448AF925.png"; // Warrior
    if (primaryProf == 10) return "GW.EXE_0x46522695.png"; // Paragon
    return "GW.EXE_0x44FABD1B.png";                        // Default
}

const char* GetSpearTexture(int primaryProf)
{
    if (primaryProf == 1) return "GW.EXE_0x326CECAC.png"; // Warrior
    return "GW.EXE_0x9F1FFD71.png";                       // Default
}

WeaponTextureResult ResolveWeaponTextures(uint16_t weapType, uint8_t weapItemType,
                                                  int primaryProf, int teamId,
                                                  BundleType bundleType)
{
    WeaponTextureResult r;
    r.bundleType = bundleType;

    // (weapon_type, weapon_item_type) lookup
    switch (weapType)
    {
    case 0: // Flag / bundle
        if (weapItemType == 46) {
            if (bundleType == BundleType::RepairKit) {
                r.isNPCIcon = true;
                r.mainTex = "RepairKit.png";
            } else if (bundleType == BundleType::VineSeed) {
                r.isNPCIcon = true;
                r.mainTex = "Vine Seed.png";
            } else {
                r.isFlag = true;
                r.mainTex = (teamId == 2) ? "Blue_flag_waving.svg.png" : "Red_flag_waving.svg.png";
            }
        }
        break;

    case 1: // Bow (1, 46)
        if (weapItemType == 46) r.mainTex = "GW.EXE_0xA1EB9A01.png";
        break;

    case 2: // Axe
        r.mainTex = "GW.EXE_0x1B498CF9.png";
        if (weapItemType == 24) { r.offTex = GetShieldTexture(primaryProf); r.isShield = true; }
        else if (weapItemType == 12) { r.offTex = "GW.EXE_0x07B80EA9.png"; }
        break;

    case 3: // Hammer (3, 46)
        if (weapItemType == 46) r.mainTex = "GW.EXE_0x4AA72219.png";
        break;

    case 4: // Daggers (4, 46)
        if (weapItemType == 46) r.mainTex = "GW.EXE_0x45D46F95.png";
        break;

    case 5: // Scythe (5, 46)
        if (weapItemType == 46) r.mainTex = "GW.EXE_0x4D455BF0.png";
        break;

    case 6: // Spear
        r.mainTex = GetSpearTexture(primaryProf);
        if (weapItemType == 24) { r.offTex = GetShieldTexture(primaryProf); r.isShield = true; }
        else if (weapItemType == 12) { r.offTex = "GW.EXE_0x07B80EA9.png"; }
        break;

    case 7: // Sword
        r.mainTex = "GW.EXE_0x5356CC35.png";
        if (weapItemType == 24) { r.offTex = GetShieldTexture(primaryProf); r.isShield = true; }
        else if (weapItemType == 12) { r.offTex = "GW.EXE_0x07B80EA9.png"; }
        break;

    case 8: case 9: case 10: case 11: case 12: case 13: case 14: // Caster weapons
        if (weapItemType == 46) {
            r.mainTex = "GW.EXE_0x0C5FF809.png"; // Staff (generic)
        } else if (weapItemType == 24) {
            r.mainTex = "GW.EXE_0x99766683.png"; // Wand (generic)
            r.offTex  = GetShieldTexture(primaryProf);
            r.isShield = true;
        } else if (weapItemType == 12) {
            r.mainTex = "GW.EXE_0x99766683.png"; // Wand (generic)
            r.offTex  = "GW.EXE_0x07B80EA9.png"; // Focus (generic)
        }
        break;
    }

    if (!r.mainTex && !r.isFlag && !r.isNPCIcon)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "Unknown weapon combo: type=[%u] item_type=[%u]", weapType, weapItemType);
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
    }

    return r;
}

std::filesystem::path GetEffectsBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "effects"))
        {
            cached = dir / "Textures" / "effects";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "effects";
    return cached;
}

// ---------------------------------------------------------------------------
// Cast bar gradient textures (1-pixel wide, built once)
// ---------------------------------------------------------------------------

ComPtr<ID3D11ShaderResourceView> BuildGradientTex1xN(
    ID3D11Device* device, int height, const GradStop* stops, int nStops)
{
    std::vector<uint32_t> pixels(height);
    for (int y = 0; y < height; ++y)
    {
        float t = (height > 1) ? float(y) / float(height - 1) : 0.f;
        float R, G, B;
        SampleGradient(stops, nStops, t, R, G, B);
        pixels[y] = IM_COL32((uint8_t)R, (uint8_t)G, (uint8_t)B, 255);
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1;
    td.Height = (UINT)height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = pixels.data();
    sd.SysMemPitch = sizeof(uint32_t);

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &sd, &tex))) return nullptr;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return nullptr;
    return srv;
}

ComPtr<ID3D11ShaderResourceView> BuildCastBarFillTex2D(
    ID3D11Device* device, int width, int height,
    const GradStop* hStops, int nH,
    float topBlackAlpha, float botBlackAlpha)
{
    std::vector<uint32_t> pixels(width * height);
    for (int y = 0; y < height; ++y)
    {
        float v = (height > 1) ? float(y) / float(height - 1) : 0.5f;
        float dark;
        if (v < 0.5f)
            dark = topBlackAlpha * (1.0f - v * 2.0f);
        else
            dark = botBlackAlpha * ((v - 0.5f) * 2.0f);

        for (int x = 0; x < width; ++x)
        {
            float u = (width > 1) ? float(x) / float(width - 1) : 0.f;
            float R, G, B;
            SampleGradient(hStops, nH, u, R, G, B);
            R *= (1.0f - dark);
            G *= (1.0f - dark);
            B *= (1.0f - dark);
            pixels[y * width + x] = IM_COL32(
                (uint8_t)std::clamp(R, 0.f, 255.f),
                (uint8_t)std::clamp(G, 0.f, 255.f),
                (uint8_t)std::clamp(B, 0.f, 255.f), 255);
        }
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)width;
    td.Height = (UINT)height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = pixels.data();
    sd.SysMemPitch = (UINT)(width * sizeof(uint32_t));

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &sd, &tex))) return nullptr;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return nullptr;
    return srv;
}

// ---------------------------------------------------------------------------
// Skill icon index & loader (Textures/Skill_Icons/[ID] - Name.jpg)
// ---------------------------------------------------------------------------

std::filesystem::path GetSkillIconsBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "Skill_Icons"))
        {
            cached = dir / "Textures" / "Skill_Icons";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "Skill_Icons";
    return cached;
}

ImTextureID LoadSkillIcon(ReplayWindow* rw, ID3D11Device* device,
                                 int skillId,
                                 std::unordered_map<int, std::string>& index,
                                 std::unordered_map<int, ComPtr<ID3D11ShaderResourceView>>& cache)
{
    auto cit = cache.find(skillId);
    if (cit != cache.end()) return (ImTextureID)cit->second.Get();

    auto iit = index.find(skillId);
    if (iit == index.end()) return nullptr;

    std::wstring wpath(iit->second.begin(), iit->second.end());
    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(wpath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImages(), DXGI_FORMAT_R8G8B8A8_UNORM,
                              DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::Image* src = (converted.GetImageCount() > 0) ? converted.GetImages() : image.GetImages();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)src->width;
    td.Height = (UINT)src->height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = src->pixels;
    sd.SysMemPitch = (UINT)src->rowPitch;

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&td, &sd, &tex);
    if (FAILED(hr)) return nullptr;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (FAILED(hr)) return nullptr;

    cache[skillId] = srv;
    return (ImTextureID)srv.Get();
}

// ---------------------------------------------------------------------------
// Profession icon paths & loaders
// ---------------------------------------------------------------------------

std::filesystem::path GetProfIconsBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "Professions_Icons"))
        {
            cached = dir / "Textures" / "Professions_Icons";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "Professions_Icons";
    return cached;
}

std::filesystem::path GetProfStylizedBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "professions" / "Profession stylized"))
        {
            cached = dir / "Textures" / "professions" / "Profession stylized";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "professions" / "Profession stylized";
    return cached;
}

const char* ProfIconFileName(int profId)
{
    switch (profId) {
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

ImTextureID LoadProfIconGeneric(ID3D11Device* device,
                                       const std::filesystem::path& basePath,
                                       const std::string& key)
{
    static ID3D11Device* s_dev = nullptr;
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_dev) { s_cache.clear(); s_dev = device; }

    auto it = s_cache.find(key);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    auto fullPath = basePath / std::filesystem::path(key);
    if (!std::filesystem::exists(fullPath)) { s_cache[key] = nullptr; return nullptr; }

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) { s_cache[key] = nullptr; return nullptr; }

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) { s_cache[key] = nullptr; return nullptr; }

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) { s_cache[key] = nullptr; return nullptr; }
    }
    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) { s_cache[key] = nullptr; return nullptr; }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf());
    if (FAILED(hr)) { s_cache[key] = nullptr; return nullptr; }

    s_cache[key] = srv;
    return (ImTextureID)srv.Get();
}

ImTextureID LoadProfIcon(ID3D11Device* device, int profId)
{
    const char* fn = ProfIconFileName(profId);
    if (!fn) return nullptr;
    return LoadProfIconGeneric(device, GetProfIconsBasePath(), fn);
}

ImTextureID LoadProfStylized(ID3D11Device* device, int profId,
                                     int teamId,
                                     AgentIconState state)
{
    if (profId < 1 || profId > 10) return nullptr;

    const char* teamStr = (teamId == 2) ? "blue" : "red";
    const char* suffix  = "";
    if (state == AgentIconState::Dead)      suffix = "_dead";
    else if (state == AgentIconState::Knockdown) suffix = "_KD";

    char fn[48];
    snprintf(fn, sizeof(fn), "%d%s%s.png", profId, teamStr, suffix);

    auto basePath = GetProfStylizedBasePath();
    ImTextureID tex = LoadProfIconGeneric(device, basePath, fn);

    // Fallback: if dead/KD icon missing, try alive
    if (!tex && state != AgentIconState::Alive)
    {
        snprintf(fn, sizeof(fn), "%d%s.png", profId, teamStr);
        tex = LoadProfIconGeneric(device, basePath, fn);
    }

    // Fallback: if team-specific icon missing, try plain number
    if (!tex)
    {
        snprintf(fn, sizeof(fn), "%d.png", profId);
        tex = LoadProfIconGeneric(device, basePath, fn);
    }

    return tex;
}

ImTextureID LoadProfIconCL(ID3D11Device* device, int profId)
{
    if (profId < 1 || profId > 10) return nullptr;

    static ID3D11Device* s_dev = nullptr;
    static std::unordered_map<int, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_dev) { s_cache.clear(); s_dev = device; }

    auto it = s_cache.find(profId);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    static std::filesystem::path basePath;
    if (basePath.empty())
    {
        wchar_t exeBuf[MAX_PATH];
        GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
        auto dir = std::filesystem::path(exeBuf).parent_path();
        for (int i = 0; i < 5; i++)
        {
            if (std::filesystem::exists(dir / "Textures" / "professions"))
            { basePath = dir / "Textures" / "professions"; break; }
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
        if (basePath.empty())
            basePath = std::filesystem::path(exeBuf).parent_path() / "Textures" / "professions";
    }

    char fn[16];
    snprintf(fn, sizeof(fn), "%d.png", profId);
    auto fullPath = basePath / fn;
    if (!std::filesystem::exists(fullPath)) { s_cache[profId] = nullptr; return nullptr; }

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) { s_cache[profId] = nullptr; return nullptr; }

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) { s_cache[profId] = nullptr; return nullptr; }

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) { s_cache[profId] = nullptr; return nullptr; }
    }
    const auto* img = (converted.GetImageCount() > 0 ? converted : image).GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)img->width; td.Height = (UINT)img->height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = img->pixels; sd.SysMemPitch = (UINT)img->rowPitch;

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&td, &sd, &tex);
    if (FAILED(hr)) { s_cache[profId] = nullptr; return nullptr; }

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (FAILED(hr)) { s_cache[profId] = nullptr; return nullptr; }

    s_cache[profId] = srv;
    return (ImTextureID)srv.Get();
}

ImTextureID LoadPartyIcon(ID3D11Device* device, const char* filename)
{
    static ID3D11Device* s_cachedDevice = nullptr;
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_cachedDevice) { s_cache.clear(); s_cachedDevice = device; }

    auto it = s_cache.find(filename);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    auto fullPath = GetGameUIBasePath() / std::filesystem::path(filename);
    if (!std::filesystem::exists(fullPath)) return nullptr;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    s_cache[filename] = srv;
    return (ImTextureID)srv.Get();
}

ImTextureID LoadEffectIcon(ID3D11Device* device, const char* filename)
{
    static ID3D11Device* s_cachedDevice = nullptr;
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_cachedDevice) { s_cache.clear(); s_cachedDevice = device; }

    auto it = s_cache.find(filename);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    auto fullPath = GetEffectsBasePath() / std::filesystem::path(filename);
    if (!std::filesystem::exists(fullPath)) return nullptr;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    s_cache[filename] = srv;
    return (ImTextureID)srv.Get();
}

ImTextureID LoadSkillDescIcon(ID3D11Device* device, const char* filename)
{
    static ID3D11Device* s_cachedDevice = nullptr;
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_cachedDevice) { s_cache.clear(); s_cachedDevice = device; }

    auto it = s_cache.find(filename);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    auto fullPath = GetGameUIBasePath() / "Skill Description" / filename;
    if (!std::filesystem::exists(fullPath)) return nullptr;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    s_cache[filename] = srv;
    return (ImTextureID)srv.Get();
}

std::filesystem::path GetOthersUIBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "Others_UI"))
        {
            cached = dir / "Textures" / "Others_UI";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "Others_UI";
    return cached;
}

ImTextureID LoadFlagIcon(ID3D11Device* device, const char* filename)
{
    static ID3D11Device* s_cachedDevice = nullptr;
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_cachedDevice) { s_cache.clear(); s_cachedDevice = device; }

    auto it = s_cache.find(filename);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    auto fullPath = GetOthersUIBasePath() / std::filesystem::path(filename);
    if (!std::filesystem::exists(fullPath)) return nullptr;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    s_cache[filename] = srv;
    return (ImTextureID)srv.Get();
}

ImTextureID LoadSpeechBubbleTexture(ID3D11Device* device)
{
    static ID3D11Device* s_cachedDevice = nullptr;
    static ComPtr<ID3D11ShaderResourceView> s_srv;
    if (device != s_cachedDevice) { s_srv.Reset(); s_cachedDevice = device; }
    if (s_srv.Get()) return (ImTextureID)s_srv.Get();

    auto fullPath = GetOthersUIBasePath() / L"Speech_bubble_0x21997386.dds";
    if (!std::filesystem::exists(fullPath)) return nullptr;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromDDSFile(fullPath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage decompressed;
    if (DirectX::IsCompressed(meta.format))
    {
        hr = DirectX::Decompress(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM, decompressed);
        if (FAILED(hr)) return nullptr;
        image = std::move(decompressed);
    }

    DirectX::ScratchImage converted;
    if (image.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, s_srv.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    return (ImTextureID)s_srv.Get();
}

std::string StripPvpSuffix(const std::string& name)
{
    constexpr const char* kSuffix = " (PvP)";
    constexpr size_t kSuffixLen = 6;
    if (name.size() >= kSuffixLen &&
        name.compare(name.size() - kSuffixLen, kSuffixLen, kSuffix) == 0)
        return name.substr(0, name.size() - kSuffixLen);
    return name;
}

PartyIcons LoadAllPartyIcons(ID3D11Device* dev)
{
    PartyIcons icons;
    icons.weaponSpell = LoadEffectIcon(dev, "WeaponSpell.png");
    icons.enchanted   = LoadPartyIcon(dev, "Enchanted.png");
    icons.condition   = LoadPartyIcon(dev, "Condition.png");
    icons.hexed       = LoadPartyIcon(dev, "Hexed.png");
    return icons;
}

void DrawPartyHealthBar(
    ImDrawList* dl, ImVec2 barTL, float barW, float barH,
    const AgentSnapshot* snap, uint8_t teamId, bool isDead,
    const char* name, const PartyIcons& icons,
    int followedAgentId, int agentId, bool fogHidden,
    ImTextureID flagTex)
{
    ImVec2 barBR(barTL.x + barW, barTL.y + barH);

    // Border
    bool isHovered = ImGui::IsMouseHoveringRect(barTL, barBR);
    bool isFollowed = (followedAgentId == agentId);
    ImU32 borderCol = IM_COL32(0x4E, 0x4D, 0x48, 0xFF);
    if (isFollowed)
        borderCol = IM_COL32(0xCB, 0xAA, 0x09, 0xFF);
    else if (isHovered)
        borderCol = IM_COL32(0x9A, 0x8A, 0x3E, 0xFF);

    if (isFollowed)
    {
        dl->AddRectFilled(ImVec2(barTL.x - 2, barTL.y - 2), ImVec2(barBR.x + 2, barBR.y + 2),
                          IM_COL32(0xD8, 0xD0, 0x73, 0x3C), 3.f);
        dl->AddRect(barTL, barBR, borderCol, 0.f, 0, 2.0f);
        dl->AddRect(ImVec2(barTL.x - 1, barTL.y - 1), ImVec2(barBR.x + 1, barBR.y + 1),
                    IM_COL32(0xD8, 0xD0, 0x73, 0x80), 0.f, 0, 1.0f);
    }
    else
    {
        dl->AddRect(barTL, barBR, borderCol, 0.f, 0, 1.0f);
    }

    // Inner area (1px inset from border)
    ImVec2 innerTL(barTL.x + 1, barTL.y + 1);
    ImVec2 innerBR(barBR.x - 1, barBR.y - 1);
    float innerW = innerBR.x - innerTL.x;
    float innerH = innerBR.y - innerTL.y;

    if (!snap) return;

    if (fogHidden)
    {
        dl->AddRectFilled(innerTL, innerBR, IM_COL32(0x18, 0x18, 0x1C, 0xFF));
        const char* fogText = "???";
        ImVec2 ts = ImGui::CalcTextSize(fogText);
        ImVec2 tp(innerTL.x + (innerW - ts.x) * 0.5f, innerTL.y + (innerH - ts.y) * 0.5f);
        dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 0xCC), fogText);
        dl->AddText(tp, IM_COL32(0x80, 0x80, 0x80, 0xFF), fogText);
        return;
    }

    float healthPct = std::clamp(snap->health_pct, 0.f, 1.f);
    bool hasDeepWound = snap->has_deep_wound && !isDead;

    // Choose gradient by priority
    const Gradient5* fillGrad = nullptr;
    if (isDead)
        fillGrad = (teamId == 1) ? &kDeadRed : &kDeadBlue;
    else if (snap->has_degen_hex)
        fillGrad = &kDegenHex;
    else if (snap->has_poison)
        fillGrad = &kPoison;
    else if (snap->has_bleeding)
        fillGrad = &kBleeding;
    else
        fillGrad = (teamId == 1) ? &kAliveRed : &kAliveBlue;

    // Dead background fills full width
    if (isDead)
    {
        DrawGradientRect(dl, innerTL, innerBR, *fillGrad);
    }
    else
    {
        // Background: dark fill for empty portion
        const Gradient5* deadGrad = (teamId == 1) ? &kDeadRed : &kDeadBlue;
        DrawGradientRect(dl, innerTL, innerBR, *deadGrad);

        // Health fill
        float fillPct = hasDeepWound ? std::min(healthPct, 0.80f) : healthPct;
        if (fillPct > 0.f)
        {
            float fillW = innerW * fillPct;
            DrawGradientRect(dl, innerTL, ImVec2(innerTL.x + fillW, innerBR.y), *fillGrad);
        }

        // Deep wound overlay on rightmost 20%
        if (hasDeepWound)
        {
            float dwStart = innerTL.x + innerW * 0.80f;
            DrawGradientRect(dl, ImVec2(dwStart, innerTL.y), innerBR, kDeepWound);
        }
    }

    // Player name (text with shadow) + eye icon when followed
    if (name && name[0])
    {
        float textOffsetX = 4.f;
        ImVec2 textPos(innerTL.x + textOffsetX, innerTL.y + (innerH - ImGui::GetFontSize()) * 0.5f);
        ImU32 textCol = isDead ? IM_COL32(0x80, 0x80, 0x80, 0xFF) : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
        if (isFollowed)
            textCol = IM_COL32(0xF5, 0xE4, 0x5A, 0xFF);
        dl->AddText(ImVec2(textPos.x + 1, textPos.y + 1), IM_COL32(0, 0, 0, 0xCC), name);
        dl->AddText(textPos, textCol, name);

        if (isFollowed)
        {
            ImVec2 nameSize = ImGui::CalcTextSize(name);
            float eyeCx = textPos.x + nameSize.x + 10.f;
            float eyeCy = textPos.y + ImGui::GetFontSize() * 0.5f;
            float sz = 5.f;
            ImU32 eyeCol = IM_COL32(0xCB, 0xAA, 0x09, 0xFF);
            // Diamond / crosshair-style focus icon
            ImVec2 top(eyeCx, eyeCy - sz);
            ImVec2 right(eyeCx + sz, eyeCy);
            ImVec2 bottom(eyeCx, eyeCy + sz);
            ImVec2 left(eyeCx - sz, eyeCy);
            dl->AddQuadFilled(top, right, bottom, left, IM_COL32(0xCB, 0xAA, 0x09, 0x60));
            dl->AddQuad(top, right, bottom, left, eyeCol, 1.5f);
            dl->AddCircleFilled(ImVec2(eyeCx, eyeCy), 1.5f, eyeCol, 8);
        }
    }

    // Status icons (right-aligned, hidden when dead)
    // Order right-to-left: WeaponSpell, Enchanted, Condition, Hexed
    if (!isDead)
    {
        const float iconSz = std::min(innerH - 2.f, 18.f);
        float iconX = innerBR.x - 2.f;
        float iconY = innerTL.y + (innerH - iconSz) * 0.5f;

        if (snap->has_weapon_spell && icons.weaponSpell)
        {
            iconX -= iconSz;
            dl->AddImage(icons.weaponSpell, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if (snap->has_enchantment && icons.enchanted)
        {
            iconX -= iconSz;
            dl->AddImage(icons.enchanted, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if ((snap->has_condition || snap->has_deep_wound || snap->has_bleeding || snap->has_poison) && icons.condition)
        {
            iconX -= iconSz;
            dl->AddImage(icons.condition, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if (snap->has_hex && icons.hexed)
        {
            iconX -= iconSz;
            dl->AddImage(icons.hexed, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if (flagTex)
        {
            iconX -= iconSz;
            dl->AddImage(flagTex, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
        }
    }
}

// ---------------------------------------------------------------------------
// Meter bar (damage / heal) drawn beside health bars
// ---------------------------------------------------------------------------

void DrawMeterBar(
    ImDrawList* dl, ImVec2 healthTL, float healthW, float slotY, float barH,
    int value, int maxValue, int totalValue,
    bool leftSide, float maxBarW,
    ImU32 barColor)
{
    if (value <= 0 || maxValue <= 0) return;

    float frac = (float)value / (float)maxValue;
    float barW = frac * maxBarW;
    if (barW < 2.f) barW = 2.f;

    ImVec2 tl, br;
    if (leftSide)
    {
        tl = ImVec2(healthTL.x + healthW + 2.f, slotY);
        br = ImVec2(tl.x + barW, slotY + barH);
    }
    else
    {
        br = ImVec2(healthTL.x - 2.f, slotY + barH);
        tl = ImVec2(br.x - barW, slotY);
    }

    dl->AddRectFilled(tl, br, barColor, 2.f);
    dl->AddRect(tl, br, (barColor & 0x00FFFFFF) | 0x90000000, 2.f, 0, 1.f);

    int pct = (totalValue > 0) ? (int)((float)value / (float)totalValue * 100.f + 0.5f) : 0;
    char label[48];
    if (leftSide)
        snprintf(label, sizeof(label), "%d (%d%%)", value, pct);
    else
        snprintf(label, sizeof(label), "(%d%%) %d", pct, value);

    float fontScale = 0.88f;
    float origScale = ImGui::GetFont()->Scale;
    ImGui::GetFont()->Scale *= fontScale;
    ImGui::PushFont(ImGui::GetFont());

    ImVec2 ts = ImGui::CalcTextSize(label);
    float textY = slotY + (barH - ts.y) * 0.5f;
    float textX;
    if (leftSide)
        textX = tl.x + 3.f;
    else
        textX = br.x - ts.x - 3.f;

    constexpr ImU32 kTextCol   = IM_COL32(0xE0, 0xE0, 0xE0, 0xFF);
    constexpr ImU32 kShadowCol = IM_COL32(0, 0, 0, 0xCC);
    dl->AddText(ImVec2(textX + 1, textY + 1), kShadowCol, label);
    dl->AddText(ImVec2(textX, textY), kTextCol, label);

    ImGui::GetFont()->Scale = origScale;
    ImGui::PopFont();
}

void DrawMeterSumText(
    ImDrawList* dl,
    float anchorX, float anchorY, float rowH,
    int dmgSum, int healSum,
    bool showDmg, bool showHeal, bool leftSide)
{
    if (!showDmg && !showHeal) return;

    float fontScale = 0.80f;
    float origScale = ImGui::GetFont()->Scale;
    ImGui::GetFont()->Scale *= fontScale;
    ImGui::PushFont(ImGui::GetFont());

    char label[96];
    if (showDmg && showHeal)
        snprintf(label, sizeof(label), "Dmg: %d | Heal: %d", dmgSum, healSum);
    else if (showDmg)
        snprintf(label, sizeof(label), "Dmg: %d", dmgSum);
    else
        snprintf(label, sizeof(label), "Heal: %d", healSum);

    ImVec2 ts = ImGui::CalcTextSize(label);
    float textY = anchorY + (rowH - ts.y) * 0.5f;
    float textX;
    if (leftSide)
        textX = anchorX + 4.f;
    else
        textX = anchorX - ts.x - 4.f;

    constexpr float kPadX = 5.f, kPadY = 2.f;
    ImVec2 bgTL(textX - kPadX, textY - kPadY);
    ImVec2 bgBR(textX + ts.x + kPadX, textY + ts.y + kPadY);
    dl->AddRectFilled(bgTL, bgBR, IM_COL32(0x10, 0x10, 0x14, 0xD0), 4.f);
    dl->AddRect(bgTL, bgBR, IM_COL32(0x50, 0x4A, 0x38, 0xA0), 4.f, 0, 1.f);

    constexpr ImU32 kSumTextCol = IM_COL32(0xD8, 0xCE, 0xA6, 0xFF);
    dl->AddText(ImVec2(textX + 1, textY + 1), IM_COL32(0, 0, 0, 0xCC), label);
    dl->AddText(ImVec2(textX, textY), kSumTextCol, label);

    ImGui::GetFont()->Scale = origScale;
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------
// Debug window: StoC Events Viewer
// ---------------------------------------------------------------------------

ImU32 StoCCategoryColor(StoCCategory cat)
{
    switch (cat) {
    case StoCCategory::AgentMovement: return IM_COL32(160, 160, 160, 255);
    case StoCCategory::Skill:         return IM_COL32(80,  140, 255, 255);
    case StoCCategory::AttackSkill:   return IM_COL32(255, 165, 60,  255);
    case StoCCategory::BasicAttack:   return IM_COL32(240, 220, 60,  255);
    case StoCCategory::Combat:        return IM_COL32(255, 70,  70,  255);
    case StoCCategory::Jumbo:         return IM_COL32(180, 100, 255, 255);
    case StoCCategory::Unknown:       return IM_COL32(220, 220, 220, 255);
    case StoCCategory::Lifecycle:     return IM_COL32(100, 220, 160, 255);
    case StoCCategory::MapObject:     return IM_COL32(220, 180, 100, 255);
    case StoCCategory::DoorEvent:     return IM_COL32(180, 130, 220, 255);
    case StoCCategory::FlagEvent:     return IM_COL32(60,  200, 60,  255);
    default:                          return IM_COL32(255, 255, 255, 255);
    }
}

int StoCCategoryCount(const StoCData& d, StoCCategory cat)
{
    switch (cat) {
    case StoCCategory::AgentMovement: return static_cast<int>(d.agentMovement.size());
    case StoCCategory::Skill:         return static_cast<int>(d.skill.size());
    case StoCCategory::AttackSkill:   return static_cast<int>(d.attackSkill.size());
    case StoCCategory::BasicAttack:   return static_cast<int>(d.basicAttack.size());
    case StoCCategory::Combat:        return static_cast<int>(d.combat.size());
    case StoCCategory::Jumbo:         return static_cast<int>(d.jumbo.size());
    case StoCCategory::Unknown:       return static_cast<int>(d.unknown.size());
    case StoCCategory::Lifecycle:     return static_cast<int>(d.lifecycle.size());
    case StoCCategory::MapObject:     return static_cast<int>(d.mapObject.size());
    case StoCCategory::DoorEvent:     return static_cast<int>(d.doorEvents.size());
    case StoCCategory::FlagEvent:     return d.flagEvents.totalCount();
    default: return 0;
    }
}

std::string GetAgentDisplayName(const ReplayContext& ctx, int agentId)
{
    if (agentId <= 0)
        return std::format("Agent {} (Missing)", agentId);

    auto it = ctx.agents.find(agentId);
    if (it == ctx.agents.end())
        return std::format("Agent {} (Missing)", agentId);

    auto& ard = it->second;
    switch (ard.type) {
    case AgentType::Player: return std::format("{} (Player)", ard.playerName);
    case AgentType::NPC:    return std::format("{} (NPC)", ard.categoryName);
    case AgentType::Gadget: return std::format("{} (Gadget)", ard.categoryName);
    default:                return std::format("Agent {} (Unknown)", agentId);
    }
}

std::string GetSkillDisplayName(int skillId)
{
    if (skillId <= 0)
        return "None";

    auto& db = GetSkillDatabase();
    if (db.IsLoaded())
    {
        const SkillInfo* si = db.Get(skillId);
        if (si && !si->name.empty())
            return si->name;
    }

    // NPC / Guild Lord skills missing from the skill database
    static const std::unordered_map<int, const char*> s_overrides = {
        {3205, "Entourage"},
    };
    auto ov = s_overrides.find(skillId);
    if (ov != s_overrides.end()) return ov->second;

    return std::format("Skill {}", skillId);
}

int ResolveTarget(int targetId, int casterId)
{
    return (targetId == 0) ? casterId : targetId;
}

const char* JumboPartyLabel(int partyValue)
{
    if (partyValue == 1635021873) return "Party 1";
    if (partyValue == 1635021874) return "Party 2";
    return "Unknown";
}

// ---------------------------------------------------------------------------

void ReplayWindow::Clear()
{
    auto* context = m_deviceResources->GetD3DDeviceContext();
    auto* rtv     = m_deviceResources->GetRenderTargetView();
    auto* dsv     = m_deviceResources->GetDepthStencilView();

    const auto& clearColor = m_mapRenderer->GetClearColor();
    float color[4] = { clearColor.x, clearColor.y, clearColor.z, clearColor.w };
    context->ClearRenderTargetView(rtv, color);
    context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);

    auto vp = m_deviceResources->GetScreenViewport();
    context->RSSetViewports(1, &vp);
}

// ---------------------------------------------------------------------------
// IDeviceNotify
// ---------------------------------------------------------------------------

void ReplayWindow::OnDeviceLost()   {}
void ReplayWindow::OnDeviceRestored() {}

// ---------------------------------------------------------------------------
// Window sizing
// ---------------------------------------------------------------------------

void ReplayWindow::OnWindowSizeChanged(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (!m_deviceResources) return;

    if (m_deviceResources->WindowSizeChanged(width, height))
    {
        if (m_mapRenderer)
            m_mapRenderer->OnViewPortChanged(static_cast<float>(width), static_cast<float>(height));
    }
}

std::filesystem::path FindTexturesDDSDir()
{
    wchar_t exeBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    auto dir = std::filesystem::path(exeBuf).parent_path();
    for (; ; dir = dir.parent_path())
    {
        auto candidate = dir / L"Textures" / L"DDS";
        if (std::filesystem::is_directory(candidate))
            return candidate;
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
    }
    return {};
}

void ReplayWindow::OnDestroy()
{
    SaveAutoCamSettings();
    m_alive = false;
}

// ---------------------------------------------------------------------------
// Phase 2+ stub
// ---------------------------------------------------------------------------

void ReplayWindow::LoadReplayData(const std::filesystem::path& matchFolderPath)
{
    m_replayCtx.matchFolderPath = matchFolderPath;
}

// ---------------------------------------------------------------------------
// Win32 message handler
// ---------------------------------------------------------------------------

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK ReplayWindow::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* rw = reinterpret_cast<ReplayWindow*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    // Forward to ImGui if initialized (save/restore main app context).
    // Split capture checks: keyboard only blocks when a text input widget is
    // active (WantTextInput), so WASD camera movement works even when an ImGui
    // window (e.g. Agent Offset) has focus. Mouse is blocked only when the
    // cursor is over an ImGui window (WantCaptureMouse).
    bool imguiCaptureMouse = false;
    bool imguiCaptureKeys  = false;
    if (rw && rw->m_imguiInitialized)
    {
        ImGuiContext* prevCtx = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(rw->m_imguiContext);

        if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
        {
            ImGui::SetCurrentContext(prevCtx);
            return true;
        }

        imguiCaptureMouse = ImGui::GetIO().WantCaptureMouse;
        imguiCaptureKeys  = ImGui::GetIO().WantTextInput || rw->m_clSkillSearchFocused
                          || rw->m_annotationMgr.IsBookmarkPopupActive();
        ImGui::SetCurrentContext(prevCtx);
    }

    bool isReady = rw && rw->m_loadingPhase == LoadingPhase::Ready;
    bool keyAllowed   = isReady && !imguiCaptureKeys;
    bool mouseAllowed = isReady && !imguiCaptureMouse;

    switch (message)
    {
    case WM_KEYDOWN:
        if (keyAllowed && rw->m_inputManager)
            rw->m_inputManager->OnKeyDown(wParam, hWnd);
        break;

    case WM_KEYUP:
        if (keyAllowed && rw->m_inputManager)
            rw->m_inputManager->OnKeyUp(wParam, hWnd);
        break;

    case WM_INPUT:
        if (mouseAllowed && !rw->m_pipHovered && rw->m_mapRenderer)
        {
            bool dragging = rw->m_rightMouseDown || rw->m_leftMouseDown;
            if (dragging)
            {
                UINT dwSize = sizeof(RAWINPUT);
                BYTE lpb[sizeof(RAWINPUT)];
                GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER));
                auto* raw = reinterpret_cast<RAWINPUT*>(lpb);
                if (raw->header.dwType == RIM_TYPEMOUSE)
                {
                    float dx = static_cast<float>(raw->data.mouse.lLastX);
                    float dy = static_cast<float>(raw->data.mouse.lLastY);
                    Camera* cam = rw->m_mapRenderer->GetCamera();
                    const float panMul = GuiGlobalConstants::ClampReplayCameraSensitivityMultiplier(
                        GuiGlobalConstants::replay_camera_pan_speed_multiplier);
                    const float rotMul = GuiGlobalConstants::ClampReplayCameraSensitivityMultiplier(
                        GuiGlobalConstants::replay_camera_rotation_speed_multiplier);

                    if (rw->m_leftMouseDown)
                    {
                        XMFLOAT3 right = cam->GetRight3f();
                        XMFLOAT3 up    = cam->GetUp3f();
                        XMFLOAT3 pos   = cam->GetPosition3f();
                        float s = rw->m_panSpeed * panMul;
                        pos.x += (-dx * right.x + dy * up.x) * s;
                        pos.y += (-dx * right.y + dy * up.y) * s;
                        pos.z += (-dx * right.z + dy * up.z) * s;
                        cam->SetPosition(pos.x, pos.y, pos.z);
                    }

                    if (rw->m_rightMouseDown && !rw->m_topViewActive)
                    {
                        const auto& hk = ReplayHotkeys::Get();
                        float radX = DirectX::XMConvertToRadians(0.25f * dx * rotMul) * (hk.invertMouseX ? -1.f : 1.f);
                        float radY = DirectX::XMConvertToRadians(0.25f * dy * rotMul) * (hk.invertMouseY ? -1.f : 1.f);

                        if (rw->m_cameraMode == CameraMode::FollowAgent)
                        {
                            rw->m_followYaw   += radX;
                            rw->m_followPitch  = std::clamp(rw->m_followPitch + radY,
                                                            rw->kFollowMinPitch, rw->kFollowMaxPitch);
                        }
                        else
                        {
                            cam->OnMouseMove(radX, radY);
                        }
                    }
                }
                if (rw->m_leftMouseDown)
                    SetCursorPos(rw->m_mouseDragOrigin.x, rw->m_mouseDragOrigin.y);
            }
        }
        break;

    case WM_LBUTTONDOWN:
        if (mouseAllowed && rw && rw->m_cameraMode != CameraMode::FollowAgent
            && !rw->m_annotationMgr.draw_mode_active)
        {
            rw->m_leftClickPending = true;
            GetCursorPos(&rw->m_mouseDragOrigin);
        }
        break;

    case WM_LBUTTONUP:
        if (rw)
        {
            if (rw->m_leftClickPending)
                rw->m_leftClickPending = false;

            if (rw->m_leftMouseDown)
            {
                rw->m_leftMouseDown = false;
                if (!rw->m_rightMouseDown)
                {
                    SetCursorPos(rw->m_mouseDragOrigin.x, rw->m_mouseDragOrigin.y);
                    ShowCursor(TRUE);
                    ReleaseCapture();
                }
                else
                {
                    ShowCursor(TRUE);
                    RECT clip = { rw->m_mouseDragOrigin.x, rw->m_mouseDragOrigin.y,
                                  rw->m_mouseDragOrigin.x + 1, rw->m_mouseDragOrigin.y + 1 };
                    ClipCursor(&clip);
                }
            }
        }
        break;

    case WM_RBUTTONDOWN:
        if (mouseAllowed && !rw->m_pipHovered && rw)
        {
            rw->m_rightMouseDown = true;
            if (!rw->m_leftMouseDown)
            {
                GetCursorPos(&rw->m_mouseDragOrigin);
                RECT clip = { rw->m_mouseDragOrigin.x, rw->m_mouseDragOrigin.y,
                              rw->m_mouseDragOrigin.x + 1, rw->m_mouseDragOrigin.y + 1 };
                ClipCursor(&clip);
                SetCapture(hWnd);
            }
        }
        break;

    case WM_RBUTTONUP:
        if (rw && rw->m_rightMouseDown)
        {
            rw->m_rightMouseDown = false;
            ClipCursor(nullptr);
            if (!rw->m_leftMouseDown)
                ReleaseCapture();
        }
        break;

    case WM_MBUTTONDOWN:
        if (mouseAllowed && rw->m_inputManager)
            rw->m_inputManager->OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, hWnd, message);
        break;

    case WM_MBUTTONUP:
        if (mouseAllowed && rw->m_inputManager)
            rw->m_inputManager->OnMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, hWnd, message);
        break;

    case WM_MOUSEWHEEL:
        if (mouseAllowed && rw->m_mapRenderer)
        {
            float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            const float zoomMul = GuiGlobalConstants::ClampReplayCameraSensitivityMultiplier(
                GuiGlobalConstants::replay_camera_zoom_speed_multiplier);

            if (rw->m_topViewActive)
            {
                Camera* cam = rw->m_mapRenderer->GetCamera();
                XMFLOAT3 pos = cam->GetPosition3f();
                float zoomFactor = (delta > 0)
                    ? (1.f + (0.90f - 1.f) * zoomMul)
                    : (1.f + (1.12f - 1.f) * zoomMul);
                pos.y = std::max(200.f, pos.y * zoomFactor);
                cam->SetPosition(pos.x, pos.y, pos.z);
            }
            else if (rw->m_cameraMode == CameraMode::FollowAgent)
            {
                float factor = (delta > 0)
                    ? (1.f + (0.85f - 1.f) * zoomMul)
                    : (1.f + (1.18f - 1.f) * zoomMul);
                rw->m_followDistTarget = std::clamp(
                    rw->m_followDistTarget * factor,
                    rw->kFollowMinDist, rw->kFollowMaxDist);
            }
            else
            {
                Camera* cam = rw->m_mapRenderer->GetCamera();
                XMFLOAT3 look = cam->GetLook3f();
                XMFLOAT3 pos  = cam->GetPosition3f();
                float z = delta * rw->m_zoomSpeed * zoomMul;
                pos.x += look.x * z;
                pos.y += look.y * z;
                pos.z += look.z * z;
                cam->SetPosition(pos.x, pos.y, pos.z);
            }
        }
        break;

    case WM_SETCURSOR:
        g_CursorInClientArea = (LOWORD(lParam) == HTCLIENT);
        if (g_CursorInClientArea && g_Cursors.loaded)
        {
            if (rw && rw->m_leftMouseDown)
                return TRUE;  // cursor hidden during left-drag pan
            if (rw && rw->m_rightMouseDown)
            {
                ::SetCursor(g_Cursors.Get(CursorMode::Precision));
                return TRUE;
            }
            if (g_DraggingWindow)
            {
                ::SetCursor(g_Cursors.Get(CursorMode::Move));
                return TRUE;
            }
            HCURSOR cur = g_Cursors.Get(g_CurrentCursor);
            if (cur) { ::SetCursor(cur); return TRUE; }
        }
        break;

    case WM_MOUSELEAVE:
        if (rw && rw->m_inputManager)
            rw->m_inputManager->OnMouseLeave(hWnd);
        break;

    case WM_ACTIVATE:
        if (rw && rw->m_inputManager)
        {
            if (LOWORD(wParam) != WA_INACTIVE)
            {
                rw->m_inputManager->ReRegisterRawInput();
                OutputDebugStringA("ReplayWindow: Activated (input re-attached)\n");
            }
            else
            {
                rw->m_inputManager->OnFocusLost();
                OutputDebugStringA("ReplayWindow: Deactivated (input detached)\n");
            }
        }
        break;

    case WM_SIZE:
        if (rw && wParam != SIZE_MINIMIZED)
            rw->OnWindowSizeChanged(LOWORD(lParam), HIWORD(lParam));
        break;

    case WM_DESTROY:
        if (rw) rw->OnDestroy();
        break;

    case WM_GETMINMAXINFO:
        if (lParam)
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 320;
            info->ptMinTrackSize.y = 200;
        }
        break;

    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}


// ---------------------------------------------------------------------------
// Piano Roll Panel
// ---------------------------------------------------------------------------

ImU32 PianoRollSkillColor(int skillType, bool bright)
{
    if (skillType >= 2 && skillType <= 12) return bright ? IM_COL32(0xFF,0xB8,0x20,0xFF) : IM_COL32(0x4A,0x3A,0x00,0xFF);
    if (skillType == 31)                   return bright ? IM_COL32(0xFF,0xB8,0x20,0xFF) : IM_COL32(0x4A,0x3A,0x00,0xFF);
    if (skillType == 24)                   return bright ? IM_COL32(0x90,0x40,0xC0,0xFF) : IM_COL32(0x50,0x1A,0x70,0xFF);
    if (skillType == 23 || skillType == 33 || skillType == 34 || skillType == 15)
                                           return bright ? IM_COL32(0x20,0xC0,0xA0,0xFF) : IM_COL32(0x0A,0x3A,0x3A,0xFF);
    if (skillType == 27 || skillType == 26 || skillType == 28)
                                           return bright ? IM_COL32(0x20,0xC0,0xA0,0xFF) : IM_COL32(0x0A,0x3A,0x3A,0xFF);
    if (skillType == 13 || skillType == 18 || skillType == 19 || skillType == 20 || skillType == 32)
                                           return bright ? IM_COL32(0x40,0xC0,0x60,0xFF) : IM_COL32(0x1A,0x5A,0x2A,0xFF);
    if (skillType == 14 || skillType == 16 || skillType == 17 || skillType == 21 ||
        skillType == 25 || skillType == 29 || skillType == 30)
                                           return bright ? IM_COL32(0x60,0x60,0x60,0xFF) : IM_COL32(0x2A,0x2A,0x2A,0xFF);
    if (skillType == 22)                   return bright ? IM_COL32(0x40,0xC0,0x60,0xFF) : IM_COL32(0x1A,0x5A,0x2A,0xFF);
    return bright ? IM_COL32(0x60,0x60,0x60,0xFF) : IM_COL32(0x2A,0x2A,0x2A,0xFF);
}

const char* WeaponTypeName(uint8_t t)
{
    switch (t) {
    case 1:  return "Axe";
    case 2:  return "Sword";
    case 3:  return "Hammer";
    case 4:  return "Bow";
    case 5:  return "Daggers";
    case 6:  return "Scythe";
    case 7:  return "Spear";
    case 8:  return "Staff";
    case 9:  return "Wand";
    case 10: return "Focus";
    case 11: return "Shield";
    default: return "?";
    }
}

// ---------------------------------------------------------------------------
// Heatmap settings persistence
// ---------------------------------------------------------------------------

std::filesystem::path GetHeatmapSettingsPath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    auto settingsDir = dir / "settings";
    if (!std::filesystem::exists(settingsDir))
        std::filesystem::create_directories(settingsDir);
    return settingsDir / "heatmap_settings.json";
}
