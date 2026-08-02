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


void ReplayWindow::UpdatePiPIncomingEffects()
{
    if (!m_pipEnabled || m_pipTargetAgent < 0) {
        m_pipIncomingEffects.clear();
        m_pipEffectAgentId = -1;
        return;
    }

    int pipTarget = m_pipTargetAgent;
    float now = m_debugTimeline;

    if (pipTarget != m_pipEffectAgentId || now < m_pipLastEffectScanTime - 0.5f) {
        m_pipIncomingEffects.clear();
        m_pipEffectAgentId = pipTarget;
        m_pipLastEffectScanTime = now;
        return;
    }
    m_pipEffectAgentId = pipTarget;

    std::erase_if(m_pipIncomingEffects, [&](const IncomingEffect& e) {
        return (now - e.spawnTime) >= kEffectLifetime;
    });

    float scanFrom = m_pipLastEffectScanTime;
    float scanTo   = now;
    m_pipLastEffectScanTime = now;
    if (scanTo <= scanFrom) return;

    auto findAgentMaxHp = [&](int agentId, float t) -> uint32_t {
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return 0;
        if (uint32_t m = it->second.solvedMaxHpAtTime(t)) return m;
        const auto& snaps = it->second.snapshots;
        if (snaps.empty()) return 0;
        const AgentSnapshot* s = FindSnapshotAtTime(it->second, t);
        if (s && s->max_hp > 0) return s->max_hp;
        return 0;
    };

    auto pushPipEffect = [&](IncomingEffect eff) {
        constexpr float kMinSep = 35.f;
        constexpr float kTimeWindow = 0.4f;
        float bestOff = 0.f, bestDist = 0.f;
        for (int attempt = 0; attempt < 8; ++attempt) {
            float candidate = (float)(rand() % 181) - 90.f;
            float minDist = 999.f;
            for (const auto& ex : m_pipIncomingEffects) {
                if (std::abs(ex.spawnTime - eff.spawnTime) > kTimeWindow) continue;
                minDist = std::min(minDist, std::abs(candidate - ex.xOffset));
            }
            if (minDist > bestDist) { bestDist = minDist; bestOff = candidate; }
            if (bestDist >= kMinSep) break;
        }
        eff.xOffset = bestOff;
        m_pipIncomingEffects.push_back(std::move(eff));
    };

    // Attribute damage/heal events to skills by matching caster's skill history
    auto findSkillForCombat = [&](int casterId, int targetId, float hitTime) -> int {
        auto cIt = m_replayCtx.agents.find(casterId);
        if (cIt == m_replayCtx.agents.end()) return 0;
        int bestSkill = 0;
        float bestDt = 1e9f;
        for (const auto& su : cIt->second.skillUseHistory) {
            if (su.wasCancelled) continue;
            if (su.targetId != targetId && su.targetId > 0) continue;
            float dt = hitTime - su.endTime;
            if (dt < -0.1f || dt > 3.0f) continue;
            float absDt = std::abs(dt);
            if (absDt < bestDt) { bestDt = absDt; bestSkill = su.skillId; }
        }
        return bestSkill;
    };

    const auto& combatVec = m_replayCtx.stocData.combat;
    std::unordered_set<size_t> consumed;

    for (size_t i = 0; i < combatVec.size(); ++i) {
        const auto& ce = combatVec[i];
        if (!ce.IsDamageOrHeal()) continue;
        if (ce.target_id != pipTarget) continue;
        if (ce.time <= scanFrom || ce.time > scanTo) continue;
        if (consumed.count(i)) continue;

        float totalValue = ce.value;
        consumed.insert(i);

        for (size_t j = i + 1; j < combatVec.size(); ++j) {
            const auto& ce2 = combatVec[j];
            if (ce2.type != "DAMAGE") continue;
            if (ce2.caster_id != ce.caster_id || ce2.target_id != pipTarget) continue;
            if (ce2.time - ce.time > 0.15f) break;
            if (consumed.count(j)) continue;
            totalValue += ce2.value;
            consumed.insert(j);
        }

        bool isHeal = (totalValue > 0.f);
        IncomingEffect eff;
        eff.spawnTime = ce.time;
        eff.skillId = findSkillForCombat(ce.caster_id, pipTarget, ce.time);
        eff.type = isHeal ? IncomingEffectType::Heal : IncomingEffectType::Damage;
        uint32_t mhp = findAgentMaxHp(pipTarget, ce.time);
        int rawVal = (mhp > 0) ? (int)std::round(std::abs(totalValue) * mhp) : 0;
        if (rawVal > 0)
            eff.label = std::format("{}{}", isHeal ? "+" : "-", rawVal);
        else
            eff.label = std::format("{}{:.0f}%", isHeal ? "+" : "-", std::abs(totalValue) * 100.f);
        pushPipEffect(std::move(eff));
    }

    for (const auto& ce : m_replayCtx.stocData.combat) {
        if (ce.time <= scanFrom || ce.time > scanTo) continue;
        if (ce.target_id != pipTarget) continue;
        if (ce.type != "INTERRUPTED") continue;

        IncomingEffect eff;
        eff.spawnTime = ce.time;
        eff.skillId = (int)ce.value;
        eff.type = IncomingEffectType::Interrupt;
        eff.label = "INTERRUPT";
        pushPipEffect(std::move(eff));
    }
}


// ---------------------------------------------------------------------------
// Picture-in-Picture (Split Camera)
// ---------------------------------------------------------------------------

