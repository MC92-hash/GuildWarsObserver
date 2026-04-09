#pragma once
#include <string>

struct LoadingProgress
{
    bool  dat_path_is_set = false;
    int   dat_files_read  = 0;
    int   dat_files_total = 0;
    int   match_count     = -1;  // -1 = not loaded yet, 0+ = loaded count
};

struct UpdateInfo
{
    bool   available = false;
    std::string currentVersion;
    std::string latestVersion;
    std::string releaseUrl;
    std::string repo;
};

// Draw the loading screen (background + progress bar + status text + optional dat-path prompt).
// Returns true when loading is visually complete and the main UI can take over.
bool draw_first_launch(const LoadingProgress& progress, const UpdateInfo* update = nullptr);

void draw_first_launch_reset_state();
