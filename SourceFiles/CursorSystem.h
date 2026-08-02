#pragma once
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
    Wait,          // app or replay still loading
    DrawPointer,   // drawing toolbar open; cursor / arrow / circle / freehand
    EraseIdle,     // eraser armed, not over anything erasable
    EraseTarget,   // eraser armed and over something it would delete
    CameraLook,    // drawing toolbar open, right mouse held
    DrawActive,    // annotation tool dragging out a shape
    Hidden         // no cursor (during camera pan)
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
    HCURSOR wait        = nullptr;
    HCURSOR drawPointer = nullptr;
    HCURSOR eraseIdle   = nullptr;
    HCURSOR eraseTarget = nullptr;
    HCURSOR cameraLook  = nullptr;
    HCURSOR drawActive  = nullptr;

    bool loaded = false;

    // Builds an HCURSOR from an uncompressed 32-bit DDS under Textures/.
    // The GW cursor art is BGRA with no mip-independent header quirks, so a
    // straight blit into a DIB section is enough.
    // targetSize scales the art down to match the rest of the cursor set
    // (0 keeps the source size). Hotspots are given in source pixels and are
    // scaled with the image.
    static HCURSOR CursorFromDDS(const std::filesystem::path& file,
                                 int hotX, int hotY, int targetSize = 0)
    {
        std::ifstream f(file, std::ios::binary);
        if (!f) return nullptr;
        std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        if (buf.size() < 128) return nullptr;

        auto rd = [&](size_t off) {
            uint32_t v; std::memcpy(&v, buf.data() + off, 4); return v;
        };
        if (rd(0) != 0x20534444u) return nullptr;          // "DDS "
        const uint32_t h = rd(12), w = rd(16);
        const uint32_t bits = rd(88);                       // RGBBitCount
        if (w == 0 || h == 0 || bits != 32) return nullptr;
        if (buf.size() < 128 + size_t(w) * h * 4) return nullptr;

        // Optional box-filter downscale, done in premultiplied space so the
        // transparent surround does not bleed dark edges into the glyph.
        const uint32_t dw = (targetSize > 0) ? static_cast<uint32_t>(targetSize) : w;
        const uint32_t dh = (targetSize > 0) ? static_cast<uint32_t>(targetSize) : h;

        BITMAPV5HEADER bi{};
        bi.bV5Size        = sizeof(BITMAPV5HEADER);
        bi.bV5Width       = static_cast<LONG>(dw);
        bi.bV5Height      = -static_cast<LONG>(dh);         // top-down
        bi.bV5Planes      = 1;
        bi.bV5BitCount    = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask     = 0x00FF0000;
        bi.bV5GreenMask   = 0x0000FF00;
        bi.bV5BlueMask    = 0x000000FF;
        bi.bV5AlphaMask   = 0xFF000000;

        void* bits32 = nullptr;
        HDC hdc = ::GetDC(nullptr);
        HBITMAP colour = ::CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                            DIB_RGB_COLORS, &bits32, nullptr, 0);
        ::ReleaseDC(nullptr, hdc);
        if (!colour || !bits32) { if (colour) ::DeleteObject(colour); return nullptr; }

        const uint8_t* src = reinterpret_cast<const uint8_t*>(buf.data()) + 128;
        uint8_t* dst = static_cast<uint8_t*>(bits32);

        if (dw == w && dh == h)
        {
            std::memcpy(dst, src, size_t(w) * h * 4);
        }
        else
        {
            for (uint32_t y = 0; y < dh; ++y)
            {
                const uint32_t sy0 = y * h / dh;
                const uint32_t sy1 = std::max(sy0 + 1, (y + 1) * h / dh);
                for (uint32_t x = 0; x < dw; ++x)
                {
                    const uint32_t sx0 = x * w / dw;
                    const uint32_t sx1 = std::max(sx0 + 1, (x + 1) * w / dw);

                    uint32_t ab = 0, ag = 0, ar = 0, aa = 0, n = 0;
                    for (uint32_t sy = sy0; sy < sy1; ++sy)
                        for (uint32_t sx = sx0; sx < sx1; ++sx)
                        {
                            const uint8_t* p = src + (size_t(sy) * w + sx) * 4;
                            const uint32_t a = p[3];
                            ab += p[0] * a; ag += p[1] * a; ar += p[2] * a;
                            aa += a; ++n;
                        }

                    uint8_t* q = dst + (size_t(y) * dw + x) * 4;
                    if (aa == 0)
                    {
                        q[0] = q[1] = q[2] = q[3] = 0;
                    }
                    else
                    {
                        q[0] = static_cast<uint8_t>(ab / aa);
                        q[1] = static_cast<uint8_t>(ag / aa);
                        q[2] = static_cast<uint8_t>(ar / aa);
                        q[3] = static_cast<uint8_t>(aa / n);
                    }
                }
            }
            hotX = hotX * static_cast<int>(dw) / static_cast<int>(w);
            hotY = hotY * static_cast<int>(dh) / static_cast<int>(h);
        }

        // Mask is unused for 32-bit alpha cursors but must still exist.
        HBITMAP mask = ::CreateBitmap(dw, dh, 1, 1, nullptr);

        ICONINFO ii{};
        ii.fIcon    = FALSE;
        ii.xHotspot = static_cast<DWORD>(hotX);
        ii.yHotspot = static_cast<DWORD>(hotY);
        ii.hbmMask  = mask;
        ii.hbmColor = colour;

        HCURSOR cur = ::CreateIconIndirect(&ii);
        ::DeleteObject(colour);
        ::DeleteObject(mask);
        return cur;
    }

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

        // Drawing-toolbar cursors, from the GW art rather than .cur files.
        // The pointer's tip is its top-left corner; the camera-look and grab
        // cursors read best anchored at their centre.
        if (auto tex = cursorDir.parent_path() / "Toolbar"; std::filesystem::exists(tex))
        {
            drawPointer = CursorFromDDS(tex / "texture_8957.dds", 2, 2);
            // Pointing hand: hotspot on the fingertip, top-centre-left.
            eraseIdle   = CursorFromDDS(tex / "texture_134682.dds", 8, 1);
            eraseTarget = CursorFromDDS(tex / "texture_9077.dds", 1, 1);
            cameraLook  = CursorFromDDS(tex / "texture_130532.dds", 16, 16);
            drawActive  = CursorFromDDS(tex / "texture_134684.dds", 16, 16);
        }

        // Hourglass shown while the app or a replay is still loading. The art
        // is 64px and fills its canvas edge to edge, where the pointers only
        // use about half of theirs, so it needs to come down below 32 to look
        // the same weight on screen.
        {
            auto gameUi = cursorDir.parent_path() / "Game_UI" / "Cursor";
            wait = CursorFromDDS(gameUi / "texture_265561.dds", 32, 32, 24);
        }

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
        case CursorMode::Wait:        return wait        ? wait        : normal;
        case CursorMode::DrawPointer: return drawPointer ? drawPointer : normal;
        case CursorMode::EraseIdle:   return eraseIdle   ? eraseIdle   : clickable;
        case CursorMode::EraseTarget: return eraseTarget ? eraseTarget : eraseIdle;
        case CursorMode::CameraLook:  return cameraLook  ? cameraLook  : precision;
        case CursorMode::DrawActive:  return drawActive  ? drawActive  : move;
        case CursorMode::Hidden:      return nullptr;
        }
        return normal;
    }
};