void ReplayWindow::InitPiPResources()
{
    m_pipTexture.Reset();
    m_pipRTV.Reset();
    m_pipDepthTexture.Reset();
    m_pipDSV.Reset();
    m_pipSRV.Reset();
    m_pipResourcesReady = false;

    auto* dev = m_deviceResources->GetD3DDevice();
    DXGI_FORMAT colorFmt = m_deviceResources->GetBackBufferFormat();
    DXGI_FORMAT depthFmt = m_deviceResources->GetDepthBufferFormat();

    // Color texture (non-MSAA, usable as shader resource)
    CD3D11_TEXTURE2D_DESC texDesc(colorFmt, m_pipWidth, m_pipHeight, 1, 1,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (FAILED(dev->CreateTexture2D(&texDesc, nullptr, m_pipTexture.GetAddressOf())))
        return;

    // RTV
    CD3D11_RENDER_TARGET_VIEW_DESC rtvDesc(D3D11_RTV_DIMENSION_TEXTURE2D, colorFmt);
    if (FAILED(dev->CreateRenderTargetView(m_pipTexture.Get(), &rtvDesc, m_pipRTV.GetAddressOf())))
        return;

    // SRV for ImGui display
    CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2D, colorFmt, 0, 1);
    if (FAILED(dev->CreateShaderResourceView(m_pipTexture.Get(), &srvDesc, m_pipSRV.GetAddressOf())))
        return;

    // Depth texture
    CD3D11_TEXTURE2D_DESC depthDesc(depthFmt, m_pipWidth, m_pipHeight, 1, 1,
        D3D11_BIND_DEPTH_STENCIL);
    if (FAILED(dev->CreateTexture2D(&depthDesc, nullptr, m_pipDepthTexture.GetAddressOf())))
        return;

    // DSV
    CD3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc(D3D11_DSV_DIMENSION_TEXTURE2D);
    if (FAILED(dev->CreateDepthStencilView(m_pipDepthTexture.Get(), &dsvDesc, m_pipDSV.GetAddressOf())))
        return;

    m_pipResourcesReady = true;
}


void ReplayWindow::UpdatePiPTarget()
{
    if (!m_agentsClassified) return;

    // Manual override: always follow the pinned agent
    if (m_pipManualAgent >= 0)
    {
        m_pipTargetAgent = m_pipManualAgent;
        return;
    }

    float now = m_debugTimeline;
    const auto& cfg = m_autoCamCfg;

    int bestAgent = -1;
    int bestScore = 0;

    // Re-score current PiP target for hysteresis
    int currentLiveScore = 0;

    auto scoreAgent = [&](int pid, const AgentReplayData& ard) {
        const AgentSnapshot* snap = FindSnapshotAtTime(ard, now);
        if (!snap || snap->is_dead) return;

        // Skip the agent the main camera is already following
        if (m_cameraMode == CameraMode::FollowAgent && pid == m_followedAgentId)
            return;

        int score = 0;

        const AgentSnapshot* futureSnap = FindSnapshotAtTime(ard, now + cfg.lookaheadSec);
        float futureHp = futureSnap ? futureSnap->health_pct : snap->health_pct;

        if (cfg.focusDeath && WillAgentDie(ard, now, cfg.lookaheadSec))
            score = std::max(score, 1000);

        if (cfg.focusLowHp && futureHp < cfg.hpThreshold && futureHp < snap->health_pct)
            score = std::max(score, 700);

        if (cfg.focusRez && IsAgentCastingRez(ard, now))
            score = std::max(score, 500);

        if (cfg.focusIsolated && IsAgentIsolated(ard, now) && IsAgentTakingDamage(pid, now))
            score = std::max(score, 400);

        if (cfg.focusFlagCarry && snap->weapon_type == 0)
        {
            BundleType bt = GetPlayerBundleType(pid, now);
            if (bt == BundleType::Flag || bt == BundleType::Unknown)
                score = std::max(score, 200);
        }

        if (pid == m_pipTargetAgent)
            currentLiveScore = score;

        if (score > bestScore)
        {
            bestScore = score;
            bestAgent = pid;
        }
    };

    for (int pid : m_team1PlayerIds)
    {
        auto it = m_replayCtx.agents.find(pid);
        if (it != m_replayCtx.agents.end())
            scoreAgent(pid, it->second);
    }
    for (int pid : m_team2PlayerIds)
    {
        auto it = m_replayCtx.agents.find(pid);
        if (it != m_replayCtx.agents.end())
            scoreAgent(pid, it->second);
    }

    // Guild lords
    if (cfg.focusLord)
    {
        for (int nid : m_npcIds)
        {
            auto it = m_replayCtx.agents.find(nid);
            if (it == m_replayCtx.agents.end()) continue;
            if (it->second.categoryName != "Guild Lord") continue;
            if (m_cameraMode == CameraMode::FollowAgent && nid == m_followedAgentId) continue;
            const AgentSnapshot* snap = FindSnapshotAtTime(it->second, now);
            if (!snap || snap->is_dead) continue;
            if (IsAgentTakingDamage(nid, now))
            {
                int lordScore = 700;
                if (nid == m_pipTargetAgent)
                    currentLiveScore = lordScore;
                if (lordScore > bestScore)
                {
                    bestScore = lordScore;
                    bestAgent = nid;
                }
            }
        }
    }

    // Flag pickup events
    if (cfg.focusFlag && m_flagTimelineBuilt && m_flagTimeline.valid)
    {
        for (int ti = 0; ti < 2; ++ti)
        {
            for (const auto& fe : m_flagTimeline.teams[ti].events)
            {
                if (fe.newLocation == FlagLocation::Carried &&
                    fe.carrierAgentId >= 0 &&
                    std::abs(fe.time - now) < 2.f)
                {
                    if (m_cameraMode == CameraMode::FollowAgent && fe.carrierAgentId == m_followedAgentId)
                        continue;
                    if (500 > bestScore)
                    {
                        bestScore = 500;
                        bestAgent = fe.carrierAgentId;
                    }
                }
            }
        }
    }

    if (bestAgent < 0) return;

    // Switch decision with dwell and hysteresis (mirrors UpdateAutoCamera)
    bool shouldSwitch = false;
    if (m_pipTargetAgent < 0)
    {
        shouldSwitch = true;
    }
    else if (bestScore >= 900 && bestAgent != m_pipTargetAgent)
    {
        shouldSwitch = true;
    }
    else if (bestAgent != m_pipTargetAgent)
    {
        m_pipDwellTimer += 1.f / 60.f;
        if (m_pipDwellTimer >= 2.0f)
        {
            float refScore = static_cast<float>(std::max(currentLiveScore, 1));
            if (static_cast<float>(bestScore) > refScore * 1.10f)
                shouldSwitch = true;
        }
    }

    if (shouldSwitch && bestAgent != m_pipTargetAgent)
    {
        m_pipTargetAgent = bestAgent;
        m_pipPrevTarget = bestAgent;
        m_pipDwellTimer = 0.f;
    }
}


