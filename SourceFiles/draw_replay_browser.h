#pragma once

#include "ReplayLibrary.h"

struct PendingReplayRequest
{
    bool requested = false;
    MatchMeta match;
};

inline PendingReplayRequest g_pendingReplay;
inline bool g_cloudDownloadInProgress = false;
inline bool g_refreshMatchIndex = false;
inline bool g_invalidateFilters = false;
inline bool g_refreshHint = false;

void draw_replay_browser(ReplayLibrary& library);

// [2026-09-30] SCOUT is a mode of this screen; the application ribbon shows it as a tool of its own
// and switches it here.
bool replay_browser_scout_mode();
void replay_browser_set_scout_mode(bool on);
