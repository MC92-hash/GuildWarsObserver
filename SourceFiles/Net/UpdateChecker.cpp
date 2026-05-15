#include "pch.h"
#include "Net/UpdateChecker.h"
#include "Net/HttpClient.h"
#include "GuiGlobalConstants.h"
#include "json.hpp"
#include <sstream>

using json = nlohmann::json;

UpdateChecker::~UpdateChecker()
{
    Cancel();
    if (m_thread.joinable())
        m_thread.join();
}

void UpdateChecker::Check(const std::string& currentVersion,
                           const std::string& repo,
                           bool userInitiated)
{
    auto current = m_state.load();
    if (current == State::Checking || current == State::Downloading)
        return;

    if (m_thread.joinable())
        m_thread.join();

    m_currentVersion = currentVersion;
    m_repo = repo;
    m_cancelRequested.store(false);
    m_dismissed.store(false);
    m_userInitiated.store(userInitiated);
    m_progress.store(0.f);
    m_bytesReceived.store(0);
    m_bytesTotal.store(0);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError.clear();
    }

    m_state.store(State::Checking);
    m_thread = std::thread(&UpdateChecker::CheckThread, this);
}

void UpdateChecker::StartDownload()
{
    if (m_state.load() != State::UpdateAvailable)
        return;

    if (m_thread.joinable())
        m_thread.join();

    m_cancelRequested.store(false);
    m_progress.store(0.f);
    m_bytesReceived.store(0);
    m_bytesTotal.store(0);

    m_state.store(State::Downloading);
    m_thread = std::thread(&UpdateChecker::DownloadThread, this);
}

// Get the full path of the running executable
static std::filesystem::path GetCurrentExePath()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    return std::filesystem::path(buf);
}

bool UpdateChecker::ApplyAndRestart(HWND appWindow)
{
    if (m_state.load() != State::ReadyToInstall)
        return false;

    if (m_thread.joinable())
        m_thread.join();

    auto currentExe = GetCurrentExePath();
    auto exeDir = currentExe.parent_path();
    auto batPath = exeDir / "_gwobs_update.bat";

    if (!std::filesystem::exists(m_downloadedPath))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "Downloaded update file not found";
        m_state.store(State::Error);
        return false;
    }

    // Write the updater batch script
    DWORD pid = GetCurrentProcessId();
    {
        std::ofstream bat(batPath);
        if (!bat.is_open())
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastError = "Failed to write updater script";
            m_state.store(State::Error);
            return false;
        }

        bat << "@echo off\r\n";
        bat << ":wait\r\n";
        bat << "tasklist /FI \"PID eq " << pid << "\" 2>NUL | find /I \"" << pid << "\" >NUL\r\n";
        bat << "if not errorlevel 1 (\r\n";
        bat << "    timeout /t 1 /nobreak >NUL\r\n";
        bat << "    goto wait\r\n";
        bat << ")\r\n";

        if (m_isZipUpdate)
        {
            // Extract zip over the install directory using tar (fast, built into Win10+)
            bat << "tar -xf \"" << m_downloadedPath.string()
                << "\" -C \"" << exeDir.string() << "\"\r\n";
            // Retry delete — tar releases the handle immediately, but guard against AV locks
            bat << "del \"" << m_downloadedPath.string() << "\" >NUL 2>&1\r\n";
            bat << "if exist \"" << m_downloadedPath.string()
                << "\" (timeout /t 2 /nobreak >NUL & del \""
                << m_downloadedPath.string() << "\" >NUL 2>&1)\r\n";
        }
        else
        {
            // Legacy exe-only swap
            bat << "del \"" << currentExe.string() << "\"\r\n";
            bat << "move \"" << m_downloadedPath.string() << "\" \""
                << currentExe.string() << "\"\r\n";
        }

        bat << "start \"\" \"" << currentExe.string() << "\"\r\n";
        bat << "del \"%~f0\"\r\n";
    }

    // Launch the batch script hidden
    std::wstring cmdLine = L"cmd.exe /c \"" + batPath.wstring() + L"\"";

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, exeDir.wstring().c_str(), &si, &pi))
    {
        std::error_code ec;
        std::filesystem::remove(batPath, ec);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "Failed to launch updater script";
        m_state.store(State::Error);
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    PostMessage(appWindow, WM_CLOSE, 0, 0);
    return true;
}

