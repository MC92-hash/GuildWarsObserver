#include "pch.h"
#include "AgentOverlay.h"
#include <json.hpp>
#include <fstream>
#include <cmath>

using namespace DirectX;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Embedded HLSL — instanced rendering, CB at register b4 to avoid conflicts
// ---------------------------------------------------------------------------

static const char overlay_vs[] = R"(
cbuffer OverlayFrameCB : register(b4)
{
    matrix ViewProj;
    float  YOffset;
};

struct VS_INPUT
{
    float3 localPos        : POSITION;
    float4 instPosRadius   : INST_POS;    // xyz = world position, w = radius
    float4 instColor       : INST_COLOR;  // rgba
};

struct PS_INPUT
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
};

PS_INPUT main(VS_INPUT input)
{
    PS_INPUT output;
    float3 worldPos = input.localPos * input.instPosRadius.w
                    + float3(input.instPosRadius.x,
                             input.instPosRadius.y + YOffset,
                             input.instPosRadius.z);
    output.pos   = mul(float4(worldPos, 1.0f), ViewProj);
    output.color = input.instColor;
    return output;
}
)";

static const char overlay_ps[] = R"(
struct PS_INPUT { float4 pos : SV_POSITION; float4 color : COLOR0; };
float4 main(PS_INPUT input) : SV_TARGET { return input.color; }
)";

// ---------------------------------------------------------------------------
AgentOverlay::AgentOverlay(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_device(device)
    , m_context(context)
    , m_lastReloadTime(std::chrono::steady_clock::now())
{
}

bool AgentOverlay::Initialize()
{
    if (!CreateShaders())        return false;
    if (!CreateConstantBuffer()) return false;
    CreateSphereMesh(8, 6);
    if (!CreateInstanceBuffer(m_maxInstances)) return false;

    if (m_jsonPath.empty())
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        m_jsonPath = std::filesystem::path(exePath).parent_path() / "agents.json";
    }

    LoadMarkersFromJson();
    return true;
}

// ---------------------------------------------------------------------------
// Shader & pipeline state creation
// ---------------------------------------------------------------------------

bool AgentOverlay::CreateShaders()
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(DEBUG) || defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vsBlob, vsErr;
    HRESULT hr = D3DCompile(overlay_vs, strlen(overlay_vs), nullptr, nullptr, nullptr,
        "main", "vs_5_0", flags, 0, vsBlob.GetAddressOf(), vsErr.GetAddressOf());
    if (FAILED(hr))
    {
        if (vsErr) OutputDebugStringA(static_cast<const char*>(vsErr->GetBufferPointer()));
        return false;
    }
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, m_vertexShader.GetAddressOf());
    if (FAILED(hr)) return false;

    // Input layout: stream 0 = per-vertex position, stream 1 = per-instance data
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,                                  D3D11_INPUT_PER_VERTEX_DATA,   0 },
        { "INST_POS",   0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, offsetof(OverlayInstanceData, positionAndRadius), D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INST_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, offsetof(OverlayInstanceData, color),             D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };
    hr = m_device->CreateInputLayout(layoutDesc, _countof(layoutDesc),
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_inputLayout.GetAddressOf());
    if (FAILED(hr)) return false;

    ComPtr<ID3DBlob> psBlob, psErr;
    hr = D3DCompile(overlay_ps, strlen(overlay_ps), nullptr, nullptr, nullptr,
        "main", "ps_5_0", flags, 0, psBlob.GetAddressOf(), psErr.GetAddressOf());
    if (FAILED(hr))
    {
        if (psErr) OutputDebugStringA(static_cast<const char*>(psErr->GetBufferPointer()));
        return false;
    }
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, m_pixelShader.GetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable    = FALSE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    m_device->CreateDepthStencilState(&dsDesc, m_depthStencilState.GetAddressOf());

    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode        = D3D11_FILL_SOLID;
    rsDesc.CullMode        = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = TRUE;
    m_device->CreateRasterizerState(&rsDesc, m_rasterizerState.GetAddressOf());

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable           = TRUE;
    blendDesc.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_device->CreateBlendState(&blendDesc, m_blendState.GetAddressOf());

    return true;
}

bool AgentOverlay::CreateConstantBuffer()
{
    D3D11_BUFFER_DESC desc = {};
    desc.Usage          = D3D11_USAGE_DYNAMIC;
    desc.ByteWidth      = sizeof(OverlayFrameCB);
    desc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(m_device->CreateBuffer(&desc, nullptr, m_constantBuffer.GetAddressOf()));
}

