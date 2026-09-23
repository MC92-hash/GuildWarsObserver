#pragma once
#include <imgui.h>

struct ReplayHotkeys
{
    // Transport
    int rewind5s    = ImGuiKey_LeftArrow;
    int forward5s   = ImGuiKey_RightArrow;
    int playPause   = ImGuiKey_Space;

    // Overlay toggles
    int toggleRangeRings    = ImGuiKey_R;
    int toggleSkillLasers   = ImGuiKey_L;
    int toggleDrawingBar    = ImGuiKey_K;
    int toggleMoralePanel   = ImGuiKey_M;
    int toggleEventTimeline = ImGuiKey_T;
    int toggleLordDamage    = ImGuiKey_G;
    int toggleAutoCamera    = ImGuiKey_A;
    int toggleFogOfWar      = ImGuiKey_F;
    int toggleTopView       = ImGuiKey_V;
    int togglePianoRoll     = ImGuiKey_P;
    int toggleHeatmap       = ImGuiKey_H;
    int toggleMinimap       = ImGuiKey_U;
    int addBookmark         = ImGuiKey_B;
    int exitFollowMode      = ImGuiKey_Escape;

    // Camera movement
    int camForward    = ImGuiKey_W;
    int camBackward   = ImGuiKey_S;
    int camStrafeLeft = ImGuiKey_Q;
    int camStrafeRight = ImGuiKey_D;

    // Camera options
    bool invertMouseX = false;
    bool invertMouseY = false;

    void ResetToDefaults() { *this = ReplayHotkeys{}; }

    static ReplayHotkeys& Get();
    void Save() const;
    void Load();

    // Convert an ImGuiKey to a Win32 virtual-key code for use with
    // GetAsyncKeyState / InputManager::IsKeyDown.  Returns 0 on failure.
    static UINT ImGuiKeyToVK(int imguiKey);

    // Check whether an ImGuiKey value is a valid bindable key
    // (keyboard key or mouse button, but not mouse wheel).
    static bool IsValidBindableKey(int k);
};


// THE WARDROBE'S OWN KEYS. Fixed and not rebindable - they are single letters and they act inside
// one sub-application only, while nothing is being typed into - so both places that list the
// application's shortcuts show them READ-ONLY, and both read them from here.
struct WardrobeShortcut { const char* keys; const char* what; };
inline constexpr WardrobeShortcut kWardrobeShortcuts[] = {
    {"Esc",             "Go up one level, or leave the Wardrobe"},
    {"Enter",           "Open the character the keyboard is on"},
    {"Arrow keys, Tab", "Move between tiles, swatches and rows"},
    {"Ctrl + Z / Y",    "Undo, redo"},
    {"Ctrl + S",        "Save look"},
    {"F",               "Favourite this character"},
    {"1 - 5",           "Head, Chest, Hands, Legs, Feet"},
    {"R",               "Reset the view"},
};

// Shared ImGui widget: button that captures a key press for rebinding.
// Returns true the frame a new key is captured.
// When modernChrome is true, uses the GW Observer setup/licence panel styling (replay Preferences modals).
bool HotkeyInput(const char* label, int* key, bool modernChrome = false);
