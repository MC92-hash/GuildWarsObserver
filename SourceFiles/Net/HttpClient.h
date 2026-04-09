#pragma once
#include <string>
#include <vector>
#include <functional>
#include <filesystem>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <Windows.h>
#include <winhttp.h>

class HttpClient
{
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    struct Response
    {
        int statusCode = 0;
        std::vector<uint8_t> body;
        std::string errorMessage;
        bool IsOk() const { return statusCode >= 200 && statusCode < 300; }
    };

    // Configure the remote host. Call once before making requests.
    // host: e.g. L"matches.tolkano.com" (no scheme, no path)
    void SetBaseUrl(const std::wstring& host, bool useTls = true);

    // Optional: set a signing function that produces CRLF-separated headers
    // for every request.  The function receives (method, host, encodedPath).
    using SigningFn = std::function<std::wstring(const std::wstring& method,
                                                 const std::wstring& host,
                                                 const std::wstring& path)>;
    void SetSigningFunction(SigningFn fn);

    // GET request. Returns the full response body in memory.
    Response Get(const std::wstring& path);

    // Download to a file with optional progress callback.
    // Writes to {dest}.tmp first, then renames on success.
    using ProgressFn = std::function<void(uint64_t bytesReceived, uint64_t totalBytes)>;
    Response DownloadToFile(const std::wstring& path,
                           const std::filesystem::path& dest,
                           ProgressFn progress = nullptr);

    // Signal cancellation. Safe to call from any thread.
    void Cancel();

    // Reset cancellation flag for reuse.
    void ResetCancel();

    bool IsConnected() const { return m_connection != nullptr; }

private:
    // Internal: open a request, send it, and read the status code.
    // On success, the returned HINTERNET is ready for WinHttpReadData.
    // Caller must close it with WinHttpCloseHandle.
    HINTERNET SendRequest(const std::wstring& path, uint64_t& outContentLength);

    // Read all remaining data from an open request handle.
    bool ReadResponseBody(HINTERNET hRequest, std::vector<uint8_t>& out);

    void CloseConnection();
    std::wstring GetLastWinHttpError() const;
    static std::wstring UrlEncodePath(const std::wstring& path);

    static constexpr int kMaxRetries = 3;
    static constexpr int kConnectTimeoutMs = 15000;
    static constexpr int kReadTimeoutMs = 30000;
    static constexpr size_t kReadBufferSize = 8192;
    static constexpr int kRetryDelaysMs[kMaxRetries] = {1000, 2000, 4000};

    HINTERNET m_session = nullptr;
    HINTERNET m_connection = nullptr;
    std::wstring m_host;
    bool m_tls = true;
    SigningFn m_signingFn;

    std::atomic<bool> m_cancelRequested{false};
    mutable std::mutex m_mutex;
};
