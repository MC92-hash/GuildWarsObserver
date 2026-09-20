#include "pch.h"
#include "ReplayWindow.h"
#include "GuiGlobalConstants.h"
#include "RunLog.h"
#include <d3dcompiler.h>
#include <algorithm>
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// SCENE BLOOM - the game's own post-process, reproduced.
//
// WHY IT IS HERE AT ALL, and why it defaults to ON. The game applies this to every frame of every
// guild hall and there is no preference, no map flag and no graphics option that switches it off -
// only a device-capability test stands in front of it. So a replay drawn without it is not "the
// same picture without an effect", it is a different picture, and every screenshot comparison
// anyone makes against the game will carry the difference. That is why it ships on.
//
// THE CHAIN, read out of the game's own post-process materials:
//
//     out = scene + B * blur4( scene * luminance(scene) )
//
//   * the bright pass is QUADRATIC and has NO THRESHOLD - every pixel contributes in proportion to
//     its own brightness, which is why a dark basin ringed by lit rock and bright sky still picks
//     up a lift: the lift comes from its NEIGHBOURS, not from itself;
//   * it runs at quarter resolution in each axis;
//   * four 4-tap passes follow, with the weights 0.48743 / 0.14791 / 0.32803 / 0.03663. Those four
//     numbers are read from the binary and they sum to exactly 1, so the blur conserves energy.
//     The tap OFFSETS and the pass directions were not recovered; this implementation runs the
//     four passes as +X, -X, +Y, -Y with the weights sorted by distance, which is the only
//     reconstruction that makes a monotone one-sided weight set into a symmetric separable blur.
//     Because the weights sum to 1, the choice changes the spatial falloff and not the amount of
//     light moved, so a measurement taken over a flat patch is insensitive to it.
//   * B is per map: environment sub-list 1 byte 0 over 256, then B = v * 0.98 + 0.008, blended
//     across regions exactly like the fog. Corrupted Isle resolves to 0.395, Druid's Isle to
//     0.513 - both reproduced from the map files.
//
// SATURATION is part of the same chain but is neutral on thirteen of the sixteen halls: byte 1 of
// the same record is 255 everywhere except Isle of Jade (142), Uncharted Isle (172) and Isle of
// Wurms (155). It is applied as lerp(luma, colour, sat) with Rec. 709 luma. The TINT stage is
// never active - byte 2 is zero on all sixteen - so it is not implemented.
//
// WHERE IT SITS: after the world, after the characters, before any of the interface. It reads a
// copy of the finished frame and writes the composite back over it, so it is the last thing that
// touches the picture and the first thing the eye should be compared against.
// ---------------------------------------------------------------------------

namespace
{
// The four tap weights, sorted by distance from the centre. PROVEN; the ordering against distance
// is the reconstruction described above.
constexpr float kBloomTapW0 = 0.48743f;
constexpr float kBloomTapW1 = 0.32803f;
constexpr float kBloomTapW2 = 0.14791f;
constexpr float kBloomTapW3 = 0.03663f;

// Quarter resolution in each axis.
constexpr int kBloomDownscale = 4;

const char kBloomHLSL[] = R"(
Texture2D    srcTex : register(t0);
Texture2D    addTex : register(t1);
SamplerState samLin : register(s0);

cbuffer BloomCB : register(b0)
{
    float2 srcTexel;    // 1 / source size
    float2 blurStep;    // one tap step, in source uv, along the pass direction
    float4 tapWeights;  // the four weights, nearest first
    float  amount;      // B for this map
    float  saturation;  // 1 = neutral
    float2 bloomPad;
};

struct VS_OUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUT VSMain(uint id : SV_VertexID)
{
    VS_OUT o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
    o.uv  = uv;
    return o;
}

float Luma(float3 c)
{
    // Rec. 709
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Downsample the finished frame to quarter size and square it by its own brightness. No threshold.
float4 PSBright(VS_OUT i) : SV_TARGET
{
    float3 c = 0;
    c += srcTex.Sample(samLin, i.uv + float2(-1.0,  -1.0) * srcTexel).rgb;
    c += srcTex.Sample(samLin, i.uv + float2( 1.0,  -1.0) * srcTexel).rgb;
    c += srcTex.Sample(samLin, i.uv + float2(-1.0,   1.0) * srcTexel).rgb;
    c += srcTex.Sample(samLin, i.uv + float2( 1.0,   1.0) * srcTexel).rgb;
    c *= 0.25;
    return float4(c * Luma(c), 1.0);
}

// One 4-tap pass along blurStep.
float4 PSBlur(VS_OUT i) : SV_TARGET
{
    float3 c  = srcTex.Sample(samLin, i.uv).rgb                 * tapWeights.x;
    c        += srcTex.Sample(samLin, i.uv + blurStep).rgb      * tapWeights.y;
    c        += srcTex.Sample(samLin, i.uv + blurStep * 2).rgb  * tapWeights.z;
    c        += srcTex.Sample(samLin, i.uv + blurStep * 3).rgb  * tapWeights.w;
    return float4(c, 1.0);
}

// scene + B * blur, then the map's saturation. Written over the frame, not blended into it, so the
// saturation stage can see the sum.
float4 PSComposite(VS_OUT i) : SV_TARGET
{
    float3 scene = srcTex.Sample(samLin, i.uv).rgb;
    float3 glow  = addTex.Sample(samLin, i.uv).rgb;
    float3 c     = scene + amount * glow;
    if (saturation < 0.999)
        c = lerp(Luma(c).xxx, c, saturation);
    return float4(c, 1.0);
}
)";
} // namespace

