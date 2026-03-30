#pragma once
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>

// Minimal gzip + tar extraction for match archives.
// Uses miniz for gzip decompression. No external dependencies.

#include "ThirdParty/miniz/miniz.h"

namespace TarGz
{

// Decompress gzip data using miniz's streaming inflate.
// Gzip format: 10-byte header, compressed data, 8-byte trailer.
inline bool GzipDecompress(const std::vector<uint8_t>& gzData, std::vector<uint8_t>& out)
{
    if (gzData.size() < 18) return false;
    // Verify gzip magic
    if (gzData[0] != 0x1F || gzData[1] != 0x8B) return false;

    // Skip gzip header (minimum 10 bytes)
    size_t offset = 10;
    uint8_t flags = gzData[3];
    if (flags & 0x04) { // FEXTRA
        if (offset + 2 > gzData.size()) return false;
        uint16_t xlen = gzData[offset] | (gzData[offset + 1] << 8);
        offset += 2 + xlen;
    }
    if (flags & 0x08) { // FNAME
        while (offset < gzData.size() && gzData[offset] != 0) offset++;
        offset++;
    }
    if (flags & 0x10) { // FCOMMENT
        while (offset < gzData.size() && gzData[offset] != 0) offset++;
        offset++;
    }
    if (flags & 0x02) offset += 2; // FHCRC

    if (offset >= gzData.size()) return false;

    // Read uncompressed size from last 4 bytes of gzip (mod 2^32)
    uint32_t uncompSize = 0;
    if (gzData.size() >= 4)
    {
        size_t p = gzData.size() - 4;
        uncompSize = gzData[p] | (gzData[p+1] << 8) | (gzData[p+2] << 16) | (gzData[p+3] << 24);
    }

    // Allocate with some headroom (gzip stores size mod 2^32, could be larger)
    size_t allocSize = uncompSize > 0 ? uncompSize : gzData.size() * 4;
    out.resize(allocSize);

    // Use raw inflate (windowBits = -MZ_DEFAULT_WINDOW_BITS for raw deflate)
    mz_stream stream = {};
    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
        return false;

    stream.next_in = gzData.data() + offset;
    stream.avail_in = static_cast<unsigned int>(gzData.size() - offset - 8); // exclude trailer
    stream.next_out = out.data();
    stream.avail_out = static_cast<unsigned int>(out.size());

    int status = mz_inflate(&stream, MZ_FINISH);

    // If buffer was too small, retry with larger
    if (status == MZ_BUF_ERROR)
    {
        mz_inflateEnd(&stream);
        out.resize(out.size() * 4);
        stream = {};
        if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
            return false;
        stream.next_in = gzData.data() + offset;
        stream.avail_in = static_cast<unsigned int>(gzData.size() - offset - 8);
        stream.next_out = out.data();
        stream.avail_out = static_cast<unsigned int>(out.size());
        status = mz_inflate(&stream, MZ_FINISH);
    }

    size_t totalOut = stream.total_out;
    mz_inflateEnd(&stream);

    if (status != MZ_STREAM_END)
        return false;

    out.resize(totalOut);
    return true;
}

// Parse octal size field from tar header.
inline uint64_t ParseOctal(const char* p, size_t len)
{
    uint64_t val = 0;
    for (size_t i = 0; i < len && p[i] >= '0' && p[i] <= '7'; i++)
        val = val * 8 + (p[i] - '0');
    return val;
}

// Extract a .tar.gz archive to a destination directory.
// Returns true on success.
inline bool ExtractTarGz(const std::filesystem::path& archivePath,
                          const std::filesystem::path& destDir)
{
    // Read the .tar.gz file
    std::ifstream file(archivePath, std::ios::binary);
    if (!file.is_open()) return false;

    file.seekg(0, std::ios::end);
    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> gzData(fileSize);
    file.read(reinterpret_cast<char*>(gzData.data()), fileSize);
    file.close();

    // Decompress gzip
    std::vector<uint8_t> tarData;
    if (!GzipDecompress(gzData, tarData))
        return false;

    gzData.clear(); // free memory

    // Parse tar
    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);

    size_t pos = 0;
    while (pos + 512 <= tarData.size())
    {
        const char* header = reinterpret_cast<const char*>(tarData.data() + pos);

        // Check for end-of-archive (two zero blocks)
        if (header[0] == '\0')
            break;

        // Extract name (offset 0, 100 bytes)
        std::string name(header, strnlen(header, 100));

        // Check for long name prefix (offset 345, 155 bytes)
        if (header[345] != '\0')
        {
            std::string prefix(header + 345, strnlen(header + 345, 155));
            name = prefix + "/" + name;
        }

        // File size (offset 124, 12 bytes, octal)
        uint64_t fileSize64 = ParseOctal(header + 124, 12);

        // Type flag (offset 156)
        char typeFlag = header[156];

        pos += 512; // advance past header

        if (typeFlag == '5' || name.back() == '/')
        {
            // Directory entry
            std::filesystem::create_directories(destDir / name, ec);
        }
        else if (typeFlag == '0' || typeFlag == '\0')
        {
            // Regular file
            auto filePath = destDir / name;
            std::filesystem::create_directories(filePath.parent_path(), ec);

            std::ofstream outFile(filePath, std::ios::binary);
            if (outFile.is_open() && fileSize64 > 0)
            {
                if (pos + fileSize64 > tarData.size())
                    return false; // truncated archive
                outFile.write(reinterpret_cast<const char*>(tarData.data() + pos),
                              static_cast<std::streamsize>(fileSize64));
            }
        }

        // Advance past file data (padded to 512 bytes)
        pos += static_cast<size_t>((fileSize64 + 511) & ~511ULL);
    }

    return true;
}

} // namespace TarGz
