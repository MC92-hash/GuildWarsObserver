#pragma once

#include <vector>
#include <cstdint>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <string>

struct HeatmapSample
{
    float    x, z;
    uint32_t timestamp_ms;
};

class PerAgentSampleBuffer
{
public:
    static constexpr size_t kMaxSamples = 30000;
    static constexpr float  kMoveThreshold = 5.0f;

    void AddSample(float x, float z, uint32_t timestamp_ms)
    {
        if (m_count > 0)
        {
            const auto& prev = m_samples[(m_head + m_count - 1) % kMaxSamples];
            float dx = x - prev.x;
            float dz = z - prev.z;
            if (dx * dx + dz * dz < kMoveThreshold * kMoveThreshold)
                return;
        }

        size_t idx;
        if (m_count < kMaxSamples)
        {
            idx = (m_head + m_count) % kMaxSamples;
            ++m_count;
        }
        else
        {
            idx = m_head;
            m_head = (m_head + 1) % kMaxSamples;
        }
        m_samples[idx] = { x, z, timestamp_ms };
    }

    size_t Count() const { return m_count; }

    const HeatmapSample& operator[](size_t i) const
    {
        return m_samples[(m_head + i) % kMaxSamples];
    }

    void Clear()
    {
        m_count = 0;
        m_head  = 0;
    }

private:
    HeatmapSample m_samples[kMaxSamples]{};
    size_t m_head  = 0;
    size_t m_count = 0;
};

// ── Enums ──────────────────────────────────────────────────────────────

enum class HeatmapTimeRange { FULL_MATCH, CURRENT_WINDOW };

enum class HeatmapPalette : int {
    THERMAL = 0,
    INFERNO,
    VIRIDIS,
    TEAM_RED,
    TEAM_BLUE,
    DOMINANCE,
    LAVA,
    SUNSET,
    AMBER,
    COUNT
};

enum class HeatmapSubjectType : int { TEAM = 0, PLAYER, DOMINANCE };

// ── Per-layer definition (config + runtime) ────────────────────────────

struct HeatmapLayerDef
{
    bool                enabled     = true;
    HeatmapSubjectType  subjectType = HeatmapSubjectType::TEAM;
    int                 subjectId   = 1;        // teamId (1/2) or runtime agentId
    std::string         subjectName;            // display label + config match key
    HeatmapPalette      palette     = HeatmapPalette::THERMAL;
    float               opacity     = 0.50f;
    bool                matched     = true;     // false = player not in this replay
};

// ── Global settings ────────────────────────────────────────────────────

struct HeatmapSettings
{
    bool                show          = false;  // panel visible
    bool                renderEnabled = false;  // 3D heatmap overlay active
    HeatmapTimeRange    timeRange     = HeatmapTimeRange::FULL_MATCH;
    float               windowSeconds = 60.0f;
    bool                showLegend    = false;
    std::vector<HeatmapLayerDef> layers;
};

// ── Per-layer CPU-side state (density grid + dirty flags) ──────────────

struct HeatmapLayerState
{
    std::vector<float> density;
    bool samplesDirty  = true;
    bool textureDirty  = false;

    int              prevSubjectId  = -1;
    HeatmapSubjectType prevType    = HeatmapSubjectType::TEAM;
    float            prevTime      = -1.0f;
    HeatmapTimeRange prevTimeRange = HeatmapTimeRange::FULL_MATCH;
    float            prevWindowSec = 60.0f;

    HeatmapLayerState()
        : density(512 * 512, 0.0f) {}
};

// ── Accumulator (shared sample buffers, per-layer density grids) ───────

class HeatmapAccumulator
{
public:
    static constexpr int   kTexSize     = 512;
    static constexpr float kSplatRadius = 120.0f;

    void SetWorldBounds(float minX, float maxX, float minZ, float maxZ)
    {
        m_minX = minX; m_maxX = maxX;
        m_minZ = minZ; m_maxZ = maxZ;
        m_invW = (maxX > minX) ? 1.0f / (maxX - minX) : 1.0f;
        m_invH = (maxZ > minZ) ? 1.0f / (maxZ - minZ) : 1.0f;
    }

    void OnAgentMoved(int agentId, float x, float z, uint32_t timestamp_ms)
    {
        m_buffers[agentId].AddSample(x, z, timestamp_ms);
    }

    // Resize the layer-state vector to match the settings layer count
    void EnsureLayerCount(size_t n);

    // Rebuild one layer's density grid if its inputs changed.
    // Returns true if the grid was actually rebuilt.
    bool RebuildLayerIfDirty(size_t layerIdx,
                             const HeatmapLayerDef& def,
                             float currentTimeSec,
                             HeatmapTimeRange timeRange,
                             float windowSec,
                             const std::unordered_map<int, uint8_t>& agentTeams);

    const float* GetLayerDensityData(size_t idx) const
    {
        return m_layerStates[idx].density.data();
    }

    bool IsLayerTextureDirty(size_t idx) const
    {
        return idx < m_layerStates.size() && m_layerStates[idx].textureDirty;
    }

    void ClearLayerTextureDirty(size_t idx)
    {
        if (idx < m_layerStates.size())
            m_layerStates[idx].textureDirty = false;
    }

    void MarkAllLayersDirty()
    {
        for (auto& ls : m_layerStates)
            ls.samplesDirty = true;
    }

    void MarkLayerDirty(size_t idx)
    {
        if (idx < m_layerStates.size())
            m_layerStates[idx].samplesDirty = true;
    }

    void Clear()
    {
        m_buffers.clear();
        for (auto& ls : m_layerStates)
        {
            std::fill(ls.density.begin(), ls.density.end(), 0.0f);
            ls.samplesDirty = true;
            ls.textureDirty = true;
        }
    }

    const PerAgentSampleBuffer* GetBuffer(int agentId) const
    {
        auto it = m_buffers.find(agentId);
        return it != m_buffers.end() ? &it->second : nullptr;
    }

    size_t LayerCount() const { return m_layerStates.size(); }

private:
    void SplatSamples(std::vector<float>& density,
                      const PerAgentSampleBuffer& buf,
                      uint32_t tMinMs, uint32_t tMaxMs);

    std::unordered_map<int, PerAgentSampleBuffer> m_buffers;
    std::vector<HeatmapLayerState> m_layerStates;

    float m_minX = 0, m_maxX = 1, m_minZ = 0, m_maxZ = 1;
    float m_invW = 1, m_invH = 1;
};
