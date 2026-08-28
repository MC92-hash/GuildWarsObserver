"""Per-player kills, split into enemy players and NPCs, for the whole archive.

``infos.json`` carries a ``kills`` counter that credits a killing blow on **any**
agent with no filter on what died -- measured, two thirds of a side's kills are
NPCs. The plugin later gained a ``player_kills`` counter gated on the victim, and
``EventHooks.cpp`` states that the split is forward-only: *"nothing can recover
it from a recording already made, because no stream carries a per-kill victim."*

**That is wrong, and this module is the counter-example.** Both halves of a kill
are in every recording ever made:

* **The death.** ``Agents/<id>.txt.gz`` field 9 ``is_dead`` transitions 0 to 1,
  and every agent gets a snapshot file -- NPCs, pets and minions included.
  Measured over 198 matches: **11,826 snapshot deaths against 11,826 summed
  ``deaths`` in infos.json. Exact.**
* **The killer.** ``EventHooks.cpp:630`` sets ``last_hit_by_`` from the same
  damage packets it writes to ``combat_events``, so the archived stream is a
  complete transcript of that counter's input. This is therefore not an
  inference in the sense the cripple and interrupt joins are -- the recording
  names the cause; only the map state at each instant has to be replayed.

Validated against the twelve matches that carry the live counter, over their 190
player rows: ``kills`` reproduced exactly for **184 (96.8%)** and
``player_kills`` for **187 (98.4%)**, with a total absolute error of 11 and 4
respectively. The residual is credit shifting inside a damage spike, because the
snapshot lags the true death by up to 100 ms and a hit landed after the killing
blow can win. It does not net out to nothing, but it is small and it is in both
directions rather than systematically flattering anybody.

Two rules earn their place, both measured:

* **Only negative-value damage counts.** 371 April 2026 matches predate the
  ``HEAL`` split and file heals as positive-valued ``DAMAGE``. The filter halves
  the error, 3.30% to 1.85%.
* **``AGENT_REMOVE`` is not death.** It means "left compass range" -- 0%
  precision and 0% recall against real deaths over 60 matches. Do not use it.
"""
from __future__ import annotations

import gzip
from collections import defaultdict
from pathlib import Path

from combat_analytics import _player_lookup as player_lookup
from condition_ledger import (INCARNATION_BREAK, _stream,
                              read_snapshot_records)
from lord_pressure import MODEL_NAMES

SCHEMA_VERSION = 1

# ``is_dead``, zero-based, in the 50-field snapshot line. Field 9 in the
# one-based table in EXPORTS_CONVENTIONS.md.
IS_DEAD_FIELD = 8
MIN_SNAPSHOT_FIELDS = 9

# Guild-hall NPCs, from `lord_pressure.MODEL_NAMES` rather than a second copy.
# Everything else that dies is pooled as "summon".
#
# Deliberately NOT broken down further. The archive holds 66 distinct non-hall
# model ids, and nothing observable separates a pet from a minion from a spirit:
# deaths-per-agent does not (Bone Horror 2.91 sits among the pets at 1.3-3.4) and
# neither does level. A pet/minion/spirit split would be curation invented here
# and presented as measurement, which is the one thing these ledgers do not do.
# The hall roster is what a GvG turns on anyway -- killing archers and knights is
# split pressure, killing the lord is the match.
SUMMON_ROLE = "summon"

# The hall roster proper. `lord_pressure.MODEL_NAMES` supplies the NAMES -- one
# table, not two -- but it also carries Bone Horror (2280), which is a minion
# somebody animated and not part of any guild hall. Left to that table, one
# minion model would get a bucket of its own while the other 65 shared "summon",
# which is a distinction the data does not support.
HALL_MODELS = frozenset({168, 170, 172, 173, 174, 175, 176})

# An `OTHER` entry below this model id is a PLAYER the recorder misfiled: 247
# agents archive-wide whose `IsPlayer()` was false at first capture. They carry
# full skill bars and level 20. Counting them as NPCs would be wrong in the
# direction that flatters everybody.
LOWEST_NPC_MODEL = 170


def _role_of(model_id: int) -> str:
    if model_id in HALL_MODELS:
        return MODEL_NAMES.get(model_id, SUMMON_ROLE)
    return SUMMON_ROLE


def agent_roster(infos: dict) -> tuple[dict, dict]:
    """(players, npcs) -- agent id to identity, for everything worth tracking.

    ``players`` is the shared ``_player_lookup`` so the two can never disagree
    about who is a player. ``npcs`` is every ``OTHER`` entry that is really an
    NPC, keyed to its role.
    """
    players = player_lookup(infos)
    npcs: dict[int, str] = {}
    parties = infos.get("parties")
    if not isinstance(parties, dict):
        return players, npcs
    for party in parties.values():
        if not isinstance(party, dict):
            continue
        for other in party.get("OTHER", ()):
            if not isinstance(other, dict):
                continue
            agent_id = other.get("id")
            model_id = other.get("model_id")
            if not isinstance(agent_id, int) or not isinstance(model_id, int):
                continue
            if agent_id in players or model_id < LOWEST_NPC_MODEL:
                continue
            npcs[agent_id] = _role_of(model_id)
    return players, npcs


