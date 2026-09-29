#include "pch.h"

#include "ui/ui_kit.h"

#include "GuiGlobalConstants.h"
#include "MatchRatings.h"
#include "TextureCache.h"

#include <cmath>
#include <cstring>

// =================================================================================================
// THE SHARED UI KIT. See ui_kit.h.
//
// The first half of this file is the replay browser's own chrome, moved here unchanged: the same
// bodies, the same numbers, the same comments. Only two things about them changed - they are no
// longer `static`, and their default arguments now live in the header, where a declaration must
// carry them. The browser keeps its pixels because nothing else did.
//
// The second half is new, and is built out of the first half's tokens rather than out of fresh
// colours, so a screen written with it cannot drift away from the gallery.
// =================================================================================================

namespace ui
{

// ─── Themeable colour palette (moved) ────────────────────────────────────────

ImVec4 kColorBg, kColorPanel, kColorPanelLight, kColorBorder;
ImVec4 kColorAccent, kColorAccentDim;
ImVec4 kColorText, kColorTextDim;
ImVec4 kColorTextMuted;
ImVec4 kColorSelected, kColorHover;

ImU32 kCardMapName, kCardDate, kCardDuration;
ImU32 kCardGuildName, kCardGuildTag, kCardVS;
ImU32 kCardTeamLabel, kCardProfSig, kCardBuildName, kCardViewDetails;
ImU32 kCardMatchupBg, kCardMatchupRule;

BrowserThemeColors s_themeColors;

ImU32 LerpColor(ImU32 a, ImU32 b, float t) {
    float r = ((a >> 0) & 0xFF) * (1 - t) + ((b >> 0) & 0xFF) * t;
    float g = ((a >> 8) & 0xFF) * (1 - t) + ((b >> 8) & 0xFF) * t;
    float bl = ((a >> 16) & 0xFF) * (1 - t) + ((b >> 16) & 0xFF) * t;
    float al = ((a >> 24) & 0xFF) * (1 - t) + ((b >> 24) & 0xFF) * t;
    return IM_COL32((int)r, (int)g, (int)bl, (int)al);
}

int s_appliedTheme = -1;

void ApplyBrowserTheme(int theme)
{
    if (theme == s_appliedTheme) return;
    s_appliedTheme = theme;

    if (theme == 1) // Watchtower Dashboard - zinc bg, amber accent (#f59e0b)
    {
        kColorBg         = ImVec4(0.094f, 0.094f, 0.106f, 1.00f); // #18181b zinc-950
        kColorPanel      = ImVec4(0.094f, 0.094f, 0.106f, 0.55f); // surface 1
        kColorPanelLight = ImVec4(0.153f, 0.153f, 0.165f, 0.40f); // surface raised
        kColorBorder     = ImVec4(0.247f, 0.247f, 0.275f, 0.45f); // border soft
        kColorAccent     = ImVec4(0.961f, 0.620f, 0.043f, 1.00f); // #f59e0b amber
        kColorAccentDim  = ImVec4(0.961f, 0.620f, 0.043f, 0.50f); // amber dimmed
        kColorText       = ImVec4(0.894f, 0.894f, 0.906f, 1.00f); // #e4e4e7 zinc-200
        kColorTextDim    = ImVec4(0.631f, 0.631f, 0.667f, 1.00f); // #a1a1aa zinc-400
        kColorTextMuted  = ImVec4(0.789f, 0.789f, 0.810f, 1.00f); // #c9c9cf
        kColorSelected   = ImVec4(0.961f, 0.620f, 0.043f, 0.15f); // amber tint
        kColorHover      = ImVec4(0.961f, 0.620f, 0.043f, 0.10f); // amber tint

        kCardMapName     = IM_COL32(161, 161, 170, 255); // zinc-400
        kCardDate        = IM_COL32(161, 161, 170, 255); // zinc-400 (was zinc-500)
        kCardDuration    = IM_COL32(113, 113, 122, 255); // zinc-500 full alpha (was zinc-600 @70%)
        kCardGuildName   = IM_COL32(228, 228, 231, 255); // zinc-200
        kCardGuildTag    = IM_COL32(245, 158,  11, 180); // amber-500 @70% (was zinc-400)
        kCardVS          = IM_COL32(113, 113, 122, 255); // zinc-500 (was zinc-600)
        kCardTeamLabel   = IM_COL32(113, 113, 122, 255); // zinc-500 (was zinc-600)
        kCardProfSig     = IM_COL32(161, 161, 170, 255); // zinc-400
        kCardBuildName   = IM_COL32(245, 158, 11, 255);  // #f59e0b amber
        kCardViewDetails = IM_COL32(161, 161, 170, 255); // zinc-400 (was zinc-500)
        kCardMatchupBg   = IM_COL32(18,  18,  20, 255);  // slightly darker than base
        kCardMatchupRule = IM_COL32(245, 158, 11,  25);  // subtle amber rule

        s_themeColors.popupBg       = ImVec4(0.094f, 0.094f, 0.106f, 0.95f);
        s_themeColors.frameBg       = ImVec4(0.094f, 0.094f, 0.106f, 0.55f);
        s_themeColors.frameBgHov    = ImVec4(0.153f, 0.153f, 0.165f, 0.60f);
        s_themeColors.frameBgAct    = ImVec4(0.153f, 0.153f, 0.165f, 0.80f);
        s_themeColors.titleBgAct    = ImVec4(0.094f, 0.094f, 0.106f, 0.95f);
        s_themeColors.scrollBg      = ImVec4(0.07f, 0.07f, 0.08f, 0.50f);
        s_themeColors.scrollGrab    = ImVec4(0.247f, 0.247f, 0.275f, 0.60f);
        s_themeColors.scrollGrabHov = ImVec4(0.322f, 0.322f, 0.357f, 0.70f);
        s_themeColors.button        = ImVec4(0.153f, 0.153f, 0.165f, 0.40f);
        s_themeColors.buttonHov     = ImVec4(0.961f, 0.620f, 0.043f, 0.15f);
        s_themeColors.tableHeaderBg = ImVec4(0.12f, 0.12f, 0.13f, 0.45f);
        s_themeColors.tableBorderStrong = ImVec4(0.247f, 0.247f, 0.275f, 0.65f);
        s_themeColors.tableBorderLight  = ImVec4(0.247f, 0.247f, 0.275f, 0.30f);
        s_themeColors.tableRowBgAlt = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
        s_themeColors.rowHoverBg    = ImVec4(0.340f, 0.340f, 0.360f, 0.90f);
        s_themeColors.rowSelectedBg = ImVec4(0.961f, 0.620f, 0.043f, 0.20f);
        s_themeColors.splitterIdle  = IM_COL32(63, 63, 70, 115);    // border soft
        s_themeColors.splitterActive = IM_COL32(245, 158, 11, 255); // amber
        s_themeColors.splitterHover = IM_COL32(251, 191, 36, 180);  // amber hover
        s_themeColors.detailPanelBg = ImVec4(0.08f, 0.08f, 0.09f, 0.90f);
        s_themeColors.replayBtnBg   = ImVec4(0.094f, 0.094f, 0.106f, 1.0f);
        s_themeColors.replayBtnHov  = ImVec4(0.153f, 0.153f, 0.165f, 1.0f);
        s_themeColors.replayBtnAct  = ImVec4(0.12f, 0.12f, 0.13f, 1.0f);
        s_themeColors.compTextCol   = ImVec4(0.631f, 0.631f, 0.667f, 1.f); // zinc-400
        s_themeColors.cardBg        = ImVec4(0.094f, 0.094f, 0.106f, 0.55f);
        s_themeColors.cardBgSel     = ImVec4(0.961f, 0.620f, 0.043f, 0.08f);
        s_themeColors.cardBorderSel = ImVec4(0.961f, 0.620f, 0.043f, 1.0f); // amber
        s_themeColors.cardBorderIdle = ImVec4(0.247f, 0.247f, 0.275f, 0.45f);
        s_themeColors.cardGradientBot  = IM_COL32(14, 14, 16, 240);
        s_themeColors.cardGradientMid  = IM_COL32(14, 14, 16, 0);
        s_themeColors.cardHoverBorder  = IM_COL32(245, 158, 11, 180);
        s_themeColors.cardFallbackBg   = IM_COL32(22, 22, 26, 255);
    }
    else // Theme 0: GW Observer - warm near-black, desaturated gold
    {
        kColorBg         = ImVec4(0.075f, 0.075f, 0.075f, 1.00f); // #131313
        kColorPanel      = ImVec4(0.102f, 0.102f, 0.102f, 1.00f); // #1A1A1A
        kColorPanelLight = ImVec4(0.141f, 0.133f, 0.125f, 1.00f); // #242220
        kColorBorder     = ImVec4(0.239f, 0.227f, 0.200f, 1.00f); // #3D3A33
        kColorAccent     = ImVec4(0.878f, 0.710f, 0.388f, 1.00f); // #E0B563
        kColorAccentDim  = ImVec4(0.878f, 0.710f, 0.388f, 0.70f);
        kColorText       = ImVec4(0.941f, 0.937f, 0.914f, 1.00f); // #F0EFE9
        kColorTextDim    = ImVec4(0.612f, 0.580f, 0.533f, 1.00f); // #9C9488
        kColorTextMuted  = ImVec4(0.809f, 0.794f, 0.762f, 1.00f); // #CFCAC2
        kColorSelected   = ImVec4(0.228f, 0.185f, 0.101f, 0.90f);
        kColorHover      = ImVec4(0.202f, 0.163f, 0.089f, 0.70f);

        kCardMapName     = IM_COL32(175, 172, 165, 255);
        kCardDate        = IM_COL32(130, 127, 120, 255);
        kCardDuration    = IM_COL32(110, 107, 100, 180);
        kCardGuildName   = IM_COL32(240, 236, 225, 255);
        kCardGuildTag    = IM_COL32(190, 185, 170, 255); // warm silver
        kCardVS          = IM_COL32(120, 105,  75, 255);
        kCardTeamLabel   = IM_COL32(100,  97,  90, 255);
        kCardProfSig     = IM_COL32(160, 155, 145, 255);
        kCardBuildName   = IM_COL32(210, 185, 120, 255);
        kCardViewDetails = IM_COL32(140, 137, 130, 255);
        kCardMatchupBg   = IM_COL32( 14,  13,  10, 255);
        kCardMatchupRule = IM_COL32(196, 169, 106,  30);

        s_themeColors.popupBg       = ImVec4(0.055f, 0.055f, 0.055f, 0.97f);
        s_themeColors.frameBg       = ImVec4(0.078f, 0.078f, 0.078f, 0.85f); // #141414 well
        s_themeColors.frameBgHov    = ImVec4(0.110f, 0.106f, 0.098f, 0.90f);
        s_themeColors.frameBgAct    = ImVec4(0.145f, 0.133f, 0.108f, 0.95f);
        s_themeColors.titleBgAct    = ImVec4(0.08f, 0.07f, 0.06f, 0.95f);
        s_themeColors.scrollBg      = ImVec4(0.05f, 0.04f, 0.03f, 0.50f);
        s_themeColors.scrollGrab    = ImVec4(0.28f, 0.24f, 0.16f, 0.60f);
        s_themeColors.scrollGrabHov = ImVec4(0.38f, 0.32f, 0.22f, 0.70f);
        s_themeColors.button        = ImVec4(0.14f, 0.12f, 0.08f, 0.80f);
        s_themeColors.buttonHov     = ImVec4(0.25f, 0.22f, 0.14f, 0.80f);
        s_themeColors.tableHeaderBg = ImVec4(0.16f, 0.14f, 0.09f, 0.45f);
        s_themeColors.tableBorderStrong = ImVec4(0.20f, 0.17f, 0.10f, 1.00f);
        s_themeColors.tableBorderLight  = ImVec4(0.25f, 0.22f, 0.15f, 0.40f);
        s_themeColors.tableRowBgAlt = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
        s_themeColors.rowHoverBg    = ImVec4(0.340f, 0.300f, 0.220f, 0.90f);
        s_themeColors.rowSelectedBg = ImVec4(0.878f, 0.710f, 0.388f, 0.22f);
        s_themeColors.splitterIdle  = IM_COL32(46, 40, 30, 255);
        s_themeColors.splitterActive = IM_COL32(196, 169, 106, 255);
        s_themeColors.splitterHover = IM_COL32(196, 169, 106, 140);
        s_themeColors.detailPanelBg = ImVec4(0.07f, 0.065f, 0.05f, 0.90f);
        s_themeColors.replayBtnBg   = ImVec4(0.09f, 0.08f, 0.06f, 1.0f);
        s_themeColors.replayBtnHov  = ImVec4(0.16f, 0.14f, 0.10f, 1.0f);
        s_themeColors.replayBtnAct  = ImVec4(0.12f, 0.10f, 0.07f, 1.0f);
        s_themeColors.compTextCol   = ImVec4(0.65f, 0.58f, 0.42f, 1.f);
        s_themeColors.cardBg        = ImVec4(0.09f, 0.08f, 0.06f, 1.0f);
        s_themeColors.cardBgSel     = ImVec4(0.11f, 0.10f, 0.07f, 1.0f);
        s_themeColors.cardBorderSel = ImVec4(0.769f, 0.663f, 0.416f, 1.0f);
        s_themeColors.cardBorderIdle = ImVec4(0.18f, 0.15f, 0.10f, 1.0f);
        s_themeColors.cardGradientBot  = IM_COL32(10, 9, 7, 240);
        s_themeColors.cardGradientMid  = IM_COL32(10, 9, 7, 0);
        s_themeColors.cardHoverBorder  = IM_COL32(196, 169, 106, 180);
        s_themeColors.cardFallbackBg   = IM_COL32(25, 22, 18, 255);
    }
}

// Vertical splitter (drag left/right to resize columns). Returns true while dragging.
bool VSplitter(const char* id, float height, float thickness)
{
    ImGui::SameLine(0, 0);
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(thickness, height));
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();

