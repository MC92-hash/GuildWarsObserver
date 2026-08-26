from combat_table import build_combat_table

def test_table_has_labeled_dps_and_taken_conservation():
    result = build_combat_table({"edges": [{
        "source_party_id": "1", "source_player_number": 1,
        "target_party_id": "2", "target_player_number": 9,
        "damage": 100, "healing": 20, "damage_packets": 2,
        "healing_packets": 1, "damage_type_counts": {"16": 80, "55": 20}}]}, wall_clock_seconds=10, combat_time_seconds=5)
    source, target = result["rows"]
    assert source["damage"] == 100 and source["wall_clock_dps"] == 10
    assert source["combat_time_dps"] == 20 and target["damage_taken"] == 100
    assert source["true_damage"] == 20 and source["armor_modified_damage"] == 80

def test_physical_elemental_split_is_explicitly_unavailable():
    result = build_combat_table({"edges": []})
    assert result["observed_split"]["physical_elemental_derivable"] is False
    assert result["observed_split"]["physical"] is None
