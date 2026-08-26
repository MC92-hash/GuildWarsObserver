#include "pch.h"
#include "ReplayWindow.h"
#include "AssetBlacklist.h"
#include "MatchRatings.h"
#include "MatchNotes.h"
#include "MatchBookmarks.h"
#include "AgentSnapshotParser.h"
#include "StoCParser.h"
#include "SkillDatabase.h"
#include "DXMathHelpers.h"
#include "FontConfig.h"
#include "GuiGlobalConstants.h"
#include "MapBrowser.h"
#include "TextureCache.h"
#include "CursorSystem.h"
#include "SpatialAudioEngine.h"
#include "SoundCache.h"
#include "Parsers/BB9AnimationParser.h"
#include "Parsers/FileReferenceParser.h"
#include "ReplayWindow_Internal.h"
#include "../ThirdParty/nanosvg/nanosvg.h"
#include "../ThirdParty/nanosvg/nanosvgrast.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <algorithm>
#include <numeric>
#include <json.hpp>
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// Extracted from ReplayWindow.cpp (partial-class split). These remain
// ReplayWindow:: member functions; only their definitions live here.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Morale computation (shared by Morale Panel + Player Info Panel)
// ---------------------------------------------------------------------------

// Morale as a step function per player, folded once at load.
//
// A death costs 15, a morale boost gives 10 and cancels death penalty before it adds anything, and
// killing an enemy player claws back 2 for every teammate still alive. Percentages are of the level
// health -- 480 at level 20 -- so a point is 4.8 health.
//
// Checked against camera-recorded maximum health: two readings taken on the same weapon set must
// differ by exactly the morale that moved between them, whatever the player's armour and weapon
// mods are, because those cancel. Across six matches 33 such pairs exist; 30 agreed to the health
// point and the three that did not were all the Death Pact Signet case below.
namespace
{
    // Death Pact Signet strikes the caster down when the ally they resurrected dies. That death is
    // the signet's price rather than a defeat: it carries no death penalty for the caster, and it
    // pays the enemy team no kill. Dropping it from the death list does both at once, since enemy
    // kills are read from the same list.
    //
    // The ids here were wrong for the whole life of this code -- 5413 and 8059 appear nowhere in
    // the skill database, so the rule never fired once. The signature is unmistakable in the data:
    // two players died at 90% and 97% health, each in the same frame as the ally they had just
    // resurrected, and each left a Dervish reading 13% better than the model predicted -- one on
    // either team. Correcting the ids reconciles all three outliers in the sample.
    constexpr int kDeathPactSignet    = 1481;
    constexpr int kDeathPactSignetPvP = 2872;

    // How long the caster stays bound to the ally. The skill sets no limit of its own, so this only
    // stops a link outliving the fight it belongs to; the real test is that the two die together.
    constexpr float kDeathPactLinkSeconds = 120.f;
    constexpr float kDeathPactDeathGap    = 1.5f;

    // A resurrection and the death that follows it arrive on separate packets, and for a moment the
    // agent can read dead again. Anything this soon after standing up is that echo, not a death.
    constexpr float kResurrectEchoSeconds = 5.f;
}

