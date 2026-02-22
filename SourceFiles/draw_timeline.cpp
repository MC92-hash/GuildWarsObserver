#include "pch.h"
#include "draw_timeline.h"
#include "MatchReplay.h"
#include <string>
#include <format>

static const char* speed_labels[] = { "0.25x", "0.5x", "1x", "2x", "4x", "8x" };
static const float speed_values[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
static constexpr int NUM_SPEEDS = 6;

static int FindClosestSpeedIndex(float speed)
{
    int best = 2; // default 1x
    float bestDist = 999.f;
    for (int i = 0; i < NUM_SPEEDS; ++i)
    {
        float d = fabsf(speed_values[i] - speed);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

static std::string FormatTime(float seconds)
{
    int totalSec = static_cast<int>(seconds);
    int m = totalSec / 60;
    int s = totalSec % 60;
    return std::format("{:02d}:{:02d}", m, s);
}

bool draw_timeline_bar(MatchReplay& replay)
{
    if (!replay.IsLoaded()) return false;

    ImGuiIO& io = ImGui::GetIO();
    const float barHeight = 48.0f;

    // Position at the bottom of the screen
    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - barHeight));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, barHeight));

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.10f, 0.92f));

    bool isActive = false;

    if (ImGui::Begin("##TimelineBar", nullptr, flags))
    {
        isActive = true;
        float duration = replay.GetDuration();
        float currentTime = replay.GetCurrentTime();
        PlaybackState state = replay.GetState();

        // --- Row: controls + slider + time ---
        // Step backward
        if (ImGui::Button("<<"))
            replay.StepBackward(5.0f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step back 5s");

        ImGui::SameLine();

        if (ImGui::Button(" < "))
            replay.StepBackward(1.0f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step back 1s");

        ImGui::SameLine();

        // Play / Pause
        const char* playLabel = (state == PlaybackState::Playing) ? " || " : " |> ";
        if (ImGui::Button(playLabel))
            replay.TogglePlayPause();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(state == PlaybackState::Playing ? "Pause" : "Play");

        ImGui::SameLine();

        // Stop
        if (ImGui::Button(" [] "))
            replay.Stop();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop (reset to start)");

        ImGui::SameLine();

        if (ImGui::Button(" > "))
            replay.StepForward(1.0f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step forward 1s");

        ImGui::SameLine();

        if (ImGui::Button(">>"))
            replay.StepForward(5.0f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step forward 5s");

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Speed selector
        int speedIdx = FindClosestSpeedIndex(replay.GetSpeed());
        ImGui::SetNextItemWidth(60.0f);
        if (ImGui::Combo("##Speed", &speedIdx, speed_labels, NUM_SPEEDS))
            replay.SetSpeed(speed_values[speedIdx]);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback speed");

        ImGui::SameLine();

        // Loop toggle
        bool looping = replay.IsLooping();
        if (ImGui::Checkbox("Loop", &looping))
            replay.SetLooping(looping);

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Time display
        std::string timeStr = FormatTime(currentTime) + " / " + FormatTime(duration);
        ImGui::Text("%s", timeStr.c_str());

        ImGui::SameLine();

        // Timeline scrubber (fills remaining width)
        float availWidth = ImGui::GetContentRegionAvail().x;
        if (availWidth > 40.0f)
        {
            ImGui::SetNextItemWidth(availWidth);
            if (ImGui::SliderFloat("##Timeline", &currentTime, 0.0f, duration, ""))
                replay.SetTime(currentTime);
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    return isActive;
}
