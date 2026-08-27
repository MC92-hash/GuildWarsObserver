"""Derive versioned, auditable combat analytics from a recording's StoC log.

The legacy ``infos.json`` counters describe outcomes but lose attribution and
timestamps.

**An ``INTERRUPTED`` record names only its victim.**  This docstring used to
claim the record carried "all three identities we need: victim, interrupted
skill, and interrupter".  It never has.  ``EventHooks.cpp`` writes the trailing
fields as the literal string ``;0;0`` -- see ``EXPORTS_CONVENTIONS.md``, which
documents the format as ``INTERRUPTED;agent_id;0;0`` -- and the packet behind it
(``GenericValueID::interrupted``, a ``GenericValue`` of value_id/agent_id/value)
has no interrupter in it to record.  Measured across 1,076 recordings, 53,827 of
53,827 interrupt events end in ``;0;0``.  ``rupts_landed`` is therefore
structurally unobservable and stays zero.

So the interrupter is **inferred**, and kept under its own name so that a join
is never mistaken for an observation.  Each interrupt is matched to a recent
interrupt-skill activation aimed at that victim; where exactly one player could
have caused it they are credited ``rupts_inferred``, and where two could have
nobody is.  Measured over 3,614 interrupts in sixty recordings: 80.3% credited,
2.2% ambiguous, 5.1% caused by a knockdown (already counted as one, so not
credited again) and 12.5% unexplained.  Every one of those figures is published
in ``attribution`` so a reader can see how much of a match the join reached.

Only facts supported by the event stream are emitted.  In particular, a
voluntary cancel is *not* labelled a fake cast: intent is not observable.
"""

from __future__ import annotations

import gzip
import re
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path


# NOT bumped for the inferred-interrupt work, deliberately. The consumer merges
# analytics only when the schema is exactly 1 and refuses unknown ones wholesale
# (watchtower stats_index.parse_shard), so a bump would discard every counter
# including the knockdowns that already work. New per-player counters need no
# bump: the shard parser copies whatever numeric keys a row carries.
SCHEMA_VERSION = 1
INTERRUPT_MATCH_EARLY_SECONDS = 0.5
INTERRUPT_MATCH_LATE_SECONDS = 3.0

# How long before an interrupt a rupt skill may have started and still be its
# cause. Split by delivery because a projectile has to fly: measured p50 deltas
# are 134 ms for spells and 304 ms for attack skills, and these bounds put the
# ambiguous share at 2.2% rather than the 3.3% a flat 3.5 s window gives.
RUPT_SPELL_WINDOW_SECONDS = 2.5
RUPT_ATTACK_WINDOW_SECONDS = 3.5

# A knockdown interrupts whatever the victim was casting, and unlike an
# interrupt its source IS recorded. Those are recognised so they can be
# subtracted from the unexplained residue -- but NOT credited as interrupts,
# because `knockdowns_dealt` already counts the same action and one action
# should not score on two axes.
KNOCKDOWN_INTERRUPT_WINDOW_SECONDS = 0.6

# Skills that interrupt, keyed by id, split by how they are delivered.
#
# Curated because the skill data carries no interrupt flag, then measured
# against sixty recordings -- the percentage is how often a cast of it was the
# unique cause of an interrupt on its target:
#
#   Cry of Frustration 75%   Leech Signet 68%   Complicate 67%   Power Leak 64%
#   Power Spike 61%   Power Leech 60%   Power Drain 60%   Distracting Blow 43%
#   Distracting Shot 42%   Savage Shot 41%   Signet of Distraction 40%
#   Savage Slash 38%
#
# Three plausible candidates were measured and REMOVED: Cruel Spear (980 casts,
# 0% -- it interrupts only a moving foe and the stream cannot see that), Chaos
# Storm (6%, an area effect with no target to join on) and Shame (1%, a hex).
# The rest are dedicated interrupts that simply did not appear in the sample;
# they can only ever add a correct attribution, and one that never fires shows
# up as nothing rather than as a wrong credit.
_RUPT_SPELLS: dict[int, str] = {
    5: "Power Block", 23: "Power Spike", 24: "Power Leak", 25: "Power Drain",
    57: "Cry of Frustration", 61: "Leech Signet", 803: "Power Leech",
    932: "Complicate", 979: "Mistrust", 1053: "Psychic Distraction",
    1057: "Psychic Instability", 1992: "Signet of Distraction",
}
_RUPT_ATTACKS: dict[int, str] = {
    325: "Distracting Blow", 329: "Skull Crack", 340: "Disrupting Chop",
    390: "Savage Slash", 399: "Distracting Shot", 408: "Concussion Shot",
    409: "Punishing Shot", 426: "Savage Shot", 445: "Disrupting Lunge",
    571: "Disrupting Dagger", 975: "Exhausting Assault", 1025: "Disrupting Stab",
    1198: "Broad Head Arrow", 1604: "Disrupting Throw", 1726: "Magebane Shot",
    2194: "Distracting Strike",
}


