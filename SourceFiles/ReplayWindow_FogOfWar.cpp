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

// ---------------------------------------------------------------------------
// Fog of War — full-screen shader overlay
// ---------------------------------------------------------------------------

static const char kFogHLSL[] = R"(
cbuffer FogCB : register(b0)
{
    float4x4 invViewProj;
    float4 playerPos[8];
    float  compassRadius;
    float  fogOpacity;
    float  edgeSoftness;
    float  refHeight;
    float  viewportW;
    float  viewportH;
    int    playerCount;
    float  pad;
};

struct VS_OUT
{
    float4 pos : SV_Position;
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

float4 PSMain(VS_OUT i) : SV_Target
{
    float2 ndc;
    ndc.x =  (i.pos.x / viewportW) * 2.0 - 1.0;
    ndc.y = 1.0 - (i.pos.y / viewportH) * 2.0;

    float4 nearH = mul(float4(ndc, 0, 1), invViewProj);
    float4 farH  = mul(float4(ndc, 1, 1), invViewProj);

    if (abs(nearH.w) < 1e-6 || abs(farH.w) < 1e-6)
        return float4(0, 0, 0, fogOpacity);

    float3 nearW = nearH.xyz / nearH.w;
    float3 farW  = farH.xyz / farH.w;

    float3 dir = farW - nearW;
    if (abs(dir.y) < 1e-6)
        return float4(0, 0, 0, fogOpacity);

    float t = (refHeight - nearW.y) / dir.y;
    if (t < 0)
        return float4(0, 0, 0, fogOpacity);

    float3 hit = nearW + dir * t;

    float minDist = 1e6;
    [loop] for (int p = 0; p < playerCount; ++p)
    {
        float dx = hit.x - playerPos[p].x;
        float dz = hit.z - playerPos[p].z;
        minDist = min(minDist, sqrt(dx * dx + dz * dz));
    }

    float edge = max(edgeSoftness, 0.1);
    float fog = smoothstep(compassRadius - edge, compassRadius, minDist);
    return float4(0, 0, 0, fogOpacity * saturate(fog));
}
)";

void ReplayWindow::InitFogRenderer()
{
    if (m_fogInitialized) return;
    m_fogInitialized = true;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif

    HRESULT hr = D3DCompile(kFogHLSL, sizeof(kFogHLSL), nullptr, nullptr, nullptr,
                            "VSMain", "vs_5_0", flags, 0, vsBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return;

    hr = D3DCompile(kFogHLSL, sizeof(kFogHLSL), nullptr, nullptr, nullptr,
                    "PSMain", "ps_5_0", flags, 0, psBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return;

    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_fogVS.GetAddressOf());
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_fogPS.GetAddressOf());

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(FogCBData);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&cbd, nullptr, m_fogCB.GetAddressOf());

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dev->CreateDepthStencilState(&dsd, m_fogDSS.GetAddressOf());

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, m_fogBS.GetAddressOf());

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, m_fogRS.GetAddressOf());
}

static constexpr float kFogCompassRadius = 5020.f;

