#include "pch.h"
#include "ReplayWindow.h"
#include "GuiGlobalConstants.h"
#include "RunLog.h"
#include <d3dcompiler.h>
#include <algorithm>
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// RENDER SCALE - the 3D scene at a fraction of the window's resolution.
//
// The biggest lever a weak GPU has: at 60 % the scene shades a third of the pixels. The scene
// draws into a target of its own at the reduced size; ResolveSceneScale() stretches it over the
// back buffer with a bilinear filter, after the world and the characters and before the bloom,
// the portraits and the interface, which all keep the window's full resolution.
//
// Every scene pass takes its target and viewport from SceneRTV() / SceneDSV() / SceneViewport().
// Passes that draw into the CURRENTLY BOUND target (the characters, the effects) follow on their
// own. Anything that projects onto the screen for the interface keeps using the device's screen
// viewport, because the interface is drawn at full size.
// ---------------------------------------------------------------------------

namespace
{
const char kSceneScaleHLSL[] = R"(
Texture2D    sceneTex : register(t0);
SamplerState samLin   : register(s0);

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

float4 PSMain(VS_OUT i) : SV_TARGET
{
    return float4(sceneTex.Sample(samLin, i.uv).rgb, 1.0);
}
)";
} // namespace

ID3D11RenderTargetView* ReplayWindow::SceneRTV() const
{
    return m_sceneScaleActive ? m_sceneScaleRTV.Get() : m_deviceResources->GetRenderTargetView();
}

ID3D11DepthStencilView* ReplayWindow::SceneDSV() const
{
    return m_sceneScaleActive ? m_sceneScaleDSV.Get() : m_deviceResources->GetDepthStencilView();
}

D3D11_VIEWPORT ReplayWindow::SceneViewport() const
{
    return m_sceneScaleActive ? m_sceneScaleViewport : m_deviceResources->GetScreenViewport();
}

void ReplayWindow::ReleaseSceneScaleResources()
{
    m_sceneScaleActive = false;
    m_sceneScaleShadersTried = false;
    m_sceneScaleW = m_sceneScaleH = 0;
    m_sceneScaleTex.Reset();
    m_sceneScaleRTV.Reset();
    m_sceneScaleSRV.Reset();
    m_sceneScaleDepth.Reset();
    m_sceneScaleDSV.Reset();
    m_sceneScaleVS.Reset();
    m_sceneScalePS.Reset();
    m_sceneScaleSampler.Reset();
    m_sceneScaleRS.Reset();
    m_sceneScaleDSS.Reset();
    m_sceneScaleBS.Reset();
}

void ReplayWindow::BeginSceneScale()
{
    m_sceneScaleActive = false;
    if (!m_deviceResources) return;

    // The asset inspector's picking target is bound beside the scene at the window's size, and a
    // multisampled back buffer cannot take the plain stretch below: both draw at full size.
    const int pct = std::clamp(GuiGlobalConstants::render_scale_percent, 25, 100);
    if (pct >= 100 || m_assetSelectionEnabled || m_deviceResources->GetMsaaLevelIndex() > 0)
        return;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    if (!dev) return;

    if (!m_sceneScaleShadersTried)
    {
        m_sceneScaleShadersTried = true;
        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
        auto compile = [&](const char* entry, const char* target, ID3DBlob** out) -> bool {
            errBlob.Reset();
            HRESULT hr = D3DCompile(kSceneScaleHLSL, sizeof(kSceneScaleHLSL), nullptr, nullptr, nullptr,
                                    entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, out,
                                    errBlob.GetAddressOf());
            if (FAILED(hr) && errBlob)
                RunLog::Line("render scale: shader %s failed to build: %s", entry,
                             static_cast<const char*>(errBlob->GetBufferPointer()));
            return SUCCEEDED(hr);
        };
        if (compile("VSMain", "vs_5_0", vsBlob.GetAddressOf()))
            dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                    m_sceneScaleVS.GetAddressOf());
        if (compile("PSMain", "ps_5_0", psBlob.GetAddressOf()))
            dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                   m_sceneScalePS.GetAddressOf());

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, m_sceneScaleSampler.GetAddressOf());

        D3D11_DEPTH_STENCIL_DESC dsd = {};
        dsd.DepthEnable = FALSE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dev->CreateDepthStencilState(&dsd, m_sceneScaleDSS.GetAddressOf());

        D3D11_BLEND_DESC bld = {};
        bld.RenderTarget[0].BlendEnable = FALSE;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bld, m_sceneScaleBS.GetAddressOf());

        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, m_sceneScaleRS.GetAddressOf());
    }
    if (!m_sceneScaleVS || !m_sceneScalePS || !m_sceneScaleSampler) return;

    const D3D11_VIEWPORT full = m_deviceResources->GetScreenViewport();
    const int w = std::max(1, static_cast<int>(full.Width * pct / 100.f + 0.5f));
    const int h = std::max(1, static_cast<int>(full.Height * pct / 100.f + 0.5f));

    if (!m_sceneScaleRTV || w != m_sceneScaleW || h != m_sceneScaleH)
    {
        m_sceneScaleTex.Reset();
        m_sceneScaleRTV.Reset();
        m_sceneScaleSRV.Reset();
        m_sceneScaleDepth.Reset();
        m_sceneScaleDSV.Reset();
        m_sceneScaleW = m_sceneScaleH = 0;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = m_deviceResources->GetBackBufferFormat();
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, m_sceneScaleTex.GetAddressOf())) ||
            FAILED(dev->CreateRenderTargetView(m_sceneScaleTex.Get(), nullptr, m_sceneScaleRTV.GetAddressOf())) ||
            FAILED(dev->CreateShaderResourceView(m_sceneScaleTex.Get(), nullptr, m_sceneScaleSRV.GetAddressOf())))
        {
            m_sceneScaleRTV.Reset();
            return;
        }

        td.Format = m_deviceResources->GetDepthBufferFormat();
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, m_sceneScaleDepth.GetAddressOf())) ||
            FAILED(dev->CreateDepthStencilView(m_sceneScaleDepth.Get(), nullptr, m_sceneScaleDSV.GetAddressOf())))
        {
            m_sceneScaleRTV.Reset();
            return;
        }

        m_sceneScaleW = w;
        m_sceneScaleH = h;
        m_sceneScaleViewport = {0.f, 0.f, static_cast<float>(w), static_cast<float>(h), 0.f, 1.f};
        RunLog::Line("render scale: the scene draws at %dx%d (%d%% of %.0fx%.0f)", w, h, pct,
                     full.Width, full.Height);
    }

    m_sceneScaleActive = true;
}

