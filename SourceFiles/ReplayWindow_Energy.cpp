#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "HenchmanEnergy.h"
#include <algorithm>
#include <cctype>

// ---------------------------------------------------------------------------
// Energy bars. Nothing in a recording states another player's energy, so the value is calculated
// forward from the match start by EnergyModel (private): costs, regeneration, weapon sets,
// overcast and every recorded event that moves it. This file only wires the window's timelines
// into the model and draws the result.
// ---------------------------------------------------------------------------

void ReplayWindow::BuildEnergyTracks() const
{
    if (!m_moraleTimelineBuilt) BuildMoraleTimelines();

    EnergyModel::Inputs in;
    in.skills    = &m_skillView;
    in.stoc      = &m_replayCtx.stocData;
    in.equipment = &m_replayCtx.stocData.equipment;
    in.matchEnd  = m_replayCtx.maxReplayTime;
    in.attributeRank = [this](int agentId, int attribute) -> int {
        auto it = m_attrProfiles.find(agentId);
        if (it == m_attrProfiles.end()) return -1;
        auto a = it->second.attributes.find(attribute);
        if (a == it->second.attributes.end() || a->second.budgetOnly) return -1;
        return a->second.best;
    };
    in.attributeRange = [this](int agentId, int attribute, int& lo, int& hi) -> bool {
        auto it = m_attrProfiles.find(agentId);
        if (it == m_attrProfiles.end()) return false;
        auto a = it->second.attributes.find(attribute);
        if (a == it->second.attributes.end()) return false;
        lo = a->second.lo; hi = a->second.hi;
        return true;
    };
    in.moralePercent = [this](const AgentReplayData& ard, float t) { return MoralePercentAtTime(ard, t); };
    in.moraleBoosts = [this](int teamId) -> std::vector<float> {
        auto it = m_moraleBoosts.find(teamId);
        return it == m_moraleBoosts.end() ? std::vector<float>{} : it->second;
    };
    // The effect bar's timeline: some effects on a player change what his own skills do to his
    // energy, and the model reads them from here rather than finding them again. Null until the
    // skill-use timeline is in; the rebuild after the attribute solve has it.
    in.effects = EffectTimelineTable();
    // Maximum health as the attribute solve reads it: Energy Surge's damage packet is a fraction
    // of it, and 7 damage per point of energy the target lost.
    in.resolveMaxHp = [this](int agentId, float t) -> std::pair<uint32_t, bool> {
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return { 0, true };
        const auto s = ResolveMaxHp(it->second, t);
        return { s.value, s.estimated };
    };

    m_energyTracks = EnergyModel::Build(m_replayCtx.agents, in);
    m_energyTracks.merge(EnergyModel::BuildHenchmen(m_replayCtx.agents, in));
    m_energyBuilt = true;
    m_energyUsedAttributes = m_attributesDeduced;

    // Developer dump, the same way the attribute solve writes its own: %TEMP%\gwo_energy_<match>.txt
    // whenever GWO_ENERGY_DEBUG is set.
    char flag[16] = {};
    char tempDir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("GWO_ENERGY_DEBUG", flag, (DWORD)sizeof(flag)) != 0 &&
        GetTempPathA(MAX_PATH, tempDir) != 0)
    {
        std::string folder = m_replayCtx.matchFolderPath.filename().string();
        if (folder.empty()) folder = "replay";
        for (char& c : folder)
            if (!std::isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '_';
        EnergyModel::WriteDebugDump(std::string(tempDir) + "gwo_energy_" + folder + ".txt",
                                    m_replayCtx.agents, m_energyTracks, m_skillView);
    }
}

const EnergyModel::Track* ReplayWindow::EnergyTrackFor(int agentId) const
{
    if (!m_agentsClassified || !m_replayCtx.stocLoaded) return nullptr;
    if (!m_energyBuilt || (!m_energyUsedAttributes && m_attributesDeduced))
        BuildEnergyTracks();
    auto it = m_energyTracks.find(agentId);
    return it == m_energyTracks.end() || it->second.samples.empty() ? nullptr : &it->second;
}

