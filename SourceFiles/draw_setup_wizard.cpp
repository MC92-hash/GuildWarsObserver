#include "pch.h"
#include "draw_setup_wizard.h"
#include "draw_gui_for_open_dat_file.h"
#include "SetupConfig.h"
#include "GuiGlobalConstants.h"
#include "FolderWatcher.h"
#include "TextureCache.h"
#include "imgui.h"
#include <filesystem>
#include <chrono>
#include <ctime>

// ─── EULA rendering ──────────────────────────────────────────────────────────

static void DrawEulaContent()
{
    ImVec4 goldCol(0.83f, 0.63f, 0.13f, 0.90f);
    ImVec4 bodyCol(1.f, 1.f, 1.f, 0.80f);
    ImVec4 linkCol(0.83f, 0.63f, 0.13f, 0.75f);
    float wrapW = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapW);

    ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.90f), "GW OBSERVER -- END USER LICENCE AGREEMENT");
    ImGui::Dummy(ImVec2(0, 8.f));
    ImGui::TextColored(bodyCol, "GW Observer is a free tool for viewing and analysing Guild Wars 1 GvG match replays.");
    ImGui::Dummy(ImVec2(0, 12.f));

    ImGui::TextColored(goldCol, "-- LICENCE --");
    ImGui::Dummy(ImVec2(0, 4.f));
    ImGui::TextColored(bodyCol,
        "GW Observer is source-available software. The source code is provided for "
        "transparency and personal review only.");
    ImGui::Dummy(ImVec2(0, 6.f));
    ImGui::TextColored(bodyCol, "Any project that forks, derives from, or incorporates code from GW Observer must:");
    ImGui::Dummy(ImVec2(0, 4.f));
    ImGui::Indent(16.f);
    ImGui::TextColored(bodyCol, "1. Credit the original GuildWarsMapBrowser project under the MIT Licence:");
    ImGui::TextColored(bodyCol, "    Jonathan Bjorn Greve");
    ImGui::TextColored(linkCol, "    https://github.com/Jonathan-Greve/GuildWarsMapBrowser");
    ImGui::Dummy(ImVec2(0, 4.f));
    ImGui::TextColored(bodyCol, "2. Credit GW Observer and its authors:");
    ImGui::TextColored(bodyCol, "    Purif / MC92-hash");
    ImGui::TextColored(linkCol, "    https://github.com/MC92-hash/GWObserver");
    ImGui::Dummy(ImVec2(0, 4.f));
    ImGui::TextColored(bodyCol, "3. Clearly state all modifications made to the original code.");
    ImGui::Unindent(16.f);
    ImGui::Dummy(ImVec2(0, 6.f));
    ImGui::TextColored(bodyCol,
        "Redistribution of GW Observer under a different name, or use of its code, in whole "
        "or in part, to build any of the following is not permitted without explicit written "
        "permission from Purif / MC92-hash:");
    ImGui::Dummy(ImVec2(0, 4.f));
    ImGui::Indent(16.f);
    ImGui::TextColored(bodyCol, "- Guild Wars match replay tools");
    ImGui::TextColored(bodyCol, "- GvG analysis or statistics tools");
    ImGui::TextColored(bodyCol, "- Spectator or observer tools for Guild Wars");
    ImGui::TextColored(bodyCol, "- Any tool serving a similar purpose to GW Observer");
    ImGui::Unindent(16.f);
    ImGui::Dummy(ImVec2(0, 12.f));

    ImGui::TextColored(goldCol, "-- DATA --");
    ImGui::Dummy(ImVec2(0, 4.f));
    ImGui::TextColored(bodyCol,
        "Match data displayed in GW Observer is for personal viewing only and may not be:");
    ImGui::Dummy(ImVec2(0, 4.f));
    ImGui::Indent(16.f);
    ImGui::TextColored(bodyCol, "- Extracted or scraped programmatically");
    ImGui::TextColored(bodyCol, "- Redistributed in any form");
    ImGui::TextColored(bodyCol, "- Used to build or populate competing websites or tools");
    ImGui::Unindent(16.f);
    ImGui::Dummy(ImVec2(0, 6.f));
    ImGui::TextColored(bodyCol,
        "Match data may not be extracted, stored, or redistributed by any fork or "
        "derivative work without explicit written permission from Purif / MC92-hash.");
    ImGui::Dummy(ImVec2(0, 12.f));

    ImGui::TextColored(goldCol, "-- YOUR DATA AND PRIVACY --");
    ImGui::Dummy(ImVec2(0, 4.f));
    ImGui::TextColored(bodyCol,
        "GW Observer does not collect, store, or transmit any personal data. Your Guild Wars "
        "account credentials are never accessed. The DAT file is opened in read-only mode "
        "and its contents are never uploaded or shared.");
    ImGui::Dummy(ImVec2(0, 12.f));

    ImGui::TextColored(goldCol, "-- CREDITS --");
    ImGui::Dummy(ImVec2(0, 4.f));
    ImGui::TextColored(bodyCol, "Maverick");
    ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.60f),
        "Back-end infrastructure, tool improvements, and testing. "
        "GW Observer's match replay pipeline would not be possible without Maverick's "
        "continued work on the server side and relentless testing efforts.");
    ImGui::Dummy(ImVec2(0, 12.f));

    ImGui::TextColored(goldCol, "-- THIRD PARTY ATTRIBUTION --");
    ImGui::Dummy(ImVec2(0, 4.f));
    ImGui::TextColored(bodyCol,
        "This software is based in part on GuildWarsMapBrowser by Jonathan Bjorn Greve, "
        "used under the MIT Licence.");
    ImGui::TextColored(bodyCol, "Copyright Jonathan Bjorn Greve");
    ImGui::TextColored(linkCol, "https://github.com/Jonathan-Greve/GuildWarsMapBrowser");
    ImGui::Dummy(ImVec2(0, 12.f));

    ImGui::TextColored(goldCol, "-- DISCLAIMER --");
    ImGui::Dummy(ImVec2(0, 4.f));
    ImGui::TextColored(bodyCol,
        "Guild Wars is a registered trademark of NCSoft Corporation. GW Observer is an "
        "unofficial fan tool and is not affiliated with NCSoft or ArenaNet in any way.");
    ImGui::Dummy(ImVec2(0, 6.f));
    ImGui::TextColored(bodyCol,
        "This software is provided as-is, without warranty of any kind. The author accepts "
        "no responsibility for any damage or data loss arising from its use.");

    ImGui::PopTextWrapPos();
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

