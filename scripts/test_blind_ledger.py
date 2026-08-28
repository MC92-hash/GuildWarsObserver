from blind_ledger import blind_windows, build_blind_ledger
from combat_analytics import Cast

# 59 and 60 are the blinders, 49 the victim, 48 the victim's monk.
PLAYERS = {
    59: ("2", 10),
    60: ("2", 11),
    49: ("1", 3),
    48: ("1", 8),
}

BLINDING_FLASH = 220
DRAW_CONDITIONS = 311
EXTINGUISH = 943
MENDING_TOUCH = 872
STEAM = 846           # blinds, but only if the target is on fire
EBON_DUST_AURA = 1760  # blinds, but names no victim


def _cast(agent, skill, start, end, target=0, outcome="completed"):
    return Cast(agent, skill, start, ended_at=end, outcome=outcome,
                target_id=target)


def _rows(result):
    return {(party, row["player_number"]): row
            for party, rows in result["players"].items() for row in rows}


def test_a_completed_blinding_flash_makes_a_window_at_both_ranks():
    history = {59: [_cast(59, BLINDING_FLASH, 10.0, 11.0, target=49)]}
    result = build_blind_ledger(PLAYERS, history, match_end=600.0)
    row = _rows(result)[("1", 3)]
    # 3...8 seconds: 7 at rank 12, 8 at rank 15.
    assert row["blind_seconds_modeled_low"] == 7
    assert row["blind_seconds_modeled_high"] == 8
    assert row["blind_applications_received"] == 1


def test_an_interrupted_cast_applies_nothing():
    history = {59: [_cast(59, BLINDING_FLASH, 10.0, 10.4, target=49,
                          outcome="interrupted")]}
    assert build_blind_ledger(PLAYERS, history, match_end=600.0) == {}


def test_overlapping_windows_are_merged_not_added():
    # Re-blinding somebody three seconds in extends the blind, it does not
    # start a second one. Added, these two would read 14 seconds at rank 12.
    history = {59: [_cast(59, BLINDING_FLASH, 10.0, 11.0, target=49),
                    _cast(59, BLINDING_FLASH, 13.0, 14.0, target=49)]}
    result = build_blind_ledger(PLAYERS, history, match_end=600.0)
    assert _rows(result)[("1", 3)]["blind_seconds_modeled_low"] == 10
    # The caster's caused-seconds is the same span seen from the other side, so
    # it must merge too rather than crediting 7 + 7.
    assert _rows(result)[("2", 10)]["blind_seconds_caused_modeled_low"] == 10


def test_a_targeted_removal_cuts_the_window_short():
    history = {
        59: [_cast(59, BLINDING_FLASH, 10.0, 11.0, target=49)],
        48: [_cast(48, DRAW_CONDITIONS, 13.9, 14.0, target=49)],
    }
    result = build_blind_ledger(PLAYERS, history, match_end=600.0)
    assert _rows(result)[("1", 3)]["blind_seconds_modeled_low"] == 3
    assert result["attribution"]["blind_windows_truncated_by_removal"] == 1


def test_a_removal_aimed_at_somebody_else_does_not_cut_it():
    history = {
        59: [_cast(59, BLINDING_FLASH, 10.0, 11.0, target=49)],
        48: [_cast(48, DRAW_CONDITIONS, 13.9, 14.0, target=48)],
    }
    result = build_blind_ledger(PLAYERS, history, match_end=600.0)
    assert _rows(result)[("1", 3)]["blind_seconds_modeled_low"] == 7


def test_a_teamwide_removal_cuts_every_window_on_its_own_team():
    history = {
        59: [_cast(59, BLINDING_FLASH, 10.0, 11.0, target=49)],
        48: [_cast(48, EXTINGUISH, 13.9, 14.0)],
    }
    result = build_blind_ledger(PLAYERS, history, match_end=600.0)
    assert _rows(result)[("1", 3)]["blind_seconds_modeled_low"] == 3


def test_a_teamwide_removal_by_the_blinding_team_does_not_cut_it():
    # Extinguish clears the CASTER's allies. Cutting the enemy's blind with it
    # would hand the blinded team a removal it never had.
    history = {
        59: [_cast(59, BLINDING_FLASH, 10.0, 11.0, target=49),
             _cast(59, EXTINGUISH, 13.9, 14.0)],
    }
    result = build_blind_ledger(PLAYERS, history, match_end=600.0)
    assert _rows(result)[("1", 3)]["blind_seconds_modeled_low"] == 7


def test_the_victim_clearing_it_themselves_cuts_the_window():
    history = {
        59: [_cast(59, BLINDING_FLASH, 10.0, 11.0, target=49)],
        49: [_cast(49, MENDING_TOUCH, 12.9, 13.0, target=49)],
    }
    result = build_blind_ledger(PLAYERS, history, match_end=600.0)
    assert _rows(result)[("1", 3)]["blind_seconds_modeled_low"] == 2


def test_a_window_is_truncated_at_the_match_end():
    history = {59: [_cast(59, BLINDING_FLASH, 96.0, 97.0, target=49)]}
    result = build_blind_ledger(PLAYERS, history, match_end=100.0)
    assert _rows(result)[("1", 3)]["blind_seconds_modeled_low"] == 3


def test_conditional_and_area_sources_are_audited_never_scored():
    # Steam blinds only a burning target and Ebon Dust Aura names no victim.
    # Both must show up as a published gap rather than as seconds or as zero.
    history = {59: [_cast(59, BLINDING_FLASH, 10.0, 11.0, target=49),
                    _cast(59, STEAM, 20.0, 21.0, target=49),
                    _cast(59, EBON_DUST_AURA, 30.0, 31.0)]}
    result = build_blind_ledger(PLAYERS, history, match_end=600.0)
    assert result["attribution"]["blind_casts_unmodelled"] == 2
    assert result["attribution"]["blind_applications_modeled"] == 1
    assert _rows(result)[("1", 3)]["blind_seconds_modeled_low"] == 7


def test_no_modellable_source_is_absent_not_zero():
    # A team blinding constantly with Ebon Dust Aura must not publish a clean
    # sheet for the players it blinds.
    history = {59: [_cast(59, EBON_DUST_AURA, 30.0, 31.0)]}
    assert build_blind_ledger(PLAYERS, history, match_end=600.0) == {}


def test_a_self_targeted_blind_credits_nobody():
    history = {59: [_cast(59, BLINDING_FLASH, 10.0, 11.0, target=59)]}
    assert build_blind_ledger(PLAYERS, history, match_end=600.0) == {}


def test_blind_windows_matches_what_the_ledger_summed():
    history = {
        59: [_cast(59, BLINDING_FLASH, 10.0, 11.0, target=49),
             _cast(59, BLINDING_FLASH, 13.0, 14.0, target=49)],
    }
    windows = blind_windows(PLAYERS, history, 600.0)
    total = sum(end - start for start, end in windows[49])
    ledger = build_blind_ledger(PLAYERS, history, match_end=600.0)
    assert round(total) == _rows(ledger)[("1", 3)]["blind_seconds_modeled_low"]
