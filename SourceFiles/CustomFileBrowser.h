#pragma once
#include <string>
#include <vector>
#include <unordered_map>

class CustomFileBrowser
{
public:
	enum class Mode { SelectFile, SelectFolder };

	static CustomFileBrowser& Instance();

	void Open(const char* dialogId, const char* title, Mode mode,
	          const char* filterExt = nullptr, const std::string& initialPath = "");

	// Returns true when the dialog is complete (OK or Cancel/close).
	bool Display(const char* dialogId);

	bool IsOk() const { return m_confirmed; }
	std::string GetSelectedPath() const { return m_selectedPath; }
	void Close();

private:
	CustomFileBrowser() = default;

	bool m_open = false;
	bool m_confirmed = false;
	bool m_justOpened = false;
	std::string m_activeId;
	std::string m_title;
	Mode m_mode = Mode::SelectFile;
	std::string m_filterExt;

	std::string m_currentDir;
	std::string m_selectedPath;
	char m_searchBuf[256] = {};
	int m_selectedIdx = -1;

	struct Entry
	{
		std::string name;
		std::string fullPath;
		bool isDir = false;
		uintmax_t size = 0;
	};
	std::vector<Entry> m_entries;

	struct Bookmark { std::string label; std::string path; };
	std::vector<Bookmark> m_bookmarks;
	std::vector<std::string> m_drives;
	bool m_platformReady = false;

	std::unordered_map<std::string, std::vector<std::string>> m_treeCache;

	void InitPlatform();
	void NavigateTo(const std::string& path);
	void Refresh();
	std::vector<std::string> GetSubfolders(const std::string& dir);

	void DrawBreadcrumbs(float w);
	void DrawSidebar(float w, float h);
	void DrawTree(const std::string& dirPath, const std::string& label, int depth);
	void DrawFiles(float w, float h);
	bool DrawBottom(float w);

	static std::string NormForCompare(const std::string& p);
	static std::string SizeStr(uintmax_t bytes);
};