// The bar spans the full maximum; the part overcast took is greyed at its right end, as the client
// does, with the game's grey fill (texture 0x32274, laid out like the energy fill: its filled column
// stretched over the overcast span).
void DrawEnergyOvercast(ImDrawList* dl, ImVec2 b0, ImVec2 b1, const EnergyModel::Sample& s, ImTextureID greyFill)
{
    const float full = s.maxEnergy + s.overcast;
    if (s.overcast <= 0.f || full <= 0.f) return;
    const float x = std::round(b0.x + (b1.x - b0.x) * std::clamp(s.maxEnergy / full, 0.f, 1.f));
    if (x >= b1.x) return;
    if (greyFill)
        dl->AddImage(greyFill, ImVec2(x, b0.y), b1, ImVec2(0.5f / 16.f, 0.f), ImVec2(0.5f / 16.f, 1.f));
    else
        dl->AddRectFilled(ImVec2(x, b0.y), b1, IM_COL32(0x6C, 0x6C, 0x6C, 0xFF));
}

float EnergyBarFraction(const EnergyModel::Sample& s)
{
    const float full = s.maxEnergy + s.overcast;
    return full > 0.f ? std::clamp(std::max(0.f, s.energy) / full, 0.f, 1.f) : 0.f;
}

// Where the real energy may be: a light band over the bar from the lowest to the highest value the
// model cannot rule out, its ends marked.
void DrawEnergyBand(ImDrawList* dl, ImVec2 b0, ImVec2 b1, const EnergyModel::Sample& s)
{
    const float full = s.maxEnergy + s.overcast;
    if (s.dead || full <= 0.f) return;
    const float lo = std::clamp(s.energy - s.down, 0.f, s.maxEnergy);
    const float hi = std::clamp(s.energy + s.up, 0.f, s.maxEnergy);
    if (hi - lo < 0.5f) return;
    const float w = b1.x - b0.x;
    const float x0 = std::round(b0.x + w * lo / full), x1 = std::round(b0.x + w * hi / full);
    dl->AddRectFilled(ImVec2(x0, b0.y), ImVec2(x1, b1.y), IM_COL32(0xFF, 0xFF, 0xFF, 0x38));
    dl->AddLine(ImVec2(x0 + 0.5f, b0.y), ImVec2(x0 + 0.5f, b1.y), IM_COL32(0xFF, 0xFF, 0xFF, 0xA0));
    dl->AddLine(ImVec2(x1 - 0.5f, b0.y), ImVec2(x1 - 0.5f, b1.y), IM_COL32(0xFF, 0xFF, 0xFF, 0xA0));
}

// The confidence lamp: the game's three lamps (texture 277147, green / orange / red, 16 px each side
// by side) on a dark disc, since the glass is half transparent.
void DrawEnergyConfidenceLamp(ImDrawList* dl, ImTextureID lamps, ImVec2 c, float d, EnergyModel::Confidence level)
{
    const float r = std::round(d * 0.5f);
    dl->AddCircleFilled(c, r * 0.8f, IM_COL32(0, 0, 0, 0xC0));
    const int i = static_cast<int>(level);
    if (!lamps)
    {
        static const ImU32 kCol[] = { IM_COL32(0x30, 0xC8, 0x30, 0xFF), IM_COL32(0xFF, 0xA0, 0x32, 0xFF),
                                      IM_COL32(0xFF, 0x4A, 0x3C, 0xFF) };
        dl->AddCircleFilled(c, r * 0.7f, kCol[i]);
        return;
    }
    const float u0 = static_cast<float>(i) * 0.25f;
    dl->AddImage(lamps, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(u0, 0.f), ImVec2(u0 + 0.25f, 1.f));
}

