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


// ---------------------------------------------------------------------------
// Asset Inspector / Blacklist debug panel
// ---------------------------------------------------------------------------

void ReplayWindow::SetPropHighlight(int propIndex, uint32_t state)
{
    if (propIndex < 0) return;
    auto meshIt = m_mapRenderer->GetPropsMeshIds().find(static_cast<uint32_t>(propIndex));
    if (meshIt == m_mapRenderer->GetPropsMeshIds().end()) return;
    for (int mid : meshIt->second)
    {
        auto data = m_mapRenderer->GetMeshManager()->GetMeshPerObjectData(mid);
        if (data.has_value()) {
            auto updated = data.value();
            updated.highlight_state = state;
            m_mapRenderer->GetMeshManager()->UpdateMeshPerObjectData(mid, updated);
        }
    }
}


void ReplayWindow::SetPickedProp(int newPropIndex)
{
    if (newPropIndex == m_pickedPropIndex) return;
    SetPropHighlight(m_pickedPropIndex, 0);
    m_pickedPropIndex = newPropIndex;
    SetPropHighlight(m_pickedPropIndex, 1);
}


void ReplayWindow::SetPropAlpha(int propIndex, float alpha)
{
    if (propIndex < 0) return;
    auto meshIt = m_mapRenderer->GetPropsMeshIds().find(static_cast<uint32_t>(propIndex));
    if (meshIt == m_mapRenderer->GetPropsMeshIds().end()) return;
    for (int mid : meshIt->second)
    {
        auto data = m_mapRenderer->GetMeshManager()->GetMeshPerObjectData(mid);
        if (data.has_value()) {
            auto updated = data.value();
            updated.mesh_alpha = alpha;
            m_mapRenderer->GetMeshManager()->UpdateMeshPerObjectData(mid, updated);
        }
    }
}


