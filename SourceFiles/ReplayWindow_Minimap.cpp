#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "GuiGlobalConstants.h"
#include "DXMathHelpers.h"

// ---------------------------------------------------------------------------
// Extracted from ReplayWindow.cpp (partial-class split). These remain
// ReplayWindow:: member functions; only their definitions live here.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Minimap
// ---------------------------------------------------------------------------

void ReplayWindow::InitMinimapResources()
{
    m_minimapTexture.Reset();
    m_minimapRTV.Reset();
    m_minimapDepthTexture.Reset();
    m_minimapDSV.Reset();
    m_minimapSRV.Reset();
    m_minimapResourcesReady = false;

    auto* dev = m_deviceResources->GetD3DDevice();
    DXGI_FORMAT colorFmt = m_deviceResources->GetBackBufferFormat();
    DXGI_FORMAT depthFmt = m_deviceResources->GetDepthBufferFormat();

    // Color texture (non-MSAA, usable as shader resource)
    CD3D11_TEXTURE2D_DESC texDesc(colorFmt, m_minimapWidth, m_minimapHeight, 1, 1,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (FAILED(dev->CreateTexture2D(&texDesc, nullptr, m_minimapTexture.GetAddressOf())))
        return;

    // RTV
    CD3D11_RENDER_TARGET_VIEW_DESC rtvDesc(D3D11_RTV_DIMENSION_TEXTURE2D, colorFmt);
    if (FAILED(dev->CreateRenderTargetView(m_minimapTexture.Get(), &rtvDesc, m_minimapRTV.GetAddressOf())))
        return;

    // SRV for ImGui display
    CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2D, colorFmt, 0, 1);
    if (FAILED(dev->CreateShaderResourceView(m_minimapTexture.Get(), &srvDesc, m_minimapSRV.GetAddressOf())))
        return;

    // Depth texture
    CD3D11_TEXTURE2D_DESC depthDesc(depthFmt, m_minimapWidth, m_minimapHeight, 1, 1,
        D3D11_BIND_DEPTH_STENCIL);
    if (FAILED(dev->CreateTexture2D(&depthDesc, nullptr, m_minimapDepthTexture.GetAddressOf())))
        return;

    // DSV
    CD3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc(D3D11_DSV_DIMENSION_TEXTURE2D);
    if (FAILED(dev->CreateDepthStencilView(m_minimapDepthTexture.Get(), &dsvDesc, m_minimapDSV.GetAddressOf())))
        return;

    m_minimapResourcesReady = true;
}


void ReplayWindow::RenderMinimap()
{
    if (!m_minimapResourcesReady) return;

    Terrain* terrain = m_mapRenderer ? m_mapRenderer->GetTerrain() : nullptr;
    if (!terrain) return;

    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;

    // --- Save main camera state ---
    XMFLOAT3 savedCamPos = cam->GetPosition3f();
    float savedYaw    = cam->GetYaw();
    float savedPitch  = cam->GetPitch();
    float savedFov    = cam->GetFovY();
    float savedAspect = cam->GetAspectRatio();
    float savedNear   = cam->GetNearZ();
    float savedFar    = cam->GetFarZ();
    CameraType savedType = cam->GetCameraType();
    float savedViewW  = cam->GetViewWidth();
    float savedViewH  = cam->GetViewHeight();

    // --- Compute orthographic camera from terrain bounds ---
    const auto& b = terrain->m_bounds;
    float cx = (b.map_min_x + b.map_max_x) * 0.5f + m_minimapPanX;
    float cz = (b.map_min_z + b.map_max_z) * 0.5f + m_minimapPanZ;

    float fullMapW = b.map_max_x - b.map_min_x;
    float fullMapH = b.map_max_z - b.map_min_z;
    if (fullMapW < 1.f) fullMapW = 10000.f;
    if (fullMapH < 1.f) fullMapH = 10000.f;

    // Add 5% padding
    fullMapW *= 1.05f;
    fullMapH *= 1.05f;

    // Apply zoom
    float viewW = fullMapW / m_minimapZoom;
    float viewH = fullMapH / m_minimapZoom;

    // Adjust for panel aspect ratio
    float aspect = static_cast<float>(m_minimapWidth) / static_cast<float>(m_minimapHeight);
    float mapAspect = viewW / viewH;
    if (aspect > mapAspect)
        viewW = viewH * aspect;
    else
        viewH = viewW / aspect;

    // Camera height: above the highest terrain point
    float maxTerrainY = std::max(std::abs(b.map_min_y), std::abs(b.map_max_y));
    float camHeight = maxTerrainY + 50000.f;

    // Set orthographic camera looking straight down
    cam->SetPosition(cx, camHeight, cz);
    cam->SetOrientation(-XM_PIDIV2 + 0.001f, 0.f);
    cam->SetFrustumAsOrthographic(viewW, viewH, 10.f, camHeight + 10000.f, true);

    // Suppress atmospheric distance/height fog for the minimap so the top-down
    // view reads crisp and clean instead of hazy/blurred. Must be set before
    // Update() below, which bakes the fog flag into the per-frame constant buffer.
    bool savedFog = m_mapRenderer->GetShouldRenderFog();
    m_mapRenderer->SetShouldRenderFog(false);

    // Rebuild view matrix and upload constant buffer
    m_mapRenderer->Update(0);

    // Cache view*proj for 2D overlay projection
    XMStoreFloat4x4(&m_minimapViewProj, cam->GetView() * cam->GetProj());

    auto* ctx = m_deviceResources->GetD3DDeviceContext();

    // Save main viewport
    UINT numVP = 1;
    D3D11_VIEWPORT savedVP;
    ctx->RSGetViewports(&numVP, &savedVP);

    // Set minimap viewport
    D3D11_VIEWPORT mmVP = { 0.f, 0.f, (float)m_minimapWidth, (float)m_minimapHeight, 0.f, 1.f };
    ctx->RSSetViewports(1, &mmVP);

    // Suppress sky rendering for top-down view
    bool savedSky = m_mapRenderer->GetShouldRenderSky();
    m_mapRenderer->SetShouldRenderSky(false);

    // Clear minimap targets
    float clearColor[4] = { 0.04f, 0.06f, 0.08f, 1.f };
    ctx->ClearRenderTargetView(m_minimapRTV.Get(), clearColor);
    ctx->ClearDepthStencilView(m_minimapDSV.Get(), D3D11_CLEAR_DEPTH, 0.f, 0);

    // Render terrain/props/water to minimap target (no picking)
    m_mapRenderer->Render(m_minimapRTV.Get(), nullptr, m_minimapDSV.Get());

    // --- Restore main camera ---
    m_mapRenderer->SetShouldRenderSky(savedSky);
    m_mapRenderer->SetShouldRenderFog(savedFog);

    if (savedType == CameraType::Orthographic)
        cam->SetFrustumAsOrthographic(savedViewW, savedViewH, savedNear, savedFar, true);
    else
        cam->SetFrustumAsPerspective(savedFov, savedAspect, savedNear, savedFar, true);

    cam->SetPosition(savedCamPos.x, savedCamPos.y, savedCamPos.z);
    cam->SetOrientation(savedPitch, savedYaw);
    m_mapRenderer->Update(0);

    // Unbind minimap render targets
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    // Restore main viewport
    ctx->RSSetViewports(1, &savedVP);
}


