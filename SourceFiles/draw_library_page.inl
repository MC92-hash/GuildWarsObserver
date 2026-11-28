// =================================================================================================
// THE LIBRARY PAGE  [2026-11-22, V41 2026-11-25]
//
// V41 (output/imagegen/library-calculate-v41.html, library-v41-implementation-spec.txt): the list and
// the sidebar are two glass panels, 10 px apart, over the selected hall's scene; the rows run Star |
// Date | Event | Guild 1 | vs | Guild 2 | Map | Duration; the open filter is a panel in the page's
// flow; the sidebar's lord damage is worked out from the recording on request (DrawLordDamage).
//
// The Library's main page as the owner's mockup draws it (output/imagegen/library-deep-blue-v11.html,
// its final charcoal-and-gold sheet): a title row with Update matches and Spoiler-free, the search
// with the All matches / Favorites switch, a row of filter menus, the match list with each team's
// eight elite skills, and a sidebar with the selected match's facts, Replay match and both teams'
// builds. Stats opens the match's Statistics page (draw_library_stats.inl, V48); Map the existing
// full-width details (DrawMatchDetailPanel). Neither fits a 450 px sidebar.
//
// Included by draw_replay_browser.cpp near its end, so this is the same translation unit: it reads
// the browser's state (s_state) and calls its helpers directly. The legacy page (the table, the card
// gallery, Scout) is not changed and is still drawn when GuiGlobalConstants::replay_library_legacy
// is set or Scout is open. Both pages share one state, so the filters, the search and the selection
// carry across a switch.
//
// The mockup is CSS. Its lengths are CSS pixels at a 15 px body, and here every length is that
// number times the Settings font size over 15 (Px). Type sizes are also multiplied by kEm: ImGui
// sizes a font by its ascent plus descent, CSS by its em, and Roboto's ascent plus descent is
// 1.172 em, so CSS 15 px Roboto is ImGui 17.6 px Roboto.
// =================================================================================================

// DEV TOOL (GWO_LIBRARY_SHOT_PAGE, MapBrowser.cpp): the match to select (by folder) and the filter
// menu to open ("-" closes it) for the page's pictures. Read once, then cleared.
std::string g_libraryPageTestSelect;
std::string g_libraryPageTestMenu;

#include "ReplayMapData.h"   // kHenchmen: the guild hall henchmen a party can fill its slots with

namespace libpage
{
// ─── Scale, type and colour ──────────────────────────────────────────────────

// The page's scale: the Settings font size over 15, times the page size, which follows the window
// (GuiGlobalConstants::LibraryPageScale): 1 from 1920 px wide up, down to 0.8 on a narrower window.
static float s_k = 1.0f;

static inline float Px(float css) { return css * s_k; }
constexpr float kAscent = 0.9277f;                   // Roboto: ascent / em
static inline float LineH(float cssPx) { return cssPx * kLibraryEm * s_k; }    // CSS line-height: normal
static inline float Ascent(float cssPx) { return cssPx * kAscent * s_k; }
static inline float R(float v) { return std::round(v); }

constexpr ImU32 Hex(unsigned rgb, unsigned a = 255)
{
    return ((ImU32)a << IM_COL32_A_SHIFT) | ((ImU32)((rgb >> 16) & 0xFF) << IM_COL32_R_SHIFT) |
           ((ImU32)((rgb >> 8) & 0xFF) << IM_COL32_G_SHIFT) | ((ImU32)(rgb & 0xFF) << IM_COL32_B_SHIFT);
}

static ImU32 WithAlpha(ImU32 c, float a)
{
    const unsigned alpha = (unsigned)std::clamp(((c >> IM_COL32_A_SHIFT) & 0xFF) * a, 0.0f, 255.0f);
    return (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

// The mockup's palette, its "current-library-colors" sheet.
namespace col
{
constexpr ImU32 Page          = Hex(0x191919);
constexpr ImU32 Text          = Hex(0xdedcd5);
constexpr ImU32 Small         = Hex(0x989387);
constexpr ImU32 SearchBg      = Hex(0x141413, 0xe8);
constexpr ImU32 SearchRule    = Hex(0x484337);
constexpr ImU32 SearchText    = Hex(0xa39c8c);
constexpr ImU32 ButtonBg      = Hex(0x25231e, 0xe8);
constexpr ImU32 ButtonHovBg   = Hex(0x2e2b24);
constexpr ImU32 ButtonText    = Hex(0xd6d1c5);
constexpr ImU32 ButtonActive  = Hex(0xcfb98b);
constexpr ImU32 ActionBg      = Hex(0x22211d, 0xe8);
constexpr ImU32 ActionText    = Hex(0xc9c2b5);
constexpr ImU32 ActionRule    = Hex(0x474033);
constexpr ImU32 ActionHovBg   = Hex(0x302c23);
constexpr ImU32 ActionHovRule = Hex(0x8b764a);
constexpr ImU32 ToggleState   = Hex(0x928a7a);
constexpr ImU32 ToggleOn      = Hex(0xe0bc70);
constexpr ImU32 ToggleOnRule  = Hex(0xa18547);
constexpr ImU32 ToggleOnBg    = Hex(0x302b1d);
constexpr ImU32 NewDot        = Hex(0xd5b469);
constexpr ImU32 Filters       = Hex(0xc5c0b5);
constexpr ImU32 FilterOn      = Hex(0xe3c47e);
constexpr ImU32 Sort          = Hex(0xd5b263);
constexpr ImU32 Labels        = Hex(0x9d9583);
constexpr ImU32 LabelsRule    = Hex(0x423d31);
constexpr ImU32 RowRule       = Hex(0x2e2c26);
constexpr ImU32 RowHover      = Hex(0xffffff, 7);
constexpr ImU32 SelA          = Hex(0x49402c);
constexpr ImU32 SelB          = Hex(0x363125);
constexpr ImU32 SelBar        = Hex(0xd6b363);
constexpr ImU32 Star          = Hex(0x8b8475);
constexpr ImU32 StarOn        = Hex(0xdfb85d);
constexpr ImU32 Versus        = Hex(0x8b8475);
constexpr ImU32 MapName       = Hex(0xc5c0b5);
constexpr ImU32 Muted         = Hex(0xa49c8c);   // dates, durations, the footer, the meta line
constexpr ImU32 HiddenText    = Hex(0x958972);
constexpr ImU32 Winner        = Hex(0xe4c782);
constexpr ImU32 CupBg         = Hex(0xb79949, 0x30);
constexpr ImU32 IconRule      = Hex(0xb39b64, 0x55);
constexpr ImU32 EmptySlot     = Hex(0x25231f);
constexpr ImU32 Comms         = Hex(0xbaa168);
constexpr ImU32 Aside         = Hex(0x13120f);
constexpr ImU32 AsideRule     = Hex(0x454033);
// The glass: the page under the art, the list's panel (clear behind the toolbar and the filters,
// nearly solid where the rows are), the sidebar's panel, the rows and the labels.
constexpr ImU32 PageDeep      = Hex(0x141413);
constexpr ImU32 Veil          = Hex(0x0c0e10);
constexpr ImU32 ListClear     = Hex(0x171716, 0x22);
constexpr ImU32 ListSolid     = Hex(0x191919, 0xd8);
constexpr ImU32 ListBorder    = Hex(0x5a5243, 0x55);
constexpr ImU32 AsideGlass    = Hex(0x13120f, 0xd8);
constexpr ImU32 AsideBorder   = Hex(0x514735, 0x88);
constexpr ImU32 RowBg         = Hex(0x191919, 0x60);
constexpr ImU32 LabelsBg      = Hex(0x181817, 0xc9);
// V41 (output/imagegen/library-v41-implementation-spec.txt): the panels, the rows, the sidebar.
namespace v41
{
constexpr ImU32 PanelLine     = Hex(0xffffff, 28);     // the panels' 1 px outline and their seam
constexpr ImU32 PanelSheen    = Hex(0xffffff, 18);     // inset top highlight, about 7%
constexpr ImU32 Wash0         = Hex(0x191919, 0x38);   // full-screen wash, 120deg
constexpr ImU32 Wash1         = Hex(0x191919, 0x70);
constexpr ImU32 Tint0         = Hex(0x242420, 0x55);   // panel tint at 0, 125, 170, 205 px down
constexpr ImU32 Tint1         = Hex(0x242420, 0x70);
constexpr ImU32 Tint2         = Hex(0x1d1d1b, 0xd4);
constexpr ImU32 Tint3         = Hex(0x1c1c1b, 0xdc);
constexpr ImU32 Row           = Hex(0x1c1c1b, 0x16);
constexpr ImU32 RowHover      = Hex(0x333129, 0x88);
constexpr ImU32 RowRule       = Hex(0xffffff, 12);
constexpr ImU32 SelA          = Hex(0x49402c, 0xe8);
constexpr ImU32 SelB          = Hex(0x363125, 0xdb);
constexpr ImU32 SelAccent     = Hex(0xc5aa70);
constexpr ImU32 GuildName     = Hex(0xcecac1);
constexpr ImU32 WinnerName    = Hex(0xfff9ec);
constexpr ImU32 ListTag       = Hex(0xb4ad9d);
constexpr ImU32 Date          = Hex(0xbab2a4);
constexpr ImU32 Event         = Hex(0xd7d2c7);
constexpr ImU32 EventBg       = Hex(0xffffff, 0x09);
constexpr ImU32 EventLine     = Hex(0xffffff, 0x18);
constexpr ImU32 MapName       = Hex(0xc4bfb4);
constexpr ImU32 Duration      = Hex(0xa9a294);
constexpr ImU32 Labels        = Hex(0xa49b89);
constexpr ImU32 Star          = Hex(0x968b76);
constexpr ImU32 StarOn        = Hex(0xd2b775);
constexpr ImU32 Versus        = Hex(0x8e887c);
constexpr ImU32 VersusLine    = Hex(0xffffff, 0x0e);
constexpr ImU32 Footer        = Hex(0xa29885);
constexpr ImU32 SearchBg      = Hex(0x151514, 0xa8);
constexpr ImU32 SearchLine    = Hex(0xffffff, 0x12);
constexpr ImU32 SearchText    = Hex(0xbfb9ae);
constexpr ImU32 BtnOnA        = Hex(0x38362f);
constexpr ImU32 BtnOnB        = Hex(0x2d2c27);
constexpr ImU32 BtnOnText     = Hex(0xe8dfcb);
constexpr ImU32 BtnOnLine     = Hex(0x8d7b50, 0x29);
constexpr ImU32 BtnOff        = Hex(0xffffff, 0x06);
constexpr ImU32 BtnOffLine    = Hex(0xffffff, 0x12);
constexpr ImU32 BtnOffText    = Hex(0xbfb8a9);
constexpr ImU32 ActA          = Hex(0x302e28);
constexpr ImU32 ActB          = Hex(0x25241f);
constexpr ImU32 ActLine       = Hex(0x675a3e, 0x55);
constexpr ImU32 ActText       = Hex(0xcbc2b0);
constexpr ImU32 NewDot        = Hex(0xc8ae72);
constexpr ImU32 Filters       = Hex(0xc5c0b5);
constexpr ImU32 MenuA         = Hex(0x302f29);
constexpr ImU32 MenuB         = Hex(0x262621);
constexpr ImU32 MenuLine      = Hex(0xffffff, 0x14);
constexpr ImU32 Meta          = Hex(0xafa38d);
constexpr ImU32 Title         = Hex(0xe8e1d1);
constexpr ImU32 FactLabel     = Hex(0xa69a84);
constexpr ImU32 FactValue     = Hex(0xd7cdb8);
constexpr ImU32 FactsBg       = Hex(0xffffff, 0x02);
constexpr ImU32 FactsLine     = Hex(0xffffff, 0x10);
constexpr ImU32 LordHead      = Hex(0xc4b9a0);
constexpr ImU32 LordStatus    = Hex(0xa69a83);
constexpr ImU32 LordValue     = Hex(0xe1dacb);
constexpr ImU32 TrackA        = Hex(0xffffff, 0x12);
constexpr ImU32 TrackB        = Hex(0xffffff, 0x06);
constexpr ImU32 BlueFillA     = Hex(0x547b9c);
constexpr ImU32 BlueFillB     = Hex(0x88b8df);
constexpr ImU32 RedFillA      = Hex(0x995f5a);
constexpr ImU32 RedFillB      = Hex(0xdb938c);
constexpr ImU32 CalcText      = Hex(0xe0ceb0);
constexpr ImU32 CalcBg        = Hex(0x27251f, 0xf5);
constexpr ImU32 CalcLine      = Hex(0x88754e);
constexpr ImU32 CalcCost      = Hex(0xafa28b);
constexpr ImU32 CalcHovBg     = Hex(0x373125);
constexpr ImU32 CalcHovLine   = Hex(0xc0a16a);
constexpr ImU32 CalcBusyBg    = Hex(0x29261f, 0xf5);
constexpr ImU32 CalcBusyText  = Hex(0xc6b898);
constexpr ImU32 VoiceIcon     = Hex(0xbca578);
constexpr ImU32 VoiceTitle    = Hex(0xcfc4ad);
constexpr ImU32 VoiceSub      = Hex(0xa1947d);
constexpr ImU32 DetailsText   = Hex(0xc1b397);
constexpr ImU32 DetailsBg     = Hex(0xffffff, 0x04);
constexpr ImU32 DetailsLine   = Hex(0xffffff, 0x14);
constexpr ImU32 ReplayA       = Hex(0xd2bb83);
constexpr ImU32 ReplayB       = Hex(0xb6a06c);
constexpr ImU32 ReplayLine    = Hex(0xddc48c, 0x55);
constexpr ImU32 ReplayText    = Hex(0x27251e);
constexpr ImU32 TabOn         = Hex(0xd5bc80);
constexpr ImU32 Tabs          = Hex(0xc5c0b5);
constexpr ImU32 BlueStripe    = Hex(0x79b4e6);
constexpr ImU32 BlueName      = Hex(0xc3e1fa);
constexpr ImU32 BlueWash      = Hex(0x669bd5, 0x15);
constexpr ImU32 RedStripe     = Hex(0xde8178);
constexpr ImU32 RedName       = Hex(0xf3c4bd);
constexpr ImU32 RedWash       = Hex(0xd36e66, 0x15);
constexpr ImU32 RosterTag     = Hex(0xbfb5a5);
constexpr ImU32 Player        = Hex(0xc6beae);
constexpr ImU32 HeroFade      = Hex(0x13120f);
}
constexpr ImU32 FactLabel     = Hex(0x968f81);
constexpr ImU32 FactValue     = Hex(0xd8d2c7);
constexpr ImU32 Link          = Hex(0xc9b27e);
constexpr ImU32 LinkHover     = Hex(0xe8cc8a);
constexpr ImU32 Rule          = Hex(0x403a2e);
constexpr ImU32 VoiceIcon     = Hex(0xbca46a);
constexpr ImU32 VoiceTitle    = Hex(0xd3c8ae);
constexpr ImU32 VoiceSub      = Hex(0x948b7a);
constexpr ImU32 Notice        = Hex(0xc8b788);
constexpr ImU32 ReplayA       = Hex(0xd1b26f);
constexpr ImU32 ReplayB       = Hex(0xbaa06a);
constexpr ImU32 ReplayHovA    = Hex(0xdcbf7e);
constexpr ImU32 ReplayHovB    = Hex(0xc6ac74);
constexpr ImU32 ReplayActB    = Hex(0xa98f5c);
constexpr ImU32 ReplayText    = Hex(0x18150e);
constexpr ImU32 Tabs          = Hex(0xc5c0b5);
constexpr ImU32 TabOn         = Hex(0xdfb85d);
constexpr ImU32 Tag           = Hex(0xaaa08c);
constexpr ImU32 WinnerLabel   = Hex(0xd9bd79);
constexpr ImU32 Player        = Hex(0xcbc5b8);
constexpr ImU32 Rating        = Hex(0xc5ae78);
constexpr ImU32 Share         = Hex(0xb8ad96);
constexpr ImU32 Menu          = Hex(0x1d1c19);
constexpr ImU32 Popover       = Hex(0x24211b);
constexpr ImU32 PopoverRule   = Hex(0x4c4231);
}
namespace v41 = col::v41;

struct Type
{
    ImFont* font;
    float size;
};

// A CSS type size, regular or bold: the font to draw it from and the size to draw it at. The
// page's own rung (LibraryFonts), else the interface font's ladder (the built-in font, which has no
// file to bake from, or a bake that failed).
static Type T(float cssPx, bool bold = false)
{
    const float px = cssPx * kLibraryEm * s_k;
    if (ImFont* font = LibraryFonts::Find(cssPx, bold)) return { font, font->FontSize };   // 1:1, never scaled
    return { ui::DisplayFont(px, bold ? kDisplayUiBold : kDisplayUi), px };
}

// The page's rungs carry Basic Latin, Latin-1 and the library's own other characters. Text with a
// character a rung lacks (Korean, Japanese or Chinese names, or something typed) is drawn from the
// interface font instead, which has the CJK merge.
static Type Fit(const Type& t, const char* s, const char* e)
{
    if (!e) e = s + strlen(s);
    for (const char* it = s; it < e;)
    {
        unsigned int cp = 0;
        const int n = ImTextCharFromUtf8(&cp, it, e);
        if (n <= 0) break;
        if (cp >= 0x20 && t.font->FindGlyphNoFallback(static_cast<ImWchar>(cp)) == nullptr)
            return { ImGui::GetFont(), t.size };
        it += n;
    }
    return t;
}

static float TextW(const Type& t, const char* s, const char* e = nullptr)
{
    const Type f = Fit(t, s, e);
    return f.font->CalcTextSizeA(f.size, FLT_MAX, 0.0f, s, e).x;
}

// AddText from a font of either atlas: the page's rungs live in their own, whose texture a draw
// list's AddText does not bind by itself.
static void AddGlyphs(ImDrawList* dl, const Type& f, ImVec2 p, ImU32 c, const char* s, const char* e)
{
    const ImTextureID tex = f.font->ContainerAtlas->TexID;
    const bool bind = tex != dl->_CmdHeader.TextureId;
    if (bind) dl->PushTextureID(tex);
    dl->AddText(f.font, f.size, ImVec2(R(p.x), R(p.y)), c, s, e);
    if (bind) dl->PopTextureID();
}

static void Text(ImDrawList* dl, const Type& t, ImVec2 p, ImU32 c, const char* s, const char* e = nullptr)
{
    AddGlyphs(dl, Fit(t, s, e), p, c, s, e);
}

// Text cut to maxW with an ellipsis, as CSS text-overflow cuts it. Returns the width drawn; a null
// draw list only measures.
static float TextFit(ImDrawList* dl, const Type& tIn, ImVec2 p, float maxW, ImU32 c, const char* s,
                     const char* e = nullptr)
{
    if (!e) e = s + strlen(s);
    const Type t = Fit(tIn, s, e);
    const float full = TextW(t, s, e);
    if (full <= maxW)
    {
        if (dl) Text(dl, t, p, c, s, e);
        return full;
    }
    static const char* const kEllipsis = "\xE2\x80\xA6";
    const float ellW = TextW(t, kEllipsis);
    if (maxW <= ellW) return 0.0f;
    const char* cut = s;
    float w = 0.0f;
    for (const char* it = s; it < e;)
    {
        unsigned int cp = 0;
        const int n = ImTextCharFromUtf8(&cp, it, e);
        if (n <= 0) break;
        const float cw = TextW(t, it, it + n);
        if (w + cw + ellW > maxW) break;
        w += cw;
        it += n;
        cut = it;
    }
    while (cut > s && cut[-1] == ' ')
    {
        cut--;
        w -= TextW(t, " ");
    }
    if (dl)
    {
        Text(dl, t, p, c, s, cut);
        Text(dl, t, ImVec2(p.x + w, p.y), c, kEllipsis);
    }
    return w + ellW;
}

// CSS letter-spacing: `tracking` after every character, the last one included. A null draw list
// only measures.
static float TextSpaced(ImDrawList* dl, const Type& t, ImVec2 p, ImU32 c, const char* s, float tracking)
{
    float x = p.x;
    for (const char* it = s; *it;)
    {
        unsigned int cp = 0;
        const int n = ImTextCharFromUtf8(&cp, it, nullptr);
        if (n <= 0) break;
        if (dl) AddGlyphs(dl, t, ImVec2(x, p.y), c, it, it + n);
        x += TextW(t, it, it + n) + tracking;
        it += n;
    }
    return x - p.x;
}

static void HRule(ImDrawList* dl, float x0, float x1, float y, ImU32 c)
{
    dl->AddRectFilled(ImVec2(R(x0), R(y)), ImVec2(R(x1), R(y) + 1.0f), c);
}

// A 1 px outline on whole pixels.
static void Outline(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float rounding)
{
    dl->AddRect(ImVec2(R(a.x) + 0.5f, R(a.y) + 0.5f), ImVec2(R(b.x) - 0.5f, R(b.y) - 0.5f), c, rounding, 0, 1.0f);
}

static std::string Thousands(size_t n)
{
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
}

// ─── The glyphs the interface font does not carry, drawn ─────────────────────

static void DrawChevronDown(ImDrawList* dl, ImVec2 c, float w, ImU32 col)   // ▾
{
    const float h = w * 0.58f;
    dl->AddTriangleFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y - h * 0.5f),
                          ImVec2(c.x, c.y + h * 0.5f), col);
}

static void DrawArrowVertical(ImDrawList* dl, ImVec2 c, float h, ImU32 col, bool down)   // ↓ ↑
{
    const float t = std::max(1.0f, Px(1.3f));
    const float s = down ? 1.0f : -1.0f;
    const ImVec2 tip(c.x, c.y + s * h * 0.5f);
    dl->AddLine(ImVec2(c.x, c.y - s * h * 0.5f), ImVec2(c.x, tip.y - s * 0.5f), col, t);
    dl->AddLine(ImVec2(c.x - h * 0.32f, tip.y - s * h * 0.32f), tip, col, t);
    dl->AddLine(ImVec2(c.x + h * 0.32f, tip.y - s * h * 0.32f), tip, col, t);
}

static void DrawArrowUpRight(ImDrawList* dl, ImVec2 c, float h, ImU32 col)   // ↗
{
    const float t = std::max(1.0f, Px(1.1f)), r = h * 0.38f;
    const ImVec2 tip(c.x + r, c.y - r);
    dl->AddLine(ImVec2(c.x - r, c.y + r), tip, col, t);
    dl->AddLine(ImVec2(tip.x - r * 1.1f, tip.y), tip, col, t);
    dl->AddLine(ImVec2(tip.x, tip.y + r * 1.1f), tip, col, t);
}

static void DrawMagnifier(ImDrawList* dl, ImVec2 c, float h, ImU32 col)   // ⌕
{
    const float r = h * 0.3f, t = std::max(1.0f, Px(1.3f));
    const ImVec2 lens(c.x + h * 0.06f, c.y - h * 0.08f);
    dl->AddCircle(lens, r, col, 20, t);
    dl->AddLine(ImVec2(lens.x - r * 0.72f, lens.y + r * 0.72f), ImVec2(lens.x - r * 1.75f, lens.y + r * 1.75f), col, t);
}

static void DrawPlay(ImDrawList* dl, ImVec2 c, float h, ImU32 col)   // ▶
{
    const float w = h * 0.86f;
    dl->AddTriangleFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y),
                          ImVec2(c.x - w * 0.5f, c.y + h * 0.5f), col);
}

// ☆ and ★. The filled star is five convex kites around the centre: ImGui's polygon fill wants
// a convex shape, and a star is not one.
static void DrawStar(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool filled, float thickness = 1.2f)
{
    ImVec2 v[10];
    for (int i = 0; i < 10; i++)
    {
        const float a = (float)i * IM_PI / 5.0f - IM_PI * 0.5f;
        const float rad = (i % 2 == 0) ? r : r * 0.42f;
        v[i] = ImVec2(c.x + std::cos(a) * rad, c.y + std::sin(a) * rad);
    }
    if (filled)
    {
        for (int i = 0; i < 5; i++)
        {
            const ImVec2 kite[4] = { c, v[(2 * i + 9) % 10], v[2 * i], v[2 * i + 1] };
            dl->AddConvexPolyFilled(kite, 4, col);
        }
    }
    else
        dl->AddPolyline(v, 10, col, ImDrawFlags_Closed, thickness);
}

// ─── Hall art: the loading screens, scaled and mipmapped ─────────────────────

struct ArtTexture
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    float w = 0.0f, h = 0.0f;
    bool tried = false;
};

// The rows' thumbnails and the sidebar's hero are the halls' loading screens, as in the mockup. The
// shared texture cache uploads one level at full size: a 1378 px picture drawn 64 px wide then
// shimmers, and the fifteen of them hold 100 MB. Here each is scaled to 640 px wide and given a mip
// chain, so a thumbnail samples a small level and the set costs about 20 MB. Its own cache, per
// device, and one picture decoded a frame so that scrolling onto new halls never stalls a frame.
static const ArtTexture* LoadArt(const std::string& path)
{
    static ID3D11Device* s_dev = nullptr;
    static std::unordered_map<std::string, ArtTexture> s_art;
    static int s_loadFrame = -1;
    if (path.empty()) return nullptr;
    ID3D11Device* dev = GetTextureCache().Device();
    if (!dev) return nullptr;
    if (dev != s_dev)
    {
        s_dev = dev;
        s_art.clear();
    }
    ArtTexture& art = s_art[path];
    if (art.srv) return &art;
    if (art.tried || s_loadFrame == ImGui::GetFrameCount()) return nullptr;
    s_loadFrame = ImGui::GetFrameCount();
    art.tried = true;

    DirectX::ScratchImage img, rgba, mips;
    if (FAILED(DirectX::LoadFromWICFile(std::filesystem::path(path).c_str(), DirectX::WIC_FLAGS_IGNORE_SRGB, nullptr, img)))
        return nullptr;
    const DirectX::Image* src = img.GetImage(0, 0, 0);
    if (!src || src->width == 0 || src->height == 0) return nullptr;
    if (src->format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        if (FAILED(DirectX::Convert(*src, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT,
                                    DirectX::TEX_THRESHOLD_DEFAULT, rgba)))
            return nullptr;
        src = rgba.GetImage(0, 0, 0);
    }
    // The whole chain, then the GPU gets it from the first level at most 640 px wide.
    constexpr size_t kMaxWidth = 640;
    if (SUCCEEDED(DirectX::GenerateMipMaps(*src, DirectX::TEX_FILTER_DEFAULT, 0, mips)))
    {
        const DirectX::TexMetadata& md = mips.GetMetadata();
        size_t first = 0;
        while (first + 1 < md.mipLevels && mips.GetImages()[first].width > kMaxWidth) first++;
        DirectX::TexMetadata sub = md;
        sub.width = mips.GetImages()[first].width;
        sub.height = mips.GetImages()[first].height;
        sub.mipLevels = md.mipLevels - first;
        DirectX::CreateShaderResourceView(dev, mips.GetImages() + first, sub.mipLevels, sub, art.srv.GetAddressOf());
        art.w = (float)sub.width;
        art.h = (float)sub.height;
    }
    else
    {
        DirectX::ScratchImage one;
        if (SUCCEEDED(one.InitializeFromImage(*src)))
            DirectX::CreateShaderResourceView(dev, one.GetImages(), one.GetImageCount(), one.GetMetadata(),
                                              art.srv.GetAddressOf());
        art.w = (float)src->width;
        art.h = (float)src->height;
    }
    return art.srv ? &art : nullptr;
}

