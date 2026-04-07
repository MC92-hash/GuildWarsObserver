#include "pch.h"
#include "draw_ui.h"
#include "draw_gui_for_open_dat_file.h"
#include "draw_first_launch.h"
#include "draw_setup_wizard.h"
#include "SetupConfig.h"
#include "draw_dat_load_progress_bar.h"
#include "draw_debug_match_metadata.h"
#include "draw_picking_info.h"
#include "draw_replay_browser.h"
#include "GuiGlobalConstants.h"
#include "ReplayLibrary.h"
#include "FolderWatcher.h"
#include "FontConfig.h"
#include "ReplayHotkeys.h"
#include "Net/SyncEngine.h"
#include <windows.h>
#include <filesystem>

extern FileType selected_file_type;
extern HSTREAM selected_audio_stream_handle;
extern std::string selected_text_file_str;
extern std::vector<uint8_t> selected_raw_data;
bool dat_manager_to_show_changed = false;
bool dat_compare_filter_result_changed = false;
bool custom_file_info_changed = false;
std::unordered_set<uint32_t> dat_compare_filter_result;

static bool s_settingsOpen = false;
static bool s_licenceModalOpen = false;

// Cloud storage settings state
static SyncEngine* s_syncEnginePtr = nullptr;
static FolderWatcher* s_folderWatcherPtr = nullptr;
static bool s_storageNeedsRestart = false;

