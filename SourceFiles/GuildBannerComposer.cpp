#include "pch.h"
#include "GuildBannerComposer.h"
#include <algorithm>
#include <cmath>

// GW color palette: 16 hues, each with a dark endpoint and light endpoint.
// Reverse-engineered from gw-memorial.net guildBanner-1.1.min.js colorsRgb array.
struct HueEndpoints { uint8_t darkR, darkG, darkB, lightR, lightG, lightB; };

static constexpr HueEndpoints kGwPalette[16] = {
    { 55,   0,   0, 246, 130, 130 },  // [0]  Red
    { 117, 57,   0, 222, 160,  83 },  // [1]  Orange
    { 75,  40,   0, 166, 131,  53 },  // [2]  Brown
    { 70,  61,   0, 184, 169,  37 },  // [3]  Yellow
    { 0,   35,   0,  36,  91,   0 },  // [4]  DkGreen
    { 40,  96,  15, 144, 196, 119 },  // [5]  Green
    { 152, 131, 25, 255, 239, 146 },  // [6]  Olive/Gold
    { 0,   90,  51,  74, 182, 141 },  // [7]  Teal
    { 0,    0,  85, 109, 117, 229 },  // [8]  Blue
    { 0,    0,  35,   6,  50, 128 },  // [9]  Navy
    { 0,   79, 117, 155, 248, 255 },  // [10] Cyan
    { 40,   0,  30,  96,   0,  89 },  // [11] Purple
    { 45,   0,  86, 149,  83, 191 },  // [12] Violet
    { 51,   0,  11,  99,   0,  57 },  // [13] Maroon
    { 95,   0,  33, 182,  61, 107 },  // [14] Pink
    { 20,  20,  20, 240, 240, 240 },  // [15] Gray
};

RgbColor GuildBannerComposer::ColorFromId(int colorId)
{
    if (colorId < 0) colorId = 0;
    if (colorId > 255) colorId = 255;

    int hue = colorId / 16;
    int shade = colorId % 16;
    float t = shade / 15.0f;

    const auto& ep = kGwPalette[hue];
    RgbColor c;
    c.r = static_cast<uint8_t>(ep.darkR + (ep.lightR - ep.darkR) * t);
    c.g = static_cast<uint8_t>(ep.darkG + (ep.lightG - ep.darkG) * t);
    c.b = static_cast<uint8_t>(ep.darkB + (ep.lightB - ep.darkB) * t);
    return c;
}

uint8_t GuildBannerComposer::ApplyContrast(uint8_t val, float contrast)
{
    float f = static_cast<float>(val) / 255.0f;
    f = (f - 0.5f) * contrast + 0.5f;
    f = std::clamp(f, 0.0f, 1.0f);
    return static_cast<uint8_t>(f * 255.0f);
}

void GuildBannerComposer::FillSolid(std::vector<uint8_t>& buf, RgbColor col)
{
    int total = kWidth * kHeight;
    for (int i = 0; i < total; i++)
    {
        int idx = i * 4;
        buf[idx + 0] = col.r;
        buf[idx + 1] = col.g;
        buf[idx + 2] = col.b;
        buf[idx + 3] = 255;
    }
}

void GuildBannerComposer::OverlayColorized(std::vector<uint8_t>& buf, const RgbaImage* pattern,
                                           RgbColor col, int yOffset, float contrastMul)
{
    if (!pattern || !pattern->IsValid()) return;

    for (int y = 0; y < pattern->height; y++)
    {
        int destY = y + yOffset;
        if (destY < 0 || destY >= kHeight) continue;

        for (int x = 0; x < pattern->width && x < kWidth; x++)
        {
            int srcIdx = (y * pattern->width + x) * 4;
            int dstIdx = (destY * kWidth + x) * 4;

            uint8_t srcA = pattern->pixels[srcIdx + 3];
            if (srcA == 0) continue;

            uint8_t srcR = col.r;
            uint8_t srcG = col.g;
            uint8_t srcB = col.b;

            if (contrastMul != 1.0f)
            {
                srcR = ApplyContrast(srcR, contrastMul);
                srcG = ApplyContrast(srcG, contrastMul);
                srcB = ApplyContrast(srcB, contrastMul);
            }

            float alpha = srcA / 255.0f;
            float invA = 1.0f - alpha;

            buf[dstIdx + 0] = static_cast<uint8_t>(srcR * alpha + buf[dstIdx + 0] * invA);
            buf[dstIdx + 1] = static_cast<uint8_t>(srcG * alpha + buf[dstIdx + 1] * invA);
            buf[dstIdx + 2] = static_cast<uint8_t>(srcB * alpha + buf[dstIdx + 2] * invA);
        }
    }
}

