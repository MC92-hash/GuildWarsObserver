#include "pch.h"
#include "TeamColors.h"
#include "ReplayWindow.h"
#include "AssetBlacklist.h"
#include "MatchRatings.h"
#include "MatchNotes.h"
#include "MatchBookmarks.h"
#include "AgentSnapshotParser.h"
#include "StoCParser.h"
#include "SkillDatabase.h"
#include "MaxHpSolver.h"
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
// Incoming effect display — floating damage/heal numbers on focused agent
// ---------------------------------------------------------------------------

int ReplayWindow::GetFocusedAgentId() const
{
    if (m_cameraMode == CameraMode::FollowAgent && m_followedAgentId >= 0)
        return m_followedAgentId;
    return -1;
}


void ReplayWindow::UpdateIncomingEffects()
{
    float now = m_debugTimeline;
    int focused = GetFocusedAgentId();

    if (focused != m_focusedAgentId || now < m_lastEffectScanTime - 0.5f)
    {
        m_incomingEffects.clear();
        m_floaterToggle = false;
        m_focusedAgentId = focused;
        m_lastEffectScanTime = now;
        return;
    }
    m_focusedAgentId = focused;
    if (focused < 0) { m_incomingEffects.clear(); return; }

    std::erase_if(m_incomingEffects, [&](const IncomingEffect& e) {
        return (now - e.spawnTime) >= kEffectLifetime;
    });

    float scanFrom = m_lastEffectScanTime;
    float scanTo   = now;
    m_lastEffectScanTime = now;
    if (scanTo <= scanFrom) return;

    auto findAgentMaxHp = [&](int agentId, float t) -> uint32_t {
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return 0;
        const auto& snaps = it->second.snapshots;
        if (snaps.empty()) return 0;
        const AgentSnapshot* s = FindSnapshotAtTime(it->second, t);
        if (s && s->max_hp > 0) return s->max_hp;
        auto sit = std::lower_bound(snaps.begin(), snaps.end(), t,
            [](const AgentSnapshot& a, float b) { return a.time < b; });
        for (; sit != snaps.end(); ++sit)
            if (sit->max_hp > 0) return sit->max_hp;
        // A guild hall NPC's maximum is a stated constant, so it is known even when the camera
        // never looked at it. Without this a Guild Lord fell through to the player-only solver,
        // which returns 0 for an NPC, and his damage numbers were dropped for want of a
        // denominator. Deep Wound is the only thing that moves it.
        if (uint32_t fixed = LookupGuildNpcMaxHealth(it->second.modelId))
            return (s && s->has_deep_wound)
                       ? AgentReplayData::ApplyDeepWound(fixed) : fixed;
        // Recorded value first (it is the effective max HP); solved per
        // weapon set only when the recording carries none.
        return it->second.solvedMaxHpAtTime(t);
    };

    const auto& db = m_skillView;

    // Collected here and laid out together at the end, in time order, by the client's rule.
    std::vector<IncomingEffect> batch;
    auto pushEffect = [&](IncomingEffect eff) { batch.push_back(std::move(eff)); };

    // Energy gains and drains, in the client's purple: the energy model's discrete changes (a
    // skill's payout, a critical hit, Soul Reaping, a foe's denial), not regeneration and not
    // weapon swaps.
    if (const EnergyModel::Track* track = EnergyTrackFor(focused))
    {
        auto g = std::upper_bound(track->gains.begin(), track->gains.end(), scanFrom,
                                  [](float v, const EnergyModel::Gain& x) { return v < x.time; });
        for (; g != track->gains.end() && g->time <= scanTo; ++g)
        {
            const int amount = static_cast<int>(std::lround(g->amount));
            if (amount == 0) continue;
            IncomingEffect eff;
            eff.spawnTime = g->time;
            eff.skillId = 0;   // the number alone, no skill icon
            eff.type = IncomingEffectType::Energy;
            eff.label = std::format("{}{}", amount > 0 ? "+" : "-", std::abs(amount));
            pushEffect(std::move(eff));
        }
    }

    // Damage and heal numbers, each with the skill the attribution table names for it (a weapon
    // hit shows the attack icon). The followed player's own damage on himself is left out, as
    // before; his own heals are not.
    const DamageAttribution::Table* attribution = DamageAttributionTable();
    {
        const auto& pops = DamagePopsOn(focused);
        auto it = std::upper_bound(pops.begin(), pops.end(), scanFrom,
                                   [](float v, const DamagePop& p) { return v < p.time; });
        for (; it != pops.end() && it->time <= scanTo; ++it)
        {
            const DamagePop& pop = *it;
            if (pop.casterId == focused && !pop.heal) continue;
            IncomingEffect eff;
            eff.spawnTime = pop.time;
            eff.skillId = pop.skillId;
            eff.type = pop.weaponHit ? IncomingEffectType::BasicAttack
                     : pop.heal      ? IncomingEffectType::Heal
                                     : IncomingEffectType::Damage;
            const uint32_t mhp = CorrectMaxHpForPacket(findAgentMaxHp(focused, pop.time), pop.firstValue);
            eff.label = DamagePopLabel(pop, mhp);
            pushEffect(std::move(eff));
        }
    }

    // Skills that target a foe but only deal damage to the caster (self-damage).
    auto isCasterDamageOnly = [](int skillId) -> bool {
        switch (skillId) {
        case 141:   // Rend Enchantments — "you lose 55..25 Health" per monk enchantment removed
        case 863:   // Order of Apostasy — "you lose 25..15% max Health" per monk enchantment removed
            return true;
        default:
            return false;
        }
    };

    // A skill that landed on the followed player without a number in its own tick shows its icon
    // alone: a hex, a skill that deals no damage, an enchantment by name. A cast that explained a
    // packet in its tick has its icon beside that number already, and a damage skill with nothing
    // to show was blocked, dodged or absorbed.
    for (const auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (agentId == focused) continue;
        for (const auto& su : ard.skillUseHistory)
        {
            if (su.wasCancelled || su.targetId != focused) continue;
            if (su.endTime <= scanFrom || su.endTime > scanTo) continue;
            if (attribution && attribution->Explained(agentId, su.endTime)) continue;

            const SkillInfo* si = db.Get(su.skillId);
            const int skillType = si ? si->type : 0;
            // A bow, spear or other ranged attack is released here and lands later, when its
            // projectile hits: its icon comes with that hit's number, not at the release.
            if (skillType == 2 || skillType == 12 || skillType == 31) continue;
            IncomingEffect eff;
            eff.spawnTime = su.endTime;
            eff.skillId = su.skillId;
            if (skillType == 24)
                eff.type = IncomingEffectType::Hex;
            else if (isCasterDamageOnly(su.skillId) ||
                     (si && !si->description.empty() && si->description.find("damage") == std::string::npos))
                eff.type = IncomingEffectType::Condition;
            else if (skillType == 23 || skillType == 33 || skillType == 34)
            {
                eff.type = IncomingEffectType::Heal;
                eff.label = si ? si->name : "Enchantment";
            }
            else
                continue;
            pushEffect(std::move(eff));
        }
    }

    // Interrupt events from combat log
    for (const auto& ce : m_replayCtx.stocData.combat)
    {
        if (ce.time <= scanFrom || ce.time > scanTo) continue;
        if (ce.target_id != focused) continue;
        if (ce.type != "INTERRUPTED") continue;

        IncomingEffect eff;
        eff.spawnTime = ce.time;
        eff.skillId = (int)ce.value;
        eff.type = IncomingEffectType::Interrupt;
        eff.label = "INTERRUPT";
        pushEffect(std::move(eff));
    }

    if (!batch.empty())
        PlaceFloaters(m_incomingEffects, batch, m_floaterToggle, FloaterBaseScale(focused, now));
}


