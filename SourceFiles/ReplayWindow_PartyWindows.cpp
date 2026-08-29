#include "pch.h"
#include "ReplayWindow.h"
#include "AssetBlacklist.h"
#include "MatchRatings.h"
#include "MatchNotes.h"
#include "MatchBookmarks.h"
#include "AgentSnapshotParser.h"
#include "StoCParser.h"
#include "SkillDatabase.h"
#include "MaxHpSolver.h"
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
// Damage / Heal meter data accumulation
// ---------------------------------------------------------------------------

void ReplayWindow::BuildMeterAbsCache()
{
    if (m_meterAbsCacheBuilt || !m_maxHpSolved) return;
    m_meterAbsCache.clear();
    m_meterAbsCache.reserve(m_replayCtx.stocData.combat.size());

    // Recorded max_hp is the effective value the packet fractions are taken
    // against; the solved per-weapon-set value is the fallback. See
    // ResolveMaxHp for why this order matters.
    auto findMaxHp = [&](int agentId, float t) -> uint32_t {
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return 0;
        if (uint32_t m = it->second.maxHpAtTime(t)) return m;
        return it->second.solvedMaxHpAtTime(t);
    };

    for (auto& ce : m_replayCtx.stocData.combat)
    {
        if (!ce.IsDamageOrHeal()) continue;
        uint32_t mhp = CorrectMaxHpForPacket(findMaxHp(ce.target_id, ce.time), ce.value);
        int absVal = (mhp > 0)
            ? (int)std::lround(std::fabs((double)ce.value) * mhp) : 0;
        if (absVal <= 0) continue;
        bool isDmg = (ce.value < 0.f);
        m_meterAbsCache.push_back({ ce.time, ce.caster_id, absVal, isDmg });
    }
    m_meterAbsCacheBuilt = true;
}

ReplayWindow::MaxHpSample ReplayWindow::ResolveMaxHp(const AgentReplayData& ard, float t) const
{
    const AgentSnapshot* snap = FindSnapshotAtTime(ard, t);
    const bool deepWounded = snap && snap->has_deep_wound;

    // (0) Forward model: base + armour + weapon set + shrine, then morale, then Deep Wound.
    //
    // Preferred over everything below because it is the only source that follows a weapon swap the
    // instant it happens. The recorded max_hp does not: the server pushes it rarely, and in one
    // measured match a player's value was pushed exactly once and then held for five minutes while
    // he swapped sets dozens of times. The packet-corrected timeline does not either -- it settles
    // on whichever value explains the most packets, which is a player's most common set rather
    // than his current one. That is why a player known to have 570 with his shield out was being
    // shown 540.
    //
    // Deep Wound is already applied inside the model, so it is not reapplied here.
    //
    // The gate asks the model rather than testing ard.armourSolved, because an armour solve is
    // only one of the ways it can produce an answer. A pet wears no armour and a guild hall NPC
    // has a stated maximum, so both are complete without a solve and neither would ever pass an
    // armourSolved test -- which is why, before this, a pet's health fell through to the 480
    // fallback and a Guild Lord was shown 480 instead of 1680. At() already returns Fallback
    // whenever it genuinely cannot run, so the inner check is the whole test.
    if (m_healthInputsBuilt)
    {
        const HealthModel::Breakdown b = HealthModel::At(ard, t, m_healthInputs);
        if (b.source != HealthModel::Source::Fallback && b.total > 0)
            return { b.total, false };
    }

    // (1) Camera-observed max_hp. A number the server actually sent, which is worth more than any
    // reconstruction of it -- even though it may be stale for the current weapon set, since the
    // server pushes it rarely.
    if (m_matchMeta.recording_version >= 2)
    {
        uint32_t maxHp = ard.maxHpAtTime(t);
        if (maxHp > 0)
        {
            // Unlike the timeline this value may ALREADY carry the reduction,
            // in the small share of cases where the observer captured the
            // refreshed field. Only reduce when it still reads as it did
            // before the episode began, so the 20% is never applied twice.
            if (deepWounded)
            {
                uint32_t before = ard.maxHpBeforeDeepWound(t);
                if (before == 0 || maxHp == before)
                    maxHp = AgentReplayData::ApplyDeepWound(maxHp);
            }
            return { maxHp, false };
        }
    }

    // Everything below is inferred from packet decimals rather than measured, and is reported as
    // estimated so the party bar prefixes it with a tilde. These are last-resort methods: they
    // search for whichever number makes the most damage fractions land on whole health, with no
    // notion of what a maximum can be made of. One player with no camera reading at all was shown
    // 697 that way -- a value that explains none of his packets and that no combination of runes,
    // weapon mods and morale can produce.

    // (2) Effective timeline reconstructed from corrected packet denominators. Entries exclude
    // Deep Wound by construction, so the reduction is applied here.
    if (uint32_t eff = ard.effectiveMaxHpAtTime(t))
        return { deepWounded ? AgentReplayData::ApplyDeepWound(eff) : eff, true };

    // (3) Solved per weapon set from combat decimals and skill breakpoints.
    if (uint32_t solved = ard.solvedMaxHpAtTime(t))
        return { deepWounded ? AgentReplayData::ApplyDeepWound(solved) : solved, true };

    // (4) Last of all: base health is 100 at level 1 and rises 20 per level, so 20*level + 80 --
    // the documented 480 at level 20, not 500.
    uint32_t estimated = (ard.playerLevel > 0) ? (20 * ard.playerLevel + 80) : 480;
    if (deepWounded) estimated = AgentReplayData::ApplyDeepWound(estimated);
    return { estimated, true };
}

