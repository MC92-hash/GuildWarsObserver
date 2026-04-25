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
#include "TextureCache.h"
#include "HeatmapData.h"
#include "CatapultLeverState.h"
#include "HeatmapRenderer.h"
#include "HeatmapMenu.h"
#include "AnnotationManager.h"
#include "FlagTimelineBuilder.h"
#include "Animation/AnimationController.h"
#include "Animation/AnimationClip.h"
#include "Animation/Skeleton.h"
#include "AnimatedMeshInstance.h"
#include "animation_state.h"
#include "Cache/AnimationDiscoveryCache.h"
#include "Cache/AnimationClipCache.h"
#include "ReplayPanelLayout.h"
#include "BitmapFont.h"
#include <string>
#include <memory>
#include <utility>
#include <variant>
#include <unordered_set>
#include <chrono>

class SpatialAudioEngine;

#include "ReplayHotkeys.h"

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

    void ApplyReplayCameraFovFromSettings();

private:
    ReplayWindow() = default;

    bool InitWindow(HINSTANCE hInstance, const std::wstring& title);
    bool InitGraphics();
    bool InitLoadingOverlay();
    void InitImGui();
    void ShutdownImGui();

    // Phased map loading (one phase per Tick to keep window responsive)
    enum class LoadingPhase { Validate, Init, PropModels, PlaceProps, FadingOut, Ready, Error };

    void StepValidate();
    void StepLoadInit();
    void StepLoadPropModels();
    void StepPlaceProps();
    void ProgressiveAgentModelPump();

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
    void LoadAgentModels();
    void DrawAgentModels();
    void DrawMapCalibrationWindow();
    void DrawInterpolationWindow();
    void DrawTimelineController();
    void DrawShortcutPreferences();
    void DrawPartyWindows();
    void DrawMatchTimer();
    void DrawJumboMessages();
    void DrawMoraleBoostTimers();
    void DrawAssetInspectorWindow();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND m_hwnd = nullptr;
    bool m_alive = false;

    // Edge detection for GetAsyncKeyState-based hotkey polling.
    // Indexed by ImGuiKey value; true = key was down last frame.
    bool m_prevKeyDown[ImGuiKey_NamedKey_COUNT] = {};
    bool HotkeyPressed(int imguiKey);  // returns true on rising edge

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
    std::vector<uint32_t> m_propModelFileHashes;
    int m_propModelLoadIndex = 0;
    int m_propPlaceIndex = 0;
    int m_totalPropFilenames = 0;
    int m_totalPropInstances = 0;

    static constexpr int kPropModelBatchSize = 15;
    static constexpr int kPropPlaceBatchSize = 10;

    // Time-budgeted loading: process items until this per-frame budget is exceeded.
    // Replaces fixed batch caps for prop and agent GPU creation phases.
    static constexpr float kLoadFrameBudgetMs = 30.0f;

    // Per-phase timing instrumentation (measured durations in seconds)
    using LoadClock = std::chrono::steady_clock;
    LoadClock::time_point m_phaseStartTime;
    LoadClock::time_point m_totalLoadStartTime;
    bool m_totalLoadTimerStarted = false;
    struct LoadPhaseTiming {
        double validateSec  = 0.0;
        double initSec      = 0.0;
        double propModelSec = 0.0;
        double placePropSec = 0.0;
        double agentIOSec   = 0.0;
        double agentGPUSec  = 0.0;
        double totalSec     = 0.0;
        bool   placePropsLogged = false;
    };
    LoadPhaseTiming m_loadTiming;

    void SetupAnimatedProp(int propIndex, const FFNA_ModelFile& modelFile,
                           uint32_t modelFileHash,
                           const std::vector<Mesh>& meshes,
                           const std::vector<PerObjectCB>& perObjectCBs,
                           const std::vector<int>& meshIds,
                           const std::vector<std::vector<int>>& perMeshTexIds,
                           PixelShaderType pst,
                           uint32_t segmentHash,
                           size_t segmentFallbackIndex);

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
    bool m_replayTimeConsolidated = false;
    float m_displayTimeOffset  = 0.f;
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

    // --- Flag Timeline (from FlagTimelineBuilder) ---
    FlagTimeline m_flagTimeline;
    bool m_flagTimelineBuilt = false;

    void BuildFlagTimeline();
    void DrawFlags();

    // --- Flag event messages (displayed below timer) ---
    struct FlagEventMessage {
        float time = 0.f;
        std::string playerName;
        int playerTeam = 0;    // 0=blue, 1=red
        int flagTeam   = 0;    // 0=blue, 1=red
        FlagTimelineEventType eventType = FlagTimelineEventType::Spawn;
    };
    std::vector<FlagEventMessage> m_flagMessages;
    void BuildFlagMessages();
    void DrawFlagEventMessages();

    // --- Flag debug panel ---
    bool m_showFlagDebugWindow = false;
    void DrawFlagDebugWindow();

    // --- Asset Inspector / Blacklist ---
    bool m_showAssetInspector = false;
    bool m_assetSelectionEnabled = false;
    int  m_pickedPropIndex = -1;
    int  m_hoveredPropMeshId = -1;
    std::unordered_map<int, uint32_t> m_meshIdToPropIndex;
    void SetPickedProp(int newPropIndex);
    void SetPropHighlight(int propIndex, uint32_t state);
    void SetPropAlpha(int propIndex, float alpha);

    // --- Bundle carry tracking (repair kits, vine seeds) ---
    struct LeverCapEvent {
        float time = 0.f;
        uint32_t objectId = 0;
        float x = 0, y = 0, z = 0;
        int teamIdx = -1;
    };
    struct VineBridgeEvent {
        float time = 0.f;
        uint32_t objectId = 0;
        float x = 0, y = 0, z = 0;
        int teamIdx = -1;
    };

    std::unordered_map<int, std::vector<BundleCarryInterval>> m_bundleCarry;
    std::unordered_map<int, float> m_itemRemoveTime;
    std::vector<LeverCapEvent>     m_leverCaps;
    std::vector<VineBridgeEvent>   m_vineBridgeEvents;
    bool m_bundleCarryBuilt = false;

    // --- Catapult lever state tracking (Warrior's Isle) ---
    static constexpr int kWarriorsIsleMapId = 171;
    std::unordered_map<uint32_t, CatapultLeverState> m_catapultStates;

    void BuildBundleCarryTimeline();
    BundleType GetPlayerBundleType(int agentId, float time) const;
    void DrawBundleItems();

    // --- Agent incarnation routing (for recycled agent IDs) ---
    // Maps originalAgentId -> list of synthetic incarnation IDs
    std::unordered_map<int, std::vector<int>> m_incarnationMap;
    // Given an original agent_id and a timestamp, find the incarnation entry
    // whose snapshot range covers that time. Returns agentId if no split exists.
    int ResolveAgentAtTime(int agentId, float time) const;

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

    ReplayPanelLayout m_panelLayout;
    bool m_panelLayoutRegistered = false;
    void RegisterPanelLayout();
    uint64_t m_lastPanelStateHash = 0;
    uint64_t ComputePanelStateHash() const;

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
    bool m_alliesOpenTeam1 = false;
    bool m_alliesOpenTeam2 = false;
    std::vector<int> m_team1PlayerIds;
    std::vector<int> m_team2PlayerIds;
    std::vector<int> m_team1NpcIds;
    std::vector<int> m_team2NpcIds;
    std::string m_team1GuildHeader;
    std::string m_team2GuildHeader;

    // --- Damage / Heal meter (party window bars) ---
    bool m_showDamageMeter = false;
    bool m_showHealMeter   = false;

    struct MeterEntry { int value = 0; };
    std::unordered_map<int, MeterEntry> m_meterDmg;
    std::unordered_map<int, MeterEntry> m_meterHeal;
    int   m_meterMaxDmg    = 0;
    int   m_meterMaxHeal   = 0;
    int   m_meterTotalDmg  = 0;
    int   m_meterTotalHeal = 0;
    float m_meterLastTime  = -1.f;
    int   m_meterLastIdx   = 0;

    struct MeterAbsEntry { float time = 0.f; int casterId = 0; int absValue = 0; bool isDamage = false; };
    std::vector<MeterAbsEntry> m_meterAbsCache;
    bool m_meterAbsCacheBuilt = false;

    void AccumulateMeterData();
    void BuildMeterAbsCache();

    // --- Agent overlay & calibration (Phase 2) ---
    bool m_showAgentOverlay = true;
    bool m_showSkillIcons   = true;
    bool m_showSkillLasers  = true;
    bool m_show3DLabels     = true;
    bool m_showNameFilterPanel = false;
    std::unordered_set<int> m_hiddenNameAgents;   // agent IDs whose names are hidden

    void DrawSkillLasers();
    void DrawNameFilterPanel();

    ImTextureID m_deathIconTex = nullptr;
    bool m_deathIconLoaded = false;

