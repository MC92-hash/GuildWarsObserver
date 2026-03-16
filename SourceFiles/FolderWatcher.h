#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <functional>
#include <Windows.h>

class FolderWatcher
{
public:
    using Callback = std::function<void()>;

    FolderWatcher() = default;
    ~FolderWatcher();

    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    void Start(const std::string& folderPath, Callback onChange);
    void Stop();
    void Restart(const std::string& newPath);

    bool IsWatching() const { return m_watching.load(); }
    bool HasPendingRefresh();

private:
    void WatchThread();
    void PollFallbackThread();

    Callback m_callback;
    std::string m_folderPath;

    std::thread m_thread;
    std::atomic<bool> m_watching{false};
    std::atomic<bool> m_stopRequested{false};
    HANDLE m_cancelEvent = nullptr;

    // Debounce state (accessed from watcher thread only, fires callback on main thread via flag)
    static constexpr int kDebounceMs = 1500;
    std::atomic<bool> m_pendingRefresh{false};

    // Fallback polling
    bool m_useFallbackPolling = false;
    int m_pollFailCount = 0;
    static constexpr int kPollIntervalMs = 30000;
    static constexpr int kRetryIntervalMs = 10000;
};
