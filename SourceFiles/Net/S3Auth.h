#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace S3Auth
{
    struct Credentials
    {
        std::string accessKey;
        std::string secretKey;
        std::string region  = "auto";
        std::string service = "s3";
    };

    // Build the set of headers needed to authenticate a GET request using
    // AWS Signature V4.  Returns a single wide string in the format
    // expected by WinHttpAddRequestHeaders (CRLF-separated).
    //
    //   method : L"GET"
    //   host   : e.g. L"account.r2.cloudflarestorage.com"
    //   path   : e.g. L"/bucket/index.json"  (must already be URL-encoded)
    //
    std::wstring SignHeaders(const std::wstring& method,
                             const std::wstring& host,
                             const std::wstring& path,
                             const Credentials& creds);

    // Low-level helpers (exposed for testing)
    std::string Sha256Hex(const std::string& data);
    std::vector<uint8_t> HmacSha256(const std::vector<uint8_t>& key,
                                     const std::string& data);
}
