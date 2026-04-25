#include "pch.h"
#include "DATManager.h"
FFNA_MapFile DATManager::parse_ffna_map_file(int index)
{
    MFTEntry* mft_entry = m_dat.get_MFT_entry_ptr(index);
    if (! mft_entry)
        throw "mft_entry not found.";

    // Get decompressed file data
    HANDLE file_handle = m_dat.get_dat_filehandle(m_dat_filepath.c_str());
    auto data = m_dat.readFile(file_handle, index, true);
    std::span<unsigned char> file_data(data, mft_entry->uncompressedSize);
    CloseHandle(file_handle);

    FFNA_MapFile ffna_map_file(0, file_data);
    delete[] data;

    return ffna_map_file;
}

FFNA_ModelFile DATManager::parse_ffna_model_file(int index)
{
    MFTEntry* mft_entry = m_dat.get_MFT_entry_ptr(index);
    if (! mft_entry)
        throw "mft_entry not found.";

    // Get decompressed file data
    HANDLE file_handle = m_dat.get_dat_filehandle(m_dat_filepath.c_str());
    auto data = m_dat.readFile(file_handle, index, true);
    std::span<unsigned char> file_data(data, mft_entry->uncompressedSize);
    CloseHandle(file_handle);

    FFNA_ModelFile ffna_model_file(0, file_data);
    delete[] data;

    return ffna_model_file;
}

FFNA_ModelFile_Other DATManager::parse_ffna_model_file_other(int index)
{
    MFTEntry* mft_entry = m_dat.get_MFT_entry_ptr(index);
    if (!mft_entry)
        throw "mft_entry not found.";

    // Get decompressed file data
    HANDLE file_handle = m_dat.get_dat_filehandle(m_dat_filepath.c_str());
    auto data = m_dat.readFile(file_handle, index, true);
    std::span<unsigned char> file_data(data, mft_entry->uncompressedSize);
    CloseHandle(file_handle);

    FFNA_ModelFile_Other ffna_model_file_other(0, file_data);
    delete[] data;

    return ffna_model_file_other;
}

bool DATManager::is_other_model_format(int index)
{
    MFTEntry* mft_entry = m_dat.get_MFT_entry_ptr(index);
    if (!mft_entry || mft_entry->uncompressedSize < 13)
        return false;

    // Get decompressed file data
    HANDLE file_handle = m_dat.get_dat_filehandle(m_dat_filepath.c_str());
    auto data = m_dat.readFile(file_handle, index, true);
    std::span<unsigned char> file_data(data, mft_entry->uncompressedSize);
    CloseHandle(file_handle);

    bool is_other = IsOtherModelFormat(file_data);
    delete[] data;

    return is_other;
}

AMAT_file DATManager::parse_amat_file(int index)
{
    MFTEntry* mft_entry = m_dat.get_MFT_entry_ptr(index);
    if (! mft_entry)
        throw "mft_entry not found.";

    // Get decompressed file data
    HANDLE file_handle = m_dat.get_dat_filehandle(m_dat_filepath.c_str());
    auto data = m_dat.readFile(file_handle, index, true);
    std::span<unsigned char> file_data(data, mft_entry->uncompressedSize);
    CloseHandle(file_handle);

    AMAT_file amat_file(file_data.data(), file_data.size());
    delete[] data;

    return amat_file;
}

DatTexture DATManager::parse_ffna_texture_file(int index)
{
    MFTEntry* mft_entry = m_dat.get_MFT_entry_ptr(index);
    if (! mft_entry)
        throw "mft_entry not found.";

    // Get decompressed file data
    HANDLE file_handle = m_dat.get_dat_filehandle(m_dat_filepath.c_str());
    auto data = m_dat.readFile(file_handle, index, true);
    std::span<unsigned char> file_data(data, mft_entry->uncompressedSize);
    CloseHandle(file_handle);

    // Process texture data
    auto dat_texture = ProcessImageFile(file_data.data(), mft_entry->uncompressedSize);

    delete[] data;

    return dat_texture;
}

std::vector<uint8_t> DATManager::parse_dds_file(int index)
{
    MFTEntry* mft_entry = m_dat.get_MFT_entry_ptr(index);
    if (! mft_entry)
        throw "mft_entry not found.";

    // Get decompressed file data
    HANDLE file_handle = m_dat.get_dat_filehandle(m_dat_filepath.c_str());
    auto data = m_dat.readFile(file_handle, index, true);
    std::vector<uint8_t> file_data(data, data + mft_entry->uncompressedSize);
    CloseHandle(file_handle);

    delete[] data;

    return file_data;
}

bool DATManager::save_raw_decompressed_data_to_file(int index, std::wstring filepath)
{
    MFTEntry* mft_entry = m_dat.get_MFT_entry_ptr(index);
    if (! mft_entry)
    {
        return false;
    }

    HANDLE file_handle = m_dat.get_dat_filehandle(m_dat_filepath.c_str());
    std::unique_ptr<unsigned char[]> data(m_dat.readFile(file_handle, index, true));
    if (!data)
    {
        // Handle error in reading file
        return false;
    }

    std::ofstream output_file(filepath, std::ios::out | std::ios::binary);
    if (output_file.is_open())
    {
        output_file.write(reinterpret_cast<char*>(data.get()), mft_entry->uncompressedSize);
        output_file.close();
        return true;
    }
    else
    {
        // Handle file opening error
        return false;
    }
}

