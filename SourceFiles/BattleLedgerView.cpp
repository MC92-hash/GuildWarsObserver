#include "pch.h"
#include "BattleLedger.h"
#include "BitmapFont.h"
#include "TeamColors.h"
#include "DamageAttribution.h"
#include "CatapultDangerZone.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <array>
#include <map>

// ---------------------------------------------------------------------------
// The Damage & Healing view (BattleLedger.h): the metric tabs, the replay's time strip, the board
// or team matrix and the selected player's breakdown, drawn from a BattleLedger::Data alone. The
// replay window and the Library's Stats tab each keep their own ViewState and Host.
// Same face as the Character panel and the Piano Roll: the bronze chrome, gold headings,
// hand-drawn rows on hairlines, flat bars on a faint track, team colours on names.
// ---------------------------------------------------------------------------

namespace BattleLedger
{
namespace
{
    // The client's condition "skills" by their own names (the skill data has none for them).
    std::string ConditionName(int id)
    {
        switch (id)
        {
        case 478:  return "Bleeding";
        case 479:  return "Blind";
        case 480:  return "Burning";
        case 481:  return "Crippled";
        case 482:  return "Deep Wound";
        case 483:  return "Disease";
        case 484:  return "Poison";
        case 485:  return "Dazed";
        case 486:  return "Weakness";
        case 2077: return "Cracked Armor";
        default:   return "Condition " + std::to_string(id);
        }
    }

    const char* kMetricName[kMetricCount] = { "Damage dealt", "Damage taken", "Healing done", "Healing received",
                                              "Damage prevented", "Interrupts", "Interrupts received" };
    const char* kScopeName[] = { "Full match", "Up to now", "Last 60 s", "Selection" };

    // The Character panel's palette (ReplayWindow_CharacterPanel.cpp, ReplayWindow_PianoRoll.cpp).
    constexpr ImU32 kGold      = IM_COL32(225, 190, 80, 255);
    constexpr ImU32 kNameText  = IM_COL32(216, 220, 226, 255);
    constexpr ImU32 kMuted     = IM_COL32(122, 128, 135, 255);
    constexpr ImU32 kSoft      = IM_COL32(170, 176, 184, 255);
    constexpr ImU32 kTabText   = IM_COL32(153, 160, 170, 255);
    constexpr ImU32 kRule      = IM_COL32(255, 255, 255, 26);
    constexpr ImU32 kBronze    = IM_COL32(0x66, 0x54, 0x26, 0x66);
    constexpr ImU32 kHairline  = IM_COL32(255, 255, 255, 8);
    constexpr ImU32 kTrack     = IM_COL32(255, 255, 255, 15);
    constexpr ImU32 kHover     = IM_COL32(255, 255, 255, 12);
    constexpr ImU32 kSelFill   = IM_COL32(225, 190, 80, 30);
    // Team name colours of the Combat Log and the Piano Roll.
    constexpr ImU32 kBlueText  = IM_COL32(74, 200, 255, 255);
    constexpr ImU32 kRedText   = IM_COL32(255, 107, 107, 255);
    constexpr ImU32 kBlueBar   = IM_COL32(74, 144, 216, 178);
    constexpr ImU32 kRedBar    = IM_COL32(208, 72, 72, 178);
    constexpr ImU32 kDamage    = IM_COL32(255, 115, 102, 255);
    constexpr ImU32 kHealing   = IM_COL32(140, 230, 128, 255);
    constexpr ImU32 kPrevent   = IM_COL32(230, 205, 110, 255);
    constexpr ImU32 kPurple    = IM_COL32(191, 97, 255, 255);   // mockup / Combat Log interrupt purple
    constexpr ImU32 kOrange    = IM_COL32(255, 159, 64, 255);

    ImU32 TeamText(int team) { return Team::IsBlue(team) ? kBlueText : Team::IsRed(team) ? kRedText : kNameText; }
    ImU32 TeamBar(int team)  { return Team::IsBlue(team) ? kBlueBar : Team::IsRed(team) ? kRedBar : IM_COL32(200, 200, 200, 150); }
    ImU32 WithAlpha(ImU32 c, int a) { return (c & 0x00FFFFFF) | ((ImU32)a << 24); }
    ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

    std::string FmtN(long long v)
    {
        std::string raw = std::to_string(v < 0 ? -v : v), out;
        for (int i = (int)raw.size() - 1, n = 0; i >= 0; --i, ++n)
        {
            if (n && n % 3 == 0) out.insert(out.begin(), ',');
            out.insert(out.begin(), raw[i]);
        }
        return v < 0 ? "-" + out : out;
    }
    std::string FmtT(float t)
    {
        const int s = std::max(0, (int)t);
        char b[16]; snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60); return b;
    }
    std::string FmtMs(int ms) { return ms < 0 ? std::string("-") : FmtN(ms) + " ms"; }
    std::string Pct(long long a, long long b) { return b ? std::to_string((int)std::lround(100.0 * a / b)) + "%" : std::string("-"); }
    int Median(std::vector<int> v)
    {
        if (v.empty()) return -1;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    void Help(const char* text)
    {
        ImGui::TextDisabled("(?)");
        if (!ImGui::IsItemHovered()) return;
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // A heading tab: plain text, gold and underlined when chosen, like the panel headings.
    bool TextTab(const char* label, bool active)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 ts = ImGui::CalcTextSize(label), p = ImGui::GetCursorScreenPos();
        const ImVec2 sz(ts.x + 14.f, ts.y + 9.f);
        const bool clicked = ImGui::InvisibleButton(label, sz);
        const bool hov = ImGui::IsItemHovered();
        if (hov && !active) dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), kHover, 3.f);
        dl->AddText(ImVec2(p.x + 7.f, p.y + 3.f), active ? kGold : hov ? kNameText : kTabText, label);
        if (active) dl->AddRectFilled(ImVec2(p.x + 7.f, p.y + sz.y - 2.f), ImVec2(p.x + sz.x - 7.f, p.y + sz.y), kGold, 1.f);
        return clicked;
    }

    // The Combat Log's filter pill (ReplayWindow_CombatLog.cpp FilterPill), with an optional icon:
    // round, faint when off, dark gold with a gold edge when on.
    bool IconTab(const char* label, ImTextureID icon, bool active)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float iconS = 16.f, h = ImGui::GetTextLineHeight() + 10.f;
        const ImVec2 ts = ImGui::CalcTextSize(label), p = ImGui::GetCursorScreenPos();
        const float w = ts.x + 22.f + (icon ? iconS + 5.f : 0.f);
        const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
        const bool hov = ImGui::IsItemHovered();
        const ImVec2 q(p.x + w, p.y + h);
        dl->AddRectFilled(p, q, active ? (hov ? IM_COL32(51, 41, 15, 255) : IM_COL32(36, 28, 10, 255))
                                       : IM_COL32(255, 255, 255, hov ? 31 : 13), h * 0.5f);
        dl->AddRect(p, q, active ? IM_COL32(255, 214, 100, 204) : IM_COL32(255, 255, 255, 20), h * 0.5f);
        float x = p.x + 11.f;
        if (icon)
        {
            dl->AddImage(icon, ImVec2(x, p.y + (h - iconS) * 0.5f), ImVec2(x + iconS, p.y + (h + iconS) * 0.5f));
            x += iconS + 5.f;
        }
        dl->AddText(ImVec2(x, p.y + (h - ts.y) * 0.5f), active ? IM_COL32(255, 232, 176, 255) : IM_COL32(153, 163, 176, 255), label);
        return clicked;
    }

    // A number in the client's own digits (the overhead damage / heal / energy fonts), thousands
    // grouped by a thin gap since the atlas has no comma. Returns the width drawn.
    float GlyphNumber(ImDrawList* dl, const BitmapFont* bf, long long v, float x, float yTop, float h, bool alignRight)
    {
        std::string raw = std::to_string(v < 0 ? -v : v);
        std::vector<std::string> groups;
        for (int end = (int)raw.size(); end > 0; end -= 3) groups.insert(groups.begin(), raw.substr(std::max(0, end - 3), end - std::max(0, end - 3)));
        if (v < 0) groups.front() = "-" + groups.front();
        if (!bf || !bf->srv.Get())
        {
            const std::string s = (v < 0 ? "-" : "") + raw;
            const float w = ImGui::CalcTextSize(s.c_str()).x;
            dl->AddText(ImVec2(alignRight ? x - w : x, yTop), kNameText, s.c_str());
            return w;
        }
        const float gap = h * 0.22f;
        float w = gap * (groups.size() - 1);
        for (const std::string& g : groups) w += bf->MeasureString(g.c_str(), h);
        float cx = alignRight ? x - w : x;
        for (const std::string& g : groups)
        {
            const float gw = bf->MeasureString(g.c_str(), h);
            bf->DrawString(dl, g.c_str(), cx + gw * 0.5f, yTop + h * 0.5f, h, 255);
            cx += gw + gap;
        }
        return w;
    }

    // Headline figures (KPIs) on one faint panel: a small uppercase label, then the number in the
    // client's digits with a short note after it.
    struct Kpi { long long value; std::string suffix, label; bool none = false; };
    void KpiRow(const BitmapFont* bf, const std::vector<Kpi>& kpis)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float fs = ImGui::GetFontSize(), digitH = 22.f, h = fs + digitH + 26.f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float W = ImGui::GetContentRegionAvail().x;
        dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + h), IM_COL32(255, 255, 255, 10), 4.f);
        ImFont* lf = ImGui::GetFont();
        float x = p0.x + 14.f;
        for (const Kpi& k : kpis)
        {
            std::string up = k.label;
            for (char& c : up) c = (char)std::toupper((unsigned char)c);
            dl->AddText(lf, fs, ImVec2(x, p0.y + 10.f), kSoft, up.c_str());
            const float ny = p0.y + 14.f + fs;
            float w = 0.f;
            if (k.none) { dl->AddText(ImVec2(x, ny + (digitH - fs) * 0.5f), kMuted, "-"); w = 10.f; }
            else w = GlyphNumber(dl, bf, k.value, x, ny, digitH, false);
            if (!k.none && !k.suffix.empty())
            {
                dl->AddText(ImVec2(x + w + 5.f, ny + digitH - fs - 2.f), kSoft, k.suffix.c_str());
                w += 5.f + ImGui::CalcTextSize(k.suffix.c_str()).x;
            }
            w = std::max(w, lf->CalcTextSizeA(fs, FLT_MAX, 0.f, up.c_str()).x);
            x += w + 40.f;
        }
        ImGui::Dummy(ImVec2(W, h));
    }

    // A row of pills (the Combat Log's filter pills); returns the clicked index, or -1. A disabled
    // choice draws dim but still reports its click.
    int Segmented(const char* id, const std::vector<std::string>& labels, int active, const std::vector<bool>* enabled = nullptr)
    {
        ImGui::PushID(id);
        int clicked = -1;
        for (size_t i = 0; i < labels.size(); ++i)
        {
            ImGui::PushID((int)i);
            const bool en = !enabled || (*enabled)[i];
            if (!en) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f);
            if (IconTab(labels[i].c_str(), nullptr, (int)i == active)) clicked = (int)i;
            if (!en) ImGui::PopStyleVar();
            ImGui::PopID();
            if (i + 1 < labels.size()) ImGui::SameLine(0.f, 4.f);
        }
        ImGui::PopID();
        return clicked;
    }

    // The player's number in every tab as small boxes (the mockup's stat tiles): an uppercase label
    // over the number in the client's digits; the current tab dark gold with a gold edge.
    int StatStrip(const char* const* labels, const std::vector<long long>& values, const std::vector<const BitmapFont*>& fonts, int active)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int n = (int)values.size();
        const float fs = ImGui::GetFontSize();
        ImFont* lf = ImGui::GetFont();
        const float digitH = 14.f, h = fs + digitH + 16.f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        float x = p0.x;
        int clicked = -1;
        for (int i = 0; i < n; ++i)
        {
            std::string up = labels[i];
            for (char& c : up) c = (char)std::toupper((unsigned char)c);
            const float w = std::max(92.f, lf->CalcTextSizeA(fs, FLT_MAX, 0.f, up.c_str()).x + 18.f);
            const ImVec2 a(x, p0.y), b(x + w, p0.y + h);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##stat", ImVec2(w, h))) clicked = i;
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool on = i == active;
            dl->AddRectFilled(a, b, on ? IM_COL32(36, 28, 10, 255) : IM_COL32(255, 255, 255, hov ? 18 : 8), 2.f);
            dl->AddRect(a, b, on ? IM_COL32(255, 214, 100, 204) : IM_COL32(255, 255, 255, 20), 2.f);
            dl->AddText(lf, fs, ImVec2(a.x + 8.f, a.y + 5.f), on ? IM_COL32(255, 232, 176, 255) : kSoft, up.c_str());
            GlyphNumber(dl, fonts[i], values[i], a.x + 8.f, a.y + 8.f + fs, digitH, false);
            x += w + 5.f;
        }
        ImGui::SetCursorScreenPos(p0);
        ImGui::Dummy(ImVec2(x - p0.x, h));
        return clicked;
    }

    std::string Ellipsize(std::string s, float width)
    {
        if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
        while (!s.empty() && ImGui::CalcTextSize((s + "...").c_str()).x > width) s.pop_back();
        return s + "...";
    }
    // At most two lines of `width`; the second ends in "..." when the rest does not fit.
    std::pair<std::string, std::string> TwoLines(const std::string& s, float width)
    {
        std::vector<std::string> words;
        for (size_t i = 0; i < s.size();)
        {
            const size_t j = s.find(' ', i);
            words.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
            if (j == std::string::npos) break;
            i = j + 1;
        }
        std::string l1, l2;
        size_t k = 0;
        for (; k < words.size(); ++k)
        {
            const std::string t = l1.empty() ? words[k] : l1 + " " + words[k];
            if (!l1.empty() && ImGui::CalcTextSize(t.c_str()).x > width) break;
            l1 = t;
        }
        l1 = Ellipsize(l1, width);
        for (; k < words.size(); ++k) l2 += (l2.empty() ? "" : " ") + words[k];
        return { l1, Ellipsize(l2, width) };
    }
    // A party label without its "Mo/A20" build prefix, which the profession icon already shows.
    std::string ShortName(const std::string& n)
    {
        const size_t sp = n.find(' ');
        return sp != std::string::npos && n.find('/') < sp ? n.substr(sp + 1) : n;
    }

    // A flat bar on a faint track, in fill's own alpha: kBarSoft for a row at rest, full for the
    // selected or hovered one, so the numbers stay the brightest thing on the board.
    constexpr int kBarSoft = 115;
    void Bar(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, ImU32 fill)
    {
        if (b.x <= a.x) return;
        const float r = std::min(3.f, (b.y - a.y) * 0.5f);
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 18), r);
        if (frac > 0.f)
            dl->AddRectFilled(a, ImVec2(a.x + std::max(3.f, (b.x - a.x) * std::min(1.f, frac)), b.y), fill, r);
    }

    // Gold UPPERCASE section label over a bronze rule, like the Piano Roll card's sections.
    void Section(const char* label, const char* caption)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        dl->AddText(p, kGold, label);
        if (caption)
        {
            const float cw = ImGui::CalcTextSize(caption).x;
            dl->AddText(ImVec2(p.x + w - cw, p.y), kMuted, caption);
        }
        const float y = p.y + ImGui::GetTextLineHeight() + 3.f;
        dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + w, y), kBronze);
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight() + 7.f));
    }

    // Right-aligned text ending at xr.
    void RText(ImDrawList* dl, float xr, float y, ImU32 col, const std::string& s)
    {
        dl->AddText(ImVec2(xr - ImGui::CalcTextSize(s.c_str()).x, y), col, s.c_str());
    }
}

