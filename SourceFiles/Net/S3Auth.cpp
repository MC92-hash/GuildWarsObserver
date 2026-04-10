#include "pch.h"
#include "Net/S3Auth.h"

#include <bcrypt.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "bcrypt.lib")

namespace S3Auth
{

// SHA-256 of the empty string — used for all GET requests (no body).
static constexpr char kEmptyPayloadHash[] =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static std::string ToHex(const uint8_t* data, size_t len)
{
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i)
    {
        out += kHex[data[i] >> 4];
        out += kHex[data[i] & 0x0F];
    }
    return out;
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
    return w;
}

// -----------------------------------------------------------------------
// SHA-256
// -----------------------------------------------------------------------

static std::vector<uint8_t> Sha256Raw(const void* data, size_t len)
{
    std::vector<uint8_t> hash(32);
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return hash;

    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return hash;
    }

    BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0);
    BCryptFinishHash(hHash, hash.data(), 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return hash;
}

std::string Sha256Hex(const std::string& data)
{
    auto h = Sha256Raw(data.data(), data.size());
    return ToHex(h.data(), h.size());
}

// -----------------------------------------------------------------------
// HMAC-SHA256
// -----------------------------------------------------------------------

std::vector<uint8_t> HmacSha256(const std::vector<uint8_t>& key,
                                  const std::string& data)
{
    std::vector<uint8_t> result(32);
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return result;

    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                          (PUCHAR)key.data(), (ULONG)key.size(), 0) != 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return result;
    }

    BCryptHashData(hHash, (PUCHAR)data.data(), (ULONG)data.size(), 0);
    BCryptFinishHash(hHash, result.data(), 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return result;
}

// -----------------------------------------------------------------------
// AWS Signature V4 signing key derivation
// -----------------------------------------------------------------------

static std::vector<uint8_t> DeriveSigningKey(const std::string& secretKey,
                                              const std::string& dateStamp,
                                              const std::string& region,
                                              const std::string& service)
{
    std::string kSecret = "AWS4" + secretKey;
    std::vector<uint8_t> key(kSecret.begin(), kSecret.end());

    key = HmacSha256(key, dateStamp);
    key = HmacSha256(key, region);
    key = HmacSha256(key, service);
    key = HmacSha256(key, "aws4_request");
    return key;
}

// -----------------------------------------------------------------------
// SignHeaders  — build Authorization + supporting headers for a request
// -----------------------------------------------------------------------

std::wstring SignHeaders(const std::wstring& method,
                          const std::wstring& host,
                          const std::wstring& path,
                          const Credentials& creds)
{
    // Current UTC time
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm utc;
    gmtime_s(&utc, &tt);

    char dateStamp[16];   // YYYYMMDD
    char amzDate[20];     // YYYYMMDD'T'HHMMSS'Z'
    snprintf(dateStamp, sizeof(dateStamp), "%04d%02d%02d",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    snprintf(amzDate, sizeof(amzDate), "%04d%02d%02dT%02d%02d%02dZ",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec);

    std::string methodA = WideToUtf8(method);
    std::string hostA   = WideToUtf8(host);
    std::string pathA   = WideToUtf8(path);

    // Lowercase the method for canonical request
    std::string methodLower = methodA;
    std::transform(methodLower.begin(), methodLower.end(), methodLower.begin(), ::tolower);

    // Signed headers (must be sorted)
    const char* signedHeaders = "host;x-amz-content-sha256;x-amz-date";

    // Canonical request
    std::ostringstream cr;
    cr << methodA << "\n"
       << pathA << "\n"
       << "\n"                              // empty query string
       << "host:" << hostA << "\n"
       << "x-amz-content-sha256:" << kEmptyPayloadHash << "\n"
       << "x-amz-date:" << amzDate << "\n"
       << "\n"                              // blank line after headers
       << signedHeaders << "\n"
       << kEmptyPayloadHash;

    std::string canonicalRequestHash = Sha256Hex(cr.str());

    // Credential scope
    std::string scope = std::string(dateStamp) + "/" + creds.region + "/"
                      + creds.service + "/aws4_request";

    // String to sign
    std::ostringstream sts;
    sts << "AWS4-HMAC-SHA256\n"
        << amzDate << "\n"
        << scope << "\n"
        << canonicalRequestHash;

    // Signing key + signature
    auto signingKey = DeriveSigningKey(creds.secretKey, dateStamp,
                                       creds.region, creds.service);
    auto signatureRaw = HmacSha256(signingKey, sts.str());
    std::string signature = ToHex(signatureRaw.data(), signatureRaw.size());

    // Authorization header value
    std::string authValue = "AWS4-HMAC-SHA256 Credential=" + creds.accessKey + "/"
                          + scope + ", SignedHeaders=" + signedHeaders
                          + ", Signature=" + signature;

    // Build CRLF-separated header block for WinHttpAddRequestHeaders
    std::ostringstream hdr;
    hdr << "Authorization: " << authValue << "\r\n"
        << "x-amz-date: " << amzDate << "\r\n"
        << "x-amz-content-sha256: " << kEmptyPayloadHash << "\r\n"
        << "Host: " << hostA << "\r\n";

    return Utf8ToWide(hdr.str());
}

} // namespace S3Auth
