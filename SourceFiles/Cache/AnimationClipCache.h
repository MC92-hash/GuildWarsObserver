#pragma once
#include <string>
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <cstdint>
#include "Animation/AnimationClip.h"

namespace GW::Cache {

/**
 * @brief Binary cache for parsed AnimationClip data.
 *
 * Stores fully parsed animation clips keyed by {mftIndex, fileHash}.
 * This allows skipping the expensive DAT decompression + VLE keyframe parsing
 * on repeat launches. Invalidated when the DAT file size changes.
 */
class AnimationClipCache
{
public:
    struct CacheKey
    {
        int mftIndex;
        uint32_t fileHash;

        bool operator==(const CacheKey& o) const
        {
            return mftIndex == o.mftIndex && fileHash == o.fileHash;
        }
    };

    struct CacheKeyHash
    {
        size_t operator()(const CacheKey& k) const
        {
            return std::hash<int>()(k.mftIndex) ^ (std::hash<uint32_t>()(k.fileHash) << 16);
        }
    };

    static std::filesystem::path GetDefaultCachePath()
    {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(NULL, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path() / "animation_clips_cache.bin";
    }

    bool Load(const std::filesystem::path& path, uint64_t datSize)
    {
        m_entries.clear();
        m_dirty = false;

        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return false;

        uint32_t magic = 0, version = 0;
        uint64_t storedDatSize = 0;
        uint32_t entryCount = 0;

        f.read(reinterpret_cast<char*>(&magic), 4);
        f.read(reinterpret_cast<char*>(&version), 4);
        f.read(reinterpret_cast<char*>(&storedDatSize), 8);
        f.read(reinterpret_cast<char*>(&entryCount), 4);

        if (!f || magic != kMagic || version != kVersion || storedDatSize != datSize)
            return false;

        for (uint32_t i = 0; i < entryCount; i++)
        {
            CacheKey key{};
            f.read(reinterpret_cast<char*>(&key.mftIndex), sizeof(key.mftIndex));
            f.read(reinterpret_cast<char*>(&key.fileHash), sizeof(key.fileHash));
            if (!f) return false;

            Animation::AnimationClip clip;
            if (!ReadClip(f, clip)) return false;
            m_entries[key] = std::move(clip);
        }

        return true;
    }

    void Save(const std::filesystem::path& path, uint64_t datSize)
    {
        if (!m_dirty) return;

        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) return;

        uint32_t magic = kMagic, version = kVersion;
        uint32_t entryCount = static_cast<uint32_t>(m_entries.size());

        f.write(reinterpret_cast<const char*>(&magic), 4);
        f.write(reinterpret_cast<const char*>(&version), 4);
        f.write(reinterpret_cast<const char*>(&datSize), 8);
        f.write(reinterpret_cast<const char*>(&entryCount), 4);

        for (const auto& [key, clip] : m_entries)
        {
            f.write(reinterpret_cast<const char*>(&key.mftIndex), sizeof(key.mftIndex));
            f.write(reinterpret_cast<const char*>(&key.fileHash), sizeof(key.fileHash));
            WriteClip(f, clip);
        }

        m_dirty = false;
    }

    Animation::AnimationClip* Get(int mftIndex, uint32_t fileHash)
    {
        auto it = m_entries.find({mftIndex, fileHash});
        if (it != m_entries.end()) return &it->second;
        return nullptr;
    }

    void Put(int mftIndex, uint32_t fileHash, const Animation::AnimationClip& clip)
    {
        m_entries[{mftIndex, fileHash}] = clip;
        m_dirty = true;
    }

    bool IsDirty() const { return m_dirty; }

private:
    static constexpr uint32_t kMagic   = 0x414E4943; // "ANIC"
    static constexpr uint32_t kVersion = 1;

    std::unordered_map<CacheKey, Animation::AnimationClip, CacheKeyHash> m_entries;
    bool m_dirty = false;

    // ── Serialization helpers ─────────────────────────────────

    template<typename T>
    static void W(std::ofstream& f, const T& v) { f.write(reinterpret_cast<const char*>(&v), sizeof(T)); }

    template<typename T>
    static bool R(std::ifstream& f, T& v) { f.read(reinterpret_cast<char*>(&v), sizeof(T)); return f.good(); }

    static void WriteStr(std::ofstream& f, const std::string& s)
    {
        uint32_t len = static_cast<uint32_t>(s.size());
        W(f, len);
        if (len > 0) f.write(s.data(), len);
    }

    static bool ReadStr(std::ifstream& f, std::string& s)
    {
        uint32_t len = 0;
        if (!R(f, len)) return false;
        s.resize(len);
        if (len > 0) f.read(s.data(), len);
        return f.good() || len == 0;
    }

    template<typename T>
    static void WriteVec(std::ofstream& f, const std::vector<T>& v)
    {
        uint32_t n = static_cast<uint32_t>(v.size());
        W(f, n);
        if (n > 0) f.write(reinterpret_cast<const char*>(v.data()), n * sizeof(T));
    }

    template<typename T>
    static bool ReadVec(std::ifstream& f, std::vector<T>& v)
    {
        uint32_t n = 0;
        if (!R(f, n)) return false;
        v.resize(n);
        if (n > 0) f.read(reinterpret_cast<char*>(v.data()), n * sizeof(T));
        return f.good() || n == 0;
    }

    // BoneTrack has nested vectors, so needs custom serialization
    static void WriteBoneTrack(std::ofstream& f, const Animation::BoneTrack& bt)
    {
        W(f, bt.boneIndex);
        W(f, bt.basePosition);
        WriteVec(f, bt.positionKeys);
        WriteVec(f, bt.rotationKeys);
        WriteVec(f, bt.scaleKeys);
    }

