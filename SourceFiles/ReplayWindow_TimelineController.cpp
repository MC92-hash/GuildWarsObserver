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


void ReplayWindow::DrawTimelineController()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float vpW = vp->Size.x;
    const float vpH = vp->Size.y;

    const float sf = 1.f;

    const float barH     = 76.f;
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

    static const float  speeds[]      = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 4.0f, 8.0f };
    static const char*  speedLabels[] = { "0.25x","0.5x","0.75x","1x","1.5x","2x","4x","8x" };
    constexpr int       speedCount    = 8;

    const float x0 = px + PAD;
    const float y0 = py + PADY;

    // ══════════════════════════════════════════════════════════════════════
    // ROW 1: SCRUBBER
    // ══════════════════════════════════════════════════════════════════════
    {
        char curBuf[16], totBuf[16];
        FormatTime(m_debugTimeline - m_displayTimeOffset, curBuf, sizeof(curBuf));
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

    float pct  = maxT > 0.f ? std::clamp(m_debugTimeline / maxT, 0.f, 1.f) : 0.f;
    float fillW = trackW * pct;
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

    for (int m = 1; (float)m * 60.f <= maxT; m++)
    {
        float tx = trackX + trackW * ((float)m * 60.f / maxT);
        bool  maj = (m % 5 == 0);
        float th  = (maj ? 7.f : 5.f) * sf;
        dl->AddLine(
            ImVec2(tx, trackBarY + TRKH + 2.f * sf),
            ImVec2(tx, trackBarY + TRKH + 2.f * sf + th),
            maj ? cGoldDim : cBorder, 1.f);
    }

    m_annotationMgr.RenderTimelineMarkers(trackX, trackW,
                                           trackBarY, TRKH + 12.f * sf,
                                           maxT, m_debugTimeline,
                                           m_displayTimeOffset);

    ImGui::SetCursorScreenPos(ImVec2(trackX, y0));
    ImGui::InvisibleButton("##Scrub", ImVec2(trackW, 20.f * sf));
    if (ImGui::IsItemActive())
    {
        float mouseX = ImGui::GetIO().MousePos.x;
        float t = (mouseX - trackX) / trackW;
        m_debugTimeline = std::clamp(t, 0.f, 1.f) * maxT;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        float mouseX = ImGui::GetIO().MousePos.x;
        m_scrubRightClickTime = std::clamp((mouseX - trackX) / trackW, 0.f, 1.f) * maxT;
        ImGui::OpenPopup("##ScrubCtx");
    }
    if (ImGui::BeginPopup("##ScrubCtx"))
    {
        if (ImGui::MenuItem("Add bookmark here"))
        {
            m_annotationMgr.AddBookmarkDirect((uint32_t)(m_scrubRightClickTime * 1000.f));
        }
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

    auto FillBtn = [&](float bx, float by, float bw, float bh, bool hov,
                       ImU32 bg0, ImU32 bdr0) {
        ImU32 bg  = hov ? cGoldFill : bg0;
        ImU32 bdr = hov ? cBorderHi : bdr0;
        if ((bg >> 24) > 0)
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), bg, 5.f);
        if ((bdr >> 24) > 0)
            dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), bdr, 5.f);
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
    const float transportX = O.x + (vpW - transportW) * 0.5f;

    // ── Status chip (left-aligned) ───────────────────────────────────────
    {
        float chipW = 66.f;
        float chipX = x0;
        dl->AddRectFilled(ImVec2(chipX, cy), ImVec2(chipX + chipW, cy + BTN_H),
                          IM_COL32(0,0,0,76), 5.f);
        dl->AddRect(ImVec2(chipX, cy), ImVec2(chipX + chipW, cy + BTN_H), cBorder, 5.f);

        bool blinkOn = ((int)(ImGui::GetTime() / 0.65) % 2 == 0);
        ImU32 dotCol = ctx.isPlaying ? (blinkOn ? cGreen : cGreenDim) : cTextDim;
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
        FillBtn(cx, cy, bw, BTN_H, hov, 0, 0);

        ImU32 fg    = hov ? cGoldBright : cTextMid;
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

        ImU32 bg  = hov ? IM_COL32(200,168,75, 33) : IM_COL32(200,168,75, 18);
        ImU32 bdr = hov ? cBorderGlow : cBorderHi;
        ImU32 fg  = hov ? cGoldBright : cGold;

        dl->AddRectFilled(ImVec2(cx, plaY), ImVec2(cx+PLAY_SZ, plaY+PLAY_SZ), bg, 6.f);
        dl->AddRect(ImVec2(cx, plaY), ImVec2(cx+PLAY_SZ, plaY+PLAY_SZ), bdr, 6.f);

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

        ImU32 fg  = hov ? cDanger : cTextMid;
        ImU32 bdr = hov ? IM_COL32(192,80,74,100) : IM_COL32(0,0,0,0);
        FillBtn(cx, cy, stopW, BTN_H, hov, 0, bdr);

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

    // ── Loop (right of transport) ────────────────────────────────────────
    cx += GDIV;
    Divider(cx - GDIV * 0.5f);
    {
        float loopW = 46.f * sf;
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::InvisibleButton("##Loop", ImVec2(loopW, BTN_H));
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();

        bool on   = ctx.loopPlayback;
        ImU32 fg  = (hov || on) ? cGoldBright : cTextMid;
        ImU32 bg  = on ? cGoldFill  : IM_COL32(0,0,0,0);
        ImU32 bdr = on ? cBorderHi  : IM_COL32(0,0,0,0);
        FillBtn(cx, cy, loopW, BTN_H, hov, bg, bdr);

        float thick = 1.3f * sf;
        float ox = cx + 4.f*sf, oy = cy + BTN_H * 0.5f;
        dl->AddLine(ImVec2(ox,          oy-3.f*sf), ImVec2(ox+8.f*sf, oy-3.f*sf), fg, thick);
        dl->AddLine(ImVec2(ox+6.f*sf,   oy-6.f*sf), ImVec2(ox+8.f*sf, oy-3.f*sf), fg, thick);
        dl->AddLine(ImVec2(ox+6.f*sf,   oy-0.f*sf), ImVec2(ox+8.f*sf, oy-3.f*sf), fg, thick);
        dl->AddLine(ImVec2(ox+8.f*sf,   oy+3.f*sf), ImVec2(ox,        oy+3.f*sf), fg, thick);
        dl->AddLine(ImVec2(ox+2.f*sf,   oy+0.f*sf), ImVec2(ox,        oy+3.f*sf), fg, thick);
        dl->AddLine(ImVec2(ox+2.f*sf,   oy+6.f*sf), ImVec2(ox,        oy+3.f*sf), fg, thick);

        dl->AddText(font, fsSm,
            ImVec2(cx + 17.f*sf, cy + (BTN_H - fsSm)*0.5f), fg, "Loop");

        if (clk) ctx.loopPlayback = !ctx.loopPlayback;
        if (hov) ImGui::SetTooltip(ctx.loopPlayback ? "Loop: ON" : "Loop: OFF");
        cx += loopW;
    }

    // Bookmarks are driven entirely from the ribbon toolbar (Add Bookmark /
    // Bookmarks) and the scrub track's right-click menu, so the play bar
    // carries no bookmark buttons of its own.

    // Auto Camera and Top View live in the ribbon toolbar; the play bar
    // keeps only transport and speed.

    // ── Speed (right-aligned) ────────────────────────────────────────────
    {
        float speedW = 54.f * sf;
        float speedX = px + pw - PAD - speedW;

        dl->AddText(font, fsSm,
            ImVec2(speedX - 34.f*sf, cy + (BTN_H - fsSm)*0.5f + 1.f), cTextDim, "SPEED");

        ImGui::SetCursorScreenPos(ImVec2(speedX, cy));
        ImGui::InvisibleButton("##Speed", ImVec2(speedW, BTN_H));
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();

        FillBtn(speedX, cy, speedW, BTN_H, hov, 0, cBorderHi);

        ImU32 sfg = hov ? cGoldBright : cTextMid;
        const char* sLbl = speedLabels[ctx.speedIndex];
        ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, sLbl);
        float tx = speedX + (speedW - tsz.x - 8.f*sf) * 0.5f;
        float ty = cy + (BTN_H - fs) * 0.5f;
        dl->AddText(font, fs, ImVec2(tx, ty), sfg, sLbl);

        float arX = tx + tsz.x + 3.f*sf;
        float arY = ty + fs * 0.35f;
        dl->AddTriangleFilled(
            ImVec2(arX, arY), ImVec2(arX + 4.f*sf, arY),
            ImVec2(arX + 2.f*sf, arY + 3.f*sf), sfg);

        if (clk) ImGui::OpenPopup("SpeedMenu");
        float popupH = speedCount * ImGui::GetFrameHeightWithSpacing() + 8.f;
        ImGui::SetNextWindowPos(ImVec2(speedX, cy - popupH));
        ImGui::SetNextWindowSizeConstraints(ImVec2(speedW, 0), ImVec2(speedW * 2.f, FLT_MAX));
        if (ImGui::BeginPopup("SpeedMenu"))
        {
            for (int i = 0; i < speedCount; i++)
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

    m_debugTimeline = std::clamp(m_debugTimeline, 0.f, maxT);

    // ── Bookmark list (movable panel; also hosts the create/rename popups)
    m_annotationMgr.RenderBookmarkList(m_debugTimeline, m_debugTimeline,
                                       m_replayCtx.isPlaying,
                                       &m_panelLayout);

    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
}
