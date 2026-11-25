#include "pch.h"
#include "LibraryFonts.h"
#include "GuiGlobalConstants.h"
#include "RunLog.h"

#include <fstream>

using Microsoft::WRL::ComPtr;

namespace
{
ImFontAtlas* g_atlas = nullptr;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11ShaderResourceView> g_texture;

std::string g_regular, g_bold;          // the faces to bake
bool g_facesChanged = true;
float g_bakedScale = 0.0f;              // 0: nothing baked (or the last bake failed)
std::vector<ImWchar> g_bakedExtra;
bool g_bakeFailed = false;              // the same request failed: not retried every frame

struct Rung
{
    float css = 0.0f;
    bool bold = false;
    ImFont* font = nullptr;
};
std::vector<Rung> g_rungs;

// Each rung is a font of the atlas with only the space from stb_truetype, which sets up the
// font's metrics exactly as ImGui sizes it; every other character is a custom glyph Windows drew.
const ImWchar kSpaceOnly[] = { 0x0020, 0x0020, 0 };
const ImWchar kBaseRanges[] = {
    0x0021, 0x007E, 0x00A1, 0x00FF,
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2022, 0x2026, 0x2026, 0x2039, 0x203A,
    0,
};

struct PendingGlyph
{
    int rect = -1;
    int w = 0, h = 0;
    std::vector<uint32_t> pixels;   // RGBA, w * h
};

struct PendingFont
{
    ImFont* font = nullptr;
    float spaceAdvance = 0.0f;
};

// What a font file says about itself: the family GDI knows it by, its weight and slant, and the
// vertical metrics stb_truetype sizes it with (ImGui's size is the ascent plus the descent).
struct FaceInfo
{
    std::vector<uint8_t> data;   // the file, which the atlas reads the space and the metrics from
    std::wstring path;
    std::wstring family;
    int weight = 400;
    bool italic = false;
    int unitsPerEm = 0;
    int ascender = 0;
    int descender = 0;
};

uint16_t U16(const std::vector<uint8_t>& d, size_t at)
{
    return at + 2 <= d.size() ? static_cast<uint16_t>(d[at] << 8 | d[at + 1]) : 0;
}

uint32_t U32(const std::vector<uint8_t>& d, size_t at)
{
    return at + 4 <= d.size() ? (static_cast<uint32_t>(U16(d, at)) << 16 | U16(d, at + 2)) : 0;
}

std::wstring Widen(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool ReadFaceInfo(const std::string& path, FaceInfo& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    out = FaceInfo();
    out.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    const std::vector<uint8_t>& d = out.data;
    if (d.size() < 12)
        return false;
    size_t head = 0, hhea = 0, os2 = 0, name = 0;
    const int tables = U16(d, 4);
    for (int i = 0; i < tables; i++)
    {
        const size_t rec = 12 + static_cast<size_t>(i) * 16;
        const uint32_t tag = U32(d, rec);
        const size_t offset = U32(d, rec + 8);
        if (tag == 0x68656164) head = offset;        // 'head'
        else if (tag == 0x68686561) hhea = offset;   // 'hhea'
        else if (tag == 0x4F532F32) os2 = offset;    // 'OS/2'
        else if (tag == 0x6E616D65) name = offset;   // 'name'
    }
    if (!head || !hhea || !name)
        return false;
    out.path = Widen(path);
    out.unitsPerEm = U16(d, head + 18);
    out.ascender = static_cast<int16_t>(U16(d, hhea + 4));
    out.descender = static_cast<int16_t>(U16(d, hhea + 6));
    if (os2)
    {
        out.weight = U16(d, os2 + 4);
        out.italic = (U16(d, os2 + 62) & 1) != 0;
    }

    // The family (name 1) for Windows (platform 3), US English first.
    const int count = U16(d, name + 2);
    const size_t strings = name + U16(d, name + 4);
    int best = 0;
    for (int i = 0; i < count; i++)
    {
        const size_t rec = name + 6 + static_cast<size_t>(i) * 12;
        if (U16(d, rec) != 3 || U16(d, rec + 6) != 1)
            continue;
        const int encoding = U16(d, rec + 2);
        if (encoding != 1 && encoding != 0)
            continue;
        const int rank = U16(d, rec + 4) == 0x0409 ? 2 : 1;
        if (rank <= best)
            continue;
        const size_t at = strings + U16(d, rec + 10);
        const size_t len = U16(d, rec + 8);
        if (at + len > d.size())
            continue;
        out.family.clear();
        for (size_t j = 0; j + 1 < len; j += 2)
            out.family.push_back(static_cast<wchar_t>(d[at + j] << 8 | d[at + j + 1]));
        best = rank;
    }
    return !out.family.empty() && out.unitsPerEm > 0 && out.ascender > out.descender;
}

bool RegisterFace(const FaceInfo& face)
{
    static std::vector<std::wstring> registered;
    if (std::find(registered.begin(), registered.end(), face.path) != registered.end())
        return true;
    if (AddFontResourceExW(face.path.c_str(), FR_PRIVATE, nullptr) == 0)
        return false;
    registered.push_back(face.path);
    return true;
}

// A rung's characters, drawn white on black into a canvas in rows of fixed-height cells with room
// around each (one ExtTextOut per row, one flush per canvas); then each cut to the pixels it lit.
// The pen, the baseline and the advance are Windows' own, so a line built from these glyphs is the
// line TextOut draws.
constexpr int kCanvas = 1024;
constexpr int kPad = 4;

bool DrawGlyphs(ImFont* font, const FaceInfo& face, float px, const std::vector<wchar_t>& chars,
                std::vector<PendingGlyph>& out, float& spaceAdvance)
{
    const int em = std::max(1, static_cast<int>(std::lround(px * face.unitsPerEm / float(face.ascender - face.descender))));
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc)
        return false;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kCanvas;
    bmi.bmiHeader.biHeight = -kCanvas;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HFONT hf = CreateFontW(-em, 0, 0, 0, face.weight, face.italic, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.family.c_str());
    bool ok = bmp && bits && hf;
    HGDIOBJ oldBmp = ok ? SelectObject(dc, bmp) : nullptr;
    HGDIOBJ oldFont = ok ? SelectObject(dc, hf) : nullptr;
    TEXTMETRICW tm = {};
    if (ok)
    {
        wchar_t chosen[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, chosen);
        ok = _wcsicmp(chosen, face.family.c_str()) == 0 && GetTextMetricsW(dc, &tm);   // not a stand-in face
    }
    if (ok)
    {
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, TRANSPARENT);
        SetTextAlign(dc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
        uint32_t* canvas = static_cast<uint32_t*>(bits);
        memset(canvas, 0, sizeof(uint32_t) * kCanvas * kCanvas);

        // A cell: the font's ascent and descent, room above for accents on capitals, padding.
        const int headroom = em / 3 + kPad;
        const int cellH = headroom + tm.tmAscent + tm.tmDescent + kPad;
        const int baseline = headroom + tm.tmAscent;

        INT space = 0;
        if (GetCharWidth32W(dc, L' ', L' ', &space))
            spaceAdvance = static_cast<float>(space);

        std::vector<WORD> index(chars.size());
        GetGlyphIndicesW(dc, chars.data(), static_cast<int>(chars.size()), index.data(), GGI_MARK_NONEXISTING_GLYPHS);
        std::vector<ABC> abc(chars.size());
        for (size_t i = 0; i < chars.size();)
        {
            size_t j = i;
            while (j + 1 < chars.size() && chars[j + 1] == chars[j] + 1)
                j++;
            if (!GetCharABCWidthsW(dc, chars[i], chars[j], &abc[i]))
                for (size_t k = i; k <= j; k++)
                    index[k] = 0xFFFF;
            i = j + 1;
        }

        struct Cell
        {
            wchar_t ch;
            int x, y, w;
            int penX;
            int advance;
        };
        std::vector<Cell> cells;
        size_t rowFirst = 0;
        int cx = 0, rowY = 0;
        std::vector<INT> dx;
        auto drawRow = [&]()
        {
            const size_t n = cells.size() - rowFirst;
            if (n == 0)
                return;
            std::vector<wchar_t> text(n);
            dx.assign(n, 0);
            for (size_t k = 0; k < n; k++)
            {
                text[k] = cells[rowFirst + k].ch;
                if (k + 1 < n)
                    dx[k] = cells[rowFirst + k + 1].penX - cells[rowFirst + k].penX;
            }
            ExtTextOutW(dc, cells[rowFirst].penX, rowY + baseline, 0, nullptr, text.data(), static_cast<UINT>(n), dx.data());
            rowFirst = cells.size();
        };
        auto cut = [&]()
        {
            drawRow();
            GdiFlush();
            for (const Cell& c : cells)
            {
                int x0 = c.x + c.w, y0 = c.y + cellH, x1 = -1, y1 = -1;
                for (int y = c.y; y < c.y + cellH; y++)
                    for (int x = c.x; x < c.x + c.w; x++)
                        if (canvas[y * kCanvas + x] & 0x00FFFFFF)
                        {
                            x0 = std::min(x0, x); x1 = std::max(x1, x);
                            y0 = std::min(y0, y); y1 = std::max(y1, y);
                        }
                PendingGlyph g;
                ImVec2 offset(0.0f, 0.0f);
                if (x1 < 0)
                {
                    g.w = g.h = 1;   // a blank: only its advance matters
                    g.pixels.assign(1, 0);
                }
                else
                {
                    g.w = x1 - x0 + 1;
                    g.h = y1 - y0 + 1;
                    g.pixels.resize(static_cast<size_t>(g.w) * g.h);
                    for (int y = 0; y < g.h; y++)
                        for (int x = 0; x < g.w; x++)
                        {
                            const uint32_t v = canvas[(y0 + y) * kCanvas + x0 + x];
                            const uint32_t coverage = std::max((v >> 16) & 0xFF, std::max((v >> 8) & 0xFF, v & 0xFF));
                            g.pixels[static_cast<size_t>(y) * g.w + x] = 0x00FFFFFFu | coverage << 24;
                        }
                    // Against the pen and the baseline; the font's ascent is added after the build.
                    offset = ImVec2(static_cast<float>(x0 - c.penX), static_cast<float>(y0 - (c.y + baseline)));
                }
                g.rect = g_atlas->AddCustomRectFontGlyph(font, static_cast<ImWchar>(c.ch), g.w, g.h,
                                                         static_cast<float>(c.advance), offset);
                out.push_back(std::move(g));
            }
            memset(canvas, 0, sizeof(uint32_t) * kCanvas * std::min(kCanvas, rowY + cellH));
            cells.clear();
            rowFirst = 0;
            cx = rowY = 0;
        };

        for (size_t i = 0; i < chars.size(); i++)
        {
            if (index[i] == 0xFFFF)
                continue;
            const ABC& a = abc[i];
            const int w = std::max(1, static_cast<int>(a.abcB)) + kPad * 2;
            if (w > kCanvas || cellH > kCanvas)
                continue;
            if (cx + w > kCanvas)
            {
                drawRow();
                cx = 0;
                rowY += cellH;
            }
            if (rowY + cellH > kCanvas)
                cut();
            cells.push_back({ chars[i], cx, rowY, w, cx + kPad - a.abcA, a.abcA + static_cast<int>(a.abcB) + a.abcC });
            cx += w;
        }
        cut();
    }
    if (oldFont) SelectObject(dc, oldFont);
    if (oldBmp) SelectObject(dc, oldBmp);
    if (hf) DeleteObject(hf);
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc);
    return ok;
}

