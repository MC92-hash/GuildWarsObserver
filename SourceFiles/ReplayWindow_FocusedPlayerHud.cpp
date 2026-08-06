#include "pch.h"
#include "ReplayWindow.h"
#include "EquipmentIcons.h"
#include "ReplayWindow_Internal.h"
#include <algorithm>

// ---------------------------------------------------------------------------
// Focused-player HUD
// ---------------------------------------------------------------------------
// Screen-space overlay that only exists while the camera is following a player, the
// companion to DrawFollowedAgentHUD's health/cast bars at the top of the screen. The
// game shows the followed character's own panels; this rebuilds the ones an observer
// cares about from the recording.
//
// Weapon sets are the first element. The live client is capped at four slots (F1-F4);
// nothing caps us here, so every set BuildWeaponSets recovered gets a circle.
// ---------------------------------------------------------------------------


namespace
{
    // texture_153611 is the game's own weapon-slot backing plate: one 128x64 sheet holding
    // two 64x64 cells side by side, the idle slot on the left and the lit one on the right.
    // The plate itself is an ellipse filling only the top 56 rows of each cell, so the V range
    // stops there and slots are drawn at that aspect - stretching to a square deforms it.
    constexpr float kPlateV      = 56.f / 64.f;
    constexpr float kPlateAspect = 56.f / 64.f;   // height / width
    constexpr ImVec2 kSlotIdleUV0(0.0f, 0.0f), kSlotIdleUV1(0.5f, kPlateV);
    constexpr ImVec2 kSlotLitUV0 (0.5f, 0.0f), kSlotLitUV1 (1.0f, kPlateV);

    // Measured optical centre of the ellipse within that 64x56 crop (its alpha centroid sits
    // at row 27.1, a touch above the geometric middle). Icons centre on this, not on the rect.
    constexpr float kPlateCentreV = 27.1f / 56.f;

    constexpr float kSlotW     = 54.f;   // idle plate width
    constexpr float kActiveMul = 1.24f;  // the equipped set reads slightly larger
    constexpr float kGap       = 2.f;    // plates sit shoulder to shoulder, as in the client
    constexpr float kIconMul   = 1.18f;  // icons overhang the plate, as in the client
    constexpr float kMargin    = 14.f;
    constexpr float kPlayBarH  = 76.f;   // DrawTimelineController's bar height

    float PlateH(float plateW)  { return plateW * kPlateAspect; }
    float IconSize(float plateW) { return PlateH(plateW) * kIconMul; }

