#include "pch.h"
#include "GuiGlobalConstants.h"
#include <bcrypt.h>
#include <vector>
#include <algorithm>

#if __has_include("contributor_key_hashes.h")
#include "contributor_key_hashes.h"
#endif

bool GuiGlobalConstants::ValidateContributorKey(const std::string& key)
{
#if __has_include("contributor_key_hashes.h")
    if (key.empty()) return false;

    // SHA-256 hash the key using Windows CNG
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    BYTE hash[32] = {};

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;

    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    BCryptHashData(hHash, (PUCHAR)key.data(), (ULONG)key.size(), 0);
    BCryptFinishHash(hHash, hash, 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    // Convert to hex string
    char hex[65] = {};
    for (int i = 0; i < 32; ++i)
        sprintf_s(hex + i * 2, 3, "%02x", hash[i]);

    std::string hexStr(hex);
    return std::find(GWO_VALID_KEY_HASHES.begin(), GWO_VALID_KEY_HASHES.end(), hexStr)
        != GWO_VALID_KEY_HASHES.end();
#else
    (void)key;
    return false;
#endif
}
