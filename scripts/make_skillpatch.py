"""Generate a ``Data/skillpatches/<date>.json`` overlay from two skill-data dumps.

A replay has to render skills as they were on the day it was recorded. The
client does that by keeping only the *current* skill data plus a chain of
reverse deltas: ``SkillDatabase::GetView(y, m, d)`` starts from the current data
and re-applies every patch dated *after* the replay date, newest first, so each
patch file stores the values that were in force **before** it landed.

Building that file is a diff, and until now it was done by hand. The June 2026
update (commit c855cd8) produced ``Data/skillpatches/2026-06-24.json`` that way
and left no tooling behind, so the next person had to rediscover both the shape
of the file and which fields matter. This script is that step, written down.

Typical use, after overwriting Data/ with a fresh upstream dump:

    # the pre-patch pair is whatever is still committed
    python scripts/make_skillpatch.py --old-from-git HEAD \\
        --date 2026-08-26 --description "August 2026 balance update" --dry-run

    # happy with the summary? drop --dry-run
    python scripts/make_skillpatch.py --old-from-git HEAD \\
        --date 2026-08-26 --description "August 2026 balance update"

Or point it at two explicit pairs with --old-data/--old-desc/--new-data/--new-desc.

Self-check, run 2026-08-26 against the one patch file that already exists. Take
the post-patch pair out of c855cd8 and the pre-patch pair out of its parent, and
regenerate Data/skillpatches/2026-06-24.json:

    git show c855cd8:Data/skilldata.json    > new-skilldata.json
    git show c855cd8:Data/skilldesc-en.json > new-skilldesc.json
    python scripts/make_skillpatch.py --date 2026-06-24 \\
        --description "June 2026 balance update" --old-from-git c855cd8^ \\
        --new-data new-skilldata.json --new-desc new-skilldesc.json \\
        --out regen.json

Result: the same 129 skills, and **zero** numeric mismatches on any of them.
Thirteen descriptions differ, and those are the interesting part - the committed
text for Gust, Sandstorm, Cry of Frustration, Wastrel's Worry, Energy Surge,
Mistrust, Mistrust (PvP), Overload, Shatter Delusions, Enchanter's Conundrum,
Signet of Illusions, Ravenous Gaze and Maiming Strike matches neither dump.
Whoever built the June file rewrote those by hand, because upstream's pre-patch
wording did not actually describe the pre-patch behaviour: upstream had Cry of
Frustration as "interrupted and suffer 15...75 damage", when what it really did
was deal that damage to area foes as well, which is the whole point of the
change that followed.

So: trust this script for the numbers, and expect a manual pass over the text of
genuinely reworked skills. Stdlib only, no dependencies.

Two things this deliberately does NOT do. It cannot express a skill that did not
exist before the patch - the loader only overrides, it never deletes - so new
skills are reported in the summary and skipped. And it does not try to separate
a real reword from upstream editorial churn: "remove the cold-weapon
requirement" is a balance change with no numeric part, so text-only entries are
kept. The summary counts them so an unexpected pile is visible.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_NEW_DATA = REPO / "Data" / "skilldata.json"
DEFAULT_NEW_DESC = REPO / "Data" / "skilldesc-en.json"
PATCH_DIR = REPO / "Data" / "skillpatches"

# Every field SkillDatabase::LoadPatches understands, in the order it reads
# them, so a generated file lines up with the loader when someone diffs the two.
NUMERIC_FIELDS = (
    "energy", "activation", "recharge", "adrenaline",
    "sacrifice", "upkeep", "overcast", "is_elite",
    "type", "profession", "attribute",
)
TEXT_FIELDS = ("name", "description", "concise")


def _load_json(source: Path | str, envelope: str) -> dict:
    """Read one dump and return its ``{id: entry}`` body.

    Both files wrap their payload (``{"skilldata": {...}}`` /
    ``{"skilldesc": {...}}``); the envelope is required rather than guessed so a
    truncated or half-written file fails here instead of silently producing an
    empty patch.
    """
    text = Path(source).read_text(encoding="utf-8") if not isinstance(source, str) \
        else source
    raw = json.loads(text)
    body = raw.get(envelope)
    if not isinstance(body, dict) or not body:
        raise SystemExit(f"no '{envelope}' object found -- is this the right file?")
    return body


def _from_git(rev: str, path: str) -> str:
    """One file's contents at a revision, for diffing against the working tree."""
    try:
        out = subprocess.run(
            ["git", "-C", str(REPO), "show", f"{rev}:{path}"],
            capture_output=True, check=True,
        )
    except FileNotFoundError:
        raise SystemExit("git not found on PATH; pass --old-data/--old-desc instead")
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", "replace").strip()
        raise SystemExit(f"git show {rev}:{path} failed: {detail}")
    return out.stdout.decode("utf-8")


