#include "pch.h"
#include "ReplayWindow.h"
#include "AgentSnapshotParser.h"
#include "FontConfig.h"
#include "GuiGlobalConstants.h"
#include "MapBrowser.h"
#include "Parsers/BB9AnimationParser.h"
#include "ReplayWindow_Internal.h"
#include "RitualistAshes.h"
#include "MeshInstance.h"
#include "AMAT_file.h"
#include <algorithm>
#include <chrono>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Weapon models — which item an agent holds, where it attaches, and drawing it.
//
// Design notes live in gwobserver-private/docs/WeaponModelRendering.md, kept
// out of this repo because they detail Gw.dat internals. The attachment point
// comes from the rig's skeleton topology (Animation/WeaponSocket.h); the
// correction from bone space to grip is per weapon class and was measured
// against the render rather than derived, because the FA1 attachment block the
// game itself uses resists decoding.
// ---------------------------------------------------------------------------


// Ritualist urns are not equipment and never appear in the equipment stream, so they are
// preloaded by model id rather than discovered from one. They still need a grip, which is
// tuned under this synthetic item type — that is what lists "Ashes" in the editor below.
constexpr uint32_t kAshesItemType = 200;

// The 12 item types that reach a hand, in the order the editor lists them. Bundles (6) are out
// of scope; item_type 28 appears once in the sampled matches and is folded into the default.
static constexpr uint32_t kWeaponItemTypes[] = { 27, 2, 15, 32, 35, 36, 5, 26, 22, 12, 24, 28,
                                                 kAshesItemType };

// Guild Wars' own generic "item info not yet resolved" placeholder mesh — a flat grey diamond,
// used by the client whenever it draws an item before the server's appearance data for that
// specific instance has arrived. The recorder logs whatever model_file_id the client had cached
// at the instant it captured an ITEM_DEF, so a flag or bundle item that was captured before its
// info packet landed is stuck pointing at this placeholder for the rest of the replay — there is
// no later, better record to fall back to. Flags and repair kits both have one fixed, known skin,
// so patch the placeholder for those rather than rendering the diamond.
//
// Item type is not a reliable discriminator between the two: sampled repair kits report Bundle
// (6), sampled flags report 28, and the flag's own name never resolves ("Unknown") since it is
// picked up long before a normal item's tooltip would ever be requested. Both are folded in here
// and the actual Flag/RepairKit distinction is left to FlagItemRegistry::Classify, which reads
// the map id and FLAG_ITEM packets instead of anything on the item record.
constexpr uint32_t kUnresolvedItemPlaceholderModelId = 111902;
constexpr uint32_t kFlagModelFileId                  = 94192;
constexpr uint32_t kRepairKitModelFileId             = 31976;
constexpr uint32_t kBundleItemType                   = 6;
constexpr uint32_t kFlagItemType                     = 28;

// Several of these models carry a submesh that is not part of the item and must never
// be drawn — the repair kit's stand, and the urns' own extra piece.
static int HiddenSubmeshFor(uint32_t modelFileId)
{
    if (modelFileId == kRepairKitModelFileId) return 1;
    for (AshesKind kind : { AshesKind::Offensive, AshesKind::Defensive, AshesKind::Resurrect })
        if (modelFileId == AshesModelFileId(kind)) return AshesHiddenSubmesh(kind);
    return -1;
}

#if GWO_DEVELOPER
static const char* WeaponItemTypeName(uint32_t itemType)
{
    switch (itemType)
    {
        case 2:  return "Axe";
        case 5:  return "Bow";
        case 6:  return "Bundle";
        case 12: return "Focus";
        case 15: return "Hammer";
        case 22: return "Wand";
        case 24: return "Shield";
        case 26: return "Staff";
        case 27: return "Sword";
        case 28: return "Type 28";
        case 32: return "Daggers";
        case 35: return "Scythe";
        case 36: return "Spear";
        case kAshesItemType: return "Ashes";
        default: return "?";
    }
}
#endif


// Starting values for every hand-held type. Only the sword's has been measured against the
// render; the rest start from it and are tuned per class from Debug > Weapon Sockets.
void ReplayWindow::SeedWeaponGrips()
{
    if (!m_weaponGrips.empty()) return;

    for (uint32_t itemType : kWeaponItemTypes)
        m_weaponGrips[itemType] = WeaponGrip{};

    // A bow is a slot-0 weapon held in the hand that does *not* draw it. Every other main-hand
    // weapon goes in the weapon hand, and off-hand items reach the off hand through their slot.
    m_weaponGrips[5].forceOffHand = true;

    // Daggers are recorded as a single item but worn as a pair, and the off-hand slot is empty
    // while they are equipped, so the same skin is drawn on both hands.
    m_weaponGrips[32].mirrorToOffHand = true;

    // An urn is cradled in the hand that does not hold a weapon, the same way a bow is.
    m_weaponGrips[kAshesItemType] = WeaponGrip{};
    m_weaponGrips[kAshesItemType].forceOffHand = true;

    // Tuned against the render in Debug > Weapon Sockets. A scythe is the one class the
    // sword-derived default is badly wrong for: it is carried along the body rather than
    // seated upright in the fist, so it needs a long push down the shaft and a roll.
    m_weaponGrips[35].offset   = { -1.00f, -3.00f, -15.00f };
    m_weaponGrips[35].rotation = {  0.0f,   0.0f,   70.5f  };

    // A shield hangs off the forearm, not the palm, so it takes none of the default's drop.
    m_weaponGrips[24].offset   = { -2.00f,  0.00f,   0.00f };
}


// Ashes are not equipment: they arrive by cast and leave when dropped, so the
// answer comes from the reconstructed holds rather than from any item slot.
const AshesSkill* ReplayWindow::AshesHeldAt(int agentId, float time) const
{
    for (const auto& h : m_flagTimeline.ashesHolds)
    {
        if (h.carrierAgentId != agentId) continue;
        if (time < h.startTime) continue;
        if (h.endTime >= 0.f && time > h.endTime) continue;
        return LookupAshesSkill(h.skillId);
    }
    return nullptr;
}


