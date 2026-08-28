"""Per-player Cripple, from the agent snapshots and the skill streams.

**There is no condition event in any recording.** ``EventHooks.cpp`` does hook
``AddEffect``/``RemoveEffect`` and format an ``EFFECT_APPLIED`` line carrying the
skill id, and ``GwReplayRecorder.cpp`` writes it to ``effect_events.txt`` -- but
that file exists in 0 of 2,366 archived matches, because those packets are only
sent for party members and an observer is in no party. No amount of recording
will produce them.

The conditions are in the agent snapshots instead. ``Agents/<id>.txt.gz`` field
16 is ``has_crippled``, sampled at 100 ms, and a snapshot is written whenever any
field changes -- so the on and off transitions are timestamped and **seconds
crippled is a directly observed fact**.

What is NOT observed is who did it. The snapshot names the victim and nothing
else, exactly as an ``INTERRUPTED`` record does, and it takes the same answer:
each onset is joined to a cripple skill aimed at that victim, credited only
where one player could have caused it, and the residue is published rather than
absorbed. Measured over 150 recordings, 10,911 onsets: **55.3% credited, 1.0%
ambiguous, 43.7% unexplained.** The ambiguous share is lower than the interrupt
join's because cripple sources are spread across professions rather than stacked
on one bar; the unexplained share is higher, and is published as such.

**Harrier's Grasp is not a cripple cast.** It is an enchantment on the attacker;
the cripple lands when the enchanted player hits a fleeing foe. Counting its
activations counts re-application, which is a maintenance number. It is also the
single largest cripple source in the archive -- 2,835 credits against the
signet's 2,708 -- so the on-hit join is not an optional refinement.
"""
from __future__ import annotations

import gzip
import json
from collections.abc import Iterable
from pathlib import Path

from combat_analytics import _player_lookup as player_lookup
from combat_analytics import match_window

SCHEMA_VERSION = 1

# ``has_crippled``, zero-based, in the 50-field snapshot line documented in
# EXPORTS_CONVENTIONS.md. Read positionally rather than through
# ``max_hp_solver.Snapshot``, which is a faithful twin of
# SourceFiles/MaxHpSolver.cpp and must not grow fields that solver does not have.
CRIPPLED_FIELD = 15
MIN_SNAPSHOT_FIELDS = 16

# How long before an onset a targeted cripple skill may have been used. Wider
# than the knockdown window because a cripple can arrive with a projectile and
# the snapshot that reports it is up to 100 ms late.
CRIPPLE_WINDOW_SECONDS = 2.0
# How long before an onset the attack that carried an on-hit cripple may have
# landed.
CRIPPLE_ATTACK_WINDOW_SECONDS = 1.5
# How long an on-hit cripple enchantment is assumed to stay up after its cast.
# Generous on purpose: over-running it can only ever add a candidate, and an
# extra candidate makes the ledger REFUSE rather than mis-credit.
ONHIT_ENCHANT_SECONDS = 30.0

# Skills whose own use applies Cripple to the target it names.
#
# Curated from the 48 skills whose description mentions Cripple, then measured.
# Two families are deliberately absent and must stay absent: condition REMOVAL
# (Mend Condition 275, Restore Condition 276, Mend Ailment 277, Purge Conditions
# 278, Return 770) and cripple IMMUNITY ("I Am Unstoppable!" 2356). Both match
# the same description text and both explain a cripple ending or never landing,
# which is the opposite of causing one.
_CRIPPLE_TARGETED: dict[int, str] = {
    37: "Illusion of Haste",
    54: "Crippling Anguish",
    320: "Hamstring",
    323: "Desperation Blow",
    334: "Axe Rake",
    392: "Pin Down",
    393: "Crippling Shot",
    438: "Maiming Strike",
    853: "Melandru's Shot",
    974: "Mantis Touch",
    1021: "Jungle Strike",
    1023: "Leaping Mantis Sting",
    1024: "Black Mantis Thrust",
    1038: "Crippling Dagger",
    1045: "Palm Strike",
    1133: "Drunken Blow",
    1415: "Crippling Slash",
    1535: "Crippling Sweep",
    1767: "Reaper's Sweep",
    2010: "Knee Cutter",
    2014: "Signet of Pious Restraint",
    2135: "Trampling Ox",
    2147: "Crippling Victory",
    2150: "Maiming Spear",
}

