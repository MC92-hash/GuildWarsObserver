#include "pch.h"
#include "draw_ui.h"
#include "draw_dat_browser.h"
#include "draw_gui_for_open_dat_file.h"
#include "draw_dat_load_progress_bar.h"
#include "draw_left_panel.h"
#include "draw_right_panel.h"
#include "draw_texture_panel.h"
#include "draw_audio_controller_panel.h"
#include "draw_text_panel.h"
#include "draw_hex_editor_panel.h"
#include "draw_picking_info.h"
#include "draw_dat_compare_panel.h"
#include "draw_file_info_editor_panel.h"
#include "draw_pathfinding_panel.h"
#include "animation_state.h"
#include "ModelViewer/ModelViewerPanel.h"
#include "draw_debug_match_metadata.h"
#include "draw_replay_browser.h"
#include "ReplayLibrary.h"
#include "FontConfig.h"
#include <draw_gui_window_controller.h>
#include <draw_extract_panel.h>
#include <byte_pattern_search_panel.h>
#include <windows.h>

extern FileType selected_file_type;
extern HSTREAM selected_audio_stream_handle;
extern std::string selected_text_file_str;
extern std::vector<uint8_t> selected_raw_data;
bool dat_manager_to_show_changed = false;
bool dat_compare_filter_result_changed = false;
bool custom_file_info_changed = false;
std::unordered_set<uint32_t> dat_compare_filter_result;

static void draw_compass_overlay(MapRenderer* map_renderer)
{
	if (!GuiGlobalConstants::is_compass_open || !map_renderer) {
		return;
	}

	Camera* camera = map_renderer->GetCamera();
	if (!camera) {
		return;
	}

	const float yaw = camera->GetYaw();   // radians, 0 = +Z, +90deg = +X
	const float pitch = camera->GetPitch();

	// Heading: 0 = North (+Z), 90 = East (+X)
	float heading_deg = yaw * (180.0f / 3.14159265358979323846f);
	heading_deg = fmodf(heading_deg + 360.0f, 360.0f);

	ImGuiIO& io = ImGui::GetIO();
	const ImVec2 display = io.DisplaySize;

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoDecoration |
		ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav |
		ImGuiWindowFlags_AlwaysAutoResize;

#ifdef IMGUI_HAS_DOCK
	flags |= ImGuiWindowFlags_NoDocking;
#endif

	ImGui::SetNextWindowPos(
		ImVec2(display.x * 0.5f, GuiGlobalConstants::menu_bar_height + 10.0f),
		ImGuiCond_FirstUseEver,
		ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.35f);

	if (!ImGui::Begin("##compass_overlay", nullptr, flags)) {
		ImGui::End();
		return;
	}

	const float radius = 42.0f;
	const float canvas_size = radius * 2.0f + 28.0f;

	// Explicit drag handle so it remains movable even with "move from title bar only" configs.
	{
		ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 25));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 255, 255, 35));

		// IMPORTANT: keep this width fixed. Using GetWindowWidth() here creates a feedback loop with
		// ImGuiWindowFlags_AlwaysAutoResize and the window expands every frame.
		const float w = canvas_size;
		ImGui::InvisibleButton("##compass_drag", ImVec2(w, 18.0f));
		const ImVec2 bar_min = ImGui::GetItemRectMin();
		const ImVec2 bar_max = ImGui::GetItemRectMax();
		if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
		{
			ImGui::SetWindowPos(ImGui::GetWindowPos() + io.MouseDelta);
		}

		ImGui::SetItemAllowOverlap();
		ImGui::SetCursorScreenPos(ImVec2(bar_min.x + 6.0f, bar_min.y + 1.0f));
		ImGui::TextUnformatted("Compass");

		// Close button (top-right)
		ImGui::SetCursorScreenPos(ImVec2(bar_max.x - 18.0f, bar_min.y));
		if (ImGui::SmallButton("x"))
		{
			GuiGlobalConstants::is_compass_open = false;
			GuiGlobalConstants::SaveSettings();
			ImGui::PopStyleColor(3);
			ImGui::End();
			return;
		}

		// Continue below the drag bar.
		ImGui::SetCursorScreenPos(ImVec2(bar_min.x, bar_max.y + 4.0f));

		ImGui::PopStyleColor(3);
	}

	const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##compass_canvas", ImVec2(canvas_size, canvas_size));

	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImU32 col_ring = IM_COL32(230, 230, 230, 220);
	const ImU32 col_text = IM_COL32(255, 255, 255, 220);
	const ImU32 col_arrow = IM_COL32(80, 200, 255, 255);
	const ImU32 col_arrow_fill = IM_COL32(80, 200, 255, 200);

	const ImVec2 center = ImVec2(canvas_pos.x + canvas_size * 0.5f, canvas_pos.y + canvas_size * 0.5f);

	dl->AddCircle(center, radius, col_ring, 48, 2.0f);

	// Cardinal labels (N at top, E right, S bottom, W left)
	dl->AddText(ImVec2(center.x - 4.0f, center.y - radius - 14.0f), col_text, "N");
	dl->AddText(ImVec2(center.x + radius + 6.0f, center.y - 6.0f), col_text, "E");
	dl->AddText(ImVec2(center.x - 4.0f, center.y + radius + 2.0f), col_text, "S");
	dl->AddText(ImVec2(center.x - radius - 14.0f, center.y - 6.0f), col_text, "W");

	// Arrow direction in screen-space: yaw=0 => up, yaw=+90deg => right.
	const float dx = sinf(yaw);
	const float dy = -cosf(yaw);
	const float len = std::max(1e-6f, sqrtf(dx * dx + dy * dy));
	const float ndx = dx / len;
	const float ndy = dy / len;
	const float pdx = -ndy;
	const float pdy = ndx;

	const float tip_len = radius - 6.0f;
	const float base_len = radius - 18.0f;
	const float head_w = 7.0f;

	const ImVec2 tip_pt = ImVec2(center.x + ndx * tip_len, center.y + ndy * tip_len);
	const ImVec2 base_pt = ImVec2(center.x + ndx * base_len, center.y + ndy * base_len);
	const ImVec2 left_pt = ImVec2(base_pt.x + pdx * head_w, base_pt.y + pdy * head_w);
	const ImVec2 right_pt = ImVec2(base_pt.x - pdx * head_w, base_pt.y - pdy * head_w);

	dl->AddTriangleFilled(tip_pt, left_pt, right_pt, col_arrow_fill);
	dl->AddLine(center, base_pt, col_arrow, 2.0f);
	dl->AddTriangle(tip_pt, left_pt, right_pt, col_arrow, 1.0f);

	ImGui::Spacing();
	ImGui::Text("Heading: %.1f deg", heading_deg);
	ImGui::Text("Pitch:   %.1f deg", pitch * (180.0f / 3.14159265358979323846f));

	ImGui::End();
}