    ImU32 col = s_themeColors.splitterIdle;
    if (active)       col = s_themeColors.splitterActive;
    else if (hovered) col = s_themeColors.splitterHover;

    float lineX = cursor.x + thickness * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(lineX, cursor.y + 4.0f),
        ImVec2(lineX, cursor.y + height - 4.0f),
        col, 2.0f);

    if (hovered || active)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    ImGui::SameLine(0, 0);
    return active;
}

// Horizontal splitter (drag up/down to resize rows). Returns true while dragging.
bool HSplitter(const char* id, float width, float thickness)
{
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, thickness));
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();

    ImU32 col = s_themeColors.splitterIdle;
    if (active)       col = s_themeColors.splitterActive;
    else if (hovered) col = s_themeColors.splitterHover;

    float lineY = cursor.y + thickness * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(cursor.x + 8.0f, lineY),
        ImVec2(cursor.x + width - 8.0f, lineY),
        col, 2.0f);

    if (hovered || active)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    return active;
}

int PushGlassTheme()
{
    int count = 0;
    auto Push = [&](ImGuiCol idx, const ImVec4& col) { ImGui::PushStyleColor(idx, col); count++; };

    Push(ImGuiCol_WindowBg,           kColorBg);
    Push(ImGuiCol_ChildBg,            kColorPanel);
    Push(ImGuiCol_PopupBg,            s_themeColors.popupBg);
    Push(ImGuiCol_Border,             kColorBorder);
    Push(ImGuiCol_FrameBg,            s_themeColors.frameBg);
    Push(ImGuiCol_FrameBgHovered,     s_themeColors.frameBgHov);
    Push(ImGuiCol_FrameBgActive,      s_themeColors.frameBgAct);
    Push(ImGuiCol_TitleBg,            kColorBg);
    Push(ImGuiCol_TitleBgActive,      s_themeColors.titleBgAct);
    Push(ImGuiCol_ScrollbarBg,        s_themeColors.scrollBg);
    Push(ImGuiCol_ScrollbarGrab,      s_themeColors.scrollGrab);
    Push(ImGuiCol_ScrollbarGrabHovered, s_themeColors.scrollGrabHov);
    Push(ImGuiCol_ScrollbarGrabActive,  kColorAccentDim);
    Push(ImGuiCol_Header,             kColorSelected);
    Push(ImGuiCol_HeaderHovered,      kColorHover);
    Push(ImGuiCol_HeaderActive,       kColorSelected);
    Push(ImGuiCol_Button,             s_themeColors.button);
    Push(ImGuiCol_ButtonHovered,      s_themeColors.buttonHov);
    Push(ImGuiCol_ButtonActive,       kColorAccentDim);
    Push(ImGuiCol_Separator,          kColorBorder);
    Push(ImGuiCol_Text,               kColorText);
    Push(ImGuiCol_TextDisabled,       kColorTextDim);
    Push(ImGuiCol_TableHeaderBg,      s_themeColors.tableHeaderBg);
    Push(ImGuiCol_TableBorderStrong,  s_themeColors.tableBorderStrong);
    Push(ImGuiCol_TableBorderLight,   s_themeColors.tableBorderLight);
    Push(ImGuiCol_TableRowBg,         ImVec4(0.00f, 0.00f, 0.00f, 0.00f));
    Push(ImGuiCol_TableRowBgAlt,      s_themeColors.tableRowBgAlt);

    return count;
}

