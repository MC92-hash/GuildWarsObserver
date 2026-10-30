#pragma once
// ---------------------------------------------------------------------------
// ReplaySync - the Library's Stats tab and an open replay window, linked
//
// Both run in this process, on the main thread (MapBrowser ticks the replay windows and draws the
// Library in the same frame loop), so the link is two plain slots, no locking:
//   - the replay publishes every tick where it is, on the DISPLAYED match clock (0:00 = the gate,
//     the clock match_analysis.json and the Library use), and whether it plays;
//   - the Library can ask it to jump to a time (a death picked on its timeline, a range).
// Only one replay window is meant to be open at a time, so one slot each is enough. A replay that
// stopped publishing (closed, still loading) is forgotten after a second.
// ---------------------------------------------------------------------------
#include <chrono>
#include <string>

namespace ReplaySync
{
    struct Replay
    {
        std::string folder;      // MatchMeta::folder_name of the match it plays
        float t = 0.f;           // displayed match clock, seconds
        bool playing = false;
        double seen = -1.0;      // Now() at its last publish
    };

    inline double Now()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    inline Replay& Slot() { static Replay r; return r; }

    struct SeekRequest { std::string folder; float t = 0.f; bool pending = false; };
    inline SeekRequest& SeekSlot() { static SeekRequest s; return s; }

    // The replay, each tick once loaded.
    inline void Publish(const std::string& folder, float displayT, bool playing)
    {
        Replay& r = Slot();
        r.folder = folder;
        r.t = displayT;
        r.playing = playing;
        r.seen = Now();
    }

    // The open replay of this match, or nullptr.
    inline const Replay* Find(const std::string& folder)
    {
        const Replay& r = Slot();
        return !folder.empty() && r.folder == folder && Now() - r.seen < 1.0 ? &r : nullptr;
    }

    // The Library: ask the replay of `folder` to jump to `displayT`.
    inline void RequestSeek(const std::string& folder, float displayT)
    {
        SeekSlot() = { folder, displayT, true };
    }

    // The replay: a jump asked for its match, taken once.
    inline bool TakeSeek(const std::string& folder, float& displayT)
    {
        SeekRequest& s = SeekSlot();
        if (!s.pending || s.folder != folder) return false;
        s.pending = false;
        displayT = s.t;
        return true;
    }
}
