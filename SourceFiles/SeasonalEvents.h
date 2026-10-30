#pragma once

// Guild Wars seasonal event windows, by calendar date (any year). Used for the launch screen
// (today's date) and for the Halloween GvG NPC disguises (the match's own date).
//   Halloween  Oct 18 - Nov 2
//   Wintersday Dec 19 - Jan 2

inline bool IsHalloweenDate(int month, int day)
{
    const int md = month * 100 + day; // e.g. Oct 18 = 1018
    return md >= 1018 && md <= 1102;
}

inline bool IsWintersdayDate(int month, int day)
{
    const int md = month * 100 + day;
    return md >= 1219 || (md >= 101 && md <= 102);
}
