"""Fold a GWToolbox client skill dump into ``Data/skilldata.json`` + ``skilldesc-en.json``.

``make_skillpatch.py`` assumes someone has already "overwritten Data/ with a fresh
upstream dump". Upstream there means build-wars/gw-skilldata, which is a curated,
*published* view of the skill table and lags a live balance update by however long
it takes a volunteer to regenerate it. The August 2026 pass was applied by hand
from the wiki notes instead, and the hand pass missed a third of the update -
every new PvP split, and reworks like Primal Rage and Decapitate that the notes
worded as prose rather than as a number.

The client's own table has none of that lag. GWToolbox can dump it
(``%APPDATA%/GWToolboxpp/GwReplayRecorder/skilldump``), and this script converts
that dump into the two files the tool actually reads. Run it, then run
``make_skillpatch.py`` to capture what moved.

    python scripts/import_skilldump.py --dump "%APPDATA%/GWToolboxpp/GwReplayRecorder/skilldump" --dry-run
    python scripts/import_skilldump.py --dump "..."

The dump is the client table, not the published one, so five things have to be
translated rather than copied. Each is a real difference, not a formatting quirk:

**attribute.** The client says 51 for "no attribute"; ``SkillDatabase::GetAttributeName``
says 101, and 102..109 for the PvE title tracks (Sunspear, Lightbringer, Luxon,
Kurzick, Asura, Deldrimor, Ebon Vanguard, Norn). The client does not distinguish
those at all - all nine collapse to 51 - so 51 maps to 101 *unless* the committed
row already carries a title track, which is then kept. Get this wrong and 56
skills lose their track, which is what ``GetProfessionForAttribute`` reads.

**adrenaline.** The client counts adrenaline in points, 25 to a strike; the tool
counts strikes. ``ceil(points / 25)`` reproduces the committed value for 101 of
the 109 skills that carry adrenaline. The eight that disagree (Skull Crack,
Test of Faith, Anthem of Guidance, Energizing Chorus, Song of Purification,
Decapitate, Reaper's Sweep, Primal Rage) are the August update's changes to those
skills, confirmed against the wiki notes.

**pvp_split / split_id.** The client points both halves of a split at each other
and uses 3476 - one past the last id - as "no split". ``SkillDatabase::Get``
follows ``pvp_split`` to answer as the PvP half, so a back-pointer on the PvP
entry would make ``Get(pvp_id)`` answer as the *PvE* half: exactly backwards.
Only the PvE side keeps the link, which is what the committed data already does.

**name.** The client's strings are raw: no "(Luxon)"/"(Kurzick)" to tell the two
Shadow Sanctuaries apart, no "(PvP)" on Pious Fury, and a scattering of double
spaces ("Aegis  (PvP)"). A committed name is kept when it is the client's name
plus one of those parentheticals, and taken from the dump otherwise - so a real
rename still lands ("Help Me!" -> "Help!", id 1594 and its PvP twin 3036).

**markup.** The client's text carries ``<c=@SkillDull>...</c>`` around the dull
half of a concise description. Nothing in the tool renders it (``concise`` is
only ever read as a fallback source for ``ParseScalesFromDescription``), and the
committed files have none, so the tags are stripped and the text kept.

Scope. The committed pair is a player-skill set: 1484 rows out of the client's
3472, with no monster or environment entries and no placeholder names. That is
worth preserving - a skill browser listing 1706 rows called "..." and "(monster
only)" is worse than one that does not. So the kept set is every id already
committed, plus every id past the previous high-water mark that carries a real
profession. The client appends, so that second rule is what catches an update's
new skills: in August it caught 32 new PvP splits and Soul Ignition, and nothing
else. Anything it skips is listed in the summary, so a genuinely new
professionless skill cannot slip through unnoticed.

Stdlib only, no dependencies.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from collections import OrderedDict
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DATA_DIR = REPO / "Data"

# The client's "no attribute", and the tool's. See the module docstring.
CLIENT_ATTR_NONE = 51
TOOL_ATTR_NONE = 101
# 101 is No Attribute; 102..109 are the PvE title tracks the client does not model.
TOOL_ATTR_SPECIAL = range(101, 110)

# One past the last skill id, used by the client as "this skill has no PvP split".
# Read from the dump rather than hardcoded, since it moves every time skills are added.
def _no_split_sentinel(skilldata: dict) -> int:
    return max(int(k) for k in skilldata) + 1

# Adrenaline points per strike.
ADRENALINE_PER_STRIKE = 25

# Parentheticals the published data adds to disambiguate names the client leaves identical.
DISAMBIGUATORS = (" (Luxon)", " (Kurzick)", " (PvP)")

# Field order in Data/skilldata.json, kept so a diff of the regenerated file shows
# only the values that moved.
DATA_FIELDS = (
    "id", "campaign", "profession", "attribute", "type", "is_elite", "is_rp",
    "is_pvp", "pvp_split", "split_id", "upkeep", "energy", "activation",
    "recharge", "adrenaline", "sacrifice", "overcast",
)
DESC_FIELDS = ("id", "name", "description", "concise")

TAG_RE = re.compile(r"</?c(?:=[^>]*)?>")
WS_RE = re.compile(r"\s+")


def _load(path: Path, envelope: str) -> tuple[dict, dict]:
    """Return ``(whole_document, body)`` for one dump/data file.

    The envelope is named rather than guessed so a truncated or half-written file
    fails here instead of quietly producing an empty import.
    """
    doc = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=OrderedDict)
    body = doc.get(envelope)
    if not isinstance(body, dict) or not body:
        raise SystemExit(f"{path}: no '{envelope}' object found -- is this the right file?")
    return doc, body


def strip_markup(text: str) -> str:
    """Drop the client's colour tags and collapse the whitespace they leave behind."""
    return WS_RE.sub(" ", TAG_RE.sub("", text)).strip()


