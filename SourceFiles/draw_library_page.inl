// =================================================================================================
// THE LIBRARY PAGE  [2026-11-22]
//
// The Library's main page as the owner's mockup draws it (output/imagegen/library-deep-blue-v11.html,
// its final charcoal-and-gold sheet): a title row with Update matches and Spoiler-free, the search
// with the All matches / Favorites switch, a row of filter menus, the match list with each team's
// eight elite skills, and a sidebar with the selected match's facts, Replay match and both teams'
// builds. Stats and Map open the existing full-width details (DrawMatchDetailPanel), which do not
// fit a 450 px sidebar.
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

namespace libpage
{
// ─── Scale, type and colour ──────────────────────────────────────────────────

// The page's scale: the Settings font size over 15, times the page size, which follows the window
// (GuiGlobalConstants::LibraryPageScale): 1 up to 1920 x 1080, 1.35 at 2560 x 1440, 2 at 4K.
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
constexpr ImU32 SearchBg      = Hex(0x141413);
constexpr ImU32 SearchRule    = Hex(0x484337);
constexpr ImU32 SearchText    = Hex(0xa39c8c);
constexpr ImU32 ButtonBg      = Hex(0x25231e);
constexpr ImU32 ButtonHovBg   = Hex(0x2e2b24);
constexpr ImU32 ButtonText    = Hex(0xd6d1c5);
constexpr ImU32 ButtonActive  = Hex(0xcfb98b);
constexpr ImU32 ActionBg      = Hex(0x22211d);
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

// ─── Match data the page shows ───────────────────────────────────────────────

// Each team's elite skills, one slot per player in player order: what the list shows of a build.
struct EliteSlot
{
    int skill = 0;   // 0: the player took no elite
    int prof = 0;
    std::string player;
};
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
    {
        auto pit = m.parties.find(side == 0 ? "1" : "2");
        if (pit == m.parties.end()) continue;
        std::vector<const PlayerMeta*> players;
        for (const auto& p : pit->second.players) players.push_back(&p);
        std::sort(players.begin(), players.end(),
                  [](const PlayerMeta* a, const PlayerMeta* b) { return a->player_number < b->player_number; });
        for (const PlayerMeta* p : players)
        {
            EliteSlot slot;
            slot.prof = p->primary;
            slot.player = p->encoded_name.empty() ? "(unnamed)" : SanitizePlayerName(p->encoded_name);
            for (int id : p->used_skills)
            {
                const SkillInfo* si = view.Get(id);
                if (si && si->is_elite) { slot.skill = id; break; }
            }
            it->second.team[side].push_back(std::move(slot));
        }
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
    dl->AddRectFilled(pos, b, on ? col::ToggleOnBg : hovered ? col::ActionHovBg : col::ActionBg, Px(3));
    Outline(dl, pos, b, on ? col::ToggleOnRule : hovered ? col::ActionHovRule : col::ActionRule, Px(3));
    return clicked;
}

// ─── Search and the All matches / Favorites switch ───────────────────────────

static void DrawSearch(ImDrawList* dl, float x, float y, float w, float h)
{
    const ImVec2 a(x, y), b(x + w, y + h);
    dl->AddRectFilled(a, b, col::SearchBg, Px(3));
    HRule(dl, a.x + Px(2), b.x - Px(2), b.y - 1.0f, col::SearchRule);

    const Type t = T(11);
    const float textTop = R(y + (h - LineH(11)) * 0.5f);
    float cx = x + Px(16);
    DrawMagnifier(dl, ImVec2(cx + Px(5), textTop + LineH(11) * 0.5f), Px(10), col::SearchText);
    cx += Px(11) + TextW(t, "  ");

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
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(col::SearchText));
    ImGui::SetNextItemWidth(std::max(Px(40), b.x - Px(16) - cx));
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
    dl->AddRectFilled(pos, b, hov ? col::ButtonHovBg : col::ButtonBg, Px(3));
    if (active) HRule(dl, pos.x + Px(2), b.x - Px(2), b.y - 1.0f, col::ButtonActive);
    const Type t = T(11);
    float tx = pos.x + Px(13);
    const float ty = R(pos.y + (size.y - LineH(11)) * 0.5f);
    if (star)
    {
        DrawStar(dl, ImVec2(tx + Px(5.5f), ty + LineH(11) * 0.5f + Px(0.5f)), Px(5.0f), active ? col::StarOn : col::ButtonText,
                 active, 1.15f);
        tx += Px(10) + TextW(t, " ");
    }
    Text(dl, t, ImVec2(tx, ty), col::ButtonText, label);
    return clicked;
}

// ─── The toolbar: search, All matches / Favorites, Update matches, Spoiler-free ──

static bool LinkText(ImDrawList* dl, const char* id, const Type& t, ImVec2 pos, float maxW, ImU32 c, const char* s);   // below

