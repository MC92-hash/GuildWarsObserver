from lord_pressure import build_lord_pressure, parse_lord_events

def test_parse_lord_fields_and_millisecond_timestamp():
    row = parse_lord_events(["[01:02.5] LORD_DAMAGE;10;170;0.1;0;1;20;30;50"])[0]
    assert row["time"] == 62.005 and row["damage_after"] == 50

def test_pressure_and_roster_are_observed(tmp_path):
    stoc = tmp_path / "StoC"; stoc.mkdir()
    (stoc / "lord_events.txt").write_text(
        "[00:01.000] LORD_DAMAGE;10;170;0.1;0;1;20;0;20\n"
        "[00:02.000] LORD_DAMAGE;11;170;0.1;0;1;30;20;50\n", encoding="utf-8")
    infos = {"parties": {"1": {"OTHER": [
        {"id": 1700, "model_id": 170, "deaths": 0},
        {"id": 1750, "model_id": 175, "deaths": 1},
        {"id": 999, "model_id": 999},
    ]}}}
    result = build_lord_pressure(infos, tmp_path)
    assert result["teams"]["1"]["damage_packets_sum"] == 50
    assert result["teams"]["1"]["lead_attacker_id"] == 11
    assert len(result["roster"]["1"]) == 2
    assert result["audit"]["unclassified_other"] == 1