def death_times(records) -> list[float]:
    """When an agent died, from ``is_dead`` going 0 to 1."""
    out: list[float] = []
    was_dead = False
    for when, fields in records:
        if not fields:
            continue
        if fields[0].startswith(INCARNATION_BREAK):
            was_dead = False
            continue
        if len(fields) < MIN_SNAPSHOT_FIELDS:
            continue
        dead = fields[IS_DEAD_FIELD] == "1"
        if dead and not was_dead:
            out.append(when)
        was_dead = dead
    return out


def read_damage(match_dir: Path) -> dict[int, list[tuple[float, int]]]:
    """victim -> [(time, attacker)] for every damaging packet, in time order."""
    path = _stream(match_dir, "combat_events")
    if path is None:
        return {}
    opener = gzip.open if path.suffix.lower() == ".gz" else open
    hits: dict[int, list[tuple[float, int]]] = defaultdict(list)
    with opener(path, "rt", encoding="utf-8-sig", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            close = line.find("]")
            if not line.startswith("[") or close < 0:
                continue
            head, body = line[1:close], line[close + 1:].strip()
            parts = body.split(";")
            if parts[0] != "DAMAGE" or len(parts) < 4:
                continue
            try:
                minute, rest = head.split(":", 1)
                second, millis = rest.split(".", 1) if "." in rest else (rest, "0")
                when = int(minute) * 60 + int(second) + int(millis) / 1000.0
                cause, victim = int(parts[1]), int(parts[2])
                value = float(parts[3])
            except (TypeError, ValueError):
                continue
            # Positive is a heal filed under DAMAGE by the April recorders.
            if value >= 0 or not cause or not victim:
                continue
            hits[victim].append((when, cause))
    for series in hits.values():
        series.sort()
    return hits


def _killer(hits: list[tuple[float, int]], when: float) -> int | None:
    """The last agent to damage this victim at or before their death."""
    found = None
    for hit_time, cause in hits:
        if hit_time > when:
            break
        found = cause
    return found


def build_kill_ledger(infos: dict, match_dir: Path,
                      records: dict | None = None) -> dict:
    """Per-player ``player_kills`` and ``npc_kills``, or {} when unreadable."""
    players, npcs = agent_roster(infos)
    if not players:
        return {}
    if records is None:
        records = read_snapshot_records(match_dir, list(players) + list(npcs))
    if not records:
        return {}
    hits = read_damage(match_dir)
    if not hits:
        return {}

    rows = {agent_id: {"player_kills": 0, "npc_kills": 0}
            for agent_id in players}
    roles = sorted({MODEL_NAMES[m] for m in HALL_MODELS if m in MODEL_NAMES}
                   | {SUMMON_ROLE})
    for row in rows.values():
        for role in roles:
            row[f"npc_kills_{role.lower().replace(' ', '_')}"] = 0

    deaths = credited = unattributed = 0
    for agent_id, series in records.items():
        is_player = agent_id in players
        if not is_player and agent_id not in npcs:
            continue
        role = None if is_player else npcs[agent_id]
        for when in death_times(series):
            deaths += 1
            killer = _killer(hits.get(agent_id, ()), when)
            if killer is None or killer not in rows:
                unattributed += 1
                continue
            credited += 1
            if is_player:
                rows[killer]["player_kills"] += 1
            else:
                rows[killer]["npc_kills"] += 1
                key = f"npc_kills_{role.lower().replace(' ', '_')}"
                if key in rows[killer]:
                    rows[killer][key] += 1

    # Where the live counter exists it is a regression test, not a competitor.
    live = {}
    for party in infos.get("parties", {}).values():
        for player in party.get("PLAYER", ()):
            if isinstance(player, dict) and "player_kills" in player:
                live[player.get("id")] = player["player_kills"]
    disagreements = sum(
        1 for agent_id, value in live.items()
        if agent_id in rows and rows[agent_id]["player_kills"] != value
    )

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
            "kill_deaths_seen": deaths,
            "kill_deaths_credited": credited,
            # Deaths with no damage packet before them. The live counter skips
            # these too -- `EventHooks.cpp` only increments when `last_hit_by_`
            # has an entry -- so both numbers share a denominator. Dominated by
            # pets and minions expiring out of compass range.
            "kill_deaths_unattributed": unattributed,
            "kill_npc_agents_tracked": len(npcs),
            "kill_live_counter_rows": len(live),
            "kill_live_counter_disagreements": disagreements,
        },
    }
