#include "pch.h"
#include "AnnotationManager.h"
#include "Terrain.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace {
    constexpr float kBtnSize       = 28.f;
    constexpr float kIconSize      = 14.f;
    constexpr float kIconPad       = (kBtnSize - kIconSize) * 0.5f;
    constexpr float kBtnSpacing    = 3.f;
    constexpr float kSepMargin     = 3.f;
    constexpr float kSwatchSize    = 18.f;
    constexpr float kSwatchSpacing = 4.f;
    constexpr float kToastDuration = 4.f;
    constexpr float kToastFadeTime = 0.8f;

    constexpr ImU32 kGoldBorder    = IM_COL32(0xc8, 0xa0, 0x20, 0xFF);
    constexpr ImU32 kGoldBg        = IM_COL32(0x2a, 0x1a, 0x00, 0xFF);
    constexpr ImU32 kEraserBorder  = IM_COL32(0xaa, 0x66, 0x44, 0xFF);
    constexpr ImU32 kEraserBg      = IM_COL32(0x1a, 0x10, 0x10, 0xFF);
    constexpr ImU32 kSepColor      = IM_COL32(0x2a, 0x25, 0x10, 0xFF);

    constexpr ImU32 kIconActive    = IM_COL32(0xe8, 0xc8, 0x60, 0xFF);
    constexpr ImU32 kIconInactive  = IM_COL32(0x6a, 0x58, 0x30, 0xFF);
    constexpr ImU32 kIconHover     = IM_COL32(0xc8, 0xb8, 0x7a, 0xFF);
    constexpr ImU32 kIconEraserAct = IM_COL32(0xcc, 0x99, 0x77, 0xFF);

    constexpr float kFreehandMinDist  = 5.f;
    constexpr float kEraserThreshold  = 8.f;
    constexpr float kOpacityFadeStart = 60.f;
    constexpr float kOpacityMin       = 0.3f;
    constexpr float kLineThickness    = 2.f;
    constexpr float kArrowHeadSize    = 8.f;
    constexpr int   kCircleSegments   = 32;
    constexpr int   kMaxUndoDepth     = 50;

    constexpr float kCheckboxH        = 20.f;
    constexpr float kBookmarkPanelW   = 160.f;
    constexpr float kBookmarkToastDur = 3.f;

    // ── Projection helper ──

    static bool ProjectToScreen(XMMATRIX viewProj, float vpW, float vpH,
                                const XMFLOAT3& worldPos, float& scrX, float& scrY)
    {
        XMVECTOR clip = XMVector4Transform(
            XMVectorSet(worldPos.x, worldPos.y, worldPos.z, 1.f), viewProj);
        float w = XMVectorGetW(clip);
        if (w <= 0.001f) return false;
        float ndcX = XMVectorGetX(clip) / w;
        float ndcY = XMVectorGetY(clip) / w;
        scrX = (ndcX + 1.f) * 0.5f * vpW;
        scrY = (1.f - ndcY) * 0.5f * vpH;
        return (scrX > -200.f && scrX < vpW + 200.f &&
                scrY > -200.f && scrY < vpH + 200.f);
    }

    // ── Terrain raycast ──

    static bool ScreenToWorld(float scrX, float scrY, float vpW, float vpH,
                              XMMATRIX viewProj, const Terrain* terrain,
                              XMFLOAT3& outWorld)
    {
        if (!terrain) return false;

        float ndcX = (2.f * scrX / vpW) - 1.f;
        float ndcY = 1.f - (2.f * scrY / vpH);

        XMVECTOR det;
        XMMATRIX invVP = XMMatrixInverse(&det, viewProj);

        XMVECTOR nearPt = XMVector3TransformCoord(
            XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invVP);
        XMVECTOR farPt  = XMVector3TransformCoord(
            XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invVP);

        XMVECTOR rayDir = XMVector3Normalize(XMVectorSubtract(farPt, nearPt));
        XMFLOAT3 origin, dir;
        XMStoreFloat3(&origin, nearPt);
        XMStoreFloat3(&dir, rayDir);

        const auto& bounds = terrain->m_bounds;
        constexpr float step = 50.f;
        float tHitLow = -1.f, tHitHigh = -1.f;

        for (float t = 0.f; t < 50000.f; t += step)
        {
            float px = origin.x + dir.x * t;
            float py = origin.y + dir.y * t;
            float pz = origin.z + dir.z * t;

            if (px < bounds.map_min_x || px > bounds.map_max_x ||
                pz < bounds.map_min_z || pz > bounds.map_max_z)
            {
                if (t > step * 2.f) break;
                continue;
            }

            float terrainY = terrain->get_height_at(px, pz);
            if (py <= terrainY)
            {
                tHitLow  = t - step;
                tHitHigh = t;
                break;
            }
        }

        if (tHitLow < 0.f) return false;

        for (int i = 0; i < 16; ++i)
        {
            float tMid = (tHitLow + tHitHigh) * 0.5f;
            float px = origin.x + dir.x * tMid;
            float py = origin.y + dir.y * tMid;
            float pz = origin.z + dir.z * tMid;
            float terrainY = terrain->get_height_at(px, pz);
            if (py <= terrainY) tHitHigh = tMid;
            else                tHitLow  = tMid;
        }

        float tFinal = (tHitLow + tHitHigh) * 0.5f;
        outWorld.x = origin.x + dir.x * tFinal;
        outWorld.z = origin.z + dir.z * tFinal;
        outWorld.y = terrain->get_height_at(outWorld.x, outWorld.z);
        return true;
    }

    // ── Distance from point to line segment (screen space) ──

    static float PointToSegmentDist(float px, float py,
                                    float ax, float ay, float bx, float by)
    {
        float dx = bx - ax, dy = by - ay;
        float lenSq = dx * dx + dy * dy;
        if (lenSq < 0.001f)
            return std::sqrt((px - ax) * (px - ax) + (py - ay) * (py - ay));
        float t = ((px - ax) * dx + (py - ay) * dy) / lenSq;
        t = std::max(0.f, std::min(1.f, t));
        float cx = ax + t * dx, cy = ay + t * dy;
        return std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
    }

    // ── Icon-local coordinate mapping ──

    inline ImVec2 Ic(float x, float y, ImVec2 iconOrigin, float s)
    {
        return ImVec2(iconOrigin.x + x * s, iconOrigin.y + y * s);
    }

    // ── Icon draw functions ──

    void DrawIcon_Select(ImDrawList* dl, ImVec2 o, float s, ImU32 col)
    {
        ImVec2 pts[] = {
            Ic(1,0,o,s),  Ic(1,9,o,s),    Ic(3,7,o,s),
            Ic(5,11,o,s), Ic(6.5f,10.5f,o,s), Ic(4.5f,6.5f,o,s), Ic(7,6.5f,o,s)
        };
        dl->AddConvexPolyFilled(pts, 7, col);
    }

    void DrawIcon_Arrow(ImDrawList* dl, ImVec2 o, float s, ImU32 col)
    {
        dl->AddLine(Ic(2,12,o,s), Ic(11,3,o,s), col, 1.0f);
        ImVec2 head[] = { Ic(11,3,o,s), Ic(8.5f,3,o,s), Ic(11,5.5f,o,s) };
        dl->AddTriangleFilled(head[0], head[1], head[2], col);
    }

    void DrawIcon_Circle(ImDrawList* dl, ImVec2 o, float s, ImU32 col)
    {
        dl->AddCircle(Ic(7,7,o,s), 4.5f * s, col, 24, 1.0f);
    }

    void DrawIcon_Freehand(ImDrawList* dl, ImVec2 o, float s, ImU32 col)
    {
        constexpr int N = 20;
        ImVec2 pts[N];
        for (int i = 0; i < N; ++i)
        {
            float t = (float)i / (float)(N - 1);
            float x = 2.f + t * 10.f;
            float y = 11.f - 8.f * t + 2.f * std::sin(t * 3.14159f * 2.f);
            pts[i] = Ic(x, y, o, s);
        }
        dl->AddPolyline(pts, N, col, ImDrawFlags_None, 1.0f);
    }

    void DrawIcon_Eraser(ImDrawList* dl, ImVec2 o, float s, ImU32 col)
    {
        ImVec2 pts[] = { Ic(3,9,o,s), Ic(8,4,o,s), Ic(11,4,o,s), Ic(6,9,o,s) };
        dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, 1.0f);
        dl->AddLine(Ic(2,12,o,s), Ic(12,12,o,s), col, 0.5f);
    }

    void DrawIcon_Bookmark(ImDrawList* dl, ImVec2 o, float s, ImU32 col)
    {
        dl->AddLine(Ic(7,2,o,s), Ic(7,11,o,s), col, 1.0f);
        ImVec2 flag[] = { Ic(7,2,o,s), Ic(11,4,o,s), Ic(7,6,o,s) };
        dl->AddTriangleFilled(flag[0], flag[1], flag[2], col);
    }

    void DrawIcon_Undo(ImDrawList* dl, ImVec2 o, float s, ImU32 col)
    {
        constexpr int N = 16;
        constexpr float cx = 7.f, cy = 8.f, r = 4.f;
        constexpr float startDeg = 200.f, endDeg = 340.f;
        ImVec2 arcPts[N];
        for (int i = 0; i < N; ++i)
        {
            float deg = startDeg + (endDeg - startDeg) * (float)i / (float)(N - 1);
            float rad = deg * 3.14159f / 180.f;
            arcPts[i] = Ic(cx + r * std::cos(rad), cy + r * std::sin(rad), o, s);
        }
        dl->AddPolyline(arcPts, N, col, ImDrawFlags_None, 1.0f);
        float endRad = endDeg * 3.14159f / 180.f;
        float ex = cx + r * std::cos(endRad);
        float ey = cy + r * std::sin(endRad);
        ImVec2 ah[] = { Ic(ex,ey,o,s), Ic(ex-2.f,ey-1.5f,o,s), Ic(ex+0.5f,ey-2.f,o,s) };
        dl->AddTriangleFilled(ah[0], ah[1], ah[2], col);
    }

    void DrawIcon_ClearAll(ImDrawList* dl, ImVec2 o, float s, ImU32 col)
    {
        dl->AddRect(Ic(2,1,o,s), Ic(12,13,o,s), col, 1.0f * s, 0, 1.0f);
        dl->AddLine(Ic(4,5,o,s),   Ic(10,5,o,s),   col, 0.8f);
        dl->AddLine(Ic(4,7.5f,o,s), Ic(10,7.5f,o,s), col, 0.8f);
        dl->AddLine(Ic(4,10,o,s),  Ic(8,10,o,s),   col, 0.8f);
    }

    using IconDrawFn = void(*)(ImDrawList*, ImVec2, float, ImU32);

    struct ToolEntry {
        IconDrawFn  drawIcon;
        const char* tooltip;
        AnnotationManager::DrawingTool tool;
        bool isSeparator;
        bool isStub;
        bool isBookmark;
    };

    constexpr int kBookmarkEntryIdx = 8;

    static const ToolEntry kToolEntries[] = {
        { DrawIcon_Select,   "Select",           AnnotationManager::SELECT,   false, false, false },
        { nullptr,           nullptr,            AnnotationManager::SELECT,   true,  false, false },
        { DrawIcon_Arrow,    "Arrow",            AnnotationManager::ARROW,    false, false, false },
        { DrawIcon_Circle,   "Circle (C)",       AnnotationManager::CIRCLE,   false, false, false },
        { DrawIcon_Freehand, "Freehand (F)",     AnnotationManager::FREEHAND, false, false, false },
        { nullptr,           nullptr,            AnnotationManager::SELECT,   true,  false, false },
        { DrawIcon_Eraser,   "Eraser",           AnnotationManager::ERASER,   false, false, false },
        { nullptr,           nullptr,            AnnotationManager::SELECT,   true,  false, false },
        { DrawIcon_Bookmark, "Add bookmark",     AnnotationManager::SELECT,   false, false, true  },
        { nullptr,           nullptr,            AnnotationManager::SELECT,   true,  false, false },
        { DrawIcon_Undo,     "Undo (Ctrl+Z)",    AnnotationManager::SELECT,   false, false, false },
        { DrawIcon_ClearAll, "Clear All",        AnnotationManager::SELECT,   false, false, false },
    };

    constexpr int kUndoEntryIdx    = 10;
    constexpr int kClearAllEntryIdx = 11;

    struct SwatchEntry { ImU32 color; };
    static const SwatchEntry kSwatches[] = {
        { IM_COL32(0xFF, 0xAA, 0x00, 0xFF) },
        { IM_COL32(0xFF, 0x44, 0x44, 0xFF) },
        { IM_COL32(0x44, 0xAA, 0xFF, 0xFF) },
        { IM_COL32(0x44, 0xFF, 0x88, 0xFF) },
        { IM_COL32(0xFF, 0xFF, 0xFF, 0xFF) },
    };

    ImU32 SwatchToABGR(ImU32 imcol)
    {
        uint8_t r = (imcol >>  0) & 0xFF;
        uint8_t g = (imcol >>  8) & 0xFF;
        uint8_t b = (imcol >> 16) & 0xFF;
        uint8_t a = (imcol >> 24) & 0xFF;
        return (a) | (b << 8) | (g << 16) | (r << 24);
    }

    ImU32 ABGRtoImCol(uint32_t abgr)
    {
        uint8_t a = (abgr >>  0) & 0xFF;
        uint8_t b = (abgr >>  8) & 0xFF;
        uint8_t g = (abgr >> 16) & 0xFF;
        uint8_t r = (abgr >> 24) & 0xFF;
        return IM_COL32(r, g, b, a);
    }

    AnnotationManager::DrawingType ToolToDrawingType(AnnotationManager::DrawingTool t)
    {
        switch (t)
        {
        case AnnotationManager::ARROW:    return AnnotationManager::DT_ARROW;
        case AnnotationManager::CIRCLE:   return AnnotationManager::DT_CIRCLE;
        case AnnotationManager::FREEHAND: return AnnotationManager::DT_FREEHAND;
        default:                          return AnnotationManager::DT_ARROW;
        }
    }

    void FormatTimeMMSS(uint32_t ms, char* buf, size_t bufSize)
    {
        int totalSec = (int)(ms / 1000);
        int m = totalSec / 60;
        int s = totalSec % 60;
        snprintf(buf, bufSize, "%02d:%02d", m, s);
    }
}

