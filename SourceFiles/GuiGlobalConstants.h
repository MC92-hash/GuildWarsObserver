#pragma once
#include "build_config.h"
#include "imgui.h"
#include <fstream>
#include <string>
#include <filesystem>
#include <algorithm>

class GuiGlobalConstants
{
public:
	inline static bool settings_loaded = false;

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

	// Developer mode: compile-time gated via GWO_DEVELOPER from build_config.h
	static constexpr bool IsDeveloperMode() { return GWO_DEVELOPER != 0; }

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

	// Window settings
	inline static int window_width = -1;
	inline static int window_height = -1;
	inline static int window_pos_x = -1;
	inline static int window_pos_y = -1;
	inline static bool window_maximized = false;

	// Replay browser splitter settings (-1 = use default proportions)
	inline static int replay_filter_width = -1;
	inline static int replay_list_height = -1;

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
		wchar_t exePath[MAX_PATH];
		GetModuleFileNameW(NULL, exePath, MAX_PATH);
		return std::filesystem::path(exePath).parent_path();
	}

	// Get the match cache directory (next to executable)
	static std::string GetMatchCacheDir()
	{
		return (GetExeDir() / "MatchCache").string();
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

		file << "\n[Rendering]\n";
		file << "use_3d_agent_models=" << (use_3d_agent_models ? 1 : 0) << "\n";
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
