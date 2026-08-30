#pragma once

#include "build_config.h"   // GWO_DEVELOPER — gates the weapon diagnostics panel
#include "DeviceResources.h"
#include "StepTimer.h"
#include "InputManager.h"
#include "MapRenderer.h"
#include "DATManager.h"
#include "Terrain.h"
#include "ReplayMapData.h"
#include "ReplayLibrary.h"
#include "GuildCapeCache.h"
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
#include "Animation/WeaponSocket.h"
#include "AnimatedMeshInstance.h"
#include "animation_state.h"
#include "Cache/AnimationDiscoveryCache.h"
#include "Cache/AnimationClipCache.h"
#include "ReplayPanelLayout.h"
#include "BitmapFont.h"
#include "AttributeDeducer.h"
#include "HealthModel.h"
#include "SkillDatabase.h"
#include <string>
#include <memory>
#include <utility>
#include <variant>
#include <unordered_set>
#include <chrono>

class SpatialAudioEngine;
class SkillSoundTable;
// Opaque declaration: gives a complete type for storage without dragging xaudio2.h in here.
// The enumerators themselves are only named in the .cpp files that include SpatialAudioEngine.h.
enum class SoundLogCategory : uint8_t;

#include "ReplayHotkeys.h"

// ---------------------------------------------------------------------------
// Agent model slots
//
// An agent shows one model at a time, but a Dervish who uses an avatar form swaps model
// mid-match and back again. Both models are built up front and live in the mesh manager
// together; every per-agent model map (mesh ids, file hash, animation state, render status)
// is keyed by a slot rather than by the agent id, and each frame exactly one of an agent's
// slots is drawn. The base skin keeps the agent id as its key, so agents that never
// transform - which is all of them in most matches - are laid out exactly as before.
//
// Agent ids are the recording's own, below kSyntheticIdBase (1'000'000) for real agents and
// just above it for split incarnations; the bases here leave that whole range alone.
// ---------------------------------------------------------------------------

inline constexpr int kAvatarSlotBase = 100'000'000;
inline constexpr int kAvatarSlotSpan =  10'000'000;

inline int AvatarSlotKey(int agentId, uint32_t avatarModelId)
{
    return kAvatarSlotBase + static_cast<int>(avatarModelId) * kAvatarSlotSpan + agentId;
}

inline bool IsAvatarSlot(int slotKey) { return slotKey >= kAvatarSlotBase; }

// The agent a slot belongs to. Identity for a base slot.
inline int SlotOwnerAgentId(int slotKey)
{
    return IsAvatarSlot(slotKey) ? (slotKey - kAvatarSlotBase) % kAvatarSlotSpan : slotKey;
}

// The avatar model id a slot renders, 0 for a base slot.
inline uint32_t SlotAvatarModelId(int slotKey)
{
    return IsAvatarSlot(slotKey)
        ? static_cast<uint32_t>((slotKey - kAvatarSlotBase) / kAvatarSlotSpan)
        : 0u;
}

class ReplayWindow final : public DX::IDeviceNotify
{
public:
    static bool RegisterWindowClass(HINSTANCE hInstance);
    static ReplayWindow* Create(HINSTANCE hInstance, const MatchMeta& match, DATManager* sharedDatManager,
                                const std::unordered_map<int, std::vector<int>>& hashIndex);

    // The same object with no window, no device and no map: everything the analysis passes need
    // and nothing they do not.
    //
    // This exists because the whole analysis chain -- agent and StoC parsing, the combat log, the
    // max-HP breakpoint solve, the morale timelines, HealthModel::SolveArmour and finally
    // AttributeModel::SolveAll -- reads and writes plain data and never touches the renderer.
    // Only its HOUSING was graphical. `Create` is this plus InitWindow/InitGraphics/
    // InitLoadingOverlay, and those three are exactly what a batch export must not do.
    //
    // Drive it with TickHeadless() until AnalysisComplete(), then ExportAttributes().
    static ReplayWindow* CreateHeadless(const MatchMeta& match, DATManager* sharedDatManager,
                                        const std::unordered_map<int, std::vector<int>>& hashIndex);

    // One analysis step. Same passes Tick() runs, stopping before anything that wants a device.
    void TickHeadless();

    // Every pass the attribute solve depends on has run.
    bool AnalysisComplete() const { return m_attributesDeduced; }

    // The parsers failed or the recording carries nothing to solve, so TickHeadless will never
    // reach AnalysisComplete and the caller should stop rather than spin.
    bool AnalysisStalled() const;

    // Write the solved builds as JSON, keyed by (party_id, player_number) from `infos.json`.
    // False when nothing was solved or the file could not be written.
    bool ExportAttributes(const std::filesystem::path& outPath) const;

    ~ReplayWindow();

    ReplayWindow(const ReplayWindow&) = delete;
    ReplayWindow& operator=(const ReplayWindow&) = delete;

    void Tick();
    bool IsAlive() const { return m_alive; }
    HWND GetHWND() const { return m_hwnd; }

