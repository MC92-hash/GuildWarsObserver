import gzip

from combat_analytics import (
    build_combat_analytics,
    build_from_match_dir,
    merge_preserving_richer,
    parse_events,
)


def _infos():
    return {
        "parties": {
            "1": {"PLAYER": [{"id": 10, "player_number": 1}]},
            "2": {"PLAYER": [{"id": 20, "player_number": 9}]},
        }
    }


def _rows(result):
    return {
        row["player_number"]: row
        for party in result["players"].values()
        for row in party
    }


def test_interrupt_is_not_counted_as_voluntary_cancel():
    events = parse_events([
        "[00:01.000] SKILL_ACTIVATED;42;20;10",
        "[00:01.700] SKILL_STOPPED;20;42;10",
        "[00:01.710] INTERRUPTED;20;42;10",
    ])
    result = build_combat_analytics(_infos(), events)
    rows = _rows(result)
    assert rows[9]["casts_interrupted"] == 1
    assert rows[9]["casts_cancelled_voluntary"] == 0
    assert rows[1]["rupts_landed"] == 1
    assert rows[1]["rupt_cast_progress_ms_sum"] == 710
    assert rows[1]["rupt_cast_progress_n"] == 1


def test_unmatched_interrupt_keeps_direct_landed_credit_and_audit():
    result = build_combat_analytics(
        _infos(), parse_events(["[00:02.000] INTERRUPTED;20;42;10"])
    )
    rows = _rows(result)
    assert rows[1]["rupts_landed"] == 1
    assert rows[9]["casts_interrupted"] == 0
    assert result["attribution"]["interrupt_casts_unmatched"] == 1


def test_later_cast_does_not_hide_the_cast_that_was_interrupted():
    events = parse_events([
        "[00:01.000] SKILL_ACTIVATED;42;20;10",
        "[00:01.700] SKILL_STOPPED;20;42;10",
        "[00:01.710] INTERRUPTED;20;42;10",
        # Cast history is complete before the interrupt join runs. This later
        # cast is therefore visited first during the reverse search, but it
        # did not exist at the time of the interrupt and cannot block it.
        "[00:05.000] SKILL_ACTIVATED;99;20;10",
        "[00:05.500] SKILL_FINISHED;20;99;10",
    ])
    result = build_combat_analytics(_infos(), events)
    rows = _rows(result)
    assert rows[9]["casts_interrupted"] == 1
    assert rows[9]["casts_cancelled_voluntary"] == 0
    assert rows[1]["rupt_cast_progress_ms_sum"] == 710
    assert result["attribution"]["interrupt_casts_matched"] == 1
    assert result["attribution"]["interrupt_casts_unmatched"] == 0


def test_lifecycle_closes_and_instant_skills_do_not_dilute_it():
    events = parse_events([
        "[00:01.000] SKILL_ACTIVATED;1;10;20",
        "[00:01.500] SKILL_FINISHED;10;1;20",
        "[00:02.000] SKILL_ACTIVATED;2;10;20",
        "[00:02.200] SKILL_STOPPED;10;2;20",
        "[00:03.000] SKILL_ACTIVATED;3;10;20",
        "[00:03.100] SKILL_ACTIVATED;4;10;20",
        "[00:04.000] INSTANT_SKILL_USED;5;10;20",
    ])
    row = _rows(build_combat_analytics(_infos(), events))[1]
    assert row["casts_started"] == 4
    assert row["casts_completed"] == 1
    assert row["casts_cancelled_voluntary"] == 1
    assert row["casts_ended_other"] == 2
    assert row["casts_started"] == (
        row["casts_completed"]
        + row["casts_interrupted"]
        + row["casts_cancelled_voluntary"]
        + row["casts_ended_other"]
    )


def test_cpp_style_millisecond_parsing_is_preserved():
    event = parse_events(["[01:02.5] INTERRUPTED;20;42;10"])[0]
    assert event.time == 62.005


def test_reads_both_named_gzip_streams_and_matches_equal_timestamps(tmp_path):
    stoc = tmp_path / "StoC"
    stoc.mkdir()
    with gzip.open(stoc / "skill_events.txt.gz", "wt", encoding="utf-8") as handle:
        handle.write("[00:01.000] SKILL_ACTIVATED;42;20;10\n")
        handle.write("[00:01.700] SKILL_STOPPED;20;42;10\n")
    with gzip.open(stoc / "combat_events.txt.gz", "wt", encoding="utf-8") as handle:
        handle.write("[00:01.700] INTERRUPTED;20;42;10\n")
    result = build_from_match_dir(_infos(), tmp_path)
    assert result["sources"] == ["combat_events", "skill_events"]
    assert _rows(result)[9]["casts_interrupted"] == 1


