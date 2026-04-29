#!/usr/bin/env python3
"""Re-upload a single match to R2 after a local metadata fix, overwriting
the existing archive and patching the matching entry in index.json.

Written to repair the 2026-04-18_Isle_of_Wurms_23.45_[LaG]vs[MT] upload whose
infos.json got "General Scrimmage" from a Toolbox has_pending_match_info race
when it should have been "C AT". The fix is also applicable to other one-off
corrections: edit the local infos.json first, then run this script pointing at
the match folder.

Usage:
    python fix_mislabeled_upload.py <match_dir> [--dry-run]
"""

import argparse
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from upload_to_r2 import (  # noqa: E402
    load_config,
    create_s3_client,
    fetch_remote_index,
    read_infos_json,
    build_index_entry,
    create_tar_gz,
    sanitize_folder_name,
    upload_file,
    upload_index,
    object_exists,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("match_dir", help="Path to the local match folder (with fixed infos.json)")
    parser.add_argument("--dry-run", action="store_true", help="Don't upload; just print what would happen")
    parser.add_argument(
        "--rename-from",
        default="",
        help="If set, also delete the old R2 archive matches/<old>.tar.gz and "
             "drop the matching index entry. Use this when the local folder was "
             "renamed (e.g. date corrected) so both keys get cleaned up.",
    )
    parser.add_argument(
        "--env",
        default=str(Path(__file__).resolve().parent / "r2_config.env"),
        help="Path to r2_config.env",
    )
    args = parser.parse_args()

    match_dir = Path(args.match_dir).resolve()
    if not match_dir.is_dir():
        print(f"Error: match directory does not exist: {match_dir}")
        return 1
    if not (match_dir / "infos.json").is_file():
        print(f"Error: no infos.json in {match_dir}")
        return 1

    config = load_config(Path(args.env))
    for key in ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY", "R2_SECRET_KEY"):
        if not config.get(key):
            print(f"Error: missing config key {key}")
            return 1

    infos = read_infos_json(match_dir)
    if infos is None:
        print("Error: could not parse infos.json")
        return 1

    print(f"Match folder: {match_dir.name}")
    print(f"  occasion (fixed): {infos.get('occasion', '<empty>')}")
    print(f"  map_id: {infos.get('map_id', 0)}")
    print(f"  duration: {infos.get('match_duration', '')}")
    print()

    s3 = create_s3_client(config)
    bucket = config["R2_BUCKET"]

    safe_name = sanitize_folder_name(match_dir.name)
    r2_key = f"matches/{safe_name}.tar.gz"
    renamed = safe_name != match_dir.name

    print(f"R2 bucket:  {bucket}")
    print(f"R2 key:     {r2_key}")
    if renamed:
        print(f"URL-safe name used for tar entries: {safe_name}")
    print()

    # Fetch current index
    print("Fetching remote index.json...")
    remote_entries = fetch_remote_index(s3, bucket)
    print(f"  {len(remote_entries)} entries in index")

    # Find the existing entry (by folder name, either original or sanitized)
    entry_idx = None
    for i, e in enumerate(remote_entries):
        if e.get("folder") in (match_dir.name, safe_name):
            entry_idx = i
            break

    current_remote_occasion = remote_entries[entry_idx].get("occasion", "<not indexed>") if entry_idx is not None else "<no index entry>"
    archive_present = object_exists(s3, bucket, r2_key)
    print(f"  archive present in R2: {archive_present}")
    print(f"  index entry found:     {'yes (#%d)' % entry_idx if entry_idx is not None else 'no'}")
    print(f"  current index occasion: {current_remote_occasion}")

    # Old-key cleanup (for folder renames)
    old_key = ""
    old_entry_idx = None
    if args.rename_from:
        old_safe_name = sanitize_folder_name(args.rename_from)
        old_key = f"matches/{old_safe_name}.tar.gz"
        old_archive_present = object_exists(s3, bucket, old_key)
        for i, e in enumerate(remote_entries):
            if e.get("folder") in (args.rename_from, old_safe_name):
                old_entry_idx = i
                break
        print()
        print(f"Rename-from old folder: {args.rename_from}")
        print(f"  old R2 key:           {old_key}")
        print(f"  old archive present:  {old_archive_present}")
        print(f"  old index entry:      {'yes (#%d)' % old_entry_idx if old_entry_idx is not None else 'no'}")
    print()

    # Build new entry from the fixed local infos.json
    # (archive_size is filled in after packaging)

    with tempfile.TemporaryDirectory() as tmp:
        archive_path = Path(tmp) / f"{safe_name}.tar.gz"
        print(f"Packaging {match_dir.name}...")
        archive_size = create_tar_gz(
            match_dir, archive_path, safe_name if renamed else None
        )
        size_mb = archive_size / (1024 * 1024)
        print(f"  archive size: {size_mb:.1f} MB")
        print()

        new_entry = build_index_entry(safe_name, infos, archive_size)

        print("New index entry preview:")
        preview = {k: new_entry[k] for k in ("folder", "date", "occasion", "duration", "winner", "size_bytes")}
        print(f"  {json.dumps(preview, ensure_ascii=False, indent=2)}")
        print()

        if args.dry_run:
            print("[DRY RUN] Skipping upload. Would:")
            step = 1
            print(f"  {step}. PUT s3://{bucket}/{r2_key}  (overwriting existing archive)")
            step += 1
            if old_key:
                print(f"  {step}. DELETE s3://{bucket}/{old_key}  (old renamed-from archive)")
                step += 1
            print(f"  {step}. PUT s3://{bucket}/index.json  (add/update entry, drop old entry if any)")
            return 0

        # Real upload
        print(f"Uploading archive to {r2_key} (overwrite)...")
        upload_file(s3, bucket, r2_key, archive_path)
        print("  archive uploaded")
        print()

    # Delete old R2 archive if we're processing a rename
    if old_key and old_key != r2_key:
        print(f"Deleting old archive {old_key}...")
        try:
            s3.delete_object(Bucket=bucket, Key=old_key)
            print("  old archive deleted")
        except Exception as e:
            print(f"  warning: delete failed (continuing anyway): {e}")
        print()

    # Replace / add index entry, and drop the old one if renamed
    if entry_idx is not None:
        remote_entries[entry_idx] = new_entry
    else:
        remote_entries.append(new_entry)

    if old_entry_idx is not None and old_entry_idx != entry_idx:
        # Remove the old entry. Indices may have shifted if we added a new
        # one above, but since we used append(), old_entry_idx is still valid.
        remote_entries.pop(old_entry_idx)

    print("Uploading patched index.json...")
    upload_index(s3, bucket, remote_entries)
    print("  index updated")
    print()
    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