void UpdateChecker::Cancel()
{
    m_cancelRequested.store(true);
}

void UpdateChecker::Dismiss()
{
    m_dismissed.store(true);
}

void UpdateChecker::DebugSimulate(State targetState)
{
    Cancel();
    if (m_thread.joinable())
        m_thread.join();

    m_cancelRequested.store(false);
    m_dismissed.store(false);
    m_userInitiated.store(true);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_latestVersion = "v99.0.0";
        m_releaseNotes = "## What's new in v99.0.0\n\n"
                         "- This is a simulated release for testing\n"
                         "- The update UI panel and all states\n"
                         "- No actual download or install will happen";
        m_downloadUrl = "https://example.com/fake.exe";
        m_releaseUrl = "https://github.com/MC92-hash/gwobserver/releases/tag/v99.0.0";
        m_lastError = (targetState == State::Error) ? "Simulated error for debug testing" : "";
    }

    if (targetState == State::Downloading)
    {
        m_bytesReceived.store(2 * 1024 * 1024);
        m_bytesTotal.store(5 * 1024 * 1024);
        m_progress.store(0.4f);
    }

    m_state.store(targetState);
}

bool UpdateChecker::DebugFullTest()
{
    Cancel();
    if (m_thread.joinable())
        m_thread.join();

    m_cancelRequested.store(false);
    m_dismissed.store(false);
    m_userInitiated.store(true);

    auto currentExe = GetCurrentExePath();
    auto exeDir = currentExe.parent_path();
    auto updateExe = exeDir / "GWObserver_update.exe";

    std::error_code ec;
    std::filesystem::copy_file(currentExe, updateExe,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "Debug: failed to copy exe: " + ec.message();
        m_state.store(State::Error);
        return false;
    }

    m_downloadedPath = updateExe;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_latestVersion = "v99.0.0 (debug test)";
        m_releaseNotes = "This is a full end-to-end test.\n\n"
                         "Clicking 'Install & Restart' will:\n"
                         "1. Write the updater batch script\n"
                         "2. Launch it (hidden)\n"
                         "3. Exit this process\n"
                         "4. The script waits, swaps the exe, and relaunches\n\n"
                         "Since the 'update' is a copy of the current exe, "
                         "the app should relaunch identically.";
        m_lastError.clear();
    }

    m_state.store(State::ReadyToInstall);
    return true;
}

// --- Accessors ---

bool UpdateChecker::IsComplete() const
{
    auto s = m_state.load();
    return s == State::Idle || s == State::UpdateAvailable ||
           s == State::ReadyToInstall || s == State::Error;
}

bool UpdateChecker::HasUpdate() const
{
    auto s = m_state.load();
    return s == State::UpdateAvailable || s == State::Downloading || s == State::ReadyToInstall;
}

std::string UpdateChecker::GetLatestVersion() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_latestVersion;
}

std::string UpdateChecker::GetReleaseUrl() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_releaseUrl;
}

std::string UpdateChecker::GetCurrentVersion() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentVersion;
}

std::string UpdateChecker::GetReleaseNotes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_releaseNotes;
}

std::string UpdateChecker::GetLastError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

// --- Version comparison ---

bool UpdateChecker::IsNewer(const std::string& latest, const std::string& current)
{
    auto parse = [](const std::string& s, int& maj, int& min, int& pat)
    {
        const char* p = s.c_str();
        if (*p == 'v' || *p == 'V') ++p;
        maj = min = pat = 0;
        sscanf_s(p, "%d.%d.%d", &maj, &min, &pat);
    };

    int lMaj, lMin, lPat;
    int cMaj, cMin, cPat;
    parse(latest, lMaj, lMin, lPat);
    parse(current, cMaj, cMin, cPat);

    if (lMaj != cMaj) return lMaj > cMaj;
    if (lMin != cMin) return lMin > cMin;
    return lPat > cPat;
}

