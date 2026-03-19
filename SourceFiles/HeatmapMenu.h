#pragma once

#include "imgui.h"
#include "HeatmapData.h"
#include <vector>
#include <string>
#include <d3d11.h>
#include <functional>

struct AgentMenuEntry
{
    int         agentId;
    std::string name;
    uint8_t     teamId;
    ImTextureID profIcon = nullptr;
};

using LutSrvGetter = std::function<ID3D11ShaderResourceView*(HeatmapPalette)>;

// Draws the Heatmap floating panel (ImGui::Begin window).
// Returns true if any setting was changed (caller should persist + dirty).
bool DrawHeatmapPanel(HeatmapSettings& settings,
                      const std::vector<AgentMenuEntry>& agents,
                      const LutSrvGetter& getLutSRV);

// Draws the colour-legend overlay (one bar per visible layer).
void DrawHeatmapLegend(const HeatmapSettings& settings,
                       const LutSrvGetter& getLutSRV);

const char* GetPaletteName(HeatmapPalette p);
