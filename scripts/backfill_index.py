"""Backfill guild capes and the desktop preview stats onto published matches.

`upload_to_r2.py` only builds an index entry for a match it is *uploading*;
every already-published match is carried through byte-for-byte
(`kept_remote` in `cmd_upload`). So when `build_index_entry` learned to emit
`guilds[*].cape` and `parties[*].PLAYER[*].preview_stats`, the whole existing
archive kept its old field-less entries -- measured at 0 of 2,643 -- and the
desktop match library renders "-" for every historical match. This fills that
in, in place, without re-uploading a single archive.

Three sources, cheapest first, because they cost wildly different amounts:

  1. **Local recordings.** `MATCH_SOURCE_DIR` and `POST_UPLOAD_ARCHIVE_DIR`
     still hold `infos.json` for most of the archive (89% when the stats
     backfill measured it). A file walk, no downloads. Gives capes *and*
     preview stats.

  2. **The stats sidecar.** `STATS_PLAYER_FIELDS` is a superset of the six
     preview counters, already published and already backfilled, joined back
     to the index on `player_number`. Six small objects for the whole archive,
     so this is nearly free -- but it is per-player, so it cannot supply
     capes, which are guild-level.

  3. **`--from-r2`**, for whatever the first two missed. Pulls each match's
     `.tar.gz` and extracts `infos.json`. The stats backfill measured 0.79 GB
     for its 290 stragglers.

Only `cape` and `preview_stats` are ever written. Existing entries are
patched in place rather than rebuilt, so no other field can drift -- which
matters, because `recorded_players`, `size_bytes`, `date` and `folder` drive
the supersede path, dedup, and sidecar shard selection elsewhere.

Safe to re-run: patching an entry that already carries the fields just writes
the same values back.

    python backfill_index.py                     # dry run, local + sidecar
    python backfill_index.py --apply
    python backfill_index.py --apply --month 2026-08
    python backfill_index.py --apply --from-r2   # finish the stragglers
"""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path

# Reuse the real producers rather than reimplementing them: a backfill that
# writes a subtly different shape from the live uploader is worse than no
# backfill, because the difference would only surface as odd numbers later.
from upload_to_r2 import (  # noqa: E402  (same directory, run as a script)
    build_cape,
    build_preview_stats,
    create_s3_client,
    fetch_remote_index,
    fetch_remote_stats,
    load_config,
    month_of_entry,
    sanitize_folder_name,
    serialize_index,
    write_index,
)

# And reuse the resolver the stats backfill already proved against this
# archive, rather than growing a second one that drifts from it.
from backfill_stats import (  # noqa: E402
    default_env_file,
    infos_from_archive,
    local_infos,
    read_infos,
)

PREVIEW_FROM_STATS = (
    "interrupted_count", "cancelled_skills_count", "skills_finished",
    "total_damage_received", "total_healing_dealt", "total_healing_received",
)


def patch_from_infos(entry: dict, infos: dict) -> tuple[int, int]:
    """Write cape + preview_stats onto `entry` from its `infos.json`.

    Returns `(capes written, player preview arrays written)`.
    """
    capes = 0
    guilds_raw = infos.get("guilds")
    if isinstance(guilds_raw, dict):
        for guild_key, guild_entry in (entry.get("guilds") or {}).items():
            guild_obj = guilds_raw.get(guild_key)
            if not isinstance(guild_obj, dict) or not isinstance(guild_entry, dict):
                continue
            cape = build_cape(guild_obj)
            if cape is not None:
                guild_entry["cape"] = cape
                capes += 1

    players = 0
    parties_raw = infos.get("parties")
    if isinstance(parties_raw, dict):
        for party_id, party_entry in (entry.get("parties") or {}).items():
            party_obj = parties_raw.get(party_id)
            if not isinstance(party_obj, dict) or not isinstance(party_entry, dict):
                continue
            source = [p for p in party_obj.get("PLAYER", []) if isinstance(p, dict)]
            # Join on player_number, the same key the stats sidecar uses, so
            # the two objects need not agree on array order. Positional
            # fallback only when the numbers cannot identify a player.
            by_number = {}
            for p in source:
                n = p.get("player_number")
                if n is not None and n not in by_number:
                    by_number[n] = p
            for i, target in enumerate(party_entry.get("PLAYER", [])):
                if not isinstance(target, dict):
                    continue
                src = by_number.get(target.get("player_number"))
                if src is None and i < len(source):
                    src = source[i]
                if src is None:
                    continue
                preview = build_preview_stats(src)
                if preview is not None:
                    target["preview_stats"] = preview
                    players += 1
    return capes, players


def patch_from_stats(entry: dict, stats_match: dict) -> int:
    """Write preview_stats onto `entry` from a stats sidecar row. No capes."""
    players = 0
    rows_by_party = stats_match.get("players")
    if not isinstance(rows_by_party, dict):
        return 0
    for party_id, party_entry in (entry.get("parties") or {}).items():
        rows = rows_by_party.get(party_id)
        if not isinstance(rows, list) or not isinstance(party_entry, dict):
            continue
        by_number = {r.get("player_number"): r for r in rows if isinstance(r, dict)}
        for target in party_entry.get("PLAYER", []):
            if not isinstance(target, dict) or "preview_stats" in target:
                continue
            row = by_number.get(target.get("player_number"))
            if not row:
                continue
            # Absent means "not recorded", which is not zero -- the client
            # renders "-" for None and a number for 0.
            preview = [row.get(f) for f in PREVIEW_FROM_STATS]
            if any(v is not None for v in preview):
                target["preview_stats"] = preview
                players += 1
    return players


