#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <DirectXMath.h>

class Terrain;
class ReplayPanelLayout;
struct ID3D11Device;

class AnnotationManager {
public:
    // SELECT is the "cursor" tool: it arms nothing, so it doubles as the
    // drawing-off state. Everything else means the next click draws.
    enum DrawingTool { SELECT, ARROW, CIRCLE, FREEHAND, ERASER };
    enum DrawingType { DT_ARROW, DT_CIRCLE, DT_FREEHAND };

    struct MapDrawing {
        DrawingType                         type;
        DirectX::XMFLOAT3                   world_start{};
        DirectX::XMFLOAT3                   world_end{};
        std::vector<DirectX::XMFLOAT3>      points;
        uint32_t                            color_abgr = 0;
        uint32_t                            timestamp_ms = 0;
        bool                                selected = false;
    };

    struct Bookmark {
        uint32_t    timestamp_ms = 0;
        std::string title;
    };

    bool        toolbar_visible        = false;
    bool        bookmarks_visible      = false;
    DrawingTool active_tool            = SELECT;
    uint32_t    active_color           = 0x44FF88FF; // green ABGR

    // Drawing is armed whenever a tool other than the cursor is selected.
    // There is deliberately no second "draw mode" flag to get out of sync
    // with the tool the toolbar is highlighting.
    bool IsDrawModeActive() const { return active_tool != SELECT; }

    // True while a shape is actually being dragged out (or erased through).
    bool IsDrawingStroke() const { return m_drawing || m_eraserDragging; }

    // True when the eraser is armed and sitting over something it would
    // delete, so the cursor can advertise the hit.
    bool IsEraserOverTarget() const { return m_eraserHover; }

    // True for the rest of the frame when Escape was used to disarm a tool,
    // so callers do not also treat that press as their own shortcut.
    bool EscapeConsumedThisFrame() const { return m_escConsumed; }

    // Keys the drawing tools claim while armed; callers suppress their own
    // bindings on these so the rest of the replay hotkeys keep working.
    bool ClaimsKey(int imguiKey) const;

    std::vector<MapDrawing>                drawings;
    std::vector<std::vector<MapDrawing>>   undo_stack;
    std::vector<Bookmark>                  bookmarks;

    std::function<void()> onBookmarksChanged;

    void Update(const Terrain* terrain, DirectX::XMMATRIX viewProj,
                float vpW, float vpH, DirectX::XMFLOAT3 camPos,
                float currentTimeSec);

    // Standalone horizontal strip, movable, defaulting under the ribbon.
    // layout may be null; when supplied the strip remembers where it was
    // dragged to, like the other replay panels.
    void RenderToolbar(ReplayPanelLayout* layout = nullptr);

    // Accent frame drawn around the viewport while a tool is armed.
    void RenderDrawModeIndicator();

    void Undo();
    void RequestClearAll();

    // Colour swatches, as ImGui IM_COL32 values. Selection goes through the
    // helpers so the ABGR conversion stays in one place.
    static constexpr int kSwatchCount = 5;
    static const uint32_t kSwatchColorsIM[kSwatchCount];
    void SetSwatch(int idx);
    bool IsSwatchActive(int idx) const;

    // Set by the host so the strip can draw game art; without it the strip
    // falls back to its vector glyphs.
    void SetIconDevice(ID3D11Device* dev) { m_iconDevice = dev; }
    void RenderDrawings(DirectX::XMMATRIX viewProj, float vpW, float vpH,
                        float currentTimeSec);

    // The bookmark list. Movable and position-persisted like the other
    // replay panels, so it cannot be trapped under the Event Timeline.
    void RenderBookmarkList(float currentTimeSec, float& timelineOut,
                            bool& isPlayingRef, ReplayPanelLayout* layout);

    void RenderTimelineMarkers(float trackX, float trackW,
                               float trackBarY, float trackH,
                               float maxTimeSec, float& timelineOut,
                               float displayTimeOffset);

    // Selects a tool and remembers it as the one to re-arm on reopen.
    void SetTool(DrawingTool t);

    void BeginAddBookmark();

    void AddBookmarkDirect(uint32_t timestampMs);

    bool IsBookmarkPopupActive() const { return m_bookmarkPopupOpen || m_renamePopupOpen; }

private:
    float m_toastTimer     = 0.f;
    bool  m_toastTriggered = false;
    bool  m_prevToolbarVisible = false;
    bool  m_escConsumed        = false;
    ID3D11Device* m_iconDevice = nullptr;
    // Re-armed when the toolbar is reopened, so the strip comes back to the
    // tool that was last actually used rather than to the cursor.
    DrawingTool m_lastDrawTool = ARROW;

    MapDrawing m_inProgress{};
    bool       m_drawing        = false;
    bool       m_eraserDragging = false;
    bool       m_eraserHover    = false;
    bool       m_showClearConfirm = false;

    // Bookmark creation popup state
    bool       m_bookmarkPopupOpen   = false;
    bool       m_bookmarkPopupJustOpened = false;
    char       m_bookmarkTitleBuf[41] = {};
    uint32_t   m_bookmarkPopupTimeMs = 0;
    bool       m_wasPlayingBeforeBookmark = false;
    DrawingTool m_toolBeforeBookmark = SELECT;
    bool       m_everCreatedBookmark = false;

    // Bookmark rename popup state
    bool       m_renamePopupOpen     = false;
    int        m_renameBookmarkIdx   = -1;
    char       m_renameTitleBuf[41]  = {};

    // Bookmark toast
    float      m_bookmarkToastTimer  = 0.f;

    void DrawToast();
    void DrawBookmarkToast();
    void HandleMouseInput(const Terrain* terrain, DirectX::XMMATRIX viewProj,
                          float vpW, float vpH, DirectX::XMFLOAT3 camPos,
                          float currentTimeSec);
    void HandleToolShortcuts();
    void DoUndo();
    void DoClearAll();
    void EraserHitTest(float scrX, float scrY, DirectX::XMMATRIX viewProj,
                       float vpW, float vpH);
    // Screen-space distance from a point to a drawing; shared by the erase
    // hit test and the hover check so both agree on what counts as a hit.
    float EraserDistance(const MapDrawing& d, float scrX, float scrY,
                         DirectX::XMMATRIX viewProj, float vpW, float vpH) const;
    void RenderClearConfirmDialog();
    void RenderBookmarkCreationPopup(bool& isPlayingRef);
    void RenderBookmarkRenamePopup();
};
