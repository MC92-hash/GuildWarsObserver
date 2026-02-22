#pragma once
#include <DirectXMath.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>

using Microsoft::WRL::ComPtr;

struct AgentMarker
{
    int id = 0;
    DirectX::XMFLOAT3 position = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT4 color = { 1.f, 0.f, 0.f, 1.f };
};

struct OverlayVertex
{
    DirectX::XMFLOAT3 position;
};

struct OverlayInstanceData
{
    DirectX::XMFLOAT4 positionAndRadius; // xyz = world pos, w = radius
    DirectX::XMFLOAT4 color;            // rgba
};

struct OverlayFrameCB
{
    DirectX::XMFLOAT4X4 viewProj;
    float yOffset;
    float pad[3];
};

class AgentOverlay
{
public:
    AgentOverlay(ID3D11Device* device, ID3D11DeviceContext* context);
    ~AgentOverlay() = default;

    bool Initialize();
    void Update();

    // viewProj: combined View * Projection matrix
    // restoreVS/restorePS/restoreIL: main renderer's shaders to re-bind after overlay draws.
    // Pass nullptr to skip restoration (e.g. if nothing renders after the overlay).
    void Render(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& projection,
                ID3D11VertexShader* restoreVS = nullptr,
                ID3D11PixelShader* restorePS = nullptr,
                ID3D11InputLayout* restoreIL = nullptr);

    void SetJsonPath(const std::filesystem::path& path) { m_jsonPath = path; }
    void SetMarkerRadius(float radius) { m_markerRadius = radius; }
    void SetReloadIntervalMs(int ms) { m_reloadIntervalMs = ms; }
    void SetYOffset(float offset) { m_yOffset = offset; }
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    const std::vector<AgentMarker>& GetMarkers() const { return m_markers; }

    // Programmatic marker update (used by MatchReplay; disables file-based reload)
    void SetMarkers(const std::vector<AgentMarker>& markers) { m_markers = markers; m_externalMarkers = true; }
    void SetMarkers(std::vector<AgentMarker>&& markers) { m_markers = std::move(markers); m_externalMarkers = true; }
    void ClearExternalMarkers() { m_externalMarkers = false; }
    bool UsingExternalMarkers() const { return m_externalMarkers; }

private:
    bool CreateShaders();
    bool CreateConstantBuffer();
    void CreateSphereMesh(int slices, int stacks);
    bool CreateInstanceBuffer(UINT maxInstances);
    bool LoadMarkersFromJson();

    static DirectX::XMFLOAT4 TeamNameToColor(const std::string& team);

    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;

    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11InputLayout> m_inputLayout;
    ComPtr<ID3D11Buffer> m_constantBuffer;
    ComPtr<ID3D11Buffer> m_sphereVB;
    ComPtr<ID3D11Buffer> m_sphereIB;
    ComPtr<ID3D11Buffer> m_instanceBuffer;
    ComPtr<ID3D11DepthStencilState> m_depthStencilState;
    ComPtr<ID3D11RasterizerState> m_rasterizerState;
    ComPtr<ID3D11BlendState> m_blendState;

    UINT m_sphereIndexCount = 0;
    UINT m_maxInstances = 256;

    std::vector<AgentMarker> m_markers;
    std::filesystem::path m_jsonPath;
    std::filesystem::file_time_type m_lastFileWriteTime;

    float m_markerRadius = 75.0f;
    float m_yOffset = 50.0f;
    int m_reloadIntervalMs = 150;
    bool m_enabled = true;
    bool m_externalMarkers = false;

    std::chrono::steady_clock::time_point m_lastReloadTime;

    static constexpr UINT CB_SLOT = 4; // High slot to avoid conflicting with main renderer (0-3)
};
