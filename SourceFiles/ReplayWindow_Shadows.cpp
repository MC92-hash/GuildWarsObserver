#include "pch.h"
#include "ReplayWindow.h"

void ReplayWindow::DrawAgentShadows()
{
    m_agentShadows.ResetStats();
    m_silhouetteShadowCount = 0;
    if (!m_showAgentShadows || !m_useAgentModels || !m_showAgentOverlay || m_shadowCasters.empty()) return;
    auto* terrain = m_mapRenderer->GetTerrain();
    auto* camera = m_mapRenderer->GetCamera();
    if (!terrain || !camera) return;
    if (!m_agentShadows.Ready() && !m_shadowInitAttempted) {
        // Load from this replay's DAT, never from a developer-machine asset path.
        m_shadowInitAttempted = true;
        if (!m_datManager || !m_hashIndex) { m_shadowLoadError = "Shadow DAT unavailable"; return; }
        const auto it = m_hashIndex->find(9087);
        if (it == m_hashIndex->end() || it->second.empty()) { m_shadowLoadError = "Shadow mask 9087 is missing from this DAT"; return; }
        const auto texture = m_datManager->parse_ffna_texture_file(it->second.front());
        if (texture.width <= 0 || texture.height <= 0 ||
            texture.rgba_data.size() < static_cast<size_t>(texture.width) * texture.height) {
            m_shadowLoadError = "Shadow mask 9087 could not be decoded"; return;
        }
        m_agentShadows.Initialize(m_deviceResources->GetD3DDevice(), texture.rgba_data.data(), texture.width, texture.height);
    }
    if (m_showSilhouetteShadows && m_agentShadows.Ready()) {
        auto direction=m_mapRenderer->GetDirectionalLight().direction;
        const float horizontal=std::hypot(direction.x,direction.z);
        // Clamp elevation to the range recovered from AvShadow. Map azimuth is
        // used here pending exact native direction parity.
        const float cot=std::clamp(horizontal/std::max(std::abs(direction.y),0.001f),0.08749f,1.191754f);
        const XMFLOAT2 shift=horizontal>0.001f ? XMFLOAT2{direction.x/horizontal*cot,direction.z/horizontal*cot} : XMFLOAT2{cot,0};
        const auto eye=camera->GetPosition3f();
        auto distance=[&](const auto& c) { const auto& p=c.position; return (p.x-eye.x)*(p.x-eye.x)+(p.y-eye.y)*(p.y-eye.y)+(p.z-eye.z)*(p.z-eye.z); };
        std::stable_sort(m_shadowCasters.begin(),m_shadowCasters.end(),[&](const auto& a,const auto& b) {
            const float da=distance(a),db=distance(b); return da==db ? a.modelSlot<b.modelSlot : da<db;
        });
        for (auto& caster:m_shadowCasters) {
            if (m_silhouetteShadowCount>=32) break;
            const auto found=m_agentAnimStates.find(caster.modelSlot);
            if (found==m_agentAnimStates.end() || !found->second.hasSkinning) continue;
            std::vector<ProjectedAgentShadows::Part> parts;
            auto& state=found->second;
            for (size_t i=0;i<state.animMeshes.size() && i<state.perMeshCBs.size();++i)
                if (state.animMeshes[i]) parts.push_back({state.animMeshes[i].get(),state.perMeshCBs[i].world});
            if (m_agentShadows.Capture(m_deviceResources->GetD3DDeviceContext(),m_silhouetteShadowCount,caster,shift,parts))
                ++m_silhouetteShadowCount;
        }
    }
    m_agentShadows.Draw(m_deviceResources->GetD3DDeviceContext(), *terrain,
        camera->GetView() * camera->GetProj(), m_shadowCasters, m_shadowStrength);
}

