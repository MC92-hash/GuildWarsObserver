#pragma once

class MatchReplay;

// Draws the replay timeline bar anchored to the bottom of the viewport.
// Returns true if the replay is active and providing agent data.
bool draw_timeline_bar(MatchReplay& replay);