bool Pill(const char* label, bool active) { return IconTab(label, nullptr, active); }

const char* MetricName(int metric) { return metric >= 0 && metric < kMetricCount ? kMetricName[metric] : ""; }

bool GivingMetric(int m) { return m == kDealt || m == kHealed || m == kPrevented || m == kInts; }

int LineMetricGiver(Kind k)
{
    switch (k)
    {
    case Kind::Damage:      return kDealt;
    case Kind::Heal:
    case Kind::DivineFavor: return kHealed;
    case Kind::Prevented:   return kPrevented;
    default:                return -1;
    }
}
int LineMetricTaker(Kind k)
{
    switch (k)
    {
    case Kind::Damage:
    case Kind::SelfCost:    return kTaken;
    case Kind::Heal:
    case Kind::DivineFavor: return kReceived;
    default:                return -1;
    }
}

std::unordered_map<int, Totals> SumTotals(const Data& d, float r0, float r1)
{
    std::unordered_set<int> board(d.players.begin(), d.players.end());
    std::unordered_map<int, Totals> tot;
    for (const Line& ln : d.lines)
    {
        if (ln.t < r0) continue;
        if (ln.t > r1) break;
        if (int m = LineMetricGiver(ln.kind); m >= 0 && board.count(ln.src)) tot[ln.src].v[m] += ln.amount;
        if (int m = LineMetricTaker(ln.kind); m >= 0 && board.count(ln.dst)) tot[ln.dst].v[m] += ln.amount;
    }
    for (const Interrupt& li : d.ints)
    {
        if (li.t < r0 || li.t > r1) continue;
        Totals& s = tot[li.src];
        Totals& t = tot[li.dst];
        s.intAtt++; t.recvAtt++;
        if (li.outcome == Interrupt::Landed) { s.v[kInts]++; t.v[kIntsRecv]++; }
        if (li.outcome == Interrupt::Faked)  { s.intFake++; t.recvFake++; }
    }
    return tot;
}

namespace
{
    // Who is who, from the data's agents.
    struct Who { int id; std::string name; int team; int prof, sec; };
    struct People
    {
        const Data& d;
        std::vector<Who> players;
        std::unordered_map<int, size_t> whoIx;
        explicit People(const Data& data) : d(data)
        {
            for (int pid : d.players)
            {
                auto it = d.agents.find(pid);
                if (it == d.agents.end()) continue;
                whoIx[pid] = players.size();
                players.push_back({ pid, it->second.name, it->second.team, it->second.prof, it->second.sec });
            }
        }
        int TeamOf(int agent) const
        {
            auto it = d.agents.find(agent);
            return it == d.agents.end() ? 0 : it->second.team;
        }
        std::string NameOf(int agent) const
        {
            auto it = d.agents.find(agent);
            if (it == d.agents.end()) return agent ? "Agent " + std::to_string(agent) : std::string("The map");
            return it->second.name;
        }
        int ProfOf(int agent) const
        {
            auto it = d.agents.find(agent);
            return it == d.agents.end() ? 0 : it->second.prof;
        }
        bool IsLord(int agent) const
        {
            auto it = d.agents.find(agent);
            return it != d.agents.end() && (it->second.lord || it->second.name == "Guild Lord");
        }
        std::string GuildLabel(int team) const
        {
            const std::string& g = d.guild[Team::IsBlue(team) ? 0 : 1];
            return !g.empty() ? g : Team::IsBlue(team) ? "Blue team" : "Red team";
        }
        std::string TagOf(int team) const
        {
            const std::string& tag = d.tag[Team::IsBlue(team) ? 0 : 1];
            return tag.empty() ? std::string(Team::IsBlue(team) ? "Blue" : "Red") : tag;
        }
    };
}

void DrawMetricTabs(Host& host, ViewState& st)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // ===================== Tabs =====================
    // On a band of their own, each with the Match Library's icon for the same figure.
    const ImTextureID tabIcon[kMetricCount] = {
        host.UiIcon("..\\Match library\\Dmg_Out-removebg-preview.png"),
        host.UiIcon("..\\Match library\\Dmg_In-removebg-preview.png"),
        host.UiIcon("..\\Match library\\Heal_Out-removebg-preview.png"),
        host.UiIcon("..\\Match library\\Heal_In-removebg-preview.png"),
        host.SkillIcon(245),     // Protective Spirit
        host.UiIcon("..\\Match library\\Rupts_Out.png"),
        host.UiIcon("..\\Match library\\Rupt_In-removebg-preview.png"),
    };
    for (int m = 0; m < kMetricCount; ++m)
    {
        ImGui::PushID(m);
        if (IconTab(kMetricName[m], tabIcon[m], st.metric == m)) { st.metric = m; st.openRows.clear(); }
        ImGui::PopID();
        ImGui::SameLine(0.f, m == kDealt + 1 || m == kPrevented ? 12.f : 4.f);
    }
    {
        const float w = ImGui::CalcTextSize("Board").x + ImGui::CalcTextSize("Team matrix").x + 50.f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.f, ImGui::GetWindowContentRegionMax().x - w));
        if (IconTab("Board", nullptr, !st.matrix)) st.matrix = false;
        ImGui::SameLine(0.f, 4.f);
        if (IconTab("Team matrix", nullptr, st.matrix)) st.matrix = true;
    }
    ImGui::Dummy(ImVec2(0.f, 4.f));
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x, p.y - 1.f), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 1.f), kRule);
        ImGui::Dummy(ImVec2(0.f, 4.f));
    }}

