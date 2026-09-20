#pragma once
#include "AtexReader.h"
#include "DirectXTex/DirectXTex.h"

inline UINT BytesPerPixel(DXGI_FORMAT format)
{
	switch (format)
	{
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SINT:
		return 1;
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return 4;
	default:
		return 0; // Return 0 for unsupported formats
	}
}

struct TextureData
{
	int textureID;
	int width;
	int height;
	std::vector<RGBA> rgba_data;
};

// One level of an already-authored mip chain (level 0 first). Pitches come straight from the
// decoder, so a source whose rows are padded still uploads correctly.
struct MipLevelSource
{
	const void* data;
	UINT row_pitch;
	UINT slice_pitch;
};

class TextureManager
{
public:
	TextureManager(ID3D11Device* device, ID3D11DeviceContext* device_context)
		: m_device(device)
		  , m_deviceContext(device_context) { }

	~TextureManager() { Clear(); }

	int AddTexture(const void* data, UINT width, UINT height, DXGI_FORMAT format, int file_hash,
	               bool autoGenerateMipMaps = true)
	{
		if (cached_textures.contains(file_hash))
			return cached_textures[file_hash].textureID;

		if (!data || width <= 0 || height <= 0) { return -1; }

		// Every pitch below is width * BytesPerPixel(format), and BytesPerPixel() answers 0 for a
		// format it does not know (block-compressed, half-float, 16-bit, anything but the three it
		// lists). A pitch of 0 is not a benign wrong number: it reaches the display driver as the
		// stride of a row, and dividing a size by it is exactly what a driver does with it. This
		// entry point only ever knew how to upload a tightly packed 1- or 4-byte texel, so say so
		// here rather than hand the driver an impossible stride.
		if (BytesPerPixel(format) == 0) { return -1; }

		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = width;
		texDesc.Height = height;
		texDesc.MipLevels = autoGenerateMipMaps ? 0 : 1;
		texDesc.ArraySize = 1;
		texDesc.Format = format;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (autoGenerateMipMaps ? D3D11_BIND_RENDER_TARGET : 0);
		texDesc.CPUAccessFlags = 0;
		texDesc.MiscFlags = autoGenerateMipMaps ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

		D3D11_SUBRESOURCE_DATA* pInitData = nullptr;
		D3D11_SUBRESOURCE_DATA initData = {};
		if (!autoGenerateMipMaps)
		{
			initData.pSysMem = data;
			UINT bytesPerPixel = BytesPerPixel(format);
			initData.SysMemPitch = width * bytesPerPixel;
			initData.SysMemSlicePitch = width * height * bytesPerPixel;
			pInitData = &initData;
		}

		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture2D;
		HRESULT hr = m_device->CreateTexture2D(&texDesc, pInitData, texture2D.GetAddressOf());
		if (FAILED(hr)) { return -1; }

		if (autoGenerateMipMaps)
		{
			UINT bytesPerPixel = BytesPerPixel(format);
			m_deviceContext->UpdateSubresource(texture2D.Get(), 0, nullptr, data, width * bytesPerPixel, 0);
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = autoGenerateMipMaps ? -1 : 1;
		srvDesc.Texture2D.MostDetailedMip = 0;

		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResourceView;
		hr = m_device->CreateShaderResourceView(texture2D.Get(), &srvDesc, shaderResourceView.GetAddressOf());
		if (FAILED(hr)) { return -1; }

		if (autoGenerateMipMaps) { m_deviceContext->GenerateMips(shaderResourceView.Get()); }

		int textureID = m_nextTextureID++;
		m_textures[textureID] = shaderResourceView;

		// The level-0 CPU copy goes through CacheLevel0(), which is the same function
		// GuildWarsMapBrowser's copy of this header calls here. The inline loop this replaced read
		// byteData[i]..byteData[i+3] for every texel whatever the format's real texel size, so any
		// format that is not four bytes per texel over-read the caller's buffer, and it ignored the
		// row pitch so a padded decode was cached skewed. CacheLevel0() stores nothing unless the
		// format really is four bytes per texel and the pitch covers the row.
		CacheLevel0(file_hash, textureID, width, height, format, data,
		            width * BytesPerPixel(format));

		return textureID;
	}

	int AddTextureArray(const std::vector<void*>& dataArray, UINT width, UINT height, DXGI_FORMAT format, int file_hash,
	                    bool autoGenerateMipMaps = true)
	{
		if (!dataArray.size() || width <= 0 || height <= 0) { return -1; }

		// Same reason as AddTexture(): the per-slice pitch below is width * BytesPerPixel(format),
		// and a format this header does not know gives 0, which is not a stride any driver can use.
		if (BytesPerPixel(format) == 0) { return -1; }

		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = width;
		texDesc.Height = height;
		texDesc.MipLevels = autoGenerateMipMaps ? std::floor(std::log2(std::max(width, height))) + 1 : 1;
		texDesc.ArraySize = dataArray.size();
		texDesc.Format = format;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (autoGenerateMipMaps ? D3D11_BIND_RENDER_TARGET : 0);
		texDesc.CPUAccessFlags = 0;
		texDesc.MiscFlags = autoGenerateMipMaps ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture2D;
		HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, texture2D.GetAddressOf());
		if (FAILED(hr)) { return -1; }

		UINT bytesPerPixel = BytesPerPixel(format); // Assuming you have this function

		// 1. Upload the original (mip level 0) texture data
    for (size_t i = 0; i < dataArray.size(); ++i)
    {
        m_deviceContext->UpdateSubresource(
            texture2D.Get(),
            D3D11CalcSubresource(0, i, texDesc.MipLevels),
            nullptr,
            dataArray[i],
            width * bytesPerPixel,
            0
        );
    }


		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.ArraySize = dataArray.size();
		srvDesc.Texture2DArray.MipLevels = autoGenerateMipMaps ? -1 : 1;
		srvDesc.Texture2DArray.MostDetailedMip = 0;

		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResourceView;
		hr = m_device->CreateShaderResourceView(texture2D.Get(), &srvDesc, shaderResourceView.GetAddressOf());
		if (FAILED(hr)) { return -1; }

		    // 2. If auto-generation of mipmaps is enabled, use the hardware-accelerated GenerateMips method
    if (autoGenerateMipMaps)
    {
        // Ensure we have a valid shader resource view
        m_deviceContext->GenerateMips(shaderResourceView.Get());
    }

		int textureID = m_nextTextureID++;
		m_textures[textureID] = shaderResourceView;

		if (file_hash >= 0)
		{
			TextureData textureData;
			textureData.textureID = textureID;
			textureData.width = width;
			textureData.height = height;
			cached_textures[file_hash] = textureData;
		}

		return textureID;
	}