void DATManager::EnsureTypeClassified(int index)
{
    auto* entry = m_dat.get_MFT_entry_ptr(index);
    if (!entry || entry->type != NOTREAD)
        return;  // already classified or invalid

    HANDLE fh = m_dat.get_dat_filehandle(m_dat_filepath.c_str());
    if (fh == INVALID_HANDLE_VALUE)
        return;

    unsigned char* data = m_dat.readFile(fh, index, false);
    if (data)
        delete[] data;
    CloseHandle(fh);
}

std::vector<int> DATManager::FindAnimationFiles(uint32_t modelHash0, uint32_t modelHash1)
{
    std::vector<int> results;
    if (modelHash0 == 0 && modelHash1 == 0)
        return results;

    const auto& mft = m_dat.get_MFT();
    for (int i = 0; i < (int)mft.size(); i++)
    {
        if (mft[i].type != FFNA_Type2) continue;
        if (mft[i].animModelHash0 == modelHash0 && mft[i].animModelHash1 == modelHash1)
            results.push_back(i);
    }
    return results;
}

void DATManager::read_all_files()
{
    const auto num_files = m_dat.getNumFiles();

    // Get the number of available threads
    const auto num_threads = std::thread::hardware_concurrency();

    // Fill the concurrent queue with file indices
    Concurrency::concurrent_queue<int> file_indices_queue;
    for (int i = 0; i < num_files; ++i)
    {
        file_indices_queue.push(i);
    }

    // Start the file reading threads
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(&DATManager::read_files_thread, this, std::ref(file_indices_queue));
    }

    // Wait for all threads to finish
    for (auto& thread : threads)
    {
        thread.join();
    }

    const auto& mft = get_MFT();
    for (const auto& entry : mft) {
        const auto num_files_for_type_it = num_files_per_type.find(static_cast<FileType>(entry.type));
        if (num_files_for_type_it != num_files_per_type.end()) {
            num_files_for_type_it->second += 1;
        }
        else {
            num_files_per_type.emplace(static_cast<FileType>(entry.type), 1);
        }
    }

    m_animIndexReady.store(true, std::memory_order_release);

    if (file_indices_queue.empty())
    {
        // Save cache so the next launch skips the full scan
        SaveMftCache();
        m_initialization_state = InitializationState::Completed;
    }
}

// ── MFT type cache ────────────────────────────────────────────────────

static constexpr uint32_t kMftCacheMagic   = 0x4D465443; // "MFTC"
static constexpr uint32_t kMftCacheVersion = 3;  // v3: added animModelHash0/1

struct MftCacheHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t entryCount;
    uint64_t datFileSize;  // for invalidation
};

std::filesystem::path DATManager::GetMftCachePath() const
{
    return std::filesystem::path(m_dat_filepath).parent_path() / "gw_dat_cache.bin";
}

bool DATManager::LoadMftCache()
{
    auto cachePath = GetMftCachePath();
    if (!std::filesystem::exists(cachePath))
        return false;

    // Check DAT file size for invalidation
    std::error_code ec;
    auto datSize = std::filesystem::file_size(m_dat_filepath, ec);
    if (ec)
        return false;

    std::ifstream file(cachePath, std::ios::binary);
    if (!file.is_open())
        return false;

    MftCacheHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || header.magic != kMftCacheMagic || header.version != kMftCacheVersion)
        return false;

    auto& mft = m_dat.get_MFT();
    if (header.entryCount != static_cast<uint32_t>(mft.size()))
        return false;
    if (header.datFileSize != datSize)
        return false;

    // Read per-entry data
    for (uint32_t i = 0; i < header.entryCount; ++i)
    {
        int32_t type = 0, uncompressedSize = 0;
        uint32_t murmurhash = 0, chunkCount = 0;
        uint32_t animHash0 = 0, animHash1 = 0;

        file.read(reinterpret_cast<char*>(&type), sizeof(type));
        file.read(reinterpret_cast<char*>(&uncompressedSize), sizeof(uncompressedSize));
        file.read(reinterpret_cast<char*>(&murmurhash), sizeof(murmurhash));
        file.read(reinterpret_cast<char*>(&chunkCount), sizeof(chunkCount));
        file.read(reinterpret_cast<char*>(&animHash0), sizeof(animHash0));
        file.read(reinterpret_cast<char*>(&animHash1), sizeof(animHash1));

        if (!file)
            return false;

        mft[i].type = type;
        mft[i].uncompressedSize = uncompressedSize;
        mft[i].murmurhash3 = murmurhash;
        mft[i].animModelHash0 = animHash0;
        mft[i].animModelHash1 = animHash1;

        if (chunkCount > 0)
        {
            mft[i].chunk_ids.resize(chunkCount);
            file.read(reinterpret_cast<char*>(mft[i].chunk_ids.data()),
                      chunkCount * sizeof(uint32_t));
            if (!file)
                return false;
        }
    }

    return true;
}