void DrawTimeStrip(const Data& d, ViewState& st, const StripInputs& in, float& r0, float& r1)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const People who(d);
    const auto& whoIx = who.whoIx;
    auto TeamOf = [&](int a) { return who.TeamOf(a); };
    auto TagOf = [&](int team) { return who.TagOf(team); };
    const float lineH = ImGui::GetTextLineHeight();
    const float tEnd = std::max(1.f, in.tEnd), tNow = std::clamp(in.tNow, 0.f, tEnd);
    r0 = 0.f; r1 = tEnd;
    if (st.scope == kPlayhead) r1 = tNow;
    else if (st.scope == kLast60) { r1 = tNow; r0 = std::max(0.f, tNow - 60.f); }
    else if (st.scope == kSelection && st.hasSel) { r0 = st.selA; r1 = st.selB; }
    const bool intsTab = st.metric == kInts || st.metric == kIntsRecv;

    // ===================== Time strip =====================
    {
        // The stretch to count, first: it decides every number below.
        {
            std::vector<std::string> labels;
            std::vector<bool> enabled;
            for (int s = 0; s < 4; ++s)
            {
                labels.push_back(s == kSelection && st.hasSel ? "Selection " + FmtT(st.selA) + "-" + FmtT(st.selB) : std::string(kScopeName[s]));
                enabled.push_back(s != kSelection || st.hasSel);
            }
            const int c = Segmented("##scope", labels, st.scope, &enabled);
            if (c == kSelection && !st.hasSel) st.selHintUntil = ImGui::GetTime() + 3.0;
            else if (c >= 0) st.scope = c;
        }
        ImGui::SameLine(0.f, 14.f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(V4(kSoft), "Playback %s   ·   counting %s-%s", FmtT(tNow).c_str(), FmtT(r0).c_str(), FmtT(r1).c_str());
        if (ImGui::GetTime() < st.selHintUntil)
        {
            ImGui::SameLine(0.f, 12.f);
            ImGui::TextColored(V4(kGold), "Drag on the strip to make a selection.");
        }
        ImGui::SameLine();
        Help("Both teams over the match, six seconds per bar: Blue above the line, Red below. Drag on the "
             "strip to count only that stretch; double-click it to go back to the full match. A triangle is a "
             "spike (three or more enemy damage skills on one player within 1.5 s), a cross a death.");

        const float W = ImGui::GetContentRegionAvail().x, H = 76.f, padL = 40.f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##strip", ImVec2(W, H));
        const bool hov = ImGui::IsItemHovered();
        auto xOf = [&](float t) { return p.x + padL + t / tEnd * (W - padL - 4.f); };
        auto tOf = [&](float x) { return std::clamp((x - p.x - padL) / (W - padL - 4.f) * tEnd, 0.f, tEnd); };
        if (ImGui::IsItemActivated()) st.dragFrom = tOf(ImGui::GetIO().MousePos.x);
        if (ImGui::IsItemActive() && st.dragFrom >= 0.f)
        {
            const float a = std::min(st.dragFrom, tOf(ImGui::GetIO().MousePos.x)), b = std::max(st.dragFrom, tOf(ImGui::GetIO().MousePos.x));
            if (b - a > 2.f) { st.selA = a; st.selB = b; st.hasSel = true; st.scope = kSelection; }
        }
        if (ImGui::IsItemDeactivated()) st.dragFrom = -1.f;
        if (hov && ImGui::IsMouseDoubleClicked(0)) { st.hasSel = false; st.scope = kFull; }

        dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), IM_COL32(0, 0, 0, 110), 3.f);
        const float mid = p.y + H * 0.5f;
        constexpr float kBin = 6.f;
        const int n = std::max(1, (int)std::ceil(tEnd / kBin));
        std::vector<float> top(n), bot(n);
        const bool byTaker = st.metric == kTaken || st.metric == kReceived || st.metric == kIntsRecv;
        auto Put = [&](float t, int agent, float v) {
            const int team = TeamOf(agent);
            if (!Team::IsSide(team)) return;
            const int b = std::clamp((int)(t / kBin), 0, n - 1);
            (Team::IsBlue(team) ? top : bot)[b] += v;
        };
        if (intsTab)
        {
            for (const Interrupt& li : d.ints)
                if (li.outcome == Interrupt::Landed) Put(li.t, byTaker ? li.dst : li.src, 1.f);
        }
        else
            for (const Line& ln : d.lines)
            {
                const int m = byTaker ? LineMetricTaker(ln.kind) : LineMetricGiver(ln.kind);
                if (m == st.metric) Put(ln.t, byTaker ? ln.dst : ln.src, (float)ln.amount);
            }
        float mx = 1.f;
        for (int i = 0; i < n; ++i) mx = std::max({ mx, top[i], bot[i] });
        for (float t = 120.f; t < tEnd; t += 120.f)
        {
            const float x = xOf(t);
            dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + H), IM_COL32(255, 255, 255, 14));
            dl->AddText(ImVec2(x + 3.f, p.y + 1.f), IM_COL32(150, 156, 165, 160), FmtT(t).c_str());
        }
        const float bw = (W - padL - 4.f) / n;
        for (int i = 0; i < n; ++i)
        {
            const float x = p.x + padL + i * bw;
            const float a = top[i] / mx * (H * 0.5f - 4.f), b = bot[i] / mx * (H * 0.5f - 4.f);
            if (a > 0.f) dl->AddRectFilled(ImVec2(x, mid - a), ImVec2(x + std::max(1.f, bw - 0.6f), mid), kBlueBar);
            if (b > 0.f) dl->AddRectFilled(ImVec2(x, mid), ImVec2(x + std::max(1.f, bw - 0.6f), mid + b), kRedBar);
        }
        dl->AddLine(ImVec2(p.x + padL, mid), ImVec2(p.x + W, mid), IM_COL32(0, 0, 0, 160));
        dl->AddText(ImVec2(p.x + 5.f, mid - lineH - 6.f), kBlueText, TagOf(Team::Blue).c_str());
        dl->AddText(ImVec2(p.x + 5.f, mid + 6.f), kRedText, TagOf(Team::Red).c_str());
        if (st.metric == kDealt || st.metric == kTaken)
            for (const auto& [firstHit, target] : in.spikes)
            {
                // On the side that took it.
                const bool blueHit = Team::IsBlue(TeamOf(target));
                const float x = xOf(firstHit);
                if (blueHit) dl->AddTriangleFilled(ImVec2(x - 3.f, p.y + 3.f), ImVec2(x + 3.f, p.y + 3.f), ImVec2(x, p.y + 8.f), IM_COL32(220, 224, 230, 200));
                else         dl->AddTriangleFilled(ImVec2(x - 3.f, p.y + H - 3.f), ImVec2(x + 3.f, p.y + H - 3.f), ImVec2(x, p.y + H - 8.f), IM_COL32(220, 224, 230, 200));
            }
        if (in.deaths) for (const auto& [aid, times] : *in.deaths)
        {
            if (!whoIx.count(aid)) continue;
            const bool blue = Team::IsBlue(TeamOf(aid));
            for (float t : times)
            {
                const float x = xOf(t), y = blue ? mid - 7.f : mid + 7.f;
                dl->AddLine(ImVec2(x - 3.f, y - 3.f), ImVec2(x + 3.f, y + 3.f), IM_COL32(255, 255, 255, 220), 1.5f);
                dl->AddLine(ImVec2(x + 3.f, y - 3.f), ImVec2(x - 3.f, y + 3.f), IM_COL32(255, 255, 255, 220), 1.5f);
            }
        }
        if (st.scope != kFull)
        {
            const float x0 = xOf(r0), x1 = xOf(r1);
            dl->AddRectFilled(ImVec2(p.x + padL, p.y), ImVec2(x0, p.y + H), IM_COL32(0, 0, 0, 140));
            dl->AddRectFilled(ImVec2(x1, p.y), ImVec2(p.x + W, p.y + H), IM_COL32(0, 0, 0, 140));
            dl->AddRect(ImVec2(x0, p.y), ImVec2(x1, p.y + H), IM_COL32(225, 190, 80, 0xB0));
        }
        dl->AddLine(ImVec2(xOf(tNow), p.y), ImVec2(xOf(tNow), p.y + H), IM_COL32(225, 190, 80, 0xD0), 1.5f);
        if (hov && !ImGui::IsItemActive())
        {
            const int i = std::clamp((int)(tOf(ImGui::GetIO().MousePos.x) / kBin), 0, n - 1);
            ImGui::BeginTooltip();
            ImGui::Text("%s-%s", FmtT(i * kBin).c_str(), FmtT((i + 1) * kBin).c_str());
            ImGui::TextColored(V4(kBlueText), "%s  %s", TagOf(Team::Blue).c_str(), FmtN((long long)top[i]).c_str());
            ImGui::TextColored(V4(kRedText), "%s  %s", TagOf(Team::Red).c_str(), FmtN((long long)bot[i]).c_str());
            ImGui::EndTooltip();
        }
        ImGui::Dummy(ImVec2(0.f, 4.f));
    }
}

