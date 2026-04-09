#include "pch.h"
#include "Net/SyncEngine.h"
#include "Net/CloudReplayProvider.h"
#include "Net/TarGzExtractor.h"
#include "Net/MatchIndex.h"
#include "Net/HttpClient.h"
#include <set>

SyncEngine::~SyncEngine()
{
    Cancel();
    if (m_thread.joinable())
        m_thread.join();
}

void SyncEngine::Start(CloudReplayProvider& provider,
                        std::shared_ptr<MatchIndex> index,
                        HttpClient& http,
                        const std::string& bucket)
{
    if (m_state.load() == State::FetchingIndex || m_state.load() == State::Downloading)
        return; // Already running

    if (m_thread.joinable())
        m_thread.join();

    m_provider = &provider;
    m_index = std::move(index);
    m_http = &http;
    m_bucket = bucket;
    m_cancelRequested.store(false);
    m_hasNewData.store(false);
    m_progress.store(0.f);
    m_downloadedCount.store(0);
    m_totalToDownload.store(0);
    m_newMatchCount.store(0);

    m_thread = std::thread(&SyncEngine::SyncThread, this);
}

void SyncEngine::Cancel()
{
    m_cancelRequested.store(true);
    if (m_provider)
        m_provider->CancelDownload();
}

std::string SyncEngine::GetStatusText() const
{
    std::lock_guard<std::mutex> lock(m_textMutex);
    return m_statusText;
}

std::string SyncEngine::GetLastError() const
{
    std::lock_guard<std::mutex> lock(m_textMutex);
    return m_lastError;
}

