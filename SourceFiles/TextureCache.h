#pragma once
#include <d3d11.h>
#include <string>
#include <unordered_map>
#include <wrl/client.h>
#include "imgui.h"

class TextureCache
{
public:
    void Init(ID3D11Device* device);
    void Shutdown();

    ImTextureID GetTexture(const std::string& filePath);
    ImTextureID GetTexture(const std::wstring& filePath);

    bool IsInitialized() const { return m_device != nullptr; }

    // For callers that build their own textures and want the device this cache was given,
    // rather than threading it through the UI layer a second time.
    ID3D11Device* Device() const { return m_device; }

private:
    ID3D11Device* m_device = nullptr;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_cache;

    ID3D11ShaderResourceView* LoadFromFile(const std::wstring& wpath);
};

TextureCache& GetTextureCache();
