#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "TeamColors.h"
#include "SkillDatabase.h"
#include "RunLog.h"
#include "Highlights.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <json.hpp>
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// HIGHLIGHTS: the replay plays only the key moments of the match (owner, 2026-10-29).
//
// The moments - deaths, spikes, interrupt pressure - come from the private Highlights module, found
// once when the match loads. This file plays them: x0.75 through a moment and x10 between them (or a
// jump cut), easing between the two; a slow-motion beat on the decisive instant; the camera framed
// on the target and the people acting on it; the rest of the scene greyed out, the bystanders
// optionally hidden; a card over the replay saying what happened; markers on the play bar; and a
// GIF of any moment, or of all of them, through the Clip export. Spec:
// gwobserver-private docs/HIGHLIGHTS_PLAN.md.
// ---------------------------------------------------------------------------

using namespace DirectX;

namespace
{
    constexpr float kSpeedTau      = 0.15f;   // speed easing time constant: settled in about 0.5 s
    constexpr float kArriveWall    = 0.5f;    // a fast-forward slows so it reaches the next moment in this much real time
    constexpr float kSlowMoSpeed   = 0.25f;
    constexpr float kSlowMoHalf    = 0.5f;    // replay seconds either side of the decisive instant
    constexpr float kCameraLead    = 1.0f;    // the camera moves to the next target this long before its moment
    constexpr float kFilterFade    = 0.3f;    // the grey look fades in and out over this long (real seconds)
    constexpr float kCharHeight    = 120.f;   // world units, feet to the top of the head
    // The shot: wide on everyone at a moment's start, a little closer on the target by the decisive
    // instant. Measured from the owner's reference shot (2026-10-29): about 30 degrees above the
    // target at about 1600; a light zoom only.
    constexpr float kWideMinDist   = 1500.f;
    constexpr float kWideMaxDist   = 2200.f;
    constexpr float kCloseDist     = 1350.f;
    constexpr float kCloseKeep     = 0.85f;   // the close shot keeps at least this much of the wide distance
    constexpr float kWidePitch     = 0.56f;   // radians above the target: about 32 degrees
    constexpr float kClosePitch    = 0.50f;   // about 29 degrees
    constexpr float kSpotMin       = 300.f;   // the coloured zone's radius around the followed player
    constexpr float kSpotMax       = 900.f;
    constexpr float kJoinGrow      = 0.4f;    // replay seconds a joining person's colour takes to grow in
    // Split view (owner, 2026-10-29): a second moment playing at the same time, far enough from the
    // first that one shot cannot hold both, gets the right half of the screen.
    constexpr float kSplitMinDist  = 1000.f;  // world units between the two targets
    constexpr float kSplitSlide    = 0.35f;   // real seconds the right half takes to slide in or out
    // The clearest angle (owner, 2026-10-29: trees hid the action). Each half tries turning around its
    // target and lifting, and takes the angle whose sight lines the fewest props and hills block.
    constexpr float kClearProbeEvery = 0.25f;  // real seconds between two searches
    constexpr float kClearYaws[] = { 0.f, 20.f, -20.f, 40.f, -40.f, 60.f, -60.f, 90.f, -90.f, 135.f, -135.f, 180.f };   // degrees
    constexpr float kClearLifts[] = { 0.f, 0.3f };   // radians added to the pitch
    constexpr int   kClearOthers = 4;          // the people besides the target whose sight lines count
    constexpr int   kMaxEllipses   = 32;

    // 0 = no slow-down: the moment plays at the play bar's own speed.
    constexpr float kMomentSpeeds[]   = { 0.f, 0.75f, 0.5f, 0.25f };
    constexpr const char* kMomentSpeedLabels[] = { "Normal speed (no slow-down)", "0.75x", "0.5x", "0.25x" };
    constexpr float kGapSpeeds[]      = { 4.f, 8.f, 10.f, 16.f, 0.f };   // 0 = jump cut
    constexpr const char* kGapLabels[] = { "Fast-forward 4x", "Fast-forward 8x", "Fast-forward 10x", "Fast-forward 16x", "Jump cut" };
    constexpr float kBudgets[]        = { 0.f, 600.f, 300.f, 180.f };
    constexpr const char* kBudgetLabels[] = { "Every moment", "10 minutes", "5 minutes", "3 minutes" };
    constexpr const char* kOverheadLabels[] = { "Skill icons and numbers", "Skill icons only", "Nothing" };
    constexpr double kCleanHintSeconds = 4.0;  // clean screen: how long the "Esc" hint shows
    constexpr const char* kBystanderLabels[] = { "Show them in colour", "Grey them out", "Hide them" };

    // GW1 dark-glass gold, as the play bar.
    const ImU32 cGold       = IM_COL32(200, 168,  75, 255);
    const ImU32 cGoldBright = IM_COL32(226, 194, 106, 255);
    const ImU32 cText       = IM_COL32(232, 223, 200, 255);
    const ImU32 cTextMid    = IM_COL32(154, 138, 104, 255);
    const ImU32 cBorderHi   = IM_COL32(200, 168,  75,  82);
    const ImU32 cBorderGlow = IM_COL32(200, 168,  75, 140);

    ImU32 TeamColour(uint8_t team, int alpha)
    {
        return Team::IsBlue(team) ? IM_COL32(74, 200, 255, alpha) : Team::IsRed(team) ? IM_COL32(255, 107, 107, alpha)
                                                                  : IM_COL32(0xC8, 0xC8, 0xC8, alpha);
    }

    std::filesystem::path HighlightsSettingsPath()
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto dir = std::filesystem::path(exePath).parent_path() / "settings";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir / "highlights.json";
    }

    std::string NarrowName(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(std::max(0, n)), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    template <size_t N>
    int IndexOf(const float (&table)[N], float v, int fallback)
    {
        for (size_t i = 0; i < N; ++i)
            if (std::fabs(table[i] - v) < 1e-3f) return static_cast<int>(i);
        return fallback;
    }

    float WrapAngle(float a)
    {
        while (a > XM_PI) a -= XM_2PI;
        while (a < -XM_PI) a += XM_2PI;
        return a;
    }

    const char kHighlightsHLSL[] = R"(
Texture2D    sceneTex : register(t0);
SamplerState samLin   : register(s0);

cbuffer HighlightsCB : register(b0)
{
    float4 ellipses[32];   // centre x, y and radii x, y, in uv
    float  amount;         // 0 = untouched, 1 = fully greyed outside the mask
    float  count;
    float  darken;
    float  pad;
};

struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VS_OUT VSMain(uint id : SV_VertexID)
{
    VS_OUT o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
    o.uv  = uv;
    return o;
}

float4 PSMain(VS_OUT i) : SV_TARGET
{
    float3 c = sceneTex.Sample(samLin, i.uv).rgb;
    float m = 0;
    int n = (int)count;
    for (int k = 0; k < n; ++k)
    {
        float2 d = (i.uv - ellipses[k].xy) / max(ellipses[k].zw, 1e-4);
        m = max(m, 1.0 - smoothstep(0.7, 1.0, length(d)));
    }
    float grey = dot(c, float3(0.2126, 0.7152, 0.0722)) * darken;
    return float4(lerp(c, grey.xxx, amount * (1.0 - m)), 1.0);
}
)";

    // One moment's card: a dark-glass box, the team's edge, a gold title, a count, a line of figures.
    void DrawCardAt(ImDrawList* dl, float x, float y, const std::string& title, const std::string& detail,
                    const std::string& count, uint8_t team, float alpha)
    {
        ImFont* font = ImGui::GetFont();
        const float fsT = font->FontSize * 1.35f, fsD = font->FontSize;
        const ImVec2 tsz = font->CalcTextSizeA(fsT, FLT_MAX, 0.f, title.c_str());
        const ImVec2 dsz = font->CalcTextSizeA(fsD, FLT_MAX, 0.f, detail.c_str());
        const ImVec2 csz = font->CalcTextSizeA(fsD, FLT_MAX, 0.f, count.c_str());
        const float padX = 18.f, padY = 10.f;
        const float w = std::max(tsz.x + (count.empty() ? 0.f : csz.x + 24.f), dsz.x) + padX * 2.f;
        const float hgt = tsz.y + dsz.y + padY * 2.f + 4.f;
        const int a = static_cast<int>(alpha * 255.f);
        auto A = [&](ImU32 c) {
            return (c & 0x00FFFFFF) | (static_cast<ImU32>(std::clamp(((c >> 24) & 0xFF) * alpha, 0.f, 255.f)) << 24);
        };
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + hgt), IM_COL32(10, 9, 7, static_cast<int>(215 * alpha)), 8.f);
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + hgt), A(cBorderGlow), 8.f);
        if (team != Team::None)
            dl->AddRectFilled(ImVec2(x, y + 6.f), ImVec2(x + 4.f, y + hgt - 6.f), TeamColour(team, a), 2.f);
        dl->AddText(font, fsT, ImVec2(x + padX, y + padY), A(cGoldBright), title.c_str());
        if (!count.empty())
            dl->AddText(font, fsD, ImVec2(x + w - padX - csz.x, y + padY + (tsz.y - csz.y) * 0.5f), A(cTextMid), count.c_str());
        dl->AddText(font, fsD, ImVec2(x + padX, y + padY + tsz.y + 4.f), A(cText), detail.c_str());
    }

    struct HighlightsCBData
    {
        XMFLOAT4 ellipses[kMaxEllipses];
        float amount, count, darken, pad;
    };
}

struct ReplayWindow::HighlightsState
{
    // ---- the match's moments ----
    bool built = false;
    int  buildTries = 0;
    std::vector<Highlights::Moment> moments;
    std::vector<Highlights::Card>   cards;
    std::vector<uint8_t>            teams;      // the target's team, per moment
    std::vector<int>                shown;      // kept by the filters and the budget, in time order
    std::vector<std::pair<float, float>> spans; // the shown moments' ranges, overlaps joined
    bool dirty = true;

    // ---- settings (settings/highlights.json) ----
    bool  settingsLoaded = false;
    float momentSpeed = 0.75f;
    float gapSpeed = 10.f;          // 0 = jump cut
    bool  slowMo = true;
    bool  turnCamera = true;       // turn the camera across the attack (off: keep the heading)
    bool  splitView = true;        // a second moment at the same time gets the right half
    int   overhead = 0;            // over the players: 0 skill icons and numbers, 1 skill icons only, 2 nothing
    bool  cleanScreen = false;     // hide the interface while watching; Escape brings it back
    double cleanSince = -1.0;      // ImGui time the interface went away, for the "Esc" hint
    bool  mouseBlocked = false;    // the clean screen took the mouse from ImGui's hidden panels
    float filterStrength = 1.f;
    int   bystanders = 1;           // 0 in colour, 1 greyed out, 2 hidden
    bool  showDeaths = true, showNpcDeaths = true, showSpikes = true, showInterrupts = true, showFlags = true;
    int   budgetIdx = 0;

    // ---- playback ----
    bool  active = false;
    bool  panelOpen = true;
    bool  ended = false;
    float speed = 1.f;
    bool  savedAutoCam = false;

    // ---- camera ----
    int   current = -1;             // the moment the camera frames
    bool  userYaw = false, userDist = false, followDropped = false;
    float lastDist = -1.f;
    float frameYaw = 0.f, frameDist = 0.f;
    bool  frameValid = false;
    XMFLOAT3 bias{0.f, 0.f, 0.f};
    float filterAmt = 0.f;          // 0..1, eased
    std::unordered_set<int> involved;
    // The clearest angle, per half (0 the main view, 1 the right half): the turn and lift on top of
    // the shot's own, and the time to the next search.
    float clearYaw[2] = { 0.f, 0.f }, clearLift[2] = { 0.f, 0.f }, clearTimer[2] = { 0.f, 0.f };