void ReplayWindow::EnsureBitmapFontsLoaded()
{
    if (m_damageBitmapFont.loaded && m_healBitmapFont.loaded && m_energyBitmapFont.loaded) return;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    auto ddsDir = FindTexturesDDSDir();
    if (ddsDir.empty()) return;

    if (!m_damageBitmapFont.loaded)
        m_damageBitmapFont.Load(dev, (ddsDir / L"GW.EXE_0x753F0FB5.dds").c_str());
    if (!m_healBitmapFont.loaded)
        m_healBitmapFont.Load(dev, (ddsDir / L"GW.EXE_0xA0629E7F.dds").c_str());
    if (!m_energyBitmapFont.loaded)
        m_energyBitmapFont.Load(dev, (ddsDir / L"texture_265569.dds").c_str());
}


// ---------------------------------------------------------------------------
// The client's floater layout (OverheadFloaters): box, spline, place, draw
// ---------------------------------------------------------------------------

// Damage, heal and energy figures are the client's own digits; everything else (an icon alone, a
// skill name, INTERRUPT) is ours and draws as text in a pill.
bool ReplayWindow::FloaterUsesDigits(const IncomingEffect& e)
{
    using T = IncomingEffectType;
    if (e.label.empty()) return false;
    if (e.type == T::Damage || e.type == T::BasicAttack || e.type == T::Energy) return true;
    // A heal floater carries a number, or an enchantment's name when it landed without one.
    const char c = e.label[0];
    return e.type == T::Heal && (c == '+' || c == '-' || (c >= '0' && c <= '9'));
}

bool ReplayWindow::FloaterHasIcon(const IncomingEffect& e)
{
    return e.skillId > 0 || e.type == IncomingEffectType::BasicAttack;
}

float ReplayWindow::FloaterBoxWidth(const IncomingEffect& e)
{
    const bool icon = FloaterHasIcon(e);
    if (FloaterUsesDigits(e))
        return OverheadFloaters::NumberWidth(static_cast<int>(e.label.size()), icon);
    if (e.label.empty())
        return icon ? OverheadFloaters::kIconSize : 0.f;
    const float text = ImGui::GetFont()->CalcTextSizeA(kFloaterTextSize, FLT_MAX, 0.f, e.label.c_str()).x + 8.f;
    return text + (icon ? OverheadFloaters::kIconGap + OverheadFloaters::kIconSize : 0.f);
}