const ReplayWindow::WeaponGrip& ReplayWindow::GripFor(uint32_t itemType) const
{
    auto it = m_weaponGrips.find(itemType);
    return (it != m_weaponGrips.end()) ? it->second : m_weaponGripDefault;
}


// Indexes EQUIP_SET/EQUIP_CLEAR by (agent, slot). The source events are already time-ordered,
// so each bucket comes out sorted and can be binary-searched. A cleared slot keeps its entry
// with ref -1, which Find() turns into nullptr — that is the difference between "unequipped at
// this time" and "no record", and both must resolve to no weapon.
void ReplayWindow::BuildEquipSlotTimeline() const
{
    if (m_equipSlotTimelineBuilt) return;
    m_equipSlotTimelineBuilt = true;

    for (const auto& ev : m_replayCtx.stocData.equipment.events)
    {
        const auto key = (static_cast<EquipSlotKey>(ev.agentId) << 8) | ev.slot;
        m_equipSlotTimeline[key].emplace_back(ev.time, ev.ref);
    }
}


// The item in one hand at one instant.
//
// The snapshot fields are the primary source: they are already time-indexed, and every distinct
// weapon_item_id across the recorded matches joins against the equipment stream (measured: 0
// misses over 41-58 agents per match). The EQUIP_SET walk is a fallback for agents whose
// snapshots carry no item ids at all, which is why it is only consulted when the id is zero.
ReplayWindow::EquippedWeapon ReplayWindow::ResolveEquippedWeapon(const AgentReplayData& ard,
                                                                 int snapIdx, uint8_t slot) const
{
    EquippedWeapon out;

    if (slot != Equipment::Slot_Weapon && slot != Equipment::Slot_Offhand)
        return out;
    if (snapIdx < 0 || snapIdx >= static_cast<int>(ard.snapshots.size()))
        return out;

    const auto& equipment = m_replayCtx.stocData.equipment;
    if (!equipment.loaded)
        return out;

    const auto& snap = ard.snapshots[snapIdx];
    const uint16_t agentItemId = (slot == Equipment::Slot_Weapon)
                                     ? snap.weapon_item_id
                                     : snap.offhand_item_id;

    if (agentItemId != 0)
    {
        out.item = equipment.FindByAgentItemId(agentItemId);
    }
    else
    {
        BuildEquipSlotTimeline();
        const auto it = m_equipSlotTimeline.find(
            (static_cast<EquipSlotKey>(ard.agent_id) << 8) | slot);
        if (it != m_equipSlotTimeline.end())
        {
            // Last entry at or before the snapshot time.
            const auto& entries = it->second;
            auto pos = std::upper_bound(entries.begin(), entries.end(), snap.time,
                [](float t, const std::pair<float, int>& e) { return t < e.first; });
            if (pos != entries.begin())
            {
                out.item = equipment.Find(std::prev(pos)->second);
                out.fromEventStream = (out.item != nullptr);
            }
        }
    }

    if (out.item)
    {
        out.modelFileId = out.item->modelFileId;
        out.itemType    = out.item->itemType;

        if ((out.itemType == kBundleItemType || out.itemType == kFlagItemType) &&
            (out.modelFileId == 0 || out.modelFileId == kUnresolvedItemPlaceholderModelId))
        {
            switch (m_flagItems.Classify(out.item->itemId, snap.time))
            {
                case BundleType::Flag:      out.modelFileId = kFlagModelFileId;      break;
                case BundleType::RepairKit: out.modelFileId = kRepairKitModelFileId; break;
                default: break;
            }
        }
    }
    return out;
}


// Every model_file_id the match ever puts in a hand, from both the event stream and the
// snapshots. Main thread only: it reads m_replayCtx, which the model loader thread must not
// touch (see the threading note on LoadWeaponModelsAsync).
void ReplayWindow::CollectHandHeldModelFileIds(std::vector<uint32_t>& out) const
{
    out.clear();

    std::unordered_set<uint32_t> unique;

    // Urns come from the skill stream, so they are collected even when the match has
    // no equipment stream at all.
    if (!m_flagTimeline.ashesHolds.empty())
        for (AshesKind kind : { AshesKind::Offensive, AshesKind::Defensive, AshesKind::Resurrect })
            unique.insert(AshesModelFileId(kind));

    const auto& equipment = m_replayCtx.stocData.equipment;
    if (!equipment.loaded) {
        out.assign(unique.begin(), unique.end());
        return;
    }

    for (const auto& ev : equipment.events)
    {
        if (ev.slot != Equipment::Slot_Weapon && ev.slot != Equipment::Slot_Offhand) continue;
        if (const auto* item = equipment.Find(ev.ref); item && item->modelFileId)
            unique.insert(item->modelFileId);
    }
    for (const auto& [agentId, ard] : m_replayCtx.agents)
    {
        for (const auto& snap : ard.snapshots)
            for (uint16_t id : { snap.weapon_item_id, snap.offhand_item_id })
                if (const auto* item = equipment.FindByAgentItemId(id); item && item->modelFileId)
                    unique.insert(item->modelFileId);
    }

    out.assign(unique.begin(), unique.end());
}


// ---------------------------------------------------------------------------
// Loading. A second background pass, deliberately separate from
// LoadAgentModelsIO: that thread starts as soon as agents are classified, which
// is before StoC parsing has necessarily published m_replayCtx.stocData, so
// reading the equipment stream from it would race the move-assign in
// PollStoCParseCompletion. Measured cost of this pass is ~25 ms for ~70 models,
// so preloading all of them unconditionally is cheaper than any lazy scheme —
// and lazy loading would hitch on every seek, since the replay is scrubbable.
// ---------------------------------------------------------------------------

