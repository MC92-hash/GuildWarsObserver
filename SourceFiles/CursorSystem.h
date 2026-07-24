#pragma once
#include <Windows.h>
#include <filesystem>
#include <string>

enum class CursorMode {
    Normal,
    Clickable,
    ResizeDiag1,   // NWSE (top-left / bottom-right)
    ResizeDiag2,   // NESW (top-right / bottom-left)
    ResizeH,
    ResizeV,
    Move,
    Precision,
    Link,
    Text,
    Hidden         // no cursor (during camera drag)
};

struct GWOCursors
{
    HCURSOR normal      = nullptr;
    HCURSOR clickable   = nullptr;
    HCURSOR resizeDiag1 = nullptr;
    HCURSOR resizeDiag2 = nullptr;
    HCURSOR resizeH     = nullptr;
    HCURSOR resizeV     = nullptr;
    HCURSOR move        = nullptr;
    HCURSOR precision   = nullptr;
    HCURSOR link        = nullptr;
    HCURSOR text        = nullptr;

    bool loaded = false;

    void Load()
    {
        if (loaded) return;

        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto dir = std::filesystem::path(exePath).parent_path();

        std::filesystem::path cursorDir;
        for (int i = 0; i < 5; i++)
        {
            auto candidate = dir / "Textures" / "cursor";
            if (std::filesystem::exists(candidate)) { cursorDir = candidate; break; }
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }

        if (cursorDir.empty()) return;

        auto load = [&](const char* filename) -> HCURSOR {
            auto p = cursorDir / filename;
            return (HCURSOR)LoadImageW(nullptr, p.wstring().c_str(),
                                       IMAGE_CURSOR, 0, 0,
                                       LR_LOADFROMFILE | LR_DEFAULTSIZE);
        };

        normal      = load("NORMAL.cur");
        clickable   = load("WIB.cur");
        resizeDiag1 = load("RESIZE 1.cur");
        resizeDiag2 = load("RESIZE 2.cur");
        resizeH     = load("H RESIZE.cur");
        resizeV     = load("V RESIZE.cur");
        move        = load("MOVE.cur");
        precision   = load("PRECISION.cur");
        link        = load("LINK SELECT.cur");
        text        = load("TEXT SELECT.cur");

        loaded = true;
    }

    HCURSOR Get(CursorMode mode) const
    {
        switch (mode)
        {
        case CursorMode::Normal:      return normal;
        case CursorMode::Clickable:   return clickable;
        case CursorMode::ResizeDiag1: return resizeDiag1;
        case CursorMode::ResizeDiag2: return resizeDiag2;
        case CursorMode::ResizeH:     return resizeH;
        case CursorMode::ResizeV:     return resizeV;
        case CursorMode::Move:        return move;
        case CursorMode::Precision:   return precision;
        case CursorMode::Link:        return link;
        case CursorMode::Text:        return text;
        case CursorMode::Hidden:      return nullptr;
        }
        return normal;
    }
};

inline GWOCursors g_Cursors;
inline CursorMode g_CurrentCursor = CursorMode::Normal;
inline bool       g_DraggingWindow = false;
inline bool       g_CursorInClientArea = true;

// Determine the correct cursor mode from ImGui state.
// Does NOT call SetCursor — callers can apply overrides before committing.
inline void UpdateCursorMode()
{
    if (!g_Cursors.loaded) { g_CurrentCursor = CursorMode::Normal; return; }

    // Sticky window-drag: once detected, hold Move until LMB is physically released.
    if (g_DraggingWindow)
    {
        if (!(GetKeyState(VK_LBUTTON) & 0x8000))
            g_DraggingWindow = false;
        else
        {
            g_CurrentCursor = CursorMode::Move;
            return;
        }
    }

    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) { g_CurrentCursor = CursorMode::Normal; return; }

    // Detect ImGui window being moved → latch into sticky drag
    if (ctx->MovingWindow != nullptr && (GetKeyState(VK_LBUTTON) & 0x8000))
    {
        g_DraggingWindow = true;
        g_CurrentCursor = CursorMode::Move;
        return;
    }

    ImGuiMouseCursor imCursor = ImGui::GetMouseCursor();

    switch (imCursor)
    {
    case ImGuiMouseCursor_TextInput:   g_CurrentCursor = CursorMode::Text;        return;
    case ImGuiMouseCursor_ResizeAll:   g_CurrentCursor = CursorMode::Move;        return;
    case ImGuiMouseCursor_ResizeNS:    g_CurrentCursor = CursorMode::ResizeV;     return;
    case ImGuiMouseCursor_ResizeEW:    g_CurrentCursor = CursorMode::ResizeH;     return;
    case ImGuiMouseCursor_ResizeNESW:  g_CurrentCursor = CursorMode::ResizeDiag2; return;
    case ImGuiMouseCursor_ResizeNWSE:  g_CurrentCursor = CursorMode::ResizeDiag1; return;
    case ImGuiMouseCursor_Hand:        g_CurrentCursor = CursorMode::Clickable;   return;
    default: break;
    }

    // Hovering a clickable widget
    if (ctx->HoveredId != 0 && !ctx->HoveredIdDisabled)
    {
        g_CurrentCursor = CursorMode::Clickable;
        return;
    }

    g_CurrentCursor = CursorMode::Normal;
}

// Commit the current cursor mode to the OS.
inline void ApplyCursor()
{
    if (!g_Cursors.loaded) return;
    if (!g_CursorInClientArea) return;  // let Windows handle non-client cursors
    if (g_CurrentCursor == CursorMode::Hidden) { ::SetCursor(nullptr); return; }
    HCURSOR cur = g_Cursors.Get(g_CurrentCursor);
    if (cur) ::SetCursor(cur);
}