def _rupt_window(skill_id: int) -> float | None:
    """Seconds a cast of this skill may precede the interrupt it caused."""
    if skill_id in _RUPT_SPELLS:
        return RUPT_SPELL_WINDOW_SECONDS
    if skill_id in _RUPT_ATTACKS:
        return RUPT_ATTACK_WINDOW_SECONDS
    return None

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

# How long after a use its knockdown may still arrive. Measured p90 deltas are
# 784 ms for Hammer Bash and 798 ms for Bull's Strike, with every melee skill
# under 840 ms and Gale the slowest at 1,034 ms; past 2.4 s the deltas are
# coincidence, so 1.2 s keeps the real tail and cuts the noise.
KD_WINDOW_SECONDS = 1.2

# Knockdown attempts, keyed by id -> (name, baseline conversion in thousandths).
#
# A skill belongs here when its OWN use is the attempt: you use it, and a
# knockdown either follows or does not. Excluded are the families where the
# knockdown is fired by a separate later event, so the use is not the try --
# stances whose knockdown comes from a subsequent auto-attack (Bull's Charge,
# 1,319 uses, 4%), hexes and traps that knock down when they end or when the
# victim moves (Lightning Surge 0%, Wastrel's Collapse 10%, Scorpion Wire 0%,
# Fetid Ground 0%), and the anti-knockdown skills that only ever prevent one
# (Shield Bash 3%, Balthazar's Pendulum 0%).
#
# The baseline is how often a use was followed by a knockdown from the same
# agent, measured over 623 recordings. It is what the skill lands on average,
# so `kd_landed / kd_expected` reads the player instead of the build: a hammer
# warrior converts 75% because Hammer Bash converts 75%, not because they are
# good at it.
#
# Grouped by how a knockdown is joined back to a use:
#   EXACT   untargeted, resolves server-side in the same millisecond
#   TARGET  the knockdown lands on the declared target, inside the window
#   AREA    untargeted, hits whoever is nearby, inside the window
_KD_EXACT: dict[int, tuple[str, int]] = {
    869: ('"Coward!"', 402),                    # 14422 uses  40%
    170: ("Earthquake", 957),                   #    70 uses  96%
    891: ('"None Shall Pass!"', 91),            #    44 uses   9%
}
_KD_TARGET: dict[int, tuple[str, int]] = {
    331: ("Hammer Bash", 750),                  #  8016 uses  75%
    3185: ("Psychic Instability (PvP)", 150),   #  3705 uses  15%
    1057: ("Psychic Instability", 150),         #   PvP twin
    332: ("Bull's Strike", 333),                #  2762 uses  33%
    171: ("Stoning", 171),                      #  1519 uses  17%
    231: ("Shock", 345),                        #  1132 uses  34%
    2804: ("Mind Shock (PvP)", 742),            #  1089 uses  74%
    226: ("Mind Shock", 742),                   #   PvP twin
    354: ("Earth Shaker", 757),                 #  1024 uses  76%
    355: ("Devastating Hammer", 749),           #   977 uses  75%
    237: ("Water Trident", 82),                 #   974 uses   8%
    2808: ("Enraged Smash (PvP)", 289),         #   737 uses  29%
    993: ("Enraged Smash", 289),                #   PvP twin
    162: ("Gale", 321),                         #   386 uses  32%
    2011: ("Grapple", 251),                     #   383 uses  25%
    786: ("Iron Palm", 801),                    #   261 uses  80%
    777: ("Horns of the Ox", 793),              #   184 uses  79%
    784: ("Entangling Asp", 488),               #   162 uses  49%
    296: ("Bane Signet", 43),                   #   140 uses   4%
    2135: ("Trampling Ox", 714),                #    98 uses  71%
    3193: ("Signet of Clumsiness (PvP)", 276),  #    98 uses  28%
    1657: ("Signet of Clumsiness", 276),        #   PvP twin
    356: ("Irresistible Blow", 125),            #    80 uses  12%
    1767: ("Reaper's Sweep", 615),              #    78 uses  62%
    358: ("Backbreaker", 746),                  #    71 uses  75%
    1697: ("Magehunter's Smash", 732),          #    56 uses  73%
    1146: ("Shove", 553),                       #    47 uses  55%
    327: ("Griffon's Sweep", 65),               #    46 uses   7%
    # Dedicated warrior knockdowns too rare to measure -- 15 and 12 uses in 623
    # recordings. Carried so the attempt count stays complete, at the pooled
    # rate rather than a baseline the sample cannot support.
    1410: ("Overbearing Smash", 452),           #    15 uses
    359: ("Heavy Blow", 452),                   #    12 uses
}
_KD_AREA: dict[int, tuple[str, int]] = {
    843: ("Gust", 430),                         #  2795 uses  43%
    163: ("Whirlwind", 232),                    #   224 uses  23%
}

