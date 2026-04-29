#!/usr/bin/env python3
"""Retroactive cleanup for late-started recordings already uploaded to R2.

Scans a match archive directory, identifies recordings whose first StoC
event is later than `--threshold` seconds (meaning the orchestrator joined
the match partway through), and:

  1. Moves each local folder into <scan-dir>/rejected/.
  2. Deletes the matching R2 archive (matches/<safe_name>.tar.gz).
  3. Patches index.json to drop the removed entries.

Dry-run by default. Pass --execute to actually perform the destructive ops.

Usage:
    python reject_late_starts.py --scan-dir d:\\matcharchive
    python reject_late_starts.py --scan-dir d:\\matcharchive --execute --yes
"""

import argparse
import gzip
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from upload_to_r2 import (  # noqa: E402
    load_config,
    create_s3_client,
    fetch_remote_index,
    sanitize_folder_name,
    upload_index,
    object_exists,
)

TS_RE = re.compile(r"\[(\d+):(\d+)(?:\.(\d+))?\]")


def first_event_seconds(events_path: Path) -> float | None:
    """Return first valid timestamp in seconds, or None if unreadable/empty."""
    try:
        with gzip.open(events_path, "rt", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = TS_RE.match(line)
                if not m:
                    continue
                mins = int(m.group(1))
                secs = int(m.group(2))
                frac = m.group(3) or ""
                ms = 0
                if frac:
                    ms = int(frac)
                    pad = 3 - len(frac)
                    if pad > 0:
                        ms *= 10 ** pad
                return mins * 60 + secs + ms / 1000.0
    except OSError:
        return None
    return None


def find_late_matches(scan_dir: Path, threshold: float) -> list[tuple[Path, float]]:
    candidates: list[tuple[Path, float]] = []
    for folder in sorted(scan_dir.iterdir()):
        if not folder.is_dir():
            continue
        if folder.name == "rejected":
            continue
        events = folder / "StoC" / "agent_events.txt.gz"
        if not events.is_file():
            continue
        ts = first_event_seconds(events)
        if ts is None:
            continue
        if ts > threshold:
            candidates.append((folder, ts))
    candidates.sort(key=lambda x: -x[1])
    return candidates


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scan-dir", type=Path, required=True,
                    help="Directory to scan (e.g. d:\\matcharchive)")
    ap.add_argument("--threshold", type=float, default=60.0,
                    help="Seconds of missing data to treat as 'late' (default 60)")
    ap.add_argument("--execute", action="store_true",
                    help="Perform moves + R2 deletes + index patch (default: dry-run)")
    ap.add_argument("--yes", action="store_true",
                    help="Skip interactive confirmation in --execute mode")
    ap.add_argument("--env", type=Path,
                    default=Path(__file__).resolve().parent / "r2_config.env",
                    help="R2 credentials env file")
    args = ap.parse_args()

    scan_dir: Path = args.scan_dir.resolve()
    if not scan_dir.is_dir():
        print(f"ERROR: scan-dir not found: {scan_dir}")
        return 1
    if "rejected" in scan_dir.parts:
        print(f"ERROR: scan-dir path contains 'rejected' segment; refusing to double-process.")
        return 1

    print(f"Scanning {scan_dir} for recordings with first event > {args.threshold:.0f}s ...")
    candidates = find_late_matches(scan_dir, args.threshold)
    print()
    print(f"Found {len(candidates)} late-started match(es).")
    if not candidates:
        return 0

    for folder, ts in candidates:
        mins = int(ts // 60)
        secs = ts - mins * 60
        print(f"  [{mins:02d}:{secs:05.2f}]  {folder.name}")

    if not args.execute:
        print()
        print("Dry-run. Pass --execute to move folders + delete R2 archives + patch index.json.")
        return 0

    if not args.yes:
        print()
        print("EXECUTE mode: this will move local folders AND delete R2 objects AND patch index.json.")
        print("Type 'yes' to continue.")
        try:
            ans = input("> ").strip().lower()
        except EOFError:
            ans = ""
        if ans != "yes":
            print("Aborted.")
            return 1

    config = load_config(args.env)
    for key in ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY", "R2_SECRET_KEY"):
        if not config.get(key):
            print(f"ERROR: missing config key {key}")
            return 1

    s3 = create_s3_client(config)
    bucket = config["R2_BUCKET"]

    print()
    print("Fetching remote index.json...")
    remote_entries = fetch_remote_index(s3, bucket)
    print(f"  {len(remote_entries)} entries in index")

    rejected_dir = scan_dir / "rejected"
    rejected_dir.mkdir(exist_ok=True)

    folders_to_remove: set[str] = set()
    failures: list[str] = []

    for i, (folder, ts) in enumerate(candidates, 1):
        mins = int(ts // 60)
        secs = ts - mins * 60
        print()
        print(f"[{i}/{len(candidates)}] {folder.name}  (first event {mins:02d}:{secs:05.2f})")

        safe_name = sanitize_folder_name(folder.name)
        r2_key = f"matches/{safe_name}.tar.gz"

        if object_exists(s3, bucket, r2_key):
            print(f"  Deleting {r2_key} ...")
            try:
                s3.delete_object(Bucket=bucket, Key=r2_key)
                print("  R2 archive deleted")
            except Exception as e:
                print(f"  ERROR deleting R2 archive: {e}")
                failures.append(folder.name)
                continue
        else:
            print(f"  R2 archive not present at {r2_key} (already gone) — skipping delete")

        folders_to_remove.add(folder.name)
        folders_to_remove.add(safe_name)

        dest = rejected_dir / folder.name
        try:
            if dest.exists():
                print(f"  Destination already exists: {dest} — leaving local folder in place")
            else:
                shutil.move(str(folder), str(dest))
                print(f"  Moved local folder -> {dest}")
        except Exception as e:
            print(f"  ERROR moving folder: {e}")
            failures.append(folder.name)

    kept = [e for e in remote_entries if e.get("folder") not in folders_to_remove]
    dropped = len(remote_entries) - len(kept)
    print()
    print(f"Patching index.json: dropping {dropped} entries ({len(kept)} remain).")
    try:
        upload_index(s3, bucket, kept)
        print("  index.json uploaded")
    except Exception as e:
        print(f"  ERROR uploading patched index: {e}")
        return 2

    print()
    print("=" * 60)
    print(f"Done. {len(candidates) - len(failures)}/{len(candidates)} candidates processed.")
    if failures:
        print(f"Failures ({len(failures)}):")
        for name in failures:
            print(f"  - {name}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
