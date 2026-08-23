#include "pch.h"
#include "ReplayWindow.h"
#include "EquipmentData.h"
#include "EquipmentIcons.h"
#include "ArmourNames.h"
#include "HealthModel.h"
#include "TextureCache.h"
#include "ReplayWindow_Internal.h"

// The Character panel: one player's equipment laid out the way the in-game inventory lays it out.
//
// Several can be open at once, because the question this answers is almost always comparative --
// why is that Warrior 60 health tankier than mine, what shield is he holding, has he given up 75
// health to a superior rune. So each panel is an independent instance with its own player, and the
// toolbar button opens another one rather than toggling a single window.
//
// What the recording actually carries, and what it does not, matters here more than anywhere else
// in the app:
//
//   * weapons and offhands arrive complete -- name, skin, dyes and every mod word
//   * armour pieces arrive as a skin and a dye, and nothing else: no name, and an EMPTY mod list
//
// That second point is not a gap in our parsing. The server never sends another player's runes and
// insignias, so the panel cannot show them as fact. What it shows instead is every rune and
// insignia combination that reaches the armour health we measured, which is the honest answer and
// the one HealthModel::ArmourCandidates exists to give.

namespace
{
    // Equipment slot numbers as they appear in the recording's EQUIP_SET events. Confirmed by
    // grouping every event against its item type: slot 0 only ever holds weapons, slot 1 only
    // shields and foci, and slots 2..6 hold exactly one armour piece each per player.
    constexpr uint8_t kSlotMainHand = 0;
    constexpr uint8_t kSlotOffHand  = 1;
    constexpr uint8_t kSlotChest    = 2;
    constexpr uint8_t kSlotLegs     = 3;
    constexpr uint8_t kSlotHead     = 4;
    constexpr uint8_t kSlotFeet     = 5;
    constexpr uint8_t kSlotHands    = 6;

    // The bag backdrop is one 256x256 sheet holding five curved bands stacked vertically -- the
    // same art the client uses behind a row of inventory slots. Each row of the panel takes one
    // band, so the five armour rows reproduce a full bag and the two weapon rows reuse the top of
    // one. Pixel coordinates, converted to UVs at draw time.
    constexpr float kBackdropSize = 256.f;
    constexpr float kBandTop[5]    = {   5.f,  52.f,  99.f, 146.f, 192.f };
    constexpr float kBandBottom[5] = {  46.f,  93.f, 140.f, 187.f, 233.f };

    constexpr float kSlotSize = 52.f;
    constexpr float kRowGap   = 6.f;

    // Guild Wars dye identifiers, as they arrive in ItemDef::dyes. Ids run 2..13 with 0 meaning
    // undyed; the sampled matches use every one of them except Gray. Swatches are eyeballed from
    // the client's dye bottles and exist to make the tooltip readable at a glance, not to
    // reproduce the shader.
    struct Dye { const char* name; ImU32 swatch; };

    const Dye* DyeInfo(uint8_t id)
    {
        static const Dye kDyes[] = {
            { "Blue",   IM_COL32(0x3A, 0x66, 0xC8, 0xFF) },
            { "Green",  IM_COL32(0x3E, 0x8E, 0x4A, 0xFF) },
            { "Purple", IM_COL32(0x7B, 0x4B, 0xA8, 0xFF) },
            { "Red",    IM_COL32(0xB8, 0x33, 0x33, 0xFF) },
            { "Yellow", IM_COL32(0xD8, 0xC0, 0x3A, 0xFF) },
            { "Brown",  IM_COL32(0x6B, 0x4A, 0x2E, 0xFF) },
            { "Orange", IM_COL32(0xD2, 0x76, 0x22, 0xFF) },
            { "Silver", IM_COL32(0xC0, 0xC6, 0xCC, 0xFF) },
            { "Black",  IM_COL32(0x2A, 0x2A, 0x2E, 0xFF) },
            { "Gray",   IM_COL32(0x80, 0x86, 0x8C, 0xFF) },
            { "White",  IM_COL32(0xEE, 0xF0, 0xF2, 0xFF) },
            { "Pink",   IM_COL32(0xD8, 0x74, 0xA8, 0xFF) },
        };
        if (id < 2 || id > 13) return nullptr;
        return &kDyes[id - 2];
    }

