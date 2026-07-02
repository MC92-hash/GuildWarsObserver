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

const char* ReplayWindow::GetMapNameForLoading(int mapId)
{
    switch (mapId)
    {
    case 7: case 171:   return "Warrior's Isle";
    case 8: case 172:   return "Hunter's Isle";
    case 9: case 173:   return "Wizard's Isle";
    case 52: case 167:  return "Burning Isle";
    case 68: case 170:  return "Frozen Isle";
    case 69: case 174:  return "Nomad's Isle";
    case 70: case 168:  return "Druid's Isle";
    case 71: case 175:  return "Isle of the Dead";
    case 360: case 358: return "Isle of Meditation";
    case 361: case 355: return "Isle of Weeping Stone";
    case 362: case 356: return "Isle of Jade";
    case 363: case 357: return "Imperial Isle";
    case 531: case 533: return "Uncharted Isle";
    case 532: case 534: return "Isle of Wurms";
    case 537: case 541: return "Corrupted Isle";
    case 540: case 542: return "Isle of Solitude";
    default:            return nullptr;
    }
}

const char* ReplayWindow::GetMapScreenshotFile(int mapId)
{
    switch (mapId)
    {
    case 52: case 167:  return "Burning Isle.jpg";
    case 537: case 541: return "Corrupted Isle.jpg";
    case 70: case 168:  return "Druid's Isle.webp";
    case 68: case 170:  return "Frozen Isle.webp";
    case 363: case 357: return "Imperial Isle.jpg";
    case 362: case 356: return "Isle of Jade.jpg";
    case 360: case 358: return "Isle of Meditation.jpg";
    case 540: case 542: return "Isle of Solitude.jpg";
    case 71: case 175:  return "Isle of the Dead.jpg";
    case 361: case 355: return "Isle of Weeping Stone.webp";
    case 532: case 534: return "Isle of Wurms.webp";
    case 69: case 174:  return "Nomad's Isle.jpg";
    case 531: case 533: return "Uncharted Isle.jpg";
    case 7: case 171:   return "Warrior's Isle.webp";
    case 9: case 173:   return "Wizard's Isle.webp";
    default:            return nullptr;
    }
}

std::string ReplayWindow::GetMatchLoadingBgPath() const
{
    const char* file = GetMapScreenshotFile(m_matchMeta.map_id);

    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return "";
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 6; i++)
    {
        if (file)
        {
            auto p = dir / "Textures" / "Loading_Screen" / file;
            if (std::filesystem::exists(p))
                return p.string();
        }
        // Fallback: main splash screen
        auto fb = dir / "Textures" / "Launch_screen" / "GWOBS_Loading_Screen_1.png";
        if (std::filesystem::exists(fb))
            return fb.string();

        if (!dir.has_parent_path() || dir == dir.parent_path())
            break;
        dir = dir.parent_path();
    }
    return "";
}
// FindGuildByTagStatic: hoisted to file scope (declared in ReplayWindow_Internal.h)
const GuildMeta* FindGuildByTagStatic(const MatchMeta& m, const std::string& tag)
{
    if (tag.empty()) return nullptr;
    for (const auto& [id, gm] : m.guilds)
        if (gm.tag == tag) return &gm;
    return nullptr;
}



// ---------------------------------------------------------------------------
// Loading screen overlay (ImGui-based match loading screen)
// ---------------------------------------------------------------------------

namespace
{
    static std::string LsFormatWithCommas(int value)
    {
        if (value < 0) value = 0;
        std::string raw = std::to_string(value);
        std::string result;
        int count = 0;
        for (int i = static_cast<int>(raw.size()) - 1; i >= 0; --i)
        {
            if (count > 0 && count % 3 == 0)
                result.insert(result.begin(), ',');
            result.insert(result.begin(), raw[i]);
            ++count;
        }
        return result;
    }

    static void LsDrawTextWithShadow(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text)
    {
        ImU32 shadow = IM_COL32(0, 0, 0, 220);
        dl->AddText(ImVec2(pos.x - 1.f, pos.y + 1.f), shadow, text);
        dl->AddText(ImVec2(pos.x + 1.f, pos.y + 1.f), shadow, text);
        dl->AddText(ImVec2(pos.x,       pos.y + 2.f), shadow, text);
        dl->AddText(pos, col, text);
    }

    static void LsDrawTextWithShadowEx(ImDrawList* dl, ImFont* font, float fontSize,
                                        ImVec2 pos, ImU32 col, const char* text)
    {
        ImU32 shadow = IM_COL32(0, 0, 0, 220);
        dl->AddText(font, fontSize, ImVec2(pos.x - 1.f, pos.y + 1.f), shadow, text);
        dl->AddText(font, fontSize, ImVec2(pos.x + 1.f, pos.y + 1.f), shadow, text);
        dl->AddText(font, fontSize, ImVec2(pos.x,       pos.y + 2.f), shadow, text);
        dl->AddText(font, fontSize, pos, col, text);
    }

    static void LsDrawTextCrispShadow(ImDrawList* dl, ImFont* font, float fontSize,
                                       ImVec2 pos, ImU32 col, const char* text)
    {
        int srcAlpha = static_cast<int>((col >> IM_COL32_A_SHIFT) & 0xFF);
        int shadowA  = (std::min)(230, srcAlpha);
        ImU32 shadow = IM_COL32(0, 0, 0, shadowA);
        dl->AddText(font, fontSize, ImVec2(pos.x, pos.y + 1.f), shadow, text);
        dl->AddText(font, fontSize, ImVec2(pos.x + 1.f, pos.y + 1.f), shadow, text);
        dl->AddText(font, fontSize, pos, col, text);
    }

