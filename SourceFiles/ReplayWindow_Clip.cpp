#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "ClipGifEncoder.h"
#include "TeamColors.h"
#include "RunLog.h"
#include "CursorSystem.h"
#include <imgui_internal.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// Clip export: a looping GIF of a part of the screen over a time range, made for Discord.
//
// The user opens the editor from the play bar's Clip button (or Alt+X), frames a box over the
// view, picks a start and an end on the play bar's track, and exports. The clip is RENDERED, not
// recorded: every frame is drawn at an exact replay time on a fixed clock (the StepTimer's manual
// step), read back from the back buffer and kept, then a worker thread encodes the GIF. Spec:
// gwobserver-private docs/CLIP_EXPORT_PLAN.md.
// ---------------------------------------------------------------------------

using namespace DirectX;
using Phase = ClipExportState::Phase;

namespace
{
    constexpr const char* kClipProgressWindow = "##ClipProgress";
    constexpr const char* kClipBoxWindow      = "##ClipBoxFrame";

    // Discord's upload limit for accounts without Nitro. Above it a clip still exports, but most
    // people cannot post it there.
    constexpr int kDiscordLimitMb = 10;

    constexpr int   kFpsChoices[]   = {10, 15, 20, 25, 30};
    constexpr int   kScaleChoices[] = {100, 75, 50};
    constexpr int   kLimitChoices[] = {10, 25, 50, 0};
    constexpr float kPrerollSeconds = 2.f;     // particles and cross-fades already in flight on frame 0
    constexpr float kMinClipSeconds = 0.5f;
    constexpr float kMaxClipSeconds = 60.f;
    constexpr float kMinBoxPx       = 64.f;
    constexpr float kFollowHeight   = 50.f;    // world units above the feet the follow box centres on
    constexpr float kFollowSmooth   = 0.12f;   // SmoothDamp time: settled in about 0.25 s
    constexpr double kMaxFrameBytes = 1.5e9;   // frames are kept in memory until the encode
    constexpr double kPerFrameBytes = 600.0;   // GIF block overhead per frame, for the estimate
    // Measured 2026-10-16: 0.10 (a quiet, flat scene) to 0.40 (a fight with the camera moving). The
    // estimate never assumes a scene quieter than a fight, so a green meter means the clip fits.
    constexpr double kMinBytesPerPixelFrame = 0.40;
    constexpr float kHeadroom       = 0.85f;   // aim 15 % under the limit: scenes vary that much around the estimate

    enum ClipDrag { kDragLeft = 1, kDragRight = 2, kDragTop = 4, kDragBottom = 8, kDragMove = 16 };

    // GW1 dark-glass gold, as the play bar.
    const ImU32 cGold       = IM_COL32(200, 168,  75, 255);
    const ImU32 cGoldBright = IM_COL32(226, 194, 106, 255);
    const ImU32 cText       = IM_COL32(232, 223, 200, 255);

