#!/usr/bin/env python3
"""Purge private scrim recordings that leaked to the public R2 bucket.

Detects entries in ``index.json`` whose ``occasion`` matches the scrim
pattern ("General Scrimmage", "Scrimmage", etc.), deletes the
corresponding ``matches/<folder>.tar.gz`` objects from R2, and writes a
filtered index.json back to the bucket.

Defaults to ``--dry-run``. Pass ``--execute`` to actually delete things.
A backup of the live index.json is always saved to disk before any
destructive operation, so you can roll back if needed.

Usage:
    python purge_leaked_scrims.py                # dry-run
    python purge_leaked_scrims.py --execute      # actually purge

Requires r2_config.env in the same directory (same file the AT upload
script uses).
"""

import argparse
import io
import json
import sys
import time
from pathlib import Path

# Reuse the AT upload script's helpers — same R2 client, same index format.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from upload_to_r2 import (  # noqa: E402
    create_s3_client,
    fetch_remote_index,
    is_scrim_recording,
    load_config,
)


def is_leaked_entry(entry: dict) -> bool:
    """An index.json entry is a leaked scrim if its occasion smells scrimmy.

    Mirrors :func:`upload_to_r2.is_scrim_recording`, but applied to the
    flattened entry instead of an infos.json dict — same key.
    """
    return is_scrim_recording(entry)  # keys match: occasion is top-level on both


def fmt_size(bytes_: int) -> str:
    if bytes_ < 1024:
        return f"{bytes_} B"
    if bytes_ < 1024 * 1024:
        return f"{bytes_/1024:.1f} KB"
    return f"{bytes_/1024/1024:.1f} MB"


def main() -> int:
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--execute", action="store_true",
        help="Actually delete from R2 and rewrite index.json. Default is dry-run.",
    )
    parser.add_argument(
        "--yes", action="store_true",
        help="Skip the interactive confirmation prompt (use with --execute).",
    )
    parser.add_argument(
        "--config", type=Path,
        default=Path(__file__).resolve().parent / "r2_config.env",
        help="Path to r2_config.env (defaults to alongside this script).",
    )
    parser.add_argument(
        "--backup-dir", type=Path,
        default=Path(__file__).resolve().parent / "purge_backups",
        help="Where to save a backup of the live index.json.",
    )
    parser.add_argument(
        "--folder", type=str, default=None,
        help=(
            "Optional substring filter on the entry's folder name (case "
            "insensitive). When set, restricts the purge to entries whose "
            "folder contains this substring AND matches the scrim filter. "
            "Use for surgical removal of a single match."
        ),
    )
    args = parser.parse_args()

    cfg = load_config(args.config)
    s3 = create_s3_client(cfg)
    bucket = cfg["R2_BUCKET"]

    print(f"Bucket: {bucket}")
    print(f"Endpoint: {cfg['R2_ENDPOINT']}")
    print()

    matches = fetch_remote_index(s3, bucket)
    print(f"Loaded index.json with {len(matches)} entries.")

    folder_filter = (args.folder or "").lower().strip()

    def _is_target(m: dict) -> bool:
        if not is_leaked_entry(m):
            return False
        if folder_filter:
            return folder_filter in (m.get("folder") or "").lower()
        return True

    leaked = [m for m in matches if _is_target(m)]
    kept = [m for m in matches if not _is_target(m)]
    print(f"  {len(leaked)} entries selected for purge"
          + (f" (folder filter={args.folder!r})" if folder_filter else ""))
    print(f"  {len(kept)} entries to keep")
    print()

    if not leaked:
        print("Nothing to purge. Index is clean.")
        return 0

    total_bytes = sum(int(m.get("size_bytes") or 0) for m in leaked)
    print(f"Leaked entries (will be removed, {fmt_size(total_bytes)} on R2):")
    for i, m in enumerate(leaked):
        guilds = m.get("guilds") or {}
        g1 = guilds.get("1", {}); g2 = guilds.get("2", {})
        date = m.get("date", "")
        folder = m.get("folder", "")
        occ = m.get("occasion", "")
        size = fmt_size(int(m.get("size_bytes") or 0))
        print(f"  [{i:2}] {date:10} {occ:20} [{g1.get('tag','')}]vs[{g2.get('tag','')}]  {size:>10}  -> matches/{folder}.tar.gz")

    if not args.execute:
        print()
        print("[DRY RUN] Nothing deleted. Re-run with --execute to perform the purge.")
        return 0

    # Always save a backup of the live index before mutating anything.
    args.backup_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    backup_path = args.backup_dir / f"index_pre_purge_{stamp}.json"
    backup_path.write_text(json.dumps({"matches": matches}, indent=2), encoding="utf-8")
    print(f"\nBackup saved: {backup_path}")

    if not args.yes:
        print()
        print(f"About to delete {len(leaked)} object(s) from R2 and rewrite index.json.")
        print("This is irreversible (objects in R2 are gone). The backup above lets")
        print("you reconstruct the old index if needed (objects must be re-uploaded).")
        answer = input("Type 'PURGE' to proceed: ").strip()
        if answer != "PURGE":
            print("Aborted.")
            return 1

    # Delete the .tar.gz objects from R2.
    deleted = 0
    missing = 0
    failed = []
    for m in leaked:
        folder = m.get("folder", "")
        if not folder:
            continue
        key = f"matches/{folder}.tar.gz"
        try:
            head = s3.head_object(Bucket=bucket, Key=key)  # noqa: F841 - existence check
            s3.delete_object(Bucket=bucket, Key=key)
            print(f"  deleted: {key}")
            deleted += 1
        except Exception as e:
            err_code = getattr(getattr(e, "response", None), "get", lambda *_: {})("Error", {}).get("Code", "")  # type: ignore
            if "404" in str(err_code) or "NoSuchKey" in str(err_code) or "NotFound" in str(err_code):
                print(f"  missing (already gone): {key}")
                missing += 1
            else:
                print(f"  FAILED: {key} — {e}")
                failed.append((key, str(e)))

    # Rewrite index.json with only the legitimate entries.
    cleaned_body = json.dumps({"matches": kept}, indent=2).encode("utf-8")
    s3.put_object(
        Bucket=bucket,
        Key="index.json",
        Body=cleaned_body,
        ContentType="application/json",
        CacheControl="no-cache",
    )
    print(f"\nWrote new index.json ({len(kept)} entries).")
    print(f"Deletions: {deleted} ok, {missing} already gone, {len(failed)} failed.")
    if failed:
        print("Failed deletions:")
        for k, e in failed:
            print(f"  - {k}: {e}")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
