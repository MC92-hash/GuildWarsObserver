#pragma once
//
// ReplayWindow_Internal.h
// -----------------------------------------------------------------------------
// Private shared helpers for the ReplayWindow partial-class split.
//
// ReplayWindow.cpp was split into per-feature translation units
// (ReplayWindow_<Feature>.cpp). Several small free helper functions in the
// original file had internal linkage (file-scope `static`) yet are called from
// more than one feature. To let those features live in separate TUs, each such
// helper's single definition had its `static` removed (external linkage) and is
// declared here. Feature TUs (and the core ReplayWindow.cpp) include this header.
//
// NOTE: include this AFTER "pch.h" and "ReplayWindow.h" — the declarations below
// rely on types from ReplayMapData.h / DirectX / Direct3D / ImGui.
//

// --- Shared enums (originally file-scope in ReplayWindow.cpp) ---
enum class AgentIconState { Alive, Dead, Knockdown };

// --- Agent position / map-space transform (shared: FogOfWar, AgentModels,
//     AgentOverlay, Props, BundleCarry, ...) ---
DirectX::XMFLOAT3 ApplyMapTransformToPos(float snapX, float snapY, float snapZ,
                                         const MapTransform& t);

void InterpolateAgentPosition(const AgentReplayData& ard, float t,
                              const InterpolationSettings& is,
                              float& outX, float& outY, float& outZ);

const AgentSnapshot* FindSnapshotAtTime(const AgentReplayData& ard, float t);
ImU32 GetAgentTeamColor(uint8_t teamId);
void SnapAgentPosition(const AgentReplayData& ard, float t, float& outX, float& outY, float& outZ);
std::string GetAgentLabel(const AgentReplayData& ard);

// --- Icon / texture loaders (shared: CombatLog, LordDamage, PartyWindows,
//     PlayerInfo, SkillAnalytics, EventTimeline, ...) ---
ImTextureID LoadProfIcon(ID3D11Device* device, int profId);
ImTextureID LoadFlagIcon(ID3D11Device* device, const char* filename);
ImTextureID LoadPartyIcon(ID3D11Device* device, const char* filename);
ImTextureID LoadEffectIcon(ID3D11Device* device, const char* filename);
ImTextureID LoadSkillIcon(ReplayWindow* rw, ID3D11Device* device, int skillId,
                          std::unordered_map<int, std::string>& index,
                          std::unordered_map<int, ComPtr<ID3D11ShaderResourceView>>& cache);

// --- Skill / message text (shared: CombatLog, EventTimeline, ...) ---
std::string GetSkillDisplayName(int skillId);
const char* JumboMessageDisplayText(const std::string& msgType, int team);

// --- Formatting / name / category helpers (shared across panels) ---
void FormatMMSS(char* buf, size_t bufSz, float seconds);
void FormatTime(float seconds, char* buf, size_t bufSize);
std::string GetAgentDisplayName(const ReplayContext& ctx, int agentId);
const GuildMeta* FindGuildByTagStatic(const MatchMeta& m, const std::string& tag);
const char* GetDaggerStatusName(uint8_t ds);
const char* GetTeamName(uint8_t tid);
const char* GetWeaponTypeName(uint16_t wt);
const char* JumboPartyLabel(int partyValue);
ImU32 PianoRollSkillColor(int skillType, bool bright);
int ResolveTarget(int targetId, int casterId);
ImU32 StoCCategoryColor(StoCCategory cat);
int StoCCategoryCount(const StoCData& d, StoCCategory cat);

// --- Screen projection (shared: EventTimeline, Flags, overlays, ...) ---
bool ProjectToScreen(XMMATRIX viewProj, float vpW, float vpH,
                     const XMFLOAT3& worldPos, float& scrX, float& scrY);

// --- Flag naming (shared: Flags, EventTimeline) ---
const char* FlagLocationName(FlagLocation loc);
const char* FlagEventTypeName(FlagTimelineEventType t);
const char* StandOwnerName(StandOwner o);

