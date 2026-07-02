#include "pch.h"
#include "ReplayWindow.h"
#include "AssetBlacklist.h"
#include "MatchRatings.h"
#include "MatchNotes.h"
#include "MatchBookmarks.h"
#include "AgentSnapshotParser.h"
#include "StoCParser.h"
#include "SkillDatabase.h"
#include "DXMathHelpers.h"
#include "FontConfig.h"
#include "GuiGlobalConstants.h"
#include "MapBrowser.h"
#include "TextureCache.h"
#include "CursorSystem.h"
#include "SpatialAudioEngine.h"
#include "SoundCache.h"
#include "Parsers/BB9AnimationParser.h"
#include "Parsers/FileReferenceParser.h"
#include "ReplayWindow_Internal.h"
#include "../ThirdParty/nanosvg/nanosvg.h"
#include "../ThirdParty/nanosvg/nanosvgrast.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <algorithm>
#include <numeric>
#include <json.hpp>
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// Extracted from ReplayWindow.cpp (partial-class split). These remain
// ReplayWindow:: member functions; only their definitions live here.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Animated map prop setup (Isle of Solitude portal)
// ---------------------------------------------------------------------------

void ReplayWindow::SetupAnimatedProp(
    int propIndex,
    const FFNA_ModelFile& modelFile,
    uint32_t modelFileHash,
    const std::vector<Mesh>& meshes,
    const std::vector<PerObjectCB>& perObjectCBs,
    const std::vector<int>& meshIds,
    const std::vector<std::vector<int>>& perMeshTexIds,
    PixelShaderType pst,
    uint32_t segmentHash,
    size_t segmentFallbackIndex)
{
    if (!m_datManager || !m_hashIndex || !m_mapRenderer)
        return;

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    auto mit = m_hashIndex->find(static_cast<int>(modelFileHash));
    if (mit == m_hashIndex->end() || mit->second.empty())
        return;

    int mftIndex = mit->second.at(0);
    uint8_t* fileData = m_datManager->read_file(mftIndex);
    if (!fileData)
        return;

    size_t fileSize = m_datManager->get_MFT()[mftIndex].uncompressedSize;
    auto clipOpt = GW::Parsers::ParseAnimationFromFile(fileData, fileSize);
    delete[] fileData;

    if (!clipOpt || !clipOpt->IsValid())
        return;

    auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*clipOpt));
    clip->BuildAnimationGroups();

    const auto& segments = clip->animationSegments;
    if (segments.size() < 2)
        return;

    size_t targetSegment = SIZE_MAX;
    for (size_t i = 0; i < segments.size(); i++)
    {
        if (segments[i].hash == segmentHash)
        {
            targetSegment = i;
            break;
        }
    }
    if (targetSegment == SIZE_MAX)
        targetSegment = segmentFallbackIndex;

    auto controller = std::make_shared<GW::Animation::AnimationController>();
    controller->Initialize(clip);
    controller->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
    controller->SetSegment(targetSegment);
    controller->SetLooping(true);
    controller->SetPlaybackSpeed(100000.0f);
    controller->Play();

    const auto& geomModels = modelFile.geometry_chunk.models;
    size_t boneCount = clip->boneTracks.size();

    std::vector<std::shared_ptr<AnimatedMeshInstance>> animatedMeshes;
    for (size_t j = 0; j < meshes.size(); j++)
    {
        const auto& mesh = meshes[j];

        AnimationPanelState::SubmeshBoneData boneData;
        std::vector<uint32_t> vertexBoneGroups;
        if (j < geomModels.size())
        {
            const auto& geomModel = geomModels[j];
            boneData = AnimationPanelState::ExtractBoneData(
                geomModel.extra_data, geomModel.u0, geomModel.u1);

            vertexBoneGroups.reserve(geomModel.vertices.size());
            for (const auto& mv : geomModel.vertices)
                vertexBoneGroups.push_back(mv.group);
        }

        auto skinnedVerts = AnimationPanelState::CreateSkinnedVertices(
            mesh, boneData, vertexBoneGroups, boneCount,
            clip->hierarchyMode, j);

        // Debug: log bone assignments per submesh to file
        {
            std::set<uint32_t> uniqueBones;
            for (const auto& sv : skinnedVerts)
                uniqueBones.insert(sv.boneIndices[0]);
            std::string boneList;
            for (uint32_t b : uniqueBones)
                boneList += std::to_string(b) + " ";
            bool usedPalette = !boneData.groupToSkeletonBone.empty();
            std::ofstream dbg("door_debug.log", std::ios::app);
            dbg << "[SetupAnimatedProp] hash 0x" << std::hex << modelFileHash << std::dec
                << " submesh " << j << " : " << skinnedVerts.size()
                << " verts, bones: [" << boneList << "], palette="
                << usedPalette << ", boneCount=" << boneCount << "\n";
        }

        auto animMesh = std::make_shared<AnimatedMeshInstance>(
            device, skinnedVerts, mesh.indices, static_cast<int>(j));

        if (j < perMeshTexIds.size())
        {
            auto texSRVs = m_mapRenderer->GetTextureManager()->GetTextures(perMeshTexIds[j]);
            animMesh->SetTextures(texSRVs, 3);
        }

        animMesh->SetPerObjectData(perObjectCBs[j]);
        animatedMeshes.push_back(std::move(animMesh));
    }

    MapAnimatedProp prop;
    prop.controller = controller;
    prop.clip = clip;
    prop.meshes = std::move(animatedMeshes);
    prop.perObjectCBs = perObjectCBs;
    prop.staticMeshIds = meshIds;
    prop.pixelShaderType = pst;

    m_mapRenderer->AddAnimatedProp(std::move(prop));
}

// ---------------------------------------------------------------------------
// Obelisk Flag Stand — load and render 3D model (Isle of Meditation)
// ---------------------------------------------------------------------------

static std::ofstream g_obeliskLog;
static void ObeliskLog(const char* msg)
{
    OutputDebugStringA(msg);
    if (!g_obeliskLog.is_open())
        g_obeliskLog.open("obelisk_debug.txt", std::ios::trunc);
    if (g_obeliskLog.is_open())
    {
        g_obeliskLog << msg;
        g_obeliskLog.flush();
    }
}

// ApplyMapTransformToPos: definition below; declared in ReplayWindow_Internal.h