    std::filesystem::path ClipSettingsPath()
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto dir = std::filesystem::path(exePath).parent_path() / "settings";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir / "clip_export.json";
    }

    std::string ToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }

    std::wstring FromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
        return w;
    }

    // "12:30.4": the timeline's MM:SS with tenths, for the range readout.
    std::string FormatTenths(float seconds)
    {
        seconds = std::max(0.f, seconds);
        int tenths = static_cast<int>(std::lround(seconds * 10.f));
        int s = tenths / 10;
        return std::format("{:02}:{:02}.{}", s / 60, s % 60, tenths % 10);
    }

    std::string FormatMb(double bytes)
    {
        return std::format("{:.1f} MB", bytes / (1024.0 * 1024.0));
    }

    // Unity's SmoothDamp: a critically damped spring that stays stable at any step, so the follow
    // box moves the same at 10 fps as at 60.
    float SmoothDamp(float cur, float target, float& vel, float smoothTime, float dt)
    {
        const float omega = 2.f / smoothTime;
        const float x = omega * dt;
        const float e = 1.f / (1.f + x + 0.48f * x * x + 0.235f * x * x * x);
        const float change = cur - target;
        const float temp = (vel + omega * change) * dt;
        vel = (vel - omega * temp) * e;
        return target + (change + temp) * e;
    }

    std::string SanitizeForFileName(std::string s)
    {
        for (char& c : s)
        {
            if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
                c == '?' || c == '*' || c == ' ' || static_cast<unsigned char>(c) < 32)
                c = '-';
        }
        std::string out;
        for (char c : s)
            if (!(c == '-' && !out.empty() && out.back() == '-')) out.push_back(c);
        while (!out.empty() && out.back() == '-') out.pop_back();
        while (!out.empty() && out.front() == '-') out.erase(out.begin());
        return out;
    }

    // Puts the file itself on the clipboard (CF_HDROP), the way Explorer's Copy does, so Ctrl+V in
    // Discord attaches the GIF rather than pasting its path.
    bool CopyFileToClipboard(HWND owner, const std::wstring& path)
    {
        const size_t chars = path.size() + 2;   // the list ends with an empty string
        HGLOBAL mem = GlobalAlloc(GHND, sizeof(DROPFILES) + chars * sizeof(wchar_t));
        if (!mem) return false;
        auto* df = static_cast<DROPFILES*>(GlobalLock(mem));
        if (!df) { GlobalFree(mem); return false; }
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        auto* list = reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES));
        memcpy(list, path.c_str(), path.size() * sizeof(wchar_t));
        GlobalUnlock(mem);

        if (!OpenClipboard(owner)) { GlobalFree(mem); return false; }
        EmptyClipboard();
        if (!SetClipboardData(CF_HDROP, mem))
        {
            CloseClipboard();
            GlobalFree(mem);
            return false;
        }
        CloseClipboard();
        return true;
    }

    void PushClipPanelStyle()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f, 10.f));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.f);   // opaque even when the replay UI is faded
        ImGui::PushStyleColor(ImGuiCol_WindowBg,         ImVec4(0.07f, 0.06f, 0.04f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border,           ImVec4(0.78f, 0.66f, 0.29f, 0.32f));
        ImGui::PushStyleColor(ImGuiCol_TitleBg,          ImVec4(0.10f, 0.08f, 0.05f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,    ImVec4(0.16f, 0.13f, 0.07f, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Text,             ImVec4(0.91f, 0.87f, 0.78f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled,     ImVec4(0.60f, 0.54f, 0.41f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,          ImVec4(0.f, 0.f, 0.f, 0.40f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   ImVec4(0.78f, 0.66f, 0.29f, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    ImVec4(0.78f, 0.66f, 0.29f, 0.28f));
        ImGui::PushStyleColor(ImGuiCol_Button,           ImVec4(0.78f, 0.66f, 0.29f, 0.14f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,    ImVec4(0.78f, 0.66f, 0.29f, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,     ImVec4(0.78f, 0.66f, 0.29f, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,        ImVec4(0.89f, 0.76f, 0.42f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Header,           ImVec4(0.78f, 0.66f, 0.29f, 0.14f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,    ImVec4(0.78f, 0.66f, 0.29f, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg,          ImVec4(0.07f, 0.06f, 0.04f, 0.97f));
        ImGui::PushStyleColor(ImGuiCol_Separator,        ImVec4(0.78f, 0.66f, 0.29f, 0.22f));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,    ImVec4(0.78f, 0.66f, 0.29f, 0.85f));
    }
    constexpr int kClipPanelColours = 18;
    constexpr int kClipPanelVars = 4;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void ReplayWindow::LoadClipSettings()
{
    m_clip.settingsLoaded = true;
    std::ifstream f(ClipSettingsPath());
    if (!f.is_open()) return;
    try
    {
        nlohmann::json j = nlohmann::json::parse(f);
        auto& c = m_clip;
        c.lastFolder      = FromUtf8(j.value("last_folder", std::string()));
        c.fps             = j.value("fps", c.fps);
        c.sizeLimitMb     = j.value("size_limit_mb", c.sizeLimitMb);
        c.outputScalePct  = j.value("output_scale_pct", c.outputScalePct);
        c.tagEnabled      = j.value("tag_enabled", c.tagEnabled);
        c.copyToClipboard = j.value("copy_to_clipboard", c.copyToClipboard);
        c.bytesPerPixelFrame = j.value("bytes_per_pixel_frame", c.bytesPerPixelFrame);
        const std::string corner = j.value("tag_corner", std::string("bottom-left"));
        c.tagCorner = corner == "bottom-right" ? 1 : corner == "top-left" ? 2 : corner == "top-right" ? 3 : 0;
        if (j.contains("last_box") && j["last_box"].is_array() && j["last_box"].size() == 4)
            for (int i = 0; i < 4; i++) c.box[i] = j["last_box"][i].get<float>();

        if (std::find(std::begin(kFpsChoices), std::end(kFpsChoices), c.fps) == std::end(kFpsChoices)) c.fps = 20;
        if (std::find(std::begin(kScaleChoices), std::end(kScaleChoices), c.outputScalePct) == std::end(kScaleChoices))
            c.outputScalePct = 100;
        if (std::find(std::begin(kLimitChoices), std::end(kLimitChoices), c.sizeLimitMb) == std::end(kLimitChoices))
            c.sizeLimitMb = kDiscordLimitMb;
        c.bytesPerPixelFrame = std::clamp(c.bytesPerPixelFrame, kMinBytesPerPixelFrame, 1.5);
        c.box[2] = std::clamp(c.box[2], 0.05f, 1.f);
        c.box[3] = std::clamp(c.box[3], 0.05f, 1.f);
        c.box[0] = std::clamp(c.box[0], 0.f, 1.f - c.box[2]);
        c.box[1] = std::clamp(c.box[1], 0.f, 1.f - c.box[3]);
    }
    catch (...) {}
}

void ReplayWindow::SaveClipSettings() const
{
    const auto& c = m_clip;
    static const char* kCorners[] = {"bottom-left", "bottom-right", "top-left", "top-right"};
    nlohmann::json j;
    j["last_folder"]           = ToUtf8(c.lastFolder);
    j["fps"]                   = c.fps;
    j["size_limit_mb"]         = c.sizeLimitMb;
    j["output_scale_pct"]      = c.outputScalePct;
    j["tag_enabled"]           = c.tagEnabled;
    j["tag_corner"]            = kCorners[std::clamp(c.tagCorner, 0, 3)];
    j["copy_to_clipboard"]     = c.copyToClipboard;
    j["bytes_per_pixel_frame"] = c.bytesPerPixelFrame;
    j["last_box"]              = {c.box[0], c.box[1], c.box[2], c.box[3]};
    std::ofstream f(ClipSettingsPath());
    if (f.is_open()) f << j.dump(2);
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

void ReplayWindow::ToggleClipEditor()
{
    if (m_clip.phase == Phase::Closed)
        OpenClipEditor();
    else if (m_clip.phase == Phase::Editing)
        CloseClipEditor();
}

void ReplayWindow::OpenClipEditor()
{
    if (m_loadingPhase != LoadingPhase::Ready) return;
    if (!m_clip.settingsLoaded) LoadClipSettings();

    auto& c = m_clip;
    m_replayCtx.isPlaying = false;

    const float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
    c.startTime = std::max(0.f, m_debugTimeline - 4.f);
    c.endTime   = std::min(maxT, c.startTime + 8.f);
    c.startTime = std::max(0.f, c.endTime - 8.f);

    // The camera's subject is the natural one to follow; otherwise the first player of the match.
    c.followAgentId = -1;
    auto it = m_replayCtx.agents.find(m_followedAgentId);
    if (it != m_replayCtx.agents.end() && it->second.type == AgentType::Player)
        c.followAgentId = m_followedAgentId;
    else if (!m_team1PlayerIds.empty())
        c.followAgentId = m_team1PlayerIds.front();
    else if (!m_team2PlayerIds.empty())
        c.followAgentId = m_team2PlayerIds.front();
    c.followPlayer = false;   // owner: a new clip starts with a box that stays put
    c.followPosValid = false;
    c.previewing = false;
    c.boxDrag = 0;
    c.timelineDrag = 0;
    c.phase = Phase::Editing;
}

void ReplayWindow::CloseClipEditor()
{
    if (m_clip.phase != Phase::Editing) return;
    if (m_clip.previewing) m_replayCtx.isPlaying = false;
    m_clip.previewing = false;
    m_clip.phase = Phase::Closed;
    SaveClipSettings();
}

// ---------------------------------------------------------------------------
// Keys: Alt+X opens and closes the editor, Escape closes it or cancels the export. Run every
// frame before the replay's own hotkeys, which skip Escape when it was used here.
// ---------------------------------------------------------------------------

void ReplayWindow::UpdateClipHotkeys(bool processActive)
{
    auto& c = m_clip;
    c.escConsumed = false;
    // Keys typed in the Save dialog are this process's too: leave them to the dialog.
    if (c.dialogOpen) processActive = false;

    const bool esc  = processActive && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool alt  = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;   // Alt Gr is Ctrl + Alt
    const bool altX = processActive && alt && !ctrl && (GetAsyncKeyState('X') & 0x8000) != 0;
    const bool escPressed  = esc && !c.escDown;
    const bool altXPressed = altX && !c.altXDown;
    c.escDown = esc;
    c.altXDown = altX;

    const bool typing = ImGui::GetIO().WantTextInput;
    if (altXPressed && !typing)
        ToggleClipEditor();

    if (escPressed)
    {
        if (c.phase == Phase::Rendering)
        {
            AbortClipRender();
            c.escConsumed = true;
        }
        else if (c.phase == Phase::Encoding)
        {
            c.cancel = true;
            c.escConsumed = true;
        }
        else if (c.phase == Phase::Editing && !typing &&
                 !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        {
            CloseClipEditor();
            c.escConsumed = true;
        }
    }

    // During the render ImGui sees no mouse (no hover or tooltip may land in a frame), so the
    // progress window's Cancel button is hit-tested here on the real cursor.
    const bool lmb = processActive && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (c.phase == Phase::Rendering && lmb && !c.clickDown)
    {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(m_hwnd, &p);
        if (p.x >= c.cancelMin.x && p.x <= c.cancelMax.x && p.y >= c.cancelMin.y && p.y <= c.cancelMax.y)
            AbortClipRender();
    }
    c.clickDown = lmb;
}

// ---------------------------------------------------------------------------
// Playback: the preview loop, and the export's exact frame times. Runs in Tick right after the
// playback engine has advanced the timeline.
// ---------------------------------------------------------------------------

void ReplayWindow::UpdateClipPlayback()
{
    auto& c = m_clip;
    if (c.phase == Phase::Rendering)
    {
        m_debugTimeline = std::max(0.f, c.startTime + static_cast<float>(c.frameIndex) / c.fps);
        c.captureThisFrame = c.frameIndex >= 0;
        return;
    }
    c.captureThisFrame = false;

    if (c.phase == Phase::Editing && c.previewing)
    {
        if (!m_replayCtx.isPlaying)
            c.previewing = false;
        else if (m_debugTimeline >= c.endTime || m_debugTimeline < c.startTime - 0.5f)
            m_debugTimeline = c.startTime;
    }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

bool ReplayWindow::ClipAgentScreenPos(int agentId, ImVec2& out) const
{
    auto it = m_replayCtx.agents.find(agentId);
    if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) return false;
    Camera* cam = m_mapRenderer ? m_mapRenderer->GetCamera() : nullptr;
    if (!cam) return false;

    float sx, sy, sz;
    InterpolateAgentPosition(it->second, m_debugTimeline, m_replayCtx.interpSettings, sx, sy, sz);
    XMFLOAT3 p = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);
    p.y += kFollowHeight;

    const D3D11_VIEWPORT vp = m_deviceResources->GetScreenViewport();
    float x, y;
    if (!ProjectToScreen(cam->GetView() * cam->GetProj(), vp.Width, vp.Height, p, x, y)) return false;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    out = ImVec2(x * disp.x / std::max(1.f, vp.Width), y * disp.y / std::max(1.f, vp.Height));
    return true;
}

void ReplayWindow::UpdateClipFollow(float dt)
{
    auto& c = m_clip;
    if (!c.followPlayer) { c.followPosValid = false; return; }
    ImVec2 target;
    if (!ClipAgentScreenPos(c.followAgentId, target)) return;   // out of view: hold where it is
    if (!c.followPosValid)
    {
        c.followPos = target;
        c.followVel = ImVec2(0.f, 0.f);
        c.followPosValid = true;
        return;
    }
    dt = std::clamp(dt, 0.f, 0.25f);
    c.followPos.x = SmoothDamp(c.followPos.x, target.x, c.followVel.x, kFollowSmooth, dt);
    c.followPos.y = SmoothDamp(c.followPos.y, target.y, c.followVel.y, kFollowSmooth, dt);
}

// The capture box in ImGui display coordinates. Fixed: the stored fractions. Following: the same
// size, centred on the followed player and held inside the view.
void ReplayWindow::ClipBoxRect(ImVec2& mn, ImVec2& mx) const
{
    const auto& c = m_clip;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    float w = c.box[2] * disp.x, h = c.box[3] * disp.y;
    if (c.phase == Phase::Rendering && c.boxW > 0)
    {
        // The export's size is fixed in back-buffer pixels; turn it back into display units.
        const RECT o = m_deviceResources->GetOutputSize();
        w = c.boxW * disp.x / std::max(1.f, static_cast<float>(o.right - o.left));
        h = c.boxH * disp.y / std::max(1.f, static_cast<float>(o.bottom - o.top));
    }
    float x = c.box[0] * disp.x, y = c.box[1] * disp.y;
    if (c.followPlayer && c.followPosValid)
    {
        x = c.followPos.x - w * 0.5f;
        y = c.followPos.y - h * 0.5f;
    }
    x = std::clamp(x, 0.f, std::max(0.f, disp.x - w));
    y = std::clamp(y, 0.f, std::max(0.f, disp.y - h));
    mn = ImVec2(std::floor(x), std::floor(y));
    mx = ImVec2(mn.x + std::round(w), mn.y + std::round(h));
}

void ReplayWindow::ClipOutputSize(int& boxW, int& boxH, int& outW, int& outH) const
{
    ImVec2 mn, mx;
    ClipBoxRect(mn, mx);
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const RECT o = m_deviceResources->GetOutputSize();
    const float sx = (o.right - o.left) / std::max(1.f, disp.x);
    const float sy = (o.bottom - o.top) / std::max(1.f, disp.y);
    boxW = std::clamp(static_cast<int>(std::lround((mx.x - mn.x) * sx)), 16, std::max(16, static_cast<int>(o.right - o.left)));
    boxH = std::clamp(static_cast<int>(std::lround((mx.y - mn.y) * sy)), 16, std::max(16, static_cast<int>(o.bottom - o.top)));
    outW = std::max(16, static_cast<int>(std::lround(boxW * m_clip.outputScalePct / 100.0)));
    outH = std::max(16, static_cast<int>(std::lround(boxH * m_clip.outputScalePct / 100.0)));
}

double ReplayWindow::EstimateClipBytes(int fps, int scalePct, float seconds) const
{
    int bw, bh, ow, oh;
    ClipOutputSize(bw, bh, ow, oh);
    ow = std::max(16, static_cast<int>(std::lround(bw * scalePct / 100.0)));
    oh = std::max(16, static_cast<int>(std::lround(bh * scalePct / 100.0)));
    const double frames = std::max(1.0, std::round(static_cast<double>(seconds) * fps));
    return static_cast<double>(ow) * oh * frames * m_clip.bytesPerPixelFrame + frames * kPerFrameBytes + 2048.0;
}

// ---------------------------------------------------------------------------
// The corner tag: who played, when, where, and the match time of the clip's start
// ---------------------------------------------------------------------------

std::pair<std::string, std::string> ReplayWindow::ClipGuildNameAndTag(int side) const
{
    const std::string& folderTag = side == 1 ? m_folderTag1 : m_folderTag2;
    if (!folderTag.empty())
        for (const auto& [id, g] : m_matchMeta.guilds)
            if (g.tag == folderTag) return {g.name, g.tag};

    const std::string& header = side == 1 ? m_team1GuildHeader : m_team2GuildHeader;
    const size_t open = header.rfind(" [");
    if (open != std::string::npos && !header.empty() && header.back() == ']')
        return {header.substr(0, open), header.substr(open + 2, header.size() - open - 3)};
    return {header.empty() ? (side == 1 ? "Blue" : "Red") : header, folderTag};
}

void ReplayWindow::DrawClipMatchTag(ImDrawList* dl, ImVec2 mn, ImVec2 mx) const
{
    const auto& c = m_clip;
    if (!c.tagEnabled) return;

    ImFont* font = ImGui::GetFont();
    const float boxW = mx.x - mn.x, boxH = mx.y - mn.y;
    // Small: about 3 % of the box height, 10 to 15 px, so it labels the clip without covering it.
    const float fs = std::clamp(boxH * 0.03f, 10.f, 15.f);
    const float pad = fs * 0.35f;
    const float margin = fs * 0.4f;
    const float maxTextW = boxW - 2.f * (pad + margin);
    if (maxTextW < 20.f) return;

    auto width = [&](const std::string& s) { return font->CalcTextSizeA(fs, FLT_MAX, 0.f, s.c_str()).x; };

    // Line 1: the two guilds in their team colours. Too wide: tags only.
    auto [name1, tag1] = ClipGuildNameAndTag(1);
    auto [name2, tag2] = ClipGuildNameAndTag(2);
    auto label = [](const std::string& name, const std::string& tag) {
        return tag.empty() ? name : name + " [" + tag + "]";
    };
    std::string left = label(name1, tag1), right = label(name2, tag2);
    const std::string vs = "  vs  ";
    if (width(left + vs + right) > maxTextW && !tag1.empty() && !tag2.empty())
    {
        left = "[" + tag1 + "]";
        right = "[" + tag2 + "]";
    }

    // Line 2: date, event type, map, match time. Too wide: the map goes first, then the event.
    std::string date;
    if (m_matchMeta.year > 0)
        date = std::format("{:04}-{:02}-{:02}", m_matchMeta.year, m_matchMeta.month, m_matchMeta.day);
    char timeBuf[16];
    FormatTime(c.startTime - m_displayTimeOffset, timeBuf, sizeof(timeBuf));
    const char* mapName = GetMapNameForLoading(m_matchMeta.map_id);
    std::string map = mapName ? mapName : "";
    std::string occasion = m_matchMeta.occasion;

    auto join = [](std::initializer_list<const std::string*> parts) {
        std::string s;
        for (const std::string* p : parts)
        {
            if (p->empty()) continue;
            if (!s.empty()) s += " \xC2\xB7 ";
            s += *p;
        }
        return s;
    };
    const std::string time = timeBuf;
    std::string line2 = join({&date, &occasion, &map, &time});
    if (width(line2) > maxTextW) line2 = join({&date, &occasion, &time});
    if (width(line2) > maxTextW) line2 = join({&date, &time});

    const float l1w = width(left) + width(vs) + width(right);
    const float textW = std::min(maxTextW, std::max(l1w, width(line2)));
    const float lineH = fs * 1.18f;
    const float bandW = textW + 2.f * pad;
    const float bandH = 2.f * lineH + 2.f * pad - (lineH - fs);

    const bool rightSide = c.tagCorner == 1 || c.tagCorner == 3;
    const bool top = c.tagCorner >= 2;
    const float bx = rightSide ? mx.x - margin - bandW : mn.x + margin;
    const float by = top ? mn.y + margin : mx.y - margin - bandH;

    dl->PushClipRect(mn, mx, true);
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bandW, by + bandH), IM_COL32(0, 0, 0, 150), fs * 0.3f);
    float tx = bx + pad, ty = by + pad;
    dl->AddText(font, fs, ImVec2(tx, ty), GetAgentTeamColor(Team::Blue), left.c_str());
    tx += width(left);
    dl->AddText(font, fs, ImVec2(tx, ty), cText, vs.c_str());
    tx += width(vs);
    dl->AddText(font, fs, ImVec2(tx, ty), GetAgentTeamColor(Team::Red), right.c_str());
    dl->AddText(font, fs, ImVec2(bx + pad, ty + lineH), IM_COL32(220, 210, 185, 255), line2.c_str());
    dl->PopClipRect();
}

std::string ReplayWindow::ClipSuggestedFileName() const
{
    auto [name1, tag1] = ClipGuildNameAndTag(1);
    auto [name2, tag2] = ClipGuildNameAndTag(2);
    std::string a = SanitizeForFileName(tag1.empty() ? name1 : tag1);
    std::string b = SanitizeForFileName(tag2.empty() ? name2 : tag2);
    std::string name = (a.empty() ? "Blue" : a) + "-vs-" + (b.empty() ? "Red" : b);
    const std::string occasion = SanitizeForFileName(m_matchMeta.occasion);
    if (!occasion.empty()) name += "_" + occasion;
    if (m_matchMeta.year > 0)
        name += std::format("_{:04}-{:02}-{:02}", m_matchMeta.year, m_matchMeta.month, m_matchMeta.day);
    const int s = std::max(0, static_cast<int>(m_clip.startTime - m_displayTimeOffset));
    name += std::format("_{:02}m{:02}s.gif", s / 60, s % 60);
    return name;
}

// ---------------------------------------------------------------------------
// The play bar's part: the clip's range shown on the track. Display only - a few seconds of a
// fifteen-minute match is a few pixels there, so the range is edited on the panel's mini bar.
// ---------------------------------------------------------------------------

void ReplayWindow::DrawClipTimelineRange(float trackX, float trackW, float trackY, float trackH, float maxT)
{
    auto& c = m_clip;
    if (c.phase == Phase::Closed || maxT <= 0.f) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = trackX + trackW * std::clamp(c.startTime / maxT, 0.f, 1.f);
    const float x1 = std::max(x0 + 2.f, trackX + trackW * std::clamp(c.endTime / maxT, 0.f, 1.f));
    const float yTop = trackY - 6.f, yBot = trackY + trackH + 6.f;
    dl->AddRectFilled(ImVec2(x0, yTop), ImVec2(x1, yBot), IM_COL32(226, 194, 106, 60), 2.f);
    dl->AddLine(ImVec2(x0, yTop - 2.f), ImVec2(x0, yBot + 2.f), cGoldBright, 1.5f);
    dl->AddLine(ImVec2(x1, yTop - 2.f), ImVec2(x1, yBot + 2.f), cGoldBright, 1.5f);
}

// The panel's zoomed time bar: the clip and some time either side, at least 20 s or three times
// the clip. Drag a handle to move one end, the range to slide the whole clip, or click the bar to
// seek. Every drag scrubs the replay so the frame under the handle is on screen.
void ReplayWindow::DrawClipMiniTimeline()
{
    auto& c = m_clip;
    const float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
    const float len = c.endTime - c.startTime;

    if (c.timelineDrag == 0)
    {
        const float span = std::min(maxT, std::max(20.f, len * 3.f));
        const float mid = 0.5f * (c.startTime + c.endTime);
        c.miniView0 = std::clamp(mid - 0.5f * span, 0.f, std::max(0.f, maxT - span));
        c.miniView1 = c.miniView0 + span;
    }
    const float v0 = c.miniView0, span = std::max(0.1f, c.miniView1 - c.miniView0);

    ImFont* font = ImGui::GetFont();
    const float fsSm = font->FontSize * 0.8f;
    const float width = 330.f, barH = 24.f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y0 = p.y + 4.f, y1 = y0 + barH;
    auto X = [&](float t) { return p.x + (t - v0) / span * width; };
    auto T = [&](float x) { return v0 + (x - p.x) / width * span; };
    auto snap = [](float t) { return std::round(t * 10.f) / 10.f; };
    const float mouseT = T(ImGui::GetIO().MousePos.x);

    auto begin = [&](int which) {
        c.timelineDrag = which;
        c.previewing = false;
        m_replayCtx.isPlaying = false;
    };

    // Hit areas, most specific first: where they overlap, the first one wins the mouse.
    const float xs = X(c.startTime), xe = X(c.endTime);
    bool hovStart = false, hovEnd = false, hovBody = false;

    ImGui::SetCursorScreenPos(ImVec2(xs - 6.f, y0 - 4.f));
    ImGui::InvisibleButton("##ClipMiniStart", ImVec2(12.f, barH + 8.f));
    hovStart = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) begin(1);
    if (ImGui::IsItemActive() && c.timelineDrag == 1)
    {
        c.startTime = std::clamp(snap(mouseT), std::max(0.f, c.endTime - kMaxClipSeconds), c.endTime - kMinClipSeconds);
        m_debugTimeline = c.startTime;
    }
    if (ImGui::IsItemDeactivated()) c.timelineDrag = 0;

    ImGui::SetCursorScreenPos(ImVec2(xe - 6.f, y0 - 4.f));
    ImGui::InvisibleButton("##ClipMiniEnd", ImVec2(12.f, barH + 8.f));
    hovEnd = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) begin(2);
    if (ImGui::IsItemActive() && c.timelineDrag == 2)
    {
        c.endTime = std::clamp(snap(mouseT), c.startTime + kMinClipSeconds, std::min(maxT, c.startTime + kMaxClipSeconds));
        m_debugTimeline = c.endTime;
    }
    if (ImGui::IsItemDeactivated()) c.timelineDrag = 0;

    if (xe - xs > 14.f)
    {
        ImGui::SetCursorScreenPos(ImVec2(xs + 6.f, y0));
        ImGui::InvisibleButton("##ClipMiniBody", ImVec2(xe - xs - 12.f, barH));
        hovBody = ImGui::IsItemHovered();
        if (ImGui::IsItemActivated())
        {
            begin(3);
            c.timelineGrab = mouseT - c.startTime;
        }
        if (ImGui::IsItemActive() && c.timelineDrag == 3)
        {
            c.startTime = std::clamp(snap(mouseT - c.timelineGrab), 0.f, std::max(0.f, maxT - len));
            c.endTime = c.startTime + len;
            m_debugTimeline = c.startTime;
        }
        if (ImGui::IsItemDeactivated()) c.timelineDrag = 0;
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x, y0));
    ImGui::InvisibleButton("##ClipMiniSeek", ImVec2(width, barH));
    if (ImGui::IsItemActivated()) begin(4);
    if (ImGui::IsItemActive() && c.timelineDrag == 4)
        m_debugTimeline = std::clamp(mouseT, 0.f, maxT);
    if (ImGui::IsItemDeactivated()) c.timelineDrag = 0;
    const bool hovSeek = ImGui::IsItemHovered();

    // ---- drawing ----
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + width, y1), IM_COL32(0, 0, 0, 110), 3.f);
    dl->AddRect(ImVec2(p.x, y0), ImVec2(p.x + width, y1), IM_COL32(200, 168, 75, 60), 3.f);

    // Ticks on whole seconds of match time; a label every 5, 10 or 20 s depending on the zoom.
    const float tickStep = span <= 60.f ? 1.f : 5.f;
    const int labelStep = span <= 30.f ? 5 : span <= 60.f ? 10 : 20;
    const float off = m_displayTimeOffset;
    dl->PushClipRect(ImVec2(p.x - 20.f, y0), ImVec2(p.x + width + 20.f, y1 + fsSm + 4.f), true);
    for (float d = std::ceil((v0 - off) / tickStep) * tickStep; d + off <= v0 + span; d += tickStep)
    {
        const float x = X(d + off);
        const int secs = static_cast<int>(std::lround(d));
        const bool major = secs % labelStep == 0;
        dl->AddLine(ImVec2(x, y1 - (major ? 8.f : 4.f)), ImVec2(x, y1), IM_COL32(200, 168, 75, major ? 150 : 70), 1.f);
        if (major)
        {
            char buf[16];
            FormatTime(static_cast<float>(secs), buf, sizeof(buf));
            const float w = font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, buf).x;
            dl->AddText(font, fsSm, ImVec2(x - w * 0.5f, y1 + 2.f), IM_COL32(154, 138, 104, 255), buf);
        }
    }
    dl->PopClipRect();

    dl->PushClipRect(ImVec2(p.x, y0 - 4.f), ImVec2(p.x + width, y1 + 4.f), true);
    const bool rangeHot = hovBody || c.timelineDrag == 3;
    dl->AddRectFilled(ImVec2(xs, y0 + 2.f), ImVec2(xe, y1 - 2.f), IM_COL32(226, 194, 106, rangeHot ? 95 : 65), 2.f);
    dl->AddRect(ImVec2(xs, y0 + 2.f), ImVec2(xe, y1 - 2.f), IM_COL32(226, 194, 106, 140), 2.f);

    if (m_debugTimeline >= v0 && m_debugTimeline <= v0 + span)
    {
        const float xp = X(m_debugTimeline);
        dl->AddLine(ImVec2(xp, y0 - 2.f), ImVec2(xp, y1 + 2.f), IM_COL32(255, 255, 255, 220), 1.5f);
    }

    auto drawHandle = [&](float x, bool hot) {
        const ImU32 col = hot ? IM_COL32(255, 236, 170, 255) : cGoldBright;
        dl->AddRectFilled(ImVec2(x - 2.f, y0 - 3.f), ImVec2(x + 2.f, y1 + 3.f), col, 1.5f);
        dl->AddRectFilled(ImVec2(x - 4.f, y0 + barH * 0.5f - 5.f), ImVec2(x + 4.f, y0 + barH * 0.5f + 5.f), col, 2.f);
    };
    drawHandle(xs, hovStart || c.timelineDrag == 1);
    drawHandle(xe, hovEnd || c.timelineDrag == 2);
    dl->PopClipRect();

    if (hovStart || c.timelineDrag == 1)
        ImGui::SetTooltip("Start  %s", FormatTenths(c.startTime - off).c_str());
    else if (hovEnd || c.timelineDrag == 2)
        ImGui::SetTooltip("End  %s", FormatTenths(c.endTime - off).c_str());
    else if (rangeHot)
        ImGui::SetTooltip("Drag to move the clip");
    else if (hovSeek || c.timelineDrag == 4)
        ImGui::SetTooltip("Go to %s", FormatTenths(std::clamp(mouseT, 0.f, maxT) - off).c_str());

    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, (y1 - p.y) + fsSm + 6.f));
}

// ---------------------------------------------------------------------------
// The editor: box frame, options panel, progress and toast. Called from DrawImGuiOverlay.
// ---------------------------------------------------------------------------

void ReplayWindow::DrawClipEditor()
{
    auto& c = m_clip;
    c.cursorWanted = -1;
    PollClipSaveDialog();
    PollClipEncoder();

    if (c.phase == Phase::Editing || c.phase == Phase::Rendering)
    {
        const float dt = c.phase == Phase::Rendering ? 1.f / c.fps : ImGui::GetIO().DeltaTime;
        UpdateClipFollow(dt);

        ImVec2 mn, mx;
        ClipBoxRect(mn, mx);
        // The tag is part of the picture: drawn on the foreground list, the last thing in the frame,
        // so the render captures it exactly as the editor shows it.
        DrawClipMatchTag(ImGui::GetForegroundDrawList(), mn, mx);
        if (c.phase == Phase::Editing)
        {
            DrawClipBoxFrame(mn, mx);
            DrawClipPanel();
        }
    }
    if (c.phase == Phase::Rendering || c.phase == Phase::Encoding)
        DrawClipProgress();
    DrawClipToast();
}

void ReplayWindow::DrawClipBoxFrame(ImVec2 mn, ImVec2 mx)
{
    auto& c = m_clip;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;

    // Everything outside the box is dimmed: it will not be in the GIF. The dim is a window of its
    // own, over every panel but under the box frame and the options panel, which are brought to
    // the front after it, so the clip's own controls stay bright.
    const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(disp);
    constexpr ImGuiWindowFlags kDimFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##ClipDim", nullptr, kDimFlags))
    {
        if (!popupOpen)
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRectFullScreen();
        const ImU32 dim = IM_COL32(0, 0, 0, 90);
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(disp.x, mn.y), dim);
        dl->AddRectFilled(ImVec2(0, mx.y), ImVec2(disp.x, disp.y), dim);
        dl->AddRectFilled(ImVec2(0, mn.y), ImVec2(mn.x, mx.y), dim);
        dl->AddRectFilled(ImVec2(mx.x, mn.y), ImVec2(disp.x, mx.y), dim);
        dl->PopClipRect();
    }
    ImGui::End();

    // The move tab sits above the box, or below it when the box touches the top of the view.
    int bw, bh, ow, oh;
    ClipOutputSize(bw, bh, ow, oh);
    const std::string tabText = c.followPlayer
        ? std::format("Following  {} x {}", ow, oh)
        : std::format("Move  {} x {}", ow, oh);
    ImFont* font = ImGui::GetFont();
    const float fs = font->FontSize;
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, tabText.c_str());
    const float tabW = ts.x + 26.f, tabH = fs + 8.f;
    const bool tabAbove = mn.y >= tabH + 2.f;
    const ImVec2 tabMin(mn.x, tabAbove ? mn.y - tabH : mx.y);
    const ImVec2 tabMax(tabMin.x + tabW, tabMin.y + tabH);

    // The interaction window covers the box's edges and the tab; its inside is a hit-test hole,
    // so the camera and the panels under the box keep working.
    constexpr float kGrab = 7.f;
    const ImVec2 winMin(mn.x - kGrab, std::min(mn.y - kGrab, tabMin.y));
    const ImVec2 winMax(std::max(mx.x + kGrab, tabMax.x), std::max(mx.y + kGrab, tabMax.y));
    ImGui::SetNextWindowPos(winMin);
    ImGui::SetNextWindowSize(ImVec2(winMax.x - winMin.x, winMax.y - winMin.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin(kClipBoxWindow, nullptr, kFlags))
    {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            ImGui::BringWindowToDisplayFront(win);
        const float holeInset = kGrab - 1.f;
        const ImVec2 holeMin(mn.x + holeInset, mn.y + holeInset);
        const ImVec2 holeSize(std::max(1.f, mx.x - mn.x - 2.f * holeInset), std::max(1.f, mx.y - mn.y - 2.f * holeInset));
        ImGui::SetWindowHitTestHole(win, holeMin, holeSize);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 io = ImGui::GetIO().MousePos;

        // The app's own arrow cursors. The editor is drawn after the frame's cursor decision, so
        // the request is kept and applied at the next one (DrawImGuiOverlay, after UpdateCursorMode).
        auto grab = [&](const char* id, ImVec2 a, ImVec2 b, int drag, CursorMode cursor) {
            ImGui::SetCursorScreenPos(a);
            ImGui::InvisibleButton(id, ImVec2(std::max(1.f, b.x - a.x), std::max(1.f, b.y - a.y)));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                c.cursorWanted = static_cast<int>(cursor);
            if (ImGui::IsItemActivated())
            {
                c.boxDrag = drag;
                c.boxDragMouse = io;
                for (int i = 0; i < 4; i++) c.boxDragStart[i] = c.box[i];
            }
            if (ImGui::IsItemDeactivated()) c.boxDrag = 0;
            return ImGui::IsItemHovered() || ImGui::IsItemActive();
        };

        // Corners first: where an edge and a corner overlap, the corner wins.
        const float g = kGrab;
        bool hot = false;
        hot |= grab("##nw", ImVec2(mn.x - g, mn.y - g), ImVec2(mn.x + g, mn.y + g), kDragLeft | kDragTop, CursorMode::ResizeDiag1);
        hot |= grab("##ne", ImVec2(mx.x - g, mn.y - g), ImVec2(mx.x + g, mn.y + g), kDragRight | kDragTop, CursorMode::ResizeDiag2);
        hot |= grab("##sw", ImVec2(mn.x - g, mx.y - g), ImVec2(mn.x + g, mx.y + g), kDragLeft | kDragBottom, CursorMode::ResizeDiag2);
        hot |= grab("##se", ImVec2(mx.x - g, mx.y - g), ImVec2(mx.x + g, mx.y + g), kDragRight | kDragBottom, CursorMode::ResizeDiag1);
        if (!c.followPlayer)
            hot |= grab("##tab", tabMin, tabMax, kDragMove, CursorMode::Move);
        hot |= grab("##n", ImVec2(mn.x + g, mn.y - g), ImVec2(mx.x - g, mn.y + g), kDragTop, CursorMode::ResizeV);
        hot |= grab("##s", ImVec2(mn.x + g, mx.y - g), ImVec2(mx.x - g, mx.y + g), kDragBottom, CursorMode::ResizeV);
        hot |= grab("##w", ImVec2(mn.x - g, mn.y + g), ImVec2(mn.x + g, mx.y - g), kDragLeft, CursorMode::ResizeH);
        hot |= grab("##e", ImVec2(mx.x - g, mn.y + g), ImVec2(mx.x + g, mx.y - g), kDragRight, CursorMode::ResizeH);

        // Apply the drag in fractions of the view, keeping the box inside it and at least 64 px.
        if (c.boxDrag && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const float dx = (io.x - c.boxDragMouse.x) / std::max(1.f, disp.x);
            const float dy = (io.y - c.boxDragMouse.y) / std::max(1.f, disp.y);
            const float minW = kMinBoxPx / std::max(1.f, disp.x), minH = kMinBoxPx / std::max(1.f, disp.y);
            float l = c.boxDragStart[0], t = c.boxDragStart[1];
            float r = l + c.boxDragStart[2], b = t + c.boxDragStart[3];
            if (c.boxDrag & kDragMove)
            {
                const float w = r - l, h = b - t;
                l = std::clamp(l + dx, 0.f, 1.f - w);
                t = std::clamp(t + dy, 0.f, 1.f - h);
                r = l + w;
                b = t + h;
            }
            else
            {
                // While following, the box is centred on the player: resize both sides at once.
                const bool sym = c.followPlayer;
                if (c.boxDrag & kDragLeft)   { l = std::clamp(l + dx, 0.f, r - minW); if (sym) r = std::clamp(r - dx, l + minW, 1.f); }
                if (c.boxDrag & kDragRight)  { r = std::clamp(r + dx, l + minW, 1.f); if (sym) l = std::clamp(l - dx, 0.f, r - minW); }
                if (c.boxDrag & kDragTop)    { t = std::clamp(t + dy, 0.f, b - minH); if (sym) b = std::clamp(b - dy, t + minH, 1.f); }
                if (c.boxDrag & kDragBottom) { b = std::clamp(b + dy, t + minH, 1.f); if (sym) t = std::clamp(t - dy, 0.f, b - minH); }
            }
            c.box[0] = l; c.box[1] = t; c.box[2] = r - l; c.box[3] = b - t;
        }

        // The frame: a gold border, corner squares and the tab.
        const ImU32 border = (hot || c.boxDrag) ? cGoldBright : cGold;
        dl->AddRect(ImVec2(mn.x - 1.f, mn.y - 1.f), ImVec2(mx.x + 1.f, mx.y + 1.f), border, 0.f, 0, 2.f);
        const float hs = 4.f;
        for (ImVec2 p : {mn, ImVec2(mx.x, mn.y), ImVec2(mn.x, mx.y), mx})
            dl->AddRectFilled(ImVec2(p.x - hs, p.y - hs), ImVec2(p.x + hs, p.y + hs), border);
        dl->AddRectFilled(tabMin, tabMax, IM_COL32(18, 16, 10, 220), 4.f);
        dl->AddRect(tabMin, tabMax, border, 4.f);
        // Four-way arrow glyph for "move", drawn as two crossed lines with dots.
        const ImVec2 ic(tabMin.x + 11.f, (tabMin.y + tabMax.y) * 0.5f);
        dl->AddLine(ImVec2(ic.x - 5.f, ic.y), ImVec2(ic.x + 5.f, ic.y), cGoldBright, 1.5f);
        dl->AddLine(ImVec2(ic.x, ic.y - 5.f), ImVec2(ic.x, ic.y + 5.f), cGoldBright, 1.5f);
        dl->AddText(font, fs, ImVec2(tabMin.x + 20.f, tabMin.y + 4.f), cText, tabText.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void ReplayWindow::DrawClipPanel()
{
    auto& c = m_clip;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    constexpr float kPlayBarH = 76.f;

    ImGui::SetNextWindowPos(ImVec2(disp.x - 12.f, disp.y - kPlayBarH - 8.f), ImGuiCond_Appearing, ImVec2(1.f, 1.f));
    PushClipPanelStyle();
    bool open = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("Create GIF clip###ClipPanel", &open, flags))
    {
        // Above the dim and the box frame, and always inside the window: it opens where the last
        // window size put it, which on a smaller window can be off the edge.
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            ImGui::BringWindowToDisplayFront(win);
        const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const ImVec2 clamped(std::clamp(pos.x, 0.f, std::max(0.f, disp.x - size.x)),
                             std::clamp(pos.y, 0.f, std::max(0.f, disp.y - size.y)));
        if (clamped.x != pos.x || clamped.y != pos.y)
            ImGui::SetWindowPos(clamped);

        int bw, bh, ow, oh;
        ClipOutputSize(bw, bh, ow, oh);
        const float seconds = c.endTime - c.startTime;
        const int frames = std::max(1, static_cast<int>(std::lround(seconds * c.fps)));
        const float maxT = std::max(1.f, m_replayCtx.maxReplayTime);

        // Hierarchy: gold section titles, dim labels in a left column, bright values, one gold
        // primary button. Everything 330 px wide like the size meter.
        const ImVec4 kAccent(0.89f, 0.76f, 0.42f, 1.f);
        const ImVec4 kLabel(0.60f, 0.54f, 0.41f, 1.f);
        const ImVec4 kValue(0.96f, 0.92f, 0.83f, 1.f);
        constexpr float kWidth = 330.f, kLabelW = 86.f;
        bool firstSection = true;
        auto section = [&](const char* title) {
            if (!firstSection) ImGui::Dummy(ImVec2(0.f, 6.f));
            firstSection = false;
            ImGui::SetWindowFontScale(0.82f);
            ImGui::TextColored(kAccent, "%s", title);
            ImGui::SetWindowFontScale(1.f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 1.f), ImVec2(p.x + kWidth, p.y - 1.f),
                                                IM_COL32(200, 168, 75, 55));
            ImGui::Dummy(ImVec2(0.f, 2.f));
        };
        auto label = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kLabel, "%s", text);
            ImGui::SameLine(kLabelW + ImGui::GetStyle().WindowPadding.x);
        };

        // ---- CAPTURE ----
        section("CAPTURE");
        label("Box");
        if (ow != bw)
            ImGui::TextColored(kValue, "%d x %d  \xE2\x86\x92  GIF %d x %d", bw, bh, ow, oh);
        else
            ImGui::TextColored(kValue, "%d x %d", bw, bh);

        label("Follow");
        if (ImGui::Checkbox("##ClipFollowOn", &c.followPlayer))
            c.followPosValid = false;
        ImGui::SameLine();
        ImGui::BeginDisabled(!c.followPlayer);
        ImGui::SetNextItemWidth(kWidth - kLabelW - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        auto playerLabel = [&](int id) -> std::string {
            auto it = m_replayCtx.agents.find(id);
            if (it == m_replayCtx.agents.end()) return "(none)";
            return it->second.partyBarLabel.empty() ? it->second.playerName : it->second.partyBarLabel;
        };
        if (ImGui::BeginCombo("##ClipFollow", c.followPlayer ? playerLabel(c.followAgentId).c_str() : "Off - the box stays put"))
        {
            for (const std::vector<int>* team : {&m_team1PlayerIds, &m_team2PlayerIds})
                for (int id : *team)
                {
                    auto it = m_replayCtx.agents.find(id);
                    if (it == m_replayCtx.agents.end()) continue;
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(GetAgentTeamColor(it->second.teamId)));
                    if (ImGui::Selectable(std::format("{}##{}", playerLabel(id), id).c_str(), id == c.followAgentId))
                    {
                        c.followAgentId = id;
                        c.followPosValid = false;
                    }
                    ImGui::PopStyleColor();
                }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        // ---- TIME ----
        section("TIME");
        label("Range");
        ImGui::TextColored(kValue, "%s \xE2\x86\x92 %s", FormatTenths(c.startTime - m_displayTimeOffset).c_str(),
                           FormatTenths(c.endTime - m_displayTimeOffset).c_str());
        ImGui::SameLine();
        ImGui::TextColored(kAccent, "%.1f s", seconds);
        DrawClipMiniTimeline();
        if (ImGui::SmallButton("Start here"))
        {
            c.startTime = std::clamp(std::round(m_debugTimeline * 10.f) / 10.f, 0.f, maxT - kMinClipSeconds);
            if (c.endTime < c.startTime + kMinClipSeconds || c.endTime > c.startTime + kMaxClipSeconds)
                c.endTime = std::min(maxT, c.startTime + 8.f);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("End here"))
        {
            c.endTime = std::clamp(std::round(m_debugTimeline * 10.f) / 10.f, kMinClipSeconds, maxT);
            if (c.startTime > c.endTime - kMinClipSeconds || c.startTime < c.endTime - kMaxClipSeconds)
                c.startTime = std::max(0.f, c.endTime - 8.f);
        }
        ImGui::SameLine();
        ImGui::TextColored(kLabel, "at the replay's current moment");

        // ---- OUTPUT ----
        section("OUTPUT");
        label("Frame rate");
        ImGui::SetNextItemWidth(80.f);
        if (ImGui::BeginCombo("##ClipFps", std::format("{} fps", c.fps).c_str()))
        {
            for (int f : kFpsChoices)
                if (ImGui::Selectable(std::format("{} fps", f).c_str(), f == c.fps)) c.fps = f;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextColored(kLabel, "Scale");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.f);
        if (ImGui::BeginCombo("##ClipScale", std::format("{}%", c.outputScalePct).c_str()))
        {
            for (int s : kScaleChoices)
                if (ImGui::Selectable(std::format("{}%", s).c_str(), s == c.outputScalePct)) c.outputScalePct = s;
            ImGui::EndCombo();
        }
        label("Match tag");
        ImGui::Checkbox("##ClipTagOn", &c.tagEnabled);
        ImGui::SameLine();
        ImGui::BeginDisabled(!c.tagEnabled);
        static const char* kCorners[] = {"Bottom left", "Bottom right", "Top left", "Top right"};
        ImGui::SetNextItemWidth(130.f);
        ImGui::Combo("##ClipTagCorner", &c.tagCorner, kCorners, 4);
        ImGui::EndDisabled();

        // ---- FILE SIZE ----
        section("FILE SIZE");
        const double est = EstimateClipBytes(c.fps, c.outputScalePct, seconds);
        const double limit = c.sizeLimitMb > 0 ? c.sizeLimitMb * 1024.0 * 1024.0 : 0.0;
        const double frameBytes = static_cast<double>(ow) * oh * 3.0 * frames;
        const bool tooBigForMemory = frameBytes > kMaxFrameBytes;
        // Green: safe. Amber: inside the headroom, a busy scene can still go over. Red: over.
        const bool over = limit > 0.0 && est > limit * kHeadroom;
        const bool overLimit = limit > 0.0 && est > limit;
        {
            const float frac = limit > 0.0 ? static_cast<float>(std::min(1.0, est / limit)) : 0.f;
            const ImVec4 col = overLimit ? ImVec4(0.75f, 0.31f, 0.29f, 0.95f)
                             : over      ? ImVec4(0.85f, 0.65f, 0.25f, 0.95f)
                                         : ImVec4(0.35f, 0.67f, 0.47f, 0.95f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
            ImGui::PushStyleColor(ImGuiCol_Text, kValue);
            const std::string overlay = limit > 0.0
                ? std::format("~{}  of  {} MB", FormatMb(est), c.sizeLimitMb)
                : std::format("~{}", FormatMb(est));
            ImGui::ProgressBar(limit > 0.0 ? frac : 0.f, ImVec2(kWidth, 22.f), overlay.c_str());
            ImGui::PopStyleColor(2);
        }
        if (over)
        {
            if (overLimit)
                ImGui::TextColored(ImVec4(0.95f, 0.50f, 0.45f, 1.f), "Likely over the limit:");
            else
                ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.30f, 1.f), "Close to the limit:");
            ImGui::SameLine();
            auto fits = [&](int fps, int scale, float secs) {
                return EstimateClipBytes(fps, scale, secs) <= limit * kHeadroom;
            };
            if (c.fps > kFpsChoices[0] && ImGui::SmallButton("Lower fps"))
            {
                for (int i = static_cast<int>(std::size(kFpsChoices)) - 1; i >= 0; i--)
                    if (kFpsChoices[i] < c.fps)
                    {
                        c.fps = kFpsChoices[i];
                        if (fits(c.fps, c.outputScalePct, seconds)) break;
                    }
            }
            ImGui::SameLine();
            if (c.outputScalePct > kScaleChoices[std::size(kScaleChoices) - 1] && ImGui::SmallButton("Shrink"))
            {
                for (int s : kScaleChoices)
                    if (s < c.outputScalePct)
                    {
                        c.outputScalePct = s;
                        if (fits(c.fps, s, seconds)) break;
                    }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Shorten"))
            {
                const double perFrame = static_cast<double>(ow) * oh * c.bytesPerPixelFrame + kPerFrameBytes;
                const double maxFrames = std::max(1.0, (limit * kHeadroom - 2048.0) / perFrame);
                const float secs = std::max(1.f, static_cast<float>(std::floor(maxFrames) / c.fps));
                c.endTime = std::min(c.endTime, std::round((c.startTime + secs) * 10.f) / 10.f);
            }
        }

        label("Limit");
        ImGui::SetNextItemWidth(150.f);
        auto limitLabel = [](int mb) {
            return mb == 0 ? std::string("No limit")
                 : mb == kDiscordLimitMb ? std::format("{} MB (Discord)", mb) : std::format("{} MB", mb);
        };
        if (ImGui::BeginCombo("##ClipLimit", limitLabel(c.sizeLimitMb).c_str()))
        {
            for (int mb : kLimitChoices)
                if (ImGui::Selectable(limitLabel(mb).c_str(), mb == c.sizeLimitMb)) c.sizeLimitMb = mb;
            ImGui::EndCombo();
        }
        if (c.sizeLimitMb == 0 || c.sizeLimitMb > kDiscordLimitMb)
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kWidth);
            ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.30f, 1.f),
                               "Over 10 MB: users without Discord Nitro can't upload this file.");
            ImGui::PopTextWrapPos();
        }
        ImGui::Checkbox("Copy to clipboard after export", &c.copyToClipboard);

        ImGui::Separator();

        // ---- actions ----
        if (ImGui::Button(c.previewing ? "Stop preview" : "Preview", ImVec2(110.f, 0.f)))
        {
            if (c.previewing)
            {
                c.previewing = false;
                m_replayCtx.isPlaying = false;
            }
            else
            {
                c.previewing = true;
                m_debugTimeline = c.startTime;
                m_replayCtx.isPlaying = true;
                m_replayCtx.playbackSpeed = 1.f;
                m_replayCtx.speedIndex = 3;
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(tooBigForMemory || c.dialogOpen);
        if (ImGui::Button(c.dialogOpen ? "Choosing file..." : "Export...", ImVec2(110.f, 0.f)))
            BeginClipSaveDialog();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(80.f, 0.f)))
            open = false;
        if (tooBigForMemory)
            ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.40f, 1.f), "Too large to render: shrink the box, the scale or the length.");
        ImGui::TextDisabled("Alt+X or Esc closes");
    }
    ImGui::End();
    ImGui::PopStyleColor(kClipPanelColours);
    ImGui::PopStyleVar(kClipPanelVars);

    if (!open && c.phase == Phase::Editing)
        CloseClipEditor();
}

