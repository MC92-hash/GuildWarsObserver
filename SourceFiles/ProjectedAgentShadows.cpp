#include "pch.h"
#include "ProjectedAgentShadows.h"
#include "Terrain.h"
#include "AnimatedMeshInstance.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {
const char shader[] = R"(
cbuffer Frame : register(b0) { float4x4 viewProj; };
Texture2D maskTexture : register(t0);
SamplerState maskSampler : register(s0);
struct V { float3 p : POSITION; float2 uv : TEXCOORD0; float opacity : TEXCOORD1; };
struct P { float4 p : SV_Position; float2 uv : TEXCOORD0; float opacity : TEXCOORD1; };
P VSMain(V i) {
    P o; o.p = mul(float4(i.p, 1), viewProj); o.uv = i.uv; o.opacity = i.opacity; return o;
}
float4 PSMain(P i) : SV_Target0 {
    clip(min(min(i.uv.x, i.uv.y), min(1-i.uv.x, 1-i.uv.y)));
    // The original fallback is an RGB mask with opaque alpha.
    // Destination multiplication preserves the receiver's lighting and texture.
    return float4(saturate(maskTexture.Sample(maskSampler, i.uv).rgb * i.opacity), 0);
}
)";

// Save every state this pass changes, including the SRV and sampler. Render targets,
// viewport, index buffer, and constant-buffer contents are not modified.
struct SavedState {
    ID3D11DeviceContext* c;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11InputLayout> il;
    ComPtr<ID3D11Buffer> vb, cb;
    ComPtr<ID3D11Buffer> ib, bones;
    DXGI_FORMAT ibFormat{}; UINT ibOffset=0;
    std::array<ComPtr<ID3D11ShaderResourceView>, 10> resources;
    std::array<ComPtr<ID3D11RenderTargetView>, 8> targets;
    ComPtr<ID3D11DepthStencilView> dsv;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewportCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> raster;
    UINT stride = 0, offset = 0, stencil = 0, sampleMask = 0;
    float factor[4]{};
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    explicit SavedState(ID3D11DeviceContext* ctx) : c(ctx) {
        c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr); c->IAGetInputLayout(&il);
        c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset); c->IAGetPrimitiveTopology(&topology);
        c->VSGetConstantBuffers(0, 1, &cb); c->PSGetShaderResources(0, 1, &srv);
        c->PSGetSamplers(0, 1, &sampler); c->RSGetState(&raster);
        c->OMGetBlendState(&blend, factor, &sampleMask); c->OMGetDepthStencilState(&depth, &stencil);
        c->IAGetIndexBuffer(&ib,&ibFormat,&ibOffset); c->VSGetConstantBuffers(3,1,&bones);
        ID3D11ShaderResourceView* srvs[10]{}; c->PSGetShaderResources(0,10,srvs);
        for (int i=0;i<10;++i) resources[i].Attach(srvs[i]);
        ID3D11RenderTargetView* rtvs[8]{}; c->OMGetRenderTargets(8,rtvs,&dsv);
        for (int i=0;i<8;++i) targets[i].Attach(rtvs[i]);
        c->RSGetViewports(&viewportCount,viewports);
    }
    ~SavedState() {
        c->VSSetShader(vs.Get(), nullptr, 0); c->PSSetShader(ps.Get(), nullptr, 0);
        c->GSSetShader(gs.Get(), nullptr, 0); c->IASetInputLayout(il.Get());
        c->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset); c->IASetPrimitiveTopology(topology);
        c->VSSetConstantBuffers(0, 1, cb.GetAddressOf()); c->PSSetShaderResources(0, 1, srv.GetAddressOf());
        c->PSSetSamplers(0, 1, sampler.GetAddressOf()); c->RSSetState(raster.Get());
        c->OMSetBlendState(blend.Get(), factor, sampleMask); c->OMSetDepthStencilState(depth.Get(), stencil);
        c->IASetIndexBuffer(ib.Get(),ibFormat,ibOffset); c->VSSetConstantBuffers(3,1,bones.GetAddressOf());
        ID3D11RenderTargetView* rtvs[8]{}; for(int i=0;i<8;++i) rtvs[i]=targets[i].Get();
        c->OMSetRenderTargets(8,rtvs,dsv.Get()); c->RSSetViewports(viewportCount,viewports);
        ID3D11ShaderResourceView* srvs[10]{}; for(int i=0;i<10;++i) srvs[i]=resources[i].Get();
        c->PSSetShaderResources(0,10,srvs);
    }
};
}