namespace
{
    static std::string GetLoadingScreenPath()
    {
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
            return "";
        auto dir = std::filesystem::path(exePath).parent_path();
        for (int i = 0; i < 6; i++)
        {
            auto p = dir / "Textures" / "Launch_screen" / "GWOBS_Loading_Screen_1.png";
            if (std::filesystem::exists(p))
                return p.string();
            if (!dir.has_parent_path() || dir == dir.parent_path())
                break;
            dir = dir.parent_path();
        }
        return "";
    }

    static std::string GetTodayString()
    {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm lt;
        localtime_s(&lt, &t);
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
        return buf;
    }

    // Validation helpers are now in draw_setup_wizard.h
    // TryAutoDetect is now TryAutoDetectDat in draw_setup_wizard.h
    static auto TryAutoDetect = TryAutoDetectDat;

    static void DrawShadowedText(ImDrawList* dl, ImFont* font, float size,
                                  ImVec2 pos, ImU32 col, const char* text)
    {
        ImU32 shadow = IM_COL32(0, 0, 0, 200);
        dl->AddText(font, size, ImVec2(pos.x - 1.f, pos.y + 1.f), shadow, text);
        dl->AddText(font, size, ImVec2(pos.x + 1.f, pos.y + 1.f), shadow, text);
        dl->AddText(font, size, ImVec2(pos.x, pos.y + 2.f), shadow, text);
        dl->AddText(font, size, pos, col, text);
    }
}

// ─── Wizard state ────────────────────────────────────────────────────────────

static int  s_step = 1;
static bool s_eulaScrolledToBottom = false;
static bool s_eulaAccepted = false;

static char s_datPathBuf[512] = "";
static DatValidation s_datValidation = DatValidation::None;
static std::string s_autoDetectMsg;
static bool s_datDialogOpen = false;

static char s_matchFolderBuf[512] = "";
static FolderValidation s_matchFolderValidation = FolderValidation::None;
static bool s_matchFolderDialogOpen = false;

// Data source choice: 0 = cloud (default), 1 = local folder
static int  s_dataSourceChoice = 0;
// Cloud sub-choice: 0 = full_cache, 1 = online_only (default to online_only)
static int  s_cloudModeChoice = 1;
// Cloud host is read from GW_CLOUD_HOST env var (see GuiGlobalConstants::cloud_storage_host)

// ─── Background drawing ─────────────────────────────────────────────────────

static void DrawWizardBackground(ImDrawList* dl, ImVec2 display)
{
    static std::string bgPath = GetLoadingScreenPath();
    ImTextureID tex = bgPath.empty() ? nullptr : GetTextureCache().GetTexture(bgPath);
    if (tex)
        dl->AddImage(tex, ImVec2(0, 0), display);

    dl->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 140));
}

// ─── Step indicator ──────────────────────────────────────────────────────────

static void DrawStepIndicator(ImDrawList* dl, ImVec2 display, int currentStep)
{
    float cx = display.x * 0.5f;
    float cy = 36.f;
    float dotR = 6.f;
    float spacing = 120.f;

    struct StepDef { const char* label; };
    StepDef steps[] = { {"Licence"}, {"DAT File"} };

    for (int i = 0; i < 2; i++)
    {
        float x = cx + (i - 0.5f) * spacing;
        ImVec2 center(x, cy);
        int stepNum = i + 1;

        if (stepNum < currentStep)
        {
            // Completed: green filled with checkmark
            dl->AddCircleFilled(center, dotR, IM_COL32(64, 192, 96, 255));
            float cs = dotR * 0.5f;
            dl->AddLine(ImVec2(x - cs, cy), ImVec2(x - cs * 0.2f, cy + cs * 0.7f),
                        IM_COL32(255, 255, 255, 255), 2.f);
            dl->AddLine(ImVec2(x - cs * 0.2f, cy + cs * 0.7f), ImVec2(x + cs, cy - cs * 0.5f),
                        IM_COL32(255, 255, 255, 255), 2.f);
        }
        else if (stepNum == currentStep)
        {
            dl->AddCircleFilled(center, dotR, IM_COL32(212, 160, 32, 255));
        }
        else
        {
            dl->AddCircle(center, dotR, IM_COL32(112, 125, 136, 200), 0, 1.5f);
        }

        // Label below dot
        char label[64];
        snprintf(label, sizeof(label), "Step %d -- %s", stepNum, steps[i].label);
        ImVec2 labelSz = ImGui::CalcTextSize(label);
        ImU32 labelCol = (stepNum == currentStep)
            ? IM_COL32(212, 160, 32, 220)
            : IM_COL32(180, 180, 180, 160);
        dl->AddText(ImVec2(x - labelSz.x * 0.5f, cy + dotR + 6.f), labelCol, label);
    }

    // Connecting line
    float lineY = cy;
    float x1 = cx - 0.5f * spacing + dotR + 4.f;
    float x2 = cx + 0.5f * spacing - dotR - 4.f;
    dl->AddLine(ImVec2(x1, lineY), ImVec2(x2, lineY), IM_COL32(112, 125, 136, 100), 1.f);
}

// ─── Card styling helpers ────────────────────────────────────────────────────

static void PushCardStyle()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.078f, 0.102f, 0.88f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(40, 40));
}