void PopGlassTheme(int count)
{
    ImGui::PopStyleColor(count);
}

// A slow breathing value in [0,1], for drawing the eye to a control without moving it.
// Driven off the shared clock so every pulsing control beats together rather than drifting
// apart, which is what makes two of them read as one deliberate cue.
float BrowserPulse()
{
    constexpr float kPeriodSeconds = 1.9f;
    return 0.5f + 0.5f * sinf((float)ImGui::GetTime() * (2.0f * IM_PI / kPeriodSeconds));
}

// A soft halo just outside mn..mx, brightest at the pulse peak. Three rings with a falling
// alpha rather than one, so it reads as a glow instead of a second hard border.
void DrawPulseGlow(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
                          ImU32 rgb, float rounding, float strength)
{
    const float p = BrowserPulse();
    for (int i = 0; i < 3; i++)
    {
        const float grow = 1.0f + i * 2.0f;
        const float a = (0.40f - i * 0.11f) * p * strength;
        if (a <= 0.0f) continue;
        const ImU32 col = (rgb & ~IM_COL32_A_MASK)
                        | ((ImU32)(a * 255.0f) << IM_COL32_A_SHIFT);
        dl->AddRect(ImVec2(mn.x - grow, mn.y - grow), ImVec2(mx.x + grow, mx.y + grow),
                    col, rounding + grow, 0, 1.5f);
    }
}