void GuildBannerComposer::BlendHardLight(std::vector<uint8_t>& buf, const RgbaImage* overlay, float contrast)
{
    if (!overlay || !overlay->IsValid()) return;

    int total = kWidth * kHeight;
    for (int i = 0; i < total; i++)
    {
        int idx = i * 4;
        int oIdx = i * 4;
        if (oIdx + 3 >= static_cast<int>(overlay->pixels.size())) break;

        uint8_t oA = overlay->pixels[oIdx + 3];
        if (oA == 0) continue;

        float alpha = oA / 255.0f;

        for (int c = 0; c < 3; c++)
        {
            uint8_t base = buf[idx + c];
            uint8_t blend = ApplyContrast(overlay->pixels[oIdx + c], contrast);

            int result;
            if (blend < 128)
                result = (2 * base * blend) / 255;
            else
                result = 255 - (2 * (255 - base) * (255 - blend)) / 255;

            result = std::clamp(result, 0, 255);
            buf[idx + c] = static_cast<uint8_t>(base + (result - base) * alpha);
        }
    }
}

void GuildBannerComposer::BlendScreen(std::vector<uint8_t>& buf, const RgbaImage* overlay, float contrast)
{
    if (!overlay || !overlay->IsValid()) return;

    int total = kWidth * kHeight;
    for (int i = 0; i < total; i++)
    {
        int idx = i * 4;
        int oIdx = i * 4;
        if (oIdx + 3 >= static_cast<int>(overlay->pixels.size())) break;

        uint8_t oA = overlay->pixels[oIdx + 3];
        if (oA == 0) continue;

        float alpha = oA / 255.0f;

        for (int c = 0; c < 3; c++)
        {
            uint8_t base = buf[idx + c];
            uint8_t blend = ApplyContrast(overlay->pixels[oIdx + c], contrast);

            int result = 255 - ((255 - base) * (255 - blend)) / 255;
            result = std::clamp(result, 0, 255);
            buf[idx + c] = static_cast<uint8_t>(base + (result - base) * alpha);
        }
    }
}

void GuildBannerComposer::RgbToHsl(uint8_t r, uint8_t g, uint8_t b, float& h, float& s, float& l)
{
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float maxC = (std::max)({ rf, gf, bf });
    float minC = (std::min)({ rf, gf, bf });
    float delta = maxC - minC;

    l = (maxC + minC) * 0.5f;
    if (delta < 0.00001f)
    {
        h = 0.0f;
        s = 0.0f;
        return;
    }

    s = (l > 0.5f) ? delta / (2.0f - maxC - minC) : delta / (maxC + minC);

    if (maxC == rf)
        h = std::fmod((gf - bf) / delta, 6.0f);
    else if (maxC == gf)
        h = (bf - rf) / delta + 2.0f;
    else
        h = (rf - gf) / delta + 4.0f;

    h /= 6.0f;
    if (h < 0.0f) h += 1.0f;
}

