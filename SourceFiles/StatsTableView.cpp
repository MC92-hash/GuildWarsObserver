#include "pch.h"
// ---------------------------------------------------------------------------
// StatsTableView - the Stats table, shared by the Library's Stats tab and the replay's Stats panel
// (StatsTableView.h). Moved out of draw_replay_browser.cpp on 2026-10-30 so the replay can show the
// same columns, names, tooltips and favourites; the window's icons and bold font come from a Host,
// the numbers from Data (match_analysis.json's ledger, stats records, deaths and tracks).
// ---------------------------------------------------------------------------
#include "StatsTableView.h"
#include "SkillDatabase.h"
#include "TeamColors.h"
#include "ui/ui_kit.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_set>

namespace
{
    StatsTable::Host* s_host = nullptr;   // the window drawing now (its icons and bold font)
    std::filesystem::path s_favFile;      // stats_favourites.txt, given by the Library

    enum StatCol { SC_Dealt, SC_Taken, SC_Healed, SC_Received, SC_Prevented, SC_Kills, SC_Deaths, SC_Ints,
                   SC_IntsRecv, SC_Skills, SC_Cancelled, SC_Dead, SC_Distance,
                   SC_DmgAttack, SC_DmgSkill, SC_DmgHex, SC_DmgCond, SC_DmgMinion,
                   SC_DmgBleed, SC_DmgBurn, SC_DmgPoison, SC_DmgDisease, SC_Sacrifice, SC_SelfCond, SC_Count };
    // name: the column's id in MatchStats::Layout() ("base:<name>"); help: its plain meaning;
    // lowerBetter: a smaller number is the better play (the header marks it).
    struct StatColInfo { const char* name; const char* label; const char* head; int ledgerTab; const char* help; bool lowerBetter; };
    const StatColInfo kStatCols[SC_Count] = {
        { "dealt",     "Damage dealt",        "Damage",      BattleLedger::kDealt,     "All the damage this player caused: attacks, skills, hexes, conditions and minions. Degeneration is included: the Health lost to it is split among the conditions and hexes active at the time, each part credited to whoever applied it. Own sacrifices are left out.", false },
        { "taken",     "Damage taken",        "Taken",       BattleLedger::kTaken,     "Damage taken from foes and the map, degeneration included. The player's own sacrifices are apart (Health sacrificed).", true },
        { "healed",    "Healing done",        "Healing",     BattleLedger::kHealed,    "Healing given, the player's own included.", false },
        { "received",  "Healing received",    "Healed",      BattleLedger::kReceived,  "Healing received, from anyone and the map.", false },
        { "prevented", "Damage prevented",    "Prevented",   BattleLedger::kPrevented, "Damage the player's protections spared, some of it estimated.", false },
        { "kills",     "Kills",               "Kills",       BattleLedger::kDealt,     "Foes whose last hit before dying (within 5 s) was this player's.", false },
        { "deaths",    "Deaths",              "Deaths",      BattleLedger::kTaken,     "Times the player died.", true },
        { "ints",      "Interrupts landed",   "Interrupts",  BattleLedger::kInts,      "Foe casts this player interrupted.", false },
        { "intsrecv",  "Interrupted",         "Interrupted", BattleLedger::kIntsRecv,  "Casts of this player that a foe interrupted.", true },
        { "skills",    "Skills used",         "Skills",      BattleLedger::kDealt,     "Skills used, interrupted ones included, cancelled ones left out.", false },
        { "cancelled", "Skills cancelled",    "Cancelled",   BattleLedger::kIntsRecv,  "Casts the player stopped themselves.", true },
        { "dead",      "Time dead",           "Dead",        BattleLedger::kTaken,     "Seconds spent dead. Per minute: the share of the interval.", true },
        { "distance",  "Distance",            "Distance",    BattleLedger::kDealt,     "Ground covered while alive, in game units.", false },
        { "dmg_attack", "Attack damage",      "Attacks",     BattleLedger::kDealt,     "Damage dealt by weapon hits and attack skills.", false },
        { "dmg_skill",  "Skill damage",       "Skills dmg",  BattleLedger::kDealt,     "Damage dealt by spells and other non-attack skills (hexes apart), and damage whose skill is unknown.", false },
        { "dmg_hex",    "Hex damage",         "Hexes",       BattleLedger::kDealt,     "Damage dealt by hexes: their hits (Empathy, Spiteful Spirit...) and their degeneration.", false },
        { "dmg_cond",   "Condition damage",   "Conditions",  BattleLedger::kDealt,     "Degeneration from the conditions this player applied: Bleeding, Burning, Poison, Disease.", false },
        { "dmg_minion", "Minion, spirit & pet damage", "Summons", BattleLedger::kDealt, "Damage dealt by this player's minions, spirits (Bloodsong...) and pet.", false },
        { "dmg_bleed",  "Bleeding damage",    "Bleeding",    BattleLedger::kDealt,     "Degeneration from the Bleeding this player applied.", false },
        { "dmg_burn",   "Burning damage",     "Burning",     BattleLedger::kDealt,     "Degeneration from the Burning this player applied.", false },
        { "dmg_poison", "Poison damage",      "Poison",      BattleLedger::kDealt,     "Degeneration from the Poison this player applied.", false },
        { "dmg_disease","Disease damage",     "Disease",     BattleLedger::kDealt,     "Degeneration from the Disease this player applied.", false },
        { "sacrifice",  "Health sacrificed",  "Sacrificed",  BattleLedger::kTaken,     "Health this player paid for their own skills (sacrifices). Not part of Damage taken.", false },
        { "selfcond",   "Own conditions caught", "Own condi",  BattleLedger::kTaken,     "Health this player lost to a condition from their own skill that came back to them: Mystic Corruption puts Disease on adjacent foes, Disease spreads, and the Dervish catches it. Their own doing, so it is not part of Damage taken.", true },
    };

    // A column is one of the fixed ones above (0 .. SC_Count - 1) or a stat of the MatchStats
    // catalogue (kExtraBase + its index), whose records match_analysis.json carries.
    constexpr int kExtraBase = 100;
    const MatchStats::StatDef* ExtraDef(int c)
    {
        const auto& cat = MatchStats::Catalog();
        return c >= kExtraBase && c - kExtraBase < (int)cat.size() ? &cat[c - kExtraBase] : nullptr;
    }
    const char* ColLabel(int c) { const auto* d = ExtraDef(c); return d ? d->label : kStatCols[c].label; }
    const char* ColHead(int c)  { const auto* d = ExtraDef(c); return d ? d->head : kStatCols[c].head; }
    bool ColLowerBetter(int c)  { const auto* d = ExtraDef(c); return d ? d->lowerBetter : kStatCols[c].lowerBetter; }

    // A MatchStats::Layout() key's column: "base:<name>" a fixed one, else the catalogue stat; -1 unknown.
    int LayoutCol(const char* key)
    {
        if (std::strncmp(key, "base:", 5) == 0)
        {
            for (int c = 0; c < SC_Count; ++c) if (std::strcmp(key + 5, kStatCols[c].name) == 0) return c;
            return -1;
        }
        const auto& cat = MatchStats::Catalog();
        for (size_t i = 0; i < cat.size(); ++i) if (std::strcmp(cat[i].key, key) == 0) return kExtraBase + (int)i;
        return -1;
    }