void ReplayWindow::SetupObeliskFlagStand()
{
    if (m_obeliskModelLoaded)
        return;
    m_obeliskModelLoaded = true;

    if (!m_datManager || !m_hashIndex || !m_mapRenderer)
    {
        ObeliskLog("[Obelisk] Missing datManager/hashIndex/mapRenderer\n");
        return;
    }

    if (m_flagTimeline.obelisk.standAgentId < 0)
    {
        ObeliskLog("[Obelisk] No obelisk stand agent found in timeline\n");
        return;
    }

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    constexpr uint32_t kObeliskFileHash = 0x212DB;

    auto mit = m_hashIndex->find(static_cast<int>(kObeliskFileHash));
    if (mit == m_hashIndex->end() || mit->second.empty())
    {
        ObeliskLog("[Obelisk] Hash 0x212DB not found in hash index\n");
        return;
    }

    int mftIndex = mit->second.at(0);

    ObeliskLog(std::format("[Obelisk] Loading model from MFT index {}\n", mftIndex).c_str());

    // Parse FFNA model (geometry + textures)
    FFNA_ModelFile modelFile;
    try {
        modelFile = m_datManager->parse_ffna_model_file(mftIndex);
    } catch (...) {
        ObeliskLog("[Obelisk] Exception parsing model file\n");
        return;
    }
    if (!modelFile.parsed_correctly)
    {
        ObeliskLog("[Obelisk] Model file not parsed correctly\n");
        return;
    }

    // Build meshes
    const auto& geom = modelFile.geometry_chunk;
    std::vector<Mesh> meshes;
    for (size_t j = 0; j < geom.models.size(); j++)
    {
        AMAT_file amat;
        if (modelFile.textures_parsed_correctly &&
            !modelFile.AMAT_filenames_chunk.texture_filenames.empty())
        {
            int subIdx = geom.models[j].unknown;
            if (!geom.tex_and_vertex_shader_struct.uts0.empty())
                subIdx %= static_cast<int>(geom.tex_and_vertex_shader_struct.uts0.size());
            if (!geom.uts1.empty())
            {
                const auto& uts1 = geom.uts1[subIdx % geom.uts1.size()];
                int amatIdx = ((uts1.some_flags0 >> 8) & 0xFF)
                    % static_cast<int>(modelFile.AMAT_filenames_chunk.texture_filenames.size());
                auto amatFn = modelFile.AMAT_filenames_chunk.texture_filenames[amatIdx];
                auto amatHash = decode_filename(amatFn.id0, amatFn.id1);
                auto aIt = m_hashIndex->find(amatHash);
                if (aIt != m_hashIndex->end())
                    amat = m_datManager->parse_amat_file(aIt->second.at(0));
            }
        }
        Mesh mesh = modelFile.GetMesh(static_cast<int>(j), amat);
        if (mesh.indices.size() % 3 == 0)
            meshes.push_back(mesh);
    }
    if (meshes.empty())
    {
        ObeliskLog("[Obelisk] No valid meshes built from model\n");
        return;
    }

    ObeliskLog(std::format("[Obelisk] Built {} meshes from model\n", meshes.size()).c_str());

    // Load textures
    auto* map_renderer = m_mapRenderer.get();
    std::vector<int> textureIds;
    if (modelFile.textures_parsed_correctly)
    {
        for (size_t t = 0; t < modelFile.texture_filenames_chunk.texture_filenames.size(); t++)
        {
            auto tf = modelFile.texture_filenames_chunk.texture_filenames[t];
            auto decoded = decode_filename(tf.id0, tf.id1);
            int texId = map_renderer->GetTextureManager()->GetTextureIdByHash(decoded);
            if (texId >= 0) { textureIds.push_back(texId); continue; }
            auto tit = m_hashIndex->find(decoded);
            if (tit != m_hashIndex->end())
            {
                DatTexture dt = m_datManager->parse_ffna_texture_file(tit->second.at(0));
                if (dt.width > 0 && dt.height > 0)
                {
                    map_renderer->GetTextureManager()->CreateTextureFromRGBA(
                        dt.width, dt.height, dt.rgba_data.data(), &texId, decoded);
                }
                textureIds.push_back(texId);
            }
        }
    }

    ObeliskLog(std::format("[Obelisk] textures_parsed_correctly={}, textureIds.size()={}\n",
        modelFile.textures_parsed_correctly, textureIds.size()).c_str());
    for (size_t t = 0; t < textureIds.size(); t++)
    {
        ObeliskLog(std::format("[Obelisk]   texture[{}] id={}\n", t, textureIds[t]).c_str());
    }

    // Remap per-mesh texture indices (must match prop pipeline pattern)
    std::vector<std::vector<int>> perMeshTexIds(meshes.size());
    for (size_t k = 0; k < meshes.size(); k++)
    {
        std::vector<uint8_t> remappedIndices;
        for (size_t ti = 0; ti < meshes[k].tex_indices.size(); ti++)
        {
            int idx = std::min(static_cast<int>(meshes[k].tex_indices[ti]),
                               static_cast<int>(textureIds.size()) - 1);
            if (idx >= 0 && idx < static_cast<int>(textureIds.size()))
            {
                perMeshTexIds[k].push_back(textureIds[idx]);
                remappedIndices.push_back(static_cast<uint8_t>(ti));
            }
        }
        meshes[k].tex_indices = remappedIndices;
    }

    // Compute world transform: place at obelisk agent position
    float ox = m_flagTimeline.obelisk.standX;
    float oy = m_flagTimeline.obelisk.standY;
    float oz = m_flagTimeline.obelisk.standZ;
    XMFLOAT3 pos = ApplyMapTransformToPos(ox, oy, oz, m_replayCtx.mapTransform);

    ObeliskLog(std::format("[Obelisk] GWCA pos ({:.0f}, {:.0f}, {:.0f}) -> render pos ({:.0f}, {:.0f}, {:.0f})\n",
        ox, oy, oz, pos.x, pos.y, pos.z).c_str());

    XMMATRIX worldMat = XMMatrixTranslation(pos.x, pos.y, pos.z);

    std::vector<PerObjectCB> perObjectCBs(meshes.size());
    for (size_t j = 0; j < meshes.size(); j++)
    {
        XMStoreFloat4x4(&perObjectCBs[j].world, worldMat);

        auto& mesh = meshes[j];
        if (mesh.uv_coord_indices.size() == mesh.tex_indices.size() &&
            mesh.uv_coord_indices.size() < MAX_NUM_TEX_INDICES &&
            modelFile.textures_parsed_correctly)
        {
            perObjectCBs[j].num_uv_texture_pairs = static_cast<uint32_t>(mesh.uv_coord_indices.size());
            for (size_t k = 0; k < mesh.uv_coord_indices.size(); k++)
            {
                perObjectCBs[j].uv_indices[k / 4][k % 4]       = static_cast<uint32_t>(mesh.uv_coord_indices[k]);
                perObjectCBs[j].texture_indices[k / 4][k % 4]   = static_cast<uint32_t>(mesh.tex_indices[k]);
                // Force opaque blending for flag submeshes (0,1) to avoid
                // alpha-discard issues with blend_flag=8 on flag textures
                uint32_t bf = static_cast<uint32_t>(mesh.blend_flags[k]);
                if (j <= 1) bf = 0;
                perObjectCBs[j].blend_flags[k / 4][k % 4]       = bf;
                perObjectCBs[j].texture_types[k / 4][k % 4]     = static_cast<uint32_t>(mesh.texture_types[k]);
            }
        }
    }

    for (size_t j = 0; j < meshes.size(); j++)
    {
        ObeliskLog(std::format("[Obelisk] Submesh {} : verts={} tris={} uv_coords={} tex_indices={} num_uv_tex_pairs={} perMeshTexIds={} cull={} mesh_alpha={}\n",
            j,
            meshes[j].vertices.size(),
            meshes[j].indices.size() / 3,
            meshes[j].uv_coord_indices.size(),
            meshes[j].tex_indices.size(),
            perObjectCBs[j].num_uv_texture_pairs,
            perMeshTexIds[j].size(),
            meshes[j].should_cull,
            perObjectCBs[j].mesh_alpha).c_str());

        if (j == 0 && !meshes[j].vertices.empty())
        {
            float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
            float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
            float minU = FLT_MAX, minV = FLT_MAX, maxU = -FLT_MAX, maxV = -FLT_MAX;
            for (const auto& v : meshes[j].vertices)
            {
                minX = std::min(minX, v.position.x); maxX = std::max(maxX, v.position.x);
                minY = std::min(minY, v.position.y); maxY = std::max(maxY, v.position.y);
                minZ = std::min(minZ, v.position.z); maxZ = std::max(maxZ, v.position.z);
                minU = std::min(minU, v.tex_coord0.x); maxU = std::max(maxU, v.tex_coord0.x);
                minV = std::min(minV, v.tex_coord0.y); maxV = std::max(maxV, v.tex_coord0.y);
            }
            ObeliskLog(std::format("[Obelisk]   bbox: X[{:.2f},{:.2f}] Y[{:.2f},{:.2f}] Z[{:.2f},{:.2f}]\n",
                minX, maxX, minY, maxY, minZ, maxZ).c_str());
            ObeliskLog(std::format("[Obelisk]   UV0 range: U[{:.4f},{:.4f}] V[{:.4f},{:.4f}]\n",
                minU, maxU, minV, maxV).c_str());
            ObeliskLog(std::format("[Obelisk]   size: dX={:.2f} dY={:.2f} dZ={:.2f}\n",
                maxX - minX, maxY - minY, maxZ - minZ).c_str());
        }

        for (size_t ti = 0; ti < perMeshTexIds[j].size(); ti++)
        {
            ObeliskLog(std::format("[Obelisk]   tex[{}] id={} blend={} (CB_blend={}) type={} uv={}\n",
                ti, perMeshTexIds[j][ti],
                ti < meshes[j].blend_flags.size() ? meshes[j].blend_flags[ti] : -1,
                perObjectCBs[j].blend_flags[ti / 4][ti % 4],
                ti < meshes[j].texture_types.size() ? meshes[j].texture_types[ti] : -1,
                ti < meshes[j].uv_coord_indices.size() ? meshes[j].uv_coord_indices[ti] : -1).c_str());
        }
    }

    auto pst = geom.unknown_tex_stuff1.empty() ? PixelShaderType::OldModel : PixelShaderType::NewModel;
    auto meshIds = map_renderer->AddProp(meshes, perObjectCBs, 0xFFFF0001u, pst);

    if (modelFile.textures_parsed_correctly)
    {
        for (size_t l = 0; l < meshIds.size() && l < perMeshTexIds.size(); l++)
        {
            auto texVec = map_renderer->GetTextureManager()->GetTextures(perMeshTexIds[l]);
            map_renderer->GetMeshManager()->SetTexturesForMesh(meshIds[l], texVec, 3);
            ObeliskLog(std::format("[Obelisk] Submesh {} textures bound: {} SRVs (meshId={})\n",
                l, texVec.size(), meshIds[l]).c_str());
            for (size_t ti2 = 0; ti2 < texVec.size(); ti2++)
            {
                ObeliskLog(std::format("[Obelisk]   SRV[{}] = {}\n",
                    ti2, texVec[ti2] ? "valid" : "NULL").c_str());
            }
        }
    }

    ObeliskLog(std::format("[Obelisk] Added {} static mesh IDs via AddProp\n", meshIds.size()).c_str());

    m_obeliskStaticMeshIds = meshIds;

    // Hide submeshes 0 and 1 in static pipeline immediately
    if (meshIds.size() > 0) map_renderer->SetMeshShouldRender(meshIds[0], false);
    if (meshIds.size() > 1) map_renderer->SetMeshShouldRender(meshIds[1], false);

    // Create a hanging flag banner quad using the flag texture (texture 178)
    // The pole (submesh 0) extends from ~(0,45,10) to ~(0,112,92) in model space.
    // We attach the banner to the outer portion of the pole, hanging downward.
    if (modelFile.textures_parsed_correctly && !perMeshTexIds.empty() && !perMeshTexIds[0].empty())
    {
        Mesh bannerMesh;
        float bannerHeight = 55.0f;

        // Pole direction in model space: from (0.3, 45, 10) to (0.3, 112, 92)
        // Banner attaches to the outer ~55% of the pole, hanging straight down
        XMFLOAT3 poleStart = { 0.3f, 45.0f, 10.0f };
        XMFLOAT3 poleEnd   = { 0.3f, 112.0f, 92.0f };

        float t0 = 0.45f; // start of banner along pole
        float t1 = 0.95f; // end of banner along pole

        XMFLOAT3 topLeft = {
            poleStart.x + t0 * (poleEnd.x - poleStart.x),
            poleStart.y + t0 * (poleEnd.y - poleStart.y),
            poleStart.z + t0 * (poleEnd.z - poleStart.z)
        };
        XMFLOAT3 topRight = {
            poleStart.x + t1 * (poleEnd.x - poleStart.x),
            poleStart.y + t1 * (poleEnd.y - poleStart.y),
            poleStart.z + t1 * (poleEnd.z - poleStart.z)
        };

        XMFLOAT3 normal = { 1.0f, 0.0f, 0.0f };

        GWVertex v0, v1, v2, v3;
        v0.position = topLeft;
        v0.normal = normal;
        v0.tex_coord0 = { 0.0f, 0.0f };

        v1.position = topRight;
        v1.normal = normal;
        v1.tex_coord0 = { 1.0f, 0.0f };

        v2.position = { topLeft.x, topLeft.y - bannerHeight, topLeft.z };
        v2.normal = normal;
        v2.tex_coord0 = { 0.0f, 1.0f };

        v3.position = { topRight.x, topRight.y - bannerHeight, topRight.z };
        v3.normal = normal;
        v3.tex_coord0 = { 1.0f, 1.0f };

        bannerMesh.vertices = { v0, v1, v2, v3 };
        bannerMesh.indices = { 0, 1, 2, 1, 3, 2 };
        bannerMesh.should_cull = false;
        bannerMesh.blend_state = BlendState::Opaque;
        bannerMesh.num_textures = 1;
        bannerMesh.uv_coord_indices = { 0 };
        bannerMesh.tex_indices = { 0 };
        bannerMesh.blend_flags = { 0 };
        bannerMesh.texture_types = { 0 };

        PerObjectCB bannerCB;
        XMStoreFloat4x4(&bannerCB.world, worldMat);
        bannerCB.num_uv_texture_pairs = 1;

        std::vector<Mesh> bannerMeshes = { bannerMesh };
        std::vector<PerObjectCB> bannerCBs = { bannerCB };
        auto bannerIds = map_renderer->AddProp(bannerMeshes, bannerCBs, 0xFFFF0002u, pst);

        if (!bannerIds.empty())
        {
            m_obeliskBannerMeshId = bannerIds[0];

            // Load red and blue flag DDS textures from disk
            auto loadFlagDDS = [&](const wchar_t* filename) -> ComPtr<ID3D11ShaderResourceView>
            {
                wchar_t exePath[MAX_PATH];
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                auto baseDir = std::filesystem::path(exePath).parent_path();
                for (int up = 0; up < 5; up++)
                {
                    if (std::filesystem::exists(baseDir / L"Textures" / L"Others_UI"))
                    { baseDir = baseDir / L"Textures" / L"Others_UI"; break; }
                    if (!baseDir.has_parent_path() || baseDir == baseDir.parent_path()) break;
                    baseDir = baseDir.parent_path();
                }
                auto fullPath = baseDir / filename;
                if (!std::filesystem::exists(fullPath)) return nullptr;

                DirectX::ScratchImage image;
                HRESULT hr2 = DirectX::LoadFromDDSFile(fullPath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
                if (FAILED(hr2)) return nullptr;

                const auto& meta = image.GetMetadata();
                if (meta.width == 0 || meta.height == 0) return nullptr;

                DirectX::ScratchImage decompressed;
                if (DirectX::IsCompressed(meta.format))
                {
                    hr2 = DirectX::Decompress(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM, decompressed);
                    if (FAILED(hr2)) return nullptr;
                    image = std::move(decompressed);
                }

                DirectX::ScratchImage converted;
                if (image.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM)
                {
                    hr2 = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
                        DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
                    if (FAILED(hr2)) return nullptr;
                }
                const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
                const auto* img = src.GetImage(0, 0, 0);

                D3D11_TEXTURE2D_DESC texDesc = {};
                texDesc.Width = static_cast<UINT>(img->width);
                texDesc.Height = static_cast<UINT>(img->height);
                texDesc.MipLevels = 1;
                texDesc.ArraySize = 1;
                texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                texDesc.SampleDesc.Count = 1;
                texDesc.Usage = D3D11_USAGE_DEFAULT;
                texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                D3D11_SUBRESOURCE_DATA initData2 = {};
                initData2.pSysMem = img->pixels;
                initData2.SysMemPitch = static_cast<UINT>(img->rowPitch);

                ComPtr<ID3D11Texture2D> tex;
                hr2 = m_deviceResources->GetD3DDevice()->CreateTexture2D(&texDesc, &initData2, tex.GetAddressOf());
                if (FAILED(hr2)) return nullptr;

                ComPtr<ID3D11ShaderResourceView> srv;
                hr2 = m_deviceResources->GetD3DDevice()->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
                if (FAILED(hr2)) return nullptr;
                return srv;
            };

            m_obeliskRedFlagSRV  = loadFlagDDS(L"GW.EXE_0x48C8D86F.dds");
            m_obeliskBlueFlagSRV = loadFlagDDS(L"GW.EXE_0x901869A3.dds");

            // Default to red flag texture
            if (m_obeliskRedFlagSRV)
            {
                std::vector<ID3D11ShaderResourceView*> flagTex = { m_obeliskRedFlagSRV.Get() };
                map_renderer->GetMeshManager()->SetTexturesForMesh(m_obeliskBannerMeshId, flagTex, 3);
            }

            map_renderer->SetMeshShouldRender(m_obeliskBannerMeshId, false);

            ObeliskLog(std::format("[Obelisk] Banner quad created (meshId={}) redFlag={} blueFlag={}\n",
                m_obeliskBannerMeshId,
                m_obeliskRedFlagSRV ? "loaded" : "MISSING",
                m_obeliskBlueFlagSRV ? "loaded" : "MISSING").c_str());
        }
    }

    // Parse animation from separate FA1 file (0x21297)
    bool animLoaded = false;
    constexpr uint32_t kObeliskAnimFileHash = 0x21297;
    auto animIt = m_hashIndex->find(static_cast<int>(kObeliskAnimFileHash));
    if (animIt != m_hashIndex->end() && !animIt->second.empty())
    {
        int animMftIndex = animIt->second.at(0);
        ObeliskLog(std::format("[Obelisk] Loading animation from FA1 file hash 0x21297, MFT index {}\n",
            animMftIndex).c_str());

        uint8_t* fileData = m_datManager->read_file(animMftIndex);
        if (fileData)
        {
            size_t fileSize = m_datManager->get_MFT()[animMftIndex].uncompressedSize;
            auto clipOpt = GW::Parsers::ParseAnimationFromFile(fileData, fileSize);
            delete[] fileData;

            if (clipOpt && clipOpt->IsValid())
            {
                auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*clipOpt));
                clip->BuildAnimationGroups();

                const auto& segments = clip->animationSegments;
                ObeliskLog(std::format("[Obelisk] Animation has {} segments\n", segments.size()).c_str());
                for (size_t si = 0; si < segments.size(); si++)
                {
                    ObeliskLog(std::format("[Obelisk]   segment[{}] hash=0x{:X} startTime={} endTime={}\n",
                        si, segments[si].hash, segments[si].startTime, segments[si].endTime).c_str());
                }

                if (segments.size() >= 2)
                {
                    constexpr uint32_t kObeliskAnimHash = 0x35E6AE29;
                    size_t targetSegment = SIZE_MAX;
                    for (size_t i = 0; i < segments.size(); i++)
                    {
                        if (segments[i].hash == kObeliskAnimHash)
                        {
                            targetSegment = i;
                            break;
                        }
                    }
                    if (targetSegment == SIZE_MAX)
                        targetSegment = 1;

                    auto controller = std::make_shared<GW::Animation::AnimationController>();
                    controller->Initialize(clip);
                    controller->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
                    controller->SetSegment(targetSegment);
                    controller->SetLooping(true);
                    controller->SetPlaybackSpeed(100000.0f);
                    controller->Play();

                    const auto& geomModels = modelFile.geometry_chunk.models;
                    size_t boneCount = clip->boneTracks.size();

                    std::vector<std::shared_ptr<AnimatedMeshInstance>> animatedMeshes;
                    std::vector<int> animStaticMeshIds;
                    for (size_t j = 0; j < meshes.size(); j++)
                    {
                        const auto& mesh = meshes[j];

                        // Extract bone data to check if submesh has real skeletal animation
                        AnimationPanelState::SubmeshBoneData boneData;
                        std::vector<uint32_t> vertexBoneGroups;
                        bool hasSkeletal = false;
                        if (j < geomModels.size())
                        {
                            const auto& geomModel = geomModels[j];
                            boneData = AnimationPanelState::ExtractBoneData(
                                geomModel.extra_data, geomModel.u0, geomModel.u1);

                            vertexBoneGroups.reserve(geomModel.vertices.size());
                            for (const auto& mv : geomModel.vertices)
                                vertexBoneGroups.push_back(mv.group);

                            // Only treat as animated if multiple bone groups (real deformation)
                            hasSkeletal = (boneData.groupSizes.size() > 1);
                        }

                        ObeliskLog(std::format("[Obelisk] Submesh {} : u0={} groupSizes={} hasSkeletal={}\n",
                            j,
                            j < geomModels.size() ? geomModels[j].u0 : 0,
                            boneData.groupSizes.size(),
                            hasSkeletal).c_str());

                        if (!hasSkeletal)
                        {
                            animatedMeshes.push_back(nullptr);
                            continue;
                        }

                        if (j < meshIds.size())
                            animStaticMeshIds.push_back(meshIds[j]);

                        auto skinnedVerts = AnimationPanelState::CreateSkinnedVertices(
                            mesh, boneData, vertexBoneGroups, boneCount,
                            clip->hierarchyMode, j);

                        auto animMesh = std::make_shared<AnimatedMeshInstance>(
                            device, skinnedVerts, mesh.indices, static_cast<int>(j));

                        if (j < perMeshTexIds.size())
                        {
                            auto texSRVs = map_renderer->GetTextureManager()->GetTextures(perMeshTexIds[j]);
                            animMesh->SetTextures(texSRVs, 3);
                        }

                        animMesh->SetPerObjectData(perObjectCBs[j]);
                        animatedMeshes.push_back(std::move(animMesh));
                    }

                    MapAnimatedProp prop;
                    prop.controller     = controller;
                    prop.clip           = clip;
                    prop.meshes         = std::move(animatedMeshes);
                    prop.perObjectCBs   = perObjectCBs;
                    prop.staticMeshIds  = animStaticMeshIds;
                    prop.pixelShaderType = pst;

                    prop.submeshVisibility.resize(meshes.size(), true);
                    if (prop.submeshVisibility.size() > 0) prop.submeshVisibility[0] = false;
                    if (prop.submeshVisibility.size() > 1) prop.submeshVisibility[1] = false;

                    map_renderer->AddAnimatedProp(std::move(prop));
                    m_obeliskAnimPropIndex = static_cast<int>(map_renderer->GetAnimatedProps().size()) - 1;
                    animLoaded = true;

                    ObeliskLog(std::format("[Obelisk] Animated prop created (index={}, segment={})\n",
                        m_obeliskAnimPropIndex, targetSegment).c_str());
                }
                else
                {
                    ObeliskLog("[Obelisk] Not enough animation segments\n");
                }
            }
            else
            {
                ObeliskLog("[Obelisk] Animation clip invalid or not found\n");
            }
        }
        else
        {
            ObeliskLog("[Obelisk] Failed to read animation file data\n");
        }
    }
    else
    {
        ObeliskLog("[Obelisk] Animation file hash 0x21297 not found in hash index\n");
    }

    if (!animLoaded)
    {
        ObeliskLog(std::format("[Obelisk] Falling back to static rendering ({} meshes)\n",
            meshIds.size()).c_str());
    }
}

