// =================================================================================================
// THE STATISTICS PAGE  [2026-11-26, V48]
//
// The match's Statistics page of the new Library (output/imagegen/statistics-v48-handoff: the
// approved mockup index.html, reference.png, cleared-reference.png and CLAUDE-HANDOFF.txt). The
// page opens from the sidebar's Stats tab, full width over the hall's scene: the match heading
// (back to the library, the hall, both guilds, the facts, Replay match, Builds / Statistics / Map),
// then one workspace with two ways to read the same indicators:
//   Explore  the indicator's two guild totals and every player on one scale at the left, the
//            selected player's breakdown at the right (their bar, the figures behind the number,
//            the skills and the players it came from);
//   Table    every player against every indicator picked.
// The indicators are ONE ordered selection (StatsTable::Selection, kept between sessions and
// shared with the replay's Stats panel): a chip per indicator over both views, a column per
// indicator in the Table. The picker opens in the page's flow above the results.
//
// Every number is the table's own (StatsTable::Values: the ledger, the catalogue's records, the
// deaths and the per-second track over the interval); the breakdowns read the Battle Ledger's
// lines and interrupts and the catalogue's records over the same interval, never full-match
// figures when a part of the match is counted. The interval (Full match, a range, a moment, the
// open replay) and Total / Per minute are the Stats tab's, shared with the Map tab.
//
// Included by draw_library_page.inl inside namespace libpage, so this is the same translation unit
// as the Library page and draw_replay_browser.cpp: it uses their type (T, Px), panels, colours,
// helpers and state directly. Lengths are the mockup's CSS pixels through Px; type through T.
// =================================================================================================

namespace v48
{
constexpr ImU32 Text         = Hex(0xdedbd4);
constexpr ImU32 Muted        = Hex(0xa49d8f);
constexpr ImU32 Gold         = Hex(0xc5aa70);
constexpr ImU32 Crumb        = Hex(0x9d9586);
constexpr ImU32 CrumbLink    = Hex(0xd7c08d);
constexpr ImU32 Versus       = Hex(0x827c72);
constexpr ImU32 Meta         = Hex(0xaaa292);
constexpr ImU32 BlueTitle    = Hex(0xc3e1fa);
constexpr ImU32 RedTitle     = Hex(0xf3c4bd);
constexpr ImU32 TabText      = Hex(0xdedbd4);
constexpr ImU32 TabOff       = Hex(0xa8a294);
constexpr ImU32 TabOn        = Hex(0xd5bc80);
constexpr ImU32 TabRule      = Hex(0xffffff, 0x16);
constexpr ImU32 Rule         = Hex(0xffffff, 0x12);
constexpr ImU32 ToggleBg     = Hex(0x171716, 0x77);
constexpr ImU32 ToggleLine   = Hex(0xffffff, 0x13);
constexpr ImU32 ToggleOnBg   = Hex(0xffffff, 0x0d);
constexpr ImU32 ToggleOnText = Hex(0xe7d6ad);
constexpr ImU32 ToggleText   = Hex(0xbdb6a8);
constexpr ImU32 Hint         = Hex(0xb4ab98);
constexpr ImU32 Scope        = Hex(0xa59d8d);
constexpr ImU32 ScopeHover   = Hex(0xe3cd9d);
constexpr ImU32 ChipBg       = Hex(0x292822, 0xcc);
constexpr ImU32 ChipLine     = Hex(0xffffff, 0x15);
constexpr ImU32 ChipText     = Hex(0xc8c1b3);
constexpr ImU32 ChipOnBg     = Hex(0xbda36a, 0x17);
constexpr ImU32 ChipOnLine   = Hex(0xbda36a, 0x77);
constexpr ImU32 ChipOnText   = Hex(0xe3cd9d);
constexpr ImU32 ChipX        = Hex(0xa9a293);
constexpr ImU32 ChipXLine    = Hex(0xffffff, 0x0a);
constexpr ImU32 Clear        = Hex(0xb6ada0);
constexpr ImU32 ClearHover   = Hex(0xeee6d7);
constexpr ImU32 HoverBg      = Hex(0xffffff, 0x07);
constexpr ImU32 AddText      = Hex(0xe5d1a2);
constexpr ImU32 AddLine      = Hex(0xbba36d, 0x66);
constexpr ImU32 PickerA      = Hex(0x34332d, 0xe8);
constexpr ImU32 PickerB      = Hex(0x252523, 0xf5);
constexpr ImU32 PickerLine   = Hex(0xffffff, 0x1a);
constexpr ImU32 Sub          = Hex(0xaaa292);
constexpr ImU32 SearchBg     = Hex(0x181817, 0xa6);
constexpr ImU32 SearchLine   = Hex(0xffffff, 0x20);
constexpr ImU32 SearchText   = Hex(0xded7c8);
constexpr ImU32 CatCount     = Hex(0x9c9588);
constexpr ImU32 CatOnText    = Hex(0xe3cd9c);
constexpr ImU32 CatOnLine    = Hex(0xbda36a, 0x55);
constexpr ImU32 GroupHead    = Hex(0xc3b08a);
constexpr ImU32 Option       = Hex(0xd0cbc0);
constexpr ImU32 Foot         = Hex(0xa59c8a);
constexpr ImU32 Feedback     = Hex(0xc6bdab);
constexpr ImU32 FeedbackRule = Hex(0xffffff, 0x10);
constexpr ImU32 EmptyBg      = Hex(0xffffff, 0x02);
constexpr ImU32 EmptyLine    = Hex(0xffffff, 0x10);
constexpr ImU32 EmptySub     = Hex(0xa99f8e);
constexpr ImU32 PeriodText   = Hex(0xb9b09e);
constexpr ImU32 PeriodLine   = Hex(0xffffff, 0x16);
constexpr ImU32 Track        = Hex(0xffffff, 0x07);
constexpr ImU32 BlueA        = Hex(0x587b97);
constexpr ImU32 BlueB        = Hex(0x88b8df);
constexpr ImU32 RedA         = Hex(0x905f59);
constexpr ImU32 RedB         = Hex(0xdb938c);
constexpr ImU32 Score        = Hex(0xe7e1d5);
constexpr ImU32 Explain      = Hex(0xc9beaa);
constexpr ImU32 ExplainBold  = Hex(0xe3d3b0);
constexpr ImU32 RankRule     = Hex(0xffffff, 0x16);
constexpr ImU32 RankSmall    = Hex(0x8f897d);
constexpr ImU32 Order        = Hex(0xb5a990);
constexpr ImU32 Labels       = Hex(0x8f897b);
constexpr ImU32 LabelsRule   = Hex(0xffffff, 0x13);
constexpr ImU32 RowRule      = Hex(0xffffff, 0x07);
constexpr ImU32 RowNumber    = Hex(0x827d71);
constexpr ImU32 Name         = Hex(0xd7d2c7);
constexpr ImU32 BlueTag      = Hex(0x96bddc);
constexpr ImU32 RedTag       = Hex(0xdca39c);
constexpr ImU32 Value        = Hex(0xded8ca);
constexpr ImU32 NoValue      = Hex(0x7d776c);
constexpr ImU32 BlueBar      = Hex(0x8db6d6);
constexpr ImU32 RedBar       = Hex(0xd1978f, 0xad);   // its 0.68 opacity
constexpr ImU32 SelA         = Hex(0xbda36a, 0x1c);
constexpr ImU32 SelB         = Hex(0xbda36a, 0x07);
constexpr ImU32 Footer       = Hex(0x938b7a);
constexpr ImU32 InspLine     = Hex(0xffffff, 0x17);
constexpr ImU32 InspLabel    = Hex(0xa39883);
constexpr ImU32 BlueGuild    = Hex(0x9abfdf);
constexpr ImU32 RedGuild     = Hex(0xdca39c);
constexpr ImU32 KpiRule      = Hex(0xffffff, 0x15);
constexpr ImU32 KpiLabel     = Hex(0xaaa396);
constexpr ImU32 KpiValue     = Hex(0xece6d9);
constexpr ImU32 KpiNote      = Hex(0xaaa08d);
constexpr ImU32 Landed       = Hex(0x8fb8d6);
constexpr ImU32 Faked        = Hex(0xb9a577);
constexpr ImU32 Missed       = Hex(0x74716a);
constexpr ImU32 Caption      = Hex(0xaba496);
constexpr ImU32 DetailTitle  = Hex(0xe1dacd);
constexpr ImU32 DetailSub    = Hex(0x9e9789);
constexpr ImU32 DetailRule   = Hex(0xffffff, 0x0c);
constexpr ImU32 Small        = Hex(0x948f83);
constexpr ImU32 TargetValue  = Hex(0xdfd6c4);
constexpr ImU32 MiniTrack    = Hex(0xffffff, 0x09);
constexpr ImU32 NoteText     = Hex(0xb5ad9f);
constexpr ImU32 NoteBg       = Hex(0xbaa16d, 0x08);
constexpr ImU32 NoteLine     = Hex(0xbaa16d);
constexpr ImU32 TableCaption = Hex(0xaaa293);
constexpr ImU32 TableHead    = Hex(0xa29b8d);
constexpr ImU32 TableHeadOn  = Hex(0xe3cd9d);
constexpr ImU32 TableRule    = Hex(0xffffff, 0x1c);
constexpr ImU32 TableCell    = Hex(0xcbc6ba);
constexpr ImU32 TableSel     = Hex(0xbda36a, 0x16);
constexpr ImU32 TableSelLine = Hex(0xc8ac71);
constexpr ImU32 TableBlueTag = Hex(0x9cbed7);
constexpr ImU32 TableRedTag  = Hex(0xd89a92);
constexpr ImU32 TeamRowBg    = Hex(0xffffff, 0x05);
constexpr ImU32 Sticky       = Hex(0x1d1d1b, 0xf2);
}

// ─── State (one per app: the match it belongs to resets the player and the undo) ─────────────

struct StatsPageState
{
    bool table = false;              // Explore (false) or Table
    bool lord = false;               // the Lord damage view (draw_library_lord.inl) over both
    std::string active;              // the Explore's indicator (a selected key)
    int agent = -1;                  // the selected player, as the ledger knows him
    std::string folder;              // the match the player and the undo belong to
    bool pickerOpen = false;
    bool pickerShown = false;        // drawn last frame (its search takes the keys when it opens)
    std::string category;            // the picker's tab: a layout group
    char search[64] = {};
    bool timeOpen = false;           // the timeline strip under the toolbar
    bool undoReady = false;          // Clear all can be undone
    StatsTable::Snapshot undo;
    std::string undoActive;
    int cleared = 0;
    std::string focusColumn;         // Table: bring this column into view (a chip's click)
    int lastSpan = 0, lastPre = 0;   // the interval's clock as last drawn (the dev tool's ranges)
};
static StatsPageState s_sp;

// The active indicator: still selected, else the first one left (or none).
static void NormalizeActive()
{
    const auto sel = StatsTable::Selection();
    if (std::find(sel.begin(), sel.end(), s_sp.active) == sel.end()) s_sp.active = sel.empty() ? std::string() : sel.front();
}

// A chip, a checkbox or the table: the indicator becomes the Explore's and the sort, highest first
// when the sort moves to it (its direction is kept when it already had it).
static void ActivateIndicator(const std::string& key)
{
    s_sp.active = key;
    if (StatsTable::SortKey() != key) StatsTable::SetSort(key, true);
    if (s_sp.table) s_sp.focusColumn = key;
}

static void RemoveIndicator(const std::string& key)
{
    StatsTable::Select(key, false);
    NormalizeActive();
}

static void AddIndicator(const std::string& key)
{
    const bool wasEmpty = StatsTable::Selection().empty();
    StatsTable::Select(key, true);
    if (wasEmpty || s_sp.active.empty()) ActivateIndicator(key);
}

// ─── Small drawn parts ───────────────────────────────────────────────────────────────────────

static void DrawCross(ImDrawList* dl, ImVec2 c, float s, ImU32 col)   // ×
{
    const float h = s * 0.5f, t = std::max(1.0f, Px(1.3f));
    dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, t);
    dl->AddLine(ImVec2(c.x - h, c.y + h), ImVec2(c.x + h, c.y - h), col, t);
}

static void DrawPlus(ImDrawList* dl, ImVec2 c, float s, ImU32 col)   // ＋
{
    const float h = s * 0.5f, t = std::max(1.0f, Px(1.3f));
    dl->AddLine(ImVec2(c.x - h, c.y), ImVec2(c.x + h, c.y), col, t);
    dl->AddLine(ImVec2(c.x, c.y - h), ImVec2(c.x, c.y + h), col, t);
}

static void DrawArrowRight(ImDrawList* dl, ImVec2 c, float w, ImU32 col)   // →
{
    const float t = std::max(1.0f, Px(1.1f)), h = w * 0.3f;
    dl->AddLine(ImVec2(c.x - w * 0.5f, c.y), ImVec2(c.x + w * 0.5f, c.y), col, t);
    dl->AddLine(ImVec2(c.x + w * 0.5f - h, c.y - h), ImVec2(c.x + w * 0.5f, c.y), col, t);
    dl->AddLine(ImVec2(c.x + w * 0.5f - h, c.y + h), ImVec2(c.x + w * 0.5f, c.y), col, t);
}

// A framed box: its fill, a 1 px outline. Returns true on a click; *hov the hover.
static bool BoxButton(ImDrawList* dl, const char* id, ImVec2 p, ImVec2 size, ImU32 fill, ImU32 line, float rounding, bool* hov = nullptr)
{
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, size.x), std::max(1.0f, size.y)));
    const bool h = ImGui::IsItemHovered();
    if (h) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImVec2 b(p.x + size.x, p.y + size.y);
    if (fill) dl->AddRectFilled(p, b, fill, rounding);
    if (line) Outline(dl, p, b, line, rounding);
    if (hov) *hov = h;
    return clicked;
}

// Words to lines of at most maxW, at most maxLines (the last one cut with an ellipsis).
static std::vector<std::string> WrapLines(const Type& t, const std::string& text, float maxW, int maxLines)
{
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < text.size())
    {
        size_t j = text.find(' ', i);
        if (j == std::string::npos) j = text.size();
        const std::string word = text.substr(i, j - i);
        const std::string tryLine = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextW(t, tryLine.c_str()) > maxW)
        {
            lines.push_back(line);
            line = word;
            if ((int)lines.size() == maxLines) { line = lines.back() + " " + word + " ..."; lines.pop_back(); break; }
        }
        else line = tryLine;
        i = j + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// Wrapped text, cut to maxLines; returns the height drawn.
static float TextWrapped(ImDrawList* dl, const Type& t, ImVec2 p, float maxW, float lineH, ImU32 c, const std::string& s, int maxLines = 8)
{
    const auto lines = WrapLines(t, s, maxW, maxLines);
    float y = p.y;
    for (size_t k = 0; k < lines.size(); ++k)
    {
        if ((int)k == maxLines - 1) TextFit(dl, t, ImVec2(p.x, y), maxW, c, lines[k].c_str());
        else Text(dl, t, ImVec2(p.x, y), c, lines[k].c_str());
        y += lineH;
    }
    return y - p.y;
}

static void TextRight(ImDrawList* dl, const Type& t, float xr, float y, ImU32 c, const char* s)
{
    Text(dl, t, ImVec2(R(xr - TextW(t, s)), y), c, s);
}

static std::string ClockText(float seconds)
{
    char b[16];
    FormatClock((int)std::lround(seconds), b, sizeof b);
    return b;
}

static std::string PercentText(double part, double whole)
{
    if (whole <= 0.0) return "\xE2\x80\x94";
    return std::to_string((int)std::lround(100.0 * part / whole)) + "%";
}