# Enchantments that cripple when the ENCHANTED player lands a hit. The cast is
# not the application, so these are joined through the attack streams.
_CRIPPLE_ONHIT: dict[int, str] = {
    1495: "Aura of Thorns",
    1756: "Grenth's Grasp",
    1758: "Harrier's Grasp",
}

# Sources with no target this stream can see. Two different reasons, one
# behaviour: they are candidates for any victim in window, which lets them both
# earn a credit and, more often, REFUSE one that would otherwise land on a
# targeted skill that happened to be nearby.
#
# Traps and area skills genuinely have no target. The SHOUTS are here because
# of how they are recorded, not how they work: a shout arrives as
# `INSTANT_SKILL_USED`, whose payload carries no target, so the record names the
# caster in the target slot. Measured, "You're All Alone!" appears 862 times
# across 250 recordings and NEVER with a real target. Listing it as targeted --
# as this table first did -- meant `target_id in (victim, 0)` could never match
# it, so it could neither be credited nor refuse, and cripples it caused were
# handed to whatever targeted skill was in window. Moving it here credits 223
# more cripples over 120 matches and correctly refuses 21 that had been credited
# to the wrong player.
_CRIPPLE_UNTARGETED: dict[int, str] = {
    458: "Barbed Trap",
    461: "Spike Trap",
    854: "Snare",
    985: "Caltrops",
    1412: "You're All Alone!",
    1476: "Tripwire",
    1554: "Crippling Anthem",
    1642: "Hidden Caltrops",
    2358: "You Move Like a Dwarf!",
}

_ACTIVATION_KINDS = ("SKILL_ACTIVATED", "ATTACK_SKILL_ACTIVATED", "INSTANT_SKILL_USED")
_ATTACK_LANDED_KINDS = ("ATTACK_FINISHED", "ATTACK_SKILL_FINISHED")

_META_PATH = Path(__file__).resolve().parent / "data" / "skill_meta.json"
_PVP_TWINS: dict[int, int] | None = None


def canonical_skill_id(skill_id: int) -> int:
    """A PvP split id mapped back to its base id, or the id unchanged.

    Every credited Signet of Pious Restraint cripple in the archive came from
    3273, the PvP twin, and none from 2014. A skill table keyed on base ids only
    works because of this call. A missing data file degrades to identity, which
    costs the split skills and keeps the rest.
    """
    global _PVP_TWINS
    if _PVP_TWINS is None:
        try:
            meta = json.loads(_META_PATH.read_text(encoding="utf-8"))
            _PVP_TWINS = {int(k): int(v) for k, v in meta.get("pvp_twins", {}).items()}
        except (OSError, ValueError, TypeError):
            _PVP_TWINS = {}
    return _PVP_TWINS.get(skill_id, skill_id)


def _seconds(header: str) -> float | None:
    try:
        minute, rest = header.split(":", 1)
        second, millis = rest.split(".", 1) if "." in rest else (rest, "0")
        return int(minute) * 60 + int(second) + int(millis) / 1000.0
    except (ValueError, TypeError):
        return None


def _open(path: Path):
    opener = gzip.open if path.suffix.lower() == ".gz" else open
    return opener(path, "rt", encoding="utf-8-sig", errors="replace")


def _stream(match_dir: Path, stem: str) -> Path | None:
    stoc = match_dir / "StoC"
    return next((p for p in (stoc / f"{stem}.txt.gz", stoc / f"{stem}.txt")
                 if p.is_file()), None)


def _snapshot_path(match_dir: Path, agent_id: int) -> Path | None:
    return next((p for p in (match_dir / "Agents" / f"{agent_id}.txt.gz",
                             match_dir / "Agents" / f"{agent_id}.txt")
                 if p.is_file()), None)


