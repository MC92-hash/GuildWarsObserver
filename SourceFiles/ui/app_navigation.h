#pragma once

#include "GuiGlobalConstants.h"
#include "TextureCache.h"
#include "ui_kit.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>

// The approved D1 artwork is kept intact as an atlas. UV rectangles below use
// its original 1536 x 1024 coordinates for texture and engraved corners.
// Navigation emblems are drawn as paths, independently of the raster artwork.
enum class AppTab { None = -1, Library, Scout, Wardrobe };

namespace app_navigation
{
// Shared 24-unit grid: flat ink, open counters and no baked lighting. Drawing
// paths keeps the book, compass and armor crisp at every interface scale.
static void DrawEmblem(ImDrawList* dl, AppTab tab, ImVec2 origin, float size, ImU32 ink)
{
    const float scale = size / 24.f;
    const float stroke = 1.25f * scale;
    auto p = [&](float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
    auto line = [&](float x1, float y1, float x2, float y2) { dl->AddLine(p(x1, y1), p(x2, y2), ink, stroke); };
    if (tab == AppTab::Library)
    {
        // Two facing pages and a separate cover, with a gently curved binding.
        for (float side : {-1.f, 1.f})
        {
            auto q = [&](float x, float y) { return p(12.f + side * x, y); };
            dl->PathLineTo(q(0, 6));
            dl->PathBezierCubicCurveTo(q(2.5f, 3.5f), q(5.5f, 3), q(9, 4));
            dl->PathLineTo(q(9, 18));
            dl->PathBezierCubicCurveTo(q(5.5f, 17), q(2.5f, 17.5f), q(0, 20));
            dl->PathStroke(ink, ImDrawFlags_None, stroke);
            dl->PathLineTo(q(11, 6)); dl->PathLineTo(q(11, 21));
            dl->PathBezierCubicCurveTo(q(6, 19.5f), q(3, 20), q(0, 22));
            dl->PathStroke(ink, ImDrawFlags_None, stroke);
            for (float y : {9.f, 13.f})
                dl->AddBezierQuadratic(q(3, y), q(5.5f, y - 1), q(7, y - .5f), ink, stroke);
        }
        line(12, 6, 12, 20);
    }
    else if (tab == AppTab::Scout)
    {
        dl->AddCircle(p(12, 12), 7.5f * scale, ink, 48, stroke);
        // Four split points leave the center and the needle edges readable.
        for (int i = 0; i < 4; ++i)
        {
            auto q = [&](float x, float y) {
                for (int j = 0; j < i; ++j) { const float tmp = x; x = -y; y = tmp; }
                return p(12 + x, 12 + y);
            };
            dl->AddTriangleFilled(q(0, -11.5f), q(-2, -2), q(0, -3.2f), ink);
            dl->AddTriangleFilled(q(0, -11.5f), q(0, -3.2f), q(2, -2), ink);
            dl->AddLine(q(3.5f, -3.5f), q(4.7f, -4.7f), ink, stroke);
        }
        dl->AddCircle(p(12, 12), 1.5f * scale, ink, 16, stroke * .8f);
    }
    else if (tab == AppTab::Wardrobe)
    {
        // Symmetric plate panels; the gaps form the neckline and central seam.
        for (float side : {-1.f, 1.f})
        {
            auto panel = [&](std::initializer_list<ImVec2> points) {
                // Mirroring reverses winding: keep filled paths clockwise.
                if (side > 0)
                    for (const auto& point : points) dl->PathLineTo(p(12 + point.x, point.y));
                else
                    for (auto it = points.end(); it != points.begin();) { --it; dl->PathLineTo(p(12 - it->x, it->y)); }
                dl->PathFillConvex(ink);
            };
            panel({{4, 2}, {7, 3}, {10.5f, 7}, {9, 9}, {5.8f, 8}, {3, 4}});
            panel({{.65f, 6}, {2, 5.5f}, {4.7f, 9}, {7, 10.3f}, {6.5f, 15.5f}, {.65f, 18}});
            panel({{.65f, 19.3f}, {6.5f, 16.8f}, {7.5f, 20}, {.65f, 23}});
        }
    }
}

struct Layout
{
    float scale, height, menuWidth, menuX, navX, tabWidth;
    bool brand, labels;
    float creditX, creditWidth;
    bool compact;
};

static Layout GetLayout()
{
    const float s = GuiGlobalConstants::ContentTop() / 64.f;
    const float width = ImGui::GetIO().DisplaySize.x;
    const int menus = GuiGlobalConstants::IsDeveloperMode() ? 3 : 2;
    const float menuWidth = ImGui::CalcTextSize("FileHelp").x
        + (menus == 3 ? ImGui::CalcTextSize("Debug").x : 0.f)
        + menus * ImGui::GetStyle().ItemSpacing.x * 2.f + 16.f * s;
    ImFont* font = ImGui::GetFont();
    const float creditWidth = font->CalcTextSizeA(12.f * s, FLT_MAX, 0, "By Purif & Maverick").x;
    const float versionWidth = font->CalcTextSizeA(12.f * s, FLT_MAX, 0, "v" GWO_VERSION).x;
    const bool compact = width < 650.f * s;
    const float right = width - 28.f * s;
    const float creditX = right - (compact ? std::max(creditWidth, menuWidth + versionWidth + 8.f * s)
                                          : creditWidth + versionWidth + 12.f * s);
    const float menuX = compact ? creditX : creditX - menuWidth - 16.f * s;
    const bool brand = menuX >= 580.f * s;
    const float navX = brand ? 196.f * s : 30.f * s;
    const float tabWidth = std::min(164.f * s, std::max(1.f, (menuX - navX - 8.f * s) / 3.f));
    return { s, 64.f * s, menuWidth, menuX, navX, tabWidth, brand, tabWidth >= 120.f * s,
        creditX, creditWidth, compact };
}

static void Atlas(ImDrawList* dl, ImTextureID texture, ImVec2 pos, ImVec2 size,
                  ImVec4 source, ImU32 tint = IM_COL32_WHITE)
{
    if (texture)
        dl->AddImage(texture, pos, ImVec2(pos.x + size.x, pos.y + size.y),
            ImVec2(source.x / 1536.f, source.y / 1024.f),
            ImVec2(source.z / 1536.f, source.w / 1024.f), tint);
}

// A separate transparent menu window retains ImGui's native menu navigation,
// popup placement and keyboard behavior while sharing the one visual header.
static bool BeginMenus()
{
    const Layout l = GetLayout();
    const float mh = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(l.menuX, std::floor(l.compact ? l.height - mh - 10.f * l.scale : (l.height - mh) * .5f)));
    ImGui::SetNextWindowSize(ImVec2(l.menuWidth, mh + 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(216, 210, 195, 255));
    ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(74, 63, 42, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(83, 71, 47, 255));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(57, 49, 34, 255));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(30, 28, 24, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(132, 108, 65, 255));
    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(90, 76, 51, 255));
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##app_header_menus", nullptr, ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_MenuBar);
    // Restore popup styling before any menus are opened.
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    return ImGui::BeginMenuBar();
}

