#include "pch.h"
#include "FolderWatcher.h"
#include <filesystem>

FolderWatcher::~FolderWatcher()
{
    Stop();
}

void FolderWatcher::Start(const std::string& folderPath, Callback onChange)
{
    Stop();

    if (folderPath.empty()) return;
    if (!std::filesystem::exists(folderPath) || !std::filesystem::is_directory(folderPath))
        return;

    m_folderPath = folderPath;
    m_callback = std::move(onChange);
    m_stopRequested = false;
    m_pendingRefresh = false;
    m_useFallbackPolling = false;
    m_pollFailCount = 0;

    m_cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_cancelEvent) return;

    m_watching = true;
    m_thread = std::thread(&FolderWatcher::WatchThread, this);
}

void FolderWatcher::Stop()
{
    if (!m_watching.load()) return;

    m_stopRequested = true;
    if (m_cancelEvent)
        SetEvent(m_cancelEvent);

    if (m_thread.joinable())
        m_thread.join();

    if (m_cancelEvent)
    {
        CloseHandle(m_cancelEvent);
        m_cancelEvent = nullptr;
    }

    m_watching = false;
    m_stopRequested = false;
}

void FolderWatcher::Restart(const std::string& newPath)
{
    Stop();
    Start(newPath, m_callback);
}

bool FolderWatcher::HasPendingRefresh()
{
    return m_pendingRefresh.exchange(false);
}

void FolderWatcher::WatchThread()
{
    std::wstring widePath(m_folderPath.begin(), m_folderPath.end());

    HANDLE dirHandle = CreateFileW(
        widePath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (dirHandle == INVALID_HANDLE_VALUE)
    {
        m_useFallbackPolling = true;
        PollFallbackThread();
        m_watching = false;
        return;
    }

    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent)
    {
        CloseHandle(dirHandle);
        m_watching = false;
        return;
    }

    alignas(DWORD) char buffer[4096];
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME |
                         FILE_NOTIFY_CHANGE_DIR_NAME |
                         FILE_NOTIFY_CHANGE_LAST_WRITE;

    auto lastEventTime = std::chrono::steady_clock::now();
    bool debounceActive = false;

    while (!m_stopRequested.load())
    {
        ResetEvent(overlapped.hEvent);

        BOOL ok = ReadDirectoryChangesW(
            dirHandle, buffer, sizeof(buffer), TRUE, filter,
            nullptr, &overlapped, nullptr);

        if (!ok)
        {
            // Folder may have been deleted; fall back to polling with retry
            CloseHandle(overlapped.hEvent);
            CloseHandle(dirHandle);
            m_useFallbackPolling = true;
            PollFallbackThread();
            m_watching = false;
            return;
        }

        // Wait on either: directory change, debounce timeout, or cancellation
        HANDLE handles[2] = { overlapped.hEvent, m_cancelEvent };

        while (!m_stopRequested.load())
        {
            DWORD waitMs = debounceActive
                ? static_cast<DWORD>(std::max(0LL,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        lastEventTime + std::chrono::milliseconds(kDebounceMs) -
                        std::chrono::steady_clock::now()).count()))
                : INFINITE;

            if (debounceActive && waitMs == 0)
            {
                // Debounce expired — trigger refresh
                debounceActive = false;
                m_pendingRefresh = true;
                break;
            }

            DWORD result = WaitForMultipleObjects(2, handles, FALSE, debounceActive ? waitMs : INFINITE);

            if (m_stopRequested.load())
                break;

            if (result == WAIT_OBJECT_0)
            {
                // Directory change received — reset debounce
                DWORD bytesReturned = 0;
                GetOverlappedResult(dirHandle, &overlapped, &bytesReturned, FALSE);
                lastEventTime = std::chrono::steady_clock::now();
                debounceActive = true;
                break; // Break inner loop to re-issue ReadDirectoryChangesW
            }
            else if (result == WAIT_OBJECT_0 + 1)
            {
                // Cancel event
                break;
            }
            else if (result == WAIT_TIMEOUT)
            {
                // Debounce timer expired
                debounceActive = false;
                m_pendingRefresh = true;
                break;
            }
        }
    }

    CancelIo(dirHandle);
    CloseHandle(overlapped.hEvent);
    CloseHandle(dirHandle);
    m_watching = false;
}

void FolderWatcher::PollFallbackThread()
{
    auto lastScanTime = std::filesystem::file_time_type::min();

    while (!m_stopRequested.load())
    {
        DWORD waitMs = m_useFallbackPolling
            ? static_cast<DWORD>(kPollIntervalMs)
            : static_cast<DWORD>(kRetryIntervalMs);

        if (WaitForSingleObject(m_cancelEvent, waitMs) != WAIT_TIMEOUT)
            break;

        if (!std::filesystem::exists(m_folderPath) ||
            !std::filesystem::is_directory(m_folderPath))
        {
            m_pollFailCount++;
            continue;
        }

        // Check if anything changed by looking at directory write time
        std::error_code ec;
        auto dirTime = std::filesystem::last_write_time(m_folderPath, ec);
        if (ec) { m_pollFailCount++; continue; }

        if (dirTime > lastScanTime)
        {
            lastScanTime = dirTime;
            m_pendingRefresh = true;
        }

        // If folder re-appeared and we were in fallback, try upgrading to real watcher
        if (!m_useFallbackPolling)
            break;
    }
}
