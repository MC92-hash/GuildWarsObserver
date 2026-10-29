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
    if (it->second.type != AgentType::Player && !IsHenchmanAgent(it->second)) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    m_focusHudRightLimitX = vp->Pos.x + vp->Size.x - kMargin;   // the weapon sets lower it when shown
    DrawFocusHudWeaponSets(focused);
    DrawFocusHudSkillBar(focused);
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
    m_focusHudRightLimitX = posX;

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


// ---------------------------------------------------------------------------
// Skill bar: the followed player's eight skills, bottom centre, drawn with the client's own art
// and recipe (GmSkillBar / GmSkSlot, decoded from Gw.exe 2026-10-05; DAT ids below). Every slot
// reads its state from ComputeSkillCooldowns -- the same model as the player info panel -- so the
// two can never disagree about what is up.
//
//   frame    143021 SkillBarLockedHorz (the in-instance art), 9-slice, art rect = slots grown
//            16 px left/right, 7 above, 11 below; slots touch, no spacing
//   slot     icon UV 0.0625..0.9375 (its inner 56x56) under frame cell 0 of 265556, cell 1 = elite
//   empty    solid 0xC0404040 under frame cell 0
//   recharge black 0xA0 fan: the wedge still to come back, edge sweeping clockwise from 12
//            o'clock, measured against max(base recharge, remaining); no countdown, no flash
//   disabled the bright red no-sign, cell 3 of 157942, stretched over the icon
//   number   keycap image from 143032 (24x24 at 1x), bottom right
//   marker   164476, top right at 32/56 of the icon: hex, enchantment, weapon spell, lead,
//            off-hand, dual
//   in use   143774, 16 frames at 16 fps over the icon (premultiplied; rebaked to straight alpha)
// Sizes are given at the client's 1x, where an icon is 56 px; the bar scales with the window as
// the owner's 2560x1440 client does (70 px slots).
// ---------------------------------------------------------------------------
namespace
{
    constexpr int   kBarSlots      = 8;
    constexpr float kBarAboveHud   = 8.f;      // gap above the play bar / event timeline
    constexpr float kRefSlot       = 70.f;     // owner's client at 1440 px tall
    constexpr float kRefHeight     = 1440.f;
    constexpr float kUnitSlot      = 56.f;     // a slot at UI scale 1x
    constexpr float kEffectIcon    = 52.f;     // an effect monitor icon at UI scale 1x
    constexpr float kPopPeak       = 1.6f;     // arrival peak, in icons: the client's other pop table

    // Frame atlas 265556: 56x56 cells, 4 per row, sampled 1/512 inside their edges.
    void FrameCellUV(int cell, ImVec2& uv0, ImVec2& uv1)
    {
        const float x = static_cast<float>((cell % 4) * 56), y = static_cast<float>((cell / 4) * 56);
        constexpr float in = 1.f / 512.f;
        uv0 = ImVec2(x / 256.f + in, y / 256.f + in);
        uv1 = ImVec2((x + 56.f) / 256.f - in, (y + 56.f) / 256.f - in);
    }

    // The client's GmClock: a black fan over the part still recharging, its edge leaving 12
    // o'clock and sweeping clockwise as the skill comes back.
    void DrawClockWedge(ImDrawList* dl, ImVec2 tl, float sz, float remainingFrac, ImU32 col)
    {
        const float frac = std::min(remainingFrac, 1.f);
        if (frac <= 0.001f) return;
        const ImVec2 c(tl.x + sz * 0.5f, tl.y + sz * 0.5f);
        const float r = sz * 0.75f;            // past the corners; the clip squares it off
        const float aEdge = -IM_PI * 0.5f + 2.f * IM_PI * (1.f - frac);
        const float sweep = 2.f * IM_PI * frac;
        const int segs = std::max(6, static_cast<int>(48.f * frac));
        dl->PushClipRect(tl, ImVec2(tl.x + sz, tl.y + sz), true);
        dl->PathClear();
        dl->PathLineTo(c);
        for (int s = 0; s <= segs; ++s) {
            const float a = aEdge + sweep * (static_cast<float>(s) / segs);
            dl->PathLineTo(ImVec2(c.x + r * cosf(a), c.y + r * sinf(a)));
        }
        dl->PathFillConvex(col);
        dl->PopClipRect();
    }

    // Image of 164476 the client puts in the top-right corner, from the skill's own data
    // (combo / type): -1 for none.
    int ProvidedMarker(const SkillInfo* si)
    {
        if (!si) return -1;
        switch (si->type) {
        case 5:  return 0;   // Lead Attack: single slash
        case 6:  return 1;   // Off-Hand Attack: cross
        case 7:  return 3;   // Dual Attack: starburst
        case 24: return 4;   // Hex Spell: purple down triangle
        case 27: return 5;   // Weapon Spell: sword
        }
        if (SkillDatabase::IsEnchantmentType(si->type)) return 2;   // yellow up triangle
        return -1;
    }
}