	// Upload a texture that already owns a mip chain (level 0 first).
	//
	// A single entry means "only the top level exists": the texture is then created with a full
	// chain (MipLevels = 0) plus RENDER_TARGET/GENERATE_MIPS and the driver fills the rest, which
	// is what every runtime-decoded ATEX/ATTX texture needs. Two or more entries means the file
	// shipped its own chain (compressed DDS almost always does) and that chain is uploaded as-is -
	// authored mips minify better than anything GenerateMips can reconstruct from level 0.
	int AddTextureWithMips(const std::vector<MipLevelSource>& mips, UINT width, UINT height,
	                       DXGI_FORMAT format, int file_hash)
	{
		if (cached_textures.contains(file_hash))
			return cached_textures[file_hash].textureID;

		if (mips.empty() || !mips[0].data || width <= 0 || height <= 0) { return -1; }

		// A level whose pitch is 0 would reach the driver as the stride of a row. Refuse the whole
		// chain rather than upload one impossible level: a decoder that could not size a level did
		// not produce that level's bytes either.
		for (const MipLevelSource& level : mips)
		{
			if (level.row_pitch == 0) { return -1; }
		}

		const bool has_authored_chain = mips.size() > 1;

		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = width;
		texDesc.Height = height;
		texDesc.MipLevels = has_authored_chain ? static_cast<UINT>(mips.size()) : 0;
		texDesc.ArraySize = 1;
		texDesc.Format = format;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
			(has_authored_chain ? 0 : D3D11_BIND_RENDER_TARGET);
		texDesc.CPUAccessFlags = 0;
		texDesc.MiscFlags = has_authored_chain ? 0 : D3D11_RESOURCE_MISC_GENERATE_MIPS;

		std::vector<D3D11_SUBRESOURCE_DATA> initData;
		if (has_authored_chain)
		{
			initData.reserve(mips.size());
			for (const MipLevelSource& level : mips)
			{
				if (!level.data) { return -1; }

				D3D11_SUBRESOURCE_DATA subresource = {};
				subresource.pSysMem = level.data;
				subresource.SysMemPitch = level.row_pitch;
				subresource.SysMemSlicePitch = level.slice_pitch;
				initData.push_back(subresource);
			}
		}

		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture2D;
		HRESULT hr = m_device->CreateTexture2D(&texDesc, has_authored_chain ? initData.data() : nullptr,
		                                       texture2D.GetAddressOf());
		if (FAILED(hr)) { return -1; }

		if (!has_authored_chain)
		{
			m_deviceContext->UpdateSubresource(texture2D.Get(), 0, nullptr, mips[0].data,
			                                   mips[0].row_pitch, 0);
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = -1; // whole chain, however many levels the resource ended up with
		srvDesc.Texture2D.MostDetailedMip = 0;

		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> shaderResourceView;
		hr = m_device->CreateShaderResourceView(texture2D.Get(), &srvDesc, shaderResourceView.GetAddressOf());
		if (FAILED(hr)) { return -1; }

		if (!has_authored_chain) { m_deviceContext->GenerateMips(shaderResourceView.Get()); }

		int textureID = m_nextTextureID++;
		m_textures[textureID] = shaderResourceView;

		CacheLevel0(file_hash, textureID, width, height, format, mips[0].data, mips[0].row_pitch);

		return textureID;
	}

	// Retire a texture built at runtime, both halves of it.
	//
	// RemoveTexture() only drops the shader resource view; the level-0 CPU copy stays in
	// `cached_textures` for ever, and for a 512x512 RGBA composition that copy is 1 MB. A caller
	// that builds such a canvas under a fresh cache key on every rebuild therefore leaks one D3D
	// texture with its whole mip chain and one megabyte of `rgba_data` per rebuild.
	//
	// This is the entry point that lets the owner of such a key give it back. It takes the HASH
	// (the cache key) rather than the texture id, because that is what the cache is keyed by and
	// what the caller remembers; the texture id comes out of the entry. Returns false when the key
	// is unknown, which is the normal answer for a key that was never built or already retired.
	bool RemoveTextureByHash(int file_hash)
	{
		auto it = cached_textures.find(file_hash);
		if (it == cached_textures.end())
			return false;
		const int textureID = it->second.textureID;
		cached_textures.erase(it);
		m_textures.erase(textureID);
		m_texture_types.erase(textureID);
		return true;
	}

	bool RemoveTexture(int textureID)
	{
		auto it = m_textures.find(textureID);
		if (it != m_textures.end())
		{
			m_textures.erase(it);
			m_texture_types.erase(textureID);
			return true;
		}
		return false;
	}

	ID3D11ShaderResourceView* GetTexture(int textureID) const
	{
		auto it = m_textures.find(textureID);

		if (it != m_textures.end()) { return it->second.Get(); }
		return nullptr;
	}

	std::optional<TextureData> GetTextureDataByHash(int file_hash) const
	{
		auto it = cached_textures.find(file_hash);

		if (it != cached_textures.end()) { return it->second; }
		return std::nullopt;
	}

	int GetTextureIdByHash(int file_hash) const
	{
		auto it = cached_textures.find(file_hash);

		if (it != cached_textures.end()) { return it->second.textureID; }
		return -1;
	}

	// ---- THE DECODED FORMAT OF A TEXTURE, KEYED BY THE ID THIS MANAGER MINTED ------------------
	//
	// PerObjectCB::texture_types carries this to the model pixel shaders, and they switch REAL
	// blend branches on it - OldModelPixelShader's prev_texture_type picks between two source-over
	// forms, so a wrong answer here does not look like a wrong format, it looks like the piece
	// below coming through the piece above.
	//
	// It lives HERE, beside the ids it is keyed on, because a texture id means NOTHING outside the
	// manager that issued it. Every window builds its own TextureManager (MapRenderer::Initialize)
	// and every one of them starts numbering at 0, so the same small integers are handed out again,
	// to different textures, in every window the process opens. A process-wide table keyed on a
	// bare id therefore answers the second window with the first window's formats. Keyed here it
	// dies with the manager that minted the ids, and the question cannot be asked across windows.
	void SetTextureType(int textureID, uint32_t texture_type)
	{
		if (textureID >= 0) { m_texture_types[textureID] = texture_type; }
	}

	// BC1 (1) when this manager never registered one. That is the same default every caller already
	// used for a missed lookup, and the format the great majority of DAT textures decode as.
	uint32_t GetTextureType(int textureID) const
	{
		const auto it = m_texture_types.find(textureID);
		return it != m_texture_types.end() ? it->second : 1u;
	}

	std::vector<ID3D11ShaderResourceView*> GetTextures(const std::vector<int>& textureIDs) const
	{
		std::vector<ID3D11ShaderResourceView*> textures;
		textures.reserve(textureIDs.size());

		for (const int textureID : textureIDs)
		{
			ID3D11ShaderResourceView* texture = GetTexture(textureID);
			if (texture) { textures.push_back(texture); }
			else
			{
				// Handle the case when a texture is not found, if necessary
			}
		}

		return textures;
	}

	HRESULT CreateTextureFromRGBA(int width, int height, const RGBA* data, int* textureID, int file_hash)
	{
		if (width >= 0 && height >= 0)
		{
			DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
			*textureID = AddTexture(data, width, height, format, file_hash);
			return (*textureID >= 0) ? S_OK : E_FAIL;
		}
		return E_FAIL;
	}

	HRESULT CreateTextureFromDDSInMemory(const uint8_t* ddsData, size_t ddsDataSize, int* textureID_out,
	                                     int* width_out, int* height_out, std::vector<RGBA>& rgba_data_out,
	                                     int file_hash);
	HRESULT SaveTextureToFile(ID3D11ShaderResourceView* srv, const wchar_t* filename);

	DatTexture BuildTextureAtlas(const std::vector<DatTexture>& terrain_dat_textures, int num_cols,
	                             int num_rows);

	void Clear() { 
		m_textures.clear(); 
		cached_textures.clear();
		m_texture_types.clear();
	}

private:
	ID3D11Device* m_device;
	ID3D11DeviceContext* m_deviceContext;
	int m_nextTextureID = 0;
	std::unordered_map<int, TextureData> cached_textures;

	// Keyed by the texture id THIS manager minted - see SetTextureType().
	std::unordered_map<int, uint32_t> m_texture_types;

	std::unordered_map<int, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_textures;

	// Keep the CPU-side copy of mip level 0 that GetTextureDataByHash() consumers rely on
	// (exporters, texture previews, any caller that measures a texture it has uploaded). Only
	// level 0 is cached - the extra mip levels exist on the GPU only.
	void CacheLevel0(int file_hash, int textureID, UINT width, UINT height, DXGI_FORMAT format,
	                 const void* data, UINT row_pitch)
	{
		if (file_hash < 0) { return; }

		TextureData textureData;
		textureData.textureID = textureID;
		textureData.width = width;
		textureData.height = height;

		if (data && BytesPerPixel(format) == 4 && row_pitch >= width * 4u)
		{
			textureData.rgba_data.resize(static_cast<size_t>(width) * height);

			const unsigned char* byteData = static_cast<const unsigned char*>(data);
			for (UINT y = 0; y < height; ++y)
			{
				std::memcpy(&textureData.rgba_data[static_cast<size_t>(y) * width],
				            byteData + static_cast<size_t>(y) * row_pitch,
				            static_cast<size_t>(width) * 4);
			}
		}

		cached_textures[file_hash] = std::move(textureData);
	}

	void GenerateMipmapLevel(const std::vector<uint8_t>& higherLevelData,
                         std::vector<uint8_t>& lowerLevelData,
                         UINT higherWidth, UINT higherHeight, UINT bytesPerPixel)
{
    if (bytesPerPixel != 4)
    {
        return; // Unsupported format
    }

    UINT lowerWidth = higherWidth / 2;
    UINT lowerHeight = higherHeight / 2;
    lowerLevelData.resize(lowerWidth * lowerHeight * bytesPerPixel);

    const uint8_t* src = higherLevelData.data();
    uint8_t* dst = lowerLevelData.data();

    for (UINT y = 0; y < lowerHeight; ++y)
    {
        for (UINT x = 0; x < lowerWidth; ++x)
        {
            UINT dstIdx = 4 * (y * lowerWidth + x);

            for (UINT channel = 0; channel < 4; ++channel)
            {
                std::vector<uint8_t> values;
                for (int dy = -1; dy <= 1; dy++)
                {
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        int nx = 2 * x + dx;
                        int ny = 2 * y + dy;

                        if (nx < 0 || ny < 0 || nx >= higherWidth || ny >= higherHeight)
                            continue;

                        UINT srcIdx = 4 * (ny * higherWidth + nx);
                        values.push_back(src[srcIdx + channel]);
                    }
                }

                // Sort and find the median
                std::sort(values.begin(), values.end());
                dst[dstIdx + channel] = values[values.size() / 2];
            }
        }
    }
}

};