static void draw_settings_window()
{
	if (!s_settingsOpen) return;

	using Clock = std::chrono::steady_clock;

	// File Paths state (persistent across frames while window is open)
	static char datBuf[512] = "";
	static DatValidation datVal = DatValidation::None;
	static std::string autoDetectMsg;
	static bool datDialogOpen = false;

	static char folderBuf[512] = "";
	static FolderValidation folderVal = FolderValidation::None;
	static bool folderDialogOpen = false;

	static bool initialized = false;
	static Clock::time_point datSaveTime;
	static bool datSaved = false;
	static Clock::time_point folderSaveTime;
	static bool folderSaved = false;

	if (!initialized)
	{
		initialized = true;

		std::string datCur = SetupConfig::dat_file_path;
		if (datCur.empty()) datCur = GuiGlobalConstants::saved_gw_dat_path;
		size_t dlen = std::min(datCur.size(), sizeof(datBuf) - 1);
		if (dlen > 0) memcpy(datBuf, datCur.c_str(), dlen);
		datBuf[dlen] = '\0';
		if (datBuf[0] != '\0') datVal = ValidateDatPath(datBuf);

		std::string folCur;
		// For cloud modes, prefer the actual cache dir next to the executable
		if (GuiGlobalConstants::storage_mode != "local")
			folCur = GuiGlobalConstants::GetMatchCacheDir();
		if (folCur.empty()) folCur = GuiGlobalConstants::saved_match_data_folder_path;
		if (folCur.empty()) folCur = SetupConfig::match_data_folder;
		size_t flen = std::min(folCur.size(), sizeof(folderBuf) - 1);
		if (flen > 0) memcpy(folderBuf, folCur.c_str(), flen);
		folderBuf[flen] = '\0';
		if (folderBuf[0] != '\0') folderVal = ValidateMatchFolder(folderBuf);

		datSaved = false;
		folderSaved = false;
	}

	ImGui::SetNextWindowSize(ImVec2(580, 860), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Settings", &s_settingsOpen, ImGuiWindowFlags_NoCollapse))
	{
		ImGui::End();
		return;
	}

	// ──── Data Source Section ────────────────────────────────────
	ImGui::SeparatorText("Data Source");

	const std::string& mode = GuiGlobalConstants::storage_mode;

	// Mode selection
	int modeChoice = (mode == "full_cache") ? 1 : (mode == "online_only") ? 2 : 0;
	int prevChoice = modeChoice;

	if (ImGui::RadioButton("Local", &modeChoice, 0)){}
#if GWO_CLOUD_ENABLED
	ImGui::SameLine(0, 16);
	if (ImGui::RadioButton("Cloud + Local Cache", &modeChoice, 1)){}
	ImGui::SameLine(0, 16);
	if (ImGui::RadioButton("Cloud Only", &modeChoice, 2)){}
#endif

	// Description text for each mode
	switch (modeChoice)
	{
	case 0:
		ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.f),
			"Load matches from a local folder only.");
		break;
	case 1:
		ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.f),
			"Download all matches from cloud storage and keep a local copy.");
		break;
	case 2:
		ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.f),
			"Stream matches on demand from cloud storage. No permanent local copy.");
		break;
	}

	if (modeChoice != prevChoice)
	{
		switch (modeChoice)
		{
		case 0: GuiGlobalConstants::storage_mode = "local";       break;
		case 1: GuiGlobalConstants::storage_mode = "full_cache";  break;
		case 2: GuiGlobalConstants::storage_mode = "online_only"; break;
		}

		// Auto-set cache dir for cloud modes
		if (modeChoice > 0)
		{
			std::string cacheDir = GuiGlobalConstants::GetMatchCacheDir();
			std::filesystem::create_directories(cacheDir);
			SetupConfig::match_data_folder = cacheDir;
			GuiGlobalConstants::saved_match_data_folder_path = cacheDir;
		}

		SetupConfig::storage_mode = GuiGlobalConstants::storage_mode;
		SetupConfig::Save();
		GuiGlobalConstants::SaveSettings();
		s_storageNeedsRestart = true;
	}

	// Sync status (shown for cloud modes)
	if (s_syncEnginePtr && modeChoice > 0)
	{
		ImGui::Spacing();
		auto syncState = s_syncEnginePtr->GetState();
		ImGui::Text("Sync:");
		ImGui::SameLine();
		switch (syncState)
		{
		case SyncEngine::State::Idle:
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.f), "Idle");
			break;
		case SyncEngine::State::FetchingIndex:
			ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.3f, 1.f), "Fetching index...");
			break;
		case SyncEngine::State::Downloading:
		{
			int dl = s_syncEnginePtr->GetDownloadedCount();
			int total = s_syncEnginePtr->GetTotalToDownload();
			float progress = s_syncEnginePtr->GetProgress();
			ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.3f, 1.f), "Downloading %d / %d", dl, total);
			ImGui::ProgressBar(progress, ImVec2(-1.f, 14.f));
			break;
		}
		case SyncEngine::State::Complete:
		{
			int newCount = s_syncEnginePtr->GetNewMatchCount();
			if (newCount > 0)
				ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 1.f), "Synced (%d new)", newCount);
			else
				ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 1.f), "Up to date");
			break;
		}
		case SyncEngine::State::Error:
		{
			ImGui::TextColored(ImVec4(0.88f, 0.47f, 0.19f, 1.f), "Error");
			std::string err = s_syncEnginePtr->GetLastError();
			if (!err.empty())
			{
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
				ImGui::TextColored(ImVec4(0.7f, 0.4f, 0.4f, 1.f), "%s", err.c_str());
				ImGui::PopTextWrapPos();
			}
			break;
		}
		}
	}

	// Restart notice
	if (s_storageNeedsRestart)
	{
		ImGui::Spacing();
		ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.3f, 1.f),
		                   "Restart the application to apply changes.");
	}

	// ──── File Paths Section ────────────────────────────────────
	ImGui::Spacing();
	ImGui::SeparatorText("File Paths");

	float contentW = ImGui::GetContentRegionAvail().x;
	float browseW = 80.f;
	float saveW = 70.f;
	float inputW = contentW - browseW - saveW - 20.f;

	// -- Guild Wars DAT File --
	ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "Guild Wars DAT File");
	ImGui::Dummy(ImVec2(0, 4.f));

	{
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.4f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.15f));

		ImGui::SetNextItemWidth(inputW);
		if (ImGui::InputText("##settingsdatpath", datBuf, sizeof(datBuf)))
		{
			datVal = ValidateDatPath(datBuf);
			autoDetectMsg.clear();
		}

		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(2);

		ImGui::SameLine(0.f, 4.f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
		if (ImGui::Button("Browse##dat", ImVec2(browseW, 0)))
		{
			std::string initial = "C:\\";
			if (datBuf[0] != '\0')
			{
				auto parent = std::filesystem::path(datBuf).parent_path();
				if (std::filesystem::exists(parent))
					initial = parent.string();
			}
			ImGuiFileDialog::Instance()->OpenDialog("SettingsChooseGwDat", "Select Gw.dat",
				".dat", initial + "\\.");
			datDialogOpen = true;
		}
		ImGui::PopStyleVar();

		ImGui::SameLine(0.f, 4.f);
		{
			bool canSave = (datVal == DatValidation::Valid);
			if (!canSave) ImGui::BeginDisabled();
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
			if (ImGui::Button("Save##dat", ImVec2(saveW, 0)) && canSave)
			{
				std::string p(datBuf);
				SetupConfig::dat_file_path = p;
				SetupConfig::Save();
				GuiGlobalConstants::saved_gw_dat_path = p;
				GuiGlobalConstants::SaveSettings();
				std::wstring wp(p.begin(), p.end());
				gw_dat_path = wp;
				gw_dat_path_set = true;
				datSaved = true;
				datSaveTime = Clock::now();
			}
			ImGui::PopStyleVar();
			if (!canSave) ImGui::EndDisabled();
		}
	}

	// DAT validation feedback
	{
		switch (datVal)
		{
		case DatValidation::Valid:
			ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 1.f), "OK - DAT file found");
			break;
		case DatValidation::NotFound:
			ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f), "X  File not found at this path");
			break;
		case DatValidation::WrongType:
			ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f),
				"X  This does not appear to be a Guild Wars DAT file");
			break;
		case DatValidation::TooSmall:
			ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f),
				"X  File too small -- is this the correct file?");
			break;
		default:
			ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
			break;
		}

		if (datSaved)
		{
			float elapsed = std::chrono::duration<float>(Clock::now() - datSaveTime).count();
			if (elapsed < 2.f)
			{
				float alpha = std::max(0.f, 1.f - (elapsed - 1.5f) / 0.5f);
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, alpha), "Saved");
			}
			else
				datSaved = false;
		}

		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.10f));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.83f, 0.63f, 0.13f, 1.f));
		if (ImGui::SmallButton("[ Auto-detect ]##settings"))
		{
			std::string found = TryAutoDetectDat();
			if (!found.empty())
			{
				size_t len = std::min(found.size(), sizeof(datBuf) - 1);
				memcpy(datBuf, found.c_str(), len);
				datBuf[len] = '\0';
				datVal = ValidateDatPath(datBuf);
				autoDetectMsg.clear();
			}
			else
				autoDetectMsg = "Could not auto-detect. Please browse manually.";
		}
		ImGui::PopStyleColor(4);
		if (!autoDetectMsg.empty())
			ImGui::TextColored(ImVec4(0.88f, 0.65f, 0.20f, 1.f), "%s", autoDetectMsg.c_str());
	}

	ImGui::Dummy(ImVec2(0, 12.f));

	// -- Match Data Folder --
	ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "Match Data Folder");
	ImGui::Dummy(ImVec2(0, 4.f));

	{
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.4f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.15f));

		ImGui::SetNextItemWidth(inputW);
		if (ImGui::InputText("##settingsfolderpath", folderBuf, sizeof(folderBuf)))
			folderVal = ValidateMatchFolder(folderBuf);

		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(2);

		ImGui::SameLine(0.f, 4.f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
		if (ImGui::Button("Browse##folder", ImVec2(browseW, 0)))
		{
			std::string initial = "C:\\";
			if (folderBuf[0] != '\0')
			{
				std::filesystem::path p(folderBuf);
				if (std::filesystem::exists(p) && std::filesystem::is_directory(p))
					initial = p.string();
			}
			ImGuiFileDialog::Instance()->OpenDialog("SettingsChooseMatchFolder",
				"Select Match Data Folder", nullptr, initial + "\\.");
			folderDialogOpen = true;
		}
		ImGui::PopStyleVar();

		ImGui::SameLine(0.f, 4.f);
		{
			bool canSave = (folderVal == FolderValidation::Valid ||
			                folderVal == FolderValidation::ValidEmpty);
			if (!canSave) ImGui::BeginDisabled();
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
			if (ImGui::Button("Save##folder", ImVec2(saveW, 0)) && canSave)
			{
				std::string p(folderBuf);
				SetupConfig::match_data_folder = p;
				SetupConfig::Save();
				GuiGlobalConstants::saved_match_data_folder_path = p;
				GuiGlobalConstants::SaveSettings();
				if (s_folderWatcherPtr)
					s_folderWatcherPtr->Restart(p);
				folderSaved = true;
				folderSaveTime = Clock::now();
			}
			ImGui::PopStyleVar();
			if (!canSave) ImGui::EndDisabled();
		}
	}

	// Folder validation feedback
	{
		switch (folderVal)
		{
		case FolderValidation::Valid:
			ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 1.f), "OK - Folder found");
			break;
		case FolderValidation::ValidEmpty:
			ImGui::TextColored(ImVec4(0.88f, 0.47f, 0.19f, 1.f),
				"No match files found. You can still save -- files can be added later.");
			break;
		case FolderValidation::NotFound:
			ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f), "X  Folder not found at this path");
			break;
		case FolderValidation::NotFolder:
			ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f), "X  This path is not a folder");
			break;
		case FolderValidation::NotReadable:
			ImGui::TextColored(ImVec4(0.8f, 0.19f, 0.19f, 1.f),
				"X  Cannot read this folder -- check permissions");
			break;
		default:
			ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
			break;
		}

		if (folderSaved)
		{
			float elapsed = std::chrono::duration<float>(Clock::now() - folderSaveTime).count();
			if (elapsed < 2.f)
			{
				float alpha = std::max(0.f, 1.f - (elapsed - 1.5f) / 0.5f);
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, alpha), "Saved");
			}
			else
				folderSaved = false;
		}
	}

	// ──── Font Section ──────────────────────────────────────────
	ImGui::Spacing();
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

	// ──── Keybindings Section ──────────────────────────────────────
	ImGui::Spacing();
	ImGui::SeparatorText("Keybindings");

	static ReplayHotkeys editingKeys;
	static bool keysNeedInit = true;
	if (keysNeedInit)
	{
		editingKeys = ReplayHotkeys::Get();
		keysNeedInit = false;
	}

	ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "Replay Transport");
	ImGui::Dummy(ImVec2(0, 2.f));
	HotkeyInput("Rewind 5 seconds",    &editingKeys.rewind5s);
	HotkeyInput("Forward 5 seconds",   &editingKeys.forward5s);
	HotkeyInput("Play / Pause",        &editingKeys.playPause);

	ImGui::Spacing();
	ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "Overlay Toggles");
	ImGui::Dummy(ImVec2(0, 2.f));
	HotkeyInput("Range Rings",         &editingKeys.toggleRangeRings);
	HotkeyInput("Morale Panel",        &editingKeys.toggleMoralePanel);
	HotkeyInput("Event Timeline",      &editingKeys.toggleEventTimeline);
	HotkeyInput("Lord Damage Panel",   &editingKeys.toggleLordDamage);
	HotkeyInput("Heatmap",             &editingKeys.toggleHeatmap);
	HotkeyInput("Piano Roll",          &editingKeys.togglePianoRoll);

	ImGui::Spacing();
	ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "Camera & View");
	ImGui::Dummy(ImVec2(0, 2.f));
	HotkeyInput("Auto Camera",         &editingKeys.toggleAutoCamera);
	HotkeyInput("Fog of War",          &editingKeys.toggleFogOfWar);
	HotkeyInput("Top View",            &editingKeys.toggleTopView);
	HotkeyInput("Exit Follow Mode",    &editingKeys.exitFollowMode);

	ImGui::Spacing();
	ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "Camera Movement");
	ImGui::Dummy(ImVec2(0, 2.f));
	HotkeyInput("Move Forward",        &editingKeys.camForward);
	HotkeyInput("Move Backward",       &editingKeys.camBackward);
	HotkeyInput("Strafe Left",         &editingKeys.camStrafeLeft);
	HotkeyInput("Strafe Right",        &editingKeys.camStrafeRight);

	ImGui::Spacing();
	ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "Camera Options");
	ImGui::Dummy(ImVec2(0, 2.f));
	ImGui::Checkbox("Invert Mouse X (horizontal)", &editingKeys.invertMouseX);
	ImGui::Checkbox("Invert Mouse Y (vertical)",   &editingKeys.invertMouseY);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Duplicate key warning
	{
		int allKeys[] = {
			editingKeys.rewind5s, editingKeys.forward5s, editingKeys.playPause,
			editingKeys.toggleRangeRings, editingKeys.toggleMoralePanel,
			editingKeys.toggleEventTimeline, editingKeys.toggleLordDamage,
			editingKeys.toggleAutoCamera, editingKeys.toggleFogOfWar,
			editingKeys.toggleTopView, editingKeys.togglePianoRoll,
			editingKeys.toggleHeatmap, editingKeys.exitFollowMode,
			editingKeys.camForward, editingKeys.camBackward,
			editingKeys.camStrafeLeft, editingKeys.camStrafeRight
		};
		constexpr int count = sizeof(allKeys) / sizeof(allKeys[0]);
		bool hasDupe = false;
		for (int i = 0; i < count && !hasDupe; i++)
			for (int j = i + 1; j < count && !hasDupe; j++)
				if (allKeys[i] == allKeys[j]) hasDupe = true;
		if (hasDupe)
			ImGui::TextColored(ImVec4(0.88f, 0.47f, 0.19f, 1.f),
				"Warning: Duplicate key bindings detected.");
	}

	static bool keySaved = false;
	static std::chrono::steady_clock::time_point keySaveTime;

	if (ImGui::Button("Save Keybindings", ImVec2(150, 0)))
	{
		ReplayHotkeys::Get() = editingKeys;
		ReplayHotkeys::Get().Save();
		keySaved = true;
		keySaveTime = std::chrono::steady_clock::now();
	}
	ImGui::SameLine();
	if (ImGui::Button("Reset to Defaults", ImVec2(150, 0)))
	{
		editingKeys.ResetToDefaults();
		ReplayHotkeys::Get() = editingKeys;
		ReplayHotkeys::Get().Save();
		keySaved = true;
		keySaveTime = std::chrono::steady_clock::now();
	}

	if (keySaved)
	{
		float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - keySaveTime).count();
		if (elapsed < 2.f)
		{
			float alpha = std::max(0.f, 1.f - (elapsed - 1.5f) / 0.5f);
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, alpha), "Saved");
		}
		else
			keySaved = false;
	}

	if constexpr (GuiGlobalConstants::IsDeveloperMode())
	{
		ImGui::SeparatorText("Contributor");
		ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.f),
			"Enter a contributor key to enable build naming.");

		static char contribBuf[256] = "";
		static bool contribInit = false;
		static bool contribValid = false;
		if (!contribInit)
		{
			contribInit = true;
			size_t len = std::min(GuiGlobalConstants::contributor_key.size(), sizeof(contribBuf) - 1);
			if (len > 0) memcpy(contribBuf, GuiGlobalConstants::contributor_key.c_str(), len);
			contribBuf[len] = '\0';
			if (contribBuf[0] != '\0')
				contribValid = GuiGlobalConstants::ValidateContributorKey(contribBuf);
		}

		ImGui::SetNextItemWidth(-1);
		if (ImGui::InputTextWithHint("##contributor_key", "Contributor Key", contribBuf, sizeof(contribBuf),
			ImGuiInputTextFlags_Password))
		{
			GuiGlobalConstants::contributor_key = contribBuf;
			contribValid = (contribBuf[0] != '\0') && GuiGlobalConstants::ValidateContributorKey(contribBuf);
			GuiGlobalConstants::SaveSettings();
		}

		if (contribBuf[0] == '\0')
			ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.f), "No key set. Builds are read-only.");
		else if (contribValid)
			ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.37f, 1.f), "Valid key. Build naming enabled.");
		else
			ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.f), "Invalid key.");
	}

	ImGui::End();

	// ──── File dialogs (must be drawn outside the Settings window) ──
	if (datDialogOpen && ImGuiFileDialog::Instance()->Display("SettingsChooseGwDat",
		ImGuiWindowFlags_NoCollapse, ImVec2(500, 400)))
	{
		if (ImGuiFileDialog::Instance()->IsOk())
		{
			std::string fp = ImGuiFileDialog::Instance()->GetFilePathName();
			size_t len = std::min(fp.size(), sizeof(datBuf) - 1);
			memcpy(datBuf, fp.c_str(), len);
			datBuf[len] = '\0';
			datVal = ValidateDatPath(datBuf);
			autoDetectMsg.clear();
		}
		ImGuiFileDialog::Instance()->Close();
		datDialogOpen = false;
	}

	if (folderDialogOpen && ImGuiFileDialog::Instance()->Display("SettingsChooseMatchFolder",
		ImGuiWindowFlags_NoCollapse, ImVec2(500, 400)))
	{
		if (ImGuiFileDialog::Instance()->IsOk())
		{
			std::string fp = ImGuiFileDialog::Instance()->GetCurrentPath();
			size_t len = std::min(fp.size(), sizeof(folderBuf) - 1);
			memcpy(folderBuf, fp.c_str(), len);
			folderBuf[len] = '\0';
			folderVal = ValidateMatchFolder(folderBuf);
		}
		ImGuiFileDialog::Instance()->Close();
		folderDialogOpen = false;
	}

	if (!s_settingsOpen)
	{
		initialized = false;
		keysNeedInit = true;
	}
}

