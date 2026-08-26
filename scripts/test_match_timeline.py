from match_timeline import build_timeline

def test_timeline_decodes_factual_streams_and_audits_unknowns(tmp_path):
    stoc=tmp_path/"StoC"; stoc.mkdir()
    (stoc/"jumbo_messages.txt").write_text(
        "[00:01.005] GAME_SMSG_JUMBO_MESSAGE;9;1 (Party 1)\n"
        "[00:02.000] GAME_SMSG_JUMBO_MESSAGE;999;raw\n", encoding="utf-8")
    (stoc/"flag_events.txt").write_text(
        "[00:03.000] FLAG_PICKUP;0;10;1\n", encoding="utf-8")
    result=build_timeline({},tmp_path)
    assert result["schema"]==1
    assert result["streams"]["jumbo_messages"]["events"][0]["kind"]=="jumbo.morale_boost"
    assert result["streams"]["flag_events"]["events"][0]["kind"]=="flag.pickup"

def test_missing_streams_are_absent_not_empty(tmp_path):
    assert build_timeline({},tmp_path)=={}
