#include "pch.h"
#include "Net/CloudReplayProvider.h"
#include "Net/TarGzExtractor.h"
#include "json.hpp"

using json = nlohmann::json;

CloudReplayProvider::CloudReplayProvider() = default;

void CloudReplayProvider::Configure(Mode mode,
                                     const std::filesystem::path& cachePath,
                                     const std::wstring& s3Host,
                                     bool useTls)
{
    m_mode = mode;
    m_cachePath = cachePath;
    m_http.SetBaseUrl(s3Host, useTls);

    // Ensure cache directory exists
    std::error_code ec;
    std::filesystem::create_directories(m_cachePath, ec);
}

void CloudReplayProvider::SetIndex(std::shared_ptr<MatchIndex> index)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_index = std::move(index);
}

std::filesystem::path CloudReplayProvider::GetCachePath(const std::string& folder) const
{
    return m_cachePath / folder;
}

bool CloudReplayProvider::IsMatchCached(const std::string& folder) const
{
    auto matchDir = m_cachePath / folder;
    auto infosPath = matchDir / "infos.json";
    return std::filesystem::exists(infosPath);
}

MatchMeta CloudReplayProvider::BuildMetaFromIndex(const RemoteMatchEntry& entry,
                                                   const std::filesystem::path& cachePath)
{
    MatchMeta meta;
    meta.folder_name = entry.folder;
    meta.folder_path = (cachePath / entry.folder).string();
    meta.map_id = entry.map_id;
    meta.year = entry.year;
    meta.month = entry.month;
    meta.day = entry.day;
    meta.occasion = entry.occasion;
    meta.flux = entry.flux;
    meta.match_duration = entry.duration;
    meta.winner_party_id = entry.winner;

    for (const auto& [pid, gi] : entry.guilds)
    {
        GuildMeta gm;
        gm.name = gi.name;
        gm.tag = gi.tag;
        meta.guilds[pid] = std::move(gm);
    }

    meta.team_kills = entry.team_kills;
    meta.team_damage = entry.team_damage;

    for (const auto& [pid, party] : entry.parties)
    {
        PartyMeta pm;
        for (const auto& rp : party.players)
        {
            PlayerMeta p;
            p.encoded_name = rp.encoded_name;
            p.primary = rp.primary;
            p.secondary = rp.secondary;
            p.player_number = rp.player_number;
            p.used_skills = rp.used_skills;
            p.skill_template_code = rp.skill_template_code;
            p.kills = rp.kills;
            p.deaths = rp.deaths;
            p.total_damage = rp.total_damage;
            pm.players.push_back(std::move(p));
        }
        meta.parties[pid] = std::move(pm);
    }

    return meta;
}

std::vector<MatchMeta> CloudReplayProvider::GetAvailableReplays()
{
    std::vector<MatchMeta> results;

    // First, scan locally cached matches (these have full metadata from infos.json)
    if (std::filesystem::exists(m_cachePath))
    {
        LocalReplayProvider localProvider;
        localProvider.SetFolder(m_cachePath.string());
        results = localProvider.GetAvailableReplays();
    }

    // For OnlineOnly mode, also include remote-only entries with metadata from the index
    if (m_mode == Mode::OnlineOnly && m_index)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Build a set of already-loaded folder names
        std::set<std::string> loadedFolders;
        for (const auto& m : results)
            loadedFolders.insert(m.folder_name);

        for (const auto& entry : m_index->GetEntries())
        {
            if (loadedFolders.find(entry.folder) != loadedFolders.end())
                continue;

            // Not cached locally — add metadata from the index
            auto meta = BuildMetaFromIndex(entry, m_cachePath);
            meta.is_cloud_only = true;
            results.push_back(std::move(meta));
        }
    }

    return results;
}

CloudReplayProvider::DownloadResult CloudReplayProvider::EnsureMatchAvailable(
    const RemoteMatchEntry& entry,
    HttpClient::ProgressFn progress)
{
    DownloadResult result;
    auto matchDir = m_cachePath / entry.folder;

    // Already cached?
    if (IsMatchCached(entry.folder))
    {
        result.localPath = matchDir;
        result.success = true;
        TouchMatch(entry.folder);
        return result;
    }

    // Download the .tar.gz archive for this match
    std::string archivePathStr = "/matches/" + entry.folder + ".tar.gz";
    int wchars = MultiByteToWideChar(CP_UTF8, 0, archivePathStr.c_str(), -1, nullptr, 0);
    std::wstring archivePath(wchars - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, archivePathStr.c_str(), -1, archivePath.data(), wchars);

    auto downloadDir = m_cachePath / (entry.folder + ".downloading");
    std::error_code ec;
    std::filesystem::create_directories(downloadDir, ec);
    if (ec)
    {
        result.error = "Failed to create download directory: " + ec.message();
        return result;
    }

    auto archiveFile = downloadDir / "archive.tar.gz";
    auto resp = m_http.DownloadToFile(archivePath, archiveFile, progress);
    if (!resp.IsOk())
    {
        std::filesystem::remove_all(downloadDir, ec);
        result.error = "Failed to download archive: " + resp.errorMessage;
        return result;
    }

    // Extract the archive
    if (!TarGz::ExtractTarGz(archiveFile, downloadDir))
    {
        std::filesystem::remove_all(downloadDir, ec);
        result.error = "Failed to extract match archive";
        return result;
    }

    // Remove the archive file, keep only extracted content
    std::filesystem::remove(archiveFile, ec);

    // Handle nested extraction: archive may contain folder_name/infos.json
    // which results in downloadDir/folder_name/infos.json — flatten it
    auto nestedDir = downloadDir / entry.folder;
    if (std::filesystem::exists(nestedDir / "infos.json"))
    {
        // Move nested content up to downloadDir
        auto tmpFlat = m_cachePath / (entry.folder + ".flatten");
        std::filesystem::remove_all(tmpFlat, ec);
        std::filesystem::rename(nestedDir, tmpFlat, ec);
        std::filesystem::remove_all(downloadDir, ec);
        std::filesystem::rename(tmpFlat, downloadDir, ec);
    }

    // Atomic rename: downloadDir → matchDir
    std::filesystem::remove_all(matchDir, ec); // remove any stale partial
    std::filesystem::rename(downloadDir, matchDir, ec);
    if (ec)
    {
        result.error = "Failed to finalize download: " + ec.message();
        std::filesystem::remove_all(downloadDir, ec);
        return result;
    }

    result.localPath = matchDir;
    result.success = true;
    TouchMatch(entry.folder);

    // In OnlineOnly mode, evict old matches if over cache limit
    if (m_mode == Mode::OnlineOnly)
        EvictIfNeeded();

    return result;
}

