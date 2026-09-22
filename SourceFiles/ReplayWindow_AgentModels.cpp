#include "pch.h"

// The stand-in pool audit: loads EVERY entry in every pool so the run log prints each one's
// skeleton signature in a single pass, instead of waiting for a match to happen to draw it. Costs
// ~60 model loads, so it is off in a shipping build - set to 1, run any replay once, read the
// 'pool audit' lines, set it back.
#ifndef GWO_AUDIT_MODEL_POOLS
#define GWO_AUDIT_MODEL_POOLS 0
#endif

#include "TeamColors.h"
#include "ReplayWindow.h"
#include "AssetBlacklist.h"
#include "RunLog.h"
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
static const char kCylinderHLSL[] = R"(
cbuffer CBPerFrame : register(b0)
{
    float4x4 gViewProj;
    float4   gCamPos;
};
cbuffer CBPerInstance : register(b1)
{
    float4x4 gWorld;
    float4   gTeamColor;
};

struct VS_IN  { float3 pos : POSITION; float3 nrm : NORMAL; float height01 : HEIGHT; };
struct VS_OUT { float4 pos : SV_Position; float3 worldPos : TEXCOORD0;
                float3 worldNrm : TEXCOORD1; float  height01 : TEXCOORD2; };

VS_OUT VSMain(VS_IN i)
{
    VS_OUT o;
    float4 wp = mul(float4(i.pos, 1), gWorld);
    o.pos      = mul(wp, gViewProj);
    o.worldPos = wp.xyz;
    o.worldNrm = normalize(mul(float4(i.nrm, 0), gWorld).xyz);
    o.height01 = i.height01;
    return o;
}

float4 PSMain(VS_OUT i) : SV_Target
{
    float3 baseColor = gTeamColor.rgb;

    // Ambient light from above (Y is up in GWMB render space)
    float3 lightDir = normalize(float3(0.2, 1.0, 0.3));
    float ndl = saturate(dot(i.worldNrm, lightDir));

    // View direction for specular on top cap
    float3 viewDir = normalize(gCamPos.xyz - i.worldPos);
    float3 halfVec = normalize(lightDir + viewDir);
    float spec = pow(saturate(dot(i.worldNrm, halfVec)), 32.0);

    // Vertical gradient: darker at bottom, brighter toward top
    float vertBright = lerp(0.55, 1.0, i.height01);

    // Facing camera brightness (body sides brighter at center)
    float facing = saturate(dot(i.worldNrm, viewDir));
    float faceBright = lerp(0.6, 1.0, facing * facing);

    // Top cap gets extra brightness + specular (Y is up)
    float isTop = saturate(i.worldNrm.y * 4.0 - 3.0);
    float topBright = lerp(1.0, 1.3, isTop);

    float3 color = baseColor * (0.35 + 0.65 * ndl) * vertBright * faceBright * topBright;
    color += spec * isTop * 0.4;

    // Base glow: additive team color emission at the very bottom
    float baseGlow = saturate(1.0 - i.height01 * 8.0) * 0.35;
    color += baseColor * baseGlow;

    return float4(saturate(color), gTeamColor.a);
}
)";

static constexpr int kAgentModelBatchSize = 2;

// ReplayWindow:: member functions; only their definitions live here.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Progressive agent model loading: called each frame during FadingOut & Ready
// ---------------------------------------------------------------------------

void ReplayWindow::ProgressiveAgentModelPump()
{
    if (!m_useAgentModels) return;
    if (m_agentModelsLoaded) {
        // Recorded players become their own characters once the stand-in models are in - their
        // animation banks are what a character is posed against. One step per frame, so the
        // timeline never waits for more than one.
        StepPlayerVisuals();
        return;
    }

    // If agents weren't classified when StepPlaceProps finished, retry here
    if (!m_agentModelsLoading && m_agentsClassified)
        LoadAgentModelsAsync();

    // Once the background IO thread is done, create GPU resources in batches
    if (m_agentModelsLoading && m_bgLoadDone)
        StepCreateAgentModelResources();
}


// ---------------------------------------------------------------------------
// In-scene banner: shows agent model loading progress during playback
// ---------------------------------------------------------------------------

void ReplayWindow::DrawAgentModelLoadingBanner()
{
    bool isLoading = m_useAgentModels && m_agentModelsLoading && !m_agentModelsLoaded;

    float dt = static_cast<float>(m_timer.GetElapsedSeconds());
    if (isLoading)
        m_agentLoadBannerFade = std::min(m_agentLoadBannerFade + dt * 4.f, 1.f);
    else
        m_agentLoadBannerFade = std::max(m_agentLoadBannerFade - dt * 2.f, 0.f);

    if (m_agentLoadBannerFade <= 0.001f) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float vpW = vp->Size.x;
    const float alpha = m_agentLoadBannerFade;

    const float bannerW = std::min(340.f, vpW * 0.35f);
    const float bannerH = 32.f;
    const float bannerX = (vpW - bannerW) * 0.5f + vp->Pos.x;
    const float playbarH = 76.f;
    const float bannerY = vp->Pos.y + vp->Size.y - playbarH - bannerH - 6.f;

    const ImU32 cBg      = IM_COL32(  8,   9,  12, static_cast<int>(180 * alpha));
    const ImU32 cBorder  = IM_COL32(160, 120,  40, static_cast<int>( 46 * alpha));
    const ImU32 cTrackBg = IM_COL32( 30,  28,  22, static_cast<int>(200 * alpha));
    const ImU32 cGold    = IM_COL32(200, 168,  75, static_cast<int>(255 * alpha));
    const ImU32 cGoldDim = IM_COL32(122,  96,  32, static_cast<int>(255 * alpha));
    const ImU32 cText    = IM_COL32(200, 184, 140, static_cast<int>(220 * alpha));

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    dl->AddRectFilled(ImVec2(bannerX, bannerY),
                      ImVec2(bannerX + bannerW, bannerY + bannerH), cBg, 4.f);
    dl->AddRect(ImVec2(bannerX, bannerY),
                ImVec2(bannerX + bannerW, bannerY + bannerH), cBorder, 4.f);

    float progress = 0.f;
    std::string label;
    int total = m_bgLoadTotal;
    if (total > 0) {
        int ioProgress = m_bgLoadProgress.load();
        if (!m_bgLoadDone) {
            progress = static_cast<float>(ioProgress) / total * 0.8f;

            const char* phaseNames[] = {
                "", "Parsing 3D models", "Loading textures", "Loading animations",
                "Loading animations", "Loading animations", "Loading animations"
            };
            int phase = m_bgLoadSubPhase.load();
            if (phase >= 0 && phase < 7)
                label = std::format("{}  ({}/{})", phaseNames[phase], ioProgress, total);
            else
                label = std::format("Loading 3D models  ({}/{})", ioProgress, total);
        } else {
            int createTotal = static_cast<int>(m_agentModelCreateOrder.size());
            float createFrac = createTotal > 0
                ? static_cast<float>(m_agentModelCreateIndex) / createTotal : 1.f;
            progress = 0.8f + 0.2f * createFrac;
            label = std::format("Preparing 3D models  ({}/{})", m_agentModelCreateIndex, createTotal);
        }
    }

    float padX = 10.f;
    float trackY = bannerY + bannerH - 8.f;
    float trackH = 3.f;
    float trackW = bannerW - padX * 2.f;
    float trackX = bannerX + padX;

    dl->AddRectFilled(ImVec2(trackX, trackY), ImVec2(trackX + trackW, trackY + trackH), cTrackBg, 1.5f);
    float fillW = trackW * std::clamp(progress, 0.f, 1.f);
    if (fillW > 0.f)
        dl->AddRectFilledMultiColor(
            ImVec2(trackX, trackY), ImVec2(trackX + fillW, trackY + trackH),
            cGoldDim, cGold, cGold, cGoldDim);

    if (!label.empty()) {
        ImFont* font = ImGui::GetFont();
        float fontSize = font->FontSize * 0.85f;
        ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, label.c_str());
        float textX = bannerX + (bannerW - textSz.x) * 0.5f;
        float textY = bannerY + (bannerH - trackH - 4.f - textSz.y) * 0.5f + 1.f;
        dl->AddText(font, fontSize, ImVec2(textX + 1.f, textY + 1.f),
                    IM_COL32(0, 0, 0, static_cast<int>(120 * alpha)), label.c_str());
        dl->AddText(font, fontSize, ImVec2(textX, textY), cText, label.c_str());
    }
}


void ReplayWindow::InitCylinderRenderer()
{
    if (m_cylInitialized) return;
    m_cylInitialized = true;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();

    // --- Compile shaders ---
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif

    HRESULT hr = D3DCompile(kCylinderHLSL, sizeof(kCylinderHLSL), nullptr, nullptr, nullptr,
                            "VSMain", "vs_5_0", flags, 0, vsBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return;

    hr = D3DCompile(kCylinderHLSL, sizeof(kCylinderHLSL), nullptr, nullptr, nullptr,
                    "PSMain", "ps_5_0", flags, 0, psBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return;

    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_cylVS.GetAddressOf());
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_cylPS.GetAddressOf());

    // --- Input layout ---
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "HEIGHT",   0, DXGI_FORMAT_R32_FLOAT,           0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    dev->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_cylIL.GetAddressOf());

    // --- Generate cylinder geometry ---
    constexpr int   SEG = 16;
    constexpr float R   = 30.f;
    constexpr float H   = 120.f;
    constexpr float PI2 = 6.2831853f;

    // Vertices: bottom ring + top ring + bottom center + top center
    // bottom ring: SEG verts, top ring: SEG verts
    // Each side quad uses 2 bottom + 2 top (shared normals per segment)
    // Plus 2 cap centers
    // Total: SEG*2 (body) + 1 (bottom center) + SEG (bottom fan) + 1 (top center) + SEG (top fan)
    // Simplify: body ring duplicated for caps to have different normals

    std::vector<CylVertex> verts;
    std::vector<uint16_t> indices;

    // Body vertices — Y is UP in GWMB render space (ApplyMapTransformToPos already converts)
    int bodyBase = 0;
    for (int i = 0; i <= SEG; ++i)
    {
        float a = (float(i) / SEG) * PI2;
        float cs = cosf(a), sn = sinf(a);
        float nx = cs, nz = sn;
        // Bottom (y=0)
        verts.push_back({ R * cs, 0.f, R * sn,  nx, 0, nz,  0.f });
        // Top (y=H)
        verts.push_back({ R * cs, H,   R * sn,  nx, 0, nz,  1.f });
    }

    // Body indices (triangle strip as quads)
    for (int i = 0; i < SEG; ++i)
    {
        int b0 = bodyBase + i * 2;
        int b1 = bodyBase + i * 2 + 1;
        int b2 = bodyBase + (i + 1) * 2;
        int b3 = bodyBase + (i + 1) * 2 + 1;
        indices.push_back((uint16_t)b0); indices.push_back((uint16_t)b1); indices.push_back((uint16_t)b2);
        indices.push_back((uint16_t)b2); indices.push_back((uint16_t)b1); indices.push_back((uint16_t)b3);
    }

    // Bottom cap (normal pointing down: -Y)
    int botCenter = (int)verts.size();
    verts.push_back({ 0, 0, 0,  0, -1, 0,  0.f });
    int botRing = (int)verts.size();
    for (int i = 0; i < SEG; ++i)
    {
        float a = (float(i) / SEG) * PI2;
        verts.push_back({ R * cosf(a), 0, R * sinf(a),  0, -1, 0,  0.f });
    }
    for (int i = 0; i < SEG; ++i)
    {
        indices.push_back((uint16_t)botCenter);
        indices.push_back((uint16_t)(botRing + (i + 1) % SEG));
        indices.push_back((uint16_t)(botRing + i));
    }

    // Top cap (normal pointing up: +Y)
    int topCenter = (int)verts.size();
    verts.push_back({ 0, H, 0,  0, 1, 0,  1.f });
    int topRing = (int)verts.size();
    for (int i = 0; i < SEG; ++i)
    {
        float a = (float(i) / SEG) * PI2;
        verts.push_back({ R * cosf(a), H, R * sinf(a),  0, 1, 0,  1.f });
    }
    for (int i = 0; i < SEG; ++i)
    {
        indices.push_back((uint16_t)topCenter);
        indices.push_back((uint16_t)(topRing + i));
        indices.push_back((uint16_t)(topRing + (i + 1) % SEG));
    }

    m_cylIndexCount = (UINT)indices.size();

    // Vertex buffer
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = (UINT)(verts.size() * sizeof(CylVertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = { verts.data(), 0, 0 };
    dev->CreateBuffer(&vbd, &vsd, m_cylVB.GetAddressOf());

    // Index buffer
    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = (UINT)(indices.size() * sizeof(uint16_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd = { indices.data(), 0, 0 };
    dev->CreateBuffer(&ibd, &isd, m_cylIB.GetAddressOf());

    // Constant buffers
    D3D11_BUFFER_DESC cbd = {};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    cbd.ByteWidth = sizeof(CylPerFrame);
    dev->CreateBuffer(&cbd, nullptr, m_cylCBFrame.GetAddressOf());

    cbd.ByteWidth = sizeof(CylPerInst);
    dev->CreateBuffer(&cbd, nullptr, m_cylCBInst.GetAddressOf());

    // Rasterizer: solid, no culling (overlay cylinders visible from all angles)
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthBias = 0;
    rd.DepthBiasClamp = 0.f;
    rd.SlopeScaledDepthBias = 0.f;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, m_cylRS.GetAddressOf());

    // Depth-stencil: depth disabled — cylinders render as 3D overlay, always visible
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dev->CreateDepthStencilState(&dsd, m_cylDSS.GetAddressOf());

    // Blend: disabled — cylinders are fully opaque
    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, m_cylBS.GetAddressOf());

    // --- Generate pillar geometry (thin cylinder for medium LOD) ---
    {
        constexpr int   P_SEG = 8;
        constexpr float P_R   = 6.f;
        constexpr float P_H   = 120.f;

        std::vector<CylVertex> pv;
        std::vector<uint16_t>  pi;

        int pBase = 0;
        for (int i = 0; i <= P_SEG; ++i)
        {
            float a = (float(i) / P_SEG) * PI2;
            float cs = cosf(a), sn = sinf(a);
            pv.push_back({ P_R * cs, 0.f,  P_R * sn,  cs, 0, sn,  0.f });
            pv.push_back({ P_R * cs, P_H,  P_R * sn,  cs, 0, sn,  1.f });
        }
        for (int i = 0; i < P_SEG; ++i)
        {
            int b0 = pBase + i * 2, b1 = b0 + 1, b2 = pBase + (i + 1) * 2, b3 = b2 + 1;
            pi.push_back((uint16_t)b0); pi.push_back((uint16_t)b1); pi.push_back((uint16_t)b2);
            pi.push_back((uint16_t)b2); pi.push_back((uint16_t)b1); pi.push_back((uint16_t)b3);
        }

        int pBotC = (int)pv.size();
        pv.push_back({ 0, 0, 0,  0, -1, 0,  0.f });
        int pBotR = (int)pv.size();
        for (int i = 0; i < P_SEG; ++i) {
            float a = (float(i) / P_SEG) * PI2;
            pv.push_back({ P_R * cosf(a), 0, P_R * sinf(a),  0, -1, 0,  0.f });
        }
        for (int i = 0; i < P_SEG; ++i) {
            pi.push_back((uint16_t)pBotC);
            pi.push_back((uint16_t)(pBotR + (i + 1) % P_SEG));
            pi.push_back((uint16_t)(pBotR + i));
        }

        int pTopC = (int)pv.size();
        pv.push_back({ 0, P_H, 0,  0, 1, 0,  1.f });
        int pTopR = (int)pv.size();
        for (int i = 0; i < P_SEG; ++i) {
            float a = (float(i) / P_SEG) * PI2;
            pv.push_back({ P_R * cosf(a), P_H, P_R * sinf(a),  0, 1, 0,  1.f });
        }
        for (int i = 0; i < P_SEG; ++i) {
            pi.push_back((uint16_t)pTopC);
            pi.push_back((uint16_t)(pTopR + i));
            pi.push_back((uint16_t)(pTopR + (i + 1) % P_SEG));
        }

        m_pillarIndexCount = (UINT)pi.size();

        D3D11_BUFFER_DESC pvbd = {};
        pvbd.ByteWidth = (UINT)(pv.size() * sizeof(CylVertex));
        pvbd.Usage = D3D11_USAGE_IMMUTABLE;
        pvbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA pvsd = { pv.data(), 0, 0 };
        dev->CreateBuffer(&pvbd, &pvsd, m_pillarVB.GetAddressOf());

        D3D11_BUFFER_DESC pibd = {};
        pibd.ByteWidth = (UINT)(pi.size() * sizeof(uint16_t));
        pibd.Usage = D3D11_USAGE_IMMUTABLE;
        pibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA pisd = { pi.data(), 0, 0 };
        dev->CreateBuffer(&pibd, &pisd, m_pillarIB.GetAddressOf());
    }
}


// ---------------------------------------------------------------------------
// Load 3D models from the .dat for known NPC / spirit agent types.
// Creates per-agent mesh instances so each agent can have its own transform.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Async entry point: collects hashes, launches background IO thread
// ---------------------------------------------------------------------------

void ReplayWindow::LoadAgentModelsAsync()
{
    if (m_agentModelsLoaded || m_agentModelsLoading) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;
    // Avatar slots are decided here, once, from the per-agent model timelines. Starting before
    // those are distributed would build the match without any of its avatar models.
    if (!m_modelChangesBuilt) return;
    if (!m_datManager || !m_hashIndex) return;

    // 1. Collect unique file hashes and cache per-agent hash (main thread)
    //    Player variants are shuffled randomly per (profession, gender) group
    //    so that duplicate models are avoided until all variants are exhausted.
    std::unordered_set<uint32_t> uniqueHashes;

    // Count players per (profession, gender) pair
    std::unordered_map<uint64_t, int> profGenderCount;
    for (auto& [agentId, ard] : m_replayCtx.agents) {
        if (ard.type != AgentType::Player) continue;
        uint64_t key = ((uint64_t)ard.primaryProf << 1) | (ard.isFemale ? 1 : 0);
        profGenderCount[key]++;
    }

    // Build shuffled variant-index sequences per group
    std::mt19937 rng(std::random_device{}());
    std::unordered_map<uint64_t, std::vector<int>> profGenderShuffled;
    for (auto& [key, count] : profGenderCount) {
        int prof = static_cast<int>(key >> 1);
        bool isFemale = (key & 1) != 0;
        auto variants = GetPlayerModelVariants(prof, isFemale);
        int numVariants = static_cast<int>(variants.size());
        if (numVariants == 0) continue;

        auto& indices = profGenderShuffled[key];
        while (static_cast<int>(indices.size()) < count) {
            std::vector<int> round(numVariants);
            std::iota(round.begin(), round.end(), 0);
            std::shuffle(round.begin(), round.end(), rng);
            indices.insert(indices.end(), round.begin(), round.end());
        }
    }

    // Assign variants from the shuffled sequences
    std::unordered_map<uint64_t, int> profGenderAssignIdx;
    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        uint32_t fileHash = 0;
        if (ard.type == AgentType::Player) {
            uint64_t key = ((uint64_t)ard.primaryProf << 1) | (ard.isFemale ? 1 : 0);
            auto it = profGenderShuffled.find(key);
            if (it != profGenderShuffled.end() && !it->second.empty()) {
                int idx = profGenderAssignIdx[key]++;
                fileHash = LookupPlayerFileHash(ard.primaryProf, ard.isFemale,
                                                it->second[idx]);
            }
        } else {
            fileHash = LookupAgentFileHash(ard.type, ard.modelId);
        }
        if (fileHash == 0) continue;
        m_agentFileHashCache[agentId] = fileHash;
        uniqueHashes.insert(fileHash);
    }

    // Give every avatar form a player wears during the match its own model slot. The models are
    // built once here rather than on the fly, because a form goes up mid-fight and a DAT read on
    // that frame would stall the replay; an avatar that is never used costs nothing.
    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Player) continue;
        for (const auto& change : ard.modelChanges)
        {
            uint32_t avatarHash = LookupAvatarFileHash(change.modelId);
            if (avatarHash == 0) continue;   // base skin, costume or unknown transmog
            int slot = AvatarSlotKey(agentId, change.modelId);
            if (m_agentFileHashCache.count(slot)) continue;
            m_agentFileHashCache[slot] = avatarHash;
            uniqueHashes.insert(avatarHash);
        }
    }