void ReplayWindow::DrawClipProgress()
{
    auto& c = m_clip;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, 64.f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    PushClipPanelStyle();
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin(kClipProgressWindow, nullptr, kFlags))
    {
        float frac = 0.f;
        std::string label;
        if (c.phase == Phase::Rendering)
        {
            const int total = c.prerollFrames + c.frameCount;
            frac = static_cast<float>(c.frameIndex + c.prerollFrames) / std::max(1, total);
            label = c.frameIndex < 0 ? std::string("Preparing...")
                                     : std::format("Rendering frame {} / {}", c.frameIndex + 1, c.frameCount);
        }
        else
        {
            frac = c.progress.load();
            label = std::format("Encoding GIF... {}%", static_cast<int>(frac * 100.f));
        }
        ImGui::TextUnformatted("Creating GIF clip");
        ImGui::ProgressBar(std::clamp(frac, 0.f, 1.f), ImVec2(300.f, 0.f), label.c_str());
        if (ImGui::Button("Cancel (Esc)", ImVec2(300.f, 0.f)) && c.phase == Phase::Encoding)
            c.cancel = true;
        c.cancelMin = ImGui::GetItemRectMin();
        c.cancelMax = ImGui::GetItemRectMax();
    }
    ImGui::End();
    ImGui::PopStyleColor(kClipPanelColours);
    ImGui::PopStyleVar(kClipPanelVars);
}