void ReplayWindow::DrawFocusHudSkillBar(int agentId)
{
    auto agentIt = m_replayCtx.agents.find(agentId);
    if (agentIt == m_replayCtx.agents.end()) return;
    const AgentReplayData& ard = agentIt->second;

    if (SkillBarDisplayOrder(agentId).empty()) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float slot  = std::round(std::clamp(kRefSlot * vp->Size.y / kRefHeight, 40.f, 84.f));
    const float scale = slot / kUnitSlot;

    // The frame art reaches past the slots; the window covers it all.
    const float artL = 16.f * scale, artR = 16.f * scale, artT = 7.f * scale, artB = 11.f * scale;
    const float winW = artL + kBarSlots * slot + artR;
    // The health bar (StatHealth) stands on the skill bar frame: its rect ends where the frame
    // art begins, 14 px tall at the owner's 70 px slots, and its own pewter frame reaches 2 UI
    // units above it.
    const float hpH    = std::round(14.f * slot / kRefSlot) + 1.f;   // one pixel over the client, for the text
    const float hpArtT = 2.f * scale;
    // The energy bar stands above the health bar, same height, its own frame clear of the health
    // bar's: each frame reaches 2 UI units past its bar, so the gap is 4.
    const float enGap  = std::round(4.f * scale);
    // The effect monitor's row stands above the energy bar's frame: icons of 52 UI units.
    const float effSide = std::round(kEffectIcon * scale);
    // More effects than a row holds wrap onto rows above it, so the window grows upwards by
    // last frame's extra rows (the skill bar itself stays put).
    const int   effRows = std::max(1, m_focusHudEffectRows);
    const float effRowH = effRows * effSide + std::round(4.f * scale);
    const float frameY = std::round(effRowH + hpArtT + hpH + enGap + hpH);
    const float winH   = frameY + artT + slot + artB;

    const float floorY = std::min(vp->Pos.y + vp->Size.y - kPlayBarH, m_eventTimelineTopY);
    const float posX = std::round(vp->Pos.x + (vp->Size.x - winW) * 0.5f);
    const float posY = std::round(std::max(vp->Pos.y, floorY - kBarAboveHud - winH));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::SetNextWindowPos(ImVec2(posX, posY));
    ImGui::SetNextWindowSize(ImVec2(winW, winH));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoResize        |
        ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoScrollbar     |
        ImGuiWindowFlags_NoCollapse   | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing;
    if (!ImGui::Begin("##focus_hud_skill_bar", nullptr, kFlags))
    {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    EnsureSkillIconIndex();
    const ImVec2 origin = ImGui::GetWindowPos();
    const bool windowHovered = ImGui::IsWindowHovered();

    ImTextureID texFrame   = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_frame.png");

    const ImVec2 frameTL(origin.x, origin.y + frameY);
    if (texFrame)
        DrawGameNineSlice(dl, texFrame, frameTL, ImVec2(origin.x + winW, origin.y + winH),
                          64.f * scale, 64.f * scale);
    else
        dl->AddRectFilled(frameTL, ImVec2(origin.x + winW, origin.y + winH), IM_COL32(12, 12, 14, 200), 8.f);

    // Bar rect = the slot row grown 7 UI units each side (measured on the owner's client).
    DrawFocusHudHealthBar(ard, ImVec2(origin.x + artL - 7.f * scale, frameTL.y - hpH),
                          ImVec2(origin.x + winW - artR + 7.f * scale, frameTL.y), scale);
    DrawFocusHudEnergyBar(ard, ImVec2(origin.x + artL - 7.f * scale, frameTL.y - hpH - enGap - hpH),
                          ImVec2(origin.x + winW - artR + 7.f * scale, frameTL.y - hpH - enGap), scale);
    m_focusHudEffectRows = DrawFocusHudEffectBar(agentId,
        ImVec2(origin.x + artL - 7.f * scale, origin.y + (effRows - 1) * effSide), effSide,
        kBarSlots * slot + 14.f * scale);

    DrawFocusHudSkillSlots(dl, agentId, ImVec2(origin.x + artL, frameTL.y + artT), slot, scale, windowHovered);

    ImGui::End();
    ImGui::PopStyleVar(2);

    // The cast strip stands beside the bar, as tall as its frame, and stops short of the weapon sets.
    if (m_showFocusCasts)
    {
        const float stripX = posX + winW + std::round(10.f * scale);
        const float stripW = std::min(m_focusHudRightLimitX - std::round(16.f * scale) - stripX, 12.f * slot);
        if (stripW > 3.f * slot)
            DrawFocusHudCastStrip(agentId, ImVec2(stripX, posY + frameY), stripW, artT + slot + artB, slot);
    }
}


void ReplayWindow::DrawFocusHudCastStrip(int agentId, ImVec2 tl, float width, float height, float slot)
{
    auto focusIt = m_replayCtx.agents.find(agentId);
    if (focusIt == m_replayCtx.agents.end()) return;
    const AgentReplayData& focus = focusIt->second;

    // Light by design: no frame, a faint shade that fades out at both ends, hairlines, and icons
    // that carry the weight. Future casts are dimmed like the info panel's cast timeline.
    // Incoming casts on the top row, the player's own on the bottom row beside the skill bar. Incoming
    // casts that land together stack upwards like cards (each kStackStep of an icon higher), rising
    // over the scene rather than leaving their true time.
    constexpr float kLabelFont = 11.f;
    constexpr int   kMaxStack  = 6;
    const float labelW = std::round(ImGui::GetFont()->CalcTextSizeA(kLabelFont, FLT_MAX, 0.f, "OUTGOING").x + 8.f);
    const float lineX  = tl.x + labelW;                 // where the timeline starts, right of the row labels
    const float icon   = std::round(std::min(height * 0.5f - 6.f, slot * 0.48f));
    const float step   = std::round(icon * 0.55f);
    constexpr float kRulerFont = 12.f;
    const float below  = kRulerFont + 4.f;              // second labels hang under the strip
    // Text over a bright floor needs a dark shadow to stay readable without a backing panel.
    auto shadowText = [](ImDrawList* d, float size, ImVec2 p, ImU32 col, const char* text) {
        const ImU32 shadow = IM_COL32(0, 0, 0, ((col >> IM_COL32_A_SHIFT) & 0xFF) * 3 / 4);
        d->AddText(nullptr, size, ImVec2(p.x + 1.f, p.y + 1.f), shadow, text);
        d->AddText(nullptr, size, p, col, text);
    };
    const float rise   = (kMaxStack - 1) * step;
    const float pps    = icon * 1.35f;                  // pixels per second
    const float nowX   = std::round(lineX + (width - labelW) * 0.42f);
    const float curT   = m_debugTimeline;
    const float tStart = curT - (nowX - lineX) / pps;
    const float tEnd   = curT + (tl.x + width - nowX) / pps;
    const float rowInY  = std::round(tl.y + height * 0.25f - icon * 0.5f);
    const float rowOutY = std::round(tl.y + height * 0.75f - icon * 0.5f);
    auto timeToX = [&](float t) { return nowX + (t - curT) * pps; };

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoResize        |
        ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoScrollbar     |
        ImGuiWindowFlags_NoCollapse   | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::SetNextWindowPos(ImVec2(tl.x, tl.y - rise));
    ImGui::SetNextWindowSize(ImVec2(width, height + rise + below));
    if (!ImGui::Begin("##focus_hud_cast_strip", nullptr, kFlags))
    {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    EnsureSkillIconIndex();
    const ImVec2 br(tl.x + width, tl.y + height);
    dl->PushClipRect(ImVec2(tl.x, tl.y - rise), ImVec2(br.x, br.y + below), true);

    // Shade: transparent at both ends, at most a light veil around the playhead.
    const ImU32 shade0 = IM_COL32(0, 0, 0, 0), shade1 = IM_COL32(0, 0, 0, 46);
    dl->AddRectFilledMultiColor(ImVec2(lineX, tl.y), ImVec2(nowX, br.y), shade0, shade1, shade1, shade0);
    dl->AddRectFilledMultiColor(ImVec2(nowX, tl.y), br, shade1, shade0, shade0, shade1);
    const float midY = std::round(tl.y + height * 0.5f);
    dl->AddRectFilledMultiColor(ImVec2(lineX, midY), ImVec2(nowX, midY + 1.f),
        IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 22), IM_COL32(255, 255, 255, 22), IM_COL32(255, 255, 255, 0));
    dl->AddRectFilledMultiColor(ImVec2(nowX, midY), ImVec2(br.x, midY + 1.f),
        IM_COL32(255, 255, 255, 22), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 22));

    // Ruler on the bottom edge, counted from the playhead: a tick every half second, a taller one
    // and a label every second, so the gap between a spike and the answer to it can be read off.
    dl->AddRectFilledMultiColor(ImVec2(lineX, br.y - 1.f), ImVec2(nowX, br.y + 1.f),
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(ImVec2(nowX, br.y - 1.f), ImVec2(br.x, br.y + 1.f),
        IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 90));
    dl->AddRectFilledMultiColor(ImVec2(lineX, br.y - 1.f), ImVec2(nowX, br.y),
        IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 130), IM_COL32(255, 255, 255, 130), IM_COL32(255, 255, 255, 0));
    dl->AddRectFilledMultiColor(ImVec2(nowX, br.y - 1.f), ImVec2(br.x, br.y),
        IM_COL32(255, 255, 255, 130), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 130));
    for (int k = static_cast<int>(std::floor((tStart - curT) * 2.f)); k <= static_cast<int>((tEnd - curT) * 2.f); ++k)
    {
        if (k == 0) continue;
        const float x = std::round(timeToX(curT + k * 0.5f));
        if (x < lineX || x > br.x) continue;
        const float edge = std::min(std::clamp((x - lineX) / icon, 0.f, 1.f), std::clamp((br.x - x) / icon, 0.f, 1.f));
        const bool whole = (k % 2) == 0;
        const float tickH = whole ? 8.f : 4.f;
        dl->AddLine(ImVec2(x + 1.f, br.y - tickH), ImVec2(x + 1.f, br.y), IM_COL32(0, 0, 0, static_cast<int>(110 * edge)), 1.f);
        dl->AddLine(ImVec2(x, br.y - tickH), ImVec2(x, br.y),
                    IM_COL32(255, 255, 255, static_cast<int>((whole ? 220 : 150) * edge)), whole ? 1.5f : 1.f);
        if (whole)
        {
            char lab[8];
            snprintf(lab, sizeof(lab), "%+d", k / 2);
            const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(kRulerFont, FLT_MAX, 0.f, lab);
            shadowText(dl, kRulerFont, ImVec2(std::round(x - sz.x * 0.5f), br.y + 2.f),
                       IM_COL32(0xF2, 0xE6, 0xC0, static_cast<int>(235 * edge)), lab);
        }
    }

    // Row labels, quiet, right-aligned against the start of the timeline.
    auto rowLabel = [&](const char* text, float rowY) {
        const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(kLabelFont, FLT_MAX, 0.f, text);
        shadowText(dl, kLabelFont, ImVec2(std::round(lineX - 6.f - sz.x), std::round(rowY + (icon - sz.y) * 0.5f)),
                   IM_COL32(0xF2, 0xE6, 0xC0, 215), text);
    };
    rowLabel("INCOMING", rowInY);
    rowLabel("OUTGOING", rowOutY);

    // Fades the ends out, and the past more the older it is.
    const float fadeW = icon;
    auto alphaAt = [&](float x0, float x1, float t) -> float {
        float a = std::clamp((x0 - lineX) / fadeW, 0.f, 1.f) * std::clamp((br.x - x1) / fadeW, 0.f, 1.f);
        if (t > curT) a *= 0.6f;
        else          a *= 1.f - 0.45f * std::clamp((curT - t) / (curT - tStart), 0.f, 1.f);
        return a;
    };
    auto withAlpha = [](ImU32 c, float a) {
        const float base = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF);
        return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(base * a) << IM_COL32_A_SHIFT);
    };
    // One colour code only: the caster's team, so a cast from an ally reads apart from one from an
    // enemy. An interrupted or cancelled cast keeps the info panel's purple / orange.
    auto teamColour = [&](int casterId) -> ImU32 {
        auto cit = m_replayCtx.agents.find(casterId);
        const uint8_t team = cit != m_replayCtx.agents.end() ? cit->second.teamId : 0;
        return team == 1 ? IM_COL32(0x60, 0x9C, 0xFF, 220) : team == 2 ? IM_COL32(0xE8, 0x50, 0x48, 220)
                                                         : IM_COL32(0xC8, 0xC8, 0xC8, 220);
    };
    auto castColour = [&](const SkillUseEvent& ev, int casterId) -> ImU32 {
        if (ev.wasInterrupted) return IM_COL32(0x90, 0x30, 0xD0, 220);
        if (ev.wasCancelled)   return IM_COL32(0xE0, 0x80, 0x20, 220);
        return teamColour(casterId);
    };

    // (x, y) = where the icon is drawn: a crowd on the outgoing row pushes it right of its true time
    // (tx), a crowd on the incoming row stacks it upwards. lineY = its row's cast-line height. pile =
    // the run of overlapping icons it belongs to, listed together on hover.
    struct Entry { const SkillUseEvent* ev; int casterId; float x = 0.f, y = 0.f, tx = 0.f, lineY = 0.f; int pile = 0; };

    // Outgoing: the player's own casts rarely collide, and when they do they fan out to the right,
    // each keeping at least this much of itself visible.
    constexpr float kMinShown = 0.55f;
    auto fanRow = [&](std::vector<Entry>& row, float rowY) {
        float prevX = -FLT_MAX;
        int pile = -1;
        for (auto& e : row)
        {
            e.tx = std::round(timeToX(e.ev->startTime));
            e.x  = std::max(e.tx, prevX + std::round(icon * kMinShown));
            e.y  = rowY;
            e.lineY = rowY + icon + 2.f;
            if (e.x >= prevX + icon) ++pile;
            e.pile = pile;
            prevX = e.x;
        }
    };

    // Incoming: each icon keeps its true time and takes the lowest level free at that moment.
    auto stackRow = [&](std::vector<Entry>& row, float rowY) {
        std::vector<float> levelRight;   // right edge of the last icon on each level
        float pileRight = -FLT_MAX;
        int pile = -1;
        for (auto& e : row)
        {
            e.tx = e.x = std::round(timeToX(e.ev->startTime));
            size_t level = 0;
            while (level < levelRight.size() && levelRight[level] > e.x) ++level;
            if (level == levelRight.size())
            {
                if (levelRight.size() < static_cast<size_t>(kMaxStack)) levelRight.push_back(0.f);
                else level = levelRight.size() - 1;   // a pile taller than the room overlaps its top card
            }
            levelRight[level] = e.x + icon;
            e.y = rowY - level * step;
            e.lineY = rowY + icon + 2.f;
            if (e.x >= pileRight) ++pile;
            pileRight = std::max(pileRight, e.x + icon);
            e.pile = pile;
        }
    };

    auto drawEntry = [&](const Entry& e, ImU32 edge)
    {
        const SkillUseEvent& ev = *e.ev;
        const float x0 = e.x;
        const float a  = alphaAt(x0, x0 + icon, ev.startTime);
        if (a <= 0.01f) return;
        const ImU32 col = castColour(ev, e.casterId);

        // The cast's length as a hairline under the row, filled up to the playhead. It starts at the
        // true time, so a pushed icon stays tied to when it was cast.
        const float ly = e.lineY;
        if (!ev.isInstant && ev.endTime > ev.startTime)
        {
            const float lx1 = timeToX(ev.endTime);
            dl->AddRectFilled(ImVec2(e.tx, ly), ImVec2(lx1, ly + 2.f), withAlpha(col, a * 0.45f), 1.f);
            const float fill = std::clamp(timeToX(curT), e.tx, lx1);
            if (fill > e.tx) dl->AddRectFilled(ImVec2(e.tx, ly), ImVec2(fill, ly + 2.f), withAlpha(col, a), 1.f);
        }
        else if (x0 > e.tx + 1.f)
            dl->AddRectFilled(ImVec2(e.tx, ly), ImVec2(e.tx + 2.f, ly + 3.f), withAlpha(col, a), 1.f);

        const ImVec2 i0(x0, e.y), i1(x0 + icon, e.y + icon);
        if (ImTextureID tex = LoadSkillIcon(this, dev, ev.skillId, m_skillIconIndex, m_skillIconCache))
            dl->AddImageRounded(tex, i0, i1, ImVec2(0, 0), ImVec2(1, 1), withAlpha(IM_COL32_WHITE, a), 3.f);
        else
            dl->AddRectFilled(i0, i1, withAlpha(col, a), 3.f);

        if (ev.wasInterrupted || ev.wasCancelled)
            dl->AddRect(ImVec2(i0.x - 0.5f, i0.y - 0.5f), ImVec2(i1.x + 0.5f, i1.y + 0.5f), withAlpha(col, a), 3.f, 0, 1.5f);
        else if (edge)
            dl->AddRect(ImVec2(i0.x - 0.5f, i0.y - 0.5f), ImVec2(i1.x + 0.5f, i1.y + 0.5f), withAlpha(edge, a), 3.f, 0, 1.2f);

        // Incoming casts carry their caster's profession in the corner.
        if (e.casterId != agentId)
        {
            auto cit = m_replayCtx.agents.find(e.casterId);
            if (cit != m_replayCtx.agents.end() && cit->second.primaryProf > 0)
                if (ImTextureID prof = LoadProfIcon(dev, cit->second.primaryProf))
                {
                    const float ps = std::round(icon * 0.42f);
                    dl->AddImage(prof, ImVec2(i1.x - ps + 2.f, i1.y - ps + 2.f), ImVec2(i1.x + 2.f, i1.y + 2.f),
                                 ImVec2(0, 0), ImVec2(1, 1), withAlpha(IM_COL32_WHITE, a));
                }
        }
    };

    // Spikes found on the incoming row: how many damage skills, and first hit to last hit.
    struct Spike { int pile; int count; float span; };
    std::vector<Spike> spikes;

    // Hovering any icon of a pile lists the whole pile, in time order.
    auto tooltipFor = [&](const std::vector<Entry>& row) {
        const Entry* hit = nullptr;
        for (const auto& e : row)   // later icons are drawn on top, so the last hit wins
            if (ImGui::IsMouseHoveringRect(ImVec2(e.x, e.y), ImVec2(e.x + icon, e.y + icon), false))
                hit = &e;
        if (!hit) return;

        std::vector<CastRef> pile;
        for (const auto& e : row)
            if (e.pile == hit->pile) pile.push_back({ e.ev, e.casterId });
        std::string headline;
        if (!row.empty() && row.front().casterId != agentId)
            for (const Spike& sp : spikes)
                if (sp.pile == hit->pile)
                {
                    char buf[96];
                    snprintf(buf, sizeof(buf), "Spike: %d damage skills hit in %.2f s", sp.count, sp.span);
                    headline = buf;
                }
        DrawCastTableTooltip(pile, headline.empty() ? nullptr : headline.c_str());
    };

    // Events sorted by start; a long cast that began before the window can still reach into it.
    auto forEachInWindow = [&](const AgentReplayData& ard, auto&& fn) {
        const auto& hist = ard.skillUseHistory;
        auto it = std::lower_bound(hist.begin(), hist.end(), tStart - 10.f,
            [](const SkillUseEvent& ev, float t) { return ev.startTime < t; });
        for (; it != hist.end() && it->startTime <= tEnd; ++it)
            if (std::max(it->endTime, it->startTime) + icon / pps >= tStart) fn(*it);
    };

    // Outgoing: the followed player's own casts.
    std::vector<Entry> outgoing;
    forEachInWindow(focus, [&](const SkillUseEvent& ev) { outgoing.push_back({ &ev, agentId }); });
    fanRow(outgoing, rowOutY);
    for (const auto& e : outgoing) drawEntry(e, 0);

    // Incoming: every other agent's casts aimed at the followed player.
    std::vector<Entry> incoming;
    for (const auto& [id, ard] : m_replayCtx.agents)
    {
        if (id == agentId) continue;
        forEachInWindow(ard, [&](const SkillUseEvent& ev) {
            if (ev.targetId == agentId) incoming.push_back({ &ev, id });
        });
    }
    std::sort(incoming.begin(), incoming.end(),
        [](const Entry& a, const Entry& b) { return a.ev->startTime < b.ev->startTime; });
    stackRow(incoming, rowInY);

    auto isEnemy = [&](int casterId) {
        auto cit = m_replayCtx.agents.find(casterId);
        return cit != m_replayCtx.agents.end() && cit->second.teamId != focus.teamId;
    };
    auto dealsDamage = [&](int skillId) {
        const SkillInfo* si = m_skillView.IsLoaded() ? m_skillView.Get(skillId) : nullptr;
        if (!si) return false;
        for (const auto& sc : si->scales)
            if (sc.kind == SkillScaleKind::Damage || sc.kind == SkillScaleKind::LifeSteal ||
                sc.kind == SkillScaleKind::LifeLoss)
                return true;
        return false;
    };

    // Spikes: three or more enemy damage skills landing within 1.5 s of each other.
    {
        std::vector<std::pair<float, const Entry*>> lands;
        for (const auto& e : incoming)
            if (!e.ev->wasInterrupted && !e.ev->wasCancelled && isEnemy(e.casterId) && dealsDamage(e.ev->skillId))
                lands.push_back({ std::max(e.ev->endTime, e.ev->startTime), &e });
        std::sort(lands.begin(), lands.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i + 2 < lands.size();)
        {
            size_t j = i;
            while (j + 1 < lands.size() && lands[j + 1].first - lands[i].first <= 1.5f) ++j;
            if (j - i + 1 < 3) { ++i; continue; }
            float x0 = FLT_MAX, x1 = -FLT_MAX, y0 = rowInY;
            for (size_t k = i; k <= j; ++k)
            {
                const Entry& e = *lands[k].second;
                x0 = std::min(x0, e.x); x1 = std::max(x1, e.x + icon); y0 = std::min(y0, e.y);
            }
            const float a = alphaAt(x0, x0, lands[i].first) * 0.85f + 0.15f;
            const ImU32 red = withAlpha(IM_COL32(0xE0, 0x48, 0x40, 200), a);
            dl->AddRect(ImVec2(x0 - 3.f, y0 - 3.f), ImVec2(x1 + 3.f, rowInY + icon + 3.f), red, 4.f, 0, 1.f);
            char label[24];
            snprintf(label, sizeof(label), "SPIKE x%d", static_cast<int>(j - i + 1));
            shadowText(dl, kLabelFont, ImVec2(std::round(x1 + 6.f), std::round(rowInY + icon * 0.5f - 6.f)), red, label);
            // How tightly the damage was packed (first hit to last hit) goes to the hover tooltip.
            spikes.push_back({ lands[i].second->pile, static_cast<int>(j - i + 1), lands[j].first - lands[i].first });
            i = j + 1;
        }
    }

    for (const auto& e : incoming)
        drawEntry(e, teamColour(e.casterId));
    tooltipFor(outgoing);
    tooltipFor(incoming);

    // The playhead.
    dl->AddLine(ImVec2(nowX, tl.y + 2.f), ImVec2(nowX, br.y - 2.f), IM_COL32(0xCB, 0xAA, 0x09, 200), 1.5f);
    dl->AddTriangleFilled(ImVec2(nowX - 3.f, tl.y), ImVec2(nowX + 3.f, tl.y), ImVec2(nowX, tl.y + 4.f),
                          IM_COL32(0xCB, 0xAA, 0x09, 220));

    dl->PopClipRect();
    ImGui::End();
    ImGui::PopStyleVar(2);
}


