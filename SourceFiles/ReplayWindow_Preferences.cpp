#include "pch.h"
#include "ReplayWindow.h"
#include "AssetBlacklist.h"
#include "MatchRatings.h"
#include "MatchNotes.h"
#include "MatchBookmarks.h"
#include "AgentSnapshotParser.h"
#include "StoCParser.h"
#include "SkillDatabase.h"
#include "DXMathHelpers.h"
#include "FontConfig.h"
#include "GuiGlobalConstants.h"
#include "MapBrowser.h"
#include "TextureCache.h"
#include "CursorSystem.h"
#include "SpatialAudioEngine.h"
#include "SoundCache.h"
#include "Parsers/BB9AnimationParser.h"
#include "Parsers/FileReferenceParser.h"
#include "ReplayWindow_Internal.h"
#include "../ThirdParty/nanosvg/nanosvg.h"
#include "../ThirdParty/nanosvg/nanosvgrast.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <algorithm>
#include <numeric>
#include <json.hpp>
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// Extracted from ReplayWindow.cpp (partial-class split). These remain
// ReplayWindow:: member functions; only their definitions live here.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Debug window: Map Calibration
// ---------------------------------------------------------------------------

void ReplayWindow::DrawMapCalibrationWindow()
{
    ImGui::SetNextWindowSize(ImVec2(400, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Map Calibration", &m_showMapCalibrationWindow))
    {
        ImGui::End();
        return;
    }

    MapTransform& t = m_replayCtx.mapTransform;
    ImGui::Text("Map ID: %d", m_replayCtx.mapId);
    ImGui::TextDisabled("Base remap: x=snap.x  y=snap.z(height)  z=snap.y");
    ImGui::Separator();

    ImGui::Text("1. Axis Swaps");
    ImGui::Checkbox("Swap Y / Z", &t.swapYZ);
    ImGui::SameLine();
    ImGui::Checkbox("Swap X / Z", &t.swapXZ);
    ImGui::SameLine();
    ImGui::Checkbox("Swap X / Y", &t.swapXY);
    ImGui::Separator();

    ImGui::Text("2. Axis Flips");
    ImGui::Checkbox("Flip X", &t.flipX);
    ImGui::SameLine();
    ImGui::Checkbox("Flip Y", &t.flipY);
    ImGui::SameLine();
    ImGui::Checkbox("Flip Z", &t.flipZ);
    ImGui::Separator();

    ImGui::Text("3. Rotation (Y axis)");
    ImGui::SliderFloat("Rotation", &t.rotationDegrees, 0.f, 360.f, "%.1f deg");
    ImGui::Separator();

    ImGui::Text("4. Offset");
    ImGui::DragFloat("Offset X", &t.offsetX, 10.f, -100000.f, 100000.f, "%.1f");
    ImGui::DragFloat("Offset Y", &t.offsetY, 10.f, -100000.f, 100000.f, "%.1f");
    ImGui::DragFloat("Offset Z", &t.offsetZ, 10.f, -100000.f, 100000.f, "%.1f");
    ImGui::Separator();

    ImGui::Text("5. Scale");
    ImGui::DragFloat("Scale X", &t.scaleX, 0.005f, 0.01f, 10.f, "%.4f");
    ImGui::DragFloat("Scale Y", &t.scaleY, 0.005f, 0.01f, 10.f, "%.4f");
    ImGui::DragFloat("Scale Z", &t.scaleZ, 0.005f, 0.01f, 10.f, "%.4f");
    ImGui::Separator();

    ImGui::Text("Visualization");
    ImGui::Checkbox("Show raw positions", &m_showRawPositions);
    ImGui::SameLine();
    ImGui::Checkbox("Show origin axes", &m_showMapOriginAxes);
    ImGui::Separator();

    if (ImGui::Button("Reset (identity)"))
        t = {};

    ImGui::SameLine();
    if (ImGui::Button("Reset (default)"))
        t = GetDefaultMapTransform();

    ImGui::SameLine();
    if (ImGui::Button("Save"))
        SaveMapTransform(m_replayCtx.mapId, t);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Debug window: Interpolation Settings
// ---------------------------------------------------------------------------

void ReplayWindow::DrawInterpolationWindow()
{
    ImGui::SetNextWindowSize(ImVec2(340, 280), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Interpolation", &m_showInterpolationWindow))
    {
        ImGui::End();
        return;
    }

    InterpolationSettings& s = m_replayCtx.interpSettings;

    ImGui::Checkbox("Enable Interpolation", &s.enabled);
    if (!s.enabled) ImGui::TextDisabled("(all agents snap to nearest snapshot)");
    ImGui::Separator();

    ImGui::Text("Mode");
    int mode = static_cast<int>(s.mode);
    ImGui::RadioButton("Original (Linear)", &mode, 0);
    ImGui::RadioButton("Improved (MOVE_TO_POINT Aware)", &mode, 1);
    s.mode = static_cast<InterpolationMode>(mode);
    ImGui::Separator();

    bool improved = (s.mode == InterpolationMode::Improved);
    if (!improved) ImGui::BeginDisabled();

    ImGui::Text("Improved Mode Settings");
    ImGui::SliderFloat("Gap Threshold", &s.gapThreshold, 0.1f, 2.0f, "%.2f s");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Minimum time gap before MOVE_TO_POINT prediction is used");
    ImGui::SliderFloat("Velocity Influence", &s.velocityInfluence, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Blending weight between linear and MOVE_TO_POINT prediction");

    if (!improved) ImGui::EndDisabled();
    ImGui::Separator();

    ImGui::Text("Movement Freeze Rules");
    ImGui::TextDisabled("Agents freeze when casting or dead.");
    ImGui::TextDisabled("Applies to both interpolation modes.");
    ImGui::Checkbox("Show Casting Freeze", &s.showCastingFreeze);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Purple ring around agents that are currently casting");
    ImGui::Checkbox("Show Dead Freeze", &s.showDeadFreeze);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Black X over agents that are dead");
    ImGui::Separator();

    ImGui::Text("Visualization");
    ImGui::Checkbox("Show raw snapshot positions", &s.showRawSnapshots);
    ImGui::Checkbox("Show interp debug lines", &s.showInterpolatedLine);
    ImGui::Checkbox("Show MOVE_TO_POINT anchors", &s.showMoveAnchors);
    ImGui::Separator();

    const char* modeLabel = (s.mode == InterpolationMode::OriginalLinear)
        ? "Original (Linear)" : "Improved (MOVE_TO_POINT)";
    ImGui::TextDisabled("Active: %s  |  %s",
                        modeLabel, s.enabled ? "ON" : "OFF");

    ImGui::End();
}

// ---------------------------------------------------------------------------
// GetAsyncKeyState-based edge detection (bypass ImGui focus issues)
// ---------------------------------------------------------------------------

bool ReplayWindow::HotkeyPressed(int imguiKey)
{
    UINT vk = ReplayHotkeys::ImGuiKeyToVK(imguiKey);
    if (!vk) return false;
    int idx = imguiKey - ImGuiKey_NamedKey_BEGIN;
    if (idx < 0 || idx >= ImGuiKey_NamedKey_COUNT) return false;
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool wasDown = m_prevKeyDown[idx];
    m_prevKeyDown[idx] = down;
    return down && !wasDown;
}

// ---------------------------------------------------------------------------
// Shortcut / Interface Preferences modals — match Licence, File Paths, and
// setup wizard card styling (draw_setup_wizard.cpp / draw_licence_modal).
// ---------------------------------------------------------------------------

namespace
{
    static void PushPrefsModalChrome()
    {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.078f, 0.102f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.08f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    }

    static void PopPrefsModalChrome()
    {
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }

    static void DrawPrefsModalTitle(const char* dimSuffix)
    {
        ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "GW Observer");
        ImGui::SameLine(0.f, 0.f);
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.5f), "%s", dimSuffix);
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8.f));
    }

    static void DrawPrefsSectionHeader(const char* title)
    {
        ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "%s", title);
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 6.f));
    }

    static void PushPrefsFrameStyle()
    {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.4f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.15f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    }

    static void PopPrefsFrameStyle()
    {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    static void PushPrefsPrimaryButtonStyle()
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.15f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.18f, 0.22f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.25f, 0.30f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.63f, 0.13f, 0.8f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
    }

    static void PopPrefsPrimaryButtonStyle()
    {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
    }

    static void PushPrefsGhostButtonStyle()
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.83f, 0.63f, 0.13f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
    }

    static void PopPrefsGhostButtonStyle()
    {
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(4);
    }

    static void PushPrefsSecondaryButtonStyle()
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 0.85f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
    }

    static void PopPrefsSecondaryButtonStyle()
    {
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(4);
    }
} // namespace

