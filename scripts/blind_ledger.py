"""Modelled Blind uptime, from the casts that applied it.

Every other condition in this pipeline is read off the agent snapshots, where an
on and an off transition are timestamped and the seconds between them are a
directly observed fact.  **Blind cannot be, and no recording will ever fix it.**

Two independent sources both fail, for different reasons:

* ``effect_events.txt`` carries the skill id and the apply/remove pair, and
  ``GwReplayRecorder.cpp`` writes it -- but ``AddEffect``/``RemoveEffect`` are
  sent only to party members and an observer is in no party.  The file is absent
  from 0 of 2,366 archived matches.
* Snapshot field 17 is ``has_blind``, and it is **always 0**.
  ``AgentSerializer.cpp`` never assigns it, because GWCA's
  ``AgentLiving::effects`` bitfield has no Blind bit to read.

So this is a model, and it is named one.  Every seconds field carries a
``_modeled`` suffix, and is published as a **low/high band** rather than a point
estimate, because the one input the model needs after the cast -- the caster's
attribute rank -- is not observable either.  The band is the skill's own
duration range evaluated at rank 12 and rank 15.

**It leans short, on purpose.**  Three of its rules can only remove seconds:

* Only *targeted, unconditional* sources are modelled.  An area blind names no
  victim and a conditional one ("if target foe is on fire") cannot be resolved
  from the stream, so both are counted in ``blind_casts_unmodelled`` and
  contribute nothing.  On a bar with Blinding Flash that residue is near zero;
  on one with Ebon Dust Aura it is everything.
* Any condition removal reaching a blinded player truncates their window, even
  though most removals take one condition and it may not have been the Blind.
* A window is truncated at the match end, never extended past it.

Validate a change here against damage, not against itself: a blinded player's
attacks miss, so the share of their damage packets falling inside a modelled
window should be far below the share of their attack starts.  On 2026-08-27 Isle
of the Dead, a ranger blinded 16 times had 32% of his attack starts inside the
windows and 1 of 125 damage packets -- that gap is what says the windows landed
in the right places.

Kept out of ``condition_ledger`` deliberately.  That module's whole thesis is
that a cripple span is observed; this one's is that a blind span is not, and a
defect in a model must not be able to take the observation down with it.
"""
from __future__ import annotations

from collections.abc import Mapping

SCHEMA_VERSION = 1

# The band. GW interpolates a "3...8" range linearly across attribute ranks 0 to
# 15, so the rank is what turns a range into a number -- and the rank is the one
# thing neither the stream nor the snapshot carries. Rank 12 is the floor of a
# serious bar; 15 is a 12 attribute with a superior rune and a headpiece.
RANK_LOW = 12
RANK_HIGH = 15
RANK_SCALE = 15

# Sources that name their victim and blind unconditionally, id -> the (low,
# high) seconds of the skill's own range, read from the skill descriptions.
#
# Deliberately absent, and why:
#
#   Conditional on target state -- Steam 846 ("if target foe is on fire"), Gaze
#   from Beyond 1245 ("if you are within earshot of a spirit"), Ineptitude 47
#   (on the victim's next attack, not on the cast). The stream cannot resolve
#   the condition, so a cast is not evidence a blind landed.
#
#   Delivered by an attack -- Temple Strike 988, Sneak Attack 2116. These blind
#   only if the attack hits, and a hit is exactly what this pipeline cannot see:
#   a blocked swing and a landed one both emit ATTACK_FINISHED.
#
#   Area, with no target in the record -- Eruption 167, Belly Smash 350, Dust
#   Trap 457, Blind Was Mingson 788, Ride the Lightning 836, Shadowsong 871,
#   Rupture Soul 917, Unseen Fury 1041, Dust Cloak 1497, Smoke Trap 1729, Ebon
#   Dust Aura 1760, Smoke Powder Defense 2136, Black Powder Mine 2223.
#
# Every one of those is counted in `blind_casts_unmodelled` instead, so a bar
# this table cannot model shows up as a published gap rather than a quiet zero.
_BLIND_TARGETED: dict[int, tuple[int, int]] = {
    58: (15, 15),    # Signet of Midnight -- flat, no scaling
    220: (3, 8),     # Blinding Flash
    232: (1, 4),     # Lightning Touch
    424: (3, 15),    # Throw Dirt
    973: (3, 15),    # Blinding Powder
    1367: (3, 8),    # Blinding Surge -- named target only, adjacent unmodelled
}

