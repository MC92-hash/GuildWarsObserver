#!/usr/bin/env python3
"""
Watch the in-app updater carry a real v1.2.7 install up to the staged release.

    py -3.11 scripts/demo_update.py

Everything the updater does that decides success or failure happens in a hidden
console window: the batch script is written, runs, and deletes itself, leaving
no trace either way. That is why the silent failures in it went unnoticed for so
long. This stages a genuine old install, launches it, and then narrates every
change to the install directory as it happens - including grabbing the batch
script before it erases itself - so the whole thing is visible for once.

The update is real throughout: real GitHub API call, real download over the real
redirect, real tar extraction over a real install, real relaunch.

You click three times in the middle. The script says when.

See shiprelease.md, "Testing the updater".
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import winreg
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
EXE_NAME = "GuildWarsObserver.exe"
ARCHIVE_NAME = "GWObserver_update.zip"
BAT_NAME = "_gwobs_update.bat"
ERROR_LOG = "_gwobs_update_error.log"

# Markers that prove extraction is not destroying files the user owns. Both have
# to be state the app keeps NEXT TO THE EXE.
#
# gui_settings.ini's match_data_folder looked like the obvious marker and is not:
# SetupConfig stores it machine-wide in %APPDATA%\GWObserver\config.ini, shared by
# every install on the box (SetupConfig.cpp:15), so the app legitimately rewrites
# the local copy from there and the check fails for a reason that has nothing to
# do with the package.
MARKER_LAYOUT_KEY = "gwo_demo_marker"
MARKER_FILE = "USER_FILE_DO_NOT_TOUCH.txt"
MARKER_FILE_TEXT = "staged by demo_update.py; tar must not remove or alter this"

TAR = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "tar.exe"


# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_zip_entry(zip_path: Path, entry: str) -> str:
    with zipfile.ZipFile(zip_path) as z:
        return hashlib.sha256(z.read(entry)).hexdigest()


def mb(n: float) -> str:
    return f"{n / 1_000_000:.1f} MB"


def running_pids(image: str = EXE_NAME) -> set:
    """PIDs of a running image, via tasklist. Fast enough to poll twice a second."""
    # Captured as bytes and decoded permissively on purpose: tasklist emits the
    # console OEM codepage, which is not decodable as the Python default here and
    # throws inside subprocess's reader thread rather than somewhere catchable.
    try:
        raw = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {image}", "/FO", "CSV", "/NH"],
            capture_output=True, timeout=10,
        ).stdout or b""
    except Exception:
        return set()
    out = raw.decode("utf-8", errors="replace")
    pids = set()
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower() == image.lower():
            try:
                pids.add(int(parts[1]))
            except ValueError:
                pass
    return pids


def desktop_path() -> str:
    """The PATH a process gets when started from Explorer, read from the registry
    rather than inherited.

    This matters more than it looks. Pre-2.0.0 clients invoke `tar` unqualified,
    so which binary runs is decided by the PATH the app inherited. Launching this
    script from Git Bash, whose PATH puts /usr/bin ahead of system32, hands the
    app GNU tar - which cannot read a zip and parses "C:\\..." as a remote host,
    failing instantly. The first rehearsal did exactly that and reported a bug
    that no ordinary user would ever hit. Read the real machine and user PATH so
    the run reflects a double-click.
    """
    parts = []
    for hive, key in ((winreg.HKEY_LOCAL_MACHINE,
                       r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"),
                      (winreg.HKEY_CURRENT_USER, "Environment")):
        try:
            with winreg.OpenKey(hive, key) as k:
                value, _ = winreg.QueryValueEx(k, "Path")
                parts.append(os.path.expandvars(value))
        except OSError:
            pass
    return ";".join(p for p in parts if p)


def child_env(shadow_tar: str = "") -> dict:
    env = dict(os.environ)
    path = desktop_path() or env.get("PATH", "")
    if shadow_tar:
        path = shadow_tar + ";" + path
    env["PATH"] = path
    return env


def resolve_tar(env: dict) -> str:
    """Which tar.exe the batch script would actually get under this environment."""
    for d in env.get("PATH", "").split(";"):
        if not d:
            continue
        cand = Path(d) / "tar.exe"
        try:
            if cand.is_file():
                return str(cand)
        except OSError:
            continue
    return "<not found on PATH>"


def tree_manifest(root: Path) -> dict:
    out = {}
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            p = Path(dirpath) / name
            try:
                st = p.stat()
            except OSError:
                continue
            out[str(p.relative_to(root))] = (st.st_size, int(st.st_mtime))
    return out


class Log:
    def __init__(self):
        self.t0 = time.time()

    def __call__(self, kind: str, msg: str) -> None:
        print(f"{time.time() - self.t0:6.1f}  {kind:<10} {msg}", flush=True)


# --------------------------------------------------------------------------
# staging
# --------------------------------------------------------------------------

def operator_dat_path() -> str:
    """The real gw_dat_path from this machine's own install, so the staged copy
    starts normally instead of dropping into the setup wizard."""
    for candidate in (REPO / "x64" / "Release" / "gui_settings.ini",
                      REPO / "x64" / "ReleaseShip" / "gui_settings.ini"):
        if candidate.exists():
            for line in candidate.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith("gw_dat_path="):
                    return line.split("=", 1)[1].strip()
    return ""


def remote_asset_size(tag: str):
    """Size of the .zip asset GitHub is serving for a tag, or None."""
    try:
        raw = subprocess.run(
            ["gh", "api", f"repos/MC92-hash/GuildWarsObserver/releases/tags/{tag}",
             "--jq", '[.assets[]|select(.name|endswith(".zip"))]|last|.size'],
            capture_output=True, timeout=60,
        ).stdout or b""
        return int(raw.decode("utf-8", errors="replace").strip())
    except Exception:
        return None


def fetch_release(tag: str, cache: Path, log: Log) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    dest = cache / f"GWObserver-{tag}.zip"

    expected = remote_asset_size(tag)

    if dest.exists() and (expected is None or dest.stat().st_size == expected):
        log("cache", f"{dest.name} already downloaded ({mb(dest.stat().st_size)})")
        return dest

    log("download", f"fetching {tag} from GitHub, this is a one-time ~100 MB pull")
    tmp = cache / f"_dl_{tag}"
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir()
    subprocess.run(
        ["gh", "release", "download", tag, "--repo", "MC92-hash/GuildWarsObserver",
         "--pattern", "*.zip", "-D", str(tmp)],
        check=True,
    )
    got = next(tmp.glob("*.zip"))
    if dest.exists():
        dest.unlink()
    shutil.move(str(got), str(dest))
    shutil.rmtree(tmp, ignore_errors=True)

    if expected is not None and dest.stat().st_size != expected:
        sys.exit(f"FAIL: downloaded {tag} is {dest.stat().st_size} bytes, API says {expected}")

    log("download", f"{dest.name} ({mb(dest.stat().st_size)})")
    return dest


def stage(install: Path, source_zip: Path, client_exe: Path, log: Log) -> dict:
    if install.exists():
        shutil.rmtree(install)
    install.mkdir(parents=True)

    subprocess.run([str(TAR), "-xf", str(source_zip), "-C", str(install)], check=True)

    staged_exe = install / EXE_NAME
    if not staged_exe.exists():
        sys.exit(f"FAIL: {source_zip.name} has no {EXE_NAME} at its root")

    shutil.copy2(client_exe, staged_exe)

    dat = operator_dat_path()
    (install / "gui_settings.ini").write_text(
        "[WindowVisibility]\n"
        "replay_browser=1\n"
        "\n[Config]\n"
        f"gw_dat_path={dat}\n",
        encoding="utf-8",
    )

    (install / MARKER_FILE).write_text(MARKER_FILE_TEXT, encoding="utf-8")

    (install / "settings").mkdir(exist_ok=True)
    (install / "settings" / "ui_layout.json").write_text(
        json.dumps({MARKER_LAYOUT_KEY: "must survive the update", "ribbonPinned": True},
                   indent=2),
        encoding="utf-8",
    )

    log("staged", f"{install}")
    log("staged", f"client = {client_exe.name} from {client_exe.parent.name}/  "
                  f"(reports 1.2.7)")
    log("staged", f"markers = {MARKER_FILE} + ui_layout.json:{MARKER_LAYOUT_KEY}")
    if not dat:
        log("warn", "no gw_dat_path found on this machine; the setup wizard may "
                    "appear in front of the update card")

    return tree_manifest(install)


# --------------------------------------------------------------------------
# watching
# --------------------------------------------------------------------------

def watch(install: Path, workdir: Path, log: Log, timeout: float, env: dict) -> dict:
    exe = install / EXE_NAME
    archive = install / ARCHIVE_NAME
    partial = install / (ARCHIVE_NAME + ".tmp")
    bat = install / BAT_NAME
    errlog = install / ERROR_LOG
    captured = workdir / "captured_update.bat"

    exe_before = sha256_file(exe)
    size_before = exe.stat().st_size
    baseline_pids = running_pids()

    proc = subprocess.Popen([str(exe)], cwd=str(install), env=env)
    log("launch", f"{EXE_NAME}  pid {proc.pid}")
    log("env", f"tar the batch will get -> {resolve_tar(env)}")
    print()
    print("    >> Click 'Download & Install' on the update card when it appears.")
    print("    >> Keep the window focused until it closes.")
    print()

    seen = {"partial": False, "archive": False, "bat": False, "exit": False,
            "swap": False, "bat_gone": False, "archive_gone": False,
            "relaunch": False, "errlog": False}
    last_report = 0.0
    relaunch_pid = None
    deadline = time.time() + timeout

    while time.time() < deadline:
        time.sleep(0.25)

        # download, which lands as .tmp first and is renamed on completion
        if partial.exists():
            sz = partial.stat().st_size
            if not seen["partial"]:
                seen["partial"] = True
                log("download", f"{partial.name} appeared")
            if sz - last_report > 10_000_000:
                last_report = sz
                log("download", mb(sz))
        if archive.exists() and not seen["archive"]:
            seen["archive"] = True
            log("download", f"complete, {mb(archive.stat().st_size)} -> {archive.name}")

        # the batch deletes itself as its last act, so grab it the moment it exists
        if bat.exists() and not seen["bat"]:
            seen["bat"] = True
            try:
                shutil.copy2(bat, captured)
                log("batch", f"{BAT_NAME} written -> captured to {captured.name}")
            except OSError as e:
                log("batch", f"{BAT_NAME} written, could not capture ({e})")

        if not seen["exit"] and proc.poll() is not None:
            seen["exit"] = True
            log("app exit", f"pid {proc.pid} gone")

        if not seen["swap"]:
            try:
                if exe.stat().st_size != size_before or sha256_file(exe) != exe_before:
                    seen["swap"] = True
                    log("extract", f"{EXE_NAME} replaced ({exe.stat().st_size:,} bytes)")
            except OSError:
                pass  # briefly locked mid-extract

        if seen["bat"] and not bat.exists() and not seen["bat_gone"]:
            seen["bat_gone"] = True
            log("cleanup", f"{BAT_NAME} self-deleted")
        if seen["archive"] and not archive.exists() and not seen["archive_gone"]:
            seen["archive_gone"] = True
            log("cleanup", f"{ARCHIVE_NAME} removed")

        if errlog.exists() and not seen["errlog"]:
            seen["errlog"] = True
            log("ERROR", f"{ERROR_LOG} written: "
                         f"{errlog.read_text(errors='replace').strip()}")

        if seen["exit"] and not seen["relaunch"]:
            fresh = running_pids() - baseline_pids - {proc.pid}
            if fresh:
                relaunch_pid = sorted(fresh)[0]
                seen["relaunch"] = True
                log("relaunch", f"pid {relaunch_pid}")

        if seen["swap"] and seen["relaunch"]:
            time.sleep(2.0)  # let cleanup finish
            break

        # The silent no-op: the archive is gone and the app is back, but the exe
        # is untouched. That is the whole answer, so stop rather than sitting here
        # until the timeout looking like the test is still in progress.
        if seen["relaunch"] and seen["archive_gone"] and not seen["swap"]:
            log("NO-OP", "archive consumed and app relaunched, but the exe never "
                         "changed - extraction did nothing")
            break
    else:
        log("timeout", f"gave up after {timeout:.0f}s")

    return {"seen": seen, "relaunch_pid": relaunch_pid, "captured": captured,
            "exe_before": exe_before, "tar": resolve_tar(env)}


# --------------------------------------------------------------------------
# verifying
# --------------------------------------------------------------------------

def verify(install: Path, before: dict, state: dict, expect_zip: Path, log: Log) -> bool:
    print()
    print("-" * 66)
    print("VERIFICATION")
    print("-" * 66)

    checks = []

    expected_hash = sha256_zip_entry(expect_zip, EXE_NAME)
    actual_hash = sha256_file(install / EXE_NAME)
    checks.append((
        "exe replaced with the packaged build",
        actual_hash == expected_hash,
        f"{actual_hash[:16]} vs packaged {expected_hash[:16]}",
    ))

    checks.append((
        "app relaunched itself",
        state["seen"]["relaunch"],
        f"pid {state['relaunch_pid']}" if state["relaunch_pid"] else "no new process seen",
    ))

    marker = install / MARKER_FILE
    kept = marker.exists() and marker.read_text(errors="replace").strip() == MARKER_FILE_TEXT
    checks.append((
        "user file untouched by extraction",
        kept,
        f"{MARKER_FILE} intact" if kept else f"{MARKER_FILE} removed or altered",
    ))

    with zipfile.ZipFile(expect_zip) as z:
        leaked = [n for n in z.namelist()
                  if n.rsplit("/", 1)[-1] in ("gui_settings.ini", "ui_layout.json")]
    checks.append((
        "package ships no user state",
        not leaked,
        "no gui_settings.ini or ui_layout.json in the zip" if not leaked else str(leaked),
    ))

    layout = install / "settings" / "ui_layout.json"
    layout_text = layout.read_text(encoding="utf-8", errors="replace") if layout.exists() else ""
    checks.append((
        "settings/ui_layout.json survived",
        MARKER_LAYOUT_KEY in layout_text,
        "marker intact" if MARKER_LAYOUT_KEY in layout_text else "MARKER GONE",
    ))

    nested = [p.name for p in install.iterdir()
              if p.is_dir() and p.name.lower().startswith("guildwarsobserver")]
    checks.append((
        "no nested folder",
        not nested,
        "flat" if not nested else f"found {nested} - update was a silent no-op",
    ))

    leftovers = [n for n in (ARCHIVE_NAME, BAT_NAME, ERROR_LOG) if (install / n).exists()]
    checks.append((
        "install directory clean",
        not leftovers,
        "no leftovers" if not leftovers else f"left behind: {leftovers}",
    ))

    for rel in ("Data/skilldata.json", "settings/skill_sounds.json"):
        p = install / rel
        ok = p.exists() and p.stat().st_size > 0
        checks.append((f"{rel} landed", ok,
                       f"{p.stat().st_size:,} bytes" if ok else "missing or empty"))

    for name, ok, detail in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}]  {name:<42} {detail}")

    print()
    print(f"  tar the batch resolved to: {state.get('tar', '?')}")
    if not state["seen"]["swap"]:
        print("  Nothing was extracted. Pre-2.0.0 clients call tar unqualified, so this")
        print("  is the first thing to check: GNU tar cannot read a zip and treats a")
        print("  drive-letter path as a remote host, failing instantly and silently.")

    after = tree_manifest(install)
    added = sorted(set(after) - set(before))
    changed = sorted(k for k in set(after) & set(before) if after[k] != before[k])
    untouched = len(set(after) & set(before)) - len(changed)
    print()
    print(f"  tree: {len(changed)} replaced, {len(added)} added, {untouched} untouched")
    print("        (tar never deletes, so anything dropped from the package lingers)")
    for k in added[:6]:
        print(f"        + {k}")
    if len(added) > 6:
        print(f"        + ... and {len(added) - 6} more")

    cap = state["captured"]
    if cap.exists():
        print()
        print(f"  the batch that ran ({cap}):")
        for line in cap.read_text(errors="replace").splitlines():
            print(f"      {line}")

    ok = all(c[1] for c in checks)
    print()
    print("  RESULT:", "PASS - the updater works end to end" if ok else "FAIL")
    return ok


# --------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--from-tag", default="v1.2.7",
                    help="release to stage the old install from")
    ap.add_argument("--client", default=str(REPO / "dist" / "prober" / EXE_NAME),
                    help="exe to use as the old client; defaults to the 1.2.7-equivalent prober")
    ap.add_argument("--expect", default=str(REPO / "dist" / "GWObserver.zip"),
                    help="the packaged zip the install should end up matching")
    ap.add_argument("--to-tag", default="v2.0.0",
                    help="the release the client is expected to update TO")
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--shadow-tar", nargs="?", const=r"C:\Program Files\Git\usr\bin",
                    default="",
                    help="prepend a directory holding a non-bsdtar tar.exe to the "
                         "child PATH, reproducing the machines where a pre-2.0.0 "
                         "client silently fails to extract")
    ap.add_argument("--keep", action="store_true",
                    help="do not wipe the staged install from a previous run")
    args = ap.parse_args()

    if os.name != "nt":
        sys.exit("This only means anything on Windows.")

    workdir = Path(os.environ["LOCALAPPDATA"]) / "GWObserver" / "update-demo"
    install = workdir / "install"
    cache = workdir / "cache"

    client = Path(args.client)
    if not client.exists():
        sys.exit(f"FAIL: no client exe at {client}\n"
                 f"      dist/prober/ holds the 1.2.7-equivalent build; see shiprelease.md")

    expect_zip = Path(args.expect)
    if not expect_zip.exists():
        sys.exit(f"FAIL: no packaged zip at {expect_zip}. Run scripts/package_release.py first.")

    log = Log()
    print("=" * 66)
    print("GW Observer - live update rehearsal")
    print("=" * 66)

    # The client downloads whatever GitHub is serving, so if the asset up there is
    # not the zip we are checking against, say so now rather than failing the hash
    # check ten minutes later. Almost always means the upload step was skipped.
    remote = remote_asset_size(args.to_tag)
    local = expect_zip.stat().st_size
    if remote is None:
        log("warn", f"could not read the {args.to_tag} asset size from GitHub")
    elif remote != local:
        print()
        print(f"  !! The {args.to_tag} asset on GitHub is {remote:,} bytes but")
        print(f"     {expect_zip} is {local:,} bytes.")
        print(f"     The demo will install what GitHub is serving, so the final")
        print(f"     hash check will fail. Upload the current package first:")
        print()
        print(f"       gh release upload {args.to_tag} {expect_zip} "
              f"--repo MC92-hash/GuildWarsObserver --clobber")
        print()
        try:
            answer = input("  Continue anyway? [y/N] ").strip().lower()
        except EOFError:
            answer = "n"
        if answer != "y":
            sys.exit(1)
    else:
        log("preflight", f"{args.to_tag} asset matches the local package ({local:,} bytes)")

    env = child_env(args.shadow_tar)
    if args.shadow_tar:
        log("env", f"shadowing PATH with {args.shadow_tar} on purpose")
    source = fetch_release(args.from_tag, cache, log)
    before = stage(install, source, client, log) if not args.keep else tree_manifest(install)
    state = watch(install, workdir, log, args.timeout, env)
    ok = verify(install, before, state, expect_zip, log)

    if state["relaunch_pid"]:
        print(f"\n  The relaunched app is still running as pid {state['relaunch_pid']}. "
              f"Close it when you are done looking.")

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