    static std::string GetPartyGuildDisplay(const MatchMeta& m, const std::string& partyId,
                                            std::string& outName, std::string& outTag,
                                            const std::string& folderTag = "")
    {
        auto* fg = FindGuildByTagStatic(m, folderTag);
        if (fg) {
            outName = fg->name;
            outTag = fg->tag;
            return outName + " [" + outTag + "]";
        }

        auto pit = m.parties.find(partyId);
        if (pit == m.parties.end() || pit->second.players.empty())
        {
            // No player data — try guild lookup directly by party ID
            auto git = m.guilds.find(partyId);
            if (git != m.guilds.end() && !git->second.name.empty())
            {
                outName = git->second.name;
                outTag = git->second.tag;
                return outName + " [" + outTag + "]";
            }
            outName = "?";
            outTag = "";
            return "?";
        }

        std::map<int, int> guildCounts;
        for (const auto& p : pit->second.players)
            if (p.guild_id > 0) guildCounts[p.guild_id]++;

        int bestGuildId = 0, bestCount = 0;
        for (const auto& [gid, cnt] : guildCounts)
            if (cnt > bestCount) { bestGuildId = gid; bestCount = cnt; }

        if (bestGuildId == 0)
        {
            auto git = m.guilds.find(partyId);
            if (git != m.guilds.end() && !git->second.name.empty())
            {
                outName = git->second.name;
                outTag = git->second.tag;
                return outName + " [" + outTag + "]";
            }
            outName = "Unknown";
            outTag = "";
            return "Unknown";
        }

        auto guildIdStr = std::to_string(bestGuildId);
        auto git = m.guilds.find(guildIdStr);
        if (git != m.guilds.end())
        {
            outName = git->second.name;
            outTag = git->second.tag;
            return outName + " [" + outTag + "]";
        }
        outName = "Guild #" + guildIdStr;
        outTag = "";
        return outName;
    }

    static void GetPartyGuildInfo(const MatchMeta& m, const std::string& partyId,
                                  std::string& outName, std::string& outTag,
                                  int& outRank, int& outRating,
                                  const std::string& folderTag = "")
    {
        outRank = 0;
        outRating = 0;

        auto findGuild = [&](const GuildMeta& gm) {
            outName = gm.name;
            outTag = gm.tag;
            outRank = gm.rank;
            outRating = gm.rating;
        };

        auto* fg = FindGuildByTagStatic(m, folderTag);
        if (fg) { findGuild(*fg); return; }

        auto pit = m.parties.find(partyId);
        if (pit == m.parties.end() || pit->second.players.empty())
        {
            auto git = m.guilds.find(partyId);
            if (git != m.guilds.end() && !git->second.name.empty())
            {
                findGuild(git->second);
                return;
            }
            outName = "?";
            outTag = "";
            return;
        }

        std::map<int, int> guildCounts;
        for (const auto& p : pit->second.players)
            if (p.guild_id > 0) guildCounts[p.guild_id]++;

        int bestGuildId = 0, bestCount = 0;
        for (const auto& [gid, cnt] : guildCounts)
            if (cnt > bestCount) { bestGuildId = gid; bestCount = cnt; }

        if (bestGuildId == 0)
        {
            auto git = m.guilds.find(partyId);
            if (git != m.guilds.end() && !git->second.name.empty())
            {
                findGuild(git->second);
                return;
            }
            outName = "Unknown";
            outTag = "";
            return;
        }

        auto guildIdStr = std::to_string(bestGuildId);
        auto git = m.guilds.find(guildIdStr);
        if (git != m.guilds.end())
        {
            findGuild(git->second);
            return;
        }
        outName = "Guild #" + guildIdStr;
        outTag = "";
    }
}


