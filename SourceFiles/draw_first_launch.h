#pragma once

// Draw the loading screen (background + progress bar + optional dat-path prompt).
// dat_path_is_set  = true when a valid gw.dat path has been configured
// dat_load_fraction = 0..1 representing how far along dat file reading is
// Returns true when loading is visually complete and the main UI can take over.
bool draw_first_launch(bool dat_path_is_set, float dat_load_fraction);

void draw_first_launch_reset_state();