bool ProjectedAgentShadows::Initialize(ID3D11Device* device, const void* rgba, unsigned width, unsigned height) {
    if (Ready()) return true;
    if (!device || !rgba || !width || !height || width > 4096 || height > 4096) {
        error_ = "Shadow mask unavailable"; return false;
    }
    // Commit only a complete resource set, so failed initialization can be retried.
    ProjectedAgentShadows next;
    auto check = [&](HRESULT hr) { if (FAILED(hr)) error_ = "Shadow resource creation failed: " + std::to_string(hr); return SUCCEEDED(hr); };
    ComPtr<ID3DBlob> vs, ps, errors;
    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        const HRESULT hr = D3DCompile(shader, sizeof(shader), "ProjectedAgentShadows", nullptr, nullptr,
            entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &out, &errors);
        if (FAILED(hr)) error_ = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "Shadow shader compile failed";
        return SUCCEEDED(hr);
    };
    if (!compile("VSMain", "vs_5_0", vs) || !compile("PSMain", "ps_5_0", ps)) return false;
    if (!check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &next.vs_)) ||
        !check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &next.ps_))) return false;
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",1,DXGI_FORMAT_R32_FLOAT,0,20,D3D11_INPUT_PER_VERTEX_DATA,0}
    };
    if (!check(device->CreateInputLayout(elements, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &next.layout_))) return false;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(XMFLOAT4X4); cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!check(device->CreateBuffer(&cb, nullptr, &next.constants_))) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{rgba, width * 4, 0}; ComPtr<ID3D11Texture2D> texture;
    if (!check(device->CreateTexture2D(&td, &data, &texture)) ||
        !check(device->CreateShaderResourceView(texture.Get(), nullptr, &next.mask_))) return false;
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS; sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (!check(device->CreateSamplerState(&sd, &next.sampler_))) return false;
    D3D11_BLEND_DESC bd{}; bd.IndependentBlendEnable = TRUE;
    auto& rt = bd.RenderTarget[0]; rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ZERO; rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO; rt.DestBlendAlpha = D3D11_BLEND_ONE; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    // Other render targets (notably picking IDs) have zero write masks.
    if (!check(device->CreateBlendState(&bd, &next.blend_))) return false;
    D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL; // Observer uses reversed Z.
    if (!check(device->CreateDepthStencilState(&dd, &next.depth_))) return false;
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    if (!check(device->CreateRasterizerState(&rd, &next.raster_))) return false;
    *this = std::move(next); return true;
}