void ReplayWindow::DrawFogOfWar()
{
    if (m_fogPerspective == 0) return;
    if (m_topViewActive) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    InitFogRenderer();
    if (!m_fogVS || !m_fogPS) return;

    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;
    XMMATRIX viewProj = cam->GetView() * cam->GetProj();

    auto dvp = m_deviceResources->GetScreenViewport();
    float vpW = dvp.Width;
    float vpH = dvp.Height;

    const MapTransform& mt = m_replayCtx.mapTransform;
    const InterpolationSettings& is = m_replayCtx.interpSettings;
    Terrain* terrain = m_mapRenderer->GetTerrain();

    FogCBData cb = {};

    XMVECTOR det;
    XMMATRIX invVP = XMMatrixInverse(&det, viewProj);
    XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(invVP));

    XMFLOAT3 origin = ApplyMapTransformToPos(0, 0, 0, mt);
    XMFLOAT3 xOff   = ApplyMapTransformToPos(kFogCompassRadius, 0, 0, mt);
    float wdx = xOff.x - origin.x, wdz = xOff.z - origin.z;
    float worldCompassRadius = sqrtf(wdx * wdx + wdz * wdz);

    constexpr float kFogEdgeSoftness = 200.f;
    XMFLOAT3 eOff = ApplyMapTransformToPos(kFogEdgeSoftness, 0, 0, mt);
    float edx = eOff.x - origin.x, edz = eOff.z - origin.z;
    float worldEdgeSoftness = sqrtf(edx * edx + edz * edz);

    float avgHeight = 0.f;
    int playerCount = 0;
    bool isPlayerMode = (m_fogPlayerAgent >= 0);

    if (isPlayerMode)
    {
        auto pit = m_replayCtx.agents.find(m_fogPlayerAgent);
        if (pit != m_replayCtx.agents.end() && !pit->second.snapshots.empty()
            && !pit->second.isDeadAtTime(m_debugTimeline))
        {
            float sx, sy, sz;
            InterpolateAgentPosition(pit->second, m_debugTimeline, is, sx, sy, sz);
            XMFLOAT3 wpos = ApplyMapTransformToPos(sx, sy, sz, mt);
            if (terrain) wpos.y = terrain->get_height_at(wpos.x, wpos.z);
            cb.playerPos[0] = { wpos.x, wpos.y, wpos.z, 0.f };
            avgHeight = wpos.y;
            playerCount = 1;
        }
    }
    else
    {
        for (auto& [agentId, ard] : m_replayCtx.agents)
        {
            if (playerCount >= 8) break;
            if (ard.teamId != m_fogPerspective) continue;
            if (ard.type != AgentType::Player) continue;
            if (ard.snapshots.empty()) continue;
            if (ard.isDeadAtTime(m_debugTimeline)) continue;

            float sx, sy, sz;
            InterpolateAgentPosition(ard, m_debugTimeline, is, sx, sy, sz);
            XMFLOAT3 wpos = ApplyMapTransformToPos(sx, sy, sz, mt);
            if (terrain) wpos.y = terrain->get_height_at(wpos.x, wpos.z);

            cb.playerPos[playerCount] = { wpos.x, wpos.y, wpos.z, 0.f };
            avgHeight += wpos.y;
            playerCount++;
        }
    }

    float fogOpacity = isPlayerMode ? 0.82f : 0.72f;

    cb.playerCount    = playerCount;
    cb.compassRadius  = worldCompassRadius;
    cb.fogOpacity     = (playerCount > 0) ? fogOpacity : 1.0f;
    cb.edgeSoftness   = worldEdgeSoftness;
    cb.refHeight      = (playerCount > 0) ? (avgHeight / playerCount) : 0.f;
    cb.viewportW      = vpW;
    cb.viewportH      = vpH;

    auto* ctx = m_deviceResources->GetD3DDeviceContext();

    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   prevRS;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> prevDSS;
    UINT prevStencilRef;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        prevBS;
    FLOAT prevBF[4]; UINT prevSM;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      prevVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       prevPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>       prevIL;
    D3D11_PRIMITIVE_TOPOLOGY prevTopo;
    Microsoft::WRL::ComPtr<ID3D11Buffer> prevPSCB0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> prevVSCB0;

    ctx->RSGetState(prevRS.GetAddressOf());
    ctx->OMGetDepthStencilState(prevDSS.GetAddressOf(), &prevStencilRef);
    ctx->OMGetBlendState(prevBS.GetAddressOf(), prevBF, &prevSM);
    ctx->VSGetShader(prevVS.GetAddressOf(), nullptr, nullptr);
    ctx->PSGetShader(prevPS.GetAddressOf(), nullptr, nullptr);
    ctx->IAGetInputLayout(prevIL.GetAddressOf());
    ctx->IAGetPrimitiveTopology(&prevTopo);
    ctx->PSGetConstantBuffers(0, 1, prevPSCB0.GetAddressOf());
    ctx->VSGetConstantBuffers(0, 1, prevVSCB0.GetAddressOf());

    D3D11_MAPPED_SUBRESOURCE mapped;
    ctx->Map(m_fogCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(m_fogCB.Get(), 0);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(m_fogVS.Get(), nullptr, 0);
    ctx->PSSetShader(m_fogPS.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, m_fogCB.GetAddressOf());

    ctx->RSSetState(m_fogRS.Get());
    ctx->OMSetDepthStencilState(m_fogDSS.Get(), 0);
    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(m_fogBS.Get(), blendFactor, 0xFFFFFFFF);

    ctx->Draw(3, 0);

    ctx->RSSetState(prevRS.Get());
    ctx->OMSetDepthStencilState(prevDSS.Get(), prevStencilRef);
    ctx->OMSetBlendState(prevBS.Get(), prevBF, prevSM);
    ctx->VSSetShader(prevVS.Get(), nullptr, 0);
    ctx->PSSetShader(prevPS.Get(), nullptr, 0);
    ctx->IASetInputLayout(prevIL.Get());
    ctx->IASetPrimitiveTopology(prevTopo);
    ctx->PSSetConstantBuffers(0, 1, prevPSCB0.GetAddressOf());
    ctx->VSSetConstantBuffers(0, 1, prevVSCB0.GetAddressOf());
}

