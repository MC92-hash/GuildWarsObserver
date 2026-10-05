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
    const float frameY = std::round(hpArtT + hpH + enGap + hpH);
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
    ImTextureID texSlots   = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_slot_frames.png");
    ImTextureID texKeys    = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_keycaps.png");
    ImTextureID texMarkers = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_markers.png");
    ImTextureID texNoSign  = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_check_forbidden.png");
    ImTextureID texSparkle = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_sparkle.png");

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

    for (int i = 0; i < kBarSlots; ++i)
    {
        const ImVec2 tl(origin.x + artL + i * slot, frameTL.y + artT);
        const ImVec2 br(tl.x + slot, tl.y + slot);
        const bool filled = i < static_cast<int>(bar.size());
        const SkillInfo* si = filled ? m_skillView.Get(bar[i]) : nullptr;

        // Icon (or the empty slot's grey), then the slot frame over it.
        if (!filled)
            dl->AddRectFilled(tl, br, IM_COL32(0x40, 0x40, 0x40, 0xC0));
        else if (ImTextureID tex = LoadSkillIcon(this, dev, bar[i], m_skillIconIndex, m_skillIconCache))
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
        if (texSparkle && bar[i] == usingSkill) {
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
        if (texKeys) {
            const float ks = 24.f * scale;
            const float kx = static_cast<float>(i % 5) * 24.f / 128.f, ky = static_cast<float>(i / 5) * 24.f / 128.f;
            dl->AddImage(texKeys, ImVec2(br.x - ks, br.y - ks), br,
                         ImVec2(kx, ky), ImVec2(kx + 24.f / 128.f, ky + 24.f / 128.f));
        }

        if (windowHovered && ImGui::IsMouseHoveringRect(tl, br))
            DrawGameSkillTooltip(bar[i], agentId, &cd);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
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
    static BarState s_bar;
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
    DrawEnergyOvercast(dl, b0, b1, s);
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

    if (ImGui::IsMouseHoveringRect(b0, b1))
        ImGui::SetTooltip("Energy %d / %d (calculated)\nRegeneration %+d pips%s", value,
                          static_cast<int>(std::floor(s.maxEnergy)), static_cast<int>(s.pips),
                          s.overcast >= 1.f ? "\nOvercast shown in grey" : "");

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
void ReplayWindow::DrawGameSkillTooltip(int skillId, int agentId, const SkillCooldownState* cd)
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
    if (si->energy > 0)     costs.push_back({ std::to_string(si->energy), "energy.png" });
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

    // One paragraph: type, description at this player's rank, attribute.
    const AttributeModel::AttributeRange* rank = nullptr;
    if (auto bit = m_attrProfiles.find(agentId); bit != m_attrProfiles.end())
        if (auto it = bit->second.attributes.find(si->attribute); it != bit->second.attributes.end())
            if (!it->second.budgetOnly) rank = &it->second;

    std::vector<SkillTextRun> runs;
    std::string lead = std::string(si->is_elite ? "Elite " : "") + SkillDatabase::GetTypeName(si->type) + ". ";
    runs.push_back({ lead, kPlain });
    for (auto& r : BuildSkillTextRuns(*si, rank, kPlain, kGreen)) runs.push_back(std::move(r));
    const char* attrName = SkillDatabase::GetAttributeName(si->attribute);
    if (si->attribute < 101 && attrName && attrName[0])
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
        } else if (cd->state == CdState::Recharging) {
            snprintf(buf, sizeof(buf), "Recharging: %.0fs", std::ceil(cd->remaining));
            state = buf;
        } else if (cd->state == CdState::Spent) {
            state = "Used: back on a morale boost";
        }
        if (!cd->modifierText.empty())
            state += (state.empty() ? "" : "\n") + std::string("Recharge: ") + cd->modifierText;
        if (!state.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kMuted));
            ImGui::TextUnformatted(state.c_str());
            ImGui::PopStyleColor();
        }
        if (rank) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kMuted));
            ImGui::Text("Green: this player's %s, read off the match.", attrName);
            ImGui::PopStyleColor();
        }
    }

    ImGui::EndTooltip();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}