void ReplayWindow::DrawClipToast()
{
    auto& c = m_clip;
    if (c.toast.empty()) return;
    if (ImGui::GetTime() > c.toastUntil) { c.toast.clear(); return; }

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x - 12.f, disp.y - 76.f - 8.f), ImGuiCond_Always, ImVec2(1.f, 1.f));
    PushClipPanelStyle();
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##ClipToast", nullptr, kFlags))
    {
        if (c.toastIsError)
            ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.40f, 1.f), "%s", c.toast.c_str());
        else
            ImGui::TextUnformatted(c.toast.c_str());
        if (!c.savedPath.empty() && !c.toastIsError)
        {
            if (ImGui::SmallButton("Open folder"))
            {
                // A plain process launch: ShellExecute wants a single-threaded COM apartment.
                std::wstring cmd = L"explorer.exe /select,\"" + c.savedPath + L"\"";
                STARTUPINFOW si = {sizeof(si)};
                PROCESS_INFORMATION pi = {};
                if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
                {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }
            if (c.toastOverLimit)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Adjust and re-export") && c.phase == Phase::Closed)
                {
                    const float start = c.startTime, end = c.endTime;
                    OpenClipEditor();
                    c.startTime = start;
                    c.endTime = end;
                    c.toast.clear();
                }
            }
        }
        if (ImGui::IsWindowHovered()) c.toastUntil = ImGui::GetTime() + 2.0;
    }
    ImGui::End();
    ImGui::PopStyleColor(kClipPanelColours);
    ImGui::PopStyleVar(kClipPanelVars);
}