// The hall's loading screen (Textures/Loading_Screen/<map name>.jpg or .webp), else its small
// picture in Guild_Halls. Resolved once per map.
static const std::string& HallArtPath(int mapId)
{
    static std::unordered_map<int, std::string> s_paths;
    if (auto it = s_paths.find(mapId); it != s_paths.end()) return it->second;
    EnsureTextureBasePath();
    std::string path;
    std::error_code ec;
    if (const char* name = GetMapName(mapId))
        for (const char* ext : { ".jpg", ".webp", ".png" })
        {
            const std::string p = ui::TextureBasePath() + "\\Textures\\Loading_Screen\\" + name + ext;
            if (std::filesystem::exists(p, ec)) { path = p; break; }
        }
    if (path.empty())
        if (const char* icon = GetMapIconFile(mapId))
        {
            const std::string p = ui::TextureBasePath() + "\\Textures\\Guild_Halls\\" + icon;
            if (std::filesystem::exists(p, ec)) path = p;
        }
    return s_paths.emplace(mapId, std::move(path)).first->second;
}

// CSS object-fit / background-size: cover.
static void ImageCover(ImDrawList* dl, ImTextureID tex, float tw, float th, ImVec2 a, ImVec2 b, ImU32 tint,
                       float rounding)
{
    const float bw = b.x - a.x, bh = b.y - a.y;
    if (tw <= 0.0f || th <= 0.0f || bw <= 0.0f || bh <= 0.0f) return;
    const float s = std::max(bw / tw, bh / th);
    const float uw = bw / (tw * s), vh = bh / (th * s);
    const ImVec2 uv0((1.0f - uw) * 0.5f, (1.0f - vh) * 0.5f), uv1(uv0.x + uw, uv0.y + vh);
    if (rounding > 0.0f) dl->AddImageRounded(tex, a, b, uv0, uv1, tint, rounding);
    else                 dl->AddImage(tex, a, b, uv0, uv1, tint);
}

// CSS object-fit: contain.
static void ImageContain(ImDrawList* dl, ImTextureID tex, float tw, float th, ImVec2 a, ImVec2 b,
                         ImU32 tint = IM_COL32_WHITE)
{
    const float bw = b.x - a.x, bh = b.y - a.y, s = std::min(bw / tw, bh / th);
    const float w = tw * s, h = th * s;
    const ImVec2 p(a.x + (bw - w) * 0.5f, a.y + (bh - h) * 0.5f);
    dl->AddImage(tex, p, ImVec2(p.x + w, p.y + h), ImVec2(0, 0), ImVec2(1, 1), tint);
}

// ─── The backdrop: the selected match's hall behind the page, glass panels over it ─

struct BackdropArt
{
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sharp, soft;   // soft: lightly blurred, seen through the glass
    float w = 0.0f, h = 0.0f;
};

// A loading screen decoded, toned down and blurred off the main thread; uploaded on it.
struct BackdropPixels
{
    std::string path;
    std::vector<uint8_t> sharp, soft;
    int w = 0, h = 0, softW = 0, softH = 0;
};

// The mockup's scene filter, CSS sepia(0.3) saturate(0.65): the two filter matrices of the Filter
// Effects spec, applied in that order to the sRGB bytes, as a browser applies them.
static void Desaturate(std::vector<uint8_t>& px)
{
    const float a = 1.0f - 0.3f, s = 0.65f;
    const float sep[9] = { 0.393f + 0.607f * a, 0.769f - 0.769f * a, 0.189f - 0.189f * a,
                           0.349f - 0.349f * a, 0.686f + 0.314f * a, 0.168f - 0.168f * a,
                           0.272f - 0.272f * a, 0.534f - 0.534f * a, 0.131f + 0.869f * a };
    const float sat[9] = { 0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s,
                           0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s,
                           0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s };
    for (size_t i = 0; i + 3 < px.size(); i += 4)
    {
        const float c[3] = { (float)px[i], (float)px[i + 1], (float)px[i + 2] };
        float d[3], o[3];
        for (int k = 0; k < 3; k++)
            d[k] = std::clamp(sep[k * 3] * c[0] + sep[k * 3 + 1] * c[1] + sep[k * 3 + 2] * c[2], 0.0f, 255.0f);
        for (int k = 0; k < 3; k++)
            o[k] = std::clamp(sat[k * 3] * d[0] + sat[k * 3 + 1] * d[1] + sat[k * 3 + 2] * d[2], 0.0f, 255.0f);
        for (int k = 0; k < 3; k++) px[i + k] = (uint8_t)(o[k] + 0.5f);
    }
}

// Three box passes each way: close to a gaussian.
static void BoxBlur(std::vector<uint8_t>& px, int w, int h, int radius)
{
    std::vector<uint8_t> tmp(px.size());
    const float n = 2.0f * radius + 1.0f;
    for (int pass = 0; pass < 3; pass++)
    {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 4; c++)
                {
                    float sum = 0.0f;
                    for (int k = -radius; k <= radius; k++)
                        sum += px[((size_t)y * w + std::clamp(x + k, 0, w - 1)) * 4 + c];
                    tmp[((size_t)y * w + x) * 4 + c] = (uint8_t)(sum / n + 0.5f);
                }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 4; c++)
                {
                    float sum = 0.0f;
                    for (int k = -radius; k <= radius; k++)
                        sum += tmp[((size_t)std::clamp(y + k, 0, h - 1) * w + x) * 4 + c];
                    px[((size_t)y * w + x) * 4 + c] = (uint8_t)(sum / n + 0.5f);
                }
    }
}

static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> UploadRgba(ID3D11Device* dev, const std::vector<uint8_t>& px,
                                                                   int w, int h)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = px.data();
    data.SysMemPitch = (UINT)w * 4;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(dev->CreateTexture2D(&desc, &data, tex.GetAddressOf())))
        dev->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
    return srv;
}

// On a worker thread (WIC needs COM there too): the picture at most 1920 px wide, toned down,
// and a 640 px copy lightly blurred, which drawn at the panels' size blurs about as much as the
// mockup's backdrop-filter: blur(3px).
static BackdropPixels DecodeBackdrop(std::string path)
{
    BackdropPixels out;
    out.path = path;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DirectX::ScratchImage img, rgba, mips;
    const DirectX::Image* src = nullptr;
    if (SUCCEEDED(DirectX::LoadFromWICFile(std::filesystem::path(path).c_str(), DirectX::WIC_FLAGS_IGNORE_SRGB, nullptr, img)))
        src = img.GetImage(0, 0, 0);
    if (src && src->format != DXGI_FORMAT_R8G8B8A8_UNORM)
        src = SUCCEEDED(DirectX::Convert(*src, DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT,
                                         DirectX::TEX_THRESHOLD_DEFAULT, rgba))
                  ? rgba.GetImage(0, 0, 0)
                  : nullptr;
    if (src && src->width > 0 && src->height > 0 &&
        SUCCEEDED(DirectX::GenerateMipMaps(*src, DirectX::TEX_FILTER_DEFAULT, 0, mips)))
    {
        auto level = [&](size_t maxW) {
            const DirectX::Image* im = mips.GetImages();
            size_t i = 0;
            while (i + 1 < mips.GetImageCount() && im[i].width > maxW) i++;
            return im + i;
        };
        auto pixels = [](const DirectX::Image* im) {
            std::vector<uint8_t> v(im->width * im->height * 4);
            for (size_t y = 0; y < im->height; y++)
                memcpy(v.data() + y * im->width * 4, im->pixels + y * im->rowPitch, im->width * 4);
            return v;
        };
        const DirectX::Image* big = level(1920);
        out.sharp = pixels(big);
        Desaturate(out.sharp);
        out.w = (int)big->width;
        out.h = (int)big->height;
        const DirectX::Image* blurSource = level(640);
        out.soft = pixels(blurSource);
        Desaturate(out.soft);
        out.softW = (int)blurSource->width;
        out.softH = (int)blurSource->height;
        BoxBlur(out.soft, out.softW, out.softH, 1);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}

// The pictures loaded (the last four, most recent first), the one being decoded, and the fade from
// the previous match's hall to the selected one's.
struct BackdropState
{
    ID3D11Device* dev = nullptr;
    std::list<std::pair<std::string, BackdropArt>> cache;
    std::future<BackdropPixels> job;
    std::set<std::string> failed;
    std::string current, previous;
    double fadeStart = -10.0;
};
static BackdropState s_backdrop;

static const BackdropArt* FindBackdrop(const std::string& path, bool touch)
{
    auto& cache = s_backdrop.cache;
    for (auto it = cache.begin(); it != cache.end(); ++it)
        if (it->first == path)
        {
            if (touch && it != cache.begin()) cache.splice(cache.begin(), cache, it);   // `it` stays valid
            return &it->second;
        }
    return nullptr;
}

// How much of the art shows (Settings > Browser > Background).
static float BackdropStrength()
{
    switch (GuiGlobalConstants::replay_library_backdrop)
    {
    case 1:  return 0.38f;   // Subtle
    case 2:  return 0.0f;    // Off
    default: return 0.68f;   // Normal: V41's scene opacity
    }
}

// The scene: the selected match's hall, its loading screen (owner 2026-11-25), else (no match
// selected, or a hall with only a small picture) V41's Isle of Meditation.
static std::string BackdropPath(int mapId)
{
    const std::string& p = HallArtPath(mapId);
    if (p.find("\\Loading_Screen\\") != std::string::npos) return p;
    EnsureTextureBasePath();
    return ui::TextureBasePath() + "\\Textures\\Loading_Screen\\Isle of Meditation.jpg";
}

// Once a frame, with the selected match's map: a picture not loaded yet is decoded on a worker (the
// list never waits for it), and the page fades to it once it is in.
static void UpdateBackdrop(int mapId)
{
    BackdropState& s = s_backdrop;
    ID3D11Device* dev = GetTextureCache().Device();
    if (!dev || BackdropStrength() <= 0.0f) return;
    if (dev != s.dev)
    {
        s.cache.clear();
        s.current.clear();
        s.previous.clear();
        s.dev = dev;
    }
    if (s.job.valid() && s.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        BackdropPixels px = s.job.get();
        BackdropArt art;
        if (!px.sharp.empty())
        {
            art.sharp = UploadRgba(dev, px.sharp, px.w, px.h);
            art.soft = UploadRgba(dev, px.soft, px.softW, px.softH);
            art.w = (float)px.w;
            art.h = (float)px.h;
        }
        if (art.sharp && art.soft)
        {
            s.cache.emplace_front(px.path, art);
            while (s.cache.size() > 4) s.cache.pop_back();
        }
        else
        {
            s.failed.insert(px.path);
            RunLog::Line("library-page: no backdrop art from '%s'", px.path.c_str());
        }
    }
    const std::string want = BackdropPath(mapId);
    if (want == s.current) return;
    if (FindBackdrop(want, true))
    {
        s.previous = s.current;
        s.current = want;
        s.fadeStart = ImGui::GetTime();
    }
    else if (!s.job.valid() && !s.failed.count(want))
        s.job = std::async(std::launch::async, DecodeBackdrop, want);
}

// CSS background-size: cover, at 46% down as in the mockup: the art's texture coordinate under a
// point of the page.
struct CoverMap
{
    ImVec2 a, uv0;
    float scale = 1.0f, tw = 1.0f, th = 1.0f;
    CoverMap(ImVec2 pa, ImVec2 pb, float w, float h) : a(pa), tw(w), th(h)
    {
        const float bw = pb.x - pa.x, bh = pb.y - pa.y;
        scale = std::max(bw / tw, bh / th);
        const float uw = bw / (tw * scale), vh = bh / (th * scale);
        uv0 = ImVec2((1.0f - uw) * 0.5f, (1.0f - vh) * 0.46f);
    }
    ImVec2 UV(ImVec2 p) const { return ImVec2(uv0.x + (p.x - a.x) / (tw * scale), uv0.y + (p.y - a.y) / (th * scale)); }
};

// The art over [a, b] of the page: the sharp picture or the blurred one, the previous hall fading
// out under the selected one.
static void DrawBackdropArt(ImDrawList* dl, ImVec2 pageA, ImVec2 pageB, ImVec2 a, ImVec2 b, bool soft, float rounding)
{
    const float k = BackdropStrength();
    if (k <= 0.0f) return;
    const BackdropState& s = s_backdrop;
    const float t = (float)std::clamp((ImGui::GetTime() - s.fadeStart) / 0.45, 0.0, 1.0);
    auto draw = [&](const std::string& path, float alpha) {
        const BackdropArt* art = path.empty() ? nullptr : FindBackdrop(path, false);
        if (!art || alpha < 0.004f) return;
        const CoverMap map(pageA, pageB, art->w, art->h);
        ID3D11ShaderResourceView* tex = soft ? art->soft.Get() : art->sharp.Get();
        dl->AddImageRounded((ImTextureID)tex, a, b, map.UV(a), map.UV(b), IM_COL32(255, 255, 255, (int)(alpha * 255.0f)), rounding);
    };
    if (t < 1.0f) draw(s.previous, k);
    draw(s.current, k * t);
}

static ImU32 LerpColor(ImU32 a, ImU32 b, float t)
{
    auto ch = [&](int shift) {
        const float x = (float)((a >> shift) & 0xFF), y = (float)((b >> shift) & 0xFF);
        return (ImU32)(x + (y - x) * t + 0.5f) << shift;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}

// A rounded rectangle filled top to bottom through colour stops (a CSS linear-gradient; stops at
// fractions of the height, in order). Drawn in bands, a band per stop, since a colour only changes
// between vertices: the rounded caps keep their antialiased fringe, the bands between them have none.
struct GradientStop
{
    float at;
    ImU32 col;
};

static ImU32 GradientAt(std::initializer_list<GradientStop> stops, float t)
{
    if (t <= stops.begin()->at) return stops.begin()->col;
    for (auto it = stops.begin(); it + 1 != stops.end(); ++it)
        if (t <= (it + 1)->at)
            return LerpColor(it->col, (it + 1)->col, (it + 1)->at > it->at ? (t - it->at) / ((it + 1)->at - it->at) : 1.0f);
    return (stops.end() - 1)->col;
}

static void FillRoundedGradient(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, std::initializer_list<GradientStop> stops,
                                ImDrawFlags corners = 0)
{
    const float h = b.y - a.y;
    if (h <= 0.0f || b.x <= a.x) return;
    if ((corners & ImDrawFlags_RoundCornersMask_) == 0) corners = ImDrawFlags_RoundCornersAll;
    r = std::min(r, h * 0.5f);
    auto colAt = [&](float y) { return GradientAt(stops, std::clamp((y - a.y) / h, 0.0f, 1.0f)); };
    auto cap = [&](float y0, float y1, ImDrawFlags which) {
        ImDrawFlags f = corners & which;
        if (f == 0) f = ImDrawFlags_RoundCornersNone;
        const int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(ImVec2(a.x, y0), ImVec2(b.x, y1), IM_COL32_WHITE, r, f);
        for (int i = v0; i < dl->VtxBuffer.Size; i++)
        {
            ImDrawVert& v = dl->VtxBuffer[i];
            ImU32 c = colAt(v.pos.y);
            if ((v.col & IM_COL32_A_MASK) == 0) c &= ~IM_COL32_A_MASK;
            v.col = c;
        }
    };
    if (r > 0.0f) cap(a.y, a.y + r, ImDrawFlags_RoundCornersTop);
    std::vector<float> ys = { a.y + r };
    for (const GradientStop& st : stops)
    {
        const float y = a.y + st.at * h;
        if (y > a.y + r && y < b.y - r) ys.push_back(y);
    }
    ys.push_back(b.y - r);
    for (size_t i = 0; i + 1 < ys.size(); i++)
        if (ys[i + 1] > ys[i])
        {
            const ImU32 c0 = colAt(ys[i]), c1 = colAt(ys[i + 1]);
            dl->AddRectFilledMultiColor(ImVec2(a.x, ys[i]), ImVec2(b.x, ys[i + 1]), c0, c0, c1, c1);
        }
    if (r > 0.0f) cap(b.y - r, b.y, ImDrawFlags_RoundCornersBottom);
}

static void FillRoundedGradient(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 top, ImU32 middle, float mid,
                                ImU32 bottom, ImDrawFlags corners = 0)
{
    FillRoundedGradient(dl, a, b, r, { { 0.0f, top }, { mid, middle }, { 1.0f, bottom } }, corners);
}

// A CSS linear-gradient(angle, c0, c1) over the box [a, b], applied to the vertices the draw list
// added since v0: each takes the colour of its position on the gradient line. A shape ImGui fills as
// one convex polygon interpolates a linear function exactly, so a rounded rectangle gets the true
// gradient.
static void ShadeLinear(ImDrawList* dl, int v0, ImVec2 a, ImVec2 b, float angleDeg, ImU32 c0, ImU32 c1)
{
    const float th = angleDeg * 3.14159265f / 180.0f;
    const ImVec2 dir(std::sin(th), -std::cos(th));
    const float w = b.x - a.x, h = b.y - a.y;
    const float len = std::max(1e-3f, std::fabs(w * dir.x) + std::fabs(h * dir.y));
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    for (int i = v0; i < dl->VtxBuffer.Size; i++)
    {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float t = std::clamp(((v.pos.x - c.x) * dir.x + (v.pos.y - c.y) * dir.y) / len + 0.5f, 0.0f, 1.0f);
        ImU32 out = LerpColor(c0, c1, t);
        if ((v.col & IM_COL32_A_MASK) == 0) out &= ~IM_COL32_A_MASK;   // the antialiased fringe
        v.col = out;
    }
}

static void FillLinear(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float angleDeg, ImU32 c0, ImU32 c1,
                       ImDrawFlags flags = 0)
{
    if (b.x <= a.x || b.y <= a.y) return;
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding, flags);
    ShadeLinear(dl, v0, a, b, angleDeg, c0, c1);
}

// The full-screen wash, linear-gradient(120deg, #19191938, #19191970): page-wide, drawn on [a, b].
static void DrawWash(ImDrawList* dl, ImVec2 pageA, ImVec2 pageB, ImVec2 a, ImVec2 b, float rounding, ImDrawFlags flags)
{
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding, flags);
    ShadeLinear(dl, v0, pageA, pageB, 120.0f, v41::Wash0, v41::Wash1);
}

// The scene behind the panels: the page colour, the art at its strength (V41: opacity 0.68), the wash.
static void DrawBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b)
{
    dl->AddRectFilled(a, b, col::Page);
    DrawBackdropArt(dl, a, b, a, b, false, 0.0f);
    DrawWash(dl, a, b, a, b, 0.0f, 0);
}

// A V41 glass panel (the list and the sidebar alike): the scene under it blurred (backdrop-filter:
// blur(4px)), then its tint, set in pixels from the panel's top and holding its last stop below
// 205 px, then a 1 px white outline and the inset top highlight. `corners`: the outer ones only, as
// the two panels meet on a straight seam. `rightLine`: false on the list, whose right edge is the
// sidebar's left border.
static void DrawPanel(ImDrawList* dl, ImVec2 pageA, ImVec2 pageB, ImVec2 a, ImVec2 b, ImDrawFlags corners, bool rightLine)
{
    const float r = Px(5);
    dl->AddRectFilled(a, b, col::Page, r, corners);
    if (BackdropStrength() > 0.0f)
    {
        DrawBackdropArt(dl, pageA, pageB, a, b, true, 0.0f);
        DrawWash(dl, pageA, pageB, a, b, 0.0f, 0);
    }
    const float h = std::max(1.0f, b.y - a.y);
    auto at = [&](float css) { return std::min(1.0f, Px(css) / h); };
    FillRoundedGradient(dl, a, b, r,
                        { { 0.0f, v41::Tint0 }, { at(125), v41::Tint1 }, { at(170), v41::Tint2 }, { at(205), v41::Tint3 },
                          { 1.0f, v41::Tint3 } },
                        corners);
    dl->AddLine(ImVec2(a.x + ((corners & ImDrawFlags_RoundCornersTopLeft) ? r : 1.0f), a.y + 1.5f),
                ImVec2(b.x - ((corners & ImDrawFlags_RoundCornersTopRight) ? r : 1.0f), a.y + 1.5f), v41::PanelSheen);
    if (rightLine)
        dl->AddRect(ImVec2(R(a.x) + 0.5f, R(a.y) + 0.5f), ImVec2(R(b.x) - 0.5f, R(b.y) - 0.5f), v41::PanelLine, r, corners, 1.0f);
    else
    {
        // Top, left and bottom: the right edge is the seam the sidebar draws.
        const ImVec2 a2(R(a.x) + 0.5f, R(a.y) + 0.5f), b2(R(b.x), R(b.y) - 0.5f);
        dl->PathLineTo(ImVec2(b2.x, b2.y));
        dl->PathArcToFast(ImVec2(a2.x + r, b2.y - r), r, 3, 6);
        dl->PathArcToFast(ImVec2(a2.x + r, a2.y + r), r, 6, 9);
        dl->PathLineTo(ImVec2(b2.x, a2.y));
        dl->PathStroke(v41::PanelLine, 0, 1.0f);
    }
}


// ─── Match data the page shows ───────────────────────────────────────────────

// Each team's elite skills, one slot per player in player order: what the list shows of a build.
struct EliteSlot
{
    int skill = 0;   // 0: the player took no elite
    int prof = 0;
    std::string player;
    bool henchman = false;
    bool empty = false;   // nobody in this slot: a player who never joined
};

// A team's eight slots, as the Library shows them (owner 2026-11-25): its players by player number,
// then its henchmen (infos.json "OTHER", recognised by model id, kHenchmen), then empty slots up to
// eight. The cloud index carries players only, so a cloud match's henchmen show as empty slots
// until it is downloaded.
struct RosterSlot
{
    const PlayerMeta* player = nullptr;     // a player, or the henchman's own agent entry
    const HenchmanInfo* henchman = nullptr; // set for a henchman
};

static std::vector<RosterSlot> TeamSlots(const MatchMeta& m, const char* partyId)
{
    std::vector<RosterSlot> slots;
    auto pit = m.parties.find(partyId);
    if (pit != m.parties.end())
    {
        for (const auto& p : pit->second.players) slots.push_back({ &p, nullptr });
        std::sort(slots.begin(), slots.end(),
                  [](const RosterSlot& a, const RosterSlot& b) { return a.player->player_number < b.player->player_number; });
        std::vector<RosterSlot> hench;
        for (const auto& o : pit->second.others)
            if (const HenchmanInfo* h = LookupHenchman((uint32_t)o.model_id)) hench.push_back({ &o, h });
        std::sort(hench.begin(), hench.end(),
                  [](const RosterSlot& a, const RosterSlot& b) { return a.henchman->modelId < b.henchman->modelId; });
        slots.insert(slots.end(), hench.begin(), hench.end());
    }
    while (slots.size() < 8) slots.push_back({});
    return slots;
}

// A henchman's bar: the wiki's (kHenchmen), the same in every match (owner 2026-11-25), not what
// the recording happened to see it use.
static std::vector<int> HenchmanSkills(const RosterSlot& s)
{
    std::vector<int> out;
    if (s.henchman)
        for (int id : s.henchman->skills)
            if (id) out.push_back(id);
    return out;
}
struct EliteLineup
{
    std::vector<EliteSlot> team[2];
};

static const EliteLineup& LineupOf(const MatchMeta& m)
{
    static std::unordered_map<std::string, EliteLineup> s_cache;
    static int s_generation = -2;
    if (s_generation != s_libraryGeneration)
    {
        s_cache.clear();
        s_generation = s_libraryGeneration;
    }
    auto [it, inserted] = s_cache.try_emplace(m.folder_name);
    if (!inserted) return it->second;
    const SkillDatabaseView& view = GetMatchSkillView(m);
    for (int side = 0; side < 2; side++)
        for (const RosterSlot& rs : TeamSlots(m, side == 0 ? "1" : "2"))
        {
            EliteSlot slot;
            if (!rs.player)
                slot.empty = true;
            else if (rs.henchman)
            {
                slot.henchman = true;
                slot.prof = rs.henchman->profession;
                slot.player = rs.henchman->name;
                for (int id : HenchmanSkills(rs))
                {
                    const SkillInfo* si = view.Get(id);
                    if (si && si->is_elite) { slot.skill = id; break; }
                }
            }
            else
            {
                const PlayerMeta* p = rs.player;
                slot.prof = p->primary;
                slot.player = p->encoded_name.empty() ? "(unnamed)" : SanitizePlayerName(p->encoded_name);
                for (int id : p->used_skills)
                {
                    const SkillInfo* si = view.Get(id);
                    if (si && si->is_elite) { slot.skill = id; break; }
                }
            }
            it->second.team[side].push_back(std::move(slot));
        }
    return it->second;
}

static std::string MatchDate(const MatchMeta& m, bool upper)
{
    std::string month = MonthAbbrev(m.month);
    if (upper)
        for (char& ch : month) ch = (char)toupper((unsigned char)ch);
    return std::to_string(m.day) + " " + month + " " + std::to_string(m.year);
}

// ─── Page state (not persisted, as the other filters are not) ────────────────

struct PageState
{
    bool favouritesOnly = false;
    bool autoSelected = false;
    bool showDetail = false;         // single-pane width: the sidebar in place of the list
    std::string openMenu;            // the filter menu open under the filter row ("" = none)
    std::string queriedMenu;         // the menu the queries below were typed in; another one clears them
    char mapQuery[64] = {};          // the Map menu's field
    char fluxQuery[64] = {};         // the Flux menu's field: a flux or a month
    ImVec2 menuAnchor;
    bool menuLabelHovered = false;
    int scrollToRow = -1;            // a keyboard move asks the list to bring this row into view
    double shareCopiedAt = -10.0;
    std::string shareCopiedFor;
    // The player a right-click opened the context menu for.
    std::string ctxName, ctxTemplate, ctxTeamTemplates;
    bool openCtx = false;
};
static PageState s_page;

static bool Active()
{
    return GuiGlobalConstants::replay_library_legacy == 0 && !s_state.tournamentMode;
}

// Everything the legacy sidebar's Clear All clears, but Scout's own fields.
static void ClearAllFilters()
{
    s_state.searchBuf[0] = '\0';
    s_state.searchDebounced[0] = '\0';
    s_state.lastSearchEditTime = -1.0f;
    s_state.selectedSearchTerms.clear();
    s_state.selectedMaps.clear();
    s_state.selectedFluxes.clear();
    s_state.selectedOccasions.clear();
    s_state.selectedBuilds.clear();
    s_state.selectedSkills.clear();
    s_state.mapSearchBuf[0] = '\0';
    s_state.fluxSearchBuf[0] = '\0';
    s_state.buildSearchBuf[0] = '\0';
    s_state.skillSearchBuf[0] = '\0';
    s_state.skillMatchMode = 0;
    s_state.skillScope = 2;
    s_state.browseOpenId.clear();
    s_state.matchupBufA[0] = '\0';
    s_state.matchupBufB[0] = '\0';
    s_state.matchupDisplayA.clear();
    s_state.matchupNameA.clear();
    s_state.matchupTagA.clear();
    s_state.matchupDisplayB.clear();
    s_state.matchupNameB.clear();
    s_state.matchupTagB.clear();
    s_state.dateFrom = {};
    s_state.dateTo = {};
    s_state.calBrowseFromMonth = 0;
    s_state.calBrowseFromYear = 0;
    s_state.calBrowseToMonth = 0;
    s_state.calBrowseToYear = 0;
    s_state.minRatingFilter = 0;
    s_state.myRated = s_state.myNotes = s_state.myComms = false;
    s_page.favouritesOnly = false;
}

