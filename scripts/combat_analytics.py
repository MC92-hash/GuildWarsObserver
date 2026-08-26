"""Derive versioned, auditable combat analytics from a recording's StoC log.

The legacy ``infos.json`` counters describe outcomes but lose attribution and
timestamps.  Newer StoC ``INTERRUPTED`` records carry all three identities we
need: victim, interrupted skill, and interrupter.  This module keeps that
direct observation separate from the cast-lifecycle join used for latency and
cancel classification.

Only facts supported by the event stream are emitted.  In particular, a
voluntary cancel is *not* labelled a fake cast: intent is not observable.
"""

from __future__ import annotations

import gzip
import re
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path


SCHEMA_VERSION = 1
INTERRUPT_MATCH_EARLY_SECONDS = 0.5
INTERRUPT_MATCH_LATE_SECONDS = 3.0

_LINE = re.compile(
    r"^\[(?P<minute>\d+):(?P<second>\d+)(?:\.(?P<millis>\d+))?\]\s*(?P<body>.*)$"
)


@dataclass(frozen=True)
class Event:
    time: float
    kind: str
    fields: tuple[str, ...]


@dataclass
class Cast:
    agent_id: int
    skill_id: int
    started_at: float
    ended_at: float | None = None
    outcome: str = "ended_other"


