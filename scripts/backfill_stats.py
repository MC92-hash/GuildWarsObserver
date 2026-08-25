"""Backfill the combat-stats sidecar over matches already uploaded.

`upload_to_r2.py` publishes `stats/<YYYY-MM>.json` for every match it uploads,
but it only started doing so recently -- so the whole existing archive has an
index entry and no stats row, and every combat metric on the website reads "no
data" for it. This fills that in.

Two sources, in order, because they cost wildly different amounts:

  1. **Local recordings.** `MATCH_SOURCE_DIR` and `POST_UPLOAD_ARCHIVE_DIR`
     already hold `infos.json` for most of the archive -- measured at 2,345 of
     2,635 indexed matches, 89%. A file walk, no downloads.

  2. **`--from-r2`**, for whatever the local pass missed. Pulls each match's
     `.tar.gz` and extracts `infos.json`. Measured at 0.79 GB for the 290
     stragglers, so minutes rather than hours.

     Do not bother trying to stream the gzip and stop early: `create_tar_gz`
     adds members in `sorted()` path order, and in ASCII `Agents` < `StoC` <
     `infos.json`, so the file we want is the *last* member of every archive.
     The whole object comes down regardless.

Safe to re-run. Entries are keyed by folder name, so a second pass over the same
month rewrites the same rows; and each shard is read-modify-written, the same way
`cmd_upload` patches it, so a backfill and a live upload cannot clobber one
another.

    python backfill_stats.py                      # dry run, local sources
    python backfill_stats.py --apply
    python backfill_stats.py --apply --month 2026-08
    python backfill_stats.py --apply --from-r2     # finish the stragglers
"""

from __future__ import annotations

import argparse
import io
import json
import sys
import tarfile
from collections import defaultdict
from pathlib import Path

# Reuse the real producers rather than reimplementing them: a backfill that
# writes a subtly different shape from the live uploader is worse than no
# backfill, because the difference would only surface as odd numbers later.
from upload_to_r2 import (  # noqa: E402  (same directory, run as a script)
    build_stats_entry,
    create_s3_client,
    fetch_remote_index,
    fetch_remote_stats,
    load_config,
    month_of_entry,
    sanitize_folder_name,
    upload_stats,
    STATS_SCHEMA,
)

LOCAL_DIR_KEYS = ("MATCH_SOURCE_DIR", "POST_UPLOAD_ARCHIVE_DIR")


def default_env_file() -> Path:
    """The same resolution upload_to_r2 uses: private repo first, then local."""
    script_dir = Path(__file__).parent
    private = script_dir.parent.parent / "gwobserver-private" / "r2_config.env"
    return private if private.exists() else script_dir / "r2_config.env"


def expand(raw: str) -> Path:
    """Expand a configured directory the way upload_to_r2 does."""
    import os

    return Path(os.path.expandvars(os.path.expanduser(raw)))


def local_infos(config: dict) -> dict[str, Path]:
    """`{sanitized folder name: path to infos.json}` across the local sources.

    Keyed on the **sanitised** name because that is what `index.json` stores,
    while the directories on disk keep the original -- which can hold non-ASCII
    (a German client writes `Insel_der_Würmer`). Joining on raw names would
    silently skip every recording made on a localised client.
    """
    found: dict[str, Path] = {}
    for key in LOCAL_DIR_KEYS:
        raw = config.get(key, "")
        if not raw:
            continue
        root = expand(raw)
        if not root.is_dir():
            print(f"  {key}: {root} is not a directory, skipping")
            continue
        hits = 0
        for infos in sorted(root.glob("*/infos.json")):
            safe = sanitize_folder_name(infos.parent.name)
            # First source wins. MATCH_SOURCE_DIR is listed first because a
            # folder still sitting there is the copy most recently written.
            if safe not in found:
                found[safe] = infos
                hits += 1
        print(f"  {key}: {root}  ->  {hits} recording(s)")
    return found


def read_infos(path: Path) -> dict | None:
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except Exception as e:
        print(f"    unreadable {path.parent.name}: {type(e).__name__}: {e}")
        return None