void ReplayWindow::RenderTerrainShadows()
{
    if (!m_mapRenderer || !m_mapRenderer->GetTerrain()) return;
    m_mapRenderer->SetShouldRenderShadows(m_showTerrainShadows);
    m_mapRenderer->SetShouldRenderShadowsForModels(m_showTerrainShadows);
    if (!m_showTerrainShadows) { m_mapRenderer->Update(0); return; }
    if (m_deviceResources->GetShadowMapSRV() && !m_mapRenderer->GetShouldRerenderShadows()) return;

    auto* ctx=m_deviceResources->GetD3DDeviceContext();
    auto* meshes=m_mapRenderer->GetMeshManager();
    auto* terrain=m_mapRenderer->GetTerrain();
    const auto& b=terrain->m_bounds;
    const XMVECTOR center=XMVectorSet((b.map_min_x+b.map_max_x)*0.5f,(b.map_min_y+b.map_max_y)*0.5f,(b.map_min_z+b.map_max_z)*0.5f,1);
    auto direction=m_mapRenderer->GetDirectionalLight().direction;
    XMVECTOR dir=XMLoadFloat3(&direction);
    if (XMVectorGetX(XMVector3LengthSq(dir))<0.001f) dir=XMVectorSet(0.6f,-0.8f,0,0);
    dir=XMVector3Normalize(dir);
    const XMVECTOR eye=center-dir*100000.f;
    const XMVECTOR up=std::abs(XMVectorGetY(dir))>0.98f ? XMVectorSet(0,0,1,0) : XMVectorSet(0,1,0,0);
    const XMMATRIX view=XMMatrixLookAtLH(eye,center,up);
    XMFLOAT3 lo{FLT_MAX,FLT_MAX,FLT_MAX},hi{-FLT_MAX,-FLT_MAX,-FLT_MAX};
    for(int i=0;i<8;++i) {
        XMFLOAT3 p; XMStoreFloat3(&p,XMVector3TransformCoord(XMVectorSet(i&1?b.map_max_x:b.map_min_x,
            i&2?b.map_max_y+2000.f:b.map_min_y-100.f,i&4?b.map_max_z:b.map_min_z,1),view));
        lo.x=std::min(lo.x,p.x); lo.y=std::min(lo.y,p.y); lo.z=std::min(lo.z,p.z);
        hi.x=std::max(hi.x,p.x); hi.y=std::max(hi.y,p.y); hi.z=std::max(hi.z,p.z);
    }
    // Reverse-Z, including an off-centre light-space box for maps away from origin.
    const XMMATRIX proj=XMMatrixOrthographicOffCenterLH(lo.x-10,hi.x+10,lo.y-10,hi.y+10,hi.z+10,lo.z-10);
    auto savedCamera=m_mapRenderer->GetPerCameraCB();
    XMStoreFloat4x4(&savedCamera.directional_light_view,XMMatrixTranspose(view));
    XMStoreFloat4x4(&savedCamera.directional_light_proj,XMMatrixTranspose(proj));
    savedCamera.shadowmap_texel_size_x=savedCamera.shadowmap_texel_size_y=1.f/4096.f;
    m_deviceResources->CreateShadowResources(4096,4096);
    auto terrainMesh=meshes->GetMesh(m_mapRenderer->GetTerrainMeshId());
    if (!terrainMesh) return;
    terrainMesh->SetTextures({nullptr},3);
    for(const auto& [key,ids]:m_mapRenderer->GetPropsMeshIds())
        for(int id:ids) if(auto mesh=meshes->GetMesh(id)) mesh->SetTextures({nullptr},0);
    ID3D11ShaderResourceView* empty[16]{}; ctx->PSSetShaderResources(0,16,empty);
    std::vector<std::pair<int,bool>> visibility;
    for(const auto& [slot,ids]:m_agentMeshIds) for(int id:ids) {
        visibility.emplace_back(id,meshes->GetMeshShouldRender(id)); meshes->SetMeshShouldRender(id,false);
    }
    auto lightCamera=savedCamera;
    lightCamera.view=savedCamera.directional_light_view; lightCamera.projection=savedCamera.directional_light_proj;
    XMStoreFloat3(&lightCamera.position,eye);
    m_mapRenderer->UploadRenderCamera(lightCamera);
    meshes->SetViewProjMatrix(view*proj);
    auto shadowVP=m_deviceResources->GetShadowViewport(); ctx->RSSetViewports(1,&shadowVP);
    ctx->ClearDepthStencilView(m_deviceResources->GetShadowMapDSV(),D3D11_CLEAR_DEPTH,0,0);
    m_mapRenderer->RenderForShadowMap(m_deviceResources->GetShadowMapDSV());
    ctx->OMSetRenderTargets(0,nullptr,nullptr);
    for(const auto& [id,visible]:visibility) meshes->SetMeshShouldRender(id,visible);
    auto srv=m_deviceResources->GetShadowMapSRV(); terrainMesh->SetTextures({srv},3);
    for(const auto& [key,ids]:m_mapRenderer->GetPropsMeshIds())
        for(int id:ids) if(auto mesh=meshes->GetMesh(id)) mesh->SetTextures({srv},0);
    m_mapRenderer->UploadRenderCamera(savedCamera);
    meshes->SetViewProjMatrix(m_mapRenderer->GetCamera()->GetView()*m_mapRenderer->GetCamera()->GetProj());
    auto viewport=m_deviceResources->GetScreenViewport(); ctx->RSSetViewports(1,&viewport);
    m_mapRenderer->SetShouldRerenderShadows(false);
    m_mapRenderer->Update(0);
}