static void PopCardStyle()
{
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ─── Step 1: EULA ────────────────────────────────────────────────────────────

static bool DrawStep1(ImVec2 display)
{
    float cardW = std::min(700.f, display.x - 80.f);
    float cardH = std::min(520.f, display.y - 140.f);
    ImVec2 cardPos((display.x - cardW) * 0.5f, 80.f);

    ImGui::SetNextWindowPos(cardPos);
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH));
    PushCardStyle();

    bool wantContinue = false;

    if (ImGui::Begin("##EulaCard", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
    {
        // Heading
        {
            const char* heading = "Welcome to GW Observer";
            ImFont* font = ImGui::GetFont();
            float headingSz = 24.f;
            ImVec2 hsz = font->CalcTextSizeA(headingSz, FLT_MAX, 0.f, heading);
            float hx = (cardW - hsz.x) * 0.5f;
            ImVec2 wpos = ImGui::GetWindowPos();
            ImVec2 cpos = ImGui::GetCursorPos();
            DrawShadowedText(ImGui::GetWindowDrawList(), font, headingSz,
                ImVec2(wpos.x + hx, wpos.y + cpos.y), IM_COL32(212, 160, 32, 255), heading);
            ImGui::Dummy(ImVec2(0, hsz.y + 4.f));
        }

        // Subheading
        {
            const char* sub = "Please read and accept the following before continuing.";
            ImVec2 ssz = ImGui::CalcTextSize(sub);
            ImGui::SetCursorPosX((cardW - ssz.x) * 0.5f);
            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.65f), "%s", sub);
        }

        ImGui::Dummy(ImVec2(0, 16.f));

        // Scrollable licence text
        float childH = std::min(280.f, cardH - 260.f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.3f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));

        if (ImGui::BeginChild("##EulaScroll", ImVec2(cardW - 80.f, childH), true))
        {
            DrawEulaContent();

            float scrollY = ImGui::GetScrollY();
            float scrollMax = ImGui::GetScrollMaxY();
            if (scrollMax > 0.f && scrollY >= scrollMax - 4.f)
                s_eulaScrolledToBottom = true;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 12.f));

        // Checkbox (only shown after scrolling)
        if (s_eulaScrolledToBottom)
        {
            ImGui::Checkbox("I have read and agree to the terms above", &s_eulaAccepted);
            ImGui::Dummy(ImVec2(0, 8.f));
        }

        // Button
        {
            bool canContinue = s_eulaScrolledToBottom && s_eulaAccepted;
            const char* btnLabel = canContinue ? "I Accept -- Continue" : "Scroll to read";
            float btnW = 220.f;
            ImGui::SetCursorPosX((cardW - btnW) * 0.5f);

            if (!canContinue)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.f));
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.15f, 0.9f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.18f, 0.22f, 0.9f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.25f, 0.30f, 0.9f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
            }
            ImGui::PushStyleColor(ImGuiCol_Border, canContinue
                ? ImVec4(0.83f, 0.63f, 0.13f, 0.8f) : ImVec4(0.3f, 0.3f, 0.3f, 0.4f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);

            if (ImGui::Button(btnLabel, ImVec2(btnW, 36.f)) && canContinue)
                wantContinue = true;

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(5);
        }
    }
    ImGui::End();
    PopCardStyle();

    return wantContinue;
}

// ─── Step 2: Info block drawing helpers ──────────────────────────────────────

static void DrawInfoBlockBg(ImDrawList* dl, ImVec2 minP, ImVec2 maxP,
                            ImU32 bgCol, ImU32 borderCol)
{
    dl->AddRectFilled(minP, maxP, bgCol, 4.f);
    dl->AddRectFilled(minP, ImVec2(minP.x + 2.f, maxP.y), borderCol, 2.f);
}

static void DrawGoldBullet(ImDrawList* dl, ImVec2 pos, float fontSize)
{
    float r = 2.5f;
    float cy = pos.y + fontSize * 0.5f;
    dl->AddCircleFilled(ImVec2(pos.x + r, cy), r, IM_COL32(212, 160, 32, 200));
}

static void DrawLockIcon(ImDrawList* dl, ImVec2 pos, float fontSize)
{
    float cx = pos.x + 5.f;
    float cy = pos.y + fontSize * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, cy), 5.f, IM_COL32(64, 192, 96, 180));
    dl->AddRectFilled(ImVec2(cx - 4.f, cy), ImVec2(cx + 4.f, cy + 6.f),
                      IM_COL32(64, 192, 96, 180), 1.f);
}

static void DrawWarningIcon(ImDrawList* dl, ImVec2 pos, float fontSize)
{
    float cx = pos.x + 6.f;
    float top = pos.y + 2.f;
    float bot = pos.y + fontSize - 2.f;
    float half = 6.f;
    dl->AddTriangleFilled(ImVec2(cx, top), ImVec2(cx - half, bot),
                          ImVec2(cx + half, bot), IM_COL32(224, 120, 48, 200));
    dl->AddText(ImVec2(cx - 1.5f, top + 3.f), IM_COL32(0, 0, 0, 220), "!");
}

// ─── Step 2: DAT file ────────────────────────────────────────────────────────

