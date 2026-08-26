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
    python upload_to_r2.py --source-dir <dir> --include-scrimmage --dry-run
                                             # publish a manually-recorded
                                             # guild scrim to the public feed
                                             # (bypasses the private-scrim guard;
                                             # always dry-run first)

Configuration via scripts/r2_config.env or environment variables:
    R2_ENDPOINT, R2_BUCKET, R2_ACCESS_KEY, R2_SECRET_KEY, MATCH_SOURCE_DIR
"""

import argparse
import io
import gzip
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


def is_scrim_recording(infos: dict | None) -> bool:
    """True if this recording is a private scrim that must never reach the
    public R2 bucket / index.json.

    Detection is via ``infos.json["occasion"]`` — the GWToolbox plugin
    writes "General Scrimmage" or "Scrimmage" for the My-Guild's-GvG
    flow used by the scrim booking system. AT matches use "Automated
    Tournament", "mAT ...", "A AT", "B AT", "C AT", etc.

    Scrim recordings flow through the orchestrator's private
    ``scrim_upload`` pipeline (different R2 prefix, presigned URL only).
    This function exists so a stray scrim folder in MatchDirectory or
    GwReplayRecorder cannot leak to the public feed.
    """
    if not infos:
        return False
    occasion = (infos.get("occasion") or "").lower()
    return "scrimmage" in occasion


def collect_recordings(recording_dir: Path, staging_dir: Path, include_scrimmage: bool = False) -> int:
    """Move completed recordings from GWToolbox output to the upload staging directory.

    Only moves match folders that:
    - Contain infos.json (recording finished writing metadata)
    - Have no files modified in the last 5 minutes (not still being recorded)
    - Don't already exist in the staging directory
    - Are NOT private scrim recordings (those are handled by the
      orchestrator's scrim_upload pipeline; never let them into the
      public AT staging area) — unless ``include_scrimmage`` is True,
      which is an explicit operator opt-in to publish a manually-recorded
      scrim to the public feed.
    """
    STALENESS_SECONDS = 300  # 5 minutes
    now = time.time()
    count = 0

    for entry in sorted(recording_dir.iterdir()):
        if not entry.is_dir() or entry.name in ("processed", "rejected"):
            continue
        infos_path = entry / "infos.json"
        if not infos_path.exists():
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

        # Defense in depth: never collect a scrim recording into the public
        # AT staging area, even if the orchestrator failed to move it first.
        try:
            scrim_infos = read_infos_json(entry)
        except Exception:
            scrim_infos = None
        if is_scrim_recording(scrim_infos):
            if include_scrimmage:
                print(f"  ⚠ INCLUDING scrimmage match (private-scrim guard bypassed): {entry.name}")
            else:
                print(f"  Skipping (private scrim, occasion={scrim_infos.get('occasion','?')!r}): {entry.name}")
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


def count_recorded_players(match_dir: Path, infos: dict) -> int:
    """Cheap count of how many roster players actually have a position file.

    Used for dedup so we prefer the most-complete recording when several
    observer instances captured the same match. GWToolbox can drop the
    per-agent position file entirely for players whose first position
    event was missed (network hiccup at match start, late instance
    join, etc.) — the infos.json roster is unaffected because it comes
    from a single packet at match end.

    The 1 KiB size floor filters header-only files that exist but never
    received a position event.
    """
    agents_dir = match_dir / "Agents"
    if not agents_dir.is_dir():
        return 0
    player_ids: set[int] = set()
    for party in (infos.get("parties") or {}).values():
        if not isinstance(party, dict):
            continue
        for p in (party.get("PLAYER") or []):
            pid = p.get("id") if isinstance(p, dict) else None
            if isinstance(pid, int) and pid > 0:
                player_ids.add(pid)
    count = 0
    for pid in player_ids:
        for ext in (".txt.gz", ".txt"):
            f = agents_dir / f"{pid}{ext}"
            if f.is_file() and f.stat().st_size > 1024:
                count += 1
                break
    return count


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


# ── Desktop preview fields ──────────────────────────────────────────────────
#
# Pulled out of build_index_entry so backfill_index.py can call the real
# producers over already-published entries instead of copying these shapes.
# Both return None for "the recording does not carry this", which is not the
# same as zero -- the client distinguishes the two and renders "-".

CAPE_FIELDS = (
    "bg_color", "detail_color", "emblem_color", "shape", "detail", "emblem",
    "trim",
)

# Positional, not named keys: ~40 bytes per player against ~528 as named keys
# (see the sidecar note below). The first three positions are frozen for
# clients already in the wild; new counters are append-only.
PREVIEW_FIELDS = (
    "interrupted_count", "cancelled_skills_count", "skills_finished",
    "total_damage_received", "total_healing_dealt", "total_healing_received",
)


def build_cape(guild_obj: dict) -> dict | None:
    """The guild's cape from an infos.json guild object, or None."""
    cape = guild_obj.get("cape")
    if not isinstance(cape, dict):
        return None
    return {field: cape.get(field, 0) for field in CAPE_FIELDS}