// --- Background thread: check GitHub API ---

void UpdateChecker::CheckThread()
{
    HttpClient http;
    http.SetBaseUrl(L"api.github.com", true);

    std::string pathStr = "/repos/" + m_repo + "/releases/latest";
    std::wstring path(pathStr.begin(), pathStr.end());

    auto resp = http.Get(path);

    if (m_cancelRequested.load())
    {
        m_state.store(State::Idle);
        return;
    }

    if (!resp.IsOk())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (resp.statusCode > 0)
            m_lastError = "GitHub API returned HTTP " + std::to_string(resp.statusCode);
        else if (!resp.errorMessage.empty())
            m_lastError = resp.errorMessage;
        else
            m_lastError = "Could not connect to GitHub";
        m_state.store(State::Error);
        return;
    }

    std::string body(resp.body.begin(), resp.body.end());

    try
    {
        auto j = json::parse(body);

        std::string tagName = j.value("tag_name", "");
        std::string htmlUrl = j.value("html_url", "");
        std::string releaseBody = j.value("body", "");
        std::string downloadUrl;
        bool isZip = false;

        // Find the best update asset: prefer .zip (full update), fall back to .exe
        if (j.contains("assets") && j["assets"].is_array())
        {
            std::string zipUrl, exeUrl;
            for (auto& asset : j["assets"])
            {
                std::string name = asset.value("name", "");
                if (name.size() >= 4 && name.substr(name.size() - 4) == ".zip")
                    zipUrl = asset.value("browser_download_url", "");
                else if (name.size() >= 4 && name.substr(name.size() - 4) == ".exe")
                    exeUrl = asset.value("browser_download_url", "");
            }

            if (!zipUrl.empty())
            {
                downloadUrl = zipUrl;
                isZip = true;
            }
            else
            {
                downloadUrl = exeUrl;
            }
        }

        if (!tagName.empty())
        {
            // Strip leading 'v'/'V' so displayed versions are consistent with GWO_VERSION
            if (tagName.front() == 'v' || tagName.front() == 'V')
                tagName.erase(0, 1);

            std::lock_guard<std::mutex> lock(m_mutex);
            m_latestVersion = tagName;
            m_releaseUrl = htmlUrl;
            m_releaseNotes = releaseBody;
            m_downloadUrl = downloadUrl;
            m_isZipUpdate = isZip;

            if (IsNewer(tagName, m_currentVersion))
            {
                m_state.store(State::UpdateAvailable);
                return;
            }
        }
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "Failed to parse GitHub release JSON";
        m_state.store(State::Error);
        return;
    }

    m_state.store(State::Idle);
}

// --- URL parsing helper ---

struct ParsedUrl
{
    std::wstring host;
    std::wstring path;
    bool tls = true;
};

static bool ParseUrl(const std::string& url, ParsedUrl& out)
{
    std::string_view sv(url);
    if (sv.starts_with("https://"))
    {
        sv.remove_prefix(8);
        out.tls = true;
    }
    else if (sv.starts_with("http://"))
    {
        sv.remove_prefix(7);
        out.tls = false;
    }
    else
    {
        return false;
    }

    auto slashPos = sv.find('/');
    std::string host, path;
    if (slashPos == std::string_view::npos)
    {
        host = std::string(sv);
        path = "/";
    }
    else
    {
        host = std::string(sv.substr(0, slashPos));
        path = std::string(sv.substr(slashPos));
    }

    out.host = std::wstring(host.begin(), host.end());
    out.path = std::wstring(path.begin(), path.end());
    return true;
}

static std::wstring GetLocationHeader(HINTERNET hRequest)
{
    wchar_t buf[2048] = {};
    DWORD bufSize = sizeof(buf);
    if (WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_LOCATION,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            buf, &bufSize, WINHTTP_NO_HEADER_INDEX))
    {
        return std::wstring(buf);
    }
    return {};
}