def clean_name(name: str) -> str:
    """Collapse the double spaces the client's own strings carry ("Aegis  (PvP)")."""
    return WS_RE.sub(" ", name).strip()


def pick_name(dump_name: str, committed_name: str | None) -> str:
    """The dump's name, unless the committed one only adds a disambiguator."""
    dump_name = clean_name(dump_name)
    if committed_name is None:
        return dump_name
    for suffix in DISAMBIGUATORS:
        if committed_name == dump_name + suffix:
            return committed_name
    return dump_name


def convert_row(row: dict, committed: dict | None, sentinel: int) -> OrderedDict:
    """One skilldata row, client schema -> tool schema."""
    out = OrderedDict()

    attribute = row["attribute"]
    if attribute == CLIENT_ATTR_NONE:
        # The client cannot tell No Attribute from a title track. Keep whichever
        # the committed row already established; default to No Attribute.
        prior = committed.get("attribute") if committed else None
        attribute = prior if prior in TOOL_ATTR_SPECIAL else TOOL_ATTR_NONE

    split_id = row["split_id"]
    if row["is_pvp"] or split_id == sentinel:
        # The PvP half never carries the link: SkillDatabase::Get follows it to
        # answer as the PvP skill, and a back-pointer would answer as the PvE one.
        pvp_split, split_id = False, 0
    else:
        pvp_split = True

    adrenaline = math.ceil(row["adrenaline"] / ADRENALINE_PER_STRIKE) if row["adrenaline"] else 0

    values = {
        "id": row["id"],
        "campaign": row["campaign"],
        "profession": row["profession"],
        "attribute": attribute,
        "type": row["type"],
        "is_elite": row["is_elite"],
        "is_rp": row["is_rp"],
        "is_pvp": row["is_pvp"],
        "pvp_split": pvp_split,
        "split_id": split_id,
        "upkeep": row["upkeep"],
        "energy": row["energy"],
        "activation": row["activation"],
        "recharge": row["recharge"],
        "adrenaline": adrenaline,
        "sacrifice": row["sacrifice"],
        "overcast": row["overcast"],
    }
    for field in DATA_FIELDS:
        out[field] = values[field]
    return out


