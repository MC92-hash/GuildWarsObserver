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
// Player Info Panel
// ---------------------------------------------------------------------------

void ReplayWindow::OpenPlayerInfoPanel(int agentId)
{
    if (agentId < 0) return;
    auto it = m_replayCtx.agents.find(agentId);
    if (it == m_replayCtx.agents.end()) return;
    if (it->second.type != AgentType::Player) return;

    m_playerInfoAgentId = agentId;
    m_showPlayerInfoPanel = true;

    if (m_pipWeaponSets.agentId != agentId)
        BuildWeaponSets(agentId);
}


void ReplayWindow::ClosePlayerInfoPanel()
{
    m_showPlayerInfoPanel = false;
    m_playerInfoAgentId = -1;
}


void ReplayWindow::BuildWeaponSets(int agentId)
{
    m_pipWeaponSets.agentId = agentId;
    m_pipWeaponSets.sets.clear();
    m_pipWeaponSets.built = true;

    auto it = m_replayCtx.agents.find(agentId);
    if (it == m_replayCtx.agents.end()) return;

    const auto& ard = it->second;
    const auto& snaps = ard.snapshots;
    int primaryProf = ard.primaryProf;

    if (primaryProf < 1 || primaryProf > 10) {
        OutputDebugStringA("Unknown profession for weapon slot ordering, defaulting to caster classification\n");
    }

    auto isMartial = [](int prof) {
        return prof == 1 || prof == 2 || prof == 7 || prof == 9 || prof == 10;
    };

    auto getSlotCategory = [&](const WeaponSetEntry& ws) -> int {
        if ((ws.weapCat == 2 || ws.weapCat == 6 || ws.weapCat == 7) && ws.mainType == 24)
            return 1; // Defensive: Sword/Axe/Spear + Shield
        if (isMartial(primaryProf)) {
            if (ws.weapCat == 1 || ws.weapCat == 3 || ws.weapCat == 4 || ws.weapCat == 5)
                return 2; // Martial second set
        } else {
            if (ws.weapCat >= 8 && ws.weapCat <= 14 && ws.mainType == 12)
                return 2; // Caster: Wand + Focus
        }
        if (ws.weapCat >= 8 && ws.weapCat <= 14 && ws.mainType == 46)
            return 3; // Staff
        return 4;     // Others (flags, unusual combos)
    };

    struct KeyInfo {
        uint16_t   mainId, offId;
        uint16_t   weapCat;
        uint8_t    mainType, offType;
        float      firstSeen;
        int        maxConsec;
        BundleType bundleType;
    };
    std::vector<KeyInfo> candidates;

    uint16_t prevMain = 0xFFFF, prevOff = 0xFFFF;
    int consecCount = 0;
    bool flagSeen = false;

    for (size_t si = 0; si < snaps.size(); si++)
    {
        const auto& s = snaps[si];
        if (s.weapon_item_id == 0 && s.offhand_item_id == 0) {
            prevMain = 0xFFFF; prevOff = 0xFFFF; consecCount = 0;
            continue;
        }

        if (s.weapon_item_id == prevMain && s.offhand_item_id == prevOff) {
            consecCount++;
        } else {
            consecCount = 1;
            prevMain = s.weapon_item_id;
            prevOff  = s.offhand_item_id;
        }

        BundleType thisBundleType = BundleType::Unknown;
        bool isBundle = (s.weapon_type == 0 && s.weapon_item_type == 46);
        if (isBundle)
            thisBundleType = GetPlayerBundleType(agentId, s.time);

        // Deduplicate flags: only one flag entry allowed
        if (isBundle && thisBundleType == BundleType::Flag && flagSeen) continue;

        bool found = false;
        for (auto& c : candidates) {
            if (c.mainId == s.weapon_item_id && c.offId == s.offhand_item_id) {
                if (consecCount > c.maxConsec) c.maxConsec = consecCount;
                found = true;
                break;
            }
        }
        if (!found) {
            if (isBundle && thisBundleType == BundleType::Flag) flagSeen = true;
            KeyInfo ki;
            ki.mainId      = s.weapon_item_id;
            ki.offId       = s.offhand_item_id;
            ki.weapCat     = s.weapon_type;
            ki.mainType    = s.weapon_item_type;
            ki.offType     = s.offhand_item_type;
            ki.firstSeen   = s.time;
            ki.maxConsec   = consecCount;
            ki.bundleType  = thisBundleType;
            candidates.push_back(ki);
        }
    }

    // Build set list — require >= 3 consecutive snapshots unless offhand present or bundle
    for (auto& c : candidates) {
        bool hasOffhand = (c.offId != 0);
        bool isBundle = (c.bundleType != BundleType::Unknown);
        if (!isBundle && !hasOffhand && c.maxConsec < 3) continue;

        WeaponSetEntry e;
        e.mainId      = c.mainId;
        e.offId       = c.offId;
        e.weapCat     = c.weapCat;
        e.mainType    = c.mainType;
        e.offType     = c.offType;
        e.firstSeen   = c.firstSeen;
        e.bundleType  = c.bundleType;
        m_pipWeaponSets.sets.push_back(e);
    }

    // Sort by slot category (1→2→3→4), then by firstSeen within same category
    std::stable_sort(m_pipWeaponSets.sets.begin(), m_pipWeaponSets.sets.end(),
        [&](const WeaponSetEntry& a, const WeaponSetEntry& b) {
            int sa = getSlotCategory(a);
            int sb = getSlotCategory(b);
            if (sa != sb) return sa < sb;
            return a.firstSeen < b.firstSeen;
        });

    // Compute disambiguation subscripts for sets sharing the same icon appearance
    for (size_t i = 0; i < m_pipWeaponSets.sets.size(); i++) {
        auto& a = m_pipWeaponSets.sets[i];
        if (a.disambig > 0) continue;
        std::vector<size_t> dupes;
        dupes.push_back(i);
        for (size_t j = i + 1; j < m_pipWeaponSets.sets.size(); j++) {
            auto& b = m_pipWeaponSets.sets[j];
            if (b.weapCat == a.weapCat && b.mainType == a.mainType)
                dupes.push_back(j);
        }
        if (dupes.size() > 1) {
            int sub = 1;
            for (size_t idx : dupes)
                m_pipWeaponSets.sets[idx].disambig = sub++;
        }
    }
}


std::vector<ReplayWindow::PipSkillStat> ReplayWindow::BuildSkillStats(int agentId, float currentTime) const
{
    std::vector<PipSkillStat> result;
    auto it = m_replayCtx.agents.find(agentId);
    if (it == m_replayCtx.agents.end()) return result;

    const auto& history = it->second.skillUseHistory;

    // Count casts per skill up to currentTime
    struct SkillAccum {
        int totalCasts = 0;
        std::unordered_map<int, int> targetCounts; // targetId -> count
    };
    std::unordered_map<int, SkillAccum> accum;
    int grandTotal = 0;

    for (const auto& ev : history)
    {
        if (ev.startTime > currentTime) break;
        auto& a = accum[ev.skillId];
        a.totalCasts++;
        a.targetCounts[ev.targetId >= 0 ? ev.targetId : agentId]++;
        grandTotal++;
    }

    if (grandTotal == 0) return result;

    for (auto& [skillId, a] : accum)
    {
        PipSkillStat stat;
        stat.skillId    = skillId;
        stat.totalCasts = a.totalCasts;
        stat.castPct    = (float)a.totalCasts / (float)grandTotal;

        for (auto& [tId, cnt] : a.targetCounts)
        {
            PipSkillStat::TargetBreakdown tb;
            tb.targetId = tId;
            tb.count    = cnt;
            tb.pct      = (float)cnt / (float)a.totalCasts;

            auto tit = m_replayCtx.agents.find(tId);
            if (tit != m_replayCtx.agents.end())
            {
                tb.name   = tit->second.partyBarLabel.empty() ? tit->second.playerName : tit->second.partyBarLabel;
                tb.teamId = tit->second.teamId;
            }
            else
            {
                tb.name = (tId == agentId) ? "Self" : ("Agent " + std::to_string(tId));
            }

            stat.targets.push_back(std::move(tb));
        }

        std::sort(stat.targets.begin(), stat.targets.end(),
                  [](const auto& a, const auto& b) { return a.pct > b.pct; });

        result.push_back(std::move(stat));
    }

    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.castPct > b.castPct; });

    return result;
}


