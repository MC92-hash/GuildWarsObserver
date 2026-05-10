# Flag Rendering and Flag State — Overview

This document describes every layer of flag logic in GW Observer's replay system: how raw StoC packets are parsed, how the state machine reconstructs flag ownership through time, how flags are rendered on the 3D map, and how flag events feed into the UI (event messages, morale timers, timeline pills, party bar icons, and the debug panel).

---

## Table of Contents

1. [Data Sources — Raw StoC Flag Events](#1-data-sources--raw-stoc-flag-events)
2. [Agent Classification — Identifying Flags](#2-agent-classification--identifying-flags)
3. [FlagTimelineBuilder — Reconstructing the State Machine](#3-flagtimelinebuilder--reconstructing-the-state-machine)
4. [Flag State Model — Enums and Structures](#4-flag-state-model--enums-and-structures)
5. [Runtime Queries — Querying State at a Point in Time](#5-runtime-queries--querying-state-at-a-point-in-time)
6. [3D Map Rendering — `DrawFlags()`](#6-3d-map-rendering--drawflags)
7. [Flag Event Messages — On-Screen Notifications](#7-flag-event-messages--on-screen-notifications)
8. [Morale Boost Timers](#8-morale-boost-timers)
9. [Party Bar Flag Icons](#9-party-bar-flag-icons)
10. [Timeline Panel — Flag Pills and Icons](#10-timeline-panel--flag-pills-and-icons)
11. [Flag Debug Window](#11-flag-debug-window)
12. [Interpolation Rules — Flags Never Interpolate](#12-interpolation-rules--flags-never-interpolate)
13. [Agent Incarnation Splitting — Flags Are Skipped](#13-agent-incarnation-splitting--flags-are-skipped)

---

## 1. Data Sources — Raw StoC Flag Events

Flags originate from the file `flag_events.txt` inside a recorded match folder. The StoC parser (`StoCParser.cpp`, function `ParseFlagEvents`) reads lines with semicolon-delimited fields and sorts them into seven typed vectors inside `FlagEventData`:

| Code | Name | Fields | Struct |
|------|------|--------|--------|
| 0 | `FLAG_PICKUP` | `item_id`, `player_agent_id`, `team_code` | `FlagPickupEvent` |
| 1 | `FLAG_DROP` | `player_agent_id`, `team_code` | `FlagDropEvent` |
| 2 | `FLAG_STATE` | `team_code`, `item_id`, `state` | `FlagStateEvent` |
| 3 | `FLAG_ITEM` | `item_id`, `model_id`, `extra_id`, `type` | `FlagItemEvent` |
| 4 | `FLAG_STAND` | `stand_agent_id`, `sub_field`, `value` | `FlagStandEvent` |
| 5 | `FLAG_SPAWN` | `agent_id`, `unk`, `object_id` | `FlagSpawnEvent` |
| 6 | `FLAG_ANNOUNCE` | `action`, `template_id`, `team` | `FlagAnnounceEvent` |

Every event carries a `time` field (seconds since match start) and a `raw_line` for debug inspection.

`FlagEventData` is stored inside `StoCData::flagEvents` and is populated asynchronously on a background thread during replay loading.

---

## 2. Agent Classification — Identifying Flags

### Hardcoded Per-Map Item IDs

`IsFlagItemId(mapId, itemId)` in `ReplayMapData.h` returns true if an item_id corresponds to a flag on a given GvG map. Each map has exactly two flag item_ids (one per team). Example:

| Map | Blue Flag item_id | Red Flag item_id |
|-----|-------------------|------------------|
| Burning Isle (167) | 57 | 58 |
| Warrior's Isle (171) | 67 | 68 |
| Isle of Jade (356) | 61 | 62 |

### Classification in AgentSnapshotParser

During agent parsing (`AgentSnapshotParser.cpp`), each agent's first snapshot is checked:

```cpp
if (IsFlagItemId(mapId, first.item_id)) {
    ard.type         = AgentType::Flag;
    ard.categoryName = "Flag";
}
```

### Dynamic Re-Classification from FLAG_ITEM Data

After StoC loading completes, `ReplayWindow` re-classifies any unknown agents whose `item_id` appears in the StoC `FLAG_ITEM` events. This catches dynamically spawned flags (respawns) whose item_ids are not in the hardcoded table:

```cpp
for (auto& e : m_replayCtx.stocData.flagEvents.items)
    flagItemIds.insert(e.item_id);
for (auto& [id, ard] : m_replayCtx.agents) {
    if (ard.type != AgentType::Unknown) continue;
    if (flagItemIds.count(ard.snapshots.front().item_id)) {
        ard.type = AgentType::Flag;
        ard.categoryName = "Flag";
    }
}
```

### Team Identification Constants

Flags are associated with teams via `extra_id` from FLAG_ITEM events:
- **Blue**: `extra_id == 59808`
- **Red**: `extra_id == 57400`

Team codes from FLAG_PICKUP / FLAG_DROP use different values:
- **Blue**: `team_code == 20`
- **Red**: `team_code == 21`

---

## 3. FlagTimelineBuilder — Reconstructing the State Machine

`FlagTimelineBuilder::Build()` transforms the raw, interleaved StoC events into a clean per-team timeline of state transitions. It runs once, after both agent data and StoC data are loaded. The build process has six phases:

### Phase 1: Collect Flag Item IDs

All `item_id` values from FLAG_ITEM events are recorded in `allFlagItemIds`. A running `itemTeamMap` (item_id → FlagTeam) is populated incrementally during Phase 4 as FLAG_ITEM events are encountered.

### Phase 2: Resolve Base Spawn Positions

For each team's first FLAG_ITEM event, the builder determines where the flag spawns at the start of the match. It tries four strategies in order:

1. **Lifecycle match** — Find an `AGENT_ADD` lifecycle event within 2 seconds of the FLAG_ITEM event whose `type_code` passes `IsFlagItemId()`. Use that event's (x, y) coordinates.
2. **Item_id match** — Find an `AGENT_ADD` whose agent has a matching `item_id` in its snapshots.
3. **Agent snapshot scan** — Search all agents for one whose first snapshot `item_id` matches the FLAG_ITEM's `item_id`.
4. **Direct agent lookup** — Use the `item_id` as an agent_id and look up its first snapshot position.

### Phase 3: Resolve Flagstand Position

The Tower Flag Stand is identified by finding a `FLAG_STAND` event with `value == 120000` (the morale timer sentinel value in milliseconds = 120 seconds). Fallback: search the agents map for an agent with `categoryName == "Tower Flag Stand"`.

The stand's world position is read from the flagstand agent's first snapshot.

### Phase 4: Chronological Event Processing

All seven event types are merged into a single chronological sequence, sorted by `(time, priority)`. Priority ensures correct ordering when events share the same timestamp:

| Priority | Event Type |
|----------|-----------|
| 0 | FLAG_ITEM (register item → team mapping first) |
| 1 | FLAG_STAND |
| 2 | FLAG_STATE |
| 3 | FLAG_SPAWN |
| 4 | FLAG_PICKUP |
| 5 | FLAG_DROP |
| 6 | FLAG_ANNOUNCE |

The builder processes each event and emits `FlagTimelineEvent` entries:

#### FLAG_ITEM (code 3)
Registers the item_id → team mapping (Blue or Red based on `extra_id`).

#### FLAG_SPAWN (code 5)
Records a recently spawned flag agent (`agent_id` → `object_id`) for position resolution of subsequent DROP events.

#### FLAG_PICKUP (code 0)
Team resolution uses three fallback strategies:
1. Resolve from the picking player's `teamId` in the agent data
2. Resolve from `itemTeamMap` (item_id → team)
3. Resolve from `team_code` (20 = Blue, 21 = Red)

**Synthetic DROP insertion**: If the team already has a carrier when a new pickup arrives, the builder synthesizes a DROP event. It scans the previous carrier's snapshots backwards to find:
- A `weapon_type` transition from 0 (holding bundle) to non-zero (dropped the flag), or
- A `is_dead` transition from false to true (died while carrying)

This gives the exact drop time and position. If neither is found, the carrier's position at the pickup time is used.

Emits: `Pickup` event with `FlagLocation::Carried`.

#### FLAG_DROP (code 1)
Team resolution: player's teamId → team_code → check which team has this player as carrier.

**Spurious drop filtering**: The GW server sometimes fires a FLAG_DROP shortly after a PICKUP even though the player keeps holding the flag. The builder validates by checking if the carrier's `weapon_type` is still 0 one second after the alleged drop. If so, the drop is discarded as spurious.

**Position resolution** (in priority order):
1. If a FLAG_SPAWN occurred at the same time, use the spawn's `object_id` to find the ground agent's lifecycle position
2. Fall back to the dropping player's position at that time

Emits: `Drop` event with `FlagLocation::Ground`.

#### FLAG_ANNOUNCE — STICK (code 6, action=1)
A team places their flag on the watchtower flagstand. This emits two events:
1. **Stick** — flag moves to `FlagLocation::Stand` at the stand's world coordinates
2. **Spawn** (time + 0.001s) — the flag immediately respawns at `FlagLocation::Base` (a new flag appears at the team's base)

Also emits a `StandControlEvent` recording the team as the new stand owner, with `moraleExpiry = time + 120` seconds.

#### FLAG_ANNOUNCE — RETURN (code 6, action=0)
The opposing team returns a dropped flag. The `team` field identifies who performed the return (not whose flag). The flag team is the *opposite* team.

**Synthetic DROP for missing data**: If the flag team still has a carrier recorded (meaning the raw data missed the drop), the builder synthesizes a DROP using the same weapon_type/death scanning logic.

**Return actor detection**: The builder identifies which opposing player actually touched the flag by scanning all agents of the returning team within ±1 second of the event, checking for specific animation codes (`3002646805`, `3002646795`, `3002646789`) and proximity to the flag's last known ground position (within 200 game units).

Emits: `Return` event with `FlagLocation::Base`.

### Phase 4b: Trailing Carrier Cleanup

After all events are processed, any team that still has an active carrier gets a synthetic DROP event. The builder scans the carrier's snapshots for the weapon_type transition or death to find the actual drop moment.

### Phase 5: Stand Control from MAP_OBJECT Events

The builder looks at `MAP_OBJECT` events for the flagstand's object_id. An `animation_stage == 2` indicates the stand returning to neutral (e.g., timer expiration). These produce `StandControlEvent` entries with `StandOwner::Neutral`, unless a FLAG_ANNOUNCE already handled that timestamp.

### Phase 6: Insert Initial Spawn Events

Every team with at least one event gets an initial `Spawn` event if one doesn't exist. The spawn time is taken from the team's earliest FLAG_ITEM event, or defaults to `time = 0`. Position is the base spawn coordinates resolved in Phase 2.

Finally, per-team events are sorted by time, and a merged `allEvents` list is built chronologically.

---

## 4. Flag State Model — Enums and Structures

### FlagTeam
```
Blue = 0, Red = 1
```

### FlagLocation (where the flag currently is)
| Value | Meaning |
|-------|---------|
| `Base` | Sitting at the team's neutral spawn point |
| `Carried` | Held by a player (no world agent visible) |
| `Ground` | Dropped on the ground |
| `Stand` | Placed on the tower flagstand |

### FlagTimelineEventType (what happened)
| Value | Meaning |
|-------|---------|
| `Spawn` | Flag appears at base (match start, after stick, after return) |
| `Pickup` | Same-team player picks up the flag |
| `Drop` | Carrier drops the flag (death or manual) |
| `Stick` | Carrier places flag on flagstand |
| `Return` | Opposing team touches dropped flag, teleporting it to base |
| `GroundSpawn` | Flag appears on ground after drop |

### StandOwner
```
Neutral, Blue, Red
```

### Key Structures

- **`FlagTimelineEvent`** — One state change: time, team, event type, new location, actor/carrier agent IDs, world position, flag world agent ID, stand agent ID.
- **`FlagTeamTimeline`** — Per-team: base spawn position + sorted event list. Provides query functions for `locationAtTime()`, `carrierAtTime()`, `positionAtTime()`.
- **`StandControlEvent`** — Flagstand ownership change: time, owner, stand agent ID, morale expiry time.
- **`StandTimeline`** — Stand position + sorted control events. Provides `ownerAtTime()`.
- **`FlagTimeline`** — Top-level output: two `FlagTeamTimeline` entries (Blue/Red), one `StandTimeline`, a merged `allEvents` list, the set of all flag item IDs, and a `valid` flag.

---

## 5. Runtime Queries — Querying State at a Point in Time

All query functions use binary search to find the latest event at or before time `t`:

- **`FlagTeamTimeline::locationAtTime(t)`** — Returns the `FlagLocation` of this team's flag. Defaults to `Base` before any events.
- **`FlagTeamTimeline::carrierAtTime(t)`** — Returns the carrier's agent ID, or -1 if not carried.
- **`FlagTeamTimeline::positionAtTime(t)`** — Returns the world (x, y, z) of the flag. Falls back to the base spawn if before any events.
- **`StandTimeline::ownerAtTime(t)`** — Returns which team owns the flagstand, or `Neutral`.

---

## 6. 3D Map Rendering — `DrawFlags()`

`ReplayWindow::DrawFlags()` draws team-colored flag icons on the 3D map view using ImGui's foreground draw list. It runs every frame during replay playback.

### Prerequisites
- `m_flagTimelineBuilt` and `m_flagTimeline.valid` must be true
- Agents must be classified

### Icon Assets
Two PNG textures are loaded from `Textures/Others_UI/`:
- `Blue_flag_waving.svg.png`
- `Red_flag_waving.svg.png`

Icon size is `clamp(viewportHeight * 0.035, 18, 32)` pixels.

### Per-Flag Drawing (`DrawFlagAt` lambda)

For a given world position and team:
1. Apply the map transform (coordinate system conversion)
2. Project to screen coordinates via the camera's view-projection matrix
3. Draw a filled circle (team-colored dot, radius 5px) with a black outline
4. Draw the flag icon image above the dot (offset upward by `iconSz * 0.8`)
5. If a label is provided, draw it above the icon with a black text shadow

### Stand Rendering (captured flag on flagstand)

When `standOwner != Neutral`:
- Draw the capturing team's flag icon at the flagstand's world position
- Add a **pulsing glow** effect: a translucent circle behind the icon that oscillates in opacity using `sin(time * 1.8)`, with team-appropriate color (blue or red)

### Per-Team Active Flag

For each team (Blue, Red):
1. Query `locationAtTime(m_debugTimeline)`
2. **`Stand`** — Skip (already drawn above as the stand icon)
3. **`Carried`** — Get the carrier's interpolated position. If the carrier agent exists in the agent data, use `InterpolateAgentPosition()`. Otherwise fall back to the timeline's `positionAtTime()`. Label: "Flag (Carried)".
4. **`Ground`** — Use the timeline's `positionAtTime()`. Label: "Flag (Dropped)".
5. **`Base`** — Use the timeline's `positionAtTime()` (which returns base spawn coords). No label.
6. Call `DrawFlagAt()` with the resolved position.

---

## 7. Flag Event Messages — On-Screen Notifications

`DrawFlagEventMessages()` displays text notifications (similar to the in-game chat announcements) whenever flag events occur. Messages appear below the match timer and auto-fade after 5 seconds.

### Message Building (`BuildFlagMessages`)

Iterates all timeline events. Skips `Spawn` and `GroundSpawn` (these are silent). For each remaining event:
- Records the actor's player name and team
- Skips events with no player name (except Returns, which may be attributed to a team)

### Display Rules

- **Duration**: 5 seconds per message
- **Fade**: Starts fading at 4 seconds
- **Max visible**: 2 messages at a time (newest at bottom)
- Messages are rendered centered on screen, at a configurable position

### Message Text per Event Type

| Event | Display Text |
|-------|-------------|
| **Pickup** | `{player} picked up {team}'s team flag!` |
| **Drop** | `{player} has dropped {team}'s team flag!` |
| **Return** | `{player} has returned {team}'s team flag!` — or `{opposingTeam} team has returned {team}'s team flag!` if no actor was detected |
| **Stick** | Line 1: `{player} has taken control of the watchtower!` / Line 2: `{team} team will earn a morale boost every two minutes they hold the watchtower.` |

Text is colored: player names use their team color (blue/red), flag team names use the flag's team color, connective words are white.

---

## 8. Morale Boost Timers

`DrawMoraleBoostTimers()` displays a countdown timer for each team that currently controls the flagstand.

### Logic

1. Walk the `StandTimeline` events up to the current playback time to find the last capture per team and the overall owner
2. For the team that currently owns the stand, compute `secondsSince = currentTime - captureTime`
3. The morale timer cycles every 120 seconds: `remaining = 120 - (secondsSince % 120)`
4. Display as "Blue Morale Boost" / "Red Morale Boost" with a `MM:SS` countdown

### Rendering

- Team label in team color (blue/red)
- Timer value in gold `#F5E4B4`
- Both rendered with a double text shadow for readability
- Position is configurable via overlay drag handles

---

## 9. Party Bar Flag Icons

When drawing party health bars in the right panel (`DrawPartyHealthBar`), the code checks whether each player is currently carrying a flag:

```cpp
for (int fti = 0; fti < 2; fti++) {
    if (m_flagTimeline.teams[fti].carrierAtTime(m_debugTimeline) == agentId) {
        carriedFlagTex = LoadFlagIcon(dev, (fti == 0)
            ? "Blue_flag_waving.svg.png" : "Red_flag_waving.svg.png");
        break;
    }
}
```

If the player is carrying a flag, the flag icon is drawn alongside the player's condition/enchantment icons on their health bar. This appears in both the side-panel party bars and the focused-agent HUD.

---

## 10. Timeline Panel — Flag Pills and Icons

The match timeline visualization (`draw_timeline.cpp` area in `ReplayWindow.cpp`) integrates flag events:

### Filter Pills

The timeline offers toggle pills to show/hide event categories. Flag-related pills:
- **"Flag"** — Flag pickup/drop/stick events (yellow pill)
- **"Return"** — Flag return events (green pill)
- **"Morale"** — Morale boost events (gold pill, uses `bluemorale.png` / `redmorale.png`)

### Timeline Event Icons

Events are drawn as icons on the timeline chart:
- **`FlagCapture`** — Team-colored flag icon (`Blue_flag_waving.svg.png` or `Red_flag_waving.svg.png`)
- **`FlagReturn`** — Same flag icon, with a green circle drawn behind it as a return indicator
- **`MoraleBoost`** — Morale icon (`bluemorale.png` / `redmorale.png`)

---

## 11. Flag Debug Window

`DrawFlagDebugWindow()` provides a developer-facing ImGui window ("Flag Timeline") for inspecting the reconstructed flag state:

### Current State Summary
- Current playback time
- Per-team: flag location, world position, carrier agent ID, base spawn coordinates
- Flagstand ownership

### All Events Table
A scrollable table with columns: Time, Team, Event Type, Location, Actor, Carrier, Position, Flag Agent. Highlights events near the current playback time.

---

## 12. Interpolation Rules — Flags Never Interpolate

Flags (and spirits) are explicitly excluded from position interpolation:

```cpp
// Flags and Spirits never interpolate — snap to nearest recorded position
if (ard.type == AgentType::Flag || ard.type == AgentType::Spirit) {
    SnapAgentPosition(ard, t, outX, outY, outZ);
    return;
}
```

This is documented in `ReplayMapData.h`:
> Flags must never be interpolated — they snap to their exact recorded snapshot positions.

Flag agents are also excluded from:
- Raw snapshot debug dots
- MOVE_TO_POINT anchor visualization
- Interpolation debug lines
- Dead freeze indicators
- Casting freeze indicators

---

## 13. Agent Incarnation Splitting — Flags Are Skipped

When the replay system splits agents with recycled IDs into separate incarnation records, flag agents are excluded. The build step collects all flag agent IDs (from both the hardcoded `IsFlagItemId` table and StoC FLAG_ITEM data) into a skip set:

```cpp
if (iid != 0 && (IsFlagItemId(m_replayCtx.mapId, iid) || flagItemIds.count(iid)))
    flagAgentSkipIds.insert(id);
```

Flag state is fully handled by the `FlagTimelineBuilder` via StoC events, making per-agent incarnation tracking unnecessary and potentially harmful (flags reuse agent IDs by design as they are destroyed and respawned).

---

## Rendering Call Order

Within `ReplayWindow::Render()`, flag-related drawing happens in this order:

1. `DrawAgentOverlay()` — Agent cylinders/dots (flag agents are **skipped** here)
2. **`DrawFlags()`** — Flag icons on the 3D map
3. `DrawBundleItems()` — Repair kits, vine seeds, and other bundle items
4. `DrawMatchTimer()` — Match timer display
5. **`DrawFlagEventMessages()`** — On-screen flag event notifications
6. `DrawJumboMessages()` — "Blue Captured Tower" etc.
7. **`DrawMoraleBoostTimers()`** — Morale countdown timers
