#!/usr/bin/env python3
"""Retag the ``occasion`` label on published index.json entries.

Edits index.json in place and republishes it. Archives are never touched, so
this is the cheap way to correct a label: the ``.tar.gz`` for the match keeps
whatever its recorded infos.json said, and only the index (which is what the
library actually reads) is corrected.

Why this exists: the occasion string is copied verbatim from the recording
plugin's infos.json into index.json, and the C++ replay browser keys its
filter leaves, their counts and the selection set on the *raw* string. So
"Automated  Tournament" (two spaces) renders as a second, visually identical
child row next to "Automated Tournament", with its own count and its own
checkbox. One stray space is enough to split a bucket.

Two modes, exactly one required:

    --normalize-whitespace
        Sweep every entry: strip, and collapse internal whitespace runs to a
        single space. Fixes split buckets without hardcoding any label.

    --folder SUBSTR --to "New Occasion"
        Retag entries whose folder name contains SUBSTR (case insensitive).
        Add --from "Old Occasion" to abort if the current label is not what
        you expected, rather than silently retagging the wrong match.

Dry run by default. Pass --apply to publish.

    py -3.11 scripts/retag_occasion.py --normalize-whitespace
    py -3.11 scripts/retag_occasion.py --normalize-whitespace --apply

Requires r2_config.env in the same directory (the same file the AT upload
script uses).
"""

import argparse
import io
import json
import sys
import time
from collections import Counter
from pathlib import Path

# Reuse the AT upload script's helpers - same R2 client, same index format,
# same single writer. Nothing here reimplements the index encoding.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from upload_to_r2 import (  # noqa: E402
    create_s3_client,
    fetch_remote_index,
    index_entry_fingerprint,
    load_config,
    normalize_occasion,
    serialize_index,
    write_index,
)


def fmt_size(bytes_: int) -> str:
    if bytes_ < 1024:
        return f"{bytes_} B"
    if bytes_ < 1024 * 1024:
        return f"{bytes_/1024:.1f} KB"
    return f"{bytes_/1024/1024:.1f} MB"


def plan_changes(entries: list[dict], args) -> tuple[list[tuple[int, str, str]], list[dict]]:
    """Return (changes, mismatches).

    ``changes`` is a list of (position in ``entries``, old occasion, new
    occasion) for every entry whose label would actually change. ``mismatches``
    are folder-matched entries rejected by the --from guard.
    """
    changes: list[tuple[int, str, str]] = []
    mismatches: list[dict] = []

    if args.normalize_whitespace:
        for i, entry in enumerate(entries):
            old = entry.get("occasion", "") or ""
            new = normalize_occasion(old)
            if new != old:
                changes.append((i, old, new))
        return changes, mismatches

    needle = args.folder.lower().strip()
    for i, entry in enumerate(entries):
        folder = entry.get("folder") or ""
        if needle not in folder.lower():
            continue
        old = entry.get("occasion", "") or ""
        if args.from_occasion is not None and old != args.from_occasion:
            mismatches.append(entry)
            continue
        if args.to != old:
            changes.append((i, old, args.to))
    return changes, mismatches


def find_collisions(entries: list[dict],
                    changes: list[tuple[int, str, str]]) -> list[tuple[int, str]]:
    """Changed entries whose *new* fingerprint collides with another entry.

    index_entry_fingerprint includes the occasion, so a retag moves an entry's
    dedupe identity. That can silently create a duplicate group as seen by
    ``upload_to_r2.py --cleanup-duplicates``, which would later drop one of
    them. Worth refusing by default.
    """
    projected = [dict(e) for e in entries]
    for i, _old, new in changes:
        projected[i]["occasion"] = new

    counts = Counter(index_entry_fingerprint(e) for e in projected)
    collisions: list[tuple[int, str]] = []
    for i, _old, _new in changes:
        fp = index_entry_fingerprint(projected[i])
        if counts[fp] > 1:
            collisions.append((i, fp))
    return collisions