static bool AnyFilterActive()
{
    return s_page.favouritesOnly || !s_state.selectedSearchTerms.empty() || s_state.searchDebounced[0] != '\0' ||
           !s_state.selectedMaps.empty() || !s_state.selectedFluxes.empty() || !s_state.selectedOccasions.empty() ||
           !s_state.selectedBuilds.empty() || !s_state.selectedSkills.empty() ||
           MatchupSlotSet(s_state.matchupNameA, s_state.matchupTagA) ||
           MatchupSlotSet(s_state.matchupNameB, s_state.matchupTagB) || DateValValid(s_state.dateFrom) ||
           DateValValid(s_state.dateTo) || s_state.minRatingFilter > 0 || s_state.myRated || s_state.myNotes ||
           s_state.myComms;
}

// ─── The action buttons ──────────────────────────────────────────────────────

// One of the outlined actions (Update matches, Spoiler-free): 3 px rounding, amber while `on`.
static bool ActionButton(const char* id, ImVec2 pos, ImVec2 size, bool on, bool& hovered)
{
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, size);
    hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    // V41: height 40, linear-gradient(#302e28, #25241f), 1 px #675a3e55, 5 px rounding.
    if (on)
        dl->AddRectFilled(pos, b, col::ToggleOnBg, Px(5));
    else
        FillLinear(dl, pos, b, Px(5), 180.0f, hovered ? LerpColor(v41::ActA, Hex(0x3d3a31), 0.6f) : v41::ActA,
                   hovered ? LerpColor(v41::ActB, Hex(0x2e2c25), 0.6f) : v41::ActB);
    Outline(dl, pos, b, on ? col::ToggleOnRule : hovered ? col::ActionHovRule : v41::ActLine, Px(5));
    return clicked;
}

// ─── Search and the All matches / Favorites switch ───────────────────────────

static void DrawSearch(ImDrawList* dl, float x, float y, float w, float h)
{
    // V41: 44 px, #151514a8 with a 1 px #ffffff12 outline, 5 px rounding, a faint inner shadow.
    const ImVec2 a(x, y), b(x + w, y + h);
    dl->AddRectFilled(a, b, v41::SearchBg, Px(5));
    dl->AddRectFilledMultiColor(ImVec2(a.x + 1.0f, a.y + 1.0f), ImVec2(b.x - 1.0f, a.y + Px(5)), Hex(0, 0x12), Hex(0, 0x12),
                                Hex(0, 0), Hex(0, 0));
    Outline(dl, a, b, v41::SearchLine, Px(5));

    const Type t = T(13);
    const float textTop = R(y + (h - LineH(13)) * 0.5f);
    float cx = x + Px(15);
    DrawMagnifier(dl, ImVec2(cx + Px(5), textTop + LineH(13) * 0.5f), Px(10), v41::SearchText);
    cx += Px(11) + TextW(t, " ") + Px(4);

    // The search chips (a guild or player picked from the suggestions, or Enter): inside the box,
    // before the text. A click on one removes it.
    std::string remove;
    const Type ct = T(10);
    for (const auto& term : s_state.selectedSearchTerms)
    {
        const float tw = TextW(ct, term.c_str());
        const float chipW = Px(10) + tw + Px(6) + Px(7) + Px(8);
        if (cx + chipW > b.x - Px(140)) break;
        const ImVec2 c0(cx, y + Px(6)), c1(cx + chipW, b.y - Px(6));
        ImGui::SetCursorScreenPos(c0);
        ImGui::PushID(term.c_str());
        if (ImGui::InvisibleButton("##chip", ImVec2(chipW, c1.y - c0.y))) remove = term;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        dl->AddRectFilled(c0, c1, hov ? Hex(0x3a3428) : Hex(0x2a2720), (c1.y - c0.y) * 0.5f);
        Text(dl, ct, ImVec2(c0.x + Px(10), (c0.y + c1.y - LineH(10)) * 0.5f), col::ButtonText, term.c_str());
        const ImVec2 xc(c0.x + Px(10) + tw + Px(6) + Px(3.5f), (c0.y + c1.y) * 0.5f);
        const float r = Px(3);
        const ImU32 xcol = hov ? col::ToggleOn : col::SearchText;
        dl->AddLine(ImVec2(xc.x - r, xc.y - r), ImVec2(xc.x + r, xc.y + r), xcol, 1.3f);
        dl->AddLine(ImVec2(xc.x - r, xc.y + r), ImVec2(xc.x + r, xc.y - r), xcol, 1.3f);
        if (hov) ImGui::SetTooltip("Remove \"%s\" from the search", term.c_str());
        cx += chipW + Px(6);
    }
    if (!remove.empty()) s_state.selectedSearchTerms.erase(remove);

    // The text field itself: frameless, in the box's own type.
    ImGui::SetCursorScreenPos(ImVec2(cx, textTop));
    ImGui::PushFont(t.font);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::Text));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(v41::SearchText));
    ImGui::SetNextItemWidth(std::max(Px(40), b.x - Px(15) - cx));
    const char* hint = s_state.selectedSearchTerms.empty() ? "Search players, guilds, or notes\xE2\x80\xA6" : "Add to the search\xE2\x80\xA6";
    const bool edited = ImGui::InputTextWithHint("##lib_search", hint, s_state.searchBuf, sizeof(s_state.searchBuf));
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    if (edited) s_state.lastSearchEditTime = (float)ImGui::GetTime();
    // Enter ends the edit, so the field is either still active or deactivated this frame.
    const bool enter = (ImGui::IsItemActive() || ImGui::IsItemDeactivated()) &&
                       (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));

    // A share link pasted here is not a search: one row under the box that opens it.
    const bool linkInSearch = MatchLinks::LooksLikeLink(s_state.searchBuf);
    if (linkInSearch)
    {
        s_state.lastSearchEditTime = -1.0f;
        s_state.searchDebounced[0] = '\0';
        MatchLinks::ParseError err = MatchLinks::ParseError::None;
        const auto link = MatchLinks::Parse(s_state.searchBuf, &err);
        const std::string label = link ? "Open shared match: " + MatchLinks::DescribeFolder(link->folder) + " at " +
                                             MatchLinks::FormatClock(link->t)
                                       : (err == MatchLinks::ParseError::NewerVersion ? "Update GW Observer to open this link"
                                                                                      : "This link names no match");
        ImGui::SetNextWindowPos(ImVec2(a.x, b.y + Px(2)));
        ImGui::SetNextWindowSize(ImVec2(w, 0));
        bool open = enter;
        if (ImGui::Begin("##lib_link_row", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::BeginDisabled(!link);
            ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
            open |= ImGui::Selectable(label.c_str());
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
        }
        ImGui::End();
        if (open && link)
        {
            RunLog::Line("link: received (Library search box) - match '%s' at %.1f s", link->folder.c_str(), link->t);
            UrlProtocol::SetPendingLink(*link);
            s_state.searchBuf[0] = '\0';
        }
        return;
    }

    if (enter && s_state.searchBuf[0] != '\0')
    {
        s_state.selectedSearchTerms.insert(std::string(s_state.searchBuf));
        s_state.searchBuf[0] = '\0';
        s_state.searchDebounced[0] = '\0';
        s_state.lastSearchEditTime = -1.0f;
    }
    if (s_state.searchBuf[0] != '\0')
        DrawGlobalSearchAutoComplete(a, b);
}

static bool ToolbarButton(ImDrawList* dl, const char* id, ImVec2 pos, ImVec2 size, const char* label, bool active,
                          bool star)
{
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    // V41: the chosen view on a gradient (#38362f to #2d2c27) with a gold-tinted outline and an inset
    // highlight; the other one a faint outlined box.
    if (active)
    {
        FillLinear(dl, pos, b, Px(5), 180.0f, v41::BtnOnA, v41::BtnOnB);
        dl->AddLine(ImVec2(pos.x + Px(5), pos.y + 1.5f), ImVec2(b.x - Px(5), pos.y + 1.5f), v41::PanelSheen);
        Outline(dl, pos, b, v41::BtnOnLine, Px(5));
    }
    else
    {
        dl->AddRectFilled(pos, b, hov ? Hex(0xffffff, 0x0e) : v41::BtnOff, Px(5));
        Outline(dl, pos, b, v41::BtnOffLine, Px(5));
    }
    const ImU32 fg = active ? v41::BtnOnText : hov ? col::Text : v41::BtnOffText;
    const Type t = T(13);
    float tx = pos.x + Px(13);
    const float ty = R(pos.y + (size.y - LineH(13)) * 0.5f);
    if (star)
    {
        DrawStar(dl, ImVec2(tx + Px(5.5f), ty + LineH(13) * 0.5f + Px(0.5f)), Px(5.0f), active ? v41::StarOn : fg, active, 1.1f);
        tx += Px(11) + TextW(t, " ");
    }
    Text(dl, t, ImVec2(tx, ty), fg, label);
    return clicked;
}

// ─── The toolbar: search, All matches / Favorites, Update matches, Spoiler-free ──

static bool LinkText(ImDrawList* dl, const char* id, const Type& t, ImVec2 pos, float maxW, ImU32 c, const char* s);   // below

// One row where the mockup had two: the "Match library" title row is gone (the count is in the
// footer), and its two actions sit at the end of the search row. While the search would be narrower
// than 260 px, the two actions keep only their pictures.
// V41: the search (44 px, flexing), All matches and Favorites (13 px type, 12 x 13 padding, 41 px),
// Update matches and Spoiler-free (11 px type, 40 px), 10 px apart, each centred on the search's row.
static float DrawToolbar(ImDrawList* dl, float x0, float x1, float y)
{
    const float h = Px(44);
    const float bh = R(Px(12) * 2.0f + LineH(13) + 2.0f), ah = Px(40);
    const float by = R(y + (h - bh) * 0.5f), ay = R(y + (h - ah) * 0.5f);
    const Type tb = T(13), ab = T(11), st = T(9);
    SpoilerFree& sf = SpoilerFree::Get();
    const bool spoilers = sf.Enabled();
    const char* state = spoilers ? "On" : "Off";
    const bool waiting = g_refreshHint;

    const float favW = 2.0f + Px(13) * 2.0f + Px(11) + TextW(tb, " ") + TextW(tb, "Favorites");
    const float allW = 2.0f + Px(13) * 2.0f + TextW(tb, "All matches");
    float updW = 2.0f + Px(8) + Px(20) + Px(8) + TextW(ab, "Update matches") + (waiting ? Px(13) : 0.0f) + Px(8);
    float spW = 2.0f + Px(8) + Px(16) + Px(8) + TextW(ab, "Spoiler-free") + Px(14) + TextW(st, state) + Px(8);
    const float gap = Px(10), groupGap = Px(10);
    const bool compact = (x1 - x0) - (allW + favW + updW + spW + gap * 3.0f + groupGap) < Px(260);
    if (compact)
    {
        updW = 2.0f + Px(8) * 2.0f + Px(20) + (waiting ? Px(10) : 0.0f);
        spW = 2.0f + Px(8) * 2.0f + Px(16) + Px(6) + TextW(st, state);
    }
    const ImVec2 spPos(R(x1 - spW), ay);
    const ImVec2 upPos(R(spPos.x - gap - updW), ay);
    const ImVec2 favPos(R(upPos.x - groupGap - favW), by);
    const ImVec2 allPos(R(favPos.x - gap - allW), by);
    DrawSearch(dl, x0, y, allPos.x - gap - x0, h);

    if (ToolbarButton(dl, "##lib_all", allPos, ImVec2(allW, bh), "All matches", !s_page.favouritesOnly, false))
        s_page.favouritesOnly = false;
    if (ToolbarButton(dl, "##lib_fav", favPos, ImVec2(favW, bh), "Favorites", s_page.favouritesOnly, true))
        s_page.favouritesOnly = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The matches you starred (%d). Click a row's star to add one.", (int)MatchFavourites::Get().Count());

    // Update matches: the lit refresh badge, the label, and a dot while new matches wait.
    bool hov = false;
    y = ay;   // the actions' row, centred on the search's
    const float hh = ah;
    if (ActionButton("##lib_update", upPos, ImVec2(updW, hh), false, hov))
    {
        g_refreshMatchIndex = true;
        g_refreshHint = false;
    }
    {
        float cx = upPos.x + 1.0f + Px(8);
        EnsureTextureBasePath();
        if (ImTextureID tex = GetTextureCache().GetTexture(ui::TextureBasePath() + "\\Textures\\DDS\\ATEXDXT5\\texture_337393.dds"))
            dl->AddImage(tex, ImVec2(cx, R(y + (hh - Px(20)) * 0.5f)), ImVec2(cx + Px(20), R(y + (hh - Px(20)) * 0.5f) + Px(20)),
                         ImVec2(kRefreshLitX0 / kRefreshAtlasW, kRefreshSrcY0 / kRefreshAtlasH),
                         ImVec2(kRefreshLitX1 / kRefreshAtlasW, kRefreshSrcY1 / kRefreshAtlasH));
        cx += Px(20);
        if (!compact)
        {
            cx += Px(8);
            Text(dl, ab, ImVec2(cx, y + (hh - LineH(11)) * 0.5f), v41::ActText, "Update matches");
            cx += TextW(ab, "Update matches");
        }
        if (waiting)
            dl->AddCircleFilled(ImVec2(cx + Px(6) + Px(2.5f), y + hh * 0.5f), Px(2.5f), v41::NewDot, 12);
    }
    if (hov)
        ImGui::SetTooltip(waiting ? "New matches are waiting: refresh the list" : "Update matches: re-fetch the match list from the cloud");

    // Spoiler-free: eye, label, Off / On.
    if (ActionButton("##lib_spoiler", spPos, ImVec2(spW, hh), spoilers, hov))
        sf.SetEnabled(!spoilers);
    {
        const ImU32 fg = spoilers ? col::ToggleOn : v41::ActText;
        float cx = spPos.x + 1.0f + Px(8);
        DrawEyeGlyph(dl, ImVec2(cx + Px(8), y + hh * 0.5f), Px(14), fg, spoilers);
        cx += Px(16);
        if (!compact)
        {
            cx += Px(8);
            Text(dl, ab, ImVec2(cx, y + (hh - LineH(11)) * 0.5f), fg, "Spoiler-free");
            cx += TextW(ab, "Spoiler-free") + Px(14);
        }
        else
            cx += Px(6);
        Text(dl, st, ImVec2(cx, y + (hh - LineH(9)) * 0.5f + Px(1)), spoilers ? col::ToggleOn : col::ToggleState, state);
    }
    if (hov)
        ImGui::SetTooltip(spoilers ? "Spoiler is on: winners, match lengths, lord damage and match stats\n"
                                     "are hidden for every match. Turn it off to see them."
                                   : "Spoiler: hide winners, match lengths, lord damage and match stats\n"
                                     "for every match.");
    return h;
}

// ─── Filters: what each one holds ────────────────────────────────────────────

static DateVal Today()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    return { tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900 };
}

static DateVal AddDays(const DateVal& d, int days)
{
    std::tm tm{};
    tm.tm_mday = d.day + days;
    tm.tm_mon = d.month - 1;
    tm.tm_year = d.year - 1900;
    tm.tm_hour = 12;
    std::mktime(&tm);
    return { tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900 };
}

static bool SameDate(const DateVal& a, const DateVal& b)
{
    return a.day == b.day && a.month == b.month && a.year == b.year;
}

struct DatePreset
{
    const char* label;
    DateVal from, to;
};

// The Date menu's quick ranges, as of today.
static std::vector<DatePreset> DatePresets()
{
    const DateVal today = Today();
    const DateVal monthStart{ 1, today.month, today.year };
    const int lastMonth = today.month == 1 ? 12 : today.month - 1;
    const int lastMonthYear = today.month == 1 ? today.year - 1 : today.year;
    return {
        { "Today", today, today },
        { "Last 7 days", AddDays(today, -6), today },
        { "Last 30 days", AddDays(today, -29), today },
        { "This month", monthStart, today },
        { "Last month", { 1, lastMonth, lastMonthYear }, { DaysInMonth(lastMonth, lastMonthYear), lastMonth, lastMonthYear } },
        { "This year", { 1, 1, today.year }, today },
    };
}

static std::string ShortDate(const DateVal& d, bool withYear)
{
    std::string s = std::to_string(d.day) + " " + MonthAbbrev(d.month);
    if (withYear) s += " " + std::to_string(d.year);
    return s;
}

static std::string DateValue()
{
    const bool f = DateValValid(s_state.dateFrom), t = DateValValid(s_state.dateTo);
    if (f && t)
        for (const DatePreset& p : DatePresets())
            if (SameDate(p.from, s_state.dateFrom) && SameDate(p.to, s_state.dateTo)) return p.label;
    const int year = Today().year;
    if (f && t)
    {
        const bool sameYear = s_state.dateFrom.year == s_state.dateTo.year;
        return ShortDate(s_state.dateFrom, !sameYear || s_state.dateFrom.year != year) + " \xE2\x80\x93 " +
               ShortDate(s_state.dateTo, s_state.dateTo.year != year || !sameYear);
    }
    if (f) return "from " + ShortDate(s_state.dateFrom, s_state.dateFrom.year != year);
    if (t) return "until " + ShortDate(s_state.dateTo, s_state.dateTo.year != year);
    return {};
}

static std::string FirstAndMore(const std::set<std::string>& values)
{
    if (values.empty()) return {};
    std::string s = *values.begin();
    if (values.size() > 1) s += "  +" + std::to_string(values.size() - 1);
    return s;
}

static std::string TeamValue()
{
    auto side = [](const std::string& name, const std::string& tag, const std::string& display) {
        return !name.empty() ? name : !tag.empty() ? "[" + tag + "]" : display;
    };
    const bool a = MatchupSlotSet(s_state.matchupNameA, s_state.matchupTagA);
    const bool b = MatchupSlotSet(s_state.matchupNameB, s_state.matchupTagB);
    if (a && b)
        return side(s_state.matchupNameA, s_state.matchupTagA, s_state.matchupDisplayA) + " vs " +
               side(s_state.matchupNameB, s_state.matchupTagB, s_state.matchupDisplayB);
    if (a) return side(s_state.matchupNameA, s_state.matchupTagA, s_state.matchupDisplayA);
    if (b) return side(s_state.matchupNameB, s_state.matchupTagB, s_state.matchupDisplayB);
    return {};
}

static std::string MoreValue()
{
    std::vector<std::string> parts;
    if (s_state.myRated) parts.push_back("Rated");
    if (s_state.myNotes) parts.push_back("Notes");
    if (s_state.myComms) parts.push_back("Comms");
    if (s_state.minRatingFilter > 0) parts.push_back("Rating " + std::to_string(s_state.minRatingFilter) + "+");
    if (!s_state.selectedBuilds.empty()) parts.push_back("Team build");
    std::string s;
    for (const auto& p : parts) s += (s.empty() ? "" : ", ") + p;
    return s;
}

// The month each flux runs in (GW rotates them monthly): the month most of the library's matches
// under it were played in. A flux with no match in the library has none.
static std::map<std::string, int> s_fluxMonth;

static void UpdateFluxMonths(const std::vector<MatchMeta>& matches)
{
    static int s_generation = -2;
    static size_t s_count = 0;
    if (s_generation == s_libraryGeneration && s_count == matches.size()) return;
    s_generation = s_libraryGeneration;
    s_count = matches.size();
    std::map<std::string, std::array<int, 13>> byMonth;
    for (const MatchMeta& m : matches)
        if (!m.flux.empty() && m.month >= 1 && m.month <= 12) byMonth[m.flux][m.month]++;
    s_fluxMonth.clear();
    for (const auto& [name, months] : byMonth)
        s_fluxMonth[name] = (int)(std::max_element(months.begin() + 1, months.end()) - months.begin());
}

// The wiki's month for a flux of the cycle; else the month most of its matches were played in.
static int FluxMonth(const std::string& name)
{
    if (const int month = GetFluxCycleMonth(name)) return month;
    const auto it = s_fluxMonth.find(name);
    return it == s_fluxMonth.end() ? 0 : it->second;
}

// "[Nov] All In".
static std::string FluxLabel(const std::string& name)
{
    const int month = FluxMonth(name);
    return month > 0 ? "[" + std::string(MonthAbbrev(month)) + "] " + name : name;
}

// What the filter button shows for an active filter (skills draw icons instead).
static std::string FilterValue(const std::string& id)
{
    if (id == "team") return TeamValue();
    if (id == "map") return FirstAndMore(s_state.selectedMaps);
    if (id == "occasion") return FirstAndMore(s_state.selectedOccasions);
    if (id == "flux")
    {
        std::set<std::string> labels;
        for (const auto& f : s_state.selectedFluxes) labels.insert(FluxLabel(f));
        return FirstAndMore(labels);
    }
    if (id == "date") return DateValue();
    if (id == "more") return MoreValue();
    return {};
}

static bool FilterActive(const std::string& id)
{
    if (id == "skills") return !s_state.selectedSkills.empty();
    return !FilterValue(id).empty();
}

static void ClearFilter(const std::string& id)
{
    if (id == "team")
    {
        s_state.matchupBufA[0] = s_state.matchupBufB[0] = '\0';
        s_state.matchupDisplayA.clear(); s_state.matchupNameA.clear(); s_state.matchupTagA.clear();
        s_state.matchupDisplayB.clear(); s_state.matchupNameB.clear(); s_state.matchupTagB.clear();
    }
    else if (id == "map")      s_state.selectedMaps.clear();
    else if (id == "occasion") s_state.selectedOccasions.clear();
    else if (id == "skills")   s_state.selectedSkills.clear();
    else if (id == "flux")     s_state.selectedFluxes.clear();
    else if (id == "date")     { s_state.dateFrom = {}; s_state.dateTo = {}; }
    else if (id == "more")
    {
        s_state.myRated = s_state.myNotes = s_state.myComms = false;
        s_state.minRatingFilter = 0;
        s_state.selectedBuilds.clear();
    }
}

// The full selection, for the filter button's tooltip.
static std::string FilterTooltip(const std::string& id)
{
    auto list = [](const std::set<std::string>& values) {
        std::string s;
        for (const auto& v : values) s += (s.empty() ? "" : "\n") + v;
        return s;
    };
    if (id == "map") return list(s_state.selectedMaps);
    if (id == "occasion") return list(s_state.selectedOccasions);
    if (id == "flux") return list(s_state.selectedFluxes);
    if (id == "skills")
    {
        std::string s;
        for (int sk : s_state.selectedSkills)
            if (const SkillInfo* si = GetSkillDatabase().Get(sk)) s += (s.empty() ? "" : "\n") + si->name;
        return s + (s_state.skillMatchMode == 0 ? "\n(all of them" : "\n(any of them") +
               (s_state.skillScope == 0 ? ", same player)" : s_state.skillScope == 1 ? ", same team)" : ", either team)");
    }
    return FilterValue(id);
}

// ─── How many matches each option would give ─────────────────────────────────

enum class CountCat { Map, Occasion, Flux, Skills, Mine, Count_ };

struct OptionCounts
{
    uint64_t key = 0;
    bool valid = false;
    std::unordered_map<std::string, int> byName;
    std::unordered_map<int, int> bySkill;   // canonical skill id
    int rated = 0, notes = 0, comms = 0, total = 0;
};

// The other filters as they stand with this one cleared, run through the list's own filter: so a
// count is exactly what ticking that option would show. Rebuilt only when a filter changes, and only
// for the open menu.
static const OptionCounts& CountsFor(CountCat cat, const std::vector<MatchMeta>& matches)
{
    static OptionCounts s_counts[(int)CountCat::Count_];
    OptionCounts& c = s_counts[(int)cat];

    std::set<std::string> savedSet;
    std::set<int> savedSkills;
    const bool rated = s_state.myRated, notes = s_state.myNotes, comms = s_state.myComms;
    const int minRating = s_state.minRatingFilter;
    switch (cat)
    {
    case CountCat::Map:      std::swap(savedSet, s_state.selectedMaps); break;
    case CountCat::Occasion: std::swap(savedSet, s_state.selectedOccasions); break;
    case CountCat::Flux:     std::swap(savedSet, s_state.selectedFluxes); break;
    case CountCat::Skills:   std::swap(savedSkills, s_state.selectedSkills); break;
    case CountCat::Mine:
        s_state.myRated = s_state.myNotes = s_state.myComms = false;
        s_state.minRatingFilter = 0;
        break;
    default: break;
    }

    uint64_t key = FilterSignature(matches) * 31u + (uint64_t)cat;
    if (s_page.favouritesOnly) key ^= 0x9e3779b97f4a7c15ull + ((uint64_t)MatchFavourites::Get().Version() << 7);
    if (cat == CountCat::Mine)
        key ^= ((uint64_t)MatchNotes::Get().Version() * 1315423911ull) ^ (Comms::BadgeVersion() * 2654435761ull);
    if (!c.valid || c.key != key)
    {
        c = OptionCounts();
        c.valid = true;
        c.key = key;
        const std::vector<FilteredMatch> base = BuildFilteredMatches(matches);
        const MatchFavourites& favs = MatchFavourites::Get();
        for (const FilteredMatch& fm : base)
        {
            const MatchMeta& m = *fm.meta;
            if (s_page.favouritesOnly && !favs.IsFavourite(m.folder_name)) continue;
            c.total++;
            switch (cat)
            {
            case CountCat::Map:      c.byName[fm.mapName]++; break;
            case CountCat::Occasion: c.byName[m.occasion]++; break;
            case CountCat::Flux:     c.byName[m.flux]++; break;
            case CountCat::Skills:
                if ((size_t)fm.originalIndex < s_matchSkillIndex.size())
                    for (int id : s_matchSkillIndex[fm.originalIndex].allUnion) c.bySkill[id]++;
                break;
            case CountCat::Mine:
                c.rated += MatchRatings::Get().GetRating(m.folder_name) > 0 ? 1 : 0;
                c.notes += MatchNotes::Get().HasNote(m.folder_name) ? 1 : 0;
                c.comms += Comms::BadgeFor(m.folder_name).present ? 1 : 0;
                break;
            default: break;
            }
        }
    }

    switch (cat)
    {
    case CountCat::Map:      std::swap(savedSet, s_state.selectedMaps); break;
    case CountCat::Occasion: std::swap(savedSet, s_state.selectedOccasions); break;
    case CountCat::Flux:     std::swap(savedSet, s_state.selectedFluxes); break;
    case CountCat::Skills:   std::swap(savedSkills, s_state.selectedSkills); break;
    case CountCat::Mine:
        s_state.myRated = rated;
        s_state.myNotes = notes;
        s_state.myComms = comms;
        s_state.minRatingFilter = minRating;
        break;
    default: break;
    }
    return c;
}

static int CountOf(const OptionCounts& c, const std::string& name)
{
    auto it = c.byName.find(name);
    return it == c.byName.end() ? 0 : it->second;
}

static int MapIdOf(const std::string& name, const std::vector<MatchMeta>& matches)
{
    static std::unordered_map<std::string, int> s_ids;
    static int s_generation = -2;
    if (s_generation != s_libraryGeneration)
    {
        s_ids.clear();
        s_generation = s_libraryGeneration;
        for (const MatchMeta& m : matches)
        {
            const char* mn = GetMapName(m.map_id);
            s_ids.try_emplace(mn ? std::string(mn) : "Map " + std::to_string(m.map_id), m.map_id);
        }
    }
    auto it = s_ids.find(name);
    return it == s_ids.end() ? 0 : it->second;
}