// The hover table shared by the cast strip and the Piano Roll: one line per cast in the order they
// hit - skill, caster, the match-clock moment it hits - and a small bar on a shared scale (cast from
// start to hit) so the order reads at a glance. `headline` (optional) goes above it in red.
void ReplayWindow::DrawCastTableTooltip(std::vector<CastRef> rows, const char* headline)
{
    if (rows.empty()) return;
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    EnsureSkillIconIndex();

    auto landsAt = [](const CastRef& c) { return std::max(c.ev->endTime, c.ev->startTime); };
    std::stable_sort(rows.begin(), rows.end(),
        [&](const CastRef& a, const CastRef& b) { return landsAt(a) < landsAt(b); });
    float axis0 = FLT_MAX, axis1 = -FLT_MAX;
    for (const CastRef& c : rows)
    {
        axis0 = std::min(axis0, c.ev->startTime);
        axis1 = std::max(axis1, landsAt(c));
    }
    axis0 -= 0.25f;
    axis1 += 0.25f;

    const float iconSz = std::round(ImGui::GetTextLineHeight() * 1.45f);
    const ImVec4 kHead(0.72f, 0.68f, 0.56f, 1.f), kTime(1.f, 0.84f, 0.30f, 1.f);
    auto teamOf = [&](int agentId) -> uint8_t {
        auto it = m_replayCtx.agents.find(agentId);
        return it != m_replayCtx.agents.end() ? it->second.teamId : 0;
    };
    auto teamText = [](uint8_t team) {
        return team == 1 ? ImVec4(0.45f, 0.66f, 1.f, 1.f) : team == 2 ? ImVec4(1.f, 0.45f, 0.40f, 1.f)
                                                          : ImVec4(0.85f, 0.85f, 0.85f, 1.f);
    };
    // The bar takes the caster's team colour, or the outcome's for an interrupted / cancelled cast.
    auto barColour = [&](const CastRef& c) -> ImU32 {
        if (c.ev->wasInterrupted) return IM_COL32(0x90, 0x30, 0xD0, 176);
        if (c.ev->wasCancelled)   return IM_COL32(0xE0, 0x80, 0x20, 176);
        const uint8_t team = teamOf(c.casterId);
        return team == 1 ? IM_COL32(0x60, 0x9C, 0xFF, 176) : team == 2 ? IM_COL32(0xE8, 0x50, 0x48, 176)
                                                           : IM_COL32(0xC8, 0xC8, 0xC8, 176);
    };
    // Text centred on an icon of iconSz drawn just before it on the same line.
    auto alignedText = [&](const ImVec4& col, const char* fmt, auto... args) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (iconSz - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextColored(col, fmt, args...);
    };

    ImGui::BeginTooltip();
    if (headline)
        ImGui::TextColored(ImVec4(1.f, 0.42f, 0.36f, 1.f), "%s", headline);
    constexpr ImGuiTableFlags kTable = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_PadOuterX;
    constexpr int kCols = 4;
    if (ImGui::BeginTable("##cast_pile", kCols, kTable))
    {
        ImGui::TableSetupColumn("Skill");
        ImGui::TableSetupColumn("Cast by");
        ImGui::TableSetupColumn("Hits at");
        ImGui::TableSetupColumn("");
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int c = 0; c < kCols; ++c)
        {
            ImGui::TableSetColumnIndex(c);
            ImGui::TextColored(kHead, "%s", ImGui::TableGetColumnName(c));
        }

        for (const CastRef& c : rows)
        {
            const SkillUseEvent& ev = *c.ev;
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (ImTextureID tex = LoadSkillIcon(this, dev, ev.skillId, m_skillIconIndex, m_skillIconCache))
                ImGui::Image(tex, ImVec2(iconSz, iconSz));
            else
                ImGui::Dummy(ImVec2(iconSz, iconSz));
            ImGui::SameLine();
            const std::string name = GetSkillDisplayName(ev.skillId);
            if (ev.wasInterrupted)     alignedText(ImVec4(0.75f, 0.45f, 1.f, 1.f), "%s (interrupted)", name.c_str());
            else if (ev.wasCancelled)  alignedText(ImVec4(1.f, 0.62f, 0.25f, 1.f), "%s (cancelled)", name.c_str());
            else                       alignedText(ImVec4(1.f, 1.f, 1.f, 1.f), "%s", name.c_str());

            ImGui::TableSetColumnIndex(1);
            auto cit = m_replayCtx.agents.find(c.casterId);
            if (cit != m_replayCtx.agents.end())
            {
                const AgentReplayData& caster = cit->second;
                ImTextureID prof = caster.primaryProf > 0 ? LoadProfIcon(dev, caster.primaryProf) : nullptr;
                if (prof) ImGui::Image(prof, ImVec2(iconSz, iconSz));
                else      ImGui::Dummy(ImVec2(iconSz, iconSz));
                ImGui::SameLine();
                const std::string& who = !caster.playerName.empty() ? caster.playerName : caster.categoryName;
                alignedText(teamText(caster.teamId), "%s", who.c_str());
            }

            ImGui::TableSetColumnIndex(2);
            const float hitT = landsAt(c);
            const int mins = static_cast<int>(hitT / 60.f);
            alignedText(kTime, "%d:%04.1f", mins, hitT - mins * 60.f);

            // The cast as a bar from its start to its hit, the hit as a gold mark.
            ImGui::TableSetColumnIndex(3);
            {
                const float barW = std::round(iconSz * 5.f);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(barW, iconSz));
                ImDrawList* tdl = ImGui::GetWindowDrawList();
                auto ax = [&](float t) { return std::round(p.x + (t - axis0) / (axis1 - axis0) * barW); };
                const float cy = p.y + iconSz * 0.5f;
                for (float g = std::ceil(axis0 * 2.f) * 0.5f; g <= axis1; g += 0.5f)
                    tdl->AddLine(ImVec2(ax(g), p.y + 2.f), ImVec2(ax(g), p.y + iconSz - 2.f), IM_COL32(255, 255, 255, 28));
                tdl->AddRectFilled(ImVec2(ax(ev.startTime), cy - 1.5f), ImVec2(ax(hitT), cy + 1.5f), barColour(c), 1.5f);
                tdl->AddRectFilled(ImVec2(ax(hitT) - 1.5f, p.y + 3.f), ImVec2(ax(hitT) + 1.5f, p.y + iconSz - 3.f),
                                   IM_COL32(0xFF, 0xD6, 0x4D, 0xFF), 1.f);
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndTooltip();
}


// The eight slots of the skill bar alone, top-left at rowTL, slot px each: what the HUD's skill bar
// draws inside its frame, and what the split camera draws under its view.
void ReplayWindow::DrawFocusHudSkillSlots(ImDrawList* dl, int agentId, ImVec2 rowTL, float slot, float scale, bool hoverable)
{
    auto agentIt = m_replayCtx.agents.find(agentId);
    if (agentIt == m_replayCtx.agents.end()) return;
    const AgentReplayData& ard = agentIt->second;

    std::vector<int> bar = SkillBarDisplayOrder(agentId);
    if (bar.empty()) return;
    if (bar.size() > kBarSlots) bar.resize(kBarSlots);

    const float t = m_debugTimeline;
    const std::vector<SkillCooldownState> cds = ComputeSkillCooldowns(ard, bar, t);
    using CdState = SkillCooldownState::State;

    // The skill in use right now, and since when (the sparkle's phase).
    int   usingSkill = 0;
    float usingSince = 0.f;
    for (const auto& ev : ard.skillUseHistory) {
        if (ev.startTime > t) break;
        if (!ev.isInstant && ev.endTime > t) {
            usingSkill = m_skillView.ResolvePvpSkillId(ev.skillId);
            usingSince = ev.startTime;
        }
    }

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    EnsureSkillIconIndex();
    ImTextureID texSlots   = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_slot_frames.png");
    ImTextureID texKeys    = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_keycaps.png");
    ImTextureID texMarkers = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_markers.png");
    ImTextureID texNoSign  = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_check_forbidden.png");
    ImTextureID texSparkle = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_sparkle.png");

    for (int i = 0; i < kBarSlots; ++i)
    {
        const ImVec2 tl(rowTL.x + i * slot, rowTL.y);
        const ImVec2 br(tl.x + slot, tl.y + slot);
        const bool filled = i < static_cast<int>(bar.size());
        // The skill the slot shows: Echo / Arcane Echo show the copy they hold.
        const int shown = filled ? cds[i].shownSkillId : 0;
        const SkillInfo* si = filled ? m_skillView.Get(shown) : nullptr;

        // Icon (or the empty slot's grey), then the slot frame over it.
        if (!filled)
            dl->AddRectFilled(tl, br, IM_COL32(0x40, 0x40, 0x40, 0xC0));
        else if (shown == SkillCooldownState::kUnknownSkill) {
            // A stolen skill the recording does not name: a neutral slot with a question mark.
            dl->AddRectFilled(tl, br, IM_COL32(0x22, 0x24, 0x2A, 0xF0));
            const float fs = slot * 0.6f;
            const ImVec2 qs = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, "?");
            dl->AddText(nullptr, fs, ImVec2(tl.x + (slot - qs.x) * 0.5f, tl.y + (slot - qs.y) * 0.5f),
                        IM_COL32(0xB8, 0xB8, 0xB0, 0xFF), "?");
        }
        else if (ImTextureID tex = LoadSkillIcon(this, dev, shown, m_skillIconIndex, m_skillIconCache))
            dl->AddImage(tex, tl, br, ImVec2(0.0625f, 0.0625f), ImVec2(0.9375f, 0.9375f));
        if (texSlots) {
            ImVec2 uv0, uv1;
            FrameCellUV(si && si->is_elite ? 1 : 0, uv0, uv1);
            dl->AddImage(texSlots, tl, br, uv0, uv1);
        }
        if (!filled) continue;

        const SkillCooldownState& cd = cds[i];
        const bool disabled = cd.state == CdState::Disabled;

        // Skill in use: the client's looping sparkle.
        if (texSparkle && shown == usingSkill && cd.castingNow) {
            const int frame = static_cast<int>((t - usingSince) * 16.f) & 15;
            const float fx = static_cast<float>(frame % 4) * 0.25f, fy = static_cast<float>(frame / 4) * 0.25f;
            dl->AddImage(texSparkle, tl, br, ImVec2(fx, fy), ImVec2(fx + 0.25f, fy + 0.25f));
        }

        // Corner marker, from the skill's own data.
        if (texMarkers) {
            const int m = ProvidedMarker(si);
            if (m >= 0) {
                const float ms = slot * 32.f / 56.f;
                const float mx = static_cast<float>(m % 4) * 0.25f, my = static_cast<float>(m / 4) * 0.5f;
                dl->AddImage(texMarkers, ImVec2(br.x - ms, tl.y), ImVec2(br.x, tl.y + ms),
                             ImVec2(mx, my), ImVec2(mx + 0.25f, my + 0.5f));
            }
        }

        // Unavailable: the clock fan, measured as the client measures it, then the no-sign over a
        // disabled skill. Resurrection Signet after its use is a full fan until a morale boost.
        if (cd.state == CdState::Spent) {
            DrawClockWedge(dl, tl, slot, 1.f, IM_COL32(0, 0, 0, 0xA0));
        } else if ((cd.state == CdState::Recharging || disabled) && cd.remaining > 0.f) {
            const float base = si ? si->recharge : 0.f;
            const float total = disabled ? std::max(cd.total, cd.remaining)
                                         : std::max(base, cd.remaining);
            DrawClockWedge(dl, tl, slot, total > 0.f ? cd.remaining / total : 1.f, IM_COL32(0, 0, 0, 0xA0));
        }
        if (disabled && texNoSign)
            dl->AddImage(texNoSign, tl, br, ImVec2(0.5f, 0.5f), ImVec2(1.f, 1.f));

        // Keycap, bottom right.
        if (texKeys && slot >= 32.f) {
            const float ks = 24.f * scale;
            const float kx = static_cast<float>(i % 5) * 24.f / 128.f, ky = static_cast<float>(i / 5) * 24.f / 128.f;
            dl->AddImage(texKeys, ImVec2(br.x - ks, br.y - ks), br,
                         ImVec2(kx, ky), ImVec2(kx + 24.f / 128.f, ky + 24.f / 128.f));
        }

        if (hoverable && ImGui::IsMouseHoveringRect(tl, br)) {
            if (shown == SkillCooldownState::kUnknownSkill) ImGui::SetTooltip("%s", cd.slotNote.c_str());
            else DrawGameSkillTooltip(shown, agentId, &cd);
        }
    }

}


