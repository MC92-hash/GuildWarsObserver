#pragma once
#include <cstdint>

// The one place that says which team is which colour.
//
// A team id is the game's own value (GWCA Agent::team_id): 0 = none, 1 = Blue, 2 = Red,
// 3 = Yellow. Every other team encoding in the replay data names the same two teams in the
// same order - jumbo "att1"/"att2", match-metadata party "1"/"2", flag item codes 20/21,
// the [0]/[1] slots of per-team arrays - so team-index 0 is Blue and team-index 1 is Red.
//
// Never compare a team id against a literal 1 or 2 to pick a colour, a label or a texture;
// ask these helpers. Grouping code that only needs "same team / other team" may keep
// comparing ids directly.
namespace Team
{
    constexpr uint8_t None   = 0;
    constexpr uint8_t Blue   = 1;
    constexpr uint8_t Red    = 2;
    constexpr uint8_t Yellow = 3;

    constexpr bool IsBlue(int teamId) { return teamId == Blue; }
    constexpr bool IsRed(int teamId)  { return teamId == Red; }
    constexpr bool IsSide(int teamId) { return teamId == Blue || teamId == Red; }

    // Team id <-> the [0]/[1] slot of a per-team array. Slot 0 is Blue, slot 1 is Red.
    constexpr int     Index01(int teamId)   { return teamId == Blue ? 0 : teamId == Red ? 1 : -1; }
    constexpr uint8_t FromIndex01(int slot) { return slot == 0 ? Blue : slot == 1 ? Red : None; }
    constexpr bool    IndexIsBlue(int slot) { return slot == 0; }
    constexpr bool    IndexIsRed(int slot)  { return slot == 1; }

    // The opposing side; anything that is not a side comes back unchanged.
    constexpr uint8_t Other(int teamId)
    {
        return teamId == Blue ? Red : teamId == Red ? Blue : static_cast<uint8_t>(teamId);
    }

    constexpr const char* Name(int teamId)
    {
        switch (teamId) {
        case None:   return "None";
        case Blue:   return "Blue";
        case Red:    return "Red";
        case Yellow: return "Yellow";
        default:     return "?";
        }
    }
    constexpr const char* NameLower(int teamId)
    {
        switch (teamId) {
        case Blue: return "blue";
        case Red:  return "red";
        default:   return "none";
        }
    }
}
