#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "LiveSource.h"
#include "GuiGlobalConstants.h"
#include "RunLog.h"
#include "SkillDatabase.h"
#include "SkillSoundTable.h"
#include "SpatialAudioEngine.h"
#include "Animation/GWAnimationHashes.h"

#include <DirectXTex.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <map>

// =================================================================================================
// LIVE MODE (LiveSource.h)
//
// A live window is an ordinary replay window whose match has no files: its source writes every
// agent once the map is in (LiveSeedIfReady), then appends snapshots and StoC events tick by tick
// (LiveUpdate). Every view reads those the way it reads a recording. What changes here:
//   - the clock is the source's display time, one tick behind its newest data, so interpolation
//     always has two snapshots and every event lands later than the frame that shows it;
//   - the caches built once from the whole recording are invalidated, or rebuilt, after an append;
//   - the camera is the GW chase camera on the player; a click picks a target instead of following;
//   - a click on the ground walks there, a click on the skill bar uses the skill, the skills window
//     rearranges the bar (all handed to the source, which decides);
//   - the scrub and playback interface, the intro overlay and the replay hotkeys are off.
// =================================================================================================

namespace
{
double QpcSeconds()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

float WrapAngle(float a)
{
    while (a > DirectX::XM_PI) a -= DirectX::XM_2PI;
    while (a < -DirectX::XM_PI) a += DirectX::XM_2PI;
    return a;
}

// The client's chase camera: about 450 units behind, a little above, eased behind the heading.
constexpr float kLiveCameraDistance = 450.f;
constexpr float kLiveCameraPitch = 0.30f;
constexpr float kLiveCameraEase = 5.0f;          // 1/s, the yaw's approach to "behind"
constexpr float kLiveCameraLookHeight = 70.f;    // ~0.92 of a character's height
constexpr size_t kLiveFrameRing = 240;
// The drag-and-drop payload of a skill: { skill id, bar slot it comes from or -1 from the list }.
constexpr const char* kTrialsSkillPayload = "TRIALS_SKILL";
}

