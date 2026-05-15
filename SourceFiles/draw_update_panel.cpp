#include "pch.h"
#include "draw_update_panel.h"
#include "Net/UpdateChecker.h"
#include "GuiGlobalConstants.h"
#include "imgui.h"

static bool s_releaseNotesOpen = false;
static std::string s_releaseNotesText;

void draw_update_notification(UpdateChecker* checker, HWND appWindow, bool suppressIfIdle)
{
    if (!checker)
        return;

    auto state = checker->GetState();

    // Nothing to show
    if (state == UpdateChecker::State::Idle)
        return;

    // Suppress entirely during loading screen — the central update banner handles all states.
    if (suppressIfIdle)
        return;

    // User dismissed this notification
    if ((state == UpdateChecker::State::UpdateAvailable ||
         state == UpdateChecker::State::ReadyToInstall) && checker->IsDismissed())
        return;

    // Auto-dismiss if the available version matches the previously dismissed version
    if (state == UpdateChecker::State::UpdateAvailable && !checker->IsUserInitiated())
    {
        std::string ver = checker->GetLatestVersion();
        if (!ver.empty() && ver == GuiGlobalConstants::dismissed_update_version)
        {
            checker->Dismiss();
            return;
        }
    }

    // For automatic (non-user-initiated) checks, only show when an update is actually available
    if (!checker->IsUserInitiated() &&
        state != UpdateChecker::State::UpdateAvailable &&
        state != UpdateChecker::State::Downloading &&
        state != UpdateChecker::State::ReadyToInstall)
        return;

    // Position: bottom-right, above the sync status and play-download panels
    ImGuiIO& io = ImGui::GetIO();
    float panelW = 320.f;
    ImVec2 pos(io.DisplaySize.x - panelW - 16.f, io.DisplaySize.y - 200.f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.f, 1.f));
    ImGui::SetNextWindowSize(ImVec2(panelW, 0.f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 8.f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.12f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.75f, 0.37f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    if (ImGui::Begin("##UpdateNotification", nullptr, flags))
    {
        switch (state)
        {
        case UpdateChecker::State::Checking:
        {
            float time = static_cast<float>(ImGui::GetTime());
            const char* spinner = "|/-\\";
            char spin = spinner[static_cast<int>(time * 8.f) % 4];
            ImGui::Text("%c  Checking for updates...", spin);
            break;
        }

        case UpdateChecker::State::UpdateAvailable:
        {
            std::string version = checker->GetLatestVersion();
            ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 1.f), "Update available: %s", version.c_str());
            ImGui::Dummy(ImVec2(0, 2.f));

            if (ImGui::Button("Download"))
                checker->StartDownload();

            ImGui::SameLine();
            float availW = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availW - ImGui::CalcTextSize("X").x);
            if (ImGui::SmallButton("X"))
            {
                checker->Dismiss();
                GuiGlobalConstants::dismissed_update_version = version;
            }
            break;
        }

        case UpdateChecker::State::Downloading:
        {
            float time = static_cast<float>(ImGui::GetTime());
            const char* spinner = "|/-\\";
            char spin = spinner[static_cast<int>(time * 8.f) % 4];
            ImGui::Text("%c  Downloading update...", spin);
            ImGui::Dummy(ImVec2(0, 2.f));

            float progress = checker->GetDownloadProgress();
            ImGui::ProgressBar(progress, ImVec2(-1.f, 16.f));

            double recvMB = checker->GetDownloadedBytes() / (1024.0 * 1024.0);
            double totalMB = checker->GetTotalBytes() / (1024.0 * 1024.0);
            ImGui::Text("%.1f / %.1f MB", recvMB, totalMB);

            if (ImGui::Button("Cancel"))
                checker->Cancel();
            break;
        }

        case UpdateChecker::State::ReadyToInstall:
        {
            // Auto-install if requested by the loading screen's "Download & Install"
            if (checker->ShouldAutoInstall())
            {
                checker->SetAutoInstall(false);
                checker->ApplyAndRestart(appWindow);
            }

            ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 1.f), "Update ready!");
            ImGui::Dummy(ImVec2(0, 2.f));

            if (ImGui::Button("Install & Restart"))
                checker->ApplyAndRestart(appWindow);

            ImGui::SameLine();
            if (ImGui::Button("Later"))
                checker->Dismiss();
            break;
        }

        case UpdateChecker::State::Error:
        {
            std::string error = checker->GetLastError();
            ImGui::TextColored(ImVec4(0.88f, 0.47f, 0.19f, 1.f), "Update check failed");
            if (!error.empty())
            {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + panelW - 24.f);
                ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.6f), "%s", error.c_str());
                ImGui::PopTextWrapPos();
            }

            if (ImGui::Button("Retry"))
                checker->Check(checker->GetCurrentVersion());
            break;
        }

        default:
            break;
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    // Release Notes modal (rendered outside the overlay window)
    if (s_releaseNotesOpen)
    {
        ImGui::OpenPopup("Release Notes");
        s_releaseNotesOpen = false;
    }

    if (ImGui::BeginPopupModal("Release Notes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushTextWrapPos(400.f);
        ImGui::TextUnformatted(s_releaseNotesText.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 4.f));
        if (ImGui::Button("Close"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
