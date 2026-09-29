# GW Observer Wardrobe: product and UX design

Status: SUPERSEDED BY THE REDESIGN. The MVP this document describes was implemented on 2026-09-23;
it was then redesigned to five approved mockups and rebuilt in stages R1-R4, finished on 2026-09-24
(uncommitted). Read gwobserver-private-dev/docs/WARDROBE_REDESIGN.md first: it is the current
specification and wins wherever the two differ (one editor with Character and Armor sections
instead of the Create and Dress screens, painted backdrops, a height slider, a dye mixture).
gwobserver-private-dev/docs/WARDROBE_IMPLEMENTATION.md is the engineering record. Of sections 5.3
and 6 below, height and backdrops now exist (see the redesign); everything else there is STILL OPEN.

Decisions from review (2026-09-23): the Wardrobe is a STANDALONE character-creation sub-application, not a Library mode and not linked to matches, filters or Scout. At launch the app will later open on a 3D hub where the user picks either the Match Library or the Wardrobe; today's Library button is a stand-in for that hub tile. All Wardrobe UI and data code lives in `gwobserver-private-dev`, exactly as the armour rendering for match replays does; the public repo carries only the switch. Everything is built in the dev environment. Companion to `HANDOFF.md` (the card redesign), which fixes the palette this document reuses.

Grounding: the "Library" is the replay browser (`SourceFiles/draw_replay_browser.cpp`). Scout is a boolean on `BrowserState` toggled from the gallery toolbar, not a separate screen. The private dev Wardrobe (`gwobserver-private-dev/SourceFiles/Character/draw_character_builder.cpp`) already knows how to dress a character and is compiled into the Observer but unreachable. This design is the consumer front end for that engine.

---

## 1. Product vision

**What it is.** The Wardrobe is where a player dresses a Guild Wars character and keeps the looks they like. Pick a profession, a face, a hairstyle and a skin tone. Try any armour from any campaign, dye it, save the look, come back to it later. Everything renders with the same character engine that draws real players in replays, so what you see is what the game shows.

**Why people use it.**
- Fashion is a game of its own in Guild Wars. People plan armour and dye combinations before spending in game, and there is no good way to preview a full set with dyes outside the game.
- Nostalgia. Recreating an old character or a look from a memorable match is rewarding in itself.
- Ownership. A collection of your own characters, saved on your device, rendered the way the game renders them.

**How it fits GW Observer.** GW Observer becomes two sub-applications sharing one engine, one palette and one launcher: the Match Library, and the Wardrobe. The Wardrobe is only character creation. It does not filter matches, does not browse match data, and has no Scout-style overlap with the Library. The one link between the two runs the other way: in a replay, the character panel can export a player's look into the Wardrobe as a new character. The Wardrobe receives; it never reaches into matches. Later, the app opens on a 3D hub where the user walks up to either. Until the hub exists, the Library carries a single Wardrobe button that plays the hub's role.

**Design stance.** One accent colour, generous cards, instant results, no jargon. Words the UI uses: Wardrobe, character, look, wearing, armour, dye. Words it never uses: composite, mesh, preset, JSON, campaign row, DAT, texture.

---

## 2. User flow

### 2.1 Entry: an app switch, not a mode

The Wardrobe is a sibling of the replay browser, not a state inside it. Model it that way from day one so the hub can take over later without a rewrite:

```
draw_ui.cpp:   AppScreen { Library, Wardrobe }   (Hub joins this enum later)
```

Today's placement, until the hub exists:

```
MATCHES (129) [refresh]   sort v   Table | Cards | Scout                          [ Wardrobe ]
```

- One button at the far right of the gallery toolbar and of the table header, visually distinct from the Scout toggle: an outlined secondary button with a hanger icon, never the amber mode style. It switches the app, it does not toggle a filter.
- The same entry in the main menu ("File > Wardrobe"), which stays when the hub replaces the button.
- Tooltip: "Wardrobe - create a character and dress it".
- No recent-characters strip on the Library. Those chips belong on the hub's Wardrobe tile later.

