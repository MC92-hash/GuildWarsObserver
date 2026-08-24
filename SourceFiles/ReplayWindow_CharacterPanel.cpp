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
    constexpr float kBandTop[5]    = {  14.f,  58.f, 102.f, 148.f, 194.f };
    constexpr float kBandBottom[5] = {  49.f,  95.f, 141.f, 185.f, 229.f };

    // Whole width, and drawn at one texel per pixel -- magnifying a 123-pixel crop across a 440
    // pixel bar was what made it look soft, not the compression.
    //
    // This sheet rather than texture_354850: measured across the alpha channel, every band here
    // holds 190-202 from edge to edge, while 354850's bottom two bands fade to zero on the left
    // where its bag flap sits. That one is the flap variant; this one is the clean tile.
    constexpr float kBandU0 = 0.f;

    // Cells are the client's size, and each row is a bar wide enough for its cells plus the name
    // beside them -- which is what keeps the bar art looking like a bar.
    constexpr float kCellSize   = 40.f;
    constexpr float kSetLabelW  = 44.f;   // room for "Set 1" ahead of the cells
    // The bands sit 45 texels apart on the sheet; matching that keeps the bar reading as one
    // continuous piece of art rather than five strips with arbitrary gaps.
    constexpr float kRowHeight  = 45.f;
    constexpr float kRowPitch   = 45.f;
    // Weapon sets and armour share one bar: main hand and offhand at its left end, the armour
    // piece at its right. Three cells is the whole width of it -- the bar art is a bar, and
    // stretching it across half a window turns it into a smear.
    constexpr float kCellGap  = 4.f;

    // The bar is exactly as wide as the sheet, so it draws one texel per pixel. Weapon sets hang
    // off its left end and armour off its right; the span between them is where the character
    // model belongs, the way the client's own equipment window is laid out.
    constexpr float kColItems   = 256.f;
    constexpr float kWeaponPad  = 5.f;
    constexpr float kArmourPad  = 5.f;
    constexpr float kNameW      = 190.f;   // armour names, just outside the bar
    constexpr float kBarInset   = 0.f;

    // Breathing room before the runes column; butted against the armour cell it read as one block.
    constexpr float kRunesGap = 28.f;

    // The gold the rest of the UI uses for section headings.
    constexpr ImU32 kHeadingGold = IM_COL32(225, 190, 80, 255);

    // The player list column. Names run from three letters to "Candyboy Timewaster", so the width
    // is measured against the names actually in the match rather than fixed and hopeful.
    constexpr float kPlayerRowH   = 17.f;
    constexpr float kPlayerIndent = 27.f;  // the ">" marker and the profession icon
    constexpr float kColPlayersMin = 130.f;
    constexpr float kColPlayersMax = 280.f;
    constexpr float kColRunes   = 250.f;

    // Guild Wars dye identifiers, as they arrive in ItemDef::dyes. Ids run 2..13 with 0 meaning
    // undyed; the sampled matches use every one of them except Gray. Swatches are eyeballed from
    // the client's dye bottles and exist to make the tooltip readable at a glance, not to
    // reproduce the shader.
    struct Dye { const char* name; ImU32 swatch; };

    // Dye bottles are 52x64, same portrait shape as the rune scrolls.
    constexpr float kDyeIconH = 30.f;
    constexpr float kDyeIconW = kDyeIconH * 52.f / 64.f;

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

    // Loads this panel's art against the DEVICE THAT WILL DRAW IT.
    //
    // The shared TextureCache cannot be used here. It is initialised once with the MapBrowser's
    // device, while a ReplayWindow renders on its own -- so its textures load perfectly and then
    // draw as nothing, because a shader resource view belongs to the device that made it. That is
    // why the backdrop never appeared and the rune scrolls came out as noise, and it is why
    // LoadProfIconGeneric, whose icons do work in this very panel, takes a device and drops its
    // cache whenever that device changes. This does the same.
    ImTextureID Art(ID3D11Device* device, const std::string& file)
    {
        static ID3D11Device* cachedDevice = nullptr;
        static std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> cache;
        if (!device) return nullptr;
        if (device != cachedDevice) { cache.clear(); cachedDevice = device; }

        auto it = cache.find(file);
        if (it != cache.end()) return (ImTextureID)it->second.Get();

        auto& slot = cache[file];   // remembers failures too, so a missing file is not retried
        const auto dir = ArtDir();
        if (dir.empty()) return nullptr;
        const auto path = dir / file;
        if (!std::filesystem::exists(path)) return nullptr;

        DirectX::ScratchImage image;
        const bool isDds = path.extension() == ".dds" || path.extension() == ".DDS";
        HRESULT hr = isDds
            ? DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image)
            : DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
        if (FAILED(hr)) return nullptr;

        const auto& meta = image.GetMetadata();
        if (!meta.width || !meta.height) return nullptr;

        // Block-compressed sources have to be decompressed; Convert refuses them outright.
        DirectX::ScratchImage converted;
        if (DirectX::IsCompressed(meta.format))
            hr = DirectX::Decompress(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM, converted);
        else if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
            hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
                                  DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;

        const DirectX::ScratchImage& src = converted.GetImageCount() ? converted : image;
        const auto* img = src.GetImage(0, 0, 0);

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = (UINT)img->width;
        desc.Height = (UINT)img->height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = img->pixels;
        init.SysMemPitch = (UINT)img->rowPitch;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        if (FAILED(device->CreateTexture2D(&desc, &init, tex.GetAddressOf()))) return nullptr;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(tex.Get(), &srvDesc, slot.GetAddressOf())))
            return nullptr;

        return (ImTextureID)slot.Get();
    }

    // Every rune scroll in Textures\Character pannel is 51x62 and the Survivor insignia is
    // 64x64. Drawing either into a square squashed the scrolls into unreadable smudges, so
    // keep the two shapes apart rather than querying the texture at draw time.
    constexpr float kRuneIconH = 48.f;
    ImVec2 RuneIconSize(bool square)
    {
        return square ? ImVec2(kRuneIconH, kRuneIconH)
                      : ImVec2(kRuneIconH * 51.f / 62.f, kRuneIconH);
    }

    ImTextureID Backdrop(ID3D11Device* d) { return Art(d, "GW.EXE_0x4361D3FB.dds"); }

    struct SlotSpec { uint8_t slot; const char* label; };

    // Rune art, keyed the way the files are named: Rune_<who>_<tier>.png.
    //
    // Two families. The common runes -- Vigor and Vitae among them -- go on any profession's armour
    // and share one icon, "All". Attribute runes are tied to the wearer's PRIMARY profession, since
    // that is where the attributes live, so those take his own art. Which profession's rune it must
    // be is the only thing about it we can state as fact; the rest is inferred from health.
    ImTextureID RuneIcon(ID3D11Device* d, const char* who, const char* tier)
    {
        return Art(d, std::format("Rune_{}_{}.png", who, tier));
    }

    ImTextureID SurvivorIcon(ID3D11Device* d) { return Art(d, "Survivor_Insignia.png"); }

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

    // One rune or insignia in a build: the art, what it is called, what it is worth, and the rarity
    // its name is written in -- superior gold, major purple, minor blue, as an item tooltip would.
    struct BuildEntry
    {
        ImTextureID icon = nullptr;
        std::string name;
        int health = 0;
        Equipment::Rarity rarity = Equipment::Rarity::Blue;
        bool square = false;   // the insignia art is square; the rune scrolls are not
    };

    ImVec4 RarityVec(Equipment::Rarity r)
    {
        const Equipment::Rgb c = Equipment::RarityColor(r);
        return ImVec4(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f);
    }

    std::vector<BuildEntry> BuildEntries(ID3D11Device* dev, const HealthModel::ArmourBuild& b,
                                         int primaryProf)
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
            out.push_back({ RuneIcon(dev, "All", tier),
                            std::format("{} Rune of Vigor", word), b.vigor, rarity });
        }
        if (b.vitae)
        {
            // Vitae has no tiers; the common art stands in for all of them.
            out.push_back({ RuneIcon(dev, "All", "Minor"),
                            b.vitae == 1 ? std::string("Rune of Vitae")
                                         : std::format("{} Runes of Vitae", b.vitae),
                            b.vitae * 10, Equipment::Rarity::Blue });
        }
        if (b.superiorRunes)
            out.push_back({ prof ? RuneIcon(dev, prof, "Sup") : nullptr,
                            b.superiorRunes == 1 ? std::string("A superior rune")
                                                 : std::format("{} superior runes", b.superiorRunes),
                            -75 * b.superiorRunes, Equipment::Rarity::Gold });
        if (b.majorRunes)
            out.push_back({ prof ? RuneIcon(dev, prof, "Major") : nullptr,
                            b.majorRunes == 1 ? std::string("A major rune")
                                              : std::format("{} major runes", b.majorRunes),
                            -35 * b.majorRunes, Equipment::Rarity::Purple });
        if (b.survivor)
            out.push_back({ SurvivorIcon(dev), "Survivor insignias", b.survivor,
                            Equipment::Rarity::Blue, true });

        return out;
    }

    // Item tooltip. A weapon arrives with a name and every mod word, so it gets the full
    // rarity-coloured tooltip an item deserves. Armour arrives as a skin and a dye and nothing
    // else, so it gets the set name from the model id and says what it is honestly.
    void DrawItemTooltip(ID3D11Device* dev, const Equipment::ItemDef& item,
                         const char* slotLabel, ImVec4 muted)
    {
        const auto toVec = [](Equipment::Rgb c) {
            return ImVec4(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f);
        };

        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(360.f);

        if (!item.name.empty())
        {
            const auto tip = Equipment::BuildTooltip(item);
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
        else if (const ArmourNames::Piece* piece = ArmourNames::Find(item.modelFileId))
        {
            ImGui::TextUnformatted(piece->name);
            ImGui::TextColored(muted, "%s  -  %s", slotLabel, piece->campaign);
        }
        else
        {
            ImGui::TextUnformatted(slotLabel);
            ImGui::TextColored(muted, "Unknown skin %u", item.modelFileId);
        }

        // Dyes last, as the bottles themselves. An item can carry up to four, applied in order,
        // so they read left to right the way the game lists them.
        bool anyDye = false;
        for (const uint8_t id : item.dyes)
        {
            const Dye* dye = DyeInfo(id);
            if (!dye) continue;
            if (!anyDye) { ImGui::Separator(); anyDye = true; }
            else           ImGui::SameLine(0.f, 4.f);

            if (ImTextureID tex = Art(dev, std::format("{}_Dye.png", dye->name)))
                ImGui::Image(tex, ImVec2(kDyeIconW, kDyeIconH));
            else
                ImGui::ColorButton("##dye", ImColor(dye->swatch),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                                   ImVec2(kDyeIconW, kDyeIconH));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", dye->name);
        }
        if (!anyDye)
        {
            ImGui::Separator();
            ImGui::TextColored(muted, "Undyed");
        }

        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
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
    ImTextureID backdrop = Backdrop(dev);

    const ImVec4 muted(0.48f, 0.50f, 0.53f, 1.f);

    static const SlotSpec kArmourSlots[] = {
        { kSlotHead,  "Head"  },
        { kSlotChest, "Chest" },
        { kSlotHands, "Hands" },
        { kSlotLegs,  "Legs"  },
        { kSlotFeet,  "Feet"  },
    };

    // One band of the bag sheet behind a row. The art is a wide, shallow bar -- roughly six to one
    // -- so it only reads as a bar while the row it fills is wide and shallow too. Stretched across
    // a half-window column it turned into a glow, which is what the first build did.
    auto drawBand = [&](ImDrawList* dl, int band, ImVec2 tl, float width, float rowHeight) {
        if (!backdrop) return;
        band %= 5;
        const float bandH = kBandBottom[band] - kBandTop[band];
        const float srcW  = kBackdropSize * (1.f - kBandU0);
        const float drawH = std::min(rowHeight, width * bandH / srcW);
        const float y = tl.y + (rowHeight - drawH) * 0.5f;
        dl->AddImage(backdrop, ImVec2(tl.x, y), ImVec2(tl.x + width, y + drawH),
                     ImVec2(kBandU0, kBandTop[band] / kBackdropSize),
                     ImVec2(1.f, kBandBottom[band] / kBackdropSize));
    };

    // A single item cell: the bevelled square the client draws every equipment slot in.
    auto drawCell = [&](ImDrawList* dl, ImVec2 tl, const Equipment::ItemDef* item,
                        const char* emptyLabel, const char* slotLabel) {
        const ImVec2 br(tl.x + kCellSize, tl.y + kCellSize);
        dl->AddRectFilled(tl, br, IM_COL32(18, 26, 24, 210), 2.f);
        dl->AddRect(tl, br, IM_COL32(118, 138, 128, 200), 2.f);

        if (item)
        {
            if (ImTextureID skin = EquipmentIcons::Get(m_datManager, dev, item->iconFileId))
                dl->AddImage(skin, ImVec2(tl.x + 2.f, tl.y + 2.f), ImVec2(br.x - 2.f, br.y - 2.f));
        }

        ImGui::SetCursorScreenPos(tl);
        ImGui::InvisibleButton(slotLabel, ImVec2(kCellSize, kCellSize));
        if (ImGui::IsItemHovered())
        {
            dl->AddRect(tl, br, IM_COL32(255, 215, 100, 200), 2.f, 0, 1.6f);
            if (item) DrawItemTooltip(dev, *item, slotLabel, muted);
            else      ImGui::SetTooltip("%s\n%s", slotLabel, emptyLabel);
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

        // Same chrome as the notepad and morale panels: one look across the app's windows.
        ImGui::PushStyleColor(ImGuiCol_WindowBg,             ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
        ImGui::PushStyleColor(ImGuiCol_TitleBg,              ImVec4(0.07f, 0.08f, 0.10f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,        ImVec4(0.10f, 0.09f, 0.06f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border,               ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,              ImVec4(0.04f, 0.05f, 0.06f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          ImVec4(1.f, 1.f, 1.f, 0.04f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        ImVec4(0.80f, 0.68f, 0.30f, 0.60f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1.f, 0.84f, 0.39f, 0.80f));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  ImVec4(1.f, 0.84f, 0.39f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,    6.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,  1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 4.f);

        ImGui::SetNextWindowSize(ImVec2(kColPlayersMin + kColItems + kNameW + kRunesGap + kColRunes + 90.f, 400.f),
                                 ImGuiCond_FirstUseEver);
        const bool shown = ImGui::Begin(title.c_str(), &panel.open);
        if (!shown)
        {
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(9);
            continue;
        }

        // Player list down the left, always visible. A collapsing header hid the one control the
        // panel has behind a click, and a dropdown hid fifteen of the sixteen choices; comparing
        // two players means switching often enough that neither is worth the width it saves.
        float colPlayers = kColPlayersMin;
        for (const auto& ids : { &m_team1PlayerIds, &m_team2PlayerIds })
            for (int id : *ids)
            {
                auto ait = m_replayCtx.agents.find(id);
                if (ait == m_replayCtx.agents.end() || ait->second.type != AgentType::Player) continue;
                colPlayers = std::max(colPlayers,
                                      kPlayerIndent + ImGui::CalcTextSize(ait->second.playerName.c_str()).x + 12.f);
            }
        colPlayers = std::min(colPlayers, kColPlayersMax);

        ImGui::BeginChild("##players", ImVec2(colPlayers, 0.f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        {
            auto teamList = [&](const std::vector<int>& ids, const char* heading, ImU32 colour) {
                ImGui::PushStyleColor(ImGuiCol_Text, colour);
                ImGui::TextUnformatted(heading);
                ImGui::PopStyleColor();

                for (int id : ids)
                {
                    auto ait = m_replayCtx.agents.find(id);
                    if (ait == m_replayCtx.agents.end()) continue;
                    const AgentReplayData& a = ait->second;
                    if (a.type != AgentType::Player) continue;

                    ImGui::PushID(id);
                    const bool active = (id == panel.agentId);

                    // The row is the control: a selectable spanning the column, with the icon and
                    // name drawn over it, so the whole strip is clickable rather than a tick box.
                    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
                    if (ImGui::Selectable("##row", active, 0, ImVec2(0.f, kPlayerRowH)))
                        panel.agentId = id;

                    ImDrawList* rdl = ImGui::GetWindowDrawList();
                    float x = rowPos.x + 2.f;
                    if (active)
                    {
                        rdl->AddText(ImVec2(x, rowPos.y + 1.f), kHeadingGold, ">");
                        x += 10.f;
                    }
                    else x += 10.f;

                    if (dev && a.primaryProf >= 1)
                    {
                        if (ImTextureID prof = LoadProfIcon(dev, a.primaryProf))
                        {
                            rdl->AddImage(prof, ImVec2(x, rowPos.y + 1.f),
                                          ImVec2(x + 14.f, rowPos.y + 15.f));
                        }
                    }
                    x += 17.f;
                    rdl->AddText(ImVec2(x, rowPos.y + 1.f),
                                 active ? kHeadingGold : IM_COL32(216, 220, 226, 255),
                                 a.playerName.c_str());
                    ImGui::PopID();
                }
            };

            teamList(m_team1PlayerIds, m_folderTag1.empty() ? "Red" : m_folderTag1.c_str(),
                     IM_COL32(255, 107, 107, 255));
            ImGui::Spacing();
            teamList(m_team2PlayerIds, m_folderTag2.empty() ? "Blue" : m_folderTag2.c_str(),
                     IM_COL32(74, 200, 255, 255));
        }
        ImGui::EndChild();
        ImGui::SameLine(0.f, 8.f);

        if (!ard)
        {
            ImGui::TextColored(muted, "Pick a player.");
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(9);
            continue;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        // ── Weapon sets and armour, sharing one bar per row ──────────────────────────────────
        //
        // One bar, not two: the client's bag rows run the full width of the window and this reads
        // as one piece of furniture rather than two lists that happen to be side by side. Weapon
        // sets hang off the left end of each bar and armour off the right, so the two columns stay
        // legible while the art stays whole.
        if (m_hudWeaponSets.agentId != ard->agent_id)
            BuildWeaponSets(ard->agent_id, m_hudWeaponSets);

        if (m_hudWeaponSets.agentId != ard->agent_id)
            BuildWeaponSets(ard->agent_id, m_hudWeaponSets);

        // Headings in the UI's gold, each over what it names.
        const float barX    = origin.x;
        const float mainX   = barX + kWeaponPad;
        const float offX    = mainX + kCellSize + kCellGap;
        const float armourX = barX + kColItems - kCellSize - kArmourPad;

        dl->AddText(ImVec2(mainX, origin.y), kHeadingGold, "Weapon sets");
        dl->AddText(ImVec2(armourX - 6.f, origin.y), kHeadingGold, "Armour");

        const float rowTop   = origin.y + ImGui::GetTextLineHeightWithSpacing();
        const int   setCount = (int)m_hudWeaponSets.sets.size();
        const int   rows     = std::max(setCount, (int)std::size(kArmourSlots));

        for (int i = 0; i < rows; ++i)
        {
            const ImVec2 tl(barX, rowTop + i * kRowPitch);
            drawBand(dl, i, tl, kColItems, kRowHeight);

            const float cellY = tl.y + (kRowHeight - kCellSize) * 0.5f;
            const float textY = tl.y + kRowHeight * 0.5f - 7.f;

            // One weapon set, with its mods spelled out in the column to the left of the bar.
            if (i < setCount)
            {
                const auto& ws = m_hudWeaponSets.sets[i];
                const Equipment::ItemDef* main = equipment.FindByAgentItemId(ws.mainId);
                const Equipment::ItemDef* off  = equipment.FindByAgentItemId(ws.offId);

                ImGui::PushID(1000 + i);
                drawCell(dl, ImVec2(mainX, cellY), main, "no weapon", "Main hand");
                drawCell(dl, ImVec2(offX, cellY), off, "no offhand", "Off hand");
                ImGui::PopID();

            }

            // One armour piece, its name running to the right end of the bar.
            if (i < (int)std::size(kArmourSlots))
            {
                const Equipment::ItemDef* item =
                    equipment.FindAtTime(ard->agent_id, kArmourSlots[i].slot, t);

                ImGui::PushID(2000 + i);
                drawCell(dl, ImVec2(armourX, cellY), item, "not worn", kArmourSlots[i].label);
                ImGui::PopID();

                const ArmourNames::Piece* piece = item ? ArmourNames::Find(item->modelFileId) : nullptr;
                dl->AddText(ImVec2(barX + kColItems + 10.f, textY),
                            IM_COL32(232, 236, 242, 255),
                            piece ? piece->name : kArmourSlots[i].label);
            }
        }

        // ── Runes, beside the armour they were measured from ─────────────────────────────────
        const float runesX = barX + kColItems + kNameW + kRunesGap;
        dl->AddText(ImVec2(runesX, origin.y), kHeadingGold, "Runes");
        ImGui::SetCursorScreenPos(ImVec2(runesX + ImGui::CalcTextSize("Runes").x + 6.f, origin.y));
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Guild Wars never tells an observer which runes a player is wearing.\n\n"
                "What we can do is measure how much health his runes add, from the maximum\n"
                "health the camera recorded, and then list the rune sets that add exactly\n"
                "that much. So these are possibilities, not his actual runes.");

        ImGui::SetCursorScreenPos(ImVec2(runesX, rowTop));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(runesX + kColRunes - 16.f);

        if (!ard->armourSolved)
        {
            ImGui::TextColored(muted, "Not enough readings to measure this player's runes.");
        }
        else
        {
            ImGui::Text("%+d Max health added from runes", ard->solvedArmourHealth);
            ImGui::TextDisabled("measured from %d readings", ard->armourObservations);
            ImGui::Spacing();

            const auto builds = HealthModel::ArmourCandidates(ard->solvedArmourHealth, 6);
            if (builds.empty())
            {
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                                   "No rune set adds %+d health.", ard->solvedArmourHealth);
            }
            else
            {
                if (builds.size() == 1) ImGui::TextDisabled("Only one possible setup:");
                else                    ImGui::TextDisabled("Most likely setup (out of %d guesses)",
                                                    (int)builds.size());

                for (const auto& e : BuildEntries(dev, builds.front(), ard->primaryProf))
                {
                    ImGui::BeginGroup();
                    if (e.icon) ImGui::Image(e.icon, RuneIconSize(e.square));
                    else        ImGui::Dummy(RuneIconSize(e.square));
                    ImGui::EndGroup();

                    ImGui::SameLine(0.f, 8.f);
                    ImGui::BeginGroup();
                    ImGui::TextColored(RarityVec(e.rarity), "%s", e.name.c_str());
                    ImGui::TextColored(e.health >= 0 ? ImVec4(0.55f, 0.85f, 0.60f, 1.f)
                                                     : ImVec4(0.90f, 0.55f, 0.55f, 1.f),
                                       "%+d health", e.health);
                    ImGui::EndGroup();
                }

                if (builds.size() > 1 && ImGui::TreeNode("Other sets"))
                {
                    for (size_t i = 1; i < builds.size(); ++i)
                        ImGui::BulletText("%s", builds[i].label.c_str());
                    ImGui::TreePop();
                }
            }
        }

        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        // Reserve the space the rows were drawn into, so the window sizes to its content.
        ImGui::SetCursorScreenPos(ImVec2(origin.x, rowTop + rows * kRowPitch + 4.f));
        ImGui::Dummy(ImVec2(kColItems + kNameW + kRunesGap + kColRunes, 1.f));

        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(9);
    }

    // Closing a panel drops it, so the vector holds only live ones and the ids stay unique.
    std::erase_if(m_characterPanels, [](const CharacterPanelInstance& c) { return !c.open; });
}
