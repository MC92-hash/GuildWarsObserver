#pragma once

// Item icons (the 2D miniature, not the 3D model) for the weapon-set panel, decoded from Gw.dat.
//
// An item's model_file_id in the recording is a Gw.dat file number. Looking that number up in the
// hash list lands on the file's *stream 2*, which is not the icon - a file is a chain of streams
// linked through MFTEntry::ID, and the icon lives in stream 1 (stream 0 as a fallback). Verified
// against a retail dat: file 9454 heads at stream 2 and reaches streams {0, 1, 11}, and it is
// stream 1 that decodes to an image. This is the same stream GWToolbox reads.
//
// The catch is that MFTEntry::ID refers to a *raw* MFT slot, while DATManager indexes its loaded
// vector differently: a file listed under several file numbers is pushed once per alias, and a
// retail dat has ~34,700 of those, so the two numbering schemes drift apart almost immediately.
// BuildIndex rebuilds the correspondence by replaying the loader against the same hash list.
//
// Dye tinting is not applied: the mask is a separate stream (0xC) and weapon icons are rarely dyed.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "AtexReader.h"
#include "DATManager.h"

namespace EquipmentIcons
{
    inline constexpr uint32_t kInlineTextureDXT3 = 0xFA3;
    inline constexpr int kIconStream = 1;
    inline constexpr int kFallbackStream = 0;
    inline constexpr int kMaxChainHops = 16;

    namespace detail
    {
        struct State
        {
            ID3D11Device* device = nullptr;
            std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> textures;
            std::unordered_set<uint32_t> failed;   // ids known to have no usable icon
            std::unordered_map<uint32_t, int> hashToIndex; // file number -> loaded vector index
            std::unordered_map<int, int>      rawToIndex;  // raw MFT slot -> loaded vector index
            bool indexBuilt = false;
        };

        inline State& Get()
        {
            static State state;
            return state;
        }

        inline void BuildIndex(DATManager* dat, State& state)
        {
            if (state.indexBuilt) return;
            state.indexBuilt = true;

            const auto& mft = dat->get_MFT();
            if (mft.size() < 16) return;

            state.hashToIndex.reserve(mft.size());
            for (size_t i = 0; i < mft.size(); i++)
            {
                const auto hash = static_cast<uint32_t>(mft[i].Hash);
                if (hash) state.hashToIndex.emplace(hash, static_cast<int>(i)); // first wins
            }

            // Reserved slot 1 holds the hash list the loader itself consumed.
            const auto& hash_list = mft[1];
            if (hash_list.Size <= 0) return;

            std::ifstream file(dat->get_filepath(), std::ios::binary);
            if (!file) return;

            struct Alias { int32_t fileNumber; int32_t fileOffset; };
            std::vector<Alias> aliases(static_cast<size_t>(hash_list.Size) / sizeof(Alias));
            if (aliases.empty()) return;

            file.seekg(hash_list.Offset);
            file.read(reinterpret_cast<char*>(aliases.data()),
                      static_cast<std::streamsize>(aliases.size() * sizeof(Alias)));
            if (!file) return;

            std::sort(aliases.begin(), aliases.end(),
                      [](const Alias& a, const Alias& b) { return a.fileOffset < b.fileOffset; });

            size_t alias = 0;
            while (alias < aliases.size() && aliases[alias].fileOffset < 16) alias++;

            // Vector index 15 is where raw slot 16 landed: 15 reserved entries precede it.
            const auto count = static_cast<int>(mft.size());
            state.rawToIndex.reserve(mft.size());
            for (int slot = 16, index = 15; index < count; slot++)
            {
                state.rawToIndex.emplace(slot, index);
                index++;
                if (alias < aliases.size() && slot == aliases[alias].fileOffset)
                {
                    while (alias + 1 < aliases.size() &&
                           aliases[alias].fileOffset == aliases[alias + 1].fileOffset)
                    {
                        alias++;
                        index++; // this raw slot was pushed again under another file number
                    }
                    alias++;
                }
            }
        }

