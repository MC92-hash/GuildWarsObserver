#include "pch.h"
#include "HeatmapRenderer.h"
#include "Terrain.h"
#include <d3dcompiler.h>
#include <cmath>
#include <algorithm>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Embedded HLSL (Shader Model 5.0) — simplified, no team tint
// ---------------------------------------------------------------------------
static const char kHeatmapHLSL[] = R"(
cbuffer HeatmapCB : register(b0)
{
    float4x4 viewProj;
    float    opacity;
    float3   pad;
};

Texture2D<float>  densityTex : register(t0);
Texture2D<float4> lutTex     : register(t1);
SamplerState      samp       : register(s0);

struct VS_IN  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VS_OUT VSMain(VS_IN i)
{
    VS_OUT o;
    o.pos = mul(float4(i.pos, 1), viewProj);
    o.uv  = i.uv;
    return o;
}

float4 PSMain(VS_OUT i) : SV_Target
{
    float d = densityTex.Sample(samp, i.uv);
    if (d < 0.005) discard;

    float4 col = lutTex.Sample(samp, float2(saturate(d), 0.5));
    col.a *= opacity;
    return col;
}
)";

// ---------------------------------------------------------------------------
// Palette stop definitions
// ---------------------------------------------------------------------------
struct ColorStop { float t; uint8_t r, g, b; };

static void BakeLUT(const ColorStop* stops, int numStops, uint8_t* out)
{
    for (int i = 0; i < 256; i++)
    {
        float t = i / 255.0f;

        const ColorStop* lo = &stops[0];
        const ColorStop* hi = &stops[numStops - 1];
        for (int s = 0; s < numStops - 1; s++)
        {
            if (t >= stops[s].t && t <= stops[s + 1].t)
            {
                lo = &stops[s];
                hi = &stops[s + 1];
                break;
            }
        }

        float range = hi->t - lo->t;
        float f = (range > 0.0f) ? (t - lo->t) / range : 0.0f;

        out[i * 4 + 0] = static_cast<uint8_t>(lo->r + f * (hi->r - lo->r));
        out[i * 4 + 1] = static_cast<uint8_t>(lo->g + f * (hi->g - lo->g));
        out[i * 4 + 2] = static_cast<uint8_t>(lo->b + f * (hi->b - lo->b));
        out[i * 4 + 3] = static_cast<uint8_t>(std::min(1.0f, t / 0.15f) * 255.0f);
    }
}

