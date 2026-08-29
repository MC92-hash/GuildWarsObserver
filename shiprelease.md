# Shipping a GW Observer release

The procedure for cutting a public release and pushing it through the in-app updater.
Follow the phases in order. The ordering is not cosmetic: several of the failure modes below
are completely silent, and the phase order is what catches them before users do.

`scripts/package_release.py` is the executable form of the manifest in this document. If you
change one, change the other, or they drift and the document stops being true.

---

## Status of the release in flight

**v2.0.0, staged 29 August 2026, NOT yet public.** Delete this section once it ships.

Done:

- [x] `GWO_VERSION` bumped to `2.0.0`, Release x64 built and verified
- [x] `dist/GWObserver.zip` built (4798 entries, 101.7 MB) and verified flat
- [x] Committed on `dev` (`6ca980a`), merged to `master`, both pushed
- [x] `v2.0.0` published as a **prerelease** on `MC92-hash/GuildWarsObserver`, asset
      `GWObserver.zip` uploaded and byte-identical to the local zip
- [x] Confirmed `/releases/latest` still returns `v1.2.7`, so no user sees anything yet

Remaining, in this order:

- [ ] **Phase 3 test.** A throwaway "prober" build sits at `dist/prober/GuildWarsObserver.exe`:
      it is the 2.0.0 codebase still reporting version `1.2.7`, with the update endpoint
      pointed at `/releases/tags/v2.0.0` so it can see a release the public endpoint hides.
      Use it to run the update test below. It is the only reason that build exists; delete it
      afterwards.
- [ ] **Phase 4 promote**, then re-test once on the genuine `/releases/latest` path
- [ ] **Phase 5 website.** The `GWObserver-Website` repo has uncommitted 2.0.0 work on
      `master` (index, script, style, plus untracked `releases/2.0.0/` and
      `assets/releases/2.0.0/`). Push it only after the release is promoted.

---

## What the updater actually does

`SourceFiles/Net/UpdateChecker.cpp` is the whole mechanism. Everything in this document
follows from it.

- Polls `GET https://api.github.com/repos/MC92-hash/GuildWarsObserver/releases/latest`,
  unauthenticated, automatically at startup. Anonymous GitHub allows 60 requests per hour per
  IP; exceeding it surfaces as `GitHub API returned HTTP 403`.
- `/releases/latest` never returns drafts or prereleases. That is what makes the staged test
  in Phase 3 safe, and the rollback in Phase 4 possible.
- Version compare is `sscanf_s(p, "%d.%d.%d")` after stripping one leading `v`. There is no
  pre-release or 4-part handling: `1.3.0-beta` parses as `1.3.0`, `1.2.7.1` equals `1.2.7`,
  and a non-numeric tag parses as `0.0.0` and is never offered.
- Asset selection is a **case-sensitive** `.zip` suffix match, preferred over `.exe`.
  `Update.ZIP` would not match. If several `.zip` assets are attached the **last one wins**,
  so attach exactly one.
- Install writes a batch script that waits for the app to exit, then runs
  `tar -xf "<zip>" -C "<exeDir>"` over the live install directory and relaunches the exe.

### The two silent failure modes

**A nested folder in the zip.** `tar` preserves entry paths and nothing strips a top-level
directory. If the zip contains `GuildWarsObserver-2.0.0/GuildWarsObserver.exe`, extraction
succeeds, the running exe is never replaced, and the app relaunches the **old version**. No
error is shown, logged, or reported anywhere. The zip must be flat, with
`GuildWarsObserver.exe` at the root.

**Shipping user state.** The app writes `gui_settings.ini` next to the exe
(`GuiGlobalConstants.h:373-376`), holding `gw_dat_path`, `match_data_folder`, window geometry
and `dismissed_update_version`. It writes `settings/ui_layout.json`
(`ReplayWindow.cpp:268-277`), holding the ribbon and panel layout. Because the updater
extracts over the install directory, shipping either one means every auto-update overwrites
the user's own configuration and drops them back into the setup wizard. The July 2026 test
package shipped both. `package_release.py` now refuses to build a zip containing them.

Also note `tar` never deletes, so files dropped from the package linger in an updated install.
Harmless, but it means an updated install is not byte-identical to a fresh one.

---

## The package

Flat, exe at the root.

**Include**

