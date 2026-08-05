#pragma once

class SpatialAudioEngine;

// "Sound FX" settings panel: master volume + per-category (Background/Effects/UI/Music/Dialog)
// volume sliders for the StoC/sound_events.txt playback engine. Shown while `show` is true
// (bound to the ribbon toolbar's Sound FX toggle - the panel and audio playback share the same
// on/off state, same as Notepad/Bookmarks share their toggle with their panel's visibility).
void DrawSoundFxPanel(SpatialAudioEngine* engine, bool& show);
