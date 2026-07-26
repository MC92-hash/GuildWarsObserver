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
#include <tuple>
#include <map>
#include <cmath>
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
    size_t segmentFallbackIndex,
    uint32_t animFileHash,
    const std::unordered_map<size_t, uint32_t>& submeshBoneOverride)
{
    if (!m_datManager || !m_hashIndex || !m_mapRenderer)
        return;

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    // The animation clip may live in a different file than the geometry
    // (e.g. Druid's Isle vine bridge: geometry 0x29FD, animation 0x21297).
    uint32_t clipFileHash = (animFileHash != 0) ? animFileHash : modelFileHash;
    auto mit = m_hashIndex->find(static_cast<int>(clipFileHash));
    if (mit == m_hashIndex->end() || mit->second.empty())
        return;

    std::optional<GW::Animation::AnimationClip> clipOpt;
    if (animFileHash == 0)
    {
        // Geometry and animation share the file: use the first entry as before.
        int mftIndex = mit->second.at(0);
        uint8_t* fileData = m_datManager->read_file(mftIndex);
        if (!fileData)
            return;
        size_t fileSize = m_datManager->get_MFT()[mftIndex].uncompressedSize;
        clipOpt = GW::Parsers::ParseAnimationFromFile(fileData, fileSize);
        delete[] fileData;
    }
    else
    {
        // Separate animation file: several MFT entries can share this hash. Pick the
        // one whose clip actually contains the requested segment, preferring the one
        // with the most bones (the real rigged clip vs. unrelated stub clips).
        size_t bestBones = 0;
        std::optional<GW::Animation::AnimationClip> bestWithSeg, bestAny;
        for (int idx : mit->second)
        {
            uint8_t* fileData = m_datManager->read_file(idx);
            if (!fileData)
                continue;
            size_t fileSize = m_datManager->get_MFT()[idx].uncompressedSize;
            auto co = GW::Parsers::ParseAnimationFromFile(fileData, fileSize);
            delete[] fileData;
            if (!co || !co->IsValid())
                continue;
            bool hasSeg = false;
            for (const auto& sg : co->animationSegments)
                if (sg.hash == segmentHash) { hasSeg = true; break; }
            if (hasSeg && co->boneTracks.size() >= bestBones)
            {
                bestBones = co->boneTracks.size();
                bestWithSeg = std::move(co);
            }
            else if (!bestWithSeg && !bestAny)
            {
                bestAny = std::move(co);
            }
        }
        clipOpt = bestWithSeg ? std::move(bestWithSeg) : std::move(bestAny);
    }

    if (!clipOpt || !clipOpt->IsValid())
        return;

    auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*clipOpt));
    clip->BuildAnimationGroups();

    const auto& segments = clip->animationSegments;
    // Doors carry a closed+open segment pair, but some ambient looping props
    // (e.g. Druid's Isle 0x1504E) ship a single segment that just loops.
    if (segments.empty())
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
    if (targetSegment >= segments.size())
        targetSegment = 0;

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

        // A submesh can be authored on a static bone even though it belongs to a
        // moving part, in which case it stays behind while the rest of the piece
        // moves. Re-pin it to the bone that actually carries the motion.
        if (auto ov = submeshBoneOverride.find(j); ov != submeshBoneOverride.end())
            for (auto& sv : skinnedVerts)
                sv.SetSingleBone(ov->second);

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
// Uncharted Isle double gates (0x3C163, 0x32F3A, ...).
//
// Structure (from door_debug.log): 6 submeshes, 3 bones.
//   - submesh 0 is the static pillar/frame (must not move)
//   - submeshes 1..5 are the two door leaves, each split ~50/50 between
//     bone 0 (right leaf, the hierarchy root — animates correctly) and
//     bone 1 (left leaf, parented to bone 0 so it compounds the motion and
//     pivots incorrectly).
// The .dat animation only opens one leaf correctly. We drive the second leaf
// from a *mirrored* copy of bone 0: the bone-1 verts are reassigned to a virtual
// mirror slot, and bone 0 is reflected across the door's symmetry plane (the
// midpoint of the two leaf centroids, which is where the leaves meet).
// ---------------------------------------------------------------------------

void ReplayWindow::SetupUnchartedMirrorDoor(
    int propIndex,
    const FFNA_ModelFile& modelFile,
    uint32_t modelFileHash,
    const std::vector<Mesh>& meshes,
    const std::vector<PerObjectCB>& perObjectCBs,
    const std::vector<int>& meshIds,
    const std::vector<std::vector<int>>& perMeshTexIds,
    PixelShaderType pst,
    uint8_t doorType,
    size_t staticSubmesh)
{
    if (!m_datManager || !m_hashIndex || !m_mapRenderer || !m_deviceResources)
        return;

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    auto mit = m_hashIndex->find(static_cast<int>(modelFileHash));
    if (mit == m_hashIndex->end() || mit->second.empty())
        return;

    int mftIndex = mit->second.at(0);
    uint8_t* animFileData = m_datManager->read_file(mftIndex);
    if (!animFileData)
        return;

    size_t animFileSize = m_datManager->get_MFT()[mftIndex].uncompressedSize;
    auto clipOpt = GW::Parsers::ParseAnimationFromFile(animFileData, animFileSize);
    delete[] animFileData;

    if (!clipOpt || !clipOpt->IsValid())
        return;

    auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*clipOpt));
    clip->BuildAnimationGroups();

    const auto& segments = clip->animationSegments;
    if (segments.size() < 2)
        return;

    size_t openSeg = SIZE_MAX;
    for (size_t s = 0; s < segments.size(); s++)
        if (segments[s].hash == 0x303419C9) openSeg = s;
    if (openSeg == SIZE_MAX) openSeg = 2;

    auto controller = std::make_shared<GW::Animation::AnimationController>();
    controller->Initialize(clip);
    controller->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
    controller->SetSegment(openSeg);
    controller->SetLooping(false);
    controller->SetPlaybackSpeed(100000.0f);
    controller->SetTime(static_cast<float>(segments[openSeg].startTime));
    controller->Play();
    controller->Pause();

    const auto& geomModels = modelFile.geometry_chunk.models;
    size_t boneCount = clip->boneTracks.size();

    const size_t       kStaticSubmesh = staticSubmesh;   // pillar / frame (model-specific)
    constexpr uint32_t kDriverBone    = 0;   // correctly-opening leaf (hierarchy root)
    constexpr uint32_t kMirroredBone  = 1;   // leaf to replace with a mirror of bone 0
    const uint32_t staticSlot = static_cast<uint32_t>(boneCount);       // identity
    const uint32_t mirrorSlot = static_cast<uint32_t>(boneCount) + 1;   // mirror of bone 0

    double sumXDriver = 0.0, sumXMirror = 0.0;
    size_t cntDriver = 0, cntMirror = 0;

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

        if (j == kStaticSubmesh)
        {
            for (auto& sv : skinnedVerts)
                sv.SetSingleBone(staticSlot);
        }
        else
        {
            for (auto& sv : skinnedVerts)
            {
                if (sv.boneIndices[0] == kDriverBone)
                {
                    sumXDriver += sv.position.x; cntDriver++;
                }
                else if (sv.boneIndices[0] == kMirroredBone)
                {
                    sumXMirror += sv.position.x; cntMirror++;
                    sv.SetSingleBone(mirrorSlot);
                }
            }
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

    // Symmetry plane = midpoint of the two leaf centroids (per-leaf half-width
    // cancels, so it's independent of leaf size).
    float mirrorPlaneX = 0.f;
    if (cntDriver > 0 && cntMirror > 0)
        mirrorPlaneX = static_cast<float>(
            (sumXDriver / cntDriver + sumXMirror / cntMirror) * 0.5);

    MapAnimatedProp prop;
    prop.controller        = controller;
    prop.clip              = clip;
    prop.meshes            = std::move(animatedMeshes);
    prop.perObjectCBs      = perObjectCBs;
    prop.staticMeshIds     = meshIds;
    prop.pixelShaderType   = pst;
    prop.doorType          = doorType;
    prop.closedAtOpenStart = true;
    prop.openSegmentIndex  = openSeg;
    prop.closeSegmentIndex = openSeg;
    prop.mirrorBonePairs   = {{ static_cast<int>(kDriverBone), static_cast<int>(mirrorSlot) }};
    prop.mirrorPlaneX      = mirrorPlaneX;
    prop.doubleSided       = true;   // mirroring reverses winding

    m_mapRenderer->AddAnimatedProp(std::move(prop));
    m_doorAnimPropCount++;
    OutputDebugStringA(std::format(
        "[DoorAnim] Uncharted Isle prop {} door type {} (hash 0x{:X}) mirror bone {}->{} plane x={:.2f}\n",
        propIndex, static_cast<int>(doorType), modelFileHash,
        kDriverBone, mirrorSlot, mirrorPlaneX).c_str());
}

// ---------------------------------------------------------------------------
// Uncharted Isle horizontal double-slide gate (0x32F0C).
//
// Structure (from door_debug.log): 4 submeshes, 3 bones.
//   - bone 0 (root) slides +X by ~122 over the open segment  -> driver leaf
//   - bone 2 (child of 0) copies bone 0's motion (also +X)    -> opposite leaf,
//                                                                but wrongly moved +X
//   - bone 1 (child of 0) is net-fixed in world               -> opposite-leaf trim
// The gate should open by both leaves sliding apart. We keep the driver bone (0)
// leaf sliding +X and drive everything else from a *mirrored* copy of bone 0, which
// for a pure translation +t produces exactly -t (independent of the mirror plane) and
// preserves winding, so no static bone or double-sided rendering is required.
// ---------------------------------------------------------------------------