void ReplayWindow::AccumulateMeterData()
{
    if (!m_replayCtx.stocLoaded || !m_replayCtx.agentsLoaded) return;

    BuildMeterAbsCache();

    float curTime = m_debugTimeline;

    if (curTime < m_meterLastTime || m_meterLastTime < 0.f)
    {
        m_meterDmg.clear();
        m_meterHeal.clear();
        m_meterTotalDmg = 0;
        m_meterTotalHeal = 0;
        m_meterTotalDmgTeam1 = 0;
        m_meterTotalDmgTeam2 = 0;
        m_meterTotalHealTeam1 = 0;
        m_meterTotalHealTeam2 = 0;
        m_meterMaxDmg = 0;
        m_meterMaxHeal = 0;
        m_meterLastIdx = 0;
    }

    int n = (int)m_meterAbsCache.size();
    for (int i = m_meterLastIdx; i < n; ++i)
    {
        auto& e = m_meterAbsCache[i];
        if (e.time > curTime) break;

        uint8_t casterTeam = 0;
        auto ait = m_replayCtx.agents.find(e.casterId);
        if (ait != m_replayCtx.agents.end())
            casterTeam = ait->second.teamId;

        if (e.isDamage)
        {
            m_meterDmg[e.casterId].value += e.absValue;
            m_meterTotalDmg += e.absValue;
            if (casterTeam == 1)       m_meterTotalDmgTeam1 += e.absValue;
            else if (casterTeam == 2)  m_meterTotalDmgTeam2 += e.absValue;
            else { m_meterTotalDmgTeam1 += e.absValue; m_meterTotalDmgTeam2 += e.absValue; }
            if (m_meterDmg[e.casterId].value > m_meterMaxDmg)
                m_meterMaxDmg = m_meterDmg[e.casterId].value;
        }
        else
        {
            m_meterHeal[e.casterId].value += e.absValue;
            m_meterTotalHeal += e.absValue;
            if (casterTeam == 1)       m_meterTotalHealTeam1 += e.absValue;
            else if (casterTeam == 2)  m_meterTotalHealTeam2 += e.absValue;
            else { m_meterTotalHealTeam1 += e.absValue; m_meterTotalHealTeam2 += e.absValue; }
            if (m_meterHeal[e.casterId].value > m_meterMaxHeal)
                m_meterMaxHeal = m_meterHeal[e.casterId].value;
        }
        m_meterLastIdx = i + 1;
    }

    m_meterLastTime = curTime;
}


// ---------------------------------------------------------------------------

