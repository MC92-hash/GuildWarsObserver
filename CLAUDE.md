# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

GW Observer is a Guild Wars 1 GvG match replay and analysis tool. It loads recorded matches, replays them in a 3D map view using DirectX 11, and provides tactical overlays, metrics, and analytics for competitive GvG analysis. Back-end support, tool improvements, and testing by Maverick.

**Closed-source community tool** — see LICENCE.md.

## Build System

Visual Studio 2022 solution, Platform Toolset v143, C++20 (`/std:c++20`), Warning Level 4.

**Solution path:** `GuildWarsObserver.sln` (repository root)
**Source files:** `SourceFiles/` (~345 files)
**Build output:** `x64/Release/GuildWarsObserver.exe`

**Build from command line (bash):**
```bash
MSBUILDER='/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe' && "$MSBUILDER" GuildWarsObserver.sln '-p:Configuration=Release' '-p:Platform=x64' '-v:minimal'
```
Use `-p:` instead of `/p:` to avoid bash flag issues. MSBuild path: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe`.

**Configurations:** Debug/Release × Win32/x64. Release uses `/O2`, whole-program optimization, and fast floating-point model. SSE2 is enabled in all configs.

**No package manager** — all third-party libraries are vendored in-tree:
- `DearImGui/` — ImGui with D3D11/Win32 backends
- `ImGuiFileDialog-Lib_Only/` — file dialog extension
- `DirectXTex/` — Microsoft texture processing
- `peglib/` — PEG parser generator
- `tinytiff/` — TIFF image support (links `TinyTIFF_x86.lib` or `TinyTIFF_x64.lib`)

**BASS audio** (`bass.dll`, `bass_fx.dll`) is loaded at runtime via dynamic function pointers — extracted from resources if not found in the exe directory. Function pointer typedefs are in `pch.h`.

**Precompiled headers:** `pch.h` / `pch.cpp` — include `pch.h` first in every .cpp file. It pulls in DirectX 11, Windows headers, COM support (`wrl/client.h`), ImGui, DirectXTex, GW format decompressors, and standard C++20 headers (`<format>`, `<span>`, `<optional>`, `<filesystem>`).

**Linker dependencies:** d3d11, dxgi, dxguid, ddraw, d3dcompiler, DbgHelp, GDI+.

**No test framework** — testing is manual against recorded match replays.

## Architecture

### Application Startup
`wWinMain()` in `Main.cpp` → crash handler setup (`CrashDump.dmp`) → `XMVerifyCPUSupport()` → COM init → `GuiGlobalConstants::LoadSettings()` → create `MapBrowser` (main app singleton, stored in `g_map_browser`) → Win32 message loop. GPU preference exports (`NvOptimusEnablement`, `AmdPowerXpressRequestHighPerformance`) force discrete GPU selection.

### Core Loop
`MapBrowser::Tick()` → manages `ReplayWindow` instances (one per open match). Each `ReplayWindow::Tick()` → `Update()` + `Render()`.

### Phased Map Loading
`ReplayWindow` uses a loading state machine: **Validate → Init → PropModels → PlaceProps → FadingOut → Ready/Error**. Long operations are batched across frames (`kPropModelBatchSize = 15`, `kPropPlaceBatchSize = 10`) to keep the UI responsive.

### Render Pipeline
`ReplayWindow::Render()` → MapRenderer (sky, terrain, props, water) → AgentOverlay → picking readback → ImGui → Present. The overlay runs as a separate pass after the main scene, before picking and UI.

### Key Modules
- **ReplayWindow** (~17K lines) — main window, rendering, UI integration, replay logic
- **MapRenderer / Terrain** — heightmap geometry from FFNA format, terrain rendering
- **MeshManager / TextureManager** — 3D model instances and texture loading/caching
- **AgentOverlay** — per-agent visuals (cylinders, skill icons, cast bars, range rings, lasers)
- **Animation/** — skeletal animation system (clips, controller, evaluator, skeleton, state machine) — header-only
- **Audio/** — BASS-based spatial audio, skill→sound mappings in `settings/skill_sounds.json`
- **Parsers/** — header-only binary format parsers (BB8 geometry, BB9 animation, FFNA maps/models, StoC protocol)
- **DATManager** — Guild Wars .dat file reading and unpacking (shared across replay windows)
- **HeatmapRenderer / HeatmapData** — player position density visualization
- **Cache/** — file and model caching (header-only: `FileCache.h`, `ModelCache.h`)
- **GuiGlobalConstants** — global settings singleton (DAT path, panel visibility, auto-camera config, etc.), persisted to disk via `LoadSettings()`/`SaveSettings()`
- **Net/** — cloud storage: `CloudReplayProvider` (fetch/cache), `HttpClient` (WinHTTP), `MatchIndex` (index.json parser), `SyncEngine` (background sync thread), `TarGzExtractor` (archive extraction supporting both `.tar` and `.tar.gz`)

### Data Formats
- **FFNA** — Guild Wars asset format (maps and models), parsed in header-only files (~400KB)
- **ATEX** — custom texture format with dedicated decompressor (AtexReader/AtexDecompress/AtexAsm)
- **StoC** — server-to-client binary protocol for match replay data
- **agents.json** — JSON marker overlay format (see `docs/OVERLAY_DESIGN.md` in gwobserver-private)

### UI
All UI is Dear ImGui immediate-mode. ~28 modular panels live in separate `draw_*.cpp` files (e.g., `draw_replay_browser.cpp`, `draw_right_panel.cpp`, `draw_timeline.cpp`, `draw_dat_browser.cpp`, `draw_setup_wizard.cpp`, `draw_file_info_editor_panel.cpp`). Panel visibility is toggled via `GuiGlobalConstants`. Default hotkeys: Space = play/pause, Left/Right arrows = seek ±5s. The replay browser is always visible as the default background. Settings window (File > Settings) consolidates Data Source, File Paths, and Font configuration.

### Shaders
HLSL shaders are compiled to C++ headers. Multiple pixel/vertex shaders: NewModel, OldModel, Terrain, Water, Sky, Skinned, Clouds.

### Utility Scripts
`scripts/` contains ~17 Python 3 scripts for reverse-engineering FFNA binary formats (chunk analysis, submesh parsing, UV layout comparison). Those are developer tools, not part of the build.

**Not all of `scripts/` is a developer tool.** Part of it is the publish pipeline Watchtower runs, and those modules are runtime dependencies with a real import chain:

```
upload_to_r2.build_stats_entry
  -> combat_analytics.build_from_match_dir
       -> player_matrix.build_player_matrix
            -> max_hp_solver          (a faithful Python port of SourceFiles/MaxHpSolver.cpp)
