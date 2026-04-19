#include "pch.h"
#include "CustomFileBrowser.h"
#include "imgui.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// ─── Singleton ──────────────────────────────────────────────────────────────

CustomFileBrowser& CustomFileBrowser::Instance()
{
	static CustomFileBrowser inst;
	return inst;
}

// ─── Helpers ────────────────────────────────────────────────────────────────

std::string CustomFileBrowser::NormForCompare(const std::string& p)
{
	std::string r = p;
	for (auto& c : r)
	{
		if (c == '/') c = '\\';
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (!r.empty() && r.back() != '\\')
		r += '\\';
	return r;
}

std::string CustomFileBrowser::SizeStr(uintmax_t bytes)
{
	if (bytes < 1024)
		return std::to_string(bytes) + " B";
	if (bytes < 1024ULL * 1024)
		return std::format("{:.1f} KB", bytes / 1024.0);
	if (bytes < 1024ULL * 1024 * 1024)
		return std::format("{:.1f} MB", bytes / (1024.0 * 1024));
	return std::format("{:.2f} GB", bytes / (1024.0 * 1024 * 1024));
}

// ─── Platform init ──────────────────────────────────────────────────────────

void CustomFileBrowser::InitPlatform()
{
	if (m_platformReady) return;
	m_platformReady = true;

	// Enumerate drives
	m_drives.clear();
	DWORD mask = GetLogicalDrives();
	for (int i = 0; i < 26; i++)
	{
		if (mask & (1 << i))
		{
			char letter = 'A' + static_cast<char>(i);
			m_drives.push_back(std::string(1, letter) + ":\\");
		}
	}

	// Quick-access bookmarks
	m_bookmarks.clear();
	struct { const char* label; KNOWNFOLDERID fid; } known[] = {
		{ "Desktop",   FOLDERID_Desktop },
		{ "Documents", FOLDERID_Documents },
		{ "Downloads", FOLDERID_Downloads },
	};
	for (auto& k : known)
	{
		wchar_t* wp = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(k.fid, 0, nullptr, &wp)))
		{
			std::wstring wide(wp);
			CoTaskMemFree(wp);
			std::string narrow(wide.begin(), wide.end());
			if (fs::exists(narrow) && fs::is_directory(narrow))
				m_bookmarks.push_back({ k.label, narrow });
		}
	}
}

// ─── Open / Close ───────────────────────────────────────────────────────────

void CustomFileBrowser::Open(const char* dialogId, const char* title, Mode mode,
                             const char* filterExt, const std::string& initialPath)
{
	m_open = true;
	m_confirmed = false;
	m_justOpened = true;
	m_activeId = dialogId ? dialogId : "";
	m_title = title ? title : "Browse";
	m_mode = mode;
	m_filterExt = filterExt ? filterExt : "";
	m_selectedIdx = -1;
	m_searchBuf[0] = '\0';
	m_selectedPath.clear();
	m_treeCache.clear();

	for (auto& c : m_filterExt)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

	InitPlatform();

	// Resolve starting directory
	std::string startDir = initialPath;
	if (!startDir.empty())
	{
		try
		{
			fs::path p(startDir);
			if (fs::exists(p))
			{
				if (!fs::is_directory(p))
					startDir = p.parent_path().string();
			}
			else if (p.has_parent_path() && fs::exists(p.parent_path()))
				startDir = p.parent_path().string();
			else
				startDir.clear();
		}
		catch (...) { startDir.clear(); }
	}

	if (startDir.empty())
	{
		wchar_t* profile = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)))
		{
			std::wstring w(profile);
			CoTaskMemFree(profile);
			startDir.assign(w.begin(), w.end());
		}
		else
			startDir = "C:\\";
	}

	NavigateTo(startDir);
}

void CustomFileBrowser::Close()
{
	m_open = false;
	m_activeId.clear();
}

// ─── Navigation ─────────────────────────────────────────────────────────────