// ---------------------------------------------------------------------------
// The energy tooltips, in the skill tooltip's look (DrawGameSkillTooltip): dark parchment, gold rim,
// cream title, the game's cost icons inline where the text would say "energy", "overcast" or
// "regeneration".
//
//   bar      the value, the regeneration with every source behind it, overcast and its rules, the
//            range the real value lies in; the party strip adds why the lamp is not green
//   lamp     what each of the three lamps means (the one showing now lit), the range, and why
// ---------------------------------------------------------------------------
namespace
{
    constexpr ImU32 kTipName  = IM_COL32(0xF2, 0xEC, 0xD6, 0xFF);
    constexpr ImU32 kTipPlain = IM_COL32(0xE6, 0xE6, 0xE6, 0xFF);
    constexpr ImU32 kTipMuted = IM_COL32(0xA8, 0xA8, 0xA0, 0xFF);
    constexpr ImU32 kTipGain  = IM_COL32(0x5C, 0xE6, 0x3C, 0xFF);
    constexpr ImU32 kTipLoss  = IM_COL32(0xFF, 0x6A, 0x5C, 0xFF);
    constexpr ImU32 kLampText[] = { IM_COL32(0x5C, 0xE6, 0x3C, 0xFF), IM_COL32(0xFF, 0xA0, 0x32, 0xFF),
                                    IM_COL32(0xFF, 0x5A, 0x4A, 0xFF) };
    constexpr float kTipW      = 330.f;
    constexpr float kTipIndent = 22.f;   // a breakdown row's value column

    const char* ProfName(int prof)
    {
        static const char* kNames[] = { "", "Warrior", "Ranger", "Monk", "Necromancer", "Mesmer",
                                        "Elementalist", "Assassin", "Ritualist", "Paragon", "Dervish" };
        return prof >= 0 && prof <= 10 ? kNames[prof] : "";
    }

    void BeginEnergyTip()
    {
        ImGui::PushStyleColor(ImGuiCol_PopupBg,   ImVec4(0.11f, 0.105f, 0.085f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border,    ImVec4(0.62f, 0.58f, 0.46f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.62f, 0.58f, 0.46f, 0.40f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.f, 5.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
        ImGui::BeginTooltip();
        ImGui::Dummy(ImVec2(kTipW, 0.f));
    }

    void EndEnergyTip()
    {
        ImGui::EndTooltip();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
    }

    // The pieces of one line: each leaves the cursor right after itself; Br() ends the line.
    void Txt(ImU32 col, const char* s)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
        ImGui::TextUnformatted(s);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.f, 0.f);
    }

    void Icon(ImTextureID tex)
    {
        const float h = ImGui::GetTextLineHeight();
        if (tex) ImGui::Image(tex, ImVec2(h, h));
        else     ImGui::Dummy(ImVec2(h, h));
        ImGui::SameLine(0.f, 0.f);
    }

    void Lamp(ImTextureID lamps, EnergyModel::Confidence level)
    {
        const float h = ImGui::GetTextLineHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(h, h));
        DrawEnergyConfidenceLamp(ImGui::GetWindowDrawList(), lamps, ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h, level);
        ImGui::SameLine(0.f, 0.f);
    }

    void Br() { ImGui::NewLine(); }

    void Rule()
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    // Text placed so it ends at the tooltip's right edge, on the current line.
    void TxtRight(ImU32 col, const char* s)
    {
        ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + kTipW - ImGui::CalcTextSize(s).x);
        Txt(col, s);
    }

    // A pip count's colour: green for a gain, red for a loss.
    ImU32 PipColour(int pips) { return pips > 0 ? kTipGain : pips < 0 ? kTipLoss : kTipPlain; }
}

