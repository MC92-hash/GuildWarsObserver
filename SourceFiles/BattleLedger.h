#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "imgui.h"

struct BitmapFont;

// ---------------------------------------------------------------------------
// The Battle Ledger's lines and its Damage & Healing view (BattleLedgerView.cpp), shared by the
// replay window (ReplayWindow_BattleLedger.cpp builds the lines) and the Library's Stats tab
// (which reads them back from match_analysis.json). The view draws from Data alone; a Host gives
// it the icons, skill names and digit fonts of the window it draws in.
// ---------------------------------------------------------------------------
namespace BattleLedger
{
    // One line per damage / heal / prevention packet, with the number its floater showed and the
    // skill the damage attribution named, and one per interrupt attempt.
    enum class Kind : uint8_t { Damage, SelfCost, Heal, DivineFavor, Prevented };
    struct Line
    {
        float t = 0.f;
        int   src = 0, dst = 0;
        int   amount = 0;
        int   skillId = 0;            // resolved PvP id; 0 none (a weapon hit when weaponHit)
        Kind  kind = Kind::Damage;
        bool  weaponHit = false;
        // Nonzero: the detail table's row instead of the skill's (kRowMinions, kRowDegenUnknown,
        // or DegenRow(condition, cause)); skillId then names the icon - the Animate skill, the
        // condition's cause.
        int   rowKey = 0;
        // Prevented: an estimate (a capped hit, a blocked or failed attack) rather than a number
        // the recording proves, and how many hits it stands for (capped / blocked counts).
        bool  estimated = false;
        int   count = 0;
        uint8_t preventGroup = 0;     // Prevented without a heal: 1 + DamageAttribution::PreventGroup
    };
    constexpr int kRowMinions      = -4;
    constexpr int kRowPets         = -12;   // a pet's hits, on its ranger's "Pets" row
    constexpr int kRowDegenUnknown = -5;
    // Map effects: never a player's (src 0), shown on the board's MAP EFFECTS strip and in a
    // victim's "hit by" / "healed by" under their own name.
    constexpr int kRowCatapult = -6;
    constexpr int kRowCoral    = -7;
    constexpr int kRowAcid     = -8;
    constexpr int kRowFlux     = -9;
    constexpr int kRowMiasma   = -10;
    constexpr int kRowWater    = -11;
    constexpr bool IsMapRow(int key) { return key <= -6 && key >= -11; }
    const char* MapRowName(int key);
    class Host;
    ImTextureID MapRowIcon(Host& host, int key);
    // A condition's degeneration from one cause: -(1,000,000 + condition x 10,000 + cause skill).
    constexpr int DegenRow(int condition, int cause) { return -(1000000 + condition * 10000 + cause); }
    constexpr bool IsDegenRow(int key) { return key <= -1000000; }
    constexpr int DegenCondition(int key) { return (-key - 1000000) / 10000; }
    constexpr int DegenCause(int key) { return (-key - 1000000) % 10000; }
    struct Interrupt
    {
        enum Outcome : uint8_t { Landed, Faked, Missed };
        float t = 0.f;                // when it landed (or would have)
        int   src = 0, dst = 0;
        int   skillId = 0;            // the interrupting skill
        Outcome outcome = Missed;
        int   lostSkill = 0;          // the cast it stopped (landed) or the one cancelled (faked)
        int   afterMs = -1;           // landed: how far into that cast
        int   reflexMs = -1;          // landed: interrupter's activation minus that cast's start
    };

    // A player's skill use, for the casts / interrupted / cancelled columns. skillId resolved.
    struct Use { int skillId = 0; int target = -1; float start = 0.f, end = 0.f; bool interrupted = false, cancelled = false; };
    struct Agent { std::string name; int team = 0, prof = 0, sec = 0; bool lord = false; };

    struct Data
    {
        std::vector<Line> lines;              // time order
        std::vector<Interrupt> ints;
        std::vector<int> players;             // the board's rows
        std::unordered_map<int, Agent> agents;   // every agent the lines name
        std::unordered_map<int, std::vector<Use>> uses;   // per player, time order
        std::string guild[2], tag[2];         // blue, red: "Name [TAG]" and "TAG"
    };

    enum Metric { kDealt, kTaken, kHealed, kReceived, kPrevented, kInts, kIntsRecv, kMetricCount };
    const char* MetricName(int metric);
    int  LineMetricGiver(Kind k);
    int  LineMetricTaker(Kind k);
    bool GivingMetric(int metric);

    // Every tab's number per player over [r0, r1].
    struct Totals { long long v[kMetricCount] = {}; int intAtt = 0, intFake = 0, recvAtt = 0, recvFake = 0; };
    std::unordered_map<int, Totals> SumTotals(const Data& d, float r0, float r1);

    class Host
    {
    public:
        virtual ~Host() = default;
        virtual ImTextureID SkillIcon(int skillId) = 0;
        virtual ImTextureID ProfIcon(int prof) = 0;
        virtual ImTextureID UiIcon(const char* othersUiFile) = 0;   // a file of Textures/Others_UI
        virtual std::string SkillName(int skillId) = 0;
        virtual const BitmapFont* Digits(int metric) = 0;            // the client's digits for a tab; may be null
    };

    struct ViewState
    {
        int  metric = kDealt;
        bool matrix = false;
        int  selPlayer = -1;
        bool showAll = false;
        int  mxSide = 0;                      // 0: Blue's players give, 1: Red's
        std::unordered_set<int> openRows;     // expanded rows of the selected player's table
        // The replay's time strip.
        int   scope = 0;                      // Scope
        float selA = 0.f, selB = 0.f;
        bool  hasSel = false;
        float dragFrom = -1.f;
        double selHintUntil = 0.0;
    };
    enum Scope { kFull, kPlayhead, kLast60, kSelection };

    // The panel's round pill (the Combat Log's filter pill), for hosts that put choices beside it.
    bool Pill(const char* label, bool active);

    // The tab band: one pill per metric, then Board / Team matrix at the right.
    void DrawMetricTabs(Host& host, ViewState& st);

    // The replay's time strip under the tabs: scope pills and both teams' bars; sets [r0, r1].
    struct StripInputs
    {
        float tEnd = 1.f, tNow = 0.f;
        std::vector<std::pair<float, int>> spikes;                    // first hit, target
        const std::unordered_map<int, std::vector<float>>* deaths = nullptr;   // by agent
    };
    void DrawTimeStrip(const Data& d, ViewState& st, const StripInputs& in, float& r0, float& r1);

    // The board (or team matrix) and the selected player's breakdown over [r0, r1]. stretch names
    // the counted stretch in the player's header ("Selection 04:12-05:40").
    void DrawBody(const Data& d, Host& host, ViewState& st, float r0, float r1, const std::string& stretch);
}
