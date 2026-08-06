#include "pch.h"
#include "ReplayWindow.h"
#include "GuiGlobalConstants.h"
#include "ReplayHotkeys.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Extracted from ReplayWindow.cpp (partial-class split). These remain
// ReplayWindow:: member functions; only their definitions live here.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Ribbon toolbar — full-width collapsible strip docked to the top edge.
//
// Collapsed it is a thin grip strip; it expands when the pointer reaches the
// top edge and collapses again after an idle delay, unless pinned. Every icon
// is a toggle bound directly to the panel's visibility flag, so the button
// renders the state rather than issuing a command.
// ---------------------------------------------------------------------------

namespace
{
    // Geometry, in unscaled pixels.
    constexpr float kCollapsedH   = 6.f;
    constexpr float kExpandedH    = 52.f;
    constexpr float kIconBox      = 28.f;   // hit target
    constexpr float kGlyph        = 18.f;   // drawn glyph extent
    constexpr float kIconGap      = 4.f;    // between icons in a group
    constexpr float kGroupGap     = 16.f;   // between groups
    constexpr float kCaptionPx    = 10.5f;
    constexpr float kEdgePad      = 12.f;
    constexpr float kEdgeReveal   = 8.f;    // pointer proximity that reveals
    constexpr float kIdleHideSec  = 2.5f;
    constexpr float kAnimRate     = 12.f;

    // Palette matched to the existing dark/gold replay chrome.
    constexpr ImU32 kBg        = IM_COL32( 14,  16,  20, 242);
    constexpr ImU32 kBgEdge    = IM_COL32(226, 178,  54,  46);
    constexpr ImU32 kGripCol   = IM_COL32(120, 128, 140, 170);
    constexpr ImU32 kDivider   = IM_COL32(255, 255, 255,  24);
    constexpr ImU32 kCaptionCol= IM_COL32(126, 134, 146, 255);
    constexpr ImU32 kIconOff   = IM_COL32(152, 160, 172, 255);
    constexpr ImU32 kIconHot   = IM_COL32(228, 233, 240, 255);
    constexpr ImU32 kIconOn    = IM_COL32(226, 178,  54, 255);
    constexpr ImU32 kFillOn    = IM_COL32(226, 178,  54,  40);
    constexpr ImU32 kBorderOn  = IM_COL32(226, 178,  54, 132);
    constexpr ImU32 kFillHot   = IM_COL32(255, 255, 255,  22);

    // Team colours taken from DrawAgentCylinders() so the icon matches the
    // cylinders actually drawn in the 3D view.
    constexpr ImU32 kCylRed  = IM_COL32(208,  72,  72, 255);
    constexpr ImU32 kCylBlue = IM_COL32( 74, 144, 216, 255);

    enum class Ico
    {
        None,
        Shield, Bubble, Lasers, Rings, CloudOff, Tag, Arrow,
        Morale, Crown, LogList, BarChart, Gradient, Piano,
        Camcorder, Eye, Frame, MapGrid,
        Party1, Party2, Clock,
        Brush, Bookmark, Doc, Speaker, More,
        Pin, Close
    };

    float IcoAlpha(ImU32 col)
    {
        return static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f;
    }

    // Draws a glyph centred on c, sized to fit a s x s box.
    void DrawIcon(ImDrawList* dl, Ico ico, ImVec2 c, float s, ImU32 col)
    {
        const float h  = s * 0.5f;
        const float th = std::max(1.f, s * 0.095f);
        auto P = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };

        switch (ico)
        {
        case Ico::None:
            break;
        case Ico::Shield:
        {
            ImVec2 p[5] = { P(-0.85f, -0.8f), P(0.85f, -0.8f), P(0.85f, 0.15f),
                            P(0.f, 0.9f),     P(-0.85f, 0.15f) };
            dl->AddPolyline(p, 5, col, ImDrawFlags_Closed, th);
            break;
        }
        case Ico::Bubble:
            dl->AddRect(P(-0.9f, -0.85f), P(0.9f, 0.3f), col, s * 0.18f, 0, th);
            dl->AddTriangleFilled(P(-0.5f, 0.25f), P(-0.1f, 0.25f), P(-0.45f, 0.9f), col);
            dl->AddLine(P(-0.55f, -0.45f), P(0.55f, -0.45f), col, th);
            dl->AddLine(P(-0.55f, -0.1f),  P(0.25f, -0.1f),  col, th);
            break;
        case Ico::Lasers:
            dl->AddLine(P(-0.9f, -0.4f), P(0.9f, -0.4f), col, th);
            dl->AddLine(P(0.9f, -0.4f),  P(0.35f, -0.8f), col, th);
            dl->AddLine(P(0.9f, -0.4f),  P(0.35f, 0.f),   col, th);
            dl->AddLine(P(0.9f, 0.45f),  P(-0.9f, 0.45f), col, th);
            dl->AddLine(P(-0.9f, 0.45f), P(-0.35f, 0.05f), col, th);
            dl->AddLine(P(-0.9f, 0.45f), P(-0.35f, 0.85f), col, th);
            break;
        case Ico::Rings:
            dl->AddCircle(c, h * 0.92f, col, 24, th);
            dl->AddCircle(c, h * 0.55f, col, 20, th);
            dl->AddCircleFilled(c, std::max(1.f, h * 0.16f), col, 10);
            break;
        case Ico::CloudOff:
        {
            // A solid cloud silhouette reads better than an outline at this size.
            dl->AddCircleFilled(P(-0.40f,  0.06f), h * 0.40f, col, 14);
            dl->AddCircleFilled(P( 0.02f, -0.28f), h * 0.52f, col, 16);
            dl->AddCircleFilled(P( 0.54f,  0.08f), h * 0.38f, col, 14);
            dl->AddRectFilled(P(-0.78f, 0.02f), P(0.74f, 0.46f), col, h * 0.22f);
            // The slash is carved out of the cloud so it stays legible against it.
            const int bga = static_cast<int>(IcoAlpha(col) * 255.f);
            dl->AddLine(P(-0.88f, 0.92f), P(0.88f, -0.92f),
                        IM_COL32(14, 16, 20, bga), th * 2.4f);
            dl->AddLine(P(-0.88f, 0.92f), P(0.88f, -0.92f), col, th * 1.15f);
            break;
        }
        case Ico::Tag:
        {
            ImVec2 p[5] = { P(-0.9f, -0.6f), P(0.35f, -0.6f), P(0.9f, 0.f),
                            P(0.35f, 0.6f),  P(-0.9f, 0.6f) };
            dl->AddPolyline(p, 5, col, ImDrawFlags_Closed, th);
            dl->AddCircleFilled(P(-0.45f, 0.f), std::max(1.f, h * 0.15f), col, 8);
            break;
        }
        case Ico::Arrow:
        {
            // Diagonal shaft with a filled head, matching the annotation the
            // tool actually draws on the map.
            const ImVec2 tail = P(-0.75f, 0.75f);
            const ImVec2 tip  = P(0.75f, -0.75f);
            dl->AddLine(tail, tip, col, th * 1.2f);
            dl->AddTriangleFilled(tip, P(0.05f, -0.62f), P(0.62f, -0.05f), col);
            break;
        }
        case Ico::Morale:
            dl->AddRect(P(-0.95f, -0.7f), P(0.95f, 0.7f), col, s * 0.16f, 0, th);
            dl->AddLine(P(0.f, 0.4f), P(0.f, -0.4f), col, th);
            dl->AddLine(P(0.f, -0.45f), P(-0.4f, -0.02f), col, th);
            dl->AddLine(P(0.f, -0.45f), P(0.4f, -0.02f),  col, th);
            break;
        case Ico::Crown:
        {
            ImVec2 p[7] = { P(-0.9f, 0.55f), P(-0.9f, -0.35f), P(-0.4f, 0.1f),
                            P(0.f, -0.7f),   P(0.4f, 0.1f),    P(0.9f, -0.35f),
                            P(0.9f, 0.55f) };
            dl->AddPolyline(p, 7, col, ImDrawFlags_Closed, th);
            break;
        }
        case Ico::LogList:
            for (int i = 0; i < 3; i++)
            {
                float y = -0.6f + i * 0.6f;
                dl->AddCircleFilled(P(-0.75f, y), std::max(1.f, h * 0.13f), col, 8);
                dl->AddLine(P(-0.35f, y), P(0.85f, y), col, th);
            }
            break;
        case Ico::BarChart:
        {
            const float hs[4] = { 0.25f, 0.7f, 0.45f, 0.95f };
            for (int i = 0; i < 4; i++)
            {
                float x = -0.78f + i * 0.52f;
                dl->AddRectFilled(P(x, 0.8f - hs[i] * 1.6f), P(x + 0.3f, 0.8f), col, 1.f);
            }
            break;
        }
        case Ico::Gradient:
        {
            int a = static_cast<int>(IcoAlpha(col) * 255.f);
            dl->AddRectFilledMultiColor(P(-0.9f, -0.9f), P(0.9f, 0.9f),
                                        IM_COL32( 60, 190, 120, a),
                                        IM_COL32(232, 200,  70, a),
                                        IM_COL32(226,  90,  60, a),
                                        IM_COL32( 70, 150, 200, a));
            break;
        }
        case Ico::Piano:
            dl->AddRect(P(-0.95f, -0.8f), P(0.95f, 0.8f), col, 1.f, 0, th);
            for (int i = 1; i < 4; i++)
                dl->AddLine(P(-0.95f + i * 0.475f, -0.8f), P(-0.95f + i * 0.475f, 0.8f), col, th * 0.8f);
            dl->AddRectFilled(P(-0.62f, -0.8f), P(-0.32f, 0.15f), col, 0.f);
            dl->AddRectFilled(P(0.32f, -0.8f),  P(0.62f, 0.15f),  col, 0.f);
            break;
        case Ico::Camcorder:
            dl->AddRect(P(-0.95f, -0.55f), P(0.25f, 0.55f), col, s * 0.14f, 0, th);
            dl->AddTriangleFilled(P(0.35f, 0.f), P(0.95f, -0.5f), P(0.95f, 0.5f), col);
            break;
        case Ico::Eye:
            dl->AddBezierQuadratic(P(-0.95f, 0.f), P(0.f, -1.f), P(0.95f, 0.f), col, th, 14);
            dl->AddBezierQuadratic(P(-0.95f, 0.f), P(0.f, 1.f),  P(0.95f, 0.f), col, th, 14);
            dl->AddCircleFilled(c, h * 0.28f, col, 12);
            break;
        case Ico::Frame:
            dl->AddRect(P(-0.95f, -0.8f), P(0.95f, 0.8f), col, s * 0.1f, 0, th);
            dl->AddRectFilled(P(0.05f, 0.f), P(0.75f, 0.55f), col, 1.f);
            break;
        case Ico::MapGrid:
            dl->AddRect(P(-0.95f, -0.75f), P(0.95f, 0.75f), col, 1.f, 0, th);
            dl->AddLine(P(-0.32f, -0.75f), P(-0.32f, 0.75f), col, th * 0.85f);
            dl->AddLine(P(0.32f, -0.75f),  P(0.32f, 0.75f),  col, th * 0.85f);
            break;
        case Ico::Party1:
        case Ico::Party2:
        {
            // Two overlapping figures, plus a numbered badge.
            auto figure = [&](float cx, float scale)
            {
                dl->AddCircle(P(cx, -0.42f * scale), h * 0.26f * scale, col, 12, th);
                dl->AddBezierQuadratic(P(cx - 0.5f * scale, 0.75f * scale),
                                       P(cx, -0.15f * scale),
                                       P(cx + 0.5f * scale, 0.75f * scale), col, th, 12);
            };
            figure(-0.35f, 0.95f);
            figure(0.42f, 0.8f);
            ImFont* f = ImGui::GetFont();
            const char* d = (ico == Ico::Party1) ? "1" : "2";
            float fs = s * 0.5f;
            ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.f, d);
            ImVec2 bc = P(-0.62f, 0.62f);
            dl->AddCircleFilled(bc, fs * 0.62f, IM_COL32(14, 16, 20, 255), 12);
            dl->AddCircle(bc, fs * 0.62f, col, 12, th * 0.8f);
            dl->AddText(f, fs, ImVec2(bc.x - ts.x * 0.5f, bc.y - ts.y * 0.5f), col, d);
            break;
        }
        case Ico::Clock:
            dl->AddCircle(P(0.1f, -0.15f), h * 0.72f, col, 22, th);
            dl->AddLine(P(0.1f, -0.15f), P(0.1f, -0.62f), col, th);
            dl->AddLine(P(0.1f, -0.15f), P(0.52f, -0.15f), col, th);
            dl->AddLine(P(-0.95f, 0.82f), P(0.95f, 0.82f), col, th * 0.85f);
            dl->AddCircleFilled(P(-0.35f, 0.82f), std::max(1.f, h * 0.16f), col, 8);
            break;
        case Ico::Brush:
        {
            // Laid out along a diagonal axis: t runs from the handle end (+1) down
            // to the bristle tip (-1), w offsets perpendicular to it. The head
            // flares out to an angled chisel tip, which is what separates a brush
            // from a chisel or a pencil at this size.
            auto AX = [&](float t, float w)
            {
                return P(0.80f * t + 0.7071f * w, -0.80f * t + 0.7071f * w);
            };

            ImVec2 handle[4] = { AX(1.00f, 0.065f), AX(1.00f, -0.065f),
                                 AX(0.24f, -0.115f), AX(0.24f, 0.115f) };
            dl->AddConvexPolyFilled(handle, 4, col);

            ImVec2 ferrule[4] = { AX(0.26f, 0.17f), AX(0.26f, -0.17f),
                                  AX(0.06f, -0.19f), AX(0.06f, 0.19f) };
            dl->AddConvexPolyFilled(ferrule, 4, col);

            ImVec2 head[4] = { AX(0.05f, 0.20f), AX(0.05f, -0.20f),
                               AX(-0.64f, -0.28f), AX(-0.54f, 0.30f) };
            dl->AddConvexPolyFilled(head, 4, col);

            // Carved separator so the band and the bristles stay distinct in a
            // single colour.
            const int bga = static_cast<int>(IcoAlpha(col) * 255.f);
            dl->AddLine(AX(0.05f, 0.21f), AX(0.05f, -0.21f),
                        IM_COL32(14, 16, 20, bga), th * 0.55f);
            break;
        }
        case Ico::Bookmark:
        {
            ImVec2 p[5] = { P(-0.6f, -0.9f), P(0.6f, -0.9f), P(0.6f, 0.9f),
                            P(0.f, 0.4f),    P(-0.6f, 0.9f) };
            dl->AddPolyline(p, 5, col, ImDrawFlags_Closed, th);
            break;
        }
        case Ico::Doc:
        {
            ImVec2 p[5] = { P(-0.72f, -0.9f), P(0.3f, -0.9f), P(0.72f, -0.45f),
                            P(0.72f, 0.9f),   P(-0.72f, 0.9f) };
            dl->AddPolyline(p, 5, col, ImDrawFlags_Closed, th);
            dl->AddLine(P(-0.4f, 0.1f), P(0.4f, 0.1f), col, th * 0.85f);
            dl->AddLine(P(-0.4f, 0.5f), P(0.4f, 0.5f), col, th * 0.85f);
            break;
        }
        case Ico::Speaker:
        {
            ImVec2 p[5] = { P(-0.9f, -0.3f), P(-0.45f, -0.3f), P(0.f, -0.85f),
                            P(0.f, 0.85f),   P(-0.45f, 0.3f) };
            ImVec2 q[6] = { p[0], p[1], p[2], p[3], p[4], P(-0.9f, 0.3f) };
            dl->AddPolyline(q, 6, col, ImDrawFlags_Closed, th);
            dl->AddBezierQuadratic(P(0.35f, -0.5f), P(0.7f, 0.f), P(0.35f, 0.5f), col, th, 10);
            dl->AddBezierQuadratic(P(0.6f, -0.8f),  P(1.05f, 0.f), P(0.6f, 0.8f), col, th, 10);
            break;
        }
        case Ico::More:
            for (int i = -1; i <= 1; i++)
                dl->AddCircleFilled(P(i * 0.6f, 0.f), std::max(1.5f, h * 0.16f), col, 10);
            break;
        case Ico::Pin:
            dl->AddCircle(P(0.f, -0.35f), h * 0.45f, col, 14, th);
            dl->AddLine(P(0.f, 0.1f), P(0.f, 0.9f), col, th);
            break;
        case Ico::Close:
            dl->AddLine(P(-0.7f, -0.7f), P(0.7f, 0.7f), col, th);
            dl->AddLine(P(-0.7f, 0.7f),  P(0.7f, -0.7f), col, th);
            break;
        }
    }

    // Resolves a path relative to the Textures directory by walking up from the
    // exe, matching how the loading screen and flag icons locate their art.
    std::string TexturePath(const char* relative)
    {
        static std::string base;
        static bool searched = false;
        if (!searched)
        {
            searched = true;
            wchar_t exePath[MAX_PATH];
            if (GetModuleFileNameW(nullptr, exePath, MAX_PATH))
            {
                auto dir = std::filesystem::path(exePath).parent_path();
                for (int i = 0; i < 6; i++)
                {
                    auto cand = dir / "Textures";
                    if (std::filesystem::exists(cand)) { base = cand.string(); break; }
                    if (!dir.has_parent_path() || dir == dir.parent_path()) break;
                    dir = dir.parent_path();
                }
            }
        }
        if (base.empty()) return {};
        return base + "\\" + relative;
    }

    struct RibbonTex
    {
        ImTextureID id = nullptr;
        float       w  = 0.f;
        float       h  = 0.f;
    };

    enum class TexXform
    {
        None,
        Grey,       // desaturated, for toggles that grey out when off
        WhiteMask   // line art on an opaque white page -> tintable alpha mask
    };

    // The art ranges from 16 px atlas cells to 512 px icons, all drawn at roughly
    // 22 px. The shared texture cache uploads only the top mip, which aliases badly
    // at that reduction, so this loader builds a real mip chain. Each transform is
    // cached separately from the untouched image.
    RibbonTex RibbonIconTex(ID3D11Device* device, const char* relative, TexXform xform)
    {
        struct Entry
        {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
            float w = 0.f, h = 0.f;
        };
        static ID3D11Device* cachedDev = nullptr;
        static std::unordered_map<std::string, Entry> cache;
        if (device != cachedDev) { cache.clear(); cachedDev = device; }

        const std::string key = std::string(relative)
            + (xform == TexXform::Grey ? "#g" : xform == TexXform::WhiteMask ? "#m" : "");
        if (auto it = cache.find(key); it != cache.end())
            return { (ImTextureID)it->second.srv.Get(), it->second.w, it->second.h };

        cache[key] = Entry{};   // negative-cache; overwritten only on success

        auto path = TexturePath(relative);
        if (path.empty() || !std::filesystem::exists(path)) return {};
        std::filesystem::path fp(path);

        DirectX::ScratchImage loaded;
        HRESULT hr;
        if (auto ext = fp.extension(); ext == L".dds" || ext == L".DDS")
            hr = DirectX::LoadFromDDSFile(fp.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, loaded);
        else
            hr = DirectX::LoadFromWICFile(fp.c_str(), DirectX::WIC_FLAGS_IGNORE_SRGB, nullptr, loaded);
        if (FAILED(hr)) return {};
        if (loaded.GetMetadata().width == 0) return {};

        DirectX::ScratchImage conv;
        const bool needConv = loaded.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM;
        if (needConv && FAILED(DirectX::Convert(*loaded.GetImage(0, 0, 0),
                                               DXGI_FORMAT_R8G8B8A8_UNORM,
                                               DirectX::TEX_FILTER_DEFAULT,
                                               DirectX::TEX_THRESHOLD_DEFAULT, conv)))
            return {};
        DirectX::ScratchImage& srcImg = needConv ? conv : loaded;

        if (xform != TexXform::None)
        {
            const DirectX::Image* im = srcImg.GetImage(0, 0, 0);
            for (size_t y = 0; y < im->height; y++)
            {
                uint8_t* row = im->pixels + y * im->rowPitch;
                for (size_t x = 0; x < im->width; x++)
                {
                    uint8_t* px = row + x * 4;
                    if (xform == TexXform::Grey)
                    {
                        const auto l = static_cast<uint8_t>(px[0] * 0.299f + px[1] * 0.587f
                                                            + px[2] * 0.114f);
                        px[0] = px[1] = px[2] = l;
                    }
                    else
                    {
                        // The page becomes transparent and the strokes survive as
                        // coverage, leaving a mask the state colour can tint.
                        px[3] = static_cast<uint8_t>(255 - std::min({ px[0], px[1], px[2] }));
                        px[0] = px[1] = px[2] = 255;
                    }
                }
            }
        }

        DirectX::ScratchImage mips;
        bool haveMips = SUCCEEDED(DirectX::GenerateMipMaps(*srcImg.GetImage(0, 0, 0),
                                                          DirectX::TEX_FILTER_DEFAULT, 0, mips));
        const DirectX::ScratchImage& chain = haveMips ? mips : srcImg;

        const size_t levels = haveMips ? chain.GetImageCount() : 1;
        std::vector<D3D11_SUBRESOURCE_DATA> sub(levels);
        for (size_t i = 0; i < levels; i++)
        {
            const auto* im = chain.GetImage(i, 0, 0);
            sub[i].pSysMem          = im->pixels;
            sub[i].SysMemPitch      = static_cast<UINT>(im->rowPitch);
            sub[i].SysMemSlicePitch = 0;
        }

        const auto* top = chain.GetImage(0, 0, 0);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width      = static_cast<UINT>(top->width);
        td.Height     = static_cast<UINT>(top->height);
        td.MipLevels  = static_cast<UINT>(levels);
        td.ArraySize  = 1;
        td.Format     = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage      = D3D11_USAGE_DEFAULT;
        td.BindFlags  = D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        if (FAILED(device->CreateTexture2D(&td, sub.data(), tex.GetAddressOf())))
            return {};

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                    = td.Format;
        sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels       = static_cast<UINT>(levels);

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device->CreateShaderResourceView(tex.Get(), &sd, srv.GetAddressOf())))
            return {};

        const float tw = static_cast<float>(top->width), thh = static_cast<float>(top->height);
        cache[key] = Entry{ srv, tw, thh };
        return { (ImTextureID)srv.Get(), tw, thh };
    }

    ImU32 Shade(ImU32 c, float mul, float alpha)
    {
        auto ch = [&](int shift) {
            return static_cast<int>(std::min(255.f, ((c >> shift) & 0xFF) * mul));
        };
        int a = static_cast<int>(((c >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), a);
    }

    void FillEllipse(ImDrawList* dl, ImVec2 c, float rx, float ry, ImU32 col)
    {
        constexpr int kSeg = 20;
        ImVec2 pts[kSeg];
        for (int i = 0; i < kSeg; i++)
        {
            float a = (static_cast<float>(i) / kSeg) * 6.2831853f;
            pts[i] = ImVec2(c.x + std::cos(a) * rx, c.y + std::sin(a) * ry);
        }
        dl->AddConvexPolyFilled(pts, kSeg, col);
    }

    // One capped tube, shaded like the cylinder pixel shader: a vertical ramp up
    // the body and a brighter elliptical top cap.
    void DrawCylinder(ImDrawList* dl, ImVec2 topC, float rx, float ry, float bodyH,
                      ImU32 base, float alpha)
    {
        const ImU32 cTop = Shade(base, 1.00f, alpha);
        const ImU32 cBot = Shade(base, 0.55f, alpha);
        const ImU32 cCap = Shade(base, 1.30f, alpha);
        const ImVec2 botC(topC.x, topC.y + bodyH);

        FillEllipse(dl, botC, rx, ry, cBot);
        dl->AddRectFilledMultiColor(ImVec2(topC.x - rx, topC.y), ImVec2(topC.x + rx, botC.y),
                                    cTop, cTop, cBot, cBot);
        FillEllipse(dl, topC, rx, ry, cCap);
    }

    // Range rings carry meaning in their colour, so they are drawn in their own
    // palette instead of the state colour and only dim when the toggle is off.
    void DrawRingsGlyph(ImDrawList* dl, ImVec2 c, float s, float alpha, bool on)
    {
        const float h  = s * 0.5f;
        const float th = std::max(1.f, s * 0.10f);
        const float m  = on ? 1.f : 0.62f;
        dl->AddCircle(c, h * 0.95f, Shade(IM_COL32( 92, 200, 112, 255), m, alpha), 28, th);
        dl->AddCircle(c, h * 0.60f, Shade(IM_COL32(236, 176,  56, 255), m, alpha), 22, th);
        dl->AddCircle(c, h * 0.26f, Shade(IM_COL32(220,  78,  70, 255), m, alpha), 16, th);
    }

    // The legacy representation: one cylinder per team, offset in depth.
    void DrawTeamCylinders(ImDrawList* dl, ImVec2 c, float s, float alpha)
    {
        const float h = s * 0.5f;
        auto P = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };
        const float rx = 0.34f * h, ry = 0.14f * h, body = 1.05f * h;

        DrawCylinder(dl, P(-0.42f, -0.62f), rx, ry, body, kCylRed,  alpha);
        DrawCylinder(dl, P( 0.42f, -0.34f), rx, ry, body, kCylBlue, alpha);
    }

    // A texture-backed glyph. Most of these files are atlas sheets or carry
    // transparent padding, so the source region is given as a pixel rect rather
    // than assuming the whole image is the icon.
    struct TexIcon
    {
        const char* file  = nullptr;
        ImVec4      rect  = ImVec4(0.f, 0.f, 0.f, 0.f);   // x0,y0,x1,y1; zero = all
        float       scale = 1.20f;                        // extent vs stroked glyphs

        // Several sheets ship a lit variant of each icon; when given, the art
        // itself carries the on state and no dimming is applied.
        ImVec4      rectOn = ImVec4(0.f, 0.f, 0.f, 0.f);

        // Separate off-state file (e.g. SpeakerON / SpeakerOFF). When set, the
        // art itself carries the state and no dimming is applied.
        const char* fileOff = nullptr;
        ImVec4      rectOff = ImVec4(0.f, 0.f, 0.f, 0.f);

        bool greyWhenOff  = false;   // desaturate when off instead of dimming
        bool whiteToAlpha = false;   // line art on white -> alpha mask
        bool tintAsGlyph  = false;   // colour it like a stroked glyph
    };

    // Returns false when the art is missing so the caller can fall back to a glyph.
    bool DrawTexIcon(ImDrawList* dl, ID3D11Device* dev, const TexIcon& t, ImVec2 c,
                     float s, float alpha, bool on, bool hovered, ImU32 glyphCol)
    {
        const bool  dualFile = t.fileOff != nullptr;
        const char* path     = (!on && dualFile) ? t.fileOff : t.file;
        const TexXform xf = t.whiteToAlpha            ? TexXform::WhiteMask
                          : (t.greyWhenOff && !on)    ? TexXform::Grey
                                                      : TexXform::None;
        RibbonTex rt = RibbonIconTex(dev, path, xf);
        if (!rt.id) return false;

        const bool  stateArt = dualFile || t.rectOn.z > t.rectOn.x;
        const ImVec4& src = (!on && dualFile)            ? t.rectOff
                          : (stateArt && on && !dualFile) ? t.rectOn
                                                          : t.rect;

        float x0 = src.x, y0 = src.y, x1 = src.z, y1 = src.w;
        if (x1 <= x0 || y1 <= y0) { x0 = 0.f; y0 = 0.f; x1 = rt.w; y1 = rt.h; }

        // Fit the source region into the glyph box without distorting it; the
        // atlas cells range from tall (a book) to wide (the heatmap swatch).
        const float sw = x1 - x0, sh = y1 - y0;
        float rx = s * 0.5f * t.scale, ry = rx;
        if (sw > sh)      ry = rx * sh / sw;
        else if (sh > sw) rx = ry * sw / sh;

        // Inactive art is darkened rather than recoloured, which keeps it readable
        // while still reading as off next to the lit-up active buttons. Art that is
        // already desaturated only needs a light touch, or it goes nearly black,
        // and art with its own lit variant needs none at all.
        ImU32 tint;
        if (t.tintAsGlyph)
        {
            tint = glyphCol;
        }
        else
        {
            const int lum = (stateArt || on) ? 255
                          : hovered          ? 235
                          : xf == TexXform::Grey ? 205 : 160;
            tint = IM_COL32(lum, lum, lum, static_cast<int>(255 * alpha));
        }
        dl->AddImage(rt.id, ImVec2(c.x - rx, c.y - ry), ImVec2(c.x + rx, c.y + ry),
                     ImVec2(x0 / rt.w, y0 / rt.h), ImVec2(x1 / rt.w, y1 / rt.h), tint);
        return true;
    }

    struct Item
    {
        Ico                   ico;
        const char*           name;
        std::function<bool()> get;
        std::function<void()> toggle;
        bool                  isMore = false;

        // For toggles that switch between two representations rather than simply
        // hiding something, the glyph and tooltip follow the state so the button
        // always shows what is currently on screen.
        Ico                   icoOff  = Ico::None;
        const char*           nameOff = nullptr;

        // Set for icons that are not a single tinted glyph (multi-colour art, or
        // art that differs per state). Receives the glyph centre, extent, current
        // state and fade alpha.
        std::function<void(ImDrawList*, ImVec2, float, bool, float)> customDraw;

        // Game art to use in place of the stroked glyph; ico stays as the fallback.
        TexIcon tex{};

        // Replaces the state colour for glyphs whose colour is itself meaningful.
        ImU32 glyphTint = 0;

        // Hover helper: gold title (+ optional shortcut), then a short description.
        const char* desc     = nullptr;
        int         shortcut = 0;   // ImGuiKey, or 0 when unbound
    };

    struct Group
    {
        const char*       caption;
        std::vector<Item> items;
    };

    // Bold title (light cold), shortcut in warm cream (255,238,187), body muted.
    // ASCII-only body text: the UI fonts miss several Unicode dash glyphs.
    void DrawRibbonTooltip(ImFont* titleBold, const char* title, const char* desc, int shortcut)
    {
        if (!ImGui::BeginTooltip()) return;

        if (titleBold) ImGui::PushFont(titleBold);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 0.85f));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        if (shortcut > 0)
        {
            ImGui::SameLine(0.f, 4.f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(255.f / 255.f, 238.f / 255.f, 187.f / 255.f, 1.f));
            ImGui::Text("(%s)", ImGui::GetKeyName(static_cast<ImGuiKey>(shortcut)));
            ImGui::PopStyleColor();
        }
        if (titleBold) ImGui::PopFont();

        if (desc && desc[0])
        {
            // Slightly smaller than the title/body font so a long description
            // doesn't read as loud as the button name above it.
            ImGui::SetWindowFontScale(0.88f);
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.81f, 0.86f, 1.f));
            ImGui::TextUnformatted(desc);
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::SetWindowFontScale(1.f);
        }

        ImGui::EndTooltip();
    }
}