// A gain fades in, a loss pops: the sign of the amount decides, as in the client.
OverheadFloaters::Motion ReplayWindow::FloaterMotion(const IncomingEffect& e)
{
    using T = IncomingEffectType;
    if (e.type == T::Heal) return OverheadFloaters::Motion::Gain;
    if (e.type == T::Energy && !e.label.empty() && e.label[0] == '+') return OverheadFloaters::Motion::Gain;
    return OverheadFloaters::Motion::Loss;
}

void ReplayWindow::PlaceFloaters(std::vector<IncomingEffect>& live, std::vector<IncomingEffect>& batch,
                                 bool& toggle, float baseScale) const
{
    std::stable_sort(batch.begin(), batch.end(),
                     [](const IncomingEffect& a, const IncomingEffect& b) { return a.spawnTime < b.spawnTime; });
    std::vector<OverheadFloaters::Live> boxes;
    boxes.reserve(live.size() + batch.size());
    for (const IncomingEffect& e : live)
        boxes.push_back({ e.spawnTime, e.xOffset, e.boxWidth, e.motion });
    std::stable_sort(boxes.begin(), boxes.end(),
                     [](const OverheadFloaters::Live& a, const OverheadFloaters::Live& b) { return a.spawn < b.spawn; });

    for (IncomingEffect& e : batch)
    {
        e.boxWidth  = FloaterBoxWidth(e);
        e.motion    = FloaterMotion(e);
        e.baseScale = baseScale;
        e.xOffset   = OverheadFloaters::Place(boxes, e.spawnTime, e.boxWidth, baseScale, toggle);
        boxes.push_back({ e.spawnTime, e.xOffset, e.boxWidth, e.motion });
        live.push_back(std::move(e));
    }
    batch.clear();
}

// The client's base scale for a floater over this agent: its floater-size slider at the middle, and
// the camera's distance in game units.
float ReplayWindow::FloaterBaseScale(int agentId, float time) const
{
    constexpr float kFloaterScalePref = 50.f;
    const Camera* cam = m_mapRenderer ? m_mapRenderer->GetCamera() : nullptr;
    auto it = m_replayCtx.agents.find(agentId);
    if (!cam || it == m_replayCtx.agents.end() || it->second.snapshots.empty())
        return OverheadFloaters::BaseScale(kFloaterScalePref, 1000.f);
    float sx, sy, sz;
    InterpolateAgentPosition(it->second, time, m_replayCtx.interpSettings, sx, sy, sz);
    const XMFLOAT3 p = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);
    const XMFLOAT3 c = cam->GetPosition3f();
    const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
    const float unitScale = std::abs(m_replayCtx.mapTransform.scaleX);
    const float unit = unitScale > 1e-6f ? unitScale : 1.f;
    return OverheadFloaters::BaseScale(kFloaterScalePref, std::sqrt(dx * dx + dy * dy + dz * dz) / unit);
}

void ReplayWindow::DrawFloater(ImDrawList* dl, const IncomingEffect& e, float now, float anchorX, float anchorY,
                               float view)
{
    const float age = now - e.spawnTime;
    if (age < 0.f || age >= OverheadFloaters::kLifetime) return;
    const OverheadFloaters::Pose pose = OverheadFloaters::Evaluate(e.motion, age);
    const uint8_t alpha = static_cast<uint8_t>(pose.alpha * 255.f);
    if (alpha == 0) return;

    // Screen y grows downward; the client's floater y grows upward.
    const float cx = anchorX + e.xOffset * e.baseScale * view;
    const float cy = anchorY - (OverheadFloaters::kBaseY + pose.rise * e.baseScale) * view;
    const float s  = e.baseScale * view * pose.scale;   // the whole model scales, icon and gaps too

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    auto drawIcon = [&](float left) {
        ImTextureID tex = (e.type == IncomingEffectType::BasicAttack)
                              ? LoadFlagIcon(dev, "kill.png")
                              : LoadSkillIcon(this, dev, e.skillId, m_skillIconIndex, m_skillIconCache);
        if (!tex) return;
        const float sz = OverheadFloaters::kIconSize * s;
        dl->AddImage(tex, ImVec2(left, cy - sz * 0.5f), ImVec2(left + sz, cy + sz * 0.5f),
                     ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, alpha));
    };
    const bool icon = FloaterHasIcon(e);

    if (FloaterUsesDigits(e))
    {
        const BitmapFont* font = (e.type == IncomingEffectType::Heal)   ? &m_healBitmapFont
                               : (e.type == IncomingEffectType::Energy) ? &m_energyBitmapFont
                                                                        : &m_damageBitmapFont;
        if (font->srv.Get())
        {
            // The digits centre on the floater; the icon sits 10 past the digits' box, so a number
            // with an icon hangs to the right, as in the client.
            font->DrawCells(dl, e.label.c_str(), cx, cy, OverheadFloaters::kCell * s, alpha);
            if (icon)
                drawIcon(cx + (OverheadFloaters::kAdvance * 0.5f * static_cast<float>(e.label.size())
                               + OverheadFloaters::kIconGap) * s);
            return;
        }
    }

    if (e.label.empty())
    {
        if (icon) drawIcon(cx - OverheadFloaters::kIconSize * 0.5f * s);
        return;
    }

    ImU32 col;
    switch (e.type) {
    case IncomingEffectType::Interrupt:
    case IncomingEffectType::Condition: col = IM_COL32(0xE0, 0x70, 0x30, alpha); break;
    case IncomingEffectType::Hex:       col = IM_COL32(0x90, 0x40, 0xC0, alpha); break;
    case IncomingEffectType::Energy:    col = IM_COL32(0xE0, 0x60, 0xF0, alpha); break;
    default:                            col = IM_COL32(0xFF, 0xFF, 0xFF, alpha); break;
    }
    ImFont* font = ImGui::GetFont();
    const float fs = kFloaterTextSize * s;
    const ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, e.label.c_str());
    const float pad = 4.f * s;
    const float left = cx - (tsz.x * 0.5f + pad);
    dl->AddRectFilled(ImVec2(left, cy - tsz.y * 0.5f - pad), ImVec2(left + tsz.x + 2.f * pad, cy + tsz.y * 0.5f + pad),
                      IM_COL32(0, 0, 0, static_cast<uint8_t>(0.55f * alpha)), 3.f * s);
    dl->AddText(font, fs, ImVec2(left + pad, cy - tsz.y * 0.5f), col, e.label.c_str());
    if (icon)
        drawIcon(left + tsz.x + 2.f * pad + OverheadFloaters::kIconGap * s);
}