void ReplayWindow::DrawPartyWindows()
{
    if (!m_replayCtx.agentsLoaded || !m_agentsClassified) return;

    if (m_showDamageMeter || m_showHealMeter)
        AccumulateMeterData();

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    PartyIcons icons = LoadAllPartyIcons(dev);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x;
    float vpH = vp->Size.y;

    constexpr float kBarHeight    = 23.f;
    constexpr float kNpcBarHeight = 17.f;
    constexpr float kBarSpacing   = 4.f;
    constexpr float kDeathGraceSec = 5.f;
    float padY = ImGui::GetStyle().WindowPadding.y;
    float titleBarH = ImGui::GetFrameHeight();
    float treeNodeH = ImGui::GetFrameHeight() + kBarSpacing;
    float curTime = m_debugTimeline;

    auto IsSpiritHidden = [&](const AgentReplayData& ard) -> bool {
        if (ard.type != AgentType::Spirit) return false;
        if (ard.overlapHidden) return true;
        if (ard.snapshots.empty()) return true;

        // Spirit outside its snapshot time range is gone
        if (curTime < ard.snapshots.front().time ||
            curTime > ard.snapshots.back().time)
            return true;

        const AgentSnapshot* snap = FindSnapshotAtTime(ard, curTime);
        if (!snap) return true;

        // Primary check: is_alive == false means the spirit no longer exists
        if (!snap->is_alive)
        {
            float goneStart = ard.notAliveTransitionTime(curTime);
            if (curTime - goneStart > kDeathGraceSec)
                return true;
        }

        // Secondary check: is_dead flag (covers explicit kills)
        if (snap->is_dead)
        {
            float deathStart = ard.deathTransitionTime(curTime);
            if (curTime - deathStart > kDeathGraceSec)
                return true;
        }

        return false;
    };

    // Summoned minions (e.g. Bone Horror): the ally entry only appears while
    // the agent exists and is alive (same rule as its 3D model / minimap dot).
    auto IsMinionHidden = [&](const AgentReplayData& ard) -> bool {
        if (ard.type != AgentType::NPC) return false;
        if (!IsNpcHiddenWhenDead(ard.modelId)) return false;
        return !ard.isMinionVisibleAtTime(curTime);
    };

    bool bothMeters = m_showDamageMeter && m_showHealMeter;
    constexpr ImU32 kDmgBarCol  = IM_COL32(0xD0, 0x8C, 0x20, 0x80);
    constexpr ImU32 kHealBarCol = IM_COL32(0x40, 0xA8, 0x40, 0x80);

    auto DrawBars = [&](ImDrawList* dl, float availW,
                        const std::vector<int>& ids, float barH,
                        bool filterSpirits, bool leftSide, float maxBarW,
                        int teamTotalDmg, int teamTotalHeal)
    {
        int n = static_cast<int>(ids.size());
        for (int i = 0; i < n; ++i)
        {
            int agentId = ids[i];
            auto it = m_replayCtx.agents.find(agentId);
            if (it == m_replayCtx.agents.end()) continue;

            const AgentReplayData& ard = it->second;
            if (filterSpirits && IsSpiritHidden(ard))
                continue;
            if (IsMinionHidden(ard))
                continue;

            const AgentSnapshot* snap = FindSnapshotAtTime(ard, m_debugTimeline);
            bool isDead = snap ? snap->is_dead : false;

            ImVec2 cursor = ImGui::GetCursorScreenPos();

            char btnId[32];
            snprintf(btnId, sizeof(btnId), "##PB%d", agentId);
            ImGui::InvisibleButton(btnId, ImVec2(availW, barH));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !m_annotationMgr.IsDrawModeActive())
            {
                EnterFollowMode(agentId);
                OpenPlayerInfoPanel(agentId);
                if (m_showRangeRings)
                    m_ringAgentFilter = (m_ringAgentFilter == agentId) ? -1 : agentId;
                if (m_fogPerspective > 0)
                    m_fogPlayerAgent = (m_fogPlayerAgent == agentId) ? -1 : agentId;
            }

            bool isFogHidden = (m_fogPerspective > 0 && ard.teamId != m_fogPerspective && IsAgentInFog(agentId));

            ImTextureID carriedFlagTex =
                CarriedBundleIcon(m_deviceResources->GetD3DDevice(), agentId);

            uint32_t absMaxHp = 0;
            bool hpEstimated = false;
            if (m_showAbsoluteHp)
            {
                auto sample = ResolveMaxHp(ard, m_debugTimeline);
                absMaxHp = sample.value;
                hpEstimated = sample.estimated;
            }

            // A summon's level is the one number on this row that says something about the
            // player who made it: "Create a level 1...16 spirit" is a breakpoint table, so a
            // level 13 Rejuvenation spirit is Restoration Magic 12 read straight off the ally
            // list. The recording gives it (infos.json), and only for the first agent of a
            // recycled id - so it is 0 for a spirit whose slot had been used before, and a row
            // with nothing to say says nothing.
            const char* barLabel = ard.partyBarLabel.c_str();
            char levelledLabel[160];
            if (ard.playerLevel > 0 &&
                (ard.type == AgentType::Spirit || IsMinionModelId(ard.modelId)))
            {
                snprintf(levelledLabel, sizeof(levelledLabel), "%s L%d",
                         ard.partyBarLabel.c_str(), ard.playerLevel);
                barLabel = levelledLabel;
            }

            DrawPartyHealthBar(dl, cursor, availW, barH,
                               snap, ard.teamId, isDead,
                               barLabel, icons,
                               m_followedAgentId, agentId, isFogHidden,
                               carriedFlagTex, absMaxHp, hpEstimated);

            if (m_showDamageMeter || m_showHealMeter)
            {
                ImDrawList* fgDl = ImGui::GetForegroundDrawList();
                float meterH = bothMeters ? barH * 0.5f : barH * 0.75f;

                float singleOffset = bothMeters ? 0.f : (barH - meterH) * 0.5f;

                if (m_showDamageMeter)
                {
                    auto dit = m_meterDmg.find(agentId);
                    int dmgVal = (dit != m_meterDmg.end()) ? dit->second.value : 0;
                    float slotY = cursor.y + singleOffset;
                    DrawMeterBar(fgDl, cursor, availW, slotY, meterH,
                                 dmgVal, m_meterMaxDmg, teamTotalDmg,
                                 leftSide, maxBarW, kDmgBarCol);
                }

                if (m_showHealMeter)
                {
                    auto hit = m_meterHeal.find(agentId);
                    int healVal = (hit != m_meterHeal.end()) ? hit->second.value : 0;
                    float slotY = bothMeters ? cursor.y + barH * 0.5f
                                             : cursor.y + singleOffset;
                    DrawMeterBar(fgDl, cursor, availW, slotY, meterH,
                                 healVal, m_meterMaxHeal, teamTotalHeal,
                                 leftSide, maxBarW, kHealBarCol);
                }
            }
        }
    };

    auto CountVisibleNpcs = [&](const std::vector<int>& npcIds) -> int {
        int count = 0;
        for (int id : npcIds)
        {
            auto it = m_replayCtx.agents.find(id);
            if (it == m_replayCtx.agents.end()) continue;
            const AgentReplayData& ard = it->second;
            if (IsSpiritHidden(ard)) continue;
            if (IsMinionHidden(ard)) continue;
            ++count;
        }
        return count;
    };

    bool& s_alliesOpenTeam1 = m_alliesOpenTeam1;
    bool& s_alliesOpenTeam2 = m_alliesOpenTeam2;

    auto DrawTeamPanel = [&](const char* title, bool* show,
                             const std::vector<int>& playerIds,
                             const std::vector<int>& npcIds,
                             ImVec4 bgCol, bool leftSide,
                             bool* prevAlliesOpen,
                             const char* layoutKey,
                             int teamTotalDmg, int teamTotalHeal)
    {
        if (!*show || playerIds.empty()) return;

        int nPlayers = static_cast<int>(playerIds.size());
        bool hasNpcs = !npcIds.empty();
        float panelW = std::clamp(vpW * 0.18f, 220.f, 350.f);

        float playersH = nPlayers * kBarHeight + (nPlayers - 1) * kBarSpacing;
        float alliesHeaderH = hasNpcs ? treeNodeH + kBarSpacing : 0.f;
        float collapsedH = titleBarH + padY * 2 + playersH + alliesHeaderH + 4.f;

        if (!m_partyWindowsPositioned)
        {
            if (m_panelLayout.HasSavedPosition(layoutKey))
            {
                m_panelLayout.ApplyPosition(layoutKey);
                if (m_panelLayout.HasSavedSize(layoutKey))
                    m_panelLayout.ApplySize(layoutKey);
                else
                    ImGui::SetNextWindowSize(ImVec2(panelW, collapsedH));
            }
            else
            {
                float midY = vp->Pos.y + vpH * 0.5f;
                float marginX = vpW * 0.02f;
                ImVec2 pos;
                if (leftSide)
                    pos = ImVec2(vp->Pos.x + marginX, midY - collapsedH * 0.5f);
                else
                    pos = ImVec2(vp->Pos.x + vpW - panelW - marginX, midY - collapsedH * 0.5f);
                ImGui::SetNextWindowPos(pos);
                ImGui::SetNextWindowSize(ImVec2(panelW, collapsedH));
            }
        }

        ImGui::PushStyleColor(ImGuiCol_WindowBg, bgCol);
        ImGui::PushStyleColor(ImGuiCol_TitleBg,       ImVec4(0.06f, 0.06f, 0.08f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.08f, 0.08f, 0.10f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.89f, 0.71f, 1.0f));

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, kBarSpacing));

        if (ImGui::Begin(title, show))
        {
            ImGui::PopStyleColor();

            m_panelLayout.TrackWindow(layoutKey);

            // Clamp window within viewport
            ImVec2 wPos = ImGui::GetWindowPos();
            ImVec2 wSize = ImGui::GetWindowSize();
            bool clamped = false;
            if (wPos.x < vp->Pos.x) { wPos.x = vp->Pos.x; clamped = true; }
            if (wPos.y < vp->Pos.y) { wPos.y = vp->Pos.y; clamped = true; }
            if (wPos.x + wSize.x > vp->Pos.x + vpW) { wPos.x = vp->Pos.x + vpW - wSize.x; clamped = true; }
            if (wPos.y + wSize.y > vp->Pos.y + vpH) { wPos.y = vp->Pos.y + vpH - wSize.y; clamped = true; }
            if (clamped) ImGui::SetWindowPos(wPos);

            float availW = ImGui::GetContentRegionAvail().x;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // --- Sword / Heart toggle icons in title bar ---
            {
                ImTextureID swordTex = LoadFlagIcon(dev, "damagedone.png");
                ImTextureID heartTex = LoadFlagIcon(dev, "healingreceived.png");
                const float iconSz = titleBarH - 6.f;
                const float iconPad = 3.f;
                float iconY = wPos.y + (titleBarH - iconSz) * 0.5f;

                float closeBtnW = titleBarH;
                float heartX = wPos.x + wSize.x - closeBtnW - iconPad - iconSz;
                float swordX = heartX - iconPad - iconSz;

                dl->PushClipRect(wPos, ImVec2(wPos.x + wSize.x, wPos.y + wSize.y), false);

                ImGuiIO& io = ImGui::GetIO();
                ImGuiContext* ctx = ImGui::GetCurrentContext();
                bool windowHovered = (ctx && ctx->HoveredWindow == ImGui::GetCurrentWindow());

                auto DrawToggleIcon = [&](ImTextureID tex, float ix, bool& toggleRef, const char* tooltip)
                {
                    if (!tex) return;
                    ImVec2 tl(ix, iconY);
                    ImVec2 br(ix + iconSz, iconY + iconSz);

                    bool hovered = windowHovered
                        && io.MousePos.x >= tl.x && io.MousePos.x < br.x
                        && io.MousePos.y >= tl.y && io.MousePos.y < br.y;

                    if (toggleRef)
                    {
                        dl->AddImage(tex, tl, br, ImVec2(0,0), ImVec2(1,1),
                                     IM_COL32(0xFF, 0xD7, 0x00, 0xFF));
                        float uy = br.y + 1.f;
                        dl->AddLine(ImVec2(tl.x, uy), ImVec2(br.x, uy),
                                    IM_COL32(0xCB, 0xAA, 0x09, 0xFF), 2.f);
                    }
                    else
                    {
                        dl->AddImage(tex, tl, br);
                    }

                    if (hovered)
                    {
                        float expand = iconSz * 0.10f;
                        ImVec2 htl(tl.x - expand, tl.y - expand);
                        ImVec2 hbr(br.x + expand, br.y + expand);
                        dl->AddImage(tex, htl, hbr, ImVec2(0,0), ImVec2(1,1),
                                     IM_COL32(0xFF, 0xE0, 0x80, 0xB0));

                        g_CurrentCursor = CursorMode::Clickable;
                        ImGui::SetTooltip("%s", tooltip);

                        if (io.MouseClicked[0])
                        {
                            toggleRef = !toggleRef;
                            if (ctx->MovingWindow == ImGui::GetCurrentWindow())
                                ctx->MovingWindow = nullptr;
                        }
                    }
                };

                DrawToggleIcon(swordTex, swordX, m_showDamageMeter, "Toggle Damage Meter");
                DrawToggleIcon(heartTex, heartX, m_showHealMeter, "Toggle Heal Meter");

                dl->PopClipRect();
            }

            float maxBarW = std::clamp(vpW * 0.10f, 60.f, 200.f);
            DrawBars(dl, availW, playerIds, kBarHeight, false, leftSide, maxBarW,
                     teamTotalDmg, teamTotalHeal);

            if (m_showDamageMeter || m_showHealMeter)
            {
                int pDmg = 0, pHeal = 0;
                for (int id : playerIds)
                {
                    auto d = m_meterDmg.find(id);
                    if (d != m_meterDmg.end()) pDmg += d->second.value;
                    auto h = m_meterHeal.find(id);
                    if (h != m_meterHeal.end()) pHeal += h->second.value;
                }
                ImDrawList* fgDl = ImGui::GetForegroundDrawList();
                float sumAnchorX = leftSide ? (wPos.x + wSize.x) : wPos.x;
                DrawMeterSumText(fgDl, sumAnchorX, wPos.y, titleBarH,
                                 pDmg, pHeal,
                                 m_showDamageMeter, m_showHealMeter, leftSide);
            }

            // Allies collapsible section
            if (hasNpcs)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
                ImGui::Spacing();
                ImGui::PopStyleVar();

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.72f, 0.60f, 1.0f));
                ImGui::SetNextItemOpen(*prevAlliesOpen, ImGuiCond_Once);
                ImVec2 alliesHeaderPos = ImGui::GetCursorScreenPos();
                bool alliesOpen = ImGui::TreeNodeEx("Allies",
                    ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_NoAutoOpenOnLog);
                ImGui::PopStyleColor();

                if (m_showDamageMeter || m_showHealMeter)
                {
                    int aDmg = 0, aHeal = 0;
                    for (int id : npcIds)
                    {
                        auto it2 = m_replayCtx.agents.find(id);
                        if (it2 == m_replayCtx.agents.end()) continue;
                        if (IsSpiritHidden(it2->second)) continue;
                        auto d = m_meterDmg.find(id);
                        if (d != m_meterDmg.end()) aDmg += d->second.value;
                        auto h = m_meterHeal.find(id);
                        if (h != m_meterHeal.end()) aHeal += h->second.value;
                    }
                    float allyAnchorX = leftSide ? (wPos.x + wSize.x) : wPos.x;
                    DrawMeterSumText(ImGui::GetForegroundDrawList(),
                                     allyAnchorX, alliesHeaderPos.y, ImGui::GetFrameHeight(),
                                     aDmg, aHeal,
                                     m_showDamageMeter, m_showHealMeter, leftSide);
                }

                // Auto-resize on open/close transition
                if (alliesOpen != *prevAlliesOpen)
                {
                    *prevAlliesOpen = alliesOpen;
                    if (alliesOpen)
                    {
                        int visNpcs = CountVisibleNpcs(npcIds);
                        float npcsH = visNpcs * kNpcBarHeight + std::max(0, visNpcs - 1) * kBarSpacing;
                        float expandedH = collapsedH + npcsH + kBarSpacing;
                        ImGui::SetWindowSize(ImVec2(wSize.x, expandedH));
                    }
                    else
                    {
                        ImGui::SetWindowSize(ImVec2(wSize.x, collapsedH));
                    }
                }

                if (alliesOpen)
                {
                    DrawBars(dl, availW, npcIds, kNpcBarHeight, true, leftSide, maxBarW,
                             teamTotalDmg, teamTotalHeal);
                }
            }
        }
        else
        {
            ImGui::PopStyleColor();
            m_panelLayout.TrackWindow(layoutKey);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    };

    DrawTeamPanel(m_team1GuildHeader.c_str(), &m_showTeam1Party, m_team1PlayerIds,
                  m_team1NpcIds,
                  ImVec4(44.f/255.f, 8.f/255.f, 5.f/255.f, 0.10f), true,
                  &s_alliesOpenTeam1, "team1_party",
                  m_meterTotalDmgTeam1, m_meterTotalHealTeam1);

    DrawTeamPanel(m_team2GuildHeader.c_str(), &m_showTeam2Party, m_team2PlayerIds,
                  m_team2NpcIds,
                  ImVec4(11.f/255.f, 8.f/255.f, 38.f/255.f, 0.10f), false,
                  &s_alliesOpenTeam2, "team2_party",
                  m_meterTotalDmgTeam2, m_meterTotalHealTeam2);

    if (!m_partyWindowsPositioned)
        m_partyWindowsPositioned = true;
}