// One row where the mockup had two: the "Match library" title row is gone (the count is in the
// footer), and its two actions sit at the end of the search row. While the search would be narrower
// than 260 px, the two actions keep only their pictures.
static float DrawToolbar(ImDrawList* dl, float x0, float x1, float y)
{
    const float h = R(Px(8) * 2.0f + LineH(11) + 1.0f);
    const Type tb = T(11), ab = T(11), st = T(9);
    SpoilerFree& sf = SpoilerFree::Get();
    const bool spoilers = sf.Enabled();
    const char* state = spoilers ? "On" : "Off";
    const bool waiting = g_refreshHint;

    const float favW = Px(13) * 2.0f + Px(10) + TextW(tb, " ") + TextW(tb, "Favorites");
    const float allW = Px(13) * 2.0f + TextW(tb, "All matches");
    float updW = 2.0f + Px(12) + Px(20) + Px(8) + TextW(ab, "Update matches") + (waiting ? Px(13) : 0.0f) + Px(12);
    float spW = 2.0f + Px(12) + Px(16) + Px(8) + TextW(ab, "Spoiler") + Px(8) + TextW(st, state) + Px(12);
    const float gap = Px(12), groupGap = Px(20);
    const bool compact = (x1 - x0) - (allW + favW + updW + spW + gap * 3.0f + groupGap) < Px(260);
    if (compact)
    {
        updW = 2.0f + Px(12) * 2.0f + Px(20) + (waiting ? Px(10) : 0.0f);
        spW = 2.0f + Px(12) * 2.0f + Px(16) + Px(6) + TextW(st, state);
    }
    const ImVec2 spPos(R(x1 - spW), y);
    const ImVec2 upPos(R(spPos.x - gap - updW), y);
    const ImVec2 favPos(R(upPos.x - groupGap - favW), y);
    const ImVec2 allPos(R(favPos.x - gap - allW), y);
    DrawSearch(dl, x0, y, allPos.x - gap - x0, h);

    if (ToolbarButton(dl, "##lib_all", allPos, ImVec2(allW, h), "All matches", !s_page.favouritesOnly, false))
        s_page.favouritesOnly = false;
    if (ToolbarButton(dl, "##lib_fav", favPos, ImVec2(favW, h), "Favorites", s_page.favouritesOnly, true))
        s_page.favouritesOnly = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The matches you starred (%d). Click a row's star to add one.", (int)MatchFavourites::Get().Count());

    // Update matches: the lit refresh badge, the label, and a dot while new matches wait.
    bool hov = false;
    if (ActionButton("##lib_update", upPos, ImVec2(updW, h), false, hov))
    {
        g_refreshMatchIndex = true;
        g_refreshHint = false;
    }
    {
        float cx = upPos.x + 1.0f + Px(12);
        EnsureTextureBasePath();
        if (ImTextureID tex = GetTextureCache().GetTexture(ui::TextureBasePath() + "\\Textures\\DDS\\ATEXDXT5\\texture_337393.dds"))
            dl->AddImage(tex, ImVec2(cx, R(y + (h - Px(20)) * 0.5f)), ImVec2(cx + Px(20), R(y + (h - Px(20)) * 0.5f) + Px(20)),
                         ImVec2(kRefreshLitX0 / kRefreshAtlasW, kRefreshSrcY0 / kRefreshAtlasH),
                         ImVec2(kRefreshLitX1 / kRefreshAtlasW, kRefreshSrcY1 / kRefreshAtlasH));
        cx += Px(20);
        if (!compact)
        {
            cx += Px(8);
            Text(dl, ab, ImVec2(cx, y + (h - LineH(11)) * 0.5f), col::ActionText, "Update matches");
            cx += TextW(ab, "Update matches");
        }
        if (waiting)
            dl->AddCircleFilled(ImVec2(cx + Px(6) + Px(2.5f), y + h * 0.5f), Px(2.5f), col::NewDot, 12);
    }
    if (hov)
        ImGui::SetTooltip(waiting ? "New matches are waiting: refresh the list" : "Update matches: re-fetch the match list from the cloud");

    // Spoiler-free: eye, label, Off / On.
    if (ActionButton("##lib_spoiler", spPos, ImVec2(spW, h), spoilers, hov))
        sf.SetEnabled(!spoilers);
    {
        const ImU32 fg = spoilers ? col::ToggleOn : col::ActionText;
        float cx = spPos.x + 1.0f + Px(12);
        DrawEyeGlyph(dl, ImVec2(cx + Px(8), y + h * 0.5f), Px(14), fg, spoilers);
        cx += Px(16);
        if (!compact)
        {
            cx += Px(8);
            Text(dl, ab, ImVec2(cx, y + (h - LineH(11)) * 0.5f), fg, "Spoiler");
            cx += TextW(ab, "Spoiler") + Px(8);
        }
        else
            cx += Px(6);
        Text(dl, st, ImVec2(cx, y + (h - LineH(9)) * 0.5f + Px(1)), spoilers ? col::ToggleOn : col::ToggleState, state);
    }
    if (hov)
        ImGui::SetTooltip(spoilers ? "Spoiler-free is on: winners, match lengths and match stats stay hidden\n"
                                     "until you watch a match to the end or reveal it."
                                   : "Spoiler-free: hide winners, match lengths and match stats\n"
                                     "until you watch a match to the end.");
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

static int FluxMonth(const std::string& name)
{
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
    if (focusOnOpen && ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
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

static void MenuMap(const std::vector<MatchMeta>& matches)
{
    if (MenuHeader("MAP", !s_state.selectedMaps.empty())) s_state.selectedMaps.clear();
    const OptionCounts& c = CountsFor(CountCat::Map, matches);
    for (size_t i = 1; i < s_state.mapNames.size(); i++)
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

static void MenuFlux(const std::vector<MatchMeta>& matches)
{
    if (MenuHeader("FLUX", !s_state.selectedFluxes.empty())) s_state.selectedFluxes.clear();
    const OptionCounts& c = CountsFor(CountCat::Flux, matches);
    // In calendar order, January first; a flux with no month last.
    std::vector<size_t> order;
    for (size_t i = 1; i < s_state.fluxNames.size(); i++) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [](size_t a, size_t b) {
        const int ma = FluxMonth(s_state.fluxNames[a]), mb = FluxMonth(s_state.fluxNames[b]);
        return (ma > 0 ? ma : 13) < (mb > 0 ? mb : 13);
    });
    for (const size_t i : order)
    {
        const std::string& name = s_state.fluxNames[i];
        const bool on = s_state.selectedFluxes.count(name) > 0;
        const std::string label = FluxLabel(name);
        ImGui::PushID((int)i);
        if (CheckRow("##flux", on ? 1 : 0, label.c_str(), CountOf(c, name), false, nullptr, nullptr, false, 0.0f))
        {
            if (on) s_state.selectedFluxes.erase(name);
            else    s_state.selectedFluxes.insert(name);
        }
        if (ImGui::IsItemHovered())
            if (const char* desc = GetFluxDescription(name))
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(320.0f);
                ImGui::TextUnformatted(desc);
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
static void DrawFilterMenuWindow(const std::vector<MatchMeta>& matches)
{
    if (s_page.openMenu.empty()) return;
    const std::string id = s_page.openMenu;
    const ImGuiIO& io = ImGui::GetIO();
    const float w = id == "skills" ? Px(390) : Px(330);
    const float padX = Px(14), padY = Px(12);
    s_menuW = w - padX * 2.0f;
    ImVec2 pos = s_page.menuAnchor;
    pos.x = std::max(Px(8), std::min(pos.x, io.DisplaySize.x - w - Px(8)));
    const float maxH = std::max(Px(220), io.DisplaySize.y - pos.y - Px(16));
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, maxH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(col::Menu));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(col::ActionRule));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(Hex(0x3b3830)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(5));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Px(8));
    bool hovered = false;
    if (ImGui::Begin("##lib_filter_menu", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
    {
        if (id == "team")          MenuTeam();
        else if (id == "map")      MenuMap(matches);
        else if (id == "occasion") MenuOccasion(matches);
        else if (id == "skills")   MenuSkills(matches);
        else if (id == "flux")     MenuFlux(matches);
        else if (id == "date")     MenuDate();
        else if (id == "more")     MenuMore(matches);
        hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                         ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(4);

    const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered && !popupOpen && !s_page.menuLabelHovered)
        s_page.openMenu.clear();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !popupOpen)
        s_page.openMenu.clear();
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
static float DrawFilterRow(ImDrawList* dl, float x0, float x1, float y)
{
    const Type t = T(11);
    const float lineH = R(LineH(11) + Px(10));
    s_page.menuLabelHovered = false;

    // The sort, at the right: the current order and its arrow; a click lists the others.
    int current = -1;
    for (int i = 0; i < IM_ARRAYSIZE(kSortOptions); i++)
        if (kSortOptions[i].column == s_state.sortColumn && kSortOptions[i].ascending == s_state.sortAscending) current = i;
    const char* sortLabel = current >= 0 ? kSortOptions[current].label : "Sorted";
    const bool down = current < 0 ? !s_state.sortAscending : !kSortOptions[current].ascending;
    const float sortW = TextW(t, sortLabel) + TextW(t, " ") + Px(9);
    const ImVec2 sortPos(R(x1 - sortW), R(y + (lineH - LineH(11)) * 0.5f));
    ImGui::SetCursorScreenPos(sortPos);
    const bool sortClicked = ImGui::InvisibleButton("##lib_sort_btn", ImVec2(sortW, LineH(11)));
    const bool sortHov = ImGui::IsItemHovered();
    const ImU32 sc = sortHov ? col::LinkHover : col::Sort;
    Text(dl, t, sortPos, sc, sortLabel);
    DrawArrowVertical(dl, ImVec2(sortPos.x + sortW - Px(4.5f), sortPos.y + LineH(11) * 0.52f), Px(10), sc, down);
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
        const float w = active ? FilterChipWidth(f, value) : TextW(t, f.label) + TextW(t, " ") + Px(9);
        if (x > x0 && x + w > limit)
        {
            x = x0;
            ly += lineH + Px(8);
            limit = x1;   // later lines have the whole width
        }
        if (active)
        {
            DrawFilterChip(dl, f, value, ImVec2(R(x), R(ly)), R(w), lineH);
            x += R(w) + Px(10);
            continue;
        }
        const float ty = R(ly + (lineH - LineH(11)) * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(x, ly));
        ImGui::PushID(f.id);
        const bool clicked = ImGui::InvisibleButton("##menu", ImVec2(w, lineH));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        s_page.menuLabelHovered |= hov;
        const bool open = s_page.openMenu == f.id;
        const ImU32 c = open ? col::FilterOn : hov ? col::Text : col::Filters;
        Text(dl, t, ImVec2(x, ty), c, f.label);
        DrawChevronDown(dl, ImVec2(x + TextW(t, f.label) + TextW(t, " ") + Px(4), ty + LineH(11) * 0.56f), Px(6), c);
        if (clicked) s_page.openMenu = open ? std::string() : std::string(f.id);
        if (s_page.openMenu == f.id) s_page.menuAnchor = ImVec2(x - Px(12), ly + lineH + Px(4));
        x += w + Px(28);
    }
    return ly + lineH - y;
}

// ─── Match list ──────────────────────────────────────────────────────────────

struct ListGeom
{
    float w = 0.0f;                    // the rows' width
    bool thumb = true;                 // the thumbnail column, dropped first when the list narrows
    float icon = 0.0f, iconGap = 0.0f; // the elite lineup's icons
    float starX = 0, thumbX = 0, team1X = 0, vsX = 0, team2X = 0, mapX = 0, durX = 0;
    float teamW = 0.0f, mapW = 0.0f;
};

// The mockup's grid: 18px 66px 1fr 20px 1fr 130px 40px, 10 px gaps, 12 px padding. The map column
// grows to 210 px on a wide list, where "mAT Quarterfinals" no longer fits in 130.
static ListGeom ComputeListGeom(float w)
{
    ListGeom g;
    g.w = w;
    const float gap = Px(10), pad = Px(12);
    const float star = Px(18), thumb = Px(66), vs = Px(20), dur = Px(40);
    const float map = R(std::clamp(w * 0.13f, Px(130), Px(210)));
    g.mapW = map;
    const float inner = w - pad * 2.0f;
    float fixed = star + vs + map + dur + gap * 5.0f + thumb + gap;
    float teamW = (inner - fixed) * 0.5f;
    if (teamW < Px(212))
    {
        g.thumb = false;
        fixed -= thumb + gap;
        teamW = (inner - fixed) * 0.5f;
    }
    g.icon = Px(23);
    g.iconGap = Px(4);
    if (teamW < g.icon * 8.0f + g.iconGap * 7.0f)
    {
        g.iconGap = Px(2);
        g.icon = std::max(Px(12), std::floor((teamW - g.iconGap * 7.0f) / 8.0f));
    }
    float x = pad;
    g.starX = x;  x += star + gap;
    if (g.thumb) { g.thumbX = x; x += thumb + gap; }
    g.team1X = x; x += teamW + gap;
    g.vsX = x;    x += vs + gap;
    g.team2X = x; x += teamW + gap;
    g.mapX = x;   x += map + gap;
    g.durX = x;
    g.teamW = teamW;
    return g;
}

// The column labels; MAP · OCCASION over its column. The "N new matches added" pill sits after
// the first label, since the title row it was beside is gone.
static void DrawLabelsRow(ImDrawList* dl, float x0, float x1, float y, const ListGeom& g)
{
    const Type t = T(10);
    const float tracking = Px(1.1f);
    const char* left = "MATCHUP / ELITE SKILLS";
    const ImVec2 leftPos(x0 + Px(20), y + Px(10));
    const float leftW = TextSpaced(dl, t, leftPos, col::Labels, left, tracking);
    ImGui::SetCursorScreenPos(leftPos);
    ImGui::Dummy(ImVec2(leftW, LineH(10)));
    DrawNewMatchesPill();
    const char* right = "DURATION";
    const float wr = TextSpaced(nullptr, t, ImVec2(), 0, right, tracking);
    TextSpaced(dl, t, ImVec2(x1 - Px(20) - wr, y + Px(10)), col::Labels, right, tracking);
    TextSpaced(dl, t, ImVec2(x0 + g.mapX, y + Px(10)), col::Labels, "MAP \xC2\xB7 OCCASION", tracking);
    HRule(dl, x0, x1, y + Px(10) * 2.0f + LineH(10), col::LabelsRule);
}

// One team's cell: the name (gold behind a cup when it won), its tag, the comms headset on the
// first team, and the eight elite skills.
static void DrawTeamCell(ImDrawList* dl, const ListGeom& g, float x, float cy, const MatchMeta& m,
                         const GuildLabel& gl, bool winner, bool comms, const std::vector<EliteSlot>& slots,
                         bool hoverable)
{
    const Type name = T(12, true), tag = T(11);
    const float nameH = LineH(12);
    const float top = R(cy - (nameH + Px(5) + g.icon) * 0.5f);
    float tx = x, maxW = g.teamW;
    if (winner)
    {
        const float box = Px(22);
        const ImVec2 b0(x, R(top + nameH * 0.5f - box * 0.5f)), b1(b0.x + box, b0.y + box);
        dl->AddRectFilled(b0, b1, col::CupBg, Px(3));
        if (ImTextureID cup = GetCupIcon())
            ImageContain(dl, cup, 64.0f, 109.0f, ImVec2(b0.x + Px(2), b0.y + Px(2)), ImVec2(b1.x - Px(2), b1.y - Px(2)));
        tx += Px(27);
        maxW -= Px(27);
    }
    const std::string& nm = gl.name.empty() ? gl.display : gl.name;
    const float nameW = TextW(name, nm.c_str());
    const float drawn = TextFit(dl, name, ImVec2(tx, top), maxW, winner ? col::Winner : col::Text, nm.c_str());
    float after = tx + drawn;
    if (drawn >= nameW && !gl.name.empty() && !gl.tag.empty())
    {
        const std::string tg = "[" + gl.tag + "]";
        const float sp = TextW(name, " ");
        if (after + sp < tx + maxW)
            after += sp + TextFit(dl, tag, ImVec2(after + sp, top + Ascent(12) - Ascent(11)), tx + maxW - after - sp,
                                  col::Small, tg.c_str());
    }
    if (comms && after + Px(8) + Px(15) <= tx + maxW)
    {
        const Comms::Badge badge = Comms::BadgeFor(m.folder_name);
        const ImVec2 bp(after + Px(8), R(top + (nameH - Px(15)) * 0.5f));
        DrawHeadsetGlyph(dl, bp, Px(15), badge.exists ? col::Comms : Hex(0x828288));
        if (hoverable && ImGui::IsMouseHoveringRect(bp, ImVec2(bp.x + Px(15), bp.y + Px(15))))
            ImGui::SetTooltip(badge.exists ? "Voice comms attached: %s" : "Voice comms file missing: %s", badge.name.c_str());
    }

    // The lineup.
    const SkillDatabaseView* view = slots.empty() ? nullptr : &GetMatchSkillView(m);
    float ix = x;
    const float iy = top + nameH + Px(5);
    for (int i = 0; i < 8; i++)
    {
        const ImVec2 a(R(ix), R(iy)), b(R(ix) + g.icon, R(iy) + g.icon);
        const EliteSlot* slot = i < (int)slots.size() ? &slots[i] : nullptr;
        bool drewIcon = false;
        if (slot && slot->skill)
            if (ImTextureID tex = GetSkillIcon(slot->skill, view))
            {
                dl->AddImage(tex, a, b);
                drewIcon = true;
            }
        if (!drewIcon)
        {
            dl->AddRectFilled(a, b, col::EmptySlot);
            if (slot && slot->prof)
                if (ImTextureID prof = GetProfessionIcon(slot->prof))
                {
                    const float inset = g.icon * 0.2f;
                    dl->AddImage(prof, ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset),
                                 ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 110));
                }
        }
        Outline(dl, a, b, col::IconRule, 0.0f);
        if (hoverable && slot && ImGui::IsMouseHoveringRect(a, b))
        {
            if (slot->skill)
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

// ─── Sidebar ─────────────────────────────────────────────────────────────────

// A small outlined button on the sidebar (Details, Reveal this match): 10 px type.
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
    dl->AddRectFilled(pos, b, hov ? col::ActionHovBg : col::ActionBg, Px(3));
    Outline(dl, pos, b, hov ? col::ActionHovRule : col::ActionRule, Px(3));
    Text(dl, t, ImVec2(pos.x + 1.0f + padX, pos.y + 1.0f + padY), col::ActionText, label);
    if (chevron)
        DrawChevronDown(dl, ImVec2(b.x - 1.0f - padX - Px(3.5f), pos.y + h * 0.53f), Px(5.5f), col::ActionText);
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
    const ImU32 ca = act ? col::ReplayB : hov ? col::ReplayHovA : col::ReplayA;
    const ImU32 cb = act ? col::ReplayActB : hov ? col::ReplayHovB : col::ReplayB;
    dl->AddRectFilledMultiColor(pos, b, ca, cb, cb, ca);
    if (busy) dl->AddRectFilled(pos, b, Hex(0x13120f, 120));

    const Type t = T(15, true);
    const float tracking = Px(0.5f);
    const float labelW = TextSpaced(nullptr, t, ImVec2(), 0, "REPLAY MATCH", tracking);
    const float triW = Px(11), gap = TextW(t, "  ") + Px(6);
    const float x0 = R(pos.x + (size.x - (triW + gap + labelW)) * 0.5f);
    const float push = act ? 1.0f : 0.0f;
    DrawPlay(dl, ImVec2(x0 + triW * 0.5f, pos.y + size.y * 0.5f + push), Px(12.5f), col::ReplayText);
    TextSpaced(dl, t, ImVec2(x0 + triW + gap, pos.y + Px(11) + push), col::ReplayText, "REPLAY MATCH", tracking);

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

// A team in the Builds tab: its heading (cup and WINNER for the winner), then a line a player:
// profession, name, and the bar. The profession icon and the name show the build card on hover; a
// click on the icon pins it, a click on the name searches for the player, a right-click copies the
// template.
static float DrawBuildsTeam(ImDrawList* dl, const MatchMeta& m, const char* partyId, const GuildLabel& gl,
                            bool winner, float x, float w, float y, const OverviewBuilds& ob, bool compact)
{
    const SkillDatabaseView& view = GetMatchSkillView(m);
    y += Px(17);

    // Heading.
    const Type name = T(13, true), tag = T(13);
    const float headH = winner ? Px(23) : LineH(13);
    float hx = x;
    if (winner)
    {
        const ImVec2 b0(x, R(y)), b1(x + Px(23), R(y) + Px(23));
        dl->AddRectFilled(b0, b1, col::CupBg, Px(3));
        if (ImTextureID cup = GetCupIcon())
            ImageContain(dl, cup, 64.0f, 109.0f, ImVec2(b0.x + Px(2), b0.y + Px(2)), ImVec2(b1.x - Px(2), b1.y - Px(2)));
        hx += Px(23) + Px(8);
    }
    const float textTop = y + (headH - LineH(13)) * 0.5f;
    float winnerW = 0.0f;
    const Type wl = T(9);
    if (winner)
    {
        winnerW = TextSpaced(nullptr, wl, ImVec2(), 0, "WINNER", Px(0.7f));
        TextSpaced(dl, wl, ImVec2(x + w - winnerW, y + (headH - LineH(9)) * 0.5f), col::WinnerLabel, "WINNER", Px(0.7f));
    }
    const std::string nm = gl.name.empty() ? gl.display : gl.name;
    const float maxNameW = x + w - winnerW - Px(8) - hx;
    ImGui::PushID(partyId);
    if (LinkText(dl, "##guild", name, ImVec2(hx, textTop), maxNameW, col::Text, nm.c_str()))
        FilterBySearchTerm(gl.display);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show every match for %s", gl.display.c_str());
    const float nameW = TextFit(nullptr, name, ImVec2(), maxNameW, 0, nm.c_str());
    if (!gl.name.empty() && !gl.tag.empty())
    {
        const float gap = winner ? Px(5) : TextW(name, " ");
        const std::string tg = "[" + gl.tag + "]";
        if (hx + nameW + gap < x + w - winnerW - Px(8))
            TextFit(dl, tag, ImVec2(hx + nameW + gap, textTop), x + w - winnerW - Px(8) - (hx + nameW + gap), col::Tag, tg.c_str());
    }
    y += headH + Px(9);

    // Players.
    auto pit = m.parties.find(partyId);
    if (pit == m.parties.end() || pit->second.players.empty())
    {
        Text(dl, T(11), ImVec2(x, y), col::FactLabel, m.is_cloud_only ? "Builds arrive with the match download" : "No player data");
        ImGui::PopID();
        return y + LineH(11);
    }
    std::vector<const PlayerMeta*> players;
    for (const auto& p : pit->second.players) players.push_back(&p);
    std::sort(players.begin(), players.end(),
              [](const PlayerMeta* a, const PlayerMeta* b) { return a->player_number < b->player_number; });

    const float rowH = compact ? Px(25) : Px(28);
    const float barW = Px(22) * 8.0f + Px(2) * 7.0f;
    const Type pn = T(11);
    for (const PlayerMeta* pp : players)
    {
        const PlayerMeta& p = *pp;
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

        // Name: a search link.
        const float nx = x + Px(23);
        const float nameMaxW = std::max(Px(20), w - Px(23) - Px(7) - barW);
        if (LinkText(dl, "##name", pn, ImVec2(nx, cy - LineH(11) * 0.5f), nameMaxW, col::Player, clean.c_str()))
        {
            if (ImGui::GetIO().KeyCtrl) AddSearchTerm(clean);
            else                        FilterBySearchTerm(clean);
        }
        showCard |= ImGui::IsItemHovered();

        // Right-click on the icon or the name: the templates.
        if (ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(nx + nameMaxW, y + rowH)) &&
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

        // The bar: hover for the skill, click to filter by it.
        ImGui::SetCursorScreenPos(ImVec2(R(x + w - barW), R(cy - Px(11))));
        DrawPlayerSkillRow(p, view, Px(22));

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

    // Single-pane width: the way back to the list, above the hero.
    if (backLink)
    {
        const float bh = Px(40);
        if (LinkText(dl, "##lib_back", T(13), ImVec2(x, y + (bh - LineH(13)) * 0.5f), w, col::Link, "\xE2\x80\xB9  Back to the matches"))
            s_page.showDetail = false;
        y += bh;
    }

    // Hero: the hall's loading screen, fading into the sidebar.
    {
        const float heroH = Px(100);
        const ImVec2 a(o.x, y), b(o.x + fullW, y + heroH);
        if (const ArtTexture* art = LoadArt(HallArtPath(m.map_id)))
            ImageCover(dl, (ImTextureID)art->srv.Get(), art->w, art->h, a, b, IM_COL32_WHITE, 0.0f);
        else
            dl->AddRectFilledMultiColor(a, b, Hex(0x2a2720), Hex(0x2a2720), col::Aside, col::Aside);
        const ImU32 clear = col::Aside & ~IM_COL32_A_MASK;
        dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + heroH * 0.1f), b, clear, clear, col::Aside, col::Aside);
        y = b.y;
    }

    // SELECTED MATCH · 18 NOV 2026
    y += Px(4);
    const std::string meta = "SELECTED MATCH \xC2\xB7 " + MatchDate(m, true);
    TextSpaced(dl, T(10), ImVec2(x, y), col::Muted, meta.c_str(), Px(1));
    y += LineH(10);

    // The hall.
    y += Px(11);
    {
        const float titleH = LineH(23);
        if (ImTextureID ico = GetGuildHallIcon())
            dl->AddImage(ico, ImVec2(x, R(y + (titleH - Px(22)) * 0.5f)), ImVec2(x + Px(22), R(y + (titleH - Px(22)) * 0.5f) + Px(22)));
        const char* mapName = GetMapName(m.map_id);
        const std::string mapStr = mapName ? mapName : ("Map " + std::to_string(m.map_id));
        TextFit(dl, T(23), ImVec2(x + Px(31), y), w - Px(31), col::Text, mapStr.c_str());
        y += titleH + Px(15);
    }

    // The facts: occasion, duration, flux, notes.
    {
        const float colW = (w - Px(24)) * 0.5f;
        const float factH = LineH(10) + Px(3) + LineH(13);
        const Type label = T(10), value = T(13);
        auto icon = [&](float fx, float fy, ImTextureID tex, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1)) {
            if (!tex) return;
            const float iy = R(fy + (factH - Px(20)) * 0.5f);
            dl->AddImage(tex, ImVec2(fx, iy), ImVec2(fx + Px(20), iy + Px(20)), uv0, uv1);
        };
        const float c0 = x, c1 = x + colW + Px(24);
        const float r0 = y, r1 = y + factH + Px(10);
        const float vx0 = c0 + Px(30), vx1 = c1 + Px(30), vw = colW - Px(30);

        icon(c0, r0, GetArenaIcon());
        Text(dl, label, ImVec2(vx0, r0), col::FactLabel, "Occasion");
        TextFit(dl, value, ImVec2(vx0, r0 + LineH(10) + Px(3)), vw, col::FactValue, m.occasion.empty() ? "-" : m.occasion.c_str());

        icon(c1, r0, GetDurationIcon());
        Text(dl, label, ImVec2(vx1, r0), col::FactLabel, "Duration");
        Text(dl, T(11), ImVec2(vx1, r0 + LineH(10) + Px(3) + Px(0.7f)), hidden ? col::HiddenText : col::FactValue,
             hidden ? kHiddenWord : (m.match_duration.empty() ? "-" : m.match_duration.c_str()));

        icon(c0, r1, GetFluxIcon());
        Text(dl, label, ImVec2(vx0, r1), col::FactLabel, "Flux");
        TextFit(dl, value, ImVec2(vx0, r1 + LineH(10) + Px(3)), vw, col::FactValue, m.flux.empty() ? "None" : m.flux.c_str());
        if (!m.flux.empty() && ImGui::IsMouseHoveringRect(ImVec2(c0, r1), ImVec2(c0 + colW, r1 + factH)) && ImGui::IsWindowHovered())
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
        icon(c1, r1, GetNotesScrollIcon(&uv0, &uv1), uv0, uv1);
        Text(dl, label, ImVec2(vx1, r1), col::FactLabel, "Notes");
        const std::string note = MatchNotes::Get().GetNote(folder);
        std::string firstLine = note.substr(0, note.find('\n'));
        const ImVec2 linkPos(vx1, r1 + LineH(10) + Px(3) + (LineH(13) - LineH(12)) * 0.5f);
        if (LinkText(dl, "##lib_note_link", T(12), linkPos, vw, col::Link, note.empty() ? "Add a note" : firstLine.c_str()))
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
        ImGui::SetNextWindowPos(ImVec2(x, r1 + factH + Px(6)));
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

        y = r1 + factH + Px(11);
        HRule(dl, x, x + w, y, col::Rule);
        y += 1.0f;
    }

    // Voice comms attached on this PC.
    const Comms::Badge badge = Comms::BadgeFor(folder);
    if (badge.present)
    {
        y += Px(10);
        const float rowH = LineH(12) + Px(3) + LineH(10);
        DrawHeadsetGlyph(dl, ImVec2(x, R(y + (rowH - Px(20)) * 0.5f)), Px(20), badge.exists ? col::VoiceIcon : Hex(0x828288));
        Text(dl, T(12), ImVec2(x + Px(29), y), col::VoiceTitle, "Voice comms attached");
        Text(dl, T(10), ImVec2(x + Px(29), y + LineH(12) + Px(3)), col::VoiceSub,
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

    // Spoiler-free: the result is hidden until revealed.
    if (hidden)
    {
        y += Px(10);
        HRule(dl, x, x + w, y, col::Rule);
        y += 1.0f + Px(9);
        const float bh = 2.0f + Px(8) + LineH(10);
        Text(dl, T(11), ImVec2(x, y + (bh - LineH(11)) * 0.5f), col::Notice, "Result hidden");
        const float bw = SmallButtonWidth("Reveal this match", Px(8), false);
        if (SmallButton(dl, "##lib_reveal", ImVec2(R(x + w - bw), R(y)), "Reveal this match", Px(8), Px(4), false))
            SpoilerFree::Get().Reveal(folder);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show the winner, the length and the stats of this match.\n"
                              "Watching the replay to the end reveals it too.");
        y += bh;
    }

    // Replay match.
    y += Px(11);
    const float replayH = Px(11) * 2.0f + LineH(15);
    DrawReplayButton(dl, m, ImVec2(x, R(y)), ImVec2(w, R(replayH)));
    y += replayH + Px(12);

    // Tabs: Builds here; Stats and Map open the full-width details.
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
            const ImU32 c = active ? col::TabOn : locked ? WithAlpha(col::Tabs, 0.45f) : hov ? col::Text : col::Tabs;
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
        y += LineH(15) + Px(10);
        HRule(dl, x, x + w, y, col::Rule);
        y += 1.0f;
    }

    // Builds.
    UpdateMatchAnalysis(m, false);
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
    std::string ft1, ft2;
    ParseFolderTags(m.folder_name, ft1, ft2);
    const GuildLabel g1 = GetPartyGuild(m, "1", ft1), g2 = GetPartyGuild(m, "2", ft2);
    const int winnerSide = ShownWinner(m);
    y = DrawBuildsTeam(dl, m, "1", g1, winnerSide == 1, x, w, y, ob, hidden);
    y = DrawBuildsTeam(dl, m, "2", g2, winnerSide == 2, x, w, y, ob, hidden);

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

    // Rating and Share.
    y += Px(15);
    HRule(dl, x, x + w, y, col::Rule);
    y += 1.0f + Px(13);
    {
        const float lh = LineH(15);
        const Type rl = T(11);
        Text(dl, rl, ImVec2(x, y + (lh - LineH(11)) * 0.5f), col::FactLabel, "Rating");
        DrawRatingStars(dl, ImVec2(x + TextW(rl, "Rating") + Px(10), y), lh, folder);
        DrawShareLink(dl, m, x + w, y, lh);
        y += lh;
    }
    y += pad;

    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(fullW, y - o.y));
}

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
        s_page.openMenu = g_libraryPageTestMenu == "-" ? std::string() : g_libraryPageTestMenu;
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

    // Stats and Map: the full-width details, until their Show list.
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

    // Widths: the sidebar narrows with the window; below 1000 px the page is one pane, the list or
    // the selected match.
    const bool single = ws.x < Px(1000);
    if (!single) s_page.showDetail = false;
    const bool detailPane = single && s_page.showDetail && validSel;
    float asideW = ws.x >= Px(1500) ? Px(450) : ws.x >= Px(1250) ? Px(410) : Px(380);
    if (single) asideW = detailPane ? ws.x : 0.0f;
    const float listW = ws.x - asideW;

    if (!detailPane)
    {
        const float x0 = wp.x + Px(28), x1 = wp.x + listW - Px(28);
        float y = R(wp.y + Px(22));

        // Search, All matches / Favorites, Update matches, Spoiler-free.
        y += DrawToolbar(dl, x0, x1, y) + Px(14);

        // The filter buttons (two lines when the active ones need them), the sort.
        y += DrawFilterRow(dl, x0, x1, y) + Px(12);

        const ListGeom g = ComputeListGeom(x1 - x0);
        DrawLabelsRow(dl, x0, x1, y, g);
        y += Px(10) * 2.0f + LineH(10) + 1.0f;

        // The rows: a child as wide as the list plus a gutter for its scrollbar, in the page's
        // right padding, so the rows keep the mockup's width whether it shows or not.
        const float rowH = Px(59);
        const float footH = Px(16) + LineH(11) + Px(14);
        const float listTop = R(y);
        const float room = std::max(rowH, wp.y + ws.y - footH - listTop);
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

                    if (selected)
                    {
                        ldl->AddRectFilledMultiColor(a, b, col::SelA, col::SelB, col::SelB, col::SelA);
                        ldl->AddRectFilled(a, ImVec2(a.x + Px(2), b.y - 1.0f), col::SelBar);
                    }
                    else if (rowHot)
                        ldl->AddRectFilled(a, b, col::RowHover);
                    if (auto hl = s_state.highlightStartTimes.find(m.folder_path); hl != s_state.highlightStartTimes.end())
                    {
                        const float alpha = std::max(0.0f, 1.0f - ((float)ImGui::GetTime() - hl->second) / 2.0f) * 0.15f;
                        if (alpha > 0.0f) ldl->AddRectFilled(a, b, IM_COL32(212, 160, 32, (int)(alpha * 255)));
                    }
                    HRule(ldl, a.x, b.x, b.y - 1.0f, col::RowRule);

                    // Favourite star.
                    const bool fav = MatchFavourites::Get().IsFavourite(m.folder_name);
                    ImGui::SetCursorScreenPos(ImVec2(a.x + g.starX, cy - Px(11)));
                    const bool starClicked = ImGui::InvisibleButton("##fav", ImVec2(Px(18), Px(22)));
                    const bool starHov = ImGui::IsItemHovered();
                    const ImU32 starCol = starHov ? col::LinkHover : (fav || selected) ? col::StarOn : col::Star;
                    DrawStar(ldl, ImVec2(a.x + g.starX + Px(7.5f), cy + Px(0.5f)), Px(7.2f), starCol, fav, 1.15f);
                    if (starClicked) MatchFavourites::Get().Toggle(m.folder_name);
                    if (starHov) ImGui::SetTooltip(fav ? "Remove from favorites" : "Add to favorites");

                    // Thumbnail: the hall's loading screen.
                    if (g.thumb)
                    {
                        const ImVec2 t0(a.x + g.thumbX, R(cy - Px(21))), t1(t0.x + Px(64), t0.y + Px(42));
                        if (const ArtTexture* art = LoadArt(HallArtPath(m.map_id)))
                            ImageCover(ldl, (ImTextureID)art->srv.Get(), art->w, art->h, t0, t1, IM_COL32(255, 255, 255, 242), Px(2));
                        else
                            ldl->AddRectFilled(t0, t1, Hex(0x2a2823), Px(2));
                    }

                    // The teams.
                    const EliteLineup& lineup = LineupOf(m);
                    const int winner = ShownWinner(m);
                    const bool comms = Comms::BadgeFor(m.folder_name).present;
                    DrawTeamCell(ldl, g, a.x + g.team1X, cy, m, fm.guild1, winner == 1, comms, lineup.team[0], rowHot);
                    Text(ldl, T(12), ImVec2(a.x + g.vsX, cy - LineH(12) * 0.5f), col::Versus, "vs");
                    DrawTeamCell(ldl, g, a.x + g.team2X, cy, m, fm.guild2, winner == 2, false, lineup.team[1], rowHot);

                    // Map, date and occasion.
                    {
                        const Type t = T(11);
                        const float blockTop = cy - (LineH(11) * 2.0f + Px(5)) * 0.5f;
                        TextFit(ldl, t, ImVec2(a.x + g.mapX, blockTop), g.mapW, col::MapName, fm.mapName.c_str());
                        std::string line = MatchDate(m, false);
                        if (!m.occasion.empty()) line += " \xC2\xB7 " + m.occasion;
                        TextFit(ldl, t, ImVec2(a.x + g.mapX, blockTop + LineH(11) + Px(5)), g.mapW, col::Muted, line.c_str());
                    }

                    // Duration.
                    {
                        const bool hidden = ResultHidden(m);
                        const char* dur = hidden ? kHiddenWord : (m.match_duration.empty() ? "-" : m.match_duration.c_str());
                        Text(ldl, T(11), ImVec2(a.x + g.durX, cy - LineH(11) * 0.5f), hidden ? col::HiddenText : col::Muted, dur);
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
            Text(dl, t, ImVec2(x0, fy), col::Muted, left.c_str());
            const char* legacy = "Legacy layout";
            const float lw = TextW(t, legacy);
            if (LinkText(dl, "##lib_legacy", t, ImVec2(R(x1 - lw), fy), lw + 1.0f, col::Muted, legacy))
            {
                GuiGlobalConstants::replay_library_legacy = 1;
                GuiGlobalConstants::SaveSettings();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Switch to the previous Library layout: the table or the cards, with the filter sidebar.\n"
                                  "Its top bar has a button to come back, and so does Settings > Browser.");
        }
    }

    // The sidebar.
    OverviewBuilds ob;
    bool asideDrawn = false;
    if (asideW > 0.0f)
    {
        const float ax = wp.x + listW;
        if (!detailPane) dl->AddRectFilled(ImVec2(ax, wp.y), ImVec2(ax + 1.0f, wp.y + ws.y), col::AsideRule);
        ImGui::SetCursorScreenPos(ImVec2(detailPane ? ax : ax + 1.0f, wp.y));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(col::Aside));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImGui::ColorConvertU32ToFloat4(Hex(0x3b3830)));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Px(8));
        ImGui::BeginChild("##lib_aside", ImVec2(detailPane ? asideW : asideW - 1.0f, ws.y), ImGuiChildFlags_None,
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
            Text(ImGui::GetWindowDrawList(), t, ImVec2(o.x + (asideW - TextW(t, msg)) * 0.5f, o.y + ws.y * 0.4f), col::Muted, msg);
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
        const int page = std::max(1, (int)((ws.y * 0.6f) / Px(59)));
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

    DrawFilterMenuWindow(matches);
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
