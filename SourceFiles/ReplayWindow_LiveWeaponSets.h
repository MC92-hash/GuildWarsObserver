#pragma once

// The weapon set bar of a live match (Trials): the player's four sets in the bottom-right corner of the
// window, the game's ovals and F1-F4 keycaps, each set's weapon icons. A click on a set asks for it.
// Only a live window draws it (ReplayWindow_Live.cpp); the replay has its own panel
// (ReplayWindow_FocusedPlayerHud.cpp) and is not touched by this one.

#include <imgui.h>

class ILiveSource;
class DATManager;
struct ID3D11Device;

// Draws the bar for this frame. Nothing when the source has no weapon sets.
void DrawLiveWeaponSetBar(ILiveSource& live, ID3D11Device* device, DATManager* dat);

// The set the bar's last click asked for (0..3), once; -1 when none. One live window exists at a time
// (a second replay window is refused), so the click is held here rather than on the window.
int TakeLiveWeaponSetClick();