def infos_from_archive(s3, bucket: str, folder: str) -> dict | None:
    """Pull one match's `infos.json` out of its archived `.tar.gz`."""
    key = f"matches/{folder}.tar.gz"
    try:
        body = s3.get_object(Bucket=bucket, Key=key)["Body"].read()
    except Exception as e:
        print(f"    {folder}: download failed ({type(e).__name__})")
        return None
    try:
        with tarfile.open(fileobj=io.BytesIO(body), mode="r:gz") as tar:
            member = next(
                (m for m in tar.getmembers()
                 if m.isfile() and Path(m.name).name == "infos.json"),
                None,
            )
            if member is None:
                print(f"    {folder}: archive holds no infos.json")
                return None
            handle = tar.extractfile(member)
            if handle is None:
                return None
            return json.loads(handle.read().decode("utf-8-sig"))
    except Exception as e:
        print(f"    {folder}: unreadable archive ({type(e).__name__}: {e})")
        return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--apply", action="store_true",
                        help="actually write the shards (default is a dry run)")
    parser.add_argument("--month", action="append", metavar="YYYY-MM",
                        help="restrict to this month; repeatable")
    parser.add_argument("--limit", type=int, default=0,
                        help="stop after N matches (smoke test)")
    parser.add_argument("--from-r2", action="store_true",
                        help="also fetch archives for matches with no local copy")
    parser.add_argument("--only-missing", action="store_true",
                        help="skip the local pass and do only the R2 stragglers "
                             "(implies --from-r2). Worth having because --limit "
                             "takes local hits first, so it can never reach the "
                             "R2 entries on its own.")
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
        indexed = {f: e for f, e in indexed.items()
                   if month_of_entry(e) in months}
        print(f"  {len(indexed):,} after --month {sorted(months)}")

    print("Scanning local recordings...")
    local = local_infos(config)
    print(f"  {len(local):,} local recording(s) total")

    # Only ever write folders that index.json knows about. Local disks also hold
    # rejects and superseded duplicates; a stats row for a match the website
    # cannot show is dead weight, and it would inflate the Quality panel's
    # denominator with matches that are not in the corpus.
    hits = sorted(set(indexed) & set(local))
    missing = sorted(set(indexed) - set(local))
    print(f"\nindexed and local : {len(hits):,}")
    print(f"indexed, no local : {len(missing):,}"
          + ("" if (args.from_r2 or args.only_missing)
             else "   (use --from-r2 to fetch these)"))

    from_r2 = args.from_r2 or args.only_missing
    work: list[tuple[str, str]] = ([] if args.only_missing
                                   else [(f, "local") for f in hits])
    if from_r2:
        work += [(f, "r2") for f in missing]
    if args.limit:
        work = work[:args.limit]
        print(f"--limit: {len(work)} match(es)")

    shards: dict[str, dict] = defaultdict(dict)
    skipped_no_stats = 0
    skipped_no_month = 0
    unreadable = 0
    downloaded = 0

    for index, (folder, source) in enumerate(work, start=1):
        entry = indexed[folder]
        month = month_of_entry(entry)
        if not month:
            skipped_no_month += 1
            continue

        if source == "local":
            infos = read_infos(local[folder])
        else:
            infos = infos_from_archive(s3, bucket, folder)
            downloaded += 1
        if infos is None:
            unreadable += 1
            continue

        stats = build_stats_entry(
            infos, local[folder].parent if source == "local" else None,
        )
        if not stats:
            # An older capture with no per-player stat block. Expected, not an
            # error -- and it must not be written as a row of zeroes.
            skipped_no_stats += 1
            continue
        shards[month][folder] = stats

        if index % 250 == 0 or index == len(work):
            print(f"  ...{index}/{len(work)}")

    print(f"\nprepared {sum(len(v) for v in shards.values()):,} row(s) "
          f"across {len(shards)} month(s)")
    for month in sorted(shards):
        body = json.dumps({"schema": STATS_SCHEMA, "month": month,
                           "matches": shards[month]},
                          ensure_ascii=False, separators=(",", ":"))
        print(f"  {month}: {len(shards[month]):5} match(es)  "
              f"{len(body) / 1e6:5.2f} MB")
    if skipped_no_stats:
        print(f"  {skipped_no_stats} recording(s) carry no stat block "
              f"(older capture software)")
    if skipped_no_month:
        print(f"  {skipped_no_month} entry(ies) have no usable date")
    if unreadable:
        print(f"  {unreadable} recording(s) unreadable")
    if downloaded:
        print(f"  {downloaded} archive(s) downloaded")

    if not args.apply:
        print("\n[DRY RUN] Nothing was written. Re-run with --apply.")
        return 0

    print("\nMerging into the remote shards...")
    for month in sorted(shards):
        try:
            # Read-modify-write, exactly as cmd_upload does, so a live upload
            # landing mid-backfill is merged rather than lost.
            existing = fetch_remote_stats(s3, bucket, month)
            before = len(existing)
            for folder, row in shards[month].items():
                prior = existing.get(folder)
                if isinstance(prior, dict) and "combat_analytics" in prior:
                    from combat_analytics import merge_preserving_richer
                    row["combat_analytics"] = merge_preserving_richer(
                        prior["combat_analytics"], row.get("combat_analytics", {}),
                    )
                existing[folder] = row
            upload_stats(s3, bucket, month, existing)
            print(f"  {month}: {before} -> {len(existing)} match(es)")
        except Exception as e:
            print(f"  ERROR {month}: {type(e).__name__}: {e}")
            return 1

    print("\nDone.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
