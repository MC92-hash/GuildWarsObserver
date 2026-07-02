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
// Skill Analytics: build per-skill stats for all players
// ---------------------------------------------------------------------------
std::vector<ReplayWindow::PlayerAnalytics> ReplayWindow::BuildAllPlayerAnalytics(float currentTime) const
{
    std::vector<PlayerAnalytics> out;
    if (!m_agentsClassified || !m_combatLogBuilt) return out;

    const auto& sdb = m_skillView;

    // Pre-index combat log rows by caster for O(N) total scan
    std::unordered_map<int, std::vector<size_t>> combatByCaster;
    for (size_t i = 0; i < m_combatLog.size(); ++i)
    {
        const auto& row = m_combatLog[i];
        if (row.time > currentTime) continue;
        if (row.category != CombatLogCategory::Damage &&
            row.category != CombatLogCategory::Heal)
            continue;
        combatByCaster[row.casterId].push_back(i);
    }

    for (int agentId : m_playerIds)
    {
        auto ait = m_replayCtx.agents.find(agentId);
        if (ait == m_replayCtx.agents.end()) continue;
        const auto& ard = ait->second;
        if (ard.type != AgentType::Player) continue;

        PlayerAnalytics pa;
        pa.agentId       = agentId;
        pa.playerName    = ard.partyBarLabel.empty() ? ard.playerName : ard.partyBarLabel;
        pa.teamId        = ard.teamId;
        pa.primaryProf   = ard.primaryProf;
        pa.secondaryProf = ard.secondaryProf;
        pa.playerNumber  = ard.playerNumber;

        // --- Accumulate casts from skillUseHistory ---
        struct SkillAccum {
            int totalCasts   = 0;
            int cancelled    = 0;
            int interrupted  = 0;
            float firstUse   = 1e9f;
            std::unordered_map<int, int> targetCasts;
        };
        std::unordered_map<int, SkillAccum> accum;

        for (const auto& ev : ard.skillUseHistory)
        {
            if (ev.startTime > currentTime) break;
            int resolved = sdb.ResolvePvpSkillId(ev.skillId);
            auto& a = accum[resolved];
            a.totalCasts++;
            if (ev.wasCancelled)   a.cancelled++;
            if (ev.wasInterrupted) a.interrupted++;
            if (ev.startTime < a.firstUse) a.firstUse = ev.startTime;
            int tgt = ev.targetId >= 0 ? ev.targetId : agentId;
            a.targetCasts[tgt]++;
        }

        // --- Accumulate damage/healing from combat log ---
        struct DmgHealAccum { int damage = 0; int healing = 0; };
        std::unordered_map<int, DmgHealAccum> skillDmgTotal;
        // skillId -> targetId -> DmgHealAccum
        std::unordered_map<int, std::unordered_map<int, DmgHealAccum>> skillTargetDmg;

        auto cit = combatByCaster.find(agentId);
        if (cit != combatByCaster.end())
        {
            for (size_t idx : cit->second)
            {
                const auto& row = m_combatLog[idx];
                int resolved = (row.skillId > 0) ? sdb.ResolvePvpSkillId(row.skillId) : 0;
                int absVal = std::abs(row.valueAbs);
                if (row.category == CombatLogCategory::Damage)
                {
                    skillDmgTotal[resolved].damage += absVal;
                    skillTargetDmg[resolved][row.targetId].damage += absVal;
                    pa.totalDamage += absVal;
                }
                else if (row.category == CombatLogCategory::Heal)
                {
                    skillDmgTotal[resolved].healing += absVal;
                    skillTargetDmg[resolved][row.targetId].healing += absVal;
                    pa.totalHealing += absVal;
                }
            }
        }

        // --- Order skills using same logic as player info panel ---
        const std::vector<int>* usedSkills = nullptr;
        const PlayerMeta* playerMeta = nullptr;
        for (const auto& [pid, party] : m_matchMeta.parties)
        {
            for (const auto& pm : party.players)
            {
                if (pm.id == agentId && !pm.used_skills.empty())
                { usedSkills = &pm.used_skills; playerMeta = &pm; break; }
            }
            if (usedSkills) break;
            for (const auto& pm : party.others)
            {
                if (pm.id == agentId && !pm.used_skills.empty())
                { usedSkills = &pm.used_skills; playerMeta = &pm; break; }
            }
            if (usedSkills) break;
        }

        std::vector<int> orderedSkillIds;
        std::unordered_set<int> placed;

        if (usedSkills)
        {
            for (int sid : *usedSkills)
            {
                int resolved = sdb.ResolvePvpSkillId(sid);
                if (!placed.insert(resolved).second) continue;
                orderedSkillIds.push_back(resolved);
            }
        }

        // Append extras from history ordered by first use
        struct ExtraSkill { int id; float firstUse; };
        std::vector<ExtraSkill> extras;
        for (auto& [id, a] : accum)
            if (placed.find(id) == placed.end())
                extras.push_back({ id, a.firstUse });
        std::sort(extras.begin(), extras.end(),
            [](const ExtraSkill& a, const ExtraSkill& b) { return a.firstUse < b.firstUse; });
        for (auto& e : extras)
        {
            placed.insert(e.id);
            orderedSkillIds.push_back(e.id);
        }

        // Apply profession-aware sort
        if (playerMeta && orderedSkillIds.size() > 1)
        {
            orderedSkillIds = sdb.SortSkillsForDisplay(
                orderedSkillIds, playerMeta->primary, playerMeta->secondary);
        }

        // --- Build SkillAnalyticsStat for each ordered skill ---
        for (int skillId : orderedSkillIds)
        {
            SkillAnalyticsStat stat;
            stat.skillId = skillId;

            auto ait2 = accum.find(skillId);
            if (ait2 != accum.end())
            {
                stat.totalCasts  = ait2->second.totalCasts;
                stat.cancelled   = ait2->second.cancelled;
                stat.interrupted = ait2->second.interrupted;
            }

            auto dit = skillDmgTotal.find(skillId);
            if (dit != skillDmgTotal.end())
            {
                stat.totalDamage  = dit->second.damage;
                stat.totalHealing = dit->second.healing;
            }

            // Per-target breakdown: merge cast counts + damage/healing
            std::unordered_set<int> allTargets;
            if (ait2 != accum.end())
                for (auto& [tid, _] : ait2->second.targetCasts) allTargets.insert(tid);
            auto stdit = skillTargetDmg.find(skillId);
            if (stdit != skillTargetDmg.end())
                for (auto& [tid, _] : stdit->second) allTargets.insert(tid);

            for (int tid : allTargets)
            {
                SkillAnalyticsStat::TargetBreakdown tb;
                tb.targetId = tid;

                auto tit = m_replayCtx.agents.find(tid);
                if (tit != m_replayCtx.agents.end())
                {
                    tb.name        = tit->second.partyBarLabel.empty()
                                   ? tit->second.playerName : tit->second.partyBarLabel;
                    tb.teamId      = tit->second.teamId;
                    tb.primaryProf = tit->second.primaryProf;
                }
                else
                {
                    tb.name = (tid == agentId) ? "Self" : ("Agent " + std::to_string(tid));
                }

                if (ait2 != accum.end())
                {
                    auto tcit = ait2->second.targetCasts.find(tid);
                    if (tcit != ait2->second.targetCasts.end())
                    {
                        tb.castCount = tcit->second;
                        tb.castPct   = (stat.totalCasts > 0)
                            ? (float)tcit->second / (float)stat.totalCasts : 0.f;
                    }
                }

                if (stdit != skillTargetDmg.end())
                {
                    auto tdmg = stdit->second.find(tid);
                    if (tdmg != stdit->second.end())
                    {
                        tb.damage  = tdmg->second.damage;
                        tb.healing = tdmg->second.healing;
                    }
                }

                stat.targets.push_back(std::move(tb));
            }

            std::sort(stat.targets.begin(), stat.targets.end(),
                [](const auto& a, const auto& b) { return a.castPct > b.castPct; });

            pa.skills.push_back(std::move(stat));
        }

        out.push_back(std::move(pa));
    }

    // Sort: team 1 first, then team 2; within team, by ascending player number
    std::sort(out.begin(), out.end(), [](const PlayerAnalytics& a, const PlayerAnalytics& b) {
        if (a.teamId != b.teamId) return a.teamId < b.teamId;
        if (a.playerNumber != b.playerNumber) return a.playerNumber < b.playerNumber;
        return a.agentId < b.agentId;
    });

    return out;
}


