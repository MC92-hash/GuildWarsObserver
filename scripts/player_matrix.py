"""Observed player-to-player combat packet matrix for Watchtower."""

from __future__ import annotations

from collections import defaultdict
from pathlib import Path

from max_hp_solver import (Observation, correct_max_hp_for_packet, cpp_round,
                           max_hp_at_time, read_snapshots, snapshot_at,
                           solve_observations)

MATRIX_SCHEMA = 1


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


def build_player_matrix(infos: dict, events, match_dir: Path) -> dict:
    players = confirmed_players(infos)
    if not players:
        return {}

    snapshots = {}
    for agent_id in players:
        path = _snapshot_path(match_dir, agent_id)
        if path is not None:
            rows = read_snapshots(path)
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
        if event.kind == "DAMAGE":
            edges[key]["damage"] += amount
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
    return {"schema": MATRIX_SCHEMA, "edges": output, "audit": dict(audit)}
