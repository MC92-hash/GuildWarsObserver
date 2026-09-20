#pragma once
#include "build_config.h"
#include "imgui.h"
#include <fstream>
#include <string>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <system_error>

class GuiGlobalConstants
{
public:
	inline static bool settings_loaded = false;

	// Window-visibility flags for the two private character panels. Inert in the Observer, which
	// has no menu entry for either and never opens them; they exist so that the one shared copy of
	// those panels compiles against this class in both applications.
	inline static bool is_character_composer_open = false;
	inline static bool is_wardrobe_open = false;

	// Persistent gw.dat path (saved across sessions)
	inline static std::string saved_gw_dat_path;

	// Persistent match data folder path (saved across sessions)
	inline static std::string saved_match_data_folder_path;

	// Cloud storage settings
	inline static std::string storage_mode = GWO_CLOUD_ENABLED ? "online_only" : "local";
	inline static std::string cloud_storage_host = GWO_CLOUD_HOST;
	inline static std::string r2_endpoint = GWO_R2_ENDPOINT;
	inline static std::string r2_bucket = GWO_R2_BUCKET;
	inline static std::string r2_read_access_key = GWO_R2_READ_ACCESS_KEY;
	inline static std::string r2_read_secret_key = GWO_R2_READ_SECRET_KEY;

	// Contributor key for build naming (empty = read-only)
	inline static std::string contributor_key;

	// Developer mode. GWO_DEVELOPER turns it on at compile time; setting GWO_DEV
	// in the environment turns it on at runtime, so the team can reach the Debug
	// menu in the exact binary that ships instead of needing a separate build.
	// Public builds are compiled with GWO_DEVELOPER 0 and stay clean unless
	// somebody deliberately sets the variable.
	//
	// Note this only governs the runtime-gated UI. Blocks written as
	// #if GWO_DEVELOPER (the weapon diagnostics) are compiled out of a public
	// build entirely and the variable will not bring them back.
	static bool IsDeveloperMode()
	{
		if constexpr (GWO_DEVELOPER != 0)
			return true;

		static const bool enabled = []
		{
			char v[8] = {};
			return GetEnvironmentVariableA("GWO_DEV", v, (DWORD)sizeof(v)) != 0;
		}();
		return enabled;
	}

	// Validate a contributor key against known hashes (returns true if valid)
	static bool ValidateContributorKey(const std::string& key);

	// Some ImGui layout vars:
	inline static const int left_panel_width = 450;
	inline static const int right_panel_width = 450;
	inline static const float panel_padding = 6.0f;
	inline static const float menu_bar_height = 20.0f; // Height of the main menu bar

	inline static bool hide_all = false;
	inline static bool is_dat_browser_open = true;
	inline static bool is_dat_browser_resizeable = false;
	inline static bool is_dat_browser_movable = false;
	inline static bool is_left_panel_open = false;
	inline static bool is_right_panel_open = true;
	inline static bool is_hex_editor_open = false;
	inline static bool is_text_panel_open = false;
	inline static bool is_audio_controller_open = false;
	inline static bool is_texture_panel_open = false;
	inline static bool is_picking_panel_open = false;
	inline static bool is_compare_panel_open = false;
	inline static bool is_custom_file_info_editor_open = false;
	inline static bool is_extract_panel_open = false;
	inline static bool is_byte_search_panel_open = false;
	inline static bool is_pathfinding_panel_open = false;
	inline static bool is_model_viewer_panel_open = false;
	inline static bool is_window_controller_open = true;
	inline static bool is_compass_open = true;
	inline static bool is_debug_match_metadata_open = false;
	inline static bool is_replay_browser_open = true;

	// 3D Agent model rendering (persisted)
	inline static bool use_3d_agent_models = true;

	// SCENE BLOOM (persisted). The game itself has always had it, on every guild hall, with no
	// setting that turns it off, so a replay without it can never look like the game - but it is
	// an 11-15% lift on top of a picture whose level is still an open question, and the owner's
	// decision is that the DEFAULT look is the one the last commit drew. So the toggle stays and
	// the default is OFF; a user who already turned it on keeps it, in either light mode.
	static constexpr bool kDefaultMapBloomEnabled = false;
	inline static bool map_bloom_enabled = kDefaultMapBloomEnabled;

