#!/usr/bin/env python3
"""Verify a specific match in R2: head object, find its index entry, fetch + extract.

Usage:
    python check_match_in_r2.py <folder-name>
"""
import argparse
import io
import json
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from upload_to_r2 import load_config, create_s3_client


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("folder")
    args = parser.parse_args()

    script_dir = Path(__file__).parent
    private_env = script_dir.parent.parent / "gwobserver-private" / "r2_config.env"
    local_env = script_dir / "r2_config.env"
    env_path = private_env if private_env.exists() else local_env

    config = load_config(env_path)
    s3 = create_s3_client(config)
    bucket = config["R2_BUCKET"]
    key = f"matches/{args.folder}.tar.gz"

    print(f"Bucket: {bucket}")
    print()
    print(f"[1/3] head_object: {key}")
    head = s3.head_object(Bucket=bucket, Key=key)
    print(f"  size: {head['ContentLength']:,} bytes")
    print(f"  last modified: {head['LastModified']}")
    print()

    print("[2/3] looking up in index.json")
    resp = s3.get_object(Bucket=bucket, Key="index.json")
    idx = json.loads(resp["Body"].read().decode("utf-8"))
    matches = idx.get("matches", [])
    print(f"  index has {len(matches)} entries")
    entry = next((m for m in matches if m.get("folder") == args.folder), None)
    if entry is None:
        print(f"  NOT FOUND in index.json")
        sys.exit(1)
    print(f"  folder:           {entry.get('folder')}")
    print(f"  date:             {entry.get('date')}")
    print(f"  duration:         {entry.get('duration')}")
    print(f"  occasion:         {entry.get('occasion')}")
    print(f"  winner_party_id:  {entry.get('winner')}")
    print(f"  size_bytes:       {entry.get('size_bytes'):,}")
    print(f"  recorded_players: {entry.get('recorded_players', '<missing>')}")
    print(f"  guild tags:       {[g.get('tag') for g in entry.get('guilds', {}).values()]}")
    party_player_count = sum(
        len(p.get("PLAYER", [])) for p in entry.get("parties", {}).values()
    )
    print(f"  roster players:   {party_player_count}")
    print()

    print(f"[3/3] downloading and extracting archive")
    body = s3.get_object(Bucket=bucket, Key=key)["Body"].read()
    print(f"  downloaded {len(body):,} bytes")
    with tarfile.open(fileobj=io.BytesIO(body), mode="r:gz") as tar:
        names = tar.getnames()
        agents = [n for n in names if "/Agents/" in n and (n.endswith(".txt.gz") or n.endswith(".txt"))]
        has_infos = any(n.endswith("/infos.json") for n in names)
        stoc = [n for n in names if "/StoC/" in n]
    print(f"  total entries in archive: {len(names)}")
    print(f"  infos.json present:       {has_infos}")
    print(f"  StoC files:               {len(stoc)}")
    print(f"  Agents/ files:            {len(agents)}")
    # Cross-check: how many roster player IDs have an Agents/ file in the archive?
    player_ids = set()
    for party in entry.get("parties", {}).values():
        for p in party.get("PLAYER", []):
            # parties in index.json only has subset — fall back to checking infos.json from archive
            pass
    with tarfile.open(fileobj=io.BytesIO(body), mode="r:gz") as tar:
        for member in tar.getmembers():
            if member.name.endswith("/infos.json"):
                f = tar.extractfile(member)
                infos_raw = f.read().decode("utf-8", errors="replace")
                infos_doc = json.loads(infos_raw)
                for party in infos_doc.get("parties", {}).values():
                    for p in party.get("PLAYER", []):
                        pid = p.get("id")
                        if isinstance(pid, int):
                            player_ids.add(pid)
                break
    agents_ids = {Path(n).stem.removesuffix(".txt") for n in agents}
    agents_ids = {int(s) for s in agents_ids if s.isdigit()}
    have = player_ids & agents_ids
    missing = player_ids - agents_ids
    print(f"  roster IDs present in Agents/: {len(have)}/{len(player_ids)}")
    if missing:
        print(f"  MISSING from Agents/: {sorted(missing)}")
    else:
        print("  All roster players have position files. Archive is complete.")


if __name__ == "__main__":
    main()
