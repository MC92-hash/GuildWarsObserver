#pragma once
#include <string>
#include <filesystem>

// Two-step first-launch wizard. Returns true when setup is complete.
bool draw_setup_wizard();

// Read-only licence viewer modal (for Help menu).
void draw_licence_modal(bool* open);

// ── Path validation helpers (shared by setup wizard + settings window) ───

enum class DatValidation { None, Valid, NotFound, WrongType, TooSmall };

inline DatValidation ValidateDatPath(const std::string& path)
{
    if (path.empty()) return DatValidation::None;
    if (!std::filesystem::exists(path)) return DatValidation::NotFound;

    auto ext = std::filesystem::path(path).extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != ".dat") return DatValidation::WrongType;

    auto sz = std::filesystem::file_size(path);
    if (sz < 1'000'000'000ULL) return DatValidation::TooSmall;

    return DatValidation::Valid;
}

enum class FolderValidation { None, Valid, ValidEmpty, NotFound, NotFolder, NotReadable };

inline FolderValidation ValidateMatchFolder(const std::string& path)
{
    if (path.empty()) return FolderValidation::None;
    if (!std::filesystem::exists(path)) return FolderValidation::NotFound;
    if (!std::filesystem::is_directory(path)) return FolderValidation::NotFolder;

    try {
        bool hasFiles = false;
        for (auto& entry : std::filesystem::directory_iterator(path))
        {
            (void)entry;
            hasFiles = true;
            break;
        }
        return hasFiles ? FolderValidation::Valid : FolderValidation::ValidEmpty;
    }
    catch (...) {
        return FolderValidation::NotReadable;
    }
}

inline std::string TryAutoDetectDat()
{
    static const char* candidates[] = {
        "C:\\Program Files\\Guild Wars\\Gw.dat",
        "C:\\Program Files (x86)\\Guild Wars\\Gw.dat",
        "C:\\Games\\Guild Wars\\Gw.dat",
    };
    for (const char* c : candidates)
    {
        if (std::filesystem::exists(c))
            return c;
    }
    const char* pf86 = std::getenv("ProgramFiles(x86)");
    if (pf86)
    {
        std::string steam = std::string(pf86) + "\\Steam\\steamapps\\common\\Guild Wars\\Gw.dat";
        if (std::filesystem::exists(steam))
            return steam;
    }
    return "";
}
