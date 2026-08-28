import gzip

from max_hp_solver import (Observation, correct_max_hp_for_packet, max_hp_at_time,
                           parse_snapshot_line, read_snapshots, snapshot_at,
                           solve_observations)


def _line(max_hp=500, deep=0, weapon_type=1, offhand_type=2,
          weapon_id=3, offhand_id=4):
    fields = ["0"] * 46
    fields[9], fields[11], fields[13] = "1", str(max_hp), str(deep)
    fields[25:29] = map(str, (weapon_type, offhand_type, weapon_id, offhand_id))
    return "[00:01.050] " + ";".join(fields)


def test_snapshot_positions_and_cpp_milliseconds():
    row = parse_snapshot_line(_line())
    assert row.time == 1.05 and row.max_hp == 500 and not row.has_deep_wound
    assert row.weapon_set_key == (3 << 32) | (4 << 16) | (1 << 8) | 2


def test_cpp_millisecond_digits_are_not_decimal_places():
    assert parse_snapshot_line(_line().replace("01.050", "01.05")).time == 1.005


def test_snapshot_lookup_and_nonzero_camera_scan():
    first = parse_snapshot_line(_line(max_hp=0).replace("01.050", "01.000"))
    second = parse_snapshot_line(_line(max_hp=550).replace("01.050", "02.000"))
    third = parse_snapshot_line(_line(max_hp=0).replace("01.050", "03.000"))
    rows = (first, second, third)
    assert snapshot_at(rows, 0).time == 1
    assert snapshot_at(rows, 2.5).time == 2
    assert snapshot_at(rows, 9).time == 3
    assert max_hp_at_time(rows, 1) == 550
    assert max_hp_at_time(rows, 3) == 550
    assert max_hp_at_time((), 1) == 0


def test_parser_matches_cpp_bool_prefix_and_unsigned_widths():
    assert not parse_snapshot_line(_line(deep="0.0")).has_deep_wound
    assert parse_snapshot_line(_line(deep=" 0")).has_deep_wound
    row = parse_snapshot_line(_line(max_hp="500.0", weapon_type=300,
                                    weapon_id=70000))
    assert row.max_hp == 500
    assert row.weapon_item_type == 0 and row.weapon_item_id == 0


def test_plain_and_gzip_snapshot_parity(tmp_path):
    plain, zipped = tmp_path / "1.txt", tmp_path / "1.txt.gz"
    plain.write_text(_line() + "\n", encoding="utf-8")
    with gzip.open(zipped, "wt", encoding="utf-8") as handle:
        handle.write(_line() + "\n")
    assert read_snapshots(plain) == read_snapshots(zipped)


def test_solver_separates_weapon_sets_and_drops_deep_wound():
    hits = [Observation(i, amount / 500, 1, 500) for i, amount in enumerate((17, 23, 41, 67))]
    hits += [Observation(i + 10, amount / 600, 2, 600) for i, amount in enumerate((19, 29, 43, 71))]
    hits.append(Observation(20, 31 / 400, 1, 400, True))
    solved = solve_observations(hits)
    assert solved[1].accepted and solved[1].max_hp == 500
    assert solved[2].accepted and solved[2].max_hp == 600


def test_one_outlier_does_not_eliminate_the_supported_answer():
    hits = [Observation(i, amount / 500, 1, 500)
            for i, amount in enumerate((17, 23, 41, 67))]
    hits.append(Observation(9, 0.037777, 1, 500))
    solved = solve_observations(hits)[1]
    assert solved.max_hp == 500
    assert solved.observations == 5 and solved.supporting == 4
    assert solved.first_seen == 0 and solved.last_seen == 9


def test_insufficient_and_constructed_observations_are_absent():
    assert solve_observations([Observation(i, 0.1, 1, 500) for i in range(8)]) == {}
    assert solve_observations([Observation(i, 17 / 500, 1, 500) for i in range(3)]) == {}


def test_packet_correction_uses_measured_offsets():
    assert correct_max_hp_for_packet(530, 17 / 500) == 500
    assert correct_max_hp_for_packet(500, 17 / 500) == 500
    assert correct_max_hp_for_packet(0, 0.1) == 0
    assert correct_max_hp_for_packet(500, 1.1) == 500