ReplayWindow* ReplayWindow::CreateLive(HINSTANCE hInstance, std::shared_ptr<ILiveSource> source, int mapId,
                                       DATManager* sharedDatManager,
                                       const std::unordered_map<int, std::vector<int>>& hashIndex,
                                       bool offscreen)
{
    if (!source) return nullptr;
    auto* rw = new ReplayWindow();
    rw->m_live = std::move(source);
    rw->m_liveMode = true;

    // A synthetic match: no folder, no date (no flux, no seasonal disguise), version 2 so the
    // party window reads each snapshot's max_hp. The folder name meets no recording.
    rw->m_matchMeta.map_id = mapId;
    rw->m_matchMeta.folder_name = "trials-live_[Blue]vs[Red]";
    rw->m_matchMeta.recording_version = 2;
    SYSTEMTIME today{};
    GetLocalTime(&today);
    rw->m_skillView = GetSkillDatabase().GetView(today.wYear, today.wMonth, today.wDay);
    rw->m_datManager = sharedDatManager;
    rw->m_hashIndex = &hashIndex;
    rw->m_folderTag1 = "Blue";
    rw->m_folderTag2 = "Red";

    rw->m_replayCtx.mapId = mapId;
    rw->m_replayCtx.datMapId = GetDatMapId(mapId);
    // Non-null progress objects: StepValidate starts the file parsers only when these are missing.
    // They never finish; LiveSeedIfReady marks both streams loaded itself.
    rw->m_replayCtx.agentParseProgress = std::make_shared<AgentParseProgress>();
    rw->m_replayCtx.stocParseProgress = std::make_shared<StoCParseProgress>();

    if (!RegisterWindowClass(hInstance)) { delete rw; return nullptr; }
    if (!rw->InitWindow(hInstance, L"Trials - DEV Test Arena (developer)"))
    {
        delete rw;
        return nullptr;
    }
    if (!rw->InitGraphics())
    {
        DestroyWindow(rw->m_hwnd);
        delete rw;
        return nullptr;
    }
    if (!rw->InitLoadingOverlay())
    {
        DestroyWindow(rw->m_hwnd);
        delete rw;
        return nullptr;
    }

    rw->m_alive = true;
    rw->m_useAgentModels = true;
    rw->m_loadingPhase = LoadingPhase::Validate;
    rw->m_liveFrameMs.assign(kLiveFrameRing, 0.f);

    if (offscreen)
    {
        // Off every monitor, visible and never activated (the clip self-test's placement).
        RECT r = {0, 0, 1280, 720};
        AdjustWindowRect(&r, GetWindowLong(rw->m_hwnd, GWL_STYLE), FALSE);
        SetWindowPos(rw->m_hwnd, HWND_BOTTOM, 20000, 20000, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
        ShowWindow(rw->m_hwnd, SW_SHOWNOACTIVATE);
    }
    else
        ShowWindow(rw->m_hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(rw->m_hwnd);
    RunLog::Line("live: a live window opened on map %d (dat 0x%X)%s", mapId, rw->m_replayCtx.datMapId,
                 offscreen ? ", off-screen" : "");
    return rw;
}

void ReplayWindow::LiveSeedIfReady()
{
    if (m_liveSeeded || !m_live || !m_replayCtx.mapLoaded || !m_mapRenderer) return;
    Terrain* terrain = m_mapRenderer->GetTerrain();
    if (!terrain) return;

    LiveWorld world;
    const auto& b = terrain->m_bounds;
    // World (x, z) is game (x, y) under the default transform (flip Y = the height axis only).
    world.centreX = (b.map_min_x + b.map_max_x) * 0.5f;
    world.centreY = (b.map_min_z + b.map_max_z) * 0.5f;
    MapRenderer* renderer = m_mapRenderer.get();
    world.groundZ = [renderer](float x, float y, float& z) {
        float h = 0.f;
        if (!renderer->TryGetGroundHeight(x, y, h)) return false;
        z = -h;   // snapshots are negative up
        return true;
    };

    m_live->Seed(m_replayCtx, m_matchMeta, world);
    float maxT = 0.f;
    for (const auto& [id, ard] : m_replayCtx.agents)
        if (!ard.snapshots.empty()) maxT = std::max(maxT, ard.snapshots.back().time);
    m_replayCtx.maxReplayTime = maxT;
    m_replayCtx.agentsLoaded = true;
    m_replayCtx.stocLoaded = true;
    m_liveSeeded = true;
    m_autoCameraEnabled = false;
    RunLog::Line("live: seeded %zu agent(s) round (%.0f, %.0f)", m_replayCtx.agents.size(), world.centreX,
                 world.centreY);
}

void ReplayWindow::LiveClickAgent(int agentId)
{
    m_liveClickedAgent = agentId;
}

void ReplayWindow::LiveUpdate(float dt)
{
    if (!m_live || !m_liveSeeded || !m_agentsClassified) return;

    // The keyboard is the player's while our process is in front and nothing is being typed.
    DWORD fgPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    const bool processActive = fgPid == GetCurrentProcessId();

    // The self-test clicks a party row the way the party window does: by its row, resolved here.
    const int row = m_live->TakeScriptedPartyRowClick();
    if (row >= 0 && row < static_cast<int>(m_team1PartyIds.size()))
        LiveClickAgent(m_team1PartyIds[row]);

    LiveFrameInput in;
    in.realDt = dt;
    in.keyboardAllowed = processActive && !m_imguiWantTextInput && !m_clSkillSearchFocused;
    in.clickedAgentId = m_liveClickedAgent;
    in.cameraOrbiting = m_rightMouseDown;
    // The inverse of the chase camera's "behind" rule below.
    in.cameraFacing = std::atan2(-std::cos(m_followYaw), -std::sin(m_followYaw));
    in.cameraYaw = m_followYaw;
    in.groundClicked = m_liveGroundClicked;
    in.groundX = m_liveGroundX;
    in.groundY = m_liveGroundY;
    in.groundHeld = m_liveGroundHeld && m_liveHeldValid;
    in.heldX = m_liveHeldX;
    in.heldY = m_liveHeldY;
    in.clickedSkillSlot = m_liveClickedSlot;
    in.setBar = m_liveSetBar;
    for (int i = 0; i < 8; ++i) in.bar[i] = m_liveNewBar[i];
    m_liveClickedAgent = -1;
    m_liveGroundClicked = false;
    m_liveClickedSlot = -1;
    m_liveSetBar = false;

    m_live->Advance(in, m_replayCtx);
    if (m_live->TakeSkillsWindowToggle())
    {
        m_liveSkillsOpen = !m_liveSkillsOpen;
        m_live->Report(m_liveSkillsOpen ? "ui: skills window opened" : "ui: skills window closed");
    }
    LiveTakeScriptedUi();
    m_debugTimeline = static_cast<float>(m_live->DisplayTime());
    m_replayCtx.maxReplayTime = std::max(m_replayCtx.maxReplayTime, static_cast<float>(m_live->SimTime()));
    m_replayCtx.isPlaying = true;
    m_matchOverlayActive = false;
    m_autoCameraEnabled = false;
    LiveInvalidate(m_live->TakeAppended());
    m_liveTargetId = m_live->TargetAgentId();

    if (m_agentModelsLoaded && !m_liveModelsReported)
        LiveReportModels();
    LiveReportAudio();

    // THE CHASE CAMERA: always on the player; GW's distance and pitch the first time, then the
    // user's (wheel, right drag), with the yaw eased behind the heading while the player steers with
    // the keys or autorun. A walk to a clicked point (and the auto-attack's run into range) is not
    // steering: the camera keeps its yaw and only follows the position (the source says which).
    const int playerId = m_live->PlayerAgentId();
    const float behind = [&] {
        const float f = m_live->PlayerFacing();
        return std::atan2(-std::cos(f), -std::sin(f));
    }();
    if (m_cameraMode != CameraMode::FollowAgent || m_followedAgentId != playerId)
    {
        EnterFollowMode(playerId);
        m_followTransActive = false;
        if (!m_liveCameraPlaced)
        {
            m_followDist = m_followDistTarget = kLiveCameraDistance;
            m_followPitch = kLiveCameraPitch;
            m_followYaw = behind;
            m_liveCameraPlaced = true;
        }
    }
    m_followCenterBias = DirectX::XMFLOAT3(0.f, kLiveCameraLookHeight, 0.f);
    if (m_live->PlayerSteering() && !m_rightMouseDown)
    {
        const float k = 1.f - std::exp(-kLiveCameraEase * std::max(dt, 0.f));
        m_followYaw = WrapAngle(m_followYaw + WrapAngle(behind - m_followYaw) * k);
    }
}

void ReplayWindow::LiveInvalidate(const LiveAppended& a)
{
    if (!a.Any()) return;
    // Damage attribution is indexed by combat-packet index: rebuilt from the whole list (and the
    // floaters it feeds with it).
    if (a.combat)
    {
        m_damageAttributionBuilt = false;
        m_damagePops.clear();
    }
    // The forward models read the skill uses and the packets.
    if (a.skills || a.combat)
    {
        m_energyBuilt = false;
        m_adrenalineBuilt = false;
        m_effectTimelineBuilt = false;
        m_skillDisablesBuilt = false;
    }
    // The private schedules: what the effects and the sounds play, rebuilt without replaying what
    // is already past.
    if (a.skills || a.combat || a.visuals)
    {
        LiveRebuildEffectSchedules();
        LiveRebuildSoundPlan();
    }
}

void ReplayWindow::DrawLiveHud()
{
    // The frame time, between two of this window's frames.
    const double now = QpcSeconds();
    double frameMs = 0.0;
    if (m_liveLastFrameQpc > 0.0)
    {
        frameMs = (now - m_liveLastFrameQpc) * 1000.0;
        if (!m_liveFrameMs.empty())
        {
            m_liveFrameMs[m_liveFrameNext % m_liveFrameMs.size()] = static_cast<float>(frameMs);
            ++m_liveFrameNext;
        }
        if (m_live) m_live->OnFrame(frameMs);
    }
    m_liveLastFrameQpc = now;

    const size_t n = std::min(m_liveFrameNext, m_liveFrameMs.size());
    float mean = 0.f, p95 = 0.f;
    if (n > 0)
    {
        std::vector<float> v(m_liveFrameMs.begin(), m_liveFrameMs.begin() + static_cast<std::ptrdiff_t>(n));
        for (float x : v) mean += x;
        mean /= static_cast<float>(n);
        const size_t k = std::min(n - 1, static_cast<size_t>(0.95 * static_cast<double>(n)));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        p95 = v[k];
    }

    // Under the main menu bar.
    ImGui::SetNextWindowPos(ImVec2(10.f, ImGui::GetFrameHeight() + 8.f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##live_fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
    {
        const float fps = mean > 0.f ? 1000.f / mean : 0.f;
        const ImVec4 colour = p95 <= 16.7f ? ImVec4(0.55f, 0.85f, 0.55f, 1.f) : ImVec4(0.95f, 0.55f, 0.35f, 1.f);
        ImGui::TextColored(colour, "%.0f FPS   p95 %.1f ms", fps, p95);
    }
    ImGui::End();

    if (!m_live) return;
    m_live->DrawHud();
    DrawLiveSkillsWindow();

    // A slot dragged off the bar and let go anywhere but on a slot or the skills window: cleared.
    if (const ImGuiPayload* p = ImGui::GetDragDropPayload();
        p && p->IsDataType(kTrialsSkillPayload) && !p->Delivery && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        int data[2] = {};
        std::memcpy(data, p->Data, sizeof(data));
        const ImVec2 m = ImGui::GetIO().MousePos;
        bool onSlot = false;
        for (const ImVec4& r : m_liveSlotRects)
            onSlot = onSlot || (m.x >= r.x && m.x < r.z && m.y >= r.y && m.y < r.w);
        int bar[8] = {};
        if (data[1] >= 0 && data[1] < 8 && !onSlot && m_live->PlayerBar(bar))
        {
            for (int i = 0; i < 8; ++i) m_liveNewBar[i] = bar[i];
            m_liveNewBar[data[1]] = 0;
            m_liveSetBar = true;
        }
    }
    m_liveDragFromSlot = -1;
    LiveGroundInput();
    LiveReportGroundMarker();
    if (m_liveReportHud)
    {
        m_liveReportHud = false;
        m_live->Report("hud: " + m_liveHudReport + "sound " + LiveSoundState());
        m_liveHudReport.clear();
    }
}

// -------------------------------------------------------------------------------------------------
// THE TOP BAR IS GW'S TARGET FRAME. Gw.exe GmAgentStatus.cpp binds it to the current target (0x7DF310 ->
// 0x7E4840, read at 0x516546 / 0x5166FA). Its health fill does not follow the allegiance: 0x517160 picks
// it from the target's own state, the same for yourself, an ally and a foe - poison 205433, bleeding
// 205426, a degeneration hex 205427, otherwise 205431 (the red health fill) - deep wound caps the bar at
// 0.8 (0x94525C) and a dead target reads empty. That is the party row's recipe with the red fill
// (DrawPartyHealthBar), the name white. No target, no frame. The target's cast bar stands under it as
// the replay's top bar draws one; the player's own stands over the skill bar.
// -------------------------------------------------------------------------------------------------
void ReplayWindow::DrawLiveTargetFrame()
{
    const auto it = m_replayCtx.agents.find(m_liveTargetId);
    const AgentSnapshot* snap = m_liveTargetId >= 0 && it != m_replayCtx.agents.end() && !it->second.snapshots.empty()
                                    ? FindSnapshotAtTime(it->second, m_debugTimeline) : nullptr;
    if (!snap)
    {
        if (m_liveReportHud) m_liveHudReport += "target frame none; ";
        return;
    }
    const AgentReplayData& ard = it->second;
    constexpr float kBarW = 320.f, kBarH = 26.f, kTopY = 42.f, kCastGap = 5.f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 tl(std::round(vp->Pos.x + (vp->Size.x - kBarW) * 0.5f), vp->Pos.y + kTopY);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    const PartyIcons icons = LoadAllPartyIcons(dev);
    const std::string name = GetAgentLabel(ard);
    const bool dead = snap->is_dead;
    // Its own fill state, apart from the party row's (which keys its cross-fade by the agent id).
    constexpr int kFrameKey = 0x40000000;
    DrawPartyHealthBar(dl, tl, kBarW, kBarH, snap, 2, dead, name.c_str(), icons, -1, kFrameKey | m_liveTargetId, false,
                       CarriedBundleIcon(dev, m_liveTargetId), 0, false, dev, m_debugTimeline);
    if (m_liveReportHud)
    {
        const char* fill = dead ? "empty (dead)" : snap->has_poison ? "205433 poison" : snap->has_bleeding ? "205426 bleeding"
                         : snap->has_degen_hex ? "205427 hexed" : "205431 health (red)";
        const int playerTeam = [&] {
            const auto p = m_replayCtx.agents.find(m_live ? m_live->PlayerAgentId() : -1);
            return p != m_replayCtx.agents.end() ? static_cast<int>(p->second.teamId) : 0;
        }();
        const char* who = m_live && m_liveTargetId == m_live->PlayerAgentId() ? "self"
                        : static_cast<int>(ard.teamId) == playerTeam ? "ally" : "foe";
        m_liveHudReport += std::format("target frame {} '{}' {} hp {:.3f} fill {} at ({:.0f}, {:.0f}) {:.0f}x{:.0f}; ", m_liveTargetId,
                                       name, who, dead ? 0.f : snap->health_pct, fill, tl.x, tl.y, kBarW, kBarH);
    }
    DrawLiveCastRow(dl, ard, ImVec2(tl.x, tl.y + kBarH + kCastGap), kBarW, "target cast");
}

// The player's own cast bar: centred over the skill bar's window (its effect row), the row's look.
void ReplayWindow::DrawLivePlayerCastBar(int agentId, float centreX, float bottomY)
{
    const auto it = m_replayCtx.agents.find(agentId);
    if (it == m_replayCtx.agents.end()) return;
    constexpr float kW = 320.f, kH = 34.f, kGap = 8.f;
    DrawLiveCastRow(ImGui::GetBackgroundDrawList(), it->second, ImVec2(std::round(centreX - kW * 0.5f), bottomY - kGap - kH), kW,
                    "player cast");
}

// One cast row: the skill's icon, the game's cast bar (DrawGameCastBar: fill, outcome colour, fade) and
// the skill's name, as the replay's top bar draws it.
void ReplayWindow::DrawLiveCastRow(ImDrawList* dl, const AgentReplayData& ard, ImVec2 topLeft, float width, const char* who)
{
    const AgentReplayData::SkillVisual sv = ard.skillVisualAtTime(m_debugTimeline);
    if (sv.skillId <= 0 || sv.alpha <= 0.f)
    {
        if (m_liveReportHud) m_liveHudReport += std::format("{} none; ", who);
        return;
    }
    constexpr float kIcon = 34.f, kGap = 5.f, kBarH = 18.f;
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    const uint8_t alpha = static_cast<uint8_t>(sv.alpha * 255.f);
    EnsureSkillIconIndex();
    if (ImTextureID tex = LoadSkillIcon(this, dev, sv.skillId, m_skillIconIndex, m_skillIconCache))
        dl->AddImage(tex, topLeft, ImVec2(topLeft.x + kIcon, topLeft.y + kIcon), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, alpha));
    const ImVec2 b0(topLeft.x + kIcon + kGap, topLeft.y + (kIcon - kBarH) * 0.5f);
    const ImVec2 b1(topLeft.x + width, b0.y + kBarH);
    DrawGameCastBar(dl, b0, b1, sv);
    const SkillInfo* si = m_skillView.Get(sv.skillId);
    if (si && !si->name.empty())
    {
        ImFont* font = ImGui::GetFont();
        constexpr float fs = 11.f;
        const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, si->name.c_str());
        const ImVec2 tp(b0.x + (b1.x - b0.x - ts.x) * 0.5f, b0.y + (kBarH - ts.y) * 0.5f);
        dl->AddText(font, fs, ImVec2(tp.x + 1.f, tp.y + 1.f), IM_COL32(0, 0, 0, alpha), si->name.c_str());
        dl->AddText(font, fs, tp, IM_COL32(0xFF, 0xFF, 0xFF, alpha), si->name.c_str());
    }
    if (m_liveReportHud)
        m_liveHudReport += std::format("{} agent {} skill {} ({}) progress {:.2f} alpha {:.2f} {} at ({:.0f}, {:.0f}); ", who,
                                       ard.agent_id, sv.skillId, si ? si->name : std::string("?"), sv.progress, sv.alpha,
                                       sv.isCasting ? "casting" : "done", b0.x, b0.y);
}

// The sound path's state, reported once and on every change (the self-test reads it: a live match
// has no recording, so every sound is the plan's).
void ReplayWindow::LiveReportAudio()
{
    if (!m_live) return;
    const std::string flags = std::format(
        "audio: initialized {}, engine {}, enabled {}, timeline {} (skill uses {}, boss {}, bundles {})",
        m_audioInitialized ? "yes" : "no", m_audioEngine ? "yes" : "no", m_audioEnabled ? "yes" : "no",
        m_audioSkillTimelineBuilt ? "yes" : "no", m_skillUseTimelineBuilt ? "yes" : "no", m_bossTimelineBuilt ? "yes" : "no",
        m_bundleCarryBuilt ? "yes" : "no");
    // The plan's counters move every cast: a change of the flags is what is worth a line.
    if (flags == m_liveAudioFlags) return;
    m_liveAudioFlags = flags;
    const std::string line = flags + ", plan " + LiveSoundState();
    RunLog::Line("live: %s", line.c_str());
    m_live->Report(line);
}

// THE CLICK-TO-MOVE MARKER. Gw.exe: the ground click (0x534550) calls AvSelect.cpp 0x7E4D70, which
// destroys the previous marker, snaps the point to the ground (0x7E36C0 -> the map height 0x708720, no
// offset) and creates file 9097 (s_targetCursor, 0xA951D4) through AvGhost 0x7E8B10 as a FREE model:
// scale 1, the world's axes (0xBFBC78 / 0xBFBC6C), not the camera's or the player's; its clip's first
// segment (0xD2AA1677, 0.733 s) played once (mode 6); the ghost's timer ends it with the clip (0x7E9280,
// no emitters), the next click sooner. No tint or fade from AvSelect: the file's own cut-out layer
// (blend state 8, texture 31186) and its three bones' rotation keys (each chevron turns 135 degrees
// about its hinge) are the whole look. Here the effect player plays that file, at that point, with
// that clip (LiveSpawnGroundMarker, private); this function only reports it for the self-test.
void ReplayWindow::LiveReportGroundMarker()
{
    if (!m_liveMarkerReport || !m_live) return;
    const std::string state = LiveGroundMarkerState();
    m_live->Report("marker: " + state);
    if (state.rfind("none", 0) == 0) m_liveMarkerReport = false;
}

// -------------------------------------------------------------------------------------------------
// THE PLAYER'S CONTROLS: click to move, the skill bar, the skills window (Trials).
// -------------------------------------------------------------------------------------------------
bool ReplayWindow::LivePlayerBar(int agentId, int (&bar)[8]) const
{
    if (!m_liveMode || !m_live || agentId != m_live->PlayerAgentId()) return false;
    return m_live->PlayerBar(bar);
}

// The ground under the cursor, game (x, y): the camera ray marched against the terrain, then halved
// down to a tenth of a unit. World (x, z) is game (x, y) under the default map transform, the one the
// live seed places agents with.
bool ReplayWindow::LivePickGround(float mouseX, float mouseY, float& gameX, float& gameY) const
{
    if (!m_mapRenderer || !m_deviceResources) return false;
    Camera* cam = m_mapRenderer->GetCamera();
    const D3D11_VIEWPORT vp = m_deviceResources->GetScreenViewport();
    if (!cam || vp.Width <= 0.f || vp.Height <= 0.f) return false;
    const XMMATRIX inv = XMMatrixInverse(nullptr, cam->GetView() * cam->GetProj());
    const float ndcX = (mouseX - vp.TopLeftX) / vp.Width * 2.f - 1.f;
    const float ndcY = 1.f - (mouseY - vp.TopLeftY) / vp.Height * 2.f;
    const XMVECTOR a = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.1f, 1.f), inv);
    const XMVECTOR b = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.9f, 1.f), inv);
    XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(b, a));
    if (XMVectorGetX(XMVector3Dot(dir, cam->GetLook())) < 0.f) dir = XMVectorNegate(dir);
    const XMVECTOR origin = cam->GetPosition();
    auto above = [&](float t, float& x, float& z) {
        const XMVECTOR p = XMVectorAdd(origin, XMVectorScale(dir, t));
        x = XMVectorGetX(p);
        z = XMVectorGetZ(p);
        float h = 0.f;
        if (!m_mapRenderer->TryGetGroundHeight(x, z, h)) return true;
        return XMVectorGetY(p) > h;
    };
    float x = 0.f, z = 0.f;
    if (!above(0.f, x, z)) return false;   // the camera itself is under the ground
    float lo = 0.f, hi = 0.f, step = 16.f;
    bool hit = false;
    while (hi < 40000.f)
    {
        lo = hi;
        hi += step;
        step = std::min(step * 1.03f, 96.f);
        if (!above(hi, x, z)) { hit = true; break; }
    }
    if (!hit) return false;
    for (int i = 0; i < 20 && hi - lo > 0.1f; ++i)
    {
        const float mid = 0.5f * (lo + hi);
        if (above(mid, x, z)) lo = mid; else hi = mid;
    }
    above(hi, x, z);
    gameX = x;
    gameY = z;
    return true;
}

