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


def _snap(time="00:01.000", max_hp=500):
    fields = ["0"] * 46
    fields[9], fields[11] = "1", str(max_hp)
    fields[25:29] = "1", "2", "3", "4"
    return f"[{time}] " + ";".join(fields) + "\n"


def _two_players(tmp_path: Path, max_hp=500):
    agents = tmp_path / "Agents"
    agents.mkdir(exist_ok=True)
    for agent_id in (10, 20):
        (agents / f"{agent_id}.txt").write_text(
            "".join(_snap(t, max_hp) for t in ("00:00.000", "00:30.000")),
            encoding="utf-8")
    return {"parties": {
        "1": {"PLAYER": [{"id": 10, "player_number": 1}]},
        "2": {"PLAYER": [{"id": 20, "player_number": 9}]},
    }}


def _spike(result, player_number=1):
    for row in result["spike"]:
        if row["player_number"] == player_number:
            return row["spike_2s_max"]
    return 0


def test_a_heal_filed_as_damage_is_not_damage(tmp_path: Path):
    # Recordings before 2026-05 have no HEAL kind and write armor-ignoring heals
    # as positive-valued DAMAGE -- 371 of 2,375 archived matches, all April.
    # Branching on the record kind alone counted those heals as damage dealt.
    infos = _two_players(tmp_path)
    events = parse_events([
        "[00:01.000] DAMAGE;10;20;-0.10;16",
        "[00:01.500] DAMAGE;10;20;0.40;55",
    ])
    result = build_player_matrix(infos, events, tmp_path)
    edge = next(e for e in result["edges"] if e["source_player_number"] == 1)
    assert edge["damage"] == 50
    assert edge["healing"] == 200
    assert _spike(result) == 50


def test_spike_is_the_best_two_seconds_on_one_victim(tmp_path: Path):
    infos = _two_players(tmp_path)
    events = parse_events([
        "[00:01.000] DAMAGE;10;20;-0.10;16",   # 50, inside the window
        "[00:02.500] DAMAGE;10;20;-0.20;16",   # 100, inside
        "[00:09.000] DAMAGE;10;20;-0.30;16",   # 150, far outside
    ])
    # 50 + 100 within 1.5s beats the lone 150.
    assert _spike(build_player_matrix(infos, events, tmp_path)) == 150


def test_damage_spread_over_more_than_the_window_is_not_a_spike(tmp_path: Path):
    infos = _two_players(tmp_path)
    events = parse_events([f"[00:0{i}.000] DAMAGE;10;20;-0.10;16" for i in range(1, 8)])
    # Seven 50s, one per second: the best two seconds hold three of them.
    assert _spike(build_player_matrix(infos, events, tmp_path)) == 150


def test_spike_never_counts_two_victims_together(tmp_path: Path):
    # Cleave is not a spike. Two enemies hit in the same instant must not add.
    agents = tmp_path / "Agents"
    agents.mkdir(exist_ok=True)
    for agent_id in (10, 20, 21):
        (agents / f"{agent_id}.txt").write_text(
            "".join(_snap(t) for t in ("00:00.000", "00:30.000")), encoding="utf-8")
    infos = {"parties": {
        "1": {"PLAYER": [{"id": 10, "player_number": 1}]},
        "2": {"PLAYER": [{"id": 20, "player_number": 9},
                         {"id": 21, "player_number": 8}]},
    }}
    events = parse_events([
        "[00:01.000] DAMAGE;10;20;-0.20;16",
        "[00:01.000] DAMAGE;10;21;-0.20;16",
    ])
    assert _spike(build_player_matrix(infos, events, tmp_path)) == 100


def test_damage_to_a_teammate_is_not_a_spike(tmp_path: Path):
    agents = tmp_path / "Agents"
    agents.mkdir(exist_ok=True)
    for agent_id in (10, 11):
        (agents / f"{agent_id}.txt").write_text(
            "".join(_snap(t) for t in ("00:00.000", "00:30.000")), encoding="utf-8")
    infos = {"parties": {
        "1": {"PLAYER": [{"id": 10, "player_number": 1},
                         {"id": 11, "player_number": 2}]},
    }}
    events = parse_events(["[00:01.000] DAMAGE;10;11;-0.50;16"])
    assert _spike(build_player_matrix(infos, events, tmp_path)) == 0