    // Typical skills behind a column, shown as icons in its tooltip so the meaning is plain to see.
    // Ids checked against skilldesc-en.json; the conditions are the client's condition "skills"
    // (478 Bleeding ... 486 Weakness), which have icons but no skill data.
    struct ExampleSkill { int id; const char* name; };
    using Examples = std::vector<ExampleSkill>;
    const Examples& ColExamples(int c)
    {
        static const Examples kHeals   = { { 2061, "Patient Spirit" }, { 1257, "Spirit Light Weapon" }, { 292, "Infuse Health" }, { 1118, "Healing Burst" } };
        static const Examples kProts   = { { 245, "Protective Spirit" }, { 258, "Guardian" }, { 307, "Reversal of Fortune" }, { 299, "Shielding Hands" }, { 1114, "Spirit Bond" } };
        static const Examples kRupts   = { { 426, "Savage Shot" }, { 399, "Distracting Shot" }, { 25, "Power Drain" }, { 61, "Leech Signet" } };
        static const Examples kKds     = { { 2011, "Grapple" }, { 226, "Mind Shock" }, { 332, "Bull's Strike" }, { 869, "\"Coward!\"" }, { 162, "Gale" } };
        static const Examples kStab    = { { 2063, "Aura of Stability" }, { 938, "Ward of Stability" }, { 1572, "\"Brace Yourself!\"" }, { 371, "Balanced Stance" }, { 1514, "Fleeting Stability" } };
        static const Examples kDrains  = { { 42, "Energy Burn" }, { 49, "Mind Wrack" }, { 39, "Energy Surge" }, { 366, "\"Fear Me!\"" } };
        static const Examples kCondRm  = { { 275, "Mend Condition" }, { 276, "Restore Condition" }, { 311, "Draw Conditions" }, { 1401, "Mending Touch" } };
        static const Examples kHexRm   = { { 2003, "Cure Hex" }, { 295, "Purge Signet" }, { 1692, "Divert Hexes" }, { 302, "Smite Hex" } };
        static const Examples kHexes   = { { 26, "Empathy" }, { 30, "Diversion" }, { 52, "Panic" }, { 31, "Conjure Phantasm" } };
        static const Examples kConds   = { { 481, "Crippled" }, { 479, "Blind" }, { 486, "Weakness" }, { 485, "Dazed" }, { 478, "Bleeding" } };
        static const Examples kRes     = { { 1481, "Death Pact Signet" }, { 2, "Resurrection Signet" }, { 791, "Flesh of My Flesh" } };
        static const std::unordered_map<std::string, Examples> byKey = {
            { "base:dealt",      { { 338, "Eviscerate" }, { 1227, "Essence Strike" }, { 26, "Empathy" }, { 478, "Bleeding" }, { 1253, "Bloodsong" } } },
            { "base:dmg_attack", { { 338, "Eviscerate" }, { 336, "Executioner's Strike" }, { 352, "Crushing Blow" }, { 907, "Dragon Slash" } } },
            { "base:dmg_skill",  { { 1227, "Essence Strike" }, { 133, "Dark Pact" }, { 1662, "Mind Blast" }, { 205, "Lightning Surge" } } },
            { "base:dmg_hex",    { { 26, "Empathy" }, { 121, "Spiteful Spirit" }, { 31, "Conjure Phantasm" }, { 109, "Life Siphon" } } },
            { "base:dmg_cond",   { { 478, "Bleeding" }, { 480, "Burning" }, { 484, "Poison" }, { 483, "Disease" } } },
            { "base:dmg_minion", { { 84, "Animate Bone Fiend" }, { 832, "Animate Flesh Golem" }, { 1253, "Bloodsong" }, { 411, "Charm Animal" } } },
            { "base:dmg_bleed",  { { 478, "Bleeding" } } },
            { "base:dmg_burn",   { { 480, "Burning" } } },
            { "base:dmg_poison", { { 484, "Poison" } } },
            { "base:dmg_disease",{ { 483, "Disease" } } },
            { "cond_inflicted_crippled",   { { 481, "Crippled" } } },
            { "cond_inflicted_dazed",      { { 485, "Dazed" } } },
            { "cond_inflicted_weakness",   { { 486, "Weakness" } } },
            { "cond_inflicted_cracked",    { { 2077, "Cracked Armor" } } },
            { "cond_inflicted_blind",      { { 479, "Blind" } } },
            { "cond_inflicted_deep_wound", { { 482, "Deep Wound" } } },
            { "base:sacrifice",  { { 133, "Dark Pact" }, { 791, "Flesh of My Flesh" }, { 119, "Blood is Power" }, { 146, "Offering of Blood" } } },
            { "base:selfcond",   { { 1755, "Mystic Corruption" }, { 483, "Disease" } } },
            { "base:healed", kHeals }, { "heal_effective", kHeals }, { "heal_overheal", kHeals },
            { "base:prevented", kProts }, { "spike_reaction", kProts },
            { "base:ints", kRupts }, { "base:intsrecv", kRupts }, { "int_accuracy", kRupts }, { "int_reaction", kRupts }, { "int_fakes", kRupts },
            { "kd_dealt", kKds }, { "kd_success", kKds }, { "kd_taken", kKds },
            { "kd_blocked", kStab }, { "kd_prevent_reaction", kStab },
            { "en_denied", kDrains }, { "en_lost_foes", kDrains },
            { "ench_removed", { { 141, "Rend Enchantments" }, { 69, "Shatter Enchantment" }, { 143, "Strip Enchantment" } } },
            { "cond_inflicted_total", kConds }, { "cond_time", kConds },
            { "hex_time", kHexes },
            { "cond_removed", kCondRm }, { "cond_removal_reaction", kCondRm },
            { "hex_removed", kHexRm }, { "hex_removal_reaction", kHexRm },
            { "act_res_done", kRes }, { "act_res_reaction", kRes },
            { "en_given", { { 1782, "\"The Power Is Yours!\"" } } },
            { "kd_coward_dodge", { { 869, "\"Coward!\"" } } },
        };
        static const Examples none;
        const auto* d = ExtraDef(c);
        auto it = byKey.find(d ? std::string(d->key) : std::string("base:") + kStatCols[c].name);
        return it == byKey.end() ? none : it->second;
    }