void CloudReplayProvider::CancelDownload()
{
    m_http.Cancel();
}

void CloudReplayProvider::TouchMatch(const std::string& folder)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_accessTimes[folder] = std::chrono::steady_clock::now();
    WriteLastPlayed(m_cachePath / folder);
}

void CloudReplayProvider::EvictIfNeeded()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Calculate total cache size
    uint64_t totalSize = 0;
    std::map<std::string, uint64_t> folderSizes;

    std::error_code ec;
    if (!std::filesystem::exists(m_cachePath, ec))
        return;

    for (const auto& entry : std::filesystem::directory_iterator(m_cachePath, ec))
    {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();

        // Skip .downloading directories
        if (name.size() > 12 && name.substr(name.size() - 12) == ".downloading")
            continue;

        uint64_t dirSize = 0;
        for (const auto& file : std::filesystem::recursive_directory_iterator(entry.path(), ec))
        {
            if (file.is_regular_file())
                dirSize += file.file_size();
        }

        folderSizes[name] = dirSize;
        totalSize += dirSize;
    }

    // Evict LRU matches until under the limit
    while (totalSize > m_maxCacheBytes && !folderSizes.empty())
    {
        // Find the least recently accessed match
        std::string lruFolder;
        auto oldestTime = std::chrono::steady_clock::time_point::max();

        for (const auto& [folder, size] : folderSizes)
        {
            auto it = m_accessTimes.find(folder);
            auto accessTime = (it != m_accessTimes.end())
                ? it->second
                : std::chrono::steady_clock::time_point::min();

            if (accessTime < oldestTime)
            {
                oldestTime = accessTime;
                lruFolder = folder;
            }
        }

        if (lruFolder.empty())
            break;

        auto folderPath = m_cachePath / lruFolder;
        uint64_t freedSize = folderSizes[lruFolder];
        std::filesystem::remove_all(folderPath, ec);

        totalSize -= freedSize;
        folderSizes.erase(lruFolder);
        m_accessTimes.erase(lruFolder);
    }
}

void CloudReplayProvider::WriteLastPlayed(const std::filesystem::path& matchDir)
{
    auto path = matchDir / ".last_played";
    std::error_code ec;
    if (!std::filesystem::exists(matchDir, ec))
        return;

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm utc;
    gmtime_s(&utc, &time);

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec);

    std::ofstream f(path);
    if (f.is_open())
        f << buf;
}

std::chrono::system_clock::time_point CloudReplayProvider::ReadLastPlayed(
    const std::filesystem::path& matchDir)
{
    auto path = matchDir / ".last_played";
    std::error_code ec;

    // Try reading the marker file
    if (std::filesystem::exists(path, ec))
    {
        std::ifstream f(path);
        std::string line;
        if (f.is_open() && std::getline(f, line) && line.size() >= 19)
        {
            // Parse "YYYY-MM-DDTHH:MM:SSZ"
            struct tm t = {};
            if (sscanf_s(line.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
                         &t.tm_year, &t.tm_mon, &t.tm_mday,
                         &t.tm_hour, &t.tm_min, &t.tm_sec) == 6)
            {
                t.tm_year -= 1900;
                t.tm_mon -= 1;
                auto time = _mkgmtime(&t);
                if (time != -1)
                    return std::chrono::system_clock::from_time_t(time);
            }
        }
    }

    // Fallback: use filesystem last-write-time of the match folder
    if (std::filesystem::exists(matchDir, ec))
    {
        auto ftime = std::filesystem::last_write_time(matchDir, ec);
        if (!ec)
        {
            // Convert file_time to system_clock (C++20)
            auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
            return sctp;
        }
    }

    // Unknown — return epoch (will be evicted)
    return std::chrono::system_clock::time_point{};
}

void CloudReplayProvider::EvictExpired(int retentionDays)
{
    if (m_mode != Mode::OnlineOnly)
        return;

    std::error_code ec;
    if (!std::filesystem::exists(m_cachePath, ec))
        return;

    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24 * retentionDays);

    for (const auto& entry : std::filesystem::directory_iterator(m_cachePath, ec))
    {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();

        // Skip temp download directories
        if (name.size() > 12 && name.substr(name.size() - 12) == ".downloading")
            continue;

        // Skip if no infos.json (not a complete match)
        if (!std::filesystem::exists(entry.path() / "infos.json"))
            continue;

        auto lastPlayed = ReadLastPlayed(entry.path());
        if (lastPlayed < cutoff)
            std::filesystem::remove_all(entry.path(), ec);
    }
}