// Why the band is open: the doubts since it was last closed, newest first, repeats of one skill
// folded into a count. Each: time, the skill, who, then the reason under it.
void ReplayWindow::DrawEnergyDoubts(const EnergyModel::Track& track, float t)
{
    using EnergyModel::DoubtKind;
    struct Row { DoubtKind kind; int skillId; int other; float last; int n; };
    std::vector<Row> rows;
    for (const EnergyModel::Doubt* d : track.DoubtsAt(t))
    {
        auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& r) {
            return r.kind == d->kind && r.skillId == d->skillId && r.other == d->other; });
        if (it != rows.end()) ++it->n;
        else rows.push_back({ d->kind, d->skillId, d->other, d->time, 1 });
    }
    if (rows.empty()) return;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImTextureID texEnergy = LoadGameUITexture(dev, "Skill Description\\energy.png");
    EnsureSkillIconIndex();

    Rule();
    Txt(kTipName, "Why");
    Br();
    char buf[96];
    int shown = 0;
    for (const Row& r : rows)
    {
        if (++shown > 6)
        {
            snprintf(buf, sizeof(buf), "and %d more", static_cast<int>(rows.size()) - 6);
            Txt(kTipMuted, buf);
            Br();
            break;
        }
        snprintf(buf, sizeof(buf), "%02d:%02d  ", static_cast<int>(r.last) / 60, static_cast<int>(r.last) % 60);
        Txt(kTipMuted, buf);
        Icon(LoadSkillIcon(this, dev, r.skillId, m_skillIconIndex, m_skillIconCache));
        const SkillInfo* si = m_skillView.Get(r.skillId);
        Txt(kTipName, (std::string(" ") + (si ? si->name : std::string("?"))).c_str());
        if (r.n > 1) { snprintf(buf, sizeof(buf), "  x%d", r.n); Txt(kTipMuted, buf); }
        if (auto a = m_replayCtx.agents.find(r.other);
            r.other > 0 && a != m_replayCtx.agents.end() && !a->second.playerName.empty())
            Txt(kTipMuted, (std::string("  ") + a->second.playerName).c_str());
        Br();

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kTipIndent);
        switch (r.kind)
        {
        case DoubtKind::Resurrection:
            Txt(kTipPlain, "Resurrected: the ");
            Icon(texEnergy);
            Txt(kTipPlain, " given back depends on a rank not known");
            break;
        case DoubtKind::Expertise:
            Txt(kTipPlain, "Cost cut by Expertise, rank not solved");
            break;
        case DoubtKind::Mysticism:
            Txt(kTipPlain, "Cost cut by Mysticism, rank not solved");
            break;
        case DoubtKind::AvatarOfLyssa:
            Txt(kTipPlain, "Avatar of Lyssa may have run out: Mysticism not solved");
            break;
        case DoubtKind::Contradiction:
            Txt(kTipPlain, "Cast while the bar held less ");
            Icon(texEnergy);
            Txt(kTipPlain, " than it costs");
            break;
        case DoubtKind::Aneurysm:
            Txt(kTipPlain, "Aneurysm next to them: loss capped by a rank not known");
            break;
        }
        Br();
    }
}

// The lamp's tooltip: the three lamps and what each means, the one showing now lit; the range; why.
void ReplayWindow::DrawEnergyConfidenceTooltip(const EnergyModel::Track& track)
{
    const float t = m_debugTimeline;
    const EnergyModel::Sample s = track.At(t);
    const EnergyModel::Confidence level = track.ConfidenceAt(t);
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImTextureID texEnergy = LoadGameUITexture(dev, "Skill Description\\energy.png");
    ImTextureID texLamps  = LoadGameUITexture(dev, "Progressbar\\ui_confidence_lamps.png");

    BeginEnergyTip();
    static const char* kLevelName[] = { "High", "Medium", "Low" };
    Icon(texEnergy);
    Txt(kTipName, " Confidence level");
    TxtRight(kLampText[static_cast<int>(level)], kLevelName[static_cast<int>(level)]);
    Br();
    Rule();

    // The legend; the lamp showing now on a light band.
    const float h = ImGui::GetTextLineHeight();
    const float descX = ImGui::GetStyle().WindowPadding.x + h + 8.f;
    for (int i = 0; i < 3; ++i)
    {
        const bool now = i == static_cast<int>(level);
        const int lines = i == 2 ? 2 : 1;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (now)
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(p.x - 3.f, p.y - 1.f),
                ImVec2(p.x + kTipW + 3.f, p.y + lines * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y + 1.f),
                IM_COL32(0xFF, 0xEE, 0xBB, 0x1C), 2.f);
        const ImU32 col = now ? kTipPlain : kTipMuted;
        Lamp(texLamps, static_cast<EnergyModel::Confidence>(i));
        ImGui::SameLine(descX);
        Txt(now ? kLampText[i] : kTipMuted, kLevelName[i]);
        Txt(col, " - ");
        switch (i)
        {
        case 0: Txt(col, "within 3 "); Icon(texEnergy); Txt(col, " of the real value"); break;
        case 1: Txt(col, "may be off by up to 10 "); Icon(texEnergy); break;
        case 2:
            Txt(col, "off by more than 10 ");
            Icon(texEnergy);
            Br();
            ImGui::SetCursorPosX(descX);
            Txt(col, "or a skill cast the bar said they could not afford");
            break;
        }
        Br();
    }

    if (!s.dead)
    {
        const int lo = std::max(0, static_cast<int>(std::floor(s.energy - s.down)));
        const int hi = std::max(0, static_cast<int>(std::floor(s.energy + s.up)));
        if (hi > lo)
        {
            Rule();
            char buf[64];
            snprintf(buf, sizeof(buf), "Real value between %d and %d ", lo, hi);
            Txt(kTipPlain, buf);
            Icon(texEnergy);
            Br();
        }
        DrawEnergyDoubts(track, t);
    }

    Rule();
    Icon(texEnergy);
    Txt(kTipMuted, " is calculated from the match events, not recorded.");
    Br();
    EndEnergyTip();
}