// ════════════════════════════════════════════════════════════════════════
// Update
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::Update(const Terrain* terrain, XMMATRIX viewProj,
                               float vpW, float vpH, XMFLOAT3 camPos,
                               float currentTimeSec)
{
    if (toolbar_visible && !m_prevToolbarVisible)
    {
        m_toastTimer     = kToastDuration;
        m_toastTriggered = true;
    }
    m_prevToolbarVisible = toolbar_visible;

    if (toolbar_visible && draw_mode_active)
    {
        if (m_drawing || m_eraserDragging)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        else
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    if (draw_mode_active && toolbar_visible)
        HandleToolShortcuts();

    if (draw_mode_active && toolbar_visible)
        HandleMouseInput(terrain, viewProj, vpW, vpH, camPos, currentTimeSec);
}

// ════════════════════════════════════════════════════════════════════════
// HandleToolShortcuts
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::HandleToolShortcuts()
{
    if (ImGui::GetIO().WantTextInput) return;

    if (ImGui::IsKeyPressed(ImGuiKey_C)) active_tool = CIRCLE;
    if (ImGui::IsKeyPressed(ImGuiKey_F)) active_tool = FREEHAND;

    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
        DoUndo();
}

// ════════════════════════════════════════════════════════════════════════
// HandleMouseInput
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::HandleMouseInput(const Terrain* terrain, XMMATRIX viewProj,
                                         float vpW, float vpH, XMFLOAT3 camPos,
                                         float currentTimeSec)
{
    if (ImGui::GetIO().WantCaptureMouse) return;
    if (active_tool == SELECT) return;

    ImVec2 mousePos = ImGui::GetMousePos();

    if (active_tool == ERASER)
    {
        if (ImGui::IsMouseClicked(0))
        {
            m_eraserDragging = true;
            EraserHitTest(mousePos.x, mousePos.y, viewProj, vpW, vpH);
        }
        if (ImGui::IsMouseDown(0) && m_eraserDragging)
            EraserHitTest(mousePos.x, mousePos.y, viewProj, vpW, vpH);
        if (ImGui::IsMouseReleased(0))
            m_eraserDragging = false;
        return;
    }

    if (ImGui::IsMouseClicked(0) && !m_drawing)
    {
        XMFLOAT3 worldPos;
        if (!ScreenToWorld(mousePos.x, mousePos.y, vpW, vpH, viewProj, terrain, worldPos))
            return;

        if (undo_stack.size() >= kMaxUndoDepth)
            undo_stack.erase(undo_stack.begin());
        undo_stack.push_back(drawings);

        m_inProgress = MapDrawing{};
        m_inProgress.type         = ToolToDrawingType(active_tool);
        m_inProgress.world_start  = worldPos;
        m_inProgress.world_end    = worldPos;
        m_inProgress.color_abgr   = active_color;
        m_inProgress.timestamp_ms = (uint32_t)(currentTimeSec * 1000.f);

        if (m_inProgress.type == DT_FREEHAND)
            m_inProgress.points.push_back(worldPos);

        m_drawing = true;
    }

    if (ImGui::IsMouseDown(0) && m_drawing)
    {
        XMFLOAT3 worldPos;
        if (ScreenToWorld(mousePos.x, mousePos.y, vpW, vpH, viewProj, terrain, worldPos))
        {
            m_inProgress.world_end = worldPos;

            if (m_inProgress.type == DT_FREEHAND && !m_inProgress.points.empty())
            {
                auto& last = m_inProgress.points.back();
                float dx = worldPos.x - last.x;
                float dz = worldPos.z - last.z;
                if (dx * dx + dz * dz >= kFreehandMinDist * kFreehandMinDist)
                    m_inProgress.points.push_back(worldPos);
            }
        }
    }

    if (ImGui::IsMouseReleased(0) && m_drawing)
    {
        XMFLOAT3 worldPos;
        if (ScreenToWorld(mousePos.x, mousePos.y, vpW, vpH, viewProj, terrain, worldPos))
            m_inProgress.world_end = worldPos;

        if (m_inProgress.type == DT_FREEHAND && m_inProgress.points.size() < 2)
        {
            if (!undo_stack.empty()) undo_stack.pop_back();
        }
        else
        {
            drawings.push_back(m_inProgress);
        }

        m_drawing = false;
    }
}

// ════════════════════════════════════════════════════════════════════════
// EraserHitTest
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::EraserHitTest(float scrX, float scrY,
                                      XMMATRIX viewProj, float vpW, float vpH)
{
    bool snapshotTaken = false;

    for (int i = (int)drawings.size() - 1; i >= 0; --i)
    {
        const auto& d = drawings[i];
        float minDist = 1e9f;

        if (d.type == DT_ARROW)
        {
            float sx1, sy1, sx2, sy2;
            if (ProjectToScreen(viewProj, vpW, vpH, d.world_start, sx1, sy1) &&
                ProjectToScreen(viewProj, vpW, vpH, d.world_end, sx2, sy2))
                minDist = PointToSegmentDist(scrX, scrY, sx1, sy1, sx2, sy2);
        }
        else if (d.type == DT_CIRCLE)
        {
            float sx1, sy1, sx2, sy2;
            if (ProjectToScreen(viewProj, vpW, vpH, d.world_start, sx1, sy1) &&
                ProjectToScreen(viewProj, vpW, vpH, d.world_end, sx2, sy2))
            {
                float cx = (sx1 + sx2) * 0.5f;
                float cy = (sy1 + sy2) * 0.5f;
                float r = std::sqrt((sx2 - sx1) * (sx2 - sx1) + (sy2 - sy1) * (sy2 - sy1)) * 0.5f;
                float dist = std::sqrt((scrX - cx) * (scrX - cx) + (scrY - cy) * (scrY - cy));
                minDist = std::abs(dist - r);
            }
        }
        else if (d.type == DT_FREEHAND)
        {
            for (size_t j = 0; j + 1 < d.points.size(); ++j)
            {
                float sx1, sy1, sx2, sy2;
                if (ProjectToScreen(viewProj, vpW, vpH, d.points[j], sx1, sy1) &&
                    ProjectToScreen(viewProj, vpW, vpH, d.points[j + 1], sx2, sy2))
                    minDist = std::min(minDist, PointToSegmentDist(scrX, scrY, sx1, sy1, sx2, sy2));
            }
        }

        if (minDist < kEraserThreshold)
        {
            if (!snapshotTaken)
            {
                if (undo_stack.size() >= kMaxUndoDepth)
                    undo_stack.erase(undo_stack.begin());
                undo_stack.push_back(drawings);
                snapshotTaken = true;
            }
            drawings.erase(drawings.begin() + i);
        }
    }
}

// ════════════════════════════════════════════════════════════════════════
// Undo / Clear All
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::DoUndo()
{
    if (undo_stack.empty()) return;
    drawings = undo_stack.back();
    undo_stack.pop_back();
}

void AnnotationManager::DoClearAll()
{
    if (drawings.empty()) return;
    if (undo_stack.size() >= kMaxUndoDepth)
        undo_stack.erase(undo_stack.begin());
    undo_stack.push_back(drawings);
    drawings.clear();
}

void AnnotationManager::RenderClearConfirmDialog()
{
    if (m_showClearConfirm)
        ImGui::OpenPopup("ClearAllConfirm");

    if (ImGui::BeginPopupModal("ClearAllConfirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
    {
        ImGui::Text("Clear all drawings?");
        ImGui::Separator();
        if (ImGui::Button("Yes", ImVec2(80, 0)))
        {
            DoClearAll();
            m_showClearConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(80, 0)))
        {
            m_showClearConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    else
    {
        m_showClearConfirm = false;
    }
}

// ════════════════════════════════════════════════════════════════════════
// RenderDrawings
// ════════════════════════════════════════════════════════════════════════

static void RenderOneDrawing(const AnnotationManager::MapDrawing& d,
                             XMMATRIX viewProj, float vpW, float vpH,
                             float currentTimeSec, ImDrawList* dl)
{
    float ageSec = std::abs(currentTimeSec - d.timestamp_ms / 1000.f);
    float alpha = 1.f;
    if (ageSec > kOpacityFadeStart)
    {
        float fade = (ageSec - kOpacityFadeStart) / kOpacityFadeStart;
        alpha = std::max(kOpacityMin, 1.f - fade * (1.f - kOpacityMin));
    }

    ImU32 baseCol = ABGRtoImCol(d.color_abgr);
    uint8_t a = (uint8_t)((float)((baseCol >> 24) & 0xFF) * alpha);
    ImU32 col = (baseCol & 0x00FFFFFF) | ((ImU32)a << 24);

    if (d.type == AnnotationManager::DT_ARROW)
    {
        float sx1, sy1, sx2, sy2;
        if (!ProjectToScreen(viewProj, vpW, vpH, d.world_start, sx1, sy1)) return;
        if (!ProjectToScreen(viewProj, vpW, vpH, d.world_end, sx2, sy2)) return;

        dl->AddLine(ImVec2(sx1, sy1), ImVec2(sx2, sy2), col, kLineThickness);

        float dx = sx2 - sx1, dy = sy2 - sy1;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.f)
        {
            float ux = dx / len, uy = dy / len;
            float px = -uy, py = ux;
            ImVec2 tip(sx2, sy2);
            ImVec2 left(sx2 - ux * kArrowHeadSize + px * kArrowHeadSize * 0.4f,
                        sy2 - uy * kArrowHeadSize + py * kArrowHeadSize * 0.4f);
            ImVec2 right(sx2 - ux * kArrowHeadSize - px * kArrowHeadSize * 0.4f,
                         sy2 - uy * kArrowHeadSize - py * kArrowHeadSize * 0.4f);
            dl->AddTriangleFilled(tip, left, right, col);
        }
    }
    else if (d.type == AnnotationManager::DT_CIRCLE)
    {
        float sx1, sy1, sx2, sy2;
        if (!ProjectToScreen(viewProj, vpW, vpH, d.world_start, sx1, sy1)) return;
        if (!ProjectToScreen(viewProj, vpW, vpH, d.world_end, sx2, sy2)) return;

        float cx = (sx1 + sx2) * 0.5f;
        float cy = (sy1 + sy2) * 0.5f;
        float r = std::sqrt((sx2 - sx1) * (sx2 - sx1) + (sy2 - sy1) * (sy2 - sy1)) * 0.5f;
        if (r > 1.f)
            dl->AddCircle(ImVec2(cx, cy), r, col, kCircleSegments, kLineThickness);
    }
    else if (d.type == AnnotationManager::DT_FREEHAND)
    {
        if (d.points.size() < 2) return;
        std::vector<ImVec2> scrPts;
        scrPts.reserve(d.points.size());
        for (auto& pt : d.points)
        {
            float sx, sy;
            if (ProjectToScreen(viewProj, vpW, vpH, pt, sx, sy))
                scrPts.push_back(ImVec2(sx, sy));
        }
        if (scrPts.size() >= 2)
            dl->AddPolyline(scrPts.data(), (int)scrPts.size(), col, ImDrawFlags_None, kLineThickness);
    }
}

void AnnotationManager::RenderDrawings(XMMATRIX viewProj, float vpW, float vpH,
                                       float currentTimeSec)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    for (auto& d : drawings)
        RenderOneDrawing(d, viewProj, vpW, vpH, currentTimeSec, dl);

    if (m_drawing)
        RenderOneDrawing(m_inProgress, viewProj, vpW, vpH, currentTimeSec, dl);
}

// ════════════════════════════════════════════════════════════════════════
// RenderToolbar — with draw-mode checkbox + bookmark action button
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::RenderToolbar()
{
    if (m_toastTimer > 0.f)
        DrawToast();
    if (m_bookmarkToastTimer > 0.f)
        DrawBookmarkToast();

    if (!toolbar_visible) return;

    float totalH = kCheckboxH + kSepMargin * 2.f + 1.f;
    for (auto& e : kToolEntries)
        totalH += e.isSeparator ? (kSepMargin * 2.f + 1.f) : (kBtnSize + kBtnSpacing);
    float swatchSepH   = kSepMargin * 2.f + 1.f;
    float swatchTotalH  = 5 * (kSwatchSize + kSwatchSpacing);
    float windowPadY    = 6.f * 2.f;
    float panelH = totalH + swatchSepH + swatchTotalH + windowPadY + 4.f;

    ImVec2 display = ImGui::GetIO().DisplaySize;
    float panelW   = kBtnSize + 12.f;
    float panelX   = 4.f;
    float panelY   = (display.y - panelH) * 0.5f;

    ImGui::SetNextWindowPos(ImVec2(panelX, panelY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 6.f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoFocusOnAppearing
                           | ImGuiWindowFlags_NoNavInputs;

    if (!ImGui::Begin("##DrawToolbar", nullptr, flags))
    {
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        return;
    }

    // ── Draw-mode checkbox at the top ──
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.78f, 0.63f, 0.13f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.12f, 0.10f, 0.06f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.18f, 0.14f, 0.06f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.22f, 0.18f, 0.06f, 0.9f));

    ImGui::Checkbox("##DrawMode", &draw_mode_active);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Enable drawing mode");

    ImGui::PopStyleColor(4);

    ImGui::Dummy(ImVec2(kBtnSize, kSepMargin));
    {
        ImDrawList* dlSep = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dlSep->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + kBtnSize, p.y), kSepColor, 0.5f);
    }
    ImGui::Dummy(ImVec2(kBtnSize, kSepMargin));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float iconScale = 1.0f;

    for (int idx = 0; idx < (int)(sizeof(kToolEntries)/sizeof(kToolEntries[0])); ++idx)
    {
        auto& entry = kToolEntries[idx];

        if (entry.isSeparator)
        {
            ImGui::Dummy(ImVec2(kBtnSize, kSepMargin));
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + kBtnSize, p.y),
                        kSepColor, 0.5f);
            ImGui::Dummy(ImVec2(kBtnSize, kSepMargin));
            continue;
        }

        bool isBookmarkBtn = entry.isBookmark;
        bool isDrawingTool = !entry.isStub && !isBookmarkBtn && (entry.tool != SELECT || idx == 0);
        bool isActive = isDrawingTool && (active_tool == entry.tool) && (idx != kUndoEntryIdx) && (idx != kClearAllEntryIdx);
        bool isEraser = (entry.tool == ERASER);

        ImVec2 btnPos = ImGui::GetCursorScreenPos();
        ImVec2 iconOrigin(btnPos.x + kIconPad, btnPos.y + kIconPad);

        ImGui::PushID(idx);
        ImGui::InvisibleButton("##tb", ImVec2(kBtnSize, kBtnSize));
        bool hovered = ImGui::IsItemHovered();

        if (ImGui::IsItemClicked())
        {
            if (isBookmarkBtn)
            {
                // Momentary action: open bookmark creation popup
                m_bookmarkPopupOpen = true;
                m_bookmarkPopupJustOpened = true;
            }
            else if (idx == kUndoEntryIdx)
                DoUndo();
            else if (idx == kClearAllEntryIdx)
                m_showClearConfirm = true;
            else if (!entry.isStub)
                active_tool = entry.tool;
        }
        ImGui::PopID();

        ImVec2 btnMax(btnPos.x + kBtnSize, btnPos.y + kBtnSize);

        if (isActive)
        {
            ImU32 bg     = isEraser ? kEraserBg     : kGoldBg;
            ImU32 border = isEraser ? kEraserBorder  : kGoldBorder;
            dl->AddRectFilled(btnPos, btnMax, bg, 3.f);
            dl->AddRect(btnPos, btnMax, border, 3.f, 0, 1.0f);

            ImU32 iconCol = isEraser ? kIconEraserAct : kIconActive;
            if (entry.drawIcon)
                entry.drawIcon(dl, iconOrigin, iconScale, iconCol);
        }
        else
        {
            if (hovered)
                dl->AddRectFilled(btnPos, btnMax, IM_COL32(0x1a, 0x16, 0x0a, 0xC0), 3.f);

            ImU32 iconCol = hovered ? kIconHover : kIconInactive;
            if (entry.drawIcon)
                entry.drawIcon(dl, iconOrigin, iconScale, iconCol);
        }

        if (hovered && entry.tooltip)
            ImGui::SetTooltip("%s", entry.tooltip);
    }

    // Separator before swatches
    ImGui::Dummy(ImVec2(kBtnSize, kSepMargin));
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + kBtnSize, p.y),
                    kSepColor, 0.5f);
    }
    ImGui::Dummy(ImVec2(kBtnSize, kSepMargin));

    const float swatchR = kSwatchSize * 0.5f;
    for (int i = 0; i < 5; ++i)
    {
        ImU32 col = kSwatches[i].color;
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        float cx = cursor.x + kBtnSize * 0.5f;
        float cy = cursor.y + swatchR;

        ImGui::PushID(i + 100);
        float hitLeft = cx - swatchR;
        ImGui::SetCursorScreenPos(ImVec2(hitLeft, cursor.y));
        ImGui::InvisibleButton("##sw", ImVec2(kSwatchSize, kSwatchSize));
        if (ImGui::IsItemClicked())
            active_color = SwatchToABGR(col);
        ImGui::PopID();

        dl->AddCircleFilled(ImVec2(cx, cy), swatchR, col, 24);

        if (active_color == SwatchToABGR(col))
        {
            dl->AddCircle(ImVec2(cx, cy), swatchR + 2.5f,
                          IM_COL32(255, 255, 255, 255), 24, 1.5f);
        }

        ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + kSwatchSize + kSwatchSpacing));
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    RenderClearConfirmDialog();
}

