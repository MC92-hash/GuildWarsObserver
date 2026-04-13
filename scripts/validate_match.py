#!/usr/bin/env python3
"""
Validate GvG match recordings for position data corruption.

Detects the "jitter" corruption pattern where the GWToolbox network surface
glitches during recording, causing player positions to oscillate between
incorrect coordinates on nearly every tick.

Detection metric: median p95 step distance across all player agents.
  - Clean matches: 28-44 game units (normal GW1 movement per ~100ms tick)
  - Corrupt matches: 56+ game units (constant medium-range position jitter)

Usage:
    python validate_match.py <match_folder>             # validate one match
    python validate_match.py <match_folder> --verbose    # show per-agent details
    python validate_match.py --scan-dir <directory>      # batch scan all matches
"""

import argparse
import gzip
import json
import math
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass
class ValidationResult:
    passed: bool
    match_folder: str
    total_players: int
    players_with_jumps: int
    total_jumps_200: int
    total_snapshots: int
    corruption_ratio: float  # total_jumps_200 / total_snapshots
    median_p95: float  # median of per-agent p95 step distances
    duration_minutes: float
    reason: str


def parse_timestamp(ts_str: str) -> float:
    """Parse '[MM:SS.mmm]' or '[MM:SS]' -> seconds as float.

    Port of C++ ParseTimestamp from AgentSnapshotParser.cpp:82-110.
    """
    if not ts_str.startswith("[") or "]" not in ts_str:
        return -1.0
    inner = ts_str[1 : ts_str.index("]")]
    if ":" not in inner:
        return -1.0
    parts = inner.split(":")
    minutes = int(parts[0])
    sec_part = parts[1]
    if "." in sec_part:
        sec, ms = sec_part.split(".")
        return minutes * 60 + int(sec) + int(ms) / 1000.0
    return minutes * 60 + int(sec_part)