// The bar's tooltip. `lift`: energy the best weapon set adds over the set in hand (the party strip
// reads the best set); `reasons`: list why the lamp is not green (the party strip has no lamp to hover).
void ReplayWindow::DrawEnergyBarTooltip(const AgentReplayData& ard, const EnergyModel::Track& track,
                                        float lift, bool reasons)
{
    using PS = EnergyModel::Track::PipSource;
    const float t = m_debugTimeline;
    const EnergyModel::Sample s = track.At(t);
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImTextureID texEnergy   = LoadGameUITexture(dev, "Skill Description\\energy.png");
    ImTextureID texOvercast = LoadGameUITexture(dev, "Skill Description\\overcast.png");
    ImTextureID texPips     = LoadGameUITexture(dev, "Skill Description\\upkeep.png");
    ImTextureID texLamps    = LoadGameUITexture(dev, "Progressbar\\ui_confidence_lamps.png");
    char buf[128];

    BeginEnergyTip();

    // Header: the value (the client shows whole points, 0 below zero), "calculated" right.
    const int inHand  = s.dead ? 0 : std::max(0, static_cast<int>(std::floor(s.energy)));
    const int maxHand = static_cast<int>(std::floor(s.maxEnergy));
    Icon(texEnergy);
    if (lift > 0.f)
        snprintf(buf, sizeof(buf), " %d / %d", s.dead ? 0 : std::max(0, static_cast<int>(std::floor(s.energy + lift))),
                 static_cast<int>(std::floor(s.maxEnergy + lift)));
    else
        snprintf(buf, sizeof(buf), " %d / %d", inHand, maxHand);
    Txt(kTipName, buf);
    TxtRight(kTipMuted, "calculated");
    Br();
    if (lift > 0.f)
    {
        snprintf(buf, sizeof(buf), "On the best weapon set. In hand: %d / %d%s", inHand, maxHand,
                 s.energy < 0.f ? " (hiding)" : "");
        Txt(kTipMuted, buf);
        Br();
    }
    if (s.dead)
    {
        EndEnergyTip();
        return;
    }

    // Regeneration: the net in pips and as a rate, then every source behind it.
    Rule();
    const int pips = s.pips;
    Icon(texPips);
    snprintf(buf, sizeof(buf), " %+d", pips);
    Txt(PipColour(pips), buf);
    if (pips == 0)
        Txt(kTipPlain, "   no regeneration");
    else
    {
        const float every = 3.f / static_cast<float>(std::abs(pips));
        Txt(kTipPlain, pips > 0 ? "   gains 1 " : "   loses 1 ");
        Icon(texEnergy);
        if (std::fabs(every - 1.f) < 0.01f) snprintf(buf, sizeof(buf), " every second");
        else                                 snprintf(buf, sizeof(buf), " every %.2g s", every);
        Txt(kTipPlain, buf);
    }
    Br();

    EnsureSkillIconIndex();
    int sum = 0;
    const float numW   = ImGui::CalcTextSize("+10").x;
    const float valueX = ImGui::GetStyle().WindowPadding.x + kTipIndent;
    const float labelX = valueX + numW + 10.f;
    for (const EnergyModel::Track::PipTerm* p : track.PipsAt(t))
    {
        sum += p->pips;
        snprintf(buf, sizeof(buf), "%+d", static_cast<int>(p->pips));
        ImGui::SetCursorPosX(valueX + numW - ImGui::CalcTextSize(buf).x);
        Txt(PipColour(p->pips), buf);
        ImGui::SameLine(labelX);
        switch (p->source)
        {
        case PS::Base:
            snprintf(buf, sizeof(buf), "Base (%s)", ProfName(ard.primaryProf));
            Txt(kTipPlain, buf);
            break;
        case PS::Weapon:
            Txt(kTipPlain, p->zealous ? "Zealous weapon" : "Weapon mod");
            break;
        case PS::Upkeep:
        case PS::Effect:
        {
            const SkillInfo* si = m_skillView.Get(p->skillId);
            Icon(LoadSkillIcon(this, dev, p->skillId, m_skillIconIndex, m_skillIconCache));
            Txt(kTipPlain, (std::string(" ") + (si ? si->name : std::string("?"))).c_str());
            if (p->source == PS::Upkeep) Txt(kTipMuted, "  maintained");
            break;
        }
        }
        Br();
    }
    if (std::abs(sum) > 10)
    {
        ImGui::SetCursorPosX(labelX);
        Txt(kTipMuted, "Capped at 10 either way");
        Br();
    }

    // Overcast and its rules, while the player carries any.
    if (s.overcast >= 1.f)
    {
        Rule();
        Icon(texOvercast);
        snprintf(buf, sizeof(buf), " %d", static_cast<int>(std::floor(s.overcast)));
        Txt(kTipName, buf);
        Txt(kTipPlain, "   maximum ");
        Icon(texEnergy);
        snprintf(buf, sizeof(buf), " lowered from %d to %d",
                 static_cast<int>(std::floor(s.maxEnergy + s.overcast)), maxHand);
        Txt(kTipPlain, buf);
        Br();
        ImGui::SetCursorPosX(valueX);
        Txt(kTipMuted, "Each ");
        Icon(texOvercast);
        Txt(kTipMuted, " skill lowers the maximum by its overcast cost");
        Br();
        ImGui::SetCursorPosX(valueX);
        Txt(kTipMuted, "1 point comes back every 3 s; it stays through death");
        Br();
        ImGui::SetCursorPosX(valueX);
        Txt(kTipMuted, "Shown grey at the right end of the bar");
        Br();
    }

    // Where the real value lies, while the model is not sure of it.
    const EnergyModel::Confidence level = track.ConfidenceAt(t);
    const int lo = std::max(0, static_cast<int>(std::floor(s.energy - s.down)));
    const int hi = std::max(0, static_cast<int>(std::floor(s.energy + s.up)));
    if (hi > lo)
    {
        Rule();
        Lamp(texLamps, level);
        snprintf(buf, sizeof(buf), "  Real value between %d and %d ", lo, hi);
        Txt(kTipPlain, buf);
        Icon(texEnergy);
        Br();
    }
    if (reasons && level != EnergyModel::Confidence::High)
        DrawEnergyDoubts(track, t);

    EndEnergyTip();
}

