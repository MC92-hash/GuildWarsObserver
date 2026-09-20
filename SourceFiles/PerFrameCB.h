#pragma once
#include <DirectXMath.h>
#include "DirectionalLight.h"
#include "RenderConstants.h"

// Per-frame constant buffer struct
struct PerFrameCB
{
    DirectionalLight directionalLight; // Sun
    float time_elapsed;
    float fog_color_rgb[3];
    float fog_start;
    float fog_end;
    float fog_start_y; // The height at which fog starts.
    float fog_end_y; // The height at which fog ends.
    // THE FLAG WORD, one bit per switch. Every shader that reads it reads the same bits, so the
    // list lives here:
    //   bit 0 (1)   terrain / world shadows
    //   bit 1 (2)   water reflection
    //   bit 2 (4)   haze
    //   bit 3 (8)   model shadows
    //   bit 4 (16)  the per-map prop MODULATE 2X gate (old-format model program only)
    //   bit 5 (32)  the map light mode: SET means Classic (the last commit's terrain law,
    //               `texture * 1.4 * lightingColor`), clear means Client (experimental) (the
    //               client's own two-endpoint lerp at a quartic bake). Only the terrain program
    //               reads it. A new bit was used deliberately rather than a new field, so no
    //               constant-buffer offset moves and every shader that does not declare the
    //               field reads exactly what it read before.
    //
    // A WORD OF WARNING BEFORE ADDING TO TerrainRevPixelShader.hlsl: its whole text is embedded
    // in TerrainRevPixelShader.h as ONE raw string literal, and MSVC refuses a string literal
    // over 16380 bytes (C2026, which reads as a truncated shader rather than a size problem).
    // The file is at 15841 bytes, so roughly 500 bytes of comment are left before that trips.
    // Long rationale belongs here, in ordinary C++, where nothing is counted.
    //
    uint32_t should_render_flags;
    // The owner's environment light gain for the MAP - terrain and the world's own models. It is
    // 1.0 for every other draw in the frame: MapRenderer::Render sets it for the duration of the
    // world pass and puts it back, so the composed characters, their weapons and the emissive
    // sub-pass never see it. It takes the first of the three padding words, so every offset before
    // it is unchanged and any shader that does not declare it reads exactly what it read before.
    float map_light_gain;
    uint32_t pad[2];
};
