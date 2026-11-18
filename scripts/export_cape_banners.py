#!/usr/bin/env python3
"""
Export a PNG banner for every guild cape in the published match index.

The website's share-link page (watch.html) shows each team's cape. The site gets the
images only: the banners are composed by GW Observer itself, headless
(`GuildWarsObserver.exe --export-cape-banners <in.json> <outdir>`), exactly as the
replay's loading screen draws them, and written as <capeId>.png where capeId is
shape-detail-emblem-trim-bg_color-detail_color-emblem_color (MatchLinks::CapeId).

Steps:
  1. download index.json.gz from R2 (same config and credentials as upload_to_r2.py:
     ../gwobserver-private/r2_config.env, then scripts/r2_config.env, then the environment);
  2. collect every distinct cape in entries' guilds.*.cape;
  3. run the exe on them; files already in the output folder are skipped.

Usage:
  python scripts/export_cape_banners.py [--out DIR] [--exe PATH] [--env-file PATH]
                                        [--index-file index.json]

--index-file reads a local copy of the index instead of R2 (for example the app's
MatchCache/index_cache.json). Nothing is committed or pushed anywhere.
"""

import argparse
import gzip
import json
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent

CAPE_FIELDS = ("bg_color", "detail_color", "emblem_color", "shape", "detail", "emblem", "trim")


def cape_id(cape: dict) -> str:
    """The banner's file name stem; the order is MatchLinks::CapeId's."""
    order = ("shape", "detail", "emblem", "trim", "bg_color", "detail_color", "emblem_color")
    return "-".join(str(int(cape.get(k, 0) or 0)) for k in order)


def default_env_file() -> Path:
    private_env = REPO_ROOT.parent / "gwobserver-private" / "r2_config.env"
    return private_env if private_env.exists() else SCRIPT_DIR / "r2_config.env"


def fetch_index_from_r2(env_file: Path) -> list[dict]:
    """index.json.gz from the bucket (index.json when the gzipped copy is missing)."""
    sys.path.insert(0, str(SCRIPT_DIR))
    import upload_to_r2  # needs boto3; exits with a message when it is missing

    config = upload_to_r2.load_config(env_file)
    missing = [k for k in ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY", "R2_SECRET_KEY") if not config.get(k)]
    if missing:
        sys.exit(f"Missing R2 settings {', '.join(missing)} (looked in {env_file} and the environment).")
    s3 = upload_to_r2.create_s3_client(config)
    bucket = config["R2_BUCKET"]
    try:
        body = s3.get_object(Bucket=bucket, Key="index.json.gz")["Body"].read()
        data = json.loads(gzip.decompress(body).decode("utf-8"))
        return data.get("matches", [])
    except Exception as e:  # the gzipped copy is an addition; fall back to the plain one
        print(f"index.json.gz unavailable ({e}); reading index.json")
        return upload_to_r2.fetch_remote_index(s3, bucket)


def read_index_file(path: Path) -> list[dict]:
    raw = path.read_bytes()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    data = json.loads(raw.decode("utf-8"))
    return data.get("matches", []) if isinstance(data, dict) else data


def collect_capes(entries: list[dict]) -> list[dict]:
    capes: dict[str, dict] = {}
    for entry in entries:
        guilds = entry.get("guilds") or {}
        if not isinstance(guilds, dict):
            continue
        for guild in guilds.values():
            cape = guild.get("cape") if isinstance(guild, dict) else None
            if not isinstance(cape, dict):
                continue
            clean = {k: int(cape.get(k, 0) or 0) for k in CAPE_FIELDS}
            capes.setdefault(cape_id(clean), clean)
    return list(capes.values())


def find_exe(explicit: str | None) -> Path | None:
    if explicit:
        return Path(explicit)
    for candidate in (REPO_ROOT / "x64" / "Release" / "GuildWarsObserver.exe",
                      REPO_ROOT / "GuildWarsObserver.exe"):
        if candidate.exists():
            return candidate
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="Export guild cape banner PNGs for the website.")
    parser.add_argument("--out", type=Path, default=REPO_ROOT.parent / "gwobserver-website" / "assets" / "capes",
                        help="output folder (default: ../gwobserver-website/assets/capes)")
    parser.add_argument("--exe", help="GuildWarsObserver.exe to run (default: x64/Release next to this repo)")
    parser.add_argument("--env-file", type=Path, default=default_env_file(),
                        help="R2 config (default: gwobserver-private/r2_config.env or scripts/r2_config.env)")
    parser.add_argument("--index-file", type=Path, help="read this local index instead of R2")
    args = parser.parse_args()

    exe = find_exe(args.exe)
    if not exe or not exe.exists():
        print("GuildWarsObserver.exe not found; pass --exe.")
        return 1

    entries = read_index_file(args.index_file) if args.index_file else fetch_index_from_r2(args.env_file)
    capes = collect_capes(entries)
    print(f"{len(entries)} match(es) in the index, {len(capes)} distinct cape(s)")

    out_dir = args.out.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    before = {p.name for p in out_dir.glob("*.png")}

    with tempfile.TemporaryDirectory() as tmp:
        in_json = Path(tmp) / "capes.json"
        in_json.write_text(json.dumps(capes), encoding="utf-8")
        result = subprocess.run([str(exe), "--export-cape-banners", str(in_json), str(out_dir)],
                                cwd=str(exe.parent), capture_output=True, text=True)
    if result.stdout.strip():
        print(result.stdout.strip())

    after = {p.name for p in out_dir.glob("*.png")}
    wanted = {cape_id(c) + ".png" for c in capes}
    print(f"{len(after - before)} new banner(s), {len(wanted & after)}/{len(wanted)} capes have one, in {out_dir}")
    if result.returncode != 0:
        print(f"The exporter exited with code {result.returncode}.")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