// ════════════════════════════════════════════════════════════════════════
// BeginAddBookmark — opens the bookmark creation popup from any caller
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::BeginAddBookmark()
{
    if (m_bookmarkPopupOpen) return;
    m_bookmarkPopupOpen = true;
    m_bookmarkPopupJustOpened = true;
}

// ════════════════════════════════════════════════════════════════════════
// AddBookmarkDirect — instantly create a bookmark without showing the popup
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::AddBookmarkDirect(uint32_t timestampMs)
{
    Bookmark bk;
    bk.timestamp_ms = timestampMs;

    char timeBuf[16];
    FormatTimeMMSS(timestampMs, timeBuf, sizeof(timeBuf));
    char defTitle[32];
    snprintf(defTitle, sizeof(defTitle), "Bookmark at %s", timeBuf);
    bk.title = defTitle;

    auto it = std::lower_bound(bookmarks.begin(), bookmarks.end(), bk,
        [](const Bookmark& a, const Bookmark& b){ return a.timestamp_ms < b.timestamp_ms; });
    bookmarks.insert(it, bk);
    if (onBookmarksChanged) onBookmarksChanged();

    if (!m_everCreatedBookmark)
    {
        m_everCreatedBookmark = true;
        bookmarks_visible = true;
        m_bookmarkToastTimer = kBookmarkToastDur;
    }
}

