#include "pch.h"
#include "ReplayWindow.h"
#include "ReplaySync.h"
#include "UrlProtocol.h"
#include "RunLog.h"

// ---------------------------------------------------------------------------
// Share links in the replay (SHARE_LINKS_PLAN.md sections 5 and 6). These remain ReplayWindow::
// member functions; only their definitions live here.
// ---------------------------------------------------------------------------

namespace
{
    const char* CamName(MatchLink::Cam c)
    {
        return c == MatchLink::Cam::Follow ? "follow" : c == MatchLink::Cam::Free ? "free" : "default";
    }
}

// Called every tick once loaded (next to ReplaySync::TakeSeek), never in a live window.
void ReplayWindow::ApplyLinkView()
{
    // The window turns Ready while the recording's parsers may still run: the displayed clock's
    // offset (m_displayTimeOffset) is only final once both are in.
    if (!m_replayTimeConsolidated)
        return;

    MatchLink link;
    if (ReplaySync::TakeView(m_matchMeta.folder_name, link))
    {
        // Time and pause at once: they need nothing that loads later.
        const float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
        m_debugTimeline = std::clamp(link.t + m_displayTimeOffset, 0.f, maxT);
        m_replayCtx.isPlaying = !link.paused;
        if (link.agent >= 0 && m_replayCtx.agents.count(link.agent))
            m_selectedAgentId = link.agent;

        // A camera from the link: nothing else may steer it from the next frame on.
        if (link.cam != MatchLink::Cam::None)
        {
            if (HighlightsActive())
                ToggleHighlights();
            m_autoCameraEnabled = false;
            if (m_topViewActive)
                ExitTopView();
            m_topViewTransitioning = false;
        }

        m_linkView = link;
        m_linkViewPending = link.cam != MatchLink::Cam::None;
        m_linkViewSince = ReplaySync::Now();
        m_linkViewLogFrames = m_linkViewPending ? -1 : 30;
        RunLog::Line("link: view taken by the replay of '%s' - match clock %.1f s (timeline %.1f s), %s, agent %d,"
                     " %s camera",
                     m_matchMeta.folder_name.c_str(), link.t, m_debugTimeline, link.paused ? "paused" : "playing",
                     link.agent, CamName(link.cam));
    }

    if (m_linkViewPending)
    {
        const MatchLink& l = m_linkView;
        Camera* cam = m_mapRenderer ? m_mapRenderer->GetCamera() : nullptr;
        if (l.cam == MatchLink::Cam::Free && cam)
        {
            if (m_cameraMode == CameraMode::FollowAgent)
                ExitFollowMode();
            cam->SetPosition(l.pos.x, l.pos.y, l.pos.z);
            cam->SetOrientation(l.pitch, l.yaw);
            m_linkViewPending = false;
        }
        else if (l.cam == MatchLink::Cam::Follow)
        {
            // The follow camera aims at the agent's position: wait until it has one.
            auto it = m_replayCtx.agents.find(l.agent);
            bool ok = it != m_replayCtx.agents.end() && !it->second.snapshots.empty();
            if (ok)
            {
                EnterFollowMode(l.agent);
                ok = m_cameraMode == CameraMode::FollowAgent && m_followedAgentId == l.agent;
            }
            if (ok)
            {
                m_followYaw = l.yaw;
                m_followPitch = std::clamp(l.pitch, kFollowMinPitch, kFollowMaxPitch);
                m_followDist = m_followDistTarget = std::clamp(l.dist, kFollowMinDist, kFollowMaxDist);
                m_followTransActive = false;   // no flight in from the old view
                m_linkViewPending = false;
            }
            else if (ReplaySync::Now() - m_linkViewSince > 2.0)
            {
                RunLog::Line("link: FAILED to follow agent %d - it has no position in this recording", l.agent);
                m_linkViewPending = false;
            }
        }
        else
            m_linkViewPending = false;

        if (!m_linkViewPending)
            m_linkViewLogFrames = 30;
    }

    // Half a second later, what the window actually shows: proves nothing took the view back.
    if (m_linkViewLogFrames > 0 && --m_linkViewLogFrames == 0)
    {
        m_linkViewLogFrames = -1;
        Camera* cam = m_mapRenderer ? m_mapRenderer->GetCamera() : nullptr;
        const XMFLOAT3 p = cam ? cam->GetPosition3f() : XMFLOAT3(0.f, 0.f, 0.f);
        RunLog::Line("link: view applied - match clock %.1f s, %s, %s camera (followed %d, yaw %.3f, pitch %.3f,"
                     " dist %.0f), camera at (%.0f, %.0f, %.0f) yaw %.3f pitch %.3f",
                     m_debugTimeline - m_displayTimeOffset, m_replayCtx.isPlaying ? "playing" : "paused",
                     m_cameraMode == CameraMode::FollowAgent ? "follow" : "free", m_followedAgentId, m_followYaw,
                     m_followPitch, m_followDistTarget, p.x, p.y, p.z, cam ? cam->GetYaw() : 0.f,
                     cam ? cam->GetPitch() : 0.f);
    }
}