static void EndMenus(bool open)
{
    if (open) ImGui::EndMenuBar();
    ImGui::End();
    ImGui::PopStyleColor(7);
}
}

static AppTab draw_app_ribbon(AppTab current)
{
    using namespace app_navigation;
    const Layout l = GetLayout();
    const float s = l.scale;
    const float width = ImGui::GetIO().DisplaySize.x;
    const ImU32 bronze = IM_COL32(132, 108, 65, 255);
    const ImU32 gold = IM_COL32(228, 188, 111, 255);
    const ImU32 ivory = IM_COL32(244, 232, 207, 255);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, l.height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(28, 27, 23, 255));
    AppTab clicked = AppTab::None;
    if (ImGui::Begin("##app_ribbon", nullptr, ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus))
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ui::EnsureTextureBasePath();
        ImTextureID atlas = GetTextureCache().GetTexture(
            ui::TextureBasePath() + "\\Textures\\Toolbar\\navigation_d1_atlas.png");
        // Tile the empty material region, rather than stretching its grain.
        for (float x = 0; x < width; x += 340.f * s)
        {
            const float w = std::min(340.f * s, width - x);
            Atlas(dl, atlas, ImVec2(x, 4.f * s), ImVec2(w, l.height - 8.f * s),
                ImVec4(970, 122, 970 + w / s, 169));
        }
        const float ruleY = l.height - 5.f * s;
        dl->AddRect(ImVec2(5.f * s, 4.f * s), ImVec2(width - 5.f * s, ruleY), bronze, 2.f * s);
        dl->AddRect(ImVec2(8.f * s, 7.f * s), ImVec2(width - 8.f * s, ruleY - 3.f * s),
            IM_COL32(108, 88, 55, 110), 1.f * s);
        Atlas(dl, atlas, ImVec2(5.f * s, 4.f * s), ImVec2(34.f * s, l.height - 8.f * s),
            ImVec4(201, 119, 235, 173));
        Atlas(dl, atlas, ImVec2(width - 39.f * s, 4.f * s), ImVec2(34.f * s, l.height - 8.f * s),
            ImVec4(1477, 119, 1511, 173));

        if (l.brand)
        {
            // Transparent version of the loading screen's two-line logo artwork.
            if (ImTextureID logo = GetTextureCache().GetTexture(
                ui::TextureBasePath() + "\\Textures\\Toolbar\\gw_observer_logo.png"))
            {
                const float logoH = 48.f * s;
                const float logoW = logoH * (2032.f / 774.f);
                const ImVec2 pos(46.f * s, (l.height - logoH) * .5f);
                dl->AddImage(logo, pos, ImVec2(pos.x + logoW, pos.y + logoH));
            }
        }

        ImFont* interfaceFont = ImGui::GetFont();
        const float creditSize = 12.f * s;
        const float versionWidth = interfaceFont->CalcTextSizeA(creditSize, FLT_MAX, 0, "v" GWO_VERSION).x;
        const float right = width - 28.f * s;
        const float creditY = l.compact ? 10.f * s : (l.height - creditSize) * .5f;
        dl->AddText(interfaceFont, creditSize,
            ImVec2(l.compact ? right - l.creditWidth : l.creditX, creditY),
            IM_COL32(170, 163, 147, 255), "By Purif & Maverick");
        dl->AddText(interfaceFont, creditSize,
            ImVec2(right - versionWidth, l.compact ? l.height - creditSize - 13.f * s : creditY),
            gold, "v" GWO_VERSION);

        struct Tab { const char* label; const char* tip; };
        static const Tab tabs[] = {
            { "Library", "Your matches" },
            { "Scout", "What they run, and what beats them" },
            { "Wardrobe", "Create a character and dress it" },
        };
        ImFont* font = ImGui::GetFont();
        for (int i = 0; i < 3; ++i)
        {
            const auto& tab = tabs[i];
            const bool active = static_cast<AppTab>(i) == current;
            const ImVec2 mn(l.navX + i * l.tabWidth, 8.f * s);
            const ImVec2 mx(mn.x + l.tabWidth, ruleY - 3.f * s);
            ImGui::SetCursorScreenPos(mn);
            ImGui::PushID(i);
            // Button supplies keyboard activation; its visual is entirely custom.
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
            if (ImGui::Button("##destination", ImVec2(l.tabWidth, mx.y - mn.y)))
                clicked = static_cast<AppTab>(i);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
            const bool hover = ImGui::IsItemHovered();
            const bool held = ImGui::IsItemActive();
            const bool focus = ImGui::IsItemFocused();
            if (hover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                ImGui::SetTooltip("%s\n%s", tab.label, tab.tip);
            if (held || hover || active)
                dl->AddRectFilled(mn, mx, held ? IM_COL32(0, 0, 0, 85) :
                    (hover ? IM_COL32(156, 126, 70, 30) : IM_COL32(156, 126, 70, 12)));

            const float textSize = ImGui::GetFontSize();
            const ImVec2 ts = font->CalcTextSizeA(textSize, FLT_MAX, 0, tab.label);
            const float iconSize = std::max(1.f, std::min(30.f * s, l.tabWidth - 8.f * s));
            const float gap = l.labels ? 12.f * s : 0.f;
            float x = mn.x + (l.tabWidth - (l.labels ? ts.x : 0.f) - iconSize - gap) * .5f;
            {
                const ImU32 ink = active ? IM_COL32(196, 173, 118, 255) :
                    (hover ? IM_COL32(218, 207, 178, 255) : IM_COL32(170, 166, 152, 255));
                DrawEmblem(dl, static_cast<AppTab>(i), ImVec2(x, (l.height - iconSize) * .5f), iconSize, ink);
                x += iconSize + gap;
            }
            if (l.labels)
                dl->AddText(font, textSize, ImVec2(x, (l.height - ts.y) * .5f),
                    active || hover ? ivory : IM_COL32(184, 181, 171, 255), tab.label);
            if (active)
            {
                const float cx = mn.x + l.tabWidth * .5f;
                const float r = 6.f * s;
                dl->AddLine(ImVec2(mn.x + 12.f * s, ruleY), ImVec2(mx.x - 12.f * s, ruleY), gold, 2.f * s);
                const ImVec2 diamond[] = { {cx, ruleY - r}, {cx + r, ruleY}, {cx, ruleY + r}, {cx - r, ruleY} };
                dl->AddConvexPolyFilled(diamond, 4, gold);
                dl->AddPolyline(diamond, 4, IM_COL32(255, 226, 166, 255), ImDrawFlags_Closed, 1.f);
            }
            if (focus && ImGui::GetIO().NavVisible)
                dl->AddRect(ImVec2(mn.x + 3.f * s, mn.y + 2.f * s),
                    ImVec2(mx.x - 3.f * s, mx.y - 2.f * s), ivory, 1.f, 0, 1.f * s);
            ImGui::PopID();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    return clicked;
}
