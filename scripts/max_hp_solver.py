"""Faithful Python subset of MaxHpSolver.cpp for offline analytics."""

from __future__ import annotations

import gzip
import math
import re
from dataclasses import dataclass
from pathlib import Path

M_MIN, M_MAX = 380, 750
MIN_FRAC, MAX_FRAC = 0.003, 1.0
RESID_THRESH, MIN_EVENTS, ACCEPT_MEDIAN = 0.03, 4, 0.02
PACKET_TOL = 0.0015
CORRECTION_OFFSETS = (-30, 48, 30, -72, 100, -60, 60, 78, 10, 18,
                      -102, 70, -12, -62)


def cpp_round(value: float) -> int:
    """C++ round/lround semantics for the non-negative values used here."""
    return math.floor(value + 0.5)


def residual(fraction: float, max_hp: int) -> float:
    value = abs(fraction) * max_hp
    return abs(value - cpp_round(value))


def is_constructed_fraction(fraction: float) -> bool:
    value = abs(fraction)
    for denominator in (3, 4, 5, 6, 10, 20):
        scaled = value * denominator
        if abs(scaled - cpp_round(scaled)) < 1e-3 and cpp_round(scaled) >= 1:
            return True
    return False


def correct_max_hp_for_packet(recorded: int, fraction: float) -> int:
    if recorded <= 0 or not 0 < abs(fraction) <= 1:
        return recorded
    value = abs(fraction)

    def integral(candidate: int) -> bool:
        scaled = value * candidate
        return candidate > 0 and scaled >= 0.5 and abs(scaled - cpp_round(scaled)) <= PACKET_TOL

    if integral(recorded):
        return recorded
    for offset in CORRECTION_OFFSETS:
        candidate = recorded + offset
        if integral(candidate):
            return candidate
    return recorded


@dataclass(frozen=True)
class Snapshot:
    time: float
    health_pct: float
    max_hp: int
    has_deep_wound: bool
    weapon_item_type: int
    offhand_item_type: int
    weapon_item_id: int
    offhand_item_id: int

    @property
    def weapon_set_key(self) -> int:
        return ((self.weapon_item_id << 32) | (self.offhand_item_id << 16) |
                (self.weapon_item_type << 8) | self.offhand_item_type)


@dataclass(frozen=True)
class Observation:
    time: float
    fraction: float
    weapon_set_key: int
    camera_max_hp: int = 0
    has_deep_wound: bool = False


@dataclass(frozen=True)
class SolvedMaxHp:
    max_hp: int
    observations: int
    supporting: int
    median_residual: float
    accepted: bool
    first_seen: float
    last_seen: float
    source: str = "lattice"


def parse_timestamp(text: str) -> float | None:
    close = text.find("]")
    if not text.startswith("[") or close < 0:
        return None
    body = text[1:close]
    if ":" not in body:
        return None
    minute, rest = body.split(":", 1)
    second, millis = rest.split(".", 1) if "." in rest else (rest, "0")
    total = (_signed_prefix(minute) * 60 + _signed_prefix(second) +
             _unsigned_prefix(millis) / 1000.0)
    return total if total >= 0 else None


