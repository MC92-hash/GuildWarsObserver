#pragma once
// ---------------------------------------------------------------------------
// --export-cape-banners <in.json> <outdir>: guild cape banners as PNG files, headless.
//
// in.json is an array of objects with the seven CapeData fields (bg_color, detail_color,
// emblem_color, shape, detail, emblem, trim). Each cape is composed on the CPU exactly as the
// loading screen composes it (GuildBannerComposer, 128x256 RGBA, assets from Textures/CapeAssets)
// and written to <outdir>/<MatchLinks::CapeId>.png; files already there are skipped. No window,
// no device. scripts/export_cape_banners.py publishes them to the website (images only).
// ---------------------------------------------------------------------------
#include <string>

// Process exit code: 0 when every cape was written or already there, 1 otherwise.
int RunCapeBannerExport(const std::wstring& inJson, const std::wstring& outDir);