    static bool ReadBoneTrack(std::ifstream& f, Animation::BoneTrack& bt)
    {
        if (!R(f, bt.boneIndex)) return false;
        if (!R(f, bt.basePosition)) return false;
        if (!ReadVec(f, bt.positionKeys)) return false;
        if (!ReadVec(f, bt.rotationKeys)) return false;
        if (!ReadVec(f, bt.scaleKeys)) return false;
        return true;
    }

    static void WriteSequence(std::ofstream& f, const Animation::AnimationSequence& s)
    {
        W(f, s.hash);
        WriteStr(f, s.name);
        W(f, s.startTime);
        W(f, s.endTime);
        W(f, s.frameCount);
        W(f, s.sequenceIndex);
        W(f, s.bounds);
    }

    static bool ReadSequence(std::ifstream& f, Animation::AnimationSequence& s)
    {
        if (!R(f, s.hash)) return false;
        if (!ReadStr(f, s.name)) return false;
        if (!R(f, s.startTime)) return false;
        if (!R(f, s.endTime)) return false;
        if (!R(f, s.frameCount)) return false;
        if (!R(f, s.sequenceIndex)) return false;
        if (!R(f, s.bounds)) return false;
        return true;
    }

    static void WriteClip(std::ofstream& f, const Animation::AnimationClip& c)
    {
        // Scalars
        WriteStr(f, c.name);
        W(f, c.duration);
        W(f, c.minTime);
        W(f, c.maxTime);
        W(f, c.totalFrames);
        W(f, c.modelHash0);
        W(f, c.modelHash1);
        W(f, c.geometryScale);
        W(f, static_cast<int32_t>(c.hierarchyMode));
        WriteStr(f, c.sourceChunkType);

        // Bone tracks
        uint32_t trackCount = static_cast<uint32_t>(c.boneTracks.size());
        W(f, trackCount);
        for (const auto& bt : c.boneTracks)
            WriteBoneTrack(f, bt);

        // Bone parents
        WriteVec(f, c.boneParents);

        // Sequences
        uint32_t seqCount = static_cast<uint32_t>(c.sequences.size());
        W(f, seqCount);
        for (const auto& s : c.sequences)
            WriteSequence(f, s);

        // Segments (POD, 22 bytes each)
        WriteVec(f, c.animationSegments);
        WriteVec(f, c.animationSegmentSourceTypes);

        // Loop config
        W(f, c.loopConfig.introStartSequence);
        W(f, c.loopConfig.introEndSequence);
        W(f, c.loopConfig.loopStartSequence);
        W(f, c.loopConfig.loopEndSequence);
        W(f, c.loopConfig.hasIntro);
        W(f, c.loopConfig.canPlayIntroReverse);

        // Intermediate bone data
        // std::vector<bool> needs special handling
        uint32_t intCount = static_cast<uint32_t>(c.boneIsIntermediate.size());
        W(f, intCount);
        for (uint32_t i = 0; i < intCount; i++) {
            uint8_t v = c.boneIsIntermediate[i] ? 1 : 0;
            W(f, v);
        }

        WriteVec(f, c.outputToAnimBone);
        WriteVec(f, c.animBoneToOutput);
    }

    static bool ReadClip(std::ifstream& f, Animation::AnimationClip& c)
    {
        if (!ReadStr(f, c.name)) return false;
        if (!R(f, c.duration)) return false;
        if (!R(f, c.minTime)) return false;
        if (!R(f, c.maxTime)) return false;
        if (!R(f, c.totalFrames)) return false;
        if (!R(f, c.modelHash0)) return false;
        if (!R(f, c.modelHash1)) return false;
        if (!R(f, c.geometryScale)) return false;
        int32_t hm = 0;
        if (!R(f, hm)) return false;
        c.hierarchyMode = static_cast<Animation::HierarchyMode>(hm);
        if (!ReadStr(f, c.sourceChunkType)) return false;

        uint32_t trackCount = 0;
        if (!R(f, trackCount)) return false;
        c.boneTracks.resize(trackCount);
        for (uint32_t i = 0; i < trackCount; i++)
            if (!ReadBoneTrack(f, c.boneTracks[i])) return false;

        if (!ReadVec(f, c.boneParents)) return false;

        uint32_t seqCount = 0;
        if (!R(f, seqCount)) return false;
        c.sequences.resize(seqCount);
        for (uint32_t i = 0; i < seqCount; i++)
            if (!ReadSequence(f, c.sequences[i])) return false;

        if (!ReadVec(f, c.animationSegments)) return false;
        if (!ReadVec(f, c.animationSegmentSourceTypes)) return false;

        if (!R(f, c.loopConfig.introStartSequence)) return false;
        if (!R(f, c.loopConfig.introEndSequence)) return false;
        if (!R(f, c.loopConfig.loopStartSequence)) return false;
        if (!R(f, c.loopConfig.loopEndSequence)) return false;
        if (!R(f, c.loopConfig.hasIntro)) return false;
        if (!R(f, c.loopConfig.canPlayIntroReverse)) return false;

        uint32_t intCount = 0;
        if (!R(f, intCount)) return false;
        c.boneIsIntermediate.resize(intCount);
        for (uint32_t i = 0; i < intCount; i++) {
            uint8_t v = 0;
            if (!R(f, v)) return false;
            c.boneIsIntermediate[i] = (v != 0);
        }

        if (!ReadVec(f, c.outputToAnimBone)) return false;
        if (!ReadVec(f, c.animBoneToOutput)) return false;

        return true;
    }
};

} // namespace GW::Cache