void DrawPillBadge(ImDrawList* dl, ImFont* font, float fontSize,
                          const char* text, ImVec2 pos, ImU32 textCol,
                          ImU32 bgCol, float rounding)
{
    ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
    float padX = 6.0f, padY = 2.0f;
    ImVec2 mn(pos.x, pos.y);
    ImVec2 mx(pos.x + textSz.x + padX * 2, pos.y + textSz.y + padY * 2);
    dl->AddRectFilled(mn, mx, bgCol, rounding);
    dl->AddText(font, fontSize, ImVec2(pos.x + padX, pos.y + padY), textCol, text);
}

std::string ToLower(const std::string& s)
{
    std::string r = s;
    for (auto& c : r) c = (char)tolower((unsigned char)c);
    return r;
}

bool FuzzyMatch(const std::string& query, const std::string& target)
{
    std::string qLow = ToLower(query);
    std::string tLow = ToLower(target);
    size_t qi = 0;
    for (size_t ti = 0; ti < tLow.size() && qi < qLow.size(); ti++)
        if (tLow[ti] == qLow[qi]) qi++;
    return qi == qLow.size();
}

std::string TruncateToWidth(const std::string& text, float maxW)
{
    if (maxW <= 0.0f) return std::string();
    if (ImGui::CalcTextSize(text.c_str()).x <= maxW) return text;

    const float ellW = ImGui::CalcTextSize("...").x;
    size_t n = text.size();
    while (n > 0 &&
           ImGui::CalcTextSize(text.c_str(), text.c_str() + n).x + ellW > maxW)
        n--;
    return text.substr(0, n) + "...";
}

