#pragma once

// =================================================================================================
// THE SHARED UI KIT
//
// One home for the look of this application. Everything in here was a `static` inside
// draw_replay_browser.cpp until the Wardrobe needed the same widgets: the palette tokens, the
// theme push, the splitters, the responsive breakpoints, the little drawing helpers. The move is
// mechanical - the bodies are the browser's own, unchanged - so the browser still renders exactly
// what it rendered before, and a second screen can now be built out of the same parts instead of
// copying their styles.
//
// WHY A KIT AND NOT A SECOND COPY. Two screens that copy each other's colours drift apart on the
// first theme change. Reading every colour through `ui::kColor*` and `ui::s_themeColors` means a
// future theme carries every screen with it, which is the whole point of ApplyBrowserTheme setting
// them in one place.
//
// The kit is PUBLIC on purpose: the replay browser lives in the public tree and uses it. Private
// screens include it; nothing public ever includes a private header.
// =================================================================================================

#include "imgui.h"

#include <functional>
#include <string>
#include <vector>

namespace ui
{
// ─── Themeable colour palette ────────────────────────────────────────────────
//
// Set by ApplyBrowserTheme and read everywhere. They are variables rather than constants because
// the theme is chosen at run time.

extern ImVec4 kColorBg, kColorPanel, kColorPanelLight, kColorBorder;
extern ImVec4 kColorAccent, kColorAccentDim;
extern ImVec4 kColorText, kColorTextDim;
// Between the two: for the list's supporting columns, which should sit back from the guild
// names without receding as far as a disabled label.
extern ImVec4 kColorTextMuted;
extern ImVec4 kColorSelected, kColorHover;

// Card gallery visual hierarchy
extern ImU32 kCardMapName, kCardDate, kCardDuration;
extern ImU32 kCardGuildName, kCardGuildTag, kCardVS;
extern ImU32 kCardTeamLabel, kCardProfSig, kCardBuildName, kCardViewDetails;
extern ImU32 kCardMatchupBg, kCardMatchupRule;

// Colour interpolation helper
ImU32 LerpColor(ImU32 a, ImU32 b, float t);

// Theme-specific inline colors for PushGlassTheme
struct BrowserThemeColors
{
    ImVec4 popupBg, frameBg, frameBgHov, frameBgAct;
    ImVec4 titleBgAct, scrollBg, scrollGrab, scrollGrabHov;
    ImVec4 button, buttonHov;
    ImVec4 tableHeaderBg, tableBorderStrong, tableBorderLight, tableRowBgAlt;
    ImVec4 rowHoverBg, rowSelectedBg;
    ImU32 splitterIdle, splitterActive, splitterHover;
    ImVec4 detailPanelBg;
    ImVec4 replayBtnBg, replayBtnHov, replayBtnAct;
    ImVec4 compTextCol;
    ImVec4 cardBg, cardBgSel, cardBorderSel, cardBorderIdle;
    // Netflix card
    ImU32 cardGradientBot, cardGradientMid;
    ImU32 cardHoverBorder;
    ImU32 cardFallbackBg;
};
extern BrowserThemeColors s_themeColors;

// Theme 1 is "Watchtower" (zinc + amber), theme 0 the original warm gold. Cheap to call every
// frame: it returns at once when the theme has not moved.
void ApplyBrowserTheme(int theme);

int  PushGlassTheme();
void PopGlassTheme(int count);

// ─── Splitters ───────────────────────────────────────────────────────────────

bool VSplitter(const char* id, float height, float thickness = 6.0f);
bool HSplitter(const char* id, float width, float thickness = 6.0f);

// ─── Attention helpers ───────────────────────────────────────────────────────

// A slow breathing value in [0,1], for drawing the eye to a control without moving it.
float BrowserPulse();

// A soft halo just outside mn..mx, brightest at the pulse peak.
void DrawPulseGlow(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx,
                   ImU32 rgb, float rounding, float strength = 1.0f);

void DrawPillBadge(ImDrawList* dl, ImFont* font, float fontSize,
                   const char* text, ImVec2 pos, ImU32 textCol,
                   ImU32 bgCol = IM_COL32(255, 255, 255, 25), float rounding = 10.f);

// ─── Text and search helpers ─────────────────────────────────────────────────

std::string ToLower(const std::string& s);
bool FuzzyMatch(const std::string& query, const std::string& target);
std::string TruncateToWidth(const std::string& text, float maxW);

bool ComboFromVec(const char* label, int& current, const std::vector<std::string>& items);

// Make the "browse all" arrow button read as part of its adjoining search field.
void PushDropdownArrowStyle();
void PopDropdownArrowStyle();

bool HighlightedSelectable(const char* text, const char* query, const char* uid, bool fuzzy);

// ─── Responsive layout system ────────────────────────────────────────────────

enum class LayoutMode { Full, Compact, Narrow, Mobile };

LayoutMode ComputeLayout(float windowWidth);

struct ResponsiveSizes
{
    float profIcon;
    float skillIcon;
    float cupIcon;
    float mapImg;
    float spacing;      // base spacing unit (multiples of 8)
};

ResponsiveSizes GetSizes(LayoutMode mode);

// ─── Profession icons ────────────────────────────────────────────────────────

const char* GetProfessionIconFile(int id);
// Resolves <project>/Textures once per run by walking up from the exe.
void EnsureTextureBasePath();
const std::string& TextureBasePath();
ImTextureID GetProfessionIcon(int profId);

// ─── Buttons ─────────────────────────────────────────────────────────────────

// One of a row of mutually exclusive view buttons ("Table | Cards | Scout"): outlined always,
// tinted with `accent` when it is the active one. The default accent is the browser's blue, so
// the gallery's own row is unchanged by the move.
bool SegmentedButton(const char* label, bool active,
                     const ImVec4& accent = ImVec4(0.431f, 0.659f, 0.996f, 1.0f),
                     const ImVec2& framePadding = ImVec2(8, 3));

// The one amber action on a screen: filled accent, dark text, 4 px rounding.
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// Everything else: outlined in kColorBorder, text in kColorText.
bool SecondaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// A secondary (or primary) button with a small glyph drawn to the left of its label. The callback
// is handed a draw list, the glyph's centre, its box size and the button's text colour, so a glyph
// costs no texture and inherits the button's state colour.
using GlyphDrawer = std::function<void(ImDrawList*, const ImVec2& center, float size, ImU32 col)>;
bool IconTextButton(const char* label, const GlyphDrawer& glyph,
                    const ImVec2& size = ImVec2(0, 0), bool primary = false);

// What IconTextButton will measure, so that a caller can right-align the button before drawing it.
float IconTextButtonWidth(const char* label, bool primary = false);

// A clothes hanger: hook, shoulders, bar. There is no hanger in the texture set, so it is drawn.
void DrawHangerGlyph(ImDrawList* dl, const ImVec2& center, float size, ImU32 col);

// ─── Labels ──────────────────────────────────────────────────────────────────

// 15 px bold accent caps, the "MATCHES" of the gallery. The text is used as given; write it in
// capitals at the call site.
void SectionLabel(const char* text);

// 22 px bold body text, the one size the Wardrobe adds ("Your characters").
void ScreenTitle(const char* text);

// ─── Display type ────────────────────────────────────────────────────────────

// The baked font for text drawn at `px` in one DisplayFontFamily (GuiGlobalConstants.h): the
// smallest rung at least that large, else the largest, so large type is shrunk a little rather
// than blown up. Draw with ImDrawList::AddText(font, px, ...) at the exact size; a widget that
// cannot take a size uses the font as pushed. A family that was not baked answers from its
// nearest relative: a bold from its regular, the small caps from the serif, the interface
// font's ladder from the interface font itself. Never null: with no ladder baked (missing font
// files) it answers the interface font, bold for the bold families.
ImFont* DisplayFont(float px, int family = 0);

// ─── Inputs ──────────────────────────────────────────────────────────────────

// The browser's search field: 4 px frame, hint text, optional fixed width (0 = fill).
bool SearchBox(const char* id, const char* hint, char* buf, size_t bufSize, float width = 0.0f);

// ─── Cards ───────────────────────────────────────────────────────────────────

// The gallery card's frame, generalised: #1c1c1f fill, 8 px rounding, idle or amber border, and an
// amber tint at 6 percent while the pointer is over it. Returns BeginChild's own answer; call
// EndCard() exactly once for every BeginCard(), as with BeginChild.
//
// `navFlattened` puts the card's CONTENTS into the surrounding screen's keyboard scope instead of
// making the card one item that has to be entered first. A grid of cards that the arrow keys walk
// through wants it; a card whose contents are not meant to be tabbed into does not, which is why
// it is off by default and the gallery is unchanged.
bool BeginCard(const char* id, const ImVec2& size, bool selected, bool navFlattened = false);
void EndCard();

// 
// THE KEYBOARD'S OWN RING. Dear ImGui draws a focus ring for every widget it paints itself, but
// the tiles, swatches and cards of this application are InvisibleButtons with their own
// painting - and an InvisibleButton paints nothing, so a keyboard user would move through a
// gallery with no idea where they were. Called straight after the item, with the box that was
// drawn for it, it puts a 2 px accent ring just outside that box - and only while the KEYBOARD is
// what is driving, so a mouse click never leaves a ring behind it.
void FocusRing(const ImVec2& mn, const ImVec2& mx, float rounding = 6.0f);

// One star as a toggle, not five. Draws filled amber when on, a dim outline when off, and flips
// `on` when clicked; returns true on that click.
bool FavouriteStar(const char* id, bool& on, float radius = 8.0f);

} // namespace ui
