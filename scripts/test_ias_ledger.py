"""Tests for the attack-speed uptime ledger.

The numbers asserted here are measurements over `D:\\MatchArchive`, not guesses:
the eight-value census, the 100.000% latch, the 0-of-380 death result and the
per-profession medians all come from a pass over the archive and are what a
regression would break.
"""
from pathlib import Path

import pytest

from ias_ledger import (LONG_INTERVAL_SECONDS, MODIFIER_TOLERANCE, _MODIFIERS,
                        build_ias_ledger, classify)

# infos with a 100 s match ending at 160 s, so the window is [60, 160] -- the
# instance starts about a minute before the match, which is the trap
# `match_window` exists for.
INFOS = {
    "match_duration": "01:40",
    "match_end_time_ms": 160000,
    "parties": {"1": {"PLAYER": [{"id": 7, "player_number": 1}]}},
}


def line(when, modifier, weapon_speed="1.500", dead="0"):
    """One 51-field snapshot row with everything zero but the fields under test."""
    fields = ["0"] * 51
    fields[8] = dead
    fields[34] = weapon_speed
    fields[35] = modifier
    return when, fields


def build(rows, infos=INFOS, agent=7):
    return build_ias_ledger(infos, Path("."), records={agent: rows})


def row_of(out, agent_number=1):
    return next(r for r in out["players"]["1"] if r["player_number"] == agent_number)


# --------------------------------------------------------------- the model
def test_the_modifier_table_is_the_cross_product_and_nothing_else():
    # Nine products of three boosts by three slows. The archive shows eight of
    # them and no tenth value in 3.29M samples.
    assert len(_MODIFIERS) == 9
    assert set(_MODIFIERS) == {1.0, 0.75, 0.67, 1.33, 1.5, 1.125, 1.005, 0.8911, 0.9975}


@pytest.mark.parametrize("modifier,tier,slowed", [
    (0.75, 25, False), (0.67, 33, False),
    (1.125, 25, True),          # 0.75 x 1.5 -- ABOVE 1.0 and still a boost
    (1.005, 33, True),          # 0.67 x 1.5
    (0.891, 33, True),          # 0.67 x 1.33
    (0.9975, 25, True),         # never seen, and 0.0025 from 1.000
    (1.0, 0, False), (1.33, 0, True), (1.5, 0, True),
])
def test_every_predicted_combination_classifies(modifier, tier, slowed):
    assert classify(modifier) == (tier, slowed)


def test_a_slow_alone_is_not_a_boost():
    for modifier in (1.33, 1.5):
        tier, slowed = classify(modifier)
        assert tier == 0 and slowed


def test_an_unknown_modifier_is_refused_rather_than_rounded():
    assert classify(0.5) is None
    assert classify(0.9) is None


def test_classification_is_nearest_match_not_a_threshold():
    # 0.9975 and 1.000 are 0.0025 apart; a band would collapse them.
    assert classify(0.9975) != classify(1.0)
    assert MODIFIER_TOLERANCE < 0.0025


# ------------------------------------------------------------- the measuring
def test_a_boost_is_credited_for_the_seconds_between_its_transitions():
    out = build([line(70, "0.750"), line(90, "0.750"), line(90.1, "1.000")])
    row = row_of(out)
    assert row["ias_seconds"] == 20
    assert row["ias_seconds_25"] == 20
    assert row["ias_seconds_33"] == 0
    assert row["ias_windows"] == 1


def test_time_weighting_not_row_counting():
    # Three rows 0.1 s apart at 1.000, then one 20 s stretch at 0.750.
    # Counting rows says 25% up; integrating says 99.5%.
    rows = [line(70, "1.000"), line(70.1, "1.000"), line(70.2, "0.750"),
            line(90.2, "0.750"), line(90.3, "1.000")]
    row = row_of(build(rows))
    share = 100.0 * row["ias_seconds"] / row["ias_measured_seconds"]
    assert share > 98.0


def test_the_modifier_is_not_read_before_the_first_attack():
    # weapon_attack_speed == 0 means "has not attacked"; the 1.000 there is a
    # default. 929,789 of 929,789 such rows read exactly 1.000.
    rows = [line(70, "1.000", weapon_speed="0.000"),
            line(90, "1.000", weapon_speed="0.000"),
            line(90.1, "0.750"), line(110.1, "0.750"), line(110.2, "1.000")]
    row = row_of(build(rows))
    assert row["ias_unarmed_seconds"] == 20
    assert row["ias_measured_seconds"] == 20      # only the armed stretch
    assert row["ias_seconds"] == 20


