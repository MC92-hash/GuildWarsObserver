#pragma once
#include <string>

class SetupConfig
{
public:
    inline static bool   first_launch_complete = false;
    inline static bool   accepted_eula = false;
    inline static std::string accepted_eula_date;
    inline static std::string dat_file_path;
    inline static std::string match_data_folder;

    static void Load();
    static void Save();
    static bool IsFirstLaunch() { return !first_launch_complete; }
    static std::string GetConfigDir();
    static std::string GetConfigPath();
};