| Item | Source | Why |
|---|---|---|
| `GuildWarsObserver.exe` | `x64/Release/` | |
| `bass.dll`, `bass_fx.dll` | `SourceFiles/` | Also embedded as `IDR_DLL1`/`IDR_DLL2` and self-extracted on first run, but shipping them avoids a write into a read-only install dir |
| `Data/` | repo root | Skill database and balance patches. Omitting it leaves the app with no skill data, which reads as an older build |
| `settings/` | **two sources, merged** | `builds.json`, `skill_sounds.json`, `sound_overrides.json` from the repo root; `asset_blacklists/*.json` from `x64/Release/settings/` |
| `Textures/` | repo root | Fonts, cursors, icons, launch screen. The bulk of the download |
| `LICENCE.md`, `NOTICE.md` | repo root | Attribution, never remove |
| `RELEASE_NOTES.md` | generated | From the `--notes` file |

The `settings/` merge is the easy one to get wrong. In the dev tree the exe lives at
`x64/Release/` and the app walks up parent directories to find `Data/`, `settings/` and
`Textures/` at the repo root, so `x64/Release/settings/` and the root `settings/` are two
different directories that must become one in the package.

**Exclude**

| Item | Why |
|---|---|
| `gui_settings.ini` | User state. Overwrites `gw_dat_path` on update |
| `settings/ui_layout.json` | User state. Overwrites the ribbon and panel layout |
| `imgui_layout.ini`, `notes.json`, `ratings.json`, `bookmarks.json` | User state |
| `MatchCache/`, `UserData/` | User data, and large |
| `animation_clips_cache.bin`, `animation_cache.ini` | Keyed to the user's own `gw.dat` size and rejected on mismatch |
| `*.cso` | Dead weight. Nothing reads them: every shader is compiled at runtime from in-memory string literals via `D3DCompile` (`PixelShader.h`, `VertexShader.h`). They are FxCompile byproducts |
| `*.pdb`, `*.lib`, `*.exp`, `*_debug.log`, `CrashDump.dmp`, `*.bak-*` | Build and debug debris |

The CRT is statically linked (`RuntimeLibrary=MultiThreaded`), so no VC++ redist is needed.

---

## Phase 0 - Pre-flight

1. `gwobserver-private` must sit as a **sibling of the repo root**. The build hard-fails
   without it: the vcxproj references its sources unconditionally. Separately,
   `build_config.local.h` is picked up through `__has_include`, and when it is missing the
   build still succeeds while silently falling back to `GWO_CLOUD_ENABLED 0`. That produces a
   binary that shows only local matches. Phase 2 verifies this against the binary rather than
   trusting it.
2. `dev` clean and in sync with `origin/dev`. Fetch first; it moves.
3. Note the stray `v8.1.0` tag, which points at an old March commit and exists on origin. It
   has no GitHub release attached, so `/releases/latest` is unaffected. **Never create a
   release from it**: `8.1.0` beats every real version, and every client would be stuck being
   offered it forever. Worth archiving as `archive/v8.1.0` and deleting.
4. Close any running `GuildWarsObserver.exe`, or build to a separate `OutDir`. A running
   instance holds a lock on the exe and the link step fails with `LNK1104`.

## Phase 1 - Build the prober

Do this **before** the version bump, while the tree still reads the old version and only one
line needs touching.

1. In `SourceFiles/Net/UpdateChecker.cpp`, change `/releases/latest` to
   `/releases/tags/vX.Y.Z`. Leave `GWO_VERSION` at the **old** version.
2. Build Release x64, copy the exe to `dist/prober/GuildWarsObserver.exe`.
3. **Revert the source edit** and confirm the tree is clean.

The prober is a throwaway client that reports the old version and can see a release the public
endpoint is still hiding. Nothing else about the download or install path differs.

## Phase 2 - Bump, build, package