**Hub-readiness.** The Wardrobe's top-level breadcrumb is a single "< Back" target named by the caller: "Library" today, "Hub" later. The Wardrobe never assumes what is behind it.

### 2.2 Screens and navigation

Three screens, one level deeper each. Breadcrumbs are the only navigation you need, plus Esc.

```
Library  ──Wardrobe──▶  Your characters  ──open / create──▶  Create a character  ──Choose armor──▶  Dress
  ▲                        │  "< Library"                        │  "< Your characters"                │  "< Your characters"
  └────────────────────────┘                                     └─────────────────────────────────────┘
```

- **Your characters** is the Wardrobe's home. Its header reads "< Library | WARDROBE" today and "< Hub | WARDROBE" later.
- **Create a character** and **Dress** get a focused header with "< Your characters". Depth is signalled by removing chrome, not adding it.
- **Edit appearance** in the Dress header returns to the Create screen for that character with everything filled in.
- Esc goes up one level everywhere. It never loses work, because identity and looks autosave as drafts.
- The Wardrobe never launches replays and never reads the match library. The two sub-applications share the window, the engine and the settings folder, nothing else. The replay's character panel may push a look INTO the Wardrobe (see the roadmap); nothing flows back.

### 2.3 Main workflows, counted in clicks

| Workflow | Clicks from the Library |
|---|---|
| Create a character and see it dressed in a full set | Wardrobe, Create character, profession, body, face, Choose armor, Full sets, set: 8 |
| Re-dye a saved character's chest piece | Wardrobe, character card, Chest row, swatch: 4 |
| Put a whole set on and save it as a new look | Wardrobe, card, Full sets, set, Save look dropdown, Save as new: 6 |
| Favourite a character | star on the card: 1 |

Full sets are what keep these numbers low. They are MVP.

### 2.4 Saving model

- **Identity autosaves.** Name, profession, face and hair are written as you change them. The header shows "Draft saved" for two seconds after each write, as in the mock.
- **Looks have a draft and a saved state.** Editing a look changes its draft immediately, and the viewport reflects it. "Save look" commits the draft, renders the thumbnail, and clears the "Unsaved changes" label. Leaving the screen keeps the draft. Reopening shows the draft with "Unsaved changes" and a "Revert to saved" entry in the Save dropdown. No blocking dialog on exit, ever.
- **Delete** is the only destructive action and the only confirmation modal: "Delete Aveline and her 4 looks? This can't be undone." Uses the existing `BeginPopupModal` pattern.
- Everything lives under `<exe dir>/settings/wardrobe/`, beside `ratings.json`. The Wardrobe never sends anything anywhere.

---

## 3. UI and UX recommendations

### 3.1 Visual integration: the rules

1. **Watchtower tokens only.** `kColorBg #18181b`, card `#1c1c1f`, inset `#161618`, borders `#27272a` / `#3f3f46`, accent `#f59e0b`, accent bright `#fbbf24`, text `#e4e4e7`, dim `#a1a1aa`, faint `#71717a`. The mock-ups use a warmer gold and a serif display face. Do not port those. Read colours through the tokens and `s_themeColors` so a future theme change carries the Wardrobe with it.
2. **Amber is the only accent** and it means "selected or active": the selected card border, the ✓ badge on a chosen face or piece, the active tab, the primary button, the filled favourite star, caps section labels. Nothing decorative is amber.
3. **Type scale, reusing the browser's.** Caps section labels 15 px bold amber ("WARDROBE", "COLLECTION", "PROFESSION"), like "MATCHES". Screen title 22 px bold text ("Your characters", "Create a character"), the one new size. Card name 17 px 600. Subtitles and helper text 14 px dim. Numbers and codes mono, words not.
4. **Geometry, reusing the browser's.** Spacing in multiples of 8. Panels 6 px radius, cards 8 px, buttons and inputs 4 px, chips and pills 10 px. Frame border 1 px. Card grid gap 12. Card padding 12.
5. **Cards are the browser's card.** Same background, idle border, amber selected border, amber hover fill at 6 percent. Single click selects, double click opens, exactly as match cards.
6. **Search and filters are the browser's widgets.** `InputTextWithHint` with the 4 px frame, the multi-select chip filter for campaigns, `ComboFromVec` for sort.
7. **Stars are `MatchRatings::DrawStarFilled / DrawStarOutline`**, one star as a favourite toggle, not five.
8. **One shared UI kit before any screen is built.** `PushGlassTheme`, the `ViewBtn` lambda, the card frame and the star helpers are all `static` inside one .cpp today. Extract them into `SourceFiles/ui/ui_kit.h`: `SectionLabel`, `PrimaryButton`, `SecondaryButton`, `SegmentedButton`, `CardFrame`, `SearchBox`, `ChipFilter`, `FavouriteStar`, `PushGlassTheme`. The browser keeps its pixels. The Wardrobe stops copying styles. This is the single most important integration step.

