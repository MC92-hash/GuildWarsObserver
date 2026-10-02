# Navigation preview

Run `build.cmd` from a Visual Studio C++ environment on Windows (the script
initializes the installed VS 2022 Community toolchain).

This small harness renders the production `ui/app_navigation.h` through ImGui
and Direct3D WARP without opening the app or changing its settings. It loads the
approved atlas and the bundled Friz Quadrata font. It substitutes the texture
cache and font lookup; application routing and Wardrobe lifecycle are not tested.

It checks mouse activation of all three destinations at 1536, 800 and 320 pixels,
Space-key activation and opening the File menu. It writes PNG previews for these
widths, hover, pressed, keyboard focus and a 24-pixel UI font setting beside the
executable. Generated files are ignored.