    // Art for this panel lives under Textures\Character pannel. FindTexturesDDSDir() is no use
    // here: it resolves Textures\DDS, which holds the ATEX conversions and nothing else.
    std::filesystem::path ArtDir()
    {
        static std::filesystem::path base;
        static bool searched = false;
        if (!searched)
        {
            searched = true;
            wchar_t exePath[MAX_PATH];
            if (GetModuleFileNameW(nullptr, exePath, MAX_PATH))
            {
                auto dir = std::filesystem::path(exePath).parent_path();
                for (int i = 0; i < 6; ++i)
                {
                    auto cand = dir / "Textures" / "Character pannel";
                    if (std::filesystem::is_directory(cand)) { base = cand; break; }
                    if (!dir.has_parent_path() || dir == dir.parent_path()) break;
                    dir = dir.parent_path();
                }
            }
        }
        return base;
    }

    ImTextureID Art(const std::string& file)
    {
        const auto dir = ArtDir();
        if (dir.empty()) return nullptr;
        return GetTextureCache().GetTexture((dir / file).string());
    }

    // The rune scrolls are 51x62 and the insignia is square, so drawing either into a square box
    // squashes it. Ask the texture how big it is rather than guessing.
    ImVec2 FitHeight(ImTextureID tex, float height)
    {
        float aspect = 1.f;
        if (auto* srv = (ID3D11ShaderResourceView*)tex)
        {
            Microsoft::WRL::ComPtr<ID3D11Resource> res;
            srv->GetResource(res.GetAddressOf());
            Microsoft::WRL::ComPtr<ID3D11Texture2D> tex2d;
            if (res && SUCCEEDED(res.As(&tex2d)) && tex2d)
            {
                D3D11_TEXTURE2D_DESC desc{};
                tex2d->GetDesc(&desc);
                if (desc.Height) aspect = (float)desc.Width / (float)desc.Height;
            }
        }
        return ImVec2(height * aspect, height);
    }

    ImTextureID Backdrop() { return Art("GW.EXE_0x4361D3FB.dds"); }

    struct SlotSpec { uint8_t slot; const char* label; };

    // Rune art, keyed the way the files are named: Rune_<who>_<tier>.png.
    //
    // Two families. The common runes -- Vigor and Vitae among them -- can go on any profession's
    // armour and share one icon, "All". Attribute runes are tied to the wearer's PRIMARY
    // profession, since that is where the attributes live, so those take his own art. That is also
    // the only thing about a rune we can state as fact: which profession's it must be. Everything
    // else on this panel's rune list is inferred from the health it adds.
    ImTextureID RuneIcon(const char* who, const char* tier)
    {
        return Art(std::format("Rune_{}_{}.png", who, tier));
    }

    ImTextureID SurvivorIcon() { return Art("Survivor_Insignia.png"); }

    const char* ProfessionArtName(int primaryProf)
    {
        switch (primaryProf)
        {
        case 1:  return "Warrior";
        case 2:  return "Ranger";
        case 3:  return "Monk";
        case 4:  return "Necromancer";
        case 5:  return "Mesmer";
        case 6:  return "Elementalist";
        case 7:  return "Assassin";
        case 8:  return "Ritualist";
        case 9:  return "Paragon";
        case 10: return "Dervish";
        default: return nullptr;
        }
    }

    // One rune or insignia in a build: the art to show, what it is called, what it is worth, and
    // the rarity its name is written in -- superior gold, major purple, minor blue, exactly as an
    // item tooltip colours the same words.
    struct BuildEntry
    {
        ImTextureID icon = nullptr;
        std::string name;
        int health = 0;
        Equipment::Rarity rarity = Equipment::Rarity::Blue;
    };