// ════════════════════════════════════════════════════════════════════════
// Bookmark creation popup
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::RenderBookmarkCreationPopup(bool& isPlayingRef)
{
    if (!m_bookmarkPopupOpen) return;

    if (m_bookmarkPopupJustOpened)
    {
        m_wasPlayingBeforeBookmark = isPlayingRef;
        if (isPlayingRef)
            isPlayingRef = false;

        m_toolBeforeBookmark = active_tool;
        memset(m_bookmarkTitleBuf, 0, sizeof(m_bookmarkTitleBuf));

        ImGui::OpenPopup("##BookmarkCreate");
        m_bookmarkPopupJustOpened = false;
    }

    ImVec2 display = ImGui::GetIO().DisplaySize;
    float popW = 240.f;
    float popH = 100.f;
    float toolbarRight = 4.f + kBtnSize + 12.f + 8.f;
    float popX = toolbarRight;
    float popY = (display.y - popH) * 0.5f;

    ImGui::SetNextWindowPos(ImVec2(popX, popY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(popW, 0.f));

    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.05f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.5f, 0.4f, 0.1f, 0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 8.f));

    bool open = true;
    if (ImGui::BeginPopupModal("##BookmarkCreate", &open,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove
                               | ImGuiWindowFlags_NoTitleBar))
    {
        char timeBuf[16];
        FormatTimeMMSS(m_bookmarkPopupTimeMs, timeBuf, sizeof(timeBuf));

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.63f, 0.13f, 1.f));
        ImGui::Text("Bookmark at  %s", timeBuf);
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.f);

        static bool needFocus = false;
        if (m_bookmarkTitleBuf[0] == '\0' && !ImGui::IsAnyItemActive())
            needFocus = true;
        if (needFocus)
        {
            ImGui::SetKeyboardFocusHere();
            needFocus = false;
        }

        ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.10f, 0.09f, 0.07f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,  ImVec4(0.15f, 0.13f, 0.08f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,   ImVec4(0.18f, 0.15f, 0.08f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg,  ImVec4(0.78f, 0.63f, 0.13f, 0.3f));
        bool entered = ImGui::InputText("##bkTitle", m_bookmarkTitleBuf,
                                        sizeof(m_bookmarkTitleBuf),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor(4);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool cancelled = false;
        bool confirmed = entered;

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.14f, 0.12f, 0.06f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.25f, 0.20f, 0.08f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.30f, 0.24f, 0.10f, 1.f));
        if (ImGui::Button("Cancel", ImVec2(70, 0)))
            cancelled = true;

        ImGui::SameLine(240.f - 10.f * 2.f - 60.f);
        if (ImGui::Button("Add", ImVec2(60, 0)))
            confirmed = true;
        ImGui::PopStyleColor(3);

        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            cancelled = true;

        if (confirmed)
        {
            Bookmark bk;
            bk.timestamp_ms = m_bookmarkPopupTimeMs;
            if (m_bookmarkTitleBuf[0] != '\0')
                bk.title = m_bookmarkTitleBuf;
            else
            {
                char defTitle[32];
                snprintf(defTitle, sizeof(defTitle), "Bookmark at %s", timeBuf);
                bk.title = defTitle;
            }

            auto it = std::lower_bound(bookmarks.begin(), bookmarks.end(), bk,
                [](const Bookmark& a, const Bookmark& b){ return a.timestamp_ms < b.timestamp_ms; });
            bookmarks.insert(it, bk);
            if (onBookmarksChanged) onBookmarksChanged();

            if (!m_everCreatedBookmark)
            {
                m_everCreatedBookmark = true;
                bookmarks_visible = true;
                m_bookmarkToastTimer = kBookmarkToastDur;
            }

            m_bookmarkPopupOpen = false;
            if (m_wasPlayingBeforeBookmark)
                isPlayingRef = true;
            active_tool = m_toolBeforeBookmark;
            ImGui::CloseCurrentPopup();
        }

        if (cancelled)
        {
            m_bookmarkPopupOpen = false;
            if (m_wasPlayingBeforeBookmark)
                isPlayingRef = true;
            active_tool = m_toolBeforeBookmark;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    else
    {
        if (m_bookmarkPopupOpen)
        {
            m_bookmarkPopupOpen = false;
            if (m_wasPlayingBeforeBookmark)
                isPlayingRef = true;
            active_tool = m_toolBeforeBookmark;
        }
    }

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ════════════════════════════════════════════════════════════════════════
// Bookmark rename popup
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::RenderBookmarkRenamePopup()
{
    if (m_renamePopupOpen && !ImGui::IsPopupOpen("##BookmarkRename"))
        ImGui::OpenPopup("##BookmarkRename");

    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.07f, 0.07f, 0.05f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.5f, 0.4f, 0.1f, 0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 8.f));

    ImGui::SetNextWindowSize(ImVec2(240.f, 0.f));
    if (ImGui::BeginPopupModal("##BookmarkRename", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
    {
        if (m_renameBookmarkIdx < 0 || m_renameBookmarkIdx >= (int)bookmarks.size())
        {
            m_renamePopupOpen = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            return;
        }

        auto& bk = bookmarks[m_renameBookmarkIdx];
        char timeBuf[16];
        FormatTimeMMSS(bk.timestamp_ms, timeBuf, sizeof(timeBuf));

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.63f, 0.13f, 1.f));
        ImGui::Text("Bookmark at  %s", timeBuf);
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.f);

        static bool rnFocus = false;
        if (!ImGui::IsAnyItemActive()) rnFocus = true;
        if (rnFocus) { ImGui::SetKeyboardFocusHere(); rnFocus = false; }

        ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.10f, 0.09f, 0.07f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,  ImVec4(0.15f, 0.13f, 0.08f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,   ImVec4(0.18f, 0.15f, 0.08f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg,  ImVec4(0.78f, 0.63f, 0.13f, 0.3f));
        bool entered = ImGui::InputText("##rnTitle", m_renameTitleBuf,
                                        sizeof(m_renameTitleBuf),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor(4);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape);
        bool confirmed = entered;

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.14f, 0.12f, 0.06f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.25f, 0.20f, 0.08f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.30f, 0.24f, 0.10f, 1.f));
        if (ImGui::Button("Cancel", ImVec2(70, 0)))
            cancelled = true;

        ImGui::SameLine(240.f - 10.f * 2.f - 60.f);
        if (ImGui::Button("Save", ImVec2(60, 0)))
            confirmed = true;
        ImGui::PopStyleColor(3);

        if (confirmed && m_renameTitleBuf[0] != '\0')
        {
            bk.title = m_renameTitleBuf;
            if (onBookmarksChanged) onBookmarksChanged();
            m_renamePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }
        else if (cancelled)
        {
            m_renamePopupOpen = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    else
    {
        m_renamePopupOpen = false;
    }

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ════════════════════════════════════════════════════════════════════════
// Bookmark panel (right side, 160px wide)
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::RenderBookmarkPanel(float currentTimeSec, float& timelineOut,
                                            bool& isPlayingRef)
{
    // Render bookmark creation popup (must run each frame regardless of panel)
    m_bookmarkPopupTimeMs = (uint32_t)(currentTimeSec * 1000.f);
    RenderBookmarkCreationPopup(isPlayingRef);
    RenderBookmarkRenamePopup();

    if (!bookmarks_visible) return;

    ImVec2 display = ImGui::GetIO().DisplaySize;
    float panelH = display.y - 80.f;
    float panelX = display.x - kBookmarkPanelW - 4.f;
    float panelY = 30.f;

    ImGui::SetNextWindowPos(ImVec2(panelX, panelY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kBookmarkPanelW, panelH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 6.f));

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar
                        | ImGuiWindowFlags_NoResize
                        | ImGuiWindowFlags_NoMove
                        | ImGuiWindowFlags_NoCollapse
                        | ImGuiWindowFlags_NoFocusOnAppearing
                        | ImGuiWindowFlags_NoNavInputs;

    if (!ImGui::Begin("##BookmarkPanel", nullptr, wf))
    {
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        return;
    }

    // Header
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.63f, 0.13f, 1.f));
    ImGui::Text("Bookmarks");
    ImGui::PopStyleColor();

    ImGui::SameLine(kBookmarkPanelW - 30.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.4f, 0.2f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.15f, 0.05f, 0.6f));
    if (ImGui::SmallButton("x##closeBk"))
        bookmarks_visible = false;
    ImGui::PopStyleColor(3);

    ImGui::Separator();

    // Find closest bookmark to current time for highlighting
    int closestIdx = -1;
    {
        uint32_t curMs = (uint32_t)(currentTimeSec * 1000.f);
        uint32_t bestDist = UINT32_MAX;
        for (int i = 0; i < (int)bookmarks.size(); ++i)
        {
            uint32_t d = (bookmarks[i].timestamp_ms > curMs)
                       ? (bookmarks[i].timestamp_ms - curMs)
                       : (curMs - bookmarks[i].timestamp_ms);
            if (d < bestDist) { bestDist = d; closestIdx = i; }
        }
    }

    if (bookmarks.empty())
    {
        ImGui::Spacing(); ImGui::Spacing();
        float w = ImGui::GetContentRegionAvail().x;

        const char* msg1 = "No bookmarks yet";
        ImVec2 ts1 = ImGui::CalcTextSize(msg1);
        ImGui::SetCursorPosX((w - ts1.x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.43f, 0.3f, 1.f));
        ImGui::TextUnformatted(msg1);
        ImGui::PopStyleColor();

        const char* msg2 = "Use the bookmark tool";
        ImVec2 ts2 = ImGui::CalcTextSize(msg2);
        ImGui::SetCursorPosX((w - ts2.x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.35f, 0.25f, 0.8f));
        ImGui::TextUnformatted(msg2);
        ImGui::PopStyleColor();

        const char* msg3 = "in the drawing toolbar";
        ImVec2 ts3 = ImGui::CalcTextSize(msg3);
        ImGui::SetCursorPosX((w - ts3.x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.35f, 0.25f, 0.8f));
        ImGui::TextUnformatted(msg3);
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::BeginChild("##BkList", ImVec2(0, 0), false);

        int removeIdx = -1;
        for (int i = 0; i < (int)bookmarks.size(); ++i)
        {
            auto& bk = bookmarks[i];
            char timeBuf[16];
            FormatTimeMMSS(bk.timestamp_ms, timeBuf, sizeof(timeBuf));

            bool isClosest = (i == closestIdx);

            ImGui::PushID(i);

            // Active bookmark highlight
            if (isClosest)
            {
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImDrawList* dlP = ImGui::GetWindowDrawList();
                float rowH = ImGui::GetFrameHeightWithSpacing() * 2.f + 4.f;
                dlP->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + kBookmarkPanelW - 16.f, p.y + rowH),
                                   IM_COL32(0x1a, 0x14, 0x00, 0x60), 3.f);
                dlP->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + 3.f, p.y + rowH),
                                   IM_COL32(0xc8, 0xa0, 0x20, 0xFF));
            }

            // Timestamp + title
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.63f, 0.13f, 1.f));
            ImGui::Text("[%s]", timeBuf);
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // Truncate title
            const float availW = ImGui::GetContentRegionAvail().x;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.68f, 0.58f, 1.f));
            if (ImGui::CalcTextSize(bk.title.c_str()).x > availW)
            {
                char trunc[44];
                snprintf(trunc, sizeof(trunc), "%.36s...", bk.title.c_str());
                ImGui::TextUnformatted(trunc);
            }
            else
            {
                ImGui::TextUnformatted(bk.title.c_str());
            }
            ImGui::PopStyleColor();

            // Action buttons
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.10f, 0.06f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.18f, 0.08f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.5f, 0.3f, 1.f));

            if (ImGui::SmallButton("jump"))
            {
                timelineOut = bk.timestamp_ms / 1000.f;
                isPlayingRef = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("rename"))
            {
                m_renamePopupOpen = true;
                m_renameBookmarkIdx = i;
                memset(m_renameTitleBuf, 0, sizeof(m_renameTitleBuf));
                strncpy(m_renameTitleBuf, bk.title.c_str(), sizeof(m_renameTitleBuf) - 1);
            }
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.3f, 0.2f, 1.f));
            if (ImGui::SmallButton("x"))
                removeIdx = i;
            ImGui::PopStyleColor();

            ImGui::PopStyleColor(3);

            ImGui::Spacing();
            ImGui::PopID();
        }

        if (removeIdx >= 0)
        {
            bookmarks.erase(bookmarks.begin() + removeIdx);
            if (onBookmarksChanged) onBookmarksChanged();
        }

        ImGui::EndChild();
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ════════════════════════════════════════════════════════════════════════
// Bookmark floating panel (compact, above the left of the timeline bar)
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::RenderBookmarkDrawer(float barX, float barY, float /*barW*/, float /*barH*/,
                                             float currentTimeSec, float& timelineOut,
                                             bool& isPlayingRef, float /*displayTimeOffset*/)
{
    m_bookmarkPopupTimeMs = (uint32_t)(currentTimeSec * 1000.f);
    RenderBookmarkCreationPopup(isPlayingRef);
    RenderBookmarkRenamePopup();

    if (!bookmarks_visible) return;

    constexpr float kPanelW = 240.f;
    constexpr float kRowH   = 22.f;
    constexpr float kHeaderH = 26.f;
    constexpr float kMaxListH = 180.f;
    constexpr float kPad = 8.f;

    int bkCount = (int)bookmarks.size();
    float listH = std::min(kMaxListH, std::max(kRowH, (float)bkCount * kRowH));
    float panelH = kHeaderH + listH + kPad;

    float panelX = barX + 8.f;
    float panelY = barY - panelH - 4.f;

    ImGui::SetNextWindowPos(ImVec2(panelX, panelY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kPanelW, panelH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.5f, 0.4f, 0.1f, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPad, 4.f));

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar
                        | ImGuiWindowFlags_NoResize
                        | ImGuiWindowFlags_NoMove
                        | ImGuiWindowFlags_NoCollapse
                        | ImGuiWindowFlags_NoFocusOnAppearing
                        | ImGuiWindowFlags_NoNavInputs
                        | ImGuiWindowFlags_NoScrollbar;

    if (!ImGui::Begin("##BookmarkFloatPanel", nullptr, wf))
    {
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        return;
    }

    // Header
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.63f, 0.13f, 1.f));
    ImGui::Text("Bookmarks");
    ImGui::PopStyleColor();

    ImGui::SameLine(kPanelW - kPad * 2.f - 14.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.4f, 0.2f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.15f, 0.05f, 0.6f));
    if (ImGui::SmallButton("x##closeBkFloat"))
        bookmarks_visible = false;
    ImGui::PopStyleColor(3);

    ImGui::Separator();

    // Find closest bookmark
    int closestIdx = -1;
    {
        uint32_t curMs = (uint32_t)(currentTimeSec * 1000.f);
        uint32_t bestDist = UINT32_MAX;
        for (int i = 0; i < bkCount; ++i)
        {
            uint32_t d = (bookmarks[i].timestamp_ms > curMs)
                       ? (bookmarks[i].timestamp_ms - curMs)
                       : (curMs - bookmarks[i].timestamp_ms);
            if (d < bestDist) { bestDist = d; closestIdx = i; }
        }
    }

    if (bookmarks.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.43f, 0.3f, 0.7f));
        ImGui::TextWrapped("No bookmarks yet. Press B or right-click the timeline.");
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::BeginChild("##BkFloatList", ImVec2(0, listH), false,
                          ImGuiWindowFlags_NoBackground);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.10f, 0.06f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.18f, 0.08f, 0.8f));

        int removeIdx = -1;
        for (int i = 0; i < bkCount; ++i)
        {
            auto& bk = bookmarks[i];
            char timeBuf[16];
            FormatTimeMMSS(bk.timestamp_ms, timeBuf, sizeof(timeBuf));

            bool isClosest = (i == closestIdx);

            ImGui::PushID(i);

            if (isClosest)
            {
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImDrawList* dlC = ImGui::GetWindowDrawList();
                float rw = kPanelW - kPad * 2.f;
                dlC->AddRectFilled(ImVec2(p.x - 2.f, p.y), ImVec2(p.x + rw, p.y + kRowH),
                                   IM_COL32(0x1a, 0x14, 0x00, 0x60), 3.f);
                dlC->AddRectFilled(ImVec2(p.x - 2.f, p.y), ImVec2(p.x + 1.f, p.y + kRowH),
                                   IM_COL32(0xc8, 0xa0, 0x20, 0xFF));
            }

            // Timestamp (clickable to jump)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.63f, 0.13f, 1.f));
            char jumpLabel[24];
            snprintf(jumpLabel, sizeof(jumpLabel), "[%s]##j", timeBuf);
            if (ImGui::SmallButton(jumpLabel))
            {
                timelineOut = bk.timestamp_ms / 1000.f;
                isPlayingRef = true;
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            // Title (truncated to fit)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.68f, 0.58f, 1.f));
            float availForTitle = ImGui::GetContentRegionAvail().x - 65.f;
            const char* title = bk.title.c_str();
            ImVec2 tsz = ImGui::CalcTextSize(title);
            if (tsz.x > availForTitle && availForTitle > 20.f)
            {
                char trunc[44];
                snprintf(trunc, sizeof(trunc), "%.28s...", title);
                ImGui::TextUnformatted(trunc);
            }
            else
            {
                ImGui::TextUnformatted(title);
            }
            ImGui::PopStyleColor();

            // Action buttons (right-aligned)
            float btnsX = kPanelW - kPad * 2.f - 42.f;
            ImGui::SameLine(btnsX);

            // Rename button (pencil-on-page icon drawn over a SmallButton)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
                bool renClk = ImGui::SmallButton(" ##ren");
                bool renHov = ImGui::IsItemHovered();
                ImGui::PopStyleColor();

                ImVec2 rMin = ImGui::GetItemRectMin();
                ImVec2 rMax = ImGui::GetItemRectMax();
                float cx = (rMin.x + rMax.x) * 0.5f;
                float cy = (rMin.y + rMax.y) * 0.5f;

                ImU32 col = renHov ? IM_COL32(0xe8, 0xc8, 0x60, 0xFF)
                                   : IM_COL32(0x99, 0x80, 0x4d, 0xFF);
                ImDrawList* dlP = ImGui::GetWindowDrawList();
                float s = 4.5f;

                // Page outline (open top-right corner for the pencil)
                dlP->AddLine(ImVec2(cx - s, cy - s),     ImVec2(cx - s, cy + s),   col, 1.2f);
                dlP->AddLine(ImVec2(cx - s, cy + s),     ImVec2(cx + s, cy + s),   col, 1.2f);
                dlP->AddLine(ImVec2(cx + s, cy + s),     ImVec2(cx + s, cy - 1.f), col, 1.2f);
                dlP->AddLine(ImVec2(cx - s, cy - s),     ImVec2(cx + 1.f, cy - s), col, 1.2f);

                // Pencil (diagonal)
                float px1 = cx - 2.f, py1 = cy + 2.f;
                float px2 = cx + 4.f, py2 = cy - 4.f;
                dlP->AddLine(ImVec2(px1, py1), ImVec2(px2, py2), col, 1.4f);
                // Pencil tip
                dlP->AddLine(ImVec2(px1, py1), ImVec2(px1 - 1.2f, py1 + 1.2f), col, 1.2f);
                // Eraser cap
                dlP->AddLine(ImVec2(px2 - 0.8f, py2 - 0.8f), ImVec2(px2 + 0.8f, py2 + 0.8f), col, 2.f);

                if (renClk)
                {
                    m_renamePopupOpen = true;
                    m_renameBookmarkIdx = i;
                    memset(m_renameTitleBuf, 0, sizeof(m_renameTitleBuf));
                    strncpy(m_renameTitleBuf, bk.title.c_str(), sizeof(m_renameTitleBuf) - 1);
                }
                if (renHov) ImGui::SetTooltip("Rename");
            }

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.3f, 0.2f, 1.f));
            if (ImGui::SmallButton("x"))
                removeIdx = i;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete");
            ImGui::PopStyleColor();

            ImGui::PopID();
        }

        if (removeIdx >= 0)
        {
            bookmarks.erase(bookmarks.begin() + removeIdx);
            if (onBookmarksChanged) onBookmarksChanged();
        }

        ImGui::PopStyleColor(2);
        ImGui::EndChild();
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ════════════════════════════════════════════════════════════════════════
// Timeline markers
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::RenderTimelineMarkers(float trackX, float trackW,
                                              float trackBarY, float trackH,
                                              float maxTimeSec, float& timelineOut,
                                              float displayTimeOffset)
{
    if (bookmarks.empty() || maxTimeSec <= 0.f) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 markerCol = IM_COL32(0xc8, 0xa0, 0x20, 0xFF);

    for (auto& bk : bookmarks)
    {
        float t = bk.timestamp_ms / 1000.f;
        if (t < 0.f || t > maxTimeSec) continue;

        float x = trackX + trackW * (t / maxTimeSec);
        dl->AddLine(ImVec2(x, trackBarY), ImVec2(x, trackBarY + trackH), markerCol, 1.5f);

        // Hit-test for hover tooltip and click-to-seek
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        if (std::abs(mousePos.x - x) < 5.f &&
            mousePos.y >= trackBarY && mousePos.y <= trackBarY + trackH + 16.f)
        {
            char timeBuf[16];
            float displayTime = t - displayTimeOffset;
            int totalSec = (int)displayTime;
            bool neg = totalSec < 0;
            if (neg) totalSec = -totalSec;
            int m = totalSec / 60;
            int s = totalSec % 60;
            if (neg)
                snprintf(timeBuf, sizeof(timeBuf), "-%02d:%02d", m, s);
            else
                snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", m, s);

            ImGui::SetTooltip("%s  %s", timeBuf, bk.title.c_str());

            if (ImGui::IsMouseClicked(0))
                timelineOut = t;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════
// DrawToast
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::DrawToast()
{
    float dt = ImGui::GetIO().DeltaTime;
    m_toastTimer -= dt;
    if (m_toastTimer <= 0.f) return;

    float alpha = (m_toastTimer < kToastFadeTime)
                ? (m_toastTimer / kToastFadeTime)
                : 1.f;

    const char* msg = "Use the checkbox to enable drawing mode";

    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec2 textSize = ImGui::CalcTextSize(msg);
    float padX = 10.f, padY = 6.f;
    float boxW = textSize.x + padX * 2.f;
    float boxH = textSize.y + padY * 2.f;
    float posX = 12.f;
    float posY = display.y - boxH - 40.f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    ImU32 bgCol   = IM_COL32(0x11, 0x11, 0x08, (int)(0xF0 * alpha));
    ImU32 bordCol = IM_COL32(0x3a, 0x30, 0x20, (int)(0xFF * alpha));
    ImU32 txtCol  = IM_COL32(0xc8, 0xa0, 0x20, (int)(0xFF * alpha));

    dl->AddRectFilled(ImVec2(posX, posY),
                      ImVec2(posX + boxW, posY + boxH),
                      bgCol, 4.f);
    dl->AddRect(ImVec2(posX, posY),
                ImVec2(posX + boxW, posY + boxH),
                bordCol, 4.f, 0, 1.f);
    dl->AddText(ImVec2(posX + padX, posY + padY), txtCol, msg);
}

// ════════════════════════════════════════════════════════════════════════
// DrawBookmarkToast
// ════════════════════════════════════════════════════════════════════════

void AnnotationManager::DrawBookmarkToast()
{
    float dt = ImGui::GetIO().DeltaTime;
    m_bookmarkToastTimer -= dt;
    if (m_bookmarkToastTimer <= 0.f) return;

    float alpha = (m_bookmarkToastTimer < kToastFadeTime)
                ? (m_bookmarkToastTimer / kToastFadeTime)
                : 1.f;

    const char* msg = "Bookmark added \xE2\x80\x94 panel opened on the right";

    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec2 textSize = ImGui::CalcTextSize(msg);
    float padX = 10.f, padY = 6.f;
    float boxW = textSize.x + padX * 2.f;
    float boxH = textSize.y + padY * 2.f;
    float posX = 12.f;
    float posY = display.y - boxH - 40.f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    ImU32 bgCol   = IM_COL32(0x11, 0x11, 0x08, (int)(0xF0 * alpha));
    ImU32 bordCol = IM_COL32(0x3a, 0x30, 0x20, (int)(0xFF * alpha));
    ImU32 txtCol  = IM_COL32(0xc8, 0xa0, 0x20, (int)(0xFF * alpha));

    dl->AddRectFilled(ImVec2(posX, posY),
                      ImVec2(posX + boxW, posY + boxH),
                      bgCol, 4.f);
    dl->AddRect(ImVec2(posX, posY),
                ImVec2(posX + boxW, posY + boxH),
                bordCol, 4.f, 0, 1.f);
    dl->AddText(ImVec2(posX + padX, posY + padY), txtCol, msg);
}