static std::string Lower(std::string s)
{
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static std::string Upper(std::string s)
{
    for (char& ch : s) ch = (char)std::toupper((unsigned char)ch);
    return s;
}

static const char* ConditionLabel(int id)
{
    switch (id)
    {
    case 478: return "Bleeding";
    case 479: return "Blind";
    case 480: return "Burning";
    case 481: return "Crippled";
    case 482: return "Deep Wound";
    case 483: return "Disease";
    case 484: return "Poison";
    case 485: return "Dazed";
    case 486: return "Weakness";
    default:  return "Condition";
    }
}

// ─── Who is who ──────────────────────────────────────────────────────────────────────────────

static bool IsLordAgent(const BattleLedger::Data& d, int agent)
{
    auto it = d.agents.find(agent);
    return it != d.agents.end() && (it->second.lord || it->second.name == "Guild Lord");
}

static std::string AgentName(const BattleLedger::Data& d, int agent)
{
    if (BattleLedger::IsMapRow(agent)) return BattleLedger::MapRowName(agent);
    if (agent == BattleLedger::kRowDegenUnknown) return "Degeneration, source unknown";
    auto it = d.agents.find(agent);
    if (it == d.agents.end()) return agent ? "Agent " + std::to_string(agent) : std::string("The map");
    return it->second.name;
}

static int AgentTeam(const BattleLedger::Data& d, int agent)
{
    auto it = d.agents.find(agent);
    return it == d.agents.end() ? 0 : it->second.team;
}

static ImTextureID AgentIcon(const BattleLedger::Data& d, BattleLedger::Host& host, int agent)
{
    if (BattleLedger::IsMapRow(agent)) return BattleLedger::MapRowIcon(host, agent);
    if (IsLordAgent(d, agent)) return host.UiIcon(Team::IsRed(AgentTeam(d, agent)) ? "redguildlord.png" : "blueguildlord.png");
    auto it = d.agents.find(agent);
    return it != d.agents.end() && it->second.prof ? host.ProfIcon(it->second.prof) : nullptr;
}

// "[TAG]" of a side (0 blue, 1 red), from the ledger.
static std::string SideTag(const BattleLedger::Data& d, int side)
{
    const std::string& tag = d.tag[side];
    return "[" + (tag.empty() ? std::string(side == 0 ? "Blue" : "Red") : tag) + "]";
}

// The guild as the ledger names it: "Name [TAG]" already.
static std::string SideGuild(const BattleLedger::Data& d, int side)
{
    const std::string& g = d.guild[side];
    return g.empty() ? (side == 0 ? "Blue team" : "Red team") : g;
}

// The match's player entry for a ledger name (its build).
static const PlayerMeta* PlayerByName(const MatchMeta& m, const std::string& name)
{
    for (const auto& [partyId, party] : m.parties)
        for (const auto& p : party.players)
            if (!p.encoded_name.empty() && SanitizePlayerName(p.encoded_name) == name) return &p;
    return nullptr;
}

// ─── The match heading ───────────────────────────────────────────────────────────────────────

static void LeaveStatsPage(int tab)
{
    s_sp.pickerOpen = false;
    s_state.detailTab = tab;
    if (tab == DetailTab_Overview) s_state.detailMaximised = false;
}

// The breadcrumb, the hall's picture, both guilds, the facts, Replay match, and the page's tabs.
// Returns the y under the tabs' rule.
static float DrawMatchHeading(ImDrawList* dl, const MatchMeta& m, float x0, float x1, float y)
{
    const bool hidden = ResultHidden(m);

    // Breadcrumb: 12 px link, 11 px rest, 14 apart.
    {
        const Type tl = T(12), tc = T(11);
        const float h = LineH(12);
        if (LinkText(dl, "##st_back", tl, ImVec2(x0, y), Px(260), v48::CrumbLink, "\xE2\x80\xB9 Back to match library"))
            LeaveStatsPage(DetailTab_Overview);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The match list, with this match's builds beside it");
        float x = x0 + TextW(tl, "\xE2\x80\xB9 Back to match library") + Px(14);
        Text(dl, tc, ImVec2(x, y + (h - LineH(11)) * 0.5f), v48::Crumb, "/");
        x += TextW(tc, "/") + Px(14);
        Text(dl, tc, ImVec2(x, y + (h - LineH(11)) * 0.5f), v48::Crumb, "Match statistics");
        const Type tr = T(10);
        TextRight(dl, tr, x1, y + (h - LineH(10)) * 0.5f, v48::Crumb, "Recorded analysis from this match");
        y += h;
    }
    y += Px(15);

    // The hall's picture (96 x 57), the title, the facts; Replay match at the right.
    const float ctxY = y;
    {
        const ImVec2 a(x0, R(y)), b(x0 + Px(96), R(y) + Px(57));
        if (const ArtTexture* art = LoadArt(HallArtPath(m.map_id)); art && art->srv)
            ImageCover(dl, (ImTextureID)art->srv.Get(), art->w, art->h, a, b, IM_COL32_WHITE, Px(4));
        else
            dl->AddRectFilled(a, b, Hex(0x2a2823), Px(4));
    }
    const float replayW = Px(170), replayH = Px(45);
    const ImVec2 replayP(R(x1 - replayW), R(ctxY + Px(6)));
    DrawReplayButton(dl, m, replayP, ImVec2(replayW, replayH));

    const float tx = x0 + Px(116), maxX = replayP.x - Px(24);
    {
        std::string ft1, ft2;
        ParseFolderTags(m.folder_name, ft1, ft2);
        const GuildLabel g[2] = { GetPartyGuild(m, "1", ft1), GetPartyGuild(m, "2", ft2) };
        const Type tn = T(23), tt = T(19), tv = T(12);
        const float ty = R(ctxY + Px(3));
        const float base = ty + Ascent(23);
        dl->PushClipRect(ImVec2(tx, ty - Px(4)), ImVec2(maxX, ty + LineH(23) + Px(4)), true);
        float x = tx;
        for (int side = 0; side < 2; ++side)
        {
            const ImU32 c = side == 0 ? v48::BlueTitle : v48::RedTitle;
            const std::string name = g[side].name.empty() ? g[side].display : g[side].name;
            Text(dl, tn, ImVec2(x, ty), c, name.c_str());
            x += TextW(tn, name.c_str());
            if (!g[side].tag.empty())
            {
                const std::string tag = " [" + g[side].tag + "]";
                Text(dl, tt, ImVec2(x, R(base - Ascent(19))), c, tag.c_str());
                x += TextW(tt, tag.c_str());
            }
            if (side == 0)
            {
                x += Px(12);
                Text(dl, tv, ImVec2(x, R(base - Ascent(12)) + Px(2)), v48::Versus, "vs");
                x += TextW(tv, "vs") + Px(12);
            }
        }
        dl->PopClipRect();
    }
    {
        // The facts: the hall, the date, the occasion, the length (hidden in spoiler-free), the flux.
        const Type t = T(12);
        const float fy = R(ctxY + Px(39)), ih = Px(13);
        float x = tx;
        auto fact = [&](ImTextureID icon, const std::string& text, ImU32 c) {
            if (x >= maxX) return;
            if (icon)
            {
                dl->AddImage(icon, ImVec2(x, R(fy + (LineH(12) - ih) * 0.5f)), ImVec2(x + ih, R(fy + (LineH(12) - ih) * 0.5f) + ih));
                x += ih + Px(6);
            }
            x += TextFit(dl, t, ImVec2(x, fy), maxX - x, c, text.c_str()) + Px(18);
        };
        const char* mapName = GetMapName(m.map_id);
        fact(GetGuildHallIcon(), mapName ? mapName : "Map " + std::to_string(m.map_id), v48::Meta);
        fact(nullptr, MatchDate(m, false), v48::Meta);
        if (!m.occasion.empty()) fact(nullptr, m.occasion, v48::Meta);
        fact(GetDurationIcon(), hidden ? std::string(kHiddenWord) : (m.match_duration.empty() ? std::string("-") : m.match_duration),
             hidden ? col::HiddenText : v48::Meta);
        if (!m.flux.empty()) fact(nullptr, "Flux \xC2\xB7 " + m.flux, v48::Meta);
    }
    y = ctxY + Px(73);

    // Tabs: Builds (back to the library and its sidebar), Statistics, Map; 14 px, 26 apart, the
    // active one gold over a 2 px rule; a faint rule under the row.
    {
        const Type t = T(14);
        const float rowH = Px(31);
        struct Tab { const char* label; int tab; } tabs[] = {
            { "Builds", DetailTab_Overview }, { "Statistics", DetailTab_Stats }, { "Map", DetailTab_Map } };
        float x = x0;
        for (const Tab& tab : tabs)
        {
            const bool active = tab.tab == DetailTab_Stats;
            const bool locked = hidden && tab.tab == DetailTab_Map;
            const float w = TextW(t, tab.label);
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::PushID(tab.tab);
            const bool clicked = ImGui::InvisibleButton("##st_tab", ImVec2(w, rowH));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const ImU32 c = active ? v48::TabOn : locked ? WithAlpha(v48::TabOff, 0.5f) : hov ? v48::TabText : tab.tab == DetailTab_Overview ? v48::TabText : v48::TabOff;
            Text(dl, t, ImVec2(x, y), c, tab.label);
            if (active) dl->AddRectFilled(ImVec2(x, y + rowH - Px(2)), ImVec2(x + w, y + rowH), v48::Gold);
            if (hov && !active)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s", locked ? "The map replay is hidden in spoiler-free mode"
                                        : tab.tab == DetailTab_Overview ? "Both teams' builds, beside the match list"
                                                                         : "The match on the map, full width");
            }
            if (clicked && !active && !locked) LeaveStatsPage(tab.tab);
            x += w + Px(26);
        }
        y += rowH;
        HRule(dl, x0, x1, y, v48::TabRule);
        y += 1.0f;
    }
    return y;
}

// ─── The toolbar: Explore / Table, Total / Per minute, the interval ─────────────────────────

struct StatsFrame
{
    const MatchMeta* m = nullptr;
    const BattleLedger::Data* d = nullptr;
    StatsTable::Data data;
    LibraryLedgerHost* host = nullptr;
    float r0 = 0.f, r1 = 0.f;
    int span = 1, pre = 0, duration = 1;
    bool full = true;
    bool perMinute = false;
    std::string scope;        // "Full match · 13:06", "04:12 - 05:40"
    std::string scopeUpper;   // for the inspector's label
};

// A two-way switch (Explore | Table, Total | Per minute): its box, the active side lit. Returns
// the side clicked (-1 none). `t`: the type, `padX` / `padY` the buttons' padding.
static int Segmented(ImDrawList* dl, const char* id, ImVec2 p, const char* a, const char* b, int on, const Type& t, float padX,
                     float padY, float* widthOut)
{
    const float inner = Px(3);
    const float wa = padX * 2.0f + TextW(t, a), wb = padX * 2.0f + TextW(t, b);
    const float h = padY * 2.0f + t.size;
    const float w = 2.0f + inner * 3.0f + wa + wb;
    const ImVec2 box1(p.x + w, p.y + 2.0f + inner * 2.0f + h);
    dl->AddRectFilled(p, box1, v48::ToggleBg, Px(6));
    Outline(dl, p, box1, v48::ToggleLine, Px(6));
    int clicked = -1;
    float x = p.x + 1.0f + inner;
    for (int k = 0; k < 2; ++k)
    {
        const float bw = k == 0 ? wa : wb;
        const ImVec2 q(x, p.y + 1.0f + inner);
        ImGui::SetCursorScreenPos(q);
        ImGui::PushID(id);
        ImGui::PushID(k);
        if (ImGui::InvisibleButton("##seg", ImVec2(bw, h))) clicked = k;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        ImGui::PopID();
        if (hov && k != on) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (k == on) dl->AddRectFilled(q, ImVec2(q.x + bw, q.y + h), v48::ToggleOnBg, Px(5));
        else if (hov) dl->AddRectFilled(q, ImVec2(q.x + bw, q.y + h), v48::HoverBg, Px(5));
        const char* s = k == 0 ? a : b;
        Text(dl, t, ImVec2(q.x + padX, R(q.y + (h - t.size) * 0.5f)), k == on ? v48::ToggleOnText : hov ? v48::Text : v48::ToggleText, s);
        x += bw + inner;
    }
    if (widthOut) *widthOut = w;
    return clicked;
}

// Segmented with a third side (Explore | Table | Lord damage): the same box, the same look.
static int Segmented3(ImDrawList* dl, const char* id, ImVec2 p, const char* const labels[3], int on, const Type& t, float padX,
                      float padY, float* widthOut)
{
    const float inner = Px(3);
    float w = 2.0f + inner * 4.0f;
    for (int k = 0; k < 3; ++k) w += padX * 2.0f + TextW(t, labels[k]);
    const float h = padY * 2.0f + t.size;
    const ImVec2 box1(p.x + w, p.y + 2.0f + inner * 2.0f + h);
    dl->AddRectFilled(p, box1, v48::ToggleBg, Px(6));
    Outline(dl, p, box1, v48::ToggleLine, Px(6));
    int clicked = -1;
    float x = p.x + 1.0f + inner;
    for (int k = 0; k < 3; ++k)
    {
        const float bw = padX * 2.0f + TextW(t, labels[k]);
        const ImVec2 q(x, p.y + 1.0f + inner);
        ImGui::SetCursorScreenPos(q);
        ImGui::PushID(id);
        ImGui::PushID(k);
        if (ImGui::InvisibleButton("##seg", ImVec2(bw, h))) clicked = k;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        ImGui::PopID();
        if (hov && k != on) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (k == on) dl->AddRectFilled(q, ImVec2(q.x + bw, q.y + h), v48::ToggleOnBg, Px(5));
        else if (hov) dl->AddRectFilled(q, ImVec2(q.x + bw, q.y + h), v48::HoverBg, Px(5));
        Text(dl, t, ImVec2(q.x + padX, R(q.y + (h - t.size) * 0.5f)), k == on ? v48::ToggleOnText : hov ? v48::Text : v48::ToggleText, labels[k]);
        x += bw + inner;
    }
    if (widthOut) *widthOut = w;
    return clicked;
}

static float DrawStatsToolbar(ImDrawList* dl, StatsFrame& f, float x0, float x1, float y)
{
    const float top = y + Px(15);
    float viewW = 0.0f;
    static const char* const kViews[3] = { "Explore", "Table", "Lord damage" };
    const int view = Segmented3(dl, "##st_view", ImVec2(x0, top), kViews, s_sp.lord ? 2 : s_sp.table ? 1 : 0, T(13), Px(19), Px(8), &viewW);
    if (view == 2) s_sp.lord = true;
    else if (view >= 0)
    {
        s_sp.lord = false;
        if ((view == 1) != s_sp.table)
        {
            s_sp.table = view == 1;
            if (s_sp.table && !s_sp.active.empty()) s_sp.focusColumn = s_sp.active;
        }
    }
    const float boxH = 2.0f + Px(3) * 2.0f + Px(8) * 2.0f + T(13).size;
    const float midY = top + boxH * 0.5f;
    if (s_sp.lord)
    {
        // The whole match: no indicators, no interval, no per-minute.
        y = top + boxH + Px(15);
        HRule(dl, x0, x1, y, v48::Rule);
        return y + 1.0f;
    }
    Text(dl, T(12), ImVec2(x0 + viewW + Px(20), R(midY - LineH(12) * 0.5f)), v48::Hint, "Same indicators \xC2\xB7 two ways to explore them");

    // At the right: the players, the interval (a click opens the timeline), Total / Per minute.
    const Type ts = T(11);
    char players[32];
    snprintf(players, sizeof players, "%d players", (int)f.d->players.size());
    float x = x1 - TextW(ts, players);
    Text(dl, ts, ImVec2(x, R(midY - LineH(11) * 0.5f)), v48::Scope, players);
    x -= Px(12);
    dl->AddRectFilled(ImVec2(R(x), R(midY - Px(6))), ImVec2(R(x) + 1.0f, R(midY + Px(6))), v48::Rule);
    x -= Px(12);
    {
        const float chevW = Px(7), w = TextW(ts, f.scope.c_str()) + Px(6) + chevW;
        x -= w;
        const ImVec2 p(R(x), R(midY - LineH(11) * 0.5f - Px(5)));
        ImGui::SetCursorScreenPos(p);
        const bool clicked = ImGui::InvisibleButton("##st_scope", ImVec2(w, LineH(11) + Px(10)));
        const bool hov = ImGui::IsItemHovered();
        const ImU32 c = hov || s_sp.timeOpen ? v48::ScopeHover : v48::Scope;
        Text(dl, ts, ImVec2(p.x, R(midY - LineH(11) * 0.5f)), c, f.scope.c_str());
        DrawChevronDown(dl, ImVec2(p.x + w - chevW * 0.5f, midY + (s_sp.timeOpen ? -Px(0.5f) : Px(0.5f))), chevW, c);
        if (hov)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("%s", s_sp.timeOpen ? "Hide the timeline" : "The counted time: the whole match, a range, a moment or the open replay");
        }
        if (clicked) s_sp.timeOpen = !s_sp.timeOpen;
    }
    x -= Px(18);
    {
        const Type t = T(11);
        const float padX = Px(10), padY = Px(4);
        const float w = 2.0f + Px(3) * 3.0f + padX * 4.0f + TextW(t, "Total") + TextW(t, "Per minute");
        const float h = 2.0f + Px(3) * 2.0f + padY * 2.0f + t.size;
        const int pm = Segmented(dl, "##st_pm", ImVec2(R(x - w), R(midY - h * 0.5f)), "Total", "Per minute", f.perMinute ? 1 : 0, t, padX, padY, nullptr);
        if (ImGui::IsMouseHoveringRect(ImVec2(x - w, midY - h * 0.5f), ImVec2(x, midY + h * 0.5f)))
            ImGui::SetTooltip("Total over the counted time, or divided by its length in minutes.\nA share of time is shown in %% per minute.");
        if (pm >= 0)
        {
            StatsTable::SetPerMinute(pm == 1);
            f.perMinute = pm == 1;
        }
    }

    y = top + boxH + Px(15);
    HRule(dl, x0, x1, y, v48::Rule);
    return y + 1.0f;
}

// ─── The indicator strip: a chip per indicator, Clear all, + Indicators ──────────────────────

