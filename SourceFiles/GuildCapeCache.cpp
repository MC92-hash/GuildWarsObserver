#include "pch.h"
#include "GuildCapeCache.h"

void GuildCapeCache::Init(ID3D11Device* device, const std::filesystem::path& assetRoot)
{
    m_device = device;
    m_assets.Init(assetRoot);
}

ImTextureID GuildCapeCache::GetOrCreate(const std::string& guildTag, const CapeData& cape)
{
    if (!m_device || !m_assets.IsInitialized())
        return nullptr;

    auto it = m_cache.find(guildTag);
    if (it != m_cache.end())
        return static_cast<ImTextureID>(it->second.srv.Get());

    std::vector<uint8_t> pixels = GuildBannerComposer::ComposeBanner(cape, m_assets);
    if (pixels.empty())
        return nullptr;

    ID3D11ShaderResourceView* srv = CreateTextureFromRgba(pixels,
        GuildBannerComposer::kWidth, GuildBannerComposer::kHeight);
    if (!srv)
        return nullptr;

    CacheEntry entry;
    entry.srv.Attach(srv);
    auto [inserted, _] = m_cache.emplace(guildTag, std::move(entry));
    return static_cast<ImTextureID>(inserted->second.srv.Get());
}

void GuildCapeCache::Clear()
{
    m_cache.clear();
}

ID3D11ShaderResourceView* GuildCapeCache::CreateTextureFromRgba(
    const std::vector<uint8_t>& pixels, int width, int height)
{
    if (!m_device || pixels.empty()) return nullptr;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(width);
    texDesc.Height = static_cast<UINT>(height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixels.data();
    initData.SysMemPitch = static_cast<UINT>(width * 4);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = m_device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    hr = m_device->CreateShaderResourceView(tex.Get(), &srvDesc, &srv);
    if (FAILED(hr)) return nullptr;

    return srv;
}
