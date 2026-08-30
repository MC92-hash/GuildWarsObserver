"""Per-player weapon sets and armour, from the `equipment_events` StoC stream.

Unlike every other ledger here this one publishes no counters. It publishes the
items themselves, because equipment is the one thing on the recording that is
**fact rather than inference**: the server sends another player's weapon with
its name, its skin and every modifier word, and the recorder writes them down.

Three rules from ``gwtoolbox-replay-plugin/docs/RECORDER_ITEM_CAPTURE_RULES.md``
survive into this file and out the other side:

* **Mods stay raw.** The 32-bit words go out exactly as recorded, never as
  decoded text. Three decode bugs were found and fixed in a single session while
  the item inspector was being built; a published match full of decoded strings
  would carry today's mistakes permanently, where raw words can be re-read
  whenever the identifier table improves. The decode side lives on the website.
* **`unresolved` is not "no mods".** ``GW::Item`` may never arrive for a slot.
  That is a different fact from an item that genuinely has none, and a reader
  that collapses the two shows a bare weapon for a missed packet -- wrong in a
  way nothing downstream can detect. ``mod_state`` is carried verbatim.
* **Armour arrives as a skin and a dye, with an empty mod list, always.** The
  server never sends another player's runes and insignias. Nothing here should
  ever be read as one; that answer belongs to the desktop app's attribute
  solver, and it is not published yet.

**Coverage.** The recorder only started writing this stream on 2026-08-17, so
most of the archive has no file at all. That is the normal state and returns
``{}``, exactly as the other ledgers treat a missing stream -- not an error, and
not an empty kit.

Line format, fixed by ``GWToolboxdll/Modules/EquipmentRecorder.h:26-29``::

    [ts] ITEM_DEF;ref;item_id;agent_item_id;model_file_id;item_type;interaction;
         d1,d2,d3,d4;dye_tint;model_id;mod_state;mods;name;icon_file_id
    [ts] EQUIP_SET;agent_id;slot;ref
    [ts] EQUIP_CLEAR;agent_id;slot
"""
from __future__ import annotations

import gzip
from pathlib import Path

# The same definition of "who is a player" every other ledger uses, so the two
# cannot disagree about the roster, and the same (party_id, player_number) key
# the whole website joins on -- agent ids are match-local and recycled.
from combat_analytics import _player_lookup as player_lookup

SCHEMA_VERSION = 1

# Slot order is fixed by GW::NPCEquipment::items[] (EquipmentRecorder.h:42-44).
SLOT_WEAPON, SLOT_OFFHAND = 0, 1
ARMOUR_SLOTS = (2, 3, 4, 5, 6)  # chest, legs, head, feet, hands
# 7 and 8 are costume body/head: cosmetic overrides that hide the real piece,
# and no GvG reader is asking about them. Left out rather than published unused.

# GW::Constants::ItemType. A bundle is a flag, a repair kit or a rock -- carried
# in the weapon slot, so it lands on this stream, and it is not a weapon set.
# The desktop drops the same thing by a different test (``isBundle`` in
# ReplayWindow_PlayerInfo.cpp), reading the snapshot's weapon_type because
# snapshots carry no item type.
ITEM_TYPE_BUNDLE = 6


def _open(path: Path):
    opener = gzip.open if path.suffix.lower() == ".gz" else open
    return opener(path, "rt", encoding="utf-8-sig", errors="replace")


def _stream(match_dir: Path) -> Path | None:
    stoc = match_dir / "StoC"
    return next((p for p in (stoc / "equipment_events.txt.gz",
                             stoc / "equipment_events.txt") if p.is_file()), None)