# Casts that blind somebody this cannot name. Audited, never scored.
_BLIND_UNMODELLED: frozenset[int] = frozenset({
    47, 167, 350, 457, 788, 836, 846, 871, 917, 988, 1041, 1245, 1497, 1729,
    1760, 2116, 2136, 2223,
})

# Removals aimed at one named ally. Any of them ends a modelled window early.
_REMOVAL_TARGETED: frozenset[int] = frozenset({
    275,   # Mend Condition
    276,   # Restore Condition
    277,   # Mend Ailment
    278,   # Purge Conditions
    295,   # Purge Signet
    311,   # Draw Conditions
    941,   # Blessed Light
    1123,  # Life Sheath
    1234,  # Mend Body and Soul
    1599,  # "It's Just a Flesh Wound."
    1690,  # Signet of Removal
    1691,  # Dismiss Condition
    1692,  # Divert Hexes
    2004,  # Smite Condition
    2057,  # Foul Feast
    2202,  # Mending Grip
})

# Removals that clear allies without naming one. These truncate every window on
# the caster's own team -- the blunt reading, chosen because it can only shorten.
_REMOVAL_TEAMWIDE: frozenset[int] = frozenset({
    298,   # Martyr
    943,   # Extinguish
    1570,  # Song of Purification
    1588,  # Cautery Signet
})

# Removals a blinded player uses on themselves.
_REMOVAL_SELF: frozenset[int] = frozenset({
    132,   # Plague Signet
    149,   # Plague Sending
    154,   # Plague Touch
    300,   # Contemplation of Purity
    427,   # Antidote Signet
    872,   # Mending Touch
    1036,  # Signet of Malice
    1513,  # Guiding Hands
    1639,  # Assassin's Remedy
})


def _at_rank(low: int, high: int, rank: int) -> float:
    """A GW "low...high" range at one attribute rank."""
    return low + (high - low) * (rank / RANK_SCALE)


def _merge(spans: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """Overlapping windows collapsed, so re-blinding does not double-count."""
    out: list[tuple[float, float]] = []
    for start, end in sorted(spans):
        if end <= start:
            continue
        if out and start <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], end))
        else:
            out.append((start, end))
    return out


def _applications_and_clears(players: Mapping[int, tuple[str, int]],
                             history: Mapping[int, list],
                             fold):
    """Modelled blind applications, plus a per-victim list of clear times.

    Split out so that :func:`blind_windows` and :func:`build_blind_ledger` can
    never disagree about what a window is -- the validation gate in the module
    docstring is worthless if it is checking a second implementation.
    """
    applications: list[tuple[int, int, float, int, int]] = []
    removals_targeted: list[tuple[int, float]] = []
    removals_teamwide: list[tuple[str, float]] = []
    removals_self: list[tuple[int, float]] = []
    unmodelled = 0

    for agent_id, casts in history.items():
        party_id = players.get(agent_id, ("", 0))[0]
        for cast in casts:
            if cast.outcome != "completed" or cast.ended_at is None:
                continue
            skill_id = fold(cast.skill_id)
            when = cast.ended_at
            target = cast.target_id
            if skill_id in _BLIND_TARGETED and target and target != agent_id:
                low, high = _BLIND_TARGETED[skill_id]
                applications.append((target, agent_id, when, low, high))
            elif skill_id in _BLIND_UNMODELLED:
                unmodelled += 1
            if skill_id in _REMOVAL_TARGETED and target:
                removals_targeted.append((target, when))
            elif skill_id in _REMOVAL_TEAMWIDE and party_id:
                removals_teamwide.append((party_id, when))
            elif skill_id in _REMOVAL_SELF:
                removals_self.append((agent_id, when))

    cache: dict[int, list[float]] = {}

    def clears(victim: int) -> list[float]:
        if victim not in cache:
            party_id = players.get(victim, ("", 0))[0]
            cache[victim] = sorted(
                [w for target, w in removals_targeted if target == victim]
                + [w for pid, w in removals_teamwide if pid == party_id]
                + [w for agent_id, w in removals_self if agent_id == victim])
        return cache[victim]

    return applications, clears, unmodelled


def _window(when: float, low: int, high: int, rank: int,
            clears: list[float], limit: float) -> tuple[float, float]:
    """One application's window at one rank, cut by the first clear inside it."""
    end = min(when + _at_rank(low, high, rank), limit)
    cut = next((c for c in clears if when < c < end), None)
    return (when, cut if cut is not None else end)