// --- NPC icon loader ---
ImTextureID LoadNPCIcon(ID3D11Device* device, const char* filename);

// --- Settings file paths (shared: core + Heatmap/Audio feature TUs) ---
std::filesystem::path GetHeatmapSettingsPath();
std::filesystem::path GetSkillSoundsFilePath();

// --- Party window shared type + helpers (shared: PartyWindows, FollowedAgentHUD) ---
struct PartyIcons {
    ImTextureID weaponSpell = nullptr;
    ImTextureID enchanted   = nullptr;
    ImTextureID condition   = nullptr;
    ImTextureID hexed       = nullptr;
};
PartyIcons LoadAllPartyIcons(ID3D11Device* dev);
void DrawPartyHealthBar(ImDrawList* dl, ImVec2 barTL, float barW, float barH,
                        const AgentSnapshot* snap, uint8_t teamId, bool isDead,
                        const char* name, const PartyIcons& icons,
                        int followedAgentId, int agentId, bool fogHidden = false,
                        ImTextureID flagTex = nullptr, uint32_t absMaxHp = 0, bool hpEstimated = false);
void DrawMeterBar(ImDrawList* dl, ImVec2 healthTL, float healthW, float slotY,
                  float barH, int value, int maxValue, int totalValue,
                  bool leftSide, float maxBarW, ImU32 barColor);
void DrawMeterSumText(ImDrawList* dl, float anchorX, float anchorY, float rowH,
                      int dmgSum, int healSum, bool showDmg, bool showHeal, bool leftSide);

// --- Gradient infrastructure (shared: PlayerInfo, AgentOverlay, Effects) ---
struct GradStop { float pos; uint8_t r, g, b; };
struct Gradient5 { ImU32 c[5]; };
inline constexpr Gradient5 kAliveBlue   = {{ IM_COL32(0x4A,0x6B,0xA3,0xFF), IM_COL32(0x3D,0x5F,0x98,0xFF), IM_COL32(0x30,0x5A,0x90,0xFF), IM_COL32(0x29,0x4E,0x7A,0xFF), IM_COL32(0x22,0x42,0x65,0xFF) }};
inline constexpr Gradient5 kDeadBlue    = {{ IM_COL32(0x3A,0x4A,0x66,0xFF), IM_COL32(0x33,0x42,0x59,0xFF), IM_COL32(0x2C,0x3A,0x4D,0xFF), IM_COL32(0x25,0x32,0x40,0xFF), IM_COL32(0x1E,0x29,0x33,0xFF) }};
inline constexpr Gradient5 kAliveRed    = {{ IM_COL32(0xCE,0x0C,0x0C,0xFF), IM_COL32(0xD6,0x34,0x34,0xFF), IM_COL32(0xD9,0x43,0x43,0xFF), IM_COL32(0xB2,0x00,0x00,0xFF), IM_COL32(0x7E,0x00,0x00,0xFF) }};
inline constexpr Gradient5 kDeadRed     = {{ IM_COL32(0x47,0x1C,0x17,0xFF), IM_COL32(0x53,0x24,0x1B,0xFF), IM_COL32(0x52,0x24,0x1C,0xFF), IM_COL32(0x3C,0x19,0x14,0xFF), IM_COL32(0x2F,0x13,0x0F,0xFF) }};
inline constexpr Gradient5 kDegenHex    = {{ IM_COL32(0xC9,0x47,0x9E,0xFF), IM_COL32(0xCE,0x5A,0xA8,0xFF), IM_COL32(0xD3,0x6C,0xB1,0xFF), IM_COL32(0xB5,0x33,0x8A,0xFF), IM_COL32(0x86,0x26,0x66,0xFF) }};
inline constexpr Gradient5 kPoison      = {{ IM_COL32(0x7F,0x7F,0x3F,0xFF), IM_COL32(0x8B,0x8B,0x50,0xFF), IM_COL32(0x94,0x94,0x5F,0xFF), IM_COL32(0x66,0x66,0x2B,0xFF), IM_COL32(0x4E,0x4E,0x1D,0xFF) }};
inline constexpr Gradient5 kBleeding    = {{ IM_COL32(0xDF,0x71,0x70,0xFF), IM_COL32(0xE1,0x7B,0x7B,0xFF), IM_COL32(0xE2,0x7E,0x7E,0xFF), IM_COL32(0xBC,0x59,0x59,0xFF), IM_COL32(0xA9,0x4F,0x50,0xFF) }};
inline constexpr Gradient5 kDeepWound   = {{ IM_COL32(0x92,0x92,0x92,0xFF), IM_COL32(0x9F,0x9F,0x9F,0xFF), IM_COL32(0xAB,0xAB,0xAB,0xFF), IM_COL32(0x84,0x84,0x84,0xFF), IM_COL32(0x73,0x73,0x73,0xFF) }};
void DrawGradientRect(ImDrawList* dl, ImVec2 tl, ImVec2 br, const Gradient5& g);
void SampleGradient(const GradStop* stops, int n, float t, float& outR, float& outG, float& outB);

