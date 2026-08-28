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
// DrawLordDamagePanel — guild lord damage stats floating panel
// ---------------------------------------------------------------------------
void ReplayWindow::DrawLordDamagePanel()
{
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

    if (!ImGui::Begin("Lord Damage##lord_dmg", &m_showLordDamagePanel,
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
        bool isRed = (idx == 0);
        const char* teamLabel = isRed ? "Red" : "Blue";
        const char* iconFile = isRed ? "redguildlord.png" : "blueguildlord.png";
        const std::string& guildHeader = isRed ? m_team1GuildHeader : m_team2GuildHeader;

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
        uint8_t teamId = isRed ? 1 : 2;
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
                fillGrad = (teamId == 1) ? &gDeadRed : &gDeadBlue;
            else if (snap && snap->has_degen_hex)
                fillGrad = &gDegenHex;
            else if (snap && snap->has_poison)
                fillGrad = &gPoison;
            else if (snap && snap->has_bleeding)
                fillGrad = &gBleeding;
            else
                fillGrad = (teamId == 1) ? &gAliveRed : &gAliveBlue;

            if (isDead)
            {
                DrawLordGradRect(dl, innerTL, innerBR, *fillGrad);
            }
            else
            {
                const LordGrad5* deadGrad = (teamId == 1) ? &gDeadRed : &gDeadBlue;
                DrawLordGradRect(dl, innerTL, innerBR, *deadGrad);
                bool hasDeepWound = snap && snap->has_deep_wound && !isDead;
                float fillPct = hasDeepWound ? std::min(hpPct, 0.80f) : hpPct;
                if (fillPct > 0.f)
                    DrawLordGradRect(dl, innerTL, ImVec2(innerTL.x + innerW * fillPct, innerBR.y), *fillGrad);
                if (hasDeepWound)
                    DrawLordGradRect(dl, ImVec2(innerTL.x + innerW * 0.80f, innerTL.y), innerBR, gDeepWound);
            }

            char hpText[32];
            snprintf(hpText, sizeof(hpText), "%d%%", (int)(hpPct * 100.f));
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
            ImU32 lpCol = hpColor(liveLowHp);
            ImGui::GetWindowDrawList()->AddText(font, font->FontSize,
                ImGui::GetCursorScreenPos(), lpCol,
                std::format("Low point: {:.0f}%", liveLowHp * 100.f).c_str());
            ImGui::Dummy(ImVec2(colW, font->FontSize + 2.f));
            ImGui::SetCursorPosX(startX);

            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddText(font, font->FontSize, p, cLabel, "Dmg taken: ");
            ImVec2 valSz = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, "Dmg taken: ");
            ImGui::GetWindowDrawList()->AddText(font, font->FontSize,
                ImVec2(p.x + valSz.x, p.y), cWhite, formatDmg(liveTotalDmg).c_str());
            ImGui::Dummy(ImVec2(colW, font->FontSize + 2.f));
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
            ImU32 barFg = (atk.teamId == 2)
                ? IM_COL32(74, 144, 216, 178)
                : IM_COL32(208, 72, 72, 178);
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

    ImGui::SetCursorPosY(std::max(afterHdr0Y, afterHdr1Y) + 4.f);
    ImGui::Separator();

    // Row 2: Timeline strip (full width, stable position)
    {
        float stripH = 24.f;
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
            ImVec2 bMax(stripP.x + (b + 1) * bw, stripP.y + 12.f);
            ImU32 col = m_lordDmg[1].damageBuckets[b]
                ? IM_COL32(208, 72, 72, 204)
                : IM_COL32(255, 255, 255, 10);
            dl->AddRectFilled(bMin, bMax, col);
        }

        for (int b = 0; b < (int)m_lordDmg[0].damageBuckets.size(); ++b)
        {
            ImVec2 bMin(stripP.x + b * bw, stripP.y + 12.f);
            ImVec2 bMax(stripP.x + (b + 1) * bw, stripP.y + 24.f);
            ImU32 col = m_lordDmg[0].damageBuckets[b]
                ? IM_COL32(74, 144, 216, 204)
                : IM_COL32(255, 255, 255, 10);
            dl->AddRectFilled(bMin, bMax, col);
        }

        dl->AddLine(ImVec2(stripP.x, stripP.y + 12.f),
                    ImVec2(stripP.x + stripW, stripP.y + 12.f),
                    IM_COL32(255, 255, 255, 25));

        float phX = stripP.x + (m_debugTimeline / maxT) * stripW;
        dl->AddLine(ImVec2(phX, stripP.y), ImVec2(phX, stripP.y + stripH),
            IM_COL32(200, 168, 75, 220), 1.5f);
    }

    ImGui::Separator();

    // Row 3: Attacker lists (two columns, grows with content)
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

        // Use lord_events.txt data (server-computed integer damage)
        std::unordered_map<int, int> attackerDmg;
        std::vector<float> hitTimes;

        for (const auto& le : m_replayCtx.stocData.lordDamage)
        {
            if (le.target_id != ld.lordAgentId) continue;
            if (le.damage <= 0) continue;

            int rawDmg = le.damage;
            ld.totalDmgAbs += rawDmg;
            attackerDmg[le.caster_id] += rawDmg;
            hitTimes.push_back(le.time);
            ld.hits.push_back({ le.time, le.caster_id, rawDmg });

            int bucket = std::clamp((int)(le.time / 5.f), 0, bucketCount - 1);
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