    // ---- split view: the second moment, its own eased camera and its own target ----
    int   second = -1;
    float splitAmt = 0.f;           // 0 = one view, 1 = the right half is the second moment's
    bool  secValid = false;         // the second camera has been placed at least once
    float secYaw = 0.f, secDist = 0.f, secPitch = 0.f;
    XMFLOAT3 secBias{0.f, 0.f, 0.f};
    XMFLOAT3 secLook{0.f, 0.f, 0.f};
    int   secShown = -1;            // the moment the right half shows, kept while it slides out
    XMFLOAT4X4 secViewProj{};       // its camera, for what is drawn over its picture
    std::vector<IncomingEffect> secFloaters;   // the numbers over its target
    int   secFloaterAgent = -1;
    float secFloaterScan = -1.f;
    bool  secFloaterToggle = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          secTex, secDepth;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   secRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>   secDSV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> secSRV;
    ID3D11Device* secDevice = nullptr;
    int secW = 0, secH = 0;

    // ---- grey pass ----
    ID3D11Device* device = nullptr;
    bool shadersTried = false;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>       vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>        ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer>             cb;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>       sampler;
    Microsoft::WRL::ComPtr<ID3D11BlendState>         bs;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState>  dss;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>    rs;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          sceneTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneSRV;
    int texW = 0, texH = 0;
    XMFLOAT3 spotCenter{0.f, 0.f, 0.f};   // the mask in the world, from Update; projected in Render
    float    spotRadius = 0.f;
    bool     spotValid = false;
    std::vector<XMFLOAT4> maskFeet;   // feet position, and the ellipse scale (0..1, growing in)

    // ---- GIF of every moment ----
    enum class Batch { Off, WaitFirst, Running };
    Batch batch = Batch::Off;
    std::vector<int> batchQueue;
    size_t batchPos = 0;
    std::filesystem::path batchFirst;
    bool batchSawRender = false;
    int  batchDone = 0;
};

void ReplayWindow::HighlightsStateDeleter::operator()(HighlightsState* p) const { delete p; }

ReplayWindow::HighlightsState& ReplayWindow::HL()
{
    if (!m_hl) m_hl.reset(new HighlightsState());
    return *m_hl;
}

bool ReplayWindow::HighlightsActive() const { return m_hl && m_hl->active; }

// What shows over the players while Highlights plays: 0 everything, 1 skill icons only, 2 nothing.
int ReplayWindow::HighlightsOverheadMode() const
{
    if (!m_hl || (!m_hl->active && m_hl->batch != HighlightsState::Batch::Running)) return 0;
    return m_hl->overhead;
}

// Clean screen: the interface is gone while Highlights plays, until Escape (owner, 2026-10-29: a
// mouse move or a right-drag must not bring it back). Never while the clip editor is open: its box
// and panel must show.
bool ReplayWindow::HighlightsHidesInterface() const
{
    if (!m_hl || !m_hl->cleanScreen) return false;
    if (!m_hl->active && m_hl->batch != HighlightsState::Batch::Running) return false;
    return m_clip.phase == ClipExportState::Phase::Closed || m_clip.phase == ClipExportState::Phase::Rendering;
}

// Escape: the interface comes back. True when the key was used for that.
bool ReplayWindow::LeaveHighlightsCleanScreen()
{
    if (!HighlightsHidesInterface()) return false;
    m_hl->cleanScreen = false;
    SaveHighlightsSettings();
    return true;
}

// Before ImGui::NewFrame: the hidden panels must not take clicks or the camera's drags, so ImGui
// gets no mouse while the interface is gone.
void ReplayWindow::ApplyHighlightsMouseBlock()
{
    ImGuiIO& io = ImGui::GetIO();
    const bool block = HighlightsHidesInterface();
    if (block && !m_hl->mouseBlocked)
    {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        m_hl->mouseBlocked = true;
        m_hl->cleanSince = ImGui::GetTime();
    }
    else if (!block && m_hl && m_hl->mouseBlocked)
    {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        m_hl->mouseBlocked = false;
    }
}

// After ImGui::Render: with the interface hidden, only the background list is drawn - what floats
// over the world (names, skill icons, numbers), the right half and the moment cards. The foreground
// list goes too: the followed player's effect icons are drawn there.
void ReplayWindow::FilterHighlightsDrawData(ImDrawData* dd)
{
    if (!dd || !HighlightsHidesInterface()) return;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImVector<ImDrawList*> keep;
    dd->TotalVtxCount = dd->TotalIdxCount = 0;
    for (ImDrawList* l : dd->CmdLists)
        if (l == bg)
        {
            keep.push_back(l);
            dd->TotalVtxCount += l->VtxBuffer.Size;
            dd->TotalIdxCount += l->IdxBuffer.Size;
        }
    dd->CmdLists.swap(keep);
    dd->CmdListsCount = dd->CmdLists.Size;
}