public:
    // --- Fog of War ---
    int   m_fogPerspective  = 0;     // 0=Off, 1=Blue, 2=Red
    bool  m_fogGhostMode    = false;
    int   m_fogLastActive   = 1;
    int   m_fogPlayerAgent  = -1;    // -1=team mode, else single-player agent id
    bool  m_fogInitialized  = false;

    struct FogCBData {
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT4   playerPos[8];
        float compassRadius;
        float fogOpacity;
        float edgeSoftness;
        float refHeight;
        float viewportW;
        float viewportH;
        int   playerCount;
        float pad;
    };

    Microsoft::WRL::ComPtr<ID3D11VertexShader>      m_fogVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       m_fogPS;
    Microsoft::WRL::ComPtr<ID3D11Buffer>            m_fogCB;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        m_fogBS;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_fogDSS;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   m_fogRS;

    void InitFogRenderer();
    void DrawFogOfWar();
    void DrawFogOfWarToolbar();
    bool IsAgentInFog(int agentId) const;

    // --- Morale Panel ---
    bool m_showMoralePanel = false;
    void DrawMoralePanel();

    // --- Lord Damage Panel ---
    struct LordAttackerRow {
        int   agentId     = 0;
        int   professionId = 0;
        std::string name;
        int   totalDmg    = 0;
        float pct         = 0.f;
        uint8_t teamId    = 0;
    };
    struct LordHitEvent {
        float time     = 0.f;
        int   casterId = 0;
        int   rawDmg   = 0;
    };
    struct LordDamageData {
        int      lordAgentId = -1;
        float    lowPointHp  = 1.f;
        int      totalDmgAbs = 0;
        int      phaseCount  = 0;
        uint32_t lordMaxHp   = 0;
        std::vector<LordAttackerRow> attackers;
        std::vector<bool> damageBuckets;
        std::vector<LordHitEvent> hits;
    };
    LordDamageData m_lordDmg[2];
    bool m_showLordDamagePanel = false;
    bool m_lordDamageBuilt = false;
    void BuildLordDamageData();
    void DrawLordDamagePanel();

    // --- Event Timeline ---
    enum class TimelineEventType { Death, Resurrection, FlagCapture, FlagReturn, MoraleBoost, LordAttacked, Victory, ShrineCaptured, ShrineNeutralized };
    struct TimelineEvent {
        float time = 0.f;
        TimelineEventType type = TimelineEventType::Death;
        int agentId = 0;
        int teamId = 0;
        int professionId = 0;
        std::string label;
    };
    struct TimelineData {
        std::vector<float> blueHealth;
        std::vector<float> redHealth;
        std::vector<TimelineEvent> events;
        bool computed = false;
    };
    TimelineData m_timeline;
    bool m_showEventTimeline = false;
    bool m_tlFilterDeath = true;
    bool m_tlFilterRes = true;
    bool m_tlFilterFlag = true;
    bool m_tlFilterFlagReturn = true;
    bool m_tlFilterMorale = true;
    bool m_tlFilterLord = true;
    bool m_tlFilterVictory = true;
    bool m_tlFilterShrine = true;
    void BuildTimelineData();
    void DrawEventTimeline();

    // --- Range Rings ---
    struct RingDef {
        const char* name;
        float radius;
        ImU32 color;
        float thickness;
        bool solid;
        float dashOn, dashOff;
        float fillAlpha;
    };
    static constexpr int kRingTypeCount = 8;
    bool m_showRangeRings = false;
    bool m_ringType[kRingTypeCount] = { false, false, false, false, false, true, false, false };
    bool m_ringShowBlue    = true;
    bool m_ringShowRed     = true;
    int  m_ringAgentFilter = -1;
    int  m_ringHoveredType = -1;
    bool m_ringSoloActive  = false;
    bool m_ringSoloPrev[kRingTypeCount] = {};
    std::unordered_set<int> m_ringHiddenAgents;

    // --- Isle of Wurms: South Health Shrine capture overlay ---
    enum class ShrineState : uint8_t {
        Neutral,
        OwnedByBlue,
        OwnedByRed,
        CapturingBlue,
        CapturingRed,
        DecappingBlue,
        DecappingRed,
        Contested
    };
    struct ShrineSample {
        ShrineState state       = ShrineState::Neutral;
        uint8_t ownerTeam       = 0; // 0=neutral, 1=blue, 2=red
        uint8_t progressTeam    = 0; // team whose color to render in the fill
        int     bluePips        = 0;
        int     redPips         = 0;
        int     effectivePips   = 0;
        float   progress        = 0.f; // 0..1, normalized to jumbo endpoints
    };
    int m_wurmsSouthShrineAgentId = -1;
    std::vector<std::pair<float, int>> m_wurmsShrineCaptureEvents; // sorted by time: team 1|2 for capture, 0 for neutralize
    std::vector<ShrineSample> m_wurmsShrineSamples;                // pre-computed timeline (sampled at m_wurmsShrineSampleDt)
    static constexpr float m_wurmsShrineSampleDt = 0.1f;
    ShrineSample m_wurmsShrineCurrentSample;
    void PrecomputeShrineTimeline();
    void DrawRangeRings();
    void DrawSpiritRanges();
    void DrawWurmsShrineCaptureRadius();
    void DrawShrineBeam(uint8_t ownerTeam, float sx, float sy, float sz);
    void DrawRangeRingToolbar();