// ---------------------------------------------------------------------------
// Effect monitor over the energy bar: the effects on the followed player, drawn as the client
// draws its own (GmEffect / GmEffectFloater / GmCtlSkImageEffect, decoded from Gw.exe 2026-10-07;
// gwobserver-private/docs/research/gw_effect_bar.md). Which effects, and when each ends, come from
// EffectTimeline (private).
//
//   order    by kind - conditions, hexes, rituals (a spirit's range), stances, enchantments, then
//            the rest - a new effect at the end of its group; one icon per skill, timed by its
//            longest instance, and a refresh that lengthens it moves it to the end of its group.
//            Ours: what the map itself pays (Terrain) stands before all of them
//   icon     52 UI units, icons touching; frame cell of 265556 by kind (orange condition, magenta
//            hex, yellow-green enchantment, teal Dervish enchantment, dark green other), elite odd
//   bar      3 px black, 85% of the icon wide, 2 px above its bottom; a 1 px line in the kind's
//            colour drains left; none for an effect without a duration
//   arrival  a 0.5 s pop in place: alpha 0 -> 1 in 0.2 s, size 0.25 -> peak (f^1/3) then -> 1 (f^3);
//            peak 1.6: the client has a 4.0 and a 1.6 table, and the owner judged 4 and 3 far too big
//   removal  the icon goes at once and the ones after it close the gap, no slide (ours: when the
//            skill that removed it is known, it stays 0.7 s, dimmed, under that skill's icon)
//   blink    from clamp(0.2 x duration, 2, 4) s before the end it was given: opacity 1 <-> 0.25,
//            each phase clamp(time left / 8, 0.0625, 0.333) s; the bar too runs against that
//            duration, so a removed effect goes with part of its bar left
// ---------------------------------------------------------------------------
namespace
{
    int EffectSortKey(EffectTimeline::Kind k)
    {
        using K = EffectTimeline::Kind;
        switch (k) {
        case K::Condition:   return 4;
        case K::Terrain:     return -1;
        case K::Hex:         return 5;
        case K::Ritual:      return 6;
        case K::Stance:      return 9;
        case K::Enchantment: return 12;
        default:             return 13;
        }
    }

    int EffectFrameCell(EffectTimeline::Kind k, const SkillInfo* si)
    {
        using K = EffectTimeline::Kind;
        const int elite = si && si->is_elite ? 1 : 0;
        switch (k) {
        case K::Condition:   return 4 + elite;
        case K::Hex:         return 8 + elite;
        case K::Enchantment: return (si && si->profession == 10 ? 10 : 6) + elite;
        default:             return 2 + elite;
        }
    }

    ImU32 EffectBarColour(EffectTimeline::Kind k, const SkillInfo* si, float opacity)
    {
        using K = EffectTimeline::Kind;
        const int a = static_cast<int>(255.f * opacity);
        switch (k) {
        case K::Condition:   return IM_COL32(0xDF, 0xB1, 0x3E, a);
        case K::Hex:         return IM_COL32(0xE9, 0x3D, 0xB5, a);
        case K::Enchantment: return si && si->profession == 10 ? IM_COL32(0xBE, 0x9F, 0x49, a) : IM_COL32(0xA8, 0xD4, 0x38, a);
        default:             return IM_COL32(0x4E, 0x9C, 0x49, a);
        }
    }

    // The client toggles opacity at fixed times from the effect's start, so the state at any
    // moment is a replay of those toggles - which keeps it right when the timeline is scrubbed.
    float EffectBlinkOpacity(float elapsed, float duration)
    {
        const float lead = std::clamp(0.2f * duration, 2.f, 4.f);
        float next = std::max(duration - lead, 0.f);
        bool dim = false;
        for (int guard = 0; next <= elapsed && guard < 256; ++guard) {
            dim = !dim;
            next += std::clamp((duration - next) / 8.f, 0.0625f, 0.333f);
        }
        return dim ? 0.25f : 1.f;
    }

    const char* ConditionName(int skillId)
    {
        switch (skillId) {
        case EffectTimeline::kBleeding:     return "Bleeding";
        case EffectTimeline::kBlind:        return "Blind";
        case EffectTimeline::kBurning:      return "Burning";
        case EffectTimeline::kCrippled:     return "Crippled";
        case EffectTimeline::kDeepWound:    return "Deep Wound";
        case EffectTimeline::kDisease:      return "Disease";
        case EffectTimeline::kPoison:       return "Poison";
        case EffectTimeline::kDazed:        return "Dazed";
        case EffectTimeline::kWeakness:     return "Weakness";
        case EffectTimeline::kCrackedArmor: return "Cracked Armor";
        default:                            return nullptr;
        }
    }
}


