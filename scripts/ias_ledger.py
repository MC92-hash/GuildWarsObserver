"""Per-player attack-speed uptime, from the agent snapshots.

**Aggressive Refrain does not need an effect stream, and nothing else here did
either.** ``condition_ledger`` and ``blind_ledger`` both record that
``effect_events.txt`` exists in 0 of 2,366 archived matches -- those packets are
sent only to party members and an observer is in no party -- and every uptime
request since has been answered "it has to be modelled". That is right about the
mechanism and wrong about this metric: a 25% attack-speed increase is visible in
snapshot field 36, ``attack_speed_modifier``, which has been recorded all along
and which nothing in either repo has ever parsed.

**The field is a multiplier on the attack INTERVAL, so lower is faster.** GWCA's
own header says ``0.67 = 33% increase (1-.33)``, and
``ReplayWindow_AgentModels.cpp`` settles it: ``effectiveAttackTime =
weapon_attack_speed * attack_speed_modifier``. An increase and a slow MULTIPLY,
so a 25% boost under a 50% slow reads 1.125 -- above 1.0, and still a boost.

Measured over 3,291,411 player rows, the field takes exactly eight values, and
they are precisely the cross product of {none, 0.75, 0.67} against {none, 1.33,
1.5}: 1.000, 0.750, 0.670, 1.500, 1.005, 1.330, 1.125, 0.891. **Not one sample
falls outside that model**, so the table below is generated from the factors
rather than listed, and ``ias_modifier_unrecognised`` should stay at zero
forever. A non-zero value means a game update changed a factor -- read it as an
alarm, not as a shrug.

**The value is latched to the last attack, and that decides the denominator.**
GWCA calls it the modifier of the *last attack* and it is literally that:

* 929,789 rows (28.2%) carry ``weapon_attack_speed == 0``, and in **100.000% of
  them** the modifier reads exactly ``1.000``. That region is "this agent has
  not attacked yet"; the 1.000 is a default, not a reading. A Paragon's Refrain
  is up from the pre-match setup but the field says nothing about it until their
  first spear throw, a median ~21 s in -- and ~134 s for a Monk.
* It latches THROUGH death. While dead, 9.6% of samples still read a boost, and
  **0 of 380 resurrections reset it**. A corpse has no attack speed; counting
  those seconds rewards dying under Refrain.

So the modifier is an observation only while the player is alive and has swung
at least once, and the denominator is those seconds -- not the match. Both gates
come free off the same line. This is absent-is-not-zero applied per interval
rather than per row.

**Round once, at the end.** ``avatar_ledger`` and ``condition_ledger`` both
round per state change, which is right for them: model changes and cripple spans
are seconds apart. This ledger credits every snapshot INTERVAL and those have a
median of about 100 ms, every one of which would round to nothing. Accumulate in
float across the appearance and round once.

The denominator is deliberately **not** called ``match_seconds``.
``avatar_ledger`` publishes its own denominator under that bare key and returns
``{}`` for the ~2,322 matches with no model stream; a second publisher would put
``match_seconds`` on rows carrying no ``form_seconds``, and since
``avatar_uptime`` is a POOLED ratio that would silently deflate it across the
whole archive with nothing failing anywhere.

Named for what it measures, not for one profession: the same field carries
Frenzy and Flail on a Warrior, Tiger's Fury on a Ranger and Heart of Fury on a
Dervish. Measured medians under the rule above: Paragon 96.6%, Warrior 49.5%,
Dervish 40.2%, Ranger 18.5%, Assassin 10.5%, and **exactly 0.0% for the five
professions that carry no boost at all** -- which is the control group the data
hands you for free.
"""
from __future__ import annotations

from pathlib import Path

from combat_analytics import _player_lookup as player_lookup
from combat_analytics import match_window
from condition_ledger import INCARNATION_BREAK, read_snapshot_records

SCHEMA_VERSION = 1

