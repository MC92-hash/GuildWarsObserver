#include "pch.h"
#include "Net/HttpClient.h"
#include <sstream>

#pragma comment(lib, "winhttp.lib")

HttpClient::HttpClient()
{
    m_session = WinHttpOpen(
        L"GWObserver/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (m_session)
    {
        DWORD connectTimeout = kConnectTimeoutMs;
        DWORD readTimeout = kReadTimeoutMs;
        WinHttpSetOption(m_session, WINHTTP_OPTION_CONNECT_TIMEOUT, &connectTimeout, sizeof(connectTimeout));
        WinHttpSetOption(m_session, WINHTTP_OPTION_RECEIVE_TIMEOUT, &readTimeout, sizeof(readTimeout));

        // Enable TLS 1.2+
        DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(m_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));
    }
}

HttpClient::~HttpClient()
{
    CloseConnection();
    if (m_session)
    {
        WinHttpCloseHandle(m_session);
        m_session = nullptr;
    }
}

void HttpClient::SetBaseUrl(const std::wstring& host, bool useTls)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    CloseConnection();
    m_host = host;
    m_tls = useTls;

    if (m_session && !m_host.empty())
    {
        INTERNET_PORT port = m_tls ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
        m_connection = WinHttpConnect(m_session, m_host.c_str(), port, 0);
    }
}

void HttpClient::SetSigningFunction(SigningFn fn)
{
    m_signingFn = std::move(fn);
}

void HttpClient::CloseConnection()
{
    if (m_connection)
    {
        WinHttpCloseHandle(m_connection);
        m_connection = nullptr;
    }
}

void HttpClient::Cancel()
{
    m_cancelRequested.store(true);
}

void HttpClient::ResetCancel()
{
    m_cancelRequested.store(false);
}

std::wstring HttpClient::GetLastWinHttpError() const
{
    DWORD err = GetLastError();
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE,
                   GetModuleHandleW(L"winhttp.dll"),
                   err, 0, buf, 512, nullptr);

    std::wstringstream ss;
    ss << L"WinHTTP error " << err << L": " << buf;
    return ss.str();
}

std::wstring HttpClient::UrlEncodePath(const std::wstring& path)
{
    std::wstring encoded;
    encoded.reserve(path.size() * 2);

    for (wchar_t ch : path)
    {
        // Keep unreserved characters and path separator as-is
        if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
            (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' ||
            ch == L'.' || ch == L'~' || ch == L'/')
        {
            encoded += ch;
        }
        else if (ch <= 0x7F)
        {
            // Percent-encode ASCII special characters
            wchar_t buf[8];
            swprintf_s(buf, L"%%%02X", static_cast<unsigned char>(ch));
            encoded += buf;
        }
        else
        {
            // For non-ASCII, encode as UTF-8 bytes
            char mb[4] = {};
            int len = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, sizeof(mb), nullptr, nullptr);
            for (int i = 0; i < len; ++i)
            {
                wchar_t buf[8];
                swprintf_s(buf, L"%%%02X", static_cast<unsigned char>(mb[i]));
                encoded += buf;
            }
        }
    }

    return encoded;
}