	// ---- THE MAP LIGHT MODE (persisted) -----------------------------------------------------
	//
	// Two complete looks, chosen by the owner, not a slider between them. Nothing is deleted:
	// both paths are live code and this switch picks which one runs.
	//
	//   Classic (the DEFAULT) - the light and the terrain law of the last commit, which is the
	//     picture the owner has approved. The light is the previous override: the map's colour
	//     bytes over 255*2, with the HSL LIGHTNESS of the result rewritten to
	//     max(intensity/255 * 0.9, floor) - floor 0.70 for the ambient and 0.50 for the sun - so
	//     the map keeps its hue and the level comes from the floors. The terrain multiplies its
	//     blended texture by 1.4 * lightingColor. ONE thing differs from the commit, and it is a
	//     bug fix rather than a look change: the colours fed into that override are the
	//     REGION-BLENDED ones the environment table resolves per frame, not entry 0 of every
	//     list. The committed build never read the region table at all, so every map was drawn
	//     on its entry 0; now the tower side of Corrupted Isle takes its own warm entry and the
	//     blue-base side its own blue one, both at the old brightness. The gain below is LIVE in
	//     this mode too, with its own per-mode value: the floors decide the light's HUE and its
	//     level relative to the map, and the gain is the one brightness the owner sets on top of
	//     them. Its Classic default is calibrated, so the flag stand matches the reference
	//     capture instead of sitting well above it - see the gain block below.
	//
	//   Client (experimental) - the client's own light law, read out of Gw.exe
	//     (colour = bytes/255 * intensity/256, no floors anywhere) and the client's own terrain
	//     law: one lerp between two endpoint colours at a quartic bake. It is the reading the
	//     binary supports and it renders the ground too dark against the reference capture, for
	//     a reason that is still open - see the note under docs/appearance_data. The pre-clamp
	//     gain below has its own value in this mode and keeps its calibrated default there.
	//
	// Everything that is not the light LEVEL is shared by both modes: the region table and the
	// query point, the haze curve, the per-map prop modulate gate, the shadows, and every one of
	// the character passes.
	static constexpr int kMapLightModeClassic   = 0;
	static constexpr int kMapLightModeClientExp = 1;
	static constexpr int kDefaultMapLightMode   = kMapLightModeClassic;
	inline static int map_light_mode = kDefaultMapLightMode;

	static int ClampMapLightMode(int mode)
	{
		return (mode == kMapLightModeClientExp) ? kMapLightModeClientExp : kMapLightModeClassic;
	}

	static bool IsClassicMapLight() { return map_light_mode == kMapLightModeClassic; }

	static const char* MapLightModeName()
	{
		return IsClassicMapLight() ? "Classic" : "Client (experimental)";
	}

	// ENVIRONMENT LIGHT GAIN for the map - the terrain and the world's own models only. The game
	// applies no gain of its own: its terrain program is an authored one that multiplies the
	// texture by a plain blend of two light colours with no doubling anywhere, and the per-map
	// doubling that exists for models is read only by the model code. So 1.00x is the client's
	// own light law, and this control is a brightness the owner chooses on top of it.
	//
	// HOW IT IS APPLIED: to the light's INPUTS - "the same map under brighter lights", not "the
	// same image, amplified". In Client mode those inputs are then clamped per channel, so a
	// channel that reaches full stays at full while the others climb towards it, which is why a
	// cold, blue-ambient hall gets brighter instead of turning saturated royal blue. Classic's
	// own law carries no clamp there and never did, so the scaling is a plain multiply on its
	// floored pair and the mode's picture at 1.00x is unchanged to the byte. Both go through the
	// SAME multiplication in the vertex, terrain and new-model programs - there is no second
	// path for Classic. See TerrainRevPixelShader.hlsl and VertexShader.hlsl.
	//
	// ONE VALUE PER MODE, and that is the whole reason there are two constants here. The two
	// light laws sit a long way apart - Classic's floors put the ambient at 0.57 to 0.83 per
	// channel where the client's law asks for a fraction of that - so one shared number cannot
	// be right in both, and a single setting would silently re-tune the other mode every time
	// the owner flipped the switch. Each mode owns its own value, each is persisted under its
	// own key, and switching modes reads the other one back exactly as it was left.
	//
	// BOTH DEFAULTS ARE CALIBRATED THE SAME WAY - neither is a value read from the client. Each
	// is the gain at which this renderer's luminance (Rec. 709) at the one tile both sides can
	// name equals the luminance of the reference capture's de-bloomed ground: the Tower Flag
	// Stand, grid cell (144, 89), albedo (0.489, 0.252, 0.121) measured out of the archive,
	// against (86.2, 55.1, 37.9), with the scene bloom on both sides.
	//
	//   Client, 2.03x - under the client's own law and its terrain lerp at a flat-ground bake of
	//     0.993. Residual there: red x1.30, green x0.89, blue x0.62.
	//
	//   Classic, 0.5486x - under the committed law, `albedo * 1.4 * (ambient + sun * N.L)`, with
	//     the floored pair Corrupted Isle region 0 resolves to, ambient (0.569, 0.633, 0.831)
	//     and sun (0.726, 0.576, 0.453), at the flat-ground N.L of 0.700. At 1.00x - what the
	//     previous build drew - that tile lands on (188.0, 93.2, 49.6) of 255 against the
	//     capture's (86.2, 55.1, 37.9): Classic is about 1.8x too bright in luminance, which is
	//     the "too bright" the owner reported. 0.5486x brings it to (103.2, 51.1, 27.2), whose
	//     Rec. 709 luminance is the capture's to the digit. Residual: red x1.20, green x0.93,
	//     blue x0.72 - the same kind of hue error the Client default leaves, and for the same
	//     reason: this matches the BRIGHTNESS at one measured tile and nothing more. No scalar
	//     can close the hue, and that question is still open.
	//
	// 1.00x is the previous build's Classic brightness, so an owner who preferred it has one
	// number to type - and it is the game's own level in Client mode. The range is the same in
	// both modes.
	static constexpr float kDefaultMapLightGainClassic = 0.5486f;  // calibrated - see above
	static constexpr float kDefaultMapLightGainClient  = 2.03f;    // calibrated - see above
	// There is deliberately no mode-less `kDefaultMapLightGain` any more: "the default" is not a
	// single number now, and a name that answered for both modes could only be wrong in one.
	static constexpr float kClientMapLightGain  = 1.0f;   // the level the game itself draws
	static constexpr float kMinMapLightGain     = 0.25f;
	static constexpr float kMaxMapLightGain     = 4.0f;
	inline static float map_light_gain_classic = kDefaultMapLightGainClassic;
	inline static float map_light_gain_client  = kDefaultMapLightGainClient;