# Zero-based, in the 50- and 51-field snapshot line. Index 35 exists in both
# variants -- `max_hp_is_live` was appended last -- so this reaches the whole
# local archive. Read positionally rather than through `max_hp_solver.Snapshot`,
# which is a twin of MaxHpSolver.cpp and must not grow fields it does not have.
DEAD_FIELD = 8
WEAPON_SPEED_FIELD = 34
MODIFIER_FIELD = 35
MIN_SNAPSHOT_FIELDS = 36

# factor -> tier percent. An increase is stored as (1 - fraction) on the
# interval; a slow as (1 + fraction). The nine products are generated below.
IAS_FACTORS = {1.0: 0, 0.75: 25, 0.67: 33}
SLOW_FACTORS = (1.0, 1.33, 1.5)
# Values print to three decimals, and 0.75 * 1.33 = 0.9975 sits 0.0025 from
# 1.000 -- far too close for a threshold, so classification is nearest-match
# inside this tolerance and refuses outside it.
MODIFIER_TOLERANCE = 0.002

_MODIFIERS: dict[float, tuple[int, bool]] = {
    round(ias * slow, 4): (tier, slow != 1.0)
    for ias, tier in IAS_FACTORS.items()
    for slow in SLOW_FACTORS
}

# An interval longer than this is still credited -- capping it moved the Paragon
# median by 0.5 pp and would have bought a constant -- but the seconds it
# credits are published so the exposure is a number rather than a caveat.
LONG_INTERVAL_SECONDS = 2.0


def classify(modifier: float) -> tuple[int, bool] | None:
    """``(tier_percent, under_a_slow)`` for a modifier, or None if unrecognised."""
    best: tuple[int, bool] | None = None
    best_delta = MODIFIER_TOLERANCE
    for value, info in _MODIFIERS.items():
        delta = abs(modifier - value)
        if delta < best_delta:
            best, best_delta = info, delta
    return best


def _row() -> dict:
    return {
        "ias_seconds": 0.0,
        "ias_measured_seconds": 0.0,
        "ias_seconds_25": 0.0,
        "ias_seconds_33": 0.0,
        "ias_slowed_seconds": 0.0,
        "ias_dead_seconds": 0.0,
        "ias_unarmed_seconds": 0.0,
        "ias_windows": 0,
    }


