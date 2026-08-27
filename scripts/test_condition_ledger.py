from condition_ledger import (build_condition_ledger, canonical_skill_id,
                              crippled_intervals)

# A 50-field snapshot line, with `has_crippled` at index 15. Every other field
# is zero: the ledger reads exactly one of them and must not care about the rest.
_PREFIX = "0.000;0.000;0.000;0.000;0;1;0;1;0;1.000;0;480;0;0;0"
_SUFFIX = ";".join("0" for _ in range(34))


def _snapshot(time: str, crippled: int) -> str:
    return f"[{time}] {_PREFIX};{crippled};{_SUFFIX}"


def _infos():
    return {
        "parties": {
            "1": {"PLAYER": [{"id": 63, "player_number": 1},
                             {"id": 56, "player_number": 2}]},
            "2": {"PLAYER": [{"id": 81, "player_number": 9}]},
        }
    }


def _agents(tmp_path, agent_id, *lines):
    agents = tmp_path / "Agents"
    agents.mkdir(exist_ok=True)
    (agents / f"{agent_id}.txt").write_text(
        "".join(line + "\n" for line in lines), encoding="utf-8")
    return tmp_path


def _stoc(tmp_path, stem, *lines):
    stoc = tmp_path / "StoC"
    stoc.mkdir(exist_ok=True)
    (stoc / f"{stem}.txt").write_text(
        "".join(line + "\n" for line in lines), encoding="utf-8")
    return tmp_path


def _rows(result):
    return {row["player_number"]: row
            for party in result["players"].values() for row in party}


def test_crippled_seconds_run_from_onset_to_clear(tmp_path):
    _agents(tmp_path, 56,
            _snapshot("00:05.000", 0),
            _snapshot("00:10.000", 1),
            _snapshot("00:22.000", 0))
    spans = crippled_intervals(tmp_path / "Agents" / "56.txt")
    assert spans == [(10.0, 22.0)]


def test_a_cripple_still_up_at_the_last_snapshot_closes_there(tmp_path):
    # Not at the match end: past the final snapshot nothing was observed, and
    # inventing seconds is the one thing this must not do.
    _agents(tmp_path, 56,
            _snapshot("00:10.000", 1),
            _snapshot("00:30.000", 1))
    assert crippled_intervals(tmp_path / "Agents" / "56.txt") == [(10.0, 30.0)]


def test_a_targeted_signet_is_credited_to_its_caster(tmp_path):
    _agents(tmp_path, 56,
            _snapshot("00:09.000", 0),
            _snapshot("00:10.000", 1),
            _snapshot("00:18.000", 0))
    # 3273 is the PvP twin of Signet of Pious Restraint. Counting only the base
    # id scores zero on the whole archive, so the twin has to resolve.
    _stoc(tmp_path, "skill_events", "[00:09.500] SKILL_ACTIVATED;3273;63;56")
    result = build_condition_ledger(_infos(), tmp_path)
    rows = _rows(result)
    assert rows[1]["cripple_applications"] == 1
    assert rows[1]["cripple_caused_seconds"] == 8
    assert rows[2]["crippled_seconds_received"] == 8
    assert result["attribution"]["cripple_credited_unique"] == 1


def test_two_possible_casters_credit_neither(tmp_path):
    _agents(tmp_path, 56,
            _snapshot("00:09.000", 0),
            _snapshot("00:10.000", 1),
            _snapshot("00:18.000", 0))
    _stoc(tmp_path, "skill_events",
          "[00:09.500] SKILL_ACTIVATED;3273;63;56",
          "[00:09.600] SKILL_ACTIVATED;392;81;56")
    result = build_condition_ledger(_infos(), tmp_path)
    rows = _rows(result)
    assert rows[1]["cripple_applications"] == 0
    assert rows[9]["cripple_applications"] == 0
    assert result["attribution"]["cripple_ambiguous"] == 1
    assert result["attribution"]["cripple_credited_unique"] == 0


