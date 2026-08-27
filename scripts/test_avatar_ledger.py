from avatar_ledger import build_avatar_ledger, match_seconds, read_model_records


def _infos(duration="20:02"):
    return {
        "match_duration": duration,
        "parties": {
            "1": {"PLAYER": [{"id": 63, "player_number": 1},
                             {"id": 56, "player_number": 2}]},
            "2": {"PLAYER": [{"id": 81, "player_number": 9}]},
        },
    }


def _stoc(tmp_path, *lines):
    stoc = tmp_path / "StoC"
    stoc.mkdir(exist_ok=True)
    (stoc / "model_events.txt").write_text(
        "".join(line + "\n" for line in lines), encoding="utf-8")
    return tmp_path


def _rows(result):
    return {row["player_number"]: row
            for party in result["players"].values() for row in party}


def test_match_duration_is_minutes_and_seconds_not_a_number():
    assert match_seconds({"match_duration": "20:02"}) == 1202
    assert match_seconds({"match_duration": 90}) == 90
    assert match_seconds({}) == 0


def test_uptime_runs_from_transform_to_revert(tmp_path):
    _stoc(tmp_path,
          "[00:00.933] MODEL_INIT;63;0;0;3;0x3000",
          "[00:49.327] MODEL_CHANGE;63;7;0;3;0x3000",
          "[01:52.404] MODEL_CHANGE;63;0;7;3;0x3000")
    rows = _rows(build_avatar_ledger(_infos(), tmp_path))
    assert rows[1]["form_seconds"] == 63
    assert rows[1]["form_transforms"] == 1
    assert rows[1]["form_id"] == 7


def test_a_form_still_up_at_the_end_runs_to_the_end_of_the_match(tmp_path):
    _stoc(tmp_path,
          "[00:00.933] MODEL_INIT;63;0;0;3;0x3000",
          "[19:02.000] MODEL_CHANGE;63;7;0;3;0x3000")
    rows = _rows(build_avatar_ledger(_infos("20:02"), tmp_path))
    assert rows[1]["form_seconds"] == 60


def test_a_player_who_never_transformed_is_an_observed_zero(tmp_path):
    # Unlike a missing stream, this really is zero: the tracker was running and
    # emitted a baseline for them.
    _stoc(tmp_path,
          "[00:00.933] MODEL_INIT;63;0;0;3;0x3000",
          "[00:00.933] MODEL_INIT;56;0;0;4;0x3000",
          "[00:49.327] MODEL_CHANGE;63;7;0;3;0x3000")
    rows = _rows(build_avatar_ledger(_infos(), tmp_path))
    assert rows[2]["form_seconds"] == 0
    assert rows[2]["form_id"] == 0
    assert rows[1]["form_seconds"] > 0


def test_re_entering_the_same_form_counts_both_stints(tmp_path):
    _stoc(tmp_path,
          "[00:00.000] MODEL_INIT;63;0;0;3;0x3000",
          "[00:10.000] MODEL_CHANGE;63;7;0;3;0x3000",
          "[00:20.000] MODEL_CHANGE;63;0;7;3;0x3000",
          "[00:30.000] MODEL_CHANGE;63;7;0;3;0x3000",
          "[00:40.000] MODEL_CHANGE;63;0;7;3;0x3000")
    rows = _rows(build_avatar_ledger(_infos(), tmp_path))
    assert rows[1]["form_seconds"] == 20
    assert rows[1]["form_transforms"] == 2


def test_a_resync_counts_the_same_as_a_packet_change(tmp_path):
    # MODEL_RESYNC is the poll noticing a change 0x00AE did not carry. It is
    # still a real transform and must not be dropped.
    _stoc(tmp_path,
          "[00:00.000] MODEL_INIT;63;0;0;3;0x3000",
          "[00:10.000] MODEL_RESYNC;63;10;0;0;3;0x3000",
          "[00:25.000] MODEL_CHANGE;63;0;10;3;0x3000")
    rows = _rows(build_avatar_ledger(_infos(), tmp_path))
    assert rows[1]["form_seconds"] == 15
    assert rows[1]["form_id"] == 10


def test_no_stream_means_absent_not_zero(tmp_path):
    (tmp_path / "StoC").mkdir()
    assert read_model_records(tmp_path) == []
    assert build_avatar_ledger(_infos(), tmp_path) == {}