def choose_ids(dump_data: dict, committed_data: dict) -> tuple[list[str], list[str]]:
    """``(kept, skipped)`` ids, both sorted numerically. See the docstring's Scope."""
    committed_ids = set(committed_data)
    high_water = max(int(k) for k in committed_ids)

    kept, skipped = [], []
    for key in dump_data:
        if key in committed_ids:
            kept.append(key)
        elif int(key) > high_water and dump_data[key]["profession"] != 0:
            kept.append(key)
        elif int(key) > high_water:
            skipped.append(key)
    kept.sort(key=int)
    skipped.sort(key=int)
    return kept, skipped


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Import a GWToolbox client skill dump into Data/.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    default_dump = Path(os.environ.get("APPDATA", "")) / "GWToolboxpp" / "GwReplayRecorder" / "skilldump"
    parser.add_argument("--dump", type=Path, default=default_dump,
                        help=f"directory holding the dumped pair (default: {default_dump})")
    parser.add_argument("--data-dir", type=Path, default=DATA_DIR,
                        help=f"where the tool's pair lives (default: {DATA_DIR})")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the summary and write nothing")
    args = parser.parse_args(argv)

    dump_data_path = args.dump / "skilldata.json"
    dump_desc_path = args.dump / "skilldesc-en.json"
    for path in (dump_data_path, dump_desc_path):
        if not path.is_file():
            raise SystemExit(f"missing {path}")

    _, dump_data = _load(dump_data_path, "skilldata")
    _, dump_desc = _load(dump_desc_path, "skilldesc")
    data_doc, committed_data = _load(args.data_dir / "skilldata.json", "skilldata")
    desc_doc, committed_desc = _load(args.data_dir / "skilldesc-en.json", "skilldesc")

    sentinel = _no_split_sentinel(dump_data)
    kept, skipped = choose_ids(dump_data, committed_data)

    new_data, new_desc = OrderedDict(), OrderedDict()
    changed_fields: dict[str, int] = {}
    changed_ids: set[str] = set()

    for key in kept:
        prior_row = committed_data.get(key)
        prior_txt = committed_desc.get(key)
        row = convert_row(dump_data[key], prior_row, sentinel)

        entry = dump_desc.get(key, {})
        text = OrderedDict()
        text["id"] = row["id"]
        text["name"] = pick_name(entry.get("name", ""), prior_txt.get("name") if prior_txt else None)
        text["description"] = strip_markup(entry.get("description", ""))
        text["concise"] = strip_markup(entry.get("concise", ""))

        # Id 0 is the tool's empty-slot placeholder, not a skill. The client calls
        # it "(none)"; leave the committed wording alone.
        if key == "0" and prior_txt is not None:
            text = OrderedDict((f, prior_txt[f]) for f in DESC_FIELDS)

        new_data[key] = row
        new_desc[key] = text

        for field in DATA_FIELDS:
            if prior_row is not None and prior_row.get(field) != row[field]:
                changed_fields[field] = changed_fields.get(field, 0) + 1
                changed_ids.add(key)
        for field in ("name", "description", "concise"):
            if prior_txt is not None and prior_txt.get(field) != text[field]:
                changed_fields[field] = changed_fields.get(field, 0) + 1
                changed_ids.add(key)

    added = [k for k in kept if k not in committed_data]
    dropped = [k for k in committed_data if k not in dump_data]

    print(f"kept {len(kept)} skills ({len(added)} new), {len(changed_ids)} changed")
    for field, count in sorted(changed_fields.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f"  {field:<12} {count:>4}")
    if added:
        print(f"  added: " + ", ".join(f"{k} {new_desc[k]['name']}" for k in added))
    if dropped:
        print(f"  !! {len(dropped)} committed id(s) absent from the dump, kept as-is: "
              + ", ".join(dropped[:10]))
        for key in dropped:
            new_data[key] = committed_data[key]
            new_desc[key] = committed_desc[key]
    if skipped:
        names = [f"{k} {dump_desc.get(k, {}).get('name', '?')!r}" for k in skipped]
        print(f"  skipped {len(skipped)} new professionless id(s) (monster/environment): "
              + ", ".join(names))

    if not changed_ids and not added:
        print("nothing to import -- Data/ already matches the dump")
        return 0

    if args.dry_run:
        print("(dry run) wrote nothing")
        return 0

    data_doc["skilldata"] = OrderedDict(sorted(new_data.items(), key=lambda kv: int(kv[0])))
    desc_doc["skilldesc"] = OrderedDict(sorted(new_desc.items(), key=lambda kv: int(kv[0])))
    for path, doc in ((args.data_dir / "skilldata.json", data_doc),
                      (args.data_dir / "skilldesc-en.json", desc_doc)):
        path.write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