void ReplayWindow::UpdateObeliskFlagStand()
{
    if (m_obeliskAnimPropIndex < 0 || !m_mapRenderer)
        return;

    auto& animProps = m_mapRenderer->GetAnimatedProps();
    if (m_obeliskAnimPropIndex >= static_cast<int>(animProps.size()))
        return;

    auto& ap = animProps[m_obeliskAnimPropIndex];
    if (!ap.active || !ap.controller)
        return;

    StandOwner owner = m_flagTimeline.obelisk.ownerAtTime(m_debugTimeline);

    bool visible = (owner == StandOwner::Neutral);

    if (!ap.controller->IsPlaying())
    {
        ap.controller->SetLooping(true);
        ap.controller->Play();
    }

    // Submesh 0: visible when captured (flag)
    if (ap.submeshVisibility.size() > 0) ap.submeshVisibility[0] = !visible;
    // Submesh 1: always hidden (particles)
    if (ap.submeshVisibility.size() > 1) ap.submeshVisibility[1] = false;

    // Submeshes 2, 3: visible when neutral, hidden when captured
    if (ap.submeshVisibility.size() > 2) ap.submeshVisibility[2] = visible;
    if (ap.submeshVisibility.size() > 3) ap.submeshVisibility[3] = visible;

    // Submesh 4: always visible (the static base)
    if (ap.submeshVisibility.size() > 4) ap.submeshVisibility[4] = true;

    // Toggle static meshes for boneless submeshes (not managed by animated pipeline)
    auto* mr = m_mapRenderer.get();
    for (size_t i = 0; i < m_obeliskStaticMeshIds.size(); i++)
    {
        bool isAnimated = (i < ap.meshes.size() && ap.meshes[i] != nullptr);
        if (!isAnimated)
        {
            bool show = false;
            if (i == 0)       show = !visible;    // show when captured (pole)
            else if (i == 1)  show = false;        // always hidden (particles)
            else if (i == 4)  show = true;         // always visible (base)
            else              show = visible;      // 2,3: visible when neutral
            mr->SetMeshShouldRender(m_obeliskStaticMeshIds[i], show);
        }
    }

    // Toggle the hanging flag banner quad (show when captured) with team texture
    if (m_obeliskBannerMeshId >= 0)
    {
        bool showBanner = !visible;
        mr->SetMeshShouldRender(m_obeliskBannerMeshId, showBanner);

        if (showBanner)
        {
            ID3D11ShaderResourceView* flagSRV = nullptr;
            if (owner == StandOwner::Red && m_obeliskRedFlagSRV)
                flagSRV = m_obeliskRedFlagSRV.Get();
            else if (owner == StandOwner::Blue && m_obeliskBlueFlagSRV)
                flagSRV = m_obeliskBlueFlagSRV.Get();

            if (flagSRV)
            {
                std::vector<ID3D11ShaderResourceView*> flagTex = { flagSRV };
                mr->GetMeshManager()->SetTexturesForMesh(m_obeliskBannerMeshId, flagTex, 3);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Tower Flag Stand — load and render 3D model (all GvG maps)
// ---------------------------------------------------------------------------

void ReplayWindow::SetupTowerFlagStand()
{
    if (m_towerModelLoaded)
        return;
    m_towerModelLoaded = true;

    if (!m_datManager || !m_hashIndex || !m_mapRenderer)
        return;

    if (m_flagTimeline.stand.standAgentId < 0)
        return;

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    constexpr uint32_t kTowerFileHash = 0x13D84;

    auto mit = m_hashIndex->find(static_cast<int>(kTowerFileHash));
    if (mit == m_hashIndex->end() || mit->second.empty())
        return;

    int mftIndex = mit->second.at(0);

    FFNA_ModelFile modelFile;
    try {
        modelFile = m_datManager->parse_ffna_model_file(mftIndex);
    } catch (...) {
        return;
    }
    if (!modelFile.parsed_correctly)
        return;

    const auto& geom = modelFile.geometry_chunk;
    std::vector<Mesh> meshes;
    for (size_t j = 0; j < geom.models.size(); j++)
    {
        AMAT_file amat;
        if (modelFile.textures_parsed_correctly &&
            !modelFile.AMAT_filenames_chunk.texture_filenames.empty())
        {
            int subIdx = geom.models[j].unknown;
            if (!geom.tex_and_vertex_shader_struct.uts0.empty())
                subIdx %= static_cast<int>(geom.tex_and_vertex_shader_struct.uts0.size());
            if (!geom.uts1.empty())
            {
                const auto& uts1 = geom.uts1[subIdx % geom.uts1.size()];
                int amatIdx = ((uts1.some_flags0 >> 8) & 0xFF)
                    % static_cast<int>(modelFile.AMAT_filenames_chunk.texture_filenames.size());
                auto amatFn = modelFile.AMAT_filenames_chunk.texture_filenames[amatIdx];
                auto amatHash = decode_filename(amatFn.id0, amatFn.id1);
                auto aIt = m_hashIndex->find(amatHash);
                if (aIt != m_hashIndex->end())
                    amat = m_datManager->parse_amat_file(aIt->second.at(0));
            }
        }
        Mesh mesh = modelFile.GetMesh(static_cast<int>(j), amat);
        if (mesh.indices.size() % 3 == 0)
            meshes.push_back(mesh);
    }
    if (meshes.empty())
        return;

    auto* map_renderer = m_mapRenderer.get();
    std::vector<int> textureIds;
    if (modelFile.textures_parsed_correctly)
    {
        for (size_t t = 0; t < modelFile.texture_filenames_chunk.texture_filenames.size(); t++)
        {
            auto tf = modelFile.texture_filenames_chunk.texture_filenames[t];
            auto decoded = decode_filename(tf.id0, tf.id1);
            int texId = map_renderer->GetTextureManager()->GetTextureIdByHash(decoded);
            if (texId >= 0) { textureIds.push_back(texId); continue; }
            auto tit = m_hashIndex->find(decoded);
            if (tit != m_hashIndex->end())
            {
                DatTexture dt = m_datManager->parse_ffna_texture_file(tit->second.at(0));
                if (dt.width > 0 && dt.height > 0)
                {
                    map_renderer->GetTextureManager()->CreateTextureFromRGBA(
                        dt.width, dt.height, dt.rgba_data.data(), &texId, decoded);
                }
                textureIds.push_back(texId);
            }
        }
    }

    std::vector<std::vector<int>> perMeshTexIds(meshes.size());
    for (size_t k = 0; k < meshes.size(); k++)
    {
        std::vector<uint8_t> remappedIndices;
        for (size_t ti = 0; ti < meshes[k].tex_indices.size(); ti++)
        {
            int idx = std::min(static_cast<int>(meshes[k].tex_indices[ti]),
                               static_cast<int>(textureIds.size()) - 1);
            if (idx >= 0 && idx < static_cast<int>(textureIds.size()))
            {
                perMeshTexIds[k].push_back(textureIds[idx]);
                remappedIndices.push_back(static_cast<uint8_t>(ti));
            }
        }
        meshes[k].tex_indices = remappedIndices;
    }

    float tx = m_flagTimeline.stand.standX;
    float ty = m_flagTimeline.stand.standY;
    float tz = m_flagTimeline.stand.standZ;
    XMFLOAT3 pos = ApplyMapTransformToPos(tx, ty, tz, m_replayCtx.mapTransform);

    XMMATRIX worldMat = XMMatrixTranslation(pos.x, pos.y, pos.z);

    std::vector<PerObjectCB> perObjectCBs(meshes.size());
    for (size_t j = 0; j < meshes.size(); j++)
    {
        XMStoreFloat4x4(&perObjectCBs[j].world, worldMat);

        auto& mesh = meshes[j];
        if (mesh.uv_coord_indices.size() == mesh.tex_indices.size() &&
            mesh.uv_coord_indices.size() < MAX_NUM_TEX_INDICES &&
            modelFile.textures_parsed_correctly)
        {
            perObjectCBs[j].num_uv_texture_pairs = static_cast<uint32_t>(mesh.uv_coord_indices.size());
            for (size_t k = 0; k < mesh.uv_coord_indices.size(); k++)
            {
                perObjectCBs[j].uv_indices[k / 4][k % 4]       = static_cast<uint32_t>(mesh.uv_coord_indices[k]);
                perObjectCBs[j].texture_indices[k / 4][k % 4]   = static_cast<uint32_t>(mesh.tex_indices[k]);
                uint32_t bf = static_cast<uint32_t>(mesh.blend_flags[k]);
                if (j == 0) bf = 0;
                perObjectCBs[j].blend_flags[k / 4][k % 4]       = bf;
                perObjectCBs[j].texture_types[k / 4][k % 4]     = static_cast<uint32_t>(mesh.texture_types[k]);
            }
        }
    }

    auto pst = geom.unknown_tex_stuff1.empty() ? PixelShaderType::OldModel : PixelShaderType::NewModel;
    auto meshIds = map_renderer->AddProp(meshes, perObjectCBs, 0xFFFF0003u, pst);

    if (modelFile.textures_parsed_correctly)
    {
        for (size_t l = 0; l < meshIds.size() && l < perMeshTexIds.size(); l++)
        {
            auto texVec = map_renderer->GetTextureManager()->GetTextures(perMeshTexIds[l]);
            map_renderer->GetMeshManager()->SetTexturesForMesh(meshIds[l], texVec, 3);
        }
    }

    m_towerStaticMeshIds = meshIds;

    // Initial visibility: submesh 0 hidden (pole, shown when capped), 1 shown, 2 hidden, 3+4 shown (neutral)
    if (meshIds.size() > 0) map_renderer->SetMeshShouldRender(meshIds[0], false);
    if (meshIds.size() > 2) map_renderer->SetMeshShouldRender(meshIds[2], false);

    // Banner quad on the pole (submesh 0)
    if (modelFile.textures_parsed_correctly && meshes.size() > 0 && !meshes[0].vertices.empty())
    {
        float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
        float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
        for (const auto& v : meshes[0].vertices)
        {
            minX = std::min(minX, v.position.x); maxX = std::max(maxX, v.position.x);
            minY = std::min(minY, v.position.y); maxY = std::max(maxY, v.position.y);
            minZ = std::min(minZ, v.position.z); maxZ = std::max(maxZ, v.position.z);
        }

        Mesh bannerMesh;
        float bannerHeight = 55.0f;

        XMFLOAT3 poleStart = { (minX + maxX) * 0.5f, minY, minZ };
        XMFLOAT3 poleEnd   = { (minX + maxX) * 0.5f, maxY, maxZ };

        float t0 = 0.45f;
        float t1 = 0.95f;

        XMFLOAT3 topLeft = {
            poleStart.x + t0 * (poleEnd.x - poleStart.x),
            poleStart.y + t0 * (poleEnd.y - poleStart.y),
            poleStart.z + t0 * (poleEnd.z - poleStart.z)
        };
        XMFLOAT3 topRight = {
            poleStart.x + t1 * (poleEnd.x - poleStart.x),
            poleStart.y + t1 * (poleEnd.y - poleStart.y),
            poleStart.z + t1 * (poleEnd.z - poleStart.z)
        };

        XMFLOAT3 normal = { 1.0f, 0.0f, 0.0f };

        GWVertex v0, v1, v2, v3;
        v0.position = topLeft;
        v0.normal = normal;
        v0.tex_coord0 = { 0.0f, 0.0f };

        v1.position = topRight;
        v1.normal = normal;
        v1.tex_coord0 = { 1.0f, 0.0f };

        v2.position = { topLeft.x, topLeft.y - bannerHeight, topLeft.z };
        v2.normal = normal;
        v2.tex_coord0 = { 0.0f, 1.0f };

        v3.position = { topRight.x, topRight.y - bannerHeight, topRight.z };
        v3.normal = normal;
        v3.tex_coord0 = { 1.0f, 1.0f };

        bannerMesh.vertices = { v0, v1, v2, v3 };
        bannerMesh.indices = { 0, 1, 2, 1, 3, 2 };
        bannerMesh.should_cull = false;
        bannerMesh.blend_state = BlendState::Opaque;
        bannerMesh.num_textures = 1;
        bannerMesh.uv_coord_indices = { 0 };
        bannerMesh.tex_indices = { 0 };
        bannerMesh.blend_flags = { 0 };
        bannerMesh.texture_types = { 0 };

        PerObjectCB bannerCB;
        XMStoreFloat4x4(&bannerCB.world, worldMat);
        bannerCB.num_uv_texture_pairs = 1;

        std::vector<Mesh> bannerMeshes = { bannerMesh };
        std::vector<PerObjectCB> bannerCBs = { bannerCB };
        auto bannerIds = map_renderer->AddProp(bannerMeshes, bannerCBs, 0xFFFF0004u, pst);

        if (!bannerIds.empty())
        {
            m_towerBannerMeshId = bannerIds[0];

            auto loadFlagDDS = [&](const wchar_t* filename) -> ComPtr<ID3D11ShaderResourceView>
            {
                wchar_t exePath[MAX_PATH];
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                auto baseDir = std::filesystem::path(exePath).parent_path();
                for (int up = 0; up < 5; up++)
                {
                    if (std::filesystem::exists(baseDir / L"Textures" / L"Others_UI"))
                    { baseDir = baseDir / L"Textures" / L"Others_UI"; break; }
                    if (!baseDir.has_parent_path() || baseDir == baseDir.parent_path()) break;
                    baseDir = baseDir.parent_path();
                }
                auto fullPath = baseDir / filename;
                if (!std::filesystem::exists(fullPath)) return nullptr;

                DirectX::ScratchImage image;
                HRESULT hr = DirectX::LoadFromDDSFile(fullPath.c_str(),
                    DirectX::DDS_FLAGS_NONE, nullptr, image);
                if (FAILED(hr)) return nullptr;

                ComPtr<ID3D11ShaderResourceView> srv;
                hr = DirectX::CreateShaderResourceView(device,
                    image.GetImages(), image.GetImageCount(),
                    image.GetMetadata(), srv.GetAddressOf());
                if (FAILED(hr)) return nullptr;
                return srv;
            };

            m_towerRedFlagSRV  = loadFlagDDS(L"GW.EXE_0x48C8D86F.dds");
            m_towerBlueFlagSRV = loadFlagDDS(L"GW.EXE_0x901869A3.dds");

            if (m_towerRedFlagSRV)
            {
                std::vector<ID3D11ShaderResourceView*> flagTex = { m_towerRedFlagSRV.Get() };
                map_renderer->GetMeshManager()->SetTexturesForMesh(m_towerBannerMeshId, flagTex, 3);
            }

            map_renderer->SetMeshShouldRender(m_towerBannerMeshId, false);
        }
    }

    // Animation from FA1 file (0x21297), segment 1
    bool animLoaded = false;
    constexpr uint32_t kTowerAnimFileHash = 0x21297;
    auto animIt = m_hashIndex->find(static_cast<int>(kTowerAnimFileHash));
    if (animIt != m_hashIndex->end() && !animIt->second.empty())
    {
        int animMftIndex = animIt->second.at(0);

        uint8_t* fileData = m_datManager->read_file(animMftIndex);
        if (fileData)
        {
            size_t fileSize = m_datManager->get_MFT()[animMftIndex].uncompressedSize;
            auto clipOpt = GW::Parsers::ParseAnimationFromFile(fileData, fileSize);
            delete[] fileData;

            if (clipOpt && clipOpt->IsValid())
            {
                auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*clipOpt));
                clip->BuildAnimationGroups();

                const auto& segments = clip->animationSegments;
                if (segments.size() >= 2)
                {
                    constexpr uint32_t kTowerAnimSegmentHash = 0x36F05E31;
                    size_t targetSegment = SIZE_MAX;
                    for (size_t i = 0; i < segments.size(); i++)
                    {
                        if (segments[i].hash == kTowerAnimSegmentHash)
                        {
                            targetSegment = i;
                            break;
                        }
                    }
                    if (targetSegment == SIZE_MAX)
                        targetSegment = 1;

                    auto controller = std::make_shared<GW::Animation::AnimationController>();
                    controller->Initialize(clip);
                    controller->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
                    controller->SetSegment(targetSegment);
                    controller->SetLooping(true);
                    controller->SetPlaybackSpeed(100000.0f);
                    controller->Play();

                    const auto& geomModels = modelFile.geometry_chunk.models;
                    size_t boneCount = clip->boneTracks.size();

                    std::vector<std::shared_ptr<AnimatedMeshInstance>> animatedMeshes;
                    std::vector<int> animStaticMeshIds;
                    for (size_t j = 0; j < meshes.size(); j++)
                    {
                        // Only submeshes 3 and 4 are animated
                        if (j != 3 && j != 4)
                        {
                            animatedMeshes.push_back(nullptr);
                            continue;
                        }

                        const auto& mesh = meshes[j];

                        AnimationPanelState::SubmeshBoneData boneData;
                        std::vector<uint32_t> vertexBoneGroups;
                        bool hasSkeletal = false;
                        if (j < geomModels.size())
                        {
                            const auto& geomModel = geomModels[j];
                            boneData = AnimationPanelState::ExtractBoneData(
                                geomModel.extra_data, geomModel.u0, geomModel.u1);

                            vertexBoneGroups.reserve(geomModel.vertices.size());
                            for (const auto& mv : geomModel.vertices)
                                vertexBoneGroups.push_back(mv.group);

                            hasSkeletal = (boneData.groupSizes.size() > 1);
                        }

                        if (!hasSkeletal)
                        {
                            animatedMeshes.push_back(nullptr);
                            continue;
                        }

                        if (j < meshIds.size())
                            animStaticMeshIds.push_back(meshIds[j]);

                        auto skinnedVerts = AnimationPanelState::CreateSkinnedVertices(
                            mesh, boneData, vertexBoneGroups, boneCount,
                            clip->hierarchyMode, j);

                        auto animMesh = std::make_shared<AnimatedMeshInstance>(
                            device, skinnedVerts, mesh.indices, static_cast<int>(j));

                        if (j < perMeshTexIds.size())
                        {
                            auto texSRVs = map_renderer->GetTextureManager()->GetTextures(perMeshTexIds[j]);
                            animMesh->SetTextures(texSRVs, 3);
                        }

                        animMesh->SetPerObjectData(perObjectCBs[j]);
                        animatedMeshes.push_back(std::move(animMesh));
                    }

                    MapAnimatedProp prop;
                    prop.controller     = controller;
                    prop.clip           = clip;
                    prop.meshes         = std::move(animatedMeshes);
                    prop.perObjectCBs   = perObjectCBs;
                    prop.staticMeshIds  = animStaticMeshIds;
                    prop.pixelShaderType = pst;

                    prop.submeshVisibility.resize(meshes.size(), true);
                    // Initial: submesh 0 hidden (pole), 1 shown, 2 hidden, 3+4 shown (neutral)
                    if (prop.submeshVisibility.size() > 0) prop.submeshVisibility[0] = false;
                    if (prop.submeshVisibility.size() > 2) prop.submeshVisibility[2] = false;

                    map_renderer->AddAnimatedProp(std::move(prop));
                    m_towerAnimPropIndex = static_cast<int>(map_renderer->GetAnimatedProps().size()) - 1;
                    animLoaded = true;
                }
            }
        }
    }
}

void ReplayWindow::UpdateTowerFlagStand()
{
    if (m_towerAnimPropIndex < 0 || !m_mapRenderer)
        return;

    auto& animProps = m_mapRenderer->GetAnimatedProps();
    if (m_towerAnimPropIndex >= static_cast<int>(animProps.size()))
        return;

    auto& ap = animProps[m_towerAnimPropIndex];
    if (!ap.active || !ap.controller)
        return;

    StandOwner owner = m_flagTimeline.stand.ownerAtTime(m_debugTimeline);

    bool neutral = (owner == StandOwner::Neutral);

    if (!ap.controller->IsPlaying())
    {
        ap.controller->SetLooping(true);
        ap.controller->Play();
    }

    // Submesh 0 (pole): show when capped
    if (ap.submeshVisibility.size() > 0) ap.submeshVisibility[0] = !neutral;
    // Submesh 1: always show
    if (ap.submeshVisibility.size() > 1) ap.submeshVisibility[1] = true;
    // Submesh 2: always hide
    if (ap.submeshVisibility.size() > 2) ap.submeshVisibility[2] = false;
    // Submeshes 3, 4: show when neutral (animated parts)
    if (ap.submeshVisibility.size() > 3) ap.submeshVisibility[3] = neutral;
    if (ap.submeshVisibility.size() > 4) ap.submeshVisibility[4] = neutral;

    auto* mr = m_mapRenderer.get();
    for (size_t i = 0; i < m_towerStaticMeshIds.size(); i++)
    {
        bool isAnimated = (i < ap.meshes.size() && ap.meshes[i] != nullptr);
        if (!isAnimated)
        {
            bool show = false;
            if (i == 0)       show = !neutral;   // pole: show when capped
            else if (i == 1)  show = true;       // always show
            else if (i == 2)  show = false;      // always hide
            else              show = neutral;    // 3, 4: show when neutral
            mr->SetMeshShouldRender(m_towerStaticMeshIds[i], show);
        }
    }

    // Banner: show when capped, set team texture
    if (m_towerBannerMeshId >= 0)
    {
        bool showBanner = !neutral;
        mr->SetMeshShouldRender(m_towerBannerMeshId, showBanner);

        if (showBanner)
        {
            ID3D11ShaderResourceView* flagSRV = nullptr;
            if (owner == StandOwner::Red && m_towerRedFlagSRV)
                flagSRV = m_towerRedFlagSRV.Get();
            else if (owner == StandOwner::Blue && m_towerBlueFlagSRV)
                flagSRV = m_towerBlueFlagSRV.Get();

            if (flagSRV)
            {
                std::vector<ID3D11ShaderResourceView*> flagTex = { flagSRV };
                mr->GetMeshManager()->SetTexturesForMesh(m_towerBannerMeshId, flagTex, 3);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Gate Lock — load and render animated lever models (Isle of Meditation)
// ---------------------------------------------------------------------------

void ReplayWindow::SetupGateLockProps()
{
    if (m_gateLockModelsLoaded)
        return;
    m_gateLockModelsLoaded = true;

    if (!m_datManager || !m_hashIndex || !m_mapRenderer)
        return;

    if (m_replayCtx.datMapId != 0x28784)
        return;

    if (m_replayCtx.agents.empty())
        return;

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    struct GateLockPos { float x, y, z; };
    std::vector<GateLockPos> positions;
    for (auto& [aid, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;
        uint32_t gid = ard.snapshots[0].gadget_id;
        if (gid != 4721 && gid != 4722) continue;
        positions.push_back({ ard.snapshots[0].x, ard.snapshots[0].y, ard.snapshots[0].z });
    }
    if (positions.empty())
        return;

    constexpr uint32_t kGateLockModelHash = 0x2AD09;
    auto mit = m_hashIndex->find(static_cast<int>(kGateLockModelHash));
    if (mit == m_hashIndex->end() || mit->second.empty())
        return;

    int mftIndex = mit->second.at(0);

    FFNA_ModelFile modelFile;
    try {
        modelFile = m_datManager->parse_ffna_model_file(mftIndex);
    } catch (...) {
        return;
    }
    if (!modelFile.parsed_correctly)
        return;

    const auto& geom = modelFile.geometry_chunk;
    std::vector<Mesh> meshes;
    for (size_t j = 0; j < geom.models.size(); j++)
    {
        AMAT_file amat;
        if (modelFile.textures_parsed_correctly &&
            !modelFile.AMAT_filenames_chunk.texture_filenames.empty())
        {
            int subIdx = geom.models[j].unknown;
            if (!geom.tex_and_vertex_shader_struct.uts0.empty())
                subIdx %= static_cast<int>(geom.tex_and_vertex_shader_struct.uts0.size());
            if (!geom.uts1.empty())
            {
                const auto& uts1 = geom.uts1[subIdx % geom.uts1.size()];
                int amatIdx = ((uts1.some_flags0 >> 8) & 0xFF)
                    % static_cast<int>(modelFile.AMAT_filenames_chunk.texture_filenames.size());
                auto amatFn = modelFile.AMAT_filenames_chunk.texture_filenames[amatIdx];
                auto amatHash = decode_filename(amatFn.id0, amatFn.id1);
                auto aIt = m_hashIndex->find(amatHash);
                if (aIt != m_hashIndex->end())
                    amat = m_datManager->parse_amat_file(aIt->second.at(0));
            }
        }
        Mesh mesh = modelFile.GetMesh(static_cast<int>(j), amat);
        if (mesh.indices.size() % 3 == 0)
            meshes.push_back(mesh);
    }
    if (meshes.empty())
        return;

    auto* map_renderer = m_mapRenderer.get();
    std::vector<int> textureIds;
    if (modelFile.textures_parsed_correctly)
    {
        for (size_t t = 0; t < modelFile.texture_filenames_chunk.texture_filenames.size(); t++)
        {
            auto tf = modelFile.texture_filenames_chunk.texture_filenames[t];
            auto decoded = decode_filename(tf.id0, tf.id1);
            int texId = map_renderer->GetTextureManager()->GetTextureIdByHash(decoded);
            if (texId >= 0) { textureIds.push_back(texId); continue; }
            auto tit = m_hashIndex->find(decoded);
            if (tit != m_hashIndex->end())
            {
                DatTexture dt = m_datManager->parse_ffna_texture_file(tit->second.at(0));
                if (dt.width > 0 && dt.height > 0)
                {
                    map_renderer->GetTextureManager()->CreateTextureFromRGBA(
                        dt.width, dt.height, dt.rgba_data.data(), &texId, decoded);
                }
                textureIds.push_back(texId);
            }
        }
    }

    std::vector<std::vector<int>> perMeshTexIds(meshes.size());
    for (size_t k = 0; k < meshes.size(); k++)
    {
        std::vector<uint8_t> remappedIndices;
        for (size_t ti = 0; ti < meshes[k].tex_indices.size(); ti++)
        {
            int idx = std::min(static_cast<int>(meshes[k].tex_indices[ti]),
                               static_cast<int>(textureIds.size()) - 1);
            if (idx >= 0 && idx < static_cast<int>(textureIds.size()))
            {
                perMeshTexIds[k].push_back(textureIds[idx]);
                remappedIndices.push_back(static_cast<uint8_t>(ti));
            }
        }
        meshes[k].tex_indices = remappedIndices;
    }

    auto pst = geom.unknown_tex_stuff1.empty() ? PixelShaderType::OldModel : PixelShaderType::NewModel;

    constexpr uint32_t kGateLockAnimHash = 0x7079;
    auto animIt = m_hashIndex->find(static_cast<int>(kGateLockAnimHash));
    if (animIt == m_hashIndex->end() || animIt->second.empty())
        return;

    int animMftIndex = animIt->second.at(0);
    uint8_t* animData = m_datManager->read_file(animMftIndex);
    if (!animData)
        return;

    size_t animSize = m_datManager->get_MFT()[animMftIndex].uncompressedSize;
    auto clipOpt = GW::Parsers::ParseAnimationFromFile(animData, animSize);
    delete[] animData;

    if (!clipOpt || !clipOpt->IsValid())
        return;

    auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*clipOpt));
    clip->BuildAnimationGroups();

    const auto& segments = clip->animationSegments;
    if (segments.size() < 2)
        return;

    size_t openSeg = SIZE_MAX, closeSeg = SIZE_MAX;
    for (size_t s = 0; s < segments.size(); s++)
    {
        if (segments[s].hash == 0x35E6AE29) openSeg = s;
        if (segments[s].hash == 0x36F05E31) closeSeg = s;
    }
    if (openSeg == SIZE_MAX) openSeg = 2;
    if (closeSeg == SIZE_MAX) closeSeg = 3;

    const auto& geomModels = modelFile.geometry_chunk.models;
    size_t boneCount = clip->boneTracks.size();

    for (const auto& pos : positions)
    {
        XMFLOAT3 renderPos = ApplyMapTransformToPos(pos.x, pos.y, pos.z, m_replayCtx.mapTransform);
        XMMATRIX worldMat = XMMatrixTranslation(renderPos.x, renderPos.y, renderPos.z);

        std::vector<PerObjectCB> perObjectCBs(meshes.size());
        for (size_t j = 0; j < meshes.size(); j++)
        {
            XMStoreFloat4x4(&perObjectCBs[j].world, worldMat);
            auto& mesh = meshes[j];
            if (mesh.uv_coord_indices.size() == mesh.tex_indices.size() &&
                mesh.uv_coord_indices.size() < MAX_NUM_TEX_INDICES &&
                modelFile.textures_parsed_correctly)
            {
                perObjectCBs[j].num_uv_texture_pairs = static_cast<uint32_t>(mesh.uv_coord_indices.size());
                for (size_t k = 0; k < mesh.uv_coord_indices.size(); k++)
                {
                    perObjectCBs[j].uv_indices[k / 4][k % 4]     = static_cast<uint32_t>(mesh.uv_coord_indices[k]);
                    perObjectCBs[j].texture_indices[k / 4][k % 4] = static_cast<uint32_t>(mesh.tex_indices[k]);
                    perObjectCBs[j].blend_flags[k / 4][k % 4]     = static_cast<uint32_t>(mesh.blend_flags[k]);
                    perObjectCBs[j].texture_types[k / 4][k % 4]   = static_cast<uint32_t>(mesh.texture_types[k]);
                }
            }
        }

        auto meshIds = map_renderer->AddProp(meshes, perObjectCBs, 0xFFFF0005u, pst);

        if (modelFile.textures_parsed_correctly)
        {
            for (size_t l = 0; l < meshIds.size() && l < perMeshTexIds.size(); l++)
            {
                auto texVec = map_renderer->GetTextureManager()->GetTextures(perMeshTexIds[l]);
                map_renderer->GetMeshManager()->SetTexturesForMesh(meshIds[l], texVec, 3);
            }
        }

        auto controller = std::make_shared<GW::Animation::AnimationController>();
        controller->Initialize(clip);
        controller->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
        controller->SetLooping(false);

        std::vector<std::shared_ptr<AnimatedMeshInstance>> animatedMeshes;
        std::vector<int> animStaticMeshIds;
        for (size_t j = 0; j < meshes.size(); j++)
        {
            // Only submesh 0 is animated (lever on bone 0); submesh 1 stays static (base)
            if (j != 0)
            {
                animatedMeshes.push_back(nullptr);
                continue;
            }

            const auto& mesh = meshes[j];
            AnimationPanelState::SubmeshBoneData boneData;
            std::vector<uint32_t> vertexBoneGroups;
            if (j < geomModels.size())
            {
                const auto& geomModel = geomModels[j];
                boneData = AnimationPanelState::ExtractBoneData(
                    geomModel.extra_data, geomModel.u0, geomModel.u1);
                vertexBoneGroups.reserve(geomModel.vertices.size());
                for (const auto& mv : geomModel.vertices)
                    vertexBoneGroups.push_back(mv.group);
            }

            if (j < meshIds.size())
                animStaticMeshIds.push_back(meshIds[j]);

            auto skinnedVerts = AnimationPanelState::CreateSkinnedVertices(
                mesh, boneData, vertexBoneGroups, boneCount,
                clip->hierarchyMode, j);

            // Lock bone 1 vertices to identity so only bone 0 (lever) animates
            for (auto& sv : skinnedVerts)
            {
                if (sv.boneIndices[0] == 1)
                    sv.SetSingleBone(static_cast<uint32_t>(boneCount));
            }

            auto animMesh = std::make_shared<AnimatedMeshInstance>(
                device, skinnedVerts, mesh.indices, static_cast<int>(j));

            if (j < perMeshTexIds.size())
            {
                auto texSRVs = map_renderer->GetTextureManager()->GetTextures(perMeshTexIds[j]);
                animMesh->SetTextures(texSRVs, 3);
            }

            animMesh->SetPerObjectData(perObjectCBs[j]);
            animatedMeshes.push_back(std::move(animMesh));
        }

        MapAnimatedProp prop;
        prop.controller      = controller;
        prop.clip            = clip;
        prop.meshes          = std::move(animatedMeshes);
        prop.perObjectCBs    = perObjectCBs;
        prop.staticMeshIds   = animStaticMeshIds;
        prop.pixelShaderType = pst;
        prop.doorType        = 2;
        prop.openSegmentIndex  = openSeg;
        prop.closeSegmentIndex = closeSeg;

        controller->SetSegment(closeSeg);
        controller->SetTime(static_cast<float>(segments[closeSeg].endTime));
        controller->Pause();

        map_renderer->AddAnimatedProp(std::move(prop));
        m_doorAnimPropCount++;
    }
}

// ---------------------------------------------------------------------------
// Gate Lock — render static lever models (Imperial Isle)
// ---------------------------------------------------------------------------

void ReplayWindow::SetupImperialGateLockProps()
{
    if (m_imperialGateLockLoaded)
        return;
    m_imperialGateLockLoaded = true;

    if (!m_datManager || !m_hashIndex || !m_mapRenderer)
        return;

    if (m_replayCtx.datMapId != 0x28736)
        return;

    if (m_replayCtx.agents.empty())
        return;

    auto GetGateLockRotationOffset = [](uint32_t gadgetId) -> float {
        constexpr float k90 = XM_PIDIV2;
        switch (gadgetId) {
        case 4645: return k90;
        case 4646: return k90;
        case 4647: return k90;
        case 4648: return 0.f;
        case 4649: return k90;
        case 4650: return k90;
        case 4651: return k90;
        case 4652: return 0.f;
        default:   return 0.f;
        }
    };

    struct GateLockPos { float x, y, z, rotation; };
    std::vector<GateLockPos> positions;
    for (auto& [aid, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;
        uint32_t gid = ard.snapshots[0].gadget_id;
        if (gid < 4645 || gid > 4652) continue;
        float rot = ard.snapshots[0].rotation + GetGateLockRotationOffset(gid);
        positions.push_back({ ard.snapshots[0].x, ard.snapshots[0].y,
                              ard.snapshots[0].z, rot });
    }
    if (positions.empty())
        return;

    constexpr uint32_t kModelHash = 0x2AD08;
    auto mit = m_hashIndex->find(static_cast<int>(kModelHash));
    if (mit == m_hashIndex->end() || mit->second.empty())
        return;

    int mftIndex = mit->second.at(0);

    FFNA_ModelFile modelFile;
    try {
        modelFile = m_datManager->parse_ffna_model_file(mftIndex);
    } catch (...) {
        return;
    }
    if (!modelFile.parsed_correctly)
        return;

    const auto& geom = modelFile.geometry_chunk;
    std::vector<Mesh> meshes;
    for (size_t j = 0; j < geom.models.size(); j++)
    {
        AMAT_file amat;
        if (modelFile.textures_parsed_correctly &&
            !modelFile.AMAT_filenames_chunk.texture_filenames.empty())
        {
            int subIdx = geom.models[j].unknown;
            if (!geom.tex_and_vertex_shader_struct.uts0.empty())
                subIdx %= static_cast<int>(geom.tex_and_vertex_shader_struct.uts0.size());
            if (!geom.uts1.empty())
            {
                const auto& uts1 = geom.uts1[subIdx % geom.uts1.size()];
                int amatIdx = ((uts1.some_flags0 >> 8) & 0xFF)
                    % static_cast<int>(modelFile.AMAT_filenames_chunk.texture_filenames.size());
                auto amatFn = modelFile.AMAT_filenames_chunk.texture_filenames[amatIdx];
                auto amatHash = decode_filename(amatFn.id0, amatFn.id1);
                auto aIt = m_hashIndex->find(amatHash);
                if (aIt != m_hashIndex->end())
                    amat = m_datManager->parse_amat_file(aIt->second.at(0));
            }
        }
        Mesh mesh = modelFile.GetMesh(static_cast<int>(j), amat);
        if (mesh.indices.size() % 3 == 0)
            meshes.push_back(mesh);
    }
    if (meshes.empty())
        return;

    auto* map_renderer = m_mapRenderer.get();
    std::vector<int> textureIds;
    if (modelFile.textures_parsed_correctly)
    {
        for (size_t t = 0; t < modelFile.texture_filenames_chunk.texture_filenames.size(); t++)
        {
            auto tf = modelFile.texture_filenames_chunk.texture_filenames[t];
            auto decoded = decode_filename(tf.id0, tf.id1);
            int texId = map_renderer->GetTextureManager()->GetTextureIdByHash(decoded);
            if (texId >= 0) { textureIds.push_back(texId); continue; }
            auto tit = m_hashIndex->find(decoded);
            if (tit != m_hashIndex->end())
            {
                DatTexture dt = m_datManager->parse_ffna_texture_file(tit->second.at(0));
                if (dt.width > 0 && dt.height > 0)
                {
                    map_renderer->GetTextureManager()->CreateTextureFromRGBA(
                        dt.width, dt.height, dt.rgba_data.data(), &texId, decoded);
                }
                textureIds.push_back(texId);
            }
        }
    }

    std::vector<std::vector<int>> perMeshTexIds(meshes.size());
    for (size_t k = 0; k < meshes.size(); k++)
    {
        std::vector<uint8_t> remappedIndices;
        for (size_t ti = 0; ti < meshes[k].tex_indices.size(); ti++)
        {
            int idx = std::min(static_cast<int>(meshes[k].tex_indices[ti]),
                               static_cast<int>(textureIds.size()) - 1);
            if (idx >= 0 && idx < static_cast<int>(textureIds.size()))
            {
                perMeshTexIds[k].push_back(textureIds[idx]);
                remappedIndices.push_back(static_cast<uint8_t>(ti));
            }
        }
        meshes[k].tex_indices = remappedIndices;
    }

    auto pst = geom.unknown_tex_stuff1.empty() ? PixelShaderType::OldModel : PixelShaderType::NewModel;

    for (const auto& pos : positions)
    {
        XMFLOAT3 renderPos = ApplyMapTransformToPos(pos.x, pos.y, pos.z, m_replayCtx.mapTransform);
        XMMATRIX rotMat = XMMatrixRotationY(pos.rotation);
        XMMATRIX transMat = XMMatrixTranslation(renderPos.x, renderPos.y, renderPos.z);
        XMMATRIX worldMat = rotMat * transMat;

        std::vector<PerObjectCB> perObjectCBs(meshes.size());
        for (size_t j = 0; j < meshes.size(); j++)
        {
            XMStoreFloat4x4(&perObjectCBs[j].world, worldMat);
            auto& mesh = meshes[j];
            if (mesh.uv_coord_indices.size() == mesh.tex_indices.size() &&
                mesh.uv_coord_indices.size() < MAX_NUM_TEX_INDICES &&
                modelFile.textures_parsed_correctly)
            {
                perObjectCBs[j].num_uv_texture_pairs = static_cast<uint32_t>(mesh.uv_coord_indices.size());
                for (size_t k = 0; k < mesh.uv_coord_indices.size(); k++)
                {
                    perObjectCBs[j].uv_indices[k / 4][k % 4]     = static_cast<uint32_t>(mesh.uv_coord_indices[k]);
                    perObjectCBs[j].texture_indices[k / 4][k % 4] = static_cast<uint32_t>(mesh.tex_indices[k]);
                    perObjectCBs[j].blend_flags[k / 4][k % 4]     = static_cast<uint32_t>(mesh.blend_flags[k]);
                    perObjectCBs[j].texture_types[k / 4][k % 4]   = static_cast<uint32_t>(mesh.texture_types[k]);
                }
            }
        }

        auto meshIds = map_renderer->AddProp(meshes, perObjectCBs, 0xFFFF0006u, pst);

        if (modelFile.textures_parsed_correctly)
        {
            for (size_t l = 0; l < meshIds.size() && l < perMeshTexIds.size(); l++)
            {
                auto texVec = map_renderer->GetTextureManager()->GetTextures(perMeshTexIds[l]);
                map_renderer->GetMeshManager()->SetTexturesForMesh(meshIds[l], texVec, 3);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Door animation update (event-driven, per door type)
// ---------------------------------------------------------------------------

static int GetDoorType(uint32_t datMapId, uint32_t objectId)
{
    if (datMapId == 0x28784) // Isle of Meditation
    {
        switch (objectId) {
        case 56526: case 11692: case 12669: case 61318: return 1;
        case 41431: case 1760:  case 54552:             return 2;
        default: return 0;
        }
    }
    if (datMapId == 0x28736) // Imperial Isle
    {
        switch (objectId) {
        case 56526: case 147: case 12669: case 30563: return 3;
        default: return 0;
        }
    }
    return 0;
}

void ReplayWindow::UpdateDoorAnimations()
{
    if (!m_mapRenderer || !m_replayCtx.stocLoaded || m_doorAnimPropCount == 0)
        return;

    const auto& doorEvents = m_replayCtx.stocData.doorEvents;
    if (doorEvents.empty())
        return;

    float curTime = m_debugTimeline;
    float timeDelta = curTime - m_doorLastScanTime;
    bool seeked = (m_doorLastScanTime < 0.f)
               || (timeDelta < 0.f)
               || (timeDelta > 1.0f);
    m_doorLastScanTime = curTime;

    bool prevOpen[5] = { false, m_doorTypeOpen[1], m_doorTypeOpen[2], m_doorTypeOpen[3], m_doorTypeOpen[4] };

    if (seeked)
    {
        m_doorTypeOpen[1] = false;
        m_doorTypeOpen[2] = false;
        m_doorTypeOpen[3] = false;
        m_doorTypeOpen[4] = false;
    }

    m_doorTypeOpen[4] = (curTime >= 1.0f);

    for (const auto& ev : doorEvents)
    {
        if (ev.time > curTime)
            break;

        if (ev.isState)
            continue;
        if (ev.animation_stage != 2)
            continue;

        int dt = GetDoorType(m_replayCtx.datMapId, ev.object_id);
        if (dt == 0)
            continue;

        m_doorTypeOpen[dt] = (ev.status == 1);
    }

    float frameDt = static_cast<float>(m_timer.GetElapsedSeconds());

    auto& animProps = m_mapRenderer->GetAnimatedProps();
    for (auto& ap : animProps)
    {
        if (ap.doorType == 0)
            continue;

        bool isOpen = m_doorTypeOpen[ap.doorType];
        bool stateChanged = seeked || (isOpen != prevOpen[ap.doorType]);

        if (stateChanged)
        {
            size_t targetSeg = isOpen ? ap.openSegmentIndex : ap.closeSegmentIndex;
            const auto& segments = ap.clip->animationSegments;

            if (targetSeg >= segments.size())
            {
                if (!isOpen && ap.openSegmentIndex < segments.size())
                {
                    ap.controller->SetSegment(ap.openSegmentIndex);
                    ap.controller->SetTime(
                        static_cast<float>(segments[ap.openSegmentIndex].startTime));
                    ap.controller->Pause();
                }
                continue;
            }

            if (seeked)
            {
                ap.controller->SetSegment(targetSeg);
                ap.controller->SetTime(static_cast<float>(segments[targetSeg].endTime));
                ap.controller->Pause();
            }
            else
            {
                ap.controller->SetSegment(targetSeg);
                ap.controller->SetLooping(false);
                ap.controller->Play();
            }
        }

        if (m_replayCtx.isPlaying && ap.controller->IsPlaying())
            ap.controller->Update(frameDt * m_replayCtx.playbackSpeed);
    }
}