// ---------------------------------------------------------------------------
// Save dialog
// ---------------------------------------------------------------------------

namespace
{
    // The shell's Save dialog needs a single-threaded COM apartment; the UI thread was initialised
    // multithreaded and the dialog hangs there. So it runs on a short-lived thread of its own,
    // owned by the replay window (which it disables while open, like any modal dialog), and the
    // replay keeps drawing meanwhile.
    void RunClipSaveDialog(HWND owner, std::wstring fileName, std::wstring folderPath, ClipExportState* state)
    {
        using Microsoft::WRL::ComPtr;
        bool ok = false;
        std::wstring result;
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
        {
            {
                ComPtr<IFileSaveDialog> dlg;
                if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
                {
                    const COMDLG_FILTERSPEC types[] = {{L"GIF animation (*.gif)", L"*.gif"}};
                    dlg->SetFileTypes(1, types);
                    dlg->SetDefaultExtension(L"gif");
                    dlg->SetTitle(L"Save GIF clip");
                    dlg->SetFileName(fileName.c_str());
                    DWORD opts = 0;
                    if (SUCCEEDED(dlg->GetOptions(&opts)))
                        dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

                    ComPtr<IShellItem> folder;
                    if (!folderPath.empty())
                        SHCreateItemFromParsingName(folderPath.c_str(), nullptr, IID_PPV_ARGS(&folder));
                    if (!folder)
                        SHGetKnownFolderItem(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder));
                    if (folder)
                        dlg->SetFolder(folder.Get());

                    ComPtr<IShellItem> item;
                    PWSTR p = nullptr;
                    if (SUCCEEDED(dlg->Show(owner)) && SUCCEEDED(dlg->GetResult(&item)) &&
                        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p)
                    {
                        result = p;
                        ok = true;
                    }
                    if (p) CoTaskMemFree(p);
                }
            }
            CoUninitialize();
        }
        state->dialogPath = std::move(result);
        state->dialogOk = ok;
        state->dialogDone.store(true, std::memory_order_release);
    }
}

