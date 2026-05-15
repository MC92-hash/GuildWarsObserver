#include "pch.h"
#include "draw_first_launch.h"
#include "Net/UpdateChecker.h"
#include "draw_gui_for_open_dat_file.h"
#include "GuiGlobalConstants.h"
#include "TextureCache.h"
#include "imgui.h"
#include <shellapi.h>
#include <filesystem>
#include <chrono>

namespace
{
    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_startTime;
    static bool  s_startTimeSet = false;
    static constexpr float kSimDurationSec = 2.5f;

    static Clock::time_point s_hitFullTime;
    static bool  s_hitFull = false;
    static constexpr float kFadeDurationSec  = 0.4f;
    static constexpr float kReadyDisplaySec  = 0.8f;
    static constexpr float kHoldAtFullSec    = kFadeDurationSec + kReadyDisplaySec;


    // --- helpers ---

    static std::string FormatWithCommas(int value)
    {
        if (value < 0) value = 0;
        std::string raw = std::to_string(value);
        std::string result;
        int count = 0;
        for (int i = static_cast<int>(raw.size()) - 1; i >= 0; --i)
        {
            if (count > 0 && count % 3 == 0)
                result.insert(result.begin(), ',');
            result.insert(result.begin(), raw[i]);
            ++count;
        }
        return result;
    }

    static void DrawTextWithShadow(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text)
    {
        ImU32 shadow = IM_COL32(0, 0, 0, 220);
        dl->AddText(ImVec2(pos.x - 1.f, pos.y + 1.f), shadow, text);
        dl->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), shadow, text);
        dl->AddText(ImVec2(pos.x,       pos.y + 2.f), shadow, text);
        dl->AddText(pos, col, text);
    }

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

}

static bool s_updatePopupShown = false;
static bool s_updateDismissed = false;
// s_autoInstallAfterDownload removed — now lives on UpdateChecker::SetAutoInstall()