bool ReplayWindow::IsAgentInFog(int agentId) const
{
    if (m_fogPerspective == 0) return false;

    auto eit = m_replayCtx.agents.find(agentId);
    if (eit == m_replayCtx.agents.end()) return true;
    const auto& enemyArd = eit->second;

    if (m_fogPlayerAgent < 0 && enemyArd.teamId == m_fogPerspective) return false;

    float ex, ey, ez;
    InterpolateAgentPosition(enemyArd, m_debugTimeline, m_replayCtx.interpSettings, ex, ey, ez);

    if (m_fogPlayerAgent >= 0)
    {
        if (agentId == m_fogPlayerAgent) return false;
        auto pit = m_replayCtx.agents.find(m_fogPlayerAgent);
        if (pit == m_replayCtx.agents.end()) return true;
        if (pit->second.snapshots.empty() || pit->second.isDeadAtTime(m_debugTimeline))
            return true;
        float fx, fy, fz;
        InterpolateAgentPosition(pit->second, m_debugTimeline, m_replayCtx.interpSettings, fx, fy, fz);
        float dx = ex - fx, dy = ey - fy;
        return (dx * dx + dy * dy > kFogCompassRadius * kFogCompassRadius);
    }

    for (auto& [fid, fard] : m_replayCtx.agents)
    {
        if (fard.teamId != m_fogPerspective) continue;
        if (fard.type != AgentType::Player) continue;
        if (fard.snapshots.empty()) continue;
        if (fard.isDeadAtTime(m_debugTimeline)) continue;

        float fx, fy, fz;
        InterpolateAgentPosition(fard, m_debugTimeline, m_replayCtx.interpSettings, fx, fy, fz);

        float dx = ex - fx, dy = ey - fy;
        if (dx * dx + dy * dy <= kFogCompassRadius * kFogCompassRadius)
            return false;
    }
    return true;
}


// ---------------------------------------------------------------------------
// Fog of War Toolbar UI
// ---------------------------------------------------------------------------

