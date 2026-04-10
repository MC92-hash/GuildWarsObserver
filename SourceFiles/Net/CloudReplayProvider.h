#pragma once
#include "ReplayLibrary.h"
#include "Net/HttpClient.h"
#include "Net/MatchIndex.h"
#include <filesystem>
#include <mutex>
#include <map>
#include <chrono>

class CloudReplayProvider : public IReplayProvider
{
public:
    enum class Mode { FullCache, OnlineOnly };

    CloudReplayProvider();

    void Configure(Mode mode,
                   const std::filesystem::path& cachePath,
                   const std::wstring& s3Host,
                   bool useTls = true);

    // Set the S3 bucket name — prepended to all request paths.
    void SetBucket(const std::string& bucket);

    // Set the signing function for S3 auth on this provider's HttpClient.
    void SetSigningFunction(HttpClient::SigningFn fn);

    void SetIndex(std::shared_ptr<MatchIndex> index);

    // IReplayProvider — returns MatchMeta for all known matches.
    // For cached matches, parses infos.json fully.
    // For remote-only matches (OnlineOnly mode), returns metadata from the index.
    std::vector<MatchMeta> GetAvailableReplays() override;

    // Download a specific match to the local cache.
    // Fetches files.json from the match folder to discover files, then downloads them all.
    // Blocks until complete or cancelled. Thread-safe.
    struct DownloadResult
    {
        std::filesystem::path localPath;
        std::string error;
        bool success = false;
    };
    DownloadResult EnsureMatchAvailable(const RemoteMatchEntry& entry,
                                        HttpClient::ProgressFn progress = nullptr);

    bool IsMatchCached(const std::string& folder) const;

    // Cancel an in-progress download.
    void CancelDownload();

    // Get the local cache path for a match folder.
    std::filesystem::path GetCachePath(const std::string& folder) const;

    Mode GetMode() const { return m_mode; }
    const std::filesystem::path& GetCacheDir() const { return m_cachePath; }

private:
    // Build a MatchMeta from the rich index entry (date, map, guilds, etc.).
    static MatchMeta BuildMetaFromIndex(const RemoteMatchEntry& entry,
                                        const std::filesystem::path& cachePath);

    // LRU eviction for OnlineOnly mode.
    void TouchMatch(const std::string& folder);
    void EvictIfNeeded();

public:
    // Time-based cache retention: delete matches not played within retentionDays.
    // Call on startup before SyncEngine starts.
    void EvictExpired(int retentionDays = 30);

    // Write/read the .last_played marker for retention tracking.
    static void WriteLastPlayed(const std::filesystem::path& matchDir);

private:
    static std::chrono::system_clock::time_point ReadLastPlayed(const std::filesystem::path& matchDir);

    Mode m_mode = Mode::OnlineOnly;
    std::filesystem::path m_cachePath;
    std::shared_ptr<MatchIndex> m_index;
    HttpClient m_http;
    std::string m_bucket;
    mutable std::mutex m_mutex;

    // LRU tracking for OnlineOnly eviction
    std::map<std::string, std::chrono::steady_clock::time_point> m_accessTimes;
    static constexpr uint64_t kDefaultMaxCacheBytes = 2ULL * 1024 * 1024 * 1024; // 2 GB
    uint64_t m_maxCacheBytes = kDefaultMaxCacheBytes;
};
