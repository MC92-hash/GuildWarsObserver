#!/usr/bin/env python3
"""
Build the GW Observer distributable zip.

    py -3.11 scripts/package_release.py --version 2.0.0 \
        --exe x64/ReleaseShip/GuildWarsObserver.exe \
        --notes dist/release_notes_2.0.0.md

The zip MUST be flat (GuildWarsObserver.exe at the root): the in-app updater
installs it with `tar -xf <zip> -C <exeDir>` over the live install directory
(SourceFiles/Net/UpdateChecker.cpp). A wrapper folder extracts without error,
never replaces the running exe, and silently relaunches the old version.

See shiprelease.md for the full procedure.
"""

import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Files the app writes at runtime. Shipping any of these means an auto-update
# extracts over the user's own state: gui_settings.ini carries gw_dat_path and
# match_data_folder, settings/ui_layout.json carries the ribbon/panel layout.
# Users would land back in the setup wizard after every update.
USER_STATE = {
    "gui_settings.ini",
    "imgui_layout.ini",
    "ui_layout.json",
    "notes.json",
    "ratings.json",
    "bookmarks.json",
    "animation_cache.ini",
    "animation_clips_cache.bin",
    "replay.json",
}

# Build byproducts and local debris that must never reach a user.
EXCLUDED_SUFFIXES = (".pdb", ".lib", ".exp", ".ilk", ".iobj", ".ipdb", ".log", ".dmp", ".cso")
EXCLUDED_DIRS = {"MatchCache", "UserData", "__pycache__", ".git"}


def skip(path: Path) -> bool:
    if path.name in USER_STATE:
        return True
    if path.suffix.lower() in EXCLUDED_SUFFIXES:
        return True
    if ".bak-" in path.name:
        return True
    return False


def copy_tree(src: Path, dst: Path) -> int:
    """Copy src into dst, applying the exclude rules. Returns files copied."""
    count = 0
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in EXCLUDED_DIRS]
        rel = Path(root).relative_to(src)
        for name in files:
            source = Path(root) / name
            if skip(source):
                continue
            target = dst / rel / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            count += 1
    return count


def copy_file(src: Path, dst: Path) -> None:
    if not src.exists():
        sys.exit(f"error: required file missing: {src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help='e.g. "2.0.0" (no leading v)')
    ap.add_argument("--exe", default="x64/Release/GuildWarsObserver.exe",
                    help="Release x64 build to ship")
    ap.add_argument("--notes", help="markdown file copied in as RELEASE_NOTES.md")
    ap.add_argument("--output", default="dist/GWObserver.zip",
                    help="the asset name must match what the website links to")
    ap.add_argument("--keep-stage", action="store_true")
    args = ap.parse_args()

    exe = (REPO / args.exe).resolve()
    stage = REPO / "dist" / f"stage-{args.version}"
    out = (REPO / args.output).resolve()

    if not exe.exists():
        sys.exit(f"error: exe not found: {exe}")

    # The shipped binary must actually be the version being tagged. GWO_VERSION
    # is a plain string literal, so it survives into the binary verbatim.
    if args.version.encode() not in exe.read_bytes():
        sys.exit(f"error: {exe.name} does not contain the string {args.version!r}. "
                 f"Bump GWO_VERSION in SourceFiles/build_config.h and rebuild.")

    print(f"staging {args.version} -> {stage}")
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    top_level = []

    copy_file(exe, stage / "GuildWarsObserver.exe")
    top_level.append("GuildWarsObserver.exe")

    # Also embedded as resources IDR_DLL1/IDR_DLL2 and self-extracted on first
    # run, but shipping them avoids a write into a read-only install dir.
    for dll in ("bass.dll", "bass_fx.dll"):
        copy_file(REPO / "SourceFiles" / dll, stage / dll)
        top_level.append(dll)

    n = copy_tree(REPO / "Data", stage / "Data")
    print(f"  Data/                     {n} files")
    top_level.append("Data")

    # settings/ has two sources that must be merged: the curated JSONs at the
    # repo root, and the per-map asset blacklists that live under the build dir.
    n = copy_tree(REPO / "settings", stage / "settings")
    n += copy_tree(REPO / "x64" / "Release" / "settings", stage / "settings")
    print(f"  settings/                 {n} files (merged from 2 sources)")
    top_level.append("settings")

    n = copy_tree(REPO / "Textures", stage / "Textures")
    print(f"  Textures/                 {n} files")
    top_level.append("Textures")

    for doc in ("LICENCE.md", "NOTICE.md"):
        copy_file(REPO / doc, stage / doc)
        top_level.append(doc)

    if args.notes:
        copy_file(REPO / args.notes, stage / "RELEASE_NOTES.md")
        top_level.append("RELEASE_NOTES.md")

    # Windows tar.exe, listing top-level names explicitly. Do NOT use `-C stage .`
    # (prefixes every entry with "./", which Explorer refuses to open) and do NOT
    # use .NET ZipFile.CreateFromDirectory under PowerShell 5.1 (writes backslash
    # separators, violating the zip spec and breaking Explorer).
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    print(f"zipping -> {out}")
    subprocess.run(
        ["tar.exe", "-a", "-c", "-f", str(out), "-C", str(stage), *top_level],
        check=True,
    )

    verify(out, args.version)

    if not args.keep_stage:
        shutil.rmtree(stage)

    print(f"\nOK  {out}  ({out.stat().st_size / 1_000_000:.1f} MB)")


def verify(zip_path: Path, version: str) -> None:
    print("verifying...")
    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
        roots = sorted({n.split("/")[0] for n in names})

        if "GuildWarsObserver.exe" not in names:
            sys.exit("FAIL: GuildWarsObserver.exe is not a top-level entry. "
                     "The updater extracts over the install dir; a nested folder "
                     "means the running exe is never replaced.")

        for n in names:
            if n.startswith("./") or "\\" in n:
                sys.exit(f"FAIL: malformed entry name {n!r}")
            leaf = n.rsplit("/", 1)[-1]
            if leaf in USER_STATE:
                sys.exit(f"FAIL: {n} is user state and would be overwritten on update")

        print(f"  top level: {', '.join(roots)}")
        print(f"  entries:   {len(names)}")
        print(f"  exe:       {version}")


if __name__ == "__main__":
    main()