void draw_ui(std::map<int, std::unique_ptr<DATManager>>& dat_managers, int& dat_manager_to_show, MapRenderer* map_renderer, PickingInfo picking_info,
	std::vector<std::vector<std::string>>& csv_data, int& FPS_target, DX::StepTimer& timer, ExtractPanelInfo& extract_panel_info, bool& msaa_changed,
	int& msaa_level_index, const std::vector<std::pair<int, int>>& msaa_levels, std::unordered_map<int, std::vector<int>>& hash_index,
	ReplayLibrary& replay_library, FolderWatcher& folder_watcher, SyncEngine* syncEngine)
{
	s_syncEnginePtr = syncEngine;
	s_folderWatcherPtr = &folder_watcher;

	// First-launch setup wizard (blocks everything until complete)
	{
		static bool s_setupDone = !SetupConfig::IsFirstLaunch();
		if (!s_setupDone)
		{
			if (draw_setup_wizard())
			{
				s_setupDone = true;

				// Trigger match folder scan now that the wizard has set the path
				if (!GuiGlobalConstants::saved_match_data_folder_path.empty())
				{
					std::filesystem::path mf(GuiGlobalConstants::saved_match_data_folder_path);
					if (std::filesystem::exists(mf) && std::filesystem::is_directory(mf))
					{
						replay_library.SetMatchDataFolder(GuiGlobalConstants::saved_match_data_folder_path);
						replay_library.ScanFolder();
						folder_watcher.Start(GuiGlobalConstants::saved_match_data_folder_path, []{});
					}
				}
			}
			else
				return;
		}
	}

	{
		static bool s_loadingScreenDone = false;
		if (!s_loadingScreenDone)
		{
			LoadingProgress lp;
			lp.dat_path_is_set = gw_dat_path_set;

			if (gw_dat_path_set && dat_managers.count(0) > 0 && dat_managers[0])
			{
				auto& dm = dat_managers[0];
				if (dm->m_initialization_state == InitializationState::Completed)
				{
					lp.dat_files_read  = dm->get_num_files();
					lp.dat_files_total = dm->get_num_files();
				}
				else if (dm->m_initialization_state == InitializationState::Started)
				{
					lp.dat_files_total = dm->get_num_files();
					lp.dat_files_read  = dm->get_num_files_type_read();
				}
			}
			if (replay_library.IsLoaded())
				lp.match_count = replay_library.GetMatchCount();

			if (draw_first_launch(lp))
				s_loadingScreenDone = true;
			else
				return;
		}
	}

	int initial_dat_manager_to_show = dat_manager_to_show;

	// Main menu bar — always visible
	if (ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("Settings...")) {
				s_settingsOpen = true;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Exit")) {
				PostQuitMessage(0);
			}
			ImGui::EndMenu();
		}
		if (GuiGlobalConstants::IsDeveloperMode() && ImGui::BeginMenu("Debug")) {
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
		if (ImGui::BeginMenu("Help")) {
			if (ImGui::MenuItem("Licence & Credits"))
				s_licenceModalOpen = true;
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}

	// DAT file missing notification bar
	if (!s_settingsOpen && gw_dat_path_set &&
		!SetupConfig::dat_file_path.empty() &&
		!std::filesystem::exists(SetupConfig::dat_file_path))
	{
		ImVec2 display = ImGui::GetIO().DisplaySize;
		float barH = 28.f;
		ImGui::SetNextWindowPos(ImVec2(0, GuiGlobalConstants::menu_bar_height));
		ImGui::SetNextWindowSize(ImVec2(display.x, barH));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.55f, 0.35f, 0.05f, 0.95f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 4));
		if (ImGui::Begin("##DatMissingBar", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav))
		{
			ImGui::Text("DAT file not found at saved path.");
			ImGui::SameLine();
			if (ImGui::SmallButton("Update Path"))
				s_settingsOpen = true;
		}
		ImGui::End();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
	}

	const auto& initialization_state = dat_managers[dat_manager_to_show]->m_initialization_state;
	const auto& dat_files_read = dat_managers[dat_manager_to_show]->get_num_files_type_read();
	const auto& dat_total_files = dat_managers[dat_manager_to_show]->get_num_files();

	if (initialization_state == InitializationState::Started)
	{
		draw_dat_load_progress_bar(dat_files_read, dat_total_files);
	}

	// Check folder watcher for new match files
	if (folder_watcher.HasPendingRefresh() && replay_library.IsLoaded())
		replay_library.RescanDiff();

	// Replay browser (available regardless of DAT state)
	draw_replay_browser(replay_library);

	// Debug panels (available regardless of DAT state)
	draw_debug_match_metadata_panel(replay_library);

	// Settings window
	draw_settings_window();

	// Help menu modals
	draw_licence_modal(&s_licenceModalOpen);

	dat_manager_to_show_changed = dat_manager_to_show != initial_dat_manager_to_show;
}
