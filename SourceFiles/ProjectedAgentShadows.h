#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <vector>
#include <string>
#include <array>

class Terrain;
class AnimatedMeshInstance;

// First shadow milestone: original DAT mask projected onto terrain triangles.
// Material intensity and model bounds remain approximations; see research/shadows.
class ProjectedAgentShadows {
public:
    struct Caster {
        DirectX::XMFLOAT3 position;
        float radius;
        float opacity;
        int modelSlot = -1;
        float height = 0;
        int maskIndex = -1;
        DirectX::XMFLOAT2 projectionShift{};
        DirectX::XMFLOAT2 maskCenter{};
        float maskExtent = 0;
    };
    struct Part { AnimatedMeshInstance* mesh; DirectX::XMFLOAT4X4 world; };
    bool Capture(ID3D11DeviceContext* context, unsigned slot, Caster& caster,
                 DirectX::XMFLOAT2 projectionShift, const std::vector<Part>& parts);
    bool Initialize(ID3D11Device* device, const void* rgba, unsigned width, unsigned height);
    void Draw(ID3D11DeviceContext* context, const Terrain& terrain,
              DirectX::FXMMATRIX viewProj, const std::vector<Caster>& casters, float strength);
    const std::string& Error() const { return error_; }
    unsigned DrawnCount() const { return drawnCount_; }
    unsigned ReceiverSkipCount() const { return receiverSkipCount_; }
    void ResetStats() { drawnCount_ = receiverSkipCount_ = 0; }
    static float DistanceOpacity(float distance, bool originalDistanceLimit);
    bool Ready() const { return ps_ != nullptr; }
private:
    struct Vertex { DirectX::XMFLOAT3 position; DirectX::XMFLOAT2 uv; float opacity; };
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_, vertices_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mask_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> captureVS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> capturePS_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> captureLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> captureCB_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> captureDepth_;
    std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, 32> maskTargets_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 32> masks_;
    size_t capacity_ = 0;
    unsigned drawnCount_ = 0;
    unsigned receiverSkipCount_ = 0;
    std::vector<Vertex> staging_;
    std::string error_;
};
