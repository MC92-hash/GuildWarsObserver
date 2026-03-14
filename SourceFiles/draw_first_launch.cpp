#include "pch.h"
#include "draw_first_launch.h"
#include "draw_gui_for_open_dat_file.h"
#include "GuiGlobalConstants.h"
#include "TextureCache.h"
#include "imgui.h"
#include <filesystem>
#include <chrono>

namespace
{
    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_startTime;
    static bool  s_startTimeSet = false;
    static constexpr float kSimDurationSec = 2.5f;  // wall-clock seconds for 0→90%
    static Clock::time_point s_hitFullTime;          // when progress first reached 100%
    static bool  s_hitFull = false;
    static constexpr float kHoldAtFullSec = 1.0f;   // linger at 100% before transition
    static bool  s_datDialogOpen = false;
    static char  s_pathBuffer[512] = "";
    static std::string s_errorMsg;
    static bool  s_showPathMissingNote = false;
    static bool  s_overlayInitialized = false;

    static std::string GetLoadingScreenPath()
    {
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
            return "";
        auto dir = std::filesystem::path(exePath).parent_path();
        for (int i = 0; i < 6; i++)
        {
            auto p = dir / "Textures" / "Launch_screen" / "GWOBS_Loading_Screen_1.png";
            if (std::filesystem::exists(p))
                return p.string();
            if (!dir.has_parent_path() || dir == dir.parent_path())
                break;
            dir = dir.parent_path();
        }
        return "";
    }

    static std::string GetDefaultBrowsePath()
    {
        static const char* candidates[] = {
            "C:\\Program Files\\Guild Wars\\Gw.dat",
            "C:\\Program Files (x86)\\Guild Wars\\Gw.dat",
            "D:\\Guild Wars\\Gw.dat",
        };
        for (const char* c : candidates)
        {
            if (std::filesystem::exists(c))
                return std::filesystem::path(c).parent_path().string();
        }
        return "C:\\";
    }

    static void OpenBrowseDialog()
    {
        std::string initial = GetDefaultBrowsePath();
        if (!GuiGlobalConstants::saved_gw_dat_path.empty())
        {
            auto parent = std::filesystem::path(GuiGlobalConstants::saved_gw_dat_path).parent_path();
            if (std::filesystem::exists(parent))
                initial = parent.string();
        }
        ImGuiFileDialog::Instance()->OpenDialog("FirstLaunchChooseGwDat", "Select Gw.dat",
            ".dat", initial + "\\.");
        s_datDialogOpen = true;
    }
}