// A left click on the ground, not on a character, a panel or a drag: the point to walk to. While
// the button stays down the point under the cursor goes along each frame (the client steers so).
void ReplayWindow::LiveGroundInput()
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool free = !io.WantCaptureMouse && !m_rightMouseDown && m_hoveredAgentId < 0 &&
                      !m_annotationMgr.IsDrawModeActive() && !ImGui::GetDragDropPayload();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_live && m_liveMouseNext < m_liveMouseSteps.size())
    {
        // The self-test's click: what the window saw, so a refused click says why.
        float gx = 0.f, gy = 0.f;
        const bool picked = LivePickGround(io.MousePos.x, io.MousePos.y, gx, gy);
        m_live->Report(std::format("ui: scripted click at ({:.0f}, {:.0f}): panel {}, right button {}, character {}, "
                                   "drawing {}, drag {}, ground {}",
                                   io.MousePos.x, io.MousePos.y, io.WantCaptureMouse ? "yes" : "no", m_rightMouseDown ? "yes" : "no",
                                   m_hoveredAgentId, m_annotationMgr.IsDrawModeActive() ? "yes" : "no",
                                   ImGui::GetDragDropPayload() ? "yes" : "no",
                                   picked ? std::format("({:.0f}, {:.0f})", gx, gy) : std::string("none")));
    }
    if (free && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        float gx = 0.f, gy = 0.f;
        if (LivePickGround(io.MousePos.x, io.MousePos.y, gx, gy))
        {
            m_liveGroundClicked = true;
            m_liveGroundX = gx;
            m_liveGroundY = gy;
            m_liveGroundHeld = true;
            // The game's marker, at the clicked point (a held button moves the walk, not the marker).
            m_liveMarkerX = gx;
            m_liveMarkerY = gy;
            LiveSpawnGroundMarker(gx, gy);
        }
    }
    if (m_liveGroundHeld && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_liveGroundHeld = false;
    m_liveHeldValid = false;
    if (m_liveGroundHeld && !m_liveGroundClicked)
        m_liveHeldValid = LivePickGround(io.MousePos.x, io.MousePos.y, m_liveHeldX, m_liveHeldY);
}

