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
#include <unordered_set>

struct ReplayHotkeys
{
    int rewind5s  = ImGuiKey_LeftArrow;
    int forward5s = ImGuiKey_RightArrow;
    int playPause = ImGuiKey_Space;

    static ReplayHotkeys& Get();
    void Save() const;
    void Load();
};

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
    void DrawAgentCylinders();
    void InitCylinderRenderer();
    void DrawMapCalibrationWindow();
    void DrawInterpolationWindow();
    void DrawTimelineController();
    void DrawShortcutPreferences();
    void DrawPartyWindows();
    void DrawMatchTimer();
    void DrawJumboMessages();
    void DrawMoraleBoostTimers();

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
    bool m_skillUseTimelineBuilt = false;
    bool m_knockdownIntervalsBuilt = false;

    // --- Combat Log ---
    bool m_showCombatLog     = false;
    bool m_combatLogBuilt    = false;
    std::vector<CombatLogRow> m_combatLog;
    bool m_clFilterDamage     = true;
    bool m_clFilterHeals      = true;
    bool m_clFilterSkills     = true;
    bool m_clFilterInterrupt  = true;
    bool m_clFilterCancel     = true;
    bool m_clFilterDeaths     = true;
    bool m_clFilterAttacks    = false;
    bool m_clFilterJumbo      = true;
    int  m_clFilterPlayerId   = -1;
    bool m_clAutoScroll       = true;
    int  m_clSelectedRowIdx   = -1;
    bool m_clScrollToSelected = false;

    // Skill name filter (multi-select with autocomplete)
    char m_clSkillSearchBuf[128] = {};
    bool m_clSkillSearchFocused  = false;
    std::vector<int> m_clFilterSkillIds;
    std::unordered_set<int> m_clFilterSkillSet;

    void DrawCombatLog();

    // Skill icon index: skill_id → full file path (populated once from Textures/Skill_Icons)
    std::unordered_map<int, std::string> m_skillIconIndex;
    bool m_skillIconIndexBuilt = false;
    void EnsureSkillIconIndex();
    std::unordered_map<int, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_skillIconCache;

    // --- Flag state machine ---
    enum class FlagLocationType { Base, Carried, Ground, Stand };

    struct FlagEvent {
        float time = 0.f;
        FlagLocationType location = FlagLocationType::Base;
        float x = 0, y = 0, z = 0;
        int carrierAgentId = -1;
        int flagAgentId = -1;   // when >= 0, use this agent's position (moves with carrier)
    };

    struct FlagTeamState {
        bool valid = false;
        float baseX = 0, baseY = 0, baseZ = 0;
        std::vector<int> flagAgentIds;
        std::vector<FlagEvent> timeline;
    };

    FlagTeamState m_flagState[2];
    std::vector<std::pair<float, int>> m_captureEvents;  // (time, teamIdx) — only one flag on stand at a time
    float m_flagStandX = 0, m_flagStandY = 0, m_flagStandZ = 0;
    bool  m_flagStandFound = false;
    bool  m_flagStateBuilt = false;

    FlagEvent EvaluateFlagState(int teamIdx, float time) const;
    void BuildFlagStateTimeline();
    void DrawFlags();

    // --- Scene overlays (timer, jumbo, morale) ---
    float m_matchStartOffset = 60.f;
    char  m_timerBuf[32] = {};
    char  m_moraleBuf[2][32] = {};
    ImFont* m_latoRegular = nullptr;
    ImFont* m_latoBold    = nullptr;
    ImFont* m_latoBoldBig = nullptr;

    // UI layout (positions stored as viewport fractions 0..1)
    struct UILayoutConfig
    {
        float jumboX = 0.50f,  jumboY = 0.30f;
        float moBlueX = 0.65f, moBlueY = 0.22f;
        float moRedX  = 0.35f, moRedY  = 0.22f;
        float timerX  = 0.50f, timerY  = 0.12f;
        bool  useCustom = false;

        bool  lodEnabled  = false;
        float lodDotDist  = 6000.f;
        float lodPillarDist = 1800.f;
    };
    UILayoutConfig m_uiLayout;
    bool m_showInterfacePrefs = false;
    int  m_draggingUIElement  = -1;   // -1=none, 0=jumbo, 1=moBlue, 2=moRed, 3=timer

    void DrawInterfacePreferences();
    void SaveUILayout();
    void LoadUILayout();
    bool HandleOverlayDrag(int elementIdx, float* fracX, float* fracY,
                           ImVec2 boxTL, ImVec2 boxBR);

    // --- Playback bar (always visible, bottom-anchored) ---

    // --- Party windows (Phase 5+6) ---
    bool m_showTeam1Party = true;
    bool m_showTeam2Party = true;
    bool m_partyWindowsPositioned = false;
    std::vector<int> m_team1PlayerIds;
    std::vector<int> m_team2PlayerIds;
    std::vector<int> m_team1NpcIds;
    std::vector<int> m_team2NpcIds;
    std::string m_team1GuildHeader;
    std::string m_team2GuildHeader;

    // --- Agent overlay & calibration (Phase 2) ---
    bool m_showAgentOverlay = true;
    bool m_showSkillIcons   = true;
    bool m_showSkillLasers  = true;
    void DrawSkillLasers();

    ImTextureID m_deathIconTex = nullptr;
    bool m_deathIconLoaded = false;

    // Cast bar gradient textures (1-pixel wide, N-pixel tall)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_castBarBgTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_castBarFillTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_castBarCancelTex;
    int  m_castBarTexH = 0;
    void EnsureCastBarTextures();
    bool m_showMapCalibrationWindow = false;
    bool m_showInterpolationWindow = false;
    bool m_showRawPositions = false;
    bool m_showMapOriginAxes = false;
    bool m_calibrationLoaded = false;

    bool m_showShortcutPreferences = false;

    // --- Follow-agent camera (Phase 4) ---
    enum class CameraMode { Free, FollowAgent };
    CameraMode m_cameraMode = CameraMode::Free;
    int        m_followedAgentId = -1;
    int        m_hoveredAgentId  = -1;

    float      m_followDist      = 0.f;
    float      m_followDistTarget = 0.f;
    float      m_followYaw       = 0.f;
    float      m_followPitch     = 0.f;
    static constexpr float kFollowLerpSpeed   = 6.0f;
    static constexpr float kFollowZoomFactor  = 0.6f;
    static constexpr float kFollowMinDist     = 50.f;
    static constexpr float kFollowMaxDist     = 10000.f;
    static constexpr float kFollowMinPitch    = -1.40f;  // ~-80 degrees
    static constexpr float kFollowMaxPitch    =  1.40f;  // ~+80 degrees

    void UpdateFollowCamera(float dt);
    void EnterFollowMode(int agentId);
    void ExitFollowMode();

    // --- Mouse drag & scroll zoom state ---
    bool  m_rightMouseDown = false;
    bool  m_leftMouseDown  = false;
    bool  m_leftClickPending = false;
    POINT m_mouseDragOrigin{};
    float m_panSpeed = 2.0f;
    float m_zoomSpeed = 200.0f;

    // --- StoC debug window ---
    bool m_showStoCWindow = false;
    StoCCategory m_selectedStoCCategory = StoCCategory::AgentMovement;
    int  m_selectedStoCEventIdx = -1;
    float m_stocListWidth = 180.f;
    bool m_stocShowRaw = false;

    // --- LOD levels for agent rendering ---
    enum class AgentLOD { Dot, Pillar, Cylinder };

    // --- Cylinder/Pillar agent renderer (3D) ---
    struct CylVertex { float x, y, z; float nx, ny, nz; float height01; };

    struct CylPerFrame { DirectX::XMFLOAT4X4 viewProj; DirectX::XMFLOAT4 camPos; };
    struct CylPerInst  { DirectX::XMFLOAT4X4 world; DirectX::XMFLOAT4 teamColor; };

    Microsoft::WRL::ComPtr<ID3D11VertexShader>   m_cylVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>    m_cylPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>    m_cylIL;
    Microsoft::WRL::ComPtr<ID3D11Buffer>         m_cylVB;
    Microsoft::WRL::ComPtr<ID3D11Buffer>         m_cylIB;
    Microsoft::WRL::ComPtr<ID3D11Buffer>         m_cylCBFrame;
    Microsoft::WRL::ComPtr<ID3D11Buffer>         m_cylCBInst;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_cylRS;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_cylDSS;
    Microsoft::WRL::ComPtr<ID3D11BlendState>     m_cylBS;
    UINT m_cylIndexCount = 0;
    bool m_cylInitialized = false;

    // Pillar geometry (thin cylinder for medium LOD)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_pillarVB;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_pillarIB;
    UINT m_pillarIndexCount = 0;

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