# One more exact-millisecond source is in the recordings and is NOT in the
# table: skill 3456, 114 uses, 44% conversion, every hit in the same
# millisecond -- the Coward! signature exactly. The bundled skill data stops at
# id 3431, so it cannot be named, and an unnamed skill is not something to
# credit. Counted in the audit as `kd_attempts_unknown` so the gap stays
# visible and closes itself when the skill data is refreshed.
_KD_UNKNOWN_EXACT = frozenset({3456})

_KD_ATTEMPT_KINDS = (
    "SKILL_ACTIVATED", "ATTACK_SKILL_ACTIVATED", "INSTANT_SKILL_USED",
)

_KD_TIERS: tuple[tuple[str, dict[int, tuple[str, int]]], ...] = (
    ("exact", _KD_EXACT), ("target", _KD_TARGET), ("area", _KD_AREA),
)


def _kd_spec(skill_id: int) -> tuple[str, int] | None:
    """(tier, baseline in thousandths) for a knockdown attempt, or None."""
    for tier, table in _KD_TIERS:
        entry = table.get(skill_id)
        if entry is not None:
            return tier, entry[1]
    return None


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
            # Unobservable: the packet carries no interrupter. Kept, and kept
            # zero, so that if the recorder ever gains a real source the two can
            # be compared instead of one silently replacing the other.
            "rupts_landed": 0,
            "rupts_inferred": 0,
            "rupt_cast_progress_ms_sum": 0,
            "rupt_cast_progress_n": 0,
            "knockdowns_dealt": 0,
            "knockdowns_received": 0,
            "coward_uses": 0,
            "coward_kds": 0,
            "bulls_strike_uses": 0,
            "bulls_strike_target_unknown": 0,
            # Every knockdown skill on the bar, not just the two named ones.
            # `kd_expected_milli` is the sum of each attempt's baseline, so
            # landed/expected says whether this player beats what their own
            # skills land on average.
            "kd_attempts": 0,
            "kd_landed": 0,
            "kd_expected_milli": 0,
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
    rupt_inferred_unique = 0
    rupt_inferred_ambiguous = 0
    rupt_inferred_none = 0
    rupt_from_knockdown = 0
    # (started_at, skill_id, caster_id, target_id) for every interrupt-capable
    # cast aimed at somebody, and every knockdown, collected in pass one and
    # joined in pass two -- the same two-pass shape the cast lifecycle uses.
    rupt_casts: list[tuple[float, int, int, int]] = []
    knockdowns: list[tuple[float, int, int]] = []
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
            # The third field is who it was aimed at, and it is the join key the
            # interrupt inference needs. Absent for self- and party-targeted
            # skills, which cannot interrupt anybody anyway.
            target_id = _integer(event.fields[2]) if len(event.fields) >= 3 else 0
            if (agent_id in rows and target_id
                    and _rupt_window(skill_id) is not None):
                rupt_casts.append((event.time, skill_id, agent_id, target_id))
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
        elif event.kind == "ATTACK_SKILL_ACTIVATED" and len(event.fields) >= 3:
            # Same layout as SKILL_ACTIVATED. Collected only for the interrupt
            # join -- attack skills stay outside the cast lifecycle, which is
            # about spell cancellation.
            skill_id = _integer(event.fields[0])
            agent_id = _integer(event.fields[1])
            target_id = _integer(event.fields[2])
            if (agent_id in rows and target_id
                    and _rupt_window(skill_id) is not None):
                rupt_casts.append((event.time, skill_id, agent_id, target_id))
        elif event.kind == "KNOCKED_DOWN" and len(event.fields) >= 2:
            # Collected here rather than where the counters are incremented, so
            # that a knockdown is on record before the interrupt it caused is
            # judged. The two share a millisecond routinely.
            kd_victim = _integer(event.fields[0])
            kd_source = _integer(event.fields[1])
            if kd_victim and kd_source:
                knockdowns.append((event.time, kd_victim, kd_source))
        elif event.kind == "INSTANT_SKILL_USED":
            # Instant skills cannot be cancelled and would only dilute every
            # lifecycle rate, so they are intentionally outside this family.
            continue
    for agent_id in tuple(open_casts):
        close_other(agent_id)

    def _credit_interrupt(when: float, victim_id: int) -> None:
        """Work out who interrupted ``victim_id``, and credit them if it is certain.

        A knockdown is checked first and, when it explains the interrupt, ends
        the search WITHOUT crediting anybody: the knockdown is already counted
        in ``knockdowns_dealt``, and scoring one action on two axes would
        inflate whatever reads them together.

        Otherwise every interrupt-capable cast aimed at this victim inside its
        skill's window is a candidate. Exactly one caster credits them; two
        credits nobody. Guessing between two would produce a number that is
        right on average and wrong about every individual, which is the kind of
        figure people quote.
        """
        nonlocal rupt_inferred_unique, rupt_inferred_ambiguous
        nonlocal rupt_inferred_none, rupt_from_knockdown

        sources = {
            source for kd_time, kd_victim, source in knockdowns
            if kd_victim == victim_id
            and -0.1 <= when - kd_time <= KNOCKDOWN_INTERRUPT_WINDOW_SECONDS
        }
        if len(sources) == 1:
            rupt_from_knockdown += 1
            return

        casters = {
            caster for started, skill_id, caster, target in rupt_casts
            if target == victim_id and caster != victim_id
            and 0 <= when - started <= (_rupt_window(skill_id) or 0)
        }
        if len(casters) == 1:
            caster = next(iter(casters))
            if caster in rows:
                rows[caster]["rupts_inferred"] += 1
                rupt_inferred_unique += 1
                return
        if casters:
            rupt_inferred_ambiguous += 1
        else:
            rupt_inferred_none += 1

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
            _credit_interrupt(event.time, victim_id)
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

    # Every knockdown attempt claims from one pool of knockdowns, tightest
    # evidence first: same-millisecond untargeted, then target-matched inside
    # the window, then untargeted area. A knockdown is claimable once, and when
    # two attempts could equally claim it NEITHER is credited -- the refusal
    # the interrupt inference already makes. That is what makes this safe where
    # a per-skill window guess was not: an ambiguous case becomes an audited
    # number instead of landing on whichever attempt happened to sort first.
    attempts: list[dict] = []
    kd_attempts_unknown = 0
    for event in ordered:
        if event.kind not in _KD_ATTEMPT_KINDS or len(event.fields) < 3:
            continue
        skill_id = _integer(event.fields[0])
        source_id = _integer(event.fields[1])
        if not source_id:
            continue
        if skill_id in _KD_UNKNOWN_EXACT:
            kd_attempts_unknown += 1
            continue
        spec = _kd_spec(skill_id)
        if spec is None:
            continue
        tier, baseline = spec
        target_id = _integer(event.fields[2])
        attempts.append({
            "time": event.time, "ms": _event_ms(event), "skill": skill_id,
            "source": source_id, "target": target_id, "tier": tier,
            "landed": False,
        })
        if source_id not in rows:
            continue
        rows[source_id]["kd_attempts"] += 1
        rows[source_id]["kd_expected_milli"] += baseline
        if skill_id == COWARD_ID:
            rows[source_id]["coward_uses"] += 1
        elif skill_id == BULLS_STRIKE_ID:
            rows[source_id]["bulls_strike_uses"] += 1
            if target_id <= 0:
                rows[source_id]["bulls_strike_target_unknown"] += 1

    consumed_kds: set[int] = set()
    ambiguous_kds: set[int] = set()

    def _kd_matches(attempt: dict, when: float, victim: int, source: int) -> bool:
        if attempt["source"] != source:
            return False
        if attempt["tier"] == "exact":
            return round(when * 1000) == attempt["ms"]
        if not 0 <= when - attempt["time"] <= KD_WINDOW_SECONDS:
            return False
        return attempt["tier"] == "area" or attempt["target"] == victim

    for tier, _table in _KD_TIERS:
        pool = [attempt for attempt in attempts if attempt["tier"] == tier]
        if not pool:
            continue
        for index, (kd_time, kd_victim, kd_source) in enumerate(kd_events):
            if index in consumed_kds or index in ambiguous_kds:
                continue
            # An area skill knocks several people down at once, so it stays
            # claimable after it lands; a single-target one is spent on the
            # knockdown it made.
            candidates = [
                attempt for attempt in pool
                if (tier == "area" or not attempt["landed"])
                and _kd_matches(attempt, kd_time, kd_victim, kd_source)
            ]
            if not candidates:
                continue
            if len(candidates) > 1:
                ambiguous_kds.add(index)
                continue
            winner = candidates[0]
            consumed_kds.add(index)
            if winner["landed"] or winner["source"] not in rows:
                continue
            winner["landed"] = True
            rows[winner["source"]]["kd_landed"] += 1
            if winner["skill"] == COWARD_ID:
                rows[winner["source"]]["coward_kds"] += 1

    for agent_id, casts in history.items():
        rows[agent_id]["casts_cancelled_voluntary"] += sum(
            cast.outcome == "stopped" for cast in casts
        )

    output: dict[str, list[dict[str, int]]] = {}
    for agent_id, (party_id, player_number) in players.items():
        row = {"player_number": player_number, **rows[agent_id]}
        # The expectation is a sum of fractional baselines, so it is carried in
        # thousandths; the numerator is scaled to match here rather than stored
        # twice, so the two cannot drift apart.
        row["kd_landed_milli"] = row["kd_landed"] * 1000
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
            # How far the inference reached. Published so a reader can see the
            # share of a match it explained rather than trusting the counter.
            "interrupt_inferred_unique": rupt_inferred_unique,
            "interrupt_inferred_ambiguous": rupt_inferred_ambiguous,
            "interrupt_inferred_none": rupt_inferred_none,
            "interrupt_from_knockdown": rupt_from_knockdown,
            "rupt_spell_window_ms": round(RUPT_SPELL_WINDOW_SECONDS * 1000),
            "rupt_attack_window_ms": round(RUPT_ATTACK_WINDOW_SECONDS * 1000),
            "match_early_ms": round(INTERRUPT_MATCH_EARLY_SECONDS * 1000),
            "match_late_ms": round(INTERRUPT_MATCH_LATE_SECONDS * 1000),
            "knockdown_events": knockdown_events,
            "knockdown_events_untracked_source": knockdown_untracked_source,
            "knockdown_events_untracked_victim": knockdown_untracked_victim,
            "conditional_kd_events_assigned": len(consumed_kds),
            "conditional_kd_events_unassigned": len(kd_events) - len(consumed_kds),
            "coward_match_tolerance_ms": 0,
            # How far the knockdown ledger reached, same shape as the interrupt
            # audit above: attempts seen, knockdowns it explained, knockdowns it
            # refused because two attempts could equally claim them.
            "kd_attempt_events": len(attempts),
            "kd_attempts_unknown_skill": kd_attempts_unknown,
            "kd_events_claimed": len(consumed_kds),
            "kd_events_ambiguous": len(ambiguous_kds),
            "kd_events_unclaimed": (
                len(kd_events) - len(consumed_kds) - len(ambiguous_kds)
            ),
            "kd_window_ms": round(KD_WINDOW_SECONDS * 1000),
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
        # Most knockdown skills ARE attack skills, so without that stream the
        # attempts are undercounted while the knockdowns they caused are still
        # counted -- a conversion rate well above what the player managed.
        # Withheld entirely rather than published wrong.
        for party_rows in result["players"].values():
            for row in party_rows:
                row.pop("bulls_strike_uses", None)
                row.pop("bulls_strike_target_unknown", None)
                row.pop("kd_attempts", None)
                row.pop("kd_landed", None)
                row.pop("kd_landed_milli", None)
                row.pop("kd_expected_milli", None)
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
