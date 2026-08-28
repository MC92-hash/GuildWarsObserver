from kill_ledger import agent_roster, build_kill_ledger, death_times

# A snapshot line with `is_dead` at index 8. Only that field is read.
_HEAD = "0.000;0.000;0.000;0.000;0;1;0;1"
_TAIL = ";".join("0" for _ in range(41))


def _snapshot(time: str, dead: int) -> str:
    return f"[{time}] {_HEAD};{dead};{_TAIL}"


def _infos(**extra):
    return {
        "parties": {
            "1": {"PLAYER": [{"id": 10, "player_number": 1},
                             {"id": 11, "player_number": 2}],
                  "OTHER": [{"id": 30, "model_id": 170},     # guild lord
                            {"id": 31, "model_id": 175},     # archer
                            {"id": 32, "model_id": 4278}]},  # a pet
            "2": {"PLAYER": [{"id": 20, "player_number": 9}],
                  "OTHER": []},
        },
        **extra,
    }


def _agents(tmp_path, agent_id, *lines):
    agents = tmp_path / "Agents"
    agents.mkdir(exist_ok=True)
    (agents / f"{agent_id}.txt").write_text(
        "".join(line + "\n" for line in lines), encoding="utf-8")
    return tmp_path


def _combat(tmp_path, *lines):
    stoc = tmp_path / "StoC"
    stoc.mkdir(exist_ok=True)
    (stoc / "combat_events.txt").write_text(
        "".join(line + "\n" for line in lines), encoding="utf-8")
    return tmp_path


def _rows(result):
    return {row["player_number"]: row
            for party in result["players"].values() for row in party}


def test_death_is_the_is_dead_transition(tmp_path):
    _agents(tmp_path, 20,
            _snapshot("00:05.000", 0),
            _snapshot("00:10.000", 1),
            _snapshot("00:40.000", 0),
            _snapshot("00:55.000", 1))
    from condition_ledger import _records
    assert death_times(_records(tmp_path / "Agents" / "20.txt")) == [10.0, 55.0]


def test_a_recycled_agent_id_does_not_fabricate_a_death(tmp_path):
    # The recorder writes INCARNATION_BREAK when an id is reused. Without the
    # reset the new agent starts life already flagged dead and the next 0->1
    # never fires -- or worse, a stale 1 reads as a fresh death.
    _agents(tmp_path, 20,
            _snapshot("00:10.000", 1),
            "# INCARNATION_BREAK",
            _snapshot("00:20.000", 1))
    from condition_ledger import _records
    assert death_times(_records(tmp_path / "Agents" / "20.txt")) == [10.0, 20.0]


def test_the_last_attacker_before_the_death_is_credited(tmp_path):
    _agents(tmp_path, 20, _snapshot("00:05.000", 0), _snapshot("00:10.000", 1))
    _combat(tmp_path,
            "[00:07.000] DAMAGE;11;20;-0.20;16",
            "[00:09.000] DAMAGE;10;20;-0.30;16",
            "[00:12.000] DAMAGE;11;20;-0.10;16")
    rows = _rows(build_kill_ledger(_infos(), tmp_path))
    assert rows[1]["player_kills"] == 1
    assert rows[2]["player_kills"] == 0


def test_a_heal_filed_as_damage_never_credits_a_kill(tmp_path):
    # April 2026 recordings have no HEAL kind and write armor-ignoring heals as
    # positive-valued DAMAGE. Reading those as the killing blow credited the
    # monk who healed the victim last.
    _agents(tmp_path, 20, _snapshot("00:05.000", 0), _snapshot("00:10.000", 1))
    _combat(tmp_path,
            "[00:07.000] DAMAGE;10;20;-0.30;16",
            "[00:09.000] DAMAGE;11;20;0.40;55")
    rows = _rows(build_kill_ledger(_infos(), tmp_path))
    assert rows[1]["player_kills"] == 1
    assert rows[2]["player_kills"] == 0


def test_npc_deaths_are_counted_apart_from_player_deaths(tmp_path):
    _agents(tmp_path, 20, _snapshot("00:05.000", 0), _snapshot("00:10.000", 1))
    _agents(tmp_path, 30, _snapshot("00:05.000", 0), _snapshot("00:20.000", 1))
    _agents(tmp_path, 31, _snapshot("00:05.000", 0), _snapshot("00:30.000", 1))
    _combat(tmp_path,
            "[00:09.000] DAMAGE;10;20;-0.30;16",
            "[00:19.000] DAMAGE;10;30;-0.30;16",
            "[00:29.000] DAMAGE;10;31;-0.30;16")
    rows = _rows(build_kill_ledger(_infos(), tmp_path))
    assert rows[1]["player_kills"] == 1
    assert rows[1]["npc_kills"] == 2
    assert rows[1]["npc_kills_guild_lord"] == 1
    assert rows[1]["npc_kills_archer"] == 1


def test_a_summon_is_pooled_not_invented(tmp_path):
    # 66 non-hall model ids exist and nothing observable separates a pet from a
    # minion from a spirit, so they share one bucket rather than a guessed one.
    _agents(tmp_path, 32, _snapshot("00:05.000", 0), _snapshot("00:10.000", 1))
    _combat(tmp_path, "[00:09.000] DAMAGE;10;32;-0.30;16")
    rows = _rows(build_kill_ledger(_infos(), tmp_path))
    assert rows[1]["npc_kills"] == 1
    assert rows[1]["npc_kills_summon"] == 1


def test_a_death_with_no_preceding_damage_credits_nobody(tmp_path):
    # Minions decaying and pets dying out of compass range. The live counter
    # skips these too, so both share a denominator.
    _agents(tmp_path, 32, _snapshot("00:05.000", 0), _snapshot("00:10.000", 1))
    _combat(tmp_path, "[00:20.000] DAMAGE;10;32;-0.30;16")
    result = build_kill_ledger(_infos(), tmp_path)
    assert _rows(result)[1]["npc_kills"] == 0
    assert result["attribution"]["kill_deaths_unattributed"] == 1


def test_a_player_misfiled_into_other_is_not_an_npc():
    # 247 agents archive-wide sit in OTHER with a model_id below 170 because
    # IsPlayer() was false at first capture. They are players.
    infos = _infos()
    infos["parties"]["1"]["OTHER"].append({"id": 33, "model_id": 71})
    _players, npcs = agent_roster(infos)
    assert 33 not in npcs
    assert 30 in npcs and 32 in npcs


def test_no_combat_stream_means_absent_not_zero(tmp_path):
    _agents(tmp_path, 20, _snapshot("00:05.000", 0), _snapshot("00:10.000", 1))
    (tmp_path / "StoC").mkdir(exist_ok=True)
    assert build_kill_ledger(_infos(), tmp_path) == {}