static void DrawUpdateCard(ImVec2 display, const UpdateInfo& update)
{
    if (!update.available || !update.checker) return;
    if (s_updateDismissed) return;

    auto state = update.checker->GetState();

    // Nothing to show once back to idle (e.g. after cancel)
    if (state == UpdateChecker::State::Idle || state == UpdateChecker::State::Checking)
        return;

    float cardW = 440.f;
    // Position below the logo text, slightly right of center
    ImVec2 cardPos(display.x * 0.5f + 30.f, display.y * 0.62f);

    ImGui::SetNextWindowPos(cardPos, ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(cardW, 0));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.078f, 0.102f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32, 28));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    if (ImGui::Begin("##UpdateCard", nullptr, flags))
    {
        // Gold heading
        ImFont* font = ImGui::GetFont();
        float headingSz = 22.f;
        const char* heading = (state == UpdateChecker::State::ReadyToInstall) ? "Update Ready!" :
                              (state == UpdateChecker::State::Downloading)    ? "Downloading Update..." :
                              (state == UpdateChecker::State::Error)          ? "Update Failed" :
                                                                                "Update Available";
        ImVec2 hsz = font->CalcTextSizeA(headingSz, FLT_MAX, 0.f, heading);
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 cpos = ImGui::GetCursorPos();
        float hx = (cardW - hsz.x) * 0.5f;
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        ImU32 shadow = IM_COL32(0, 0, 0, 200);
        ImU32 headingCol = (state == UpdateChecker::State::Error) ? IM_COL32(224, 120, 48, 255) : IM_COL32(212, 160, 32, 255);
        wdl->AddText(font, headingSz, ImVec2(wpos.x + hx - 1.f, wpos.y + cpos.y + 1.f), shadow, heading);
        wdl->AddText(font, headingSz, ImVec2(wpos.x + hx + 1.f, wpos.y + cpos.y + 1.f), shadow, heading);
        wdl->AddText(font, headingSz, ImVec2(wpos.x + hx, wpos.y + cpos.y + 2.f), shadow, heading);
        wdl->AddText(font, headingSz, ImVec2(wpos.x + hx, wpos.y + cpos.y), headingCol, heading);
        ImGui::Dummy(ImVec2(0, hsz.y + 6.f));

        // State-specific content
        switch (state)
        {
        case UpdateChecker::State::UpdateAvailable:
        {
            const char* sub = "A new version of GW Observer is available.";
            ImVec2 ssz = ImGui::CalcTextSize(sub);
            ImGui::SetCursorPosX((cardW - ssz.x) * 0.5f);
            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.65f), "%s", sub);

            ImGui::Dummy(ImVec2(0, 12.f));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 12.f));

            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.50f), "Current version:");
            ImGui::SameLine(210.f);
            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.90f), "%s", update.currentVersion.c_str());
            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.50f), "Latest version:");
            ImGui::SameLine(210.f);
            ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1.f), "%s", update.latestVersion.c_str());

            ImGui::Dummy(ImVec2(0, 16.f));

            float btnW = 130.f;
            float dlBtnW = 150.f;
            float dismissW = 80.f;
            float gap = 8.f;
            float totalBtnW = btnW + gap + dlBtnW + gap + dismissW;
            ImGui::SetCursorPosX((cardW - totalBtnW) * 0.5f);

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.14f, 0.05f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.23f, 0.19f, 0.08f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.91f, 0.69f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 0.84f, 0.39f, 0.85f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);

            if (ImGui::Button("Release Page", ImVec2(btnW, 0)))
            {
                if (!update.releaseUrl.empty())
                {
                    std::wstring wUrl(update.releaseUrl.begin(), update.releaseUrl.end());
                    ShellExecuteW(nullptr, L"open", wUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            ImGui::SameLine(0, gap);
            if (ImGui::Button("Download & Install", ImVec2(dlBtnW, 0)))
            {
                update.checker->StartDownload();
                update.checker->SetAutoInstall(true);
            }

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);

            ImGui::SameLine(0, gap);
            if (ImGui::Button("Dismiss", ImVec2(dismissW, 0)))
                s_updateDismissed = true;
            break;
        }

        case UpdateChecker::State::Downloading:
        {
            ImGui::Dummy(ImVec2(0, 4.f));
            float progress = update.checker->GetDownloadProgress();
            ImGui::ProgressBar(progress, ImVec2(-1.f, 18.f));

            double recvMB = update.checker->GetDownloadedBytes() / (1024.0 * 1024.0);
            double totalMB = update.checker->GetTotalBytes() / (1024.0 * 1024.0);
            char buf[64];
            snprintf(buf, sizeof(buf), "%.1f / %.1f MB", recvMB, totalMB);
            ImVec2 tsz = ImGui::CalcTextSize(buf);
            ImGui::SetCursorPosX((cardW - tsz.x) * 0.5f);
            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.65f), "%s", buf);

            ImGui::Dummy(ImVec2(0, 8.f));
            float cancelW = 80.f;
            ImGui::SetCursorPosX((cardW - cancelW) * 0.5f);
            if (ImGui::Button("Cancel", ImVec2(cancelW, 0)))
                update.checker->Cancel();
            break;
        }

        case UpdateChecker::State::ReadyToInstall:
        {
            if (update.checker->ShouldAutoInstall())
            {
                update.checker->SetAutoInstall(false);
                update.checker->ApplyAndRestart(update.appWindow);
            }

            const char* sub = "The update has been downloaded and is ready to install.";
            ImVec2 ssz = ImGui::CalcTextSize(sub);
            ImGui::SetCursorPosX((cardW - ssz.x) * 0.5f);
            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.65f), "%s", sub);

            ImGui::Dummy(ImVec2(0, 16.f));

            float installW = 160.f;
            float laterW = 80.f;
            float gap = 8.f;
            float totalW = installW + gap + laterW;
            ImGui::SetCursorPosX((cardW - totalW) * 0.5f);

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.18f, 0.08f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.25f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.85f, 0.45f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.75f, 0.37f, 0.85f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);

            if (ImGui::Button("Install & Restart", ImVec2(installW, 0)))
                update.checker->ApplyAndRestart(update.appWindow);

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);

            ImGui::SameLine(0, gap);
            if (ImGui::Button("Later", ImVec2(laterW, 0)))
                s_updateDismissed = true;
            break;
        }

        case UpdateChecker::State::Error:
        {
            std::string err = update.checker->GetLastError();
            if (!err.empty())
            {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardW - 64.f);
                ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.6f), "%s", err.c_str());
                ImGui::PopTextWrapPos();
            }

            ImGui::Dummy(ImVec2(0, 12.f));
            float retryW = 80.f;
            float dismissW = 80.f;
            float gap = 8.f;
            float totalW = retryW + gap + dismissW;
            ImGui::SetCursorPosX((cardW - totalW) * 0.5f);

            if (ImGui::Button("Retry", ImVec2(retryW, 0)))
                update.checker->Check(update.currentVersion);
            ImGui::SameLine(0, gap);
            if (ImGui::Button("Dismiss", ImVec2(dismissW, 0)))
                s_updateDismissed = true;
            break;
        }

        default:
            break;
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