// The speed a moment plays at: the chosen slow-down, or with none the play bar's own speed.
float ReplayWindow::HighlightsMomentSpeed() const
{
    const float user = kReplaySpeeds[m_replayCtx.speedIndex];
    return (m_hl && m_hl->momentSpeed > 0.f) ? m_hl->momentSpeed : user;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void ReplayWindow::LoadHighlightsSettings()
{
    auto& h = HL();
    h.settingsLoaded = true;
    std::ifstream f(HighlightsSettingsPath());
    if (!f.is_open()) return;
    try
    {
        nlohmann::json j = nlohmann::json::parse(f);
        // Version 2 (2026-10-29): the default moment speed went from 0.5x to 0.75x; an older file's
        // 0.5x was the old default, not a choice.
        if (j.value("version", 1) >= 2)
            h.momentSpeed = j.value("moment_speed", h.momentSpeed);
        h.gapSpeed       = j.value("gap_speed", h.gapSpeed);
        h.slowMo         = j.value("slow_motion", h.slowMo);
        h.turnCamera     = j.value("turn_camera", h.turnCamera);
        h.splitView      = j.value("split_view", h.splitView);
        h.overhead       = std::clamp(j.value("overhead", h.overhead), 0, 2);
        h.cleanScreen    = j.value("clean_screen", h.cleanScreen);
        h.filterStrength = std::clamp(j.value("filter_strength", h.filterStrength), 0.f, 1.f);
        h.bystanders     = std::clamp(j.value("bystanders", h.bystanders), 0, 2);
        h.showDeaths     = j.value("show_deaths", h.showDeaths);
        h.showNpcDeaths  = j.value("show_npc_deaths", h.showNpcDeaths);
        h.showSpikes     = j.value("show_spikes", h.showSpikes);
        h.showInterrupts = j.value("show_interrupts", h.showInterrupts);
        h.showFlags      = j.value("show_flag_returns", h.showFlags);
        h.budgetIdx      = std::clamp(j.value("budget", h.budgetIdx), 0, static_cast<int>(std::size(kBudgets)) - 1);
        h.panelOpen      = j.value("panel_open", h.panelOpen);
        if (std::fabs(h.momentSpeed - 1.f) < 1e-3f) h.momentSpeed = 0.f;   // the old "1x" is no slow-down
        h.momentSpeed    = kMomentSpeeds[IndexOf(kMomentSpeeds, h.momentSpeed, 1)];
        h.gapSpeed       = kGapSpeeds[IndexOf(kGapSpeeds, h.gapSpeed, 2)];
    }
    catch (...) {}
}

void ReplayWindow::SaveHighlightsSettings() const
{
    if (!m_hl) return;
    const auto& h = *m_hl;
    nlohmann::json j;
    j["version"]         = 2;
    j["moment_speed"]    = h.momentSpeed;
    j["gap_speed"]       = h.gapSpeed;
    j["slow_motion"]     = h.slowMo;
    j["turn_camera"]     = h.turnCamera;
    j["split_view"]      = h.splitView;
    j["overhead"]        = h.overhead;
    j["clean_screen"]    = h.cleanScreen;
    j["filter_strength"] = h.filterStrength;
    j["bystanders"]      = h.bystanders;
    j["show_deaths"]     = h.showDeaths;
    j["show_npc_deaths"] = h.showNpcDeaths;
    j["show_spikes"]     = h.showSpikes;
    j["show_interrupts"] = h.showInterrupts;
    j["show_flag_returns"] = h.showFlags;
    j["budget"]          = h.budgetIdx;
    j["panel_open"]      = h.panelOpen;
    std::ofstream f(HighlightsSettingsPath());
    if (f.is_open()) f << j.dump(2);
}

// ---------------------------------------------------------------------------
// Finding the moments: once per match, as soon as the casts, the spikes and the interrupt
// attempts are in. The interrupt attempts come from the Battle Ledger, which waits for the damage
// attribution; if that never comes, the moments are found without them.
// ---------------------------------------------------------------------------

void ReplayWindow::BuildHighlights()
{
    auto& h = HL();
    if (h.built) return;
    if ((m_loadingPhase != LoadingPhase::Ready && !m_headless) || !m_agentsClassified || !m_skillUseTimelineBuilt) return;
    if (!m_skillView.IsLoaded()) return;
    if (!m_spikesBuilt) BuildSpikes();
    if (!m_spikesBuilt) return;
    if (!m_ledgerBuilt && h.buildTries < 600) BuildBattleLedger();
    if ((!m_ledgerBuilt || !m_flagTimelineBuilt) && ++h.buildTries < 600) return;
    h.built = true;

    Highlights::Inputs in;
    in.matchEnd = m_replayCtx.maxReplayTime;

    auto keyNpc = [](const AgentReplayData& a, Highlights::Who& who) {
        if (a.type != AgentType::NPC) return false;
        switch (a.modelId)
        {
        case 170: who = Highlights::Who::GuildLord; return true;
        case 172: who = Highlights::Who::Bodyguard; return true;
        case 174: who = Highlights::Who::Knight;    return true;
        default:  return false;
        }
    };

    // Spikes: the Piano Roll's own, so both views agree.
    auto toCast = [](const CastRef& r, int dst, bool enemy, bool answer) {
        Highlights::Cast c;
        c.src = r.casterId; c.dst = dst;
        c.start = r.ev->startTime; c.end = r.ev->endTime;
        c.skillId = r.ev->skillId; c.enemy = enemy; c.answer = answer;
        return c;
    };
    for (const SpikeInfo& s : m_spikes)
    {
        Highlights::SpikeIn si;
        si.target = s.target; si.cue = s.cue; si.firstHit = s.firstHit; si.lastHit = s.lastHit; si.lowHp = s.lowHp;
        for (const auto& r : s.hits)    si.hits.push_back(toCast(r, s.target, true, false));
        for (const auto& r : s.answers) si.answers.push_back(toCast(r, s.target, false, true));
        in.spikes.push_back(std::move(si));
    }

    // Deaths: every alive-to-dead step of a player or a key guild NPC.
    std::unordered_set<int> keyTargets;
    for (const auto& [id, a] : m_replayCtx.agents)
    {
        Highlights::Who who = Highlights::Who::Player;
        if (a.type != AgentType::Player && !keyNpc(a, who)) continue;
        keyTargets.insert(id);
        bool wasDead = true;   // a corpse at the first snapshot is not a death seen
        for (const auto& snap : a.snapshots)
        {
            if (snap.is_dead && !wasDead) in.deaths.push_back({ id, snap.time, who });
            wasDead = snap.is_dead;
        }
    }

    // Flags sent home by the other team: who touched each, and when it had fallen.
    if (m_flagTimelineBuilt)
        for (const auto& team : m_flagTimeline.teams)
        {
            float dropped = -1.f;
            int carrier = -1;
            for (const auto& ev : team.events)
            {
                if (ev.eventType == FlagTimelineEventType::Drop) { dropped = ev.time; carrier = ev.actorAgentId; }
                else if (ev.eventType == FlagTimelineEventType::Return && ev.actorAgentId >= 0)
                {
                    in.flagReturns.push_back({ ev.actorAgentId, carrier, ev.time, dropped });
                    dropped = -1.f;
                    carrier = -1;
                }
                else if (ev.eventType == FlagTimelineEventType::Pickup) { dropped = -1.f; carrier = -1; }
            }
        }

    // Interrupt attempts, with what came of each.
    for (const auto& li : m_ledgerInts)
    {
        Highlights::InterruptIn ii;
        ii.src = li.src; ii.dst = li.dst; ii.t = li.t; ii.skillId = li.skillId;
        ii.outcome = static_cast<uint8_t>(li.outcome);
        in.interrupts.push_back(ii);
    }

    // Every enemy skill and allied answer on a player or a key NPC: who took part in a death.
    for (const auto& [casterId, caster] : m_replayCtx.agents)
        for (const auto& ev : caster.skillUseHistory)
        {
            if (ev.targetId < 0 || ev.targetId == casterId || ev.wasCancelled || ev.wasInterrupted) continue;
            if (!keyTargets.count(ev.targetId)) continue;
            const auto& tgt = m_replayCtx.agents.at(ev.targetId);
            const bool enemy = caster.teamId != tgt.teamId;
            const bool answer = !enemy && IsSpikeAnswerSkill(ev.skillId);
            if (!enemy && !answer) continue;
            Highlights::Cast c;
            c.src = casterId; c.dst = ev.targetId; c.start = ev.startTime; c.end = ev.endTime;
            c.skillId = ev.skillId; c.enemy = enemy; c.answer = answer;
            in.casts.push_back(c);
        }
    // ...and every enemy damage packet on them, so a death by weapon attacks still has its killers.
    for (const CombatEvent& ce : m_replayCtx.stocData.combat)
    {
        if (ce.type != "DAMAGE" || !keyTargets.count(ce.target_id) || ce.caster_id == ce.target_id) continue;
        auto src = m_replayCtx.agents.find(ce.caster_id);
        if (src == m_replayCtx.agents.end() || src->second.teamId == m_replayCtx.agents.at(ce.target_id).teamId) continue;
        Highlights::Cast c;
        c.src = ce.caster_id; c.dst = ce.target_id; c.start = c.end = ce.time; c.enemy = true;
        in.casts.push_back(c);
    }

    h.moments = Highlights::Detect(in);

    Highlights::Names names;
    names.agent = [this](int id) -> std::string {
        auto it = m_replayCtx.agents.find(id);
        if (it == m_replayCtx.agents.end()) return "?";
        const auto& a = it->second;
        if (a.type == AgentType::Player && !a.playerName.empty()) return a.playerName;
        if (const char* n = LookupNpcName(a.modelId))
            return Team::IsSide(a.teamId) ? std::format("{} {}", Team::Name(a.teamId), n) : std::string(n);
        return a.cachedLabel.empty() ? std::string("?") : a.cachedLabel;
    };
    names.skill = [this](int id) -> std::string {
        const SkillInfo* si = m_skillView.Get(id);
        return si ? si->name : std::string();
    };
    h.cards.clear();
    h.teams.clear();
    for (const auto& m : h.moments)
    {
        h.cards.push_back(Highlights::MakeCard(m, names));
        auto it = m_replayCtx.agents.find(m.target);
        h.teams.push_back(it != m_replayCtx.agents.end() ? it->second.teamId : Team::None);
    }
    h.dirty = true;
    RunLog::Line("highlights: %d moments (%d spikes, %d deaths, %d interrupt attempts in)",
                 static_cast<int>(h.moments.size()), static_cast<int>(in.spikes.size()),
                 static_cast<int>(in.deaths.size()), static_cast<int>(in.interrupts.size()));
}

// Developer dump (GWO_HIGHLIGHTS_DEBUG): every moment found, its card, score and people.
void ReplayWindow::WriteHighlightsDebug()
{
    char flag[16] = {};
    if (GetEnvironmentVariableA("GWO_HIGHLIGHTS_DEBUG", flag, (DWORD)sizeof(flag)) == 0) return;
    char tempDir[MAX_PATH] = {};
    if (GetTempPathA(MAX_PATH, tempDir) == 0) return;
    std::string folder = m_replayCtx.matchFolderPath.filename().string();
    if (folder.empty()) folder = "replay";
    for (char& c : folder)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') c = '_';

    auto& h = HL();
    // Written right after the attribute solve, before the pass that builds the flag timeline.
    if (!m_flagTimelineBuilt && m_agentsClassified && m_replayCtx.stocLoaded) BuildFlagTimeline();
    for (int i = 0; i < 601 && !h.built; ++i) BuildHighlights();
    std::ofstream f(std::string(tempDir) + "gwo_highlights_" + folder + ".txt");
    if (!f.is_open()) return;
    f << "# " << h.moments.size() << " moments, ledger " << (m_ledgerBuilt ? "built" : "missing")
      << ", " << m_ledgerInts.size() << " interrupt attempts\n";
    for (size_t i = 0; i < h.moments.size(); ++i)
    {
        const auto& m = h.moments[i];
        char t0[16], t1[16], tk[16];
        FormatTime(m.start - m_displayTimeOffset, t0, sizeof(t0));
        FormatTime(m.end - m_displayTimeOffset, t1, sizeof(t1));
        FormatTime(m.key - m_displayTimeOffset, tk, sizeof(tk));
        f << std::format("{:3} {}-{} key {} score {:5.1f} team {} [{}{}{}{}] {} | {}\n", i, t0, t1, tk, m.score,
                         h.teams[i], (m.flags & Highlights::kDeath) ? "D" : "-", (m.flags & Highlights::kSpike) ? "S" : "-",
                         (m.flags & Highlights::kInterrupts) ? "I" : "-", (m.flags & Highlights::kFlagReturn) ? "F" : "-", h.cards[i].title, h.cards[i].detail);
        f << "      people:";
        for (const auto& p : m.people)
        {
            auto it = m_replayCtx.agents.find(p.agent);
            const std::string name = it == m_replayCtx.agents.end() ? std::string("?")
                                   : !it->second.playerName.empty() ? it->second.playerName : it->second.cachedLabel;
            f << std::format(" {}{}", p.role == Highlights::Role::Target ? "*" : p.role == Highlights::Role::Enemy ? "-" : "+", name);
        }
        f << "\n";
    }
}

// The moments the filters and the budget keep, and the stretches of the match they cover.
void ReplayWindow::RefreshHighlightsSelection()
{
    auto& h = HL();
    if (!h.dirty) return;
    h.dirty = false;
    std::vector<int> pass;
    for (int i = 0; i < static_cast<int>(h.moments.size()); ++i)
    {
        const auto& m = h.moments[i];
        const bool death = (m.flags & Highlights::kDeath) != 0;
        const bool npc = m.who != Highlights::Who::Player;
        const bool ok = (death && (npc ? h.showNpcDeaths : h.showDeaths)) ||
                        ((m.flags & Highlights::kSpike) && h.showSpikes) ||
                        ((m.flags & Highlights::kInterrupts) && h.showInterrupts) ||
                        ((m.flags & Highlights::kFlagReturn) && h.showFlags);
        if (ok) pass.push_back(i);
    }
    h.shown = Highlights::FitToBudget(h.moments, pass, HighlightsMomentSpeed(), kBudgets[h.budgetIdx]);
    h.spans.clear();
    for (int i : h.shown)
    {
        const auto& m = h.moments[i];
        if (!h.spans.empty() && m.start <= h.spans.back().second)
            h.spans.back().second = std::max(h.spans.back().second, m.end);
        else
            h.spans.push_back({ m.start, m.end });
    }
    if (h.current >= 0 && std::find(h.shown.begin(), h.shown.end(), h.current) == h.shown.end())
        h.current = -1;
}

// ---------------------------------------------------------------------------
// On / off, and stepping between moments
// ---------------------------------------------------------------------------

void ReplayWindow::ToggleHighlights()
{
    auto& h = HL();
    if (!h.settingsLoaded) LoadHighlightsSettings();
    if (!h.active)
    {
        BuildHighlights();
        h.active = true;
        h.ended = false;
        h.panelOpen = true;
        h.speed = m_replayCtx.playbackSpeed;
        h.current = -1;
        h.savedAutoCam = m_autoCameraEnabled;
        m_autoCameraEnabled = false;
        RefreshHighlightsSelection();
        // Start at the next moment rather than wherever the playhead was.
        const float t = m_debugTimeline;
        bool inside = false;
        for (const auto& s : h.spans) if (t >= s.first && t < s.second) inside = true;
        if (!inside)
            for (const auto& s : h.spans)
                if (s.first > t) { m_debugTimeline = s.first; break; }
        h.speed = HighlightsMomentSpeed();
        m_replayCtx.isPlaying = true;
    }
    else
    {
        h.active = false;
        h.current = -1;
        h.filterAmt = 0.f;
        h.involved.clear();
        h.maskFeet.clear();
        h.spotValid = false;
        h.second = -1;
        h.secShown = -1;
        h.splitAmt = 0.f;
        h.secValid = false;
        m_followCenterBias = XMFLOAT3(0.f, 0.f, 0.f);
        m_replayCtx.playbackSpeed = kReplaySpeeds[m_replayCtx.speedIndex];
        m_autoCameraEnabled = h.savedAutoCam;
    }
    SaveHighlightsSettings();
}

void ReplayWindow::HighlightsStep(int dir)
{
    if (!HighlightsActive()) return;
    auto& h = *m_hl;
    RefreshHighlightsSelection();
    if (h.shown.empty()) return;
    const float t = m_debugTimeline;
    int pick = -1;
    if (dir > 0)
    {
        for (int i : h.shown) if (h.moments[i].start > t + 0.25f) { pick = i; break; }
    }
    else
    {
        // Back to the start of the moment playing, unless it has only just begun.
        for (auto it = h.shown.rbegin(); it != h.shown.rend(); ++it)
            if (h.moments[*it].start < t - 1.5f) { pick = *it; break; }
    }
    if (pick < 0) return;
    m_debugTimeline = h.moments[pick].start;
    h.current = h.second = -1;
    h.speed = HighlightsMomentSpeed();
    h.ended = false;
    m_replayCtx.isPlaying = true;
}

void ReplayWindow::JumpToHighlight(int index)
{
    auto& h = HL();
    if (index < 0 || index >= static_cast<int>(h.moments.size())) return;
    m_debugTimeline = h.moments[index].start;
    h.current = h.second = -1;
    h.speed = HighlightsMomentSpeed();
    h.ended = false;
    if (h.active) m_replayCtx.isPlaying = true;
}

// ---------------------------------------------------------------------------
// The clock: called by the playback engine in place of the plain speed. Inside a moment, the
// moment speed (and the slow-motion beat); between moments, the fast-forward that slows down to
// arrive, or a jump cut. Past the last moment the replay stops.
// ---------------------------------------------------------------------------

float ReplayWindow::HighlightsPlaybackSpeed(float dt)
{
    if (!HighlightsActive() || IsClipRendering()) return m_replayCtx.playbackSpeed;
    auto& h = *m_hl;
    RefreshHighlightsSelection();
    const float t = m_debugTimeline;

    // Nothing to play: the user's own speed.
    if (h.spans.empty())
    {
        m_replayCtx.playbackSpeed = kReplaySpeeds[m_replayCtx.speedIndex];
        return m_replayCtx.playbackSpeed;
    }

    int inSpan = -1, next = -1;
    for (int i = 0; i < static_cast<int>(h.spans.size()); ++i)
    {
        if (t >= h.spans[i].first && t < h.spans[i].second) { inSpan = i; break; }
        if (h.spans[i].first > t) { next = i; break; }
    }
    if (inSpan >= 0 || next >= 0) h.ended = false;

    float target = HighlightsMomentSpeed();
    if (inSpan < 0 && next < 0 && h.ended)
    {
        // Played on past the end of the highlights: the user's own speed.
        target = kReplaySpeeds[m_replayCtx.speedIndex];
    }
    else if (inSpan >= 0)
    {
        if (h.slowMo)
            for (int i : h.shown)
            {
                const auto& m = h.moments[i];
                if (t >= m.start && t < m.end && std::fabs(t - m.key) < kSlowMoHalf)
                    target = std::min(target, kSlowMoSpeed);
            }
    }
    else if (next < 0)
    {
        // Past the last moment: stop here, once.
        m_replayCtx.isPlaying = false;
        h.ended = true;
        target = HighlightsMomentSpeed();
    }
    else if (h.gapSpeed <= 0.f)
    {
        m_debugTimeline = h.spans[next].first;
        h.speed = HighlightsMomentSpeed();
        m_replayCtx.playbackSpeed = h.speed;
        return h.speed;
    }
    else
        target = h.gapSpeed;

    const float a = 1.f - std::exp(-std::max(0.f, dt) / kSpeedTau);
    h.speed += (target - h.speed) * a;
    // Arrive at the next moment at the moment speed rather than run into it at full speed.
    if (inSpan < 0 && next >= 0)
        h.speed = std::min(h.speed, std::max(HighlightsMomentSpeed(), (h.spans[next].first - t) / kArriveWall));
    h.speed = std::max(0.05f, h.speed);
    m_replayCtx.playbackSpeed = h.speed;
    return h.speed;
}

// ---------------------------------------------------------------------------
// The camera and the people in the moment. Runs in Update(), before the follow camera places itself.
// ---------------------------------------------------------------------------

// One moment's shot at time t: who is in it so far, where, and the camera it wants. Both halves
// of the screen frame their moment with it; aspect is the shape of the half it will be seen in.
struct ReplayWindow::HighlightShot
{
    struct Placed { XMFLOAT3 pos; Highlights::Role role; int agent; };
    std::vector<Placed> placed;
    XMFLOAT3 target{0.f, 0.f, 0.f};   // the target's feet
    XMFLOAT3 group{0.f, 0.f, 0.f};    // everyone's centre, the target counted twice
    float spread = 0.f, reach = 0.f;  // around the group centre, around the target
    float prog = 0.f;                 // 0 at the moment's start, 1 at the decisive instant (eased)
    float yaw = 0.f, dist = 0.f, pitch = 0.f;
};

bool ReplayWindow::FrameHighlight(int index, float t, float refYaw, float aspect, HighlightShot& out) const
{
    const auto& h = *m_hl;
    const auto& mo = h.moments[index];
    out = HighlightShot{};
    bool haveTarget = false;
    for (const auto& p : mo.people)
    {
        if (p.from > t + 0.3f && p.role != Highlights::Role::Target) continue;
        auto it = m_replayCtx.agents.find(p.agent);
        if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) continue;
        float sx, sy, sz;
        InterpolateAgentPosition(it->second, t, m_replayCtx.interpSettings, sx, sy, sz);
        const XMFLOAT3 w = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);
        out.placed.push_back({ w, p.role, p.agent });
        if (p.role == Highlights::Role::Target) { out.target = w; haveTarget = true; }
    }
    if (!haveTarget) return false;
    const XMFLOAT3 tp = out.target;

    // How far into the moment: the shot opens wide on everyone and closes in a little on the
    // target as the instant comes (owner, 2026-10-29).
    const float span = std::max(0.5f, mo.key - mo.start);
    float prog = std::clamp((t - mo.start) / span, 0.f, 1.f);
    out.prog = prog * prog * (3.f - 2.f * prog);

    XMFLOAT3 c{ tp.x * 2.f, tp.y * 2.f, tp.z * 2.f };
    float wsum = 2.f;
    XMFLOAT2 enemyMean{0.f, 0.f};
    int enemies = 0;
    for (const auto& p : out.placed)
    {
        if (p.role == Highlights::Role::Target) continue;
        c.x += p.pos.x; c.y += p.pos.y; c.z += p.pos.z; wsum += 1.f;
        if (p.role == Highlights::Role::Enemy) { enemyMean.x += p.pos.x; enemyMean.y += p.pos.z; ++enemies; }
    }
    out.group = XMFLOAT3(c.x / wsum, c.y / wsum, c.z / wsum);
    for (const auto& p : out.placed)
    {
        out.spread = std::max(out.spread, std::hypot(p.pos.x - out.group.x, p.pos.z - out.group.z));
        out.reach  = std::max(out.reach, std::hypot(p.pos.x - tp.x, p.pos.z - tp.z));
    }

    // The heading: across the line from the casters to the target, a little behind the casters, so
    // the pressure reads from one side of the screen to the other; the side nearer the current
    // heading, so a new moment swings it the least. Off: the current heading stays.
    out.yaw = refYaw;
    if (h.turnCamera && enemies > 0)
    {
        enemyMean.x /= enemies; enemyMean.y /= enemies;
        float dx = tp.x - enemyMean.x, dz = tp.z - enemyMean.y;
        const float len = std::hypot(dx, dz);
        if (len > 1.f)
        {
            dx /= len; dz /= len;
            const float sideA[2] = { -dz, dx }, sideB[2] = { dz, -dx };
            const float cy = std::sin(refYaw), cz = std::cos(refYaw);
            const float* side = (sideA[0] * cy + sideA[1] * cz >= sideB[0] * cy + sideB[1] * cz) ? sideA : sideB;
            out.yaw = std::atan2(side[0] - 0.5f * dx, side[1] - 0.5f * dz);
        }
    }

    // The wide distance fits everyone in; the close one keeps most of it.
    float wideDist = kWideMinDist;
    if (Camera* cam = m_mapRenderer->GetCamera())
    {
        const float halfV = cam->GetFovY() * 0.5f;
        const float halfH = std::atan(std::tan(halfV) * aspect);
        wideDist = (out.spread + 120.f) / std::tan(std::min(halfV, halfH));
    }
    wideDist = std::clamp(wideDist, kWideMinDist, kWideMaxDist);
    const float closeDist = std::max(kCloseDist, wideDist * kCloseKeep);
    out.dist  = wideDist + (closeDist - wideDist) * out.prog;
    out.pitch = kWidePitch + (kClosePitch - kWidePitch) * out.prog;
    return true;
}

