#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include <algorithm>
#include <cctype>

// ---------------------------------------------------------------------------
// Energy bars. Nothing in a recording states another player's energy, so the value is calculated
// forward from the match start by EnergyModel (private): costs, regeneration, weapon sets,
// overcast and every recorded event that moves it. This file only wires the window's timelines
// into the model and draws the result.
// ---------------------------------------------------------------------------

void ReplayWindow::BuildEnergyTracks() const
{
    if (!m_moraleTimelineBuilt) BuildMoraleTimelines();

    EnergyModel::Inputs in;
    in.skills    = &m_skillView;
    in.stoc      = &m_replayCtx.stocData;
    in.equipment = &m_replayCtx.stocData.equipment;
    in.matchEnd  = m_replayCtx.maxReplayTime;
    in.attributeRank = [this](int agentId, int attribute) -> int {
        auto it = m_attrProfiles.find(agentId);
        if (it == m_attrProfiles.end()) return -1;
        auto a = it->second.attributes.find(attribute);
        if (a == it->second.attributes.end() || a->second.budgetOnly) return -1;
        return a->second.best;
    };
    in.moralePercent = [this](const AgentReplayData& ard, float t) { return MoralePercentAtTime(ard, t); };
    in.moraleBoosts = [this](int teamId) -> std::vector<float> {
        auto it = m_moraleBoosts.find(teamId);
        return it == m_moraleBoosts.end() ? std::vector<float>{} : it->second;
    };

    m_energyTracks = EnergyModel::Build(m_replayCtx.agents, in);
    m_energyBuilt = true;
    m_energyUsedAttributes = m_attributesDeduced;

    // Developer dump, the same way the attribute solve writes its own: %TEMP%\gwo_energy_<match>.txt
    // whenever GWO_ENERGY_DEBUG is set.
    char flag[16] = {};
    char tempDir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("GWO_ENERGY_DEBUG", flag, (DWORD)sizeof(flag)) != 0 &&
        GetTempPathA(MAX_PATH, tempDir) != 0)
    {
        std::string folder = m_replayCtx.matchFolderPath.filename().string();
        if (folder.empty()) folder = "replay";
        for (char& c : folder)
            if (!std::isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '_';
        EnergyModel::WriteDebugDump(std::string(tempDir) + "gwo_energy_" + folder + ".txt",
                                    m_replayCtx.agents, m_energyTracks, m_skillView);
    }
}

const EnergyModel::Track* ReplayWindow::EnergyTrackFor(int agentId) const
{
    if (!m_agentsClassified || !m_replayCtx.stocLoaded) return nullptr;
    if (!m_energyBuilt || (!m_energyUsedAttributes && m_attributesDeduced))
        BuildEnergyTracks();
    auto it = m_energyTracks.find(agentId);
    return it == m_energyTracks.end() || it->second.samples.empty() ? nullptr : &it->second;
}

// The bar spans the full maximum; the part overcast took is greyed at its right end, as the client
// does.
void DrawEnergyOvercast(ImDrawList* dl, ImVec2 b0, ImVec2 b1, const EnergyModel::Sample& s)
{
    const float full = s.maxEnergy + s.overcast;
    if (s.overcast <= 0.f || full <= 0.f) return;
    const float x = b0.x + (b1.x - b0.x) * std::clamp(s.maxEnergy / full, 0.f, 1.f);
    dl->AddRectFilled(ImVec2(x, b0.y), b1, IM_COL32(0x80, 0x80, 0x80, 0xB0));
}

float EnergyBarFraction(const EnergyModel::Sample& s)
{
    const float full = s.maxEnergy + s.overcast;
    return full > 0.f ? std::clamp(std::max(0.f, s.energy) / full, 0.f, 1.f) : 0.f;
}