void ReplayWindow::DrawAssetInspectorWindow()
{
    ImGui::SetNextWindowSize(ImVec2(420, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Asset Inspector / Blacklist", &m_showAssetInspector))
    {
        if (!m_showAssetInspector) {
            SetPickedProp(-1);
            m_assetSelectionEnabled = false;
        }
        ImGui::End();
        return;
    }

    bool wasEnabled = m_assetSelectionEnabled;
    ImGui::Checkbox("Enable Asset Selection", &m_assetSelectionEnabled);
    if (wasEnabled && !m_assetSelectionEnabled)
        SetPickedProp(-1);
    if (m_assetSelectionEnabled)
        ImGui::TextDisabled("Left-click on the map to select an asset");

    ImGui::Separator();

    const auto& propsInfo = m_mapFile.props_info_chunk.prop_array.props_info;
    const auto& propFN = m_mapFile.prop_filenames_chunk.array;
    const auto& moreFN = m_mapFile.more_filnames_chunk.array;

    // --- Selected asset info ---
    if (m_pickedPropIndex >= 0 && m_pickedPropIndex < static_cast<int>(propsInfo.size()))
    {
        ImGui::SeparatorText("Selected Asset");

        const auto& prop = propsInfo[m_pickedPropIndex];
        ImGui::Text("Prop Index: %d", m_pickedPropIndex);
        ImGui::Text("Filename Index: %u", static_cast<unsigned>(prop.filename_index));
        ImGui::Text("Position: (%.1f, %.1f, %.1f)", prop.x, prop.y, prop.z);
        ImGui::Text("Scale: %.3f", prop.scaling_factor);

        int fnIdx = static_cast<int>(prop.filename_index);
        int totalFN = static_cast<int>(propFN.size() + moreFN.size());
        if (fnIdx >= 0 && fnIdx < totalFN)
        {
            const auto& fn = (fnIdx < static_cast<int>(propFN.size()))
                ? propFN[fnIdx] : moreFN[fnIdx - static_cast<int>(propFN.size())];
            auto datHash = decode_filename(fn.filename.id0, fn.filename.id1);
            ImGui::Text("DAT Hash: 0x%X", datHash);
        }

        auto& blacklist = AssetBlacklist::Get();
        uint32_t selIdx = static_cast<uint32_t>(m_pickedPropIndex);
        bool alreadyBlacklisted = blacklist.HasEntry(selIdx);
        if (alreadyBlacklisted)
        {
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "Already blacklisted");
        }
        else
        {
            bool hasAlpha = blacklist.HasAlphaOverride(selIdx);
            if (ImGui::Button("Blacklist Asset"))
            {
                blacklist.SetEntry(selIdx, true);
                auto meshIt = m_mapRenderer->GetPropsMeshIds().find(selIdx);
                if (meshIt != m_mapRenderer->GetPropsMeshIds().end())
                {
                    for (int mid : meshIt->second)
                        m_mapRenderer->GetMeshManager()->SetMeshShouldRender(mid, false);
                }
            }
            ImGui::SameLine();
            if (!hasAlpha)
            {
                if (ImGui::Button("Make Transparent"))
                {
                    blacklist.SetAlpha(selIdx, 0.3f);
                    SetPropAlpha(m_pickedPropIndex, 0.3f);
                }
            }
            else
            {
                if (ImGui::SmallButton("Reset Opacity"))
                {
                    blacklist.RemoveAlpha(selIdx);
                    SetPropAlpha(m_pickedPropIndex, 1.0f);
                }
            }

            if (hasAlpha)
            {
                float alpha = blacklist.GetAlpha(selIdx);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderFloat("##opacity", &alpha, 0.0f, 1.0f, "Opacity: %.2f"))
                {
                    blacklist.SetAlpha(selIdx, alpha);
                    SetPropAlpha(m_pickedPropIndex, alpha);
                }
            }
        }
    }
    else
    {
        ImGui::TextDisabled("No asset selected");
    }

    ImGui::Separator();

    // --- Blacklisted assets list ---
    ImGui::SeparatorText("Blacklisted Assets");
    ImGui::Text("Map ID: %d", m_replayCtx.mapId);

    auto& blacklist = AssetBlacklist::Get();
    const auto& entries = blacklist.GetEntries();

    if (entries.empty())
    {
        ImGui::TextDisabled("No blacklisted assets for this map");
    }
    else
    {
        // Sort entries by prop index for stable display order
        std::vector<uint32_t> sortedKeys;
        sortedKeys.reserve(entries.size());
        for (auto& [k, _] : entries)
            sortedKeys.push_back(k);
        std::sort(sortedKeys.begin(), sortedKeys.end());

        for (uint32_t propIdx : sortedKeys)
        {
            auto entryIt = entries.find(propIdx);
            if (entryIt == entries.end()) continue;

            ImGui::PushID(static_cast<int>(propIdx));

            bool hidden = entryIt->second;
            if (ImGui::Checkbox("##hidden", &hidden))
            {
                blacklist.SetEntry(propIdx, hidden);
                auto meshIt = m_mapRenderer->GetPropsMeshIds().find(propIdx);
                if (meshIt != m_mapRenderer->GetPropsMeshIds().end())
                {
                    for (int mid : meshIt->second)
                        m_mapRenderer->GetMeshManager()->SetMeshShouldRender(mid, !hidden);
                }
            }
            ImGui::SameLine();
            ImGui::Text("#%u", propIdx);

            if (propIdx < propsInfo.size())
            {
                const auto& prop = propsInfo[propIdx];
                ImGui::SameLine();
                ImGui::TextDisabled("(%.0f, %.0f, %.0f)", prop.x, prop.y, prop.z);
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Remove"))
            {
                blacklist.RemoveEntry(propIdx);
                auto meshIt = m_mapRenderer->GetPropsMeshIds().find(propIdx);
                if (meshIt != m_mapRenderer->GetPropsMeshIds().end())
                {
                    for (int mid : meshIt->second)
                        m_mapRenderer->GetMeshManager()->SetMeshShouldRender(mid, true);
                }
                ImGui::PopID();
                break;
            }

            ImGui::PopID();
        }
    }

    ImGui::Separator();

    // --- Transparent assets list ---
    ImGui::SeparatorText("Transparent Assets");

    const auto& alphaEntries = blacklist.GetAlphaEntries();

    if (alphaEntries.empty())
    {
        ImGui::TextDisabled("No transparency overrides for this map");
    }
    else
    {
        std::vector<uint32_t> alphaSortedKeys;
        alphaSortedKeys.reserve(alphaEntries.size());
        for (auto& [k, _] : alphaEntries)
            alphaSortedKeys.push_back(k);
        std::sort(alphaSortedKeys.begin(), alphaSortedKeys.end());

        for (uint32_t propIdx : alphaSortedKeys)
        {
            auto alphaIt = alphaEntries.find(propIdx);
            if (alphaIt == alphaEntries.end()) continue;

            ImGui::PushID(static_cast<int>(propIdx) + 100000);

            float alpha = alphaIt->second;
            ImGui::Text("#%u", propIdx);
            if (propIdx < propsInfo.size())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%.0f, %.0f, %.0f)",
                    propsInfo[propIdx].x, propsInfo[propIdx].y, propsInfo[propIdx].z);
            }
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::SliderFloat("##alpha", &alpha, 0.0f, 1.0f, "%.2f"))
            {
                blacklist.SetAlpha(propIdx, alpha);
                SetPropAlpha(static_cast<int>(propIdx), alpha);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove"))
            {
                blacklist.RemoveAlpha(propIdx);
                SetPropAlpha(static_cast<int>(propIdx), 1.0f);
                ImGui::PopID();
                break;
            }

            ImGui::PopID();
        }
    }

    ImGui::End();
}