void ReplayWindow::DrawPlayerInfoPanel()
{
    if (!m_showPlayerInfoPanel || m_playerInfoAgentId < 0) return;

    auto agentIt = m_replayCtx.agents.find(m_playerInfoAgentId);
    if (agentIt == m_replayCtx.agents.end()) { ClosePlayerInfoPanel(); return; }

    const AgentReplayData& ard = agentIt->second;
    const AgentSnapshot* snap = FindSnapshotAtTime(ard, m_debugTimeline);
    bool isDead = snap ? snap->is_dead : false;

    ImGuiIO& io = ImGui::GetIO();
    float vpW = io.DisplaySize.x;
    float vpH = io.DisplaySize.y;
    float maxH = vpH * 0.80f;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();

    static bool s_firstOpen = true;
    if (s_firstOpen)
    {
        ImGui::SetNextWindowPos(ImVec2(vpW - 460.f, vpH * 0.40f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(440.f, 0.f), ImGuiCond_FirstUseEver);
        s_firstOpen = false;
    }
    float maxW = std::min(vpW * 0.5f, 900.f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.f, 100.f), ImVec2(maxW, maxH));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.067f, 0.082f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0.05f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(1.f, 1.f, 1.f, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1.f, 1.f, 1.f, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(1.f, 1.f, 1.f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.f);

    bool open = true;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("##PlayerInfoPanel", &open, flags))
    {
        ImGui::End();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(6);
        return;
    }

    if (!open)
    {
        ClosePlayerInfoPanel();
        ImGui::End();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(6);
        return;
    }

    // Clamp panel to viewport bounds
    {
        ImVec2 wPos = ImGui::GetWindowPos();
        ImVec2 wSz  = ImGui::GetWindowSize();
        float clampX = std::clamp(wPos.x, 0.f, std::max(0.f, vpW - wSz.x));
        float clampY = std::clamp(wPos.y, 0.f, std::max(0.f, vpH - wSz.y));
        if (clampX != wPos.x || clampY != wPos.y)
            ImGui::SetWindowPos(ImVec2(clampX, clampY));
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float contentW = ImGui::GetWindowSize().x;
    constexpr float kPadX = 12.f;
    constexpr ImU32 kGold = IM_COL32(0xD4, 0xA0, 0x20, 0xFF);
    constexpr ImU32 kDivider = IM_COL32(255, 255, 255, 15);
    constexpr ImU32 kBlueTeam = IM_COL32(0x4a, 0x90, 0xd8, 0xFF);
    constexpr ImU32 kRedTeam  = IM_COL32(0xd0, 0x48, 0x48, 0xFF);

    // Helper: strip profession/level prefix ("Mo/W20 Name" -> "Name")
    auto StripProfPrefix = [](const std::string& raw) -> std::string {
        if (raw.empty()) return raw;
        // Pattern: "X/Y## Name" or "X## Name" — find first space after digits
        size_t i = 0;
        bool foundSlashOrDigit = false;
        while (i < raw.size() && (raw[i] == '/' || isalpha((unsigned char)raw[i]) || isdigit((unsigned char)raw[i])))
        {
            if (raw[i] == '/' || isdigit((unsigned char)raw[i])) foundSlashOrDigit = true;
            i++;
        }
        if (foundSlashOrDigit && i < raw.size() && raw[i] == ' ')
            return raw.substr(i + 1);
        return raw;
    };

    // ═══════════════════════════════════════════════════════════════
    // SECTION 1 — HEADER: PriIcon / SecIcon  PlayerName  [DP near name]
    // ═══════════════════════════════════════════════════════════════
    {
        ImVec2 headerTL = ImGui::GetCursorScreenPos();
        float headerH = 40.f;
        dl->AddRectFilled(headerTL, ImVec2(headerTL.x + contentW, headerTL.y + headerH),
                          IM_COL32(0, 0, 0, 64));

        float cx = headerTL.x + kPadX;
        float iconCenterY = headerTL.y + headerH * 0.5f;

        // Primary profession icon 22x22
        ImTextureID priTex = LoadProfIcon(dev, ard.primaryProf);
        if (priTex)
            dl->AddImage(priTex, ImVec2(cx, iconCenterY - 11.f), ImVec2(cx + 22, iconCenterY + 11.f));
        cx += 24.f;

        // "/" separator
        dl->AddText(nullptr, 14.f,
            ImVec2(cx, iconCenterY - 7.f), IM_COL32(255, 255, 255, 100), "/");
        cx += ImGui::CalcTextSize("/").x + 2.f;

        // Secondary profession icon 16x16
        if (ard.secondaryProf > 0)
        {
            ImTextureID secTex = LoadProfIcon(dev, ard.secondaryProf);
            if (secTex)
                dl->AddImage(secTex, ImVec2(cx, iconCenterY - 8.f), ImVec2(cx + 16, iconCenterY + 8.f));
            cx += 18.f;
        }

        cx += 6.f;

        // Player name in gold, 16px
        std::string displayName = ard.playerName.empty() ? ard.partyBarLabel : ard.playerName;
        displayName = StripProfPrefix(displayName);
        float nameFontSz = 16.f;
        float maxNameW = contentW - (cx - headerTL.x) - 44.f;
        ImVec2 nameSz = ImGui::CalcTextSize(displayName.c_str());
        float nameScaledW = nameSz.x * (nameFontSz / ImGui::GetFontSize());
        if (nameScaledW > maxNameW && displayName.size() > 4)
        {
            while (nameScaledW > maxNameW && displayName.size() > 4)
            {
                size_t sp = displayName.rfind(' ');
                if (sp != std::string::npos && sp > 0)
                    displayName = displayName.substr(0, sp) + "...";
                else
                {
                    displayName.pop_back();
                    if (displayName.size() > 3)
                        displayName = displayName.substr(0, displayName.size()) + "...";
                }
                nameSz = ImGui::CalcTextSize(displayName.c_str());
                nameScaledW = nameSz.x * (nameFontSz / ImGui::GetFontSize());
            }
        }

        float nameY = headerTL.y + (headerH - nameFontSz) * 0.5f;
        dl->AddText(nullptr, nameFontSz, ImVec2(cx + 1, nameY + 1), IM_COL32(0, 0, 0, 200), displayName.c_str());
        dl->AddText(nullptr, nameFontSz, ImVec2(cx, nameY), kGold, displayName.c_str());

        // Right side: team dot + close
        float rx = headerTL.x + contentW - kPadX;
        ImU32 dotCol = (ard.teamId == 1) ? kRedTeam : kBlueTeam;

        ImGui::SetCursorScreenPos(ImVec2(rx - 16.f, headerTL.y + (headerH - 16.f) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f, 1.f, 1.f, 0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.f, 1.f, 1.f, 0.2f));
        if (ImGui::SmallButton("X##pip_close"))
            ClosePlayerInfoPanel();
        ImGui::PopStyleColor(4);

        float dotCx = rx - 16.f - 12.f;
        dl->AddCircleFilled(ImVec2(dotCx, headerTL.y + headerH * 0.5f), 4.f, dotCol);

        ImGui::SetCursorScreenPos(ImVec2(headerTL.x, headerTL.y + headerH));
    }

    // ═══════════════════════════════════════════════════════════════
    // SECTION 2 — Morale row + HP bar (gradient, red theme for all, status icons)
    // ═══════════════════════════════════════════════════════════════
    if (snap)
    {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + kPadX,
                                         ImGui::GetCursorScreenPos().y + 8.f));
        ImVec2 sec = ImGui::GetCursorScreenPos();
        float barW = contentW - 2 * kPadX;

        int boostCount = 0;
        int moraleValue = ComputeAgentMorale(ard, m_debugTimeline, nullptr, &boostCount);

        // Morale row (only if non-zero), above HP bar, right-aligned
        if (moraleValue != 0)
        {
            constexpr float kMoraleRowH = 18.f;
            float rowRight = sec.x + barW;

            if (moraleValue > 0)
            {
                std::string stars;
                int starCount = std::min(4, boostCount);
                for (int s = 0; s < starCount; ++s) stars += "\xe2\x98\x85";

                char boostBuf[64];
                snprintf(boostBuf, sizeof(boostBuf), "MORALE BOOST  +%d%%  %s", moraleValue, stars.c_str());
                ImVec2 bSz = ImGui::CalcTextSize(boostBuf);
                float bScale = 12.f / ImGui::GetFontSize();
                float bx = rowRight - bSz.x * bScale;
                float by = sec.y + (kMoraleRowH - 12.f) * 0.5f;
                dl->AddText(nullptr, 12.f, ImVec2(bx, by), kGold, boostBuf);
            }
            else
            {
                ImU32 dpCol;
                if (moraleValue >= -15)      dpCol = IM_COL32(0xE0, 0x78, 0x30, 0xFF);
                else if (moraleValue >= -30) dpCol = IM_COL32(0xC0, 0x50, 0x20, 0xFF);
                else                         dpCol = IM_COL32(0xCC, 0x30, 0x30, 0xFF);

                int dotCount = (-moraleValue) / 15;
                char dpBuf[48];
                std::string dots;
                for (int d = 0; d < dotCount; d++) dots += "\xE2\x97\x8F"; // ●
                if (moraleValue <= -60)
                    snprintf(dpBuf, sizeof(dpBuf), "%d%%  %s MAX", moraleValue, dots.c_str());
                else
                    snprintf(dpBuf, sizeof(dpBuf), "%d%%  %s", moraleValue, dots.c_str());
                ImVec2 dSz = ImGui::CalcTextSize(dpBuf);
                float dScale = 12.f / ImGui::GetFontSize();
                float dx = rowRight - dSz.x * dScale;
                float dy = sec.y + (kMoraleRowH - 12.f) * 0.5f;
                dl->AddText(nullptr, 12.f, ImVec2(dx + 1, dy + 1), IM_COL32(0, 0, 0, 0xCC), dpBuf);
                dl->AddText(nullptr, 12.f, ImVec2(dx, dy), dpCol, dpBuf);
            }

            sec.y += kMoraleRowH;
            ImGui::SetCursorScreenPos(sec);
        }
        float barH = 16.f;

        float hpPct = std::clamp(snap->health_pct, 0.f, 1.f);
        bool hasDeepWound = snap->has_deep_wound && !isDead;

        // Always use red theme for readability in this panel
        const Gradient5* fillGrad = nullptr;
        if (isDead)
            fillGrad = &kDeadRed;
        else if (snap->has_degen_hex)
            fillGrad = &kDegenHex;
        else if (snap->has_poison)
            fillGrad = &kPoison;
        else if (snap->has_bleeding)
            fillGrad = &kBleeding;
        else
            fillGrad = &kAliveRed;

        ImVec2 bTL(sec.x, sec.y);
        ImVec2 bBR(sec.x + barW, sec.y + barH);

        dl->AddRect(bTL, bBR, IM_COL32(0x4E, 0x4D, 0x48, 0xFF), 0.f, 0, 1.0f);

        ImVec2 innerTL(bTL.x + 1, bTL.y + 1);
        ImVec2 innerBR(bBR.x - 1, bBR.y - 1);
        float innerW = innerBR.x - innerTL.x;
        float innerH = innerBR.y - innerTL.y;

        if (isDead)
        {
            DrawGradientRect(dl, innerTL, innerBR, *fillGrad);
        }
        else
        {
            DrawGradientRect(dl, innerTL, innerBR, kDeadRed);

            float fillPct = hasDeepWound ? std::min(hpPct, 0.80f) : hpPct;
            if (fillPct > 0.f)
                DrawGradientRect(dl, innerTL, ImVec2(innerTL.x + innerW * fillPct, innerBR.y), *fillGrad);

            if (hasDeepWound)
                DrawGradientRect(dl, ImVec2(innerTL.x + innerW * 0.80f, innerTL.y), innerBR, kDeepWound);
        }

        // Status icons inside bar (right-aligned, same as party window)
        if (!isDead)
        {
            PartyIcons icons = LoadAllPartyIcons(dev);
            const float iconSz = std::min(innerH - 2.f, 14.f);
            float iconX = innerBR.x - 2.f;
            float iconY = innerTL.y + (innerH - iconSz) * 0.5f;

            if (snap->has_weapon_spell && icons.weaponSpell)
            {
                iconX -= iconSz;
                dl->AddImage(icons.weaponSpell, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
                iconX -= 1.f;
            }
            if (snap->has_enchantment && icons.enchanted)
            {
                iconX -= iconSz;
                dl->AddImage(icons.enchanted, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
                iconX -= 1.f;
            }
            if ((snap->has_condition || snap->has_deep_wound || snap->has_bleeding || snap->has_poison) && icons.condition)
            {
                iconX -= iconSz;
                dl->AddImage(icons.condition, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
                iconX -= 1.f;
            }
            if (snap->has_hex && icons.hexed)
            {
                iconX -= iconSz;
                dl->AddImage(icons.hexed, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            }
        }

        if (isDead)
        {
            // Solid red bar + centered DEAD label
            DrawGradientRect(dl, innerTL, innerBR, kDeadRed);
            const char* deadLabel = "DEAD";
            ImVec2 dSz = ImGui::CalcTextSize(deadLabel);
            float dScale = 11.f / ImGui::GetFontSize();
            float dx = innerTL.x + (innerW - dSz.x * dScale) * 0.5f;
            float dy = innerTL.y + (innerH - 11.f) * 0.5f;
            dl->AddText(nullptr, 11.f, ImVec2(dx + 1, dy + 1), IM_COL32(0, 0, 0, 0xCC), deadLabel);
            dl->AddText(nullptr, 11.f, ImVec2(dx, dy), IM_COL32(255, 255, 255, 255), deadLabel);
        }
        else
        {
            // HP text with shadow (inside bar, left-aligned)
            char hpBuf[32];
            if (snap->max_hp > 0)
                snprintf(hpBuf, sizeof(hpBuf), "%d / %d", (int)(hpPct * snap->max_hp), (int)snap->max_hp);
            else
                snprintf(hpBuf, sizeof(hpBuf), "%d%%", (int)(hpPct * 100));
            ImVec2 textPos(innerTL.x + 4.f, innerTL.y + (innerH - ImGui::GetFontSize()) * 0.5f);
            dl->AddText(ImVec2(textPos.x + 1, textPos.y + 1), IM_COL32(0, 0, 0, 0xCC), hpBuf);
            dl->AddText(textPos, IM_COL32(255, 255, 255, 255), hpBuf);
        }

        // Morale/DP now drawn in dedicated row above HP bar

        sec.y = bBR.y + 4.f;

        if (isDead)
        {
            bool beingRezzed = false;
            for (auto& [aid, otherArd] : m_replayCtx.agents)
            {
                if (aid == m_playerInfoAgentId) continue;
                for (auto& ev : otherArd.skillUseHistory)
                {
                    if (ev.startTime > m_debugTimeline) break;
                    if (ev.endTime < m_debugTimeline) continue;
                    if (ev.targetId == m_playerInfoAgentId && !ev.wasCancelled)
                    {
                        const auto& db = m_skillView;
                        const SkillInfo* sinfo = db.IsLoaded() ? db.Get(ev.skillId) : nullptr;
                        if (sinfo && sinfo->type == 22)
                            beingRezzed = true;
                    }
                }
            }

            if (beingRezzed)
            {
                float pulse = 0.6f + 0.4f * (sinf((float)ImGui::GetTime() * 7.85f) * 0.5f + 0.5f);
                dl->AddText(nullptr, 11.f, ImVec2(sec.x + 4.f, sec.y),
                    IM_COL32(224, 120, 48, (int)(255 * pulse)), "Being rezzed...");
                sec.y += 16.f;
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(sec.x, sec.y + 4.f));
    }

    // Shared tooltip lambda for skill icons (used by Sections 3 and 5)
    auto DrawPipSkillTooltip = [&](int skillId, ImTextureID skillTex, const char* modTooltip = nullptr) {
        const auto& db = m_skillView;
        const SkillInfo* si = db.IsLoaded() ? db.Get(skillId) : nullptr;
        if (!si) return;

        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.039f, 0.055f, 0.071f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.63f, 0.13f, 0.3f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(340.f);

        if (skillTex) { ImGui::Image(skillTex, ImVec2(40, 40)); ImGui::SameLine(); }

        ImGui::BeginGroup();
        if (si->is_elite)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.85f, 0.3f, 1.f));
            ImGui::Text("{Elite} %s", si->name.c_str());
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s", si->name.c_str());
        }

        const char* typeName = SkillDatabase::GetTypeName(si->type);
        const char* attrName = SkillDatabase::GetAttributeName(si->attribute);
        if (typeName[0] || attrName[0])
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.f));
            if (attrName[0])
                ImGui::Text("%s  (%s)", typeName, attrName);
            else
                ImGui::TextUnformatted(typeName);
            ImGui::PopStyleColor();
        }
        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        {
            bool hasCost = false;

            auto CostInt = [&](const char* iconFile, const char* fmt, int val) {
                if (val <= 0) return;
                ImTextureID tex = LoadSkillDescIcon(dev, iconFile);
                if (!tex) { if (hasCost) ImGui::SameLine(0, 10); ImGui::Text(fmt, val); hasCost = true; return; }
                if (hasCost) ImGui::SameLine(0, 10);
                ImGui::Image(tex, ImVec2(14.f, 14.f));
                ImGui::SameLine(0, 4);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.f);
                ImGui::Text(fmt, val);
                hasCost = true;
            };

            auto CostFloat = [&](const char* iconFile, const char* fmt, float val) {
                if (val <= 0.f) return;
                ImTextureID tex = LoadSkillDescIcon(dev, iconFile);
                if (!tex) { if (hasCost) ImGui::SameLine(0, 10); ImGui::Text(fmt, val); hasCost = true; return; }
                if (hasCost) ImGui::SameLine(0, 10);
                ImGui::Image(tex, ImVec2(14.f, 14.f));
                ImGui::SameLine(0, 4);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.f);
                ImGui::Text(fmt, val);
                hasCost = true;
            };

            CostInt("energy.png", "%d", si->energy);
            CostInt("adrenaline.png", "%d", si->adrenaline);
            CostInt("sacrifice.png", "%d%%", si->sacrifice);
            if (si->upkeep < 0)
                CostInt("upkeep.png", "%d", -si->upkeep);

            if (si->activation > 0)
                CostFloat("activation.png",
                    (si->activation == (int)si->activation) ? "%.0f" : "%.1f",
                    si->activation);
            if (si->recharge > 0)
                CostFloat("recharge.png",
                    (si->recharge == (int)si->recharge) ? "%.0f" : "%.1f",
                    si->recharge);
            CostInt("overcast.png", "%d", si->overcast);
        }

        ImGui::Spacing();

        if (!si->description.empty())
        {
            std::string desc = si->description;
            size_t pos;
            while ((pos = desc.find('<')) != std::string::npos)
            {
                size_t end = desc.find('>', pos);
                if (end == std::string::npos) break;
                desc.erase(pos, end - pos + 1);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.82f, 0.75f, 1.f));
            ImGui::TextWrapped("%s", desc.c_str());
            ImGui::PopStyleColor();
        }

        if (modTooltip && modTooltip[0])
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.19f, 0.63f, 0.63f, 1.f));
            ImGui::Text("Recharge modifier: %s", modTooltip);
            ImGui::PopStyleColor();
        }

        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    };

    // Pre-compute skill stats (shared by Section 3 cast counter tooltip and Section 5)
    auto pipSkillStats = BuildSkillStats(m_playerInfoAgentId, m_debugTimeline);

    // ═══════════════════════════════════════════════════════════════
    // SECTION 3 — SKILL ICON BAR (skills with cast counters)
    // ═══════════════════════════════════════════════════════════════
    float pipSkillIconSz = 40.f; // will be overwritten by computed skill icon size
    {
        float winX = ImGui::GetWindowPos().x;
        ImVec2 skPos = ImVec2(winX + kPadX, ImGui::GetCursorScreenPos().y);
        float innerW = contentW - 2 * kPadX;

        dl->AddLine(ImVec2(skPos.x, skPos.y), ImVec2(skPos.x + innerW, skPos.y), kDivider);
        skPos.y += 8.f;

        struct SkillSlot {
            int skillId = 0;
            int castsAtCurrent = 0;
            int castsTotal = 0;
            float firstUseTime = 1e9f;
        };
        std::vector<SkillSlot> slots;
        {
            const auto& sdb = m_skillView;

            // Count casts per skill from history, resolving PvE skills to their PvP variant
            std::unordered_map<int, SkillSlot> slotMap;
            for (const auto& ev : ard.skillUseHistory)
            {
                int resolved = sdb.ResolvePvpSkillId(ev.skillId);
                auto& sl = slotMap[resolved];
                if (sl.skillId == 0) { sl.skillId = resolved; sl.firstUseTime = ev.startTime; }
                sl.castsTotal++;
                if (ev.startTime <= m_debugTimeline)
                    sl.castsAtCurrent++;
            }

            const std::vector<int>* usedSkills = nullptr;
            const PlayerMeta* playerMeta = nullptr;
            for (const auto& [pid, party] : m_matchMeta.parties)
            {
                for (const auto& pm : party.players)
                {
                    if (pm.id == m_playerInfoAgentId && !pm.used_skills.empty())
                    { usedSkills = &pm.used_skills; playerMeta = &pm; break; }
                }
                if (usedSkills) break;
                for (const auto& pm : party.others)
                {
                    if (pm.id == m_playerInfoAgentId && !pm.used_skills.empty())
                    { usedSkills = &pm.used_skills; playerMeta = &pm; break; }
                }
                if (usedSkills) break;
            }

            std::unordered_set<int> placed;
            if (usedSkills)
            {
                for (int sid : *usedSkills)
                {
                    int resolved = sdb.ResolvePvpSkillId(sid);
                    if (!placed.insert(resolved).second) continue;
                    auto it = slotMap.find(resolved);
                    if (it != slotMap.end())
                        slots.push_back(it->second);
                    else
                        slots.push_back({ resolved, 0, 0, 1e9f });
                }
            }
            // Append any skills from history not in used_skills, ordered by first use
            std::vector<SkillSlot> extras;
            for (auto& [id, sl] : slotMap)
                if (placed.find(id) == placed.end())
                    extras.push_back(sl);
            std::sort(extras.begin(), extras.end(),
                [](const SkillSlot& a, const SkillSlot& b) { return a.firstUseTime < b.firstUseTime; });
            for (auto& e : extras)
                slots.push_back(e);

            // Sort slots by profession-aware priority buckets
            if (playerMeta && slots.size() > 1)
            {
                std::vector<int> ids;
                ids.reserve(slots.size());
                for (auto& s : slots) ids.push_back(s.skillId);

                auto sorted = m_skillView.SortSkillsForDisplay(
                    ids, playerMeta->primary, playerMeta->secondary);

                std::unordered_map<int, SkillSlot> slotById;
                for (auto& s : slots) slotById[s.skillId] = s;
                slots.clear();
                for (int sid : sorted)
                {
                    auto it = slotById.find(sid);
                    if (it != slotById.end())
                        slots.push_back(it->second);
                }
            }
        }

        int numSlots = std::max((int)slots.size(), 1);
        constexpr float kSkGap = 5.f;
        constexpr int kGridCols = 8;
        float kSkIconSz = std::floor((innerW - (kGridCols - 1) * kSkGap) / kGridCols);
        if (kSkIconSz < 28.f) kSkIconSz = 28.f;

        float startX = skPos.x;
        pipSkillIconSz = kSkIconSz;

        EnsureSkillIconIndex();

        float kCountFontX = (kSkIconSz < 32.f) ? 10.f : 12.f;
        float kCountFontSm = (kSkIconSz < 32.f) ? 9.f : 11.f;

        // ── Compute per-skill cooldown state for current timestamp ──
        constexpr int kResSigId = 2;
        constexpr int kComplicateId = 932;
        constexpr float kComplicateDuration = 12.f;

        // Recharge modifier skill IDs
        constexpr int kSqSkillId  = 456;   // Serpent's Quickness
        constexpr int kPsSkillId  = 449;   // Practiced Stance
        constexpr int kDpSkillId  = 572;   // Deadly Paradox
        constexpr int kAolSkillId = 1521;  // Avatar of Lyssa
        constexpr int kLhSkillId  = 1512;  // Lyssa's Haste
        constexpr uint32_t kQzSpiritModelId = 2937;
        constexpr float kQzRange = 2512.f;

        struct RechargeModifier {
            int   sourceSkillId    = 0;
            float multiplier       = 1.f;
            bool  appliesToAll     = false;
            bool  preparationsOnly = false;
            bool  assassinOnly     = false;
            bool  dervishEnchOnly  = false;
            const char* sourceName = "";
        };
        std::vector<RechargeModifier> activeModifiers;
        bool dpDisableAttacks = false;
        float dpDisableExpiry = 0.f;
        bool qzActive = false;

        // ── Build active modifiers from skill cast history (recomputed every frame for scrub) ──
        const auto& skDb = m_skillView;
        {
            float curTime = m_debugTimeline;

            // --- Quickening Zephyr: check all QZ spirits alive + in range ---
            {
                float playerX = 0, playerY = 0, playerZ = 0;
                InterpolateAgentPosition(ard, curTime, m_replayCtx.interpSettings, playerX, playerY, playerZ);

                for (auto& [spiritId, spiritArd] : m_replayCtx.agents)
                {
                    if (spiritArd.modelId != kQzSpiritModelId) continue;
                    if (spiritArd.isDeadAtTime(curTime)) continue;
                    if (!spiritArd.isAliveAtTime(curTime)) continue;

                    float spX = 0, spY = 0, spZ = 0;
                    InterpolateAgentPosition(spiritArd, curTime, m_replayCtx.interpSettings, spX, spY, spZ);
                    float dx = playerX - spX, dy = playerY - spY;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist <= kQzRange)
                    {
                        qzActive = true;
                        break;
                    }
                }
            }

            // --- Scan focused agent's own casts for buff-type modifiers ---
            for (const auto& ev : ard.skillUseHistory)
            {
                if (ev.wasCancelled) continue;
                float castEnd = ev.isInstant ? ev.startTime : ev.endTime;
                if (castEnd > curTime) continue;

                // Serpent's Quickness (SQ): all skills x0.67, 20s, ends if HP<50%
                if (ev.skillId == kSqSkillId)
                {
                    float expiresAt = castEnd + 20.f;
                    if (curTime < expiresAt)
                    {
                        float hpPct = ard.healthPctAtTime(curTime);
                        if (hpPct >= 0.50f)
                        {
                            RechargeModifier m;
                            m.sourceSkillId = kSqSkillId;
                            m.multiplier = 0.67f;
                            m.appliesToAll = true;
                            m.sourceName = "SQ";
                            activeModifiers.push_back(m);
                        }
                    }
                }

                // Practiced Stance (PS): preparations only x0.50
                if (ev.skillId == kPsSkillId)
                {
                    const SkillInfo* psInfo = skDb.Get(kPsSkillId);
                    float dur = psInfo ? psInfo->recharge : 30.f;
                    float expiresAt = castEnd + dur;
                    if (curTime < expiresAt)
                    {
                        RechargeModifier m;
                        m.sourceSkillId = kPsSkillId;
                        m.multiplier = 0.50f;
                        m.preparationsOnly = true;
                        m.sourceName = "PS";
                        activeModifiers.push_back(m);
                    }
                }

                // Deadly Paradox (DP): assassin skills x0.67 + disable attacks, 10s
                if (ev.skillId == kDpSkillId)
                {
                    float expiresAt = castEnd + 10.f;
                    if (curTime < expiresAt)
                    {
                        RechargeModifier m;
                        m.sourceSkillId = kDpSkillId;
                        m.multiplier = 0.67f;
                        m.assassinOnly = true;
                        m.sourceName = "DP";
                        activeModifiers.push_back(m);

                        dpDisableAttacks = true;
                        dpDisableExpiry = expiresAt;
                    }
                }

                // Avatar of Lyssa: dervish enchantments x0.50
                if (ev.skillId == kAolSkillId)
                {
                    const SkillInfo* aolInfo = skDb.Get(kAolSkillId);
                    float dur = aolInfo ? aolInfo->recharge : 45.f;
                    float expiresAt = castEnd + dur;
                    if (curTime < expiresAt && !ev.wasCancelled)
                    {
                        RechargeModifier m;
                        m.sourceSkillId = kAolSkillId;
                        m.multiplier = 0.50f;
                        m.dervishEnchOnly = true;
                        m.sourceName = "AoL";
                        activeModifiers.push_back(m);
                    }
                }

                // Lyssa's Haste: dervish enchantments x0.67
                if (ev.skillId == kLhSkillId)
                {
                    const SkillInfo* lhInfo = skDb.Get(kLhSkillId);
                    float dur = lhInfo ? lhInfo->recharge : 20.f;
                    float expiresAt = castEnd + dur;
                    if (curTime < expiresAt && !ev.wasCancelled)
                    {
                        RechargeModifier m;
                        m.sourceSkillId = kLhSkillId;
                        m.multiplier = 0.67f;
                        m.dervishEnchOnly = true;
                        m.sourceName = "LH";
                        activeModifiers.push_back(m);
                    }
                }
            }
        }

        // Lambda: compute effective recharge multiplier for a given skill
        auto GetRechargeMultiplier = [&](int skillId) -> float
        {
            const SkillInfo* si = skDb.Get(skillId);
            float mult = 1.f;

            // QZ applies to all skills
            if (qzActive) mult *= 0.50f;

            for (const auto& mod : activeModifiers)
            {
                if (mod.appliesToAll)
                {
                    mult *= mod.multiplier;
                    continue;
                }
                if (mod.preparationsOnly && si && si->type == 17)
                {
                    mult *= mod.multiplier;
                    continue;
                }
                if (mod.assassinOnly && si && si->profession == 7)
                {
                    mult *= mod.multiplier;
                    continue;
                }
                if (mod.dervishEnchOnly && si && si->profession == 10 &&
                    (si->type == 15 || si->type == 23 || si->type == 33 || si->type == 34))
                {
                    mult *= mod.multiplier;
                    continue;
                }
            }

            return mult;
        };

        // Lambda: is this skill an attack skill (disabled by Deadly Paradox)
        auto IsAttackSkill = [&](int skillId) -> bool
        {
            const SkillInfo* si = skDb.Get(skillId);
            if (!si) return false;
            return si->type == 2 || si->type == 3 || si->type == 4 ||
                   si->type == 5 || si->type == 6 || si->type == 7 ||
                   si->type == 8 || si->type == 9 || si->type == 10;
        };

        // Collect morale boost timestamps for this team
        std::vector<float> teamMoraleBoosts;
        for (auto& jmb : m_replayCtx.stocData.jumbo)
        {
            if (jmb.time > m_debugTimeline) break;
            if (jmb.message == "MORALE_BOOST")
            {
                bool isTeam1 = (jmb.party_value == 1635021873);
                bool isTeam2 = (jmb.party_value == 1635021874);
                if ((ard.teamId == 1 && isTeam1) || (ard.teamId == 2 && isTeam2))
                    teamMoraleBoosts.push_back(jmb.time);
            }
        }

        // Collect Complicate lockouts affecting this agent
        struct ComplicateLock {
            int skillId = 0;
            float start = 0.f;
            float end = 0.f;
        };
        std::vector<ComplicateLock> complicateLocks;
        for (const auto& ev : ard.skillUseHistory)
        {
            if (!ev.wasInterrupted) continue;
            if (ev.endTime > m_debugTimeline) break;
            // Check if a Complicate was the source of this interrupt
            for (const auto& [eid, eard] : m_replayCtx.agents)
            {
                if (eard.teamId == ard.teamId) continue;
                for (const auto& eev : eard.skillUseHistory)
                {
                    if (eev.skillId != kComplicateId) continue;
                    if (eev.wasCancelled) continue;
                    if (eev.targetId != m_playerInfoAgentId) continue;
                    if (std::abs(eev.endTime - ev.endTime) < 0.3f)
                    {
                        float lockEnd = eev.endTime + kComplicateDuration;
                        bool clearedByBoost = false;
                        for (float bt : teamMoraleBoosts)
                            if (bt > eev.endTime && bt <= m_debugTimeline) { clearedByBoost = true; break; }
                        if (!clearedByBoost && lockEnd > m_debugTimeline)
                        {
                            ComplicateLock cl; cl.skillId = ev.skillId; cl.start = eev.endTime; cl.end = lockEnd;
                            complicateLocks.push_back(cl);
                        }
                    }
                }
            }
        }
        // Also check Complicate against allies in range (AoE effect on focused agent)
        for (const auto& [eid, eard] : m_replayCtx.agents)
        {
            if (eard.teamId == ard.teamId) continue;
            for (const auto& eev : eard.skillUseHistory)
            {
                if (eev.skillId != kComplicateId || eev.wasCancelled) continue;
                if (eev.endTime > m_debugTimeline) continue;
                if (eev.targetId == m_playerInfoAgentId) continue;
                // Find the target agent (ally of focused)
                auto targetIt = m_replayCtx.agents.find(eev.targetId);
                if (targetIt == m_replayCtx.agents.end()) continue;
                if (targetIt->second.teamId != ard.teamId) continue;
                // Find interrupted skill on the direct target
                int intSkill = 0;
                for (const auto& tev : targetIt->second.skillUseHistory)
                {
                    if (!tev.wasInterrupted) continue;
                    if (std::abs(tev.endTime - eev.endTime) < 0.3f)
                    { intSkill = tev.skillId; break; }
                }
                if (intSkill == 0) continue;
                // Check distance: focused agent must be in range of direct target
                float tx, ty, tz, fx, fy, fz;
                InterpolateAgentPosition(targetIt->second, eev.endTime, m_replayCtx.interpSettings, tx, ty, tz);
                InterpolateAgentPosition(ard, eev.endTime, m_replayCtx.interpSettings, fx, fy, fz);
                float dx = tx - fx, dy = ty - fy, dz = tz - fz;
                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (dist > 322.f) continue;
                float lockEnd = eev.endTime + kComplicateDuration;
                bool clearedByBoost = false;
                for (float bt : teamMoraleBoosts)
                    if (bt > eev.endTime && bt <= m_debugTimeline) { clearedByBoost = true; break; }
                if (!clearedByBoost && lockEnd > m_debugTimeline)
                {
                    ComplicateLock cl; cl.skillId = intSkill; cl.start = eev.endTime; cl.end = lockEnd;
                    complicateLocks.push_back(cl);
                }
            }
        }

        // 0=available, 1=recharging, 2=permanent (Res Signet)
        struct SkillCooldown {
            int state = 0;
            float rechargeStart = 0.f;
            float rechargeDuration = 0.f;
            float remaining = 0.f;
            bool isComplicate = false;
            float rechargeMult = 1.f;
            bool isDisabledByDP = false;
            float dpCountdown = 0.f;
            std::string modifierTooltip;
        };
        constexpr int RS_AVAILABLE = 0, RS_RECHARGING = 1, RS_PERMANENT = 2;
        std::vector<SkillCooldown> cooldowns;
        cooldowns.resize(slots.size());

        for (int si2 = 0; si2 < (int)slots.size(); si2++)
        {
            int sid = slots[si2].skillId;
            SkillCooldown& cd = cooldowns[si2];

            // Scan all successful casts for this skill up to current time
            for (const auto& ev : ard.skillUseHistory)
            {
                if (ev.skillId != sid) continue;
                if (ev.wasCancelled) continue;
                if (ev.endTime > m_debugTimeline) continue;
                float castEnd = ev.isInstant ? ev.startTime : ev.endTime;
                if (sid == kResSigId)
                {
                    cd.state = RS_PERMANENT;
                    cd.rechargeStart = castEnd;
                }
                else if (ev.rechargeDuration > 0.f)
                {
                    cd.state = RS_RECHARGING;
                    cd.rechargeStart = castEnd;
                    cd.rechargeDuration = ev.rechargeDuration;
                }
            }

            // Check if recharge expired
            if (cd.state == RS_RECHARGING)
            {
                cd.remaining = cd.rechargeStart + cd.rechargeDuration - m_debugTimeline;
                if (cd.remaining <= 0.f)
                    cd.state = RS_AVAILABLE;
            }

            // Morale boost clears both RECHARGING and PERMANENT
            if (cd.state != RS_AVAILABLE)
            {
                for (float bt : teamMoraleBoosts)
                {
                    if (bt > cd.rechargeStart && bt <= m_debugTimeline)
                    { cd.state = RS_AVAILABLE; break; }
                }
            }

            // Complicate override (takes priority if still active)
            for (const auto& cl : complicateLocks)
            {
                if (cl.skillId == sid)
                {
                    float rem = cl.end - m_debugTimeline;
                    if (rem > 0.f && (cd.state == RS_AVAILABLE || rem > cd.remaining))
                    {
                        cd.state = RS_RECHARGING;
                        cd.rechargeStart = cl.start;
                        cd.rechargeDuration = kComplicateDuration;
                        cd.remaining = rem;
                        cd.isComplicate = true;
                    }
                }
            }

            // Apply recharge modifier to remaining time
            cd.rechargeMult = GetRechargeMultiplier(sid);
            if (cd.state == RS_RECHARGING && !cd.isComplicate)
            {
                float effectiveDur = cd.rechargeDuration * cd.rechargeMult;
                cd.remaining = cd.rechargeStart + effectiveDur - m_debugTimeline;
                if (cd.remaining <= 0.f)
                    cd.state = RS_AVAILABLE;
            }
            else if (cd.state == RS_RECHARGING)
            {
                cd.remaining = cd.rechargeStart + cd.rechargeDuration - m_debugTimeline;
            }

            // Deadly Paradox disables attack skills
            if (dpDisableAttacks && IsAttackSkill(sid))
            {
                cd.isDisabledByDP = true;
                cd.dpCountdown = dpDisableExpiry - m_debugTimeline;
            }

            // Build modifier tooltip string
            if (cd.rechargeMult < 0.999f || cd.isDisabledByDP)
            {
                std::string tt;
                if (qzActive) tt += "QZ x0.50";
                for (const auto& mod : activeModifiers)
                {
                    bool applies = false;
                    if (mod.appliesToAll) applies = true;
                    else if (mod.preparationsOnly) {
                        const SkillInfo* si = skDb.Get(sid);
                        if (si && si->type == 17) applies = true;
                    }
                    else if (mod.assassinOnly) {
                        const SkillInfo* si = skDb.Get(sid);
                        if (si && si->profession == 7) applies = true;
                    }
                    else if (mod.dervishEnchOnly) {
                        const SkillInfo* si = skDb.Get(sid);
                        if (si && si->profession == 10 &&
                            (si->type == 15 || si->type == 23 || si->type == 33 || si->type == 34))
                            applies = true;
                    }
                    if (applies)
                    {
                        if (!tt.empty()) tt += " + ";
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%s x%.2f", mod.sourceName, mod.multiplier);
                        tt += buf;
                    }
                }
                cd.modifierTooltip = tt;
            }
        }

        // Detect recent morale boost for white flash effect
        float moraleFlashAlpha = 0.f;
        if (!teamMoraleBoosts.empty())
        {
            float lastBoost = teamMoraleBoosts.back();
            float elapsed = m_debugTimeline - lastBoost;
            if (elapsed >= 0.f && elapsed < 0.15f)
                moraleFlashAlpha = 0.6f * (1.f - elapsed / 0.15f);
        }

        // Hover delay state for cast counter tooltip
        static int    s_hoverSkillId   = -1;
        static float  s_hoverStartTime = -1.f;
        constexpr float kHoverDelay    = 0.080f;
        int hoveredCounterSkill = -1;

        for (int i = 0; i < (int)slots.size(); i++)
        {
            auto& sl = slots[i];
            int col = i % kGridCols;
            int row = i / kGridCols;
            float ix = startX + col * (kSkIconSz + kSkGap);
            float iy = skPos.y + row * (kSkIconSz + 3.f + kCountFontX + 6.f);

            dl->AddRectFilled(ImVec2(ix, iy), ImVec2(ix + kSkIconSz, iy + kSkIconSz),
                              IM_COL32(0, 0, 0, 77), 5.f);
            dl->AddRect(ImVec2(ix, iy), ImVec2(ix + kSkIconSz, iy + kSkIconSz),
                        IM_COL32(255, 255, 255, 31), 5.f);

            ImTextureID skillTex = LoadSkillIcon(this, dev, sl.skillId,
                m_skillIconIndex, m_skillIconCache);
            if (skillTex)
                dl->AddImage(skillTex, ImVec2(ix + 1, iy + 1),
                             ImVec2(ix + kSkIconSz - 1, iy + kSkIconSz - 1));

            // ── Recharge arc overlay ──
            const SkillCooldown& cd = cooldowns[i];
            if (cd.state == RS_PERMANENT)
            {
                // Full dark overlay (Res Signet — permanent until morale boost)
                float cx = ix + kSkIconSz * 0.5f, cy = iy + kSkIconSz * 0.5f;
                float r = kSkIconSz * 0.5f;
                dl->AddCircleFilled(ImVec2(cx, cy), r, IM_COL32(0, 0, 0, 166), 32);
            }
            else if (cd.state == RS_RECHARGING && cd.rechargeDuration > 0.f)
            {
                float effectiveDur = cd.isComplicate ? cd.rechargeDuration : cd.rechargeDuration * cd.rechargeMult;
                float progress = (effectiveDur > 0.f) ? std::clamp(cd.remaining / effectiveDur, 0.f, 1.f) : 0.f;
                float cx = ix + kSkIconSz * 0.5f, cy = iy + kSkIconSz * 0.5f;
                float r = kSkIconSz * 0.5f;

                ImU32 arcCol = cd.isComplicate
                    ? IM_COL32(80, 0, 80, 179)
                    : IM_COL32(0, 0, 0, 166);

                if (progress > 0.001f)
                {
                    // Filled pie from 12 o'clock, clockwise sweep
                    float startAngle = -IM_PI * 0.5f;
                    float sweepAngle = 2.f * IM_PI * progress;
                    int nArcSegs = std::max(8, (int)(32.f * progress));

                    dl->PathClear();
                    dl->PathLineTo(ImVec2(cx, cy));
                    for (int s = 0; s <= nArcSegs; s++)
                    {
                        float a = startAngle - sweepAngle * ((float)s / nArcSegs);
                        dl->PathLineTo(ImVec2(cx + r * cosf(a), cy + r * sinf(a)));
                    }
                    dl->PathFillConvex(arcCol);
                }

                // Countdown text
                if (cd.remaining > 1.0f)
                {
                    char cdBuf[8];
                    snprintf(cdBuf, sizeof(cdBuf), "%d", (int)ceilf(cd.remaining));
                    ImVec2 cdSz = ImGui::CalcTextSize(cdBuf);
                    float cdScale = 10.f / ImGui::GetFontSize();
                    float cdW = cdSz.x * cdScale;
                    float cdH = cdSz.y * cdScale;
                    dl->AddText(nullptr, 10.f,
                        ImVec2(cx - cdW * 0.5f + 1.f, cy - cdH * 0.5f + 1.f),
                        IM_COL32(0, 0, 0, 200), cdBuf);
                    dl->AddText(nullptr, 10.f,
                        ImVec2(cx - cdW * 0.5f, cy - cdH * 0.5f),
                        IM_COL32(255, 255, 255, 230), cdBuf);
                }
            }

            // Morale boost white flash
            if (moraleFlashAlpha > 0.f && cd.state == RS_AVAILABLE)
            {
                ImU8 flashA = (ImU8)(moraleFlashAlpha * 255.f);
                dl->AddRectFilled(ImVec2(ix, iy), ImVec2(ix + kSkIconSz, iy + kSkIconSz),
                    IM_COL32(255, 255, 255, flashA), 5.f);
            }

            // Deadly Paradox disabled overlay (attack skills)
            if (cd.isDisabledByDP && cd.dpCountdown > 0.f)
            {
                dl->AddRectFilled(ImVec2(ix, iy), ImVec2(ix + kSkIconSz, iy + kSkIconSz),
                    IM_COL32(0, 0, 0, 191), 5.f);
                // Red X
                const char* xStr = "X";
                ImVec2 xSz = ImGui::CalcTextSize(xStr);
                float xScale = 14.f / ImGui::GetFontSize();
                float xW = xSz.x * xScale, xH = xSz.y * xScale;
                float cxDP = ix + kSkIconSz * 0.5f, cyDP = iy + kSkIconSz * 0.35f;
                dl->AddText(nullptr, 14.f,
                    ImVec2(cxDP - xW * 0.5f, cyDP - xH * 0.5f),
                    IM_COL32(204, 48, 48, 255), xStr);
                // Countdown below X
                char dpBuf[8];
                snprintf(dpBuf, sizeof(dpBuf), "%.0f", ceilf(cd.dpCountdown));
                ImVec2 dpSz = ImGui::CalcTextSize(dpBuf);
                float dpScale = 9.f / ImGui::GetFontSize();
                float dpW = dpSz.x * dpScale;
                dl->AddText(nullptr, 9.f,
                    ImVec2(cxDP - dpW * 0.5f, cyDP + xH * 0.4f),
                    IM_COL32(204, 48, 48, 200), dpBuf);
            }

            // QZ active indicator: thin cyan border ring
            if (qzActive && cd.rechargeMult < 0.999f)
            {
                dl->AddRect(ImVec2(ix - 1, iy - 1),
                    ImVec2(ix + kSkIconSz + 1, iy + kSkIconSz + 1),
                    IM_COL32(48, 160, 160, 200), 5.f, 0, 1.5f);
            }

            // Complicate tooltip on hover
            if (cd.isComplicate && cd.state == RS_RECHARGING)
            {
                if (ImGui::IsMouseHoveringRect(ImVec2(ix, iy), ImVec2(ix + kSkIconSz, iy + kSkIconSz)))
                {
                    char ttBuf[128];
                    if (qzActive)
                        snprintf(ttBuf, sizeof(ttBuf), "On cooldown: Complicate (%.0fs) + QZ active", cd.remaining);
                    else
                        snprintf(ttBuf, sizeof(ttBuf), "On cooldown: Complicate (%.0fs)", cd.remaining);
                    ImGui::SetTooltip("%s", ttBuf);
                }
            }

            // Icon hover → skill description tooltip (only if not showing complicate tooltip)
            bool iconHovered = false;
            if (!(cd.isComplicate && cd.state == RS_RECHARGING))
            {
                iconHovered = ImGui::IsMouseHoveringRect(
                    ImVec2(ix, iy), ImVec2(ix + kSkIconSz, iy + kSkIconSz));
                if (iconHovered)
                {
                    const char* modTip = (!cd.modifierTooltip.empty()) ? cd.modifierTooltip.c_str() : nullptr;
                    DrawPipSkillTooltip(sl.skillId, skillTex, modTip);
                }
            }

            // Cast counter text
            float textY = iy + kSkIconSz + 3.f;
            float counterH = kCountFontX + 2.f;
            ImVec2 counterMin(ix, textY);
            ImVec2 counterMax(ix + kSkIconSz, textY + counterH);
            bool counterHovered = !iconHovered && ImGui::IsMouseHoveringRect(counterMin, counterMax);

            if (sl.castsTotal == 0)
            {
                const char* dash = "--";
                ImVec2 dSz = ImGui::CalcTextSize(dash);
                float dScale = kCountFontSm / ImGui::GetFontSize();
                dl->AddText(nullptr, kCountFontSm,
                    ImVec2(ix + (kSkIconSz - dSz.x * dScale) * 0.5f, textY),
                    IM_COL32(0x70, 0x7d, 0x88, 0xFF), dash);
            }
            else
            {
                char xBuf[8], yBuf[8];
                snprintf(xBuf, sizeof(xBuf), "%d", sl.castsAtCurrent);
                snprintf(yBuf, sizeof(yBuf), "%d", sl.castsTotal);
                float fScaleX = kCountFontX / ImGui::GetFontSize();
                float fScaleSm = kCountFontSm / ImGui::GetFontSize();
                ImVec2 xSz = ImGui::CalcTextSize(xBuf);  xSz.x *= fScaleX;
                ImVec2 sSz = ImGui::CalcTextSize("/");    sSz.x *= fScaleSm;
                ImVec2 ySz = ImGui::CalcTextSize(yBuf);   ySz.x *= fScaleSm;
                float totalTW = xSz.x + sSz.x + ySz.x;
                float tx = ix + (kSkIconSz - totalTW) * 0.5f;
                dl->AddText(nullptr, kCountFontX, ImVec2(tx, textY),
                    IM_COL32(0xF0, 0xF0, 0xF0, 0xFF), xBuf);
                tx += xSz.x;
                dl->AddText(nullptr, kCountFontSm, ImVec2(tx, textY + 1.f),
                    IM_COL32(0x50, 0x5a, 0x64, 0xFF), "/");
                tx += sSz.x;
                dl->AddText(nullptr, kCountFontSm, ImVec2(tx, textY + 1.f),
                    IM_COL32(0x70, 0x7d, 0x88, 0xFF), yBuf);
            }

            if (counterHovered)
                hoveredCounterSkill = sl.skillId;
        }

        // Hover delay logic
        float now = (float)ImGui::GetTime();
        if (hoveredCounterSkill < 0)
        {
            s_hoverSkillId = -1;
            s_hoverStartTime = -1.f;
        }
        else if (s_hoverSkillId != hoveredCounterSkill)
        {
            s_hoverSkillId = hoveredCounterSkill;
            s_hoverStartTime = now;
        }

        bool showCounterTooltip = (hoveredCounterSkill >= 0 &&
            s_hoverSkillId == hoveredCounterSkill &&
            s_hoverStartTime > 0.f &&
            (now - s_hoverStartTime) >= kHoverDelay);

        if (showCounterTooltip)
        {
            const PipSkillStat* foundStat = nullptr;
            for (const auto& st : pipSkillStats)
                if (st.skillId == hoveredCounterSkill) { foundStat = &st; break; }

            ImTextureID ttSkillTex = LoadSkillIcon(this, dev, hoveredCounterSkill,
                m_skillIconIndex, m_skillIconCache);
            std::string ttSkillName = GetSkillDisplayName(hoveredCounterSkill);
            int castsNow = foundStat ? foundStat->totalCasts : 0;

            int hovIdx = -1;
            for (int i = 0; i < (int)slots.size(); i++)
                if (slots[i].skillId == hoveredCounterSkill) { hovIdx = i; break; }
            float hoverIx = startX + (hovIdx >= 0 ? hovIdx : 0) * (kSkIconSz + kSkGap);
            float hoverIy = skPos.y;

            constexpr float kTtPadX = 14.f;
            constexpr float kTtPadY = 12.f;
            constexpr float kTtRowH = 22.f;
            constexpr float kTtRowGap = 3.f;

            bool isSelfOnly = false;
            int numTargets = 0;
            if (foundStat && castsNow > 0)
            {
                isSelfOnly = foundStat->targets.empty() ||
                    (foundStat->targets.size() == 1 &&
                     foundStat->targets[0].targetId == m_playerInfoAgentId);
                numTargets = (int)foundStat->targets.size();
            }

            float headerH = 24.f;
            float dividerH = 6.f;
            float bodyH = 0.f;
            if (!foundStat || castsNow == 0) bodyH = 20.f;
            else if (isSelfOnly) bodyH = 24.f;
            else bodyH = numTargets * (kTtRowH + kTtRowGap);
            float listMaxH = 320.f;
            if (bodyH > listMaxH) bodyH = listMaxH;
            float ttH = kTtPadY * 2 + headerH + dividerH + bodyH;
            float ttW = std::clamp(280.f, 240.f, 320.f);

            // Position with 8px screen margin
            float ttX = hoverIx - ttW - 4.f;
            float ttY = hoverIy;
            if (ttX < 8.f)
            {
                ttX = hoverIx + kSkIconSz + 4.f;
                if (ttX + ttW > vpW - 8.f)
                    ttX = vpW - 8.f - ttW;
            }
            if (ttY + ttH > vpH - 8.f)
                ttY = vpH - 8.f - ttH;
            if (ttY < 8.f) ttY = 8.f;
            if (ttX < 8.f) ttX = 8.f;

            ImGui::SetNextWindowPos(ImVec2(ttX, ttY));
            ImGui::SetNextWindowSize(ImVec2(ttW, 0.f));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.031f, 0.055f, 0.078f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.63f, 0.13f, 0.45f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0.05f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(1.f, 1.f, 1.f, 0.20f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kTtPadX, kTtPadY));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
            ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 3.f);
            ImGui::BeginTooltip();

            ImDrawList* ttDl = ImGui::GetWindowDrawList();
            float innerTtW = ttW - 2 * kTtPadX;

            // Header: [icon 20x20]  [name 12px bold]  [X casts 11px] right-aligned
            if (ttSkillTex)
            {
                ImGui::Image(ttSkillTex, ImVec2(20, 20));
                ImGui::SameLine(0, 6);
            }
            {
                float cy = ImGui::GetCursorPosY();
                ImGui::SetCursorPosY(cy + 2.f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                ImGui::TextUnformatted(ttSkillName.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();

                char castsBuf[32];
                snprintf(castsBuf, sizeof(castsBuf), "%d cast%s", castsNow, castsNow == 1 ? "" : "s");
                ImVec2 cSz = ImGui::CalcTextSize(castsBuf);
                float rightX = innerTtW + kTtPadX - cSz.x;
                if (ImGui::GetCursorPosX() < rightX)
                    ImGui::SetCursorPosX(rightX);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.44f, 0.49f, 0.53f, 1.f));
                ImGui::TextUnformatted(castsBuf);
                ImGui::PopStyleColor();
            }

            // Divider
            {
                ImVec2 dp = ImGui::GetCursorScreenPos();
                ttDl->AddLine(ImVec2(dp.x, dp.y + 2.f), ImVec2(dp.x + innerTtW, dp.y + 2.f),
                    IM_COL32(255, 255, 255, 20));
                ImGui::Dummy(ImVec2(0, 5.f));
            }

            if (!foundStat || castsNow == 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.44f, 0.49f, 0.53f, 1.f));
                ImGui::TextUnformatted("Not used yet");
                ImGui::PopStyleColor();
            }
            else if (isSelfOnly)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.83f, 0.63f, 0.13f, 1.f));
                float tw = ImGui::CalcTextSize("(self only)").x;
                ImGui::SetCursorPosX((ttW - tw) * 0.5f);
                ImGui::TextUnformatted("(self only)");
                ImGui::PopStyleColor();
            }
            else
            {
                // Build sorted target list
                struct TtTarget {
                    int targetId;
                    std::string name;
                    uint8_t teamId;
                    int primaryProf;
                    float pct;
                    bool isSelf;
                };
                std::vector<TtTarget> sorted;
                for (const auto& t : foundStat->targets)
                {
                    TtTarget tt;
                    tt.targetId = t.targetId;
                    tt.name = StripProfPrefix(t.name);
                    tt.teamId = t.teamId;
                    tt.pct = t.pct;
                    tt.isSelf = (t.targetId == m_playerInfoAgentId);
                    tt.primaryProf = 0;
                    auto ait = m_replayCtx.agents.find(t.targetId);
                    if (ait != m_replayCtx.agents.end())
                        tt.primaryProf = ait->second.primaryProf;
                    sorted.push_back(tt);
                }
                std::sort(sorted.begin(), sorted.end(),
                    [](const TtTarget& a, const TtTarget& b) {
                        if (a.isSelf && a.pct <= 0.50f && !b.isSelf) return false;
                        if (b.isSelf && b.pct <= 0.50f && !a.isSelf) return true;
                        return a.pct > b.pct;
                    });

                // Scrollable target list if > 6 targets
                bool needScroll = (int)sorted.size() > 6;
                if (needScroll)
                    ImGui::BeginChild("##TtTargets", ImVec2(innerTtW, listMaxH), false);

                for (int ri = 0; ri < (int)sorted.size(); ri++)
                {
                    const auto& tt = sorted[ri];
                    ImVec2 rp = ImGui::GetCursorScreenPos();

                    // Primary profession icon only, 16x16
                    float iconStackW = 0.f;
                    ImTextureID priTex = (tt.primaryProf > 0) ? LoadProfIcon(dev, tt.primaryProf) : nullptr;
                    if (priTex)
                    {
                        float priY = rp.y + (kTtRowH - 16.f) * 0.5f;
                        ttDl->AddImage(priTex, ImVec2(rp.x, priY), ImVec2(rp.x + 16.f, priY + 16.f));
                        iconStackW = 20.f;
                    }

                    std::string dispName = tt.name;
                    if (tt.isSelf) dispName += " (self)";
                    ImU32 nameCol;
                    if (tt.isSelf) nameCol = kGold;
                    else if (tt.teamId == 1) nameCol = kRedTeam;
                    else nameCol = kBlueTeam;

                    // Percentage text (right-aligned, 32px reserved)
                    constexpr float kPctReservedW = 32.f;
                    constexpr float kBarMaxW = 80.f;
                    constexpr float kBarGap = 8.f;
                    char pBuf[8];
                    int pctInt = (int)(tt.pct * 100);
                    snprintf(pBuf, sizeof(pBuf), "%d%%", pctInt);
                    ImU32 pctCol;
                    if (pctInt > 40)       pctCol = IM_COL32(0xF0, 0xF0, 0xF0, 0xFF);
                    else if (pctInt >= 20)  pctCol = IM_COL32(0xa0, 0xaa, 0xb4, 0xFF);
                    else                    pctCol = IM_COL32(0x70, 0x7d, 0x88, 0xFF);

                    // Layout: [icon 20px] [name] [8px gap] [bar 80px max] [8px gap] [pct 32px]
                    float rightReserved = kBarGap + kBarMaxW + kBarGap + kPctReservedW;
                    float maxNameW = innerTtW - iconStackW - 4.f - rightReserved;
                    if (maxNameW < 40.f) maxNameW = 40.f;

                    float nameX = rp.x + iconStackW + 4.f;
                    float nameY = rp.y + (kTtRowH - 13.f) * 0.5f;
                    ImVec2 nSz = ImGui::CalcTextSize(dispName.c_str());
                    float nScale = 13.f / ImGui::GetFontSize();
                    std::string fullName = dispName;
                    bool truncated = false;
                    if (nSz.x * nScale > maxNameW && dispName.size() > 4)
                    {
                        while (nSz.x * nScale > maxNameW && dispName.size() > 4)
                        {
                            dispName.pop_back();
                            nSz = ImGui::CalcTextSize((dispName + "..").c_str());
                        }
                        dispName += "..";
                        truncated = true;
                    }
                    ttDl->AddText(nullptr, 13.f, ImVec2(nameX, nameY), nameCol, dispName.c_str());

                    if (truncated)
                    {
                        float nhW = nSz.x * nScale;
                        if (ImGui::IsMouseHoveringRect(ImVec2(nameX, nameY),
                            ImVec2(nameX + nhW, nameY + 15.f)))
                            ImGui::SetTooltip("%s", fullName.c_str());
                    }

                    // Proportional bar between name and %
                    ImU32 barCol = nameCol;
                    float barAreaX = rp.x + innerTtW - kPctReservedW - kBarGap - kBarMaxW;
                    float barY = rp.y + (kTtRowH - 5.f) * 0.5f;
                    float barFillW = kBarMaxW * tt.pct;
                    ttDl->AddRectFilled(ImVec2(barAreaX, barY),
                        ImVec2(barAreaX + kBarMaxW, barY + 5.f),
                        IM_COL32(255, 255, 255, 20), 2.f);
                    if (barFillW > 0.f)
                        ttDl->AddRectFilled(ImVec2(barAreaX, barY),
                            ImVec2(barAreaX + barFillW, barY + 5.f),
                            barCol, 2.f);

                    ImVec2 pSz = ImGui::CalcTextSize(pBuf);
                    float pScale = 12.f / ImGui::GetFontSize();
                    ttDl->AddText(nullptr, 12.f,
                        ImVec2(rp.x + innerTtW - pSz.x * pScale, nameY + 0.5f),
                        pctCol, pBuf);

                    ImGui::Dummy(ImVec2(innerTtW, kTtRowH + kTtRowGap));
                }

                if (needScroll)
                    ImGui::EndChild();
            }

            ImGui::EndTooltip();
            ImGui::PopStyleVar(4);
            ImGui::PopStyleColor(4);
        }

        int totalRows = std::max(1, (numSlots + kGridCols - 1) / kGridCols);
        float rowH = kSkIconSz + 3.f + kCountFontX + 6.f;
        skPos.y += totalRows * rowH + 4.f;
        ImGui::SetCursorScreenPos(ImVec2(skPos.x, skPos.y));
    }

    // ═══════════════════════════════════════════════════════════════
    // SECTION 3B — ACTIVE CAST BAR
    // ═══════════════════════════════════════════════════════════════
    if (!isDead)
    {
        auto sv = ard.skillVisualAtTime(m_debugTimeline);
        bool isNonInstant = false;
        if (sv.skillId > 0 && sv.alpha > 0.f)
        {
            // Find the matching event
            int lo = 0, hi = static_cast<int>(ard.skillUseHistory.size()) - 1, best = -1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                if (ard.skillUseHistory[mid].startTime <= m_debugTimeline) { best = mid; lo = mid + 1; }
                else hi = mid - 1;
            }
            if (best >= 0)
                isNonInstant = !ard.skillUseHistory[best].isInstant &&
                    (ard.skillUseHistory[best].endTime - ard.skillUseHistory[best].startTime > 0.001f);
        }

        if (sv.skillId > 0 && sv.alpha > 0.f && isNonInstant)
        {
            float winX = ImGui::GetWindowPos().x;
            constexpr float kCbPadT = 6.f, kCbPadB = 8.f;
            constexpr float kCbIconSz = 32.f;
            constexpr float kCbBarH = 10.f;
            constexpr float kCbGap = 8.f;

            ImVec2 cbPos = ImVec2(winX + kPadX, ImGui::GetCursorScreenPos().y + kCbPadT);
            ImU8 icoAlpha = (ImU8)(sv.alpha * 255.f);

            ImTextureID cbSkillTex = LoadSkillIcon(this, dev, sv.skillId,
                m_skillIconIndex, m_skillIconCache);
            if (cbSkillTex)
            {
                dl->AddImageRounded(cbSkillTex, ImVec2(cbPos.x, cbPos.y),
                    ImVec2(cbPos.x + kCbIconSz, cbPos.y + kCbIconSz),
                    ImVec2(0, 0), ImVec2(1, 1),
                    IM_COL32(255, 255, 255, icoAlpha), 6.f);
            }

            float barLeft = cbPos.x + kCbIconSz + kCbGap;
            float barRight = winX + contentW - kPadX;
            float barW = barRight - barLeft;
            float barY = cbPos.y + (kCbIconSz - kCbBarH) * 0.5f;
            ImVec2 barMin(barLeft, barY);
            ImVec2 barMax(barRight, barY + kCbBarH);
            float midY = barY + kCbBarH * 0.5f;
            ImU8 barAlpha = (ImU8)(sv.alpha * 255.f);

            // Background
            {
                ImU32 bgD = IM_COL32(0, 0, 0, barAlpha);
                ImU32 bgM = IM_COL32(36, 36, 36, barAlpha);
                dl->AddRectFilledMultiColor(barMin, ImVec2(barMax.x, midY), bgD, bgD, bgM, bgM);
                dl->AddRectFilledMultiColor(ImVec2(barMin.x, midY), barMax, bgM, bgM, bgD, bgD);
            }

            // Green = success/casting, Yellow = cancelled, Purple = interrupted
            const auto& db = m_skillView;
            const SkillInfo* castSi = db.IsLoaded() ? db.Get(sv.skillId) : nullptr;

            static const GradStop sGreenH[] = {
                { 0.000f,  10, 10, 10 }, { 0.200f,  26, 58, 10 },
                { 0.400f,  64,176, 32 }, { 0.600f, 168,240, 80 },
                { 0.800f, 200,255,112 }, { 1.000f, 144,224, 64 }
            };
            static const GradStop sYellowH[] = {
                { 0.000f,  10,  8,  0 }, { 0.143f,  58, 30,  0 },
                { 0.286f, 122, 58,  0 }, { 0.429f, 192, 96,  0 },
                { 0.571f, 232,144, 16 }, { 0.714f, 255,184, 32 },
                { 0.857f, 255,208, 64 }, { 1.000f, 232,160, 16 }
            };
            static const GradStop sPurpleH[] = {
                { 0.000f,  10, 10, 10 }, { 0.300f, 120, 32,192 },
                { 0.600f, 224,160,255 }, { 1.000f, 160, 80,224 }
            };

            const GradStop* hStops;
            int nStops;
            ImU32 glowCol;
            if (sv.interrupted) {
                hStops = sPurpleH; nStops = 4;
                glowCol = IM_COL32(128, 48, 192, (ImU8)(0.3f * barAlpha));
            } else if (sv.cancelled) {
                hStops = sYellowH; nStops = 8;
                glowCol = IM_COL32(192, 120, 0, (ImU8)(0.3f * barAlpha));
            } else {
                hStops = sGreenH; nStops = 6;
                glowCol = IM_COL32(96, 208, 32, (ImU8)(0.3f * barAlpha));
            }

            float pct = sv.progress;
            float fillW = barW * pct;
            if (pct > 0.005f)
            {
                int nSegs = std::clamp((int)(fillW / 3.f), 4, 24);
                float topV = (sv.cancelled || sv.interrupted) ? 0.58f : 0.55f;
                float botV = (sv.cancelled || sv.interrupted) ? 0.52f : 0.50f;
                for (int si2 = 0; si2 < nSegs; ++si2)
                {
                    float u0 = (float)si2 / nSegs;
                    float u1 = (float)(si2 + 1) / nSegs;
                    float r0, g0, b0, r1, g1, b1;
                    SampleGradient(hStops, nStops, u0 * pct, r0, g0, b0);
                    SampleGradient(hStops, nStops, u1 * pct, r1, g1, b1);

                    float x0 = barMin.x + fillW * u0;
                    float x1 = barMin.x + fillW * u1;

                    auto vig = [&](float r, float g, float b, float d) -> ImU32 {
                        float m = 1.f - d;
                        return IM_COL32((ImU8)(r * m), (ImU8)(g * m), (ImU8)(b * m), barAlpha);
                    };
                    ImU32 tl = vig(r0,g0,b0, topV);
                    ImU32 tr = vig(r1,g1,b1, topV);
                    ImU32 ml = IM_COL32((ImU8)r0,(ImU8)g0,(ImU8)b0, barAlpha);
                    ImU32 mr = IM_COL32((ImU8)r1,(ImU8)g1,(ImU8)b1, barAlpha);
                    ImU32 bl = vig(r0,g0,b0, botV);
                    ImU32 br = vig(r1,g1,b1, botV);

                    dl->AddRectFilledMultiColor(
                        ImVec2(x0, barMin.y), ImVec2(x1, midY), tl, tr, mr, ml);
                    dl->AddRectFilledMultiColor(
                        ImVec2(x0, midY), ImVec2(x1, barMax.y), ml, mr, br, bl);
                }

                // Leading-edge glow
                float fillX = barMin.x + fillW;
                float gw = 6.f;
                dl->AddRectFilled(
                    ImVec2(fillX - gw * 0.5f, barMin.y),
                    ImVec2(fillX + gw * 0.5f, barMax.y), glowCol);
                ImU32 glowOuter = (glowCol & 0x00FFFFFF) | ((ImU32)((barAlpha * 0.15f)) << 24);
                dl->AddRectFilled(
                    ImVec2(fillX - gw, barMin.y - 1.f),
                    ImVec2(fillX + gw, barMax.y + 1.f), glowOuter);
            }

            // Skill name inside bar (if wide enough)
            constexpr float kCbNameFontSz = 12.f;
            if (castSi && fillW > 60.f)
            {
                const char* sName = castSi->name.c_str();
                ImVec2 nameSz = ImGui::CalcTextSize(sName);
                float nameScale = kCbNameFontSz / ImGui::GetFontSize();
                float nameW = nameSz.x * nameScale;
                if (nameW < fillW - 4.f)
                {
                    float nx = barMin.x + (fillW - nameW) * 0.5f;
                    float ny = barY + (kCbBarH - kCbNameFontSz) * 0.5f;
                    dl->AddText(nullptr, kCbNameFontSz, ImVec2(nx + 1, ny + 1),
                        IM_COL32(0, 0, 0, (ImU8)(0.50f * barAlpha)), sName);
                    dl->AddText(nullptr, kCbNameFontSz, ImVec2(nx, ny),
                        IM_COL32(255, 255, 255, (ImU8)(0.85f * barAlpha)), sName);
                }
            }

            // Cast time remaining (above bar, flush right)
            if (sv.isCasting)
            {
                int bIdx = -1;
                {
                    int lo2 = 0, hi2 = static_cast<int>(ard.skillUseHistory.size()) - 1;
                    while (lo2 <= hi2) {
                        int mid2 = lo2 + (hi2 - lo2) / 2;
                        if (ard.skillUseHistory[mid2].startTime <= m_debugTimeline) { bIdx = mid2; lo2 = mid2 + 1; }
                        else hi2 = mid2 - 1;
                    }
                }
                if (bIdx >= 0)
                {
                    const auto& ev = ard.skillUseHistory[bIdx];
                    float remaining = ev.endTime - m_debugTimeline;
                    if (remaining < 0.f) remaining = 0.f;
                    char timeBuf[16];
                    snprintf(timeBuf, sizeof(timeBuf), "%.1fs", remaining);
                    ImVec2 tSz = ImGui::CalcTextSize(timeBuf);
                    float tScale = 10.f / ImGui::GetFontSize();
                    dl->AddText(nullptr, 10.f,
                        ImVec2(barMax.x - tSz.x * tScale, barMin.y - 12.f),
                        IM_COL32(255, 255, 255, (ImU8)(0.60f * barAlpha)), timeBuf);
                }
            }

            float totalH = kCbPadT + kCbIconSz + kCbPadB;
            ImGui::SetCursorScreenPos(ImVec2(cbPos.x, cbPos.y - kCbPadT + totalH));
        }
    }

    // ═══════════════════════════════════════════════════════════════
    // SECTION 4 — WEAPON SETS
    // ═══════════════════════════════════════════════════════════════
    if (m_pipWeaponSets.built && !m_pipWeaponSets.sets.empty())
    {
        float winX = ImGui::GetWindowPos().x;
        ImVec2 wPos = ImVec2(winX + kPadX, ImGui::GetCursorScreenPos().y);

        dl->AddLine(ImVec2(wPos.x, wPos.y), ImVec2(wPos.x + contentW - 2 * kPadX, wPos.y), kDivider);
        wPos.y += 8.f;

        dl->AddText(nullptr, 10.f, wPos, kGold, "WEAPON SET");
        wPos.y += 16.f;

        int activeIdx = -1;
        if (snap)
        {
            for (int i = 0; i < (int)m_pipWeaponSets.sets.size(); i++)
            {
                auto& ws = m_pipWeaponSets.sets[i];
                if (ws.mainId == snap->weapon_item_id && ws.offId == snap->offhand_item_id)
                { activeIdx = i; break; }
            }
        }

        float kWsBtnSz = pipSkillIconSz;
        constexpr float kWsGap = 5.f, kWsPad = 5.f;
        for (int i = 0; i < (int)m_pipWeaponSets.sets.size(); i++)
        {
            auto& ws = m_pipWeaponSets.sets[i];
            bool active = (i == activeIdx);

            ImVec2 btnTL(wPos.x, wPos.y);
            ImVec2 btnBR(btnTL.x + kWsBtnSz, btnTL.y + kWsBtnSz);

            bool isBundle = (ws.bundleType != BundleType::Unknown);
            if (isBundle)
            {
                // Bundle (flag / repair kit / vine seed): dashed border style
                ImU32 borderCol = active ? IM_COL32(212, 160, 32, 200) : IM_COL32(255, 255, 255, 50);
                dl->AddRectFilled(btnTL, btnBR, IM_COL32(255, 255, 255, 6), 6.f);
                float dashLen = 4.f, gapLen = 3.f;
                auto drawDashed = [&](ImVec2 a, ImVec2 b) {
                    float dx = b.x - a.x, dy = b.y - a.y;
                    float len = sqrtf(dx * dx + dy * dy);
                    if (len < 1.f) return;
                    float ux = dx / len, uy = dy / len;
                    float t = 0.f;
                    while (t < len) {
                        float e = std::min(t + dashLen, len);
                        dl->AddLine(ImVec2(a.x + ux * t, a.y + uy * t),
                                    ImVec2(a.x + ux * e, a.y + uy * e), borderCol, 1.f);
                        t = e + gapLen;
                    }
                };
                drawDashed(btnTL, ImVec2(btnBR.x, btnTL.y));
                drawDashed(ImVec2(btnBR.x, btnTL.y), btnBR);
                drawDashed(btnBR, ImVec2(btnTL.x, btnBR.y));
                drawDashed(ImVec2(btnTL.x, btnBR.y), btnTL);
            }
            else if (active)
            {
                dl->AddRectFilled(btnTL, btnBR, IM_COL32(212, 160, 32, 38), 6.f);
                dl->AddRect(btnTL, btnBR, IM_COL32(212, 160, 32, 255), 6.f, 0, 1.5f);
            }
            else
            {
                dl->AddRectFilled(btnTL, btnBR, IM_COL32(255, 255, 255, 10), 6.f);
                dl->AddRect(btnTL, btnBR, IM_COL32(255, 255, 255, 31), 6.f);
            }

            WeaponTextureResult wtr = ResolveWeaponTextures(ws.weapCat, ws.mainType, ard.primaryProf, ard.teamId, ws.bundleType);
            ImTextureID mainTex = nullptr;
            if (wtr.mainTex) {
                if (wtr.isNPCIcon)
                    mainTex = LoadNPCIcon(dev, wtr.mainTex);
                else if (wtr.isFlag)
                    mainTex = LoadFlagIcon(dev, wtr.mainTex);
                else
                    mainTex = LoadWeaponTexture(dev, wtr.mainTex);
            }
            ImTextureID offTex = wtr.offTex ? LoadWeaponTexture(dev, wtr.offTex) : nullptr;

            float iconArea = kWsBtnSz - 2 * kWsPad;
            if (mainTex && offTex)
            {
                float offSz  = iconArea - 4.f;
                float mainSz = iconArea;
                // Offhand (back layer, offset +4px)
                ImVec2 oTL(btnTL.x + kWsPad + 4.f, btnTL.y + kWsPad + 4.f);
                ImVec2 oBR(oTL.x + offSz, oTL.y + offSz);
                dl->AddImage(offTex, oTL, oBR, ImVec2(0, 0), ImVec2(1, 1),
                    IM_COL32(255, 255, 255, 217));
                // Main hand (front layer, top-left offset)
                ImVec2 mTL(btnTL.x + kWsPad - 2.f, btnTL.y + kWsPad - 2.f);
                ImVec2 mBR(mTL.x + mainSz, mTL.y + mainSz);
                dl->AddImage(mainTex, mTL, mBR);
            }
            else if (mainTex)
            {
                float sz = iconArea;
                float cx = btnTL.x + (kWsBtnSz - sz) * 0.5f;
                float cy = btnTL.y + (kWsBtnSz - sz) * 0.5f;
                dl->AddImage(mainTex, ImVec2(cx, cy), ImVec2(cx + sz, cy + sz));
            }
            else
            {
                // Empty placeholder
                dl->AddRect(ImVec2(btnTL.x + 8, btnTL.y + 8),
                    ImVec2(btnBR.x - 8, btnBR.y - 8),
                    IM_COL32(255, 255, 255, 38), 4.f, 0, 1.f);
                const char* q = "?";
                ImVec2 qSz = ImGui::CalcTextSize(q);
                float qScale = 12.f / ImGui::GetFontSize();
                dl->AddText(nullptr, 12.f,
                    ImVec2(btnTL.x + (kWsBtnSz - qSz.x * qScale) * 0.5f,
                           btnTL.y + (kWsBtnSz - qSz.y * qScale) * 0.5f),
                    IM_COL32(0x50, 0x5a, 0x64, 0xFF), q);
            }

            // Set label below icon area: #1, #2, #3...
            char setLabel[8];
            snprintf(setLabel, sizeof(setLabel), "#%d", i + 1);
            ImVec2 ls = ImGui::CalcTextSize(setLabel);
            float lScale = 10.f / ImGui::GetFontSize();
            dl->AddText(nullptr, 10.f,
                ImVec2(btnTL.x + (kWsBtnSz - ls.x * lScale) * 0.5f, btnBR.y - 12.f),
                IM_COL32(0x70, 0x7d, 0x88, 0xFF), setLabel);

            wPos.x += kWsBtnSz + kWsGap;
        }

        wPos.y += kWsBtnSz + 8.f;
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + kPadX, wPos.y));
    }

    // (Section 5 — Skill Cast Stats removed, replaced by skill icon grid + tooltip)

    // ═══════════════════════════════════════════════════════════════
    // SECTION 6 — CENTERED PLAYHEAD CAST TIMELINE (±10s)
    // ═══════════════════════════════════════════════════════════════
    {
        float winX = ImGui::GetWindowPos().x;
        ImVec2 tPos = ImVec2(winX + kPadX, ImGui::GetCursorScreenPos().y);
        float innerW = contentW - 2 * kPadX;

        dl->AddLine(ImVec2(tPos.x, tPos.y), ImVec2(tPos.x + innerW, tPos.y), kDivider);
        tPos.y += 8.f;

        dl->AddText(nullptr, 10.f, tPos, kGold, "CAST TIMELINE");
        tPos.y += 16.f;

        constexpr float kTlIconSz     = 24.f;
        constexpr float kTlIconCastSz = 28.f;
        constexpr float kTlIconGap    = 3.f;
        constexpr float kTlBarH       = 12.f;
        constexpr float kTimeLabelH   = 16.f;
        constexpr float kFadeW        = 16.f;
        constexpr float kHalfWindow   = 10.f;
        constexpr float kFullWindow   = 20.f;
        constexpr int   kFutureAlpha  = 153;

        float tlW    = innerW;
        float tlH    = kTlIconCastSz + kTlIconGap + kTlBarH;
        float barTopY = tPos.y + kTlIconCastSz + kTlIconGap;

        ImVec2 tlTL(tPos.x, tPos.y);

        // Bar track background
        dl->AddRectFilled(ImVec2(tPos.x, barTopY),
            ImVec2(tPos.x + tlW, barTopY + kTlBarH),
            IM_COL32(0, 0, 0, 77), 4.f);

        float curTime     = m_debugTimeline;
        float windowStart = curTime - kHalfWindow;
        float windowEnd   = curTime + kHalfWindow;
        float centerX     = tPos.x + tlW * 0.5f;

        auto timeToX = [&](float t) -> float {
            return tPos.x + ((t - windowStart) / kFullWindow) * tlW;
        };

        // Collect visible timeline entries (cast bars + instant skill icons)
        struct TlBar {
            float x0 = 0.f, x1 = 0.f;
            float rawStart = 0.f, rawEnd = 0.f;
            int skillId = 0;
            ImU32 barCol = 0;
            bool isActive = false;
            float castProgress = 0.f;
            bool isInstant = false;
            bool wasInterrupted = false;
            bool wasCancelled = false;
        };
        std::vector<TlBar> tlBars;

        auto agentIt2 = m_replayCtx.agents.find(m_playerInfoAgentId);
        if (agentIt2 != m_replayCtx.agents.end())
        {
            const auto& skillHist = agentIt2->second.skillUseHistory;
            const auto& db = m_skillView;
            for (const auto& ev : skillHist)
            {
                if (ev.endTime < windowStart) continue;
                if (ev.startTime > windowEnd) break;

                if (ev.isInstant)
                {
                    float ix = timeToX(ev.startTime);
                    if (ix < tPos.x || ix > tPos.x + tlW) continue;
                    tlBars.push_back({ ix, ix, ev.startTime, ev.endTime, ev.skillId,
                        0, false, 1.f, true, ev.wasInterrupted, ev.wasCancelled });
                    continue;
                }

                float x0raw = timeToX(ev.startTime);
                float x1raw = timeToX(ev.endTime);
                float x0 = std::max(x0raw, tPos.x);
                float x1 = std::min(x1raw, tPos.x + tlW);
                if (x1 <= x0) continue;

                ImU32 barCol = IM_COL32(0xD4, 0xA0, 0x20, 200);
                if (db.IsLoaded())
                {
                    const SkillInfo* si = db.Get(ev.skillId);
                    if (si)
                    {
                        int type = si->type;
                        if (type == 1 || type == 2 || type == 10 || type == 14 || type == 24)
                            barCol = IM_COL32(0xC8, 0xA8, 0x20, 200);
                        else if (type == 6 || type == 22)
                            barCol = IM_COL32(0x40, 0xA0, 0x40, 200);
                        else if (type == 4 || type == 5)
                            barCol = IM_COL32(0x80, 0x40, 0xC0, 200);
                    }
                }

                if (ev.wasInterrupted)
                    barCol = IM_COL32(0x90, 0x30, 0xD0, 200);
                else if (ev.wasCancelled)
                    barCol = IM_COL32(0xE0, 0x80, 0x20, 200);

                bool isActive = (ev.startTime <= curTime && ev.endTime > curTime);
                float castDur = ev.endTime - ev.startTime;
                float progress = (castDur > 0.f) ? std::clamp((curTime - ev.startTime) / castDur, 0.f, 1.f) : 1.f;

                if (isActive)
                {
                    float fillX = timeToX(curTime);
                    fillX = std::clamp(fillX, x0, x1);
                    if (fillX > x0)
                        dl->AddRectFilled(ImVec2(x0, barTopY), ImVec2(fillX, barTopY + kTlBarH), barCol, 3.f);
                    if (fillX < x1)
                    {
                        ImU32 dimCol = (barCol & 0x00FFFFFF) | (((ImU32)80) << 24);
                        dl->AddRectFilled(ImVec2(fillX, barTopY), ImVec2(x1, barTopY + kTlBarH), dimCol, 3.f);
                    }
                }
                else
                {
                    bool isFuture = (ev.startTime > curTime);
                    ImU32 drawCol = barCol;
                    if (isFuture)
                        drawCol = (barCol & 0x00FFFFFF) | (((ImU32)kFutureAlpha) << 24);
                    dl->AddRectFilled(ImVec2(x0, barTopY), ImVec2(x1, barTopY + kTlBarH), drawCol, 3.f);
                }

                tlBars.push_back({ x0, x1, ev.startTime, ev.endTime, ev.skillId, barCol,
                    isActive, progress, false, ev.wasInterrupted, ev.wasCancelled });

                if (ImGui::IsMouseHoveringRect(ImVec2(x0, barTopY), ImVec2(x1, barTopY + kTlBarH)))
                {
                    std::string sName = GetSkillDisplayName(ev.skillId);
                    int mins = (int)(ev.startTime / 60.f);
                    float secs = ev.startTime - mins * 60.f;
                    ImGui::SetTooltip("%s -- %d:%05.2f", sName.c_str(), mins, secs);
                }
            }
        }

        // Skill icons above/below bars with overlap alternation
        EnsureSkillIconIndex();
        bool lastBelow = false;
        float lastIconRight = -1000.f;
        for (size_t bi = 0; bi < tlBars.size(); bi++)
        {
            const auto& tb = tlBars[bi];
            bool isFuture = (tb.rawStart > curTime);
            float icoSz = tb.isActive ? kTlIconCastSz : kTlIconSz;
            ImU32 icoTint = isFuture ? IM_COL32(255, 255, 255, kFutureAlpha) : IM_COL32(255, 255, 255, 255);

            float icoX;
            if (tb.isInstant)
            {
                icoX = tb.x0 - icoSz * 0.5f;
            }
            else if (tb.isActive)
            {
                float fillEdge = tb.x0 + (tb.x1 - tb.x0) * tb.castProgress;
                icoX = fillEdge - icoSz * 0.5f;
            }
            else if (isFuture)
                icoX = tb.x0 - icoSz * 0.5f;
            else
                icoX = tb.x1 - icoSz * 0.5f;

            icoX = std::clamp(icoX, tPos.x, tPos.x + tlW - icoSz);

            bool placeBelow = false;
            if (icoX < lastIconRight + 2.f)
                placeBelow = !lastBelow;

            float icoY = placeBelow
                ? barTopY + kTlBarH + kTlIconGap
                : barTopY - kTlIconGap - icoSz;

            ImTextureID skTex = LoadSkillIcon(this, dev, tb.skillId,
                m_skillIconIndex, m_skillIconCache);
            if (skTex)
            {
                dl->AddImageRounded(skTex,
                    ImVec2(icoX, icoY), ImVec2(icoX + icoSz, icoY + icoSz),
                    ImVec2(0, 0), ImVec2(1, 1), icoTint, 3.f);
            }

            // Border color: green=active cast, purple=interrupted, orange=cancelled
            if (tb.wasInterrupted)
            {
                dl->AddRect(ImVec2(icoX - 0.5f, icoY - 0.5f),
                    ImVec2(icoX + icoSz + 0.5f, icoY + icoSz + 0.5f),
                    IM_COL32(0xA0, 0x30, 0xE0, 0xFF), 3.f, 0, 1.5f);
            }
            else if (tb.wasCancelled)
            {
                dl->AddRect(ImVec2(icoX - 0.5f, icoY - 0.5f),
                    ImVec2(icoX + icoSz + 0.5f, icoY + icoSz + 0.5f),
                    IM_COL32(0xF0, 0x80, 0x20, 0xFF), 3.f, 0, 1.5f);
            }
            else if (tb.isActive)
            {
                dl->AddRect(ImVec2(icoX - 0.5f, icoY - 0.5f),
                    ImVec2(icoX + icoSz + 0.5f, icoY + icoSz + 0.5f),
                    IM_COL32(0x30, 0xC0, 0x30, 0xFF), 3.f, 0, 1.5f);
            }

            if (tb.isInstant && ImGui::IsMouseHoveringRect(ImVec2(icoX, icoY), ImVec2(icoX + icoSz, icoY + icoSz)))
            {
                std::string sName = GetSkillDisplayName(tb.skillId);
                int mins = (int)(tb.rawStart / 60.f);
                float secs = tb.rawStart - mins * 60.f;
                ImGui::SetTooltip("%s -- %d:%05.2f", sName.c_str(), mins, secs);
            }

            lastBelow = placeBelow;
            lastIconRight = icoX + icoSz;
        }

        // Center playhead line
        dl->AddLine(ImVec2(centerX, tPos.y), ImVec2(centerX, barTopY + kTlBarH),
            IM_COL32(255, 255, 255, 179), 1.f);

        // Edge fade overlays (16px gradient at each end)
        {
            float fadeL = tPos.x;
            float fadeR = tPos.x + tlW;
            float topY  = tPos.y;
            float botY  = barTopY + kTlBarH;
            ImU32 opaque = IM_COL32(13, 13, 18, 255);
            ImU32 clear  = IM_COL32(13, 13, 18, 0);
            // Left fade
            dl->AddRectFilledMultiColor(
                ImVec2(fadeL, topY), ImVec2(fadeL + kFadeW, botY),
                opaque, clear, clear, opaque);
            // Right fade
            dl->AddRectFilledMultiColor(
                ImVec2(fadeR - kFadeW, topY), ImVec2(fadeR, botY),
                clear, opaque, opaque, clear);
        }

        // Time labels below bar
        {
            float labelY = barTopY + kTlBarH + 3.f;
            constexpr float kLblFontSz = 11.f;
            ImU32 lblCol = IM_COL32(0x60, 0x6a, 0x74, 0xFF);

            struct TLabel { float xRatio; const char* text; };
            TLabel labels[] = {
                { 0.00f, "-10s" },
                { 0.25f,  "-5s" },
                { 0.50f,  "now" },
                { 0.75f,  "+5s" },
                { 1.00f, "+10s" },
            };
            for (auto& lb : labels)
            {
                float lx = tPos.x + tlW * lb.xRatio;
                ImVec2 sz = ImGui::CalcTextSize(lb.text);
                float scale = kLblFontSz / ImGui::GetFontSize();
                float tw = sz.x * scale;
                ImU32 col = (lb.xRatio == 0.50f) ? IM_COL32(255, 255, 255, 140) : lblCol;
                dl->AddText(nullptr, kLblFontSz, ImVec2(lx - tw * 0.5f, labelY), col, lb.text);
            }
        }

        // Interaction: click to scrub timeline
        float totalH = tlH + kTimeLabelH;
        ImGui::SetCursorScreenPos(tlTL);
        ImGui::InvisibleButton("##pip_timeline", ImVec2(tlW, totalH));
        if (ImGui::IsItemClicked())
        {
            float mouseX = ImGui::GetIO().MousePos.x;
            float ratio = std::clamp((mouseX - tPos.x) / tlW, 0.f, 1.f);
            m_debugTimeline = windowStart + ratio * kFullWindow;
        }

        ImGui::SetCursorScreenPos(ImVec2(tPos.x, tPos.y + totalH + 8.f));
    }

    // TODO: Current target + nearest ally/enemy (Section 7, deferred)

    ImGui::Dummy(ImVec2(0, 4.f));

    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(6);
}
