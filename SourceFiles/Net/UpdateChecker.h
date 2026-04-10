#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

class UpdateChecker
{
public:
    UpdateChecker() = default;
    ~UpdateChecker();

    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    // Start background check against GitHub releases.
    // currentVersion: e.g. "1.0.2" (no 'v' prefix)
    // repo: e.g. "MC92-hash/gwobserver"
    void Check(const std::string& currentVersion,
               const std::string& repo = "MC92-hash/gwobserver");

    bool IsComplete() const { return m_complete.load(); }
    bool HasUpdate() const { return m_hasUpdate.load(); }

    std::string GetLatestVersion() const;
    std::string GetReleaseUrl() const;
    std::string GetCurrentVersion() const;

private:
    void CheckThread();

    static bool IsNewer(const std::string& latest, const std::string& current);

    std::thread m_thread;
    std::atomic<bool> m_complete{false};
    std::atomic<bool> m_hasUpdate{false};

    std::string m_currentVersion;
    std::string m_repo;
    std::string m_latestVersion;
    std::string m_releaseUrl;
    mutable std::mutex m_mutex;
};