#if GWO_AUDIT_MODEL_POOLS
    // ---- THE POOL AUDIT -------------------------------------------------------------------
    //
    // A stand-in is not just a look: it is the RIG a composed character is posed by, so every
    // entry has to sit on the skeleton its profession's armour binds to. Five entries were found
    // NOT to - each a named NPC of one profession built on another's skeleton - and they were
    // found one at a time, by someone noticing a deformed player in a match.
    //
    // This loads every entry in every pool so the run log prints each one's skeleton signature in
    // one pass, which is what turns "wait for it to show up" into a table that can be read. It is
    // compiled out by default because it costs ~60 model loads that a match does not need; build
    // with GWO_AUDIT_MODEL_POOLS=1, open any replay, and read the 'pool audit' lines.
    for (int prof = 1; prof <= 10; prof++) {
        for (int sex = 0; sex < 2; sex++) {
            for (uint32_t hash : GetPlayerModelVariants(prof, sex != 0))
                uniqueHashes.insert(hash);
        }
    }
    RunLog::Line("pool audit: ON - every stand-in in every pool is loaded this run so its"
                 " skeleton can be printed; this is NOT a normal load");
#endif

    if (uniqueHashes.empty()) { m_agentModelsLoaded = true; return; }

    m_agentModelCreateOrder.assign(uniqueHashes.begin(), uniqueHashes.end());
    m_bgLoadTotal = static_cast<int>(m_agentModelCreateOrder.size());
    m_bgLoadProgress.store(0);
    m_bgLoadDone.store(false);
    m_agentModelCreateIndex = 0;
    m_agentModelsLoading = true;

    m_agentModelLoadThread = std::thread([this]() { LoadAgentModelsIO(); });
}


// ---------------------------------------------------------------------------
// Background thread: DAT reads + CPU parsing, NO D3D calls
// ---------------------------------------------------------------------------

