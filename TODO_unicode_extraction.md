# Unicode extraction bug - non-ASCII match names fail to load

## Problem

Matches with non-ASCII characters in folder names (e.g. German map name
"Insel der Wurmer" with u-umlaut) fail to load when placed as `.tar.gz`
archives in MatchCache. The match data extracts but ends up one directory
level too deep, so the app never finds `infos.json`.

## Root cause

Two independent bugs combine:

### 1. SyncEngine assumes inner tar folder name matches archive filename

`SyncEngine.cpp` Phase 1b derives `folderName` from the archive filename
on disk, then checks `tmpDir / folderName / "infos.json"` to detect nested
archives. If the folder name inside the tar differs from the archive
filename (which happens with any Unicode encoding mismatch), the check
fails and extraction is silently abandoned.

`CloudReplayProvider::EnsureMatchAvailable` does NOT have this bug - it
iterates subdirectories to find wherever `infos.json` ended up, regardless
of folder name.

**Status: fixed** - SyncEngine now uses directory iteration (same approach
as CloudReplayProvider) instead of relying on folder name matching.

### 2. TarGzExtractor treats UTF-8 tar entry names as system code page

Tar headers store filenames as UTF-8 bytes. `TarGzExtractor.h` reads them
into `std::string` and passes them to `std::filesystem::path`. On MSVC,
the `path(std::string)` constructor interprets the string as the active
Windows code page (ACP), not UTF-8. This causes non-ASCII characters to
produce mojibake in extracted file/folder names.

**Status: fixed** - TarGzExtractor now converts the raw bytes via
`std::u8string` / `char8_t` so the path constructor treats them as UTF-8.

## Remaining work

- [ ] Verify the two code fixes compile and pass manual testing
- [ ] Consider sanitizing folder names in the scrim booking tool before
      creating `.tar.gz` archives (same `sanitize_folder_name` logic as
      `upload_to_r2.py`), so archives arrive with ASCII-safe names and
      avoid the problem at the source
- [ ] Check if the double-UTF-8 encoding of the `.tar.gz` filename itself
      is a bug in the scrim booking tool's download path (the archive on
      disk shows `WÃ¼rmer` instead of `Wurmer` or `W__rmer`)
- [ ] Audit `HttpClient` download path for correct UTF-8 handling when
      saving files with non-ASCII names to disk

## Affected matches

Any match on a map with non-ASCII characters in the German name (e.g.
"Insel der Wurmer") that arrives as a `.tar.gz` via scrim booking rather
than through the normal R2 upload pipeline.

## Quick fix for affected users

Run `scripts/fix_nested_match.py` pointing at the MatchCache directory.
It finds directories where `infos.json` is incorrectly nested one level
deep and flattens them.
