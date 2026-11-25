#pragma once

#include <string>
#include <vector>

struct ImFont;
struct ImFontAtlas;
struct ID3D11Device;
typedef unsigned short ImWchar16;

// The Library page's type: a font atlas of its own, whose glyphs Windows draws (GDI, grey
// antialiasing) with the font's own hinting and whole-pixel advances, as Windows itself draws the
// same text. The interface fonts (stb_truetype, unhinted) stay as they are.
//
// The page bakes its sizes on the frame it needs them (Bake, called every frame, does nothing while
// the atlas already holds that scale and those characters), so a new window size or a new library
// never shows stand-in type: the frame that changes the scale draws with the new rungs. Only the
// page's atlas is rebuilt, never io.Fonts, so it is quick (Basic Latin, Latin-1, the page's
// punctuation and the library's own other characters, not whole Unicode blocks).
//
// Text from these fonts needs the atlas's texture bound: ImGui::PushFont binds it, a draw list's
// AddText does not (the page's text helpers bind it).
namespace LibraryFonts
{
    void SetDevice(ID3D11Device* device);

    // The interface font's files, from LoadSelectedFont (empty for the built-in font, which has no
    // file: then nothing is baked and the page draws from the interface ladder). A change re-bakes.
    void SetFaces(const std::string& regular, const std::string& bold);

    // Bakes the page's sizes (kLibraryPageSizes, kLibraryPageBoldSizes) at `scale` with the base
    // characters plus `extra` (sorted, below U+0800). Returns at once when the atlas already holds
    // exactly that. False when nothing usable is baked.
    bool Bake(float scale, const std::vector<ImWchar16>& extra);

    // The rung for a CSS size, or null.
    ImFont* Find(float css, bool bold);

    ImFontAtlas* Atlas();
}
