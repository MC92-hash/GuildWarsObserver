#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
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

    m_energyTracks = EnergyModel::Build(m_replayCtx.agents, in);
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

// The lamp's tooltip: how sure the bar is, the range, and why - the doubts since the band was last
// closed, newest first, repeats of one skill folded into a count.
std::string EnergyConfidenceText(const EnergyModel::Track& track, float t, const SkillDatabaseView& skills,
                                 const std::unordered_map<int, AgentReplayData>& agents, bool legend)
{
    using EnergyModel::DoubtKind;
    const EnergyModel::Sample s = track.At(t);
    static const char* kLevel[] = { "high (green)", "medium (orange)", "low (red)" };
    std::string out = std::string("Energy confidence: ") + kLevel[static_cast<int>(track.ConfidenceAt(t))];
    if (legend)
        out += "\n\nNo recording states another player's energy: the bar is calculated."
               "\n  Green   the real energy is within 3 points of the bar"
               "\n  Orange  it may be off by up to 10"
               "\n  Red     it may be off by more than 10, or the player just cast a skill"
               "\n          the bar said they could not afford"
               "\nThe lamp turns green again as the player regenerates to full, or when"
               "\nthe recording proves the value (shrine, morale boost, Mind Wrack ending).";
    if (legend && !s.dead) out += "\n";
    if (s.dead) return out;
    char line[256];
    const int lo = std::max(0, static_cast<int>(std::floor(s.energy - s.down)));
    const int hi = std::max(0, static_cast<int>(std::floor(s.energy + s.up)));
    if (hi > lo) { snprintf(line, sizeof(line), "\nReal energy between %d and %d", lo, hi); out += line; }

    struct Row { DoubtKind kind; int skillId; int other; float last; int n; };
    std::vector<Row> rows;
    for (const EnergyModel::Doubt* d : track.DoubtsAt(t))
    {
        auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& r) {
            return r.kind == d->kind && r.skillId == d->skillId && r.other == d->other; });
        if (it != rows.end()) ++it->n;
        else rows.push_back({ d->kind, d->skillId, d->other, d->time, 1 });
    }
    int shown = 0;
    for (const Row& r : rows)
    {
        if (++shown > 6) { out += "\n..."; break; }
        const SkillInfo* si = skills.Get(r.skillId);
        const char* skill = si ? si->name.c_str() : "?";
        std::string who;
        if (auto a = agents.find(r.other); r.other > 0 && a != agents.end()) who = a->second.playerName;
        const int mm = static_cast<int>(r.last) / 60, ss = static_cast<int>(r.last) % 60;
        switch (r.kind)
        {
        case DoubtKind::Resurrection:
            snprintf(line, sizeof(line), "\n%02d:%02d  Resurrected by %s%s%s: energy on return depends on a rank not known",
                     mm, ss, skill, who.empty() ? "" : " from ", who.c_str());
            break;
        case DoubtKind::Expertise:
            snprintf(line, sizeof(line), "\n%02d:%02d  %s (x%d): cost cut by Expertise, rank not solved", mm, ss, skill, r.n);
            break;
        case DoubtKind::Mysticism:
            snprintf(line, sizeof(line), "\n%02d:%02d  %s (x%d): cost cut by Mysticism, rank not solved", mm, ss, skill, r.n);
            break;
        case DoubtKind::AvatarOfLyssa:
            snprintf(line, sizeof(line), "\n%02d:%02d  Avatar of Lyssa%s%s (x%d): may have run out, Mysticism not solved",
                     mm, ss, who.empty() ? "" : " of ", who.c_str(), r.n);
            break;
        case DoubtKind::Contradiction:
            snprintf(line, sizeof(line), "\n%02d:%02d  Cast %s, which the model said they could not afford", mm, ss, skill);
            break;
        case DoubtKind::Aneurysm:
            snprintf(line, sizeof(line), "\n%02d:%02d  Aneurysm%s%s next to them: loss capped by a rank not known",
                     mm, ss, who.empty() ? "" : " from ", who.c_str());
            break;
        }
        out += line;
    }
    return out;
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
        DrawEnergyConfidenceLamp(dl, LoadGameUITexture(m_deviceResources->GetD3DDevice(), "Progressbar\ui_confidence_lamps.png"),
                                 ImVec2(b1.x - d * 0.5f, std::round((b0.y + b1.y) * 0.5f)), d, level);
    }

    if (ImGui::IsMouseHoveringRect(b0, b1))
    {
        const int e = s.dead ? 0 : std::max(0, static_cast<int>(std::floor(s.energy)));
        const int m = static_cast<int>(std::floor(s.maxEnergy));
        const std::string why = EnergyConfidenceText(*track, m_debugTimeline, m_skillView, m_replayCtx.agents,
                                                     level != EnergyModel::Confidence::High);
        if (lift > 0.f)
            ImGui::SetTooltip("Energy %d / %d on the best weapon set (calculated)\nOn the set in hand: %d / %d%s\n\n%s",
                              s.dead ? 0 : std::max(0, static_cast<int>(std::floor(best))),
                              static_cast<int>(std::floor(s.maxEnergy + lift)), e, m,
                              s.energy < 0.f ? " (hiding energy)" : "", why.c_str());
        else
            ImGui::SetTooltip("Energy %d / %d (calculated)\n\n%s", e, m, why.c_str());
    }
}