bool HotkeyInput(const char* label, int* key, bool modernChrome)
{
    // Track which widget is currently capturing a key press.
    // Only one can be active at a time across all HotkeyInput calls.
    static ImGuiID s_capturingID = 0;

    constexpr float kLabelCol = 200.f;
    if (modernChrome)
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.85f), "%s", label);
    else
        ImGui::Text("%s", label);
    ImGui::SameLine(kLabelCol);

    ImGui::PushID(label);
    ImGuiID thisID = ImGui::GetID("##hotkey");

    bool capturing = (s_capturingID == thisID);

    char buf[64];
    if (capturing)
        snprintf(buf, sizeof(buf), "Press a key...");
    else if (*key != 0)
        snprintf(buf, sizeof(buf), "%s", ImGui::GetKeyName((ImGuiKey)*key));
    else
        snprintf(buf, sizeof(buf), "(none)");

    // Snapshot for balanced Push/Pop — 'capturing' may change inside the click handler
    bool wasCapturing = capturing;

    if (modernChrome)
    {
        if (wasCapturing)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.18f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.23f, 0.13f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.32f, 0.27f, 0.15f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.91f, 0.69f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 0.84f, 0.39f, 0.95f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        }
        else
        {
            PushPrefsPrimaryButtonStyle();
        }
    }
    else if (wasCapturing)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }

    if (ImGui::Button(buf, ImVec2(150, 0)))
    {
        // Toggle capture on click
        s_capturingID = wasCapturing ? 0 : thisID;
        capturing = (s_capturingID == thisID);
    }

    if (modernChrome)
    {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
    }
    else if (wasCapturing)
    {
        ImGui::PopStyleColor();
    }

    bool changed = false;
    if (capturing)
    {
        // Cancel on Escape
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            s_capturingID = 0;
        }
        else
        {
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
            {
                // Only accept valid bindable keys (keyboard + RMB/MMB/X1/X2, not LMB or wheel)
                if (!ReplayHotkeys::IsValidBindableKey(k))
                    continue;
                if (ImGui::IsKeyPressed((ImGuiKey)k))
                {
                    *key = k;
                    s_capturingID = 0;
                    changed = true;
                    break;
                }
            }
        }
    }

    ImGui::PopID();
    return changed;
}