    // A column's tooltip: what it means in plain words, "Lower is better" when it is, typical
    // skills as icons, the method dimmed below (catalogue stats), then the caller's own hint.
    void ColTooltip(int c, const char* hint = nullptr, bool withName = false)
    {
        const auto* d = ExtraDef(c);
        const ImVec4 dim(0.55f, 0.57f, 0.60f, 1.0f);
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        if (withName) ImGui::TextColored(ImVec4(0.88f, 0.75f, 0.31f, 1.0f), "%s", ColLabel(c));
        ImGui::TextUnformatted(d ? (d->meaning[0] ? d->meaning : d->help) : kStatCols[c].help);
        if (ColLowerBetter(c)) ImGui::TextColored(ImVec4(0.90f, 0.62f, 0.45f, 1.0f), "Lower is better.");
        if (const Examples& ex = ColExamples(c); !ex.empty())
        {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::TextColored(dim, "Typical skills:");
            const float sz = ImGui::GetFontSize() * 2.2f;
            for (size_t i = 0; i < ex.size(); ++i)
            {
                if (i) ImGui::SameLine(0.0f, 4.0f);
                if (ImTextureID t = s_host->SkillIcon(ex[i].id)) ImGui::Image(t, ImVec2(sz, sz));
                else ImGui::Dummy(ImVec2(sz, sz));
            }
        }
        if (d && d->meaning[0] && d->meaning != d->help)
        {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            ImGui::TextColored(dim, "How it is measured: %s", d->help);
        }
        if (d && d->estimated) ImGui::TextColored(dim, "~ = estimated from a model.");
        if (hint) ImGui::TextColored(ImVec4(0.67f, 0.69f, 0.72f, 1.0f), "%s", hint);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    struct StatsTabState
    {
        bool perMinute = false;
        bool on[SC_Count] = { true, true, true, true, true, true, true, true, false, true, false, false, false };
        std::vector<int> extra;           // the catalogue's columns picked, in the order picked (kExtraBase + index)
        char search[64] = {};             // the More stats picker's keywords
        int  sort = SC_Dealt;
    };
    StatsTabState s_stats;


    // Every column's number per player over [r0, r1] on the match clock.
    std::unordered_map<int, std::array<double, SC_Count>> StatValues(const StatsTable::Data& data, float r0, float r1)
    {
        const BattleLedger::Data& d = *data.ledger;
        const SkillDatabaseView& skills = *data.skills;
        std::unordered_map<int, std::array<double, SC_Count>> out;
        for (int pid : d.players) out[pid].fill(0.0);
        const auto tot = BattleLedger::SumTotals(d, r0, r1);
        for (auto& [pid, v] : out)
        {
            auto it = tot.find(pid);
            if (it == tot.end()) continue;
            const auto& t = it->second.v;
            v[SC_Dealt]     = (double)t[BattleLedger::kDealt];
            v[SC_Taken]     = (double)t[BattleLedger::kTaken];
            v[SC_Healed]    = (double)t[BattleLedger::kHealed];
            v[SC_Received]  = (double)t[BattleLedger::kReceived];
            v[SC_Prevented] = (double)t[BattleLedger::kPrevented];
            v[SC_Ints]      = (double)t[BattleLedger::kInts];
            v[SC_IntsRecv]  = (double)t[BattleLedger::kIntsRecv];
        }
        // Damage dealt by kind, the same lines Damage dealt sums, so the five always add up to it.
        for (const BattleLedger::Line& ln : d.lines)
        {
            if (ln.t < r0) continue;
            if (ln.t > r1) break;
            if (BattleLedger::LineMetricGiver(ln.kind) != BattleLedger::kDealt) continue;
            auto it = out.find(ln.src);
            if (it == out.end()) continue;
            int col = SC_DmgSkill;
            if (ln.rowKey == BattleLedger::kRowMinions || ln.rowKey == BattleLedger::kRowPets) col = SC_DmgMinion;
            else if (BattleLedger::IsDegenRow(ln.rowKey) || ln.rowKey == BattleLedger::kRowDegenUnknown)
            {
                col = SC_DmgCond;
                // Each damaging condition also on its own column (the client's condition ids).
                switch (BattleLedger::IsDegenRow(ln.rowKey) ? BattleLedger::DegenCondition(ln.rowKey) : 0)
                {
                case 478: it->second[SC_DmgBleed]   += ln.amount; break;
                case 480: it->second[SC_DmgBurn]    += ln.amount; break;
                case 484: it->second[SC_DmgPoison]  += ln.amount; break;
                case 483: it->second[SC_DmgDisease] += ln.amount; break;
                default: break;
                }
            }
            else if (ln.weaponHit) col = SC_DmgAttack;
            else if (const SkillInfo* si = ln.skillId ? skills.Get(ln.skillId) : nullptr)
                col = SkillDatabase::IsWeaponAttack(si->type) ? SC_DmgAttack
                    : si->type == 24                    ? SC_DmgHex
                    : si->type == 18 || si->type == 19  ? SC_DmgMinion     // a spirit's attack (Bloodsong)
                                                        : SC_DmgSkill;
            it->second[col] += ln.amount;
        }
        // Sacrifices out of Damage taken, on their own column.
        for (const BattleLedger::Line& ln : d.lines)
        {
            if (ln.t < r0) continue;
            if (ln.t > r1) break;
            if (ln.kind != BattleLedger::Kind::SelfCost) continue;
            auto it = out.find(ln.dst);
            if (it == out.end()) continue;
            it->second[BattleLedger::IsDegenRow(ln.rowKey) ? SC_SelfCond : SC_Sacrifice] += ln.amount;
            it->second[SC_Taken] -= ln.amount;
        }
        // Deaths, and the kill: the last foe player's hit within 5 s before it.
        auto teamOf = [&](int id) { auto it = d.agents.find(id); return it == d.agents.end() ? 0 : it->second.team; };
        for (const auto& e : data.deaths)
        {
            if (e.t < r0 || e.t > r1 || !out.count(e.agent)) continue;
            out[e.agent][SC_Deaths] += 1.0;
            int killer = 0;
            for (const auto& ln : d.lines)
            {
                if (ln.t > e.t) break;
                if (ln.t < e.t - 5.0f || ln.dst != e.agent || ln.kind != BattleLedger::Kind::Damage) continue;
                killer = ln.src;
            }
            if (killer && out.count(killer) && teamOf(killer) != teamOf(e.agent)) out[killer][SC_Kills] += 1.0;
        }
        for (const auto& [pid, uses] : d.uses)
        {
            auto it = out.find(pid);
            if (it == out.end()) continue;
            for (const auto& u : uses)
            {
                if (u.start < r0 || u.start > r1) continue;
                if (u.cancelled && !u.interrupted) it->second[SC_Cancelled] += 1.0;
                else it->second[SC_Skills] += 1.0;
            }
        }
        // Time dead and distance, from the per-second track the caller reads.
        if (data.track)
            for (auto& [pid, v] : out) data.track(pid, r0, r1, v[SC_Dead], v[SC_Distance]);
        return out;
    }

    bool IsCountCol(int col)
    {
        return col == SC_Kills || col == SC_Deaths || col == SC_Ints || col == SC_IntsRecv || col == SC_Skills || col == SC_Cancelled;
    }

    std::string StatText(double v, int col, bool perMinute)
    {
        char b[32];
        if (col == SC_Dead && perMinute) { if (v < 0.5) return "-"; snprintf(b, sizeof b, "%d%%", (int)std::lround(v)); return b; }
        if (v < 0.05) return "-";
        if (col == SC_Dead) { snprintf(b, sizeof b, "%d s", (int)std::lround(v)); return b; }
        if (perMinute && IsCountCol(col)) { snprintf(b, sizeof b, "%.1f", v); return b; }
        const long long n = std::llround(v);
        std::string raw = std::to_string(n), s;
        for (int i = (int)raw.size() - 1, k = 0; i >= 0; --i, ++k)
        {
            if (k && k % 3 == 0) s.insert(s.begin(), ',');
            s.insert(s.begin(), raw[i]);
        }
        return s;
    }
}

namespace
{
    // One player's cell of a catalogue column over [r0, r1]: `v` is what sorts and ranks (already
    // per minute when asked), num / den a ratio's counts, n how many records a median rests on.
    struct ExtraCell { double v = 0.0, num = 0.0, den = 0.0; int n = 0; };