void ReplayWindow::RenderIncomingEffects()
{
    if (m_incomingEffects.empty()) return;
    int focused = m_focusedAgentId;
    if (focused < 0) return;

    auto it = m_replayCtx.agents.find(focused);
    if (it == m_replayCtx.agents.end()) return;
    const auto& ard = it->second;
    if (ard.snapshots.empty()) return;

    float now = m_debugTimeline;
    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;

    XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    auto* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x, vpH = vp->Size.y;

    float sx, sy, sz;
    InterpolateAgentPosition(ard, now, m_replayCtx.interpSettings, sx, sy, sz);
    XMFLOAT3 worldPos = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);

    // Anchored just above the profession icon, which sits on the model's top, so the numbers keep
    // clear of it at any camera zoom or angle.
    float modelTopY = AgentModelTopY(focused, ard, worldPos.y, now);
    if (modelTopY <= worldPos.y)
        modelTopY = worldPos.y + 120.f;

    XMFLOAT3 topPos = { worldPos.x, modelTopY, worldPos.z };
    float anchorX, anchorY;
    if (!ProjectToScreen(viewProj, vpW, vpH, topPos, anchorX, anchorY)) return;
    float iconSz = std::clamp(vpH * 0.020f, 12.f, 20.f);
    constexpr float kScreenPad = 4.f;
    anchorY -= iconSz + kScreenPad * 2.f;

    EnsureSkillIconIndex();
    EnsureBitmapFontsLoaded();

    // Spawn order, so the newest draws on top.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const IncomingEffect& e : m_incomingEffects)
        DrawFloater(dl, e, now, anchorX, anchorY, 1.f);
}


// ---------------------------------------------------------------------------
// Shout speech bubbles — update + render
// ---------------------------------------------------------------------------

void ReplayWindow::UpdateSpeechBubbles()
{
    float now = m_debugTimeline;

    if (now < m_lastShoutScanTime - 0.5f)
    {
        m_speechBubbles.clear();
        m_shoutScanCursor.clear();
        m_bossClaimScanCursor = 0;
        m_lastShoutScanTime = now;
        return;
    }

    float scanFrom = m_lastShoutScanTime;
    m_lastShoutScanTime = now;
    if (now <= scanFrom) return;

    for (auto it = m_speechBubbles.begin(); it != m_speechBubbles.end(); )
    {
        if ((now - it->second.spawnTime) >= kSpeechBubbleLifetime)
            it = m_speechBubbles.erase(it);
        else
            ++it;
    }

    // The Boss claims are the server's own shout, not a skill, so they have their own list
    for (; m_bossClaimScanCursor < m_bossClaims.size(); ++m_bossClaimScanCursor)
    {
        const BossClaim& claim = m_bossClaims[m_bossClaimScanCursor];
        if (claim.time > now) break;
        if (claim.time <= scanFrom) continue;

        SpeechBubble sb;
        sb.agentId   = claim.agentId;
        sb.text      = "I am the boss!";
        sb.spawnTime = claim.time;
        m_speechBubbles[claim.agentId] = std::move(sb);
    }

    const auto& db = m_skillView;
    if (!db.IsLoaded()) return;

    constexpr int kShoutType = 20;

    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Player && ard.type != AgentType::NPC) continue;
        if (ard.skillUseHistory.empty()) continue;

        size_t& cursor = m_shoutScanCursor[agentId];
        if (cursor >= ard.skillUseHistory.size()) continue;

        for (; cursor < ard.skillUseHistory.size(); ++cursor)
        {
            const auto& ev = ard.skillUseHistory[cursor];
            if (ev.startTime > now) break;
            if (ev.startTime <= scanFrom) continue;

            const SkillInfo* si = db.Get(ev.skillId);
            if (!si || si->type != kShoutType) continue;

            SpeechBubble sb;
            sb.agentId   = agentId;
            sb.skillId   = ev.skillId;
            sb.text      = StripPvpSuffix(si->name);
            sb.spawnTime = ev.startTime;
            m_speechBubbles[agentId] = std::move(sb);
        }
    }
}


