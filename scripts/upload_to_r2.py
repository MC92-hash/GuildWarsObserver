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
import sys
import tarfile
import tempfile
from pathlib import Path

try:
    import boto3
    from botocore.exceptions import ClientError
except ImportError:
    print("Error: boto3 is required. Install it with: pip install boto3")
    sys.exit(1)


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
    for key in ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY", "R2_SECRET_KEY", "MATCH_SOURCE_DIR"):
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


def scan_local_matches(source_dir: Path) -> list[Path]:
    """Find all match directories containing infos.json."""
    matches = []
    if not source_dir.is_dir():
        print(f"Error: source directory does not exist: {source_dir}")
        return matches

    for entry in sorted(source_dir.iterdir()):
        if entry.is_dir() and (entry / "infos.json").exists():
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


def create_tar_gz(match_dir: Path, output_path: Path) -> int:
    """Package a match directory as a .tar.gz archive. Returns the archive size in bytes."""
    folder_name = match_dir.name
    with tarfile.open(output_path, "w:gz") as tar:
        for item in sorted(match_dir.rglob("*")):
            if item.is_file():
                arcname = f"{folder_name}/{item.relative_to(match_dir)}"
                tar.add(str(item), arcname=arcname)
    return output_path.stat().st_size


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


def cmd_upload(args, config: dict):
    """Main upload command: scan local, compare remote, upload new matches."""
    s3 = create_s3_client(config)
    bucket = config["R2_BUCKET"]
    source_dir = Path(args.source_dir or config.get("MATCH_SOURCE_DIR", ""))

    if not source_dir or not source_dir.is_dir():
        print(f"Error: invalid source directory: {source_dir}")
        sys.exit(1)

    print(f"Source directory: {source_dir}")
    print(f"R2 bucket: {bucket}")
    print()

    # Fetch existing index
    print("Fetching remote index.json...")
    remote_entries = fetch_remote_index(s3, bucket)
    remote_folders = {e["folder"] for e in remote_entries}
    print(f"  {len(remote_entries)} matches currently in index.")

    # Scan local matches
    local_matches = scan_local_matches(source_dir)
    print(f"  {len(local_matches)} match folders found locally.")

    # Find new matches
    new_matches = [m for m in local_matches if m.name not in remote_folders]
    if not new_matches:
        print("\nNo new matches to upload. Everything is up to date.")
        return

    print(f"\n{len(new_matches)} new match(es) to upload:")
    for m in new_matches:
        size_mb = dir_size_bytes(m) / (1024 * 1024)
        print(f"  - {m.name} ({size_mb:.1f} MB)")

    if args.dry_run:
        print("\n[DRY RUN] No files were uploaded.")
        return

    # Upload each new match
    new_entries = []
    with tempfile.TemporaryDirectory() as tmp_dir:
        for i, match_dir in enumerate(new_matches, 1):
            folder_name = match_dir.name
            print(f"\n[{i}/{len(new_matches)}] Uploading: {folder_name}")

            # Read metadata
            infos = read_infos_json(match_dir)
            if infos is None:
                print(f"  Skipping (could not read infos.json)")
                continue

            # Create archive
            archive_path = Path(tmp_dir) / f"{folder_name}.tar.gz"
            print(f"  Packaging...")
            archive_size = create_tar_gz(match_dir, archive_path)
            size_mb = archive_size / (1024 * 1024)
            print(f"  Archive size: {size_mb:.1f} MB")

            # Upload archive
            r2_key = f"matches/{folder_name}.tar.gz"
            print(f"  Uploading to {r2_key}...")
            upload_file(s3, bucket, r2_key, archive_path)

            # Build index entry
            entry = build_index_entry(folder_name, infos, archive_size)
            new_entries.append(entry)

            # Clean up temp archive
            archive_path.unlink(missing_ok=True)
            print(f"  Done.")

    # Update index
    if new_entries:
        all_entries = remote_entries + new_entries
        print(f"\nUpdating index.json ({len(all_entries)} total matches)...")
        upload_index(s3, bucket, all_entries)
        print("Index updated.")

    print(f"\nUpload complete: {len(new_entries)} new match(es) uploaded.")


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


def main():
    parser = argparse.ArgumentParser(
        description="Upload GvG match recordings to Cloudflare R2."
    )
    parser.add_argument(
        "--env-file",
        type=Path,
        default=Path(__file__).parent / "r2_config.env",
        help="Path to config .env file (default: scripts/r2_config.env)",
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

    args = parser.parse_args()

    # Load config
    config = load_config(args.env_file)

    required_keys = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY", "R2_SECRET_KEY")
    missing = [k for k in required_keys if k not in config]
    if missing:
        print(f"Error: missing config values: {', '.join(missing)}")
        print(f"Set them in {args.env_file} or as environment variables.")
        sys.exit(1)

    if args.list_remote:
        cmd_list_remote(args, config)
    else:
        cmd_upload(args, config)


if __name__ == "__main__":
    main()