static float DrawIndicatorStrip(ImDrawList* dl, StatsFrame& f, float x0, float x1, float y)
{
    const auto sel = StatsTable::Selection();
    const float top = y + Px(16);
    const float chipH = Px(34);
    const float midY = top + chipH * 0.5f;
    const Type tc = T(12);

    // + Indicators · N, at the right.
    char addLabel[48];
    snprintf(addLabel, sizeof addLabel, "Indicators \xC2\xB7 %d", (int)sel.size());
    const float plusW = Px(9);
    const float addW = 2.0f + Px(12) * 2.0f + plusW + Px(7) + TextW(tc, addLabel), addH = Px(36);
    const ImVec2 addP(R(x1 - addW), R(midY - addH * 0.5f));
    bool hov = false;
    if (BoxButton(dl, "##st_add", addP, ImVec2(addW, addH), s_sp.pickerOpen ? v48::ChipOnBg : v48::ChipBg,
                  s_sp.pickerOpen ? v48::ChipOnLine : v48::AddLine, Px(5), &hov))
        s_sp.pickerOpen = !s_sp.pickerOpen;
    if (hov) dl->AddRectFilled(addP, ImVec2(addP.x + addW, addP.y + addH), v48::HoverBg, Px(5));
    DrawPlus(dl, ImVec2(addP.x + 1.0f + Px(12) + plusW * 0.5f, midY), plusW, v48::AddText);
    Text(dl, tc, ImVec2(addP.x + 1.0f + Px(12) + plusW + Px(7), R(midY - LineH(12) * 0.5f)), v48::AddText, addLabel);
    if (hov) ImGui::SetTooltip("%s", s_sp.pickerOpen ? "Close the indicator list" : "Choose the indicators: one list for Explore and Table");
    float stripRight = addP.x - Px(8);

    // Clear all.
    if (!sel.empty())
    {
        const float w = Px(12) * 2.0f + TextW(tc, "Clear all"), h = Px(32);
        const ImVec2 p(R(stripRight - w), R(midY - h * 0.5f));
        bool ch = false;
        if (BoxButton(dl, "##st_clear", p, ImVec2(w, h), 0, 0, Px(5), &ch))
        {
            s_sp.undo = StatsTable::TakeSnapshot();
            s_sp.undoActive = s_sp.active;
            s_sp.cleared = (int)sel.size();
            s_sp.undoReady = true;
            StatsTable::ClearSelection();
            s_sp.active.clear();
        }
        if (ch)
        {
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), v48::HoverBg, Px(5));
            ImGui::SetTooltip("Take every indicator out of both views (Undo brings them back)");
        }
        Text(dl, tc, ImVec2(p.x + Px(12), R(midY - LineH(12) * 0.5f)), ch ? v48::ClearHover : v48::Clear, "Clear all");
        stripRight = p.x - Px(16);
    }

    if (sel.empty())
    {
        Text(dl, tc, ImVec2(x0, R(midY - LineH(12) * 0.5f)), v48::Muted, "No indicators selected");
    }
    else
    {
        // The chips, in a strip that scrolls sideways when they overflow (the wheel scrolls it);
        // a chip's name and its cross are two hit boxes.
        const float stripW = std::max(Px(60), stripRight - x0);
        const float barH = Px(4);
        ImGui::SetCursorScreenPos(ImVec2(x0, top));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, barH);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("##st_chips", ImVec2(stripW, chipH + barH + Px(2)), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f && ImGui::GetScrollMaxX() > 0.0f)
            ImGui::SetScrollX(std::clamp(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * Px(80), 0.0f, ImGui::GetScrollMaxX()));
        const ImVec2 o = ImGui::GetCursorScreenPos();
        float x = o.x;
        std::string remove;
        for (const std::string& key : sel)
        {
            StatsTable::Indicator ind;
            if (!StatsTable::Describe(key, ind)) continue;
            const bool on = key == s_sp.active;
            const float labelW = Px(12) * 2.0f + TextW(tc, ind.label.c_str());
            const float xW = Px(30);
            const ImVec2 a(R(x), R(o.y)), b(R(x + labelW + xW + 2.0f), R(o.y) + chipH);
            cdl->AddRectFilled(a, b, on ? v48::ChipOnBg : v48::ChipBg, Px(5));
            ImGui::PushID(key.c_str());
            ImGui::SetCursorScreenPos(ImVec2(a.x + 1.0f, a.y + 1.0f));
            if (ImGui::InvisibleButton("##chip", ImVec2(labelW, chipH - 2.0f))) ActivateIndicator(key);
            const bool hl = ImGui::IsItemHovered();
            if (hl)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                cdl->AddRectFilled(ImVec2(a.x + 1.0f, a.y + 1.0f), ImVec2(a.x + 1.0f + labelW, b.y - 1.0f), v48::HoverBg, Px(4));
                StatsTable::IndicatorTooltip(*f.host, key, s_sp.table ? "Click: sort the table by it." : "Click: its ranking and breakdown.");
            }
            ImGui::SetCursorScreenPos(ImVec2(a.x + 1.0f + labelW, a.y + 1.0f));
            if (ImGui::InvisibleButton("##x", ImVec2(xW, chipH - 2.0f))) remove = key;
            const bool hx = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hx)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                cdl->AddRectFilled(ImVec2(a.x + 1.0f + labelW, a.y + 1.0f), ImVec2(b.x - 1.0f, b.y - 1.0f), v48::HoverBg, Px(4));
                ImGui::SetTooltip("Remove %s from both views", ind.label.c_str());
            }
            Outline(cdl, a, b, on ? v48::ChipOnLine : v48::ChipLine, Px(5));
            Text(cdl, tc, ImVec2(a.x + 1.0f + Px(12), R(o.y + (chipH - LineH(12)) * 0.5f)), on ? v48::ChipOnText : v48::ChipText, ind.label.c_str());
            cdl->AddRectFilled(ImVec2(a.x + 1.0f + labelW, a.y + 1.0f), ImVec2(a.x + 2.0f + labelW, b.y - 1.0f), v48::ChipXLine);
            DrawCross(cdl, ImVec2(a.x + 1.0f + labelW + xW * 0.5f, o.y + chipH * 0.5f), Px(7.5f), hx ? v48::ClearHover : v48::ChipX);
            x = b.x + Px(8);
        }
        ImGui::SetCursorScreenPos(ImVec2(x, o.y));
        ImGui::Dummy(ImVec2(1.0f, chipH));
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (!remove.empty()) RemoveIndicator(remove);
    }

    y = top + chipH + Px(17);
    HRule(dl, x0, x1, y, v48::Rule);
    return y + 1.0f;
}

// ─── The timeline strip: Full match, a range, a moment, the open replay ─────────────────────

static float DrawTimeStrip(const StatsFrame& f, float x0, float x1, float y)
{
    const MatchMeta& m = *f.m;
    y += Px(12);
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    const ReplaySync::Replay* rp = ReplaySync::Find(m.folder_name);
    float& from = s_state.detailIntervalFrom;
    float& to = s_state.detailIntervalTo;
    const float fspan = (float)std::max(1, f.span);
    auto frac = [&](float t) { return std::clamp((t + f.pre) / fspan, 0.0f, 1.0f); };
    const float frameH = ImGui::GetFrameHeight();
    const float rowH = std::max(frameH, ImGui::GetTextLineHeight() + 10.0f);
    float x = x0;
    auto place = [&](float h) { ImGui::SetCursorScreenPos(ImVec2(x, y + (rowH - h) * 0.5f)); };
    auto next = [&](float gap = 6.0f) { x = ImGui::GetItemRectMax().x + gap; };
    const float pillH = ImGui::GetTextLineHeight() + 10.0f;

    place(pillH);
    if (BattleLedger::Pill("Full match", s_state.scope == Scope_Full)) { s_state.scope = Scope_Full; from = 0.0f; to = 1.0f; }
    next(10.0f);

    // Range: the two ends, typed (Enter applies), or dragged on the timeline below.
    place(pillH);
    if (BattleLedger::Pill("Range", s_state.scope == Scope_Range)) s_state.scope = Scope_Range;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag on the timeline, or type the start and the end (12:30) and press Enter.");
    next();
    {
        static char fromBuf[16], toBuf[16];
        static float shownFrom = -1.0f, shownTo = -1.0f;
        if (shownFrom != from || shownTo != to)
        {
            FormatClock((int)std::lround(from * fspan) - f.pre, fromBuf, sizeof fromBuf);
            FormatClock((int)std::lround(to * fspan) - f.pre, toBuf, sizeof toBuf);
            shownFrom = from; shownTo = to;
        }
        const float fw = ImGui::CalcTextSize("-00:00").x + 14.0f;
        place(frameH);
        ImGui::SetNextItemWidth(fw);
        bool apply = ImGui::InputText("##st_from", fromBuf, sizeof fromBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        next(4.0f);
        place(ImGui::GetTextLineHeight());
        ImGui::TextUnformatted("-");
        next(4.0f);
        place(frameH);
        ImGui::SetNextItemWidth(fw);
        apply |= ImGui::InputText("##st_to", toBuf, sizeof toBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        next(16.0f);
        float a = 0.0f, b = 0.0f;
        if (apply && ParseClockText(fromBuf, a) && ParseClockText(toBuf, b) && b > a)
        {
            s_state.scope = Scope_Range;
            from = frac(a); to = frac(b);
            shownFrom = shownTo = -1.0f;
            if (rp) ReplaySync::RequestSeek(m.folder_name, a);
        }
    }

    // A moment: 10 s either side of a death or a flag capture; the arrows step through them.
    const std::vector<float> moments = ScopeMoments();
    auto step = [&](int dir) {
        const float ref = s_state.scope == Scope_Moment ? s_state.momentT : rp ? rp->t : from * fspan - f.pre;
        float best = 0.0f;
        bool found = false;
        for (float t : moments)
        {
            if (dir > 0 && t > ref + 0.01f) { best = t; found = true; break; }
            if (dir < 0 && t < ref - 0.01f) { best = t; found = true; }
        }
        if (found) SetMoment(m, best);
    };
    ImGui::BeginDisabled(moments.empty());
    place(frameH);
    if (ImGui::ArrowButton("##st_prev", ImGuiDir_Left)) step(-1);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The previous death or flag capture.");
    next(4.0f);
    char momentLabel[48];
    if (s_state.scope == Scope_Moment)
        snprintf(momentLabel, sizeof momentLabel, "Moment %s  \xC2\xB1%d s", ClockText(s_state.momentT).c_str(), (int)kMomentHalf);
    else
        snprintf(momentLabel, sizeof momentLabel, "Moment  \xC2\xB1%d s", (int)kMomentHalf);
    place(pillH);
    if (BattleLedger::Pill(momentLabel, s_state.scope == Scope_Moment)) step(+1);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("10 s either side of a death or a flag capture.\nClick one on the timeline, or step with the arrows.");
    next(4.0f);
    place(frameH);
    if (ImGui::ArrowButton("##st_next", ImGuiDir_Right)) step(+1);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The next death or flag capture.");
    next(16.0f);
    ImGui::EndDisabled();

    // The open replay: the stats follow its playhead.
    if (rp)
    {
        char followLabel[48];
        snprintf(followLabel, sizeof followLabel, rp->playing ? "Follow the replay  %s" : "Follow the replay  %s  (paused)",
                 ClockText(std::floor(rp->t)).c_str());
        place(pillH);
        if (BattleLedger::Pill(followLabel, s_state.scope == Scope_Live)) s_state.scope = Scope_Live;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The stats move with the open replay.\nClicks on the timeline move the replay.");
        next();
        if (s_state.scope == Scope_Live)
        {
            place(pillH);
            if (BattleLedger::Pill("Up to now", !s_state.liveLast30)) s_state.liveLast30 = false;
            next();
            place(pillH);
            if (BattleLedger::Pill("Last 30 s", s_state.liveLast30)) s_state.liveLast30 = true;
            next();
        }
    }
    else
    {
        place(ImGui::GetTextLineHeight());
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(v48::Muted), "%s",
                           g_cloudDownloadInProgress || g_pendingReplay.requested ? "Opening the replay..."
                                                                                  : "While the replay is open, the stats can follow it.");
    }
    y += rowH + Px(10);

    // The match's timeline: health, deaths, captures; drag a range, click a moment.
    const float h = Px(150);
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    ImGui::PushID("##st_timeline");
    ImGui::BeginChild("##st_timeline_c", ImVec2(x1 - x0, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    DrawDetailTimeline(m, h);
    ImGui::EndChild();
    ImGui::PopID();
    y += h + Px(14);
    HRule(ImGui::GetWindowDrawList(), x0, x1, y, v48::Rule);
    return y + 1.0f;
}

// ─── The picker: every indicator by category, in the page's flow ────────────────────────────

struct PickerGroup
{
    std::string title;
    std::vector<std::string> keys;
};

static std::vector<std::string> LayoutGroups()
{
    std::vector<std::string> g;
    for (const auto& e : MatchStats::Layout())
        if (std::find(g.begin(), g.end(), e.group) == g.end()) g.push_back(e.group);
    return g;
}

static float DrawPicker(ImDrawList* dl, StatsFrame& f, float x0, float x1, float y)
{
    const auto groups = LayoutGroups();
    if (s_sp.category.empty() || std::find(groups.begin(), groups.end(), s_sp.category) == groups.end())
        s_sp.category = groups.empty() ? std::string() : groups.front();
    const auto sel = StatsTable::Selection();
    auto selected = [&](const std::string& k) { return std::find(sel.begin(), sel.end(), k) != sel.end(); };
    const bool searching = s_sp.search[0] != '\0';

    // What the list shows: the category's sub-groups, or every match of the search across the
    // categories (the detailed indicators included), under "Group · Sub-group".
    std::vector<PickerGroup> list;
    auto groupFor = [&](const std::string& title) -> PickerGroup& {
        for (auto& g : list) if (g.title == title) return g;
        list.push_back({ title, {} });
        return list.back();
    };
    for (const auto& e : MatchStats::Layout())
    {
        if (searching ? !StatsTable::MatchesSearch(e.key, s_sp.search) : s_sp.category != e.group) continue;
        StatsTable::Indicator ind;
        if (!StatsTable::Describe(e.key, ind)) continue;
        const std::string title = searching ? std::string(e.group) + " \xC2\xB7 " + e.sub : std::string(e.sub);
        groupFor(title).keys.push_back(e.key);
    }
    if (searching)
        for (const auto& key : StatsTable::AllIndicators())
        {
            StatsTable::Indicator ind;
            if (!StatsTable::Describe(key, ind) || !ind.detailed || !StatsTable::MatchesSearch(key, s_sp.search)) continue;
            groupFor("Detailed (search only)").keys.push_back(key);
        }

    y += Px(15);
    const float pad = Px(20);
    const ImVec2 a(x0, R(y));
    const float innerX0 = x0 + 1.0f + pad, innerX1 = x1 - 1.0f - pad;

    // Measure the list first: auto-fit columns of at least 230 px, 18 apart; rows 12 apart.
    const float gapX = Px(18), gapY = Px(12), optH = Px(36), headH = LineH(10) + Px(10);
    const float listW = innerX1 - innerX0 - Px(10);
    int cols = std::max(1, (int)std::floor((listW + gapX) / (Px(230) + gapX)));
    cols = std::max(1, std::min(cols, (int)list.size()));
    const float colW = (listW - gapX * (cols - 1)) / cols;
    std::vector<float> rowHeights;
    for (size_t i = 0; i < list.size(); ++i)
    {
        const float h = headH + optH * list[i].keys.size();
        if (i % cols == 0) rowHeights.push_back(h);
        else rowHeights.back() = std::max(rowHeights.back(), h);
    }
    float listH = 0.0f;
    for (size_t r = 0; r < rowHeights.size(); ++r) listH += rowHeights[r] + (r ? gapY : 0.0f);
    if (list.empty()) listH = LineH(12) + Px(10);
    const float viewH = std::min(listH, Px(340));

    const float headBlockH = Px(37);
    const float tabsH = Px(18) + Px(31) + Px(13);
    const float footH = Px(16) + 1.0f + Px(13) + LineH(11);
    const float boxH = 2.0f + pad * 2.0f + headBlockH + tabsH + 1.0f + Px(20) + viewH + footH;
    const ImVec2 b(x1, R(y + boxH));
    FillLinear(dl, a, b, Px(5), 110.0f, v48::PickerA, v48::PickerB);
    Outline(dl, a, b, v48::PickerLine, Px(5));

    float py = a.y + 1.0f + pad;
    // Head: the title and what it does; the search and Done at the right.
    Text(dl, T(17), ImVec2(innerX0, R(py)), v48::Text, "Choose your indicators");
    Text(dl, T(11), ImVec2(innerX0, R(py + LineH(17) + Px(5))), v48::Sub,
         "One selection for both views. Checked indicators appear above and become table columns.");
    {
        const float doneW = 2.0f + Px(17) * 2.0f + TextW(T(13), "Done"), doneH = Px(33);
        const ImVec2 dp(R(innerX1 - doneW), R(py + (headBlockH - doneH) * 0.5f));
        bool dh = false;
        if (BoxButton(dl, "##st_done", dp, ImVec2(doneW, doneH), v48::ChipBg, v48::ChipLine, Px(5), &dh))
            s_sp.pickerOpen = false;
        if (dh) dl->AddRectFilled(dp, ImVec2(dp.x + doneW, dp.y + doneH), v48::HoverBg, Px(5));
        Text(dl, T(13), ImVec2(dp.x + 1.0f + Px(17), R(dp.y + (doneH - LineH(13)) * 0.5f)), dh ? v48::Text : v48::ChipText, "Done");

        const float sw = Px(300), sh = Px(37);
        const ImVec2 sp(R(dp.x - Px(18) - sw), R(py + (headBlockH - sh) * 0.5f));
        dl->AddRectFilled(sp, ImVec2(sp.x + sw, sp.y + sh), v48::SearchBg, Px(4));
        Outline(dl, sp, ImVec2(sp.x + sw, sp.y + sh), ImGui::IsMouseHoveringRect(sp, ImVec2(sp.x + sw, sp.y + sh)) ? col::ActionHovRule : v48::SearchLine, Px(4));
        const Type t = T(13);
        ImGui::SetCursorScreenPos(ImVec2(sp.x + Px(12), R(sp.y + (sh - t.font->FontSize) * 0.5f)));
        ImGui::PushFont(t.font);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(v48::SearchText));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::ColorConvertU32ToFloat4(v48::Muted));
        ImGui::SetNextItemWidth(sw - Px(24));
        if (!s_sp.pickerShown) ImGui::SetKeyboardFocusHere();
        s_sp.pickerShown = true;
        ImGui::InputTextWithHint("##st_search", "Search indicators\xE2\x80\xA6", s_sp.search, sizeof s_sp.search);
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(2);
        ImGui::PopFont();
    }
    py += headBlockH;

    // The categories, each with how many of its indicators are selected; a rule under them.
    {
        float tx = innerX0;
        const float ty = R(py + Px(18)), th = Px(31);
        const Type t = T(13), tn = T(11);
        for (const auto& g : groups)
        {
            int n = 0;
            for (const auto& e : MatchStats::Layout()) if (g == e.group && selected(e.key)) ++n;
            const std::string count = std::to_string(n);
            const float w = 2.0f + Px(17) * 2.0f + TextW(t, g.c_str()) + Px(8) + TextW(tn, count.c_str());
            const bool on = !searching && g == s_sp.category;
            bool h = false;
            ImGui::PushID(g.c_str());
            if (BoxButton(dl, "##cat", ImVec2(R(tx), ty), ImVec2(w, th), on ? v48::ChipOnBg : v48::ChipBg, on ? v48::CatOnLine : v48::ChipLine, Px(5), &h))
            {
                s_sp.category = g;
                s_sp.search[0] = '\0';
            }
            ImGui::PopID();
            if (h && !on) dl->AddRectFilled(ImVec2(R(tx), ty), ImVec2(R(tx) + w, ty + th), v48::HoverBg, Px(5));
            const float lx = R(tx) + 1.0f + Px(17);
            Text(dl, t, ImVec2(lx, R(ty + (th - LineH(13)) * 0.5f)), on ? v48::CatOnText : h ? v48::Text : v48::ChipText, g.c_str());
            Text(dl, tn, ImVec2(lx + TextW(t, g.c_str()) + Px(8), R(ty + (th - LineH(11)) * 0.5f + Px(1))), v48::CatCount, count.c_str());
            tx += w + Px(7);
        }
        if (searching)
        {
            char found[64];
            int total = 0;
            for (const auto& g : list) total += (int)g.keys.size();
            snprintf(found, sizeof found, "%d found in every category", total);
            Text(dl, T(11), ImVec2(tx + Px(10), R(ty + (th - LineH(11)) * 0.5f)), v48::Muted, found);
        }
        py += tabsH;
        HRule(dl, innerX0, innerX1, py, v48::Rule);
        py += 1.0f + Px(20);
    }

    // The list: a box per sub-group with a rule at its left; scrolls past 340 px.
    {
        ImGui::SetCursorScreenPos(ImVec2(innerX0, py));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Px(8));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("##st_picker_list", ImVec2(innerX1 - innerX0, viewH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        if (list.empty())
            Text(ldl, T(12), o, v48::Muted, searching ? "No indicator matches." : "No indicators in this category.");
        std::string toggle;
        float gy = o.y;
        for (size_t i = 0; i < list.size(); ++i)
        {
            const size_t r = i / cols;
            if (i % cols == 0 && i) gy += rowHeights[r - 1] + gapY;
            const float gx = o.x + (i % cols) * (colW + gapX);
            const PickerGroup& g = list[i];
            const float gh = headH + optH * g.keys.size();
            ldl->AddRectFilled(ImVec2(R(gx), gy), ImVec2(R(gx) + 1.0f, gy + gh), v48::Rule);
            const float ix = gx + 1.0f + Px(15), iw = colW - 1.0f - Px(15);
            ldl->PushClipRect(ImVec2(ix - Px(4), gy), ImVec2(ix + iw, gy + gh), true);
            TextSpaced(ldl, T(10, false), ImVec2(ix, gy), v48::GroupHead, g.title.c_str(), Px(1.5f));
            float oy = gy + headH;
            for (const std::string& key : g.keys)
            {
                StatsTable::Indicator ind;
                StatsTable::Describe(key, ind);
                const bool on = selected(key);
                ImGui::SetCursorScreenPos(ImVec2(ix, oy));
                ImGui::PushID(key.c_str());
                const bool clicked = ImGui::InvisibleButton("##opt", ImVec2(iw, optH));
                const bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                if (hov)
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ldl->AddRectFilled(ImVec2(ix - Px(4), oy + Px(3)), ImVec2(ix + iw, oy + optH - Px(3)), v48::HoverBg, Px(3));
                    StatsTable::IndicatorTooltip(*f.host, key, on ? "Click: take it out of both views." : "Click: add it to both views.");
                }
                if (clicked) toggle = key;
                const float cy = oy + optH * 0.5f;
                DrawCheckBox(ldl, ImVec2(R(ix + Px(4)), R(cy - Px(7))), Px(14), on ? 1 : 0, hov);
                const float lx = ix + Px(4) + Px(14) + Px(10);
                const char* tag = ind.estimated ? "ESTIMATED" : nullptr;
                const float tagW = tag ? TextW(T(9), tag) + Px(8) : 0.0f;
                TextFit(ldl, T(12), ImVec2(lx, R(cy - LineH(12) * 0.5f)), ix + iw - lx - tagW, on ? v48::Text : v48::Option, ind.label.c_str());
                if (tag) TextRight(ldl, T(9), ix + iw, R(cy - LineH(9) * 0.5f), v48::GroupHead, tag);
                oy += optH;
            }
            ldl->PopClipRect();
        }
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + listH));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (!toggle.empty())
        {
            if (selected(toggle)) RemoveIndicator(toggle);
            else AddIndicator(toggle);
        }
        py += viewH;
    }

    // Foot: what the selection makes; the rule over it.
    {
        py += Px(16);
        HRule(dl, innerX0, innerX1, py, v48::Rule);
        py += 1.0f + Px(13);
        const Type t = T(11);
        const int n = (int)StatsTable::Selection().size();
        char left[48], right[96];
        snprintf(left, sizeof left, "%d selected", n);
        Text(dl, t, ImVec2(innerX0, R(py)), v48::Foot, left);
        float fx = innerX0 + TextW(t, left) + Px(6);
        DrawArrowRight(dl, ImVec2(fx + Px(6), py + LineH(11) * 0.55f), Px(11), v48::Foot);
        fx += Px(18);
        snprintf(right, sizeof right, "%d Explore shortcuts + %d table columns", n, n);
        Text(dl, t, ImVec2(fx, R(py)), v48::Foot, right);
        TextRight(dl, t, innerX1, R(py), v48::Foot, "Search finds the detailed indicators too \xC2\xB7 unavailable values show as \xE2\x80\x94");
    }
    return b.y + Px(20);
}

