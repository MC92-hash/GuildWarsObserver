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
#include "UrlProtocol.h"
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


void ReplayWindow::DrawTimelineController()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float vpW = vp->Size.x;
    const float vpH = vp->Size.y;

    const float sf = 1.f;

    // Taller while a comms recording is attached: its lane runs under the controls.
    const float barH     = PlayBarHeight();
    const float PAD      = 12.f;
    const float PADY     = 6.f;
    const float TRKH     = 4.f;
    const float BTN_H    = 30.f;
    const float PLAY_SZ  = 36.f;
    const float BGAP     = 2.f;
    const float GDIV     = 10.f;
    const float ROW2_OFS = 28.f;
    const float d2r      = IM_PI / 180.f;

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vpH - barH));
    ImGui::SetNextWindowSize(ImVec2(vpW, barH));

    constexpr ImGuiWindowFlags kBarFlags =
        ImGuiWindowFlags_NoTitleBar      | ImGuiWindowFlags_NoResize       |
        ImGuiWindowFlags_NoMove          | ImGuiWindowFlags_NoScrollbar    |
        ImGuiWindowFlags_NoCollapse      | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground    | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,       ImVec4(0.07f, 0.06f, 0.04f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.78f, 0.66f, 0.29f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.78f, 0.66f, 0.29f, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.60f, 0.54f, 0.41f, 1.0f));

    if (!ImGui::Begin("PlaybackBar", nullptr, kBarFlags))
    {
        ImGui::End();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        return;
    }

    ImDrawList* dl   = ImGui::GetWindowDrawList();
    const ImVec2 O   = ImGui::GetWindowPos();
    ImFont* font     = ImGui::GetFont();
    const float fs   = font->FontSize;
    const float fsSm = fs * 0.82f;

    // ── GW1 Dark-Glass Gold Palette ──────────────────────────────────────
    const ImU32 cBg         = IM_COL32(  8,   9,  12, 140);
    const ImU32 cGlass      = IM_COL32( 18,  16,  10, 150);
    const ImU32 cBorder     = IM_COL32(160, 120,  40,  46);
    const ImU32 cBorderHi   = IM_COL32(200, 168,  75,  82);
    const ImU32 cBorderGlow = IM_COL32(200, 168,  75, 140);
    const ImU32 cGold       = IM_COL32(200, 168,  75, 255);
    const ImU32 cGoldBright = IM_COL32(226, 194, 106, 255);
    const ImU32 cGoldDim    = IM_COL32(122,  96,  32, 255);
    const ImU32 cGoldFill   = IM_COL32(200, 168,  75,  20);
    const ImU32 cText       = IM_COL32(232, 223, 200, 255);
    const ImU32 cTextMid    = IM_COL32(154, 138, 104, 255);
    const ImU32 cTextDim    = IM_COL32(120, 108,  80, 255);
    const ImU32 cDanger     = IM_COL32(192,  80,  74, 255);
    const ImU32 cGreen      = IM_COL32( 90, 170, 120, 255);
    const ImU32 cGreenDim   = IM_COL32( 90, 170, 120,  80);
    const ImU32 cTrackBg    = IM_COL32(200, 168,  75,  18);

    // ── Background: dark bg + glass panel + border + shimmer ─────────────
    dl->AddRectFilled(O, ImVec2(O.x + vpW, O.y + barH), cBg);

    const float px = O.x + 2.f, py = O.y + 2.f;
    const float pw = vpW - 4.f, ph = barH - 4.f;
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph), cGlass, 8.f);
    dl->AddRect(ImVec2(px, py), ImVec2(px + pw, py + ph), cBorderHi, 8.f);

    {
        float sx = px + 12.f * sf, sw = pw - 24.f * sf, sy = py;
        float mx = sx + sw * 0.5f;
        dl->AddRectFilledMultiColor(
            ImVec2(sx, sy), ImVec2(mx, sy + 1.f),
            IM_COL32(200,168,75, 0), IM_COL32(200,168,75,100),
            IM_COL32(200,168,75,100), IM_COL32(200,168,75, 0));
        dl->AddRectFilledMultiColor(
            ImVec2(mx, sy), ImVec2(sx + sw, sy + 1.f),
            IM_COL32(200,168,75,100), IM_COL32(200,168,75, 0),
            IM_COL32(200,168,75, 0), IM_COL32(200,168,75,100));
    }

    // ── State ────────────────────────────────────────────────────────────
    float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
    auto& ctx  = m_replayCtx;
    // Spoiler-free: the track's length. The recording's end while the result shows; a fixed
    // 30:00 while it is hidden, so neither the bar nor its minute ticks give the length away.
    const bool  spoilersHidden = SpoilersHidden();
    const float axisT = std::max(1.f, SpoilerAxisEnd(maxT));

    const auto&   speeds      = kReplaySpeeds;
    const auto&   speedLabels = kReplaySpeedLabels;
    constexpr int speedCount  = kReplaySpeedCount;

    const float x0 = px + PAD;
    const float y0 = py + PADY;

    // ══════════════════════════════════════════════════════════════════════
    // ROW 1: SCRUBBER
    // ══════════════════════════════════════════════════════════════════════
    {
        char curBuf[16], totBuf[16];
        FormatTime(m_debugTimeline - m_displayTimeOffset, curBuf, sizeof(curBuf));
        if (spoilersHidden)
            snprintf(totBuf, sizeof(totBuf), "--:--");
        else
            FormatTime(maxT - m_displayTimeOffset, totBuf, sizeof(totBuf));

        dl->AddText(font, fs, ImVec2(x0, y0 + 1.f), cText, curBuf);
        float curW = font->CalcTextSizeA(fs, FLT_MAX, 0.f, curBuf).x;

        float sepY = y0 + (fs - fsSm) * 0.5f + 1.f;
        dl->AddText(font, fsSm, ImVec2(x0 + curW + 2.f, sepY), cBorderHi, " / ");
        float sepW = font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, " / ").x;
        dl->AddText(font, fsSm, ImVec2(x0 + curW + 2.f + sepW, sepY), cTextMid, totBuf);
    }

    const float timeW     = 80.f * sf;
    const float badgeW    = 64.f * sf;
    const float trackX    = x0 + timeW + 6.f * sf;
    const float trackW    = pw - PAD * 2.f - timeW - 6.f * sf - badgeW - 6.f * sf;
    const float trackMidY = y0 + 10.f * sf;
    const float trackBarY = trackMidY - TRKH * 0.5f;

    dl->AddRectFilled(
        ImVec2(trackX, trackBarY), ImVec2(trackX + trackW, trackBarY + TRKH), cTrackBg, 2.f);
    dl->AddRect(
        ImVec2(trackX, trackBarY), ImVec2(trackX + trackW, trackBarY + TRKH), cBorder, 2.f);

    float pct  = std::clamp(m_debugTimeline / axisT, 0.f, 1.f);
    float fillW = trackW * pct;
    // Spoiler-free: what was already watched past the playhead (after a scrub back), faintly.
    if (spoilersHidden && m_spoilerSeenT > m_debugTimeline)
    {
        const float seenW = trackW * std::clamp(m_spoilerSeenT / axisT, 0.f, 1.f);
        if (seenW > fillW + 1.f)
            dl->AddRectFilled(ImVec2(trackX + fillW, trackBarY), ImVec2(trackX + seenW, trackBarY + TRKH),
                              IM_COL32(200, 168, 75, 46), 2.f);
    }
    if (fillW > 2.f)
    {
        dl->AddRectFilledMultiColor(
            ImVec2(trackX, trackBarY), ImVec2(trackX + fillW, trackBarY + TRKH),
            cGoldDim, cGold, cGold, cGoldDim);
    }

    float hx   = std::clamp(trackX + fillW, trackX + 5.f * sf, trackX + trackW - 5.f * sf);
    float dotR = 5.f * sf;
    dl->AddCircleFilled(ImVec2(hx, trackMidY), dotR, cGoldBright);
    dl->AddCircle(ImVec2(hx, trackMidY), dotR, IM_COL32(255,255,220,100), 0, 1.5f);

    for (int m = 1; (float)m * 60.f <= axisT; m++)
    {
        float tx = trackX + trackW * ((float)m * 60.f / axisT);
        bool  maj = (m % 5 == 0);
        float th  = (maj ? 7.f : 5.f) * sf;
        dl->AddLine(
            ImVec2(tx, trackBarY + TRKH + 2.f * sf),
            ImVec2(tx, trackBarY + TRKH + 2.f * sf + th),
            maj ? cGoldDim : cBorder, 1.f);
    }

    m_annotationMgr.RenderTimelineMarkers(trackX, trackW,
                                           trackBarY, TRKH + 12.f * sf,
                                           axisT, m_debugTimeline,
                                           m_displayTimeOffset);

    // The key moments under the track while Highlights is on, in the target's team colour.
    DrawHighlightsTimelineMarkers(trackX, trackW, trackBarY, TRKH, axisT);

    // The clip editor's range and its two handles, before the scrub button so a handle wins
    // the mouse over the track under it.
    DrawClipTimelineRange(trackX, trackW, trackBarY, TRKH, axisT);

    // A point on the track is a moment on the axis; past the recording's end (only possible on
    // the spoiler-free axis) it is the end, which reveals the match.
    ImGui::SetCursorScreenPos(ImVec2(trackX, y0));
    ImGui::InvisibleButton("##Scrub", ImVec2(trackW, 20.f * sf));
    if (ImGui::IsItemActive())
    {
        float mouseX = ImGui::GetIO().MousePos.x;
        float t = (mouseX - trackX) / trackW;
        m_debugTimeline = std::min(std::clamp(t, 0.f, 1.f) * axisT, maxT);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        float mouseX = ImGui::GetIO().MousePos.x;
        m_scrubRightClickTime = std::min(std::clamp((mouseX - trackX) / trackW, 0.f, 1.f) * axisT, maxT);
        ImGui::OpenPopup("##ScrubCtx");
    }
    if (ImGui::BeginPopup("##ScrubCtx"))
    {
        if (ImGui::MenuItem("Add bookmark here"))
        {
            m_annotationMgr.AddBookmarkDirect((uint32_t)(m_scrubRightClickTime * 1000.f));
        }
        if (ImGui::MenuItem("Copy link at this moment", nullptr, false,
                            UrlProtocol::IsPublished(m_matchMeta.folder_name)))
            CopyShareLink(m_scrubRightClickTime);
        if (!UrlProtocol::IsPublished(m_matchMeta.folder_name) &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", UrlProtocol::kNotPublishedTip);
        ImGui::EndPopup();
    }

    // ── Frame badge (right of track) ─────────────────────────────────────
    {
        long frame = (long)(m_debugTimeline * 30.0);
        char fBuf[24];
        snprintf(fBuf, sizeof(fBuf), "F %ld", frame);
        float bx = trackX + trackW + 6.f * sf;
        float by = y0;
        float bh = 16.f * sf;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + badgeW, by + bh),
                          IM_COL32(0,0,0,100), 3.f);
        dl->AddRect(ImVec2(bx, by), ImVec2(bx + badgeW, by + bh), cBorder, 3.f);
        dl->AddText(font, fsSm,
            ImVec2(bx + 5.f * sf, by + (bh - fsSm) * 0.5f), cTextMid, fBuf);
    }

    // ══════════════════════════════════════════════════════════════════════
    // ROW 2: CONTROLS  (transport buttons centered, chip left, speed right)
    // ══════════════════════════════════════════════════════════════════════
    const float cy = y0 + ROW2_OFS;

    // One hover for every button in the bar: a soft rounded highlight, the same radius
    // everywhere, no border; the glyph brightens to near-white (cHoverFg).
    const float kBtnRound = 6.f * sf;
    const ImU32 cHoverFill = IM_COL32(255, 244, 222, 24);
    const ImU32 cHoverFg   = IM_COL32(248, 242, 230, 255);
    auto FillBtn = [&](float bx, float by, float bw, float bh, bool hov,
                       ImU32 bg0, ImU32 bdr0) {
        if ((bg0 >> 24) > 0)
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), bg0, kBtnRound);
        if (hov)
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), cHoverFill, kBtnRound);
        if ((bdr0 >> 24) > 0)
            dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), bdr0, kBtnRound);
    };
    // DEV TOOL (the comms self-test): where each button is, to hover it by code.
    m_playBarHot.clear();
    auto Hot = [&](const char* name) {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        m_playBarHot.push_back({name, ImVec4(a.x, a.y, b.x, b.y)});
    };

    auto Divider = [&](float dx) {
        dl->AddLine(ImVec2(dx, cy + 3.f * sf), ImVec2(dx, cy + BTN_H - 3.f * sf), cBorder);
    };

    // Pre-compute transport group width to center it
    const float bk30W = 40.f * sf, bk5W = 34.f * sf, bk1W = 34.f * sf;
    const float fw1W  = 34.f * sf, fw5W = 34.f * sf, fw30W = 40.f * sf;
    const float stopW = 24.f * sf;
    const float transportW = bk30W + BGAP + bk5W + BGAP + bk1W + GDIV + PLAY_SZ + BGAP + stopW
                           + GDIV + fw1W + BGAP + fw5W + BGAP + fw30W;
    // The centre group: the transport, the game volume (with voice comms attached its speaker
    // opens the mixer), Highlights. The right group: the speed, then the Share split button.
    const float hlW     = HighlightsButtonWidth();
    const float speedW  = 46.f * sf;
    const float volIconW = 24.f * sf;
    const float shareMainW = 74.f * sf, shareArrowW = 22.f * sf;
    const float shareW  = m_liveMode ? 0.f : shareMainW + shareArrowW;
    const float leftEnd = x0 + 66.f + 10.f * sf + 100.f * sf;   // the status chip and the stars
    const float rightStart = px + pw - PAD - shareW - (shareW > 0.f ? 10.f * sf : 0.f) - speedW;
    // Narrow windows: the volume track gives up width before anything overlaps.
    float volTrkW = 96.f * sf;
    auto GroupW = [&] { return transportW + GDIV + volIconW + 4.f * sf + volTrkW + GDIV + hlW; };
    {
        const float room = rightStart - 14.f * sf - (leftEnd + 12.f * sf);
        if (GroupW() > room) volTrkW = std::max(48.f * sf, volTrkW - (GroupW() - room));
    }
    const float groupW = GroupW();
    float transportX = O.x + (vpW - groupW) * 0.5f;
    transportX = std::min(transportX, rightStart - 14.f * sf - groupW);
    transportX = std::max(transportX, leftEnd + 12.f * sf);

    // ── Status chip (left-aligned) ───────────────────────────────────────
    {
        float chipW = 66.f;
        float chipX = x0;
        dl->AddRectFilled(ImVec2(chipX, cy), ImVec2(chipX + chipW, cy + BTN_H),
                          IM_COL32(0,0,0,76), 5.f);
        dl->AddRect(ImVec2(chipX, cy), ImVec2(chipX + chipW, cy + BTN_H), cBorder, 5.f);

        // Playing blinks green; stopped holds a steady red so the state reads at a glance.
        bool blinkOn = ((int)(ImGui::GetTime() / 0.65) % 2 == 0);
        ImU32 dotCol = ctx.isPlaying ? (blinkOn ? cGreen : cGreenDim) : cDanger;
        dl->AddCircleFilled(ImVec2(chipX + 8.f, cy + BTN_H*0.5f), 2.5f, dotCol);

        const char* stLabel = ctx.isPlaying ? "PLAYING" : "STOPPED";
        dl->PushClipRect(ImVec2(chipX, cy), ImVec2(chipX + chipW, cy + BTN_H), true);
        dl->AddText(font, fsSm,
            ImVec2(chipX + 16.f, cy + (BTN_H - fsSm)*0.5f), cTextDim, stLabel);
        dl->PopClipRect();
    }

    // ── Star rating (between status chip and transport) ──────────────────
    {
        float starX = x0 + 66.f + 10.f * sf;
        float starY = cy + (BTN_H - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(starX, starY));
        int cur = MatchRatings::Get().GetRating(m_matchMeta.folder_name);
        int res = DrawStarRating("##barRating", cur);
        if (res > 0)
            MatchRatings::Get().SetRating(m_matchMeta.folder_name, res);
        else if (res == -1)
            MatchRatings::Get().SetRating(m_matchMeta.folder_name, 0);
    }

    // ── Centered transport group ─────────────────────────────────────────
    float cx = transportX;

    auto DrawStepBtn = [&](const char* id, float bw, const char* label,
                           float stepSec, bool forward) {
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::InvisibleButton(id, ImVec2(bw, BTN_H));
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();
        Hot(id);
        FillBtn(cx, cy, bw, BTN_H, hov, 0, 0);

        ImU32 fg    = hov ? cHoverFg : cTextMid;
        float thick = 1.3f * sf;
        float rad   = 4.f * sf;

        if (!forward)
        {
            float icx = cx + bw - 10.f * sf;
            float icy = cy + BTN_H * 0.5f;
            dl->PathArcTo(ImVec2(icx, icy), rad, 270.f*d2r, (270.f+300.f)*d2r, 24);
            dl->PathStroke(fg, false, thick);
            float tipX = icx + rad - 0.5f*sf, tipY = icy - 1.f*sf;
            dl->AddLine(ImVec2(tipX, tipY), ImVec2(tipX-2.5f*sf, tipY-2.f*sf), fg, thick);
            dl->AddLine(ImVec2(tipX, tipY), ImVec2(tipX+0.8f*sf, tipY-2.5f*sf), fg, thick);
            dl->AddText(font, fsSm,
                ImVec2(cx + 4.f*sf, cy + (BTN_H - fsSm)*0.5f), fg, label);
        }
        else
        {
            float icx = cx + 10.f * sf;
            float icy = cy + BTN_H * 0.5f;
            dl->PathArcTo(ImVec2(icx, icy), rad, 30.f*d2r, (30.f+300.f)*d2r, 24);
            dl->PathStroke(fg, false, thick);
            float tipX = icx - rad + 0.5f*sf, tipY = icy - 1.f*sf;
            dl->AddLine(ImVec2(tipX, tipY), ImVec2(tipX+2.5f*sf, tipY-2.f*sf), fg, thick);
            dl->AddLine(ImVec2(tipX, tipY), ImVec2(tipX-0.8f*sf, tipY-2.5f*sf), fg, thick);
            dl->AddText(font, fsSm,
                ImVec2(cx + 16.f*sf, cy + (BTN_H - fsSm)*0.5f), fg, label);
        }

        if (clk) m_debugTimeline = std::clamp(m_debugTimeline + stepSec, 0.f, maxT);
        if (hov) ImGui::SetTooltip(forward ? "Forward %s" : "Back %s", label);
        cx += bw + BGAP;
    };

    DrawStepBtn("##Bk30", bk30W, "30s", -30.f, false);
    DrawStepBtn("##Bk5",  bk5W,  "5s",  -5.f,  false);
    DrawStepBtn("##Bk1",  bk1W,  "1s",  -1.f,  false);

    cx += GDIV;
    Divider(cx - GDIV * 0.5f);

    // ── Play / Pause ─────────────────────────────────────────────────────
    {
        float plaY = cy - (PLAY_SZ - BTN_H) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(cx, plaY));
        ImGui::InvisibleButton("##PlayPause", ImVec2(PLAY_SZ, PLAY_SZ));
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();
        Hot("##PlayPause");

        // The main button keeps its soft frame; the hover is the bar's one highlight on top.
        ImU32 fg  = hov ? cHoverFg : cGold;
        dl->AddRectFilled(ImVec2(cx, plaY), ImVec2(cx+PLAY_SZ, plaY+PLAY_SZ), IM_COL32(200,168,75, 18), kBtnRound);
        if (hov)
            dl->AddRectFilled(ImVec2(cx, plaY), ImVec2(cx+PLAY_SZ, plaY+PLAY_SZ), cHoverFill, kBtnRound);
        dl->AddRect(ImVec2(cx, plaY), ImVec2(cx+PLAY_SZ, plaY+PLAY_SZ), cBorderHi, kBtnRound);

        float pcx = cx + PLAY_SZ * 0.5f;
        float pcy = plaY + PLAY_SZ * 0.5f;

        if (!ctx.isPlaying)
        {
            dl->AddTriangleFilled(
                ImVec2(pcx - 4.f*sf, pcy - 5.f*sf),
                ImVec2(pcx + 5.f*sf, pcy),
                ImVec2(pcx - 4.f*sf, pcy + 5.f*sf), fg);
        }
        else
        {
            dl->AddRectFilled(
                ImVec2(pcx - 4.5f*sf, pcy - 5.f*sf),
                ImVec2(pcx - 1.5f*sf, pcy + 5.f*sf), fg);
            dl->AddRectFilled(
                ImVec2(pcx + 1.5f*sf, pcy - 5.f*sf),
                ImVec2(pcx + 4.5f*sf, pcy + 5.f*sf), fg);
        }

        if (clk) ctx.isPlaying = !ctx.isPlaying;
        if (hov) ImGui::SetTooltip(ctx.isPlaying ? "Pause" : "Play");
        cx += PLAY_SZ + BGAP;
    }

    // ── Stop ─────────────────────────────────────────────────────────────
    {
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::InvisibleButton("##Stop", ImVec2(stopW, BTN_H));
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();

        Hot("##Stop");
        ImU32 fg  = hov ? cHoverFg : cTextMid;
        FillBtn(cx, cy, stopW, BTN_H, hov, 0, 0);

        float sq  = 8.f * sf;
        float scx = cx + (stopW - sq) * 0.5f;
        float scy = cy + (BTN_H - sq) * 0.5f;
        dl->AddRectFilled(ImVec2(scx, scy), ImVec2(scx+sq, scy+sq), fg);

        if (clk) { m_debugTimeline = 0.f; ctx.isPlaying = false; }
        if (hov) ImGui::SetTooltip("Stop");
        cx += stopW + BGAP;
    }

    cx += GDIV;
    Divider(cx - GDIV * 0.5f);

    DrawStepBtn("##Fw1",  fw1W,  "1s",  +1.f,  true);
    DrawStepBtn("##Fw5",  fw5W,  "5s",  +5.f,  true);
    DrawStepBtn("##Fw30", fw30W, "30s", +30.f, true);

    // ── The game volume, right of the transport ──────────────────────────
    // The speaker mutes the game's sound and the track sets the master volume, always shown (the
    // track doubles as the mute indicator). With voice comms attached the speaker opens a small
    // mixer instead: the game and the comms, each with its mute and its volume. The speaker is
    // flat vector art in the transport glyphs' weight and colours.
    cx += GDIV;
    Divider(cx - GDIV * 0.5f);
    {
        const float volW = volIconW;
        const float trkW = volTrkW;
        AudioConfig* acfg = m_audioEngine ? &m_audioEngine->GetConfig() : nullptr;
        const bool mixer = CommsAttached();

        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::InvisibleButton("##Mute", ImVec2(volW, BTN_H));
        const bool hovIcon = ImGui::IsItemHovered();
        Hot("##Mute");
        if (ImGui::IsItemClicked())
        {
            if (mixer) ImGui::OpenPopup("##VolumeMixer");
            else m_audioEnabled = !m_audioEnabled;
        }
        const bool mixerOpen = mixer && ImGui::IsPopupOpen("##VolumeMixer");
        FillBtn(cx, cy, volW, BTN_H, hovIcon || mixerOpen, 0, 0);
        if (mixer) DrawVolumeMixer("##VolumeMixer", cx, cy - 6.f * sf);

        const ImU32 vfg   = (hovIcon || mixerOpen) ? cHoverFg : m_audioEnabled ? cGoldBright : cTextMid;
        const float thick = 1.3f * sf;
        const float ox    = cx + 5.f * sf, oy = cy + BTN_H * 0.5f;

        dl->AddRectFilled(ImVec2(ox, oy - 2.f * sf), ImVec2(ox + 3.2f * sf, oy + 2.f * sf), vfg);
        dl->AddTriangleFilled(ImVec2(ox + 7.2f * sf, oy - 4.6f * sf),
                              ImVec2(ox + 7.2f * sf, oy + 4.6f * sf),
                              ImVec2(ox + 1.6f * sf, oy), vfg);

        if (m_audioEnabled)
        {
            // A single wave arc keeps the on-state readable without crowding the glyph.
            dl->PathArcTo(ImVec2(ox + 7.2f * sf, oy), 3.4f * sf, -58.f * d2r, 58.f * d2r, 12);
            dl->PathStroke(vfg, false, thick);
        }
        else
        {
            // Struck through rather than swapped for a second glyph, so the shape stays
            // recognisable and the mute reads as a state on top of it.
            dl->AddLine(ImVec2(ox, oy - 5.5f * sf), ImVec2(ox + 11.f * sf, oy + 5.5f * sf),
                        vfg, thick);
        }
        // With the mixer: a small caret beside the speaker says it opens something.
        if (mixer)
            dl->AddTriangleFilled(ImVec2(ox + 13.f * sf, oy + 3.f * sf), ImVec2(ox + 17.f * sf, oy + 3.f * sf),
                                  ImVec2(ox + 15.f * sf, oy + 5.5f * sf), vfg);
        cx += volW + 4.f * sf;

        // Track, to the right of the icon.
        {
            const float trkX = cx;
            ImGui::SetCursorScreenPos(ImVec2(trkX, cy));
            ImGui::InvisibleButton("##Vol", ImVec2(trkW, BTN_H));
            const bool hovTrk = ImGui::IsItemHovered();
            const bool held   = ImGui::IsItemActive();
            m_volumeBarActive = held;

            if (held && acfg)
            {
                const float t = (ImGui::GetIO().MousePos.x - trkX) / trkW;
                acfg->master_volume = std::clamp(t, 0.f, 1.f);
                m_audioEnabled = true;      // grabbing the slider means you want to hear it
            }

            const float ty    = cy + BTN_H * 0.5f;
            const float vol   = acfg ? std::clamp(acfg->master_volume, 0.f, 1.f) : 0.f;
            const float frac  = m_audioEnabled ? vol : 0.f;
            const float tthick = 3.f * sf;
            const float kr = 6.f * sf;   // the knob stays inside the track's ends
            const float k0 = trkX + kr, k1 = trkX + trkW - kr;
            dl->AddLine(ImVec2(k0, ty), ImVec2(k1, ty), cTextDim, tthick);
            if (frac > 0.f)
                dl->AddLine(ImVec2(k0, ty), ImVec2(k0 + (k1 - k0) * frac, ty), cGoldBright, tthick);
            dl->AddCircleFilled(ImVec2(k0 + (k1 - k0) * frac, ty),
                                (hovTrk || held) ? 6.f * sf : 5.f * sf, (hovTrk || held) ? cHoverFg : cGoldBright);

            if (hovTrk || held)
                ImGui::SetTooltip("Game volume  %.0f%%", vol * 100.f);
            cx += trkW;
        }

        if (hovIcon)
            ImGui::SetTooltip("%s", mixer ? "Volume: the game and the voice comms" : m_audioEnabled ? "Mute" : "Unmute");
    }

    // Highlights: play only the key moments (ReplayWindow_Highlights.cpp), the bar's hover style.
    cx += GDIV;
    Divider(cx - GDIV * 0.5f);
    DrawHighlightsButton(cx, cy, BTN_H);
    Hot("##Highlights");
    cx += hlW;

    // ══════════════════════════════════════════════════════════════════════
    // RIGHT: the speed, then Share
    // ══════════════════════════════════════════════════════════════════════
    {
        const float speedX = rightStart;
        ImGui::SetCursorScreenPos(ImVec2(speedX, cy));
        ImGui::InvisibleButton("##Speed", ImVec2(speedW, BTN_H));
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();
        Hot("##Speed");
        const bool open = ImGui::IsPopupOpen("SpeedMenu");

        FillBtn(speedX, cy, speedW, BTN_H, hov || open, 0, cBorder);

        ImU32 sfg = (hov || open) ? cHoverFg : cTextMid;
        // While Highlights drives the clock, the speed it is playing at.
        const std::string hlLbl = HighlightsSpeedLabel();
        const char* sLbl = hlLbl.empty() ? speedLabels[ctx.speedIndex] : hlLbl.c_str();
        ImVec2 tsz = font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, sLbl);
        float tx = speedX + (speedW - tsz.x - 8.f*sf) * 0.5f;
        float ty = cy + (BTN_H - fsSm) * 0.5f;
        dl->AddText(font, fsSm, ImVec2(tx, ty), sfg, sLbl);

        float arX = tx + tsz.x + 3.f*sf;
        float arY = ty + fsSm * 0.4f;
        dl->AddTriangleFilled(
            ImVec2(arX, arY), ImVec2(arX + 4.f*sf, arY),
            ImVec2(arX + 2.f*sf, arY + 3.f*sf), sfg);

        if (clk) ImGui::OpenPopup("SpeedMenu");
        float popupH = speedCount * ImGui::GetFrameHeightWithSpacing() + 8.f;
        ImGui::SetNextWindowPos(ImVec2(speedX, cy - popupH));
        ImGui::SetNextWindowSizeConstraints(ImVec2(speedW, 0), ImVec2(speedW * 2.f, FLT_MAX));
        if (ImGui::BeginPopup("SpeedMenu"))
        {
            // Listed fastest-first: the popup opens upwards, so descending order puts the
            // slow speeds nearest the button they drop out of.
            for (int i = speedCount - 1; i >= 0; i--)
            {
                bool sel = (i == ctx.speedIndex);
                if (ImGui::Selectable(speedLabels[i], sel))
                {
                    ctx.speedIndex    = i;
                    ctx.playbackSpeed = speeds[i];
                }
            }
            ImGui::EndPopup();
        }
        if (hov) ImGui::SetTooltip("Playback speed");
    }

    // Share: one split button, an outlined pill. The main part copies a link to this moment with
    // this camera (ReplayWindow_ShareLink.cpp), dimmed for a match that is not published; the
    // arrow opens the other ways to share, the GIF clip among them. Each half lights on its own.
    if (!m_liveMode)
    {
        const float shareX = px + pw - PAD - shareW;
        const float arrowX = shareX + shareMainW;
        const bool published = UrlProtocol::IsPublished(m_matchMeta.folder_name);
        const float rr = BTN_H * 0.5f;

        ImGui::SetCursorScreenPos(ImVec2(shareX, cy));
        ImGui::InvisibleButton("##Share", ImVec2(shareMainW, BTN_H));
        const bool hovS = ImGui::IsItemHovered();
        Hot("##Share");
        if (ImGui::IsItemClicked() && published)
            CopyShareLink(m_debugTimeline);
        ImGui::SetCursorScreenPos(ImVec2(arrowX, cy));
        ImGui::InvisibleButton("##ShareMore", ImVec2(shareArrowW, BTN_H));
        const bool hovA = ImGui::IsItemHovered();
        Hot("##ShareMore");
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("##ShareMenu");
        const bool menuOpen = ImGui::IsPopupOpen("##ShareMenu") || m_shareMenuForced;

        dl->AddRectFilled(ImVec2(shareX, cy), ImVec2(arrowX + shareArrowW, cy + BTN_H), IM_COL32(0, 0, 0, 60), rr);
        if (hovS && published)
            dl->AddRectFilled(ImVec2(shareX, cy), ImVec2(arrowX, cy + BTN_H), cHoverFill, rr, ImDrawFlags_RoundCornersLeft);
        if (hovA || menuOpen)
            dl->AddRectFilled(ImVec2(arrowX, cy), ImVec2(arrowX + shareArrowW, cy + BTN_H), cHoverFill, rr, ImDrawFlags_RoundCornersRight);
        dl->AddRect(ImVec2(shareX, cy), ImVec2(arrowX + shareArrowW, cy + BTN_H), cBorderHi, rr);
        dl->AddLine(ImVec2(arrowX, cy + 6.f * sf), ImVec2(arrowX, cy + BTN_H - 6.f * sf), cBorder, 1.f);

        const ImU32 fgS = !published ? cTextDim : hovS ? cHoverFg : cTextMid;
        // Chain link: two rounded links, overlapping on the diagonal.
        const float lx = shareX + 12.f * sf, ly = cy + BTN_H * 0.5f;
        const float lw = 8.f * sf, lh = 5.f * sf, r = 2.5f * sf, th = 1.4f * sf;
        dl->AddRect(ImVec2(lx, ly - lh * 0.5f - 1.5f * sf), ImVec2(lx + lw, ly + lh * 0.5f - 1.5f * sf), fgS, r, 0, th);
        dl->AddRect(ImVec2(lx + lw * 0.55f, ly - lh * 0.5f + 1.5f * sf), ImVec2(lx + lw * 1.55f, ly + lh * 0.5f + 1.5f * sf), fgS, r, 0, th);
        dl->AddText(font, fs, ImVec2(lx + 19.f * sf, cy + (BTN_H - fs) * 0.5f), fgS, "Share");
        const ImU32 fgA = (hovA || menuOpen) ? cHoverFg : cTextMid;
        const float ax = arrowX + shareArrowW * 0.5f - 1.f * sf, ay = cy + BTN_H * 0.5f - 1.5f * sf;
        dl->AddTriangleFilled(ImVec2(ax - 3.5f * sf, ay), ImVec2(ax + 3.5f * sf, ay), ImVec2(ax, ay + 4.f * sf), fgA);

        if (hovS)
            ImGui::SetTooltip("%s", published ? "Copy a link to this moment, with this camera" : UrlProtocol::kNotPublishedTip);
        else if (hovA && !menuOpen)
            ImGui::SetTooltip("More ways to share");

        // The menu opens upwards, its right edge on the button's.
        if (m_shareMenuForced && !ImGui::IsPopupOpen("##ShareMenu")) ImGui::OpenPopup("##ShareMenu");
        ImGui::SetNextWindowPos(ImVec2(arrowX + shareArrowW, cy - 6.f * sf), ImGuiCond_Always, ImVec2(1.f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 8.f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 6.f));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.06f, 0.04f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.78f, 0.66f, 0.29f, 0.40f));
        const bool shareMenu = ImGui::BeginPopup("##ShareMenu");
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(4);
        if (shareMenu)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.91f, 0.87f, 0.78f, 1.f));
            if (ImGui::MenuItem("Copy link to this moment", nullptr, false, published))
                CopyShareLink(m_debugTimeline);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", published ? "With this camera and the selected player" : UrlProtocol::kNotPublishedTip);
            if (ImGui::MenuItem("Copy link from match start", nullptr, false, published))
                CopyShareLink(0.f, true);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", published ? "The match from 0:00, with the default camera" : UrlProtocol::kNotPublishedTip);
            ImGui::Separator();
            if (ImGui::MenuItem("Make a clip (GIF)...", "Alt+X", false, !IsClipRendering()) && !IsClipEditorOpen())
                ToggleClipEditor();
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
    }

    // ── Comms lane (under the controls, while a recording is attached) ───
    if (CommsLaneVisible())
        DrawCommsLane(x0, pw - PAD * 2.f, trackX, trackW, O.y + 72.f * sf, barH - 72.f * sf - 8.f * sf, axisT);

    m_debugTimeline = std::clamp(m_debugTimeline, 0.f, maxT);

    // ── Bookmark list (movable panel; also hosts the create/rename popups)
    m_annotationMgr.RenderBookmarkList(m_debugTimeline, m_debugTimeline,
                                       m_replayCtx.isPlaying,
                                       &m_panelLayout);

    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
}