### 3.2 Visual hierarchy

Each screen has one primary action, always amber, always bottom-right or top-right: "Create character", "Choose armor", "Save look". Everything else is outlined or text. The viewport is the visual centre of Create and Dress. Side panels are darker inset panels, so the character reads as the brightest thing on screen.

### 3.3 Progressive disclosure

Shown by default: what nine out of ten users change. Under a disclosure row ("▸ More appearance options", "▸ Mix dyes", "▸ Filters"): the rest.

| Always visible | Behind a disclosure |
|---|---|
| Profession, body, face, skin tone, hair style, hair colour | Campaign of face and hair, facial hair |
| Five armour slots, hide headgear, one dye per piece, apply to all | The four dye channels ("Mix dyes"), costume and festival hat |
| Search, campaign dropdown, slot tabs | Set-name filter, "only pieces I have not tried" |

### 3.4 Empty states

Every empty state has a heading, one sentence, and one button. Copy is warm, never technical.

- **No characters yet** (Your characters): "No characters yet. Familiar faces, new possibilities." Button: Create character.
- **No favourites**: "Star a character to keep it here." No button.
- **Search found nothing**: "Nothing matches 'xyz'." Link: Clear search.
- **No saved looks**: the looks strip shows only the "+" tile with the caption "Save this look".
- **Game files not found** (any Wardrobe screen when the DAT is unavailable): "The Wardrobe needs your Guild Wars installation." Button: Locate Guild Wars, opening the existing settings path picker. The screen keeps its frame so the user knows where they are.

### 3.5 Loading states

Loads are small but not free: the engine reads icons from the game archive a few per frame and services one character load per frame.

- **The viewport never goes blank.** While a piece loads, the previous character stays. A 16 px amber ring spins in the viewport's bottom-left with "Fitting Elite Kurzick…". Fade the new character in over 150 ms.
- **Piece tiles fill in progressively.** A tile without its icon draws as a shimmering inset panel with the name underneath, then the icon appears. The existing text-tile fallback becomes a shimmer.
- **Face and hair thumbnails are rendered on the device** in the current skin and hair colour, cached as PNG under `settings/wardrobe/cache/`. First open of a face gallery shows shimmer tiles that resolve in a second or two; later opens are instant.
- **Collection cards** load their thumbnails from PNG. If a character has no thumbnail yet, the card shows a large dim profession icon on the inset panel.
- **Save look** shows "Saving…" in the header for the render, then "Saved".

### 3.6 Responsive behaviour

Reuse `LayoutMode` and its breakpoints.

| Mode | Width | Your characters | Dress |
|---|---|---|---|
| Full | > 1600 | Rail 220 + grid of 3 or 4 | Wearing 320 · viewport · Choose armor 480, armour grid 2 columns |
| Compact | 1200 to 1600 | Rail 220 + grid of 3 | Wearing 280 · viewport · Choose armor 400 |
| Narrow | 800 to 1200 | Rail collapses to a 40 px icon rail, grid of 2 | Wearing becomes a 56 px slot rail. Choose armor becomes a slide-over drawer opened by tapping a slot; the viewport takes the width |
| Mobile | ≤ 800 | Grid of 1 | Viewport on top, slot chips below, drawer for pieces |

