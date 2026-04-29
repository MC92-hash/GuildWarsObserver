#include "pch.h"
#include "draw_character_builder.h"
#include "CharacterBuilder.h"
#include "GuiGlobalConstants.h"
#include "DATManager.h"
#include "MapRenderer.h"
#include "ModelViewer/ModelViewer.h"
#include "animation_state.h"
#include "draw_dat_browser.h"
#include "CheckerboardTexture.h"
#include "Parsers/BB9AnimationParser.h"
#include "imgui.h"

#include <string>
#include <numeric>
#include <algorithm>
#include <set>

// Defined in draw_dat_browser.cpp
extern std::unordered_map<int, TextureType> model_texture_types;

using namespace DirectX;

namespace
{
	CharacterData g_charData;
	std::string g_statusMsg;
	float g_statusTimer = 0.f;

	// Last loaded model info for debug display
	int g_lastMftIndex = -1;
	int g_lastFileType = -1;
	bool g_lastLoadSuccess = false;

	// Snapshot captured immediately after parse_file returns
	struct ParseFileDebug
	{
		int animMeshIds = -1;
		int animOrigMeshes = -1;
		int viewerMeshIds = -1;
		int viewerMeshes = -1;
		bool viewerActive = false;
		bool called = false;
	};
	ParseFileDebug g_pfDebug;

	// Body slots: 0=Feet, 1=Legs, 2=Chest, 3=Hands, 4=Head
	constexpr int BODY_SLOT_COUNT = 5;
	const char* g_bodySlotNames[BODY_SLOT_COUNT] = {"Feet", "Legs", "Chest", "Hands", "Head"};

	// Per-slot assigned body hash (the user picks these from the candidate browser)
	uint32_t g_bodySlotHash[BODY_SLOT_COUNT] = {};

	// Single candidate index for the flat candidate browser
	int g_bodyCandidateIdx = 0;

	// Per-slot body loading state (after assembly)
	constexpr int PROF_COUNT = 11;
	bool g_bodySlotLoaded[BODY_SLOT_COUNT] = {};
	std::string g_bodySlotStatus[BODY_SLOT_COUNT];

	uint32_t GetBodySlotHash(int bodySlot)
	{
		if (bodySlot >= 0 && bodySlot < BODY_SLOT_COUNT)
			return g_bodySlotHash[bodySlot];
		return 0;
	}

	// Assembly state
	struct LoadedPiece
	{
		std::vector<int> meshIds;
		std::vector<Mesh> meshes;
		std::vector<PerObjectCB> perObjectCBs;
		std::vector<std::vector<int>> perMeshTexIds;

		// Bone data per submesh (for skeleton pipeline)
		std::vector<AnimationPanelState::SubmeshBoneData> boneData;
		std::vector<std::vector<uint32_t>> vertexBoneGroups;

		// Raw file bytes + model hashes (used by first piece for skeleton creation)
		std::vector<uint8_t> rawBytes;
		uint32_t modelHash0 = 0;
		uint32_t modelHash1 = 0;
		uint32_t fileHash = 0;
		bool usingOtherFormat = false;
	};

	struct AssemblyState
	{
		bool assembled = false;
		std::string faceStatus;
		int totalMeshes = 0;
		int slotsLoaded = 0;
		int slotsFailed = 0;
		int slotsSkipped = 0;
		std::string perSlotStatus[CharacterData::SlotCount];
		bool bodySlotLoaded[BODY_SLOT_COUNT] = {};
		int bodySlotMeshCount[BODY_SLOT_COUNT] = {};
		std::string bodySlotStatus[BODY_SLOT_COUNT];
		int totalBodyMeshes = 0;
	};
	AssemblyState g_assembly;

	// Animation hash candidates collected during assembly
	struct AnimHashCandidate
	{
		uint32_t hash0 = 0;
		uint32_t hash1 = 0;
		uint32_t fileHash = 0;
		size_t fa1BoneCount = 0;
		std::string label;
	};
	std::vector<AnimHashCandidate> g_animHashCandidates;

	static constexpr size_t MIN_ANIM_BONES = 10;

	// Sequential animation search retry state
	int g_animRetryIdx = -1;            // -1 = not retrying, >=0 = current candidate index
	int g_searchResultIdx = 0;          // which search result to try next for current candidate
	bool g_animSearchActive = false;    // true while waiting for a search to complete
	bool g_animFound = false;           // true once an animation was loaded
	char g_manualAnimHashBuf[32] = {};  // manual animation file hash input

	ImVec4 DyeColorToImVec4(uint32_t dyeId)
	{
		switch (dyeId)
		{
		case 1:  return ImVec4(0.0f, 0.0f, 0.7f, 1.f);
		case 2:  return ImVec4(0.0f, 0.6f, 0.0f, 1.f);
		case 3:  return ImVec4(0.6f, 0.3f, 0.0f, 1.f);
		case 4:  return ImVec4(0.7f, 0.0f, 0.0f, 1.f);
		case 5:  return ImVec4(0.8f, 0.7f, 0.0f, 1.f);
		case 6:  return ImVec4(0.5f, 0.3f, 0.1f, 1.f);
		case 7:  return ImVec4(0.7f, 0.5f, 0.0f, 1.f);
		case 8:  return ImVec4(0.6f, 0.6f, 0.6f, 1.f);
		case 9:  return ImVec4(0.1f, 0.1f, 0.1f, 1.f);
		case 10: return ImVec4(0.5f, 0.5f, 0.5f, 1.f);
		case 11: return ImVec4(0.95f, 0.95f, 0.95f, 1.f);
		case 12: return ImVec4(0.9f, 0.5f, 0.7f, 1.f);
		default: return ImVec4(0.3f, 0.3f, 0.3f, 1.f);
		}
	}

	const char* DyeColorName(uint32_t dyeId)
	{
		static const char* names[] = {
			"None", "Blue", "Green", "Purple", "Red", "Yellow",
			"Brown", "Orange", "Silver", "Black", "Gray", "White", "Pink"
		};
		if (dyeId < std::size(names)) return names[dyeId];
		return "?";
	}

	void DrawDyeSwatches(const std::array<uint32_t, 4>& dyes)
	{
		bool any = false;
		for (int i = 0; i < 4; i++)
		{
			if (dyes[i] == 0) continue;
			if (any) ImGui::SameLine(0, 2);
			any = true;
			char label[16];
			snprintf(label, sizeof(label), "##dye%d", i);
			ImGui::ColorButton(label, DyeColorToImVec4(dyes[i]),
				ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(14, 14));
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Dye %d: %s", i + 1, DyeColorName(dyes[i]));
		}
		if (!any) ImGui::TextDisabled("None");
	}

	void SetStatus(const char* msg)
	{
		g_statusMsg = msg;
		g_statusTimer = 4.f;
	}

	// Resolve a file hash to an MFT index, returning -1 on failure
	int ResolveHash(uint32_t file_hash,
		DATManager* dat_manager,
		std::unordered_map<int, std::vector<int>>& hash_index)
	{
		auto it = hash_index.find(static_cast<int>(file_hash));
		if (it == hash_index.end() || it->second.empty())
			return -1;
		return it->second.at(0);
	}

	// Push a unique (hash0, hash1) pair into g_animHashCandidates
	void AddAnimHashCandidate(uint32_t hash0, uint32_t hash1, uint32_t fileHash,
		size_t fa1BoneCount, const char* label)
	{
		if (hash0 == 0 && hash1 == 0) return;
		for (auto& c : g_animHashCandidates)
		{
			if (c.hash0 == hash0 && c.hash1 == hash1)
			{
				if (fa1BoneCount > c.fa1BoneCount)
					c.fa1BoneCount = fa1BoneCount;
				return;
			}
		}
		g_animHashCandidates.push_back({ hash0, hash1, fileHash, fa1BoneCount, label });
	}

	// Start animation search retry from candidate index 0
	void BeginAnimHashRetry()
	{
		g_animFound = false;
		g_animRetryIdx = 0;
		g_searchResultIdx = 0;
		g_animSearchActive = false;
		if (g_animHashCandidates.empty())
		{
			g_animRetryIdx = -1;
			return;
		}
		// Sort candidates: most FA1 bones first so the chest (105 bones) is
		// tried before the weapon (1 bone).
		std::sort(g_animHashCandidates.begin(), g_animHashCandidates.end(),
			[](const AnimHashCandidate& a, const AnimHashCandidate& b)
			{ return a.fa1BoneCount > b.fa1BoneCount; });

		// Use ManualAllResults to get ALL matching files, not just the first.
		auto& c = g_animHashCandidates[0];
		g_animationState.SetModelHashes(c.hash0, c.hash1, c.fileHash);
		StartAnimationSearchFromStoredManagers();
		g_animSearchActive = true;
	}

	// Called every frame to pump search results and advance retry logic
	void TickAnimHashRetry()
	{
		if (g_animFound) return;
		if (!g_animSearchActive && g_animRetryIdx < 0) return;

		// Pump results from the background search
		PumpAnimationSearchResultsFromStoredManagers();

		// Check if animation was loaded by the pump (AutoFirstMatch auto-loads the first result)
		if (g_animationState.hasAnimation)
		{
			size_t loadedBones = g_animationState.clip
				? g_animationState.clip->boneTracks.size() : 0;
			if (loadedBones >= MIN_ANIM_BONES)
			{
				g_animFound = true;
				g_animSearchActive = false;
				return;
			}
			// Trivial animation -- reject it but DON'T cancel the search yet;
			// we may have more results to try.
			g_animationState.hasAnimation = false;
			g_animationState.clip.reset();
			g_animationState.skeleton.reset();
			g_animationState.controller.reset();
			g_animationState.hasSkinnedMeshes = false;
			// Start iterating from result index 1 (0 was auto-loaded and rejected)
			g_searchResultIdx = 1;
		}

		// Iterate through remaining search results looking for one with enough bones
		while (!g_animationState.searchResults.empty() &&
			g_searchResultIdx < static_cast<int>(g_animationState.searchResults.size()) &&
			!g_animationState.hasAnimation)
		{
			LoadAnimationFromSearchResultFromStoredManagers(g_searchResultIdx);
			g_searchResultIdx++;
			if (g_animationState.hasAnimation)
			{
				size_t loadedBones = g_animationState.clip
					? g_animationState.clip->boneTracks.size() : 0;
				if (loadedBones >= MIN_ANIM_BONES)
				{
					g_animFound = true;
					g_animSearchActive = false;
					return;
				}
				g_animationState.hasAnimation = false;
				g_animationState.clip.reset();
				g_animationState.skeleton.reset();
				g_animationState.controller.reset();
				g_animationState.hasSkinnedMeshes = false;
			}
		}

		// Still searching in background?
		if (g_animationState.searchInProgress.load())
			return;

		// Background search completed -- all results for this candidate exhausted.
		// Clear results and try the next candidate hash pair.
		g_animationState.searchResults.clear();

		// No candidate retry active -- just a standalone search that finished
		if (g_animRetryIdx < 0)
		{
			g_animSearchActive = false;
			return;
		}

		// Search finished with no animation loaded -- try next candidate
		g_animRetryIdx++;
		if (g_animRetryIdx >= static_cast<int>(g_animHashCandidates.size()))
		{
			g_animRetryIdx = -1;
			g_animSearchActive = false;
			return;
		}

		auto& c = g_animHashCandidates[g_animRetryIdx];
		g_animationState.SetModelHashes(c.hash0, c.hash1, c.fileHash);
		g_searchResultIdx = 0;
		StartAnimationSearchFromStoredManagers();
		g_animSearchActive = true;
	}