def _integer(value, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _parse_item_def(fields: list[str]) -> dict | None:
    """One ITEM_DEF line's payload, or None if it is too short to trust.

    **Two field counts exist and both are valid.** The recorder gained a
    trailing ``icon_file_id`` partway through August 2026; recordings from
    before that carry 13 fields where later ones carry 14. Requiring 14 threw
    away every item in 107 recordings -- a whole fortnight of the archive --
    silently, because a dropped ITEM_DEF just leaves the item table empty and
    an empty table looks exactly like a match that has no stream.

    The icon id is the only thing missing, and nothing on the website uses it:
    item icons live in Gw.dat and are not extracted. So the older shape is read
    in full and the icon id reports 0.

    Anything shorter than 13 is a truncated line, which is corruption -- the
    writer emits every field or none -- and is skipped rather than back-filled.
    """
    if len(fields) < 13:
        return None
    dyes = [_integer(token) for token in fields[7].split(",")]
    dyes = (dyes + [0, 0, 0, 0])[:4]
    return {
        "model_file_id": _integer(fields[4]),
        "item_type": _integer(fields[5]),
        "interaction": _integer(fields[6]),
        "dyes": dyes,
        "dye_tint": _integer(fields[8]),
        "model_id": _integer(fields[9]),
        # "resolved" / "unresolved", verbatim. See the module docstring.
        "mod_state": fields[10],
        # The '|'-joined 32-bit words, untouched.
        "mods": fields[11],
        # Sanitised by the recorder, so it can never contain a ';'.
        "name": fields[12],
        # Absent on the pre-icon recorder. Nothing reads it yet; it is carried
        # so an icon pipeline later has it wherever the recording had it.
        "icon_file_id": _integer(fields[13]) if len(fields) > 13 else 0,
    }


def _identity(item: dict) -> tuple:
    """What makes two item records the same item to a reader.

    Not ``ref`` and not ``item_id``: both are per-instance. An agent leaving and
    returning re-interns its weapons under fresh refs, and a flag is a new item
    on every pickup. Deduplicating on those publishes the same axe four times.
    """
    return (item["model_file_id"], item["item_type"], item["interaction"],
            item["mod_state"], item["mods"], item["name"])


def read_equipment_records(match_dir: Path):
    """``(items_by_ref, batches)``, or ``({}, [])`` when the stream is absent.

    ``batches`` groups the slot events by timestamp. That grouping is the whole
    trick: a weapon swap changes the mainhand and the offhand in two separate
    records, so applying them one at a time invents a set out of every
    intermediate state -- measured at 9 to 12 "sets" per player on a match where
    the real answer is 2 to 7.
    """
    path = _stream(match_dir)
    if path is None:
        return {}, []

    items: dict[int, dict] = {}
    batches: list[list[tuple[str, int, int, int]]] = []
    current_stamp: str | None = None

    with _open(path) as handle:
        for line in handle:
            line = line.strip()
            close = line.find("]")
            if not line.startswith("[") or close < 0:
                continue
            stamp = line[1:close]
            fields = [token.strip() for token in line[close + 1:].strip().split(";")]
            kind = fields[0] if fields else ""

            if kind == "ITEM_DEF":
                parsed = _parse_item_def(fields)
                if parsed is not None:
                    items[_integer(fields[1], -1)] = parsed
                continue

            if kind == "EQUIP_SET" and len(fields) >= 4:
                record = ("set", _integer(fields[1]), _integer(fields[2]),
                          _integer(fields[3], -1))
            elif kind == "EQUIP_CLEAR" and len(fields) >= 3:
                record = ("clear", _integer(fields[1]), _integer(fields[2]), -1)
            else:
                continue

            if stamp != current_stamp:
                batches.append([])
                current_stamp = stamp
            batches[-1].append(record)

    return items, batches


def build_equipment(infos: dict, match_dir: Path) -> dict:
    """Per-player weapon sets and armour, or ``{}`` when the stream is absent."""
    players = player_lookup(infos)
    if not players:
        return {}

    items, batches = read_equipment_records(match_dir)
    if not items or not batches:
        return {}

    slots: dict[int, dict[int, int]] = {}     # agent -> slot -> ref, live
    sets: dict[int, list[tuple]] = {}         # agent -> ordered identity pairs
    seen_sets: dict[int, set] = {}
    armour: dict[int, dict[int, tuple]] = {}  # agent -> slot -> identity

    def identity_of(ref):
        item = items.get(ref) if ref is not None else None
        return _identity(item) if item is not None else None

    for batch in batches:
        for kind, agent_id, slot, ref in batch:
            occupancy = slots.setdefault(agent_id, {})
            if kind == "set":
                occupancy[slot] = ref
            else:
                occupancy.pop(slot, None)

        # Read the batch's effect only once it has all been applied, so the two
        # halves of a swap count as one change rather than two.
        touched = {(agent_id, slot) for _, agent_id, slot, _ in batch}
        for agent_id, slot in sorted(touched):
            if agent_id not in players:
                continue
            occupancy = slots.get(agent_id, {})
            if slot in (SLOT_WEAPON, SLOT_OFFHAND):
                main = items.get(occupancy.get(SLOT_WEAPON))
                # A flag in the hand is not a weapon set.
                if main is not None and main["item_type"] == ITEM_TYPE_BUNDLE:
                    continue
                pair = (identity_of(occupancy.get(SLOT_WEAPON)),
                        identity_of(occupancy.get(SLOT_OFFHAND)))
                if pair == (None, None):
                    continue
                if pair not in seen_sets.setdefault(agent_id, set()):
                    seen_sets[agent_id].add(pair)
                    sets.setdefault(agent_id, []).append(pair)
            elif slot in ARMOUR_SLOTS:
                identity = identity_of(occupancy.get(slot))
                if identity is not None:
                    armour.setdefault(agent_id, {})[slot] = identity

    # Re-key the item table by a dense index over what the players actually
    # wore. The raw table holds every NPC's kit as well, and republishing that
    # roughly doubles the object for something nobody asks it.
    by_identity: dict[tuple, dict] = {}
    for item in items.values():
        by_identity.setdefault(_identity(item), item)

    index_of: dict[tuple, int] = {}
    published: dict[str, dict] = {}

    def intern(identity) -> int | None:
        if identity is None:
            return None
        if identity not in index_of:
            index_of[identity] = len(index_of)
            published[str(index_of[identity])] = by_identity[identity]
        return index_of[identity]

    out_players: dict[str, dict] = {}
    for agent_id, (party_id, player_number) in players.items():
        pairs = sets.get(agent_id, [])
        pieces = armour.get(agent_id, {})
        if not pairs and not pieces:
            continue
        out_players[f"{party_id}:{player_number}"] = {
            "sets": [[intern(main), intern(off)] for main, off in pairs],
            "armour": {str(slot): intern(identity)
                       for slot, identity in sorted(pieces.items())},
        }

    if not out_players:
        return {}

    return {"schema": SCHEMA_VERSION, "items": published, "players": out_players}