	static float ClampMapLightGain(float gain)
	{
		return std::clamp(gain, kMinMapLightGain, kMaxMapLightGain);
	}

	// The stored value for a mode, by reference, so the panel edits one place. Anything that
	// reads or writes "the gain" goes through these and therefore cannot address the wrong mode.
	static float& MapLightGainForMode(int mode)
	{
		return (ClampMapLightMode(mode) == kMapLightModeClassic) ? map_light_gain_classic
		                                                         : map_light_gain_client;
	}
	static float& MapLightGain() { return MapLightGainForMode(map_light_mode); }

	static float DefaultMapLightGainForMode(int mode)
	{
		return (ClampMapLightMode(mode) == kMapLightModeClassic) ? kDefaultMapLightGainClassic
		                                                         : kDefaultMapLightGainClient;
	}
	static float DefaultMapLightGain() { return DefaultMapLightGainForMode(map_light_mode); }

	// THE GAIN ACTUALLY IN FORCE: the current mode's own value, clamped. It is live in both
	// modes now - where Classic once forced 1.0 - and there is exactly ONE multiplication behind
	// it, the existing pre-clamp one in the vertex, terrain and new-model programs. In Classic
	// that means the FLOORED ambient and sun are scaled before the terrain's
	// `1.4 * lightingColor` ever sees them, which is why 1.00x here is byte for byte the
	// previous build and why no second path was added to make the control work in this mode.
	static float EffectiveMapLightGain()
	{
		return ClampMapLightGain(MapLightGain());
	}

	// Replay window camera vertical FOV in degrees (persisted; GW default gameplay FOV is 50)
	static constexpr float kDefaultReplayCameraFovDegrees = 50.0f;
	static constexpr float kMinReplayCameraFovDegrees     = 30.0f;
	static constexpr float kMaxReplayCameraFovDegrees     = 90.0f;
	inline static float replay_camera_fov_degrees = kDefaultReplayCameraFovDegrees;

	static float ClampReplayCameraFovDegrees(float degrees)
	{
		return std::clamp(degrees, kMinReplayCameraFovDegrees, kMaxReplayCameraFovDegrees);
	}

	// Replay camera sensitivity multipliers (0.1x–2.0x; 1.0 = prior default behavior). Persisted.
	static constexpr float kDefaultReplayCameraSensitivityMultiplier = 1.0f;
	static constexpr float kMinReplayCameraSensitivityMultiplier     = 0.1f;
	static constexpr float kMaxReplayCameraSensitivityMultiplier     = 2.0f;
	inline static float replay_camera_pan_speed_multiplier       = kDefaultReplayCameraSensitivityMultiplier;
	inline static float replay_camera_rotation_speed_multiplier  = kDefaultReplayCameraSensitivityMultiplier;
	inline static float replay_camera_zoom_speed_multiplier      = kDefaultReplayCameraSensitivityMultiplier;
	inline static float replay_camera_keyboard_speed_multiplier = kDefaultReplayCameraSensitivityMultiplier;

	static float ClampReplayCameraSensitivityMultiplier(float mult)
	{
		return std::clamp(mult, kMinReplayCameraSensitivityMultiplier, kMaxReplayCameraSensitivityMultiplier);
	}