def parse_agent_file(file_path: Path) -> list[tuple[float, float, float, int]]:
    """Parse an agent file, returning list of (time, x, y, agent_model_type).

    Handles both .txt and .txt.gz files. Only extracts the fields needed
    for validation: timestamp, x (field 0), y (field 1), agent_model_type
    (field 44, zero-indexed).
    """
    results = []
    try:
        opener = gzip.open if file_path.suffix == ".gz" else open
        with opener(file_path, "rt", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or not line.startswith("["):
                    continue
                bracket_end = line.index("]")
                time_s = parse_timestamp(line[: bracket_end + 1])
                if time_s < 0:
                    continue
                data = line[bracket_end + 1 :].strip()
                fields = data.split(";")
                if len(fields) < 10:
                    continue
                try:
                    x = float(fields[0])
                    y = float(fields[1])
                    amt = int(fields[44]) if len(fields) > 44 else 0
                except (ValueError, IndexError):
                    continue
                results.append((time_s, x, y, amt))
    except Exception:
        pass
    return results


def enumerate_agent_files(agents_dir: Path) -> list[tuple[int, Path]]:
    """Enumerate agent files, preferring .txt.gz over .txt for same agent ID.

    Mirrors C++ logic from AgentSnapshotParser.cpp:403-448.
    """
    files: dict[int, Path] = {}
    seen_gz: set[int] = set()

    for entry in agents_dir.iterdir():
        if not entry.is_file():
            continue
        name = entry.name
        if name.endswith(".txt.gz"):
            stem = name[: -len(".txt.gz")]
            is_gz = True
        elif name.endswith(".txt"):
            stem = name[: -len(".txt")]
            is_gz = False
        else:
            continue

        try:
            agent_id = int(stem)
        except ValueError:
            continue
        if agent_id <= 0:
            continue

        if is_gz:
            seen_gz.add(agent_id)
            files[agent_id] = entry
        elif agent_id not in seen_gz:
            if agent_id not in files:
                files[agent_id] = entry

    return sorted(files.items())


def validate_match(
    match_dir: Path,
    p95_threshold: float = 50.0,
    jump_distance: float = 200.0,
) -> ValidationResult:
    """Validate a match recording for position data corruption.

    Args:
        match_dir: Path to match folder containing Agents/ subdirectory.
        p95_threshold: Median p95 step distance above which the match is
            flagged as corrupt. Default 50.0 game units.
        jump_distance: Distance threshold for counting individual jumps
            (used for reporting, not for the pass/fail decision).

    Returns:
        ValidationResult with pass/fail verdict and details.
    """
    agents_dir = match_dir / "Agents"
    if not agents_dir.exists() or not agents_dir.is_dir():
        return ValidationResult(
            passed=True,
            match_folder=match_dir.name,
            total_players=0,
            players_with_jumps=0,
            total_jumps_200=0,
            total_snapshots=0,
            corruption_ratio=0.0,
            median_p95=0.0,
            duration_minutes=0.0,
            reason="No Agents/ directory found",
        )

    agent_files = enumerate_agent_files(agents_dir)

    total_players = 0
    players_with_jumps = 0
    total_jumps = 0
    total_snapshots = 0
    max_duration = 0.0
    p95_values: list[float] = []

    for agent_id, file_path in agent_files:
        positions = parse_agent_file(file_path)
        if len(positions) < 5:
            continue

        # Filter to player agents only (agent_model_type == 0x3000)
        if positions[0][3] != 0x3000:
            continue

        total_players += 1
        duration = positions[-1][0] - positions[0][0]
        max_duration = max(max_duration, duration)

        # Compute step distances
        step_dists: list[float] = []
        agent_jumps = 0
        for i in range(1, len(positions)):
            dx = positions[i][1] - positions[i - 1][1]
            dy = positions[i][2] - positions[i - 1][2]
            dist = math.hypot(dx, dy)
            step_dists.append(dist)
            if dist > jump_distance:
                agent_jumps += 1

        total_snapshots += len(step_dists)
        total_jumps += agent_jumps
        if agent_jumps > 0:
            players_with_jumps += 1

        # Compute p95 for this agent
        if step_dists:
            step_dists.sort()
            p95_idx = min(int(len(step_dists) * 0.95), len(step_dists) - 1)
            p95_values.append(step_dists[p95_idx])

    # Compute median p95 across all players
    if p95_values:
        p95_values.sort()
        median_p95 = p95_values[len(p95_values) // 2]
    else:
        median_p95 = 0.0

    duration_min = max_duration / 60.0 if max_duration > 0 else 0.0
    ratio = (total_jumps / total_snapshots * 100) if total_snapshots > 0 else 0.0

    # Decision: median p95 > threshold means corrupt
    if total_players < 2:
        passed = True
        reason = "Too few player agents to validate"
    elif median_p95 > p95_threshold:
        passed = False
        reason = (
            f"Position jitter detected: median p95 step distance = {median_p95:.1f} "
            f"(threshold: {p95_threshold:.0f}), "
            f"corruption ratio = {ratio:.2f}%, "
            f"{players_with_jumps}/{total_players} players affected"
        )
    else:
        passed = True
        reason = "OK"

    return ValidationResult(
        passed=passed,
        match_folder=match_dir.name,
        total_players=total_players,
        players_with_jumps=players_with_jumps,
        total_jumps_200=total_jumps,
        total_snapshots=total_snapshots,
        corruption_ratio=ratio,
        median_p95=median_p95,
        duration_minutes=duration_min,
        reason=reason,
    )


def main():
    parser = argparse.ArgumentParser(
        description="Validate GvG match recordings for position data corruption."
    )
    parser.add_argument(
        "match_dir",
        type=Path,
        nargs="?",
        help="Path to a match folder containing Agents/",
    )
    parser.add_argument(
        "--scan-dir",
        type=Path,
        default=None,
        help="Scan all match folders in a directory",
    )
    parser.add_argument(
        "--p95-threshold",
        type=float,
        default=50.0,
        help="Median p95 step distance threshold (default: 50.0)",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print detailed per-agent information",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output result as JSON",
    )
    args = parser.parse_args()

    if not args.match_dir and not args.scan_dir:
        parser.error("Provide either a match_dir or --scan-dir")

    if args.scan_dir:
        # Batch scan mode
        dirs = sorted(
            d for d in args.scan_dir.iterdir() if d.is_dir() and (d / "Agents").exists()
        )
        if not dirs:
            print(f"No match folders with Agents/ found in {args.scan_dir}")
            sys.exit(1)

        print(
            f"{'Match':<65} {'Plyrs':>5} {'MedP95':>7} "
            f"{'Ratio%':>7} {'Result':>8}"
        )
        print("-" * 100)
        corrupt_count = 0
        for d in dirs:
            r = validate_match(d, p95_threshold=args.p95_threshold)
            status = "CORRUPT" if not r.passed else "ok"
            if not r.passed:
                corrupt_count += 1
            print(
                f"{r.match_folder:<65} {r.total_players:>5} "
                f"{r.median_p95:>7.1f} {r.corruption_ratio:>7.2f} "
                f"{status:>8}"
            )
        print(f"\n{len(dirs)} matches scanned, {corrupt_count} corrupt.")
        sys.exit(1 if corrupt_count > 0 else 0)

    # Single match mode
    result = validate_match(args.match_dir, p95_threshold=args.p95_threshold)

    if args.json:
        print(json.dumps(asdict(result), indent=2))
    else:
        status = "FAIL" if not result.passed else "PASS"
        print(f"[{status}] {result.match_folder}")
        print(f"  Players:          {result.total_players}")
        print(f"  Median p95:       {result.median_p95:.1f} game units")
        print(f"  Corruption ratio: {result.corruption_ratio:.2f}%")
        print(f"  Jumps >200u:      {result.total_jumps_200} / {result.total_snapshots} steps")
        print(f"  Players w/jumps:  {result.players_with_jumps}/{result.total_players}")
        print(f"  Duration:         {result.duration_minutes:.1f} min")
        print(f"  Reason:           {result.reason}")

    sys.exit(0 if result.passed else 2)


if __name__ == "__main__":
    main()
