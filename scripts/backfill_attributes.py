"""Backfill the per-match attribute and rune object over matches already uploaded.

`upload_to_r2.py` publishes `attributes/<folder>.json` for every match it
uploads. This fills in the archive that was uploaded before it did.

**Slower than the other backfills, by a lot.** Each match is a full replay
analysis -- agent and StoC parsing, the combat log, the max-HP breakpoint solve,
the armour solve, then the build search -- run headless by the desktop app.
Measured at ~10 s per match on a Debug build of a five-minute recording, and a
long match costs more. Budget minutes, not seconds, and use `--limit` first.

Unlike equipment, this does NOT need the recorder to have written anything new:
attributes are solved from the combat log, which every recording has carried
since the beginning. The real floor is whether a match has enough combat to read
magnitudes from, and the solver decides that per match rather than by date.

Safe to re-run. Objects are keyed by folder name, so a second pass overwrites
the same key.

    python backfill_attributes.py --limit 5           # dry run, five matches
    python backfill_attributes.py --apply --limit 5
    python backfill_attributes.py --apply --since 2026-08-01
    python backfill_attributes.py --apply --only-missing
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from upload_to_r2 import (  # noqa: E402  (same directory, run as a script)
    attributes_key,
    build_attributes_entry,
    create_s3_client,
    fetch_remote_index,
    load_config,
    month_of_entry,
    object_exists,
    observer_exe,
    upload_attributes,
)
from backfill_stats import default_env_file, local_infos, read_infos


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--apply", action="store_true",
                        help="actually write the objects (default is a dry run)")
    parser.add_argument("--month", action="append", metavar="YYYY-MM",
                        help="restrict to this month; repeatable")
    parser.add_argument("--since", metavar="YYYY-MM-DD",
                        help="only matches recorded on or after this date")
    parser.add_argument("--until", metavar="YYYY-MM-DD",
                        help="only matches recorded on or before this date")
    parser.add_argument("--limit", type=int, default=0,
                        help="stop after N matches. Worth using: this pass is "
                             "minutes per hundred matches, not seconds")
    parser.add_argument("--only-missing", action="store_true",
                        help="skip matches that already have an attribute object")
    parser.add_argument("--config", default=None,
                        help=f"path to r2_config.env (default: {default_env_file()})")
    args = parser.parse_args(argv)

    config = load_config(default_env_file() if args.config is None
                         else Path(args.config))
    required = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY", "R2_SECRET_KEY")
    absent = [k for k in required if k not in config]
    if absent:
        print(f"Error: missing config values: {', '.join(absent)}")
        return 1

    # Named up front rather than discovered one failure at a time: without the
    # binary every match would report "nothing solved" and the run would look
    # like a data problem instead of a missing build.
    exe = observer_exe(config)
    if exe is None:
        print("Error: no GW Observer binary found. Build it, or set OBSERVER_EXE "
              "in r2_config.env to the path of GuildWarsObserver.exe.")
        return 1
    print(f"Solver: {exe}")

    bucket = config["R2_BUCKET"]
    s3 = create_s3_client(config)

    print("Fetching remote index.json...")
    entries = fetch_remote_index(s3, bucket)
    if not entries:
        print("index.json is empty or unreadable; nothing to backfill.")
        return 1
    indexed = {e["folder"]: e for e in entries if e.get("folder")}
    print(f"  {len(indexed):,} indexed match(es)")

    months = set(args.month or ())
    if months:
        indexed = {f: e for f, e in indexed.items() if month_of_entry(e) in months}
        print(f"  {len(indexed):,} after --month {sorted(months)}")
    if args.since:
        indexed = {f: e for f, e in indexed.items()
                   if (e.get("date") or "") >= args.since}
        print(f"  {len(indexed):,} after --since {args.since}")
    if args.until:
        indexed = {f: e for f, e in indexed.items()
                   if "" < (e.get("date") or "") <= args.until}
        print(f"  {len(indexed):,} after --until {args.until}")

    print("Scanning local recordings...")
    local = local_infos(config)

    work = sorted(set(indexed) & set(local))
    print(f"\nindexed and local : {len(work):,}")

    if args.only_missing:
        before = len(work)
        work = [f for f in work if not object_exists(s3, bucket, attributes_key(f))]
        print(f"--only-missing    : {len(work):,} (skipped {before - len(work):,})")

    if args.limit:
        work = work[:args.limit]
        print(f"--limit           : {len(work)} match(es)")

    written = unsolved = unreadable = 0
    total_bytes = 0

    for index, folder in enumerate(work, start=1):
        infos = read_infos(local[folder])
        if infos is None:
            unreadable += 1
            continue

        attributes = build_attributes_entry(infos, local[folder].parent, config)
        if not attributes:
            # Nothing to solve. A match with little combat gives the providers
            # no magnitudes to read, and that is a fact about the match.
            unsolved += 1
            print(f"[{index}/{len(work)}] {folder}: nothing solved")
            continue

        body = json.dumps(attributes, ensure_ascii=False, separators=(",", ":"))
        total_bytes += len(body.encode("utf-8"))
        print(f"[{index}/{len(work)}] {folder}: "
              f"{len(attributes.get('players', {}))} players, {len(body) / 1024:.1f} KB")

        if args.apply:
            try:
                upload_attributes(s3, bucket, folder, attributes)
            except Exception as e:
                print(f"    ERROR: {type(e).__name__}: {e}")
                return 1
        written += 1

    print(f"\nsolved          : {written:,}")
    print(f"nothing to solve: {unsolved:,}")
    print(f"unreadable      : {unreadable:,}")
    print(f"total published : {total_bytes / 1024 / 1024:.2f} MB")
    if not args.apply:
        print("\n[DRY RUN] Nothing was written. Re-run with --apply.")
    else:
        print("\nDone.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