void ReplayWindow::DrawShortcutPreferences()
{
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float cardW = std::max(380.f, std::min(520.f, display.x - 80.f));
    float cardH = std::max(420.f, std::min(760.f, display.y - 80.f));
    ImGui::SetNextWindowPos(ImVec2((display.x - cardW) * 0.5f, (display.y - cardH) * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH), ImGuiCond_Always);

    PushPrefsModalChrome();
    if (!ImGui::BeginPopupModal("Shortcut Preferences", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse))
    {
        PopPrefsModalChrome();
        return;
    }

    static ReplayHotkeys editing;
    static bool needsInit = true;
    if (needsInit) { editing = ReplayHotkeys::Get(); needsInit = false; }

    DrawPrefsModalTitle(" -  Shortcut Preferences");

    const float kFooterH = 56.f;
    float scrollH = ImGui::GetContentRegionAvail().y - kFooterH;
    if (scrollH < 120.f)
        scrollH = 120.f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.25f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);
    if (ImGui::BeginChild("##ShortcutPrefsScroll", ImVec2(-1, scrollH), true))
    {
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.06f));

        DrawPrefsSectionHeader("REPLAY TRANSPORT");
        HotkeyInput("Rewind 5 seconds",  &editing.rewind5s, true);
        HotkeyInput("Forward 5 seconds", &editing.forward5s, true);
        HotkeyInput("Play / Pause",      &editing.playPause, true);

        ImGui::Dummy(ImVec2(0, 8.f));
        DrawPrefsSectionHeader("OVERLAY TOGGLES");
        HotkeyInput("Range Rings",         &editing.toggleRangeRings, true);
        HotkeyInput("Morale Panel",        &editing.toggleMoralePanel, true);
        HotkeyInput("Event Timeline",      &editing.toggleEventTimeline, true);
        HotkeyInput("Lord Damage Panel",   &editing.toggleLordDamage, true);
        HotkeyInput("Heatmap",             &editing.toggleHeatmap, true);
        HotkeyInput("Piano Roll",          &editing.togglePianoRoll, true);
        HotkeyInput("Minimap",             &editing.toggleMinimap, true);
        HotkeyInput("Add Bookmark",        &editing.addBookmark, true);

        ImGui::Dummy(ImVec2(0, 8.f));
        DrawPrefsSectionHeader("CAMERA & VIEW");
        HotkeyInput("Auto Camera",         &editing.toggleAutoCamera, true);
        HotkeyInput("Fog of War",          &editing.toggleFogOfWar, true);
        HotkeyInput("Top View",            &editing.toggleTopView, true);
        HotkeyInput("Exit Follow Mode",    &editing.exitFollowMode, true);

        ImGui::Dummy(ImVec2(0, 8.f));
        DrawPrefsSectionHeader("CAMERA MOVEMENT");
        HotkeyInput("Move Forward",        &editing.camForward, true);
        HotkeyInput("Move Backward",       &editing.camBackward, true);
        HotkeyInput("Strafe Left",         &editing.camStrafeLeft, true);
        HotkeyInput("Strafe Right",        &editing.camStrafeRight, true);

        ImGui::Dummy(ImVec2(0, 8.f));
        DrawPrefsSectionHeader("CAMERA OPTIONS");
        PushPrefsFrameStyle();
        ImGui::Checkbox("Invert Mouse X (horizontal)", &editing.invertMouseX);
        ImGui::Checkbox("Invert Mouse Y (vertical)",   &editing.invertMouseY);
        PopPrefsFrameStyle();

        ImGui::PopStyleColor(); // Child border
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(1);

    ImGui::Dummy(ImVec2(0, 8.f));

    float btnGap = 8.f;
    float saveW = 120.f;
    float resetW = 150.f;
    float cancelW = 100.f;
    float totalW = saveW + btnGap + resetW + btnGap + cancelW;
    ImGui::SetCursorPosX(std::max(0.f, (ImGui::GetWindowSize().x - totalW) * 0.5f));

    PushPrefsPrimaryButtonStyle();
    if (ImGui::Button("Save", ImVec2(saveW, 34.f)))
    {
        ReplayHotkeys::Get() = editing;
        ReplayHotkeys::Get().Save();
        needsInit = true;
        ImGui::CloseCurrentPopup();
    }
    PopPrefsPrimaryButtonStyle();

    ImGui::SameLine(0.f, btnGap);
    PushPrefsPrimaryButtonStyle();
    if (ImGui::Button("Reset to Defaults", ImVec2(resetW, 34.f)))
        editing.ResetToDefaults();
    PopPrefsPrimaryButtonStyle();

    ImGui::SameLine(0.f, btnGap);
    PushPrefsSecondaryButtonStyle();
    if (ImGui::Button("Cancel", ImVec2(cancelW, 34.f)))
    {
        needsInit = true;
        ImGui::CloseCurrentPopup();
    }
    PopPrefsSecondaryButtonStyle();

    ImGui::EndPopup();
    PopPrefsModalChrome();
}

