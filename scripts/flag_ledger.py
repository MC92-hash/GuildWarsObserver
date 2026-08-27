"""Per-player flag carrying, from the GvG `flag_events` StoC stream.

Separate from combat_analytics because a flag run is not combat, but it emits
the same shape -- one numeric row per player per match -- so the counters ride
the existing per-player plumbing into the shard without new readers.

Unlike the interrupt stream, the carrier here is real: a pickup record names
the player agent directly, and across a 523-recording sample every pickup and
drop resolved to a player on the roster. What is NOT attributable is a flag
RETURN: that record carries a team and no agent, so returns are left to the
timeline stream at team level rather than guessed onto a player.
"""
from __future__ import annotations

import gzip
from pathlib import Path

# The same definition of "who is a player" the combat rows use. Shared rather
# than re-derived so the two cannot disagree about the roster.
from combat_analytics import _player_lookup as player_lookup

SCHEMA_VERSION = 1

# Record types on the multiplexed stream (EventHooks.cpp:989-1085).
PICKUP, DROP, STATE, ITEM, STAND, SPAWN, ANNOUNCE = 0, 1, 2, 3, 4, 5, 6

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
    """Per-player carry counters, or {} when the stream is absent.

    Absent is not zero: a recording without the stream returns nothing at all
    rather than a row of confident zeroes.
    """
    records = read_flag_records(match_dir)
    if not records:
        return {}
    players = player_lookup(infos)
    match_end = max(when for when, _fields in records)

    # Two passes, because a flag is declared by an ITEM record that may sit
    # after a pickup in file order, and because a stick or a return respawns
    # the flag as a NEW item id -- so a flag is tracked by which team's it is,
    # never by item id.
    team_of_item: dict[int, int] = {}
    for _when, fields in records:
        if fields[0] == ITEM and len(fields) >= 5 and fields[2] == FLAG_MODEL_ID:
            team_of_item[fields[1]] = fields[3]

    rows = {
        agent_id: {
            "flag_pickups": 0,
            "flag_drops": 0,
            "flag_carry_seconds": 0,
        }
        for agent_id in players
    }
    carrier: dict[int, tuple[int, float]] = {}
    legs = 0
    carry_untracked_agent = 0
    pickups_undeclared_item = 0
    closed_by = {"taken_over": 0, "respawn": 0, "drop": 0, "match_end": 0}

    def close(flag_team: int, when: float, why: str) -> None:
        nonlocal legs, carry_untracked_agent
        held = carrier.pop(flag_team, None)
        if held is None:
            return
        agent_id, since = held
        legs += 1
        closed_by[why] += 1
        if agent_id in rows:
            rows[agent_id]["flag_carry_seconds"] += max(0, round(when - since))
        else:
            carry_untracked_agent += 1

    for when, fields in records:
        kind = fields[0]
        if kind == ITEM and len(fields) >= 5 and fields[2] == FLAG_MODEL_ID:
            # A respawn means the previous flag is gone -- stuck at the stand
            # or returned -- so whoever was holding it is no longer holding it.
            close(fields[3], when, "respawn")
        elif kind == PICKUP and len(fields) >= 3:
            flag_team = team_of_item.get(fields[1])
            if flag_team is None:
                pickups_undeclared_item += 1
                continue
            close(flag_team, when, "taken_over")
            carrier[flag_team] = (fields[2], when)
            if fields[2] in rows:
                rows[fields[2]]["flag_pickups"] += 1
        elif kind == DROP and len(fields) >= 2:
            for flag_team, (agent_id, _since) in list(carrier.items()):
                if agent_id == fields[1]:
                    close(flag_team, when, "drop")
            if fields[1] in rows:
                rows[fields[1]]["flag_drops"] += 1

    for flag_team in list(carrier):
        close(flag_team, match_end, "match_end")

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
            "flag_items_declared": len(team_of_item),
            "flag_carry_legs": legs,
            "flag_carry_legs_untracked_agent": carry_untracked_agent,
            "flag_pickups_undeclared_item": pickups_undeclared_item,
            # A leg ends when the flag is next touched. A carrier who DIES
            # drops it with no record of its own, so the seconds it then spends
            # on the ground stay on their leg: `taken_over` is the share of
            # legs that end that way and is the size of that overcount.
            **{f"flag_leg_closed_{why}": count for why, count in closed_by.items()},
            "flag_match_end_seconds": round(match_end),
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
