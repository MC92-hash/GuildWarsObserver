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

// =================================================================================================
// THE BREADCRUMB - the one thing a crash handler can still read.
//
// A hardware exception (an integer divide by zero, an access violation, a display driver that
// refuses a call) gives the faulting code no chance to write anything: there is no unwinding and no
// return. What there IS, by the time the unhandled-exception filter runs, is memory - so the risky
// per-frame steps leave a mark in a fixed place instead of logging, and the filter reads that mark
// back and puts it in the log.
//
// Set() is plain stores into fixed-size storage with no lock, no allocation and no I/O, so it is
// cheap enough for a per-frame, per-agent, per-submesh step. `where` must be a string literal or
// something else that outlives the process: only the pointer is kept.
//
// Read() returns the last mark, formatted, and is meant for the crash handler alone.
// =================================================================================================
void Set(const char* where, int a = -1, int b = -1, int c = -1);
std::string Read();
} // namespace RunLog
