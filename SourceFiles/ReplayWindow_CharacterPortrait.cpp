#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"

// ---------------------------------------------------------------------------
// The character panel's portrait: the player standing in the gap between the weapon sets and the
// armour, in the armour and dyes the recording carries, idling, with his weapons in his hands.
//
// It is the replay's own character - the same submeshes, the same textures, the same draw pass -
// drawn a second time into a small target of its own, from a camera of its own, in a pose of its
// own. Nothing is composed or loaded for it. What it borrows it hands back: the camera, the light,
// the render targets and the bone palette of the submeshes it posed are all restored before the
// interface is drawn.
// ---------------------------------------------------------------------------

namespace
{
    // Drawn at twice the panel's size and shrunk by the interface's bilinear sampler: the target
    // cannot be multisampled (it is read as a texture), and at a hundred pixels wide a character's
    // silhouette is nearly all edge.
    constexpr int kSupersample = 2;

    // The replay's own idle code: the fallback when no clip on the rig has a standing idle.
    constexpr uint32_t kIdlePrimaryHash = 0x365C0E24u;

    constexpr float kFovY = XMConvertToRadians(28.f);

    // A studio light, not the map's. The character rules were measured under the laboratory's
    // neutral light (MapRenderer's constructor), so that is the light that shows the dyes as they
    // are; the map's light is often strongly coloured and belongs to the map. It comes from above
    // the camera and a little to its left, so the face is lit.
    DirectionalLight StudioLight(const DirectionalLight& scene)
    {
        DirectionalLight light = scene;
        light.ambient  = XMFLOAT4(0.5f, 0.5f, 0.5f, 1.f);
        light.diffuse  = XMFLOAT4(0.5f, 0.5f, 0.5f, 1.f);
        light.specular = XMFLOAT4(0.1f, 0.1f, 0.1f, 1.f);
        XMStoreFloat3(&light.direction, XMVector3Normalize(XMVectorSet(0.45f, -0.75f, -0.55f, 0.f)));
        return light;
    }

}

