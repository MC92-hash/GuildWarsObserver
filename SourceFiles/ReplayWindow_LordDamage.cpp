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
// DrawLordDamagePanel — guild lord damage stats floating panel
// ---------------------------------------------------------------------------
void ReplayWindow::DrawLordDamagePanel()
{
    if (!m_showLordDamagePanel) m_lordDmgHighlightAgent = -1;
    if (!m_showLordDamagePanel || !m_lordDamageBuilt) return;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImFont* font = ImGui::GetFont();
    float maxT = std::max(1.f, m_replayCtx.maxReplayTime);

    if (std::isnan(m_debugTimeline) || std::isinf(m_debugTimeline))
        m_debugTimeline = 0.f;
    float curTime = std::clamp(m_debugTimeline, 0.f, maxT);

    // Party-style gradient definitions (matching DrawPartyHealthBar)
    struct LordGrad5 { ImU32 c[5]; };
    static constexpr LordGrad5 gAliveBlue = {{ IM_COL32(0x4A,0x6B,0xA3,0xFF), IM_COL32(0x3D,0x5F,0x98,0xFF), IM_COL32(0x30,0x5A,0x90,0xFF), IM_COL32(0x29,0x4E,0x7A,0xFF), IM_COL32(0x22,0x42,0x65,0xFF) }};
    static constexpr LordGrad5 gDeadBlue  = {{ IM_COL32(0x3A,0x4A,0x66,0xFF), IM_COL32(0x33,0x42,0x59,0xFF), IM_COL32(0x2C,0x3A,0x4D,0xFF), IM_COL32(0x25,0x32,0x40,0xFF), IM_COL32(0x1E,0x29,0x33,0xFF) }};
    static constexpr LordGrad5 gAliveRed  = {{ IM_COL32(0xCE,0x0C,0x0C,0xFF), IM_COL32(0xD6,0x34,0x34,0xFF), IM_COL32(0xD9,0x43,0x43,0xFF), IM_COL32(0xB2,0x00,0x00,0xFF), IM_COL32(0x7E,0x00,0x00,0xFF) }};
    static constexpr LordGrad5 gDeadRed   = {{ IM_COL32(0x47,0x1C,0x17,0xFF), IM_COL32(0x53,0x24,0x1B,0xFF), IM_COL32(0x52,0x24,0x1C,0xFF), IM_COL32(0x3C,0x19,0x14,0xFF), IM_COL32(0x2F,0x13,0x0F,0xFF) }};
    static constexpr LordGrad5 gDegenHex  = {{ IM_COL32(0xC9,0x47,0x9E,0xFF), IM_COL32(0xCE,0x5A,0xA8,0xFF), IM_COL32(0xD3,0x6C,0xB1,0xFF), IM_COL32(0xB5,0x33,0x8A,0xFF), IM_COL32(0x86,0x26,0x66,0xFF) }};
    static constexpr LordGrad5 gPoison    = {{ IM_COL32(0x7F,0x7F,0x3F,0xFF), IM_COL32(0x8B,0x8B,0x50,0xFF), IM_COL32(0x94,0x94,0x5F,0xFF), IM_COL32(0x66,0x66,0x2B,0xFF), IM_COL32(0x4E,0x4E,0x1D,0xFF) }};
    static constexpr LordGrad5 gBleeding  = {{ IM_COL32(0xDF,0x71,0x70,0xFF), IM_COL32(0xE1,0x7B,0x7B,0xFF), IM_COL32(0xE2,0x7E,0x7E,0xFF), IM_COL32(0xBC,0x59,0x59,0xFF), IM_COL32(0xA9,0x4F,0x50,0xFF) }};
    static constexpr LordGrad5 gDeepWound = {{ IM_COL32(0x92,0x92,0x92,0xFF), IM_COL32(0x9F,0x9F,0x9F,0xFF), IM_COL32(0xAB,0xAB,0xAB,0xFF), IM_COL32(0x84,0x84,0x84,0xFF), IM_COL32(0x73,0x73,0x73,0xFF) }};

    auto DrawLordGradRect = [](ImDrawList* dl, ImVec2 tl, ImVec2 br, const LordGrad5& g) {
        float w = br.x - tl.x;
        float h = br.y - tl.y;
        if (w < 1.f || h < 1.f) return;
        float bandH = h / 5.f;
        for (int i = 0; i < 5; ++i) {
            float y0 = tl.y + i * bandH;
            float y1 = (i == 4) ? br.y : y0 + bandH;
            ImU32 cTop = g.c[i];
            ImU32 cBot = (i < 4) ? g.c[i + 1] : g.c[i];
            dl->AddRectFilledMultiColor(ImVec2(tl.x, y0), ImVec2(br.x, y1), cTop, cTop, cBot, cBot);
        }
    };

    // Load status icons every frame through the cached loaders
    // (the loaders themselves cache by device+filename, so this is cheap)
    // Do NOT store raw pointers in statics — they can dangle if the
    // internal cache is invalidated on device change.
    ImTextureID sIconWeaponSpell = LoadEffectIcon(dev, "WeaponSpell.png");
    ImTextureID sIconEnchanted   = LoadPartyIcon(dev, "Enchanted.png");
    ImTextureID sIconCondition   = LoadPartyIcon(dev, "Condition.png");
    ImTextureID sIconHexed       = LoadPartyIcon(dev, "Hexed.png");

    ImGui::PushStyleColor(ImGuiCol_WindowBg,      ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,     ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));

    ImGui::SetNextWindowSizeConstraints(ImVec2(600, 100), ImVec2(900, FLT_MAX));
    m_panelLayout.ApplyPosition("lord_damage");

    if (!ImGui::Begin("Aggressiveness##lord_dmg", &m_showLordDamagePanel,
            ImGuiWindowFlags_AlwaysAutoResize))
    {
        m_panelLayout.TrackWindow("lord_damage");
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);
        return;
    }

    m_panelLayout.TrackWindow("lord_damage");

    // Viewport clamping
    {
        ImVec2 vpSz = ImGui::GetMainViewport()->Size;
        ImVec2 wPos = ImGui::GetWindowPos();
        ImVec2 wSz  = ImGui::GetWindowSize();
        wPos.x = std::clamp(wPos.x, 0.f, std::max(0.f, vpSz.x - wSz.x));
        wPos.y = std::clamp(wPos.y, 0.f, std::max(0.f, vpSz.y - wSz.y));
        ImGui::SetWindowPos(wPos);
    }

    const ImU32 cLabel   = IM_COL32(0x70, 0x7d, 0x88, 255);
    const ImU32 cWhite   = IM_COL32(0xe2, 0xe3, 0xe4, 255);
    const ImU32 cBlue    = IM_COL32(74, 144, 216, 255);
    const ImU32 cRed     = IM_COL32(208, 72, 72, 255);
    const ImU32 cGreen   = IM_COL32(80, 200, 80, 255);
    const ImU32 cAmber   = IM_COL32(220, 180, 40, 255);
    const ImU32 cDanger  = IM_COL32(220, 60, 50, 255);

    auto hpColor = [&](float pct) -> ImU32 {
        if (pct > 0.6f) return cGreen;
        if (pct > 0.3f) return cAmber;
        return cDanger;
    };

    auto formatDmg = [](int v) -> std::string {
        if (v < 1000) return std::to_string(v);
        std::string s = std::to_string(v);
        int insertPos = (int)s.length() - 3;
        while (insertPos > 0) { s.insert(insertPos, ","); insertPos -= 3; }
        return s;
    };

    float colW = std::max(50.f, (ImGui::GetContentRegionAvail().x - 10.f) * 0.5f);

    // ---- PART 1: Lord header (icon, name, HP bar, stats) per column ----
    auto DrawLordHeader = [&](int idx, float startX) {
        auto& ld = m_lordDmg[idx];
        bool isRed = Team::IndexIsRed(idx);
        const char* teamLabel = isRed ? "Red" : "Blue";
        const char* iconFile = isRed ? "redguildlord.png" : "blueguildlord.png";
        const std::string& guildHeader = isRed ? m_team2GuildHeader : m_team1GuildHeader;

        ImGui::SetCursorPosX(startX);
        float cursorY = ImGui::GetCursorPosY();

        ImTextureID lordTex = LoadFlagIcon(dev, iconFile);
        if (lordTex)
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddImage(lordTex,
                ImVec2(p.x, p.y), ImVec2(p.x + 32, p.y + 32));
        }

        const ImU32 cGuildHeader = IM_COL32(245, 227, 181, 255);
        float textX = startX + 36.f;
        ImGui::SetCursorPos(ImVec2(textX, cursorY + 1.f));
        ImGui::GetWindowDrawList()->AddText(font, font->FontSize,
            ImGui::GetCursorScreenPos(), cGuildHeader,
            guildHeader.empty() ? teamLabel : guildHeader.c_str());
        ImGui::SetCursorPos(ImVec2(textX, cursorY + 17.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        ImGui::TextUnformatted(std::format("{} Guild Lord", teamLabel).c_str());
        // His team's morale boosts raise his maximum (about +1% each, up to ten).
        const int lordBoosts = std::min(10, TeamMoraleBoostsAt(idx + 1, curTime));
        if (lordBoosts > 0)
        {
            ImGui::SameLine(0.f, 8.f);
            ImGui::BeginGroup();
            if (ImTextureID moraleTex = LoadGameUITexture(dev, "Effectbar\\ui_effect_morale_boost.png"))
            {
                ImGui::Image(moraleTex, ImVec2(font->FontSize, font->FontSize));
                ImGui::SameLine(0.f, 3.f);
            }
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(cGreen), "+%d%%", lordBoosts);
            ImGui::EndGroup();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Morale boost: his team has earned %d boost%s.\nMaximum Health %d instead of 1680.",
                                  lordBoosts, lordBoosts == 1 ? "" : "s",
                                  LordAggressiveness::kLordHealthByBoosts[lordBoosts]);
        }
        ImGui::PopStyleColor();

        ImGui::SetCursorPosX(startX);
        ImGui::SetCursorPosY(cursorY + 36.f);

        if (ld.lordAgentId < 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.33f, 0.34f, 0.35f, 1.f));
            ImGui::TextUnformatted("No lord found");
            ImGui::PopStyleColor();
            return;
        }

        const AgentSnapshot* snap = nullptr;
        float hpPct = 1.f;
        bool isDead = false;
        uint8_t teamId = isRed ? Team::Red : Team::Blue;
        auto lordIt = m_replayCtx.agents.find(ld.lordAgentId);
        if (lordIt != m_replayCtx.agents.end())
        {
            snap = FindSnapshotAtTime(lordIt->second, curTime);
            if (snap) { hpPct = std::clamp(snap->health_pct, 0.f, 1.f); isDead = snap->is_dead; }
        }

        // Party-window-style HP bar
        {
            constexpr float barH = 20.f;
            float barW = std::max(4.f, colW);
            ImVec2 barTL = ImGui::GetCursorScreenPos();
            ImVec2 barBR(barTL.x + barW, barTL.y + barH);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            dl->AddRect(barTL, barBR, IM_COL32(0x4E, 0x4D, 0x48, 0xFF), 0.f, 0, 1.0f);
            ImVec2 innerTL(barTL.x + 1, barTL.y + 1);
            ImVec2 innerBR(barBR.x - 1, barBR.y - 1);
            float innerW = innerBR.x - innerTL.x;
            float innerH = innerBR.y - innerTL.y;

            const LordGrad5* fillGrad = nullptr;
            if (isDead)
                fillGrad = Team::IsRed(teamId) ? &gDeadRed : &gDeadBlue;
            else if (snap && snap->has_degen_hex)
                fillGrad = &gDegenHex;
            else if (snap && snap->has_poison)
                fillGrad = &gPoison;
            else if (snap && snap->has_bleeding)
                fillGrad = &gBleeding;
            else
                fillGrad = Team::IsRed(teamId) ? &gAliveRed : &gAliveBlue;

            if (isDead)
            {
                DrawLordGradRect(dl, innerTL, innerBR, *fillGrad);
            }
            else
            {
                const LordGrad5* deadGrad = Team::IsRed(teamId) ? &gDeadRed : &gDeadBlue;
                DrawLordGradRect(dl, innerTL, innerBR, *deadGrad);
                bool hasDeepWound = snap && snap->has_deep_wound && !isDead;
                float fillPct = hasDeepWound ? std::min(hpPct, 0.80f) : hpPct;
                if (fillPct > 0.f)
                    DrawLordGradRect(dl, innerTL, ImVec2(innerTL.x + innerW * fillPct, innerBR.y), *fillGrad);
                if (hasDeepWound)
                    DrawLordGradRect(dl, ImVec2(innerTL.x + innerW * 0.80f, innerTL.y), innerBR, gDeepWound);
            }

            // Current / maximum, the maximum moved by his team's morale boosts and Deep Wound.
            int lordMax = LordAggressiveness::kLordHealthByBoosts[std::min(10, TeamMoraleBoostsAt(idx + 1, curTime))];
            if (snap && snap->has_deep_wound) lordMax = (int)AgentReplayData::ApplyDeepWound((uint32_t)lordMax);
            char hpText[48];
            snprintf(hpText, sizeof(hpText), "%s / %s", formatDmg((int)std::lround(hpPct * lordMax)).c_str(),
                     formatDmg(lordMax).c_str());
            ImVec2 hpSz = ImGui::CalcTextSize(hpText);
            ImVec2 hpPos(innerTL.x + 4.f, innerTL.y + (innerH - hpSz.y) * 0.5f);
            dl->AddText(ImVec2(hpPos.x + 1, hpPos.y + 1), IM_COL32(0, 0, 0, 0xCC), hpText);
            dl->AddText(hpPos, isDead ? IM_COL32(0x80, 0x80, 0x80, 0xFF) : cWhite, hpText);

            if (snap && !isDead)
            {
                const float icoSz = std::min(innerH - 2.f, 16.f);
                float iconX = innerBR.x - 2.f;
                float iconY = innerTL.y + (innerH - icoSz) * 0.5f;
                if (snap->has_weapon_spell && sIconWeaponSpell)
                    { iconX -= icoSz; dl->AddImage(sIconWeaponSpell, ImVec2(iconX, iconY), ImVec2(iconX + icoSz, iconY + icoSz)); iconX -= 1.f; }
                if (snap->has_enchantment && sIconEnchanted)
                    { iconX -= icoSz; dl->AddImage(sIconEnchanted, ImVec2(iconX, iconY), ImVec2(iconX + icoSz, iconY + icoSz)); iconX -= 1.f; }
                if ((snap->has_condition || snap->has_deep_wound || snap->has_bleeding || snap->has_poison) && sIconCondition)
                    { iconX -= icoSz; dl->AddImage(sIconCondition, ImVec2(iconX, iconY), ImVec2(iconX + icoSz, iconY + icoSz)); iconX -= 1.f; }
                if (snap->has_hex && sIconHexed)
                    { iconX -= icoSz; dl->AddImage(sIconHexed, ImVec2(iconX, iconY), ImVec2(iconX + icoSz, iconY + icoSz)); }
            }
            ImGui::Dummy(ImVec2(barW, barH));
        }
        ImGui::SetCursorPosX(startX);

        // Compute live stats up to current timestamp
        int liveTotalDmg = 0;
        float liveLowHp = 1.f;
        for (const auto& h : ld.hits)
        {
            if (h.time > curTime) break;
            liveTotalDmg += h.rawDmg;
        }
        if (lordIt != m_replayCtx.agents.end())
        {
            for (const auto& s : lordIt->second.snapshots)
            {
                if (s.time > curTime) break;
                if (s.health_pct < liveLowHp && s.health_pct >= 0.f)
                    liveLowHp = s.health_pct;
            }
        }

        if (liveTotalDmg > 0)
        {
            // One line: the damage he has taken (party window's icon) and his lowest health so far.
            ImDrawList* hdl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float isz = font->FontSize;
            float x = p.x;
            if (ImTextureID taken = LoadFlagIcon(dev, "damagereceived.png"))
            {
                hdl->AddImage(taken, ImVec2(x, p.y), ImVec2(x + isz, p.y + isz));
                x += isz + 4.f;
            }
            const std::string takenText = formatDmg(liveTotalDmg);
            hdl->AddText(font, font->FontSize, ImVec2(x, p.y), cWhite, takenText.c_str());
            x += font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, takenText.c_str()).x + 14.f;
            hdl->AddText(font, font->FontSize, ImVec2(x, p.y), hpColor(liveLowHp),
                         std::format("low {:.0f}%", liveLowHp * 100.f).c_str());
            ImGui::Dummy(ImVec2(colW, font->FontSize + 1.f));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Damage this lord has taken so far (life steal not counted), and the lowest his health went.");
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.33f, 0.34f, 0.35f, 1.f));
            ImGui::TextUnformatted("Never attacked");
            ImGui::PopStyleColor();
        }
    };

    // ---- PART 2: Attacker breakdown per column ----
    auto DrawLordAttackers = [&](int idx, float startX) {
        auto& ld = m_lordDmg[idx];
        if (ld.lordAgentId < 0) return;

        int liveTotalDmg = 0;
        std::unordered_map<int, int> liveAttackerDmg;
        for (const auto& h : ld.hits)
        {
            if (h.time > curTime) break;
            liveTotalDmg += h.rawDmg;
            liveAttackerDmg[h.casterId] += h.rawDmg;
        }
        if (liveTotalDmg <= 0) return;

        ImGui::SetCursorPosX(startX);
        {
            float fs10 = 10.f;
            ImGui::GetWindowDrawList()->AddText(font, fs10,
                ImGui::GetCursorScreenPos(), cLabel, "ATTACKERS");
            ImGui::Dummy(ImVec2(colW, fs10 + 3.f));
        }

        std::vector<LordAttackerRow> liveRows;
        liveRows.reserve(liveAttackerDmg.size());
        for (auto& [aid, dmg] : liveAttackerDmg)
        {
            LordAttackerRow row;
            row.agentId = aid;
            row.totalDmg = dmg;
            row.pct = (liveTotalDmg > 0) ? (float)dmg / (float)liveTotalDmg : 0.f;
            auto ait = m_replayCtx.agents.find(aid);
            if (ait != m_replayCtx.agents.end())
            {
                row.name = ait->second.partyBarLabel.empty()
                    ? ait->second.playerName : ait->second.partyBarLabel;
                row.professionId = ait->second.primaryProf;
                row.teamId = ait->second.teamId;
            }
            liveRows.push_back(std::move(row));
        }
        std::sort(liveRows.begin(), liveRows.end(),
            [](const LordAttackerRow& a, const LordAttackerRow& b) { return a.totalDmg > b.totalDmg; });
        if (liveRows.size() > 8) liveRows.resize(8);

        const float atkFontSz = font->FontSize;
        const float rowH = std::max(20.f, atkFontSz + 6.f);
        const float iconSz = 16.f;

        for (const auto& atk : liveRows)
        {
            ImGui::SetCursorPosX(startX);
            ImVec2 rowP = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // The player picked from a party row's context menu (Lord damage...).
            if (atk.agentId == m_lordDmgHighlightAgent)
            {
                dl->AddRectFilled(ImVec2(rowP.x - 2.f, rowP.y), ImVec2(rowP.x + colW, rowP.y + rowH),
                                  IM_COL32(0xFF, 0xD7, 0x64, 0x1C), 3.f);
                dl->AddRect(ImVec2(rowP.x - 2.f, rowP.y), ImVec2(rowP.x + colW, rowP.y + rowH),
                            IM_COL32(0xFF, 0xD7, 0x64, 0xB0), 3.f);
            }

            float iconY = rowP.y + (rowH - iconSz) * 0.5f;
            ImTextureID profTex = LoadProfIcon(dev, atk.professionId);
            if (profTex)
                dl->AddImage(profTex, ImVec2(rowP.x, iconY), ImVec2(rowP.x + iconSz, iconY + iconSz));

            float textY = rowP.y + (rowH - atkFontSz) * 0.5f;
            float nameW = std::max(40.f, colW * 0.45f);
            float nameX = rowP.x + iconSz + 4.f;
            dl->PushClipRect(ImVec2(nameX, rowP.y),
                ImVec2(nameX + nameW, rowP.y + rowH), true);
            dl->AddText(font, atkFontSz, ImVec2(nameX, textY),
                cWhite, atk.name.c_str());
            dl->PopClipRect();

            char pctBuf[16];
            snprintf(pctBuf, sizeof(pctBuf), "%d%%", (int)(atk.pct * 100.f));
            ImVec2 pctSz = font->CalcTextSizeA(atkFontSz, FLT_MAX, 0.f, pctBuf);
            float pctX = nameX + nameW + 4 + 30 - pctSz.x;
            dl->AddText(font, atkFontSz, ImVec2(pctX, textY), cWhite, pctBuf);

            float barX = nameX + nameW + 4 + 32;
            float barW = std::max(0.f, colW - (iconSz + 4 + nameW + 4 + 32));
            ImU32 barBg = IM_COL32(255, 255, 255, 15);
            ImU32 barFg = Team::IsRed(atk.teamId)
                ? IM_COL32(208, 72, 72, 178)
                : IM_COL32(74, 144, 216, 178);
            dl->AddRectFilled(ImVec2(barX, rowP.y + 3),
                ImVec2(barX + barW, rowP.y + rowH - 3), barBg, 2.f);
            dl->AddRectFilled(ImVec2(barX, rowP.y + 3),
                ImVec2(barX + barW * atk.pct, rowP.y + rowH - 3), barFg, 2.f);

            std::string absLabel = formatDmg(atk.totalDmg);
            ImVec2 absSz = font->CalcTextSizeA(atkFontSz, FLT_MAX, 0.f, absLabel.c_str());
            const float barPad = 4.f;
            float absTextY = rowP.y + (rowH - absSz.y) * 0.5f;
            float absTextX;
            if (barW >= absSz.x + barPad * 2.f)
                absTextX = barX + barW - barPad - absSz.x;
            else
                absTextX = barX + barW + barPad;
            dl->AddText(font, atkFontSz, ImVec2(absTextX + 1.f, absTextY + 1.f),
                IM_COL32(0, 0, 0, 0xCC), absLabel.c_str());
            dl->AddText(font, atkFontSz, ImVec2(absTextX, absTextY), cWhite, absLabel.c_str());

            ImGui::Dummy(ImVec2(colW, rowH));
        }

        // A highlighted attacker the top-8 list cut off still gets his line.
        if (m_lordDmgHighlightAgent >= 0)
        {
            const bool listed = std::any_of(liveRows.begin(), liveRows.end(),
                [&](const LordAttackerRow& r) { return r.agentId == m_lordDmgHighlightAgent; });
            auto hit = liveAttackerDmg.find(m_lordDmgHighlightAgent);
            if (!listed && hit != liveAttackerDmg.end())
            {
                ImGui::SetCursorPosX(startX);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.84f, 0.39f, 1.f));
                ImGui::Text("Highlighted player: %s (%d%%, outside the top 8)",
                            formatDmg(hit->second).c_str(), (int)(100.f * hit->second / liveTotalDmg));
                ImGui::PopStyleColor();
            }
        }
    };

    // ---- PART 3: aggressiveness - the 28:00 tiebreak score of the team attacking this column's lord ----
    // Icons only; each one explains itself on hover. Points move the attacking team's score, except
    // enchantments and weapon spells, which that team cast on its own lord.
    auto agentName = [&](int agentId) -> std::string {
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return agentId ? std::format("#{}", agentId) : "unknown";
        const auto& a = it->second;
        if (!a.partyBarLabel.empty()) return a.partyBarLabel;
        if (!a.playerName.empty()) return a.playerName;
        return a.categoryName.empty() ? std::format("#{}", agentId) : a.categoryName;
    };
    auto skillName = [&](int skillId) -> std::string {
        const SkillInfo* si = skillId ? m_skillView.Get(skillId) : nullptr;
        return si ? si->name : std::string();
    };
    // The skill icon list is filled on first use by whichever panel needs it; this one may be first.
    EnsureSkillIconIndex();
    auto skillIcon = [&](int skillId) -> ImTextureID {
        return skillId ? LoadSkillIcon(this, dev, skillId, m_skillIconIndex, m_skillIconCache) : ImTextureID{};
    };
    auto profIconOf = [&](int agentId) -> ImTextureID {
        auto it = m_replayCtx.agents.find(agentId);
        return it != m_replayCtx.agents.end() ? LoadProfIcon(dev, it->second.primaryProf) : ImTextureID{};
    };
    auto tipIcon = [&](ImTextureID tex, float sz) {
        if (tex) ImGui::Image(tex, ImVec2(sz, sz));
        else ImGui::Dummy(ImVec2(sz, sz));
        ImGui::SameLine(0.f, 5.f);
    };
    // Whole points with a thousands separator and, when asked, a sign: +4,312 / -150 / 0.
    auto fmtPoints = [](float v, bool sign) -> std::string {
        const long n = std::lround(v);
        std::string digits = std::to_string(std::labs(n));
        for (int i = (int)digits.size() - 3; i > 0; i -= 3) digits.insert((size_t)i, ",");
        if (n < 0) return "-" + digits;
        return (sign && n > 0 ? "+" : "") + digits;
    };
    const ImVec4 kGain(0.40f, 0.78f, 0.40f, 1.f), kCost(0.86f, 0.40f, 0.35f, 1.f);
    const ImVec4 kDim(0.47f, 0.49f, 0.53f, 1.f), kGold(0.88f, 0.71f, 0.39f, 1.f);

    // The events of one cause for one team so far, grouped by (actor, source, skill), largest first.
    struct AggrGroup { int actor; int source; int skill; bool estimated; int count; float health; float points; };
    auto groupsOf = [&](int team, LordAggressiveness::Cause cause) {
        std::vector<AggrGroup> out;
        for (const auto& ev : m_lordAggr.events)
        {
            if (ev.time > curTime) break;
            if (ev.team != team || ev.cause != cause) continue;
            auto g = std::find_if(out.begin(), out.end(), [&](const AggrGroup& x) {
                return x.actor == ev.actor && x.source == ev.source && x.skill == ev.sourceSkill; });
            if (g == out.end())
            {
                out.push_back({ ev.actor, ev.source, ev.sourceSkill, ev.estimated, 0, 0.f, 0.f });
                g = std::prev(out.end());
            }
            ++g->count; g->health += ev.health; g->points += ev.points;
        }
        std::sort(out.begin(), out.end(), [](const AggrGroup& a, const AggrGroup& b) {
            return std::fabs(a.points) + a.health > std::fabs(b.points) + b.health; });
        return out;
    };

    auto DrawAggressiveness = [&](int idx) {
        using LordAggressiveness::Cause;
        if (!m_lordAggr.valid) return;
        const int lordTeam = idx + 1;
        const int team = 3 - lordTeam;   // the attackers of this column's lord
        const auto mine = LordAggressiveness::Sum(m_lordAggr, team, curTime);
        const auto theirs = LordAggressiveness::Sum(m_lordAggr, lordTeam, curTime);
        const bool isRed = Team::IndexIsRed(team - 1);
        const std::string& guild = isRed ? m_team2GuildHeader : m_team1GuildHeader;
        const std::string who = guild.empty() ? (isRed ? "Red" : "Blue") : guild;
        const std::string other = (isRed ? m_team1GuildHeader : m_team2GuildHeader).empty()
            ? (isRed ? "Blue" : "Red") : (isRed ? m_team1GuildHeader : m_team2GuildHeader);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // The score: the team's flag, the number in the team's colour, a triangle for ahead / behind.
        const float lineH = 20.f;
        ImGui::BeginGroup();
        if (ImTextureID flag = LoadFlagIcon(dev, isRed ? "Red_flag_waving.svg.png" : "Blue_flag_waving.svg.png"))
        {
            ImGui::Image(flag, ImVec2(lineH, lineH));
            ImGui::SameLine(0.f, 6.f);
        }
        {
            const std::string score = fmtPoints(mine.score, false);
            const float big = font->FontSize * 1.3f;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            dl->AddText(font, big, ImVec2(at.x, at.y + (lineH - big) * 0.5f), isRed ? cRed : cBlue, score.c_str());
            ImGui::Dummy(ImVec2(font->CalcTextSizeA(big, FLT_MAX, 0.f, score.c_str()).x, lineH));
        }
        const float lead = mine.score - theirs.score;
        if (std::fabs(lead) >= 0.5f)
        {
            ImGui::SameLine(0.f, 8.f);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float s = 5.f, cy = at.y + lineH * 0.5f;
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(lead > 0.f ? kGain : kCost);
            if (lead > 0.f) dl->AddTriangleFilled(ImVec2(at.x, cy + s * 0.5f), ImVec2(at.x + s * 2.f, cy + s * 0.5f), ImVec2(at.x + s, cy - s * 0.7f), col);
            else            dl->AddTriangleFilled(ImVec2(at.x, cy - s * 0.5f), ImVec2(at.x + s * 2.f, cy - s * 0.5f), ImVec2(at.x + s, cy + s * 0.7f), col);
            ImGui::Dummy(ImVec2(s * 2.f + 3.f, lineH));
            ImGui::SameLine(0.f, 2.f);
            const std::string diff = fmtPoints(std::fabs(lead), false);
            const ImVec2 at2 = ImGui::GetCursorScreenPos();
            dl->AddText(ImVec2(at2.x, at2.y + (lineH - font->FontSize) * 0.5f), col, diff.c_str());
            ImGui::Dummy(ImVec2(ImGui::CalcTextSize(diff.c_str()).x, lineH));
        }
        ImGui::EndGroup();
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
            ImGui::TextColored(kGold, "Aggressiveness - %s", who.c_str());
            ImGui::Text("%s at %s, %s %s.", fmtPoints(mine.score, false).c_str(),
                        std::format("{}:{:02}", (int)curTime / 60, (int)curTime % 60).c_str(),
                        lead > 0.f ? "ahead of" : lead < 0.f ? "behind" : "level with", other.c_str());
            if (std::fabs(lead) >= 0.5f) ImGui::Text("Gap: %s points.", fmtPoints(std::fabs(lead), false).c_str());
            ImGui::Spacing();
            ImGui::TextColored(kDim, "If neither Guild Lord dies, the team with more aggressiveness at 28:00 wins. "
                                     "Score = damage on the enemy lord + 20%% of it under his Entourage + 30 per blind miss + 20 per blocked "
                                     "attack - 50 per enchantment or weapon spell on your own lord (-300 while its Entourage is up).");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

        // One icon per cause, its points beside it, details on hover.
        struct Chip { Cause cause; ImTextureID icon; const char* title; const char* rule; bool counted; };
        const bool lordIsRed = Team::IndexIsRed(idx);
        std::vector<Chip> chips = {
            { Cause::Damage, LoadFlagIcon(dev, "damagedone.png"), "Damage on the enemy lord",
              "+1 per point of damage: normal, critical and armour-ignoring hits. Damage his Amulet of Protection heals back still counts.", true },
            { Cause::EntourageDamage, LoadFlagIcon(dev, lordIsRed ? "redguildlord.png" : "blueguildlord.png"), "Damage under Entourage",
              "Damage that lands while the enemy lord's Entourage is up counts 20%. He can only cast it while his pit NPCs outnumber the attackers in the pit.", true },
        };
        // Blind always has its chip; a recording made before the blind effect was logged (Oct 8, 2026)
        // can't tell a blind miss from a block, so there it stays dimmed with a "?".
        chips.push_back({ Cause::BlindMiss, skillIcon(479), "Blind misses",
              m_lordAggr.blindKnown
                ? "+30 for each attack on the enemy lord that missed while the attacker was Blinded."
                : "+30 for each attack on the enemy lord that missed while the attacker was Blinded. "
                  "This recording was made before blind was logged (Oct 8, 2026), so its blind misses are counted with the blocks.", true });
        chips.push_back({ Cause::BlockOrMiss, skillIcon(810), m_lordAggr.blindKnown ? "Blocked attacks" : "Blocked or missed attacks",
              m_lordAggr.blindKnown
                ? "+20 for each attack on the enemy lord that was blocked or missed. The blocking team loses nothing for it."
                : "+20 for each attack on the enemy lord that was blocked or missed. The blocking team loses nothing for it. "
                  "Recordings made before Oct 8, 2026 don't show blind, so blind misses are counted here too.", true });
        chips.push_back({ Cause::Enchantment, sIconEnchanted, "Enchantments on own lord",
              "-50 for each enchantment this team's players cast on their own lord, -300 if it lands while the lord's Entourage is up.", true });
        chips.push_back({ Cause::WeaponSpell, sIconWeaponSpell, "Weapon spells on own lord",
              "-50 for each weapon spell this team's players cast on their own lord, -300 if it lands while the lord's Entourage is up.", true });
        chips.push_back({ Cause::LifeSteal, skillIcon(657), "Life steal",
              "Health stolen from the enemy lord is not counted.", false });

        // Chips sized to their number and flowed left to right, wrapping only when the column is full.
        const float chipH = 22.f, ico = 18.f, chipGap = 4.f;
        const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY() + 3.f;
        const float availW = ImGui::GetContentRegionAvail().x;
        float cx = 0.f, cy = 0.f;
        for (size_t i = 0; i < chips.size(); ++i)
        {
            const Chip& c = chips[i];
            const auto& line = mine.by[(size_t)c.cause];
            const bool active = line.count > 0;
            const bool unknown = c.cause == Cause::BlindMiss && !m_lordAggr.blindKnown;
            const std::string val = unknown ? "?" : c.counted ? fmtPoints(line.points, true) : fmtPoints(line.health, false);
            const float chipW = 2.f + ico + 4.f + ImGui::CalcTextSize(val.c_str()).x + 6.f;
            if (cx > 0.f && cx + chipW > availW) { cx = 0.f; cy += chipH + 2.f; }
            ImGui::SetCursorPos(ImVec2(x0 + cx, y0 + cy));
            cx += chipW + chipGap;
            ImGui::PushID((int)c.cause + idx * 100);
            ImGui::InvisibleButton("##chip", ImVec2(chipW, chipH));
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
            dl->AddRectFilled(r0, r1, hovered ? IM_COL32(255, 255, 255, 26) : IM_COL32(255, 255, 255, 8), 3.f);
            const ImVec2 i0(r0.x + 2.f, r0.y + (r1.y - r0.y - ico) * 0.5f);
            if (c.icon)
                dl->AddImage(c.icon, i0, ImVec2(i0.x + ico, i0.y + ico), ImVec2(0, 0), ImVec2(1, 1),
                             active ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 70));
            const ImVec4 valCol = !active ? kDim : !c.counted ? kDim : line.points < 0.f ? kCost : kGain;
            dl->AddText(ImVec2(i0.x + ico + 4.f, r0.y + (r1.y - r0.y - font->FontSize) * 0.5f),
                        ImGui::ColorConvertFloat4ToU32(valCol), val.c_str());
            if (hovered)
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
                tipIcon(c.icon, 20.f);
                ImGui::TextColored(kGold, "%s", c.title);
                ImGui::TextColored(kDim, "%s", c.rule);
                ImGui::Spacing();
                switch (c.cause)
                {
                case Cause::Damage:
                    ImGui::Text("%s HP in %d hits: %s points.", fmtPoints(line.health, false).c_str(), line.count, fmtPoints(line.points, true).c_str());
                    break;
                case Cause::EntourageDamage:
                    ImGui::Text("%s HP in %d hits under Entourage: %s points.", fmtPoints(line.health, false).c_str(), line.count, fmtPoints(line.points, true).c_str());
                    break;
                case Cause::LifeSteal:
                    ImGui::Text("%s HP stolen in %d hits: not counted.", fmtPoints(line.health, false).c_str(), line.count);
                    break;
                case Cause::BlindMiss:
                case Cause::BlockOrMiss:
                    ImGui::Text("%d attacks: %s points.", line.count, fmtPoints(line.points, true).c_str());
                    break;
                default:
                {
                    ImGui::Text("%d casts: %s points.", line.count, fmtPoints(line.points, true).c_str());
                    int underEnt = 0;
                    for (const auto& ev : m_lordAggr.events)
                    {
                        if (ev.time > curTime) break;
                        if (ev.team == team && ev.cause == c.cause && ev.underEntourage) ++underEnt;
                    }
                    if (underEnt > 0)
                        ImGui::TextColored(kCost, "%d of them while the lord's Entourage was up: -300 each.", underEnt);
                }
                    break;
                }
                const auto groups = groupsOf(team, c.cause);
                if (!groups.empty())
                {
                    ImGui::Separator();
                    const size_t shown = std::min<size_t>(groups.size(), 8);
                    for (size_t k = 0; k < shown; ++k)
                    {
                        const AggrGroup& g = groups[k];
                        switch (c.cause)
                        {
                        case Cause::Enchantment:
                        case Cause::WeaponSpell:
                            tipIcon(skillIcon(g.skill), 16.f);
                            ImGui::Text("%s x%d", skillName(g.skill).c_str(), g.count);
                            ImGui::SameLine(0.f, 6.f);
                            tipIcon(profIconOf(g.actor), 16.f);
                            ImGui::Text("%s", agentName(g.actor).c_str());
                            ImGui::SameLine(0.f, 8.f);
                            ImGui::TextColored(kCost, "%s", fmtPoints(g.points, true).c_str());
                            break;
                        case Cause::BlindMiss:
                            tipIcon(profIconOf(g.actor), 16.f);
                            ImGui::Text("%s x%d, blinded by", agentName(g.actor).c_str(), g.count);
                            ImGui::SameLine(0.f, 6.f);
                            tipIcon(skillIcon(g.skill), 16.f);
                            ImGui::Text("%s", g.source ? agentName(g.source).c_str() : "unknown");
                            ImGui::SameLine(0.f, 8.f);
                            ImGui::TextColored(kGain, "%s", fmtPoints(g.points, true).c_str());
                            break;
                        case Cause::BlockOrMiss:
                            tipIcon(profIconOf(g.actor), 16.f);
                            ImGui::Text("%s x%d", agentName(g.actor).c_str(), g.count);
                            ImGui::SameLine(0.f, 6.f);
                            if (g.source)
                            {
                                ImGui::TextColored(kDim, "likely");
                                ImGui::SameLine(0.f, 5.f);
                                tipIcon(skillIcon(g.skill), 16.f);
                                ImGui::Text("%s", agentName(g.source).c_str());
                            }
                            else
                                ImGui::TextColored(kDim, "no block effect seen");
                            ImGui::SameLine(0.f, 8.f);
                            ImGui::TextColored(kGain, "%s", fmtPoints(g.points, true).c_str());
                            break;
                        default:
                            tipIcon(profIconOf(g.actor), 16.f);
                            ImGui::Text("%s", agentName(g.actor).c_str());
                            ImGui::SameLine(0.f, 8.f);
                            ImGui::TextColored(c.counted ? kGain : kDim, "%s HP", fmtPoints(g.health, false).c_str());
                            break;
                        }
                    }
                    if (groups.size() > shown)
                        ImGui::TextColored(kDim, "and %d more", (int)(groups.size() - shown));
                    if (c.cause == Cause::BlockOrMiss)
                        ImGui::TextColored(kDim, "The game doesn't say which defence blocked: the blocker is the block effect last put on the lord.");
                }
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        ImGui::SetCursorPos(ImVec2(x0, y0 + cy + chipH + 2.f));
        ImGui::Dummy(ImVec2(0.f, 0.f));
    };

    // ===== LAYOUT =====

    // Row 1: Lord headers (two columns)
    float col0X = ImGui::GetCursorPosX();
    float col1X = col0X + colW + 10.f;
    float savedY = ImGui::GetCursorPosY();

    DrawLordHeader(0, col0X);
    float afterHdr0Y = ImGui::GetCursorPosY();

    ImGui::SetCursorPosY(savedY);
    DrawLordHeader(1, col1X);
    float afterHdr1Y = ImGui::GetCursorPosY();

    ImGui::SetCursorPosY(std::max(afterHdr0Y, afterHdr1Y) + 2.f);
    ImGui::Separator();

    // Row 2: Timeline strip (full width, stable position)
    {
        float stripH = 14.f;   // two bands of 7 px: red lord on top, blue below
        float stripW = std::max(1.f, ImGui::GetContentRegionAvail().x);
        ImVec2 stripP = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImGui::InvisibleButton("##LordTimeline", ImVec2(stripW, stripH));
        if (ImGui::IsItemActive() && stripW > 1.f)
        {
            float mx = ImGui::GetIO().MousePos.x - stripP.x;
            float ratio = std::clamp(mx / stripW, 0.f, 1.f);
            m_debugTimeline = ratio * maxT;
        }

        int nBuckets = (int)std::max(m_lordDmg[0].damageBuckets.size(),
                                      m_lordDmg[1].damageBuckets.size());
        if (nBuckets < 1) nBuckets = 1;
        float bw = stripW / (float)nBuckets;

        for (int b = 0; b < (int)m_lordDmg[1].damageBuckets.size(); ++b)
        {
            ImVec2 bMin(stripP.x + b * bw, stripP.y);
            ImVec2 bMax(stripP.x + (b + 1) * bw, stripP.y + stripH * 0.5f);
            ImU32 col = m_lordDmg[1].damageBuckets[b]
                ? IM_COL32(208, 72, 72, 204)
                : IM_COL32(255, 255, 255, 10);
            dl->AddRectFilled(bMin, bMax, col);
        }

        for (int b = 0; b < (int)m_lordDmg[0].damageBuckets.size(); ++b)
        {
            ImVec2 bMin(stripP.x + b * bw, stripP.y + stripH * 0.5f);
            ImVec2 bMax(stripP.x + (b + 1) * bw, stripP.y + stripH);
            ImU32 col = m_lordDmg[0].damageBuckets[b]
                ? IM_COL32(74, 144, 216, 204)
                : IM_COL32(255, 255, 255, 10);
            dl->AddRectFilled(bMin, bMax, col);
        }

        dl->AddLine(ImVec2(stripP.x, stripP.y + stripH * 0.5f),
                    ImVec2(stripP.x + stripW, stripP.y + stripH * 0.5f),
                    IM_COL32(255, 255, 255, 25));

        float phX = stripP.x + (m_debugTimeline / maxT) * stripW;
        dl->AddLine(ImVec2(phX, stripP.y), ImVec2(phX, stripP.y + stripH),
            IM_COL32(200, 168, 75, 220), 1.5f);
    }

    ImGui::Separator();

    // Row 3: aggressiveness, the 28:00 tiebreak score, and its causes (two columns)
    // Each column is its own child window: a tree node's click area runs to its window's edge, and
    // one shared window would let the left column's rows cover the right column's.
    {
        const ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground;
        ImGui::SetCursorPosX(col0X);
        if (ImGui::BeginChild("##lord_score_0", ImVec2(colW, 0.f), ImGuiChildFlags_AutoResizeY, childFlags))
            DrawAggressiveness(0);
        ImGui::EndChild();
        ImGui::SameLine(col1X);
        if (ImGui::BeginChild("##lord_score_1", ImVec2(colW, 0.f), ImGuiChildFlags_AutoResizeY, childFlags))
            DrawAggressiveness(1);
        ImGui::EndChild();
        ImGui::Separator();
    }

    // Row 4: Attacker lists (two columns, grows with content)
    float atkSavedY = ImGui::GetCursorPosY();

    DrawLordAttackers(0, col0X);
    float afterAtk0Y = ImGui::GetCursorPosY();

    ImGui::SetCursorPosY(atkSavedY);
    DrawLordAttackers(1, col1X);
    float afterAtk1Y = ImGui::GetCursorPosY();

    ImGui::SetCursorPosY(std::max(afterAtk0Y, afterAtk1Y));

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}