void DATManager::SaveMftCache()
{
    auto cachePath = GetMftCachePath();

    std::error_code ec;
    auto datSize = std::filesystem::file_size(m_dat_filepath, ec);
    if (ec)
        return;

    const auto& mft = m_dat.get_MFT();

    std::ofstream file(cachePath, std::ios::binary);
    if (!file.is_open())
        return;

    MftCacheHeader header{};
    header.magic = kMftCacheMagic;
    header.version = kMftCacheVersion;
    header.entryCount = static_cast<uint32_t>(mft.size());
    header.datFileSize = datSize;

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    for (const auto& entry : mft)
    {
        int32_t type = entry.type;
        int32_t uncompressedSize = entry.uncompressedSize;
        uint32_t murmurhash = entry.murmurhash3;
        uint32_t chunkCount = static_cast<uint32_t>(entry.chunk_ids.size());
        uint32_t animHash0 = entry.animModelHash0;
        uint32_t animHash1 = entry.animModelHash1;

        file.write(reinterpret_cast<const char*>(&type), sizeof(type));
        file.write(reinterpret_cast<const char*>(&uncompressedSize), sizeof(uncompressedSize));
        file.write(reinterpret_cast<const char*>(&murmurhash), sizeof(murmurhash));
        file.write(reinterpret_cast<const char*>(&chunkCount), sizeof(chunkCount));
        file.write(reinterpret_cast<const char*>(&animHash0), sizeof(animHash0));
        file.write(reinterpret_cast<const char*>(&animHash1), sizeof(animHash1));

        if (chunkCount > 0)
        {
            file.write(reinterpret_cast<const char*>(entry.chunk_ids.data()),
                       chunkCount * sizeof(uint32_t));
        }
    }
}

// ── Background scan ───────────────────────────────────────────────────

static unsigned char* SafeReadDatFile(GWDat& dat, HANDLE file_handle, int index)
{
    __try
    {
        return dat.readFile(file_handle, index, false);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

static void ScanFFNA2ForAnimHashes(const unsigned char* data, size_t size,
                                   std::vector<std::pair<uint32_t, uint32_t>>& outPairs)
{
    if (size < 5 || data[0] != 'f' || data[1] != 'f' || data[2] != 'n' || data[3] != 'a')
        return;
    size_t off = 5;
    while (off + 8 <= size) {
        uint32_t cid, csz;
        std::memcpy(&cid, &data[off], 4);
        std::memcpy(&csz, &data[off + 4], 4);
        if (cid == 0 || csz == 0 || off + 8 + csz > size) break;
        size_t cDataOff = off + 8;
        if (cid == 0x00000BB9 && csz >= 44) {
            uint32_t h0, h1;
            std::memcpy(&h0, &data[cDataOff + 0x0C], 4);
            std::memcpy(&h1, &data[cDataOff + 0x10], 4);
            if (h0 != 0 || h1 != 0)
                outPairs.push_back({ h0, h1 });
        }
        else if (cid == 0x00000FA1 && csz >= 88) {
            uint32_t h0, h1;
            std::memcpy(&h0, &data[cDataOff + 0x0C], 4);
            std::memcpy(&h1, &data[cDataOff + 0x10], 4);
            if (h0 != 0 || h1 != 0)
                outPairs.push_back({ h0, h1 });
        }
        off += 8 + csz;
    }
}

void DATManager::read_files_thread(Concurrency::concurrent_queue<int>& file_indices_queue)
{
    HANDLE file_handle = m_dat.get_dat_filehandle(m_dat_filepath.c_str());
    if (file_handle == INVALID_HANDLE_VALUE || file_handle == nullptr)
    {
        m_num_running_dat_reader_threads.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    std::vector<std::pair<uint64_t, AnimIndexEntry>> localEntries;
    int index;

    while (file_indices_queue.try_pop(index))
    {
        try
        {
            unsigned char* data = SafeReadDatFile(m_dat, file_handle, index);
            if (data) {
                MFTEntry* entry = m_dat.get_MFT_entry_ptr(index);
                if (entry && entry->type == FFNA_Type2 && entry->uncompressedSize > 0) {
                    std::vector<std::pair<uint32_t, uint32_t>> hashPairs;
                    ScanFFNA2ForAnimHashes(data, entry->uncompressedSize, hashPairs);
                    AnimIndexEntry ae{ index, static_cast<uint32_t>(entry->Hash) };
                    for (auto& [h0, h1] : hashPairs)
                        localEntries.push_back({ MakeAnimKey(h0, h1), ae });
                }
                delete[] data;
            }
            m_num_types_read.fetch_add(1, std::memory_order_relaxed);
        }
        catch (...)
        {
        }
    }

    if (!localEntries.empty()) {
        std::lock_guard<std::mutex> lock(m_animIndexMutex);
        for (auto& [key, entry] : localEntries)
            m_animIndex[key].push_back(entry);
    }

    CloseHandle(file_handle);

    m_num_running_dat_reader_threads.fetch_sub(1, std::memory_order_relaxed);
}