// The link for `timelineT` (the replay's own clock) with the current selection, camera and pause.
MatchLink ReplayWindow::CurrentShareLink(float timelineT) const
{
    MatchLink l;
    l.folder = m_matchMeta.folder_name;
    l.t = std::max(0.f, timelineT - m_displayTimeOffset);
    l.paused = !m_replayCtx.isPlaying;
    MatchLinks::FillDisplay(l, m_matchMeta);   // the website's card: guilds, capes, professions
    if (m_cameraMode == CameraMode::FollowAgent && m_followedAgentId >= 0)
    {
        l.agent = m_followedAgentId;
        l.cam = MatchLink::Cam::Follow;
        l.yaw = m_followYaw;
        l.pitch = m_followPitch;
        l.dist = m_followDistTarget > 0.f ? m_followDistTarget : m_followDist;
    }
    else
    {
        l.agent = m_selectedAgentId;
        // Top View is a mode of its own, not a camera to hand over: the link keeps the default.
        Camera* cam = m_mapRenderer ? m_mapRenderer->GetCamera() : nullptr;
        if (cam && !m_topViewActive && !m_topViewTransitioning)
        {
            l.cam = MatchLink::Cam::Free;
            l.pos = cam->GetPosition3f();
            l.yaw = cam->GetYaw();
            l.pitch = cam->GetPitch();
        }
    }
    return l;
}

// `fromStart`: the match from 0:00 with the default camera and nobody selected (the play bar's
// Share menu, "Copy link from match start").
void ReplayWindow::CopyShareLink(float timelineT, bool fromStart)
{
    auto toast = [this](const char* text, bool error)
    {
        m_clip.toast = text;
        m_clip.toastIsError = error;
        m_clip.toastOverLimit = false;
        m_clip.savedPath.clear();
        m_clip.toastUntil = ImGui::GetTime() + 2.5;
    };
    if (!UrlProtocol::IsPublished(m_matchMeta.folder_name))
    {
        toast(UrlProtocol::kNotPublishedTip, true);
        return;
    }
    MatchLink link;
    if (fromStart)
    {
        link.folder = m_matchMeta.folder_name;
        link.t = 0.f;
        link.paused = !m_replayCtx.isPlaying;
        MatchLinks::FillDisplay(link, m_matchMeta);
    }
    else
        link = CurrentShareLink(timelineT);
    const std::string url = MatchLinks::FormatHttps(link);
    if (UrlProtocol::CopyToClipboard(url, m_hwnd))
    {
        RunLog::Line("link: copied %s", url.c_str());
        toast("Link copied", false);
    }
    else
        toast("Could not copy the link", true);
}

// File menu entries: the link for now, and the way back to the Library after a link-only start.
void ReplayWindow::DrawShareLinkMenuItems()
{
    if (m_liveMode) return;
    const bool published = UrlProtocol::IsPublished(m_matchMeta.folder_name);
    if (ImGui::MenuItem("Copy Share Link", nullptr, false, published))
        CopyShareLink(m_debugTimeline);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", published ? "A link to this moment, with this camera" : UrlProtocol::kNotPublishedTip);
    if (UrlProtocol::LibraryHidden() && ImGui::MenuItem("Open Library"))
        UrlProtocol::ShowLibrary("Open Library in the replay window");
}