bool AgentOverlay::CreateInstanceBuffer(UINT maxInstances)
{
    D3D11_BUFFER_DESC desc = {};
    desc.Usage          = D3D11_USAGE_DYNAMIC;
    desc.ByteWidth      = maxInstances * sizeof(OverlayInstanceData);
    desc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(m_device->CreateBuffer(&desc, nullptr, m_instanceBuffer.GetAddressOf()));
}

// ---------------------------------------------------------------------------
// Low-poly sphere (unit sphere, scaled via instance data at draw time)
// ---------------------------------------------------------------------------

void AgentOverlay::CreateSphereMesh(int slices, int stacks)
{
    std::vector<OverlayVertex> vertices;
    std::vector<UINT16> indices;

    for (int i = 0; i <= stacks; ++i)
    {
        float phi = XM_PI * static_cast<float>(i) / static_cast<float>(stacks);
        float y = cosf(phi);
        float r = sinf(phi);
        for (int j = 0; j <= slices; ++j)
        {
            float theta = 2.0f * XM_PI * static_cast<float>(j) / static_cast<float>(slices);
            vertices.push_back({ { r * cosf(theta), y, r * sinf(theta) } });
        }
    }

    for (int i = 0; i < stacks; ++i)
    {
        for (int j = 0; j < slices; ++j)
        {
            UINT16 tl = static_cast<UINT16>(i * (slices + 1) + j);
            UINT16 tr = tl + 1;
            UINT16 bl = static_cast<UINT16>((i + 1) * (slices + 1) + j);
            UINT16 br = bl + 1;
            indices.push_back(tl); indices.push_back(bl); indices.push_back(tr);
            indices.push_back(tr); indices.push_back(bl); indices.push_back(br);
        }
    }

    m_sphereIndexCount = static_cast<UINT>(indices.size());

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage     = D3D11_USAGE_IMMUTABLE;
    vbDesc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(OverlayVertex));
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData = { vertices.data() };
    m_device->CreateBuffer(&vbDesc, &vbData, m_sphereVB.GetAddressOf());

    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.Usage     = D3D11_USAGE_IMMUTABLE;
    ibDesc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(UINT16));
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData = { indices.data() };
    m_device->CreateBuffer(&ibDesc, &ibData, m_sphereIB.GetAddressOf());
}

// ---------------------------------------------------------------------------
// JSON loading
// ---------------------------------------------------------------------------

XMFLOAT4 AgentOverlay::TeamNameToColor(const std::string& team)
{
    if (team == "red")    return { 1.0f, 0.2f, 0.2f, 0.85f };
    if (team == "blue")   return { 0.2f, 0.4f, 1.0f, 0.85f };
    if (team == "green")  return { 0.2f, 0.9f, 0.3f, 0.85f };
    if (team == "yellow") return { 1.0f, 0.9f, 0.1f, 0.85f };
    if (team == "white")  return { 1.0f, 1.0f, 1.0f, 0.85f };
    if (team == "purple") return { 0.7f, 0.2f, 0.9f, 0.85f };
    if (team == "orange") return { 1.0f, 0.5f, 0.0f, 0.85f };
    if (team == "cyan")   return { 0.0f, 0.9f, 0.9f, 0.85f };
    return { 1.0f, 1.0f, 1.0f, 0.85f };
}

