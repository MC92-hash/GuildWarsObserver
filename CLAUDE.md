# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

GW Observer is a Guild Wars 1 GvG match replay and analysis tool. It loads recorded matches, replays them in a 3D map view using DirectX 11, and provides tactical overlays, metrics, and analytics for competitive GvG analysis. Back-end support, tool improvements, and testing by Maverick.

**Closed-source community tool** — see LICENCE.md.

## Build System

Visual Studio 2022 solution, Platform Toolset v143, C++20 (`/std:c++20`), Warning Level 4.

**Solution path:** `GuildWarsObserver-1.0.2-sourcecode/GuildWarsObserver-1.0.2/GuildWarsObserver.sln`
**Source files:** `SourceFiles/` (~275 files)

**Build from command line:**
```
cd GuildWarsObserver-1.0.2-sourcecode/GuildWarsObserver-1.0.2
msbuild GuildWarsObserver.sln /p:Configuration=Release /p:Platform=x64
```

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
- **agents.json** — JSON marker overlay format (see `SourceFiles/OVERLAY_DESIGN.md`)

### UI
All UI is Dear ImGui immediate-mode. ~28 modular panels live in separate `draw_*.cpp` files (e.g., `draw_replay_browser.cpp`, `draw_right_panel.cpp`, `draw_timeline.cpp`, `draw_dat_browser.cpp`, `draw_setup_wizard.cpp`, `draw_file_info_editor_panel.cpp`). Panel visibility is toggled via `GuiGlobalConstants`. Default hotkeys: Space = play/pause, Left/Right arrows = seek ±5s. The replay browser is always visible as the default background. Settings window (File > Settings) consolidates Data Source, File Paths, and Font configuration.

### Shaders
HLSL shaders are compiled to C++ headers. Multiple pixel/vertex shaders: NewModel, OldModel, Terrain, Water, Sky, Skinned, Clouds.

### Utility Scripts
`scripts/` contains ~17 Python 3 scripts for reverse-engineering FFNA binary formats (chunk analysis, submesh parsing, UV layout comparison). These are developer tools, not part of the build.

## Code Style

- Follow existing naming conventions in surrounding code
- Comments in English
- No commented-out code in pull requests
- C++ standard: C++20
- HRESULT errors use `DX::ThrowIfFailed()` (defined in `pch.h`)

## Branch Naming

`fix/description` or `feat/description`

## Cloud Storage & Upload Pipeline

Matches are distributed via Cloudflare R2 cloud storage. The app fetches an index and compressed match archives from the configured cloud host.

**Key files:**
- `scripts/upload_to_r2.py` — main upload script (`--dry-run`, `--list-remote`, `--source-dir`)
- `scripts/r2_config.env` — R2 credentials (gitignored, see `.example`)
- `scripts/setup_scheduled_task.bat` — Windows Task Scheduler automation

## Important Constraints

- Do not integrate unauthorised data sources without prior discussion
- Do not remove or alter licence/attribution notices
- Do not expose API credentials