void CustomFileBrowser::NavigateTo(const std::string& path)
{
	try
	{
		fs::path p(path);
		if (!fs::exists(p) || !fs::is_directory(p))
			return;
		m_currentDir = fs::canonical(p).string();
	}
	catch (...)
	{
		m_currentDir = path;
	}

	m_selectedIdx = -1;
	m_selectedPath = (m_mode == Mode::SelectFolder) ? m_currentDir : "";

	Refresh();
}

void CustomFileBrowser::Refresh()
{
	m_entries.clear();

	if (m_currentDir.empty()) return;

	// Parent directory entry
	fs::path cur(m_currentDir);
	if (cur.has_parent_path() && cur.parent_path() != cur)
	{
		Entry e;
		e.name = "..";
		e.fullPath = cur.parent_path().string();
		e.isDir = true;
		m_entries.push_back(std::move(e));
	}

	std::vector<Entry> dirs, files;
	try
	{
		for (auto& it : fs::directory_iterator(m_currentDir,
		     fs::directory_options::skip_permission_denied))
		{
			DWORD attrs = GetFileAttributesW(it.path().wstring().c_str());
			if (attrs != INVALID_FILE_ATTRIBUTES &&
			    (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
				continue;

			Entry e;
			e.name = it.path().filename().string();
			e.fullPath = it.path().string();
			e.isDir = it.is_directory();
			if (!e.isDir)
			{
				try { e.size = it.file_size(); }
				catch (...) { e.size = 0; }
			}

			if (e.isDir)
				dirs.push_back(std::move(e));
			else
				files.push_back(std::move(e));
		}
	}
	catch (...) {}

	auto cmpCI = [](const Entry& a, const Entry& b)
	{
		std::string an = a.name, bn = b.name;
		for (auto& c : an) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		for (auto& c : bn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return an < bn;
	};
	std::sort(dirs.begin(), dirs.end(), cmpCI);
	std::sort(files.begin(), files.end(), cmpCI);

	m_entries.insert(m_entries.end(), dirs.begin(), dirs.end());
	m_entries.insert(m_entries.end(), files.begin(), files.end());
}

std::vector<std::string> CustomFileBrowser::GetSubfolders(const std::string& dir)
{
	auto it = m_treeCache.find(dir);
	if (it != m_treeCache.end())
		return it->second;

	std::vector<std::string> result;
	try
	{
		for (auto& entry : fs::directory_iterator(dir,
		     fs::directory_options::skip_permission_denied))
		{
			if (!entry.is_directory()) continue;

			DWORD attrs = GetFileAttributesW(entry.path().wstring().c_str());
			if (attrs != INVALID_FILE_ATTRIBUTES &&
			    (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
				continue;

			result.push_back(entry.path().string());
			if (result.size() >= 200) break;
		}
	}
	catch (...) {}

	std::sort(result.begin(), result.end(),
		[](const std::string& a, const std::string& b)
		{
			std::string an = fs::path(a).filename().string();
			std::string bn = fs::path(b).filename().string();
			for (auto& c : an) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			for (auto& c : bn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return an < bn;
		});

	m_treeCache[dir] = result;
	return result;
}

// ─── Drawing: Breadcrumbs ───────────────────────────────────────────────────

void CustomFileBrowser::DrawBreadcrumbs(float /*w*/)
{
	fs::path p(m_currentDir);

	struct Crumb { std::string label; std::string fullPath; };
	std::vector<Crumb> crumbs;

	if (p.has_root_name())
		crumbs.push_back({ p.root_name().string(), p.root_path().string() });

	fs::path accum = p.root_path();
	for (const auto& comp : p.relative_path())
	{
		std::string part = comp.string();
		if (part.empty() || part == "\\" || part == "/") continue;
		accum /= comp;
		crumbs.push_back({ part, accum.string() });
	}

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.08f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.12f));

	for (size_t i = 0; i < crumbs.size(); i++)
	{
		if (i > 0)
		{
			ImGui::SameLine(0.f, 2.f);
			ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.25f), ">");
			ImGui::SameLine(0.f, 2.f);
		}

		bool isLast = (i == crumbs.size() - 1);
		ImVec4 col = isLast ? ImVec4(0.83f, 0.63f, 0.13f, 1.f)
		                    : ImVec4(1.f, 1.f, 1.f, 0.60f);

		ImGui::PushStyleColor(ImGuiCol_Text, col);
		std::string id = crumbs[i].label + "##crumb" + std::to_string(i);
		if (ImGui::SmallButton(id.c_str()))
			NavigateTo(crumbs[i].fullPath);
		ImGui::PopStyleColor();
	}

	ImGui::PopStyleColor(3);

	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.f, 1.f, 1.f, 0.06f));
	ImGui::Separator();
	ImGui::PopStyleColor();
	ImGui::Spacing();
}

