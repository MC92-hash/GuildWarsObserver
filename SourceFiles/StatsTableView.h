#pragma once
// ---------------------------------------------------------------------------
// StatsTableView - the Stats table and its columns panel, shared by the Library's Stats tab and the
// replay window's Stats panel.
//
// Every stat by role (MatchStats::Layout), stars and ticks, plain tooltips with typical skills, and
// the per-player table over an interval [r0, r1] on the DISPLAYED match clock (0:00 = the gate).
// The numbers come from Data, read from match_analysis.json; the window draws through a Host (its
// own icons and bold font: each window has its own device and ImGui context). The picked columns,
// sort and favourites are one state for the whole app, so both windows show the same columns.
// ---------------------------------------------------------------------------
#include "BattleLedger.h"
#include "MatchStats.h"
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class SkillDatabaseView;
struct ImFont;

namespace StatsTable
{
    struct Death { float t = 0.f; int agent = 0; };

    struct Data
    {
        const BattleLedger::Data* ledger = nullptr;     // times on the displayed clock
        const MatchStats::Records* stats = nullptr;     // the extra columns' records, same clock
        const SkillDatabaseView* skills = nullptr;      // the match's skills (damage kinds)
        std::vector<Death> deaths;                      // players' deaths (Deaths, Kills)
        // Seconds dead and ground covered by `agent` over [r0, r1], added to dead / dist; empty
        // when the caller has no per-second track.
        std::function<void(int agent, float r0, float r1, double& dead, double& dist)> track;
    };

    class Host : public BattleLedger::Host
    {
    public:
        virtual ImFont* Bold() = 0;
        virtual void PlayerTooltip(const std::string& /*name*/) {}       // the mouse is on a player's name
        virtual void OpenBars(int /*agent*/, int /*ledgerTab*/) {}         // a fixed column's cell was clicked
    };

    // What a window without the Library's analysis job reads from match_analysis.json (the replay).
    struct Track { int agent = 0; std::vector<int> pos; };   // per second of the recording: x, y, z, state (1 dead)
    struct Loaded
    {
        BattleLedger::Data ledger;
        MatchStats::Records stats;
        std::vector<Death> deaths;
        std::vector<Track> tracks;
        int gateSeconds = 0;   // recording seconds before 0:00 (tracks start there)
    };
    // match_analysis.json at the current version; false when missing, unreadable or out of date.
    // (draw_replay_browser.cpp: the Library's own reader.)
    bool LoadFile(const std::filesystem::path& file, Loaded& out);
    // Data over a Loaded file; it must outlive the Data.
    Data MakeData(const Loaded& file, const SkillDatabaseView* skills);

    // Where the starred columns are kept (stats_favourites.txt); set once by the Library. The
    // selection (below) is kept beside it, in stats_selection.txt.
    void SetFavouritesFile(const std::filesystem::path& file);

    bool PerMinute();
    void SetPerMinute(bool on);

    // ── The selection: ONE ordered list of indicators ────────────────────────────────────────
    // The indicators shown are Layout keys ("base:dealt", "kd_blocked"...), in the order they
    // were picked, shared by the Library's Statistics page (its Explore chips, its Table columns,
    // its picker) and the replay's Stats panel. Kept between sessions; the first time, it is what
    // the table showed before (its default columns and the starred ones). A starred column (the
    // replay panel's favourite) is always in it.
    std::vector<std::string> Selection();
    bool IsSelected(const std::string& key);
    // On: added at the end. Off: taken out, and unstarred.
    void Select(const std::string& key, bool on);
    // Empties the selection (and unstars what it held).
    void ClearSelection();

    // The sort: an indicator, and an explicit direction (true: highest first).
    std::string SortKey();
    bool SortDescending();
    void SetSort(const std::string& key, bool descending);

    // What Clear all takes away, for its Undo.
    struct Snapshot
    {
        std::vector<std::string> selection, favourites;
        std::string sortKey;
        bool sortDescending = true;
    };
    Snapshot TakeSnapshot();
    void Restore(const Snapshot& s);

    // ── Indicators: what a key is ────────────────────────────────────────────────────────────
    struct Indicator
    {
        std::string key;
        std::string label, head, meaning, method, group, sub;
        MatchStats::Agg agg = MatchStats::Agg::Count;
        MatchStats::Hue hue = MatchStats::Hue::Neutral;
        int  ledgerMetric = -1;     // a fixed column that IS a BattleLedger tab's figure; -1 otherwise
        bool lowerBetter = false;
        bool estimated = false;     // values rest on a model's estimate (shown with "~")
        bool detailed = false;      // left out of the layout: found by search only
        bool fixed = false;         // a Library fixed column ("base:...")
    };
    bool Describe(const std::string& key, Indicator& out);
    // Every indicator offered: the layout's in its order, then the detailed ones.
    std::vector<std::string> AllIndicators();
    // Every keyword of `query` appears in the indicator's name, group, sub-group or text.
    bool MatchesSearch(const std::string& key, const char* query);
    // The unit its cells are in: "Health", "Health per minute", "% of the time", "seconds, median"...
    std::string UnitText(const std::string& key, bool perMinute);
    // Its tooltip: the plain meaning, typical skills as icons, the method; `hint` under it.
    void IndicatorTooltip(Host& host, const std::string& key, const char* hint = nullptr);

    // ── Values: the table's own aggregation, for any view ───────────────────────────────────
    // One cell. v sorts and ranks (already per minute when asked); num / den a ratio's counts; n
    // the records a median rests on. has = false is "unavailable" (no data, a median with no case,
    // a ratio with no try): shown as an em dash, never as 0. A computed zero has has = true.
    struct Cell { double v = 0.0, num = 0.0, den = 0.0; int n = 0; bool has = false; };
    struct Column
    {
        std::unordered_map<int, Cell> players;   // every board player
        // Blue, red: worked out again from the records (a ratio over the summed tries, a median
        // over every case of the team, a share of time averaged), never a sum of medians or shares.
        Cell team[2];
        bool additive = false;                   // team = the players' sum: a share of it means something
    };
    Column Values(const Data& data, const std::string& key, float r0, float r1, bool perMinute);
    // "1,234", "57", "4.3", "95%", "0.82 s", "312 s"; unavailable is an em dash. `compact`: a
    // ratio as its percentage alone ("95%") rather than "20/21  95%".
    std::string Format(const std::string& key, const Cell& c, bool perMinute, bool compact = false);

    // The columns panel: search, Clear all, favourites, every stat by role. Fills the current region.
    void DrawColumnsPanel(Host& host);
    // The table: both teams, the picked columns over [r0, r1]. Fills the current region.
    void DrawTable(const Data& data, Host& host, float r0, float r1);
}
