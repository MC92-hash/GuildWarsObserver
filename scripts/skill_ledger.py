"""Per-skill cast counts, rolled up from the cast lifecycle.

Nothing in the pipeline could answer "how many times did this player cast this
skill".  ``infos.json`` carries ``used_skills``, but that is a *set* built with
``s.used_skills.insert(...)`` -- presence, for reconstructing a bar, with no
counts.  The only per-skill counters that existed were four hand-picked ones in
``combat_analytics`` (``signet_casts``, ``sopr_casts``, ``coward_uses``,
``bulls_strike_uses``), each added for its own question.

This does not re-read the streams.  It rolls up the ``Cast`` history
``build_combat_analytics`` has already built, so a per-skill row cannot drift
from the ``casts_started`` / ``casts_completed`` / ``casts_interrupted`` totals
published beside it -- they are literally the same objects, grouped differently.
A parallel implementation reading the same files would be a second chance to get
the two field layouts below wrong.

**The two layout traps, both live in every recording.**

``SKILL_ACTIVATED`` is ``skill_id;caster;target``, but ``SKILL_FINISHED`` and
``SKILL_STOPPED`` are ``caster;0;target`` -- **the skill id is always 0 there**,
so an outcome cannot be attributed by reading it off the closing record.  It has
to come from pairing per caster, which is what the lifecycle pass does and why
this rides on it.  ``combat_analytics``'s ``cast.skill_id != skill_id`` guard
survives only because ``skill_id > 0`` is never true.

Attack skills are in ``attack_skill_events.txt.gz``, **not**
``skill_events.txt.gz``.  Distracting Shot and Savage Shot are invisible to a
reader that opens only the latter -- it returns zero rather than failing.

**Instant skills get uses and nothing else, in their own block.**  They never
become a ``Cast``: ``combat_analytics`` keeps them out of the lifecycle family
on purpose, because an instant cannot be cancelled or interrupted and counting
it would dilute every completion rate.  That is right, but ``skills`` is built
from the same history, so until now an instant reached no page at all --
measured over 80 matches, **64 skills appear only as instants and their 29,147
uses are 17.4% of every skill use in the archive**.  Every stance in the game
was missing.  Only signets and ``"Coward!"`` had ever been rescued, one
hand-written counter at a time.

**Attack skills get attempts and nothing else.**  ``ATTACK_SKILL_FINISHED``
fires far less often than ``ATTACK_SKILL_ACTIVATED`` (99 against 406 in one
measured match), so the pairing that works for spells does not work here: a
completion rate computed from it would read as a player cancelling three
quarters of their attacks.  Until that is understood, attempts are the only
honest number and the completion column is absent rather than wrong.
"""
from __future__ import annotations

from collections import Counter
from collections.abc import Iterable, Mapping

SCHEMA_VERSION = 1

# A skill a player used once in a match is nearly always a mis-parse, an
# NPC-borrowed skill or a PvP twin that did not fold -- but it is also a real
# res signet. Nothing is dropped; the floor exists only as a documented decision
# not to have one, so that a reader knows a 1 is a 1.
MIN_CASTS_PUBLISHED = 1


def _rollup(casts: Iterable, fold) -> dict[str, list[int]]:
    """skill id -> [started, completed, interrupted] for one player.

    ``interrupted`` is the lifecycle's own label, applied in the interrupt join
    after a stopped cast is matched to an ``INTERRUPTED`` on its caster -- it is
    not every stopped cast.  A cast stopped with no interrupt in window is a
    voluntary cancel, and counting the two together would turn every fake cast
    into an opponent's success.

    ``fold`` maps a PvP split id back to its base id.  Applied here rather than
    by the caller so that the three counters can never be grouped under
    different keys.
    """
    started: Counter[int] = Counter()
    completed: Counter[int] = Counter()
    interrupted: Counter[int] = Counter()
    for cast in casts:
        skill_id = fold(getattr(cast, "skill_id", 0))
        if skill_id <= 0:
            continue
        started[skill_id] += 1
        if cast.outcome == "completed":
            completed[skill_id] += 1
        elif cast.outcome == "interrupted":
            interrupted[skill_id] += 1
    return {
        str(skill_id): [count, completed[skill_id], interrupted[skill_id]]
        for skill_id, count in sorted(started.items())
        if count >= MIN_CASTS_PUBLISHED
    }


def build_skill_casts(players: Mapping[int, tuple[str, int]],
                      history: Mapping[int, list],
                      attack_attempts: Mapping[tuple[int, int], int] | None = None,
                      instant_uses: Mapping[tuple[int, int], int] | None = None,
                      canonicalise=None) -> dict:
    """Per-player, per-skill cast counts keyed by party id.

    Returned as its own block rather than folded onto the player rows, because
    ``stats_index.parse_shard`` copies only keys whose value ``isinstance(v,
    (int, float))`` -- a nested map on a player row is dropped silently, with no
    error anywhere to say the counters never arrived.
    """
    if not players:
        return {}

    fold = canonicalise or (lambda skill_id: skill_id)
    attacks_by_agent: dict[int, Counter[int]] = {}
    for (agent_id, skill_id), count in (attack_attempts or {}).items():
        if skill_id > 0:
            attacks_by_agent.setdefault(agent_id, Counter())[fold(skill_id)] += count
    instants_by_agent: dict[int, Counter[int]] = {}
    for (agent_id, skill_id), count in (instant_uses or {}).items():
        if skill_id > 0:
            instants_by_agent.setdefault(agent_id, Counter())[fold(skill_id)] += count

    out: dict[str, list[dict]] = {}
    for agent_id, (party_id, player_number) in players.items():
        # PvP twins are folded inside the rollup. Every credited Signet of
        # Pious Restraint in the archive came from the twin id and none from
        # the base, so an unfolded histogram splits one skill across two rows
        # that no consumer will ever add back together.
        skills = _rollup(history.get(agent_id, ()), fold)
        attacks = {str(skill_id): count
                   for skill_id, count in sorted(
                       attacks_by_agent.get(agent_id, Counter()).items())}
        instants = {str(skill_id): count
                    for skill_id, count in sorted(
                        instants_by_agent.get(agent_id, Counter()).items())}
        if not skills and not attacks and not instants:
            continue
        row: dict = {"player_number": player_number}
        if skills:
            row["skills"] = skills
        if attacks:
            # Named apart from `skills` so the shape difference is visible in
            # the JSON: three numbers there, one here, because an attack skill
            # has no trustworthy outcome. See the module docstring.
            row["attack_attempts"] = attacks
        if instants:
            # A third shape, and a third number, for the same reason as the
            # second: an instant skill has no cast to complete or be
            # interrupted during. Publishing `completed = started` for one
            # would be true and useless, and would quietly flatter anybody
            # running more stances if a reader ever averaged completion.
            row["instant_uses"] = instants
        out.setdefault(party_id, []).append(row)
    for party_rows in out.values():
        party_rows.sort(key=lambda row: row["player_number"])
    if not out:
        return {}
    return {"schema": SCHEMA_VERSION, "players": out}
