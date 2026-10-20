#include "Net/MatchIndex.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;

    const auto cacheFile = std::filesystem::path(argv[1]);
    {
        std::ofstream file(cacheFile);
        if (!file.is_open())
            return 3;
        file << R"({"matches":[
            {"folder":"../escape"},
            {"folder":"nested/escape"},
            {"folder":"..\\escape"},
            {"folder":"C:\\escape"},
            {"folder":"2026-10-18 - safe match"}
        ]})";
    }

    MatchIndex index;
    if (!index.LoadFromCache(cacheFile))
        return 4;
    std::filesystem::remove(cacheFile);

    const auto& entries = index.GetEntries();
    if (entries.size() != 1)
        return 5;
    if (entries.front().folder != "2026-10-18 - safe match")
        return 6;
}
