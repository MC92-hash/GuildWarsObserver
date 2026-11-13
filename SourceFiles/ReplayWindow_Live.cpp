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
#include "AtexReader.h"
#include "CompassMapData.h"

#include <DirectXTex.h>
#include <d3dcompiler.h>
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
constexpr float kLiveCameraLookHeight = 70.f;    // ~0.92 of a character's height
// Read statically from Gw.exe (2026-11-04 client, GmCam.cpp). The camera keeps a yaw, a pitch and a
// distance, each with a TARGET (+0x5C, +0x60, +0x64) and a CURRENT value (+0x14, +0x18, +0x1C). Every
// frame the mode's update (0x4F0680) moves the current value toward the target by
// min(rate x dt, 1) of the difference (0x4F5ED0 for the angles, the short way round; 0x4F5FD0 for the
// distance): rate 6 / s standing (mode 0, 0x4F20A0; the rate re-approaches 6 at min(dt, 1) of the gap
// each frame) and 8 / s while the player moves with the keys (mode 5, 0x4F24F0, flag 4 from GmWalk
// 0x5340DC). Moving with the keys, 0.25 s after the last mouse look (context +0x48, cleared by every
// mouse event at 0x4F4720), the target becomes the character's facing (0x4F25E1 -> 0x4F73D0), and the
// camera swings in behind at 8 / s. The wheel (0x4F6470) writes the target and the current distance alike.
// THE MOUSE LOOK: the path read (0x4F6300 / 0x4F6400 -> 0x4F6180 -> 0x4F72C0 / 0x4F7350 with 1 as the
// second argument) writes the current yaw and pitch too, which would stop the camera with the mouse; the
// owner sees it coast on after a fast drag in the game, so that reading misses something (not found).
// ASSUMED, the model that gives that coast: the drag moves the TARGETS only and the yaw and pitch catch
// up at the mode's rate, 6 / s standing, 8 / s moving.
constexpr float kGwLookRadPerCount = 0.0013962634f;   // 0.08 degree a mouse count (0x945E70), x CameraRotationSpeed
constexpr float kGwPitchLimit = 0.9424779f;           // +-54 degrees on the pitch target (0x945E7C / 0x94D428)
constexpr float kGwRateStanding = 6.f;                // 1 / s (0x945280)
constexpr float kGwRateMoving = 8.f;                  // 1 / s (0x945790)
constexpr float kGwFollowAfterLook = 0.25f;           // s (0x94524C against context +0x48 and +0x54)
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

    // The self-test's right drag: the buttons as it holds them, its movement through the raw input's code.
    if (const LiveScriptedLook look = m_live->TakeScriptedLook(); look.active)
    {
        if (look.loseCapture)
            LiveCursorLost(true);
        else
        {
            if (look.right && !m_rightMouseDown) m_mouseDragOrigin = POINT{ look.pressX, look.pressY };
            if (look.right != m_rightMouseDown) LiveRightButton(look.right, true);
            m_rightMouseDown = look.right;
        }
        if (!look.loseCapture) m_liveLeftDown = look.left;
        if (look.right && (look.dx != 0.f || look.dy != 0.f)) LiveMouseLook(look.dx, look.dy);
    }
    m_liveSinceLook += std::max(dt, 0.f);

    LiveFrameInput in;
    in.realDt = dt;
    in.keyboardAllowed = processActive && !m_imguiWantTextInput && !m_clSkillSearchFocused;
    in.clickedAgentId = m_liveClickedAgent;
    in.cameraOrbiting = m_rightMouseDown;
    in.bothButtons = m_rightMouseDown && m_liveLeftDown;
    // The inverse of the chase camera's "behind" rule below.
    in.cameraFacing = std::atan2(-std::cos(m_followYaw), -std::sin(m_followYaw));
    in.cameraYaw = m_followYaw;
    in.cameraYawTarget = m_liveYawTarget;
    in.cameraRate = m_liveCameraRate;
    in.groundClicked = m_liveGroundClicked;
    in.groundX = m_liveGroundX;
    in.groundY = m_liveGroundY;
    // Both buttons walk forward instead (0x5346A0 drops the held walk).
    in.groundHeld = m_liveGroundHeld && m_liveHeldValid && !in.bothButtons;
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
    // user's (wheel, right drag). The yaw approaches its target as GmCam does (constants above): a
    // mouse look moves both, so it stops with the mouse; while the player moves or turns with the keys
    // or autorun, 0.25 s after the last mouse look, the target is "behind the heading" and the yaw swings
    // in at 8 / s, then at a rate going back to 6 / s once the keys are let go. A walk to a clicked point
    // (and the auto-attack's run into range) is not steering: the camera keeps its yaw and only follows
    // the position (the source says which).
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
            m_liveYawTarget = behind;
            m_livePitchTarget = kLiveCameraPitch;
            m_liveCameraPlaced = true;
        }
    }
    m_followCenterBias = DirectX::XMFLOAT3(0.f, kLiveCameraLookHeight, 0.f);
    const bool moving = m_live->PlayerSteering();
    if (moving && m_liveSinceLook >= kGwFollowAfterLook)
        m_liveYawTarget = behind;
    // The rate: 8 while moving; standing, 6 approached by min(dt, 1) of the gap a frame (0x4F076C).
    if (moving)
        m_liveCameraRate = kGwRateMoving;
    else
        m_liveCameraRate += (kGwRateStanding - m_liveCameraRate) * std::clamp(dt, 0.f, 1.f);
    const float k = std::clamp(m_liveCameraRate * std::max(dt, 0.f), 0.f, 1.f);
    m_followYaw = WrapAngle(m_followYaw + WrapAngle(m_liveYawTarget - m_followYaw) * k);
    m_followPitch += (m_livePitchTarget - m_followPitch) * k;
}

// THE CURSOR DURING MOUSE LOOK (live mode). The window's right-button handler already holds the cursor
// in place (ClipCursor to the press point, raw input for the movement, as the left-drag pan does); here
// it is hidden while the button is down and put back at the press point on release, as the game shows it.
void ReplayWindow::LiveRightButton(bool down, bool scripted)
{
    // Hide once, show once: ShowCursor is a counter, so every hide is paired with exactly one show.
    if (scripted)
    {
        if (down && !m_liveScriptCursorHidden)
        {
            m_liveScriptCursorHidden = true;
            --m_liveScriptCursorCount;
            if (m_live)
                m_live->Report(std::format("cursor: would hide at ({}, {}), count {}", m_mouseDragOrigin.x, m_mouseDragOrigin.y,
                                           m_liveScriptCursorCount));
        }
        else if (!down && m_liveScriptCursorHidden)
        {
            m_liveScriptCursorHidden = false;
            ++m_liveScriptCursorCount;
            if (m_live)
                m_live->Report(std::format("cursor: would restore to ({}, {}), count {}", m_mouseDragOrigin.x, m_mouseDragOrigin.y,
                                           m_liveScriptCursorCount));
        }
        return;
    }
    if (down && !m_liveCursorHidden)
    {
        ShowCursor(FALSE);
        m_liveCursorHidden = true;
    }
    else if (!down && m_liveCursorHidden)
    {
        SetCursorPos(m_mouseDragOrigin.x, m_mouseDragOrigin.y);
        ShowCursor(TRUE);
        m_liveCursorHidden = false;
    }
}

void ReplayWindow::LiveCursorLost(bool scripted)
{
    const bool hidden = scripted ? m_liveScriptCursorHidden : m_liveCursorHidden;
    if (!m_rightMouseDown && !hidden)
        return;
    if (scripted && m_live && hidden)
        m_live->Report("cursor: capture lost during a right drag");
    m_rightMouseDown = false;
    m_liveLeftDown = false;
    if (!scripted && !m_leftMouseDown)
        ClipCursor(nullptr);
    LiveRightButton(false, scripted);
}