// ---------------------------------------------------------------------------
// The fixed icons at the head of the effect monitor, shown alive or dead: the flux, the Isle of
// Wurms shrine bonus, then morale.
//
//   flux     ours (the client has no flux icon): the match's PvP flux, PvP_Flair art under the
//            plain effect frame (cell 2); the tooltip gives its name and description
//   shrine   the Southern Health Shrine Bonus (skill 1669, DAT 384519) while the agent's team holds
//            the shrine - the client lists it as an ordinary effect; the owner wants it fixed here
//   morale   the client's fixed icon code 3, sort key 1 - before every effect (GmEffect.cpp:
//            create 0x5241c0, update 0x524d50; decoded from Gw.exe 2026-10-09). Shown while
//            morale != 0, no bar: DAT 32745 (teal arrow up) for a boost, 32746 (red arrow down) for
//            death penalty - 52x52 of premultiplied art, composited over black - under the grey
//            slot frame (cell 0), and the text L"%+i%%" in a CtlText of style 0x94080 (centred,
//            bottom-aligned), Friz Quadrata in 0xFFEEBB, matched on the owner's screenshot
// ---------------------------------------------------------------------------
float ReplayWindow::DrawFocusHudFixedEffects(int agentId, ImVec2 rowTopLeft, float side)
{
    auto agentIt = m_replayCtx.agents.find(agentId);
    if (agentIt == m_replayCtx.agents.end()) return 0.f;
    const AgentReplayData& ard = agentIt->second;
    const float t = m_debugTimeline;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    const bool windowHovered = ImGui::IsWindowHovered();
    float x = rowTopLeft.x;

    // The recording names the flux when the recorder saw it; the month decides otherwise.
    AttributeModel::Flux flux = AttributeModel::FluxFromName(m_matchMeta.flux);
    if (flux == AttributeModel::Flux::None && m_matchMeta.month > 0)
        flux = AttributeModel::FluxForMonth(m_matchMeta.month);
    if (flux != AttributeModel::Flux::None) {
        const ImVec2 tl(x, rowTopLeft.y), br(x + side, rowTopLeft.y + side);
        ImVec2 fuv0, fuv1;
        FrameCellUV(2, fuv0, fuv1);
        if (ImTextureID tex = LoadSkillIconFile(dev, "PvP_Flair.png"))
            dl->AddImage(tex, tl, br, ImVec2(0.0625f, 0.0625f), ImVec2(0.9375f, 0.9375f));
        if (ImTextureID texSlots = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_slot_frames.png"))
            dl->AddImage(texSlots, tl, br, fuv0, fuv1);

        if (windowHovered && ImGui::IsMouseHoveringRect(tl, br)) {
            const char* name = AttributeModel::FluxName(flux);
            const char* desc = GetFluxDescription(name);
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(340.f);
            // The client's tooltip: the title in pale yellow, the text, then the grey footer.
            ImGui::TextColored(ImVec4(1.f, 238.f / 255.f, 187.f / 255.f, 1.f), "%s", name);
            if (desc) ImGui::TextUnformatted(desc);
            if (flux == AttributeModel::Flux::LikeABoss)
                if (const BossTenure* boss = FindBossTenure(agentId, t)) {
                    const int held = static_cast<int>(std::max(0.f, t - boss->start));
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.83f, 0.63f, 0.13f, 1.f), "The Boss, held for %d:%02d", held / 60, held % 60);
                }
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.f), "PvP flux");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        x += side;
    }

    // Isle of Wurms: the Southern Health Shrine Bonus while this agent's team holds the shrine, from
    // the same timeline the max-HP model reads, so the icon and the +120 come and go together.
    if (!m_wurmsShrineSamples.empty() && (ard.teamId == 1 || ard.teamId == 2) &&
        !(ard.type == AgentType::NPC && IsGuildNpcModelId(ard.modelId))) {
        const int idx = std::clamp(static_cast<int>(t / m_wurmsShrineSampleDt), 0,
                                   static_cast<int>(m_wurmsShrineSamples.size()) - 1);
        if (m_wurmsShrineSamples[idx].ownerTeam == ard.teamId) {
            const ImVec2 tl(x, rowTopLeft.y), br(x + side, rowTopLeft.y + side);
            EnsureSkillIconIndex();
            if (ImTextureID tex = LoadSkillIcon(this, dev, kSouthernHealthShrineBonusSkillId, m_skillIconIndex, m_skillIconCache))
                dl->AddImage(tex, tl, br, ImVec2(0.0625f, 0.0625f), ImVec2(0.9375f, 0.9375f));
            if (ImTextureID texSlots = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_slot_frames.png")) {
                ImVec2 fuv0, fuv1;
                FrameCellUV(2, fuv0, fuv1);
                dl->AddImage(texSlots, tl, br, fuv0, fuv1);
            }

            if (windowHovered && ImGui::IsMouseHoveringRect(tl, br)) {
                float since = -1.f;
                for (auto it = m_wurmsShrineCaptureEvents.rbegin(); it != m_wurmsShrineCaptureEvents.rend(); ++it)
                    if (it->first <= t && it->second == ard.teamId) { since = it->first; break; }
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(340.f);
                ImGui::TextColored(ImVec4(1.f, 238.f / 255.f, 187.f / 255.f, 1.f), "Southern Health Shrine Bonus");
                ImGui::Text("+%d maximum Health for every member of the team holding the Southern Health Shrine.",
                            kWurmsShrineHealthBonus);
                if (since >= 0.f) {
                    const int held = static_cast<int>(std::max(0.f, t - since));
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "Held for %d:%02d", held / 60, held % 60);
                }
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.f), "Isle of Wurms shrine");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            x += side;
        }
    }

    int deaths = 0, boosts = 0;
    const int morale = ComputeAgentMorale(ard, t, &deaths, &boosts);
    if (morale != 0) {
        const ImVec2 tl(x, rowTopLeft.y), br(x + side, rowTopLeft.y + side);
        if (ImTextureID tex = LoadGameUITexture(dev, morale > 0 ? "Effectbar\\ui_effect_morale_boost.png"
                                                                : "Effectbar\\ui_effect_morale_penalty.png"))
            dl->AddImage(tex, tl, br);
        // The grey slot frame over it, as the owner's screenshot shows (2026-10-09).
        if (ImTextureID texSlots = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_slot_frames.png")) {
            ImVec2 fuv0, fuv1;
            FrameCellUV(0, fuv0, fuv1);
            dl->AddImage(texSlots, tl, br, fuv0, fuv1);
        }

        // The number in the game's face and pale yellow, placed by its ink (not its advance box):
        // "-15%" inks 0.66 of the icon wide, centred under the arrow's tip, the digits' bottom 0.09
        // of the icon above the bottom edge, inside the frame (tuned on the owner's screenshots).
        char num[16];
        snprintf(num, sizeof(num), "%+d%%", morale);
        ImFont* font = m_frizHud ? m_frizHud : ImGui::GetFont();
        // Ink extent of a string at the font's own size: left of the first glyph to right of the last.
        auto inkSpan = [&](const char* s, float& x0, float& x1) {
            float pen = 0.f;
            x0 = FLT_MAX; x1 = 0.f;
            for (const char* c = s; *c; ++c) {
                const ImFontGlyph* g = font->FindGlyph(static_cast<ImWchar>(*c));
                if (!g) continue;
                x0 = std::min(x0, pen + g->X0);
                x1 = std::max(x1, pen + g->X1);
                pen += g->AdvanceX;
            }
            if (x0 > x1) x0 = x1 = 0.f;
        };
        float r0, r1, n0, n1;
        inkSpan("-15%", r0, r1);
        inkSpan(num, n0, n1);
        const float k  = r1 > r0 ? 0.66f * side / (r1 - r0) : side * 0.3f / font->FontSize;   // px per font unit
        const float fs = font->FontSize * k;
        const ImFontGlyph* digit = font->FindGlyph('0');
        const float inkBottom = digit ? digit->Y1 : font->Ascent;
        const ImVec2 tp(std::round(tl.x + side * 0.5f - (n0 + n1) * 0.5f * k),
                        std::round(br.y - 0.09f * side - inkBottom * k));
        dl->AddText(font, fs, ImVec2(tp.x + 1.f, tp.y + 1.f), IM_COL32(0, 0, 0, 0xB0), num);
        dl->AddText(font, fs, tp, IM_COL32(255, 238, 187, 255), num);

        if (windowHovered && ImGui::IsMouseHoveringRect(tl, br))
            DrawMoraleTooltip(morale, deaths, boosts);
        x += side;
    }
    return x - rowTopLeft.x;
}


void ReplayWindow::DrawMoraleTooltip(int morale, int deaths, int boosts)
{
    ImGui::BeginTooltip();
    if (morale > 0) {
        ImGui::TextUnformatted("Morale Boost");
        ImGui::Text("Maximum Health and Energy +%d%%.", morale);
    } else {
        ImGui::TextUnformatted("Death Penalty");
        ImGui::Text("Maximum Health and Energy %d%%.", morale);
    }
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f), "%d death%s, %d team morale boost%s (calculated)",
                       deaths, deaths == 1 ? "" : "s", boosts, boosts == 1 ? "" : "s");
    ImGui::EndTooltip();
}


void ReplayWindow::DrawEffectIconTooltip(const EffectTimeline::Effect& e, int agentId, bool removed)
{
    const SkillInfo* si = m_skillView.Get(e.skillId);
    const float t = m_debugTimeline;
    if (si && !e.label) DrawGameSkillTooltip(e.skillId, e.caster ? e.caster : agentId, nullptr, e.start);
    ImGui::BeginTooltip();
    if (e.label) {
        ImGui::TextUnformatted(e.label);
    } else if (!si) {
        const char* cond = ConditionName(e.skillId);
        ImGui::TextUnformatted(cond ? cond : "Effect");
    }
    auto nameOf = [&](int id) -> std::string {
        auto a = m_replayCtx.agents.find(id);
        return a == m_replayCtx.agents.end() || a->second.playerName.empty() ? std::string("?") : a->second.playerName;
    };
    if (e.causeSkill)
        ImGui::Text("From %s, %s", GetSkillDisplayName(e.causeSkill).c_str(), nameOf(e.caster).c_str());
    else if (e.caster && e.caster != agentId)
    {
        // "Cast by [primary][secondary] Name", the name in the caster's team colour.
        ImGui::TextUnformatted("Cast by");
        auto a = m_replayCtx.agents.find(e.caster);
        const float sz = ImGui::GetTextLineHeight();
        if (a != m_replayCtx.agents.end())
        {
            for (int prof : { a->second.primaryProf, a->second.secondaryProf })
                if (ImTextureID ico = LoadProfIcon(m_deviceResources->GetD3DDevice(), prof))
                {
                    ImGui::SameLine(0.f, 4.f);
                    ImGui::Image(ico, ImVec2(sz, sz));
                }
            ImGui::SameLine(0.f, 4.f);
            ImGui::PushStyleColor(ImGuiCol_Text, GetAgentTeamColor(a->second.teamId));
            ImGui::TextUnformatted(nameOf(e.caster).c_str());
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::SameLine();
            ImGui::TextUnformatted("?");
        }
    }
    if (removed)
        ImGui::Text("Removed by %s, %s", GetSkillDisplayName(e.removedBy).c_str(), nameOf(e.removedByCaster).c_str());
    else if (e.hasDuration)
        ImGui::Text("%.1f s left%s", std::max(0.f, e.end - t), e.endExact ? "" : " (estimated)");
    ImGui::EndTooltip();
}