void ReplayWindow::RenderSpeechBubbles()
{
    if (m_speechBubbles.empty()) return;
    if (!m_showAgentOverlay) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;

    XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    auto* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x, vpH = vp->Size.y;
    float now = m_debugTimeline;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImTextureID bubbleTex = LoadSpeechBubbleTexture(dev);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();

    const InterpolationSettings& is = m_replayCtx.interpSettings;
    const MapTransform& mt = m_replayCtx.mapTransform;

    for (auto& [agentId, sb] : m_speechBubbles)
    {
        float age = now - sb.spawnTime;
        if (age < 0.f || age >= kSpeechBubbleLifetime) continue;

        auto ait = m_replayCtx.agents.find(agentId);
        if (ait == m_replayCtx.agents.end()) continue;
        const auto& ard = ait->second;
        if (ard.snapshots.empty()) continue;

        float sx, sy, sz;
        InterpolateAgentPosition(ard, now, is, sx, sy, sz);
        XMFLOAT3 worldPos = ApplyMapTransformToPos(sx, sy, sz, mt);

        float modelTopY = AgentModelTopY(agentId, ard, worldPos.y, now);
        if (modelTopY <= worldPos.y)
            modelTopY = worldPos.y + 120.f;

        XMFLOAT3 topPos = { worldPos.x, modelTopY, worldPos.z };
        float anchorX, anchorY;
        if (!ProjectToScreen(viewProj, vpW, vpH, topPos, anchorX, anchorY)) continue;

        float iconSz = std::clamp(vpH * 0.020f, 12.f, 20.f);
        constexpr float kScreenPad = 4.f;
        anchorY -= iconSz + kScreenPad;

        float t = age / kSpeechBubbleLifetime;
        float opacity = (t < 0.65f) ? 1.f : 1.f - ((t - 0.65f) / 0.35f);
        opacity = std::clamp(opacity, 0.f, 1.f);
        uint8_t alpha = static_cast<uint8_t>(opacity * 255.f);

        float fontSize = std::clamp(vpH * 0.011f, 9.f, 13.f);
        ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, sb.text.c_str());

        float padH = fontSize * 0.6f;
        float padV = fontSize * 0.35f;
        float bubbleW = textSize.x + padH * 2.f;
        float bubbleH = textSize.y + padV * 2.f;
        float tailH   = fontSize * 0.45f;

        float bx = anchorX - bubbleW * 0.5f;
        float by = anchorY - bubbleH - tailH;

        if (bubbleTex)
        {
            ImVec2 bubbleTL(bx, by);
            ImVec2 bubbleBR(bx + bubbleW, anchorY);
            dl->AddImage(bubbleTex, bubbleTL, bubbleBR,
                         ImVec2(0, 0), ImVec2(1, 1),
                         IM_COL32(255, 255, 255, alpha));
        }
        else
        {
            ImVec2 rectTL(bx, by);
            ImVec2 rectBR(bx + bubbleW, by + bubbleH);
            dl->AddRectFilled(rectTL, rectBR, IM_COL32(20, 20, 20, (uint8_t)(0.85f * alpha)), 3.f);
            dl->AddRect(rectTL, rectBR, IM_COL32(200, 200, 200, alpha), 3.f, 0, 1.f);

            float triCx = anchorX;
            float triTop = by + bubbleH;
            dl->AddTriangleFilled(
                ImVec2(triCx - tailH * 0.4f, triTop),
                ImVec2(triCx + tailH * 0.4f, triTop),
                ImVec2(triCx, triTop + tailH),
                IM_COL32(20, 20, 20, (uint8_t)(0.85f * alpha)));
        }

        float textX = bx + (bubbleW - textSize.x) * 0.5f;
        float textY = by + (bubbleH - textSize.y) * 0.5f;
        dl->AddText(font, fontSize, ImVec2(textX, textY),
                    IM_COL32(255, 255, 255, alpha), sb.text.c_str());
    }
}


// ---------------------------------------------------------------------------
// The Boss (Like a Boss flux) - who held it, and when
// ---------------------------------------------------------------------------