void ReplayWindow::LoadAgentModelsIO()
{
    auto ioStart = LoadClock::now();

    const auto& mft = m_datManager->get_MFT();

    HANDLE datHandle = m_datManager->open_dat_handle();
    if (datHandle == INVALID_HANDLE_VALUE || datHandle == nullptr) {
        m_bgLoadDone.store(true);
        return;
    }

    // Load persistent animation discovery cache and parsed clip cache
    uintmax_t datSize = 0;
    {
        auto cachePath = GW::Cache::AnimationDiscoveryCache::GetDefaultCachePath();
        auto datPath = m_datManager->get_filepath();
        try { datSize = std::filesystem::file_size(datPath); } catch (...) {}
        m_animDiscoveryCache.SetDatIdentity(datPath, datSize);
        m_animDiscoveryCache.LoadFromFile(cachePath, datPath, datSize);
        m_clipCache.Load(GW::Cache::AnimationClipCache::GetDefaultCachePath(), datSize);
    }

    struct FoundClipInfo {
        GW::Animation::AnimationClip clip;
        uint32_t sourceFileHash;
    };

    // Per-model geometry needed for bone extraction in Phase C
    struct GeoModelInfo {
        std::vector<uint8_t> extra_data;
        uint32_t u0 = 0, u1 = 0;
        std::vector<std::pair<bool, uint32_t>> vertexGroups;
    };

    struct ModelWorkItem {
        uint32_t fileHash = 0;
        int mftIndex = -1;
        AgentModelInstance tmpl;
        std::vector<FoundClipInfo> foundClips;
        std::vector<GeoModelInfo> geoModels;
        bool needsMftScan = false;
        bool valid = false;
    };

    std::vector<ModelWorkItem> workItems;
    workItems.reserve(m_agentModelCreateOrder.size());

    // ====== Phase A: Parse models, try cache + embedded + references ======
    m_bgLoadSubPhase.store(static_cast<int>(AgentLoadSubPhase::ParsingModel));

    for (uint32_t fileHash : m_agentModelCreateOrder)
    {
        ModelWorkItem wi;
        wi.fileHash = fileHash;

        auto hashIt = m_hashIndex->find(static_cast<int>(fileHash));
        if (hashIt == m_hashIndex->end() || hashIt->second.empty()) {
            workItems.push_back(std::move(wi));
            m_bgLoadProgress.fetch_add(1);
            continue;
        }

        int mftIndex = hashIt->second.at(0);
        wi.mftIndex = mftIndex;
        if (mftIndex < 0 || mftIndex >= (int)mft.size() || mft[mftIndex].type != FFNA_Type2) {
            workItems.push_back(std::move(wi));
            m_bgLoadProgress.fetch_add(1);
            continue;
        }

        bool isOtherFormat = m_datManager->is_other_model_format(mftIndex);
        FFNA_ModelFile modelFile;
        FFNA_ModelFile_Other modelFileOther;

        try {
            if (isOtherFormat)
                modelFileOther = m_datManager->parse_ffna_model_file_other(mftIndex);
            else
                modelFile = m_datManager->parse_ffna_model_file(mftIndex);
        } catch (...) {
            workItems.push_back(std::move(wi));
            m_bgLoadProgress.fetch_add(1);
            continue;
        }

        if (isOtherFormat && !modelFileOther.parsed_correctly) {
            workItems.push_back(std::move(wi));
            m_bgLoadProgress.fetch_add(1);
            continue;
        }
        if (!isOtherFormat && !modelFile.parsed_correctly) {
            workItems.push_back(std::move(wi));
            m_bgLoadProgress.fetch_add(1);
            continue;
        }

        const auto& geoModelsRef = isOtherFormat
            ? modelFileOther.geometry_chunk.models
            : modelFile.geometry_chunk.models;
        const size_t numModels = geoModelsRef.size();

        std::vector<Mesh> propMeshes;
        for (size_t j = 0; j < numModels; j++) {
            AMAT_file amat;
            int matRow = FFNA_ModelFile::kModernMaterialRowOrdinal;
            if (!isOtherFormat && !modelFile.AMAT_filenames_chunk.texture_filenames.empty()) {
            // A modern-format submodel's material row is the client's own assignment, and
            // the AMAT lookup and GetMesh have to agree on it - the AMAT's stage reorder only
            // fires when its stage count matches the row's, so two different rows silently
            // produce two different texture lists. One helper answers both.
                int amatHash = 0;
                if (modelFile.ModernMaterialForSubmodel((int)j, matRow, amatHash)) {
                    auto aIt = m_hashIndex->find(amatHash);
                    if (aIt != m_hashIndex->end() && !aIt->second.empty())
                        amat = m_datManager->parse_amat_file(aIt->second.at(0));
                }
            }
            Mesh mesh = isOtherFormat
                ? modelFileOther.GetMesh((int)j, amat)
                : modelFile.GetMesh((int)j, amat, matRow);
            if (mesh.indices.size() % 3 == 0 && !mesh.indices.empty())
                propMeshes.push_back(mesh);
        }
        if (propMeshes.empty()) {
            workItems.push_back(std::move(wi));
            m_bgLoadProgress.fetch_add(1);
            continue;
        }

        // Extract geometry bone data for Phase C (before modelFile goes out of scope)
        for (size_t si = 0; si < geoModelsRef.size() && si < propMeshes.size(); si++) {
            GeoModelInfo gmi;
            gmi.extra_data = geoModelsRef[si].extra_data;
            gmi.u0 = geoModelsRef[si].u0;
            gmi.u1 = geoModelsRef[si].u1;
            gmi.vertexGroups.reserve(geoModelsRef[si].vertices.size());
            for (const auto& mv : geoModelsRef[si].vertices)
                gmi.vertexGroups.push_back({ mv.has_group, mv.group });
            wi.geoModels.push_back(std::move(gmi));
        }

        // Textures
        const bool texOk = isOtherFormat
            ? modelFileOther.textures_parsed_correctly
            : modelFile.textures_parsed_correctly;
        std::vector<int> texFileHashes;
        if (texOk) {
            if (isOtherFormat) {
                for (const auto& tf : modelFileOther.texture_filenames_chunk.texture_filenames)
                    texFileHashes.push_back(decode_filename(tf.id0, tf.id1));
            } else {
                for (const auto& tf : modelFile.texture_filenames_chunk.texture_filenames)
                    texFileHashes.push_back(decode_filename(tf.id0, tf.id1));
            }
        }
        std::vector<ParsedTextureEntry> parsedTextures;
        for (int decoded : texFileHashes) {
            ParsedTextureEntry pt;
            pt.decodedHash = decoded;
            auto mit = m_hashIndex->find(decoded);
            if (mit != m_hashIndex->end()) {
                try {
                    pt.datTex = m_datManager->parse_ffna_texture_file(mit->second.at(0));
                    pt.hasDatTex = (pt.datTex.width > 0 && pt.datTex.height > 0);
                } catch (...) {}
            }
            parsedTextures.push_back(std::move(pt));
        }

        const bool hasNewTexStuff = isOtherFormat
            ? !modelFileOther.geometry_chunk.unknown_tex_stuff1.empty()
            : !modelFile.geometry_chunk.unknown_tex_stuff1.empty();

        // Bounding box
        float bbMinX = FLT_MAX, bbMinY = FLT_MAX, bbMinZ = FLT_MAX;
        float bbMaxX = -FLT_MAX, bbMaxY = -FLT_MAX, bbMaxZ = -FLT_MAX;
        for (const auto& mesh : propMeshes) {
            for (const auto& v : mesh.vertices) {
                bbMinX = std::min(bbMinX, v.position.x);
                bbMinY = std::min(bbMinY, v.position.y);
                bbMinZ = std::min(bbMinZ, v.position.z);
                bbMaxX = std::max(bbMaxX, v.position.x);
                bbMaxY = std::max(bbMaxY, v.position.y);
                bbMaxZ = std::max(bbMaxZ, v.position.z);
            }
        }

        wi.tmpl.nativeHeight = bbMaxY - bbMinY;
        wi.tmpl.nativeShadowRadius = 0.5f * std::hypot(bbMaxX - bbMinX, bbMaxZ - bbMinZ);
        wi.tmpl.nativeMinY = bbMinY;
        wi.tmpl.nativeCenter = { (bbMinX + bbMaxX) * 0.5f, (bbMinY + bbMaxY) * 0.5f, (bbMinZ + bbMaxZ) * 0.5f };
        wi.tmpl.originalMeshes = propMeshes;
        wi.tmpl.parsedTextures_ = std::move(parsedTextures);
        wi.tmpl.pixelShaderType = hasNewTexStuff ? PixelShaderType::NewModel : PixelShaderType::OldModel;
        wi.tmpl.texturesOk = texOk;
        if (isOtherFormat) {
            wi.tmpl.modelHash0 = modelFileOther.geometry_chunk.header.model_hash0;
            wi.tmpl.modelHash1 = modelFileOther.geometry_chunk.header.model_hash1;
        } else {
            wi.tmpl.modelHash0 = modelFile.geometry_chunk.sub_1.f0xC;
            wi.tmpl.modelHash1 = modelFile.geometry_chunk.sub_1.f0x10;
        }
        wi.valid = true;

        // Try animation cache
        const auto* cachedAnimInfo = m_animDiscoveryCache.GetModel(fileHash);
        bool usedCache = false;
        if (cachedAnimInfo && !cachedAnimInfo->animSources.empty() &&
            cachedAnimInfo->modelHash0 == wi.tmpl.modelHash0 &&
            cachedAnimInfo->modelHash1 == wi.tmpl.modelHash1)
        {
            for (const auto& src : cachedAnimInfo->animSources) {
                if (src.mftIndex < 0 || src.mftIndex >= (int)mft.size()) continue;
                // Try parsed clip cache first
                auto* cached = m_clipCache.Get(src.mftIndex, src.fileHash);
                if (cached) {
                    wi.foundClips.push_back({ *cached, src.fileHash });
                    continue;
                }
                try {
                    uint8_t* data = m_datManager->read_file(src.mftIndex, datHandle);
                    if (!data) continue;
                    size_t dataSize = mft[src.mftIndex].uncompressedSize;
                    auto clip = GW::Parsers::ParseAnimationFromFile(data, dataSize);
                    delete[] data;
                    if (clip && clip->IsValid()) {
                        m_clipCache.Put(src.mftIndex, src.fileHash, *clip);
                        wi.foundClips.push_back({ std::move(*clip), src.fileHash });
                    }
                } catch (...) {}
            }
            usedCache = !wi.foundClips.empty();
        }

        // Try embedded + references if cache missed
        if (!usedCache) {
            try {
                uint8_t* rawData = m_datManager->read_file(mftIndex, datHandle);
                if (rawData) {
                    size_t rawSize = mft[mftIndex].uncompressedSize;
                    auto clipOpt = GW::Parsers::ParseAnimationFromFile(rawData, rawSize);
                    if (clipOpt && clipOpt->IsValid())
                        wi.foundClips.push_back({ std::move(*clipOpt), fileHash });

                    std::vector<uint32_t> animFileIds;
                    size_t scanOffset = 5;
                    while (scanOffset + 8 <= rawSize) {
                        uint32_t chunkId, chunkSize;
                        std::memcpy(&chunkId, &rawData[scanOffset], sizeof(uint32_t));
                        std::memcpy(&chunkSize, &rawData[scanOffset + 4], sizeof(uint32_t));
                        if (chunkId == 0 || chunkSize == 0 || scanOffset + 8 + chunkSize > rawSize) break;
                        const uint8_t* chunkData = &rawData[scanOffset + 8];
                        bool isAnimRef = (chunkId == GW::Parsers::CHUNK_ID_BBD ||
                                          chunkId == GW::Parsers::CHUNK_ID_FA8);
                        if (isAnimRef && chunkSize >= 4) {
                            uint32_t count;
                            size_t entryOffset;
                            if (chunkId == GW::Parsers::CHUNK_ID_FA8) {
                                std::memcpy(&count, &chunkData[0], sizeof(uint32_t));
                                entryOffset = 4;
                            } else {
                                if (chunkSize < 8) { scanOffset += 8 + chunkSize; continue; }
                                std::memcpy(&count, &chunkData[4], sizeof(uint32_t));
                                entryOffset = 8;
                            }
                            size_t maxEntries = (chunkSize - entryOffset) / 6;
                            if (count > maxEntries) count = static_cast<uint32_t>(maxEntries);
                            for (uint32_t i = 0; i < count; i++) {
                                if (entryOffset + 6 > chunkSize) break;
                                uint16_t id0, id1;
                                std::memcpy(&id0, &chunkData[entryOffset], sizeof(uint16_t));
                                std::memcpy(&id1, &chunkData[entryOffset + 2], sizeof(uint16_t));
                                entryOffset += 6;
                                int32_t fid = static_cast<int32_t>(id0) - 0xFF00FF;
                                fid += static_cast<int32_t>(id1) * 0xFF00;
                                animFileIds.push_back(static_cast<uint32_t>(fid));
                            }
                        }
                        scanOffset += 8 + chunkSize;
                    }

                    for (uint32_t animFileId : animFileIds) {
                        auto refIt = m_hashIndex->find(static_cast<int>(animFileId));
                        if (refIt == m_hashIndex->end() || refIt->second.empty()) continue;
                        int refMftIdx = refIt->second.at(0);
                        if (refMftIdx < 0 || refMftIdx >= (int)mft.size()) continue;
                        // Try parsed clip cache first
                        auto* cached = m_clipCache.Get(refMftIdx, animFileId);
                        if (cached) {
                            wi.foundClips.push_back({ *cached, animFileId });
                            continue;
                        }
                        uint8_t* refData = m_datManager->read_file(refMftIdx, datHandle);
                        if (!refData) continue;
                        size_t refSize = mft[refMftIdx].uncompressedSize;
                        auto refClip = GW::Parsers::ParseAnimationFromFile(refData, refSize);
                        delete[] refData;
                        if (refClip && refClip->IsValid()) {
                            m_clipCache.Put(refMftIdx, animFileId, *refClip);
                            wi.foundClips.push_back({ std::move(*refClip), animFileId });
                        }
                    }

                    if (wi.foundClips.empty() && wi.tmpl.modelHash0 != 0) {
                        m_bgLoadSubPhase.store(static_cast<int>(AgentLoadSubPhase::ScanningMFT));
                        // Use pre-built model hash index instead of scanning entire MFT
                        auto animIndices = m_datManager->FindAnimationFiles(wi.tmpl.modelHash0, wi.tmpl.modelHash1);
                        for (int mi : animIndices) {
                            if (mi == mftIndex) continue;
                            uint32_t fHash = static_cast<uint32_t>(mft[mi].Hash);
                            auto* cached = m_clipCache.Get(mi, fHash);
                            if (cached) {
                                wi.foundClips.push_back({ *cached, fHash });
                                continue;
                            }
                            uint8_t* scanData = m_datManager->read_file(mi, datHandle);
                            if (!scanData) continue;
                            size_t scanSize = mft[mi].uncompressedSize;
                            auto scanClip = GW::Parsers::ParseAnimationFromFile(scanData, scanSize);
                            if (scanClip && scanClip->IsValid()) {
                                m_clipCache.Put(mi, fHash, *scanClip);
                                wi.foundClips.push_back({ std::move(*scanClip), fHash });
                            }
                            delete[] scanData;
                        }
                    }

                    delete[] rawData;
                }
            } catch (...) {}

            if (wi.foundClips.empty() && wi.tmpl.modelHash0 != 0)
                wi.needsMftScan = true;
        }

        workItems.push_back(std::move(wi));
        m_bgLoadProgress.fetch_add(1);
    }

    // ====== Phase B: Single MFT scan for ALL models needing discovery ======
    // Build lookup: (modelHash0, modelHash1) → work item indices
    std::unordered_map<uint64_t, std::vector<size_t>> hashToWorkIndices;
    std::unordered_set<int> skipMftIndices;

    for (size_t idx = 0; idx < workItems.size(); idx++) {
        if (!workItems[idx].needsMftScan) continue;
        uint64_t key = (uint64_t(workItems[idx].tmpl.modelHash0) << 32) | workItems[idx].tmpl.modelHash1;
        hashToWorkIndices[key].push_back(idx);
        skipMftIndices.insert(workItems[idx].mftIndex);
    }

    if (!hashToWorkIndices.empty()) {
        m_bgLoadSubPhase.store(static_cast<int>(AgentLoadSubPhase::ScanningMFT));
        OutputDebugStringA(std::format("[ReplayLoad] MFT scan: {} models need discovery\n",
                                       hashToWorkIndices.size()).c_str());

        for (size_t mi = 0; mi < mft.size(); mi++) {
            if (mft[mi].type != FFNA_Type2) continue;
            if (mft[mi].uncompressedSize < 5 + 8 + 44) continue;
            if (skipMftIndices.count(static_cast<int>(mi))) continue;

            uint8_t* scanData = m_datManager->read_file(static_cast<int>(mi), datHandle);
            if (!scanData) continue;
            size_t scanSize = mft[mi].uncompressedSize;

            if (scanSize < 5 || scanData[0] != 'f' || scanData[1] != 'f' ||
                scanData[2] != 'n' || scanData[3] != 'a') {
                delete[] scanData;
                continue;
            }

            // Extract all BB9/FA1 hash pairs and check against needed models
            bool anyMatch = false;
            size_t off = 5;
            while (off + 8 <= scanSize) {
                uint32_t cid, csz;
                std::memcpy(&cid, &scanData[off], sizeof(uint32_t));
                std::memcpy(&csz, &scanData[off + 4], sizeof(uint32_t));
                if (cid == 0 || csz == 0 || off + 8 + csz > scanSize) break;
                size_t cDataOff = off + 8;

                uint32_t h0 = 0, h1 = 0;
                bool gotHash = false;
                if (cid == GW::Parsers::CHUNK_ID_BB9 &&
                    cDataOff + sizeof(GW::Parsers::BB9Header) <= scanSize) {
                    GW::Parsers::BB9Header hdr;
                    std::memcpy(&hdr, &scanData[cDataOff], sizeof(hdr));
                    h0 = hdr.modelHash0;
                    h1 = hdr.modelHash1;
                    gotHash = true;
                } else if (cid == GW::Parsers::CHUNK_ID_FA1 &&
                           cDataOff + sizeof(GW::Parsers::FA1Header) <= scanSize) {
                    GW::Parsers::FA1Header hdr;
                    std::memcpy(&hdr, &scanData[cDataOff], sizeof(hdr));
                    h0 = hdr.boundingBoxId;
                    h1 = hdr.collisionMeshId;
                    gotHash = true;
                }

                if (gotHash) {
                    uint64_t key = (uint64_t(h0) << 32) | h1;
                    auto it = hashToWorkIndices.find(key);
                    if (it != hashToWorkIndices.end()) {
                        anyMatch = true;
                        break;
                    }
                }
                off += 8 + csz;
            }

            if (anyMatch) {
                auto scanClip = GW::Parsers::ParseAnimationFromFile(scanData, scanSize);
                if (scanClip && scanClip->IsValid()) {
                    uint32_t fHash = static_cast<uint32_t>(mft[mi].Hash);
                    off = 5;
                    while (off + 8 <= scanSize) {
                        uint32_t cid, csz;
                        std::memcpy(&cid, &scanData[off], sizeof(uint32_t));
                        std::memcpy(&csz, &scanData[off + 4], sizeof(uint32_t));
                        if (cid == 0 || csz == 0 || off + 8 + csz > scanSize) break;
                        size_t cDataOff = off + 8;

                        uint32_t h0 = 0, h1 = 0;
                        bool gotHash = false;
                        if (cid == GW::Parsers::CHUNK_ID_BB9 &&
                            cDataOff + sizeof(GW::Parsers::BB9Header) <= scanSize) {
                            GW::Parsers::BB9Header hdr;
                            std::memcpy(&hdr, &scanData[cDataOff], sizeof(hdr));
                            h0 = hdr.modelHash0; h1 = hdr.modelHash1; gotHash = true;
                        } else if (cid == GW::Parsers::CHUNK_ID_FA1 &&
                                   cDataOff + sizeof(GW::Parsers::FA1Header) <= scanSize) {
                            GW::Parsers::FA1Header hdr;
                            std::memcpy(&hdr, &scanData[cDataOff], sizeof(hdr));
                            h0 = hdr.boundingBoxId; h1 = hdr.collisionMeshId; gotHash = true;
                        }
                        if (gotHash) {
                            uint64_t key = (uint64_t(h0) << 32) | h1;
                            auto it = hashToWorkIndices.find(key);
                            if (it != hashToWorkIndices.end()) {
                                for (size_t wiIdx : it->second)
                                    workItems[wiIdx].foundClips.push_back({ *scanClip, fHash });
                            }
                        }
                        off += 8 + csz;
                    }
                }
            }
            delete[] scanData;
        }
    }

    // ====== Phase C: Build animation data + finalize templates ======
    m_bgLoadSubPhase.store(static_cast<int>(AgentLoadSubPhase::BuildingAnimData));

    for (auto& wi : workItems) {
        if (!wi.valid) {
            m_agentModelTemplates[wi.fileHash] = std::move(wi.tmpl);
            continue;
        }

        // Update discovery cache for models that did fresh discovery
        if (wi.needsMftScan && !wi.foundClips.empty()) {
            GW::Cache::CachedModelAnimInfo cacheEntry;
            cacheEntry.modelHash0 = wi.tmpl.modelHash0;
            cacheEntry.modelHash1 = wi.tmpl.modelHash1;
            for (const auto& fc : wi.foundClips) {
                GW::Cache::AnimSourceEntry src;
                src.fileHash = fc.sourceFileHash;
                auto srcIt = m_hashIndex->find(static_cast<int>(fc.sourceFileHash));
                if (srcIt != m_hashIndex->end() && !srcIt->second.empty())
                    src.mftIndex = srcIt->second.at(0);
                else
                    src.mftIndex = wi.mftIndex;
                cacheEntry.animSources.push_back(src);
            }
            m_animDiscoveryCache.SetModel(wi.fileHash, std::move(cacheEntry));
        }
        // The pair this model actually sits on, against the pool it was listed in. Printed for
        // every model in an audit run; in a normal run it is one line per model the match uses,
        // which is cheap and is what makes a mismatch attributable after the fact.
        // ...and WHICH ANIMATION BANK it ended up with. A skeleton is shared between professions
        // by design - male Monk and Mesmer are one rig - but a BANK is not: the recorded animation
        // code is resolved against this clip's own segment table, so two professions holding the
        // same bank play each other's motions. The fallback discovery searches by skeleton hash
        // pair and takes allClips[0], which is exactly how that happens. Printing the source file
        // and the number of candidates is what makes it visible: two pools on one skeleton whose
        // models report the SAME source are sharing a bank.
        RunLog::Line("pool audit: model 0x%08X -> skeleton 0x%08X/0x%08X, bank 0x%08X of %zu"
                     " candidate(s)%s",
                     wi.fileHash, wi.tmpl.modelHash0, wi.tmpl.modelHash1,
                     wi.foundClips.empty() ? 0u : wi.foundClips[0].sourceFileHash,
                     wi.foundClips.size(),
                     DescribePlayerModelPool(wi.fileHash).c_str());

        // Also cache models resolved via embedded/refs (not from cache hit, not needing MFT scan)
        if (!wi.needsMftScan && !wi.foundClips.empty()) {
            const auto* existing = m_animDiscoveryCache.GetModel(wi.fileHash);
            if (!existing) {
                GW::Cache::CachedModelAnimInfo cacheEntry;
                cacheEntry.modelHash0 = wi.tmpl.modelHash0;
                cacheEntry.modelHash1 = wi.tmpl.modelHash1;
                for (const auto& fc : wi.foundClips) {
                    GW::Cache::AnimSourceEntry src;
                    src.fileHash = fc.sourceFileHash;
                    auto srcIt = m_hashIndex->find(static_cast<int>(fc.sourceFileHash));
                    if (srcIt != m_hashIndex->end() && !srcIt->second.empty())
                        src.mftIndex = srcIt->second.at(0);
                    else
                        src.mftIndex = wi.mftIndex;
                    cacheEntry.animSources.push_back(src);
                }
                m_animDiscoveryCache.SetModel(wi.fileHash, std::move(cacheEntry));
            }
        }

        if (!wi.foundClips.empty()) {
            for (size_t ci = 0; ci < wi.foundClips.size(); ci++) {
                AnimClipEntry entry;
                entry.clip = std::make_shared<GW::Animation::AnimationClip>(std::move(wi.foundClips[ci].clip));
                entry.clip->BuildAnimationGroups();
                entry.skeleton = std::make_shared<GW::Animation::Skeleton>(
                    GW::Parsers::BB9AnimationParser::CreateSkeleton(*entry.clip));
                entry.sourceFileHash = wi.foundClips[ci].sourceFileHash;
                wi.tmpl.allClips.push_back(std::move(entry));
            }
            // WHICH of the candidates, for a player stand-in. The discovery above falls back to
            // a search by SKELETON hash pair, and a skeleton is shared between professions while
            // an animation bank is not - so index 0 hands every profession on one rig the same
            // motions. Measured across all twenty pools: twelve of them collided this way, male
            // Mesmers playing the male Monk's bank among them. The right file was always already
            // in the list - 69 candidates for that rig, 126 for the shared female one - so this
            // chooses rather than loads.
            //
            // Anything that is not a player stand-in, and any character with no entry, keeps
            // index 0 exactly as before.
            size_t chosen = 0;
            bool bankPinned = false;
            int pool_prof = 0, pool_sex = 0;
            if (PlayerModelPoolIdentity(wi.fileHash, pool_prof, pool_sex)) {
                const uint32_t wanted = ReplayWindow::PreferredAnimationFileId(
                    pool_prof, pool_sex, wi.tmpl.modelHash0, wi.tmpl.modelHash1);
                if (wanted != 0) {
                    bool found = false;
                    for (size_t ci = 0; ci < wi.tmpl.allClips.size(); ci++) {
                        if (wi.tmpl.allClips[ci].sourceFileHash != wanted) continue;
                        chosen = ci;
                        found = true;
                        break;
                    }
                    bankPinned = found;
                    // Named but absent is worth a line: it means the file this character should
                    // play was not among the candidates, and the fallback is the old behaviour.
                    RunLog::Line("anim bank: model 0x%08X wants 0x%08X -> %s",
                                 wi.fileHash, wanted,
                                 found ? "found among the candidates"
                                       : "NOT FOUND, keeping the search's first match");
                }
            }

            wi.tmpl.clip = wi.tmpl.allClips[chosen].clip;
            wi.tmpl.skeleton = wi.tmpl.allClips[chosen].skeleton;

            // A player bank is mostly a TABLE OF CONTENTS. Most of its segments are EXTERNAL: a
            // non-zero source type k names entry #k of the bank's own FA8 table, and that file
            // carries the same hash as a LOCAL segment over the same time window (measured: every
            // external segment of the two banks checked). The bank itself has no keys in those
            // windows, so playing an external segment in place freezes the character - which is
            // what a male Paragon did once his own bank (only 39 local segments of 289) was the
            // one being read. For a pinned bank every
            // external segment is redirected to the file it names; the FA8 targets are loaded
            // here if discovery did not already bring them in.
            std::vector<SegmentRef> externalRedirect;
            if (bankPinned) {
                const auto& bankClip = *wi.tmpl.allClips[chosen].clip;
                const size_t segCount = bankClip.animationSegments.size();
                externalRedirect.assign(segCount, SegmentRef{ -1, -1 });

                std::vector<uint32_t> fa8;
                const uint32_t bankId = wi.tmpl.allClips[chosen].sourceFileHash;
                auto bankIt = m_hashIndex->find(static_cast<int>(bankId));
                if (bankIt != m_hashIndex->end() && !bankIt->second.empty()) {
                    const int bankMft = bankIt->second.at(0);
                    try {
                        uint8_t* bankData = m_datManager->read_file(bankMft, datHandle);
                        if (bankData) {
                            const size_t bankSize = mft[bankMft].uncompressedSize;
                            for (size_t off = 5; off + 8 <= bankSize;) {
                                uint32_t cid, csz;
                                std::memcpy(&cid, &bankData[off], sizeof(uint32_t));
                                std::memcpy(&csz, &bankData[off + 4], sizeof(uint32_t));
                                if (cid == 0 || csz == 0 || off + 8 + csz > bankSize) break;
                                if (cid == GW::Parsers::CHUNK_ID_FA8 && csz >= 4) {
                                    uint32_t count;
                                    std::memcpy(&count, &bankData[off + 8], sizeof(uint32_t));
                                    for (uint32_t e = 0; e < count && 4 + (e + 1) * 6 <= csz; e++) {
                                        uint16_t id0, id1;
                                        std::memcpy(&id0, &bankData[off + 8 + 4 + e * 6], sizeof(uint16_t));
                                        std::memcpy(&id1, &bankData[off + 8 + 4 + e * 6 + 2], sizeof(uint16_t));
                                        int32_t fid = static_cast<int32_t>(id0) - 0xFF00FF;
                                        fid += static_cast<int32_t>(id1) * 0xFF00;
                                        fa8.push_back(static_cast<uint32_t>(fid));
                                    }
                                }
                                off += 8 + csz;
                            }
                            delete[] bankData;
                        }
                    } catch (...) {}
                }

                // FA8 entry -> index in allClips, loading the ones discovery did not bring.
                std::vector<int> fa8Clip(fa8.size(), -1);
                for (size_t e = 0; e < fa8.size(); e++) {
                    for (size_t ci = 0; ci < wi.tmpl.allClips.size(); ci++) {
                        if (wi.tmpl.allClips[ci].sourceFileHash == fa8[e]) { fa8Clip[e] = static_cast<int>(ci); break; }
                    }
                    if (fa8Clip[e] >= 0) continue;
                    auto refIt = m_hashIndex->find(static_cast<int>(fa8[e]));
                    if (refIt == m_hashIndex->end() || refIt->second.empty()) continue;
                    const int refMft = refIt->second.at(0);
                    std::optional<GW::Animation::AnimationClip> refClip;
                    if (auto* cached = m_clipCache.Get(refMft, fa8[e])) {
                        refClip = *cached;
                    } else {
                        try {
                            uint8_t* refData = m_datManager->read_file(refMft, datHandle);
                            if (refData) {
                                refClip = GW::Parsers::ParseAnimationFromFile(refData, mft[refMft].uncompressedSize);
                                delete[] refData;
                                if (refClip && refClip->IsValid())
                                    m_clipCache.Put(refMft, fa8[e], *refClip);
                            }
                        } catch (...) {}
                    }
                    if (!refClip || !refClip->IsValid()) continue;
                    AnimClipEntry entry;
                    entry.clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*refClip));
                    entry.clip->BuildAnimationGroups();
                    entry.skeleton = std::make_shared<GW::Animation::Skeleton>(
                        GW::Parsers::BB9AnimationParser::CreateSkeleton(*entry.clip));
                    entry.sourceFileHash = fa8[e];
                    wi.tmpl.allClips.push_back(std::move(entry));
                    fa8Clip[e] = static_cast<int>(wi.tmpl.allClips.size()) - 1;
                }

                // allClips may have grown: re-take the bank by index, never by the old reference.
                const auto& bank = *wi.tmpl.allClips[chosen].clip;
                int external = 0, redirected = 0;
                for (size_t si = 0; si < segCount; si++) {
                    const uint8_t k = bank.GetSegmentSourceType(si);
                    if (k == 0) continue;
                    external++;
                    if (k > fa8.size() || fa8Clip[k - 1] < 0) continue;
                    const int tci = fa8Clip[k - 1];
                    const auto& seg = bank.animationSegments[si];
                    const auto& tsegs = wi.tmpl.allClips[tci].clip->animationSegments;
                    int hashOnly = -1;
                    for (size_t ti = 0; ti < tsegs.size(); ti++) {
                        if (wi.tmpl.allClips[tci].clip->GetSegmentSourceType(ti) != 0 ||
                            tsegs[ti].hash != seg.hash) continue;
                        if (tsegs[ti].startTime == seg.startTime) { hashOnly = static_cast<int>(ti); break; }
                        if (hashOnly < 0) hashOnly = static_cast<int>(ti);
                    }
                    if (hashOnly < 0) continue;
                    externalRedirect[si] = SegmentRef{ tci, hashOnly };
                    redirected++;
                }
                wi.tmpl.localSegmentsOnly = true;
                RunLog::Line("anim bank: model 0x%08X bank 0x%08X - FA8 %zu file(s), %d external"
                             " segment(s), %d redirected", wi.fileHash, bankId, fa8.size(),
                             external, redirected);
            }

            // Weapon attachment points. Bone ordering is consistent across every clip belonging
            // to one rig (verified: all 14 male-human clips agree on 86 bones and the same
            // indices), so resolving against clip 0 covers the whole rig. Re-validated at use
            // against whichever clip the controller currently holds.
            {
                const auto* cachedSocket = m_animDiscoveryCache.GetModel(wi.fileHash);
                if (cachedSocket && cachedSocket->socketResolved &&
                    cachedSocket->negativeXBone >= 0 && cachedSocket->positiveXBone >= 0 &&
                    static_cast<size_t>(cachedSocket->negativeXBone) < wi.tmpl.clip->boneTracks.size() &&
                    static_cast<size_t>(cachedSocket->positiveXBone) < wi.tmpl.clip->boneTracks.size())
                {
                    wi.tmpl.weaponSocket.negativeXBone = cachedSocket->negativeXBone;
                    wi.tmpl.weaponSocket.positiveXBone = cachedSocket->positiveXBone;
                    wi.tmpl.weaponSocket.resolved      = true;
                }
                else
                {
                    wi.tmpl.weaponSocket = GW::Animation::ResolveWeaponSocket(*wi.tmpl.clip);
                    m_animDiscoveryCache.SetSocket(wi.fileHash,
                                                   wi.tmpl.weaponSocket.negativeXBone,
                                                   wi.tmpl.weaponSocket.positiveXBone,
                                                   wi.tmpl.weaponSocket.resolved);
                }
            }

            for (size_t si = 0; si < wi.geoModels.size(); si++) {
                auto boneData = AnimationPanelState::ExtractBoneData(
                    wi.geoModels[si].extra_data, wi.geoModels[si].u0, wi.geoModels[si].u1);
                wi.tmpl.submeshBoneData.push_back(std::move(boneData));
                std::vector<uint32_t> vbg;
                vbg.reserve(wi.geoModels[si].vertexGroups.size());
                for (const auto& [hasGroup, group] : wi.geoModels[si].vertexGroups)
                    vbg.push_back(hasGroup ? group : 0);
                wi.tmpl.perVertexBoneGroups.push_back(std::move(vbg));
            }

            // The CHOSEN bank goes first. Every code below is first-match-wins, and every player
            // bank carries the same hashes (idle, run, cast...), so walking the candidates in
            // discovery order handed each code to whichever bank on the rig came first - and the
            // first code played re-initialises the controller onto that clip. Choosing `clip`
            // alone was not enough: male Mesmers still played the male Monk's bank and male
            // Paragons the male Ritualist's. The other candidates still fill codes it lacks.
            auto& clipOrder = wi.tmpl.clipOrder;
            clipOrder.clear();
            clipOrder.reserve(wi.tmpl.allClips.size());
            clipOrder.push_back(static_cast<int>(chosen));
            for (int ci = 0; ci < static_cast<int>(wi.tmpl.allClips.size()); ci++)
                if (ci != static_cast<int>(chosen)) clipOrder.push_back(ci);

            std::unordered_map<uint32_t, SegmentRef> segHashToRef;
            for (int ci : clipOrder) {
                const auto& clipEntry = wi.tmpl.allClips[ci];
                for (size_t si = 0; si < clipEntry.clip->animationSegments.size(); si++) {
                    uint32_t segHash = clipEntry.clip->animationSegments[si].hash;
                    SegmentRef ref{ ci, static_cast<int>(si) };
                    // An external segment has no keys of its own (see externalRedirect above):
                    // for a pinned bank it plays from the file it names, or not at all.
                    if (wi.tmpl.localSegmentsOnly && clipEntry.clip->GetSegmentSourceType(si) != 0) {
                        if (ci != static_cast<int>(chosen) || si >= externalRedirect.size() ||
                            externalRedirect[si].clipIndex < 0)
                            continue;
                        ref = externalRedirect[si];
                    }
                    if (!segHashToRef.count(segHash)) segHashToRef[segHash] = ref;
                    if (!wi.tmpl.animCodeToSegment.count(segHash)) wi.tmpl.animCodeToSegment[segHash] = ref;
                    auto& lookup = GW::Animation::AnimationHashLookup::Instance();
                    int stateIdx = lookup.GetStateIndex(segHash);
                    if (stateIdx >= 0 && stateIdx < (int)GW::Animation::g_animationStateCount) {
                        uint32_t primary = GW::Animation::g_animationStateTable[stateIdx].primaryHash;
                        if (!wi.tmpl.animCodeToSegment.count(primary))
                            wi.tmpl.animCodeToSegment[primary] = ref;
                    }
                    for (size_t bs = 0; bs < GW::Animation::g_boneSlotCount; bs++) {
                        uint32_t fallback = GW::Animation::ReverseSegmentHash(
                            segHash, GW::Animation::g_boneSlotChars[bs]);
                        if (fallback != 0 && !wi.tmpl.animCodeToSegment.count(fallback))
                            wi.tmpl.animCodeToSegment[fallback] = ref;
                    }
                }
            }
            for (size_t i = 0; i < GW::Animation::g_animationStateCount; i++) {
                uint32_t primary = GW::Animation::g_animationStateTable[i].primaryHash;
                if (wi.tmpl.animCodeToSegment.count(primary)) continue;
                for (size_t bs = 0; bs < GW::Animation::g_boneSlotCount; bs++) {
                    uint32_t candidate = GW::Animation::ComputeSegmentHash(
                        primary, GW::Animation::g_boneSlotChars[bs]);
                    auto it = segHashToRef.find(candidate);
                    if (it != segHashToRef.end()) {
                        wi.tmpl.animCodeToSegment[primary] = it->second;
                        break;
                    }
                }
            }
            wi.tmpl.hasAnimation = true;
        }

        m_agentModelTemplates[wi.fileHash] = std::move(wi.tmpl);
    }

    CloseHandle(datHandle);

    if (m_animDiscoveryCache.IsDirty()) {
        auto cachePath = GW::Cache::AnimationDiscoveryCache::GetDefaultCachePath();
        m_animDiscoveryCache.SaveToFile(cachePath);
    }

    if (m_clipCache.IsDirty()) {
        m_clipCache.Save(GW::Cache::AnimationClipCache::GetDefaultCachePath(), datSize);
    }

    m_loadTiming.agentIOSec = std::chrono::duration<double>(LoadClock::now() - ioStart).count();
    OutputDebugStringA(std::format("[ReplayLoad] AgentIO: {:.3f}s\n", m_loadTiming.agentIOSec).c_str());

    m_bgLoadDone.store(true);
}


