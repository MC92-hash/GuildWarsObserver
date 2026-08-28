"""Check the skillpatches chain for fields an older patch would clobber.

SkillDatabase::GetView reconstructs a replay-date view by walking the patch files
backwards, newest first, and assigning each patched skill wholesale:

    for (const auto& [skillId, oldInfo] : patch.overrides)
        (*data)[skillId] = oldInfo;                  // SkillDatabase.cpp:644

`oldInfo` was materialised in LoadPatches from the *current* skill data with only
the fields that patch names overridden. So when a skill appears in two patches
and the older one does NOT name a field the newer one does, the older patch -
applied last, for a replay predating both - writes the current value back over
the newer patch's reversion.

Concretely: Black Spider Strike had its recharge changed in June and its energy
changed in August. For a replay from May, GetView applies August (energy -> 10),
then June, whose oldInfo carries the post-August energy of 5. The replay renders
5 energy for a skill that cost 10 at the time.

This could not happen while only one patch file existed. It can now, so this
checks for it. Run it after generating any new patch file.

    python scripts/check_skillpatches.py

Exit code 1 if any clobbering pair is found. Two ways to resolve one:

* Data fix, no rebuild: add the field to the OLDER patch, set to the value that
  was in force during that older era (which, since the field did not change then,
  is the newer patch's recorded old value). Forward-compatible - it stays correct
  if the loader is later fixed.
* Code fix, needs a rebuild: have LoadPatches keep the set of field names each
  patch actually names, and have GetView apply only those onto the accumulating
  view rather than assigning a whole SkillInfo.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PATCH_DIR = REPO / "Data" / "skillpatches"
DESC = REPO / "Data" / "skilldesc-en.json"


def main() -> int:
    patches = []
    for path in sorted(PATCH_DIR.glob("*.json")):
        stem = path.stem
        if len(stem) != 10 or stem[4] != "-" or stem[7] != "-":
            continue
        body = json.loads(path.read_text(encoding="utf-8"))
        patches.append((stem, body.get("skills", {})))
    patches.sort(key=lambda row: row[0])

    if len(patches) < 2:
        print(f"{len(patches)} patch file(s) -- nothing to compare")
        return 0

    try:
        desc = json.loads(DESC.read_text(encoding="utf-8"))["skilldesc"]
    except Exception:
        desc = {}
    name = lambda k: desc.get(k, {}).get("name", k)

    problems = []
    # For each pair (older, newer), a field the newer names and the older does not
    # is lost for any replay predating the older patch.
    for i, (older_date, older) in enumerate(patches):
        for newer_date, newer in patches[i + 1:]:
            for sid, newer_fields in newer.items():
                if sid not in older:
                    continue
                lost = set(newer_fields) - set(older[sid])
                if lost:
                    problems.append((older_date, newer_date, sid, sorted(lost),
                                     {f: newer_fields[f] for f in sorted(lost)}))

    if not problems:
        print(f"{len(patches)} patch files, no clobbering pairs found")
        return 0

    print(f"{len(problems)} clobbering pair(s) found:\n")
    for older_date, newer_date, sid, lost, values in problems:
        print(f"  {name(sid)} (id {sid})")
        print(f"    {newer_date} sets {lost}, {older_date} does not name "
              f"{'it' if len(lost) == 1 else 'them'}")
        print(f"    -> a replay before {older_date} renders the post-{newer_date} value")
        print(f"    -> fix: add {values} to {older_date}.json\n")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