    ImVec4 RarityVec(Equipment::Rarity r)
    {
        const Equipment::Rgb c = Equipment::RarityColor(r);
        return ImVec4(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f);
    }

    std::vector<BuildEntry> BuildEntries(const HealthModel::ArmourBuild& b, int primaryProf)
    {
        const char* prof = ProfessionArtName(primaryProf);
        std::vector<BuildEntry> out;

        if (b.vigor)
        {
            const char* tier = b.vigor == 50 ? "Sup" : b.vigor == 41 ? "Major" : "Minor";
            const char* word = b.vigor == 50 ? "Superior" : b.vigor == 41 ? "Major" : "Minor";
            const auto rarity = b.vigor == 50 ? Equipment::Rarity::Gold
                              : b.vigor == 41 ? Equipment::Rarity::Purple
                                              : Equipment::Rarity::Blue;
            out.push_back({ RuneIcon("All", tier), std::format("{} Rune of Vigor", word), b.vigor, rarity });
        }
        if (b.vitae)
        {
            // Vitae has no tiers; the common art stands in for all of them.
            out.push_back({ RuneIcon("All", "Minor"),
                            b.vitae == 1 ? std::string("Rune of Vitae")
                                         : std::format("{} Runes of Vitae", b.vitae),
                            b.vitae * 10, Equipment::Rarity::Blue });
        }
        if (b.superiorRunes)
            out.push_back({ prof ? RuneIcon(prof, "Sup") : nullptr,
                            b.superiorRunes == 1 ? std::string("A superior rune")
                                                 : std::format("{} superior runes", b.superiorRunes),
                            -75 * b.superiorRunes, Equipment::Rarity::Gold });
        if (b.majorRunes)
            out.push_back({ prof ? RuneIcon(prof, "Major") : nullptr,
                            b.majorRunes == 1 ? std::string("A major rune")
                                              : std::format("{} major runes", b.majorRunes),
                            -35 * b.majorRunes, Equipment::Rarity::Purple });
        if (b.survivor)
            out.push_back({ SurvivorIcon(), "Survivor insignias", b.survivor,
                            Equipment::Rarity::Blue });

        return out;
    }
}

void ReplayWindow::OpenCharacterPanel(int agentId)
{
    // Default to whoever the camera is on, which is nearly always the player being asked about.
    if (agentId < 0) agentId = m_followedAgentId;
    if (agentId < 0 && !m_team1PlayerIds.empty()) agentId = m_team1PlayerIds.front();

    m_characterPanels.push_back({ m_nextCharacterPanelUid++, agentId, true });
}