    double Median(std::vector<float> v)
    {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        const size_t h = v.size() / 2;
        return v.size() % 2 ? v[h] : 0.5 * (v[h - 1] + v[h]);
    }

    std::unordered_map<int, ExtraCell> ExtraValues(const StatsTable::Data& data, const MatchStats::StatDef& def,
                                                   float r0, float r1, bool perMinute)
    {
        using MatchStats::Agg;
        std::unordered_map<int, ExtraCell> out;
        auto recsOf = [&](const char* key) -> const std::vector<MatchStats::Record>* {
            if (!key) return nullptr;
            if (!data.stats) return nullptr;
            auto it = data.stats->find(key);
            return it == data.stats->end() ? nullptr : &it->second;
        };
        auto inside = [&](const MatchStats::Record& r) { return r.t >= r0 && r.t <= r1; };
        const float minutes = std::max(1.0f / 60.0f, (r1 - r0) / 60.0f);
        if (def.agg == Agg::Ratio)
        {
            if (const auto* num = recsOf(def.num)) for (const auto& r : *num) if (inside(r)) out[r.agent].num += 1.0;
            if (const auto* den = recsOf(def.den)) for (const auto& r : *den) if (inside(r)) out[r.agent].den += 1.0;
            for (auto& [pid, c] : out) c.v = c.den > 0.0 ? c.num / c.den : 0.0;
            return out;
        }
        const auto* recs = recsOf(def.key);
        if (!recs) return out;
        std::unordered_map<int, std::vector<float>> values;
        for (const auto& r : *recs)
        {
            if (def.agg == Agg::Time)
            {
                const double a = std::max<double>(r.t, r0), b = std::min<double>(r.t + r.dur, r1);
                if (b > a) out[r.agent].v += b - a;
                continue;
            }
            if (!inside(r)) continue;
            ExtraCell& c = out[r.agent];
            ++c.n;
            if (def.agg == Agg::Count) c.v += 1.0;
            else if (def.agg == Agg::Sum) c.v += r.value;
            else values[r.agent].push_back(r.value);
        }
        if (def.agg == Agg::Median)
            for (auto& [pid, list] : values) out[pid].v = Median(list);
        if (perMinute)
            for (auto& [pid, c] : out)
            {
                if (def.agg == Agg::Time) c.v = c.v / std::max(1.0f, r1 - r0) * 100.0;
                else if (def.agg == Agg::Count || def.agg == Agg::Sum) c.v /= minutes;
            }
        return out;
    }

    std::string ExtraText(const ExtraCell& c, const MatchStats::StatDef& def, bool perMinute)
    {
        using MatchStats::Agg;
        char b[48];
        const char* est = def.estimated ? "~" : "";
        switch (def.agg)
        {
        case Agg::Ratio:
            if (c.den <= 0.0) return "-";
            snprintf(b, sizeof b, "%d/%d  %d%%", (int)c.num, (int)c.den, (int)std::lround(c.v * 100.0));
            return b;
        case Agg::Median:
            if (c.n == 0) return "-";
            snprintf(b, sizeof b, "%s%.2f s", est, c.v);
            return b;
        case Agg::Time:
            if (c.v < 0.5) return "-";
            snprintf(b, sizeof b, perMinute ? "%s%d%%" : "%s%d s", est, (int)std::lround(c.v));
            return b;
        case Agg::Count:
            if (c.v < 0.05) return "-";
            if (perMinute) { snprintf(b, sizeof b, "%s%.1f", est, c.v); return b; }
            snprintf(b, sizeof b, "%s%d", est, (int)std::lround(c.v));
            return b;
        case Agg::Sum:
            break;
        }
        if (c.v < 0.05) return "-";
        const long long n = std::llround(c.v);
        std::string raw = std::to_string(n), s;
        for (int i = (int)raw.size() - 1, k = 0; i >= 0; --i, ++k)
        {
            if (k && k % 3 == 0) s.insert(s.begin(), ',');
            s.insert(s.begin(), raw[i]);
        }
        return est + s;
    }

    ImU32 ExtraHue(MatchStats::Hue h)
    {
        switch (h)
        {
        case MatchStats::Hue::Damage:    return IM_COL32(226, 80, 63, 0);
        case MatchStats::Hue::Healing:   return IM_COL32(108, 196, 86, 0);
        case MatchStats::Hue::Prevented: return IM_COL32(230, 200, 70, 0);
        case MatchStats::Hue::Interrupt: return IM_COL32(170, 110, 220, 0);
        case MatchStats::Hue::Energy:    return IM_COL32(80, 150, 235, 0);
        case MatchStats::Hue::Control:   return IM_COL32(232, 140, 60, 0);
        default:                         return IM_COL32(190, 196, 206, 0);
        }
    }

    // Favourite stats: starred in the picker, shown as columns every time, kept between sessions in
    // stats_favourites.txt beside gui_settings.ini (one stat key per line, in the order starred).
    std::vector<int> s_favs;          // kExtraBase + catalogue index
    bool s_favsLoaded = false;

    std::filesystem::path FavouritesFile() { return s_favFile; }

    // A fixed column is kept as "base:<name>". Once one fixed column is starred, the starred ones
    // replace the default set (the others start unticked).
    void LoadFavourites()
    {
        if (s_favsLoaded || s_favFile.empty()) return;   // read once the path is known
        s_favsLoaded = true;
        std::ifstream f(FavouritesFile());
        std::string key;
        bool anyBase = false;
        while (std::getline(f, key))
        {
            while (!key.empty() && (key.back() == '\r' || key.back() == ' ')) key.pop_back();
            if (key.empty()) continue;
            const int c = LayoutCol(key.c_str());
            if (c < 0 || std::find(s_favs.begin(), s_favs.end(), c) != s_favs.end()) continue;
            s_favs.push_back(c);
            anyBase |= c < SC_Count;
        }
        if (anyBase)
            for (bool& on : s_stats.on) on = false;
    }

    void SaveFavourites()
    {
        std::ofstream f(FavouritesFile(), std::ios::trunc);
        for (int c : s_favs)
        {
            if (c < SC_Count) f << "base:" << kStatCols[c].name << "\n";
            else if (const auto* d = ExtraDef(c)) f << d->key << "\n";
        }
    }

    bool IsFavourite(int c) { return std::find(s_favs.begin(), s_favs.end(), c) != s_favs.end(); }
    // A fixed column is on screen when ticked or starred.
    bool BaseShown(int c) { return s_stats.on[c] || IsFavourite(c); }

    // Starring keeps the column for good; unstarring leaves it shown for this session.
    void ToggleFavourite(int c)
    {
        auto it = std::find(s_favs.begin(), s_favs.end(), c);
        if (c < SC_Count)
        {
            // A fixed column: unstarring leaves it ticked for this session.
            if (it != s_favs.end()) { s_favs.erase(it); s_stats.on[c] = true; }
            else s_favs.push_back(c);
            SaveFavourites();
            return;
        }
        auto ex = std::find(s_stats.extra.begin(), s_stats.extra.end(), c);
        if (it != s_favs.end()) { s_favs.erase(it); if (ex == s_stats.extra.end()) s_stats.extra.push_back(c); }
        else { s_favs.push_back(c); if (ex != s_stats.extra.end()) s_stats.extra.erase(ex); }
        SaveFavourites();
    }

