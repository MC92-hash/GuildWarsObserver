#pragma once

struct FontEntry {
    const char* displayName;
    const char* fileName;   // relative to Textures/Fonts, or nullptr for special cases
    bool isSystemFont;      // true = load from C:\Windows\Fonts instead
};

inline const FontEntry g_fontTable[] = {
    { "Inter",            "Inter-Regular.otf",                            false },
    { "Segoe UI",         "segoeuithis.ttf",                             false },
    { "Roboto",           "Roboto-Regular.ttf",                          false },
    { "Helvetica Neue",   "HelveticaNeueRoman.otf",                      false },
    { "Arial",            "arial.ttf",                                   true  },
    { "Default (Built-in)", nullptr,                                     false },
    { "Fontin SmallCaps", "Fontin-SmallCaps.otf",                        false },
    { "Friz Quadrata",    "friz-quadrata-std-medium-5870338ec7ef8.otf",  false },
    { "Montserrat",       "Montserrat-Regular.ttf",                      false },
    { "TB Font",          "TB-Font.ttf",                                 false },
};

inline constexpr int g_fontTableCount = sizeof(g_fontTable) / sizeof(g_fontTable[0]);