static bool DrawStep2(ImVec2 display)
{
    float cardW = std::min(700.f, display.x - 80.f);
    float cardH = std::min(display.y - 100.f, 720.f);
    ImVec2 cardPos((display.x - cardW) * 0.5f, 80.f);

    ImGui::SetNextWindowPos(cardPos);
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH));
    PushCardStyle();

    bool wantLaunch = false;
    float contentW = cardW - 80.f;

    if (ImGui::Begin("##DatCard", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
    {
        // Back button
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 0.50f));
            if (ImGui::SmallButton("<- Back"))
                s_step = 1;
            ImGui::PopStyleColor(4);
        }

        ImGui::Dummy(ImVec2(0, 4.f));

        // Heading
        {
            const char* heading = "Locate Your Guild Wars DAT File";
            ImFont* font = ImGui::GetFont();
            float headingSz = 24.f;
            ImVec2 hsz = font->CalcTextSizeA(headingSz, FLT_MAX, 0.f, heading);
            float hx = (cardW - hsz.x) * 0.5f;
            ImVec2 wpos = ImGui::GetWindowPos();
            ImVec2 cpos = ImGui::GetCursorPos();
            DrawShadowedText(ImGui::GetWindowDrawList(), font, headingSz,
                ImVec2(wpos.x + hx, wpos.y + cpos.y), IM_COL32(212, 160, 32, 255), heading);
            ImGui::Dummy(ImVec2(0, hsz.y + 8.f));
        }

        // Scrollable content area (info blocks + input fields)
        float launchBtnH = 36.f + 16.f + 12.f;
        float scrollH = cardH - ImGui::GetCursorPosY() - launchBtnH;
        if (scrollH < 100.f) scrollH = 100.f;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        if (ImGui::BeginChild("##DatInfoScroll", ImVec2(contentW, scrollH), false))
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec4 bodyCol(1.f, 1.f, 1.f, 0.75f);
            ImVec4 goldCol(0.83f, 0.63f, 0.13f, 1.f);
            float fontSize = ImGui::GetFontSize();
            float wrapW = ImGui::GetContentRegionAvail().x;

            // ── Block 1: What is the DAT file? ──────────────────────
            {
                ImGui::TextColored(goldCol, "WHAT IS THE DAT FILE?");
                ImGui::Dummy(ImVec2(0, 4.f));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapW);
                ImGui::TextColored(bodyCol,
                    "The Guild Wars DAT file (Gw.dat) is a single large archive "
                    "that contains all of the game's local assets:");
                ImGui::PopTextWrapPos();
                ImGui::Dummy(ImVec2(0, 6.f));

                struct { const char* text; } assets[] = {
                    {"Maps and terrain geometry"},
                    {"Textures and environment art"},
                    {"Character and NPC animations"},
                    {"Sound effects and music"},
                    {"Skill icons and UI elements"},
                };
                for (auto& a : assets)
                {
                    ImVec2 bpos = ImGui::GetCursorScreenPos();
                    DrawGoldBullet(dl, ImVec2(bpos.x + 12.f, bpos.y), fontSize);
                    ImGui::Indent(24.f);
                    ImGui::TextColored(bodyCol, "%s", a.text);
                    ImGui::Unindent(24.f);
                }
                ImGui::Dummy(ImVec2(0, 6.f));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapW);
                ImGui::TextColored(bodyCol,
                    "GW Observer uses it exclusively to render the 3D map and display "
                    "skill icons during match replays. No other data is read from it.");
                ImGui::PopTextWrapPos();
            }

            ImGui::Dummy(ImVec2(0, 16.f));

            // ── Block 2: Your account and data are safe (green tint) ─
            {
                float blockStartY = ImGui::GetCursorScreenPos().y;
                float blockX = ImGui::GetCursorScreenPos().x;

                ImGui::Indent(14.f);
                ImGui::Dummy(ImVec2(0, 6.f));

                ImGui::TextColored(goldCol, "YOUR ACCOUNT AND DATA ARE SAFE");
                ImGui::Dummy(ImVec2(0, 6.f));

                float itemWrap = wrapW - 40.f;
                struct { const char* text; } items[] = {
                    {"GW Observer does NOT read, store, or transmit your Guild Wars login credentials."},
                    {"Your account password and account data are not contained in the DAT file and cannot be accessed through it."},
                    {"The DAT file is opened in read-only mode. GW Observer never writes to or modifies it in any way."},
                    {"No data from your local machine is sent to any server. Your local files are never uploaded or shared."},
                };
                for (auto& item : items)
                {
                    ImVec2 ipos = ImGui::GetCursorScreenPos();
                    DrawLockIcon(dl, ImVec2(ipos.x, ipos.y), fontSize);
                    ImGui::Indent(16.f);
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + itemWrap);
                    ImGui::TextColored(bodyCol, "%s", item.text);
                    ImGui::PopTextWrapPos();
                    ImGui::Unindent(16.f);
                    ImGui::Dummy(ImVec2(0, 4.f));
                }

                ImGui::Dummy(ImVec2(0, 4.f));
                ImGui::Unindent(14.f);

                float blockEndY = ImGui::GetCursorScreenPos().y;
                DrawInfoBlockBg(dl,
                    ImVec2(blockX, blockStartY),
                    ImVec2(blockX + wrapW, blockEndY),
                    IM_COL32(64, 192, 96, 15),
                    IM_COL32(64, 192, 96, 102));
            }

            ImGui::Dummy(ImVec2(0, 16.f));

            // ── Block 3: Running alongside Guild Wars (amber tint) ───
            {
                float blockStartY = ImGui::GetCursorScreenPos().y;
                float blockX = ImGui::GetCursorScreenPos().x;

                ImGui::Indent(14.f);
                ImGui::Dummy(ImVec2(0, 6.f));

                ImGui::TextColored(goldCol, "RUNNING GW OBSERVER ALONGSIDE GUILD WARS");
                ImGui::Dummy(ImVec2(0, 6.f));

                float itemWrap = wrapW - 40.f;

                // Main warning line
                {
                    ImVec2 wpos = ImGui::GetCursorScreenPos();
                    DrawWarningIcon(dl, ImVec2(wpos.x, wpos.y), fontSize);
                    ImGui::Indent(16.f);
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + itemWrap);
                    ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.90f),
                        "GW Observer cannot share the DAT file with a running Guild Wars instance.");
                    ImGui::PopTextWrapPos();
                    ImGui::Unindent(16.f);
                }
                ImGui::Dummy(ImVec2(0, 6.f));

                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + itemWrap);
                ImGui::TextColored(bodyCol,
                    "Guild Wars locks the DAT file exclusively while it is running. "
                    "If you try to open GW Observer while Guild Wars is active, the "
                    "DAT file will be unavailable and map rendering will fail.");
                ImGui::Dummy(ImVec2(0, 6.f));
                ImGui::TextColored(bodyCol, "If you want to run both programs at the same time:");
                ImGui::PopTextWrapPos();
                ImGui::Dummy(ImVec2(0, 4.f));

                // Numbered steps
                ImGui::Indent(16.f);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + itemWrap - 16.f);

                ImGui::TextColored(goldCol, "1."); ImGui::SameLine();
                ImGui::TextColored(bodyCol, "Copy your entire Guild Wars installation folder to a new location.");
                ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 0.85f), "   Original: C:\\Program Files\\Guild Wars");
                ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 0.85f), "   Copy:     C:\\Games\\Guild Wars Copy");
                ImGui::Dummy(ImVec2(0, 4.f));

                ImGui::TextColored(goldCol, "2."); ImGui::SameLine();
                ImGui::TextColored(bodyCol, "In GW Observer, point the DAT file path to the copied folder:");
                ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 0.85f), "   C:\\Games\\Guild Wars Copy\\Gw.dat");
                ImGui::Dummy(ImVec2(0, 4.f));

                ImGui::TextColored(goldCol, "3."); ImGui::SameLine();
                ImGui::TextColored(bodyCol,
                    "Guild Wars uses its original folder. GW Observer uses the copy. "
                    "Both run independently with no conflict.");

                ImGui::PopTextWrapPos();
                ImGui::Unindent(16.f);
                ImGui::Dummy(ImVec2(0, 6.f));

                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + itemWrap);
                ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.55f),
                    "The copy only needs to be done once. The copied DAT file does not "
                    "need to be updated separately -- it is only used for rendering and "
                    "does not affect gameplay.");
                ImGui::PopTextWrapPos();

                ImGui::Dummy(ImVec2(0, 6.f));
                ImGui::Unindent(14.f);

                float blockEndY = ImGui::GetCursorScreenPos().y;
                DrawInfoBlockBg(dl,
                    ImVec2(blockX, blockStartY),
                    ImVec2(blockX + wrapW, blockEndY),
                    IM_COL32(224, 120, 48, 15),
                    IM_COL32(224, 120, 48, 128));
            }

            ImGui::Dummy(ImVec2(0, 16.f));

            // ── DAT file path input ──────────────────────────────────

            // Default hint
            {
                const char* hint = "Default location:  C:\\Program Files\\Guild Wars\\Gw.dat";
                ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 0.70f), "%s", hint);
            }

            ImGui::Dummy(ImVec2(0, 6.f));

            // Path input + Browse
            {
                float browseW = 80.f;
                float inputW = contentW - browseW - 12.f;

                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.4f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.15f));

                ImGui::SetNextItemWidth(inputW);
                if (ImGui::InputText("##datpath", s_datPathBuf, sizeof(s_datPathBuf)))
                {
                    s_datValidation = ValidateDatPath(s_datPathBuf);
                    s_autoDetectMsg.clear();
                }

                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);

                ImGui::SameLine(0.f, 8.f);

                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
                if (ImGui::Button("Browse", ImVec2(browseW, 0)))
                {
                    std::string initial = "C:\\";
                    if (s_datPathBuf[0] != '\0')
                    {
                        auto parent = std::filesystem::path(s_datPathBuf).parent_path();
                        if (std::filesystem::exists(parent))
                            initial = parent.string();
                    }
                    ImGuiFileDialog::Instance()->OpenDialog("SetupChooseGwDat", "Select Gw.dat",
                        ".dat", initial + "\\.");
                    s_datDialogOpen = true;
                }
                ImGui::PopStyleVar();
            }

            // Validation + auto-detect
            {
                switch (s_datValidation)
                {
                case DatValidation::Valid:
                    ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 1.f), "OK - DAT file found");
                    break;
                case DatValidation::NotFound:
                    ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f), "X  File not found at this path");
                    break;
                case DatValidation::WrongType:
                    ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f),
                        "X  This does not appear to be a Guild Wars DAT file");
                    break;
                case DatValidation::TooSmall:
                    ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f),
                        "X  File too small -- is this the correct file?");
                    break;
                default:
                    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
                    break;
                }

                ImGui::SameLine(contentW - 100.f);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.10f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.83f, 0.63f, 0.13f, 1.f));
                if (ImGui::SmallButton("[ Auto-detect ]"))
                {
                    std::string found = TryAutoDetect();
                    if (!found.empty())
                    {
                        size_t len = std::min(found.size(), sizeof(s_datPathBuf) - 1);
                        memcpy(s_datPathBuf, found.c_str(), len);
                        s_datPathBuf[len] = '\0';
                        s_datValidation = ValidateDatPath(s_datPathBuf);
                        s_autoDetectMsg.clear();
                    }
                    else
                    {
                        s_autoDetectMsg = "Could not auto-detect. Please browse manually.";
                    }
                }
                ImGui::PopStyleColor(4);

                if (!s_autoDetectMsg.empty())
                    ImGui::TextColored(ImVec4(0.88f, 0.65f, 0.20f, 1.f), "%s", s_autoDetectMsg.c_str());
            }

            ImGui::Dummy(ImVec2(0, 24.f));

            // ── Match Data Source section ────────────────────────────
            ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "MATCH DATA SOURCE");
            ImGui::Dummy(ImVec2(0, 4.f));

            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + contentW);
            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.75f),
                "How would you like to access match replay data?");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 6.f));

            // Radio: Cloud Storage
            ImGui::RadioButton("Cloud Storage (Recommended)##datasrc", &s_dataSourceChoice, 0);
            if (s_dataSourceChoice == 0)
            {
                ImGui::Indent(24.f);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + contentW - 24.f);
                ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.60f),
                    "Matches are downloaded from cloud storage and cached locally.");
                ImGui::PopTextWrapPos();
                ImGui::Dummy(ImVec2(0, 4.f));

                ImGui::RadioButton("Cloud + Local Cache -- download all matches##cmode", &s_cloudModeChoice, 0);
                ImGui::RadioButton("Cloud Only -- stream matches on demand##cmode", &s_cloudModeChoice, 1);

                ImGui::Unindent(24.f);
            }
            ImGui::Dummy(ImVec2(0, 6.f));

            // Radio: Local Folder
            ImGui::RadioButton("Local Folder##datasrc", &s_dataSourceChoice, 1);
            if (s_dataSourceChoice == 1)
            {
                ImGui::Indent(24.f);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + contentW - 24.f);
                ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.60f),
                    "Load matches from a folder on your computer.");
                ImGui::PopTextWrapPos();
                ImGui::Dummy(ImVec2(0, 4.f));

                {
                    const char* hint = "Example:  D:\\Guild Wars OBS\\Matches data\\Captures";
                    ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 0.70f), "%s", hint);
                }
                ImGui::Dummy(ImVec2(0, 4.f));

                // Folder input + Browse
                {
                    float browseW = 80.f;
                    float inputW = contentW - browseW - 36.f;

                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.4f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.15f));

                    ImGui::SetNextItemWidth(inputW);
                    if (ImGui::InputText("##matchfolder", s_matchFolderBuf, sizeof(s_matchFolderBuf)))
                        s_matchFolderValidation = ValidateMatchFolder(s_matchFolderBuf);

                    ImGui::PopStyleColor(2);
                    ImGui::PopStyleVar(2);

                    ImGui::SameLine(0.f, 8.f);

                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
                    if (ImGui::Button("Browse##folder", ImVec2(browseW, 0)))
                    {
                        std::string initial = "C:\\";
                        if (s_matchFolderBuf[0] != '\0')
                        {
                            std::filesystem::path p(s_matchFolderBuf);
                            if (std::filesystem::exists(p) && std::filesystem::is_directory(p))
                                initial = p.string();
                            else if (std::filesystem::exists(p.parent_path()))
                                initial = p.parent_path().string();
                        }
                        ImGuiFileDialog::Instance()->OpenDialog("SetupChooseMatchFolder",
                            "Select Match Data Folder", nullptr, initial + "\\.");
                        s_matchFolderDialogOpen = true;
                    }
                    ImGui::PopStyleVar();
                }

                // Folder validation feedback
                {
                    switch (s_matchFolderValidation)
                    {
                    case FolderValidation::Valid:
                        ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 1.f), "OK - Folder found");
                        break;
                    case FolderValidation::ValidEmpty:
                        ImGui::TextColored(ImVec4(0.88f, 0.47f, 0.19f, 1.f),
                            "No match files found in this folder. You can continue -- files can be added later.");
                        break;
                    case FolderValidation::NotFound:
                        ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f), "X  Folder not found at this path");
                        break;
                    case FolderValidation::NotFolder:
                        ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f), "X  This path is not a folder");
                        break;
                    case FolderValidation::NotReadable:
                        ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f),
                            "X  Cannot read this folder -- check permissions");
                        break;
                    default:
                        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
                        break;
                    }
                }
                ImGui::Unindent(24.f);
            }

            ImGui::Dummy(ImVec2(0, 12.f));
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0, 4.f));

        // Launch button
        {
            bool datOk = (s_datValidation == DatValidation::Valid);
            bool folderOk = false;
            if (s_dataSourceChoice == 0)
            {
                // Cloud mode: valid if S3 URL is provided
                folderOk = true; // Cloud URL is hardcoded
            }
            else
            {
                // Local mode: folder must be valid
                folderOk = (s_matchFolderValidation == FolderValidation::Valid ||
                            s_matchFolderValidation == FolderValidation::ValidEmpty);
            }
            bool canLaunch = datOk && folderOk;
            const char* btnLabel = "Launch GW Observer";
            float btnW = 220.f;
            ImGui::SetCursorPosX((cardW - btnW) * 0.5f);

            if (!canLaunch)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.f));
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.15f, 0.9f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.18f, 0.22f, 0.9f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.25f, 0.30f, 0.9f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
            }
            ImGui::PushStyleColor(ImGuiCol_Border, canLaunch
                ? ImVec4(0.83f, 0.63f, 0.13f, 0.8f) : ImVec4(0.3f, 0.3f, 0.3f, 0.4f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);

            if (ImGui::Button(btnLabel, ImVec2(btnW, 36.f)) && canLaunch)
                wantLaunch = true;

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(5);
        }
    }
    ImGui::End();
    PopCardStyle();

    // DAT file dialog
    if (s_datDialogOpen && ImGuiFileDialog::Instance()->Display("SetupChooseGwDat",
        ImGuiWindowFlags_NoCollapse, ImVec2(500, 400)))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string fp = ImGuiFileDialog::Instance()->GetFilePathName();
            size_t len = std::min(fp.size(), sizeof(s_datPathBuf) - 1);
            memcpy(s_datPathBuf, fp.c_str(), len);
            s_datPathBuf[len] = '\0';
            s_datValidation = ValidateDatPath(s_datPathBuf);
            s_autoDetectMsg.clear();
        }
        ImGuiFileDialog::Instance()->Close();
        s_datDialogOpen = false;
    }

    // Match folder dialog
    if (s_matchFolderDialogOpen && ImGuiFileDialog::Instance()->Display("SetupChooseMatchFolder",
        ImGuiWindowFlags_NoCollapse, ImVec2(500, 400)))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string fp = ImGuiFileDialog::Instance()->GetCurrentPath();
            size_t len = std::min(fp.size(), sizeof(s_matchFolderBuf) - 1);
            memcpy(s_matchFolderBuf, fp.c_str(), len);
            s_matchFolderBuf[len] = '\0';
            s_matchFolderValidation = ValidateMatchFolder(s_matchFolderBuf);
        }
        ImGuiFileDialog::Instance()->Close();
        s_matchFolderDialogOpen = false;
    }

    return wantLaunch;
}