bool ComboFromVec(const char* label, int& current, const std::vector<std::string>& items)
{
    if (items.empty()) return false;
    if (current >= (int)items.size()) current = 0;
    const char* preview = items[current].c_str();

    bool changed = false;
    if (ImGui::BeginCombo(label, preview))
    {
        for (int i = 0; i < (int)items.size(); i++)
        {
            bool selected = (i == current);
            if (ImGui::Selectable(items[i].c_str(), selected))
            {
                current = i;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Make the "browse all" arrow button read as part of its adjoining search
// field (same frame colors) instead of the generic, mismatched button chrome
// it would otherwise inherit.
void PushDropdownArrowStyle()
{
    ImGui::PushStyleColor(ImGuiCol_Button,        s_themeColors.frameBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, s_themeColors.frameBgHov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  s_themeColors.frameBgAct);
    ImGui::PushStyleColor(ImGuiCol_Text,          kColorTextDim);
}

void PopDropdownArrowStyle()
{
    ImGui::PopStyleColor(4);
}

bool HighlightedSelectable(const char* text, const char* query,
                                  const char* uid, bool fuzzy)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float lineH = ImGui::GetTextLineHeightWithSpacing();

    std::string hiddenId = std::string("##hl_") + uid;
    bool clicked = ImGui::Selectable(hiddenId.c_str(), false);

    float x = pos.x + 4.0f;
    float y = pos.y + (lineH - ImGui::GetTextLineHeight()) * 0.5f;
    ImU32 colNorm = ImGui::GetColorU32(kColorText);
    ImU32 colHL   = ImGui::GetColorU32(kColorAccent);

    if (!query || query[0] == '\0')
    {
        dl->AddText(ImVec2(x, y), colNorm, text);
        return clicked;
    }

    if (fuzzy)
    {
        std::string qLow = ToLower(query);
        std::string tLow = ToLower(text);
        size_t qi = 0;
        for (size_t ti = 0; ti < tLow.size(); ti++)
        {
            bool m = (qi < qLow.size() && tLow[ti] == qLow[qi]);
            if (m) qi++;
            char c[2] = { text[ti], '\0' };
            dl->AddText(ImVec2(x, y), m ? colHL : colNorm, c);
            x += ImGui::CalcTextSize(c).x;
        }
    }
    else
    {
        std::string tLow = ToLower(text);
        std::string qLow = ToLower(query);
        size_t mpos = tLow.find(qLow);
        if (mpos == std::string::npos)
        {
            dl->AddText(ImVec2(x, y), colNorm, text);
        }
        else
        {
            size_t qLen = strlen(query);
            if (mpos > 0)
            {
                dl->AddText(ImVec2(x, y), colNorm, text, text + mpos);
                x += ImGui::CalcTextSize(text, text + mpos).x;
            }
            dl->AddText(ImVec2(x, y), colHL, text + mpos, text + mpos + qLen);
            x += ImGui::CalcTextSize(text + mpos, text + mpos + qLen).x;
            if (text[mpos + qLen] != '\0')
                dl->AddText(ImVec2(x, y), colNorm, text + mpos + qLen);
        }
    }
    return clicked;
}

LayoutMode ComputeLayout(float windowWidth)
{
    if (windowWidth > 1600.0f) return LayoutMode::Full;
    if (windowWidth > 1200.0f) return LayoutMode::Compact;
    if (windowWidth > 800.0f)  return LayoutMode::Narrow;
    return LayoutMode::Mobile;
}

ResponsiveSizes GetSizes(LayoutMode mode)
{
    switch (mode)
    {
    case LayoutMode::Full:    return { 22.0f, 36.0f, 14.0f, 140.0f, 8.0f };
    case LayoutMode::Compact: return { 20.0f, 30.0f, 13.0f, 120.0f, 8.0f };
    case LayoutMode::Narrow:  return { 18.0f, 26.0f, 12.0f, 100.0f, 8.0f };
    case LayoutMode::Mobile:  return { 18.0f, 24.0f, 12.0f, 80.0f,  8.0f };
    }
    return { 22.0f, 36.0f, 14.0f, 140.0f, 8.0f };
}

const char* GetProfessionIconFile(int id)
{
    switch (id)
    {
    case 1:  return "[1] - Warrior.png";
    case 2:  return "[2] - Ranger.png";
    case 3:  return "[3] - Monk.png";
    case 4:  return "[4] - Necromancer.png";
    case 5:  return "[5] - Mesmer.png";
    case 6:  return "[6] - Elementalist.png";
    case 7:  return "[7] - Assassin.png";
    case 8:  return "[8] - Ritualist.png";
    case 9:  return "[9] - Paragon.png";
    case 10: return "[10] - Dervish.png";
    default: return nullptr;
    }
}

static std::string g_textureBasePath;

const std::string& TextureBasePath()
{
    EnsureTextureBasePath();
    return g_textureBasePath;
}

void EnsureTextureBasePath()
{
    if (!g_textureBasePath.empty()) return;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Walk up from exe directory to find the Textures folder.
    // Exe is typically at <project>/x64/Release/GuildWarsObserver.exe
    // while Textures lives at <project>/Textures/
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures"))
        {
            g_textureBasePath = dir.string();
            return;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    // Fallback to exe directory
    g_textureBasePath = std::filesystem::path(exePath).parent_path().string();
}

ImTextureID GetProfessionIcon(int profId)
{
    const char* file = GetProfessionIconFile(profId);
    if (!file) return nullptr;
    EnsureTextureBasePath();
    std::string path = g_textureBasePath + "\\Textures\\Professions_Icons\\" + file;
    return GetTextureCache().GetTexture(path);
}

// =================================================================================================
// NEW WIDGETS
//
// Everything below is new in WP1 and reads its colours out of the tokens above. Nothing here
// invents a colour: a screen built out of these parts follows a theme change for free.
// =================================================================================================

// ─── Buttons ─────────────────────────────────────────────────────────────────

bool SegmentedButton(const char* label, bool active, const ImVec4& accent,
                     const ImVec2& framePadding)
{
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(accent.x, accent.y, accent.z, 0.15f));
        ImGui::PushStyleColor(ImGuiCol_Text, accent);
        ImGui::PushStyleColor(ImGuiCol_Border, accent);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.15f, 0.7f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.56f, 0.63f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, framePadding);
    bool clicked = ImGui::Button(label);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    return clicked;
}

// The amber fill needs DARK text on it, not the body colour: #f59e0b against #e4e4e7 is not a
// readable pair, and the one primary action on a screen is the last place to be subtle.
bool PrimaryButton(const char* label, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, kColorAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.984f, 0.749f, 0.141f, 1.0f)); // #fbbf24
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.851f, 0.541f, 0.024f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.094f, 0.094f, 0.106f, 1.0f));           // #18181b
    ImGui::PushStyleColor(ImGuiCol_Border, kColorAccent);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 5));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    return clicked;
}