void ReplayWindow::ResolveSceneScale()
{
    if (!m_sceneScaleActive) return;
    m_sceneScaleActive = false;

    auto* ctx = m_deviceResources->GetD3DDeviceContext();
    ID3D11RenderTargetView* backRTV = m_deviceResources->GetRenderTargetView();
    if (!ctx || !backRTV) return;

    // Only what the stretch changes and the passes after it might read is put back; the interface
    // sets its own state in full.
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
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      prevSamp0;
    ctx->RSGetState(prevRS.GetAddressOf());
    ctx->OMGetDepthStencilState(prevDSS.GetAddressOf(), &prevStencilRef);
    ctx->OMGetBlendState(prevBS.GetAddressOf(), prevBF, &prevSM);
    ctx->VSGetShader(prevVS.GetAddressOf(), nullptr, nullptr);
    ctx->PSGetShader(prevPS.GetAddressOf(), nullptr, nullptr);
    ctx->IAGetInputLayout(prevIL.GetAddressOf());
    ctx->IAGetPrimitiveTopology(&prevTopo);
    ctx->PSGetSamplers(0, 1, prevSamp0.GetAddressOf());

    // From here on every pass draws at full size, onto the back buffer.
    const D3D11_VIEWPORT full = m_deviceResources->GetScreenViewport();
    ctx->OMSetRenderTargets(1, &backRTV, nullptr);
    ctx->RSSetViewports(1, &full);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_sceneScaleVS.Get(), nullptr, 0);
    ctx->PSSetShader(m_sceneScalePS.Get(), nullptr, 0);
    ctx->RSSetState(m_sceneScaleRS.Get());
    ctx->OMSetDepthStencilState(m_sceneScaleDSS.Get(), 0);
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(m_sceneScaleBS.Get(), blendFactor, 0xFFFFFFFF);
    ctx->PSSetSamplers(0, 1, m_sceneScaleSampler.GetAddressOf());
    ID3D11ShaderResourceView* srv = m_sceneScaleSRV.Get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);

    ctx->OMSetRenderTargets(1, &backRTV, m_deviceResources->GetDepthStencilView());
    ctx->RSSetState(prevRS.Get());
    ctx->OMSetDepthStencilState(prevDSS.Get(), prevStencilRef);
    ctx->OMSetBlendState(prevBS.Get(), prevBF, prevSM);
    ctx->VSSetShader(prevVS.Get(), nullptr, 0);
    ctx->PSSetShader(prevPS.Get(), nullptr, 0);
    ctx->IASetInputLayout(prevIL.Get());
    ctx->IASetPrimitiveTopology(prevTopo);
    ctx->PSSetSamplers(0, 1, prevSamp0.GetAddressOf());
}

void ReplayWindow::ApplyGraphicsSettings()
{
    if (!m_mapRenderer) return;
    m_mapRenderer->SetLODQuality(GuiGlobalConstants::map_detail > 0 ? LODQuality::Medium : LODQuality::High);
    // Taken over only when the setting CHANGES, so the debug panel's own shadow checkboxes still
    // work between two changes.
    const int mapShadows = GuiGlobalConstants::map_shadows ? 1 : 0;
    if (m_appliedMapShadows != mapShadows)
    {
        m_appliedMapShadows = mapShadows;
        m_showTerrainShadows = mapShadows != 0;
        // Switched back on: the shadow map is drawn again on the next frame.
        if (m_showTerrainShadows)
            m_mapRenderer->SetShouldRerenderShadows(true);
    }
    const int silhouettes = GuiGlobalConstants::character_silhouette_shadows ? 1 : 0;
    if (m_appliedSilhouetteShadows != silhouettes)
    {
        m_appliedSilhouetteShadows = silhouettes;
        m_showSilhouetteShadows = silhouettes != 0;
    }
}
