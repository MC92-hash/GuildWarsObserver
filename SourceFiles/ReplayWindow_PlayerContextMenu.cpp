#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "CursorSystem.h"
#include "TeamColors.h"
#include <algorithm>
#include <format>

// ---------------------------------------------------------------------------
// Party-row player context menu.
//
// Opened from a party row (right-click, its "..." button, Shift+F10 / Menu on the focused
// row). The target is captured when the menu opens, so the camera or playback moving on
// cannot change who the menu acts on. Everything here writes the same state the toolbar
// panels read; what is per player (ring ranges, forced / solo lasers) lives in explicit
// overrides those panels show and can reset. A ReplayWindow is one match, so none of it
// reaches another replay.
// ---------------------------------------------------------------------------

namespace
{
    const char* kPlayerMenuPopup = "##PlayerContextMenu";

    std::string WithThousands(int v)
    {
        std::string s = std::to_string(std::abs(v));
        for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
        return v < 0 ? "-" + s : s;
    }

    // An explanation under a menu entry: visible to keyboard users too, unlike a tooltip.
    void MenuNote(const char* text, ImVec4 col = ImVec4(0.62f, 0.60f, 0.54f, 1.f))
    {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 280.f);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
}

void ReplayWindow::RequestPlayerContextMenu(int agentId, bool byKeyboard)
{
    m_playerMenuRequestAgent = agentId;
    m_playerMenuRequestByKey = byKeyboard;
    // Called right after the row's item: a keyboard-opened menu drops down under it.
    m_playerMenuAnchor = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y);
}

// ---------------------------------------------------------------------------
// Actions shared with the toolbar panels
// ---------------------------------------------------------------------------

void ReplayWindow::SetPlayerLasersShown(int agentId, bool show)
{
    // Inside a "Show only" view, Show / Hide edit who is in it; the selection underneath stays put.
    if (m_laserSoloActive)
    {
        if (show) m_laserSoloAgents.insert(agentId);
        else      m_laserSoloAgents.erase(agentId);
        return;
    }
    if (show)
    {
        m_laserHiddenAgents.erase(agentId);
        // Still filtered out (overlay off, team or profession off)? Force this one caster on
        // rather than widening a filter that would also let other players through.
        auto it = m_replayCtx.agents.find(agentId);
        if (it != m_replayCtx.agents.end() && !LaserCasterVisible(agentId, it->second))
            m_laserForcedAgents.insert(agentId);
    }
    else
    {
        m_laserForcedAgents.erase(agentId);
        m_laserHiddenAgents.insert(agentId);
    }
}

bool ReplayWindow::PlayerHeatmapLayerShown(int agentId) const
{
    if (!m_heatmapSettings.renderEnabled) return false;
    for (const auto& l : m_heatmapSettings.layers)
        if (l.subjectType == HeatmapSubjectType::PLAYER && l.subjectId == agentId && l.matched && l.enabled)
            return true;
    return false;
}

void ReplayWindow::ShowPlayerHeatmapLayer(int agentId)
{
    auto it = m_replayCtx.agents.find(agentId);
    if (it == m_replayCtx.agents.end()) return;
    const auto& ard = it->second;
    // Same subject name the Heatmap panel gives a layer, so ResolveHeatmapLayers() matches it.
    const std::string name = ard.playerName.empty() ? ard.categoryName : ard.playerName;

    // Re-enable this player's layer if there is one; otherwise add one. Other layers stay.
    bool found = false;
    for (auto& l : m_heatmapSettings.layers)
    {
        if (l.subjectType != HeatmapSubjectType::PLAYER) continue;
        if (l.subjectId != agentId && l.subjectName != name) continue;
        l.enabled = true;
        found = true;
    }
    if (!found)
    {
        HeatmapLayerDef def;
        def.subjectType = HeatmapSubjectType::PLAYER;
        def.subjectId   = agentId;
        def.subjectName = name;
        def.palette     = HeatmapPalette::THERMAL;
        m_heatmapSettings.layers.push_back(std::move(def));
    }

    // What the host does after the Heatmap panel edits its layers.
    const size_t n = m_heatmapSettings.layers.size();
    m_heatmapAccumulator.EnsureLayerCount(n);
    if (m_heatmapInitialized)
        m_heatmapRenderer.EnsureLayerTextures(m_deviceResources->GetD3DDevice(), n);
    m_heatmapAccumulator.MarkAllLayersDirty();
    ResolveHeatmapLayers();
    m_heatmapSettings.renderEnabled = true;
    m_heatmapSettings.show = true;
    SaveHeatmapSettings();
    m_pendingWindowFocus = "Heatmap";
}