def _records(path: Path):
    """(time, fields) for a timestamped semicolon stream."""
    with _open(path) as handle:
        for line in handle:
            line = line.strip()
            close = line.find("]")
            if not line.startswith("[") or close < 0:
                continue
            when = _seconds(line[1:close])
            if when is None:
                continue
            yield when, line[close + 1:].strip().split(";")


def crippled_spans(rows: Iterable[tuple[float, list[str]]]) -> list[tuple[float, float]]:
    """(start, end) for every stretch an agent spent crippled.

    Takes already-parsed snapshot lines so the file is read once per match
    rather than once per consumer. Reading it again here cost 1.15 s on a 6.85 s
    call, which is 45 minutes over a full backfill, entirely to re-parse lines
    ``player_matrix`` had just parsed.

    A stretch still open at the last snapshot is closed there rather than at the
    match end: past the final snapshot nothing was observed, and inventing
    seconds is the one thing this must not do.
    """
    spans: list[tuple[float, float]] = []
    started: float | None = None
    last_time = 0.0
    for when, fields in rows:
        if len(fields) < MIN_SNAPSHOT_FIELDS:
            continue
        last_time = when
        crippled = fields[CRIPPLED_FIELD] == "1"
        if crippled and started is None:
            started = when
        elif not crippled and started is not None:
            spans.append((started, when))
            started = None
    if started is not None and last_time > started:
        spans.append((started, last_time))
    return spans


def crippled_intervals(path: Path) -> list[tuple[float, float]]:
    """:func:`crippled_spans` for one snapshot file, read from disk."""
    return crippled_spans(_records(path))


def _read_activations(match_dir: Path, players: dict):
    """Targeted cripple attempts, and when each on-hit enchantment was cast."""
    attempts: list[tuple[float, int, int, int]] = []
    onhit: dict[int, list[float]] = {}
    for stem in ("skill_events", "attack_skill_events"):
        path = _stream(match_dir, stem)
        if path is None:
            continue
        for when, parts in _records(path):
            if parts[0] not in _ACTIVATION_KINDS or len(parts) < 3:
                continue
            try:
                skill_id, agent_id = int(parts[1]), int(parts[2])
                target_id = int(parts[3]) if len(parts) > 3 else 0
            except ValueError:
                continue
            if agent_id not in players:
                continue
            # A record that names the caster in the target slot is not a skill
            # aimed at its own caster -- it is a skill whose target this stream
            # cannot see, which is what `INSTANT_SKILL_USED` always looks like.
            # Read as targeted-at-self it matches no victim and so can neither
            # credit nor refuse; read as untargeted it does both.
            if target_id == agent_id:
                target_id = 0
            skill_id = canonical_skill_id(skill_id)
            if skill_id in _CRIPPLE_TARGETED or skill_id in _CRIPPLE_UNTARGETED:
                attempts.append((when, agent_id, target_id, skill_id))
            elif skill_id in _CRIPPLE_ONHIT:
                onhit.setdefault(agent_id, []).append(when)
    return attempts, onhit


def _read_landed_attacks(match_dir: Path, players: dict) -> list[tuple[float, int, int]]:
    """(time, attacker, victim) for every attack that landed.

    ``ATTACK_FINISHED`` is ``attacker;0;target`` -- the middle field is the
    GenericValue payload, not the victim. Reading it as the victim credits
    nobody, which is how the first cut of this join lost every on-hit cripple.
    """
    landed: list[tuple[float, int, int]] = []
    for stem in ("basic_attack_events", "attack_skill_events"):
        path = _stream(match_dir, stem)
        if path is None:
            continue
        for when, parts in _records(path):
            if parts[0] not in _ATTACK_LANDED_KINDS or len(parts) < 4:
                continue
            try:
                attacker, victim = int(parts[1]), int(parts[3])
            except ValueError:
                continue
            if attacker in players and victim:
                landed.append((when, attacker, victim))
    return landed