    // Still building the map: the window is not interactive yet.
    bool IsLoading() const
    {
        return m_loadingPhase != LoadingPhase::Ready
            && m_loadingPhase != LoadingPhase::Error;
    }

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
    // No window, no device, no map. Set only by CreateHeadless; Tick() returns as soon as the
    // analysis passes are done rather than falling through to anything that draws.
    bool m_headless = false;
    // Wall-clock of the last time an analysis flag flipped, so a headless run can tell "still
    // parsing a big recording" from "stuck, and will be stuck forever".
    double m_headlessLastProgress = 0.0;
    int    m_headlessProgressMark = -1;

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
    SkillDatabaseView m_skillView;
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
                           size_t segmentFallbackIndex,
                           uint32_t animFileHash = 0,
                           const std::unordered_map<size_t, uint32_t>& submeshBoneOverride = {});

    // Uncharted Isle double-gate setup. Locks the pillar/frame submesh (staticSubmesh)
    // static, animates the open segment (hash 0x303419C9), and drives the second leaf
    // from a mirrored copy of bone 0 so both leaves open symmetrically and meet at the
    // center. The static submesh index differs per model (0x3C163 -> 0, 0x32F3A -> 5).
    void SetupUnchartedMirrorDoor(int propIndex, const FFNA_ModelFile& modelFile,
                                  uint32_t modelFileHash,
                                  const std::vector<Mesh>& meshes,
                                  const std::vector<PerObjectCB>& perObjectCBs,
                                  const std::vector<int>& meshIds,
                                  const std::vector<std::vector<int>>& perMeshTexIds,
                                  PixelShaderType pst,
                                  uint8_t doorType,
                                  size_t staticSubmesh);

    // Uncharted Isle horizontal double-slide gates (0x32F0C, 0x336BB). The .dat only
    // slides the driver panel; the opposite panel's bones are authored fixed/partial.
    // Each (follower, driver) pair drives the follower bone's vertices from a mirrored
    // copy of the driver bone, so the two panels slide apart symmetrically. staticSubmeshes
    // are pinned to the bind pose (frame/pillars). A reflected pure translation preserves
    // winding, so no double-sided rendering is needed. Open segment hash 0x303419C9.
    void SetupUnchartedSlideDoor(int propIndex, const FFNA_ModelFile& modelFile,
                                 uint32_t modelFileHash,
                                 const std::vector<Mesh>& meshes,
                                 const std::vector<PerObjectCB>& perObjectCBs,
                                 const std::vector<int>& meshIds,
                                 const std::vector<std::vector<int>>& perMeshTexIds,
                                 PixelShaderType pst,
                                 uint8_t doorType,
                                 const std::vector<size_t>& staticSubmeshes,
                                 const std::vector<std::pair<uint32_t, uint32_t>>& mirrorFollowerToDriver);

    // Generic door where only part of the mesh animates: a vertex is pinned to the bind
    // pose if it belongs to a static submesh OR is skinned to a static bone; everything
    // else plays the open segment (hash 0x303419C9) normally. Used by Nomad's Isle doors.
    void SetupDoorPartialStatic(int propIndex, const FFNA_ModelFile& modelFile,
                                uint32_t modelFileHash,
                                const std::vector<Mesh>& meshes,
                                const std::vector<PerObjectCB>& perObjectCBs,
                                const std::vector<int>& meshIds,
                                const std::vector<std::vector<int>>& perMeshTexIds,
                                PixelShaderType pst,
                                uint8_t doorType,
                                const std::vector<size_t>& staticSubmeshes,
                                const std::vector<uint32_t>& staticBones,
                                bool lockRootPosition = false,
                                const std::vector<std::pair<uint32_t, uint32_t>>& boneRemap = {});

    // Hinged double-door where the .dat only swings one leaf. staticBones are pinned to
    // the bind pose (pillars/frame); mirrorBones' vertices are driven by a mirrored copy
    // of driverBone (reflected across the door's symmetry plane) so the second leaf swings
    // open symmetrically. The mirror plane is the midpoint of the two leaves' X-centroids,
    // and winding is reversed by the reflection (double-sided). Open segment hash 0x303419C9.
    //
    // driverRemapBones: bones whose vertices are re-skinned ONTO the driver bone so they
    // swing with it. Needed when the visible leaf geometry is rigged to a static bone but a
    // *different* bone carries the correct hinge swing (e.g. Isle of Solitude 0x33CD5, where
    // the panels sit on static bones 0/1 while bone 3 holds the swing). The driver-side
    // centroid used for the mirror plane is then taken from these remapped verts.
    void SetupDoorHingeMirror(int propIndex, const FFNA_ModelFile& modelFile,
                              uint32_t modelFileHash,
                              const std::vector<Mesh>& meshes,
                              const std::vector<PerObjectCB>& perObjectCBs,
                              const std::vector<int>& meshIds,
                              const std::vector<std::vector<int>>& perMeshTexIds,
                              PixelShaderType pst,
                              uint8_t doorType,
                              const std::vector<uint32_t>& staticBones,
                              uint32_t driverBone,
                              const std::vector<uint32_t>& mirrorBones,
                              const std::vector<size_t>& staticSubmeshes = {},
                              const std::vector<uint32_t>& driverRemapBones = {},
                              const std::vector<size_t>& hiddenSubmeshes = {},
                              const std::vector<uint32_t>& unlockedBones = {},
                              uint32_t openSegHash = 0x303419C9,
                              bool lockRoot = false,
                              uint32_t closeSegHash = 0,
                              const std::vector<uint32_t>& ctrlLockedBones = {});

    // Procedural double-hinge door for a broken rig: the two visible panels are rigged
    // to static bones (the .dat never swings them), so instead of borrowing another
    // bone's matrix we synthesize a pure hinge rotation about each panel's own vertical
    // edge, driven by the door's open progress and mirrored between the leaves. leftBones
    // / rightBones select which bones' vertices belong to each leaf; everything in
    // staticSubmeshes or staticBones is pinned. Open segment hash 0x303419C9.
    void SetupDoorProceduralDoubleHinge(int propIndex, const FFNA_ModelFile& modelFile,
                                        uint32_t modelFileHash,
                                        const std::vector<Mesh>& meshes,
                                        const std::vector<PerObjectCB>& perObjectCBs,
                                        const std::vector<int>& meshIds,
                                        const std::vector<std::vector<int>>& perMeshTexIds,
                                        PixelShaderType pst,
                                        uint8_t doorType,
                                        const std::vector<size_t>& staticSubmeshes,
                                        const std::vector<uint32_t>& staticBones,
                                        const std::vector<uint32_t>& leftBones,
                                        const std::vector<uint32_t>& rightBones,
                                        float openAngleDegrees,
                                        DirectX::XMFLOAT3 leftOffset = { 0.f, 0.f, 0.f },
                                        DirectX::XMFLOAT3 rightOffset = { 0.f, 0.f, 0.f });

    void UpdateDoorAnimations();

    // --- ImGui state ---
    bool m_imguiInitialized = false;
    ImGuiContext* m_imguiContext = nullptr;
    bool m_showAgentDataWindow = false;
    int  m_selectedAgentId = -1;
    float m_debugTimeline = 0.f;
    float m_scrubRightClickTime = 0.f;
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
    bool m_modelChangesBuilt   = false;
    bool m_castIntervalsBuilt  = false;
    bool m_skillUseTimelineBuilt = false;
    bool m_knockdownIntervalsBuilt = false;
    bool m_maxHpSolved             = false;
    bool m_maxHpBreakpointSolved   = false;
    bool m_armourSolved            = false;

    // Inputs the forward max-HP model needs from this window: the equipment stream, plus the
    // morale and shrine timelines, which live here rather than in the agent data. Built once and
    // cached, because ResolveMaxHp runs per agent per frame.
    HealthModel::Inputs BuildHealthModelInputs() const;
    HealthModel::Inputs m_healthInputs;
    bool m_healthInputsBuilt = false;

    // Morale as a step function per player, folded once over the match. Both the health model and
    // the morale panel read it, so the rules exist in exactly one place; the panel used to carry
    // its own copy and the two disagreed about which deaths count.
    //
    // Mutable because the panel can draw before the load path reaches the fold, and asking a
    // const accessor to build its own cache is cheaper than ordering the two.
    // Character panels: one player's equipment laid out like the in-game inventory. Several can
    // be open at once because the question is nearly always comparative, so this is a list of
    // independent instances rather than a single visibility flag. `uid` keeps each window's ImGui
    // identity stable while its title follows whichever player it is pointed at.
    struct CharacterPanelInstance
    {
        int  uid     = 0;
        int  agentId = -1;
        bool open    = true;
    };
    std::vector<CharacterPanelInstance> m_characterPanels;
    int  m_nextCharacterPanelUid = 1;
    void OpenCharacterPanel(int agentId = -1);
    void DrawCharacterPanels();

    mutable std::unordered_map<int, std::vector<std::pair<float, int>>> m_moraleTimeline;
    mutable std::unordered_map<int, std::vector<float>> m_moraleDeaths; // by agent, signet backfires removed
    mutable std::unordered_map<int, std::vector<float>> m_moraleBoosts; // by team id
    mutable bool m_moraleTimelineBuilt = false;
    void BuildMoraleTimelines() const;
    int  MoralePercentAtTime(const AgentReplayData& ard, float t) const;

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

    // --- Attribute Model ---
    bool m_attributesDeduced = false;
    std::unordered_map<int, AttributeModel::PlayerBuild> m_attrProfiles;

    // The eight skills the match index lists for a player, which the evidence providers need to
    // ask "which skill on this bar could have paid this out?".
    std::vector<int> SkillBarForAgent(int agentId) const;

    // Writes the whole solve to %TEMP% when GWO_ATTR_DEBUG is set. There is no UI for the
    // evidence yet, and a model nobody can inspect is a model nobody can correct.
    void WriteAttributeDebugDump() const;

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
    // Which item ids are flags at which point in the match. Built before agents
    // are split, since the split depends on telling flags from map bundles.
    FlagItemRegistry m_flagItems;
    FlagTimeline m_flagTimeline;
    bool m_flagTimelineBuilt = false;
    StandOwner m_obeliskLastOwner = StandOwner::Neutral;
    bool m_obeliskOwnerInitialized = false;
    StandOwner m_towerLastOwner = StandOwner::Neutral;
    bool m_towerOwnerInitialized = false;

    void BuildFlagTimeline();
    void DrawFlags();

    // --- Flag event messages (displayed below timer) ---
    struct FlagEventMessage {
        float time = 0.f;
        std::string playerName;
        int playerTeam = 0;    // 0=blue, 1=red
        int flagTeam   = 0;    // 0=blue, 1=red
        FlagTimelineEventType eventType = FlagTimelineEventType::Spawn;
        int standAgentId = -1; // which stand for Stick events (-1 = tower, else obelisk)
        // Repair kits and vine seeds belong to no team, so the flag fields above
        // say nothing about them and the wording comes from this instead.
        BundleType bundleType = BundleType::Unknown;
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

    // Each repaired catapult, tied to the lever gadget standing at it. The map
    // object events only carry an object id, so the gadget is matched by
    // proximity to the player who applied the repair kit.
    struct CatapultLever {
        uint32_t objectId     = 0;
        int      agentId      = -1;
        float    x = 0, y = 0, z = 0;
        float    repairedTime = -1.f;
        std::vector<int> meshIds;   // 3D lever model, hidden until repaired
        bool     visible      = false;
    };
    std::vector<CatapultLever> m_catapultLevers;
    bool m_catapultLeversResolved = false;
    bool m_catapultLeverModelLoaded = false;
    void ResolveCatapultLevers();
    void SetupCatapultLeverProps();
    void UpdateCatapultLeverProps();

    // The catapults themselves (model 0x220F6). One clip holds every pose the
    // machine can be in, from wrecked through to mid-shot, so the prop is parked
    // on whichever segment its object's state calls for and scrubbed by hand
    // instead of being played. Which prop belongs to which object id is only
    // known once the levers resolve, so that link is made later by proximity.
    struct CatapultProp {
        int      animPropIndex = -1;
        DirectX::XMFLOAT3 renderPos{ 0.f, 0.f, 0.f };
        uint32_t objectId      = 0;
        float    repairedTime  = FLT_MAX;
        size_t   segBroken     = SIZE_MAX;
        size_t   segRepairing  = SIZE_MAX;
        size_t   segRepaired   = SIZE_MAX;
        size_t   segLoading    = SIZE_MAX;
        size_t   segFiring     = SIZE_MAX;
    };
    std::vector<CatapultProp> m_catapultProps;
    bool m_catapultPropsResolved = false;
    void ResolveCatapultProps();
    void UpdateCatapultAnimations();

    // --- Door animation state tracking (per door type, not per object) ---
    static constexpr int kDoorTypeCount = 32;
    bool  m_doorTypeOpen[kDoorTypeCount] = {};    // index 1 = IoM doors, index 2 = IoM gate locks, index 3 = Imperial Isle event doors, index 4 = Imperial Isle auto-open doors, index 5/6 = Burning Isle doors (0x1F23F / 0x1F247), index 7 = Uncharted Isle door 0x3C163, index 8 = Uncharted Isle door 0x32F3A, index 9 = Uncharted Isle slide gate 0x32F0C, index 10 = Uncharted Isle slide gate 0x336BB, index 11 = Nomad's Isle doors (0x19750/0x197A9), index 12 = Nomad's Isle looping doors (0x22143), index 13 = Isle of Wurms doors (0x330F7/0x331A4), index 14 = Isle of Jade doors (0x285E7/0x265B5), index 15 = Isle of the Dead doors (0x1F294/0x1F291/0x1F281/0x1E820), index 16 = Isle of Solitude doors (0x33CD5/0x3323B), index 17 = Isle of the Weeping Stone auto-open doors (0x2858E/0x28578), index 18 = Isle of the Weeping Stone event gates (0x1EAFB, ids 147/9305/30563/4417), index 19 = Isle of the Weeping Stone lever door (0x1EAFB nearest the flag stand, object 122), index 20 = Frozen Isle doors (0x1F251/0x1F252), index 21 = Frozen Isle lever gate door 1 (0x255BE, obj 61318, red side), index 22 = Frozen Isle lever gate door 2 (0x255BE, obj 11692, blue side), index 23 = Frozen Isle lever gate door 3 (0x57B57, obj 56526, red side), index 24 = Frozen Isle lever gate door 4 (0x57B57, obj 12669, blue side), index 25 = Corrupted Isle doors (0x330EA/0x32F5C), index 26 = Druid's Isle vine bridge nearest the red lord (obj 39278), index 27 = Druid's Isle vine bridge nearest the blue lord (obj 51238), index 28 = Warrior's Isle base gates (objs 11692/12669/56526/61318, all four open together one minute in)
    float m_doorLastScanTime = -1.f;
    int   m_doorAnimPropCount = 0;

    // Isle of the Weeping Stone lever door: 0x1EAFB is shared by the event gates and the
    // lever door (object 122). Props carry no game object id, so we record every 0x1EAFB
    // animated-prop (index into MapRenderer animated props + world position) at load and,
    // once the flag timeline is known, promote the one closest to the flag stand to the
    // lever door type (open on 0x303419C9, close on 0x31D3EDC8).
    std::vector<std::pair<size_t, DirectX::XMFLOAT3>> m_weepingLeverCandidates;
    bool  m_weepingLeverResolved = false;
    bool  m_weepingLeverHasDoor  = false;               // a lever door was resolved
    DirectX::XMFLOAT3 m_weepingLeverWorldPos{ 0.f, 0.f, 0.f }; // its world position (prop space)

    // Isle of the Weeping Stone portcullis gate (0x285BC): not animated - faded out to
    // fake the opening. Holds the prop indices whose alpha is driven by the timeline.
    std::vector<uint32_t> m_weepingFadeProps;

    // Frozen Isle lever gates (0x255BE = doors 1/2, 0x57B57 = doors 3/4). Each model has
    // two instances that toggle independently from different levers, so each needs its own
    // door type. Props carry no game object id, so we record every instance (animated-prop
    // index + world pos + model hash) at load and, once guild-lord positions are known,
    // assign the one nearest the blue lord to the blue-side object id's door type and the
    // one nearest the red lord to the red-side type (see door_events analysis).
    struct FrozenGateCandidate {
        size_t            animPropIndex;
        DirectX::XMFLOAT3 worldPos;
        uint32_t          modelHash;
    };
    std::vector<FrozenGateCandidate> m_frozenGateCandidates;
    bool m_frozenGatesResolved = false;
    // Resolved (doorType, world pos) per lever gate, for the minimap open/closed state icons.
    std::vector<std::pair<int, DirectX::XMFLOAT3>> m_frozenGateIcons;

    // --- Obelisk Flag Stand 3D model (Isle of Meditation) ---
    int  m_obeliskAnimPropIndex = -1;
    bool m_obeliskModelLoaded   = false;
    std::vector<int> m_obeliskStaticMeshIds;
    int  m_obeliskBannerMeshId  = -1;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_obeliskRedFlagSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_obeliskBlueFlagSRV;
    void SetupObeliskFlagStand();
    void UpdateObeliskFlagStand();

    // --- Tower Flag Stand 3D model (all maps) ---
    int  m_towerAnimPropIndex = -1;
    bool m_towerModelLoaded   = false;
    std::vector<int> m_towerStaticMeshIds;
    int  m_towerBannerMeshId  = -1;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_towerRedFlagSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_towerBlueFlagSRV;
    void SetupTowerFlagStand();
    void UpdateTowerFlagStand();

    // --- Gate Lock animated models (Isle of Meditation) ---
    bool m_gateLockModelsLoaded = false;
    void SetupGateLockProps();

    // --- Gate Lock animated models (Imperial Isle) ---
    bool m_imperialGateLockLoaded = false;
    void SetupImperialGateLockProps();

    // --- Gate lever animated model (Isle of the Weeping Stone) ---
    bool m_weepingLeverModelLoaded = false;
    void SetupWeepingLeverProp();

    // --- Frozen Isle gate lock animated levers (model 0x1E0E1) ---
    // Two lever props are placed at the "Gate lever" gadgets. Each is resolved to a gate
    // door type (21 red side / 22 blue side) by nearest guild lord in ResolveFrozenGates,
    // so the lever plays its open (0x35E6AE29) / close (0x36F05E31) segment in sync with
    // the gate it controls (only submesh 0 animates; bone 1 locked).
    bool m_frozenLeverModelLoaded = false;
    void SetupFrozenGateLockProps();
    std::vector<FrozenGateCandidate> m_frozenLeverCandidates;

    // --- Frozen Isle lever gates: assign each shared-model prop its door type by nearest
    // guild lord (runs once, after guild-lord snapshots are available). ---
    void ResolveFrozenGates();

    // --- Druid's Isle vine bridges (model 0x29FD, two instances). Each grows on its own
    // vine-seed door event, so each needs its own door type. Props carry no game object id,
    // so we record both instances at load and, once guild-lord positions are known, assign
    // the one nearest the red lord to object 39278's type and the other to 51238's. ---
    std::vector<FrozenGateCandidate> m_druidBridgeCandidates;
    bool m_druidBridgesResolved = false;
    void ResolveDruidBridges();
    // Resolved (doorType, game-space pos) per vine bridge, for the minimap
    // grown/not-grown icons. The position is the bridge's gadget rather than the
    // prop, so it is in agent space and needs the map transform when drawn.
    std::vector<std::pair<int, DirectX::XMFLOAT3>> m_druidBridgeIcons;
    // The gadget agents those icons replace, so their dots can be suppressed.
    std::unordered_set<int> m_druidBridgeGadgets;

    void BuildBundleCarryTimeline();
    BundleType GetPlayerBundleType(int agentId, float time) const;
    void DrawBundleItems();
    // Icon for whatever bundle a player is holding, for the single carry slot on
    // the party bars and the focused-player HUD.
    ImTextureID CarriedBundleIcon(ID3D11Device* device, int agentId) const;

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
    int  m_draggingUIElement  = -1;   // -1=none, 0=jumbo, 1=moRed, 2=moBlue, 3=timer

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
    std::string m_folderTag1;
    std::string m_folderTag2;

    // --- Damage / Heal meter (party window bars) ---
    bool m_showDamageMeter = false;
    bool m_showHealMeter   = false;
    bool m_showAbsoluteHp  = true;

    struct MeterEntry { int value = 0; };
    std::unordered_map<int, MeterEntry> m_meterDmg;
    std::unordered_map<int, MeterEntry> m_meterHeal;
    int   m_meterMaxDmg    = 0;
    int   m_meterMaxHeal   = 0;
    int   m_meterTotalDmg  = 0;
    int   m_meterTotalHeal = 0;
    int   m_meterTotalDmgTeam1  = 0;
    int   m_meterTotalDmgTeam2  = 0;
    int   m_meterTotalHealTeam1 = 0;
    int   m_meterTotalHealTeam2 = 0;
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

    // --- Skill laser filters (panel toggled from the ribbon / hotkey) ---
    // Professions are indexed by their in-game id (1..10); slot 0 covers agents
    // with no known profession, so NPC casters stay filterable too.
    static constexpr int kLaserProfCount = 11;
    bool m_showLaserPanel  = false;
    bool m_laserShowRed    = true;
    bool m_laserShowBlue   = true;
    bool m_laserProf[kLaserProfCount] = { true, true, true, true, true, true,
                                          true, true, true, true, true };
    std::unordered_set<int> m_laserHiddenAgents;  // casters whose lasers are hidden

    // True when this caster passes the team / profession / per-agent filters.
    bool LaserCasterVisible(int agentId, const AgentReplayData& ard) const;

    void DrawSkillLasers();
    void DrawSkillLaserPanel();
    void DrawNameFilterPanel();

    ImTextureID m_deathIconTex = nullptr;
    bool m_deathIconLoaded = false;

public:
    // --- Fog of War ---
    int   m_fogPerspective  = 0;     // 0=Off, 1=Red, 2=Blue
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
    bool IsPositionInFog(float x, float y) const;

    // --- Morale Panel ---
    bool m_showMoralePanel = false;
    void DrawMoralePanel();
    int  ComputeAgentMorale(const AgentReplayData& ard, float curTime, int* outDeathCount = nullptr, int* outBoostCount = nullptr) const;

    // --- Max HP Resolver ---
    struct MaxHpSample { uint32_t value = 0; bool estimated = true; };
    MaxHpSample ResolveMaxHp(const AgentReplayData& ard, float t) const;

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
    enum class TimelineEventType { Death, Resurrection, FlagCapture, FlagReturn, MoraleBoost, LordAttacked, Victory, ShrineCaptured, ShrineNeutralized, ObeliskCapture, Catapult };
    struct TimelineEvent {
        float time = 0.f;
        TimelineEventType type = TimelineEventType::Death;
        int agentId = 0;
        int teamId = 0;
        int professionId = 0;
        std::string label;
        // Which of the catapult's states this marker reports, for Catapult events.
        CatapultState catapultState = CatapultState::Unknown;
    };
    struct TimelineData {
        std::vector<float> redHealth;
        std::vector<float> blueHealth;
        std::vector<TimelineEvent> events;
        bool computed = false;
    };
    TimelineData m_timeline;
    bool m_showEventTimeline = false;
    bool m_tlFilterDeath = true;
    bool m_tlFilterRes = true;
    bool m_tlFilterFlag = true;
    bool m_tlFilterMorale = true;
    bool m_tlFilterLord = true;
    bool m_tlFilterVictory = true;
    bool m_tlFilterShrine = true;
    bool m_tlFilterObelisk = true;
    bool m_tlFilterCatapult = true;
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
        uint8_t ownerTeam       = 0; // 0=neutral, 1=red, 2=blue
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

        // Where a weapon would attach on this rig. Resolved once from allClips[0] and cached in
        // animation_cache.ini; unresolved on non-humanoid rigs.
        GW::Animation::WeaponSocket weaponSocket;
    };

    // file hash -> parsed model template (shared geometry, one AddProp per unique model)
    std::unordered_map<uint32_t, AgentModelInstance> m_agentModelTemplates;

    // model slot -> mesh IDs (each slot gets its own AddProp so transforms are independent)
    std::unordered_map<int, std::vector<int>> m_agentMeshIds;

    // model slot -> file hash (cached so we don't re-lookup every frame)
    std::unordered_map<int, uint32_t> m_agentFileHashCache;

    // Per-frame diagnostic: why each model slot was shown/hidden. Also the gate the skinned
    // and weapon passes read, so a slot that must not draw has to be written here every frame.
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

        // Snapshot index DrawAgentModels resolved for this agent this frame. DrawWeaponModels
        // runs straight after it and reuses the result rather than repeating the binary search.
        int   lastSnapIdx = -1;
    };
    std::unordered_map<int, AgentAnimState> m_agentAnimStates;
    void DrawSkinnedAgentModels();

    // Top of the model an agent is wearing at `time`, for overlays that anchor above the head.
    float AgentModelTopY(int agentId, const AgentReplayData& ard, float groundY, float time) const;

    // --- Weapon models ---
    // Design notes: gwobserver-private/docs/WeaponModelRendering.md

    // The equipped item in one hand at one instant. Resolved from the agent snapshot, which
    // carries the item ids directly and joins against the recorded equipment stream with full
    // coverage; the EQUIP_SET/EQUIP_CLEAR walk is only a fallback for agents that have none.
    struct EquippedWeapon {
        const Equipment::ItemDef* item = nullptr;
        uint32_t modelFileId = 0;
        uint32_t itemType    = 0;
        bool     fromEventStream = false;  // true => came from EQUIP_SET, not the snapshot

        bool Valid() const { return item != nullptr && modelFileId != 0; }
    };

    EquippedWeapon ResolveEquippedWeapon(const AgentReplayData& ard, int snapIdx, uint8_t slot) const;

    // Equipment::Data::FindAtTime scans the whole event list, which is fine for a tooltip but
    // not for every agent and slot every frame. This indexes the same events by (agent, slot)
    // so the per-frame path is a binary search. Built lazily on first use.
    using EquipSlotKey = uint64_t;  // agentId << 8 | slot
    mutable std::unordered_map<EquipSlotKey, std::vector<std::pair<float, int>>> m_equipSlotTimeline;
    mutable bool m_equipSlotTimelineBuilt = false;
    void BuildEquipSlotTimeline() const;

    // One GPU resource set per distinct model_file_id, drawn once per (agent, slot) with its own
    // PerObjectCB. Weapons attach rigidly, so they need no bone palette and must not be
    // duplicated per agent the way character models are.
    struct WeaponModelTemplate {
        // Produced on the IO thread.
        std::vector<Mesh> meshes;
        std::vector<ParsedTextureEntry> parsedTextures_;
        PixelShaderType pixelShaderType = PixelShaderType::OldModel;
        bool texturesOk = false;

        // Produced on the main thread from the above, which is then released.
        std::vector<std::shared_ptr<MeshInstance>> gpuMeshes;
        std::vector<PerObjectCB> templateCBs;
        bool gpuReady = false;
    };

    std::unordered_map<uint32_t, WeaponModelTemplate> m_weaponModelTemplates;
    std::vector<uint32_t>  m_weaponModelOrder;
    int                    m_weaponModelCreateIndex = 0;
    std::thread            m_weaponModelLoadThread;
    std::atomic<bool>      m_weaponBgLoadDone{ false };
    bool                   m_weaponModelsLoading = false;
    bool                   m_weaponModelsLoaded  = false;

    // Every model_file_id the match ever puts in a hand. Main thread only — it reads the
    // equipment stream and the agent snapshots.
    void CollectHandHeldModelFileIds(std::vector<uint32_t>& out) const;

    void LoadWeaponModelsAsync();
    void LoadWeaponModelsIO();
    void StepCreateWeaponModelResources();
    void ProgressiveWeaponModelPump();
    void DrawWeaponModels();

    bool m_showWeaponModels = true;

    // How one class of weapon attaches. Keyed by item_type, because which hand a weapon uses is
    // a property of the weapon and not of the slot it occupies: a bow is a slot-0 main-hand
    // weapon that an archer holds in the *off* hand and draws with the other.
    //
    // Rotation and scale came out identity for a sword in Phase 1 — GW authors weapon models
    // with their local origin at the grip and their axes aligned to the hand bone's frame — so
    // the correction is normally just a translation seating the hilt in the fist. They are kept
    // per type because long two-handed weapons are not guaranteed to share that.
    struct WeaponGrip {
        bool              forceOffHand = false;
        // Dual-wielded: one item record, one skin, but a blade in each hand. The off-hand slot
        // stays empty for these, so the main-hand model is drawn a second time on the other bone.
        bool              mirrorToOffHand = false;
        DirectX::XMFLOAT3 offset   { -1.f, -3.f, 0.f };
        DirectX::XMFLOAT3 rotation { 0.f, 0.f, 0.f };   // degrees, pitch/yaw/roll
        float             scale = 1.f;
    };

    WeaponGrip m_weaponGripDefault;
    std::unordered_map<uint32_t, WeaponGrip> m_weaponGrips;   // item_type -> grip
