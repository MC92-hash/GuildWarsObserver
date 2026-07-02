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
#include "ReplayWindow_Internal.h"
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


// ===========================================================================
// Heatmap system (layer-stack)
// ===========================================================================

void ReplayWindow::InitHeatmapRenderer()
{
    if (m_heatmapInitialized) return;
    m_heatmapInitialized = true;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    m_heatmapRenderer.Init(dev);

    size_t n = m_heatmapSettings.layers.size();
    m_heatmapAccumulator.EnsureLayerCount(n);
    m_heatmapRenderer.EnsureLayerTextures(dev, n);
}


void ReplayWindow::PopulateHeatmapFromSnapshots()
{
    if (m_heatmapPopulated) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;
    m_heatmapPopulated = true;

    const MapTransform& t = m_replayCtx.mapTransform;

    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Player) continue;
        if (ard.snapshots.empty()) continue;

        for (const auto& snap : ard.snapshots)
        {
            XMFLOAT3 wpos = ApplyMapTransformToPos(snap.x, snap.y, snap.z, t);
            uint32_t tsMs = static_cast<uint32_t>(snap.time * 1000.0f);
            m_heatmapAccumulator.OnAgentMoved(agentId, wpos.x, wpos.z, tsMs);
        }
    }
}


void ReplayWindow::UpdateHeatmapSamples()
{
    if (!m_heatmapSettings.renderEnabled) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;
    PopulateHeatmapFromSnapshots();
}


void ReplayWindow::DrawHeatmapOverlay()
{
    if (!m_heatmapSettings.renderEnabled) return;
    if (!m_agentsClassified) return;
    if (m_heatmapSettings.layers.empty()) return;

    InitHeatmapRenderer();

    Terrain* terrain = m_mapRenderer->GetTerrain();
    if (!terrain) return;

    if (!m_heatmapMeshBuilt)
    {
        const auto& b = terrain->m_bounds;
        m_heatmapAccumulator.SetWorldBounds(b.map_min_x, b.map_max_x,
                                            b.map_min_z, b.map_max_z);
        float waterY = m_mapRenderer->GetWaterLevel();
        m_heatmapRenderer.BuildMesh(m_deviceResources->GetD3DDevice(), terrain,
                                    b.map_min_x, b.map_max_x,
                                    b.map_min_z, b.map_max_z,
                                    waterY);
        m_heatmapMeshBuilt = true;
    }

    if (!m_heatmapRenderer.IsReady()) return;

    UpdateHeatmapSamples();

    std::unordered_map<int, uint8_t> agentTeams;
    for (auto& [agentId, ard] : m_replayCtx.agents)
        agentTeams[agentId] = ard.teamId;

    size_t layerCount = m_heatmapSettings.layers.size();
    m_heatmapAccumulator.EnsureLayerCount(layerCount);
    m_heatmapRenderer.EnsureLayerTextures(m_deviceResources->GetD3DDevice(), layerCount);

    auto* ctx = m_deviceResources->GetD3DDeviceContext();

    for (size_t i = 0; i < layerCount; ++i)
    {
        const auto& def = m_heatmapSettings.layers[i];
        if (!def.enabled || !def.matched) continue;

        m_heatmapAccumulator.RebuildLayerIfDirty(
            i, def, m_debugTimeline,
            m_heatmapSettings.timeRange, m_heatmapSettings.windowSeconds,
            agentTeams);

        m_heatmapRenderer.UpdateLayerDensityTexture(ctx, i, m_heatmapAccumulator);
        m_heatmapAccumulator.ClearLayerTextureDirty(i);
    }

    Camera* cam = m_mapRenderer->GetCamera();
    XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    m_heatmapRenderer.RenderLayers(ctx, viewProj, m_heatmapSettings.layers);
}


// ---------------------------------------------------------------------------
// Resolve layer subjects against current match agents
// ---------------------------------------------------------------------------

void ReplayWindow::ResolveHeatmapLayers()
{
    if (!m_agentsClassified) return;

    for (auto& layer : m_heatmapSettings.layers)
    {
        if (layer.subjectType == HeatmapSubjectType::TEAM ||
            layer.subjectType == HeatmapSubjectType::DOMINANCE)
        {
            layer.matched = true;
            continue;
        }

        layer.matched = false;
        for (auto& [agentId, ard] : m_replayCtx.agents)
        {
            if (ard.type != AgentType::Player) continue;
            std::string name = ard.playerName.empty() ? ard.categoryName : ard.playerName;
            if (name == layer.subjectName)
            {
                layer.subjectId = agentId;
                layer.matched = true;
                break;
            }
        }
    }
}


void ReplayWindow::SaveHeatmapSettings()
{
    // No persistence — heatmap starts fresh each session
}


void ReplayWindow::LoadHeatmapSettings()
{
    m_heatmapSettings = HeatmapSettings{};
}
