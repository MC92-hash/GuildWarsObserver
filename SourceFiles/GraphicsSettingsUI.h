#pragma once

// The Performance controls - frame limit and graphics quality - drawn the same way in the main
// Settings window and in a replay window's Graphics preferences. Saves on every change.

#include "GuiGlobalConstants.h"
#include "imgui.h"

inline void DrawGraphicsSettingsControls()
{
    const ImVec4 note(1.f, 1.f, 1.f, 0.5f);
    bool changed = false;

    // ---- frame limit, independent of the preset ----
    {
        static const int kLimits[] = {30, 60, 120, 144, 0};
        static const char* kLabels[] = {"30 FPS", "60 FPS", "120 FPS", "144 FPS", "No limit"};
        int current = 4;
        for (int i = 0; i < 5; ++i)
            if (GuiGlobalConstants::frame_limit_fps == kLimits[i]) current = i;
        ImGui::SetNextItemWidth(160.f);
        if (ImGui::Combo("Frame limit", &current, kLabels, 5))
        {
            GuiGlobalConstants::frame_limit_fps = kLimits[current];
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The most frames drawn per second. Lower leaves more of the PC to\n"
                              "everything else - OBS when streaming, for example.");
    }

    // ---- the preset ----
    {
        static const char* kPresets[] = {"High", "Balanced", "Low", "Custom"};
        int preset = GuiGlobalConstants::graphics_quality;
        ImGui::SetNextItemWidth(160.f);
        if (ImGui::Combo("Graphics quality", &preset, kPresets, 4))
        {
            GuiGlobalConstants::ApplyGraphicsPreset(preset);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("High: the full picture.\n"
                              "Balanced: slightly lower resolution, lighter shadows.\n"
                              "Low: for weak or older PCs and laptops. Characters and skill effects\n"
                              "stay; the 3D view is drawn at a lower resolution, textures are\n"
                              "smaller and the map's shadows are off.");
    }

    // ---- the details, any change makes the preset Custom ----
    if (ImGui::TreeNode("Details##graphicsDetails"))
    {
        bool custom = false;

        ImGui::SetNextItemWidth(160.f);
        if (ImGui::SliderInt("3D resolution", &GuiGlobalConstants::render_scale_percent, 50, 100, "%d%%"))
            custom = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The 3D view's resolution. The interface always stays sharp.");

        static const char* kTex[] = {"Full", "Half", "Quarter"};
        ImGui::SetNextItemWidth(160.f);
        if (ImGui::Combo("Texture resolution", &GuiGlobalConstants::texture_quality, kTex, 3))
            custom = true;
        ImGui::SameLine();
        ImGui::TextColored(note, "(next match opened)");

        static const int kAniso[] = {16, 4, 1};
        static const char* kAnisoLabels[] = {"16x", "4x", "Off"};
        int aniso = 0;
        for (int i = 0; i < 3; ++i)
            if (GuiGlobalConstants::texture_anisotropy == kAniso[i]) aniso = i;
        ImGui::SetNextItemWidth(160.f);
        if (ImGui::Combo("Texture filtering", &aniso, kAnisoLabels, 3))
        {
            GuiGlobalConstants::texture_anisotropy = kAniso[aniso];
            custom = true;
        }
        ImGui::SameLine();
        ImGui::TextColored(note, "(next match opened)");

        if (ImGui::Checkbox("Map shadows", &GuiGlobalConstants::map_shadows))
            custom = true;
        if (ImGui::Checkbox("Character silhouette shadows", &GuiGlobalConstants::character_silhouette_shadows))
            custom = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off: a soft round shadow under each character instead.");
        bool mediumDetail = GuiGlobalConstants::map_detail > 0;
        if (ImGui::Checkbox("Lighter map models", &mediumDetail))
        {
            GuiGlobalConstants::map_detail = mediumDetail ? 1 : 0;
            custom = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The map's props use their own lower-detail models.");
        if (ImGui::Checkbox("Minimap at a reduced rate", &GuiGlobalConstants::minimap_reduced_rate))
            custom = true;

        if (custom)
        {
            GuiGlobalConstants::graphics_quality = GuiGlobalConstants::kGraphicsCustom;
            changed = true;
        }
        ImGui::TreePop();
    }

    if (changed)
        GuiGlobalConstants::SaveSettings();
}