void HeatmapRenderer::CreateAllLUTs(ID3D11Device* device)
{
    static const ColorStop sThermal[] = {
        { 0.00f,   0,   0,   0 }, { 0.15f,   0,   0, 150 },
        { 0.35f,   0, 180, 255 }, { 0.55f,   0, 255, 140 },
        { 0.72f, 255, 238,   0 }, { 0.87f, 255, 120,   0 },
        { 1.00f, 255,  34,   0 },
    };
    static const ColorStop sInferno[] = {
        { 0.00f,   0,   0,   0 }, { 0.15f,  20,   5,  40 },
        { 0.35f,  90,  15, 100 }, { 0.55f, 220,  50, 100 },
        { 0.72f, 254, 130,  50 }, { 0.87f, 252, 200, 120 },
        { 1.00f, 252, 253, 190 },
    };
    static const ColorStop sViridis[] = {
        { 0.00f,   0,   0,   0 }, { 0.15f,  68,   1,  84 },
        { 0.35f,  59,  82, 139 }, { 0.55f,  33, 145, 140 },
        { 0.72f,  94, 201,  98 }, { 0.87f, 180, 222,  44 },
        { 1.00f, 253, 231,  37 },
    };
    static const ColorStop sTeamBlue[] = {
        { 0.00f,   0,   0,   0 }, { 0.15f,   5,  20,  80 },
        { 0.35f,  13,  71, 161 }, { 0.55f,  25, 118, 210 },
        { 0.72f,  66, 165, 245 }, { 0.87f, 144, 202, 249 },
        { 1.00f, 255, 255, 255 },
    };
    static const ColorStop sTeamRed[] = {
        { 0.00f,   0,   0,   0 }, { 0.15f,  80,   5,   5 },
        { 0.35f, 161,  13,  13 }, { 0.55f, 211,  47,  47 },
        { 0.72f, 239, 116,  94 }, { 0.87f, 248, 187, 181 },
        { 1.00f, 255, 255, 255 },
    };
    static const ColorStop sDominance[] = {
        { 0.00f,   0,   0,   0 },
        { 0.08f,  20,  60, 200 },
        { 0.15f,  40, 120, 255 },
        { 0.30f, 120, 190, 255 },
        { 0.42f, 200, 230, 255 },
        { 0.50f, 255, 255, 210 },
        { 0.58f, 255, 220, 180 },
        { 0.70f, 255, 130,  70 },
        { 0.85f, 240,  40,  20 },
        { 1.00f, 160,   0,   0 },
    };
    static const ColorStop sLava[] = {
        { 0.00f,   0,   0,   0 }, { 0.15f,  74,   0,   0 },
        { 0.35f, 170,  17,   0 }, { 0.55f, 255,  68,   0 },
        { 0.72f, 255, 153,   0 }, { 0.87f, 255, 238,   0 },
        { 1.00f, 255, 255, 255 },
    };
    static const ColorStop sSunset[] = {
        { 0.00f,  26,   0,  48 }, { 0.15f,  90,   0,  96 },
        { 0.35f, 204,   0, 102 }, { 0.55f, 255, 102,   0 },
        { 0.72f, 255, 204,   0 }, { 1.00f, 255, 255, 170 },
    };
    static const ColorStop sAmber[] = {
        { 0.00f,   0,   0,   0 }, { 0.15f,  42,  10,   0 },
        { 0.35f, 120,  40,   0 }, { 0.55f, 210,  95,   0 },
        { 0.72f, 255, 165,   0 }, { 0.87f, 255, 215,  80 },
        { 1.00f, 255, 248, 200 },
    };

    struct PaletteTable { const ColorStop* stops; int count; };
    static const PaletteTable tables[] = {
        { sThermal,   _countof(sThermal)   },
        { sInferno,   _countof(sInferno)   },
        { sViridis,   _countof(sViridis)   },
        { sTeamBlue,  _countof(sTeamBlue)  },
        { sTeamRed,   _countof(sTeamRed)   },
        { sDominance, _countof(sDominance) },
        { sLava,      _countof(sLava)      },
        { sSunset,    _countof(sSunset)    },
        { sAmber,     _countof(sAmber)     },
    };
    static_assert(_countof(tables) == kNumPalettes, "palette table count mismatch");

    for (int p = 0; p < kNumPalettes; ++p)
    {
        uint8_t pixels[256 * 4];
        BakeLUT(tables[p].stops, tables[p].count, pixels);

        D3D11_TEXTURE2D_DESC td = {};
        td.Width     = 256;
        td.Height    = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage     = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem     = pixels;
        init.SysMemPitch = 256 * 4;

        device->CreateTexture2D(&td, &init, m_lutTex[p].GetAddressOf());
        device->CreateShaderResourceView(m_lutTex[p].Get(), nullptr,
                                          m_lutSRV[p].GetAddressOf());
    }
}

// ---------------------------------------------------------------------------
// Shader compilation
// ---------------------------------------------------------------------------
void HeatmapRenderer::CompileShaders(ID3D11Device* device)
{
    ComPtr<ID3DBlob> vsBlob, psBlob, err;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif

    HRESULT hr = D3DCompile(kHeatmapHLSL, sizeof(kHeatmapHLSL), "HeatmapVS",
                            nullptr, nullptr, "VSMain", "vs_5_0", flags, 0,
                            vsBlob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) return;

    hr = D3DCompile(kHeatmapHLSL, sizeof(kHeatmapHLSL), "HeatmapPS",
                    nullptr, nullptr, "PSMain", "ps_5_0", flags, 0,
                    psBlob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) return;

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                               nullptr, m_vs.GetAddressOf());
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                              nullptr, m_ps.GetAddressOf());

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(),
                              vsBlob->GetBufferSize(), m_il.GetAddressOf());
}

// ---------------------------------------------------------------------------
// Init — one-time GPU resource creation
// ---------------------------------------------------------------------------
bool HeatmapRenderer::Init(ID3D11Device* device)
{
    CompileShaders(device);
    if (!m_vs || !m_ps) return false;

    CreateAllLUTs(device);

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(HeatmapCB);
    cbd.Usage           = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cbd, nullptr, m_cb.GetAddressOf());

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable    = TRUE;
    bld.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bld, m_blendState.GetAddressOf());

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable    = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc      = D3D11_COMPARISON_GREATER_EQUAL;
    device->CreateDepthStencilState(&dsd, m_dss.GetAddressOf());

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.DepthBias       = 100;
    rd.SlopeScaledDepthBias = 1.0f;
    device->CreateRasterizerState(&rd, m_rs.GetAddressOf());

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sd, m_sampler.GetAddressOf());

    return true;
}

