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
#include "../ThirdParty/nanosvg/nanosvg.h"
#include "../ThirdParty/nanosvg/nanosvgrast.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <algorithm>
#include <numeric>
#include <json.hpp>
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// Extracted from ReplayWindow.cpp (partial-class split). These remain
// ReplayWindow:: member functions; only their definitions live here.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Match Notepad — floating text panel for per-match notes
// ---------------------------------------------------------------------------

void ReplayWindow::DrawNotepad()
{
    if (!m_showNotepad) return;

    // Sync buffer when match changes or on first open
    if (m_notepadMatchId != m_matchMeta.folder_name)
    {
        m_notepadBuffer  = MatchNotes::Get().GetNote(m_matchMeta.folder_name);
        m_notepadMatchId = m_matchMeta.folder_name;
    }

    ImGuiIO& io = ImGui::GetIO();
    float vpW = io.DisplaySize.x;
    float vpH = io.DisplaySize.y;

    ImGui::SetNextWindowSizeConstraints(ImVec2(250.f, 150.f), ImVec2(vpW, vpH));
    if (m_panelLayout.HasSavedSize("notepad"))
        m_panelLayout.ApplySize("notepad");
    else
        ImGui::SetNextWindowSize(ImVec2(380.f, 300.f), ImGuiCond_FirstUseEver);
    m_panelLayout.ApplyPosition("notepad");

    // Gold-accented dark panel styling (matches CombatLog / other panels)
    ImGui::PushStyleColor(ImGuiCol_WindowBg,             ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,              ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,        ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,               ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,              ImVec4(0.04f, 0.05f, 0.06f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          ImVec4(1.f, 1.f, 1.f, 0.04f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        ImVec4(0.80f, 0.68f, 0.30f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1.f, 0.84f, 0.39f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  ImVec4(1.f, 0.84f, 0.39f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,    6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,  1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 4.f);

    if (!ImGui::Begin("Match Notepad", &m_showNotepad))
    {
        m_panelLayout.TrackWindow("notepad");
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(9);
        return;
    }

    m_panelLayout.TrackWindow("notepad");

    // Clamp window to viewport
    {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, 0.f, std::max(0.f, vpW - sz.x));
        float cy = std::clamp(pos.y, 0.f, std::max(0.f, vpH - sz.y));
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));
    }

    // Match name header
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.f), "%s", m_matchMeta.folder_name.c_str());
    ImGui::Separator();

    // Multiline text editor filling remaining space
    if (ImGui::InputTextMultiline("##notepad", &m_notepadBuffer,
                                  ImVec2(-FLT_MIN, -FLT_MIN)))
    {
        MatchNotes::Get().SetNote(m_matchMeta.folder_name, m_notepadBuffer);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(9);
}