// Drop everything and re-arm. Called on a device loss: the objects below belong to the device that
// has gone, and unlike the loaded scene this pass can rebuild itself from nothing on the next frame.
void ReplayWindow::ReleaseBloomResources()
{
    m_bloomShadersTried = false;
    m_bloomVS.Reset();
    m_bloomBrightPS.Reset();
    m_bloomBlurPS.Reset();
    m_bloomCompositePS.Reset();
    m_bloomCB.Reset();
    m_bloomSampler.Reset();
    m_bloomBS.Reset();
    m_bloomDSS.Reset();
    m_bloomRS.Reset();
    m_bloomSceneTex.Reset();
    m_bloomSceneSRV.Reset();
    for (int i = 0; i < 2; ++i)
    {
        m_bloomTex[i].Reset();
        m_bloomRTV[i].Reset();
        m_bloomSRV[i].Reset();
    }
    m_bloomWidth = 0;
    m_bloomHeight = 0;
}

void ReplayWindow::InitBloomShaders()
{
    if (m_bloomShadersTried) return;
    m_bloomShadersTried = true;

    ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;
    if (!dev) return;

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif

    auto compile = [&](const char* entry, const char* target, ID3DBlob** out) -> bool {
        errBlob.Reset();
        HRESULT hr = D3DCompile(kBloomHLSL, sizeof(kBloomHLSL), nullptr, nullptr, nullptr, entry,
                                target, flags, 0, out, errBlob.GetAddressOf());
        if (FAILED(hr))
        {
            if (errBlob)
                RunLog::Line("map:   scene bloom shader %s failed to build: %s", entry,
                             static_cast<const char*>(errBlob->GetBufferPointer()));
            return false;
        }
        return true;
    };

    if (!compile("VSMain", "vs_5_0", vsBlob.GetAddressOf())) return;
    if (FAILED(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                       m_bloomVS.GetAddressOf())))
        return;

    struct { const char* entry; Microsoft::WRL::ComPtr<ID3D11PixelShader>* slot; } stages[] = {
        {"PSBright",    &m_bloomBrightPS},
        {"PSBlur",      &m_bloomBlurPS},
        {"PSComposite", &m_bloomCompositePS},
    };
    for (auto& st : stages)
    {
        psBlob.Reset();
        if (!compile(st.entry, "ps_5_0", psBlob.GetAddressOf())) { m_bloomVS.Reset(); return; }
        if (FAILED(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                          nullptr, st.slot->GetAddressOf())))
        {
            m_bloomVS.Reset();
            return;
        }
    }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(BloomCBData);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&cbd, nullptr, m_bloomCB.GetAddressOf());

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, m_bloomSampler.GetAddressOf());

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dev->CreateDepthStencilState(&dsd, m_bloomDSS.GetAddressOf());

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = FALSE;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, m_bloomBS.GetAddressOf());

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, m_bloomRS.GetAddressOf());
}