def test_partial_stoc_stream_is_absent_not_zero(tmp_path):
    stoc = tmp_path / "StoC"
    stoc.mkdir()
    (stoc / "combat_events.txt").write_text(
        "[00:01.700] INTERRUPTED;20;42;10\n", encoding="utf-8"
    )
    assert build_from_match_dir(_infos(), tmp_path) == {}


def test_knockdown_credits_source_and_victim_directly():
    result = build_combat_analytics(
        _infos(), parse_events(["[00:02.000] KNOCKED_DOWN;20;10"])
    )
    rows = _rows(result)
    assert rows[1]["knockdowns_dealt"] == 1
    assert rows[9]["knockdowns_received"] == 1
    assert result["attribution"]["knockdown_events"] == 1


def test_coward_claims_only_same_timestamp_kd_from_same_source():
    result = build_combat_analytics(_infos(), parse_events([
        "[00:02.000] INSTANT_SKILL_USED;869;10;10",
        "[00:02.000] KNOCKED_DOWN;20;10",
        "[00:03.000] INSTANT_SKILL_USED;869;10;10",
        "[00:03.200] KNOCKED_DOWN;20;10",
    ]))
    row = _rows(result)[1]
    assert row["coward_uses"] == 2
    assert row["coward_kds"] == 1


def test_bulls_publishes_uses_but_withholds_ambiguous_success():
    result = build_combat_analytics(_infos(), parse_events([
        "[00:01.000] ATTACK_SKILL_ACTIVATED;332;10;20",
        "[00:01.600] KNOCKED_DOWN;20;10",
        "[00:02.000] ATTACK_SKILL_ACTIVATED;332;10;20",
        "[00:03.500] KNOCKED_DOWN;20;10",
    ]))
    row = _rows(result)[1]
    assert row["bulls_strike_uses"] == 2
    assert "bulls_strike_kds" not in row


def test_bulls_audits_unknown_target_instead_of_calling_it_a_miss():
    result = build_combat_analytics(_infos(), parse_events([
        "[00:01.000] ATTACK_SKILL_ACTIVATED;332;10;0",
    ]))
    row = _rows(result)[1]
    assert row["bulls_strike_uses"] == 1
    assert row["bulls_strike_target_unknown"] == 1


def test_missing_optional_attack_stream_only_withholds_bulls(tmp_path):
    stoc = tmp_path / "StoC"
    stoc.mkdir()
    (stoc / "skill_events.txt").write_text(
        "[00:02.000] INSTANT_SKILL_USED;869;10;10\n", encoding="utf-8"
    )
    (stoc / "combat_events.txt").write_text(
        "[00:03.000] KNOCKED_DOWN;20;10\n", encoding="utf-8"
    )
    result = build_from_match_dir(_infos(), tmp_path)
    row = _rows(result)[1]
    assert row["coward_uses"] == 1
    assert "bulls_strike_uses" not in row


def test_empty_required_stream_is_absent_not_observed_zero(tmp_path):
    stoc = tmp_path / "StoC"
    stoc.mkdir()
    (stoc / "skill_events.txt").write_text(
        "[00:02.000] INSTANT_SKILL_USED;869;10;10\n", encoding="utf-8"
    )
    (stoc / "combat_events.txt").write_text("", encoding="utf-8")
    assert build_from_match_dir(_infos(), tmp_path) == {}


def test_coward_exact_timestamp_join_is_not_stolen_by_bulls():
    result = build_combat_analytics(_infos(), parse_events([
        "[00:01.600] ATTACK_SKILL_ACTIVATED;332;10;20",
        "[00:02.000] INSTANT_SKILL_USED;869;10;10",
        "[00:02.000] KNOCKED_DOWN;20;10",
    ]))
    row = _rows(result)[1]
    assert row["coward_kds"] == 1
    assert "bulls_strike_kds" not in row


def test_merge_preserves_fields_from_a_richer_prior_stream_set():
    prior = {"schema": 1, "sources": ["attack_skill_events"],
             "players": {"1": [{"player_number": 1,
                                  "bulls_strike_uses": 7}]}}
    current = {"schema": 1, "sources": ["skill_events", "combat_events"],
               "players": {"1": [{"player_number": 1,
                                    "coward_uses": 4}]}}
    merged = merge_preserving_richer(prior, current)
    assert merged["sources"] == ["attack_skill_events", "combat_events",
                                  "skill_events"]
    assert merged["players"]["1"][0]["bulls_strike_uses"] == 7
    assert merged["players"]["1"][0]["coward_uses"] == 4


# ── Inferred interrupt attribution ───────────────────────────────────
#
# An INTERRUPTED record names only its victim -- the recorder writes the
# trailing fields as a literal `;0;0`. These pin the join that recovers the
# interrupter, and the cases where it must refuse to guess.