bool ProjectedAgentShadows::Capture(ID3D11DeviceContext* ctx, unsigned slot, Caster& caster,
    XMFLOAT2 shift, const std::vector<Part>& parts) {
    if (!Ready() || slot>=masks_.size() || parts.empty() || caster.height<=0 || caster.height>200) return false;
    ComPtr<ID3D11Device> device; ctx->GetDevice(&device);
    struct CaptureConstants { XMFLOAT4X4 world; XMFLOAT4 area; XMFLOAT4 projection; };
    if (!captureVS_) {
        const char source[]=R"(
cbuffer Capture : register(b0) { float4x4 world; float4 area; float4 projection; };
cbuffer Bones : register(b3) { float4x4 bones[256]; };
struct V { float3 p:POSITION; uint4 indices:BLENDINDICES; float4 weights:BLENDWEIGHT; };
float4 VSMain(V v):SV_Position {
    float4 p=0;
    [unroll] for(uint i=0;i<4;++i) p+=mul(float4(v.p,1),bones[min(v.indices[i],255)])*v.weights[i];
    if (p.w==0) p=float4(v.p,1);
    p=mul(p,world);
    float2 ground=p.xz+projection.xy*(p.y-projection.z);
    return float4((ground-area.xy)/area.z * float2(1,-1),0.5,1);
}
float4 PSMain():SV_Target { return 1; }
)";
        ComPtr<ID3DBlob> vs,ps,errors;
        if (FAILED(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"VSMain","vs_5_0",0,0,&vs,&errors)) ||
            FAILED(D3DCompile(source,sizeof(source),nullptr,nullptr,nullptr,"PSMain","ps_5_0",0,0,&ps,&errors))) {
            error_="Silhouette shader compilation failed"; return false;
        }
        ComPtr<ID3D11VertexShader> newVS; ComPtr<ID3D11PixelShader> newPS;
        ComPtr<ID3D11InputLayout> layout; ComPtr<ID3D11Buffer> constants; ComPtr<ID3D11DepthStencilState> depth;
        D3D11_INPUT_ELEMENT_DESC elements[]={
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"BLENDINDICES",0,DXGI_FORMAT_R32G32B32A32_UINT,0,sizeof(GWVertex),D3D11_INPUT_PER_VERTEX_DATA,0},
            {"BLENDWEIGHT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,sizeof(GWVertex)+16,D3D11_INPUT_PER_VERTEX_DATA,0}};
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth=sizeof(CaptureConstants); bd.Usage=D3D11_USAGE_DEFAULT; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable=FALSE; dd.DepthFunc=D3D11_COMPARISON_ALWAYS;
        if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&newVS)) ||
            FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&newPS)) ||
            FAILED(device->CreateInputLayout(elements,3,vs->GetBufferPointer(),vs->GetBufferSize(),&layout)) ||
            FAILED(device->CreateBuffer(&bd,nullptr,&constants)) || FAILED(device->CreateDepthStencilState(&dd,&depth))) {
            error_="Silhouette resources unavailable"; return false;
        }
        captureVS_=newVS; capturePS_=newPS; captureLayout_=layout; captureCB_=constants; captureDepth_=depth;
    }
    if (!masks_[slot]) {
        D3D11_TEXTURE2D_DESC td{}; td.Width=td.Height=256; td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&td,nullptr,&texture)) ||
            FAILED(device->CreateRenderTargetView(texture.Get(),nullptr,maskTargets_[slot].ReleaseAndGetAddressOf())) ||
            FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,masks_[slot].ReleaseAndGetAddressOf()))) {
            error_="Silhouette texture allocation failed"; return false;
        }
    }
    SavedState saved(ctx);
    ID3D11ShaderResourceView* nulls[10]{}; ctx->PSSetShaderResources(0,10,nulls);
    ctx->OMSetRenderTargets(1,maskTargets_[slot].GetAddressOf(),nullptr);
    const float clear[4]{}; ctx->ClearRenderTargetView(maskTargets_[slot].Get(),clear);
    D3D11_VIEWPORT vp{0,0,256,256,0,1}; ctx->RSSetViewports(1,&vp);
    ctx->OMSetBlendState(nullptr,nullptr,UINT_MAX); ctx->OMSetDepthStencilState(captureDepth_.Get(),0);
    ctx->RSSetState(raster_.Get()); ctx->IASetInputLayout(captureLayout_.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(captureVS_.Get(),nullptr,0); ctx->PSSetShader(capturePS_.Get(),nullptr,0); ctx->GSSetShader(nullptr,nullptr,0);
    ctx->VSSetConstantBuffers(0,1,captureCB_.GetAddressOf());
    caster.projectionShift=shift;
    caster.maskCenter={caster.position.x+shift.x*caster.height*0.5f,caster.position.z+shift.y*caster.height*0.5f};
    caster.maskExtent=caster.radius/0.75f + 20.f + std::max(std::abs(shift.x),std::abs(shift.y))*caster.height*0.5f;
    for (const auto& part:parts) {
        if (!part.mesh) continue;
        CaptureConstants cb{}; XMStoreFloat4x4(&cb.world,XMMatrixTranspose(XMLoadFloat4x4(&part.world)));
        cb.area={caster.maskCenter.x,caster.maskCenter.y,caster.maskExtent,0};
        cb.projection={shift.x,shift.y,caster.position.y,0};
        ctx->UpdateSubresource(captureCB_.Get(),0,nullptr,&cb,0,0); part.mesh->Draw(ctx,LODQuality::High);
    }
    caster.maskIndex=static_cast<int>(slot); return true;
}

float ProjectedAgentShadows::DistanceOpacity(float distance, bool originalDistanceLimit) {
    if (!std::isfinite(distance) || distance < 0) return 0.f;
    // Observer cameras can be 2,000--10,000 units from visible actors. Applying
    // the original close-camera limit here would discard every overview shadow.
    return originalDistanceLimit ? std::clamp((1500.f-distance)/500.f, 0.f, 1.f) : 1.f;
}

