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

    // Where the starred columns are kept (stats_favourites.txt); set once by the Library.
    void SetFavouritesFile(const std::filesystem::path& file);

    bool PerMinute();
    void SetPerMinute(bool on);

    // The columns panel: search, Clear all, favourites, every stat by role. Fills the current region.
    void DrawColumnsPanel(Host& host);
    // The table: both teams, the picked columns over [r0, r1]. Fills the current region.
    void DrawTable(const Data& data, Host& host, float r0, float r1);
}