// ─── The menus' parts ────────────────────────────────────────────────────────
// Every part draws at the cursor, s_menuW wide, and leaves the cursor exactly below itself, so the
// menu's rows tile with the spacing written here rather than the style's.

static float s_menuW = 0.0f;
static bool s_menuJustOpened = false;   // a menu with a field opened this frame: the field takes the keys

static void MenuGap(float css)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(s_menuW, Px(css)));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + Px(css)));
}

// The menu's title in spaced capitals, Clear at the right while it holds anything, and a rule.
// Returns true when Clear was clicked.
static bool MenuHeader(const char* title, bool active)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const Type lt = T(10), ct = T(12);
    const float lineH = LineH(12);
    TextSpaced(dl, lt, ImVec2(p.x, p.y + (lineH - LineH(10)) * 0.5f), col::Labels, title, Px(1.1f));
    bool clear = false;
    if (active)
    {
        const float cw = TextW(ct, "Clear");
        clear = LinkText(dl, "##menu_clear", ct, ImVec2(R(p.x + s_menuW - cw), p.y), cw + 1.0f, col::Link, "Clear");
    }
    const float ruleY = R(p.y + lineH + Px(8));
    HRule(dl, p.x, p.x + s_menuW, ruleY, col::Rule);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(s_menuW, ruleY + 1.0f + Px(6) - p.y));
    ImGui::SetCursorScreenPos(ImVec2(p.x, ruleY + 1.0f + Px(6)));
    return clear;
}

static void MenuSection(const char* title)
{
    MenuGap(10);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    TextSpaced(dl, T(10), p, col::Labels, title, Px(1.1f));
    ImGui::Dummy(ImVec2(s_menuW, LineH(10) + Px(6)));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + LineH(10) + Px(6)));
}

static void MenuNote(const char* text, ImU32 c = col::Muted)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    TextFit(dl, T(12), ImVec2(p.x + Px(6), p.y + Px(4)), s_menuW - Px(12), c, text);
    ImGui::Dummy(ImVec2(s_menuW, LineH(12) + Px(8)));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + LineH(12) + Px(8)));
}

// A box: ticked (1), empty (0) or partly ticked (2).
static void DrawCheckBox(ImDrawList* dl, ImVec2 p, float s, int state, bool hov)
{
    const ImVec2 b(p.x + s, p.y + s);
    if (state == 1)
    {
        dl->AddRectFilled(p, b, col::SelBar, Px(3));
        const float t = std::max(1.5f, Px(1.8f));
        dl->AddLine(ImVec2(p.x + s * 0.22f, p.y + s * 0.52f), ImVec2(p.x + s * 0.42f, p.y + s * 0.72f), col::ReplayText, t);
        dl->AddLine(ImVec2(p.x + s * 0.42f, p.y + s * 0.72f), ImVec2(p.x + s * 0.78f, p.y + s * 0.30f), col::ReplayText, t);
        return;
    }
    dl->AddRectFilled(p, b, col::SearchBg, Px(3));
    Outline(dl, p, b, state == 2 ? col::SelBar : hov ? col::ActionHovRule : Hex(0x5c5648), Px(3));
    if (state == 2)
        dl->AddRectFilled(ImVec2(R(p.x + s * 0.25f), R(p.y + s * 0.44f)), ImVec2(R(p.x + s * 0.75f), R(p.y + s * 0.44f) + std::max(2.0f, Px(2))),
                          col::SelBar);
}

// One option of a checklist: the box, an optional picture (a hall's thumbnail, or an icon), the
// label and how many matches it would give (count < 0: none shown). Returns true on a click.
enum class RowMark { None, Star, Note, Headset };

static bool CheckRow(const char* id, int state, const char* label, int count, bool picture, const ArtTexture* art,
                     ImTextureID icon, bool strong, float indent, ImU32 labelCol = 0, RowMark mark = RowMark::None)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = picture ? Px(38) : icon ? Px(32) : Px(28);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(s_menuW, rowH));
    const bool hov = ImGui::IsItemHovered();
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + s_menuW, p.y + rowH), Hex(0xffffff, 12), Px(3));
    float x = p.x + Px(6) + indent;
    const float cy = p.y + rowH * 0.5f;
    DrawCheckBox(dl, ImVec2(x, R(cy - Px(7))), Px(14), state, hov);
    x += Px(14) + Px(10);
    // The mark the list itself shows (the rating star, the notes scroll, the comms headset).
    if (mark != RowMark::None)
    {
        const float m = Px(16);
        const ImVec2 a(R(x), R(cy - m * 0.5f));
        if (mark == RowMark::Star)
            DrawStar(dl, ImVec2(a.x + m * 0.5f, a.y + m * 0.5f), m * 0.47f, col::StarOn, true, 1.0f);
        else if (mark == RowMark::Note)
        {
            ImVec2 uv0, uv1;
            if (ImTextureID scroll = GetNotesScrollIcon(&uv0, &uv1))
            {
                const float iw = m * 22.0f / 23.0f;
                dl->AddImage(scroll, ImVec2(a.x + (m - iw) * 0.5f, a.y), ImVec2(a.x + (m + iw) * 0.5f, a.y + m), uv0, uv1,
                             ImGui::ColorConvertFloat4ToU32(kNotesScrollTint));
            }
        }
        else
            DrawHeadsetGlyph(dl, a, m, IM_COL32(200, 168, 75, 255));
        x = a.x + m + Px(9);
    }
    if (picture)
    {
        const ImVec2 a(x, R(cy - Px(14))), b(a.x + Px(44), a.y + Px(28));
        if (art) ImageCover(dl, (ImTextureID)art->srv.Get(), art->w, art->h, a, b, IM_COL32_WHITE, Px(2));
        else     dl->AddRectFilled(a, b, Hex(0x2a2823), Px(2));
        x = b.x + Px(10);
    }
    else if (icon)
    {
        const ImVec2 a(x, R(cy - Px(12))), b(a.x + Px(24), a.y + Px(24));
        dl->AddImage(icon, a, b);
        Outline(dl, a, b, col::IconRule, 0.0f);
        x = b.x + Px(10);
    }
    const Type ct = T(12);
    char cnt[16] = "";
    if (count >= 0) snprintf(cnt, sizeof(cnt), "%d", count);
    const float cntW = count >= 0 ? TextW(ct, cnt) : 0.0f;
    const ImU32 lc = labelCol ? labelCol : state == 1 ? col::FilterOn : count == 0 ? WithAlpha(col::Filters, 0.55f) : col::Text;
    TextFit(dl, T(13, strong), ImVec2(x, cy - LineH(13) * 0.5f), p.x + s_menuW - Px(10) - cntW - Px(10) - x, lc, label);
    if (count >= 0)
        Text(dl, ct, ImVec2(p.x + s_menuW - Px(10) - cntW, cy - LineH(12) * 0.5f), col::Muted, cnt);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    return clicked;
}

// A rounded toggle in a row of them. Advances *x.
static bool Pill(const char* id, const char* label, bool on, float& x, float y, float w = 0.0f)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Type t = T(12);
    const float h = Px(26);
    if (w <= 0.0f) w = Px(12) * 2.0f + TextW(t, label);
    const ImVec2 p(R(x), R(y));
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 b(p.x + w, p.y + h);
    dl->AddRectFilled(p, b, on ? Hex(0x3a3221) : hov ? col::ActionHovBg : col::ActionBg, h * 0.5f);
    Outline(dl, p, b, on ? col::SelBar : hov ? col::ActionHovRule : col::ActionRule, h * 0.5f);
    Text(dl, t, ImVec2(p.x + (w - TextW(t, label)) * 0.5f, p.y + (h - LineH(12)) * 0.5f), on ? col::FilterOn : col::ActionText, label);
    x += w + Px(6);
    return clicked;
}

// A row of pills: the cursor is left below it.
static void PillRowEnd(float startX, float y)
{
    ImGui::SetCursorScreenPos(ImVec2(startX, y));
    ImGui::Dummy(ImVec2(s_menuW, Px(26)));
    ImGui::SetCursorScreenPos(ImVec2(startX, y + Px(26)));
}

// The menus' text field: the search box's look at 13 px. `enter` reports Enter in it.
static bool MenuInput(const char* id, const char* hint, char* buf, size_t size, bool focusOnOpen, bool* enter)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = R(Px(32));
    dl->AddRectFilled(p, ImVec2(p.x + s_menuW, p.y + h), col::SearchBg, Px(3));
    HRule(dl, p.x + Px(2), p.x + s_menuW - Px(2), p.y + h - 1.0f, col::SearchRule);
    DrawMagnifier(dl, ImVec2(p.x + Px(15), p.y + h * 0.5f), Px(12), col::SearchText);
    const Type t = T(13);
    ImGui::SetCursorScreenPos(ImVec2(p.x + Px(28), R(p.y + (h - t.font->FontSize) * 0.5f)));
    ImGui::PushFont(t.font);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col::Text));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(col::SearchText));
    ImGui::SetNextItemWidth(s_menuW - Px(36));
    if (focusOnOpen && (ImGui::IsWindowAppearing() || s_menuJustOpened)) ImGui::SetKeyboardFocusHere();
    s_menuJustOpened = false;
    const bool edited = ImGui::InputTextWithHint(id, hint, buf, size);
    if (enter)
        *enter = (ImGui::IsItemActive() || ImGui::IsItemDeactivated()) &&
                 (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(s_menuW, h));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    return edited;
}

// ─── The menus ───────────────────────────────────────────────────────────────

// A query typed in a menu belongs to that menu: opening another one clears both.
static void SyncMenuQueries()
{
    if (s_page.queriedMenu == s_page.openMenu) return;
    s_page.queriedMenu = s_page.openMenu;
    s_menuJustOpened = true;
    s_page.mapQuery[0] = '\0';
    s_page.fluxQuery[0] = '\0';
}

static void MenuMap(const std::vector<MatchMeta>& matches)
{
    SyncMenuQueries();
    if (MenuHeader("MAP", !s_state.selectedMaps.empty())) s_state.selectedMaps.clear();
    const OptionCounts& c = CountsFor(CountCat::Map, matches);

    // Type a map: the halls whose name holds it, the most played first; Enter takes the first.
    bool enter = false;
    MenuInput("##lib_map_query", "Type a map\xE2\x80\xA6", s_page.mapQuery, sizeof(s_page.mapQuery), true, &enter);
    const std::string query = ToLower(std::string(s_page.mapQuery));
    std::vector<size_t> rows;
    for (size_t i = 1; i < s_state.mapNames.size(); i++)
        if (query.empty() || ToLower(s_state.mapNames[i]).find(query) != std::string::npos) rows.push_back(i);
    if (!query.empty())
    {
        std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
            return CountOf(c, s_state.mapNames[a]) > CountOf(c, s_state.mapNames[b]);
        });
        if (enter && !rows.empty())
        {
            const std::string& first = s_state.mapNames[rows[0]];
            if (s_state.selectedMaps.count(first)) s_state.selectedMaps.erase(first);
            else                                   s_state.selectedMaps.insert(first);
            s_page.mapQuery[0] = '\0';
        }
        MenuGap(4);
        if (rows.empty()) MenuNote("No map by that name.");
    }
    else
        MenuGap(4);
    for (const size_t i : rows)
    {
        const std::string& name = s_state.mapNames[i];
        const bool on = s_state.selectedMaps.count(name) > 0;
        const ArtTexture* art = LoadArt(HallArtPath(MapIdOf(name, matches)));
        ImGui::PushID((int)i);
        if (CheckRow("##map", on ? 1 : 0, name.c_str(), CountOf(c, name), true, art, nullptr, false, 0.0f))
        {
            if (on) s_state.selectedMaps.erase(name);
            else    s_state.selectedMaps.insert(name);
        }
        ImGui::PopID();
    }
}

static void MenuOccasion(const std::vector<MatchMeta>& matches)
{
    if (MenuHeader("OCCASION", !s_state.selectedOccasions.empty())) s_state.selectedOccasions.clear();
    const OptionCounts& c = CountsFor(CountCat::Occasion, matches);
    static const char* kGroups[] = { "Monthly AT (mAT)", "Automated Tournament", "Scrimmage", "Other" };
    std::map<std::string, std::vector<const std::string*>> groups;
    for (size_t i = 1; i < s_state.occasionNames.size(); i++)
        groups[OccasionGroupOf(s_state.occasionNames[i])].push_back(&s_state.occasionNames[i]);
    int row = 0;
    for (const char* group : kGroups)
    {
        auto git = groups.find(group);
        if (git == groups.end() || git->second.empty()) continue;
        std::vector<const std::string*> items = git->second;
        std::sort(items.begin(), items.end(), [](const std::string* a, const std::string* b) {
            const int oa = GetOccasionOrder(*a), ob = GetOccasionOrder(*b);
            return oa != ob ? oa < ob : *a < *b;
        });
        int on = 0, total = 0;
        for (const std::string* occ : items)
        {
            on += s_state.selectedOccasions.count(*occ) ? 1 : 0;
            total += CountOf(c, *occ);
        }
        const int state = on == (int)items.size() ? 1 : on > 0 ? 2 : 0;
        ImGui::PushID(row++);
        if (CheckRow("##group", state, group, total, false, nullptr, nullptr, true, 0.0f, state ? col::FilterOn : col::Text))
            for (const std::string* occ : items)
            {
                if (state == 1) s_state.selectedOccasions.erase(*occ);
                else            s_state.selectedOccasions.insert(*occ);
            }
        for (const std::string* occ : items)
        {
            const bool sel = s_state.selectedOccasions.count(*occ) > 0;
            ImGui::PushID(occ->c_str());
            if (CheckRow("##occ", sel ? 1 : 0, occ->c_str(), CountOf(c, *occ), false, nullptr, nullptr, false, Px(22)))
            {
                if (sel) s_state.selectedOccasions.erase(*occ);
                else     s_state.selectedOccasions.insert(*occ);
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}

// Does a flux answer the Flux menu's query: its name, or its month ("nov", "november", "11")?
static bool FluxMatches(const std::string& name, const std::string& query)
{
    if (query.empty()) return true;
    if (ToLower(name).find(query) != std::string::npos) return true;
    static const char* const kMonths[] = { "", "january", "february", "march", "april", "may", "june",
                                           "july", "august", "september", "october", "november", "december" };
    const int month = FluxMonth(name);
    if (month < 1 || month > 12) return false;
    const std::string full = kMonths[month];
    if (full.rfind(query, 0) == 0) return true;   // "nov", "novem"
    return query == std::to_string(month) || query == (month < 10 ? "0" : "") + std::to_string(month);
}

static void MenuFlux(const std::vector<MatchMeta>& matches)
{
    SyncMenuQueries();
    if (MenuHeader("FLUX", !s_state.selectedFluxes.empty())) s_state.selectedFluxes.clear();
    const OptionCounts& c = CountsFor(CountCat::Flux, matches);

    // Type a flux or a month: the matching ones; Enter takes the first.
    bool enter = false;
    MenuInput("##lib_flux_query", "Type a flux or a month\xE2\x80\xA6", s_page.fluxQuery, sizeof(s_page.fluxQuery), true, &enter);
    const std::string query = ToLower(std::string(s_page.fluxQuery));
    MenuGap(4);

    // In calendar order, January first; a flux with no month last.
    std::vector<size_t> order;
    for (size_t i = 1; i < s_state.fluxNames.size(); i++)
        if (FluxMatches(s_state.fluxNames[i], query)) order.push_back(i);
    if (!query.empty() && order.empty()) MenuNote("No flux by that name or month.");
    std::stable_sort(order.begin(), order.end(), [](size_t a, size_t b) {
        const int ma = FluxMonth(s_state.fluxNames[a]), mb = FluxMonth(s_state.fluxNames[b]);
        return (ma > 0 ? ma : 13) < (mb > 0 ? mb : 13);
    });
    if (enter && !order.empty())
    {
        const std::string& first = s_state.fluxNames[order[0]];
        if (s_state.selectedFluxes.count(first)) s_state.selectedFluxes.erase(first);
        else                                     s_state.selectedFluxes.insert(first);
        s_page.fluxQuery[0] = '\0';
    }
    const ImTextureID fluxIcon = GetFluxIcon();
    for (const size_t i : order)
    {
        const std::string& name = s_state.fluxNames[i];
        const bool on = s_state.selectedFluxes.count(name) > 0;
        const std::string label = FluxLabel(name);
        ImGui::PushID((int)i);
        if (CheckRow("##flux", on ? 1 : 0, label.c_str(), CountOf(c, name), false, nullptr, fluxIcon, false, 0.0f))
        {
            if (on) s_state.selectedFluxes.erase(name);
            else    s_state.selectedFluxes.insert(name);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(320.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", name.c_str());
            if (const char* desc = GetFluxDescription(name))
            {
                ImGui::Spacing();
                ImGui::TextUnformatted(desc);
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }
}

static void ToggleSkill(int id)
{
    if (s_state.selectedSkills.count(id)) s_state.selectedSkills.erase(id);
    else                                  s_state.selectedSkills.insert(id);
}

static void MenuSkills(const std::vector<MatchMeta>& matches)
{
    if (MenuHeader("SKILLS", !s_state.selectedSkills.empty())) s_state.selectedSkills.clear();
    const OptionCounts& c = CountsFor(CountCat::Skills, matches);
    auto countOf = [&](int id) { auto it = c.bySkill.find(id); return it == c.bySkill.end() ? 0 : it->second; };
    ImDrawList* dl = ImGui::GetWindowDrawList();

    bool enter = false;
    MenuInput("##lib_skill_query", "Search skills\xE2\x80\xA6", s_state.skillSearchBuf, sizeof(s_state.skillSearchBuf), true, &enter);
    const std::string query = ToLower(std::string(s_state.skillSearchBuf));

    if (!query.empty())
    {
        // Results: the skills played most under the other filters first.
        std::vector<const SkillOption*> hits;
        for (const SkillOption& o : s_skillOptions)
            if (o.nameLower.find(query) != std::string::npos) hits.push_back(&o);
        std::sort(hits.begin(), hits.end(), [&](const SkillOption* a, const SkillOption* b) {
            const int ca = countOf(a->id), cb = countOf(b->id);
            if (ca != cb) return ca > cb;
            return a->nameLower < b->nameLower;
        });
        if (enter && !hits.empty())
        {
            ToggleSkill(hits[0]->id);
            s_state.skillSearchBuf[0] = '\0';
        }
        MenuGap(4);
        if (hits.empty()) MenuNote("No skill by that name.");
        const size_t shown = std::min<size_t>(hits.size(), 10);
        for (size_t i = 0; i < shown; i++)
        {
            const SkillOption& o = *hits[i];
            const bool on = s_state.selectedSkills.count(o.id) > 0;
            ImGui::PushID(o.id);
            if (CheckRow("##skill", on ? 1 : 0, o.name.c_str(), countOf(o.id), false, nullptr, GetSkillIcon(o.id), o.isElite,
                         0.0f, o.isElite && !on ? col::Winner : 0))
                ToggleSkill(o.id);
            if (ImGui::IsItemHovered()) DrawSkillTooltip(o.id);
            ImGui::PopID();
        }
        if (hits.size() > shown)
        {
            const std::string more = "+" + std::to_string(hits.size() - shown) + " more: keep typing";
            MenuNote(more.c_str());
        }
    }
    else
    {
        // The elites played most under the other filters: one click each.
        MenuSection("MOST PLAYED ELITES");
        std::vector<std::pair<int, int>> elites;   // count, id
        for (const SkillOption& o : s_skillOptions)
            if (o.isElite)
                if (const int n = countOf(o.id); n > 0) elites.push_back({ n, o.id });
        std::sort(elites.begin(), elites.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        if (elites.size() > 21) elites.resize(21);
        const float icon = Px(34), gap = Px(5);
        const int perRow = std::max(1, (int)((s_menuW + gap) / (icon + gap)));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        for (size_t i = 0; i < elites.size(); i++)
        {
            const int id = elites[i].second;
            const ImVec2 a(R(p.x + (i % perRow) * (icon + gap)), R(p.y + (i / perRow) * (icon + gap)));
            const ImVec2 b(a.x + icon, a.y + icon);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(id);
            if (ImGui::InvisibleButton("##elite", ImVec2(icon, icon))) ToggleSkill(id);
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (ImTextureID tex = GetSkillIcon(id)) dl->AddImage(tex, a, b);
            const bool on = s_state.selectedSkills.count(id) > 0;
            if (on)
            {
                dl->AddRect(ImVec2(a.x - 1.0f, a.y - 1.0f), ImVec2(b.x + 1.0f, b.y + 1.0f), col::SelBar, 0.0f, 0, std::max(2.0f, Px(2)));
            }
            else
                Outline(dl, a, b, hov ? col::ActionHovRule : col::IconRule, 0.0f);
            if (hov)
            {
                DrawSkillTooltip(id);
                ImGui::BeginTooltip();
                ImGui::TextColored(kColorTextDim, "In %d match%s here. Click to %s.", elites[i].first, elites[i].first == 1 ? "" : "es",
                                   on ? "remove it" : "filter by it");
                ImGui::EndTooltip();
            }
        }
        const int rows = elites.empty() ? 0 : (int)((elites.size() + perRow - 1) / perRow);
        const float gridH = rows > 0 ? rows * icon + (rows - 1) * gap : 0.0f;
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(s_menuW, std::max(1.0f, gridH)));
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + gridH));
        if (elites.empty()) MenuNote("No elite in the matches left by the other filters.");
    }

    // The picked skills, each with its own x.
    if (!s_state.selectedSkills.empty())
    {
        MenuSection("SELECTED");
        const Type t = T(12);
        const float h = Px(26), icon = Px(20);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        float x = p.x, y = p.y;
        int remove = 0;
        for (int id : s_state.selectedSkills)
        {
            const SkillInfo* si = GetSkillDatabase().Get(id);
            const std::string name = si ? si->name : "#" + std::to_string(id);
            const float w = Px(4) + icon + Px(6) + std::min(TextW(t, name.c_str()), Px(150)) + Px(6) + Px(9) + Px(8);
            if (x > p.x && x + w > p.x + s_menuW) { x = p.x; y += h + Px(6); }
            const ImVec2 a(R(x), R(y)), b(a.x + w, a.y + h);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(id);
            if (ImGui::InvisibleButton("##sel", ImVec2(w, h))) remove = id;
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            dl->AddRectFilled(a, b, hov ? Hex(0x3a3428) : Hex(0x2a2720), Px(3));
            if (ImTextureID tex = GetSkillIcon(id))
                dl->AddImage(tex, ImVec2(a.x + Px(4), R(a.y + (h - icon) * 0.5f)), ImVec2(a.x + Px(4) + icon, R(a.y + (h - icon) * 0.5f) + icon));
            TextFit(dl, t, ImVec2(a.x + Px(4) + icon + Px(6), a.y + (h - LineH(12)) * 0.5f), Px(150), col::ButtonText, name.c_str());
            const ImVec2 xc(b.x - Px(8) - Px(4.5f), a.y + h * 0.5f);
            const float r = Px(3);
            const ImU32 xcol = hov ? col::ToggleOn : col::SearchText;
            dl->AddLine(ImVec2(xc.x - r, xc.y - r), ImVec2(xc.x + r, xc.y + r), xcol, 1.3f);
            dl->AddLine(ImVec2(xc.x - r, xc.y + r), ImVec2(xc.x + r, xc.y - r), xcol, 1.3f);
            if (hov) ImGui::SetTooltip("Remove %s", name.c_str());
            x += w + Px(6);
        }
        if (remove) s_state.selectedSkills.erase(remove);
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(s_menuW, y + h - p.y));
        ImGui::SetCursorScreenPos(ImVec2(p.x, y + h));

        MenuSection("A MATCH NEEDS");
        float px = p.x;
        const float ry = ImGui::GetCursorScreenPos().y;
        if (Pill("##mode_all", "All of them", s_state.skillMatchMode == 0, px, ry)) s_state.skillMatchMode = 0;
        if (Pill("##mode_any", "Any of them", s_state.skillMatchMode == 1, px, ry)) s_state.skillMatchMode = 1;
        PillRowEnd(p.x, ry);
        MenuGap(6);
        px = p.x;
        const float ry2 = ImGui::GetCursorScreenPos().y;
        if (Pill("##scope_player", "Same player", s_state.skillScope == 0, px, ry2)) s_state.skillScope = 0;
        if (Pill("##scope_team", "Same team", s_state.skillScope == 1, px, ry2)) s_state.skillScope = 1;
        if (Pill("##scope_any", "Either team", s_state.skillScope == 2, px, ry2)) s_state.skillScope = 2;
        PillRowEnd(p.x, ry2);
    }
}

// One side of the Team menu: the guild picked (with its x), or a field with suggestions under it.
static void TeamSlot(const char* id, const char* title, const char* hint, char* buf, size_t size, std::string& display,
                     std::string& name, std::string& tag, bool focus)
{
    MenuSection(title);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!display.empty())
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = R(Px(32));
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(id);
        const bool clear = ImGui::InvisibleButton("##picked", ImVec2(s_menuW, h));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        dl->AddRectFilled(p, ImVec2(p.x + s_menuW, p.y + h), hov ? Hex(0x3a3428) : Hex(0x2a2720), Px(3));
        TextFit(dl, T(13), ImVec2(p.x + Px(10), p.y + (h - LineH(13)) * 0.5f), s_menuW - Px(40), col::FilterOn, display.c_str());
        const ImVec2 xc(p.x + s_menuW - Px(14), p.y + h * 0.5f);
        const float r = Px(3.5f);
        const ImU32 xcol = hov ? col::ToggleOn : col::SearchText;
        dl->AddLine(ImVec2(xc.x - r, xc.y - r), ImVec2(xc.x + r, xc.y + r), xcol, 1.4f);
        dl->AddLine(ImVec2(xc.x - r, xc.y + r), ImVec2(xc.x + r, xc.y - r), xcol, 1.4f);
        if (hov) ImGui::SetTooltip("Remove %s", display.c_str());
        if (clear)
        {
            buf[0] = '\0';
            display.clear();
            name.clear();
            tag.clear();
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
        return;
    }
    bool enter = false;
    MenuInput(id, hint, buf, size, focus, &enter);
    const std::string query = ToLower(std::string(buf));
    if (query.empty()) return;
    std::vector<const std::string*> hits;
    for (size_t i = 1; i < s_state.guildNames.size() && hits.size() < 8; i++)
        if (ToLower(s_state.guildNames[i]).find(query) != std::string::npos) hits.push_back(&s_state.guildNames[i]);
    auto pick = [&](const std::string& picked) {
        display = picked;
        SplitGuildDisplay(display, name, tag);
        snprintf(buf, size, "%s", display.c_str());
    };
    if (enter && !hits.empty())
    {
        pick(*hits[0]);
        return;
    }
    MenuGap(4);
    if (hits.empty()) MenuNote("No guild by that name.");
    for (size_t i = 0; i < hits.size(); i++)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = Px(28);
        ImGui::PushID((int)i);
        const bool clicked = ImGui::InvisibleButton("##hit", ImVec2(s_menuW, h));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) dl->AddRectFilled(p, ImVec2(p.x + s_menuW, p.y + h), Hex(0xffffff, 12), Px(3));
        TextFit(dl, T(13), ImVec2(p.x + Px(10), p.y + (h - LineH(13)) * 0.5f), s_menuW - Px(20), hov ? col::FilterOn : col::Text,
                hits[i]->c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
        if (clicked)
        {
            pick(*hits[i]);
            break;
        }
    }
}

static void MenuTeam()
{
    if (MenuHeader("TEAM", FilterActive("team"))) ClearFilter("team");
    TeamSlot("##lib_team_a", "GUILD", "Search a guild\xE2\x80\xA6", s_state.matchupBufA, sizeof(s_state.matchupBufA),
             s_state.matchupDisplayA, s_state.matchupNameA, s_state.matchupTagA, true);
    TeamSlot("##lib_team_b", "AGAINST (OPTIONAL)", "Any opponent\xE2\x80\xA6", s_state.matchupBufB, sizeof(s_state.matchupBufB),
             s_state.matchupDisplayB, s_state.matchupNameB, s_state.matchupTagB, false);
}

static void MenuDate()
{
    if (MenuHeader("DATE", FilterActive("date"))) ClearFilter("date");
    const std::vector<DatePreset> presets = DatePresets();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float gap = Px(6), w = (s_menuW - gap * 2.0f) / 3.0f;
    float y = p.y;
    for (size_t i = 0; i < presets.size(); i++)
    {
        if (i > 0 && i % 3 == 0) y += Px(26) + gap;
        float x = p.x + (i % 3) * (w + gap);
        const DatePreset& d = presets[i];
        const bool on = SameDate(d.from, s_state.dateFrom) && SameDate(d.to, s_state.dateTo);
        ImGui::PushID((int)i);
        if (Pill("##preset", d.label, on, x, y, w))
        {
            if (on) ClearFilter("date");
            else { s_state.dateFrom = d.from; s_state.dateTo = d.to; }
        }
        ImGui::PopID();
    }
    PillRowEnd(p.x, y);

    // A range of one's own: the calendars the legacy sidebar uses.
    MenuSection("FROM - TO");
    const float rowY = ImGui::GetCursorScreenPos().y;
    ImGui::SetCursorScreenPos(ImVec2(p.x, rowY));
    ImGui::PushItemWidth((s_menuW - Px(30)) * 0.5f);
    const bool fromChanged = DrawCalendarPicker("lib_from", s_state.dateFrom, s_state.calBrowseFromMonth, s_state.calBrowseFromYear);
    ImGui::SameLine(0, Px(10));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::Muted), "-");
    ImGui::SameLine(0, Px(10));
    const bool toChanged = DrawCalendarPicker("lib_to", s_state.dateTo, s_state.calBrowseToMonth, s_state.calBrowseToYear);
    ImGui::PopItemWidth();
    if (DateValValid(s_state.dateFrom) && DateValValid(s_state.dateTo) &&
        CompareDate(s_state.dateFrom.day, s_state.dateFrom.month, s_state.dateFrom.year, s_state.dateTo.day,
                    s_state.dateTo.month, s_state.dateTo.year) > 0)
    {
        if (fromChanged) s_state.dateTo = {};
        else if (toChanged) s_state.dateFrom = {};
    }
}