        // Follows the stream chain to the icon. Returns a loaded-vector index, or -1.
        inline int FindIconStream(const std::vector<MFTEntry>& mft, const State& state, int head)
        {
            int fallback = -1;
            int current = head;
            for (int hop = 0; hop < kMaxChainHops; hop++)
            {
                if (current < 0 || current >= static_cast<int>(mft.size())) break;
                const auto& entry = mft[current];
                if (entry.c == kIconStream) return current;
                if (entry.c == kFallbackStream && fallback < 0) fallback = current;

                if (entry.ID <= 0) break;
                const auto next = state.rawToIndex.find(entry.ID);
                if (next == state.rawToIndex.end()) break;
                current = next->second;
            }
            return fallback;
        }

        // Narrows a decompressed dat file down to the bytes that hold the image.
        inline bool ExtractImageBytes(const std::vector<uint8_t>& file, std::vector<uint8_t>& out)
        {
            const uint8_t* p = file.data();
            size_t n = file.size();
            if (n < 4) return false;

            if (n >= 5 && memcmp(p, "ffna", 4) == 0)
            {
                // "ffna" plus a type byte, then {uint32 id, uint32 size, payload} chunks.
                size_t offset = 5;
                bool found = false;
                while (offset + 8 <= n)
                {
                    uint32_t id = 0, size = 0;
                    memcpy(&id, p + offset, 4);
                    memcpy(&size, p + offset + 4, 4);
                    if (size > n - offset - 8) break; // truncated or misparsed
                    if (id == kInlineTextureDXT3)
                    {
                        p += offset + 8;
                        n = size;
                        found = true;
                        break;
                    }
                    offset += 8 + size;
                }
                if (!found) return false;
            }

            if (n < 4) return false;
            if (memcmp(p, "ATEX", 4) != 0 && memcmp(p, "ATTX", 4) != 0) return false;

            // Copied rather than decoded in place: the source belongs to the dat's LRU cache and
            // ProcessImageFile writes to what it is given.
            out.assign(p, p + n);
            return true;
        }
    }

    // Icon for a recorded model_file_id, or nullptr when the dat has no usable image for it.
    // Decoded once and cached; failures are remembered so a missing skin is not retried per frame.
    inline ImTextureID Get(DATManager* dat, ID3D11Device* device, uint32_t modelFileId)
    {
        if (!dat || !device || !modelFileId) return nullptr;

        auto& state = detail::Get();
        if (device != state.device)
        {
            state.textures.clear();
            state.failed.clear();
            state.device = device;
        }

        if (const auto it = state.textures.find(modelFileId); it != state.textures.end())
            return reinterpret_cast<ImTextureID>(it->second.Get());
        if (state.failed.contains(modelFileId)) return nullptr;

        detail::BuildIndex(dat, state);

        const auto found = state.hashToIndex.find(modelFileId);
        if (found == state.hashToIndex.end()) { state.failed.insert(modelFileId); return nullptr; }

        const int stream = detail::FindIconStream(dat->get_MFT(), state, found->second);
        if (stream < 0) { state.failed.insert(modelFileId); return nullptr; }

        const auto file = dat->read_file_cached(static_cast<uint32_t>(stream));
        std::vector<uint8_t> image;
        if (!file || !detail::ExtractImageBytes(*file, image))
        {
            state.failed.insert(modelFileId);
            return nullptr;
        }

        const DatTexture tex = ProcessImageFile(image.data(), static_cast<int>(image.size()));
        if (tex.width <= 0 || tex.height <= 0 || tex.rgba_data.empty())
        {
            state.failed.insert(modelFileId);
            return nullptr;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(tex.width);
        desc.Height = static_cast<UINT>(tex.height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        // The decoder's "RGBA" is B8G8R8A8 in memory, the same assumption
        // TextureManager::CreateTextureFromRGBA makes.
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = tex.rgba_data.data();
        init.SysMemPitch = static_cast<UINT>(tex.width) * 4;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture2d;
        if (FAILED(device->CreateTexture2D(&desc, &init, &texture2d)))
        {
            state.failed.insert(modelFileId);
            return nullptr;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device->CreateShaderResourceView(texture2d.Get(), nullptr, &srv)))
        {
            state.failed.insert(modelFileId);
            return nullptr;
        }

        auto& slot = state.textures[modelFileId];
        slot = srv;
        return reinterpret_cast<ImTextureID>(slot.Get());
    }

    inline void Clear()
    {
        auto& state = detail::Get();
        state.textures.clear();
        state.failed.clear();
    }
}
