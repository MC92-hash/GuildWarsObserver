#!/usr/bin/env python3
"""
Upload builds.json to Cloudflare R2.

Usage:
    python push_builds.py <path_to_builds.json>
    python push_builds.py <path_to_builds.json> --key <contributor_key>

The contributor key is validated before uploading.
R2 credentials are read from scripts/r2_config.env.
"""

import json
import sys
import hashlib
from pathlib import Path

try:
    import boto3
except ImportError:
    print("Error: boto3 is required. Install with: pip install boto3")
    sys.exit(1)


# Valid contributor key hashes (SHA-256).
# To add a new contributor: hash their key and add it here.
VALID_KEY_HASHES = {
    # Default admin key hash — replace with real hashes
    hashlib.sha256(b"gwobserver-contributor-2026").hexdigest(),
    # Test contributor keys (see scripts/contributor_keys.env)
    "eecef75586867da0205f6465d667f8ae2f63a68b52c79ff657f59d56af8c6655",
    "1330209e5c93f5d82bb38ea2cf5382a525bfaf8ffbfa6e81cb91fc3562761d98",
}


def load_config(env_path: Path) -> dict:
    config = {}
    if env_path.exists():
        with open(env_path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    key, _, value = line.partition("=")
                    config[key.strip()] = value.strip()
    return config


def validate_key(key: str) -> bool:
    key_hash = hashlib.sha256(key.encode("utf-8")).hexdigest()
    return key_hash in VALID_KEY_HASHES


def main():
    if len(sys.argv) < 2:
        print("Usage: push_builds.py <builds.json> [--key <contributor_key>]")
        sys.exit(1)

    builds_path = Path(sys.argv[1])
    if not builds_path.exists():
        print(f"Error: {builds_path} not found")
        sys.exit(1)

    # Parse optional --key argument
    key = None
    if "--key" in sys.argv:
        idx = sys.argv.index("--key")
        if idx + 1 < len(sys.argv):
            key = sys.argv[idx + 1]

    if key and not validate_key(key):
        print("Error: invalid contributor key")
        sys.exit(1)

    # Load R2 config
    script_dir = Path(__file__).parent
    config = load_config(script_dir / "r2_config.env")

    required = ("R2_ENDPOINT", "R2_BUCKET", "R2_ACCESS_KEY", "R2_SECRET_KEY")
    missing = [k for k in required if k not in config]
    if missing:
        print(f"Error: missing config: {', '.join(missing)}")
        sys.exit(1)

    # Validate JSON
    content = builds_path.read_text(encoding="utf-8")
    try:
        json.loads(content)
    except json.JSONDecodeError as e:
        print(f"Error: invalid JSON in {builds_path}: {e}")
        sys.exit(1)

    # Upload
    s3 = boto3.client(
        "s3",
        endpoint_url=config["R2_ENDPOINT"],
        aws_access_key_id=config["R2_ACCESS_KEY"],
        aws_secret_access_key=config["R2_SECRET_KEY"],
        region_name="auto",
    )

    print(f"Uploading {builds_path} to R2...")
    s3.put_object(
        Bucket=config["R2_BUCKET"],
        Key="builds.json",
        Body=content.encode("utf-8"),
        ContentType="application/json",
    )
    print("Done. builds.json updated on R2.")


if __name__ == "__main__":
    main()