void ReplayWindow::BeginClipSaveDialog()
{
    auto& c = m_clip;
    if (c.dialogOpen || c.phase != Phase::Editing) return;
    if (c.dialogThread.joinable()) c.dialogThread.join();

    std::error_code ec;
    std::wstring folder = (!c.lastFolder.empty() && std::filesystem::is_directory(c.lastFolder, ec))
        ? c.lastFolder : std::wstring();
    c.previewing = false;
    m_replayCtx.isPlaying = false;
    c.dialogOpen = true;
    c.dialogOk = false;
    c.dialogDone = false;
    c.dialogThread = std::thread(RunClipSaveDialog, m_hwnd, FromUtf8(ClipSuggestedFileName()), folder, &c);
}

void ReplayWindow::PollClipSaveDialog()
{
    auto& c = m_clip;
    if (!c.dialogOpen || !c.dialogDone.load(std::memory_order_acquire)) return;
    c.dialogThread.join();
    c.dialogOpen = false;
    if (!c.dialogOk || c.phase != Phase::Editing) return;   // cancelled, or the editor was closed

    c.lastFolder = std::filesystem::path(c.dialogPath).parent_path().wstring();
    SaveClipSettings();
    StartClipExport(c.dialogPath);
}

// ---------------------------------------------------------------------------
// Render on a fixed clock
// ---------------------------------------------------------------------------

