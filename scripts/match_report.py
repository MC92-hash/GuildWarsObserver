"""Print the per-skill, interrupt and blind numbers for one recorded match.

Everything here is already computed by ``combat_analytics.build_from_match_dir``
and published into the ``stats/<YYYY-MM>.json`` sidecar, but the sidecar is only
written at upload time and the website only shows what its pages render.  This
reads a match folder straight off disk, so a question about a game can be
answered before anything is backfilled or deployed -- including for a recording
that was never published at all.

    python match_report.py "D:\\MatchArchive\\2026-08-27_Insel_der_Toten_17.41_[Men]vs[Crew]"
    python match_report.py <folder> --skill 220        # one skill, both teams
    python match_report.py <folder> --player "R O C K" # one player, full detail

Read-only.  It opens the recording and the skill-name table and writes nothing.
"""
from __future__ import annotations

import argparse
import io
import json
import sys
from pathlib import Path

from combat_analytics import build_from_match_dir

# The name table lives in the orchestrator, which is a sibling checkout rather
# than an installed package. Missing is not fatal: ids still identify a skill.
_NAMES_PATH = (Path(__file__).resolve().parents[2] / "gw-observer-orchestrator"
               / "src" / "watchtower" / "data" / "skill_names.json")

_PROFESSIONS = {1: "W", 2: "R", 3: "Mo", 4: "N", 5: "Me", 6: "E", 7: "A",
                8: "Rt", 9: "P", 10: "D"}


def _skill_names() -> dict:
    try:
        return json.loads(_NAMES_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _roster(infos: dict) -> dict:
    """(party_id, player_number) -> a printable label."""
    out = {}
    for party_id, party in infos.get("parties", {}).items():
        tag = infos.get("guilds", {}).get(party_id, {}).get("tag", "?")
        for player in party.get("PLAYER", ()):
            name = str(player.get("encoded_name", "")).split(" (")[0]
            prof = "{}/{}".format(
                _PROFESSIONS.get(player.get("primary"), "?"),
                _PROFESSIONS.get(player.get("secondary"), "?"))
            out[(party_id, player.get("player_number"))] = (
                f"{name} [{tag}] {prof}")
    return out


def _ascii(text: str) -> str:
    """Windows consoles are cp1252; a CJK guild tag must not crash a report."""
    return text.encode(sys.stdout.encoding or "ascii", "replace").decode(
        sys.stdout.encoding or "ascii")


def report(match_dir: Path, only_skill: int | None = None,
           only_player: str | None = None) -> int:
    infos = json.loads((match_dir / "infos.json").read_text(encoding="utf-8"))
    analytics = build_from_match_dir(infos, match_dir)
    if not analytics:
        print("No analytics: a required StoC stream is missing from this "
              "recording. Absent is not zero.")
        return 1

    names = _skill_names()
    label = lambda sid: names.get(str(sid), "?")  # noqa: E731
    roster = _roster(infos)
    winner = str(infos.get("winner_party_id", ""))
    guilds = infos.get("guilds", {})

    print(_ascii(f"{match_dir.name}"))
    print(f"  {infos.get('occasion', '?')} | {infos.get('match_duration', '?')} "
          f"| flux {infos.get('flux', '?')}")
    for party_id in sorted(p for p in guilds if p in infos.get("parties", {})):
        guild = guilds[party_id]
        mark = "  <-- WON" if party_id == winner else ""
        print(_ascii(f"  party {party_id}: {guild.get('name')} "
                     f"[{guild.get('tag')}]{mark}"))

    casts = analytics.get("skill_casts", {}).get("players", {})
    rows = {(party_id, row["player_number"]): row
            for party_id, party_rows in analytics["players"].items()
            for row in party_rows}
    cast_rows = {(party_id, row["player_number"]): row
                 for party_id, party_rows in casts.items()
                 for row in party_rows}

    def wanted(key) -> bool:
        if only_player is None:
            return True
        return only_player.lower() in roster.get(key, "").lower()

    print("\n--- casts, interrupts, blind ---")
    for key in sorted(rows, key=lambda k: (k[0], k[1])):
        if not wanted(key):
            continue
        row, name = rows[key], roster.get(key, f"party {key[0]} slot {key[1]}")
        skills = cast_rows.get(key, {}).get("skills", {})
        attacks = cast_rows.get(key, {}).get("attack_attempts", {})
        if only_skill is not None:
            sid = str(only_skill)
            if sid not in skills and sid not in attacks:
                continue

        print(_ascii(f"\n{name}"))
        blind_low = row.get("blind_seconds_modeled_low")
        if blind_low is not None and (blind_low or row.get(
                "blind_applications_received")):
            print(f"   blinded (MODELLED) {row['blind_applications_received']}x, "
                  f"{blind_low}-{row['blind_seconds_modeled_high']}s")
        if row.get("blind_applications_caused"):
            print(f"   blind applied (MODELLED) "
                  f"{row['blind_applications_caused']}x, "
                  f"{row['blind_seconds_caused_modeled_low']}-"
                  f"{row['blind_seconds_caused_modeled_high']}s")
        attempts = row.get("rupt_attempts", 0)
        if attempts:
            casting = row.get("rupt_attempts_on_casting_target", 0)
            landed = row.get("rupts_inferred", 0)
            rate = f"{landed / casting * 100:.0f}%" if casting else "n/a"
            print(f"   interrupts {landed} INFERRED from {attempts} attempts "
                  f"({casting} at a casting target, {rate})")

        for sid, counts in sorted(skills.items(), key=lambda kv: -kv[1][0]):
            if only_skill is not None and sid != str(only_skill):
                continue
            started, completed, interrupted = counts
            extra = f", {interrupted} interrupted" if interrupted else ""
            print(f"     {sid:>5} {label(sid):<26} {started:>3} cast, "
                  f"{completed:>3} completed{extra}")
        for sid, count in sorted(attacks.items(), key=lambda kv: -kv[1]):
            if only_skill is not None and sid != str(only_skill):
                continue
            # Attempts only -- ATTACK_SKILL_FINISHED is too sparse to classify.
            print(f"     {sid:>5} {label(sid):<26} {count:>3} attempts "
                  f"(attack skill)")

    audit = analytics.get("attribution", {})
    print("\n--- audit ---")
    for field in ("interrupt_events", "interrupt_inferred_unique",
                  "interrupt_inferred_ambiguous", "interrupt_inferred_none",
                  "rupt_attempts", "rupt_attempts_on_casting_target",
                  "blind_applications_modeled", "blind_casts_unmodelled",
                  "blind_windows_truncated_by_removal"):
        if field in audit:
            print(f"   {field:<38} {audit[field]}")
    if audit.get("blind_casts_unmodelled"):
        print("   (unmodelled = area or conditional blind sources, which name "
              "no victim and contribute no seconds)")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("match_dir", type=Path)
    parser.add_argument("--skill", type=int, default=None,
                        help="only this skill id")
    parser.add_argument("--player", default=None,
                        help="only players whose name contains this")
    args = parser.parse_args(argv)
    if not (args.match_dir / "infos.json").is_file():
        parser.error(f"not a match folder: {args.match_dir}")
    return report(args.match_dir, args.skill, args.player)


if __name__ == "__main__":
    raise SystemExit(main())
