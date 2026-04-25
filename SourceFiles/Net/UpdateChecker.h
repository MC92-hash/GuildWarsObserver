#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <cstdint>
#include <Windows.h>

class UpdateChecker
{
public:
    enum class State { Idle, Checking, UpdateAvailable, Downloading, ReadyToInstall, Error };

    UpdateChecker() = default;
    ~UpdateChecker();

    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    // Start background check against GitHub releases.
    // currentVersion: e.g. "1.0.2" (no 'v' prefix)
    // repo: e.g. "MC92-hash/gwobserver"
    void Check(const std::string& currentVersion,
               const std::string& repo = "MC92-hash/gwobserver",
               bool userInitiated = false);

    // Download the release exe to <exe_dir>/GWObserver_update.exe.
    void StartDownload();

    // Write updater batch script, launch it, and signal the app to exit.
    bool ApplyAndRestart(HWND appWindow);

    void Cancel();
    void Dismiss();

    // Debug: simulate states for UI testing without a real release
    void DebugSimulate(State targetState);

    // Debug: copy the running exe as the "update" and set ReadyToInstall,
    // so clicking "Install & Restart" runs the real batch script swap flow.
    bool DebugFullTest();

    // --- Accessors (all thread-safe) ---

    State GetState() const { return m_state.load(); }
    bool IsComplete() const;
    bool HasUpdate() const;
    bool IsUserInitiated() const { return m_userInitiated.load(); }
    bool IsDismissed() const { return m_dismissed.load(); }

    float GetDownloadProgress() const { return m_progress.load(); }
    uint64_t GetDownloadedBytes() const { return m_bytesReceived.load(); }
    uint64_t GetTotalBytes() const { return m_bytesTotal.load(); }

    std::string GetLatestVersion() const;
    std::string GetReleaseUrl() const;
    std::string GetCurrentVersion() const;
    std::string GetReleaseNotes() const;
    std::string GetLastError() const;

private:
    void CheckThread();
    void DownloadThread();

    static bool IsNewer(const std::string& latest, const std::string& current);

    std::thread m_thread;

    std::atomic<State> m_state{State::Idle};
    std::atomic<float> m_progress{0.f};
    std::atomic<uint64_t> m_bytesReceived{0};
    std::atomic<uint64_t> m_bytesTotal{0};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<bool> m_userInitiated{false};
    std::atomic<bool> m_dismissed{false};

    // Protected by m_mutex
    std::string m_currentVersion;
    std::string m_repo;
    std::string m_latestVersion;
    std::string m_releaseUrl;
    std::string m_releaseNotes;
    std::string m_downloadUrl;   // browser_download_url for .exe asset
    std::string m_lastError;
    mutable std::mutex m_mutex;

    std::filesystem::path m_downloadedExePath;
};
