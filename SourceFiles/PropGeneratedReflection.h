#pragma once
#include "FFNA_ModelFile.h"
#include "Mesh.h"
#include <algorithm>
#include <cstdint>

// A MAP PROP LAYER ON GENERATED COORDINATES - the "shine" layer of an old-format material.
//
// A layer whose UV-set byte is 253 (-3) has no UV set at all: the client generates its coordinate per
// vertex from the camera-space reflection of the eye ray about the normal,
//
//     uv = (0.5 R.x + 0.5, -0.5 R.y + 0.5),   R = reflect(normalize(P_view), normalize(N_view))
//
// (decoded for weapons - the Map Browser's docs/appearance_data/WEAPON_EFFECTS.md, section FX2). GetMesh
// clamps the byte to UV set 0, which TILES the environment texture over the mesh. On Uncharted Isle every
// stone prop (bridges, towers, pillars, gates) carries 0x9B51 / 0x9B4A - soft white blobs on black - on
// such a layer, and they drew as rows of white spots all over the stone.
//
// The pair is [env, uv 253, blend 0] + [art, blend 3], and the client composes it as
//
//     art.rgb + art.a * env.rgb          (D3DTOP_MODULATEALPHA_ADDCOLOR, Arg1 = the art)
//
// so the art's alpha is the shine mask - nearly black on that stone, which is why the game shows no
// shine on it. The old-model shader's blend-3 stage computes exactly that when the art comes FIRST
// (`finalColor.a * stage + finalColor`), which is how GetMesh orders the pair when the two layers' flags0
// differ. When they are equal it keeps the env first and the env went in at full strength; this puts the
// art first in that case too. GetMesh keeps the blend flags in file order in both cases, so only the
// texture, UV set and type move.
//
// Blend 5 (GR op 8, not decoded) is drawn the same way: the one map pair that uses it, Isle of Solitude's
// crates and doors [0x454EF, 0x333B1 at blend 5], stores the metal fittings' shine mask in the art's alpha
// exactly as blend 3 does, and the shader already treats 5 like 3. INFERRED from that texture, not read.
//
// A material whose record says PIXEL SHADER 1 (uts0 byte 6) is not composed by its blend states at all: the
// client binds legacy program 1 (Map Browser docs/appearance_data/gw_program1.md, WEAPON_EFFECTS.md section 21)
//
//     rgb = art x light + env x saturate(2 art.a - 1)        a = saturate(2 art.a)
//
// - the art's alpha is coverage below one half and a highlight weight above it. On a map it is Isle of Solitude's
// banners [0x9B51, 0x3240C at blend 8], which drew darkened by the env map multiplied into them. Its env
// stage carries kTextureTypeProgramOneHighlight as well, and the pair is put art first like the others.
//
// The stages are flagged on the constant buffer's texture_types (Mesh::texture_types is 16 bits wide);
// every shader reads only the low byte of that field, so one that does not know the bits is unaffected.
constexpr uint32_t kTextureTypeGeneratedReflection = 0x10000u;
constexpr uint32_t kTextureTypeProgramOneHighlight = 0x20000u;

struct PropGeneratedStages
{
    uint32_t generated = 0;     // bit k: stage k samples at the generated reflection
    uint32_t program_one = 0;   // bit k: stage k is program 1's highlight term
};

inline PropGeneratedStages ApplyPropGeneratedReflectionStages(const FFNA_ModelFile& model, int submodel, Mesh& mesh)
{
    PropGeneratedStages out;
    const auto& geom = model.geometry_chunk;
    const auto& tvs = geom.tex_and_vertex_shader_struct;
    // Old-format (layered) materials only - the path whose stages GetMesh reads from these tables.
    if (!geom.unknown_tex_stuff1.empty() || tvs.uts0.empty() || submodel < 0 ||
        submodel >= static_cast<int>(geom.models.size()))
        return out;

    // The submodel's material, and its window in the layer tables - GetMesh's own arithmetic.
    int material = geom.models[submodel].unknown;
    material %= static_cast<int>(tvs.uts0.size());
    size_t start = 0;
    for (int m = 0; m < material; ++m)
        start += tvs.uts0[m].f0x7;
    const size_t end = std::min({start + tvs.uts0[material].f0x7, tvs.tex_array.size(),
                                 tvs.texture_index_UV_mapping_maybe.size()});

    // Textures this material reads ONLY on generated coordinates. A texture also read on a real UV set
    // cannot tell its two stages apart after GetMesh, so it is left as drawn.
    uint32_t generated = 0, regular = 0;
    for (size_t i = start; i < end; ++i)
    {
        const uint8_t tex = tvs.texture_index_UV_mapping_maybe[i];
        if (tex >= 32)
            continue;
        if (tvs.tex_array[i] == 253)
            generated |= 1u << tex;
        else if (tvs.tex_array[i] != 255)
            regular |= 1u << tex;
    }
    generated &= ~regular;
    if (generated == 0)
        return out;

    const size_t n = std::min({mesh.tex_indices.size(), mesh.uv_coord_indices.size(), mesh.texture_types.size(),
                               mesh.blend_flags.size(), size_t{32}});
    auto isGenerated = [&](size_t k) { return mesh.tex_indices[k] < 32 && ((generated >> mesh.tex_indices[k]) & 1u); };
    auto swapStages = [&](size_t k) {
        std::swap(mesh.tex_indices[k], mesh.tex_indices[k + 1]);
        std::swap(mesh.uv_coord_indices[k], mesh.uv_coord_indices[k + 1]);
        std::swap(mesh.texture_types[k], mesh.texture_types[k + 1]);
    };

    // Program 1: exactly [art, env] - the program has two samplers and nothing else.
    if (tvs.uts0[material].pixel_shader_id == 1 && n == 2 && isGenerated(0) != isGenerated(1))
    {
        if (isGenerated(0))
            swapStages(0);
        out.generated = out.program_one = 1u << 1;
        return out;
    }

    for (size_t k = 0; k < n; ++k)
    {
        if (!isGenerated(k))
            continue;
        // [env, blend 0] followed by [art, blend 3 or 5]: put the art first, so that stage adds the env by
        // the art's alpha.
        if (mesh.blend_flags[k] == 0 && k + 1 < n && (mesh.blend_flags[k + 1] == 3 || mesh.blend_flags[k + 1] == 5) &&
            !isGenerated(k + 1))
        {
            swapStages(k);
            out.generated |= 1u << (k + 1);
            ++k;
            continue;
        }
        out.generated |= 1u << k;
    }
    return out;
}