void ReplayWindow::UpdateHighlights(float dt)
{
    // Found when the match loads, whether or not Highlights is on, so the button can say how many.
    auto& h = HL();
    if (!h.settingsLoaded) LoadHighlightsSettings();
    if (!h.built) BuildHighlights();
    UpdateHighlightsBatch();
    const bool framing = h.active || h.batch == HighlightsState::Batch::Running;
    if (!framing || !h.built)
    {
        m_followCenterBias = XMFLOAT3(0.f, 0.f, 0.f);
        return;
    }
    RefreshHighlightsSelection();
    const float t = m_debugTimeline;

    // The moment to frame. A moment keeps its half of the screen until it is over (owner,
    // 2026-10-29: switching 1-2-1 views mid-fight was disturbing). With the screen split, both
    // halves stay until BOTH moments are over; a half whose moment ended keeps its shot, and a
    // new moment may take it. Otherwise: the best moment playing, else the next one coming up.
    if (h.second >= 0 && HighlightOver(h.current, t) && HighlightOver(h.second, t))
        h.second = -1;
    const bool split = h.second >= 0;
    int best = h.current;
    if (HighlightOver(h.current, t))
    {
        int cand = -1;
        for (int i : h.shown)
        {
            if (i == h.second || !HighlightPlaying(i, t)) continue;
            if (split && HighlightsRelated(i, h.second)) continue;
            if (cand < 0 || h.moments[i].score > h.moments[cand].score) cand = i;
        }
        if (cand >= 0)
            best = cand;
        else if (!split && h.current >= 0 && t > h.moments[h.current].end)
            for (int i : h.shown)
                if (h.moments[i].start > t) { best = i; break; }
    }

    if (best != h.current && best >= 0)
    {
        h.current = best;
        h.userYaw = h.userDist = h.followDropped = false;
        h.clearYaw[0] = h.clearLift[0] = h.clearTimer[0] = 0.f;
        h.frameValid = false;
        EnterFollowMode(h.moments[best].target);
        h.lastDist = m_followDistTarget;
    }

    // In a moment or not: the grey look eases in and out.
    bool inMoment = false;
    for (int i : h.shown)
        if (t >= h.moments[i].start && t < h.moments[i].end) { inMoment = true; break; }
    const float fadeStep = std::clamp(dt / kFilterFade, 0.f, 1.f);
    h.filterAmt = std::clamp(h.filterAmt + (inMoment ? fadeStep : -fadeStep), 0.f, 1.f);

    h.involved.clear();
    h.maskFeet.clear();
    h.spotValid = false;
    if (h.current < 0)
    {
        m_followCenterBias = XMFLOAT3(0.f, 0.f, 0.f);
        h.second = -1;
        h.splitAmt = 0.f;
        return;
    }
    const auto& mo = h.moments[h.current];

    // Escape (or a click on another player) took the camera off the target: leave it to the user
    // until the next moment.
    if (m_cameraMode != CameraMode::FollowAgent || m_followedAgentId != mo.target) h.followDropped = true;

    for (const auto& p : mo.people) h.involved.insert(p.agent);

    // The screen's shape, and the main view's when the right half is the second moment's.
    const D3D11_VIEWPORT svp = m_deviceResources->GetScreenViewport();
    const float fullAspect = svp.Height > 0.f ? svp.Width / svp.Height : 16.f / 9.f;
    const float mainAspect = fullAspect * (1.f - 0.5f * h.splitAmt);

    HighlightShot shot;
    if (!FrameHighlight(h.current, t, m_followYaw - h.clearYaw[0], mainAspect, shot)) { m_followCenterBias = XMFLOAT3(0.f, 0.f, 0.f); return; }
    ProbeHighlightAngle(0, shot, dt);
    const XMFLOAT3 targetPos = shot.target;
    const XMFLOAT3 c = shot.group;
    const float prog = shot.prog, reach = shot.reach;
    const auto& placed = shot.placed;
    const float wantYaw = shot.yaw + h.clearYaw[0], wantDist = shot.dist;
    const float wantPitch = std::min(shot.pitch + h.clearLift[0], kFollowMaxPitch);

    // A new moment opens on the wide shot at once; within it everything is eased.
    if (!h.frameValid)
    {
        h.frameYaw = wantYaw;
        h.frameDist = wantDist;
        h.bias = XMFLOAT3(c.x - targetPos.x, c.y - targetPos.y, c.z - targetPos.z);
        h.spotRadius = std::clamp(reach + 150.f, kSpotMin, kSpotMax);
        h.frameValid = true;
        if (!h.followDropped)
        {
            m_followDist = m_followDistTarget = wantDist;
            m_followPitch = wantPitch;
            h.lastDist = wantDist;
        }
    }
    const float ky = 1.f - std::exp(-dt * 1.5f);
    const float kd = 1.f - std::exp(-dt * 2.0f);
    h.frameYaw  = WrapAngle(h.frameYaw + WrapAngle(wantYaw - h.frameYaw) * ky);
    h.frameDist += (wantDist - h.frameDist) * kd;

    const float lean = 1.f - prog;
    const XMFLOAT3 wantBias{ (c.x - targetPos.x) * lean, (c.y - targetPos.y) * lean, (c.z - targetPos.z) * lean };
    h.bias.x += (wantBias.x - h.bias.x) * kd;
    h.bias.y += (wantBias.y - h.bias.y) * kd;
    h.bias.z += (wantBias.z - h.bias.z) * kd;

    if (h.followDropped)
    {
        m_followCenterBias = XMFLOAT3(0.f, 0.f, 0.f);
    }
    else
    {
        // Right-drag turns the camera, the wheel zooms: the user's for the rest of this moment.
        if (m_rightMouseDown) h.userYaw = true;
        if (h.lastDist >= 0.f && std::fabs(m_followDistTarget - h.lastDist) > 1.f) h.userDist = true;
        if (!h.userYaw)
        {
            m_followYaw = WrapAngle(m_followYaw + WrapAngle(h.frameYaw - m_followYaw) * ky);
            m_followPitch += (wantPitch - m_followPitch) * kd;
        }
        if (!h.userDist)
            m_followDistTarget = h.frameDist;
        h.lastDist = m_followDistTarget;
        m_followCenterBias = h.bias;

        // With the right half taken, the orbit centre moves right by a quarter of the screen's
        // width at that distance, so the target sits in the middle of the left half.
        if (h.splitAmt > 0.f)
            if (Camera* cam = m_mapRenderer->GetCamera())
            {
                XMFLOAT3 r = cam->GetRight3f();
                const float rl = std::hypot(r.x, r.z);
                if (rl > 1e-4f)
                {
                    const float halfH = std::atan(std::tan(cam->GetFovY() * 0.5f) * fullAspect);
                    const float shift = m_followDist * std::tan(halfH) * 0.5f * h.splitAmt;
                    m_followCenterBias.x += r.x / rl * shift;
                    m_followCenterBias.z += r.z / rl * shift;
                }
            }
    }

    UpdateHighlightsSecond(dt, targetPos, fullAspect);

    // The grey look's mask, in the world: a spotlight on the ground centred on the followed player,
    // and each person in it - or everyone, when bystanders stay in colour. Projected at draw time
    // with the frame's own camera. The radius is eased and each person's ellipse grows in over
    // kJoinGrow as they join, so the zone settles instead of jumping.
    XMFLOAT3 spot = targetPos;
    if (m_cameraMode == CameraMode::FollowAgent && m_followedAgentId != mo.target)
        if (auto it = m_replayCtx.agents.find(m_followedAgentId); it != m_replayCtx.agents.end() && !it->second.snapshots.empty())
        {
            float sx, sy, sz;
            InterpolateAgentPosition(it->second, t, m_replayCtx.interpSettings, sx, sy, sz);
            spot = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);
        }
    h.spotCenter = spot;
    h.spotRadius += (std::clamp(reach + 150.f, kSpotMin, kSpotMax) - h.spotRadius) * (1.f - std::exp(-dt * 1.0f));
    h.spotValid = true;
    for (const auto& p : mo.people)
    {
        if (p.role == Highlights::Role::Target) continue;
        const float grow = std::clamp((t - p.from) / kJoinGrow, 0.f, 1.f);
        if (grow <= 0.f) continue;
        for (const auto& q : placed)
            if (q.agent == p.agent) { h.maskFeet.push_back({ q.pos.x, q.pos.y, q.pos.z, grow }); break; }
    }
    h.maskFeet.push_back({ targetPos.x, targetPos.y, targetPos.z, 1.f });
    if (h.bystanders == 0)
        for (const auto& [id, a] : m_replayCtx.agents)
        {
            if (h.involved.count(id) || a.snapshots.empty()) continue;
            if (a.type != AgentType::Player && a.type != AgentType::NPC) continue;
            if (!AgentOnStageAt(id, a, t)) continue;
            float sx, sy, sz;
            InterpolateAgentPosition(a, t, m_replayCtx.interpSettings, sx, sy, sz);
            const XMFLOAT3 w = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);
            h.maskFeet.push_back({ w.x, w.y, w.z, 1.f });
        }
}