    // Decoded once and held for the lifetime of the device, as the other DDS loaders do.
    ImTextureID LoadWeaponSlotCircle(ID3D11Device* device)
    {
        static ID3D11Device* s_cachedDevice = nullptr;
        static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> s_srv;
        static bool s_failed = false;

        if (device != s_cachedDevice)
        {
            s_srv.Reset();
            s_failed = false;
            s_cachedDevice = device;
        }
        if (s_srv.Get()) return (ImTextureID)s_srv.Get();
        if (s_failed || !device) return nullptr;
        s_failed = true; // cleared again only on success, so a missing file is not retried per frame

        auto ddsDir = FindTexturesDDSDir();
        if (ddsDir.empty()) return nullptr;

        auto fullPath = ddsDir / L"texture_153611.dds";
        if (!std::filesystem::exists(fullPath)) return nullptr;

        DirectX::ScratchImage image;
        HRESULT hr = DirectX::LoadFromDDSFile(fullPath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image);
        if (FAILED(hr)) return nullptr;

        const auto& meta = image.GetMetadata();
        if (meta.width == 0 || meta.height == 0) return nullptr;

        DirectX::ScratchImage decompressed;
        if (DirectX::IsCompressed(meta.format))
        {
            hr = DirectX::Decompress(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM, decompressed);
            if (FAILED(hr)) return nullptr;
            image = std::move(decompressed);
        }

        DirectX::ScratchImage converted;
        if (image.GetMetadata().format != DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
                DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
            if (FAILED(hr)) return nullptr;
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

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = img->pixels;
        initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        hr = device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
        if (FAILED(hr)) return nullptr;

        hr = device->CreateShaderResourceView(tex.Get(), nullptr, s_srv.GetAddressOf());
        if (FAILED(hr)) { s_srv.Reset(); return nullptr; }

        s_failed = false;
        return (ImTextureID)s_srv.Get();
    }
}


void ReplayWindow::DrawFocusedPlayerHud()
{
    if (!m_showFocusHud) return;

    // Nothing to anchor to when the camera is free — the whole HUD disappears with the follow.
    const int focused = GetFocusedAgentId();
    if (focused < 0) return;

    auto it = m_replayCtx.agents.find(focused);
    if (it == m_replayCtx.agents.end()) return;
    if (it->second.type != AgentType::Player) return;

    DrawFocusHudWeaponSets(focused);
}


void ReplayWindow::DrawFocusHudWeaponSets(int agentId)
{
    auto agentIt = m_replayCtx.agents.find(agentId);
    if (agentIt == m_replayCtx.agents.end()) return;
    const AgentReplayData& ard = agentIt->second;

    if (m_hudWeaponSets.agentId != agentId)
        BuildWeaponSets(agentId, m_hudWeaponSets);
    if (m_hudWeaponSets.sets.empty()) return;

    const int setCount = static_cast<int>(m_hudWeaponSets.sets.size());

    // The equipped set is identified the same way the player info panel does it: a set is
    // "current" when both of its item ids match what the agent is holding right now.
    const AgentSnapshot* snap = FindSnapshotAtTime(ard, m_debugTimeline);
    int activeIdx = -1;
    if (snap)
    {
        for (int i = 0; i < setCount; i++)
        {
            const auto& ws = m_hudWeaponSets.sets[i];
            if (ws.mainId == snap->weapon_item_id && ws.offId == snap->offhand_item_id)
            { activeIdx = i; break; }
        }
    }

    // Plates are laid out on a fixed pitch and share one baseline; the enlarged active plate
    // and the overhanging icons spill past that pitch, so the window carries padding wide
    // enough for the worst case (an end slot being the active one) and nothing shifts when
    // the player swaps sets.
    const float activeW = kSlotW * kActiveMul;
    const float activeH = PlateH(activeW);
    const float activeIcon = IconSize(activeW);

    const float padX = std::max(0.f, std::max(activeW, activeIcon) * 0.5f - kSlotW * 0.5f);
    // Icons centre on the plate's optical centre, which sits above the plate's own bottom
    // edge — hence the separate above/below reach.
    const float aboveBaseline = std::max(activeH, activeH * (1.f - kPlateCentreV) + activeIcon * 0.5f);
    const float belowBaseline = std::max(0.f, activeIcon * 0.5f - activeH * (1.f - kPlateCentreV));

    const float winW = 2.f * padX + setCount * kSlotW + (setCount - 1) * kGap;
    const float winH = aboveBaseline + belowBaseline;

    const ImGuiViewport* vp = ImGui::GetMainViewport();

    // Anchored, not remembered: recomputed from the viewport every frame so a window resize
    // keeps it bottom-right and on screen. It clears the play bar, and defers to the event
    // timeline strip when that is up, the way the followed agent's health bar defers to the
    // ribbon at the top of the screen.
    const float floorY = std::min(vp->Pos.y + vp->Size.y - kPlayBarH, m_eventTimelineTopY);
    const float posX = std::max(vp->Pos.x, vp->Pos.x + vp->Size.x - winW - kMargin);
    const float posY = std::max(vp->Pos.y, floorY - kMargin - winH);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);

    ImGui::SetNextWindowPos(ImVec2(posX, posY));
    ImGui::SetNextWindowSize(ImVec2(winW, winH));

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoResize        |
        ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoScrollbar     |
        ImGuiWindowFlags_NoCollapse   | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing;

    if (!ImGui::Begin("##focus_hud_weapon_sets", nullptr, kFlags))
    {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImTextureID circleTex = LoadWeaponSlotCircle(dev);

    const ImVec2 origin = ImGui::GetWindowPos();
    const float baselineY = origin.y + aboveBaseline;
    const bool windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);

    // Icons overhang their plate and so can overlap a neighbour's; resolving the hover up
    // front keeps two adjacent slots from both opening a tooltip.
    auto slotCentreX = [&](int i) {
        return origin.x + padX + kSlotW * 0.5f + i * (kSlotW + kGap);
    };
    int hoveredIdx = -1;
    if (windowHovered)
    {
        for (int i = 0; i < setCount; i++)
        {
            const float slotW = (i == activeIdx) ? activeW : kSlotW;
            const float slotH = PlateH(slotW);
            const float cx = slotCentreX(i);
            // The plate is the hit target; the overhanging art is decoration.
            if (ImGui::IsMouseHoveringRect(ImVec2(cx - slotW * 0.5f, baselineY - slotH),
                                           ImVec2(cx + slotW * 0.5f, baselineY)))
            { hoveredIdx = i; break; }
        }
    }

    for (int i = 0; i < setCount; i++)
    {
        const auto& ws = m_hudWeaponSets.sets[i];
        const bool active = (i == activeIdx);
        const bool hovered = (i == hoveredIdx);

        const float slotW = active ? activeW : kSlotW;
        const float slotH = PlateH(slotW);
        const float centreX = slotCentreX(i);
        // Bottom-aligned so the enlarged plate grows upward off a common baseline.
        const ImVec2 slotTL(centreX - slotW * 0.5f, baselineY - slotH);
        const ImVec2 slotBR(centreX + slotW * 0.5f, baselineY);

        if (circleTex)
        {
            const bool lit = active || hovered;
            dl->AddImage(circleTex, slotTL, slotBR,
                         lit ? kSlotLitUV0 : kSlotIdleUV0,
                         lit ? kSlotLitUV1 : kSlotIdleUV1,
                         active ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 224));
        }
        else
        {
            // Backing plate missing from Textures/DDS: a plain disc still reads as a slot.
            const ImVec2 c((slotTL.x + slotBR.x) * 0.5f, (slotTL.y + slotBR.y) * 0.5f);
            dl->AddCircleFilled(c, slotH * 0.5f, IM_COL32(24, 26, 30, 200));
            dl->AddCircle(c, slotH * 0.5f, active ? IM_COL32(212, 160, 32, 255)
                                                  : IM_COL32(255, 255, 255, 46), 0, 1.5f);
        }

