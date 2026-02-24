#pragma once
#include "ReplayMapData.h"
#include <filesystem>
#include <memory>

// Launches async parsing of all StoC event files inside matchFolder/StoC/.
// Results are accumulated in the shared StoCParseProgress and later moved
// into the ReplayContext when finished.
void LaunchStoCParsing(const std::filesystem::path& matchFolder,
                       std::shared_ptr<StoCParseProgress> progress);

// Call once per frame. Returns true when parsing is done and results have been
// transferred into ctx.stocData.
bool PollStoCParseCompletion(ReplayContext& ctx);