int ReplayWindow::DrawFocusHudEffectBar(int agentId, ImVec2 rowTopLeft, float side, float rowWidth)
{
    // The fixed icons open the bottom row; the effects follow and wrap upwards, a row at a time.
    const int fixedCount = static_cast<int>(std::round(DrawFocusHudFixedEffects(agentId, rowTopLeft, side) / side));
    const int perRow = std::max(1, static_cast<int>(rowWidth / side));

    const EffectTimeline::Table* table = EffectTimelineTable();
    if (!table) return 1;
    const auto all = table->byAgent.find(agentId);
    if (all == table->byAgent.end()) return 1;
    const float t = m_debugTimeline;

    // An effect a known skill removed stays this long after it went, dimmed under that skill's
    // icon, so the viewer sees what took it. The client drops it at once; this is ours.
    constexpr float kRemovedLinger = 0.7f;

    struct Icon
    {
        const EffectTimeline::Effect* longest;
        float placed;
        int   key;
        bool  removed;
    };
    std::vector<Icon> icons;
    for (const EffectTimeline::Effect& e : all->second) {
        if (e.start > t) break;
        const bool active  = t < e.end;
        const bool removed = !active && e.removedBy && t < e.end + kRemovedLinger;
        if (!active && !removed) continue;
        auto it = std::find_if(icons.begin(), icons.end(), [&](const Icon& i) { return i.longest->skillId == e.skillId; });
        if (it == icons.end()) {
            icons.push_back({ &e, e.start, EffectSortKey(e.kind), removed });
            continue;
        }
        // An effect still on the player wins over one just removed; among those still on, the one
        // that runs longest, and a refresh that lengthens it moves it to the end of its group.
        if (it->removed && active) *it = { &e, e.start, EffectSortKey(e.kind), false };
        else if (!removed && e.end > it->longest->end) { it->longest = &e; it->placed = e.start; }
    }
    if (icons.empty()) return 1;
    std::stable_sort(icons.begin(), icons.end(), [](const Icon& a, const Icon& b) {
        return a.key != b.key ? a.key < b.key : a.placed < b.placed;
    });

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    EnsureSkillIconIndex();
    ImTextureID texSlots = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_slot_frames.png");
    const bool windowHovered = ImGui::IsWindowHovered();
    const ImVec2 iconUV0(0.0625f, 0.0625f), iconUV1(0.9375f, 0.9375f);

    for (size_t i = 0; i < icons.size(); ++i) {
        const EffectTimeline::Effect& e = *icons[i].longest;
        const SkillInfo* si = m_skillView.Get(e.skillId);
        const int slotIdx = fixedCount + static_cast<int>(i);
        const ImVec2 tl(rowTopLeft.x + static_cast<float>(slotIdx % perRow) * side,
                        rowTopLeft.y - static_cast<float>(slotIdx / perRow) * side);
        const ImVec2 br(tl.x + side, tl.y + side);
        ImTextureID tex = LoadSkillIcon(this, dev, e.skillId, m_skillIconIndex, m_skillIconCache);
        ImVec2 fuv0, fuv1;
        FrameCellUV(EffectFrameCell(e.kind, si), fuv0, fuv1);

        // The duration bar runs against the time the effect was given, so a removed one goes with
        // part of its bar still left - as in the client.
        const float given = e.expectedEnd > e.end ? e.expectedEnd : e.end;
        auto drawBar = [&](float at, float opacity) {
            if (!e.hasDuration || given <= e.start) return;
            const float w  = std::round(0.85f * side);
            const float x0 = tl.x + std::round(0.075f * side);
            const float y1 = br.y - std::floor(0.04f * side);
            const float y0 = y1 - 3.f;
            const float frac = std::clamp((given - at) / (given - e.start), 0.f, 1.f);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y1), IM_COL32(0, 0, 0, static_cast<int>(255.f * opacity)));
            const float fw = std::round((w - 2.f) * frac);
            if (fw > 0.f)
                dl->AddRectFilled(ImVec2(x0 + 1.f, y0 + 1.f), ImVec2(x0 + 1.f + fw, y1 - 1.f), EffectBarColour(e.kind, si, opacity));
        };

        const float age = t - icons[i].placed;
        if (icons[i].removed) {
            const float since = t - e.end;
            const float fade = std::clamp((kRemovedLinger - since) / 0.2f, 0.f, 1.f);
            const ImU32 dim = IM_COL32(150, 150, 150, static_cast<int>(200.f * fade));
            if (tex) dl->AddImage(tex, tl, br, iconUV0, iconUV1, dim);
            if (texSlots) dl->AddImage(texSlots, tl, br, fuv0, fuv1, dim);
            drawBar(e.end, 0.8f * fade);
            // The remover pops in over the top-left corner: up to 1.25 in 0.12 s, settling at 1 by 0.24 s.
            const float pop = since < 0.12f ? 1.25f * since / 0.12f
                            : since < 0.24f ? 1.25f - 0.25f * (since - 0.12f) / 0.12f : 1.f;
            const float h = side * 0.62f * pop * 0.5f;
            // Anchored in the top-left corner, border flush with the edge, so the pop grows from there.
            const ImVec2 c(tl.x + 1.f + h, tl.y + 1.f + h);
            if (ImTextureID by = LoadSkillIcon(this, dev, e.removedBy, m_skillIconIndex, m_skillIconCache); by && h > 0.5f) {
                const ImVec2 b0(c.x - h, c.y - h), b1(c.x + h, c.y + h);
                fg->AddRectFilled(ImVec2(b0.x - 1.f, b0.y - 1.f), ImVec2(b1.x + 1.f, b1.y + 1.f), IM_COL32(0, 0, 0, static_cast<int>(230.f * fade)));
                fg->AddImage(by, b0, b1, iconUV0, iconUV1, IM_COL32(255, 255, 255, static_cast<int>(255.f * fade)));
            }
        } else if (age < 0.5f) {
            const float f = std::min(age / 0.2f, 1.f);
            const float alpha = std::cbrt(f);
            const float size = age < 0.2f ? 0.25f + (kPopPeak - 0.25f) * std::cbrt(f)
                                          : kPopPeak - (kPopPeak - 1.f) * std::pow((age - 0.2f) / 0.3f, 3.f);
            const ImVec2 c(tl.x + side * 0.5f, tl.y + side * 0.5f);
            const float h = side * size * 0.5f;
            const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(255.f * alpha));
            if (tex) fg->AddImage(tex, ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), iconUV0, iconUV1, tint);
            if (texSlots) fg->AddImage(texSlots, ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), fuv0, fuv1, tint);
        } else {
            // The client blinks against the time it gave the effect: through a natural end, and
            // through a removal only when that falls inside the blink.
            const bool blinks = e.hasDuration && (e.natural || given > e.end + 0.01f);
            const float opacity = blinks ? EffectBlinkOpacity(t - e.start, given - e.start) : 1.f;
            const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(255.f * opacity));
            if (tex) dl->AddImage(tex, tl, br, iconUV0, iconUV1, tint);
            else dl->AddRectFilled(tl, br, IM_COL32(0x40, 0x40, 0x40, static_cast<int>(0xC0 * opacity)));
            if (texSlots) dl->AddImage(texSlots, tl, br, fuv0, fuv1, tint);
            drawBar(t, opacity);
        }

        if (windowHovered && ImGui::IsMouseHoveringRect(tl, br))
            DrawEffectIconTooltip(e, agentId, icons[i].removed);
    }
    return (fixedCount + static_cast<int>(icons.size()) + perRow - 1) / perRow;
}


// ---------------------------------------------------------------------------
// Morale + effects beside a party window row - Iris's strip beside GW's own party window
// (Gwtoolbox-Iris plugins/Iris/Iris.cpp DrawEffectStrips), same size, order and rules: outside the
// window, one strip per player row; terrain, then morale (its arrow and "+10%"), then the effects
// in the effect bar's order (EffectSortKey); one icon per skill, timed by its longest instance, a
// longer refresh moving to the end of its group; 0.95 of the row high (12..26 px), 1 px apart; no
// frames or kind colours (owner, 2026-10-22), a white duration bar, a thin black outline; "+N"
// past 8 effects. Timing as the HUD's: the bar and the blink run against the time it was given.
// ---------------------------------------------------------------------------
void ReplayWindow::DrawPartyEffectStrip(int agentId, float anchorX, bool toLeft, float rowY, float rowH, float clipY0, float clipY1)
{
    auto agentIt = m_replayCtx.agents.find(agentId);
    if (agentIt == m_replayCtx.agents.end() || clipY1 <= clipY0) return;
    const AgentReplayData& ard = agentIt->second;
    const float t = m_debugTimeline;
    const float side = PartyEffectIconSize(rowH);
    constexpr float kGap = 1.f;   // GW's icons nearly touch

    struct Icon
    {
        const EffectTimeline::Effect* e;   // null = morale
        float placed;
        int   key;
    };
    std::vector<Icon> icons;
    int deaths = 0, boosts = 0;
    const int morale = ComputeAgentMorale(ard, t, &deaths, &boosts);
    if (morale != 0) icons.push_back({ nullptr, 0.f, 1 });
    if (const EffectTimeline::Table* table = EffectTimelineTable())
        if (const auto all = table->byAgent.find(agentId); all != table->byAgent.end())
            for (const EffectTimeline::Effect& e : all->second) {
                if (e.start > t) break;
                if (t >= e.end) continue;
                auto it = std::find_if(icons.begin(), icons.end(), [&](const Icon& i) { return i.e && i.e->skillId == e.skillId; });
                if (it == icons.end()) icons.push_back({ &e, e.start, EffectSortKey(e.kind) });
                else if (e.end > it->e->end) { it->e = &e; it->placed = e.start; }
            }
    if (icons.empty()) return;
    std::stable_sort(icons.begin(), icons.end(), [](const Icon& a, const Icon& b) {
        return a.key != b.key ? a.key < b.key : a.placed < b.placed;
    });
    const size_t cap = kPartyEffectsMax + (morale != 0 ? 1u : 0u);
    const size_t hidden = icons.size() > cap ? icons.size() - cap : 0;
    icons.resize(icons.size() - hidden);

    // Under every window, as Iris draws under GW's: whatever covers the strip hides it.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    EnsureSkillIconIndex();
    ImFont* font = ImGui::GetFont();
    const bool canHover = !ImGui::GetIO().WantCaptureMouse;
    const ImVec2 iconUV0(0.0625f, 0.0625f), iconUV1(0.9375f, 0.9375f);
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    auto outlined = [&](float fs, ImVec2 p, ImU32 col, const char* s) {
        for (const ImVec2 d : { ImVec2(-1, 0), ImVec2(1, 0), ImVec2(0, -1), ImVec2(0, 1) })
            dl->AddText(font, fs, ImVec2(p.x + d.x, p.y + d.y), IM_COL32(0, 0, 0, 255), s);
        dl->AddText(font, fs, p, col, s);
    };
    float x = anchorX;
    auto place = [&](float width) {
        const float at = toLeft ? x - width : x;
        x = toLeft ? at - kGap : at + width + kGap;
        return std::round(at);
    };
    const float y = std::round(rowY + (rowH - side) * 0.5f);
    auto hovered = [&](ImVec2 tl, ImVec2 br) {
        return canHover && ImGui::IsMouseHoveringRect(ImVec2(tl.x, std::max(tl.y, clipY0)),
                                                      ImVec2(br.x, std::min(br.y, clipY1)), false);
    };

    dl->PushClipRect(ImVec2(vp->Pos.x, clipY0), ImVec2(vp->Pos.x + vp->Size.x, clipY1), true);
    for (const Icon& icon : icons) {
        const ImVec2 tl(place(side), y), br(tl.x + side, tl.y + side);
        if (!icon.e) {
            // GW's morale icon, its text centred at the bottom in Friz-gold 0xFFEEBB.
            if (ImTextureID tex = LoadGameUITexture(dev, morale > 0 ? "Effectbar\\ui_effect_morale_boost.png"
                                                                    : "Effectbar\\ui_effect_morale_penalty.png"))
                dl->AddImage(tex, tl, br);
            char num[16];
            snprintf(num, sizeof(num), "%+d%%", morale);
            const float fs = std::clamp(side * 0.5f, 9.f, 14.f);
            const ImVec2 ext = font->CalcTextSizeA(fs, FLT_MAX, 0.f, num);
            outlined(fs, ImVec2(std::round(tl.x + (side - ext.x) * 0.5f), std::round(br.y - ext.y)),
                     IM_COL32(0xFF, 0xEE, 0xBB, 255), num);
            if (hovered(tl, br)) DrawMoraleTooltip(morale, deaths, boosts);
            continue;
        }
        const EffectTimeline::Effect& e = *icon.e;
        const float given = e.expectedEnd > e.end ? e.expectedEnd : e.end;
        const bool blinks = e.hasDuration && (e.natural || given > e.end + 0.01f);
        const float opacity = blinks ? EffectBlinkOpacity(t - e.start, given - e.start) : 1.f;
        const int a = static_cast<int>(255.f * opacity);
        if (ImTextureID tex = LoadSkillIcon(this, dev, e.skillId, m_skillIconIndex, m_skillIconCache))
            dl->AddImage(tex, tl, br, iconUV0, iconUV1, IM_COL32(255, 255, 255, a));
        else
            dl->AddRectFilled(tl, br, IM_COL32(40, 40, 40, static_cast<int>(220.f * opacity)));
        if (e.hasDuration && given > e.start) {
            // GW's duration bar: 3 px black, 85% of the icon wide, 2 px above its bottom, a 1 px line.
            const float w  = std::round(side * 0.85f);
            const float x0 = tl.x + std::round((side - w) * 0.5f);
            const float y0 = br.y - 2.f - 3.f;
            const float frac = std::clamp((given - t) / (given - e.start), 0.f, 1.f);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + 3.f), IM_COL32(0, 0, 0, a));
            dl->AddRectFilled(ImVec2(x0 + 1.f, y0 + 1.f), ImVec2(x0 + 1.f + std::round((w - 2.f) * frac), y0 + 2.f),
                              IM_COL32(255, 255, 255, a));
        }
        dl->AddRect(tl, br, IM_COL32(0, 0, 0, 200));
        if (hovered(tl, br)) DrawEffectIconTooltip(e, agentId, false);
    }
    if (hidden) {
        char more[16];
        snprintf(more, sizeof(more), "+%zu", hidden);
        const float fs = std::clamp(side * 0.7f, 10.f, 16.f);
        const ImVec2 ext = font->CalcTextSizeA(fs, FLT_MAX, 0.f, more);
        outlined(fs, ImVec2(place(ext.x), std::round(y + (side - ext.y) * 0.5f)), IM_COL32(230, 230, 230, 255), more);
    }
    dl->PopClipRect();
}


