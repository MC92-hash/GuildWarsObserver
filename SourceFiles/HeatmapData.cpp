#include "pch.h"
#include "HeatmapData.h"
#include <cstring>

void HeatmapAccumulator::EnsureLayerCount(size_t n)
{
    if (m_layerStates.size() == n) return;
    m_layerStates.resize(n);
}

bool HeatmapAccumulator::RebuildLayerIfDirty(
    size_t layerIdx,
    const HeatmapLayerDef& def,
    float currentTimeSec,
    HeatmapTimeRange timeRange,
    float windowSec,
    const std::unordered_map<int, uint8_t>& agentTeams)
{
    if (layerIdx >= m_layerStates.size()) return false;
    auto& ls = m_layerStates[layerIdx];

    bool paramsChanged =
        def.subjectId   != ls.prevSubjectId  ||
        def.subjectType != ls.prevType       ||
        timeRange       != ls.prevTimeRange  ||
        windowSec       != ls.prevWindowSec;

    float quantised     = std::floor(currentTimeSec * 2.0f) * 0.5f;
    float prevQuantised = std::floor(ls.prevTime * 2.0f) * 0.5f;
    bool  timeChanged   = (quantised != prevQuantised);

    if (!ls.samplesDirty && !paramsChanged && !timeChanged)
        return false;

    ls.prevSubjectId = def.subjectId;
    ls.prevType      = def.subjectType;
    ls.prevTimeRange = timeRange;
    ls.prevWindowSec = windowSec;
    ls.prevTime      = currentTimeSec;

    std::fill(ls.density.begin(), ls.density.end(), 0.0f);

    uint32_t tMaxMs = (currentTimeSec > 0)
        ? static_cast<uint32_t>(currentTimeSec * 1000.0f) : 0;

    uint32_t tMinMs = 0;
    if (timeRange == HeatmapTimeRange::CURRENT_WINDOW)
    {
        float lo = currentTimeSec - windowSec;
        tMinMs = (lo > 0) ? static_cast<uint32_t>(lo * 1000.0f) : 0;
    }

    if (def.subjectType == HeatmapSubjectType::DOMINANCE)
    {
        std::vector<float> blueGrid(kTexSize * kTexSize, 0.0f);
        std::vector<float> redGrid(kTexSize * kTexSize, 0.0f);
        for (auto& [agentId, buf] : m_buffers)
        {
            auto teamIt = agentTeams.find(agentId);
            if (teamIt == agentTeams.end()) continue;
            if (teamIt->second == 1)
                SplatSamples(blueGrid, buf, tMinMs, tMaxMs);
            else if (teamIt->second == 2)
                SplatSamples(redGrid, buf, tMinMs, tMaxMs);
        }
        float maxB = *std::max_element(blueGrid.begin(), blueGrid.end());
        float maxR = *std::max_element(redGrid.begin(), redGrid.end());
        float maxAll = std::max(maxB, maxR);
        if (maxAll > 0.0f)
        {
            float inv = 1.0f / maxAll;
            const int totalCells = kTexSize * kTexSize;
            for (int i = 0; i < totalCells; ++i)
            {
                float b = blueGrid[i] * inv;
                float r = redGrid[i] * inv;
                float sum = b + r;
                if (sum < 0.01f) { ls.density[i] = 0.0f; continue; }

                // ratio: 0 = pure blue, 0.5 = contested, 1 = pure red
                float ratio = r / sum;
                // Map directly into the LUT range [0.15, 0.85]
                //   0.15 = bright blue, 0.50 = warm white (contested), 0.85 = bright red
                // No presence scaling — both sides get equal visual weight;
                // the Gaussian splat falloff and sum threshold handle edge fading.
                ls.density[i] = 0.15f + ratio * 0.70f;
            }
        }
    }
    else if (def.subjectType == HeatmapSubjectType::PLAYER)
    {
        auto it = m_buffers.find(def.subjectId);
        if (it != m_buffers.end())
            SplatSamples(ls.density, it->second, tMinMs, tMaxMs);
    }
    else
    {
        for (auto& [agentId, buf] : m_buffers)
        {
            auto teamIt = agentTeams.find(agentId);
            if (teamIt == agentTeams.end()) continue;
            if (teamIt->second != static_cast<uint8_t>(def.subjectId)) continue;
            SplatSamples(ls.density, buf, tMinMs, tMaxMs);
        }
    }

    if (def.subjectType != HeatmapSubjectType::DOMINANCE)
    {
        float maxVal = 0.0f;
        for (float v : ls.density)
            if (v > maxVal) maxVal = v;

        if (maxVal > 0.0f)
        {
            float inv = 1.0f / maxVal;
            constexpr float kGamma = 0.35f;
            for (float& v : ls.density)
            {
                v *= inv;
                if (v > 0.0f)
                    v = std::pow(v, kGamma);
            }
        }
    }

    ls.samplesDirty = false;
    ls.textureDirty = true;
    return true;
}

void HeatmapAccumulator::SplatSamples(
    std::vector<float>& density,
    const PerAgentSampleBuffer& buf,
    uint32_t tMinMs, uint32_t tMaxMs)
{
    const float cellW = (m_maxX - m_minX) / static_cast<float>(kTexSize);
    const float cellH = (m_maxZ - m_minZ) / static_cast<float>(kTexSize);
    const float radiusCells = kSplatRadius / std::max(cellW, cellH);
    const int   kernelR = static_cast<int>(std::ceil(radiusCells));
    const float invR2 = 1.0f / (radiusCells * radiusCells);

    for (size_t i = 0; i < buf.Count(); ++i)
    {
        const auto& s = buf[i];
        if (s.timestamp_ms < tMinMs || s.timestamp_ms > tMaxMs)
            continue;

        float u = (s.x - m_minX) * m_invW;
        float v = (s.z - m_minZ) * m_invH;

        int cx = static_cast<int>(u * kTexSize);
        int cy = static_cast<int>(v * kTexSize);

        int x0 = std::max(0, cx - kernelR);
        int x1 = std::min(kTexSize - 1, cx + kernelR);
        int y0 = std::max(0, cy - kernelR);
        int y1 = std::min(kTexSize - 1, cy + kernelR);

        for (int py = y0; py <= y1; ++py)
        {
            float dy = static_cast<float>(py - cy);
            for (int px = x0; px <= x1; ++px)
            {
                float dx = static_cast<float>(px - cx);
                float d2 = (dx * dx + dy * dy) * invR2;
                if (d2 >= 1.0f) continue;
                float weight = std::exp(-3.5f * d2);
                density[py * kTexSize + px] += weight;
            }
        }
    }
}
