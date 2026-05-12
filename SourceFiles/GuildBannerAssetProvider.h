#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <filesystem>

struct RgbaImage
{
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;

    bool IsValid() const { return !pixels.empty() && width > 0 && height > 0; }
};

class GuildBannerAssetProvider
{
public:
    void Init(const std::filesystem::path& assetRoot);
    bool IsInitialized() const { return m_initialized; }

    const RgbaImage* GetDetailPattern(int id) const;
    const RgbaImage* GetEmblem(int id) const;
    const RgbaImage* GetShapeBase(int shape) const;
    const RgbaImage* GetShapeHighlight(int shape) const;
    const RgbaImage* GetShapeColor(int shape) const;
    const RgbaImage* GetTrim(int trimType, int shape) const;

private:
    bool m_initialized = false;
    std::filesystem::path m_root;

    mutable std::unordered_map<int, RgbaImage> m_details;
    mutable std::unordered_map<int, RgbaImage> m_emblems;
    mutable std::unordered_map<int, RgbaImage> m_shapeBase;
    mutable std::unordered_map<int, RgbaImage> m_shapeHighlight;
    mutable std::unordered_map<int, RgbaImage> m_shapeColor;
    mutable std::unordered_map<int, RgbaImage> m_trims;

    static int TrimKey(int trimType, int shape) { return trimType * 100 + shape; }
    const RgbaImage* LoadCached(std::unordered_map<int, RgbaImage>& cache,
                                int key, const std::filesystem::path& path) const;
    static bool LoadPng(const std::filesystem::path& path, RgbaImage& out);
};