def build_overrides(
    old_data: dict, new_data: dict, old_desc: dict, new_desc: dict,
) -> tuple[dict[str, dict], dict[str, int], list[int]]:
    """Per changed skill, the fields that moved, set to their **old** values.

    Returns ``(overrides, per_field_counts, new_skill_ids)``. Keyed by skill id
    as a string because that is what the loader parses and what the June file
    already uses.
    """
    overrides: dict[str, dict] = {}
    counts: dict[str, int] = {}
    new_skills: list[int] = []

    ids = sorted(
        {int(k) for k in new_data if k.lstrip("-").isdigit()}
        | {int(k) for k in new_desc if k.lstrip("-").isdigit()}
    )

    for skill_id in ids:
        key = str(skill_id)
        old_row = old_data.get(key)
        new_row = new_data.get(key)
        old_txt = old_desc.get(key)
        new_txt = new_desc.get(key)

        # Absent before the patch: the loader can only override, so there is
        # nothing to write. Reported instead of dropped silently.
        if old_row is None and old_txt is None:
            new_skills.append(skill_id)
            continue

        changed: dict = {}
        for field in NUMERIC_FIELDS:
            if not isinstance(old_row, dict) or not isinstance(new_row, dict):
                continue
            if field not in old_row or field not in new_row:
                continue
            if old_row[field] != new_row[field]:
                changed[field] = old_row[field]
        for field in TEXT_FIELDS:
            if not isinstance(old_txt, dict) or not isinstance(new_txt, dict):
                continue
            if field not in old_txt or field not in new_txt:
                continue
            if old_txt[field] != new_txt[field]:
                changed[field] = old_txt[field]

        if changed:
            overrides[key] = changed
            for field in changed:
                counts[field] = counts.get(field, 0) + 1

    return overrides, counts, new_skills


def summarize(overrides: dict[str, dict], counts: dict[str, int],
              new_skills: list[int], desc: dict) -> str:
    lines = [f"{len(overrides)} skills changed"]
    for field, count in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
        lines.append(f"  {field:<12} {count:>4}")

    text_only = sum(
        1 for fields in overrides.values()
        if not any(f in fields for f in NUMERIC_FIELDS)
    )
    lines.append(f"  ({text_only} of those changed text only -- check for editorial churn)")

    if new_skills:
        names = []
        for skill_id in new_skills[:10]:
            entry = desc.get(str(skill_id))
            names.append(entry.get("name", str(skill_id))
                         if isinstance(entry, dict) else str(skill_id))
        more = "" if len(new_skills) <= 10 else f" (+{len(new_skills) - 10} more)"
        lines.append(f"  {len(new_skills)} skill(s) new since the old dump, no override "
                     f"emitted: {', '.join(names)}{more}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Build a skillpatches overlay from two skill-data dumps.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--date", required=True,
                        help="patch date, YYYY-MM-DD; also names the output file")
    parser.add_argument("--description", default="",
                        help='e.g. "August 2026 balance update"')
    parser.add_argument("--old-from-git", metavar="REV",
                        help="take the pre-patch pair from this revision "
                             "(e.g. HEAD, before you commit the new dump)")
    parser.add_argument("--old-data", type=Path, help="pre-patch skilldata.json")
    parser.add_argument("--old-desc", type=Path, help="pre-patch skilldesc-en.json")
    parser.add_argument("--new-data", type=Path, default=DEFAULT_NEW_DATA,
                        help=f"post-patch skilldata.json (default: {DEFAULT_NEW_DATA})")
    parser.add_argument("--new-desc", type=Path, default=DEFAULT_NEW_DESC,
                        help=f"post-patch skilldesc-en.json (default: {DEFAULT_NEW_DESC})")
    parser.add_argument("--out", type=Path,
                        help="output path (default: Data/skillpatches/<date>.json)")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the summary and write nothing")
    args = parser.parse_args(argv)

    try:
        year, month, day = (int(part) for part in args.date.split("-"))
    except ValueError:
        raise SystemExit(f"--date must be YYYY-MM-DD, got {args.date!r}")
    if not (1 <= month <= 12 and 1 <= day <= 31 and year > 0):
        raise SystemExit(f"--date is not a real date: {args.date!r}")

    if args.old_from_git:
        old_data = _load_json(_from_git(args.old_from_git, "Data/skilldata.json"),
                              "skilldata")
        old_desc = _load_json(_from_git(args.old_from_git, "Data/skilldesc-en.json"),
                              "skilldesc")
    elif args.old_data and args.old_desc:
        old_data = _load_json(args.old_data, "skilldata")
        old_desc = _load_json(args.old_desc, "skilldesc")
    else:
        raise SystemExit("pass --old-from-git REV, or both --old-data and --old-desc")

    new_data = _load_json(args.new_data, "skilldata")
    new_desc = _load_json(args.new_desc, "skilldesc")

    overrides, counts, new_skills = build_overrides(
        old_data, new_data, old_desc, new_desc)

    print(f"{args.date}: {summarize(overrides, counts, new_skills, new_desc)}")

    if not overrides:
        print("nothing changed -- refusing to write an empty patch", file=sys.stderr)
        return 1

    payload = {"date": args.date, "skills": overrides}
    if args.description:
        # Matches the June file's key order: date, description, skills.
        payload = {"date": args.date, "description": args.description,
                   "skills": overrides}

    out = args.out or (PATCH_DIR / f"{args.date}.json")
    if args.dry_run:
        print(f"(dry run) would write {out}")
        return 0

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
                   encoding="utf-8")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
