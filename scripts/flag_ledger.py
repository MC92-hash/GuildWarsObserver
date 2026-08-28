"""Per-player flag carrying, from the GvG `flag_events` StoC stream.

Separate from combat_analytics because a flag run is not combat, but it emits
the same shape -- one numeric row per player per match -- so the counters ride
the existing per-player plumbing into the shard without new readers.

Unlike the interrupt stream, the carrier here is real: a pickup record names
the player agent directly, and across a 523-recording sample every pickup and
drop resolved to a player on the roster.

**Returns cannot be attributed to a player, and this is now measured rather
than assumed.** The desktop tool does attribute them -- ``FlagTimelineBuilder.cpp``
looks for a player of the returning team playing one of three pickup animations
within a second of the announce and within 200 units of the flag. That was
ported here in full, using the flag agent's OWN recorded position rather than an
approximation, and it does not survive contact with the archive:

* **The three animation constants never fire on a returner.** Of every animation
  shown by an opposing player standing within 200 units of a returned flag,
  none is 3002646805, 3002646795 or 3002646789. The codes that do appear are
  different values entirely (3259108696, 3580925846, 3259067526 ...). Separately,
  those three constants fire about 78 times per return match-wide, so they are a
  common animation rather than a pickup one.
* **Proximity alone only reaches 40%.** The median distance from a returned flag
  to the nearest opposing player is 379 units, well outside the 200-unit gate.
* Attribution therefore credits **1 return in 169** over 120 matches spread
  across the archive.

An earlier pass reported 42.6% coverage. That figure was wrong: it searched
every player rather than the returning team, so a teammate hovering near their
own dropped flag counted as the returner. Filtering to the correct side, as the
C++ does, collapses it.

Deriving a corrected animation set is possible in principle and is deliberately
not attempted, because nothing in the archive says who actually returned a flag
-- a new constant list could be fitted but never validated, which is curation
presented as measurement.

So returns stay a team-level fact, counted and published as such. Sticks are a
different matter and are attributed: the sticker is whoever was holding when the
announce fired, which is named by the record rather than inferred.
"""
from __future__ import annotations

import gzip
from pathlib import Path

# The same definition of "who is a player" the combat rows use. Shared rather
# than re-derived so the two cannot disagree about the roster.
from combat_analytics import _player_lookup as player_lookup
from combat_analytics import match_window

SCHEMA_VERSION = 1

# Record types on the multiplexed stream (EventHooks.cpp:989-1085).
PICKUP, DROP, STATE, ITEM, STAND, SPAWN, ANNOUNCE = 0, 1, 2, 3, 4, 5, 6
# The two actions an ANNOUNCE carries.
RETURN_ACTION, STICK_ACTION = 0, 1

# Which team owns which flag. Derived from the archive rather than read off a
# doc: `FlagTimelineBuilder.h:14` says Red=0/Blue=1 and
# `FlagRenderingAndState.md:202` says the opposite, so one of them is stale and
# neither was worth trusting. A flagger runs their OWN flag, so the pickup
# record settles it -- over 126 matches, 59808 was picked up by team 1 1,079
# times against 4, and 57400 by team 2 1,022 against 10.
#
# Nothing reads this today. It is kept because it is measured, it resolves a
# documented contradiction, and any future work on returns needs it.
FLAG_OWNER_TEAM = {59808: 1, 57400: 2}



# Every flag declared in the archive is model 493; nothing else is ever
# declared. 8.5% of pickups are of an UNdeclared item -- Warrior's Isle repair
# kits and Druid's Isle vine seeds ride the same packet -- so a pickup counts
# only when its item was declared by an ITEM record first. Without that filter
# a repair-kit run reads as a flag run.
FLAG_MODEL_ID = 493


def _seconds(header: str) -> float | None:
    try:
        minute, rest = header.split(":", 1)
        second, millis = rest.split(".", 1) if "." in rest else (rest, "0")
        return int(minute) * 60 + int(second) + int(millis) / 1000.0
    except (ValueError, TypeError):
        return None