bool ReplayWindow::EnsureBloomTargets(int width, int height)
{
    if (width <= 0 || height <= 0) return false;
    if (m_bloomSceneSRV && m_bloomWidth == width && m_bloomHeight == height) return true;

    ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;
    if (!dev) return false;

    m_bloomSceneTex.Reset();
    m_bloomSceneSRV.Reset();
    for (int i = 0; i < 2; ++i) { m_bloomTex[i].Reset(); m_bloomRTV[i].Reset(); m_bloomSRV[i].Reset(); }

    const DXGI_FORMAT fmt = m_deviceResources->GetBackBufferFormat();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, m_bloomSceneTex.GetAddressOf()))) return false;
    if (FAILED(dev->CreateShaderResourceView(m_bloomSceneTex.Get(), nullptr,
                                             m_bloomSceneSRV.GetAddressOf())))
        return false;

    const int qw = std::max(1, width / kBloomDownscale);
    const int qh = std::max(1, height / kBloomDownscale);
    td.Width = static_cast<UINT>(qw);
    td.Height = static_cast<UINT>(qh);
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < 2; ++i)
    {
        if (FAILED(dev->CreateTexture2D(&td, nullptr, m_bloomTex[i].GetAddressOf()))) return false;
        if (FAILED(dev->CreateRenderTargetView(m_bloomTex[i].Get(), nullptr,
                                               m_bloomRTV[i].GetAddressOf())))
            return false;
        if (FAILED(dev->CreateShaderResourceView(m_bloomTex[i].Get(), nullptr,
                                                 m_bloomSRV[i].GetAddressOf())))
            return false;
    }

    m_bloomWidth = width;
    m_bloomHeight = height;
    m_bloomQuarterW = qw;
    m_bloomQuarterH = qh;
    return true;
}