	// Auto Camera settings (persisted)
	inline static float  autocam_lookahead   = 3.f;
	inline static int    autocam_hp_thresh   = 70;   // percent 10-80
	inline static float  autocam_dwell       = 5.f;
	inline static bool   autocam_death       = true;
	inline static bool   autocam_low_hp      = true;
	inline static bool   autocam_lord        = true;
	inline static bool   autocam_flag        = true;
	inline static bool   autocam_rez         = true;
	inline static bool   autocam_isolated    = true;
	inline static bool   autocam_flag_carry  = true;

	// Font settings
	inline static int saved_font_index = 2;
	inline static float saved_font_size = 15.0f;
	inline static bool font_needs_rebuild = false;
	inline static ImFont* boldFont = nullptr;
	inline static ImFont* monoFont = nullptr;      // Roboto Mono Regular
	inline static ImFont* monoBoldFont = nullptr;   // Roboto Mono Bold

	// Window settings
	inline static int window_width = -1;
	inline static int window_height = -1;
	inline static int window_pos_x = -1;
	inline static int window_pos_y = -1;
	inline static bool window_maximized = false;

	// Replay browser splitter settings (-1 = use default proportions)
	inline static int replay_filter_width = -1;
	inline static int replay_list_height = -1;
	inline static int replay_card_gallery_mode = 0;  // 0=table, 1=card gallery
	inline static int replay_gallery_columns = 3;    // 2, 3, or 4
	inline static int replay_browser_theme = 0;      // 0=GW Observer, 1=Watchtower
	inline static int replay_card_style = 0;         // 0=Classic, 1=Visual

	// Update settings
	inline static std::string dismissed_update_version;

	inline static bool prev_is_dat_browser_open;
	inline static bool prev_is_dat_browser_resizeable;
	inline static bool prev_is_dat_browser_movable;
	inline static bool prev_is_left_panel_open;
	inline static bool prev_is_right_panel_open;
	inline static bool prev_is_hex_editor_open;
	inline static bool prev_is_text_panel_open;
	inline static bool prev_is_audio_controller_open;
	inline static bool prev_is_texture_panel_open;
	inline static bool prev_is_picking_panel_open;
	inline static bool prev_is_compare_panel_open;
	inline static bool prev_is_custom_file_info_editor_open;
	inline static bool prev_is_extract_panel_open;
	inline static bool prev_is_byte_search_panel_open;
	inline static bool prev_is_pathfinding_panel_open;
	inline static bool prev_is_model_viewer_panel_open;
	inline static bool prev_is_window_controller_open;
	inline static bool prev_is_compass_open;

	// Method to save the current state of all panels
	static void SaveCurrentStates()
	{
		prev_is_dat_browser_open = is_dat_browser_open;
		prev_is_dat_browser_resizeable = is_dat_browser_resizeable;
		prev_is_dat_browser_movable = is_dat_browser_movable;
		prev_is_left_panel_open = is_left_panel_open;
		prev_is_right_panel_open = is_right_panel_open;
		prev_is_hex_editor_open = is_hex_editor_open;
		prev_is_text_panel_open = is_text_panel_open;
		prev_is_audio_controller_open = is_audio_controller_open;
		prev_is_texture_panel_open = is_texture_panel_open;
		prev_is_picking_panel_open = is_picking_panel_open;
		prev_is_compare_panel_open = is_compare_panel_open;
		prev_is_custom_file_info_editor_open = is_custom_file_info_editor_open;
		prev_is_extract_panel_open = is_extract_panel_open;
		prev_is_byte_search_panel_open = is_byte_search_panel_open;
		prev_is_pathfinding_panel_open = is_pathfinding_panel_open;
		prev_is_model_viewer_panel_open = is_model_viewer_panel_open;
		prev_is_window_controller_open = is_window_controller_open;
		prev_is_compass_open = is_compass_open;
	}

	// Method to restore the previous state of all panels
	static void RestorePreviousStates()
	{
		is_dat_browser_open = prev_is_dat_browser_open;
		is_dat_browser_movable = prev_is_dat_browser_movable;
		is_dat_browser_resizeable = prev_is_dat_browser_resizeable;
		is_left_panel_open = prev_is_left_panel_open;
		is_right_panel_open = prev_is_right_panel_open;
		is_hex_editor_open = prev_is_hex_editor_open;
		is_text_panel_open = prev_is_text_panel_open;
		is_audio_controller_open = prev_is_audio_controller_open;
		is_texture_panel_open = prev_is_texture_panel_open;
		is_picking_panel_open = prev_is_picking_panel_open;
		is_compare_panel_open = prev_is_compare_panel_open;
		is_custom_file_info_editor_open = prev_is_custom_file_info_editor_open;
		is_extract_panel_open = prev_is_extract_panel_open;
		is_byte_search_panel_open = prev_is_byte_search_panel_open;
		is_pathfinding_panel_open = prev_is_pathfinding_panel_open;
		is_model_viewer_panel_open = prev_is_model_viewer_panel_open;
		is_window_controller_open = prev_is_window_controller_open;
		is_compass_open = prev_is_compass_open;
	}