bool draw_first_launch(bool dat_path_is_set, float dat_load_fraction)
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 display = io.DisplaySize;

    // Record the first call time so simulated progress uses wall-clock time.
    // This correctly accounts for time spent in Initialize() before the
    // render loop starts.
    if (!s_startTimeSet)
    {
        s_startTime = Clock::now();
        s_startTimeSet = true;
    }

    float elapsed = std::chrono::duration<float>(Clock::now() - s_startTime).count();
    float simFraction = std::min(elapsed / kSimDurationSec, 1.f);  // 0→1 over kSimDurationSec
    bool simulatedDone = (simFraction >= 1.f);

    // Displayed progress:
    //   0  → 0.9   simulated ramp (wall-clock)
    //   0.9 → 1.0  mapped from dat_load_fraction when path is set
    float displayProgress;
    if (!simulatedDone)
    {
        displayProgress = simFraction * 0.9f;
    }
    else if (dat_path_is_set)
    {
        displayProgress = 0.9f + 0.1f * std::clamp(dat_load_fraction, 0.f, 1.f);
    }
    else
    {
        displayProgress = 0.9f;
    }

    bool atFull = simulatedDone && dat_path_is_set &&
                  dat_load_fraction >= 1.f && displayProgress >= 1.f;
    if (atFull && !s_hitFull)
    {
        s_hitFull = true;
        s_hitFullTime = Clock::now();
    }
    if (s_hitFull)
    {
        float holdElapsed = std::chrono::duration<float>(Clock::now() - s_hitFullTime).count();
        if (holdElapsed >= kHoldAtFullSec)
            return true;
    }

    // Status text
    const char* statusText;
    if (!simulatedDone)
        statusText = "Initializing...";
    else if (!dat_path_is_set)
        statusText = "Please locate your Guild Wars installation to continue.";
    else if (dat_load_fraction < 1.f)
        statusText = "Loading Guild Wars data...";
    else
        statusText = "Ready";

    // --- Fullscreen background window ---
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.078f, 0.094f, 0.118f, 1.f));

    ImGui::Begin("##LoadingScreenBG", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background image
    static std::string cachedLoadPath = GetLoadingScreenPath();
    ImTextureID tex = cachedLoadPath.empty() ? nullptr : GetTextureCache().GetTexture(cachedLoadPath);
    if (tex)
        dl->AddImage(tex, ImVec2(0, 0), display);

    // Progress bar
    {
        float barW = display.x * 0.4f;
        float barH = 6.f;
        float barX = (display.x - barW) * 0.5f;
        float barY = display.y * 0.85f;

        dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH),
            IM_COL32(40, 44, 52, 255), 3.f);
        dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW * displayProgress, barY + barH),
            IM_COL32(74, 144, 216, 230), 3.f);
    }

    // Status text (centered)
    {
        ImVec2 textSize = ImGui::CalcTextSize(statusText);
        float tx = (display.x - textSize.x) * 0.5f;
        float ty = display.y * 0.78f;
        dl->AddText(ImVec2(tx, ty), IM_COL32(240, 240, 240, 255), statusText);
    }

    ImGui::End();
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(3);

    // --- Dat-path overlay (only when no path is configured and simulated progress done) ---
    if (simulatedDone && !dat_path_is_set)
    {
        if (!s_overlayInitialized)
        {
            s_overlayInitialized = true;
            if (!GuiGlobalConstants::saved_gw_dat_path.empty() &&
                !std::filesystem::exists(GuiGlobalConstants::saved_gw_dat_path))
            {
                s_showPathMissingNote = true;
                size_t len = std::min(GuiGlobalConstants::saved_gw_dat_path.size(), sizeof(s_pathBuffer) - 1);
                memcpy(s_pathBuffer, GuiGlobalConstants::saved_gw_dat_path.c_str(), len);
                s_pathBuffer[len] = '\0';
            }
        }

        float panelW = 480.f;
        float panelH = s_showPathMissingNote ? 250.f : 220.f;
        ImVec2 panelPos((display.x - panelW) * 0.5f, (display.y - panelH) * 0.5f);

        ImGui::SetNextWindowPos(panelPos);
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.039f, 0.055f, 0.071f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.12f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));

        if (ImGui::Begin("##DatPathPrompt", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
        {
            {
                const char* title = "Guild Wars installation not found.";
                float titleW = ImGui::CalcTextSize(title).x;
                ImGui::SetCursorPosX((panelW - titleW) * 0.5f);
                ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 1.f), "%s", title);
            }

            ImGui::Spacing();

            {
                const char* sub = "Please locate your GW.dat file to complete setup.";
                float subW = ImGui::CalcTextSize(sub).x;
                ImGui::SetCursorPosX((panelW - subW) * 0.5f);
                ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.f), "%s", sub);
            }

            ImGui::Spacing();
            ImGui::Spacing();

            ImGui::Text("Path:");
            ImGui::SetNextItemWidth(panelW - 40.f);
            ImGui::InputText("##datpath", s_pathBuffer, sizeof(s_pathBuffer));

            if (s_showPathMissingNote)
            {
                ImGui::TextColored(ImVec4(0.878f, 0.471f, 0.188f, 1.f),
                    "Previously configured file was not found at this location.");
            }

            if (!s_errorMsg.empty())
            {
                ImGui::TextColored(ImVec4(1.f, 0.376f, 0.376f, 1.f), "%s", s_errorMsg.c_str());
            }

            ImGui::Spacing();
            ImGui::Spacing();

            float btnTotalW = 100.f + 8.f + 100.f;
            ImGui::SetCursorPosX((panelW - btnTotalW) * 0.5f);

            if (ImGui::Button("Browse...", ImVec2(100, 28)))
                OpenBrowseDialog();

            ImGui::SameLine(0.f, 8.f);

            if (ImGui::Button("Confirm", ImVec2(100, 28)))
            {
                s_errorMsg.clear();
                std::string path(s_pathBuffer);
                while (!path.empty() && (path.back() == ' ' || path.back() == '\t'))
                    path.pop_back();

                if (path.empty())
                {
                    s_errorMsg = "Please select a GW.dat file.";
                }
                else if (!std::filesystem::exists(path))
                {
                    s_errorMsg = "File not found. Please check the path.";
                }
                else
                {
                    GuiGlobalConstants::saved_gw_dat_path = path;
                    GuiGlobalConstants::SaveSettings();

                    std::wstring wpath(path.begin(), path.end());
                    gw_dat_path = wpath;
                    gw_dat_path_set = true;

                    s_showPathMissingNote = false;
                    s_overlayInitialized = false;
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }

    // Handle file dialog result
    if (s_datDialogOpen && ImGuiFileDialog::Instance()->Display("FirstLaunchChooseGwDat",
        ImGuiWindowFlags_NoCollapse, ImVec2(500, 400)))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string fp = ImGuiFileDialog::Instance()->GetFilePathName();
            size_t len = std::min(fp.size(), sizeof(s_pathBuffer) - 1);
            memcpy(s_pathBuffer, fp.c_str(), len);
            s_pathBuffer[len] = '\0';
        }
        ImGuiFileDialog::Instance()->Close();
        s_datDialogOpen = false;
    }

    return false;
}

void draw_first_launch_reset_state()
{
    s_startTimeSet = false;
    s_hitFull = false;
    s_datDialogOpen = false;
    s_pathBuffer[0] = '\0';
    s_errorMsg.clear();
    s_showPathMissingNote = false;
    s_overlayInitialized = false;
}
