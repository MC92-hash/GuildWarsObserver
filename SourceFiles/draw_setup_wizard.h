#pragma once

// Two-step first-launch wizard. Returns true when setup is complete.
bool draw_setup_wizard();

// Read-only licence viewer modal (for Help menu).
void draw_licence_modal(bool* open);

// File paths modal (DAT file + match data folder, for Help menu).
void draw_dat_settings_modal(bool* open);
