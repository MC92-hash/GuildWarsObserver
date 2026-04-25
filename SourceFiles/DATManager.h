#pragma once
#include "AMAT_file.h"
#include "FFNA_MapFile.h"
#include "FFNA_ModelFile.h"
#include "FFNA_ModelFile_Other.h"
#include "Cache/FileCache.h"
#include <ppl.h>
#include <concurrent_queue.h>
#include <mutex>

enum InitializationState
{
    NotStarted,
    Started,
    IndexReady,  // MFT metadata loaded, hash index can be built, replays can open
    Completed    // Background scan done, all types classified, cache written
};

struct AnimIndexEntry {
    int mftIndex;
    uint32_t fileHash;
};

class DATManager
{
public:
    bool Init(std::wstring dat_filepath)
    {
        m_initialization_state = InitializationState::Started;

        m_dat_filepath = dat_filepath;
        int result = m_dat.readDat(m_dat_filepath.c_str());
        if (result == 0)
        {
            m_initialization_state = InitializationState::NotStarted;
            return false;
        }

        m_fileCache.SetFileLoader([this](uint32_t fileId) -> std::shared_ptr<std::vector<uint8_t>> {
            return LoadFileForCache(fileId);
        });

        // Try to load cached MFT type data from a previous scan
        if (LoadMftCache())
        {
            // Cache loaded — count files per type and mark as done
            const auto& mft = get_MFT();
            for (const auto& entry : mft) {
                const auto it = num_files_per_type.find(static_cast<FileType>(entry.type));
                if (it != num_files_per_type.end())
                    it->second += 1;
                else
                    num_files_per_type.emplace(static_cast<FileType>(entry.type), 1);
            }
            m_num_types_read.store(static_cast<int>(mft.size()));
            m_initialization_state = InitializationState::Completed;
            return true;
        }

        // MFT metadata is loaded — hash index can be built and replays can open
        m_initialization_state = InitializationState::IndexReady;

        // Full scan runs in the background to classify all file types
        auto read_all_thread = std::thread(&DATManager::read_all_files, this);
        read_all_thread.detach();

        return true;
    }

    std::atomic<InitializationState> m_initialization_state{NotStarted};

    int get_num_files_type_read() { return m_num_types_read; }
    int get_num_files() { return m_dat.getNumFiles(); }

    const std::wstring get_filepath() {
        return m_dat_filepath;
    }

    std::vector<MFTEntry>& get_MFT() { return m_dat.get_MFT(); }

    FFNA_MapFile parse_ffna_map_file(int index);
    FFNA_ModelFile parse_ffna_model_file(int index);
    FFNA_ModelFile_Other parse_ffna_model_file_other(int index);
    bool is_other_model_format(int index);
    AMAT_file parse_amat_file(int index);
    DatTexture parse_ffna_texture_file(int index);
    std::vector<uint8_t> parse_dds_file(int index);

    bool save_raw_decompressed_data_to_file(int index, std::wstring filepath);

    // Classify a single file's type on-demand if the background scan hasn't reached it yet.
    void EnsureTypeClassified(int index);

    // Find all MFT indices whose BB9/FA1 animation header matches the given model hashes.
    // Uses the animModelHash0/1 fields populated during background scan — no file I/O.
    std::vector<int> FindAnimationFiles(uint32_t modelHash0, uint32_t modelHash1);

    unsigned char* read_file(int index)
    {
        HANDLE file_handle = m_dat.get_dat_filehandle(m_dat_filepath.c_str());
        unsigned char* data = m_dat.readFile(file_handle, index, true);
        CloseHandle(file_handle);
        return data;
    }

    // Overload that reuses an existing file handle (avoids CreateFile/CloseHandle per call)
    unsigned char* read_file(int index, HANDLE file_handle)
    {
        return m_dat.readFile(file_handle, index, true);
    }

    // Opens a persistent file handle for batch operations. Caller must CloseHandle when done.
    HANDLE open_dat_handle()
    {
        return m_dat.get_dat_filehandle(m_dat_filepath.c_str());
    }

    // Read a file through the LRU cache (thread-safe, avoids redundant disk reads)
    std::shared_ptr<std::vector<uint8_t>> read_file_cached(uint32_t mftIndex)
    {
        return m_fileCache.GetFile(mftIndex);
    }

    GW::Cache::FileCache& GetFileCache() { return m_fileCache; }

    bool IsAnimIndexReady() const { return m_animIndexReady.load(std::memory_order_acquire); }

    std::vector<AnimIndexEntry> LookupAnimByModelHash(uint64_t key) const
    {
        std::lock_guard<std::mutex> lock(m_animIndexMutex);
        auto it = m_animIndex.find(key);
        if (it != m_animIndex.end())
            return it->second;
        return {};
    }

    static uint64_t MakeAnimKey(uint32_t h0, uint32_t h1) { return (uint64_t(h0) << 32) | h1; }

    int get_num_files_for_type(FileType type) {
        return num_files_per_type[type];
    }

private:
    std::wstring m_dat_filepath;
    GWDat m_dat;

    std::atomic<int> m_num_types_read{0};
    std::atomic<int> m_num_running_dat_reader_threads{0};

    std::unordered_map<FileType, int> num_files_per_type;

    GW::Cache::FileCache m_fileCache{512 * 1024 * 1024};

    mutable std::mutex m_animIndexMutex;
    std::unordered_map<uint64_t, std::vector<AnimIndexEntry>> m_animIndex;
    std::atomic<bool> m_animIndexReady{false};

    std::shared_ptr<std::vector<uint8_t>> LoadFileForCache(uint32_t fileId)
    {
        HANDLE fh = m_dat.get_dat_filehandle(m_dat_filepath.c_str());
        if (fh == INVALID_HANDLE_VALUE || fh == nullptr)
            return nullptr;
        unsigned char* raw = m_dat.readFile(fh, static_cast<int>(fileId), true);
        CloseHandle(fh);
        if (!raw)
            return nullptr;
        auto* entry = m_dat.get_MFT_entry_ptr(static_cast<int>(fileId));
        size_t size = entry ? entry->uncompressedSize : 0;
        if (size == 0) { delete[] raw; return nullptr; }
        auto vec = std::make_shared<std::vector<uint8_t>>(raw, raw + size);
        delete[] raw;
        return vec;
    }

private:
    void read_all_files();
    void read_files_thread(Concurrency::concurrent_queue<int>& file_indices_queue);

    // MFT type cache — avoids re-decompressing all files on subsequent launches
    bool LoadMftCache();
    void SaveMftCache();
    std::filesystem::path GetMftCachePath() const;
};