void ReplayWindow::StartClipExport(const std::wstring& path)
{
    auto& c = m_clip;
    c.outPath = path;
    c.previewing = false;

    ClipOutputSize(c.boxW, c.boxH, c.outW, c.outH);
    c.frameCount = std::max(1, static_cast<int>(std::lround((c.endTime - c.startTime) * c.fps)));
    c.prerollFrames = static_cast<int>(std::min(kPrerollSeconds * c.fps, std::floor(c.startTime * c.fps)));
    c.frameIndex = -c.prerollFrames;
    c.frames.clear();
    c.frames.reserve(c.frameCount);

    c.savedTimeline = m_debugTimeline;
    c.savedPlaying = m_replayCtx.isPlaying;
    c.savedLoop = m_replayCtx.loopPlayback;
    c.savedSpeed = m_replayCtx.playbackSpeed;
    c.savedSpeedIndex = m_replayCtx.speedIndex;

    m_replayCtx.isPlaying = true;
    m_replayCtx.loopPlayback = false;
    m_replayCtx.playbackSpeed = 1.f;
    m_replayCtx.speedIndex = 3;
    m_timer.SetManualStepSeconds(1.0 / c.fps);
    c.phase = Phase::Rendering;
}

void ReplayWindow::RestoreAfterClipRender()
{
    auto& c = m_clip;
    m_timer.SetManualStepSeconds(0.0);
    m_timer.ResetElapsedTime();
    m_debugTimeline = c.savedTimeline;
    m_replayCtx.isPlaying = c.savedPlaying;
    m_replayCtx.loopPlayback = c.savedLoop;
    m_replayCtx.playbackSpeed = c.savedSpeed;
    m_replayCtx.speedIndex = c.savedSpeedIndex;
    c.captureThisFrame = false;
    c.resolveTex.Reset();
    c.stagingTex.Reset();
}

void ReplayWindow::AbortClipRender()
{
    auto& c = m_clip;
    if (c.phase != Phase::Rendering) return;
    RestoreAfterClipRender();
    c.frames.clear();
    c.frames.shrink_to_fit();
    c.phase = Phase::Editing;
    c.toast = "GIF export cancelled";
    c.toastIsError = false;
    c.toastOverLimit = false;
    c.savedPath.clear();
    c.toastUntil = ImGui::GetTime() + 3.0;
}

// Replaces ImGui_ImplDX11_RenderDrawData at the end of DrawImGuiOverlay. On a captured frame the
// interface is drawn without the clip's progress window, the box is read back, and the progress
// window is drawn on top afterwards: it is on screen but never in the GIF.
void ReplayWindow::RenderImGuiWithClipCapture(ImDrawData* drawData)
{
    auto& c = m_clip;
    if (c.phase != Phase::Rendering)
    {
        ImGui_ImplDX11_RenderDrawData(drawData);
        if (m_clipSelfTestShotPending) SaveClipSelfTestShot();
        return;
    }

    ImGuiWindow* progress = ImGui::FindWindowByName(kClipProgressWindow);
    ImDrawList* progressList = progress ? progress->DrawList : nullptr;

    ImDrawData scene = *drawData;
    ImDrawData over = *drawData;
    scene.CmdLists.clear();
    over.CmdLists.clear();
    scene.TotalVtxCount = scene.TotalIdxCount = 0;
    over.TotalVtxCount = over.TotalIdxCount = 0;
    for (ImDrawList* list : drawData->CmdLists)
    {
        ImDrawData& target = (list == progressList) ? over : scene;
        target.CmdLists.push_back(list);
        target.TotalVtxCount += list->VtxBuffer.Size;
        target.TotalIdxCount += list->IdxBuffer.Size;
    }
    scene.CmdListsCount = scene.CmdLists.Size;
    over.CmdListsCount = over.CmdLists.Size;

    ImGui_ImplDX11_RenderDrawData(&scene);
    if (c.captureThisFrame)
        CaptureClipFrame();
    if (over.CmdListsCount > 0)
        ImGui_ImplDX11_RenderDrawData(&over);

    if (c.phase == Phase::Rendering)
    {
        c.frameIndex++;
        if (c.frameIndex >= c.frameCount)
            FinishClipRender();
    }
}

