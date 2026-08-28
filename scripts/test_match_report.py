import json

from match_report import main, report

_INFOS = {
    "occasion": "C AT",
    "match_duration": "10:00",
    "flux": "Test Flux",
    "winner_party_id": 2,
    "guilds": {"1": {"name": "Alpha", "tag": "AA"},
               "2": {"name": "Beta", "tag": "BB"}},
    "parties": {
        "1": {"PLAYER": [{"id": 10, "player_number": 1, "primary": 2,
                          "secondary": 3, "encoded_name": "Archer (1)"}]},
        "2": {"PLAYER": [{"id": 20, "player_number": 9, "primary": 6,
                          "secondary": 9, "encoded_name": "Sparky (9)"}]},
    },
}

_SKILLS = [
    "[00:01.000] SKILL_ACTIVATED;220;20;10",   # Blinding Flash on the archer
    "[00:02.000] SKILL_FINISHED;20;0;10",
    "[00:05.000] SKILL_ACTIVATED;249;10;20",   # archer casts something
    "[00:05.500] SKILL_STOPPED;10;0;20",
]
_ATTACKS = ["[00:05.200] ATTACK_SKILL_ACTIVATED;399;10;20"]
_COMBAT = ["[00:05.400] INTERRUPTED;20;0;0"]


def _match(tmp_path):
    (tmp_path / "infos.json").write_text(json.dumps(_INFOS), encoding="utf-8")
    stoc = tmp_path / "StoC"
    stoc.mkdir()
    for stem, lines in (("skill_events", _SKILLS),
                        ("attack_skill_events", _ATTACKS),
                        ("combat_events", _COMBAT)):
        (stoc / f"{stem}.txt").write_text(
            "".join(line + "\n" for line in lines), encoding="utf-8")
    return tmp_path


def test_report_prints_blind_interrupts_and_per_skill(tmp_path, capsys):
    assert report(_match(tmp_path)) == 0
    out = capsys.readouterr().out
    assert "Beta [BB]  <-- WON" in out
    # The blind is labelled a model everywhere it appears.
    assert "MODELLED" in out
    assert "Blinding Flash" in out
    assert "Distracting Shot" in out and "attempts (attack skill)" in out
    # Never presented as an observation.
    assert "INFERRED" in out


def test_a_recording_missing_a_required_stream_says_so(tmp_path):
    (tmp_path / "infos.json").write_text(json.dumps(_INFOS), encoding="utf-8")
    (tmp_path / "StoC").mkdir()
    assert report(tmp_path) == 1


def test_skill_filter_narrows_to_one_skill(tmp_path, capsys):
    report(_match(tmp_path), only_skill=220)
    out = capsys.readouterr().out
    assert "Blinding Flash" in out
    assert "Distracting Shot" not in out


def test_player_filter_narrows_to_one_player(tmp_path, capsys):
    report(_match(tmp_path), only_player="archer")
    out = capsys.readouterr().out
    assert "Archer" in out
    assert "Sparky" not in out


def test_a_folder_with_no_infos_is_rejected(tmp_path):
    try:
        main([str(tmp_path)])
    except SystemExit as exc:
        assert exc.code == 2
    else:
        raise AssertionError("expected argparse to reject the folder")
