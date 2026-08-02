#pragma once

// Divine Favor flat companion-heal bonus per rank: round(3.2 * rank), r in
// [0..16] (source: GWW).
//
// The game logs this as its OWN separate HEAL combat event, co-timed with the
// spell, on every Monk spell cast on an ally -- it is not baked into the
// spell's own heal value. That makes it unusually useful: a Monk's DF rank is
// a single small integer, constant for the match and shared across every ally
// they touch, so one rank explains packets aimed at eight different targets.
//
// Two passes consume this table from opposite directions, which is why it
// lives here rather than in either of them:
//   * MaxHpSolver inverts it   -- known bonus / observed fraction  -> max HP
//   * AttributeDeducer reads it -- observed absolute value          -> rank
inline constexpr int kDivineFavorBonus[17] = {
    0, 3, 6, 10, 13, 16, 19, 22, 26, 29, 32, 35, 38, 42, 45, 48, 51
};
