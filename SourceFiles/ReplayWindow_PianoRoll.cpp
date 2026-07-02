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


void ReplayWindow::DrawPianoRollPanel()
{
    if (!m_showPianoRoll) return;
    if (m_team1PlayerIds.empty() && m_team2PlayerIds.empty()) return;

    auto* dev = m_deviceResources->GetD3DDevice();
    auto& db  = GetSkillDatabase();
    const float now = m_debugTimeline;
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = ImGui::GetFont();

    // Zoom table: half-window sizes in seconds
    static const float kZoomHalf[] = { 5.f, 10.f, 15.f, 30.f, 60.f };
    static const int   kZoomCount  = 5;
    m_pianoRollZoomIdx = std::clamp(m_pianoRollZoomIdx, 0, kZoomCount - 1);
    const float halfWin = kZoomHalf[m_pianoRollZoomIdx];
    const float winStart = now - halfWin;
    const float winEnd   = now + halfWin;
    const float winDur   = winEnd - winStart;

    // Layout constants
    constexpr float kNameColW   = 120.f;
    constexpr float kHeaderH    = 32.f;
    constexpr float kTimeAxisH  = 22.f;
    constexpr float kTeamLabelH = 24.f;
    constexpr float kRowH       = 32.f;
    constexpr float kBarH       = 20.f;
    constexpr float kLegendH    = 28.f;
    constexpr float kIconSz     = 18.f;

    const int nRed  = (int)m_team1PlayerIds.size();
    const int nBlue = (int)m_team2PlayerIds.size();
    const int nRedRows  = m_pianoRollTeam1Open ? nRed  : 0;
    const int nBlueRows = m_pianoRollTeam2Open ? nBlue : 0;
    const float bodyH = kTimeAxisH
                      + kTeamLabelH + nRedRows  * kRowH
                      + kTeamLabelH + nBlueRows * kRowH
                      + kLegendH;
    const float totalH = kHeaderH + bodyH;

    const float screenW = io.DisplaySize.x;

    // Panel window — Morale-style: standard ImGui title bar, matching colors
    ImGui::PushStyleColor(ImGuiCol_WindowBg,      ImVec4(0.055f, 0.078f, 0.102f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,     ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGui::SetNextWindowSizeConstraints(ImVec2(500.f, 0.f), ImVec2(screenW, io.DisplaySize.y));
    if (m_panelLayout.HasSavedPosition("piano_roll"))
        m_panelLayout.ApplyPosition("piano_roll");
    else
        ImGui::SetNextWindowPos(ImVec2(screenW - 720.f, 80.f), ImGuiCond_FirstUseEver);
    if (m_panelLayout.HasSavedSize("piano_roll"))
        m_panelLayout.ApplySize("piano_roll");
    else
        ImGui::SetNextWindowSize(ImVec2(700.f, totalH + 30.f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Piano Roll", &m_showPianoRoll,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        m_panelLayout.TrackWindow("piano_roll");
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);
        return;
    }

    m_panelLayout.TrackWindow("piano_roll");

    // Clamp to viewport
    {
        const auto* vp = ImGui::GetMainViewport();
        ImVec2 wPos = ImGui::GetWindowPos();
        ImVec2 wSz  = ImGui::GetWindowSize();
        float cx = std::clamp(wPos.x, vp->Pos.x, vp->Pos.x + vp->Size.x - wSz.x);
        float cy = std::clamp(wPos.y, vp->Pos.y, vp->Pos.y + vp->Size.y - wSz.y);
        if (cx != wPos.x || cy != wPos.y) ImGui::SetWindowPos(ImVec2(cx, cy));
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cp = ImGui::GetCursorScreenPos();
    const float panelW = ImGui::GetWindowWidth();
    const float tlW = panelW - kNameColW;
    float curY = cp.y;

    // ── Mouse wheel zoom (FIX 5) ──────────────────────────────────────
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
    {
        float wheel = io.MouseWheel;
        if (wheel != 0.f)
        {
            if (wheel > 0.f && m_pianoRollZoomIdx > 0) {
                m_pianoRollZoomIdx--;
                m_pianoRollZoomToast = (float)ImGui::GetTime();
            } else if (wheel < 0.f && m_pianoRollZoomIdx < kZoomCount - 1) {
                m_pianoRollZoomIdx++;
                m_pianoRollZoomToast = (float)ImGui::GetTime();
            }
        }
    }

    // ── Time axis ──────────────────────────────────────────────────────
    {
        float axisTop = curY;
        dl->AddRectFilled(ImVec2(cp.x, axisTop), ImVec2(cp.x + panelW, axisTop + kTimeAxisH),
                          IM_COL32(0, 0, 0, 40));

        char lbl[16];
        struct TimeLabel { float pct; char text[16]; bool isNow; };
        TimeLabel labels[5];
        auto fmtSec = [](char* buf, float s) {
            if (s == 0.f) { snprintf(buf, 16, "now"); return; }
            if (s == (int)s) snprintf(buf, 16, "%+ds", (int)s);
            else             snprintf(buf, 16, "%+.1fs", s);
        };
        float offsets[5] = { -halfWin, -halfWin * 0.5f, 0.f, halfWin * 0.5f, halfWin };
        for (int i = 0; i < 5; i++) {
            labels[i].pct = (float)i / 4.f;
            labels[i].isNow = (offsets[i] == 0.f);
            fmtSec(labels[i].text, offsets[i]);
        }

        float tlLeft = cp.x + kNameColW;
        for (int i = 0; i < 5; i++)
        {
            float lx = tlLeft + labels[i].pct * tlW;
            float fSz = labels[i].isNow ? 12.f : 11.f;
            ImU32 col = labels[i].isNow ? IM_COL32(0xD4,0xA0,0x20,0xFF) : IM_COL32(0x60,0x6A,0x74,0xFF);
            ImVec2 ts = font->CalcTextSizeA(fSz, FLT_MAX, 0.f, labels[i].text);
            float tx = lx - ts.x * 0.5f;
            tx = std::clamp(tx, tlLeft + 2.f, cp.x + panelW - ts.x - 2.f);
            dl->AddText(font, fSz, ImVec2(tx, axisTop + (kTimeAxisH - fSz) * 0.5f), col, labels[i].text);
        }
        curY += kTimeAxisH;
    }

    float nowLineTop = curY;
    float nowLineX   = cp.x + kNameColW + 0.5f * tlW;

    // Helper: snapshot for agent at time
    auto getSnap = [&](int agentId, float t) -> const AgentSnapshot*
    {
        auto ait = m_replayCtx.agents.find(agentId);
        if (ait == m_replayCtx.agents.end()) return nullptr;
        const auto& snaps = ait->second.snapshots;
        if (snaps.empty()) return nullptr;
        if (t <= snaps.front().time) return &snaps.front();
        if (t >= snaps.back().time)  return &snaps.back();
        int lo = 0, hi = (int)snaps.size() - 1;
        while (lo < hi) { int mid = lo + (hi - lo + 1) / 2; if (snaps[mid].time <= t) lo = mid; else hi = mid - 1; }
        return &snaps[lo];
    };

    // Edge fade bg color (matches panel background)
    const ImU32 kFadeSolid = IM_COL32(14, 20, 26, 234);
    const ImU32 kFadeTrans = IM_COL32(14, 20, 26, 0);

    // Helper: draw one team section (label + player rows)
    auto drawTeamBlock = [&](const std::vector<int>& playerIds, int teamId,
                             const char* teamName, bool& expanded,
                             ImU32 teamCol, ImU32 teamBg, ImU32 teamBorder)
    {
        float tlLeft = cp.x + kNameColW;
        int nPlayers = (int)playerIds.size();

        // Team label row
        ImVec2 tlMin(cp.x, curY);
        ImVec2 tlMax(cp.x + panelW, curY + kTeamLabelH);
        dl->AddRectFilled(tlMin, tlMax, teamBg);
        dl->AddLine(tlMin, ImVec2(tlMax.x, tlMin.y), teamBorder);
        dl->AddLine(ImVec2(tlMin.x, tlMax.y), tlMax, teamBorder);

        // Collapse triangle
        {
            float triX = cp.x + 8.f;
            float triY = curY + kTeamLabelH * 0.5f;
            float triSz = 5.f;
            ImU32 triCol = IM_COL32(255, 255, 255, 178);
            if (expanded) {
                dl->AddTriangleFilled(
                    ImVec2(triX, triY - triSz),
                    ImVec2(triX + triSz * 2.f, triY - triSz),
                    ImVec2(triX + triSz, triY + triSz), triCol);
            } else {
                dl->AddTriangleFilled(
                    ImVec2(triX, triY - triSz),
                    ImVec2(triX + triSz * 2.f, triY),
                    ImVec2(triX, triY + triSz), triCol);
            }
        }

        // Team name
        char teamBuf[128];
        if (expanded)
            snprintf(teamBuf, sizeof(teamBuf), "%s", teamName);
        else
            snprintf(teamBuf, sizeof(teamBuf), "%s  (%d players)", teamName, nPlayers);
        dl->AddText(font, 12.f, ImVec2(cp.x + 22.f, curY + (kTeamLabelH - 12.f) * 0.5f), teamCol, teamBuf);

        // Click to toggle collapse
        if (ImGui::IsMouseHoveringRect(tlMin, tlMax) && ImGui::IsMouseClicked(0))
            expanded = !expanded;

        curY += kTeamLabelH;

        if (!expanded) return;

        for (int pi = 0; pi < nPlayers; pi++)
        {
            int agentId = playerIds[pi];
            auto ait = m_replayCtx.agents.find(agentId);
            if (ait == m_replayCtx.agents.end()) { curY += kRowH; continue; }
            const auto& ard = ait->second;

            float rowTop = curY;
            float rowBot = curY + kRowH;

            bool rowHovered = ImGui::IsMouseHoveringRect(ImVec2(cp.x, rowTop), ImVec2(cp.x + panelW, rowBot));
            if (rowHovered) m_pianoRollHoverRow = agentId;

            float rowAlpha = 1.f;
            if (m_pianoRollHoverRow >= 0 && m_pianoRollHoverRow != agentId)
                rowAlpha = 0.6f;

            const AgentSnapshot* snap = getSnap(agentId, now);
            bool isDead = snap && snap->is_dead;

            dl->AddLine(ImVec2(cp.x, rowBot), ImVec2(cp.x + panelW, rowBot),
                        IM_COL32(255, 255, 255, 8));
            if (isDead)
                dl->AddRectFilled(ImVec2(cp.x, rowTop), ImVec2(cp.x + panelW, rowBot),
                                  IM_COL32(204, 48, 48, (int)(15 * rowAlpha)));
            if (rowHovered)
                dl->AddRectFilled(ImVec2(cp.x, rowTop), ImVec2(cp.x + panelW, rowBot),
                                  IM_COL32(255, 255, 255, 10));

            // Player label
            {
                float profIconSz = 16.f;
                ImTextureID profTex = LoadProfIcon(dev, ard.primaryProf);
                float iconY = rowTop + (kRowH - profIconSz) * 0.5f;
                if (profTex)
                    dl->AddImage(profTex, ImVec2(cp.x + 6.f, iconY),
                                 ImVec2(cp.x + 6.f + profIconSz, iconY + profIconSz),
                                 ImVec2(0,0), ImVec2(1,1),
                                 IM_COL32(255,255,255,(int)(255 * rowAlpha)));

                ImU32 nameCol = isDead
                    ? IM_COL32(0xCC,0x30,0x30,(int)(255 * rowAlpha))
                    : IM_COL32(0xF0,0xF0,0xF0,(int)(255 * rowAlpha));

                const char* pn = ard.playerName.c_str();
                char nameBuf[32];
                size_t pnLen = strlen(pn);
                if (pnLen > 13) { snprintf(nameBuf, sizeof(nameBuf), "%.12s..", pn); pn = nameBuf; }
                dl->AddText(font, 12.f, ImVec2(cp.x + 26.f, rowTop + (kRowH - 12.f) * 0.5f), nameCol, pn);

                if (rowHovered && ImGui::IsMouseHoveringRect(ImVec2(cp.x, rowTop), ImVec2(cp.x + kNameColW, rowBot))
                    && ImGui::IsMouseClicked(0) && !m_annotationMgr.draw_mode_active)
                {
                    OpenPlayerInfoPanel(agentId);
                }
            }

            // Timeline area
            if (isDead)
            {
                ImVec2 ts = font->CalcTextSizeA(11.f, FLT_MAX, 0.f, "DEAD");
                float dx = tlLeft + tlW * 0.5f - ts.x * 0.5f;
                dl->AddText(font, 11.f, ImVec2(dx, rowTop + (kRowH - 11.f) * 0.5f),
                            IM_COL32(0xCC,0x30,0x30,(int)(128 * rowAlpha)), "DEAD");
            }
            else
            {
                dl->PushClipRect(ImVec2(tlLeft, rowTop), ImVec2(cp.x + panelW, rowBot), true);

                float barY = rowTop + (kRowH - kBarH) * 0.5f;

                for (const auto& ev : ard.skillUseHistory)
                {
                    if (ev.isInstant) continue;
                    float castStart = ev.startTime;
                    float castEnd   = ev.endTime;
                    float dur = castEnd - castStart;
                    if (dur < 0.01f) continue;

                    float lPct = (castStart - winStart) / winDur;
                    float rPct = (castEnd   - winStart) / winDur;
                    if (rPct < -0.02f || lPct > 1.02f) continue;

                    float bx0 = tlLeft + lPct * tlW;
                    float bx1 = tlLeft + rPct * tlW;
                    float bw  = bx1 - bx0;

                    bool isFuture = castStart > now;
                    bool isActive = castStart <= now && castEnd > now;

                    float barAlpha = rowAlpha;
                    if (isFuture) barAlpha *= 0.50f;

                    const SkillInfo* si = db.IsLoaded() ? db.Get(ev.skillId) : nullptr;
                    int skillType = si ? si->type : 0;

                    ImU32 colL = PianoRollSkillColor(skillType, false);
                    ImU32 colR = PianoRollSkillColor(skillType, true);
                    colL = (colL & 0x00FFFFFF) | ((ImU32)(((colL >> 24) & 0xFF) * barAlpha) << 24);
                    colR = (colR & 0x00FFFFFF) | ((ImU32)(((colR >> 24) & 0xFF) * barAlpha) << 24);

                    if (isActive)
                    {
                        float progress = (now - castStart) / dur;
                        float fillX = bx0 + bw * progress;
                        dl->AddRectFilledMultiColor(ImVec2(bx0, barY), ImVec2(fillX, barY + kBarH), colL, colR, colR, colL);
                        dl->AddRectFilled(ImVec2(fillX, barY), ImVec2(bx1, barY + kBarH),
                                          IM_COL32(255,255,255,(int)(15 * barAlpha)), 2.f);
                    }
                    else
                    {
                        dl->AddRectFilledMultiColor(ImVec2(bx0, barY), ImVec2(bx1, barY + kBarH), colL, colR, colR, colL);
                    }
                    dl->AddRect(ImVec2(bx0, barY), ImVec2(bx1, barY + kBarH),
                                IM_COL32(0,0,0,(int)(60 * barAlpha)), 2.f);

                    // Skill icon (hidden if bar too narrow at max zoom)
                    if (bw >= 14.f)
                    {
                        ImTextureID skTex = LoadSkillIcon(this, dev, ev.skillId,
                                                         m_skillIconIndex, m_skillIconCache);
                        if (skTex)
                        {
                            float icoSz = std::min(kIconSz, kBarH - 2.f);
                            float icoX  = bx0 + 1.f;
                            float icoY  = barY + (kBarH - icoSz) * 0.5f;
                            float icoA  = (bw < icoSz + 4.f) ? 0.7f * barAlpha : barAlpha;
                            dl->AddImage(skTex, ImVec2(icoX, icoY),
                                         ImVec2(icoX + icoSz, icoY + icoSz),
                                         ImVec2(0,0), ImVec2(1,1),
                                         IM_COL32(255,255,255,(int)(255 * icoA)));
                        }
                    }

                    // Hover tooltip
                    if (ImGui::IsMouseHoveringRect(ImVec2(bx0, barY), ImVec2(std::max(bx1, bx0 + 6.f), barY + kBarH)))
                    {
                        ImGui::BeginTooltip();
                        if (si) ImGui::TextUnformatted(si->name.c_str());
                        else    ImGui::Text("Skill %d", ev.skillId);

                        int mm = (int)(ev.startTime) / 60;
                        float ss = ev.startTime - mm * 60.f;
                        ImGui::Text("Time: %d:%05.2f", mm, ss);
                        ImGui::Text("Caster: %s", ard.playerName.c_str());

                        if (ev.targetId >= 0)
                        {
                            auto tit = m_replayCtx.agents.find(ev.targetId);
                            if (tit != m_replayCtx.agents.end())
                                ImGui::Text("Target: %s", tit->second.playerName.c_str());
                        }
                        ImGui::EndTooltip();

                        if (ImGui::IsMouseClicked(0))
                            m_debugTimeline = ev.startTime;
                    }
                }

                // Edge fades
                dl->AddRectFilledMultiColor(
                    ImVec2(tlLeft, rowTop), ImVec2(tlLeft + 20.f, rowBot),
                    kFadeSolid, kFadeTrans, kFadeTrans, kFadeSolid);
                dl->AddRectFilledMultiColor(
                    ImVec2(cp.x + panelW - 20.f, rowTop), ImVec2(cp.x + panelW, rowBot),
                    kFadeTrans, kFadeSolid, kFadeSolid, kFadeTrans);

                dl->PopClipRect();
            }

            curY += kRowH;
        }
    };

    // ── Red team ──────────────────────────────────────────────────────
    drawTeamBlock(m_team1PlayerIds, 1,
                  m_team1GuildHeader.empty() ? "Red Team" : m_team1GuildHeader.c_str(),
                  m_pianoRollTeam1Open,
                  IM_COL32(0xD0,0x48,0x48,0xFF),
                  IM_COL32(0xD0,0x48,0x48,0x14),
                  IM_COL32(0xD0,0x48,0x48,0x26));

    // ── Blue team ───────────────────────────────────────────────────────
    drawTeamBlock(m_team2PlayerIds, 2,
                  m_team2GuildHeader.empty() ? "Blue Team" : m_team2GuildHeader.c_str(),
                  m_pianoRollTeam2Open,
                  IM_COL32(0x4A,0x90,0xD8,0xFF),
                  IM_COL32(0x4A,0x90,0xD8,0x14),
                  IM_COL32(0x4A,0x90,0xD8,0x26));

    float nowLineBot = curY;

    // ── Now line ───────────────────────────────────────────────────────
    dl->AddLine(ImVec2(nowLineX, nowLineTop), ImVec2(nowLineX, nowLineBot),
                IM_COL32(255, 255, 255, 140), 1.f);

    // ── Legend bar ──────────────────────────────────────────────────────
    {
        ImVec2 legMin(cp.x, curY);
        ImVec2 legMax(cp.x + panelW, curY + kLegendH);
        dl->AddRectFilled(legMin, legMax, IM_COL32(0, 0, 0, 40));
        dl->AddLine(legMin, ImVec2(legMax.x, legMin.y), IM_COL32(255, 255, 255, 13));

        float lx = cp.x + 12.f;
        float ly = curY + (kLegendH - 6.f) * 0.5f;

        struct LegItem { ImU32 colL; ImU32 colR; const char* label; };
        LegItem items[] = {
            { IM_COL32(0x1A,0x5A,0x2A,0xFF), IM_COL32(0x40,0xC0,0x60,0xFF), "Heal/Prot" },
            { IM_COL32(0x4A,0x3A,0x00,0xFF), IM_COL32(0xFF,0xB8,0x20,0xFF), "Damage" },
            { IM_COL32(0x50,0x1A,0x70,0xFF), IM_COL32(0x90,0x40,0xC0,0xFF), "Hex/Curse" },
            { IM_COL32(0x0A,0x3A,0x3A,0xFF), IM_COL32(0x20,0xC0,0xA0,0xFF), "Enchant" },
            { IM_COL32(0x2A,0x2A,0x2A,0xFF), IM_COL32(0x60,0x60,0x60,0xFF), "Other" },
        };

        for (auto& it : items)
        {
            dl->AddRectFilledMultiColor(
                ImVec2(lx, ly), ImVec2(lx + 20.f, ly + 6.f),
                it.colL, it.colR, it.colR, it.colL);
            dl->AddText(font, 11.f, ImVec2(lx + 24.f, curY + (kLegendH - 11.f) * 0.5f),
                        IM_COL32(0xA0,0xAA,0xB4,0xFF), it.label);
            ImVec2 ts = font->CalcTextSizeA(11.f, FLT_MAX, 0.f, it.label);
            lx += 24.f + ts.x + 14.f;
        }

        float rx = cp.x + panelW - 12.f;
        const char* futTxt = "dimmed = future";
        ImVec2 fts = font->CalcTextSizeA(11.f, FLT_MAX, 0.f, futTxt);
        dl->AddText(font, 11.f, ImVec2(rx - fts.x, curY + (kLegendH - 11.f) * 0.5f),
                    IM_COL32(0x70,0x7D,0x88,0xFF), futTxt);

        float nlx = rx - fts.x - 60.f;
        dl->AddLine(ImVec2(nlx, ly), ImVec2(nlx, ly + 6.f), IM_COL32(255,255,255,140));
        dl->AddText(font, 11.f, ImVec2(nlx + 4.f, curY + (kLegendH - 11.f) * 0.5f),
                    IM_COL32(0x70,0x7D,0x88,0xFF), "now");

        curY += kLegendH;
    }

    // ── Zoom toast ─────────────────────────────────────────────────────
    {
        float elapsed = (float)ImGui::GetTime() - m_pianoRollZoomToast;
        if (elapsed < 1.2f)
        {
            float alpha = (elapsed < 0.8f) ? 1.f : 1.f - (elapsed - 0.8f) / 0.4f;
            char zoomTxt[16];
            snprintf(zoomTxt, sizeof(zoomTxt), "\xc2\xb1 %.0fs", halfWin);
            ImVec2 ts = font->CalcTextSizeA(12.f, FLT_MAX, 0.f, zoomTxt);
            float tx = cp.x + panelW * 0.5f - ts.x * 0.5f;
            float ty = curY - kLegendH * 0.5f - 6.f;
            ImU32 zcol = IM_COL32(255, 255, 255, (int)(220 * alpha));
            ImU32 zbg  = IM_COL32(0, 0, 0, (int)(180 * alpha));
            dl->AddRectFilled(ImVec2(tx - 8.f, ty - 4.f), ImVec2(tx + ts.x + 8.f, ty + ts.y + 4.f), zbg, 6.f);
            dl->AddText(font, 12.f, ImVec2(tx, ty), zcol, zoomTxt);
        }
    }

    // Advance cursor so ImGui knows the content size
    float contentH = curY - cp.y;
    ImGui::Dummy(ImVec2(panelW, contentH));

    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        m_pianoRollHoverRow = -1;

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}