// A right drag in live mode, in mouse counts: GW's mouse look (GmCam 0x4F6180): 0.08 degree a count times
// CameraRotationSpeed (the Trials setting "Camera rotation speed", 25..175 %, preference 0x24 / 100 in the
// game; not the replay's rotation multiplier), the pitch target kept within +-54 degrees. It
// moves the targets; LiveUpdate brings the yaw and pitch after them (ASSUMED, see above). The raw mouse input
// and the self-test's scripted drag both come here.
void ReplayWindow::LiveMouseLook(float dx, float dy)
{
    const auto& hk = ReplayHotkeys::Get();
    const float speed = static_cast<float>(std::clamp(hk.trCameraRotation, 25, 175)) / 100.f;
    if (!m_liveLookSpeedReported && m_live)
    {
        m_liveLookSpeedReported = true;
        m_live->Report(std::format("camera: rotation speed {} %", std::clamp(hk.trCameraRotation, 25, 175)));
    }
    const float radX = dx * kGwLookRadPerCount * speed * (hk.invertMouseX ? -1.f : 1.f);
    const float radY = dy * kGwLookRadPerCount * speed * (hk.invertMouseY ? -1.f : 1.f);
    m_liveYawTarget = WrapAngle(m_liveYawTarget + radX);
    m_livePitchTarget = std::clamp(m_livePitchTarget + radY, std::max(kFollowMinPitch, -kGwPitchLimit), kGwPitchLimit);
    m_liveSinceLook = 0.f;
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
    // The effect bar, the party strips and every reader of the effect table: the source's own effects
    // (EffectHistory), and the channels a recording would read them from.
    if (a.effects || a.visuals)
        m_effectTimelineBuilt = false;
    // The private schedules: what the effects and the sounds play, rebuilt without replaying what
    // is already past.
    if (a.skills || a.combat || a.visuals)
    {
        LiveRebuildEffectSchedules();
        LiveRebuildSoundPlan();
    }
}