// The interface draws its images straight-alpha; the portrait holds premultiplied colour (it was
// composited over transparent black). Drawn straight, every soft edge - hair most of all - would
// come out dark, so the one image is drawn with a blend state of its own. The callback runs inside
// the interface's own render, which resets its state again after the image.
void ReplayWindow::SetPortraitCompositeBlend(const ImDrawList*, const ImDrawCmd* cmd)
{
    auto* blend = static_cast<ID3D11BlendState*>(cmd->UserCallbackData);
    if (!blend) return;
    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    blend->GetDevice(dev.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(ctx.GetAddressOf());
    const float factor[4] = {};
    ctx->OMSetBlendState(blend, factor, 0xFFFFFFFFu);
}

void ReplayWindow::DrawCharacterPortraitImage(ImDrawList* dl, ImTextureID tex, ImVec2 tl, ImVec2 br)
{
    if (!tex || !m_portraitCompositeBlend) return;
    dl->AddCallback(&ReplayWindow::SetPortraitCompositeBlend, m_portraitCompositeBlend.Get());
    dl->AddImage(tex, tl, br);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

ImTextureID ReplayWindow::RequestCharacterPortrait(int panelUid, int agentId, int width, int height,
                                                   uint16_t fallbackMainId, uint16_t fallbackOffId)
{
    CharacterPortrait& p = m_characterPortraits[panelUid];
    p.fallbackMainId = fallbackMainId;
    p.fallbackOffId = fallbackOffId;
    if (p.agentId != agentId)
    {
        // Another player: another rig, another pose, and his own facing.
        p.agentId = agentId;
        p.controller.reset();
        p.controllerModelHash = 0;
        p.rendered = false;
        p.yaw = 0.f;
    }
    p.requested = true;

    width *= kSupersample;
    height *= kSupersample;
    if (width < 8 || height < 8)
        return ImTextureID{};

    if (p.width != width || p.height != height || !p.srv)
    {
        p.color.Reset(); p.rtv.Reset(); p.srv.Reset(); p.depth.Reset(); p.dsv.Reset();
        p.rendered = false;
        p.width = width;
        p.height = height;

        ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;
        if (!dev)
            return ImTextureID{};

        const DXGI_FORMAT colorFmt = m_deviceResources->GetBackBufferFormat();
        const DXGI_FORMAT depthFmt = m_deviceResources->GetDepthBufferFormat();

        CD3D11_TEXTURE2D_DESC colorDesc(colorFmt, width, height, 1, 1,
                                        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        CD3D11_TEXTURE2D_DESC depthDesc(depthFmt, width, height, 1, 1, D3D11_BIND_DEPTH_STENCIL);
        CD3D11_RENDER_TARGET_VIEW_DESC rtvDesc(D3D11_RTV_DIMENSION_TEXTURE2D, colorFmt);
        CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2D, colorFmt, 0, 1);
        CD3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc(D3D11_DSV_DIMENSION_TEXTURE2D);

        if (FAILED(dev->CreateTexture2D(&colorDesc, nullptr, p.color.GetAddressOf())) ||
            FAILED(dev->CreateRenderTargetView(p.color.Get(), &rtvDesc, p.rtv.GetAddressOf())) ||
            FAILED(dev->CreateShaderResourceView(p.color.Get(), &srvDesc, p.srv.GetAddressOf())) ||
            FAILED(dev->CreateTexture2D(&depthDesc, nullptr, p.depth.GetAddressOf())) ||
            FAILED(dev->CreateDepthStencilView(p.depth.Get(), &dsvDesc, p.dsv.GetAddressOf())))
        {
            p.color.Reset(); p.rtv.Reset(); p.srv.Reset(); p.depth.Reset(); p.dsv.Reset();
            return ImTextureID{};
        }
    }

    return p.rendered ? (ImTextureID)p.srv.Get() : ImTextureID{};
}

void ReplayWindow::ReleaseCharacterPortraits()
{
    m_characterPortraits.clear();
    m_portraitBlend.Reset();
    m_portraitCompositeBlend.Reset();
    m_portraitGlowBlend.Reset();
}

void ReplayWindow::RenderCharacterPortraits()
{
    // A portrait no panel asked for last frame belongs to a panel that is closed or collapsed.
    std::erase_if(m_characterPortraits, [](const auto& kv) { return !kv.second.requested; });
    if (m_characterPortraits.empty())
        return;

    ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;
    ID3D11DeviceContext* ctx = m_deviceResources ? m_deviceResources->GetD3DDeviceContext() : nullptr;
    auto* meshManager = m_mapRenderer ? m_mapRenderer->GetMeshManager() : nullptr;
    auto* textureManager = m_mapRenderer ? m_mapRenderer->GetTextureManager() : nullptr;
    const bool ready = dev && ctx && meshManager && textureManager &&
                       m_useAgentModels && m_agentModelsLoaded;
    if (!ready)
    {
        for (auto& [uid, p] : m_characterPortraits) p.requested = false;
        return;
    }

    // Colour blends as the scene does; ALPHA accumulates coverage (a + A(1 - a)), so the target
    // ends up holding exactly how much of each texel the character covers.
    if (!m_portraitBlend)
    {
        D3D11_BLEND_DESC desc{};
        auto& rt = desc.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&desc, m_portraitBlend.GetAddressOf());

        rt.SrcBlend = D3D11_BLEND_ONE;   // the colour is already multiplied by its coverage
        dev->CreateBlendState(&desc, m_portraitCompositeBlend.GetAddressOf());

        // An additive glow is light, not surface: ONE/ONE on the colour, and the coverage left
        // exactly as it was.
        rt.SrcBlend = D3D11_BLEND_ONE;
        rt.DestBlend = D3D11_BLEND_ONE;
        rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
        rt.DestBlendAlpha = D3D11_BLEND_ONE;
        dev->CreateBlendState(&desc, m_portraitGlowBlend.GetAddressOf());
    }

    // ---- save everything this borrows ---------------------------------------------------------
    UINT numVP = 1;
    D3D11_VIEWPORT savedVP{};
    ctx->RSGetViewports(&numVP, &savedVP);

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> savedRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> savedDSV;
    ctx->OMGetRenderTargets(1, savedRTV.GetAddressOf(), savedDSV.GetAddressOf());

    Microsoft::WRL::ComPtr<ID3D11BlendState> savedBlend;
    float savedBlendFactor[4] = {};
    UINT savedSampleMask = 0xFFFFFFFFu;
    ctx->OMGetBlendState(savedBlend.GetAddressOf(), savedBlendFactor, &savedSampleMask);

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> savedDepth;
    UINT savedStencilRef = 0;
    ctx->OMGetDepthStencilState(savedDepth.GetAddressOf(), &savedStencilRef);

    const RasterizerStateType savedRasterizer = m_mapRenderer->GetCurrentRasterizerState();
    const DirectionalLight savedLight = m_mapRenderer->GetDirectionalLight();
    const bool savedFog = m_mapRenderer->GetShouldRenderFog();
    const bool savedModelShadows = m_mapRenderer->GetShouldRenderShadowsForModels();
    // Moving the light's direction arms a re-render of the terrain's shadow maps; the portrait's
    // light is gone again before the next frame, so that request is taken back.
    const bool savedRerenderShadows = m_mapRenderer->GetShouldRerenderShadows();

    // ---- the studio -------------------------------------------------------------------------
    m_mapRenderer->SetDirectionalLight(StudioLight(savedLight));
    m_mapRenderer->SetShouldRenderFog(false);
    m_mapRenderer->SetShouldRenderShadowsForModels(false);

    const auto now = std::chrono::steady_clock::now();

    for (auto& [uid, p] : m_characterPortraits)
    {
        p.requested = false;
        if (!p.rtv || !p.dsv) continue;

        // The base skin, not an avatar form: the sheet is about the player.
        const int agentId = p.agentId;
        auto ardIt = m_replayCtx.agents.find(agentId);
        auto animIt = m_agentAnimStates.find(agentId);
        auto hashIt = m_agentFileHashCache.find(agentId);
        if (ardIt == m_replayCtx.agents.end() || animIt == m_agentAnimStates.end() ||
            hashIt == m_agentFileHashCache.end())
            continue;
        auto tmplIt = m_agentModelTemplates.find(hashIt->second);
        if (tmplIt == m_agentModelTemplates.end()) continue;

        const AgentReplayData& ard = ardIt->second;
        AgentAnimState& animState = animIt->second;
        const AgentModelInstance& tmpl = tmplIt->second;
        if (!animState.hasSkinning || !animState.controller || animState.animMeshes.empty() ||
            !tmpl.clip)
            continue;

        // ---- the pose: idle, on a controller of the portrait's own ------------------------
        if (!p.controller || p.controllerModelHash != hashIt->second)
        {
            // The idle the Wardrobe plays for this profession and sex: the pinned bank's own
            // idle segment, looked for in the pinned bank first, then in the bank the rig chose,
            // then anywhere else on the rig. The replay's idle code is only the last resort - it
            // resolves to a segment that is not the character's standing idle.
            int prof = ard.primaryProf;
            int sex = ard.isFemale ? 1 : 0;
            PlayerModelPoolIdentity(hashIt->second, prof, sex);
            const uint32_t pinnedBank =
                PreferredAnimationFileId(prof, sex, tmpl.modelHash0, tmpl.modelHash1);

            std::vector<std::shared_ptr<GW::Animation::AnimationClip>> candidates;
            for (const auto& entry : tmpl.allClips)
                if (pinnedBank != 0 && entry.sourceFileHash == pinnedBank && entry.clip)
                    candidates.push_back(entry.clip);
            candidates.push_back(tmpl.clip);
            for (const int ci : tmpl.clipOrder)
                if (ci >= 0 && ci < static_cast<int>(tmpl.allClips.size()) && tmpl.allClips[ci].clip)
                    candidates.push_back(tmpl.allClips[ci].clip);

            std::shared_ptr<GW::Animation::AnimationClip> clip;
            int segment = -1;
            for (const auto& candidate : candidates)
            {
                segment = GW::Animation::CharacterIdleSegment(*candidate, tmpl.modelHash0,
                                                              tmpl.modelHash1, prof, sex);
                if (segment >= 0) { clip = candidate; break; }
            }
            if (!clip)
            {
                clip = tmpl.clip;
                segment = 0;
                if (auto seg = tmpl.animCodeToSegment.find(kIdlePrimaryHash);
                    seg != tmpl.animCodeToSegment.end() && seg->second.clipIndex >= 0 &&
                    seg->second.clipIndex < static_cast<int>(tmpl.allClips.size()) &&
                    tmpl.allClips[seg->second.clipIndex].clip)
                {
                    clip = tmpl.allClips[seg->second.clipIndex].clip;
                    segment = seg->second.segmentIndex;
                }
            }

            p.controller = std::make_unique<GW::Animation::AnimationController>();
            p.controller->Initialize(clip);
            p.controller->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
            p.controller->SetLooping(true);
            if (!clip->animationSegments.empty())
                p.controller->SetSegment(segment);
            // A composed character was built for the skinning path its live controller uses.
            p.controller->SetUseMatrixStackSkinning(
                animState.controller->IsUsingMatrixStackSkinning());
            p.controller->SetPlaybackSpeed(100000.f);
            p.controller->Play();
            p.controller->Update(0.f);
            p.controllerModelHash = hashIt->second;
            p.lastTick = now;
        }
        // Wall-clock time: he keeps breathing while the replay is paused.
        const float dt = std::min(std::chrono::duration<float>(now - p.lastTick).count(), 0.1f);
        p.lastTick = now;
        p.controller->Update(dt);

        // ---- placement: the replay's own scale and centring, at the origin ----------------
        const bool composed = HasPlayerVisual(agentId);
        float scale = 1.f;
        float height = 0.f;
        XMMATRIX centering = XMMatrixTranslation(-tmpl.nativeCenter.x, -tmpl.nativeMinY,
                                                 -tmpl.nativeCenter.z);
        float pvScale = 1.f;
        XMFLOAT3 pvCentre{};
        if (composed && PlayerVisualsPlacement(agentId, pvScale, pvCentre))
        {
            scale = pvScale * m_agentModelScale;
            centering = XMMatrixTranslation(-pvCentre.x, -pvCentre.y, -pvCentre.z);
            PlayerVisualsTopY(agentId, m_agentModelScale, height);
        }
        else
        {
            const AgentModelInfo info =
                LookupAgentModelInfo(ard.type, ard.modelId, ard.primaryProf, ard.isFemale);
            scale = (info.fitHeight > 0.f && tmpl.nativeHeight > 1.f)
                        ? (info.fitHeight / tmpl.nativeHeight) * m_agentModelScale
                        : info.npcAdjustment * m_agentModelScale;
        }
        if (height <= 0.f) height = tmpl.nativeHeight * scale;
        if (height <= 0.f) continue;

        // The replay turns the model a quarter turn and then by the agent's heading; a heading
        // of -pi/2 is the one that faces the camera below, which looks down -z.
        const XMMATRIX world = centering
            * XMMatrixRotationY(XM_PIDIV2)
            * XMMatrixScaling(scale, scale, scale)
            * XMMatrixRotationY(-XM_PIDIV2 + p.yaw);
        XMFLOAT4X4 worldF;
        XMStoreFloat4x4(&worldF, world);

        // ---- the camera: the figure fills the five armour rows, head to feet ----------------
        //
        // Every player the same size on the sheet, whatever his height in the game and however
        // many weapon sets stretch the panel: the portrait is exactly the five rows tall and the
        // character exactly the portrait. A level camera at mid-height maps the plane he stands
        // in onto the target with his feet on the bottom edge and the top of his head on the top.
        const float aspect = static_cast<float>(p.width) / static_cast<float>(p.height);
        const float halfExtent = height * 0.5f;
        const float distance = halfExtent / std::tan(kFovY * 0.5f);
        const XMVECTOR target = XMVectorSet(0.f, halfExtent, 0.f, 0.f);
        const XMVECTOR eye = XMVectorSet(0.f, halfExtent, distance, 0.f);
        const XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0.f, 1.f, 0.f, 0.f));
        // Reversed Z, like every other camera in this window: far and near swap places.
        const XMMATRIX proj = XMMatrixPerspectiveFovLH(kFovY, aspect, distance * 4.f,
                                                       std::max(1.f, distance * 0.05f));
        XMFLOAT3 eyeF;
        XMStoreFloat3(&eyeF, eye);
        m_mapRenderer->SetCameraOverride(view, proj, eyeF);
        m_mapRenderer->Update(0);

        // ---- the target -------------------------------------------------------------------
        const D3D11_VIEWPORT vp = { 0.f, 0.f, (float)p.width, (float)p.height, 0.f, 1.f };
        ctx->RSSetViewports(1, &vp);
        const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
        ctx->ClearRenderTargetView(p.rtv.Get(), clear);
        ctx->ClearDepthStencilView(p.dsv.Get(), D3D11_CLEAR_DEPTH, 0.f, 0);
        ID3D11RenderTargetView* rtv = p.rtv.Get();
        ctx->OMSetRenderTargets(1, &rtv, p.dsv.Get());
        ctx->OMSetBlendState(m_portraitBlend.Get(), nullptr, 0xFFFFFFFFu);
        m_mapRenderer->SetDepthStencilState(DepthStencilStateType::Enabled);
        m_mapRenderer->SwitchRasterizerState(RasterizerStateType::Solid);

        // ---- the body, on the portrait's pose ---------------------------------------------
        for (auto& mesh : animState.animMeshes)
            if (mesh) mesh->UpdateBoneMatrices(ctx, *p.controller);

        // A soft body is CPU-written vertices, not a bone upload, so it needs re-seating on this
        // pose by hand - and giving back afterwards.
        if (composed) PortraitPoseSoftBodies(agentId, p.controller->GetBoneMatrices());

        if (composed)
        {
            // A linked headpiece hangs on the head, on the portrait's own idle pose.
            XMFLOAT4X4 linkedWorld{};
            bool haveLinked = false;
            if (PlayerVisualsNeedsAttach(agentId))
            {
                EnsureHeadLink(const_cast<AgentModelInstance&>(tmpl));
                haveLinked = PlayerVisualsHeadAttach(
                    agentId, tmpl.headLinkState == 1 ? tmpl.headLinkBone : -1,
                    tmpl.headLinkOffsetGw, tmpl.headLinkBindDx, p.controller->GetBoneMatrices(),
                    worldF, linkedWorld);
            }
            if (!haveLinked && PlayerVisualsLinkedSubmeshes(agentId) != nullptr)
                XMStoreFloat4x4(&linkedWorld, XMMatrixScaling(0.f, 0.f, 0.f));
            DrawPlayerVisuals(/*secondaryView=*/true, agentId, &worldF,
                              PlayerVisualsLinkedSubmeshes(agentId) != nullptr ? &linkedWorld
                                                                               : nullptr);
            // The same particles as the world view, through the portrait's own attachment.
            if (haveLinked)
                DrawHeadpieceParticles(view, agentId, &linkedWorld);
        }
        else
        {
            // A player the recording has no appearance for: his stand-in, drawn the way
            // DrawSkinnedAgentModels draws it.
            m_mapRenderer->BindSkinnedVertexShader();
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_mapRenderer->BindModelPixelShader(false);
            for (size_t si = 0; si < animState.animMeshes.size(); si++)
            {
                auto& mesh = animState.animMeshes[si];
                if (!mesh) continue;
                if (si < animState.perMeshCBs.size())
                {
                    PerObjectCB cb = animState.perMeshCBs[si];
                    XMStoreFloat4x4(&cb.world, XMMatrixTranspose(world));
                    cb.mesh_alpha = 1.f;
                    cb.highlight_state = 0;
                    meshManager->SetPerObjectCB(cb);
                }
                if (si < animState.perMeshTextureIds.size() &&
                    !animState.perMeshTextureIds[si].empty())
                {
                    auto textures = textureManager->GetTextures(animState.perMeshTextureIds[si]);
                    if (!textures.empty()) mesh->SetTextures(textures, 3);
                }
                mesh->Draw(ctx, m_mapRenderer->GetLODQuality());
            }
            m_mapRenderer->BindRegularVertexShader();
        }

        // ---- the hands: what he holds at this moment, else his first set ------------------
        if (m_showWeaponModels && m_weaponModelsLoaded && tmpl.weaponSocket.resolved)
        {
            SeedWeaponGrips();
            const Equipment::Data& equipment = m_replayCtx.stocData.equipment;
            const Equipment::ItemDef* firstMain =
                p.fallbackMainId ? equipment.FindByAgentItemId(p.fallbackMainId) : nullptr;
            const Equipment::ItemDef* firstOff =
                p.fallbackOffId ? equipment.FindByAgentItemId(p.fallbackOffId) : nullptr;

            PerObjectCB agentCB = animState.perMeshCBs.empty() ? PerObjectCB{}
                                                               : animState.perMeshCBs[0];
            agentCB.world = worldF;
            agentCB.mesh_alpha = 1.f;
            agentCB.highlight_state = 0;

            bool shadersBound = false;
            int boundPixelShader = -1;
            DrawHeldWeapons(agentId, ard, FindSnapshotIndex(ard.snapshots, m_debugTimeline),
                            tmpl.weaponSocket, *p.controller, agentCB, shadersBound,
                            boundPixelShader, firstMain, firstOff);
        }

        // ---- hand the submeshes back their live pose ---------------------------------------
        for (auto& mesh : animState.animMeshes)
            if (mesh) mesh->UpdateBoneMatrices(ctx, *animState.controller);
        if (composed) PortraitRestoreSoftBodies(agentId);

        p.rendered = true;
    }

    // ---- restore ------------------------------------------------------------------------------
    m_mapRenderer->ClearCameraOverride();
    m_mapRenderer->SetDirectionalLight(savedLight);
    m_mapRenderer->SetShouldRenderFog(savedFog);
    m_mapRenderer->SetShouldRenderShadowsForModels(savedModelShadows);
    m_mapRenderer->SetShouldRerenderShadows(savedRerenderShadows);
    m_mapRenderer->Update(0);

    ctx->OMSetRenderTargets(1, savedRTV.GetAddressOf(), savedDSV.Get());
    ctx->RSSetViewports(1, &savedVP);
    ctx->OMSetBlendState(savedBlend.Get(), savedBlendFactor, savedSampleMask);
    ctx->OMSetDepthStencilState(savedDepth.Get(), savedStencilRef);
    m_mapRenderer->SwitchRasterizerState(savedRasterizer);
}