def build_preview_stats(player: dict) -> list | None:
    """The compact preview array from an infos.json player object, or None."""
    if not any(field in player for field in PREVIEW_FIELDS):
        return None
    return [player.get(field) for field in PREVIEW_FIELDS]


def build_index_entry(
    folder_name: str,
    infos: dict,
    archive_size: int,
    recorded_players: int = 0,
) -> dict:
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
        "recorded_players": recorded_players,
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
                cape = build_cape(guild_obj)
                if cape is not None:
                    entry["guilds"][guild_key]["cape"] = cape

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
                player_out = {
                    "encoded_name": player.get("encoded_name", ""),
                    "primary": player.get("primary", 0),
                    "secondary": player.get("secondary", 0),
                    "player_number": player.get("player_number", 0),
                    "used_skills": player.get("used_skills", []),
                    "skill_template_code": player.get("skill_template_code", ""),
                    "kills": player.get("kills", 0),
                    "deaths": player.get("deaths", 0),
                    "total_damage": player.get("total_damage", 0),
                }
                preview = build_preview_stats(player)
                if preview is not None:
                    player_out["preview_stats"] = preview
                players_out.append(player_out)
            parties_out[party_id] = {"PLAYER": players_out}
        entry["parties"] = parties_out

    return entry


# ── Combat stats sidecar ────────────────────────────────────────────────────
#
# infos.json carries far more per-player detail than build_index_entry above
# publishes, and the rest was simply being dropped. It is not folded into
# index.json because that object is downloaded by every desktop client on
# every refresh: as named JSON keys these fields cost ~528 bytes per player
# against a 194-byte published object, which would take index.json from
# 24 MB to 46 MB. So they go to a website-only sidecar instead, sharded by
# month to keep any single fetch bounded.
#
# An allowlist rather than "everything build_index_entry skipped": id,
# gadget_id and model_id are per-match agent identifiers with no meaning
# across matches, and gender/level say nothing about a GvG. Naming the wanted
# fields also means a new field appearing upstream cannot silently change the
# shape of a published object.
STATS_PLAYER_FIELDS = (
    # Interrupts, in both directions.
    "interrupted_count", "interrupted_skills_count",
    # Casts started and not completed -- "fake casts".
    "cancelled_skills_count", "cancelled_attacks_count",
    # Denominators for every cast-completion ratio.
    "skills_activated", "skills_finished", "skills_stopped",
    "attacks_started", "attacks_finished", "attacks_stopped",
    "attack_skills_activated", "attack_skills_finished", "attack_skills_stopped",
    "crits_dealt", "crits_received",
    # total_damage, kills and deaths are already in index.json; these are the
    # other three sides of the same story.
    "total_damage_received", "total_healing_dealt", "total_healing_received",
    # team_id ties a player to a side; guild_id is the player's *home* guild,
    # which is what distinguishes a guild's own member from a guest -- a
    # player is a member iff guild_id equals their party's key in `guilds`.
    "team_id", "guild_id",
)