void DrawBody(const Data& d, Host& host, ViewState& st, float r0, float r1, const std::string& stretch)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    const People who(d);
    const auto& players = who.players;
    auto whoIx = who.whoIx;
    auto TeamOf = [&](int a) { return who.TeamOf(a); };
    auto NameOf = [&](int a) { return who.NameOf(a); };
    auto ProfOf = [&](int a) { return who.ProfOf(a); };
    auto IsLord = [&](int a) { return who.IsLord(a); };
    auto GuildLabel = [&](int team) { return who.GuildLabel(team); };
    auto TagOf = [&](int team) { return who.TagOf(team); };
    auto In = [&](float t) { return t >= r0 && t <= r1; };
    auto tot = SumTotals(d, r0, r1);
    const bool intsTab = st.metric == kInts || st.metric == kIntsRecv;
    auto MetricCol = [&](int m) {
        return m == kDealt || m == kTaken ? kDamage : m == kHealed || m == kReceived ? kHealing : m == kPrevented ? kPrevent : kPurple;
    };
    // The client's digits for each tab: damage red, heal blue (prevented too), energy purple for interrupts.
    auto GlyphFont = [&](int m) { return host.Digits(m); };

    // Map effects (IsMapRow): their names and icons.
    auto MapName = [](int key) -> const char* {
        switch (key)
        {
        case kRowCatapult: return "Catapult";
        case kRowCoral:    return "Spiked Coral";
        case kRowAcid:     return "Acid Trap";
        case kRowFlux:     return "Minion Apocalypse";
        case kRowMiasma:   return "Miasma";
        case kRowWater:    return "Healing water";
        default:                 return "Map";
        }
    };
    auto MapIcon = [&](int key) -> ImTextureID {
        switch (key)
        {
        case kRowCatapult: return host.UiIcon(CatapultDangerZone::kHitIconFile);
        case kRowCoral:    return host.SkillIcon(DamageAttribution::kSpikedCoral);
        case kRowAcid:     return host.SkillIcon(DamageAttribution::kAcidTrap);
        case kRowFlux:     return host.SkillIcon(DamageAttribution::kMinionApocalypse);
        case kRowMiasma:   return host.SkillIcon(DamageAttribution::kMiasma);
        case kRowWater:    return host.SkillIcon(290);   // Mending, as the effect bar shows it
        default:                 return nullptr;
        }
    };

    if (!st.matrix)
    {
        // ===================== Board =====================
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(V4(kMuted), "Rows");
        ImGui::SameLine();
        if (const int c = Segmented("##rows", { "Contributors only", "All players" }, st.showAll ? 1 : 0); c >= 0) st.showAll = c == 1;
        ImGui::Dummy(ImVec2(0.f, 2.f));

        const float colGap = 18.f;
        const float colW = (ImGui::GetContentRegionAvail().x - colGap) * 0.5f - 20.f;   // inside the box's 10 px margins
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        float colBottom[2] = { origin.y, origin.y };
        long long boardMax = 1;
        for (const Who& w : players) boardMax = std::max(boardMax, tot[w.id].v[st.metric]);

        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        for (int side = 0; side < 2; ++side)
        {
            const uint8_t team = Team::FromIndex01(side);
            std::vector<const Who*> rows;
            for (const Who& w : players) if (w.team == team) rows.push_back(&w);
            std::stable_sort(rows.begin(), rows.end(), [&](const Who* a, const Who* b) { return tot[a->id].v[st.metric] > tot[b->id].v[st.metric]; });
            long long teamTotal = 0;
            for (const Who* w : rows) teamTotal += tot[w->id].v[st.metric];

            const float x = origin.x + side * (colW + 20.f + colGap) + 10.f;
            float y = origin.y + 10.f;
            // Team heading: the guild in team colour over a team rule, its total at the right.
            const float totH = 19.f;
            dl->AddText(ImVec2(x, y + (totH - lineH) * 0.5f), TeamText(team), GuildLabel(team).c_str());
            GlyphNumber(dl, GlyphFont(st.metric), teamTotal, x + colW - 6.f, y, totH, true);
            y += totH + 4.f;
            dl->AddLine(ImVec2(x, y), ImVec2(x + colW, y), WithAlpha(TeamText(team), 0x55));
            y += 4.f;

            int hidden = 0;
            std::vector<int> hiddenProfs;
            const float rowH = 40.f, valH = 15.f;
            for (const Who* w : rows)
            {
                const Totals& t = tot[w->id];
                const long long v = t.v[st.metric];
                const bool any = v > 0 || (st.metric == kInts && t.intAtt) || (st.metric == kIntsRecv && t.recvAtt);
                if (!any && !st.showAll) { ++hidden; hiddenProfs.push_back(w->prof); continue; }
                ImGui::SetCursorScreenPos(ImVec2(x, y));
                ImGui::PushID(w->id);
                ImGui::InvisibleButton("##row", ImVec2(colW, rowH));
                const bool hov = ImGui::IsItemHovered(), sel = st.selPlayer == w->id;
                if (ImGui::IsItemClicked()) { st.selPlayer = w->id; st.openRows.clear(); }
                ImGui::PopID();
                if (sel)
                {
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + colW, y + rowH), kSelFill, 3.f);
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 2.5f, y + rowH), kGold);
                }
                else if (hov) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + colW, y + rowH), kHover, 3.f);
                if (ImTextureID pt = w->prof ? host.ProfIcon(w->prof) : nullptr)
                    dl->AddImage(pt, ImVec2(x + 7.f, y + 5.f), ImVec2(x + 25.f, y + 23.f));
                dl->AddText(ImVec2(x + 31.f, y + 4.f), sel ? kGold : kNameText, w->name.c_str());

                // The value, and a note under it.
                std::string note;
                if (st.metric == kInts)          note = t.intAtt ? Pct(v, t.intAtt) + " of " + std::to_string(t.intAtt) : "no attempts";
                else if (st.metric == kIntsRecv) note = t.recvAtt ? std::to_string(t.recvFake) + " faked of " + std::to_string(t.recvAtt) : "no attempts";
                else                           note = teamTotal ? Pct(v, teamTotal) + " of team" : "";
                GlyphNumber(dl, GlyphFont(st.metric), v, x + colW - 6.f, y + 3.f, valH, true);
                RText(dl, x + colW - 6.f, y + 5.f + valH, kSoft, note);
                const float barR = x + colW - 6.f - std::max(ImGui::CalcTextSize(note.c_str()).x, 84.f) - 12.f;

                // Top three skills beside the name, then the bar under it.
                if (!intsTab)
                {
                    std::unordered_map<int, long long> bySkill;
                    const bool taker = !GivingMetric(st.metric);
                    for (const Line& ln : d.lines)
                    {
                        if (ln.t < r0) continue;
                        if (ln.t > r1) break;
                        const int m = taker ? LineMetricTaker(ln.kind) : LineMetricGiver(ln.kind);
                        if (m == st.metric && (taker ? ln.dst : ln.src) == w->id && ln.skillId > 0) bySkill[ln.skillId] += ln.amount;
                    }
                    std::vector<std::pair<long long, int>> topSk;
                    for (auto& [k, a] : bySkill) topSk.push_back({ a, k });
                    std::sort(topSk.rbegin(), topSk.rend());
                    const float ix = barR - 3 * 23.f;
                    for (size_t k = 0; k < topSk.size() && k < 3; ++k)
                        if (ImTextureID sk = host.SkillIcon(topSk[k].second))
                            dl->AddImageRounded(sk, ImVec2(ix + k * 23.f, y + 2.f), ImVec2(ix + k * 23.f + 21.f, y + 23.f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 2.f);
                }
                Bar(dl, ImVec2(x + 31.f, y + rowH - 9.f), ImVec2(barR, y + rowH - 5.f), (float)v / (float)boardMax,
                    sel || hov ? MetricCol(st.metric) : WithAlpha(MetricCol(st.metric), kBarSoft));
                dl->AddLine(ImVec2(x, y + rowH), ImVec2(x + colW, y + rowH), kHairline);
                y += rowH + 1.f;
            }
            if (hidden)
            {
                char b[96];
                snprintf(b, sizeof b, "%d player%s with no %s", hidden, hidden == 1 ? "" : "s", intsTab ? "attempts" : "share");
                float hx = x + 7.f;
                for (int pr : hiddenProfs)
                    if (ImTextureID pt = pr ? host.ProfIcon(pr) : nullptr)
                    { dl->AddImage(pt, ImVec2(hx, y + 5.f), ImVec2(hx + 14.f, y + 19.f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 150)); hx += 15.f; }
                dl->AddText(ImVec2(hx + 6.f, y + 4.f), kMuted, b);
                ImGui::SetCursorScreenPos(ImVec2(hx + 6.f + ImGui::CalcTextSize(b).x + 4.f, y + 1.f));
                ImGui::PushID(side);
                if (TextTab("Show", false)) st.showAll = true;
                ImGui::PopID();
                y += 26.f;
            }
            colBottom[side] = y + 6.f;
        }
        for (int side = 0; side < 2; ++side)
        {
            dl->ChannelsSetCurrent(0);
            const float bx = origin.x + side * (colW + 20.f + colGap);
            const ImVec2 a(bx, origin.y), b(bx + colW + 20.f, colBottom[side]);
            dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 6), 3.f);
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 15), 3.f);
            dl->AddRectFilled(a, ImVec2(b.x, a.y + 2.f), WithAlpha(TeamText(Team::FromIndex01(side)), 0x8C), 2.f);
        }
        dl->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(origin.x, std::max(colBottom[0], colBottom[1]) + 12.f));
        ImGui::Dummy(ImVec2(0.f, 0.f));

        // ===================== Map effects =====================
        // A strip under both teams: what the map itself did to each team in this tab (damage taken,
        // healing received) - never any player's. Other tabs show none.
        if (st.metric == kTaken || st.metric == kReceived)
        {
            std::map<int, std::array<long long, 2>> fx;      // map row -> per team (blue, red)
            for (const Line& ln : d.lines)
            {
                if (ln.t < r0) continue;
                if (ln.t > r1) break;
                if (!IsMapRow(ln.rowKey) || LineMetricTaker(ln.kind) != st.metric || !whoIx.count(ln.dst)) continue;
                const int team = TeamOf(ln.dst);
                if (Team::IsBlue(team)) fx[ln.rowKey][0] += ln.amount;
                else if (Team::IsRed(team)) fx[ln.rowKey][1] += ln.amount;
            }
            if (!fx.empty())
            {
                const float fullW = (colW + 20.f) * 2.f + colGap, rowH = 24.f;
                const ImVec2 a0 = ImGui::GetCursorScreenPos();
                const float h = 10.f + lineH + 6.f + rowH * (float)fx.size() + 6.f;
                dl->AddRectFilled(a0, ImVec2(a0.x + fullW, a0.y + h), IM_COL32(255, 255, 255, 6), 3.f);
                dl->AddRect(a0, ImVec2(a0.x + fullW, a0.y + h), IM_COL32(255, 255, 255, 15), 3.f);
                const float smallFs = ImGui::GetFontSize() * 0.85f;
                dl->AddText(ImGui::GetFont(), smallFs, ImVec2(a0.x + 10.f, a0.y + 8.f), kGold, "MAP EFFECTS");
                float y = a0.y + 10.f + lineH + 6.f;
                for (const auto& [key, v] : fx)
                {
                    float x = a0.x + 10.f;
                    if (ImTextureID ic = MapIcon(key))
                        dl->AddImageRounded(ic, ImVec2(x, y + 1.f), ImVec2(x + 20.f, y + 21.f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 2.f);
                    x += 28.f;
                    dl->AddText(ImVec2(x, y + 3.f), kNameText, MapName(key));
                    x = a0.x + std::max(190.f, ImGui::CalcTextSize(MapName(key)).x + 50.f);
                    for (int side = 0; side < 2; ++side)
                    {
                        const uint8_t team = Team::FromIndex01(side);
                        const std::string tag = TagOf(team) + " ";
                        dl->AddText(ImVec2(x, y + 3.f), TeamText(team), tag.c_str());
                        x += ImGui::CalcTextSize(tag.c_str()).x;
                        const std::string val = FmtN(v[side]);
                        dl->AddText(ImVec2(x, y + 3.f), v[side] ? MetricCol(st.metric) : kMuted, val.c_str());
                        x += ImGui::CalcTextSize(val.c_str()).x;
                        if (side == 0) { dl->AddText(ImVec2(x + 6.f, y + 3.f), kMuted, "\xC2\xB7"); x += 20.f; }
                    }
                    y += rowH;
                }
                ImGui::Dummy(ImVec2(fullW, h + 12.f));
            }
        }

        // ===================== Detail =====================
        if (st.selPlayer < 0 || !whoIx.count(st.selPlayer))
            for (const Who& w : players) if (tot[w.id].v[st.metric] > 0) { st.selPlayer = w.id; break; }
        if (st.selPlayer >= 0 && whoIx.count(st.selPlayer))
        {
            const Who& me = players[whoIx[st.selPlayer]];
            const Totals& mt = tot[me.id];
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), kRule);
                ImGui::Dummy(ImVec2(0.f, 8.f));
            }
            // Header: the professions, the name in team colour, the tab, the stretch.
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImTextureID pt = me.prof ? host.ProfIcon(me.prof) : nullptr) dl->AddImage(pt, p, ImVec2(p.x + 26.f, p.y + 26.f));
                if (ImTextureID sec = me.sec ? host.ProfIcon(me.sec) : nullptr)
                    dl->AddImage(sec, ImVec2(p.x + 18.f, p.y + 12.f), ImVec2(p.x + 34.f, p.y + 28.f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 210));
                // A baked bold rung (crisp, not a scaled font), one step above the body text.
                // The window's own font: a font from the main window's atlas does not draw here.
                ImFont* nf = ImGui::GetFont();
                const float big = ImGui::GetFontSize();
                dl->AddText(nf, big, ImVec2(p.x + 42.f, p.y), TeamText(me.team), me.name.c_str());
                const float nw = nf->CalcTextSizeA(big, FLT_MAX, 0.f, me.name.c_str()).x;
                dl->AddText(ImVec2(p.x + 42.f + nw + 12.f, p.y + (big - lineH)), kGold, MetricName(st.metric));
                const std::string sub = stretch + "   " + GuildLabel(me.team);
                dl->AddText(ImVec2(p.x + 42.f, p.y + big + 3.f), kSoft, sub.c_str());
                ImGui::Dummy(ImVec2(0.f, big + lineH + 12.f));
            }

            // Headline: the tab's number large, what it means beside it, on one line.
            std::vector<int> rxs, afters;
            for (const Interrupt& li : d.ints)
            {
                if (!In(li.t) || li.outcome != Interrupt::Landed) continue;
                if (li.src == me.id && li.reflexMs >= 0) rxs.push_back(li.reflexMs);
                if (li.dst == me.id && li.afterMs >= 0) afters.push_back(li.afterMs);
            }
            // Headline figures (KPIs) in the client's digits, each with what it is under it.
            std::string what = MetricName(st.metric);
            what[0] = (char)std::tolower((unsigned char)what[0]);
            auto PctOf = [](long long a, long long b) { return b ? (long long)std::lround(100.0 * a / b) : 0LL; };
            const BitmapFont* kf = GlyphFont(st.metric);
            const ImVec2 kpiAt = ImGui::GetCursorScreenPos();
            const float kpiW = ImGui::GetContentRegionAvail().x;
            if (st.metric == kInts)
            {
                const int rx = Median(rxs);
                KpiRow(kf, { { PctOf(mt.v[kInts], mt.intAtt), "%", "accuracy", mt.intAtt == 0 },
                             { mt.v[kInts], "", "landed" }, { mt.intAtt, "", "attempts" }, { mt.intFake, "", "faked by the target" },
                             { rx, " ms", "median reaction", rx < 0 } });
            }
            else if (st.metric == kIntsRecv)
            {
                const int af = Median(afters);
                KpiRow(kf, { { mt.v[kIntsRecv], "", "interrupted" }, { mt.recvAtt, "", "attempts on him" },
                             { mt.recvFake, "", "faked away" }, { af, " ms", "into the cast, median", af < 0 } });
            }
            else
            {
                long long teamTotal = 0;
                int rank = 1, count = 0;
                for (const Who& w : players)
                    if (w.team == me.team) { teamTotal += tot[w.id].v[st.metric]; ++count; if (tot[w.id].v[st.metric] > mt.v[st.metric]) ++rank; }
                KpiRow(kf, { { mt.v[st.metric], "", what }, { PctOf(mt.v[st.metric], teamTotal), "%", "of the team" },
                             { rank, " of " + std::to_string(count), "rank in the team" } });
            }
            if (intsTab)
            {
                const ImVec2 after = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(kpiAt.x + kpiW - 26.f, kpiAt.y + 6.f));
                Help("Accuracy: the attempts that stopped a cast. An attempt is a completed cast of an interrupting "
                     "skill on a foe; faked means the foe stopped his own cast in the quarter second before it would "
                     "have landed. Reaction time: from the start of the cast that was stopped to the start of the interrupt.");
                ImGui::SetCursorScreenPos(after);
                ImGui::Dummy(ImVec2(0.f, 0.f));
            }

            // Every tab for this player in one strip; a click switches the tab.
            ImGui::Dummy(ImVec2(0.f, 6.f));
            {
                std::vector<long long> values;
                std::vector<const BitmapFont*> fonts;
                for (int m = 0; m < kMetricCount; ++m) { values.push_back(mt.v[m]); fonts.push_back(GlyphFont(m)); }
                if (const int c = StatStrip(kMetricName, values, fonts, st.metric); c >= 0) { st.metric = c; st.openRows.clear(); }
            }
            ImGui::Dummy(ImVec2(0.f, 10.f));

            // --- The breakdown table ---
            const float W = ImGui::GetContentRegionAvail().x;
            auto RowFrame = [&](int key, float rh, bool& open) -> ImVec2 {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushID(key);
                ImGui::InvisibleButton("##r", ImVec2(W, rh));
                open = st.openRows.count(key) != 0;
                if (ImGui::IsItemClicked()) { if (open) st.openRows.erase(key); else st.openRows.insert(key); open = !open; }
                if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + W, p.y + rh), kHover, 3.f);
                ImGui::PopID();
                // The game-style caret: a small triangle, like the Morale and Piano Roll headers.
                const float cx = p.x + 6.f, cy = p.y + rh * 0.5f;
                if (open) dl->AddTriangleFilled(ImVec2(cx - 4.f, cy - 2.f), ImVec2(cx + 4.f, cy - 2.f), ImVec2(cx, cy + 3.f), kMuted);
                else      dl->AddTriangleFilled(ImVec2(cx - 2.f, cy - 4.f), ImVec2(cx - 2.f, cy + 4.f), ImVec2(cx + 3.f, cy), kMuted);
                dl->AddLine(ImVec2(p.x, p.y + rh), ImVec2(p.x + W, p.y + rh), kHairline);
                return p;
            };
            auto SubLineHead = [&](int id) -> ImVec2 {
                const ImVec2 q = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(W, 20.f));
                // A Guild Lord has no profession: his own lord art, in his team's colour.
                const bool lord = id >= 0 && IsLord(id);
                ImTextureID pt = lord ? host.UiIcon(Team::IsRed(TeamOf(id)) ? "redguildlord.png" : "blueguildlord.png")
                               : ProfOf(id) ? host.ProfIcon(ProfOf(id)) : nullptr;
                if (pt) dl->AddImage(pt, ImVec2(q.x + 44.f, q.y + 2.f), ImVec2(q.x + 60.f, q.y + 18.f));
                dl->AddText(ImVec2(q.x + 66.f, q.y + 2.f), TeamText(TeamOf(id)), NameOf(id).c_str());
                return q;
            };
            // iconSkill: the skill whose icon a special row shows (the Animate skill, the cause of a
            // condition), 0 for its own.
            auto SkillCell = [&](ImVec2 p, int key, const std::string& name, int iconSkill = 0) {
                ImTextureID ic = IsMapRow(key) ? MapIcon(key)
                               : iconSkill > 0 ? host.SkillIcon(iconSkill)
                               : key > 0 ? host.SkillIcon(key)
                               : key == -1 || key == kRowMinions ? host.UiIcon("kill.png")
                               : key == -2 ? host.ProfIcon(3) : nullptr;   // Divine Favor bonus: the Monk icon
                if (ic) dl->AddImageRounded(ic, ImVec2(p.x + 16.f, p.y + 2.f), ImVec2(p.x + 38.f, p.y + 24.f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 2.f);
                else    dl->AddRectFilled(ImVec2(p.x + 16.f, p.y + 2.f), ImVec2(p.x + 38.f, p.y + 24.f), IM_COL32(40, 40, 40, 255), 2.f);
                dl->AddText(ImVec2(p.x + 46.f, p.y + 5.f), kNameText, name.c_str());
            };

            if (!intsTab)
            {
                const bool taker = !GivingMetric(st.metric);
                // Row key: the skill, or -1 weapon hits, -2 the Divine Favor bonus, -3 nothing named,
                // or the line's own row (minions, degeneration: Line::rowKey).
                struct Row
                {
                    int key = 0, icon = 0; long long amount = 0; int casts = 0, intr = 0, canc = 0;
                    bool est = false; int capped = 0, blocked = 0;   // prevention estimates
                    std::map<int, long long> per; std::map<int, int> castsOn, intrOn, cancOn;
                };
                std::map<int, Row> rows;
                long long sum = 0;
                for (const Line& ln : d.lines)
                {
                    if (ln.t < r0) continue;
                    if (ln.t > r1) break;
                    const int m = taker ? LineMetricTaker(ln.kind) : LineMetricGiver(ln.kind);
                    if (m != st.metric || (taker ? ln.dst : ln.src) != me.id) continue;
                    const int key = ln.rowKey ? ln.rowKey : ln.kind == Kind::DivineFavor ? -2 : ln.skillId > 0 ? ln.skillId : ln.weaponHit ? -1 : -3;
                    Row& r = rows[key];
                    if (ln.rowKey && ln.skillId > 0) r.icon = ln.skillId;
                    if (ln.estimated) r.est = true;
                    if (ln.preventGroup == 1 + (uint8_t)DamageAttribution::PreventGroup::Cap)   r.capped += ln.count;
                    if (ln.preventGroup == 1 + (uint8_t)DamageAttribution::PreventGroup::Block) r.blocked += ln.count;
                    r.key = key; r.amount += ln.amount; r.per[taker ? ln.src : ln.dst] += ln.amount; sum += ln.amount;
                }
                if (!taker)
                    if (auto uit = d.uses.find(me.id); uit != d.uses.end())
                        for (const Use& su : uit->second)
                        {
                            if (!In(su.start)) continue;
                            auto rit = rows.find(su.skillId);
                            if (rit == rows.end()) continue;
                            Row& r = rit->second;
                            const int tg = su.target >= 0 ? su.target : me.id;
                            r.casts++; r.castsOn[tg]++;
                            if (su.interrupted)    { r.intr++; r.intrOn[tg]++; }
                            else if (su.cancelled) { r.canc++; r.cancOn[tg]++; }
                        }
                std::vector<Row*> order;
                for (auto& [k, r] : rows) order.push_back(&r);
                std::sort(order.begin(), order.end(), [](const Row* a, const Row* b) { return a->amount > b->amount; });

                const float xCasts = W * 0.46f, xInt = W * 0.555f, xCanc = W * 0.645f, xAmt = W * 0.75f, xBar0 = W * 0.78f, xPct = W - 4.f;
                Section(taker ? "BY SKILL" : "SKILLS", taker ? "click a row for who it came from" : "click a row for who it was cast on");
                const ImVec2 h = ImGui::GetCursorScreenPos();
                dl->AddText(ImVec2(h.x + 46.f, h.y), kMuted, "Skill");
                if (!taker)
                {
                    RText(dl, h.x + xCasts, h.y, kMuted, "Casts");
                    RText(dl, h.x + xInt, h.y, kMuted, "Interrupted");
                    RText(dl, h.x + xCanc, h.y, kMuted, "Cancelled");
                }
                RText(dl, h.x + xAmt, h.y, kMuted, "Amount");
                dl->AddText(ImVec2(h.x + xBar0, h.y), kMuted, "Share");
                ImGui::Dummy(ImVec2(W, lineH + 4.f));
                if (order.empty()) ImGui::TextColored(V4(kMuted), "Nothing in this stretch.");
                for (Row* r : order)
                {
                    bool open = false;
                    const ImVec2 p = RowFrame(r->key, 26.f, open);
                    const bool lit = open || ImGui::IsItemHovered();
                    std::string nm;
                    if (r->key == 657) nm = "Life Draining";      // the vampiric weapon's hidden skill
                    else if (r->key > 0) nm = host.SkillName(r->key);
                    else if (IsMapRow(r->key)) nm = MapName(r->key);
                    else if (IsDegenRow(r->key))
                    {
                        // "Poison from Apply Poison", or the condition alone when its cause is unknown.
                        const int cond = DegenCondition(r->key), cause = DegenCause(r->key);
                        nm = ConditionName(cond);
                        if (cause > 0) nm += " from " + host.SkillName(cause);
                    }
                    else nm = r->key == -1 ? "Weapon hits" : r->key == -2 ? "Divine Favor bonus" : r->key == kRowMinions ? "Minions"
                            : r->key == kRowDegenUnknown ? "Degeneration (source unknown)" : "No skill named";
                    int icon = r->icon;
                    if (IsDegenRow(r->key) && host.SkillIcon(DegenCondition(r->key)) != nullptr) icon = DegenCondition(r->key);
                    SkillCell(p, r->key, nm, icon);
                    if (r->capped || r->blocked)
                    {
                        // An estimate: what the capped hits and stopped attacks would have dealt.
                        std::string note = r->capped ? FmtN(r->capped) + " capped" : std::string();
                        if (r->blocked) note += (note.empty() ? "" : ", ") + FmtN(r->blocked) + " blocked";
                        dl->AddText(ImVec2(p.x + 46.f + ImGui::CalcTextSize(nm.c_str()).x + 8.f, p.y + 5.f), kMuted, note.c_str());
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Estimated: each capped hit or stopped attack counts the attacker's median\nhit with the same skill or weapon in this match.");
                    }
                    if (!taker && r->key > 0)
                    {
                        RText(dl, p.x + xCasts, p.y + 5.f, kNameText, std::to_string(r->casts));
                        RText(dl, p.x + xInt, p.y + 5.f, r->intr ? kPurple : kMuted, r->intr ? std::to_string(r->intr) : "-");
                        RText(dl, p.x + xCanc, p.y + 5.f, r->canc ? kOrange : kMuted, r->canc ? std::to_string(r->canc) : "-");
                    }
                    RText(dl, p.x + xAmt, p.y + 5.f, MetricCol(st.metric), (r->est ? std::string("~") : std::string()) + FmtN(r->amount));
                    Bar(dl, ImVec2(p.x + xBar0, p.y + 11.f), ImVec2(p.x + xPct - 44.f, p.y + 15.f), sum ? (float)r->amount / (float)sum : 0.f,
                        lit ? MetricCol(st.metric) : WithAlpha(MetricCol(st.metric), kBarSoft));
                    RText(dl, p.x + xPct, p.y + 5.f, kMuted, Pct(r->amount, sum));
                    if (!open) continue;
                    // One line per target (or source), with the casts aimed at it.
                    std::vector<int> list;
                    for (auto& [id, _] : r->per) list.push_back(id);
                    for (auto& [id, _] : r->castsOn) if (!r->per.count(id)) list.push_back(id);
                    std::sort(list.begin(), list.end(), [&](int a, int b) { return r->per[a] != r->per[b] ? r->per[a] > r->per[b] : r->castsOn[a] > r->castsOn[b]; });
                    long long pmax = 1;
                    for (int id : list) pmax = std::max(pmax, r->per[id]);
                    for (int id : list)
                    {
                        const ImVec2 q = SubLineHead(id);
                        if (!taker && r->key > 0)
                        {
                            RText(dl, q.x + xCasts, q.y + 2.f, r->castsOn[id] ? kNameText : kMuted, r->castsOn[id] ? std::to_string(r->castsOn[id]) : "-");
                            RText(dl, q.x + xInt, q.y + 2.f, r->intrOn[id] ? kPurple : kMuted, r->intrOn[id] ? std::to_string(r->intrOn[id]) : "-");
                            RText(dl, q.x + xCanc, q.y + 2.f, r->cancOn[id] ? kOrange : kMuted, r->cancOn[id] ? std::to_string(r->cancOn[id]) : "-");
                        }
                        RText(dl, q.x + xAmt, q.y + 2.f, r->per[id] ? kNameText : kMuted, r->per[id] ? FmtN(r->per[id]) : "-");
                        Bar(dl, ImVec2(q.x + xBar0, q.y + 8.f), ImVec2(q.x + xPct - 44.f, q.y + 12.f), (float)r->per[id] / (float)pmax, WithAlpha(MetricCol(st.metric), 90));
                    }
                    ImGui::Dummy(ImVec2(W, 4.f));
                }

                // Everyone this player's figures went to (or came from), all skills together: the
                // mockup's target grid, four to a row.
                std::map<int, long long> byOther;
                for (Row* r : order) for (auto& [id, a] : r->per)                 // -5: degeneration, source unknown; map effects by their own row
                    byOther[r->key == -5 || IsMapRow(r->key) ? r->key : id] += a;
                std::vector<std::pair<long long, int>> others;
                for (auto& [id, a] : byOther) if (a > 0) others.push_back({ a, id });
                std::sort(others.rbegin(), others.rend());
                if (!others.empty())
                {
                    ImGui::Dummy(ImVec2(0.f, 10.f));
                    const char* head = st.metric == kDealt ? "TARGETS" : st.metric == kTaken ? "HIT BY" : st.metric == kHealed ? "HEALED"
                                     : st.metric == kReceived ? "HEALED BY" : "PROTECTED";
                    Section(head, nullptr);
                    const float gap = 22.f, cw4 = (W - gap * 3.f) / 4.f, rh = 34.f;
                    const ImVec2 g0 = ImGui::GetCursorScreenPos();
                    const long long omax = others.front().first;
                    for (size_t i = 0; i < others.size(); ++i)
                    {
                        const int id = others[i].second;
                        const float x = g0.x + (i % 4) * (cw4 + gap), y = g0.y + (i / 4) * rh;
                        const bool lord = IsLord(id);
                        ImTextureID ic = lord ? host.UiIcon(Team::IsRed(TeamOf(id)) ? "redguildlord.png" : "blueguildlord.png")
                                       : IsMapRow(id) ? MapIcon(id)
                                       : id == -5 ? nullptr : ProfOf(id) ? host.ProfIcon(ProfOf(id)) : nullptr;
                        if (ic) dl->AddImage(ic, ImVec2(x, y + 2.f), ImVec2(x + 20.f, y + 22.f));
                        const std::string val = FmtN(others[i].first);
                        const float vw = ImGui::CalcTextSize(val.c_str()).x;
                        dl->PushClipRect(ImVec2(x + 26.f, y), ImVec2(x + cw4 - vw - 8.f, y + rh), true);
                        dl->AddText(ImVec2(x + 26.f, y + 2.f), id == -5 || IsMapRow(id) ? kSoft : TeamText(TeamOf(id)),
                                    IsMapRow(id) ? MapName(id) : id == -5 ? "Degeneration, source unknown" : NameOf(id).c_str());
                        dl->PopClipRect();
                        RText(dl, x + cw4, y + 2.f, kNameText, val);
                        Bar(dl, ImVec2(x + 26.f, y + lineH + 7.f), ImVec2(x + cw4, y + lineH + 11.f), (float)others[i].first / (float)omax,
                            WithAlpha(MetricCol(st.metric), kBarSoft));
                    }
                    ImGui::Dummy(ImVec2(W, ((others.size() + 3) / 4) * rh + 4.f));
                }
            }
            else
            {
                // Interrupts: one row per interrupting skill (or per skill lost), one line per foe inside.
                const bool recv = st.metric == kIntsRecv;
                struct IRow
                {
                    int key = 0, att = 0, landed = 0, faked = 0, missed = 0; std::vector<int> ms;
                    std::map<int, std::array<int, 4>> per; std::map<int, std::vector<int>> perMs;
                    std::map<int, std::map<int, int>> lost;      // foe -> skill he lost -> times (giving side)
                };
                std::map<int, IRow> rows;
                std::map<int, std::map<int, int>> stopped;       // foe -> skill he lost -> times, every skill together
                for (const Interrupt& li : d.ints)
                {
                    if (!In(li.t) || (recv ? li.dst : li.src) != me.id) continue;
                    if (recv && li.outcome == Interrupt::Missed) continue;
                    const int key = recv ? li.lostSkill : li.skillId;
                    IRow& r = rows[key];
                    r.key = key; r.att++;
                    auto& pc = r.per[recv ? li.src : li.dst];
                    pc[0]++;
                    if (li.outcome == Interrupt::Landed)     { r.landed++; pc[1]++; }
                    else if (li.outcome == Interrupt::Faked) { r.faked++;  pc[2]++; }
                    else                                           { r.missed++; pc[3]++; }
                    if (!recv && li.outcome == Interrupt::Landed && li.lostSkill > 0)
                    { r.lost[li.dst][li.lostSkill]++; stopped[li.dst][li.lostSkill]++; }
                    const int ms = recv ? li.afterMs : li.reflexMs;
                    if (li.outcome == Interrupt::Landed && ms >= 0) { r.ms.push_back(ms); r.perMs[recv ? li.src : li.dst].push_back(ms); }
                }
                std::vector<IRow*> order;
                for (auto& [k, r] : rows) order.push_back(&r);
                std::sort(order.begin(), order.end(), [](const IRow* a, const IRow* b) { return a->landed != b->landed ? a->landed > b->landed : a->att > b->att; });
                const float xTry = W * 0.42f, xLand = W * 0.50f, xFake = W * 0.58f, xMiss = W * 0.66f, xAcc = W * 0.76f, xMs = W * 0.90f;
                Section(recv ? "SKILLS LOST" : "INTERRUPTING SKILLS", recv ? "click a row for who interrupted" : "click a row for the foes it was used on");
                const ImVec2 h = ImGui::GetCursorScreenPos();
                dl->AddText(ImVec2(h.x + 46.f, h.y), kMuted, "Skill");
                if (recv)
                {
                    RText(dl, h.x + xLand, h.y, kMuted, "Interrupted");
                    RText(dl, h.x + xFake, h.y, kMuted, "Faked");
                    RText(dl, h.x + xMs, h.y, kMuted, "After (median)");
                }
                else
                {
                    RText(dl, h.x + xTry, h.y, kMuted, "Tries");
                    RText(dl, h.x + xLand, h.y, kMuted, "Landed");
                    RText(dl, h.x + xFake, h.y, kMuted, "Faked");
                    RText(dl, h.x + xMiss, h.y, kMuted, "Missed");
                    RText(dl, h.x + xAcc, h.y, kMuted, "Accuracy");
                    RText(dl, h.x + xMs, h.y, kMuted, "Reaction (median)");
                }
                ImGui::Dummy(ImVec2(W, lineH + 4.f));
                if (order.empty())
                    ImGui::TextColored(V4(kMuted), "%s", recv ? "No interrupt attempts on this player in this stretch." : "No interrupt attempts in this stretch.");
                // A skill icon with a small count badge in its corner (dark, rounded, white digits).
                auto IconBadge = [&](float x, float y, float sz, int skill, int count) {
                    if (ImTextureID ic = host.SkillIcon(skill))
                        dl->AddImageRounded(ic, ImVec2(x, y), ImVec2(x + sz, y + sz), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 2.f);
                    else
                        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + sz, y + sz), IM_COL32(40, 40, 40, 255), 2.f);
                    if (ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + sz, y + sz)))
                        ImGui::SetTooltip("%s x%d", host.SkillName(skill).c_str(), count);
                    const std::string n = std::to_string(count);
                    const float fs = ImGui::GetFontSize() * 0.72f;
                    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, n.c_str());
                    const ImVec2 b1(x + sz + 2.f, y + sz + 2.f), b0(b1.x - ts.x - 6.f, b1.y - ts.y - 1.f);
                    dl->AddRectFilled(b0, b1, IM_COL32(12, 12, 14, 230), 3.f);
                    dl->AddText(ImGui::GetFont(), fs, ImVec2(b0.x + 3.f, b0.y), IM_COL32_WHITE, n.c_str());
                };
                auto SortedLost = [](const std::map<int, int>& m) {
                    std::vector<std::pair<int, int>> v;
                    for (auto& [sk, n] : m) v.push_back({ n, sk });
                    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
                    return v;
                };
                auto Figures = [&](ImVec2 p, float y, int att, int landed, int faked, int missed, int ms) {
                    if (!recv)
                    {
                        RText(dl, p.x + xTry, y, kNameText, std::to_string(att));
                        RText(dl, p.x + xMiss, y, kMuted, missed ? std::to_string(missed) : "-");
                        RText(dl, p.x + xAcc, y, kNameText, Pct(landed, att));
                    }
                    RText(dl, p.x + xLand, y, landed ? kPurple : kMuted, std::to_string(landed));
                    RText(dl, p.x + xFake, y, faked ? kOrange : kMuted, faked ? std::to_string(faked) : "-");
                    RText(dl, p.x + xMs, y, kNameText, FmtMs(ms));
                };
                for (IRow* r : order)
                {
                    bool open = false;
                    const ImVec2 p = RowFrame(r->key, 26.f, open);
                    SkillCell(p, r->key, r->key > 0 ? host.SkillName(r->key) : std::string("Unknown skill"));
                    Figures(p, p.y + 5.f, r->att, r->landed, r->faked, r->missed, Median(r->ms));
                    if (!open) continue;
                    for (auto& [id, c] : r->per)
                    {
                        const ImVec2 q = SubLineHead(id);
                        Figures(q, q.y + 2.f, c[0], c[1], c[2], c[3], Median(r->perMs[id]));
                        // What he lost to it: small icons with counts after his name.
                        if (auto lit = r->lost.find(id); lit != r->lost.end())
                        {
                            float ix = q.x + 66.f + ImGui::CalcTextSize(NameOf(id).c_str()).x + 10.f;
                            for (auto& [n, sk] : SortedLost(lit->second))
                            {
                                if (ix + 16.f > q.x + xTry - 40.f) break;
                                IconBadge(ix, q.y + 2.f, 16.f, sk, n);
                                ix += 24.f;
                            }
                        }
                    }
                    ImGui::Dummy(ImVec2(W, 4.f));
                }

                // What was interrupted: one row per foe, the skills he lost (with counts), his total.
                if (!recv && !stopped.empty())
                {
                    ImGui::Dummy(ImVec2(0.f, 10.f));
                    Section("SKILLS STOPPED", "what each foe lost to this player's interrupts");
                    std::vector<std::pair<int, int>> foes;            // total, foe
                    for (auto& [id, m] : stopped)
                    {
                        int n = 0;
                        for (auto& [sk, c] : m) n += c;
                        foes.push_back({ n, id });
                    }
                    std::sort(foes.begin(), foes.end(), [](auto& a, auto& b) { return a.first > b.first; });
                    const float gap = 22.f, cw = (W - gap) * 0.5f, rh = 36.f, icon = 26.f;
                    const ImVec2 g0 = ImGui::GetCursorScreenPos();
                    for (size_t i = 0; i < foes.size(); ++i)
                    {
                        const int id = foes[i].second;
                        const float x = g0.x + (i % 2) * (cw + gap), y = g0.y + (i / 2) * rh;
                        const bool lord = IsLord(id);
                        ImTextureID pt = lord ? host.UiIcon(Team::IsRed(TeamOf(id)) ? "redguildlord.png" : "blueguildlord.png")
                                       : ProfOf(id) ? host.ProfIcon(ProfOf(id)) : nullptr;
                        if (pt) dl->AddImage(pt, ImVec2(x, y + 5.f), ImVec2(x + 18.f, y + 23.f));
                        const float nameW = std::min(cw * 0.32f, ImGui::CalcTextSize(NameOf(id).c_str()).x);
                        dl->PushClipRect(ImVec2(x + 24.f, y), ImVec2(x + 24.f + cw * 0.32f, y + rh), true);
                        dl->AddText(ImVec2(x + 24.f, y + 6.f), TeamText(TeamOf(id)), NameOf(id).c_str());
                        dl->PopClipRect();
                        const std::string tot = std::to_string(foes[i].first);
                        RText(dl, x + cw, y + 6.f, kPurple, tot);
                        float ix = x + 24.f + nameW + 12.f;
                        const float stop = x + cw - ImGui::CalcTextSize(tot.c_str()).x - 12.f;
                        for (auto& [n, sk] : SortedLost(stopped[id]))
                        {
                            if (ix + icon > stop) break;
                            IconBadge(ix, y + 2.f, icon, sk, n);
                            ix += icon + 8.f;
                        }
                        dl->AddLine(ImVec2(x, y + rh - 2.f), ImVec2(x + cw, y + rh - 2.f), kHairline);
                    }
                    ImGui::Dummy(ImVec2(W, ((foes.size() + 1) / 2) * rh + 4.f));
                }
            }
        }
    }
    else
    {
        // ===================== Team matrix =====================
        const int kind = st.metric == kDealt || st.metric == kTaken ? 0 : st.metric == kHealed || st.metric == kReceived ? 1 : st.metric == kPrevented ? 2 : 3;
        const bool within = kind == 1 || kind == 2;
        const uint8_t giveTeam = Team::FromIndex01(st.mxSide);
        const uint8_t takeTeam = within ? giveTeam : Team::Other(giveTeam);
        const char* kAxes[4][2] = { { "attackers", "targets" }, { "healers", "recipients" }, { "protectors", "protected" }, { "interrupters", "interrupted" } };
        const char* kMost[4] = { "most hit", "most healed", "most protected", "most stopped" };
        const char* kVerb[4] = { "damage", "healing", "damage prevented", "interrupts landed" };

        {
            std::vector<std::string> labels;
            for (int s = 0; s < 2; ++s)
            {
                const uint8_t g = Team::FromIndex01(s);
                labels.push_back(within ? GuildLabel(g) : GuildLabel(g) + "   to   " + GuildLabel(Team::Other(g)));
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(V4(kGold), "%s", within ? "TEAM" : "WHO HITS WHOM");
            ImGui::SameLine(0.f, 12.f);
            // Pills like the other choices, but each guild name in its team's colour.
            for (int s = 0; s < 2; ++s)
            {
                const uint8_t g = Team::FromIndex01(s), o = Team::Other(g);
                std::vector<std::pair<std::string, ImU32>> parts = { { GuildLabel(g), TeamText(g) } };
                if (!within) { parts.push_back({ "  to  ", kSoft }); parts.push_back({ GuildLabel(o), TeamText(o) }); }
                float tw = 0.f;
                for (auto& [s2, c] : parts) tw += ImGui::CalcTextSize(s2.c_str()).x;
                const float h = lineH + 12.f, w = tw + 26.f;
                const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + w, p.y + h);
                ImGui::PushID(320 + s);
                if (ImGui::InvisibleButton("##side", ImVec2(w, h))) st.mxSide = s;
                const bool hov = ImGui::IsItemHovered(), on = st.mxSide == s;
                ImGui::PopID();
                dl->AddRectFilled(p, q, on ? IM_COL32(36, 28, 10, 255) : IM_COL32(255, 255, 255, hov ? 31 : 13), h * 0.5f);
                dl->AddRect(p, q, on ? IM_COL32(255, 214, 100, 204) : IM_COL32(255, 255, 255, 20), h * 0.5f, 0, on ? 1.5f : 1.f);
                float x = p.x + 13.f;
                for (auto& [s2, c] : parts)
                {
                    dl->AddText(ImVec2(x, p.y + 6.f), on || hov ? c : WithAlpha(c, 170), s2.c_str());
                    x += ImGui::CalcTextSize(s2.c_str()).x;
                }
                if (s == 0) ImGui::SameLine(0.f, 8.f);
            }
            ImGui::Dummy(ImVec2(0.f, 4.f));
        }
        {
            const std::string what = within ? GuildLabel(giveTeam) + ", " + kVerb[kind]
                                            : GuildLabel(giveTeam) + " to " + GuildLabel(takeTeam) + ", " + kVerb[kind];
            ImGui::TextColored(V4(kMuted), "%s.", what.c_str());
            ImGui::SameLine();
            ImGui::TextColored(V4(kNameText), "Rows: %s   Columns: %s", kAxes[kind][0], kAxes[kind][1]);
            ImGui::SameLine();
            Help("Each cell is what the row's player gave the column's player in the counted stretch. "
                 "Hover a cell to trace its row and column, or a column head for the full name. A dash means none. "
                 "The tagged column is the player who received the most.");
        }
        ImGui::Dummy(ImVec2(0.f, 4.f));
        std::vector<const Who*> rowsP, colsP;
        for (const Who& w : players) { if (w.team == giveTeam) rowsP.push_back(&w); if (w.team == takeTeam) colsP.push_back(&w); }
        std::map<std::pair<int, int>, long long> cell;
        long long mx = 1;
        if (kind == 3)
        {
            for (const Interrupt& li : d.ints)
                if (In(li.t) && li.outcome == Interrupt::Landed && TeamOf(li.src) == giveTeam)
                    mx = std::max(mx, ++cell[{ li.src, li.dst }]);
        }
        else
            for (const Line& ln : d.lines)
            {
                if (ln.t < r0) continue;
                if (ln.t > r1) break;
                const bool match = kind == 0 ? ln.kind == Kind::Damage
                                 : kind == 1 ? (ln.kind == Kind::Heal || ln.kind == Kind::DivineFavor)
                                             : ln.kind == Kind::Prevented;
                if (!match || TeamOf(ln.src) != giveTeam || TeamOf(ln.dst) != takeTeam) continue;
                mx = std::max(mx, cell[{ ln.src, ln.dst }] += ln.amount);
            }
        std::vector<long long> colTot(colsP.size());
        for (size_t c = 0; c < colsP.size(); ++c)
            for (const Who* r : rowsP) colTot[c] += cell[{ r->id, colsP[c]->id }];
        const size_t peak = colTot.empty() ? 0 : (size_t)(std::max_element(colTot.begin(), colTot.end()) - colTot.begin());
        const ImU32 tint = kind == 0 ? IM_COL32(226, 80, 63, 0) : kind == 1 ? IM_COL32(108, 196, 86, 0)
                         : kind == 2 ? IM_COL32(230, 200, 70, 0) : IM_COL32(170, 110, 220, 0);

        // Column heads: a tag row, the profession icon, then the name (without its build prefix) on at
        // most two lines; hovering a head gives the full name.
        const float nameW = 190.f, cw = 92.f, ch = 28.f, iconS = 22.f;
        const float tagY = 0.f, iconY = lineH + 8.f, nameY = iconY + iconS + 4.f, headH = nameY + lineH * 2.f + 8.f;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(nameW + cw * (colsP.size() + 1) + 10.f, headH + ch * (rowsP.size() + 1) + 10.f));
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int hovR = -1, hovC = -1, hovHead = -1;
        if (ImGui::IsWindowHovered())
            for (size_t c = 0; c < colsP.size(); ++c)
            {
                const float x = o.x + nameW + c * cw;
                if (mouse.x >= x && mouse.x < x + cw - 2.f && mouse.y >= o.y && mouse.y < o.y + headH) { hovHead = (int)c; hovC = (int)c; }
                for (size_t r = 0; r < rowsP.size(); ++r)
                {
                    const float y = o.y + headH + r * ch;
                    if (mouse.x >= x && mouse.x < x + cw - 2.f && mouse.y >= y && mouse.y < y + ch - 2.f) { hovR = (int)r; hovC = (int)c; }
                }
            }
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        for (size_t c = 0; c < colsP.size(); ++c)
        {
            const float x = o.x + nameW + c * cw, cx = x + (cw - 2.f) * 0.5f;
            if (c == peak && colTot[c] > 0)
            {
                const float ts = fs * 0.85f, tw = font->CalcTextSizeA(ts, FLT_MAX, 0.f, kMost[kind]).x;
                dl->AddRectFilled(ImVec2(cx - tw * 0.5f - 6.f, o.y + tagY), ImVec2(cx + tw * 0.5f + 6.f, o.y + tagY + ts + 5.f), IM_COL32(255, 255, 255, 28), 3.f);
                dl->AddText(font, ts, ImVec2(cx - tw * 0.5f, o.y + tagY + 2.f), kNameText, kMost[kind]);
            }
            if (ImTextureID pt = colsP[c]->prof ? host.ProfIcon(colsP[c]->prof) : nullptr)
                dl->AddImage(pt, ImVec2(cx - iconS * 0.5f, o.y + iconY), ImVec2(cx + iconS * 0.5f, o.y + iconY + iconS));
            const auto [l1, l2] = TwoLines(ShortName(colsP[c]->name), cw - 8.f);
            const ImU32 nc = (int)c == hovC ? kNameText : TeamText(takeTeam);
            dl->AddText(ImVec2(cx - ImGui::CalcTextSize(l1.c_str()).x * 0.5f, o.y + nameY), nc, l1.c_str());
            if (!l2.empty()) dl->AddText(ImVec2(cx - ImGui::CalcTextSize(l2.c_str()).x * 0.5f, o.y + nameY + lineH), nc, l2.c_str());
        }
        if (hovHead >= 0 && hovR < 0) ImGui::SetTooltip("%s", colsP[hovHead]->name.c_str());
        dl->AddText(ImVec2(o.x + nameW + colsP.size() * cw + 6.f, o.y + headH - lineH - 4.f), kMuted, "Total");
        for (size_t r = 0; r < rowsP.size(); ++r)
        {
            const float y = o.y + headH + r * ch;
            if (ImTextureID pt = rowsP[r]->prof ? host.ProfIcon(rowsP[r]->prof) : nullptr)
                dl->AddImage(pt, ImVec2(o.x, y + 4.f), ImVec2(o.x + 20.f, y + 24.f));
            dl->AddText(ImVec2(o.x + 26.f, y + 6.f), (int)r == hovR ? kNameText : TeamText(giveTeam), rowsP[r]->name.c_str());
            long long rt = 0;
            for (size_t c = 0; c < colsP.size(); ++c)
            {
                const long long v = cell[{ rowsP[r]->id, colsP[c]->id }];
                rt += v;
                const float x = o.x + nameW + c * cw;
                const bool traced = (int)r == hovR || (int)c == hovC;
                const int a = std::min(255, (int)(255.f * (0.10f + 0.70f * (float)v / (float)mx)) + (traced ? 40 : 0));
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw - 2.f, y + ch - 2.f), v ? (tint | ((ImU32)a << 24)) : IM_COL32(255, 255, 255, traced ? 16 : 8), 2.f);
                if ((int)r == hovR && (int)c == hovC) dl->AddRect(ImVec2(x, y), ImVec2(x + cw - 2.f, y + ch - 2.f), IM_COL32(225, 190, 80, 0xE0), 2.f);
                const std::string s = v ? FmtN(v) : "-";
                const float tw = ImGui::CalcTextSize(s.c_str()).x;
                dl->AddText(ImVec2(x + (cw - 2.f - tw) * 0.5f, y + 6.f), v ? kNameText : IM_COL32(111, 106, 94, 255), s.c_str());
            }
            dl->AddText(ImVec2(o.x + nameW + colsP.size() * cw + 6.f, y + 6.f), kMuted, FmtN(rt).c_str());
        }
        {
            const float y = o.y + headH + rowsP.size() * ch + 2.f;
            dl->AddText(ImVec2(o.x + 26.f, y + 6.f), kMuted, "Total");
            for (size_t c = 0; c < colsP.size(); ++c)
            {
                const std::string s = FmtN(colTot[c]);
                const float tw = ImGui::CalcTextSize(s.c_str()).x;
                dl->AddText(ImVec2(o.x + nameW + c * cw + (cw - 2.f - tw) * 0.5f, y + 6.f), kMuted, s.c_str());
            }
        }
        if (hovR >= 0)
        {
            const long long v = cell[{ rowsP[hovR]->id, colsP[hovC]->id }];
            ImGui::BeginTooltip();
            ImGui::TextColored(V4(TeamText(giveTeam)), "%s", rowsP[hovR]->name.c_str());
            ImGui::SameLine(0.f, 4.f); ImGui::TextUnformatted("to"); ImGui::SameLine(0.f, 4.f);
            ImGui::TextColored(V4(TeamText(takeTeam)), "%s", colsP[hovC]->name.c_str());
            ImGui::Text("%s %s", v ? FmtN(v).c_str() : "No", kVerb[kind]);
            ImGui::EndTooltip();
        }
        ImGui::TextColored(V4(kMuted), "-  none in this stretch");
    }
}

}   // namespace BattleLedger
