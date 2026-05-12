#include "pch.h"
#include "GuildBannerAssetProvider.h"
#include "DirectXTex/DirectXTex.h"

void GuildBannerAssetProvider::Init(const std::filesystem::path& assetRoot)
{
    m_root = assetRoot;
    m_initialized = std::filesystem::exists(m_root);
}

const RgbaImage* GuildBannerAssetProvider::GetDetailPattern(int id) const
{
    if (id < 0 || id > 31) return nullptr;
    auto path = m_root / "details" / ("details-" + std::to_string(id) + ".png");
    return LoadCached(m_details, id, path);
}

const RgbaImage* GuildBannerAssetProvider::GetEmblem(int id) const
{
    if (id < 0 || id > 173) return nullptr;
    auto path = m_root / "symbol" / ("symbol-" + std::to_string(id) + ".png");
    return LoadCached(m_emblems, id, path);
}

const RgbaImage* GuildBannerAssetProvider::GetShapeBase(int shape) const
{
    if (shape < 1 || shape > 9) return nullptr;
    auto path = m_root / ("base-" + std::to_string(shape) + ".png");
    return LoadCached(m_shapeBase, shape, path);
}

const RgbaImage* GuildBannerAssetProvider::GetShapeHighlight(int shape) const
{
    if (shape < 1 || shape > 9) return nullptr;
    auto path = m_root / ("base-" + std::to_string(shape) + "h.png");
    return LoadCached(m_shapeHighlight, shape, path);
}

const RgbaImage* GuildBannerAssetProvider::GetShapeColor(int shape) const
{
    if (shape < 1 || shape > 9) return nullptr;
    auto path = m_root / ("base-" + std::to_string(shape) + "c.png");
    return LoadCached(m_shapeColor, shape, path);
}

const RgbaImage* GuildBannerAssetProvider::GetTrim(int trimType, int shape) const
{
    if (trimType <= 0) return nullptr;
    if (shape < 1 || shape > 9) return nullptr;

    int key = TrimKey(trimType, shape);

    static const char* kTrimNames[] = {
        "",        // [0] none
        "silver",  // [1] silver
        "gold",    // [2] gold
        "bronze",  // [3] bronze
        "bronze",  // [4] red      (bronze + hue filter)
        "bronze",  // [5] blue     (bronze + hue filter)
        "bronze",  // [6] green    (bronze + hue filter)
        "bronze",  // [7] purple   (bronze + hue filter)
        "bronze",  // [8] orange   (bronze + hue filter)
        "bronze",  // [9] obsidian (bronze + hue filter)
        "bronze",  // [10] unknown (bronze + hue filter)
        "bronze",  // [11] pink    (bronze + hue filter)
    };

    int nameIdx = (trimType >= 0 && trimType <= 11) ? trimType : 1;
    std::string filename = std::string("trim-") + kTrimNames[nameIdx] + "-" + std::to_string(shape) + ".png";
    auto path = m_root / "trim" / filename;
    return LoadCached(m_trims, key, path);
}

const RgbaImage* GuildBannerAssetProvider::LoadCached(
    std::unordered_map<int, RgbaImage>& cache,
    int key, const std::filesystem::path& path) const
{
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second.IsValid() ? &it->second : nullptr;

    RgbaImage img;
    if (LoadPng(path, img))
    {
        auto [inserted, _] = cache.emplace(key, std::move(img));
        return &inserted->second;
    }

    cache[key] = {};
    return nullptr;
}

bool GuildBannerAssetProvider::LoadPng(const std::filesystem::path& path, RgbaImage& out)
{
    if (!std::filesystem::exists(path))
        return false;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_IGNORE_SRGB, nullptr, image);
    if (FAILED(hr))
        return false;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0)
        return false;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr))
            return false;
    }

    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    out.width = static_cast<int>(img->width);
    out.height = static_cast<int>(img->height);
    out.pixels.resize(out.width * out.height * 4);

    const uint8_t* srcRow = img->pixels;
    for (int y = 0; y < out.height; y++)
    {
        memcpy(out.pixels.data() + y * out.width * 4, srcRow, out.width * 4);
        srcRow += img->rowPitch;
    }

    return true;
}