STATS_SCHEMA = 2


def stats_key(month: str) -> str:
    """R2 key for one month's stats shard, beside the tournaments/ prefix."""
    return f"stats/{month}.json"


def build_stats_entry(infos: dict, match_dir: Path | None = None) -> dict:
    """Per-match combat stats, or {} when this recording carries none.

    Every field is optional. Older captures predate the per-player stat block
    entirely (`recording_version` is absent on those, 2 on newer ones), so a
    missing key is a normal state and is omitted rather than written as 0 --
    "we did not record this" and "this happened zero times" are different
    facts, and collapsing them is the same class of bug as reading only
    `winner_party_id` and scoring every match a loss.
    """
    parties_raw = infos.get("parties")
    if not isinstance(parties_raw, dict):
        return {}

    parties_out: dict[str, list] = {}
    for party_id, party_obj in parties_raw.items():
        if not isinstance(party_obj, dict):
            continue
        players_out = []
        for player in party_obj.get("PLAYER", []):
            if not isinstance(player, dict):
                continue
            # The join key back to index.json's player list. Carried
            # explicitly so the two objects do not have to agree on array
            # order -- they are written by different code paths.
            row = {"player_number": player.get("player_number", 0)}
            for field in STATS_PLAYER_FIELDS:
                if field in player:
                    row[field] = player[field]
            # A row holding nothing but its join key is noise.
            if len(row) > 1:
                players_out.append(row)
        if players_out:
            parties_out[party_id] = players_out

    if not parties_out:
        return {}

    out: dict = {"players": parties_out}
    if "recording_version" in infos:
        out["recording_version"] = infos["recording_version"]
    # index.json publishes team_kills and team_damage but not team_healing.
    if isinstance(infos.get("team_healing"), dict):
        out["team_healing"] = infos["team_healing"]
    if match_dir is not None:
        # Imported lazily so listing/deleting remote objects does not depend on
        # the event parser. A missing StoC stream is normal for legacy captures
        # and stays absent rather than becoming a page full of zeroes.
        try:
            from combat_analytics import build_from_match_dir
            analytics = build_from_match_dir(infos, match_dir)
        except Exception as exc:
            # Analytics are optional; an event-parser defect must never leave
            # a successfully uploaded archive without its index entry.
            print(f"  Warning: combat analytics unavailable: {type(exc).__name__}: {exc}")
            analytics = {}
        if analytics:
            out["combat_analytics"] = analytics
    return out


def fetch_remote_stats(s3, bucket: str, month: str) -> dict:
    """One month's stats shard, or an empty shard if it does not exist yet."""
    try:
        resp = s3.get_object(Bucket=bucket, Key=stats_key(month))
        data = json.loads(resp["Body"].read().decode("utf-8-sig"))
        matches = data.get("matches")
        if isinstance(matches, dict):
            return matches
        return {}
    except Exception as e:
        if "NoSuchKey" in str(type(e).__name__) or "NoSuchKey" in str(e):
            return {}
        # Any other failure must not be mistaken for "no stats yet": merging
        # onto {} would silently erase the month.
        raise


def upload_stats(s3, bucket: str, month: str, matches: dict):
    """Write one month's stats shard."""
    body = json.dumps(
        {"schema": STATS_SCHEMA, "month": month, "matches": matches},
        ensure_ascii=False, separators=(",", ":"),
    ).encode("utf-8")
    s3.put_object(
        Bucket=bucket,
        Key=stats_key(month),
        Body=body,
        ContentType="application/json",
    )


def month_of_entry(entry: dict) -> str:
    """The `YYYY-MM` shard an index entry belongs to, or "" if undatable."""
    date_str = entry.get("date") or ""
    return date_str[:7] if len(date_str) >= 7 else ""