	// Method to set the hide_all state and update panels accordingly
	static void SetHideAll(bool hide)
	{
		if (hide)
		{
			if (!hide_all) {
				SaveCurrentStates();
			}
			hide_all = true;
			is_dat_browser_open = false;
			is_dat_browser_resizeable = false;
			is_dat_browser_movable = false;
			is_left_panel_open = false;
			is_right_panel_open = false;
			is_hex_editor_open = false;
			is_text_panel_open = false;
			is_audio_controller_open = false;
			is_texture_panel_open = false;
			is_picking_panel_open = false;
			is_compare_panel_open = false;
			is_custom_file_info_editor_open = false;
			is_extract_panel_open = false;
			is_byte_search_panel_open = false;
			is_pathfinding_panel_open = false;
			is_model_viewer_panel_open = false;
			is_compass_open = false;
		}
		else
		{
			RestorePreviousStates();
			hide_all = false;
		}
	}

	// Method to reset all panels to default visibility
	static void ResetToDefaults()
	{
		hide_all = false;
		is_dat_browser_open = true;
		is_dat_browser_resizeable = false;
		is_dat_browser_movable = false;
		is_left_panel_open = false;
		is_right_panel_open = true;
		is_hex_editor_open = false;
		is_text_panel_open = false;
		is_audio_controller_open = false;
		is_texture_panel_open = false;
		is_picking_panel_open = false;
		is_compare_panel_open = false;
		is_custom_file_info_editor_open = false;
		is_extract_panel_open = false;
		is_byte_search_panel_open = false;
		is_pathfinding_panel_open = false;
		is_model_viewer_panel_open = false;
		is_window_controller_open = true;
		is_compass_open = true;
	}

	// Helper to clamp a window to stay within screen bounds
	// Call this after ImGui::Begin() returns true for floating windows
	static void ClampWindowToScreen()
	{
		ImVec2 pos = ImGui::GetWindowPos();
		ImVec2 size = ImGui::GetWindowSize();
		ImVec2 display = ImGui::GetIO().DisplaySize;
		const float margin = 50.0f;

		bool needsClamp = false;

		// Ensure at least 'margin' pixels of window are visible on each side
		if (pos.x + size.x < margin) {
			pos.x = margin - size.x + 100;
			needsClamp = true;
		}
		if (pos.x > display.x - margin) {
			pos.x = display.x - margin - 100;
			needsClamp = true;
		}
		if (pos.y < 0) {
			pos.y = 10;
			needsClamp = true;
		}
		if (pos.y > display.y - margin) {
			pos.y = display.y - margin - 100;
			needsClamp = true;
		}

		if (needsClamp) {
			ImGui::SetWindowPos(pos);
		}
	}

	// Get the directory where the executable lives
	static std::filesystem::path GetExeDir()
	{
		std::wstring buf(MAX_PATH, L'\0');
		for (;;)
		{
			DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
			if (len == 0)
				return std::filesystem::path(L".");
			if (len < buf.size())
			{
				buf.resize(len);
				break;
			}
			// Truncated (buffer too small and not null terminated) -- retry bigger.
			if (buf.size() >= 32768)
				return std::filesystem::path(L".");
			buf.resize(buf.size() * 2, L'\0');
		}
		return std::filesystem::path(buf).parent_path();
	}

	// Per-user writable location, used when the exe directory is read-only.
	static std::filesystem::path GetUserDataDir()
	{
		auto from_env = [](const wchar_t* name) -> std::filesystem::path
		{
			wchar_t* value = nullptr;
			size_t len = 0;
			std::filesystem::path result;
			if (_wdupenv_s(&value, &len, name) == 0 && value && *value)
				result = value;
			free(value);
			return result;
		};

		if (auto local = from_env(L"LOCALAPPDATA"); !local.empty())
			return local / L"GWObserver";
		if (auto tmp = from_env(L"TEMP"); !tmp.empty())
			return tmp / L"GWObserver";
		return GetExeDir() / L"UserData";
	}

	// Match cache directory, created on first use.
	//
	// Prefers "MatchCache" next to the executable. If that cannot be created
	// (exe installed under Program Files, on read-only media, or blocked by
	// Controlled Folder Access) it falls back to %LOCALAPPDATA%\GWObserver so
	// the app stays usable. Never throws -- a failure here used to escape as an
	// unhandled filesystem_error and kill the process during first-launch setup.
	static const std::filesystem::path& GetMatchCachePath()
	{
		static const std::filesystem::path cached = []() -> std::filesystem::path
		{
			std::error_code ec;
			auto preferred = GetExeDir() / L"MatchCache";
			if (std::filesystem::is_directory(preferred, ec))
				return preferred;

			std::filesystem::create_directories(preferred, ec);
			if (std::filesystem::is_directory(preferred, ec))
				return preferred;

			auto fallback = GetUserDataDir() / L"MatchCache";
			std::filesystem::create_directories(fallback, ec);
			return fallback;
		}();
		return cached;
	}