void ReplayWindow::BuildMoraleTimelines() const
{
    m_moraleTimeline.clear();
    m_moraleDeaths.clear();
    m_moraleBoosts.clear();

    // A pet takes death penalty on the same terms its owner does, so it needs a timeline too.
    // Its own death pays the enemy nothing though -- only a player kill claws penalty back --
    // which is why the enemy-kill events below are filtered back down to players.
    auto CarriesMorale = [](const AgentReplayData& a) {
        return a.type == AgentType::Player
            || (a.type == AgentType::NPC && IsPetModelId(a.modelId));
    };

    struct DPSLink { int casterId; int targetId; float linkStart; float linkEnd; };
    std::vector<DPSLink> dpsLinks;
    for (const auto& [aid, agentData] : m_replayCtx.agents) {
        if (agentData.type != AgentType::Player) continue;
        for (const auto& ev : agentData.skillUseHistory) {
            if (ev.skillId != kDeathPactSignet && ev.skillId != kDeathPactSignetPvP) continue;
            if (ev.wasCancelled || ev.wasInterrupted || ev.targetId <= 0) continue;
            dpsLinks.push_back({ aid, ev.targetId, ev.endTime, ev.endTime + kDeathPactLinkSeconds });
        }
    }

    // Every death-penalty-carrying death, once, so each team can read the other team's as kills
    // and neither side counts a signet backfire. Pets are in here for their own penalty only.
    for (const auto& [aid, ard] : m_replayCtx.agents) {
        if (!CarriesMorale(ard)) continue;
        auto& times = m_moraleDeaths[aid];
        float lastResTime = -999.f;
        for (size_t i = 1; i < ard.snapshots.size(); ++i) {
            if (!ard.snapshots[i].is_dead && ard.snapshots[i - 1].is_dead)
                lastResTime = ard.snapshots[i].time;
            if (!ard.snapshots[i].is_dead || ard.snapshots[i - 1].is_dead) continue;
            const float dt = ard.snapshots[i].time;
            if (dt - lastResTime < kResurrectEchoSeconds) continue;

            bool dpsLinked = false;
            for (const auto& link : dpsLinks) {
                if (link.casterId != aid) continue;
                if (dt < link.linkStart || dt > link.linkEnd) continue;
                auto tit = m_replayCtx.agents.find(link.targetId);
                if (tit == m_replayCtx.agents.end()) continue;
                const auto& trd = tit->second;
                for (size_t j = 1; j < trd.snapshots.size(); ++j) {
                    if (trd.snapshots[j].time > dt + kDeathPactDeathGap) break;
                    if (trd.snapshots[j].is_dead && !trd.snapshots[j - 1].is_dead &&
                        std::abs(trd.snapshots[j].time - dt) < kDeathPactDeathGap) { dpsLinked = true; break; }
                }
                if (dpsLinked) break;
            }
            if (!dpsLinked) times.push_back(dt);
        }
    }

    for (const auto& ev : m_replayCtx.stocData.jumbo) {
        if (ev.message != "MORALE_BOOST") continue;
        if (ev.party_value == 1635021873) m_moraleBoosts[1].push_back(ev.time);
        else if (ev.party_value == 1635021874) m_moraleBoosts[2].push_back(ev.time);
    }

    for (const auto& [aid, ard] : m_replayCtx.agents) {
        if (!CarriesMorale(ard)) continue;
        if (ard.teamId != 1 && ard.teamId != 2) continue;

        enum class MET : uint8_t { Death, Boost, EnemyKill };
        struct ME { float time; MET type; };
        std::vector<ME> events;

        for (float t : m_moraleDeaths[aid]) events.push_back({ t, MET::Death });
        for (float t : m_moraleBoosts[ard.teamId]) events.push_back({ t, MET::Boost });
        for (const auto& [otherId, times] : m_moraleDeaths) {
            auto oit = m_replayCtx.agents.find(otherId);
            if (oit == m_replayCtx.agents.end()) continue;
            if (oit->second.teamId == ard.teamId) continue;
            // Killing a pet is not a kill: only a player death pays the other side.
            if (oit->second.type != AgentType::Player) continue;
            for (float t : times) events.push_back({ t, MET::EnemyKill });
        }
        std::sort(events.begin(), events.end(),
                  [](const ME& a, const ME& b) { return a.time < b.time; });

        auto& steps = m_moraleTimeline[aid];
        steps.push_back({ 0.f, 0 });
        int morale = 0;
        for (const auto& ev : events) {
            const int before = morale;
            switch (ev.type) {
            case MET::Death:     morale = std::max(morale - 15, -60); break;
            case MET::Boost:     morale = std::min(morale + 10, 10);  break;
            case MET::EnemyKill:
                if (morale < 0 && ard.isAliveAtTime(ev.time)) morale = std::min(morale + 2, 0);
                break;
            }
            if (morale != before) steps.push_back({ ev.time, morale });
        }
    }

    m_moraleTimelineBuilt = true;
}

int ReplayWindow::MoralePercentAtTime(const AgentReplayData& ard, float t) const
{
    if (!m_moraleTimelineBuilt) BuildMoraleTimelines();

    auto it = m_moraleTimeline.find(ard.agent_id);
    if (it == m_moraleTimeline.end() || it->second.empty()) return 0;
    const auto& steps = it->second;
    auto s = std::upper_bound(steps.begin(), steps.end(), t,
        [](float v, const std::pair<float, int>& e) { return v < e.first; });
    return s == steps.begin() ? 0 : (--s)->second;
}

