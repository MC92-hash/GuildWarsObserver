#include "pch.h"
#include "HeatmapMenu.h"
#include <cstdio>
#include <algorithm>
#include <string>

// ── Style constants (dark/gold matching existing panels) ───────────────
static const ImVec4 kGoldText    = ImVec4(0.78f, 0.72f, 0.55f, 1.f);
static const ImVec4 kGoldCheckMk = ImVec4(1.f, 0.84f, 0.39f, 1.f);
static const ImVec4 kGoldSlider  = ImVec4(0.78f, 0.65f, 0.29f, 0.85f);
static const ImVec4 kDimText     = ImVec4(0.45f, 0.45f, 0.40f, 1.f);
static const ImVec4 kBlueText    = ImVec4(0.4f, 0.6f, 1.0f, 1.0f);
static const ImVec4 kRedText     = ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
static const ImVec4 kBlueBtnBg   = ImVec4(0.12f, 0.18f, 0.35f, 1.0f);
static const ImVec4 kRedBtnBg    = ImVec4(0.35f, 0.12f, 0.12f, 1.0f);
static const ImVec4 kDomBtnBg    = ImVec4(0.25f, 0.18f, 0.10f, 1.0f);

static int ParseTrailingNumber(const std::string& name)
{
    auto rp = name.rfind('(');
    if (rp == std::string::npos) return 9999;
    int val = 0;
    for (size_t i = rp + 1; i < name.size() && name[i] >= '0' && name[i] <= '9'; ++i)
        val = val * 10 + (name[i] - '0');
    return val > 0 ? val : 9999;
}

static void SectionHeader(const char* label)
{
    ImGui::PushStyleColor(ImGuiCol_Text, kGoldText);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
}

const char* GetPaletteName(HeatmapPalette p)
{
    switch (p) {
        case HeatmapPalette::THERMAL:   return "Thermal";
        case HeatmapPalette::INFERNO:   return "Inferno";
        case HeatmapPalette::VIRIDIS:   return "Viridis";
        case HeatmapPalette::TEAM_BLUE: return "Team Blue";
        case HeatmapPalette::TEAM_RED:  return "Team Red";
        case HeatmapPalette::DOMINANCE: return "Dominance";
        case HeatmapPalette::LAVA:      return "Lava";
        case HeatmapPalette::SUNSET:    return "Sunset";
        case HeatmapPalette::AMBER:     return "Amber";
        default: return "Unknown";
    }
}

// ── Floating panel ─────────────────────────────────────────────────────