    // The catalogue's columns on screen: the favourites, then this session's picks.
    std::vector<int> ShownExtras()
    {
        std::vector<int> v;
        for (int c : s_favs) if (ExtraDef(c)) v.push_back(c);
        for (int c : s_stats.extra) if (ExtraDef(c) && !IsFavourite(c)) v.push_back(c);
        return v;
    }

    // A five-pointed star (the app font has no star glyph): filled gold, or an outline.
    void DrawStar(ImDrawList* dl, ImVec2 c, float r, bool filled, ImU32 col)
    {
        ImVec2 pts[10];
        for (int i = 0; i < 10; ++i)
        {
            const float a = -1.5707963f + i * 0.6283185f, rr = (i & 1) ? r * 0.45f : r;
            pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
        }
        if (filled)
            for (int i = 0; i < 10; ++i) dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], col);
        else
            dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.2f);
    }

    // A star button the size of a text line; true when clicked.
    bool StarButton(bool on)
    {
        const float h = ImGui::GetFrameHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##star", ImVec2(h, h));
        const bool hov = ImGui::IsItemHovered();
        DrawStar(ImGui::GetWindowDrawList(), ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.36f, on || hov,
                 on ? IM_COL32(240, 200, 80, 255) : hov ? IM_COL32(240, 200, 80, 140) : IM_COL32(150, 156, 165, 200));
        return clicked;
    }

    // Every keyword of the picker's search appears in the stat's label, head, group or help.
    bool MatchesSearch(const MatchStats::StatDef& d, const char* query)
    {
        auto lower = [](std::string s) { for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; };
        const std::string hay = lower(std::string(d.label) + " " + d.head + " " + d.group + " " + d.sub + " " + d.meaning + " " + d.help);
        std::string q = lower(query), word;
        for (size_t i = 0; i <= q.size(); ++i)
        {
            if (i == q.size() || q[i] == ' ')
            {
                if (!word.empty() && hay.find(word) == std::string::npos) return false;
                word.clear();
            }
            else word += q[i];
        }
        return true;
    }
}

// The columns panel (left of the table). Its own function, never inlined: inside DrawStatsTab's large
// frame MSVC once emitted an aligned SSE load (movdqa) to an 8-aligned stack slot here, and opening
// the picker crashed reading 0xFFFFFFFFFFFFFFFF (2026-10-30).
__declspec(noinline) void StatsTable::DrawColumnsPanel(Host& host)
{
    s_host = &host;
    LoadFavourites();
    // Clear all: every ticked column off (and the search); starred ones stay, they are kept on purpose.
    const float clearW = ImGui::CalcTextSize("Clear all").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearW - 6.0f);
    ImGui::InputTextWithHint("##search", "Search stats (kd, energy, flag...)", s_stats.search, sizeof s_stats.search);
    ImGui::SameLine(0.0f, 6.0f);
    if (ImGui::Button("Clear all"))
    {
        s_stats.extra.clear();
        for (bool& on : s_stats.on) on = false;
        s_stats.search[0] = 0;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Untick every column. Starred columns stay: unstar them to remove them.");
    ImGui::TextColored(ImVec4(0.59f, 0.61f, 0.65f, 1.0f), "Tick to show a column; star to keep it every time.");
    ImGui::Separator();
    ImGui::BeginChild("##picker_list", ImVec2(0.0f, 0.0f), false);
    const auto& cat = MatchStats::Catalog();
    int toggleFav = -1;
    // One stat's line: star, tick box, label, its short column name dimmed.
    auto row = [&](int c, const MatchStats::StatDef& d, const char* idSuffix) {
        ImGui::PushID(c);
        ImGui::PushID(idSuffix);
        const bool fav = IsFavourite(c);
        if (StarButton(fav)) toggleFav = c;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(fav ? "Favourite: shown every time. Click to unstar." : "Star: show this column every time.");
        ImGui::SameLine(0.0f, 2.0f);
        bool on = fav || std::find(s_stats.extra.begin(), s_stats.extra.end(), c) != s_stats.extra.end();
        if (fav) ImGui::BeginDisabled();
        if (ImGui::Checkbox(d.label, &on))
        {
            if (on) s_stats.extra.push_back(c);
            else s_stats.extra.erase(std::find(s_stats.extra.begin(), s_stats.extra.end(), c));
        }
        if (fav) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ColTooltip(c);
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.47f, 0.50f, 1.0f), "%s", d.head);
        ImGui::PopID();
        ImGui::PopID();
    };
    const bool searching = s_stats.search[0] != '\0';
    if (!searching && !s_favs.empty())
    {
        ImGui::TextColored(ImVec4(0.94f, 0.78f, 0.31f, 1.0f), "Favourites");
    }
    // A fixed column's line: star, tick box, label, its short name dimmed, like a stat's.
    auto baseRow = [&](int c, const char* idSuffix) {
        ImGui::PushID(c);
        ImGui::PushID(idSuffix);
        const bool fav = IsFavourite(c);
        if (StarButton(fav)) toggleFav = c;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(fav ? "Favourite: shown every time. Click to unstar." : "Star: show this column every time.");
        ImGui::SameLine(0.0f, 2.0f);
        bool on = BaseShown(c);
        if (fav) ImGui::BeginDisabled();
        if (ImGui::Checkbox(kStatCols[c].label, &on)) s_stats.on[c] = on;
        if (fav) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ColTooltip(c);
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextColored(ImVec4(0.45f, 0.47f, 0.50f, 1.0f), "%s", kStatCols[c].head);
        ImGui::PopID();
        ImGui::PopID();
    };
    if (!searching && !s_favs.empty())
        for (int c : s_favs)
        {
            if (c < SC_Count) baseRow(c, "fav");
            else if (const auto* d = ExtraDef(c)) row(c, *d, "fav");
        }
    auto baseMatches = [&](int c, const MatchStats::LayoutEntry& e) {
        MatchStats::StatDef probe;
        probe.label = kStatCols[c].label; probe.head = kStatCols[c].head; probe.help = kStatCols[c].help;
        probe.group = e.group; probe.sub = e.sub;
        return MatchesSearch(probe, s_stats.search);
    };
    // The tree: group, sub-group (unless it repeats the group's name), then its stats.
    std::string group, sub;
    bool any = false;
    for (const auto& e : MatchStats::Layout())
    {
        const int c = LayoutCol(e.key);
        if (c < 0) continue;
        const auto* d = ExtraDef(c);
        if (d ? !MatchesSearch(*d, s_stats.search) : !baseMatches(c, e)) continue;
        any = true;
        if (group != e.group)
        {
            group = e.group; sub.clear();
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::PushFont(s_host->Bold());
            ImGui::TextColored(ImVec4(0.94f, 0.78f, 0.31f, 1.0f), "%s", group.c_str());
            ImGui::PopFont();
        }
        if (sub != e.sub)
        {
            sub = e.sub;
            if (sub != group) ImGui::TextColored(ImVec4(0.72f, 0.64f, 0.42f, 1.0f), "  %s", sub.c_str());
        }
        ImGui::Indent(10.0f);
        if (d) row(c, *d, "all"); else baseRow(c, "all");
        ImGui::Unindent(10.0f);
    }
    // The detailed stats left out of the tree, found by search only.
    if (searching)
    {
        bool header = false;
        for (size_t i = 0; i < cat.size(); ++i)
        {
            const auto& d = cat[i];
            if (!d.hidden || !MatchesSearch(d, s_stats.search)) continue;
            if (!header)
            {
                header = true;
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                ImGui::TextColored(ImVec4(0.59f, 0.61f, 0.65f, 1.0f), "Detailed (search only)");
            }
            any = true;
            ImGui::Indent(10.0f);
            row(kExtraBase + (int)i, d, "all");
            ImGui::Unindent(10.0f);
        }
    }
    if (!any) ImGui::TextColored(ImVec4(0.59f, 0.61f, 0.65f, 1.0f), "%s", cat.empty() ? "No extra stats yet." : "No stat matches.");
    if (toggleFav >= 0) ToggleFavourite(toggleFav);
    ImGui::EndChild();
}


