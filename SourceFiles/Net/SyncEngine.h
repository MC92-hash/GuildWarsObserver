#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

class CloudReplayProvider;
class MatchIndex;
class HttpClient;

class SyncEngine
{
public:
    enum class State { Idle, FetchingIndex, Downloading, Complete, Error };

    SyncEngine() = default;
    ~SyncEngine();

    SyncEngine(const SyncEngine&) = delete;
    SyncEngine& operator=(const SyncEngine&) = delete;

    // Start the sync process on a background thread.
    // For FullCache: fetches index, then downloads all new matches.
    // For OnlineOnly: fetches index only (new matches appear in browser immediately).
    void Start(CloudReplayProvider& provider,
               std::shared_ptr<MatchIndex> index,
               HttpClient& http);

    void Cancel();

    State GetState() const { return m_state.load(); }
    float GetProgress() const { return m_progress.load(); }
    int GetNewMatchCount() const { return m_newMatchCount.load(); }
    int GetDownloadedCount() const { return m_downloadedCount.load(); }
    int GetTotalToDownload() const { return m_totalToDownload.load(); }

    std::string GetStatusText() const;
    std::string GetLastError() const;

    // Check if new data is available and the library should rescan.
    bool HasNewData() const { return m_hasNewData.load(); }
    void AcknowledgeNewData() { m_hasNewData.store(false); }

private:
    void SyncThread();

    std::thread m_thread;
    std::atomic<State> m_state{State::Idle};
    std::atomic<float> m_progress{0.f};
    std::atomic<int> m_newMatchCount{0};
    std::atomic<int> m_downloadedCount{0};
    std::atomic<int> m_totalToDownload{0};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<bool> m_hasNewData{false};

    std::string m_statusText;
    std::string m_lastError;
    mutable std::mutex m_textMutex;

    CloudReplayProvider* m_provider = nullptr;
    std::shared_ptr<MatchIndex> m_index;
    HttpClient* m_http = nullptr;
};
