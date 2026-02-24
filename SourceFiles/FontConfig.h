#pragma once

struct FontEntry {
    const char* displayName;
    const char* fileName;       // relative to Textures/Fonts, or nullptr for special cases
    const char* boldFileName;   // bold variant, or nullptr if none
    bool isSystemFont;          // true = load from C:\Windows\Fonts instead
};

inline const FontEntry g_fontTable[] = {
    { "Inter",            "Inter-Regular.otf",                           "Inter-Bold.otf",                                  false },
    { "Segoe UI",         "segoeuithis.ttf",                            "segoeuithibd.ttf",                                false },
    { "Roboto",           "Roboto-Regular.ttf",                         "Roboto-Bold.ttf",                                 false },
    { "Helvetica Neue",   "HelveticaNeueRoman.otf",                     "HelveticaNeueBold.otf",                           false },
    { "Arial",            "arial.ttf",                                  "arialbd.ttf",                                     true  },
    { "Default (Built-in)", nullptr,                                    nullptr,                                           false },
    { "Fontin SmallCaps", "Fontin-SmallCaps.otf",                       nullptr,                                           false },
    { "Friz Quadrata",    "friz-quadrata-std-medium-5870338ec7ef8.otf", "friz-quadrata-std-bold-587034a220f9f.otf",        false },
    { "Montserrat",       "Montserrat-Regular.ttf",                     nullptr,                                           false },
    { "TB Font",          "TB-Font.ttf",                                nullptr,                                           false },
};

inline constexpr int g_fontTableCount = sizeof(g_fontTable) / sizeof(g_fontTable[0]);