void ReplayWindow::RenderLoadingScreen()
{
    // --- D3D clear ---
    auto* d3dCtx = m_deviceResources->GetD3DDeviceContext();
    auto* rtv = m_deviceResources->GetRenderTargetView();
    auto* dsv = m_deviceResources->GetDepthStencilView();

    float darkBg[4] = { 0.039f, 0.055f, 0.071f, 1.0f };
    d3dCtx->ClearRenderTargetView(rtv, darkBg);
    d3dCtx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);

    auto vp = m_deviceResources->GetScreenViewport();
    d3dCtx->RSSetViewports(1, &vp);
    d3dCtx->OMSetRenderTargets(1, &rtv, nullptr);

    // --- Init ImGui if needed ---
    if (!m_imguiInitialized)
        InitImGui();

    ImGuiContext* prevCtx = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_imguiContext);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 display = io.DisplaySize;

    // --- Init texture cache and background path (once) ---
    if (!m_lsTexCache.IsInitialized())
    {
        m_lsTexCache.Init(m_deviceResources->GetD3DDevice());
        m_lsBgPath = GetMatchLoadingBgPath();
    }

    // --- Timing ---
    if (!m_lsStartTimeSet)
    {
        m_lsStartTime = LsClock::now();
        m_lsStartTimeSet = true;
    }
    float elapsed = std::chrono::duration<float>(LsClock::now() - m_lsStartTime).count();

    constexpr float kFadeInSec         = 0.3f;
    constexpr float kBgFadeInSec       = 0.5f;
    constexpr float kStatusFadeOutSec  = 0.2f;
    constexpr float kReadyLabelSec     = 0.2f;
    constexpr float kReadyHoldSec      = 2.0f;
    constexpr float kFinalFadeOutSec   = 0.4f;

    float screenAlpha = std::clamp(elapsed / kFadeInSec, 0.f, 1.f);
    float bgAlpha     = std::clamp((elapsed - 0.05f) / kBgFadeInSec, 0.f, 1.f);

    // --- Check if "Ready" (FadingOut phase = map load done, fade animation plays) ---
    bool isReady = (m_loadingPhase == LoadingPhase::FadingOut);

    float readyFadeOut = 1.f;
    float readyTextAlpha = 0.f;
    bool shouldTransition = false;

    if (isReady && !m_lsHitReady)
    {
        m_lsHitReady = true;
        m_lsReadyTime = LsClock::now();
    }

    if (m_lsHitReady)
    {
        float re = std::chrono::duration<float>(LsClock::now() - m_lsReadyTime).count();

        // Phase A: status text fades out (0 .. kStatusFadeOutSec)
        float fadeT = std::clamp(re / kStatusFadeOutSec, 0.f, 1.f);
        readyFadeOut = 1.f - fadeT;

        // Phase B: "Ready" label appears (kStatusFadeOutSec .. + kReadyLabelSec)
        float readyStart = kStatusFadeOutSec;
        if (re >= readyStart)
            readyTextAlpha = std::clamp((re - readyStart) / 0.1f, 0.f, 1.f);

        // Phase C: hold on completed screen (kStatusFadeOutSec + kReadyLabelSec .. + kReadyHoldSec)
        float holdEnd = kStatusFadeOutSec + kReadyLabelSec + kReadyHoldSec;

        // Phase D: final fade out to black
        float afterHold = re - holdEnd;
        if (afterHold > 0.f)
        {
            float finalFade = std::clamp(afterHold / kFinalFadeOutSec, 0.f, 1.f);
            screenAlpha = 1.f - finalFade;
            if (finalFade >= 1.f)
                shouldTransition = true;
        }
    }

    // --- Fullscreen ImGui window ---
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.039f, 0.055f, 0.071f, screenAlpha));

    ImGui::Begin("##MatchLoadingScreen", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // --- Background image (contain-fit) ---
    ImTextureID bgTex = m_lsBgPath.empty() ? nullptr : m_lsTexCache.GetTexture(m_lsBgPath);
    if (bgTex && screenAlpha > 0.01f)
    {
        // We need the actual image dimensions for contain-fit.
        // TextureCache returns an SRV; query the underlying texture for size.
        ID3D11ShaderResourceView* srv = static_cast<ID3D11ShaderResourceView*>(bgTex);
        Microsoft::WRL::ComPtr<ID3D11Resource> res;
        srv->GetResource(res.GetAddressOf());
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex2d;
        res.As(&tex2d);

        float imgW = display.x, imgH = display.y;
        if (tex2d)
        {
            D3D11_TEXTURE2D_DESC desc;
            tex2d->GetDesc(&desc);
            imgW = static_cast<float>(desc.Width);
            imgH = static_cast<float>(desc.Height);
        }

        // Contain-fit: scale to fit within window, maintain aspect ratio
        float scaleX = display.x / imgW;
        float scaleY = display.y / imgH;
        float scale = (std::min)(scaleX, scaleY);
        float drawW = imgW * scale;
        float drawH = imgH * scale;
        float offX = (display.x - drawW) * 0.5f;
        float offY = (display.y - drawH) * 0.5f;

        ImU32 imgCol = IM_COL32(255, 255, 255, static_cast<int>(255 * bgAlpha * screenAlpha));
        dl->AddImage(bgTex, ImVec2(offX, offY), ImVec2(offX + drawW, offY + drawH),
                     ImVec2(0, 0), ImVec2(1, 1), imgCol);
    }

    // --- Dark overlay ---
    {
        ImU32 overlayCol = IM_COL32(0, 0, 0, static_cast<int>(115 * screenAlpha));
        dl->AddRectFilled(ImVec2(0, 0), display, overlayCol);
    }

    // --- Ensure cape textures are ready ---
    if (screenAlpha > 0.01f)
    {
        if (!m_capeCacheInitialized)
            InitCapeCache();
        if (!m_capeTexturesResolved)
            ResolveCapeTextures();
    }

    // --- Team info cards + header pill (computed together for vertical alignment) ---
    if (screenAlpha > 0.01f)
        DrawMatchInfoOverlay(dl, display, screenAlpha);

    // --- Progress bar geometry ---
    float barW = (std::min)(800.f, display.x - 120.f);
    float barH = 6.f;
    float barX = (display.x - barW) * 0.5f;
    float barY = display.y - 40.f - barH;
    float barRight = barX + barW;

    // --- Progress bar ---
    {
        float barAlpha = readyFadeOut * screenAlpha;
        ImU32 bgCol   = IM_COL32(40, 44, 52, static_cast<int>(255 * barAlpha));
        ImU32 fillCol = IM_COL32(74, 144, 216, static_cast<int>(230 * barAlpha));
        float progress = std::clamp(m_loadProgress, 0.f, 1.f);
        dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), bgCol, 3.f);
        dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW * progress, barY + barH), fillCol, 3.f);
    }

    // --- Status text lines (right-aligned above bar) ---
    if (readyFadeOut > 0.01f && screenAlpha > 0.01f && !m_lsHitReady)
    {
        float alpha = readyFadeOut * screenAlpha;
        ImU32 labelCol   = IM_COL32(255, 255, 255, static_cast<int>(210 * alpha));
        ImU32 counterCol = IM_COL32(255, 255, 255, static_cast<int>(250 * alpha));

        float lineH = ImGui::GetFontSize() + 2.f;
        float textY = barY - 8.f;

        auto drawStatusLine = [&](const std::string& label, const std::string& counter)
        {
            std::string full = label + counter;
            ImVec2 fullSz = ImGui::CalcTextSize(full.c_str());
            ImVec2 labelSz = ImGui::CalcTextSize(label.c_str());
            textY -= lineH;
            float lineX = barRight - fullSz.x;
            LsDrawTextWithShadow(dl, ImVec2(lineX, textY), labelCol, label.c_str());
            LsDrawTextWithShadow(dl, ImVec2(lineX + labelSz.x, textY), counterCol, counter.c_str());
        };

        bool showedAny = false;

        // Map loading status
        if (m_loadingPhase == LoadingPhase::Init)
        {
            drawStatusLine("Loading map geometry...", "");
            showedAny = true;
        }
        else if (m_loadingPhase == LoadingPhase::PropModels && m_totalPropFilenames > 0)
        {
            drawStatusLine("Loading map models  ",
                "[" + LsFormatWithCommas(m_propModelLoadIndex) +
                " / " + LsFormatWithCommas(m_totalPropFilenames) + "]");
            showedAny = true;
        }
        else if (m_loadingPhase == LoadingPhase::PlaceProps && m_agentModelsLoading && !m_agentModelsLoaded)
        {
            if (!m_bgLoadDone) {
                int prog = m_bgLoadProgress.load();
                auto subPhase = static_cast<AgentLoadSubPhase>(m_bgLoadSubPhase.load());
                const char* subLabel = "";
                switch (subPhase) {
                    case AgentLoadSubPhase::ParsingModel:         subLabel = "Parsing model geometry..."; break;
                    case AgentLoadSubPhase::LoadingTextures:       subLabel = "Loading textures..."; break;
                    case AgentLoadSubPhase::DiscoveringAnimations: subLabel = "Discovering animations..."; break;
                    case AgentLoadSubPhase::ScanningReferences:    subLabel = "Scanning animation references..."; break;
                    case AgentLoadSubPhase::ScanningMFT:           subLabel = "Scanning DAT file for animations..."; break;
                    case AgentLoadSubPhase::BuildingAnimData:      subLabel = "Building animation data..."; break;
                    default: break;
                }
                ImU32 subCol = IM_COL32(180, 180, 180, static_cast<int>(170 * alpha));
                if (subLabel[0]) {
                    ImVec2 subSz = ImGui::CalcTextSize(subLabel);
                    float subX = barRight - subSz.x;
                    textY -= lineH;
                    LsDrawTextWithShadow(dl, ImVec2(subX, textY), subCol, subLabel);
                }
                int display = std::min(prog + 1, m_bgLoadTotal);
                drawStatusLine("Loading 3D models  ",
                    "[" + std::to_string(display) + " / " + std::to_string(m_bgLoadTotal) + "]");
            } else {
                int total3d = static_cast<int>(m_agentModelCreateOrder.size());
                drawStatusLine("Creating 3D model resources  ",
                    "[" + std::to_string(m_agentModelCreateIndex) + " / " + std::to_string(total3d) + "]");
            }
            showedAny = true;
        }
        else if (m_loadingPhase == LoadingPhase::PlaceProps && m_totalPropInstances > 0)
        {
            drawStatusLine("Placing props  ",
                "[" + LsFormatWithCommas(m_propPlaceIndex) +
                " / " + LsFormatWithCommas(m_totalPropInstances) + "]");
            showedAny = true;
        }

        // Match data parsing (async, can run in parallel with map loading)
        {
            int agentDone = 0, agentTotal = 0, stocDone = 0, stocTotal = 0;
            bool agentActive = false, stocActive = false;
            if (m_replayCtx.agentParseProgress && !m_replayCtx.agentsLoaded)
            {
                agentDone = m_replayCtx.agentParseProgress->files_done.load();
                agentTotal = m_replayCtx.agentParseProgress->files_total.load();
                agentActive = (agentTotal > 0);
            }
            if (m_replayCtx.stocParseProgress && !m_replayCtx.stocLoaded)
            {
                stocDone = m_replayCtx.stocParseProgress->files_done.load();
                stocTotal = m_replayCtx.stocParseProgress->files_total.load();
                stocActive = (stocTotal > 0);
            }

            if (agentActive || stocActive)
            {
                int totalDone = agentDone + stocDone;
                int totalAll = agentTotal + stocTotal;
                drawStatusLine("Loading match data  ",
                    "[" + LsFormatWithCommas(totalDone) +
                    " / " + LsFormatWithCommas(totalAll) + "]");
                showedAny = true;
            }
        }

        if (!showedAny && m_loadingPhase == LoadingPhase::Validate)
        {
            drawStatusLine("Initializing...", "");
        }
    }

    // --- "Ready" text (centered, fades in) ---
    if (readyTextAlpha > 0.01f && screenAlpha > 0.01f)
    {
        const char* readyText = "Ready";
        ImVec2 sz = ImGui::CalcTextSize(readyText);
        float rx = (display.x - sz.x) * 0.5f;
        float ry = barY - 30.f;
        ImU32 readyCol = IM_COL32(255, 255, 255, static_cast<int>(230 * readyTextAlpha * screenAlpha));
        LsDrawTextWithShadow(dl, ImVec2(rx, ry), readyCol, readyText);
    }

    // --- Error text ---
    if (m_loadingPhase == LoadingPhase::Error && !m_errorMsg.empty() && screenAlpha > 0.01f)
    {
        std::string errDisplay = "Error: " + m_errorMsg;
        ImVec2 sz = ImGui::CalcTextSize(errDisplay.c_str());
        float ex = (display.x - sz.x) * 0.5f;
        float ey = display.y * 0.5f;
        ImU32 errCol = IM_COL32(255, 96, 96, static_cast<int>(255 * screenAlpha));
        LsDrawTextWithShadow(dl, ImVec2(ex, ey), errCol, errDisplay.c_str());
    }

    ImGui::End();
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(3);

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    if (prevCtx)
        ImGui::SetCurrentContext(prevCtx);

    m_deviceResources->Present();

    // Transition to Ready phase after all fades complete
    if (shouldTransition) {
        m_loadingPhase = LoadingPhase::Ready;
        m_matchOverlayStartTime = m_debugTimeline;
        m_matchOverlayActive = true;
        InitAudioEngine();
        m_replayCtx.isPlaying = true;
    }
}