def test_seconds_spent_dead_are_excluded_because_the_reading_latches():
    # 0 of 380 resurrections reset the modifier, so a corpse still reads 0.750.
    rows = [line(70, "0.750", dead="1"), line(90, "0.750", dead="1"),
            line(90.1, "0.750"), line(100.1, "0.750"), line(100.2, "1.000")]
    row = row_of(build(rows))
    assert row["ias_dead_seconds"] == 20
    assert row["ias_seconds"] == 10
    assert row["ias_measured_seconds"] == 10


def test_one_window_survives_a_slow_landing_mid_stretch():
    # 0.750 -> 1.125 -> 0.750 is one boost with a hex on top, not two boosts.
    rows = [line(70, "0.750"), line(80, "1.125"), line(90, "0.750"),
            line(100, "1.000")]
    row = row_of(build(rows))
    assert row["ias_windows"] == 1
    assert row["ias_seconds"] == 30
    assert row["ias_slowed_seconds"] == 10


def test_the_window_is_the_match_not_the_duration():
    # Snapshots from 5 s, but the match runs [60, 160]. Pre-match seconds are
    # not the player's uptime -- this is the trap that read an avatar at 75.0%
    # instead of 92.7%.
    rows = [line(5, "0.750"), line(70, "0.750"), line(160, "0.750"),
            line(200, "0.750")]
    row = row_of(build(rows))
    assert row["ias_measured_seconds"] == 100
    assert row["ias_match_seconds"] == 100


def test_the_last_snapshot_ends_the_measurement_not_the_match_end():
    # Snapshots stop at 100 s of a window ending at 160. Inventing the
    # remaining 60 s is the one thing this must not do.
    rows = [line(70, "0.750"), line(100, "0.750")]
    row = row_of(build(rows))
    assert row["ias_measured_seconds"] == 30
    assert row["ias_match_seconds"] == 100


def test_an_incarnation_break_does_not_carry_a_stretch_across_it():
    rows = [line(70, "0.750"), (80.0, ["# INCARNATION_BREAK"]),
            line(80, "0.750"), line(90, "1.000")]
    row = row_of(build(rows))
    # The 10 s before the break is credited; the break drops the open state, so
    # the second stretch opens a NEW window rather than continuing the first.
    assert row["ias_windows"] == 2


def test_an_unrecognised_modifier_is_counted_and_credited_to_neither_side():
    rows = [line(70, "0.500"), line(90, "0.750"), line(100, "1.000")]
    out = build(rows)
    assert out["attribution"]["ias_modifier_unrecognised"] == 1
    row = row_of(out)
    assert row["ias_measured_seconds"] == 10       # the 0.500 stretch is not measured
    assert row["ias_seconds"] == 10


def test_the_numerator_never_exceeds_the_published_denominator():
    rows = [line(70 + i * 0.1, "0.750") for i in range(200)]
    row = row_of(build(rows))
    assert row["ias_seconds"] <= row["ias_measured_seconds"]


# ------------------------------------------------------------ absent, not zero
def test_a_line_without_the_field_is_absent_not_zero():
    short = [(70.0, ["0"] * 30), (90.0, ["0"] * 30)]
    assert build(short) == {}


def test_a_player_with_no_snapshots_gets_no_row_rather_than_a_zero():
    infos = {
        "match_duration": "01:40", "match_end_time_ms": 160000,
        "parties": {"1": {"PLAYER": [{"id": 7, "player_number": 1},
                                     {"id": 8, "player_number": 2}]}},
    }
    out = build_ias_ledger(infos, Path("."),
                           records={7: [line(70, "0.750"), line(90, "1.000")]})
    assert [r["player_number"] for r in out["players"]["1"]] == [1]


def test_a_denominator_of_zero_publishes_no_row():
    # Alive but never armed: nothing was observed, so nothing is claimed.
    rows = [line(70, "1.000", weapon_speed="0.000"),
            line(90, "1.000", weapon_speed="0.000")]
    assert build(rows) == {}


# ---------------------------------------------------- the cross-metric hazard
def test_the_denominator_is_not_named_match_seconds():
    # `avatar_ledger` publishes its denominator as the bare key `match_seconds`
    # and returns {} for the ~2,322 matches with no model stream. A second
    # publisher of that key would land it on rows carrying no `form_seconds`,
    # and `avatar_uptime` is a POOLED ratio -- so it would be deflated across
    # the whole archive with nothing failing anywhere.
    out = build([line(70, "0.750"), line(90, "1.000")])
    for row in out["players"]["1"]:
        assert "match_seconds" not in row
        assert "ias_match_seconds" in row


def test_the_long_interval_exposure_is_published_rather_than_capped():
    rows = [line(70, "0.750"), line(70 + LONG_INTERVAL_SECONDS + 5, "1.000")]
    out = build(rows)
    assert out["attribution"]["ias_long_interval_seconds"] > 0
