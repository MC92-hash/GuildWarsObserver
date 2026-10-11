from equipment_ledger import build_equipment, read_equipment_records

ITEM = "[00:01.000] ITEM_DEF;{ref};{iid};{iid};{skin};{typ};570425600;0,0,0,0;0;{iid};{state};{mods};{name};{skin}"


def _stream(tmp_path, lines):
    stoc = tmp_path / "StoC"
    stoc.mkdir(exist_ok=True)
    (stoc / "equipment_events.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return tmp_path


def _infos(*agent_ids):
    return {"parties": {"1": {"PLAYER": [
        {"id": agent_id, "player_number": index + 1}
        for index, agent_id in enumerate(agent_ids)
    ]}}}


def _item(ref, iid, skin, typ, name, mods="24B80200|C0000000", state="resolved"):
    return ITEM.format(ref=ref, iid=iid, skin=skin, typ=typ, name=name,
                       mods=mods, state=state)


def test_the_older_thirteen_field_item_def_still_reads(tmp_path):
    """The recorder gained a trailing icon_file_id partway through August 2026.
    Requiring it threw away every item in 107 recordings, and did it silently:
    a dropped ITEM_DEF leaves an empty item table, which looks exactly like a
    match that never had the stream."""
    folder = _stream(tmp_path, [
        # 13 fields, no trailing icon id.
        "[00:01.000] ITEM_DEF;0;39;39;39753;2;570425600;0,0,0,0;0;8437;"
        "resolved;24B80200|C0000000;Stammesklinge",
        "[00:01.000] EQUIP_SET;50;0;0",
    ])
    result = build_equipment(_infos(50), folder)
    item = result["items"][str(result["players"]["1:1"]["sets"][0][0])]
    assert item["name"] == "Stammesklinge"
    assert item["mods"] == "24B80200|C0000000"
    assert item["icon_file_id"] == 0


def test_a_truncated_item_def_is_still_refused(tmp_path):
    folder = _stream(tmp_path, [
        "[00:01.000] ITEM_DEF;0;39;39;39753;2;570425600",
        "[00:01.000] EQUIP_SET;50;0;0",
    ])
    assert build_equipment(_infos(50), folder) == {}


def test_absent_stream_is_not_an_error(tmp_path):
    (tmp_path / "StoC").mkdir()
    assert build_equipment(_infos(50), tmp_path) == {}


def test_item_def_fields_and_slot_events_parse(tmp_path):
    folder = _stream(tmp_path, [
        _item(0, 61, 39753, 2, "Zealous Axe of Fortitude"),
        "[00:01.000] EQUIP_SET;50;0;0",
        "[00:02.000] EQUIP_CLEAR;50;0",
    ])
    items, batches = read_equipment_records(folder)
    assert items[0]["name"] == "Zealous Axe of Fortitude"
    assert items[0]["model_file_id"] == 39753 and items[0]["item_type"] == 2
    assert items[0]["mods"] == "24B80200|C0000000"
    assert [record[0] for batch in batches for record in batch] == ["set", "clear"]


def test_a_swap_is_one_set_not_three(tmp_path):
    # The mainhand and the offhand change in two records at the same instant.
    # Applying them one at a time invents a set out of the intermediate state,
    # which is what produced 9-12 "sets" per player before the batching.
    folder = _stream(tmp_path, [
        _item(0, 61, 100, 2, "Axe"),
        _item(1, 62, 200, 24, "Shield"),
        _item(2, 63, 300, 27, "Sword"),
        _item(3, 64, 400, 24, "Other Shield"),
        "[00:01.000] EQUIP_SET;50;0;0",
        "[00:01.000] EQUIP_SET;50;1;1",
        "[00:05.000] EQUIP_SET;50;0;2",
        "[00:05.000] EQUIP_SET;50;1;3",
    ])
    sets = build_equipment(_infos(50), folder)["players"]["1:1"]["sets"]
    assert len(sets) == 2


def test_a_re_interned_item_is_the_same_item(tmp_path):
    # An agent leaving and returning re-interns its weapons under fresh refs.
    # Deduplicating on ref publishes the same axe twice.
    folder = _stream(tmp_path, [
        _item(0, 61, 100, 2, "Axe"),
        _item(7, 99, 100, 2, "Axe"),
        "[00:01.000] EQUIP_SET;50;0;0",
        "[00:05.000] EQUIP_SET;50;0;7",
    ])
    result = build_equipment(_infos(50), folder)
    assert len(result["players"]["1:1"]["sets"]) == 1
    assert len(result["items"]) == 1


def test_a_flag_in_the_hand_is_not_a_weapon_set(tmp_path):
    folder = _stream(tmp_path, [
        _item(0, 61, 100, 2, "Axe"),
        _item(1, 62, 94192, 6, "Flag"),
        "[00:01.000] EQUIP_SET;50;0;0",
        "[00:05.000] EQUIP_SET;50;0;1",
        "[00:09.000] EQUIP_SET;50;0;0",
    ])
    sets = build_equipment(_infos(50), folder)["players"]["1:1"]["sets"]
    assert len(sets) == 1


def test_armour_keeps_its_slot_and_its_empty_mod_list(tmp_path):
    # Armour genuinely arrives with no mods. That is not the same as an
    # unresolved item, and neither may be rendered as the other.
    folder = _stream(tmp_path, [
        _item(0, 1, 9380, 5, "", mods=""),
        _item(1, 2, 9381, 5, "", mods="", state="unresolved"),
        "[00:01.000] EQUIP_SET;50;2;0",
        "[00:01.000] EQUIP_SET;50;4;1",
    ])
    player = build_equipment(_infos(50), folder)["players"]["1:1"]
    items = build_equipment(_infos(50), folder)["items"]
    assert sorted(player["armour"]) == ["2", "4"]
    assert items[str(player["armour"]["2"])]["mod_state"] == "resolved"
    assert items[str(player["armour"]["4"])]["mod_state"] == "unresolved"
    assert items[str(player["armour"]["2"])]["mods"] == ""


def test_npcs_are_left_out(tmp_path):
    folder = _stream(tmp_path, [
        _item(0, 61, 100, 2, "Axe"),
        "[00:01.000] EQUIP_SET;50;0;0",
        "[00:01.000] EQUIP_SET;29;0;0",  # an NPC archer
    ])
    assert list(build_equipment(_infos(50), folder)["players"]) == ["1:1"]


def test_a_player_who_wore_nothing_is_absent_not_empty(tmp_path):
    folder = _stream(tmp_path, [
        _item(0, 61, 100, 2, "Axe"),
        "[00:01.000] EQUIP_SET;50;0;0",
    ])
    assert list(build_equipment(_infos(50, 51), folder)["players"]) == ["1:1"]