Card width never drops below 240. The viewport keeps a 3:4 minimum box and recreates its render target on resize, as the PiP does. Splitter positions persist through `GuiGlobalConstants` like the browser's.

### 3.7 Accessibility

- **Keyboard.** ImGui keyboard navigation is already on. Every card, tile, swatch and tab is a focusable item with a visible amber focus ring. Arrow keys move inside galleries, Enter applies, Esc goes up. Shortcuts: Ctrl+Z / Ctrl+Y undo and redo, Ctrl+S save look, F favourite, 1 to 5 jump to the slot tabs, R reset the view. List them in the existing Shortcuts modal.
- **Never colour alone.** The chosen dye is named in text beside the swatches ("Purple"). Every swatch has a tooltip with its name. The selected face, piece and swatch carry a ✓ badge, not just an amber border. The hidden head slot shows an eye-slash icon and the word "Hidden".
- **Contrast.** Amber on zinc-950 and dim text on zinc-950 both clear 4.5:1. Faint text is reserved for hints.
- **Font size setting.** Row heights and rails derive from the frame height, not fixed pixels. The browser's 215 px cards do not scale with the font; the Wardrobe's must.
- **Motion.** Fades only, 150 to 200 ms, and none if a "reduce motion" preference is set. The idle animation in the viewport is the only continuous motion and can be paused from the viewport toolbar.
- **Honest limit.** Dear ImGui exposes nothing to screen readers. Say so in the docs rather than implying otherwise.

### 3.8 Making it fun

- Every click shows its result instantly on the character. No apply button anywhere.
- Full sets dress the whole character in one click.
- Undo and redo make experimenting free.
- The character idles in its profession animation, as the replay portrait does, and can be turned with a drag.
- The looks strip is a small gallery of your own work, and the collection grid fills with it over time.
- Saving renders a proper thumbnail, so the collection grid fills with your characters, not placeholders.

---

## 4. Screen designs

### 4.1 Your characters

```
┌ Wardrobe header ────────────────────────────────────────────────────────────────────────────┐
│ < Library  │  WARDROBE                                                      [ + Create character ]│
├────────────┬──────────────────────────────────────────────────────────────────────────────────┤
│ COLLECTION │ Your characters                                                                  │
│ ▣ All   6  │ Familiar faces. New possibilities.                                               │
│ ☆ Favs  3  │ [🔍 Search characters…                       ]            [Recently edited ▾]     │
│ ◷ Recent   │ ┌──────────┐ ┌──────────┐ ┌──────────┐                                          │
│            │ │  ★       │ │       ☆  │ │       ☆  │      portrait 3:4, inset panel           │
│ PROFESSION │ │ portrait │ │ portrait │ │ portrait │      name 17 px 600                       │
│ [All     ▾]│ │          │ │          │ │          │      "Mesmer · 4 looks" 14 px dim         │
│            │ │ Aveline ⋯│ │ Kael   ⋯ │ │ Sora   ⋯ │      ⋯ opens: Open, Rename, Duplicate,    │
│            │ └──────────┘ └──────────┘ └──────────┘               Favourite, Delete            │
└────────────┴──────────────────────────────────────────────────────────────────────────────────┘
```

- Left rail is the browser's filter panel style, collapsible to the 40 px rail below Narrow.
- Sort options: Recently edited, Name, Profession, Created.
- The profession filter is one combo. A second row of ten icon toggles is denser than the count of characters justifies.
- Double click or Enter opens the Dress screen with the character's default look. Single click selects.
- The overflow menu is the ribbon's `##more` popup style.

### 4.2 Create a character

