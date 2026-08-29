//
// MapBrowser.h
//

#pragma once

#include "DeviceResources.h"
#include "StepTimer.h"
#include "InputManager.h"
#include "Camera.h"
#include "DATManager.h"
#include "MapRenderer.h"
#include "ModelViewer/ModelViewer.h"
#include "AgentOverlay.h"
#include "MatchReplay.h"
#include "ReplayLibrary.h"
#include "ReplayWindow.h"
#include "FolderWatcher.h"
#include "Net/HttpClient.h"
#include "Net/MatchIndex.h"
#include "Net/CloudReplayProvider.h"
#include "Net/SyncEngine.h"
#include "Net/UpdateChecker.h"
#include <draw_extract_panel.h>

using namespace std::chrono;

// A basic MapBrowser implementation that creates a D3D11 device and
// provides a MapBrowser loop.
class MapBrowser final : public DX::IDeviceNotify
{
public:
    MapBrowser(InputManager* input_manager) noexcept(false);
    ~MapBrowser();

    MapBrowser(MapBrowser&&) = default;
    MapBrowser& operator=(MapBrowser&&) = default;

    MapBrowser(MapBrowser const&) = delete;
    MapBrowser& operator=(MapBrowser const&) = delete;

    // Initialization and management
    void Initialize(HWND window, int width, int height);

    // Basic MapBrowser loop
    void Tick();

    // Applies persisted replay camera FOV to every open ReplayWindow (Settings / Preferences).
    static void NotifyReplayWindowsReplayCameraFovChanged();

    // IDeviceNotify
    void OnDeviceLost() override;
    void OnDeviceRestored() override;


    // Messages
    void OnActivated();
    void OnDeactivated();
    void OnSuspending();
    void OnResuming();
    void OnWindowMoved();
    void OnDisplayChange();
    void OnWindowSizeChanged(int width, int height);

    // Properties
    void GetDefaultSize(int& width, int& height) const noexcept;

private:
    int m_cachedPickingObjectId = -1;

    void Update(duration<double, std::milli> elapsed);
    void Render();

    void RenderWaterReflection();

    void RenderShadows();

    void Clear();
    void ClearOffscreen();
    void ClearShadow();
    void ClearReflection();

    void ShowErrorMessage();

    void CreateDeviceDependentResources();
    void CreateWindowSizeDependentResources();

    void DrawStopExtractionButton();
    void ProcessTextureExtraction(int index, const std::wstring& save_directory);

    // Texture Error Logging Helpers
    void OpenTextureErrorLog(const std::wstring& save_directory);
    void CloseTextureErrorLog();
    void WriteToTextureErrorLog(int mft_index, int file_hash, const std::wstring& error_message);


    // Device resources.
    std::unique_ptr<DX::DeviceResources> m_deviceResources;

    // Rendering loop timer.
    DX::StepTimer m_timer;

    int m_FPS_target = 60;
    std::chrono::time_point<std::chrono::high_resolution_clock> last_frame_time;

    // Used for resizing the offscreen buffer when extracting arbitrary sized render of map to png or dds (Extract Panel GUI)
    ExtractPanelInfo m_extract_panel_info;

    // Input manager
    InputManager* m_input_manager;

    std::map<int, std::unique_ptr<DATManager>> m_dat_managers;
    int m_dat_manager_to_show_in_dat_browser;

    std::set<uint32_t> m_mft_indices_to_extract;
    std::unordered_map<int, std::vector<int>> m_hash_index;
    bool m_hash_index_initialized = false;

    //Texture extraction state
    std::set<int> m_mft_indices_to_extract_textures;
    int m_total_textures_to_extract = 0;
    std::wofstream m_texture_error_log_file; // Log file stream
    bool m_is_texture_error_log_open; // Flag for log file state


    std::vector<std::vector<std::string>> m_csv_data;

    std::unique_ptr<MapRenderer> m_map_renderer;
    std::unique_ptr<AgentOverlay> m_agent_overlay;
    MatchReplay m_match_replay;
    ReplayLibrary m_replay_library;
    FolderWatcher m_folderWatcher;

    // Cloud storage system
    HttpClient m_httpClient;
    std::shared_ptr<MatchIndex> m_matchIndex;
    std::unique_ptr<CloudReplayProvider> m_cloudProvider;
    std::unique_ptr<SyncEngine> m_syncEngine;
    std::string m_cloudBucket;

    // Update checker
    UpdateChecker m_updateChecker;

    // Set at startup when the previous run's update failed to install. Shown
    // once, then cleared.
    std::string m_pendingInstallError;

    std::string m_error_msg = "";
    bool m_show_error_msg = false;

    // Cloud match download-on-play state
    enum class PlayDownloadState { Idle, Downloading, Complete, Error };
    struct PlayDownloadCtx
    {
        std::atomic<PlayDownloadState> state{PlayDownloadState::Idle};
        std::atomic<uint64_t> bytesReceived{0};
        std::atomic<uint64_t> bytesTotal{0};
        std::thread thread;
        CloudReplayProvider::DownloadResult result;
        MatchMeta originalMatch;
        std::string errorMsg;
    };
    PlayDownloadCtx m_playDl;
    void ProcessCloudDownloadResult();

    std::vector<std::unique_ptr<ReplayWindow>> m_replay_windows;
    void ProcessPendingReplayRequest();
    void TickReplayWindows();

    static MapBrowser* s_activeInstance;
};