void ReplayWindow::RenderPiP()
{
    if (!m_pipResourcesReady || m_pipTargetAgent < 0) return;

    auto it = m_replayCtx.agents.find(m_pipTargetAgent);
    if (it == m_replayCtx.agents.end()) return;
    const auto& ard = it->second;
    if (ard.snapshots.empty()) return;

    // Get target world position
    float sx, sy, sz;
    InterpolateAgentPosition(ard, m_debugTimeline, m_replayCtx.interpSettings, sx, sy, sz);
    XMFLOAT3 targetWorld = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);

    // Compute PiP camera position (spherical offset from target)
    float cosP = cosf(m_pipFollowPitch);
    float camX = targetWorld.x + m_pipFollowDist * sinf(m_pipFollowYaw) * cosP;
    float camY = targetWorld.y + m_pipFollowDist * sinf(m_pipFollowPitch);
    float camZ = targetWorld.z + m_pipFollowDist * cosf(m_pipFollowYaw) * cosP;
    XMFLOAT3 pipCamPos(camX, camY, camZ);

    float aspect = static_cast<float>(m_pipWidth) / static_cast<float>(m_pipHeight);
    float fovRad = 50.0f * XM_PI / 180.0f;

    // --- Save main camera state ---
    Camera* cam = m_mapRenderer->GetCamera();
    XMFLOAT3 savedCamPos = cam->GetPosition3f();
    float savedYaw    = cam->GetYaw();
    float savedPitch  = cam->GetPitch();
    float savedFov    = cam->GetFovY();
    float savedAspect = cam->GetAspectRatio();
    float savedNear   = cam->GetNearZ();
    float savedFar    = cam->GetFarZ();

    // --- Set camera to PiP position, looking at target ---
    cam->SetFrustumAsPerspective(fovRad, aspect, savedNear, savedFar, true);
    cam->SetPosition(camX, camY, camZ);
    cam->SetOrientation(-m_pipFollowPitch, m_pipFollowYaw + XM_PI);

    // Update rebuilds view matrix + uploads constant buffer with PiP camera
    m_mapRenderer->Update(0);

    // Cache view*proj for overlay projection in DrawPiPPanel
    XMStoreFloat4x4(&m_pipViewProj, cam->GetView() * cam->GetProj());
    m_pipCamPos = pipCamPos;

    auto* ctx = m_deviceResources->GetD3DDeviceContext();

    // Save main viewport
    UINT numVP = 1;
    D3D11_VIEWPORT savedVP;
    ctx->RSGetViewports(&numVP, &savedVP);

    // Set PiP viewport
    D3D11_VIEWPORT pipVP = { 0.f, 0.f, (float)m_pipWidth, (float)m_pipHeight, 0.f, 1.f };
    ctx->RSSetViewports(1, &pipVP);

    // Clear PiP targets
    float clearColor[4] = { 0.02f, 0.02f, 0.04f, 1.f };
    ctx->ClearRenderTargetView(m_pipRTV.Get(), clearColor);
    ctx->ClearDepthStencilView(m_pipDSV.Get(), D3D11_CLEAR_DEPTH, 0.f, 0);

    // Render terrain/props/water to PiP target
    m_mapRenderer->Render(m_pipRTV.Get(), nullptr, m_pipDSV.Get());

    // Rebind PiP render targets for agent model + cylinder passes
    ID3D11RenderTargetView* pipRTV = m_pipRTV.Get();
    ctx->OMSetRenderTargets(1, &pipRTV, m_pipDSV.Get());

    DrawAgentModels();
    DrawSkinnedAgentModels();
    DrawAgentCylinders();

    // --- Restore main camera ---
    cam->SetPosition(savedCamPos.x, savedCamPos.y, savedCamPos.z);
    cam->SetOrientation(savedPitch, savedYaw);
    cam->SetFrustumAsPerspective(savedFov, savedAspect, savedNear, savedFar, true);
    m_mapRenderer->Update(0);  // rebuilds view matrix + constant buffer for main camera

    // Unbind PiP render targets so main render isn't corrupted
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    // Restore main viewport
    ctx->RSSetViewports(1, &savedVP);
}