// Panel-facing view of the same fold. It used to replay the whole match on every call with its own
// copy of the rules, which is how the two drifted apart: the panel counted a signet backfire as an
// enemy kill while the model did not. There is now one set of rules and one pass over the match.
int ReplayWindow::ComputeAgentMorale(const AgentReplayData& ard, float curTime, int* outDeathCount, int* outBoostCount) const
{
    if (!m_moraleTimelineBuilt) BuildMoraleTimelines();

    if (outDeathCount) {
        int deaths = 0;
        auto it = m_moraleDeaths.find(ard.agent_id);
        if (it != m_moraleDeaths.end())
            for (float t : it->second) if (t <= curTime) ++deaths;
        *outDeathCount = deaths;
    }
    if (outBoostCount) {
        int boosts = 0;
        auto it = m_moraleBoosts.find(static_cast<int>(ard.teamId));
        if (it != m_moraleBoosts.end())
            for (float t : it->second) if (t <= curTime) ++boosts;
        *outBoostCount = boosts;
    }

    return std::clamp(MoralePercentAtTime(ard, curTime), -60, 10);
}


// ---------------------------------------------------------------------------
// Morale Panel
// ---------------------------------------------------------------------------

void ReplayWindow::DrawMoralePanel()
{
    if (!m_showMoralePanel) return;

    const float curTime = m_debugTimeline;
    const auto* vp = ImGui::GetMainViewport();

    struct PlayerMorale {
        int    agentId = 0;
        std::string name;
        int    primaryProf = 0;
        int    secondaryProf = 0;
        int    morale = 0;
        int    deathCount = 0;
        int    boostCount = 0;
        bool   dead = false;
    };

    std::vector<PlayerMorale> redTeam, blueTeam;

    auto buildTeam = [&](const std::vector<int>& ids) {
        std::vector<PlayerMorale> result;
        for (int id : ids) {
            auto it = m_replayCtx.agents.find(id);
            if (it == m_replayCtx.agents.end()) continue;
            const auto& ard = it->second;
            if (ard.type != AgentType::Player) continue;

            PlayerMorale pm;
            pm.agentId = id;
            pm.name = ard.playerName;
            pm.primaryProf = ard.primaryProf;
            pm.secondaryProf = ard.secondaryProf;
            pm.morale = ComputeAgentMorale(ard, curTime, &pm.deathCount, &pm.boostCount);
            pm.dead = ard.isDeadAtTime(curTime);
            result.push_back(std::move(pm));
        }
        return result;
    };

    redTeam  = buildTeam(m_team1PlayerIds);
    blueTeam = buildTeam(m_team2PlayerIds);

    auto getGuildLabel = [&](const std::string& partyId, const std::string& folderTag) -> std::string {
        auto* fg = FindGuildByTagStatic(m_matchMeta, folderTag);
        if (fg) return fg->name + " [" + fg->tag + "]";

        auto pit = m_matchMeta.parties.find(partyId);
        if (pit == m_matchMeta.parties.end()) return "?";
        std::map<int, int> guildCounts;
        for (const auto& p : pit->second.players)
            if (p.guild_id > 0) guildCounts[p.guild_id]++;
        int bestId = 0, bestCnt = 0;
        for (const auto& [gid, cnt] : guildCounts)
            if (cnt > bestCnt) { bestId = gid; bestCnt = cnt; }
        if (bestId == 0) return "?";
        auto git = m_matchMeta.guilds.find(std::to_string(bestId));
        if (git != m_matchMeta.guilds.end())
            return git->second.name + " [" + git->second.tag + "]";
        return "?";
    };

    std::string redLabel  = getGuildLabel("1", m_folderTag1);
    std::string blueLabel = getGuildLabel("2", m_folderTag2);

    constexpr float kPanelW = 520.f;
    constexpr float kRowH = 22.f;
    constexpr float kIconSz = 16.f;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,      ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));

    if (m_panelLayout.HasSavedPosition("morale"))
        m_panelLayout.ApplyPosition("morale");
    else
        ImGui::SetNextWindowPos(
            ImVec2(vp->Pos.x + (vp->Size.x - kPanelW) * 0.5f,
                   vp->Pos.y + vp->Size.y - 300.f),
            ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(kPanelW, 0.f), ImVec2(kPanelW, vp->Size.y));

    if (!ImGui::Begin("Morale##morale_panel", &m_showMoralePanel,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize))
    {
        m_panelLayout.TrackWindow("morale");
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);
        return;
    }

    m_panelLayout.TrackWindow("morale");

    {
        ImVec2 wPos = ImGui::GetWindowPos();
        ImVec2 wSz  = ImGui::GetWindowSize();
        float cx = std::clamp(wPos.x, vp->Pos.x, vp->Pos.x + vp->Size.x - wSz.x);
        float cy = std::clamp(wPos.y, vp->Pos.y, vp->Pos.y + vp->Size.y - wSz.y);
        if (cx != wPos.x || cy != wPos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fontSize = ImGui::GetFontSize();
    ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;

    constexpr ImU32 kBlueTeam = IM_COL32(0x4A, 0xC8, 0xFF, 0xFF);
    constexpr ImU32 kRedTeam  = IM_COL32(0xFF, 0x6B, 0x6B, 0xFF);
    constexpr ImU32 kGreen    = IM_COL32(0x40, 0xE0, 0x80, 0xFF);
    constexpr ImU32 kMuted    = IM_COL32(0x70, 0x7D, 0x88, 0xFF);
    constexpr ImU32 kAmber    = IM_COL32(0xFF, 0xA0, 0x30, 0xFF);
    constexpr ImU32 kRed      = IM_COL32(0xFF, 0x50, 0x50, 0xFF);
    constexpr ImU32 kText     = IM_COL32(0xE8, 0xEC, 0xF2, 0xFF);
    constexpr ImU32 kTextDead = IM_COL32(0x60, 0x60, 0x60, 0xFF);

    auto moraleColor = [&](int m) -> ImU32 {
        if (m > 0)    return kGreen;
        if (m == 0)   return kMuted;
        if (m >= -15) return kAmber;
        if (m >= -30) return IM_COL32(0xFF, 0x80, 0x30, 0xFF);
        return kRed;
    };

    auto avgColor = [&](float avg) -> ImU32 {
        if (avg > 0.f)    return kGreen;
        if (avg >= -5.f)  return kMuted;
        if (avg >= -15.f) return kAmber;
        return kRed;
    };

    const float contentW = kPanelW - 20.f;
    const float colW = (contentW - 1.f) * 0.5f;
    const float startX = ImGui::GetCursorScreenPos().x;
    const float startY = ImGui::GetCursorScreenPos().y;

    size_t maxRows = std::max(blueTeam.size(), redTeam.size());

    // Column headers: guild name [tag] in team color, standard font
    {
        ImVec2 p(startX, startY);
        dl->AddText(ImVec2(p.x + 2.f, p.y), kRedTeam, redLabel.c_str());
        dl->AddText(ImVec2(p.x + colW + 1.f + 2.f, p.y), kBlueTeam, blueLabel.c_str());
        ImGui::Dummy(ImVec2(0.f, fontSize + 4.f));
    }

    float rowStartY = ImGui::GetCursorScreenPos().y;

    auto drawPlayerRow = [&](const PlayerMorale& pm, float x, float y) {
        float cx = x;

        // Primary profession icon
        if (dev && pm.primaryProf >= 1) {
            ImTextureID tex = LoadProfIcon(dev, pm.primaryProf);
            if (tex) {
                float iy = y + (kRowH - kIconSz) * 0.5f;
                dl->AddImage(tex, ImVec2(cx, iy), ImVec2(cx + kIconSz, iy + kIconSz));
            }
        }
        cx += kIconSz + 1.f;

        // Secondary profession icon
        if (dev && pm.secondaryProf >= 1) {
            ImTextureID tex = LoadProfIcon(dev, pm.secondaryProf);
            if (tex) {
                float iy = y + (kRowH - kIconSz) * 0.5f;
                dl->AddImage(tex, ImVec2(cx, iy), ImVec2(cx + kIconSz, iy + kIconSz));
            }
        }
        cx += kIconSz + 3.f;

        // Player name in standard white/grey
        ImU32 nameCol = pm.dead ? kTextDead : kText;
        dl->AddText(ImVec2(cx, y + (kRowH - fontSize) * 0.5f), nameCol, pm.name.c_str());

        // Right side: morale value + dots or star
        float rightEdge = x + colW;
        char valBuf[16];

        if (pm.morale > 0) {
            snprintf(valBuf, sizeof(valBuf), "+%d%%", pm.morale);
            ImVec2 valSz = ImGui::CalcTextSize(valBuf);
            int stars = std::min(4, pm.boostCount);
            ImVec2 starSz = ImGui::CalcTextSize("\xe2\x98\x85");
            float starSpacing = starSz.x + 1.f;
            float starsWidth = stars > 0 ? (stars * starSpacing) : 0.f;
            float totalW = starsWidth + 2.f + valSz.x + 2.f;
            float vx = rightEdge - totalW;
            float ty = y + (kRowH - fontSize) * 0.5f;
            for (int s = 0; s < stars; ++s)
                dl->AddText(ImVec2(vx + s * starSpacing, ty), kGreen, "\xe2\x98\x85");
            dl->AddText(ImVec2(rightEdge - valSz.x - 2.f, ty), kGreen, valBuf);
        }
        else if (pm.morale == 0) {
            ImVec2 zSz = ImGui::CalcTextSize("0%");
            dl->AddText(ImVec2(rightEdge - zSz.x - 2.f, y + (kRowH - fontSize) * 0.5f), kMuted, "0%");
        }
        else {
            snprintf(valBuf, sizeof(valBuf), "%d%%", pm.morale);
            ImU32 valCol = moraleColor(pm.morale);
            ImVec2 valSz = ImGui::CalcTextSize(valBuf);

            int dots = std::min(4, pm.deathCount);
            float dotSpacing = 10.f;
            float dotsWidth = dots > 0 ? (dots * dotSpacing) : 0.f;

            float totalW = valSz.x + 4.f + dotsWidth + 2.f;
            float vx = rightEdge - totalW;

            dl->AddText(ImVec2(vx, y + (kRowH - fontSize) * 0.5f), valCol, valBuf);
            vx += valSz.x + 4.f;

            for (int d = 0; d < dots; ++d) {
                dl->AddCircleFilled(
                    ImVec2(vx + d * dotSpacing + 3.f, y + kRowH * 0.5f),
                    3.f, valCol);
            }
        }
    };

    for (size_t i = 0; i < maxRows; ++i) {
        float rowY = rowStartY + i * kRowH;
        if (i < redTeam.size())
            drawPlayerRow(redTeam[i], startX, rowY);
        if (i < blueTeam.size())
            drawPlayerRow(blueTeam[i], startX + colW + 1.f, rowY);
    }

    float divX = startX + colW;
    float divTop = rowStartY;
    float divBot = rowStartY + maxRows * kRowH;
    dl->AddLine(ImVec2(divX, divTop), ImVec2(divX, divBot),
                IM_COL32(0xFF, 0xD7, 0x64, 0x30), 1.f);

    ImGui::Dummy(ImVec2(0.f, maxRows * kRowH + 4.f));

    // Footer divider
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + contentW, p.y),
                    IM_COL32(0xFF, 0xD7, 0x64, 0x20), 1.f);
        ImGui::Dummy(ImVec2(0.f, 4.f));
    }

    auto computeAvg = [](const std::vector<PlayerMorale>& team) -> float {
        if (team.empty()) return 0.f;
        float sum = 0.f;
        for (auto& pm : team) sum += static_cast<float>(pm.morale);
        return sum / static_cast<float>(team.size());
    };

    float blueAvg = computeAvg(blueTeam);
    float redAvg  = computeAvg(redTeam);

    // Footer: team averages
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        char buf[32];

        snprintf(buf, sizeof(buf), "Avg  %.0f%%", redAvg);
        dl->AddText(ImVec2(p.x + 2.f, p.y), avgColor(redAvg), buf);

        snprintf(buf, sizeof(buf), "Avg  %.0f%%", blueAvg);
        dl->AddText(ImVec2(p.x + colW + 1.f + 2.f, p.y), avgColor(blueAvg), buf);

        ImGui::Dummy(ImVec2(0.f, fontSize + 4.f));
    }

    // Bottom bar
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float barW = contentW;
        float barH = 8.f;
        float halfW = barW * 0.5f;

        dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + barW, p.y + barH),
                          IM_COL32(255, 255, 255, 15), 3.f);

        float redLen = std::min(1.f, std::abs(redAvg) / 60.f) * halfW;
        if (redLen > 1.f) {
            dl->AddRectFilled(
                ImVec2(p.x + halfW - redLen, p.y),
                ImVec2(p.x + halfW, p.y + barH),
                IM_COL32(0xFF, 0x6B, 0x6B, 0xB3), 2.f);
        }

        float blueLen = std::min(1.f, std::abs(blueAvg) / 60.f) * halfW;
        if (blueLen > 1.f) {
            dl->AddRectFilled(
                ImVec2(p.x + halfW, p.y),
                ImVec2(p.x + halfW + blueLen, p.y + barH),
                IM_COL32(0x4A, 0xC8, 0xFF, 0xB3), 2.f);
        }

        dl->AddLine(ImVec2(p.x + halfW, p.y), ImVec2(p.x + halfW, p.y + barH),
                    IM_COL32(0xFF, 0xD7, 0x64, 0x60), 1.f);

        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f%%", blueAvg);
        dl->AddText(ImVec2(p.x + 2.f, p.y + barH + 2.f), avgColor(blueAvg), buf);

        snprintf(buf, sizeof(buf), "%.0f%%", redAvg);
        ImVec2 rSz = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(p.x + barW - rSz.x - 2.f, p.y + barH + 2.f), avgColor(redAvg), buf);

        ImGui::Dummy(ImVec2(0.f, barH + fontSize + 4.f));
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}