void StatsTable::DrawTable(const Data& data, Host& host, float r0, float r1)
{
    s_host = &host;
    LoadFavourites();
    const BattleLedger::Data& d = *data.ledger;
    const float minutes = std::max(1.0f / 60.0f, (r1 - r0) / 60.0f);
    auto raw = StatValues(data, r0, r1);
    std::vector<int> cols;
    for (int c = 0; c < SC_Count; ++c) if (BaseShown(c)) cols.push_back(c);
    for (int c : ShownExtras()) cols.push_back(c);
    std::unordered_map<int, std::unordered_map<int, ExtraCell>> extraCells;
    for (int c : cols)
        if (const auto* def = ExtraDef(c)) extraCells[c] = ExtraValues(data, *def, r0, r1, s_stats.perMinute);
    auto shown = [&](int pid, int c) -> double {
        if (ExtraDef(c))
        {
            const auto& cells = extraCells[c];
            auto it = cells.find(pid);
            return it == cells.end() ? 0.0 : it->second.v;
        }
        const double v = raw[pid][c];
        if (!s_stats.perMinute) return v;
        return c == SC_Dead ? v / std::max(1.0f, r1 - r0) * 100.0 : v / minutes;
    };
    // Whether a player has a value at all in a column (a median with no case, a ratio with no try).
    auto present = [&](int pid, int c) -> bool {
        const auto* def = ExtraDef(c);
        if (!def) return shown(pid, c) > 0.0;
        const auto& cells = extraCells[c];
        auto it = cells.find(pid);
        if (it == cells.end()) return false;
        if (def->agg == MatchStats::Agg::Median) return it->second.n > 0;
        if (def->agg == MatchStats::Agg::Ratio) return it->second.den > 0.0;
        return it->second.v > 0.0;
    };
    // Better first: the larger number, but the quicker one for a median (reactions, timings).
    auto better = [&](int a, int b, int c) {
        const auto* def = ExtraDef(c);
        if (def && def->agg == MatchStats::Agg::Median)
        {
            const bool pa = present(a, c), pb = present(b, c);
            if (pa != pb) return pa;
            return shown(a, c) < shown(b, c);
        }
        return shown(a, c) > shown(b, c);
    };
    auto cellText = [&](int pid, int c) -> std::string {
        if (const auto* def = ExtraDef(c))
        {
            const auto& cells = extraCells[c];
            auto it = cells.find(pid);
            return it == cells.end() ? std::string("-") : ExtraText(it->second, *def, s_stats.perMinute);
        }
        return StatText(shown(pid, c), c, s_stats.perMinute);
    };
    if (std::find(cols.begin(), cols.end(), s_stats.sort) == cols.end() && !cols.empty()) s_stats.sort = cols.front();

    const float lineH = ImGui::GetTextLineHeight();
    const float rowH = std::max(lineH + 8.0f, 24.0f), iconS = lineH + 2.0f;
    float nameW = 120.0f;
    for (int pid : d.players)
        if (auto it = d.agents.find(pid); it != d.agents.end())
            nameW = std::max(nameW, ImGui::CalcTextSize(it->second.name.c_str()).x);
    const float nameColW = iconS * 2.0f + 10.0f + nameW + 16.0f;
    float colW = 70.0f;
    for (int c : cols) colW = std::max(colW, ImGui::CalcTextSize(ColHead(c)).x + 26.0f);
    // Each column at the common width, a ratio's wider when "123/456  100%" needs it.
    std::vector<float> colX, colWs;
    float tableW = nameColW;
    for (int c : cols)
    {
        float w = colW;
        if (const auto* def = ExtraDef(c); def && def->agg == MatchStats::Agg::Ratio)
            w = std::max(w, ImGui::CalcTextSize("888/888  100%").x + 18.0f);
        colX.push_back(tableW);
        colWs.push_back(w);
        tableW += w;
    }

    // Each kind of figure in the colours of Bars and the client's digits: damage red, healing
    // green, prevented gold, interrupts purple; counts neutral.
    auto hue = [](int c) -> ImU32 {
        if (const auto* def = ExtraDef(c)) return ExtraHue(def->hue);
        switch (c)
        {
        case SC_Dealt: case SC_Taken:       return IM_COL32(226, 80, 63, 0);
        case SC_Healed: case SC_Received:   return IM_COL32(108, 196, 86, 0);
        case SC_Prevented:                  return IM_COL32(230, 200, 70, 0);
        case SC_Ints: case SC_IntsRecv:     return IM_COL32(170, 110, 220, 0);
        default:                            return IM_COL32(190, 196, 206, 0);
        }
    };
    ImFont* bold = s_host->Bold();
    const float fs = ImGui::GetFontSize();

    ImGui::BeginChild("##stats_table", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 cDim = IM_COL32(150, 156, 165, 255), cGold = IM_COL32(225, 190, 80, 255);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    float y = o.y;

    // Header: click a column to sort both teams by it.
    dl->AddText(ImVec2(o.x + 6.0f, y + (rowH - lineH) * 0.5f), cDim, "PLAYER");
    for (size_t k = 0; k < cols.size(); ++k)
    {
        const int c = cols[k];
        const float x = o.x + colX[k], cw = colWs[k];
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(c);
        if (ImGui::InvisibleButton("##sort", ImVec2(cw - 2.0f, rowH))) s_stats.sort = c;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hov) ColTooltip(c, "Click to sort.", true);
        const bool sorted = s_stats.sort == c;
        const float tw = ImGui::CalcTextSize(ColHead(c)).x;
        const float tx = x + cw - 14.0f - tw;
        dl->AddText(ImVec2(tx, y + (rowH - lineH) * 0.5f), sorted ? cGold : hov ? IM_COL32(216, 220, 226, 255) : cDim, ColHead(c));
        if (ColLowerBetter(c))
        {
            // "Lower is better": a small hollow down arrow after the name (the sort mark is filled, before it).
            const float ax = tx + tw + 2.0f, ay = y + (rowH - lineH) * 0.5f;
            dl->AddTriangle(ImVec2(ax, ay + 1.0f), ImVec2(ax + 6.0f, ay + 1.0f), ImVec2(ax + 3.0f, ay + 5.5f), IM_COL32(230, 158, 115, 220), 1.0f);
        }
        if (sorted)
        {
            const float cy = y + rowH * 0.5f;     // a small down triangle before the name
            dl->AddTriangleFilled(ImVec2(tx - 11.0f, cy - 2.0f), ImVec2(tx - 4.0f, cy - 2.0f), ImVec2(tx - 7.5f, cy + 2.5f), cGold);
            dl->AddLine(ImVec2(x + 4.0f, y + rowH - 1.0f), ImVec2(x + cw - 6.0f, y + rowH - 1.0f), cGold, 1.5f);
        }
    }
    y += rowH + 2.0f;

    for (int side = 0; side < 2; ++side)
    {
        const uint8_t team = Team::FromIndex01(side);
        const ImU32 teamCol = Team::IsBlue(team) ? IM_COL32(74, 200, 255, 255) : IM_COL32(255, 107, 107, 255);
        std::vector<int> rows;
        for (int pid : d.players)
            if (auto it = d.agents.find(pid); it != d.agents.end() && it->second.team == team) rows.push_back(pid);
        std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) { return better(a, b, s_stats.sort); });

        // Team heading over a team-coloured rule.
        dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + tableW, y), (teamCol & 0x00FFFFFF) | 0xB0000000, 2.0f);
        const std::string guild = !d.guild[side].empty() ? d.guild[side] : Team::IsBlue(team) ? "Blue team" : "Red team";
        dl->AddText(bold, fs, ImVec2(o.x + 6.0f, y + (rowH - lineH) * 0.5f), teamCol, guild.c_str());
        y += rowH;

        // The team's standouts per column: first, second and third (nonzero only, ties share).
        std::unordered_map<int, std::unordered_map<int, int>> rank;
        for (int c : cols)
        {
            std::vector<int> order;
            for (int pid : rows) if (present(pid, c)) order.push_back(pid);
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return better(a, b, c); });
            for (size_t i = 0; i < order.size() && i < 3; ++i)
                rank[c][order[i]] = i > 0 && shown(order[i], c) == shown(order[i - 1], c) ? rank[c][order[i - 1]] : (int)i + 1;
        }

        int stripe = 0;
        for (int pid : rows)
        {
            // Faint stripes, and the whole row lit under the mouse to follow one player across.
            const ImVec2 ra(o.x, y), rb(o.x + tableW, y + rowH);
            if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ra, rb))
                dl->AddRectFilled(ra, rb, IM_COL32(255, 255, 255, 16), 2.0f);
            else if (stripe & 1)
                dl->AddRectFilled(ra, rb, IM_COL32(255, 255, 255, 6), 2.0f);
            ++stripe;
            const BattleLedger::Agent& ag = d.agents.at(pid);
            float x = o.x + 6.0f;
            if (ImTextureID t = ag.prof ? s_host->ProfIcon(ag.prof) : nullptr)
                dl->AddImage(t, ImVec2(x, y + (rowH - iconS) * 0.5f), ImVec2(x + iconS, y + (rowH + iconS) * 0.5f));
            x += iconS + 2.0f;
            if (ImTextureID t = ag.sec ? s_host->ProfIcon(ag.sec) : nullptr)
                dl->AddImage(t, ImVec2(x, y + (rowH - iconS) * 0.5f), ImVec2(x + iconS, y + (rowH + iconS) * 0.5f),
                             ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 170));
            x += iconS + 8.0f;
            dl->AddText(ImVec2(x, y + (rowH - lineH) * 0.5f), teamCol, ag.name.c_str());
            if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ra, ImVec2(o.x + nameColW, rb.y)))
                host.PlayerTooltip(ag.name);
            for (size_t k = 0; k < cols.size(); ++k)
            {
                const int c = cols[k];
                const ImVec2 a(o.x + colX[k], y + 1.0f), b(a.x + colWs[k] - 2.0f, y + rowH - 1.0f);
                ImGui::SetCursorScreenPos(a);
                ImGui::PushID(pid * 32 + c);
                const bool clicked = ImGui::InvisibleButton("##cell", ImVec2(b.x - a.x, b.y - a.y));
                const bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                // Only the team's standouts are filled, in the column's colour: the first strongly
                // with a bold number, the second and third lightly; the rest are plain numbers.
                auto rit = rank[c].find(pid);
                const int place = rit == rank[c].end() ? 0 : rit->second;
                if (place) dl->AddRectFilled(a, b, hue(c) | ((ImU32)(place == 1 ? 105 : 38) << 24), 2.0f);
                if (hov) dl->AddRect(a, b, IM_COL32(225, 190, 80, 220), 2.0f);
                const std::string s = cellText(pid, c);
                ImFont* f = place == 1 ? bold : ImGui::GetFont();
                const float tw = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, s.c_str()).x;
                dl->AddText(f, fs, ImVec2(b.x - 8.0f - tw, y + (rowH - lineH) * 0.5f),
                            s == "-" ? IM_COL32(90, 88, 82, 255) : place == 1 ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 205, 212, 255),
                            s.c_str());
                if (hov && ExtraDef(c)) ImGui::SetTooltip("%s - %s: %s\n%s", ag.name.c_str(), ColLabel(c), s.c_str(),
                                                          ExtraDef(c)->meaning[0] ? ExtraDef(c)->meaning : ExtraDef(c)->help);
                else if (hov) ImGui::SetTooltip("%s - %s: %s\nClick to see how it was made (Breakdown, %s).", ag.name.c_str(), kStatCols[c].label,
                                                s.c_str(), BattleLedger::MetricName(kStatCols[c].ledgerTab));
                if (clicked && !ExtraDef(c)) host.OpenBars(pid, kStatCols[c].ledgerTab);
            }
            dl->AddLine(ImVec2(o.x, y + rowH), ImVec2(o.x + tableW, y + rowH), IM_COL32(255, 255, 255, 8));
            y += rowH;
        }
        // Team total (time dead per minute: the team's average share), on a faint band of the
        // team's colour under a rule.
        dl->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + tableW, y + rowH), (teamCol & 0x00FFFFFF) | 0x16000000, 2.0f);
        dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + tableW, y), (teamCol & 0x00FFFFFF) | 0x60000000, 1.0f);
        dl->AddText(bold, fs, ImVec2(o.x + 6.0f, y + (rowH - lineH) * 0.5f), cGold, "Team total");
        for (size_t k = 0; k < cols.size(); ++k)
        {
            const int c = cols[k];
            std::string s;
            if (const auto* def = ExtraDef(c))
            {
                // The team's own cell: counts and times summed (a time per minute: the average
                // share), a ratio over the summed tries, a median over every case of the team.
                ExtraCell tc;
                const auto& cells = extraCells[c];
                for (int pid : rows)
                    if (auto it = cells.find(pid); it != cells.end())
                    { tc.v += it->second.v; tc.num += it->second.num; tc.den += it->second.den; tc.n += it->second.n; }
                if (def->agg == MatchStats::Agg::Ratio) tc.v = tc.den > 0.0 ? tc.num / tc.den : 0.0;
                if (def->agg == MatchStats::Agg::Time && s_stats.perMinute && !rows.empty()) tc.v /= rows.size();
                if (def->agg == MatchStats::Agg::Median)
                {
                    std::vector<float> all;
                    auto it = data.stats ? data.stats->find(def->key) : MatchStats::Records::const_iterator();
                    std::unordered_set<int> mine(rows.begin(), rows.end());
                    if (data.stats && it != data.stats->end())
                        for (const auto& r : it->second)
                            if (r.t >= r0 && r.t <= r1 && mine.count(r.agent)) all.push_back(r.value);
                    tc.n = (int)all.size();
                    tc.v = Median(std::move(all));
                }
                s = ExtraText(tc, *def, s_stats.perMinute);
            }
            else
            {
                double sum = 0.0;
                for (int pid : rows) sum += shown(pid, c);
                if (c == SC_Dead && s_stats.perMinute && !rows.empty()) sum /= rows.size();
                s = StatText(sum, c, s_stats.perMinute);
            }
            const float tw = bold->CalcTextSizeA(fs, FLT_MAX, 0.0f, s.c_str()).x;
            dl->AddText(bold, fs, ImVec2(o.x + colX[k] + colWs[k] - 10.0f - tw, y + (rowH - lineH) * 0.5f), cGold, s.c_str());
        }
        y += rowH;
        // What the map did to this team: already inside the players' Taken and Healed, shown apart
        // on two rows, the map's own effects (catapults, coral, acid, Miasma, the healing water)
        // and the flux (Minion Apocalypse).
        for (int group = 0; group < 2; ++group)
        {
            const bool fluxRow = group == 1;
            double mapV[SC_Count] = {};
            std::map<int, long long> byEffect[2];         // 0 damage taken, 1 healing received
            std::unordered_set<int> mine(rows.begin(), rows.end());
            for (const auto& ln : d.lines)
            {
                if (ln.t < r0) continue;
                if (ln.t > r1) break;
                if (!BattleLedger::IsMapRow(ln.rowKey) || !mine.count(ln.dst)) continue;
                if ((ln.rowKey == BattleLedger::kRowFlux) != fluxRow) continue;
                const int m = BattleLedger::LineMetricTaker(ln.kind);
                if (m == BattleLedger::kTaken)         { mapV[SC_Taken] += ln.amount;    byEffect[0][ln.rowKey] += ln.amount; }
                else if (m == BattleLedger::kReceived) { mapV[SC_Received] += ln.amount; byEffect[1][ln.rowKey] += ln.amount; }
            }
            const bool showTaken = BaseShown(SC_Taken) && mapV[SC_Taken] > 0.0;
            const bool showHealed = BaseShown(SC_Received) && mapV[SC_Received] > 0.0;
            if (showTaken || showHealed)
            {
                if (s_stats.perMinute) { mapV[SC_Taken] /= minutes; mapV[SC_Received] /= minutes; }
                // One icon per effect that struck (or healed) the team, then the label.
                std::set<int> keys;
                for (const auto& list : byEffect) for (const auto& [key, amount] : list) keys.insert(key);
                float ix = o.x + 6.0f;
                for (int key : keys)
                {
                    const ImVec2 a(ix, y + (rowH - iconS) * 0.5f), b(ix + iconS, y + (rowH + iconS) * 0.5f);
                    if (ImTextureID ic = BattleLedger::MapRowIcon(host, key))
                        dl->AddImageRounded(ic, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 2.0f);
                    if (ImGui::IsMouseHoveringRect(a, b)) ImGui::SetTooltip("%s", BattleLedger::MapRowName(key));
                    ix += iconS + 2.0f;
                }
                const float smallFs = fs * 0.9f;
                dl->AddText(ImGui::GetFont(), smallFs, ImVec2(ix + 6.0f, y + (rowH - smallFs) * 0.5f), IM_COL32(140, 146, 154, 255),
                            fluxRow ? "Flux" : "Map effects");
                for (size_t k = 0; k < cols.size(); ++k)
                {
                    const int c = cols[k];
                    if (c != SC_Taken && c != SC_Received) continue;
                    const ImVec2 a(o.x + colX[k], y + 1.0f), b(a.x + colWs[k] - 2.0f, y + rowH - 1.0f);
                    const std::string s = StatText(mapV[c], c, s_stats.perMinute);
                    const float tw = ImGui::GetFont()->CalcTextSizeA(smallFs, FLT_MAX, 0.0f, s.c_str()).x;
                    dl->AddText(ImGui::GetFont(), smallFs, ImVec2(b.x - 8.0f - tw, y + (rowH - smallFs) * 0.5f),
                                s == "-" ? IM_COL32(90, 88, 82, 255) : IM_COL32(140, 146, 154, 255), s.c_str());
                    const auto& list = byEffect[c == SC_Taken ? 0 : 1];
                    if (!list.empty() && ImGui::IsMouseHoveringRect(a, b))
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(fluxRow ? "Damage taken from the flux"
                                             : c == SC_Taken ? "Damage taken from the map" : "Healing received from the map");
                        for (const auto& [key, amount] : list)
                            ImGui::Text("%s  %s", BattleLedger::MapRowName(key),
                                        StatText(s_stats.perMinute ? amount / minutes : (double)amount, c, s_stats.perMinute).c_str());
                        ImGui::EndTooltip();
                    }
                }
                dl->AddLine(ImVec2(o.x, y + rowH), ImVec2(o.x + tableW, y + rowH), IM_COL32(255, 255, 255, 8));
                y += rowH;
            }
        }
        y += 10.0f;
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, y));
    ImGui::Dummy(ImVec2(tableW, 0.0f));
    ImGui::EndChild();
}