// The thin energy strip under a party row's health bar.
//
// It shows the energy a player has to work with: what they would hold on their BEST weapon set. A
// swap moves current energy and maximum by the same amount (and below zero when they hide energy),
// so this number stays put through every swap and moves only when they spend, regenerate or are
// drained. 36 on a +47 set reads 36/72; swapped to a +0 set the player holds -11 (the client shows 0),
// and the strip still reads 36/72.
//   [0, energy on best set]   blue, orange under 30 %, red under 15 %
//   [.., best set max]        near-black
//   grey                      taken by overcast
//   light marker              the maximum of the set in hand; hatched beyond it (only reachable
//                             by swapping)
void ReplayWindow::DrawPartyEnergyBar(ImDrawList* dl, const AgentReplayData& ard, ImVec2 b0, ImVec2 b1) const
{
    const EnergyModel::Track* track = EnergyTrackFor(ard.agent_id);
    if (!track) return;
    const EnergyModel::Sample s = track->At(m_debugTimeline);

    const float lift    = static_cast<float>(std::max(0, track->peakWeaponTerm - s.weaponTerm));
    const float setMax  = s.maxEnergy + s.overcast;         // the set in hand, before overcast
    const float bestMax = setMax + lift;                     // the best set, now
    const float best    = s.energy + lift;                   // energy on the best set, now
    if (bestMax <= 0.f) return;
    const float w = b1.x - b0.x;
    auto X = [&](float v) { return std::round(b0.x + w * std::clamp(v / bestMax, 0.f, 1.f)); };

    // An outline round the whole strip, so it reads on a red and a blue panel alike.
    dl->AddRectFilled(ImVec2(b0.x - 1.f, b0.y - 1.f), ImVec2(b1.x + 1.f, b1.y + 1.f), IM_COL32(0, 0, 0, 0xFF));

    const float xSet = X(setMax);
    dl->AddRectFilled(b0, ImVec2(xSet, b1.y), IM_COL32(0x0C, 0x0C, 0x12, 0xFF));
    if (xSet < b1.x)
    {
        dl->AddRectFilled(ImVec2(xSet, b0.y), b1, IM_COL32(0x22, 0x26, 0x34, 0xFF));
        dl->PushClipRect(ImVec2(xSet, b0.y), b1, true);
        const float h = b1.y - b0.y;
        for (float x = xSet - h; x < b1.x; x += 4.f)
            dl->AddLine(ImVec2(x, b1.y), ImVec2(x + h, b0.y), IM_COL32(0x78, 0x8C, 0xB4, 0xC0), 1.f);
        dl->PopClipRect();
    }
    if (!s.dead)
    {
        const float frac = std::max(0.f, best) / bestMax;
        ImU32 top = IM_COL32(0x5A, 0x96, 0xFF, 0xFF), bot = IM_COL32(0x23, 0x50, 0xD2, 0xFF);
        ImU32 edge = IM_COL32(0xB4, 0xD2, 0xFF, 0xFF);
        if (frac < 0.15f)      { top = IM_COL32(0xFF, 0x4A, 0x3C, 0xFF); bot = IM_COL32(0xB4, 0x1E, 0x14, 0xFF); edge = IM_COL32(0xFF, 0xB4, 0xA8, 0xFF); }
        else if (frac < 0.30f) { top = IM_COL32(0xFF, 0xA0, 0x32, 0xFF); bot = IM_COL32(0xC8, 0x64, 0x0A, 0xFF); edge = IM_COL32(0xFF, 0xDC, 0xA0, 0xFF); }
        const float x = X(std::max(0.f, best));
        if (x > b0.x)
        {
            dl->AddRectFilledMultiColor(b0, ImVec2(x, b1.y), top, top, bot, bot);
            dl->AddLine(ImVec2(b0.x, b0.y + 0.5f), ImVec2(x, b0.y + 0.5f), edge);
        }
    }
    if (s.overcast > 0.f)
        dl->AddRectFilled(ImVec2(X(s.maxEnergy + lift), b0.y), ImVec2(X(bestMax), b1.y), IM_COL32(0x8C, 0x8C, 0x8C, 0xFF));
    if (xSet < b1.x)
        dl->AddLine(ImVec2(xSet + 0.5f, b0.y - 1.f), ImVec2(xSet + 0.5f, b1.y + 1.f), IM_COL32(0xE6, 0xEB, 0xF5, 0xFF), 1.f);

    // The confidence lamp at the strip's right end, only when the model is unsure: eight green
    // lamps would say nothing.
    const EnergyModel::Confidence level = track->ConfidenceAt(m_debugTimeline);
    if (level != EnergyModel::Confidence::High && !s.dead)
    {
        const float d = 11.f;
        DrawEnergyConfidenceLamp(dl, LoadGameUITexture(m_deviceResources->GetD3DDevice(), "Progressbar\\ui_confidence_lamps.png"),
                                 ImVec2(b1.x - d * 0.5f, std::round((b0.y + b1.y) * 0.5f)), d, level);
    }

    if (ImGui::IsMouseHoveringRect(b0, b1))
        const_cast<ReplayWindow*>(this)->DrawEnergyBarTooltip(ard, *track, lift, true);
}
