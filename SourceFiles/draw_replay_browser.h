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
