#pragma once

#include "ReplayLibrary.h"

struct PendingReplayRequest
{
    bool requested = false;
    MatchMeta match;
    // False: only make the match local (download it if it is cloud-only) and open no replay.
    // The Library's analysis tabs ask for this.
    bool openReplay = true;
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

// The Library's skill hover card (icon, cost, description), for other screens' skill lists.
void replay_browser_skill_tooltip(int skillId);

// The ui_kit theme the Library draws with right now (its card gallery has its own), for screens
// that dress like it.
int replay_browser_active_theme();