def blind_windows(players: Mapping[int, tuple[str, int]],
                  history: Mapping[int, list],
                  match_end: float | None = None,
                  canonicalise=None,
                  rank: int = RANK_LOW) -> dict[int, list[tuple[float, float]]]:
    """victim agent id -> merged modelled blind windows, for validation.

    Public because the damage-suppression check described in the module
    docstring needs the windows themselves, not the seconds they sum to.
    """
    if not players or not history:
        return {}
    fold = canonicalise or (lambda skill_id: skill_id)
    limit = match_end if match_end and match_end > 0 else float("inf")
    applications, clears, _ = _applications_and_clears(players, history, fold)
    spans: dict[int, list[tuple[float, float]]] = {}
    for victim, _caster, when, low, high in applications:
        spans.setdefault(victim, []).append(
            _window(when, low, high, rank, clears(victim), limit))
    return {victim: _merge(found) for victim, found in spans.items()}


def build_blind_ledger(players: Mapping[int, tuple[str, int]],
                       history: Mapping[int, list],
                       match_end: float | None = None,
                       canonicalise=None) -> dict:
    """Per-player modelled blind seconds, as a low/high band.

    Reads the ``Cast`` history ``build_combat_analytics`` already built rather
    than the streams: a blind lands when a cast *completes*, and the closing
    record carries a skill id of 0, so completion is knowable only from the
    per-caster pairing that history is.
    """
    if not players or not history:
        return {}

    fold = canonicalise or (lambda skill_id: skill_id)
    limit = match_end if match_end and match_end > 0 else float("inf")
    applications, clears, unmodelled = _applications_and_clears(
        players, history, fold)

    if not applications:
        # No modellable source on either bar. Absent, not zero -- a team with
        # Ebon Dust Aura blinds constantly and would read as a clean sheet.
        return {}

    rows = {
        agent_id: {
            "blind_applications_received": 0,
            "blind_seconds_modeled_low": 0,
            "blind_seconds_modeled_high": 0,
            "blind_applications_caused": 0,
            "blind_seconds_caused_modeled_low": 0,
            "blind_seconds_caused_modeled_high": 0,
        }
        for agent_id in players
    }

    truncated = 0
    per_victim: dict[int, dict[str, list[tuple[float, float]]]] = {}
    per_caster: dict[int, dict[str, list[tuple[float, float]]]] = {}
    for victim, caster, when, low, high in applications:
        victim_clears = clears(victim)
        for band, rank in (("low", RANK_LOW), ("high", RANK_HIGH)):
            span = _window(when, low, high, rank, victim_clears, limit)
            if band == "low" and span[1] < min(when + _at_rank(low, high, rank),
                                               limit):
                truncated += 1
            per_victim.setdefault(victim, {}).setdefault(band, []).append(span)
            per_caster.setdefault(caster, {}).setdefault(band, []).append(span)
        if victim in rows:
            rows[victim]["blind_applications_received"] += 1
        if caster in rows:
            rows[caster]["blind_applications_caused"] += 1

    # Merged per player before summing: two Blinding Flashes three seconds apart
    # are one blind, not two, and a caused-seconds figure that adds them whole is
    # the same double count seen from the other side.
    for victim, bands in per_victim.items():
        if victim not in rows:
            continue
        for band, spans in bands.items():
            total = sum(end - start for start, end in _merge(spans))
            rows[victim][f"blind_seconds_modeled_{band}"] = round(total)
    for caster, bands in per_caster.items():
        if caster not in rows:
            continue
        for band, spans in bands.items():
            total = sum(end - start for start, end in _merge(spans))
            rows[caster][f"blind_seconds_caused_modeled_{band}"] = round(total)

    output: dict[str, list[dict[str, int]]] = {}
    for agent_id, (party_id, player_number) in players.items():
        output.setdefault(party_id, []).append(
            {"player_number": player_number, **rows[agent_id]})
    for party_rows in output.values():
        party_rows.sort(key=lambda row: row["player_number"])

    return {
        "schema": SCHEMA_VERSION,
        "players": output,
        "attribution": {
            "blind_applications_modeled": len(applications),
            "blind_casts_unmodelled": unmodelled,
            "blind_windows_truncated_by_removal": truncated,
            "blind_rank_low": RANK_LOW,
            "blind_rank_high": RANK_HIGH,
        },
    }