def _integer(value: str, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def parse_events(lines: Iterable[str]) -> list[Event]:
    """Parse the semicolon StoC records used by ``StoCParser.cpp``."""
    events: list[Event] = []
    for raw in lines:
        match = _LINE.match(raw.strip())
        if match is None:
            continue
        millis_text = match.group("millis") or "0"
        # C++ parses the digits as milliseconds rather than as a decimal
        # fraction, so ``.5`` means 5 ms and ``.050`` means 50 ms.
        timestamp = (
            int(match.group("minute")) * 60
            + int(match.group("second"))
            + int(millis_text) / 1000.0
        )
        tokens = tuple(part.strip() for part in match.group("body").split(";"))
        if tokens and tokens[0]:
            events.append(Event(timestamp, tokens[0], tokens[1:]))
    return events


def _event_ms(event: Event) -> int:
    return round(event.time * 1000)


REQUIRED_STOC_SOURCES = ("skill_events", "combat_events")
OPTIONAL_STOC_SOURCES = ("attack_skill_events",)
STOC_SOURCES = REQUIRED_STOC_SOURCES + OPTIONAL_STOC_SOURCES

BULLS_STRIKE_ID = 332
COWARD_ID = 869


def _stoc_path(match_dir: Path, stem: str) -> Path | None:
    stoc_dir = match_dir / "StoC"
    for path in (stoc_dir / f"{stem}.txt.gz", stoc_dir / f"{stem}.txt"):
        if path.is_file():
            return path
    return None


def read_stoc_events(match_dir: Path) -> tuple[list[Event], frozenset[str]]:
    events: list[Event] = []
    sources: set[str] = set()
    for stem in STOC_SOURCES:
        path = _stoc_path(match_dir, stem)
        if path is None:
            continue
        opener = gzip.open if path.suffix.lower() == ".gz" else open
        with opener(path, "rt", encoding="utf-8-sig", errors="replace") as handle:
            parsed = parse_events(handle)
        if parsed:
            events.extend(parsed)
            sources.add(stem)
    return events, frozenset(sources)


def _player_lookup(infos: dict) -> dict[int, tuple[str, int]]:
    lookup: dict[int, tuple[str, int]] = {}
    parties = infos.get("parties")
    if not isinstance(parties, dict):
        return lookup
    for party_id, party in parties.items():
        if not isinstance(party, dict):
            continue
        for player in party.get("PLAYER", ()):
            if not isinstance(player, dict):
                continue
            agent_id = _integer(player.get("id"))
            player_number = _integer(player.get("player_number"))
            if agent_id > 0 and player_number > 0:
                lookup[agent_id] = (str(party_id), player_number)
    return lookup


def build_combat_analytics(infos: dict, events: Iterable[Event]) -> dict:
    """Return a per-player v1 aggregate with reconciliation metadata.

    Cast lifecycle currently covers normal skill activations. Attack skills
    remain separate upstream and will receive their own resolution metrics in
    the blocks/conditional-KD slice.
    """
    players = _player_lookup(infos)
    rows: dict[int, dict[str, int]] = {
        agent_id: {
            "casts_started": 0,
            "casts_completed": 0,
            "casts_interrupted": 0,
            "casts_cancelled_voluntary": 0,
            "casts_ended_other": 0,
            "rupts_landed": 0,
            "rupt_cast_progress_ms_sum": 0,
            "rupt_cast_progress_n": 0,
            "knockdowns_dealt": 0,
            "knockdowns_received": 0,
            "coward_uses": 0,
            "coward_kds": 0,
            "bulls_strike_uses": 0,
            "bulls_strike_target_unknown": 0,
        }
        for agent_id in players
    }
    open_casts: dict[int, Cast] = {}
    history: dict[int, list[Cast]] = {agent_id: [] for agent_id in players}
    interrupted_total = 0
    interrupted_player_victim = 0
    interrupted_matched = 0
    interrupted_unmatched = 0
    interrupted_untracked_victim = 0
    knockdown_events = 0
    knockdown_untracked_source = 0
    knockdown_untracked_victim = 0
    kd_events: list[tuple[float, int, int]] = []

    def close_other(agent_id: int) -> None:
        cast = open_casts.pop(agent_id, None)
        if cast is not None and agent_id in rows:
            rows[agent_id]["casts_ended_other"] += 1
            history[agent_id].append(cast)

    ordered = sorted(events, key=lambda item: item.time)
    # Pass one closes every cast before interrupts are joined. The source files
    # are independent and equal timestamps are common; a single merged pass
    # would make the outcome depend on file-read order.
    for event in ordered:
        if event.kind == "SKILL_ACTIVATED" and len(event.fields) >= 2:
            skill_id = _integer(event.fields[0])
            agent_id = _integer(event.fields[1])
            if agent_id not in rows or skill_id <= 0:
                continue
            close_other(agent_id)
            open_casts[agent_id] = Cast(agent_id, skill_id, event.time)
            rows[agent_id]["casts_started"] += 1
        elif event.kind in {"SKILL_FINISHED", "SKILL_STOPPED"} and len(event.fields) >= 2:
            # Unlike ACTIVATED, the recorder writes caster before skill here.
            agent_id = _integer(event.fields[0])
            skill_id = _integer(event.fields[1])
            cast = open_casts.pop(agent_id, None)
            if cast is None or agent_id not in rows:
                continue
            if skill_id > 0 and cast.skill_id != skill_id:
                rows[agent_id]["casts_ended_other"] += 1
                history[agent_id].append(cast)
                continue
            cast.ended_at = event.time
            if event.kind == "SKILL_FINISHED":
                cast.outcome = "completed"
                rows[agent_id]["casts_completed"] += 1
            else:
                cast.outcome = "stopped"
            history[agent_id].append(cast)
        elif event.kind == "INSTANT_SKILL_USED":
            # Instant skills cannot be cancelled and would only dilute every
            # lifecycle rate, so they are intentionally outside this family.
            continue
    for agent_id in tuple(open_casts):
        close_other(agent_id)

    # Pass two mirrors ReplayWindow.cpp: the complete lifecycle is available
    # before an INTERRUPTED event searches backwards for its stopped cast.
    for event in ordered:
        if event.kind == "KNOCKED_DOWN" and len(event.fields) >= 2:
            # combat_events order is victim;source (StoCParser.cpp assigns
            # target_id from token 1 and caster_id from token 2).
            victim_id = _integer(event.fields[0])
            source_id = _integer(event.fields[1])
            knockdown_events += 1
            kd_events.append((event.time, victim_id, source_id))
            if source_id in rows:
                rows[source_id]["knockdowns_dealt"] += 1
            else:
                knockdown_untracked_source += 1
            if victim_id in rows:
                rows[victim_id]["knockdowns_received"] += 1
            else:
                knockdown_untracked_victim += 1
        elif event.kind == "INTERRUPTED" and len(event.fields) >= 3:
            victim_id = _integer(event.fields[0])
            skill_id = _integer(event.fields[1])
            interrupter_id = _integer(event.fields[2])
            interrupted_total += 1
            if interrupter_id in rows:
                rows[interrupter_id]["rupts_landed"] += 1

            if victim_id not in rows:
                interrupted_untracked_victim += 1
                continue
            interrupted_player_victim += 1
            candidates = history.get(victim_id, ())
            matched: Cast | None = None
            passed_newer_cast = False
            for cast in reversed(candidates):
                reference_time = cast.ended_at if cast.ended_at is not None else cast.started_at
                delta = event.time - reference_time
                if delta > INTERRUPT_MATCH_LATE_SECONDS:
                    break
                # The lifecycle pass has already seen the whole match, so
                # reverse history starts with casts that may occur well after
                # this interrupt. They are not evidence of a newer competing
                # cast at the interrupt timestamp and must not poison the
                # search for the stopped cast immediately before the event.
                if delta < -INTERRUPT_MATCH_EARLY_SECONDS:
                    continue
                if cast.outcome != "stopped":
                    passed_newer_cast = True
                    continue
                if passed_newer_cast:
                    continue
                if skill_id > 0 and cast.skill_id != skill_id:
                    passed_newer_cast = True
                    continue
                matched = cast
                break
            if matched is None:
                interrupted_unmatched += 1
                continue
            matched.outcome = "interrupted"
            interrupted_matched += 1
            if victim_id in rows:
                rows[victim_id]["casts_interrupted"] += 1
            if interrupter_id in rows:
                latency_ms = max(0, round((event.time - matched.started_at) * 1000))
                rows[interrupter_id]["rupt_cast_progress_ms_sum"] += latency_ms
                rows[interrupter_id]["rupt_cast_progress_n"] += 1

    # Coward resolves server-side at the same millisecond. Bull's Strike uses
    # are observable, but success is not: another warrior KD can share its
    # source/target window, and blocks/KD immunity are absent from the stream.
    consumed_kds: set[int] = set()
    coward_uses = [
        event for event in ordered
        if event.kind == "INSTANT_SKILL_USED" and len(event.fields) >= 3
        and _integer(event.fields[0]) == COWARD_ID
    ]
    bulls_uses = [
        event for event in ordered
        if event.kind == "ATTACK_SKILL_ACTIVATED" and len(event.fields) >= 3
        and _integer(event.fields[0]) == BULLS_STRIKE_ID
    ]

    for use in coward_uses:
        source_id = _integer(use.fields[1])
        if source_id not in rows:
            continue
        rows[source_id]["coward_uses"] += 1
        candidates = [
            index
            for index, (kd_time, _victim, kd_source) in enumerate(kd_events)
            if index not in consumed_kds and kd_source == source_id
            and round(kd_time * 1000) == _event_ms(use)
        ]
        if candidates:
            index = candidates[0]
            consumed_kds.add(index)
            rows[source_id]["coward_kds"] += 1

    for use in bulls_uses:
        source_id = _integer(use.fields[1])
        victim_id = _integer(use.fields[2])
        if source_id not in rows:
            continue
        rows[source_id]["bulls_strike_uses"] += 1
        if victim_id <= 0:
            rows[source_id]["bulls_strike_target_unknown"] += 1

    for agent_id, casts in history.items():
        rows[agent_id]["casts_cancelled_voluntary"] += sum(
            cast.outcome == "stopped" for cast in casts
        )

    output: dict[str, list[dict[str, int]]] = {}
    for agent_id, (party_id, player_number) in players.items():
        row = {"player_number": player_number, **rows[agent_id]}
        # A fully observed zero is meaningful here; unlike a missing event
        # stream, it says the event parser ran and nothing happened.
        output.setdefault(party_id, []).append(row)
    for party_rows in output.values():
        party_rows.sort(key=lambda row: row["player_number"])

    return {
        "schema": SCHEMA_VERSION,
        "players": output,
        "attribution": {
            "interrupt_events": interrupted_total,
            "interrupt_events_player_victim": interrupted_player_victim,
            "interrupt_casts_matched": interrupted_matched,
            "interrupt_casts_unmatched": interrupted_unmatched,
            "interrupt_events_untracked_victim": interrupted_untracked_victim,
            "match_early_ms": round(INTERRUPT_MATCH_EARLY_SECONDS * 1000),
            "match_late_ms": round(INTERRUPT_MATCH_LATE_SECONDS * 1000),
            "knockdown_events": knockdown_events,
            "knockdown_events_untracked_source": knockdown_untracked_source,
            "knockdown_events_untracked_victim": knockdown_untracked_victim,
            "conditional_kd_events_assigned": len(consumed_kds),
            "conditional_kd_events_unassigned": len(kd_events) - len(consumed_kds),
            "coward_match_tolerance_ms": 0,
        },
    }


def build_from_match_dir(infos: dict, match_dir: Path) -> dict:
    events, sources = read_stoc_events(match_dir)
    # Both streams are required for lifecycle classification. Publishing
    # confident zeroes from a partial recording violates absent-is-not-zero.
    if not all(source in sources for source in REQUIRED_STOC_SOURCES):
        return {}
    result = build_combat_analytics(infos, events)
    from player_matrix import build_player_matrix
    matrix = build_player_matrix(infos, events, match_dir)
    if matrix:
        result["player_matrix"] = matrix
    if "attack_skill_events" not in sources:
        for party_rows in result["players"].values():
            for row in party_rows:
                row.pop("bulls_strike_uses", None)
                row.pop("bulls_strike_target_unknown", None)
    result["sources"] = sorted(sources)
    return result


def merge_preserving_richer(prior: dict, current: dict) -> dict:
    """Merge same-schema analytics without erasing fields from missing streams."""
    if not isinstance(prior, dict) or not prior:
        return current
    if not isinstance(current, dict) or not current:
        return prior
    if prior.get("schema") != current.get("schema"):
        return current

    merged = dict(current)
    merged["sources"] = sorted(set(prior.get("sources", ())) |
                               set(current.get("sources", ())))
    attribution = dict(prior.get("attribution", {}))
    attribution.update(current.get("attribution", {}))
    merged["attribution"] = attribution

    players: dict[str, list[dict]] = {}
    party_ids = set(prior.get("players", {})) | set(current.get("players", {}))
    for party_id in party_ids:
        by_number: dict[int, dict] = {}
        for block in (prior, current):
            for row in block.get("players", {}).get(party_id, ()): 
                if not isinstance(row, dict) or "player_number" not in row:
                    continue
                number = _integer(row["player_number"], -1)
                if number < 0:
                    continue
                by_number.setdefault(number, {}).update(row)
        if by_number:
            players[party_id] = [by_number[number] for number in sorted(by_number)]
    merged["players"] = players
    return merged
