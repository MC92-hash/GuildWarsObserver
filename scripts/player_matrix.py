"""Observed player-to-player combat packet matrix for Watchtower.

Also computes **damage compression**: the most damage a player put on a single
enemy player inside any short window. Every ingredient was already here -- the
packets are millisecond-stamped and this module already converts each one to
absolute HP -- and only the timestamp was being thrown away. It lives here
rather than in a module of its own because ``upload_to_r2.py`` wraps the whole
analytics call in a broad ``except``, so a new module that failed to import
would cost the match every counter it has, not just this one.
"""

from __future__ import annotations

from collections import defaultdict
from pathlib import Path

from max_hp_solver import (Observation, correct_max_hp_for_packet, cpp_round,
                           max_hp_at_time, read_snapshots, snapshot_at,
                           solve_observations)

MATRIX_SCHEMA = 1

# The burst window, in seconds.
#
# Two, not three, and it was measured. Best window on a single enemy player,
# median by profession over 45 archived matches, Assassin against Warrior:
#
#     1 s   188 vs 172   (+9%)        3 s   294 vs 270   (+9%)
#     2 s   250 vs 219   (+14%)       5 s   346 vs 328   (+5%)
#
# The separation peaks at two seconds and decays as the window widens toward
# what `damage_per_min` already measures -- which is the burst-versus-sustained
# split this metric exists to capture. Change this and the counter name changes
# with it, because a stored `spike_2s_max` that silently became three seconds
# would be unreadable next to the archive that came before it.
SPIKE_WINDOW_SECONDS = 2.0
SPIKE_FIELD = "spike_2s_max"


