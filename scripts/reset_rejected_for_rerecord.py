#!/usr/bin/env python3
"""Mark rejected match folders as `status='failed'` in the orchestrator DB so
the orchestrator will try to re-record them if they're still in the GW
observer queue.

Reads all folders under d:\\matcharchive\\rejected\\, extracts (date,
team1_tag, team2_tag) from each folder name, and updates the matching
DB row from status='completed' -> status='failed' plus clears output_path.

IMPORTANT: the orchestrator MUST be stopped before running this. SQLite
in WAL mode allows concurrent reads but concurrent writes can lock the DB
and corrupt state.

Usage:
    python reset_rejected_for_rerecord.py                 # dry-run
    python reset_rejected_for_rerecord.py --execute        # apply
    python reset_rejected_for_rerecord.py --scan-dir PATH  # default d:\\matcharchive\\rejected
    python reset_rejected_for_rerecord.py --db PATH        # default orchestrator.db in orch repo
"""

import argparse
import json
import re
import sqlite3
import sys
from pathlib import Path

DEFAULT_DB = Path(r"C:\Users\gwobserver\Documents\gw-observer-orchestrator\orchestrator.db")
DEFAULT_SCAN = Path(r"d:\matcharchive\rejected")

# Folder format: YYYY-MM-DD_MapName_MM.SS_[tag1]vs[tag2]
# MapName may contain underscores; tags are inside brackets.
FOLDER_RE = re.compile(
    r"^(?P<date>\d{4}-\d{2}-\d{2})_"
    r"(?P<map>.+)_"
    r"(?P<dur>\d{2}\.\d{2})_"
    r"\[(?P<tag1>[^\]]+)\]vs\[(?P<tag2>[^\]]+)\]$"
)


def parse_folder(name: str) -> str | None:
    m = FOLDER_RE.match(name)
    if not m:
        return None
    return m.group("date")


def guild_names_from_infos(folder: Path) -> tuple[str, str] | None:
    """Extract the two full guild names from infos.json."""
    infos = folder / "infos.json"
    if not infos.is_file():
        return None
    try:
        with open(infos, encoding="utf-8") as f:
            data = json.load(f)
    except (json.JSONDecodeError, OSError):
        return None
    guilds = data.get("guilds") or {}
    g1 = guilds.get("1") or {}
    g2 = guilds.get("2") or {}
    n1 = g1.get("name") or ""
    n2 = g2.get("name") or ""
    if not n1 or not n2:
        return None
    return n1, n2


def find_db_rows(conn: sqlite3.Connection, date: str, name1: str, name2: str, map_id: int | None = None) -> list[dict]:
    """Find rows matching date + guild names (either order).

    DB team_name values are prefixed with a rank like "#3 " so we match via LIKE.
    """
    q = (
        "SELECT id, status, team1_tag, team2_tag, team1_name, team2_name, "
        "       map_id, match_type, output_path, fingerprint "
        "FROM recordings "
        "WHERE date = ? AND status = 'completed' "
        "  AND ((team1_name LIKE ? AND team2_name LIKE ?) "
        "    OR (team1_name LIKE ? AND team2_name LIKE ?))"
    )
    p1 = f"%{name1}%"
    p2 = f"%{name2}%"
    cur = conn.execute(q, (date, p1, p2, p2, p1))
    rows = [dict(r) for r in cur.fetchall()]
    if map_id is not None and len(rows) > 1:
        rows = [r for r in rows if r.get("map_id") == map_id]
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scan-dir", type=Path, default=DEFAULT_SCAN,
                    help=f"Directory containing rejected folders (default: {DEFAULT_SCAN})")
    ap.add_argument("--db", type=Path, default=DEFAULT_DB,
                    help=f"SQLite DB path (default: {DEFAULT_DB})")
    ap.add_argument("--execute", action="store_true",
                    help="Apply UPDATE (default: dry-run)")
    args = ap.parse_args()

    if not args.scan_dir.is_dir():
        print(f"ERROR: scan-dir not found: {args.scan_dir}")
        return 1
    if not args.db.is_file():
        print(f"ERROR: DB not found: {args.db}")
        return 1

    folders = sorted(d for d in args.scan_dir.iterdir() if d.is_dir())
    print(f"Scanning {args.scan_dir} -> {len(folders)} folders")

    conn = sqlite3.connect(f"file:{args.db}?mode={'rw' if args.execute else 'ro'}", uri=True, timeout=10)
    conn.row_factory = sqlite3.Row

    unmatched: list[str] = []
    matches: list[tuple[Path, dict]] = []
    multi: list[tuple[Path, list[dict]]] = []

    for folder in folders:
        date = parse_folder(folder.name)
        if not date:
            unmatched.append(folder.name + "  (could not parse folder name)")
            continue
        names = guild_names_from_infos(folder)
        if not names:
            unmatched.append(folder.name + "  (no usable infos.json)")
            continue
        name1, name2 = names
        # Try to read map_id to disambiguate multi-matches
        map_id = None
        try:
            with open(folder / "infos.json", encoding="utf-8") as f:
                map_id = json.load(f).get("map_id")
        except Exception:
            pass
        rows = find_db_rows(conn, date, name1, name2, map_id)
        if not rows:
            unmatched.append(f"{folder.name}  (no DB row with status=completed for {date} {name1!r}/{name2!r})")
        elif len(rows) > 1:
            multi.append((folder, rows))
        else:
            matches.append((folder, rows[0]))

    print()
    print(f"Single-match: {len(matches)}")
    print(f"Multi-match (ambiguous): {len(multi)}")
    print(f"Unmatched: {len(unmatched)}")
    print()

    if matches:
        print("Candidates to update (status='completed' -> 'failed'):")
        for folder, row in matches:
            print(f"  id={row['id']:<6} {row['team1_tag']:<10}vs{row['team2_tag']:<10}  map={row['map_id']:<5} type={row['match_type']}  {folder.name}")

    if multi:
        print()
        print("AMBIGUOUS (multiple DB rows match; skipping to avoid mistakes):")
        for folder, rows in multi:
            print(f"  {folder.name}  -> {len(rows)} rows: ids={[r['id'] for r in rows]}")

    if unmatched:
        print()
        print("UNMATCHED:")
        for line in unmatched:
            print(f"  {line}")

    if not args.execute:
        print()
        print("Dry-run. Pass --execute to apply UPDATE.")
        return 0

    if not matches:
        print("Nothing to update.")
        return 0

    print()
    print(f"Updating {len(matches)} rows...")
    ids = [row["id"] for _, row in matches]
    placeholders = ",".join("?" * len(ids))
    cur = conn.execute(
        f"UPDATE recordings SET status='failed', output_path='' WHERE id IN ({placeholders})",
        ids,
    )
    conn.commit()
    print(f"  {cur.rowcount} row(s) updated.")
    conn.close()
    print("Done. Orchestrator will attempt to re-observe these identities on next filter tick (if they're still in the GW observer queue).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
