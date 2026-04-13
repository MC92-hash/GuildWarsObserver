#!/usr/bin/env python3
"""
Upload new GvG match recordings to Cloudflare R2.

Scans a local match directory for folders containing infos.json,
compares against the remote index.json, and uploads only new matches.
The index.json is updated incrementally (existing entries preserved).

Usage:
    python upload_to_r2.py                  # upload new matches
    python upload_to_r2.py --dry-run        # show what would be uploaded
    python upload_to_r2.py --list-remote    # list matches in the R2 bucket

Configuration via scripts/r2_config.env or environment variables:
    R2_ENDPOINT, R2_BUCKET, R2_ACCESS_KEY, R2_SECRET_KEY, MATCH_SOURCE_DIR
"""

import argparse
import io
import json
import os
import shutil
import sys
import tarfile
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

try:
    import boto3
    from botocore.exceptions import ClientError
except ImportError:
    print("Error: boto3 is required. Install it with: pip install boto3")
    sys.exit(1)

try:
    from validate_match import validate_match
    HAS_VALIDATOR = True
except ImportError:
    HAS_VALIDATOR = False


def load_config(env_path: Path | None) -> dict:
    """Load configuration from .env file and/or environment variables."""
    config = {}

    # Try loading from .env file
    if env_path and env_path.exists():
        with open(env_path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    key, _, value = line.partition("=")
                    config[key.strip()] = value.strip()

    # Environment variables override .env file
    for key in ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY", "R2_SECRET_KEY",
                "MATCH_SOURCE_DIR", "POST_UPLOAD_ARCHIVE_DIR",
                "RECORDING_SOURCE_DIR", "ORCHESTRATOR_DB"):
        env_val = os.environ.get(key)
        if env_val:
            config[key] = env_val

    return config


def create_s3_client(config: dict):
    """Create a boto3 S3 client pointed at the R2 endpoint."""
    return boto3.client(
        "s3",
        endpoint_url=config["R2_ENDPOINT"],
        aws_access_key_id=config["R2_ACCESS_KEY"],
        aws_secret_access_key=config["R2_SECRET_KEY"],
        region_name="auto",
    )


def fetch_remote_index(s3, bucket: str) -> list[dict]:
    """Fetch and parse index.json from R2. Returns empty list if not found."""
    try:
        resp = s3.get_object(Bucket=bucket, Key="index.json")
        data = json.loads(resp["Body"].read().decode("utf-8"))
        return data.get("matches", [])
    except ClientError as e:
        if e.response["Error"]["Code"] == "NoSuchKey":
            print("No existing index.json found in bucket, starting fresh.")
            return []
        raise


def extract_archives(source_dir: Path) -> int:
    """Auto-extract .tar, .tar.gz, and .tar.gz.zip archives in source_dir.

    Extracted folders are placed next to the archive. Processed archives are
    moved to a 'processed/' subdirectory so they aren't re-extracted.
    Returns the number of archives extracted.
    """
    import zipfile

    processed_dir = source_dir / "processed"
    count = 0

    for entry in sorted(source_dir.iterdir()):
        if not entry.is_file():
            continue

        name = entry.name.lower()

        # Handle .tar.gz.zip (double-compressed archives)
        if name.endswith(".tar.gz.zip"):
            print(f"  Extracting (zip->tar.gz->folder): {entry.name}")
            try:
                with zipfile.ZipFile(entry, "r") as zf:
                    tar_gz_names = [n for n in zf.namelist() if n.endswith(".tar.gz") or n.endswith(".tar")]
                    if not tar_gz_names:
                        print(f"    Warning: no .tar.gz found inside zip, skipping")
                        continue
                    with tempfile.TemporaryDirectory() as tmp:
                        zf.extractall(tmp)
                        for tgz_name in tar_gz_names:
                            tgz_path = Path(tmp) / tgz_name
                            with tarfile.open(tgz_path, "r:*") as tar:
                                tar.extractall(source_dir, filter="data")
                processed_dir.mkdir(exist_ok=True)
                shutil.move(str(entry), str(processed_dir / entry.name))
                count += 1
            except Exception as e:
                print(f"    Error extracting {entry.name}: {e}")
            continue

        # Handle .tar.gz
        if name.endswith(".tar.gz") or name.endswith(".tgz"):
            print(f"  Extracting (tar.gz->folder): {entry.name}")
            try:
                with tarfile.open(entry, "r:gz") as tar:
                    tar.extractall(source_dir, filter="data")
                processed_dir.mkdir(exist_ok=True)
                shutil.move(str(entry), str(processed_dir / entry.name))
                count += 1
            except Exception as e:
                print(f"    Error extracting {entry.name}: {e}")
            continue

        # Handle plain .tar
        if name.endswith(".tar"):
            print(f"  Extracting (tar->folder): {entry.name}")
            try:
                with tarfile.open(entry, "r:") as tar:
                    tar.extractall(source_dir, filter="data")
                processed_dir.mkdir(exist_ok=True)
                shutil.move(str(entry), str(processed_dir / entry.name))
                count += 1
            except Exception as e:
                print(f"    Error extracting {entry.name}: {e}")
            continue

    return count