#if GWO_DEVELOPER
    uint32_t   m_weaponGripEditType = 27;                     // type shown in the editor
#endif

    void SeedWeaponGrips();
    const WeaponGrip& GripFor(uint32_t itemType) const;

    // The urn this agent is holding at the given moment, or nullptr. Ashes replace
    // whatever the agent was carrying, so this is asked before the equipment slots.
    const struct AshesSkill* AshesHeldAt(int agentId, float time) const;

    // Which bind-pose side holds the main-hand weapon. The two hand bones are mirror images, so
    // this cannot be derived from the rig. Settled visually in Phase 1: +x is the weapon hand,
    // confirmed by the shield landing in the off hand.
    bool m_weaponHandIsPositiveX = true;

#if GWO_DEVELOPER
    // Developer-only diagnostics. Compiled out entirely of public builds — the panel exists to
    // calibrate attachment against the render, which is not something a released build needs.
    bool m_showWeaponSocketWindow = false;
    void DrawWeaponSocketWindow();

    // Term-by-term view of how each player's maximum health is arrived at, so a disagreement with
    // reality can be traced to the term that caused it rather than guessed at.
    bool m_showHealthModelWindow = false;
    void DrawHealthModelWindow();

    struct WeaponPreloadStats {
        int  distinctModels = 0;
        int  resolvedModels = 0;
        int  parsedOk       = 0;
        double parseSeconds = 0.0;
        bool ran = false;
    };
    WeaponPreloadStats m_weaponPreloadStats;

    // Main thread only. This model loader runs on m_agentModelLoadThread, which is started as
    // soon as agents are classified — before StoC parsing has necessarily published
    // m_replayCtx.stocData. Reading the equipment stream from that thread races the move-assign
    // in PollStoCParseCompletion, so the measurement is taken on demand instead.
    void ProbeWeaponModels();