// ─── Drawing: Sidebar (Quick Access + Drive Tree) ───────────────────────────

void CustomFileBrowser::DrawSidebar(float w, float h)
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.050f, 0.065f, 1.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));

	if (ImGui::BeginChild("##Sidebar", ImVec2(w, h), true))
	{
		std::string normCur = NormForCompare(m_currentDir);

		// Quick access bookmarks
		ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 0.75f), "QUICK ACCESS");
		ImGui::Dummy(ImVec2(0, 2.f));

		for (auto& bm : m_bookmarks)
		{
			bool active = (NormForCompare(bm.path) == normCur);
			if (active)
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.83f, 0.63f, 0.13f, 1.f));

			if (ImGui::Selectable(bm.label.c_str(), active))
				NavigateTo(bm.path);

			if (active)
				ImGui::PopStyleColor();
		}

		ImGui::Dummy(ImVec2(0, 8.f));
		ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.f, 1.f, 1.f, 0.06f));
		ImGui::Separator();
		ImGui::PopStyleColor();
		ImGui::Dummy(ImVec2(0, 4.f));

		// Drive tree
		ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 0.75f), "DRIVES");
		ImGui::Dummy(ImVec2(0, 2.f));

		for (auto& drv : m_drives)
			DrawTree(drv, drv, 0);
	}
	ImGui::EndChild();

	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
}

// ─── Drawing: Recursive folder tree ─────────────────────────────────────────

void CustomFileBrowser::DrawTree(const std::string& dirPath, const std::string& label, int depth)
{
	if (depth > 5) return;

	std::string normDir = NormForCompare(m_currentDir);
	std::string normThis = NormForCompare(dirPath);

	bool isCurrent = (normDir == normThis);
	bool isAncestor = !isCurrent && normDir.starts_with(normThis);

	if (isAncestor)
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);

	ImGui::PushID(dirPath.c_str());

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;

	if (isCurrent)
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.83f, 0.63f, 0.13f, 1.f));

	bool open = ImGui::TreeNodeEx(label.c_str(), flags);

	if (isCurrent)
		ImGui::PopStyleColor();

	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
		NavigateTo(dirPath);

	if (open)
	{
		auto subs = GetSubfolders(dirPath);
		for (auto& sub : subs)
		{
			std::string subLabel = fs::path(sub).filename().string();
			DrawTree(sub, subLabel, depth + 1);
		}
		ImGui::TreePop();
	}

	ImGui::PopID();
}

// ─── Drawing: File list ─────────────────────────────────────────────────────