```
│ ← Your characters │ WARDROBE                                                   Draft saved │
│ Create a character                                                                         │
│ ┌ identity panel 600 ─────────────────┐ ┌ viewport ─────────────────────────────────────┐ │
│ │ Name      [Aveline              ]   │ │                          [ Full body | Face ]  │ │
│ │ PROFESSION                          │ │                                                │ │
│ │ [W][R][Mo][N][Me]  10 icon tiles    │ │              character, idle                   │ │
│ │ [E][A][Rt][P][D]   ✓ on chosen      │ │                                                │ │
│ │ [ Body & face | Hair ]  tabs        │ │                                                │ │
│ │ Body   [♀ ✓] [♂]                    │ │                                                │ │
│ │ Face   ○ ○ ○ ○ ○ ○  rendered tiles  │ │                                                │ │
│ │ Skin   ■ ■ ■ ■ ■ ■ ■ ■  swatches    │ │   [↺] [ Front ] [↻]  │  [−][+]  [Reset view]  │ │
│ │ ▸ More appearance options           │ └────────────────────────────────────────────────┘ │
│ └─────────────────────────────────────┘                                                    │
│ [ ← Back ]                                                              [ Choose armor → ] │
```

- Profession tiles use the game's icons already loaded by `GetProfessionIcon`, in game id order.
- Face tiles are rendered head crops, in the current skin tone, so changing skin tone re-tints the gallery.
- The Hair tab holds style tiles and hair colour swatches. Facial hair appears there for a male body when the profession has options.
- "More appearance options" reveals the campaign chips for face and hair, with the note "Armour is not limited by this choice", and nothing else in MVP.
- **No height slider in MVP.** The scale rule is not solved. A slider that changes nothing, or guesses, would be worse than none. Reserve the row.
- Full body and Face are two camera presets. The arrows turn by 45 degrees. The label between them names the facing. Drag rotates, scroll zooms, as the hint under the Dress viewport says.

### 4.3 Dress

```
│ ← Your characters │ WARDROBE          Aveline · Mesmer · Edit appearance     [☆] [ Save look ▾ ] │
│ ┌ Wearing 320 ─────────┐ ┌ viewport ───────────────────────────┐ ┌ Choose armor 480 ─────────┐ │
│ │ WEARING              │ │ [Evening in violet ▾] Unsaved · ↶ ↷ │ │ [ Pieces | Full sets ]     │ │
│ │ ▣ Head   Hidden   👁 │ │                                     │ │ [🔍 Search chest armor… ]  │ │
│ │ ▣ Chest  Elite Kurz ◀│ │                                     │ │ [All campaigns ▾] [Filters]│ │
│ │ ▣ Hands  Canthan     │ │            character                │ │ [Head][Chest][Hands][Legs] │ │
│ │ ▣ Legs   Elite Kurz  │ │                                     │ │ ┌────────┐ ┌────────┐      │ │
│ │ ▣ Feet   Asuran      │ │                                     │ │ │ ✓      │ │        │      │ │
│ │ DYE · CHEST          │ │                                     │ │ │  icon  │ │  icon  │      │ │
│ │ ○ ■ ■ ■ ■ ■          │ │                                     │ │ │Elite Ku│ │Kurzick │      │ │
│ │ ■ ■ ■ ■ ■ ■  Purple  │ │ [<][ Front ][>] │ [−][+] [Reset]   │ │ └────────┘ └────────┘      │ │
│ │ ☐ Apply to all pieces│ │  Drag to rotate · Scroll to zoom    │ │  …grid, campaign headings  │ │
│ │ ▸ Mix dyes           │ └─────────────────────────────────────┘ │                            │ │
│ │ SAVED LOOKS          │                                         │  Click a piece to try it on│ │
│ │ [▣][▣][▣][ + ]       │                                         └────────────────────────────┘ │
│ └──────────────────────┘                                                                        │
```

**Wearing panel.** Five slot rows: icon, slot name, piece name. The selected row has the amber border and drives both the dye section and the slot tab on the right. The Head row carries the eye toggle: hidden headgear stays equipped, as in the game. Costume and festival hat rows appear only when "More slots" is expanded, and only after MVP.