	// Load one FFNA_Type2 model into the scene WITHOUT clearing existing meshes.
	// Extracts mesh + texture + bone data from the model file additively.
	LoadedPiece LoadSingleModelIntoScene(
		uint32_t file_hash,
		DATManager* dat_manager,
		MapRenderer* map_renderer,
		std::unordered_map<int, std::vector<int>>& hash_index,
		const XMMATRIX& shared_world_matrix)
	{
		LoadedPiece result;

		int mftIndex = ResolveHash(file_hash, dat_manager, hash_index);
		if (mftIndex < 0)
			return result;

		const auto& mft = dat_manager->get_MFT();
		result.fileHash = static_cast<uint32_t>(mft[mftIndex].Hash);
		if (mftIndex >= static_cast<int>(mft.size()))
			return result;
		if (mft[mftIndex].type != FFNA_Type2)
			return result;

		// Read raw file bytes for FA1 bind pose / animation extraction
		unsigned char* raw_data = dat_manager->read_file(mftIndex);
		if (raw_data)
		{
			result.rawBytes.assign(raw_data, raw_data + mft[mftIndex].uncompressedSize);
			delete raw_data;
		}

		bool using_other = dat_manager->is_other_model_format(mftIndex);
		result.usingOtherFormat = using_other;
		FFNA_ModelFile model_file{};
		FFNA_ModelFile_Other model_file_other{};

		if (using_other)
			model_file_other = dat_manager->parse_ffna_model_file_other(mftIndex);
		else
			model_file = dat_manager->parse_ffna_model_file(mftIndex);

		bool parsed_ok = using_other ? model_file_other.parsed_correctly : model_file.parsed_correctly;
		if (!parsed_ok)
			return result;

		// Extract model hashes from geometry (needed for animation lookup)
		if (using_other)
		{
			result.modelHash0 = model_file_other.geometry_chunk.header.model_hash0;
			result.modelHash1 = model_file_other.geometry_chunk.header.model_hash1;
		}
		else
		{
			result.modelHash0 = model_file.geometry_chunk.sub_1.f0xC;
			result.modelHash1 = model_file.geometry_chunk.sub_1.f0x10;
		}

		const auto& models = using_other
			? model_file_other.geometry_chunk.models
			: model_file.geometry_chunk.models;

		if (models.empty())
			return result;

		// Extract bone data per submesh (mirrors parse_file lines 380-398)
		for (size_t i = 0; i < models.size(); i++)
		{
			const auto& model = models[i];
			auto bd = AnimationPanelState::ExtractBoneData(model.extra_data, model.u0, model.u1);
			result.boneData.push_back(bd);

			std::vector<uint32_t> vbg;
			vbg.reserve(model.vertices.size());
			for (const auto& mv : model.vertices)
				vbg.push_back(mv.has_group ? mv.group : 0);
			result.vertexBoneGroups.push_back(std::move(vbg));
		}

		// Build meshes
		const auto& geometry_chunk_uts0 = using_other
			? model_file_other.geometry_chunk.tex_and_vertex_shader_struct.uts0
			: model_file.geometry_chunk.tex_and_vertex_shader_struct.uts0;
		const auto& geometry_chunk_uts1 = using_other
			? model_file_other.geometry_chunk.uts1
			: model_file.geometry_chunk.uts1;

		std::vector<Mesh> piece_meshes;
		std::vector<int> sort_orders;

		for (int i = 0; i < static_cast<int>(models.size()); i++)
		{
			AMAT_file amat_file;
			if (!using_other && model_file.AMAT_filenames_chunk.texture_filenames.size() > 0)
			{
				int sub_model_index = models[i].unknown;
				if (geometry_chunk_uts0.size() > 0)
					sub_model_index %= static_cast<int>(geometry_chunk_uts0.size());
				const auto uts1 = geometry_chunk_uts1[sub_model_index % geometry_chunk_uts1.size()];
				const int amat_file_index = ((uts1.some_flags0 >> 8) & 0xFF)
					% static_cast<int>(model_file.AMAT_filenames_chunk.texture_filenames.size());
				const auto amat_filename = model_file.AMAT_filenames_chunk.texture_filenames[amat_file_index];
				const auto decoded_filename = decode_filename(amat_filename.id0, amat_filename.id1);
				auto mft_entry_it = hash_index.find(decoded_filename);
				if (mft_entry_it != hash_index.end())
				{
					auto file_index = mft_entry_it->second.at(0);
					amat_file = dat_manager->parse_amat_file(file_index);
				}
			}

			Mesh prop_mesh = using_other
				? model_file_other.GetMesh(i, amat_file)
				: model_file.GetMesh(i, amat_file);
			prop_mesh.center = {
				(models[i].maxX - models[i].minX) / 2.0f,
				(models[i].maxY - models[i].minY) / 2.0f,
				(models[i].maxZ - models[i].minZ) / 2.0f
			};

			uint32_t sort_order = amat_file.GRMT_chunk.sort_order;
			if ((prop_mesh.indices.size() % 3) == 0)
			{
				piece_meshes.push_back(prop_mesh);
				sort_orders.push_back(sort_order);
			}
		}

		if (piece_meshes.empty())
			return result;

		// Sort by sort_order
		std::vector<size_t> indices(sort_orders.size());
		std::iota(indices.begin(), indices.end(), 0);
		std::sort(indices.begin(), indices.end(),
			[&sort_orders](size_t i1, size_t i2) { return sort_orders[i1] < sort_orders[i2]; });
		{
			std::vector<Mesh> sorted(piece_meshes.size());
			for (size_t i = 0; i < indices.size(); ++i)
				sorted[i] = piece_meshes[indices[i]];
			piece_meshes.swap(sorted);

			std::vector<AnimationPanelState::SubmeshBoneData> sortedBD(result.boneData.size());
			std::vector<std::vector<uint32_t>> sortedVBG(result.vertexBoneGroups.size());
			for (size_t i = 0; i < indices.size(); ++i)
			{
				if (indices[i] < result.boneData.size())
					sortedBD[i] = result.boneData[indices[i]];
				if (indices[i] < result.vertexBoneGroups.size())
					sortedVBG[i] = result.vertexBoneGroups[indices[i]];
			}
			result.boneData.swap(sortedBD);
			result.vertexBoneGroups.swap(sortedVBG);
		}

		// Load textures
		std::vector<int> texture_ids;
		std::vector<DatTexture> model_dat_textures;
		std::vector<std::vector<int>> per_mesh_tex_ids(piece_meshes.size());

		bool textures_available = using_other
			? (model_file_other.has_inline_textures || model_file_other.textures_parsed_correctly)
			: model_file.textures_parsed_correctly;

		const auto& MFT = dat_manager->get_MFT();
		const auto* entry = &MFT[mftIndex];

		if (textures_available)
		{
			auto load_texture_by_hash = [&](size_t j, int decoded_filename) {
				int texture_id = map_renderer->GetTextureManager()->GetTextureIdByHash(decoded_filename);
				auto mft_entry_it = hash_index.find(decoded_filename);
				if (mft_entry_it != hash_index.end())
				{
					auto file_index = mft_entry_it->second.at(0);
					const auto* tex_entry = &MFT[file_index];
					if (!tex_entry) return;

					DatTexture dat_texture;
					if (tex_entry->type == DDS)
					{
						const auto ddsData = dat_manager->parse_dds_file(file_index);
						map_renderer->GetTextureManager()->CreateTextureFromDDSInMemory(
							ddsData.data(), ddsData.size(), &texture_id,
							&dat_texture.width, &dat_texture.height,
							dat_texture.rgba_data, tex_entry->Hash);
						model_texture_types.insert({ texture_id, DDSt });
						dat_texture.texture_type = DDSt;
					}
					else
					{
						dat_texture = dat_manager->parse_ffna_texture_file(file_index);
						if (texture_id < 0)
						{
							map_renderer->GetTextureManager()->CreateTextureFromRGBA(
								dat_texture.width, dat_texture.height,
								dat_texture.rgba_data.data(), &texture_id, decoded_filename);
						}
						model_texture_types.insert({ texture_id, dat_texture.texture_type });
					}
					model_dat_textures.push_back(dat_texture);
					if (texture_id >= 0)
						texture_ids.push_back(texture_id);
				}
			};

			if (using_other)
			{
				const auto& texture_filenames = model_file_other.texture_filenames_chunk.texture_filenames;
				texture_ids.resize(texture_filenames.size(), -1);
				model_dat_textures.resize(texture_filenames.size());
				for (size_t j = 0; j < texture_filenames.size(); j++)
				{
					auto decoded_fn = decode_filename(texture_filenames[j].id0, texture_filenames[j].id1);
					auto mft_entry_it = hash_index.find(decoded_fn);
					if (mft_entry_it != hash_index.end())
					{
						auto file_index = mft_entry_it->second.at(0);
						const auto* tex_entry = &MFT[file_index];
						if (tex_entry)
						{
							DatTexture dat_texture;
							int texture_id = -1;
							if (tex_entry->type == DDS)
							{
								const auto ddsData = dat_manager->parse_dds_file(file_index);
								map_renderer->GetTextureManager()->CreateTextureFromDDSInMemory(
									ddsData.data(), ddsData.size(), &texture_id,
									&dat_texture.width, &dat_texture.height,
									dat_texture.rgba_data, tex_entry->Hash);
								dat_texture.texture_type = DDSt;
							}
							else
							{
								dat_texture = dat_manager->parse_ffna_texture_file(file_index);
								if (dat_texture.width > 0 && dat_texture.height > 0 && !dat_texture.rgba_data.empty())
								{
									map_renderer->GetTextureManager()->CreateTextureFromRGBA(
										dat_texture.width, dat_texture.height,
										dat_texture.rgba_data.data(), &texture_id, decoded_fn);
								}
							}
							texture_ids[j] = texture_id;
							model_dat_textures[j] = dat_texture;
							if (texture_id >= 0)
								model_texture_types.insert({ texture_id, dat_texture.texture_type });
						}
					}
				}
			}
			else
			{
				const auto& texture_filenames = model_file.texture_filenames_chunk.texture_filenames;
				for (size_t j = 0; j < texture_filenames.size(); j++)
				{
					auto decoded_fn = decode_filename(texture_filenames[j].id0, texture_filenames[j].id1);
					load_texture_by_hash(j, decoded_fn);
				}
			}

			// Placeholder texture if no textures loaded
			if (texture_ids.empty() || std::all_of(texture_ids.begin(), texture_ids.end(), [](int id) { return id < 0; }))
			{
				int tile = 8;
				CheckerboardTexture checkerboard(tile * 2, tile * 2, tile,
					CheckerboardTexture::ColorChoice::Silver, CheckerboardTexture::ColorChoice::Silver);
				int placeholderId = map_renderer->GetTextureManager()->AddTexture(
					(void*)checkerboard.getData().data(), tile * 2, tile * 2,
					DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<int>(file_hash) + 999000);
				model_texture_types.insert({ placeholderId, BC1 });
				texture_ids.clear();
				texture_ids.push_back(placeholderId);
				for (auto& m : piece_meshes)
					for (auto& bf : m.blend_flags)
						bf = 0;
			}

			// Remap tex_indices per mesh so they're local indices into per_mesh_tex_ids
			for (int i = 0; i < static_cast<int>(piece_meshes.size()); i++)
			{
				std::vector<uint8_t> new_tex_indices;
				std::vector<uint8_t> new_uv_indices;
				std::vector<uint8_t> new_blend_flags;

				for (int j = 0; j < static_cast<int>(piece_meshes[i].tex_indices.size()); j++)
				{
					int tex_index = piece_meshes[i].tex_indices[j];
					if (tex_index >= 0 && tex_index < static_cast<int>(texture_ids.size()) && texture_ids[tex_index] >= 0)
					{
						per_mesh_tex_ids[i].push_back(texture_ids[tex_index]);
						new_tex_indices.push_back(static_cast<uint8_t>(per_mesh_tex_ids[i].size() - 1));
						if (j < static_cast<int>(piece_meshes[i].uv_coord_indices.size()))
							new_uv_indices.push_back(piece_meshes[i].uv_coord_indices[j]);
						if (j < static_cast<int>(piece_meshes[i].blend_flags.size()))
							new_blend_flags.push_back(piece_meshes[i].blend_flags[j]);
					}
				}
				piece_meshes[i].tex_indices = new_tex_indices;
				piece_meshes[i].uv_coord_indices = new_uv_indices;
				piece_meshes[i].blend_flags = new_blend_flags;
			}
		}

		// Build PerObjectCBs using the shared world matrix
		std::vector<PerObjectCB> per_object_cbs(piece_meshes.size());
		for (int i = 0; i < static_cast<int>(per_object_cbs.size()); i++)
		{
			XMStoreFloat4x4(&per_object_cbs[i].world, shared_world_matrix);

			auto& pm = piece_meshes[i];
			if (pm.uv_coord_indices.size() != pm.tex_indices.size() ||
				pm.uv_coord_indices.size() >= MAX_NUM_TEX_INDICES)
			{
				continue;
			}

			if (textures_available)
			{
				per_object_cbs[i].num_uv_texture_pairs = static_cast<uint32_t>(pm.uv_coord_indices.size());
				for (int j = 0; j < static_cast<int>(pm.uv_coord_indices.size()); j++)
				{
					int index0 = j / 4;
					int index1 = j % 4;
					per_object_cbs[i].uv_indices[index0][index1] = static_cast<uint32_t>(pm.uv_coord_indices[j]);
					per_object_cbs[i].texture_indices[index0][index1] = static_cast<uint32_t>(pm.tex_indices[j]);
					per_object_cbs[i].blend_flags[index0][index1] = static_cast<uint32_t>(pm.blend_flags[j]);
					if (j < static_cast<int>(per_mesh_tex_ids[i].size()))
					{
						auto tex_type_it = model_texture_types.find(per_mesh_tex_ids[i][j]);
						if (tex_type_it != model_texture_types.end())
							per_object_cbs[i].texture_types[index0][index1] = static_cast<uint32_t>(tex_type_it->second);
					}
				}
			}
		}

		// Determine pixel shader
		auto pixel_shader_type = PixelShaderType::OldModel;
		const auto& unknown_tex_stuff1 = using_other
			? model_file_other.geometry_chunk.unknown_tex_stuff1
			: model_file.geometry_chunk.unknown_tex_stuff1;
		if (!unknown_tex_stuff1.empty())
			pixel_shader_type = PixelShaderType::NewModel;

		// Add to GPU (additive - does NOT clear existing meshes)
		auto mesh_ids = map_renderer->AddProp(piece_meshes, per_object_cbs,
			static_cast<uint32_t>(file_hash), pixel_shader_type);

		// Bind textures
		if (textures_available)
		{
			for (int i = 0; i < static_cast<int>(mesh_ids.size()); i++)
			{
				auto& tex_ids = per_mesh_tex_ids[i];
				if (!tex_ids.empty())
				{
					map_renderer->GetMeshManager()->SetTexturesForMesh(
						mesh_ids[i],
						map_renderer->GetTextureManager()->GetTextures(tex_ids), 3);
				}
			}
		}

		result.meshIds = mesh_ids;
		result.meshes = piece_meshes;
		result.perObjectCBs = per_object_cbs;
		result.perMeshTexIds = per_mesh_tex_ids;
		return result;
	}

