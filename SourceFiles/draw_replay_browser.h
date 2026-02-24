#pragma once

#include "ReplayLibrary.h"

struct PendingReplayRequest
{
    bool requested = false;
    MatchMeta match;
};

inline PendingReplayRequest g_pendingReplay;

void draw_replay_browser(ReplayLibrary& library);
