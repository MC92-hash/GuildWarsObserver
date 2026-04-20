#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include <unordered_map>
#include <string>
#include <DirectXTex.h>

struct GlyphInfo {
    float u0, v0, u1, v1;
    float widthPx, heightPx;
};

struct BitmapFont {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    std::unordered_map<char, GlyphInfo> glyphs;
    float refHeight = 23.f;
    bool loaded = false;

    bool Load(ID3D11Device* device, const wchar_t* ddsPath)
    {
        if (loaded) return srv.Get() != nullptr;

        loaded = true;
        InitGlyphTable();

        DirectX::ScratchImage image;
        HRESULT hr = DirectX::LoadFromDDSFile(ddsPath, DirectX::DDS_FLAGS_NONE, nullptr, image);
        if (FAILED(hr)) return false;

        const auto& meta = image.GetMetadata();
        if (meta.width == 0 || meta.height == 0) return false;

        DirectX::ScratchImage converted;
        if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            if (DirectX::IsCompressed(meta.format))
            {
                DirectX::ScratchImage decompressed;
                hr = DirectX::Decompress(*image.GetImages(), DXGI_FORMAT_R8G8B8A8_UNORM, decompressed);
                if (FAILED(hr)) return false;
                image = std::move(decompressed);
            }
            if (image.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM)
            {
                hr = DirectX::Convert(*image.GetImages(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                      DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
                if (FAILED(hr)) return false;
            }
        }
        const DirectX::Image* src = (converted.GetImageCount() > 0)
                                        ? converted.GetImages()
                                        : image.GetImages();

        D3D11_TEXTURE2D_DESC td = {};
        td.Width  = static_cast<UINT>(src->width);
        td.Height = static_cast<UINT>(src->height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage     = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem     = src->pixels;
        sd.SysMemPitch  = static_cast<UINT>(src->rowPitch);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        hr = device->CreateTexture2D(&td, &sd, &tex);
        if (FAILED(hr)) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = td.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf());
        return SUCCEEDED(hr);
    }

    float MeasureString(const char* str, float glyphHeight) const
    {
        float scale = glyphHeight / refHeight;
        float width = 0.f;
        float kerning = glyphHeight * -0.08f;
        for (const char* p = str; *p; ++p)
        {
            auto it = glyphs.find(*p);
            if (it == glyphs.end()) continue;
            if (width > 0.f) width += kerning;
            width += it->second.widthPx * scale;
        }
        return width;
    }

    void DrawString(ImDrawList* dl, const char* str, float cx, float cy,
                    float glyphHeight, uint8_t alpha) const
    {
        if (!srv.Get() || !dl || !str || !*str) return;

        ImTextureID texId = (ImTextureID)srv.Get();
        float scale = glyphHeight / refHeight;
        float kerning = glyphHeight * -0.08f;
        float totalW = MeasureString(str, glyphHeight);
        float curX = cx - totalW * 0.5f;
        ImU32 col = IM_COL32(255, 255, 255, alpha);

        for (const char* p = str; *p; ++p)
        {
            auto it = glyphs.find(*p);
            if (it == glyphs.end()) continue;

            const GlyphInfo& g = it->second;
            float gw = g.widthPx * scale;
            float gh = g.heightPx * scale;
            float yOff = (glyphHeight - gh) * 0.5f;

            if (curX > cx - totalW * 0.5f) curX += kerning;

            ImVec2 tl(curX, cy - glyphHeight * 0.5f + yOff);
            ImVec2 br(curX + gw, tl.y + gh);
            dl->AddImage(texId, tl, br, ImVec2(g.u0, g.v0), ImVec2(g.u1, g.v1), col);

            curX += gw;
        }
    }

private:
    void InitGlyphTable()
    {
        // 128x128 texture, 4x4 grid of 32px cells.
        // Tight bounding boxes measured from the alpha channel.
        struct RawGlyph { char ch; float u0, v0, u1, v1; float w, h; };
        static const RawGlyph kGlyphs[] = {
            { '0', 0.0547f, 0.0391f, 0.1953f, 0.2188f, 18, 23 },
            { '1', 0.3281f, 0.0312f, 0.4141f, 0.2188f, 11, 24 },
            { '2', 0.5625f, 0.0391f, 0.7031f, 0.2188f, 18, 23 },
            { '3', 0.8125f, 0.0391f, 0.9453f, 0.2188f, 17, 23 },
            { '4', 0.0469f, 0.2891f, 0.2031f, 0.4688f, 20, 23 },
            { '5', 0.3125f, 0.2891f, 0.4453f, 0.4688f, 17, 23 },
            { '6', 0.5547f, 0.2891f, 0.6953f, 0.4688f, 18, 23 },
            { '7', 0.8125f, 0.2891f, 0.9453f, 0.4688f, 17, 23 },
            { '8', 0.0547f, 0.5391f, 0.2031f, 0.7188f, 19, 23 },
            { '9', 0.3047f, 0.5391f, 0.4531f, 0.7188f, 19, 23 },
            { '-', 0.5703f, 0.6016f, 0.6797f, 0.6484f, 14,  6 },
            { '+', 0.8047f, 0.5547f, 0.9531f, 0.7031f, 19, 19 },
            { '!', 0.0938f, 0.7891f, 0.1484f, 0.9688f,  7, 23 },
        };

        glyphs.clear();
        for (const auto& g : kGlyphs)
            glyphs[g.ch] = { g.u0, g.v0, g.u1, g.v1, g.w, g.h };
    }
};