// ---------------------------------------------------------------------------
// Health bar over the skill bar: the client's StatHealth window (GmAgentStatus, style 0xa2000;
// decoded from Gw.exe 2026-10-05). The fills are the party bars' (UiCtlProgress), in the game's
// own red whatever the team -- this is the player's own bar -- changed by poison > bleeding >
// degeneration hex with a 0.2 s cross-fade, Deep Wound capping the last fifth. Unlike a party row
// the bar has no inset: the fill covers the whole rect, and the HUD frame 205437 (pewter, metal
// caps both ends) is drawn over it on the rect grown 7 UI units left/right and 2 up/down.
// The value sits centred in F0F0F0; the regeneration pips (DAT 31522) start one
// arrow width from the widest value shown so far -- degeneration to the left, regeneration to the
// right -- pips = round(hp_pips * max health / 2), one pip being 2 health per second, at most 10.
// ---------------------------------------------------------------------------
namespace
{
    // FrApi's nine-slice as the HUD bars meet it: margins art/3 drawn texel for texel, and a rect
    // shorter than two margins is cut at its middle, each half showing its own edge 1:1 rather
    // than the corners being shrunk.
    void DrawHudBarFrame(ImDrawList* dl, ImTextureID tex, ImVec2 r0, ImVec2 r1, float scale)
    {
        constexpr float art = 32.f, m = art / 3.f;
        const float mx = m * scale;
        const float h = r1.y - r0.y;
        // Rows: top band, middle (stretched; empty for a short rect), bottom band.
        const float band  = std::min(mx, h * 0.5f);
        const float tBand = band / scale;
        const float ys[4] = { r0.y, r0.y + band, r1.y - band, r1.y };
        const float vs[4] = { 0.f, tBand / art, 1.f - tBand / art, 1.f };
        const float xs[4] = { r0.x, r0.x + mx, r1.x - mx, r1.x };
        const float us[4] = { 0.f, m / art, 1.f - m / art, 1.f };
        for (int row = 0; row < 3; ++row) {
            if (ys[row + 1] <= ys[row]) continue;
            for (int col = 0; col < 3; ++col) {
                if (xs[col + 1] <= xs[col]) continue;
                dl->AddImage(tex, ImVec2(xs[col], ys[row]), ImVec2(xs[col + 1], ys[row + 1]),
                             ImVec2(us[col], vs[row]), ImVec2(us[col + 1], vs[row + 1]));
            }
        }
    }
}


void ReplayWindow::DrawFocusHudHealthBar(const AgentReplayData& ard, ImVec2 b0, ImVec2 b1, float scale)
{
    const float t = m_debugTimeline;
    const AgentSnapshot* snap = FindSnapshotAtTime(ard, t);
    if (!snap) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    auto tex = [&](const char* file) {
        return LoadGameUITexture(dev, (std::string("Progressbar\\") + file).c_str());
    };
    const bool dead = snap->is_dead;

    const char* fill = "ui_progress_health.png";
    if (!dead) {
        if (snap->has_poison)         fill = "ui_progress_health_poisoned.png";
        else if (snap->has_bleeding)  fill = "ui_progress_health_bleeding.png";
        else if (snap->has_degen_hex) fill = "ui_progress_health_hexed.png";
    }

    // 0.2 s smoothstep cross-fade from the previous fill, on the replay clock. The widest value
    // shown is kept with it: the client never lets the pips move back in.
    struct BarState { int agentId = -1; std::string fill, prev; float changed = -1.f; float textW = 0.f; };
    // One per agent: the HUD and the split camera each draw a bar, often for different players.
    static std::unordered_map<int, BarState> s_bars;
    BarState& s_bar = s_bars[ard.agent_id];
    if (s_bar.agentId != ard.agent_id) s_bar = { ard.agent_id, fill, "", -1.f, 0.f };
    if (s_bar.fill != fill) { s_bar.prev = s_bar.fill; s_bar.fill = fill; s_bar.changed = t; }
    float prevAlpha = 0.f;
    if (!s_bar.prev.empty() && t >= s_bar.changed && t - s_bar.changed < 0.2f) {
        const float x = (t - s_bar.changed) / 0.2f;
        prevAlpha = 1.f - x * x * (3.f - 2.f * x);
    }

    const bool  deepWound = snap->has_deep_wound && !dead;
    const float hp = dead ? 0.f : std::clamp(snap->health_pct, 0.f, 1.f);
    DrawGameStatBarFill(dl, tex(fill), b0, b1, deepWound ? hp * 0.8f : hp);
    if (prevAlpha > 0.f && !deepWound)
        DrawGameStatBarFill(dl, tex(s_bar.prev.c_str()), b0, b1, hp,
                            IM_COL32(255, 255, 255, static_cast<int>(prevAlpha * 255.f)));
    if (deepWound)
        DrawGameStatBarFill(dl, tex("ui_health_deep_wound.png"), b0, b1, 0.8f);
    if (ImTextureID frame = tex("ui_hud_bar_frame.png"))
        DrawHudBarFrame(dl, frame, ImVec2(b0.x - 7.f * scale, b0.y - 2.f * scale),
                        ImVec2(b1.x + 7.f * scale, b1.y + 2.f * scale), scale);

    const MaxHpSample mhp = ResolveMaxHp(ard, t);
    if (mhp.value == 0) return;

    const float cx = std::round((b0.x + b1.x) * 0.5f), cy = (b0.y + b1.y) * 0.5f;
    char num[16];
    snprintf(num, sizeof(num), "%d", static_cast<int>(std::lround(hp * static_cast<float>(mhp.value))));
    // The bar scales with the window and the text with it: never taller than the bar allows.
    ImFont* font = ImGui::GetFont();
    const float fs = std::min(ImGui::GetFontSize(), std::max(8.f, std::round(b1.y - b0.y)));
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, num);
    const ImVec2 tp(std::round(cx - ts.x * 0.5f), std::round(cy - ts.y * 0.5f));
    dl->AddText(font, fs, ImVec2(tp.x + 1.f, tp.y + 1.f), IM_COL32(0, 0, 0, 0xCC), num);
    dl->AddText(font, fs, tp, IM_COL32(0xF0, 0xF0, 0xF0, 0xFF), num);
    s_bar.textW = std::max(s_bar.textW, ts.x);

    if (dead) return;
    const int pips = std::clamp(static_cast<int>(std::lround(snap->hp_pips * static_cast<float>(mhp.value) / 2.f)), -10, 10);
    ImTextureID arrows = tex("ui_health_arrows.png");
    if (pips == 0 || !arrows) return;

    const float ah = fs;
    const float aw = std::round(ah * 10.f / 16.f);
    const int   n  = std::abs(pips);
    const float D  = s_bar.textW + 2.f * aw;
    const float G  = static_cast<float>(n + 1) * aw;
    const float y0 = std::round(cy - ah * 0.5f);
    if (pips > 0) {
        const float S = std::min(cx + D * 0.5f, b1.x - G);
        for (int k = 0; k < n; ++k)
            dl->AddImage(arrows, ImVec2(S + k * aw, y0), ImVec2(S + (k + 1) * aw, y0 + ah),
                         ImVec2(10.f / 32.f, 0.f), ImVec2(20.f / 32.f, 1.f));
    } else {
        const float S = std::max(cx - D * 0.5f, b0.x + G);
        for (int k = 0; k < n; ++k)
            dl->AddImage(arrows, ImVec2(S - (k + 1) * aw, y0), ImVec2(S - k * aw, y0 + ah),
                         ImVec2(0.f, 0.f), ImVec2(10.f / 32.f, 1.f));
    }
}