void ReplayWindow::BuildBossTimeline()
{
    m_bossClaims.clear();
    m_bossTenures.clear();
    m_bossClaimScanCursor = 0;

    // "I am the boss!" as the encoded string the server sends with the shout
    constexpr std::wstring_view kClaimWords = L"\x8103\x08D9";
    for (const auto& sb : m_replayCtx.stocData.speechBubbles)
    {
        auto it = m_replayCtx.agents.find(sb.agent_id);
        if (it == m_replayCtx.agents.end() || it->second.type != AgentType::Player) continue;
        if (std::wstring_view(sb.words).starts_with(kClaimWords))
            m_bossClaims.push_back({ sb.time, sb.agent_id });
    }
    if (m_bossClaims.empty()) return;
    std::stable_sort(m_bossClaims.begin(), m_bossClaims.end(),
                     [](const BossClaim& a, const BossClaim& b) { return a.time < b.time; });

    // A holder loses it by dying, or by leaving the match for good (a removal with no later add)
    struct Ending { float time; int agentId; };
    std::vector<Ending> endings;
    for (const auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Player) continue;
        bool wasDead = false;
        for (const auto& snap : ard.snapshots)
        {
            if (snap.is_dead && !wasDead) endings.push_back({ snap.time, agentId });
            wasDead = snap.is_dead;
        }
    }
    std::unordered_map<int, float> lastAdd;
    for (const auto& ev : m_replayCtx.stocData.lifecycle)
        if (ev.isAdd) lastAdd[ev.agent_id] = std::max(lastAdd[ev.agent_id], ev.time);
    for (const auto& ev : m_replayCtx.stocData.lifecycle)
    {
        if (ev.isAdd) continue;
        auto la = lastAdd.find(ev.agent_id);
        if (la == lastAdd.end() || la->second < ev.time)
            endings.push_back({ ev.time, ev.agent_id });
    }
    std::sort(endings.begin(), endings.end(),
              [](const Ending& a, const Ending& b) { return a.time < b.time; });

    // Endings are applied in time order up to each shout, so a death before a shout never closes
    // the tenure that shout opens, and the victim's death just after it closes the victim's.
    std::unordered_map<int, size_t> open;   // holder -> index in m_bossTenures
    size_t next = 0;
    auto closeUntil = [&](float t) {
        for (; next < endings.size() && endings[next].time < t; ++next)
        {
            auto it = open.find(endings[next].agentId);
            if (it == open.end()) continue;
            m_bossTenures[it->second].end = endings[next].time;
            open.erase(it);
        }
    };
    for (const auto& claim : m_bossClaims)
    {
        closeUntil(claim.time);
        if (open.count(claim.agentId)) continue;   // already holding it
        open[claim.agentId] = m_bossTenures.size();
        m_bossTenures.push_back({ claim.agentId, claim.time, FLT_MAX });
    }
    closeUntil(FLT_MAX);
}


const ReplayWindow::BossTenure* ReplayWindow::FindBossTenure(int agentId, float t) const
{
    for (const auto& tenure : m_bossTenures)
        if (tenure.agentId == agentId && tenure.start <= t && t < tenure.end)
            return &tenure;
    return nullptr;
}


// ---------------------------------------------------------------------------
// Top-of-screen HUD for the followed agent: health bar (with name inside)
// and current skill cast bar.
// ---------------------------------------------------------------------------

