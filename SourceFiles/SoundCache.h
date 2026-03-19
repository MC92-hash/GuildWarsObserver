#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>

class DATManager;

struct AudioBuffer {
    std::vector<uint8_t> pcmData;
    uint32_t sampleRate    = 44100;
    uint16_t channels      = 1;
    uint16_t bitsPerSample = 16;
    bool valid = false;
};

class SoundCache {
public:
    void Init(DATManager* datMgr,
              const std::unordered_map<int, std::vector<int>>* sharedHashIndex);

    // Returns cached buffer, or loads from dat on first access.
    // fileId is the MFT array index (same as "File ID" in the GWMB browser).
    // Thread-safe. Returns nullptr if file is not a SOUND entry.
    const AudioBuffer* Get(uint32_t fileId);

    void Clear();

    // Debug
    int GetHashNotFound() const { return m_idOutOfRange; }
    int GetNotSoundType() const { return m_notSoundType; }
    int GetDecodeOk()     const { return m_decodeOk; }
    int GetDecodeFail()   const { return m_decodeFail; }

    struct LastLoadInfo {
        uint32_t fileId = 0;
        uint32_t resolvedIndex = 0;
        int      dataSize = 0;
        uint8_t  magic[4]{};
        int      mftType = -1;
        std::string rejectReason;
        std::string resolveMethod;
        bool     loaded = false;
    };
    const LastLoadInfo& GetLastLoad() const { return m_lastLoad; }
    int GetHashIndexSize() const { return m_hashIndex ? static_cast<int>(m_hashIndex->size()) : 0; }

private:
    AudioBuffer LoadFromDat(uint32_t fileId);
    bool DecodeWAV(const uint8_t* data, int size, AudioBuffer& out);
    uint32_t ResolveFileId(uint32_t fileId);

    DATManager* m_dat = nullptr;
    const std::unordered_map<int, std::vector<int>>* m_hashIndex = nullptr;
    std::unordered_map<uint32_t, AudioBuffer> m_cache;
    std::mutex m_mutex;

    int m_idOutOfRange = 0;
    int m_notSoundType = 0;
    int m_decodeOk     = 0;
    int m_decodeFail   = 0;
    LastLoadInfo m_lastLoad;
};