// ---------------------------------------------------------------------------
// Build terrain-conform grid mesh
// Each vertex samples the max terrain height in its neighborhood to prevent
// terrain peaks between vertices from clipping through the mesh.
// ---------------------------------------------------------------------------
void HeatmapRenderer::BuildMesh(
    ID3D11Device* device, const Terrain* terrain,
    float minX, float maxX, float minZ, float maxZ,
    float waterLevel)
{
    const int N = kGridRes;
    const int vertCount = (N + 1) * (N + 1);
    const int quadCount = N * N;

    std::vector<HeatmapVertex> verts(vertCount);
    std::vector<uint32_t> indices(quadCount * 6);

    float stepX = (maxX - minX) / N;
    float stepZ = (maxZ - minZ) / N;
    constexpr float kYOffset = 6.0f;
    constexpr int   kSubSamples = 3;

    for (int iz = 0; iz <= N; ++iz)
    {
        float wz = minZ + iz * stepZ;
        float v  = static_cast<float>(iz) / N;
        for (int ix = 0; ix <= N; ++ix)
        {
            float wx = minX + ix * stepX;
            float u  = static_cast<float>(ix) / N;

            float maxY = waterLevel;
            if (terrain)
            {
                float subStepX = stepX / kSubSamples;
                float subStepZ = stepZ / kSubSamples;
                float startX = wx - stepX * 0.5f;
                float startZ = wz - stepZ * 0.5f;
                for (int sz = 0; sz <= kSubSamples; ++sz)
                {
                    for (int sx = 0; sx <= kSubSamples; ++sx)
                    {
                        float h = terrain->get_height_at(
                            startX + sx * subStepX,
                            startZ + sz * subStepZ);
                        if (h > maxY) maxY = h;
                    }
                }
            }
            float wy = std::max(maxY, waterLevel) + kYOffset;

            int idx = iz * (N + 1) + ix;
            verts[idx] = { wx, wy, wz, u, v };
        }
    }

    uint32_t* dst = indices.data();
    for (int iz = 0; iz < N; ++iz)
    {
        for (int ix = 0; ix < N; ++ix)
        {
            uint32_t tl = iz * (N + 1) + ix;
            uint32_t tr = tl + 1;
            uint32_t bl = (iz + 1) * (N + 1) + ix;
            uint32_t br = bl + 1;
            *dst++ = tl; *dst++ = bl; *dst++ = tr;
            *dst++ = tr; *dst++ = bl; *dst++ = br;
        }
    }

    m_indexCount = static_cast<UINT>(indices.size());

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(HeatmapVertex));
    vbd.Usage     = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vdata = { verts.data() };
    device->CreateBuffer(&vbd, &vdata, m_vb.ReleaseAndGetAddressOf());

    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    ibd.Usage     = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA idata = { indices.data() };
    device->CreateBuffer(&ibd, &idata, m_ib.ReleaseAndGetAddressOf());

    m_meshReady = true;
}

// ---------------------------------------------------------------------------
// Per-layer density texture management
// ---------------------------------------------------------------------------
void HeatmapRenderer::EnsureLayerTextures(ID3D11Device* device, size_t layerCount)
{
    if (m_layers.size() == layerCount) return;

    if (layerCount > m_layers.size())
    {
        size_t oldSize = m_layers.size();
        m_layers.resize(layerCount);
        for (size_t i = oldSize; i < layerCount; ++i)
        {
            D3D11_TEXTURE2D_DESC dd = {};
            dd.Width     = HeatmapAccumulator::kTexSize;
            dd.Height    = HeatmapAccumulator::kTexSize;
            dd.MipLevels = 1;
            dd.ArraySize = 1;
            dd.Format    = DXGI_FORMAT_R32_FLOAT;
            dd.SampleDesc.Count = 1;
            dd.Usage     = D3D11_USAGE_DYNAMIC;
            dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            dd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            device->CreateTexture2D(&dd, nullptr,
                                    m_layers[i].densityTex.GetAddressOf());
            device->CreateShaderResourceView(m_layers[i].densityTex.Get(), nullptr,
                                              m_layers[i].densitySRV.GetAddressOf());
        }
    }
    else
    {
        m_layers.resize(layerCount);
    }
}

