#pragma once

// State of the replay's Clip export (ReplayWindow_Clip.cpp): a GIF of a part of the screen over a
// time range, made for sharing on Discord. The editor (capture box, time range on the play bar,
// options panel), the frame-by-frame render on a fixed clock, the encoder thread and the toast.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui.h>

struct ClipExportState
{
    enum class Phase { Closed, Editing, Rendering, Encoding };
    Phase phase = Phase::Closed;

    // ---- remembered between clips (settings/clip_export.json) ----
    float box[4] = {0.25f, 0.25f, 0.5f, 0.5f};   // x, y, w, h as fractions of the view
    int   fps = 20;
    int   sizeLimitMb = 10;          // 0 = no limit. 10 MB is Discord's upload limit without Nitro.
    int   outputScalePct = 100;      // 100 / 75 / 50
    bool  followPlayer = false;
    bool  tagEnabled = true;
    int   tagCorner = 0;             // 0 bottom-left, 1 bottom-right, 2 top-left, 3 top-right
    bool  copyToClipboard = true;
    std::wstring lastFolder;
    double bytesPerPixelFrame = 0.40;   // GIF bytes per output pixel per frame: a busy fight; raised by exports, never below kMinBytesPerPixelFrame
    bool  settingsLoaded = false;

    // ---- editor ----
    float  startTime = 0.f, endTime = 8.f;   // replay seconds
    int    followAgentId = -1;
    bool   previewing = false;
    int    boxDrag = 0;                      // which edges a box drag moves (ReplayWindow_Clip.cpp)
    ImVec2 boxDragMouse{0.f, 0.f};
    float  boxDragStart[4] = {0.f, 0.f, 0.f, 0.f};
    int    cursorWanted = -1;                // CursorMode the box handles ask for; -1 = none
    int    timelineDrag = 0;                 // mini bar: 1 start handle, 2 end handle, 3 whole range, 4 seek
    float  timelineGrab = 0.f;               // whole-range drag: where in the range it was grabbed
    float  miniView0 = 0.f, miniView1 = 20.f; // the mini bar's time window, held still during a drag
    ImVec2 followPos{0.f, 0.f}, followVel{0.f, 0.f};
    bool   followPosValid = false;
    bool   escDown = false, altXDown = false, clickDown = false;
    bool   escConsumed = false;              // this frame's Escape closed or cancelled the clip
    ImVec2 cancelMin{0.f, 0.f}, cancelMax{0.f, 0.f};   // the render's Cancel button, hit-tested by hand

    // ---- save dialog: on a thread of its own (the UI thread is a multithreaded COM apartment, and
    //      the shell's file dialog hangs there) ----
    std::thread       dialogThread;
    std::atomic<bool> dialogDone{false};
    bool              dialogOpen = false;
    bool              dialogOk = false;
    std::wstring      dialogPath;

    // ---- render ----
    std::wstring outPath;
    int   boxW = 0, boxH = 0;                // capture size in back-buffer pixels, fixed for the export
    int   outW = 0, outH = 0;                // after the output scale
    int   frameIndex = 0;                    // negative during the pre-roll
    int   prerollFrames = 0;
    int   frameCount = 0;
    bool  captureThisFrame = false;
    std::vector<std::vector<uint8_t>> frames;
    float savedTimeline = 0.f;
    bool  savedPlaying = false;
    bool  savedLoop = false;
    float savedSpeed = 1.f;
    int   savedSpeedIndex = 3;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> resolveTex, stagingTex;

    // ---- encoder thread ----
    std::thread        worker;
    std::atomic<float> progress{0.f};
    std::atomic<bool>  cancel{false};
    std::atomic<bool>  workerDone{false};
    bool               workerOk = false;
    std::vector<uint8_t> gif;

    // ---- result toast ----
    std::string  toast;
    double       toastUntil = 0.0;          // ImGui time
    bool         toastIsError = false;
    bool         toastOverLimit = false;
    std::wstring savedPath;

    ClipExportState() = default;
    ClipExportState(const ClipExportState&) = delete;
    ClipExportState& operator=(const ClipExportState&) = delete;
    ~ClipExportState()
    {
        cancel = true;
        if (worker.joinable()) worker.join();
        if (dialogThread.joinable()) dialogThread.join();
    }
};