	// Accumulate bounding box from one file hash
	bool AccumulateBoundsForHash(
		uint32_t file_hash,
		DATManager* dat_manager,
		std::unordered_map<int, std::vector<int>>& hash_index,
		float& minX, float& minY, float& minZ,
		float& maxX, float& maxY, float& maxZ)
	{
		int mftIndex = ResolveHash(file_hash, dat_manager, hash_index);
		if (mftIndex < 0) return false;
		const auto& mft = dat_manager->get_MFT();
		if (mftIndex >= static_cast<int>(mft.size()) || mft[mftIndex].type != FFNA_Type2)
			return false;

		bool using_other = dat_manager->is_other_model_format(mftIndex);
		FFNA_ModelFile mf{};
		FFNA_ModelFile_Other mfo{};
		if (using_other) mfo = dat_manager->parse_ffna_model_file_other(mftIndex);
		else             mf = dat_manager->parse_ffna_model_file(mftIndex);

		bool ok = using_other ? mfo.parsed_correctly : mf.parsed_correctly;
		if (!ok) return false;

		const auto& models = using_other ? mfo.geometry_chunk.models : mf.geometry_chunk.models;
		bool any = false;
		for (const auto& m : models)
		{
			minX = std::min(minX, m.minX); minY = std::min(minY, m.minY); minZ = std::min(minZ, m.minZ);
			maxX = std::max(maxX, m.maxX); maxY = std::max(maxY, m.maxY); maxZ = std::max(maxZ, m.maxZ);
			any = true;
		}
		return any;
	}

	// Compute a shared bounding box across body slots + all equipment slots
	bool ComputeSharedBounds(
		const uint32_t bodyHashes[BODY_SLOT_COUNT],
		DATManager* dat_manager,
		std::unordered_map<int, std::vector<int>>& hash_index,
		const std::array<CharacterSlotData, CharacterData::SlotCount>& slots,
		float& outMinX, float& outMinY, float& outMinZ,
		float& outMaxX, float& outMaxY, float& outMaxZ)
	{
		outMinX = FLT_MAX; outMinY = FLT_MAX; outMinZ = FLT_MAX;
		outMaxX = -FLT_MAX; outMaxY = -FLT_MAX; outMaxZ = -FLT_MAX;
		bool any = false;

		for (int bs = 0; bs < BODY_SLOT_COUNT; bs++)
		{
			if (bodyHashes[bs] != 0)
				any |= AccumulateBoundsForHash(bodyHashes[bs], dat_manager, hash_index, outMinX, outMinY, outMinZ, outMaxX, outMaxY, outMaxZ);
		}

		for (int i = 0; i < CharacterData::SlotCount; i++)
		{
			if (slots[i].dat_file_id == 0) continue;
			any |= AccumulateBoundsForHash(slots[i].dat_file_id, dat_manager, hash_index, outMinX, outMinY, outMinZ, outMaxX, outMaxY, outMaxZ);
		}
		return any;
	}

	// Build a shared world matrix from a bounding box (same formula as parse_file)
	XMMATRIX BuildSharedWorldMatrix(float minX, float minY, float minZ, float maxX, float maxY, float maxZ)
	{
		float modelWidth = maxX - minX;
		float modelHeight = maxY - minY;
		float modelDepth = maxZ - minZ;
		float maxDim = std::max({ modelWidth, modelHeight, modelDepth });
		float scale = (maxDim > 0.001f) ? (10000.0f / maxDim) : 1.0f;
		float centerX = minX + modelWidth * 0.5f;
		float centerY = minY + modelHeight * 0.5f;
		float centerZ = minZ + modelDepth * 0.5f;
		XMMATRIX scaling = XMMatrixScaling(scale, scale, scale);
		XMMATRIX translation = XMMatrixTranslation(
			-centerX * scale,
			(-centerY + modelHeight / 2.0f) * scale,
			-centerZ * scale);
		return scaling * translation;
	}