static void MenuMore(const std::vector<MatchMeta>& matches)
{
    if (MenuHeader("MY MATCHES", FilterActive("more"))) ClearFilter("more");
    const OptionCounts& c = CountsFor(CountCat::Mine, matches);
    if (CheckRow("##mine_rated", s_state.myRated ? 1 : 0, "Rated by me", c.rated, false, nullptr, nullptr, false, 0.0f, 0,
                 RowMark::Star))
        s_state.myRated = !s_state.myRated;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Matches you gave a rating");
    if (CheckRow("##mine_notes", s_state.myNotes ? 1 : 0, "With my notes", c.notes, false, nullptr, nullptr, false, 0.0f, 0,
                 RowMark::Note))
        s_state.myNotes = !s_state.myNotes;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Matches with notes");
    if (CheckRow("##mine_comms", s_state.myComms ? 1 : 0, "With voice comms", c.comms, false, nullptr, nullptr, false, 0.0f, 0,
                 RowMark::Headset))
        s_state.myComms = !s_state.myComms;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Matches with voice comms attached on this PC (a missing file counts too)");

    MenuSection("MINIMUM RATING");
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float pitch = Px(22), r = Px(7.5f), h = Px(24);
        ImGui::SetCursorScreenPos(ImVec2(p.x + Px(6), p.y));
        ImGui::InvisibleButton("##min_rating", ImVec2(pitch * 5.0f, h));
        const bool hov = ImGui::IsItemHovered();
        int shown = s_state.minRatingFilter;
        if (hov)
        {
            shown = std::clamp((int)((ImGui::GetIO().MousePos.x - p.x - Px(6)) / pitch) + 1, 1, 5);
            if (ImGui::IsItemClicked()) s_state.minRatingFilter = shown == s_state.minRatingFilter ? 0 : shown;
        }
        for (int i = 0; i < 5; i++)
            DrawStar(dl, ImVec2(p.x + Px(6) + pitch * i + r, p.y + h * 0.5f), r, i < shown ? (hov ? col::LinkHover : col::StarOn) : col::Rating,
                     i < shown, 1.1f);
        const std::string label = s_state.minRatingFilter > 0 ? std::to_string(s_state.minRatingFilter) + " stars or more" : "Any rating";
        Text(dl, T(12), ImVec2(p.x + Px(6) + pitch * 5.0f + Px(10), p.y + (h - LineH(12)) * 0.5f), col::Muted, label.c_str());
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(s_menuW, h));
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    }

    // A team-build filter set in the legacy layout still narrows this list: say so here.
    if (!s_state.selectedBuilds.empty())
    {
        MenuSection("FROM THE LEGACY LAYOUT");
        MenuNote("A team build filter is on.", col::Notice);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (LinkText(ImGui::GetWindowDrawList(), "##clear_builds", T(12), ImVec2(p.x + Px(6), p.y), s_menuW, col::Link, "Clear it"))
            s_state.selectedBuilds.clear();
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(s_menuW, LineH(12) + Px(4)));
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + LineH(12) + Px(4)));
    }
}

// The open filter menu: a plain window under its button, not a popup, since the Date menu's
// calendars are popups of their own. It closes itself: a click outside it, or Escape.
// V41: the open filter is a panel in the page's flow, between the filter row and the list, which it
// pushes down; it never covers a match. One group open at a time. 16 px padding, 8 px rounding,
// linear-gradient(110deg, #302f29, #262621), a 1 px white 8% outline, 18 px below it. Taller than
// about half the page, it scrolls. Returns the height it takes, its margin included.
static float DrawFilterPanel(const std::vector<MatchMeta>& matches, float x0, float x1, float y, float maxH)
{
    if (s_page.openMenu.empty()) return 0.0f;
    const std::string id = s_page.openMenu;
    const float w = x1 - x0, pad = Px(16);
    s_menuW = std::min(w - pad * 2.0f - Px(10), id == "skills" ? Px(760) : Px(560));
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, std::max(Px(160), maxH)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(Hex(0x4a463b)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Px(8));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    bool hovered = false;
    if (ImGui::BeginChild("##lib_filter_panel", ImVec2(w, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav))
    {
        {
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            const ImVec2 pa = ImGui::GetWindowPos(), ps = ImGui::GetWindowSize();
            const ImVec2 pb(pa.x + ps.x, pa.y + ps.y);
            FillLinear(pdl, pa, pb, Px(8), 110.0f, v41::MenuA, v41::MenuB);
            Outline(pdl, pa, pb, v41::MenuLine, Px(8));
        }
        if (id == "team")          MenuTeam();
        else if (id == "map")      MenuMap(matches);
        else if (id == "occasion") MenuOccasion(matches);
        else if (id == "skills")   MenuSkills(matches);
        else if (id == "flux")     MenuFlux(matches);
        else if (id == "date")     MenuDate();
        else if (id == "more")     MenuMore(matches);
        hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                         ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    }
    ImGui::EndChild();
    const float h = ImGui::GetItemRectSize().y;
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);

    const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered && !popupOpen && !s_page.menuLabelHovered &&
        !ImGui::IsMouseHoveringRect(ImVec2(x0, y), ImVec2(x1, y + h)))
        s_page.openMenu.clear();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !popupOpen)
        s_page.openMenu.clear();
    return h + Px(18);
}

// ─── The filter row ──────────────────────────────────────────────────────────

struct SortOption
{
    const char* label;
    int column;
    bool ascending;
};
static const SortOption kSortOptions[] = {
    { "Newest first", 0, false }, { "Oldest first", 0, true }, { "Rating", 9, false },
    { "Map", 2, true },           { "Occasion", 1, true },     { "Team", 3, true },
};

struct FilterMenu
{
    const char* id;
    const char* label;
};
static const FilterMenu kFilterMenus[] = {
    { "team", "Team" }, { "map", "Map" },   { "occasion", "Occasion" }, { "skills", "Skills" },
    { "flux", "Flux" }, { "date", "Date" }, { "more", "More" },
};

// An active filter: a framed button with its name, what it holds (the skills as their icons) and an
// x that clears it. The button opens the menu; the x does not.
static float FilterChipWidth(const FilterMenu& f, const std::string& value)
{
    const Type t = T(11);
    float w = Px(11) + TextW(t, f.label) + Px(8);
    if (std::string(f.id) == "skills")
    {
        const int n = (int)s_state.selectedSkills.size(), shown = std::min(n, 4);
        w += shown * Px(18) + (shown - 1) * Px(3);
        if (n > shown) w += Px(6) + TextW(t, ("+" + std::to_string(n - shown)).c_str());
    }
    else
        w += std::min(TextW(t, value.c_str()), Px(230));
    return w + Px(10) + Px(9) + Px(10);
}

static void DrawFilterChip(ImDrawList* dl, const FilterMenu& f, const std::string& value, ImVec2 pos, float w, float h)
{
    const Type t = T(11);
    const bool open = s_page.openMenu == f.id;
    const float xW = Px(10) + Px(9) + Px(10);
    ImGui::PushID(f.id);
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton("##chip", ImVec2(w - xW, h));
    const bool hov = ImGui::IsItemHovered();
    ImGui::SetCursorScreenPos(ImVec2(pos.x + w - xW, pos.y));
    const bool cleared = ImGui::InvisibleButton("##clear", ImVec2(xW, h));
    const bool xHov = ImGui::IsItemHovered();
    ImGui::PopID();
    s_page.menuLabelHovered |= hov || xHov;

    const ImVec2 b(pos.x + w, pos.y + h);
    dl->AddRectFilled(pos, b, hov || open ? Hex(0x332d21) : Hex(0x29251c), Px(4));
    Outline(dl, pos, b, open ? col::SelBar : hov || xHov ? Hex(0xa18547) : Hex(0x6b5b3a), Px(4));
    float x = pos.x + Px(11);
    const float ty = pos.y + (h - LineH(11)) * 0.5f;
    Text(dl, t, ImVec2(x, ty), Hex(0xb39b66), f.label);
    x += TextW(t, f.label) + Px(8);
    if (std::string(f.id) == "skills")
    {
        int i = 0;
        for (int id : s_state.selectedSkills)
        {
            if (i == 4) break;
            const ImVec2 a(R(x), R(pos.y + (h - Px(18)) * 0.5f)), ib(a.x + Px(18), a.y + Px(18));
            if (ImTextureID tex = GetSkillIcon(id)) dl->AddImage(tex, a, ib);
            Outline(dl, a, ib, col::IconRule, 0.0f);
            x += Px(18) + Px(3);
            i++;
        }
        if ((int)s_state.selectedSkills.size() > 4)
        {
            const std::string more = "+" + std::to_string(s_state.selectedSkills.size() - 4);
            Text(dl, t, ImVec2(x + Px(3), ty), Hex(0xf0deb0), more.c_str());
        }
    }
    else
        TextFit(dl, t, ImVec2(x, ty), Px(230), Hex(0xf0deb0), value.c_str());

    const ImVec2 xc(b.x - Px(10) - Px(4.5f), pos.y + h * 0.5f);
    const float r = Px(3.5f);
    const ImU32 xcol = xHov ? col::ToggleOn : Hex(0xb39b66);
    dl->AddLine(ImVec2(xc.x - r, xc.y - r), ImVec2(xc.x + r, xc.y + r), xcol, 1.4f);
    dl->AddLine(ImVec2(xc.x - r, xc.y + r), ImVec2(xc.x + r, xc.y - r), xcol, 1.4f);

    if (hov) ImGui::SetTooltip("%s", FilterTooltip(f.id).c_str());
    if (xHov) ImGui::SetTooltip("Clear the %s filter", f.label);
    if (clicked) s_page.openMenu = open ? std::string() : std::string(f.id);
    if (cleared) ClearFilter(f.id);
    if (s_page.openMenu == f.id) s_page.menuAnchor = ImVec2(pos.x, b.y + Px(6));
}

// The filter buttons, wrapping onto a second line if the active ones need it; the sort and Clear
// filters at the right of the first. Returns the row's height.
// V41: 31 px, 12 px type, each control padded 12 px and 12 px apart; the sort at the far right.
static float DrawFilterRow(ImDrawList* dl, float x0, float x1, float y)
{
    const Type t = T(12);
    const float lineH = Px(31);
    const float padX = Px(12);
    s_page.menuLabelHovered = false;

    // The sort, at the right: the current order and its arrow; a click lists the others.
    int current = -1;
    for (int i = 0; i < IM_ARRAYSIZE(kSortOptions); i++)
        if (kSortOptions[i].column == s_state.sortColumn && kSortOptions[i].ascending == s_state.sortAscending) current = i;
    const char* sortLabel = current >= 0 ? kSortOptions[current].label : "Sorted";
    const bool down = current < 0 ? !s_state.sortAscending : !kSortOptions[current].ascending;
    const float sortW = TextW(t, sortLabel) + TextW(t, " ") + Px(9);
    const ImVec2 sortPos(R(x1 - Px(10) - sortW), R(y + (lineH - LineH(12)) * 0.5f));
    ImGui::SetCursorScreenPos(sortPos);
    const bool sortClicked = ImGui::InvisibleButton("##lib_sort_btn", ImVec2(sortW, LineH(12)));
    const bool sortHov = ImGui::IsItemHovered();
    const ImU32 sc = sortHov ? col::LinkHover : v41::Filters;
    Text(dl, t, sortPos, sc, sortLabel);
    DrawArrowVertical(dl, ImVec2(sortPos.x + sortW - Px(4.5f), sortPos.y + LineH(12) * 0.52f), Px(10), sc, down);
    if (sortClicked) ImGui::OpenPopup("##lib_sort");
    ImGui::SetNextWindowPos(ImVec2(sortPos.x + sortW, y + lineH + Px(4)), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(col::Menu));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(col::ActionRule));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(6), Px(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Px(4));
    if (ImGui::BeginPopup("##lib_sort"))
    {
        ImGui::PushFont(T(11).font);
        for (int i = 0; i < IM_ARRAYSIZE(kSortOptions); i++)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(i == current ? col::Sort : col::Filters));
            if (ImGui::Selectable(kSortOptions[i].label, i == current, 0, ImVec2(Px(170), Px(24))))
            {
                s_state.sortColumn = kSortOptions[i].column;
                s_state.sortAscending = kSortOptions[i].ascending;
            }
            ImGui::PopStyleColor();
        }
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    // Clear filters, left of the sort, while anything narrows the list.
    float limit = sortPos.x - Px(28);
    if (AnyFilterActive())
    {
        const Type ct = T(10);
        const char* clear = "Clear filters";
        const float cw = TextW(ct, clear);
        const ImVec2 cp(R(sortPos.x - Px(28) - cw), R(y + (lineH - LineH(10)) * 0.5f));
        if (LinkText(dl, "##lib_clear", ct, cp, cw + 1.0f, col::Muted, clear)) ClearAllFilters();
        limit = cp.x - Px(28);
    }

    // The menus' buttons.
    float x = x0, ly = y;
    for (const FilterMenu& f : kFilterMenus)
    {
        const bool active = FilterActive(f.id);
        const std::string value = active ? FilterValue(f.id) : std::string();
        const float w = active ? FilterChipWidth(f, value) : padX + TextW(t, f.label) + TextW(t, " ") + Px(9) + padX;
        if (x > x0 && x + w > limit)
        {
            x = x0;
            ly += lineH + Px(8);
            limit = x1;   // later lines have the whole width
        }
        if (active)
        {
            DrawFilterChip(dl, f, value, ImVec2(R(x), R(ly)), R(w), lineH);
            x += R(w) + Px(12);
            continue;
        }
        const float ty = R(ly + (lineH - LineH(12)) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(x, ly));
        ImGui::PushID(f.id);
        const bool clicked = ImGui::InvisibleButton("##menu", ImVec2(w, lineH));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        s_page.menuLabelHovered |= hov;
        const bool open = s_page.openMenu == f.id;
        if (open || hov)
            dl->AddRectFilled(ImVec2(x, ly), ImVec2(x + w, ly + lineH), open ? Hex(0xffffff, 0x0c) : Hex(0xffffff, 0x07), Px(5));
        const ImU32 c = open ? col::FilterOn : hov ? col::Text : v41::Filters;
        Text(dl, t, ImVec2(x + padX, ty), c, f.label);
        DrawChevronDown(dl, ImVec2(x + padX + TextW(t, f.label) + TextW(t, " ") + Px(4), ty + LineH(12) * 0.56f), Px(6), c);
        if (clicked) s_page.openMenu = open ? std::string() : std::string(f.id);
        x += w + Px(12);
    }
    return ly + lineH - y;
}

// ─── Match list ──────────────────────────────────────────────────────────────

struct ListGeom
{
    float w = 0.0f;                    // the rows' width
    bool event = true;                 // the Event column, dropped first when the list narrows
    float icon = 0.0f, iconGap = 0.0f; // the elite lineup's cells
    float starX = 0, dateX = 0, eventX = 0, team1X = 0, vsX = 0, team2X = 0, mapX = 0, durX = 0;
    float teamW = 0.0f, mapW = 0.0f;
};

// V41's grid, the same for the labels and the rows: 18px 95px 105px 1fr 20px 1fr 185px 40px, 9 px
// gaps, 9 px padding. Star | Date | Event | Guild 1 | vs | Guild 2 | Map | Duration. A list too
// narrow for two 246 px lineups drops the Event column, then narrows the cells.
static ListGeom ComputeListGeom(float w)
{
    ListGeom g;
    g.w = w;
    const float gap = Px(9), pad = Px(9);
    const float star = Px(18), date = Px(95), event = Px(105), vs = Px(20), map = Px(185), dur = Px(40);
    g.mapW = map;
    const float inner = w - pad * 2.0f;
    float fixed = star + date + event + vs + map + dur + gap * 7.0f;
    float teamW = (inner - fixed) * 0.5f;
    g.icon = Px(29);
    g.iconGap = Px(4);   // owner 2026-11-25: more room between the elites than V41's 2 px
    const float lineup = g.icon * 8.0f + g.iconGap * 7.0f;
    if (teamW < lineup)
    {
        g.event = false;
        fixed -= event + gap;
        teamW = (inner - fixed) * 0.5f;
    }
    if (teamW < lineup) g.icon = std::max(Px(12), std::floor((teamW - g.iconGap * 7.0f) / 8.0f));
    float x = pad;
    g.starX = x;  x += star + gap;
    g.dateX = x;  x += date + gap;
    if (g.event) { g.eventX = x; x += event + gap; }
    g.team1X = x; x += teamW + gap;
    g.vsX = x;    x += vs + gap;
    g.team2X = x; x += teamW + gap;
    g.mapX = x;   x += map + gap;
    g.durX = x;
    g.teamW = teamW;
    return g;
}

// The column labels: 32 px (6 px above, 12 below), 10 px type tracked 1.5 px, a faint rule under
// them. The "N new matches added" pill follows MATCHUP / ELITE SKILLS.
static void DrawLabelsRow(ImDrawList* dl, float x0, float x1, float y, const ListGeom& g)
{
    const Type t = T(10);
    const float tracking = Px(1.5f);
    const float ty = y + Px(6);
    TextSpaced(dl, t, ImVec2(x0 + g.dateX, ty), v41::Labels, "DATE", tracking);
    if (g.event) TextSpaced(dl, t, ImVec2(x0 + g.eventX, ty), v41::Labels, "EVENT", tracking);
    const char* left = "MATCHUP / ELITE SKILLS";
    const ImVec2 leftPos(x0 + g.team1X, ty);
    const float leftW = TextSpaced(dl, t, leftPos, v41::Labels, left, tracking);
    TextSpaced(dl, t, ImVec2(x0 + g.mapX, ty), v41::Labels, "MAP", tracking);
    // The mockup's DURATION is smaller and barely tracked (9 px, 0.4 px), so it fits its 40 px column.
    TextSpaced(dl, T(9), ImVec2(x0 + g.durX, ty + Ascent(10) - Ascent(9)), v41::Labels, "DURATION", Px(0.4f));
    HRule(dl, x0, x1, y + Px(32) - 1.0f, Hex(0xffffff, 0x15));
    ImGui::SetCursorScreenPos(leftPos);
    ImGui::Dummy(ImVec2(leftW, LineH(10)));
    DrawNewMatchesPill();
}

// A skill's picture in a cell. The list crops the elites' baked-in gold edge (V41: the picture drawn
// 33 px in a 29 px cell); the sidebar's bars keep it, the whole picture (owner 2026-11-25).
// A head and shoulders filling the cell a..b: the list's mark for a henchman (no art for one ships).
static void DrawHenchmanGlyph(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col)
{
    const float s = b.x - a.x, cx = (a.x + b.x) * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, a.y + s * 0.36f), s * 0.17f, col, 20);
    dl->PathArcTo(ImVec2(cx, b.y - s * 0.12f), s * 0.30f, 3.14159265f, 2.0f * 3.14159265f, 20);
    dl->PathLineTo(ImVec2(cx + s * 0.30f, b.y - s * 0.12f));
    dl->PathFillConvex(col);
}

static void SkillCell(ImDrawList* dl, ImTextureID tex, ImVec2 a, ImVec2 b, bool cropEdge)
{
    const float f = cropEdge ? 2.0f / 33.0f : 0.0f;
    dl->AddImage(tex, a, b, ImVec2(f, f), ImVec2(1.0f - f, 1.0f - f));
}

// One team's cell: the trophy before the winner's name, the name (bold 14, the winner's lighter,
// never gold), its tag, then the eight elite skills, 29 px cells 2 px apart.
static void DrawTeamCell(ImDrawList* dl, const ListGeom& g, float x, float cy, const MatchMeta& m,
                         const GuildLabel& gl, bool winner, const std::vector<EliteSlot>& slots, bool hoverable)
{
    const Type name = T(14, true), tag = T(11);
    const float lineH = Px(23);
    const float top = R(cy - (lineH + Px(5) + g.icon) * 0.5f);
    const float nameTop = R(top + (lineH - LineH(14)) * 0.5f);
    float tx = x, maxW = g.teamW;
    if (winner)
    {
        const float box = Px(23);
        const ImVec2 b0(x, top), b1(b0.x + box, b0.y + box);
        if (ImTextureID cup = GetCupIcon()) ImageContain(dl, cup, 64.0f, 109.0f, b0, b1);
        if (hoverable && ImGui::IsMouseHoveringRect(b0, b1)) ImGui::SetTooltip("Winning guild");
        tx += box + Px(9);
        maxW -= box + Px(9);
    }
    const std::string& nm = gl.name.empty() ? gl.display : gl.name;
    const float nameW = TextW(name, nm.c_str());
    const float drawn = TextFit(dl, name, ImVec2(tx, nameTop), maxW, winner ? v41::WinnerName : v41::GuildName, nm.c_str());
    if (drawn >= nameW && !gl.name.empty() && !gl.tag.empty())
    {
        const std::string tg = "[" + gl.tag + "]";
        const float after = tx + drawn + Px(4);
        if (after < tx + maxW)
            TextFit(dl, tag, ImVec2(after, nameTop + Ascent(14) - Ascent(11)), tx + maxW - after, v41::ListTag, tg.c_str());
    }

    // The lineup.
    const SkillDatabaseView* view = slots.empty() ? nullptr : &GetMatchSkillView(m);
    float ix = x;
    const float iy = top + lineH + Px(5);
    for (int i = 0; i < 8; i++)
    {
        const ImVec2 a(R(ix), R(iy)), b(R(ix) + g.icon, R(iy) + g.icon);
        const EliteSlot* slot = i < (int)slots.size() ? &slots[i] : nullptr;
        bool drewIcon = false;
        if (slot && slot->henchman)
        {
            // A henchman: no elite skill, a head and shoulders.
            dl->AddRectFilled(a, b, Hex(0x1a1916, 0xd0));
            DrawHenchmanGlyph(dl, a, b, Hex(0xc8b27a, 0xe0));
            drewIcon = true;
        }
        else if (slot && slot->skill)
            if (ImTextureID tex = GetSkillIcon(slot->skill, view))
            {
                SkillCell(dl, tex, a, b, true);
                drewIcon = true;
            }
        if (!drewIcon)
        {
            // No elite: the primary profession's icon, nearly the cell's size; an empty slot stays
            // a dark cell.
            dl->AddRectFilled(a, b, slot && !slot->empty ? Hex(0x1a1916, 0xd0) : Hex(0x1a1916, 0x70));
            if (slot && slot->prof)
                if (ImTextureID prof = GetProfessionIcon(slot->prof))
                {
                    const float inset = R(g.icon * 0.1f);
                    dl->AddImage(prof, ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset),
                                 ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 235));
                }
        }
        if (hoverable && slot && ImGui::IsMouseHoveringRect(a, b))
        {
            if (slot->empty)
                ImGui::SetTooltip(m.is_cloud_only ? "Empty slot (a henchman shows here once the match is downloaded)"
                                                  : "Empty slot: nobody played here");
            else if (slot->henchman)
                ImGui::SetTooltip("%s\nA guild hall henchman, filling an empty slot.", slot->player.c_str());
            else if (slot->skill)
            {
                DrawSkillTooltip(slot->skill, view);
                ImGui::BeginTooltip();
                ImGui::TextColored(kColorTextDim, "%s", slot->player.c_str());
                ImGui::EndTooltip();
            }
            else
                ImGui::SetTooltip("%s - no elite skill", slot->player.c_str());
        }
        ix += g.icon + g.iconGap;
    }
}

// Text wrapped on words into at most two lines of maxW (the second cut with an ellipsis), centred
// on cy: the list's map names. lineH: the line pitch.
static void TextWrap2(ImDrawList* dl, const Type& t, float x, float cy, float maxW, float lineH, ImU32 c, const std::string& s)
{
    std::string first = s, second;
    if (TextW(t, s.c_str()) > maxW)
    {
        size_t cut = std::string::npos;
        for (size_t i = s.find(' '); i != std::string::npos; i = s.find(' ', i + 1))
        {
            if (TextW(t, s.c_str(), s.c_str() + i) > maxW) break;
            cut = i;
        }
        if (cut != std::string::npos)
        {
            first = s.substr(0, cut);
            second = s.substr(cut + 1);
        }
    }
    const float total = second.empty() ? lineH : lineH * 2.0f;
    const float y0 = cy - total * 0.5f + (lineH - LineH(11)) * 0.5f;
    TextFit(dl, t, ImVec2(x, R(y0)), maxW, c, first.c_str());
    if (!second.empty()) TextFit(dl, t, ImVec2(x, R(y0 + lineH)), maxW, c, second.c_str());
}