void ReplayWindow::DrawFollowedAgentHUD()
{
    int focused = GetFocusedAgentId();
    if (focused < 0) return;

    auto it = m_replayCtx.agents.find(focused);
    if (it == m_replayCtx.agents.end()) return;
    const auto& ard = it->second;
    if (ard.snapshots.empty()) return;

    const AgentSnapshot* snap = FindSnapshotAtTime(ard, m_debugTimeline);
    if (!snap) return;

    auto* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();

    // --- Panel background (only around health bar) ---
    constexpr float BAR_W   = 320.f;
    constexpr float BAR_H   = 26.f;
    constexpr float PAD     = 6.f;
    constexpr float TOP_Y   = 36.f;   // below the menu bar
    constexpr float PANEL_R = 5.f;
    constexpr float RIBBON_GAP = 6.f; // breathing room under the ribbon strip

    float panelW = BAR_W + PAD * 2.f;
    float panelH = PAD + BAR_H + PAD;
    float panelX = (vpW - panelW) * 0.5f;

    // Slide clear of the ribbon toolbar instead of being drawn over it. The
    // ribbon republishes its animated bottom edge each frame, so the bar rides
    // the collapse/expand tween; once the strip is collapsed (or closed) the
    // edge falls above TOP_Y and the bar settles back to its usual place.
    float baseY = std::max(TOP_Y, m_ribbonBottomY + RIBBON_GAP);

    // The drawing strip is a movable window and by default sits exactly where the
    // bar rests under an expanded ribbon, so drop below it when it covers the bar.
    // Tested against baseY, not the animated position, so clearing the strip
    // cannot make the test flip back and leave the bar oscillating.
    float dropTarget = 0.f;
    const auto strip = m_annotationMgr.ToolbarRect();
    if (strip.valid() &&
        strip.x1 > panelX && strip.x0 < panelX + panelW &&
        strip.y1 > baseY  && strip.y0 < baseY + panelH)
    {
        dropTarget = strip.y1 + RIBBON_GAP - baseY;
    }

    // Same exponential tween the ribbon uses for its own reveal, so the bar moves
    // at one speed whichever strip it is dodging.
    constexpr float DROP_RATE = 12.f;
    const int  hudFrame  = ImGui::GetFrameCount();
    const bool justShown = (m_followedHudLastFrame != hudFrame - 1);
    m_followedHudLastFrame = hudFrame;

    if (justShown)
    {
        m_followedHudDropY = dropTarget;
    }
    else
    {
        const float dt = ImGui::GetIO().DeltaTime;
        m_followedHudDropY += (dropTarget - m_followedHudDropY) *
                              std::min(1.f, dt * DROP_RATE);
        if (std::abs(dropTarget - m_followedHudDropY) < 0.5f)
            m_followedHudDropY = dropTarget;
    }

    float panelY = baseY + m_followedHudDropY;

    bool isDead = snap->is_dead;
    float healthPct = std::clamp(snap->health_pct, 0.f, 1.f);
    uint8_t teamId = (uint8_t)ard.teamId;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    PartyIcons hudIcons = LoadAllPartyIcons(dev);

    ImTextureID carriedFlagTex = CarriedBundleIcon(dev, focused);

    ImVec2 panelTL(panelX, panelY);
    ImVec2 panelBR(panelX + panelW, panelY + panelH);

    ImU32 panelBg = Team::IsRed(teamId)
        ? IM_COL32(0x28, 0x0A, 0x0A, 0xA0)   // dark red, more transparent
        : IM_COL32(0x0A, 0x12, 0x28, 0xA0);   // dark blue, more transparent
    dl->AddRectFilled(panelTL, panelBR, panelBg, PANEL_R);
    dl->AddRect(panelTL, panelBR, IM_COL32(0xA0, 0xA0, 0xA0, 0x90), PANEL_R, 0, 1.0f);

    // Health bar inside the panel
    float barX = panelX + PAD;
    float barY = panelY + PAD;
    ImVec2 barTL(barX, barY);
    ImVec2 barBR(barX + BAR_W, barY + BAR_H);

    auto sv = ard.skillVisualAtTime(m_debugTimeline);
    bool showCast = (sv.skillId > 0 && sv.alpha > 0.f);
    constexpr float CAST_ICON  = 34.f;
    constexpr float CAST_GAP   = 5.f;
    constexpr float CAST_BAR_H = 18.f;

    const Gradient5* deadGrad = Team::IsRed(teamId) ? &kDeadRed : &kDeadBlue;
    const Gradient5* fillGrad = nullptr;
    if (isDead)
        fillGrad = deadGrad;
    else if (snap->has_degen_hex)
        fillGrad = &kDegenHex;
    else if (snap->has_poison)
        fillGrad = &kPoison;
    else if (snap->has_bleeding)
        fillGrad = &kBleeding;
    else
        fillGrad = Team::IsRed(teamId) ? &kAliveRed : &kAliveBlue;

    ImVec2 innerTL(barTL.x + 1, barTL.y + 1);
    ImVec2 innerBR(barBR.x - 1, barBR.y - 1);
    float innerW = innerBR.x - innerTL.x;

    dl->AddRectFilled(barTL, barBR, IM_COL32(0, 0, 0, 0xFF), 2.f);

    if (isDead)
    {
        DrawGradientRect(dl, innerTL, innerBR, *fillGrad);
    }
    else
    {
        DrawGradientRect(dl, innerTL, innerBR, *deadGrad);
        if (healthPct > 0.f)
        {
            bool dw = snap->has_deep_wound && !isDead;
            float fp = dw ? std::min(healthPct, 0.80f) : healthPct;
            DrawGradientRect(dl, innerTL, ImVec2(innerTL.x + innerW * fp, innerBR.y), *fillGrad);
            if (dw)
            {
                float dwX = innerTL.x + innerW * 0.80f;
                DrawGradientRect(dl, ImVec2(dwX, innerTL.y), innerBR, kDeepWound);
            }
        }
    }

    dl->AddRect(barTL, barBR, IM_COL32(0x50, 0x50, 0x50, 0xFF), 2.f);

    // Agent name inside the health bar
    const std::string& nameLabel = ard.partyBarLabel.empty() ? GetAgentLabel(ard) : ard.partyBarLabel;

    ImVec2 nameSz = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, nameLabel.c_str());
    float nameX = barTL.x + 5.f;
    float nameY = barTL.y + (BAR_H - nameSz.y) * 0.5f;
    dl->AddText(ImVec2(nameX + 1.f, nameY + 1.f), IM_COL32(0, 0, 0, 0xCC), nameLabel.c_str());
    ImU32 nameCol = isDead ? IM_COL32(0x80, 0x80, 0x80, 0xFF) : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
    dl->AddText(ImVec2(nameX, nameY), nameCol, nameLabel.c_str());

    // Status effect icons (right-aligned, hidden when dead) — matches party window
    if (!isDead)
    {
        float innerH = innerBR.y - innerTL.y;
        const float iconSz = std::min(innerH - 2.f, 18.f);
        float iconX = innerBR.x - 2.f;
        float iconY = innerTL.y + (innerH - iconSz) * 0.5f;

        if (snap->has_weapon_spell && hudIcons.weaponSpell)
        {
            iconX -= iconSz;
            dl->AddImage(hudIcons.weaponSpell, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if (snap->has_enchantment && hudIcons.enchanted)
        {
            iconX -= iconSz;
            dl->AddImage(hudIcons.enchanted, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if ((snap->has_condition || snap->has_deep_wound || snap->has_bleeding || snap->has_poison) && hudIcons.condition)
        {
            iconX -= iconSz;
            dl->AddImage(hudIcons.condition, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if (snap->has_hex && hudIcons.hexed)
        {
            iconX -= iconSz;
            dl->AddImage(hudIcons.hexed, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if (carriedFlagTex)
        {
            iconX -= iconSz;
            dl->AddImage(carriedFlagTex, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
        }
    }

    // --- Current skill cast bar (icon left-aligned under health bar) ---
    if (showCast)
    {
        const auto& db = m_skillView;
        const SkillInfo* si = db.Get(sv.skillId);
        uint8_t castAlpha = (uint8_t)(sv.alpha * 255.f);

        float castY = panelBR.y + CAST_GAP;
        float castBarW = BAR_W - CAST_ICON - CAST_GAP;

        EnsureSkillIconIndex();
        ImTextureID skillTex = LoadSkillIcon(this, dev, sv.skillId,
                                             m_skillIconIndex, m_skillIconCache);
        if (skillTex)
        {
            ImVec2 icoTL(barX, castY);
            ImVec2 icoBR(barX + CAST_ICON, castY + CAST_ICON);
            dl->AddImage(skillTex, icoTL, icoBR,
                ImVec2(0,0), ImVec2(1,1), IM_COL32(255, 255, 255, castAlpha));
        }

        float cbX = barX + CAST_ICON + CAST_GAP;
        float cbY = castY + (CAST_ICON - CAST_BAR_H) * 0.5f;
        ImVec2 cbTL(cbX, cbY);
        ImVec2 cbBR(cbX + castBarW, cbY + CAST_BAR_H);
        DrawGameCastBar(dl, cbTL, cbBR, sv);

        if (si && !si->name.empty())
        {
            float fs = 11.f;
            ImVec2 snSz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, si->name.c_str());
            float snX = cbTL.x + (castBarW - snSz.x) * 0.5f;
            float snY = cbTL.y + (CAST_BAR_H - snSz.y) * 0.5f;
            dl->AddText(font, fs, ImVec2(snX + 1.f, snY + 1.f),
                IM_COL32(0, 0, 0, castAlpha), si->name.c_str());
            dl->AddText(font, fs, ImVec2(snX, snY),
                IM_COL32(0xFF, 0xFF, 0xFF, castAlpha), si->name.c_str());
        }
    }
}

// Energy gains and drains over every player but the followed one, in the client's purple digits and
// with the client's floater layout. Only energy, which the energy model knows for everyone; damage
// and heals stay on the followed player.
void ReplayWindow::RenderEnergyPopsOverPlayers()
{
    if (!m_replayCtx.agentsLoaded || !m_agentsClassified) return;
    Camera* cam = m_mapRenderer ? m_mapRenderer->GetCamera() : nullptr;
    if (!cam) return;
    EnsureBitmapFontsLoaded();
    if (!m_energyBitmapFont.srv.Get()) return;

    const float now = m_debugTimeline;
    const XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float vpW = vp->Size.x, vpH = vp->Size.y;
    const float iconSz = std::clamp(vpH * 0.020f, 12.f, 20.f);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    std::vector<IncomingEffect> live, batch;

    for (int pid : m_playerIds)
    {
        if (pid == m_focusedAgentId) continue;
        const EnergyModel::Track* track = EnergyTrackFor(pid);
        if (!track || track->gains.empty()) continue;
        auto first = std::upper_bound(track->gains.begin(), track->gains.end(), now - kEffectLifetime,
                                      [](float v, const EnergyModel::Gain& g) { return v < g.time; });
        if (first == track->gains.end() || first->time > now) continue;

        auto it = m_replayCtx.agents.find(pid);
        if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) continue;
        const AgentReplayData& ard = it->second;
        if (m_fogPerspective > 0 && ard.teamId != m_fogPerspective && IsAgentInFog(pid)) continue;

        float sx, sy, sz;
        InterpolateAgentPosition(ard, now, m_replayCtx.interpSettings, sx, sy, sz);
        const XMFLOAT3 worldPos = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);
        float topY = AgentModelTopY(pid, ard, worldPos.y, now);
        if (topY <= worldPos.y) topY = worldPos.y + 120.f;
        float ax, ay;
        if (!ProjectToScreen(viewProj, vpW, vpH, XMFLOAT3{ worldPos.x, topY, worldPos.z }, ax, ay)) continue;
        ay -= iconSz + 8.f;

        // Nothing is kept between frames here, so the layout is replayed from the start of the
        // current run of overlapping pops: back to a pop that spawned when every earlier one had
        // expired. The side switch starts afresh there; the same frame always gives the same layout.
        auto start = first;
        while (start != track->gains.begin() && std::prev(start)->time > start->time - kEffectLifetime)
            --start;

        live.clear();
        batch.clear();
        bool toggle = false;
        const float scale = FloaterBaseScale(pid, now);
        for (auto g = start; g != track->gains.end() && g->time <= now; ++g)
        {
            const int amount = static_cast<int>(std::lround(g->amount));
            if (amount == 0) continue;
            IncomingEffect eff;
            eff.spawnTime = g->time;
            eff.type = IncomingEffectType::Energy;
            eff.label = std::format("{}{}", amount > 0 ? "+" : "-", std::abs(amount));
            batch.push_back(std::move(eff));
            PlaceFloaters(live, batch, toggle, scale);
        }
        for (const IncomingEffect& e : live)
            DrawFloater(dl, e, now, ax, ay, 1.f);
    }
}
