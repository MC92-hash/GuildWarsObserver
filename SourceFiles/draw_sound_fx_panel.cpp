#include "pch.h"
#include "draw_sound_fx_panel.h"
#include "SpatialAudioEngine.h"
#include "GuiGlobalConstants.h"

namespace
{
    constexpr ImVec4 kGold        = ImVec4(0.83f, 0.63f, 0.13f, 1.f);
    constexpr ImVec4 kTextPrimary = ImVec4(1.f, 1.f, 1.f, 0.85f);
    constexpr ImVec4 kTextDim     = ImVec4(1.f, 1.f, 1.f, 0.55f);

    // Matches DrawPrefsSectionHeader / PushPrefsFrameStyle in ReplayWindow_Preferences.cpp so the
    // Sound FX panel reads as part of the same design language as Settings > Preferences.
    void SectionHeader(const char* title)
    {
        ImGui::TextColored(kGold, "%s", title);
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 6.f));
    }

    void PushFrameStyle()
    {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.4f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.15f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    }

    void PopFrameStyle()
    {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    // The tuned levels span two orders of magnitude (Background 0.01 against Skills 0.95), so a
    // plain 0-100% slider crams most of them into the first few pixels where they cannot be
    // adjusted at all. Instead each slider is bent so its own default sits at the halfway mark:
    // the lower half covers 0 -> default with fine resolution, the upper half covers
    // default -> full. Nothing is clipped off - 100% still reaches unity - the travel is just
    // redistributed to where the setting actually lives.
    // `reference` is the level pinned to the 50% mark. It is a fixed calibration point, not the
    // startup value - keeping them separate is what lets a setting default to something other
    // than dead centre.
    //
    // Below the midpoint the mapping is linear down to `minVal`, so a fade to silence stays
    // predictable. Above it the mapping is geometric (equal ratio per unit of travel, i.e. equal
    // dB), because linear-to-full is unusable when the reference is small: with Background at
    // 0.01, a linear upper half turns a 2% nudge into a 5x jump, where the geometric one makes it
    // the ~1.6 dB it looks like.
    float PositionToValue(float pos, float reference, float minVal, float maxVal)
    {
        pos = std::clamp(pos, 0.f, 1.f);
        if (pos <= 0.5f)
            return minVal + (reference - minVal) * (pos / 0.5f);
        if (reference <= 0.0001f)   // no ratio to work with; fall back to linear
            return minVal + (maxVal - minVal) * pos;
        return reference * powf(maxVal / reference, (pos - 0.5f) / 0.5f);
    }

    float ValueToPosition(float value, float reference, float minVal, float maxVal)
    {
        if (value <= reference)
            return (reference > minVal) ? 0.5f * ((value - minVal) / (reference - minVal)) : 0.f;
        if (reference <= 0.0001f || maxVal <= reference)
            return 1.0f;
        return std::clamp(0.5f + 0.5f * (logf(value / reference) / logf(maxVal / reference)),
                          0.f, 1.f);
    }

    // Plain min..max slider in the same visual language as VolumeSlider, for values that are not
    // volumes and so must not be bent around a reference point.
    bool CurveSlider(const char* id, const char* label, float* value,
                     float vmin, float vmax, const char* fmt, float displayScale)
    {
        ImGui::TextColored(kTextPrimary, "%s", label);
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(std::max(120.f, avail - 62.f));

        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 10.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.f);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, kGold);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(1.f, 0.84f, 0.39f, 1.f));

        std::string sliderId = "##";
        sliderId += id;
        bool changed = ImGui::SliderFloat(sliderId.c_str(), value, vmin, vmax, "");

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        ImGui::SameLine(0.f, 10.f);
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.75f), fmt, *value * displayScale);
        return changed;
    }

    // Plots the falloff actually in force, sampled from the engine so the drawing cannot drift
    // from what is audible. X is distance in GW units (using the widest category radius, spell
    // effects, as the reference); Y is gain. GW's own distance landmarks are marked so the shape
    // can be read against ranges that mean something in game.
    void DrawFalloffPlot(SpatialAudioEngine* engine, const AudioConfig& cfg)
    {
        constexpr float kSkillEffectRadius = 2512.f;   // widest per-category radius
        const float radius = kSkillEffectRadius * cfg.hearing_distance_scale;

        const float w = std::max(180.f, ImGui::GetContentRegionAvail().x - 8.f);
        constexpr float h = 104.f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##falloffPlot", ImVec2(w, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const ImVec2 tl = origin;
        const ImVec2 br = ImVec2(origin.x + w, origin.y + h);
        dl->AddRectFilled(tl, br, IM_COL32(0, 0, 0, 110), 4.f);
        dl->AddRect(tl, br, IM_COL32(255, 255, 255, 38), 4.f);

        // Horizontal gridlines at -6 / -12 / -20 dB: even spacing here would mean the curve is
        // logarithmic, which is the question being asked.
        const struct { float gain; const char* label; } kRows[] = {
            { 0.501f, "-6" }, { 0.251f, "-12" }, { 0.100f, "-20" },
        };
        for (const auto& row : kRows) {
            const float y = br.y - row.gain * h;
            dl->AddLine(ImVec2(tl.x, y), ImVec2(br.x, y), IM_COL32(255, 255, 255, 24));
            dl->AddText(ImVec2(tl.x + 3.f, y - 12.f), IM_COL32(255, 255, 255, 70), row.label);
        }

        // GW distance landmarks, so the curve is read against ranges with in-game meaning.
        const struct { float units; const char* label; } kMarks[] = {
            { 240.f, "Nearby" }, { 1000.f, "Earshot" }, { 1248.f, "Cast" }, { 2512.f, "Passive" },
        };
        for (const auto& mark : kMarks) {
            if (mark.units > radius) continue;
            const float x = tl.x + (mark.units / radius) * w;
            dl->AddLine(ImVec2(x, tl.y), ImVec2(x, br.y), IM_COL32(255, 255, 255, 30));
            dl->AddText(ImVec2(x + 3.f, tl.y + 3.f), IM_COL32(255, 255, 255, 85), mark.label);
        }

        // The curve itself.
        constexpr int kSamples = 72;
        ImVec2 pts[kSamples];
        for (int i = 0; i < kSamples; ++i) {
            const float t = static_cast<float>(i) / (kSamples - 1);
            const float g = engine->DistanceGain(t);
            pts[i] = ImVec2(tl.x + t * w, br.y - std::clamp(g, 0.f, 1.f) * h);
        }
        dl->AddPolyline(pts, kSamples, IM_COL32(212, 161, 33, 255), 0, 2.0f);

        ImGui::TextColored(kTextDim, "0 to %.0f units   |   grid: -6 / -12 / -20 dB", radius);

        // Concrete readout: what a spell effect actually costs at each landmark.
        std::string line;
        char cell[64];
        for (const auto& mark : kMarks) {
            if (mark.units > radius) continue;
            const float g = engine->DistanceGain(mark.units / radius);
            const float db = (g > 0.0001f) ? 20.f * log10f(g) : -99.f;
            snprintf(cell, sizeof(cell), "%s %.0fdB   ", mark.label, db);
            line += cell;
        }
        if (!line.empty())
            ImGui::TextColored(kTextDim, "%s", line.c_str());
    }

    // Thin slider matching the sync-download progress bar height (14px) with the value shown as
    // a percentage readout to the right, same layout as the Interface Preferences sliders.
    // `reference` is the level that should land at 50%; the readout shows slider travel, and the
    // tooltip shows the gain actually applied.
    bool VolumeSlider(const char* id, const char* label, float* value, float reference,
                      float minVal = 0.f, float maxVal = 1.f, const char* unit = nullptr)
    {
        ImGui::TextColored(kTextPrimary, "%s", label);
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(std::max(120.f, avail - 56.f));

        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 10.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.f);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, kGold);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(1.f, 0.84f, 0.39f, 1.f));

        std::string sliderId = "##";
        sliderId += id;
        float pos = ValueToPosition(*value, reference, minVal, maxVal);
        bool changed = ImGui::SliderFloat(sliderId.c_str(), &pos, 0.0f, 1.0f, "");
        if (changed)
            *value = PositionToValue(pos, reference, minVal, maxVal);

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);

        ImGui::SameLine(0.f, 10.f);
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.75f), "%3.0f%%", pos * 100.f);
        if (ImGui::IsItemHovered()) {
            if (unit)
                ImGui::SetTooltip("Slider travel. Value: %.2f%s  (50%% = %.2f%s)",
                                  *value, unit, reference, unit);
            else
                ImGui::SetTooltip("Slider travel. Applied gain: %.3f  (50%% = %.3f)",
                                  *value, reference);
        }
        return changed;
    }
}