bool draw_first_launch(const LoadingProgress& progress, const UpdateInfo* update)
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 display = io.DisplaySize;

    if (!s_startTimeSet)
    {
        s_startTime = Clock::now();
        s_startTimeSet = true;
    }

    float elapsed = std::chrono::duration<float>(Clock::now() - s_startTime).count();
    float simFraction = std::min(elapsed / kSimDurationSec, 1.f);
    bool simulatedDone = (simFraction >= 1.f);

    float dat_load_fraction = 0.f;
    if (progress.dat_files_total > 0)
        dat_load_fraction = static_cast<float>(progress.dat_files_read) /
                            static_cast<float>(progress.dat_files_total);

    // Displayed progress
    float displayProgress;
    if (!simulatedDone)
        displayProgress = simFraction * 0.9f;
    else if (progress.dat_path_is_set)
        displayProgress = 0.9f + 0.1f * std::clamp(dat_load_fraction, 0.f, 1.f);
    else
        displayProgress = 0.9f;

    bool atFull = simulatedDone && progress.dat_path_is_set &&
                  dat_load_fraction >= 1.f && displayProgress >= 1.f;
    if (atFull && !s_hitFull)
    {
        s_hitFull = true;
        s_hitFullTime = Clock::now();
    }

    // Block loading screen exit while an update notification is showing
    bool updatePending = update && update->available && update->checker &&
        !s_updateDismissed &&
        update->checker->GetState() != UpdateChecker::State::Idle &&
        update->checker->GetState() != UpdateChecker::State::Checking;

    float fadeElapsed = 0.f;
    if (s_hitFull)
    {
        fadeElapsed = std::chrono::duration<float>(Clock::now() - s_hitFullTime).count();
        if (fadeElapsed >= kHoldAtFullSec && !updatePending)
            return true;
    }

    // Fade factor: 0→1 over kFadeDurationSec after hitting full
    float fadeOutAlpha = 1.f;  // status lines + bar
    float readyAlpha   = 0.f;  // "Ready" text
    if (s_hitFull)
    {
        float t = std::clamp(fadeElapsed / kFadeDurationSec, 0.f, 1.f);
        fadeOutAlpha = 1.f - t;
        readyAlpha   = t;
    }

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

    // Progress bar geometry (unchanged position)
    float barW = display.x * 0.4f;
    float barH = 6.f;
    float barX = (display.x - barW) * 0.5f;
    float barY = display.y * 0.85f;
    float barRight = barX + barW;

    // Progress bar (with fade)
    {
        ImU32 bgCol   = IM_COL32(40, 44, 52, static_cast<int>(255 * fadeOutAlpha));
        ImU32 fillCol = IM_COL32(74, 144, 216, static_cast<int>(230 * fadeOutAlpha));
        dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), bgCol, 3.f);
        dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW * displayProgress, barY + barH), fillCol, 3.f);
    }

    // --- Status text lines (right-aligned above bar, with shadow) ---
    if (fadeOutAlpha > 0.01f && !s_hitFull)
    {
        ImU32 labelCol   = IM_COL32(255, 255, 255, static_cast<int>(210 * fadeOutAlpha));
        ImU32 counterCol = IM_COL32(255, 255, 255, static_cast<int>(250 * fadeOutAlpha));

        float lineH = ImGui::GetFontSize() + 2.f;
        float textY = barY - 8.f;
        bool showedAnyLine = false;

        // Dat file line — show whenever dat loading is in progress
        if (progress.dat_path_is_set && progress.dat_files_total > 0 && dat_load_fraction < 1.f)
        {
            std::string label = "Loading .dat file  ";
            std::string counter = "[" + FormatWithCommas(progress.dat_files_read) +
                                  " / " + FormatWithCommas(progress.dat_files_total) + "]";
            std::string full = label + counter;
            ImVec2 fullSz = ImGui::CalcTextSize(full.c_str());
            ImVec2 labelSz = ImGui::CalcTextSize(label.c_str());

            textY -= lineH;
            float lineX = barRight - fullSz.x;
            DrawTextWithShadow(dl, ImVec2(lineX, textY), labelCol, label.c_str());
            DrawTextWithShadow(dl, ImVec2(lineX + labelSz.x, textY), counterCol, counter.c_str());
            showedAnyLine = true;
        }

        // Match metadata line — show once loaded (count known)
        if (progress.match_count >= 0)
        {
            std::string label = "Match metadata loaded  ";
            std::string counter = "[" + FormatWithCommas(progress.match_count) + " matches]";
            std::string full = label + counter;
            ImVec2 fullSz = ImGui::CalcTextSize(full.c_str());
            ImVec2 labelSz = ImGui::CalcTextSize(label.c_str());

            textY -= lineH;
            float lineX = barRight - fullSz.x;
            DrawTextWithShadow(dl, ImVec2(lineX, textY), labelCol, label.c_str());
            DrawTextWithShadow(dl, ImVec2(lineX + labelSz.x, textY), counterCol, counter.c_str());
            showedAnyLine = true;
        }

        // Fallback — only when no granular info is available yet
        if (!showedAnyLine)
        {
            const char* initText = "Initializing...";
            ImVec2 sz = ImGui::CalcTextSize(initText);
            textY -= lineH;
            DrawTextWithShadow(dl, ImVec2(barRight - sz.x, textY), labelCol, initText);
        }
    }

    // "Ready" text (centered, fades in)
    if (readyAlpha > 0.01f)
    {
        const char* readyText = "Ready";
        ImVec2 sz = ImGui::CalcTextSize(readyText);
        float rx = (display.x - sz.x) * 0.5f;
        float ry = barY - 30.f;
        ImU32 readyCol = IM_COL32(255, 255, 255, static_cast<int>(230 * readyAlpha));
        DrawTextWithShadow(dl, ImVec2(rx, ry), readyCol, readyText);
    }

    ImGui::End();
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(3);

    // Draw update card on top of loading screen
    if (update)
        DrawUpdateCard(display, *update);

    return false;
}

void draw_first_launch_reset_state()
{
    s_startTimeSet = false;
    s_hitFull = false;
}