// ─── Sidebar ─────────────────────────────────────────────────────────────────

// A small outlined button on the sidebar (Details): 10 px type.
static bool SmallButton(ImDrawList* dl, const char* id, ImVec2 pos, const char* label, float padX, float padY,
                        bool chevron, ImVec2* sizeOut = nullptr)
{
    const Type t = T(10);
    const float w = 2.0f + padX * 2.0f + TextW(t, label) + (chevron ? Px(4) + Px(7) : 0.0f);
    const float h = 2.0f + padY * 2.0f + LineH(10);
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 b(pos.x + w, pos.y + h);
    // V41's Details: white 1.6% fill, a white 8% outline, 3 px rounding, #c1b397 type.
    dl->AddRectFilled(pos, b, hov ? Hex(0xffffff, 0x0c) : v41::DetailsBg, Px(3));
    Outline(dl, pos, b, hov ? col::ActionHovRule : v41::DetailsLine, Px(3));
    Text(dl, t, ImVec2(pos.x + 1.0f + padX, pos.y + 1.0f + padY), v41::DetailsText, label);
    if (chevron)
        DrawChevronDown(dl, ImVec2(b.x - 1.0f - padX - Px(3.5f), pos.y + h * 0.53f), Px(5.5f), v41::DetailsText);
    if (sizeOut) *sizeOut = ImVec2(w, h);
    return clicked;
}

static float SmallButtonWidth(const char* label, float padX, bool chevron)
{
    return 2.0f + padX * 2.0f + TextW(T(10), label) + (chevron ? Px(4) + Px(7) : 0.0f);
}

// A text link: hover lights it and underlines it. Returns true on click.
static bool LinkText(ImDrawList* dl, const char* id, const Type& t, ImVec2 pos, float maxW, ImU32 c, const char* s)
{
    const float w = TextFit(nullptr, t, pos, maxW, c, s);
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, w), t.size));
    const bool hov = ImGui::IsItemHovered();
    TextFit(dl, t, pos, maxW, hov ? col::LinkHover : c, s);
    if (hov)
    {
        HRule(dl, pos.x, pos.x + w, pos.y + t.size - 1.0f, col::LinkHover);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    return clicked;
}

static void DrawReplayButton(ImDrawList* dl, const MatchMeta& m, ImVec2 pos, ImVec2 size)
{
    ImGui::SetCursorScreenPos(pos);
    const bool busy = g_cloudDownloadInProgress;
    const bool clicked = ImGui::InvisibleButton("##lib_replay", size);
    const bool hov = ImGui::IsItemHovered() && !busy;
    const bool act = ImGui::IsItemActive() && !busy;
    const ImVec2 b(pos.x + size.x, pos.y + size.y);
    // V41: linear-gradient(105deg, #d2bb83, #b6a06c), a #ddc48c55 outline, 4 px rounding, an inset
    // white highlight along the top, a soft shadow under it.
    const ImU32 ca = act ? Hex(0xc4ad77) : hov ? Hex(0xdcc690) : v41::ReplayA;
    const ImU32 cb = act ? Hex(0xa58f5f) : hov ? Hex(0xc2ac78) : v41::ReplayB;
    for (int k = 1; k <= 3; k++)
        dl->AddRectFilled(ImVec2(pos.x - k, pos.y + Px(5) - k), ImVec2(b.x + k, b.y + Px(5) + k), Hex(0, 0x0a), Px(4) + k);
    FillLinear(dl, pos, b, Px(4), 105.0f, ca, cb);
    dl->AddLine(ImVec2(pos.x + Px(4), pos.y + 1.5f), ImVec2(b.x - Px(4), pos.y + 1.5f), Hex(0xffffff, 0x33));
    Outline(dl, pos, b, v41::ReplayLine, Px(4));
    if (busy) dl->AddRectFilled(pos, b, Hex(0x13120f, 120), Px(4));

    const Type t = T(13, true);
    const float tracking = Px(1.0f);
    const float labelW = TextSpaced(nullptr, t, ImVec2(), 0, "REPLAY MATCH", tracking);
    const float triW = Px(10), gap = Px(14);
    const float x0 = R(pos.x + (size.x - (triW + gap + labelW)) * 0.5f);
    const float push = act ? 1.0f : 0.0f;
    DrawPlay(dl, ImVec2(x0 + triW * 0.5f, pos.y + size.y * 0.5f + push), Px(11), v41::ReplayText);
    TextSpaced(dl, t, ImVec2(x0 + triW + gap, R(pos.y + (size.y - LineH(13)) * 0.5f) + push), v41::ReplayText, "REPLAY MATCH",
               tracking);

    if (clicked && !busy)
    {
        g_pendingReplay.requested = true;
        g_pendingReplay.match = m;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", busy ? "A match is downloading..."
                                : m.is_cloud_only ? "Downloads the match, then opens the replay"
                                                  : "Open the replay (or double-click the match in the list)");
}

static const char* const kHiddenWord = "Hidden";

// The sidebar's five-star rating, in the mockup's muted gold: hover previews, a click sets it, a
// click on the current rating clears it.
static void DrawRatingStars(ImDrawList* dl, ImVec2 pos, float h, const std::string& folder)
{
    const float pitch = Px(16.5f), r = Px(5.8f);
    const int current = MatchRatings::Get().GetRating(folder);
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##lib_rating", ImVec2(pitch * 5.0f, h));
    const bool hov = ImGui::IsItemHovered();
    int shown = current;
    if (hov)
    {
        shown = std::clamp((int)((ImGui::GetIO().MousePos.x - pos.x) / pitch) + 1, 1, 5);
        if (ImGui::IsItemClicked())
            MatchRatings::Get().SetRating(folder, shown == current ? 0 : shown);
        ImGui::SetTooltip(current > 0 ? "Your rating (kept on this PC). Click the same star again to clear it."
                                      : "Rate this match (kept on this PC)");
    }
    for (int i = 0; i < 5; i++)
    {
        const ImVec2 c(pos.x + pitch * i + r + Px(0.5f), pos.y + h * 0.5f + Px(0.5f));
        const bool filled = i < shown;
        DrawStar(dl, c, r, filled ? (hov ? col::LinkHover : col::StarOn) : col::Rating, filled, 1.05f);
    }
}

static void DrawShareLink(ImDrawList* dl, const MatchMeta& m, float right, float y, float h)
{
    const bool canShare = UrlProtocol::IsPublished(m.folder_name);
    const bool recent = s_page.shareCopiedFor == m.folder_name && ImGui::GetTime() - s_page.shareCopiedAt < 1.5;
    const Type t = T(11);
    const char* label = recent ? "Link copied" : "Share match";
    const float arrowSlot = recent ? 0.0f : TextW(t, " ") + Px(8);
    const float w = TextW(t, label) + arrowSlot;
    const ImVec2 pos(R(right - w), y + (h - LineH(11)) * 0.5f);
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton("##lib_share", ImVec2(w, LineH(11)));
    const bool hov = ImGui::IsItemHovered();
    const ImU32 c = !canShare ? Hex(0x6f6a60) : recent ? Hex(0x8fd49c) : hov ? col::LinkHover : col::Share;
    Text(dl, t, pos, c, label);
    if (!recent)
        DrawArrowUpRight(dl, ImVec2(pos.x + w - Px(4), pos.y + LineH(11) * 0.5f), Px(9), c);
    if (clicked && canShare)
    {
        MatchLink link;
        link.folder = m.folder_name;
        MatchLinks::FillDisplay(link, m);
        const std::string url = MatchLinks::FormatHttps(link);
        if (UrlProtocol::CopyToClipboard(url, nullptr))
        {
            RunLog::Line("link: copied %s", url.c_str());
            s_page.shareCopiedAt = ImGui::GetTime();
            s_page.shareCopiedFor = m.folder_name;
        }
    }
    if (hov)
        ImGui::SetTooltip("%s", canShare ? "Copy a link to this match. Paste it in Discord: a click opens it in GW Observer."
                                         : UrlProtocol::kNotPublishedTip);
}

// V41's build bar: eight 28 px cells with no gaps (224 px), each skill's picture drawn 32 px and
// cropped to the cell. Hover a skill for its card; a click adds it to the Skills filter or takes it
// out, as the legacy row does.
static void DrawBuildBar(ImDrawList* dl, const PlayerMeta& p, const SkillDatabaseView& view, ImVec2 pos, float cell)
{
    std::vector<int> skills;
    if (!p.used_skills.empty())
    {
        const auto sorted = view.SortSkillsForDisplay(p.used_skills, p.primary, p.secondary);
        skills.assign(sorted.begin(), sorted.end());
    }
    for (int i = 0; i < 8; i++)
    {
        const ImVec2 a(R(pos.x + cell * i), R(pos.y)), b(R(pos.x + cell * (i + 1)), R(pos.y) + cell);
        const int id = i < (int)skills.size() ? skills[i] : 0;
        const ImTextureID tex = id ? GetSkillIcon(id, &view) : ImTextureID();
        if (tex)
            SkillCell(dl, tex, a, b, false);
        else
            dl->AddRectFilled(a, b, Hex(0x1a1916, 0xb0));
        if (!id) continue;
        const bool picked = s_state.selectedSkills.count(id) > 0;
        if (picked) dl->AddRect(a, b, ImGui::GetColorU32(kColorAccent), 0.0f, 0, 2.0f);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##skill", ImVec2(b.x - a.x, b.y - a.y));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            DrawSkillTooltip(id, &view);
        }
        if (clicked)
        {
            if (picked) s_state.selectedSkills.erase(id);
            else        s_state.selectedSkills.insert(id);
        }
    }
}

// A team in the Builds tab. The heading (40 px): the guild's cape, a 3 px stripe in its side's
// colour (party 1 blue, party 2 red), the name in that colour, its tag, and the trophy at the far
// right for the winner, over a wash of the side's colour. Then a 30 px line a player: profession,
// name, the full bar. The profession icon and the name show the build card on hover; a click on the
// icon pins it, a click on the name searches for the player, a right-click copies the template.
static float DrawRosterTeam(ImDrawList* dl, const MatchMeta& m, const char* partyId, const GuildLabel& gl, bool red,
                            bool winner, float x, float w, float y, const OverviewBuilds& ob, bool compact)
{
    const SkillDatabaseView& view = GetMatchSkillView(m);
    const ImU32 stripe = red ? v41::RedStripe : v41::BlueStripe;
    const ImU32 nameCol = red ? v41::RedName : v41::BlueName;
    const ImU32 wash = red ? v41::RedWash : v41::BlueWash;

    // Heading.
    y += compact ? Px(8) : Px(12);
    const float hh = Px(40);
    const ImVec2 h0(x, R(y)), h1(x + w, R(y) + hh);
    FillLinear(dl, h0, ImVec2(x + w * 0.9f, h1.y), 0.0f, 90.0f, wash, wash & ~IM_COL32_A_MASK);
    if (ImTextureID cape = GetGuildCape(m, gl))
        ImageContain(dl, cape, 96.0f, 192.0f, h0, ImVec2(h0.x + Px(22), h1.y));
    dl->AddRectFilled(ImVec2(x + Px(29), h0.y + Px(8)), ImVec2(x + Px(32), h1.y - Px(8)), stripe, Px(2));
    float right = x + w;
    if (winner)
    {
        const ImVec2 c0(R(x + w - Px(22)), R(h0.y + (hh - Px(22)) * 0.5f)), c1(c0.x + Px(22), c0.y + Px(22));
        if (ImTextureID cup = GetCupIcon()) ImageContain(dl, cup, 64.0f, 109.0f, c0, c1);
        if (ImGui::IsMouseHoveringRect(c0, c1) && ImGui::IsWindowHovered()) ImGui::SetTooltip("Winning guild");
        right = c0.x - Px(8);
    }
    const std::string nm = gl.name.empty() ? gl.display : gl.name;
    const Type name = T(14, true), tag = T(11);
    const float nx = x + Px(38);
    const float textTop = R(h0.y + (hh - LineH(14)) * 0.5f);
    ImGui::PushID(partyId);
    if (LinkText(dl, "##guild", name, ImVec2(nx, textTop), right - nx, nameCol, nm.c_str()))
        FilterBySearchTerm(gl.display);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s team: %s\nClick to show every match for this guild", red ? "Red" : "Blue", gl.display.c_str());
    else if (ImGui::IsMouseHoveringRect(h0, h1) && ImGui::IsWindowHovered())
        ImGui::SetTooltip("%s team", red ? "Red" : "Blue");
    const float nameW = TextFit(nullptr, name, ImVec2(), right - nx, 0, nm.c_str());
    if (!gl.name.empty() && !gl.tag.empty() && nx + nameW + Px(4) < right)
    {
        const std::string tg = "[" + gl.tag + "]";
        TextFit(dl, tag, ImVec2(nx + nameW + Px(4), textTop + Ascent(14) - Ascent(11)), right - (nx + nameW + Px(4)),
                v41::RosterTag, tg.c_str());
    }
    y = h1.y + Px(6);

    // Eight rows: the players, the henchmen, then an empty row a slot nobody filled (TeamSlots).
    const std::vector<RosterSlot> slots = TeamSlots(m, partyId);
    std::vector<const PlayerMeta*> players;
    for (const RosterSlot& rs : slots)
        if (rs.player && !rs.henchman) players.push_back(rs.player);

    // The list's cell size; 6 px between bars (2 px when compact).
    const float rowH = compact ? Px(31) : Px(35), cell = Px(29), barW = cell * 8.0f;
    const Type pn = T(13);
    const float copyS = Px(18), copyGap = Px(8);
    int slotIndex = 0;
    for (const RosterSlot& rs : slots)
    {
        const int thisSlot = slotIndex++;
        if (!rs.player || rs.henchman)
        {
            const float cy = y + rowH * 0.5f;
            const ImVec2 bar0(R(x + w - barW), R(cy - cell * 0.5f));
            const float nx = x + Px(16) + Px(7);
            const float nameMaxW = std::max(Px(20), w - Px(16) - Px(7) - copyGap * 2.0f - copyS - barW);
            ImGui::PushID(1000 + thisSlot);
            if (!rs.player)
            {
                // Nobody here: a faint row, eight dark cells.
                dl->AddCircle(ImVec2(x + Px(7.5f), cy), Px(5.5f), Hex(0xffffff, 0x22), 16, 1.0f);
                Text(dl, pn, ImVec2(nx, R(cy - LineH(13) * 0.5f)), Hex(0x8a8172), "Empty slot");
                for (int i = 0; i < 8; i++)
                {
                    const ImVec2 a(bar0.x + R(cell * i), bar0.y), b(bar0.x + R(cell * (i + 1)), bar0.y + cell);
                    dl->AddRectFilled(ImVec2(a.x + 1.0f, a.y + 1.0f), ImVec2(b.x - 1.0f, b.y - 1.0f), Hex(0x1a1916, 0x60));
                }
                if (ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + w, y + rowH)) && ImGui::IsWindowHovered())
                    ImGui::SetTooltip(m.is_cloud_only ? "Nobody in this slot in the cloud index.\n"
                                                        "A henchman shows here once the match is downloaded."
                                                      : "Nobody played in this slot (the player never joined).");
            }
            else
            {
                // A henchman: its profession, its name and title, its bar.
                const HenchmanInfo& h = *rs.henchman;
                PlayerMeta hp = *rs.player;
                hp.primary = h.profession;
                hp.secondary = 0;
                hp.used_skills = HenchmanSkills(rs);
                const ImVec2 i0(x, R(cy - Px(7.5f))), i1(x + Px(15), R(cy - Px(7.5f)) + Px(15));
                if (ImTextureID ico = GetProfessionIcon(h.profession)) dl->AddImage(ico, i0, i1);
                const std::string full = h.name;
                const size_t br = full.find(" [");
                const std::string nm = full.substr(0, br);
                const std::string title = br == std::string::npos ? std::string() : full.substr(br + 1);
                const float ty = R(cy - LineH(13) * 0.5f);
                const float nameW = TextFit(dl, pn, ImVec2(nx, ty), nameMaxW, v41::Player, nm.c_str());
                if (!title.empty() && nameW + Px(5) < nameMaxW)
                    TextFit(dl, T(11), ImVec2(nx + nameW + Px(5), ty + Ascent(13) - Ascent(11)), nameMaxW - nameW - Px(5),
                            v41::RosterTag, title.c_str());
                if (ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(nx + nameMaxW, y + rowH)) && ImGui::IsWindowHovered())
                    ImGui::SetTooltip("%s\nA guild hall henchman, filling an empty slot. Its bar is always the same.", full.c_str());

                if (!hp.used_skills.empty())
                {
                    static std::unordered_map<std::string, float> s_henchCopiedAt;
                    const std::string ckey = m.folder_name + "|" + partyId + "|h" + std::to_string(h.modelId);
                    const auto cit = s_henchCopiedAt.find(ckey);
                    const float fb = cit == s_henchCopiedAt.end() ? -1.0f : cit->second;
                    ImGui::SetCursorScreenPos(ImVec2(R(x + w - barW - copyGap - copyS), y));
                    if (DrawCopyIcon("##copy_hench", copyS, R((rowH - copyS) * 0.5f), fb))
                    {
                        const std::string chatLink = "[" + nm + "-Skills;" + PlayerTemplateCode(hp, nullptr) + "]";
                        ImGui::SetClipboardText(chatLink.c_str());
                        s_henchCopiedAt[ckey] = (float)ImGui::GetTime();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", fb >= 0.0f && (float)ImGui::GetTime() - fb < 1.5f ? "Copied!" : "Copy build template");
                }
                DrawBuildBar(dl, hp, view, bar0, cell);
            }
            ImGui::PopID();
            y += rowH;
            continue;
        }
        const PlayerMeta& p = *rs.player;
        const std::string key = std::string(partyId) + ":" + std::to_string(p.player_number);
        const std::vector<SolvedAttribute>* build = nullptr;
        if (ob.builds)
            if (auto it = ob.builds->find(key); it != ob.builds->end()) build = &it->second;
        const bool pinned = s_pinnedCard.folder == m.folder_name && s_pinnedCard.key == key;
        const std::string clean = p.encoded_name.empty() ? "(unnamed)" : SanitizePlayerName(p.encoded_name);
        const float cy = y + rowH * 0.5f;
        ImGui::PushID(p.player_number);

        // Profession icon: card on hover, pinned on a click.
        const ImVec2 i0(x, R(cy - Px(7.5f))), i1(x + Px(15), R(cy - Px(7.5f)) + Px(15));
        if (ImTextureID ico = GetProfessionIcon(p.primary)) dl->AddImage(ico, i0, i1);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        const bool iconClicked = ImGui::InvisibleButton("##prof", ImVec2(Px(16), rowH));
        bool showCard = ImGui::IsItemHovered();
        if (showCard) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (iconClicked)
        {
            if (pinned) s_pinnedCard = PinnedBuildCard();
            else s_pinnedCard = { m.folder_name, key, ImVec2(ImGui::GetMousePos().x + 16.0f, ImGui::GetMousePos().y + 8.0f) };
        }
        if (pinned) Outline(dl, ImVec2(i0.x - 2.0f, i0.y - 2.0f), ImVec2(i1.x + 2.0f, i1.y + 2.0f), ImGui::GetColorU32(kColorAccent), 3.0f);

        // Name: a search link. Then the copy button, 8 px before the bar.
        const float nx2 = x + Px(16) + Px(7);
        const float nameMaxW = std::max(Px(20), w - Px(16) - Px(7) - copyGap * 2.0f - copyS - barW);
        if (LinkText(dl, "##name", pn, ImVec2(nx2, R(cy - LineH(13) * 0.5f)), nameMaxW, v41::Player, clean.c_str()))
        {
            if (ImGui::GetIO().KeyCtrl) AddSearchTerm(clean);
            else                        FilterBySearchTerm(clean);
        }
        showCard |= ImGui::IsItemHovered();

        // Right-click on the icon or the name: the templates.
        if (ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(nx2 + nameMaxW, y + rowH)) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsWindowHovered())
        {
            s_page.ctxName = clean;
            s_page.ctxTemplate = "[" + clean + "-Skills;" + PlayerTemplateCode(p, build) + "]";
            s_page.ctxTeamTemplates.clear();
            for (const PlayerMeta* tp : players)
            {
                if (tp->used_skills.empty() && tp->skill_template_code.empty()) continue;
                const std::string tkey = std::string(partyId) + ":" + std::to_string(tp->player_number);
                const std::vector<SolvedAttribute>* tb = nullptr;
                if (ob.builds)
                    if (auto it = ob.builds->find(tkey); it != ob.builds->end()) tb = &it->second;
                const std::string tn = tp->encoded_name.empty() ? "(unnamed)" : SanitizePlayerName(tp->encoded_name);
                s_page.ctxTeamTemplates += "[" + tn + "-Skills;" + PlayerTemplateCode(*tp, tb) + "]\n";
            }
            s_page.openCtx = true;   // opened by DrawAside, outside this row's ID scope
        }

        if (showCard && !pinned)
        {
            ImGui::BeginTooltip();
            DrawBuildCard(p, clean, build, ob.pending, view, Px(22), false);
            ImGui::EndTooltip();
        }

        // Copy the build: the template (skills and the solved attributes) as a chat link, as the
        // legacy Overview's button copies it.
        if (!p.used_skills.empty() || !p.skill_template_code.empty())
        {
            static std::unordered_map<std::string, float> s_copiedAt;
            const std::string ckey = m.folder_name + "|" + key;
            const auto cit = s_copiedAt.find(ckey);
            const float fb = cit == s_copiedAt.end() ? -1.0f : cit->second;
            ImGui::SetCursorScreenPos(ImVec2(R(x + w - barW - copyGap - copyS), y));
            if (DrawCopyIcon("##copy_build", copyS, R((rowH - copyS) * 0.5f), fb))
            {
                const std::string chatLink = "[" + clean + "-Skills;" + PlayerTemplateCode(p, build) + "]";
                ImGui::SetClipboardText(chatLink.c_str());
                s_copiedAt[ckey] = (float)ImGui::GetTime();
            }
            if (ImGui::IsItemHovered())
            {
                const bool copied = fb >= 0.0f && (float)ImGui::GetTime() - fb < 1.5f;
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(copied ? "Copied!" : "Copy build template");
                if (!copied)
                    ImGui::TextColored(kColorTextDim, build ? "Skills and the solved attributes (points only, no runes)"
                                                            : "Skills only until the match has been read");
                ImGui::EndTooltip();
            }
        }

        DrawBuildBar(dl, p, view, ImVec2(R(x + w - barW), R(cy - cell * 0.5f)), cell);

        ImGui::PopID();
        y += rowH;
    }
    ImGui::PopID();
    return y;
}

static void DrawPinnedCard(const MatchMeta& m, const OverviewBuilds& ob)
{
    if (s_pinnedCard.folder != m.folder_name) return;
    const SkillDatabaseView& view = GetMatchSkillView(m);
    for (const auto& [partyId, party] : m.parties)
        for (const PlayerMeta& p : party.players)
        {
            const std::string key = partyId + ":" + std::to_string(p.player_number);
            if (key != s_pinnedCard.key) continue;
            const std::vector<SolvedAttribute>* build = nullptr;
            if (ob.builds)
                if (auto it = ob.builds->find(key); it != ob.builds->end()) build = &it->second;
            const std::string clean = p.encoded_name.empty() ? "(unnamed)" : SanitizePlayerName(p.encoded_name);
            ImGui::SetNextWindowPos(s_pinnedCard.pos, ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
            ImGui::PushStyleColor(ImGuiCol_Border, kColorAccent);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
            if (ImGui::Begin("Build##pinned_build_card", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoMove))
            {
                DrawBuildCard(p, clean, build, ob.pending, view, Px(22), true);
                if (ImGui::SmallButton("Close")) s_pinnedCard = PinnedBuildCard();
            }
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            return;
        }
}

// ─── Lord damage: worked out from the recording, on request ──────────────────
//
// The totals are the aggressiveness scores (the 28:00 tiebreak) from the match reader
// (ReplayWindow::ExportMatchAnalysis "lord_damage", the replay's own LordAggressiveness), never from
// the recorder's lord_events, which assumed a fixed Lord maximum and no tiebreak rules. A local match is read as soon as it is selected (its builds need the same pass); a
// cloud match waits for Calculate, which downloads it without opening the replay, then reads it.
// The job is the browser's single analysis job, bound to its match's folder.

static const char* const kLordTip = "The lord damage is worked out from the recording. Nothing opens; it takes a few seconds.";

// Set by the sidebar's "Details" link: the Statistics page opens on its Lord damage view.
static bool s_openLordView = false;

static std::string DownloadSize(uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024ull * 1024ull) snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else                             snprintf(buf, sizeof(buf), "%d KB", (int)std::max<uint64_t>(1, (bytes + 512) / 1024));
    return buf;
}

// A circular arrow (the mockup's refresh mark), h px tall, centred on c.
static void DrawRefreshGlyph(ImDrawList* dl, ImVec2 c, float h, ImU32 col)
{
    const float r = h * 0.38f, pi = 3.14159265f;
    dl->PathArcTo(c, r, -pi * 0.35f, pi * 1.35f, 20);
    dl->PathStroke(col, 0, std::max(1.0f, h * 0.11f));
    const ImVec2 tip(c.x + r * std::cos(-pi * 0.35f), c.y + r * std::sin(-pi * 0.35f));
    const float s = h * 0.26f;
    dl->AddTriangleFilled(ImVec2(tip.x + s * 0.55f, tip.y - s * 0.1f), ImVec2(tip.x - s * 0.55f, tip.y - s * 0.45f),
                          ImVec2(tip.x - s * 0.1f, tip.y + s * 0.6f), col);
}

// The tiebreak rules behind the totals, one icon and one line each (the replay's Aggressiveness
// panel holds the long wording).
static void DrawLordRulesTooltip()
{
    EnsureTextureBasePath();
    const std::string base = ui::TextureBasePath() + "\\Textures\\";
    auto file = [&](const char* rel) { return GetTextureCache().GetTexture(base + rel); };
    struct Rule { ImTextureID icon; const char* value; ImU32 col; const char* text; };
    const ImU32 plus = IM_COL32(120, 200, 120, 255), minus = IM_COL32(225, 110, 100, 255), zero = IM_COL32(150, 150, 150, 255);
    const Rule rules[] = {
        { file("Others_UI\\damagedone.png"), "+1",   plus,  "per point of damage on the enemy lord" },
        { file("Skill_Icons\\Monster_skill.jpg"), "x0.2", plus, "damage within 10.5 s of his Entourage" },
        { GetSkillIcon(479),                 "+30",  plus,  "per attack missed under Blind or a miss hex" },
        { file("Game_UI\\Enchanted.png"),    "-50",  minus, "per enchantment on your own lord (-300 near Entourage)" },
        { file("Game_UI\\WeaponSpell.png"),  "-50",  minus, "per weapon spell on your own lord (-300 near Entourage)" },
        { GetSkillIcon(810),                 "0",    zero,  "blocked attacks and other misses" },
        { GetSkillIcon(657),                 "0",    zero,  "life steal" },
    };
    ImGui::BeginTooltip();
    ImGui::TextUnformatted("Lord damage / Aggressiveness rules");
    ImGui::Spacing();
    const float lineH = ImGui::GetTextLineHeight(), ico = lineH + Px(4), valueW = ImGui::CalcTextSize("x0.2").x + Px(8);
    for (const Rule& r : rules)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (r.icon) ImGui::GetWindowDrawList()->AddImage(r.icon, p, ImVec2(p.x + ico, p.y + ico));
        const float ty = p.y + (ico - lineH) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(p.x + ico + Px(6), ty));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(r.col), "%s", r.value);
        ImGui::SetCursorScreenPos(ImVec2(p.x + ico + Px(6) + valueW, ty));
        ImGui::TextUnformatted(r.text);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + ico + Px(3)));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Hits count at the lord's full Health. The higher score wins.");
    ImGui::EndTooltip();
}