// ---------------------------------------------------------------------------
// BuildTimelineData — precompute health curves + collect events
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// BuildLordDamageData — one-time aggregation of guild lord combat stats
// ---------------------------------------------------------------------------
void ReplayWindow::BuildLordDamageData()
{
    if (m_lordDamageBuilt) return;
    m_lordDamageBuilt = true;

    m_lordDmg[0] = {};
    m_lordDmg[1] = {};

    // Find guild lord agent IDs
    for (int nid : m_npcIds)
    {
        auto it = m_replayCtx.agents.find(nid);
        if (it == m_replayCtx.agents.end()) continue;
        if (it->second.categoryName != "Guild Lord") continue;
        int idx = (it->second.teamId == 2) ? 1 : 0;
        m_lordDmg[idx].lordAgentId = nid;
    }

    BuildLordAggressiveness();

    float maxT = m_replayCtx.maxReplayTime;
    int bucketCount = std::max(1, (int)std::ceil(maxT / 5.f));

    for (int li = 0; li < 2; ++li)
    {
        auto& ld = m_lordDmg[li];
        if (ld.lordAgentId < 0) continue;

        ld.damageBuckets.resize(bucketCount, false);

        auto lordIt = m_replayCtx.agents.find(ld.lordAgentId);
        if (lordIt == m_replayCtx.agents.end()) continue;
        const auto& lordArd = lordIt->second;

        // Resolve base max HP for HP-bar rendering. The Lord's maximum is a stated constant, so
        // it is preferred over a recorded reading: max_hp only arrives while the recorder's
        // camera is on the agent, and a camera that never looks at the Lord left this at 0.
        uint32_t baseMaxHp = LookupGuildNpcMaxHealth(lordArd.modelId);
        if (baseMaxHp == 0)
            for (const auto& s : lordArd.snapshots)
                if (s.max_hp > 0) { baseMaxHp = s.max_hp; break; }
        if (baseMaxHp == 0)
            for (auto rit = lordArd.snapshots.rbegin(); rit != lordArd.snapshots.rend(); ++rit)
                if (rit->max_hp > 0) { baseMaxHp = rit->max_hp; break; }
        ld.lordMaxHp = baseMaxHp;

        // Low point from snapshots
        for (const auto& s : lordArd.snapshots)
            if (s.health_pct < ld.lowPointHp && s.health_pct >= 0.f)
                ld.lowPointHp = s.health_pct;

        // Hits from the tiebreak events: each one in Health at the lord's maximum when it landed
        // (morale boosts and Deep Wound move it), life steal left out as the tiebreak leaves it out.
        // The recorder's own lord_events assumed 1680 throughout.
        std::unordered_map<int, int> attackerDmg;
        std::vector<float> hitTimes;
        const int lordTeam = lordArd.teamId == 2 ? 2 : 1;

        for (const auto& ev : m_lordAggr.events)
        {
            using LordAggressiveness::Cause;
            if (ev.team != 3 - lordTeam) continue;
            if (ev.cause != Cause::Damage && ev.cause != Cause::EntourageDamage) continue;
            const int rawDmg = (int)std::lround(ev.health);
            if (rawDmg <= 0) continue;
            ld.totalDmgAbs += rawDmg;
            attackerDmg[ev.actor] += rawDmg;
            hitTimes.push_back(ev.time);
            ld.hits.push_back({ ev.time, ev.actor, rawDmg });

            int bucket = std::clamp((int)(ev.time / 5.f), 0, bucketCount - 1);
            ld.damageBuckets[bucket] = true;
        }

        // Phases: 10+ second gap = new phase
        if (!hitTimes.empty())
        {
            std::sort(hitTimes.begin(), hitTimes.end());
            ld.phaseCount = 1;
            for (size_t i = 1; i < hitTimes.size(); ++i)
                if (hitTimes[i] - hitTimes[i - 1] >= 10.f)
                    ld.phaseCount++;
        }

        // Build attacker rows
        for (auto& [aid, dmg] : attackerDmg)
        {
            LordAttackerRow row;
            row.agentId = aid;
            row.totalDmg = dmg;
            row.pct = (ld.totalDmgAbs > 0) ? (float)dmg / (float)ld.totalDmgAbs : 0.f;
            auto ait = m_replayCtx.agents.find(aid);
            if (ait != m_replayCtx.agents.end())
            {
                row.name = ait->second.partyBarLabel.empty()
                    ? ait->second.playerName : ait->second.partyBarLabel;
                row.professionId = ait->second.primaryProf;
                row.teamId = ait->second.teamId;
            }
            ld.attackers.push_back(std::move(row));
        }
        std::sort(ld.attackers.begin(), ld.attackers.end(),
            [](const LordAttackerRow& a, const LordAttackerRow& b) { return a.totalDmg > b.totalDmg; });
        if (ld.attackers.size() > 8)
            ld.attackers.resize(8);
    }

    // Lord damage panel is toggled via menu, not auto-shown
}

