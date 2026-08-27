from match_timeline import build_timeline

def test_timeline_decodes_factual_streams_and_audits_unknowns(tmp_path):
    stoc=tmp_path/"StoC"; stoc.mkdir()
    (stoc/"jumbo_messages.txt").write_text(
        "[00:01.005] GAME_SMSG_JUMBO_MESSAGE;9;1 (Party 1)\n"
        "[00:02.000] GAME_SMSG_JUMBO_MESSAGE;999;raw\n", encoding="utf-8")
    # Captured verbatim from 2026-08-01_Insel_der_Toten_03.45_[hook]vs[NwAy].
    # The old fixture used a FLAG_PICKUP;... shape the recorder has never
    # emitted, so this test passed while every real line was discarded.
    (stoc/"flag_events.txt").write_text(
        "[00:00.951] 3;45;493;59808;6\n"
        "[00:11.754] 0;46;62;0\n"
        "[02:19.573] 1;61;0\n"
        "[03:27.646] 6;0;2079;2\n"
        "[04:42.021] 9;1;4;171\n", encoding="utf-8")
    result=build_timeline({},tmp_path)
    assert result["schema"]==1
    assert result["streams"]["jumbo_messages"]["events"][0]["kind"]=="jumbo.morale_boost"
    flags=result["streams"]["flag_events"]
    assert [e["kind"] for e in flags["events"]]==[
        "flag.item","flag.pickup","flag.drop","flag.return"]
    assert flags["events"][1]["agent_id"]==62
    assert flags["events"][0]["extra_id"]==59808
    # 9;... is an unknown type code, audited rather than decoded
    assert flags["audit"]["unknown_record"]==1
    assert flags["audit"]["records"]==4

def test_missing_streams_are_absent_not_empty(tmp_path):
    assert build_timeline({},tmp_path)=={}