```

Keep that chain intact when adding or moving files. `upload_to_r2.py:580` wraps the analytics call in a broad `try/except` so a parser defect never costs a match its index entry - which also means a **missing module in this chain fails silently**: the `ImportError` is caught, `analytics` becomes `{}`, and every match publishes with no `combat_analytics` at all, not merely without the piece that was missing. The only trace is a `Warning: combat analytics unavailable` line in the upload log.

`max_hp_solver.py` duplicates the constants in `SourceFiles/MaxHpSolver.cpp` on purpose (search window, residual thresholds, packet tolerance and the fourteen correction offsets). If you change them in one, change them in the other, or the desktop tool and the website will disagree about the same match.

## Code Style

- Follow existing naming conventions in surrounding code
- Comments in English
- No commented-out code in pull requests
- C++ standard: C++20
- HRESULT errors use `DX::ThrowIfFailed()` (defined in `pch.h`)

## Branch Naming

`dev` is the integration branch for all new work. The latest working state always lives
there. `master` is the public/release line and only receives merges from `dev` at a
release, or hotfixes.

- Branch off `dev`, never off `master`: `feat/description` or `fix/description`
- Merge finished work back into `dev`, then delete the feature branch
- Keep the branch list short. Long-lived stacks of feature branches on the remote are
  redundant once their work is in `dev`
- Archive an abandoned branch as a tag (`archive/name`) before deleting it, so the
  commit stays reachable

## Release Workflow

No CI/CD - releases are manual build-and-publish.

**`shiprelease.md` at the repo root is the procedure.** Follow it rather than working from the
summary here, because most of what can go wrong in a release goes wrong silently.

The short version: bump `GWO_VERSION` in `SourceFiles/build_config.h`, build Release x64,
package with `scripts/package_release.py`, merge `dev` into `master`, publish as a
**prerelease** first, test the update end to end, then promote with `gh release edit
--prerelease=false --latest`, and only then push the website.

The in-app `UpdateChecker` polls `/releases/latest`, compares `tag_name` against `GWO_VERSION`
via semver, and offers download plus swap-and-restart. Two constraints decide whether a
release works:

- The zip must be **flat**, exe at the root. The updater installs it with
  `tar -xf <zip> -C <exeDir>` over the live install directory, so a wrapper folder extracts
  without error, never replaces the running exe, and relaunches the old version silently.
- The package must **never** contain `gui_settings.ini` or `settings/ui_layout.json`. Both are
  written next to the exe at runtime, so shipping them makes every auto-update overwrite the
  user's DAT path and layout.

`scripts/package_release.py` enforces both, and refuses to package an exe whose embedded
version string does not match the version being tagged. Prereleases and drafts are never
returned by `/releases/latest`, which is what makes the staged test safe and gives a one-command
rollback.

## Cloud Storage & Upload Pipeline

Matches are distributed via Cloudflare R2 cloud storage. The app fetches an index and compressed match archives from the configured cloud host.

**Key files:**
- `scripts/upload_to_r2.py` — main upload script (`--dry-run`, `--list-remote`, `--source-dir`)
- `scripts/r2_config.env` — R2 credentials (gitignored, see `.example`)
- `scripts/setup_scheduled_task.bat` — Windows Task Scheduler automation
- `scripts/backfill_index.py` — retrofits guild capes and `preview_stats` onto matches already published (dry run by default; `--from-r2` for matches with no local recording)
- `scripts/backfill_stats.py` — the same idea for the `stats/<YYYY-MM>.json` sidecar
- `scripts/combat_analytics.py`, `scripts/player_matrix.py`, `scripts/max_hp_solver.py` — the analytics chain above, required at publish time

`write_index()` in `upload_to_r2.py` is the **single writer** for `index.json`. It publishes minified JSON plus a gzipped `index.json.gz` alongside it, never instead of it, so builds already released keep working. Anything that rewrites the index goes through it - a hand-rolled `put_object` re-inflates a 12 MB object that every client downloads.

## Important Constraints

- Do not integrate unauthorised data sources without prior discussion
- Do not remove or alter licence/attribution notices
- Do not expose API credentials
- Do not add Co-Authored-By lines to git commit messages
- Do not use em-dashes (-) in written text - use regular hyphens (-) instead