**Dye section.** First swatch is "Original", the piece's undyed colour. Then the twelve game dyes in dye-trader order: Blue, Green, Purple, Red, Yellow, Brown, Orange, Gray, Black, White, Silver, Pink. The chosen name is written beside the grid. "Apply to all pieces" copies the dye to every worn slot in one click. "Mix dyes" expands to four channel slots, each with the same palette and a result swatch, which is the game's own limit.

**Viewport.** Look name dropdown at top-left lists this character's looks and "New look". "Unsaved changes" appears in dim text when the draft differs from the saved look. Undo and redo are history over the selection, not over the render, so they are instant. Camera controls sit inside the viewport's bottom edge on a translucent strip, mirroring the mock.

**Choose armor panel.** Two tabs. Pieces: one slot at a time, tabs synced with the Wearing selection, tiles with the game's own inventory icon and the set name, grouped under campaign headings when "All campaigns" is chosen. Full sets: one tile per set showing all five icons in a row plus the set name, one click dresses every slot and keeps existing dyes. Filters, behind the button: campaign chips, "elite only", "hide sets already worn". Hover shows a tooltip with set, campaign and the slot name. No hover try-on; each try-on is a load and a hover storm would stutter.

**Saved looks strip.** Thumbnails rendered at save time. Click loads that look. "+" saves the current draft as a new look and asks for a name inline, with a suggested name like "Look 4". Right-click or ⋯: Rename, Duplicate, Set as default, Delete.

**Header.** Star toggles favourite. "Save look" is the primary button. Its dropdown: Save as new look, Rename look, Revert to saved, Delete look. "Edit appearance" is a text link back to the Create screen.

### 4.4 Chip and card specifications

| Element | Size | Notes |
|---|---|---|
| Character card | width from grid, 3:4 portrait plus 56 px footer | star top-right, ⋯ bottom-right, both hidden until hover unless set |
| Face tile | 72 × 72 | 6 px radius, ✓ badge top-right when chosen |
| Piece tile | 200 × 180 in Full, 160 × 150 in Compact | icon 96 px on inset, name centred below, 2-line clamp, never truncated mid-word |
| Dye swatch | 44 × 44 | 6 px radius, ✓ on chosen, 1 px border, tooltip with the name |
| Look thumbnail | 88 × 110 | name below, 12 px dim |

---

## 5. MVP implementation proposal

### 5.1 Architecture

- **Everything Wardrobe lives in the private repo.** New screens go in `gwobserver-private-dev/SourceFiles/Character/Wardrobe/` (`WardrobeApp.cpp/.h`, `WardrobeCollection.cpp`, `WardrobeCreate.cpp`, `WardrobeDress.cpp`, `WardrobeStore.cpp`, `WardrobeViewport.cpp`, `WardrobeThumbnails.cpp`). They are listed in the Observer vcxproj only, per the two-host compile contract, and compile UNGUARDED because they ship. The existing dev Wardrobe stays under its diagnostics guard as the GWMB tool.
- **The public repo carries only the switch.** `draw_ui.cpp` gains the `AppScreen` enum, the toolbar button, the menu entry, and one call `draw_wardrobe_app(dat_manager, map_renderer, back_target_name)` declared in a private header, on the model of the two inert bools that exist today. No catalogue, no option list, no rule vocabulary and no diagnostic string reaches public source. Check the public binary for diagnostic strings before release, as the `GWO_PLAYERVISUALS_DIAGNOSTICS` work established.
- **The shared UI kit is public** because the browser uses it, and the private screens include it through the existing host-shim route. Private code may depend on public headers, never the reverse.
- **Persist the selection, not the composed character.** `settings/wardrobe/<character-id>.json`, schema `gwo-wardrobe-character-1`: name, profession, sex, campaign, face, hair, hair colour, skin tone, facial hair, favourite, timestamps, looks (name, five slots with armour id and up to four dyes, head hidden, default flag). Thumbnails as PNG beside it. Files hold ids the game's own item names already make public, never rule output.
- **Viewport.** A render target presented through `ImGui::InvisibleButton` plus `AddImage`, recreated on resize, orbit and zoom on the `OrbitalCamera`, reversed-Z, the private character shaders, the profession idle. Copy `ReplayWindow_PiP.cpp` and `ReplayWindow_CharacterPortrait.cpp`, do not fork the model viewer panel, which renders into the main scene.
- **Thumbnails** reuse the portrait render path at 360 x 480 and 128 x 128 and are written as PNG. Check which PNG writer the tree already links before adding one.
- **Scout is untouched.** The Wardrobe shares nothing with `BrowserState`.
- **One import entry, private.** `WardrobeStore::ImportFromAppearance(const CapturedAppearance&)` turns a recorded player's appearance record (profession, sex, appearance bitmap, slots 2 to 8 with dyes, visibility) into a character plus one look, without the UI. It is not used by the MVP screens, but it is the seam the replay export plugs into later, so the store is designed around the same selection struct the recorder already produces.

