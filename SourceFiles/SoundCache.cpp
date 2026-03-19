#include "pch.h"
#include "SoundCache.h"
#include "DATManager.h"
#include "GWUnpacker.h"
#include <cstring>

#pragma pack(push, 1)
struct WavHeader {
    char     riff[4];
    uint32_t chunkSize;
    char     wave[4];
    char     fmt[4];
    uint32_t fmtSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};
#pragma pack(pop)

void SoundCache::Init(DATManager* datMgr,
                      const std::unordered_map<int, std::vector<int>>* sharedHashIndex)
{
    m_dat = datMgr;
    m_hashIndex = sharedHashIndex;
    OutputDebugStringA(std::format("[SoundCache] Init: hashIndex={}, MFT={}\n",
        m_hashIndex ? m_hashIndex->size() : 0,
        m_dat ? m_dat->get_MFT().size() : 0).c_str());
}

const AudioBuffer* SoundCache::Get(uint32_t fileId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(fileId);
    if (it != m_cache.end())
        return it->second.valid ? &it->second : nullptr;

    AudioBuffer buf = LoadFromDat(fileId);
    auto [ins, _] = m_cache.emplace(fileId, std::move(buf));
    return ins->second.valid ? &ins->second : nullptr;
}

void SoundCache::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.clear();
}

uint32_t SoundCache::ResolveFileId(uint32_t fileId)
{
    auto& mft = m_dat->get_MFT();

    // Try as Hash via the shared hash index (same one used for textures/models)
    if (m_hashIndex) {
        auto it = m_hashIndex->find(static_cast<int>(fileId));
        if (it != m_hashIndex->end() && !it->second.empty()) {
            uint32_t idx = static_cast<uint32_t>(it->second[0]);
            m_lastLoad.resolveMethod = std::format("hash->idx {} (of {})", idx, it->second.size());
            return idx;
        }
    }

    // Fall back to direct MFT index
    if (fileId < mft.size()) {
        m_lastLoad.resolveMethod = "direct index (hash not found)";
        return fileId;
    }

    m_lastLoad.resolveMethod = std::format("NOT FOUND (hash={}, mft={})",
        m_hashIndex ? m_hashIndex->size() : 0, mft.size());
    return UINT32_MAX;
}

AudioBuffer SoundCache::LoadFromDat(uint32_t fileId)
{
    AudioBuffer result;
    if (!m_dat) return result;

    m_lastLoad = {};
    m_lastLoad.fileId = fileId;

    auto& mft = m_dat->get_MFT();
    uint32_t mftIndex = ResolveFileId(fileId);
    m_lastLoad.resolvedIndex = mftIndex;

    if (mftIndex == UINT32_MAX || mftIndex >= mft.size()) {
        m_idOutOfRange++;
        OutputDebugStringA(std::format("[SoundCache] fileId {} could not resolve ({})\n",
            fileId, m_lastLoad.resolveMethod).c_str());
        return result;
    }

    unsigned char* raw = m_dat->read_file(static_cast<int>(mftIndex));
    if (!raw) {
        m_idOutOfRange++;
        m_lastLoad.rejectReason = "read_file returned null";
        OutputDebugStringA(std::format("[SoundCache] fileId {} read_file returned null\n", fileId).c_str());
        return result;
    }

    MFTEntry& entry = mft[mftIndex];
    int dataSize = entry.uncompressedSize;
    m_lastLoad.dataSize = dataSize;
    m_lastLoad.mftType = entry.type;

    if (dataSize <= 4) {
        m_notSoundType++;
        m_lastLoad.rejectReason = std::format("dataSize too small ({})", dataSize);
        delete[] raw;
        return result;
    }

    uint8_t* data = reinterpret_cast<uint8_t*>(raw);
    memcpy(m_lastLoad.magic, data, std::min(dataSize, 4));

    // Detect audio format by magic bytes (curated JSON, skip MFT type classifier).
    bool isWav = (dataSize >= 4 && memcmp(data, "RIFF", 4) == 0);
    bool isMp3Sync = (dataSize >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0);
    bool isId3 = (dataSize >= 3 && data[0] == 'I' && data[1] == 'D' && data[2] == '3');
    bool isOgg = (dataSize >= 4 && memcmp(data, "OggS", 4) == 0);

    if (!isWav && !isMp3Sync && !isId3 && !isOgg) {
        m_notSoundType++;
        m_lastLoad.rejectReason = std::format("unknown magic 0x{:02X}{:02X}{:02X}{:02X}, mftType={}",
            data[0], data[1], dataSize > 2 ? data[2] : 0, dataSize > 3 ? data[3] : 0, entry.type);
        OutputDebugStringA(std::format("[SoundCache] fileId {} REJECTED: {}\n", fileId, m_lastLoad.rejectReason).c_str());
        delete[] raw;
        return result;
    }

    m_lastLoad.loaded = true;
    OutputDebugStringA(std::format("[SoundCache] fileId {} loaded, fmt={}, size={}\n",
        fileId, isWav ? "WAV" : (isMp3Sync || isId3) ? "MP3" : "OGG", dataSize).c_str());

    if (isWav) {
        if (DecodeWAV(data, dataSize, result))
            m_decodeOk++;
        else
            m_decodeFail++;
    }
    else {
        // MP3 (sync bytes 0xFFxx) or ID3-tagged MP3 — store raw for MF decode in engine
        result.pcmData.assign(data, data + dataSize);
        result.sampleRate = 0;     // sentinel: needs MP3 decode
        result.channels = 0;
        result.bitsPerSample = 0;
        result.valid = true;
        m_decodeOk++;
    }

    delete[] raw;
    return result;
}

bool SoundCache::DecodeWAV(const uint8_t* data, int size, AudioBuffer& out)
{
    if (size < static_cast<int>(sizeof(WavHeader))) return false;

    const WavHeader* hdr = reinterpret_cast<const WavHeader*>(data);
    if (memcmp(hdr->riff, "RIFF", 4) != 0 || memcmp(hdr->wave, "WAVE", 4) != 0)
        return false;

    int offset = 12;
    while (offset + 8 < size) {
        const char* chunkId = reinterpret_cast<const char*>(data + offset);
        uint32_t chunkSize = *reinterpret_cast<const uint32_t*>(data + offset + 4);

        if (memcmp(chunkId, "data", 4) == 0) {
            int dataStart = offset + 8;
            int dataLen = std::min(static_cast<int>(chunkSize), size - dataStart);
            if (dataLen <= 0) return false;

            out.pcmData.assign(data + dataStart, data + dataStart + dataLen);
            out.sampleRate = hdr->sampleRate;
            out.channels = hdr->numChannels;
            out.bitsPerSample = hdr->bitsPerSample;
            out.valid = true;
            return true;
        }

        offset += 8 + chunkSize;
        if (chunkSize & 1) offset++;
    }
    return false;
}
