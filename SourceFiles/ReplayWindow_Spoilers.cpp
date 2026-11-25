#include "pch.h"
#include "ReplayWindow.h"
#include "SpoilerFree.h"
#include "RunLog.h"

// ---------------------------------------------------------------------------
// Spoiler-free mode in the replay (SpoilerFree.h). While the match's result is hidden:
//   - every time axis, the scrub bar first, runs to a fixed length (kAxisMatchSeconds of match
//     clock after the minute before the gate), never to the recording's real end;
//   - nothing is drawn past the furthest moment the viewer has reached (m_spoilerSeenT): no
//     markers, buckets, spikes or health curves from the part still to come;
//   - Highlights is locked, since its moments include the Guild Lord's death.
// Reaching the Victory, or the end of the recording, reveals the match for good. These remain
// ReplayWindow:: member functions; only their definitions live here.
// ---------------------------------------------------------------------------

namespace
{
    // A GvG cannot run past 28:00 (the lord-damage tiebreak), so 30:00 holds every match.
    constexpr float kAxisMatchSeconds = 30.f * 60.f;
}

bool ReplayWindow::SpoilersHidden() const
{
    return !m_liveMode && SpoilerFree::Get().IsHidden(m_matchMeta.folder_name);
}

float ReplayWindow::SpoilerAxisEnd(float maxT) const
{
    if (!SpoilersHidden()) return maxT;
    return std::max(maxT, m_displayTimeOffset + kAxisMatchSeconds);
}

float ReplayWindow::SpoilerHorizon() const
{
    return SpoilersHidden() ? m_spoilerSeenT : FLT_MAX;
}

void ReplayWindow::SpoilerToast(const char* text)
{
    m_clip.toast = text;
    m_clip.toastIsError = false;
    m_clip.toastOverLimit = false;
    m_clip.savedPath.clear();
    m_clip.toastUntil = ImGui::GetTime() + 2.5;
}

// Called every tick once loaded, after the clock has moved (next to ApplyLinkView).
void ReplayWindow::UpdateSpoilers()
{
    if (m_liveMode) return;
    const std::string& folder = m_matchMeta.folder_name;
    SpoilerFree& sf = SpoilerFree::Get();
    const bool hidden = sf.IsHidden(folder);
    // Hidden from this tick (the window opened on a hidden match, or the toggle went on): what
    // counts as watched starts at the playhead.
    if (hidden && !m_spoilerWasHidden)
        m_spoilerSeenT = m_debugTimeline;
    m_spoilerSeenT = std::max(m_spoilerSeenT, m_debugTimeline);

    const bool nowHidden = sf.IsHidden(folder);
    // Hidden while Highlights plays (a share link, or the toggle switched on): its moments
    // include the Guild Lord's death, so it stops.
    if (nowHidden && HighlightsActive())
    {
        ToggleHighlights();
        RunLog::Line("spoilers: Highlights left - the result is hidden");
    }
    if (nowHidden != m_spoilerWasHidden)
        RunLog::Line("spoilers: the replay of '%s' %s", folder.c_str(),
                     nowHidden ? "hides its result (fixed 30:00 axis, nothing past the furthest moment watched)"
                               : "shows its result");
    m_spoilerWasHidden = nowHidden;
}