def match_fingerprint(infos: dict) -> str:
    """Build a content-based fingerprint from match metadata for dedup.

    Uses date + map_id + sorted team tags (from the two match parties only)
    so that the same match is identified regardless of folder naming.
    The occasion is also part of the identity, so a Swiss round and a
    playoff game between the same two guilds on the same map and day are
    treated as distinct matches instead of colliding.
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
    occasion = (infos.get("occasion", "") or "").strip()
    return f"{year:04d}-{month:02d}-{day:02d}_{map_id}_{'_vs_'.join(tags)}_{occasion}"


def index_entry_fingerprint(entry: dict) -> str:
    """Build a content fingerprint from an index.json entry for dedup.

    The occasion is included in the identity to stay consistent with
    match_fingerprint, so a Swiss round and a playoff game between the
    same two guilds on the same map and day are treated as distinct matches.
    """
    party_ids = set(entry.get("parties", {}).keys())
    guilds = entry.get("guilds", {})
    tags = sorted(
        g.get("tag", "") for gid, g in guilds.items()
        if isinstance(g, dict) and g.get("tag") and gid in party_ids
    )
    occasion = (entry.get("occasion", "") or "").strip()
    return f"{entry.get('date', '')}_{entry.get('map_id', 0)}_{'_vs_'.join(tags)}_{occasion}"


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


# ── index.json serialisation ────────────────────────────────────────────────
#
# One helper, because index.json has several writers -- cmd_upload, the
# maintenance commands below, purge_leaked_scrims.py, and the orchestrator's
# scrim_uploader -- and any one of them writing a different shape re-inflates
# the object for every desktop client on its next run.
#
# Minified, not indent=2. Measured on the live 2,643-match object: 23.07 MB
# on the wire against 3.7 MB of actual JSON payload, the rest whitespace, key
# names and punctuation. Minifying alone takes it to 9.27 MB, costs no schema
# change, and every client already in the wild parses it unchanged -- JSON is
# JSON. The stats sidecar has always been written this way (upload_stats
# below); index.json was the only pretty-printed object in the bucket.
#
# CacheControl is "no-cache", which means *revalidate*, not "do not store" --
# the correct directive for the ETag/If-None-Match path, and what the two
# writers that set it at all already used.

INDEX_KEY = "index.json"
# Published alongside the plain key, never instead of it: a build already in
# the wild knows nothing about this object, and flipping index.json itself to
# Content-Encoding: gzip would break every one of them. Clients that
# understand it ask for this key first and fall back to the plain one.
INDEX_GZ_KEY = "index.json.gz"
INDEX_CACHE_CONTROL = "no-cache"


def serialize_index(entries: list[dict]) -> bytes:
    """Serialise index entries to the exact bytes published to R2."""
    index_data = {"version": 1, "matches": entries}
    return json.dumps(
        index_data, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def gzip_bytes(body: bytes) -> bytes:
    """Deterministic gzip: no mtime, so identical input keeps its ETag.

    That matters more than it looks. The whole point of the .gz key is that a
    client revalidates with If-None-Match and gets a 304; a timestamp in the
    header would change the ETag on every publish and turn every launch back
    into a full download.
    """
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=6, mtime=0) as f:
        f.write(body)
    return buf.getvalue()


def write_index(s3, bucket: str, entries: list[dict]) -> int:
    """Publish index.json and index.json.gz. The single writer.

    The gzipped key goes first: if it fails, nothing has changed and both
    objects still agree. A failure after it leaves newer clients ahead of
    older ones rather than serving anybody a half-written index.
    """
    body = serialize_index(entries)
    s3.put_object(
        Bucket=bucket,
        Key=INDEX_GZ_KEY,
        Body=gzip_bytes(body),
        ContentType="application/gzip",
        CacheControl=INDEX_CACHE_CONTROL,
    )
    s3.put_object(
        Bucket=bucket,
        Key=INDEX_KEY,
        Body=body,
        ContentType="application/json",
        CacheControl=INDEX_CACHE_CONTROL,
    )
    return len(body)


def upload_index(s3, bucket: str, entries: list[dict]):
    """Back-compat alias for write_index (imported by the fixup scripts)."""
    write_index(s3, bucket, entries)


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

    if args.dry_run:
        print("Dry run: skipping recording collection + archive extraction (no files will be moved).\n")
    else:
        # Collect completed recordings from GWToolbox output
        recording_source = config.get("RECORDING_SOURCE_DIR", "")
        if recording_source:
            rec_dir = Path(os.path.expandvars(os.path.expanduser(recording_source)))
            if rec_dir.is_dir():
                print("Collecting new recordings...")
                collected = collect_recordings(rec_dir, source_dir, include_scrimmage=args.include_scrimmage)
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
    remote_by_fp: dict[str, dict] = {}
    for e in remote_entries:
        remote_by_fp[index_entry_fingerprint(e)] = e
    print(f"  {len(remote_entries)} matches currently in index.")

    # Scan local matches
    local_matches = scan_local_matches(source_dir)
    print(f"  {len(local_matches)} match folders found locally.")
    report["matched_local"] = len(local_matches)

    # First pass: inspect each local match, group by content fingerprint.
    # When multiple observer instances captured the same match, GWToolbox
    # produces near-identical folders (e.g. _24.53_ and _24.54_); we pick
    # the one with the most player position files, not the first by name.
    from collections import defaultdict
    local_by_fp: dict[str, list[tuple[Path, dict, int]]] = defaultdict(list)
    included_scrimmage_count = 0
    for m in local_matches:
        if m.name in remote_folders or sanitize_folder_name(m.name) in remote_folders:
            continue
        infos = read_infos_json(m)
        if is_scrim_recording(infos):
            if args.include_scrimmage:
                print(f"  ⚠ INCLUDING scrimmage match (private-scrim guard bypassed): {m.name}")
                included_scrimmage_count += 1
            else:
                print(f"  Skipping (private scrim, occasion={(infos.get('occasion') or '?')!r}): {m.name}")
                continue
        if not infos:
            continue
        fp = match_fingerprint(infos)
        rp = count_recorded_players(m, infos)
        local_by_fp[fp].append((m, infos, rp))

    if args.include_scrimmage and included_scrimmage_count:
        print(f"⚠ {included_scrimmage_count} scrimmage-occasion match(es) will be published to the PUBLIC feed (guard bypassed).")

    # Second pass: for each fingerprint, pick the best local copy and
    # decide whether it beats whatever is already in R2.
    new_matches: list[Path] = []
    match_recorded_players: dict[str, int] = {}  # folder -> recorded_players
    folders_to_replace: set[str] = set()  # remote folders to delete and supersede
    for fp, group in local_by_fp.items():
        # Best = most recorded players, tie-break by name for determinism
        group.sort(key=lambda x: (-x[2], x[0].name))
        best_path, best_infos, best_rp = group[0]
        for loser_path, _, loser_rp in group[1:]:
            print(f"  Skipping (local duplicate, {loser_rp} players vs "
                  f"{best_rp} in {best_path.name}): {loser_path.name}")

        remote_entry = remote_by_fp.get(fp)
        if remote_entry is not None:
            remote_rp = remote_entry.get("recorded_players")
            if remote_rp is None:
                # Legacy entry without completeness data: trust it, skip.
                print(f"  Skipping duplicate (content match): {best_path.name}")
                continue
            if best_rp <= remote_rp:
                print(f"  Skipping duplicate (remote has {remote_rp} players, "
                      f"local {best_rp}): {best_path.name}")
                continue
            print(f"  Replacing remote (local has {best_rp} players vs remote "
                  f"{remote_rp}): {best_path.name} supersedes {remote_entry['folder']}")
            folders_to_replace.add(remote_entry["folder"])

        new_matches.append(best_path)
        match_recorded_players[best_path.name] = best_rp
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

    # Delete obsolete R2 archives that the local copies will supersede.
    # The corresponding index entries are removed when we rebuild the index below.
    for folder in sorted(folders_to_replace):
        r2_key = f"matches/{folder}.tar.gz"
        try:
            s3.delete_object(Bucket=bucket, Key=r2_key)
            print(f"  Deleted obsolete archive: {r2_key}")
        except Exception as e:
            print(f"  Warning: failed to delete obsolete {r2_key}: {e}")
            report["warnings"].append({"match": folder, "warning": f"Failed to delete obsolete archive: {e}"})

    # Upload each new match
    new_entries = []
    new_stats: dict[str, dict] = {}      # month -> {folder: stats entry}
    with tempfile.TemporaryDirectory() as tmp_dir:
        for i, match_dir in enumerate(new_matches, 1):
            folder_name = match_dir.name
            safe_name = sanitize_folder_name(folder_name)
            renamed = safe_name != folder_name
            recorded_players = match_recorded_players.get(folder_name, 0)

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
                entry = build_index_entry(safe_name, infos, 0, recorded_players)
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
            entry = build_index_entry(safe_name, infos, archive_size, recorded_players)
            new_entries.append(entry)
            # Combat stats go to a per-month sidecar, keyed by the same folder
            # name the index entry uses so the two join without a new identity.
            stats_entry = build_stats_entry(infos, match_dir)
            if stats_entry:
                month = month_of_entry(entry)
                if month:
                    new_stats.setdefault(month, {})[safe_name] = stats_entry
                else:
                    report["warnings"].append({
                        "match": folder_name,
                        "warning": "No usable date; combat stats not published",
                    })
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

    # Update index (deduplicate by folder name, new entries take precedence,
    # and drop any remote entries that the new uploads superseded).
    if new_entries or folders_to_replace:
        kept_remote = [e for e in remote_entries if e["folder"] not in folders_to_replace]
        seen: dict[str, dict] = {}
        for entry in kept_remote + new_entries:
            seen[entry["folder"]] = entry
        all_entries = list(seen.values())
        deduped = len(kept_remote) + len(new_entries) - len(all_entries)
        if deduped:
            print(f"\n  Removed {deduped} duplicate index entry(ies).")
        if folders_to_replace:
            print(f"  Superseded {len(folders_to_replace)} previous upload(s).")
        print(f"\nUpdating index.json ({len(all_entries)} total matches)...")
        try:
            upload_index(s3, bucket, all_entries)
            print("Index updated.")
        except Exception as e:
            msg = f"Failed to update index.json: {e}"
            print(f"ERROR: {msg}")
            report["errors"].append({"match": "", "error": msg})
            report["status"] = "error"

    # Combat stats sidecar, one shard per month touched. Written *after* the
    # index on purpose: index.json is what the client and the website read, so
    # a stats failure must leave a complete index behind rather than the other
    # way round. A superseded folder is dropped from its shard for the same
    # reason the index rebuild drops it -- a stale stats row keyed to an
    # archive that no longer exists would be joined onto nothing.
    if new_stats or folders_to_replace:
        replace_by_month: dict[str, set] = {}
        for e in remote_entries:
            if e["folder"] in folders_to_replace:
                month = month_of_entry(e)
                if month:
                    replace_by_month.setdefault(month, set()).add(e["folder"])

        for month in sorted(set(new_stats) | set(replace_by_month)):
            additions = new_stats.get(month, {})
            removals = replace_by_month.get(month, set())
            try:
                shard = fetch_remote_stats(s3, bucket, month)
                for folder in removals:
                    shard.pop(folder, None)
                shard.update(additions)
                upload_stats(s3, bucket, month, shard)
                print(f"  Stats {month}: +{len(additions)} match(es), "
                      f"{len(shard)} total.")
            except Exception as e:
                # Not fatal. The stats sidecar feeds website analytics; the
                # recordings and the index are already safely uploaded, and a
                # missing shard degrades those pages rather than the archive.
                msg = f"Failed to update {stats_key(month)}: {e}"
                print(f"  Warning: {msg}")
                report["warnings"].append({"match": "", "warning": msg})

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
        "--include-scrimmage",
        action="store_true",
        default=False,
        help="Also publish matches whose occasion is a scrimmage (bypasses the "
             "private-scrim guard). For manually-recorded games you intend to "
             "make PUBLIC. Always --dry-run first and point --source-dir at "
             "the specific folder(s).",
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
