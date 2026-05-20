#!/usr/bin/env python3
"""One-off helper to delete a specific object from the R2 bucket.

Usage:
    python delete_r2_object.py <key>            # dry-run (default)
    python delete_r2_object.py <key> --execute  # actually delete
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from upload_to_r2 import load_config, create_s3_client


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("key", help="R2 object key (e.g. matches/foo.tar.gz)")
    parser.add_argument("--execute", action="store_true", help="Actually delete (default: dry-run)")
    args = parser.parse_args()

    script_dir = Path(__file__).parent
    private_env = script_dir.parent.parent / "gwobserver-private" / "r2_config.env"
    local_env = script_dir / "r2_config.env"
    env_path = private_env if private_env.exists() else local_env

    config = load_config(env_path)
    s3 = create_s3_client(config)
    bucket = config["R2_BUCKET"]

    try:
        head = s3.head_object(Bucket=bucket, Key=args.key)
        size_mb = head["ContentLength"] / (1024 * 1024)
        print(f"Found: s3://{bucket}/{args.key} ({size_mb:.1f} MB, last modified {head['LastModified']})")
    except Exception as e:
        print(f"Object not found or error: {e}")
        sys.exit(1)

    if not args.execute:
        print("[DRY RUN] Pass --execute to delete.")
        return

    s3.delete_object(Bucket=bucket, Key=args.key)
    print(f"Deleted: s3://{bucket}/{args.key}")


if __name__ == "__main__":
    main()