// Energy bar above the followed player's health bar, in the same HUD frame: the game's blue fill,
// the value centred, the regeneration arrows beside it.
void ReplayWindow::DrawFocusHudEnergyBar(const AgentReplayData& ard, ImVec2 b0, ImVec2 b1, float scale)
{
    const EnergyModel::Track* track = EnergyTrackFor(ard.agent_id);
    if (!track) return;
    const EnergyModel::Sample s = track->At(m_debugTimeline);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    auto tex = [&](const char* file) {
        return LoadGameUITexture(dev, (std::string("Progressbar\\") + file).c_str());
    };

    dl->AddRectFilled(b0, b1, IM_COL32(0, 0, 0, 0xA0));
    if (!s.dead) DrawGameStatBarFill(dl, tex("ui_progress_energy.png"), b0, b1, EnergyBarFraction(s));
    DrawEnergyOvercast(dl, b0, b1, s, tex("texture_0x32274.png"));
    DrawEnergyBand(dl, b0, b1, s);
    if (ImTextureID frame = tex("ui_hud_bar_frame.png"))
        DrawHudBarFrame(dl, frame, ImVec2(b0.x - 7.f * scale, b0.y - 2.f * scale),
                        ImVec2(b1.x + 7.f * scale, b1.y + 2.f * scale), scale);

    // The client shows the whole points, and 0 while the pool is below zero.
    const int value = s.dead ? 0 : std::max(0, static_cast<int>(std::floor(s.energy)));
    char num[16];
    snprintf(num, sizeof(num), "%d", value);
    const float cx = std::round((b0.x + b1.x) * 0.5f), cy = (b0.y + b1.y) * 0.5f;
    ImFont* font = ImGui::GetFont();
    const float fs = std::min(ImGui::GetFontSize(), std::max(8.f, std::round(b1.y - b0.y)));
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, num);
    const ImVec2 tp(std::round(cx - ts.x * 0.5f), std::round(cy - ts.y * 0.5f));
    dl->AddText(font, fs, ImVec2(tp.x + 1.f, tp.y + 1.f), IM_COL32(0, 0, 0, 0xCC), num);
    dl->AddText(font, fs, tp, IM_COL32(0xF0, 0xF0, 0xF0, 0xFF), num);

    // How sure the model is: the lamp just right of the bar's frame, past the HUD window's own
    // edge (so drawn unclipped). Hovering it says what the colours mean and why it shows this one.
    const float lampD = std::round(b1.y - b0.y + 4.f * scale);
    const ImVec2 lampC(std::round(b1.x + 10.f * scale + lampD * 0.5f), std::round(cy));
    dl->PushClipRectFullScreen();
    DrawEnergyConfidenceLamp(dl, tex("ui_confidence_lamps.png"), lampC, lampD, track->ConfidenceAt(m_debugTimeline));
    dl->PopClipRect();
    const float lampR = lampD * 0.5f;
    if (ImGui::IsMouseHoveringRect(ImVec2(lampC.x - lampR, lampC.y - lampR), ImVec2(lampC.x + lampR, lampC.y + lampR), false))
        DrawEnergyConfidenceTooltip(*track);
    else if (ImGui::IsMouseHoveringRect(b0, b1))
        DrawEnergyBarTooltip(ard, *track, 0.f, false);

    ImTextureID arrows = tex("ui_health_arrows.png");
    const int pips = s.dead ? 0 : static_cast<int>(s.pips);
    if (pips == 0 || !arrows) return;
    const float ah = fs;
    const float aw = std::round(ah * 10.f / 16.f);
    const int   n  = std::abs(pips);
    const float D  = ts.x + 2.f * aw;
    const float G  = static_cast<float>(n + 1) * aw;
    const float y0 = std::round(cy - ah * 0.5f);
    if (pips > 0) {
        const float S = std::min(cx + D * 0.5f, b1.x - G);
        for (int k = 0; k < n; ++k)
            dl->AddImage(arrows, ImVec2(S + k * aw, y0), ImVec2(S + (k + 1) * aw, y0 + ah),
                         ImVec2(10.f / 32.f, 0.f), ImVec2(20.f / 32.f, 1.f));
    } else {
        const float S = std::max(cx - D * 0.5f, b0.x + G);
        for (int k = 0; k < n; ++k)
            dl->AddImage(arrows, ImVec2(S - (k + 1) * aw, y0), ImVec2(S - k * aw, y0 + ah),
                         ImVec2(0.f, 0.f), ImVec2(10.f / 32.f, 1.f));
    }
}



// ---------------------------------------------------------------------------
// The client's skill tooltip, for a slot of the followed player's bar: the name with its costs
// right-aligned (value, then the game's glyph), then one paragraph -- "<Type>." + the description
// with every "A...B" answered at this player's solved rank, in green, + "(Attrib: <attribute>)".
// The replay state (what disabled it, how long it is still out) follows under a rule, muted.
// ---------------------------------------------------------------------------
void ReplayWindow::DrawGameSkillTooltip(int skillId, int agentId, const SkillCooldownState* cd, float usedAt)
{
    const SkillInfo* si = m_skillView.Get(skillId);
    if (!si) return;
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();

    constexpr ImU32 kName   = IM_COL32(0xF2, 0xEC, 0xD6, 0xFF);
    constexpr ImU32 kPlain  = IM_COL32(0xE6, 0xE6, 0xE6, 0xFF);
    constexpr ImU32 kGreen  = IM_COL32(0x5C, 0xE6, 0x3C, 0xFF);
    constexpr ImU32 kMuted  = IM_COL32(0xA8, 0xA8, 0xA0, 0xFF);
    constexpr float kWrap   = 300.f;

    // Costs, in the client's order.
    struct Cost { std::string value; const char* icon; };
    std::vector<Cost> costs;
    auto secs = [](float v) {
        char b[16];
        if (v == 0.25f) snprintf(b, sizeof(b), "\xC2\xBC");
        else if (v == 0.5f) snprintf(b, sizeof(b), "\xC2\xBD");
        else if (v == 0.75f) snprintf(b, sizeof(b), "\xC2\xBE");
        else if (v == std::floor(v)) snprintf(b, sizeof(b), "%d", static_cast<int>(v));
        else snprintf(b, sizeof(b), "%.1f", v);
        return std::string(b);
    };
    if (si->upkeep < 0)     costs.push_back({ std::to_string(si->upkeep), "upkeep.png" });
    if (si->energy > 0) {
        // What the energy model charged this player the last time they used it (Expertise,
        // Mysticism, Quickening Zephyr, a glyph), then the listed cost in brackets - "5 (10)", as
        // the wiki writes it. Only the listed cost until the skill has been used.
        std::string energy = std::to_string(si->energy);
        if (const EnergyModel::Track* track = EnergyTrackFor(agentId)) {
            const int wanted = m_skillView.ResolvePvpSkillId(skillId);
            for (auto c = track->castLog.rbegin(); c != track->castLog.rend(); ++c) {
                if (c->time > m_debugTimeline || m_skillView.ResolvePvpSkillId(c->skillId) != wanted) continue;
                const int paid = static_cast<int>(std::lround(c->charged));
                if (paid != si->energy) energy = std::to_string(paid) + " (" + std::to_string(si->energy) + ")";
                break;
            }
        }
        costs.push_back({ energy, "energy.png" });
    }
    if (si->adrenaline > 0) costs.push_back({ std::to_string(si->adrenaline), "adrenaline.png" });
    if (si->sacrifice > 0)  costs.push_back({ std::to_string(si->sacrifice) + "%", "sacrifice.png" });
    if (si->overcast > 0)   costs.push_back({ std::to_string(si->overcast), "overcast.png" });
    if (si->activation > 0) costs.push_back({ secs(si->activation), "activation.png" });
    if (si->recharge > 0)   costs.push_back({ secs(si->recharge), "recharge.png" });

    const float lineH = ImGui::GetTextLineHeight();
    const float iconSz = lineH;
    float costsW = 0.f;
    for (const auto& c : costs) costsW += ImGui::CalcTextSize(c.value.c_str()).x + 2.f + iconSz + 8.f;
    const float nameW = ImGui::CalcTextSize(si->name.c_str()).x;
    const float width = std::max(kWrap, nameW + 16.f + costsW);

    // One paragraph: type, description at this player's rank, attribute. The rank is the one the
    // skill read at `usedAt` (now, unless the caller names when it landed), which for a signet can
    // be another attribute's.
    int rankAttr = si->attribute;
    const float rankTime = usedAt < 0.f ? m_debugTimeline : usedAt;
    const AttributeModel::AttributeRange* rank = SkillRankAt(skillId, agentId, rankTime, &rankAttr);
    // A rank lower while the effect timeline reads the player Weakened then.
    AttributeModel::AttributeRange lowered;
    if (const EffectTimeline::Table* fx = rank ? EffectTimelineTable() : nullptr; fx && fx->WeaknessAt(agentId, rankTime) == 1) {
        lowered.lo = std::max(0, rank->lo - 1);
        lowered.hi = std::max(0, rank->hi - 1);
        lowered.best = std::max(0, rank->best - 1);
        lowered.confidence = rank->confidence;
        lowered.budgetOnly = rank->budgetOnly;
        rank = &lowered;
    }

    std::vector<SkillTextRun> runs;
    std::string lead = std::string(si->is_elite ? "Elite " : "") + SkillDatabase::GetTypeName(si->type) + ". ";
    runs.push_back({ lead, kPlain });
    for (auto& r : BuildSkillTextRuns(*si, rank, kPlain, kGreen)) runs.push_back(std::move(r));
    const char* attrName = SkillDatabase::GetAttributeName(si->attribute);
    const char* rankName = SkillDatabase::GetAttributeName(rankAttr);
    if (rankAttr != si->attribute && rank && rankName && rankName[0])
        runs.push_back({ std::string(" (Attrib: ") + (attrName ? attrName : "") + "; read at " + rankName + " " +
                         (rank->lo == rank->hi ? std::to_string(rank->lo)
                                               : std::to_string(rank->lo) + "-" + std::to_string(rank->hi)) + ")", kPlain });
    else if (si->attribute < 101 && attrName && attrName[0])
        runs.push_back({ std::string(" (Attrib: ") + attrName + ")", kPlain });

    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.11f, 0.105f, 0.085f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border,  ImVec4(0.62f, 0.58f, 0.46f, 0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.f, 5.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::BeginTooltip();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();

    // Header: name left, costs right.
    dl->AddText(p0, kName, si->name.c_str());
    float x = p0.x + width - costsW + 8.f;
    for (const auto& c : costs) {
        dl->AddText(ImVec2(x, p0.y), kName, c.value.c_str());
        x += ImGui::CalcTextSize(c.value.c_str()).x + 2.f;
        if (ImTextureID icon = LoadGameUITexture(dev, (std::string("Skill Description\\") + c.icon).c_str()))
            dl->AddImage(icon, ImVec2(x, p0.y), ImVec2(x + iconSz, p0.y + iconSz));
        x += iconSz + 8.f;
    }

    const ImVec2 bodyAt(p0.x, p0.y + lineH + 2.f);
    const ImVec2 body = DrawSkillTextRuns(dl, bodyAt, width, runs);
    ImGui::Dummy(ImVec2(width, lineH + 2.f + body.y));

    // The replay's own word on the slot.
    if (cd) {
        std::string state;
        char buf[192];
        using CdState = SkillCooldownState::State;
        if (cd->state == CdState::Disabled) {
            const SkillInfo* src = m_skillView.Get(cd->disableSourceSkill);
            std::string who;
            if (cd->disableSourceAgent == agentId) who = "own skill";
            else if (auto ait = m_replayCtx.agents.find(cd->disableSourceAgent); ait != m_replayCtx.agents.end())
                who = ait->second.playerName;
            snprintf(buf, sizeof(buf), "Disabled by %s%s%s%s: %.0fs", src ? src->name.c_str() : "?",
                     who.empty() ? "" : " (", who.c_str(), who.empty() ? "" : ")", std::ceil(cd->remaining));
            state = buf;
            // A length read off a rank the solve did not pin: say how far it may run.
            if (std::ceil(cd->disableRemainingHi) > std::ceil(cd->disableRemainingLo)) {
                snprintf(buf, sizeof(buf), " (%.0f-%.0fs by rank)", std::ceil(cd->disableRemainingLo),
                         std::ceil(cd->disableRemainingHi));
                state += buf;
            }
        } else if (cd->state == CdState::Recharging) {
            snprintf(buf, sizeof(buf), "Recharging: %.0fs", std::ceil(cd->remaining));
            state = buf;
        } else if (cd->state == CdState::Spent) {
            state = "Used: back on a morale boost";
        }
        if (!cd->modifierText.empty())
            state += (state.empty() ? "" : "\n") + std::string("Recharge: ") + cd->modifierText;
        if (!cd->slotNote.empty())
            state += (state.empty() ? "" : "\n") + cd->slotNote;
        if (!state.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kMuted));
            ImGui::TextUnformatted(state.c_str());
            ImGui::PopStyleColor();
        }
        if (rank) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kMuted));
            ImGui::Text("Green: this player's %s, read off the match.", rankName);
            ImGui::PopStyleColor();
        }
    }

    ImGui::EndTooltip();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}