// One slot of the player's bar, inside the focus HUD's bar window. Closed skills window: the press
// uses the skill, as the client does. Open: a press and release uses it, a drag carries it to
// another slot (the two swap) or off the bar (cleared), and the slot takes a skill dropped on it.
void ReplayWindow::LiveSkillSlotInput(int slot, int skill, ImVec2 tl, ImVec2 br)
{
    if (slot < 0 || slot >= 8 || !m_live) return;
    m_liveSlotRects[slot] = ImVec4(tl.x, tl.y, br.x, br.y);
    ImGui::PushID("trials_slot");
    ImGui::PushID(slot);
    ImGui::SetCursorScreenPos(tl);
    ImGui::InvisibleButton("##slot", ImVec2(br.x - tl.x, br.y - tl.y));
    if (skill > 0)
    {
        if (ImGui::IsItemActivated())
        {
            m_liveSlotDragged = false;
            if (!m_liveSkillsOpen) m_liveClickedSlot = slot;
        }
        if (m_liveSkillsOpen && ImGui::BeginDragDropSource())
        {
            const int payload[2] = { skill, slot };
            ImGui::SetDragDropPayload(kTrialsSkillPayload, payload, sizeof(payload));
            ID3D11Device* dev = m_deviceResources->GetD3DDevice();
            if (ImTextureID tex = LoadSkillIcon(this, dev, skill, m_skillIconIndex, m_skillIconCache))
                ImGui::Image(tex, ImVec2(40.f, 40.f), ImVec2(0.0625f, 0.0625f), ImVec2(0.9375f, 0.9375f));
            const SkillInfo* si = m_skillView.Get(skill);
            ImGui::TextUnformatted(si ? si->name.c_str() : "skill");
            ImGui::EndDragDropSource();
            m_liveDragFromSlot = slot;
            m_liveSlotDragged = true;
        }
        if (m_liveSkillsOpen && ImGui::IsItemDeactivated() && ImGui::IsItemHovered() && !m_liveSlotDragged)
            m_liveClickedSlot = slot;
    }
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kTrialsSkillPayload))
        {
            int data[2] = {};
            std::memcpy(data, p->Data, sizeof(data));
            int bar[8] = {};
            if (m_live->PlayerBar(bar))
            {
                int next[8];
                for (int i = 0; i < 8; ++i) next[i] = bar[i];
                if (data[1] >= 0 && data[1] < 8)
                    std::swap(next[data[1]], next[slot]);
                else
                {
                    // From the list: a skill already on the bar moves here, the one here goes there.
                    for (int i = 0; i < 8; ++i)
                        if (i != slot && next[i] == data[0]) next[i] = next[slot];
                    next[slot] = data[0];
                }
                for (int i = 0; i < 8; ++i) m_liveNewBar[i] = next[i];
                m_liveSetBar = true;
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
    ImGui::PopID();
}

// THE SKILLS WINDOW (the client's K): the skills of the player's professions by attribute, with
// a search box. A simulated skill drags onto the bar; one the simulation cannot run yet is greyed
// and does not drag. The game's window frame, as the party window draws it.
void ReplayWindow::DrawLiveSkillsWindow()
{
    static std::vector<LiveSkillOption> s_options;
    static bool s_loaded = false;
    if (!m_liveSkillsOpen)
    {
        m_liveListRects.clear();
        s_loaded = false;
        return;
    }
    if (!s_loaded)
    {
        m_live->SkillOptions(s_options);
        s_loaded = true;
    }
    int bar[8] = {};
    const bool haveBar = m_live->PlayerBar(bar);
    const bool casting = m_live->PlayerCasting();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    EnsureSkillIconIndex();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float winW = 400.f, winH = std::min(600.f, vp->Size.y - 140.f);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f + 140.f, vp->Pos.y + 70.f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Appearing);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.89f, 0.71f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f, 14.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    bool open = true;
    if (ImGui::Begin("Skills##trials_skills", &open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar))
    {
        // The frame: dark body, the team frame art, the title bar with its close button.
        const ImVec2 A0 = ImGui::GetWindowPos();
        const ImVec2 A1(A0.x + ImGui::GetWindowSize().x, A0.y + ImGui::GetWindowSize().y);
        ImDrawList* fdl = ImGui::GetWindowDrawList();
        fdl->PushClipRect(A0, A1, false);
        fdl->AddRectFilled(ImVec2(A0.x + 10.f, A0.y + 32.f), ImVec2(A1.x - 10.f, A1.y - 14.f), IM_COL32(14, 14, 18, 0xEE));
        DrawGameNineSlice(fdl, LoadGameUITexture(dev, "Window\\ui_window_frame_team.png"), ImVec2(A0.x, A0.y + 32.f), A1, 128.f, 128.f);
        DrawGameNineSlice(fdl, LoadGameUITexture(dev, "Window\\ui_window_titlebar.png"), A0, ImVec2(A1.x, A0.y + 32.f), 128.f, 32.f);
        const float bandY = A0.y + 12.f;
        fdl->AddText(ImVec2(A0.x + 19.f, bandY + (20.f - ImGui::GetFontSize()) * 0.5f), IM_COL32(0xDC, 0xDC, 0xDC, 0xFF), "Skills");
        const ImVec2 x0(A1.x - 3.f - 24.f, bandY + 2.f), x1(x0.x + 16.f, x0.y + 16.f);
        const ImGuiIO& io = ImGui::GetIO();
        const bool xHover = ImGui::IsWindowHovered() && io.MousePos.x >= x0.x && io.MousePos.x < x1.x &&
                            io.MousePos.y >= x0.y && io.MousePos.y < x1.y;
        if (ImTextureID btn = LoadGameUITexture(dev, "Window\\ui_window_buttons.png"))
        {
            const int cell = xHover ? (io.MouseDown[0] ? 2 : 1) : 0;
            fdl->AddImage(btn, x0, x1, ImVec2(cell * 16.f / 128.f, 0.f), ImVec2((cell + 1) * 16.f / 128.f, 1.f));
        }
        if (xHover && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) open = false;
        fdl->PopClipRect();

        ImGui::SetCursorPosY(40.f);
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint("##trials_skill_search", "Search skills", m_liveSkillSearch, sizeof(m_liveSkillSearch));
        ImGui::TextDisabled(casting ? "Casting: the bar cannot change now" : "Drag a skill onto the bar. Grey: not simulated yet");

        // The list, by attribute; a row drops back a dragged slot (cleared).
        std::string needle(m_liveSkillSearch);
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::map<int, std::vector<const SkillInfo*>> byAttribute;
        for (const LiveSkillOption& o : s_options)
        {
            const SkillInfo* si = m_skillView.Get(o.skill);
            if (!si) continue;
            if (!needle.empty())
            {
                std::string name = si->name;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name.find(needle) == std::string::npos) continue;
            }
            byAttribute[si->attribute].push_back(si);
        }
        m_liveListRects.clear();
        ImGui::BeginChild("##trials_skill_list", ImVec2(0.f, 0.f), false);
        const float rowH = 36.f;
        for (auto& [attribute, skills] : byAttribute)
        {
            std::sort(skills.begin(), skills.end(), [](const SkillInfo* a, const SkillInfo* b) { return a->name < b->name; });
            const char* attrName = SkillDatabase::GetAttributeName(attribute);
            ImGui::SeparatorText(attrName && *attrName ? attrName : "No Attribute");
            for (const SkillInfo* si : skills)
            {
                const bool simulated = std::any_of(s_options.begin(), s_options.end(),
                                                   [&](const LiveSkillOption& o) { return o.skill == si->id && o.simulated; });
                bool onBar = false;
                for (int i = 0; i < 8 && haveBar; ++i) onBar = onBar || bar[i] == si->id;
                const ImVec2 r0 = ImGui::GetCursorScreenPos();
                const ImVec2 r1(r0.x + ImGui::GetContentRegionAvail().x, r0.y + rowH);
                ImGui::PushID(si->id);
                ImGui::InvisibleButton("##row", ImVec2(r1.x - r0.x, rowH));
                const bool hovered = ImGui::IsItemHovered();
                m_liveListRects[si->id] = ImVec4(r0.x, r0.y, r1.x, r1.y);
                if (simulated && ImGui::BeginDragDropSource())
                {
                    const int payload[2] = { si->id, -1 };
                    ImGui::SetDragDropPayload(kTrialsSkillPayload, payload, sizeof(payload));
                    if (ImTextureID tex = LoadSkillIcon(this, dev, si->id, m_skillIconIndex, m_skillIconCache))
                        ImGui::Image(tex, ImVec2(40.f, 40.f), ImVec2(0.0625f, 0.0625f), ImVec2(0.9375f, 0.9375f));
                    ImGui::TextUnformatted(si->name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kTrialsSkillPayload))
                    {
                        int data[2] = {};
                        std::memcpy(data, p->Data, sizeof(data));
                        if (data[1] >= 0 && data[1] < 8 && haveBar)
                        {
                            for (int i = 0; i < 8; ++i) m_liveNewBar[i] = bar[i];
                            m_liveNewBar[data[1]] = 0;
                            m_liveSetBar = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::PopID();

                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (hovered) dl->AddRectFilled(r0, r1, IM_COL32(255, 255, 255, 18), 3.f);
                const ImVec2 i0(r0.x + 2.f, r0.y + 2.f), i1(i0.x + rowH - 4.f, i0.y + rowH - 4.f);
                const ImU32 tint = simulated ? IM_COL32_WHITE : IM_COL32(110, 110, 110, 150);
                if (ImTextureID tex = LoadSkillIcon(this, dev, si->id, m_skillIconIndex, m_skillIconCache))
                    dl->AddImage(tex, i0, i1, ImVec2(0.0625f, 0.0625f), ImVec2(0.9375f, 0.9375f), tint);
                if (si->is_elite) dl->AddRect(i0, i1, IM_COL32(0xE8, 0xC0, 0x40, simulated ? 0xFF : 0x70), 0.f, 0, 1.5f);
                if (onBar) dl->AddRect(ImVec2(i0.x - 1.f, i0.y - 1.f), ImVec2(i1.x + 1.f, i1.y + 1.f), IM_COL32(0x70, 0xD0, 0x70, 0xFF), 0.f, 0, 2.f);
                const ImU32 text = simulated ? IM_COL32(0xF4, 0xE3, 0xB5, 0xFF) : IM_COL32(0x80, 0x80, 0x80, 0xFF);
                dl->AddText(ImVec2(i1.x + 8.f, r0.y + (rowH - ImGui::GetFontSize()) * 0.5f), text, si->name.c_str());
                if (!simulated)
                    dl->AddText(ImVec2(r1.x - ImGui::CalcTextSize("not simulated").x - 6.f, r0.y + (rowH - ImGui::GetFontSize()) * 0.5f),
                                IM_COL32(0x90, 0x70, 0x60, 0xFF), "not simulated");
                if (hovered && !ImGui::GetDragDropPayload())
                {
                    if (simulated)
                        DrawGameSkillTooltip(si->id, m_live->PlayerAgentId(), nullptr);
                    else
                        ImGui::SetTooltip("%s: not simulated yet", si->name.c_str());
                }
            }
        }
        if (byAttribute.empty()) ImGui::TextDisabled("No skill matches");
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!open)
    {
        m_liveSkillsOpen = false;
        m_live->Report("ui: skills window closed");
    }
}

// THE SELF-TEST'S MOUSE. The source names a click or a drag; the window turns it into mouse steps,
// one a frame, resolved against what it drew the frame before (a slot, a row of the skills window,
// a point of the ground projected onto the screen) and fed into this window's own ImGui context
// before its NewFrame. No OS input is read or sent; the real cursor never moves.
void ReplayWindow::LiveTakeScriptedUi()
{
    if (!m_live || m_liveMouseNext < m_liveMouseSteps.size()) return;
    const LiveScriptedUi ui = m_live->TakeScriptedUi();
    if (ui.kind == LiveScriptedUi::Kind::None) return;
    m_liveMouseSteps.clear();
    m_liveMouseNext = 0;
    auto at = [&](int ref, int id, float x, float y, float lerp) { m_liveMouseSteps.push_back({ ref, id, x, y, lerp, -1 }); };
    auto button = [&](bool down) { m_liveMouseSteps.push_back({ -1, 0, 0.f, 0.f, 1.f, down ? 1 : 0 }); };
    auto wait = [&] { m_liveMouseSteps.push_back({ -1, 0, 0.f, 0.f, 1.f, -1 }); };
    switch (ui.kind)
    {
    case LiveScriptedUi::Kind::GroundClick:
    {
        float h = 0.f, sx = 0.f, sy = 0.f;
        const D3D11_VIEWPORT vp = m_deviceResources->GetScreenViewport();
        Camera* cam = m_mapRenderer->GetCamera();
        const bool onScreen = m_mapRenderer->TryGetGroundHeight(ui.x, ui.y, h) &&
                              ProjectToScreen(cam->GetView() * cam->GetProj(), vp.Width, vp.Height, XMFLOAT3(ui.x, h, ui.y), sx, sy);
        if (!onScreen)
        {
            m_live->Report(std::format("ui: ground click at ({:.0f}, {:.0f}) is not on the screen", ui.x, ui.y));
            return;
        }
        at(0, 0, sx, sy, 1.f);
        button(true);
        button(false);
        m_liveScriptNote = std::format("ui: ground click at ({:.0f}, {:.0f}) -> screen ({:.0f}, {:.0f})", ui.x, ui.y, sx, sy);
        break;
    }
    case LiveScriptedUi::Kind::ClickBody:
    {
        // The middle of the body: 60 units over the feet (a character is about 120 high).
        float h = 0.f, sx = 0.f, sy = 0.f;
        const D3D11_VIEWPORT vp = m_deviceResources->GetScreenViewport();
        Camera* cam = m_mapRenderer->GetCamera();
        const bool onScreen = m_mapRenderer->TryGetGroundHeight(ui.x, ui.y, h) &&
                              ProjectToScreen(cam->GetView() * cam->GetProj(), vp.Width, vp.Height, XMFLOAT3(ui.x, h + 60.f, ui.y), sx, sy);
        if (!onScreen)
        {
            m_live->Report(std::format("ui: body click at ({:.0f}, {:.0f}) is not on the screen", ui.x, ui.y));
            return;
        }
        at(0, 0, sx, sy, 1.f);
        button(true);
        button(false);
        m_liveScriptNote = std::format("ui: body click at ({:.0f}, {:.0f}) -> screen ({:.0f}, {:.0f})", ui.x, ui.y, sx, sy);
        break;
    }
    case LiveScriptedUi::Kind::ReportOverheads:
        m_liveReportOverheads = true;
        return;
    case LiveScriptedUi::Kind::ReportHud:
        m_liveReportHud = true;
        m_liveHudReport.clear();
        return;
    case LiveScriptedUi::Kind::ReportMarker:
        m_liveMarkerReport = true;
        return;
    case LiveScriptedUi::Kind::SkillSlotClick:
        at(2, ui.slot, 0.f, 0.f, 1.f);
        button(true);
        button(false);
        m_liveScriptNote = std::format("ui: skill bar click on slot {}", ui.slot + 1);
        break;
    case LiveScriptedUi::Kind::DragSkillToSlot:
    {
        m_liveSkillsOpen = true;
        const SkillInfo* si = m_skillView.Get(ui.skill);
        snprintf(m_liveSkillSearch, sizeof(m_liveSkillSearch), "%s", si ? si->name.c_str() : "");
        wait();
        wait();
        wait();
        at(1, ui.skill, 0.f, 0.f, 1.f);
        button(true);
        for (float f : { 0.2f, 0.25f, 0.333f, 0.5f, 1.f }) at(2, ui.slot, 0.f, 0.f, f);
        at(2, ui.slot, 0.f, 0.f, 1.f);
        button(false);
        wait();
        m_liveScriptNote = std::format("ui: dragged {} from the skills window to slot {}", si ? si->name : std::string("?"), ui.slot + 1);
        break;
    }
    default:
        break;
    }
}

void ReplayWindow::LiveFeedScriptedMouse()
{
    if (m_liveMouseNext >= m_liveMouseSteps.size()) return;
    const LiveMouseStep s = m_liveMouseSteps[m_liveMouseNext++];
    ImGuiIO& io = ImGui::GetIO();
    if (s.ref >= 0)
    {
        float tx = s.x, ty = s.y;
        bool known = true;
        if (s.ref == 1)
        {
            const auto it = m_liveListRects.find(s.id);
            known = it != m_liveListRects.end();
            if (known)
            {
                tx = it->second.x + 20.f;
                ty = 0.5f * (it->second.y + it->second.w);
            }
        }
        else if (s.ref == 2 && s.id >= 0 && s.id < 8)
        {
            const ImVec4& r = m_liveSlotRects[s.id];
            known = r.z > r.x;
            tx = 0.5f * (r.x + r.z);
            ty = 0.5f * (r.y + r.w);
        }
        if (!known && m_live)
            m_live->Report(std::format("ui: scripted mouse target {} {} not drawn", s.ref == 1 ? "row" : "slot", s.id));
        m_liveMouseX += (tx - m_liveMouseX) * s.lerp;
        m_liveMouseY += (ty - m_liveMouseY) * s.lerp;
        io.AddMousePosEvent(m_liveMouseX, m_liveMouseY);
    }
    if (s.button >= 0) io.AddMouseButtonEvent(ImGuiMouseButton_Left, s.button == 1);
    if (m_liveMouseNext >= m_liveMouseSteps.size())
    {
        if (m_live && !m_liveScriptNote.empty()) m_live->Report(m_liveScriptNote);
        // A drag leaves the list whole again for the next picture.
        if (m_liveScriptNote.rfind("ui: dragged", 0) == 0) m_liveSkillSearch[0] = '\0';
        m_liveScriptNote.clear();
        m_liveMouseSteps.clear();
        m_liveMouseNext = 0;
    }
}

void ReplayWindow::LiveAfterRender()
{
    if (!m_live) return;
    const std::wstring path = m_live->TakeCaptureRequest();
    if (path.empty() || !m_deviceResources) return;
    DirectX::ScratchImage shot;
    if (FAILED(DirectX::CaptureTexture(m_deviceResources->GetD3DDevice(), m_deviceResources->GetD3DDeviceContext(),
                                       m_deviceResources->GetRenderTarget(), shot)))
        return;
    // The swap chain's alpha is not meant to be read: write the frame opaque.
    const DirectX::Image* img = shot.GetImage(0, 0, 0);
    if (img && img->pixels && (img->format == DXGI_FORMAT_R8G8B8A8_UNORM || img->format == DXGI_FORMAT_B8G8R8A8_UNORM))
        for (size_t y = 0; y < img->height; ++y)
            for (size_t x = 0; x < img->width; ++x)
                img->pixels[y * img->rowPitch + x * 4 + 3] = 255;
    DirectX::SaveToWICFile(*img, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), path.c_str());
}