inline bool SaveTextureToPng(ID3D11ShaderResourceView* texture, std::wstring& filename,
	TextureManager* texture_manager)
{
	HRESULT hr = texture_manager->SaveTextureToFile(texture, filename.c_str());
	if (FAILED(hr))
	{
		// Handle the error
		return false;
	}

	return true;
}

enum class CompressionFormat {
	None,
	BC1, // DXGI_FORMAT_BC1_UNORM
	BC3, // DXGI_FORMAT_BC3_UNORM
	BC5, // DXGI_FORMAT_BC5_UNORM
};


inline bool SaveTextureToDDS(const TextureData& textureData, const std::wstring& filename, CompressionFormat compressionFormat)
{
	size_t totalSize = textureData.rgba_data.size() * sizeof(RGBA);
	std::vector<uint8_t> pixelData(totalSize);

	// Copy the pixel data
	std::memcpy(pixelData.data(), textureData.rgba_data.data(), totalSize);

	// Create the Image structure
	DirectX::Image image;
	image.width = static_cast<size_t>(textureData.width);
	image.height = static_cast<size_t>(textureData.height);
	image.format = DXGI_FORMAT_B8G8R8A8_UNORM;
	image.rowPitch = textureData.width * sizeof(RGBA);
	image.slicePitch = image.rowPitch * textureData.height;
	image.pixels = pixelData.data();

	// Create a ScratchImage to hold the initial texture
	DirectX::ScratchImage scratchImage;
	HRESULT hr = scratchImage.InitializeFromImage(image);
	if (FAILED(hr)) {
		return false;
	}

	// Generate mipmaps
	DirectX::ScratchImage mipmappedImage;
	hr = DirectX::GenerateMipMaps(*scratchImage.GetImage(0, 0, 0), DirectX::TEX_FILTER_DEFAULT, 0, mipmappedImage);
	if (FAILED(hr)) {
		return false;
	}

	DirectX::ScratchImage finalImage;
	switch (compressionFormat) {
	case CompressionFormat::None:
		finalImage = std::move(mipmappedImage);
		break;
	case CompressionFormat::BC1:
		hr = DirectX::Compress(mipmappedImage.GetImages(), mipmappedImage.GetImageCount(), mipmappedImage.GetMetadata(), DXGI_FORMAT_BC1_UNORM, DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, finalImage);
		break;
	case CompressionFormat::BC3:
		hr = DirectX::Compress(mipmappedImage.GetImages(), mipmappedImage.GetImageCount(), mipmappedImage.GetMetadata(), DXGI_FORMAT_BC3_UNORM, DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, finalImage);
		break;
	case CompressionFormat::BC5:
		hr = DirectX::Compress(mipmappedImage.GetImages(), mipmappedImage.GetImageCount(), mipmappedImage.GetMetadata(), DXGI_FORMAT_BC5_UNORM, DirectX::TEX_COMPRESS_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, finalImage);
		break;

	default:
		return false;
	}

	if (FAILED(hr)) {
		return false;
	}

	// Save the final texture (compressed or uncompressed) to a DDS file
	hr = DirectX::SaveToDDSFile(finalImage.GetImages(), finalImage.GetImageCount(), finalImage.GetMetadata(), DirectX::DDS_FLAGS_NONE, filename.c_str());
	return SUCCEEDED(hr);
}
