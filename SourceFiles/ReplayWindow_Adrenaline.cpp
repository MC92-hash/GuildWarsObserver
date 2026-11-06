#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include <algorithm>
#include <cctype>
#include <fstream>

// ---------------------------------------------------------------------------
// Adrenaline on the skill bars. No recording holds a player's adrenaline, so it is calculated
// forward from the match start by AdrenalineModel (private): hits, damage taken, uses, deaths, the
// 25 s out-of-combat rule, the cooldown model's recharges and disables, and the skill texts that
// move it. This file wires the window's models into it and draws the result the way Gw.exe does
// (gwobserver-private/docs/ADRENALINE_MODEL_PLAN.md, section 5).
// ---------------------------------------------------------------------------

void ReplayWindow::BuildAdrenalineTracks() const
{
    if (!m_skillDisablesBuilt || (!m_skillDisablesUsedAttributes && m_attributesDeduced))
        BuildSkillDisables();

    AdrenalineModel::Inputs in;
    in.skills    = &m_skillView;
    in.stoc      = &m_replayCtx.stocData;
    in.equipment = &m_replayCtx.stocData.equipment;
    in.effects   = EffectTimelineTable();
    in.matchEnd  = m_replayCtx.maxReplayTime;
    in.attributeRank = [this](int agentId, int attribute) -> int {
        auto it = m_attrProfiles.find(agentId);
        if (it == m_attrProfiles.end()) return -1;
        const AttributeModel::AttributeRange* a = AttributeModel::SolvedRange(&it->second, attribute);
        return a ? a->best : -1;
    };
    in.attributeRange = [this](int agentId, int attribute, int& lo, int& hi) -> bool {
        auto it = m_attrProfiles.find(agentId);
        if (it == m_attrProfiles.end()) return false;
        if (const AttributeModel::AttributeRange* s = AttributeModel::SolvedRange(&it->second, attribute)) {
            lo = s->lo; hi = s->hi;
            return true;
        }
        auto a = it->second.attributes.find(attribute);
        if (a == it->second.attributes.end()) return false;
        lo = a->second.lo; hi = a->second.hi;
        return true;
    };
    // The slots the HUD shows, in its order, PvP-resolved.
    in.skillBar = [this](int agentId) { return SkillBarDisplayOrder(agentId); };
    // The shared cooldown model: a recharging, disabled or spent slot loses all and builds nothing.
    in.unavailableFor = [this](int agentId, int skillId, float t) -> float {
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return 0.f;
        const std::vector<SkillCooldownState> cds = ComputeSkillCooldowns(it->second, { skillId }, t);
        if (cds.empty() || cds[0].state == SkillCooldownState::State::Ready) return 0.f;
        if (cds[0].state == SkillCooldownState::State::Spent) return 1e6f;
        return cds[0].remaining;
    };
    in.disableStarts = [this](int agentId) {
        std::vector<float> out;
        if (auto it = m_skillDisables.find(agentId); it != m_skillDisables.end())
            for (const SkillDisable& d : it->second) out.push_back(d.start);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    };

    m_adrenalineTracks = AdrenalineModel::Build(m_replayCtx.agents, in);
    m_adrenalineBuilt = true;
    m_adrenalineUsedAttributes = m_attributesDeduced;

    // Developer dump, as the energy model writes its own: %TEMP%\gwo_adrenaline_<match>.txt whenever
    // GWO_ADRENALINE_DEBUG is set (with every step of every slot).
    char flag[16] = {};
    char tempDir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("GWO_ADRENALINE_DEBUG", flag, (DWORD)sizeof(flag)) != 0 &&
        GetTempPathA(MAX_PATH, tempDir) != 0)
    {
        std::string folder = m_replayCtx.matchFolderPath.filename().string();
        if (folder.empty()) folder = "replay";
        const std::string name = folder;
        for (char& c : folder)
            if (!std::isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '_';
        AdrenalineModel::WriteDebugDump(std::string(tempDir) + "gwo_adrenaline_" + folder + ".txt", name,
                                        m_replayCtx.agents, m_adrenalineTracks, m_skillView, true);
    }
}

const AdrenalineModel::Track* ReplayWindow::AdrenalineTrackFor(int agentId) const
{
    if (!m_agentsClassified || !m_replayCtx.stocLoaded || !m_skillUseTimelineBuilt) return nullptr;
    if (!m_adrenalineBuilt || (!m_adrenalineUsedAttributes && m_attributesDeduced))
        BuildAdrenalineTracks();
    auto it = m_adrenalineTracks.find(agentId);
    return it == m_adrenalineTracks.end() ? nullptr : &it->second;
}

// The headless export (--export-adrenaline): per player the adrenal skills and costs, uses, paid
// uses, the rules that fired and every contradiction with its context. Built from scratch on the
// solved attributes so a run is deterministic.
bool ReplayWindow::ExportAdrenaline(const std::filesystem::path& outPath)
{
    if (!m_attributesDeduced) return false;
    BuildAdrenalineTracks();
    return AdrenalineModel::WriteDebugDump(outPath.string(), m_replayCtx.matchFolderPath.filename().string(),
                                           m_replayCtx.agents, m_adrenalineTracks, m_skillView, false);
}