static bool s_preferences_open = false;

static void draw_preferences_window()
{
	if (!s_preferences_open) return;

	ImGui::SetNextWindowSize(ImVec2(420, 220), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Preferences", &s_preferences_open, ImGuiWindowFlags_NoCollapse))
	{
		ImGui::End();
		return;
	}

	ImGui::SeparatorText("Font");

	int currentFont = GuiGlobalConstants::saved_font_index;
	if (currentFont < 0 || currentFont >= g_fontTableCount)
		currentFont = 0;

	if (ImGui::BeginCombo("Font Family", g_fontTable[currentFont].displayName))
	{
		for (int i = 0; i < g_fontTableCount; i++)
		{
			bool selected = (i == currentFont);
			if (ImGui::Selectable(g_fontTable[i].displayName, selected))
			{
				GuiGlobalConstants::saved_font_index = i;
				GuiGlobalConstants::font_needs_rebuild = true;
				GuiGlobalConstants::SaveSettings();
			}
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	float fontSize = GuiGlobalConstants::saved_font_size;
	if (ImGui::SliderFloat("Font Size", &fontSize, 10.0f, 28.0f, "%.0f px"))
	{
		GuiGlobalConstants::saved_font_size = fontSize;
		GuiGlobalConstants::font_needs_rebuild = true;
		GuiGlobalConstants::SaveSettings();
	}

	ImGui::Spacing();
	ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Font changes apply immediately.");

	ImGui::End();
}

void draw_ui(std::map<int, std::unique_ptr<DATManager>>& dat_managers, int& dat_manager_to_show, MapRenderer* map_renderer, PickingInfo picking_info,
	std::vector<std::vector<std::string>>& csv_data, int& FPS_target, DX::StepTimer& timer, ExtractPanelInfo& extract_panel_info, bool& msaa_changed,
	int& msaa_level_index, const std::vector<std::pair<int, int>>& msaa_levels, std::unordered_map<int, std::vector<int>>& hash_index,
	ReplayLibrary& replay_library)
{
	// Set DAT managers pointer for animation auto-loading (needs to be available in draw_dat_browser)
	SetAnimationDATManagers(&dat_managers);
	PumpAnimationSearchResults(dat_managers);

	int initial_dat_manager_to_show = dat_manager_to_show;

	// File dialog key used by both the menu item and the first-run prompt
	static bool open_dat_file_dialog = false;
	static bool open_match_folder_dialog = false;

	// Main menu bar — always visible
	if (ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("Load .dat File...")) {
				open_dat_file_dialog = true;
			}
			if (ImGui::MenuItem("Load Match Data Folder...")) {
				open_match_folder_dialog = true;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Preferences...")) {
				s_preferences_open = true;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Exit")) {
				PostQuitMessage(0);
			}
			ImGui::EndMenu();
		}
		if (gw_dat_path_set) {
			if (ImGui::BeginMenu("View")) {
				bool changed = false;
				changed |= ImGui::MenuItem("DAT Browser", NULL, &GuiGlobalConstants::is_dat_browser_open);
				changed |= ImGui::MenuItem("Left Panel (File Info)", NULL, &GuiGlobalConstants::is_left_panel_open);
				changed |= ImGui::MenuItem("Right Panel (Render)", NULL, &GuiGlobalConstants::is_right_panel_open);
				changed |= ImGui::MenuItem("Window Controller", NULL, &GuiGlobalConstants::is_window_controller_open);
				ImGui::Separator();
				changed |= ImGui::MenuItem("Hex Editor", NULL, &GuiGlobalConstants::is_hex_editor_open);
				changed |= ImGui::MenuItem("Texture Panel", NULL, &GuiGlobalConstants::is_texture_panel_open);
				changed |= ImGui::MenuItem("Picking Info", NULL, &GuiGlobalConstants::is_picking_panel_open);
				changed |= ImGui::MenuItem("Pathfinding Map", NULL, &GuiGlobalConstants::is_pathfinding_panel_open);
				ImGui::Separator();
				changed |= ImGui::MenuItem("Audio Controller", NULL, &GuiGlobalConstants::is_audio_controller_open);
				changed |= ImGui::MenuItem("Model Viewer", NULL, &GuiGlobalConstants::is_model_viewer_panel_open);
				changed |= ImGui::MenuItem("Text Panel", NULL, &GuiGlobalConstants::is_text_panel_open);
				changed |= ImGui::MenuItem("Compass", NULL, &GuiGlobalConstants::is_compass_open);
				ImGui::Separator();
				changed |= ImGui::MenuItem("Extract Panel", NULL, &GuiGlobalConstants::is_extract_panel_open);
				changed |= ImGui::MenuItem("Compare Panel", NULL, &GuiGlobalConstants::is_compare_panel_open);
				changed |= ImGui::MenuItem("Byte Search", NULL, &GuiGlobalConstants::is_byte_search_panel_open);
				changed |= ImGui::MenuItem("Custom File Info", NULL, &GuiGlobalConstants::is_custom_file_info_editor_open);
				ImGui::Separator();
				if (ImGui::MenuItem("DAT Browser Movable/Resizeable", NULL, &GuiGlobalConstants::is_dat_browser_movable)) {
					GuiGlobalConstants::is_dat_browser_resizeable = GuiGlobalConstants::is_dat_browser_movable;
					changed = true;
				}
				if (changed) GuiGlobalConstants::SaveSettings();
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Layout")) {
				if (ImGui::MenuItem("Reset Window Visibility")) {
					GuiGlobalConstants::ResetToDefaults();
					GuiGlobalConstants::SaveSettings();
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Hide All", NULL, GuiGlobalConstants::hide_all)) {
					GuiGlobalConstants::SetHideAll(!GuiGlobalConstants::hide_all);
					GuiGlobalConstants::SaveSettings();
				}
				ImGui::EndMenu();
			}
		}
		if (ImGui::MenuItem("Replay Browser", NULL, &GuiGlobalConstants::is_replay_browser_open)) {
			GuiGlobalConstants::SaveSettings();
		}
		if (ImGui::BeginMenu("Debug")) {
			if (ImGui::MenuItem("Match Metadata", NULL, &GuiGlobalConstants::is_debug_match_metadata_open)) {
				GuiGlobalConstants::SaveSettings();
			}
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}

	// Open file dialog triggered by File menu
	if (open_dat_file_dialog) {
		open_dat_file_dialog = false;

		std::string initial_filepath = ".";
		if (!GuiGlobalConstants::saved_gw_dat_path.empty()) {
			auto parentDir = std::filesystem::path(GuiGlobalConstants::saved_gw_dat_path).parent_path();
			if (std::filesystem::exists(parentDir))
				initial_filepath = parentDir.string();
		}
		if (!std::filesystem::exists(initial_filepath) || !std::filesystem::is_directory(initial_filepath)) {
			auto exe_dir = get_executable_directory();
			if (exe_dir.has_value())
				initial_filepath = exe_dir.value().string();
		}

		ImGuiFileDialog::Instance()->OpenDialog("ChooseGwDatKey", "Select Gw.dat", ".dat",
			initial_filepath + "\\.");
	}

	// Display the file dialog (shared between menu and first-run)
	if (ImGuiFileDialog::Instance()->Display("ChooseGwDatKey", ImGuiWindowFlags_NoCollapse,
		ImVec2(500, 400)))
	{
		if (ImGuiFileDialog::Instance()->IsOk())
		{
			std::string filePathName = ImGuiFileDialog::Instance()->GetFilePathName();

			GuiGlobalConstants::saved_gw_dat_path = filePathName;
			GuiGlobalConstants::SaveSettings();

			save_last_filepath(filePathName, "dat_browser_last_filepath.txt");

			std::wstring wstr(filePathName.begin(), filePathName.end());
			gw_dat_path = wstr;
			gw_dat_path_set = true;
		}
		ImGuiFileDialog::Instance()->Close();
	}

	// Handle match data folder selection via Windows folder browser
	if (open_match_folder_dialog) {
		open_match_folder_dialog = false;

		std::wstring wFolderPath = OpenDirectoryDialog();
		if (!wFolderPath.empty())
		{
			std::string folderPath(wFolderPath.begin(), wFolderPath.end());
			GuiGlobalConstants::saved_match_data_folder_path = folderPath;
			GuiGlobalConstants::SaveSettings();

			replay_library.SetMatchDataFolder(folderPath);
			replay_library.ScanFolder();
		}
	}

	if (!gw_dat_path_set)
	{
		draw_gui_for_open_dat_file();
	}
	else
	{
		if (GuiGlobalConstants::is_window_controller_open) {
			draw_gui_window_controller();
		}

		const auto& initialization_state = dat_managers[dat_manager_to_show]->m_initialization_state;
		const auto& dat_files_read = dat_managers[dat_manager_to_show]->get_num_files_type_read();
		const auto& dat_total_files = dat_managers[dat_manager_to_show]->get_num_files();

		if (initialization_state == InitializationState::Started)
		{
			draw_dat_load_progress_bar(dat_files_read, dat_total_files);
		}
		if (initialization_state == InitializationState::Completed)
		{
			draw_data_browser(dat_managers[dat_manager_to_show].get(), map_renderer, dat_manager_to_show_changed, dat_compare_filter_result, dat_compare_filter_result_changed, csv_data, custom_file_info_changed);

			if (GuiGlobalConstants::is_left_panel_open) {
				draw_left_panel(map_renderer);
			}
			if (GuiGlobalConstants::is_right_panel_open) {
				draw_right_panel(map_renderer, FPS_target, timer, msaa_changed, msaa_level_index, msaa_levels);
			}

			draw_extract_panel(extract_panel_info, dat_managers[dat_manager_to_show].get());

			dat_compare_filter_result_changed = false;
			draw_dat_compare_panel(dat_managers, dat_manager_to_show, dat_compare_filter_result, dat_compare_filter_result_changed);

			// shares dat_compare_filter_result_changed and dat_compare_filter_result. So might not work when using both compare and byte pattern search at the same time.
			// But lets be honest, no one uses the dat compare anyways... hahaha. Right?
			draw_byte_pattern_search_panel(dat_managers, dat_manager_to_show, dat_compare_filter_result, dat_compare_filter_result_changed);

			custom_file_info_changed = false;
			custom_file_info_changed = draw_file_info_editor_panel(csv_data);

			draw_picking_info(picking_info, map_renderer, dat_managers[dat_manager_to_show].get(), hash_index);

			// Always draw these panels when enabled - they show helpful messages when no content is loaded
			draw_texture_panel(map_renderer);
			draw_pathfinding_panel(map_renderer);
			draw_audio_controller_panel(selected_audio_stream_handle);
			// Animation Controller panel removed - functionality moved to Model Viewer
			draw_model_viewer_panel(map_renderer, dat_managers);
			draw_text_panel(selected_text_file_str);
			draw_hex_editor_panel(selected_raw_data.data(), static_cast<int>(selected_raw_data.size()));

			// Overlay HUD widgets
			draw_compass_overlay(map_renderer);
		}
	}

	// Replay browser (available regardless of DAT state)
	draw_replay_browser(replay_library);

	// Debug panels (available regardless of DAT state)
	draw_debug_match_metadata_panel(replay_library);

	// Preferences window
	draw_preferences_window();

	dat_manager_to_show_changed = dat_manager_to_show != initial_dat_manager_to_show;
}