// ─── Clear all's line, and the empty state ──────────────────────────────────────────────────

static float DrawClearedLine(ImDrawList* dl, float x0, float x1, float y)
{
    const Type t = T(12);
    const float h = Px(47);
    char msg[80];
    snprintf(msg, sizeof msg, "%d indicator%s cleared from both views.", s_sp.cleared, s_sp.cleared == 1 ? "" : "s");
    const float midY = y + h * 0.5f;
    Text(dl, t, ImVec2(x0, R(midY - LineH(12) * 0.5f)), v48::Feedback, msg);
    const float bw = 2.0f + Px(12) * 2.0f + TextW(t, "Undo"), bh = Px(26);
    const ImVec2 bp(R(x0 + TextW(t, msg) + Px(12)), R(midY - bh * 0.5f));
    bool hov = false;
    if (BoxButton(dl, "##st_undo", bp, ImVec2(bw, bh), v48::ChipBg, hov ? v48::ChipOnLine : v48::ChipLine, Px(5), &hov))
    {
        StatsTable::Restore(s_sp.undo);
        s_sp.active = s_sp.undoActive;
        s_sp.undoReady = false;
        NormalizeActive();
    }
    if (hov)
    {
        dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), v48::HoverBg, Px(5));
        ImGui::SetTooltip("Bring back the indicators as they were before Clear all");
    }
    Text(dl, t, ImVec2(bp.x + 1.0f + Px(12), R(midY - LineH(12) * 0.5f)), v48::ChipOnText, "Undo");
    HRule(dl, x0, x1, y + h - 1.0f, v48::FeedbackRule);
    return y + h;
}

static float DrawEmptyState(ImDrawList* dl, float x0, float x1, float y)
{
    y += Px(22);
    const float h = Px(60) * 2.0f + LineH(22) + Px(12) + Px(12) + LineH(12) + Px(22) + Px(35);
    const ImVec2 a(x0, R(y)), b(x1, R(y + h));
    dl->AddRectFilled(a, b, v48::EmptyBg, Px(5));
    Outline(dl, a, b, v48::EmptyLine, Px(5));
    const float cx = (x0 + x1) * 0.5f;
    float ty = a.y + Px(60);
    const char* title = "No indicators selected";
    const char* sub = "Choose indicators to compare players. Your selected player and match are kept.";
    Text(dl, T(22), ImVec2(R(cx - TextW(T(22), title) * 0.5f), R(ty)), v48::Text, title);
    ty += LineH(22) + Px(24);
    Text(dl, T(12), ImVec2(R(cx - TextW(T(12), sub) * 0.5f), R(ty)), v48::EmptySub, sub);
    ty += LineH(12) + Px(22);
    const Type bt = T(13);
    const float plusW = Px(9);
    const float bw = 2.0f + Px(12) * 2.0f + plusW + Px(7) + TextW(bt, "Choose indicators"), bh = Px(35);
    const ImVec2 bp(R(cx - bw * 0.5f), R(ty));
    bool hov = false;
    if (BoxButton(dl, "##st_choose", bp, ImVec2(bw, bh), v48::ChipBg, v48::CatOnLine, Px(5), &hov)) s_sp.pickerOpen = true;
    if (hov) dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), v48::HoverBg, Px(5));
    DrawPlus(dl, ImVec2(bp.x + 1.0f + Px(12) + plusW * 0.5f, bp.y + bh * 0.5f), plusW, v48::ChipOnText);
    Text(dl, bt, ImVec2(bp.x + 1.0f + Px(12) + plusW + Px(7), R(bp.y + (bh - LineH(13)) * 0.5f)), v48::ChipOnText, "Choose indicators");
    return b.y;
}

// ─── Explore, left: the indicator, both guilds, every player ────────────────────────────────

// Players in the active indicator's order: the explicit direction, players without a value last.
static std::vector<int> RankedPlayers(const BattleLedger::Data& d, const StatsTable::Column& col, bool desc)
{
    std::vector<int> order;
    for (int pid : d.players) if (d.agents.count(pid)) order.push_back(pid);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const auto& ca = col.players.at(a);
        const auto& cb = col.players.at(b);
        if (ca.has != cb.has) return ca.has;
        if (ca.v != cb.v) return desc ? ca.v > cb.v : ca.v < cb.v;
        return d.agents.at(a).name < d.agents.at(b).name;
    });
    return order;
}

// The sentence under the two guild bars: which guild recorded more, and by how much.
static std::string CompareSentence(const BattleLedger::Data& d, const StatsTable::Indicator& ind, const StatsTable::Column& col,
                                   bool perMinute, std::string& strongA, std::string& strongB)
{
    const auto& a = col.team[0];
    const auto& b = col.team[1];
    if (!a.has || !b.has) return "";
    using MatchStats::Agg;
    if (ind.agg == Agg::Ratio) return "Each guild's rate is worked out over all of its tries, not averaged from its players.";
    if (ind.agg == Agg::Median) return "Each guild's median is worked out over all of its cases, not averaged from its players.";
    if (!col.additive) return "Each guild's figure is its players' average share of the time counted.";
    if (std::fabs(a.v - b.v) < 0.05) return "Both guilds recorded the same.";
    const int hi = a.v > b.v ? 0 : 1;
    StatsTable::Cell diff;
    diff.has = true;
    diff.v = std::fabs(a.v - b.v);
    strongA = SideTag(d, hi);
    strongB = StatsTable::Format(ind.key, diff, perMinute) + " more " + Lower(ind.label) + (perMinute ? " per minute" : "");
    const double lo = hi == 0 ? b.v : a.v;
    char pct[64] = "";
    if (lo > 0.0) snprintf(pct, sizeof pct, " (%.1f%% more than %s)", (std::fabs(a.v - b.v) / lo) * 100.0, SideTag(d, 1 - hi).c_str());
    return std::string(pct);
}

static float DrawMetricFocus(ImDrawList* dl, StatsFrame& f, const StatsTable::Indicator& ind, const StatsTable::Column& col,
                             float x, float w, float y)
{
    const BattleLedger::Data& d = *f.d;
    const MatchMeta& m = *f.m;
    const float top = y;

    // The intro: the name, its definition; the counted time in a box at the right.
    {
        const Type tp = T(11);
        const std::string period = f.scope;
        const float pw = 2.0f + Px(10) * 2.0f + TextW(tp, period.c_str()), ph = Px(29);
        const ImVec2 pp(R(x + w - pw), R(y));
        dl->AddRectFilled(pp, ImVec2(pp.x + pw, pp.y + ph), Hex(0xffffff, 0x03), Px(4));
        Outline(dl, pp, ImVec2(pp.x + pw, pp.y + ph), v48::PeriodLine, Px(4));
        Text(dl, tp, ImVec2(pp.x + 1.0f + Px(10), R(pp.y + (ph - LineH(11)) * 0.5f)), v48::PeriodText, period.c_str());
        const float tw = w - pw - Px(20);
        TextFit(dl, T(24), ImVec2(x, R(y)), tw, v48::Text, ind.label.c_str());
        y += LineH(24) + Px(6);
        std::string def = ind.meaning;
        const std::string unit = StatsTable::UnitText(ind.key, f.perMinute);
        const float dh = TextWrapped(dl, T(12), ImVec2(x, R(y)), tw, LineH(12) + Px(2), v48::Muted, def, 2);
        if (ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + tw, y + dh)) && ImGui::IsWindowHovered())
            StatsTable::IndicatorTooltip(*f.host, ind.key, ("Unit: " + unit + ".").c_str());
        y += dh;
        y = std::max(y, top + Px(52));
    }
    y += Px(14);

    // Both guilds: cape, name, a bar on one scale, the guild's own figure.
    const bool teams = col.team[0].has && col.team[1].has;
    if (teams)
    {
        std::string ft1, ft2;
        ParseFolderTags(m.folder_name, ft1, ft2);
        const GuildLabel g[2] = { GetPartyGuild(m, "1", ft1), GetPartyGuild(m, "2", ft2) };
        const double mx = std::max({ col.team[0].v, col.team[1].v, 1e-9 });
        const float rowH = Px(28);
        for (int side = 0; side < 2; ++side)
        {
            const float ry = R(y + side * (rowH + Px(10)));
            float gx = x;
            if (ImTextureID cape = GetGuildCape(m, g[side]))
            {
                ImageContain(dl, cape, 96.0f, 192.0f, ImVec2(gx, ry), ImVec2(gx + Px(15), ry + rowH));
                gx += Px(15) + Px(9);
            }
            const std::string label = SideGuild(d, side);
            TextFit(dl, T(13), ImVec2(gx, R(ry + (rowH - LineH(13)) * 0.5f)), x + Px(210) - gx, side == 0 ? v48::BlueTitle : v48::RedTitle,
                    label.c_str());
            if (ImGui::IsMouseHoveringRect(ImVec2(x, ry), ImVec2(x + Px(210), ry + rowH)) && ImGui::IsWindowHovered())
                ImGui::SetTooltip("%s", label.c_str());
            const float tx0 = x + Px(220), tx1 = x + w - Px(80);
            const ImVec2 ta(R(tx0), R(ry + (rowH - Px(10)) * 0.5f)), tb(R(tx1), ta.y + Px(10));
            dl->AddRectFilled(ta, tb, v48::Track, Px(2));
            const float frac = (float)std::clamp(col.team[side].v / mx, 0.0, 1.0);
            if (frac > 0.0f)
                FillLinear(dl, ta, ImVec2(ta.x + (tb.x - ta.x) * frac, tb.y), Px(2), 90.0f, side == 0 ? v48::BlueA : v48::RedA,
                           side == 0 ? v48::BlueB : v48::RedB);
            const std::string v = StatsTable::Format(ind.key, col.team[side], f.perMinute, true);
            TextRight(dl, T(20), x + w, R(ry + (rowH - LineH(20)) * 0.5f), v48::Score, v.c_str());
        }
        y += rowH * 2.0f + Px(10);

        // The sentence.
        std::string strongA, strongB;
        const std::string rest = CompareSentence(d, ind, col, f.perMinute, strongA, strongB);
        y += Px(15);
        const Type t = T(12), tb = T(12, true);
        float sx = x;
        if (!strongA.empty())
        {
            Text(dl, tb, ImVec2(sx, R(y)), v48::ExplainBold, strongA.c_str());
            sx += TextW(tb, strongA.c_str());
            Text(dl, t, ImVec2(sx, R(y)), v48::Explain, " recorded ");
            sx += TextW(t, " recorded ");
            Text(dl, tb, ImVec2(sx, R(y)), v48::ExplainBold, strongB.c_str());
            sx += TextW(tb, strongB.c_str());
            TextFit(dl, t, ImVec2(sx, R(y)), x + w - sx, v48::Explain, (rest + ".").c_str());
        }
        else
            TextFit(dl, t, ImVec2(sx, R(y)), w, v48::Explain, rest.c_str());
        y += LineH(12) + Px(20);
    }

    // The ranking's heading: its name, the scale, the order (a click turns it).
    HRule(dl, x, x + w, y, v48::RankRule);
    y += 1.0f + Px(15);
    {
        const Type th = T(13), ts = T(10), to = T(11);
        Text(dl, th, ImVec2(x, R(y)), v48::Text, "Player comparison");
        Text(dl, ts, ImVec2(x + TextW(th, "Player comparison") + Px(14), R(y + Ascent(13) - Ascent(10))), v48::RankSmall,
             "Both teams \xC2\xB7 same scale");
        const bool desc = StatsTable::SortDescending();
        const char* order = desc ? "Highest first" : "Lowest first";
        const float ow = TextW(to, order) + Px(12);
        const ImVec2 op(R(x + w - ow), R(y));
        ImGui::SetCursorScreenPos(op);
        if (ImGui::InvisibleButton("##st_order", ImVec2(ow, LineH(13))))
            StatsTable::SetSort(StatsTable::SortKey().empty() ? ind.key : StatsTable::SortKey(), !desc);
        const bool hov = ImGui::IsItemHovered();
        if (hov)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("%s", desc ? "Show the lowest first" : "Show the highest first");
        }
        const ImU32 oc = hov ? v48::ScopeHover : v48::Order;
        Text(dl, to, ImVec2(op.x, R(y + Ascent(13) - Ascent(11))), oc, order);
        DrawArrowVertical(dl, ImVec2(op.x + ow - Px(4), y + LineH(13) * 0.55f), Px(9), oc, desc);
        y += LineH(13) + Px(12);
    }

    // Labels: # PLAYER GUILD <INDICATOR> TOTAL, 9 px spaced.
    const float pad = Px(10), gap = Px(10);
    const float cNum = Px(22), cPlayer = Px(190), cGuild = Px(48), cValue = Px(62);
    const float xNum = x + pad, xPlayer = xNum + cNum + gap, xGuild = xPlayer + cPlayer + gap, xTrack = xGuild + cGuild + gap;
    const float xValueR = x + w - pad, xTrackEnd = xValueR - cValue - gap;
    {
        const Type t = T(9);
        const float tr = Px(1);
        TextSpaced(dl, t, ImVec2(xNum, R(y)), v48::Labels, "#", tr);
        TextSpaced(dl, t, ImVec2(xPlayer, R(y)), v48::Labels, "PLAYER", tr);
        TextSpaced(dl, t, ImVec2(xGuild, R(y)), v48::Labels, "GUILD", tr);
        TextSpaced(dl, t, ImVec2(xTrack, R(y)), v48::Labels, Upper(ind.head).c_str(), tr);
        const char* totalLabel = f.perMinute ? "PER MIN" : ind.agg == MatchStats::Agg::Ratio ? "RATE"
                               : ind.agg == MatchStats::Agg::Median ? "MEDIAN" : "TOTAL";
        const float tw = TextSpaced(nullptr, t, ImVec2(), 0, totalLabel, tr);
        TextSpaced(dl, t, ImVec2(R(xValueR - tw), R(y)), v48::Labels, totalLabel, tr);
        y += LineH(9) + Px(8);
        HRule(dl, x, x + w, y, v48::LabelsRule);
        y += 1.0f;
    }

    // Every player, on one scale.
    const bool desc = StatsTable::SortDescending();
    const auto order = RankedPlayers(d, col, desc);
    double mx = 0.0;
    for (int pid : order) if (col.players.at(pid).has) mx = std::max(mx, std::fabs(col.players.at(pid).v));
    if (ind.agg == MatchStats::Agg::Ratio) mx = 1.0;
    const float rowH = Px(35);
    int rank = 0;
    for (int pid : order)
    {
        ++rank;
        const auto& ag = d.agents.at(pid);
        const auto& c = col.players.at(pid);
        const int side = Team::Index01(ag.team);
        const ImVec2 a(x, R(y)), b(x + w, R(y) + rowH);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(pid);
        const bool clicked = ImGui::InvisibleButton("##rank", ImVec2(w, rowH));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool sel = pid == s_sp.agent;
        if (sel)
        {
            dl->AddRectFilledMultiColor(a, b, v48::SelA, v48::SelB, v48::SelB, v48::SelA);
            dl->AddRectFilled(a, ImVec2(a.x + Px(2), b.y), v48::Gold);
        }
        else if (hov) dl->AddRectFilled(a, b, v48::HoverBg);
        if (clicked) s_sp.agent = pid;
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        HRule(dl, a.x, b.x, b.y - 1.0f, v48::RowRule);
        const float cy = a.y + rowH * 0.5f;
        Text(dl, T(10), ImVec2(xNum, R(cy - LineH(10) * 0.5f)), v48::RowNumber, std::to_string(rank).c_str());
        float nx = xPlayer;
        if (ImTextureID pt = AgentIcon(d, *f.host, pid))
        {
            dl->AddImage(pt, ImVec2(nx, R(cy - Px(8))), ImVec2(nx + Px(16), R(cy - Px(8)) + Px(16)));
        }
        nx += Px(16) + Px(9);
        TextFit(dl, T(12), ImVec2(nx, R(cy - LineH(12) * 0.5f)), xPlayer + cPlayer - nx, v48::Name, ag.name.c_str());
        if (hov && ImGui::IsMouseHoveringRect(ImVec2(xPlayer, a.y), ImVec2(xPlayer + cPlayer, b.y))) f.host->PlayerTooltip(ag.name);
        TextFit(dl, T(11), ImVec2(xGuild, R(cy - LineH(11) * 0.5f)), cGuild, side == 1 ? v48::RedTag : v48::BlueTag, SideTag(d, std::max(0, side)).c_str());
        if (c.has && mx > 0.0)
        {
            const float frac = (float)std::clamp(std::fabs(c.v) / mx, 0.0, 1.0);
            if (frac > 0.0f)
                dl->AddRectFilled(ImVec2(xTrack, R(cy - Px(3.5f))), ImVec2(R(xTrack + (xTrackEnd - xTrack) * frac), R(cy - Px(3.5f)) + Px(7)),
                                  side == 1 ? v48::RedBar : v48::BlueBar, Px(1));
        }
        const std::string v = StatsTable::Format(ind.key, c, f.perMinute, true);
        TextRight(dl, T(12), xValueR, R(cy - LineH(12) * 0.5f), c.has ? v48::Value : v48::NoValue, v.c_str());
        y += rowH;
    }

    // Footer.
    y += Px(13);
    HRule(dl, x, x + w, y, v48::Rule);
    y += 1.0f + Px(12);
    Text(dl, T(10), ImVec2(x, R(y)), v48::Footer, "Bars compare absolute values; they are not performance scores.");
    char n[32];
    snprintf(n, sizeof n, "%d players", (int)order.size());
    TextRight(dl, T(10), x + w, R(y), v48::Footer, n);
    y += LineH(10);
    return y;
}