void ReplayWindow::LiveReportModels()
{
    m_liveModelsReported = true;
    auto report = [&](const std::string& line) {
        RunLog::Line("live: %s", line.c_str());
        if (m_live) m_live->Report(line);
    };
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        const auto hashIt = m_agentFileHashCache.find(id);
        const uint32_t hash = hashIt != m_agentFileHashCache.end() ? hashIt->second : 0;
        const auto animIt = m_agentAnimStates.find(id);
        const bool skinned = animIt != m_agentAnimStates.end() && animIt->second.hasSkinning;
        report(std::format("agent {} '{}' type {} model 0x{:X} file 0x{:X} {}", id, ard.cachedLabel,
                           AgentTypeName(ard.type), ard.modelId, hash, skinned ? "skinned" : "NOT skinned"));
    }

    const auto tmplIt = m_agentModelTemplates.find(kTrialsMannequinFileHash);
    if (tmplIt == m_agentModelTemplates.end())
    {
        report("mannequin 0x29966: no template was built");
        return;
    }
    const AgentModelInstance& tmpl = tmplIt->second;
    report(std::format("mannequin 0x29966: {} clip(s), animation {}, skeleton {}, native height {:.1f}",
                       tmpl.allClips.size(), tmpl.hasAnimation ? "yes" : "no", tmpl.skeleton ? "yes" : "no",
                       tmpl.nativeHeight));
    for (int ci : tmpl.clipOrder)
    {
        if (ci < 0 || ci >= static_cast<int>(tmpl.allClips.size()) || !tmpl.allClips[ci].clip) continue;
        const auto& segs = tmpl.allClips[ci].clip->animationSegments;
        report(std::format("mannequin clip {} (file 0x{:X}): {} segment(s)", ci, tmpl.allClips[ci].sourceFileHash,
                           segs.size()));
        for (size_t si = 0; si < segs.size(); ++si)
            report(std::format("  [{}] 0x{:08X} {}", si, segs[si].hash,
                               GW::Animation::GetAnimationCategorizedName(segs[si].hash)));
    }
    // The rows the arena plays on it, each by its AvChar base under any stance letter, hashed the
    // client's way (Gw.exe 0x7F2230: base * 23 + letter index + 0xE0000000, as ResolveHitFlinch does):
    // idle (row 2), hit flinch (row 53), death (row 61), knockdown (row 56). Which segment answers.
    auto resolves = [&](uint32_t base) -> std::string {
        for (char c : GW::Animation::g_boneSlotChars)
        {
            const uint32_t letter = static_cast<uint32_t>(c) - 0x61u -
                                    static_cast<uint32_t>(GW::Animation::ComputeBoneSlotAdjustment(c));
            const uint32_t wanted = base * 23u + letter + 0xE0000000u;
            for (int ci : tmpl.clipOrder)
            {
                if (ci < 0 || ci >= static_cast<int>(tmpl.allClips.size()) || !tmpl.allClips[ci].clip) continue;
                const auto& segs = tmpl.allClips[ci].clip->animationSegments;
                for (size_t si = 0; si < segs.size(); ++si)
                    if (segs[si].hash == wanted) return std::format("0x{:08X} @{} ('{}')", wanted, si, c);
            }
        }
        return "no";
    };
    report(std::format("mannequin resolves: idle {}, hit {}, death {}, knockdown {}", resolves(0x33E46F23u),
                       resolves(0x338019B5u), resolves(0x30213BC4u), resolves(0x3001EFF5u)));
}