// Exposed for AnnotationManager's drawing strip; see ReplayWindow_Internal.h.
ImTextureID LoadRibbonArt(ID3D11Device* device, const char* relative,
                          float* outW, float* outH)
{
    RibbonTex rt = RibbonIconTex(device, relative, TexXform::None);
    if (outW) *outW = rt.w;
    if (outH) *outH = rt.h;
    return rt.id;
}

void ReplayWindow::DrawRibbonToolbar()
{
    if (m_ribbonClosed)
    {
        // Nothing occupies the top edge, so HUD elements can sit flush against it.
        m_ribbonBottomY = ImGui::GetMainViewport()->WorkPos.y;
        return;
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiIO&       io = ImGui::GetIO();

    // WorkPos already excludes the main menu bar, so the ribbon tucks directly
    // underneath it rather than overlapping the menus.
    const ImVec2 origin = vp->WorkPos;
    const float  width  = vp->WorkSize.x;
    const float  dt     = io.DeltaTime;

    const float curH = kCollapsedH + (kExpandedH - kCollapsedH) * m_ribbonReveal;

    const bool mouseInX  = io.MousePos.x >= origin.x && io.MousePos.x <= origin.x + width;
    const bool atEdge    = mouseInX && io.MousePos.y >= origin.y - 2.f
                                    && io.MousePos.y <= origin.y + kEdgeReveal;
    const bool overStrip = mouseInX && io.MousePos.y >= origin.y
                                    && io.MousePos.y <= origin.y + curH + 2.f;

    if (m_ribbonPinned || atEdge || overStrip || m_ribbonMoreOpen)
        m_ribbonIdleTimer = 0.f;
    else
        m_ribbonIdleTimer += dt;

    const float target = (m_ribbonPinned || m_ribbonIdleTimer < kIdleHideSec) ? 1.f : 0.f;
    m_ribbonReveal += (target - m_ribbonReveal) * std::min(1.f, dt * kAnimRate);
    if (std::abs(target - m_ribbonReveal) < 0.002f)
        m_ribbonReveal = target;

    const float h = kCollapsedH + (kExpandedH - kCollapsedH) * m_ribbonReveal;
    m_ribbonBottomY = origin.y + h;

    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(ImVec2(width, h));

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar        | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove            | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings   | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNavFocus        | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    const bool visible = ImGui::Begin("##ReplayRibbon", nullptr, kFlags);
    ImGui::PopStyleVar(2);

    if (!visible)
    {
        ImGui::End();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + h), kBg,
                      0.f, ImDrawFlags_RoundCornersNone);
    dl->AddLine(ImVec2(origin.x, origin.y + h - 1.f),
                ImVec2(origin.x + width, origin.y + h - 1.f), kBgEdge, 1.f);

    // Collapsed: a centred grip hints that the strip can be revealed.
    if (m_ribbonReveal < 0.5f)
    {
        const float gw = 34.f, gh = 2.f;
        float a = 1.f - m_ribbonReveal * 2.f;
        ImU32 g = IM_COL32(120, 128, 140, static_cast<int>(170 * a));
        float cy = origin.y + kCollapsedH * 0.5f;
        dl->AddRectFilled(ImVec2(origin.x + width * 0.5f - gw * 0.5f, cy - gh * 0.5f),
                          ImVec2(origin.x + width * 0.5f + gw * 0.5f, cy + gh * 0.5f),
                          g, gh * 0.5f);
    }

    // Content fades in slightly ahead of the height so it never looks squashed.
    const float contentA = std::clamp((m_ribbonReveal - 0.35f) / 0.65f, 0.f, 1.f);
    const bool  interact = m_ribbonReveal > 0.9f;

    if (contentA > 0.01f)
    {
        auto fade = [&](ImU32 c)
        {
            int a = static_cast<int>(((c >> IM_COL32_A_SHIFT) & 0xFF) * contentA);
            return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
        };

        const auto& hk = ReplayHotkeys::Get();

        auto B = [](Ico ic, const char* n, bool& f, const char* desc, int sc = 0) -> Item
        {
            Item it{ ic, n, [&f] { return f; }, [&f] { f = !f; }, false };
            it.desc = desc;
            it.shortcut = sc;
            return it;
        };

        // Same as B, but drawn from game art with the glyph kept as a fallback.
        auto T = [](Ico fallback, const char* n, bool& f, TexIcon t,
                    const char* desc, int sc = 0) -> Item
        {
            Item it{ fallback, n, [&f] { return f; }, [&f] { f = !f; }, false };
            it.tex = t;
            it.desc = desc;
            it.shortcut = sc;
            return it;
        };

        // Same as B, but with the glyph drawn in a fixed colour.
        auto C = [](Ico ic, const char* n, bool& f, ImU32 tint,
                    const char* desc, int sc = 0) -> Item
        {
            Item it{ ic, n, [&f] { return f; }, [&f] { f = !f; }, false };
            it.glyphTint = tint;
            it.desc = desc;
            it.shortcut = sc;
            return it;
        };

        std::vector<Group> groups;
        groups.push_back({ "Overlays", {
            // Models load progressively via ProgressiveAgentModelPump(), so this
            // only flips the flag; turning it off hides the meshes so the
            // team-coloured cylinders show through again.
            Item{ Ico::Shield, "Agent Models",
                  [this] { return m_useAgentModels; },
                  [this] {
                      m_useAgentModels = !m_useAgentModels;
                      GuiGlobalConstants::use_3d_agent_models = m_useAgentModels;
                      if (!m_useAgentModels && m_agentModelsLoaded)
                      {
                          auto* meshMgr = m_mapRenderer->GetMeshManager();
                          for (auto& [agentId, meshIds] : m_agentMeshIds)
                              for (int mid : meshIds)
                                  meshMgr->SetMeshShouldRender(mid, false);
                      }
                  },
                  false, Ico::None, nullptr,
                  [this](ImDrawList* d, ImVec2 c, float s, bool on, float a)
                  {
                      if (!on) { DrawTeamCylinders(d, c, s, a); return; }

                      // The rect trims the helm's transparent margin so it centres.
                      TexIcon helm{ "Others_UI\\texture_293034.dds",
                                    ImVec4(47.f, 9.f, 466.f, 512.f), 1.24f };
                      if (!DrawTexIcon(d, m_deviceResources->GetD3DDevice(), helm,
                                       c, s, a, true, false, 0))
                          DrawIcon(d, Ico::Shield, c, s, Shade(kIconOn, 1.f, a));
                  }, {}, 0,
                  "Switch between animated textured 3D player models and the "
                  "classic team-coloured cylinders." },
            T(Ico::Bubble, "Skill Icons", m_showSkillIcons,
              TexIcon{ "Toolbar\\[3] - Signet of Capture.jpg", ImVec4(0, 0, 0, 0),
                       1.16f, ImVec4(0, 0, 0, 0), nullptr, ImVec4(0, 0, 0, 0), true },
              "Show skill icons near agents when they cast."),
            // Lit by whether lasers are actually being drawn, not by whether the
            // filter panel happens to be open - the button reports what is on
            // screen, the same as every other Overlays toggle. Clicking it still
            // opens the panel, which is where the master switch lives.
            Item{ Ico::Lasers, "Skill Lasers",
                  [this] { return m_showSkillLasers; },
                  [this] { m_showLaserPanel = !m_showLaserPanel; },
                  false, Ico::None, nullptr, nullptr,
                  TexIcon{ "Toolbar\\ui_template_actions.png",
                           ImVec4(27, 23, 44, 46), 1.26f },
                  0,
                  "Open the skill laser panel. Lasers draw a line from each "
                  "caster to the target of their skill. The panel filters which "
                  "lasers are drawn:\n"
                  "- By profession, so you can watch only the Monks or only the "
                  "Mesmers\n"
                  "- By team, red and/or blue\n"
                  "- Per player, ticking individual casters on or off\n"
                  "It also holds the master switch that turns the lasers "
                  "themselves on or off.",
                  hk.toggleSkillLasers },
            Item{ Ico::Rings, "Range Rings",
                  [this] { return m_showRangeRings; },
                  [this] { m_showRangeRings = !m_showRangeRings; },
                  false, Ico::None, nullptr,
                  [](ImDrawList* d, ImVec2 c, float s, bool on, float a)
                  { DrawRingsGlyph(d, c, s, a, on); }, {}, 0,
                  "Draw range rings around the followed agent:\n"
                  "- Touch (144)\n"
                  "- Adjacent (166)\n"
                  "- Nearby (240)\n"
                  "- In Area (322)\n"
                  "- Earshot (1000)\n"
                  "- Cast Range (1248)\n"
                  "- Passive (2512)\n"
                  "- Compass (5020)",
                  hk.toggleRangeRings },
            Item{ Ico::CloudOff, "Fog of War",
                  [this] { return m_fogPerspective > 0; },
                  [this] {
                      if (m_fogPerspective > 0) {
                          m_fogLastActive = m_fogPerspective;
                          m_fogPerspective = 0;
                          m_fogPlayerAgent = -1;
                      } else {
                          m_fogPerspective = m_fogLastActive;
                      }
                  },
                  false, Ico::None, nullptr, nullptr,
                  TexIcon{ "Toolbar\\FogON.png", ImVec4(12, 14, 49, 44), 1.18f,
                           ImVec4(0, 0, 0, 0), "Toolbar\\FogOFF.png",
                           ImVec4(6, 6, 51, 51) },
                  0,
                  "Limit visibility to a single player, multiple selected "
                  "players, or an entire team - hiding agents outside their "
                  "fog of war.",
                  hk.toggleFogOfWar },
            B(Ico::Tag, "Agent Names", m_showNameFilterPanel,
              "Open the agent name filter to show or hide name labels."),
        } });

        groups.push_back({ "Combat & stats", {
            T(Ico::Morale, "Morale", m_showMoralePanel,
              TexIcon{ "Toolbar\\Morale_Boost.jpg", ImVec4(0, 0, 0, 0), 1.16f },
              "Open the per-player morale panel. Morale is tracked live for "
              "each player from these rules:\n"
              "- -15% per death, down to a floor of -60%\n"
              "- +10% while a team-wide morale boost is active\n"
              "- +2% recovered toward 0% each time an enemy dies, while you "
              "are alive and your own morale is below 0%\n"
              "Deaths counted right after a resurrection, or caused by Death "
              "Pact Signet, are ignored and do not reduce morale.",
              hk.toggleMoralePanel),
            T(Ico::Crown, "Lord Damage", m_showLordDamagePanel,
              TexIcon{ "Others_UI\\redguildlord2.png", ImVec4(0, 0, 0, 0), 1.14f },
              "Open the Guild Lord damage panel, showing how much damage "
              "each player dealt to the enemy Lord over the match:\n"
              "- Built from the lord-damage hit records logged in the match "
              "file, one entry per attack that landed\n"
              "- Totalled per attacking player, so you can see who is "
              "chipping or finishing the Lord\n"
              "- Only those recorded hits count - HP lost by the Lord to "
              "degen, poison, bleeding or lifesteal ticks is not included",
              hk.toggleLordDamage),
            T(Ico::LogList, "Combat Log", m_showCombatLog,
              TexIcon{ "Toolbar\\texture_277156.dds", ImVec4(6, 50, 17, 69), 1.30f },
              "Chronological log of skills, damage, heals, interrupts, deaths "
              "and attacks from the replay. Damage and heals are matched to "
              "skill casts when the timing fits. Category toggles in the "
              "panel control what is listed."),
            T(Ico::BarChart, "Skill Analytics", m_showSkillAnalytics,
              TexIcon{ "Toolbar\\texture_143781.dds", ImVec4(0, 96, 21, 117), 1.22f,
                       ImVec4(24, 96, 45, 117) },
              "Open skill usage analytics for the match (beta)."),
            T(Ico::Gradient, "Heatmap", m_heatmapSettings.show,
              TexIcon{ "Toolbar\\heatmap.png", ImVec4(57, 0, 305, 248), 1.22f },
              "Accumulate player movement into a density overlay on the map. "
              "Each layer samples positions over time, so hotter areas show "
              "where agents spent more time. Track a single player, several "
              "players, or an entire team over a chosen time window.",
              hk.toggleHeatmap),
            T(Ico::Piano, "Piano Roll", m_showPianoRoll,
              TexIcon{ "Toolbar\\piano.png", ImVec4(0, 0, 0, 0), 1.16f,
                       ImVec4(0, 0, 0, 0), nullptr, ImVec4(0, 0, 0, 0),
                       false, true, true },
              "Show each player's skill casts on a shared timeline, like a "
              "music piano roll, so you can compare cast timing, overlaps "
              "and cooldowns across the match.",
              hk.togglePianoRoll),
        } });

        groups.push_back({ "Camera", {
            T(Ico::Camcorder, "Auto Camera", m_showAutoCameraPanel,
              TexIcon{ "Toolbar\\texture_158270.dds", ImVec4(0, 16, 16, 32), 1.10f,
                       ImVec4(16, 16, 32, 32) },
              "Open the auto-camera panel. Lets the camera follow the action "
              "on its own, using presets (e.g. follow the flag carrier, "
              "follow a chosen player, or cut between fights) instead of you "
              "steering manually.",
              hk.toggleAutoCamera),
            Item{ Ico::Eye, "Top View",
                  [this] { return m_topViewActive; },
                  [this] { if (m_topViewActive) ExitTopView(); else EnterTopView(); },
                  false, Ico::None, nullptr, nullptr,
                  TexIcon{ "Toolbar\\texture_143781.dds", ImVec4(144, 120, 165, 141), 1.22f,
                           ImVec4(168, 120, 189, 141) },
                  0,
                  "Switch to a top-down orthographic view of the map, looking "
                  "straight down like a tactical map - useful for reading "
                  "team positioning and split calls at a glance.",
                  hk.toggleTopView },
            T(Ico::Frame, "Split Camera", m_pipEnabled,
              TexIcon{ "Toolbar\\Splitcamera.png", ImVec4(6, 12, 62, 56), 1.18f },
              "Enable a picture-in-picture second camera. Adds a smaller, "
              "independently aimed view on top of the main one, so you can "
              "watch two locations on the map at once."),
            T(Ico::MapGrid, "Minimap", m_minimapEnabled,
              TexIcon{ "Toolbar\\Minimap.png", ImVec4(8, 8, 60, 60), 1.18f },
              "Show or hide the minimap overlay - a small top-down map in "
              "the corner showing player positions. Agent name labels and "
              "profession icons can each be toggled on or off, and you can "
              "scroll the mouse wheel over it to zoom in or out.",
              hk.toggleMinimap),
        } });

        groups.push_back({ "Panels", {
            C(Ico::Party1, "Team 1 Party", m_showTeam1Party, kCylRed,
              "Show the red team's party panel: each player's health, "
              "conditions/enchantments and status at the current replay "
              "time, like an in-game party window."),
            C(Ico::Party2, "Team 2 Party", m_showTeam2Party, kCylBlue,
              "Show the blue team's party panel: each player's health, "
              "conditions/enchantments and status at the current replay "
              "time, like an in-game party window."),
            T(Ico::Clock, "Event Timeline", m_showEventTimeline,
              TexIcon{ "Toolbar\\videotimeline.png", ImVec4(7, 14, 61, 51), 1.18f },
              "Show a timeline strip marking player deaths, flag stand "
              "caps, shrine captures, catapult builds/destructions, "
              "gate/door breaks and other major match events, so you can "
              "jump straight to them. It also plots each team's overall HP "
              "over time: at every second, a team's line is the sum of its "
              "living players' current HP divided by the sum of their max "
              "HP (dead players count as 0), so it reads as \"percentage of "
              "the team's total health pool remaining\".",
              hk.toggleEventTimeline),
            // First cell of the sheet: the shield-and-sword pair, without the swap arrows
            // the second cell carries.
            T(Ico::Shield, "Weapon Sets", m_showFocusHud,
              TexIcon{ "Toolbar\\texture_283989.dds", ImVec4(7, 4, 61, 60), 1.18f },
              "Show or hide the weapon sets HUD - the row of weapon slots in the "
              "bottom-right corner, listing every set the followed player used and "
              "highlighting the one currently equipped. It only appears while the "
              "camera is following a player, and does not affect the weapon sets "
              "shown in the player info panel."),
        } });

        groups.push_back({ "Tools", {
            T(Ico::Brush, "Drawing Toolbar", m_annotationMgr.toolbar_visible,
              TexIcon{ "Toolbar\\brush.png", ImVec4(14, 13, 55, 50), 1.18f },
              "Show the drawing toolbar for annotations on the map - arrows, "
              "circles and freehand lines for marking up rotations or plays "
              "when reviewing or casting a replay. It opens as its own strip, "
              "already armed so you can draw straight away; pick Cursor or "
              "press Escape to go back to moving the camera.",
              hk.toggleDrawingBar),
            // Momentary: opens the naming prompt rather than toggling a panel.
            Item{ Ico::Bookmark, "Add Bookmark",
                  [] { return false; },
                  [this] { m_annotationMgr.BeginAddBookmark(); },
                  false, Ico::None, nullptr, nullptr,
                  TexIcon{ "Toolbar\\texture_271801.dds", ImVec4(0, 0, 0, 0), 1.20f },
                  0,
                  "Mark the current moment so you can jump back to it later. "
                  "Playback pauses while you name it. You can also right-click "
                  "the scrub bar to drop one instantly at that point.",
                  hk.addBookmark },
            T(Ico::Bookmark, "Bookmarks", m_annotationMgr.bookmarks_visible,
              TexIcon{ "Toolbar\\terrain_tex_indices_0x49EBD.png",
                       ImVec4(6, 5, 28, 28), 1.26f },
              "Show the bookmarks panel, listing saved timestamps in this "
              "match so you can jump back to key moments. Bookmarks are "
              "saved to disk per match, so they are still there if you "
              "close the replay and come back to review it later. Add a "
              "bookmark at the current time with the shortcut.",
              hk.addBookmark),
            T(Ico::Doc, "Notepad", m_showNotepad,
              TexIcon{ "Others_UI\\texture_283991.dds", ImVec4(32, 0, 48, 16), 1.14f },
              "Open the match notepad - a free-text panel for writing notes "
              "or commentary alongside the replay, saved with the match."),
        } });

        // Own group, so the ribbon's group divider separates it from the
        // panel toggles either side. Sound FX sits here rather than in Tools:
        // it is a mixer, so it belongs with the other configuration buttons.
        groups.push_back({ "Settings", {
            // One piece of art for both states. The button opens a panel rather than switching
            // the sound on and off, so a crossed-out speaker would say the wrong thing - the
            // active state is carried by the ribbon's own gold border. Passing rectOn identical
            // to rect marks this as state-carrying art, which also stops the off state being
            // dimmed: same icon, lit the same, border is the only difference.
            T(Ico::Speaker, "Sound FX", m_showSoundFxPanel,
              TexIcon{ "Toolbar\\SpeakerON.png", ImVec4(9, 12, 47, 44), 1.18f,
                       ImVec4(9, 12, 47, 44) },
              "Open the Sound FX mixer: levels, hearing distance and falloff "
              "curve. Muting is on the play bar's speaker, not here."),
            Item{ Ico::More, "Settings", [this] { return m_ribbonMoreOpen; },
                  [] {}, true, Ico::None, nullptr, nullptr,
                  TexIcon{ "Others_UI\\texture_382152.dds",
                           ImVec4(11, 11, 53, 53), 1.20f },
                  0,
                  "Open interface preferences or the keyboard shortcut list." },
        } });

        ImFont* font = ImGui::GetFont();
        const float rowY = origin.y + 4.f;                 // icon row top
        const float capY = rowY + kIconBox + 3.f;          // caption baseline top

        float x = origin.x + kEdgePad;
        int   uid = 0;

        for (size_t gi = 0; gi < groups.size(); gi++)
        {
            const Group& g = groups[gi];
            const float gx0 = x;

            for (size_t ii = 0; ii < g.items.size(); ii++)
            {
                const Item& it = g.items[ii];
                ImVec2 tl(x, rowY);
                ImVec2 br(x + kIconBox, rowY + kIconBox);

                ImGui::PushID(uid++);
                ImGui::SetCursorScreenPos(tl);

                const bool on = it.get();

                bool hovered = false, pressed = false;
                if (interact)
                {
                    pressed = ImGui::InvisibleButton("##ico", ImVec2(kIconBox, kIconBox));
                    hovered = ImGui::IsItemHovered();
                    if (hovered)
                    {
                        const char* title = (!on && it.nameOff) ? it.nameOff : it.name;
                        DrawRibbonTooltip(m_latoBold, title, it.desc, it.shortcut);
                    }
                }

                if (on)
                {
                    dl->AddRectFilled(tl, br, fade(kFillOn), 5.f);
                    dl->AddRect(tl, br, fade(kBorderOn), 5.f, 0, 1.f);
                }
                else if (hovered)
                {
                    dl->AddRectFilled(tl, br, fade(kFillHot), 5.f);
                }

                ImVec2 gc((tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f);

                ImU32 glyphCol = on ? kIconOn : (hovered ? kIconHot : kIconOff);
                if (it.glyphTint)
                    glyphCol = Shade(it.glyphTint, on ? 1.f : (hovered ? 0.92f : 0.68f), 1.f);
                glyphCol = fade(glyphCol);

                bool drawn = false;
                if (it.customDraw)
                {
                    it.customDraw(dl, gc, kGlyph, on, contentA);
                    drawn = true;
                }
                else if (it.tex.file)
                {
                    drawn = DrawTexIcon(dl, m_deviceResources->GetD3DDevice(), it.tex,
                                        gc, kGlyph, contentA, on, hovered, glyphCol);
                }
                if (!drawn)
                {
                    Ico glyph = (!on && it.icoOff != Ico::None) ? it.icoOff : it.ico;
                    DrawIcon(dl, glyph, gc, kGlyph, glyphCol);
                }

                if (pressed)
                {
                    if (it.isMore)
                    {
                        m_ribbonMoreOpen = true;
                        ImGui::OpenPopup("##ribbon_more");
                    }
                    else
                    {
                        it.toggle();
                    }
                }

                if (it.isMore)
                {
                    // Settings only. The overlay and meter toggles this menu
                    // used to carry live in the View menu, next to the rest
                    // of the panel toggles.
                    if (ImGui::BeginPopup("##ribbon_more"))
                    {
                        m_ribbonMoreOpen = true;
                        if (ImGui::MenuItem("Interface Preferences"))
                            m_showInterfacePrefs = true;
                        if (ImGui::MenuItem("Shortcuts"))
                            m_showShortcutPreferences = true;
                        ImGui::EndPopup();
                    }
                    else
                    {
                        m_ribbonMoreOpen = false;
                    }
                }

                ImGui::PopID();
                x = br.x + kIconGap;
            }

            const float gx1 = x - kIconGap;

            // Caption, centred beneath its own group.
            ImVec2 ts = font->CalcTextSizeA(kCaptionPx, FLT_MAX, 0.f, g.caption);
            dl->AddText(font, kCaptionPx,
                        ImVec2((gx0 + gx1) * 0.5f - ts.x * 0.5f, capY),
                        fade(kCaptionCol), g.caption);

            // Divider between groups.
            if (gi + 1 < groups.size())
            {
                float dx = gx1 + kGroupGap * 0.5f;
                dl->AddLine(ImVec2(dx, rowY + 2.f), ImVec2(dx, capY + kCaptionPx + 1.f),
                            fade(kDivider), 1.f);
                x = gx1 + kGroupGap;
            }
        }

        // Pin, right-aligned. Uses the game's own button plate, which already
        // carries hover/pressed states. Sideways pin = auto-hide; upright = held open.
        {
            ID3D11Device* dev = m_deviceResources->GetD3DDevice();
            ImVec2 tl(origin.x + width - kEdgePad - kIconBox, rowY);

            ImGui::PushID(uid++);
            ImGui::SetCursorScreenPos(tl);

            bool hovered = false, pressed = false;
            if (interact)
            {
                pressed = ImGui::InvisibleButton("##pin", ImVec2(kIconBox, kIconBox));
                hovered = ImGui::IsItemHovered();
                if (hovered)
                {
                    DrawRibbonTooltip(m_latoBold,
                        m_ribbonPinned ? "Unpin Ribbon" : "Pin Ribbon",
                        m_ribbonPinned
                            ? "Allow the toolbar to auto-hide when the pointer leaves."
                            : "Keep the toolbar expanded instead of auto-hiding.",
                        0);
                }
            }

            const ImVec4 normal = m_ribbonPinned ? ImVec4(80, 0, 95, 15)
                                                 : ImVec4(48, 0, 63, 15);
            const ImVec4 hot    = m_ribbonPinned ? ImVec4(96, 0, 111, 15)
                                                 : ImVec4(64, 0, 79, 15);
            ImVec2  gc(tl.x + kIconBox * 0.5f, tl.y + kIconBox * 0.5f);
            TexIcon t{ "Toolbar\\texture_134733.dds", hovered ? hot : normal, 1.06f };
            if (!DrawTexIcon(dl, dev, t, gc, kGlyph, contentA, true, false, 0))
                DrawIcon(dl, Ico::Pin, gc, kGlyph,
                         fade(hovered ? kIconHot : kIconOff));

            if (pressed)
            {
                m_ribbonPinned = !m_ribbonPinned;
                SaveUILayout();     // pin state is not covered by the panel-state hash
            }

            ImGui::PopID();
        }
    }

    ImGui::End();
}
