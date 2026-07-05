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

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.078f, 0.102f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   12.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(10.f, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    if (!ImGui::Begin("Minimap", &m_minimapEnabled))
    {
        m_panelLayout.TrackWindow("minimap");
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
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

    // --- Toolbar row ---
    {
        auto ToolbarButton = [](const char* label) -> bool {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(2);
            return clicked;
        };

        if (ToolbarButton("+"))
            m_minimapZoom = std::clamp(m_minimapZoom * 1.25f, 1.0f, 15.0f);
        ImGui::SameLine();
        if (ToolbarButton("-"))
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

        // Invisible button for mouse interaction (right-click for pan)
        ImGui::InvisibleButton("##minimap_interact", ImVec2(imgW, imgH),
            ImGuiButtonFlags_MouseButtonRight);
        bool hovered = ImGui::IsItemHovered();

        // Draw the rendered texture
        dl->AddImage((ImTextureID)m_minimapSRV.Get(), imgPos,
                     ImVec2(imgPos.x + imgW, imgPos.y + imgH));

        // Mouse wheel zoom (zoom toward center)
        if (hovered && io.MouseWheel != 0.f)
            m_minimapZoom = std::clamp(m_minimapZoom + io.MouseWheel * 0.15f * m_minimapZoom,
                                       1.0f, 15.0f);

        // Right-mouse drag to pan
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right))
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
                m_minimapPanZ -= md.y * worldPerPixelZ;

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

            // Clip agent drawing to the minimap image area
            dl->PushClipRect(imgPos, ImVec2(imgPos.x + imgW, imgPos.y + imgH), true);

            for (auto& [agentId, ard] : m_replayCtx.agents)
            {
                if (ard.snapshots.empty()) continue;

                // Only show players, NPCs, flags
                if (ard.type != AgentType::Player && ard.type != AgentType::NPC
                    && ard.type != AgentType::Flag)
                    continue;

                const AgentSnapshot* snap = FindSnapshotAtTime(ard, now);
                if (!snap) continue;

                bool isDead = snap->is_dead;
                if (isDead && ard.type != AgentType::Flag) continue;

                float sx, sy, sz;
                InterpolateAgentPosition(ard, now, is, sx, sy, sz);
                XMFLOAT3 pos = ApplyMapTransformToPos(sx, sy, sz, mt);

                float scrX, scrY;
                if (!ProjectToImage(pos, scrX, scrY)) continue;
                if (!InBounds(scrX, scrY)) continue;

                // Determine color and size
                ImU32 dotColor;
                float dotRadius;

                if (ard.teamId == 1)      dotColor = kRedTeam;
                else if (ard.teamId == 2) dotColor = kBlueTeam;
                else                      dotColor = kNeutral;

                bool isLord = (ard.categoryName.find("Guild Lord") != std::string::npos);
                if (isLord)
                    dotRadius = 6.f;
                else if (ard.type == AgentType::Player)
                    dotRadius = 4.5f;
                else if (ard.type == AgentType::Flag)
                    dotRadius = 3.5f;
                else
                    dotRadius = 3.f;

                // Draw dot: black outline + filled circle
                dl->AddCircleFilled(ImVec2(scrX, scrY), dotRadius + 1.f,
                                    IM_COL32(0, 0, 0, 180));
                dl->AddCircleFilled(ImVec2(scrX, scrY), dotRadius, dotColor);

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

                    // Shadow text for readability
                    dl->AddText(font, fontSize, ImVec2(lx + 1.f, ly + 1.f),
                                IM_COL32(0, 0, 0, 200), label.c_str());
                    dl->AddText(font, fontSize, ImVec2(lx, ly), labelCol, label.c_str());
                }
            }

            dl->PopClipRect();
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}