// ---------------------------------------------------------------------------
// Skill Analytics Panel
// ---------------------------------------------------------------------------
void ReplayWindow::DrawSkillAnalyticsPanel()
{
    if (!m_showSkillAnalytics) return;
    if (!m_agentsClassified || !m_combatLogBuilt) return;

    // Rebuild cache when timeline changes
    if (std::abs(m_debugTimeline - m_analyticsCacheTime) > 0.01f || m_analyticsCache.empty())
    {
        m_analyticsCache = BuildAllPlayerAnalytics(m_debugTimeline);
        m_analyticsCacheTime = m_debugTimeline;
    }

    ImGuiIO& io = ImGui::GetIO();
    float vpW = io.DisplaySize.x;
    float vpH = io.DisplaySize.y;

    // Two columns of kMaxCol icons each, plus gap and padding
    constexpr int   kMaxColInit = 12;
    constexpr float kInitW = 2 * (kMaxColInit * (28.f + 3.f) + 16.f + 8.f) + 12.f + 24.f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.f, 250.f), ImVec2(vpW, vpH));
    if (m_panelLayout.HasSavedSize("skill_analytics"))
        m_panelLayout.ApplySize("skill_analytics");
    else
        ImGui::SetNextWindowSize(ImVec2(std::min(kInitW, vpW), std::min(vpH * 0.85f, vpH)), ImGuiCond_FirstUseEver);
    m_panelLayout.ApplyPosition("skill_analytics");

    // Gold-accented dark panel styling (matches Combat Log / Morale)
    ImGui::PushStyleColor(ImGuiCol_WindowBg,             ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,              ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,        ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,               ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,            ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          ImVec4(1.f, 1.f, 1.f, 0.04f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        ImVec4(0.80f, 0.68f, 0.30f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1.f, 0.84f, 0.39f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  ImVec4(1.f, 0.84f, 0.39f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));

    if (!ImGui::Begin("Skill Analytics (BETA)", &m_showSkillAnalytics))
    {
        m_panelLayout.TrackWindow("skill_analytics");
        ImGui::End();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(9);
        return;
    }

    m_panelLayout.TrackWindow("skill_analytics");

    // Clamp window to viewport
    {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, 0.f, std::max(0.f, vpW - sz.x));
        float cy = std::clamp(pos.y, 0.f, std::max(0.f, vpH - sz.y));
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));
    }

    EnsureSkillIconIndex();
    auto* dev = m_deviceResources->GetD3DDevice();

    auto FmtNum = [](int v) -> std::string {
        if (v < 0) v = 0;
        std::string raw = std::to_string(v);
        std::string result;
        int count = 0;
        for (int i = (int)raw.size() - 1; i >= 0; --i)
        {
            if (count > 0 && count % 3 == 0) result.insert(result.begin(), ',');
            result.insert(result.begin(), raw[i]);
            ++count;
        }
        return result;
    };

    auto ProfShort = [](int id) -> const char* {
        switch (id) {
        case 1: return "W"; case 2: return "R"; case 3: return "Mo";
        case 4: return "N"; case 5: return "Me"; case 6: return "E";
        case 7: return "A"; case 8: return "Rt"; case 9: return "P";
        case 10: return "D"; default: return "?";
        }
    };

    constexpr ImU32 kBlueTeam = IM_COL32(0x4A, 0xC8, 0xFF, 0xFF);
    constexpr ImU32 kRedTeam  = IM_COL32(0xFF, 0x6B, 0x6B, 0xFF);
    constexpr ImU32 kText     = IM_COL32(0xE8, 0xEC, 0xF2, 0xFF);
    constexpr ImU32 kDmgRed   = IM_COL32(0xFF, 0x80, 0x80, 0xFF);
    constexpr ImU32 kHealGrn  = IM_COL32(0x40, 0xE0, 0x80, 0xFF);
    constexpr ImU32 kMuted    = IM_COL32(0x70, 0x7D, 0x88, 0xFF);

    auto TeamColor = [](uint8_t teamId) -> ImU32 {
        switch (teamId) {
        case 1:  return IM_COL32(0xFF, 0x6B, 0x6B, 0xFF);
        case 2:  return IM_COL32(0x4A, 0xC8, 0xFF, 0xFF);
        default: return IM_COL32(0xAA, 0xAA, 0xAA, 0xFF);
        }
    };

    // --- Filter bar: team checkboxes + profession icon toggles ---
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kRedTeam);
        ImGui::Checkbox("Red", &m_analyticsShowTeam[0]);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kBlueTeam);
        ImGui::Checkbox("Blue", &m_analyticsShowTeam[1]);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        // Profession icon toggles
        float iconSz = ImGui::GetTextLineHeight() + 2.f;
        for (int p = 1; p <= 10; ++p)
        {
            ImGui::PushID(p);
            ImTextureID profTex = LoadProfIcon(dev, p);
            bool active = m_analyticsProfFilter[p - 1];
            if (!active) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.30f);
            if (profTex)
            {
                if (ImGui::ImageButton("##pf", profTex, ImVec2(iconSz, iconSz)))
                    m_analyticsProfFilter[p - 1] = !m_analyticsProfFilter[p - 1];
            }
            else
            {
                if (ImGui::SmallButton(ProfShort(p)))
                    m_analyticsProfFilter[p - 1] = !m_analyticsProfFilter[p - 1];
            }
            if (!active) ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(ProfShort(p));
                ImGui::EndTooltip();
            }
            ImGui::PopID();
            ImGui::SameLine(0.f, 2.f);
        }

        // All / None toggle
        {
            bool allOn = true;
            for (int i = 0; i < 10; ++i) if (!m_analyticsProfFilter[i]) { allOn = false; break; }
            if (ImGui::SmallButton(allOn ? "None" : "All"))
            {
                bool newVal = !allOn;
                for (int i = 0; i < 10; ++i) m_analyticsProfFilter[i] = newVal;
            }
        }
    }

    ImGui::Separator();

    // --- Filter logic: build per-team player lists ---
    auto PassesFilter = [&](const PlayerAnalytics& pa) -> bool {
        if (pa.teamId == 1 && !m_analyticsShowTeam[0]) return false;
        if (pa.teamId == 2 && !m_analyticsShowTeam[1]) return false;
        if (pa.primaryProf >= 1 && pa.primaryProf <= 10 && !m_analyticsProfFilter[pa.primaryProf - 1])
            return false;
        return true;
    };

    std::vector<const PlayerAnalytics*> team1, team2;
    for (const auto& pa : m_analyticsCache)
    {
        if (!PassesFilter(pa)) continue;
        if (pa.teamId == 1) team1.push_back(&pa);
        else                 team2.push_back(&pa);
    }

    // --- Find max damage/healing across visible skills for bar scaling ---
    int globalMaxDmg = 1, globalMaxHeal = 1;
    for (const auto& pa : m_analyticsCache)
    {
        if (!PassesFilter(pa)) continue;
        for (const auto& sk : pa.skills)
        {
            if (sk.totalDamage  > globalMaxDmg)  globalMaxDmg  = sk.totalDamage;
            if (sk.totalHealing > globalMaxHeal) globalMaxHeal = sk.totalHealing;
        }
    }

    // --- Player card drawing lambda ---
    constexpr float kSkIco  = 28.f;
    constexpr float kSkGap  = 3.f;
    constexpr int   kMaxCol = 12;
    constexpr float kBarH   = 3.f;
    constexpr float kProfIco = 14.f;

    constexpr float kCardW = kMaxCol * (kSkIco + kSkGap) + 16.f;

    auto DrawPlayerCard = [&](const PlayerAnalytics& pa)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Header: prof icon + name, then D/H on second line
        {
            ImVec2 cur = ImGui::GetCursorScreenPos();
            float lineH = ImGui::GetTextLineHeight();

            ImTextureID profTex = (pa.primaryProf >= 1) ? LoadProfIcon(dev, pa.primaryProf) : nullptr;
            float cx = cur.x;
            if (profTex)
            {
                float iy = cur.y + (lineH - kProfIco) * 0.5f;
                dl->AddImage(profTex, ImVec2(cx, iy), ImVec2(cx + kProfIco, iy + kProfIco));
            }
            cx += kProfIco + 2.f;

            ImU32 nameCol = TeamColor(pa.teamId);
            dl->AddText(ImVec2(cx, cur.y), nameCol, pa.playerName.c_str());

            // D/H right of name
            char dmgBuf[32], healBuf[32];
            snprintf(dmgBuf, sizeof(dmgBuf), "D:%s", FmtNum(pa.totalDamage).c_str());
            snprintf(healBuf, sizeof(healBuf), "H:%s", FmtNum(pa.totalHealing).c_str());
            ImVec2 dmgSz = ImGui::CalcTextSize(dmgBuf);
            ImVec2 healSz = ImGui::CalcTextSize(healBuf);
            float rx = cur.x + kCardW - dmgSz.x - 6.f - healSz.x - 4.f;
            dl->AddText(ImVec2(rx, cur.y), kDmgRed, dmgBuf);
            dl->AddText(ImVec2(rx + dmgSz.x + 6.f, cur.y), kHealGrn, healBuf);

            ImGui::Dummy(ImVec2(kCardW, lineH + 2.f));
        }

        // Skill icons row
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        int numSkills = std::min((int)pa.skills.size(), kMaxCol * 2);

        for (int i = 0; i < numSkills; ++i)
        {
            int col = i % kMaxCol;
            int row = i / kMaxCol;
            const auto& sk = pa.skills[i];

            float x = cursor.x + col * (kSkIco + kSkGap);
            float y = cursor.y + row * (kSkIco + 14.f + kBarH * 2 + 4.f);

            ImTextureID iconTex = LoadSkillIcon(this, dev, sk.skillId,
                m_skillIconIndex, m_skillIconCache);

            ImVec2 iconTL(x, y);
            ImVec2 iconBR(x + kSkIco, y + kSkIco);

            if (iconTex)
                dl->AddImage(iconTex, iconTL, iconBR);
            else
                dl->AddRectFilled(iconTL, iconBR, IM_COL32(40, 40, 40, 255));

            // Click to open player detail sub-panel
            if (ImGui::IsMouseHoveringRect(iconTL, iconBR) && ImGui::IsMouseClicked(0))
                m_analyticsOpenPlayers.insert(pa.agentId);

            // Highlight all icons if player sub-panel is open
            if (m_analyticsOpenPlayers.count(pa.agentId))
                dl->AddRect(iconTL, iconBR, IM_COL32(255, 200, 60, 180), 0.f, 0, 1.5f);

            // Hover tooltip
            if (ImGui::IsMouseHoveringRect(iconTL, iconBR))
            {
                ImGui::BeginTooltip();
                std::string sn = GetSkillDisplayName(sk.skillId);
                ImGui::Text("%s", sn.c_str());
                ImGui::Text("Casts: %d  Dmg: %s  Heal: %s",
                    sk.totalCasts, FmtNum(sk.totalDamage).c_str(), FmtNum(sk.totalHealing).c_str());
                if (sk.cancelled > 0 || sk.interrupted > 0)
                    ImGui::Text("Cancelled: %d  Interrupted: %d", sk.cancelled, sk.interrupted);
                ImGui::EndTooltip();
            }

            // Cast count below icon
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", sk.totalCasts);
                ImVec2 tsz = ImGui::CalcTextSize(buf);
                float tx = x + (kSkIco - tsz.x) * 0.5f;
                float ty = y + kSkIco + 1.f;
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f,
                    ImVec2(tx, ty), kText, buf);
            }

            // Mini damage/heal bars
            {
                float barY = y + kSkIco + 11.f;
                float barW = kSkIco;

                if (sk.totalDamage > 0)
                {
                    float frac = (float)sk.totalDamage / (float)globalMaxDmg;
                    dl->AddRectFilled(ImVec2(x, barY),
                        ImVec2(x + barW * frac, barY + kBarH),
                        IM_COL32(220, 60, 60, 200));
                }
                if (sk.totalHealing > 0)
                {
                    float frac = (float)sk.totalHealing / (float)globalMaxHeal;
                    dl->AddRectFilled(ImVec2(x, barY + kBarH + 1.f),
                        ImVec2(x + barW * frac, barY + kBarH * 2 + 1.f),
                        IM_COL32(60, 200, 60, 200));
                }
            }
        }

        int numRows = (numSkills + kMaxCol - 1) / kMaxCol;
        float totalH = numRows * (kSkIco + 14.f + kBarH * 2 + 4.f);
        ImGui::Dummy(ImVec2(kCardW, totalH));
    };

    // --- Two-column layout (fixed column width) ---
    constexpr float kColGap = 12.f;
    float colW = kCardW + 8.f;

    ImGui::BeginChild("##SAScroll", ImVec2(0, 0), false);
    {
        // Column headers
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 hPos = ImGui::GetCursorScreenPos();

        // Find guild names for headers
        auto getGuildLabel = [&](const char* partyKey, const std::string& folderTag) -> std::string {
            auto* fg = FindGuildByTagStatic(m_matchMeta, folderTag);
            if (fg) return fg->name + " [" + fg->tag + "]";

            auto pit = m_matchMeta.parties.find(partyKey);
            if (pit == m_matchMeta.parties.end()) return "?";
            std::map<int,int> guildCounts;
            for (const auto& p : pit->second.players)
                if (p.guild_id > 0) guildCounts[p.guild_id]++;
            int bestId = 0, bestCnt = 0;
            for (const auto& [gid, cnt] : guildCounts)
                if (cnt > bestCnt) { bestId = gid; bestCnt = cnt; }
            if (bestId == 0) return "Team";
            auto git = m_matchMeta.guilds.find(std::to_string(bestId));
            if (git != m_matchMeta.guilds.end())
                return git->second.name + " [" + git->second.tag + "]";
            return "Team";
        };

        std::string redLabel  = getGuildLabel("1", m_folderTag1);
        std::string blueLabel = getGuildLabel("2", m_folderTag2);

        dl->AddText(ImVec2(hPos.x + 2.f, hPos.y), kRedTeam, redLabel.c_str());
        dl->AddText(ImVec2(hPos.x + colW + kColGap + 2.f, hPos.y), kBlueTeam, blueLabel.c_str());
        ImGui::Dummy(ImVec2(0.f, ImGui::GetTextLineHeight() + 4.f));

        // Left column (Team 1)
        ImGui::BeginChild("##SALeft", ImVec2(colW, 0), false);
        for (const auto* pa : team1)
        {
            ImGui::PushID(pa->agentId);
            DrawPlayerCard(*pa);
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::SameLine(0.f, kColGap);

        // Right column (Team 2)
        ImGui::BeginChild("##SARight", ImVec2(colW, 0), false);
        for (const auto* pa : team2)
        {
            ImGui::PushID(pa->agentId);
            DrawPlayerCard(*pa);
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(9);

}


// ---------------------------------------------------------------------------
// Skill Analytics: per-player detail sub-panels (independent of main panel)
// ---------------------------------------------------------------------------
void ReplayWindow::DrawSkillAnalyticsPlayerPopups()
{
    if (m_analyticsOpenPlayers.empty()) return;
    if (!m_agentsClassified || !m_combatLogBuilt) return;

    // Ensure cache is up to date even when main panel is closed
    if (std::abs(m_debugTimeline - m_analyticsCacheTime) > 0.01f || m_analyticsCache.empty())
    {
        m_analyticsCache = BuildAllPlayerAnalytics(m_debugTimeline);
        m_analyticsCacheTime = m_debugTimeline;
    }
    if (m_analyticsCache.empty()) return;

    EnsureSkillIconIndex();
    auto* dev = m_deviceResources->GetD3DDevice();

    ImGuiIO& io = ImGui::GetIO();
    float vpW = io.DisplaySize.x;
    float vpH = io.DisplaySize.y;

    auto FmtNum = [](int v) -> std::string {
        if (v < 0) v = 0;
        std::string raw = std::to_string(v);
        std::string result;
        int count = 0;
        for (int i = (int)raw.size() - 1; i >= 0; --i)
        {
            if (count > 0 && count % 3 == 0) result.insert(result.begin(), ',');
            result.insert(result.begin(), raw[i]);
            ++count;
        }
        return result;
    };

    constexpr ImU32 kDmgRed  = IM_COL32(0xFF, 0x80, 0x80, 0xFF);
    constexpr ImU32 kHealGrn = IM_COL32(0x40, 0xE0, 0x80, 0xFF);
    constexpr ImU32 kMuted   = IM_COL32(0x70, 0x7D, 0x88, 0xFF);

    auto TeamColor = [](uint8_t teamId) -> ImU32 {
        switch (teamId) {
        case 1:  return IM_COL32(0xFF, 0x6B, 0x6B, 0xFF);
        case 2:  return IM_COL32(0x4A, 0xC8, 0xFF, 0xFF);
        default: return IM_COL32(0xAA, 0xAA, 0xAA, 0xFF);
        }
    };

    std::vector<int> toClose;

    for (int agentId : m_analyticsOpenPlayers)
    {
        const PlayerAnalytics* pa = nullptr;
        for (const auto& p : m_analyticsCache)
            if (p.agentId == agentId) { pa = &p; break; }
        if (!pa) { toClose.push_back(agentId); continue; }

        std::string winTitle = std::format("{} - Skill Detail###SAPlayer_{}", pa->playerName, agentId);

        ImGui::SetNextWindowSize(ImVec2(580, 480), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(480, 300), ImVec2(vpW * 0.7f, vpH * 0.8f));

        ImGui::PushStyleColor(ImGuiCol_WindowBg,            ImVec4(0.06f, 0.07f, 0.09f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_TitleBg,             ImVec4(0.07f, 0.08f, 0.10f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,       ImVec4(0.10f, 0.09f, 0.06f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border,              ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_Separator,           ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,         ImVec4(1.f, 1.f, 1.f, 0.04f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,       ImVec4(0.80f, 0.68f, 0.30f, 0.60f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered,ImVec4(1.f, 0.84f, 0.39f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(1.f, 0.84f, 0.39f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 4.f);

        bool winOpen = true;
        bool winVisible = ImGui::Begin(winTitle.c_str(), &winOpen);

        {
            ImVec2 dp = ImGui::GetWindowPos();
            ImVec2 ds = ImGui::GetWindowSize();
            float dx = std::clamp(dp.x, 0.f, std::max(0.f, vpW - ds.x));
            float dy = std::clamp(dp.y, 0.f, std::max(0.f, vpH - ds.y));
            if (dx != dp.x || dy != dp.y)
                ImGui::SetWindowPos(ImVec2(dx, dy));
        }

        if (winVisible)
        {
            ImDrawList* ddl = ImGui::GetWindowDrawList();

            // Player header: prof icon + name + totals
            {
                ImVec2 cur = ImGui::GetCursorScreenPos();
                float lineH = ImGui::GetTextLineHeight();
                float profSz = 18.f;
                ImTextureID profTex = (pa->primaryProf >= 1) ? LoadProfIcon(dev, pa->primaryProf) : nullptr;
                if (profTex)
                {
                    float iy = cur.y + (lineH - profSz) * 0.5f;
                    ddl->AddImage(profTex, ImVec2(cur.x, iy), ImVec2(cur.x + profSz, iy + profSz));
                }
                ImU32 nameCol = TeamColor(pa->teamId);
                ddl->AddText(ImVec2(cur.x + profSz + 4.f, cur.y), nameCol, pa->playerName.c_str());

                char statBuf[64];
                snprintf(statBuf, sizeof(statBuf), "D: %s   H: %s",
                    FmtNum(pa->totalDamage).c_str(), FmtNum(pa->totalHealing).c_str());
                ImVec2 statSz = ImGui::CalcTextSize(statBuf);
                float rx = cur.x + ImGui::GetContentRegionAvail().x - statSz.x;
                ddl->AddText(ImVec2(rx, cur.y), kMuted, statBuf);
                ImGui::Dummy(ImVec2(0, lineH + 4.f));
            }

            ImGui::Separator();

            // Per-skill sections using a table for proper column alignment
            constexpr float kIconSz = 28.f;
            constexpr float kShareBarMaxW = 50.f;

            if (ImGui::BeginTable("##SASkills", 3,
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX))
            {
                ImGui::TableSetupColumn("Skill",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Dmg",    ImGuiTableColumnFlags_WidthFixed, 58.f);
                ImGui::TableSetupColumn("Heal",   ImGuiTableColumnFlags_WidthFixed, 58.f);

                for (const auto& sk : pa->skills)
                {
                    ImGui::PushID(sk.skillId);
                    ImGui::TableNextRow();

                    // Column 0: icon + name + casts
                    ImGui::TableSetColumnIndex(0);
                    {
                        ImVec2 cur = ImGui::GetCursorScreenPos();
                        ImTextureID iconTex = LoadSkillIcon(this, dev, sk.skillId,
                            m_skillIconIndex, m_skillIconCache);
                        if (iconTex)
                            ddl->AddImage(iconTex, cur, ImVec2(cur.x + kIconSz, cur.y + kIconSz));
                        else
                            ddl->AddRectFilled(cur, ImVec2(cur.x + kIconSz, cur.y + kIconSz),
                                IM_COL32(40, 40, 40, 255));
                        ImGui::Dummy(ImVec2(kIconSz + 4.f, kIconSz));
                        ImGui::SameLine();

                        ImGui::BeginGroup();
                        std::string sn = GetSkillDisplayName(sk.skillId);
                        ImGui::TextUnformatted(sn.c_str());
                        {
                            char line[128];
                            snprintf(line, sizeof(line), "Casts: %d", sk.totalCasts);
                            std::string detail(line);
                            if (sk.cancelled > 0 || sk.interrupted > 0)
                            {
                                snprintf(line, sizeof(line), "  (cancel: %d, intr: %d)",
                                    sk.cancelled, sk.interrupted);
                                detail += line;
                            }
                            ImGui::TextDisabled("%s", detail.c_str());
                        }
                        ImGui::EndGroup();
                    }

                    // Column 1: damage
                    ImGui::TableSetColumnIndex(1);
                    if (sk.totalDamage > 0)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, kDmgRed);
                        ImGui::Text("-%s", FmtNum(sk.totalDamage).c_str());
                        ImGui::PopStyleColor();
                    }
                    else
                        ImGui::TextDisabled("--");

                    // Column 2: healing
                    ImGui::TableSetColumnIndex(2);
                    if (sk.totalHealing > 0)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, kHealGrn);
                        ImGui::Text("+%s", FmtNum(sk.totalHealing).c_str());
                        ImGui::PopStyleColor();
                    }
                    else
                        ImGui::TextDisabled("--");

                    // Target distribution (spans full row below)
                    if (!sk.targets.empty())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);

                        std::string treeLabel = std::format("Targets ({})###tgt_{}", sk.targets.size(), sk.skillId);
                        if (ImGui::TreeNode(treeLabel.c_str()))
                        {
                            if (ImGui::BeginTable(
                                    std::format("##t_{}_{}", agentId, sk.skillId).c_str(),
                                    5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit
                                     | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX))
                            {
                                ImGui::TableSetupColumn("Targ...",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
                                ImGui::TableSetupColumn("Casts",   ImGuiTableColumnFlags_WidthFixed, 32.f);
                                ImGui::TableSetupColumn("Share",   ImGuiTableColumnFlags_WidthFixed, kShareBarMaxW + 28.f);
                                ImGui::TableSetupColumn("Dmg",     ImGuiTableColumnFlags_WidthFixed, 54.f);
                                ImGui::TableSetupColumn("Heal",    ImGuiTableColumnFlags_WidthFixed, 54.f);
                                ImGui::TableHeadersRow();

                                for (const auto& tb : sk.targets)
                                {
                                    ImGui::TableNextRow();

                                    ImGui::TableSetColumnIndex(0);
                                    {
                                        ImTextureID pTex = (tb.primaryProf >= 1)
                                            ? LoadProfIcon(dev, tb.primaryProf) : nullptr;
                                        if (pTex)
                                        {
                                            ImVec2 cp = ImGui::GetCursorScreenPos();
                                            float isz = ImGui::GetTextLineHeight();
                                            ddl->AddImage(pTex, cp, ImVec2(cp.x + isz, cp.y + isz));
                                            ImGui::Dummy(ImVec2(isz + 2.f, isz));
                                            ImGui::SameLine(0.f, 2.f);
                                        }
                                        ImGui::PushStyleColor(ImGuiCol_Text, TeamColor(tb.teamId));
                                        ImGui::TextUnformatted(tb.name.c_str());
                                        ImGui::PopStyleColor();
                                    }

                                    ImGui::TableSetColumnIndex(1);
                                    ImGui::Text("%d", tb.castCount);

                                    ImGui::TableSetColumnIndex(2);
                                    {
                                        ImVec2 barPos = ImGui::GetCursorScreenPos();
                                        float barH2 = ImGui::GetTextLineHeight() * 0.55f;
                                        float barY2 = barPos.y + (ImGui::GetTextLineHeight() - barH2) * 0.5f;

                                        ImU32 bc = TeamColor(tb.teamId);
                                        uint8_t r = (bc >> 0) & 0xFF;
                                        uint8_t g = (bc >> 8) & 0xFF;
                                        uint8_t b = (bc >> 16) & 0xFF;
                                        ddl->AddRectFilled(
                                            ImVec2(barPos.x, barY2),
                                            ImVec2(barPos.x + kShareBarMaxW * tb.castPct, barY2 + barH2),
                                            IM_COL32(r, g, b, 140), 2.f);

                                        char pctBuf[16];
                                        snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", tb.castPct * 100.f);
                                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kShareBarMaxW + 4.f);
                                        ImGui::TextUnformatted(pctBuf);
                                    }

                                    ImGui::TableSetColumnIndex(3);
                                    if (tb.damage > 0)
                                    {
                                        ImGui::PushStyleColor(ImGuiCol_Text, kDmgRed);
                                        ImGui::Text("-%s", FmtNum(tb.damage).c_str());
                                        ImGui::PopStyleColor();
                                    }
                                    else
                                        ImGui::TextDisabled("--");

                                    ImGui::TableSetColumnIndex(4);
                                    if (tb.healing > 0)
                                    {
                                        ImGui::PushStyleColor(ImGuiCol_Text, kHealGrn);
                                        ImGui::Text("+%s", FmtNum(tb.healing).c_str());
                                        ImGui::PopStyleColor();
                                    }
                                    else
                                        ImGui::TextDisabled("--");
                                }
                                ImGui::EndTable();
                            }
                            ImGui::TreePop();
                        }
                    }

                    // Separator between skills
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Separator();

                    ImGui::PopID();
                }

                ImGui::EndTable();
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(9);

        if (!winOpen)
            toClose.push_back(agentId);
    }

    for (int id : toClose)
        m_analyticsOpenPlayers.erase(id);
}