void ReplayWindow::LoadWeaponModelsAsync()
{
    if (m_weaponModelsLoaded || m_weaponModelsLoading) return;
    if (!m_replayCtx.stocLoaded || !m_agentsClassified) return;
    // The urn models are collected from the reconstructed ashes holds, so the set to
    // preload is not complete until the flag timeline has been built. Both are gated
    // on the same two flags above, and the timeline is built earlier in the same
    // update, so this only ever waits a frame.
    if (!m_flagTimelineBuilt) return;
    if (!m_datManager || !m_hashIndex) return;

    CollectHandHeldModelFileIds(m_weaponModelOrder);
    if (m_weaponModelOrder.empty())
    {
        // No equipment stream, or nothing hand-held in it. Old replays land here and simply
        // render no weapons, which is the intended behaviour.
        m_weaponModelsLoaded = true;
        return;
    }

    m_weaponModelCreateIndex = 0;
    m_weaponBgLoadDone.store(false);
    m_weaponModelsLoading = true;
    m_weaponModelLoadThread = std::thread([this]() { LoadWeaponModelsIO(); });
}


void ReplayWindow::LoadWeaponModelsIO()
{
    const auto start = LoadClock::now();
    const auto& mft = m_datManager->get_MFT();

    for (uint32_t modelFileId : m_weaponModelOrder)
    {
        WeaponModelTemplate tmpl;

        auto hashIt = m_hashIndex->find(static_cast<int>(modelFileId));
        if (hashIt == m_hashIndex->end() || hashIt->second.empty())
        {
            m_weaponModelTemplates[modelFileId] = std::move(tmpl);
            continue;
        }

        const int mftIndex = hashIt->second.at(0);
        if (mftIndex < 0 || mftIndex >= static_cast<int>(mft.size()) ||
            mft[mftIndex].type != FFNA_Type2)
        {
            m_weaponModelTemplates[modelFileId] = std::move(tmpl);
            continue;
        }

        const bool isOtherFormat = m_datManager->is_other_model_format(mftIndex);
        FFNA_ModelFile modelFile;
        FFNA_ModelFile_Other modelFileOther;

        try
        {
            if (isOtherFormat) modelFileOther = m_datManager->parse_ffna_model_file_other(mftIndex);
            else               modelFile      = m_datManager->parse_ffna_model_file(mftIndex);
        }
        catch (...)
        {
            m_weaponModelTemplates[modelFileId] = std::move(tmpl);
            continue;
        }

        if (isOtherFormat ? !modelFileOther.parsed_correctly : !modelFile.parsed_correctly)
        {
            m_weaponModelTemplates[modelFileId] = std::move(tmpl);
            continue;
        }

        const auto& geoModels = isOtherFormat ? modelFileOther.geometry_chunk.models
                                              : modelFile.geometry_chunk.models;

        // Same material resolution the agent loader uses: the submesh's index selects an AMAT
        // out of the model's table, and without it the weapon renders untextured.
        for (size_t j = 0; j < geoModels.size(); j++)
        {
            if (static_cast<int>(j) == HiddenSubmeshFor(modelFileId))
                continue;

            AMAT_file amat;
            if (!isOtherFormat && !modelFile.AMAT_filenames_chunk.texture_filenames.empty())
            {
                const auto& geom = modelFile.geometry_chunk;
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
                    if (aIt != m_hashIndex->end() && !aIt->second.empty())
                        amat = m_datManager->parse_amat_file(aIt->second.at(0));
                }
            }

            Mesh mesh = isOtherFormat ? modelFileOther.GetMesh(static_cast<int>(j), amat)
                                      : modelFile.GetMesh(static_cast<int>(j), amat);
            if (!mesh.indices.empty() && mesh.indices.size() % 3 == 0)
                tmpl.meshes.push_back(std::move(mesh));
        }

        if (tmpl.meshes.empty())
        {
            m_weaponModelTemplates[modelFileId] = std::move(tmpl);
            continue;
        }

        tmpl.texturesOk = isOtherFormat ? modelFileOther.textures_parsed_correctly
                                        : modelFile.textures_parsed_correctly;
        if (tmpl.texturesOk)
        {
            std::vector<int> texFileHashes;
            if (isOtherFormat)
                for (const auto& tf : modelFileOther.texture_filenames_chunk.texture_filenames)
                    texFileHashes.push_back(decode_filename(tf.id0, tf.id1));
            else
                for (const auto& tf : modelFile.texture_filenames_chunk.texture_filenames)
                    texFileHashes.push_back(decode_filename(tf.id0, tf.id1));

            for (int decoded : texFileHashes)
            {
                ParsedTextureEntry pt;
                pt.decodedHash = decoded;
                auto mit = m_hashIndex->find(decoded);
                if (mit != m_hashIndex->end() && !mit->second.empty())
                {
                    try
                    {
                        pt.datTex = m_datManager->parse_ffna_texture_file(mit->second.at(0));
                        pt.hasDatTex = (pt.datTex.width > 0 && pt.datTex.height > 0);
                    }
                    catch (...) {}
                }
                tmpl.parsedTextures_.push_back(std::move(pt));
            }
        }

        const bool hasNewTexStuff = isOtherFormat
            ? !modelFileOther.geometry_chunk.unknown_tex_stuff1.empty()
            : !modelFile.geometry_chunk.unknown_tex_stuff1.empty();
        tmpl.pixelShaderType = hasNewTexStuff ? PixelShaderType::NewModel : PixelShaderType::OldModel;

        m_weaponModelTemplates[modelFileId] = std::move(tmpl);
    }

    OutputDebugStringA(std::format("[WeaponLoad] IO: {} models, {:.3f}s\n",
        m_weaponModelOrder.size(),
        std::chrono::duration<double>(LoadClock::now() - start).count()).c_str());

    m_weaponBgLoadDone.store(true);
}