bool SecondaryButton(const char* label, const ImVec2& size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kColorHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kColorSelected);
    ImGui::PushStyleColor(ImGuiCol_Text, kColorText);
    ImGui::PushStyleColor(ImGuiCol_Border, kColorBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 4));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    return clicked;
}

namespace
{
// The label IconTextButton actually hands to ImGui: the caller's words with the glyph's width
// measured out in front of them in spaces, so that ImGui lays a button with an icon out exactly
// like one without.
std::string GlyphPaddedLabel(const char* label)
{
    const float glyphBox = ImGui::GetTextLineHeight();
    const float gap = 6.0f;
    const float spaceW = ImGui::CalcTextSize(" ").x;
    const int spaces = spaceW > 0.0f ? (int)ceilf((glyphBox + gap) / spaceW) : 3;
    std::string padded((size_t)spaces, ' ');
    padded += label ? label : "";
    return padded;
}
} // namespace

float IconTextButtonWidth(const char* label, bool primary)
{
    const float padX = primary ? 12.0f : 10.0f;
    return ImGui::CalcTextSize(GlyphPaddedLabel(label).c_str()).x + padX * 2.0f;
}

bool IconTextButton(const char* label, const GlyphDrawer& glyph, const ImVec2& size, bool primary)
{
    // The glyph is drawn OVER the button after the fact rather than baked into the label, so it
    // takes the button's own text colour in whatever state the button ended up in, and so a button
    // with an icon is laid out by ImGui exactly like one without.
    const float glyphBox = ImGui::GetTextLineHeight();

    // Reserve the glyph's width by padding the label. The spaces are measured, not guessed.
    const std::string padded = GlyphPaddedLabel(label);

    bool clicked = primary ? PrimaryButton(padded.c_str(), size)
                           : SecondaryButton(padded.c_str(), size);

    if (glyph)
    {
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        const float padX = (primary ? 12.0f : 10.0f);
        const ImVec2 center(mn.x + padX + glyphBox * 0.5f, (mn.y + mx.y) * 0.5f);
        const ImU32 col = ImGui::GetColorU32(primary ? ImVec4(0.094f, 0.094f, 0.106f, 1.0f)
                                                     : kColorText);
        glyph(ImGui::GetWindowDrawList(), center, glyphBox, col);
    }
    return clicked;
}