// ─── Main wizard entry point ─────────────────────────────────────────────────

bool draw_setup_wizard()
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 display = io.DisplaySize;

    // Fullscreen background
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.039f, 0.055f, 0.071f, 1.f));

    ImGui::Begin("##SetupWizardBG", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    DrawWizardBackground(dl, display);
    DrawStepIndicator(dl, display, s_step);

    ImGui::End();
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(3);

    // Draw the active step card
    bool completed = false;

    if (s_step == 1)
    {
        if (DrawStep1(display))
            s_step = 2;
    }
    else if (s_step == 2)
    {
        if (DrawStep2(display))
        {
            // Save config
            std::string datPath(s_datPathBuf);
            SetupConfig::first_launch_complete = true;
            SetupConfig::accepted_eula = true;
            SetupConfig::accepted_eula_date = GetTodayString();
            SetupConfig::dat_file_path = datPath;

            if (s_dataSourceChoice == 0)
            {
                // Cloud storage mode
                SetupConfig::storage_mode = (s_cloudModeChoice == 0) ? "full_cache" : "online_only";
                // Auto-set match folder to cache directory
                std::string cacheDir = GuiGlobalConstants::GetMatchCacheDir();
                std::filesystem::create_directories(cacheDir);
                SetupConfig::match_data_folder = cacheDir;
            }
            else
            {
                // Local folder mode
                SetupConfig::storage_mode = "local";
                SetupConfig::match_data_folder = std::string(s_matchFolderBuf);
            }
            SetupConfig::Save();

            // Set globals so splash screen proceeds
            GuiGlobalConstants::saved_gw_dat_path = datPath;
            GuiGlobalConstants::saved_match_data_folder_path = SetupConfig::match_data_folder;
            GuiGlobalConstants::storage_mode = SetupConfig::storage_mode;
            GuiGlobalConstants::SaveSettings();

            std::wstring wpath(datPath.begin(), datPath.end());
            gw_dat_path = wpath;
            gw_dat_path_set = true;

            completed = true;
        }
    }

    return completed;
}

