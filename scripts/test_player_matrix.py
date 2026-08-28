from pathlib import Path

from combat_analytics import parse_events
from player_matrix import build_player_matrix


def _line(max_hp=500):
    fields = ["0"] * 46
    fields[9], fields[11] = "1", str(max_hp)
    fields[25:29] = "1", "2", "3", "4"
    return "[00:01.000] " + ";".join(fields) + "\n"


def test_matrix_excludes_npcs_and_conserves_taken(tmp_path: Path):
    agents = tmp_path / "Agents"
    agents.mkdir()
    (agents / "10.txt").write_text(_line(), encoding="utf-8")
    (agents / "20.txt").write_text(_line(), encoding="utf-8")
    infos = {"parties": {
        "1": {"PLAYER": [{"id": 10, "player_number": 1}]},
        "2": {"PLAYER": [{"id": 20, "player_number": 9}]},
    }}
    events = parse_events([
        "[00:01.000] DAMAGE;10;20;-0.1;0",
        "[00:01.100] HEAL;20;20;0.04;55",
        "[00:01.200] DAMAGE;99;20;-0.2;0",
        "[00:01.300] DAMAGE;10;77;-0.3;0",
    ])
    result = build_player_matrix(infos, events, tmp_path)
    assert len(result["edges"]) == 2
    first, second = result["edges"]
    assert first["damage"] == 50 and first["damage_type_counts"] == {"0": 50}
    assert second["healing"] == 20 and second["damage"] == 0
    assert result["audit"]["included_damage"] == 50
    assert result["audit"]["excluded_unconfirmed_source"] == 1
    assert result["audit"]["excluded_unconfirmed_target"] == 1


def test_missing_snapshots_is_absent_not_zero(tmp_path: Path):
    infos = {"parties": {"1": {"PLAYER": [{"id": 10, "player_number": 1}]}}}
    assert build_player_matrix(infos, [], tmp_path) == {}