void HeatmapRenderer::UpdateLayerDensityTexture(
    ID3D11DeviceContext* ctx, size_t layerIdx,
    const HeatmapAccumulator& accum)
{
    if (layerIdx >= m_layers.size()) return;
    if (!accum.IsLayerTextureDirty(layerIdx)) return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(m_layers[layerIdx].densityTex.Get(), 0,
                          D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;

    const float* src = accum.GetLayerDensityData(layerIdx);
    const int texSize = HeatmapAccumulator::kTexSize;

    for (int row = 0; row < texSize; ++row)
    {
        memcpy(
            static_cast<uint8_t*>(mapped.pData) + row * mapped.RowPitch,
            src + row * texSize,
            texSize * sizeof(float));
    }
    ctx->Unmap(m_layers[layerIdx].densityTex.Get(), 0);
}

// ---------------------------------------------------------------------------
// Render all enabled layers (painter's order)
// ---------------------------------------------------------------------------
void HeatmapRenderer::RenderLayers(
    ID3D11DeviceContext* ctx,
    const XMMATRIX& viewProj,
    const std::vector<HeatmapLayerDef>& layers)
{
    if (!IsReady()) return;

    bool anyEnabled = false;
    for (size_t i = 0; i < layers.size(); ++i)
    {
        if (i >= m_layers.size()) break;
        if (layers[i].enabled && layers[i].matched)
        { anyEnabled = true; break; }
    }
    if (!anyEnabled) return;

    // Save state once
    ComPtr<ID3D11RasterizerState>    prevRS;
    ComPtr<ID3D11DepthStencilState>  prevDSS;
    UINT prevStencilRef;
    ComPtr<ID3D11BlendState>         prevBS;
    FLOAT prevBF[4]; UINT prevSM;
    ComPtr<ID3D11VertexShader>       prevVS;
    ComPtr<ID3D11PixelShader>        prevPS;
    ComPtr<ID3D11InputLayout>        prevIL;
    D3D11_PRIMITIVE_TOPOLOGY         prevTopo;
    ComPtr<ID3D11Buffer>             prevVSCB0;
    ComPtr<ID3D11Buffer>             prevPSCB0;
    ComPtr<ID3D11ShaderResourceView> prevSRV0, prevSRV1;
    ComPtr<ID3D11SamplerState>       prevSamp;

    ctx->RSGetState(prevRS.GetAddressOf());
    ctx->OMGetDepthStencilState(prevDSS.GetAddressOf(), &prevStencilRef);
    ctx->OMGetBlendState(prevBS.GetAddressOf(), prevBF, &prevSM);
    ctx->VSGetShader(prevVS.GetAddressOf(), nullptr, nullptr);
    ctx->PSGetShader(prevPS.GetAddressOf(), nullptr, nullptr);
    ctx->IAGetInputLayout(prevIL.GetAddressOf());
    ctx->IAGetPrimitiveTopology(&prevTopo);
    ctx->VSGetConstantBuffers(0, 1, prevVSCB0.GetAddressOf());
    ctx->PSGetConstantBuffers(0, 1, prevPSCB0.GetAddressOf());
    ctx->PSGetShaderResources(0, 1, prevSRV0.GetAddressOf());
    ctx->PSGetShaderResources(1, 1, prevSRV1.GetAddressOf());
    ctx->PSGetSamplers(0, 1, prevSamp.GetAddressOf());

    // Bind shared pipeline state
    UINT stride = sizeof(HeatmapVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, m_vb.GetAddressOf(), &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetInputLayout(m_il.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

    ctx->RSSetState(m_rs.Get());
    ctx->OMSetDepthStencilState(m_dss.Get(), 0);
    float bf[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(m_blendState.Get(), bf, 0xFFFFFFFF);

    // Draw each enabled layer
    for (size_t i = 0; i < layers.size(); ++i)
    {
        if (i >= m_layers.size()) break;
        const auto& def = layers[i];
        if (!def.enabled || !def.matched) continue;

        int palIdx = static_cast<int>(def.palette);
        if (palIdx < 0 || palIdx >= kNumPalettes) palIdx = 0;

        // Update CB with this layer's opacity
        HeatmapCB cb = {};
        XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(viewProj));
        cb.opacity = def.opacity;

        D3D11_MAPPED_SUBRESOURCE mapped;
        ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy(mapped.pData, &cb, sizeof(cb));
        ctx->Unmap(m_cb.Get(), 0);

        ctx->VSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, m_cb.GetAddressOf());

        ID3D11ShaderResourceView* srvs[] = {
            m_layers[i].densitySRV.Get(),
            m_lutSRV[palIdx].Get()
        };
        ctx->PSSetShaderResources(0, 2, srvs);

        ctx->DrawIndexed(m_indexCount, 0, 0);
    }

    // Restore state once
    ctx->RSSetState(prevRS.Get());
    ctx->OMSetDepthStencilState(prevDSS.Get(), prevStencilRef);
    ctx->OMSetBlendState(prevBS.Get(), prevBF, prevSM);
    ctx->VSSetShader(prevVS.Get(), nullptr, 0);
    ctx->PSSetShader(prevPS.Get(), nullptr, 0);
    ctx->IASetInputLayout(prevIL.Get());
    ctx->IASetPrimitiveTopology(prevTopo);
    ctx->VSSetConstantBuffers(0, 1, prevVSCB0.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, prevPSCB0.GetAddressOf());
    ID3D11ShaderResourceView* restoreSrvs[] = { prevSRV0.Get(), prevSRV1.Get() };
    ctx->PSSetShaderResources(0, 2, restoreSrvs);
    ctx->PSSetSamplers(0, 1, prevSamp.GetAddressOf());
}