// ─── Explore, right: the selected player's breakdown ────────────────────────────────────────

struct Kpi
{
    std::string label, value, unit, note;
};

static float DrawKpis(ImDrawList* dl, const std::vector<Kpi>& kpis, float x, float w, float y)
{
    HRule(dl, x, x + w, y, v48::KpiRule);
    y += 1.0f + Px(16);
    const float gap = Px(16);
    const int n = std::max(1, (int)kpis.size());
    const float cw = (w - gap * 3.0f) / 4.0f;
    const Type tl = T(11), tv = T(27), tu = T(13), tn = T(10);
    for (int i = 0; i < n && i < 4; ++i)
    {
        const Kpi& k = kpis[i];
        const float cx = x + i * (cw + gap);
        TextFit(dl, tl, ImVec2(cx, R(y)), cw, v48::KpiLabel, k.label.c_str());
        const float vy = R(y + LineH(11) + Px(8));
        const float vw = TextFit(dl, tv, ImVec2(cx, vy), cw, v48::KpiValue, k.value.c_str());
        if (!k.unit.empty())
            Text(dl, tu, ImVec2(cx + vw + Px(2), R(vy + Ascent(27) - Ascent(13))), v48::KpiValue, k.unit.c_str());
        TextFit(dl, tn, ImVec2(cx, R(vy + LineH(27) + Px(4))), cw, v48::KpiNote, k.note.c_str());
    }
    y += LineH(11) + Px(8) + LineH(27) + Px(4) + LineH(10) + Px(16);
    HRule(dl, x, x + w, y, v48::KpiRule);
    return y + 1.0f;
}

static float DetailHeading(ImDrawList* dl, float x, float w, float y, const char* title, const char* sub)
{
    y += Px(23);
    TextFit(dl, T(14, true), ImVec2(x, R(y)), w, v48::DetailTitle, title);
    y += LineH(14) + Px(6);
    if (sub && *sub) y += TextWrapped(dl, T(11), ImVec2(x, R(y)), w, Px(16.5f), v48::DetailSub, sub, 2);
    return y + Px(12);
}

// A row of a "which skills" list: the icon (32 px), the name, then up to two right-hand figures,
// each a value over a small caption.
struct DetailFigure { std::string value, caption; ImU32 col = 0; };

static float DetailRow(ImDrawList* dl, BattleLedger::Host& host, float x, float w, float y, ImTextureID icon, int skillForTip,
                       const std::string& name, const std::string& note, const DetailFigure& a, const DetailFigure& b)
{
    const float pad = Px(9), ic = Px(32);
    const float rowH = pad * 2.0f + std::max(ic, LineH(12) + Px(4) + LineH(10));
    const float cy = y + rowH * 0.5f;
    const ImVec2 ia(x, R(cy - ic * 0.5f)), ib(x + ic, R(cy - ic * 0.5f) + ic);
    if (icon) dl->AddImage(icon, ia, ib);
    else dl->AddRectFilled(ia, ib, Hex(0x2a2823), Px(2));
    if (skillForTip > 0 && ImGui::IsMouseHoveringRect(ia, ib) && ImGui::IsWindowHovered()) DrawSkillTooltip(skillForTip, nullptr);
    const float colB = Px(70), colA = Px(110), gap = Px(12);
    const float xB = x + w, xA = xB - colB - gap;
    const float nameX = x + ic + gap, nameW = xA - colA - gap - nameX;
    const float nw = TextFit(dl, T(12), ImVec2(nameX, R(cy - LineH(12) * 0.5f)), nameW, v48::Text, name.c_str());
    if (!note.empty()) TextFit(dl, T(11), ImVec2(nameX + nw + Px(8), R(cy - LineH(11) * 0.5f)), nameW - nw - Px(8), v48::Small, note.c_str());
    (void)host;
    auto fig = [&](const DetailFigure& f, float xr) {
        if (f.value.empty()) return;
        const float vy = R(cy - (LineH(12) + Px(4) + LineH(10)) * 0.5f);
        TextRight(dl, T(12), xr, vy, f.col ? f.col : v48::Text, f.value.c_str());
        TextRight(dl, T(10), xr, R(vy + LineH(12) + Px(4)), v48::Small, f.caption.c_str());
    };
    fig(a, xA);
    fig(b, xB);
    HRule(dl, x, x + w, y + rowH - 1.0f, v48::DetailRule);
    return y + rowH;
}

// A line of a "who" list: a name and its figure, a thin bar under them.
static float TargetLine(ImDrawList* dl, const BattleLedger::Data& d, BattleLedger::Host& host, float x, float w, float y, int agent,
                        const std::string& value, float frac, ImU32 bar)
{
    y += Px(13);
    const Type t = T(12);
    float nx = x;
    if (ImTextureID ic = AgentIcon(d, host, agent))
    {
        dl->AddImage(ic, ImVec2(nx, R(y)), ImVec2(nx + LineH(12), R(y) + LineH(12)));
        nx += LineH(12) + Px(6);
    }
    const float vw = TextW(t, value.c_str());
    TextFit(dl, t, ImVec2(nx, R(y)), x + w - vw - Px(10) - nx, v48::Text, AgentName(d, agent).c_str());
    TextRight(dl, t, x + w, R(y), v48::TargetValue, value.c_str());
    y += LineH(12) + Px(7);
    dl->AddRectFilled(ImVec2(x, R(y)), ImVec2(x + w, R(y) + Px(3)), v48::MiniTrack);
    if (frac > 0.0f) dl->AddRectFilled(ImVec2(x, R(y)), ImVec2(R(x + w * std::min(1.0f, frac)), R(y) + Px(3)), bar);
    return y + Px(3);
}

// A line of a "skills" list in a column: the icon (25 px), the name, the count.
static float SkillLine(ImDrawList* dl, BattleLedger::Host& host, float x, float w, float y, int skill, const std::string& value)
{
    y += Px(11);
    const float ic = Px(25);
    const ImVec2 a(x, R(y)), b(x + ic, R(y) + ic);
    if (ImTextureID t = skill > 0 ? host.SkillIcon(skill) : nullptr) dl->AddImage(t, a, b);
    else dl->AddRectFilled(a, b, Hex(0x2a2823), Px(2));
    if (skill > 0 && ImGui::IsMouseHoveringRect(a, b) && ImGui::IsWindowHovered()) DrawSkillTooltip(skill, nullptr);
    const Type t = T(12);
    const float vw = TextW(t, value.c_str());
    TextFit(dl, t, ImVec2(x + ic + Px(9), R(y + (ic - LineH(12)) * 0.5f)), w - ic - Px(9) - vw - Px(10),
            v48::Text, skill > 0 ? host.SkillName(skill).c_str() : "Unknown skill");
    TextRight(dl, t, x + w, R(y + (ic - LineH(12)) * 0.5f), v48::Text, value.c_str());
    return y + ic;
}

static float ContextNote(ImDrawList* dl, float x, float w, float y, const std::string& text)
{
    if (text.empty()) return y;
    y += Px(20);
    const Type t = T(11);
    const auto lines = WrapLines(t, text, w - Px(28), 6);
    const float h = Px(24) + Px(17.6f) * lines.size();
    dl->AddRectFilled(ImVec2(x, R(y)), ImVec2(x + w, R(y + h)), v48::NoteBg);
    dl->AddRectFilled(ImVec2(x, R(y)), ImVec2(x + Px(2), R(y + h)), v48::NoteLine);
    float ly = y + Px(12);
    for (const auto& l : lines)
    {
        Text(dl, t, ImVec2(x + Px(14), R(ly + (Px(17.6f) - LineH(11)) * 0.5f)), v48::NoteText, l.c_str());
        ly += Px(17.6f);
    }
    return y + h + Px(11);
}

static ImU32 SideBar(int team) { return Team::IsRed(team) ? Hex(0xd1978f) : Hex(0x8fb8d6); }

// A player's place among those with a value: "3 of 16", highest first.
static std::string PlaceText(const StatsTable::Column& col, int agent)
{
    const auto& me = col.players.at(agent);
    if (!me.has) return "\xE2\x80\x94";
    int place = 1, n = 0;
    for (const auto& [pid, c] : col.players)
    {
        if (!c.has) continue;
        ++n;
        if (c.v > me.v) ++place;
    }
    return std::to_string(place) + " of " + std::to_string(n);
}

// The player's share of their guild's figure (a sum only).
static std::string GuildShare(const StatsTable::Column& col, int agent, int side)
{
    if (!col.additive || side < 0) return "\xE2\x80\x94";
    const auto& me = col.players.at(agent);
    return me.has ? PercentText(me.v, col.team[side].v) : "\xE2\x80\x94";
}

// ── Breakdown: interrupts landed, and interrupts suffered ──