// ---------------------------------------------------------------------------
// Guild cape loading screen helpers
// ---------------------------------------------------------------------------

void ReplayWindow::InitCapeCache()
{
    if (m_capeCacheInitialized) return;
    m_capeCacheInitialized = true;

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();

    std::filesystem::path assetRoot;
    for (int i = 0; i < 6; i++)
    {
        auto candidate = dir / "Textures" / "CapeAssets";
        if (std::filesystem::exists(candidate))
        {
            assetRoot = candidate;
            break;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path())
            break;
        dir = dir.parent_path();
    }

    if (!assetRoot.empty())
        m_capeCache.Init(m_deviceResources->GetD3DDevice(), assetRoot);
}

void ReplayWindow::ResolveCapeTextures()
{
    if (m_capeTexturesResolved) return;
    if (!m_capeCache.IsReady()) return;
    m_capeTexturesResolved = true;

    // Ensure folder tags are parsed (may run before Tick processes agent data)
    if (m_folderTag1.empty() && m_folderTag2.empty()) {
        const auto& fn = m_matchMeta.folder_name;
        auto vs = fn.find("]vs[");
        if (vs != std::string::npos) {
            auto open1 = fn.rfind('[', vs);
            auto close2 = fn.find(']', vs + 4);
            if (open1 != std::string::npos && close2 != std::string::npos) {
                m_folderTag1 = fn.substr(open1 + 1, vs - open1 - 1);
                m_folderTag2 = fn.substr(vs + 4, close2 - (vs + 4));
            }
        }
    }

    auto FindGuildByTag = [&](const std::string& tag) -> const GuildMeta* {
        if (tag.empty()) return nullptr;
        for (const auto& [id, gm] : m_matchMeta.guilds)
            if (gm.tag == tag) return &gm;
        return nullptr;
    };

    auto getTeamCape = [&](const std::string& partyId, const std::string& folderTag) -> ImTextureID
    {
        // Prefer folder-name tag (authoritative from GW match list)
        auto* fg = FindGuildByTag(folderTag);
        if (fg && !fg->tag.empty())
            return m_capeCache.GetOrCreate(fg->tag, fg->cape);

        // Fallback: find the dominant guild for this party
        auto pit = m_matchMeta.parties.find(partyId);
        if (pit == m_matchMeta.parties.end() || pit->second.players.empty())
        {
            auto git = m_matchMeta.guilds.find(partyId);
            if (git != m_matchMeta.guilds.end() && !git->second.tag.empty())
                return m_capeCache.GetOrCreate(git->second.tag, git->second.cape);
            return nullptr;
        }

        std::map<int, int> guildCounts;
        for (const auto& p : pit->second.players)
            if (p.guild_id > 0) guildCounts[p.guild_id]++;

        if (guildCounts.empty()) return nullptr;

        int bestGuildId = 0, bestCount = 0;
        for (const auto& [gid, cnt] : guildCounts)
            if (cnt > bestCount) { bestGuildId = gid; bestCount = cnt; }

        auto git = m_matchMeta.guilds.find(std::to_string(bestGuildId));
        if (git == m_matchMeta.guilds.end()) return nullptr;

        return m_capeCache.GetOrCreate(git->second.tag, git->second.cape);
    };

    m_capeTexTeam1 = getTeamCape("1", m_folderTag1);
    m_capeTexTeam2 = getTeamCape("2", m_folderTag2);
}