void ReplayWindow::DrawSceneBloom()
{
    if (!GuiGlobalConstants::map_bloom_enabled) return;
    if (!m_deviceResources) return;
    if (!(m_mapBloomAmount > 0.0f) && m_mapSceneSaturation >= 0.999f) return;

    InitBloomShaders();
    if (!m_bloomVS || !m_bloomBrightPS || !m_bloomBlurPS || !m_bloomCompositePS || !m_bloomCB)
        return;

    auto* ctx = m_deviceResources->GetD3DDeviceContext();
    ID3D11RenderTargetView* backRTV = m_deviceResources->GetRenderTargetView();
    if (!ctx || !backRTV) return;

    Microsoft::WRL::ComPtr<ID3D11Resource> backRes;
    backRTV->GetResource(backRes.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backTex;
    if (FAILED(backRes.As(&backTex))) return;

    D3D11_TEXTURE2D_DESC backDesc = {};
    backTex->GetDesc(&backDesc);
    if (!EnsureBloomTargets(static_cast<int>(backDesc.Width), static_cast<int>(backDesc.Height)))
        return;

    // A copy of the finished frame, resolved if the back buffer is multisampled. Doing the resolve
    // here rather than letting Present do it is not a loss: the composite is written back over
    // every sample, so the eventual resolve sees one value.
    if (backDesc.SampleDesc.Count > 1)
        ctx->ResolveSubresource(m_bloomSceneTex.Get(), 0, backTex.Get(), 0, backDesc.Format);
    else
        ctx->CopyResource(m_bloomSceneTex.Get(), backTex.Get());

    // ---- save every piece of state this touches ------------------------------------------
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   prevRS;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> prevDSS;
    UINT prevStencilRef = 0;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        prevBS;
    FLOAT prevBF[4] = {0, 0, 0, 0};
    UINT prevSM = 0xFFFFFFFF;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      prevVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       prevPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>       prevIL;
    D3D11_PRIMITIVE_TOPOLOGY prevTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Microsoft::WRL::ComPtr<ID3D11Buffer>            prevPSCB0;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      prevSamp0;
    // Two slots, because the world pass binds the picking buffer alongside the frame and dropping
    // it silently on restore is the kind of thing that only shows up as a debug-layer complaint.
    ID3D11RenderTargetView* prevRTVs[2] = {nullptr, nullptr};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>  prevDSV;

    ctx->RSGetState(prevRS.GetAddressOf());
    ctx->OMGetDepthStencilState(prevDSS.GetAddressOf(), &prevStencilRef);
    ctx->OMGetBlendState(prevBS.GetAddressOf(), prevBF, &prevSM);
    ctx->VSGetShader(prevVS.GetAddressOf(), nullptr, nullptr);
    ctx->PSGetShader(prevPS.GetAddressOf(), nullptr, nullptr);
    ctx->IAGetInputLayout(prevIL.GetAddressOf());
    ctx->IAGetPrimitiveTopology(&prevTopo);
    ctx->PSGetConstantBuffers(0, 1, prevPSCB0.GetAddressOf());
    ctx->PSGetSamplers(0, 1, prevSamp0.GetAddressOf());
    ctx->OMGetRenderTargets(2, prevRTVs, prevDSV.GetAddressOf());
    D3D11_VIEWPORT prevVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT prevVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&prevVpCount, prevVp);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_bloomVS.Get(), nullptr, 0);
    ctx->RSSetState(m_bloomRS.Get());
    ctx->OMSetDepthStencilState(m_bloomDSS.Get(), 0);
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(m_bloomBS.Get(), blendFactor, 0xFFFFFFFF);
    ctx->PSSetConstantBuffers(0, 1, m_bloomCB.GetAddressOf());
    ctx->PSSetSamplers(0, 1, m_bloomSampler.GetAddressOf());

    ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};

    auto setCB = [&](float sx, float sy, float stepX, float stepY, float amt, float sat) {
        BloomCBData cb = {};
        cb.srcTexelX = sx;
        cb.srcTexelY = sy;
        cb.blurStepX = stepX;
        cb.blurStepY = stepY;
        cb.tapW0 = kBloomTapW0;
        cb.tapW1 = kBloomTapW1;
        cb.tapW2 = kBloomTapW2;
        cb.tapW3 = kBloomTapW3;
        cb.amount = amt;
        cb.saturation = sat;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(ctx->Map(m_bloomCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            memcpy(mapped.pData, &cb, sizeof(cb));
            ctx->Unmap(m_bloomCB.Get(), 0);
        }
    };

    auto setViewport = [&](int w, int h) {
        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = static_cast<float>(w);
        vp.Height = static_cast<float>(h);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
    };

    const float fullTexelX = 1.0f / static_cast<float>(m_bloomWidth);
    const float fullTexelY = 1.0f / static_cast<float>(m_bloomHeight);
    const float qTexelX = 1.0f / static_cast<float>(m_bloomQuarterW);
    const float qTexelY = 1.0f / static_cast<float>(m_bloomQuarterH);

    // ---- 1. bright pass, full res in -> quarter res out ------------------------------------
    setViewport(m_bloomQuarterW, m_bloomQuarterH);
    {
        ID3D11RenderTargetView* rtv = m_bloomRTV[0].Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->PSSetShader(m_bloomBrightPS.Get(), nullptr, 0);
        // The downsample taps sit one full-resolution texel from the centre of each quarter-res
        // texel; with bilinear filtering that covers the block.
        setCB(fullTexelX * (kBloomDownscale * 0.5f), fullTexelY * (kBloomDownscale * 0.5f), 0.0f,
              0.0f, m_mapBloomAmount, 1.0f);
        ID3D11ShaderResourceView* srv = m_bloomSceneSRV.Get();
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSRVs);
    }

    // ---- 2..5. the four blur passes: +X, -X, +Y, -Y -----------------------------------------
    ctx->PSSetShader(m_bloomBlurPS.Get(), nullptr, 0);
    const float steps[4][2] = {
        { qTexelX, 0.0f}, {-qTexelX, 0.0f}, {0.0f,  qTexelY}, {0.0f, -qTexelY},
    };
    int src = 0;
    for (int pass = 0; pass < 4; ++pass)
    {
        const int dst = 1 - src;
        ID3D11RenderTargetView* rtv = m_bloomRTV[dst].Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        setCB(qTexelX, qTexelY, steps[pass][0], steps[pass][1], m_mapBloomAmount, 1.0f);
        ID3D11ShaderResourceView* srv = m_bloomSRV[src].Get();
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSRVs);
        src = dst;
    }

    // ---- 6. composite back over the frame ---------------------------------------------------
    setViewport(m_bloomWidth, m_bloomHeight);
    {
        ctx->OMSetRenderTargets(1, &backRTV, nullptr);
        ctx->PSSetShader(m_bloomCompositePS.Get(), nullptr, 0);
        setCB(fullTexelX, fullTexelY, 0.0f, 0.0f, m_mapBloomAmount, m_mapSceneSaturation);
        ID3D11ShaderResourceView* srvs[2] = {m_bloomSceneSRV.Get(), m_bloomSRV[src].Get()};
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 2, nullSRVs);
    }

    // ---- restore -----------------------------------------------------------------------------
    ctx->OMSetRenderTargets(prevRTVs[1] ? 2u : 1u, prevRTVs, prevDSV.Get());
    for (auto*& r : prevRTVs)
        if (r) { r->Release(); r = nullptr; }
    if (prevVpCount > 0) ctx->RSSetViewports(prevVpCount, prevVp);
    ctx->RSSetState(prevRS.Get());
    ctx->OMSetDepthStencilState(prevDSS.Get(), prevStencilRef);
    ctx->OMSetBlendState(prevBS.Get(), prevBF, prevSM);
    ctx->VSSetShader(prevVS.Get(), nullptr, 0);
    ctx->PSSetShader(prevPS.Get(), nullptr, 0);
    ctx->IASetInputLayout(prevIL.Get());
    if (prevTopo != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) ctx->IASetPrimitiveTopology(prevTopo);
    ctx->PSSetConstantBuffers(0, 1, prevPSCB0.GetAddressOf());
    ctx->PSSetSamplers(0, 1, prevSamp0.GetAddressOf());
}