void CustomFileBrowser::DrawFiles(float w, float h)
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.040f, 0.058f, 0.076f, 1.f));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));

	if (ImGui::BeginChild("##FileList", ImVec2(w, h), true))
	{
		// Search / filter bar
		{
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.3f));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			ImGui::InputTextWithHint("##filesearch", "Filter files...", m_searchBuf, sizeof(m_searchBuf));
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
		}

		if (!m_filterExt.empty())
		{
			ImGui::SameLine();
			// Show as a hint on the same line after filter if there's room; otherwise below
			ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 0.50f), "(%s)", m_filterExt.c_str());
		}

		ImGui::Dummy(ImVec2(0, 2.f));

		// Build lowercase search filter
		std::string filterLow;
		if (m_searchBuf[0] != '\0')
		{
			filterLow = m_searchBuf;
			for (auto& c : filterLow)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}

		ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
		                         ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;

		float tableH = ImGui::GetContentRegionAvail().y;
		if (ImGui::BeginTable("##FT", 2, tflags, ImVec2(0, tableH)))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.78f);
			ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.22f);
			ImGui::TableHeadersRow();

			for (int i = 0; i < static_cast<int>(m_entries.size()); i++)
			{
				const auto& entry = m_entries[i];

				// Apply search filter (skip ".." from filter)
				if (!filterLow.empty() && entry.name != "..")
				{
					std::string nameLow = entry.name;
					for (auto& c : nameLow)
						c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
					if (nameLow.find(filterLow) == std::string::npos)
						continue;
				}

				ImGui::TableNextRow();
				ImGui::TableNextColumn();

				bool selected = (i == m_selectedIdx);

				// Check if file matches the extension filter for highlighting
				bool matchesExt = false;
				if (!entry.isDir && !m_filterExt.empty())
				{
					std::string ext = fs::path(entry.name).extension().string();
					for (auto& c : ext)
						c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
					matchesExt = (ext == m_filterExt);
				}

				// Row text color
				ImVec4 nameCol;
				if (entry.name == "..")
					nameCol = ImVec4(1.f, 1.f, 1.f, 0.50f);
				else if (entry.isDir)
					nameCol = ImVec4(1.f, 1.f, 1.f, 0.90f);
				else if (matchesExt)
					nameCol = ImVec4(0.83f, 0.63f, 0.13f, 1.f);
				else
					nameCol = ImVec4(1.f, 1.f, 1.f, 0.65f);

				ImGui::PushStyleColor(ImGuiCol_Text, nameCol);

				std::string display;
				if (entry.name == "..")
					display = ".. (Parent)";
				else if (entry.isDir)
					display = "> " + entry.name;
				else
					display = "  " + entry.name;

				std::string selId = display + "##e" + std::to_string(i);
				ImGuiSelectableFlags sf = ImGuiSelectableFlags_SpanAllColumns |
				                          ImGuiSelectableFlags_AllowDoubleClick;

				if (ImGui::Selectable(selId.c_str(), selected, sf))
				{
					m_selectedIdx = i;

					if (entry.isDir)
					{
						if (ImGui::IsMouseDoubleClicked(0))
							NavigateTo(entry.fullPath);
						else if (m_mode == Mode::SelectFolder && entry.name != "..")
							m_selectedPath = entry.fullPath;
					}
					else
					{
						if (m_mode == Mode::SelectFile)
						{
							m_selectedPath = entry.fullPath;
							if (ImGui::IsMouseDoubleClicked(0))
								m_confirmed = true;
						}
					}
				}

				ImGui::PopStyleColor();

				// Size column
				ImGui::TableNextColumn();
				if (!entry.isDir)
					ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.40f), "%s", SizeStr(entry.size).c_str());
				else if (entry.name != "..")
					ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 0.22f), "Folder");
			}

			ImGui::EndTable();
		}
	}
	ImGui::EndChild();

	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
}

// ─── Drawing: Bottom bar (path + Select/Cancel) ────────────────────────────