StatsTable::Data StatsTable::MakeData(const Loaded& file, const SkillDatabaseView* skills)
{
    Data data;
    data.ledger = &file.ledger;
    data.stats = &file.stats;
    data.skills = skills;
    data.deaths = file.deaths;
    data.track = [&file](int agent, float a0, float a1, double& dead, double& dist) {
        const int pre = file.gateSeconds;
        for (const Track& tr : file.tracks)
        {
            if (tr.agent != agent) continue;
            const int n = (int)tr.pos.size() / 4;
            if (n == 0) return;
            const int a = std::clamp((int)std::ceil(a0) + pre, 0, n - 1), b = std::clamp((int)std::floor(a1) + pre, 0, n - 1);
            for (int i = a; i <= b; ++i)
            {
                if (tr.pos[i * 4 + 3] == 1) dead += 1.0;
                if (i > a && tr.pos[i * 4 + 3] == 0 && tr.pos[(i - 1) * 4 + 3] == 0)
                {
                    const double dx = tr.pos[i * 4] - tr.pos[(i - 1) * 4], dy = tr.pos[i * 4 + 1] - tr.pos[(i - 1) * 4 + 1];
                    dist += std::sqrt(dx * dx + dy * dy);
                }
            }
            return;
        }
    };
    return data;
}

void StatsTable::SetFavouritesFile(const std::filesystem::path& file) { s_favFile = file; }
bool StatsTable::PerMinute() { return s_stats.perMinute; }
void StatsTable::SetPerMinute(bool on) { s_stats.perMinute = on; }