def collect_recordings(recording_dir: Path, staging_dir: Path) -> int:
    """Move completed recordings from GWToolbox output to the upload staging directory.

    Only moves match folders that:
    - Contain infos.json (recording finished writing metadata)
    - Have no files modified in the last 5 minutes (not still being recorded)
    - Don't already exist in the staging directory
    """
    STALENESS_SECONDS = 300  # 5 minutes
    now = time.time()
    count = 0

    for entry in sorted(recording_dir.iterdir()):
        if not entry.is_dir() or entry.name in ("processed", "rejected"):
            continue
        if not (entry / "infos.json").exists():
            continue

        # Check if any file was modified recently (recording may be in progress)
        try:
            newest_mtime = max(
                f.stat().st_mtime for f in entry.rglob("*") if f.is_file()
            )
        except ValueError:
            continue  # empty directory
        if now - newest_mtime < STALENESS_SECONDS:
            continue

        dest = staging_dir / entry.name
        if dest.exists():
            continue  # already staged

        try:
            shutil.move(str(entry), str(dest))
            print(f"  Collected: {entry.name}")
            count += 1
        except Exception as e:
            print(f"  Warning: failed to collect {entry.name}: {e}")

    return count


def request_rerecord(infos: dict, db_path: str) -> bool:
    """Reset a corrupt recording in the orchestrator DB so it gets re-recorded.

    Looks up the recording by date, map_id, and team tags, then sets its
    status to 'failed'. The orchestrator's insert_recording() reclaims
    rows with status 'failed', so the match will be re-recorded on the
    next session if it's still observable.
    """
    import sqlite3

    year = infos.get("year", 0)
    month = infos.get("month", 0)
    day = infos.get("day", 0)
    if not year:
        return False
    date_str = f"{year:04d}-{month:02d}-{day:02d}"
    map_id = infos.get("map_id", 0)

    # Extract team tags from guilds dict
    guilds = infos.get("guilds", {})
    tags = []
    for guild_obj in guilds.values():
        if isinstance(guild_obj, dict) and guild_obj.get("tag"):
            tags.append(guild_obj["tag"])
    if len(tags) < 2:
        return False
    tag_a, tag_b = tags[0], tags[1]

    try:
        conn = sqlite3.connect(db_path, timeout=5)
        conn.row_factory = sqlite3.Row
        # Look up completed recording matching this match
        row = conn.execute(
            """SELECT fingerprint FROM recordings
               WHERE date = ? AND map_id = ? AND status = 'completed'
                 AND ((team1_tag = ? AND team2_tag = ?) OR (team1_tag = ? AND team2_tag = ?))
               ORDER BY id DESC LIMIT 1""",
            (date_str, map_id, tag_a, tag_b, tag_b, tag_a),
        ).fetchone()
        if not row:
            conn.close()
            return False
        fp = row["fingerprint"]
        conn.execute(
            "UPDATE recordings SET status = 'failed', finished_at = NULL WHERE fingerprint = ? AND date = ?",
            (fp, date_str),
        )
        conn.commit()
        conn.close()
        return True
    except Exception as e:
        print(f"  Warning: failed to reset orchestrator recording: {e}")
        return False


def scan_local_matches(source_dir: Path) -> list[Path]:
    """Find all match directories containing infos.json."""
    matches = []
    if not source_dir.is_dir():
        print(f"Error: source directory does not exist: {source_dir}")
        return matches

    for entry in sorted(source_dir.iterdir()):
        if entry.is_dir() and entry.name != "processed" and (entry / "infos.json").exists():
            matches.append(entry)
    return matches


def sanitize_json(raw: str) -> str:
    """Clean up infos.json quirks (standalone comma lines) to match C++ SanitizeJson."""
    lines = raw.splitlines()
    cleaned = []
    for line in lines:
        stripped = line.strip()
        if stripped == ",":
            # Standalone comma: append to previous line if it doesn't already end with comma
            if cleaned and not cleaned[-1].rstrip().endswith(","):
                cleaned[-1] = cleaned[-1].rstrip() + ","
            # Otherwise skip duplicate comma
        else:
            cleaned.append(line)
    return "\n".join(cleaned)