void ReplayWindow::SetupUnchartedSlideDoor(
    int propIndex,
    const FFNA_ModelFile& modelFile,
    uint32_t modelFileHash,
    const std::vector<Mesh>& meshes,
    const std::vector<PerObjectCB>& perObjectCBs,
    const std::vector<int>& meshIds,
    const std::vector<std::vector<int>>& perMeshTexIds,
    PixelShaderType pst,
    uint8_t doorType,
    const std::vector<size_t>& staticSubmeshes,
    const std::vector<std::pair<uint32_t, uint32_t>>& mirrorFollowerToDriver)
{
    if (!m_datManager || !m_hashIndex || !m_mapRenderer || !m_deviceResources)
        return;

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    auto mit = m_hashIndex->find(static_cast<int>(modelFileHash));
    if (mit == m_hashIndex->end() || mit->second.empty())
        return;

    int mftIndex = mit->second.at(0);
    uint8_t* animFileData = m_datManager->read_file(mftIndex);
    if (!animFileData)
        return;

    size_t animFileSize = m_datManager->get_MFT()[mftIndex].uncompressedSize;
    auto clipOpt = GW::Parsers::ParseAnimationFromFile(animFileData, animFileSize);
    delete[] animFileData;

    if (!clipOpt || !clipOpt->IsValid())
        return;

    auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*clipOpt));
    clip->BuildAnimationGroups();

    const auto& segments = clip->animationSegments;
    if (segments.size() < 2)
        return;

    size_t openSeg = SIZE_MAX;
    for (size_t s = 0; s < segments.size(); s++)
        if (segments[s].hash == 0x303419C9) openSeg = s;
    if (openSeg == SIZE_MAX) openSeg = segments.size() - 1;

    auto controller = std::make_shared<GW::Animation::AnimationController>();
    controller->Initialize(clip);
    controller->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
    controller->SetSegment(openSeg);
    controller->SetLooping(false);
    controller->SetPlaybackSpeed(100000.0f);
    controller->SetTime(static_cast<float>(segments[openSeg].startTime));
    controller->Play();
    controller->Pause();

    const auto& geomModels = modelFile.geometry_chunk.models;
    size_t boneCount = clip->boneTracks.size();

    // Slot layout appended after the real bones: one static (identity) slot for the
    // frame, then one mirror slot per unique driver bone. Follower bones are re-skinned
    // to their driver's mirror slot; MapRenderer fills that slot with R * driverM * R.
    const uint32_t staticSlot = static_cast<uint32_t>(boneCount);
    std::unordered_map<uint32_t, uint32_t> followerDriver;   // follower bone -> driver bone
    std::unordered_map<uint32_t, uint32_t> driverSlot;       // driver bone   -> mirror slot
    uint32_t nextSlot = staticSlot + 1;
    for (const auto& [follower, driver] : mirrorFollowerToDriver)
    {
        followerDriver[follower] = driver;
        if (!driverSlot.count(driver))
            driverSlot[driver] = nextSlot++;
    }

    auto isStaticSubmesh = [&](size_t j) {
        return std::find(staticSubmeshes.begin(), staticSubmeshes.end(), j) != staticSubmeshes.end();
    };

    std::ofstream dbg("door_debug.log", std::ios::app);
    dbg << "\n[SlideDoor 0x" << std::hex << modelFileHash << std::dec
        << "] prop " << propIndex << " staticSubmeshes={";
    for (size_t s : staticSubmeshes) dbg << s << " ";
    dbg << "} mirror{";
    for (const auto& [f, d] : mirrorFollowerToDriver) dbg << f << "->" << d << " ";
    dbg << "}\n";

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

        // Bounding box per submesh (helps confirm which piece is the pillar / leaves).
        float bxMin = FLT_MAX, bxMax = -FLT_MAX, bzMin = FLT_MAX, bzMax = -FLT_MAX;
        for (const auto& v : skinnedVerts)
        {
            bxMin = std::min(bxMin, v.position.x); bxMax = std::max(bxMax, v.position.x);
            bzMin = std::min(bzMin, v.position.z); bzMax = std::max(bzMax, v.position.z);
        }

        if (isStaticSubmesh(j))
        {
            // Pillar / frame: pin to a static bind-pose bone so it never moves.
            for (auto& sv : skinnedVerts)
                sv.SetSingleBone(staticSlot);
            dbg << "  submesh[" << j << "] STATIC verts=" << skinnedVerts.size()
                << " x[" << bxMin << "," << bxMax << "] z[" << bzMin << "," << bzMax << "]\n";
        }
        else
        {
            // Driver-bone verts stay put (they slide correctly); follower-bone verts are
            // re-skinned onto their driver's mirror slot so they slide the opposite way.
            for (auto& sv : skinnedVerts)
            {
                auto it = followerDriver.find(sv.boneIndices[0]);
                if (it != followerDriver.end())
                    sv.SetSingleBone(driverSlot[it->second]);
            }
            dbg << "  submesh[" << j << "] verts=" << skinnedVerts.size()
                << " x[" << bxMin << "," << bxMax << "] z[" << bzMin << "," << bzMax << "]\n";
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
    prop.controller        = controller;
    prop.clip              = clip;
    prop.meshes            = std::move(animatedMeshes);
    prop.perObjectCBs      = perObjectCBs;
    prop.staticMeshIds     = meshIds;
    prop.pixelShaderType   = pst;
    prop.doorType          = doorType;
    prop.closedAtOpenStart = true;
    prop.openSegmentIndex  = openSeg;
    prop.closeSegmentIndex = openSeg;
    // Each driver -> mirror-slot pair. The mirror plane cancels out for a pure
    // translation (R * T(t) * R = T(-t) regardless of the plane), so 0 is fine.
    for (const auto& [driver, slot] : driverSlot)
        prop.mirrorBonePairs.push_back({ static_cast<int>(driver), static_cast<int>(slot) });
    prop.mirrorPlaneX      = 0.f;
    prop.doubleSided       = false;   // reflected translation preserves winding

    size_t mirrorPairCount = prop.mirrorBonePairs.size();
    m_mapRenderer->AddAnimatedProp(std::move(prop));
    m_doorAnimPropCount++;
    OutputDebugStringA(std::format(
        "[DoorAnim] Uncharted Isle prop {} slide door type {} (hash 0x{:X}) with {} mirror pair(s)\n",
        propIndex, static_cast<int>(doorType), modelFileHash, mirrorPairCount).c_str());
}

// ---------------------------------------------------------------------------
// Door where only part of the mesh animates (Nomad's Isle).
// Pins vertices belonging to a static submesh OR a static bone to the bind pose,
// while the rest plays the open segment (hash 0x303419C9) normally.
// ---------------------------------------------------------------------------

void ReplayWindow::SetupDoorPartialStatic(
    int propIndex,
    const FFNA_ModelFile& modelFile,
    uint32_t modelFileHash,
    const std::vector<Mesh>& meshes,
    const std::vector<PerObjectCB>& perObjectCBs,
    const std::vector<int>& meshIds,
    const std::vector<std::vector<int>>& perMeshTexIds,
    PixelShaderType pst,
    uint8_t doorType,
    const std::vector<size_t>& staticSubmeshes,
    const std::vector<uint32_t>& staticBones,
    bool lockRootPosition,
    const std::vector<std::pair<uint32_t, uint32_t>>& boneRemap)
{
    if (!m_datManager || !m_hashIndex || !m_mapRenderer || !m_deviceResources)
        return;

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    auto mit = m_hashIndex->find(static_cast<int>(modelFileHash));
    if (mit == m_hashIndex->end() || mit->second.empty())
        return;

    int mftIndex = mit->second.at(0);
    uint8_t* animFileData = m_datManager->read_file(mftIndex);
    if (!animFileData)
        return;

    size_t animFileSize = m_datManager->get_MFT()[mftIndex].uncompressedSize;
    auto clipOpt = GW::Parsers::ParseAnimationFromFile(animFileData, animFileSize);
    delete[] animFileData;

    if (!clipOpt || !clipOpt->IsValid())
        return;

    auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*clipOpt));
    clip->BuildAnimationGroups();

    const auto& segments = clip->animationSegments;
    if (segments.size() < 2)
        return;

    size_t openSeg = SIZE_MAX;
    for (size_t s = 0; s < segments.size(); s++)
        if (segments[s].hash == 0x303419C9) openSeg = s;
    if (openSeg == SIZE_MAX) openSeg = segments.size() - 1;

    auto controller = std::make_shared<GW::Animation::AnimationController>();
    controller->Initialize(clip);
    // Mirror the GWMB "Lock Root Position" toggle: strips root-bone translation so the
    // door leaves rotate in place (hinge swing) instead of sliding away. Must be set
    // before the SetSegment/SetTime calls below, which evaluate the bone matrices.
    controller->SetLockRootPosition(lockRootPosition);
    controller->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
    controller->SetSegment(openSeg);
    controller->SetLooping(false);
    controller->SetPlaybackSpeed(100000.0f);
    controller->SetTime(static_cast<float>(segments[openSeg].startTime));
    controller->Play();
    controller->Pause();

    const auto& geomModels = modelFile.geometry_chunk.models;
    size_t boneCount = clip->boneTracks.size();
    const uint32_t staticSlot = static_cast<uint32_t>(boneCount);

    auto isStaticSubmesh = [&](size_t j) {
        return std::find(staticSubmeshes.begin(), staticSubmeshes.end(), j) != staticSubmeshes.end();
    };
    auto isStaticBone = [&](uint32_t b) {
        return std::find(staticBones.begin(), staticBones.end(), b) != staticBones.end();
    };
    auto remapTarget = [&](uint32_t b) -> int {
        for (const auto& [from, to] : boneRemap)
            if (from == b) return static_cast<int>(to);
        return -1;
    };

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

        const bool wholeSubmeshStatic = isStaticSubmesh(j);
        for (auto& sv : skinnedVerts)
        {
            uint32_t b = sv.boneIndices[0];
            if (wholeSubmeshStatic || isStaticBone(b))
            {
                sv.SetSingleBone(staticSlot);
            }
            else
            {
                // Re-skin a bone's vertices onto another bone (e.g. a locked root bone's
                // geometry that should follow a sliding child bone instead of freezing).
                int to = remapTarget(b);
                if (to >= 0)
                    sv.SetSingleBone(static_cast<uint32_t>(to));
            }
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
    prop.controller        = controller;
    prop.clip              = clip;
    prop.meshes            = std::move(animatedMeshes);
    prop.perObjectCBs      = perObjectCBs;
    prop.staticMeshIds     = meshIds;
    prop.pixelShaderType   = pst;
    prop.doorType          = doorType;
    prop.closedAtOpenStart = true;
    prop.openSegmentIndex  = openSeg;
    prop.closeSegmentIndex = openSeg;

    m_mapRenderer->AddAnimatedProp(std::move(prop));
    m_doorAnimPropCount++;
    OutputDebugStringA(std::format(
        "[DoorAnim] partial-static prop {} door type {} (hash 0x{:X}) staticSubmeshes={} staticBones={}\n",
        propIndex, static_cast<int>(doorType), modelFileHash,
        staticSubmeshes.size(), staticBones.size()).c_str());
}