// THE ENERGY BARS. A recording never states a player's energy, so the replay calculates it forward
// (EnergyModel) into a Track of samples that Track::At reads: linear between two samples while the change
// is a slope (regeneration, at most 10 pips over one step), a cut to the next sample when it is a jump
// (a cost, a gain). A live match has the energy itself: the source's value at every tick, regeneration
// accrued fractionally and costs paid whole. Those samples fill the same Track, one a tick, so every bar
// that reads EnergyTrackFor (the focus HUD, the party strips, the skill tooltips) draws them with the
// replay's rules; the display runs one tick behind the newest sample, so there is always one after it.
const EnergyModel::Track* ReplayWindow::LiveEnergyTrackFor(int agentId) const
{
    if (!m_live) return nullptr;
    EnergyModel::Track& track = m_liveEnergyTracks[agentId];
    std::vector<LiveEnergySample> fresh;
    double tick = 0.0;
    if (!m_live->EnergySamples(agentId, track.samples.size(), fresh, tick) || tick <= 0.0)
    {
        if (track.samples.empty()) m_liveEnergyTracks.erase(agentId);
        return nullptr;
    }
    track.dt = static_cast<float>(tick);
    for (const LiveEnergySample& e : fresh)
    {
        EnergyModel::Sample s;
        s.energy = e.energy;
        s.maxEnergy = e.maxEnergy;
        s.pips = e.pips;
        s.dead = e.dead;
        track.samples.push_back(s);
        track.peakMax = std::max(track.peakMax, e.maxEnergy);
    }
    return track.samples.empty() ? nullptr : &track;
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
    if (m_liveBarTime >= 0.0)
    {
        m_live->OnPlayerEnergyBar(m_liveBarTime, m_liveBarEnergy, m_liveBarFill);
        m_liveBarTime = -1.0;
    }
    DrawLiveCompass();
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
        std::string marks;
        if (!dead)
        {
            if (snap->has_weapon_spell) marks += " weapon-spell";
            if (snap->has_enchantment) marks += " enchanted";
            if (snap->has_condition || snap->has_deep_wound || snap->has_bleeding || snap->has_poison) marks += " condition";
            if (snap->has_hex) marks += " hexed";
            if (snap->has_deep_wound) marks += " deep-wound";
        }
        m_liveHudReport += std::format("target frame {} '{}' {} hp {:.3f} fill {} icons [{} ] at ({:.0f}, {:.0f}) {:.0f}x{:.0f}; ",
                                       m_liveTargetId, name, who, dead ? 0.f : snap->health_pct, fill, marks, tl.x, tl.y, kBarW, kBarH);
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

int ReplayWindow::LiveQueuedSlot()
{
    if (!m_live) return -1;
    double since = 0.0;
    const int slot = m_live->PlayerQueuedSlot(since);
    m_liveQueuedSince = static_cast<float>(since);
    return slot;
}

// -------------------------------------------------------------------------------------------------
// GW'S COMPASS. Read statically from Gw.exe (2026-11-04 client, Ui\Game\Compass\*.cpp, 0x8BA3F0..0x8C4900):
//   model     CompassModel (0x8C0820) builds one material of three DAT textures: 143016 (the fixed frame:
//             pewter bezel, the notch at the top, the pale disc in the middle), 143015 (the ring with N, E, S
//             and W) and 143017 (the glass: mid grey, a modulating shade). All three are 256 x 256 uncompressed
//             DDS (A4R4G4B4, A4R4G4B4, A8R8G8B8 for 143017 as an ATEX). The ring turns, the frame does not.
//   scale     CompassCanvas maps a world offset with (d * 0.40625) / 4500 (doubles 0xBA3948 and 0xBA3958;
//             their quotient 11076.92 at 0xBA3960 is the inverse): 4500 units reach 0.40625 of the model's
//             side, 104 of its 256 texels, which is the bezel's inner edge. The pale disc in 143016 is
//             about 22.5 texels across the radius, ~970 units: the aggro (earshot) circle is part of the art.
//   ground    CompassMap (0x8C0BB0..0x8C1A30) streams a 128 x 128 DXT1 texture round the player at 96 world
//             units a texel (0x94CD28), local texel (x - minX, maxY - y) / 96 from the map's bounds (0x7087F0,
//             the map file's own). Its source, in this order (0x8C13E0): the map's own image (area record
//             +0x68, a DAT file, repeated across the map: texel modulo its size); else the map's rectangle on
//             its continent's world map (area +0x48..+0x54, continent +0x04; tiles of 512 x 512 from the
//             world's file list, 0x5A75D0); outside the map, a missing tile or neither, the tiled 64 x 64
//             texture 9153 (0x8C0D90, "dirt"). The tables are CompassMapData.h. The area record always
//             exists: 0x5A6770 indexes a static table of 0x382 records and asserts on a larger id, so "no
//             record" never reaches the compass; a record without an image and with an empty rectangle
//             (x0 == x1, 0x8C16E5; the rectangle is +0x48..+0x54, or +0x58..+0x64 when the context flag
//             0x84C260 is set) is filled with 9153 (0x8C184D -> 0x8C0D90), as is an image that fails to load
//             (0x8C14D6). The DEV Test Arena (map 2: continent 3, both rectangles 0, image 0) is such a map:
//             the game shows 9153 there. It cannot be entered in the game, so this window shows the map
//             itself instead: its top-down render (BakeLiveCompassGroundFromMap), at the same 96 units a
//             texel, 9153 outside the map. Maps with compass art keep the game's method.
//   markers   CompassTarget.cpp: one quad per agent from the atlas 50465 (cells of 16 x 15.5 texels: dot
//             left, triangle right, 0x8C35A0), centred on the agent and reaching s = side x 6 / 256 EACH WAY
//             (0x8C35D9: the quad is pos - s .. pos + s, so 12 / 256 of the side across). The colour is AvChar
//             0x7F3F00 (0x7DD6E0), halved for the dimmed kinds. Seven passes, later on top (0x8C4530 with the
//             agent masks of AvChar 0x7FA8F0): dead foes (dimmed dot), dead players, dead allies (dimmed dot),
//             living spirits / minions / NPCs (triangle), living foes, living players not allied, living allies
//             (dot); nothing beyond 4500 units (0x8C3936: 0.40625 squared). The player is a dot at the centre
//             like any ally (the turned arrow 158487 / 23867 at 0x8C3E80 is only for the observer preference
//             0x5C). The target gets the outline 50466 of its own shape, 3 s across (side x 18 / 256,
//             0x8C37BC), tinted 0xFFFFFF00 (0x8C3291).
//   rotation  the preference LockCompassRotation (off by default: the compass turns with the camera).
//   clicks    a click on the compass is a ping / flag order in the game; not done here.
// ASSUMED: the glass drawn as a black / white overlay of its modulation; the ground's filtering (bilinear).
// -------------------------------------------------------------------------------------------------
struct LiveCompassGround
{
    bool tried = false;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;          // the baked image, in compass texels
    int pad = 0;               // texels of margin round the map (the image's (0, 0) is local texel (-pad, -pad))
    float minX = 0.f, maxY = 0.f;
    std::string source;        // what it was made of, for the report
    // A map with no compass art of its own: the ground is rebaked from the map's top-down render once the
    // map has been ready for a moment (BakeLiveCompassGroundFromMap); the 9153 image stands until then
    // and stays outside the map.
    bool fromMap = false;
    bool mapTried = false;
    int readyFrames = 0;
    std::vector<uint32_t> base;
};

namespace
{
// A DAT texture by file id, decoded to B, G, R, A rows (the decoder's own order).
bool LiveDatImage(DATManager* dat, const std::unordered_map<int, std::vector<int>>* index, uint32_t fileId, DatTexture& out)
{
    if (!dat || !index || !fileId) return false;
    const auto it = index->find(static_cast<int>(fileId));
    if (it == index->end()) return false;
    for (const int mft : it->second)
    {
        const auto bytes = dat->read_file_cached(static_cast<uint32_t>(mft));
        if (!bytes || bytes->size() < 16) continue;
        std::vector<uint8_t> copy(*bytes);   // the decoder writes to what it is given; the bytes are the cache's
        DatTexture t = ProcessImageFile(copy.data(), static_cast<int>(copy.size()));
        if (t.width > 0 && t.height > 0 && t.rgba_data.size() >= size_t(t.width) * size_t(t.height))
        {
            out = std::move(t);
            return true;
        }
    }
    return false;
}

const CompassMapData::Map* LiveCompassMapRecord(int mapId)
{
    for (const auto& m : CompassMapData::kMaps)
        if (m.mapId == mapId) return &m;
    return nullptr;
}

// The compass's ground for the whole map plus a margin of 50 texels (4800 units), as CompassMap fills it:
// texel (tx, ty) local to the map's top-left corner, 96 units each.
bool BakeLiveCompassGround(DATManager* dat, const std::unordered_map<int, std::vector<int>>* index, int mapId, float minX,
                           float minY, float maxX, float maxY, std::vector<uint32_t>& bgra, int& w, int& h, int& pad,
                           std::string& source)
{
    DatTexture dirt;
    if (!LiveDatImage(dat, index, 9153, dirt)) return false;
    const int mapW = static_cast<int>(std::ceil((maxX - minX) / 96.f));
    const int mapH = static_cast<int>(std::ceil((maxY - minY) / 96.f));
    if (mapW <= 0 || mapH <= 0 || mapW > 4096 || mapH > 4096) return false;
    pad = 50;
    w = mapW + 2 * pad;
    h = mapH + 2 * pad;
    bgra.assign(size_t(w) * size_t(h), 0);

    const CompassMapData::Map* rec = LiveCompassMapRecord(mapId);
    DatTexture own;
    const bool hasOwn = rec && rec->image && LiveDatImage(dat, index, rec->image, own);
    const bool hasRect = !hasOwn && rec && rec->x1 > rec->x0 && rec->y1 > rec->y0;
    const CompassMapData::World* world = nullptr;
    if (hasRect)
        for (const auto& wd : CompassMapData::kWorlds)
            if (wd.id == rec->continent) world = &wd;
    std::unordered_map<uint32_t, DatTexture> tiles;
    int tilesUsed = 0, tilesMissing = 0;
    auto tileTexel = [&](int wx, int wy, uint32_t& px) {
        if (!world || wx < 0 || wy < 0) return false;
        const int cx = wx >> 9, cy = wy >> 9;
        if (cx >= world->chunksX || cy >= world->chunksY) return false;
        const uint32_t id = CompassMapData::kTiles[world->first + cy * world->chunksX + cx];
        if (!id) return false;
        auto it = tiles.find(id);
        if (it == tiles.end())
        {
            DatTexture t;
            if (LiveDatImage(dat, index, id, t) && t.width == 512 && t.height == 512) ++tilesUsed;
            else { t = DatTexture{}; ++tilesMissing; }
            it = tiles.emplace(id, std::move(t)).first;
        }
        if (it->second.width != 512) return false;
        std::memcpy(&px, &it->second.rgba_data[size_t(wy & 511) * 512 + size_t(wx & 511)], 4);
        return true;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int tx = x - pad, ty = y - pad;
            uint32_t px = 0;
            bool done = false;
            if (tx >= 0 && ty >= 0 && tx < mapW && ty < mapH)
            {
                if (hasOwn)
                {
                    std::memcpy(&px, &own.rgba_data[size_t(ty % own.height) * size_t(own.width) + size_t(tx % own.width)], 4);
                    done = true;
                }
                else if (hasRect)
                    done = tileTexel(rec->x0 + tx, rec->y0 + ty, px);
            }
            if (!done)
                std::memcpy(&px, &dirt.rgba_data[size_t(((ty % dirt.height) + dirt.height) % dirt.height) * size_t(dirt.width) +
                                                 size_t(((tx % dirt.width) + dirt.width) % dirt.width)], 4);
            bgra[size_t(y) * size_t(w) + size_t(x)] = px | 0xFF000000u;   // DXT1: opaque
        }
    if (hasOwn)
        source = std::format("own image {} ({}x{}, repeated), 9153 outside", rec->image, own.width, own.height);
    else if (hasRect)
        source = std::format("world map {} rect ({}, {})-({}, {}), {} tile(s) read, {} missing, 9153 outside", rec->continent, rec->x0,
                             rec->y0, rec->x1, rec->y1, tilesUsed, tilesMissing);
    else
        source = "9153 tiled (the map has no compass image and no world-map rectangle)";
    return true;
}

bool LiveSaveBgraPng(const std::vector<uint32_t>& bgra, int w, int h, const std::wstring& path)
{
    DirectX::Image img{};
    img.width = static_cast<size_t>(w);
    img.height = static_cast<size_t>(h);
    img.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    img.rowPitch = img.width * 4;
    img.slicePitch = img.rowPitch * img.height;
    img.pixels = reinterpret_cast<uint8_t*>(const_cast<uint32_t*>(bgra.data()));
    return SUCCEEDED(DirectX::SaveToWICFile(img, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), path.c_str()));
}

// The self-test's folder (GWO_TRIALS_SELFTEST), or empty.
std::wstring LiveSelftestFolder()
{
    wchar_t buf[1024] = {};
    const DWORD n = GetEnvironmentVariableW(L"GWO_TRIALS_SELFTEST", buf, 1024);
    return n > 0 && n < 1024 ? std::wstring(buf) : std::wstring();
}
} // namespace

ImU32 ReplayWindow::LiveGwAgentColour(const AgentReplayData& ard, bool dead) const
{
    // 0x7F3F00, an explorable instance (0x108B094 != 0), no team-colour map flag (0x40000): dead 0xFFA0A0A0,
    // allegiance 3 (foe) 0xFFFF0000, allegiance 6 (NPC / minipet) 0xFFA0FF00, everyone else 0xFF00FF00.
    // An outpost's blue players (0xFF6060FF in a party, 0xFF9BBEFF alone) and self (0xFF40FF40) do not apply.
    if (dead) return IM_COL32(0xA0, 0xA0, 0xA0, 0xFF);
    int playerTeam = 1;
    if (m_live)
        if (const auto it = m_replayCtx.agents.find(m_live->PlayerAgentId()); it != m_replayCtx.agents.end())
            playerTeam = static_cast<int>(it->second.teamId);
    const int team = static_cast<int>(ard.teamId);
    if (team == 0 && ard.type == AgentType::NPC) return IM_COL32(0xA0, 0xFF, 0x00, 0xFF);
    if (team != 0 && team != playerTeam) return IM_COL32(0xFF, 0x00, 0x00, 0xFF);
    return IM_COL32(0x00, 0xFF, 0x00, 0xFF);
}