void ReplayWindow::DrawMoraleBoostTimers()
{
    auto& standEvents = m_flagTimeline.stand.events;
    if (standEvents.empty() && m_draggingUIElement != 1 && m_draggingUIElement != 2)
        return;

    float curTime = m_debugTimeline;

    float lastCapTime[2] = { -1.f, -1.f };
    int   lastCapTeam = -1;

    for (auto& sc : standEvents)
    {
        if (sc.time > curTime) break;
        if (sc.owner == StandOwner::Red)        { lastCapTime[0] = sc.time; lastCapTeam = 0; }
        else if (sc.owner == StandOwner::Blue)     { lastCapTime[1] = sc.time; lastCapTeam = 1; }
        else if (sc.owner == StandOwner::Neutral) { lastCapTeam = -1; }
    }

    ImFont* font = m_latoBold ? m_latoBold : ImGui::GetFont();
    float fontSize = font->FontSize;
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    ImU32 shA     = IM_COL32(0, 0, 0, 204);
    ImU32 shB     = IM_COL32(0, 0, 0, 230);

    auto drawShadowed = [&](ImVec2 pos, ImU32 col, const char* txt)
    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddText(font, fontSize, ImVec2(pos.x, pos.y + 2), shA, txt);
        dl->AddText(font, fontSize, ImVec2(pos.x, pos.y + 1), shB, txt);
        dl->AddText(font, fontSize, pos, col, txt);
    };

    auto DrawOneMorale = [&](int teamIdx01, int dragIdx, float* fracX, float* fracY,
                             float defaultX, float defaultY)
    {
        int team = teamIdx01 + 1;
        float px = m_uiLayout.useCustom ? *fracX : defaultX;
        float py = m_uiLayout.useCustom ? *fracY : defaultY;

        bool hasCap = (lastCapTeam >= 0 && lastCapTeam == teamIdx01);

        if (!hasCap && m_draggingUIElement != dragIdx)
            return;

        const char* teamLabel = (team == 1) ? "Red Morale Boost" : "Blue Morale Boost";
        char buf[32] = "02:00";

        if (hasCap)
        {
            float secondsSince = curTime - lastCapTime[teamIdx01];
            if (secondsSince < 0.f && m_draggingUIElement != dragIdx) return;
            int cyclePos = static_cast<int>(floorf(std::max(0.f, secondsSince))) % 120;
            int remaining = 120 - cyclePos;
            FormatMMSS(buf, sizeof(buf), static_cast<float>(remaining));
        }

        ImVec2 labelSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, teamLabel);
        ImVec2 timerSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, buf);
        float blockW = std::max(labelSize.x, timerSize.x);
        float lineH = fontSize + 4.f;

        float anchorX = vp->Pos.x + vp->Size.x * px;
        float anchorY = vp->Pos.y + vp->Size.y * py;
        float x = anchorX - blockW * 0.5f;

        ImU32 teamCol = (team == 1) ? IM_COL32(0xFF, 0x99, 0x9A, 0xFF)
                                    : IM_COL32(0x99, 0xCB, 0xFD, 0xFF);
        ImU32 goldCol = IM_COL32(0xF5, 0xE4, 0xB4, 0xFF);

        float labelX = x + (blockW - labelSize.x) * 0.5f;
        float timerX = x + (blockW - timerSize.x) * 0.5f;

        drawShadowed(ImVec2(labelX, anchorY), teamCol, teamLabel);
        drawShadowed(ImVec2(timerX, anchorY + lineH), goldCol, buf);

        ImVec2 boxTL(x - 2, anchorY - 2);
        ImVec2 boxBR(x + blockW + 2, anchorY + lineH * 2 + 2);
        HandleOverlayDrag(dragIdx, fracX, fracY, boxTL, boxBR);
    };

    DrawOneMorale(0, 1, &m_uiLayout.moRedX,  &m_uiLayout.moRedY,  0.35f, 0.22f);
    DrawOneMorale(1, 2, &m_uiLayout.moBlueX, &m_uiLayout.moBlueY, 0.65f, 0.22f);
}