void ReplayWindow::DrawCharacterPanels()
{
    if (m_characterPanels.empty()) return;

    const float t = m_debugTimeline;
    const Equipment::Data& equipment = m_replayCtx.stocData.equipment;
    ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;
    ImTextureID backdrop = Backdrop();

    const ImVec4 muted(0.48f, 0.50f, 0.53f, 1.f);

    // Weapons on the left, armour on the right, top to bottom as the client shows them.
    static const SlotSpec kWeaponSlots[] = {
        { kSlotMainHand, "Main hand" },
        { kSlotOffHand,  "Off hand"  },
    };
    static const SlotSpec kArmourSlots[] = {
        { kSlotHead,  "Head"  },
        { kSlotChest, "Chest" },
        { kSlotHands, "Hands" },
        { kSlotLegs,  "Legs"  },
        { kSlotFeet,  "Feet"  },
    };

    // One band of the bag sheet, stretched over a row.
    auto drawBand = [&](ImDrawList* dl, int band, ImVec2 tl, ImVec2 br) {
        if (!backdrop) return;
        const ImVec2 uv0(0.f, kBandTop[band] / kBackdropSize);
        const ImVec2 uv1(1.f, kBandBottom[band] / kBackdropSize);
        dl->AddImage(backdrop, tl, br, uv0, uv1);
    };

    auto drawItemTooltip = [&](const Equipment::ItemDef* item, const char* slotLabel) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(360.f);

        const auto toVec = [](Equipment::Rgb c) {
            return ImVec4(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f);
        };

        if (item && !item->name.empty())
        {
            const auto tip = Equipment::BuildTooltip(*item);
            for (const auto& line : tip.lines)
            {
                using Kind = Equipment::TooltipLine::Kind;
                switch (line.kind)
                {
                case Kind::Name:
                    ImGui::TextColored(toVec(Equipment::RarityColor(tip.rarity)), "%s", line.text.c_str());
                    break;
                case Kind::Stat:
                    ImGui::TextColored(toVec(Equipment::kStatColor), "%s", line.text.c_str());
                    if (!line.condition.empty()) { ImGui::SameLine(0.f, 6.f); ImGui::TextColored(muted, "%s", line.condition.c_str()); }
                    break;
                case Kind::Mod:
                    ImGui::TextColored(toVec(Equipment::ModTextColor(tip.rarity)), "%s", line.text.c_str());
                    if (!line.condition.empty()) { ImGui::SameLine(0.f, 6.f); ImGui::TextColored(muted, "%s", line.condition.c_str()); }
                    break;
                default:
                    if (!line.text.empty()) ImGui::TextColored(muted, "%s", line.text.c_str());
                    break;
                }
            }
        }
        else
        {
            // Armour: the recording carries a skin and a dye and nothing else, so the name comes
            // from the model id rather than from the packet.
            const ArmourNames::Piece* piece = item ? ArmourNames::Find(item->modelFileId) : nullptr;
            if (piece)
            {
                ImGui::TextUnformatted(piece->name);
                ImGui::TextColored(muted, "%s  -  %s", slotLabel, piece->campaign);
            }
            else
            {
                ImGui::TextUnformatted(slotLabel);
                if (item) ImGui::TextColored(muted, "Unknown skin %u", item->modelFileId);
            }
        }

        if (item)
        {
            const Dye* dye = DyeInfo(item->dyes[0]);
            ImGui::Separator();
            if (dye)
            {
                ImGui::ColorButton("##dye", ImColor(dye->swatch),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                                   ImVec2(12, 12));
                ImGui::SameLine(0.f, 6.f);
                ImGui::Text("Dyed %s", dye->name);
            }
            else ImGui::TextColored(muted, "Undyed");
        }

        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    };

    auto drawSlot = [&](ImDrawList* dl, ImVec2 tl, const AgentReplayData& ard,
                        const SlotSpec& spec, int band, float width) {
        const ImVec2 br(tl.x + width, tl.y + kSlotSize);
        drawBand(dl, band, tl, br);

        const Equipment::ItemDef* item = equipment.FindAtTime(ard.agent_id, spec.slot, t);

        const float icon = kSlotSize - 8.f;
        const ImVec2 iconTL(tl.x + (width - icon) * 0.5f, tl.y + 4.f);
        const ImVec2 iconBR(iconTL.x + icon, iconTL.y + icon);

        if (item)
        {
            // iconFileId, not modelFileId. A weapon's skin is its icon, but a worn armour piece
            // arrives as a COMPOSITE model id whose icon only exists once the client expands it --
            // an expansion that reads live game memory, so a replay can never do it. The recorder
            // resolves it and writes the answer; recordings older than that carry 0 for armour,
            // and those slots fall back to naming the piece.
            if (ImTextureID skin = EquipmentIcons::Get(m_datManager, dev, item->iconFileId))
            {
                dl->AddImage(skin, iconTL, iconBR);
            }
            else if (const ArmourNames::Piece* piece = ArmourNames::Find(item->modelFileId))
            {
                dl->AddRect(iconTL, iconBR, IM_COL32(255, 255, 255, 30), 3.f);
                ImGui::PushClipRect(iconTL, iconBR, true);
                ImGui::SetCursorScreenPos(ImVec2(iconTL.x + 3.f, iconTL.y + 3.f));
                ImGui::PushTextWrapPos(iconBR.x - 3.f);
                ImGui::TextColored(ImVec4(0.72f, 0.75f, 0.78f, 1.f), "%s", piece->name);
                ImGui::PopTextWrapPos();
                ImGui::PopClipRect();
            }
            else
            {
                dl->AddRect(iconTL, iconBR, IM_COL32(255, 255, 255, 40), 3.f);
            }
        }

        ImGui::SetCursorScreenPos(tl);
        ImGui::InvisibleButton(spec.label, ImVec2(width, kSlotSize));
        if (ImGui::IsItemHovered())
        {
            dl->AddRect(iconTL, iconBR, IM_COL32(255, 215, 100, 160), 3.f, 0, 1.5f);
            if (item) drawItemTooltip(item, spec.label);
            else      ImGui::SetTooltip("%s\nEmpty", spec.label);
        }
    };

    for (size_t p = 0; p < m_characterPanels.size(); ++p)
    {
        CharacterPanelInstance& panel = m_characterPanels[p];
        if (!panel.open) continue;

        auto it = m_replayCtx.agents.find(panel.agentId);
        const AgentReplayData* ard = (it != m_replayCtx.agents.end()) ? &it->second : nullptr;

        const std::string name = ard ? (ard->cachedLabel.empty() ? ard->playerName : ard->cachedLabel)
                                     : std::string("(no player)");
        const std::string title = std::format("Character - {}###char_panel_{}", name, panel.uid);

        ImGui::SetNextWindowSize(ImVec2(330, 430), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title.c_str(), &panel.open))
        {
            ImGui::End();
            continue;
        }

        // Player picker: a column per team, each row a profession icon and a name. Checkboxes
        // rather than a dropdown because the panel is used to flick between players while
        // comparing them, and a dropdown hides fifteen of the sixteen choices behind a click.
        {
            const float pickW = std::max(120.f, (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f);
            auto teamColumn = [&](const std::vector<int>& ids, const char* heading, ImVec4 colour) {
                ImGui::BeginGroup();
                ImGui::TextColored(colour, "%s", heading);
                for (int id : ids)
                {
                    auto ait = m_replayCtx.agents.find(id);
                    if (ait == m_replayCtx.agents.end()) continue;
                    const AgentReplayData& a = ait->second;
                    if (a.type != AgentType::Player) continue;

                    ImGui::PushID(id);
                    bool selected = (id == panel.agentId);
                    // One player per panel, so ticking a box moves the panel rather than adding to
                    // a set; unticking the current one would leave nothing to show, so it is a no-op.
                    if (ImGui::Checkbox("##pick", &selected) && selected) panel.agentId = id;

                    if (dev && a.primaryProf >= 1)
                    {
                        if (ImTextureID prof = LoadProfIcon(dev, a.primaryProf))
                        {
                            ImGui::SameLine(0.f, 4.f);
                            ImGui::Image(prof, ImVec2(16, 16));
                        }
                    }
                    ImGui::SameLine(0.f, 4.f);
                    ImGui::TextUnformatted(a.playerName.c_str());
                    ImGui::PopID();
                }
                ImGui::EndGroup();
            };

            teamColumn(m_team1PlayerIds, m_folderTag1.empty() ? "Red" : m_folderTag1.c_str(),
                       ImVec4(1.f, 0.42f, 0.42f, 1.f));
            ImGui::SameLine(0.f, 8.f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, pickW - ImGui::GetItemRectSize().x));
            teamColumn(m_team2PlayerIds, m_folderTag2.empty() ? "Blue" : m_folderTag2.c_str(),
                       ImVec4(0.29f, 0.78f, 1.f, 1.f));
        }

        ImGui::Separator();

        if (!ard)
        {
            ImGui::TextColored(muted, "Pick a player.");
            ImGui::End();
            continue;
        }

        ImGui::Spacing();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float colGap = 10.f;
        const float colW = std::max(72.f, (avail - colGap) * 0.5f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        for (int i = 0; i < (int)std::size(kWeaponSlots); ++i)
            drawSlot(dl, ImVec2(origin.x, origin.y + i * (kSlotSize + kRowGap)),
                     *ard, kWeaponSlots[i], i, colW);

        for (int i = 0; i < (int)std::size(kArmourSlots); ++i)
            drawSlot(dl, ImVec2(origin.x + colW + colGap, origin.y + i * (kSlotSize + kRowGap)),
                     *ard, kArmourSlots[i], i, colW);

        const int rows = (int)std::size(kArmourSlots);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (kSlotSize + kRowGap)));
        ImGui::Spacing();
        ImGui::Separator();

        // ── Runes ────────────────────────────────────────────────────────────────────────────
        ImGui::TextUnformatted("Runes");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Guild Wars never tells an observer which runes a player is wearing.\n\n"
                "What we can do is measure how much health his runes add, from the maximum\n"
                "health the camera recorded, and then list the rune sets that add exactly\n"
                "that much. So these are possibilities, not his actual runes.");

        if (!ard->armourSolved)
        {
            ImGui::TextColored(muted, "Not enough readings to measure this player's runes.");
        }
        else
        {
            ImGui::Text("His runes add %+d health.", ard->solvedArmourHealth);
            ImGui::SameLine();
            ImGui::TextDisabled("(from %d readings)", ard->armourObservations);

            const auto builds = HealthModel::ArmourCandidates(ard->solvedArmourHealth, 6);
            if (builds.empty())
            {
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                                   "No rune set adds %+d health.", ard->solvedArmourHealth);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Either a term of the health model is wrong at this player's\n"
                                      "readings, or the readings themselves were stale.");
            }
            else
            {
                // How sure we are is simply how many builds survive the ranking: one is an answer,
                // a handful is a shortlist. Never a single icon presented as fact.
                const ImVec4 confColour = builds.size() == 1 ? ImVec4(0.40f, 1.00f, 0.50f, 1.f)
                                        : builds.size() <= 3 ? ImVec4(1.00f, 0.85f, 0.40f, 1.f)
                                                             : muted;
                if (builds.size() == 1)
                    ImGui::TextColored(confColour, "Only one rune set adds that much:");
                else
                    ImGui::TextColored(confColour, "Most likely set (%d others also add %+d):",
                                       (int)builds.size() - 1, ard->solvedArmourHealth);
                ImGui::Spacing();

                // The best build gets the art; the rest stay as one-line alternatives.
                constexpr float kRuneIcon = 44.f;
                for (const auto& e : BuildEntries(builds.front(), ard->primaryProf))
                {
                    const ImVec2 size = e.icon ? FitHeight(e.icon, kRuneIcon)
                                               : ImVec2(kRuneIcon * 0.82f, kRuneIcon);
                    const float top = ImGui::GetCursorPosY();
                    if (e.icon) ImGui::Image(e.icon, size);
                    else        ImGui::Dummy(size);
                    ImGui::SameLine(0.f, 8.f);

                    const float text = ImGui::GetTextLineHeightWithSpacing();
                    ImGui::SetCursorPosY(top + std::max(0.f, (kRuneIcon - text * 2.f) * 0.5f));
                    ImGui::BeginGroup();
                    ImGui::TextColored(RarityVec(e.rarity), "%s", e.name.c_str());
                    if (e.health >= 0) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.60f, 1.f), "%+d health", e.health);
                    else               ImGui::TextColored(ImVec4(0.90f, 0.55f, 0.55f, 1.f), "%+d health", e.health);
                    ImGui::EndGroup();
                    ImGui::SetCursorPosY(top + kRuneIcon + 4.f);
                }

                if (builds.size() > 1)
                {
                    ImGui::Spacing();
                    if (ImGui::TreeNode("Other rune sets that add the same"))
                    {
                        for (size_t i = 1; i < builds.size(); ++i)
                            ImGui::BulletText("%s", builds[i].label.c_str());
                        ImGui::TreePop();
                    }
                }
            }
        }

        ImGui::End();
    }

    // Closing a panel drops it, so the vector holds only live ones and the ids stay unique.
    std::erase_if(m_characterPanels, [](const CharacterPanelInstance& c) { return !c.open; });
}