void Drop()
{
    LibraryFonts::Atlas()->Clear();
    g_texture.Reset();
    g_rungs.clear();
    g_bakedScale = 0.0f;
}
}

void LibraryFonts::SetDevice(ID3D11Device* device)
{
    g_device = device;
}

void LibraryFonts::SetFaces(const std::string& regular, const std::string& bold)
{
    if (regular == g_regular && bold == g_bold)
        return;
    g_regular = regular;
    g_bold = bold;
    g_facesChanged = true;
}

ImFontAtlas* LibraryFonts::Atlas()
{
    if (g_atlas == nullptr)
        g_atlas = IM_NEW(ImFontAtlas)();
    return g_atlas;
}

ImFont* LibraryFonts::Find(float css, bool bold)
{
    for (const Rung& r : g_rungs)
        if (r.bold == bold && std::fabs(r.css - css) < 0.01f)
            return r.font;
    return nullptr;
}

bool LibraryFonts::Bake(float scale, const std::vector<ImWchar16>& extra)
{
    const bool same = !g_facesChanged && std::fabs(scale - g_bakedScale) < 0.001f && extra == g_bakedExtra;
    if (same && g_bakedScale > 0.0f)
        return true;
    if (same && g_bakeFailed)
        return false;
    const auto start = std::chrono::steady_clock::now();
    const bool facesChanged = g_facesChanged;
    g_facesChanged = false;
    g_bakedExtra = extra;
    g_bakeFailed = true;
    Drop();

    static FaceInfo regular, bold;
    if (facesChanged)
    {
        regular = FaceInfo();
        bold = FaceInfo();
        if (!g_regular.empty() && ReadFaceInfo(g_regular, regular) && RegisterFace(regular))
        {
            if (g_bold.empty() || !ReadFaceInfo(g_bold, bold) || !RegisterFace(bold))
                bold = regular;
        }
        else
            regular = FaceInfo();
    }
    if (regular.data.empty() || !g_device)
        return false;

    std::vector<wchar_t> chars;
    for (const ImWchar* r = kBaseRanges; r[0]; r += 2)
        for (unsigned c = r[0]; c <= r[1]; c++)
            chars.push_back(static_cast<wchar_t>(c));
    for (const ImWchar c : extra)
        if (c >= 0x0100 && c < 0x0800)
            chars.push_back(static_cast<wchar_t>(c));

    ImFontAtlas* atlas = Atlas();
    std::vector<PendingGlyph> glyphs;
    std::vector<PendingFont> fonts;
    auto addRung = [&](float css, bool isBold)
    {
        FaceInfo& face = isBold ? bold : regular;
        // ImGui truncates a font's size to whole pixels (1.90), and the page draws each rung at its
        // own size: the rung is the nearest whole pixel, the glyphs Windows draws the exact size.
        const float exact = css * kLibraryEm * scale;
        ImFontConfig config;
        config.PixelSnapH = true;
        config.FontDataOwnedByAtlas = false;
        ImFont* font = atlas->AddFontFromMemoryTTF(face.data.data(), static_cast<int>(face.data.size()),
                                                   std::round(exact), &config, kSpaceOnly);
        if (font == nullptr)
            return;
        PendingFont pending;
        pending.font = font;
        if (!DrawGlyphs(font, face, exact, chars, glyphs, pending.spaceAdvance))
            return;
        fonts.push_back(pending);
        g_rungs.push_back({ css, isBold, font });
    };
    for (const float css : kLibraryPageSizes)
        addRung(css, false);
    for (const float css : kLibraryPageBoldSizes)
        addRung(css, true);

    if (g_rungs.empty() || !atlas->Build())
    {
        Drop();
        RunLog::Line("library-fonts: bake at scale %.2f failed; the page draws from the interface fonts", scale);
        return false;
    }

    // The glyphs were placed against the baseline; ImGui places them against the line's top.
    for (const PendingFont& f : fonts)
    {
        const float ascent = IM_ROUND(f.font->Ascent);
        for (ImFontGlyph& g : f.font->Glyphs)
            if (g.Codepoint != ' ')
            {
                g.Y0 += ascent;
                g.Y1 += ascent;
            }
            else if (f.spaceAdvance > 0.0f)
                g.AdvanceX = f.spaceAdvance;
        f.font->BuildLookupTable();
    }

    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    atlas->GetTexDataAsRGBA32(&pixels, &width, &height);
    bool uploaded = pixels != nullptr && width > 0 && height > 0;
    if (uploaded)
    {
        uint32_t* texels = reinterpret_cast<uint32_t*>(pixels);
        for (const PendingGlyph& g : glyphs)
        {
            const ImFontAtlasCustomRect* r = atlas->GetCustomRectByIndex(g.rect);
            if (r == nullptr || !r->IsPacked())
                continue;
            for (int y = 0; y < g.h; y++)
                memcpy(texels + static_cast<size_t>(r->Y + y) * width + r->X,
                       g.pixels.data() + static_cast<size_t>(y) * g.w, g.w * sizeof(uint32_t));
        }
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = pixels;
        data.SysMemPitch = desc.Width * 4;
        ComPtr<ID3D11Texture2D> texture;
        uploaded = SUCCEEDED(g_device->CreateTexture2D(&desc, &data, texture.GetAddressOf())) &&
                   SUCCEEDED(g_device->CreateShaderResourceView(texture.Get(), nullptr, g_texture.GetAddressOf()));
    }
    if (!uploaded)
    {
        Drop();
        RunLog::Line("library-fonts: texture upload at scale %.2f failed; the page draws from the interface fonts", scale);
        return false;
    }
    atlas->SetTexID(reinterpret_cast<ImTextureID>(g_texture.Get()));
    atlas->ClearTexData();   // the glyphs stay; the pixels are on the GPU now
    g_bakedScale = scale;
    g_bakeFailed = false;
    RunLog::Line("library-fonts: baked scale %.2f, %d sizes, %d characters, atlas %d x %d in %.0f ms", scale,
                 static_cast<int>(g_rungs.size()), static_cast<int>(chars.size()), width, height,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    return true;
}