        // Icons: the category art from ResolveWeaponTextures, overridden by the item's real
        // skin from Gw.dat wherever the recording resolved one — same order as the panel.
        WeaponTextureResult wtr = ResolveWeaponTextures(ws.weapCat, ws.mainType,
                                                        ard.primaryProf, ard.teamId, ws.bundleType);
        ImTextureID mainTex = nullptr;
        if (wtr.mainTex)
        {
            if (wtr.isNPCIcon)   mainTex = LoadNPCIcon(dev, wtr.mainTex);
            else if (wtr.isFlag) mainTex = LoadFlagIcon(dev, wtr.mainTex);
            else                 mainTex = LoadWeaponTexture(dev, wtr.mainTex);
        }
        ImTextureID offTex = wtr.offTex ? LoadWeaponTexture(dev, wtr.offTex) : nullptr;

        const bool isBundle = (ws.bundleType != BundleType::Unknown);
        if (!isBundle)
        {
            const auto& equipment = m_replayCtx.stocData.equipment;
            if (const auto* mainItem = equipment.FindByAgentItemId(ws.mainId))
            {
                if (ImTextureID skin = EquipmentIcons::Get(m_datManager, dev, mainItem->modelFileId))
                    mainTex = skin;
            }
            if (const auto* offItem = equipment.FindByAgentItemId(ws.offId))
            {
                if (ImTextureID skin = EquipmentIcons::Get(m_datManager, dev, offItem->modelFileId))
                    offTex = skin;
            }
        }

        // Square icon centred on the plate's optical centre and deliberately larger than the
        // plate, so the weapon overhangs the rim the way it does in the client.
        const float iconArea = IconSize(slotW);
        const ImVec2 centre(centreX, slotTL.y + slotH * kPlateCentreV);

        // Both halves of a pair are placed from their own centres, so the composition stays
        // balanced on the plate instead of drifting up-left.
        auto blit = [&](ImTextureID tex, float sz, float dx, float dy, ImU32 tint) {
            dl->AddImage(tex, ImVec2(centre.x + dx - sz * 0.5f, centre.y + dy - sz * 0.5f),
                              ImVec2(centre.x + dx + sz * 0.5f, centre.y + dy + sz * 0.5f),
                         ImVec2(0, 0), ImVec2(1, 1), tint);
        };

        if (mainTex && offTex)
        {
            const float d = iconArea * 0.07f;
            // Offhand behind and down-right, main hand in front and up-left.
            blit(offTex, iconArea * 0.85f, d, d, IM_COL32(255, 255, 255, 217));
            blit(mainTex, iconArea, -d, -d, IM_COL32(255, 255, 255, 255));
        }
        else if (mainTex)
        {
            blit(mainTex, iconArea, 0.f, 0.f, IM_COL32(255, 255, 255, 255));
        }
        else
        {
            const char* q = "?";
            const ImVec2 qSz = ImGui::CalcTextSize(q);
            const float qScale = 13.f / ImGui::GetFontSize();
            dl->AddText(nullptr, 13.f,
                ImVec2(centre.x - qSz.x * qScale * 0.5f, centre.y - qSz.y * qScale * 0.5f),
                IM_COL32(0x50, 0x5a, 0x64, 0xFF), q);
        }

        // Slot number where the client prints the F-key, so several similar sets stay apart.
        char slotLabel[8];
        snprintf(slotLabel, sizeof(slotLabel), "%d", i + 1);
        const ImVec2 lPos(slotTL.x + slotW * 0.15f, slotBR.y - slotH * 0.30f);
        dl->AddText(nullptr, 10.f, ImVec2(lPos.x + 1.f, lPos.y + 1.f),
                    IM_COL32(0, 0, 0, 160), slotLabel);
        dl->AddText(nullptr, 10.f, lPos,
                    active ? IM_COL32(0xE2, 0xC2, 0x6A, 0xFF) : IM_COL32(0xC0, 0xC4, 0xC8, 0xE0),
                    slotLabel);

        // Drawn straight into the draw list rather than as ImGui items, so the panel stays
        // draggable anywhere and hovering is tested against the rect directly.
        if (hovered)
            DrawWeaponSetTooltip(ws);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}
