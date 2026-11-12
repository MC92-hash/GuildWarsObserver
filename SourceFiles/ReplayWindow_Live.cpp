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
#include <cmath>
#include <format>

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
    m_liveClickedAgent = -1;

    m_live->Advance(in, m_replayCtx);
    m_debugTimeline = static_cast<float>(m_live->DisplayTime());
    m_replayCtx.maxReplayTime = std::max(m_replayCtx.maxReplayTime, static_cast<float>(m_live->SimTime()));
    m_replayCtx.isPlaying = true;
    m_matchOverlayActive = false;
    m_autoCameraEnabled = false;
    LiveInvalidate(m_live->TakeAppended());
    m_liveTargetId = m_live->TargetAgentId();

    if (m_agentModelsLoaded && !m_liveModelsReported)
        LiveReportModels();

    // THE CHASE CAMERA: always on the player; GW's distance and pitch the first time, then the
    // user's (wheel, right drag), with the yaw eased behind the heading while the player steers.
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

    if (m_live) m_live->DrawHud();
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