CRY_OF_FRUSTRATION = 57
SAVAGE_SHOT = 426
HEALING_BREEZE = 249


def test_the_interrupter_is_inferred_from_a_rupt_cast():
    events = parse_events([
        "[00:01.000] SKILL_ACTIVATED;249;10;10",      # victim starts a cast
        "[00:01.500] SKILL_ACTIVATED;57;20;10",       # a rupter aims at them
        "[00:01.700] INTERRUPTED;10;0;0",             # ...and it lands
        "[00:01.700] SKILL_STOPPED;10;249;0",
    ])
    rows = _rows(build_combat_analytics(_infos(), events))
    assert rows[9]["rupts_inferred"] == 1
    # Never the observed counter: the packet still carries no interrupter.
    assert rows[9]["rupts_landed"] == 0
    assert rows[1]["casts_interrupted"] == 1


def test_two_possible_interrupters_credit_nobody():
    """Guessing between two would be right on average and wrong every time."""
    infos = {"parties": {
        "1": {"PLAYER": [{"id": 10, "player_number": 1}]},
        "2": {"PLAYER": [{"id": 20, "player_number": 9},
                         {"id": 21, "player_number": 10}]},
    }}
    events = parse_events([
        "[00:01.000] SKILL_ACTIVATED;249;10;10",
        "[00:01.400] SKILL_ACTIVATED;57;20;10",
        "[00:01.500] SKILL_ACTIVATED;426;21;10",
        "[00:01.700] INTERRUPTED;10;0;0",
    ])
    result = build_combat_analytics(infos, events)
    rows = _rows(result)
    assert rows[9]["rupts_inferred"] == 0
    assert rows[10]["rupts_inferred"] == 0
    assert result["attribution"]["interrupt_inferred_ambiguous"] == 1


def test_a_cast_outside_the_window_is_not_the_cause():
    events = parse_events([
        "[00:01.000] SKILL_ACTIVATED;57;20;10",       # four seconds earlier
        "[00:05.000] INTERRUPTED;10;0;0",
    ])
    result = build_combat_analytics(_infos(), events)
    assert _rows(result)[9]["rupts_inferred"] == 0
    assert result["attribution"]["interrupt_inferred_none"] == 1


def test_a_cast_aimed_elsewhere_is_not_the_cause():
    infos = {"parties": {
        "1": {"PLAYER": [{"id": 10, "player_number": 1},
                         {"id": 11, "player_number": 2}]},
        "2": {"PLAYER": [{"id": 20, "player_number": 9}]},
    }}
    events = parse_events([
        "[00:01.500] SKILL_ACTIVATED;57;20;11",       # aimed at the other player
        "[00:01.700] INTERRUPTED;10;0;0",
    ])
    result = build_combat_analytics(infos, events)
    assert _rows(result)[9]["rupts_inferred"] == 0
    assert result["attribution"]["interrupt_inferred_none"] == 1


def test_a_non_interrupt_skill_is_never_the_cause():
    """Cruel Spear was measured at 980 casts and 0% and removed for this reason."""
    events = parse_events([
        "[00:01.500] SKILL_ACTIVATED;249;20;10",      # Healing Breeze
        "[00:01.700] INTERRUPTED;10;0;0",
    ])
    result = build_combat_analytics(_infos(), events)
    assert _rows(result)[9]["rupts_inferred"] == 0
    assert result["attribution"]["interrupt_inferred_none"] == 1


def test_a_knockdown_explains_the_interrupt_without_being_credited_twice():
    """A knockdown interrupts, and is already counted as a knockdown.

    Crediting it as an interrupt as well would score one action on two axes.
    The knockdown must also be known BEFORE the interrupt is judged: they share
    a millisecond routinely, and collecting them later let the interrupt fall
    through to the skill join and be credited to the wrong player.
    """
    events = parse_events([
        "[00:01.500] SKILL_ACTIVATED;57;20;10",       # a rupter was also casting
        "[00:01.700] KNOCKED_DOWN;10;20",
        "[00:01.700] INTERRUPTED;10;0;0",
    ])
    result = build_combat_analytics(_infos(), events)
    rows = _rows(result)
    assert rows[9]["knockdowns_dealt"] == 1
    assert rows[9]["rupts_inferred"] == 0
    assert result["attribution"]["interrupt_from_knockdown"] == 1


def test_an_attack_skill_interrupt_is_credited():
    events = parse_events([
        "[00:01.300] ATTACK_SKILL_ACTIVATED;426;20;10",
        "[00:01.700] INTERRUPTED;10;0;0",
    ])
    result = build_combat_analytics(_infos(), events)
    assert _rows(result)[9]["rupts_inferred"] == 1
    assert result["attribution"]["interrupt_inferred_unique"] == 1