	// Load a model by file hash using parse_file (for single-slot preview)
	bool LoadModelByHash(uint32_t file_hash,
		DATManager* dat_manager,
		MapRenderer* map_renderer,
		std::unordered_map<int, std::vector<int>>& hash_index)
	{
		int mftIndex = ResolveHash(file_hash, dat_manager, hash_index);
		g_lastMftIndex = mftIndex;

		if (mftIndex < 0)
		{
			g_lastFileType = -1;
			g_lastLoadSuccess = false;
			SetStatus("File hash not found in DAT");
			return false;
		}

		const auto& mft = dat_manager->get_MFT();
		g_lastFileType = (mftIndex < static_cast<int>(mft.size())) ? mft[mftIndex].type : -1;

		if (g_lastFileType != FFNA_Type2)
		{
			g_lastLoadSuccess = false;
			g_pfDebug = {};
			g_pfDebug.called = true;
			char msg[128];
			snprintf(msg, sizeof(msg), "Skipped: file type %d is not a 3D model (need FFNA_Type2=%d)",
				g_lastFileType, static_cast<int>(FFNA_Type2));
			SetStatus(msg);
			return false;
		}

		g_pfDebug = {};
		g_pfDebug.called = true;
		bool ok = parse_file(dat_manager, mftIndex, map_renderer, hash_index);
		g_pfDebug.animMeshIds = static_cast<int>(g_animationState.meshIds.size());
		g_pfDebug.animOrigMeshes = static_cast<int>(g_animationState.originalMeshes.size());
		g_pfDebug.viewerMeshIds = static_cast<int>(g_modelViewerState.meshIds.size());
		g_pfDebug.viewerMeshes = static_cast<int>(g_modelViewerState.meshes.size());
		g_pfDebug.viewerActive = g_modelViewerState.isActive;
		g_lastLoadSuccess = ok;

		if (!ok)
		{
			char msg[128];
			snprintf(msg, sizeof(msg), "parse_file failed (MFT %d, type %d)", mftIndex, g_lastFileType);
			SetStatus(msg);
		}
		return ok;
	}

	// Load a single slot via parse_file (standalone preview of one piece)
	void LoadSingleSlot(int slotIdx,
		DATManager* dat_manager,
		MapRenderer* map_renderer,
		std::unordered_map<int, std::vector<int>>& hash_index)
	{
		uint32_t fh = g_charData.slots[slotIdx].dat_file_id;
		if (fh == 0) { SetStatus("No file ID for this slot"); return; }

		if (LoadModelByHash(fh, dat_manager, map_renderer, hash_index))
		{
			char msg[64];
			snprintf(msg, sizeof(msg), "Loaded %s (%u)", CharacterSlotName(slotIdx), fh);
			SetStatus(msg);
		}
	}

	uint32_t ResolveBodySlotHash(int bodySlot)
	{
		return GetBodySlotHash(bodySlot);
	}

	// Helper: accumulate a loaded piece into the combined vectors and bone data
	void AccumulatePiece(const LoadedPiece& piece,
		std::vector<int>& allMeshIds,
		std::vector<Mesh>& allMeshes,
		std::vector<PerObjectCB>& allPerObjectCBs,
		std::vector<std::vector<int>>& allPerMeshTexIds)
	{
		allMeshIds.insert(allMeshIds.end(), piece.meshIds.begin(), piece.meshIds.end());
		allMeshes.insert(allMeshes.end(), piece.meshes.begin(), piece.meshes.end());
		allPerObjectCBs.insert(allPerObjectCBs.end(), piece.perObjectCBs.begin(), piece.perObjectCBs.end());
		allPerMeshTexIds.insert(allPerMeshTexIds.end(), piece.perMeshTexIds.begin(), piece.perMeshTexIds.end());

		// Accumulate bone data into g_animationState
		for (const auto& bd : piece.boneData)
			g_animationState.submeshBoneData.push_back(bd);
		for (const auto& vbg : piece.vertexBoneGroups)
			g_animationState.perVertexBoneGroups.push_back(vbg);
	}

	// Count FA1 bones in a piece without modifying global state.
	size_t CountFA1Bones(const LoadedPiece& piece)
	{
		if (piece.rawBytes.empty())
			return 0;

		const uint8_t* data = piece.rawBytes.data();
		size_t dataSize = piece.rawBytes.size();
		std::vector<int32_t> tmpParents;
		std::vector<DirectX::XMFLOAT3> tmpPositions;

		for (size_t offset = 5; offset + 8 < dataSize; )
		{
			uint32_t chunkId = *reinterpret_cast<const uint32_t*>(&data[offset]);
			uint32_t chunkSize = *reinterpret_cast<const uint32_t*>(&data[offset + 4]);

			if (chunkId == GW::Parsers::CHUNK_ID_FA0 ||
				chunkId == GW::Parsers::CHUNK_ID_FA1)
			{
				const uint8_t* fa1Data = &data[offset + 8];
				size_t fa1Size = std::min(static_cast<size_t>(chunkSize), dataSize - offset - 8);
				return GW::Parsers::BB9AnimationParser::ParseFA1BindPose(
					fa1Data, fa1Size, tmpParents, tmpPositions);
			}

			offset += 8 + chunkSize;
			if (chunkSize == 0) break;
		}
		return 0;
	}

	// Extract FA1 bind pose from a piece's raw bytes and set model hashes.
	void EstablishBindPoseFromPiece(const LoadedPiece& piece)
	{
		if (piece.rawBytes.empty()) return;

		g_animationState.SetModelHashes(piece.modelHash0, piece.modelHash1, piece.fileHash);

		g_animationState.fa1BindPoseParents.clear();
		g_animationState.fa1BindPosePositions.clear();
		g_animationState.hasFA1BindPose = false;

		const uint8_t* data = piece.rawBytes.data();
		size_t dataSize = piece.rawBytes.size();

		for (size_t offset = 5; offset + 8 < dataSize; )
		{
			uint32_t chunkId = *reinterpret_cast<const uint32_t*>(&data[offset]);
			uint32_t chunkSize = *reinterpret_cast<const uint32_t*>(&data[offset + 4]);

			if (chunkId == GW::Parsers::CHUNK_ID_FA0 ||
				chunkId == GW::Parsers::CHUNK_ID_FA1)
			{
				const uint8_t* fa1Data = &data[offset + 8];
				size_t fa1Size = std::min(static_cast<size_t>(chunkSize), dataSize - offset - 8);

				size_t boneCount = GW::Parsers::BB9AnimationParser::ParseFA1BindPose(
					fa1Data, fa1Size,
					g_animationState.fa1BindPoseParents,
					g_animationState.fa1BindPosePositions);

				if (boneCount > 0)
					g_animationState.hasFA1BindPose = true;
				break;
			}

			offset += 8 + chunkSize;
			if (chunkSize == 0) break;
		}
	}

	// Count animation bones in a piece by parsing with ParseAnimationFromFile
	// (scans BB9/BB8/FA1/FA0 chunks). Returns 0 if no valid animation found.
	size_t CountAnimBones(const LoadedPiece& piece)
	{
		if (piece.rawBytes.empty()) return 0;
		auto clipOpt = GW::Parsers::ParseAnimationFromFile(
			piece.rawBytes.data(), piece.rawBytes.size());
		if (!clipOpt || !clipOpt->IsValid()) return 0;
		return clipOpt->boneTracks.size();
	}