void ReplayWindow::StepCreateWeaponModelResources()
{
    if (!m_weaponBgLoadDone || m_weaponModelsLoaded) return;

    auto* mapRenderer = m_mapRenderer.get();
    auto* device = m_deviceResources->GetD3DDevice();
    if (!mapRenderer || !device) return;

    constexpr int kWeaponBatchSize = 8;
    const auto frameStart = LoadClock::now();
    int processed = 0;

    while (m_weaponModelCreateIndex < static_cast<int>(m_weaponModelOrder.size()))
    {
        const uint32_t modelFileId = m_weaponModelOrder[m_weaponModelCreateIndex++];
        processed++;

        auto tmplIt = m_weaponModelTemplates.find(modelFileId);
        if (tmplIt == m_weaponModelTemplates.end()) continue;

        auto& tmpl = tmplIt->second;
        if (tmpl.meshes.empty()) continue;

        std::vector<int> textureIds;
        for (const auto& pt : tmpl.parsedTextures_)
        {
            int texId = mapRenderer->GetTextureManager()->GetTextureIdByHash(pt.decodedHash);
            if (texId < 0 && pt.hasDatTex)
                mapRenderer->GetTextureManager()->CreateTextureFromRGBA(
                    pt.datTex.width, pt.datTex.height, pt.datTex.rgba_data.data(),
                    &texId, pt.decodedHash);
            textureIds.push_back(texId);
        }

        tmpl.templateCBs.resize(tmpl.meshes.size());
        tmpl.gpuMeshes.reserve(tmpl.meshes.size());

        for (size_t k = 0; k < tmpl.meshes.size(); k++)
        {
            auto& mesh = tmpl.meshes[k];

            // Remap the mesh's texture indices onto the ids just created, exactly as the agent
            // path does: the mesh indexes the model's own texture table, not the manager's.
            std::vector<int> meshTexIds;
            std::vector<uint8_t> remapped;
            for (size_t ti = 0; ti < mesh.tex_indices.size(); ti++)
            {
                int idx = std::min(static_cast<int>(mesh.tex_indices[ti]),
                                   static_cast<int>(textureIds.size()) - 1);
                if (idx >= 0 && idx < static_cast<int>(textureIds.size()))
                {
                    meshTexIds.push_back(textureIds[idx]);
                    remapped.push_back(static_cast<uint8_t>(ti));
                }
            }
            mesh.tex_indices = remapped;

            PerObjectCB& cb = tmpl.templateCBs[k];
            XMStoreFloat4x4(&cb.world, XMMatrixIdentity());
            if (mesh.uv_coord_indices.size() == mesh.tex_indices.size() &&
                mesh.uv_coord_indices.size() < MAX_NUM_TEX_INDICES && tmpl.texturesOk)
            {
                cb.num_uv_texture_pairs = static_cast<uint32_t>(mesh.uv_coord_indices.size());
                for (size_t t = 0; t < mesh.uv_coord_indices.size(); t++)
                {
                    cb.uv_indices[t / 4][t % 4]      = mesh.uv_coord_indices[t];
                    cb.texture_indices[t / 4][t % 4] = mesh.tex_indices[t];
                    cb.blend_flags[t / 4][t % 4]     = mesh.blend_flags[t];
                    cb.texture_types[t / 4][t % 4]   = mesh.texture_types[t];
                }
            }

            auto instance = std::make_shared<MeshInstance>(device, mesh, static_cast<int>(k));
            if (tmpl.texturesOk && !meshTexIds.empty())
            {
                auto textures = mapRenderer->GetTextureManager()->GetTextures(meshTexIds);
                if (!textures.empty())
                    instance->SetTextures(textures, 3);
            }
            tmpl.gpuMeshes.push_back(std::move(instance));
        }

        tmpl.gpuReady = !tmpl.gpuMeshes.empty();

        // The CPU copies were only needed to build the buffers above.
        tmpl.meshes.clear();
        tmpl.meshes.shrink_to_fit();
        tmpl.parsedTextures_.clear();
        tmpl.parsedTextures_.shrink_to_fit();

        if (processed >= kWeaponBatchSize)
        {
            const auto elapsed =
                std::chrono::duration<float, std::milli>(LoadClock::now() - frameStart).count();
            if (elapsed > kLoadFrameBudgetMs) break;
        }
    }

    if (m_weaponModelCreateIndex >= static_cast<int>(m_weaponModelOrder.size()))
    {
        if (m_weaponModelLoadThread.joinable())
            m_weaponModelLoadThread.join();
        m_weaponModelsLoaded = true;
        m_weaponModelsLoading = false;
        OutputDebugStringA("[WeaponLoad] GPU resources ready\n");
    }
}


void ReplayWindow::ProgressiveWeaponModelPump()
{
    if (!m_showWeaponModels) return;
    if (m_weaponModelsLoaded) return;

    if (!m_weaponModelsLoading)
        LoadWeaponModelsAsync();

    if (m_weaponModelsLoading && m_weaponBgLoadDone)
        StepCreateWeaponModelResources();
}


// ---------------------------------------------------------------------------
// Per-frame draw. Runs immediately after DrawSkinnedAgentModels, which is what
// leaves the pose and the agent's world matrix in the state read here.
// ---------------------------------------------------------------------------

