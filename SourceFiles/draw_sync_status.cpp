#include "pch.h"
#include "draw_sync_status.h"
#include "Net/SyncEngine.h"
#include "imgui.h"

static float s_toastAlpha = 0.f;
static float s_toastTimer = 0.f;
static std::string s_toastMessage;
static bool s_toastShown = false;          // true once we've shown the toast for this completion
static SyncEngine::State s_prevState = SyncEngine::State::Idle;
static constexpr float kToastDuration = 5.0f;
static constexpr float kToastFadeTime = 1.0f;

void draw_sync_status(SyncEngine* syncEngine)
{
    if (!syncEngine)
        return;

    auto state = syncEngine->GetState();
    float dt = ImGui::GetIO().DeltaTime;

    // Reset toast flag when sync restarts
    if (state != SyncEngine::State::Complete && s_prevState == SyncEngine::State::Complete)
        s_toastShown = false;
    s_prevState = state;

    // Handle toast fade for completed state
    if (state == SyncEngine::State::Complete)
    {
        if (!s_toastShown)
        {
            // Just completed — start the toast
            s_toastShown = true;
            int newCount = syncEngine->GetNewMatchCount();
            if (newCount > 0)
                s_toastMessage = std::to_string(newCount) + " new match(es) available";
            else
                s_toastMessage = "Match index up to date";
            s_toastTimer = kToastDuration;
            s_toastAlpha = 1.f;
        }
        else if (s_toastTimer > 0.f)
        {
            s_toastTimer -= dt;
            if (s_toastTimer < kToastFadeTime)
                s_toastAlpha = s_toastTimer / kToastFadeTime;
        }
        else
        {
            return; // Toast expired
        }
    }

    if (state == SyncEngine::State::Idle)
        return;

    // Position: bottom-right corner
    ImGuiIO& io = ImGui::GetIO();
    float panelW = 320.f;
    float panelH = 0.f; // auto-size
    ImVec2 pos(io.DisplaySize.x - panelW - 16.f, io.DisplaySize.y - 80.f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.f, 1.f));
    ImGui::SetNextWindowSize(ImVec2(panelW, 0.f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;

    float alpha = (state == SyncEngine::State::Complete) ? s_toastAlpha : 1.f;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 8.f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.12f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.63f, 0.13f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    if (ImGui::Begin("##SyncStatus", nullptr, flags))
    {
        switch (state)
        {
        case SyncEngine::State::FetchingIndex:
        {
            // Spinner animation
            float time = static_cast<float>(ImGui::GetTime());
            const char* spinner = "|/-\\";
            char spin = spinner[static_cast<int>(time * 8.f) % 4];
            ImGui::Text("%c  Checking for new matches...", spin);
            break;
        }

        case SyncEngine::State::Downloading:
        {
            int downloaded = syncEngine->GetDownloadedCount();
            int total = syncEngine->GetTotalToDownload();
            float progress = syncEngine->GetProgress();

            ImGui::Text("Downloading matches...");
            ImGui::Dummy(ImVec2(0, 2.f));

            // Progress bar
            ImGui::ProgressBar(progress, ImVec2(-1.f, 16.f));
            ImGui::Text("%d / %d matches", downloaded, total);
            break;
        }

        case SyncEngine::State::Complete:
        {
            ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 1.f), "%s", s_toastMessage.c_str());
            break;
        }

        case SyncEngine::State::Error:
        {
            std::string error = syncEngine->GetLastError();
            ImGui::TextColored(ImVec4(0.88f, 0.47f, 0.19f, 1.f), "Could not sync");
            if (!error.empty())
            {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + panelW - 24.f);
                ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.6f), "%s", error.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.5f), "Using cached data");
            break;
        }

        default:
            break;
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

void draw_play_download_progress(uint64_t bytesReceived, uint64_t bytesTotal,
                                  bool isError, const std::string& errorMsg)
{
    ImGuiIO& io = ImGui::GetIO();
    float panelW = 320.f;
    ImVec2 pos(io.DisplaySize.x - panelW - 16.f, io.DisplaySize.y - 140.f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.f, 1.f));
    ImGui::SetNextWindowSize(ImVec2(panelW, 0.f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 8.f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.12f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.63f, 0.86f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    if (ImGui::Begin("##PlayDownload", nullptr, flags))
    {
        if (isError)
        {
            ImGui::TextColored(ImVec4(0.88f, 0.47f, 0.19f, 1.f), "Download failed");
            if (!errorMsg.empty())
            {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + panelW - 24.f);
                ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.6f), "%s", errorMsg.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        else
        {
            float time = static_cast<float>(ImGui::GetTime());
            const char* spinner = "|/-\\";
            char spin = spinner[static_cast<int>(time * 8.f) % 4];
            ImGui::Text("%c  Downloading match...", spin);
            ImGui::Dummy(ImVec2(0, 2.f));

            float ratio = (bytesTotal > 0) ? static_cast<float>(bytesReceived) / bytesTotal : 0.f;
            ImGui::ProgressBar(ratio, ImVec2(-1.f, 16.f));

            double recvMB = bytesReceived / (1024.0 * 1024.0);
            double totalMB = bytesTotal / (1024.0 * 1024.0);
            ImGui::Text("%.1f / %.1f MB", recvMB, totalMB);
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}