def read_infos_json(match_dir: Path) -> dict | None:
    """Read and parse a match's infos.json, returning the raw JSON dict."""
    infos_path = match_dir / "infos.json"
    try:
        raw = infos_path.read_text(encoding="utf-8")
        sanitized = sanitize_json(raw)
        return json.loads(sanitized)
    except (json.JSONDecodeError, OSError) as e:
        print(f"  Warning: failed to parse {infos_path}: {e}")
        return None


def build_index_entry(folder_name: str, infos: dict, archive_size: int) -> dict:
    """Build an index.json entry from a match's infos.json data."""
    year = infos.get("year", 0)
    month = infos.get("month", 0)
    day = infos.get("day", 0)
    date_str = f"{year:04d}-{month:02d}-{day:02d}" if year else ""

    entry = {
        "folder": folder_name,
        "map_id": infos.get("map_id", 0),
        "date": date_str,
        "occasion": infos.get("occasion", ""),
        "flux": infos.get("flux", ""),
        "duration": infos.get("match_duration", ""),
        "winner": infos.get("winner_party_id", 0),
        "size_bytes": archive_size,
        "guilds": {},
    }

    # Extract guild name/tag from infos.json guilds object
    guilds_raw = infos.get("guilds", {})
    if isinstance(guilds_raw, dict):
        for guild_key, guild_obj in guilds_raw.items():
            if isinstance(guild_obj, dict):
                entry["guilds"][guild_key] = {
                    "name": guild_obj.get("name", ""),
                    "tag": guild_obj.get("tag", ""),
                }

    # Team-level stats
    entry["team_kills"] = infos.get("team_kills", {})
    entry["team_damage"] = infos.get("team_damage", {})

    # Party/player data for cloud-only preview
    parties_raw = infos.get("parties", {})
    if isinstance(parties_raw, dict):
        parties_out = {}
        for party_id, party_obj in parties_raw.items():
            if not isinstance(party_obj, dict):
                continue
            players_out = []
            for player in party_obj.get("PLAYER", []):
                if not isinstance(player, dict):
                    continue
                players_out.append({
                    "encoded_name": player.get("encoded_name", ""),
                    "primary": player.get("primary", 0),
                    "secondary": player.get("secondary", 0),
                    "player_number": player.get("player_number", 0),
                    "used_skills": player.get("used_skills", []),
                    "skill_template_code": player.get("skill_template_code", ""),
                    "kills": player.get("kills", 0),
                    "deaths": player.get("deaths", 0),
                    "total_damage": player.get("total_damage", 0),
                })
            parties_out[party_id] = {"PLAYER": players_out}
        entry["parties"] = parties_out

    return entry


def match_fingerprint(infos: dict) -> str:
    """Build a content-based fingerprint from match metadata for dedup.

    Uses date + map_id + sorted team tags (from the two match parties only)
    so that the same match is identified regardless of folder naming.
    """
    year = infos.get("year", 0)
    month = infos.get("month", 0)
    day = infos.get("day", 0)
    map_id = infos.get("map_id", 0)
    # Only use guilds that correspond to actual match parties (typically "1" and "2")
    party_ids = set(infos.get("parties", {}).keys())
    guilds = infos.get("guilds", {})
    tags = sorted(
        g.get("tag", "") for gid, g in guilds.items()
        if isinstance(g, dict) and g.get("tag") and gid in party_ids
    )
    return f"{year:04d}-{month:02d}-{day:02d}_{map_id}_{'_vs_'.join(tags)}"


def index_entry_fingerprint(entry: dict) -> str:
    """Build a content fingerprint from an index.json entry for dedup."""
    party_ids = set(entry.get("parties", {}).keys())
    guilds = entry.get("guilds", {})
    tags = sorted(
        g.get("tag", "") for gid, g in guilds.items()
        if isinstance(g, dict) and g.get("tag") and gid in party_ids
    )
    return f"{entry.get('date', '')}_{entry.get('map_id', 0)}_{'_vs_'.join(tags)}"