void SyncEngine::SyncThread()
{
    // --- Phase 1: Fetch index ---
    m_state.store(State::FetchingIndex);
    {
        std::lock_guard<std::mutex> lock(m_textMutex);
        m_statusText = "Checking for new matches...";
    }

    // Try to load cached index first as fallback
    auto cacheDir = m_provider->GetCacheDir();
    auto indexCachePath = cacheDir / "index_cache.json";
    m_index->LoadFromCache(indexCachePath);

    // Fetch fresh index from remote
    std::wstring indexPath = L"/index.json";
    if (!m_bucket.empty())
    {
        std::wstring wBucket(m_bucket.begin(), m_bucket.end());
        indexPath = L"/" + wBucket + L"/index.json";
    }
    bool fetchedRemote = m_index->FetchFromRemote(*m_http, indexPath);

    if (m_cancelRequested.load())
    {
        m_state.store(State::Idle);
        return;
    }

    if (!fetchedRemote)
    {
        if (m_index->IsLoaded())
        {
            // Remote fetch failed but we have a cached index — proceed with it
            std::lock_guard<std::mutex> lock(m_textMutex);
            m_statusText = "Offline — using cached match index";
        }
        else
        {
            // No index at all — error
            std::lock_guard<std::mutex> lock(m_textMutex);
            m_lastError = m_index->GetLastError();
            m_statusText = "Failed to load match index";
            m_state.store(State::Error);
            return;
        }
    }
    else
    {
        // Save the fresh index to cache
        m_index->SaveToCache(indexCachePath);
    }

    // Inform the provider about the index
    m_provider->SetIndex(m_index);

    // --- Phase 1b: Extract any .tar / .tar.gz archives sitting in the cache ---
    std::error_code ec;
    if (std::filesystem::exists(cacheDir, ec))
    {
        for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec))
        {
            if (m_cancelRequested.load()) break;
            if (!entry.is_regular_file()) continue;

            auto ext = entry.path().extension().string();
            auto stem = entry.path().stem();
            bool isTarGz = (ext == ".gz" && stem.extension().string() == ".tar");
            bool isTar = (ext == ".tar");

            if (!isTarGz && !isTar)
                continue;

            // Derive folder name: "match.tar.gz" -> "match", "match.tar" -> "match"
            std::string folderName = isTarGz ? stem.stem().string() : stem.string();
            auto destDir = cacheDir / folderName;

            // Skip if already extracted
            if (std::filesystem::exists(destDir / "infos.json"))
            {
                // Archive already extracted — remove the archive to save space
                std::filesystem::remove(entry.path(), ec);
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(m_textMutex);
                m_statusText = "Extracting " + folderName + "...";
            }

            // Extract to a temp dir first, then rename atomically
            auto tmpDir = cacheDir / (folderName + ".extracting");
            std::filesystem::remove_all(tmpDir, ec);

            bool extracted = isTarGz
                ? TarGz::ExtractTarGz(entry.path(), tmpDir)
                : TarGz::ExtractTar(entry.path(), tmpDir);

            if (extracted)
            {
                // The archive may contain files at root or inside a subfolder.
                // Check if infos.json ended up in tmpDir/folderName/ (nested) or tmpDir/ (flat).
                if (std::filesystem::exists(tmpDir / folderName / "infos.json"))
                {
                    // Nested: rename the inner folder to the final destination
                    std::filesystem::remove_all(destDir, ec);
                    std::filesystem::rename(tmpDir / folderName, destDir, ec);
                    std::filesystem::remove_all(tmpDir, ec);
                }
                else if (std::filesystem::exists(tmpDir / "infos.json"))
                {
                    // Flat: rename tmpDir itself
                    std::filesystem::remove_all(destDir, ec);
                    std::filesystem::rename(tmpDir, destDir, ec);
                }
                else
                {
                    // Extraction succeeded but no infos.json found — leave archive for inspection
                    std::filesystem::remove_all(tmpDir, ec);
                    continue;
                }

                // Remove the archive after successful extraction
                std::filesystem::remove(entry.path(), ec);

                // Signal the UI to pick up this match incrementally
                m_hasNewData.store(true);
            }
            else
            {
                std::filesystem::remove_all(tmpDir, ec);
            }
        }
    }

    if (m_cancelRequested.load())
    {
        m_state.store(State::Idle);
        return;
    }

    // --- Phase 2: Determine new matches ---

    // Build set of locally cached folder names
    std::set<std::string> localFolders;
    if (std::filesystem::exists(cacheDir, ec))
    {
        for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec))
        {
            if (!entry.is_directory()) continue;
            std::string name = entry.path().filename().string();
            // Skip temp directories
            if (name.size() > 12 && name.substr(name.size() - 12) == ".downloading")
                continue;
            // Only count folders that have infos.json (fully downloaded)
            if (std::filesystem::exists(entry.path() / "infos.json"))
                localFolders.insert(name);
        }
    }

    auto newEntries = m_index->GetNewEntries(localFolders);
    m_newMatchCount.store(static_cast<int>(newEntries.size()));

    if (m_cancelRequested.load())
    {
        m_state.store(State::Idle);
        return;
    }

    // --- Phase 3: Download new matches (FullCache only) ---

    if (m_provider->GetMode() == CloudReplayProvider::Mode::FullCache && !newEntries.empty())
    {
        m_state.store(State::Downloading);
        m_totalToDownload.store(static_cast<int>(newEntries.size()));
        m_downloadedCount.store(0);

        for (size_t i = 0; i < newEntries.size(); ++i)
        {
            if (m_cancelRequested.load())
            {
                m_state.store(State::Idle);
                return;
            }

            const auto& entry = *newEntries[i];

            {
                std::lock_guard<std::mutex> lock(m_textMutex);
                std::string sizeStr;
                if (entry.size_bytes > 0)
                {
                    double mb = entry.size_bytes / (1024.0 * 1024.0);
                    char buf[32];
                    snprintf(buf, sizeof(buf), " (%.1f MB)", mb);
                    sizeStr = buf;
                }
                m_statusText = "Downloading match " + std::to_string(i + 1) +
                               " of " + std::to_string(newEntries.size()) +
                               sizeStr;
            }

            auto progressFn = [this, i, total = newEntries.size()](uint64_t received, uint64_t totalBytes)
            {
                float matchProgress = (totalBytes > 0)
                    ? static_cast<float>(received) / totalBytes
                    : 0.f;
                float overall = (static_cast<float>(i) + matchProgress) / total;
                m_progress.store(overall);
            };

            auto result = m_provider->EnsureMatchAvailable(entry, progressFn);
            if (!result.success)
            {
                std::lock_guard<std::mutex> lock(m_textMutex);
                m_lastError = "Failed to download " + entry.folder + ": " + result.error;
                // Continue with remaining matches — don't abort the whole sync
            }

            m_downloadedCount.store(static_cast<int>(i + 1));
            m_progress.store(static_cast<float>(i + 1) / newEntries.size());
        }
    }
    else if (m_provider->GetMode() == CloudReplayProvider::Mode::OnlineOnly)
    {
        // OnlineOnly: no downloads, just update the index.
        // New matches will appear in the browser via GetAvailableReplays.
        std::lock_guard<std::mutex> lock(m_textMutex);
        if (newEntries.empty())
            m_statusText = "Match index up to date";
        else
            m_statusText = std::to_string(newEntries.size()) + " new match(es) available";
    }
    else
    {
        std::lock_guard<std::mutex> lock(m_textMutex);
        m_statusText = "Match index up to date";
    }

    m_progress.store(1.f);
    m_hasNewData.store(true);
    m_state.store(State::Complete);
}