1. Bump `GWO_VERSION` in `SourceFiles/build_config.h`.
2. Build:
   ```bash
   MSBUILDER='/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe' && "$MSBUILDER" GuildWarsObserver.sln '-p:Configuration=Release' '-p:Platform=x64' '-v:minimal'
   ```
   Add `-p:OutDir=x64\ReleaseShip\` if an instance is running.
3. Verify the binary before packaging, because both of these fail silently:
   ```bash
   grep -a -c "2\.0\.0" x64/Release/GuildWarsObserver.exe
   grep -a -o -m1 -E "https://[a-z0-9]+\.r2\.cloudflarestorage\.com" x64/Release/GuildWarsObserver.exe
   ```
   The first proves the version bump reached the binary, the second proves
   `build_config.local.h` was picked up and the cloud library is on.
4. Package:
   ```bash
   py -3.11 scripts/package_release.py --version 2.0.0 \
       --exe x64/Release/GuildWarsObserver.exe \
       --notes dist/release_notes_2.0.0.md
   ```
   The script asserts the exe carries the version being tagged, that the exe is a top-level
   zip entry, and that no user-state file made it in. It fails loudly rather than producing a
   bad zip. Sanity-check the printed entry count and size against the previous release.

   The zip is built with Windows `tar.exe`, listing top-level names explicitly. Do **not** use
   `tar -C <stage> .`, which prefixes every entry with `./` and makes Explorer refuse the
   file, and do **not** use .NET `ZipFile.CreateFromDirectory` under PowerShell 5.1, which
   writes backslash separators that violate the zip spec.

## Phase 3 - Internal update test (the gate)

Nothing in this phase is visible to users. `/releases/latest` keeps returning the previous
version throughout.

1. Commit the bump on `dev`, push, merge `dev` into `master`, push. `master` is the public
   release line and the tag belongs there. The tag target is baked in at release creation,
   which is why this happens before publishing.
2. Publish as a **prerelease**, with exactly one `.zip`:
   ```bash
   gh release create vX.Y.Z dist/GWObserver.zip --repo MC92-hash/GuildWarsObserver \
       --title "GW Observer vX.Y.Z" --notes-file dist/release_notes_X.Y.Z.md \
       --prerelease --target master
   ```
3. Confirm it is genuinely hidden, and that the upload is intact:
   ```bash
   gh api repos/MC92-hash/GuildWarsObserver/releases/latest --jq .tag_name
   gh api repos/MC92-hash/GuildWarsObserver/releases/tags/vX.Y.Z --jq '[.assets[]|{name,size}]'
   ```
   The first must still name the previous version. The second must match the local zip byte
   for byte.
4. Build the scratch install:
   - `gh release download <previous tag> --repo MC92-hash/GuildWarsObserver -D dist/scratch`
   - extract it to a scratch directory **outside the repo**
   - overwrite its exe with `dist/prober/GuildWarsObserver.exe`
   - put a recognisable `gw_dat_path` in its `gui_settings.ini`, and a marker in
     `settings/ui_layout.json`
5. Launch it and take the update through the UI: check, Download, Install and Restart.
6. Assert all of these:
   - it relaunches on its own and reports the **new version**
   - `gw_dat_path` and `ui_layout.json` **survived unchanged**
   - the exe at the scratch root has a new timestamp, and **no nested folder appeared**
   - `GWObserver_update.zip` and `_gwobs_update.bat` cleaned themselves up
   - a replay loads and skills resolve, proving `Data/` landed
7. On any failure: fix, repackage,
   `gh release upload vX.Y.Z dist/GWObserver.zip --clobber`. The release is still a
   prerelease and still invisible.

## Phase 4 - Promote

1. ```bash
   gh release edit vX.Y.Z --repo MC92-hash/GuildWarsObserver --prerelease=false --latest
   ```
   No re-upload, so the bytes that passed Phase 3 are exactly the bytes that go live.
2. Re-run the smoke test once with a **genuine unmodified** previous-version install, no
   prober, so the real `/releases/latest` path is exercised with zero code differences.
3. Verify the tag points at the `master` commit the shipped exe was built from.
4. Delete `dist/prober/`.

**Rollback.** `gh release edit vX.Y.Z --prerelease=true` pulls a bad release straight back out
of `/releases/latest`, and clients stop being offered it immediately. Anyone who already
updated stays updated, so this limits the blast radius rather than undoing it.

## Phase 5 - Website

**Only after the release is promoted.** The site hardcodes the download URL for the new tag in
several places, so pushing first means every download button 404s. `script.js` would then
repoint them at the newest non-prerelease zip, giving buttons that read "Download <previous
version>" on a page selling the new one.

The site is `mevi826/gwobserver-website`, served by GitHub Pages from `master` root with CNAME
`gwobserver.com`, so **pushing master is deploying**.

1. Review the diff, commit, push `master`.
2. Verify live: download buttons resolve to a real asset, `/releases/X.Y.Z/` renders with all
   its screenshots, and the home page changelog shows the new entry. That changelog renders
   the **GitHub release body** as markdown through `/releases?per_page=10`, which is why the
   release body is worth writing properly rather than pointing only at the site.

## Verification summary

- `package_release.py` assertions pass
- Phase 3 scratch update: relaunches on the new version, settings survive, no nested folder
- Phase 4 repeat on the genuine `/releases/latest` path
- Live site: buttons resolve, notes page renders, changelog updates
- Clean-machine check, ideally on a second box: unzip into an empty folder, launch, confirm
  the first-launch wizard appears, audio initialises, fonts render, and a replay loads
