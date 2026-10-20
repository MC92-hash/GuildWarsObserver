#pragma once

#include <string>

// Remote match folder names are identifiers, not paths. Keep them to one
// Windows-safe path component before they are joined to the local cache path.
inline bool IsSafeMatchFolderName(const std::string& folder)
{
    if (folder.empty() || folder == "." || folder == "..")
        return false;

    if (folder.find_first_of("/\\<>:\"|?*") != std::string::npos ||
        folder.find('\0') != std::string::npos ||
        folder.back() == '.' || folder.back() == ' ')
    {
        return false;
    }

    for (unsigned char ch : folder)
    {
        if (ch < 0x20 || ch == 0x7f)
            return false;
    }

    return true;
}