#endif

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

    // Guild cape display on loading screen
    GuildCapeCache m_capeCache;
    bool  m_capeCacheInitialized = false;
    ImTextureID m_capeTexTeam1 = nullptr;
    ImTextureID m_capeTexTeam2 = nullptr;
    bool  m_capeTexturesResolved = false;
    void  InitCapeCache();
    void  ResolveCapeTextures();
    void  DrawMatchInfoOverlay(ImDrawList* dl, ImVec2 display, float alpha);

    // Post-loading overlay fade (guild cards persist into replay, synced to replay time)
    float m_matchOverlayStartTime = 0.f;
    bool  m_matchOverlayActive = false;

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

    // --- Ribbon toolbar (collapsible strip docked to the top edge) ---
    bool  m_ribbonPinned    = false;            // persisted in ui_layout.json
    float m_ribbonReveal    = 0.f;              // 0 = collapsed, 1 = expanded
    float m_ribbonIdleTimer = 1e9f;             // starts collapsed until revealed
    bool  m_ribbonMoreOpen  = false;            // overflow popup holds it open

    // Bottom edge of the strip in screen space, republished every frame by
    // DrawRibbonToolbar() (equal to the work-area top when the ribbon is
    // closed). Top-anchored HUD elements read this so they can sit under the
    // strip and follow its collapse animation instead of being overlapped.
    float m_ribbonBottomY   = 0.f;
    // Top edge of the event timeline strip, republished every frame (viewport bottom when the
    // strip is down). Screen-anchored HUDs read it to stay clear of it, the way the followed
    // agent's health bar rides m_ribbonBottomY at the top of the screen.
    // FLT_MAX until the first publish, so a reader that runs before it sees "no obstruction"
    // rather than an obstruction pinned to the top of the screen.
    float m_eventTimelineTopY = FLT_MAX;

    // Extra drop, in pixels, applied to the followed agent's HUD so it clears the
    // drawing strip when that has been parked over it. Tweened rather than snapped,
    // so the bar slides the way it already does under the ribbon's collapse
    // animation. m_followedHudLastFrame lets the first frame of a newly shown HUD
    // start settled instead of sliding into place.
    float m_followedHudDropY     = 0.f;
    int   m_followedHudLastFrame = -1;

    void DrawRibbonToolbar();

    // --- Minimap ---
    bool  m_minimapEnabled       = false;
    float m_minimapZoom          = 1.0f;     // 1.0 = full map, >1 = zoomed in
    float m_minimapPanX          = 0.f;      // world-unit pan offset
    float m_minimapPanZ          = 0.f;
    bool  m_minimapShowLabels    = true;
    bool  m_minimapShowProfession = false;
    bool  m_minimapCursorActive  = false;   // true while a software cursor is drawn over the minimap

    // Resurrection Shrine -> team (1=red, 2=blue) attribution, computed once at
    // map load by nearest guild lord. Shrines are static so this never changes.
    std::unordered_map<int, int> m_resShrineTeam;
    bool  m_resShrineTeamComputed = false;
    int   m_minimapWidth         = 400;
    int   m_minimapHeight        = 400;
    DirectX::XMFLOAT4X4 m_minimapViewProj{};

    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_minimapTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_minimapRTV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_minimapDepthTexture;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView>   m_minimapDSV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_minimapSRV;
    bool m_minimapResourcesReady = false;

    void InitMinimapResources();
    void RenderMinimap();
    void DrawMinimapPanel();

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
    // The focused-player HUD follows the camera while the panel follows whatever was clicked,
    // so the two need separate caches or they rebuild each other's sets every frame.
    PlayerWeaponSets m_hudWeaponSets;

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
    void BuildWeaponSets(int agentId, PlayerWeaponSets& out) const;
    // Item tooltip for one set — shared with the focused-player HUD so both read identically.
    void DrawWeaponSetTooltip(const WeaponSetEntry& ws);
    std::vector<PipSkillStat> BuildSkillStats(int agentId, float currentTime) const;

    // --- Focused-player HUD (ReplayWindow_FocusedPlayerHud.cpp) ---
    // Screen-space overlay shown only while the camera follows a player, alongside
    // DrawFollowedAgentHUD's health/cast bars. Weapon sets are the first element.
    bool m_showFocusHud = true;
    void DrawFocusedPlayerHud();
    void DrawFocusHudWeaponSets(int agentId);

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

    // --- Shout speech bubbles ---
    struct SpeechBubble {
        int   agentId   = -1;
        int   skillId   = 0;
        std::string text;
        float spawnTime = 0.f;
    };
    static constexpr float kSpeechBubbleLifetime = 1.5f;
    std::unordered_map<int, SpeechBubble> m_speechBubbles;   // keyed by agentId (replaces on new shout)
    std::unordered_map<int, size_t> m_shoutScanCursor;       // per-agent cursor into skillUseHistory
    float m_lastShoutScanTime = -1.f;
    void  UpdateSpeechBubbles();
    void  RenderSpeechBubbles();

    // --- Spatial Audio ---
    // Background/UI/Music/Dialog/attack-hits play back from StoC/sound_events.txt directly: real
    // captured file_id/position/timing. Skill-cast (Effects/SkillCue) sounds are synthesized from
    // skillUseHistory + settings/skill_sounds.json instead, since the recording client only wrote
    // a sound_events.txt line for skills that happened to be within earshot of the *recording*
    // camera - most skill casts elsewhere on the map were never captured at all. See
    // ReplayWindow_Audio.cpp (UpdateAudioPlayback / BuildSkillSoundTimeline).
    std::unique_ptr<SpatialAudioEngine> m_audioEngine;
    std::unique_ptr<SkillSoundTable>    m_skillSoundTable;
    bool m_audioInitialized = false;
    // Sound is on unless muted. This is purely the mute state now - it used to double as the
    // Sound FX panel's visibility, which meant you could not look at the mixer without the audio
    // following it. Muting lives on the play bar's speaker; the panel has its own flag below.
    bool m_audioEnabled     = true;
    bool m_showSoundFxPanel = false;   // ribbon "Sound FX" button / File menu entry
    // Keeps the play bar's volume slider up while it is being dragged, even once the pointer has
    // left the hover zone.
    bool m_volumeBarActive  = false;
    float m_audioLastTime = -1.f;
    bool m_audioWasPlaying = true;     // last-seen m_replayCtx.isPlaying, to detect pause/resume

    // Cursor into stocData.soundEvents (time-ordered).
    size_t m_audioSoundEventCursor = 0;

    // One scheduled skill-cast sound: a single layer of a single cast, already resolved to an
    // absolute replay time and to the agent whose position it is emitted from.
    struct ScheduledSkillSound {
        float    time    = 0.f;   // absolute replay time (cast start + the cue's own offset)
        uint32_t fileId  = 0;
        int      agentId = 0;     // caster or target, whichever this cue is emitted from
        int      casterId = 0;    // kept so "is this the followed agent's sound?" can be asked at
        int      targetId = 0;    // playback time - the followed agent changes while playing
        SoundLogCategory category{};   // which per-category budget/volume it plays under
        float    gain    = 1.0f;       // ducking for stacked simultaneous layers of one cast
    };

    // Every skill sound for the whole match, flattened and sorted by time, so playback is the
    // same single-cursor walk the StoC sound events use (and a seek is one binary search).
    // Built lazily once skillUseHistory exists.
    bool m_audioSkillTimelineBuilt = false;
    std::vector<ScheduledSkillSound> m_audioSkillTimeline;
    size_t m_audioSkillCursor = 0;

    // One-off analysis of this match's StoC sound events, built lazily:
    //  - which file ids are demonstrably attack sounds, so the ~70% of them the recorder failed
    //    to correlate to an agent stop being misfiled as footsteps
    //  - a per-event gain that ducks simultaneous layers and drops exact duplicates, the same
    //    treatment the skill cues get
    bool m_audioStocAnalysisBuilt = false;
    std::unordered_set<uint32_t> m_audioAttackSoundIds;
    std::vector<float> m_audioStocGain;
    void BuildStocSoundAnalysis();

    void InitAudioEngine();
    void UpdateAudioPlayback(float currentTime, float dt);
    void BuildSkillSoundTimeline();

    // --- Heatmap system ---
    HeatmapSettings       m_heatmapSettings;
    HeatmapAccumulator    m_heatmapAccumulator;
    HeatmapRenderer       m_heatmapRenderer;
    bool                  m_heatmapInitialized = false;
    std::unordered_map<int, uint8_t> m_agentTeams;
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
