#include "pch.h"
#include "TextureCache.h"
#include "DirectXTex/DirectXTex.h"

static TextureCache g_textureCache;

TextureCache& GetTextureCache()
{
    return g_textureCache;
}

void TextureCache::Init(ID3D11Device* device)
{
    m_device = device;
}

void TextureCache::Shutdown()
{
    m_cache.clear();
    m_device = nullptr;
}

ImTextureID TextureCache::GetTexture(const std::string& filePath)
{
    if (!m_device) return nullptr;

    auto it = m_cache.find(filePath);
    if (it != m_cache.end())
        return (ImTextureID)it->second.Get();

    std::wstring wpath(filePath.begin(), filePath.end());
    auto* srv = LoadFromFile(wpath);
    if (srv)
    {
        m_cache[filePath].Attach(srv);
        return (ImTextureID)srv;
    }
    return nullptr;
}

ImTextureID TextureCache::GetTexture(const std::wstring& filePath)
{
    std::string key(filePath.begin(), filePath.end());
    return GetTexture(key);
}

ID3D11ShaderResourceView* TextureCache::LoadFromFile(const std::wstring& wpath)
{
    if (!m_device) return nullptr;
    if (!std::filesystem::exists(wpath)) return nullptr;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(wpath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }

    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    hr = m_device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
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