void ReplayWindow::DrawWeaponModels()
{
    if (!m_showWeaponModels || !m_weaponModelsLoaded) return;
    if (!m_useAgentModels || !m_agentModelsLoaded) return;
    if (m_weaponModelTemplates.empty() || m_agentAnimStates.empty()) return;

    auto* context = m_deviceResources->GetD3DDeviceContext();
    auto* meshManager = m_mapRenderer->GetMeshManager();
    if (!context || !meshManager) return;

    SeedWeaponGrips();

    bool shadersBound = false;
    int  boundPixelShader = -1;   // avoids a PSSetShader per weapon; most are OldModel

    for (auto& [slotKey, animState] : m_agentAnimStates)
    {
        if (!animState.hasSkinning || !animState.controller) continue;
        if (animState.perMeshCBs.empty()) continue;
        if (animState.lastSnapIdx < 0) continue;

        // Inherit the parent agent's visibility decision rather than re-deriving it: this is the
        // same gate DrawSkinnedAgentModels uses, and it already folds in fog, LOD, spirit
        // windows and minion death - and, for a player in an avatar form, which of their model
        // slots is the one being worn.
        auto statusIt = m_agentModelRenderStatus.find(slotKey);
        if (statusIt == m_agentModelRenderStatus.end() ||
            !statusIt->second.starts_with("skinned"))
            continue;

        // The socket comes from the rig actually on screen, so an avatar form grips its weapon
        // with the avatar's hand bones rather than the base skin's.
        const int agentId = SlotOwnerAgentId(slotKey);
        auto hashIt = m_agentFileHashCache.find(slotKey);
        if (hashIt == m_agentFileHashCache.end()) continue;
        auto tmplIt = m_agentModelTemplates.find(hashIt->second);
        if (tmplIt == m_agentModelTemplates.end()) continue;

        const auto& socket = tmplIt->second.weaponSocket;
        if (!socket.resolved) continue;

        const auto& bonePos = animState.controller->GetBoneWorldPositions();
        const auto& boneRot = animState.controller->GetBoneWorldRotations();
        if (!socket.IsValidFor(bonePos.size()) || boneRot.size() != bonePos.size()) continue;

        auto agentIt = m_replayCtx.agents.find(agentId);
        if (agentIt == m_replayCtx.agents.end()) continue;
        const auto& ard = agentIt->second;

        // The agent's own world matrix, exactly as DrawAgentModels built it — including the
        // centring translation and the fitHeight/nativeHeight renormalisation, both of which the
        // weapon has to ride or it will be the wrong size.
        const PerObjectCB& agentCB = animState.perMeshCBs[0];
        const XMMATRIX agentWorld = XMLoadFloat4x4(&agentCB.world);

        // Draws one weapon model on one hand bone. Bone pose is model space, so the weapon rides
        // the agent's world matrix unchanged. Row-vector convention: v * grip * hand * agentWorld.
        auto drawAtBone = [&](int32_t bone, WeaponModelTemplate& weaponTmpl, const WeaponGrip& gripDef)
        {
            if (bone < 0 || static_cast<size_t>(bone) >= bonePos.size()) return;

            const XMMATRIX grip =
                XMMatrixScaling(gripDef.scale, gripDef.scale, gripDef.scale)
              * XMMatrixRotationRollPitchYaw(XMConvertToRadians(gripDef.rotation.x),
                                             XMConvertToRadians(gripDef.rotation.y),
                                             XMConvertToRadians(gripDef.rotation.z))
              * XMMatrixTranslation(gripDef.offset.x, gripDef.offset.y, gripDef.offset.z);

            const XMMATRIX hand =
                XMMatrixRotationQuaternion(XMLoadFloat4(&boneRot[bone]))
              * XMMatrixTranslationFromVector(XMLoadFloat3(&bonePos[bone]));
            const XMMATRIX weaponWorld = grip * hand * agentWorld;

            if (!shadersBound)
            {
                m_mapRenderer->BindRegularVertexShader();
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                shadersBound = true;
            }
            const int wantPixelShader =
                (weaponTmpl.pixelShaderType == PixelShaderType::NewModel) ? 1 : 0;
            if (wantPixelShader != boundPixelShader)
            {
                m_mapRenderer->BindModelPixelShader(wantPixelShader == 1);
                boundPixelShader = wantPixelShader;
            }

            for (size_t k = 0; k < weaponTmpl.gpuMeshes.size(); k++)
            {
                if (!weaponTmpl.gpuMeshes[k]) continue;

                PerObjectCB cb = weaponTmpl.templateCBs[k];
                XMStoreFloat4x4(&cb.world, XMMatrixTranspose(weaponWorld));
                // Alpha and highlight come from the agent so the weapon fades with a corpse and
                // dims in fog. object_id is carried across for consistency, but note the skinned
                // agent pass leaves it at 0 — only the rigid AddProp path sets a real pick id,
                // so neither body nor weapon is pickable here.
                cb.mesh_alpha       = agentCB.mesh_alpha;
                cb.highlight_state  = agentCB.highlight_state;
                cb.object_id        = agentCB.object_id;

                meshManager->SetPerObjectCB(cb);
                weaponTmpl.gpuMeshes[k]->Draw(context, m_mapRenderer->GetLODQuality());
            }
        };

        // Resolve both hands before drawing either: the dagger mirror below needs to know
        // whether the off hand is already spoken for.
        const EquippedWeapon mainItem =
            ResolveEquippedWeapon(ard, animState.lastSnapIdx, Equipment::Slot_Weapon);
        const EquippedWeapon offItem =
            ResolveEquippedWeapon(ard, animState.lastSnapIdx, Equipment::Slot_Offhand);

        const int32_t mainBone = socket.MainHand(m_weaponHandIsPositiveX);
        const int32_t offBone  = socket.OffHand(m_weaponHandIsPositiveX);

        auto templateForModel = [&](uint32_t modelFileId) -> WeaponModelTemplate*
        {
            if (!modelFileId) return nullptr;
            auto it = m_weaponModelTemplates.find(modelFileId);
            return (it != m_weaponModelTemplates.end() && it->second.gpuReady) ? &it->second : nullptr;
        };
        auto templateFor = [&](const EquippedWeapon& w) -> WeaponModelTemplate*
        {
            return w.Valid() ? templateForModel(w.modelFileId) : nullptr;
        };

        // An urn replaces whatever was in the agent's hands. It also has to be asked
        // about before the equipment slots: while ashes are held the recorded item is
        // an unresolved bundle, which the placeholder patch above would otherwise turn
        // into a flag or a repair kit and put a banner in the ritualist's hands.
        if (const AshesSkill* ashes = AshesHeldAt(agentId, m_debugTimeline))
        {
            if (WeaponModelTemplate* urn = templateForModel(AshesModelFileId(ashes->kind)))
            {
                const WeaponGrip& gripDef = GripFor(kAshesItemType);
                drawAtBone(gripDef.forceOffHand ? offBone : mainBone, *urn, gripDef);
                continue;
            }
        }

        if (WeaponModelTemplate* mainTmpl = templateFor(mainItem))
        {
            const WeaponGrip& gripDef = GripFor(mainItem.itemType);

            // Slot decides the hand, except where the weapon class overrides it — a bow sits in
            // the hand that does not draw it.
            drawAtBone(gripDef.forceOffHand ? offBone : mainBone, *mainTmpl, gripDef);

            // A dual-wielded pair is one item record worn as two weapons. Only mirror when the
            // off hand is genuinely free, so an unexpected off-hand item is never drawn over.
            if (gripDef.mirrorToOffHand && !gripDef.forceOffHand && !offItem.Valid())
                drawAtBone(offBone, *mainTmpl, gripDef);
        }

        if (WeaponModelTemplate* offTmpl = templateFor(offItem))
            drawAtBone(offBone, *offTmpl, GripFor(offItem.itemType));
    }
}