def sanitize_folder_name(name: str) -> str:
    """Replace non-ASCII characters in folder name with ASCII-safe substitutes.

    R2 public URLs can't serve files with non-ASCII characters in the key.
    Guild tags with Unicode chars (e.g. [戦戦戦戦]) get replaced, but the
    real guild data is preserved in the index entry from infos.json.
    """
    if all(ord(c) < 128 for c in name):
        return name  # already ASCII-safe

    import re
    # Replace content inside [...] brackets if it contains non-ASCII
    def replace_bracket(m):
        content = m.group(1)
        if any(ord(c) >= 128 for c in content):
            # Use a hash-based short identifier to keep it unique
            import hashlib
            short = hashlib.md5(content.encode("utf-8")).hexdigest()[:6]
            return f"[{short}]"
        return m.group(0)

    sanitized = re.sub(r"\[([^\]]+)\]", replace_bracket, name)

    # Replace any remaining non-ASCII characters
    sanitized = "".join(c if ord(c) < 128 else "_" for c in sanitized)
    return sanitized


def create_tar_gz(match_dir: Path, output_path: Path, archive_folder_name: str | None = None) -> int:
    """Package a match directory as a .tar.gz archive. Returns the archive size in bytes.

    If archive_folder_name is provided, the files inside the tar will use that
    name instead of the actual directory name (used for URL-safe renaming).
    """
    folder_name = archive_folder_name or match_dir.name
    with tarfile.open(output_path, "w:gz") as tar:
        for item in sorted(match_dir.rglob("*")):
            if item.is_file():
                arcname = f"{folder_name}/{item.relative_to(match_dir)}"
                tar.add(str(item), arcname=arcname)
    return output_path.stat().st_size


def object_exists(s3, bucket: str, key: str) -> bool:
    """Check if an object already exists in R2."""
    try:
        s3.head_object(Bucket=bucket, Key=key)
        return True
    except ClientError as e:
        if e.response["Error"]["Code"] == "404":
            return False
        raise


def upload_file(s3, bucket: str, key: str, file_path: Path):
    """Upload a file to R2."""
    s3.upload_file(str(file_path), bucket, key)


def upload_index(s3, bucket: str, entries: list[dict]):
    """Upload the updated index.json to R2."""
    index_data = {"version": 1, "matches": entries}
    body = json.dumps(index_data, indent=2, ensure_ascii=False).encode("utf-8")
    s3.put_object(
        Bucket=bucket,
        Key="index.json",
        Body=body,
        ContentType="application/json",
    )


def dir_size_bytes(path: Path) -> int:
    """Calculate the total size of all files in a directory."""
    return sum(f.stat().st_size for f in path.rglob("*") if f.is_file())


def get_bucket_stats(s3, bucket: str) -> dict:
    """Compute bucket-wide statistics: total objects and total size."""
    total_objects = 0
    total_size = 0
    paginator = s3.get_paginator("list_objects_v2")
    for page in paginator.paginate(Bucket=bucket):
        for obj in page.get("Contents", []):
            total_objects += 1
            total_size += obj["Size"]
    return {"total_objects": total_objects, "total_size_bytes": total_size}


def write_json_report(report: dict, path: str):
    """Write JSON report to file or stdout."""
    text = json.dumps(report, indent=2, ensure_ascii=False)
    if path == "-":
        print(text)
    else:
        Path(path).write_text(text, encoding="utf-8")