void DrawHangerGlyph(ImDrawList* dl, const ImVec2& center, float size, ImU32 col)
{
    // A hanger is three strokes: the hook, the two shoulders, and the bar they hang from. Drawn
    // rather than loaded, because the texture set has no hanger in it.
    const float w = size * 0.92f;          // bar width
    const float h = size * 0.80f;          // hook tip to bar
    const float thickness = (std::max)(1.0f, size * 0.09f);

    const float barY = center.y + h * 0.42f;
    const float apexY = center.y - h * 0.18f;
    const float hookTopY = center.y - h * 0.58f;

    // Hook: a small open curve leaning right off the apex.
    dl->PathClear();
    dl->PathLineTo(ImVec2(center.x, apexY));
    dl->PathArcTo(ImVec2(center.x + size * 0.13f, hookTopY + size * 0.10f),
                  size * 0.15f, IM_PI * 0.9f, IM_PI * 2.15f, 12);
    dl->PathStroke(col, 0, thickness);

    // Shoulders down to the bar's ends.
    dl->AddLine(ImVec2(center.x, apexY), ImVec2(center.x - w * 0.5f, barY), col, thickness);
    dl->AddLine(ImVec2(center.x, apexY), ImVec2(center.x + w * 0.5f, barY), col, thickness);

    // The bar.
    dl->AddLine(ImVec2(center.x - w * 0.5f, barY), ImVec2(center.x + w * 0.5f, barY),
                col, thickness);
}

// ─── Labels ──────────────────────────────────────────────────────────────────

void SectionLabel(const char* text)
{
    ImFont* font = GuiGlobalConstants::boldFont ? GuiGlobalConstants::boldFont : ImGui::GetFont();
    const float px = 15.0f;
    const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(sz.x, sz.y));
    ImGui::GetWindowDrawList()->AddText(font, px, pos, ImGui::GetColorU32(kColorAccent), text);
}

void ScreenTitle(const char* text)
{
    ImFont* font = GuiGlobalConstants::boldFont ? GuiGlobalConstants::boldFont : ImGui::GetFont();
    const float px = 22.0f;
    const ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(sz.x, sz.y));
    ImGui::GetWindowDrawList()->AddText(font, px, pos, ImGui::GetColorU32(kColorText), text);
}

// ─── Display type ────────────────────────────────────────────────────────────

ImFont* DisplayFont(float px, int family)
{
    ImFont* best_above = nullptr;
    float best_above_px = 0.0f;
    ImFont* largest = nullptr;
    float largest_px = 0.0f;
    const bool interface_family = (family == kDisplayUi || family == kDisplayUiBold);
    for (int pass = 0; pass < 2 && best_above == nullptr && largest == nullptr; pass++)
    {
        // A family that was not baked (its file is missing, or the font has no bold) answers from
        // its regular: the serif for the display faces, the interface font's regular for its bold.
        // The interface font's regular has nothing further to fall back to in the ladder.
        int want = family;
        if (pass == 1)
        {
            if (family == kDisplayUi)
                break;
            want = interface_family ? static_cast<int>(kDisplayUi) : static_cast<int>(kDisplaySerif);
        }
        for (int i = 0; i < GuiGlobalConstants::display_font_count; i++)
        {
            const DisplayFontRung& rung = GuiGlobalConstants::display_fonts[i];
            if (rung.font == nullptr || rung.family != want)
                continue;
            // Three percent of slack: a rung that small an enlargement away is sharper than the
            // next one up shrunk by a sixth.
            if (rung.px >= px * 0.97f && (best_above == nullptr || rung.px < best_above_px))
            {
                best_above = rung.font;
                best_above_px = rung.px;
            }
            if (largest == nullptr || rung.px > largest_px)
            {
                largest = rung.font;
                largest_px = rung.px;
            }
        }
    }
    if (best_above != nullptr) return best_above;
    if (largest != nullptr) return largest;
    if ((family == kDisplaySerifBold || family == kDisplayUiBold) && GuiGlobalConstants::boldFont != nullptr)
        return GuiGlobalConstants::boldFont;
    // The interface font itself: the atlas's first font, whatever is pushed at the moment.
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (interface_family && atlas != nullptr && atlas->Fonts.Size > 0)
        return atlas->Fonts[0];
    return ImGui::GetFont();
}