void ReplayWindow::OpenPlayerCombatLog(int agentId, bool damage)
{
    m_showCombatLog = true;
    m_clFilterPlayerId = agentId;   // matches the player as source or target

    // Nothing left over from earlier use may hide what was asked for.
    m_clFilterSkillIds.clear();
    m_clFilterSkillSet.clear();
    m_clSkillSearchBuf[0] = '\0';
    m_clFilterDamage    = damage;
    m_clFilterHeals     = !damage;
    m_clFilterSkills    = false;
    m_clFilterInterrupt = false;
    m_clFilterCancel    = false;
    m_clFilterDeaths    = false;
    m_clFilterAttacks   = false;
    m_clFilterJumbo     = false;
    m_clSelectedRowIdx  = -1;
    m_clAutoScroll      = true;
    m_clScrollToSelected = true;
    m_pendingWindowFocus = "Combat Log";
}

void ReplayWindow::OpenPlayerSkillAnalytics(int agentId)
{
    m_analyticsOpenPlayers.insert(agentId);
    m_pendingWindowFocus = std::format("###SAPlayer_{}", agentId);
}

// ---------------------------------------------------------------------------
// The menu
// ---------------------------------------------------------------------------

void ReplayWindow::DrawPlayerContextMenu()
{
    // Bring a panel an action opened forward. A panel opened for the first time only exists
    // after its Begin() later this frame, so the lookup is retried for a few frames.
    if (!m_pendingWindowFocus.empty())
    {
        if (ImGuiWindow* w = ImGui::FindWindowByName(m_pendingWindowFocus.c_str()); w && w->Active)
        {
            ImGui::FocusWindow(w);
            m_pendingWindowFocus.clear();
            m_pendingWindowFocusTries = 0;
        }
        else if (++m_pendingWindowFocusTries > 3)
        {
            m_pendingWindowFocus.clear();
            m_pendingWindowFocusTries = 0;
        }
    }

    if (m_playerMenuRequestAgent >= 0)
    {
        m_playerMenuAgentId     = m_playerMenuRequestAgent;
        m_playerMenuOpenedByKey = m_playerMenuRequestByKey;
        m_playerMenuRequestAgent = -1;
        m_playerMenuActionTookFocus = false;
        ImGui::OpenPopup(kPlayerMenuPopup);
        if (m_playerMenuOpenedByKey)
        {
            // Keyboard-driven from here: show where the arrow keys are.
            ImGui::SetNextWindowPos(m_playerMenuAnchor);
            GImGui->NavDisableHighlight  = false;
            GImGui->NavDisableMouseHover = true;
        }
    }

    ImGui::PushStyleColor(ImGuiCol_PopupBg,       ImVec4(0.055f, 0.063f, 0.078f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0.40f, 0.33f, 0.15f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.30f, 0.24f, 0.09f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Separator,     ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 8.f));

    bool open = false;
    if (ImGui::BeginPopup(kPlayerMenuPopup))
    {
        open = true;
        const int id = m_playerMenuAgentId;
        auto ait = m_replayCtx.agents.find(id);
        if (ait == m_replayCtx.agents.end() || ait->second.type != AgentType::Player)
        {
            ImGui::CloseCurrentPopup();
        }
        else
        {
            const AgentReplayData& ard = ait->second;
            ID3D11Device* dev = m_deviceResources->GetD3DDevice();
            const bool dead = ard.isDeadAtTime(m_debugTimeline);
            const std::string name = ard.playerName.empty() ? ard.categoryName : ard.playerName;
            const bool keepOpen = true;

            // --- Who this is, on one line: primary/secondary icons, "Name (number)" in the
            // team colour, then the guild tag ---
            {
                for (int prof : { ard.primaryProf, ard.secondaryProf })
                    if (ImTextureID tex = (prof > 0) ? LoadProfIcon(dev, prof) : nullptr)
                    {
                        ImGui::Image(tex, ImVec2(18.f, 18.f));
                        ImGui::SameLine(0.f, 2.f);
                    }
                ImGui::SameLine(0.f, 4.f);
                const ImVec4 teamCol = Team::IsRed(ard.teamId)  ? ImVec4(1.f, 0.60f, 0.60f, 1.f)
                                     : Team::IsBlue(ard.teamId) ? ImVec4(0.60f, 0.80f, 1.f, 1.f)
                                                                : ImVec4(0.94f, 0.94f, 0.94f, 1.f);
                if (ard.playerNumber > 0)
                    ImGui::TextColored(teamCol, "%s (%d)", name.c_str(), ard.playerNumber);
                else
                    ImGui::TextColored(teamCol, "%s", name.c_str());

                // The guild header reads "Name [TAG]"; only the tag goes here.
                const std::string& header = Team::IsRed(ard.teamId)  ? m_team2GuildHeader
                                          : Team::IsBlue(ard.teamId) ? m_team1GuildHeader : std::string();
                const size_t lb = header.rfind('['), rb = header.rfind(']');
                if (lb != std::string::npos && rb != std::string::npos && rb > lb)
                {
                    ImGui::SameLine(0.f, 6.f);
                    ImGui::TextDisabled("%s", header.substr(lb, rb - lb + 1).c_str());
                }
            }
            ImGui::Separator();

            // --- Camera and panels ---
            if (ImGui::MenuItem("Follow player", nullptr, m_cameraMode == CameraMode::FollowAgent && m_followedAgentId == id))
                EnterFollowMode(id);
            if (m_autoCameraEnabled)
                MenuNote("Auto camera is on and may switch to another player.");

            if (ImGui::MenuItem("Watch in split camera", nullptr, m_pipEnabled && m_pipManualAgent == id))
            {
                m_pipEnabled = true;
                m_pipManualAgent = id;
                m_pendingWindowFocus = "Split Camera";
                m_playerMenuActionTookFocus = true;
            }

            if (ImGui::MenuItem("Open player info", nullptr, m_showPlayerInfoPanel && m_playerInfoAgentId == id))
            {
                OpenPlayerInfoPanel(id);   // one panel, retargeted: never a duplicate
                m_pendingWindowFocus = "##PlayerInfoPanel";
                m_playerMenuActionTookFocus = true;
            }

            if (ImGui::MenuItem("Open equipment"))
            {
                auto cp = std::find_if(m_characterPanels.begin(), m_characterPanels.end(),
                    [&](const CharacterPanelInstance& c) { return c.open && c.agentId == id; });
                if (cp == m_characterPanels.end())
                {
                    OpenCharacterPanel(id);
                    m_pendingWindowFocus = std::format("###char_panel_{}", m_characterPanels.back().uid);
                }
                else
                    m_pendingWindowFocus = std::format("###char_panel_{}", cp->uid);
                m_playerMenuActionTookFocus = true;
            }

            ImGui::Separator();

            // --- Range rings (this player only) ---
            if (ImGui::BeginMenu("Range rings"))
            {
                ImGui::PushItemFlag(ImGuiItemFlags_SelectableDontClosePopup, keepOpen);
                bool eff[kRingTypeCount];
                EffectiveRingTypes(id, ard, eff);
                auto ov = m_ringOverrides.find(id);
                const bool hasOverride = (ov != m_ringOverrides.end());
                bool anyOn = false;
                for (bool b : eff) anyOn |= b;

                for (int ri = 0; ri < kRingTypeCount; ++ri)
                {
                    const auto& def = GetRangeRingDef(ri);
                    const std::string radius = WithThousands((int)def.radius);
                    if (ImGui::MenuItem(def.name, radius.c_str(), eff[ri]))
                    {
                        // The first pick seeds this player's own ranges from what is drawn now.
                        auto& types = m_ringOverrides[id];
                        if (!hasOverride)
                            for (int k = 0; k < kRingTypeCount; ++k) types[k] = eff[k];
                        types[ri] = !eff[ri];
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Hide this player's rings", nullptr, hasOverride && !anyOn))
                    m_ringOverrides[id].fill(false);
                if (ImGui::MenuItem("Use toolbar defaults", nullptr, !hasOverride, hasOverride))
                    m_ringOverrides.erase(id);

                if (hasOverride)
                    MenuNote("This player uses their own ranges; other players keep the toolbar's.");
                else if (!m_showRangeRings)
                    MenuNote("Range rings are off in the toolbar. Picking a range shows it for this player only.");
                if (dead)
                    MenuNote("Rings are not drawn while the player is dead.");
                ImGui::PopItemFlag();
                ImGui::EndMenu();
            }

            // --- Skill lasers (outgoing, this player as caster) ---
            if (ImGui::BeginMenu("Skill lasers"))
            {
                ImGui::PushItemFlag(ImGuiItemFlags_SelectableDontClosePopup, keepOpen);
                MenuNote("Lines from this player to the targets of their skills.");
                const bool shown = LaserCasterVisible(id, ard) && !(m_topViewActive && !m_showSkillLasers);
                const bool solo = m_laserSoloActive && m_laserSoloAgents.count(id) && m_laserSoloAgents.size() == 1;

                if (ImGui::MenuItem("Show this player's lasers", nullptr, shown, !m_topViewActive))
                    SetPlayerLasersShown(id, true);
                if (ImGui::MenuItem("Hide this player's lasers", nullptr, !shown, !m_topViewActive))
                    SetPlayerLasersShown(id, false);
                if (ImGui::MenuItem("Show only this player's lasers", nullptr, solo, !m_topViewActive))
                {
                    m_laserSoloActive = true;
                    m_laserSoloAgents = { id };
                }
                if (ImGui::MenuItem("Restore previous selection", nullptr, false, m_laserSoloActive))
                {
                    m_laserSoloActive = false;
                    m_laserSoloAgents.clear();
                }
                if (m_topViewActive)
                    MenuNote("Skill lasers are hidden in top view.");
                else if (m_laserSoloActive)
                    MenuNote("\"Show only\" is active: Show / Hide add or remove players from it.");
                else if (m_laserForcedAgents.count(id))
                    MenuNote("Shown for this player even though the toolbar filters would hide them.");
                ImGui::PopItemFlag();
                ImGui::EndMenu();
            }

            // --- Fog of war (whole view) ---
            if (ImGui::BeginMenu("Fog of war"))
            {
                ImGui::PushItemFlag(ImGuiItemFlags_SelectableDontClosePopup, keepOpen);
                MenuNote("Changes what the whole replay view reveals, not just this player.",
                         ImVec4(0.90f, 0.78f, 0.45f, 1.f));
                const bool canFog = !m_topViewActive && Team::IsSide(ard.teamId);
                const int team = ard.teamId;

                if (ImGui::MenuItem("This player's vision", nullptr,
                                    m_fogPerspective == team && m_fogPlayerAgent == id, canFog))
                {
                    m_fogPerspective = team;
                    m_fogPlayerAgent = id;
                }
                if (dead)
                    MenuNote("Dead: this player provides no vision right now.", ImVec4(0.85f, 0.45f, 0.40f, 1.f));
                if (ImGui::MenuItem("This team's vision", nullptr,
                                    m_fogPerspective == team && m_fogPlayerAgent < 0, canFog))
                {
                    m_fogPerspective = team;
                    m_fogPlayerAgent = -1;
                }
                if (ImGui::MenuItem("Off", nullptr, m_fogPerspective == 0, !m_topViewActive))
                {
                    if (m_fogPerspective > 0) m_fogLastActive = m_fogPerspective;
                    m_fogPerspective = 0;
                    m_fogPlayerAgent = -1;
                }

                if (m_topViewActive)
                    MenuNote("Fog of war is suspended in top view.");
                else if (!Team::IsSide(ard.teamId))
                    MenuNote("This player is on neither team, so there is no vision to show.");
                else
                    ImGui::TextDisabled("Enemies in fog: %s (set in the Fog of War panel)",
                                        m_fogGhostMode ? "ghosted" : "hidden");
                ImGui::PopItemFlag();
                ImGui::EndMenu();
            }

            // --- Damage & healing ---
            if (ImGui::BeginMenu("Damage & healing"))
            {
                const bool ready = m_combatLogBuilt;
                if (ImGui::MenuItem("Open player breakdown...", nullptr, false, ready))
                {
                    OpenPlayerSkillAnalytics(id);
                    m_playerMenuActionTookFocus = true;
                }
                if (ImGui::MenuItem("Show damage events...", nullptr, false, ready))
                {
                    OpenPlayerCombatLog(id, true);
                    m_playerMenuActionTookFocus = true;
                }
                if (ImGui::MenuItem("Show healing events...", nullptr, false, ready))
                {
                    OpenPlayerCombatLog(id, false);
                    m_playerMenuActionTookFocus = true;
                }
                if (!ready)
                    MenuNote("Combat data is still being prepared for this replay.");
                else
                    MenuNote("Events lists cover this player as source or target, up to the current replay time.");
                ImGui::EndMenu();
            }

            // --- Analysis ---
            if (ImGui::BeginMenu("Analysis"))
            {
                if (ImGui::MenuItem("Skill analytics...", nullptr, false, m_combatLogBuilt))
                {
                    OpenPlayerSkillAnalytics(id);
                    m_playerMenuActionTookFocus = true;
                }
                if (!m_combatLogBuilt)
                    MenuNote("Skill analytics need the combat data, still being prepared.");

                if (ImGui::MenuItem("Cast timeline..."))
                {
                    m_showPianoRoll = true;
                    if (Team::IsRed(ard.teamId)) m_pianoRollTeam2Open = true;
                    else                         m_pianoRollTeam1Open = true;
                    m_pianoRollHighlightAgent = id;
                    m_pendingWindowFocus = "Piano Roll";
                    m_playerMenuActionTookFocus = true;
                }

                if (ImGui::MenuItem("Movement heatmap...", nullptr, PlayerHeatmapLayerShown(id)))
                {
                    ShowPlayerHeatmapLayer(id);
                    m_playerMenuActionTookFocus = true;
                }

                if (ImGui::MenuItem("Morale..."))
                {
                    m_showMoralePanel = true;
                    m_moraleDetailsExpanded = true;
                    m_moraleHighlightAgent = id;
                    m_pendingWindowFocus = "Morale##morale_panel";
                    m_playerMenuActionTookFocus = true;
                }

                // Lord damage: say up front when there is nothing to show.
                const bool hasLord = m_lordDamageBuilt &&
                                     (m_lordDmg[0].lordAgentId >= 0 || m_lordDmg[1].lordAgentId >= 0);
                int lordDmg = 0;
                if (hasLord)
                    for (const auto& ld : m_lordDmg)
                        for (const auto& h : ld.hits)
                        {
                            if (h.time > m_debugTimeline) break;
                            if (h.casterId == id) lordDmg += h.rawDmg;
                        }
                const std::string lordShort = hasLord ? (lordDmg > 0 ? WithThousands(lordDmg) : "none yet") : "";
                if (ImGui::MenuItem("Lord damage...", lordShort.c_str(), false, hasLord))
                {
                    m_showLordDamagePanel = true;
                    m_lordDmgHighlightAgent = id;
                    m_pendingWindowFocus = "Lord Damage##lord_dmg";
                    m_playerMenuActionTookFocus = true;
                }
                if (!m_combatLogBuilt || !m_lordDamageBuilt)
                    MenuNote("Lord damage needs the combat data, still being prepared.");
                else if (!hasLord)
                    MenuNote("No Guild Lord was found in this match.");
                ImGui::EndMenu();
            }

            ImGui::Separator();

            // --- This player's name label ---
            {
                ImGui::PushItemFlag(ImGuiItemFlags_SelectableDontClosePopup, keepOpen);
                const bool nameShown = m_show3DLabels && !m_hiddenNameAgents.count(id);
                if (ImGui::MenuItem("Show name under character", nullptr, nameShown, m_show3DLabels))
                {
                    if (nameShown) m_hiddenNameAgents.insert(id);
                    else           m_hiddenNameAgents.erase(id);
                }
                if (!m_show3DLabels)
                    MenuNote("Names are turned off for everyone (Agent Names panel).");
                ImGui::PopItemFlag();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);

    // Closed this frame: hand keyboard focus back to the row the menu came from, unless an
    // action brought a panel forward.
    if (m_playerMenuWasOpen && !open)
    {
        if (!m_playerMenuActionTookFocus &&
            (m_playerMenuOpenedByKey || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        {
            m_playerMenuReturnFocusAgent = m_playerMenuAgentId;
            m_playerMenuReturnFocusNav   = m_playerMenuOpenedByKey;
        }
        m_playerMenuAgentId = -1;
        m_playerMenuActionTookFocus = false;
    }

    // The menu owns the keyboard while open and for the frame it closes on (the Escape or
    // Enter that closed it must not also reach the replay hotkeys).
    m_playerMenuBlocksHotkeys = open || m_playerMenuWasOpen;
    m_playerMenuWasOpen = open;
}