void ReplayWindow::LiveCheckAnimCode(int agentId, const AgentModelInstance& tmpl, uint32_t code, uint32_t id,
                                     bool resolved, const SegmentRef& ref)
{
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(agentId)) << 48) ^
                         (static_cast<uint64_t>(id & 0xFFFFu) << 32) ^ code;
    if (!m_live || !m_liveAnimChecked.insert(key).second) return;

    // Where it plays from, and whether that segment has keys of its own (an external one does not).
    std::string plays = "UNRESOLVED";
    bool playable = false;
    if (resolved && ref.clipIndex >= 0 && ref.clipIndex < static_cast<int>(tmpl.allClips.size()) &&
        tmpl.allClips[ref.clipIndex].clip)
    {
        const auto& clip = *tmpl.allClips[ref.clipIndex].clip;
        const bool inRange = ref.segmentIndex >= 0 && ref.segmentIndex < static_cast<int>(clip.animationSegments.size());
        playable = inRange && clip.GetSegmentSourceType(static_cast<size_t>(ref.segmentIndex)) == 0;
        plays = std::format("plays 0x{:X}[{}] {}", tmpl.allClips[ref.clipIndex].sourceFileHash, ref.segmentIndex,
                            playable ? "local" : "EXTERNAL (no keys)");
    }

    std::string verdict;
    if (tmpl.bankClipIndex >= 0 && tmpl.bankClipIndex < static_cast<int>(tmpl.allClips.size()))
    {
        // A pinned bank: the id must index this very code in the bank the recorded ids index.
        const auto& bankEntry = tmpl.allClips[tmpl.bankClipIndex];
        const auto& segs = bankEntry.clip->animationSegments;
        const bool match = id < segs.size() && segs[id].hash == code;
        verdict = std::format("bank 0x{:X}[{}] = {} {}", bankEntry.sourceFileHash, id,
                              id < segs.size() ? std::format("0x{:08X}", segs[id].hash) : std::string("out of range"),
                              match && playable ? "OK" : match ? "IN BANK BUT NOT PLAYABLE" : "MISMATCH");
    }
    else
    {
        // No pinned bank (the mannequin, an unpinned henchman): the code must reach a segment of
        // that hash with keys, through the model's own clips.
        bool hashOk = false;
        if (resolved && ref.clipIndex >= 0 && ref.clipIndex < static_cast<int>(tmpl.allClips.size()))
        {
            const auto& segs = tmpl.allClips[ref.clipIndex].clip->animationSegments;
            hashOk = ref.segmentIndex >= 0 && ref.segmentIndex < static_cast<int>(segs.size()) &&
                     segs[static_cast<size_t>(ref.segmentIndex)].hash == code;
        }
        verdict = std::format("no pinned bank, hash {} {}", hashOk ? "found" : "NOT found",
                              hashOk && playable ? "OK" : "NOT OK");
    }
    const std::string line = std::format("anim check: agent {} code 0x{:08X} id {} -> {}, {}", agentId, code, id,
                                         verdict, plays);
    RunLog::Line("live: %s", line.c_str());
    m_live->Report(line);
}
