#pragma once
#include <imgui.h>

struct ReplayHotkeys
{
    // Transport
    int rewind5s    = ImGuiKey_LeftArrow;
    int forward5s   = ImGuiKey_RightArrow;
    int playPause   = ImGuiKey_Space;
    int speedUp     = ImGuiKey_KeypadAdd;       // next step of the playback speed menu
    int speedDown   = ImGuiKey_KeypadSubtract;  // previous step

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
    int toggleHighlights    = ImGuiKey_J;
    int nextHighlight       = ImGuiKey_PageDown;
    int prevHighlight       = ImGuiKey_PageUp;
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

    // The Trials module's keys and parameters are not here: they belong to that module alone and live in
    // their own file (settings/trials_keys.json, private Trials/TrialsKeys.h). Save keeps every entry of
    // hotkeys.json it does not write itself, so the old "trials" group stays until the module moves it.

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


// THE KEYBOARD AS THE SHORTCUTS READ IT. Every window that reads keys with GetAsyncKeyState goes through
// AsyncKeyDown, and decides whether it has the keyboard through KeyboardFront. A hidden self-test (Trials)
// replaces both: which kind of window is in front, and which keys are down. Nothing is sent to the OS.
struct KeyboardSeam
{
    enum class Front { None, Live, Recorded };   // what the test puts in front: a live window, a recorded replay
    bool active = false;
    Front front = Front::None;
    bool down[256] = {};
};
inline KeyboardSeam g_keyboardSeam;

inline bool AsyncKeyDown(UINT vk)
{
    if (g_keyboardSeam.active) return vk < 256 && g_keyboardSeam.down[vk];
    return vk != 0 && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}

// THE WARDROBE'S OWN KEYS. Fixed and not rebindable - they are single letters and they act inside
// one sub-application only, while nothing is being typed into - so both places that list the
// application's shortcuts show them READ-ONLY, and both read them from here. A row with no keys
// continues the row above it: both lists are narrow, so a long line is broken by hand.
struct WardrobeShortcut { const char* keys; const char* what; };
inline constexpr WardrobeShortcut kWardrobeShortcuts[] = {
    {"Esc",             "Go back, or leave the Wardrobe"},
    {"Enter",           "Open the selected character"},
    {"Arrow keys, Tab", "Move between tiles, rows and fields"},
    {"Ctrl + S",        "Save the character (Character section)"},
    {"",                "or the look (Armor section); a character"},
    {"",                "that is new or has a new profession or"},
    {"",                "gender is saved with its look"},
    {"Ctrl + Z",        "Undo"},
    {"Ctrl + Y",        "Redo"},
    {"1 - 5",           "Choose an armor slot (Armor section)"},
    {"F",               "Favorite the character"},
    {"R",               "Reset the view"},
};

// Shared ImGui widget: button that captures a key press for rebinding.
// Returns true the frame a new key is captured.
// When modernChrome is true, uses the GW Observer setup/licence panel styling (replay Preferences modals).
bool HotkeyInput(const char* label, int* key, bool modernChrome = false);