void ReplayWindow::DrawLiveCompass()
{
    if (!m_live || !m_deviceResources) return;
    const int playerId = m_live->PlayerAgentId();
    const auto pit = m_replayCtx.agents.find(playerId);
    if (pit == m_replayCtx.agents.end() || pit->second.snapshots.empty()) return;
    const AgentSnapshot* me = FindSnapshotAtTime(pit->second, m_debugTimeline);
    if (!me) return;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImTextureID texFrame = LoadGameUITexture(dev, "Compass\\ui_compass_frame.png");
    ImTextureID texRing = LoadGameUITexture(dev, "Compass\\ui_compass_ring.png");
    ImTextureID texGlass = LoadGameUITexture(dev, "Compass\\ui_compass_glass.png");
    ImTextureID texMarks = LoadGameUITexture(dev, "Compass\\ui_compass_markers.png");
    ImTextureID texOutline = LoadGameUITexture(dev, "Compass\\ui_compass_outlines.png");

    // The ground, baked once for this map from the DAT.
    if (!m_liveCompassGround) m_liveCompassGround = std::make_shared<LiveCompassGround>();
    LiveCompassGround& ground = *m_liveCompassGround;
    if (!ground.tried && m_mapRenderer && m_mapRenderer->GetTerrain())
    {
        ground.tried = true;
        const auto& b = m_mapRenderer->GetTerrain()->m_bounds;
        std::vector<uint32_t> bgra;
        if (BakeLiveCompassGround(m_datManager, m_hashIndex, m_replayCtx.mapId, b.map_min_x, b.map_min_z, b.map_max_x, b.map_max_z, bgra,
                                  ground.w, ground.h, ground.pad, ground.source))
        {
            ground.minX = b.map_min_x;
            ground.maxY = b.map_max_z;
            // No compass art (the table leaves such maps out): rebaked from the map's render later.
            ground.fromMap = LiveCompassMapRecord(m_replayCtx.mapId) == nullptr;
            if (ground.fromMap) ground.base = bgra;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(ground.w);
            td.Height = static_cast<UINT>(ground.h);
            td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA init{bgra.data(), static_cast<UINT>(ground.w) * 4, 0};
            ComPtr<ID3D11Texture2D> tex;
            if (FAILED(dev->CreateTexture2D(&td, &init, &tex)) || FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &ground.srv)))
                ground.srv.Reset();
            if (const std::wstring folder = LiveSelftestFolder(); !folder.empty())
                LiveSaveBgraPng(bgra, ground.w, ground.h, folder + std::format(L"\\compass_ground_map{}.png", m_replayCtx.mapId));
        }
        RunLog::Line("live: compass ground for map %d: %s%s", m_replayCtx.mapId, ground.source.c_str(), ground.srv ? "" : " (NOT made)");
        // The self-test also bakes two real maps, to check the other two sources against the game: the Great
        // Temple of Balthazar (248, world map 3) and the Warrior's Isle guild hall (4, its own image), each
        // over its rectangle / image size as bounds.
        if (const std::wstring folder = LiveSelftestFolder(); !folder.empty())
            for (const int other : {248, 4})
                if (const CompassMapData::Map* rec = LiveCompassMapRecord(other))
                {
                    DatTexture img;
                    const bool own = rec->image && LiveDatImage(m_datManager, m_hashIndex, rec->image, img);
                    const float wUnits = own ? img.width * 96.f : (rec->x1 - rec->x0) * 96.f;
                    const float hUnits = own ? img.height * 96.f : (rec->y1 - rec->y0) * 96.f;
                    std::vector<uint32_t> bgra;
                    int w = 0, h = 0, pad = 0;
                    std::string src;
                    if (BakeLiveCompassGround(m_datManager, m_hashIndex, other, 0.f, 0.f, wUnits, hUnits, bgra, w, h, pad, src))
                        LiveSaveBgraPng(bgra, w, h, folder + std::format(L"\\compass_ground_map{}.png", other));
                    m_live->Report(std::format("compass ground check: map {}: {} ({}x{} texels)", other, src, w, h));
                }
    }

    // The model's side on the screen: 256 at 1080 lines, top right under the menu bar.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float S = std::round(std::clamp(vp->Size.y * 256.f / 1080.f, 200.f, 340.f));
    const ImVec2 tl(vp->Pos.x + vp->Size.x - S - 8.f, vp->Pos.y + ImGui::GetFrameHeight() + 8.f);
    const ImVec2 c(tl.x + S * 0.5f, tl.y + S * 0.5f);
    const float inner = S * 0.40625f;                 // 4500 units
    const float pxPerUnit = inner / 4500.f;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // The camera's forward as a facing (the chase camera's rule), and the screen axes: a world offset
    // goes up the screen along the forward, right along (sin f, -cos f) (the core's left is (-sin, cos)).
    const float f = std::atan2(-std::cos(m_followYaw), -std::sin(m_followYaw));
    const float cf = std::cos(f), sf = std::sin(f);
    auto toScreen = [&](float dx, float dy) {
        const float along = dx * cf + dy * sf;
        const float right = dx * sf - dy * cf;
        return ImVec2(c.x + right * pxPerUnit, c.y - along * pxPerUnit);
    };
    // A square image turned by `a` (radians, clockwise on the screen) about `at`.
    auto turned = [&](ImTextureID tex, ImVec2 at, float half, float a, ImVec2 uv0, ImVec2 uv1, ImU32 col) {
        const float ca = std::cos(a), sa = std::sin(a);
        auto pt = [&](float x, float y) { return ImVec2(at.x + x * ca - y * sa, at.y + x * sa + y * ca); };
        dl->AddImageQuad(tex, pt(-half, -half), pt(half, -half), pt(half, half), pt(-half, half), uv0, ImVec2(uv1.x, uv0.y), uv1,
                         ImVec2(uv0.x, uv1.y), col);
    };

    // The ground under everything, clipped to the disc: a fan whose rim samples the baked image at the
    // world point under it (the inverse of toScreen), so it turns and scales with the markers.
    if (ground.srv)
    {
        constexpr int kRim = 96;
        const float texelX = (me->x - ground.minX) / 96.f + ground.pad, texelY = (ground.maxY - me->y) / 96.f + ground.pad;
        auto uvAt = [&](float rx, float ry) {   // a screen offset from the centre, in pixels
            const float right = rx / pxPerUnit, along = -ry / pxPerUnit;
            const float dx = along * cf + right * sf, dy = along * sf - right * cf;
            return ImVec2((texelX + dx / 96.f) / ground.w, (texelY - dy / 96.f) / ground.h);
        };
        dl->PushTextureID(reinterpret_cast<ImTextureID>(ground.srv.Get()));
        dl->PrimReserve(kRim * 3, kRim + 1);
        const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
        dl->PrimWriteVtx(c, uvAt(0.f, 0.f), IM_COL32_WHITE);
        for (int i = 0; i < kRim; ++i)
        {
            const float a = 6.2831853f * i / kRim;
            const float rx = std::sin(a) * inner, ry = -std::cos(a) * inner;
            dl->PrimWriteVtx(ImVec2(c.x + rx, c.y + ry), uvAt(rx, ry), IM_COL32_WHITE);
        }
        for (int i = 0; i < kRim; ++i)
        {
            dl->PrimWriteIdx(base);
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (i + 1) % kRim));
        }
        dl->PopTextureID();
    }
    else
        dl->AddCircleFilled(c, inner, IM_COL32(0, 0, 0, 140), 64);
    if (texGlass) dl->AddImage(texGlass, tl, ImVec2(tl.x + S, tl.y + S));
    // The ring: its N toward the world's +y. Up the texture is (0, -1) on the screen; +y lands at
    // (-cos f, -sin f), so the ring turns by atan2(-cos f, sin f).
    const float ringAngle = std::atan2(-cf, sf);
    // The frame first: the ring's letter tabs lie over the bezel (drawn under it they are hidden).
    if (texFrame) dl->AddImage(texFrame, tl, ImVec2(tl.x + S, tl.y + S));
    if (texRing) turned(texRing, c, S * 0.5f, ringAngle, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE);

    std::string report;
    const float s = S * 6.f / 256.f;     // the marker's half size (0x8C35D9)
    if (m_liveReportCompass)
        report = std::format("compass time {:.4f} player world ({:.2f}, {:.2f}) side {:.0f} at ({:.0f}, {:.0f}) centre ({:.0f}, {:.0f}) inner {:.2f} px = 4500 units, camera yaw {:.4f} "
                             "facing {:.4f}, ring {:.2f} deg, textures {}{}{}{}{}; ground {} ({}x{} texels, {}); marker {:.2f} px across, target outline {:.2f} px; ",
                             m_debugTimeline, me->x, me->y, S, tl.x, tl.y, c.x, c.y, inner, m_followYaw, f, ringAngle * 57.29578f, texFrame ? "F" : "-",
                             texRing ? "R" : "-", texGlass ? "G" : "-", texMarks ? "M" : "-", texOutline ? "O" : "-", ground.srv ? "drawn" : "MISSING",
                             ground.w, ground.h, ground.source, 2.f * s, 3.f * s);

    // The seven passes of 0x8C4530, later on top: 0 dead foes, 1 dead players (none apart), 2 dead allies,
    // 3 living spirits (triangles), 4 living foes, 5 living non-allied players (none apart), 6 living allies.
    struct Mark { int id; ImVec2 p; ImU32 col; bool triangle; int pass; bool target; };
    std::vector<Mark> marks;
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;
        if (ard.type != AgentType::Player && ard.type != AgentType::NPC && ard.type != AgentType::Spirit) continue;
        const AgentSnapshot* sn = id == playerId ? me : FindSnapshotAtTime(ard, m_debugTimeline);
        if (!sn) continue;
        const float dx = sn->x - me->x, dy = sn->y - me->y;
        if (dx * dx + dy * dy > 4500.f * 4500.f) continue;
        const bool dead = sn->is_dead;
        ImU32 col = LiveGwAgentColour(ard, dead);
        const bool foe = col == IM_COL32(0xFF, 0x00, 0x00, 0xFF);
        int pass = 6;
        if (dead) pass = foe ? 0 : 2;
        else if (ard.type == AgentType::Spirit || (ard.teamId == 0 && ard.type == AgentType::NPC)) pass = 3;
        else if (foe) pass = 4;
        if (pass == 0 || pass == 2)   // the dimmed kinds: each channel halved (0x8C35FD)
            col = IM_COL32(((col >> IM_COL32_R_SHIFT) & 0xFF) >> 1, ((col >> IM_COL32_G_SHIFT) & 0xFF) >> 1,
                           ((col >> IM_COL32_B_SHIFT) & 0xFF) >> 1, 0xFF);
        marks.push_back({id, toScreen(dx, dy), col, pass == 3, pass, id == m_liveTargetId});
    }
    std::stable_sort(marks.begin(), marks.end(), [](const Mark& a, const Mark& b) { return a.pass < b.pass; });
    constexpr float kCellV = 0.484375f;   // 15.5 / 32 (0x9529D4)
    for (const Mark& m : marks)
    {
        const float u0 = m.triangle ? 0.5f : 0.f;
        if (texMarks)
            dl->AddImage(texMarks, ImVec2(m.p.x - s, m.p.y - s), ImVec2(m.p.x + s, m.p.y + s), ImVec2(u0, 0.f), ImVec2(u0 + 0.5f, kCellV), m.col);
        else
            dl->AddCircleFilled(m.p, s * 0.55f, m.col);
    }
    // The target's outline, its own shape, over everything (the separate model 0x84 of CompassTarget).
    for (const Mark& m : marks)
        if (m.target && texOutline)
        {
            const float o = 1.5f * s;
            const float u0 = m.triangle ? 0.5f : 0.f;
            dl->AddImage(texOutline, ImVec2(m.p.x - o, m.p.y - o), ImVec2(m.p.x + o, m.p.y + o), ImVec2(u0, 0.f), ImVec2(u0 + 0.5f, 0.5f),
                         IM_COL32(0xFF, 0xFF, 0x00, 0xFF));
        }
    if (m_liveReportCompass)
    {
        for (const Mark& m : marks)
        {
            const auto& ard = m_replayCtx.agents.at(m.id);
            const AgentSnapshot* sn = m.id == playerId ? me : FindSnapshotAtTime(ard, m_debugTimeline);
            const float dx = sn->x - me->x, dy = sn->y - me->y;
            const float ang = std::atan2(m.p.x - c.x, -(m.p.y - c.y)) * 57.29578f;
            const bool foe = m.pass == 0 || m.pass == 4;
            report += std::format("agent {} {}{}{}{} world ({:.2f}, {:.2f}) {:.1f} units -> ({:.2f}, {:.2f}) r {:.2f} px angle {:.3f} deg colour {:08X} "
                                  "pass {} {};",
                                  m.id, m.id == playerId ? "player " : "", foe ? "foe" : "ally", sn->is_dead ? " dead" : "",
                                  m.target ? " target" : "", sn->x, sn->y, std::hypot(dx, dy), m.p.x, m.p.y, std::hypot(m.p.x - c.x, m.p.y - c.y), ang,
                                  ((m.col & 0xFF) << 16) | (m.col & 0xFF00) | ((m.col >> 16) & 0xFF) | 0xFF000000u, m.pass,
                                  m.triangle ? "triangle" : "dot");
        }
        // The name colours the labels use (the same rule), and what the selection ring drew last frame.
        report += " labels:";
        for (const auto& [id, ard] : m_replayCtx.agents)
            if (ard.type == AgentType::Player || ard.type == AgentType::NPC)
            {
                const ImU32 lc = LiveGwAgentColour(ard, ard.isDeadAtTime(m_debugTimeline));
                report += std::format(" {}={:08X}", id, ((lc & 0xFF) << 16) | (lc & 0xFF00) | ((lc >> 16) & 0xFF) | 0xFF000000u);
            }
        report += std::format("; tags (font {}):", m_liveLabelFont ? std::format("Tahoma {:.0f} px ascent {:.1f}", m_liveLabelFont->FontSize,
                                                                                m_liveLabelFont->Ascent)
                                                                  : std::string("NONE"));
        report += m_liveLabelReport.empty() ? std::string(" none") : m_liveLabelReport;
        report += "; ring: " + (m_liveRingReport.empty() ? std::string("none") : m_liveRingReport);
        m_live->Report("compass: " + report);
        m_liveReportCompass = false;
    }
}