static float BreakdownInterrupts(ImDrawList* dl, StatsFrame& f, const StatsTable::Indicator& ind, const StatsTable::Column& col,
                                 int me, int side, float x, float w, float y)
{
    const BattleLedger::Data& d = *f.d;
    BattleLedger::Host& host = *f.host;
    const bool recv = ind.ledgerMetric == BattleLedger::kIntsRecv;
    int att = 0, landed = 0, faked = 0, missed = 0;
    std::vector<int> ms;
    struct SkillRow { int att = 0, landed = 0, faked = 0; };
    std::map<int, SkillRow> bySkill;          // giving: the interrupting skill; suffered: the skill lost
    std::map<int, int> byFoe, otherSkills;    // giving: foe -> landed, skill stopped -> times; suffered: who, their skill
    for (const BattleLedger::Interrupt& li : d.ints)
    {
        if (li.t < f.r0 || li.t > f.r1 || (recv ? li.dst : li.src) != me) continue;
        ++att;
        const bool l = li.outcome == BattleLedger::Interrupt::Landed, fk = li.outcome == BattleLedger::Interrupt::Faked;
        landed += l; faked += fk; missed += !l && !fk;
        SkillRow& r = bySkill[recv ? li.lostSkill : li.skillId];
        r.att++; r.landed += l; r.faked += fk;
        if (l)
        {
            byFoe[recv ? li.src : li.dst]++;
            if (recv) otherSkills[li.skillId]++;
            else if (li.lostSkill > 0) otherSkills[li.lostSkill]++;
            const int v = recv ? li.afterMs : li.reflexMs;
            if (v >= 0) ms.push_back(v);
        }
    }
    std::sort(ms.begin(), ms.end());
    const int med = ms.empty() ? -1 : (ms.size() % 2 ? ms[ms.size() / 2] : (ms[ms.size() / 2 - 1] + ms[ms.size() / 2]) / 2);
    const auto& cell = col.players.at(me);
    char buf[64];

    std::vector<Kpi> k;
    if (!recv)
    {
        snprintf(buf, sizeof buf, f.perMinute ? "per minute \xC2\xB7 %d of %d attempts" : "of %d attempts", f.perMinute ? landed : att, att);
        k.push_back({ "Interrupts landed", StatsTable::Format(ind.key, cell, f.perMinute), "", buf });
        k.push_back({ "Accuracy", PercentText(landed, att), "", "landed \xC3\xB7 attempts" });
        k.push_back({ "Median reaction", med >= 0 ? std::to_string(med) : "\xE2\x80\x94", med >= 0 ? " ms" : "", "on successful interrupts" });
        k.push_back({ "Guild contribution", GuildShare(col, me, side), "", "of guild interrupts" });
    }
    else
    {
        snprintf(buf, sizeof buf, f.perMinute ? "per minute \xC2\xB7 %d of %d attempts on them" : "of %d attempts on them",
                 f.perMinute ? landed : att, att);
        k.push_back({ "Interrupted", StatsTable::Format(ind.key, cell, f.perMinute), "", buf });
        k.push_back({ "Baited", std::to_string(faked), "", "cast cancelled just in time" });
        k.push_back({ "Median depth", med >= 0 ? std::to_string(med) : "\xE2\x80\x94", med >= 0 ? " ms" : "", "into the lost cast" });
        k.push_back({ "Share of the guild", GuildShare(col, me, side), "", "of the guild's casts interrupted" });
    }
    y += Px(18);
    y = DrawKpis(dl, k, x, w, y);

    // Landed, faked, missed: one bar in three parts.
    y += Px(22) + Px(12);
    if (att > 0)
    {
        const float gap = Px(3), avail = w - gap * 2.0f;
        const int parts[3] = { landed, faked, missed };
        const ImU32 cols[3] = { v48::Landed, v48::Faked, v48::Missed };
        float bx = x;
        for (int i = 0; i < 3; ++i)
        {
            if (!parts[i]) continue;
            const float bw = avail * parts[i] / (float)att;
            dl->AddRectFilled(ImVec2(R(bx), R(y)), ImVec2(R(bx + bw), R(y) + Px(5)), cols[i]);
            bx += bw + gap;
        }
    }
    else dl->AddRectFilled(ImVec2(x, R(y)), ImVec2(x + w, R(y) + Px(5)), v48::MiniTrack);
    y += Px(5) + Px(12);
    {
        const Type t = T(11);
        float cx = x;
        char s[64];
        const char* fakedWord = recv ? "baited away" : "faked by target";
        const int vals[3] = { landed, faked, missed };
        const char* words[3] = { recv ? "interrupted" : "landed", fakedWord, "missed" };
        for (int i = 0; i < 3; ++i)
        {
            snprintf(s, sizeof s, "%d %s", vals[i], words[i]);
            Text(dl, t, ImVec2(cx, R(y)), v48::Caption, s);
            cx += TextW(t, s) + Px(24);
        }
        y += LineH(11);
    }

    // The skills.
    y = DetailHeading(dl, x, w, y, recv ? "Which skills were interrupted?" : "Which skills created the interrupts?",
                      recv ? "Each row shows the casts lost out of the attempts on them, and the ones baited away."
                           : "Each row shows successful interrupts out of attempts, and accuracy.");
    std::vector<std::pair<int, SkillRow>> rows(bySkill.begin(), bySkill.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.landed != b.second.landed ? a.second.landed > b.second.landed : a.second.att > b.second.att; });
    if (rows.empty())
    {
        Text(dl, T(12), ImVec2(x, R(y)), v48::Muted, recv ? "No interrupt attempts on this player in the counted time." : "No interrupt attempts in the counted time.");
        y += LineH(12);
    }
    for (size_t i = 0; i < rows.size() && i < 8; ++i)
    {
        const auto& [skill, r] = rows[i];
        snprintf(buf, sizeof buf, "%d / %d", r.landed, r.att);
        const DetailFigure fa{ buf, recv ? "lost / attempts" : "landed / attempts" };
        const DetailFigure fb = recv ? DetailFigure{ std::to_string(r.faked), "baited" } : DetailFigure{ PercentText(r.landed, r.att), "accuracy" };
        y = DetailRow(dl, host, x, w, y, skill > 0 ? host.SkillIcon(skill) : nullptr, skill, skill > 0 ? host.SkillName(skill) : "Unknown skill",
                      "", fa, fb);
    }
    if (rows.size() > 8)
    {
        snprintf(buf, sizeof buf, "+ %d more skills", (int)rows.size() - 8);
        y += Px(8);
        Text(dl, T(11), ImVec2(x, R(y)), v48::Small, buf);
        y += LineH(11);
    }

    // Who, and which skills, side by side.
    y += Px(5);
    const float gap = Px(28), cw = (w - gap) * 0.5f;
    float yl = DetailHeading(dl, x, cw, y, recv ? "Who interrupted them?" : "Who was interrupted?",
                             recv ? "Successful interrupts by opponent." : "Successful interrupts by opponent.");
    std::vector<std::pair<int, int>> foes;
    for (const auto& [id, n] : byFoe) foes.push_back({ n, id });
    std::sort(foes.rbegin(), foes.rend());
    const int foeMax = foes.empty() ? 1 : foes.front().first;
    for (size_t i = 0; i < foes.size() && i < 6; ++i)
        yl = TargetLine(dl, d, host, x, cw, yl, foes[i].second, std::to_string(foes[i].first), foes[i].first / (float)foeMax, v48::Landed);
    if (foes.empty()) { Text(dl, T(12), ImVec2(x, R(yl)), v48::Muted, "None in the counted time."); yl += LineH(12); }
    float yr = DetailHeading(dl, x + cw + gap, cw, y, recv ? "Interrupting skills" : "Most stopped skills",
                             recv ? "The skills that stopped their casts." : "Skills prevented from completing.");
    std::vector<std::pair<int, int>> sk;
    for (const auto& [id, n] : otherSkills) sk.push_back({ n, id });
    std::sort(sk.rbegin(), sk.rend());
    for (size_t i = 0; i < sk.size() && i < 6; ++i)
        yr = SkillLine(dl, host, x + cw + gap, cw, yr, sk[i].second, std::to_string(sk[i].first));
    if (sk.empty()) { Text(dl, T(12), ImVec2(x + cw + gap, R(yr)), v48::Muted, "None in the counted time."); yr += LineH(12); }
    y = std::max(yl, yr);
    return ContextNote(dl, x, w, y + Px(8),
                       recv ? "Depth measures how far into their cast the interrupt landed. Baited: the foe's interrupt was wasted because they cancelled the cast just in time. Context, not a player rating."
                            : "Reaction measures the time from the opponent starting a cast to this player activating the interrupt. It is context, not a player rating.");
}

// ── Breakdown: damage, healing, prevention (the Battle Ledger's lines) ──

static std::string LedgerRowName(BattleLedger::Host& host, int key)
{
    using namespace BattleLedger;
    if (key == 657) return "Life Draining";      // the vampiric weapon's hidden skill
    if (key > 0) return host.SkillName(key);
    if (IsMapRow(key)) return MapRowName(key);
    if (IsDegenRow(key))
    {
        std::string n = ConditionLabel(DegenCondition(key));
        if (DegenCause(key) > 0) n += " from " + host.SkillName(DegenCause(key));
        return n;
    }
    return key == -1 ? "Weapon hits" : key == -2 ? "Divine Favor bonus" : key == kRowMinions ? "Minions" : key == kRowPets ? "Pets"
         : key == kRowDegenUnknown ? "Degeneration (source unknown)" : "No skill named";
}

// A line's row in a breakdown: its own row (minions, degeneration, a map effect), the Divine Favor
// bonus (-2), its skill, weapon hits (-1), or nothing named (-3).
static int LedgerRowKey(const BattleLedger::Line& ln)
{
    return ln.rowKey ? ln.rowKey : ln.kind == BattleLedger::Kind::DivineFavor ? -2 : ln.skillId > 0 ? ln.skillId : ln.weaponHit ? -1 : -3;
}

static ImTextureID LedgerRowIcon(BattleLedger::Host& host, int key, int iconSkill)
{
    using namespace BattleLedger;
    if (IsMapRow(key)) return MapRowIcon(host, key);
    if (IsDegenRow(key))
        if (ImTextureID t = host.SkillIcon(DegenCondition(key))) return t;
    if (iconSkill > 0) return host.SkillIcon(iconSkill);
    if (key > 0) return host.SkillIcon(key);
    if (key == -1 || key == kRowMinions || key == kRowPets) return host.UiIcon("kill.png");
    if (key == -2) return host.ProfIcon(3);
    return nullptr;
}

static float BreakdownLedger(ImDrawList* dl, StatsFrame& f, const StatsTable::Indicator& ind, const StatsTable::Column& col,
                             int me, int side, float x, float w, float y)
{
    using namespace BattleLedger;
    const Data& d = *f.d;
    Host& host = *f.host;
    const int metric = ind.ledgerMetric;
    const bool taker = !GivingMetric(metric);
    struct Row { int key = 0, icon = 0; long long amount = 0; bool est = false; int capped = 0, blocked = 0; };
    std::map<int, Row> rows;
    std::map<int, long long> byOther;
    long long sum = 0, estimated = 0;
    for (const Line& ln : d.lines)
    {
        if (ln.t < f.r0) continue;
        if (ln.t > f.r1) break;
        const int m = taker ? LineMetricTaker(ln.kind) : LineMetricGiver(ln.kind);
        if (m != metric || (taker ? ln.dst : ln.src) != me) continue;
        if (metric == kTaken && ln.kind == Kind::SelfCost) continue;    // sacrifices are their own column
        const int key = LedgerRowKey(ln);
        Row& r = rows[key];
        r.key = key;
        if (ln.rowKey && ln.skillId > 0) r.icon = ln.skillId;
        r.amount += ln.amount;
        if (ln.estimated) { r.est = true; estimated += ln.amount; }
        if (PreventCapped(ln)) r.capped += ln.count;
        if (PreventBlocked(ln)) r.blocked += ln.count;
        const int other = taker ? (IsMapRow(ln.rowKey) || ln.rowKey == kRowDegenUnknown ? ln.rowKey : ln.src)
                                : (IsMapRow(ln.rowKey) ? ln.rowKey : ln.dst);
        byOther[other] += ln.amount;
        sum += ln.amount;
    }
    const auto& cell = col.players.at(me);
    const float minutes = std::max(1.0f / 60.0f, (f.r1 - f.r0) / 60.0f);
    char buf[96];
    StatsTable::Cell alt;
    alt.has = true;
    alt.v = f.perMinute ? (double)sum : sum / minutes;
    int players = 0;
    for (const auto& [id, a] : byOther) if (d.agents.count(id) && a > 0) ++players;

    const char* noun = metric == kDealt ? "damage" : metric == kTaken ? "damage taken" : metric == kHealed ? "healing"
                     : metric == kReceived ? "healing received" : "damage prevented";
    std::vector<Kpi> k;
    k.push_back({ ind.label, StatsTable::Format(ind.key, cell, f.perMinute), "", f.perMinute ? "Health per minute" : "Health, " + f.scope });
    k.push_back({ f.perMinute ? "Total" : "Per minute", StatsTable::Format(ind.key, alt, !f.perMinute), "", f.perMinute ? "Health in the counted time" : "Health per minute" });
    snprintf(buf, sizeof buf, "of the guild's %s", noun);
    k.push_back({ taker ? "Share of the guild" : "Guild contribution", GuildShare(col, me, side), "", buf });
    if (metric == kPrevented)
        k.push_back({ "Estimated part", sum > 0 ? "~" + PercentText((double)estimated, (double)sum) : "\xE2\x80\x94", "", "capped hits and blocked attacks" });
    else
        k.push_back({ taker ? "From" : metric == kDealt ? "Foes hit" : "Allies reached", std::to_string(players), "",
                      taker ? "players (map effects apart)" : "players, every skill together" });
    y += Px(18);
    y = DrawKpis(dl, k, x, w, y);

    // The skills.
    const char* title = metric == kDealt ? "Which skills dealt the damage?" : metric == kTaken ? "What hit them?"
                      : metric == kHealed ? "Which skills healed?" : metric == kReceived ? "What healed them?" : "Which skills prevented damage?";
    y = DetailHeading(dl, x, w, y + Px(10), title,
                      metric == kPrevented ? "Each row shows the Health spared and its share; ~ marks an estimate (a capped hit or a blocked attack)."
                                           : "Each row shows the amount and its share of this player's total.");
    std::vector<const Row*> order;
    for (const auto& [key, r] : rows) order.push_back(&r);
    std::sort(order.begin(), order.end(), [](const Row* a, const Row* b) { return a->amount > b->amount; });
    if (order.empty())
    {
        Text(dl, T(12), ImVec2(x, R(y)), v48::Muted, "Nothing in the counted time.");
        y += LineH(12);
    }
    for (size_t i = 0; i < order.size() && i < 8; ++i)
    {
        const Row& r = *order[i];
        std::string note;
        if (r.capped) note = std::to_string(r.capped) + " capped";
        if (r.blocked) note += (note.empty() ? "" : ", ") + std::to_string(r.blocked) + " blocked";
        StatsTable::Cell c;
        c.has = true;
        c.v = (double)r.amount;
        const DetailFigure fa{ (r.est ? "~" : "") + StatsTable::Format(ind.key, c, false), "Health" };
        const DetailFigure fb{ PercentText((double)r.amount, (double)sum), "share" };
        y = DetailRow(dl, host, x, w, y, LedgerRowIcon(host, r.key, r.icon), r.key > 0 ? r.key : 0, LedgerRowName(host, r.key), note, fa, fb);
    }
    if (order.size() > 8)
    {
        snprintf(buf, sizeof buf, "+ %d more", (int)order.size() - 8);
        y += Px(8);
        Text(dl, T(11), ImVec2(x, R(y)), v48::Small, buf);
        y += LineH(11);
    }

    // Who: the players (and map effects) it went to or came from; for damage dealt, its kinds.
    std::vector<std::pair<long long, int>> others;
    for (const auto& [id, a] : byOther) if (a > 0) others.push_back({ a, id });
    std::sort(others.rbegin(), others.rend());
    const long long omax = others.empty() ? 1 : others.front().first;
    const char* who = metric == kDealt ? "Who was hit?" : metric == kTaken ? "Who hit them?" : metric == kHealed ? "Who was healed?"
                    : metric == kReceived ? "Who healed them?" : "Who was protected?";
    y += Px(5);
    const float gap = Px(28), cw = (w - gap) * 0.5f;
    const ImU32 bar = SideBar(AgentTeam(d, me));
    auto amountText = [&](long long a) { StatsTable::Cell c; c.has = true; c.v = (double)a; return StatsTable::Format(ind.key, c, false); };
    if (metric == kDealt)
    {
        float yl = DetailHeading(dl, x, cw, y, who, "By player, every skill together.");
        for (size_t i = 0; i < others.size() && i < 6; ++i)
            yl = TargetLine(dl, d, host, x, cw, yl, others[i].second, amountText(others[i].first), others[i].first / (float)omax, bar);
        // The kinds: the table's own sub-columns, so they add up to Damage dealt.
        float yr = DetailHeading(dl, x + cw + gap, cw, y, "Damage by kind", "Attacks, skills, hexes, conditions and summons.");
        const char* kinds[5][2] = { { "base:dmg_attack", "Attacks" }, { "base:dmg_skill", "Skills" }, { "base:dmg_hex", "Hexes" },
                                    { "base:dmg_cond", "Conditions" }, { "base:dmg_minion", "Minions, spirits & pets" } };
        double kv[5] = {}, kmax = 1.0;
        for (int i = 0; i < 5; ++i)
        {
            const auto kc = StatsTable::Values(f.data, kinds[i][0], f.r0, f.r1, false);
            if (auto it = kc.players.find(me); it != kc.players.end()) kv[i] = it->second.v;
            kmax = std::max(kmax, kv[i]);
        }
        for (int i = 0; i < 5; ++i)
        {
            yr += Px(13);
            const Type t = T(12);
            StatsTable::Cell c; c.has = true; c.v = kv[i];
            const std::string v = StatsTable::Format("base:dealt", c, false);
            Text(dl, t, ImVec2(x + cw + gap, R(yr)), v48::Text, kinds[i][1]);
            TextRight(dl, t, x + w, R(yr), v48::TargetValue, v.c_str());
            yr += LineH(12) + Px(7);
            dl->AddRectFilled(ImVec2(x + cw + gap, R(yr)), ImVec2(x + w, R(yr) + Px(3)), v48::MiniTrack);
            if (kv[i] > 0.0) dl->AddRectFilled(ImVec2(x + cw + gap, R(yr)), ImVec2(R(x + cw + gap + cw * (float)(kv[i] / kmax)), R(yr) + Px(3)), bar);
            yr += Px(3);
        }
        y = std::max(yl, yr);
    }
    else
    {
        y = DetailHeading(dl, x, w, y, who, "By player, every skill together.");
        float yl = y, yr = y;
        for (size_t i = 0; i < others.size() && i < 12; ++i)
        {
            float& yy = i % 2 ? yr : yl;
            yy = TargetLine(dl, d, host, x + (i % 2) * (cw + gap), cw, yy, others[i].second, amountText(others[i].first),
                            others[i].first / (float)omax, bar);
        }
        if (others.empty()) { Text(dl, T(12), ImVec2(x, R(y)), v48::Muted, "Nobody in the counted time."); yl += LineH(12); }
        y = std::max(yl, yr);
    }
    std::string note = ind.meaning;
    if (metric == kPrevented)
        note += " Estimates: a capped hit counts the attacker's median uncapped hit with the same skill; a blocked attack the attacker's median hit with it.";
    return ContextNote(dl, x, w, y + Px(8), note);
}

// ── Breakdown: knockdowns blocked (the catalogue's kd_prevented and kd_avoided records) ──