// ---------------------------------------------------------------------------
// The client's adrenaline effect (GmCtlSkImageEffect, decoded 2026-10-31): over a charging slot a
// MULTIPLY blend (dst = dst x texel) of texture 143772, 4 x 4 cells of 64 px, 16 frames at
// 0.0625 s each. The bottom `level` of the icon shows the TOP `level` of the current cell (the
// flames' tips at the fill line, so they rise with the charge); above the line, the cell's top
// texel row stretched (grey ~0.49: the icon dimmed to about half). Full: nothing. From 21 s after
// the last gain the flames blink, hidden from 25 s (the icon fully dimmed). Hidden under the
// "in use" sparkle.
// ---------------------------------------------------------------------------

ID3D11BlendState* ReplayWindow::AdrenalineMultiplyBlend()
{
    if (m_adrenalineMultiplyBlend) return m_adrenalineMultiplyBlend.Get();
    ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;
    if (!dev) return nullptr;
    D3D11_BLEND_DESC desc = {};
    desc.RenderTarget[0].BlendEnable           = TRUE;
    desc.RenderTarget[0].SrcBlend              = D3D11_BLEND_DEST_COLOR;
    desc.RenderTarget[0].DestBlend             = D3D11_BLEND_ZERO;
    desc.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ZERO;
    desc.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ONE;
    desc.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&desc, m_adrenalineMultiplyBlend.GetAddressOf());
    return m_adrenalineMultiplyBlend.Get();
}

void ReplayWindow::DrawAdrenalineOverlay(ImDrawList* dl, int agentId, int skillId, ImVec2 tl, ImVec2 br, bool inUse)
{
    if (inUse || !dl) return;
    const AdrenalineModel::Track* track = AdrenalineTrackFor(agentId);
    if (!track) return;
    const int sid = m_skillView.ResolvePvpSkillId(skillId);
    int points = 0, cost = 0;
    float since = -1.f;
    const float t = m_debugTimeline;
    if (!track->SlotAt(sid, t, points, cost, since) || cost <= 0 || points >= cost) return;

    float level = std::clamp(static_cast<float>(points) / static_cast<float>(cost), 0.f, 1.f);
    // The blink: from 21 s after the last gain the flames toggle, the first toggle hiding them, with
    // period clamp(0.125 x remaining, 1/16, 1/3) s; from 25 s they stay hidden.
    if (since >= 25.f) level = 0.f;
    else if (since >= 21.f)
    {
        bool shown = true;
        for (float s = 21.f; s <= since; s += std::clamp(0.125f * (25.f - s), 1.f / 16.f, 1.f / 3.f))
            shown = !shown;
        if (!shown) level = 0.f;
    }

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImTextureID tex = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_adrenaline.png");
    ID3D11BlendState* blend = AdrenalineMultiplyBlend();
    if (!tex || !blend) return;

    // Each slot starts on its own frame and phase, stable per player and skill; replay time drives
    // it, so a paused replay freezes the flames.
    uint32_t h = static_cast<uint32_t>(agentId) * 2654435761u ^ static_cast<uint32_t>(sid) * 40503u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    const float phase = static_cast<float>(h & 0xFFFF) / 65536.f * 0.0625f;
    const int frame = (static_cast<int>(std::floor((std::max(0.f, t) + phase) / 0.0625f)) + static_cast<int>(h >> 28)) & 15;
    const float cx = static_cast<float>(frame % 4) * 64.f, cy = static_cast<float>(frame / 4) * 64.f;
    const float u0 = (cx + 0.5f) / 256.f, u1 = (cx + 63.5f) / 256.f, vTop = (cy + 0.5f) / 256.f;

    const float fillH = std::round((br.y - tl.y) * level);
    const float fillY = br.y - fillH;
    dl->AddCallback(&ReplayWindow::SetPortraitCompositeBlend, blend);
    if (fillY > tl.y)
        dl->AddImage(tex, tl, ImVec2(br.x, fillY), ImVec2(u0, vTop), ImVec2(u1, vTop));
    if (fillH > 0.f)
        dl->AddImage(tex, ImVec2(tl.x, fillY), br, ImVec2(u0, vTop), ImVec2(u1, vTop + level * 63.f / 256.f));
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// One line appended to the slot's skill tooltip: "Adrenaline 3.2 / 4 strikes (80 / 100)".
void ReplayWindow::DrawAdrenalineTooltipLine(int agentId, int skillId)
{
    const AdrenalineModel::Track* track = AdrenalineTrackFor(agentId);
    if (!track) return;
    int points = 0, cost = 0;
    float since = -1.f;
    if (!track->SlotAt(m_skillView.ResolvePvpSkillId(skillId), m_debugTimeline, points, cost, since) || cost <= 0) return;
    ImGui::BeginTooltip();
    ImGui::TextColored(ImVec4(1.f, 0.78f, 0.45f, 1.f), "Adrenaline %.1f / %.1f strikes (%d / %d, calculated)",
                       points / 25.f, cost / 25.f, points, cost);
    ImGui::EndTooltip();
}