def test_harriers_grasp_is_credited_through_the_hit_not_the_cast(tmp_path):
    # The enchantment goes up long before the cripple. What applies it is the
    # attack landing, so a cast alone must credit nothing and the hit must.
    _agents(tmp_path, 56,
            _snapshot("00:09.000", 0),
            _snapshot("00:20.000", 1),
            _snapshot("00:26.000", 0))
    _stoc(tmp_path, "skill_events", "[00:05.000] SKILL_ACTIVATED;1758;63;63")
    _stoc(tmp_path, "basic_attack_events", "[00:19.500] ATTACK_FINISHED;63;0;56")
    rows = _rows(build_condition_ledger(_infos(), tmp_path))
    assert rows[1]["cripple_applications"] == 1
    assert rows[1]["cripple_caused_seconds"] == 6


def test_an_attack_without_the_enchantment_credits_nobody(tmp_path):
    _agents(tmp_path, 56,
            _snapshot("00:09.000", 0),
            _snapshot("00:20.000", 1),
            _snapshot("00:26.000", 0))
    _stoc(tmp_path, "basic_attack_events", "[00:19.500] ATTACK_FINISHED;63;0;56")
    result = build_condition_ledger(_infos(), tmp_path)
    assert _rows(result)[1]["cripple_applications"] == 0
    assert result["attribution"]["cripple_uncredited"] == 1


def test_condition_removal_and_immunity_are_not_cripple_sources():
    from condition_ledger import (_CRIPPLE_ONHIT, _CRIPPLE_TARGETED,
                                  _CRIPPLE_UNTARGETED)
    # Mend Condition, Restore Condition, Mend Ailment, Purge Conditions, Return
    # and "I Am Unstoppable!" all match the same description text and all
    # explain a cripple ENDING or never landing.
    every = set(_CRIPPLE_TARGETED) | set(_CRIPPLE_ONHIT) | set(_CRIPPLE_UNTARGETED)
    for skill_id in (275, 276, 277, 278, 770, 2356):
        assert skill_id not in every


def test_no_snapshots_means_absent_not_zero(tmp_path):
    (tmp_path / "StoC").mkdir()
    assert build_condition_ledger(_infos(), tmp_path) == {}


def test_pvp_twin_resolves_to_its_base_id():
    assert canonical_skill_id(3273) == 2014
    assert canonical_skill_id(2014) == 2014
    assert canonical_skill_id(331) == 331


def test_a_shout_with_no_visible_target_can_still_be_credited(tmp_path):
    # A shout arrives as INSTANT_SKILL_USED, whose payload carries no target, so
    # the record names the caster in the target slot. Read as targeted-at-self
    # it matched no victim and could neither credit nor refuse; 862 uses of this
    # one across 250 recordings were silently dropped.
    _agents(tmp_path, 56,
            _snapshot("00:09.000", 0),
            _snapshot("00:10.000", 1),
            _snapshot("00:18.000", 0))
    _stoc(tmp_path, "skill_events", "[00:09.500] INSTANT_SKILL_USED;1412;63;63")
    rows = _rows(build_condition_ledger(_infos(), tmp_path))
    assert rows[1]["cripple_applications"] == 1
    assert rows[1]["cripple_caused_seconds"] == 8


def test_an_untargeted_shout_refuses_a_credit_it_could_explain(tmp_path):
    # The more important half: the shout must also be able to make the ledger
    # REFUSE. Before, a cripple it caused was handed to whatever targeted skill
    # happened to be in window -- measured, 21 such credits over 120 matches.
    _agents(tmp_path, 56,
            _snapshot("00:09.000", 0),
            _snapshot("00:10.000", 1),
            _snapshot("00:18.000", 0))
    _stoc(tmp_path, "skill_events",
          "[00:09.400] INSTANT_SKILL_USED;1412;63;63",
          "[00:09.500] SKILL_ACTIVATED;3273;81;56")
    result = build_condition_ledger(_infos(), tmp_path)
    rows = _rows(result)
    assert rows[1]["cripple_applications"] == 0
    assert rows[9]["cripple_applications"] == 0
    assert result["attribution"]["cripple_ambiguous"] == 1
