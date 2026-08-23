#include "pch.h"
#include "ReplayWindow.h"
#include "HealthModel.h"
#include "EquipmentHealth.h"
#include "SkillDatabase.h"
#include "ReplayWindow_Internal.h"

// Developer-only view of how each player's maximum health is arrived at.
//
// The whole point of the forward model is that a number can be explained term by term instead of
// asserted, so this shows the terms. Compiled out of public builds entirely: it exists to catch the
// model disagreeing with reality, which is not something a released build needs.

#if GWO_DEVELOPER

void ReplayWindow::DrawHealthModelWindow()
{
    if (!m_showHealthModelWindow) return;

    ImGui::SetNextWindowSize(ImVec2(1180, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Health Model (dev)", &m_showHealthModelWindow))
    {
        ImGui::End();
        return;
    }

    const float t = m_debugTimeline;
    ImGui::Text("t = %.2fs", t);
    ImGui::SameLine();
    ImGui::TextDisabled("| total = base + armour + weapon set + shrine, then morale, then Deep Wound");

    if (!m_healthInputsBuilt)
    {
        ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "Model inputs not built yet.");
        ImGui::End();
        return;
    }
    if (!m_replayCtx.stocData.equipment.loaded)
    {
        ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f),
                           "This recording carries no equipment stream, so the weapon-set term is "
                           "unknown and the model cannot run.");
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("healthmodel", 12, kFlags))
    {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Player");
        ImGui::TableSetupColumn("Base");
        ImGui::TableSetupColumn("Armour");
        ImGui::TableSetupColumn("Weapons");
        ImGui::TableSetupColumn("Shrine");
        ImGui::TableSetupColumn("Morale");
        ImGui::TableSetupColumn("Deep Wnd");
        ImGui::TableSetupColumn("Total");
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Recorded");
        ImGui::TableSetupColumn("Anchors");
        ImGui::TableSetupColumn("Possible armour");
        ImGui::TableHeadersRow();

        auto drawTeam = [&](const std::vector<int>& ids) {
            for (int id : ids)
            {
                auto it = m_replayCtx.agents.find(id);
                if (it == m_replayCtx.agents.end()) continue;
                const AgentReplayData& ard = it->second;
                if (ard.type != AgentType::Player) continue;

                const HealthModel::Breakdown b = HealthModel::At(ard, t, m_healthInputs);
                const AgentSnapshot* snap = FindSnapshotAtTime(ard, t);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ard.cachedLabel.empty() ? ard.playerName.c_str()
                                                               : ard.cachedLabel.c_str());

                ImGui::TableNextColumn();
                // The Dervish bonus is inside the base, so show it rather than leaving 505 to puzzle over.
                if (ard.primaryProf == 10) ImGui::Text("%d (480+25 D)", b.base);
                else                        ImGui::Text("%d", b.base);

                ImGui::TableNextColumn();
                if (ard.armourSolved) ImGui::Text("%+d", b.armour);
                else                  ImGui::TextDisabled("unsolved");

                ImGui::TableNextColumn();
                if (b.weaponSetKnown) ImGui::Text("%+d", b.weaponSet);
                else                  ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "%+d ?", b.weaponSet);
                if (ImGui::IsItemHovered() && snap)
                {
                    const Equipment::HealthMods mods = Equipment::HealthFromWeaponSet(
                        m_replayCtx.stocData.equipment, snap->weapon_item_id, snap->offhand_item_id);
                    ImGui::SetTooltip(
                        "flat %+d\nwhile enchanted %+d\nwhile hexed %+d\nwhile in a stance %+d\n\n"
                        "enchanted: %s   hexed: %s\n\n"
                        "Stances are not in the snapshot stream, so a stance mod is never counted:\n"
                        "the model under-reports rather than inventing health.",
                        mods.flat, mods.whenEnchanted, mods.whenHexed, mods.whenInStance,
                        snap->has_enchantment ? "yes" : "no",
                        (snap->has_hex || snap->has_degen_hex) ? "yes" : "no");
                }

                ImGui::TableNextColumn();
                if (b.shrine) ImGui::Text("%+d", b.shrine); else ImGui::TextDisabled("-");

                ImGui::TableNextColumn();
                const int pct = MoralePercentAtTime(ard, t);
                if (b.morale) ImGui::Text("%+d (%+d%%)", b.morale, pct);
                else          ImGui::TextDisabled("0");

                ImGui::TableNextColumn();
                if (b.deepWound) ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%+d", b.deepWound);
                else             ImGui::TextDisabled("-");

                ImGui::TableNextColumn();
                if (b.total) ImGui::Text("%u", b.total); else ImGui::TextDisabled("-");

                ImGui::TableNextColumn();
                switch (b.source)
                {
                case HealthModel::Source::Anchor:
                    // The server's own number for this instant: not a reconstruction.
                    ImGui::TextColored(ImVec4(0.4f, 1.f, 0.5f, 1.f), "anchor");
                    break;
                case HealthModel::Source::Modelled: ImGui::TextUnformatted("modelled"); break;
                default: ImGui::TextDisabled("fallback"); break;
                }

                ImGui::TableNextColumn();
                // The raw recorded field, for comparison. It is sticky, so a gap against the total
                // is expected and is exactly what the model exists to correct.
                const uint32_t recorded = snap ? snap->max_hp : 0;
                if (recorded)
                {
                    const int delta = (int)recorded - (int)b.total;
                    if (b.total && delta != 0)
                        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.4f, 1.f), "%u (%+d)", recorded, delta);
                    else
                        ImGui::Text("%u", recorded);
                    if (snap->max_hp_is_live)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.4f, 1.f, 0.5f, 1.f), "live");
                    }
                }
                else ImGui::TextDisabled("never sent");

                ImGui::TableNextColumn();
                ImGui::Text("%d/%d", ard.armourSupport, ard.armourObservations);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Readings agreeing with the solved armour, out of readings used.\n"
                                      "An anchor is a reading taken while the camera was on the agent,\n"
                                      "or one that had just changed -- both describe the state at that\n"
                                      "instant. Stale readings are skipped.");

                ImGui::TableNextColumn();
                if (!ard.armourSolved) { ImGui::TextDisabled("-"); continue; }
                const auto candidates = HealthModel::ArmourCandidates(ard.solvedArmourHealth, 3);
                if (candidates.empty())
                {
                    ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "no buildable set!");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("No combination of runes and insignias reaches %+d.\n"
                                          "Either a term of the model is wrong at the anchors, or the\n"
                                          "anchors themselves were stale.", ard.solvedArmourHealth);
                }
                else
                {
                    ImGui::TextUnformatted(candidates.front().label.c_str());
                    if (candidates.size() > 1 && ImGui::IsItemHovered())
                    {
                        std::string all = "Candidates (the total is measured, the breakdown is not):\n";
                        for (const auto& c : candidates) all += "  " + c.label + "\n";
                        ImGui::SetTooltip("%s", all.c_str());
                    }
                }
            }
        };

        drawTeam(m_team1PlayerIds);
        drawTeam(m_team2PlayerIds);
        ImGui::EndTable();
    }

    ImGui::End();
}

#endif // GWO_DEVELOPER