// The clearest angle for one half's shot: the turns and lifts of kClearYaws / kClearLifts around the
// shot's own camera, each scored by the sight lines from its eye to the target and the nearest
// others (CountBlockedSightLines: props of any kind, and the ground). A blocked target weighs most;
// then the fewest people hidden; then the smallest change. The half moves only when that unblocks
// something, or brings the camera much nearer its own angle with nothing more blocked.
void ReplayWindow::ProbeHighlightAngle(int slot, const HighlightShot& shot, float dt)
{
    auto& h = *m_hl;
    h.clearTimer[slot] -= dt;
    if (h.clearTimer[slot] > 0.f) return;
    h.clearTimer[slot] = kClearProbeEvery;

    constexpr float kChest = 60.f;
    const XMFLOAT3 target{ shot.target.x, shot.target.y + kChest, shot.target.z };
    std::vector<std::pair<float, XMFLOAT3>> nearby;
    for (const auto& p : shot.placed)
    {
        if (p.role == Highlights::Role::Target) continue;
        nearby.push_back({ std::hypot(p.pos.x - shot.target.x, p.pos.z - shot.target.z),
                         XMFLOAT3(p.pos.x, p.pos.y + kChest, p.pos.z) });
    }
    std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<XMFLOAT3> others;
    for (size_t i = 0; i < nearby.size() && static_cast<int>(i) < kClearOthers; ++i) others.push_back(nearby[i].second);
    const std::vector<XMFLOAT3> targetOnly{ target };

    const float lean = 1.f - shot.prog;
    const XMFLOAT3 look{ shot.target.x + (shot.group.x - shot.target.x) * lean,
                         shot.target.y + (shot.group.y - shot.target.y) * lean + kChest,
                         shot.target.z + (shot.group.z - shot.target.z) * lean };
    auto blocked = [&](float yawOff, float lift) {
        const float yaw = shot.yaw + yawOff;
        const float pitch = std::min(shot.pitch + lift, kFollowMaxPitch);
        const float cp = std::cos(pitch);
        const XMFLOAT3 eye{ look.x + shot.dist * std::sin(yaw) * cp, look.y + shot.dist * std::sin(pitch),
                            look.z + shot.dist * std::cos(yaw) * cp };
        return CountBlockedSightLines(eye, targetOnly) * 10 + (others.empty() ? 0 : CountBlockedSightLines(eye, others));
    };
    auto change = [](float yawOff, float lift) { return std::fabs(yawOff) * 180.f / XM_PI + lift * 100.f; };

    const int curBlocked = blocked(h.clearYaw[slot], h.clearLift[slot]);
    if (curBlocked == 0 && h.clearYaw[slot] == 0.f && h.clearLift[slot] == 0.f) return;   // clear as it is
    int bestBlocked = curBlocked;
    float bestYaw = h.clearYaw[slot], bestLift = h.clearLift[slot];
    for (float lift : kClearLifts)
        for (float deg : kClearYaws)
        {
            const float off = deg * XM_PI / 180.f;
            const int b = blocked(off, lift);
            if (b < bestBlocked || (b == bestBlocked && change(off, lift) < change(bestYaw, bestLift) - 30.f))
            {
                bestBlocked = b;
                bestYaw = off;
                bestLift = lift;
            }
        }
    h.clearYaw[slot] = bestYaw;
    h.clearLift[slot] = bestLift;
}

// A moment is playing from its camera lead to its end; it is over after its end, or when the
// playhead has gone back before it (a seek).
bool ReplayWindow::HighlightPlaying(int i, float t) const
{
    if (i < 0) return false;
    const auto& m = m_hl->moments[i];
    return t >= m.start - kCameraLead && t <= m.end;
}

bool ReplayWindow::HighlightOver(int i, float t) const
{
    if (i < 0) return true;
    const auto& m = m_hl->moments[i];
    return t > m.end || t < m.start - kCameraLead - 0.5f;
}

// Two moments about the same fight: either target takes part in the other.
bool ReplayWindow::HighlightsRelated(int a, int b) const
{
    if (a < 0 || b < 0) return false;
    const auto& ma = m_hl->moments[a];
    const auto& mb = m_hl->moments[b];
    for (const auto& p : mb.people) if (p.agent == ma.target) return true;
    for (const auto& p : ma.people) if (p.agent == mb.target) return true;
    return false;
}

// The split view's second moment: another moment playing now, far from the first, gets the right
// half of the screen with a camera of its own, framed the same way.
void ReplayWindow::UpdateHighlightsSecond(float dt, const XMFLOAT3& mainTarget, float fullAspect)
{
    auto& h = *m_hl;
    const float t = m_debugTimeline;
    // The right half: once open it stays until both moments are over (UpdateHighlights closes it);
    // a right half whose moment ended keeps its shot, or takes a new moment playing elsewhere. It
    // opens only for a moment far enough from the left one that one shot cannot hold both.
    int pick = h.splitView ? h.second : -1;
    if (h.splitView && h.current >= 0 && (h.second < 0 || HighlightOver(h.second, t)))
    {
        const bool opening = h.second < 0;
        int cand = -1;
        for (int i : h.shown)
        {
            if (i == h.current || i == h.second || !HighlightPlaying(i, t) || HighlightsRelated(i, h.current)) continue;
            if (opening)
            {
                auto it = m_replayCtx.agents.find(h.moments[i].target);
                if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) continue;
                float sx, sy, sz;
                InterpolateAgentPosition(it->second, t, m_replayCtx.interpSettings, sx, sy, sz);
                const XMFLOAT3 w = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);
                if (std::hypot(w.x - mainTarget.x, w.z - mainTarget.z) < kSplitMinDist) continue;
            }
            if (cand < 0 || h.moments[i].score > h.moments[cand].score) cand = i;
        }
        if (cand >= 0) pick = cand;
    }
    if (pick != h.second)
    {
        h.second = pick;
        if (pick >= 0) { h.secValid = false; h.secShown = pick; }
        h.clearYaw[1] = h.clearLift[1] = h.clearTimer[1] = 0.f;
    }
    const float step = std::clamp(dt / kSplitSlide, 0.f, 1.f);
    h.splitAmt = std::clamp(h.splitAmt + (h.second >= 0 ? step : -step), 0.f, 1.f);
    if (h.second < 0) return;

    for (const auto& p : h.moments[h.second].people) h.involved.insert(p.agent);
    HighlightShot shot;
    if (!FrameHighlight(h.second, t, (h.secValid ? h.secYaw : m_followYaw) - h.clearYaw[1], fullAspect * 0.5f, shot)) return;
    ProbeHighlightAngle(1, shot, dt);
    shot.yaw += h.clearYaw[1];
    shot.pitch = std::min(shot.pitch + h.clearLift[1], kFollowMaxPitch);
    const float lean = 1.f - shot.prog;
    const XMFLOAT3 wantBias{ (shot.group.x - shot.target.x) * lean, (shot.group.y - shot.target.y) * lean,
                             (shot.group.z - shot.target.z) * lean };
    if (!h.secValid)
    {
        h.secYaw = shot.yaw; h.secDist = shot.dist; h.secPitch = shot.pitch; h.secBias = wantBias;
        h.secValid = true;
    }
    const float ky = 1.f - std::exp(-dt * 1.5f);
    const float kd = 1.f - std::exp(-dt * 2.0f);
    h.secYaw   = WrapAngle(h.secYaw + WrapAngle(shot.yaw - h.secYaw) * ky);
    h.secDist  += (shot.dist - h.secDist) * kd;
    h.secPitch += (shot.pitch - h.secPitch) * kd;
    h.secBias.x += (wantBias.x - h.secBias.x) * kd;
    h.secBias.y += (wantBias.y - h.secBias.y) * kd;
    h.secBias.z += (wantBias.z - h.secBias.z) * kd;
    h.secLook = XMFLOAT3(shot.target.x + h.secBias.x, shot.target.y + h.secBias.y, shot.target.z + h.secBias.z);
}