HINTERNET HttpClient::SendRequest(const std::wstring& path, uint64_t& outContentLength,
                                  const std::wstring& extraHeaders)
{
    outContentLength = 0;

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_session || !m_connection)
        return nullptr;

    std::wstring encodedPath = UrlEncodePath(path);

    DWORD flags = m_tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        m_connection, L"GET", encodedPath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

    if (!hRequest)
        return nullptr;

    // Bypass WinHTTP's own cache so a refresh always reaches the server. This
    // does not defeat conditional requests: the origin still answers a
    // matching If-None-Match with 304, which is where the saving comes from.
    std::wstring noCacheHeaders = L"Cache-Control: no-cache\r\nPragma: no-cache\r\n";
    WinHttpAddRequestHeaders(hRequest, noCacheHeaders.c_str(),
                             (DWORD)noCacheHeaders.size(),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    // Caller-supplied headers (If-None-Match). SigV4 here signs only
    // host;x-amz-content-sha256;x-amz-date, so an extra header travels
    // unsigned and does not invalidate the signature.
    if (!extraHeaders.empty())
    {
        WinHttpAddRequestHeaders(hRequest, extraHeaders.c_str(),
                                 (DWORD)extraHeaders.size(),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    // Inject auth headers if a signing function is configured
    if (m_signingFn)
    {
        std::wstring headers = m_signingFn(L"GET", m_host, encodedPath);
        if (!headers.empty())
        {
            WinHttpAddRequestHeaders(hRequest, headers.c_str(),
                                     (DWORD)headers.size(),
                                     WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
    }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        WinHttpCloseHandle(hRequest);
        return nullptr;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr))
    {
        WinHttpCloseHandle(hRequest);
        return nullptr;
    }

    // Read status code
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    // Read content length (may be 0 if server doesn't send it)
    wchar_t contentLenBuf[32] = {};
    DWORD contentLenSize = sizeof(contentLenBuf);
    if (WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            contentLenBuf, &contentLenSize, WINHTTP_NO_HEADER_INDEX))
    {
        outContentLength = _wcstoui64(contentLenBuf, nullptr, 10);
    }

    // Store status code in a temporary location — caller reads it from the handle
    // We'll return it via the Response struct at the call site.
    // For now, stash it as user data isn't available, so we re-query at call site.

    return hRequest;
}

bool HttpClient::ReadResponseBody(HINTERNET hRequest, std::vector<uint8_t>& out)
{
    uint8_t buffer[kReadBufferSize];
    DWORD bytesRead = 0;

    while (true)
    {
        if (m_cancelRequested.load())
            return false;

        if (!WinHttpReadData(hRequest, buffer, kReadBufferSize, &bytesRead))
            return false;

        if (bytesRead == 0)
            break;

        out.insert(out.end(), buffer, buffer + bytesRead);
    }

    return true;
}

HttpClient::Response HttpClient::Get(const std::wstring& path,
                                     const std::string& ifNoneMatch)
{
    Response resp;

    std::wstring extraHeaders;
    if (!ifNoneMatch.empty())
    {
        std::wstring tag(ifNoneMatch.begin(), ifNoneMatch.end());
        extraHeaders = L"If-None-Match: " + tag + L"\r\n";
    }

    for (int attempt = 0; attempt < kMaxRetries; ++attempt)
    {
        if (m_cancelRequested.load())
        {
            resp.errorMessage = "Request cancelled";
            return resp;
        }

        if (attempt > 0)
            Sleep(kRetryDelaysMs[attempt]);

        uint64_t contentLength = 0;
        HINTERNET hRequest = SendRequest(path, contentLength, extraHeaders);
        if (!hRequest)
        {
            std::wstring err = GetLastWinHttpError();
            resp.errorMessage = std::string(err.begin(), err.end());
            continue;
        }

        // Read status code
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        resp.statusCode = static_cast<int>(statusCode);

        // Capture the ETag so the next request can be conditional.
        wchar_t etagBuf[256] = {};
        DWORD etagSize = sizeof(etagBuf);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_ETAG,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                etagBuf, &etagSize, WINHTTP_NO_HEADER_INDEX))
        {
            std::wstring tag(etagBuf);
            resp.etag.assign(tag.begin(), tag.end());
        }

        // 304 carries no body and is the success case for a conditional GET:
        // what the caller already holds is still current.
        if (resp.statusCode == 304)
        {
            WinHttpCloseHandle(hRequest);
            resp.body.clear();
            resp.errorMessage.clear();
            return resp;
        }

        resp.body.clear();
        if (contentLength > 0)
            resp.body.reserve(static_cast<size_t>(contentLength));

        if (ReadResponseBody(hRequest, resp.body))
        {
            WinHttpCloseHandle(hRequest);
            resp.errorMessage.clear();
            return resp;
        }

        WinHttpCloseHandle(hRequest);

        if (m_cancelRequested.load())
        {
            resp.errorMessage = "Request cancelled";
            return resp;
        }

        resp.errorMessage = "Failed to read response body";
    }

    return resp;
}

HttpClient::Response HttpClient::DownloadToFile(const std::wstring& path,
                                                 const std::filesystem::path& dest,
                                                 ProgressFn progress)
{
    Response resp;
    std::filesystem::path tmpPath = dest;
    tmpPath += L".tmp";

    for (int attempt = 0; attempt < kMaxRetries; ++attempt)
    {
        if (m_cancelRequested.load())
        {
            resp.errorMessage = "Download cancelled";
            return resp;
        }

        if (attempt > 0)
            Sleep(kRetryDelaysMs[attempt]);

        uint64_t contentLength = 0;
        HINTERNET hRequest = SendRequest(path, contentLength);
        if (!hRequest)
        {
            std::wstring err = GetLastWinHttpError();
            resp.errorMessage = std::string(err.begin(), err.end());
            continue;
        }

        // Read status code
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        resp.statusCode = static_cast<int>(statusCode);

        if (!resp.IsOk())
        {
            WinHttpCloseHandle(hRequest);
            resp.errorMessage = "HTTP " + std::to_string(resp.statusCode);
            return resp;
        }

        // Ensure parent directory exists
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);

        // Open temp file for writing
        HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            WinHttpCloseHandle(hRequest);
            resp.errorMessage = "Failed to create temp file: " + tmpPath.string();
            return resp;
        }

        uint8_t buffer[kReadBufferSize];
        DWORD bytesRead = 0;
        uint64_t totalReceived = 0;
        bool readOk = true;

        while (true)
        {
            if (m_cancelRequested.load())
            {
                readOk = false;
                break;
            }

            if (!WinHttpReadData(hRequest, buffer, kReadBufferSize, &bytesRead))
            {
                readOk = false;
                break;
            }

            if (bytesRead == 0)
                break;

            DWORD bytesWritten = 0;
            if (!WriteFile(hFile, buffer, bytesRead, &bytesWritten, nullptr) ||
                bytesWritten != bytesRead)
            {
                readOk = false;
                break;
            }

            totalReceived += bytesRead;

            if (progress)
                progress(totalReceived, contentLength);
        }

        CloseHandle(hFile);
        WinHttpCloseHandle(hRequest);

        if (readOk)
        {
            // Atomic rename: delete existing dest, rename tmp → dest
            std::filesystem::remove(dest, ec);
            std::filesystem::rename(tmpPath, dest, ec);
            if (ec)
            {
                resp.errorMessage = "Failed to rename temp file: " + ec.message();
                resp.statusCode = 0;
                return resp;
            }

            resp.errorMessage.clear();
            return resp;
        }

        // Clean up failed temp file
        std::filesystem::remove(tmpPath, ec);

        if (m_cancelRequested.load())
        {
            resp.errorMessage = "Download cancelled";
            return resp;
        }

        resp.errorMessage = "Failed to read download body";
    }

    return resp;
}
