#pragma once
#include "GuildBannerAssetProvider.h"
#include "GuildBannerComposer.h"
#include "ReplayLibrary.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <unordered_map>
#include <string>
#include "imgui.h"

class GuildCapeCache
{
public:
    void Init(ID3D11Device* device, const std::filesystem::path& assetRoot);
    bool IsReady() const { return m_device != nullptr && m_assets.IsInitialized(); }

    ImTextureID GetOrCreate(const std::string& guildTag, const CapeData& cape);

    void Clear();

private:
    ID3D11Device* m_device = nullptr;
    GuildBannerAssetProvider m_assets;

    struct CacheEntry {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };
    std::unordered_map<std::string, CacheEntry> m_cache;

    ID3D11ShaderResourceView* CreateTextureFromRgba(const std::vector<uint8_t>& pixels,
                                                    int width, int height);
};
