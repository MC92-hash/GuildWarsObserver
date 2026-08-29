from combat_analytics import Cast, canonical_skill_id
from skill_ledger import build_skill_casts

PLAYERS = {59: ("2", 10), 49: ("1", 3)}

BLINDING_FLASH = 220
DISTRACTING_SHOT = 399
SAVAGE_SHOT = 426


def _cast(agent, skill, outcome="completed"):
    return Cast(agent, skill, 0.0, ended_at=1.0, outcome=outcome)


def _rows(result):
    return {(party, row["player_number"]): row
            for party, rows in result["players"].items() for row in rows}


def test_the_three_counters_split_by_outcome():
    history = {59: [_cast(59, BLINDING_FLASH),
                    _cast(59, BLINDING_FLASH),
                    _cast(59, BLINDING_FLASH, "interrupted"),
                    _cast(59, BLINDING_FLASH, "stopped")]}
    row = _rows(build_skill_casts(PLAYERS, history))[("2", 10)]
    assert row["skills"][str(BLINDING_FLASH)] == [4, 2, 1]


def test_a_voluntary_cancel_is_not_an_interrupt():
    # `stopped` with no INTERRUPTED in window is a fake cast. Counting it as an
    # interrupt would turn every fake cast into an opponent's success.
    history = {59: [_cast(59, BLINDING_FLASH, "stopped")]}
    row = _rows(build_skill_casts(PLAYERS, history))[("2", 10)]
    assert row["skills"][str(BLINDING_FLASH)] == [1, 0, 0]


def test_casts_with_no_skill_id_are_dropped():
    history = {59: [_cast(59, 0), _cast(59, BLINDING_FLASH)]}
    row = _rows(build_skill_casts(PLAYERS, history))[("2", 10)]
    assert list(row["skills"]) == [str(BLINDING_FLASH)]


def test_attack_attempts_are_counted_without_an_outcome():
    # ATTACK_SKILL_FINISHED fires for a fraction of activations, so an attack
    # skill gets one number, not three.
    attempts = {(49, DISTRACTING_SHOT): 20, (49, SAVAGE_SHOT): 22}
    row = _rows(build_skill_casts(PLAYERS, {49: []}, attempts))[("1", 3)]
    assert row["attack_attempts"] == {str(DISTRACTING_SHOT): 20,
                                      str(SAVAGE_SHOT): 22}
    assert "skills" not in row


def test_pvp_twins_fold_onto_the_base_id():
    twin = next((twin for twin, base in
                 __import__("combat_analytics").pvp_twins().items()
                 if base != twin), None)
    assert twin is not None, "skill_meta.json carries no pvp_twins"
    base = canonical_skill_id(twin)
    history = {59: [_cast(59, twin), _cast(59, base)]}
    row = _rows(build_skill_casts(PLAYERS, history,
                                  canonicalise=canonical_skill_id))[("2", 10)]
    assert row["skills"][str(base)] == [2, 2, 0]
    assert str(twin) not in row["skills"]


def test_a_player_who_cast_nothing_is_absent_not_zero():
    result = build_skill_casts(PLAYERS, {59: [_cast(59, BLINDING_FLASH)]})
    assert ("1", 3) not in _rows(result)


def test_no_players_yields_nothing():
    assert build_skill_casts({}, {59: [_cast(59, BLINDING_FLASH)]}) == {}


FRENZY = 346
FRENZY_PVP = 3443


def test_an_instant_skill_is_published_even_though_it_is_not_a_cast():
    """Every stance in the game was missing from the page before this.

    `INSTANT_SKILL_USED` never becomes a `Cast`, correctly -- an instant cannot
    be cancelled or interrupted, so counting it would dilute every completion
    rate. But `skills` is built from that same history, so an instant reached
    no consumer at all. Measured over 80 matches, 64 skills appear only as
    instants and their 29,147 uses are 17.4% of every skill use in the archive.
    """
    row = _rows(build_skill_casts(
        PLAYERS, {}, None, {(59, FRENZY): 35}))[("2", 10)]
    assert row["instant_uses"] == {str(FRENZY): 35}
    # Not a cast, so it must not appear as one.
    assert not row.get("skills")


def test_an_instant_carries_a_use_count_and_no_outcome():
    row = _rows(build_skill_casts(
        PLAYERS, {}, None, {(59, FRENZY): 3}))[("2", 10)]
    # One number, like attack_attempts -- not a [started, completed, interrupted]
    # triple. An instant has no cast to complete or be interrupted during, and
    # publishing completed == started would be true, useless, and would flatter
    # anybody running more stances if a reader ever averaged completion.
    assert isinstance(row["instant_uses"][str(FRENZY)], int)


def test_instant_pvp_twins_fold_to_the_base_id():
    # The cast stream carries Frenzy (PvP) 3443 while the BAR carries 346, so
    # without folding the page shows a skill the player's bar does not list.
    assert canonical_skill_id(FRENZY_PVP) == FRENZY
    row = _rows(build_skill_casts(
        PLAYERS, {}, None, {(59, FRENZY_PVP): 20, (59, FRENZY): 15},
        canonicalise=canonical_skill_id))[("2", 10)]
    assert row["instant_uses"] == {str(FRENZY): 35}
    assert str(FRENZY_PVP) not in row["instant_uses"]


def test_a_player_with_only_instants_still_gets_a_row():
    # The reported bug: a warrior whose bar is Frenzy + Hundred Blades + Sprint
    # plus attack skills published zero skills and looked like they cast
    # nothing.
    result = build_skill_casts(PLAYERS, {}, None, {(59, FRENZY): 1})
    assert result["players"]["2"][0]["player_number"] == 10


def test_no_instants_publishes_no_block_rather_than_an_empty_one():
    row = _rows(build_skill_casts(
        PLAYERS, {59: [_cast(59, BLINDING_FLASH)]}))[("2", 10)]
    assert "instant_uses" not in row