def cmd_upload(args, config: dict) -> dict:
    """Main upload command: scan local, compare remote, upload new matches.

    Returns a report dict summarising the run (used by --json-report).
    """
    t0 = time.monotonic()
    report = {
        "status": "success",
        "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "uploaded": 0,
        "skipped": 0,
        "rejected_corrupt": 0,
        "errors": [],
        "warnings": [],
        "matched_local": 0,
        "already_remote": 0,
        "dry_run": getattr(args, "dry_run", False),
        "bucket_stats": None,
        "duration_seconds": 0,
    }

    s3 = create_s3_client(config)
    bucket = config["R2_BUCKET"]
    source_dir_raw = args.source_dir or config.get("MATCH_SOURCE_DIR", "")
    source_dir = Path(os.path.expandvars(os.path.expanduser(source_dir_raw)))

    if not source_dir or not source_dir.is_dir():
        print(f"Error: invalid source directory: {source_dir}")
        report["status"] = "error"
        report["errors"].append({"match": "", "error": f"Invalid source directory: {source_dir}"})
        return report

    print(f"Source directory: {source_dir}")
    print(f"R2 bucket: {bucket}")
    print()

    # Collect completed recordings from GWToolbox output
    recording_source = config.get("RECORDING_SOURCE_DIR", "")
    if recording_source:
        rec_dir = Path(os.path.expandvars(os.path.expanduser(recording_source)))
        if rec_dir.is_dir():
            print("Collecting new recordings...")
            collected = collect_recordings(rec_dir, source_dir)
            if collected:
                print(f"  Collected {collected} recording(s).\n")
            else:
                print("  No new recordings to collect.\n")

    # Auto-extract any archives (.tar, .tar.gz, .tar.gz.zip)
    print("Checking for archives to extract...")
    extracted = extract_archives(source_dir)
    if extracted:
        print(f"  Extracted {extracted} archive(s).\n")
    else:
        print("  No new archives found.\n")

    # Fetch existing index
    print("Fetching remote index.json...")
    remote_entries = fetch_remote_index(s3, bucket)
    remote_folders = {e["folder"] for e in remote_entries}
    # Build content-based fingerprints from remote entries for robust dedup
    remote_fingerprints: set[str] = set()
    for e in remote_entries:
        remote_fingerprints.add(index_entry_fingerprint(e))
    print(f"  {len(remote_entries)} matches currently in index.")

    # Scan local matches
    local_matches = scan_local_matches(source_dir)
    print(f"  {len(local_matches)} match folders found locally.")
    report["matched_local"] = len(local_matches)

    # Find new matches: check folder name, sanitized name, AND content fingerprint
    new_matches = []
    for m in local_matches:
        if m.name in remote_folders or sanitize_folder_name(m.name) in remote_folders:
            continue
        # Content-based dedup: read infos.json and check fingerprint
        infos = read_infos_json(m)
        if infos:
            fp = match_fingerprint(infos)
            if fp in remote_fingerprints:
                print(f"  Skipping duplicate (content match): {m.name}")
                continue
        new_matches.append(m)
    report["already_remote"] = len(local_matches) - len(new_matches)

    if not new_matches:
        print("\nNo new matches to upload. Everything is up to date.")
        report["duration_seconds"] = round(time.monotonic() - t0, 1)
        return report

    print(f"\n{len(new_matches)} new match(es) to upload:")
    for m in new_matches:
        size_mb = dir_size_bytes(m) / (1024 * 1024)
        print(f"  - {m.name} ({size_mb:.1f} MB)")

    if args.dry_run:
        print("\n[DRY RUN] No files were uploaded.")
        report["duration_seconds"] = round(time.monotonic() - t0, 1)
        return report

    # Upload each new match
    new_entries = []
    with tempfile.TemporaryDirectory() as tmp_dir:
        for i, match_dir in enumerate(new_matches, 1):
            folder_name = match_dir.name
            safe_name = sanitize_folder_name(folder_name)
            renamed = safe_name != folder_name

            print(f"\n[{i}/{len(new_matches)}] Uploading: {folder_name}")
            if renamed:
                print(f"  URL-safe name: {safe_name}")

            # Read metadata
            infos = read_infos_json(match_dir)
            if infos is None:
                print(f"  Skipping (could not read infos.json)")
                report["skipped"] += 1
                report["warnings"].append({"match": folder_name, "warning": "Could not parse infos.json"})
                continue

            # Validate match data quality
            if HAS_VALIDATOR:
                print(f"  Validating position data...")
                validation = validate_match(match_dir)
                if not validation.passed:
                    print(f"  REJECTED: {validation.reason}")
                    report["rejected_corrupt"] += 1
                    report["warnings"].append({
                        "match": folder_name,
                        "warning": f"Corruption detected: {validation.reason}",
                    })
                    rejected_dir = source_dir / "rejected"
                    rejected_dir.mkdir(exist_ok=True)
                    dest = rejected_dir / match_dir.name
                    if dest.exists():
                        shutil.rmtree(dest)
                    try:
                        shutil.move(str(match_dir), str(dest))
                        print(f"  Moved to: {dest}")
                    except Exception as e:
                        print(f"  Warning: failed to move to rejected/: {e}")
                    # Request re-recording via orchestrator DB
                    db_path = config.get("ORCHESTRATOR_DB", "")
                    if db_path and infos:
                        db_file = Path(os.path.expandvars(os.path.expanduser(db_path)))
                        if db_file.exists():
                            if request_rerecord(infos, str(db_file)):
                                print(f"  Queued for re-recording in orchestrator")
                    continue
                print(f"  Validation passed (median p95={validation.median_p95:.1f}, "
                      f"{validation.total_players} players)")

            # Check if archive already exists in R2 (handles stale index)
            r2_key = f"matches/{safe_name}.tar.gz"
            if object_exists(s3, bucket, r2_key):
                print(f"  Already in R2, skipping upload.")
                report["skipped"] += 1
                entry = build_index_entry(safe_name, infos, 0)
                new_entries.append(entry)
                continue

            # Create archive (using safe name for tar entries)
            archive_path = Path(tmp_dir) / f"{safe_name}.tar.gz"
            print(f"  Packaging...")
            archive_size = create_tar_gz(match_dir, archive_path, safe_name if renamed else None)
            size_mb = archive_size / (1024 * 1024)
            print(f"  Archive size: {size_mb:.1f} MB")

            # Upload archive with URL-safe key
            print(f"  Uploading to {r2_key}...")
            try:
                upload_file(s3, bucket, r2_key, archive_path)
            except Exception as e:
                msg = f"Upload failed for {r2_key}: {e}"
                print(f"  ERROR: {msg}")
                report["errors"].append({"match": folder_name, "error": msg})
                report["status"] = "error"
                archive_path.unlink(missing_ok=True)
                continue

            # Build index entry with safe folder name but real guild data from infos.json
            entry = build_index_entry(safe_name, infos, archive_size)
            new_entries.append(entry)
            report["uploaded"] += 1

            # Clean up temp archive
            archive_path.unlink(missing_ok=True)

            # Move uploaded match to archive directory to free source disk space
            archive_dir_raw = config.get("POST_UPLOAD_ARCHIVE_DIR", "")
            if archive_dir_raw:
                archive_dir = Path(os.path.expandvars(os.path.expanduser(archive_dir_raw)))
                try:
                    archive_dir.mkdir(parents=True, exist_ok=True)
                    dest = archive_dir / match_dir.name
                    if dest.exists():
                        print(f"  Archive destination already exists, removing: {dest}")
                        shutil.rmtree(dest)
                    shutil.move(str(match_dir), str(dest))
                    print(f"  Moved to archive: {dest}")
                except Exception as e:
                    print(f"  Warning: failed to move to archive dir: {e}")
                    report["warnings"].append({"match": folder_name, "warning": f"Post-upload move failed: {e}"})
            else:
                print(f"  Done.")

    # Update index (deduplicate by folder name, new entries take precedence)
    if new_entries:
        seen: dict[str, dict] = {}
        for entry in remote_entries + new_entries:
            seen[entry["folder"]] = entry
        all_entries = list(seen.values())
        deduped = len(remote_entries) + len(new_entries) - len(all_entries)
        if deduped:
            print(f"\n  Removed {deduped} duplicate index entry(ies).")
        print(f"\nUpdating index.json ({len(all_entries)} total matches)...")
        try:
            upload_index(s3, bucket, all_entries)
            print("Index updated.")
        except Exception as e:
            msg = f"Failed to update index.json: {e}"
            print(f"ERROR: {msg}")
            report["errors"].append({"match": "", "error": msg})
            report["status"] = "error"

    print(f"\nUpload complete: {report['uploaded']} new match(es) uploaded.")

    # Collect bucket stats if a JSON report was requested
    if getattr(args, "json_report", None):
        try:
            print("Collecting bucket statistics...")
            report["bucket_stats"] = get_bucket_stats(s3, bucket)
        except Exception as e:
            print(f"Warning: could not collect bucket stats: {e}")

    report["duration_seconds"] = round(time.monotonic() - t0, 1)
    return report