static float DrawLordDamage(ImDrawList* dl, const MatchMeta& m, const GuildLabel g[2], float x, float w, float y, bool compact)
{
    using State = MatchAnalysisJob::State;
    const float top = y;
    const bool hidden = ResultHidden(m);
    const bool mine = s_analysis.folderName == m.folder_name;
    const bool have = mine && s_analysis.state == State::Ready && s_analysis.hasLordDamage;
    const bool absent = mine && s_analysis.state == State::Ready && !s_analysis.hasLordDamage;
    const bool failed = mine && s_analysis.state == State::Failed;
    const bool downloading = (mine && s_analysis.state == State::Downloading) ||
                             (g_cloudDownloadInProgress && g_cloudDownloadFolder == m.folder_name);
    const bool computing = !downloading && mine &&
                           (s_analysis.state == State::Analysing || (s_analysis.state == State::Idle && !m.is_cloud_only));
    const bool otherDownload = g_cloudDownloadInProgress && !downloading;

    // The Guild Lord, and the heading with the state at the right.
    {
        EnsureTextureBasePath();
        if (ImTextureID lord = GetTextureCache().GetTexture(ui::TextureBasePath() + "\\Textures\\Game_UI\\guild_lord_full.png"))
            ImageContain(dl, lord, 100.0f, 217.0f, ImVec2(x, top + (compact ? Px(8) : Px(9))),
                         ImVec2(x + Px(25), top + (compact ? Px(8) : Px(9)) + (compact ? Px(50) : Px(54))));
    }
    // Calculated: the way into the full view (the Statistics page's Lord damage) - the "View
    // breakdown" label beside the heading, and the Guild Lord himself, both opening it.
    const float lordFoot = top + (compact ? Px(8) : Px(9)) + (compact ? Px(50) : Px(54));
    bool openView = false, figHov = false;
    if (have && !hidden)
    {
        const ImVec2 f0(x, top + (compact ? Px(8) : Px(9)));
        ImGui::SetCursorScreenPos(f0);
        openView = ImGui::InvisibleButton("##lib_lord_fig", ImVec2(Px(25), lordFoot - f0.y));
        figHov = ImGui::IsItemHovered();
    }
    const float cx = x + Px(36), cw = w - Px(36);
    const float headTop = top + (compact ? Px(6) : Px(10));
    const float headW = TextSpaced(dl, T(10), ImVec2(cx, R(headTop + (Px(18) - LineH(10)) * 0.5f)), v41::LordHead, "LORD DAMAGE", Px(1));
    if (have && !hidden)
    {
        // "View breakdown ›": a small gold pill right after the heading.
        const Type lt = T(10.5f, true);
        const char* label = "View breakdown";
        const float lw = TextW(lt, label) + Px(18) + Px(10), lh = Px(20);
        const ImVec2 l0(R(cx + headW + Px(8)), R(headTop + (Px(18) - lh) * 0.5f)), l1(l0.x + lw, l0.y + lh);
        ImGui::SetCursorScreenPos(l0);
        openView |= ImGui::InvisibleButton("##lib_lord_view", ImVec2(lw, lh));
        const bool hov = ImGui::IsItemHovered() || figHov;
        dl->AddRectFilled(l0, l1, hov ? Hex(0xd9b45e, 0x4d) : Hex(0xd9b45e, 0x2b), lh * 0.5f);
        Outline(dl, l0, l1, hov ? Hex(0xf0d189) : Hex(0xd9b45e, 0xd9), lh * 0.5f);
        const ImU32 fg = hov ? Hex(0xfff3d6) : Hex(0xf1dcae);
        Text(dl, lt, ImVec2(l0.x + Px(9), R(l0.y + (lh - LineH(10.5f)) * 0.5f)), fg, label);
        const float chx = l1.x - Px(10), chy = l0.y + lh * 0.5f;   // a chevron, drawn (the label's font has no ›)
        dl->AddLine(ImVec2(chx - Px(2.5f), chy - Px(3.5f)), ImVec2(chx + Px(1), chy), fg, Px(1.6f));
        dl->AddLine(ImVec2(chx + Px(1), chy), ImVec2(chx - Px(2.5f), chy + Px(3.5f)), fg, Px(1.6f));
        if (hov)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Open full lord damage");
        }
        if (openView)
        {
            s_openLordView = true;
            s_state.detailTab = DetailTab_Stats;
            s_state.detailMaximised = true;
        }
    }
    const char* status = have ? (hidden ? "Calculated \xC2\xB7 hidden" : "Calculated")
                       : absent      ? "No Guild Lord in this recording"
                       : downloading ? "Downloading"
                       : computing   ? "Calculating"
                       : failed      ? "Calculation failed"
                                     : "Not calculated";
    const Type st = T(9);
    Text(dl, st, ImVec2(R(cx + cw - TextW(st, status)), R(headTop + (Px(18) - LineH(9)) * 0.5f)), v41::LordStatus, status);

    // Two rows: tag (48 px), the track, the total (55 px, right aligned), on one scale: the larger
    // total fills its track. These compare the teams' damage; they are not the Lords' health.
    const float rowsTop = headTop + Px(18) + (compact ? Px(6) : Px(12));
    const float rowH = Px(15), rowGap = Px(12);
    const float trackX = cx + Px(48) + Px(10), trackW = cw - Px(48) - Px(55) - Px(20);
    const bool show = have && !hidden;
    const long mx = show ? std::max(s_analysis.lordDamage[0], s_analysis.lordDamage[1]) : 0;
    for (int i = 0; i < 2; i++)
    {
        const float ry = rowsTop + i * (rowH + rowGap);
        const std::string tag = g[i].tag.empty() ? std::string(i == 0 ? "Blue" : "Red") : "[" + g[i].tag + "]";
        TextFit(dl, T(11), ImVec2(cx, R(ry + (rowH - LineH(11)) * 0.5f)), Px(48), i == 0 ? v41::BlueStripe : v41::RedStripe, tag.c_str());
        const ImVec2 t0(R(trackX), R(ry + (rowH - Px(7)) * 0.5f)), t1(R(trackX + trackW), t0.y + Px(7));
        if (show)
        {
            dl->AddRectFilled(t0, t1, Hex(0xffffff, 0x08), Px(2));
            const float frac = mx > 0 ? (float)s_analysis.lordDamage[i] / (float)mx : 0.0f;
            if (frac > 0.0f)
                FillLinear(dl, t0, ImVec2(R(t0.x + (t1.x - t0.x) * frac), t1.y), Px(2), 90.0f, i == 0 ? v41::BlueFillA : v41::RedFillA,
                           i == 0 ? v41::BlueFillB : v41::RedFillB);
        }
        else
        {
            // Empty: repeating-linear-gradient(90deg, #ffffff12 0 4px, #ffffff06 4px 7px).
            dl->PushClipRect(t0, t1, true);
            for (float sx = t0.x; sx < t1.x; sx += Px(7))
            {
                dl->AddRectFilled(ImVec2(sx, t0.y), ImVec2(std::min(t1.x, sx + Px(4)), t1.y), v41::TrackA);
                dl->AddRectFilled(ImVec2(std::min(t1.x, sx + Px(4)), t0.y), ImVec2(std::min(t1.x, sx + Px(7)), t1.y), v41::TrackB);
            }
            dl->PopClipRect();
        }
        const std::string value = show ? Thousands((size_t)s_analysis.lordDamage[i]) : std::string("?");
        const Type vt = T(13);
        Text(dl, vt, ImVec2(R(cx + cw - TextW(vt, value.c_str())), R(ry + (rowH - LineH(13)) * 0.5f)), v41::LordValue, value.c_str());
    }
    if (show && ImGui::IsMouseHoveringRect(ImVec2(cx, rowsTop), ImVec2(cx + cw, rowsTop + rowH * 2.0f + rowGap)) &&
        ImGui::IsWindowHovered())
        DrawLordRulesTooltip();

    // Not worked out yet: one button over the tracks (left 52, right 57 of the rows), no extra row.
    if (!have && !absent)
    {
        const ImVec2 o0(cx + Px(52), rowsTop), o1(cx + cw - Px(57), rowsTop + rowH * 2.0f + rowGap);
        std::string label, cost;
        if (downloading)
        {
            const uint64_t tot = g_cloudDownloadTotal.load(), got = g_cloudDownloadReceived.load();
            if (tot > 0 && g_cloudDownloadFolder == m.folder_name)
                label = "Downloading " + std::to_string((int)std::min<uint64_t>(100, got * 100 / tot)) + "%\xE2\x80\xA6";
            else
                label = "Downloading\xE2\x80\xA6";
        }
        else if (computing) label = "Calculating\xE2\x80\xA6";
        else if (failed)    label = "Retry";
        else
        {
            label = "Calculate";
            if (m.is_cloud_only) cost = m.download_bytes > 0 ? " \xC2\xB7 " + DownloadSize(m.download_bytes) + " download" : " \xC2\xB7 download";
        }
        const bool busy = downloading || computing || otherDownload;
        const Type lt = T(11), ct = T(10);
        const bool glyph = !downloading && !computing;
        const float contentW = (glyph ? Px(10) + Px(5) : 0.0f) + TextW(lt, label.c_str()) + (cost.empty() ? 0.0f : TextW(ct, cost.c_str()));
        const float bw = std::max(Px(180), contentW + Px(9) * 2.0f + 2.0f), bh = Px(29);
        const ImVec2 b0(R((o0.x + o1.x - bw) * 0.5f), R((o0.y + o1.y - bh) * 0.5f)), b1(b0.x + R(bw), b0.y + bh);
        ImGui::SetCursorScreenPos(b0);
        const bool clicked = ImGui::InvisibleButton("##lib_lord_calc", ImVec2(b1.x - b0.x, bh));
        const bool hov = ImGui::IsItemHovered() && !busy;
        for (int k = 1; k <= 4; k++)   // box-shadow: 0 2px 8px #0006
            dl->AddRectFilled(ImVec2(b0.x - k, b0.y + Px(2) - k), ImVec2(b1.x + k, b1.y + Px(2) + k), Hex(0, 0x10), Px(4) + k);
        dl->AddRectFilled(b0, b1, busy ? v41::CalcBusyBg : hov ? v41::CalcHovBg : v41::CalcBg, Px(4));
        Outline(dl, b0, b1, hov ? v41::CalcHovLine : v41::CalcLine, Px(4));
        float tx = R((b0.x + b1.x - contentW) * 0.5f);
        const ImU32 fg = busy ? v41::CalcBusyText : v41::CalcText;
        const float ty = R(b0.y + (bh - LineH(11)) * 0.5f);
        if (glyph)
        {
            DrawRefreshGlyph(dl, ImVec2(tx + Px(5), b0.y + bh * 0.5f), Px(10), fg);
            tx += Px(10) + Px(5);
        }
        Text(dl, lt, ImVec2(tx, ty), fg, label.c_str());
        tx += TextW(lt, label.c_str());
        if (!cost.empty()) Text(dl, ct, ImVec2(tx, ty + Ascent(11) - Ascent(10)), v41::CalcCost, cost.c_str());
        if (ImGui::IsItemHovered())
        {
            if (failed && !s_analysis.error.empty())
                ImGui::SetTooltip("%s\n\n%s", kLordTip, s_analysis.error.c_str());
            else if (otherDownload)
                ImGui::SetTooltip("%s\n\nAnother match is downloading; this one can start when it is done.", kLordTip);
            else
                ImGui::SetTooltip("%s", kLordTip);
        }
        if (clicked && !busy)
        {
            if (failed)
            {
                StopAnalysisProcess();
                s_analysis = MatchAnalysisJob();
                s_analysis.folderName = m.folder_name;
            }
            UpdateMatchAnalysis(m, true);
        }
    }

    const float below = rowsTop + rowH * 2.0f + rowGap;
    const float bottom = std::max({ top + (compact ? Px(72) : Px(112)), below + (compact ? Px(9) : Px(13)), lordFoot + Px(8) });
    HRule(dl, x, x + w, bottom - 1.0f, Hex(0xffffff, 0x12));
    return bottom + Px(2);
}

static void DrawAside(const MatchMeta& m, bool backLink, OverviewBuilds& obOut)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float fullW = ImGui::GetContentRegionAvail().x;
    const float pad = Px(22);
    const float x = o.x + pad, w = fullW - pad * 2.0f;
    float y = o.y;
    const bool hidden = ResultHidden(m);
    const std::string& folder = m.folder_name;

    // Compact (owner 2026-11-25): on a panel too short for both rosters at full size (about 1250 px
    // of page), a thinner hero, the four facts on one line, a tighter lord block and replay button,
    // and 31 px player rows: about 250 px less, so both teams fit on a 1080p screen.
    const bool compact = ImGui::GetContentRegionAvail().y < Px(1250);

    // The match reader (builds, lord damage): a local match is read as soon as it is selected.
    UpdateMatchAnalysis(m, false);

    // Single-pane width: the way back to the list, above the hero.
    if (backLink)
    {
        const float bh = Px(40);
        if (LinkText(dl, "##lib_back", T(13), ImVec2(x, y + (bh - LineH(13)) * 0.5f), w, col::Link, "\xE2\x80\xB9  Back to the matches"))
            s_page.showDetail = false;
        y += bh;
    }

    // Hero: the hall's loading screen across the panel, 155 px, at 0.9 opacity, masked from opaque
    // at half its height to clear at the bottom, with a #13120f fade rising from its foot.
    {
        const float heroH = compact ? Px(78) : Px(155);
        const ImVec2 a(o.x, y), b(o.x + fullW, y + heroH);
        const float mid = a.y + heroH * 0.5f;
        if (const ArtTexture* art = LoadArt(HallArtPath(m.map_id)))
        {
            // Cover, centred: the two halves drawn separately so the lower one can fade.
            const float bw = b.x - a.x, bhh = b.y - a.y;
            const float s = std::max(bw / art->w, bhh / art->h);
            const float uw = bw / (art->w * s), vh = bhh / (art->h * s);
            const ImVec2 uv0((1.0f - uw) * 0.5f, (1.0f - vh) * 0.5f), uv1(uv0.x + uw, uv0.y + vh);
            const float vm = (uv0.y + uv1.y) * 0.5f;
            const ImTextureID tex = (ImTextureID)art->srv.Get();
            dl->AddImage(tex, a, ImVec2(b.x, mid), uv0, ImVec2(uv1.x, vm), IM_COL32(255, 255, 255, 230));
            dl->AddImage(tex, ImVec2(a.x, mid), b, ImVec2(uv0.x, vm), uv1, IM_COL32_WHITE);
            ImDrawVert* v = dl->VtxBuffer.Data + dl->VtxBuffer.Size - 4;   // a, (b.x, a.y), b, (a.x, b.y)
            v[0].col = v[1].col = IM_COL32(255, 255, 255, 230);
            v[2].col = v[3].col = IM_COL32(255, 255, 255, 0);
        }
        // The fade from the foot (alpha along the mask): 0 at 10% down, about 0.4 at half, 0 at the foot.
        const ImU32 f = v41::HeroFade & ~IM_COL32_A_MASK;
        const float y10 = a.y + heroH * 0.1f;
        dl->AddRectFilledMultiColor(ImVec2(a.x, y10), ImVec2(b.x, mid), f, f, f | (0x66u << IM_COL32_A_SHIFT), f | (0x66u << IM_COL32_A_SHIFT));
        dl->AddRectFilledMultiColor(ImVec2(a.x, mid), b, f | (0x66u << IM_COL32_A_SHIFT), f | (0x66u << IM_COL32_A_SHIFT), f, f);
        y = b.y;
    }

    // SELECTED MATCH · 18 NOV 2026, over the hero's foot.
    y -= compact ? Px(18) : Px(24);
    const std::string meta = "SELECTED MATCH \xC2\xB7 " + MatchDate(m, true);
    TextSpaced(dl, T(10), ImVec2(x, R(y)), v41::Meta, meta.c_str(), Px(1.5f));
    y += LineH(10);

    // The hall: 26 px, its icon beside it.
    y += compact ? Px(6) : Px(11);
    {
        const float titleH = Px(32);
        if (ImTextureID ico = GetGuildHallIcon())
            dl->AddImage(ico, ImVec2(x, R(y + (titleH - Px(22)) * 0.5f)), ImVec2(x + Px(22), R(y + (titleH - Px(22)) * 0.5f) + Px(22)));
        const char* mapName = GetMapName(m.map_id);
        const std::string mapStr = mapName ? mapName : ("Map " + std::to_string(m.map_id));
        const Type tt = T(26);
        float tx = x + Px(31);
        const float ty = R(y + (titleH - LineH(26)) * 0.5f);
        // letter-spacing -0.5px
        for (const char* it = mapStr.c_str(); *it;)
        {
            unsigned int cp = 0;
            const int n = ImTextCharFromUtf8(&cp, it, nullptr);
            if (n <= 0) break;
            const float cwid = TextW(tt, it, it + n);
            if (tx + cwid > x + w) break;
            Text(dl, tt, ImVec2(tx, ty), v41::Title, std::string(it, it + n).c_str());
            tx += cwid - Px(0.5f);
            it += n;
        }
        y += titleH + (compact ? Px(10) : Px(22));
    }

    // The facts, in a box: occasion, duration, flux, notes.
    {
        const float boxPad = compact ? Px(9) : Px(12);
        const float factH = LineH(10) + Px(3) + LineH(13);
        const float boxH = 2.0f + boxPad * 2.0f + (compact ? factH : factH * 2.0f + Px(10));
        const ImVec2 f0(x, R(y)), f1(x + w, R(y) + R(boxH));
        dl->AddRectFilled(f0, f1, v41::FactsBg, Px(6));
        Outline(dl, f0, f1, v41::FactsLine, Px(6));
        const float colGap = compact ? Px(10) : Px(24);
        const float colW = compact ? (w - 2.0f - boxPad * 2.0f - colGap * 3.0f) * 0.25f : (w - 2.0f - boxPad * 2.0f - colGap) * 0.5f;
        const Type label = T(10), value = T(13);
        // `size`: the drawn size in the 20 px slot (the clock fills its picture, the arena icon does not).
        auto icon = [&](float fx, float fy, ImTextureID tex, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1), float size = 20.0f) {
            if (!tex) return;
            const float sz = Px(size), ix = R(fx + (Px(20) - sz) * 0.5f);
            const float iy = R(fy + (factH - sz) * 0.5f);
            dl->AddImage(tex, ImVec2(ix, iy), ImVec2(ix + sz, iy + sz), uv0, uv1);
        };
        // Occasion, Duration | Flux, Notes: two by two, or four in a row when compact.
        const float iconW = compact ? Px(26) : Px(30);
        const float c0 = f0.x + 1.0f + boxPad, c1 = c0 + colW + colGap;
        const float cF = compact ? c1 + colW + colGap : c0, cN = compact ? cF + colW + colGap : c1;
        const float r0 = f0.y + 1.0f + boxPad, r1 = compact ? r0 : r0 + factH + Px(10);
        const float vx0 = c0 + iconW, vx1 = c1 + iconW, vw = colW - iconW;
        const float vxF = cF + iconW, vxN = cN + iconW;
        const float valueY = LineH(10) + Px(3);

        icon(c0, r0, GetArenaIcon());
        Text(dl, label, ImVec2(vx0, r0), v41::FactLabel, "Occasion");
        TextFit(dl, value, ImVec2(vx0, r0 + valueY), vw, v41::FactValue, m.occasion.empty() ? "-" : m.occasion.c_str());

        icon(c1, r0, GetDurationIcon(), ImVec2(0, 0), ImVec2(1, 1), 15.0f);
        Text(dl, label, ImVec2(vx1, r0), v41::FactLabel, "Duration");
        Text(dl, T(11), ImVec2(vx1, r0 + valueY + (LineH(13) - LineH(11)) * 0.5f), hidden ? col::HiddenText : v41::FactValue,
             hidden ? kHiddenWord : (m.match_duration.empty() ? "-" : m.match_duration.c_str()));

        icon(cF, r1, GetFluxIcon());
        Text(dl, label, ImVec2(vxF, r1), v41::FactLabel, "Flux");
        TextFit(dl, value, ImVec2(vxF, r1 + valueY), vw, v41::FactValue, m.flux.empty() ? "None" : m.flux.c_str());
        if (!m.flux.empty() && ImGui::IsMouseHoveringRect(ImVec2(cF, r1), ImVec2(cF + colW, r1 + factH)) && ImGui::IsWindowHovered())
            if (const char* desc = GetFluxDescription(m.flux))
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(320.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", m.flux.c_str());
                ImGui::Spacing();
                ImGui::TextWrapped("%s", desc);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

        ImVec2 uv0, uv1;
        icon(cN, r1, GetNotesScrollIcon(&uv0, &uv1), uv0, uv1);
        Text(dl, label, ImVec2(vxN, r1), v41::FactLabel, "Notes");
        const std::string note = MatchNotes::Get().GetNote(folder);
        std::string firstLine = note.substr(0, note.find('\n'));
        const ImVec2 linkPos(vxN, r1 + valueY);
        if (LinkText(dl, "##lib_note_link", T(13), linkPos, vw, col::Link, note.empty() ? "Add a note" : firstLine.c_str()))
        {
            s_state.browserNoteBuffer = note;
            s_state.browserNoteMatchId = folder;
            ImGui::OpenPopup("##lib_notes");
        }
        if (ImGui::IsItemHovered() && !note.empty())
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(360.0f);
            ImGui::TextUnformatted(note.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::SetNextWindowPos(ImVec2(x, f1.y + Px(6)));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(col::Popover));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(col::PopoverRule));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(10), Px(10)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        if (ImGui::BeginPopup("##lib_notes"))
        {
            if (s_state.browserNoteMatchId != folder)
            {
                s_state.browserNoteBuffer = MatchNotes::Get().GetNote(folder);
                s_state.browserNoteMatchId = folder;
            }
            ImGui::TextColored(kColorTextDim, "Notes on this match (kept on this PC)");
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            if (ImGui::InputTextMultiline("##lib_note_edit", &s_state.browserNoteBuffer, ImVec2(w, Px(120))))
                MatchNotes::Get().SetNote(folder, s_state.browserNoteBuffer);
            if (ImGui::Button("Done")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);

        y = f1.y + (compact ? Px(6) : Px(12));
    }

    std::string ft1, ft2;
    ParseFolderTags(m.folder_name, ft1, ft2);
    const GuildLabel guilds[2] = { GetPartyGuild(m, "1", ft1), GetPartyGuild(m, "2", ft2) };

    // Lord damage.
    y = DrawLordDamage(dl, m, guilds, x, w, y, compact);

    // Voice comms attached on this PC: the only place the list's matches show them.
    const Comms::Badge badge = Comms::BadgeFor(folder);
    if (badge.present)
    {
        y += Px(10);
        const float rowH = Px(30);
        const float textH = LineH(12) + Px(3) + LineH(10);
        DrawHeadsetGlyph(dl, ImVec2(x, R(y + (rowH - Px(20)) * 0.5f)), Px(20), badge.exists ? v41::VoiceIcon : Hex(0x828288));
        const float ty = R(y + (rowH - textH) * 0.5f);
        Text(dl, T(12), ImVec2(x + Px(29), ty), v41::VoiceTitle, "Voice comms attached");
        Text(dl, T(10), ImVec2(x + Px(29), ty + LineH(12) + Px(3)), v41::VoiceSub,
             badge.exists ? "Available on this PC" : "The file is missing on this PC");
        const float bw = SmallButtonWidth("Details", Px(9), true);
        const float bh = 2.0f + Px(10) + LineH(10);
        const ImVec2 bp(R(x + w - bw), R(y + (rowH - bh) * 0.5f));
        if (SmallButton(dl, "##lib_voice_btn", bp, "Details", Px(9), Px(5), true)) ImGui::OpenPopup("##lib_voice");
        ImGui::SetNextWindowPos(ImVec2(x + w - Px(280), bp.y + bh + Px(4)));
        ImGui::SetNextWindowSize(ImVec2(Px(280), 0));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(col::Popover));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(col::PopoverRule));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(12), Px(12)));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        if (ImGui::BeginPopup("##lib_voice"))
        {
            if (ImFont* bold = GuiGlobalConstants::boldFont) ImGui::PushFont(bold);
            ImGui::TextUnformatted(badge.name.c_str());
            if (GuiGlobalConstants::boldFont) ImGui::PopFont();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Hex(0xaaa08e)), "%s",
                               badge.exists ? "Attached on this PC. It plays in the replay window, in time with the match."
                                            : "Attached on this PC, but the file is no longer where it was.");
            ImGui::PopTextWrapPos();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        y += rowH;
    }

    // Replay match: 43 px, 11 above and 12 below.
    const float replayH = compact ? Px(38) : Px(43);
    y += compact ? Px(8) : Px(11);
    DrawReplayButton(dl, m, ImVec2(x, R(y)), ImVec2(w, replayH));
    y += replayH + (compact ? Px(8) : Px(12));

    // Tabs: Builds here; Stats and Map open the full-width details. 15 px, 28 apart, a faint rule
    // 12 px under them.
    {
        const Type tn = T(15), tb = T(15, true);
        struct Tab { const char* label; int tab; } tabs[] = {
            { "Builds", DetailTab_Overview }, { "Stats", DetailTab_Stats }, { "Map", DetailTab_Map } };
        float tx = x;
        for (const Tab& tab : tabs)
        {
            const bool active = tab.tab == DetailTab_Overview;
            const bool locked = hidden && !active;
            const Type& tt = active ? tb : tn;
            const float tw = TextW(tt, tab.label);
            ImGui::SetCursorScreenPos(ImVec2(tx, y));
            ImGui::PushID(tab.tab);
            const bool clicked = ImGui::InvisibleButton("##tab", ImVec2(tw, LineH(15) + Px(10)));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const ImU32 c = active ? v41::TabOn : locked ? WithAlpha(Hex(0xa89a82), 0.45f) : hov ? col::Text : Hex(0xa89a82);
            Text(dl, tt, ImVec2(tx, y), c, tab.label);
            if (hov && !active)
                ImGui::SetTooltip("%s", locked ? (tab.tab == DetailTab_Stats ? "Stats are hidden in spoiler-free mode"
                                                                              : "The map replay is hidden in spoiler-free mode")
                                               : (tab.tab == DetailTab_Stats ? "Match stats, full width" : "The match on the map, full width"));
            if (clicked && !active && !locked)
            {
                s_state.detailTab = tab.tab;
                s_state.detailMaximised = true;
            }
            tx += tw + Px(28);
        }
        y += LineH(15) + (compact ? Px(8) : Px(12));
        HRule(dl, x, x + w, y, Hex(0xffffff, 0x14));
        y += 1.0f;
    }

    // Builds.
    OverviewBuilds ob;
    {
        using State = MatchAnalysisJob::State;
        if (s_analysis.folderName == m.folder_name && s_analysis.state == State::Ready)
            ob.builds = &s_analysis.builds;
        else
            ob.pending = s_analysis.state == State::Idle && m.is_cloud_only ? "Attributes are read once the match is downloaded"
                       : s_analysis.state == State::Downloading           ? "Downloading the match..."
                       : s_analysis.state == State::Failed                ? "Attributes unavailable"
                                                                          : "Solving attributes...";
    }
    obOut = ob;
    const int winnerSide = ShownWinner(m);
    y = DrawRosterTeam(dl, m, "1", guilds[0], false, winnerSide == 1, x, w, y, ob, compact);
    y = DrawRosterTeam(dl, m, "2", guilds[1], true, winnerSide == 2, x, w, y, ob, compact);

    // The player context menu (right-click in a team).
    if (s_page.openCtx)
    {
        s_page.openCtx = false;
        ImGui::OpenPopup("##lib_player_ctx");
    }
    if (ImGui::BeginPopup("##lib_player_ctx"))
    {
        ImGui::TextColored(kColorTextDim, "%s", s_page.ctxName.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Copy build template")) ImGui::SetClipboardText(s_page.ctxTemplate.c_str());
        if (ImGui::MenuItem("Copy the whole team's templates")) ImGui::SetClipboardText(s_page.ctxTeamTemplates.c_str());
        if (ImGui::MenuItem("Show every match with this player")) FilterBySearchTerm(s_page.ctxName);
        ImGui::EndPopup();
    }

    // Rating and Share, under a faint rule (V41's .note: 14 px above, 13 px inside).
    y += Px(14);
    HRule(dl, x, x + w, y, Hex(0xffffff, 0x12));
    y += 1.0f + Px(13);
    {
        const float lh = LineH(15);
        const Type rl = T(11);
        Text(dl, rl, ImVec2(x, y + (lh - LineH(11)) * 0.5f), v41::FactLabel, "Rating");
        DrawRatingStars(dl, ImVec2(x + TextW(rl, "Rating") + Px(10), y), lh, folder);
        DrawShareLink(dl, m, x + w, y, lh);
        y += lh;
    }
    y += pad;

    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(fullW, y - o.y));
}