	// Extract embedded animation from a piece's raw bytes and initialize
	// the animation state. Returns true if animation was successfully loaded.
	bool InitializeAnimationFromPiece(const LoadedPiece& piece)
	{
		if (piece.rawBytes.empty()) return false;

		auto clipOpt = GW::Parsers::ParseAnimationFromFile(
			piece.rawBytes.data(), piece.rawBytes.size());
		if (!clipOpt || !clipOpt->IsValid()) return false;

		auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*clipOpt));
		auto skeleton = std::make_shared<GW::Animation::Skeleton>(
			GW::Parsers::BB9AnimationParser::CreateSkeleton(*clip));
		g_animationState.Initialize(clip, skeleton, piece.fileHash);
		return g_animationState.hasAnimation;
	}

	// Assemble all equipment pieces + body slot meshes into the same 3D scene
	// with skeleton/bone data for proper posing and skinning.
	void LoadAllAndAssemble(
		DATManager* dat_manager,
		MapRenderer* map_renderer,
		std::unordered_map<int, std::vector<int>>& hash_index)
	{
		// 1. Clear scene once
		if (g_modelViewerState.isActive)
			DeactivateModelViewer(map_renderer);
		map_renderer->GetTextureManager()->Clear();
		map_renderer->ClearProps();
		map_renderer->ClearSceneForModeSwitch();
		CancelAnimationSearch();
		g_animationState.Reset();
		map_renderer->SetShouldRenderShadowsForModels(false);
		map_renderer->ClearShadowMapBinding();

		g_assembly = {};
		g_animHashCandidates.clear();
		g_animRetryIdx = -1;
		g_animSearchActive = false;
		g_animFound = false;

		// Prepare bone data containers on animation state
		g_animationState.submeshBoneData.clear();
		g_animationState.perVertexBoneGroups.clear();
		g_animationState.originalMeshes.clear();
		g_animationState.animatedMeshes.clear();
		g_animationState.hasSkinnedMeshes = false;

		// Resolve per-slot body hashes
		uint32_t bodyHashes[BODY_SLOT_COUNT] = {};
		for (int bs = 0; bs < BODY_SLOT_COUNT; bs++)
			bodyHashes[bs] = ResolveBodySlotHash(bs);

		// 2. Compute shared bounding box (body slots + equipment)
		float bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ;
		if (!ComputeSharedBounds(bodyHashes, dat_manager, hash_index, g_charData.slots,
			bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ))
		{
			SetStatus("No valid FFNA_Type2 models found in any slot");
			return;
		}
		XMMATRIX sharedWorld = BuildSharedWorldMatrix(bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ);

		// 3. Accumulate all mesh + bone data
		std::vector<int> allMeshIds;
		std::vector<Mesh> allMeshes;
		std::vector<PerObjectCB> allPerObjectCBs;
		std::vector<std::vector<int>> allPerMeshTexIds;

		// Track the piece with the most FA1 bones for bind pose establishment.
		// Weapons (slot 0, processed first) often have only 2 bones; armor pieces
		// like Chest typically have the full 88-bone skeleton.
		size_t bestFA1BoneCount = 0;
		int bestBindPoseIdx = -1;

		// Temporarily store loaded pieces so we can pick the best bind pose after
		// all are loaded.
		std::vector<LoadedPiece> loadedPieces;
		loadedPieces.reserve(BODY_SLOT_COUNT + CharacterData::SlotCount);

		// 3a. Load body slot meshes first (so armor layers on top).
		// The head body slot (index 4) contains the base head mesh with the character's
		// hairstyle baked in. We always load it so hair is visible even when head armor
		// (circlets, half-masks, tattoos, headbands) is equipped on top. Full helms that
		// should hide hair are a minority case handled separately in the future.
		for (int bs = 0; bs < BODY_SLOT_COUNT; bs++)
		{
			g_bodySlotLoaded[bs] = false;
			g_bodySlotStatus[bs].clear();

			if (bodyHashes[bs] == 0)
			{
				g_assembly.bodySlotStatus[bs] = "No hash";
				continue;
			}

			auto bodyPiece = LoadSingleModelIntoScene(
				bodyHashes[bs], dat_manager, map_renderer, hash_index, sharedWorld);
			if (!bodyPiece.meshIds.empty())
			{
				size_t fa1Bones = CountFA1Bones(bodyPiece);
				if (fa1Bones > bestFA1BoneCount)
					bestFA1BoneCount = fa1Bones;

				char lbl[64];
				snprintf(lbl, sizeof(lbl), "Body %s (%u)", g_bodySlotNames[bs], bodyHashes[bs]);
				AddAnimHashCandidate(bodyPiece.modelHash0, bodyPiece.modelHash1, bodyPiece.fileHash, fa1Bones, lbl);

				g_bodySlotLoaded[bs] = true;
				AccumulatePiece(bodyPiece, allMeshIds, allMeshes, allPerObjectCBs, allPerMeshTexIds);
				char buf[64];
				snprintf(buf, sizeof(buf), "OK (%zu meshes)", bodyPiece.meshIds.size());
				g_assembly.bodySlotStatus[bs] = buf;
				g_assembly.bodySlotLoaded[bs] = true;
				g_assembly.bodySlotMeshCount[bs] = static_cast<int>(bodyPiece.meshIds.size());
				g_assembly.totalBodyMeshes += static_cast<int>(bodyPiece.meshIds.size());

				loadedPieces.push_back(std::move(bodyPiece));
				if (fa1Bones == bestFA1BoneCount && fa1Bones > 0)
					bestBindPoseIdx = static_cast<int>(loadedPieces.size()) - 1;
			}
			else
			{
				g_assembly.bodySlotStatus[bs] = "Failed (0 meshes)";
			}
		}

		// Face/hair from composite slot_type 5/6 are NOT loaded as standalone models.
		// Their vertices use bone-local coordinates (head bone) rather than character-
		// local space, so they appear as floating meshes with the shared world transform.
		// Proper rendering requires skeleton-aware vertex skinning at load time --
		// transforming face/hair vertices by the head bone's bind-pose matrix before
		// adding them to the scene. This is a future enhancement.
		g_assembly.faceStatus = (g_charData.face_file_id != 0)
			? "Not loaded (needs head-bone positioning)"
			: "";

		// 3b. Load each equipment slot
		for (int i = 0; i < CharacterData::SlotCount; i++)
		{
			auto& slot = g_charData.slots[i];
			slot.loaded = false;
			slot.mesh_ids.clear();

			if (slot.dat_file_id == 0)
			{
				g_assembly.perSlotStatus[i] = "No file ID";
				g_assembly.slotsSkipped++;
				continue;
			}

			int mft = ResolveHash(slot.dat_file_id, dat_manager, hash_index);
			if (mft < 0)
			{
				g_assembly.perSlotStatus[i] = "Not in DAT";
				g_assembly.slotsFailed++;
				continue;
			}

			auto piece = LoadSingleModelIntoScene(
				slot.dat_file_id, dat_manager, map_renderer, hash_index, sharedWorld);

			if (piece.meshIds.empty())
			{
				char buf[64];
				snprintf(buf, sizeof(buf), "0 meshes (type mismatch or parse fail)");
				g_assembly.perSlotStatus[i] = buf;
				g_assembly.slotsFailed++;
				continue;
			}

			size_t fa1Bones = CountFA1Bones(piece);
			if (fa1Bones > bestFA1BoneCount)
				bestFA1BoneCount = fa1Bones;

			char lbl[64];
			snprintf(lbl, sizeof(lbl), "%s (%u)", CharacterSlotName(i), slot.dat_file_id);
			AddAnimHashCandidate(piece.modelHash0, piece.modelHash1, piece.fileHash, fa1Bones, lbl);

			slot.mesh_ids = piece.meshIds;
			slot.loaded = true;

			AccumulatePiece(piece, allMeshIds, allMeshes, allPerObjectCBs, allPerMeshTexIds);

			char buf[64];
			snprintf(buf, sizeof(buf), "OK (%zu meshes)", piece.meshIds.size());
			g_assembly.perSlotStatus[i] = buf;
			g_assembly.slotsLoaded++;

			loadedPieces.push_back(std::move(piece));
			if (fa1Bones == bestFA1BoneCount && fa1Bones > 0)
				bestBindPoseIdx = static_cast<int>(loadedPieces.size()) - 1;
		}

		// 3c. Establish bind pose from the piece with the most FA1 bones.
		// This ensures we get the full character skeleton (e.g. 88 bones from
		// chest armor) rather than a trivial 2-bone skeleton from a weapon.
		bool bindPoseEstablished = false;
		if (bestBindPoseIdx >= 0)
		{
			EstablishBindPoseFromPiece(loadedPieces[bestBindPoseIdx]);
			bindPoseEstablished = true;
		}
		else if (!loadedPieces.empty())
		{
			EstablishBindPoseFromPiece(loadedPieces.front());
			bindPoseEstablished = true;
		}

		g_assembly.totalMeshes = static_cast<int>(allMeshIds.size());

		if (allMeshIds.empty())
		{
			SetStatus("All slots failed to load meshes");
			return;
		}

		// 4. Populate animation state with combined mesh + bone data
		g_animationState.meshIds = allMeshIds;
		g_animationState.originalMeshes = allMeshes;
		g_animationState.perMeshPerObjectCB = allPerObjectCBs;
		g_animationState.perMeshTextureIds = allPerMeshTexIds;
		g_animationState.SetSubmeshInfo(allMeshes.size());

		float modelWidth = bMaxX - bMinX;
		float modelHeight = bMaxY - bMinY;
		float modelDepth = bMaxZ - bMinZ;
		float maxDim = std::max({ modelWidth, modelHeight, modelDepth });
		g_animationState.meshScale = (maxDim > 0.001f) ? (10000.0f / maxDim) : 1.0f;

		// 5. Activate the model viewer
		ActivateModelViewer(map_renderer);
		g_assembly.assembled = true;

		// 6. Try embedded animation from ALL loaded pieces.
		// ParseAnimationFromFile scans BB9/BB8/FA1/FA0 chunks (broader than
		// CountFA1Bones which only finds FA0/FA1). Pick the clip with the most
		// bones so e.g. a 105-bone chest animation wins over a 2-bone weapon clip.
		size_t bestAnimBones = 0;
		int bestAnimPieceIdx = -1;

		for (size_t pi = 0; pi < loadedPieces.size(); pi++)
		{
			size_t bones = CountAnimBones(loadedPieces[pi]);
			if (bones > bestAnimBones)
			{
				bestAnimBones = bones;
				bestAnimPieceIdx = static_cast<int>(pi);
			}
		}

		bool embeddedAnimFound = false;
		if (bestAnimPieceIdx >= 0 && bestAnimBones > 10)
			embeddedAnimFound = InitializeAnimationFromPiece(loadedPieces[bestAnimPieceIdx]);

		if (embeddedAnimFound)
			g_animFound = true;
		else
			BeginAnimHashRetry();

		int bodyLoaded = 0;
		for (int bs = 0; bs < BODY_SLOT_COUNT; bs++)
			if (g_assembly.bodySlotLoaded[bs]) bodyLoaded++;

		size_t reportedBones = bestFA1BoneCount > 0 ? bestFA1BoneCount : bestAnimBones;

		char msg[256];
		if (embeddedAnimFound)
			snprintf(msg, sizeof(msg),
				"Assembled %d equip + %d/%d body (%d meshes) | Skeleton: %s (%zu bones) | Anim: embedded OK (%zu bones)",
				g_assembly.slotsLoaded, bodyLoaded, BODY_SLOT_COUNT,
				g_assembly.totalMeshes,
				bindPoseEstablished ? "OK" : "none",
				reportedBones,
				bestAnimBones);
		else
			snprintf(msg, sizeof(msg),
				"Assembled %d equip + %d/%d body (%d meshes) | Skeleton: %s (%zu bones) | Anim: searching %d candidates...",
				g_assembly.slotsLoaded, bodyLoaded, BODY_SLOT_COUNT,
				g_assembly.totalMeshes,
				bindPoseEstablished ? "OK" : "none",
				reportedBones,
				static_cast<int>(g_animHashCandidates.size()));
		SetStatus(msg);
	}

	// Load an arbitrary file hash (single model via parse_file)
	void LoadManualModel(uint32_t file_hash,
		DATManager* dat_manager,
		MapRenderer* map_renderer,
		std::unordered_map<int, std::vector<int>>& hash_index)
	{
		if (LoadModelByHash(file_hash, dat_manager, map_renderer, hash_index))
		{
			char msg[64];
			snprintf(msg, sizeof(msg), "Loaded model %u", file_hash);
			SetStatus(msg);
		}
	}
}