def read_flag_records(match_dir: Path) -> list[tuple[float, list[int]]]:
    """(time, numeric fields) for the flag stream, or [] when it is absent."""
    stoc = match_dir / "StoC"
    path = next((p for p in (stoc / "flag_events.txt.gz", stoc / "flag_events.txt")
                 if p.is_file()), None)
    if path is None:
        return []
    opener = gzip.open if path.suffix.lower() == ".gz" else open
    records: list[tuple[float, list[int]]] = []
    with opener(path, "rt", encoding="utf-8-sig", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            close = line.find("]")
            if not line.startswith("[") or close < 0:
                continue
            when = _seconds(line[1:close])
            if when is None:
                continue
            fields = [token.strip() for token in line[close + 1:].strip().split(";")]
            if not fields or not fields[0].isdigit():
                continue
            try:
                records.append((when, [int(token or 0) for token in fields]))
            except ValueError:
                continue
    return records





def build_flag_ledger(infos: dict, match_dir: Path) -> dict:
    """Per-player carry, stick and return counters, or {} when absent.

    Absent is not zero: a recording without the stream returns nothing at all
    rather than a row of confident zeroes.
    """
    records = read_flag_records(match_dir)
    if not records:
        return {}
    players = player_lookup(infos)
    stream_end = max(when for when, _fields in records)

    # Legs are clipped to the MATCH, which is a window inside instance time
    # rather than [0, duration]: the instance is created about a minute before a
    # GvG starts, and players run the flag out of the base during that setup.
    # Measured, 14.1% of all pickups happen before the match does, 9.9% of
    # published carry time was warm-up, and twelve player rows credited more
    # carry seconds than their own match lasted -- which the avatar ledger's
    # `match_seconds`, merged into these same rows, turns into a visible
    # contradiction.
    start, end = match_window(infos)
    if end <= start:
        start, end = 0.0, stream_end

    def clip(value: float) -> float:
        return min(max(value, start), end)

    # An item id to the type-3 `extra_id` naming which flag it is: 59808 blue,
    # 57400 red. NOT a team code -- the pickup record carries a team field that
    # is a dead zero and the announce record carries a 1/2 team, so three
    # different fields in this one stream would otherwise answer to that name.
    #
    # Filled DURING the chronological pass, never in a pre-pass. A pre-pass is
    # last-declaration-wins, and item ids are recycled and re-declared for the
    # OTHER flag mid-match: measured, that happens in 66% of matches and
    # mis-assigns 25.5% of every pickup, in the worst case swapping two players
    # flag time outright. The pre-pass existed to rescue pickups whose ITEM
    # record arrives later; that is 0.8% of pickups, and sampling shows each one
    # is a repair kit picked up before its id was recycled into a real flag, so
    # the rescue was itself a false positive.
    flag_id_of_item: dict[int, int] = {}

    rows = {
        agent_id: {
            "flag_pickups": 0,
            "flag_drops": 0,
            "flag_carry_seconds": 0,
            "flag_sticks": 0,
        }
        for agent_id in players
    }
    returns_seen = 0
    sticks_seen = sticks_credited = 0
    carrier: dict[int, tuple[int, float]] = {}
    legs = 0
    carry_untracked_agent = 0
    pickups_undeclared_item = 0
    closed_by = {"taken_over": 0, "respawn": 0, "drop": 0, "match_end": 0}

    def close(flag_id: int, when: float, why: str) -> bool:
        """End the leg on one flag. True when a leg was actually open."""
        nonlocal legs, carry_untracked_agent
        held = carrier.pop(flag_id, None)
        if held is None:
            return False
        agent_id, since = held
        legs += 1
        closed_by[why] += 1
        if agent_id in rows:
            rows[agent_id]["flag_carry_seconds"] += max(
                0, round(clip(when) - clip(since)))
        else:
            carry_untracked_agent += 1
        return True

    for when, fields in records:
        kind = fields[0]
        if kind == ITEM and len(fields) >= 5 and fields[2] == FLAG_MODEL_ID:
            # A respawn means the previous flag is gone -- stuck at the stand
            # or returned -- so whoever was holding it is no longer holding it.
            close(fields[3], when, "respawn")
            flag_id_of_item[fields[1]] = fields[3]
        elif kind == PICKUP and len(fields) >= 3:
            flag_id = flag_id_of_item.get(fields[1])
            if flag_id is None:
                pickups_undeclared_item += 1
                continue
            close(flag_id, when, "taken_over")
            # Nobody carries two flags. Without this an agent can hold both and
            # the two legs overlap, counting the same seconds twice -- 17
            # occurrences before the recycling fix above, 1 after it.
            for other_id, (agent_id, _since) in list(carrier.items()):
                if agent_id == fields[2]:
                    close(other_id, when, "drop")
            carrier[flag_id] = (fields[2], when)
            if fields[2] in rows:
                rows[fields[2]]["flag_pickups"] += 1
        elif kind == DROP and len(fields) >= 2:
            # Only a drop that ENDS a carry is a flag drop. 9.3% of DROP records
            # close no leg: they are the repair kits and vine seeds riding this
            # packet, which `flag_pickups` already excludes. Counting them here
            # made the two counters describe different populations.
            dropped = False
            for flag_id, (agent_id, _since) in list(carrier.items()):
                if agent_id == fields[1]:
                    dropped = close(flag_id, when, "drop") or dropped
            if dropped and fields[1] in rows:
                rows[fields[1]]["flag_drops"] += 1

        elif kind == ANNOUNCE and len(fields) >= 4:
            action = fields[1]
            if action == STICK_ACTION:
                # The sticker is whoever was holding. Named directly, never
                # inferred -- this half was always available.
                sticks_seen += 1
                for _flag_id, (agent_id, _since) in carrier.items():
                    if agent_id in rows:
                        rows[agent_id]["flag_sticks"] += 1
                        sticks_credited += 1
                        break
            elif action == RETURN_ACTION:
                returns_seen += 1
                # The announce names the team whose flag was returned, so there
                # is no need to guess which flag it was. Its position comes from
                # the flag's own snapshot; the dropper's last position is only a
                # fallback for a flag with no track of its own.
                # Team level only. See the module docstring for why the
                # animation heuristic does not port.

    for flag_id in list(carrier):
        close(flag_id, end, "match_end")

    output: dict[str, list[dict[str, int]]] = {}
    for agent_id, (party_id, player_number) in players.items():
        output.setdefault(party_id, []).append(
            {"player_number": player_number, **rows[agent_id]}
        )
    for party_rows in output.values():
        party_rows.sort(key=lambda row: row["player_number"])

    return {
        "schema": SCHEMA_VERSION,
        "players": output,
        "attribution": {
            "flag_records": len(records),
            "flag_items_declared": len(flag_id_of_item),
            "flag_match_seconds": round(end - start),
            # Returns are counted, never attributed to a player.
            "flag_returns_seen": returns_seen,
            "flag_sticks_seen": sticks_seen,
            "flag_sticks_credited": sticks_credited,
            "flag_carry_legs": legs,
            "flag_carry_legs_untracked_agent": carry_untracked_agent,
            "flag_pickups_undeclared_item": pickups_undeclared_item,
            # A leg ends when the flag is next touched. A carrier who DIES
            # drops it with no record of its own, so the seconds it then spends
            # on the ground stay on their leg: `taken_over` is the share of
            # legs that end that way and is the size of that overcount.
            **{f"flag_leg_closed_{why}": count for why, count in closed_by.items()},
            # Instance time, not match time. This stream runs on the same clock
            # as every other timestamp in a recording, and that clock starts
            # about a minute before the match. Named so nobody divides by it.
            "flag_stream_end_instance_seconds": round(stream_end),
        },
    }


def merge_into_analytics(analytics: dict, ledger: dict) -> dict:
    """Fold flag counters into the combat rows, keyed by party and slot.

    The shard parser copies whatever numeric keys an analytics row carries, so
    merging here is what lets flag counters reach the consumer with no change
    at any layer in between.
    """
    if not analytics or not ledger:
        return analytics
    by_slot = {
        (party_id, row.get("player_number")): row
        for party_id, party_rows in ledger.get("players", {}).items()
        for row in party_rows
    }
    for party_id, party_rows in analytics.get("players", {}).items():
        for row in party_rows:
            extra = by_slot.get((party_id, row.get("player_number")))
            if extra:
                row.update(
                    {k: v for k, v in extra.items() if k != "player_number"}
                )
    attribution = analytics.setdefault("attribution", {})
    attribution.update(ledger.get("attribution", {}))
    return analytics
