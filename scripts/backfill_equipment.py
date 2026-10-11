"""Backfill the per-match equipment object over matches already uploaded.

`upload_to_r2.py` publishes `equipment/<folder>.json` for every match it uploads,
but it only started doing so once the website had somewhere to show it. This
fills in what is already in the archive.

**Local recordings only, and that is the whole story.** The equipment stream
lives in `StoC/equipment_events.txt.gz` inside the recording folder, and the
recorder only started writing it on 2026-08-17 -- measured at 36 of 236 local
folders. Everything older has no file to backfill, so there is no `--from-r2`
here: downloading a tar to discover it holds nothing is a slow way to learn what
the date already says.

Safe to re-run. Objects are keyed by folder name, so a second pass over the same
match overwrites the same key with the same content.

    python backfill_equipment.py                     # dry run
    python backfill_equipment.py --apply
    python backfill_equipment.py --apply --month 2026-08
    python backfill_equipment.py --apply --since 2026-08-20
    python backfill_equipment.py --apply --only-missing
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

# The same producers the live uploader uses. A backfill that writes a subtly
# different shape is worse than no backfill: the difference only surfaces later,
# as an item panel that renders wrong on some matches and not others.
from upload_to_r2 import (  # noqa: E402  (same directory, run as a script)
    build_equipment_entry,
    create_s3_client,
    equipment_key,
    fetch_remote_index,
    load_config,
    month_of_entry,
    object_exists,
    upload_equipment,
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
                        help="stop after N matches (smoke test)")
    parser.add_argument("--only-missing", action="store_true",
                        help="skip matches that already have an equipment object")
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

    # Dates are ISO, so a string compare is a date compare. An entry with no
    # usable date is dropped rather than kept: a bound nobody can evaluate is
    # not a bound, and silently publishing past it is the failure worth
    # avoiding here.
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

    # Only ever write folders index.json knows about. Local disks also hold
    # rejects and superseded duplicates, and an equipment object for a match the
    # website cannot show is dead weight nothing will ever collect.
    work = sorted(set(indexed) & set(local))
    print(f"\nindexed and local : {len(work):,}")

    if args.only_missing:
        before = len(work)
        work = [f for f in work if not object_exists(s3, bucket, equipment_key(f))]
        print(f"--only-missing    : {len(work):,} (skipped {before - len(work):,})")

    if args.limit:
        work = work[:args.limit]
        print(f"--limit           : {len(work)} match(es)")

    written = 0
    no_stream = 0
    unreadable = 0
    total_bytes = 0

    for index, folder in enumerate(work, start=1):
        infos = read_infos(local[folder])
        if infos is None:
            unreadable += 1
            continue

        equipment = build_equipment_entry(infos, local[folder].parent)
        if not equipment:
            # A recording from before the stream existed. Expected, not an
            # error, and it must not be written as an empty kit.
            no_stream += 1
            continue

        body = json.dumps(equipment, ensure_ascii=False, separators=(",", ":"))
        total_bytes += len(body.encode("utf-8"))
        players = len(equipment.get("players", {}))
        items = len(equipment.get("items", {}))
        print(f"[{index}/{len(work)}] {folder}: {players} players, {items} items, "
              f"{len(body) / 1024:.1f} KB")

        if args.apply:
            try:
                upload_equipment(s3, bucket, folder, equipment)
            except Exception as e:
                print(f"    ERROR: {type(e).__name__}: {e}")
                return 1
        written += 1

    print(f"\nwith equipment  : {written:,}")
    print(f"no stream       : {no_stream:,}")
    print(f"unreadable      : {unreadable:,}")
    print(f"total published : {total_bytes / 1024 / 1024:.2f} MB")
    if not args.apply:
        print("\n[DRY RUN] Nothing was written. Re-run with --apply.")
    else:
        print("\nDone.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