### 5.2 Work packages, each one PR to `dev`

| # | Package | Acceptance |
|---|---|---|
| 1 | `ui_kit.h`: extract theme push, section label, primary and secondary buttons, segmented button, card frame, search box, chip filter, favourite star from the browser | Browser renders pixel-identical before and after |
| 2 | Private `WardrobeStore` (JSON, thumbnails) and the option lists the screens need, built on `GW::Catalog` | Save, load, recompose round-trips; the public tree gains only the switch |
| 3 | `AppScreen` switch, Wardrobe button in both toolbars and the menu, Your characters screen with search, sort, filters, ⋯ menu, delete confirm, empty states | All four empty states reachable; keyboard-only creation and deletion; Scout behaviour unchanged |
| 4 | Create a character: identity panel, rendered face and hair tiles with cache, skin and hair colour, disclosure row, viewport with two camera presets, autosave and "Draft saved" | Changing skin tone re-tints the face tiles; no height control anywhere |
| 5 | Dress: wearing panel, hide headgear, single dye, apply to all, mix dyes, pieces and full sets, search, campaign filter, undo and redo, loading ring | Full set in one click; undo restores dyes; viewport never blank |
| 6 | Saved looks, draft and saved states, thumbnails, header actions, responsive drawer below Narrow, shortcuts in the Shortcuts modal, "game files not found" state | Resize from 1920 to 900 wide with no clipped text; Esc from every screen |

### 5.3 Explicitly out of the MVP - STILL OPEN, none of this was built

- Height. Not solved. Do not ship a guess.
- Costume and festival hat slots. Supported by the engine, deferred for simplicity of the first release.
- Weapons. Not part of the appearance capture.
- Anything that leaves the device, and anything that reads match data.

---

## 6. Roadmap - STILL OPEN, none of this was built

Each item plugs into an extension point the MVP already has: a slot row, a card menu entry, a viewport toolbar button, a Choose armor tab, or a look dropdown entry.

**1.1, soon after release**
- **The hub.** A 3D map at launch with two destinations, Match Library and Wardrobe. The Wardrobe tile shows the most recently edited characters. The Wardrobe's "< Back" target changes from "Library" to "Hub" and nothing else moves.
- **Export this look to the Wardrobe.** In a replay, the character panel gets one button. It creates a character named after the player, with one look named after the match and date, from the appearance the recorder captured, and shows a toast with "Open in Wardrobe". The Wardrobe stays unaware of matches; the replay does the pushing.
- **Costume and festival hat rows** under "More slots".
- **Save image.** A viewport toolbar button that writes a PNG with the studio background or transparent.
- **Shuffle.** A random set and dye, one click, undoable.

**1.2**
- **Look codes.** A short shareable text code, in the spirit of the game's skill templates, pasted into "New character" to recreate a look. Exposes ids only.
- **Compare.** Two looks side by side in the viewport.
- **Backdrops.** Guild-hall lighting presets from the solved map lighting model, as a viewport dropdown.
- **Costumes and capes** once the cape emblem path is designed.

**Later**
- **Height**, once the scale rule is measured.
- **Emotes and dances** in the viewport animation menu.
- **Community gallery** on the website, opt-in, look codes only.
