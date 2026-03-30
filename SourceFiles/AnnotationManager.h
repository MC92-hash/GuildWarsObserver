#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <DirectXMath.h>

class Terrain;

class AnnotationManager {
public:
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
    bool        draw_mode_active       = false;
    DrawingTool active_tool            = SELECT;
    uint32_t    active_color           = 0x44FF88FF; // green ABGR

    std::vector<MapDrawing>                drawings;
    std::vector<std::vector<MapDrawing>>   undo_stack;
    std::vector<Bookmark>                  bookmarks;

    void Update(const Terrain* terrain, DirectX::XMMATRIX viewProj,
                float vpW, float vpH, DirectX::XMFLOAT3 camPos,
                float currentTimeSec);

    void RenderToolbar();
    void RenderDrawings(DirectX::XMMATRIX viewProj, float vpW, float vpH,
                        float currentTimeSec);

    void RenderBookmarkPanel(float currentTimeSec, float& timelineOut,
                             bool& isPlayingRef);

    void RenderTimelineMarkers(float trackX, float trackW,
                               float trackBarY, float trackH,
                               float maxTimeSec, float& timelineOut,
                               float displayTimeOffset);

    bool IsBookmarkPopupActive() const { return m_bookmarkPopupOpen || m_renamePopupOpen; }

private:
    float m_toastTimer     = 0.f;
    bool  m_toastTriggered = false;
    bool  m_prevToolbarVisible = false;

    MapDrawing m_inProgress{};
    bool       m_drawing        = false;
    bool       m_eraserDragging = false;
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
    void RenderClearConfirmDialog();
    void RenderBookmarkCreationPopup(bool& isPlayingRef);
    void RenderBookmarkRenamePopup();
};