// The second moment's picture, into a target of its own the size of the right half. Before the
// main view, like the Split Camera's.
void ReplayWindow::RenderHighlightsSecondView()
{
    if (!m_hl || !m_deviceResources) return;
    auto& h = *m_hl;
    if (!h.active && h.batch != HighlightsState::Batch::Running) return;
    if (h.splitAmt <= 0.f || !h.secValid) return;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    const D3D11_VIEWPORT vp = m_deviceResources->GetScreenViewport();
    const int w = std::max(1, static_cast<int>(vp.Width * 0.5f));
    const int hgt = std::max(1, static_cast<int>(vp.Height));
    if (h.secDevice != dev || h.secW != w || h.secH != hgt || !h.secRTV)
    {
        h.secTex.Reset(); h.secDepth.Reset(); h.secRTV.Reset(); h.secDSV.Reset(); h.secSRV.Reset();
        h.secDevice = dev; h.secW = w; h.secH = hgt;
        const DXGI_FORMAT colorFmt = m_deviceResources->GetBackBufferFormat();
        const DXGI_FORMAT depthFmt = m_deviceResources->GetDepthBufferFormat();
        CD3D11_TEXTURE2D_DESC texDesc(colorFmt, w, hgt, 1, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        CD3D11_TEXTURE2D_DESC depthDesc(depthFmt, w, hgt, 1, 1, D3D11_BIND_DEPTH_STENCIL);
        CD3D11_RENDER_TARGET_VIEW_DESC rtvDesc(D3D11_RTV_DIMENSION_TEXTURE2D, colorFmt);
        CD3D11_SHADER_RESOURCE_VIEW_DESC srvDesc(D3D11_SRV_DIMENSION_TEXTURE2D, colorFmt, 0, 1);
        CD3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc(D3D11_DSV_DIMENSION_TEXTURE2D);
        if (FAILED(dev->CreateTexture2D(&texDesc, nullptr, h.secTex.GetAddressOf())) ||
            FAILED(dev->CreateRenderTargetView(h.secTex.Get(), &rtvDesc, h.secRTV.GetAddressOf())) ||
            FAILED(dev->CreateShaderResourceView(h.secTex.Get(), &srvDesc, h.secSRV.GetAddressOf())) ||
            FAILED(dev->CreateTexture2D(&depthDesc, nullptr, h.secDepth.GetAddressOf())) ||
            FAILED(dev->CreateDepthStencilView(h.secDepth.Get(), &dsvDesc, h.secDSV.GetAddressOf())))
        {
            h.secTex.Reset(); h.secDepth.Reset(); h.secRTV.Reset(); h.secDSV.Reset(); h.secSRV.Reset();
            return;
        }
    }
    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;
    const float cosP = std::cos(h.secPitch);
    const XMFLOAT3 eye{ h.secLook.x + h.secDist * std::sin(h.secYaw) * cosP,
                        h.secLook.y + h.secDist * std::sin(h.secPitch),
                        h.secLook.z + h.secDist * std::cos(h.secYaw) * cosP };
    RenderSecondaryView(eye, h.secYaw, h.secPitch, cam->GetFovY(), w, hgt, h.secRTV.Get(), h.secDSV.Get(), &h.secViewProj);
}

// The right half: the second moment's picture over the main view's (after the agent labels, so the
// main view's names do not show through), a gold seam, and the second moment's card.
void ReplayWindow::DrawHighlightsSplit()
{
    if (!m_hl) return;
    auto& h = *m_hl;
    if (!h.active && h.batch != HighlightsState::Batch::Running) return;
    if (h.splitAmt <= 0.f || !h.secValid || !h.secSRV || h.secShown < 0) return;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float halfW = disp.x * 0.5f;
    const float ease = h.splitAmt * h.splitAmt * (3.f - 2.f * h.splitAmt);
    const float x0 = disp.x - halfW * ease;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddImage(reinterpret_cast<ImTextureID>(h.secSRV.Get()), ImVec2(x0, 0.f), ImVec2(x0 + halfW, disp.y));
    // Names, skill icons, lasers, flags and the target's numbers, as on the main view.
    if (m_agentsClassified && m_skillUseTimelineBuilt)
    {
        const int target = h.moments[h.secShown].target;
        UpdateSecondaryFloaters(target, h.secFloaters, h.secFloaterAgent, h.secFloaterScan, h.secFloaterToggle);
        bg->PushClipRect(ImVec2(x0, 0.f), ImVec2(x0 + halfW, disp.y), true);
        DrawSecondaryViewOverlay(bg, h.secViewProj, ImVec2(x0, 0.f), halfW, disp.y, target,
                                 HighlightsOverheadMode() >= 1 ? nullptr : &h.secFloaters);
        bg->PopClipRect();
    }
    bg->AddRectFilled(ImVec2(x0 - 2.f, 0.f), ImVec2(x0 + 1.f, disp.y), cGold);
    bg->AddRectFilled(ImVec2(x0 + 1.f, 0.f), ImVec2(x0 + 4.f, disp.y), TeamColour(h.teams[h.secShown], 200));

    const int idx = static_cast<int>(std::find(h.shown.begin(), h.shown.end(), h.secShown) - h.shown.begin());
    const std::string count = idx < static_cast<int>(h.shown.size()) ? std::format("{} / {}", idx + 1, h.shown.size()) : std::string();
    DrawCardAt(ImGui::GetBackgroundDrawList(), x0 + 16.f, std::max(m_ribbonBottomY, 0.f) + 14.f,
               h.cards[h.secShown].title, h.cards[h.secShown].detail, count, h.teams[h.secShown], ease);
}

// The mask in screen uv, for the camera of this frame: the spotlight's ground circle as the
// ellipse around its projection, and one upright ellipse from each person's feet to their head.
void ReplayWindow::BuildHighlightsMask(std::vector<DirectX::XMFLOAT4>& out) const
{
    out.clear();
    const auto& h = *m_hl;
    Camera* cam = m_mapRenderer ? m_mapRenderer->GetCamera() : nullptr;
    if (!cam || !h.spotValid) return;
    const D3D11_VIEWPORT vp = m_deviceResources->GetScreenViewport();
    const XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    const float vw = std::max(1.f, vp.Width), vh = std::max(1.f, vp.Height);

    {
        const XMFLOAT3 c = h.spotCenter;
        const float r = h.spotRadius;
        float mnx = FLT_MAX, mny = FLT_MAX, mxx = -FLT_MAX, mxy = -FLT_MAX;
        int got = 0;
        for (int k = 0; k < 12; ++k)
        {
            const float a = XM_2PI * k / 12.f;
            const XMFLOAT3 p{ c.x + r * std::cos(a), c.y, c.z + r * std::sin(a) };
            float sx, sy;
            if (!ProjectToScreen(viewProj, vw, vh, p, sx, sy)) continue;
            mnx = std::min(mnx, sx); mny = std::min(mny, sy); mxx = std::max(mxx, sx); mxy = std::max(mxy, sy);
            ++got;
        }
        if (got >= 6)
            out.push_back({ (mnx + mxx) * 0.5f / vw, (mny + mxy) * 0.5f / vh,
                            (mxx - mnx) * 0.5f / vw, (mxy - mny) * 0.5f / vh });
    }

    for (const XMFLOAT4& f : h.maskFeet)
    {
        if (static_cast<int>(out.size()) >= kMaxEllipses) break;
        const XMFLOAT3 feet{ f.x, f.y, f.z };
        const XMFLOAT3 head{ feet.x, feet.y + kCharHeight, feet.z };
        float fx, fy, hx, hy;
        if (!ProjectToScreen(viewProj, vw, vh, feet, fx, fy) || !ProjectToScreen(viewProj, vw, vh, head, hx, hy)) continue;
        const float tall = std::max(8.f, std::hypot(hx - fx, hy - fy));
        const float ry = tall * 0.75f * f.w, rx = std::max(tall * 0.45f, 10.f) * f.w;
        out.push_back({ (fx + hx) * 0.5f / vw, (fy + hy) * 0.5f / vh, rx / vw, ry / vh });
    }
}

// A player or NPC kept off the screen: Highlights on, bystanders hidden, a moment playing, and this
// agent not in it. Spirits, flags and items stay.
bool ReplayWindow::HighlightsHidesAgent(int agentId) const
{
    if (!m_hl || m_hl->bystanders != 2 || m_hl->current < 0 || m_hl->filterAmt <= 0.f) return false;
    if (!m_hl->active && m_hl->batch != HighlightsState::Batch::Running) return false;
    if (m_hl->involved.count(agentId)) return false;
    auto it = m_replayCtx.agents.find(agentId);
    return it != m_replayCtx.agents.end() &&
           (it->second.type == AgentType::Player || it->second.type == AgentType::NPC);
}

// ---------------------------------------------------------------------------
// The grey look: after the bloom, before the interface. The frame is copied and written back,
// greyed and darkened outside the mask.
// ---------------------------------------------------------------------------

void ReplayWindow::DrawHighlightsFilter()
{
    if (!m_hl || !m_deviceResources) return;
    auto& h = *m_hl;
    if (!h.active && h.batch != HighlightsState::Batch::Running) return;
    const float amount = h.filterAmt * h.filterStrength;
    if (amount <= 0.01f) return;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    auto* ctx = m_deviceResources->GetD3DDeviceContext();
    ID3D11RenderTargetView* backRTV = m_deviceResources->GetRenderTargetView();
    if (!dev || !ctx || !backRTV) return;

    // Everything here belongs to this window's device; a new device starts over.
    if (h.device != dev)
    {
        h.device = dev;
        h.shadersTried = false;
        h.vs.Reset(); h.ps.Reset(); h.cb.Reset(); h.sampler.Reset(); h.bs.Reset(); h.dss.Reset(); h.rs.Reset();
        h.sceneTex.Reset(); h.sceneSRV.Reset(); h.texW = h.texH = 0;
    }
    if (!h.shadersTried)
    {
        h.shadersTried = true;
        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
        auto compile = [&](const char* entry, const char* target, ID3DBlob** out) {
            errBlob.Reset();
            if (SUCCEEDED(D3DCompile(kHighlightsHLSL, sizeof(kHighlightsHLSL), nullptr, nullptr, nullptr, entry, target,
                                     D3DCOMPILE_ENABLE_STRICTNESS, 0, out, errBlob.GetAddressOf())))
                return true;
            if (errBlob) RunLog::Line("highlights: shader %s failed to build: %s", entry,
                                      static_cast<const char*>(errBlob->GetBufferPointer()));
            return false;
        };
        if (!compile("VSMain", "vs_5_0", vsBlob.GetAddressOf()) || !compile("PSMain", "ps_5_0", psBlob.GetAddressOf())) return;
        if (FAILED(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, h.vs.GetAddressOf())) ||
            FAILED(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, h.ps.GetAddressOf())))
        { h.vs.Reset(); h.ps.Reset(); return; }

        D3D11_BUFFER_DESC cbd = {};
        cbd.ByteWidth = sizeof(HighlightsCBData);
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&cbd, nullptr, h.cb.GetAddressOf());

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, h.sampler.GetAddressOf());

        D3D11_DEPTH_STENCIL_DESC dsd = {};
        dsd.DepthEnable = FALSE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dev->CreateDepthStencilState(&dsd, h.dss.GetAddressOf());

        D3D11_BLEND_DESC bld = {};
        bld.RenderTarget[0].BlendEnable = FALSE;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bld, h.bs.GetAddressOf());

        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&rd, h.rs.GetAddressOf());
    }
    if (!h.vs || !h.ps || !h.cb || !h.sampler) return;

    Microsoft::WRL::ComPtr<ID3D11Resource> backRes;
    backRTV->GetResource(backRes.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backTex;
    if (FAILED(backRes.As(&backTex))) return;
    D3D11_TEXTURE2D_DESC backDesc = {};
    backTex->GetDesc(&backDesc);
    if (!h.sceneSRV || h.texW != static_cast<int>(backDesc.Width) || h.texH != static_cast<int>(backDesc.Height))
    {
        h.sceneTex.Reset(); h.sceneSRV.Reset();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = backDesc.Width;
        td.Height = backDesc.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = backDesc.Format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, h.sceneTex.GetAddressOf())) ||
            FAILED(dev->CreateShaderResourceView(h.sceneTex.Get(), nullptr, h.sceneSRV.GetAddressOf())))
        { h.sceneTex.Reset(); h.sceneSRV.Reset(); return; }
        h.texW = static_cast<int>(backDesc.Width);
        h.texH = static_cast<int>(backDesc.Height);
    }
    if (backDesc.SampleDesc.Count > 1)
        ctx->ResolveSubresource(h.sceneTex.Get(), 0, backTex.Get(), 0, backDesc.Format);
    else
        ctx->CopyResource(h.sceneTex.Get(), backTex.Get());

    std::vector<XMFLOAT4> ellipses;
    BuildHighlightsMask(ellipses);
    HighlightsCBData cbData = {};
    const int n = std::min(static_cast<int>(ellipses.size()), kMaxEllipses);
    for (int i = 0; i < n; ++i) cbData.ellipses[i] = ellipses[i];
    cbData.amount = amount;
    cbData.count  = static_cast<float>(n);
    cbData.darken = 0.6f;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(h.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, &cbData, sizeof(cbData));
    ctx->Unmap(h.cb.Get(), 0);

    // ---- save the state this touches ----
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   prevRS;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> prevDSS;
    UINT prevStencilRef = 0;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        prevBS;
    FLOAT prevBF[4] = {0, 0, 0, 0};
    UINT prevSM = 0xFFFFFFFF;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      prevVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       prevPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>       prevIL;
    D3D11_PRIMITIVE_TOPOLOGY prevTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Microsoft::WRL::ComPtr<ID3D11Buffer>            prevPSCB0;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      prevSamp0;
    ID3D11RenderTargetView* prevRTVs[2] = {nullptr, nullptr};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>  prevDSV;
    ctx->RSGetState(prevRS.GetAddressOf());
    ctx->OMGetDepthStencilState(prevDSS.GetAddressOf(), &prevStencilRef);
    ctx->OMGetBlendState(prevBS.GetAddressOf(), prevBF, &prevSM);
    ctx->VSGetShader(prevVS.GetAddressOf(), nullptr, nullptr);
    ctx->PSGetShader(prevPS.GetAddressOf(), nullptr, nullptr);
    ctx->IAGetInputLayout(prevIL.GetAddressOf());
    ctx->IAGetPrimitiveTopology(&prevTopo);
    ctx->PSGetConstantBuffers(0, 1, prevPSCB0.GetAddressOf());
    ctx->PSGetSamplers(0, 1, prevSamp0.GetAddressOf());
    ctx->OMGetRenderTargets(2, prevRTVs, prevDSV.GetAddressOf());
    D3D11_VIEWPORT prevVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT prevVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&prevVpCount, prevVp);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(h.texW);
    vp.Height = static_cast<float>(h.texH);
    vp.MaxDepth = 1.f;
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &backRTV, nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(h.vs.Get(), nullptr, 0);
    ctx->PSSetShader(h.ps.Get(), nullptr, 0);
    ctx->RSSetState(h.rs.Get());
    ctx->OMSetDepthStencilState(h.dss.Get(), 0);
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(h.bs.Get(), blendFactor, 0xFFFFFFFF);
    ctx->PSSetConstantBuffers(0, 1, h.cb.GetAddressOf());
    ctx->PSSetSamplers(0, 1, h.sampler.GetAddressOf());
    ID3D11ShaderResourceView* srv = h.sceneSRV.Get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSRV);

    // ---- restore ----
    ctx->OMSetRenderTargets(prevRTVs[1] ? 2u : 1u, prevRTVs, prevDSV.Get());
    for (auto*& r : prevRTVs)
        if (r) { r->Release(); r = nullptr; }
    if (prevVpCount > 0) ctx->RSSetViewports(prevVpCount, prevVp);
    ctx->RSSetState(prevRS.Get());
    ctx->OMSetDepthStencilState(prevDSS.Get(), prevStencilRef);
    ctx->OMSetBlendState(prevBS.Get(), prevBF, prevSM);
    ctx->VSSetShader(prevVS.Get(), nullptr, 0);
    ctx->PSSetShader(prevPS.Get(), nullptr, 0);
    ctx->IASetInputLayout(prevIL.Get());
    if (prevTopo != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) ctx->IASetPrimitiveTopology(prevTopo);
    ctx->PSSetConstantBuffers(0, 1, prevPSCB0.GetAddressOf());
    ctx->PSSetSamplers(0, 1, prevSamp0.GetAddressOf());
}