// The compass ground of a map without compass art: the map seen from above, as this window renders it
// (the minimap's orthographic camera over the whole map, 1024 x 1024, no sky or fog), averaged into the
// compass's texels of 96 x 96 units (8 x 8 samples each) at the same local texels CompassMap uses, so it
// turns, scales and clips exactly as the game's own images do. Outside the map it keeps 9153.
void ReplayWindow::BakeLiveCompassGroundFromMap()
{
    if (!m_liveMode || !m_liveCompassGround || !m_deviceResources) return;
    LiveCompassGround& g = *m_liveCompassGround;
    if (!g.fromMap || g.mapTried || g.base.empty() || m_loadingPhase != LoadingPhase::Ready) return;
    if (++g.readyFrames < 90) return;   // the map's textures settle in the first frames after Ready
    g.mapTried = true;
    if (!m_mapRenderer || !m_mapRenderer->GetTerrain() || !m_mapRenderer->GetCamera()) return;

    const int savedW = m_minimapWidth, savedH = m_minimapHeight;
    const float savedZoom = m_minimapZoom, savedPanX = m_minimapPanX, savedPanZ = m_minimapPanZ;
    const DirectX::XMFLOAT4X4 savedViewProj = m_minimapViewProj;
    const bool wasReady = m_minimapResourcesReady;
    constexpr int kSide = 1024;
    m_minimapWidth = m_minimapHeight = kSide;
    m_minimapZoom = 1.f;
    m_minimapPanX = m_minimapPanZ = 0.f;

    DirectX::ScratchImage shot, converted;
    const DirectX::Image* img = nullptr;
    DirectX::XMFLOAT4X4 viewProj{};
    InitMinimapResources();
    if (m_minimapResourcesReady)
    {
        RenderMinimap();
        viewProj = m_minimapViewProj;
        if (SUCCEEDED(DirectX::CaptureTexture(m_deviceResources->GetD3DDevice(), m_deviceResources->GetD3DDeviceContext(),
                                              m_minimapTexture.Get(), shot)))
        {
            img = shot.GetImage(0, 0, 0);
            if (img && img->format != DXGI_FORMAT_B8G8R8A8_UNORM)
                img = SUCCEEDED(DirectX::Convert(*img, DXGI_FORMAT_B8G8R8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.5f, converted))
                          ? converted.GetImage(0, 0, 0) : nullptr;
        }
    }
    m_minimapWidth = savedW;
    m_minimapHeight = savedH;
    m_minimapZoom = savedZoom;
    m_minimapPanX = savedPanX;
    m_minimapPanZ = savedPanZ;
    m_minimapViewProj = savedViewProj;
    InitMinimapResources();
    if (!wasReady)   // the live window never shows the minimap: keep its targets released
    {
        m_minimapTexture.Reset();
        m_minimapRTV.Reset();
        m_minimapDepthTexture.Reset();
        m_minimapDSV.Reset();
        m_minimapSRV.Reset();
        m_minimapResourcesReady = false;
    }
    if (!img || img->width != size_t(kSide) || img->height != size_t(kSide))
    {
        RunLog::Line("live: compass ground from the map: the top-down render could not be read; 9153 stays");
        return;
    }

    // Texel (tx, ty), local to the map's top-left corner, covers the recorded (x, y) from minX + 96 tx and
    // maxY - 96 ty; its samples go through the map transform and the render's camera to the picture.
    const DirectX::XMMATRIX vp = DirectX::XMLoadFloat4x4(&viewProj);
    std::vector<uint32_t> bgra = g.base;
    const int mapW = g.w - 2 * g.pad, mapH = g.h - 2 * g.pad;
    int filled = 0;
    constexpr int kSub = 8;
    for (int ty = 0; ty < mapH; ++ty)
        for (int tx = 0; tx < mapW; ++tx)
        {
            uint32_t sum[3] = {0, 0, 0};
            int n = 0;
            for (int sy = 0; sy < kSub; ++sy)
                for (int sx = 0; sx < kSub; ++sx)
                {
                    const float x = g.minX + (tx + (sx + 0.5f) / kSub) * 96.f;
                    const float y = g.maxY - (ty + (sy + 0.5f) / kSub) * 96.f;
                    const DirectX::XMFLOAT3 w = ApplyMapTransformToPos(x, y, 0.f, m_replayCtx.mapTransform);
                    const DirectX::XMVECTOR c = DirectX::XMVector4Transform(DirectX::XMVectorSet(w.x, w.y, w.z, 1.f), vp);
                    const float cw = std::max(1e-4f, DirectX::XMVectorGetW(c));
                    const float u = (DirectX::XMVectorGetX(c) / cw) * 0.5f + 0.5f, v = (1.f - DirectX::XMVectorGetY(c) / cw) * 0.5f;
                    if (u < 0.f || v < 0.f || u >= 1.f || v >= 1.f) continue;
                    const uint8_t* p = img->pixels + size_t(v * kSide) * img->rowPitch + size_t(u * kSide) * 4;
                    sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
                    ++n;
                }
            if (n < kSub * kSub / 2) continue;
            bgra[size_t(ty + g.pad) * size_t(g.w) + size_t(tx + g.pad)] =
                0xFF000000u | ((sum[2] / n) << 16) | ((sum[1] / n) << 8) | (sum[0] / n);
            ++filled;
        }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(g.w);
    td.Height = static_cast<UINT>(g.h);
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init{bgra.data(), static_cast<UINT>(g.w) * 4, 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    auto* dev = m_deviceResources->GetD3DDevice();
    if (FAILED(dev->CreateTexture2D(&td, &init, &tex)) || FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
    {
        RunLog::Line("live: compass ground from the map: texture not made; 9153 stays");
        return;
    }
    g.srv = srv;
    g.source = std::format("map render (no compass image, no world-map rectangle: the map from above, {} x {} px, {} of {} texels "
                           "filled at 96 units), 9153 outside",
                           kSide, kSide, filled, mapW * mapH);
    g.base.clear();
    g.base.shrink_to_fit();
    if (const std::wstring folder = LiveSelftestFolder(); !folder.empty())
    {
        LiveSaveBgraPng(bgra, g.w, g.h, folder + std::format(L"\\compass_ground_map{}.png", m_replayCtx.mapId));
        // The render itself, for the eye.
        DirectX::SaveToWICFile(*img, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                               (folder + std::format(L"\\compass_map{}_render.png", m_replayCtx.mapId)).c_str());
    }
    RunLog::Line("live: compass ground for map %d: %s", m_replayCtx.mapId, g.source.c_str());
}

// -------------------------------------------------------------------------------------------------
// GW'S SELECTION RING. Read statically from Gw.exe (2026-11-04 client, AgentView\AvSelect.cpp and AvAgent):
//   when    AvAgent 0x7EAC60 (the target flag 0x80) and 0x7EA510 (the hover flag 0x10) make the ring through
//           0x7EAD20: the agent's virtual 0 (AvChar 0x7F4240) gives the colour, 0x7F3F00 with alpha 0xC0 for the
//           target and 0x60 for a hovered agent that is not the target, and the art: 1, or 0 for a foe on a
//           team-colour map (area flag 0x40000). The ring is destroyed when the flag goes.
//   art     0x7E4730 picks from the table 0xA95128 ("art < SELECT_ART"): 0 = 9091 once, turning at pi/4 rad/s
//           (flags 0x12, 0x7E37C0); 1 = 9089 twice (flags 6 each, 0x7E3820), each layer's size breathing with
//           MapOverlay param 3 = 0.25, param 5 = 1/3 Hz, the second layer's phase param 4 = 1.5 s (0xA95010);
//           2 = 9089 once, still (0x7E3880). Textures loaded by 0x7E49A0 (9089 at 0xA95118, 9091 at 0xA95120).
//   shape   a MapOverlay (Engine\Map\MapOverlay.cpp, "ovrl"): a square of half size r = the agent's +0x3C
//           (0x7EAD3F; at least 1, 0x70F0EA), laid on the terrain (0x750440), texture across the square (UV
//           0.5 / r, 0x70F183). Breathing (flag 4, 0x70FC31): UV scaled by k = 2 / (2 - (sin(2 pi p5 (t + p4)) + 1) p3),
//           so the ring shrinks to 1 / k: 1 down to 0.75 and back every 3 s.
// Here: art 1 under the live target (and a hovered agent at 0x60), a terrain-hugging grid in the depth
// buffer's test, drawn before the characters (no depth write), so a body in front covers it.
//   radius  the agent's +0x3C: AvAgent 0x7EADB0 asks 0x775CB0 (MdlApi) for the model's three bounding
//           cylinders {radius, top, bottom} (anim data +0x48, 12 bytes each) and keeps slot i at +0x34 + 8 i,
//           radius and height times the agent's scale (+0x94; 1.0, or a player's height scale, 0x7EB0F0), so
//           +0x3C is slot 1's radius. MdlLoad 0x7944F0 fills the slots from the file's FA1 chunk (header 0x58,
//           count at +0x14, records {slot, radius, top, bottom} of 16 bytes: each slot takes the next record
//           of its index, else the first); no FA1 or no records leave MdlBuild's default {36, 72, 0}
//           (0x779AE0). Players: every profession bank of a sex carries the same cylinder (male 14.4069,
//           female 15.3506); the mannequin's bank 0x2963E and the henchmen's 0x1C601 have none (36).
// ASSUMED: ordinary alpha blending for material 0x99A, the overlay's clock = the match clock; the live
// player has no appearance record, so its height scale is 1.
// -------------------------------------------------------------------------------------------------
struct LiveSelectionRing
{
    bool tried = false;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> constants, vertices;
    ComPtr<ID3D11ShaderResourceView> texture;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> raster;
    UINT capacity = 0;
};

namespace
{
const char kLiveRingShader[] = R"(
cbuffer Frame : register(b0) { float4x4 viewProj; };
Texture2D ring : register(t0);
SamplerState ringSampler : register(s0);
struct V { float3 p : POSITION; float2 uv : TEXCOORD0; float4 c : COLOR0; };
struct P { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 c : COLOR0; };
P VSMain(V i) { P o; o.p = mul(float4(i.p, 1), viewProj); o.uv = i.uv; o.c = i.c; return o; }
float4 PSMain(P i) : SV_Target0 { return ring.Sample(ringSampler, i.uv) * i.c; }
)";
struct LiveRingVertex { float x, y, z, u, v; float r, g, b, a; };
struct LiveRingFrame { DirectX::XMFLOAT4X4 viewProj; };

// Everything the pass changes, put back afterwards.
struct LiveRingSaved
{
    ID3D11DeviceContext* c;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11InputLayout> il;
    ComPtr<ID3D11Buffer> vb, vsCb;
    UINT stride = 0, offset = 0, stencil = 0, sampleMask = 0;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    float factor[4]{};
    explicit LiveRingSaved(ID3D11DeviceContext* ctx) : c(ctx)
    {
        c->VSGetShader(&vs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->IAGetInputLayout(&il);
        c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->IAGetPrimitiveTopology(&topology);
        c->VSGetConstantBuffers(0, 1, &vsCb);
        c->PSGetShaderResources(0, 1, &srv);
        c->PSGetSamplers(0, 1, &sampler);
        c->RSGetState(&raster);
        c->OMGetBlendState(&blend, factor, &sampleMask);
        c->OMGetDepthStencilState(&depth, &stencil);
    }
    ~LiveRingSaved()
    {
        c->VSSetShader(vs.Get(), nullptr, 0);
        c->PSSetShader(ps.Get(), nullptr, 0);
        c->GSSetShader(gs.Get(), nullptr, 0);
        c->IASetInputLayout(il.Get());
        c->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
        c->IASetPrimitiveTopology(topology);
        c->VSSetConstantBuffers(0, 1, vsCb.GetAddressOf());
        c->PSSetShaderResources(0, 1, srv.GetAddressOf());
        c->PSSetSamplers(0, 1, sampler.GetAddressOf());
        c->RSSetState(raster.Get());
        c->OMSetBlendState(blend.Get(), factor, sampleMask);
        c->OMSetDepthStencilState(depth.Get(), stencil);
    }
};
} // namespace

void ReplayWindow::DrawLiveSelectionRing()
{
    if (!m_liveMode || !m_live || !m_mapRenderer || !m_deviceResources) return;
    auto* device = m_deviceResources->GetD3DDevice();
    auto* context = m_deviceResources->GetD3DDeviceContext();
    Camera* camera = m_mapRenderer->GetCamera();
    if (!device || !context || !camera) return;
    if (!m_liveSelectionRing) m_liveSelectionRing = std::make_shared<LiveSelectionRing>();
    LiveSelectionRing& st = *m_liveSelectionRing;
    if (!st.tried)
    {
        st.tried = true;
        DatTexture tex;
        bool ok = LiveDatImage(m_datManager, m_hashIndex, 9089, tex);
        if (ok)
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(tex.width);
            td.Height = static_cast<UINT>(tex.height);
            td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // the decoder's "RGBA" is B, G, R, A in memory
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA init{tex.rgba_data.data(), static_cast<UINT>(tex.width) * 4, 0};
            ComPtr<ID3D11Texture2D> t2;
            ok = SUCCEEDED(device->CreateTexture2D(&td, &init, &t2)) && SUCCEEDED(device->CreateShaderResourceView(t2.Get(), nullptr, &st.texture));
        }
        ComPtr<ID3DBlob> vsBlob, psBlob, errors;
        ok = ok &&
            SUCCEEDED(D3DCompile(kLiveRingShader, sizeof(kLiveRingShader), "LiveRing", nullptr, nullptr, "VSMain", "vs_5_0",
                                 D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsBlob, &errors)) &&
            SUCCEEDED(D3DCompile(kLiveRingShader, sizeof(kLiveRingShader), "LiveRing", nullptr, nullptr, "PSMain", "ps_5_0",
                                 D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errors)) &&
            SUCCEEDED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &st.vs)) &&
            SUCCEEDED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &st.ps));
        if (ok)
        {
            const D3D11_INPUT_ELEMENT_DESC elements[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0}};
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = sizeof(LiveRingFrame);
            cb.Usage = D3D11_USAGE_DYNAMIC;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            D3D11_BLEND_DESC bd{};
            auto& rt = bd.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
            rt.DestBlendAlpha = D3D11_BLEND_ONE;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
            D3D11_DEPTH_STENCIL_DESC dd{};
            dd.DepthEnable = TRUE;
            dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;   // reversed Z
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            rd.MultisampleEnable = TRUE;
            ok = SUCCEEDED(device->CreateInputLayout(elements, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &st.layout)) &&
                SUCCEEDED(device->CreateBuffer(&cb, nullptr, &st.constants)) && SUCCEEDED(device->CreateSamplerState(&sd, &st.sampler)) &&
                SUCCEEDED(device->CreateBlendState(&bd, &st.blend)) && SUCCEEDED(device->CreateDepthStencilState(&dd, &st.depth)) &&
                SUCCEEDED(device->CreateRasterizerState(&rd, &st.raster));
        }
        if (!ok) st.ps.Reset();
        RunLog::Line("live: selection ring %s", ok ? "pass ready (texture 9089)" : "pass NOT ready (texture, shader or states failed)");
    }
    m_liveRingReport.clear();
    if (!st.ps) return;

    // Who carries a ring: the target at 0xC0, a hovered agent that is not the target at 0x60.
    struct Pick { int id; float alpha; };
    Pick picks[2];
    int n = 0;
    if (m_liveTargetId >= 0) picks[n++] = {m_liveTargetId, 192.f / 255.f};
    if (m_hoveredAgentId >= 0 && m_hoveredAgentId != m_liveTargetId) picks[n++] = {m_hoveredAgentId, 96.f / 255.f};
    if (n == 0) return;

    constexpr int kGrid = 12;
    const float t = m_debugTimeline;
    std::vector<LiveRingVertex> verts;
    verts.reserve(size_t(n) * 2 * kGrid * kGrid * 6);
    for (int k = 0; k < n; ++k)
    {
        const auto it = m_replayCtx.agents.find(picks[k].id);
        if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) continue;
        const AgentReplayData& ard = it->second;
        const float kRadius = LiveSelectionRingRadius(ard);   // the agent's +0x3C
        float sx, sy, sz;
        InterpolateAgentPosition(ard, t, m_replayCtx.interpSettings, sx, sy, sz);
        const DirectX::XMFLOAT3 at = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);
        const ImU32 col = LiveGwAgentColour(ard, ard.isDeadAtTime(t));
        const float cr = ((col >> IM_COL32_R_SHIFT) & 0xFF) / 255.f, cg = ((col >> IM_COL32_G_SHIFT) & 0xFF) / 255.f,
                    cb = ((col >> IM_COL32_B_SHIFT) & 0xFF) / 255.f;
        std::string layers;
        for (int layer = 0; layer < 2; ++layer)
        {
            // MapOverlay 0x70FC35: k = 2 / (2 - (sin(2 pi p5 (t + p4)) + 1) p3), p3 0.25, p5 1/3, p4 0 / 1.5.
            const float p4 = layer == 0 ? 0.f : 1.5f;
            const float kScale = 2.f / (2.f - (std::sin(6.2831853f * (1.f / 3.f) * (t + p4)) + 1.f) * 0.25f);
            layers += std::format("{}{:.3f}", layer ? "/" : "", 1.f / kScale);
            auto vertexAt = [&](int gx, int gz) {
                const float ox = (2.f * gx / kGrid - 1.f) * kRadius, oz = (2.f * gz / kGrid - 1.f) * kRadius;
                float h = at.y;
                m_mapRenderer->TryGetGroundHeight(at.x + ox, at.z + oz, h);
                LiveRingVertex v{};
                v.x = at.x + ox;
                v.y = h + 1.5f;
                v.z = at.z + oz;
                v.u = ox / kRadius * 0.5f * kScale + 0.5f;
                v.v = oz / kRadius * 0.5f * kScale + 0.5f;
                v.r = cr, v.g = cg, v.b = cb, v.a = picks[k].alpha;
                return v;
            };
            for (int gz = 0; gz < kGrid; ++gz)
                for (int gx = 0; gx < kGrid; ++gx)
                {
                    const LiveRingVertex a = vertexAt(gx, gz), b = vertexAt(gx + 1, gz), c2 = vertexAt(gx + 1, gz + 1), d = vertexAt(gx, gz + 1);
                    verts.insert(verts.end(), {a, b, c2, a, c2, d});
                }
        }
        // The square's width on the screen (its two ends along the camera's right), for the report.
        float screenW = 0.f;
        {
            const DirectX::XMMATRIX vpm = camera->GetView() * camera->GetProj();
            const DirectX::XMFLOAT3 right = camera->GetRight3f();
            auto px = [&](float s) {
                const DirectX::XMVECTOR c = DirectX::XMVector3TransformCoord(
                    DirectX::XMVectorSet(at.x + right.x * kRadius * s, at.y, at.z + right.z * kRadius * s, 1.f), vpm);
                return ImVec2(DirectX::XMVectorGetX(c), DirectX::XMVectorGetY(c));
            };
            const ImVec2 a0 = px(-1.f), a1 = px(1.f);
            const auto vpr = m_deviceResources->GetScreenViewport();
            screenW = std::hypot((a1.x - a0.x) * 0.5f * vpr.Width, (a1.y - a0.y) * 0.5f * vpr.Height);
        }
        m_liveRingReport += std::format("{}agent {} art 1 (9089 x2) r {:.4f} square {:.1f} px at ({:.1f}, {:.1f}, {:.1f}) colour {:08X} alpha {:02X} scale {}",
                                        m_liveRingReport.empty() ? "" : ", ", picks[k].id, kRadius, screenW, at.x, at.y, at.z,
                                        ((col & 0xFF) << 16) | (col & 0xFF00) | ((col >> 16) & 0xFF) | 0xFF000000u, static_cast<int>(picks[k].alpha * 255.f + 0.5f),
                                        layers);
    }
    if (verts.empty()) return;
    if (!st.vertices || st.capacity < verts.size())
    {
        D3D11_BUFFER_DESC bd{};
        st.capacity = static_cast<UINT>(verts.size());
        bd.ByteWidth = st.capacity * sizeof(LiveRingVertex);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        st.vertices.Reset();
        if (FAILED(device->CreateBuffer(&bd, nullptr, &st.vertices))) { st.capacity = 0; return; }
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(st.vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, verts.data(), verts.size() * sizeof(LiveRingVertex));
    context->Unmap(st.vertices.Get(), 0);
    LiveRingFrame frame{};
    DirectX::XMStoreFloat4x4(&frame.viewProj, DirectX::XMMatrixTranspose(camera->GetView() * camera->GetProj()));
    if (FAILED(context->Map(st.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &frame, sizeof(frame));
    context->Unmap(st.constants.Get(), 0);

    LiveRingSaved saved(context);
    context->IASetInputLayout(st.layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(st.vs.Get(), nullptr, 0);
    context->PSSetShader(st.ps.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, st.constants.GetAddressOf());
    context->PSSetShaderResources(0, 1, st.texture.GetAddressOf());
    context->PSSetSamplers(0, 1, st.sampler.GetAddressOf());
    context->RSSetState(st.raster.Get());
    context->OMSetBlendState(st.blend.Get(), nullptr, UINT_MAX);
    context->OMSetDepthStencilState(st.depth.Get(), 0);
    const UINT stride = sizeof(LiveRingVertex), offset = 0;
    context->IASetVertexBuffers(0, 1, st.vertices.GetAddressOf(), &stride, &offset);
    context->Draw(static_cast<UINT>(verts.size()), 0);
}

float ReplayWindow::LiveSelectionRingRadius(const AgentReplayData& ard)
{
    if (const auto it = m_liveRingRadius.find(ard.agent_id); it != m_liveRingRadius.end()) return it->second;
    // The file the game builds the agent's model from: a player's profession bank (W, R, Mo, N, Me, E, A,
    // Rt, P, D; the banks the renderer pins), the mannequin's bank, a henchman's NPC model file.
    static constexpr uint32_t kMaleBank[10] = {379789, 379792, 379795, 379799, 379802, 379805, 379807, 379809, 379811, 379813};
    static constexpr uint32_t kFemaleBank[10] = {379790, 379793, 379797, 379800, 379803, 379806, 379808, 379810, 379812, 379814};
    uint32_t fileId = 0;
    const char* kind = "other";
    if (IsTrialsMannequinModelId(ard.modelId)) fileId = 0x2963E, kind = "mannequin bank";
    else if (ard.type == AgentType::Player)
    {
        const int p = std::clamp(ard.primaryProf, 1, 10) - 1;
        fileId = ard.isFemale ? kFemaleBank[p] : kMaleBank[p];
        kind = ard.isFemale ? "female profession bank" : "male profession bank";
    }
    else if (const HenchmanInfo* h = LookupHenchman(ard.modelId)) fileId = h->npcModelFileId, kind = "henchman NPC model file";

    // MdlLoad 0x7944F0: the FA1 chunk's cylinders; slot 1 is the next record of index 1 after slot 0's, else the first.
    float radius = 36.f;
    int records = -1;
    if (fileId && m_datManager && m_hashIndex)
        if (const auto it = m_hashIndex->find(static_cast<int>(fileId)); it != m_hashIndex->end())
            for (const int mft : it->second)
            {
                const auto bytes = m_datManager->read_file_cached(static_cast<uint32_t>(mft));
                if (!bytes || bytes->size() < 13 || std::memcmp(bytes->data(), "ffna", 4) != 0) continue;
                const uint8_t* b = bytes->data();
                const size_t n = bytes->size();
                for (size_t o = 5; o + 8 <= n;)
                {
                    uint32_t id = 0, size = 0;
                    std::memcpy(&id, b + o, 4);
                    std::memcpy(&size, b + o + 4, 4);
                    const size_t body = o + 8;
                    if (size > n - body) break;
                    if (id == 0xFA1 && size >= 0x58)
                    {
                        uint32_t count = 0;
                        std::memcpy(&count, b + body + 0x14, 4);
                        records = static_cast<int>(count);
                        struct Cyl { uint32_t slot; float radius, top, bottom; };
                        std::vector<Cyl> cyl;
                        for (uint32_t k = 0; k < count && body + 0x58 + 16 * (k + 1) <= body + size; ++k)
                        {
                            Cyl c{};
                            std::memcpy(&c, b + body + 0x58 + 16 * k, 16);
                            cyl.push_back(c);
                        }
                        if (!cyl.empty())
                        {
                            size_t used = 0;
                            Cyl slot1 = cyl[0];
                            for (uint32_t s = 0; s < 2; ++s)
                            {
                                const Cyl c = used < cyl.size() && cyl[used].slot == s ? cyl[used++] : cyl[0];
                                if (s == 1) slot1 = c;
                            }
                            radius = slot1.radius;
                        }
                        break;
                    }
                    o = body + size;
                }
                break;
            }
    radius = std::max(1.f, radius);   // MapOverlay 0x70F0EA
    m_liveRingRadius[ard.agent_id] = radius;
    RunLog::Line("live: selection ring radius for agent %d: %.4f (%s %u, %s)", ard.agent_id, radius, kind, fileId,
                 records < 0 ? "no FA1: default 36" : records == 0 ? "FA1 without cylinders: default 36" : "FA1 slot 1");
    return radius;
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
    case LiveScriptedUi::Kind::ReportCompass:
        m_liveReportCompass = true;
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
