#pragma once

#include "DeviceResources.h"
#include "StepTimer.h"
#include "InputManager.h"
#include "MapRenderer.h"
#include "DATManager.h"
#include "Terrain.h"
#include "ReplayMapData.h"
#include "ReplayLibrary.h"
#include "FFNA_MapFile.h"
#include "FFNA_ModelFile.h"
#include "AMAT_file.h"
#include <string>
#include <memory>
#include <variant>

class ReplayWindow final : public DX::IDeviceNotify
{
public:
    static bool RegisterWindowClass(HINSTANCE hInstance);
    static ReplayWindow* Create(HINSTANCE hInstance, const MatchMeta& match, DATManager* sharedDatManager,
                                const std::unordered_map<int, std::vector<int>>& hashIndex);

    ~ReplayWindow();

    ReplayWindow(const ReplayWindow&) = delete;
    ReplayWindow& operator=(const ReplayWindow&) = delete;

    void Tick();
    bool IsAlive() const { return m_alive; }
    HWND GetHWND() const { return m_hwnd; }

    // IDeviceNotify
    void OnDeviceLost() override;
    void OnDeviceRestored() override;

    void OnWindowSizeChanged(int width, int height);
    void OnDestroy();

    // Phase 2+ entry point
    void LoadReplayData(const std::filesystem::path& matchFolderPath);

private:
    ReplayWindow() = default;

    bool InitWindow(HINSTANCE hInstance, const std::wstring& title);
    bool InitGraphics();
    bool InitLoadingOverlay();
    void InitImGui();
    void ShutdownImGui();

    // Phased map loading (one phase per Tick to keep window responsive)
    enum class LoadingPhase { Validate, Init, PropModels, PlaceProps, Ready, Error };

    void StepValidate();
    void StepLoadInit();
    void StepLoadPropModels();
    void StepPlaceProps();

    void Update(double elapsedMs);
    void Render();
    void RenderLoadingScreen();
    void Clear();
    void DrawImGuiOverlay();
    void DrawAgentDataWindow();
    void DrawStoCWindow();
    void DrawAgentOverlay();
    void DrawMapCalibrationWindow();
    void DrawInterpolationWindow();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND m_hwnd = nullptr;
    bool m_alive = false;

    std::unique_ptr<DX::DeviceResources> m_deviceResources;
    std::unique_ptr<InputManager> m_inputManager;
    std::unique_ptr<MapRenderer> m_mapRenderer;
    DX::StepTimer m_timer;

    DATManager* m_datManager = nullptr;
    const std::unordered_map<int, std::vector<int>>* m_hashIndex = nullptr;

    ReplayContext m_replayCtx;
    MatchMeta m_matchMeta;
    std::unique_ptr<Terrain> m_terrain;

    std::string m_errorMsg;

    // --- Loading state machine ---
    LoadingPhase m_loadingPhase = LoadingPhase::Validate;
    float m_loadProgress = 0.0f;

    // Intermediate state for phased loading (persists across Ticks)
    FFNA_MapFile m_mapFile;
    using ModelVariant = std::variant<FFNA_ModelFile>;
    std::vector<ModelVariant> m_propModelFiles;
    int m_propModelLoadIndex = 0;
    int m_propPlaceIndex = 0;
    int m_totalPropFilenames = 0;
    int m_totalPropInstances = 0;

    static constexpr int kPropModelBatchSize = 15;
    static constexpr int kPropPlaceBatchSize = 10;

    // --- ImGui state ---
    bool m_imguiInitialized = false;
    ImGuiContext* m_imguiContext = nullptr;
    bool m_showAgentDataWindow = false;
    int  m_selectedAgentId = -1;
    float m_debugTimeline = 0.f;
    bool m_showParsedView = true;
    float m_agentListWidth = 220.f;
    std::vector<int> m_sortedAgentIds;
    std::vector<int> m_playerIds;
    std::vector<int> m_npcIds;
    std::vector<int> m_gadgetIds;
    std::vector<int> m_flagIds;
    std::vector<int> m_spiritIds;
    std::vector<int> m_itemIds;
    std::vector<int> m_unknownIds;
    bool m_agentsClassified    = false;
    bool m_moveEventsBuilt     = false;
    bool m_castIntervalsBuilt  = false;

    // --- Agent overlay & calibration (Phase 2) ---
    bool m_showAgentOverlay = true;
    bool m_showMapCalibrationWindow = false;
    bool m_showInterpolationWindow = false;
    bool m_showRawPositions = false;
    bool m_showMapOriginAxes = false;
    bool m_calibrationLoaded = false;

    // --- StoC debug window ---
    bool m_showStoCWindow = false;
    StoCCategory m_selectedStoCCategory = StoCCategory::AgentMovement;
    int  m_selectedStoCEventIdx = -1;
    float m_stocListWidth = 180.f;
    bool m_stocShowRaw = false;

    // --- Loading overlay GPU resources ---
    struct OverlayVertex { float x, y, r, g, b, a; };

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_overlayVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_overlayPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_overlayIL;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_overlayVB;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_overlayDSS;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_overlayRS;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_overlayBS;

    static bool s_classRegistered;
    static constexpr wchar_t kWindowClassName[] = L"GWObsReplayWindowClass";
};