// ---------------------------------------------------------------------------
// Hinged double-door where the .dat only swings one leaf (Nomad's Isle 0x197A9).
// The correct leaf swings on its hinge (driverBone); the second leaf (mirrorBones)
// is driven by a mirrored copy of the driver so it swings open symmetrically.
// staticBones (pillars/frame) are pinned to the bind pose.
// ---------------------------------------------------------------------------

void ReplayWindow::SetupDoorHingeMirror(
    int propIndex,
    const FFNA_ModelFile& modelFile,
    uint32_t modelFileHash,
    const std::vector<Mesh>& meshes,
    const std::vector<PerObjectCB>& perObjectCBs,
    const std::vector<int>& meshIds,
    const std::vector<std::vector<int>>& perMeshTexIds,
    PixelShaderType pst,
    uint8_t doorType,
    const std::vector<uint32_t>& staticBones,
    uint32_t driverBone,
    const std::vector<uint32_t>& mirrorBones,
    const std::vector<size_t>& staticSubmeshes,
    const std::vector<uint32_t>& driverRemapBones,
    const std::vector<size_t>& hiddenSubmeshes,
    const std::vector<uint32_t>& unlockedBones,
    uint32_t openSegHash,
    bool lockRoot,
    uint32_t closeSegHash,
    const std::vector<uint32_t>& ctrlLockedBones)
{
    if (!m_datManager || !m_hashIndex || !m_mapRenderer || !m_deviceResources)
        return;

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    auto mit = m_hashIndex->find(static_cast<int>(modelFileHash));
    if (mit == m_hashIndex->end() || mit->second.empty())
        return;

    int mftIndex = mit->second.at(0);
    uint8_t* animFileData = m_datManager->read_file(mftIndex);
    if (!animFileData)
        return;

    size_t animFileSize = m_datManager->get_MFT()[mftIndex].uncompressedSize;
    auto clipOpt = GW::Parsers::ParseAnimationFromFile(animFileData, animFileSize);
    delete[] animFileData;

    if (!clipOpt || !clipOpt->IsValid())
        return;

    auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*clipOpt));
    clip->BuildAnimationGroups();

    const auto& segments = clip->animationSegments;
    if (segments.size() < 2)
        return;

    size_t openSeg = SIZE_MAX;
    for (size_t s = 0; s < segments.size(); s++)
        if (segments[s].hash == openSegHash) openSeg = s;
    if (openSeg == SIZE_MAX) openSeg = segments.size() - 1;

    // Optional distinct close segment (for doors that toggle open<->closed, e.g. the
    // Frozen Isle lever gates). When absent the door reuses the open segment's first
    // frame as its closed pose (closedAtOpenStart, the open-once behavior).
    size_t closeSeg = SIZE_MAX;
    if (closeSegHash != 0)
    {
        for (size_t s = 0; s < segments.size(); s++)
            if (segments[s].hash == closeSegHash) closeSeg = s;
    }

    auto controller = std::make_shared<GW::Animation::AnimationController>();
    controller->Initialize(clip);
    controller->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
    controller->SetSegment(openSeg);
    controller->SetLooping(false);
    controller->SetPlaybackSpeed(100000.0f);
    // GWMB-style per-bone lock: freeze the given bones to bind pose in the controller
    // itself (NOT SetLockRootPosition - GWMB leaves "Lock Root Position" unchecked). A
    // frozen bone contributes no motion to itself AND its child bones inherit the frozen
    // (bind) transform, which is what GWMB's per-bone "lock" checkbox does. Re-pointing
    // verts to the static slot alone can't stop hierarchy propagation to children.
    // ctrlLockedBones lets the controller-locked set differ from the static-vertex set:
    // e.g. a locked root whose own leaf verts still need to be mirrored (Frozen lever gate).
    if (!ctrlLockedBones.empty())
        controller->SetLockedBones(ctrlLockedBones);
    else if (lockRoot && !staticBones.empty())
        controller->SetLockedBones(staticBones);
    controller->SetTime(static_cast<float>(segments[openSeg].startTime));
    controller->Play();
    controller->Pause();

    const auto& geomModels = modelFile.geometry_chunk.models;
    size_t boneCount = clip->boneTracks.size();
    const uint32_t staticSlot = static_cast<uint32_t>(boneCount);
    const uint32_t mirrorSlot = static_cast<uint32_t>(boneCount) + 1;

    auto isStaticBone = [&](uint32_t b) {
        // If an explicit unlocked set is given, lock every bone NOT in it (inverse mode);
        // otherwise lock only the bones listed in staticBones.
        if (!unlockedBones.empty())
            return std::find(unlockedBones.begin(), unlockedBones.end(), b) == unlockedBones.end();
        return std::find(staticBones.begin(), staticBones.end(), b) != staticBones.end();
    };
    auto isMirrorBone = [&](uint32_t b) {
        return std::find(mirrorBones.begin(), mirrorBones.end(), b) != mirrorBones.end();
    };
    auto isDriverRemapBone = [&](uint32_t b) {
        return std::find(driverRemapBones.begin(), driverRemapBones.end(), b) != driverRemapBones.end();
    };
    auto isStaticSubmesh = [&](size_t j) {
        return std::find(staticSubmeshes.begin(), staticSubmeshes.end(), j) != staticSubmeshes.end();
    };
    auto isHiddenSubmesh = [&](size_t j) {
        return std::find(hiddenSubmeshes.begin(), hiddenSubmeshes.end(), j) != hiddenSubmeshes.end();
    };

    // Symmetry plane = midpoint between the swinging (driver-side) leaf and the mirrored
    // leaf, measured from vertex X-centroids.
    double sumXDriver = 0.0, sumXMirror = 0.0;
    size_t cntDriver = 0, cntMirror = 0;

    std::vector<std::shared_ptr<AnimatedMeshInstance>> animatedMeshes;
    for (size_t j = 0; j < meshes.size(); j++)
    {
        // Hidden submeshes are not turned into animated instances; their static
        // originals are already suppressed by AddAnimatedProp, so they vanish.
        if (isHiddenSubmesh(j))
            continue;

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

        const bool wholeSubmeshStatic = isStaticSubmesh(j);
        for (auto& sv : skinnedVerts)
        {
            uint32_t b = sv.boneIndices[0];
            if (wholeSubmeshStatic || isStaticBone(b))
            {
                sv.SetSingleBone(staticSlot);
            }
            else if (isMirrorBone(b))
            {
                sumXMirror += sv.position.x; cntMirror++;
                sv.SetSingleBone(mirrorSlot);
            }
            else if (isDriverRemapBone(b))
            {
                // Panel geometry rigged to a static bone: re-skin it onto the driver bone
                // so it swings with the real hinge. Counts toward the driver-side centroid.
                sumXDriver += sv.position.x; cntDriver++;
                sv.SetSingleBone(driverBone);
            }
            else
            {
                // Swinging (driver-side) leaf: left on its own bone, animates normally.
                // Only the driver bone's own vertices define the panel centroid used for
                // the symmetry plane (other animating bones may be off-center frame bits).
                if (b == driverBone) { sumXDriver += sv.position.x; cntDriver++; }
            }
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

    float mirrorPlaneX = 0.f;
    if (cntDriver > 0 && cntMirror > 0)
        mirrorPlaneX = static_cast<float>(
            (sumXDriver / cntDriver + sumXMirror / cntMirror) * 0.5);

    MapAnimatedProp prop;
    prop.controller        = controller;
    prop.clip              = clip;
    prop.meshes            = std::move(animatedMeshes);
    prop.perObjectCBs      = perObjectCBs;
    prop.staticMeshIds     = meshIds;
    prop.pixelShaderType   = pst;
    prop.doorType          = doorType;
    if (closeSeg != SIZE_MAX)
    {
        // Toggling door: real open and close segments, no closed-at-open-start reuse.
        prop.closedAtOpenStart = false;
        prop.openSegmentIndex  = openSeg;
        prop.closeSegmentIndex = closeSeg;
    }
    else
    {
        prop.closedAtOpenStart = true;
        prop.openSegmentIndex  = openSeg;
        prop.closeSegmentIndex = openSeg;
    }
    prop.mirrorBonePairs   = {{ static_cast<int>(driverBone), static_cast<int>(mirrorSlot) }};
    prop.mirrorPlaneX      = mirrorPlaneX;
    prop.doubleSided       = true;   // hinge reflection reverses winding

    m_mapRenderer->AddAnimatedProp(std::move(prop));
    m_doorAnimPropCount++;
    OutputDebugStringA(std::format(
        "[DoorAnim] hinge-mirror prop {} door type {} (hash 0x{:X}) driver bone {} plane x={:.2f}\n",
        propIndex, static_cast<int>(doorType), modelFileHash, driverBone, mirrorPlaneX).c_str());
}

// ---------------------------------------------------------------------------
// Procedural double-hinge door (broken rig — panels rigged to static bones).
// Synthesizes a pure vertical-axis hinge rotation for each panel about its own
// outer edge, driven by door open progress, mirrored between the two leaves.
// ---------------------------------------------------------------------------

void ReplayWindow::SetupDoorProceduralDoubleHinge(
    int propIndex,
    const FFNA_ModelFile& modelFile,
    uint32_t modelFileHash,
    const std::vector<Mesh>& meshes,
    const std::vector<PerObjectCB>& perObjectCBs,
    const std::vector<int>& meshIds,
    const std::vector<std::vector<int>>& perMeshTexIds,
    PixelShaderType pst,
    uint8_t doorType,
    const std::vector<size_t>& staticSubmeshes,
    const std::vector<uint32_t>& staticBones,
    const std::vector<uint32_t>& leftBones,
    const std::vector<uint32_t>& rightBones,
    float openAngleDegrees,
    XMFLOAT3 leftOffset,
    XMFLOAT3 rightOffset)
{
    if (!m_datManager || !m_hashIndex || !m_mapRenderer || !m_deviceResources)
        return;

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    auto mit = m_hashIndex->find(static_cast<int>(modelFileHash));
    if (mit == m_hashIndex->end() || mit->second.empty())
        return;

    int mftIndex = mit->second.at(0);
    uint8_t* animFileData = m_datManager->read_file(mftIndex);
    if (!animFileData)
        return;

    size_t animFileSize = m_datManager->get_MFT()[mftIndex].uncompressedSize;
    auto clipOpt = GW::Parsers::ParseAnimationFromFile(animFileData, animFileSize);
    delete[] animFileData;

    if (!clipOpt || !clipOpt->IsValid())
        return;

    auto clip = std::make_shared<GW::Animation::AnimationClip>(std::move(*clipOpt));
    clip->BuildAnimationGroups();

    const auto& segments = clip->animationSegments;
    if (segments.size() < 2)
        return;

    size_t openSeg = SIZE_MAX;
    for (size_t s = 0; s < segments.size(); s++)
        if (segments[s].hash == 0x303419C9) openSeg = s;
    if (openSeg == SIZE_MAX) openSeg = segments.size() - 1;

    auto controller = std::make_shared<GW::Animation::AnimationController>();
    controller->Initialize(clip);
    controller->SetPlaybackMode(GW::Animation::PlaybackMode::SegmentLoop);
    controller->SetSegment(openSeg);
    controller->SetLooping(false);
    controller->SetPlaybackSpeed(100000.0f);
    controller->SetTime(static_cast<float>(segments[openSeg].startTime));
    controller->Play();
    controller->Pause();

    const auto& geomModels = modelFile.geometry_chunk.models;
    size_t boneCount = clip->boneTracks.size();
    const uint32_t staticSlot = static_cast<uint32_t>(boneCount);
    const uint32_t leftSlot   = static_cast<uint32_t>(boneCount) + 1;
    const uint32_t rightSlot  = static_cast<uint32_t>(boneCount) + 2;

    auto inList = [](const std::vector<uint32_t>& v, uint32_t b) {
        return std::find(v.begin(), v.end(), b) != v.end();
    };
    auto isStaticSubmesh = [&](size_t j) {
        return std::find(staticSubmeshes.begin(), staticSubmeshes.end(), j) != staticSubmeshes.end();
    };

    // Pass 1: extract vertices and accumulate per-leaf X bounds + centroid. The outer
    // vertical edge (min-X for the left leaf, max-X for the right) is the hinge; the
    // centroid gives the panel's long-axis direction so we can measure how far the bind
    // pose is tilted out of the wall plane.
    bool haveL = false, haveR = false;
    float lMinX = 0, lMaxX = 0, rMinX = 0, rMaxX = 0;
    double lcx = 0, lcz = 0; int lcn = 0;
    double rcx = 0, rcz = 0; int rcn = 0;

    std::vector<std::vector<SkinnedGWVertex>> perMeshVerts(meshes.size());
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
        perMeshVerts[j] = AnimationPanelState::CreateSkinnedVertices(
            mesh, boneData, vertexBoneGroups, boneCount, clip->hierarchyMode, j);

        if (isStaticSubmesh(j)) continue;
        for (const auto& sv : perMeshVerts[j])
        {
            uint32_t b = sv.boneIndices[0];
            if (inList(leftBones, b)) {
                if (!haveL) { lMinX = lMaxX = sv.position.x; haveL = true; }
                lMinX = std::min(lMinX, sv.position.x);
                lMaxX = std::max(lMaxX, sv.position.x);
                lcx += sv.position.x; lcz += sv.position.z; lcn++;
            } else if (inList(rightBones, b)) {
                if (!haveR) { rMinX = rMaxX = sv.position.x; haveR = true; }
                rMinX = std::min(rMinX, sv.position.x);
                rMaxX = std::max(rMaxX, sv.position.x);
                rcx += sv.position.x; rcz += sv.position.z; rcn++;
            }
        }
    }

    float lHingeX = lMinX;                 // left leaf outer edge
    float rHingeX = rMaxX;                 // right leaf outer edge
    const float lEdgeBand = (lMaxX - lMinX) * 0.15f + 1.0f;
    const float rEdgeBand = (rMaxX - rMinX) * 0.15f + 1.0f;

    // Pass 1b: hinge-edge Z (average Z of the verts on the outer edge).
    double lHingeZSum = 0, rHingeZSum = 0; int lHingeZN = 0, rHingeZN = 0;
    for (size_t j = 0; j < meshes.size(); j++)
    {
        if (isStaticSubmesh(j)) continue;
        for (const auto& sv : perMeshVerts[j])
        {
            uint32_t b = sv.boneIndices[0];
            if (inList(leftBones, b) && sv.position.x <= lHingeX + lEdgeBand) {
                lHingeZSum += sv.position.z; lHingeZN++;
            } else if (inList(rightBones, b) && sv.position.x >= rHingeX - rEdgeBand) {
                rHingeZSum += sv.position.z; rHingeZN++;
            }
        }
    }
    float lHingeZ = lHingeZN ? static_cast<float>(lHingeZSum / lHingeZN) : 0.f;
    float rHingeZ = rHingeZN ? static_cast<float>(rHingeZSum / rHingeZN) : 0.f;

    // Flatten each leaf into the wall plane (both hinges share ~the same Z). The panel's
    // long axis runs from the hinge to its centroid; rotate about the hinge so that axis
    // aligns with the wall line (left leaf -> +X toward center, right leaf -> -X). This
    // removes the bind-pose tilt that leaves the panels behind the pillars and apart.
    auto wrapPi = [](float a) {
        while (a >  3.14159265358979f) a -= 6.28318530717959f;
        while (a < -3.14159265358979f) a += 6.28318530717959f;
        return a;
    };
    float betaL = 0.f, betaR = 0.f;
    if (lcn) betaL = wrapPi(std::atan2(static_cast<float>(lcz / lcn) - lHingeZ,
                                       static_cast<float>(lcx / lcn) - lHingeX) - 0.f);
    if (rcn) betaR = wrapPi(std::atan2(static_cast<float>(rcz / rcn) - rHingeZ,
                                       static_cast<float>(rcx / rcn) - rHingeX)
                            - 3.14159265358979f);

    // Seat each leaf onto its pillar: flatten (negligible here) then translate by the
    // caller-supplied offset. The hinge pivot shifts by the same offset so the runtime
    // swing stays consistent, just relocated onto the pillar.
    XMFLOAT3 Lp{ lHingeX + leftOffset.x,  leftOffset.y,  lHingeZ + leftOffset.z };
    XMFLOAT3 Rp{ rHingeX + rightOffset.x, rightOffset.y, rHingeZ + rightOffset.z };
    XMMATRIX Lflat = XMMatrixTranslation(-lHingeX, 0.f, -lHingeZ)
                   * XMMatrixRotationY(betaL)
                   * XMMatrixTranslation(lHingeX, 0.f, lHingeZ)
                   * XMMatrixTranslation(leftOffset.x, leftOffset.y, leftOffset.z);
    XMMATRIX Rflat = XMMatrixTranslation(-rHingeX, 0.f, -rHingeZ)
                   * XMMatrixRotationY(betaR)
                   * XMMatrixTranslation(rHingeX, 0.f, rHingeZ)
                   * XMMatrixTranslation(rightOffset.x, rightOffset.y, rightOffset.z);
    auto bakeMat = [](SkinnedGWVertex& sv, XMMATRIX M) {
        XMVECTOR p = XMVectorSet(sv.position.x, sv.position.y, sv.position.z, 1.0f);
        XMStoreFloat3(&sv.position, XMVector3Transform(p, M));
        XMVECTOR n = XMVectorSet(sv.normal.x, sv.normal.y, sv.normal.z, 0.0f);
        XMStoreFloat3(&sv.normal, XMVector3Normalize(XMVector3TransformNormal(n, M)));
    };

    // Pass 2: bake the flatten rotation into each leaf and assign bones to slots.
    std::vector<std::shared_ptr<AnimatedMeshInstance>> animatedMeshes;
    for (size_t j = 0; j < meshes.size(); j++)
    {
        const auto& mesh = meshes[j];
        auto skinnedVerts = std::move(perMeshVerts[j]);

        const bool wholeSubmeshStatic = isStaticSubmesh(j);
        for (auto& sv : skinnedVerts)
        {
            uint32_t b = sv.boneIndices[0];
            if (wholeSubmeshStatic || inList(staticBones, b))
            {
                sv.SetSingleBone(staticSlot);
            }
            else if (inList(leftBones, b))
            {
                bakeMat(sv, Lflat);
                sv.SetSingleBone(leftSlot);
            }
            else if (inList(rightBones, b))
            {
                bakeMat(sv, Rflat);
                sv.SetSingleBone(rightSlot);
            }
            else
            {
                // Unclassified geometry: keep static so nothing unexpected moves.
                sv.SetSingleBone(staticSlot);
            }
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

    {
        std::ofstream dbg("door_debug.log", std::ios::app);
        dbg << "\n=== [procedural-hinge] 0x" << std::hex << modelFileHash << std::dec
            << " prop " << propIndex << " ===\n";
        dbg << "  Lpivot=(" << lHingeX << "," << lHingeZ << ") Rpivot=("
            << rHingeX << "," << rHingeZ << ")\n";
        dbg << "  flatten betaL=" << (betaL * 57.29578f) << "deg betaR="
            << (betaR * 57.29578f) << "deg  Lx[" << lMinX << "," << lMaxX
            << "] Rx[" << rMinX << "," << rMaxX << "]\n";
    }

    MapAnimatedProp prop;
    prop.controller        = controller;
    prop.clip              = clip;
    prop.meshes            = std::move(animatedMeshes);
    prop.perObjectCBs      = perObjectCBs;
    prop.staticMeshIds     = meshIds;
    prop.pixelShaderType   = pst;
    prop.doorType          = doorType;
    prop.closedAtOpenStart = true;
    prop.openSegmentIndex  = openSeg;
    prop.closeSegmentIndex = openSeg;
    prop.proceduralHinge   = true;
    prop.hingeStaticSlot   = staticSlot;
    prop.hingeLeftSlot     = leftSlot;
    prop.hingeRightSlot    = rightSlot;
    prop.hingeLeftPivot    = Lp;
    prop.hingeRightPivot   = Rp;
    prop.hingeMaxAngle     = openAngleDegrees * 3.14159265358979f / 180.f;
    prop.doubleSided       = true;   // rotating panels may face away from the camera

    m_mapRenderer->AddAnimatedProp(std::move(prop));
    m_doorAnimPropCount++;
    OutputDebugStringA(std::format(
        "[DoorAnim] procedural-hinge prop {} door type {} (hash 0x{:X}) "
        "Lpivot=({:.1f},{:.1f}) Rpivot=({:.1f},{:.1f}) angle={:.0f}deg\n",
        propIndex, static_cast<int>(doorType), modelFileHash,
        lHingeX, lHingeZ, rHingeX, rHingeZ, openAngleDegrees).c_str());
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

    if (!ap.controller->IsPlaying())
    {
        ap.controller->SetLooping(true);
        ap.controller->Play();
    }

    // Only apply mesh visibility changes when the owner state has actually changed.
    // Calling SetMeshShouldRender every frame unconditionally would set
    // m_should_rerender_shadows = true each frame, forcing a full shadow-map rebuild
    // even though nothing visually changed.
    if (m_obeliskOwnerInitialized && owner == m_obeliskLastOwner)
        return;

    m_obeliskLastOwner        = owner;
    m_obeliskOwnerInitialized = true;

    bool visible = (owner == StandOwner::Neutral);

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

    if (!ap.controller->IsPlaying())
    {
        ap.controller->SetLooping(true);
        ap.controller->Play();
    }

    // Only apply mesh visibility changes when the owner state has actually changed.
    // Calling SetMeshShouldRender every frame unconditionally would set
    // m_should_rerender_shadows = true each frame, forcing a full shadow-map rebuild
    // even though nothing visually changed.
    if (m_towerOwnerInitialized && owner == m_towerLastOwner)
        return;

    m_towerLastOwner        = owner;
    m_towerOwnerInitialized = true;

    bool neutral = (owner == StandOwner::Neutral);

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
// Gate lever — load and render the animated lever model (Isle of the Weeping Stone)
//
// The physical lever (model 0x2AD0D) sits at the "Gate lever" gadget. It is static
// by default and plays its open segment (0x35E6AE29) / close segment (0x36F05E31)
// in sync with the lever door (object 122 -> door type 19), exactly like the Isle of
// Meditation gate locks: same animation clip (0x7079), only submesh 0 animated.
// ---------------------------------------------------------------------------

void ReplayWindow::SetupWeepingLeverProp()
{
    if (m_weepingLeverModelLoaded)
        return;
    m_weepingLeverModelLoaded = true;

    if (!m_datManager || !m_hashIndex || !m_mapRenderer)
        return;

    if (m_replayCtx.datMapId != 0x2661F)
        return;

    if (m_replayCtx.agents.empty())
        return;

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    struct LeverPos { float x, y, z; };
    std::vector<LeverPos> positions;
    for (auto& [aid, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;
        if (ard.categoryName != "Gate lever") continue;
        positions.push_back({ ard.snapshots[0].x, ard.snapshots[0].y, ard.snapshots[0].z });
    }
    if (positions.empty())
        return;

    constexpr uint32_t kLeverModelHash = 0x2AD0D;
    auto mit = m_hashIndex->find(static_cast<int>(kLeverModelHash));
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

    // The lever shares the Isle of Meditation gate-lock animation clip (segment hashes
    // match: 0x35E6AE29 open, 0x36F05E31 close).
    constexpr uint32_t kLeverAnimHash = 0x7079;
    auto animIt = m_hashIndex->find(static_cast<int>(kLeverAnimHash));
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
        // Rotate the lever -90 deg (yaw about vertical Y) so it faces the tower flag stand.
        XMMATRIX worldMat = XMMatrixRotationY(-XM_PIDIV2)
                          * XMMatrixTranslation(renderPos.x, renderPos.y, renderPos.z);

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

        auto meshIds = map_renderer->AddProp(meshes, perObjectCBs, 0xFFFF0007u, pst);

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
            // Only submesh 0 is animated (the lever handle on bone 0); all other
            // submeshes stay as static rendered meshes.
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
        prop.doorType        = 19;   // driven by the lever door (object 122) open/close events
        prop.openSegmentIndex  = openSeg;
        prop.closeSegmentIndex = closeSeg;

        // Start closed/static: hold the last frame of the close segment, paused.
        controller->SetSegment(closeSeg);
        controller->SetTime(static_cast<float>(segments[closeSeg].endTime));
        controller->Pause();

        map_renderer->AddAnimatedProp(std::move(prop));
        m_doorAnimPropCount++;
    }
}

// ---------------------------------------------------------------------------
// Gate Lock — load and render the animated lever models (Frozen Isle)
//
// Two lever props (model 0x1E0E1) sit at the "Gate lever" gadgets. Each is static by
// default and plays its open segment (0x35E6AE29) / close segment (0x36F05E31), once
// per trigger (no looping), in sync with the gate it controls. Bone 0 drives the lever
// handle and bone 1 is locked; only submesh 0 animates (submeshes 1 & 2 stay static).
// The shared gate-lock animation clip (0x7079) carries those segments. Each prop's real
// door type (21 red side / 22 blue side) is assigned in ResolveFrozenGates by nearest
// guild lord, so the handle animates whenever its team's gate toggles.
// ---------------------------------------------------------------------------

void ReplayWindow::SetupFrozenGateLockProps()
{
    if (m_frozenLeverModelLoaded)
        return;
    m_frozenLeverModelLoaded = true;

    if (!m_datManager || !m_hashIndex || !m_mapRenderer)
        return;

    if (m_replayCtx.datMapId != 0x1F265)
        return;

    if (m_replayCtx.agents.empty())
        return;

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    struct LeverPos { float x, y, z; };
    std::vector<LeverPos> positions;
    for (auto& [aid, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;
        if (ard.categoryName != "Gate lever") continue;   // categoryName stays "Gate lever"
        positions.push_back({ ard.snapshots[0].x, ard.snapshots[0].y, ard.snapshots[0].z });
    }
    if (positions.empty())
        return;

    constexpr uint32_t kLeverModelHash = 0x1E0E1;
    auto mit = m_hashIndex->find(static_cast<int>(kLeverModelHash));
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

    // Shared gate-lock animation clip (segment hashes: 0x35E6AE29 open, 0x36F05E31 close).
    constexpr uint32_t kLeverAnimHash = 0x7079;
    auto animIt = m_hashIndex->find(static_cast<int>(kLeverAnimHash));
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

    // Determine red/blue guild lord positions so we can orient each lever per team side.
    XMFLOAT3 redLord{}, blueLord{};
    bool haveRed = false, haveBlue = false;
    for (auto& [aid, a] : m_replayCtx.agents)
    {
        if (a.snapshots.empty()) continue;
        if (a.categoryName.find("Guild Lord") == std::string::npos) continue;
        const auto& s0 = a.snapshots.front();
        if (a.teamId == 1 && !haveRed)  { redLord  = { s0.x, s0.y, s0.z }; haveRed  = true; }
        if (a.teamId == 2 && !haveBlue) { blueLord = { s0.x, s0.y, s0.z }; haveBlue = true; }
    }

    for (const auto& pos : positions)
    {
        XMFLOAT3 renderPos = ApplyMapTransformToPos(pos.x, pos.y, pos.z, m_replayCtx.mapTransform);

        // Red-side lever rotated 180°, blue-side lever rotated 90°.
        float yaw = 0.f;
        if (haveRed && haveBlue)
        {
            float dr = (pos.x - redLord.x) * (pos.x - redLord.x)
                     + (pos.y - redLord.y) * (pos.y - redLord.y)
                     + (pos.z - redLord.z) * (pos.z - redLord.z);
            float db = (pos.x - blueLord.x) * (pos.x - blueLord.x)
                     + (pos.y - blueLord.y) * (pos.y - blueLord.y)
                     + (pos.z - blueLord.z) * (pos.z - blueLord.z);
            yaw = (dr <= db) ? XM_PI : XM_PIDIV2;   // red: 180°, blue: 90°
        }
        XMMATRIX worldMat = XMMatrixRotationY(yaw)
                          * XMMatrixTranslation(renderPos.x, renderPos.y, renderPos.z);

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

        auto meshIds = map_renderer->AddProp(meshes, perObjectCBs, 0xFFFF0007u, pst);

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
            // Only submesh 0 is animated (the lever handle on bone 0); submeshes 1 & 2
            // stay as static rendered meshes.
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

            // Lock bone 1 vertices to identity so only bone 0 (lever) animates.
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
        prop.doorType        = 21;   // placeholder; ResolveFrozenGates() assigns 21/22 by side
        prop.closedAtOpenStart = false;
        prop.openSegmentIndex  = openSeg;
        prop.closeSegmentIndex = closeSeg;

        // Start at the open pose (gates 1 & 2 open at match start); UpdateDoorAnimations
        // re-seeds the correct state on the first scan once door types are assigned.
        controller->SetSegment(openSeg);
        controller->SetTime(static_cast<float>(segments[openSeg].endTime));
        controller->Pause();

        auto& aps = map_renderer->GetAnimatedProps();
        map_renderer->AddAnimatedProp(std::move(prop));
        m_doorAnimPropCount++;

        // Record this lever so ResolveFrozenGates() can bind it to its team's gate type.
        // Store the RAW gadget position (same space as guild-lord snapshots used there).
        if (!aps.empty())
            m_frozenLeverCandidates.push_back({
                aps.size() - 1,
                XMFLOAT3(pos.x, pos.y, pos.z),
                kLeverModelHash });
    }
}

// ---------------------------------------------------------------------------
// Frozen Isle lever gates — resolve each shared-model prop's door type
// ---------------------------------------------------------------------------
// 0x255BE has two instances (doors 1 & 2) and 0x57B57 has two (doors 3 & 4). The two
// instances of a model sit on opposite team sides, so we split each pair by nearest
// guild lord: the prop closer to the BLUE lord takes the blue-side object id's door type,
// the other takes the red-side type. Runs once, after guild-lord snapshots are available.
void ReplayWindow::ResolveFrozenGates()
{
    if (m_frozenGatesResolved)
        return;
    if (m_replayCtx.datMapId != 0x1F265)
        return;
    if (m_frozenGateCandidates.empty())
        return;
    if (!m_mapRenderer || m_replayCtx.agents.empty())
        return;

    // Guild lord reference positions (teamId 1 = red, 2 = blue).
    bool haveBlue = false, haveRed = false;
    DirectX::XMFLOAT3 blueLord{}, redLord{};
    for (auto& [aid, a] : m_replayCtx.agents)
    {
        if (a.snapshots.empty()) continue;
        if (a.categoryName.find("Guild Lord") == std::string::npos) continue;
        const auto& s0 = a.snapshots.front();
        if (a.teamId == 2 && !haveBlue) { blueLord = { s0.x, s0.y, s0.z }; haveBlue = true; }
        else if (a.teamId == 1 && !haveRed) { redLord = { s0.x, s0.y, s0.z }; haveRed = true; }
    }
    if (!haveBlue || !haveRed)
        return;   // wait until both lords are present

    auto dist2 = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
        float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    };

    auto& animProps = m_mapRenderer->GetAnimatedProps();

    // Resolve one model group (exactly its two instances) to (blueType, redType).
    auto resolveGroup = [&](uint32_t modelHash, uint8_t blueType, uint8_t redType) {
        std::vector<FrozenGateCandidate*> group;
        for (auto& c : m_frozenGateCandidates)
            if (c.modelHash == modelHash) group.push_back(&c);
        if (group.size() < 2)
        {
            // Only one instance found: assign by whichever lord is nearer.
            for (auto* c : group)
            {
                if (c->animPropIndex >= animProps.size()) continue;
                bool blue = dist2(c->worldPos, blueLord) <= dist2(c->worldPos, redLord);
                animProps[c->animPropIndex].doorType = blue ? blueType : redType;
            }
            return;
        }
        // Pick the instance closest to the blue lord as the blue-side door; the other is red.
        size_t blueIdx = 0;
        float bestBlue = FLT_MAX;
        for (size_t k = 0; k < group.size(); k++)
        {
            float d = dist2(group[k]->worldPos, blueLord);
            if (d < bestBlue) { bestBlue = d; blueIdx = k; }
        }
        for (size_t k = 0; k < group.size(); k++)
        {
            if (group[k]->animPropIndex >= animProps.size()) continue;
            animProps[group[k]->animPropIndex].doorType = (k == blueIdx) ? blueType : redType;
        }
    };

    resolveGroup(0x255BE, /*blue*/ 22, /*red*/ 21);   // doors 2 (11692) / 1 (61318)
    resolveGroup(0x57B57, /*blue*/ 24, /*red*/ 23);   // doors 4 (12669) / 3 (56526)

    // Record each gate's resolved door type + world position for the minimap state icons.
    m_frozenGateIcons.clear();
    for (auto& c : m_frozenGateCandidates)
    {
        if (c.animPropIndex >= animProps.size()) continue;
        int dt = animProps[c.animPropIndex].doorType;
        if (dt >= 21 && dt <= 24)
            m_frozenGateIcons.push_back({ dt, c.worldPos });
    }

    // Bind each gate-lock lever to a gate on its own team side (nearest guild lord):
    // red-side lever -> door 1 (type 21), blue-side lever -> door 2 (type 22). Both those
    // gates start open, so the lever handle starts in the open pose and toggles with them.
    for (auto& c : m_frozenLeverCandidates)
    {
        if (c.animPropIndex >= animProps.size()) continue;
        bool red = dist2(c.worldPos, redLord) <= dist2(c.worldPos, blueLord);
        animProps[c.animPropIndex].doorType = red ? 21 : 22;
    }

    m_frozenGatesResolved = true;
    // Force UpdateDoorAnimations to re-seed poses now that door types are known.
    m_doorLastScanTime = -1.f;
}

// ---------------------------------------------------------------------------
// Druid's Isle vine bridges — one door type per instance
// ---------------------------------------------------------------------------

void ReplayWindow::ResolveDruidBridges()
{
    if (m_druidBridgesResolved)
        return;
    if (m_replayCtx.datMapId != 0x1F27A)
        return;
    if (m_druidBridgeCandidates.empty())
        return;
    if (!m_mapRenderer || m_replayCtx.agents.empty())
        return;

    // Guild lord reference positions (teamId 1 = red, 2 = blue).
    bool haveBlue = false, haveRed = false;
    DirectX::XMFLOAT3 blueLord{}, redLord{};
    for (auto& [aid, a] : m_replayCtx.agents)
    {
        if (a.snapshots.empty()) continue;
        if (a.categoryName.find("Guild Lord") == std::string::npos) continue;
        const auto& s0 = a.snapshots.front();
        if (a.teamId == 2 && !haveBlue) { blueLord = { s0.x, s0.y, s0.z }; haveBlue = true; }
        else if (a.teamId == 1 && !haveRed) { redLord = { s0.x, s0.y, s0.z }; haveRed = true; }
    }
    if (!haveBlue || !haveRed)
        return;   // wait until both lords are present

    auto dist2 = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
        float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    };

    auto& animProps = m_mapRenderer->GetAnimatedProps();

    // Type 26 = object 39278 (red side), type 27 = object 51238 (blue side). If the two
    // bridges ever turn out to be swapped relative to the events, exchange these two.
    constexpr uint8_t kRedType = 26, kBlueType = 27;

    if (m_druidBridgeCandidates.size() < 2)
    {
        for (auto& c : m_druidBridgeCandidates)
        {
            if (c.animPropIndex >= animProps.size()) continue;
            bool red = dist2(c.worldPos, redLord) <= dist2(c.worldPos, blueLord);
            animProps[c.animPropIndex].doorType = red ? kRedType : kBlueType;
        }
    }
    else
    {
        // Pick the instance closest to the red lord as the red-side bridge; the other is blue.
        size_t redIdx = 0;
        float bestRed = FLT_MAX;
        for (size_t k = 0; k < m_druidBridgeCandidates.size(); k++)
        {
            float d = dist2(m_druidBridgeCandidates[k].worldPos, redLord);
            if (d < bestRed) { bestRed = d; redIdx = k; }
        }
        for (size_t k = 0; k < m_druidBridgeCandidates.size(); k++)
        {
            size_t pi = m_druidBridgeCandidates[k].animPropIndex;
            if (pi >= animProps.size()) continue;
            animProps[pi].doorType = (k == redIdx) ? kRedType : kBlueType;
        }
    }

    // Minimap state icons. Neither the prop's origin nor its grow anchor is the
    // right place for these: the map already has a gadget standing at each bridge's
    // cliff edge — the thing the seed is planted into, and what shows as a neutral
    // dot on the minimap — so the icon belongs on the gadget and replaces its dot.
    // Pair the two gadgets to the two bridges by guild lord proximity, exactly as
    // the props were paired above, which keeps everything in agent space.
    m_druidBridgeIcons.clear();
    m_druidBridgeGadgets.clear();
    {
        std::vector<std::pair<int, DirectX::XMFLOAT3>> gadgets;
        for (auto& [aid, a] : m_replayCtx.agents)
        {
            if (a.type != AgentType::Gadget || a.snapshots.empty()) continue;
            const auto& s0 = a.snapshots.front();
            gadgets.push_back({ aid, { s0.x, s0.y, s0.z } });
        }

        std::ofstream dbg("door_debug.log", std::ios::app);
        for (auto& [aid, p] : gadgets)
            dbg << "[DruidGadget] agent=" << aid
                << " pos=(" << p.x << "," << p.y << "," << p.z << ")"
                << " dRed=" << std::sqrt(dist2(p, redLord))
                << " dBlue=" << std::sqrt(dist2(p, blueLord)) << "\n";

        int redIdx = -1, blueIdx = -1;
        float bestRed = FLT_MAX, bestBlue = FLT_MAX;
        for (size_t k = 0; k < gadgets.size(); k++)
        {
            float dr = dist2(gadgets[k].second, redLord);
            if (dr < bestRed) { bestRed = dr; redIdx = static_cast<int>(k); }
        }
        for (size_t k = 0; k < gadgets.size(); k++)
        {
            if (static_cast<int>(k) == redIdx) continue;
            float db = dist2(gadgets[k].second, blueLord);
            if (db < bestBlue) { bestBlue = db; blueIdx = static_cast<int>(k); }
        }

        if (redIdx >= 0 && blueIdx >= 0)
        {
            m_druidBridgeIcons.push_back({ kRedType,  gadgets[redIdx].second });
            m_druidBridgeIcons.push_back({ kBlueType, gadgets[blueIdx].second });
            m_druidBridgeGadgets.insert(gadgets[redIdx].first);
            m_druidBridgeGadgets.insert(gadgets[blueIdx].first);
        }
    }

    m_druidBridgesResolved = true;
    // Force UpdateDoorAnimations to re-seed poses now that door types are known.
    m_doorLastScanTime = -1.f;
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
// Catapult lever model (Warrior's Isle)
//
// Once a repair kit has been applied, the flat lever icon gives way to the real
// model. It stays hidden until then, so the world shows the catapult becoming
// usable rather than always advertising it. The state glow and label continue to
// be drawn in screen space over the model.
// ---------------------------------------------------------------------------

void ReplayWindow::SetupCatapultLeverProps()
{
    if (m_catapultLeverModelLoaded)
        return;
    if (!m_catapultLeversResolved || m_catapultLevers.empty())
        return;
    m_catapultLeverModelLoaded = true;

    if (!m_datManager || !m_hashIndex || !m_mapRenderer)
        return;

    if (m_replayCtx.mapId != kWarriorsIsleMapId)
        return;

    auto* device = m_deviceResources->GetD3DDevice();
    if (!device)
        return;

    // This file id covers more than one entry and only one of them is geometry,
    // so every candidate is tried rather than assuming the first is the model.
    constexpr uint32_t kLeverModelHash = 0x7079;
    auto mit = m_hashIndex->find(static_cast<int>(kLeverModelHash));
    if (mit == m_hashIndex->end() || mit->second.empty())
        return;

    FFNA_ModelFile modelFile;
    bool haveModel = false;
    for (int mftIndex : mit->second)
    {
        try {
            FFNA_ModelFile candidate = m_datManager->parse_ffna_model_file(mftIndex);
            if (candidate.parsed_correctly && !candidate.geometry_chunk.models.empty())
            {
                modelFile = std::move(candidate);
                haveModel = true;
                break;
            }
        } catch (...) {
        }
    }

    {
        std::ofstream dbg("door_debug.log", std::ios::app);
        dbg << "[CatapultLever] hash=0x7079 entries=" << mit->second.size()
            << " model=" << (haveModel ? "yes" : "no")
            << " levers=" << m_catapultLevers.size() << "\n";
    }
    if (!haveModel)
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

    for (auto& cl : m_catapultLevers)
    {
        float rotation = 0.f;
        auto ait = m_replayCtx.agents.find(cl.agentId);
        if (ait != m_replayCtx.agents.end() && !ait->second.snapshots.empty())
            rotation = ait->second.snapshots[0].rotation;

        XMFLOAT3 renderPos = ApplyMapTransformToPos(cl.x, cl.y, cl.z, m_replayCtx.mapTransform);
        XMMATRIX worldMat = XMMatrixRotationY(rotation)
                          * XMMatrixTranslation(renderPos.x, renderPos.y, renderPos.z);

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
                    perObjectCBs[j].uv_indices[k / 4][k % 4]      = static_cast<uint32_t>(mesh.uv_coord_indices[k]);
                    perObjectCBs[j].texture_indices[k / 4][k % 4] = static_cast<uint32_t>(mesh.tex_indices[k]);
                    perObjectCBs[j].blend_flags[k / 4][k % 4]     = static_cast<uint32_t>(mesh.blend_flags[k]);
                    perObjectCBs[j].texture_types[k / 4][k % 4]   = static_cast<uint32_t>(mesh.texture_types[k]);
                }
            }
        }

        auto meshIds = map_renderer->AddProp(meshes, perObjectCBs, 0xFFFF0008u, pst);

        if (modelFile.textures_parsed_correctly)
        {
            for (size_t l = 0; l < meshIds.size() && l < perMeshTexIds.size(); l++)
            {
                auto texVec = map_renderer->GetTextureManager()->GetTextures(perMeshTexIds[l]);
                map_renderer->GetMeshManager()->SetTexturesForMesh(meshIds[l], texVec, 3);
            }
        }

        for (int mid : meshIds)
            map_renderer->SetMeshShouldRender(mid, false);

        cl.meshIds = std::move(meshIds);
        cl.visible = false;
    }
}