static float BreakdownKnockdownsBlocked(ImDrawList* dl, StatsFrame& f, const StatsTable::Indicator& ind, const StatsTable::Column& col,
                                        int me, int side, float x, float w, float y)
{
    const BattleLedger::Data& d = *f.d;
    BattleLedger::Host& host = *f.host;
    int onAllies = 0, onSelf = 0;
    std::map<int, int> bySkill, byAlly, byKd;
    auto scan = [&](const char* key, bool self) {
        if (!f.data.stats) return;
        auto it = f.data.stats->find(key);
        if (it == f.data.stats->end()) return;
        for (const MatchStats::Record& r : it->second)
        {
            if (r.t < f.r0 || r.t > f.r1 || r.agent != me) continue;
            (self ? onSelf : onAllies)++;
            bySkill[r.skill]++;
            byAlly[self ? me : r.other]++;
            if (r.flag) byKd[std::abs(r.flag)]++;
        }
    };
    scan("kd_prevented", false);
    scan("kd_avoided", true);
    const auto& cell = col.players.at(me);
    std::vector<Kpi> k;
    k.push_back({ "Knockdowns blocked", StatsTable::Format(ind.key, cell, f.perMinute), "", f.perMinute ? "per minute" : "in the counted time" });
    k.push_back({ "On allies", std::to_string(onAllies), "", "their stability on an ally" });
    k.push_back({ "On themselves", std::to_string(onSelf), "", "their own stance or enchantment" });
    k.push_back({ "Guild contribution", GuildShare(col, me, side), "", "of the guild's blocks" });
    y += Px(18);
    y = DrawKpis(dl, k, x, w, y);

    y = DetailHeading(dl, x, w, y + Px(10), "Which stability skills?", "Each row shows the knockdowns the skill stopped and its share.");
    std::vector<std::pair<int, int>> rows;
    for (const auto& [s, n] : bySkill) rows.push_back({ n, s });
    std::sort(rows.rbegin(), rows.rend());
    const int total = onAllies + onSelf;
    if (rows.empty()) { Text(dl, T(12), ImVec2(x, R(y)), v48::Muted, "No knockdown stopped in the counted time."); y += LineH(12); }
    for (const auto& [n, s] : rows)
        y = DetailRow(dl, host, x, w, y, s > 0 ? host.SkillIcon(s) : nullptr, s, s > 0 ? host.SkillName(s) : "Unknown skill", "",
                      { std::to_string(n), "blocked" }, { PercentText(n, total), "share" });

    y += Px(5);
    const float gap = Px(28), cw = (w - gap) * 0.5f;
    float yl = DetailHeading(dl, x, cw, y, "Who was protected?", "Knockdowns stopped on each player.");
    std::vector<std::pair<int, int>> allies;
    for (const auto& [id, n] : byAlly) allies.push_back({ n, id });
    std::sort(allies.rbegin(), allies.rend());
    const int amax = allies.empty() ? 1 : allies.front().first;
    for (size_t i = 0; i < allies.size() && i < 6; ++i)
        yl = TargetLine(dl, d, host, x, cw, yl, allies[i].second, std::to_string(allies[i].first), allies[i].first / (float)amax, SideBar(AgentTeam(d, me)));
    if (allies.empty()) { Text(dl, T(12), ImVec2(x, R(yl)), v48::Muted, "Nobody in the counted time."); yl += LineH(12); }
    float yr = DetailHeading(dl, x + cw + gap, cw, y, "Knockdowns stopped", "The foes' knockdown skills.");
    std::vector<std::pair<int, int>> kds;
    for (const auto& [s, n] : byKd) kds.push_back({ n, s });
    std::sort(kds.rbegin(), kds.rend());
    for (size_t i = 0; i < kds.size() && i < 6; ++i) yr = SkillLine(dl, host, x + cw + gap, cw, yr, kds[i].second, std::to_string(kds[i].first));
    if (kds.empty()) { Text(dl, T(12), ImVec2(x + cw + gap, R(yr)), v48::Muted, "None in the counted time."); yr += LineH(12); }
    y = std::max(yl, yr);
    return ContextNote(dl, x, w, y + Px(8), ind.meaning + (ind.method.empty() ? std::string() : " " + ind.method));
}

// ── Breakdown: any other indicator ──

static float BreakdownGeneric(ImDrawList* dl, StatsFrame& f, const StatsTable::Indicator& ind, const StatsTable::Column& col,
                              int me, int side, float x, float w, float y)
{
    using MatchStats::Agg;
    const BattleLedger::Data& d = *f.d;
    BattleLedger::Host& host = *f.host;
    const auto& cell = col.players.at(me);
    const std::string unit = StatsTable::UnitText(ind.key, f.perMinute);
    std::vector<Kpi> k;
    if (ind.agg == Agg::Ratio)
    {
        k.push_back({ ind.label, StatsTable::Format(ind.key, cell, f.perMinute, true), "", "of the tries" });
        k.push_back({ "Succeeded", cell.has ? std::to_string((int)cell.num) : "\xE2\x80\x94", "", "in the counted time" });
        k.push_back({ "Tries", cell.has ? std::to_string((int)cell.den) : "\xE2\x80\x94", "", "in the counted time" });
        k.push_back({ "Guild rate", side >= 0 ? StatsTable::Format(ind.key, col.team[side], f.perMinute, true) : "\xE2\x80\x94", "", "over every try of the guild" });
    }
    else if (ind.agg == Agg::Median)
    {
        k.push_back({ ind.label, StatsTable::Format(ind.key, cell, f.perMinute), "", "the median case" });
        k.push_back({ "Cases", std::to_string(cell.n), "", "in the counted time" });
        k.push_back({ "Guild median", side >= 0 ? StatsTable::Format(ind.key, col.team[side], f.perMinute) : "\xE2\x80\x94", "", "every case of the guild" });
        k.push_back({ "Place", PlaceText(col, me), "", "players, highest first" });
    }
    else
    {
        const auto other = StatsTable::Values(f.data, ind.key, f.r0, f.r1, !f.perMinute);
        const auto it = other.players.find(me);
        k.push_back({ ind.label, StatsTable::Format(ind.key, cell, f.perMinute), "", unit });
        k.push_back({ f.perMinute ? "Total" : "Per minute",
                      it != other.players.end() ? StatsTable::Format(ind.key, it->second, !f.perMinute) : "\xE2\x80\x94", "",
                      StatsTable::UnitText(ind.key, !f.perMinute) });
        k.push_back({ "Guild contribution", GuildShare(col, me, side), "", "of the guild's total" });
        k.push_back({ "Place", PlaceText(col, me), "", "players, highest first" });
    }
    y += Px(18);
    y = DrawKpis(dl, k, x, w, y);

    // What the indicator's own records name: the skills, the other players.
    const MatchStats::StatDef* def = nullptr;
    for (const auto& s : MatchStats::Catalog()) if (ind.key == s.key) { def = &s; break; }
    const char* recKey = def ? (def->agg == Agg::Ratio ? def->num : def->key) : nullptr;
    // Rows: a skill (> 0), or for a Health stat any ledger row (LedgerRowKey: weapon hits,
    // degeneration, minions...). rowIcon: the skill a special row shows (a condition's cause).
    std::map<int, double> bySkill, byOther;
    std::map<int, int> rowIcon;
    double total = 0.0;
    // A Health stat made of the ledger's own damage or heal lines (one record a line: the Lord's
    // and the guild NPCs' damage): each record finds its line, so the weapon hits, degeneration
    // and minions it counts get their rows, as in Damage dealt, instead of falling out of a list
    // of skills (a weapon hit's record names no skill).
    const bool healthStat = def && def->agg == Agg::Sum && (def->hue == MatchStats::Hue::Damage || def->hue == MatchStats::Hue::Healing);
    std::map<std::pair<int, int>, std::vector<const BattleLedger::Line*>> linesOf;   // (target, amount) -> my lines
    if (healthStat)
        for (const BattleLedger::Line& ln : d.lines)
        {
            if (ln.t < f.r0 - 0.05f) continue;
            if (ln.t > f.r1 + 0.05f) break;
            if (ln.src == me && ln.amount > 0) linesOf[{ ln.dst, ln.amount }].push_back(&ln);
        }
    auto lineFor = [&](const MatchStats::Record& r) -> const BattleLedger::Line* {
        auto it = linesOf.find({ r.other, (int)std::lround(r.value) });
        if (it == linesOf.end()) return nullptr;
        for (const BattleLedger::Line*& ln : it->second)
            if (ln && std::fabs(ln->t - r.t) < 0.01f && (r.skill <= 0 || ln->skillId == r.skill))
            {
                const BattleLedger::Line* found = ln;
                ln = nullptr;   // each line answers one record
                return found;
            }
        return nullptr;
    };
    if (recKey && f.data.stats)
        if (auto it = f.data.stats->find(recKey); it != f.data.stats->end())
            for (const MatchStats::Record& r : it->second)
            {
                if (r.agent != me) continue;
                double v = 1.0;
                if (def->agg == Agg::Time)
                {
                    const double a = std::max<double>(r.t, f.r0), b = std::min<double>(r.t + r.dur, f.r1);
                    if (b <= a) continue;
                    v = b - a;
                }
                else if (r.t < f.r0 || r.t > f.r1) continue;
                else if (def->agg == Agg::Sum) v = r.value;
                if (healthStat)
                {
                    const BattleLedger::Line* ln = lineFor(r);
                    const int key = ln ? LedgerRowKey(*ln) : r.skill > 0 ? r.skill : -3;
                    if (ln && ln->rowKey && ln->skillId > 0) rowIcon[key] = ln->skillId;
                    bySkill[key] += v;
                }
                else if (r.skill > 0) bySkill[r.skill] += v;
                if (r.other != 0) byOther[r.other] += v;
                total += v;
            }
    // The fixed columns that count skill uses: the skills, from the same uses.
    if (ind.key == "base:skills" || ind.key == "base:cancelled")
        if (auto uit = d.uses.find(me); uit != d.uses.end())
            for (const auto& u : uit->second)
            {
                if (u.start < f.r0 || u.start > f.r1) continue;
                const bool cancelled = u.cancelled && !u.interrupted;
                if (cancelled != (ind.key == "base:cancelled")) continue;
                bySkill[u.skillId] += 1.0;
                total += 1.0;
            }
    // A row's figure in the indicator's own unit (a sum with its thousands, seconds of time); the
    // records of a median or a ratio are counted.
    const bool ownUnit = def && (def->agg == Agg::Count || def->agg == Agg::Sum || def->agg == Agg::Time);
    auto valueText = [&](double v) {
        if (ownUnit)
        {
            StatsTable::Cell c;
            c.has = true;
            c.v = v;
            return StatsTable::Format(ind.key, c, false);
        }
        return std::to_string((int)std::lround(v));
    };
    const std::string caption = def && def->agg == Agg::Sum ? StatsTable::UnitText(ind.key, false)
                              : def && def->agg == Agg::Time ? std::string("seconds") : std::string("count");
    if (!bySkill.empty())
    {
        const bool named = std::all_of(bySkill.begin(), bySkill.end(), [](const auto& e) { return e.first > 0; });
        y = DetailHeading(dl, x, w, y + Px(10), named ? "Skills involved" : "Skills and hits involved",
                          healthStat ? "Each row shows the amount and its share; weapon hits and degeneration have their own rows."
                                     : "From this indicator's own records, over the counted time.");
        std::vector<std::pair<double, int>> rows;
        for (const auto& [s, v] : bySkill) rows.push_back({ v, s });
        std::sort(rows.rbegin(), rows.rend());
        for (size_t i = 0; i < rows.size() && i < 8; ++i)
        {
            const int key = rows[i].second;
            auto ic = rowIcon.find(key);
            y = DetailRow(dl, host, x, w, y, LedgerRowIcon(host, key, ic == rowIcon.end() ? 0 : ic->second), key > 0 ? key : 0,
                          LedgerRowName(host, key), "", { valueText(rows[i].first), caption }, { PercentText(rows[i].first, total), "share" });
        }
        if (rows.size() > 8)
        {
            char more[32];
            snprintf(more, sizeof more, "+ %d more", (int)rows.size() - 8);
            y += Px(8);
            Text(dl, T(11), ImVec2(x, R(y)), v48::Small, more);
            y += LineH(11);
        }
    }
    if (!byOther.empty())
    {
        y = DetailHeading(dl, x, w, y + Px(5), "Other players involved", "The other player each record names (a target, an ally, a foe...).");
        std::vector<std::pair<double, int>> rows;
        for (const auto& [id, v] : byOther) rows.push_back({ v, id });
        std::sort(rows.rbegin(), rows.rend());
        const double mx = rows.front().first;
        const float gap = Px(28), cw = (w - gap) * 0.5f;
        float yl = y, yr = y;
        for (size_t i = 0; i < rows.size() && i < 12; ++i)
        {
            float& yy = i % 2 ? yr : yl;
            yy = TargetLine(dl, d, host, x + (i % 2) * (cw + gap), cw, yy, rows[i].second, valueText(rows[i].first), (float)(rows[i].first / mx),
                            SideBar(AgentTeam(d, me)));
        }
        y = std::max(yl, yr);
    }
    // Deaths: when they happened.
    if (ind.key == "base:deaths")
    {
        std::vector<float> times;
        for (const auto& e : f.data.deaths) if (e.agent == me && e.t >= f.r0 && e.t <= f.r1) times.push_back(e.t);
        if (!times.empty())
        {
            y = DetailHeading(dl, x, w, y + Px(10), "When they died", "On the match clock.");
            std::string line;
            for (float t : times) line += (line.empty() ? "" : "   ") + ClockText(t);
            y += TextWrapped(dl, T(12), ImVec2(x, R(y)), w, LineH(12) + Px(4), v48::Text, line, 4);
        }
    }
    std::string note = ind.meaning;
    if (!ind.method.empty()) note += " How it is measured: " + ind.method;
    if (ind.estimated) note += " ~ marks a value estimated from a model.";
    return ContextNote(dl, x, w, y + Px(8), note);
}

// The player's bar: eight 34 px icons, 2 apart, in the order the Library's builds show them.
static void DrawInspectorBar(ImDrawList* dl, const MatchMeta& m, const PlayerMeta* p, ImVec2 pos)
{
    const float cell = Px(34), gap = Px(2);
    std::vector<int> skills;
    const SkillDatabaseView& view = GetMatchSkillView(m);
    if (p && !p->used_skills.empty())
    {
        const auto sorted = view.SortSkillsForDisplay(p->used_skills, p->primary, p->secondary);
        skills.assign(sorted.begin(), sorted.end());
    }
    for (int i = 0; i < 8; ++i)
    {
        const ImVec2 a(R(pos.x + (cell + gap) * i), R(pos.y)), b(a.x + cell, a.y + cell);
        const int id = i < (int)skills.size() ? skills[i] : 0;
        const ImTextureID tex = id ? GetSkillIcon(id, &view) : ImTextureID();
        if (tex) dl->AddImage(tex, a, b);
        else dl->AddRectFilled(a, b, Hex(0x1a1916, 0xb0));
        if (id && ImGui::IsMouseHoveringRect(a, b) && ImGui::IsWindowHovered()) DrawSkillTooltip(id, &view);
    }
}

static float DrawInspector(ImDrawList* dl, StatsFrame& f, const StatsTable::Indicator& ind, const StatsTable::Column& col,
                           float x, float w, float y)
{
    const BattleLedger::Data& d = *f.d;
    const int me = s_sp.agent;
    if (me < 0 || !d.agents.count(me) || !col.players.count(me)) return y;
    const BattleLedger::Agent& ag = d.agents.at(me);
    const int side = Team::Index01(ag.team);

    TextSpaced(dl, T(10), ImVec2(x, R(y)), v48::InspLabel, ("PLAYER BREAKDOWN \xC2\xB7 " + f.scopeUpper).c_str(), Px(1.4f));
    y += LineH(10) + Px(18);

    // The player: profession, name (22 px bold), their bar at the right (under it when narrow).
    {
        const float barW = Px(34) * 8.0f + Px(2) * 7.0f, rowH = Px(34);
        const Type tn = T(22, true);
        const float nameX = x + Px(23) + Px(10);
        const float nameW = TextW(tn, ag.name.c_str());
        const bool wrap = nameX + nameW + Px(20) + barW > x + w;
        if (ImTextureID pt = AgentIcon(d, *f.host, me))
            dl->AddImage(pt, ImVec2(x, R(y + (rowH - Px(23)) * 0.5f)), ImVec2(x + Px(23), R(y + (rowH - Px(23)) * 0.5f) + Px(23)));
        TextFit(dl, tn, ImVec2(nameX, R(y + (rowH - LineH(22)) * 0.5f)), x + w - nameX, v48::Text, ag.name.c_str());
        if (ImGui::IsMouseHoveringRect(ImVec2(nameX, y), ImVec2(nameX + nameW, y + rowH)) && ImGui::IsWindowHovered())
            f.host->PlayerTooltip(ag.name);
        const PlayerMeta* pm = PlayerByName(*f.m, ag.name);
        if (wrap)
        {
            y += rowH + Px(8);
            DrawInspectorBar(dl, *f.m, pm, ImVec2(x, y));
        }
        else DrawInspectorBar(dl, *f.m, pm, ImVec2(x + w - barW, y));
        y += rowH + Px(8);
    }
    const std::string guild = side >= 0 ? SideGuild(d, side) : std::string();
    Text(dl, T(12), ImVec2(x, R(y)), side == 1 ? v48::RedGuild : v48::BlueGuild, guild.c_str());
    y += LineH(12);

    if (ind.ledgerMetric == BattleLedger::kInts || ind.ledgerMetric == BattleLedger::kIntsRecv)
        return BreakdownInterrupts(dl, f, ind, col, me, side, x, w, y);
    if (ind.ledgerMetric >= 0)
        return BreakdownLedger(dl, f, ind, col, me, side, x, w, y);
    if (ind.key == "kd_blocked")
        return BreakdownKnockdownsBlocked(dl, f, ind, col, me, side, x, w, y);
    return BreakdownGeneric(dl, f, ind, col, me, side, x, w, y);
}

// ─── Explore ─────────────────────────────────────────────────────────────────────────────────

static float DrawExplore(ImDrawList* dl, StatsFrame& f, float x0, float x1, float y)
{
    StatsTable::Indicator ind;
    if (!StatsTable::Describe(s_sp.active, ind)) return y;
    const StatsTable::Column col = StatsTable::Values(f.data, ind.key, f.r0, f.r1, f.perMinute);

    // The selected player: kept across indicators and views; the first of the ranking otherwise.
    if (s_sp.agent < 0 || !col.players.count(s_sp.agent))
    {
        const auto order = RankedPlayers(*f.d, col, true);
        s_sp.agent = order.empty() ? -1 : order.front();
    }

    y += Px(19);
    const float W = x1 - x0, gap = Px(30);
    const bool stacked = W < Px(600) + gap + Px(650);
    const float leftW = stacked ? W : (W - gap) * 0.475f;
    const float yLeft = DrawMetricFocus(dl, f, ind, col, x0, leftW, y);
    float yRight = y;
    if (stacked)
    {
        const float top = yLeft + Px(30);
        HRule(dl, x0, x1, top, v48::InspLine);
        yRight = DrawInspector(dl, f, ind, col, x0, W, top + Px(24));
    }
    else
    {
        const float xr = x0 + leftW + gap;
        const float bottom = DrawInspector(dl, f, ind, col, xr + 1.0f + gap, x1 - (xr + 1.0f + gap), y);
        const float h = std::max(bottom, yLeft) - y;
        dl->AddRectFilledMultiColor(ImVec2(xr, y), ImVec2(x1, y + h), Hex(0xffffff, 0x02), Hex(0xffffff, 0), Hex(0xffffff, 0), Hex(0xffffff, 0x02));
        dl->AddRectFilled(ImVec2(xr, y), ImVec2(xr + 1.0f, y + h), v48::InspLine);
        yRight = bottom;
    }
    return std::max(yLeft, yRight);
}