#if GWO_DEVELOPER

// ---------------------------------------------------------------------------
// Phase 0.6 — load-side dry run.
//
// Walks every item the match ever puts in a hand, resolves it against the dat and parses the
// model, purely to measure what Phase 1 will have to preload. Results are reported and thrown
// away: committing to a cache layout before the renderer exists would be guessing.
//
// Runs on the main thread on demand rather than inside LoadAgentModelsIO. That loader thread
// starts as soon as agents are classified, which happens before StoC parsing has necessarily
// published m_replayCtx.stocData — so reading the equipment stream from it would race the
// move-assign in PollStoCParseCompletion. Phase 1's real preload inherits that constraint and
// will have to gate on stocLoaded or run as a second pass.
// ---------------------------------------------------------------------------

void ReplayWindow::ProbeWeaponModels()
{
    m_weaponPreloadStats = {};

    if (!m_replayCtx.stocLoaded)
        return;

    const auto& equipment = m_replayCtx.stocData.equipment;
    if (!equipment.loaded || !m_datManager || !m_hashIndex)
        return;

    const auto start = LoadClock::now();
    const auto& mft = m_datManager->get_MFT();

    // Every item that ever occupies a hand, from both the event stream and the snapshots. The
    // union is deliberate: this is measuring worst-case preload, not per-frame lookup.
    std::unordered_set<uint32_t> modelFileIds;
    for (const auto& ev : equipment.events)
    {
        if (ev.slot != Equipment::Slot_Weapon && ev.slot != Equipment::Slot_Offhand) continue;
        if (const auto* item = equipment.Find(ev.ref); item && item->modelFileId)
            modelFileIds.insert(item->modelFileId);
    }
    for (const auto& [agentId, ard] : m_replayCtx.agents)
    {
        for (const auto& snap : ard.snapshots)
        {
            for (uint16_t id : { snap.weapon_item_id, snap.offhand_item_id })
                if (const auto* item = equipment.FindByAgentItemId(id); item && item->modelFileId)
                    modelFileIds.insert(item->modelFileId);
        }
    }

    m_weaponPreloadStats.distinctModels = static_cast<int>(modelFileIds.size());

    for (uint32_t modelFileId : modelFileIds)
    {
        auto hashIt = m_hashIndex->find(static_cast<int>(modelFileId));
        if (hashIt == m_hashIndex->end() || hashIt->second.empty())
            continue;

        const int mftIndex = hashIt->second.at(0);
        if (mftIndex < 0 || mftIndex >= static_cast<int>(mft.size()) ||
            mft[mftIndex].type != FFNA_Type2)
            continue;

        m_weaponPreloadStats.resolvedModels++;

        try
        {
            const bool isOtherFormat = m_datManager->is_other_model_format(mftIndex);
            if (isOtherFormat)
            {
                auto modelFile = m_datManager->parse_ffna_model_file_other(mftIndex);
                if (modelFile.parsed_correctly && !modelFile.geometry_chunk.models.empty())
                    m_weaponPreloadStats.parsedOk++;
            }
            else
            {
                auto modelFile = m_datManager->parse_ffna_model_file(mftIndex);
                if (modelFile.parsed_correctly && !modelFile.geometry_chunk.models.empty())
                    m_weaponPreloadStats.parsedOk++;
            }
        }
        catch (...) {}
    }

    m_weaponPreloadStats.parseSeconds =
        std::chrono::duration<double>(LoadClock::now() - start).count();
    m_weaponPreloadStats.ran = true;

    OutputDebugStringA(std::format(
        "[WeaponProbe] {} distinct models, {} resolved, {} parsed, {:.3f}s\n",
        m_weaponPreloadStats.distinctModels, m_weaponPreloadStats.resolvedModels,
        m_weaponPreloadStats.parsedOk, m_weaponPreloadStats.parseSeconds).c_str());
}


// ---------------------------------------------------------------------------
// Phase 0.5 — diagnostics window.
// ---------------------------------------------------------------------------