void DrawSoundFxPanel(SpatialAudioEngine* engine, bool& show)
{
    if (!show) return;

    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.078f, 0.102f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    if (ImGui::Begin("Sound FX", &show, ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize)) {
        GuiGlobalConstants::ClampWindowToScreen();

        if (!engine || !engine->IsInitialized()) {
            ImGui::TextWrapped("Audio engine not available for this replay.");
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            return;
        }

        auto& cfg = engine->GetConfig();

        // Calibration midpoints: the level each slider pins to 50%. Deliberately separate from
        // AudioConfig's defaults so a setting can ship somewhere other than dead centre (see
        // Background). These are the tuned reference mix and should not drift with the defaults.
        struct { float master, background, skills, attacks, ambient, ui, music, range, vertical; }
        const kRef{ 0.70f, 0.01f, 0.95f, 0.02f, 0.30f, 0.02f, 0.02f, 1.20f, 0.05f };

        SectionHeader("MASTER VOLUME");
        PushFrameStyle();
        VolumeSlider("sfxMaster", "Master", &cfg.master_volume, kRef.master);
        PopFrameStyle();

        ImGui::Dummy(ImVec2(0, 10.f));
        SectionHeader("SOUND CATEGORY VOLUME");
        PushFrameStyle();
        VolumeSlider("sfxBackground", "Background", &cfg.background_volume, kRef.background);
        ImGui::Dummy(ImVec2(0, 4.f));
        VolumeSlider("sfxSkills", "Skills", &cfg.skills_volume, kRef.skills);
        ImGui::Dummy(ImVec2(0, 4.f));
        VolumeSlider("sfxAttacks", "Attacks", &cfg.attacks_volume, kRef.attacks);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Weapon hits. These fire far more often than skills, so they\n"
                              "sit lower by default to keep casts readable.");
        ImGui::Dummy(ImVec2(0, 4.f));
        VolumeSlider("sfxAmbient", "Ambient", &cfg.ambient_volume, kRef.ambient);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Footsteps and uncorrelated effect sounds.");
        ImGui::Dummy(ImVec2(0, 4.f));
        VolumeSlider("sfxUi", "UI", &cfg.ui_volume, kRef.ui);
        ImGui::Dummy(ImVec2(0, 4.f));
        VolumeSlider("sfxMusic", "Music", &cfg.music_volume, kRef.music);
        // Dialog has no slider: it ships muted and there is nothing in a GvG replay that uses it.
        PopFrameStyle();

        ImGui::Dummy(ImVec2(0, 10.f));

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.15f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.18f, 0.22f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.25f, 0.30f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.63f, 0.13f, 0.8f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        if (ImGui::Button("Reset to Defaults", ImVec2(-1, 30.f))) {
            // Everything: levels, hearing distance, falloff shape and the stereo swap.
            cfg = AudioConfig{};
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);

        // Everything past this point is setup rather than mixing - collapsed by default so the
        // panel opens as just the faders.
        ImGui::Dummy(ImVec2(0, 10.f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1.f, 1.f, 1.f, 0.05f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.f, 1.f, 1.f, 0.09f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1.f, 1.f, 1.f, 0.12f));
        ImGui::PushStyleColor(ImGuiCol_Text, kGold);
        const bool advancedOpen = ImGui::CollapsingHeader("Advanced");
        ImGui::PopStyleColor(4);

        if (!advancedOpen) {
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            return;
        }

        ImGui::Dummy(ImVec2(0, 8.f));
        SectionHeader("HEARING DISTANCE");
        ImGui::TextColored(kTextDim, "Scales how far each kind of sound carries.");
        ImGui::Dummy(ImVec2(0, 6.f));

        PushFrameStyle();
        {
            // Both pinned so their default sits at 50%, same treatment as the volume faders:
            // 1.20x and 0.05x were at the extreme ends of their old linear ranges, where there
            // was no travel left to adjust them with.
            VolumeSlider("sfxHearing", "Range", &cfg.hearing_distance_scale,
                         kRef.range, 0.10f, 3.0f, "x");
            // Spell payoffs use the widest radius, so it is the most useful one to show in
            // GW's own units alongside the multiplier.
            ImGui::TextColored(kTextDim, "%.2fx  -  spell effects fade out by %.0f units (Passive = 2512)",
                               cfg.hearing_distance_scale,
                               2512.f * cfg.hearing_distance_scale);

            ImGui::Dummy(ImVec2(0, 8.f));
            VolumeSlider("sfxVertical", "Vertical", &cfg.vertical_distance_scale,
                         kRef.vertical, 0.0f, 1.0f, "x");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How much camera height counts toward distance.\n"
                                  "1.00 = true 3D. Lower values stop an overhead camera\n"
                                  "from pushing everything to the far end of the falloff.");
            ImGui::TextColored(kTextDim, "%.2fx  -  camera height counts as %.0f%% of its real distance",
                               cfg.vertical_distance_scale,
                               cfg.vertical_distance_scale * 100.f);
        }
        PopFrameStyle();

        ImGui::Dummy(ImVec2(0, 14.f));
        SectionHeader("FALLOFF CURVE");
        ImGui::TextColored(kTextDim, "Inverse distance law - logarithmic in dB.");
        ImGui::Dummy(ImVec2(0, 6.f));

        PushFrameStyle();
        {
            DrawFalloffPlot(engine, cfg);

            ImGui::Dummy(ImVec2(0, 8.f));
            CurveSlider("sfxNearField", "Full volume within", &cfg.near_field_fraction,
                        0.005f, 0.40f, "%.0f%%", 100.f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Share of the radius held at full volume before any falloff.\n"
                                  "Raise this if close range feels abrupt.");
            ImGui::Dummy(ImVec2(0, 6.f));
            CurveSlider("sfxRolloff", "Rolloff", &cfg.distance_rolloff,
                        0.05f, 3.0f, "%.2f", 1.f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How fast it falls past the near field.\n"
                                  "1.00 = the physical law (-6 dB per doubling of distance).\n"
                                  "Lower carries further, higher drops away faster.");
        }
        PopFrameStyle();

        ImGui::Dummy(ImVec2(0, 14.f));
        SectionHeader("STEREO CHECK");
        ImGui::TextColored(kTextDim, "Plays a sound at that side of the camera.");
        ImGui::Dummy(ImVec2(0, 6.f));

        PushFrameStyle();
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            const float btnW  = (avail - 3 * 6.f) / 4.f;

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.15f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.18f, 0.22f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.25f, 0.30f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.63f, 0.13f, 0.8f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);

            if (ImGui::Button("Left", ImVec2(btnW, 28.f)))
                engine->PlayDirectionalTest(SpatialAudioEngine::TestDirection::Left);
            ImGui::SameLine(0.f, 6.f);
            if (ImGui::Button("Right", ImVec2(btnW, 28.f)))
                engine->PlayDirectionalTest(SpatialAudioEngine::TestDirection::Right);
            ImGui::SameLine(0.f, 6.f);
            if (ImGui::Button("Front", ImVec2(btnW, 28.f)))
                engine->PlayDirectionalTest(SpatialAudioEngine::TestDirection::Front);
            ImGui::SameLine(0.f, 6.f);
            if (ImGui::Button("Back", ImVec2(btnW, 28.f)))
                engine->PlayDirectionalTest(SpatialAudioEngine::TestDirection::Back);

            ImGui::PopStyleVar(1);
            ImGui::PopStyleColor(4);

            ImGui::Dummy(ImVec2(0, 6.f));
            ImGui::Checkbox("Swap left / right", &cfg.swap_stereo);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Mirrors the stereo image if the sides still come out inverted.");
        }
        PopFrameStyle();

        ImGui::Dummy(ImVec2(0, 6.f));
        ImGui::Separator();
        const auto stats = engine->GetDebugStats();
        ImGui::TextColored(kTextDim, "Voices: %d   |   at cap: %d   |   out of range: %d",
                           stats.voicesActive, stats.droppedAtCap, stats.culledByDistance);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}
