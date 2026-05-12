#pragma once
#include "GuildBannerAssetProvider.h"
#include "ReplayLibrary.h"
#include <vector>
#include <cstdint>

struct RgbColor
{
    uint8_t r = 0, g = 0, b = 0;
};

class GuildBannerComposer
{
public:
    static constexpr int kWidth = 128;
    static constexpr int kHeight = 256;

    static std::vector<uint8_t> ComposeBanner(const CapeData& cape,
                                              const GuildBannerAssetProvider& assets);

    static RgbColor ColorFromId(int colorId);
    static int EmblemGameToAsset(int gameId);
    static int ShapeGameToAsset(int gameShape);

private:
    static void FillSolid(std::vector<uint8_t>& buf, RgbColor col);
    static void OverlayColorized(std::vector<uint8_t>& buf, const RgbaImage* pattern,
                                 RgbColor col, int yOffset = 0, float contrastMul = 1.0f);
    static void BlendHardLight(std::vector<uint8_t>& buf, const RgbaImage* overlay, float contrast);
    static void BlendScreen(std::vector<uint8_t>& buf, const RgbaImage* overlay, float contrast);
    static void BlendColor(std::vector<uint8_t>& buf, const RgbaImage* overlay);
    static void OverlayTrim(std::vector<uint8_t>& buf, const RgbaImage* trim, int trimType);
    static void ApplyShapeMask(std::vector<uint8_t>& buf, const RgbaImage* shapeBase);

    static uint8_t ApplyContrast(uint8_t val, float contrast);
    static void RgbToHsl(uint8_t r, uint8_t g, uint8_t b, float& h, float& s, float& l);
    static void HslToRgb(float h, float s, float l, uint8_t& r, uint8_t& g, uint8_t& b);
    static void ApplyHueRotation(uint8_t& r, uint8_t& g, uint8_t& b,
                                 float hueRotDeg, float brightness, float contrast, float grayscale);
};