	// True when the cache had to be redirected away from the exe directory.
	static bool IsMatchCacheRedirected()
	{
		std::error_code ec;
		return !std::filesystem::equivalent(GetMatchCachePath(), GetExeDir() / L"MatchCache", ec);
	}

	static std::string GetMatchCacheDir()
	{
		return GetMatchCachePath().string();
	}

	// Get the settings file path (next to executable)
	static std::filesystem::path GetSettingsFilePath()
	{
		return GetExeDir() / "gui_settings.ini";
	}

	// Save window visibility settings to file
	static void SaveSettings()
	{
		std::ofstream file(GetSettingsFilePath());
		if (!file.is_open()) return;

		file << "[WindowVisibility]\n";
		file << "dat_browser=" << (is_dat_browser_open ? 1 : 0) << "\n";
		file << "dat_browser_resizeable=" << (is_dat_browser_resizeable ? 1 : 0) << "\n";
		file << "dat_browser_movable=" << (is_dat_browser_movable ? 1 : 0) << "\n";
		file << "left_panel=" << (is_left_panel_open ? 1 : 0) << "\n";
		file << "right_panel=" << (is_right_panel_open ? 1 : 0) << "\n";
		file << "hex_editor=" << (is_hex_editor_open ? 1 : 0) << "\n";
		file << "text_panel=" << (is_text_panel_open ? 1 : 0) << "\n";
		file << "audio_controller=" << (is_audio_controller_open ? 1 : 0) << "\n";
		file << "texture_panel=" << (is_texture_panel_open ? 1 : 0) << "\n";
		file << "picking_panel=" << (is_picking_panel_open ? 1 : 0) << "\n";
		file << "compare_panel=" << (is_compare_panel_open ? 1 : 0) << "\n";
		file << "custom_file_info_editor=" << (is_custom_file_info_editor_open ? 1 : 0) << "\n";
		file << "extract_panel=" << (is_extract_panel_open ? 1 : 0) << "\n";
		file << "byte_search_panel=" << (is_byte_search_panel_open ? 1 : 0) << "\n";
		file << "pathfinding_panel=" << (is_pathfinding_panel_open ? 1 : 0) << "\n";
		file << "model_viewer_panel=" << (is_model_viewer_panel_open ? 1 : 0) << "\n";
		file << "window_controller=" << (is_window_controller_open ? 1 : 0) << "\n";
		file << "compass=" << (is_compass_open ? 1 : 0) << "\n";
		file << "debug_match_metadata=" << (is_debug_match_metadata_open ? 1 : 0) << "\n";
		file << "replay_browser=" << (is_replay_browser_open ? 1 : 0) << "\n";

		file << "window_width=" << window_width << "\n";
		file << "window_height=" << window_height << "\n";
		file << "window_pos_x=" << window_pos_x << "\n";
		file << "window_pos_y=" << window_pos_y << "\n";
		file << "window_maximized=" << (window_maximized ? 1 : 0) << "\n";
		file << "replay_filter_width=" << replay_filter_width << "\n";
		file << "replay_list_height=" << replay_list_height << "\n";
		file << "replay_card_gallery_mode=" << replay_card_gallery_mode << "\n";
		file << "replay_gallery_columns=" << replay_gallery_columns << "\n";
		file << "replay_browser_theme=" << replay_browser_theme << "\n";
		file << "replay_card_style=" << replay_card_style << "\n";

		file << "\n[Updates]\n";
		file << "dismissed_update_version=" << dismissed_update_version << "\n";

		file << "\n[Rendering]\n";
		file << "use_3d_agent_models=" << (use_3d_agent_models ? 1 : 0) << "\n";
		file << "map_bloom_enabled=" << (map_bloom_enabled ? 1 : 0) << "\n";
		file << "map_light_mode=" << map_light_mode << "\n";
		// ONE KEY PER MODE. The old single `map_light_gain` key is still READ below, as the
		// Client mode's value, so an existing settings file keeps the gain it was tuned to; it
		// is no longer written, because two sources of truth for one number is how a setting
		// starts drifting.
		file << "map_light_gain_classic=" << map_light_gain_classic << "\n";
		file << "map_light_gain_client=" << map_light_gain_client << "\n";
		file << "replay_camera_fov_degrees=" << replay_camera_fov_degrees << "\n";
		file << "replay_camera_pan_speed_multiplier=" << replay_camera_pan_speed_multiplier << "\n";
		file << "replay_camera_rotation_speed_multiplier=" << replay_camera_rotation_speed_multiplier << "\n";
		file << "replay_camera_zoom_speed_multiplier=" << replay_camera_zoom_speed_multiplier << "\n";
		file << "replay_camera_keyboard_speed_multiplier=" << replay_camera_keyboard_speed_multiplier << "\n";

		file << "\n[AutoCamera]\n";
		file << "autocam_lookahead=" << static_cast<int>(autocam_lookahead) << "\n";
		file << "autocam_hp_thresh=" << autocam_hp_thresh << "\n";
		file << "autocam_dwell=" << static_cast<int>(autocam_dwell) << "\n";
		file << "autocam_death=" << (autocam_death ? 1 : 0) << "\n";
		file << "autocam_low_hp=" << (autocam_low_hp ? 1 : 0) << "\n";
		file << "autocam_lord=" << (autocam_lord ? 1 : 0) << "\n";
		file << "autocam_flag=" << (autocam_flag ? 1 : 0) << "\n";
		file << "autocam_rez=" << (autocam_rez ? 1 : 0) << "\n";
		file << "autocam_isolated=" << (autocam_isolated ? 1 : 0) << "\n";
		file << "autocam_flag_carry=" << (autocam_flag_carry ? 1 : 0) << "\n";

		file << "\n[Font]\n";
		file << "font_index=" << saved_font_index << "\n";
		file << "font_size=" << static_cast<int>(saved_font_size) << "\n";

		file << "\n[Config]\n";
		file << "gw_dat_path=" << saved_gw_dat_path << "\n";
		file << "match_data_folder=" << saved_match_data_folder_path << "\n";
		if (!contributor_key.empty())
			file << "contributor_key=" << contributor_key << "\n";

		file.close();
	}

