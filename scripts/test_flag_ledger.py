from flag_ledger import build_flag_ledger, merge_into_analytics


def _infos():
    return {
        "parties": {
            "1": {"PLAYER": [{"id": 62, "player_number": 1},
                             {"id": 51, "player_number": 2}]},
            "2": {"PLAYER": [{"id": 61, "player_number": 9}]},
        }
    }


def _write(tmp_path, *lines):
    stoc = tmp_path / "StoC"
    stoc.mkdir(exist_ok=True)
    (stoc / "flag_events.txt").write_text("".join(l + "\n" for l in lines),
                                          encoding="utf-8")
    return tmp_path


def _rows(result):
    return {row["player_number"]: row
            for party in result["players"].values() for row in party}


def test_carry_runs_from_pickup_to_drop(tmp_path):
    path = _write(tmp_path,
                  "[00:00.951] 3;45;493;59808;6",
                  "[00:10.000] 0;45;62;0",
                  "[00:40.000] 1;62;0")
    rows = _rows(build_flag_ledger(_infos(), path))
    assert rows[1]["flag_pickups"] == 1
    assert rows[1]["flag_drops"] == 1
    assert rows[1]["flag_carry_seconds"] == 30


def test_a_bundle_that_was_never_declared_a_flag_is_not_a_flag_run(tmp_path):
    # Warrior's Isle repair kits ride the same packet as the guild flag and are
    # 8.5% of all pickups. Item 60 is never declared by an ITEM record, so it
    # cannot be one.
    path = _write(tmp_path,
                  "[00:00.951] 3;45;493;59808;6",
                  "[00:10.000] 0;60;62;0",
                  "[00:40.000] 1;62;0")
    result = build_flag_ledger(_infos(), path)
    rows = _rows(result)
    assert rows[1]["flag_pickups"] == 0
    assert rows[1]["flag_carry_seconds"] == 0
    assert result["attribution"]["flag_pickups_undeclared_item"] == 1


def test_a_respawned_flag_ends_the_carry_it_replaced(tmp_path):
    # Sticking or returning the flag spawns a NEW item id for the same team,
    # and there is no drop record -- so the respawn is what ends the carry.
    path = _write(tmp_path,
                  "[00:00.951] 3;45;493;59808;6",
                  "[00:10.000] 0;45;62;0",
                  "[00:25.000] 3;3717;493;59808;6",
                  "[02:00.000] 0;3717;51;0")
    result = build_flag_ledger(_infos(), path)
    rows = _rows(result)
    assert rows[1]["flag_carry_seconds"] == 15
    assert result["attribution"]["flag_leg_closed_respawn"] == 1


def test_a_flag_taken_over_ends_the_previous_carry(tmp_path):
    path = _write(tmp_path,
                  "[00:00.951] 3;45;493;59808;6",
                  "[00:10.000] 0;45;62;0",
                  "[00:30.000] 0;45;51;0",
                  "[00:50.000] 1;51;0")
    result = build_flag_ledger(_infos(), path)
    rows = _rows(result)
    assert rows[1]["flag_carry_seconds"] == 20
    assert rows[2]["flag_carry_seconds"] == 20
    assert result["attribution"]["flag_leg_closed_taken_over"] == 1


def test_the_two_flags_are_tracked_apart(tmp_path):
    path = _write(tmp_path,
                  "[00:00.951] 3;45;493;59808;6",
                  "[00:00.951] 3;46;493;57400;6",
                  "[00:10.000] 0;45;62;0",
                  "[00:12.000] 0;46;61;0",
                  "[00:40.000] 1;62;0",
                  "[01:00.000] 1;61;0")
    rows = _rows(build_flag_ledger(_infos(), path))
    assert rows[1]["flag_carry_seconds"] == 30
    assert rows[9]["flag_carry_seconds"] == 48


def test_a_missing_stream_is_absent_not_zero(tmp_path):
    assert build_flag_ledger(_infos(), tmp_path) == {}


def test_merge_folds_counters_onto_the_matching_slot():
    analytics = {"players": {"1": [{"player_number": 1, "kd_attempts": 3}]}}
    ledger = {"players": {"1": [{"player_number": 1, "flag_pickups": 2,
                                 "flag_carry_seconds": 61}]},
              "attribution": {"flag_carry_legs": 2}}
    merged = merge_into_analytics(analytics, ledger)
    row = merged["players"]["1"][0]
    assert row == {"player_number": 1, "kd_attempts": 3,
                   "flag_pickups": 2, "flag_carry_seconds": 61}
    assert merged["attribution"]["flag_carry_legs"] == 2


def test_merge_leaves_a_slot_the_ledger_does_not_know_alone():
    analytics = {"players": {"1": [{"player_number": 4, "kd_attempts": 1}]}}
    ledger = {"players": {"1": [{"player_number": 1, "flag_pickups": 2}]}}
    merged = merge_into_analytics(analytics, ledger)
    assert merged["players"]["1"][0] == {"player_number": 4, "kd_attempts": 1}