def cmd_list_remote(args, config: dict):
    """List all matches currently in the R2 bucket."""
    s3 = create_s3_client(config)
    bucket = config["R2_BUCKET"]

    # Fetch index
    print("Fetching remote index.json...")
    remote_entries = fetch_remote_index(s3, bucket)
    indexed_folders = {e["folder"]: e for e in remote_entries}

    # List actual objects in matches/ prefix
    print("Listing objects in matches/ prefix...\n")
    archives = {}
    paginator = s3.get_paginator("list_objects_v2")
    for page in paginator.paginate(Bucket=bucket, Prefix="matches/"):
        for obj in page.get("Contents", []):
            key = obj["Key"]
            if key.endswith(".tar.gz"):
                folder = key.removeprefix("matches/").removesuffix(".tar.gz")
                archives[folder] = obj["Size"]

    # Merge and display
    all_folders = sorted(set(list(indexed_folders.keys()) + list(archives.keys())))

    if not all_folders:
        print("No matches found in bucket.")
        return

    print(f"{'Folder':<55} {'Date':<12} {'Size':<10} {'In Index':<10} {'Has Archive'}")
    print("-" * 100)

    for folder in all_folders:
        entry = indexed_folders.get(folder)
        has_archive = folder in archives
        date = entry["date"] if entry else "-"
        if has_archive:
            size_mb = f"{archives[folder] / (1024*1024):.1f} MB"
        elif entry and entry.get("size_bytes"):
            size_mb = f"{entry['size_bytes'] / (1024*1024):.1f} MB"
        else:
            size_mb = "-"

        in_index = "Yes" if entry else "NO"
        has_arch_str = "Yes" if has_archive else "NO"
        print(f"{folder:<55} {date:<12} {size_mb:<10} {in_index:<10} {has_arch_str}")

    print(f"\nTotal: {len(all_folders)} match(es), {len(indexed_folders)} indexed, {len(archives)} archived.")

    # Warn about mismatches
    orphaned = set(archives.keys()) - set(indexed_folders.keys())
    dangling = set(indexed_folders.keys()) - set(archives.keys())
    if orphaned:
        print(f"\nWarning: {len(orphaned)} archive(s) not in index.json:")
        for f in sorted(orphaned):
            print(f"  - {f}")
    if dangling:
        print(f"\nWarning: {len(dangling)} index entry(ies) with no archive:")
        for f in sorted(dangling):
            print(f"  - {f}")