inline GWOCursors g_Cursors;
inline CursorMode g_CurrentCursor = CursorMode::Normal;
inline bool       g_DraggingWindow = false;
inline bool       g_CursorInClientArea = true;

// Set once per tick, before any of the frame-skipping early-outs, and honoured
// by every window's WM_SETCURSOR. A per-window or render-time check would miss
// the cases where that window's render is skipped (unfocused, minimised, or
// yielding its frame budget to a replay).
inline bool       g_AppBusy = false;

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

// Shows the hourglass for as long as the object lives, then restores the
// previous mode.
//
// Use this around *synchronous* work that blocks the message loop. Such work
// never returns to the render loop, so a cursor mode set there would never be
// committed; setting it here sticks precisely because nothing will process
// WM_SETCURSOR to undo it until the work finishes.
struct ScopedWaitCursor
{
    CursorMode prev;

    ScopedWaitCursor() : prev(g_CurrentCursor)
    {
        g_CurrentCursor = CursorMode::Wait;
        if (g_Cursors.loaded && g_CursorInClientArea)
        {
            if (HCURSOR c = g_Cursors.Get(CursorMode::Wait))
                ::SetCursor(c);
        }
    }

    ~ScopedWaitCursor() { g_CurrentCursor = prev; }

    ScopedWaitCursor(const ScopedWaitCursor&)            = delete;
    ScopedWaitCursor& operator=(const ScopedWaitCursor&) = delete;
};

// Commit the current cursor mode to the OS.
inline void ApplyCursor()
{
    if (!g_Cursors.loaded) return;
    if (!g_CursorInClientArea) return;  // let Windows handle non-client cursors
    if (g_AppBusy)
    {
        if (HCURSOR w = g_Cursors.Get(CursorMode::Wait)) { ::SetCursor(w); return; }
    }
    if (g_CurrentCursor == CursorMode::Hidden) { ::SetCursor(nullptr); return; }
    HCURSOR cur = g_Cursors.Get(g_CurrentCursor);
    if (cur) ::SetCursor(cur);
}