// ---------------------------------------------------------------------------
// Play bar: the Highlights button, and the moments as marks under the track
// ---------------------------------------------------------------------------

// Draws the pill at (x, y) and returns its width. On the play bar, left of the Clip button.
void ReplayWindow::DrawHighlightsButton(float x, float y, float h)
{
    const float w = 104.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = font->FontSize;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::InvisibleButton("##Highlights", ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    const bool on = HighlightsActive();
    if (ImGui::IsItemClicked() && !IsClipRendering()) ToggleHighlights();

    const ImU32 bg  = on ? IM_COL32(200, 168, 75, 70) : hov ? IM_COL32(200, 168, 75, 34) : IM_COL32(0, 0, 0, 76);
    const ImU32 bdr = (on || hov) ? cBorderGlow : cBorderHi;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), bg, h * 0.5f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), bdr, h * 0.5f);
    const ImU32 fg = (on || hov) ? cGoldBright : cTextMid;
    // A star: the moments worth watching. Its inner pentagon and the five points, each convex.
    const ImVec2 sc(x + 17.f, y + h * 0.5f);
    ImVec2 pts[10];
    for (int k = 0; k < 10; ++k)
    {
        const float r = (k % 2 == 0) ? 7.f : 3.f;
        const float a = -XM_PIDIV2 + k * XM_PI / 5.f;
        pts[k] = ImVec2(sc.x + r * std::cos(a), sc.y + r * std::sin(a));
    }
    const ImVec2 inner[5] = { pts[1], pts[3], pts[5], pts[7], pts[9] };
    dl->AddConvexPolyFilled(inner, 5, fg);
    for (int k = 0; k < 10; k += 2)
        dl->AddTriangleFilled(pts[(k + 9) % 10], pts[k], pts[k + 1], fg);
    dl->AddText(font, fs, ImVec2(x + 30.f, y + (h - fs) * 0.5f), fg, "Highlights");
    if (hov)
    {
        std::string tip = on ? "Leave Highlights: play the whole match" : "Play only the key moments of the match";
        if (!on && m_hl && m_hl->built) tip += std::format(" ({} found)", m_hl->moments.size());
        ImGui::SetTooltip("%s", tip.c_str());
    }
}

void ReplayWindow::DrawHighlightsTimelineMarkers(float trackX, float trackW, float trackY, float trackH, float maxT)
{
    if (!HighlightsActive() || maxT <= 0.f) return;
    auto& h = *m_hl;
    RefreshHighlightsSelection();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y0 = trackY + trackH + 1.f, y1 = y0 + 3.f;
    for (int i : h.shown)
    {
        const auto& m = h.moments[i];
        const float x0 = trackX + trackW * std::clamp(m.start / maxT, 0.f, 1.f);
        const float x1 = trackX + trackW * std::clamp(m.end / maxT, 0.f, 1.f);
        const float xk = trackX + trackW * std::clamp(m.key / maxT, 0.f, 1.f);
        const bool cur = i == h.current;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(std::max(x1, x0 + 2.f), y1), TeamColour(h.teams[i], cur ? 230 : 140));
        dl->AddLine(ImVec2(xk, trackY - 3.f), ImVec2(xk, y1), TeamColour(h.teams[i], cur ? 255 : 170),
                    (m.flags & Highlights::kDeath) ? 2.f : 1.f);
    }
}

// The speed shown on the play bar while Highlights drives it.
std::string ReplayWindow::HighlightsSpeedLabel() const
{
    if (!HighlightsActive()) return {};
    const float s = m_replayCtx.playbackSpeed;
    return s >= 1.95f ? std::format("{:.0f}x", s) : std::format("{:.2g}x", s);
}

// ---------------------------------------------------------------------------
// The card over the replay while a moment plays
// ---------------------------------------------------------------------------

void ReplayWindow::DrawHighlightsCard()
{
    if (!HighlightsActive()) return;
    auto& h = *m_hl;
    // The background list: the clean screen keeps only that one. Drawn after the agent labels.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // The way back from the clean screen, for a few seconds after the interface goes.
    if (HighlightsHidesInterface() && ImGui::GetTime() - h.cleanSince < kCleanHintSeconds)
    {
        const char* hint = "UI hidden \xC2\xB7 Esc shows it again";
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const ImVec2 sz = ImGui::CalcTextSize(hint);
        const ImVec2 p((disp.x - sz.x) * 0.5f, disp.y - sz.y - 24.f);
        dl->AddRectFilled(ImVec2(p.x - 10.f, p.y - 5.f), ImVec2(p.x + sz.x + 10.f, p.y + sz.y + 5.f), IM_COL32(10, 9, 7, 200), 6.f);
        dl->AddText(p, cTextMid, hint);
    }

    std::string title, detail, count;
    uint8_t team = Team::None;
    float alpha = 1.f;
    if (h.ended && !m_replayCtx.isPlaying)
    {
        title = "End of highlights";
        detail = std::format("{} moments \xC2\xB7 Page Up to go back, or leave Highlights for the whole match", h.shown.size());
    }
    else if (h.built && h.shown.empty())
    {
        title = h.moments.empty() ? "No key moments in this match" : "No moment matches the filters";
        detail = "Change the filters in the Highlights panel";
    }
    else
    {
        if (h.current < 0 || h.filterAmt <= 0.f) return;
        const int idx = static_cast<int>(std::find(h.shown.begin(), h.shown.end(), h.current) - h.shown.begin());
        title  = h.cards[h.current].title;
        detail = h.cards[h.current].detail;
        team   = h.teams[h.current];
        count  = std::format("{} / {}", idx + 1, h.shown.size());
        alpha  = h.filterAmt;
    }
    if (!h.built) return;

    // Top left, under the ribbon: the top centre belongs to the followed player's health bar and
    // the bottom centre to their skill bar (owner, 2026-10-29).
    DrawCardAt(dl, 16.f, std::max(m_ribbonBottomY, 0.f) + 14.f, title, detail, count, team, alpha);
}

// ---------------------------------------------------------------------------
// The panel: what is found, what is kept, how it plays, and the GIFs
// ---------------------------------------------------------------------------