void ProjectedAgentShadows::Draw(ID3D11DeviceContext* ctx, const Terrain& terrain,
    FXMMATRIX viewProj, const std::vector<Caster>& casters, float strength) {
    ResetStats(); staging_.clear();
    if (!Ready() || !ctx || !std::isfinite(strength) || strength <= 0) return;
    const auto& grid = terrain.get_heightmap_grid(); const auto& b = terrain.m_bounds;
    const int nx = static_cast<int>(terrain.m_grid_dim_x), nz = static_cast<int>(terrain.m_grid_dim_z);
    if (nx < 2 || nz < 2 || grid.size() < static_cast<size_t>(nz + 1)) return;
    const float dx = (b.map_max_x - b.map_min_x) / nx, dz = (b.map_max_z - b.map_min_z) / nz;
    if (!(dx > 0) || !(dz > 0)) return;
    struct Batch { UINT start, count; int maskIndex; };
    std::vector<Batch> batches;
    for (const auto& caster : casters) {
        const auto& p = caster.position;
        const bool detailed=caster.maskIndex>=0 && caster.maskIndex<32 && masks_[caster.maskIndex];
        const float r = detailed ? caster.maskExtent : caster.radius;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            !std::isfinite(r) || r <= 0 || r > 512 || !std::isfinite(caster.opacity) || caster.opacity <= 0) continue;
        if (p.x < b.map_min_x || p.x >= b.map_max_x || p.z < b.map_min_z || p.z >= b.map_max_z) continue;
        // Layered/model receivers are not mapped yet. Avoid dropping an elevated
        // actor's shadow through a bridge onto unrelated terrain below it.
        // Compare against the same triangle plane we draw, not the terrain
        // helper's bilinear surface (which can disagree on uneven cells).
        const float gx = (p.x-b.map_min_x)/dx, gz = (p.z-b.map_min_z)/dz;
        const int cx = std::clamp(static_cast<int>(gx), 0, nx-1);
        const int cz = std::clamp(static_cast<int>(gz), 0, nz-1);
        const float fx = gx-cx, fz = gz-cz;
        const float receiverHeight = fz >= fx
            ? grid[cz][cx]*(1-fz) + grid[cz+1][cx]*(fz-fx) + grid[cz+1][cx+1]*fx
            : grid[cz][cx]*(1-fx) + grid[cz+1][cx+1]*fz + grid[cz][cx+1]*(fx-fz);
        if (std::abs(receiverHeight - p.y) > 24.f) { ++receiverSkipCount_; continue; }
        const float centerX=detailed ? caster.maskCenter.x : p.x, centerZ=detailed ? caster.maskCenter.y : p.z;
        const int x0 = std::clamp(static_cast<int>(std::floor((centerX-r-b.map_min_x)/dx)), 0, nx-1);
        const int x1 = std::clamp(static_cast<int>(std::floor((centerX+r-b.map_min_x)/dx)), 0, nx-1);
        const int z0 = std::clamp(static_cast<int>(std::floor((centerZ-r-b.map_min_z)/dz)), 0, nz-1);
        const int z1 = std::clamp(static_cast<int>(std::floor((centerZ+r-b.map_min_z)/dz)), 0, nz-1);
        const size_t start = staging_.size();
        auto append = [&](int x, int z) {
            const float wx = b.map_min_x + x*dx, wz = b.map_min_z + z*dz;
            const float h=grid[z][x]-p.y;
            const float u=wx+(detailed ? caster.projectionShift.x*h : 0.f);
            const float v=wz+(detailed ? caster.projectionShift.y*h : 0.f);
            staging_.push_back({{wx, grid[z][x] + 0.5f, wz},
                {(u-centerX)/(2*r)+0.5f, (v-centerZ)/(2*r)+0.5f}, std::clamp(caster.opacity*strength, 0.f, 1.f)});
        };
        for (int z=z0; z<=z1; ++z) for (int x=x0; x<=x1; ++x) {
            // Same diagonal as Terrain::GenerateTerrainMesh; no bilinear patch.
            append(x,z); append(x,z+1); append(x+1,z+1);
            append(x,z); append(x+1,z+1); append(x+1,z);
        }
        if (staging_.size() != start) { ++drawnCount_; batches.push_back({static_cast<UINT>(start),static_cast<UINT>(staging_.size()-start), detailed ? caster.maskIndex : -1}); }
    }
    if (staging_.empty()) return;
    if (staging_.size() > capacity_) {
        ComPtr<ID3D11Device> device; ctx->GetDevice(&device);
        const size_t capacity = staging_.size() + 1024;
        if (capacity > UINT_MAX / sizeof(Vertex)) return;
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = static_cast<UINT>(capacity * sizeof(Vertex));
        bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> buffer;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &buffer))) { error_ = "Shadow vertex allocation failed"; return; }
        vertices_ = std::move(buffer); capacity_ = capacity;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(vertices_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, staging_.data(), staging_.size()*sizeof(Vertex)); ctx->Unmap(vertices_.Get(), 0);
    if (FAILED(ctx->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    XMStoreFloat4x4(static_cast<XMFLOAT4X4*>(mapped.pData), XMMatrixTranspose(viewProj)); ctx->Unmap(constants_.Get(), 0);
    SavedState saved(ctx);
    const UINT stride=sizeof(Vertex), offset=0;
    ctx->IASetInputLayout(layout_.Get()); ctx->IASetVertexBuffers(0, 1, vertices_.GetAddressOf(), &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0); ctx->PSSetShader(ps_.Get(), nullptr, 0); ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, constants_.GetAddressOf()); ctx->PSSetShaderResources(0, 1, mask_.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf()); ctx->RSSetState(raster_.Get());
    ctx->OMSetBlendState(blend_.Get(), nullptr, UINT_MAX); ctx->OMSetDepthStencilState(depth_.Get(), 0);
    for (const auto& batch:batches) {
        ID3D11ShaderResourceView* texture=batch.maskIndex>=0 ? masks_[batch.maskIndex].Get() : mask_.Get();
        ctx->PSSetShaderResources(0,1,&texture); ctx->Draw(batch.count,batch.start);
    }
}