void ReplayWindow::DrawFogOfWarToolbar()
{
    if (m_fogPerspective == 0) return;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,      ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));

    auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.f, 0.f), ImVec2(240.f, vp->Size.y));
    static bool fogFirstOpen = true;
    if (fogFirstOpen) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 8.f, vp->Pos.y + 8.f), ImGuiCond_Once);
        fogFirstOpen = false;
    }

    bool fogOpen = true;
    if (ImGui::Begin("Fog of War", &fogOpen,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize))
    {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, vp->Pos.x, vp->Pos.x + vp->Size.x - sz.x);
        float cy = std::clamp(pos.y, vp->Pos.y, vp->Pos.y + vp->Size.y - sz.y);
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));

        auto FogPill = [](const char* label, bool active, int team) -> bool {
            ImVec4 bg, tx, hov, bdr;
            if (active) {
                if (team == 1) {
                    bg  = ImVec4(0.25f, 0.06f, 0.06f, 1.f);
                    tx  = ImVec4(1.f, 0.42f, 0.42f, 1.f);
                    hov = ImVec4(0.30f, 0.10f, 0.10f, 1.f);
                    bdr = ImVec4(1.f, 0.42f, 0.42f, 0.85f);
                } else if (team == 2) {
                    bg  = ImVec4(0.05f, 0.12f, 0.25f, 1.f);
                    tx  = ImVec4(0.29f, 0.78f, 1.f, 1.f);
                    hov = ImVec4(0.08f, 0.16f, 0.30f, 1.f);
                    bdr = ImVec4(0.29f, 0.78f, 1.f, 0.85f);
                } else {
                    bg  = ImVec4(0.18f, 0.14f, 0.05f, 1.f);
                    tx  = ImVec4(1.f, 0.91f, 0.69f, 1.f);
                    hov = ImVec4(0.23f, 0.19f, 0.08f, 1.f);
                    bdr = ImVec4(1.f, 0.84f, 0.39f, 0.85f);
                }
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
            return clicked;
        };

        ImGui::TextColored(ImVec4(0.78f, 0.72f, 0.55f, 1.f), "Perspective");
        ImGui::SameLine();
        if (FogPill("Off", m_fogPerspective == 0 && m_fogPlayerAgent < 0, 0))
        { m_fogPerspective = 0; m_fogPlayerAgent = -1; }
        ImGui::SameLine();
        if (FogPill("Red", m_fogPerspective == 1 && m_fogPlayerAgent < 0, 1))
        { m_fogPerspective = (m_fogPerspective == 1 && m_fogPlayerAgent < 0) ? 0 : 1; m_fogPlayerAgent = -1; }
        ImGui::SameLine();
        if (FogPill("Blue", m_fogPerspective == 2 && m_fogPlayerAgent < 0, 2))
        { m_fogPerspective = (m_fogPerspective == 2 && m_fogPlayerAgent < 0) ? 0 : 2; m_fogPlayerAgent = -1; }

        if (m_fogPlayerAgent >= 0)
        {
            auto pit = m_replayCtx.agents.find(m_fogPlayerAgent);
            std::string pname = (pit != m_replayCtx.agents.end())
                ? pit->second.partyBarLabel : std::format("#{}", m_fogPlayerAgent);
            std::string pillLabel = pname + "  \xc3\x97";
            if (FogPill(pillLabel.c_str(), true, 0))
                m_fogPlayerAgent = -1;
        }

        ImGui::Separator();

        ImGui::TextColored(ImVec4(0.78f, 0.72f, 0.55f, 1.f), "Enemies");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.10f, 0.12f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.15f, 0.14f, 0.10f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.f, 0.84f, 0.39f, 1.f));
        if (ImGui::RadioButton("Hide", !m_fogGhostMode)) m_fogGhostMode = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Ghost", m_fogGhostMode)) m_fogGhostMode = true;
        ImGui::PopStyleColor(3);

        ImGui::Separator();

        bool inPlayerMode = (m_fogPlayerAgent >= 0);
        int sourceCount = 0;
        bool playerDead = false;
        std::string playerDeadName;

        if (inPlayerMode)
        {
            auto pit = m_replayCtx.agents.find(m_fogPlayerAgent);
            if (pit != m_replayCtx.agents.end() && !pit->second.snapshots.empty())
            {
                if (pit->second.isDeadAtTime(m_debugTimeline))
                {
                    playerDead = true;
                    playerDeadName = pit->second.partyBarLabel;
                }
                else
                    sourceCount = 1;
            }
        }
        else
        {
            for (auto& [aid, ard] : m_replayCtx.agents) {
                if (ard.teamId != m_fogPerspective) continue;
                if (ard.type != AgentType::Player) continue;
                if (ard.snapshots.empty()) continue;
                if (!ard.isDeadAtTime(m_debugTimeline)) sourceCount++;
            }
        }

        ImU32 dotCol;
        if (playerDead || sourceCount == 0)
            dotCol = IM_COL32(220, 60, 60, 255);
        else if (inPlayerMode)
            dotCol = IM_COL32(64, 220, 80, 255);
        else if (sourceCount >= 8)
            dotCol = IM_COL32(64, 220, 80, 255);
        else if (sourceCount >= 5)
            dotCol = IM_COL32(230, 180, 40, 255);
        else
            dotCol = IM_COL32(220, 60, 60, 255);

        ImVec2 cur = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float dotR = 4.f;
        float textH = ImGui::GetTextLineHeight();
        dl->AddCircleFilled(ImVec2(cur.x + dotR, cur.y + textH * 0.5f), dotR, dotCol);
        ImGui::Dummy(ImVec2(dotR * 2.f + 4.f, 0));
        ImGui::SameLine();

        if (playerDead)
            ImGui::Text("0 / 8 -- %s is dead", playerDeadName.c_str());
        else if (inPlayerMode)
            ImGui::Text("1 / 8 players as source");
        else
            ImGui::Text("%d / 8 players visible", sourceCount);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);

    if (!fogOpen)
        m_fogPerspective = 0;

    if (m_fogPerspective > 0 && m_fogPlayerAgent >= 0)
    {
        auto pit = m_replayCtx.agents.find(m_fogPlayerAgent);
        if (pit != m_replayCtx.agents.end() && !pit->second.snapshots.empty()
            && pit->second.isDeadAtTime(m_debugTimeline))
        {
            std::string msg = pit->second.partyBarLabel + " \xe2\x80\x94 No vision";
            ImVec2 txtSz = ImGui::CalcTextSize(msg.c_str());
            auto* fgDl = ImGui::GetForegroundDrawList();
            auto* mvp  = ImGui::GetMainViewport();
            ImVec2 center(mvp->Pos.x + mvp->Size.x * 0.5f, mvp->Pos.y + mvp->Size.y * 0.5f);
            fgDl->AddText(nullptr, 13.f,
                ImVec2(center.x - txtSz.x * 0.5f, center.y - txtSz.y * 0.5f),
                IM_COL32(255, 255, 255, 102), msg.c_str());
        }
    }
}