void ReplayWindow::DrawHighlightsPanel()
{
    if (!HighlightsActive() || !m_hl->panelOpen) return;
    auto& h = *m_hl;
    RefreshHighlightsSelection();

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x - 12.f, std::max(m_ribbonBottomY, 0.f) + 50.f), ImGuiCond_FirstUseEver, ImVec2(1.f, 0.f));
    PushGoldPanelStyle();
    bool open = true;
    bool changed = false;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("Highlights###HighlightsPanel", &open, flags))
    {
        // As the GIF panel: gold section titles, dim labels in a left column, bright values.
        const ImVec4 kAccent(0.89f, 0.76f, 0.42f, 1.f);
        const ImVec4 kLabel(0.60f, 0.54f, 0.41f, 1.f);
        const ImVec4 kValue(0.96f, 0.92f, 0.83f, 1.f);
        constexpr float kWidth = 330.f, kLabelW = 104.f;
        const float colW = kWidth * 0.5f;
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
        const float valueW = kWidth - kLabelW;

        if (!h.built)
            ImGui::TextColored(kLabel, "Finding the key moments...");
        else
        {
            float watch = 0.f;
            for (int i : h.shown) watch += (h.moments[i].end - h.moments[i].start) / std::max(0.05f, HighlightsMomentSpeed());
            ImGui::TextColored(kValue, "%d moments", static_cast<int>(h.shown.size()));
            ImGui::SameLine();
            ImGui::TextColored(kLabel, "of %d found \xC2\xB7 about %d:%02d of watching", static_cast<int>(h.moments.size()),
                               static_cast<int>(watch) / 60, static_cast<int>(watch) % 60);
        }

        // ---- SHOW ----
        section("SHOW");
        const float x0 = ImGui::GetCursorPosX();
        changed |= ImGui::Checkbox("Player deaths", &h.showDeaths);
        ImGui::SameLine(x0 + colW);
        changed |= ImGui::Checkbox("Lord, Bodyguard, Knights", &h.showNpcDeaths);
        changed |= ImGui::Checkbox("Spikes", &h.showSpikes);
        ImGui::SameLine(x0 + colW);
        changed |= ImGui::Checkbox("Interrupt pressure", &h.showInterrupts);
        changed |= ImGui::Checkbox("Flag returns", &h.showFlags);
        label("Fit into");
        ImGui::SetNextItemWidth(valueW);
        if (ImGui::Combo("##HlBudget", &h.budgetIdx, kBudgetLabels, static_cast<int>(std::size(kBudgetLabels)))) changed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the biggest moments that fit into this much watching");

        // ---- PACING ----
        section("PACING");
        label("In a moment");
        int ms = IndexOf(kMomentSpeeds, h.momentSpeed, 1);
        ImGui::SetNextItemWidth(valueW);
        if (ImGui::Combo("##HlMomentSpeed", &ms, kMomentSpeedLabels, static_cast<int>(std::size(kMomentSpeedLabels))))
        { h.momentSpeed = kMomentSpeeds[ms]; changed = true; }
        label("Between");
        int gs = IndexOf(kGapSpeeds, h.gapSpeed, 2);
        ImGui::SetNextItemWidth(valueW);
        if (ImGui::Combo("##HlGapSpeed", &gs, kGapLabels, static_cast<int>(std::size(kGapLabels))))
        { h.gapSpeed = kGapSpeeds[gs]; changed = true; }
        changed |= ImGui::Checkbox("Slow motion on the decisive instant", &h.slowMo);
        changed |= ImGui::Checkbox("Turn the camera across the attack", &h.turnCamera);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("On: the camera turns side-on to the attackers and the target. Off: it keeps its heading");
        changed |= ImGui::Checkbox("Split the screen for two moments at once", &h.splitView);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When another moment plays at the same time somewhere else, it takes the right half of the screen");

        // ---- LOOK ----
        section("LOOK");
        label("Grey out");
        float pct = h.filterStrength * 100.f;
        ImGui::SetNextItemWidth(valueW);
        if (ImGui::SliderFloat("##HlGrey", &pct, 0.f, 100.f, "%.0f %% of the scene"))
        { h.filterStrength = pct / 100.f; changed = true; }
        label("Over players");
        ImGui::SetNextItemWidth(valueW);
        if (ImGui::Combo("##HlOverhead", &h.overhead, kOverheadLabels, static_cast<int>(std::size(kOverheadLabels)))) changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Skill icons over the casters, and the damage / heal / energy numbers");
        changed |= ImGui::Checkbox("Hide UI", &h.cleanScreen);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hide the panels, bars and HUD while Highlights plays. Esc shows them again.");
        label("Not in it");
        ImGui::SetNextItemWidth(valueW);
        if (ImGui::Combo("##HlOthers", &h.bystanders, kBystanderLabels, static_cast<int>(std::size(kBystanderLabels)))) changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Players and NPCs who take no part in the moment playing");

        // ---- MOMENTS ----
        section("MOMENTS");
        if (ImGui::Button("< Previous", ImVec2(90.f, 0.f))) HighlightsStep(-1);
        ImGui::SameLine();
        if (ImGui::Button("Next >", ImVec2(90.f, 0.f))) HighlightsStep(1);
        ImGui::SameLine();
        const bool clipBusy = m_clip.phase != ClipExportState::Phase::Closed || h.batch != HighlightsState::Batch::Off;
        ImGui::BeginDisabled(clipBusy || h.shown.empty());
        if (ImGui::Button("Export all as GIFs", ImVec2(kWidth - 180.f - 2.f * ImGui::GetStyle().ItemSpacing.x, 0.f)))
            StartHighlightsBatch();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("One GIF per moment. Pick the first file's name; the rest are numbered beside it.");
        if (h.batch != HighlightsState::Batch::Off)
        {
            ImGui::TextColored(kAccent, "Exporting GIF %d / %d", static_cast<int>(h.batchPos) + 1,
                               static_cast<int>(h.batchQueue.size()));
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop")) { h.batch = HighlightsState::Batch::Off; h.batchQueue.clear(); }
        }

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.25f));
        ImGui::BeginChild("##hlList", ImVec2(kWidth, 300.f), ImGuiChildFlags_None);
        const float t = m_debugTimeline;
        const ImU32 cLabel = ImGui::ColorConvertFloat4ToU32(kLabel);
        const ImU32 cValue = ImGui::ColorConvertFloat4ToU32(kValue);
        for (int i : h.shown)
        {
            const auto& m = h.moments[i];
            ImGui::PushID(i);
            const bool cur = (i == h.current) && t >= m.start - kCameraLead && t <= m.end;
            char tbuf[16];
            FormatTime(m.start - m_displayTimeOffset, tbuf, sizeof(tbuf));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float rowW = ImGui::GetContentRegionAvail().x;
            const float rowH = ImGui::GetTextLineHeight() * 2.f + 8.f;
            if (ImGui::Selectable("##row", cur, ImGuiSelectableFlags_AllowOverlap, ImVec2(rowW - 44.f, rowH)))
                JumpToHighlight(i);
            const bool rowHov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(p.x + 2.f, p.y + 3.f), ImVec2(p.x + 5.f, p.y + rowH - 3.f), TeamColour(h.teams[i], 255), 1.f);
            dl->AddText(ImVec2(p.x + 11.f, p.y + 3.f), cLabel, tbuf);
            dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(p.x + rowW - 46.f, p.y + rowH), true);
            dl->AddText(ImVec2(p.x + 58.f, p.y + 3.f), cur ? cGoldBright : cValue, h.cards[i].title.c_str());
            dl->AddText(ImVec2(p.x + 58.f, p.y + 5.f + ImGui::GetTextLineHeight()), cLabel, h.cards[i].detail.c_str());
            dl->PopClipRect();
            if (rowHov && !h.cards[i].detail.empty()) ImGui::SetTooltip("%s", h.cards[i].detail.c_str());
            ImGui::SetCursorScreenPos(ImVec2(p.x + rowW - 40.f, p.y + (rowH - ImGui::GetFrameHeight()) * 0.5f));
            ImGui::BeginDisabled(clipBusy);
            if (ImGui::SmallButton("GIF")) OpenHighlightClip(i);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Make a GIF clip of this moment");
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + 2.f));
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0.f, 0.f));
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::TextDisabled("J toggles Highlights \xC2\xB7 Page Up / Page Down step");
    }
    ImGui::End();
    PopGoldPanelStyle();

    if (changed) { h.dirty = true; SaveHighlightsSettings(); }
    if (!open) { h.panelOpen = false; SaveHighlightsSettings(); }
}

// ---------------------------------------------------------------------------
// GIFs: one moment in the clip editor, or every moment in a row
// ---------------------------------------------------------------------------

void ReplayWindow::ConfigureClipForHighlight(int index)
{
    auto& h = HL();
    const auto& m = h.moments[index];
    auto& c = m_clip;
    const float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
    c.startTime = std::clamp(m.start, 0.f, maxT);
    c.endTime   = std::clamp(std::min(m.end, m.start + 60.f), c.startTime + 0.5f, maxT);
    c.followAgentId = m.target;
    c.followPlayer = true;
    c.followPosValid = false;
    m_debugTimeline = c.startTime;
}

void ReplayWindow::OpenHighlightClip(int index)
{
    if (m_clip.phase != ClipExportState::Phase::Closed) return;
    OpenClipEditor();
    if (m_clip.phase != ClipExportState::Phase::Editing) return;
    ConfigureClipForHighlight(index);
}

bool ReplayWindow::HighlightsBatchRunning() const
{
    return m_hl && m_hl->batch != HighlightsState::Batch::Off;
}

void ReplayWindow::StartHighlightsBatch()
{
    auto& h = HL();
    RefreshHighlightsSelection();
    if (h.shown.empty() || m_clip.phase != ClipExportState::Phase::Closed) return;
    h.batchQueue = h.shown;
    h.batchPos = 0;
    h.batchDone = 0;
    h.batchSawRender = false;
    h.batchFirst.clear();
    OpenClipEditor();
    if (m_clip.phase != ClipExportState::Phase::Editing) { h.batchQueue.clear(); return; }
    ConfigureClipForHighlight(h.batchQueue[0]);
    h.batch = HighlightsState::Batch::WaitFirst;
    BeginClipSaveDialog();
}

// The batch follows the clip export's phases: the first GIF goes through the save dialog, each
// one after it is started straight away, numbered beside the first.
void ReplayWindow::UpdateHighlightsBatch()
{
    if (!m_hl || m_hl->batch == HighlightsState::Batch::Off) return;
    auto& h = *m_hl;
    auto& c = m_clip;
    using P = ClipExportState::Phase;

    if (c.phase == P::Rendering || c.phase == P::Encoding)
    {
        if (c.phase == P::Rendering && h.batch == HighlightsState::Batch::WaitFirst)
        {
            h.batchFirst = std::filesystem::path(c.outPath);
            h.batch = HighlightsState::Batch::Running;
        }
        h.batchSawRender = true;
        return;
    }
    if (h.batch == HighlightsState::Batch::WaitFirst)
    {
        // The dialog was cancelled or the editor closed before anything started.
        if (!c.dialogOpen && (c.phase == P::Closed || (c.phase == P::Editing && !c.dialogOk)))
        { h.batch = HighlightsState::Batch::Off; h.batchQueue.clear(); }
        return;
    }
    if (!h.batchSawRender) return;
    h.batchSawRender = false;

    // An export just ended. Closed = saved; Editing = cancelled or failed: the batch stops there.
    if (c.phase != P::Closed) { h.batch = HighlightsState::Batch::Off; h.batchQueue.clear(); return; }
    ++h.batchDone;
    if (++h.batchPos >= h.batchQueue.size())
    {
        c.toast = std::format("{} highlight GIFs saved beside {}", h.batchDone, NarrowName(h.batchFirst.filename().wstring()));
        c.toastIsError = false;
        c.toastOverLimit = false;
        c.toastUntil = ImGui::GetTime() + 8.0;
        h.batch = HighlightsState::Batch::Off;
        h.batchQueue.clear();
        return;
    }
    OpenClipEditor();
    if (c.phase != P::Editing) { h.batch = HighlightsState::Batch::Off; h.batchQueue.clear(); return; }
    ConfigureClipForHighlight(h.batchQueue[h.batchPos]);
    const auto dir = h.batchFirst.parent_path();
    const std::wstring stem = h.batchFirst.stem().wstring();
    StartClipExport((dir / std::format(L"{}_{:02}.gif", stem, h.batchPos + 1)).wstring());
}
