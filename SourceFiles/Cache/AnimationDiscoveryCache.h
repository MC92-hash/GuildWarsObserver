#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace GW::Cache {

struct AnimSourceEntry {
    int mftIndex = -1;
    uint32_t fileHash = 0;
};

struct CachedModelAnimInfo {
    uint32_t modelHash0 = 0;
    uint32_t modelHash1 = 0;
    std::vector<AnimSourceEntry> animSources;

    // Weapon attachment points, resolved once per rig from the skeleton topology (see
    // Animation/WeaponSocket.h). Stored by bind-pose side, not by main/off hand: which side
    // carries the weapon is a render-time choice. Unresolved on a rig with no hands, which is a
    // real answer and not a miss - those agents carry no weapon.
    int32_t negativeXBone = -1;
    int32_t positiveXBone = -1;
    bool    socketResolved = false;
};

class AnimationDiscoveryCache
{
    // v3: added hand_bones
    static constexpr int kCacheVersion = 3;
public:
    void SetDatIdentity(const std::wstring& datPath, uintmax_t datFileSize)
    {
        m_datPath = datPath;
        m_datFileSize = datFileSize;
    }

    bool HasModel(uint32_t modelFileHash) const
    {
        return m_models.count(modelFileHash) > 0;
    }

    const CachedModelAnimInfo* GetModel(uint32_t modelFileHash) const
    {
        auto it = m_models.find(modelFileHash);
        return (it != m_models.end()) ? &it->second : nullptr;
    }

    void SetModel(uint32_t modelFileHash, CachedModelAnimInfo info)
    {
        m_models[modelFileHash] = std::move(info);
        m_dirty = true;
    }

    // Records a resolved weapon socket against a model already in the cache. Kept separate from
    // SetModel because the socket needs a parsed clip, which only exists after the anim sources
    // have been discovered and written. Does not dirty the cache when nothing changed, so a
    // cache-hit load stays a read.
    void SetSocket(uint32_t modelFileHash, int32_t negativeXBone, int32_t positiveXBone, bool resolved)
    {
        auto it = m_models.find(modelFileHash);
        if (it == m_models.end())
            return;

        auto& info = it->second;
        if (info.socketResolved == resolved &&
            info.negativeXBone == negativeXBone &&
            info.positiveXBone == positiveXBone)
            return;

        info.negativeXBone  = negativeXBone;
        info.positiveXBone  = positiveXBone;
        info.socketResolved = resolved;
        m_dirty = true;
    }

    bool IsDirty() const { return m_dirty; }

    bool LoadFromFile(const std::filesystem::path& cachePath,
                      const std::wstring& currentDatPath,
                      uintmax_t currentDatSize)
    {
        m_models.clear();
        m_dirty = false;

        std::ifstream file(cachePath);
        if (!file.is_open())
            return false;

        std::wstring storedDatPath;
        uintmax_t storedDatSize = 0;
        int storedVersion = 0;

        std::string line;
        enum class Section { Header, Model, AnimSources } section = Section::Header;
        uint32_t currentModelHash = 0;
        CachedModelAnimInfo currentInfo;

        auto flushModel = [&]() {
            if (currentModelHash != 0)
                m_models[currentModelHash] = std::move(currentInfo);
            currentModelHash = 0;
            currentInfo = {};
        };

        while (std::getline(file, line))
        {
            if (line.empty()) continue;

            if (line[0] == '#') continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);

            if (key == "cache_version") {
                try { storedVersion = std::stoi(val); } catch (...) {}
            }
            else if (key == "dat_path") {
                storedDatPath = Utf8ToWide(val);
            }
            else if (key == "dat_size") {
                try { storedDatSize = std::stoull(val); } catch (...) {}
            }
            else if (key == "model") {
                flushModel();
                try { currentModelHash = static_cast<uint32_t>(std::stoul(val, nullptr, 0)); } catch (...) {}
            }
            else if (key == "model_hash0") {
                try { currentInfo.modelHash0 = static_cast<uint32_t>(std::stoul(val, nullptr, 0)); } catch (...) {}
            }
            else if (key == "model_hash1") {
                try { currentInfo.modelHash1 = static_cast<uint32_t>(std::stoul(val, nullptr, 0)); } catch (...) {}
            }
            // "negX,posX" on a rig with hands, "none" on one without. A missing key means the
            // rig predates this field; both it and "none" simply re-resolve, which is a walk
            // over an already-parsed skeleton and costs nothing.
            else if (key == "hand_bones" && val != "none") {
                std::istringstream ss(val);
                std::string token;
                int32_t negX = -1, posX = -1;
                if (std::getline(ss, token, ','))
                    try { negX = std::stoi(token); } catch (...) {}
                if (std::getline(ss, token, ','))
                    try { posX = std::stoi(token); } catch (...) {}
                currentInfo.negativeXBone = negX;
                currentInfo.positiveXBone = posX;
                currentInfo.socketResolved = (negX >= 0 && posX >= 0);
            }
            else if (key == "anim_src") {
                AnimSourceEntry entry;
                std::istringstream ss(val);
                std::string token;
                if (std::getline(ss, token, ','))
                    try { entry.mftIndex = std::stoi(token); } catch (...) {}
                if (std::getline(ss, token, ','))
                    try { entry.fileHash = static_cast<uint32_t>(std::stoul(token, nullptr, 0)); } catch (...) {}
                if (entry.mftIndex >= 0)
                    currentInfo.animSources.push_back(entry);
            }
        }
        flushModel();

        if (storedVersion != kCacheVersion ||
            storedDatPath != currentDatPath || storedDatSize != currentDatSize) {
            m_models.clear();
            return false;
        }

        m_datPath = currentDatPath;
        m_datFileSize = currentDatSize;
        return !m_models.empty();
    }

    bool SaveToFile(const std::filesystem::path& cachePath) const
    {
        std::ofstream file(cachePath);
        if (!file.is_open())
            return false;

        file << "# GW Observer animation discovery cache\n";
        file << "cache_version=" << kCacheVersion << "\n";
        file << "dat_path=" << WideToUtf8(m_datPath) << "\n";
        file << "dat_size=" << m_datFileSize << "\n";

        for (const auto& [modelFileHash, info] : m_models)
        {
            file << "model=0x" << std::hex << modelFileHash << std::dec << "\n";
            file << "model_hash0=0x" << std::hex << info.modelHash0 << std::dec << "\n";
            file << "model_hash1=0x" << std::hex << info.modelHash1 << std::dec << "\n";
            if (info.socketResolved)
                file << "hand_bones=" << info.negativeXBone << "," << info.positiveXBone << "\n";
            else
                file << "hand_bones=none\n";
            for (const auto& src : info.animSources)
                file << "anim_src=" << src.mftIndex << ",0x" << std::hex << src.fileHash << std::dec << "\n";
        }

        return true;
    }

    static std::filesystem::path GetDefaultCachePath()
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        return std::filesystem::path(exePath).parent_path() / "animation_cache.ini";
    }

private:
    std::wstring m_datPath;
    uintmax_t m_datFileSize = 0;
    std::unordered_map<uint32_t, CachedModelAnimInfo> m_models;
    bool m_dirty = false;

    static std::string WideToUtf8(const std::wstring& wide)
    {
        if (wide.empty()) return {};
        int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
        std::string result(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), result.data(), size, nullptr, nullptr);
        return result;
    }

    static std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty()) return {};
        int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
        std::wstring result(size, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), result.data(), size);
        return result;
    }
};

} // namespace GW::Cache
