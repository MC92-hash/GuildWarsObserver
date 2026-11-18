#pragma once
// ---------------------------------------------------------------------------
// MatchLink - a share link to one moment of a published match
//
//   https://gwobserver.com/watch?v=1&m=<folder>&t=754&p=12&cam=f,1.234,-0.940,1450&pause=1
//
// The same query is accepted behind gwobserver://watch? (what the website hands to the app) and
// behind www.gwobserver.com. Pure functions, no application state: the app's handling of a link
// lives in UrlProtocol (receiving it) and MapBrowser::ProcessPendingLink (opening it).
//
//   v      link format version (kVersion). A newer one is refused: the app says to update.
//   m      MatchMeta::folder_name, as the cloud index has it. Required.
//   t      seconds on the DISPLAYED match clock (0:00 = the gate, ReplaySync's clock).
//          Also read as 12:34, 1:02:03 or 12m34s.
//   p      the selected / followed agent id.
//   cam    f,<yaw>,<pitch>,<dist>        follow camera on agent p
//          c,<x>,<y>,<z>,<yaw>,<pitch>   free camera
//   pause  1 paused (default), 0 playing.
//
// Unknown fields are ignored and broken values are dropped one by one, so a damaged camera still
// opens the match at its time.
//
// DISPLAY-ONLY fields, for the website's loading-screen-style card. Written when a link is
// copied, never used to open one (the app reads the match itself):
//   g1 / g2  <name>~<rank>~<rating>~<capeId>~<profs>   team 1 (blue, party "1") / team 2 (red)
//            name percent-encoded; capeId = CapeId() (empty without a cape); profs = the team's
//            primary professions in party order, one lowercase hex digit each (0 = unknown)
//   o        the occasion, as the Library and the loading screen show it (e.g. "A AT")
// ---------------------------------------------------------------------------
#include <DirectXMath.h>
#include <optional>
#include <string>
#include <string_view>

struct CapeData;
struct MatchMeta;

struct MatchLink
{
    // Display only (g1 / g2): see the header comment.
    struct Team
    {
        bool        present = false;
        std::string name;
        int         rank = 0;
        int         rating = 0;
        std::string capeId;
        std::string profs;
    };

    enum class Cam { None, Follow, Free };

    int         version = 1;
    std::string folder;
    float       t = 0.f;
    int         agent = -1;
    Cam         cam = Cam::None;
    float       yaw = 0.f, pitch = 0.f, dist = 0.f;
    DirectX::XMFLOAT3 pos{0.f, 0.f, 0.f};
    bool        paused = true;

    Team        teams[2];       // [0] = g1 (party "1", blue), [1] = g2 (party "2", red)
    std::string occasion;       // o
};

namespace MatchLinks
{
    constexpr int kVersion = 1;

    // The page the copied links point at. One constant, so it can become /watch.html? if the
    // site's /watch route does not resolve (owner trials, SHARE_LINKS_PLAN.md section 8).
    inline constexpr const char* kWebBase = "https://gwobserver.com/watch?";
    inline constexpr const char* kAppBase = "gwobserver://watch?";

    enum class ParseError { None, NotALink, NoMatch, NewerVersion };

    // The query part alone: v=1&m=...&t=... (no leading '?').
    std::string FormatQuery(const MatchLink& link);
    // kWebBase + query: what "Copy link" puts on the clipboard (clickable in Discord).
    std::string FormatHttps(const MatchLink& link);

    // Any of the accepted forms, with surrounding blanks, quotes or <> tolerated.
    std::optional<MatchLink> Parse(std::string_view text, ParseError* error = nullptr);

    // Cheap test for "this text is meant as a share link" (the Library search box): a share-link
    // prefix, whatever the query holds.
    bool LooksLikeLink(std::string_view text);

    // "shape-detail-emblem-trim-bg_color-detail_color-emblem_color", decimal: the cape's id in a
    // link and the file name of its banner (--export-cape-banners, assets/capes/<id>.png).
    std::string CapeId(const CapeData& cape);

    // The display-only fields from the match: each team's guild found the way the loading screen
    // finds it (the folder name's tag first, then the party's guilds), its professions, the occasion.
    void FillDisplay(MatchLink& link, const MatchMeta& match);

    // "12:34" (or "1:02:03" past an hour), for labels.
    std::string FormatClock(float seconds);

    // A label from the folder name alone, "YYYY-MM-DD_<Map_Name>_<hh.mm>" -> "Map Name, 2026-11-15";
    // the raw name when it does not have that shape.
    std::string DescribeFolder(const std::string& folder);

    // GWO_LINK_SELFTEST: format -> parse round trips over a table of cases. Writes each result to
    // the run log and returns the process exit code, 0 when every case passed.
    int RunSelfTest();
}