def _integer(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def confirmed_players(infos: dict) -> dict[int, tuple[str, int]]:
    """Agent id -> stable sidecar identity; NPCs are never inferred as players."""
    out = {}
    parties = infos.get("parties")
    if not isinstance(parties, dict):
        return out
    for party_id, party in parties.items():
        if not isinstance(party, dict):
            continue
        for player in party.get("PLAYER", ()):
            if not isinstance(player, dict):
                continue
            agent_id = _integer(player.get("id"))
            number = _integer(player.get("player_number"))
            if agent_id > 0 and number > 0:
                out[agent_id] = (str(party_id), number)
    return out


def _snapshot_path(match_dir: Path, agent_id: int) -> Path | None:
    for path in (match_dir / "Agents" / f"{agent_id}.txt.gz",
                 match_dir / "Agents" / f"{agent_id}.txt"):
        if path.is_file():
            return path
    return None


def build_player_matrix(infos: dict, events, match_dir: Path,
                        records: dict | None = None) -> dict:
    """The damage matrix. ``records`` is pre-read snapshot lines when a caller
    already has them, so a match is decompressed once rather than once per
    consumer -- see ``combat_analytics.build_from_match_dir``."""
    from max_hp_solver import snapshot_from_fields

    players = confirmed_players(infos)
    if not players:
        return {}

    snapshots = {}
    for agent_id in players:
        if records is not None:
            rows = tuple(
                snapshot for when, fields in records.get(agent_id, ())
                if (snapshot := snapshot_from_fields(when, fields)) is not None
            )
        else:
            path = _snapshot_path(match_dir, agent_id)
            rows = read_snapshots(path) if path is not None else ()
        if rows:
            snapshots[agent_id] = rows
    if not snapshots:
        return {}

    packet_events = [event for event in events
                     if event.kind in {"DAMAGE", "HEAL"} and len(event.fields) >= 4]
    solved = {}
    for target_id, rows in snapshots.items():
        observations = []
        for event in packet_events:
            if _integer(event.fields[1]) != target_id:
                continue
            snap = snapshot_at(rows, event.time)
            if snap is not None:
                observations.append(Observation(
                    event.time, float(event.fields[2]), snap.weapon_set_key,
                    max_hp_at_time(rows, event.time), snap.has_deep_wound))
        solved[target_id] = solve_observations(observations)

    edges = defaultdict(lambda: {"damage": 0, "healing": 0,
                                "damage_packets": 0, "healing_packets": 0,
                                "damage_type_counts": {}})
    # (time, amount) per attacker-victim pair, enemy players only, for the
    # window scan below. Collected in the loop that already converts every
    # packet to absolute HP, so it costs one append and no extra file read.
    spikes: dict[tuple[int, int], list[tuple[float, int]]] = defaultdict(list)
    audit = defaultdict(int)
    for event in packet_events:
        audit["packet_events"] += 1
        source_id, target_id = _integer(event.fields[0]), _integer(event.fields[1])
        if source_id not in players:
            audit["excluded_unconfirmed_source"] += 1
            continue
        if target_id not in players:
            audit["excluded_unconfirmed_target"] += 1
            continue
        rows = snapshots.get(target_id)
        snap = snapshot_at(rows or (), event.time)
        if snap is None:
            audit["excluded_missing_target_snapshots"] += 1
            continue
        max_hp = max_hp_at_time(rows, event.time)
        source = "camera"
        if max_hp <= 0:
            record = solved.get(target_id, {}).get(snap.weapon_set_key)
            if record is not None and record.accepted:
                max_hp, source = record.max_hp, "lattice"
        if max_hp <= 0:
            audit["excluded_unresolved_max_hp"] += 1
            continue
        fraction = float(event.fields[2])
        max_hp = correct_max_hp_for_packet(max_hp, fraction)
        amount = cpp_round(abs(fraction) * max_hp)
        key = (source_id, target_id)
        # The SIGN decides, not the record kind. Recordings before 2026-05 have
        # no `HEAL` kind at all and file armor-ignoring heals as positive-valued
        # `DAMAGE` -- 371 of 2,375 archived matches, every one of them April
        # 2026. Branching on `event.kind` alone counted those heals as damage.
        healing = fraction > 0
        if event.kind == "DAMAGE" and not healing:
            edges[key]["damage"] += amount
            if players[source_id][0] != players[target_id][0]:
                spikes[key].append((event.time, amount))
            edges[key]["damage_packets"] += 1
            audit["included_damage"] += amount
            type_counts = edges[key]["damage_type_counts"]
            type_name = str(_integer(event.fields[3]))
            type_counts[type_name] = type_counts.get(type_name, 0) + amount
        else:
            edges[key]["healing"] += amount
            edges[key]["healing_packets"] += 1
            audit["included_healing"] += amount
        audit[f"included_hp_source_{source}"] += 1

    output = []
    for (source_id, target_id), values in sorted(edges.items()):
        source_party, source_number = players[source_id]
        target_party, target_number = players[target_id]
        output.append({"source_party_id": source_party,
                       "source_player_number": source_number,
                       "target_party_id": target_party,
                       "target_player_number": target_number, **values})
    return {"schema": MATRIX_SCHEMA, "edges": output,
            "spike": spike_rows(spikes, players), "audit": dict(audit)}


def best_window(hits: list[tuple[float, int]],
                window: float = SPIKE_WINDOW_SECONDS) -> int:
    """Most damage inside any ``window``-second span of one attacker-victim pair.

    Two pointers over the packets in time order, so it is linear in hits rather
    than quadratic in windows. The span is inclusive at both ends: two packets
    exactly ``window`` apart are one burst.
    """
    if not hits:
        return 0
    ordered = sorted(hits)
    best = run = 0
    low = 0
    for high, (when, amount) in enumerate(ordered):
        run += amount
        while ordered[low][0] < when - window:
            run -= ordered[low][1]
            low += 1
        if run > best:
            best = run
    return best


def spike_rows(spikes: dict, players: dict) -> list[dict]:
    """Per-player best burst against a SINGLE enemy, keyed for the shard merge.

    Deliberately NOT hung off a matrix edge: ``stats_index.parse_shard``
    whitelists edge fields and would drop an unknown one silently. This is
    emitted as its own block and merged onto the per-player analytics row, where
    the parser copies any numeric key.
    """
    best: dict[int, int] = defaultdict(int)
    for (source_id, _target_id), hits in spikes.items():
        value = best_window(hits)
        if value > best[source_id]:
            best[source_id] = value
    rows = []
    for source_id, value in sorted(best.items()):
        party_id, player_number = players[source_id]
        rows.append({"party_id": party_id, "player_number": player_number,
                     SPIKE_FIELD: value})
    return rows