// ─── Licence modal (read-only, for Help menu) ───────────────────────────────

void draw_licence_modal(bool* open)
{
    if (!open || !*open) return;

    float cardW = 620.f;
    float cardH = 460.f;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2((display.x - cardW) * 0.5f, (display.y - cardH) * 0.5f),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH), ImGuiCond_Appearing);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.078f, 0.102f, 0.95f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));

    if (!ImGui::Begin("Licence & Credits", open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize))
    {
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(1);
        return;
    }

    // Heading
    ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "GW Observer");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.5f), " -  Licence & Credits");

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 8.f));

    // Scrollable licence text
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.25f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);

    if (ImGui::BeginChild("##LicenceText", ImVec2(0, cardH - 140.f), true))
    {
        DrawEulaContent();
    }
    ImGui::EndChild();

    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(1);

    ImGui::Dummy(ImVec2(0, 8.f));

    // EULA acceptance status
    if (SetupConfig::accepted_eula && !SetupConfig::accepted_eula_date.empty())
    {
        ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 0.8f),
            "Accepted on %s", SetupConfig::accepted_eula_date.c_str());
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(1);
}

// ─── File Paths modal (for Help menu) ────────────────────────────────────────

void draw_dat_settings_modal(bool* open, FolderWatcher* watcher)
{
    if (!open || !*open) return;

    using Clock = std::chrono::steady_clock;

    static char modalDatBuf[512] = "";
    static DatValidation modalDatVal = DatValidation::None;
    static std::string modalAutoMsg;
    static bool modalDatDialogOpen = false;

    static char modalFolderBuf[512] = "";
    static FolderValidation modalFolderVal = FolderValidation::None;
    static bool modalFolderDialogOpen = false;

    static bool modalInitialized = false;
    static Clock::time_point datSaveTime;
    static bool datSaved = false;
    static Clock::time_point folderSaveTime;
    static bool folderSaved = false;

    if (!modalInitialized)
    {
        modalInitialized = true;

        std::string datCur = SetupConfig::dat_file_path;
        if (datCur.empty()) datCur = GuiGlobalConstants::saved_gw_dat_path;
        size_t dlen = std::min(datCur.size(), sizeof(modalDatBuf) - 1);
        if (dlen > 0) memcpy(modalDatBuf, datCur.c_str(), dlen);
        modalDatBuf[dlen] = '\0';
        if (modalDatBuf[0] != '\0') modalDatVal = ValidateDatPath(modalDatBuf);

        std::string folCur = SetupConfig::match_data_folder;
        if (folCur.empty()) folCur = GuiGlobalConstants::saved_match_data_folder_path;
        size_t flen = std::min(folCur.size(), sizeof(modalFolderBuf) - 1);
        if (flen > 0) memcpy(modalFolderBuf, folCur.c_str(), flen);
        modalFolderBuf[flen] = '\0';
        if (modalFolderBuf[0] != '\0') modalFolderVal = ValidateMatchFolder(modalFolderBuf);

        datSaved = false;
        folderSaved = false;
    }

    float cardW = 580.f;
    float cardH = 480.f;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2((display.x - cardW) * 0.5f, (display.y - cardH) * 0.5f),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH), ImGuiCond_Appearing);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.078f, 0.102f, 0.95f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));

    if (!ImGui::Begin("File Paths", open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize))
    {
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(1);
        return;
    }

    float contentW = cardW - 48.f;
    float browseW = 80.f;
    float saveW = 70.f;
    float inputW = contentW - browseW - saveW - 20.f;

    // ── Section 1: Guild Wars DAT File ──────────────────────
    ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "GUILD WARS DAT FILE");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6.f));

    // Input + Browse + Save
    {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.4f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.15f));

        ImGui::SetNextItemWidth(inputW);
        if (ImGui::InputText("##modaldatpath", modalDatBuf, sizeof(modalDatBuf)))
        {
            modalDatVal = ValidateDatPath(modalDatBuf);
            modalAutoMsg.clear();
        }

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        ImGui::SameLine(0.f, 4.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        if (ImGui::Button("Browse##dat", ImVec2(browseW, 0)))
        {
            std::string initial = "C:\\";
            if (modalDatBuf[0] != '\0')
            {
                auto parent = std::filesystem::path(modalDatBuf).parent_path();
                if (std::filesystem::exists(parent))
                    initial = parent.string();
            }
            ImGuiFileDialog::Instance()->OpenDialog("ModalChooseGwDat", "Select Gw.dat",
                ".dat", initial + "\\.");
            modalDatDialogOpen = true;
        }
        ImGui::PopStyleVar();

        ImGui::SameLine(0.f, 4.f);
        {
            bool canSave = (modalDatVal == DatValidation::Valid);
            if (!canSave) ImGui::BeginDisabled();
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
            if (ImGui::Button("Save##dat", ImVec2(saveW, 0)) && canSave)
            {
                std::string p(modalDatBuf);
                SetupConfig::dat_file_path = p;
                SetupConfig::Save();
                GuiGlobalConstants::saved_gw_dat_path = p;
                GuiGlobalConstants::SaveSettings();
                std::wstring wp(p.begin(), p.end());
                gw_dat_path = wp;
                gw_dat_path_set = true;
                datSaved = true;
                datSaveTime = Clock::now();
            }
            ImGui::PopStyleVar();
            if (!canSave) ImGui::EndDisabled();
        }
    }

    // DAT validation + auto-detect + saved feedback
    {
        switch (modalDatVal)
        {
        case DatValidation::Valid:
            ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 1.f), "OK - DAT file found");
            break;
        case DatValidation::NotFound:
            ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f), "X  File not found at this path");
            break;
        case DatValidation::WrongType:
            ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f),
                "X  This does not appear to be a Guild Wars DAT file");
            break;
        case DatValidation::TooSmall:
            ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f),
                "X  File too small -- is this the correct file?");
            break;
        default:
            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
            break;
        }

        if (datSaved)
        {
            float elapsed = std::chrono::duration<float>(Clock::now() - datSaveTime).count();
            if (elapsed < 2.f)
            {
                float alpha = std::max(0.f, 1.f - (elapsed - 1.5f) / 0.5f);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, alpha), "Saved");
            }
            else
                datSaved = false;
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.83f, 0.63f, 0.13f, 1.f));
        if (ImGui::SmallButton("[ Auto-detect ]##modal"))
        {
            std::string found = TryAutoDetect();
            if (!found.empty())
            {
                size_t len = std::min(found.size(), sizeof(modalDatBuf) - 1);
                memcpy(modalDatBuf, found.c_str(), len);
                modalDatBuf[len] = '\0';
                modalDatVal = ValidateDatPath(modalDatBuf);
                modalAutoMsg.clear();
            }
            else
                modalAutoMsg = "Could not auto-detect. Please browse manually.";
        }
        ImGui::PopStyleColor(4);
        if (!modalAutoMsg.empty())
            ImGui::TextColored(ImVec4(0.88f, 0.65f, 0.20f, 1.f), "%s", modalAutoMsg.c_str());
    }

    ImGui::Dummy(ImVec2(0, 20.f));

    // ── Section 2: Match Data Folder ────────────────────────
    ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "MATCH DATA FOLDER");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6.f));

    // Input + Browse + Save
    {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.4f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.15f));

        ImGui::SetNextItemWidth(inputW);
        if (ImGui::InputText("##modalfolderpath", modalFolderBuf, sizeof(modalFolderBuf)))
            modalFolderVal = ValidateMatchFolder(modalFolderBuf);

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        ImGui::SameLine(0.f, 4.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        if (ImGui::Button("Browse##folder2", ImVec2(browseW, 0)))
        {
            std::string initial = "C:\\";
            if (modalFolderBuf[0] != '\0')
            {
                std::filesystem::path p(modalFolderBuf);
                if (std::filesystem::exists(p) && std::filesystem::is_directory(p))
                    initial = p.string();
            }
            ImGuiFileDialog::Instance()->OpenDialog("ModalChooseMatchFolder",
                "Select Match Data Folder", nullptr, initial + "\\.");
            modalFolderDialogOpen = true;
        }
        ImGui::PopStyleVar();

        ImGui::SameLine(0.f, 4.f);
        {
            bool canSave = (modalFolderVal == FolderValidation::Valid ||
                            modalFolderVal == FolderValidation::ValidEmpty);
            if (!canSave) ImGui::BeginDisabled();
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
            if (ImGui::Button("Save##folder", ImVec2(saveW, 0)) && canSave)
            {
                std::string p(modalFolderBuf);
                SetupConfig::match_data_folder = p;
                SetupConfig::Save();
                GuiGlobalConstants::saved_match_data_folder_path = p;
                GuiGlobalConstants::SaveSettings();
                if (watcher)
                    watcher->Restart(p);
                folderSaved = true;
                folderSaveTime = Clock::now();
            }
            ImGui::PopStyleVar();
            if (!canSave) ImGui::EndDisabled();
        }
    }

    // Folder validation + saved feedback
    {
        switch (modalFolderVal)
        {
        case FolderValidation::Valid:
            ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 1.f), "OK - Folder found");
            break;
        case FolderValidation::ValidEmpty:
            ImGui::TextColored(ImVec4(0.88f, 0.47f, 0.19f, 1.f),
                "No match files found. You can still save -- files can be added later.");
            break;
        case FolderValidation::NotFound:
            ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f), "X  Folder not found at this path");
            break;
        case FolderValidation::NotFolder:
            ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f), "X  This path is not a folder");
            break;
        case FolderValidation::NotReadable:
            ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f),
                "X  Cannot read this folder -- check permissions");
            break;
        default:
            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
            break;
        }

        if (folderSaved)
        {
            float elapsed = std::chrono::duration<float>(Clock::now() - folderSaveTime).count();
            if (elapsed < 2.f)
            {
                float alpha = std::max(0.f, 1.f - (elapsed - 1.5f) / 0.5f);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, alpha), "Saved");
            }
            else
                folderSaved = false;
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(1);

    // DAT file dialog
    if (modalDatDialogOpen && ImGuiFileDialog::Instance()->Display("ModalChooseGwDat",
        ImGuiWindowFlags_NoCollapse, ImVec2(500, 400)))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string fp = ImGuiFileDialog::Instance()->GetFilePathName();
            size_t len = std::min(fp.size(), sizeof(modalDatBuf) - 1);
            memcpy(modalDatBuf, fp.c_str(), len);
            modalDatBuf[len] = '\0';
            modalDatVal = ValidateDatPath(modalDatBuf);
            modalAutoMsg.clear();
        }
        ImGuiFileDialog::Instance()->Close();
        modalDatDialogOpen = false;
    }

    // Match folder dialog
    if (modalFolderDialogOpen && ImGuiFileDialog::Instance()->Display("ModalChooseMatchFolder",
        ImGuiWindowFlags_NoCollapse, ImVec2(500, 400)))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string fp = ImGuiFileDialog::Instance()->GetCurrentPath();
            size_t len = std::min(fp.size(), sizeof(modalFolderBuf) - 1);
            memcpy(modalFolderBuf, fp.c_str(), len);
            modalFolderBuf[len] = '\0';
            modalFolderVal = ValidateMatchFolder(modalFolderBuf);
        }
        ImGuiFileDialog::Instance()->Close();
        modalFolderDialogOpen = false;
    }

    if (!*open)
        modalInitialized = false;
}