void ReplayWindow::UpdateCatapultLeverProps()
{
    if (!m_mapRenderer) return;

    for (auto& cl : m_catapultLevers)
    {
        if (cl.meshIds.empty()) continue;

        bool show = (m_debugTimeline >= cl.repairedTime);
        // Toggling every frame would flag the shadow map dirty for no reason.
        if (show == cl.visible) continue;

        for (int mid : cl.meshIds)
            m_mapRenderer->SetMeshShouldRender(mid, show);
        cl.visible = show;
    }
}


// ---------------------------------------------------------------------------
// Catapults (Warrior's Isle, model 0x220F6)
//
// The map places the two machines but says nothing about which object id drives
// which, so each is matched to the nearest resolved lever. A catapult that is
// never repaired has no lever and simply stays wrecked, which is what it should
// look like anyway.
// ---------------------------------------------------------------------------

void ReplayWindow::ResolveCatapultProps()
{
    if (m_catapultPropsResolved) return;
    if (!m_catapultLeversResolved) return;

    m_catapultPropsResolved = true;
    if (m_catapultProps.empty()) return;

    const auto& t = m_replayCtx.mapTransform;

    for (auto& cp : m_catapultProps)
    {
        float bestDist = FLT_MAX;
        const CatapultLever* best = nullptr;

        for (const auto& cl : m_catapultLevers)
        {
            XMFLOAT3 lp = ApplyMapTransformToPos(cl.x, cl.y, cl.z, t);
            float dx = lp.x - cp.renderPos.x;
            float dz = lp.z - cp.renderPos.z;
            float d2 = dx * dx + dz * dz;
            if (d2 < bestDist) { bestDist = d2; best = &cl; }
        }

        // Levers sit a couple of hundred units from their machine. Without a
        // ceiling, a catapult that is never repaired latches onto the other
        // side's lever half a map away and mirrors its animations.
        constexpr float kMaxLeverDist = 1500.f;

        if (best && bestDist <= kMaxLeverDist * kMaxLeverDist)
        {
            cp.objectId     = best->objectId;
            cp.repairedTime = best->repairedTime;
        }
        else
        {
            best = nullptr;
        }

        std::ofstream dbg("door_debug.log", std::ios::app);
        dbg << "[Catapult] prop " << cp.animPropIndex
            << " pos=(" << cp.renderPos.x << "," << cp.renderPos.z << ")"
            << " objectId=" << cp.objectId
            << " repairedAt=" << cp.repairedTime
            << " dist=" << (best ? std::sqrt(bestDist) : -1.f) << "\n";
    }
}