// ─── The match's Statistics page (V48) ───────────────────────────────────────
// Its own file, this namespace: it uses the page's type, panels and helpers above.
#include "draw_library_stats.inl"

// ─── The page ────────────────────────────────────────────────────────────────

// The page's scale follows the window. When it settles on a new step for half a second, the page's
// type is re-baked at it (LoadSelectedFont, at the next frame's start); the first time at once.
// The characters the library's names use beyond Basic Latin and Latin-1 (Latin Extended, Greek,
// Cyrillic), baked with the page's type so every such name draws from it.
static const std::vector<ImWchar>& LibraryChars(const std::vector<MatchMeta>& matches)
{
    static int s_generation = -2;
    static size_t s_count = 0;
    static std::vector<ImWchar> s_chars;
    if (s_generation == s_libraryGeneration && s_count == matches.size()) return s_chars;
    s_generation = s_libraryGeneration;
    s_count = matches.size();
    std::set<ImWchar> found;
    auto scan = [&](const std::string& text) {
        for (const char* it = text.c_str(); *it;)
        {
            unsigned int cp = 0;
            const int n = ImTextCharFromUtf8(&cp, it, nullptr);
            if (n <= 0) break;
            if (cp >= 0x0100 && cp < 0x0800) found.insert(static_cast<ImWchar>(cp));
            it += n;
        }
    };
    for (const MatchMeta& m : matches)
    {
        for (const auto& [id, guild] : m.guilds)
        {
            scan(guild.name);
            scan(guild.tag);
        }
        for (const auto& [id, party] : m.parties)
            for (const PlayerMeta& player : party.players) scan(player.encoded_name);
    }
    s_chars.assign(found.begin(), found.end());
    return s_chars;
}

// The page's scale from the window, and its type baked at it: on the frame the scale changes (a
// resize draws no frames until it ends), before any of the page's text, so no frame shows stand-in
// type. The first frame bakes too, at the real window size.
static void UpdateScale(const std::vector<MatchMeta>& matches)
{
    const ImGuiIO& io = ImGui::GetIO();
    s_k = GuiGlobalConstants::LibraryPageScale(io.DisplaySize.x, io.DisplaySize.y);
    LibraryFonts::Bake(s_k, LibraryChars(matches));
}

static void Draw(const std::vector<MatchMeta>& matches)
{
    UpdateScale(matches);
    UpdateFluxMonths(matches);
    const auto& filtered = FilterMatches(matches);

    // The Favorites view narrows what the filters left.
    static std::vector<int> s_favRows;
    const bool favOnly = s_page.favouritesOnly;
    if (favOnly)
    {
        s_favRows.clear();
        const MatchFavourites& favs = MatchFavourites::Get();
        for (int i = 0; i < (int)filtered.size(); i++)
            if (favs.IsFavourite(filtered[i].meta->folder_name)) s_favRows.push_back(i);
    }
    const int rowCount = favOnly ? (int)s_favRows.size() : (int)filtered.size();
    auto rowAt = [&](int r) -> const FilteredMatch& { return favOnly ? filtered[s_favRows[r]] : filtered[r]; };

    if (!s_page.autoSelected)
    {
        s_page.autoSelected = true;
        if (s_state.selectedMatchIndex < 0 && rowCount > 0) s_state.selectedMatchIndex = rowAt(0).originalIndex;
    }

    // DEV TOOL (GWO_LIBRARY_SHOT_PAGE): select a match and open a menu by code.
    if (!g_libraryPageTestSelect.empty())
    {
        for (int r = 0; r < rowCount; r++)
            if (rowAt(r).meta->folder_name == g_libraryPageTestSelect)
            {
                s_state.selectedMatchIndex = rowAt(r).originalIndex;
                s_page.scrollToRow = r;
                RunLog::Line("library-page: selected '%s' (row %d of %d)", g_libraryPageTestSelect.c_str(), r, rowCount);
            }
        g_libraryPageTestSelect.clear();
    }
    if (!g_libraryPageTestMenu.empty())
    {
        // "menu:text" also types text in that menu's field (map, flux).
        const size_t colon = g_libraryPageTestMenu.find(':');
        s_page.openMenu = g_libraryPageTestMenu == "-" ? std::string() : g_libraryPageTestMenu.substr(0, colon);
        if (colon != std::string::npos)
        {
            SyncMenuQueries();
            char* field = s_page.openMenu == "flux" ? s_page.fluxQuery : s_page.mapQuery;
            snprintf(field, 64, "%s", g_libraryPageTestMenu.substr(colon + 1).c_str());
        }
        g_libraryPageTestMenu.clear();
    }

    ImGuiIO& io = ImGui::GetIO();
    const float top = GuiGlobalConstants::ContentTop();
    ImGui::SetNextWindowPos(ImVec2(0, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y - top), ImGuiCond_Always);
    const int themeColors = PushGlassTheme();
    // The legacy page's frame border, which the filter widgets and the full-width details expect.
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(col::Page));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoNavInputs;
    const bool open = ImGui::Begin("##replay_browser", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    if (!open)
    {
        ImGui::End();
        ImGui::PopStyleVar();   // FrameBorderSize
        PopGlassTheme(themeColors);
        return;
    }

    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool validSel = s_state.selectedMatchIndex >= 0 && s_state.selectedMatchIndex < (int)matches.size();

    // Stats: the match's Statistics page (draw_library_stats.inl), over the hall's scene.
    if (s_state.detailMaximised && validSel && s_state.detailTab == DetailTab_Stats)
    {
        DrawStatsPage(matches[s_state.selectedMatchIndex], wp, ws);
        ImGui::End();
        ImGui::PopStyleVar();   // FrameBorderSize
        PopGlassTheme(themeColors);
        return;
    }

    // Map: the full-width details, until their Show list.
    if (s_state.detailMaximised && validSel)
    {
        ImGui::SetCursorScreenPos(wp);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        ImGui::BeginChild("##lib_detail_full", ws, ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        DrawMatchDetailPanel(matches[s_state.selectedMatchIndex], true);
        ImGui::EndChild();
        ImGui::End();
        ImGui::PopStyleVar();   // FrameBorderSize
        PopGlassTheme(themeColors);
        return;
    }

    // The scene behind the page (the selected match's hall); the list and the sidebar are glass
    // panels over it, inside V41's margins (14 px above and below, 22 px at the sides).
    const ImVec2 pageA = wp, pageB(wp.x + ws.x, wp.y + ws.y);
    UpdateBackdrop(validSel ? matches[s_state.selectedMatchIndex].map_id : -1);
    DrawBackdrop(dl, pageA, pageB);
    const float mx = Px(22), my = Px(14);

    // Widths: below 1000 px the page is one pane, the list or the selected match.
    const bool single = ws.x < Px(1000);
    if (!single) s_page.showDetail = false;
    const bool detailPane = single && s_page.showDetail && validSel;
    // Owner 2026-11-25: a wider sidebar than V41's 490 px, and the two panels apart (10 px), each
    // with its own outline, instead of sharing a seam.
    float asideW = ws.x >= Px(1500) ? Px(540) : ws.x >= Px(1250) ? Px(470) : Px(430);
    if (single) asideW = detailPane ? ws.x - mx * 2.0f : 0.0f;
    const float panelGap = Px(10);
    const float listW = single ? ws.x - mx * 2.0f : ws.x - mx * 2.0f - asideW - panelGap;

    if (!detailPane)
    {
        const ImVec2 panelA(R(wp.x + mx), R(wp.y + my));
        const ImVec2 panelB(R(panelA.x + listW), R(pageB.y - my));
        DrawPanel(dl, pageA, pageB, panelA, panelB,
                  ImDrawFlags_RoundCornersAll, true);
        const float x0 = R(panelA.x + Px(20)), x1 = R(panelB.x - Px(20));
        float y = R(panelA.y + Px(22));

        // Search, All matches / Favorites, Update matches, Spoiler-free: 44 px, 5 px under it, then the
        // filters 18 px further down.
        y += DrawToolbar(dl, x0, x1, y) + Px(5) + Px(18);

        // The filter buttons (two lines when the active ones need them), the sort; 17 px under them.
        y += DrawFilterRow(dl, x0, x1, y) + Px(17);

        // The open filter, in the flow: it pushes the list down.
        y += DrawFilterPanel(matches, x0, x1, y, (panelB.y - y) * 0.55f);

        const ListGeom g = ComputeListGeom(x1 - x0);
        DrawLabelsRow(dl, x0, x1, y, g);
        y += Px(32);

        // The rows: a child as wide as the list plus a gutter for its scrollbar, in the panel's right
        // padding, so the rows keep their width whether it shows or not.
        const float rowH = Px(66);
        const float footH = Px(16) + LineH(11) + Px(14);
        const float listTop = R(y);
        const float room = std::max(rowH, panelB.y - footH - listTop);
        const float listH = std::min(room, std::max(rowH, rowH * rowCount));
        ImGui::SetCursorScreenPos(ImVec2(x0, listTop));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(Hex(0x3b3830)));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImGui::ColorConvertU32ToFloat4(Hex(0x58513f)));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImGui::ColorConvertU32ToFloat4(Hex(0x7a6b48)));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Px(8));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, Px(4));
        ImGui::BeginChild("##lib_rows", ImVec2(g.w + Px(16), listH), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
        {
            ImDrawList* ldl = ImGui::GetWindowDrawList();
            const ImVec2 lo = ImGui::GetCursorScreenPos();
            if (s_page.scrollToRow >= 0)
            {
                const float rowTop = s_page.scrollToRow * rowH, sy = ImGui::GetScrollY();
                if (rowTop < sy) ImGui::SetScrollY(rowTop);
                else if (rowTop + rowH > sy + listH) ImGui::SetScrollY(rowTop + rowH - listH);
                s_page.scrollToRow = -1;
            }
            const bool listHovered = ImGui::IsWindowHovered();
            if (rowCount == 0)
            {
                const char* empty = favOnly ? "No favorites here yet: click a match's star to keep it at hand."
                                            : "No match fits these filters.";
                Text(ldl, T(13), ImVec2(lo.x + Px(20), lo.y + (rowH - LineH(13)) * 0.5f), col::Muted, empty);
                ImGui::Dummy(ImVec2(g.w, rowH));
            }
            ImGuiListClipper clipper;
            clipper.Begin(rowCount, rowH);
            while (clipper.Step())
                for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++)
                {
                    const FilteredMatch& fm = rowAt(r);
                    const MatchMeta& m = *fm.meta;
                    const ImVec2 a(lo.x, lo.y + r * rowH), b(a.x + g.w, a.y + rowH);
                    const float cy = a.y + rowH * 0.5f;
                    const bool selected = s_state.selectedMatchIndex == fm.originalIndex;
                    ImGui::PushID(fm.originalIndex);
                    ImGui::SetCursorScreenPos(a);
                    ImGui::SetNextItemAllowOverlap();
                    const bool clicked = ImGui::InvisibleButton("##row", ImVec2(g.w, rowH));
                    const bool rowItemHovered = ImGui::IsItemHovered();
                    const bool rowHot = listHovered && ImGui::IsMouseHoveringRect(a, b);

                    // V41: every row the same faint tint; the hovered one lighter; the selected one a
                    // warm gradient, 5 px rounding, a 3 px gold edge on its left.
                    ldl->AddRectFilled(a, b, v41::Row);
                    if (selected)
                    {
                        ldl->AddRectFilled(a, b, v41::SelAccent, Px(5));
                        FillLinear(ldl, ImVec2(a.x + Px(3), a.y), b, Px(5), 100.0f, v41::SelA, v41::SelB,
                                   ImDrawFlags_RoundCornersRight);
                        ldl->AddLine(ImVec2(a.x + Px(5), a.y + 1.5f), ImVec2(b.x - Px(5), a.y + 1.5f), Hex(0xffffff, 0x0c));
                    }
                    else
                    {
                        if (rowHot) ldl->AddRectFilled(a, b, v41::RowHover, Px(5));
                        HRule(ldl, a.x, b.x, b.y - 1.0f, v41::RowRule);
                    }
                    if (auto hl = s_state.highlightStartTimes.find(m.folder_path); hl != s_state.highlightStartTimes.end())
                    {
                        const float alpha = std::max(0.0f, 1.0f - ((float)ImGui::GetTime() - hl->second) / 2.0f) * 0.15f;
                        if (alpha > 0.0f) ldl->AddRectFilled(a, b, IM_COL32(212, 160, 32, (int)(alpha * 255)));
                    }

                    // Favourite star (20 px type).
                    const bool fav = MatchFavourites::Get().IsFavourite(m.folder_name);
                    ImGui::SetCursorScreenPos(ImVec2(a.x + g.starX, cy - Px(13)));
                    const bool starClicked = ImGui::InvisibleButton("##fav", ImVec2(Px(18), Px(26)));
                    const bool starHov = ImGui::IsItemHovered();
                    const ImU32 starCol = starHov ? col::LinkHover : (fav || selected) ? v41::StarOn : v41::Star;
                    DrawStar(ldl, ImVec2(a.x + g.starX + Px(9), cy + Px(1)), Px(7.6f), starCol, fav, 1.15f);
                    if (starClicked) MatchFavourites::Get().Toggle(m.folder_name);
                    if (starHov) ImGui::SetTooltip(fav ? "Remove from favorites" : "Add to favorites");

                    // Date.
                    {
                        const std::string date = MatchDate(m, false);
                        TextFit(ldl, T(11), ImVec2(a.x + g.dateX, R(cy - LineH(11) * 0.5f)), Px(95), v41::Date, date.c_str());
                    }

                    // Event: one badge style for every occasion, finals included.
                    if (g.event && !m.occasion.empty())
                    {
                        const Type et = T(11, true);
                        const float maxW = Px(105);
                        const float tw = std::min(TextW(et, m.occasion.c_str()), maxW - Px(14) - 2.0f);
                        const float bw = R(tw + Px(14) + 2.0f), bh = R(LineH(11) * 1.07f + Px(6) + 2.0f);
                        const ImVec2 e0(a.x + g.eventX, R(cy - bh * 0.5f)), e1(e0.x + bw, e0.y + bh);
                        ldl->AddRectFilled(e0, e1, v41::EventBg, Px(4));
                        Outline(ldl, e0, e1, v41::EventLine, Px(4));
                        TextFit(ldl, et, ImVec2(e0.x + 1.0f + Px(7), R(cy - LineH(11) * 0.5f)), tw + 1.0f, v41::Event, m.occasion.c_str());
                    }

                    // The teams.
                    const EliteLineup& lineup = LineupOf(m);
                    const int winner = ShownWinner(m);
                    DrawTeamCell(ldl, g, a.x + g.team1X, cy, m, fm.guild1, winner == 1, lineup.team[0], rowHot);
                    {
                        const ImVec2 vc(a.x + g.vsX + Px(10), cy);
                        ldl->AddCircle(vc, Px(11) - 0.5f, v41::VersusLine, 24, 1.0f);
                        const Type vt = T(10);
                        Text(ldl, vt, ImVec2(R(vc.x - TextW(vt, "vs") * 0.5f), R(cy - LineH(10) * 0.5f)), v41::Versus, "vs");
                    }
                    DrawTeamCell(ldl, g, a.x + g.team2X, cy, m, fm.guild2, winner == 2, lineup.team[1], rowHot);

                    // Map: the hall's thumbnail (54 x 36) and its name beside it, wrapping.
                    {
                        const ImVec2 t0(a.x + g.mapX, R(cy - Px(18))), t1(t0.x + Px(54), t0.y + Px(36));
                        if (const ArtTexture* art = LoadArt(HallArtPath(m.map_id)))
                            ImageCover(ldl, (ImTextureID)art->srv.Get(), art->w, art->h, t0, t1, IM_COL32(255, 255, 255, 242), Px(3));
                        else
                            ldl->AddRectFilled(t0, t1, Hex(0x2a2823), Px(3));
                        TextWrap2(ldl, T(11), t1.x + Px(9), cy, g.mapW - Px(54) - Px(9), Px(14.85f), v41::MapName, fm.mapName);
                    }

                    // Duration.
                    {
                        const bool hidden = ResultHidden(m);
                        const char* dur = hidden ? kHiddenWord : (m.match_duration.empty() ? "-" : m.match_duration.c_str());
                        Text(ldl, T(11), ImVec2(a.x + g.durX, R(cy - LineH(11) * 0.5f)), hidden ? col::HiddenText : v41::Duration, dur);
                    }

                    if (clicked)
                    {
                        s_state.selectedMatchIndex = fm.originalIndex;
                        if (single) s_page.showDetail = true;
                    }
                    if (rowItemHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !g_cloudDownloadInProgress)
                    {
                        s_state.selectedMatchIndex = fm.originalIndex;
                        g_pendingReplay.requested = true;
                        g_pendingReplay.match = m;
                    }
                    if (m.is_cloud_only && rowHot && ImGui::IsMouseHoveringRect(ImVec2(a.x + g.mapX, a.y), b) &&
                        !ImGui::IsAnyItemActive())
                        ImGui::SetTooltip("Cloud only - downloads when you open it");

                    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
                    ImGui::PopID();
                }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);

        // Footer: the count, and the way back to the legacy layout.
        {
            const Type t = T(11);
            const float fy = listTop + listH + Px(16);
            std::string left = Thousands((size_t)rowCount) + " of " + Thousands(matches.size()) + " matches";
            if (favOnly) left += " \xC2\xB7 favorites";
            Text(dl, t, ImVec2(x0, fy), v41::Footer, left.c_str());
            const char* legacy = "Legacy layout";
            const float lw = TextW(t, legacy);
            if (LinkText(dl, "##lib_legacy", t, ImVec2(R(x1 - lw), fy), lw + 1.0f, v41::Footer, legacy))
            {
                GuiGlobalConstants::replay_library_legacy = 1;
                GuiGlobalConstants::SaveSettings();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Switch to the previous Library layout: the table or the cards, with the filter sidebar.\n"
                                  "Its top bar has a button to come back, and so does Settings > Browser.");
        }
    }

    // The sidebar: its own panel; it scrolls on its own.
    OverviewBuilds ob;
    bool asideDrawn = false;
    if (asideW > 0.0f)
    {
        const ImVec2 panelA(R(detailPane ? wp.x + mx : wp.x + mx + listW + panelGap), R(wp.y + my));
        const ImVec2 panelB(R(pageB.x - mx), R(pageB.y - my));
        DrawPanel(dl, pageA, pageB, panelA, panelB,
                  ImDrawFlags_RoundCornersAll, true);
        ImGui::SetCursorScreenPos(ImVec2(panelA.x + 1.0f, panelA.y + 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(Hex(0x3b3830)));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Px(8));
        ImGui::BeginChild("##lib_aside", ImVec2(panelB.x - panelA.x - 2.0f, panelB.y - panelA.y - 2.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoNav);
        if (validSel)
        {
            DrawAside(matches[s_state.selectedMatchIndex], detailPane, ob);
            asideDrawn = true;
        }
        else
        {
            const ImVec2 o = ImGui::GetCursorScreenPos();
            const char* msg = "Select a match to see its builds and replay it.";
            const Type t = T(13);
            Text(ImGui::GetWindowDrawList(), t, ImVec2(o.x + (panelB.x - panelA.x - TextW(t, msg)) * 0.5f, o.y + ws.y * 0.4f),
                 col::Muted, msg);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    }

    // The keyboard: Up / Down / Page Up / Page Down / Home / End move the selection, Enter opens it.
    if (!io.WantTextInput && s_page.openMenu.empty() && rowCount > 0 &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    {
        int cur = -1;
        for (int r = 0; r < rowCount && cur < 0; r++)
            if (rowAt(r).originalIndex == s_state.selectedMatchIndex) cur = r;
        const int page = std::max(1, (int)((ws.y * 0.6f) / Px(66)));
        int next = cur;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  next = cur < 0 ? 0 : std::min(cur + 1, rowCount - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    next = cur < 0 ? 0 : std::max(cur - 1, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown))   next = std::min(std::max(cur, 0) + page, rowCount - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))     next = std::max(std::max(cur, 0) - page, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) next = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End, false))  next = rowCount - 1;
        if (next != cur && next >= 0)
        {
            s_state.selectedMatchIndex = rowAt(next).originalIndex;
            s_page.scrollToRow = next;
        }
        if (cur >= 0 && !g_cloudDownloadInProgress &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        {
            g_pendingReplay.requested = true;
            g_pendingReplay.match = *rowAt(cur).meta;
        }
    }

    ImGui::End();

    if (asideDrawn) DrawPinnedCard(matches[s_state.selectedMatchIndex], ob);
    ImGui::PopStyleVar();   // FrameBorderSize
    PopGlassTheme(themeColors);
}

} // namespace libpage

// DEV TOOL (GWO_LIBRARY_SHOT_PAGE, MapBrowser.cpp): a few filters at once, for the picture of the
// filter buttons; false clears them all.
void LibraryPageTestFilters(bool on)
{
    if (!on)
    {
        libpage::ClearAllFilters();
        return;
    }
    s_state.selectedMaps = { "Imperial Isle", "Isle of Meditation" };
    s_state.selectedFluxes = { "All In" };
    static const char* const kElites[] = { "Soul Twisting", "Lyssa's Aura", "Searing Flames", "Signet of Spirits", "Energy Surge" };
    int added = 0;
    for (const char* name : kElites)
        for (const SkillOption& o : s_skillOptions)
            if (added < 3 && o.name == name)
            {
                s_state.selectedSkills.insert(o.id);
                added++;
                break;
            }
}

// DEV TOOL (GWO_LIBRARY_SHOT_STATS, MapBrowser.cpp): drives the Statistics page for its pictures.
// "open" (the selected match's page), "ready" (true once its analysis is read), "select:k1,k2,..."
// (the selection, in that order), "active:<key>", "agent:<name>", "picker:<category>" ("-" closes
// it), "search:<text>", "table:1|0", "clear", "undo", "range:<from s>,<to s>", "full",
// "timeline:1|0", "perminute:1|0". The test script restores stats_selection.txt and
// stats_favourites.txt afterwards.
bool LibraryStatsTest(const std::string& cmd)
{
    using namespace libpage;
    const size_t colon = cmd.find(':');
    const std::string verb = cmd.substr(0, colon), arg = colon == std::string::npos ? std::string() : cmd.substr(colon + 1);
    if (verb == "open")
    {
        s_state.detailTab = DetailTab_Stats;
        s_state.detailMaximised = true;
        return true;
    }
    if (verb == "lord")
    {
        s_openLordView = true;
        s_state.detailTab = DetailTab_Stats;
        s_state.detailMaximised = true;
        return true;
    }
    if (verb == "ready")
        return s_analysis.state == MatchAnalysisJob::State::Ready && !s_analysis.ledger.players.empty();
    if (verb == "select")
    {
        StatsTable::ClearSelection();
        size_t i = 0;
        while (i <= arg.size())
        {
            size_t j = arg.find(',', i);
            if (j == std::string::npos) j = arg.size();
            if (j > i) StatsTable::Select(arg.substr(i, j - i), true);
            i = j + 1;
        }
        NormalizeActive();
        return true;
    }
    if (verb == "active") { ActivateIndicator(arg); return true; }
    if (verb == "agent")
    {
        s_sp.agent = -1;   // none named: the ranking's first
        for (const auto& [id, ag] : s_analysis.ledger.agents)
            if (ag.name == arg) s_sp.agent = id;
        return true;
    }
    if (verb == "picker") { s_sp.pickerOpen = arg != "-"; if (s_sp.pickerOpen) s_sp.category = arg; return true; }
    if (verb == "search") { snprintf(s_sp.search, sizeof s_sp.search, "%s", arg.c_str()); return true; }
    if (verb == "table") { s_sp.table = arg == "1"; if (s_sp.table) s_sp.focusColumn = s_sp.active; return true; }
    if (verb == "clear")
    {
        const auto sel = StatsTable::Selection();
        s_sp.undo = StatsTable::TakeSnapshot();
        s_sp.undoActive = s_sp.active;
        s_sp.cleared = (int)sel.size();
        s_sp.undoReady = !sel.empty();
        StatsTable::ClearSelection();
        s_sp.active.clear();
        return true;
    }
    if (verb == "undo")
    {
        if (!s_sp.undoReady) return false;
        StatsTable::Restore(s_sp.undo);
        s_sp.active = s_sp.undoActive;
        s_sp.undoReady = false;
        NormalizeActive();
        return true;
    }
    if (verb == "range")
    {
        float a = 0.0f, b = 0.0f;
        if (sscanf_s(arg.c_str(), "%f,%f", &a, &b) != 2 || b <= a) return false;
        const int pre = s_sp.lastPre;
        const int span = std::max(1, s_sp.lastSpan);
        s_state.scope = Scope_Range;
        s_state.detailIntervalFrom = std::clamp((a + pre) / span, 0.0f, 1.0f);
        s_state.detailIntervalTo = std::clamp((b + pre) / span, 0.0f, 1.0f);
        return true;
    }
    if (verb == "full") { s_state.scope = Scope_Full; s_state.detailIntervalFrom = 0.0f; s_state.detailIntervalTo = 1.0f; return true; }
    if (verb == "timeline") { s_sp.timeOpen = arg == "1"; return true; }
    if (verb == "tug") { s_lv.tug = arg == "1"; return true; }
    if (verb == "possible") { s_lv.possible = arg == "1"; return true; }
    if (verb == "scroll") { s_lv.scrollReq = (float)std::atof(arg.c_str()); return true; }
    if (verb == "perminute") { StatsTable::SetPerMinute(arg == "1"); return true; }
    return false;
}
