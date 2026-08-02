#include "pch.h"
#include "SetupConfig.h"
#include "GuiGlobalConstants.h"
#include <fstream>
#include <filesystem>
#include <shlobj.h>

// Keep the path in wide form throughout: narrowing wchar_t -> char one code
// unit at a time mangles any non-ASCII profile name, which then produced an
// invalid path that made create_directories throw.
std::filesystem::path SetupConfig::GetConfigDir()
{
    wchar_t* appData = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData)
        dir = std::filesystem::path(appData) / L"GWObserver";
    if (appData)
        CoTaskMemFree(appData);
    return dir;
}

std::filesystem::path SetupConfig::GetConfigPath()
{
    auto dir = GetConfigDir();
    if (dir.empty()) return {};
    return dir / L"config.ini";
}

void SetupConfig::Load()
{
    auto path = GetConfigPath();
    if (path.empty()) return;

    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n'))
            val.pop_back();

        if (key == "first_launch_complete")
            first_launch_complete = (val == "true" || val == "1");
        else if (key == "accepted_eula")
            accepted_eula = (val == "true" || val == "1");
        else if (key == "accepted_eula_date")
            accepted_eula_date = val;
        else if (key == "dat_file_path")
            dat_file_path = val;
        else if (key == "match_data_folder")
            match_data_folder = val;
        else if (key == "storage_mode")
            storage_mode = val;
    }
    file.close();

    if (!dat_file_path.empty())
        GuiGlobalConstants::saved_gw_dat_path = dat_file_path;
    if (!match_data_folder.empty())
        GuiGlobalConstants::saved_match_data_folder_path = match_data_folder;
    if (!storage_mode.empty() && GWO_CLOUD_ENABLED)
        GuiGlobalConstants::storage_mode = storage_mode;
}

void SetupConfig::Save()
{
    auto dir = GetConfigDir();
    if (dir.empty()) return;

    // error_code overload: a failure here must not throw out of the UI frame.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    auto path = GetConfigPath();
    std::ofstream file(path);
    if (!file.is_open()) return;

    file << "[Setup]\n";
    file << "first_launch_complete=" << (first_launch_complete ? "true" : "false") << "\n";
    file << "accepted_eula=" << (accepted_eula ? "true" : "false") << "\n";
    file << "accepted_eula_date=" << accepted_eula_date << "\n";

    file << "\n[Paths]\n";
    file << "dat_file_path=" << dat_file_path << "\n";
    file << "match_data_folder=" << match_data_folder << "\n";

    file << "\n[Cloud]\n";
    file << "storage_mode=" << storage_mode << "\n";

    file.close();
}