def entry_is_complete(entry: dict) -> bool:
    """True when nothing in this entry is still missing a preview field."""
    for guild in (entry.get("guilds") or {}).values():
        if isinstance(guild, dict) and "cape" not in guild:
            return False
    for party in (entry.get("parties") or {}).values():
        if not isinstance(party, dict):
            continue
        for player in party.get("PLAYER", []):
            if isinstance(player, dict) and "preview_stats" not in player:
                return False
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Backfill guild capes and preview stats onto index.json",
    )
    parser.add_argument("--apply", action="store_true",
                        help="Actually publish. Without it this is a dry run.")
    parser.add_argument("--month", action="append", default=None,
                        metavar="YYYY-MM",
                        help="Limit to a month (repeatable).")
    parser.add_argument("--limit", type=int, default=None,
                        help="Stop after N matches (for a smoke test).")
    parser.add_argument("--from-r2", action="store_true",
                        help="Download archives for matches the local walk "
                             "and the stats sidecar could not complete.")
    parser.add_argument("--no-stats", action="store_true",
                        help="Skip the stats-sidecar source.")
    parser.add_argument("--config", type=Path, default=None,
                        help="Path to r2_config.env")
    args = parser.parse_args(argv)

    config = load_config(args.config or default_env_file())
    missing = [k for k in ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY", "R2_SECRET_KEY")
               if k not in config]
    if missing:
        print(f"Error: missing config values: {', '.join(missing)}")
        return 1
    s3 = create_s3_client(config)
    bucket = config["R2_BUCKET"]

    print("Fetching remote index.json...")
    entries = fetch_remote_index(s3, bucket)
    print(f"  {len(entries)} entries in the index.")
    if not entries:
        print("Nothing to do.")
        return 0

    wanted = entries
    if args.month:
        months = set(args.month)
        wanted = [e for e in entries if month_of_entry(e) in months]
        print(f"  {len(wanted)} in {', '.join(sorted(months))}")
    todo = [e for e in wanted if not entry_is_complete(e)]
    print(f"  {len(todo)} still missing capes or preview stats.")
    if args.limit:
        todo = todo[:args.limit]
        print(f"  limited to {len(todo)}")
    if not todo:
        print("\nEvery entry already carries both. Nothing to do.")
        return 0

    print("\nResolving local recordings...")
    local = local_infos(config)

    capes = players = 0
    from_local = from_stats = from_r2 = 0
    unresolved: list[dict] = []

    for entry in todo:
        folder = entry.get("folder", "")
        path = local.get(folder) or local.get(sanitize_folder_name(folder))
        infos = read_infos(path) if path else None
        if infos is None:
            unresolved.append(entry)
            continue
        c, p = patch_from_infos(entry, infos)
        capes += c
        players += p
        from_local += 1
    print(f"  {from_local} from local recordings, {len(unresolved)} unresolved.")

    # Sidecar pass: preview stats only, but six objects for the whole archive.
    if unresolved and not args.no_stats:
        print("\nFilling preview stats from the stats sidecar...")
        by_month: dict[str, list[dict]] = defaultdict(list)
        for entry in unresolved:
            by_month[month_of_entry(entry)].append(entry)
        for month in sorted(m for m in by_month if m):
            # fetch_remote_stats returns the {folder: row} mapping itself,
            # not the enclosing {"schema", "month", "matches"} document.
            shard = fetch_remote_stats(s3, bucket, month)
            hit = 0
            for entry in by_month[month]:
                match = shard.get(entry.get("folder", ""))
                if isinstance(match, dict):
                    got = patch_from_stats(entry, match)
                    if got:
                        players += got
                        hit += 1
            from_stats += hit
            print(f"  {month}: {hit} of {len(by_month[month])} matched")

    # Anything still incomplete needs its archive. Capes only ever come from
    # here or from a local recording.
    still = [e for e in unresolved if not entry_is_complete(e)]
    if still and args.from_r2:
        print(f"\nDownloading {len(still)} archive(s) from R2...")
        for entry in still:
            folder = entry.get("folder", "")
            infos = infos_from_archive(s3, bucket, folder)
            if infos is None:
                continue
            c, p = patch_from_infos(entry, infos)
            capes += c
            players += p
            from_r2 += 1
        print(f"  {from_r2} recovered from archives.")
    elif still:
        print(f"\n{len(still)} match(es) still incomplete "
              f"(no local recording; capes need one). Re-run with --from-r2.")

    remaining = sum(1 for e in wanted if not entry_is_complete(e))
    body = serialize_index(entries)
    print(f"\nPatched {capes} cape(s) and {players} player preview array(s)")
    print(f"  sources: {from_local} local, {from_stats} sidecar, {from_r2} archive")
    print(f"  {remaining} entry(ies) still incomplete after this pass")
    print(f"  index.json would be {len(body)/1048576:.2f} MB ({len(entries)} entries)")

    if not args.apply:
        print("\n[DRY RUN] Nothing was written. Re-run with --apply.")
        return 0

    print("\nPublishing index.json...")
    written = write_index(s3, bucket, entries)
    print(f"Index updated ({written/1048576:.2f} MB).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
