#!/usr/bin/env python3
"""One-shot retroactive relabel of 2026-04-18 mAT swiss rounds.

Finds matches under d:\\matcharchive\\2026-04-18_* whose infos.json occasion
is "B AT" and whose folder mtime falls in the mAT swiss window
(2026-04-18 16:00..19:30 UTC), rewrites the occasion to "Swiss-Rounds mAT",
and (optionally) invokes fix_mislabeled_upload.py per match to re-package
and re-upload to R2 (overwriting the existing archive and patching index.json).

Three modes (exactly one required):
    --preview       list candidate folders, no changes
    --write-local   rewrite local infos.json only (no R2 contact)
    --execute       rewrite local infos.json AND upload each to R2 for real
                    (calls fix_mislabeled_upload.py without --dry-run)

Recommended sequence:
    1) python relabel_mat_swiss_2026-04-18.py --preview
    2) python relabel_mat_swiss_2026-04-18.py --write-local
    3) (optional spot-check) python fix_mislabeled_upload.py <one_folder> --dry-run
    4) python relabel_mat_swiss_2026-04-18.py --execute
"""

import argparse
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ARCHIVE_ROOT = Path(r"d:\matcharchive")
MAT_DATE_PREFIX = "2026-04-18_"
MAT_WINDOW_START = datetime(2026, 4, 18, 16, 0, tzinfo=timezone.utc)
MAT_WINDOW_END = datetime(2026, 4, 18, 19, 30, tzinfo=timezone.utc)
OLD_OCCASION = "B AT"
NEW_OCCASION = "Swiss-Rounds mAT"

SCRIPT_DIR = Path(__file__).resolve().parent
FIX_SCRIPT = SCRIPT_DIR / "fix_mislabeled_upload.py"

OCCASION_RE = re.compile(r'("occasion"\s*:\s*)"B AT"')


def find_candidates() -> list[tuple[Path, datetime, str]]:
    """Return (folder, mtime, current_occasion) for each match that either
    still needs relabeling ("B AT") or has already been relabeled locally
    ("Swiss-Rounds mAT") but may still need re-uploading to R2.
    """
    candidates: list[tuple[Path, datetime, str]] = []
    skipped_wrong_occasion = 0
    skipped_out_of_window = 0
    accept = {OLD_OCCASION, NEW_OCCASION}

    for folder in sorted(ARCHIVE_ROOT.glob(f"{MAT_DATE_PREFIX}*")):
        if not folder.is_dir():
            continue
        infos = folder / "infos.json"
        if not infos.is_file():
            continue

        text = infos.read_text(encoding="utf-8")
        m = re.search(r'"occasion"\s*:\s*"([^"]*)"', text)
        current = m.group(1) if m else ""

        if current not in accept:
            skipped_wrong_occasion += 1
            continue

        mtime = datetime.fromtimestamp(folder.stat().st_mtime, tz=timezone.utc)
        if not (MAT_WINDOW_START <= mtime <= MAT_WINDOW_END):
            skipped_out_of_window += 1
            print(f"  SKIP (mtime {mtime:%H:%M} UTC outside window): {folder.name}")
            continue

        candidates.append((folder, mtime, current))

    already_new = sum(1 for _, _, c in candidates if c == NEW_OCCASION)
    still_old = len(candidates) - already_new
    print()
    print(f"Found {len(candidates)} candidates  ({still_old} need local rewrite, {already_new} already relabeled locally).")
    print(f"Skipped {skipped_wrong_occasion} with occasion not in {{{OLD_OCCASION!r}, {NEW_OCCASION!r}}} (A AT / bracket / unrelated).")
    print(f"Skipped {skipped_out_of_window} with mtime outside mAT window.")
    return candidates


def rewrite_occasion(folder: Path) -> bool:
    infos = folder / "infos.json"
    text = infos.read_text(encoding="utf-8")
    new_text, n = OCCASION_RE.subn(rf'\1"{NEW_OCCASION}"', text, count=1)
    if n != 1:
        print(f"  WARN: could not find \"B AT\" in {infos} — skipping")
        return False
    infos.write_text(new_text, encoding="utf-8")
    return True


def run_fix_script(folder: Path, dry_run: bool) -> int:
    cmd = [sys.executable, str(FIX_SCRIPT), str(folder)]
    if dry_run:
        cmd.append("--dry-run")
    print(f"  $ {' '.join(cmd)}")
    result = subprocess.run(cmd)
    return result.returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--preview", action="store_true",
                      help="List candidate folders. No writes, no R2 contact.")
    mode.add_argument("--write-local", action="store_true",
                      help="Rewrite each candidate's local infos.json occasion. No R2 contact.")
    mode.add_argument("--execute", action="store_true",
                      help="Rewrite local infos.json AND upload to R2 for real (no --dry-run on fix script).")
    ap.add_argument("--yes", action="store_true",
                    help="Skip the interactive confirmation prompt.")
    args = ap.parse_args()

    if not FIX_SCRIPT.is_file():
        print(f"ERROR: fix_mislabeled_upload.py not found at {FIX_SCRIPT}")
        return 1
    if not ARCHIVE_ROOT.is_dir():
        print(f"ERROR: archive root not found: {ARCHIVE_ROOT}")
        return 1

    print(f"Scanning {ARCHIVE_ROOT}\\{MAT_DATE_PREFIX}* ...")
    candidates = find_candidates()
    if not candidates:
        print("Nothing to do.")
        return 0

    if args.preview:
        print()
        print("--preview mode: no changes. Candidate folders:")
        for folder, mtime, current in candidates:
            tag = "(needs rewrite)" if current == OLD_OCCASION else "(already relabeled)"
            print(f"  [{mtime:%H:%M} UTC] {folder.name}  {tag}")
        return 0

    mode_label = "EXECUTE (write local + real R2 upload)" if args.execute else "WRITE-LOCAL (no R2 contact)"
    print()
    print(f"Mode: {mode_label}")
    if not args.yes:
        print("Proceed? Type 'yes' to continue.")
        try:
            answer = input("> ").strip().lower()
        except EOFError:
            answer = ""
        if answer != "yes":
            print("Aborted.")
            return 1

    failures: list[str] = []
    for i, (folder, mtime, current) in enumerate(candidates, 1):
        print()
        print(f"[{i}/{len(candidates)}] {folder.name}  (mtime {mtime:%H:%M} UTC, local occasion {current!r})")
        if current == OLD_OCCASION:
            if not rewrite_occasion(folder):
                failures.append(folder.name)
                continue
        if args.execute:
            rc = run_fix_script(folder, dry_run=False)
            if rc != 0:
                failures.append(folder.name)

    print()
    print("=" * 60)
    print(f"Done. {len(candidates) - len(failures)}/{len(candidates)} succeeded.")
    if failures:
        print(f"Failures ({len(failures)}):")
        for name in failures:
            print(f"  - {name}")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
