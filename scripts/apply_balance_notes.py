"""Apply a hand-transcribed balance-notes table to Data/skilldata.json.

Normally the base data is replaced wholesale with a fresh upstream dump from
build-wars/gw-skilldata and scripts/make_skillpatch.py diffs the two. When
upstream lags a patch - as it did for 2026-08-26, where the data file had not
moved since 2026-07-21 - this applies the numbers straight from the official
skill update notes instead, and emits the same reverse-delta overlay.

    python scripts/apply_balance_notes.py --table scripts/balance_2026-08-26.json --dry-run
    python scripts/apply_balance_notes.py --table scripts/balance_2026-08-26.json

The table is data, not code, so a transcription error is reviewable in a diff
rather than buried in a script. Each entry names a skill and carries both halves
of the change as the notes state it:

    {"skill": "Mantis Touch", "expect": {"recharge": 20}, "set": {"recharge": 10}}

**`expect` is the point.** The notes say "reduce recharge from 20 to 10", so if
our data does not already read 20 then either the skill was misidentified or the
base data is not the version the notes describe - and writing 10 over it would
silently corrupt the file. Every entry is checked before anything is written,
and a single mismatch aborts the whole run. Transcribing ~60 changes by hand is
exactly the kind of job that needs a tripwire.

Scope of what this can and cannot do, worth stating plainly:

* It only touches fields that exist in skilldata.json - energy, activation,
  recharge, adrenaline, attribute, sacrifice, upkeep, overcast. Changes that live
  purely in a skill's description (damage numbers, durations, conditions applied,
  reworded behaviour) are NOT representable here and are listed in the table's
  "descriptive_only" section for the record, not applied.
* A change aimed at a PvP version ArenaNet has only just split off cannot be
  applied at all: that skill id does not exist yet and cannot be derived. Those
  go in "pending_split" and wait for upstream.

Descriptions are deliberately left alone. The overlay's job is making *old*
replays render correctly, and for that the old text is already in place and
correct. Inventing new wording would risk exactly the failure the June patch
file documents, where upstream's own text did not describe real behaviour.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SKILLDATA = REPO / "Data" / "skilldata.json"
SKILLDESC = REPO / "Data" / "skilldesc-en.json"
PATCH_DIR = REPO / "Data" / "skillpatches"

APPLICABLE = ("energy", "activation", "recharge", "adrenaline",
              "attribute", "sacrifice", "upkeep", "overcast",
              "is_elite", "type", "profession")


def load(path: Path, envelope: str) -> tuple[dict, dict]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    body = raw.get(envelope)
    if not isinstance(body, dict) or not body:
        raise SystemExit(f"{path}: no '{envelope}' object")
    return raw, body


def resolve(name: str, desc: dict) -> list[str]:
    """Skill ids for an exact name. A name should identify exactly one skill;
    PvP variants carry their own name ('Mistrust (PvP)') so they resolve
    separately and are addressed explicitly by the table."""
    return [k for k, v in desc.items()
            if isinstance(v, dict) and v.get("name", "").strip() == name]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--table", type=Path, required=True)
    parser.add_argument("--skilldata", type=Path, default=SKILLDATA)
    parser.add_argument("--skilldesc", type=Path, default=SKILLDESC)
    parser.add_argument("--out-patch", type=Path,
                        help="overlay path (default: Data/skillpatches/<date>.json)")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    table = json.loads(args.table.read_text(encoding="utf-8"))
    date = table["date"]
    changes = table["changes"]

    raw_data, data = load(args.skilldata, "skilldata")
    _, desc = load(args.skilldesc, "skilldesc")

    # ---- resolve and validate everything before writing anything ----
    planned: list[tuple[str, str, dict, dict]] = []   # (id, name, set, old)
    problems: list[str] = []

    for entry in changes:
        name = entry["skill"]
        ids = resolve(name, desc)
        if len(ids) != 1:
            problems.append(f"{name!r}: resolved to {len(ids)} ids {ids} -- expected exactly 1")
            continue
        skill_id = ids[0]
        row = data.get(skill_id)
        if not isinstance(row, dict):
            problems.append(f"{name!r} (id {skill_id}): no skilldata entry")
            continue

        for field, want in entry.get("expect", {}).items():
            if field not in row:
                problems.append(f"{name!r}: no field {field!r} in skilldata")
            elif row[field] != want:
                problems.append(
                    f"{name!r} (id {skill_id}): notes say {field} was {want}, "
                    f"data says {row[field]} -- refusing to overwrite")

        new = entry.get("set", {})
        for field in new:
            if field not in APPLICABLE:
                problems.append(f"{name!r}: field {field!r} is not applicable here")

        old = {f: row[f] for f in new if f in row and row[f] != new[f]}
        planned.append((skill_id, name, new, old))

    if problems:
        print(f"{len(problems)} problem(s) -- nothing written:", file=sys.stderr)
        for line in problems:
            print(f"  {line}", file=sys.stderr)
        return 1

    applied = [p for p in planned if p[3]]
    noop = [p for p in planned if not p[3]]

    print(f"{date}: {len(planned)} entries, {len(applied)} change a value, "
          f"{len(noop)} already at the new value")
    for _, name, new, old in applied:
        pairs = ", ".join(f"{f} {old[f]} -> {new[f]}" for f in old)
        print(f"  {name:<28} {pairs}")
    if noop:
        print("  already current: " + ", ".join(n for _, n, _, _ in noop))

    pending = table.get("pending_split", [])
    descriptive = table.get("descriptive_only", [])
    print(f"\nnot applied: {len(pending)} awaiting a new PvP split id, "
          f"{len(descriptive)} description-only")

    if args.dry_run:
        print("\n(dry run) nothing written")
        return 0

    overlay: dict[str, dict] = {}
    for skill_id, _, new, old in planned:
        if not old:
            continue
        data[skill_id].update(new)
        overlay[skill_id] = old

    if not overlay:
        print("nothing to write", file=sys.stderr)
        return 1

    # indent=1 round-trips this file byte-for-byte, so the diff shows only the
    # values that moved instead of a 500 KB reformat. Preserve whether the
    # original ended in a newline rather than imposing one.
    original = args.skilldata.read_text(encoding="utf-8")
    body = json.dumps(raw_data, indent=1, ensure_ascii=False)
    args.skilldata.write_text(
        body + ("\n" if original.endswith("\n") else ""), encoding="utf-8")

    out = args.out_patch or (PATCH_DIR / f"{date}.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    payload = {"date": date,
               "description": table.get("description", ""),
               "skills": overlay}
    out.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
                   encoding="utf-8")
    print(f"\nwrote {args.skilldata}\nwrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