void ReplayWindow::UpdateCatapultAnimations()
{
    if (m_catapultProps.empty() || !m_mapRenderer) return;

    auto& animProps = m_mapRenderer->GetAnimatedProps();
    const float now = m_debugTimeline;

    for (auto& cp : m_catapultProps)
    {
        if (cp.animPropIndex < 0 || cp.animPropIndex >= static_cast<int>(animProps.size()))
            continue;

        auto& ap = animProps[cp.animPropIndex];
        if (!ap.controller || !ap.clip) continue;

        const auto& segs = ap.clip->animationSegments;

        // Clip times are in the controller's own units. 100000 of them per second
        // is what AnimationController calls 1x, and what the model viewer plays at
        // when its speed slider sits on 1.0.
        constexpr float kClipUnitsPerSecond = 100000.f;

        auto segDuration = [&](size_t seg) -> float {
            if (seg >= segs.size()) return 0.f;
            float units = static_cast<float>(segs[seg].endTime)
                        - static_cast<float>(segs[seg].startTime);
            return std::max(0.01f, units / kClipUnitsPerSecond);
        };

        // Every pose is addressed the same way: pick a segment and sit at a given
        // point inside it. Nothing is ever left playing, so scrubbing the timeline
        // lands on exactly the frame the replay time calls for.
        auto pose = [&](size_t seg, float progress) {
            if (seg >= segs.size()) return;
            float s0 = static_cast<float>(segs[seg].startTime);
            float s1 = static_cast<float>(segs[seg].endTime);
            ap.controller->SetLooping(false);
            ap.controller->SetSegment(seg);
            ap.controller->SetTime(s0 + std::clamp(progress, 0.f, 1.f) * (s1 - s0));
            ap.controller->Pause();
        };

        if (now < cp.repairedTime)
        {
            pose(cp.segBroken, 0.f);
            continue;
        }

        auto csIt = m_catapultStates.find(cp.objectId);
        if (csIt == m_catapultStates.end())
        {
            pose(cp.segRepaired, 0.f);
            continue;
        }

        CatapultState st = CatapultState::Unknown;
        float evTime = 0.f;
        if (!csIt->second.LastEvent(now, st, evTime))
        {
            pose(cp.segRepaired, 0.f);
            continue;
        }

        float elapsed = now - evTime;

        switch (st)
        {
        case CatapultState::Repaired:
        {
            float d = segDuration(cp.segRepairing);
            if (elapsed < d) pose(cp.segRepairing, elapsed / d);
            else             pose(cp.segRepaired, 0.f);
            break;
        }
        case CatapultState::Loaded:
        {
            // Holds the last frame of the load once it finishes: that pose is the
            // armed catapult, waiting to be let go.
            float d = segDuration(cp.segLoading);
            pose(cp.segLoading, (elapsed < d) ? (elapsed / d) : 1.f);
            break;
        }
        case CatapultState::Fired:
        case CatapultState::Impact:
        {
            float d = segDuration(cp.segFiring);
            if (elapsed < d) pose(cp.segFiring, elapsed / d);
            else             pose(cp.segRepaired, 0.f);
            break;
        }
        default:
            pose(cp.segRepaired, 0.f);
            break;
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
    if (datMapId == 0x1F24D) // Burning Isle
    {
        switch (objectId) {
        case 55597: return 5;   // door model 0x1F23F
        case 5733:  return 6;   // door model 0x1F247
        default: return 0;
        }
    }
    if (datMapId == 0x1F268) // Nomad's Isle
    {
        switch (objectId) {
        case 55597: case 5733: return 11;
        case 37843: case 17419: case 32640: case 18513: return 12;
        default: return 0;
        }
    }
    if (datMapId == 0x3321C) // Isle of Wurms
    {
        // 4 door events fire simultaneously (once per match); the object_id -> physical
        // door mapping is unknown, and there are only two door models (0x330F7 x2,
        // 0x331A4 x2). Since they all open together, assign every id to one door type
        // and let each model prop share it.
        switch (objectId) {
        case 56526: case 61318: case 12669: case 11692: return 13;
        default: return 0;
        }
    }
    if (datMapId == 0x33056) // Uncharted Isle
    {
        // All 4 door events fire simultaneously (once per match) and the mapping of
        // object_id -> physical door is unknown. Since they open together it does not
        // matter which is which, so assign each object_id to a distinct door type.
        switch (objectId) {
        case 56526: return 7;   // door model 0x3C163  (hinge)
        case 61318: return 8;   // door model 0x32F3A  (hinge)
        case 12669: return 9;   // door model 0x32F0C  (horizontal slide)
        case 11692: return 10;  // door model 0x336BB  (horizontal telescoping slide)
        default: return 0;
        }
    }
    if (datMapId == 0x26625) // Isle of Jade
    {
        // 4 door events fire simultaneously (once per match); object_id -> physical door
        // mapping is unknown. Two models (0x285E7 x2, 0x265B5 x2) all open together, so
        // assign every id to one shared door type.
        switch (objectId) {
        case 11692: case 56526: case 12669: case 61318: return 14;
        default: return 0;
        }
    }
    if (datMapId == 0x1F29B) // Isle of the Dead
    {
        // 4 door events fire simultaneously (once per match); object_id -> physical door
        // mapping is unknown. Four door models (0x1F294, 0x1F291, 0x1F281, 0x1E820) all
        // open together, so assign every id to one shared door type.
        switch (objectId) {
        case 56526: case 61318: case 12669: case 11692: return 15;
        default: return 0;
        }
    }
    if (datMapId == 0x334A2) // Isle of Solitude
    {
        // 4 door events fire simultaneously (once per match); object_id -> physical door
        // mapping is unknown. Two models (0x33CD5 x2, 0x3323B x2) all open together, so
        // assign every id to one shared door type.
        switch (objectId) {
        case 56529: case 61318: case 12669: case 11692: return 16;
        default: return 0;
        }
    }
    if (datMapId == 0x2661F) // Isle of the Weeping Stone
    {
        // The 0x2858E/0x28578 gates open purely on the timeline (type 17, handled below).
        // 0x1EAFB is shared: the four event gates (147/9305/30563/4417, open once at ~01:00)
        // are type 18, while object 122 is the lever door that toggles open/close and is
        // type 19. Both use model 0x1EAFB; the physical lever prop is resolved at runtime as
        // the 0x1EAFB instance nearest the flag stand.
        switch (objectId) {
        case 147: case 9305: case 30563: case 4417: return 18;
        case 122:                                    return 19;
        default: return 0;
        }
    }
    if (datMapId == 0x1F27A) // Druid's Isle
    {
        // Two vine bridges that grow when a vine seed is planted (StoC door event).
        // 39278 = bridge nearest red lord, 51238 = bridge nearest blue lord. Both are model
        // 0x29FD, but each grows on its own event, so they get separate door types; the
        // prop -> type assignment happens by nearest guild lord in ResolveDruidBridges().
        switch (objectId) {
        case 39278: return 26;
        case 51238: return 27;
        default: return 0;
        }
    }
    if (datMapId == 0x3314E) // Corrupted Isle
    {
        // 4 doors open simultaneously once per match; 6 object ids, unknown mapping.
        switch (objectId) {
        case 54462: case 63151: case 33911: case 44135: case 40443: case 52019: return 25;
        default: return 0;
        }
    }
    if (datMapId == 0x1F265) // Frozen Isle
    {
        // Door 0x1F251/0x1F252 open once per match; the event carries one of these object
        // ids (15922/55597/5733) and we can't tell them apart, so map all to one door type.
        // The four lever gates each get their own type (they toggle independently from two
        // levers): 61318/11692 share model 0x255BE, 56526/12669 share model 0x57B57.
        switch (objectId) {
        case 15922: case 55597: case 5733: return 20;
        case 61318: return 21;   // door 1 (0x255BE, red side)
        case 11692: return 22;   // door 2 (0x255BE, blue side)
        case 56526: return 23;   // door 3 (0x57B57, red side)
        case 12669: return 24;   // door 4 (0x57B57, blue side)
        default: return 0;
        }
    }
    if (datMapId == 0x1F1FC) // Warrior's Isle
    {
        // The four base gates share a single type: door_events shows all of them
        // carrying the same stage-2 open at 01:00.6 and never closing again, so
        // there is nothing to tell apart and no need to match ids to instances.
        switch (objectId) {
        case 11692: case 12669: case 56526: case 61318: return 28;
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

    bool prevOpen[kDoorTypeCount];
    std::copy(std::begin(m_doorTypeOpen), std::end(m_doorTypeOpen), std::begin(prevOpen));

    if (seeked)
    {
        std::fill(std::begin(m_doorTypeOpen), std::end(m_doorTypeOpen), false);

        // Frozen Isle lever gates start with doors 1 & 2 OPEN and doors 3 & 4 CLOSED
        // (confirmed in door_events: the match-start events for 61318/11692 carry status
        // 1=open, 56526/12669 carry status 2=closed). Those initial events are stage 3 and
        // are skipped by the stage-2 filter below, so seed the open pair here; the per-event
        // toggles after t=0 take over from there.
        if (m_replayCtx.datMapId == 0x1F265)
        {
            m_doorTypeOpen[21] = true;   // door 1 (61318) open at start
            m_doorTypeOpen[22] = true;   // door 2 (11692) open at start
        }
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

    // Isle of the Weeping Stone auto-open doors: the StoC door-event object ids don't map
    // cleanly to these gates, so open them purely on the timeline instead. m_debugTimeline
    // is 0-based (0 = start of replay, in seconds, only advancing while playing), so open
    // them 5 s after the replay begins.
    if (m_replayCtx.datMapId == 0x2661F)
        m_doorTypeOpen[17] = (curTime >= 5.0f);

    float frameDt = static_cast<float>(m_timer.GetElapsedSeconds());

    auto& animProps = m_mapRenderer->GetAnimatedProps();
    for (auto& ap : animProps)
    {
        if (ap.doorType == 0)
            continue;

        bool isOpen = m_doorTypeOpen[ap.doorType];

        // Procedural grow (Druid's Isle vine bridges): ignore the .dat clip's motion and
        // just ramp growProgress toward the open/closed target. The renderer blends the
        // bone palette bind->built by this progress. On seek, snap to the target.
        if (ap.proceduralGrow)
        {
            float target = isOpen ? 1.0f : 0.0f;
            float dur = (ap.growDuration > 0.0001f) ? ap.growDuration : 1.5f;
            if (seeked)
            {
                ap.growProgress = target;
            }
            else if (ap.growProgress != target)
            {
                float step = frameDt / dur;
                if (ap.growProgress < target)
                    ap.growProgress = std::min(target, ap.growProgress + step);
                else
                    ap.growProgress = std::max(target, ap.growProgress - step);
            }
            continue;
        }

        bool stateChanged = seeked || (isOpen != prevOpen[ap.doorType]);

        if (stateChanged)
        {
            size_t targetSeg = isOpen ? ap.openSegmentIndex : ap.closeSegmentIndex;
            const auto& segments = ap.clip->animationSegments;

            // Doors whose closed pose is frame 0 of the open segment (Burning Isle):
            // hold at the open segment's start while closed, then play it once.
            if (ap.closedAtOpenStart && !isOpen)
            {
                if (ap.openSegmentIndex < segments.size())
                {
                    ap.controller->SetSegment(ap.openSegmentIndex);
                    ap.controller->SetLooping(false);
                    ap.controller->SetTime(
                        static_cast<float>(segments[ap.openSegmentIndex].startTime));
                    ap.controller->Pause();
                }
                continue;
            }

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
                if (ap.loopOnOpen && isOpen)
                {
                    ap.controller->SetLooping(true);
                    ap.controller->Play();
                }
                else
                {
                    ap.controller->SetTime(static_cast<float>(segments[targetSeg].endTime));
                    ap.controller->Pause();
                }
            }
            else
            {
                // Capture how far the currently-playing segment has progressed BEFORE
                // switching, so an interrupted swing can resume from its current angle
                // instead of snapping to the new segment's start pose.
                float curNorm = ap.controller->GetNormalizedTime();
                curNorm = std::clamp(curNorm, 0.0f, 1.0f);

                ap.controller->SetSegment(targetSeg);   // resets time to targetSeg start
                ap.controller->SetLooping(ap.loopOnOpen);

                // Smooth reverse for toggling doors (distinct open/close segments that run
                // in opposite directions): the open segment goes closed->open as it
                // progresses, the close segment open->closed, so the same physical openness
                // maps to (1 - progress) in the opposite segment. Seed the new segment's
                // time at that mirrored progress to continue from the current pose. Only for
                // doors with BOTH segments valid and distinct (e.g. the Frozen Isle lever
                // gates); one-way doors (closeSeg == open or SIZE_MAX) keep the old behavior.
                if (ap.openSegmentIndex != ap.closeSegmentIndex &&
                    ap.openSegmentIndex < segments.size() &&
                    ap.closeSegmentIndex < segments.size() &&
                    targetSeg < segments.size())
                {
                    float newNorm = 1.0f - curNorm;
                    const auto& seg = segments[targetSeg];
                    float t = static_cast<float>(seg.startTime)
                            + newNorm * static_cast<float>(seg.endTime - seg.startTime);
                    ap.controller->SetTime(t);
                }

                ap.controller->Play();
            }
        }

        if (m_replayCtx.isPlaying && ap.controller->IsPlaying())
            ap.controller->Update(frameDt * m_replayCtx.playbackSpeed);
    }
}