def read_snapshot_records(match_dir: Path, agent_ids: Iterable[int]) -> dict:
    """agent id -> (time, fields) for every snapshot file that exists.

    Public because the caller reads these once and hands them to both this
    module and ``player_matrix``: the files are the most expensive thing in a
    match to open, and parsing them twice cost 45 minutes over a full backfill.
    """
    out: dict[int, list[tuple[float, list[str]]]] = {}
    for agent_id in agent_ids:
        path = _snapshot_path(match_dir, agent_id)
        if path is None:
            continue
        rows = list(_records(path))
        if rows:
            out[agent_id] = rows
    return out


def build_condition_ledger(infos: dict, match_dir: Path,
                           records: dict | None = None) -> dict:
    """Per-player cripple counters, or {} when the snapshots carry nothing."""
    players = player_lookup(infos)
    if not players:
        return {}

    if records is None:
        records = read_snapshot_records(match_dir, players)
    spans: dict[int, list[tuple[float, float]]] = {}
    for agent_id in players:
        found = crippled_spans(records.get(agent_id, ()))
        if found:
            spans[agent_id] = found
    if not spans:
        # No snapshot carried the field. Absent is not zero.
        return {}

    attempts, onhit = _read_activations(match_dir, players)
    landed = _read_landed_attacks(match_dir, players)

    # Snapshots span the whole instance, which begins about a minute before the
    # match does and can run past its end. Seconds outside the match are not
    # match seconds: nothing is crippled during the setup, but the tail after a
    # guild lord dies is real and would otherwise be counted as pressure.
    start, end = match_window(infos)
    if end <= start:
        start, end = 0.0, float("inf")

    rows = {
        agent_id: {
            "crippled_seconds_received": 0,
            "cripple_onsets_received": 0,
            "cripple_applications": 0,
            "cripple_caused_seconds": 0,
        }
        for agent_id in players
    }

    onsets = credited = ambiguous = uncredited = 0
    for victim, victim_spans in spans.items():
        for span_start, span_end in victim_spans:
            if span_end <= start or span_start >= end:
                continue
            span_start = max(span_start, start)
            duration = max(0, round(min(span_end, end) - span_start))
            rows[victim]["crippled_seconds_received"] += duration
            rows[victim]["cripple_onsets_received"] += 1
            onsets += 1

            casters: set[int] = set()
            for when, agent_id, target_id, skill_id in attempts:
                if agent_id == victim:
                    continue
                if not 0 <= span_start - when <= CRIPPLE_WINDOW_SECONDS:
                    continue
                # An untargeted source names nobody, so it is a candidate for
                # any victim in window -- which is what makes it a refusal.
                if skill_id in _CRIPPLE_UNTARGETED or target_id in (victim, 0):
                    casters.add(agent_id)
            for when, attacker, hit in landed:
                if hit != victim or attacker == victim or attacker in casters:
                    continue
                if not 0 <= span_start - when <= CRIPPLE_ATTACK_WINDOW_SECONDS:
                    continue
                if any(0 <= when - cast <= ONHIT_ENCHANT_SECONDS
                       for cast in onhit.get(attacker, ())):
                    casters.add(attacker)

            if len(casters) == 1:
                caster = next(iter(casters))
                credited += 1
                if caster in rows:
                    rows[caster]["cripple_applications"] += 1
                    rows[caster]["cripple_caused_seconds"] += duration
            elif casters:
                ambiguous += 1
            else:
                uncredited += 1

    output: dict[str, list[dict[str, int]]] = {}
    for agent_id, (party_id, player_number) in players.items():
        output.setdefault(party_id, []).append(
            {"player_number": player_number, **rows[agent_id]}
        )
    for party_rows in output.values():
        party_rows.sort(key=lambda row: row["player_number"])

    return {
        "schema": SCHEMA_VERSION,
        "players": output,
        "attribution": {
            "cripple_onsets": onsets,
            "cripple_credited_unique": credited,
            "cripple_ambiguous": ambiguous,
            "cripple_uncredited": uncredited,
            "cripple_window_ms": round(CRIPPLE_WINDOW_SECONDS * 1000),
            "cripple_attack_window_ms": round(CRIPPLE_ATTACK_WINDOW_SECONDS * 1000),
            "cripple_agents_with_snapshots": len(spans),
        },
    }
