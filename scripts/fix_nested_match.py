"""Fix incorrectly nested match directories in the MatchCache.

When a .tar.gz archive with non-ASCII characters in the filename is extracted,
the inner folder name may differ from the outer folder name due to Unicode
encoding mismatches. This leaves infos.json one level too deep, preventing the
match from loading.

Usage:
    python fix_nested_match.py [MatchCache_path]

If no path is given, defaults to ./MatchCache in the current directory.
"""

import os
import shutil
import sys


def fix_cache(cache_dir):
    if not os.path.isdir(cache_dir):
        print(f"Directory not found: {cache_dir}")
        return

    fixed = 0
    for name in os.listdir(cache_dir):
        folder = os.path.join(cache_dir, name)
        if not os.path.isdir(folder):
            continue

        # Skip if infos.json is already at the top level (healthy match)
        if os.path.isfile(os.path.join(folder, "infos.json")):
            continue

        # Look for a single subdirectory that contains infos.json
        subdirs = [
            d for d in os.listdir(folder)
            if os.path.isdir(os.path.join(folder, d))
        ]
        if len(subdirs) != 1:
            continue

        inner = os.path.join(folder, subdirs[0])
        if not os.path.isfile(os.path.join(inner, "infos.json")):
            continue

        print(f"Fixing: {name}")
        for item in os.listdir(inner):
            src = os.path.join(inner, item)
            dst = os.path.join(folder, item)
            shutil.move(src, dst)
        os.rmdir(inner)
        fixed += 1

    if fixed:
        print(f"\nFixed {fixed} match(es). They should now load in the observer.")
    else:
        print("No broken matches found.")


if __name__ == "__main__":
    cache_path = sys.argv[1] if len(sys.argv) > 1 else "MatchCache"
    fix_cache(cache_path)