// ---------------------------------------------------------------------------
// Interface Preferences modal — reposition scene overlays
// ---------------------------------------------------------------------------

void ReplayWindow::DrawInterfacePreferences()
{
    if (!m_showInterfacePrefs) return;

    ImVec2 display = ImGui::GetIO().DisplaySize;
    float cardW = std::max(420.f, std::min(580.f, display.x - 80.f));
    float cardH = std::max(480.f, std::min(720.f, display.y - 80.f));
    ImGui::SetNextWindowPos(ImVec2((display.x - cardW) * 0.5f, (display.y - cardH) * 0.5f), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH), ImGuiCond_Once);

    PushPrefsModalChrome();
    if (!ImGui::Begin("Interface Preferences", &m_showInterfacePrefs,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar))
    {
        ImGui::End();
        PopPrefsModalChrome();
        return;
    }

    DrawPrefsModalTitle(" -  Interface Preferences");

    bool changed = false;

    const float kFooterH = 52.f;
    float scrollH = ImGui::GetContentRegionAvail().y - kFooterH;
    if (scrollH < 160.f)
        scrollH = 160.f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.25f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);
    if (ImGui::BeginChild("##InterfacePrefsScroll", ImVec2(-1, scrollH), true))
    {
        DrawPrefsSectionHeader("REPLAY CAMERA");
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.55f), "(applies to all open replay windows)");
        ImGui::Dummy(ImVec2(0, 6.f));

        PushPrefsFrameStyle();
        {
            float fovDeg = GuiGlobalConstants::replay_camera_fov_degrees;
            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.85f), "Camera Field of View");
            float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(std::max(120.f, avail - 56.f));
            if (ImGui::SliderFloat("##ifaceReplayFov", &fovDeg,
                    GuiGlobalConstants::kMinReplayCameraFovDegrees,
                    GuiGlobalConstants::kMaxReplayCameraFovDegrees, ""))
            {
                GuiGlobalConstants::replay_camera_fov_degrees =
                    GuiGlobalConstants::ClampReplayCameraFovDegrees(fovDeg);
                GuiGlobalConstants::SaveSettings();
                MapBrowser::NotifyReplayWindowsReplayCameraFovChanged();
                changed = true;
            }
            ImGui::SameLine(0.f, 10.f);
            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.75f), "%.0f°",
                GuiGlobalConstants::replay_camera_fov_degrees);
        }
        PopPrefsFrameStyle();

        ImGui::Dummy(ImVec2(0, 10.f));
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.55f),
            "Pan, rotation, zoom, and keyboard move speeds (applies immediately).");
        ImGui::Dummy(ImVec2(0, 6.f));

        PushPrefsFrameStyle();
        {
            auto SensitivityRow = [&](const char* id, const char* label, float* pStored)
            {
                float v = *pStored;
                ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.85f), "%s", label);
                float availM = ImGui::GetContentRegionAvail().x;
                ImGui::SetNextItemWidth(std::max(120.f, availM - 68.f));
                std::string sid = "##iface";
                sid += id;
                if (ImGui::SliderFloat(sid.c_str(), &v,
                        GuiGlobalConstants::kMinReplayCameraSensitivityMultiplier,
                        GuiGlobalConstants::kMaxReplayCameraSensitivityMultiplier, ""))
                {
                    *pStored = GuiGlobalConstants::ClampReplayCameraSensitivityMultiplier(v);
                    GuiGlobalConstants::SaveSettings();
                    changed = true;
                }
                ImGui::SameLine(0.f, 10.f);
                ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.75f), "%.2fx", *pStored);
            };

            SensitivityRow("ReplayPan", "Pan Speed (left drag)",
                &GuiGlobalConstants::replay_camera_pan_speed_multiplier);
            ImGui::Dummy(ImVec2(0, 8.f));
            SensitivityRow("ReplayRot", "Rotation Speed (right drag)",
                &GuiGlobalConstants::replay_camera_rotation_speed_multiplier);
            ImGui::Dummy(ImVec2(0, 8.f));
            SensitivityRow("ReplayZoom", "Zoom Speed (mouse wheel)",
                &GuiGlobalConstants::replay_camera_zoom_speed_multiplier);
            ImGui::Dummy(ImVec2(0, 8.f));
            SensitivityRow("ReplayKbd", "Keyboard Move Speed",
                &GuiGlobalConstants::replay_camera_keyboard_speed_multiplier);
        }
        PopPrefsFrameStyle();

        ImGui::Dummy(ImVec2(0, 16.f));
        DrawPrefsSectionHeader("SCENE OVERLAY POSITIONS");
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.55f), "(stored as viewport %%)");
        ImGui::Dummy(ImVec2(0, 6.f));

        PushPrefsFrameStyle();
        if (ImGui::Checkbox("Enable custom positions", &m_uiLayout.useCustom))
            changed = true;
        PopPrefsFrameStyle();

        ImGui::Dummy(ImVec2(0, 8.f));

        if (!m_uiLayout.useCustom)
            ImGui::BeginDisabled();

        constexpr float kLabelCol = 200.f;
        auto PositionRow = [&](const char* label, float* px, float* py, int dragIdx)
        {
            ImGui::PushID(label);
            ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.85f), "%s", label);
            ImGui::SameLine(kLabelCol);
            float vals[2] = { *px * 100.f, *py * 100.f };
            PushPrefsFrameStyle();
            ImGui::SetNextItemWidth(160);
            if (ImGui::DragFloat2("##pos", vals, 0.1f, 0.f, 100.f, "%.1f%%"))
            {
                *px = vals[0] / 100.f;
                *py = vals[1] / 100.f;
                changed = true;
            }
            PopPrefsFrameStyle();
            ImGui::SameLine(0.f, 10.f);
            PushPrefsGhostButtonStyle();
            if (ImGui::SmallButton("Move"))
            {
                m_draggingUIElement = dragIdx;
                m_showInterfacePrefs = false;
            }
            PopPrefsGhostButtonStyle();
            ImGui::PopID();
        };

        PositionRow("Match Timer",      &m_uiLayout.timerX,  &m_uiLayout.timerY,  3);
        PositionRow("Jumbo Message",    &m_uiLayout.jumboX,  &m_uiLayout.jumboY,  0);
        PositionRow("Morale (Red)",     &m_uiLayout.moRedX,  &m_uiLayout.moRedY,  1);
        PositionRow("Morale (Blue)",    &m_uiLayout.moBlueX, &m_uiLayout.moBlueY, 2);

        if (!m_uiLayout.useCustom)
            ImGui::EndDisabled();

        ImGui::Dummy(ImVec2(0, 16.f));
        DrawPrefsSectionHeader("STYLIZED AGENT ICONS");
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.55f), "(profession icons at distance)");
        ImGui::Dummy(ImVec2(0, 6.f));

        PushPrefsFrameStyle();
        if (ImGui::Checkbox("Enable Stylized Agent Icons", &m_uiLayout.lodEnabled))
            changed = true;

        if (!m_uiLayout.lodEnabled)
            ImGui::BeginDisabled();

        ImGui::SetNextItemWidth(std::min(280.f, ImGui::GetContentRegionAvail().x - 8.f));
        if (ImGui::SliderFloat("Icon distance", &m_uiLayout.lodDotDist, 1000.f, 10000.f, "%.0f"))
            changed = true;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.45f), "(beyond -> stylized icons)");

        if (!m_uiLayout.lodEnabled)
            ImGui::EndDisabled();
        PopPrefsFrameStyle();

        ImGui::Dummy(ImVec2(0, 16.f));
        DrawPrefsSectionHeader("PANEL VISIBILITY");
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.55f), "(toggle replay overlay panels)");
        ImGui::Dummy(ImVec2(0, 6.f));

        PushPrefsFrameStyle();
        for (auto& reg : m_panelLayout.GetRegistrations())
        {
            if (!reg.visiblePtr) continue;
            bool vis = *reg.visiblePtr;
            if (ImGui::Checkbox(reg.displayName.c_str(), &vis))
            {
                *reg.visiblePtr = vis;
                changed = true;
            }
        }
        PopPrefsFrameStyle();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(1);

    ImGui::Dummy(ImVec2(0, 8.f));

    float btnGap = 8.f;
    float resetOverlayW = 170.f;
    float resetPanelsW  = 170.f;
    float closeW = 120.f;
    float totalW = resetOverlayW + btnGap + resetPanelsW + btnGap + closeW;
    ImGui::SetCursorPosX(std::max(0.f, (ImGui::GetWindowSize().x - totalW) * 0.5f));

    PushPrefsPrimaryButtonStyle();
    if (ImGui::Button("Reset Overlay", ImVec2(resetOverlayW, 34.f)))
    {
        m_uiLayout = UILayoutConfig{};
        GuiGlobalConstants::replay_camera_fov_degrees = GuiGlobalConstants::kDefaultReplayCameraFovDegrees;
        GuiGlobalConstants::replay_camera_pan_speed_multiplier =
            GuiGlobalConstants::kDefaultReplayCameraSensitivityMultiplier;
        GuiGlobalConstants::replay_camera_rotation_speed_multiplier =
            GuiGlobalConstants::kDefaultReplayCameraSensitivityMultiplier;
        GuiGlobalConstants::replay_camera_zoom_speed_multiplier =
            GuiGlobalConstants::kDefaultReplayCameraSensitivityMultiplier;
        GuiGlobalConstants::replay_camera_keyboard_speed_multiplier =
            GuiGlobalConstants::kDefaultReplayCameraSensitivityMultiplier;
        GuiGlobalConstants::SaveSettings();
        MapBrowser::NotifyReplayWindowsReplayCameraFovChanged();
        changed = true;
    }
    PopPrefsPrimaryButtonStyle();

    ImGui::SameLine(0.f, btnGap);
    PushPrefsPrimaryButtonStyle();
    if (ImGui::Button("Reset Panel Layout", ImVec2(resetPanelsW, 34.f)))
    {
        m_panelLayout.ResetToDefaults();
        m_partyWindowsPositioned = false;
        changed = true;
    }
    PopPrefsPrimaryButtonStyle();

    ImGui::SameLine(0.f, btnGap);
    PushPrefsSecondaryButtonStyle();
    if (ImGui::Button("Close", ImVec2(closeW, 34.f)))
        m_showInterfacePrefs = false;
    PopPrefsSecondaryButtonStyle();

    if (changed)
        SaveUILayout();

    ImGui::End();
    PopPrefsModalChrome();
}

void ReplayWindow::ApplyReplayCameraFovFromSettings()
{
    if (!m_mapRenderer)
        return;
    m_mapRenderer->SetPerspectiveFovDegrees(
        GuiGlobalConstants::ClampReplayCameraFovDegrees(GuiGlobalConstants::replay_camera_fov_degrees));
}