def cmd_sync_index(args, config: dict):
    """Remove index entries whose archive no longer exists in the bucket."""
    s3 = create_s3_client(config)
    bucket = config["R2_BUCKET"]

    print("Fetching remote index.json...")
    remote_entries = fetch_remote_index(s3, bucket)
    if not remote_entries:
        print("Index is empty, nothing to sync.")
        return

    print(f"  {len(remote_entries)} entries in index.")

    # List actual archives in bucket
    print("Listing archives in matches/ prefix...")
    archives = set()
    paginator = s3.get_paginator("list_objects_v2")
    for page in paginator.paginate(Bucket=bucket, Prefix="matches/"):
        for obj in page.get("Contents", []):
            key = obj["Key"]
            if key.endswith(".tar.gz"):
                folder = key.removeprefix("matches/").removesuffix(".tar.gz")
                archives.add(folder)

    print(f"  {len(archives)} archives found.")

    # Find dangling entries (in index but no archive)
    dangling = [e for e in remote_entries if e["folder"] not in archives]

    if not dangling:
        print("\nIndex is in sync — no dangling entries.")
        return

    print(f"\n{len(dangling)} index entry(ies) with no archive:")
    for e in dangling:
        print(f"  - {e['folder']}")

    if args.dry_run:
        print("\n[DRY RUN] No changes made.")
        return

    # Remove dangling entries and re-upload index
    dangling_folders = {e["folder"] for e in dangling}
    cleaned = [e for e in remote_entries if e["folder"] not in dangling_folders]
    print(f"\nUpdating index.json ({len(cleaned)} entries, removed {len(dangling)})...")
    upload_index(s3, bucket, cleaned)
    print("Done.")


def cmd_dedup_index(args, config: dict):
    """Remove duplicate entries from index.json, keeping the last occurrence."""
    s3 = create_s3_client(config)
    bucket = config["R2_BUCKET"]

    print("Fetching remote index.json...")
    remote_entries = fetch_remote_index(s3, bucket)
    if not remote_entries:
        print("Index is empty, nothing to deduplicate.")
        return

    seen: dict[str, dict] = {}
    for entry in remote_entries:
        seen[entry["folder"]] = entry
    deduped = list(seen.values())
    removed = len(remote_entries) - len(deduped)

    print(f"  {len(remote_entries)} entries in index, {len(deduped)} unique.")

    if removed == 0:
        print("\nNo duplicates found.")
        return

    print(f"\n{removed} duplicate entry(ies) to remove.")

    if args.dry_run:
        # Show which folders had duplicates
        from collections import Counter
        counts = Counter(e["folder"] for e in remote_entries)
        for folder, count in counts.most_common():
            if count > 1:
                print(f"  - {folder} ({count}x)")
        print("\n[DRY RUN] No changes made.")
        return

    print(f"Updating index.json ({len(deduped)} entries)...")
    upload_index(s3, bucket, deduped)
    print(f"Done. Removed {removed} duplicate(s).")


