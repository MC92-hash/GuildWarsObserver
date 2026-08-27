"""Generate ``scripts/data/skill_meta.json`` from the client's skill data.

Two facts the event streams need and cannot derive, both stated outright by
``Data/skilldata.json``:

**Which skills are signets.** ``type == 21``. Deriving this from the archive is
hopeless -- a signet looks like any other zero-energy skill in the stream -- and
reading the flag covers every profession at once rather than needing a curated
list per card.

**Which PvP ids are twins of which base id.** A split skill is published twice,
and the recording carries whichever the player actually equipped. Measured on
the cripple prototype, **every credited Signet of Pious Restraint cripple came
from 3273, the PvP id, and none from 2014** -- a table listing only the base id
scores zero. ``combat_analytics._KD_TARGET`` works around this today by
hand-listing both ids of every split knockdown skill; ``split_id`` retires that.

The map is twin -> base, so a reader canonicalises by lookup-or-itself and a
skill table only ever needs the base id.

Same arrangement as the orchestrator's ``gen_skill_elites.py``: the source is
the client's own data file, the slim derived map is committed, and regenerating
is a manual step after a game update. See ``project_balance_update_procedure``.

    python scripts/gen_skill_meta.py
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# ``type`` values from the client's skill data. Only the ones a counter needs.
SKILL_TYPE_SIGNET = 21
SKILL_TYPE_FORM = 15
# Type 15 is every form in the game, including the Norn and Asura blessings.
# Only the Dervish avatars matter here, and they are the ones on profession 10.
PROFESSION_DERVISH = 10

DEFAULT_SOURCE = Path(__file__).resolve().parent.parent / "Data" / "skilldata.json"
DEFAULT_TARGET = Path(__file__).resolve().parent / "data" / "skill_meta.json"


def build_meta(source: Path) -> dict:
    raw = json.loads(source.read_text(encoding="utf-8-sig"))
    data = raw.get("skilldata")
    if not isinstance(data, dict):
        raise SystemExit(f"{source}: no 'skilldata' object")

    signets: list[int] = []
    avatars: list[int] = []
    twins: dict[str, int] = {}
    for key, entry in data.items():
        if not isinstance(entry, dict):
            continue
        try:
            skill_id = int(entry.get("id", key))
        except (TypeError, ValueError):
            continue
        if entry.get("type") == SKILL_TYPE_SIGNET:
            signets.append(skill_id)
        if (entry.get("type") == SKILL_TYPE_FORM
                and entry.get("profession") == PROFESSION_DERVISH):
            avatars.append(skill_id)
        # `pvp_split` marks the BASE skill and `split_id` names its PvP twin,
        # so the pair is read off the base entry and stored the other way round.
        if entry.get("pvp_split"):
            try:
                twin = int(entry.get("split_id", 0))
            except (TypeError, ValueError):
                twin = 0
            if twin > 0:
                twins[str(twin)] = skill_id
    return {
        "source": source.name,
        "signets": sorted(set(signets)),
        "avatars": sorted(set(avatars)),
        "pvp_twins": {k: twins[k] for k in sorted(twins, key=int)},
    }


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Generate scripts/data/skill_meta.json")
    p.add_argument("--source", default=str(DEFAULT_SOURCE))
    p.add_argument("--target", default=str(DEFAULT_TARGET))
    args = p.parse_args(argv)

    source = Path(args.source)
    if not source.exists():
        print(f"error: source not found: {source}", file=sys.stderr)
        return 2

    meta = build_meta(source)
    target = Path(args.target)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(meta, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {len(meta['signets'])} signet(s), {len(meta['avatars'])} avatar(s), "
          f"{len(meta['pvp_twins'])} PvP twin(s) -> {target}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