void ReplayWindow::CaptureClipFrame()
{
    auto& c = m_clip;
    auto* device = m_deviceResources->GetD3DDevice();
    auto* ctx = m_deviceResources->GetD3DDeviceContext();
    ID3D11Texture2D* back = m_deviceResources->GetRenderTarget();
    if (!device || !ctx || !back) return;

    D3D11_TEXTURE2D_DESC bd;
    back->GetDesc(&bd);

    // An MSAA back buffer is resolved first; a staging copy needs a single-sample source.
    ID3D11Texture2D* src = back;
    if (bd.SampleDesc.Count > 1)
    {
        D3D11_TEXTURE2D_DESC rd = {};
        if (c.resolveTex) c.resolveTex->GetDesc(&rd);
        if (!c.resolveTex || rd.Width != bd.Width || rd.Height != bd.Height || rd.Format != bd.Format)
        {
            rd = bd;
            rd.SampleDesc.Count = 1;
            rd.SampleDesc.Quality = 0;
            rd.Usage = D3D11_USAGE_DEFAULT;
            rd.BindFlags = 0;
            rd.CPUAccessFlags = 0;
            rd.MiscFlags = 0;
            rd.MipLevels = 1;
            rd.ArraySize = 1;
            c.resolveTex.Reset();
            if (FAILED(device->CreateTexture2D(&rd, nullptr, &c.resolveTex))) return;
        }
        ctx->ResolveSubresource(c.resolveTex.Get(), 0, back, 0, bd.Format);
        src = c.resolveTex.Get();
    }

    const UINT bw = std::min<UINT>(static_cast<UINT>(c.boxW), bd.Width);
    const UINT bh = std::min<UINT>(static_cast<UINT>(c.boxH), bd.Height);
    D3D11_TEXTURE2D_DESC sd = {};
    if (c.stagingTex) c.stagingTex->GetDesc(&sd);
    if (!c.stagingTex || sd.Width != bw || sd.Height != bh || sd.Format != bd.Format)
    {
        sd = {};
        sd.Width = bw;
        sd.Height = bh;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = bd.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        c.stagingTex.Reset();
        if (FAILED(device->CreateTexture2D(&sd, nullptr, &c.stagingTex))) return;
    }

    ImVec2 mn, mx;
    ClipBoxRect(mn, mx);
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float sx = bd.Width / std::max(1.f, disp.x), sy = bd.Height / std::max(1.f, disp.y);
    const UINT x0 = static_cast<UINT>(std::clamp(static_cast<int>(std::lround(mn.x * sx)), 0, static_cast<int>(bd.Width - bw)));
    const UINT y0 = static_cast<UINT>(std::clamp(static_cast<int>(std::lround(mn.y * sy)), 0, static_cast<int>(bd.Height - bh)));
    const D3D11_BOX box = {x0, y0, 0, x0 + bw, y0 + bh, 1};
    ctx->CopySubresourceRegion(c.stagingTex.Get(), 0, 0, 0, 0, src, 0, &box);

    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(c.stagingTex.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    const bool bgra = bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || bd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    ClipGif::FrameRGB rgb(static_cast<size_t>(bw) * bh * 3);
    for (UINT y = 0; y < bh; y++)
    {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        uint8_t* out = &rgb[static_cast<size_t>(y) * bw * 3];
        for (UINT x = 0; x < bw; x++)
        {
            const uint8_t* p = row + x * 4;
            out[x * 3 + 0] = bgra ? p[2] : p[0];
            out[x * 3 + 1] = p[1];
            out[x * 3 + 2] = bgra ? p[0] : p[2];
        }
    }
    ctx->Unmap(c.stagingTex.Get(), 0);

    if (c.outW != static_cast<int>(bw) || c.outH != static_cast<int>(bh))
        rgb = ClipGif::Downscale(rgb, bw, bh, c.outW, c.outH);
    c.frames.push_back(std::move(rgb));
}

void ReplayWindow::FinishClipRender()
{
    auto& c = m_clip;
    RestoreAfterClipRender();
    if (c.frames.empty())
    {
        c.phase = Phase::Editing;
        c.toast = "GIF export failed: no frame could be captured";
        c.toastIsError = true;
        c.toastUntil = ImGui::GetTime() + 6.0;
        return;
    }

    c.phase = Phase::Encoding;
    c.progress = 0.f;
    c.cancel = false;
    c.workerDone = false;
    c.workerOk = false;
    c.gif.clear();
    if (c.worker.joinable()) c.worker.join();

    ClipGif::EncodeSettings s;
    s.width = c.outW;
    s.height = c.outH;
    s.fps = c.fps;
    ClipExportState* state = &c;
    c.worker = std::thread([state, s]() {
        state->workerOk = ClipGif::Encode(state->frames, s, state->gif, &state->progress, &state->cancel);
        state->workerDone.store(true, std::memory_order_release);
    });
}

void ReplayWindow::PollClipEncoder()
{
    auto& c = m_clip;
    if (c.phase != Phase::Encoding || !c.workerDone.load(std::memory_order_acquire)) return;
    c.worker.join();

    const size_t frameCount = c.frames.size();
    c.frames.clear();
    c.frames.shrink_to_fit();
    c.toastOverLimit = false;
    c.savedPath.clear();

    if (!c.workerOk)
    {
        const bool cancelled = c.cancel.load();
        c.phase = Phase::Editing;
        c.toast = cancelled ? "GIF export cancelled" : "GIF export failed while encoding";
        c.toastIsError = !cancelled;
        c.toastUntil = ImGui::GetTime() + (cancelled ? 3.0 : 6.0);
        c.gif.clear();
        return;
    }

    std::ofstream f(std::filesystem::path(c.outPath), std::ios::binary | std::ios::trunc);
    if (f.is_open())
        f.write(reinterpret_cast<const char*>(c.gif.data()), static_cast<std::streamsize>(c.gif.size()));
    if (!f.is_open() || !f.good())
    {
        c.phase = Phase::Editing;
        c.toast = "Could not write " + ToUtf8(c.outPath);
        c.toastIsError = true;
        c.toastUntil = ImGui::GetTime() + 8.0;
        c.gif.clear();
        return;
    }
    f.close();

    // Learn the real bytes per pixel-frame so the next estimate is closer.
    const double bytes = static_cast<double>(c.gif.size());
    const double pixelFrames = static_cast<double>(c.outW) * c.outH * std::max<size_t>(1, frameCount);
    const double measured = std::clamp((bytes - frameCount * kPerFrameBytes - 2048.0) / pixelFrames, 0.02, 1.5);
    // Up fast, down slowly, and never below a fight's rate: one quiet clip must not make the next
    // busy one look like it fits.
    const double w = measured > c.bytesPerPixelFrame ? 0.7 : 0.15;
    c.bytesPerPixelFrame = std::max(kMinBytesPerPixelFrame, (1.0 - w) * c.bytesPerPixelFrame + w * measured);

    bool copied = false;
    if (c.copyToClipboard)
        copied = CopyFileToClipboard(m_hwnd, c.outPath);

    c.savedPath = c.outPath;
    c.toast = std::format("Saved \xC2\xB7 {}{}", FormatMb(bytes), copied ? " \xC2\xB7 copied to clipboard" : "");
    c.toastIsError = false;
    if (c.sizeLimitMb > 0 && bytes > c.sizeLimitMb * 1024.0 * 1024.0)
    {
        c.toast += std::format("\nOver the {} MB limit{}", c.sizeLimitMb,
                               c.sizeLimitMb == kDiscordLimitMb ? ": Discord will refuse it without Nitro" : "");
        c.toastOverLimit = true;
    }
    c.toastUntil = ImGui::GetTime() + 8.0;
    c.gif.clear();
    c.gif.shrink_to_fit();
    c.phase = Phase::Closed;
    SaveClipSettings();
}

// ---------------------------------------------------------------------------
// DEV TOOL (GWO_CLIP_SELFTEST): one clip per batch window, through the real render and encoder,
// with no dialog. Waits for the attribute solve and a few seconds of character loading first.
// ---------------------------------------------------------------------------

void ReplayWindow::ProcessClipSelfTest()
{
    if (s_clipSelfTestPath.empty() || m_clipSelfTestStage >= 4) return;
    auto& c = m_clip;
    const double now = GetTickCount64() / 1000.0;

    switch (m_clipSelfTestStage)
    {
    case 0:   // loaded: open the editor as a user would
    {
        if (!m_agentsClassified || !m_attributesDeduced) return;
        if (m_clipSelfTestReadyAt == 0.0) { m_clipSelfTestReadyAt = now; return; }
        if (now - m_clipSelfTestReadyAt < 8.0) return;

        const float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
        m_debugTimeline = s_clipSelfTestAt >= 0.f ? std::min(s_clipSelfTestAt + 3.f, maxT - 4.f) : maxT * 0.4f + 3.f;
        if (m_followedAgentId < 0 && !m_team1PlayerIds.empty())
            EnterFollowMode(m_team1PlayerIds.front());
        OpenClipEditor();
        c.fps = 20;
        c.outputScalePct = 100;
        c.followPlayer = true;
        c.tagEnabled = true;
        c.copyToClipboard = false;
        c.box[0] = 0.3f; c.box[1] = 0.25f; c.box[2] = 0.4f; c.box[3] = 0.5f;
        m_clipSelfTestReadyAt = now;
        m_clipSelfTestStage = 1;
        return;
    }
    case 1:   // the editor on screen for a moment, then one screenshot of the whole window
        if (now - m_clipSelfTestReadyAt < 1.5) return;
        m_clipSelfTestShotPending = true;
        m_clipSelfTestStage = 2;
        return;
    case 2:
        if (m_clipSelfTestShotPending) return;
        RunLog::Line("clip-selftest: '%s' %.1f-%.1f s, estimate %.2f MB", m_matchMeta.folder_name.c_str(),
                     c.startTime, c.endTime, EstimateClipBytes(c.fps, c.outputScalePct, c.endTime - c.startTime) / 1048576.0);
        StartClipExport(s_clipSelfTestPath);
        m_clipSelfTestStage = 3;
        return;
    default:
        if (c.phase == ClipExportState::Phase::Closed || c.phase == ClipExportState::Phase::Editing)
        {
            RunLog::Line("clip-selftest: '%s' -> %s (box %dx%d, %d frames)", m_matchMeta.folder_name.c_str(),
                         c.toast.c_str(), c.outW, c.outH, c.frameCount);
            m_clipSelfTestStage = 4;
        }
        return;
    }
}

// The self-test's look at the editor: the whole back buffer, interface included, as a PNG beside
// the GIF.
void ReplayWindow::SaveClipSelfTestShot()
{
    m_clipSelfTestShotPending = false;
    DirectX::ScratchImage shot;
    if (FAILED(DirectX::CaptureTexture(m_deviceResources->GetD3DDevice(), m_deviceResources->GetD3DDeviceContext(),
                                       m_deviceResources->GetRenderTarget(), shot)))
        return;
    std::filesystem::path png = std::filesystem::path(s_clipSelfTestPath).replace_extension(L".editor.png");
    DirectX::SaveToWICFile(*shot.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE,
                           DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), png.c_str());
    RunLog::Line("clip-selftest: editor screenshot '%s'", png.string().c_str());
}