// ---------------------------------------------------------------------------
// BuildLordAggressiveness — the recording's records handed to the private tiebreak formula
// ---------------------------------------------------------------------------
void ReplayWindow::BuildLordAggressiveness()
{
    using namespace LordAggressiveness;
    const StoCData& stoc = m_replayCtx.stocData;
    const auto& agents = m_replayCtx.agents;

    Inputs in;
    for (const auto& ld : m_lordDmg)
    {
        auto it = agents.find(ld.lordAgentId);
        if (ld.lordAgentId >= 0 && it != agents.end())
            in.lord[it->second.teamId == 2 ? 2 : 1] = ld.lordAgentId;
    }
    in.teamOf = [&agents](int a) {
        auto it = agents.find(a);
        return it != agents.end() ? (int)it->second.teamId : 0;
    };
    in.isPlayer = [&agents](int a) {
        auto it = agents.find(a);
        return it != agents.end() && it->second.type == AgentType::Player;
    };
    in.skillType = [this](int skillId) {
        const SkillInfo* si = m_skillView.Get(skillId);
        return si ? si->type : 0;
    };
    in.distance = [&agents](int a, int b, float t) {
        auto ia = agents.find(a), ib = agents.find(b);
        if (ia == agents.end() || ib == agents.end()) return -1.f;
        const AgentSnapshot* sa = FindSnapshotAtTime(ia->second, t);
        const AgentSnapshot* sb = FindSnapshotAtTime(ib->second, t);
        if (!sa || !sb) return -1.f;
        return std::hypot(sa->x - sb->x, sa->y - sb->y);
    };

    in.packets.reserve(stoc.combat.size());
    for (const CombatEvent& ce : stoc.combat)
        if (ce.IsDamageOrHeal())
            in.packets.push_back({ ce.time, ce.type == "HEAL", ce.caster_id, ce.target_id, ce.value, ce.damage_type });
    for (const BasicAttackEvent& b : stoc.basicAttack)
    {
        if (b.type == "ATTACK_STARTED") in.swings.push_back({ b.time, b.caster_id, b.target_id, false });
        else if (b.type == "ATTACK_STOPPED") in.swings.push_back({ b.time, b.caster_id, b.target_id, true });
    }
    for (const SkillActivationEvent& s : stoc.skill)
    {
        if (s.type == "SKILL_ACTIVATED") in.casts.push_back({ s.time, s.skill_id, s.caster_id, s.target_id, false });
        else if (s.type == "INSTANT_SKILL_USED") in.casts.push_back({ s.time, s.skill_id, s.caster_id, s.target_id, true });
    }
    // Jumbo party values: the first party of the instance is team 1, the second team 2.
    for (const JumboMessageEvent& j : stoc.jumbo)
    {
        if (j.message != "MORALE_BOOST") continue;
        if (j.party_value == 1635021873) in.moraleBoosts[1].push_back(j.time);
        else if (j.party_value == 1635021874) in.moraleBoosts[2].push_back(j.time);
    }
    for (int team = 1; team <= 2; ++team)
    {
        auto it = agents.find(in.lord[team]);
        if (it == agents.end()) continue;
        for (const AgentSnapshot& s : it->second.snapshots)
            in.lordDeepWound[team].push_back({ s.time, s.has_deep_wound });
    }
    // Older recordings have no visual effect log; without it a blind miss can't be told from a block.
    in.visualsRecorded = !stoc.visualEffects.empty();
    for (const VisualEffectEvent& v : stoc.visualEffects)
    {
        using K = VisualEffectEvent::Kind;
        if (v.kind == K::Add) in.visuals.push_back({ v.time, Visual::Kind::Add, v.agentId, v.value, v.otherId });
        else if (v.kind == K::Remove) in.visuals.push_back({ v.time, Visual::Kind::Remove, v.agentId, v.value, v.otherId });
        else if (v.kind == K::OnTarget) in.visuals.push_back({ v.time, Visual::Kind::OnTarget, v.agentId, v.value, v.otherId });
    }

    m_lordAggr = Build(in);
}

// ---------------------------------------------------------------------------
// TeamMoraleBoostsAt — morale boosts a team has earned by time t
// ---------------------------------------------------------------------------
// The jumbo MORALE_BOOST banners name the party: the instance's first party is team 1, the second
// team 2. A Guild Lord's maximum Health steps up with each (LordAggressiveness::kLordHealthByBoosts).
int ReplayWindow::TeamMoraleBoostsAt(int team, float t) const
{
    const int party = team == 1 ? 1635021873 : team == 2 ? 1635021874 : 0;
    if (!party) return 0;
    int n = 0;
    for (const JumboMessageEvent& j : m_replayCtx.stocData.jumbo)
        if (j.time <= t && j.party_value == party && j.message == "MORALE_BOOST")
            ++n;
    return n;
}