	// Load window visibility settings from file
	static void LoadSettings()
	{
		if (settings_loaded) return;
		settings_loaded = true;

		std::ifstream file(GetSettingsFilePath());
		if (!file.is_open()) return; // Use defaults if no settings file

		bool loadedPan = false, loadedRot = false, loadedZoom = false, loadedKbd = false;
		float legacyUnifiedMovement = -1.f;

		std::string line;
		while (std::getline(file, line)) {
			if (line.empty() || line[0] == '[') continue;

			size_t pos = line.find('=');
			if (pos == std::string::npos) continue;

			std::string key = line.substr(0, pos);
			std::string val_str = line.substr(pos + 1);

			if (key == "map_light_gain_classic") {
				try {
					map_light_gain_classic = ClampMapLightGain(std::stof(val_str));
				} catch (...) {}
				continue;
			}
			if (key == "map_light_gain_client") {
				try {
					map_light_gain_client = ClampMapLightGain(std::stof(val_str));
				} catch (...) {}
				continue;
			}
			// MIGRATION, read-only: the single pre-per-mode key. It could only ever have been
			// the Client mode's value - Classic forced 1.0 and the panel greyed the control -
			// so that is where it lands, and Classic starts at its own calibrated default. The
			// writer no longer emits this key, so one save replaces it with the pair.
			if (key == "map_light_gain") {
				try {
					map_light_gain_client = ClampMapLightGain(std::stof(val_str));
				} catch (...) {}
				continue;
			}
			if (key == "replay_camera_fov_degrees") {
				try {
					replay_camera_fov_degrees = ClampReplayCameraFovDegrees(std::stof(val_str));
				} catch (...) {}
				continue;
			}
			if (key == "replay_camera_pan_speed_multiplier") {
				try {
					replay_camera_pan_speed_multiplier =
						ClampReplayCameraSensitivityMultiplier(std::stof(val_str));
					loadedPan = true;
				} catch (...) {}
				continue;
			}
			if (key == "replay_camera_rotation_speed_multiplier") {
				try {
					replay_camera_rotation_speed_multiplier =
						ClampReplayCameraSensitivityMultiplier(std::stof(val_str));
					loadedRot = true;
				} catch (...) {}
				continue;
			}
			if (key == "replay_camera_zoom_speed_multiplier") {
				try {
					replay_camera_zoom_speed_multiplier =
						ClampReplayCameraSensitivityMultiplier(std::stof(val_str));
					loadedZoom = true;
				} catch (...) {}
				continue;
			}
			if (key == "replay_camera_keyboard_speed_multiplier") {
				try {
					replay_camera_keyboard_speed_multiplier =
						ClampReplayCameraSensitivityMultiplier(std::stof(val_str));
					loadedKbd = true;
				} catch (...) {}
				continue;
			}
			// Legacy single slider (before pan/rotation/zoom/keyboard split)
			if (key == "replay_camera_movement_speed_multiplier") {
				try {
					legacyUnifiedMovement = ClampReplayCameraSensitivityMultiplier(std::stof(val_str));
				} catch (...) {}
				continue;
			}

			if (key == "gw_dat_path") {
				saved_gw_dat_path = val_str;
				continue;
			}
			if (key == "match_data_folder") {
				saved_match_data_folder_path = val_str;
				continue;
			}
			if (key == "contributor_key") {
				contributor_key = val_str;
				continue;
			}
			if (key == "dismissed_update_version") {
				dismissed_update_version = val_str;
				continue;
			}

			int value = 0;
			try { value = std::stoi(val_str); } catch (...) { continue; }

			if (key == "dat_browser") is_dat_browser_open = (value != 0);
			else if (key == "dat_browser_resizeable") is_dat_browser_resizeable = (value != 0);
			else if (key == "dat_browser_movable") is_dat_browser_movable = (value != 0);
			else if (key == "left_panel") is_left_panel_open = (value != 0);
			else if (key == "right_panel") is_right_panel_open = (value != 0);
			else if (key == "hex_editor") is_hex_editor_open = (value != 0);
			else if (key == "text_panel") is_text_panel_open = (value != 0);
			else if (key == "audio_controller") is_audio_controller_open = (value != 0);
			else if (key == "texture_panel") is_texture_panel_open = (value != 0);
			else if (key == "picking_panel") is_picking_panel_open = (value != 0);
			else if (key == "compare_panel") is_compare_panel_open = (value != 0);
			else if (key == "custom_file_info_editor") is_custom_file_info_editor_open = (value != 0);
			else if (key == "extract_panel") is_extract_panel_open = (value != 0);
			else if (key == "byte_search_panel") is_byte_search_panel_open = (value != 0);
			else if (key == "pathfinding_panel") is_pathfinding_panel_open = (value != 0);
			else if (key == "model_viewer_panel") is_model_viewer_panel_open = (value != 0);
			else if (key == "window_controller") is_window_controller_open = (value != 0);
			else if (key == "compass") is_compass_open = (value != 0);
			else if (key == "debug_match_metadata") is_debug_match_metadata_open = (value != 0);
			else if (key == "replay_browser") is_replay_browser_open = (value != 0);
			else if (key == "use_3d_agent_models") use_3d_agent_models = (value != 0);
			else if (key == "map_bloom_enabled") map_bloom_enabled = (value != 0);
			// An existing settings file has no map_light_mode key, so it takes the default -
			// Classic - while its saved map_light_gain is still loaded and kept as the Client
			// mode's own value, Classic taking its calibrated default on that first run.
			else if (key == "map_light_mode") map_light_mode = ClampMapLightMode(value);
			else if (key == "autocam_lookahead") autocam_lookahead = static_cast<float>(value);
			else if (key == "autocam_hp_thresh") autocam_hp_thresh = value;
			else if (key == "autocam_dwell") autocam_dwell = static_cast<float>(value);
			else if (key == "autocam_death") autocam_death = (value != 0);
			else if (key == "autocam_low_hp") autocam_low_hp = (value != 0);
			else if (key == "autocam_lord") autocam_lord = (value != 0);
			else if (key == "autocam_flag") autocam_flag = (value != 0);
			else if (key == "autocam_rez") autocam_rez = (value != 0);
			else if (key == "autocam_isolated") autocam_isolated = (value != 0);
			else if (key == "autocam_flag_carry") autocam_flag_carry = (value != 0);
			else if (key == "font_index") saved_font_index = value;
			else if (key == "font_size") saved_font_size = static_cast<float>(value);
			else if (key == "window_width") window_width = value;
			else if (key == "window_height") window_height = value;
			else if (key == "window_pos_x") window_pos_x = value;
			else if (key == "window_pos_y") window_pos_y = value;
			else if (key == "window_maximized") window_maximized = (value != 0);
			else if (key == "replay_filter_width") replay_filter_width = value;
			else if (key == "replay_list_height") replay_list_height = value;
			else if (key == "replay_card_gallery_mode") replay_card_gallery_mode = value;
			else if (key == "replay_gallery_columns") replay_gallery_columns = std::clamp(value, 2, 4);
			else if (key == "replay_browser_theme") replay_browser_theme = std::clamp(value, 0, 1);
			else if (key == "replay_card_style") replay_card_style = std::clamp(value, 0, 1);
		}

		if (legacyUnifiedMovement >= 0.f) {
			if (!loadedPan)
				replay_camera_pan_speed_multiplier = legacyUnifiedMovement;
			if (!loadedRot)
				replay_camera_rotation_speed_multiplier = legacyUnifiedMovement;
			if (!loadedZoom)
				replay_camera_zoom_speed_multiplier = legacyUnifiedMovement;
			if (!loadedKbd)
				replay_camera_keyboard_speed_multiplier = legacyUnifiedMovement;
		}

		file.close();
	}
};