void ReplayWindow::StepCreateAgentModelResources()
{
    if (!m_bgLoadDone || m_agentModelsLoaded) return;

    auto* map_renderer = m_mapRenderer.get();
    if (!map_renderer) return;
    auto* device = m_deviceResources->GetD3DDevice();
    if (!device) return;

    auto frameStart = LoadClock::now();
    int processed = 0;
    while (m_agentModelCreateIndex < static_cast<int>(m_agentModelCreateOrder.size()))
    {
        uint32_t fileHash = m_agentModelCreateOrder[m_agentModelCreateIndex];
        m_agentModelCreateIndex++;
        processed++;

        auto tmplIt = m_agentModelTemplates.find(fileHash);
        if (tmplIt == m_agentModelTemplates.end()) continue;

        auto& tmpl = tmplIt->second;
        auto& propMeshes = tmpl.originalMeshes;
        if (propMeshes.empty()) continue;

        // ---- GPU texture creation (same flow as working LoadAgentModels) ----
        std::vector<int> textureIds;
        for (const auto& pt : tmpl.parsedTextures_) {
            int texId = map_renderer->GetTextureManager()->GetTextureIdByHash(pt.decodedHash);
            if (texId >= 0) { textureIds.push_back(texId); continue; }
            if (pt.hasDatTex) {
                map_renderer->GetTextureManager()->CreateTextureFromRGBA(
                    pt.datTex.width, pt.datTex.height, pt.datTex.rgba_data.data(), &texId, pt.decodedHash);
            }
            textureIds.push_back(texId);
        }

        // ---- Remap per-mesh texture indices (same flow as working code) ----
        std::vector<std::vector<int>> perMeshTexIds(propMeshes.size());
        for (size_t k = 0; k < propMeshes.size(); k++) {
            std::vector<uint8_t> remappedIndices;
            for (size_t ti = 0; ti < propMeshes[k].tex_indices.size(); ti++) {
                int idx = std::min((int)propMeshes[k].tex_indices[ti], (int)textureIds.size() - 1);
                if (idx >= 0 && idx < (int)textureIds.size()) {
                    perMeshTexIds[k].push_back(textureIds[idx]);
                    remappedIndices.push_back((uint8_t)ti);
                }
            }
            propMeshes[k].tex_indices = remappedIndices;
        }

        // ---- Build template PerObjectCBs (after tex_indices remapping) ----
        std::vector<PerObjectCB> templateCBs(propMeshes.size());
        for (size_t j = 0; j < propMeshes.size(); j++) {
            XMStoreFloat4x4(&templateCBs[j].world, XMMatrixIdentity());
            auto& mesh = propMeshes[j];
            if (mesh.uv_coord_indices.size() == mesh.tex_indices.size() &&
                mesh.uv_coord_indices.size() < MAX_NUM_TEX_INDICES && tmpl.texturesOk) {
                templateCBs[j].num_uv_texture_pairs = (uint32_t)mesh.uv_coord_indices.size();
                for (size_t k = 0; k < mesh.uv_coord_indices.size(); k++) {
                    templateCBs[j].uv_indices[k / 4][k % 4] = (uint32_t)mesh.uv_coord_indices[k];
                    templateCBs[j].texture_indices[k / 4][k % 4] = (uint32_t)mesh.tex_indices[k];
                    templateCBs[j].blend_flags[k / 4][k % 4] = (uint32_t)mesh.blend_flags[k];
                    templateCBs[j].texture_types[k / 4][k % 4] = (uint32_t)mesh.texture_types[k];
                }
            }
        }
        tmpl.templateCBs = templateCBs;

        // ---- Create props and skinned meshes per agent ----
        for (auto& [slotKey, cachedHash] : m_agentFileHashCache)
        {
            if (cachedHash != fileHash) continue;

            // Picking addresses the agent, not the slot: clicking a transformed player has to
            // select the player, the same as clicking their base skin would.
            const uint32_t pickId = 0x80000000u | (uint32_t)SlotOwnerAgentId(slotKey);

            std::vector<PerObjectCB> agentCBs = templateCBs;
            auto meshIds = map_renderer->AddProp(propMeshes, agentCBs, pickId, tmpl.pixelShaderType);

            if (tmpl.texturesOk) {
                for (size_t l = 0; l < meshIds.size() && l < perMeshTexIds.size(); l++) {
                    map_renderer->GetMeshManager()->SetTexturesForMesh(
                        meshIds[l], map_renderer->GetTextureManager()->GetTextures(perMeshTexIds[l]), 3);
                }
            }

            for (int mid : meshIds)
                map_renderer->GetMeshManager()->SetMeshShouldRender(mid, false);

            m_agentMeshIds[slotKey] = meshIds;

            if (tmpl.hasAnimation && tmpl.clip && device) {
                AgentAnimState animState;
                auto hierarchyMode = tmpl.clip->hierarchyMode;
                size_t boneCount = tmpl.clip->boneTracks.size();

                for (size_t si = 0; si < tmpl.originalMeshes.size(); si++) {
                    const auto& mesh = tmpl.originalMeshes[si];
                    const auto& boneData = (si < tmpl.submeshBoneData.size())
                        ? tmpl.submeshBoneData[si]
                        : AnimationPanelState::SubmeshBoneData();
                    const auto& vbg = (si < tmpl.perVertexBoneGroups.size())
                        ? tmpl.perVertexBoneGroups[si]
                        : std::vector<uint32_t>();

                    auto skinnedVerts = AnimationPanelState::CreateSkinnedVertices(
                        mesh, boneData, vbg, boneCount, hierarchyMode, si);

                    auto animMesh = std::make_shared<AnimatedMeshInstance>(
                        device, skinnedVerts, mesh.indices, static_cast<int>(si));
                    animState.animMeshes.push_back(std::move(animMesh));
                }

                animState.perMeshCBs = templateCBs;
                animState.perMeshTextureIds = perMeshTexIds;
                animState.pixelShaderType = tmpl.pixelShaderType;

                animState.controller = std::make_unique<GW::Animation::AnimationController>();
                animState.controller->Initialize(tmpl.clip);
                animState.controller->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
                animState.controller->SetLooping(true);
                if (!tmpl.clip->animationSegments.empty())
                    animState.controller->SetSegment(0);
                animState.controller->Play();

                animState.hasSkinning = !animState.animMeshes.empty();

                // A composed character has already TAKEN OVER this slot's skinned state: its own
                // submeshes, its own constant buffers, and a controller switched to the skinning
                // path its geometry was built for. This assignment replaces the whole struct, so
                // running it over a composed character would put the stand-in's meshes back and
                // silently drop that switch - the character would either revert or animate through
                // the wrong path. The hand-over waits for m_agentModelsLoaded, so on the first load
                // this cannot fire; it is a reload (the agent-models toggle) that reaches here with
                // characters already live, and the hand-over does not run a second time to repair
                // them. This is the invariant the character pass needs and nothing else enforced.
                if (HasPlayerVisual(slotKey))
                    continue;

                m_agentAnimStates[slotKey] = std::move(animState);
            }
        }

        tmpl.parsedTextures_.clear();
        tmpl.parsedTextures_.shrink_to_fit();

        if (processed >= kAgentModelBatchSize) {
            auto elapsed = std::chrono::duration<float, std::milli>(LoadClock::now() - frameStart).count();
            if (elapsed > kLoadFrameBudgetMs)
                break;
        }
    }

    if (m_agentModelCreateIndex >= static_cast<int>(m_agentModelCreateOrder.size())) {
        m_loadTiming.agentGPUSec += std::chrono::duration<double>(LoadClock::now() - m_phaseStartTime).count();
        OutputDebugStringA(std::format("[ReplayLoad] AgentGPU: {:.3f}s\n", m_loadTiming.agentGPUSec).c_str());

        m_agentModelsLoaded = true;
        m_agentModelsLoading = false;
    }
}


