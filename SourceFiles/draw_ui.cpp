#include "pch.h"
#include "draw_ui.h"
#include "draw_gui_for_open_dat_file.h"
#include "draw_first_launch.h"
#include "draw_dat_load_progress_bar.h"
#include "draw_debug_match_metadata.h"
#include "draw_picking_info.h"
#include "draw_replay_browser.h"
#include "GuiGlobalConstants.h"
#include "ReplayLibrary.h"
#include "FontConfig.h"
#include <windows.h>

extern FileType selected_file_type;
extern HSTREAM selected_audio_stream_handle;
extern std::string selected_text_file_str;
extern std::vector<uint8_t> selected_raw_data;
bool dat_manager_to_show_changed = false;
bool dat_compare_filter_result_changed = false;
bool custom_file_info_changed = false;
std::unordered_set<uint32_t> dat_compare_filter_result;

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
	// Loading screen: shown on every launch until dat loading completes
	// AND the progress animation has finished visually.
	{
		static bool s_loadingScreenDone = false;
		if (!s_loadingScreenDone)
		{
			auto& dm = dat_managers[dat_manager_to_show];
			float dat_frac = 0.f;
			if (gw_dat_path_set)
			{
				if (dm->m_initialization_state == InitializationState::Completed)
					dat_frac = 1.f;
				else if (dm->m_initialization_state == InitializationState::Started)
				{
					int total = dm->get_num_files();
					int read  = dm->get_num_files_type_read();
					if (total > 0)
						dat_frac = static_cast<float>(read) / static_cast<float>(total);
				}
			}

			if (draw_first_launch(gw_dat_path_set, dat_frac))
				s_loadingScreenDone = true;
			else
				return;
		}
	}

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
		if (ImGui::MenuItem("Replay Browser", NULL, &GuiGlobalConstants::is_replay_browser_open)) {
			GuiGlobalConstants::SaveSettings();
		}
		if (ImGui::BeginMenu("Debug")) {
			if (ImGui::MenuItem("Match Metadata", NULL, &GuiGlobalConstants::is_debug_match_metadata_open)) {
				GuiGlobalConstants::SaveSettings();
			}
			if (ImGui::BeginMenu("Responsive Test Mode")) {
				struct Preset { const char* label; int w; int h; };
				static const Preset presets[] = {
					{ " 800 x  600", 800, 600 },
					{ "1024 x  768", 1024, 768 },
					{ "1366 x  768", 1366, 768 },
					{ "1600 x  900", 1600, 900 },
					{ "1920 x 1080", 1920, 1080 },
					{ "2560 x 1440", 2560, 1440 },
				};
				for (const auto& p : presets) {
					if (ImGui::MenuItem(p.label)) {
						HWND hw = FindWindowW(L"GuildWarsObserverWindowClass", nullptr);
						if (hw) {
							RECT rc = { 0, 0, (LONG)p.w, (LONG)p.h };
							AdjustWindowRectEx(&rc, GetWindowLong(hw, GWL_STYLE), TRUE, GetWindowLong(hw, GWL_EXSTYLE));
							SetWindowPos(hw, nullptr, 0, 0,
								rc.right - rc.left, rc.bottom - rc.top,
								SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
							ShowWindow(hw, SW_RESTORE);
						}
					}
				}
				ImGui::EndMenu();
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

	const auto& initialization_state = dat_managers[dat_manager_to_show]->m_initialization_state;
	const auto& dat_files_read = dat_managers[dat_manager_to_show]->get_num_files_type_read();
	const auto& dat_total_files = dat_managers[dat_manager_to_show]->get_num_files();

	if (initialization_state == InitializationState::Started)
	{
		draw_dat_load_progress_bar(dat_files_read, dat_total_files);
	}

	// Replay browser (available regardless of DAT state)
	draw_replay_browser(replay_library);

	// Debug panels (available regardless of DAT state)
	draw_debug_match_metadata_panel(replay_library);

	// Preferences window
	draw_preferences_window();

	dat_manager_to_show_changed = dat_manager_to_show != initial_dat_manager_to_show;
}

