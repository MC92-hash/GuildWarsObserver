#pragma once

// =================================================================================================
// A LIVE SOURCE: A MATCH THAT IS BEING WRITTEN WHILE IT IS WATCHED
//
// A ReplayWindow normally plays a recording. Opened with ReplayWindow::CreateLive it plays a match
// that a simulation writes into its ReplayContext as it goes: the same agent snapshots and the same
// StoC event streams a recording carries, appended in time order. Everything downstream of those
// (cast bars, the party window, floaters, skill effects, sounds, animations) reads them unchanged.
//
// The window owns the clock only in so far as it asks the source what time to show. The source owns
// the simulation, its tick, its input and its rules; the public tree only knows this interface.
// =================================================================================================

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ReplayContext;
struct MatchMeta;

// What the window hands the source each frame.
struct LiveFrameInput
{
    double realDt = 0.0;            // seconds since the last frame, unclamped
    bool keyboardAllowed = false;   // our process is in front and no text field has the keyboard
    int clickedAgentId = -1;        // a left click on a character or a party row this frame, else -1
    bool cameraOrbiting = false;    // the right mouse button is held (the user steers the camera)
    // The camera's forward, as a facing (radians, game coordinates). While the camera orbits, the
    // player turns with it, as GW's right-drag mouse look does.
    float cameraFacing = 0.f;
    float cameraYaw = 0.f;          // the chase camera's own yaw (the self-test checks a click leaves it)
    // A left click on the ground (not on a character or a panel) this frame: the point under the
    // cursor, game (x, y). While the button stays down the point under the cursor follows (held).
    bool groundClicked = false;
    float groundX = 0.f, groundY = 0.f;
    bool groundHeld = false;
    float heldX = 0.f, heldY = 0.f;
    int clickedSkillSlot = -1;      // a click on a slot of the player's skill bar (0..7), else -1
    bool setBar = false;            // the skills window rearranged the bar: the bar it asks for
    int bar[8] = {};
};

// One skill the skills window lists, and whether the source can run it.
struct LiveSkillOption
{
    int skill = 0;
    bool simulated = false;
};

// A step of the source's self-test that the window performs with its own mouse: injected into the
// window's ImGui context, so the click goes through the same code as a real one. Nothing reaches the OS.
struct LiveScriptedUi
{
    // ClickBody: a click on the screen point of a character's body, (x, y) = its feet, game
    // coordinates. ReportOverheads: the window reports the overhead bars it draws next frame.
    enum class Kind { None, GroundClick, SkillSlotClick, DragSkillToSlot, ClickBody, ReportOverheads };
    Kind kind = Kind::None;
    float x = 0.f, y = 0.f;   // GroundClick: the point, game coordinates
    int slot = -1;            // SkillSlotClick, DragSkillToSlot: the bar slot
    int skill = 0;            // DragSkillToSlot: the skill dragged from the skills window's list
};

// What a step appended, so the window refreshes only the caches that read it.
struct LiveAppended
{
    bool snapshots = false;   // agent snapshots
    bool skills = false;      // skillUseHistory / castHistory / stocData.skill
    bool combat = false;      // stocData.combat (DAMAGE / HEAL)
    bool visuals = false;     // stocData.visualEffects
    bool Any() const { return snapshots || skills || combat || visuals; }
};

// What the map offers the source when it places its agents.
struct LiveWorld
{
    float centreX = 0.f, centreY = 0.f;   // the map bounds' centre, game coordinates
    // Game (x, y) -> the snapshot z of the ground there (negative up). False off the terrain.
    std::function<bool(float x, float y, float& z)> groundZ;
};

class ILiveSource
{
public:
    virtual ~ILiveSource() = default;

    // Once, when the map is loaded: write every agent (one snapshot each), the match metadata the
    // classification reads (parties, guilds, bars) and empty event streams.
    virtual void Seed(ReplayContext& ctx, MatchMeta& meta, const LiveWorld& world) = 0;

    // Once a frame while the window is Ready: run as many fixed ticks as the frame's time allows and
    // append what they produced to ctx.
    virtual void Advance(const LiveFrameInput& in, ReplayContext& ctx) = 0;

    // The time the window shows this frame (seconds). Never later than the newest appended data.
    virtual double DisplayTime() const = 0;
    // The latest tick's time; the window's maxReplayTime.
    virtual double SimTime() const = 0;

    // What the last Advance appended (cleared by the call).
    virtual LiveAppended TakeAppended() = 0;

    virtual int PlayerAgentId() const = 0;
    virtual int TargetAgentId() const = 0;
    virtual void SetTarget(int agentId) = 0;

    // The chase camera eases behind the player's facing while this is true.
    virtual bool PlayerSteering() const = 0;
    virtual float PlayerFacing() const = 0;   // radians, game coordinates (cos, sin) = forward

    // Drawn inside the live window's ImGui frame, after its own panels.
    virtual void DrawHud() {}

    // Each presented frame, with its duration (ms).
    virtual void OnFrame(double /*frameMs*/) {}

    // A PNG path the window should write the frame it just rendered to (empty = none).
    virtual std::wstring TakeCaptureRequest() { return {}; }
    // A party row the source asks the window to click (the self-test), -1 = none. Row 0 = first.
    virtual int TakeScriptedPartyRowClick() { return -1; }
    // A click or drag the source asks the window to perform with its mouse (the self-test).
    virtual LiveScriptedUi TakeScriptedUi() { return {}; }

    // The player's bar in slot order (0 = empty slot); false when the source has none.
    virtual bool PlayerBar(int (&bar)[8]) const { (void)bar; return false; }
    virtual bool PlayerCasting() const { return false; }
    // The skills the skills window offers.
    virtual void SkillOptions(std::vector<LiveSkillOption>& out) const { out.clear(); }
    // The skills window key went down since the last call.
    virtual bool TakeSkillsWindowToggle() { return false; }

    // A line of the window's own report (the models it built, the animations they carry).
    virtual void Report(const std::string& /*line*/) {}

    // The window is going away.
    virtual void OnWindowClosed() {}
};

// The Trials page asks the application for a live window through this; MapBrowser opens and closes
// it. At most one live window exists.
struct PendingLiveReplay
{
    bool openRequested = false;
    bool closeRequested = false;
    std::shared_ptr<ILiveSource> source;
    int mapId = 0;
    bool offscreen = false;   // the self-test: off every monitor, visible, never activated
};
inline PendingLiveReplay g_pendingLiveReplay;
// A live window exists (MapBrowser keeps it current): the frame limit lets it run at 120 or more.
inline bool g_liveReplayOpen = false;
