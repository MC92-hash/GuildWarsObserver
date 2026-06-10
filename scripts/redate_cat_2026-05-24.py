#!/usr/bin/env python3
"""One-shot redate of the 7 C-AT matches recorded manually on 2026-05-24.

The user manually re-recorded these AT matches because the orchestrator
flagged them too_old. They're real C-AT matches from 2026-05-23 but
GWToolbox tagged them with today's date. This script:

  - For each of the 7 matches in D:\\MatchArchive\\2026-05-24_...:
    * Reads infos.json, confirms occasion == "C AT".
    * Detects whether a 2026-05-23 entry with the same map_id + team
      tags already exists on R2.
      - If yes: assume the orchestrator already captured this match
        yesterday, and just delete the 2026-05-24 duplicate (R2 archive,
        index entry, local archive folder).
      - If no: rewrite infos.json (day 24 -> 23), rename folder, and
        re-upload via fix_mislabeled_upload.py with --rename-from.

Usage:
    python redate_cat_2026-05-24.py --dry-run
    python redate_cat_2026-05-24.py --execute
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from upload_to_r2 import (  # noqa: E402
    create_s3_client,
    fetch_remote_index,
    index_entry_fingerprint,
    load_config,
    match_fingerprint,
    read_infos_json,
    sanitize_folder_name,
    upload_index,
)

SCRIPT_DIR = Path(__file__).resolve().parent
FIX_SCRIPT = SCRIPT_DIR / "fix_mislabeled_upload.py"
ARCHIVE = Path(r"D:\MatchArchive")
PRIVATE_ENV = Path(r"C:\Users\gwobserver\Documents\gwobserver-private\r2_config.env")

# Substrings used to identify the 7 C-AT folders on disk (mojibake-safe).
C_AT_KEYWORDS = [
    "Insel_der_Meditation_05.24",
    "Insel_der_Meditation_05.50",
    "rmer_05.17",
    "Insel_des_Kriegers_08.29",
    "Insel_des_Kriegers_11.51",
    "Insel_des_Kriegers_12.57",
    "Kaiserinsel_13.20",
]


def find_folder(keyword: str) -> Path | None:
    matches = [
        d for d in ARCHIVE.iterdir()
        if d.is_dir() and keyword in d.name and d.name.startswith("2026-05-24_")
    ]
    return matches[0] if len(matches) == 1 else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--dry-run", action="store_true")
    group.add_argument("--execute", action="store_true")
    args = parser.parse_args()

    cfg = load_config(PRIVATE_ENV)
    s3 = create_s3_client(cfg)
    bucket = cfg["R2_BUCKET"]

    print(f"Fetching remote index.json from {bucket}...")
    remote_entries = fetch_remote_index(s3, bucket)
    # Build fingerprint map for existing 2026-05-23 entries
    fp_to_entry: dict[str, dict] = {}
    for e in remote_entries:
        if e.get("date") == "2026-05-23":
            fp_to_entry[index_entry_fingerprint(e)] = e
    print(f"  {len(fp_to_entry)} 2026-05-23 entries currently in index\n")

    rekey_plan: list[tuple[Path, str]] = []   # (folder, new_folder_name)
    delete_plan: list[tuple[Path, str]] = []  # (folder, reason)

    for kw in C_AT_KEYWORDS:
        folder = find_folder(kw)
        if folder is None:
            print(f"NOT FOUND: keyword '{kw}'")
            continue

        infos = read_infos_json(folder)
        if not infos:
            print(f"BAD INFOS: {folder.name}")
            continue
        if infos.get("occasion") != "C AT":
            print(f"NOT C AT (occasion={infos.get('occasion')!r}): {folder.name}")
            continue

        # Build fingerprint as if the match were dated 2026-05-23
        fake_infos_23 = dict(infos)
        fake_infos_23["day"] = 23
        fp_23 = match_fingerprint(fake_infos_23)
        existing = fp_to_entry.get(fp_23)

        new_folder_name = re.sub(r"^2026-05-24_", "2026-05-23_", folder.name)
        if existing:
            delete_plan.append((folder, f"already exists on R2 as {existing['folder']} (16 players)"))
        else:
            rekey_plan.append((folder, new_folder_name))

    print("=" * 70)
    print(f"Re-key plan ({len(rekey_plan)} matches):")
    for f, new_name in rekey_plan:
        print(f"  {f.name}")
        print(f"    -> {new_name}")
    print()
    print(f"Delete plan ({len(delete_plan)} matches):")
    for f, reason in delete_plan:
        print(f"  {f.name}")
        print(f"    reason: {reason}")
    print("=" * 70)

    if args.dry_run:
        print("\n[DRY RUN] No changes made.")
        return 0

    # === EXECUTE ===
    for folder, new_name in rekey_plan:
        print(f"\n[REKEY] {folder.name}")
        infos_path = folder / "infos.json"
        raw = infos_path.read_text(encoding="utf-8")
        # Replace just the day field
        new_raw = re.sub(r'("day"\s*:\s*)24\b', r"\g<1>23", raw)
        if new_raw == raw:
            print(f"  ERROR: could not find '\"day\": 24' in infos.json, skipping")
            continue
        infos_path.write_text(new_raw, encoding="utf-8")
        print(f"  Updated day field in infos.json")

        # Rename folder
        new_folder = ARCHIVE / new_name
        if new_folder.exists():
            print(f"  ERROR: {new_folder} already exists, skipping")
            continue
        old_name = folder.name
        folder.rename(new_folder)
        print(f"  Renamed: {old_name} -> {new_name}")

        # Run fix_mislabeled_upload.py with --rename-from
        cmd = [
            sys.executable, str(FIX_SCRIPT),
            str(new_folder),
            "--rename-from", old_name,
            "--env", str(PRIVATE_ENV),
        ]
        print(f"  Running fix_mislabeled_upload.py...")
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  FAILED (exit {result.returncode}):")
            print(result.stdout)
            print(result.stderr)
            continue
        # Show last few lines of output
        for line in result.stdout.splitlines()[-5:]:
            print(f"  | {line}")

    for folder, reason in delete_plan:
        print(f"\n[DELETE DUPLICATE] {folder.name}")
        print(f"  reason: {reason}")
        safe_name = sanitize_folder_name(folder.name)
        r2_key = f"matches/{safe_name}.tar.gz"
        try:
            s3.delete_object(Bucket=bucket, Key=r2_key)
            print(f"  Deleted R2 object: {r2_key}")
        except Exception as e:
            print(f"  Warning: R2 delete failed: {e}")

        # Move local archive folder to replaced/
        replaced_dir = ARCHIVE / "replaced"
        replaced_dir.mkdir(exist_ok=True)
        dest = replaced_dir / folder.name
        if dest.exists():
            print(f"  {dest} already exists, skipping local move")
        else:
            shutil.move(str(folder), str(dest))
            print(f"  Moved local folder to: {dest}")

    # After deletes, rebuild index without the dropped entries
    if delete_plan:
        deleted_folders = {sanitize_folder_name(f.name) for f, _ in delete_plan}
        deleted_folders |= {f.name for f, _ in delete_plan}
        print(f"\nRebuilding index.json (dropping {len(deleted_folders)} entries)...")
        remote_entries = fetch_remote_index(s3, bucket)
        kept = [e for e in remote_entries if e.get("folder") not in deleted_folders]
        removed = len(remote_entries) - len(kept)
        print(f"  removed {removed} entries from index ({len(kept)} remain)")
        upload_index(s3, bucket, kept)
        print(f"  index uploaded")

    print("\nDone.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
