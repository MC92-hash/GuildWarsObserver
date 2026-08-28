"""Observed combat-table aggregates with explicit denominator labels."""
from __future__ import annotations

def build_combat_table(matrix: dict, *, wall_clock_seconds: float | None = None,
                       combat_time_seconds: float | None = None) -> dict:
    if not isinstance(matrix, dict): return {}
    rows = {}
    for edge in matrix.get("edges", ()):
        if not isinstance(edge, dict): continue
        key = (str(edge.get("source_party_id")), int(edge.get("source_player_number", 0)))
        row = rows.setdefault(key, {"damage": 0, "healing": 0, "damage_taken": 0,
                                    "damage_packets": 0, "healing_packets": 0,
                                    "damage_type_counts": {},
                                    "damage_type_derivable": False,
                                    "true_damage": None, "physical_damage": None,
                                    "elemental_damage": None})
        damage = max(0, int(edge.get("damage", 0)))
        healing = max(0, int(edge.get("healing", 0)))
        row["damage"] += damage; row["healing"] += healing
        row["damage_packets"] += max(0, int(edge.get("damage_packets", 0)))
        row["healing_packets"] += max(0, int(edge.get("healing_packets", 0)))
        for type_id, amount in (edge.get("damage_type_counts") or {}).items():
            row["damage_type_counts"][str(type_id)] = row["damage_type_counts"].get(str(type_id), 0) + max(0, int(amount))
        target = (str(edge.get("target_party_id")), int(edge.get("target_player_number", 0)))
        target_row = rows.setdefault(target, {"damage": 0, "healing": 0, "damage_taken": 0,
                                              "damage_packets": 0, "healing_packets": 0,
                                              "damage_type_counts": {},
                                              "damage_type_derivable": False,
                                              "true_damage": None, "physical_damage": None,
                                              "elemental_damage": None})
        target_row["damage_taken"] += damage
    for row in rows.values():
        counts = row.pop("damage_type_counts")
        row["true_damage"] = counts.get("55")
        row["armor_modified_damage"] = sum(value for key, value in counts.items() if key in {"16", "17"}) or None
        row["damage_type_counts"] = counts
        if wall_clock_seconds and wall_clock_seconds > 0:
            row["wall_clock_dps"] = round(row["damage"] / wall_clock_seconds, 2)
        if combat_time_seconds and combat_time_seconds > 0:
            row["combat_time_dps"] = round(row["damage"] / combat_time_seconds, 2)
    return {"schema": 1, "rows": [
        {"party_id": party, "player_number": number, **values}
        for (party, number), values in sorted(rows.items())
    ], "observed_split": {"true": None, "armor_modified": None,
                           "physical": None, "elemental": None,
                           "physical_elemental_derivable": False}}