void ReplayWindow::DrawWeaponSocketWindow()
{
    ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Weapon Sockets", &m_showWeaponSocketWindow))
    {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Render weapon models", &m_showWeaponModels);

    ImGui::Checkbox("Weapon hand is +x in bind pose", &m_weaponHandIsPositiveX);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The two hand bones are mirror images, so which one holds the weapon\n"
                          "cannot be read from the rig. Flip this and watch the render:\n"
                          "the shield is the tell, it always goes in the off hand.");

    // --- Grip correction, per item type ----------------------------------
    SeedWeaponGrips();

    if (ImGui::CollapsingHeader("Grip correction", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("Maps weapon-local space onto the hand bone's frame, per weapon class.");

        if (ImGui::BeginCombo("Editing", WeaponItemTypeName(m_weaponGripEditType)))
        {
            for (uint32_t itemType : kWeaponItemTypes)
                if (ImGui::Selectable(WeaponItemTypeName(itemType), itemType == m_weaponGripEditType))
                    m_weaponGripEditType = itemType;
            ImGui::EndCombo();
        }

        WeaponGrip& g = m_weaponGrips[m_weaponGripEditType];
        ImGui::Checkbox("Force off hand", &g.forceOffHand);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Attach to the off hand even though this is a slot-0 weapon.\n"
                              "True for bows: the archer holds the bow in one hand and draws\n"
                              "with the other.");

        ImGui::DragFloat3("Offset",   &g.offset.x,   0.05f, -80.f, 80.f, "%.2f");
        ImGui::DragFloat3("Rotation", &g.rotation.x, 0.5f, -180.f, 180.f, "%.1f deg");
        ImGui::DragFloat ("Scale",    &g.scale,      0.005f, 0.05f, 5.f, "%.3f");

        if (ImGui::Button("Reset this type"))
            g = WeaponGrip{};
        ImGui::SameLine();
        if (ImGui::Button("Copy all as C++"))
        {
            std::string out = "// Measured against the render; see the weapon-rendering notes.\n";
            for (uint32_t itemType : kWeaponItemTypes)
            {
                const WeaponGrip& w = m_weaponGrips[itemType];
                out += std::format("m_weaponGrips[{}] = {{ {}, {}, {{{:.2f}f, {:.2f}f, {:.2f}f}}, "
                                   "{{{:.1f}f, {:.1f}f, {:.1f}f}}, {:.3f}f }};  // {}\n",
                                   itemType, w.forceOffHand ? "true" : "false",
                                   w.mirrorToOffHand ? "true" : "false",
                                   w.offset.x, w.offset.y, w.offset.z,
                                   w.rotation.x, w.rotation.y, w.rotation.z,
                                   w.scale, WeaponItemTypeName(itemType));
            }
            ImGui::SetClipboardText(out.c_str());
        }
        ImGui::SameLine();
        // Only safe to size the map once the IO thread has finished inserting into it.
        if (m_weaponModelsLoaded)
            ImGui::TextDisabled("%d models", static_cast<int>(m_weaponModelTemplates.size()));
        else if (m_weaponModelsLoading)
            ImGui::TextDisabled("loading %d...", static_cast<int>(m_weaponModelOrder.size()));
        else
            ImGui::TextDisabled("not loaded");
    }

    ImGui::Separator();

    // --- Preload dry run -------------------------------------------------
    if (ImGui::CollapsingHeader("Preload dry run", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const bool ready = m_replayCtx.stocLoaded && m_replayCtx.stocData.equipment.loaded;
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Run dry run"))
            ProbeWeaponModels();
        ImGui::EndDisabled();
        if (!ready)
        {
            ImGui::SameLine();
            ImGui::TextDisabled(m_replayCtx.stocLoaded ? "(no equipment stream in this match)"
                                                       : "(StoC still loading)");
        }

        const auto& s = m_weaponPreloadStats;
        if (!s.ran)
        {
            ImGui::TextDisabled("Not measured yet.");
        }
        else
        {
            ImGui::Text("Distinct hand-held models : %d", s.distinctModels);
            ImGui::Text("Resolved to FFNA_Type2    : %d", s.resolvedModels);
            ImGui::Text("Parsed with geometry      : %d", s.parsedOk);
            ImGui::Text("Parse time                : %.3f s", s.parseSeconds);
            if (s.distinctModels > 0 && s.resolvedModels < s.distinctModels)
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
                                   "%d model ids did not resolve", s.distinctModels - s.resolvedModels);
        }
    }

    ImGui::Separator();

    // --- Focused agent ---------------------------------------------------
    const int agentId = (m_followedAgentId >= 0) ? m_followedAgentId
                      : (m_playerInfoAgentId >= 0) ? m_playerInfoAgentId
                      : m_hoveredAgentId;

    if (agentId < 0)
    {
        ImGui::TextWrapped("Follow a player, open a player info panel, or hover an agent.");
        ImGui::End();
        return;
    }

    auto agentIt = m_replayCtx.agents.find(agentId);
    if (agentIt == m_replayCtx.agents.end() || agentIt->second.snapshots.empty())
    {
        ImGui::TextWrapped("Agent %d has no snapshots.", agentId);
        ImGui::End();
        return;
    }

    const auto& ard = agentIt->second;
    const int snapIdx = FindSnapshotIndex(ard.snapshots, m_debugTimeline);

    ImGui::Text("Agent %d  %s", agentId,
                ard.playerName.empty() ? ard.categoryName.c_str() : ard.playerName.c_str());

    // --- Rig -------------------------------------------------------------
    // Follow the model slot on screen: while an avatar form is up, the rig, socket and live
    // bones all belong to the avatar, and reporting the base skin's would be misleading.
    uint32_t wornAvatar = ard.modelIdAtTime(m_debugTimeline);
    if (LookupAvatarFileHash(wornAvatar) == 0) wornAvatar = 0;
    const int slotKey = wornAvatar ? AvatarSlotKey(agentId, wornAvatar) : agentId;

    auto hashIt = m_agentFileHashCache.find(slotKey);
    const uint32_t fileHash = (hashIt != m_agentFileHashCache.end()) ? hashIt->second : 0;
    auto tmplIt = m_agentModelTemplates.find(fileHash);

    if (tmplIt == m_agentModelTemplates.end())
    {
        ImGui::TextDisabled("No model template loaded for this agent.");
    }
    else
    {
        const auto& tmpl = tmplIt->second;
        ImGui::Text("Rig  hash0=0x%08X  hash1=0x%08X  clips=%zu",
                    tmpl.modelHash0, tmpl.modelHash1, tmpl.allClips.size());

        auto animIt = m_agentAnimStates.find(slotKey);
        const GW::Animation::AnimationController* ctrl =
            (animIt != m_agentAnimStates.end() && animIt->second.controller)
                ? animIt->second.controller.get() : nullptr;

        const size_t liveBoneCount = ctrl ? ctrl->GetBoneWorldPositions().size() : 0;
        ImGui::Text("Bones  template=%zu  live=%zu  clipIndex=%d",
                    tmpl.clip ? tmpl.clip->boneTracks.size() : 0, liveBoneCount,
                    (animIt != m_agentAnimStates.end()) ? animIt->second.currentClipIndex : -1);

        // --- Socket ------------------------------------------------------
        const auto& socket = tmpl.weaponSocket;
        if (!socket.resolved)
        {
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
                               "Socket unresolved — rig is not humanoid, no weapon would render.");
        }
        else
        {
            const int32_t mainBone = socket.MainHand(m_weaponHandIsPositiveX);
            const int32_t offBone  = socket.OffHand(m_weaponHandIsPositiveX);
            ImGui::Text("Socket  -x bone=%d   +x bone=%d   ->  main=%d  off=%d",
                        socket.negativeXBone, socket.positiveXBone, mainBone, offBone);

            if (liveBoneCount && !socket.IsValidFor(liveBoneCount))
            {
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                                   "Socket indices are out of range for the live clip (%zu bones).",
                                   liveBoneCount);
            }
            else if (ctrl && liveBoneCount)
            {
                const auto& pos = ctrl->GetBoneWorldPositions();
                const auto& rot = ctrl->GetBoneWorldRotations();
                if (ImGui::BeginTable("handpose", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Hand");
                    ImGui::TableSetupColumn("Model-space position");
                    ImGui::TableSetupColumn("Rotation (x,y,z,w)");
                    ImGui::TableHeadersRow();
                    for (auto [label, bone] : { std::pair{"main", mainBone}, std::pair{"off", offBone} })
                    {
                        if (bone < 0 || static_cast<size_t>(bone) >= liveBoneCount) continue;
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::Text("%s [%d]", label, bone);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f, %.2f, %.2f", pos[bone].x, pos[bone].y, pos[bone].z);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f, %.3f, %.3f, %.3f",
                                    rot[bone].x, rot[bone].y, rot[bone].z, rot[bone].w);
                    }
                    ImGui::EndTable();
                }
            }
        }
    }

    ImGui::Separator();

    // --- Equipped items --------------------------------------------------
    const EquippedWeapon main = ResolveEquippedWeapon(ard, snapIdx, Equipment::Slot_Weapon);
    const EquippedWeapon off  = ResolveEquippedWeapon(ard, snapIdx, Equipment::Slot_Offhand);

    auto slotRow = [](const char* label, const EquippedWeapon& w)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(label);

        if (!w.item)
        {
            ImGui::TableNextColumn(); ImGui::TextDisabled("(empty)");
            ImGui::TableNextColumn(); ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextDisabled("-");
            return;
        }

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(w.item->name.empty() ? "(unnamed)" : w.item->name.c_str());
        if (w.fromEventStream && ImGui::IsItemHovered())
            ImGui::SetTooltip("Resolved from EQUIP_SET, not from the snapshot");

        ImGui::TableNextColumn();
        ImGui::Text("%s (%u)", WeaponItemTypeName(w.itemType), w.itemType);

        ImGui::TableNextColumn();
        ImGui::Text("%u", w.modelFileId);
    };

    if (ImGui::BeginTable("equipped", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Slot");
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("model_file_id");
        ImGui::TableHeadersRow();
        slotRow("Main", main);
        slotRow("Off",  off);
        ImGui::EndTable();
    }

    if (main.Valid())
    {
        if (ImGui::Button("Tune this weapon's type"))
            m_weaponGripEditType = main.itemType;
        ImGui::SameLine();
        const WeaponGrip& g = GripFor(main.itemType);
        ImGui::TextDisabled("%s -> %s hand, offset (%.2f, %.2f, %.2f)",
                            WeaponItemTypeName(main.itemType),
                            g.forceOffHand ? "off" : "weapon",
                            g.offset.x, g.offset.y, g.offset.z);
    }

    // --- Animation diagnostics -------------------------------------------
    // Two-handed weapons only look right when the rig is playing the animation authored for
    // that weapon class: a hammer run puts both hands on the shaft, a generic run does not. No
    // grip offset can compensate for the wrong animation, so this distinguishes the two.
    {
        auto animIt = m_agentAnimStates.find(slotKey);
        if (animIt != m_agentAnimStates.end())
        {
            const auto& st = animIt->second;
            const auto& snap = ard.snapshots[snapIdx];
            ImGui::Text("Animation  code=0x%08X  clip=%d", snap.animation_code, st.currentClipIndex);
            ImGui::SameLine();
            if (st.lastLookupFailed)
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "UNRESOLVED");
            else if (st.isPlayingMovementAnim)
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "generic movement fallback");
            else if (st.isPlayingIdleAnim)
                ImGui::TextDisabled("generic idle fallback");
            else
                ImGui::TextColored(ImVec4(0.6f, 1.f, 0.6f, 1.f), "server animation");
        }
    }

    // --- Model resolution for the equipped items -------------------------
    for (auto [label, weapon] : { std::pair{"Main", &main}, std::pair{"Off", &off} })
    {
        if (!weapon->Valid()) continue;
        if (!m_datManager || !m_hashIndex) break;

        auto it = m_hashIndex->find(static_cast<int>(weapon->modelFileId));
        if (it == m_hashIndex->end() || it->second.empty())
        {
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                               "%s: model_file_id %u is not in the dat", label, weapon->modelFileId);
            continue;
        }

        const int mftIndex = it->second.at(0);
        const auto& mft = m_datManager->get_MFT();
        if (mftIndex < 0 || mftIndex >= static_cast<int>(mft.size()))
            continue;

        const bool isModel = (mft[mftIndex].type == FFNA_Type2);
        ImGui::TextColored(isModel ? ImVec4(0.6f, 1.f, 0.6f, 1.f) : ImVec4(1.f, 0.4f, 0.4f, 1.f),
                           "%s: mft=%d  type=%d%s  size=%d",
                           label, mftIndex, mft[mftIndex].type,
                           isModel ? " (FFNA_Type2)" : "", mft[mftIndex].uncompressedSize);
    }

    ImGui::End();
}

#endif  // GWO_DEVELOPER