def build_ias_ledger(infos: dict, match_dir: Path,
                     records: dict | None = None) -> dict:
    """Per-player attack-speed uptime, keyed by party id.

    ``records`` is the shared snapshot read. Never re-read them here: doing so
    cost 1.15 s on a 6.85 s call, which is 45 minutes over a full backfill.
    """
    players = player_lookup(infos)
    if not players:
        return {}
    if records is None:
        records = read_snapshot_records(match_dir, players)
    if not records:
        return {}

    start, end = match_window(infos)
    horizon = max(0.0, end - start)
    if horizon <= 0:
        return {}

    rows: dict[int, dict] = {}
    audit = {
        "ias_rows_read": 0,
        "ias_rows_short_fields": 0,
        "ias_modifier_unrecognised": 0,
        "ias_incarnation_breaks": 0,
        "ias_long_interval_seconds": 0.0,
        "ias_agents_measured": 0,
    }

    for agent_id in players:
        history = records.get(agent_id)
        if not history:
            continue
        row = _row()
        previous: tuple[float, list[str]] | None = None
        was_up = False

        # File order, never sorted: INCARNATION_BREAK is positional.
        for when, fields in history:
            if fields and fields[0].startswith(INCARNATION_BREAK):
                # A different agent owns this id from here. The seconds BEFORE
                # the break are still the first agent's, so close the pending
                # interval at it rather than discarding it -- the same choice
                # `condition_ledger.crippled_spans` makes. In a real file the
                # break carries the previous line's timestamp, so this is
                # usually a zero-length close; it matters when it is not.
                if previous is not None:
                    prev_when, prev_fields = previous
                    lo, hi = max(prev_when, start), min(when, end)
                    if hi > lo:
                        _credit(row, prev_fields, hi - lo, audit, was_up)
                previous, was_up = None, False
                audit["ias_incarnation_breaks"] += 1
                continue
            if len(fields) < MIN_SNAPSHOT_FIELDS:
                audit["ias_rows_short_fields"] += 1
                continue
            audit["ias_rows_read"] += 1

            if previous is not None:
                prev_when, prev_fields = previous
                lo, hi = max(prev_when, start), min(when, end)
                duration = hi - lo
                if duration > 0:
                    was_up = _credit(row, prev_fields, duration, audit, was_up)
            previous = (when, fields)

        # No tail: the measurement ends at the last snapshot, never at match
        # end. Past it nothing was observed, and inventing seconds is the one
        # thing this must not do.
        if row["ias_measured_seconds"] <= 0:
            continue
        audit["ias_agents_measured"] += 1
        rows[agent_id] = row

    if not rows:
        return {}

    out: dict[str, list[dict]] = {}
    for agent_id, row in rows.items():
        party_id, player_number = players[agent_id]
        measured = round(row["ias_measured_seconds"])
        if measured <= 0:
            continue
        # A share cannot exceed its whole, and measured p90 is exactly 100.0%,
        # so this clamp is load-bearing rather than decorative.
        published = {
            "player_number": player_number,
            "ias_measured_seconds": measured,
            "ias_seconds": min(round(row["ias_seconds"]), measured),
            "ias_seconds_25": min(round(row["ias_seconds_25"]), measured),
            "ias_seconds_33": min(round(row["ias_seconds_33"]), measured),
            "ias_slowed_seconds": min(round(row["ias_slowed_seconds"]), measured),
            "ias_dead_seconds": round(row["ias_dead_seconds"]),
            "ias_unarmed_seconds": round(row["ias_unarmed_seconds"]),
            "ias_windows": row["ias_windows"],
            "ias_match_seconds": round(horizon),
        }
        out.setdefault(party_id, []).append(published)
    for party_rows in out.values():
        party_rows.sort(key=lambda entry: entry["player_number"])
    if not out:
        return {}

    attribution = dict(audit)
    attribution["ias_long_interval_seconds"] = round(audit["ias_long_interval_seconds"])
    attribution["ias_tolerance_milli"] = round(MODIFIER_TOLERANCE * 1000)
    return {
        "schema": SCHEMA_VERSION,
        "players": out,
        "attribution": attribution,
    }


def _credit(row: dict, fields: list[str], duration: float,
            audit: dict, was_up: bool) -> bool:
    """Credit one interval with the state held at its start. Returns `is up`."""
    # Order matters, and each gate has its own published residue.
    if fields[DEAD_FIELD] not in ("", "0"):
        # The modifier latches through death: 0 of 380 resurrections reset it.
        row["ias_dead_seconds"] += duration
        return False
    try:
        weapon_speed = float(fields[WEAPON_SPEED_FIELD])
        modifier = float(fields[MODIFIER_FIELD])
    except (TypeError, ValueError):
        return False
    if weapon_speed <= 0.0:
        # Never attacked yet, so the 1.000 is a default and not a reading.
        row["ias_unarmed_seconds"] += duration
        return False
    classified = classify(modifier)
    if classified is None:
        audit["ias_modifier_unrecognised"] += 1
        return False

    tier, slowed = classified
    row["ias_measured_seconds"] += duration
    if slowed:
        row["ias_slowed_seconds"] += duration
    if not tier:
        return False

    row["ias_seconds"] += duration
    row["ias_seconds_25" if tier == 25 else "ias_seconds_33"] += duration
    if duration > LONG_INTERVAL_SECONDS:
        audit["ias_long_interval_seconds"] += duration
    if not was_up:
        # Rising edge only. A slow landing mid-boost moves 0.750 -> 1.125 and
        # must not open a second window.
        row["ias_windows"] += 1
    return True