private:
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
    static constexpr float kFollowMinDist     = 50.f;
    static constexpr float kFollowMaxDist     = 10000.f;
    static constexpr float kFollowMinPitch    = -1.40f;  // ~-80 degrees
    static constexpr float kFollowMaxPitch    =  1.40f;  // ~+80 degrees

    // Smooth transition when switching between followed agents:
    // interpolates the orbit center from old agent to new agent while
    // keeping orbit distance/angles fixed (no disorienting camera roll).
    bool                   m_followTransActive       = false;
    float                  m_followTransElapsed      = 0.f;
    static constexpr float kFollowTransDuration      = 1.5f;
    int                    m_followTransFromAgentId   = -1;

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

    // --- 3D Agent model rendering (replaces cylinders when enabled) ---
    bool m_showAgentModelWindow = false;
    bool m_useAgentModels = false;
    bool m_agentModelsLoaded = false;
    float m_agentModelScale = 1.0f;

    // Async loading infrastructure
    std::thread m_agentModelLoadThread;
    std::atomic<bool> m_agentModelsLoading{false};
    std::atomic<bool> m_bgLoadDone{false};
    std::atomic<int> m_bgLoadProgress{0};
    int m_bgLoadTotal = 0;
    int m_agentModelCreateIndex = 0;
    std::vector<uint32_t> m_agentModelCreateOrder;

    enum class AgentLoadSubPhase : int {
        Idle = 0, ParsingModel, LoadingTextures, DiscoveringAnimations,
        ScanningReferences, ScanningMFT, BuildingAnimData
    };
    std::atomic<int> m_bgLoadSubPhase{0};

    void LoadAgentModelsAsync();
    void LoadAgentModelsIO();
    void StepCreateAgentModelResources();
    void DrawAgentModelLoadingBanner();
    float m_agentLoadBannerFade = 0.f;

    GW::Cache::AnimationDiscoveryCache m_animDiscoveryCache;
    GW::Cache::AnimationClipCache m_clipCache;

    struct SegmentRef {
        int clipIndex = 0;
        int segmentIndex = 0;
    };

    struct AnimClipEntry {
        std::shared_ptr<GW::Animation::AnimationClip> clip;
        std::shared_ptr<GW::Animation::Skeleton> skeleton;
        uint32_t sourceFileHash = 0;
    };

    struct ParsedTextureEntry {
        int decodedHash = 0;
        DatTexture datTex;
        bool hasDatTex = false;
    };

    struct AgentModelInstance {
        std::vector<int> meshIds;
        std::vector<PerObjectCB> templateCBs;
        float nativeHeight = 0.f;
        float nativeMinY = 0.f;
        DirectX::XMFLOAT3 nativeCenter = { 0.f, 0.f, 0.f };

        // Data preserved from IO phase for D3D resource creation
        std::vector<Mesh> originalMeshes;
        std::vector<ParsedTextureEntry> parsedTextures_;
        PixelShaderType pixelShaderType = PixelShaderType::OldModel;
        bool texturesOk = false;

        // Animation data (shared across all agents using this model type)
        uint32_t modelHash0 = 0;
        uint32_t modelHash1 = 0;
        std::vector<AnimClipEntry> allClips;
        std::shared_ptr<GW::Animation::AnimationClip> clip;
        std::shared_ptr<GW::Animation::Skeleton> skeleton;
        std::vector<AnimationPanelState::SubmeshBoneData> submeshBoneData;
        std::vector<std::vector<uint32_t>> perVertexBoneGroups;
        std::unordered_map<uint32_t, SegmentRef> animCodeToSegment;
        bool hasAnimation = false;
    };

    // file hash -> parsed model template (shared geometry, one AddProp per unique model)
    std::unordered_map<uint32_t, AgentModelInstance> m_agentModelTemplates;

    // agent_id -> per-agent mesh IDs (each agent gets its own AddProp so transforms are independent)
    std::unordered_map<int, std::vector<int>> m_agentMeshIds;

    // agent_id -> file hash (cached so we don't re-lookup every frame)
    std::unordered_map<int, uint32_t> m_agentFileHashCache;

    // Per-frame diagnostic: why each agent's model was shown/hidden
    std::unordered_map<int, std::string> m_agentModelRenderStatus;

    // --- Per-agent animation state ---
    struct AgentAnimState {
        std::unique_ptr<GW::Animation::AnimationController> controller;
        std::vector<std::shared_ptr<AnimatedMeshInstance>> animMeshes;
        std::vector<PerObjectCB> perMeshCBs;
        std::vector<std::vector<int>> perMeshTextureIds;
        PixelShaderType pixelShaderType = PixelShaderType::OldModel;
        uint32_t lastAnimCode = UINT32_MAX;
        int currentClipIndex = 0;
        bool hasSkinning = false;
        bool lastLookupFailed = false;
        bool wasDead = false;
        bool wasCasting = false;
        bool wasKnockedDown = false;
        bool postKdGetUp = false;
        float effectiveSpeedMult = 1.0f;

        // Movement animation state
        float prevPosX = 0.f, prevPosY = 0.f;
        float prevTime = -1.f;
        float smoothVelocity = 0.f;
        int   currentMovementDirIndex = -1;
        bool  isPlayingMovementAnim = false;
        bool  isPlayingIdleAnim = false;
    };
    std::unordered_map<int, AgentAnimState> m_agentAnimStates;
    void DrawSkinnedAgentModels();

    // --- Loading overlay GPU resources ---
    struct OverlayVertex { float x, y, r, g, b, a; };

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_overlayVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_overlayPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_overlayIL;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_overlayVB;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_overlayDSS;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_overlayRS;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_overlayBS;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_additiveBS;

    // --- Match loading screen ---
    using LsClock = std::chrono::steady_clock;
    TextureCache m_lsTexCache;
    std::string  m_lsBgPath;
    LsClock::time_point m_lsStartTime;
    bool  m_lsStartTimeSet = false;
    bool  m_lsHitReady     = false;
    LsClock::time_point m_lsReadyTime;

    std::string GetMatchLoadingBgPath() const;
    static const char* GetMapScreenshotFile(int mapId);
    static const char* GetMapNameForLoading(int mapId);

    // --- Auto Camera system ---
    struct AutoCameraConfig {
        float lookaheadSec    = 3.f;
        float hpThreshold     = 0.70f;
        float minDwellTime    = 5.f;
        bool  focusDeath      = true;
        bool  focusLowHp      = true;
        bool  focusLord       = true;
        bool  focusFlag       = true;
        bool  focusRez        = true;
        bool  focusIsolated   = true;
        bool  focusFlagCarry  = true;
    };

    struct AutoCameraState {
        int         currentTarget   = -1;
        int         currentPriority = 0;
        float       dwellTimer      = 0.f;
        float       switchCooldown  = 0.f;
        std::string currentReason;
        bool        lerpActive      = false;
        float       lerpElapsed     = 0.f;
        float       lerpDuration    = 0.5f;
        DirectX::XMFLOAT3 lerpFrom{};
    };

    struct AutoCamDebugEntry {
        int         agentId = 0;
        std::string name;
        float       hpPct       = 0.f;
        float       dmgRate     = 0.f;
        float       projectedHp = 0.f;
        int         score       = 0;
        std::string reason;
        bool        disqualified = false;
        std::string disqualReason;
    };
    std::vector<AutoCamDebugEntry> m_autoCamDebug;
    bool m_autoCamShowDebug = false;

    bool            m_autoCameraEnabled = false;
    bool            m_showAutoCameraPanel = false;
    AutoCameraConfig m_autoCamCfg;
    AutoCameraState  m_autoCamState;

    void UpdateAutoCamera(float dt);
    void DrawAutoCameraPanel();
    void DrawAutoCameraDebugPanel();
    void LoadAutoCamSettings();
    void SaveAutoCamSettings();
    float EstimateProjectedHp(const AgentReplayData& ard, float time, float lookahead) const;
    float EstimateProjectedHpEx(const AgentReplayData& ard, float time, float lookahead, float* outDmgRate) const;
    bool  WillAgentDie(const AgentReplayData& ard, float time, float lookahead) const;
    bool  IsAgentTakingDamage(int agentId, float time, float window = 1.5f) const;
    bool  IsAgentIsolated(const AgentReplayData& ard, float time, float radius = 2500.f) const;
    bool  IsAgentCastingRez(const AgentReplayData& ard, float time) const;

    // --- Top View mode ---
    bool  m_topViewActive         = false;
    bool  m_topViewTransitioning  = false;
    float m_topViewTransTimer     = 0.f;
    float m_topViewTransDuration  = 1.0f;

    // Saved state before entering top view
    DirectX::XMFLOAT3 m_tvSavedPos{};
    float              m_tvSavedYaw   = 0.f;
    float              m_tvSavedPitch = 0.f;
    CameraMode         m_tvSavedCamMode = CameraMode::Free;
    int                m_tvSavedFollowId = -1;
    float              m_tvSavedFollowDist = 0.f;
    float              m_tvSavedFollowYaw  = 0.f;
    float              m_tvSavedFollowPitch = 0.f;
    bool               m_tvSavedAutoCam    = false;
    int                m_tvSavedFogPerspective = 0;
    bool               m_tvSavedSkillIcons = true;
    bool               m_tvSavedSkillLasers = true;
    bool               m_tvSavedLodEnabled = false;
    bool               m_tvSavedShow3DLabels = true;
    bool               m_tvSavedNamePanel    = false;

    // Transition interpolation endpoints
    DirectX::XMFLOAT3 m_tvTransFrom{};
    float              m_tvTransFromYaw   = 0.f;
    float              m_tvTransFromPitch = 0.f;
    DirectX::XMFLOAT3 m_tvTransTo{};
    float              m_tvTransToYaw     = 0.f;
    float              m_tvTransToPitch   = 0.f;

    void EnterTopView();
    void ExitTopView();
    void UpdateTopViewTransition(float dt);
    DirectX::XMFLOAT3 ComputeTopViewPosition() const;

    // --- Piano Roll Panel ---
    bool  m_showPianoRoll       = false;
    int   m_pianoRollZoomIdx    = 2;   // index into zoom table: 0=±5s,1=±10s,2=±15s,3=±30s,4=±60s
    int   m_pianoRollHoverRow   = -1;  // agent id of hovered row (-1 = none)
    bool  m_pianoRollTeam1Open  = true;
    bool  m_pianoRollTeam2Open  = true;
    float m_pianoRollZoomToast  = -10.f; // timestamp when zoom toast was triggered

    void DrawPianoRollPanel();

    // Cached from ImGui overlay for use in Update() (suppresses camera keys)
    bool m_imguiWantTextInput = false;

    // --- Match Notepad ---
    bool m_showNotepad = false;
    std::string m_notepadBuffer;
    std::string m_notepadMatchId;

    void DrawNotepad();

    // --- Picture-in-Picture (Split Camera) ---
    bool  m_pipEnabled       = false;
    int   m_pipTargetAgent   = -1;
    int   m_pipPrevTarget    = -1;
    int   m_pipManualAgent   = -1;   // -1 = auto-detect, >=0 = pinned to specific agent
    float m_pipDwellTimer    = 0.f;
    float m_pipFollowDist    = 350.f;
    float m_pipFollowYaw     = 0.f;
    float m_pipFollowPitch   = 0.6f;
    bool  m_pipHovered       = false;
    int   m_pipWidth         = 480;
    int   m_pipHeight        = 270;

    DirectX::XMFLOAT4X4 m_pipViewProj{};              // cached for overlay projection
    DirectX::XMFLOAT3   m_pipCamPos{};

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_pipTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_pipRTV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_pipDepthTexture;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>   m_pipDSV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_pipSRV;
    bool m_pipResourcesReady = false;

    uint64_t m_frameCount = 0;
    uint64_t m_lastAnimUpdateFrame = 0;

    void InitPiPResources();
    void RenderPiP();
    void UpdatePiPTarget();
    void DrawPiPPanel();

    // --- Player Info Panel ---
    bool m_showPlayerInfoPanel = false;
    int  m_playerInfoAgentId   = -1;

    struct WeaponSetEntry {
        uint16_t mainId   = 0;   // weapon_item_id   (identity key part 1)
        uint16_t offId    = 0;   // offhand_item_id   (identity key part 2)
        uint16_t weapCat  = 0;   // weapon_type       (for icon resolution)
        uint8_t  mainType = 0;   // weapon_item_type  (for icon resolution)
        uint8_t  offType  = 0;   // offhand_item_type (informational)
        float    firstSeen = 0.f; // timestamp of first appearance
        int        disambig   = 0;   // >0 if multiple sets share same icon appearance (subscript number)
        BundleType bundleType = BundleType::Unknown;
    };
    struct PlayerWeaponSets {
        int agentId = -1;
        std::vector<WeaponSetEntry> sets;
        bool built = false;
    };
    PlayerWeaponSets m_pipWeaponSets;

    struct PipSkillStat {
        int   skillId     = 0;
        int   totalCasts  = 0;
        float castPct     = 0.f;
        struct TargetBreakdown {
            int         targetId = 0;
            std::string name;
            uint8_t     teamId   = 0;
            int         count    = 0;
            float       pct      = 0.f;
        };
        std::vector<TargetBreakdown> targets;
    };

    void OpenPlayerInfoPanel(int agentId);
    void ClosePlayerInfoPanel();
    void DrawPlayerInfoPanel();
    void BuildWeaponSets(int agentId);
    std::vector<PipSkillStat> BuildSkillStats(int agentId, float currentTime) const;

    // --- Skill Analytics Panel ---
    struct SkillAnalyticsStat {
        int   skillId      = 0;
        int   totalCasts   = 0;
        int   cancelled    = 0;
        int   interrupted  = 0;
        int   totalDamage  = 0;
        int   totalHealing = 0;
        struct TargetBreakdown {
            int         targetId    = 0;
            std::string name;
            uint8_t     teamId      = 0;
            int         primaryProf = 0;
            int         castCount   = 0;
            float       castPct     = 0.f;
            int         damage      = 0;
            int         healing     = 0;
        };
        std::vector<TargetBreakdown> targets;
    };

    struct PlayerAnalytics {
        int         agentId       = 0;
        std::string playerName;
        uint8_t     teamId        = 0;
        int         primaryProf   = 0;
        int         secondaryProf = 0;
        int         playerNumber  = 0;
        int         totalDamage   = 0;
        int         totalHealing  = 0;
        std::vector<SkillAnalyticsStat> skills;
    };

    bool  m_showSkillAnalytics      = false;
    bool  m_analyticsShowTeam[2]    = { true, true };
    bool  m_analyticsProfFilter[10] = { true, true, true, true, true, true, true, true, true, true };
    float m_analyticsCacheTime      = -1.f;
    std::vector<PlayerAnalytics> m_analyticsCache;
    std::unordered_set<uint64_t> m_analyticsExpandedSkills;
    std::unordered_set<int> m_analyticsOpenPlayers;

    std::vector<PlayerAnalytics> BuildAllPlayerAnalytics(float currentTime) const;
    void DrawSkillAnalyticsPanel();
    void DrawSkillAnalyticsPlayerPopups();

    // --- Incoming effect display ---
    enum class IncomingEffectType { Damage, Heal, Interrupt, Condition, Hex, BasicAttack };
    struct IncomingEffect {
        int             skillId     = 0;
        std::string     label;
        IncomingEffectType type     = IncomingEffectType::Damage;
        float           spawnTime   = 0.f;
        float           xOffset     = 0.f;
    };
    static constexpr float kEffectLifetime  = 1.5f;
    std::vector<IncomingEffect> m_incomingEffects;
    float m_lastEffectScanTime = -1.f;
    int   m_focusedAgentId     = -1;

    // Bitmap font textures for GW-style floating numbers
    BitmapFont m_damageBitmapFont;
    BitmapFont m_healBitmapFont;
    void EnsureBitmapFontsLoaded();

    void UpdateIncomingEffects();
    void RenderIncomingEffects();
    void DrawFollowedAgentHUD();
    int  GetFocusedAgentId() const;

    // PiP-specific incoming effects (simplified scan of raw combat events)
    std::vector<IncomingEffect> m_pipIncomingEffects;
    float m_pipLastEffectScanTime = -1.f;
    int   m_pipEffectAgentId      = -1;
    void  UpdatePiPIncomingEffects();

    // --- Spatial Audio ---
    std::unique_ptr<SpatialAudioEngine> m_audioEngine;
    bool m_audioInitialized = false;
    bool m_audioEnabled     = false;
    bool m_showAudioDebug   = false;

    // Per-agent cursors into skillUseHistory for caster (startTime) and target (endTime) sounds
    std::unordered_map<int, size_t> m_audioSkillCursor;       // caster: tracks startTime
    std::unordered_map<int, size_t> m_audioTargetCursor;      // target: tracks endTime
    std::vector<std::pair<int, size_t>> m_targetEventOrder;   // (agentId, eventIdx) sorted by endTime
    bool m_targetOrderBuilt = false;
    size_t m_targetOrderCursor = 0;
    float m_audioLastTime = -1.f;

    void InitAudioEngine();
    void UpdateAudioPlayback(float currentTime, float dt);

    // --- Heatmap system ---
    HeatmapSettings       m_heatmapSettings;
    HeatmapAccumulator    m_heatmapAccumulator;
    HeatmapRenderer       m_heatmapRenderer;
    bool                  m_heatmapInitialized = false;
    bool                  m_heatmapMeshBuilt   = false;
    bool                  m_heatmapPopulated   = false;

    void InitHeatmapRenderer();
    void PopulateHeatmapFromSnapshots();
    void UpdateHeatmapSamples();
    void DrawHeatmapOverlay();
    void SaveHeatmapSettings();
    void LoadHeatmapSettings();
    void ResolveHeatmapLayers();

    // --- Annotation / Drawing tools ---
    AnnotationManager m_annotationMgr;

    static bool s_classRegistered;
    static constexpr wchar_t kWindowClassName[] = L"GWObsReplayWindowClass";
};
