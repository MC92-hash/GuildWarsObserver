#pragma once

// =================================================================================================
// THE RUN LOG - what happened during this launch, on disk, in every configuration.
//
// WHY A FILE. OutputDebugStringA needs a debugger attached; a build handed to somebody to run has
// none, so anything it says is said to nobody. Three of the questions this project keeps asking -
// how long did the launch take, why is the library empty, why did the replay fall back to the
// stand-in models - are all answerable from one run IF the run leaves a record behind. This is
// that record.
//
//     %APPDATA%\GWObserver\PlayerVisuals.log
//
// next to config.ini, which is a directory the process can always write to (the build output
// directory is not: an installed copy may sit under Program Files). Truncated on the first write
// of each run, so the file is always exactly one launch.
//
// COMPILED AND ACTIVE IN DEBUG **AND** RELEASE. It carries no rule vocabulary: every string it
// writes is either a number, a file-system path, or prose about loading and timing, and the release
// string gate is what proves that. Keep it that way - a message here is read by whoever is holding
// the log, which may be anybody.
//
// Thread-safe: the replay's appearance reader runs its first phase on a worker thread, so Line()
// takes a lock and appends. Cheap enough for a few hundred lines a run; it is NOT a per-frame sink.
// =================================================================================================

#include <filesystem>
#include <string>

namespace RunLog
{
// %APPDATA%\GWObserver\PlayerVisuals.log, or an empty path when the profile directory is unknown
// (in which case every call below is a no-op).
const std::filesystem::path& Path();

// Milliseconds since the first call into this module - i.e. since the earliest logged event of the
// run, which MapBrowser::Initialize makes the first line.
double ElapsedMs();

// One line, printf-style, prefixed with the elapsed time. A trailing newline is added.
void Line(const char* format, ...);

// Text that may already contain newlines (a path list, an audit). Every line gets the prefix.
void Block(const std::string& text);
} // namespace RunLog