bool DrawHeatmapPanel(HeatmapSettings& settings,
                      const std::vector<AgentMenuEntry>& agents,
                      const LutSrvGetter& getLutSRV)
{
    if (!settings.show) return false;

    bool changed = false;

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(400, 540), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(300, 200), displaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,      ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.08f, 0.08f, 0.06f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.12f, 0.11f, 0.07f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,      kGoldCheckMk);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,     kGoldSlider);
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    if (!ImGui::Begin("Heatmap", &settings.show, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(6);
        return false;
    }

    // Clamp window inside viewport after resize / small screen
    {
        ImVec2 pos  = ImGui::GetWindowPos();
        ImVec2 size = ImGui::GetWindowSize();
        float  clampedX = std::clamp(pos.x, 0.f, std::max(0.f, displaySize.x - size.x));
        float  clampedY = std::clamp(pos.y, 0.f, std::max(0.f, displaySize.y - size.y));
        if (clampedX != pos.x || clampedY != pos.y)
            ImGui::SetWindowPos(ImVec2(clampedX, clampedY));
    }

    // ── Render toggle ──────────────────────────────────────────────────
    if (ImGui::Checkbox("Enable Heatmap Overlay", &settings.renderEnabled))
        changed = true;

    ImGui::Spacing();

    // ── Add Layer section ──────────────────────────────────────────────

    SectionHeader("Add Layer");

    float fullW = ImGui::GetContentRegionAvail().x;
    float halfW = (fullW - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    // Team buttons row
    ImGui::PushStyleColor(ImGuiCol_Button, kRedBtnBg);
    if (ImGui::Button("+ Team Red", ImVec2(halfW, 0)))
    {
        HeatmapLayerDef def;
        def.subjectType = HeatmapSubjectType::TEAM;
        def.subjectId   = 1;
        def.subjectName = "Team Red";
        def.palette     = HeatmapPalette::TEAM_RED;
        settings.layers.push_back(std::move(def));
        changed = true;
    }
    ImGui::PopStyleColor();

    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, kBlueBtnBg);
    if (ImGui::Button("+ Team Blue", ImVec2(halfW, 0)))
    {
        HeatmapLayerDef def;
        def.subjectType = HeatmapSubjectType::TEAM;
        def.subjectId   = 2;
        def.subjectName = "Team Blue";
        def.palette     = HeatmapPalette::TEAM_BLUE;
        settings.layers.push_back(std::move(def));
        changed = true;
    }
    ImGui::PopStyleColor();

    // Dominance button (full width)
    ImGui::PushStyleColor(ImGuiCol_Button, kDomBtnBg);
    if (ImGui::Button("+ Dominance (Blue vs Red)", ImVec2(fullW, 0)))
    {
        HeatmapLayerDef def;
        def.subjectType = HeatmapSubjectType::DOMINANCE;
        def.subjectId   = 0;
        def.subjectName = "Dominance";
        def.palette     = HeatmapPalette::DOMINANCE;
        settings.layers.push_back(std::move(def));
        changed = true;
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Dominance shows which team controls each area.\n"
            "Blue = Blue team dominant, Red = Red team dominant,\n"
            "Warm neutral = contested (both teams present equally).\n"
            "Empty areas remain transparent.");

    ImGui::Spacing();

    // Two-column player lists using side-by-side child regions
    if (!agents.empty())
    {
        std::vector<const AgentMenuEntry*> red, blue;
        for (auto& a : agents)
        {
            if (a.teamId == 1) red.push_back(&a);
            else if (a.teamId == 2) blue.push_back(&a);
        }
        auto byNumber = [](const AgentMenuEntry* a, const AgentMenuEntry* b)
        { return ParseTrailingNumber(a->name) < ParseTrailingNumber(b->name); };
        std::sort(red.begin(), red.end(), byNumber);
        std::sort(blue.begin(), blue.end(), byNumber);

        size_t maxRows = std::max(red.size(), blue.size());
        float iconSz = ImGui::GetTextLineHeight();
        float rowH = ImGui::GetTextLineHeightWithSpacing() + 2.f;
        float listH = std::min((float)maxRows + 1.2f, 9.f) * rowH + 4.f;

        float colW = halfW - 2.f;

        // Red column
        ImGui::BeginChild("##red_col", ImVec2(colW, listH), true);
        ImGui::PushStyleColor(ImGuiCol_Text, kRedText);
        ImGui::TextUnformatted("Red Team");
        ImGui::PopStyleColor();
        ImGui::Separator();
        for (auto* a : red)
        {
            ImGui::PushID(a->agentId);
            if (ImGui::SmallButton("+"))
            {
                HeatmapLayerDef def;
                def.subjectType = HeatmapSubjectType::PLAYER;
                def.subjectId   = a->agentId;
                def.subjectName = a->name;
                def.palette     = HeatmapPalette::THERMAL;
                settings.layers.push_back(std::move(def));
                changed = true;
            }
            ImGui::SameLine();
            if (a->profIcon)
            {
                ImGui::Image(a->profIcon, ImVec2(iconSz, iconSz));
                ImGui::SameLine();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, kRedText);
            ImGui::TextUnformatted(a->name.c_str());
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Blue column
        ImGui::BeginChild("##blue_col", ImVec2(colW, listH), true);
        ImGui::PushStyleColor(ImGuiCol_Text, kBlueText);
        ImGui::TextUnformatted("Blue Team");
        ImGui::PopStyleColor();
        ImGui::Separator();
        for (auto* a : blue)
        {
            ImGui::PushID(a->agentId + 10000);
            if (ImGui::SmallButton("+"))
            {
                HeatmapLayerDef def;
                def.subjectType = HeatmapSubjectType::PLAYER;
                def.subjectId   = a->agentId;
                def.subjectName = a->name;
                def.palette     = HeatmapPalette::THERMAL;
                settings.layers.push_back(std::move(def));
                changed = true;
            }
            ImGui::SameLine();
            if (a->profIcon)
            {
                ImGui::Image(a->profIcon, ImVec2(iconSz, iconSz));
                ImGui::SameLine();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, kBlueText);
            ImGui::TextUnformatted(a->name.c_str());
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    // ── Active Layers section ──────────────────────────────────────────

    SectionHeader("Active Layers");

    int removeIdx = -1;

    if (settings.layers.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
        ImGui::TextUnformatted("No layers. Use the buttons above to add one.");
        ImGui::PopStyleColor();
    }
    else
    {
        constexpr float kRowH = 28.f;
        float layerListH = std::min(4.0f, std::max(1.0f, (float)settings.layers.size())) * kRowH + 8.f;
        ImGui::BeginChild("##hm_layers", ImVec2(0, layerListH), true);

        for (size_t i = 0; i < settings.layers.size(); ++i)
        {
            auto& layer = settings.layers[i];
            ImGui::PushID(static_cast<int>(i));

            bool canToggle = layer.matched;
            if (!canToggle) ImGui::BeginDisabled();
            if (ImGui::Checkbox("##en", &layer.enabled))
                changed = true;
            if (!canToggle) ImGui::EndDisabled();

            ImGui::SameLine();

            // Palette swatch
            auto* lutSrv = getLutSRV(layer.palette);
            if (lutSrv)
                ImGui::Image(reinterpret_cast<ImTextureID>(lutSrv), ImVec2(40, 10));
            else
                ImGui::Dummy(ImVec2(40, 10));
            if (ImGui::IsItemClicked())
                ImGui::OpenPopup("##palette_pick");

            if (ImGui::BeginPopup("##palette_pick"))
            {
                for (int p = 0; p < static_cast<int>(HeatmapPalette::COUNT); ++p)
                {
                    HeatmapPalette pal = static_cast<HeatmapPalette>(p);
                    auto* srv = getLutSRV(pal);
                    ImGui::PushID(p);
                    bool selected = (layer.palette == pal);
                    if (srv)
                    {
                        if (ImGui::ImageButton("##pal",
                                reinterpret_cast<ImTextureID>(srv), ImVec2(60, 12),
                                ImVec2(0, 0), ImVec2(1, 1),
                                selected ? ImVec4(0.4f, 0.35f, 0.15f, 1.f) : ImVec4(0, 0, 0, 0)))
                        {
                            layer.palette = pal;
                            changed = true;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    else
                    {
                        if (ImGui::Button(GetPaletteName(pal), ImVec2(60, 16)))
                        {
                            layer.palette = pal;
                            changed = true;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(GetPaletteName(pal));
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }

            ImGui::SameLine();

            // Subject name
            if (!layer.matched)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kDimText);
                char buf[256];
                snprintf(buf, sizeof(buf), "%s (not in match)", layer.subjectName.c_str());
                ImGui::TextUnformatted(buf);
                ImGui::PopStyleColor();
            }
            else
            {
                if (layer.subjectType == HeatmapSubjectType::TEAM && layer.subjectId == 1)
                    ImGui::PushStyleColor(ImGuiCol_Text, kBlueText);
                else if (layer.subjectType == HeatmapSubjectType::TEAM && layer.subjectId == 2)
                    ImGui::PushStyleColor(ImGuiCol_Text, kRedText);
                else if (layer.subjectType == HeatmapSubjectType::DOMINANCE)
                    ImGui::PushStyleColor(ImGuiCol_Text, kGoldText);
                else
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));

                ImGui::TextUnformatted(layer.subjectName.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::SameLine();

            // Opacity inline edit
            ImGui::SetNextItemWidth(36.f);
            char opLabel[32];
            snprintf(opLabel, sizeof(opLabel), "##op_%d", (int)i);
            if (ImGui::InputFloat(opLabel, &layer.opacity, 0.0f, 0.0f, "%.2f"))
            {
                layer.opacity = std::clamp(layer.opacity, 0.01f, 1.0f);
                changed = true;
            }

            ImGui::SameLine();

            // Remove button
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.2f, 0.2f, 1.f));
            if (ImGui::SmallButton("x"))
                removeIdx = static_cast<int>(i);
            ImGui::PopStyleColor();

            ImGui::PopID();
        }

        ImGui::EndChild();
    }

    if (removeIdx >= 0 && removeIdx < (int)settings.layers.size())
    {
        settings.layers.erase(settings.layers.begin() + removeIdx);
        changed = true;
    }

    // ── Time Range section ─────────────────────────────────────────────

    SectionHeader("Time Range");
    {
        bool isFull   = (settings.timeRange == HeatmapTimeRange::FULL_MATCH);
        bool isWindow = (settings.timeRange == HeatmapTimeRange::CURRENT_WINDOW);
        if (ImGui::RadioButton("Full Match", isFull))
        { settings.timeRange = HeatmapTimeRange::FULL_MATCH; changed = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("Current Window", isWindow))
        { settings.timeRange = HeatmapTimeRange::CURRENT_WINDOW; changed = true; }

        if (isWindow)
        {
            ImGui::SetNextItemWidth(80.f);
            int sec = static_cast<int>(settings.windowSeconds);
            if (ImGui::InputInt("seconds", &sec, 0, 0))
            {
                settings.windowSeconds = static_cast<float>(std::clamp(sec, 10, 600));
                changed = true;
            }
        }
    }

    ImGui::Separator();

    if (ImGui::Checkbox("Show colour legend", &settings.showLegend))
        changed = true;

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(6);
    return changed;
}

// ── Legend overlay (one bar per visible layer, bottom-left) ─────────────

void DrawHeatmapLegend(const HeatmapSettings& settings,
                       const LutSrvGetter& getLutSRV)
{
    if (!settings.renderEnabled || !settings.showLegend)
        return;

    int visCount = 0;
    for (auto& l : settings.layers)
        if (l.enabled && l.matched) ++visCount;
    if (visCount == 0) return;

    const float barW    = 20.f;
    const float barH    = 100.f;
    const float entryH  = barH + 48.f;
    const float pad     = 12.f;
    const float winW    = barW + 96.f;
    const float winH    = visCount * entryH + 16.f;

    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec2 winPos(pad, display.y - winH - pad);

    ImGui::SetNextWindowPos(winPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    if (ImGui::Begin("##HeatmapLegend", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing))
    {
        for (auto& layer : settings.layers)
        {
            if (!layer.enabled || !layer.matched) continue;
            auto* srv = getLutSRV(layer.palette);
            if (!srv) continue;

            ImGui::PushStyleColor(ImGuiCol_Text, kGoldText);
            ImGui::TextUnformatted("High");
            ImGui::PopStyleColor();

            ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p0(cursor.x, cursor.y);
            ImVec2 p1(cursor.x + barW, cursor.y + barH);
            dl->AddImage(reinterpret_cast<ImTextureID>(srv),
                         p0, p1,
                         ImVec2(1.0f, 0.0f), ImVec2(0.0f, 1.0f));
            ImGui::Dummy(ImVec2(barW, barH));

            ImGui::PushStyleColor(ImGuiCol_Text, kGoldText);
            ImGui::TextUnformatted("Low");
            ImGui::PopStyleColor();

            ImGui::TextUnformatted(layer.subjectName.c_str());
            ImGui::Spacing();
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}