// --- Background thread: download the exe ---

void UpdateChecker::DownloadThread()
{
    std::string downloadUrl;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        downloadUrl = m_downloadUrl;
    }

    if (downloadUrl.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "No download URL available";
        m_state.store(State::Error);
        return;
    }

    auto exeDir = GuiGlobalConstants::GetExeDir();
    m_downloadedPath = exeDir / (m_isZipUpdate ? "GWObserver_update.zip" : "GWObserver_update.exe");

    // GitHub's browser_download_url redirects (302) to objects.githubusercontent.com.
    // WinHTTP doesn't follow cross-host redirects, so we handle it manually.

    ParsedUrl parsed;
    if (!ParseUrl(downloadUrl, parsed))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "Invalid download URL";
        m_state.store(State::Error);
        return;
    }

    // Try direct download first (works if server doesn't redirect)
    {
        HttpClient dlHttp;
        dlHttp.SetBaseUrl(parsed.host, parsed.tls);

        auto dlResp = dlHttp.DownloadToFile(
            parsed.path,
            m_downloadedPath,
            [this](uint64_t received, uint64_t total)
            {
                m_bytesReceived.store(received);
                m_bytesTotal.store(total);
                if (total > 0)
                    m_progress.store(static_cast<float>(received) / static_cast<float>(total));
            });

        if (m_cancelRequested.load())
        {
            std::error_code ec;
            std::filesystem::remove(m_downloadedPath, ec);
            m_state.store(State::Idle);
            return;
        }

        if (dlResp.IsOk())
        {
            m_state.store(State::ReadyToInstall);
            return;
        }
    }

    // Direct download failed (likely 302 redirect). Resolve manually with WinHTTP.
    {
        HINTERNET hSession = WinHttpOpen(
            L"GWObserver-Updater",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);

        if (!hSession)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastError = "Failed to create WinHTTP session for redirect";
            m_state.store(State::Error);
            return;
        }

        DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));

        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        INTERNET_PORT port = parsed.tls ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
        HINTERNET hConnect = WinHttpConnect(hSession, parsed.host.c_str(), port, 0);
        if (!hConnect)
        {
            WinHttpCloseHandle(hSession);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastError = "Failed to connect to download host";
            m_state.store(State::Error);
            return;
        }

        DWORD flags = parsed.tls ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"GET", parsed.path.c_str(),
            nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

        if (!hRequest)
        {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastError = "Failed to open request for redirect resolution";
            m_state.store(State::Error);
            return;
        }

        WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        WinHttpReceiveResponse(hRequest, nullptr);

        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

        std::wstring location = GetLocationHeader(hRequest);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        if (location.empty())
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastError = "Download failed: HTTP " + std::to_string(statusCode) + " with no redirect";
            m_state.store(State::Error);
            return;
        }

        // Download from the redirect target
        std::string locationStr(location.begin(), location.end());
        ParsedUrl finalUrl;
        if (!ParseUrl(locationStr, finalUrl))
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastError = "Failed to parse redirect URL";
            m_state.store(State::Error);
            return;
        }

        HttpClient finalHttp;
        finalHttp.SetBaseUrl(finalUrl.host, finalUrl.tls);

        auto finalResp = finalHttp.DownloadToFile(
            finalUrl.path,
            m_downloadedPath,
            [this](uint64_t received, uint64_t total)
            {
                m_bytesReceived.store(received);
                m_bytesTotal.store(total);
                if (total > 0)
                    m_progress.store(static_cast<float>(received) / static_cast<float>(total));
            });

        if (m_cancelRequested.load())
        {
            std::error_code ec;
            std::filesystem::remove(m_downloadedPath, ec);
            m_state.store(State::Idle);
            return;
        }

        if (finalResp.IsOk())
        {
            m_state.store(State::ReadyToInstall);
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "Download failed: HTTP " + std::to_string(finalResp.statusCode);
        if (!finalResp.errorMessage.empty())
            m_lastError += " - " + finalResp.errorMessage;
        m_state.store(State::Error);
    }
}
