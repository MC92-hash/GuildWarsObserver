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


def _infos_with_window(duration="05:00", end_ms=360000):
    """A match that starts at 60 s of instance time and runs five minutes."""
    return {**_infos(), "match_duration": duration, "match_end_time_ms": end_ms}


def test_a_recycled_item_id_is_read_as_of_the_pickup_not_last_wins(tmp_path):
    # Item 61 is the blue flag, then later respawns as the red one. A forward
    # pre-pass files BOTH pickups under red; measured, that mis-assigned 25.5%
    # of every pickup in the archive and swapped two players' flag time outright.
    path = _write(tmp_path,
                  "[00:00.100] 3;61;493;59808;6",
                  "[00:10.000] 0;61;62;0",
                  "[00:40.000] 1;62;0",
                  "[01:00.000] 3;61;493;57400;6",
                  "[01:10.000] 0;61;51;0",
                  "[01:30.000] 1;51;0")
    rows = _rows(build_flag_ledger(_infos(), path))
    # Both carries are real and belong to different flags, so both are credited.
    assert rows[1]["flag_carry_seconds"] == 30
    assert rows[2]["flag_carry_seconds"] == 20
    assert rows[1]["flag_pickups"] == 1
    assert rows[2]["flag_pickups"] == 1


def test_carry_before_the_match_starts_is_not_match_time(tmp_path):
    # Instance time begins about a minute before a GvG does, and players run the
    # flag out of the base during the setup. 14.1% of archived pickups are in
    # that window.
    path = _write(tmp_path,
                  "[00:00.100] 3;45;493;59808;6",
                  "[00:20.000] 0;45;62;0",
                  "[01:20.000] 1;62;0")
    rows = _rows(build_flag_ledger(_infos_with_window(), path))
    # Picked up at 20 s, dropped at 80 s, but the match only starts at 60 s.
    assert rows[1]["flag_carry_seconds"] == 20


def test_no_row_can_carry_longer_than_the_match(tmp_path):
    path = _write(tmp_path,
                  "[00:00.100] 3;45;493;59808;6",
                  "[00:05.000] 0;45;62;0")
    result = build_flag_ledger(_infos_with_window(), path)
    rows = _rows(result)
    match_seconds = result["attribution"]["flag_match_seconds"]
    assert match_seconds == 300
    assert rows[1]["flag_carry_seconds"] <= match_seconds


def test_a_drop_that_ends_no_carry_is_not_a_flag_drop(tmp_path):
    # Repair kits and vine seeds ride the same packet. 9.3% of DROP records
    # close no leg; counting them made flag_drops and flag_pickups describe
    # different populations.
    path = _write(tmp_path,
                  "[00:00.100] 3;45;493;59808;6",
                  "[00:10.000] 1;62;0")
    rows = _rows(build_flag_ledger(_infos(), path))
    assert rows[1]["flag_drops"] == 0


def test_one_agent_cannot_hold_both_flags_at_once(tmp_path):
    path = _write(tmp_path,
                  "[00:00.100] 3;45;493;59808;6",
                  "[00:00.200] 3;46;493;57400;6",
                  "[00:10.000] 0;45;62;0",
                  "[00:20.000] 0;46;62;0",
                  "[00:40.000] 1;62;0")
    rows = _rows(build_flag_ledger(_infos(), path))
    # 10 s on the blue flag, then 20 s on the red one -- not 30 + 20 overlapping.
    assert rows[1]["flag_carry_seconds"] == 30


def test_a_stick_is_credited_to_whoever_was_holding(tmp_path):
    # Named by the record, not inferred: the sticker is the carrier at the
    # moment the announce fires.
    path = _write(tmp_path,
                  "[00:00.100] 3;45;493;59808;6",
                  "[00:10.000] 0;45;62;0",
                  "[00:30.000] 6;1;2075;1")
    rows = _rows(build_flag_ledger(_infos(), path))
    assert rows[1]["flag_sticks"] == 1


def test_a_return_is_counted_but_never_attributed(tmp_path):
    # The ported animation heuristic credits 1 return in 169 over the archive:
    # its three constants never appear on a returner, and only 40% of returns
    # have an opposing player within the 200-unit gate at all. So returns stay
    # a team-level count and no player row claims one.
    path = _write(tmp_path,
                  "[00:00.100] 3;45;493;59808;6",
                  "[00:10.000] 0;45;62;0",
                  "[00:20.000] 1;62;0",
                  "[00:30.000] 6;0;2075;1")
    result = build_flag_ledger(_infos(), path)
    assert result["attribution"]["flag_returns_seen"] == 1
    for row in _rows(result).values():
        assert "flag_returns" not in row


def test_the_flag_owner_mapping_is_the_measured_one():
    from flag_ledger import FLAG_OWNER_TEAM
    # Two in-tree docs contradict each other on Red/Blue; the pickup record does
    # not. 59808 was picked up by team 1 1,079 times against 4.
    assert FLAG_OWNER_TEAM == {59808: 1, 57400: 2}