_UNSIGNED = re.compile(r"\d+")
_SIGNED = re.compile(r"[+-]?\d+")
_FLOAT = re.compile(r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?")


def _unsigned_prefix(value: str, bits: int = 32) -> int:
    match = _UNSIGNED.match(value)
    if match is None:
        return 0
    parsed = int(match.group())
    return parsed if parsed < (1 << bits) else 0


def _signed_prefix(value: str) -> int:
    match = _SIGNED.match(value)
    return int(match.group()) if match is not None else 0


def _number(value: str) -> float:
    match = _FLOAT.match(value)
    return float(match.group()) if match is not None else 0.0


def _boolean(value: str) -> bool:
    return bool(value) and value[0] != "0"


def parse_snapshot_line(line: str) -> Snapshot | None:
    time = parse_timestamp(line)
    close = line.find("]")
    if time is None or close < 0:
        return None
    data = line[close + 1:]
    data = data.lstrip(" \t")
    fields = data.rstrip("\r\n").split(";")
    if len(fields) < 10:
        return None
    # AgentSnapshotParser.cpp positions; later fields are optional.
    def field(index: int, default: str = "0") -> str:
        return fields[index] if index < len(fields) else default
    return Snapshot(time, _number(field(9)), _unsigned_prefix(field(11)),
                    _boolean(field(13)), _unsigned_prefix(field(25), 8),
                    _unsigned_prefix(field(26), 8),
                    _unsigned_prefix(field(27), 16),
                    _unsigned_prefix(field(28), 16))


def read_snapshots(path: Path) -> tuple[Snapshot, ...]:
    opener = gzip.open if path.suffix.lower() == ".gz" else open
    with opener(path, "rt", encoding="utf-8-sig", errors="replace") as handle:
        return tuple(snapshot for line in handle
                     if (snapshot := parse_snapshot_line(line)) is not None)


def snapshot_index_at_time(snapshots: tuple[Snapshot, ...], time: float) -> int:
    if not snapshots:
        return -1
    lo, hi = 0, len(snapshots) - 1
    if time <= snapshots[0].time:
        return 0
    if time >= snapshots[-1].time:
        return len(snapshots) - 1
    while lo < hi:
        mid = lo + (hi - lo + 1) // 2
        if snapshots[mid].time <= time:
            lo = mid
        else:
            hi = mid - 1
    return lo


def snapshot_at(snapshots: tuple[Snapshot, ...], time: float) -> Snapshot | None:
    index = snapshot_index_at_time(snapshots, time)
    return snapshots[index] if index >= 0 else None


def max_hp_at_time(snapshots: tuple[Snapshot, ...], time: float) -> int:
    index = snapshot_index_at_time(snapshots, time)
    if index < 0:
        return 0
    if snapshots[index].max_hp > 0:
        return snapshots[index].max_hp
    for snapshot in snapshots[index + 1:]:
        if snapshot.max_hp > 0:
            return snapshot.max_hp
    for snapshot in reversed(snapshots[:index]):
        if snapshot.max_hp > 0:
            return snapshot.max_hp
    return 0


def solve_observations(observations: list[Observation]) -> dict[int, SolvedMaxHp]:
    buckets: dict[int, list[Observation]] = {}
    for observation in observations:
        fraction = abs(observation.fraction)
        if (observation.has_deep_wound or fraction < MIN_FRAC or
                fraction > MAX_FRAC or is_constructed_fraction(fraction)):
            continue
        buckets.setdefault(observation.weapon_set_key, []).append(observation)

    solved: dict[int, SolvedMaxHp] = {}
    for key, hits in buckets.items():
        if len(hits) < MIN_EVENTS:
            continue
        hits.sort(key=lambda hit: hit.time)
        candidates: list[tuple[int, float, int]] = []
        for max_hp in range(M_MIN, M_MAX + 1):
            values = [residual(hit.fraction, max_hp) for hit in hits]
            viable = [value for value in values if value <= RESID_THRESH]
            candidates.append((len(viable), sum(value * value for value in viable), max_hp))
        best_count = max(item[0] for item in candidates)
        if best_count < MIN_EVENTS:
            continue
        camera = hits[len(hits) // 2].camera_max_hp
        finalists = [item for item in candidates if item[0] == best_count]
        # C++ first selects the lowest residual score, then deliberately lets
        # the camera reading break the entire equal-support tie.
        if camera:
            best_hp = min((item[2] for item in finalists),
                          key=lambda value: abs(value - camera))
        else:
            best_hp = min(finalists, key=lambda item: item[1])[2]
        values = sorted(residual(hit.fraction, best_hp) for hit in hits)
        middle = len(values) // 2
        median = values[middle] if len(values) % 2 else (values[middle - 1] + values[middle]) / 2
        solved[key] = SolvedMaxHp(best_hp, len(hits), best_count, median,
                                  median <= ACCEPT_MEDIAN, hits[0].time, hits[-1].time)
    return solved