void draw_character_builder(
	std::map<int, std::unique_ptr<DATManager>>& dat_managers,
	int dat_manager_index,
	MapRenderer* map_renderer,
	std::unordered_map<int, std::vector<int>>& hash_index)
{
	if (!GuiGlobalConstants::is_character_builder_open) return;

	DATManager* dat_manager = dat_managers[dat_manager_index].get();
	SetAnimationDATManagers(&dat_managers);

	ImGui::SetNextWindowSize(ImVec2(620, 760), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Character Builder", &GuiGlobalConstants::is_character_builder_open))
	{
		ImGui::End();
		return;
	}

	if (g_statusTimer > 0.f)
		g_statusTimer -= ImGui::GetIO().DeltaTime;

	// Tick animation hash retry each frame
	TickAnimHashRetry();

	// Import / Clear
	if (ImGui::Button("Import from Clipboard"))
	{
		const char* clip = ImGui::GetClipboardText();
		if (clip && clip[0])
		{
			if (g_charData.ParseFromJson(clip))
			{
				g_bodyCandidateIdx = 0;
				int autoAssigned = 0;
				for (int bs = 0; bs < BODY_SLOT_COUNT && bs < CharacterData::BodySlotCount; bs++)
				{
					if (g_charData.body_slot_assignments[bs] != 0)
					{
						g_bodySlotHash[bs] = g_charData.body_slot_assignments[bs];
						autoAssigned++;
					}
				}
				char msg[128];
				snprintf(msg, sizeof(msg), "Import OK! %d body candidates, %d body slots auto-assigned",
					(int)g_charData.body_candidates.size(), autoAssigned);
				SetStatus(msg);
			}
			else
				SetStatus("Failed to parse JSON from clipboard");
		}
		else
			SetStatus("Clipboard is empty");
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear All"))
	{
		g_charData.Clear();
		g_lastMftIndex = -1;
		g_lastLoadSuccess = false;
		g_assembly = {};
		g_bodyCandidateIdx = 0;
		for (int bs = 0; bs < BODY_SLOT_COUNT; bs++)
		{
			g_bodySlotHash[bs] = 0;
			g_bodySlotLoaded[bs] = false;
			g_bodySlotStatus[bs].clear();
		}
		g_animHashCandidates.clear();
		g_animRetryIdx = -1;
		g_animSearchActive = false;
		g_animFound = false;
		g_manualAnimHashBuf[0] = '\0';
		CancelAnimationSearch();
		SetStatus("Cleared");
	}

	if (g_statusTimer > 0.f && !g_statusMsg.empty())
	{
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1.f, 0.9f, 0.3f, 1.f), "%s", g_statusMsg.c_str());
	}

	if (!g_charData.valid)
	{
		ImGui::Separator();
		ImGui::TextWrapped(
			"Import a character from GW Toolbox Model Inspector:\n"
			"1. In GW Toolbox, target a character\n"
			"2. Open Model Inspector and click 'Export JSON'\n"
			"3. Click 'Import from Clipboard' above");
		ImGui::End();
		return;
	}

	ImGui::Separator();

	// Character info
	ImGui::Text("Name: %s", g_charData.name.c_str());
	ImGui::SameLine(0, 20);
	ImGui::Text("Prof: %s / %s",
		ProfessionName(g_charData.profession),
		ProfessionName(g_charData.secondary));
	ImGui::Text("Sex: %s  |  Level: %u  |  Campaign: %u  |  NPC: %s",
		g_charData.sex == 0 ? "Male" : "Female",
		g_charData.level, g_charData.campaign,
		g_charData.is_npc ? "Yes" : "No");
	ImGui::Text("Appearance: 0x%08X  (Hair=%u Color=%u Face=%u Skin=%u Height=%u)",
		g_charData.appearance_bitmap,
		g_charData.appearance.hair_style, g_charData.appearance.hair_color,
		g_charData.appearance.face, g_charData.appearance.skin_color,
		g_charData.appearance.height);

	if (g_charData.face_file_id || g_charData.hair_file_id)
	{
		ImGui::Text("Face: %u (0x%X)  |  Hair: %u (0x%X)",
			g_charData.face_file_id, g_charData.face_file_id,
			g_charData.hair_file_id, g_charData.hair_file_id);
	}

	if (g_charData.is_npc)
	{
		ImGui::Text("NPC Model: %u (0x%X)  |  Skin: %u (0x%X)",
			g_charData.npc_model_file_id, g_charData.npc_model_file_id,
			g_charData.npc_skin_file_id, g_charData.npc_skin_file_id);
	}

	ImGui::Separator();

	// Assemble All button
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.2f, 1.f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.25f, 1.f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.45f, 0.15f, 1.f));
		if (ImGui::Button("Assemble All Pieces", ImVec2(-FLT_MIN, 0)))
		{
			LoadAllAndAssemble(dat_manager, map_renderer, hash_index);
		}
		ImGui::PopStyleColor(3);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Loads body slot meshes (Chest/Legs/Head/Feet/Hands if set)\n+ all equipment slots into a single 3D scene");
	}

	ImGui::Separator();

	// Body slots section
	if (ImGui::CollapsingHeader("Body Slots (5 parts)", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const char* profName = ProfessionName(g_charData.profession);
		const char* sexName = g_charData.sex == 0 ? "Male" : "Female";

		ImGui::TextWrapped(
			"Browse body candidates with the arrows. Each click shows the model in 3D. "
			"When you see the right part, click 'Set as Chest/Legs/etc.' to assign it.");

		const auto& cands = g_charData.body_candidates;

		// --- Candidate browser with auto-preview ---
		if (!cands.empty())
		{
			if (g_bodyCandidateIdx >= (int)cands.size())
				g_bodyCandidateIdx = 0;
			if (g_bodyCandidateIdx < 0)
				g_bodyCandidateIdx = 0;

			ImGui::Spacing();
			ImGui::Text("Candidates: %d total for %s %s", (int)cands.size(), sexName, profName);

			bool changed = false;

			if (ImGui::SmallButton("<<##prev10"))
			{
				g_bodyCandidateIdx -= 10;
				if (g_bodyCandidateIdx < 0)
					g_bodyCandidateIdx = std::max(0, (int)cands.size() + g_bodyCandidateIdx);
				changed = true;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("<##prev"))
			{
				g_bodyCandidateIdx--;
				if (g_bodyCandidateIdx < 0) g_bodyCandidateIdx = (int)cands.size() - 1;
				changed = true;
			}
			ImGui::SameLine();
			ImGui::Text("%d / %d  |  Hash: %u",
				g_bodyCandidateIdx + 1, (int)cands.size(), cands[g_bodyCandidateIdx]);
			ImGui::SameLine();
			if (ImGui::SmallButton(">##next"))
			{
				g_bodyCandidateIdx++;
				if (g_bodyCandidateIdx >= (int)cands.size()) g_bodyCandidateIdx = 0;
				changed = true;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(">>##next10"))
			{
				g_bodyCandidateIdx += 10;
				if (g_bodyCandidateIdx >= (int)cands.size())
					g_bodyCandidateIdx = g_bodyCandidateIdx % (int)cands.size();
				changed = true;
			}

			// Auto-preview on navigation
			if (changed)
			{
				LoadManualModel(cands[g_bodyCandidateIdx], dat_manager, map_renderer, hash_index);
			}

			// "Set as X" buttons -- assign current candidate to a body slot
			ImGui::Spacing();
			ImGui::Text("Assign current to:");
			for (int bs = 0; bs < BODY_SLOT_COUNT; bs++)
			{
				if (bs > 0) ImGui::SameLine();
				char btnLabel[32];
				snprintf(btnLabel, sizeof(btnLabel), "%s##set_%d", g_bodySlotNames[bs], bs);
				if (ImGui::Button(btnLabel))
				{
					g_bodySlotHash[bs] = cands[g_bodyCandidateIdx];
					char msg[128];
					snprintf(msg, sizeof(msg), "Set %s = %u", g_bodySlotNames[bs], cands[g_bodyCandidateIdx]);
					SetStatus(msg);
				}
			}
		}
		else if (g_charData.valid)
		{
			ImGui::TextDisabled("No body candidates. Re-export from Toolbox to get them.");
		}

		// --- Current body slot assignments ---
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Text("Assigned body slots:");

		if (ImGui::BeginTable("##body_assign", 3,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("Part", ImGuiTableColumnFlags_WidthFixed, 55.f);
			ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 100.f);
			ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			for (int bs = 0; bs < BODY_SLOT_COUNT; bs++)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(g_bodySlotNames[bs]);

				ImGui::TableNextColumn();
				if (g_bodySlotHash[bs] != 0)
					ImGui::TextColored(ImVec4(0.3f, 1.f, 0.3f, 1.f), "%u", g_bodySlotHash[bs]);
				else
					ImGui::TextDisabled("not set");

				ImGui::TableNextColumn();
				if (g_assembly.assembled)
				{
					if (g_assembly.bodySlotLoaded[bs])
						ImGui::TextColored(ImVec4(0.3f, 1.f, 0.3f, 1.f), "%s",
							g_assembly.bodySlotStatus[bs].c_str());
					else if (!g_assembly.bodySlotStatus[bs].empty())
						ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "%s",
							g_assembly.bodySlotStatus[bs].c_str());
					else
						ImGui::TextDisabled("-");
				}
				else
					ImGui::TextDisabled("-");
			}

			// Face row
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.f, 1.f), "Face");
			ImGui::TableNextColumn();
			if (g_charData.face_file_id != 0)
				ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.f, 1.f), "%u", g_charData.face_file_id);
			else
				ImGui::TextDisabled("not set");
			ImGui::TableNextColumn();
			if (g_assembly.assembled && !g_assembly.faceStatus.empty())
				ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", g_assembly.faceStatus.c_str());
			else
				ImGui::TextDisabled("-");

			// Hair row
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.f, 1.f), "Hair");
			ImGui::TableNextColumn();
			if (g_charData.hair_file_id != 0)
				ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.f, 1.f), "%u", g_charData.hair_file_id);
			else
				ImGui::TextDisabled("not set");
			ImGui::TableNextColumn();
			ImGui::TextDisabled("Provided by Head body slot");

			ImGui::EndTable();
		}

		// Clear body slots
		ImGui::SameLine();
		if (ImGui::SmallButton("Clear Body"))
		{
			for (int bs = 0; bs < BODY_SLOT_COUNT; bs++)
				g_bodySlotHash[bs] = 0;
			SetStatus("Cleared body slot assignments");
		}
	}

	ImGui::Separator();

	// Equipment table
	if (ImGui::BeginTable("##char_slots", 7,
		ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
	{
		ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 90.f);
		ImGui::TableSetupColumn("DAT File ID", ImGuiTableColumnFlags_WidthFixed, 100.f);
		ImGui::TableSetupColumn("Composite", ImGuiTableColumnFlags_WidthFixed, 80.f);
		ImGui::TableSetupColumn("Dyes", ImGuiTableColumnFlags_WidthFixed, 100.f);
		ImGui::TableSetupColumn("MFT", ImGuiTableColumnFlags_WidthFixed, 60.f);
		ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 120.f);
		ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();

		for (int i = 0; i < CharacterData::SlotCount; i++)
		{
			const auto& slot = g_charData.slots[i];
			if (slot.dat_file_id == 0) continue;

			int mft = ResolveHash(slot.dat_file_id, dat_manager, hash_index);

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%s", CharacterSlotName(i));

			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%u", slot.dat_file_id);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("0x%X", slot.dat_file_id);

			ImGui::TableSetColumnIndex(2);
			if (slot.composite_id)
				ImGui::Text("%u", slot.composite_id);
			else
				ImGui::TextDisabled("-");

			ImGui::TableSetColumnIndex(3);
			DrawDyeSwatches(slot.dyes);

			ImGui::TableSetColumnIndex(4);
			if (mft >= 0)
				ImGui::Text("%d", mft);
			else
				ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "N/A");

			ImGui::TableSetColumnIndex(5);
			if (g_assembly.assembled)
			{
				const auto& status = g_assembly.perSlotStatus[i];
				if (slot.loaded)
				{
					ImGui::TextColored(ImVec4(0.3f, 1.f, 0.3f, 1.f), "%s", status.c_str());
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%zu GPU mesh(es)", slot.mesh_ids.size());
				}
				else if (!status.empty())
					ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "%s", status.c_str());
				else
					ImGui::TextDisabled("-");
			}
			else
			{
				ImGui::TextDisabled("-");
			}

			ImGui::TableSetColumnIndex(6);
			char btnId[32];
			snprintf(btnId, sizeof(btnId), "View##slot_%d", i);
			if (mft >= 0 && ImGui::SmallButton(btnId))
			{
				LoadSingleSlot(i, dat_manager, map_renderer, hash_index);
			}
			if (mft < 0)
			{
				ImGui::TextDisabled("Not in DAT");
			}
		}

		ImGui::EndTable();
	}

	// Assembly summary
	if (g_assembly.assembled)
	{
		int bodyCount = 0;
		for (int bs = 0; bs < BODY_SLOT_COUNT; bs++)
			if (g_assembly.bodySlotLoaded[bs]) bodyCount++;
		ImGui::Text("Assembly: %d equip + %d/%d body | %d failed, %d skipped | Total: %d meshes",
			g_assembly.slotsLoaded, bodyCount, BODY_SLOT_COUNT,
			g_assembly.slotsFailed, g_assembly.slotsSkipped,
			g_assembly.totalMeshes);
	}

	// Animation section
	ImGui::Separator();
	if (ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen))
	{
		// Status
		if (g_animFound || g_animationState.hasAnimation)
		{
			ImGui::TextColored(ImVec4(0.3f, 1.f, 0.3f, 1.f), "Animation loaded");
			if (g_animationState.clip)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(%zu bones, %zu segments)",
					g_animationState.skeleton ? g_animationState.skeleton->bones.size() : 0,
					g_animationState.clip->animationSegments.size());
			}
		}
		else if (g_animSearchActive || g_animationState.searchInProgress.load())
		{
			int total = g_animationState.totalFiles.load();
			int processed = g_animationState.filesProcessed.load();
			float frac = (total > 0) ? static_cast<float>(processed) / total : 0.f;

			char overlay[128];
			if (g_animRetryIdx >= 0 && g_animRetryIdx < static_cast<int>(g_animHashCandidates.size()))
				snprintf(overlay, sizeof(overlay), "Trying %d/%d: %s (%d/%d files)",
					g_animRetryIdx + 1, static_cast<int>(g_animHashCandidates.size()),
					g_animHashCandidates[g_animRetryIdx].label.c_str(),
					processed, total);
			else
				snprintf(overlay, sizeof(overlay), "%d / %d files", processed, total);

			ImGui::ProgressBar(frac, ImVec2(-1, 0), overlay);
		}
		else if (g_animHashCandidates.empty())
		{
			ImGui::TextDisabled("No model loaded (assemble first)");
		}
		else
		{
			ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "No animation found (%d candidates tried)",
				static_cast<int>(g_animHashCandidates.size()));
		}

		// Hash pair selector
		if (!g_animHashCandidates.empty())
		{
			ImGui::Text("Hash candidates (%d):", static_cast<int>(g_animHashCandidates.size()));
			static int selectedCandidate = 0;
			if (selectedCandidate >= static_cast<int>(g_animHashCandidates.size()))
				selectedCandidate = 0;

			ImGui::SetNextItemWidth(300.f);
			if (ImGui::BeginCombo("##anim_hash_combo",
				g_animHashCandidates[selectedCandidate].label.c_str()))
			{
				for (int i = 0; i < static_cast<int>(g_animHashCandidates.size()); i++)
				{
					char item[128];
					snprintf(item, sizeof(item), "[%d] %s (0x%X, 0x%X) %zu bones",
						i, g_animHashCandidates[i].label.c_str(),
						g_animHashCandidates[i].hash0, g_animHashCandidates[i].hash1,
						g_animHashCandidates[i].fa1BoneCount);
					if (ImGui::Selectable(item, i == selectedCandidate))
						selectedCandidate = i;
				}
				ImGui::EndCombo();
			}

			ImGui::SameLine();
			if (ImGui::Button("Search##anim_search") && !g_animationState.searchInProgress.load())
			{
				auto& c = g_animHashCandidates[selectedCandidate];
				CancelAnimationSearch();
				g_animationState.hasAnimation = false;
				g_animationState.clip.reset();
				g_animationState.skeleton.reset();
				g_animationState.controller.reset();
				g_animationState.hasSkinnedMeshes = false;
				g_animationState.searchResults.clear();
				g_animationState.SetModelHashes(c.hash0, c.hash1, c.fileHash);
				g_animRetryIdx = -1;
				g_animFound = false;
				AutoLoadAnimationFromStoredManagers();
				g_animSearchActive = true;
			}
		}

		// Manual animation hash (segment hash, file hash, or model hash)
		ImGui::SetNextItemWidth(140.f);
		ImGui::InputText("##manual_anim_hash", g_manualAnimHashBuf, sizeof(g_manualAnimHashBuf));
		ImGui::SameLine();
		if (ImGui::Button("Load Anim by Hash"))
		{
			uint32_t manualHash = static_cast<uint32_t>(strtoul(g_manualAnimHashBuf, nullptr, 0));
			if (manualHash != 0)
			{
				bool found = false;

				// 1. Try as segment hash within the currently loaded clip
				if (!found && g_animationState.clip && g_animationState.controller &&
					!g_animationState.clip->animationSegments.empty())
				{
					const auto& segs = g_animationState.clip->animationSegments;
					const bool hasSrc = (g_animationState.clip->animationSegmentSourceTypes.size() == segs.size());
					size_t bestIdx = static_cast<size_t>(-1);
					uint32_t bestDur = 0;
					for (size_t i = 0; i < segs.size(); i++)
					{
						if (segs[i].hash != manualHash) continue;
						bool isLocal = !hasSrc || g_animationState.clip->GetSegmentSourceType(i) == 0;
						uint32_t dur = segs[i].GetDuration();
						if (isLocal && (bestIdx == static_cast<size_t>(-1) || dur > bestDur))
						{
							bestIdx = i;
							bestDur = dur;
						}
					}
					if (bestIdx != static_cast<size_t>(-1))
					{
						g_animationState.playbackMode = AnimationPlaybackMode::SegmentLoop;
						g_animationState.controller->SetPlaybackMode(
							GW::Animation::PlaybackMode::SegmentLoop);
						g_animationState.controller->SetSegment(bestIdx);
						found = true;
						char msg[96];
						snprintf(msg, sizeof(msg), "Playing segment %zu (hash 0x%X)",
							bestIdx, manualHash);
						SetStatus(msg);
					}
				}

				// 2. Scan ALL search results for the segment hash
				//    (the segment may live in a different animation file)
				if (!found && !g_animationState.searchResults.empty())
				{
					SetStatus("Scanning search results for segment hash...");
					for (const auto& sr : g_animationState.searchResults)
					{
						auto dmIt = dat_managers.find(sr.datAlias);
						if (dmIt == dat_managers.end() || !dmIt->second)
							continue;
						DATManager* mgr = dmIt->second.get();
						unsigned char* raw = mgr->read_file(sr.mftIndex);
						if (!raw) continue;
						const auto& mft = mgr->get_MFT();
						size_t fileSize = mft[sr.mftIndex].uncompressedSize;
						auto clipOpt = GW::Parsers::ParseAnimationFromFile(
							reinterpret_cast<uint8_t*>(raw), fileSize);
						delete[] raw;
						if (!clipOpt || !clipOpt->IsValid())
							continue;
						const auto& segs = clipOpt->animationSegments;
						const bool hasSrc = (clipOpt->animationSegmentSourceTypes.size() == segs.size());
						size_t bestIdx = static_cast<size_t>(-1);
						uint32_t bestDur = 0;
						for (size_t i = 0; i < segs.size(); i++)
						{
							if (segs[i].hash != manualHash) continue;
							bool isLocal = !hasSrc || clipOpt->GetSegmentSourceType(i) == 0;
							uint32_t dur = segs[i].GetDuration();
							if (isLocal && (bestIdx == static_cast<size_t>(-1) || dur > bestDur))
							{
								bestIdx = i;
								bestDur = dur;
							}
						}
						if (bestIdx == static_cast<size_t>(-1))
						{
							for (size_t i = 0; i < segs.size(); i++)
							{
								if (segs[i].hash != manualHash) continue;
								uint32_t dur = segs[i].GetDuration();
								if (bestIdx == static_cast<size_t>(-1) || dur > bestDur)
								{
									bestIdx = i;
									bestDur = dur;
								}
							}
						}
						if (bestIdx == static_cast<size_t>(-1))
							continue;

						auto clip = std::make_shared<GW::Animation::AnimationClip>(
							std::move(*clipOpt));
						auto skeleton = std::make_shared<GW::Animation::Skeleton>(
							GW::Parsers::BB9AnimationParser::CreateSkeleton(*clip));
						g_animationState.Initialize(clip, skeleton, sr.fileId);
						g_animationState.playbackMode = AnimationPlaybackMode::SegmentLoop;
						g_animationState.controller->SetPlaybackMode(
							GW::Animation::PlaybackMode::SegmentLoop);
						g_animationState.controller->SetSegment(bestIdx);
						g_animFound = true;
						g_animSearchActive = false;
						g_animRetryIdx = -1;
						found = true;
						char msg[128];
						snprintf(msg, sizeof(msg),
							"Loaded file 0x%X, playing segment %zu (hash 0x%X)",
							sr.fileId, bestIdx, manualHash);
						SetStatus(msg);
						break;
					}
					if (!found)
						SetStatus("Segment hash not found in any search result");
				}

				// 3. Try as file hash: hash_index then direct MFT scan
				if (!found)
				{
					int mftIdx = ResolveHash(manualHash, dat_manager, hash_index);
					if (mftIdx < 0)
					{
						const auto& mft = dat_manager->get_MFT();
						for (size_t i = 0; i < mft.size(); i++)
						{
							if (static_cast<uint32_t>(mft[i].Hash) == manualHash)
							{
								mftIdx = static_cast<int>(i);
								break;
							}
						}
					}

					if (mftIdx >= 0)
					{
						unsigned char* raw = dat_manager->read_file(mftIdx);
						if (raw)
						{
							const auto& mft = dat_manager->get_MFT();
							std::vector<uint8_t> rawVec(raw, raw + mft[mftIdx].uncompressedSize);
							delete raw;

							auto clipOpt = GW::Parsers::ParseAnimationFromFile(
								rawVec.data(), rawVec.size());
							if (clipOpt && clipOpt->IsValid())
							{
								auto clip = std::make_shared<GW::Animation::AnimationClip>(
									std::move(*clipOpt));
								auto skeleton = std::make_shared<GW::Animation::Skeleton>(
									GW::Parsers::BB9AnimationParser::CreateSkeleton(*clip));
								g_animationState.Initialize(clip, skeleton, manualHash);
								g_animFound = true;
								g_animSearchActive = false;
								g_animRetryIdx = -1;
								SetStatus("Animation loaded from file");
								found = true;
							}
							else
								SetStatus("File found but no valid animation clip inside");
						}
						else
							SetStatus("Failed to read file from DAT");
					}
				}

				// 4. Not found as segment or file -- search by model hash
				if (!found)
				{
					CancelAnimationSearch();
					g_animationState.hasAnimation = false;
					g_animationState.clip.reset();
					g_animationState.skeleton.reset();
					g_animationState.controller.reset();
					g_animationState.hasSkinnedMeshes = false;
					g_animationState.searchResults.clear();
					g_animFound = false;
					g_animRetryIdx = -1;
					g_animationState.SetModelHashes(manualHash, 0, 0);
					StartAnimationSearchFromStoredManagers();
					g_animSearchActive = true;
					SetStatus("Searching DAT for matching animations...");
				}
			}
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Enter a segment hash, file hash, or model hash\n(decimal or 0xHex)\nSegment hashes are searched across all found animation files");
	}

	// Manual model loading
	ImGui::Separator();
	if (ImGui::CollapsingHeader("Manual Model Loading"))
	{
		static char manualHashBuf[32] = "";
		ImGui::SetNextItemWidth(140.f);
		ImGui::InputText("File Hash (decimal)##manual_hash", manualHashBuf, sizeof(manualHashBuf));
		ImGui::SameLine();
		if (ImGui::Button("Load##manual"))
		{
			uint32_t fh = 0;
			if (sscanf(manualHashBuf, "%u", &fh) == 1 && fh != 0)
				LoadManualModel(fh, dat_manager, map_renderer, hash_index);
			else
				SetStatus("Invalid file hash");
		}
	}

	// NPC model files
	if (g_charData.is_npc && !g_charData.npc_model_files.empty())
	{
		ImGui::Separator();
		if (ImGui::CollapsingHeader("NPC Model Files"))
		{
			for (size_t i = 0; i < g_charData.npc_model_files.size(); i++)
			{
				uint32_t fh = g_charData.npc_model_files[i];
				int mft = ResolveHash(fh, dat_manager, hash_index);
				ImGui::Text("[%zu] %u (0x%X)  MFT:%d", i, fh, fh, mft);
				ImGui::SameLine();
				char lbtn[32];
				snprintf(lbtn, sizeof(lbtn), "Load##npc_%zu", i);
				if (ImGui::SmallButton(lbtn))
				{
					LoadManualModel(fh, dat_manager, map_renderer, hash_index);
				}
			}
		}
	}

	// Debug info
	ImGui::Separator();
	if (ImGui::CollapsingHeader("Debug Info", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Text("Last MFT index: %d", g_lastMftIndex);
		ImGui::Text("Last file type: %d%s", g_lastFileType,
			g_lastFileType == FFNA_Type2 ? " (FFNA Model)" :
			g_lastFileType == FFNA_Type3 ? " (FFNA Map)" :
			g_lastFileType == FFNA_Unknown ? " (FFNA Unknown)" : " (not a model)");
		if (g_lastFileType >= 0 && g_lastFileType != FFNA_Type2)
		{
			ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f),
				"  WARNING: File is NOT a 3D model (type %d). Only FFNA_Type2 (%d) contains geometry.",
				g_lastFileType, static_cast<int>(FFNA_Type2));
		}
		ImGui::Text("Last load: %s", g_lastLoadSuccess ? "SUCCESS" : "FAILED");

		ImGui::Spacing();
		ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "Snapshot (right after parse_file):");
		if (g_pfDebug.called)
		{
			ImGui::Text("  anim meshIds=%d  origMeshes=%d",
				g_pfDebug.animMeshIds, g_pfDebug.animOrigMeshes);
			ImGui::Text("  viewer meshIds=%d  meshes=%d  active=%s",
				g_pfDebug.viewerMeshIds, g_pfDebug.viewerMeshes,
				g_pfDebug.viewerActive ? "YES" : "NO");
			if (g_pfDebug.animMeshIds == 0 && g_pfDebug.animOrigMeshes == 0 &&
				g_lastFileType == FFNA_Type2)
			{
				ImGui::TextColored(ImVec4(1.f, 0.5f, 0.2f, 1.f),
					"  Model file parsed OK but has 0 geometry meshes.");
			}
		}
		else
		{
			ImGui::TextDisabled("  (no load attempted yet)");
		}

		ImGui::Spacing();
		ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.f, 1.f), "Current frame:");
		ImGui::Text("  anim meshIds=%zu  origMeshes=%zu",
			g_animationState.meshIds.size(),
			g_animationState.originalMeshes.size());
		ImGui::Text("  viewer meshIds=%zu  meshes=%zu  active=%s",
			g_modelViewerState.meshIds.size(),
			g_modelViewerState.meshes.size(),
			g_modelViewerState.isActive ? "YES" : "NO");

		if (g_assembly.assembled)
		{
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(0.5f, 1.f, 0.5f, 1.f), "Assembly state:");
			ImGui::Text("  Equip Slots: Loaded=%d  Failed=%d  Skipped=%d",
				g_assembly.slotsLoaded, g_assembly.slotsFailed, g_assembly.slotsSkipped);
			for (int bs = 0; bs < BODY_SLOT_COUNT; bs++)
			{
				ImGui::Text("  Body %s: %s (%d meshes)", g_bodySlotNames[bs],
					g_assembly.bodySlotStatus[bs].empty() ? "-" : g_assembly.bodySlotStatus[bs].c_str(),
					g_assembly.bodySlotMeshCount[bs]);
			}
			if (!g_assembly.faceStatus.empty())
				ImGui::Text("  Face: %s", g_assembly.faceStatus.c_str());
			ImGui::Text("  Total meshes on GPU: %d (body: %d)", g_assembly.totalMeshes, g_assembly.totalBodyMeshes);
		}

		if (!g_animHashCandidates.empty())
		{
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(1.f, 1.f, 0.5f, 1.f), "Anim hash candidates:");
			for (size_t i = 0; i < g_animHashCandidates.size(); i++)
			{
				auto& c = g_animHashCandidates[i];
				ImGui::Text("  [%zu] %s  h0=0x%X h1=0x%X  fa1=%zu",
					i, c.label.c_str(), c.hash0, c.hash1, c.fa1BoneCount);
			}
			ImGui::Text("  searchResults: %zu  retryIdx: %d  resultIdx: %d",
				g_animationState.searchResults.size(), g_animRetryIdx, g_searchResultIdx);
		}

		if (g_modelViewerState.isActive)
		{
			const auto& bMin = g_modelViewerState.boundsMin;
			const auto& bMax = g_modelViewerState.boundsMax;
			ImGui::Text("  Bounds: (%.1f,%.1f,%.1f) to (%.1f,%.1f,%.1f)",
				bMin.x, bMin.y, bMin.z, bMax.x, bMax.y, bMax.z);
			auto* cam = g_modelViewerState.camera.get();
			if (cam)
			{
				auto pos = cam->GetPosition();
				ImGui::Text("  Camera: (%.1f, %.1f, %.1f)", pos.x, pos.y, pos.z);
			}
		}
	}

	// Viewer controls
	if (g_modelViewerState.isActive)
	{
		ImGui::Separator();
		if (ImGui::CollapsingHeader("Viewer Options", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::ColorEdit4("Background##cb_bg",
				&g_modelViewerState.options.backgroundColor.x,
				ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs);

			ImGui::Checkbox("Show Mesh##cb_mesh", &g_modelViewerState.options.showMesh);
			ImGui::SameLine();
			ImGui::Checkbox("Wireframe##cb_wire", &g_modelViewerState.options.showWireframe);

			ImGui::TextDisabled("Orbit: LMB drag  |  Zoom: Scroll  |  Pan: RMB drag");
		}

		ImGui::Separator();
		ImGui::TextWrapped(
			"Move or resize this window to see the 3D viewport behind it. "
			"The replay library is hidden while a model is loaded.");
		if (ImGui::Button("Exit 3D Viewer (back to library)"))
		{
			DeactivateModelViewer(map_renderer);
		}
	}

	ImGui::End();
}