void ReplayWindow::DrawMinimapPanel()
{
    m_minimapCursorActive = false;
    if (!m_minimapEnabled) return;

    if (!m_minimapResourcesReady)
        InitMinimapResources();
    if (!m_minimapResourcesReady) return;

    ImGuiIO& io = ImGui::GetIO();
    float vpW = io.DisplaySize.x;
    float vpH = io.DisplaySize.y;

    ImGui::SetNextWindowSizeConstraints(ImVec2(200.f, 200.f), ImVec2(vpW, vpH));
    if (m_panelLayout.HasSavedSize("minimap"))
        m_panelLayout.ApplySize("minimap");
    else
        ImGui::SetNextWindowSize(ImVec2(420.f, 480.f), ImGuiCond_FirstUseEver);
    m_panelLayout.ApplyPosition("minimap");

    // Match the "Agent Names" panel styling (dark navy bg, amber border,
    // gold checkmark) so the minimap window reads as part of the same UI family.
    ImGui::PushStyleColor(ImGuiCol_WindowBg,      ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,     ImVec4(0.90f, 0.76f, 0.30f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(10.f, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    if (!ImGui::Begin("Minimap", &m_minimapEnabled))
    {
        m_panelLayout.TrackWindow("minimap");
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);
        return;
    }

    m_panelLayout.TrackWindow("minimap");

    // Clamp to viewport
    {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float clampX = std::clamp(pos.x, 0.f, std::max(0.f, vpW - sz.x));
        float clampY = std::clamp(pos.y, 0.f, std::max(0.f, vpH - sz.y));
        if (clampX != pos.x || clampY != pos.y)
            ImGui::SetWindowPos(ImVec2(clampX, clampY));
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();

    // --- Toolbar row ---
    {
        auto ToolbarButton = [](const char* label) -> bool {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(2);
            return clicked;
        };

        // Zoom buttons use GW UI textures (Textures/Game_UI/Cursor/); if a
        // texture is missing we fall back to the plain +/- text buttons.
        auto ZoomButton = [&](const char* texFile, const char* fallbackLabel) -> bool {
            ImTextureID t = LoadGameUICursorTexture(dev, texFile);
            if (t)
            {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.f, 0.f, 0.f, 0.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.12f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.f, 1.f, 1.f, 0.20f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
                bool clicked = ImGui::ImageButton(fallbackLabel, t, ImVec2(16.f, 16.f));
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                return clicked;
            }
            return ToolbarButton(fallbackLabel);
        };

        if (ZoomButton("0xCE39.png", "+"))
            m_minimapZoom = std::clamp(m_minimapZoom * 1.25f, 1.0f, 15.0f);
        ImGui::SameLine();
        if (ZoomButton("0xCE3B.png", "-"))
            m_minimapZoom = std::clamp(m_minimapZoom / 1.25f, 1.0f, 15.0f);
        ImGui::SameLine();
        if (ToolbarButton("Reset")) {
            m_minimapZoom = 1.0f;
            m_minimapPanX = 0.f;
            m_minimapPanZ = 0.f;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%.1fx", m_minimapZoom);
        ImGui::SameLine();
        ImGui::Checkbox("Labels", &m_minimapShowLabels);
        ImGui::SameLine();
        ImGui::Checkbox("Profession", &m_minimapShowProfession);
    }

    // --- Rendered image ---
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float imgW = std::max(64.f, avail.x);
    float imgH = std::max(64.f, avail.y);

    // Dynamic resize: recreate texture if panel size changed
    {
        int newW = (int)imgW;
        int newH = (int)imgH;
        if (newW != m_minimapWidth || newH != m_minimapHeight)
        {
            m_minimapWidth = newW;
            m_minimapHeight = newH;
            InitMinimapResources();
        }
    }

    if (imgW > 0.f && imgH > 0.f)
    {
        ImVec2 imgPos = ImGui::GetCursorScreenPos();

        // Invisible button for mouse interaction (left+right click for pan)
        ImGui::InvisibleButton("##minimap_interact", ImVec2(imgW, imgH),
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hovered = ImGui::IsItemHovered();
        bool cursorHoverAgent = false;  // set below when hovering a player dot (labels off)

        // Draw the rendered texture
        dl->AddImage((ImTextureID)m_minimapSRV.Get(), imgPos,
                     ImVec2(imgPos.x + imgW, imgPos.y + imgH));

        // Mouse wheel zoom (zoom toward center)
        if (hovered && io.MouseWheel != 0.f)
            m_minimapZoom = std::clamp(m_minimapZoom + io.MouseWheel * 0.15f * m_minimapZoom,
                                       1.0f, 15.0f);

        // Left or right mouse drag to pan
        if (ImGui::IsItemActive() &&
            (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Left)))
        {
            Terrain* terrain = m_mapRenderer ? m_mapRenderer->GetTerrain() : nullptr;
            if (terrain)
            {
                const auto& b = terrain->m_bounds;
                float fullMapW = (b.map_max_x - b.map_min_x) * 1.05f;
                float fullMapH = (b.map_max_z - b.map_min_z) * 1.05f;
                float currentViewW = fullMapW / m_minimapZoom;
                float currentViewH = fullMapH / m_minimapZoom;

                ImVec2 md = io.MouseDelta;
                float worldPerPixelX = currentViewW / imgW;
                float worldPerPixelZ = currentViewH / imgH;
                m_minimapPanX -= md.x * worldPerPixelX;
                // Inverted vertical drag: dragging down pans the map up (feels natural)
                m_minimapPanZ += md.y * worldPerPixelZ;

                // Clamp pan to keep map visible
                float halfViewW = fullMapW * 0.5f;
                float halfViewH = fullMapH * 0.5f;
                m_minimapPanX = std::clamp(m_minimapPanX, -halfViewW, halfViewW);
                m_minimapPanZ = std::clamp(m_minimapPanZ, -halfViewH, halfViewH);
            }
        }

        // Reset pan when fully zoomed out
        if (m_minimapZoom <= 1.01f)
        {
            m_minimapPanX = 0.f;
            m_minimapPanZ = 0.f;
        }

        // --- 2D Agent overlay ---
        if (m_agentsClassified)
        {
            XMMATRIX vp = XMLoadFloat4x4(&m_minimapViewProj);
            ImFont* font = ImGui::GetFont();
            float fontSize = font->FontSize * 0.85f;
            const MapTransform& mt = m_replayCtx.mapTransform;
            const InterpolationSettings& is = m_replayCtx.interpSettings;
            float now = m_debugTimeline;

            const ImU32 kRedTeam  = IM_COL32(0xFF, 0x99, 0x9A, 0xFF);
            const ImU32 kBlueTeam = IM_COL32(0x99, 0xCB, 0xFD, 0xFF);
            const ImU32 kNeutral  = IM_COL32(255, 220, 100, 200);

            auto ProjectToImage = [&](const XMFLOAT3& pos, float& outX, float& outY) -> bool
            {
                XMVECTOR wp = XMVectorSet(pos.x, pos.y, pos.z, 1.f);
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

            // Hover-label tracking: show the name of the nearest dot when hovering.
            // For players: only when labels are globally hidden.
            // For NPCs/gadgets/spirits: always on hover.
            bool        hoverLabelActive = false;
            float       hoverLabelDist2  = FLT_MAX;
            ImVec2      hoverLabelPos(0.f, 0.f);
            float       hoverLabelDotR   = 0.f;
            std::string hoverLabelText;
            ImU32       hoverLabelCol    = IM_COL32(255, 255, 255, 240);

            const ImU32 kGrey      = IM_COL32(160, 160, 160, 220);
            const ImU32 kGold      = IM_COL32(230, 195, 50, 255);

            // Compute Resurrection Shrine team attribution once (nearest guild
            // lord). Shrines are static, so this only needs to run a single time.
            if (!m_resShrineTeamComputed)
            {
                struct LordPos { float x, y, z; int team; };
                std::vector<LordPos> lords;
                for (auto& [aid, a] : m_replayCtx.agents)
                {
                    if (a.snapshots.empty()) continue;
                    if (a.categoryName.find("Guild Lord") == std::string::npos) continue;
                    const auto& s0 = a.snapshots.front();
                    lords.push_back({ s0.x, s0.y, s0.z, (int)a.teamId });
                }
                for (auto& [aid, a] : m_replayCtx.agents)
                {
                    if (a.snapshots.empty()) continue;
                    if (a.categoryName != "Resurrection Shrine") continue;
                    const auto& s0 = a.snapshots.front();
                    float best = FLT_MAX; int bestTeam = 0;
                    for (auto& lp : lords)
                    {
                        float dx = lp.x - s0.x, dy = lp.y - s0.y, dz = lp.z - s0.z;
                        float d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 < best) { best = d2; bestTeam = lp.team; }
                    }
                    m_resShrineTeam[aid] = bestTeam;
                }
                m_resShrineTeamComputed = true;
            }

            // Flag icons (shared with the 3D timeline overlay)
            ImTextureID texFlagRed  = LoadFlagIcon(dev, "Red_flag_waving.svg.png");
            ImTextureID texFlagBlue = LoadFlagIcon(dev, "Blue_flag_waving.svg.png");

            // Click-to-follow: nearest pickable agent (players/NPCs/spirits) under
            // the cursor, resolved on a left click that wasn't a drag.
            int   pickAgentId  = -1;
            float pickDist2    = FLT_MAX;
            bool  followValid  = false;
            ImVec2   followScr(0.f, 0.f);
            XMFLOAT3 followWorld{};

            // Clip agent drawing to the minimap image area
            dl->PushClipRect(imgPos, ImVec2(imgPos.x + imgW, imgPos.y + imgH), true);

            for (auto& [agentId, ard] : m_replayCtx.agents)
            {
                if (ard.snapshots.empty()) continue;

                // Show players, NPCs, spirits, gadgets (flag stands/shrines).
                // Flags are drawn separately from the flag timeline below.
                if (ard.type != AgentType::Player && ard.type != AgentType::NPC
                    && ard.type != AgentType::Spirit
                    && ard.type != AgentType::Gadget && ard.type != AgentType::ObeliskFlagStand)
                    continue;

                const AgentSnapshot* snap = FindSnapshotAtTime(ard, now);
                if (!snap) continue;

                bool isDead = snap->is_dead;

                // Spirits: only show the *active* ones. A spirit is active when
                // it is within its snapshot lifetime, currently alive, not
                // explicitly dead, and not overlap-hidden by a newer spirit of
                // the same type (mirrors the party-window / overlay logic).
                if (ard.type == AgentType::Spirit)
                {
                    if (ard.overlapHidden) continue;
                    if (now < ard.snapshots.front().time ||
                        now > ard.snapshots.back().time) continue;
                    if (!snap->is_alive || snap->is_dead) continue;
                }

                // Summoned minions (e.g. Bone Horror): only show while alive
                // and existing (same rule as the 3D model).
                if (ard.type == AgentType::NPC && IsNpcHiddenWhenDead(ard.modelId))
                {
                    if (!ard.isMinionVisibleAtTime(now)) continue;
                }

                // NPCs/gadgets: skip dead ones (players persist as graves)
                if (isDead && ard.type != AgentType::Player) continue;

                float sx, sy, sz;
                InterpolateAgentPosition(ard, now, is, sx, sy, sz);
                XMFLOAT3 pos = ApplyMapTransformToPos(sx, sy, sz, mt);

                float scrX, scrY;
                if (!ProjectToImage(pos, scrX, scrY)) continue;
                if (!InBounds(scrX, scrY)) continue;

                // Track the currently-followed agent so we can draw the camera
                // direction indicator centered on it after the loop.
                if (agentId == m_followedAgentId)
                {
                    followValid = true;
                    followScr   = ImVec2(scrX, scrY);
                    followWorld = pos;
                }

                // Click-to-follow pick test. Any drawn agent is focusable:
                // players, NPCs, spirits, gadgets/items and flag stands.
                bool pickable = true;
                if (pickable && hovered)
                {
                    float mdx = io.MousePos.x - scrX;
                    float mdy = io.MousePos.y - scrY;
                    float d2  = mdx * mdx + mdy * mdy;
                    const float pr = 11.f;
                    if (d2 <= pr * pr && d2 < pickDist2)
                    {
                        pickDist2   = d2;
                        pickAgentId = agentId;
                    }
                }

                // --- Flag stands, obelisk stands & shrines: icon markers ---
                {
                    auto TeamHoverCol = [&](int team) -> ImU32 {
                        if (team == 1)      return IM_COL32(0xFF, 0x99, 0x9A, 0xFF);
                        else if (team == 2) return IM_COL32(0x99, 0xCB, 0xFD, 0xFF);
                        return IM_COL32(255, 255, 255, 240);
                    };
                    auto DrawIconMarker = [&](ImTextureID tex, float half, ImU32 tint,
                                              int hoverTeam)
                    {
                        if (tex)
                            dl->AddImage(tex, ImVec2(scrX - half, scrY - half),
                                         ImVec2(scrX + half, scrY + half),
                                         ImVec2(0, 0), ImVec2(1, 1), tint);
                        else {
                            dl->AddCircleFilled(ImVec2(scrX, scrY), half * 0.5f + 1.f,
                                                IM_COL32(0, 0, 0, 180));
                            dl->AddCircleFilled(ImVec2(scrX, scrY), half * 0.5f, tint);
                        }
                        if (hovered)
                        {
                            float mdx = io.MousePos.x - scrX;
                            float mdy = io.MousePos.y - scrY;
                            float d2  = mdx * mdx + mdy * mdy;
                            float pick = half + 4.f;
                            if (d2 <= pick * pick && d2 < hoverLabelDist2)
                            {
                                hoverLabelDist2  = d2;
                                hoverLabelActive = true;
                                hoverLabelText   = GetAgentLabel(ard);
                                hoverLabelPos    = ImVec2(scrX, scrY);
                                hoverLabelDotR   = half;
                                hoverLabelCol    = TeamHoverCol(hoverTeam);
                            }
                        }
                    };

                    bool isObeliskStand = (ard.type == AgentType::ObeliskFlagStand
                                           || ard.categoryName == "Obelisk Flag Stand");
                    bool isTowerStand   = (ard.categoryName == "Tower Flag Stand");
                    bool isHealthShrine = (ard.categoryName == "Southern Health Shrine");
                    bool isResShrine    = (ard.categoryName == "Resurrection Shrine");

                    if (isTowerStand || isObeliskStand)
                    {
                        StandOwner owner = isObeliskStand
                            ? m_flagTimeline.obelisk.ownerAtTime(now)
                            : m_flagTimeline.stand.ownerAtTime(now);
                        const char* file = (owner == StandOwner::Red)  ? "RedFlag.png"
                                         : (owner == StandOwner::Blue) ? "BlueFlag.png"
                                                                       : "GreyFlag.png";
                        int hteam = (owner == StandOwner::Red) ? 1
                                  : (owner == StandOwner::Blue) ? 2 : 0;
                        DrawIconMarker(LoadNPCIcon(dev, file), 11.f,
                                       IM_COL32(255, 255, 255, 255), hteam);
                        continue;
                    }
                    if (isHealthShrine)
                    {
                        int owner = 0; // 0=neutral,1=red,2=blue
                        if (!m_wurmsShrineSamples.empty())
                        {
                            int lastIdx = (int)m_wurmsShrineSamples.size() - 1;
                            int idx = std::clamp((int)(now / m_wurmsShrineSampleDt), 0, lastIdx);
                            owner = m_wurmsShrineSamples[idx].ownerTeam;
                        }
                        const char* file = (owner == 1) ? "RedAnkh.png"
                                         : (owner == 2) ? "BlueAnkh.png"
                                                        : "GreyAnkh.png";
                        DrawIconMarker(LoadNPCIcon(dev, file), 11.f,
                                       IM_COL32(255, 255, 255, 255), owner);
                        continue;
                    }
                    if (isResShrine)
                    {
                        int team = 0;
                        auto rit = m_resShrineTeam.find(agentId);
                        if (rit != m_resShrineTeam.end()) team = rit->second;
                        ImU32 col = (team == 1) ? kRedTeam
                                  : (team == 2) ? kBlueTeam
                                                : kGold;
                        dl->AddCircleFilled(ImVec2(scrX, scrY), 4.5f,
                                            IM_COL32(0, 0, 0, 180));
                        dl->AddCircleFilled(ImVec2(scrX, scrY), 3.5f, col);
                        if (hovered)
                        {
                            float mdx = io.MousePos.x - scrX;
                            float mdy = io.MousePos.y - scrY;
                            float d2  = mdx * mdx + mdy * mdy;
                            float pick = 12.f;
                            if (d2 <= pick * pick && d2 < hoverLabelDist2)
                            {
                                hoverLabelDist2  = d2;
                                hoverLabelActive = true;
                                hoverLabelText   = GetAgentLabel(ard);
                                hoverLabelPos    = ImVec2(scrX, scrY);
                                hoverLabelDotR   = 3.5f;
                                hoverLabelCol    = TeamHoverCol(team);
                            }
                        }
                        continue;
                    }
                }

                // Determine color and size
                ImU32 dotColor;
                float dotRadius;

                bool isTowerFlagStand = (ard.categoryName == "Tower Flag Stand");
                bool isFlameSentinel  = (ard.categoryName == "Lesser Flame Sentinel");

                if (isTowerFlagStand)
                    dotColor = kGold;
                else if (isFlameSentinel)
                    dotColor = kGrey;
                else if (ard.teamId == 1)      dotColor = kRedTeam;
                else if (ard.teamId == 2) dotColor = kBlueTeam;
                else                      dotColor = kNeutral;

                bool isLord = (ard.categoryName.find("Guild Lord") != std::string::npos);
                if (isLord)
                    dotRadius = 6.f;
                else if (ard.type == AgentType::Player)
                    dotRadius = 4.5f;
                else if (ard.type == AgentType::Flag || isTowerFlagStand)
                    dotRadius = 3.5f;
                else if (ard.type == AgentType::Spirit)
                    dotRadius = 3.f;
                else
                    dotRadius = 3.f;

                // --- Dead player rendering ---
                if (isDead && ard.type == AgentType::Player)
                {
                    if (m_minimapShowProfession && ard.primaryProf > 0)
                    {
                        // Greyed-out profession icon for dead players
                        ImTextureID profTex = LoadProfIcon(dev, ard.primaryProf);
                        if (profTex)
                        {
                            float icoR = std::max(dotRadius + 3.5f, 8.f);
                            ImVec2 p0(scrX - icoR, scrY - icoR);
                            ImVec2 p1(scrX + icoR, scrY + icoR);
                            dl->AddCircleFilled(ImVec2(scrX, scrY), icoR + 1.5f,
                                                IM_COL32(0, 0, 0, 205));
                            // Desaturated grey tint to signal death
                            ImU32 greyRing = (ard.teamId == 1) ? IM_COL32(140, 90, 90, 200)
                                                               : IM_COL32(90, 110, 140, 200);
                            dl->AddImage(profTex, p0, p1, ImVec2(0, 0), ImVec2(1, 1),
                                         IM_COL32(130, 130, 130, 180));
                            dl->AddCircle(ImVec2(scrX, scrY), icoR + 1.5f, greyRing, 0, 1.6f);
                        }
                    }
                    else
                    {
                        // Small cross (grave marker), team-colored
                        ImU32 crossCol = (ard.teamId == 1)
                            ? IM_COL32(0xFF, 0x99, 0x9A, 200)
                            : IM_COL32(0x99, 0xCB, 0xFD, 200);
                        float cr = 4.f;
                        dl->AddLine(ImVec2(scrX - cr, scrY - cr), ImVec2(scrX + cr, scrY + cr),
                                    IM_COL32(0, 0, 0, 180), 2.5f);
                        dl->AddLine(ImVec2(scrX + cr, scrY - cr), ImVec2(scrX - cr, scrY + cr),
                                    IM_COL32(0, 0, 0, 180), 2.5f);
                        dl->AddLine(ImVec2(scrX - cr, scrY - cr), ImVec2(scrX + cr, scrY + cr),
                                    crossCol, 1.6f);
                        dl->AddLine(ImVec2(scrX + cr, scrY - cr), ImVec2(scrX - cr, scrY + cr),
                                    crossCol, 1.6f);
                    }

                    // Hover label for dead players too
                    if (!m_minimapShowLabels && hovered)
                    {
                        float mdx = io.MousePos.x - scrX;
                        float mdy = io.MousePos.y - scrY;
                        float d2  = mdx * mdx + mdy * mdy;
                        float pick = 12.f;
                        if (d2 <= pick * pick && d2 < hoverLabelDist2)
                        {
                            hoverLabelDist2  = d2;
                            hoverLabelActive = true;
                            hoverLabelText   = GetAgentLabel(ard);
                            hoverLabelPos    = ImVec2(scrX, scrY);
                            hoverLabelDotR   = 5.f;
                            if (ard.teamId == 1)      hoverLabelCol = IM_COL32(0xFF, 0x99, 0x9A, 0xFF);
                            else if (ard.teamId == 2) hoverLabelCol = IM_COL32(0x99, 0xCB, 0xFD, 0xFF);
                            else                      hoverLabelCol = IM_COL32(255, 255, 255, 240);
                        }
                    }
                    continue;  // skip further drawing for dead players
                }

                // --- Alive agent marker ---
                bool drewProfIcon = false;
                if (m_minimapShowProfession && ard.type == AgentType::Player
                    && ard.primaryProf > 0)
                {
                    ImTextureID profTex = LoadProfIcon(dev, ard.primaryProf);
                    if (profTex)
                    {
                        float icoR = std::max(dotRadius + 3.5f, 8.f);
                        ImVec2 p0(scrX - icoR, scrY - icoR);
                        ImVec2 p1(scrX + icoR, scrY + icoR);
                        dl->AddCircleFilled(ImVec2(scrX, scrY), icoR + 1.5f,
                                            IM_COL32(0, 0, 0, 205));
                        dl->AddImage(profTex, p0, p1);
                        dl->AddCircle(ImVec2(scrX, scrY), icoR + 1.5f, dotColor, 0, 1.6f);
                        drewProfIcon = true;
                    }
                }

                if (!drewProfIcon)
                {
                    dl->AddCircleFilled(ImVec2(scrX, scrY), dotRadius + 1.f,
                                        IM_COL32(0, 0, 0, 180));
                    dl->AddCircleFilled(ImVec2(scrX, scrY), dotRadius, dotColor);
                }

                // Hover-to-reveal label:
                //  - Players: only when labels are globally hidden
                //  - NPCs/spirits/gadgets/flag stands: always on hover
                bool wantHover = false;
                if (ard.type == AgentType::Player)
                    wantHover = !m_minimapShowLabels;
                else
                    wantHover = true;

                if (wantHover && hovered)
                {
                    float mdx = io.MousePos.x - scrX;
                    float mdy = io.MousePos.y - scrY;
                    float d2  = mdx * mdx + mdy * mdy;
                    float pick = std::max(dotRadius, 8.f) + 4.f;
                    if (d2 <= pick * pick && d2 < hoverLabelDist2)
                    {
                        hoverLabelDist2  = d2;
                        hoverLabelActive = true;
                        hoverLabelText   = GetAgentLabel(ard);
                        hoverLabelPos    = ImVec2(scrX, scrY);
                        hoverLabelDotR   = drewProfIcon ? std::max(dotRadius + 3.5f, 8.f)
                                                        : dotRadius;
                        if (ard.teamId == 1)      hoverLabelCol = IM_COL32(0xFF, 0x99, 0x9A, 0xFF);
                        else if (ard.teamId == 2) hoverLabelCol = IM_COL32(0x99, 0xCB, 0xFD, 0xFF);
                        else                      hoverLabelCol = IM_COL32(255, 255, 255, 240);
                    }
                }

                // Labels (only for players when enabled)
                if (m_minimapShowLabels && ard.type == AgentType::Player)
                {
                    std::string label = GetAgentLabel(ard);
                    ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label.c_str());
                    float lx = scrX - textSz.x * 0.5f;
                    float ly = scrY + dotRadius + 2.f;

                    ImU32 labelCol;
                    if (ard.teamId == 1)      labelCol = IM_COL32(0xFF, 0x99, 0x9A, 0xE6);
                    else if (ard.teamId == 2) labelCol = IM_COL32(0x99, 0xCB, 0xFD, 0xE6);
                    else                      labelCol = IM_COL32(255, 255, 255, 230);

                    dl->AddText(font, fontSize, ImVec2(lx + 1.f, ly + 1.f),
                                IM_COL32(0, 0, 0, 200), label.c_str());
                    dl->AddText(font, fontSize, ImVec2(lx, ly), labelCol, label.c_str());
                }
            }

            // --- Active flag per team (from the flag timeline) ---
            // Only the single active (non-stand) flag per team is drawn. When a
            // player carries it, the icon sits beside their marker; otherwise it
            // is drawn at the flag's ground/base position.
            if (m_flagTimelineBuilt && m_flagTimeline.valid)
            {
                for (int ti = 0; ti < 2; ti++)
                {
                    auto& ft = m_flagTimeline.teams[ti];
                    if (ft.events.empty()) continue;

                    FlagLocation loc = ft.locationAtTime(now);
                    if (loc == FlagLocation::Stand) continue; // shown on the stand

                    ImTextureID ftex = (ti == 0) ? texFlagRed : texFlagBlue;
                    bool carried  = (loc == FlagLocation::Carried);
                    int  carrier  = carried ? ft.carrierAtTime(now) : -1;

                    float fwx, fwy, fwz;
                    if (carried && carrier >= 0)
                    {
                        auto cit = m_replayCtx.agents.find(carrier);
                        if (cit != m_replayCtx.agents.end() && !cit->second.snapshots.empty())
                            InterpolateAgentPosition(cit->second, now, is, fwx, fwy, fwz);
                        else
                            ft.positionAtTime(now, fwx, fwy, fwz);
                    }
                    else
                        ft.positionAtTime(now, fwx, fwy, fwz);

                    XMFLOAT3 fpos = ApplyMapTransformToPos(fwx, fwy, fwz, mt);
                    float fsx, fsy;
                    if (!ProjectToImage(fpos, fsx, fsy)) continue;
                    if (!InBounds(fsx, fsy)) continue;

                    const float fh = 9.f;
                    // Offset the icon beside the player marker when carried.
                    float ox = carried ? 8.f : 0.f;
                    float oy = carried ? -8.f : 0.f;
                    if (ftex)
                        dl->AddImage(ftex, ImVec2(fsx - fh + ox, fsy - fh + oy),
                                     ImVec2(fsx + fh + ox, fsy + fh + oy));
                    else
                    {
                        ImU32 fc = (ti == 0) ? kRedTeam : kBlueTeam;
                        dl->AddCircleFilled(ImVec2(fsx + ox, fsy + oy), 4.f,
                                            IM_COL32(0, 0, 0, 180));
                        dl->AddCircleFilled(ImVec2(fsx + ox, fsy + oy), 3.f, fc);
                    }
                }
            }

            // --- Isle of the Weeping Stone lever door state icon ---
            // Draw an enlarged open/closed marker at the resolved lever door (object 122).
            // Hovering the icon shows a "Gate" label; it is not clickable.
            if (m_weepingLeverHasDoor)
            {
                float dsx, dsy;
                if (ProjectToImage(m_weepingLeverWorldPos, dsx, dsy) && InBounds(dsx, dsy))
                {
                    bool doorOpen = m_doorTypeOpen[19];
                    const float dh = 16.f;                 // enlarged state icon (was 10)

                    ImTextureID dtex = LoadNPCIconDDS(dev, doorOpen ? "texture_193320.dds"
                                                                    : "texture_191464.dds");
                    if (dtex)
                        dl->AddImage(dtex, ImVec2(dsx - dh, dsy - dh),
                                     ImVec2(dsx + dh, dsy + dh));
                    else
                    {
                        ImU32 c = doorOpen ? IM_COL32(120, 230, 120, 230)
                                           : IM_COL32(230, 120, 120, 230);
                        dl->AddCircleFilled(ImVec2(dsx, dsy), dh * 0.4f + 1.f,
                                            IM_COL32(0, 0, 0, 180));
                        dl->AddCircleFilled(ImVec2(dsx, dsy), dh * 0.4f, c);
                    }

                    // Hover label only (no click, no dot).
                    if (hovered)
                    {
                        float mdx = io.MousePos.x - dsx;
                        float mdy = io.MousePos.y - dsy;
                        float d2  = mdx * mdx + mdy * mdy;
                        float pick = dh + 2.f;
                        if (d2 <= pick * pick && d2 < hoverLabelDist2)
                        {
                            hoverLabelDist2  = d2;
                            hoverLabelActive = true;
                            hoverLabelText   = "Gate";
                            hoverLabelPos    = ImVec2(dsx, dsy);
                            hoverLabelDotR   = dh;
                            hoverLabelCol    = IM_COL32(255, 255, 255, 240);
                        }
                    }
                }
            }

            // --- Frozen Isle lever gate state icons (doors 1-4) ---
            // Same open/closed markers as the Weeping Stone gate: draw one per resolved lever
            // gate at its world position, using m_doorTypeOpen[doorType] for the state.
            // Hovering shows a "Gate" label; not clickable.
            if (m_replayCtx.datMapId == 0x1F265)
            {
                for (const auto& [doorType, worldPos] : m_frozenGateIcons)
                {
                    float dsx, dsy;
                    if (!ProjectToImage(worldPos, dsx, dsy) || !InBounds(dsx, dsy))
                        continue;

                    bool doorOpen = (doorType >= 1 && doorType < 25) && m_doorTypeOpen[doorType];
                    const float dh = 16.f;

                    ImTextureID dtex = LoadNPCIconDDS(dev, doorOpen ? "texture_193320.dds"
                                                                    : "texture_191464.dds");
                    if (dtex)
                        dl->AddImage(dtex, ImVec2(dsx - dh, dsy - dh),
                                     ImVec2(dsx + dh, dsy + dh));
                    else
                    {
                        ImU32 c = doorOpen ? IM_COL32(120, 230, 120, 230)
                                           : IM_COL32(230, 120, 120, 230);
                        dl->AddCircleFilled(ImVec2(dsx, dsy), dh * 0.4f + 1.f,
                                            IM_COL32(0, 0, 0, 180));
                        dl->AddCircleFilled(ImVec2(dsx, dsy), dh * 0.4f, c);
                    }

                    if (hovered)
                    {
                        float mdx = io.MousePos.x - dsx;
                        float mdy = io.MousePos.y - dsy;
                        float d2  = mdx * mdx + mdy * mdy;
                        float pick = dh + 2.f;
                        if (d2 <= pick * pick && d2 < hoverLabelDist2)
                        {
                            hoverLabelDist2  = d2;
                            hoverLabelActive = true;
                            hoverLabelText   = "Gate";
                            hoverLabelPos    = ImVec2(dsx, dsy);
                            hoverLabelDotR   = dh;
                            hoverLabelCol    = IM_COL32(255, 255, 255, 240);
                        }
                    }
                }
            }

            // --- Camera-follow direction indicator ---
            // Draw the tracking icon centered on the followed agent, rotated to
            // point in the replay camera's horizontal look direction.
            if (m_cameraMode == CameraMode::FollowAgent && followValid)
            {
                Camera* mcam = m_mapRenderer ? m_mapRenderer->GetCamera() : nullptr;
                if (mcam)
                {
                    float yaw = mcam->GetYaw();
                    XMFLOAT3 dirPos(followWorld.x + sinf(yaw) * 1000.f,
                                    followWorld.y,
                                    followWorld.z + cosf(yaw) * 1000.f);
                    float dsx, dsy, rot = 0.f;
                    if (ProjectToImage(dirPos, dsx, dsy))
                        rot = atan2f(dsy - followScr.y, dsx - followScr.x) + XM_PIDIV2;

                    const float th = 16.f;
                    float ca = cosf(rot), sa = sinf(rot);
                    auto R = [&](float lx, float ly) -> ImVec2 {
                        return ImVec2(followScr.x + lx * ca - ly * sa,
                                      followScr.y + lx * sa + ly * ca);
                    };
                    ImTextureID trackTex = LoadNPCIconDDS(dev, "GW.EXE_0x37802955.dds");
                    if (trackTex)
                        dl->AddImageQuad(trackTex,
                                         R(-th, -th), R(th, -th), R(th, th), R(-th, th),
                                         ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1),
                                         IM_COL32(255, 255, 255, 255));
                    else
                    {
                        // Fallback arrow pointing in the camera direction
                        ImVec2 t0 = R(0.f, -th), t1 = R(-th * 0.6f, th * 0.5f),
                               t2 = R(th * 0.6f, th * 0.5f);
                        dl->AddTriangleFilled(t0, t1, t2, IM_COL32(255, 230, 120, 230));
                        dl->AddTriangle(t0, t1, t2, IM_COL32(0, 0, 0, 200), 1.5f);
                    }
                }
            }

            // --- Resolve click-to-follow ---
            // A left-button release with negligible drag over a pickable agent
            // starts following that agent (drags are reserved for panning).
            if (hovered && pickAgentId >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f);
                if (fabsf(dd.x) + fabsf(dd.y) < 4.f)
                    EnterFollowMode(pickAgentId);
            }

            // Hovered agent label tooltip
            if (hoverLabelActive)
            {
                ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f,
                                                    hoverLabelText.c_str());
                float lx = hoverLabelPos.x - textSz.x * 0.5f;
                float ly = hoverLabelPos.y + hoverLabelDotR + 2.f;
                dl->AddText(font, fontSize, ImVec2(lx + 1.f, ly + 1.f),
                            IM_COL32(0, 0, 0, 200), hoverLabelText.c_str());
                dl->AddText(font, fontSize, ImVec2(lx, ly), hoverLabelCol,
                            hoverLabelText.c_str());
            }

            cursorHoverAgent = hoverLabelActive;

            dl->PopClipRect();
        }

        // --- Software cursor (minimap window only) ---
        // While the cursor is over the minimap image, hide the OS cursor and draw
        // a GW cursor texture instead. State picks which texture:
        //   right mouse held      -> 0x20E1C.png
        //   hovering a player dot -> 0x20E1A.png  (only when labels are hidden)
        //   otherwise             -> 0x15EDD.png
        if (hovered)
        {
            const char* cursorFile = "0x22FD.png";
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Left))
                cursorFile = "0x20E1C.png";
            else if (cursorHoverAgent)
                cursorFile = "0x20E1A.png";

            ImTextureID cursorTex = LoadGameUICursorTexture(dev, cursorFile);
            if (cursorTex)
            {
                m_minimapCursorActive = true;
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
                const float cs = 32.f;
                ImVec2 mp = io.MousePos;
                ImGui::GetForegroundDrawList()->AddImage(
                    cursorTex, mp, ImVec2(mp.x + cs, mp.y + cs));
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}