static float HueToRgb(float p, float q, float t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

void GuildBannerComposer::HslToRgb(float h, float s, float l, uint8_t& r, uint8_t& g, uint8_t& b)
{
    if (s < 0.00001f)
    {
        uint8_t v = static_cast<uint8_t>(std::clamp(l * 255.0f, 0.0f, 255.0f));
        r = g = b = v;
        return;
    }

    float q = (l < 0.5f) ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;

    r = static_cast<uint8_t>(std::clamp(HueToRgb(p, q, h + 1.0f / 3.0f) * 255.0f, 0.0f, 255.0f));
    g = static_cast<uint8_t>(std::clamp(HueToRgb(p, q, h) * 255.0f, 0.0f, 255.0f));
    b = static_cast<uint8_t>(std::clamp(HueToRgb(p, q, h - 1.0f / 3.0f) * 255.0f, 0.0f, 255.0f));
}

void GuildBannerComposer::BlendColor(std::vector<uint8_t>& buf, const RgbaImage* overlay)
{
    if (!overlay || !overlay->IsValid()) return;

    int total = kWidth * kHeight;
    for (int i = 0; i < total; i++)
    {
        int idx = i * 4;
        int oIdx = i * 4;
        if (oIdx + 3 >= static_cast<int>(overlay->pixels.size())) break;

        uint8_t oA = overlay->pixels[oIdx + 3];
        if (oA == 0) continue;

        float alpha = oA / 255.0f;

        float baseH, baseS, baseL;
        RgbToHsl(buf[idx], buf[idx + 1], buf[idx + 2], baseH, baseS, baseL);

        float blendH, blendS, blendL;
        RgbToHsl(overlay->pixels[oIdx], overlay->pixels[oIdx + 1], overlay->pixels[oIdx + 2],
                 blendH, blendS, blendL);

        uint8_t rr, gg, bb;
        HslToRgb(blendH, blendS, baseL, rr, gg, bb);

        buf[idx + 0] = static_cast<uint8_t>(buf[idx + 0] + (rr - buf[idx + 0]) * alpha);
        buf[idx + 1] = static_cast<uint8_t>(buf[idx + 1] + (gg - buf[idx + 1]) * alpha);
        buf[idx + 2] = static_cast<uint8_t>(buf[idx + 2] + (bb - buf[idx + 2]) * alpha);
    }
}

void GuildBannerComposer::ApplyHueRotation(uint8_t& r, uint8_t& g, uint8_t& b,
                                           float hueRotDeg, float brightness, float contrast, float grayscale)
{
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;

    // Grayscale
    if (grayscale > 0.0f)
    {
        float gray = 0.2126f * rf + 0.7152f * gf + 0.0722f * bf;
        rf = rf + (gray - rf) * grayscale;
        gf = gf + (gray - gf) * grayscale;
        bf = bf + (gray - bf) * grayscale;
    }

    // Hue rotation via HSL
    if (std::abs(hueRotDeg) > 0.1f)
    {
        float h, s, l;
        uint8_t tr = static_cast<uint8_t>(std::clamp(rf * 255.0f, 0.0f, 255.0f));
        uint8_t tg = static_cast<uint8_t>(std::clamp(gf * 255.0f, 0.0f, 255.0f));
        uint8_t tb = static_cast<uint8_t>(std::clamp(bf * 255.0f, 0.0f, 255.0f));
        RgbToHsl(tr, tg, tb, h, s, l);
        h += hueRotDeg / 360.0f;
        if (h < 0.0f) h += 1.0f;
        if (h > 1.0f) h -= 1.0f;
        HslToRgb(h, s, l, tr, tg, tb);
        rf = tr / 255.0f;
        gf = tg / 255.0f;
        bf = tb / 255.0f;
    }

    // Brightness
    rf *= brightness;
    gf *= brightness;
    bf *= brightness;

    // Contrast
    rf = (rf - 0.5f) * contrast + 0.5f;
    gf = (gf - 0.5f) * contrast + 0.5f;
    bf = (bf - 0.5f) * contrast + 0.5f;

    r = static_cast<uint8_t>(std::clamp(rf * 255.0f, 0.0f, 255.0f));
    g = static_cast<uint8_t>(std::clamp(gf * 255.0f, 0.0f, 255.0f));
    b = static_cast<uint8_t>(std::clamp(bf * 255.0f, 0.0f, 255.0f));
}

void GuildBannerComposer::OverlayTrim(std::vector<uint8_t>& buf, const RgbaImage* trim, int trimType)
{
    if (!trim || !trim->IsValid()) return;

    // CSS filter parameters for colored trims from guildBanner-1.1.min.js trimFilter[]
    // Game encoding: 0=none, 1=silver, 2=gold, 3=bronze, 4=red, 5=blue,
    //   6=green, 7=purple, 8=orange, 9=obsidian, 10=?, 11=pink
    struct TrimFilter { float hueRot; float brightness; float contrast; float grayscale; };
    static constexpr TrimFilter kFilters[] = {
        { 0, 1, 1, 0 },              // [0] none
        { 0, 1, 1, 0 },              // [1] silver
        { 0, 1, 1, 0 },              // [2] gold
        { 0, 1, 1, 0 },              // [3] bronze
        { 330, 0.95f, 1.3f, 0 },     // [4] red
        { 215, 1.0f, 1.25f, 0 },     // [5] blue
        { 133, 1.4f, 1.3f, 0 },      // [6] green
        { 265, 0.82f, 1.15f, 0 },    // [7] purple
        { 355, 1.4f, 1.1f, 0.3f },   // [8] orange
        { 0, 1.0f, 1.55f, 1.0f },    // [9] obsidian
        { 0, 1, 1, 0 },              // [10] unknown
        { 305, 0.86f, 1.15f, 0 },    // [11] pink
    };

    bool needsFilter = (trimType >= 4 && trimType <= 11 && trimType != 10);
    TrimFilter filter = { 0, 1, 1, 0 };
    if (needsFilter && trimType < 12)
        filter = kFilters[trimType];

    for (int i = 0; i < kWidth * kHeight; i++)
    {
        int srcIdx = i * 4;
        int dstIdx = i * 4;
        if (srcIdx + 3 >= static_cast<int>(trim->pixels.size())) break;

        uint8_t srcA = trim->pixels[srcIdx + 3];
        if (srcA == 0) continue;

        uint8_t tr = trim->pixels[srcIdx + 0];
        uint8_t tg = trim->pixels[srcIdx + 1];
        uint8_t tb = trim->pixels[srcIdx + 2];

        if (needsFilter)
            ApplyHueRotation(tr, tg, tb, filter.hueRot, filter.brightness, filter.contrast, filter.grayscale);

        float alpha = srcA / 255.0f;
        float invA = 1.0f - alpha;
        buf[dstIdx + 0] = static_cast<uint8_t>(tr * alpha + buf[dstIdx + 0] * invA);
        buf[dstIdx + 1] = static_cast<uint8_t>(tg * alpha + buf[dstIdx + 1] * invA);
        buf[dstIdx + 2] = static_cast<uint8_t>(tb * alpha + buf[dstIdx + 2] * invA);
    }
}

void GuildBannerComposer::ApplyShapeMask(std::vector<uint8_t>& buf, const RgbaImage* shapeBase)
{
    if (!shapeBase || !shapeBase->IsValid()) return;

    for (int i = 0; i < kWidth * kHeight; i++)
    {
        int sIdx = i * 4;
        int dIdx = i * 4;
        if (sIdx + 3 >= static_cast<int>(shapeBase->pixels.size())) break;

        uint8_t maskA = shapeBase->pixels[sIdx + 3];
        if (maskA == 0)
            buf[dIdx + 3] = 0;
        else if (maskA < 255)
            buf[dIdx + 3] = static_cast<uint8_t>((buf[dIdx + 3] * maskA) / 255);
    }
}

// Game emblem ID (0-171) -> gw-memorial symbol-{index}.png asset index.
// Manually mapped via CapeDebugWindow toolbox module.
static constexpr int kEmblemLookup[172] = {
     53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  // game  0- 9
     63,  64, 137,  66,  67,  68,  69,  70,  71,  72,  // game 10-19
     73,  74,  75,  76,  77,  78,  79,  80,  31,  82,  // game 20-29
     83,  84, 158,  86,  87,   0,  88,  89,  90,  91,  // game 30-39
     92,  93,  94,  95,  96,  97, 172,  99,   3, 101,  // game 40-49
    102, 103, 104, 105, 106, 107, 108, 109, 110, 111,  // game 50-59
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121,  // game 60-69
    122, 123, 124, 125, 126, 127, 128, 129, 130, 131,  // game 70-79
    132, 133, 134, 135,  12,  13,  14,  15,  16,  17,  // game 80-89
     18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  // game 90-99
     28, 173, 169, 168, 167, 166, 165, 164, 163, 162,  // game 100-109
    161, 160, 159, 152, 150, 149, 147, 144, 143, 142,  // game 110-119
    141, 140, 139, 138,  85, 136,  29,  11,  10,   9,  // game 120-129
      8,   7,   6,   5,   4,  98,   2,   1,  81, 171,  // game 130-139
    170,  65, 157, 156, 155, 154, 153, 151, 148, 146,  // game 140-149
    145,  30, 100,  32,  33,  34,  35,  36,  37,  38,  // game 150-159
     39,  40,  41,  42,  43,  44,  45,  46,  47,  48,  // game 160-169
     49,  50,                                           // game 170-171
};

int GuildBannerComposer::EmblemGameToAsset(int gameId)
{
    if (gameId < 0 || gameId >= 172) return -1;
    return kEmblemLookup[gameId];
}

// Game shape (1-9) -> gw-memorial base-{index}.png asset number.
// Index 0 unused; game uses 1-based shape IDs.
static constexpr int kShapeLookup[10] = {
    0,  // [0] unused
    6,  // [1] -> base-6.png
    1,  // [2] -> base-1.png
    8,  // [3] -> base-8.png
    9,  // [4] -> base-9.png
    7,  // [5] -> base-7.png
    2,  // [6] -> base-2.png
    3,  // [7] -> base-3.png
    4,  // [8] -> base-4.png
    5,  // [9] -> base-5.png
};

int GuildBannerComposer::ShapeGameToAsset(int gameShape)
{
    if (gameShape < 1 || gameShape > 9) return 1;
    return kShapeLookup[gameShape];
}

std::vector<uint8_t> GuildBannerComposer::ComposeBanner(const CapeData& cape,
                                                        const GuildBannerAssetProvider& assets)
{
    std::vector<uint8_t> buf(kWidth * kHeight * 4, 0);

    int shape = ShapeGameToAsset(std::clamp(cape.shape, 1, 9));

    // 1. Fill background with solid color
    RgbColor bgCol = ColorFromId(cape.bg_color);
    FillSolid(buf, bgCol);

    // 2. Overlay detail pattern (colorized)
    if (cape.detail > 0)
    {
        RgbColor detailCol = ColorFromId(cape.detail_color);
        const RgbaImage* detail = assets.GetDetailPattern(cape.detail - 1);
        OverlayColorized(buf, detail, detailCol);
    }

    // 3. Apply shape blend layers (base-Nh.png = hard-light, base-Nc.png = color)
    const RgbaImage* shapeBase = assets.GetShapeBase(shape);
    const RgbaImage* shapeHigh = assets.GetShapeHighlight(shape);
    const RgbaImage* shapeCol = assets.GetShapeColor(shape);

    BlendHardLight(buf, shapeHigh, 1.7f);
    BlendColor(buf, shapeCol);

    // 4. Apply shape mask (clip to cape outline using base-N.png alpha)
    ApplyShapeMask(buf, shapeBase);

    // 4b. Threshold alpha: make the cape body fully opaque
    for (int i = 0; i < kWidth * kHeight; i++)
    {
        uint8_t& a = buf[i * 4 + 3];
        a = (a > 32) ? 255 : 0;
    }

    // 5. Overlay emblem using the lookup table
    int assetEmblem = EmblemGameToAsset(cape.emblem);
    if (assetEmblem >= 0)
    {
        RgbColor emblemCol = ColorFromId(cape.emblem_color);
        const RgbaImage* emblem = assets.GetEmblem(assetEmblem);
        OverlayColorized(buf, emblem, emblemCol, 24);
    }

    // 6. Overlay trim
    if (cape.trim > 0)
    {
        const RgbaImage* trim = assets.GetTrim(cape.trim, shape);
        OverlayTrim(buf, trim, cape.trim);
    }

    return buf;
}
