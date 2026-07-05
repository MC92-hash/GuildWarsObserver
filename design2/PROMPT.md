# Prompt for Claude Code — port the Match Browser redesign

Paste the following to Claude Code, run from the repo root:

---

Read `design/HANDOFF.md` and `design/Match Browser.dc.html`.

`Match Browser.dc.html` is a **visual reference** (HTML/CSS) for a redesigned match card. It is
NOT code to copy — it cannot run in our native app. Translate the design into our ImGui renderer.

Task: port the redesigned browse-view match card into the card gallery in
`SourceFiles/draw_replay_browser.cpp`.

Constraints:
- Use the existing **Watchtower** theme only — `ApplyBrowserTheme(1)` and the `kCard*` / `kColor*`
  statics. Do NOT introduce any color outside that palette.
- Reuse `GetProfessionIcon(profId)` for the profession icons (do not re-load textures).
- Match the card structure in the reference: map-thumbnail + name + date / duration + rank badge
  in the head; `left team | VS | right team` matchup body; `Details ›` footer.
- Profession comp: icon + count, ordered melees-first
  (`W, D, A, R, P` then `E, N, Me, Mo, Rt`) — see the mapping in the handoff.
- No text truncation on team names or guild tags; team name then `[tag]` on both sides.
- Rank tier ramp and legend as specified (amber intensity, no gold/silver/bronze).

Before writing any code:
1. List the concrete changes you plan to make (functions/sections you'll touch).
2. Ask me the two open questions from the handoff:
   - Is the tier order A > B > C correct?
   - Where does the real map screenshot come from to fill the map thumbnail?

---