bool CustomFileBrowser::DrawBottom(float w)
{
	bool done = false;

	std::string displayPath = m_selectedPath.empty() ? m_currentDir : m_selectedPath;

	// Path read-only input
	float btnW = 90.f;
	float pathW = w - btnW * 2 - 24.f;

	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.3f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.10f));

	char pathBuf[512];
	size_t len = std::min(displayPath.size(), sizeof(pathBuf) - 1);
	if (len > 0) memcpy(pathBuf, displayPath.c_str(), len);
	pathBuf[len] = '\0';

	ImGui::SetNextItemWidth(pathW);
	ImGui::InputText("##selpath", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);

	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(2);

	// Select button
	ImGui::SameLine(0.f, 8.f);

	bool canSelect = false;
	if (m_mode == Mode::SelectFolder)
		canSelect = !m_currentDir.empty();
	else
		canSelect = (m_selectedIdx >= 0 && m_selectedIdx < static_cast<int>(m_entries.size()) &&
		             !m_entries[m_selectedIdx].isDir);

	if (!canSelect) ImGui::BeginDisabled();

	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.15f, 0.9f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.18f, 0.12f, 0.9f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.22f, 0.15f, 0.9f));
	ImGui::PushStyleColor(ImGuiCol_Border,
		canSelect ? ImVec4(0.83f, 0.63f, 0.13f, 0.7f) : ImVec4(0.3f, 0.3f, 0.3f, 0.3f));

	if (ImGui::Button("Select", ImVec2(btnW, 0)))
	{
		if (m_mode == Mode::SelectFolder && m_selectedPath.empty())
			m_selectedPath = m_currentDir;
		m_confirmed = true;
		done = true;
	}

	ImGui::PopStyleColor(4);
	ImGui::PopStyleVar(2);
	if (!canSelect) ImGui::EndDisabled();

	// Cancel button
	ImGui::SameLine(0.f, 8.f);

	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
	if (ImGui::Button("Cancel", ImVec2(btnW, 0)))
	{
		m_confirmed = false;
		done = true;
	}
	ImGui::PopStyleVar();

	return done;
}

// ─── Display (main entry) ───────────────────────────────────────────────────

bool CustomFileBrowser::Display(const char* dialogId)
{
	if (!m_open || m_activeId != dialogId)
		return false;

	ImVec2 display = ImGui::GetIO().DisplaySize;
	float dlgW = std::min(820.f, display.x - 60.f);
	float dlgH = std::min(580.f, display.y - 60.f);

	ImGui::SetNextWindowPos(
		ImVec2((display.x - dlgW) * 0.5f, (display.y - dlgH) * 0.5f),
		ImGuiCond_Appearing);
	ImGui::SetNextWindowSize(ImVec2(dlgW, dlgH));

	if (m_justOpened)
	{
		ImGui::SetNextWindowFocus();
		m_justOpened = false;
	}

	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.078f, 0.102f, 0.97f));
	ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.63f, 0.13f, 0.25f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

	std::string winLabel = m_title + "###" + m_activeId;
	bool windowOpen = true;
	bool done = false;

	if (ImGui::Begin(winLabel.c_str(), &windowOpen,
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize))
	{
		float contentW = ImGui::GetContentRegionAvail().x;

		DrawBreadcrumbs(contentW);

		float availH = ImGui::GetContentRegionAvail().y;
		float bottomH = 36.f;
		float mainH = availH - bottomH - 12.f;
		if (mainH < 100.f) mainH = 100.f;

		float sideW = std::min(200.f, contentW * 0.28f);

		DrawSidebar(sideW, mainH);
		ImGui::SameLine(0.f, 8.f);
		DrawFiles(contentW - sideW - 8.f, mainH);

		// Double-click confirm from file list
		if (m_confirmed)
			done = true;

		ImGui::Dummy(ImVec2(0, 4.f));

		if (DrawBottom(contentW))
			done = true;

		// Keyboard shortcuts
		if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
		    !ImGui::IsAnyItemActive())
		{
			if (ImGui::IsKeyPressed(ImGuiKey_Escape))
			{
				m_confirmed = false;
				done = true;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Backspace))
			{
				fs::path parent = fs::path(m_currentDir).parent_path();
				if (parent != fs::path(m_currentDir))
					NavigateTo(parent.string());
			}
		}
	}
	ImGui::End();

	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(2);

	if (!windowOpen)
	{
		m_confirmed = false;
		done = true;
	}

	return done;
}