// --- Effects / HUD shared helpers ---
std::filesystem::path FindTexturesDDSDir();
ImTextureID LoadSpeechBubbleTexture(ID3D11Device* device);
std::string StripPvpSuffix(const std::string& name);

// --- UI asset base paths / loaders (shared: Preferences, PlayerInfo, AgentOverlay, LoadingScreen, AgentModels) ---
std::filesystem::path GetEffectsBasePath();
std::filesystem::path GetGameUIBasePath();
std::filesystem::path GetNPCIconBasePath();
std::filesystem::path GetSkillIconsBasePath();
std::filesystem::path GetWeaponsBasePath();
std::filesystem::path GetOthersUIBasePath();
std::filesystem::path GetProfIconsBasePath();
std::filesystem::path GetProfStylizedBasePath();
std::filesystem::path GetUILayoutFilePath();
std::string GetReplayFontBasePath();
std::string GetSvgIconBasePath();
const char* GetShieldTexture(int primaryProf);
const char* GetSpearTexture(int primaryProf);
const char* WeaponTypeName(uint8_t t);
const char* ProfIconFileName(int profId);
ImTextureID LoadProfIconTimeline(ID3D11Device* device, int profId);
ImTextureID LoadSkillIconFile(ID3D11Device* device, const char* filename);
ImTextureID LoadSvgIcon(ID3D11Device* device, const char* filename, int rasterSize = 64);
ImTextureID LoadWeaponTexture(ID3D11Device* device, const char* filename);
ImTextureID LoadProfIconCL(ID3D11Device* device, int profId);
ImTextureID LoadProfIconGeneric(ID3D11Device* device, const std::filesystem::path& basePath, const std::string& key);
ImTextureID LoadProfStylized(ID3D11Device* device, int profId, int teamId, AgentIconState state);
ImTextureID LoadSkillDescIcon(ID3D11Device* device, const char* filename);
int FindSnapshotIndex(const std::vector<AgentSnapshot>& snaps, float t);
int FindMoveEventIndex(const std::vector<MoveToPointEvent>& moves, float t);
float LerpGradChannel(float a, float b, float t);
void SaveMapTransform(int mapId, const MapTransform& t);
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> BuildGradientTex1xN(ID3D11Device* device, int height, const GradStop* stops, int nStops);
Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> BuildCastBarFillTex2D(ID3D11Device* device, int width, int height, const GradStop* hStops, int nH, float topBlackAlpha, float botBlackAlpha);

// --- Weapon texture resolution (shared: PlayerInfo, AgentOverlay) ---
struct WeaponTextureResult {
    const char* mainTex    = nullptr;
    const char* offTex     = nullptr;
    bool        isShield   = false;
    bool        isFlag     = false;
    bool        isNPCIcon  = false;
    BundleType  bundleType = BundleType::Unknown;
};
WeaponTextureResult ResolveWeaponTextures(uint16_t weapType, uint8_t weapItemType,
                                          int primaryProf, int teamId,
                                          BundleType bundleType = BundleType::Unknown);