def cmd_cleanup_duplicates(args, config: dict):
    """Find matches with the same content fingerprint, keep the best, delete the rest."""
    from collections import defaultdict

    s3 = create_s3_client(config)
    bucket = config["R2_BUCKET"]

    print("Fetching remote index.json...")
    remote_entries = fetch_remote_index(s3, bucket)
    if not remote_entries:
        print("Index is empty, nothing to clean up.")
        return

    # Group entries by content fingerprint
    groups: dict[str, list[dict]] = defaultdict(list)
    for entry in remote_entries:
        fp = index_entry_fingerprint(entry)
        groups[fp].append(entry)

    # Find groups with duplicates
    dup_groups = {fp: entries for fp, entries in groups.items() if len(entries) > 1}

    if not dup_groups:
        print(f"  {len(remote_entries)} entries, no content duplicates found.")
        return

    total_dupes = sum(len(entries) - 1 for entries in dup_groups.values())
    print(f"  {len(remote_entries)} entries, {len(dup_groups)} duplicate group(s), "
          f"{total_dupes} archive(s) to remove.\n")

    keep_folders: set[str] = set()
    delete_folders: set[str] = set()

    for fp, entries in sorted(dup_groups.items()):
        # Keep the entry with the largest archive (best recording)
        best = max(entries, key=lambda e: e.get("size_bytes", 0))
        keep_folders.add(best["folder"])
        rest = [e for e in entries if e["folder"] != best["folder"]]
        for e in rest:
            delete_folders.add(e["folder"])

        print(f"  {fp}")
        print(f"    KEEP:   {best['folder']} ({best.get('size_bytes', 0) / 1024 / 1024:.1f} MB)")
        for e in rest:
            print(f"    DELETE: {e['folder']} ({e.get('size_bytes', 0) / 1024 / 1024:.1f} MB)")

    if args.dry_run:
        print(f"\n[DRY RUN] Would delete {len(delete_folders)} archive(s) and update index.")
        return

    # Delete duplicate archives from R2
    deleted = 0
    for folder in sorted(delete_folders):
        r2_key = f"matches/{folder}.tar.gz"
        try:
            s3.delete_object(Bucket=bucket, Key=r2_key)
            print(f"  Deleted: {r2_key}")
            deleted += 1
        except Exception as e:
            print(f"  Warning: failed to delete {r2_key}: {e}")

    # Rebuild index without deleted entries
    cleaned = [e for e in remote_entries if e["folder"] not in delete_folders]
    # Also deduplicate by folder name in case of exact dupes
    seen: dict[str, dict] = {}
    for e in cleaned:
        seen[e["folder"]] = e
    cleaned = list(seen.values())

    print(f"\nUpdating index.json ({len(cleaned)} entries)...")
    upload_index(s3, bucket, cleaned)
    print(f"Done. Deleted {deleted} archive(s), index updated.")


def main():
    parser = argparse.ArgumentParser(
        description="Upload GvG match recordings to Cloudflare R2."
    )
    # Default env file: check private repo first, then local scripts/
    script_dir = Path(__file__).parent
    private_env = script_dir.parent.parent / "gwobserver-private" / "r2_config.env"
    local_env = script_dir / "r2_config.env"
    default_env = private_env if private_env.exists() else local_env

    parser.add_argument(
        "--env-file",
        type=Path,
        default=default_env,
        help="Path to config .env file (default: gwobserver-private/r2_config.env or scripts/r2_config.env)",
    )
    parser.add_argument(
        "--source-dir",
        type=str,
        default=None,
        help="Local match source directory (overrides MATCH_SOURCE_DIR from config)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show what would be uploaded without actually uploading",
    )
    parser.add_argument(
        "--list-remote",
        action="store_true",
        help="List all matches currently in the R2 bucket",
    )
    parser.add_argument(
        "--sync-index",
        action="store_true",
        help="Remove index entries whose archive was deleted from the bucket",
    )
    parser.add_argument(
        "--dedup-index",
        action="store_true",
        help="Remove duplicate entries from index.json",
    )
    parser.add_argument(
        "--cleanup-duplicates",
        action="store_true",
        help="Find and remove duplicate matches (same content, different folder names) from R2",
    )
    parser.add_argument(
        "--json-report",
        type=str,
        default=None,
        metavar="PATH",
        help="Write a JSON run report to PATH (use '-' for stdout)",
    )

    args = parser.parse_args()

    # Load config
    config = load_config(args.env_file)

    required_keys = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY", "R2_SECRET_KEY")
    missing = [k for k in required_keys if k not in config]
    if missing:
        print(f"Error: missing config values: {', '.join(missing)}")
        print(f"Set them in {args.env_file} or as environment variables.")
        sys.exit(1)

    if args.cleanup_duplicates:
        cmd_cleanup_duplicates(args, config)
    elif args.dedup_index:
        cmd_dedup_index(args, config)
    elif args.sync_index:
        cmd_sync_index(args, config)
    elif args.list_remote:
        cmd_list_remote(args, config)
    else:
        report = cmd_upload(args, config)
        if args.json_report:
            write_json_report(report, args.json_report)
        if report["status"] == "error":
            sys.exit(2)


if __name__ == "__main__":
    main()