// Keep synchronous LoadAgentModels as fallback (calls async + blocks)
void ReplayWindow::LoadAgentModels()
{
    if (m_agentModelsLoaded) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;
    if (!m_modelChangesBuilt) return;   // same gate as the async path; see LoadAgentModelsAsync
    if (!m_datManager || !m_hashIndex) return;

    auto* map_renderer = m_mapRenderer.get();
    if (!map_renderer) return;

    LoadAgentModelsAsync();

    // Block until IO is done, then create all resources at once
    if (m_agentModelLoadThread.joinable())
        m_agentModelLoadThread.join();

    while (!m_agentModelsLoaded)
        StepCreateAgentModelResources();
}


// ---------------------------------------------------------------------------
// World-space top of the model an agent is wearing at `time`, for the overlays that anchor
// above an agent's head. Returns groundY unchanged when there is no model to measure.
//
// Which model that is has to be resolved per call: an avatar form is a different height from
// the base skin, so a profession icon or a damage number placed from the base model would sit
// inside a transformed player's head.
// ---------------------------------------------------------------------------

float ReplayWindow::AgentModelTopY(int agentId, const AgentReplayData& ard,
                                   float groundY, float time) const
{
    if (!m_useAgentModels || !m_agentModelsLoaded) return groundY;

    uint32_t avatar = ard.modelIdAtTime(time);
    if (LookupAvatarFileHash(avatar) == 0) avatar = 0;

    auto hashIt = m_agentFileHashCache.find(avatar ? AvatarSlotKey(agentId, avatar) : agentId);
    if (hashIt == m_agentFileHashCache.end()) return groundY;
    auto tmplIt = m_agentModelTemplates.find(hashIt->second);
    if (tmplIt == m_agentModelTemplates.end()) return groundY;

    const AgentModelInfo info = avatar
        ? LookupAvatarModelInfo(avatar)
        : LookupAgentModelInfo(ard.type, ard.modelId, ard.primaryProf, ard.isFemale);

    // A recorded player is as tall as the character they played, so anything anchored above their
    // head has to measure THAT and not the stand-in it replaced.
    if (!avatar) {
        float pvTop = 0.f;
        if (PlayerVisualsTopY(agentId, m_agentModelScale, pvTop))
            return groundY + pvTop;
    }

    return groundY + tmplIt->second.nativeHeight * info.npcAdjustment * m_agentModelScale;
}

// ---------------------------------------------------------------------------
// Per-frame: update agent model world transforms and visibility
// ---------------------------------------------------------------------------