// The thin energy strip under a party row's health bar.
//
// It shows the energy a player has to work with: what they would hold on their BEST weapon set. A
// swap moves current energy and maximum by the same amount (and below zero when they hide energy),
// so this number stays put through every swap and moves only when they spend, regenerate or are
// drained. 36 on a +47 set reads 36/72; swapped to a +0 set the player holds -11 (the client shows 0),
// and the strip still reads 36/72.
//   [0, energy on best set]   blue, orange under 30 %, red under 15 %
//   [.., best set max]        near-black
//   grey                      taken by overcast
//   light marker              the maximum of the set in hand; hatched beyond it (only reachable
//                             by swapping)
void ReplayWindow::DrawPartyEnergyBar(ImDrawList* dl, const AgentReplayData& ard, ImVec2 b0, ImVec2 b1) const
{
    const EnergyModel::Track* track = EnergyTrackFor(ard.agent_id);
    if (!track) return;
    const EnergyModel::Sample s = track->At(m_debugTimeline);

    const float lift    = static_cast<float>(std::max(0, track->peakWeaponTerm - s.weaponTerm));
    const float setMax  = s.maxEnergy + s.overcast;         // the set in hand, before overcast
    const float bestMax = setMax + lift;                     // the best set, now
    const float best    = s.energy + lift;                   // energy on the best set, now
    if (bestMax <= 0.f) return;
    const float w = b1.x - b0.x;
    auto X = [&](float v) { return std::round(b0.x + w * std::clamp(v / bestMax, 0.f, 1.f)); };

    // An outline round the whole strip, so it reads on a red and a blue panel alike.
    dl->AddRectFilled(ImVec2(b0.x - 1.f, b0.y - 1.f), ImVec2(b1.x + 1.f, b1.y + 1.f), IM_COL32(0, 0, 0, 0xFF));

    const float xSet = X(setMax);
    dl->AddRectFilled(b0, ImVec2(xSet, b1.y), IM_COL32(0x0C, 0x0C, 0x12, 0xFF));
    if (xSet < b1.x)
    {
        dl->AddRectFilled(ImVec2(xSet, b0.y), b1, IM_COL32(0x22, 0x26, 0x34, 0xFF));
        dl->PushClipRect(ImVec2(xSet, b0.y), b1, true);
        const float h = b1.y - b0.y;
        for (float x = xSet - h; x < b1.x; x += 4.f)
            dl->AddLine(ImVec2(x, b1.y), ImVec2(x + h, b0.y), IM_COL32(0x78, 0x8C, 0xB4, 0xC0), 1.f);
        dl->PopClipRect();
    }
    if (!s.dead)
    {
        const float frac = std::max(0.f, best) / bestMax;
        ImU32 top = IM_COL32(0x5A, 0x96, 0xFF, 0xFF), bot = IM_COL32(0x23, 0x50, 0xD2, 0xFF);
        ImU32 edge = IM_COL32(0xB4, 0xD2, 0xFF, 0xFF);
        if (frac < 0.15f)      { top = IM_COL32(0xFF, 0x4A, 0x3C, 0xFF); bot = IM_COL32(0xB4, 0x1E, 0x14, 0xFF); edge = IM_COL32(0xFF, 0xB4, 0xA8, 0xFF); }
        else if (frac < 0.30f) { top = IM_COL32(0xFF, 0xA0, 0x32, 0xFF); bot = IM_COL32(0xC8, 0x64, 0x0A, 0xFF); edge = IM_COL32(0xFF, 0xDC, 0xA0, 0xFF); }
        const float x = X(std::max(0.f, best));
        if (x > b0.x)
        {
            dl->AddRectFilledMultiColor(b0, ImVec2(x, b1.y), top, top, bot, bot);
            dl->AddLine(ImVec2(b0.x, b0.y + 0.5f), ImVec2(x, b0.y + 0.5f), edge);
        }
    }
    if (s.overcast > 0.f)
        dl->AddRectFilled(ImVec2(X(s.maxEnergy + lift), b0.y), ImVec2(X(bestMax), b1.y), IM_COL32(0x8C, 0x8C, 0x8C, 0xFF));
    if (xSet < b1.x)
        dl->AddLine(ImVec2(xSet + 0.5f, b0.y - 1.f), ImVec2(xSet + 0.5f, b1.y + 1.f), IM_COL32(0xE6, 0xEB, 0xF5, 0xFF), 1.f);

    if (ImGui::IsMouseHoveringRect(b0, b1))
    {
        const int e = s.dead ? 0 : std::max(0, static_cast<int>(std::floor(s.energy)));
        const int m = static_cast<int>(std::floor(s.maxEnergy));
        if (lift > 0.f)
            ImGui::SetTooltip("Energy %d / %d on the best weapon set (calculated)\nOn the set in hand: %d / %d%s",
                              s.dead ? 0 : std::max(0, static_cast<int>(std::floor(best))),
                              static_cast<int>(std::floor(s.maxEnergy + lift)), e, m,
                              s.energy < 0.f ? " (hiding energy)" : "");
        else
            ImGui::SetTooltip("Energy %d / %d (calculated)", e, m);
    }
}