// ---------------------------------------------------------------------------
// Match info overlay: header pill + team cards (shared by loading & replay)
// ---------------------------------------------------------------------------

void ReplayWindow::DrawMatchInfoOverlay(ImDrawList* dl, ImVec2 display, float alpha)
{
    if (alpha < 0.01f) return;

    ImFont* font = ImGui::GetFont();

    auto resolveOthersUITex = [&](const char* filename) -> ImTextureID {
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return nullptr;
        auto dir = std::filesystem::path(exePath).parent_path();
        for (int i = 0; i < 6; i++)
        {
            auto p = dir / "Textures" / "Others_UI" / filename;
            if (std::filesystem::exists(p))
                return m_lsTexCache.GetTexture(p.string());
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
        return nullptr;
    };
    ImTextureID blueGradTex = resolveOthersUITex("texture_265588.dds");
    ImTextureID redGradTex  = resolveOthersUITex("texture_265590.dds");

    std::string name1, tag1, name2, tag2;
    int rank1 = 0, rating1 = 0, rank2 = 0, rating2 = 0;
    GetPartyGuildInfo(m_matchMeta, "1", name1, tag1, rank1, rating1, m_folderTag1);
    GetPartyGuildInfo(m_matchMeta, "2", name2, tag2, rank2, rating2, m_folderTag2);

    float capeW = 72.0f;
    float capeH = 144.0f;

    if (display.y < capeH + 280.f)
    {
        float sc = (display.y - 280.f) / capeH;
        sc = std::clamp(sc, 0.25f, 1.0f);
        capeW *= sc;
        capeH *= sc;
    }

    float nameFontSize  = 22.f;
    float tagFontSize   = 18.f;
    float statNumSize   = 18.f;
    float statLabelSize = 13.f;
    float vsFontSize    = 20.f;

    float cardPadX = 20.f;
    float cardPadY = 16.f;
    float textGap  = 6.f;

    std::string nameTag1 = name1 + (tag1.empty() ? "" : "  [" + tag1 + "]");
    std::string nameTag2 = name2 + (tag2.empty() ? "" : "  [" + tag2 + "]");
    ImVec2 nameTagSz1 = font->CalcTextSizeA(nameFontSize, FLT_MAX, 0.f, nameTag1.c_str());
    ImVec2 nameTagSz2 = font->CalcTextSizeA(nameFontSize, FLT_MAX, 0.f, nameTag2.c_str());

    char rankLine1[64], rankLine2[64];
    snprintf(rankLine1, sizeof(rankLine1), "#%d", rank1);
    snprintf(rankLine2, sizeof(rankLine2), "#%d", rank2);
    std::string rankLabelStr = "RANK";
    ImVec2 rankLabelSz = font->CalcTextSizeA(statLabelSize, FLT_MAX, 0.f, rankLabelStr.c_str());
    ImVec2 rankNumSz1  = font->CalcTextSizeA(statNumSize, FLT_MAX, 0.f, rankLine1);
    ImVec2 rankNumSz2  = font->CalcTextSizeA(statNumSize, FLT_MAX, 0.f, rankLine2);

    char ratingLine1[64], ratingLine2[64];
    snprintf(ratingLine1, sizeof(ratingLine1), "%d", rating1);
    snprintf(ratingLine2, sizeof(ratingLine2), "%d", rating2);
    std::string ratingLabelStr = "RATING";
    ImVec2 ratingLabelSz = font->CalcTextSizeA(statLabelSize, FLT_MAX, 0.f, ratingLabelStr.c_str());
    ImVec2 ratingNumSz1  = font->CalcTextSizeA(statNumSize, FLT_MAX, 0.f, ratingLine1);
    ImVec2 ratingNumSz2  = font->CalcTextSizeA(statNumSize, FLT_MAX, 0.f, ratingLine2);

    float statRowH = (std::max)(statNumSize, statLabelSize) + 2.f;
    float profIconSize = 22.f;
    float profIconGap = 12.f;
    float textBlockH = nameTagSz1.y + textGap + statRowH + textGap + statRowH + profIconGap + profIconSize;

    auto resolveProfBasePath = [&]() -> std::string {
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return "";
        auto dir = std::filesystem::path(exePath).parent_path();
        for (int i = 0; i < 6; i++)
        {
            auto p = dir / "Textures" / "professions";
            if (std::filesystem::exists(p))
                return p.string();
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
        return "";
    };
    std::string profBasePath = resolveProfBasePath();

    auto getSortedPlayers = [](const MatchMeta& meta, const std::string& partyId) {
        std::vector<PlayerMeta> sorted;
        auto pit = meta.parties.find(partyId);
        if (pit != meta.parties.end())
            sorted = pit->second.players;
        std::sort(sorted.begin(), sorted.end(),
                  [](const PlayerMeta& a, const PlayerMeta& b) { return a.player_number < b.player_number; });
        return sorted;
    };

    float cardContentH = (std::max)(capeH, textBlockH);
    float cardH = cardContentH + cardPadY * 2.f;

    float statLineW1 = (std::max)(rankLabelSz.x + 8.f + rankNumSz1.x,
                                   ratingLabelSz.x + 8.f + ratingNumSz1.x);
    float statLineW2 = (std::max)(rankLabelSz.x + 8.f + rankNumSz2.x,
                                   ratingLabelSz.x + 8.f + ratingNumSz2.x);
    float maxTextW1 = (std::max)(nameTagSz1.x, statLineW1);
    float maxTextW2 = (std::max)(nameTagSz2.x, statLineW2);
    float maxTextW  = (std::max)(maxTextW1, maxTextW2);

    float innerGap = 16.f;
    float minIconRowW = 8 * profIconSize + 7 * 3.f;
    float minTextW = (std::max)(maxTextW, minIconRowW);
    float cardW = cardPadX + capeW + innerGap + minTextW + cardPadX;

    float vsGap = 80.f;
    float totalW = cardW + vsGap + cardW;

    const char* mapName = GetMapNameForLoading(m_matchMeta.map_id);
    char dateLine[128];
    snprintf(dateLine, sizeof(dateLine), "%04d/%02d/%02d",
             m_matchMeta.year, m_matchMeta.month, m_matchMeta.day);

    std::string headerStr = std::string(dateLine);
    if (!m_matchMeta.occasion.empty())
        headerStr += "  \xC2\xB7  " + m_matchMeta.occasion;
    headerStr += "  \xC2\xB7  " + std::string(mapName ? mapName : "Unknown Map");

    // --- Header: same font / size / shadow / placement as jumbo messages, custom color ---
    ImFont* jumboFont = m_latoBoldBig ? m_latoBoldBig : ImGui::GetFont();
    const float jumboFontSize = jumboFont->FontSize;
    ImVec2 headerSz = jumboFont->CalcTextSizeA(jumboFontSize, FLT_MAX, 0.f, headerStr.c_str());

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float posX = m_uiLayout.useCustom ? m_uiLayout.jumboX : 0.50f;
    float posY = m_uiLayout.useCustom ? m_uiLayout.jumboY : 0.30f;
    float cx = vp->Pos.x + vp->Size.x * posX;
    float topY = vp->Pos.y + vp->Size.y * posY;
    float tx = cx - headerSz.x * 0.5f;
    float ty = topY;

    ImU32 jumboShadow = IM_COL32(0, 0, 0, static_cast<int>(alpha * 230));
    ImU32 headerCol = IM_COL32(255, 238, 187, static_cast<int>(255 * alpha));
    dl->AddText(jumboFont, jumboFontSize, ImVec2(tx, ty + 1.f), jumboShadow, headerStr.c_str());
    dl->AddText(jumboFont, jumboFontSize, ImVec2(tx, ty), headerCol, headerStr.c_str());

    float cardY = vp->Pos.y + (vp->Size.y - cardH) * 0.5f;
    float startX = vp->Pos.x + (vp->Size.x - totalW) * 0.5f;

    float cardR = 12.f;
    ImU32 capeCol = IM_COL32(255, 255, 255, static_cast<int>(255 * alpha));
    ImU32 gradCol = IM_COL32(255, 255, 255, static_cast<int>(200 * alpha));

    // --- Team 1 card (left) ---
    {
        float cx = startX;
        ImU32 cardBg = IM_COL32(8, 10, 14, static_cast<int>(180 * alpha));
        dl->AddRectFilled(ImVec2(cx, cardY), ImVec2(cx + cardW, cardY + cardH), cardBg, cardR);

        if (redGradTex)
        {
            dl->PushClipRect(ImVec2(cx, cardY), ImVec2(cx + cardW, cardY + cardH));
            dl->AddImage(redGradTex, ImVec2(cx, cardY), ImVec2(cx + cardW, cardY + cardH),
                         ImVec2(0,0), ImVec2(1,1), gradCol);
            dl->PopClipRect();
        }

        float capePosX = cx + cardPadX;
        float capePosY = cardY + (cardH - capeH) * 0.5f;
        if (m_capeTexTeam1)
            dl->AddImage(m_capeTexTeam1, ImVec2(capePosX, capePosY),
                         ImVec2(capePosX + capeW, capePosY + capeH), ImVec2(0,0), ImVec2(1,1), capeCol);

        float textX = capePosX + capeW + innerGap;
        float textY = cardY + (cardH - textBlockH) * 0.5f;

        ImU32 nameCol = IM_COL32(255, 120, 140, static_cast<int>(255 * alpha));
        ImU32 tagCol  = IM_COL32(255, 120, 140, static_cast<int>(180 * alpha));
        ImU32 numCol  = IM_COL32(240, 200, 80, static_cast<int>(255 * alpha));
        ImU32 lblCol  = IM_COL32(210, 180, 180, static_cast<int>(200 * alpha));

        LsDrawTextCrispShadow(dl, font, nameFontSize, ImVec2(textX, textY), nameCol, name1.c_str());
        if (!tag1.empty())
        {
            float tagOffX = textX + font->CalcTextSizeA(nameFontSize, FLT_MAX, 0.f, name1.c_str()).x;
            std::string tagPart = "  [" + tag1 + "]";
            LsDrawTextCrispShadow(dl, font, tagFontSize, ImVec2(tagOffX, textY + (nameFontSize - tagFontSize) * 0.5f), tagCol, tagPart.c_str());
        }
        textY += nameTagSz1.y + textGap;

        if (rank1 > 0)
        {
            float labelYOff = (statNumSize - statLabelSize) * 0.5f;
            LsDrawTextCrispShadow(dl, font, statLabelSize, ImVec2(textX, textY + labelYOff), lblCol, rankLabelStr.c_str());
            float afterLabel = textX + rankLabelSz.x + 8.f;
            LsDrawTextCrispShadow(dl, font, statNumSize, ImVec2(afterLabel, textY), numCol, rankLine1);
        }
        textY += statRowH + textGap;

        if (rating1 > 0)
        {
            float labelYOff = (statNumSize - statLabelSize) * 0.5f;
            LsDrawTextCrispShadow(dl, font, statLabelSize, ImVec2(textX, textY + labelYOff), lblCol, ratingLabelStr.c_str());
            float afterLabel = textX + ratingLabelSz.x + 8.f;
            LsDrawTextCrispShadow(dl, font, statNumSize, ImVec2(afterLabel, textY), numCol, ratingLine1);
        }
        textY += statRowH + profIconGap;

        if (!profBasePath.empty())
        {
            auto players1 = getSortedPlayers(m_matchMeta, "1");
            int n1 = static_cast<int>(players1.size());
            if (n1 > 0)
            {
                float iconSpacing = 3.f;
                float iconX = textX;
                ImU32 iconCol = IM_COL32(255, 255, 255, static_cast<int>(240 * alpha));
                for (const auto& p : players1)
                {
                    if (p.primary >= 1 && p.primary <= 10)
                    {
                        char fn[16];
                        snprintf(fn, sizeof(fn), "%d.png", p.primary);
                        auto iconPath = profBasePath + "\\" + fn;
                        ImTextureID tex = m_lsTexCache.GetTexture(iconPath);
                        if (tex)
                            dl->AddImage(tex, ImVec2(iconX, textY),
                                         ImVec2(iconX + profIconSize, textY + profIconSize),
                                         ImVec2(0,0), ImVec2(1,1), iconCol);
                    }
                    iconX += profIconSize + iconSpacing;
                }
            }
        }
    }

    // --- "V S" divider ---
    {
        std::string vsText = "V S";
        ImVec2 vsSz = font->CalcTextSizeA(vsFontSize, FLT_MAX, 0.f, vsText.c_str());
        float vsX = startX + cardW + (vsGap - vsSz.x) * 0.5f;
        float vsY = cardY + (cardH - vsSz.y) * 0.5f;
        ImU32 vsCol = IM_COL32(200, 200, 200, static_cast<int>(180 * alpha));
        LsDrawTextCrispShadow(dl, font, vsFontSize, ImVec2(vsX, vsY), vsCol, vsText.c_str());
    }

    // --- Team 2 card (right) ---
    {
        float cx = startX + cardW + vsGap;
        ImU32 cardBg2 = IM_COL32(8, 10, 14, static_cast<int>(180 * alpha));
        dl->AddRectFilled(ImVec2(cx, cardY), ImVec2(cx + cardW, cardY + cardH), cardBg2, cardR);

        if (blueGradTex)
        {
            dl->PushClipRect(ImVec2(cx, cardY), ImVec2(cx + cardW, cardY + cardH));
            dl->AddImage(blueGradTex, ImVec2(cx, cardY), ImVec2(cx + cardW, cardY + cardH),
                         ImVec2(0,0), ImVec2(1,1), gradCol);
            dl->PopClipRect();
        }

        float capePosX = cx + cardW - cardPadX - capeW;
        float capePosY = cardY + (cardH - capeH) * 0.5f;
        if (m_capeTexTeam2)
            dl->AddImage(m_capeTexTeam2, ImVec2(capePosX, capePosY),
                         ImVec2(capePosX + capeW, capePosY + capeH), ImVec2(0,0), ImVec2(1,1), capeCol);

        float textRightEdge = capePosX - innerGap;
        float textY = cardY + (cardH - textBlockH) * 0.5f;

        ImU32 nameCol = IM_COL32(120, 180, 255, static_cast<int>(255 * alpha));
        ImU32 tagCol  = IM_COL32(120, 180, 255, static_cast<int>(180 * alpha));
        ImU32 numCol  = IM_COL32(240, 200, 80, static_cast<int>(255 * alpha));
        ImU32 lblCol  = IM_COL32(180, 190, 210, static_cast<int>(200 * alpha));

        float ntW2 = nameTagSz2.x;
        LsDrawTextCrispShadow(dl, font, nameFontSize, ImVec2(textRightEdge - ntW2, textY), nameCol, name2.c_str());
        if (!tag2.empty())
        {
            ImVec2 nameSz2Only = font->CalcTextSizeA(nameFontSize, FLT_MAX, 0.f, name2.c_str());
            std::string tagPart = "  [" + tag2 + "]";
            float tagPartX = textRightEdge - ntW2 + nameSz2Only.x;
            LsDrawTextCrispShadow(dl, font, tagFontSize, ImVec2(tagPartX, textY + (nameFontSize - tagFontSize) * 0.5f), tagCol, tagPart.c_str());
        }
        textY += nameTagSz2.y + textGap;

        if (rank2 > 0)
        {
            float lineW = rankNumSz2.x + 8.f + rankLabelSz.x;
            float lineX = textRightEdge - lineW;
            float labelYOff = (statNumSize - statLabelSize) * 0.5f;
            LsDrawTextCrispShadow(dl, font, statNumSize, ImVec2(lineX, textY), numCol, rankLine2);
            LsDrawTextCrispShadow(dl, font, statLabelSize, ImVec2(lineX + rankNumSz2.x + 8.f, textY + labelYOff), lblCol, rankLabelStr.c_str());
        }
        textY += statRowH + textGap;

        if (rating2 > 0)
        {
            float lineW = ratingNumSz2.x + 8.f + ratingLabelSz.x;
            float lineX = textRightEdge - lineW;
            float labelYOff = (statNumSize - statLabelSize) * 0.5f;
            LsDrawTextCrispShadow(dl, font, statNumSize, ImVec2(lineX, textY), numCol, ratingLine2);
            LsDrawTextCrispShadow(dl, font, statLabelSize, ImVec2(lineX + ratingNumSz2.x + 8.f, textY + labelYOff), lblCol, ratingLabelStr.c_str());
        }
        textY += statRowH + profIconGap;

        if (!profBasePath.empty())
        {
            auto players2 = getSortedPlayers(m_matchMeta, "2");
            int n2 = static_cast<int>(players2.size());
            if (n2 > 0)
            {
                float iconSpacing = 3.f;
                float iconX = textRightEdge - profIconSize;
                ImU32 iconCol = IM_COL32(255, 255, 255, static_cast<int>(240 * alpha));
                for (const auto& p : players2)
                {
                    if (p.primary >= 1 && p.primary <= 10)
                    {
                        char fn[16];
                        snprintf(fn, sizeof(fn), "%d.png", p.primary);
                        auto iconPath = profBasePath + "\\" + fn;
                        ImTextureID tex = m_lsTexCache.GetTexture(iconPath);
                        if (tex)
                            dl->AddImage(tex, ImVec2(iconX, textY),
                                         ImVec2(iconX + profIconSize, textY + profIconSize),
                                         ImVec2(0,0), ImVec2(1,1), iconCol);
                    }
                    iconX -= profIconSize + iconSpacing;
                }
            }
        }
    }
}
