#pragma once

// ============================================================================
// HeatmapRenderer — terrain-conform mesh overlay with per-layer density +
// selectable palette LUT colouring.
//
// TECHNIQUE: Approach B (terrain-conform mesh) + CPU Gaussian splat (C).
// A 128x128 quad grid is snapped to terrain height. Each layer has its own
// 512x512 R32_FLOAT density texture. Six 256x1 RGBA8 colour-ramp LUTs are
// baked at startup.  One DrawIndexed call per enabled layer (painter order).
// ============================================================================

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>
#include "HeatmapData.h"

class Terrain;

class HeatmapRenderer
{
public:
    static constexpr int kGridRes     = 192;
    static constexpr int kNumPalettes = static_cast<int>(HeatmapPalette::COUNT);

    bool Init(ID3D11Device* device);
    void BuildMesh(ID3D11Device* device, const Terrain* terrain,
                   float minX, float maxX, float minZ, float maxZ,
                   float waterLevel = 0.0f);

    void EnsureLayerTextures(ID3D11Device* device, size_t layerCount);
    void UpdateLayerDensityTexture(ID3D11DeviceContext* ctx, size_t layerIdx,
                                   const HeatmapAccumulator& accum);
    void RenderLayers(ID3D11DeviceContext* ctx,
                      const DirectX::XMMATRIX& viewProj,
                      const std::vector<HeatmapLayerDef>& layers);

    bool IsReady() const { return m_meshReady && m_vs && m_ps; }

    ID3D11ShaderResourceView* GetLutSRV(HeatmapPalette p) const
    {
        int i = static_cast<int>(p);
        return (i >= 0 && i < kNumPalettes) ? m_lutSRV[i].Get() : nullptr;
    }

private:
    void CreateAllLUTs(ID3D11Device* device);
    void CompileShaders(ID3D11Device* device);

    struct HeatmapVertex { float x, y, z, u, v; };

    struct alignas(16) HeatmapCB
    {
        DirectX::XMFLOAT4X4 viewProj;
        float opacity;
        float pad[3];
    };

    // Mesh (shared across all layers)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ib;
    UINT m_indexCount = 0;
    bool m_meshReady  = false;

    // Per-palette LUT textures
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_lutTex[kNumPalettes];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_lutSRV[kNumPalettes];

    // Per-layer density textures
    struct LayerGPU
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D>          densityTex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> densitySRV;
    };
    std::vector<LayerGPU> m_layers;

    // Shaders + pipeline state (shared)
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       m_ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>       m_il;
    Microsoft::WRL::ComPtr<ID3D11Buffer>            m_cb;

    Microsoft::WRL::ComPtr<ID3D11BlendState>        m_blendState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dss;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   m_rs;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      m_sampler;
};