def main() -> int:
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--normalize-whitespace", action="store_true",
        help="Strip and collapse internal whitespace in every entry's occasion.",
    )
    mode.add_argument(
        "--folder", type=str, default=None,
        help="Substring filter on the entry's folder name (case insensitive). "
             "Requires --to.",
    )
    parser.add_argument(
        "--to", type=str, default=None,
        help='New occasion label, e.g. "Automated Tournament". Used with --folder.',
    )
    parser.add_argument(
        "--from", dest="from_occasion", type=str, default=None,
        help="Guard: only retag folder-matched entries whose current occasion is "
             "exactly this. Any mismatch aborts.",
    )
    parser.add_argument(
        "--apply", action="store_true",
        help="Actually rewrite and publish index.json. Default is a dry run.",
    )
    parser.add_argument(
        "--yes", action="store_true",
        help="Skip the interactive confirmation prompt (use with --apply).",
    )
    parser.add_argument(
        "--allow-duplicate-fingerprint", action="store_true",
        help="Proceed even if a retag makes an entry collide with another "
             "entry's dedupe fingerprint.",
    )
    parser.add_argument(
        "--config", type=Path,
        default=Path(__file__).resolve().parent / "r2_config.env",
        help="Path to r2_config.env (defaults to alongside this script).",
    )
    parser.add_argument(
        "--backup-dir", type=Path,
        default=Path(__file__).resolve().parent / "purge_backups",
        help="Where to save a backup of the live index.json before writing.",
    )
    args = parser.parse_args()

    if args.folder is not None and args.to is None:
        parser.error("--folder requires --to")
    if args.normalize_whitespace and (args.to or args.from_occasion):
        parser.error("--to / --from only apply to --folder mode")
    if args.to is not None and args.to != normalize_occasion(args.to):
        parser.error(f"--to {args.to!r} has stray whitespace; did you mean "
                     f"{normalize_occasion(args.to)!r}?")

    cfg = load_config(args.config)
    s3 = create_s3_client(cfg)
    bucket = cfg["R2_BUCKET"]

    print(f"Bucket: {bucket}")
    print(f"Endpoint: {cfg['R2_ENDPOINT']}")
    print()

    entries = fetch_remote_index(s3, bucket)
    print(f"Loaded index.json with {len(entries)} entries "
          f"({fmt_size(len(serialize_index(entries)))} serialised).")

    changes, mismatches = plan_changes(entries, args)

    if mismatches:
        print()
        print(f"ERROR: {len(mismatches)} folder-matched entr(ies) do not have "
              f"occasion {args.from_occasion!r}:")
        for entry in mismatches:
            print(f"  {entry.get('date','')}  {entry.get('occasion','')!r}  "
                  f"{entry.get('folder','')}")
        print("Nothing was written. Correct --from or drop it if this is expected.")
        return 1

    if not changes:
        print("\nNothing to do - no entry's occasion would change.")
        return 0

    print()
    print(f"{len(changes)} entr(ies) to retag:")
    for i, old, new in changes:
        entry = entries[i]
        print(f"  {entry.get('date',''):10}  {old!r} -> {new!r}")
        print(f"              {entry.get('folder','')}")

    collisions = find_collisions(entries, changes)
    if collisions:
        print()
        print(f"WARNING: {len(collisions)} retag(s) would collide with another "
              f"entry's dedupe fingerprint:")
        for i, fp in collisions:
            print(f"  {entries[i].get('folder','')}  ->  {fp}")
        print("A later `upload_to_r2.py --cleanup-duplicates` could drop one of them.")
        if not args.allow_duplicate_fingerprint:
            print("Nothing was written. Re-run with --allow-duplicate-fingerprint "
                  "if this is intended.")
            return 1

    projected = [dict(e) for e in entries]
    for i, _old, new in changes:
        projected[i]["occasion"] = new
    old_size = len(serialize_index(entries))
    new_size = len(serialize_index(projected))
    print()
    print(f"Index size: {fmt_size(old_size)} -> {fmt_size(new_size)} "
          f"({new_size - old_size:+d} bytes)")

    if not args.apply:
        print()
        print("[DRY RUN] Nothing was written. Re-run with --apply.")
        return 0

    # Always back up the live index before mutating it. Republishing is a
    # single put; getting the old bytes back afterwards is not.
    args.backup_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    backup_path = args.backup_dir / f"index_pre_retag_{stamp}.json"
    backup_path.write_text(json.dumps({"matches": entries}, indent=2), encoding="utf-8")
    print(f"\nBackup saved: {backup_path}")

    if not args.yes:
        print()
        print(f"About to retag {len(changes)} entr(ies) and republish index.json.")
        answer = input("Type 'RETAG' to proceed: ").strip()
        if answer != "RETAG":
            print("Aborted.")
            return 1

    # Mutate in place and republish the whole list, never a filtered subset.
    for i, _old, new in changes:
        entries[i]["occasion"] = new

    # Through write_index so this publishes the same shape and encoding as
    # every other writer, and so index.json.gz is refreshed alongside it. A
    # hand-rolled put_object here would re-inflate a 12 MB object for every
    # client.
    written = write_index(s3, bucket, entries)
    print(f"\nWrote index.json ({len(entries)} entries, {fmt_size(written)}) "
          f"and index.json.gz.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