void ReplayWindow::DrawPiPPanel()
{
    if (!m_pipEnabled) return;

    if (!m_pipResourcesReady)
        InitPiPResources();
    if (!m_pipResourcesReady) return;

    ImGuiIO& io = ImGui::GetIO();
    float vpW = io.DisplaySize.x;
    float vpH = io.DisplaySize.y;

    ImGui::SetNextWindowSizeConstraints(ImVec2(260.f, 200.f), ImVec2(vpW, vpH));
    if (m_panelLayout.HasSavedSize("split_camera"))
        m_panelLayout.ApplySize("split_camera");
    else
        ImGui::SetNextWindowSize(ImVec2((float)m_pipWidth + 24.f, (float)m_pipHeight + 100.f),
                                 ImGuiCond_FirstUseEver);
    m_panelLayout.ApplyPosition("split_camera");

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.078f, 0.102f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   12.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(10.f, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    if (!ImGui::Begin("Split Camera", &m_pipEnabled))
    {
        m_panelLayout.TrackWindow("split_camera");
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        return;
    }

    m_panelLayout.TrackWindow("split_camera");

    // Clamp to viewport
    {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, 0.f, std::max(0.f, vpW - sz.x));
        float cy = std::clamp(pos.y, 0.f, std::max(0.f, vpH - sz.y));
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));
    }

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Player selector — two-column layout: [icon] truncated_name per team
    if (m_agentsClassified)
    {
        const float iconSz = 16.f;
        const float rowH = iconSz + 2.f;
        const ImU32 kBlueTeam = IM_COL32(0x99, 0xCB, 0xFD, 0xFF);
        const ImU32 kRedTeam  = IM_COL32(0xFF, 0x99, 0x9A, 0xFF);
        const ImU32 kSelHigh  = IM_COL32(255, 220, 100, 60);
        const ImU32 kSelBord  = IM_COL32(255, 220, 100, 180);

        // Auto-detect toggle + focus pills on the same line
        {
            auto& cfg = m_autoCamCfg;

            auto Pill = [&](const char* label, bool active, bool* toggle = nullptr) {
                ImVec4 bg, tx, hov, bdr;
                if (active) {
                    bg  = ImVec4(0.18f, 0.14f, 0.05f, 1.f);
                    tx  = ImVec4(1.f, 0.91f, 0.69f, 1.f);
                    hov = ImVec4(0.23f, 0.19f, 0.08f, 1.f);
                    bdr = ImVec4(1.f, 0.84f, 0.39f, 0.85f);
                } else {
                    bg  = ImVec4(1.f, 1.f, 1.f, 0.05f);
                    tx  = ImVec4(0.60f, 0.64f, 0.69f, 1.f);
                    hov = ImVec4(1.f, 1.f, 1.f, 0.12f);
                    bdr = ImVec4(1.f, 1.f, 1.f, 0.08f);
                }
                ImGui::PushStyleColor(ImGuiCol_Button, bg);
                ImGui::PushStyleColor(ImGuiCol_Text, tx);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                    ImVec4(bg.x * 0.85f, bg.y * 0.85f, bg.z * 0.85f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Border, bdr);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
                bool clicked = ImGui::Button(label);
                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(5);
                if (clicked && toggle) *toggle = !*toggle;
                return clicked;
            };

            bool autoSel = (m_pipManualAgent < 0);
            if (Pill("Auto-detect", autoSel)) {
                if (autoSel)
                    m_pipManualAgent = (m_pipTargetAgent >= 0) ? m_pipTargetAgent : 0;
                else
                    m_pipManualAgent = -1;
            }

            if (m_pipManualAgent < 0)
            {
                ImGui::SameLine();
                Pill("Death", cfg.focusDeath, &cfg.focusDeath);           ImGui::SameLine();
                Pill("Low HP", cfg.focusLowHp, &cfg.focusLowHp);         ImGui::SameLine();
                Pill("Guild Lord", cfg.focusLord, &cfg.focusLord);        ImGui::SameLine();
                Pill("Flag", cfg.focusFlag, &cfg.focusFlag);              ImGui::SameLine();
                Pill("Resurrection", cfg.focusRez, &cfg.focusRez);        ImGui::SameLine();
                Pill("Isolated", cfg.focusIsolated, &cfg.focusIsolated);  ImGui::SameLine();
                Pill("Flag Carrier", cfg.focusFlagCarry, &cfg.focusFlagCarry);
            }
        }

        ImGui::Spacing();

        // Override Selectable hover/active colors to match our warm UI palette
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.18f, 0.14f, 0.05f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.20f, 0.16f, 0.07f, 0.70f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,   ImVec4(0.25f, 0.20f, 0.08f, 0.85f));

        float availW = ImGui::GetContentRegionAvail().x;
        float halfW = (availW - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        if (ImGui::BeginTable("##pip_teams", 2, ImGuiTableFlags_NoSavedSettings))
        {
            ImGui::TableSetupColumn("t1", ImGuiTableColumnFlags_WidthFixed, halfW);
            ImGui::TableSetupColumn("t2", ImGuiTableColumnFlags_WidthFixed, halfW);

            auto DrawTeamColumn = [&](const std::vector<int>& ids, ImU32 teamCol,
                                      const char* teamLabel)
            {
                ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "%s", teamLabel);
                float colW = ImGui::GetContentRegionAvail().x;

                for (int id : ids)
                {
                    auto agIt = m_replayCtx.agents.find(id);
                    if (agIt == m_replayCtx.agents.end()) continue;
                    const auto& ard = agIt->second;
                    bool selected = (m_pipManualAgent == id);

                    ImVec2 rowMin = ImGui::GetCursorScreenPos();

                    ImGui::PushID(id);
                    if (ImGui::Selectable("##s", selected, 0, ImVec2(colW, rowH)))
                        m_pipManualAgent = id;
                    ImGui::PopID();

                    if (selected)
                    {
                        dl->AddRectFilled(rowMin, ImVec2(rowMin.x + colW, rowMin.y + rowH),
                                          kSelHigh, 3.f);
                        dl->AddRect(rowMin, ImVec2(rowMin.x + colW, rowMin.y + rowH),
                                    kSelBord, 3.f);
                    }

                    float iconY = rowMin.y + (rowH - iconSz) * 0.5f;
                    ImTextureID profTex = (ard.primaryProf > 0)
                        ? LoadProfIcon(dev, ard.primaryProf) : nullptr;
                    if (profTex)
                        dl->AddImage(profTex, ImVec2(rowMin.x + 1, iconY),
                                     ImVec2(rowMin.x + 1 + iconSz, iconY + iconSz));
                    else
                        dl->AddRectFilled(ImVec2(rowMin.x + 1, iconY),
                                          ImVec2(rowMin.x + 1 + iconSz, iconY + iconSz),
                                          IM_COL32(40, 40, 40, 180), 2.f);

                    std::string name = ard.playerName.empty()
                        ? ard.categoryName : ard.playerName;
                    ImFont* font = ImGui::GetFont();
                    float textY = rowMin.y + (rowH - font->FontSize) * 0.5f;
                    float textX = rowMin.x + iconSz + 4.f;

                    dl->PushClipRect(ImVec2(textX, rowMin.y),
                                     ImVec2(rowMin.x + colW, rowMin.y + rowH), true);
                    dl->AddText(font, font->FontSize, ImVec2(textX, textY),
                                teamCol, name.c_str());
                    dl->PopClipRect();

                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", name.c_str());
                }
            };

            ImGui::TableNextColumn();
            DrawTeamColumn(m_team1PlayerIds, kRedTeam, "Team 1");
            ImGui::TableNextColumn();
            DrawTeamColumn(m_team2PlayerIds, kBlueTeam, "Team 2");

            ImGui::EndTable();
        }

        ImGui::PopStyleColor(3);
        ImGui::Spacing();
    }

    if (m_pipTargetAgent >= 0)
    {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float imgW = std::max(64.f, avail.x);
        float imgH = std::max(64.f, avail.y);
        {
            int newW = (int)imgW;
            int newH = (int)imgH;
            if (newW != m_pipWidth || newH != m_pipHeight)
            {
                m_pipWidth = newW;
                m_pipHeight = newH;
                InitPiPResources();
            }
        }

        if (imgW > 0.f && imgH > 0.f)
        {
            ImVec2 imgPos = ImGui::GetCursorScreenPos();

            ImGui::InvisibleButton("##pip_interact", ImVec2(imgW, imgH),
                ImGuiButtonFlags_MouseButtonRight);
            m_pipHovered = ImGui::IsItemHovered();
            dl->AddImage((ImTextureID)m_pipSRV.Get(), imgPos,
                         ImVec2(imgPos.x + imgW, imgPos.y + imgH));

            if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right))
            {
                ImVec2 md = ImGui::GetIO().MouseDelta;
                m_pipFollowYaw   += md.x * 0.01f;
                m_pipFollowPitch  = std::clamp(m_pipFollowPitch + md.y * 0.01f, 0.05f, 1.55f);
            }
            if (ImGui::IsItemHovered() && io.MouseWheel != 0.f)
                m_pipFollowDist = std::clamp(m_pipFollowDist - io.MouseWheel * 40.f, 100.f, 2000.f);

            // Update PiP incoming effects so they're fresh for this frame
            UpdatePiPIncomingEffects();

            // Overlay agent labels, cast bars, and laser lines
            if (m_agentsClassified && m_skillUseTimelineBuilt)
            {
                XMMATRIX vp = XMLoadFloat4x4(&m_pipViewProj);
                ImFont* font = ImGui::GetFont();
                float fontSize = font->FontSize;
                const MapTransform& mt = m_replayCtx.mapTransform;
                const InterpolationSettings& is = m_replayCtx.interpSettings;
                float now = m_debugTimeline;

                auto ProjectToImage = [&](const XMFLOAT3& pos, float& outX, float& outY) -> bool
                {
                    XMVECTOR wp = XMVectorSet(pos.x, pos.y + 40.f, pos.z, 1.f);
                    XMVECTOR cl = XMVector4Transform(wp, vp);
                    float cw = XMVectorGetW(cl);
                    if (cw < 0.001f) return false;
                    float nx = XMVectorGetX(cl) / cw;
                    float ny = XMVectorGetY(cl) / cw;
                    outX = imgPos.x + (nx * 0.5f + 0.5f) * imgW;
                    outY = imgPos.y + (1.f - ny) * 0.5f * imgH;
                    return true;
                };

                auto InBounds = [&](float x, float y) {
                    return x >= imgPos.x && x <= imgPos.x + imgW
                        && y >= imgPos.y && y <= imgPos.y + imgH;
                };

                for (auto& [agentId, ard] : m_replayCtx.agents)
                {
                    if (ard.snapshots.empty()) continue;
                    if (ard.type != AgentType::Player && ard.type != AgentType::NPC
                        && ard.type != AgentType::Gadget && ard.type != AgentType::Flag)
                        continue;
                    if (!m_show3DLabels) continue;
                    if (m_hiddenNameAgents.count(agentId)) continue;

                    const AgentSnapshot* snap = FindSnapshotAtTime(ard, now);
                    if (!snap || snap->is_dead) continue;

                    float sx, sy, sz;
                    InterpolateAgentPosition(ard, now, is, sx, sy, sz);
                    XMFLOAT3 pos = ApplyMapTransformToPos(sx, sy, sz, mt);

                    float scrX, scrY;
                    if (!ProjectToImage(pos, scrX, scrY)) continue;
                    if (!InBounds(scrX, scrY)) continue;

                    std::string label = GetAgentLabel(ard);
                    ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label.c_str());
                    float lx = scrX - textSz.x * 0.5f;
                    float ly = scrY;
                    float pad = 2.f;

                    bool isSpecialLabel = (ard.categoryName == "Repair Kit" ||
                                           ard.categoryName == "Tower Flag Stand" ||
                                           ard.categoryName == "Obelisk Flag Stand" ||
                                           ard.categoryName == "Resurrection Shrine" ||
                                           ard.categoryName == "Dwarven Resurrection Shrine" ||
                                           ard.categoryName == "Southern Health Shrine" ||
                                           ard.categoryName == "Gate lever");
                    if (isSpecialLabel)
                    {
                        dl->AddRectFilled(ImVec2(lx - pad, ly - pad),
                                          ImVec2(lx + textSz.x + pad, ly + textSz.y + pad),
                                          IM_COL32(0, 0, 0, 13), 3.f);
                        dl->AddText(font, fontSize, ImVec2(lx, ly),
                                    IM_COL32(245, 228, 180, 255), label.c_str());
                    }
                    else
                    {
                        dl->AddRectFilled(ImVec2(lx - pad, ly - pad),
                                          ImVec2(lx + textSz.x + pad, ly + textSz.y + pad),
                                          IM_COL32(0, 0, 0, 25), 3.f);
                        ImU32 labelCol;
                        if (ard.teamId == 1)      labelCol = IM_COL32(0xFF, 0x99, 0x9A, 0xE6);
                        else if (ard.teamId == 2) labelCol = IM_COL32(0x99, 0xCB, 0xFD, 0xE6);
                        else                      labelCol = IM_COL32(255, 255, 255, 230);
                        dl->AddText(font, fontSize, ImVec2(lx + 1.f, ly + 1.f),
                                    IM_COL32(0, 0, 0, 200), label.c_str());
                        dl->AddText(font, fontSize, ImVec2(lx, ly), labelCol, label.c_str());
                    }

                    if (ard.type == AgentType::Player)
                    {
                        auto sv = ard.skillVisualAtTime(now);
                        if (sv.skillId > 0 && sv.alpha > 0.01f)
                        {
                            int alpha = (int)(255 * sv.alpha);
                            float iconSz = 18.f;
                            float gap = 2.f;

                            float ikX = scrX - iconSz * 0.5f;
                            float ikY = ly - iconSz - gap;
                            ImTextureID skillTex = LoadSkillIcon(
                                const_cast<ReplayWindow*>(this),
                                m_deviceResources->GetD3DDevice(),
                                sv.skillId, m_skillIconIndex, m_skillIconCache);
                            if (skillTex)
                            {
                                dl->AddImage(skillTex, ImVec2(ikX, ikY),
                                             ImVec2(ikX + iconSz, ikY + iconSz),
                                             ImVec2(0, 0), ImVec2(1, 1),
                                             IM_COL32(255, 255, 255, alpha));
                            }

                            if (sv.isCasting || sv.cancelled || sv.interrupted)
                            {
                                float cbW = iconSz + 8.f;
                                float cbH = 5.f;
                                float cbX = scrX - cbW * 0.5f;
                                float cbY = ikY + iconSz + 1.f;
                                ImU32 barBg = IM_COL32(0, 0, 0, (int)(180 * sv.alpha));
                                ImU32 barFg;
                                if (sv.interrupted)      barFg = IM_COL32(180, 80, 220, alpha);
                                else if (sv.cancelled)   barFg = IM_COL32(230, 180, 40, alpha);
                                else                     barFg = IM_COL32(60, 210, 100, alpha);
                                dl->AddRectFilled(ImVec2(cbX, cbY), ImVec2(cbX + cbW, cbY + cbH), barBg, 1.f);
                                dl->AddRectFilled(ImVec2(cbX, cbY), ImVec2(cbX + cbW * sv.progress, cbY + cbH), barFg, 1.f);
                            }
                        }
                    }
                }

                for (auto& [agentId, ard] : m_replayCtx.agents)
                {
                    if (ard.type != AgentType::Player) continue;
                    if (ard.snapshots.empty()) continue;

                    auto laser = ard.skillLaserAtTime(now);
                    if (laser.targetId < 0 || laser.alpha < 0.01f) continue;

                    auto tgtIt = m_replayCtx.agents.find(laser.targetId);
                    if (tgtIt == m_replayCtx.agents.end()) continue;
                    const auto& tgtArd = tgtIt->second;
                    if (tgtArd.snapshots.empty()) continue;

                    float csx, csy, csz;
                    InterpolateAgentPosition(ard, now, is, csx, csy, csz);
                    XMFLOAT3 cpos = ApplyMapTransformToPos(csx, csy, csz, mt);
                    float cscrX, cscrY;
                    if (!ProjectToImage(cpos, cscrX, cscrY)) continue;

                    float tsx, tsy, tsz;
                    InterpolateAgentPosition(tgtArd, now, is, tsx, tsy, tsz);
                    XMFLOAT3 tpos = ApplyMapTransformToPos(tsx, tsy, tsz, mt);
                    float tscrX, tscrY;
                    if (!ProjectToImage(tpos, tscrX, tscrY)) continue;

                    if (InBounds(cscrX, cscrY) || InBounds(tscrX, tscrY))
                    {
                        ImU32 col = (ard.teamId == tgtArd.teamId)
                            ? IM_COL32(60, 255, 120, (int)(200 * laser.alpha))
                            : IM_COL32(255, 60, 60, (int)(200 * laser.alpha));
                        dl->AddLine(ImVec2(cscrX, cscrY), ImVec2(tscrX, tscrY), col, 2.f);
                    }
                }

                // --- PiP flag overlay (mirrors DrawFlags) ---
                if (m_flagTimelineBuilt && m_flagTimeline.valid)
                {
                    ImTextureID texBlue = LoadFlagIcon(dev, "Blue_flag_waving.svg.png");
                    ImTextureID texRed  = LoadFlagIcon(dev, "Red_flag_waving.svg.png");
                    const float flagIconSz = std::clamp(imgH * 0.035f, 14.f, 24.f);

                    auto DrawPipFlag = [&](float worldX, float worldY, float worldZ,
                                           ImTextureID tex, int teamIdx, const char* label)
                    {
                        XMFLOAT3 fpos = ApplyMapTransformToPos(worldX, worldY, worldZ, mt);
                        float fScrX, fScrY;
                        if (!ProjectToImage(fpos, fScrX, fScrY)) return;
                        if (!InBounds(fScrX, fScrY)) return;

                        constexpr float kDotR = 4.f;
                        ImU32 dotCol = (teamIdx == 0) ? IM_COL32(255, 100, 90, 200)
                                                      : IM_COL32(100, 160, 255, 200);
                        dl->AddCircleFilled(ImVec2(fScrX, fScrY), kDotR, dotCol);
                        dl->AddCircle(ImVec2(fScrX, fScrY), kDotR, IM_COL32(0, 0, 0, 180), 0, 1.5f);

                        if (tex)
                        {
                            float oY = flagIconSz * 0.8f;
                            ImVec2 iTL(fScrX - flagIconSz * 0.5f, fScrY - oY - flagIconSz);
                            ImVec2 iBR(iTL.x + flagIconSz, iTL.y + flagIconSz);
                            dl->AddImage(tex, iTL, iBR);
                        }

                        if (label)
                        {
                            ImVec2 tSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label);
                            float ltx = fScrX - tSz.x * 0.5f;
                            float oY = flagIconSz * 0.8f;
                            float lty = fScrY - oY - flagIconSz - fontSize - 2.f;
                            dl->AddText(ImVec2(ltx + 1.f, lty + 1.f), IM_COL32(0, 0, 0, 200), label);
                            dl->AddText(ImVec2(ltx, lty), IM_COL32(255, 255, 255, 230), label);
                        }
                    };

                    StandOwner pipStandOwner = m_flagTimeline.stand.ownerAtTime(now);
                    if (pipStandOwner != StandOwner::Neutral)
                    {
                        int sti = (pipStandOwner == StandOwner::Red) ? 0 : 1;
                        ImTextureID stTex = (sti == 0) ? texRed : texBlue;
                        float stx = m_flagTimeline.stand.standX;
                        float sty = m_flagTimeline.stand.standY;
                        float stz = m_flagTimeline.stand.standZ;

                        XMFLOAT3 stPos = ApplyMapTransformToPos(stx, sty, stz, mt);
                        float stScrX, stScrY;
                        if (ProjectToImage(stPos, stScrX, stScrY) && InBounds(stScrX, stScrY) && stTex)
                        {
                            float oY = flagIconSz * 0.8f;
                            ImVec2 iTL(stScrX - flagIconSz * 0.5f, stScrY - oY - flagIconSz);
                            ImVec2 iBR(iTL.x + flagIconSz, iTL.y + flagIconSz);

                            ImVec2 ctr((iTL.x + iBR.x) * 0.5f, (iTL.y + iBR.y) * 0.5f);
                            float glowR = flagIconSz * 0.75f;
                            float pulse = 0.6f + 0.4f * sinf((float)ImGui::GetTime() * 1.8f);
                            ImU32 glowCol = (sti == 0)
                                ? IM_COL32(255, 60, 50,  (int)(50 * pulse))
                                : IM_COL32(60, 130, 255, (int)(50 * pulse));
                            dl->AddCircleFilled(ctr, glowR, glowCol, 32);
                            dl->AddImage(stTex, iTL, iBR);
                        }
                    }

                    for (int ti = 0; ti < 2; ti++)
                    {
                        auto& ft = m_flagTimeline.teams[ti];
                        if (ft.events.empty()) continue;

                        ImTextureID tex = (ti == 0) ? texRed : texBlue;
                        FlagLocation loc = ft.locationAtTime(now);
                        if (loc == FlagLocation::Stand) continue;

                        float fwx = 0, fwy = 0, fwz = 0;
                        const char* flabel = nullptr;

                        if (loc == FlagLocation::Carried)
                        {
                            int carrierId = ft.carrierAtTime(now);
                            if (carrierId >= 0)
                            {
                                auto cIt = m_replayCtx.agents.find(carrierId);
                                if (cIt != m_replayCtx.agents.end() && !cIt->second.snapshots.empty())
                                    InterpolateAgentPosition(cIt->second, now, is, fwx, fwy, fwz);
                                else
                                    ft.positionAtTime(now, fwx, fwy, fwz);
                            }
                            else
                            {
                                ft.positionAtTime(now, fwx, fwy, fwz);
                            }
                            flabel = "Flag (Carried)";
                        }
                        else
                        {
                            ft.positionAtTime(now, fwx, fwy, fwz);
                            if (loc == FlagLocation::Ground)
                                flabel = "Flag (Dropped)";
                        }

                        DrawPipFlag(fwx, fwy, fwz, tex, ti, flabel);
                    }
                }

                // --- PiP incoming damage/heal floating numbers ---
                if (!m_pipIncomingEffects.empty() && m_pipTargetAgent >= 0)
                {
                    auto tgtIt = m_replayCtx.agents.find(m_pipTargetAgent);
                    if (tgtIt != m_replayCtx.agents.end() && !tgtIt->second.snapshots.empty())
                    {
                        float tsx, tsy, tsz;
                        InterpolateAgentPosition(tgtIt->second, now, is, tsx, tsy, tsz);
                        XMFLOAT3 tgtPos = ApplyMapTransformToPos(tsx, tsy, tsz, mt);

                        float anchorX, anchorY;
                        if (ProjectToImage(tgtPos, anchorX, anchorY))
                        {
                            constexpr float kFloatDist = 80.f;
                            constexpr float kStartOffsetY = 30.f;
                            anchorY -= kStartOffsetY;

                            EnsureBitmapFontsLoaded();
                            const float glyphH = std::clamp(imgH * 0.016f, 9.f, 16.f);

                            static std::vector<size_t> pipSorted;
                            pipSorted.clear();
                            for (size_t i = 0; i < m_pipIncomingEffects.size(); ++i) {
                                float age = now - m_pipIncomingEffects[i].spawnTime;
                                if (age >= 0.f && age < kEffectLifetime)
                                    pipSorted.push_back(i);
                            }
                            std::sort(pipSorted.begin(), pipSorted.end(), [&](size_t a, size_t b) {
                                return m_pipIncomingEffects[a].spawnTime < m_pipIncomingEffects[b].spawnTime;
                            });

                            constexpr float ICON_SZ = 20.f;
                            constexpr float GAP = 3.f;
                            EnsureSkillIconIndex();

                            const size_t total = pipSorted.size();
                            for (size_t si = 0; si < total; ++si)
                            {
                                const auto& e = m_pipIncomingEffects[pipSorted[si]];
                                if (e.label.empty() && e.skillId <= 0) continue;
                                float age = now - e.spawnTime;
                                float t = age / kEffectLifetime;
                                float opacity = (t < 0.65f) ? 1.f : 1.f - ((t - 0.65f) / 0.35f);
                                opacity = std::clamp(opacity, 0.f, 1.f);

                                float depthRank = (total > 1) ? (float)si / (float)(total - 1) : 1.f;
                                opacity *= 0.55f + 0.45f * depthRank;

                                uint8_t alpha = (uint8_t)(opacity * 255.f);
                                float fy = anchorY - t * kFloatDist;
                                float fx = anchorX + e.xOffset * (imgW / 800.f);

                                bool hasIcon = (e.skillId > 0);
                                float iconW = ICON_SZ;

                                const BitmapFont* bmFont = nullptr;
                                if (e.type == IncomingEffectType::Heal && m_healBitmapFont.srv.Get())
                                    bmFont = &m_healBitmapFont;
                                else if ((e.type == IncomingEffectType::Damage || e.type == IncomingEffectType::BasicAttack) && m_damageBitmapFont.srv.Get())
                                    bmFont = &m_damageBitmapFont;

                                float labelW = 0.f;
                                if (bmFont && !e.label.empty())
                                    labelW = bmFont->MeasureString(e.label.c_str(), glyphH);

                                if (bmFont) {
                                    float totalW = 0.f;
                                    if (hasIcon) totalW += iconW;
                                    if (hasIcon && labelW > 0.f) totalW += GAP;
                                    if (labelW > 0.f) totalW += labelW;

                                    float startX = fx - totalW * 0.5f;
                                    float curX = startX;

                                    if (hasIcon) {
                                        ImTextureID tex = LoadSkillIcon(
                                            const_cast<ReplayWindow*>(this), dev,
                                            e.skillId, m_skillIconIndex, m_skillIconCache);
                                        if (tex)
                                            dl->AddImage(tex,
                                                ImVec2(curX, fy - iconW * 0.5f),
                                                ImVec2(curX + iconW, fy + iconW * 0.5f),
                                                ImVec2(0, 0), ImVec2(1, 1),
                                                IM_COL32(255, 255, 255, alpha));
                                        curX += iconW + GAP;
                                    }

                                    if (!e.label.empty()) {
                                        float labelCX = curX + labelW * 0.5f;
                                        bmFont->DrawString(dl, e.label.c_str(), labelCX, fy, glyphH, alpha);
                                    }
                                } else {
                                    ImU32 col;
                                    if (e.type == IncomingEffectType::Heal)
                                        col = IM_COL32(100, 255, 100, alpha);
                                    else if (e.type == IncomingEffectType::Interrupt)
                                        col = IM_COL32(224, 112, 48, alpha);
                                    else
                                        col = IM_COL32(255, 80, 80, alpha);

                                    float labelFs = fontSize;
                                    ImVec2 tsz = font->CalcTextSizeA(labelFs, FLT_MAX, 0.f, e.label.c_str());
                                    float totalW = 0.f;
                                    if (hasIcon) totalW += iconW;
                                    if (hasIcon && tsz.x > 0) totalW += GAP;
                                    totalW += tsz.x;

                                    float startX = fx - totalW * 0.5f;
                                    float curX = startX;

                                    if (hasIcon) {
                                        ImTextureID tex = LoadSkillIcon(
                                            const_cast<ReplayWindow*>(this), dev,
                                            e.skillId, m_skillIconIndex, m_skillIconCache);
                                        if (tex)
                                            dl->AddImage(tex,
                                                ImVec2(curX, fy - iconW * 0.5f),
                                                ImVec2(curX + iconW, fy + iconW * 0.5f),
                                                ImVec2(0, 0), ImVec2(1, 1),
                                                IM_COL32(255, 255, 255, alpha));
                                        curX += iconW + GAP;
                                    }

                                    if (!e.label.empty()) {
                                        dl->AddText(font, labelFs,
                                            ImVec2(curX + 1.f, fy - tsz.y * 0.5f + 1.f),
                                            IM_COL32(0, 0, 0, alpha), e.label.c_str());
                                        dl->AddText(font, labelFs,
                                            ImVec2(curX, fy - tsz.y * 0.5f), col, e.label.c_str());
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    else
    {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.f), "No split detected");
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}