void ReplayWindow::DrawAgentModels()
{
    m_shadowCasters.clear();
    if (!m_useAgentModels) return;
    if (!m_showAgentOverlay) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    auto* meshMgr = m_mapRenderer->GetMeshManager();
    if (!meshMgr) return;

    auto* context = m_deviceResources->GetD3DDeviceContext();
    float frameDt = static_cast<float>(m_timer.GetElapsedSeconds());

    // A headpiece hangs where this pass puts it, and nowhere if this pass does not reach its
    // character: last frame's attachments go first.
    ClearHeadpieceAttachments();

    const MapTransform& mt = m_replayCtx.mapTransform;
    const InterpolationSettings& is = m_replayCtx.interpSettings;
    Terrain* terrain = m_mapRenderer->GetTerrain();

    Camera* cam = m_mapRenderer->GetCamera();
    XMFLOAT3 camP = cam->GetPosition3f();
    const bool lodOn = m_uiLayout.lodEnabled;
    const float lodDot = m_uiLayout.lodDotDist;

    bool hasBounds = (terrain != nullptr);
    float bMinX = 0, bMaxX = 0, bMinZ = 0, bMaxZ = 0;
    if (hasBounds) {
        bMinX = terrain->m_bounds.map_min_x;
        bMaxX = terrain->m_bounds.map_max_x;
        bMinZ = terrain->m_bounds.map_min_z;
        bMaxZ = terrain->m_bounds.map_max_z;
    }

    for (auto& [slotKey, meshIds] : m_agentMeshIds)
    {
        const int      agentId    = SlotOwnerAgentId(slotKey);
        const uint32_t slotAvatar = SlotAvatarModelId(slotKey);

        auto agentIt = m_replayCtx.agents.find(agentId);
        if (agentIt == m_replayCtx.agents.end()) {
            for (int mid : meshIds) meshMgr->SetMeshShouldRender(mid, false);
            m_agentModelRenderStatus[slotKey] = std::format("hidden: not in agents (meshes={})", meshIds.size());
            continue;
        }

        auto& ard = agentIt->second;
        if (ard.snapshots.empty()) {
            for (int mid : meshIds) meshMgr->SetMeshShouldRender(mid, false);
            m_agentModelRenderStatus[slotKey] = "hidden: no snapshots";
            continue;
        }

        // An agent owns one slot per model it wears during the match, and exactly one of them
        // draws at any time. A model we have no file for - a costume, a tonic - resolves to no
        // slot, which leaves the base skin on screen rather than making the player vanish.
        uint32_t wornAvatar = ard.modelIdAtTime(m_debugTimeline);
        if (LookupAvatarFileHash(wornAvatar) == 0) wornAvatar = 0;
        if (slotAvatar != wornAvatar) {
            for (int mid : meshIds) meshMgr->SetMeshShouldRender(mid, false);
            // Unconditional, like the fog and spirit branches below: the skinned and weapon
            // passes draw from this status alone, so the slot that is not worn has to say so
            // every frame or it keeps drawing on top of the one that is.
            m_agentModelRenderStatus[slotKey] = "hidden: model slot not worn";
            continue;
        }

        // Cache snapshot index once per agent per frame - reused below for rotation lookup
        const int snapIdx = FindSnapshotIndex(ard.snapshots, m_debugTimeline);

        // Spirits: only visible within their snapshot time range
        if (ard.type == AgentType::Spirit) {
            if (m_debugTimeline < ard.snapshots.front().time ||
                m_debugTimeline > ard.snapshots.back().time ||
                ard.overlapHidden ||
                ard.isDeadAtTime(m_debugTimeline) ||
                !ard.isAliveAtTime(m_debugTimeline)) {
                for (int mid : meshIds) meshMgr->SetMeshShouldRender(mid, false);
                // Must clear the render status unconditionally (not only when the
                // debug window is open): spirit models are skinned, so hiding the
                // rigid meshes above does nothing for them, and the skinned render
                // pass draws any agent whose status still starts with "skinned".
                // Leaving a stale "skinned" status here keeps the spirit's 3D model
                // on screen after it dies, despawns, or is overwritten by a newer
                // spirit of the same type - frozen at its last pose and position.
                m_agentModelRenderStatus[slotKey] = "hidden: spirit not active";
                continue;
            }
        }

        // Summoned minions (e.g. Bone Horror): only render while the agent
        // exists and is alive. Hide when it has 0 HP, is flagged dead, or its
        // ID no longer exists (outside its snapshot/lifecycle window) — instead
        // of leaving a stale body at its last position.
        if (ard.type == AgentType::NPC && IsNpcHiddenWhenDead(ard.modelId)
            && !ard.isMinionVisibleAtTime(m_debugTimeline)) {
            for (int mid : meshIds) meshMgr->SetMeshShouldRender(mid, false);
            // Must clear the render status unconditionally (not only when the
            // debug window is open): the skinned render pass draws any agent
            // whose status still starts with "skinned". Leaving a stale
            // "skinned" status here keeps the minion's 3D model visible after
            // it dies/despawns.
            m_agentModelRenderStatus[slotKey] = "hidden: minion dead/removed";
            continue;
        }

        // Dead state: keep rendering with fade (don't hide)
        bool dead = (ard.type == AgentType::NPC || ard.type == AgentType::Player)
                     && ard.isDeadAtTime(m_debugTimeline);

        float deadAlpha = 1.0f;
        if (dead) {
            float deathTime = ard.deathTransitionTime(m_debugTimeline);
            float fadeIn = std::clamp((m_debugTimeline - deathTime) / 0.5f, 0.f, 1.f);
            fadeIn = fadeIn * fadeIn * (3.f - 2.f * fadeIn);
            deadAlpha = 1.0f - 0.3f * fadeIn;
        }

        // The PvP team glow, a coloured rim on the sides of a team's NPCs (see the model pixel
        // shaders). The client's gate is "an NPC with a non-zero team colour id" - players never
        // get it, their team colour goes on the cape - and the id is the team id recorded here.
        // It rides in bits 8-11 of highlight_state; the client clamps ids above 7 to 7.
        const uint32_t teamGlowBits =
            ((ard.type == AgentType::NPC || ard.type == AgentType::Spirit) && ard.teamId != Team::None)
                ? static_cast<uint32_t>(std::min<int>(ard.teamId, 7)) << 8
                : 0u;

        // Fog check
        bool inFog = (m_fogPerspective > 0 && ard.teamId != m_fogPerspective && IsAgentInFog(agentId));
        if (inFog && !m_fogGhostMode) {
            for (int mid : meshIds) meshMgr->SetMeshShouldRender(mid, false);
            // Unconditional, like the spirit/minion branches above: skinned agents
            // are drawn from this status alone, so a conditional write would leave
            // players, NPCs and spirits rendering after they walk into the fog.
            // The status is rewritten to "skinned" as soon as they leave it again.
            m_agentModelRenderStatus[slotKey] = "hidden: fog";
            continue;
        }

        // Get interpolated position — use agent's own height (includes plane elevation)
        float sx, sy, sz;
        InterpolateAgentPosition(ard, m_debugTimeline, is, sx, sy, sz);
        XMFLOAT3 pos = ApplyMapTransformToPos(sx, sy, sz, mt);

        if (hasBounds) {
            pos.x = std::clamp(pos.x, bMinX, bMaxX);
            pos.z = std::clamp(pos.z, bMinZ, bMaxZ);
        }

        // LOD: beyond lodDotDist → hide 3D model, show stylized icon instead
        if (lodOn) {
            float dx = pos.x - camP.x, dy = pos.y - camP.y, dz = pos.z - camP.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist > lodDot) {
                for (int mid : meshIds) meshMgr->SetMeshShouldRender(mid, false);
                ard.currentLOD = 0;
                m_agentModelRenderStatus[slotKey] = std::format("hidden: LOD icon (dist={:.0f})", dist);
                continue;
            }
        }
        ard.currentLOD = 2;

        // Get rotation from nearest snapshot
        float rotRad = ard.snapshots[snapIdx].rotation;

        // Scale: FFNA models are already in game units; only apply the GW NPC adjustment
        auto hashIt = m_agentFileHashCache.find(slotKey);
        uint32_t cachedHash = (hashIt != m_agentFileHashCache.end()) ? hashIt->second : 0;
        auto tmplIt = m_agentModelTemplates.find(cachedHash);

        AgentModelInfo info = slotAvatar
            ? LookupAvatarModelInfo(slotAvatar)
            : LookupAgentModelInfo(ard.type, ard.modelId, ard.primaryProf, ard.isFemale);
        float nativeH = (tmplIt != m_agentModelTemplates.end())
                            ? tmplIt->second.nativeHeight : 0.f;
        float scale;
        if (info.fitHeight > 0.f && nativeH > 1.f)
            scale = (info.fitHeight / nativeH) * m_agentModelScale;
        else
            scale = info.npcAdjustment * m_agentModelScale;
        XMMATRIX centering = XMMatrixIdentity();

        if (tmplIt != m_agentModelTemplates.end()) {
            const auto& tmpl = tmplIt->second;
            centering = XMMatrixTranslation(
                -tmpl.nativeCenter.x, -tmpl.nativeMinY, -tmpl.nativeCenter.z);
        }

        // A recorded player is drawn at the size they actually were, which is a property of the
        // character and not of the stand-in model: it replaces the model adjustment entirely, and
        // its own geometry supplies the centring, so the scale is about the character's feet.
        // Everything downstream - the shadow radius below, the weapon that rides this transform,
        // the overlays that anchor on AgentModelTopY - follows from these two.
        float pvScale = 1.f;
        XMFLOAT3 pvCentre{ 0.f, 0.f, 0.f };
        const bool pvPlaced = !slotAvatar && PlayerVisualsPlacement(agentId, pvScale, pvCentre);
        if (pvPlaced) {
            scale = pvScale * m_agentModelScale;
            centering = XMMatrixTranslation(-pvCentre.x, -pvCentre.y, -pvCentre.z);
        }

        XMMATRIX worldMat = centering
            * XMMatrixRotationY(XM_PIDIV2)
            * XMMatrixScaling(scale, scale, scale)
            * XMMatrixRotationY(-rotRad)
            * XMMatrixTranslation(pos.x, pos.y, pos.z);

        // Reuse the same visibility gate, position and scale as the displayed model.
        // This is a bounds-derived fallback radius, pending native model-bound parity.
        if (tmplIt != m_agentModelTemplates.end() && !inFog) {
            const float distance = std::sqrt((pos.x-camP.x)*(pos.x-camP.x) +
                (pos.y-camP.y)*(pos.y-camP.y) + (pos.z-camP.z)*(pos.z-camP.z));
            const float fade = ProjectedAgentShadows::DistanceOpacity(distance, m_originalShadowDistanceLimit);
            float radius = std::min(0.75f * tmplIt->second.nativeShadowRadius * scale, 150.f);
            float casterHeight = tmplIt->second.nativeHeight * scale;
            // A recorded player's shadow and height come from the character's own bounds, not
            // from the stand-in's.
            if (pvPlaced) {
                float pvRadius = 0.f, pvTop = 0.f;
                if (PlayerVisualsRadius(agentId, m_agentModelScale, pvRadius) && pvRadius > 0.f)
                    radius = std::min(0.75f * pvRadius, 150.f);
                if (PlayerVisualsTopY(agentId, m_agentModelScale, pvTop) && pvTop > 0.f)
                    casterHeight = pvTop;
            }
            if (radius > 0 && fade > 0)
                m_shadowCasters.push_back({pos, radius, fade * (dead ? deadAlpha : 1.f), slotKey,
                    casterHeight});
        }

        // Decide skinned vs. rigid rendering
        auto animIt = m_agentAnimStates.find(slotKey);
        bool useSkinned = (animIt != m_agentAnimStates.end() && animIt->second.hasSkinning);

        if (useSkinned) {
            // Hide the rigid props — skinned draw will handle this agent
            for (int mid : meshIds)
                meshMgr->SetMeshShouldRender(mid, false);

            auto& animState = animIt->second;

            // Compute velocity from interpolated position deltas (game coordinates)
            float moveDx = 0.f, moveDy = 0.f;
            float frameVelocity = 0.f;
            {
                float dt = m_debugTimeline - animState.prevTime;
                if (animState.prevTime >= 0.f && dt > 0.001f && dt < 1.0f) {
                    moveDx = sx - animState.prevPosX;
                    moveDy = sy - animState.prevPosY;
                    frameVelocity = std::sqrtf(moveDx * moveDx + moveDy * moveDy) / dt;
                }
                constexpr float kSmoothAlpha = 0.3f;
                animState.smoothVelocity = kSmoothAlpha * frameVelocity
                                         + (1.f - kSmoothAlpha) * animState.smoothVelocity;
                animState.prevPosX = sx;
                animState.prevPosY = sy;
                animState.prevTime = m_debugTimeline;
            }

            // Drive animation from gameplay state
            auto& ctrl = animState.controller;
            const auto& snap = ard.snapshots[snapIdx];

            if (tmplIt != m_agentModelTemplates.end() && ctrl) {
                uint32_t animCode = snap.animation_code;
                if (animCode != animState.lastAnimCode && animCode != 0) {
                    auto& codeMap = tmplIt->second.animCodeToSegment;
                    bool resolved = false;
                    SegmentRef resolvedRef{};

                    // Strategy 1: direct lookup in pre-built map
                    auto segIt = codeMap.find(animCode);
                    if (segIt != codeMap.end()) {
                        resolvedRef = segIt->second;
                        resolved = true;
                    }

                    // Strategy 2: treat animCode as primaryHash, compute segment hash per bone slot
                    if (!resolved) {
                        for (size_t bs = 0; bs < GW::Animation::g_boneSlotCount; bs++) {
                            uint32_t candidate = GW::Animation::ComputeSegmentHash(
                                animCode, GW::Animation::g_boneSlotChars[bs]);
                            auto segIt2 = codeMap.find(candidate);
                            if (segIt2 != codeMap.end()) {
                                resolvedRef = segIt2->second;
                                codeMap[animCode] = resolvedRef;
                                resolved = true;
                                break;
                            }
                        }
                    }

                    // Strategy 3: reverse animCode, compute hash, match against all clip segments
                    if (!resolved) {
                        for (int ci : tmplIt->second.clipOrder) {
                            if (resolved) break;
                            const auto& segments = tmplIt->second.allClips[ci].clip->animationSegments;
                            for (size_t bsA = 0; bsA < GW::Animation::g_boneSlotCount && !resolved; bsA++) {
                                uint32_t animPrimary = GW::Animation::ReverseSegmentHash(
                                    animCode, GW::Animation::g_boneSlotChars[bsA]);
                                if (animPrimary == 0) continue;
                                for (size_t bsB = 0; bsB < GW::Animation::g_boneSlotCount && !resolved; bsB++) {
                                    uint32_t candidate = GW::Animation::ComputeSegmentHash(
                                        animPrimary, GW::Animation::g_boneSlotChars[bsB]);
                                    for (size_t si = 0; si < segments.size(); si++) {
                                        if (tmplIt->second.localSegmentsOnly &&
                                            tmplIt->second.allClips[ci].clip->GetSegmentSourceType(si) != 0)
                                            continue;
                                        if (segments[si].hash == candidate) {
                                            resolvedRef = { ci, static_cast<int>(si) };
                                            codeMap[animCode] = resolvedRef;
                                            resolved = true;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Strategy 4: reverse both animCode and segment hash, compare primaries
                    if (!resolved) {
                        for (int ci : tmplIt->second.clipOrder) {
                            if (resolved) break;
                            const auto& segments = tmplIt->second.allClips[ci].clip->animationSegments;
                            for (size_t si = 0; si < segments.size() && !resolved; si++) {
                                if (tmplIt->second.localSegmentsOnly &&
                                    tmplIt->second.allClips[ci].clip->GetSegmentSourceType(si) != 0)
                                    continue;
                                uint32_t segHash = segments[si].hash;
                                for (size_t bs = 0; bs < GW::Animation::g_boneSlotCount && !resolved; bs++) {
                                    uint32_t animPrimary = GW::Animation::ReverseSegmentHash(
                                        animCode, GW::Animation::g_boneSlotChars[bs]);
                                    uint32_t segPrimary = GW::Animation::ReverseSegmentHash(
                                        segHash, GW::Animation::g_boneSlotChars[bs]);
                                    if (animPrimary != 0 && animPrimary == segPrimary) {
                                        resolvedRef = { ci, static_cast<int>(si) };
                                        codeMap[animCode] = resolvedRef;
                                        resolved = true;
                                    }
                                }
                            }
                        }
                    }

                    if (resolved) {
                        if (resolvedRef.clipIndex != animState.currentClipIndex &&
                            resolvedRef.clipIndex >= 0 &&
                            resolvedRef.clipIndex < static_cast<int>(tmplIt->second.allClips.size())) {
                            const auto& newClip = tmplIt->second.allClips[resolvedRef.clipIndex].clip;
                            ctrl->Initialize(newClip);
                            ctrl->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
                            animState.currentClipIndex = resolvedRef.clipIndex;
                        }
                        animState.wasCasting = false;
                        bool stillKD = snap.is_knocked;
                        animState.postKdGetUp = animState.wasKnockedDown && !stillKD;
                        animState.wasKnockedDown = false;
                        ctrl->SetSegment(resolvedRef.segmentIndex);
                        bool playOnce = dead || (snap.is_casting && !stillKD) || animState.postKdGetUp;
                        ctrl->SetLooping(!playOnce);
                        ctrl->Play();
                    }

                    animState.lastLookupFailed = !resolved;
                    animState.lastAnimCode = animCode;
                    animState.isPlayingMovementAnim = false;
                    animState.isPlayingIdleAnim = false;
                }

                // Movement / idle animation when server sends no animation code
                constexpr float kMovingThreshold = 15.f;
                if (animCode == 0 && !dead && !snap.is_casting && !animState.wasCasting
                    && !animState.postKdGetUp && !ard.isKnockedDownAtTime(m_debugTimeline))
                {
                    bool isMoving = animState.smoothVelocity > kMovingThreshold;
                    auto& codeMap = tmplIt->second.animCodeToSegment;

                    if (isMoving) {
                        // Compute movement heading from position delta
                        float moveHeading = std::atan2(moveDx, moveDy);
                        float relAngle = moveHeading - rotRad;
                        // Normalize to [-PI, PI]
                        while (relAngle > XM_PI)  relAngle -= XM_2PI;
                        while (relAngle < -XM_PI) relAngle += XM_2PI;

                        // Quantize to 8 compass sectors (each 45 deg)
                        int dirIndex = (static_cast<int>(std::round(relAngle / (XM_PI / 4.f))) + 8) % 8;

                        if (dirIndex != animState.currentMovementDirIndex || !animState.isPlayingMovementAnim) {
                            // Use "Running" movement table (index 2)
                            constexpr int kRunningTableIdx = 2;
                            uint8_t stateIdx = GW::Animation::g_movementTables[kRunningTableIdx].indices[dirIndex];
                            if (stateIdx < GW::Animation::g_animationStateCount) {
                                uint32_t primaryHash = GW::Animation::g_animationStateTable[stateIdx].primaryHash;
                                auto segIt = codeMap.find(primaryHash);
                                if (segIt != codeMap.end()) {
                                    SegmentRef ref = segIt->second;
                                    if (ref.clipIndex != animState.currentClipIndex &&
                                        ref.clipIndex >= 0 &&
                                        ref.clipIndex < static_cast<int>(tmplIt->second.allClips.size())) {
                                        ctrl->Initialize(tmplIt->second.allClips[ref.clipIndex].clip);
                                        ctrl->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
                                        animState.currentClipIndex = ref.clipIndex;
                                    }
                                    ctrl->SetSegment(ref.segmentIndex);
                                    ctrl->SetLooping(true);
                                    ctrl->Play();
                                }
                            }
                            animState.currentMovementDirIndex = dirIndex;
                            animState.isPlayingMovementAnim = true;
                            animState.isPlayingIdleAnim = false;
                        }
                    } else if (!animState.isPlayingIdleAnim) {
                        // Switch to idle when stopped
                        constexpr uint32_t kIdlePrimaryHash = 0x365C0E24u;
                        auto segIt = codeMap.find(kIdlePrimaryHash);
                        if (segIt != codeMap.end()) {
                            SegmentRef ref = segIt->second;
                            if (ref.clipIndex != animState.currentClipIndex &&
                                ref.clipIndex >= 0 &&
                                ref.clipIndex < static_cast<int>(tmplIt->second.allClips.size())) {
                                ctrl->Initialize(tmplIt->second.allClips[ref.clipIndex].clip);
                                ctrl->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
                                animState.currentClipIndex = ref.clipIndex;
                            }
                            ctrl->SetSegment(ref.segmentIndex);
                            ctrl->SetLooping(true);
                            ctrl->Play();
                        }
                        animState.isPlayingIdleAnim = true;
                        animState.isPlayingMovementAnim = false;
                        animState.currentMovementDirIndex = -1;
                    }
                }

                // Detect resurrection
                if (!dead && animState.wasDead)
                    ctrl->Play();
                animState.wasDead = dead;

                // Track casting/KD state
                bool isKD = ard.isKnockedDownAtTime(m_debugTimeline);
                if (snap.is_casting)
                    animState.wasCasting = true;
                if (snap.is_knocked) {
                    animState.wasKnockedDown = true;
                    animState.wasCasting = false;
                }

                // Update looping dynamically
                bool playOnce = dead || isKD || (snap.is_casting && !isKD) || (animState.wasCasting && !isKD) || animState.postKdGetUp;
                ctrl->SetLooping(!playOnce);

                // Playback speed — use the game's animation_speed when available.
                // NPC/hero agents always have animation_speed > 0 in snapshot data.
                // Player agents have animation_speed = 0 (sparse observer data),
                // so we fall back to duration-based or velocity-based formulas.
                float speedMult = 1.0f;
                if (dead) {
                    speedMult = 0.8f;
                } else if (snap.is_casting && ctrl) {
                    const CastInterval* ci = ard.castIntervalAtTime(m_debugTimeline);
                    if (ci) {
                        float castDur = ci->end - ci->start;
                        float segStart = ctrl->GetSequenceStartTime();
                        float segEnd   = ctrl->GetSequenceEndTime();
                        float segDurSec = (segEnd - segStart) / 100000.f;
                        if (castDur > 0.001f && segDurSec > 0.001f)
                            speedMult = segDurSec / castDur;
                    }
                } else if (snap.animation_speed > 0.f) {
                    speedMult = snap.animation_speed;
                    // Use timing-based formula for attack animations when weapon data
                    // is available.  This fits the segment to the real attack window,
                    // just like the casting formula above.
                    bool attackTiming = false;
                    if (!animState.isPlayingMovementAnim && !animState.isPlayingIdleAnim
                        && snap.weapon_attack_speed > 0.f && snap.attack_speed_modifier > 0.f && ctrl) {
                        float effectiveAttackTime = snap.weapon_attack_speed * snap.attack_speed_modifier;
                        float segStart = ctrl->GetSequenceStartTime();
                        float segEnd   = ctrl->GetSequenceEndTime();
                        float segDurSec = (segEnd - segStart) / 100000.f;
                        if (effectiveAttackTime > 0.001f && segDurSec > 0.001f) {
                            speedMult = segDurSec / effectiveAttackTime;
                            attackTiming = true;
                        }
                    }
                    // The server sends a fixed animation_speed for running (typically
                    // 0.667) regardless of actual velocity.  When the character has a
                    // speed buff the legs need to move faster to match, just like the
                    // real game client does locally.  Use the higher of the two speeds
                    // so attack/cast anims (which have speed > 1) are never reduced.
                    if (animState.smoothVelocity > 15.f) {
                        constexpr float kBaseRunAnimSpeedNPC = 432.f;
                        float velSpeed = animState.smoothVelocity / kBaseRunAnimSpeedNPC;
                        if (velSpeed > speedMult)
                            speedMult = velSpeed;
                    }
                    else if (!attackTiming) {
                        // Idle / stationary stance (breathing, spirit channel,
                        // guild-lord idle, sentinel stance, ...): these ambient
                        // animations play at their natural authored rate in the game
                        // client and are NOT scaled by the gameplay animation_speed
                        // (which is tuned for locomotion). Scaling them made idle NPCs
                        // animate too fast, so play them at 1.0x like player idles.
                        speedMult = 1.0f;
                    }
                } else if (isKD && ctrl) {
                    const auto* kd = ard.knockdownIntervalAtTime(m_debugTimeline);
                    if (kd) {
                        float kdDur = kd->end - kd->start;
                        float segStart = ctrl->GetSequenceStartTime();
                        float segEnd   = ctrl->GetSequenceEndTime();
                        float segDurSec = (segEnd - segStart) / 100000.f;
                        if (kdDur > 0.001f && segDurSec > 0.001f)
                            speedMult = segDurSec / kdDur;
                    }
                } else if (animState.postKdGetUp && ctrl) {
                    float segStart = ctrl->GetSequenceStartTime();
                    float segEnd   = ctrl->GetSequenceEndTime();
                    float segDurSec = (segEnd - segStart) / 100000.f;
                    float codeStart = snap.time;
                    for (int si2 = snapIdx - 1; si2 >= 0; si2--) {
                        if (ard.snapshots[si2].animation_code != snap.animation_code) break;
                        codeStart = ard.snapshots[si2].time;
                    }
                    float codeEnd = snap.time;
                    for (int si2 = snapIdx + 1; si2 < static_cast<int>(ard.snapshots.size()); si2++) {
                        if (ard.snapshots[si2].animation_code != snap.animation_code) {
                            codeEnd = ard.snapshots[si2].time;
                            break;
                        }
                        codeEnd = ard.snapshots[si2].time;
                    }
                    float getUpDur = codeEnd - codeStart;
                    if (getUpDur > 0.001f && segDurSec > 0.001f)
                        speedMult = segDurSec / getUpDur;
                } else {
                    if (animState.isPlayingMovementAnim) {
                        constexpr float kBaseRunAnimSpeed = 432.f;
                        speedMult = animState.smoothVelocity / kBaseRunAnimSpeed;
                        speedMult = std::clamp(speedMult, 0.1f, 4.0f);
                    } else if (!animState.isPlayingIdleAnim
                               && snap.weapon_attack_speed > 0.f && snap.attack_speed_modifier > 0.f && ctrl) {
                        float effectiveAttackTime = snap.weapon_attack_speed * snap.attack_speed_modifier;
                        float segStart = ctrl->GetSequenceStartTime();
                        float segEnd   = ctrl->GetSequenceEndTime();
                        float segDurSec = (segEnd - segStart) / 100000.f;
                        if (effectiveAttackTime > 0.001f && segDurSec > 0.001f)
                            speedMult = segDurSec / effectiveAttackTime;
                    }
                }

                animState.effectiveSpeedMult = speedMult;
                ctrl->SetPlaybackSpeed(speedMult * 100000.f);

                // Advance animation (only once per frame; PiP pass reuses current pose)
                bool deathFrozen = dead && ctrl->GetState() == GW::Animation::AnimationController::PlaybackState::Stopped;
                if (m_replayCtx.isPlaying && !deathFrozen && m_lastAnimUpdateFrame != m_frameCount)
                    ctrl->Update(frameDt * m_replayCtx.playbackSpeed);

                if (context) {
                    // A BREADCRUMB, NOT A LOG LINE. This step uploads one constant buffer per
                    // submesh per animated agent per frame, and it is the busiest call this window
                    // makes into the graphics driver - so it is also where a driver-side fault
                    // lands, and a fault here unwinds nothing and writes nothing. Set() is four
                    // stores and no I/O; the crash handler is what turns it into a line, naming the
                    // agent and the submesh that were in flight. See RunLog.h.
                    for (size_t bi = 0; bi < animState.animMeshes.size(); bi++) {
                        auto& am = animState.animMeshes[bi];
                        if (!am) continue;
                        RunLog::Set("uploading an agent's bone palette", agentId,
                                    static_cast<int>(bi),
                                    static_cast<int>(animState.animMeshes.size()));
                        am->UpdateBoneMatrices(context, *ctrl);
                    }
                }
            }

            // Update world transforms and alpha on per-mesh CBs
            float baseAlpha = inFog ? 0.3f : 1.0f;
            if (dead) baseAlpha = deadAlpha;
            bool hovered = (agentId == m_hoveredAgentId);
            for (size_t si2 = 0; si2 < animState.perMeshCBs.size(); si2++) {
                XMStoreFloat4x4(&animState.perMeshCBs[si2].world, worldMat);
                animState.perMeshCBs[si2].mesh_alpha = baseAlpha;
                animState.perMeshCBs[si2].highlight_state = (hovered ? 5u : 0u) | teamGlowBits;
            }

            // A LINKED HEADPIECE rides the head, not the body: its submeshes have just been given
            // the character's own matrix and the piece now takes them back, each on the bone of
            // the piece's own skeleton that carries it. Its particles ride the same attachment,
            // which this keeps for them.
            if (HasPlayerVisual(slotKey) && PlayerVisualsNeedsAttach(agentId) &&
                animState.controller && tmplIt != m_agentModelTemplates.end()) {
                EnsureHeadLink(tmplIt->second);
                XMFLOAT4X4 worldF{};
                XMStoreFloat4x4(&worldF, worldMat);
                PlaceLinkedHeadpiece(
                    agentId, tmplIt->second.headLinkState == 1 ? tmplIt->second.headLinkBone : -1,
                    tmplIt->second.headLinkOffsetGw, tmplIt->second.headLinkBindDx,
                    animState.controller->GetBoneMatrices(), worldF, animState.perMeshCBs);
            }

            // Consumed by DrawWeaponModels, which runs after this pass.
            animState.lastSnapIdx = snapIdx;

            m_agentModelRenderStatus[slotKey] = m_showAgentModelWindow
                ? std::format("skinned{}: submeshes={} pos=({:.0f},{:.0f},{:.0f}) anim=0x{:X}",
                    dead ? " (dead)" : "", animState.animMeshes.size(),
                    pos.x, pos.y, pos.z, snap.animation_code)
                : "skinned";
        } else {
            // Rigid prop rendering
            float rigidAlpha = inFog ? 0.3f : 1.0f;
            if (dead) rigidAlpha = deadAlpha;
            bool rigidHovered = (agentId == m_hoveredAgentId);
            int renderedCount = 0;
            for (int mid : meshIds) {
                meshMgr->SetMeshShouldRender(mid, true);
                auto cbOpt = meshMgr->GetMeshPerObjectData(mid);
                if (cbOpt.has_value()) {
                    auto cb = cbOpt.value();
                    XMStoreFloat4x4(&cb.world, worldMat);
                    cb.mesh_alpha = rigidAlpha;
                    cb.highlight_state = (rigidHovered ? 5u : 0u) | teamGlowBits;
                    meshMgr->UpdateMeshPerObjectData(mid, cb);
                    renderedCount++;
                }
            }
            m_agentModelRenderStatus[slotKey] = m_showAgentModelWindow
                ? std::format("shown: meshes={}/{} pos=({:.0f},{:.0f},{:.0f}) scale={:.3f}",
                    renderedCount, meshIds.size(), pos.x, pos.y, pos.z, scale)
                : "shown";
        }
    }

    // The headpiece particles and the soft bodies, once per frame and at the replay's own pace:
    // a paused timeline holds the flames where they are, exactly as it holds the character.
    if (m_lastAnimUpdateFrame != m_frameCount)
    {
        const float pace = m_replayCtx.isPlaying ? frameDt * m_replayCtx.playbackSpeed : 0.f;
        StepHeadpieceParticles(pace);

        // A BODY MUST NOT BE INTEGRATED ACROSS A JUMP. The timeline is dragged, stepped and
        // restarted, and a band asked to swing from one end of a match to the other in one frame
        // leaves the character entirely. Anything bigger than a few frames of ordinary play is
        // taken as a jump, and so is a paused timeline, where the band simply follows the pose.
        const float moved = std::abs(m_debugTimeline - m_lastSoftBodyTimeline);
        const float ordinary = std::max(0.35f, pace * 4.f);
        StepPlayerVisualSoftBodies(pace, !m_replayCtx.isPlaying || moved > ordinary);
        m_lastSoftBodyTimeline = m_debugTimeline;
    }

    m_lastAnimUpdateFrame = m_frameCount;
}


// ---------------------------------------------------------------------------
// Skinned render pass for animated agent models.
// Called after DrawAgentModels() which updates bone matrices and world CBs.
// ---------------------------------------------------------------------------

// WHERE A HEADPIECE HANGS ON THIS RIG, resolved once and kept on the template.
//
// A crest or an Elementalist eye is a model of its own that the client links to the player's head
// action point; the rig's own animation file carries the record that says which bone that is. The
// chosen bank is asked first and the rig's other clips after it, and an answer is only taken from a
// file whose skeleton is this rig's and whose bone count is the pose's - the bone INDEX and the
// pose have to be in one order, or the piece would ride the wrong joint.
//
// Done here, lazily, rather than in the loader: it costs one cached file read per rig, and only for
// a rig whose player is actually wearing one of the twenty-five pieces.
void ReplayWindow::EnsureHeadLink(AgentModelInstance& tmpl)
{
    if (tmpl.headLinkState != 0)
        return;
    tmpl.headLinkState = 2;   // until some file answers

    if (!m_datManager || !m_hashIndex || !tmpl.clip)
        return;
    const size_t poseBones = tmpl.clip->boneTracks.size();

    std::vector<uint32_t> candidates;
    for (const auto& entry : tmpl.allClips)
        if (entry.clip == tmpl.clip && entry.sourceFileHash != 0)
            candidates.push_back(entry.sourceFileHash);
    for (const auto& entry : tmpl.allClips)
        if (entry.sourceFileHash != 0)
            candidates.push_back(entry.sourceFileHash);

    std::string reasons;
    for (const uint32_t fileId : candidates)
    {
        auto hashIt = m_hashIndex->find(static_cast<int>(fileId));
        if (hashIt == m_hashIndex->end() || hashIt->second.empty())
            continue;
        const auto bytes = m_datManager->read_file_cached(
            static_cast<uint32_t>(hashIt->second.at(0)));
        if (!bytes || bytes->empty())
            continue;

        int bone = -1;
        XMFLOAT3 offset{}, bind{};
        std::string why;
        if (ResolveHeadActionPoint(bytes->data(), bytes->size(), tmpl.modelHash0, tmpl.modelHash1,
                                   poseBones, bone, offset, bind, why))
        {
            tmpl.headLinkBone = bone;
            tmpl.headLinkOffsetGw = offset;
            tmpl.headLinkBindDx = bind;
            tmpl.headLinkState = 1;
            RunLog::Line("visuals: rig 0x%08X/0x%08X links a headpiece to bone %d of %zu,"
                         " from file %u",
                         tmpl.modelHash0, tmpl.modelHash1, bone, poseBones, fileId);
            return;
        }
        if (!why.empty())
            reasons += std::to_string(fileId) + ": " + why + "; ";
    }

    RunLog::Line("visuals: *** rig 0x%08X/0x%08X has no readable head link *** - a linked piece"
                 " keeps its bind position: %s",
                 tmpl.modelHash0, tmpl.modelHash1,
                 reasons.empty() ? "no candidate file carried a table" : reasons.c_str());
}


void ReplayWindow::DrawSkinnedAgentModels()
{
    if (!m_useAgentModels || !m_agentModelsLoaded) return;
    if (m_agentAnimStates.empty()) return;

    auto* context = m_deviceResources->GetD3DDeviceContext();
    auto* meshManager = m_mapRenderer->GetMeshManager();
    auto* textureManager = m_mapRenderer->GetTextureManager();
    if (!context || !meshManager || !textureManager) return;

    bool anyVisible = false;
    for (auto& [slotKey, animState] : m_agentAnimStates) {
        if (!animState.hasSkinning) continue;
        if (HasPlayerVisual(slotKey)) continue;   // drawn by its own pass, see DrawPlayerVisuals
        auto statusIt = m_agentModelRenderStatus.find(slotKey);
        if (statusIt == m_agentModelRenderStatus.end()) continue;
        if (statusIt->second.starts_with("skinned")) { anyVisible = true; break; }
    }
    if (!anyVisible) return;

    m_mapRenderer->BindSkinnedVertexShader();
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_mapRenderer->BindModelPixelShader(false);

    for (auto& [slotKey, animState] : m_agentAnimStates)
    {
        if (!animState.hasSkinning) continue;

        // A recorded player's own character needs three sub-passes and its own program, so it is
        // drawn by DrawPlayerVisuals immediately after this one. Everything else - NPCs, spirits,
        // minions, avatar forms and any player without a record - is drawn here exactly as before.
        if (HasPlayerVisual(slotKey)) continue;

        auto statusIt = m_agentModelRenderStatus.find(slotKey);
        if (statusIt == m_agentModelRenderStatus.end() || !statusIt->second.starts_with("skinned"))
            continue;

        for (size_t si = 0; si < animState.animMeshes.size(); si++)
        {
            auto& animMesh = animState.animMeshes[si];
            if (!animMesh) continue;

            if (si < animState.perMeshCBs.size()) {
                PerObjectCB transposedData = animState.perMeshCBs[si];
                XMMATRIX worldMatrix = XMLoadFloat4x4(&transposedData.world);
                worldMatrix = XMMatrixTranspose(worldMatrix);
                XMStoreFloat4x4(&transposedData.world, worldMatrix);
                meshManager->SetPerObjectCB(transposedData);
            }

            if (si < animState.perMeshTextureIds.size()) {
                const auto& texIds = animState.perMeshTextureIds[si];
                if (!texIds.empty()) {
                    auto textures = textureManager->GetTextures(texIds);
                    if (!textures.empty())
                        animMesh->SetTextures(textures, 3);
                }
            }

            RunLog::Set("drawing a skinned agent submesh", slotKey, static_cast<int>(si),
                        static_cast<int>(animState.animMeshes.size()));
            animMesh->Draw(context, m_mapRenderer->GetLODQuality());
        }
    }

    RunLog::Set("finished the skinned agent pass");
    m_mapRenderer->BindRegularVertexShader();
}


void ReplayWindow::DrawAgentCylinders()
{
    if (!m_showAgentOverlay) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    InitCylinderRenderer();
    if (!m_cylVS || !m_cylPS) return;

    ID3D11DeviceContext* ctx = m_deviceResources->GetD3DDeviceContext();

    // ---- Save ALL D3D11 state so we don't corrupt MapRenderer / ImGui ----
    Microsoft::WRL::ComPtr<ID3D11VertexShader>   oldVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>    oldPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>    oldIL;
    Microsoft::WRL::ComPtr<ID3D11Buffer>         oldVB;
    Microsoft::WRL::ComPtr<ID3D11Buffer>         oldIB;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> oldRS;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> oldDSS;
    Microsoft::WRL::ComPtr<ID3D11BlendState>     oldBS;
    UINT oldStencilRef = 0;
    float oldBlendFactor[4]; UINT oldSampleMask = 0;
    UINT oldVBStride = 0, oldVBOffset = 0;
    DXGI_FORMAT oldIBFormat = DXGI_FORMAT_UNKNOWN; UINT oldIBOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Microsoft::WRL::ComPtr<ID3D11Buffer> oldVSCB0, oldVSCB1, oldPSCB0, oldPSCB1;

    ctx->VSGetShader(oldVS.GetAddressOf(), nullptr, nullptr);
    ctx->PSGetShader(oldPS.GetAddressOf(), nullptr, nullptr);
    ctx->IAGetInputLayout(oldIL.GetAddressOf());
    ctx->IAGetVertexBuffers(0, 1, oldVB.GetAddressOf(), &oldVBStride, &oldVBOffset);
    ctx->IAGetIndexBuffer(oldIB.GetAddressOf(), &oldIBFormat, &oldIBOffset);
    ctx->IAGetPrimitiveTopology(&oldTopo);
    ctx->RSGetState(oldRS.GetAddressOf());
    ctx->OMGetDepthStencilState(oldDSS.GetAddressOf(), &oldStencilRef);
    ctx->OMGetBlendState(oldBS.GetAddressOf(), oldBlendFactor, &oldSampleMask);
    ctx->VSGetConstantBuffers(0, 1, oldVSCB0.GetAddressOf());
    ctx->VSGetConstantBuffers(1, 1, oldVSCB1.GetAddressOf());
    ctx->PSGetConstantBuffers(0, 1, oldPSCB0.GetAddressOf());
    ctx->PSGetConstantBuffers(1, 1, oldPSCB1.GetAddressOf());

    // ---- Set up cylinder pipeline ----
    Camera* cam = m_mapRenderer->GetCamera();
    const MapTransform& t = m_replayCtx.mapTransform;
    const InterpolationSettings& is = m_replayCtx.interpSettings;

    XMMATRIX vp = cam->GetView() * cam->GetProj();
    XMFLOAT3 camP = cam->GetPosition3f();

    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        ctx->Map(m_cylCBFrame.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        CylPerFrame* cb = (CylPerFrame*)mapped.pData;
        XMStoreFloat4x4(&cb->viewProj, XMMatrixTranspose(vp));
        cb->camPos = { camP.x, camP.y, camP.z, 1.f };
        ctx->Unmap(m_cylCBFrame.Get(), 0);
    }

    UINT stride = sizeof(CylVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, m_cylVB.GetAddressOf(), &stride, &offset);
    ctx->IASetIndexBuffer(m_cylIB.Get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetInputLayout(m_cylIL.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(m_cylVS.Get(), nullptr, 0);
    ctx->PSSetShader(m_cylPS.Get(), nullptr, 0);

    ID3D11Buffer* cbs[] = { m_cylCBFrame.Get(), m_cylCBInst.Get() };
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetConstantBuffers(0, 2, cbs);

    ctx->RSSetState(m_cylRS.Get());
    ctx->OMSetDepthStencilState(m_cylDSS.Get(), 0);
    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(m_cylBS.Get(), blendFactor, 0xFFFFFFFF);

    Terrain* terrain = m_mapRenderer->GetTerrain();
    bool hasBounds = (terrain != nullptr);
    float bMinX = 0, bMaxX = 0, bMinZ = 0, bMaxZ = 0;
    if (hasBounds) {
        bMinX = terrain->m_bounds.map_min_x;
        bMaxX = terrain->m_bounds.map_max_x;
        bMinZ = terrain->m_bounds.map_min_z;
        bMaxZ = terrain->m_bounds.map_max_z;
    }

    const bool lodOn = m_uiLayout.lodEnabled;
    const float lodDot = m_uiLayout.lodDotDist;

    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;
        if (ard.type != AgentType::Player && ard.type != AgentType::NPC) continue;

        // Skip cylinder for agents that already have a 3D model loaded.
        if (m_useAgentModels && m_agentMeshIds.count(agentId))
            continue;

        float sx, sy, sz;
        InterpolateAgentPosition(ard, m_debugTimeline, is, sx, sy, sz);
        XMFLOAT3 pos = ApplyMapTransformToPos(sx, sy, sz, t);

        if (hasBounds) {
            pos.x = std::clamp(pos.x, bMinX, bMaxX);
            pos.z = std::clamp(pos.z, bMinZ, bMaxZ);
        }

        // LOD: beyond lodDot distance → dot only (skip 3D draw)
        bool isDot = false;
        if (lodOn) {
            float dx = pos.x - camP.x, dy = pos.y - camP.y, dz = pos.z - camP.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            isDot = (dist > lodDot);
        }
        ard.currentLOD = isDot ? 0 : 2;  // 0=Dot, 2=Cylinder

        if (isDot) continue;

        bool inFog = (m_fogPerspective > 0 && ard.teamId != m_fogPerspective && IsAgentInFog(agentId));
        if (inFog && !m_fogGhostMode) continue;

        bool dead = ard.isDeadAtTime(m_debugTimeline);

        float tiltDeg = dead ? 90.f : ard.knockdownTiltAtTime(m_debugTimeline);
        XMMATRIX world;
        if (tiltDeg > 0.01f)
        {
            float tiltRad = XMConvertToRadians(tiltDeg);
            world = XMMatrixRotationZ(tiltRad) * XMMatrixTranslation(pos.x, pos.y, pos.z);
        }
        else
        {
            world = XMMatrixTranslation(pos.x, pos.y, pos.z);
        }

        XMFLOAT4 color;
        if (inFog)
        {
            color = { 0.4f, 0.4f, 0.4f, 0.30f };
        }
        else if (Team::IsRed(ard.teamId))  color = { 0.816f, 0.282f, 0.282f, 1.f };
        else if (Team::IsBlue(ard.teamId)) color = { 0.290f, 0.565f, 0.847f, 1.f };
        else                       color = { 0.7f, 0.7f, 0.7f, 1.f };

        if (dead)
        {
            float deathTime = ard.deathTransitionTime(m_debugTimeline);
            float fadeIn = std::clamp((m_debugTimeline - deathTime) / 0.25f, 0.f, 1.f);
            fadeIn = fadeIn * fadeIn * (3.f - 2.f * fadeIn);
            float lum = color.x * 0.299f + color.y * 0.587f + color.z * 0.114f;
            float desatR = lum + (color.x - lum) * 0.35f;
            float desatG = lum + (color.y - lum) * 0.35f;
            float desatB = lum + (color.z - lum) * 0.35f;
            color = { desatR * 0.45f, desatG * 0.45f, desatB * 0.45f, 0.6f * fadeIn };
        }

        if (agentId == m_hoveredAgentId)
        {
            color.x = std::min(color.x * 1.25f, 1.0f);
            color.y = std::min(color.y * 1.25f, 1.0f);
            color.z = std::min(color.z * 1.25f, 1.0f);
        }

        {
            D3D11_MAPPED_SUBRESOURCE mapped;
            ctx->Map(m_cylCBInst.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            CylPerInst* cb = (CylPerInst*)mapped.pData;
            XMStoreFloat4x4(&cb->world, XMMatrixTranspose(world));
            cb->teamColor = color;
            ctx->Unmap(m_cylCBInst.Get(), 0);
        }
        ctx->DrawIndexed(m_cylIndexCount, 0, 0);
    }

    // ---- Restore ALL saved D3D11 state ----
    ctx->VSSetShader(oldVS.Get(), nullptr, 0);
    ctx->PSSetShader(oldPS.Get(), nullptr, 0);
    ctx->IASetInputLayout(oldIL.Get());
    ctx->IASetVertexBuffers(0, 1, oldVB.GetAddressOf(), &oldVBStride, &oldVBOffset);
    ctx->IASetIndexBuffer(oldIB.Get(), oldIBFormat, oldIBOffset);
    ctx->IASetPrimitiveTopology(oldTopo);
    ctx->RSSetState(oldRS.Get());
    ctx->OMSetDepthStencilState(oldDSS.Get(), oldStencilRef);
    ctx->OMSetBlendState(oldBS.Get(), oldBlendFactor, oldSampleMask);
    ID3D11Buffer* restoreVSCBs[] = { oldVSCB0.Get(), oldVSCB1.Get() };
    ctx->VSSetConstantBuffers(0, 2, restoreVSCBs);
    ID3D11Buffer* restorePSCBs[] = { oldPSCB0.Get(), oldPSCB1.Get() };
    ctx->PSSetConstantBuffers(0, 2, restorePSCBs);
}