bool AgentOverlay::LoadMarkersFromJson()
{
    if (!std::filesystem::exists(m_jsonPath))
        return false;

    try
    {
        auto currentWriteTime = std::filesystem::last_write_time(m_jsonPath);
        if (currentWriteTime == m_lastFileWriteTime && !m_markers.empty())
            return true;
        m_lastFileWriteTime = currentWriteTime;

        std::ifstream file(m_jsonPath);
        if (!file.is_open()) return false;

        json root = json::parse(file);
        if (!root.is_array()) return false;

        std::vector<AgentMarker> newMarkers;
        newMarkers.reserve(root.size());

        for (auto& entry : root)
        {
            AgentMarker marker;
            marker.id         = entry.value("id", 0);
            marker.position.x = entry.value("x", 0.0f);
            marker.position.y = entry.value("y", 0.0f);
            marker.position.z = entry.value("z", 0.0f);

            if (entry.contains("team"))
                marker.color = TeamNameToColor(entry["team"].get<std::string>());
            else
            {
                marker.color.x = entry.value("r", 1.0f);
                marker.color.y = entry.value("g", 1.0f);
                marker.color.z = entry.value("b", 1.0f);
                marker.color.w = entry.value("a", 0.85f);
            }
            newMarkers.push_back(marker);
        }

        m_markers = std::move(newMarkers);
        return true;
    }
    catch (const std::exception& e)
    {
        OutputDebugStringA("AgentOverlay: JSON parse error: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        return false;
    }
}

// ---------------------------------------------------------------------------
// Update: periodic JSON reload
// ---------------------------------------------------------------------------

void AgentOverlay::Update()
{
    if (!m_enabled) return;

    // When replay feeds markers via SetMarkers(), skip file-based reload
    if (m_externalMarkers) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastReloadTime).count();
    if (elapsed >= m_reloadIntervalMs)
    {
        m_lastReloadTime = now;
        // #region agent log
        auto _t0 = std::chrono::high_resolution_clock::now();
        // #endregion
        LoadMarkersFromJson();
        // #region agent log
        {
            double _ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - _t0).count();
            auto _ts = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            std::ofstream _lf("D:\\Guild Wars OBS\\GuildWarsObserver\\debug-dddcee.log", std::ios::app);
            _lf << "{\"sessionId\":\"dddcee\",\"hypothesisId\":\"A\",\"location\":\"AgentOverlay.cpp:Update\",\"message\":\"file_reload\",\"data\":{\"ms\":" << _ms << ",\"markers\":" << m_markers.size() << "},\"timestamp\":" << _ts << "}\n";
        }
        // #endregion
    }
}

// ---------------------------------------------------------------------------
// Render — single instanced draw call, minimal state changes
// ---------------------------------------------------------------------------

void AgentOverlay::Render(const XMMATRIX& view, const XMMATRIX& projection,
                          ID3D11VertexShader* restoreVS,
                          ID3D11PixelShader* restorePS,
                          ID3D11InputLayout* restoreIL)
{
    if (!m_enabled || m_markers.empty()) return;
    if (!m_vertexShader || !m_pixelShader || !m_sphereVB || !m_sphereIB || !m_instanceBuffer) return;

    UINT instanceCount = static_cast<UINT>(m_markers.size());

    // Grow instance buffer if needed
    if (instanceCount > m_maxInstances)
    {
        m_maxInstances = instanceCount + 64;
        CreateInstanceBuffer(m_maxInstances);
    }

    // Upload per-instance data (single Map/Unmap for all markers)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_context->Map(m_instanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return;

        auto* dst = static_cast<OverlayInstanceData*>(mapped.pData);
        for (UINT i = 0; i < instanceCount; ++i)
        {
            const auto& m = m_markers[i];
            dst[i].positionAndRadius = { m.position.x, m.position.y, m.position.z, m_markerRadius };
            dst[i].color = m.color;
        }
        m_context->Unmap(m_instanceBuffer.Get(), 0);
    }

    // Upload frame constant buffer (ViewProj + YOffset)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return;

        auto* cb = static_cast<OverlayFrameCB*>(mapped.pData);
        XMStoreFloat4x4(&cb->viewProj, XMMatrixTranspose(view * projection));
        cb->yOffset = m_yOffset;
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    // --- Set overlay pipeline state ---
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
    m_context->RSSetState(m_rasterizerState.Get());
    float blendFactor[] = { 0.f, 0.f, 0.f, 0.f };
    m_context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Bind two vertex streams: 0 = sphere mesh, 1 = instance data
    ID3D11Buffer* vbs[2]   = { m_sphereVB.Get(), m_instanceBuffer.Get() };
    UINT strides[2]        = { sizeof(OverlayVertex), sizeof(OverlayInstanceData) };
    UINT offsets[2]        = { 0, 0 };
    m_context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    m_context->IASetIndexBuffer(m_sphereIB.Get(), DXGI_FORMAT_R16_UINT, 0);

    // CB at slot 4 — does not conflict with main renderer's slots 0-3
    m_context->VSSetConstantBuffers(CB_SLOT, 1, m_constantBuffer.GetAddressOf());

    // --- Single instanced draw call for ALL markers ---
    m_context->DrawIndexedInstanced(m_sphereIndexCount, instanceCount, 0, 0, 0);

    // --- Restore main renderer state ---
    // Only VS, PS, and InputLayout need explicit restoration because the main
    // renderer binds them once in Initialize() and never re-sets them per frame.
    // Depth/blend/rasterizer states are set per-mesh by MeshInstance, so they
    // self-heal on the next frame. CB slots 0-3 were never touched.
    if (restoreVS)  m_context->VSSetShader(restoreVS, nullptr, 0);
    if (restorePS)  m_context->PSSetShader(restorePS, nullptr, 0);
    if (restoreIL)  m_context->IASetInputLayout(restoreIL);
}