// ─── Table ───────────────────────────────────────────────────────────────────────────────────

static float DrawStatsTablePage(ImDrawList* dl, StatsFrame& f, float x0, float x1, float y)
{
    const BattleLedger::Data& d = *f.d;
    const auto sel = StatsTable::Selection();
    y += Px(20);
    {
        char left[96];
        snprintf(left, sizeof left, "All players \xC2\xB7 %d selected indicator%s \xC2\xB7 click a column to sort", (int)sel.size(),
                 sel.size() == 1 ? "" : "s");
        Text(dl, T(11), ImVec2(x0, R(y)), v48::TableCaption, left);
        TextRight(dl, T(11), x1, R(y), v48::TableCaption, "Click a player, then Explore to inspect their breakdown");
        y += Px(17.6f) + Px(14);
    }

    // The columns: each indicator's values over the interval, the table's own.
    struct Col { std::string key; StatsTable::Indicator ind; StatsTable::Column values; float w = 0.f; };
    std::vector<Col> cols;
    const Type th = T(11), tc = T(12);
    for (const auto& key : sel)
    {
        Col c;
        c.key = key;
        if (!StatsTable::Describe(key, c.ind)) continue;
        c.values = StatsTable::Values(f.data, key, f.r0, f.r1, f.perMinute);
        c.w = std::max(Px(150), TextW(th, c.ind.label.c_str()) + Px(13) * 2.0f + Px(9) * 2.0f + Px(14));
        cols.push_back(std::move(c));
    }
    std::string sortKey = StatsTable::SortKey();
    const bool desc = StatsTable::SortDescending();
    const Col* sortCol = nullptr;
    for (const auto& c : cols) if (c.key == sortKey) sortCol = &c;
    if (!sortCol && !cols.empty()) { sortCol = &cols.front(); sortKey = cols.front().key; }

    // The rows: one list, sorted by the sort column in its direction, players without a value last.
    std::vector<int> rows;
    for (int pid : d.players) if (d.agents.count(pid)) rows.push_back(pid);
    if (sortCol)
        std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
            const auto& ca = sortCol->values.players.at(a);
            const auto& cb = sortCol->values.players.at(b);
            if (ca.has != cb.has) return ca.has;
            if (ca.v != cb.v) return desc ? ca.v > cb.v : ca.v < cb.v;
            return d.agents.at(a).name < d.agents.at(b).name;
        });

    const float rowH = Px(42), cellPad = Px(13);
    const float nameW = Px(260), guildW = Px(127);
    const float fixedW = nameW + guildW;
    float colsW = 0.0f;
    for (const auto& c : cols) colsW += c.w;
    const float availW = x1 - x0 - fixedW;
    if (colsW < availW && !cols.empty())
    {
        const float extra = (availW - colsW) / cols.size();
        for (auto& c : cols) c.w += extra;
        colsW = availW;
    }
    const bool scrolls = colsW > availW + 0.5f;
    const int teamRows = 2;
    const float bodyH = rowH * (1 + rows.size() + teamRows) + Px(6);
    const float barH = scrolls ? Px(10) : 0.0f;
    const float tableH = bodyH + barH;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const int hoverRow = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && mouse.x >= x0 && mouse.x <= x1
                         ? (int)std::floor((mouse.y - (y + rowH)) / rowH) : -1;

    // Player and guild: a part of their own, so the names stay put while the columns scroll.
    auto rowY = [&](int i) { return y + rowH * (1 + i); };
    {
        HRule(dl, x0, x0 + fixedW, y + rowH - 1.0f, v48::TableRule);
        Text(dl, th, ImVec2(x0 + cellPad, R(y + (rowH - LineH(11)) * 0.5f)), v48::TableHead, "PLAYER");
        TextRight(dl, th, x0 + fixedW - cellPad, R(y + (rowH - LineH(11)) * 0.5f), v48::TableHead, "GUILD");
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const int pid = rows[i];
            const auto& ag = d.agents.at(pid);
            const float ry = rowY((int)i);
            const ImVec2 a(x0, R(ry)), b(x0 + fixedW, R(ry) + rowH);
            const bool sel2 = pid == s_sp.agent;
            if (sel2) dl->AddRectFilled(a, b, v48::TableSel);
            else if (hoverRow == (int)i) dl->AddRectFilled(a, b, v48::HoverBg);
            if (sel2) dl->AddRectFilled(a, ImVec2(a.x + Px(2), b.y), v48::TableSelLine);
            HRule(dl, a.x, b.x, b.y - 1.0f, v48::RowRule);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(pid);
            if (ImGui::InvisibleButton("##trow", ImVec2(fixedW, rowH))) s_sp.agent = pid;
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hov) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); f.host->PlayerTooltip(ag.name); }
            float nx = x0 + cellPad;
            const float cy = ry + rowH * 0.5f;
            if (ImTextureID pt = AgentIcon(d, *f.host, pid)) dl->AddImage(pt, ImVec2(nx, R(cy - Px(8.5f))), ImVec2(nx + Px(17), R(cy - Px(8.5f)) + Px(17)));
            nx += Px(17) + Px(10);
            TextFit(dl, tc, ImVec2(nx, R(cy - LineH(12) * 0.5f)), x0 + nameW - cellPad - nx, v48::TableCell, ag.name.c_str());
            const int side = Team::Index01(ag.team);
            TextRight(dl, th, x0 + fixedW - cellPad, R(cy - LineH(11) * 0.5f), side == 1 ? v48::TableRedTag : v48::TableBlueTag,
                      SideTag(d, std::max(0, side)).c_str());
        }
        // The guilds' own figures: worked out again from the records, as the Explore's bars.
        for (int side = 0; side < 2; ++side)
        {
            const float ry = rowY((int)rows.size() + side) + Px(6);
            dl->AddRectFilled(ImVec2(x0, R(ry)), ImVec2(x0 + fixedW, R(ry) + rowH), v48::TeamRowBg);
            const std::string g = SideGuild(d, side);
            TextFit(dl, T(12, true), ImVec2(x0 + cellPad, R(ry + (rowH - LineH(12)) * 0.5f)), fixedW - cellPad * 2.0f,
                    side == 0 ? v48::BlueTitle : v48::RedTitle, g.c_str());
        }
    }

    // The indicators: their own part, scrolling sideways when they are many.
    const float cx0 = x0 + fixedW;
    ImGui::SetCursorScreenPos(ImVec2(cx0, y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Px(10));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##st_table_cols", ImVec2(x1 - cx0, tableH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground);
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    if (!s_sp.focusColumn.empty())
    {
        float cx = 0.0f;
        for (const auto& c : cols)
        {
            if (c.key == s_sp.focusColumn)
            {
                const float view = x1 - cx0;
                if (cx < ImGui::GetScrollX() || cx + c.w > ImGui::GetScrollX() + view)
                    ImGui::SetScrollX(std::max(0.0f, cx - (view - c.w) * 0.5f));
                break;
            }
            cx += c.w;
        }
        s_sp.focusColumn.clear();
    }
    float cx = o.x;
    std::string sortClicked;
    for (size_t k = 0; k < cols.size(); ++k)
    {
        const Col& c = cols[k];
        const float xr = cx + c.w - cellPad;
        const bool sorted = c.key == sortKey;
        // The head: the indicator's name, right-aligned as its numbers, a button.
        {
            const float lw = TextW(th, c.ind.label.c_str()) + (sorted ? Px(12) : 0.0f);
            const ImVec2 bp(R(xr + Px(9) - lw - Px(18)), R(y + (rowH - Px(25)) * 0.5f));
            ImGui::SetCursorScreenPos(bp);
            ImGui::PushID(c.key.c_str());
            if (ImGui::InvisibleButton("##sort", ImVec2(lw + Px(18), Px(25)))) sortClicked = c.key;
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hov)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                tdl->AddRectFilled(bp, ImVec2(bp.x + lw + Px(18), bp.y + Px(25)), v48::HoverBg, Px(5));
                const std::string hint = "Unit: " + StatsTable::UnitText(c.key, f.perMinute) + ".\n" +
                                         (sorted ? (desc ? "Click: lowest first." : "Click: highest first.") : "Click to sort by it, highest first.");
                StatsTable::IndicatorTooltip(*f.host, c.key, hint.c_str());
            }
            const ImU32 hc = sorted ? v48::TableHeadOn : hov ? v48::Text : v48::TableHead;
            const float tx = xr - lw;
            Text(tdl, th, ImVec2(R(tx), R(y + (rowH - LineH(11)) * 0.5f)), hc, c.ind.label.c_str());
            if (sorted) DrawArrowVertical(tdl, ImVec2(xr - Px(4), y + rowH * 0.5f), Px(9), hc, desc);
        }
        HRule(tdl, cx, cx + c.w, y + rowH - 1.0f, v48::TableRule);
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const int pid = rows[i];
            const float ry = rowY((int)i);
            const ImVec2 a(cx, R(ry)), b(cx + c.w, R(ry) + rowH);
            const bool sel2 = pid == s_sp.agent;
            if (sel2) tdl->AddRectFilled(a, b, v48::TableSel);
            else if (hoverRow == (int)i) tdl->AddRectFilled(a, b, v48::HoverBg);
            HRule(tdl, a.x, b.x, b.y - 1.0f, v48::RowRule);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID((int)(k * 1000 + i));
            if (ImGui::InvisibleButton("##cell", ImVec2(c.w, rowH))) s_sp.agent = pid;
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const auto& cell = c.values.players.at(pid);
            const std::string s = StatsTable::Format(c.key, cell, f.perMinute);
            if (hov)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s - %s: %s\n%s", d.agents.at(pid).name.c_str(), c.ind.label.c_str(), s.c_str(),
                                  cell.has ? StatsTable::UnitText(c.key, f.perMinute).c_str() : "No value: nothing to count it from in this time.");
            }
            TextRight(tdl, tc, xr, R(ry + (rowH - LineH(12)) * 0.5f), cell.has ? v48::TableCell : v48::NoValue, s.c_str());
        }
        for (int side = 0; side < 2; ++side)
        {
            const float ry = rowY((int)rows.size() + side) + Px(6);
            tdl->AddRectFilled(ImVec2(cx, R(ry)), ImVec2(cx + c.w, R(ry) + rowH), v48::TeamRowBg);
            const std::string s = StatsTable::Format(c.key, c.values.team[side], f.perMinute);
            TextRight(tdl, T(12, true), xr, R(ry + (rowH - LineH(12)) * 0.5f), side == 0 ? v48::BlueTitle : v48::RedTitle, s.c_str());
        }
        cx += c.w;
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y));
    ImGui::Dummy(ImVec2(std::max(1.0f, cx - o.x), bodyH));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!sortClicked.empty())
    {
        StatsTable::SetSort(sortClicked, sortClicked == sortKey ? !desc : true);
        s_sp.active = sortClicked;
    }
    if (scrolls)
    {
        // A soft edge where the columns pass under the names.
        const ImU32 shade = Hex(0x000000, 0x40), clear = Hex(0x000000, 0);
        dl->AddRectFilledMultiColor(ImVec2(cx0, y), ImVec2(cx0 + Px(10), y + bodyH), shade, clear, clear, shade);
    }
    return y + tableH;
}

// ─── The page ────────────────────────────────────────────────────────────────────────────────

static void DrawStatsCentered(ImDrawList* dl, float x0, float x1, float y, const char* line1, const char* line2)
{
    const float cx = (x0 + x1) * 0.5f;
    Text(dl, T(17), ImVec2(R(cx - TextW(T(17), line1) * 0.5f), R(y)), v48::Text, line1);
    if (line2) Text(dl, T(12), ImVec2(R(cx - TextW(T(12), line2) * 0.5f), R(y + LineH(17) + Px(10))), v48::Muted, line2);
}

#include "draw_library_lord.inl"

static void DrawStatsPage(const MatchMeta& m, ImVec2 wp, ImVec2 ws)
{
    if (s_openLordView) { s_sp.lord = true; s_openLordView = false; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pageA = wp, pageB(wp.x + ws.x, wp.y + ws.y);
    UpdateBackdrop(m.map_id);
    DrawBackdrop(dl, pageA, pageB);
    const ImVec2 a(R(wp.x + Px(22)), R(wp.y + Px(14))), b(R(pageB.x - Px(22)), R(pageB.y - Px(14)));
    DrawPanel(dl, pageA, pageB, a, b, ImDrawFlags_RoundCornersAll, true);

    if (s_sp.folder != m.folder_name)
    {
        s_sp.folder = m.folder_name;
        s_sp.agent = -1;
        s_sp.undoReady = false;
        s_sp.focusColumn.clear();
    }
    StatsTable::SetFavouritesFile(GuiGlobalConstants::GetSettingsFilePath().parent_path() / "stats_favourites.txt");
    NormalizeActive();

    const float x0 = R(a.x + Px(22)), x1 = R(b.x - Px(22));
    float y = DrawMatchHeading(dl, m, x0, x1, R(a.y + Px(18)));
    y += Px(5);
    const float bottom = b.y - Px(16);

    // Spoiler-free: nothing of the result, its length or its numbers.
    if (ResultHidden(m))
    {
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        ImGui::BeginChild("##st_hidden", ImVec2(x1 - x0, bottom - y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        DrawResultHiddenCover(m);
        ImGui::EndChild();
        return;
    }

    UpdateMatchAnalysis(m);
    using State = MatchAnalysisJob::State;
    const MatchAnalysisJob& job = s_analysis;
    const bool ready = job.state == State::Ready && job.folderName == m.folder_name;
    if (!ready)
    {
        const char* msg = job.state == State::Downloading ? "Downloading the match..."
                        : job.state == State::Failed      ? "The statistics could not be read"
                                                          : "Reading the match...";
        DrawStatsCentered(dl, x0, x1, y + (bottom - y) * 0.35f, msg, job.state == State::Failed ? job.error.c_str() : nullptr);
        return;
    }
    if (job.ledger.players.empty())
    {
        DrawStatsCentered(dl, x0, x1, y + (bottom - y) * 0.35f, "No statistics in this match file", "Its recording carries no Damage & Healing data.");
        return;
    }

    // The interval on the match clock, as the Stats and Map tabs read it.
    StatsFrame f;
    f.m = &m;
    f.d = &job.ledger;
    f.pre = job.gateSeconds;
    const int seconds = job.players.empty() ? 0 : (int)(job.players.front().pos.size() / 4) - 1;
    const int parsed = ParseDurationSeconds(m.match_duration);
    f.duration = std::max(1, parsed > 0 ? parsed : seconds - f.pre);
    f.span = f.duration + f.pre;
    ApplyScope(m, f.span, f.pre);
    s_sp.lastSpan = f.span;
    s_sp.lastPre = f.pre;
    f.r0 = s_state.detailIntervalFrom * f.span - f.pre;
    f.r1 = s_state.detailIntervalTo * f.span - f.pre;
    f.full = s_state.detailIntervalFrom <= 0.0f && s_state.detailIntervalTo >= 1.0f;
    if (f.full) f.scope = "Full match \xC2\xB7 " + ClockText((float)f.duration);
    else f.scope = ClockText(std::floor(f.r0)) + " - " + ClockText(std::ceil(f.r1));
    if (s_state.scope == Scope_Live && !f.full) f.scope = "Replay \xC2\xB7 " + f.scope;
    f.scopeUpper = f.full ? std::string("FULL MATCH") : Upper(f.scope);
    f.perMinute = StatsTable::PerMinute();
    f.data = LibraryStatsData(m);
    LibraryLedgerHost host(GetMatchSkillView(m), &m);
    f.host = &host;

    y = DrawStatsToolbar(dl, f, x0, x1, y);
    if (!s_sp.lord) y = DrawIndicatorStrip(dl, f, x0, x1, y);

    // Below the strip, the page scrolls: the timeline, Clear all's line, the picker, the results.
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, Px(10));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##st_body", ImVec2(b.x - Px(6) - x0, std::max(Px(40), bottom - y)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImDrawList* bdl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float bx0 = o.x, bx1 = o.x + (x1 - x0);
    float by = o.y;
    if (s_sp.lord)
    {
        if (s_lv.scrollReq >= 0.f) { ImGui::SetScrollY(s_lv.scrollReq); s_lv.scrollReq = -1.f; }
        by = DrawLordDamageView(bdl, f, bx0, bx1, by);
        ImGui::SetCursorScreenPos(ImVec2(o.x, by + Px(20)));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        ImGui::EndChild();
        return;
    }
    if (s_sp.timeOpen) by = DrawTimeStrip(f, bx0, bx1, by);
    if (s_sp.undoReady) by = DrawClearedLine(bdl, bx0, bx1, by);
    if (s_sp.pickerOpen) by = DrawPicker(bdl, f, bx0, bx1, by);
    else s_sp.pickerShown = false;
    NormalizeActive();
    if (StatsTable::Selection().empty()) by = DrawEmptyState(bdl, bx0, bx1, by);
    else if (s_sp.table) by = DrawStatsTablePage(bdl, f, bx0, bx1, by);
    else by = DrawExplore(bdl, f, bx0, bx1, by);
    ImGui::SetCursorScreenPos(ImVec2(o.x, by + Px(20)));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
    ImGui::EndChild();
}
