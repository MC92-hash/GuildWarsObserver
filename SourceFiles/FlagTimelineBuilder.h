#pragma once
#include "ReplayMapData.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <algorithm>
#include <cfloat>

// ---------------------------------------------------------------------------
// Flag Timeline — reconstructed from StoC flag_events.txt
// ---------------------------------------------------------------------------

enum class FlagTeam : uint8_t { Red = 0, Blue = 1 };

enum class FlagTimelineEventType : uint8_t {
    Spawn,        // flag appears at base (match start, after stick, after return)
    Pickup,       // same-team player picks up flag
    Drop,         // carrier drops flag (death or manual)
    Stick,        // carrier places flag on flagstand
    Return,       // opposing team touches dropped flag, teleports to base
    GroundSpawn   // flag appears on ground after drop
};

enum class FlagLocation : uint8_t {
    Base,         // sitting at team's neutral spawn point
    Carried,      // held by a player (no world agent)
    Ground,       // dropped on the ground
    Stand         // placed on a flagstand (tower or obelisk — check standAgentId)
};

enum class StandOwner : uint8_t { Neutral, Red, Blue };

// One entry per flag state change
struct FlagTimelineEvent {
    float                  time = 0.f;
    FlagTeam               flagTeam = FlagTeam::Red;
    FlagTimelineEventType  eventType = FlagTimelineEventType::Spawn;
    FlagLocation           newLocation = FlagLocation::Base;

    int   actorAgentId    = -1;   // player who performed the action
    int   carrierAgentId  = -1;   // player carrying (-1 if not carried)
    float x = 0, y = 0, z = 0;   // world position (base/ground/stand coords)

    int   flagWorldAgentId = -1;  // ground agent ID when dropped
    int   standAgentId     = -1;  // flagstand gadget ID (for stick events)
};

// Per-team state machine output
struct FlagTeamTimeline {
    FlagTeam team = FlagTeam::Red;
    float spawnX = 0, spawnY = 0, spawnZ = 0;

    std::vector<FlagTimelineEvent> events; // sorted by time

    FlagLocation locationAtTime(float t) const;
    int          carrierAtTime(float t) const;
    void         positionAtTime(float t, float& outX, float& outY, float& outZ) const;
};

// ---------------------------------------------------------------------------
// Non-flag bundles — vine seeds on Druid's Isle, repair kits on Warrior's Isle.
// They travel through the same StoC packets as flags, so they are reconstructed
// alongside them, but they behave alike and unlike flags: neither belongs to a
// team (either side can carry one), both get a fresh agent id every time they are
// dropped, and both are consumed for good once spent on the map object they feed.
// So they are tracked per item id rather than per team, and share one timeline.
// ---------------------------------------------------------------------------

enum class BundleLocation : uint8_t {
    Base,      // at its initial spawn point, untouched
    Carried,   // held by a player (no world agent exists)
    Ground,    // dropped, a new world agent exists
    Consumed   // spent on a bridge / catapult; gone for the rest of the match
};

struct BundleTimelineEvent {
    float          time = 0.f;
    BundleLocation newLocation = BundleLocation::Base;

    int   actorAgentId   = -1;   // player who picked up / dropped / spent it
    int   carrierAgentId = -1;   // player carrying (-1 if not carried)
    float x = 0, y = 0, z = 0;   // world position

    // True when a pickup packet named the carrier, false when it was inferred from
    // proximity. Good enough to draw either way, but only a named carrier may
    // override the flag state machine.
    bool  carrierConfirmed = false;

    int   worldAgentId = -1;     // ground agent id after a drop
};

struct BundleTimeline {
    int        itemId = 0;       // 47/48 on Druid's Isle, 59/60 on Warrior's Isle
    BundleType type = BundleType::Unknown;
    float spawnX = 0, spawnY = 0, spawnZ = 0;
    bool  spawnResolved = false; // false when no world agent revealed the spawn point

    std::vector<BundleTimelineEvent> events; // sorted by time

    BundleLocation locationAtTime(float t) const;
    int            carrierAtTime(float t) const;
    void           positionAtTime(float t, float& outX, float& outY, float& outZ) const;
};

// Flagstand control change
struct StandControlEvent {
    float      time = 0.f;
    StandOwner owner = StandOwner::Neutral;
    int        standAgentId = -1;
    float      moraleExpiry = 0.f;  // time when the 120s morale boost ends
};

struct StandTimeline {
    float standX = 0, standY = 0, standZ = 0;
    int   standAgentId = -1;
    std::vector<StandControlEvent> events;

    StandOwner ownerAtTime(float t) const;
};

// Top-level output
struct FlagTimeline {
    FlagTeamTimeline teams[2];  // [0]=red, [1]=blue
    StandTimeline    stand;     // tower flagstand
    StandTimeline    obelisk;   // obelisk flagstand (Isle of Meditation); stays empty on other maps
    std::vector<FlagTimelineEvent> allEvents; // merged, sorted chronologically
    std::unordered_set<int> allFlagItemIds;   // all item_ids from FLAG_ITEM events
    // Vine seeds / repair kits; empty on maps without them.
    std::vector<BundleTimeline> bundles;
    bool valid = false;
};

// Builder — reconstructs FlagTimeline from raw StoC data
class FlagTimelineBuilder {
public:
    struct Input {
        const FlagEventData*                            flagEvents = nullptr;
        const std::vector<LifecycleEvent>*              lifecycle  = nullptr;
        const std::vector<MapObjectEvent>*              mapObject  = nullptr;
        // Needed to tell a bundle that was spent on its map object (a vine bridge
        // growing, a catapult being repaired) from one merely dropped on the ground.
        const std::vector<DoorEvent>*                   doorEvents = nullptr;
        const std::unordered_map<int, AgentReplayData>* agents     = nullptr;
        // Tells flags from map bundles. Item ids are recycled between the two, so
        // without this every classification here is a coin flip.
        const FlagItemRegistry*                         flagItems  = nullptr;
        int mapId = 0;
    };

    static FlagTimeline Build(const Input& input);
};
