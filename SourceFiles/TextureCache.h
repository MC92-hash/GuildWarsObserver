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

private:
    ID3D11Device* m_device = nullptr;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_cache;

    ID3D11ShaderResourceView* LoadFromFile(const std::wstring& wpath);
};

TextureCache& GetTextureCache();