// ─── Inputs ──────────────────────────────────────────────────────────────────

bool SearchBox(const char* id, const char* hint, char* buf, size_t bufSize, float width)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, s_themeColors.frameBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, s_themeColors.frameBgHov);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, s_themeColors.frameBgAct);
    if (width > 0.0f) ImGui::SetNextItemWidth(width);
    else              ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    bool changed = ImGui::InputTextWithHint(id, hint, buf, bufSize);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    return changed;
}

// ─── Cards ───────────────────────────────────────────────────────────────────

namespace
{
// BeginCard/EndCard nest the way BeginChild does, so the hover decision taken on the way in has to
// survive until the way out. One entry per open card.
struct CardFrameState
{
    ImVec2 mn, mx;
    bool selected;
    bool hovered;
};
std::vector<CardFrameState> s_openCards;
} // namespace

bool BeginCard(const char* id, const ImVec2& size, bool selected, bool navFlattened)
{
    const ImVec2 mn = ImGui::GetCursorScreenPos();
    const ImVec2 mx(mn.x + size.x, mn.y + size.y);

    // Hovering is decided BEFORE the child exists, against the rect the child is about to occupy.
    // Asking the child afterwards would answer for its content, not for the card, and a card whose
    // whole surface is a click target has to light up from its whole surface.
    const bool hovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        ImGui::IsMouseHoveringRect(mn, mx);

    ImVec4 fill(0.110f, 0.110f, 0.122f, 1.0f);   // #1c1c1f, the gallery card's own body
    if (selected)
    {
        fill.x = fill.x * 0.92f + kColorAccent.x * 0.08f;
        fill.y = fill.y * 0.92f + kColorAccent.y * 0.08f;
        fill.z = fill.z * 0.92f + kColorAccent.z * 0.08f;
    }
    else if (hovered)
    {
        // Amber at 6 percent over the body, as the design fixes it.
        fill.x = fill.x * 0.94f + kColorAccent.x * 0.06f;
        fill.y = fill.y * 0.94f + kColorAccent.y * 0.06f;
        fill.z = fill.z * 0.94f + kColorAccent.z * 0.06f;
    }

    const ImVec4 border = selected ? s_themeColors.cardBorderSel
                        : hovered  ? ImGui::ColorConvertU32ToFloat4(s_themeColors.cardHoverBorder)
                                   : s_themeColors.cardBorderIdle;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, fill);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);

    s_openCards.push_back({ mn, mx, selected, hovered });

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (navFlattened) flags |= ImGuiWindowFlags_NavFlattened;
    return ImGui::BeginChild(id, size, ImGuiChildFlags_Border, flags);
}

void EndCard()
{
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    if (!s_openCards.empty()) s_openCards.pop_back();
}

void FocusRing(const ImVec2& mn, const ImVec2& mx, float rounding)
{
    // Only while the keyboard is driving. NavVisible goes false the moment the pointer moves, so
    // a ring never outlives the keystroke that put it there.
    if (!ImGui::GetIO().NavVisible || !ImGui::IsItemFocused())
        return;
    const float grow = 2.0f;
    ImGui::GetWindowDrawList()->AddRect(ImVec2(mn.x - grow, mn.y - grow),
                                        ImVec2(mx.x + grow, mx.y + grow),
                                        ImGui::GetColorU32(kColorAccent), rounding + grow, 0,
                                        2.0f);
}

bool FavouriteStar(const char* id, bool& on, float radius)
{
    const float box = radius * 2.0f + 6.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(box, box));
    // IsItemActivated(), not IsItemClicked(): the first also answers for the KEYBOARD, which
    // activates an item without ever pressing a mouse button over it. Both fire once, on the
    // press, and a star drawn over a card's own click target still gets its press because the
    // card allows the overlap.
    const bool clicked = ImGui::IsItemActivated();
    const bool hovered = ImGui::IsItemHovered();
    if (clicked) on = !on;
    FocusRing(pos, ImVec2(pos.x + box, pos.y + box), 4.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = pos.x + box * 0.5f;
    const float cy = pos.y + box * 0.5f;
    if (on)
        DrawStarFilled(dl, cx, cy, radius, ImGui::GetColorU32(kColorAccent));
    else
        DrawStarOutline(dl, cx, cy, radius,
                        ImGui::GetColorU32(hovered ? kColorAccent : kColorTextDim));
    return clicked;
}

} // namespace ui
