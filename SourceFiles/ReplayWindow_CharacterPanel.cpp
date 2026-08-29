#include "pch.h"
#include "ReplayWindow.h"
#include "EquipmentData.h"
#include "EquipmentIcons.h"
#include "ArmourNames.h"
#include "HealthModel.h"
#include "AttributeModel.h"
#include "SkillDatabase.h"
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
//
// The attribute block under the sheet is the other side of that same coin. A rank above 12 can only
// have come from a rune, so the ranks say which runes must be on the armour, while the health says
// what the armour paid for them - and the two are worth reading together, in one panel, because
// either one alone leaves the reader guessing.

// Lays out a sentence made of differently coloured spans, wrapping it by hand. ImGui wraps
// per item, so drawing each green number as an item of its own would restart the wrap
// wherever that item began - the same thing that turned the evidence conclusions into a
// two-character column against the right margin.
ImVec2 DrawSkillTextRuns(ImDrawList* dl, ImVec2 pos, float wrapW, const std::vector<SkillTextRun>& runs)
{
    const float lineH = ImGui::GetTextLineHeight();
    const float spaceW = ImGui::CalcTextSize(" ").x;
    float x = pos.x, y = pos.y, widest = 0.f;

    for (const SkillTextRun& run : runs)
    {
        size_t i = 0;
        while (i < run.text.size())
        {
            if (run.text[i] == '\n') { x = pos.x; y += lineH; ++i; continue; }
            if (run.text[i] == ' ')  { if (x > pos.x) x += spaceW; ++i; continue; }

            size_t j = run.text.find_first_of(" \n", i);
            if (j == std::string::npos) j = run.text.size();
            const std::string word = run.text.substr(i, j - i);
            const float w = ImGui::CalcTextSize(word.c_str()).x;
            if (x > pos.x && x + w > pos.x + wrapW) { x = pos.x; y += lineH; }
            dl->AddText(ImVec2(x, y), run.colour, word.c_str());
            x += w;
            widest = std::max(widest, x - pos.x);
            i = j;
        }
    }
    return ImVec2(widest, y + lineH - pos.y);
}

// The description, with every attribute range answered at the rank this player was read at.
std::vector<SkillTextRun> BuildSkillTextRuns(const SkillInfo& si,
                                   const AttributeModel::AttributeRange* rank,
                                   ImU32 plain, ImU32 answered)
{
    // The client's own markup means nothing outside the game's renderer.
    std::string text = si.description.empty() ? si.concise : si.description;
    for (size_t pos; (pos = text.find('<')) != std::string::npos; )
    {
        const size_t end = text.find('>', pos);
        if (end == std::string::npos) break;
        text.erase(pos, end - pos + 1);
    }

    std::vector<SkillTextRun> runs;
    if (!rank) { runs.push_back({ text, plain }); return runs; }

    size_t at = 0;
    while (at < text.size())
    {
        const size_t dots = text.find("...", at);
        if (dots == std::string::npos) break;

        // Back over the first number and forward over the second.
        size_t a = dots;
        while (a > 0 && std::isdigit((unsigned char)text[a - 1])) --a;
        size_t b = dots + 3;
        while (b < text.size() && std::isdigit((unsigned char)text[b])) ++b;
        if (a == dots || b == dots + 3) { at = dots + 3; continue; }

        const float v0  = (float)std::atoi(text.substr(a, dots - a).c_str());
        const float v15 = (float)std::atoi(text.substr(dots + 3, b - dots - 3).c_str());
        const int lo = AttributeModel::Breakpoint(v0, v15, std::clamp(rank->lo, 0, 16));
        const int hi = AttributeModel::Breakpoint(v0, v15, std::clamp(rank->hi, 0, 16));

        runs.push_back({ text.substr(at, a - at), plain });
        runs.push_back({ lo == hi ? std::to_string(lo)
                                  : std::format("{}-{}", std::min(lo, hi), std::max(lo, hi)),
                         answered });
        at = b;
    }
    runs.push_back({ text.substr(at), plain });
    return runs;
}

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

    // A hairline between the parts of the sheet. The panel holds four different kinds of thing -
    // who he is, what he pressed, what he wore, what he spent - and without a rule between them
    // they run together into one long column.
    constexpr ImU32 kSheetRule = IM_COL32(255, 255, 255, 26);
    inline void SheetRule(ImDrawList* dl, float x, float y, float w)
    {
        dl->AddLine(ImVec2(x, y), ImVec2(x + w, y), kSheetRule);
    }

    // The player list column. Names run from three letters to "Candyboy Timewaster", so the width
    // is measured against the names actually in the match rather than fixed and hopeful.
    constexpr float kPlayerRowH   = 20.f;
    constexpr float kPlayerIndent = 30.f;  // the accent edge, the profession icon and the gaps
    constexpr float kPlayerSlotW  = 22.f;  // the party number, right-aligned in its own column
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
    // Everything the panel draws lives under one Textures folder, found once by walking up from
    // the exe: the bag sheet and rune scrolls under "Character pannel", the skill art under
    // "skills", the energy and recharge glyphs under "Game_UI\Skill Description".
    std::filesystem::path TexturesDir()
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
                    if (std::filesystem::is_directory(dir / "Textures")) { base = dir / "Textures"; break; }
                    if (!dir.has_parent_path() || dir == dir.parent_path()) break;
                    dir = dir.parent_path();
                }
            }
        }
        return base;
    }

    std::filesystem::path ArtDir()
    {
        const auto root = TexturesDir();
        return root.empty() ? root : root / "Character pannel";
    }

    // Loads this panel's art against the DEVICE THAT WILL DRAW IT.
    //
    // The shared TextureCache cannot be used here. It is initialised once with the MapBrowser's
    // device, while a ReplayWindow renders on its own -- so its textures load perfectly and then
    // draw as nothing, because a shader resource view belongs to the device that made it. That is
    // why the backdrop never appeared and the rune scrolls came out as noise, and it is why
    // LoadProfIconGeneric, whose icons do work in this very panel, takes a device and drops its
    // cache whenever that device changes. This does the same.
    ImTextureID ArtFromPath(ID3D11Device* device, const std::filesystem::path& path)
    {
        static ID3D11Device* cachedDevice = nullptr;
        static std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> cache;
        if (!device) return nullptr;
        if (device != cachedDevice) { cache.clear(); cachedDevice = device; }

        const std::string key = path.string();
        auto it = cache.find(key);
        if (it != cache.end()) return (ImTextureID)it->second.Get();

        auto& slot = cache[key];   // remembers failures too, so a missing file is not retried
        std::error_code ec;
        if (path.empty() || !std::filesystem::exists(path, ec)) return nullptr;

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

    ImTextureID Art(ID3D11Device* device, const std::string& file)
    {
        const auto dir = ArtDir();
        return dir.empty() ? nullptr : ArtFromPath(device, dir / file);
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

    // Skill art, from Textures\skills\<id>.jpg. It goes through the panel's own loader for the
    // same reason the rune scrolls do: the shared TextureCache belongs to the MapBrowser's
    // device, and a view made there draws as nothing here.
    //
    // A PvP split has no file of its own because it wears the icon of the skill it was split
    // from, and only the PvE row carries the link, so the way back is an index built once.
    ImTextureID SkillIcon(ID3D11Device* device, int skillId, const SkillDatabaseView& skills)
    {
        if (skillId <= 0) return nullptr;

        const auto root = TexturesDir();
        if (root.empty()) return nullptr;
        const auto folder = root / "skills";

        static std::unordered_map<int, int> pvpToBase;
        if (pvpToBase.empty())
            skills.ForEachSkill([](const SkillInfo& si) {
                if (si.pvp_split && si.split_id > 0) pvpToBase[si.split_id] = si.id;
            });

        auto back = pvpToBase.find(skillId);
        const int candidates[2] = { skillId, back == pvpToBase.end() ? skillId : back->second };
        for (int k = 0; k < 2; ++k)
        {
            if (k == 1 && candidates[1] == candidates[0]) break;
            if (ImTextureID tex = ArtFromPath(device,
                    folder / (std::to_string(candidates[k]) + ".jpg")))
                return tex;
        }
        return nullptr;
    }

    // The "(?)" that hangs off a heading, drawn as an ImGui item over the heading the draw list
    // already put down so that it can be hovered.
    ImVec4 RarityVec(Equipment::Rarity r);   // defined with the rune list, below

    // A tooltip's opening word. The bold face the browser uses has no atlas on this panel's
    // device and comes out as empty boxes, so the heading is the ordinary font in the panel's
    // gold instead.
    void TipHeading(const char* label)
    {
        ImGui::TextColored(ImVec4(0.88f, 0.75f, 0.31f, 1.f), "%s", label);
        ImGui::SameLine(0.f, 5.f);
    }

    void HelpMarker(ImVec2 headingPos, const char* heading, void (*body)())
    {
        ImGui::SetCursorScreenPos(ImVec2(headingPos.x + ImGui::CalcTextSize(heading).x + 6.f,
                                         headingPos.y));
        ImGui::TextDisabled("(?)");
        if (!ImGui::IsItemHovered()) return;

        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(470.f);
        body();
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    void TipWeaponSets()
    {
        TipHeading("Weapon sets:");
        ImGui::TextUnformatted("Recorded directly from the game data: weapon skins, upgrades, "
                               "and which set was active.");
    }

    void TipArmour()
    {
        TipHeading("Armour:");
        ImGui::TextUnformatted("Armour pieces and dye colors as sent by the game. Runes are "
                               "shown in their own section since they aren't part of the armour "
                               "packet and are deducted by our tool.");
    }

    void TipRunes()
    {
        const ImVec4 minor = RarityVec(Equipment::Rarity::Blue);
        const ImVec4 major = RarityVec(Equipment::Rarity::Purple);
        const ImVec4 head  = ImVec4(0.69f, 0.58f, 0.29f, 1.f);   // the headgear's own gold

        TipHeading("Runes:");
        ImGui::TextUnformatted("The game doesn't send rune data, so we infer it from what's "
                               "visible in the recording.");
        ImGui::TextUnformatted("You'll see the likely rune setup based on the agent's health "
                               "(recorded value) and attributes (calculated).");
        ImGui::TextUnformatted("Some builds can look identical, so in rare cases the displayed "
                               "setup may be one of several possibilities.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Example: a Monk showing Protection 14 and Divine Favor 14 could "
                               "be using either:");

        ImGui::Bullet();
        ImGui::TextUnformatted("Protection Prayers: 12");
        ImGui::SameLine(0.f, 0.f);
        ImGui::TextColored(major, "+2");
        ImGui::SameLine(0.f, 4.f);
        ImGui::TextUnformatted("or 12");
        ImGui::SameLine(0.f, 0.f);
        ImGui::TextColored(minor, "+1");
        ImGui::SameLine(0.f, 0.f);
        ImGui::TextColored(head, "+1");

        ImGui::Bullet();
        ImGui::TextUnformatted("Divine Favor: 12");
        ImGui::SameLine(0.f, 0.f);
        ImGui::TextColored(minor, "+1");
        ImGui::SameLine(0.f, 0.f);
        ImGui::TextColored(head, "+1");
        ImGui::SameLine(0.f, 4.f);
        ImGui::TextUnformatted("or 12");
        ImGui::SameLine(0.f, 0.f);
        ImGui::TextColored(major, "+2");

        ImGui::TextUnformatted("Both look the same in-game.");
    }

    void TipAttributes()
    {
        TipHeading("Attributes:");
        ImGui::TextUnformatted("The game doesn't send attribute values so we cannot record them. "
                               "These values are our best estimate of the player's attribute "
                               "ranks.");
        ImGui::Spacing();
        ImGui::TextUnformatted("We look at how each skill behaved in the recording - how much it "
                               "healed, how much damage it dealt, how long it lasted, how much "
                               "energy it returned - and from that we infer the most likely "
                               "attribute level.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Every character has a fixed pool of attribute points. That budget "
                               "limits how high the unseen attributes can be, so when a skill "
                               "suggests \"this should be at least N\", we show it as <= N if the "
                               "remaining budget can't support anything higher.");
        ImGui::Spacing();
        ImGui::TextUnformatted("If different skills point to slightly different values, we show a "
                               "range instead of pretending it's exact.");
        ImGui::TextUnformatted("Missing data is shown as missing, never guessed.");
    }

    ImTextureID SurvivorIcon(ID3D11Device* d)  { return Art(d, "Survivor_Insignia.png"); }
    ImTextureID StonefistIcon(ID3D11Device* d) { return Art(d, "Stonefist_Insignia.png"); }

    const char* ProfessionName(int prof)
    {
        switch (prof)
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
        default: return "";
        }
    }

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

        // What an attribute rune is FOR. A minor rune costs no health at all, so "+0 health" was
        // the whole of what the panel had to say about one - true, and useless. The rank it buys
        // is the reason it is on the armour.
        std::string gain;
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
        // Runes the attribute solve could put a name to. The art was always per profession and
        // tier and still is; what the ranks add is which attribute the rune sits on -- and, for a
        // minor rune, that it is there at all, since it costs no health for the armour to reveal.
        int namedSuperiors = 0, namedMajors = 0;
        for (const auto& [attr, tier] : b.namedRunes)
        {
            const char* art  = tier >= 3 ? "Sup"      : tier == 2 ? "Major" : "Minor";
            const char* word = tier >= 3 ? "Superior" : tier == 2 ? "Major" : "Minor";
            const auto rarity = tier >= 3 ? Equipment::Rarity::Gold
                              : tier == 2 ? Equipment::Rarity::Purple
                                          : Equipment::Rarity::Blue;
            if (tier >= 3)      ++namedSuperiors;
            else if (tier == 2) ++namedMajors;

            out.push_back({ prof ? RuneIcon(dev, prof, art) : nullptr,
                            std::format("{} Rune of {}", word,
                                        SkillDatabase::GetAttributeName(attr)),
                            tier >= 3 ? -75 : tier == 2 ? -35 : 0, rarity, false,
                            std::format("+{} {}", tier,
                                        SkillDatabase::GetAttributeName(attr)) });
        }

        // Whatever the ranks could not account for stays anonymous: health alone cannot say which
        // attribute a rune belongs to.
        const int restSuperiors = std::max(0, b.superiorRunes - namedSuperiors);
        const int restMajors    = std::max(0, b.majorRunes - namedMajors);
        if (restSuperiors)
            out.push_back({ prof ? RuneIcon(dev, prof, "Sup") : nullptr,
                            restSuperiors == 1 ? std::string("A superior rune")
                                               : std::format("{} superior runes", restSuperiors),
                            -75 * restSuperiors, Equipment::Rarity::Gold });
        if (restMajors)
            out.push_back({ prof ? RuneIcon(dev, prof, "Major") : nullptr,
                            restMajors == 1 ? std::string("A major rune")
                                            : std::format("{} major runes", restMajors),
                            -35 * restMajors, Equipment::Rarity::Purple });
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

    // The attribute block, under the sheet.
    //
    // What the solver can state, in the words it can state it in: an exact rank where the packets
    // pinned one, a range where they only narrowed it, and a ceiling where nothing was observed at
    // all and the 200-point rule is the whole answer. Never a bare number that hides which of the
    // three it is.

    // The colour says how much the number is worth: full white for a rank the evidence pinned, a
    // shade down for a range, muted for a bound the budget alone imposed.
    constexpr ImU32 kAttrExact  = IM_COL32(232, 236, 242, 255);
    constexpr ImU32 kAttrRanged = IM_COL32(196, 202, 210, 255);
    constexpr ImU32 kAttrMarker = IM_COL32(176, 148, 74, 255);   // dim gold, for the rune markers
    constexpr float kAttrHeadGap = 10.f;   // between the last bag row and the heading

    // Every bar is drawn against the same ceiling, or two players' columns cannot be compared.
    // Sixteen, not twelve: attribute points stop at twelve and runes carry the rest, and a bar
    // that ended at twelve would show a runed attribute as full whatever the rune was.
    constexpr int kBarCeiling = 16;

    // The attribute a character's FIRST profession grants him, which is also the one the game's own
    // attribute window puts at the top of his list.
    int PrimaryAttributeOf(int primaryProf)
    {
        switch (primaryProf)
        {
        case 1:  return 17;  // Warrior       Strength
        case 2:  return 23;  // Ranger        Expertise
        case 3:  return 16;  // Monk          Divine Favor
        case 4:  return 6;   // Necromancer   Soul Reaping
        case 5:  return 0;   // Mesmer        Fast Casting
        case 6:  return 12;  // Elementalist  Energy Storage
        case 7:  return 35;  // Assassin      Critical Strikes
        case 8:  return 36;  // Ritualist     Spawning Power
        case 9:  return 40;  // Paragon       Leadership
        case 10: return 44;  // Dervish       Mysticism
        default: return -1;
        }
    }

    // How a rank was reached, written the way the game writes it: the points, then what the
    // armour added. "12+1+1" is twelve points, a minor rune and the headgear - which is a rank 14
    // and reads as one, where "14 +min +head" made the reader do the arithmetic backwards. The
    // rune's own number carries the colour of its rarity, blue, purple or gold.
    struct RankMarks
    {
        std::string base;          // the rank the attribute points bought
        std::string rune;          // "+1" / "+2" / "+3", in the rune's colour
        std::string head;          // "+1" for the headgear
        ImU32       runeColour = 0;
    };

    RankMarks RuneMarks(const AttributeModel::PlayerBuild& build, int attrId,
                        const AttributeModel::AttributeRange& range)
    {
        RankMarks m;
        int base = range.best;

        auto tier = build.runeTier.find(attrId);
        if (tier != build.runeTier.end() && tier->second > 0)
        {
            base -= tier->second;
            m.rune = std::format("+{}", tier->second);
            const Equipment::Rgb c = Equipment::RarityColor(
                tier->second >= 3 ? Equipment::Rarity::Gold
                                  : tier->second == 2 ? Equipment::Rarity::Purple
                                                      : Equipment::Rarity::Blue);
            m.runeColour = IM_COL32(c.r, c.g, c.b, 255);
        }
        if (build.headgearAttribute == attrId) { base -= 1; m.head = "+1"; }

        // The points paid for what the armour did not, so the base carries the whole width of the
        // range: "12-14+1+1" is a rank the evidence pinned to 14-16, of which two came from the
        // armour. A bound the points alone imposed never has a rune to split off.
        const int lo = range.lo - (range.best - base);
        const int hi = range.hi - (range.best - base);
        if (!m.rune.empty() || !m.head.empty())
        {
            if (range.budgetOnly || lo < 0)
            {
                m.rune.clear();
                m.head.clear();
            }
            else
            {
                m.base = (lo == hi) ? std::to_string(lo) : std::format("{}-{}", lo, hi);
            }
        }
        return m;
    }

    // The quantity a reading turned on, in the green the panel uses for a measured number.
    const ImVec4 kReadingGreen = ImVec4(136 / 255.f, 255 / 255.f, 136 / 255.f, 1.f);

    // ── The skill card ──────────────────────────────────────────────────────────────────────
    //
    // The same face the library page puts on a skill - icon, type, the cost glyphs, the
    // description - plus one thing the library cannot do. This panel knows what the caster's
    // attribute was, so every "5...50" in the text is answered with the number he actually played
    // it at, in green. A rank the tool could only bound reads as a range, because that is what it
    // is, and a skill on an attribute that was never solved keeps the game's own wording.
    constexpr float kSkillCardWidth = 340.f;

    ImTextureID CostIcon(ID3D11Device* dev, const char* file)
    {
        const auto root = TexturesDir();
        return root.empty() ? nullptr
                            : ArtFromPath(dev, root / "Game_UI" / "Skill Description" / file);
    }

    void CostItem(ID3D11Device* dev, const char* file, const std::string& value, bool& any)
    {
        if (any) ImGui::SameLine(0.f, 10.f);
        if (ImTextureID tex = CostIcon(dev, file))
        {
            ImGui::Image(tex, ImVec2(16.f, 16.f));
            ImGui::SameLine(0.f, 3.f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.f);
        }
        ImGui::TextUnformatted(value.c_str());
        any = true;
    }

    // Quarters of a second, said exactly - the same rule the rest of the app prints times by.
    std::string TimeText(float seconds)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), SkillTimeFormat(seconds), seconds);
        return buf;
    }

    void DrawSkillCard(ID3D11Device* dev, const SkillInfo& si, const SkillDatabaseView& skills,
                       const AttributeModel::PlayerBuild* build, const ImVec4& muted)
    {
        ImGui::BeginTooltip();

        constexpr float kIconH = 40.f;
        if (ImTextureID icon = SkillIcon(dev, si.id, skills)) ImGui::Image(icon, ImVec2(kIconH, kIconH));
        else                                                  ImGui::Dummy(ImVec2(kIconH, kIconH));
        ImGui::SameLine(0.f, 8.f);

        ImGui::BeginGroup();
        if (si.is_elite) ImGui::TextColored(ImVec4(1.f, 0.85f, 0.30f, 1.f), "{Elite} %s", si.name.c_str());
        else             ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 1.f), "%s", si.name.c_str());

        const char* typeName = SkillDatabase::GetTypeName(si.type);
        const char* attrName = SkillDatabase::GetAttributeName(si.attribute);
        if (attrName && attrName[0]) ImGui::TextColored(muted, "%s  (%s)", typeName, attrName);
        else                         ImGui::TextColored(muted, "%s", typeName);
        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Cost, cast and recharge, in the game's own glyphs.
        {
            bool any = false;
            if (si.energy > 0)     CostItem(dev, "energy.png",     std::to_string(si.energy), any);
            if (si.adrenaline > 0) CostItem(dev, "adrenaline.png", std::to_string(si.adrenaline), any);
            if (si.sacrifice > 0)  CostItem(dev, "sacrifice.png",  std::format("{}%", si.sacrifice), any);
            if (si.overcast > 0)   CostItem(dev, "overcast.png",   std::to_string(si.overcast), any);
            if (si.upkeep < 0)     CostItem(dev, "upkeep.png",     std::to_string(-si.upkeep), any);
            if (si.activation > 0) CostItem(dev, "activation.png", TimeText(si.activation), any);
            if (si.recharge > 0)   CostItem(dev, "recharge.png",   TimeText(si.recharge), any);
            if (any) ImGui::Spacing();
        }

        // The rank this player was read at, if his solve covers the attribute the skill scales on.
        const AttributeModel::AttributeRange* rank = nullptr;
        if (build)
            if (auto it = build->attributes.find(si.attribute); it != build->attributes.end())
                if (!it->second.budgetOnly) rank = &it->second;

        const std::vector<SkillTextRun> runs =
            BuildSkillTextRuns(si, rank, ImGui::GetColorU32(ImVec4(0.85f, 0.82f, 0.75f, 1.f)),
                          ImGui::GetColorU32(kReadingGreen));

        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 size = DrawSkillTextRuns(ImGui::GetWindowDrawList(), at, kSkillCardWidth, runs);
        ImGui::Dummy(ImVec2(kSkillCardWidth, size.y));

        if (rank)
        {
            ImGui::Spacing();
            ImGui::TextColored(muted, "Green is this player's own %s, read off the match.",
                               SkillDatabase::GetAttributeName(si.attribute));
        }

        ImGui::EndTooltip();
    }

    // Which of a skill's scales the reading came off, so the table under it is the right table.
    bool ScaleForGenre(const SkillInfo& si, AttributeModel::Genre genre, float& v0, float& v15)
    {
        using G = AttributeModel::Genre;
        auto firstOfKind = [&](SkillScaleKind kind) {
            for (const SkillScale& sc : si.scales)
                if (sc.kind == kind && sc.v15 != sc.v0) { v0 = sc.v0; v15 = sc.v15; return true; }
            return false;
        };

        switch (genre)
        {
        case G::CombatDamage: case G::CombatHeal: case G::CombatLifeSteal:
            if (si.deductionUsable && si.dedV15 != si.dedV0)
            { v0 = si.dedV0; v15 = si.dedV15; return true; }
            return false;
        case G::EnergyGain: case G::EnergyLoss: case G::ChantRefund:
            if (si.energyUsable && si.enV15 != si.enV0)
            { v0 = si.enV0; v15 = si.enV15; return true; }
            return false;
        case G::SummonLevel:
            return firstOfKind(SkillScaleKind::Level);
        case G::AvatarForm: case G::ConditionDuration: case G::MovementWindow:
        case G::AttackSpeedWindow: case G::SnareWindow: case G::BlockWindow:
        case G::StanceWindow:
        case G::WeaponSpellEpisode: case G::HexEpisode: case G::EnchantmentEpisode:
        case G::EnchantmentUptime:
            return firstOfKind(SkillScaleKind::Duration);
        default:
            return false;
        }
    }

    // What the numbers in a skill's progression are, so the table can name its own second row.
    const char* QuantityName(AttributeModel::Genre genre)
    {
        using G = AttributeModel::Genre;
        switch (genre)
        {
        case G::CombatDamage:                        return "Damage";
        case G::CombatHeal:                          return "Healing";
        case G::CombatLifeSteal:                     return "Health stolen";
        case G::EnergyGain: case G::ChantRefund:     return "Energy gained";
        case G::EnergyLoss:                          return "Energy lost";
        case G::SummonLevel:                         return "Level";
        default:                                     return "Duration";
        }
    }

    // What a primary attribute does on its own, at each rank, as the wiki publishes it. Ten
    // attributes have an effect of their own; every other attribute does nothing but scale the
    // skills that name it, and so has no table of its own to show.
    const char* PrimaryEffectName(int attrId)
    {
        switch (attrId)
        {
        case 0:  return "Casting speed";        // Fast Casting
        case 6:  return "Energy per death";     // Soul Reaping
        case 12: return "Max Energy";           // Energy Storage
        case 16: return "Healing";              // Divine Favor
        case 17: return "Armour penetration";   // Strength
        case 23: return "Energy cost";          // Expertise
        case 35: return "Critical chance";      // Critical Strikes
        case 36: return "Health and duration";  // Spawning Power
        case 40: return "Energy per ally";      // Leadership
        case 44: return "Energy cost";          // Mysticism
        default: return nullptr;
        }
    }

    std::string PrimaryEffectAt(int attrId, int rank)
    {
        switch (attrId)
        {
        // Casting time is halved at 15, which is the same as saying the speed doubles.
        case 0:  return std::format("{}%", (int)std::lround(100.0 * std::pow(2.0, rank / 15.0)));
        case 6:  return std::to_string(rank);
        case 12: return std::format("+{}", rank * 3);
        case 16: return std::to_string((int)std::lround(3.2 * rank));
        case 17: return std::format("{}%", rank);
        case 23: return std::format("{}%", 100 - 4 * rank);
        case 35: return std::format("{}%", rank);
        case 36: return std::format("+{}%", rank * 4);
        case 40: return std::to_string(rank / 2);
        case 44: return std::format("{}%", 100 - 4 * rank);
        default: return std::string();
        }
    }

    // A progression table, drawn the way the wiki draws one: the ranks along the top, what they
    // buy underneath, and the ranks in question picked out. A reader can then see the arithmetic
    // instead of being asked to trust it.
    template <typename ValueFn>
    void DrawRankTable(const char* tableId, const char* rowLabel, const char* quantityLabel,
                       ValueFn valueAt, uint32_t marked, const ImVec4& muted)
    {
        int lo = 17, hi = -1;
        for (int r = 0; r <= 16; ++r)
            if (marked & (1u << r)) { lo = std::min(lo, r); hi = std::max(hi, r); }
        if (hi < lo) return;

        // A window around the reading, wide enough to show it is not alone on the table.
        lo = std::max(0, lo - 2);
        hi = std::min(16, hi + 2);
        if (hi - lo > 8) hi = lo + 8;

        // Every column is sized for the widest thing that goes in it, and said so up front. Left
        // to work it out, the table is squeezed by the tooltip around it and the last value ends
        // up half-drawn.
        const float pad = ImGui::GetStyle().CellPadding.x * 2.f + 4.f;
        const float labelW = std::max(ImGui::CalcTextSize(rowLabel).x,
                                      ImGui::CalcTextSize(quantityLabel).x) + pad;

        std::vector<std::string> values;
        values.reserve((size_t)(hi - lo + 1));
        float cellW = 0.f;
        for (int r = lo; r <= hi; ++r)
        {
            values.push_back(valueAt(r));
            cellW = std::max(cellW,
                             std::max(ImGui::CalcTextSize(std::to_string(r).c_str()).x,
                                      ImGui::CalcTextSize(values.back().c_str()).x));
        }
        cellW += pad;

        const ImU32 hit = ImGui::GetColorU32(ImVec4(0.20f, 0.42f, 0.24f, 0.55f));

        if (!ImGui::BeginTable(tableId, hi - lo + 2,
                               ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit |
                               ImGuiTableFlags_NoHostExtendX))
            return;

        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelW);
        for (int r = lo; r <= hi; ++r)
            ImGui::TableSetupColumn("##r", ImGuiTableColumnFlags_WidthFixed, cellW);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(muted, "%s", rowLabel);
        for (int r = lo; r <= hi; ++r)
        {
            ImGui::TableNextColumn();
            const bool on = (marked & (1u << r)) != 0;
            if (on) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, hit);
            ImGui::TextColored(on ? kReadingGreen : muted, "%d", r);
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(muted, "%s", quantityLabel);
        for (int r = lo; r <= hi; ++r)
        {
            ImGui::TableNextColumn();
            const bool on = (marked & (1u << r)) != 0;
            if (on) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, hit);
            ImGui::TextColored(on ? kReadingGreen : muted, "%s", values[(size_t)(r - lo)].c_str());
        }

        ImGui::EndTable();
    }

    void DrawProgression(const SkillInfo& si, const AttributeModel::Evidence& ev,
                         float v0, float v15, const ImVec4& muted)
    {
        DrawRankTable("prog", SkillDatabase::GetAttributeName(si.attribute),
                      QuantityName(ev.genre),
                      [&](int r) { return std::to_string(AttributeModel::Breakpoint(v0, v15, r)); },
                      ev.ranks, muted);
    }

    // One reading: the skill's own icon, the sentence with the measured number picked out, and
    // the skill's progression underneath with the rank it points at lit up.
    void DrawEvidenceLine(ID3D11Device* dev, const AttributeModel::Evidence& ev,
                          const SkillDatabaseView& skills, const ImVec4& muted)
    {
        const SkillInfo* si = ev.skillId > 0 ? skills.Get(ev.skillId) : nullptr;

        const float iconH = ImGui::GetTextLineHeight() * 1.35f;
        if (ImTextureID icon = SkillIcon(dev, ev.skillId, skills))
            ImGui::Image(icon, ImVec2(iconH, iconH));
        else
            ImGui::Dummy(ImVec2(iconH, iconH));
        if (si && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", si->name.c_str());
        ImGui::SameLine(0.f, 6.f);

        ImGui::BeginGroup();

        // Split at the arrow, so the observation reads apart from the conclusion drawn from it.
        const std::string line = AttributeModel::Describe(ev, skills);
        const size_t arrow = line.find(" -> ");
        std::string body = (arrow == std::string::npos) ? line : line.substr(0, arrow);
        const std::string tail = (arrow == std::string::npos) ? std::string()
                                                              : line.substr(arrow + 4);

        // The icon already says which skill it was, so the name is dropped from the sentence.
        if (si && body.rfind(si->name, 0) == 0)
        {
            body.erase(0, si->name.size());
            if (!body.empty() && body.front() == ' ') body.erase(0, 1);
            if (!body.empty()) body[0] = (char)std::toupper((unsigned char)body[0]);
        }

        // Green marks the number that was MEASURED, which is not always the first one in the
        // sentence: "block healed 4 allies for 25" measures the 25, and pointing at the 4 says
        // the reading came from counting allies. The measurement is on the evidence itself, so
        // the sentence is searched for the number that matches it rather than read left to right.
        size_t numStart = std::string::npos, numEnd = std::string::npos;
        {
            double closest = 0.51;   // half a unit: near enough to be the same number, printed
            for (size_t i = 0; i < body.size(); )
            {
                if (!std::isdigit((unsigned char)body[i])) { ++i; continue; }
                size_t j = i;
                while (j < body.size() &&
                       (std::isdigit((unsigned char)body[j]) || body[j] == '.'))
                    ++j;

                // "(x4)" is how many times the reading was taken, never the reading.
                const bool isCount = (i >= 2 && body[i - 1] == 'x' && body[i - 2] == '(');
                if (!isCount)
                {
                    const double gap = std::fabs(std::atof(body.substr(i, j - i).c_str()) -
                                                 (double)ev.value);
                    if (gap < closest) { closest = gap; numStart = i; numEnd = j; }
                }
                i = j;
            }

            // Nothing in the sentence is the measurement - a rule that states no number, or one
            // that rounded it away. Fall back to the first number, which is what it used to be.
            if (numStart == std::string::npos)
            {
                numStart = body.find_first_of("0123456789");
                numEnd = numStart;
                while (numEnd != std::string::npos && numEnd < body.size() &&
                       (std::isdigit((unsigned char)body[numEnd]) || body[numEnd] == '.'))
                    ++numEnd;
            }
        }

        if (numStart != std::string::npos)
        {
            ImGui::TextUnformatted(body.substr(0, numStart).c_str());
            ImGui::SameLine(0.f, 0.f);
            ImGui::TextColored(kReadingGreen, "%s",
                               body.substr(numStart, numEnd - numStart).c_str());
            ImGui::SameLine(0.f, 0.f);
            ImGui::TextUnformatted(body.substr(numEnd).c_str());
        }
        else
        {
            ImGui::TextUnformatted(body.c_str());
        }

        // The conclusion goes on a line of its own. Hung off the end of the sentence it starts
        // wherever the sentence happened to finish, and ImGui restarts every wrapped line at that
        // same x - which is how "at least Fire Magic 15-16" comes out as a six-line column two
        // characters wide against the right margin.
        if (!tail.empty())
            ImGui::TextColored(ImVec4(0.88f, 0.75f, 0.31f, 1.f), "-> %s", tail.c_str());

        float v0 = 0.f, v15 = 0.f;
        if (si && ScaleForGenre(*si, ev.genre, v0, v15))
        {
            ImGui::PopTextWrapPos();          // a table must not wrap mid-cell
            DrawProgression(*si, ev, v0, v15, muted);
            ImGui::PushTextWrapPos(470.f);
        }

        ImGui::EndGroup();
    }

    void DrawAttributeTooltip(ID3D11Device* dev, int attrId,
                              const AttributeModel::AttributeRange& range,
                              const SkillDatabaseView& skills, const ImVec4& muted)
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(470.f);

        ImGui::TextColored(ImVec4(0.88f, 0.75f, 0.31f, 1.f), "%s %s",
                           AttributeModel::FormatRange(range).c_str(),
                           SkillDatabase::GetAttributeName(attrId));

        if (range.budgetOnly || range.why.empty())
            ImGui::TextColored(muted, "Never seen in use. This is only what the 200 attribute "
                                      "points leave over once the others are paid for.");

        // What the attribute itself is worth, before any skill names it. Only the ten primaries
        // have such a thing, and for their owners it is the first question - a Monk's Divine
        // Favor is read as health on every spell he casts, not as a number.
        if (const char* effect = PrimaryEffectName(attrId))
        {
            uint32_t settled = 0;
            for (int r = std::max(0, range.lo); r <= std::min(16, range.hi); ++r)
                settled |= (1u << r);
            if (settled)
            {
                ImGui::Spacing();
                ImGui::PopTextWrapPos();          // a table must not wrap mid-cell
                DrawRankTable("primary", SkillDatabase::GetAttributeName(attrId), effect,
                              [&](int r) { return PrimaryEffectAt(attrId, r); }, settled, muted);
                ImGui::PushTextWrapPos(470.f);
            }
        }

        // Strongest reading first, so the one that settled the answer is not read last.
        //
        // Three tiers: a reading that agrees with the answer, then one taken under a condition
        // that excludes it from scoring, then one that contradicts it outright. Within a tier the
        // order is decisiveness - the model's own weight over how many ranks the reading still
        // leaves open - so an exact packet leads a floor that only says "15 or 16".
        const auto spreadOf = [](uint32_t ranks) {
            int n = 0;
            for (; ranks; ranks &= ranks - 1) ++n;
            return n > 0 ? n : 1;
        };
        const auto tierOf = [&](const AttributeModel::Evidence& ev) {
            if (ev.suspicious) return 1;
            return (ev.ranks & (1u << range.best)) ? 0 : 2;
        };

        std::vector<const AttributeModel::Evidence*> order;
        order.reserve(range.why.size());
        for (const AttributeModel::Evidence& ev : range.why) order.push_back(&ev);
        std::stable_sort(order.begin(), order.end(),
            [&](const AttributeModel::Evidence* a, const AttributeModel::Evidence* b) {
                const int ta = tierOf(*a), tb = tierOf(*b);
                if (ta != tb) return ta < tb;
                const float da = a->weight / (float)spreadOf(a->ranks);
                const float db = b->weight / (float)spreadOf(b->ranks);
                if (da != db) return da > db;
                return a->count > b->count;
            });

        for (const AttributeModel::Evidence* ev : order)
        {
            ImGui::Spacing();
            DrawEvidenceLine(dev, *ev, skills, muted);
        }

        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // Primary profession first with its own attribute at the top, then the secondary, each group
    // strongest rank first - the order a player reads his own template window in.
    std::vector<int> AttributeOrder(const AttributeModel::PlayerBuild& build, int primaryProf)
    {
        const int primaryAttr = PrimaryAttributeOf(primaryProf);

        std::vector<int> ids;
        ids.reserve(build.attributes.size());
        for (const auto& [attrId, range] : build.attributes) ids.push_back(attrId);

        std::sort(ids.begin(), ids.end(), [&](int a, int b) {
            const auto key = [&](int attr) {
                const int group = SkillDatabase::GetProfessionForAttribute(attr) == primaryProf ? 0 : 1;
                const int first = (attr == primaryAttr) ? 0 : 1;
                return std::tuple{ group, first, -build.attributes.at(attr).best, attr };
            };
            return key(a) < key(b);
        });
        return ids;
    }

    // Draws the block and returns the y it finished at, so the panel can size its window to what
    // was drawn rather than to what it hoped would be.
    float DrawAttributeBlock(ID3D11Device* dev, ImDrawList* dl, ImVec2 topLeft, float width,
                             const AttributeModel::PlayerBuild* build, int primaryProf,
                             const SkillDatabaseView& skills, const ImVec4& muted)
    {
        const ImU32 mutedU32 = ImGui::GetColorU32(muted);
        const float lineH = ImGui::GetTextLineHeight() + 3.f;

        // The game puts the rank first, in a frame of its own, with the attribute beside it, and
        // the panel now reads the same way round: one column of numbers down the left, one column
        // of names beside it, and what the armour added spelled out between them with air around
        // it. The frame is sized for the widest thing that can go in it ("12-14"), so the names
        // line up whatever each rank costs in width.
        const float boxW    = ImGui::CalcTextSize("00-00").x + 12.f;
        const float markGap = 6.f;
        const float splitW  = ImGui::CalcTextSize("00-00+0+0").x;
        const float nameX   = topLeft.x + boxW + markGap + splitW + 12.f;
        const ImU32 frameU32 = IM_COL32(120, 128, 140, 190);

        // The rank a rune helped reach is not the rank his points bought, and the panel says so
        // in the colour of the total as well as in the sum beside it.
        const ImU32 kRunedTotal = IM_COL32(68, 170, 238, 255);

        float y = topLeft.y;
        dl->AddText(ImVec2(topLeft.x, y), kHeadingGold, "Attributes");
        HelpMarker(ImVec2(topLeft.x, y), "Attributes", TipAttributes);
        y += ImGui::GetTextLineHeightWithSpacing();

        if (!build || build->attributes.empty())
        {
            dl->AddText(ImVec2(topLeft.x, y), mutedU32, "Attributes: nothing observed yet.");
            return y + lineH;
        }

        for (int attrId : AttributeOrder(*build, primaryProf))
        {
            const AttributeModel::AttributeRange& range = build->attributes.at(attrId);
            const char* name = SkillDatabase::GetAttributeName(attrId);
            if (!name || !name[0]) name = "Unknown";

            const RankMarks marks = RuneMarks(*build, attrId, range);
            const std::string total = AttributeModel::FormatRange(range);
            const ImU32 colour = range.budgetOnly ? mutedU32
                               : (range.lo == range.hi) ? kAttrExact : kAttrRanged;

            // The box holds what he plays at; the sum beside it says where it came from - the
            // points he bought, then the rune in its own rarity's colour, then the headgear.
            const float totalW = ImGui::CalcTextSize(total.c_str()).x;
            const ImU32 totalColour = marks.rune.empty() ? colour : kRunedTotal;

            dl->AddRect(ImVec2(topLeft.x, y - 1.f),
                        ImVec2(topLeft.x + boxW, y + lineH - 2.f), frameU32, 2.f);
            dl->AddText(ImVec2(topLeft.x + (boxW - totalW) * 0.5f, y), totalColour, total.c_str());

            // Nothing to spell out when the points bought the whole rank: "12  12" reads as a
            // mistake, where "14  12+1+1" reads as an answer.
            if (!marks.base.empty())
            {
                float x = topLeft.x + boxW + markGap;
                dl->AddText(ImVec2(x, y), colour, marks.base.c_str());
                x += ImGui::CalcTextSize(marks.base.c_str()).x;
                if (!marks.rune.empty())
                {
                    dl->AddText(ImVec2(x, y), marks.runeColour, marks.rune.c_str());
                    x += ImGui::CalcTextSize(marks.rune.c_str()).x;
                }
                if (!marks.head.empty())
                    dl->AddText(ImVec2(x, y), kAttrMarker, marks.head.c_str());
            }

            dl->AddText(ImVec2(nameX, y), colour, name);

            // A bar the width of the rank, so a column of them reads at a glance. A range is
            // drawn twice - solid up to the lowest rank it can be, ghosted out to the highest -
            // so the eye takes in the floor and the room left above it at once. An attribute
            // never seen in use has no floor at all and is all ghost, which is the honest
            // picture of it.
            const float barW   = std::min(150.f, width * 0.36f);
            const float barX   = topLeft.x + width - barW;
            const float barTop = y + 2.f;
            const float barBot = y + lineH - 5.f;
            dl->AddRectFilled(ImVec2(barX, barTop), ImVec2(barX + barW, barBot),
                              IM_COL32(255, 255, 255, 16), 2.f);

            const float solidW = barW * std::clamp(range.lo / (float)kBarCeiling, 0.f, 1.f);
            const float ghostW = barW * std::clamp(range.hi / (float)kBarCeiling, 0.f, 1.f);
            if (ghostW > solidW)
                dl->AddRectFilled(ImVec2(barX + solidW, barTop), ImVec2(barX + ghostW, barBot),
                                  IM_COL32(126, 200, 130, 55), 2.f);
            if (solidW > 0.f)
                dl->AddRectFilled(ImVec2(barX, barTop), ImVec2(barX + solidW, barBot),
                                  IM_COL32(126, 200, 130, 190), 2.f);

            // Where attribute points stop paying: anything past twelve came out of a rune.
            const float runeTick = barX + barW * 12.f / (float)kBarCeiling;
            dl->AddLine(ImVec2(runeTick, barTop), ImVec2(runeTick, barBot),
                        IM_COL32(255, 255, 255, 45));

            // Hover for the observations behind the number, the same way the item cells do it: an
            // invisible button over what the draw list already put on screen.
            ImGui::PushID(attrId);
            ImGui::SetCursorScreenPos(ImVec2(topLeft.x, y - 1.f));
            ImGui::InvisibleButton("##attr", ImVec2(width, lineH));
            if (ImGui::IsItemHovered()) DrawAttributeTooltip(dev, attrId, range, skills, muted);
            ImGui::PopID();

            y += lineH;
        }

        // What is LEFT of the 200, which is the half of it worth reading and the half the game's
        // own attribute window shows in its title. A build with points to spare is not a fact
        // about the character - every GvG player spends all of them - it is a statement that one
        // of the ranks above is read too low, and the number says how much room there is for it
        // to be wrong. Nothing to spare, nothing to say.
        const int unusedLo = 200 - build->pointsSpentHi;
        const int unusedHi = 200 - build->pointsSpentLo;
        const std::string points =
            (unusedHi <= 0)      ? std::string("all 200 attribute points spent")
          : (unusedLo == unusedHi) ? std::format("{} unused attribute points", unusedLo)
                                   : std::format("{}-{} unused attribute points", unusedLo,
                                                 unusedHi);
        dl->AddText(ImVec2(topLeft.x, y + 2.f), mutedU32, points.c_str());
        y += lineH + 2.f;

        // Contradictions are output, not noise: they say the observations and the rules disagree,
        // and which of the two a reader should distrust.
        for (const std::string& line : build->contradictions)
        {
            ImGui::SetCursorScreenPos(ImVec2(topLeft.x, y));
            ImGui::PushTextWrapPos(topLeft.x + width - ImGui::GetWindowPos().x);
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", line.c_str());
            ImGui::PopTextWrapPos();
            y += ImGui::GetItemRectSize().y + 2.f;
        }

        return y;
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

        ImGui::SetNextWindowSize(ImVec2(kColPlayersMin + kColItems + kNameW + kRunesGap + kColRunes + 90.f, 620.f),
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
                                      kPlayerIndent + ImGui::CalcTextSize(ait->second.playerName.c_str()).x
                                          + kPlayerSlotW + 12.f);
            }
        colPlayers = std::min(colPlayers, kColPlayersMax);

        ImGui::BeginChild("##players", ImVec2(colPlayers, 0.f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        {
            // The selection is a filled row with an accent down its left edge, not a caret in
            // front of the name. A marker that takes width of its own shunts every name sideways
            // the moment the selection moves, and the eye reads that shift before it reads the
            // row. ImGui's own blue selection is pushed out of the way for the same reason: it
            // belongs to a different palette than the rest of this panel.
            ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.f, 0.f, 0.f, 0.f));

            const float rowW = colPlayers - 10.f;

            auto teamList = [&](const std::vector<int>& ids, const char* heading, ImU32 colour) {
                // The guild's name over a rule in its own colour, so two teams of eight read as
                // two blocks rather than one list of sixteen.
                const ImVec2 headPos = ImGui::GetCursorScreenPos();
                ImGui::PushStyleColor(ImGuiCol_Text, colour);
                ImGui::TextUnformatted(heading);
                ImGui::PopStyleColor();

                ImDrawList* rdl = ImGui::GetWindowDrawList();
                const float ruleY = headPos.y + ImGui::GetTextLineHeight() + 2.f;
                rdl->AddLine(ImVec2(headPos.x, ruleY), ImVec2(headPos.x + rowW, ruleY),
                             (colour & 0x00FFFFFFu) | 0x55000000u);
                ImGui::Dummy(ImVec2(0.f, 3.f));

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
                    const bool hovered = ImGui::IsItemHovered();

                    if (active || hovered)
                        rdl->AddRectFilled(rowPos, ImVec2(rowPos.x + rowW, rowPos.y + kPlayerRowH),
                                           active ? IM_COL32(225, 190, 80, 30)
                                                  : IM_COL32(255, 255, 255, 14), 3.f);
                    if (active)
                        rdl->AddRectFilled(ImVec2(rowPos.x, rowPos.y + 2.f),
                                           ImVec2(rowPos.x + 2.5f, rowPos.y + kPlayerRowH - 2.f),
                                           kHeadingGold, 1.f);

                    float x = rowPos.x + 9.f;
                    if (dev && a.primaryProf >= 1)
                        if (ImTextureID prof = LoadProfIcon(dev, a.primaryProf))
                            rdl->AddImage(prof, ImVec2(x, rowPos.y + 3.f),
                                          ImVec2(x + 14.f, rowPos.y + 17.f));
                    x += 18.f;

                    rdl->AddText(ImVec2(x, rowPos.y + 3.f),
                                 active ? kHeadingGold : IM_COL32(216, 220, 226, 255),
                                 a.playerName.c_str());

                    // The party slot, right-aligned in a column of its own: it is how callers
                    // name a player, and pinned to the right it never moves the name about.
                    if (a.playerNumber > 0)
                    {
                        const std::string slot = std::to_string(a.playerNumber);
                        rdl->AddText(ImVec2(rowPos.x + rowW - 6.f -
                                                ImGui::CalcTextSize(slot.c_str()).x,
                                            rowPos.y + 3.f),
                                     active ? IM_COL32(225, 190, 80, 150)
                                            : IM_COL32(255, 255, 255, 70),
                                     slot.c_str());
                    }
                    ImGui::PopID();
                }
            };

            teamList(m_team1PlayerIds, m_folderTag1.empty() ? "Red" : m_folderTag1.c_str(),
                     IM_COL32(255, 107, 107, 255));
            ImGui::Spacing();
            teamList(m_team2PlayerIds, m_folderTag2.empty() ? "Blue" : m_folderTag2.c_str(),
                     IM_COL32(74, 200, 255, 255));

            ImGui::PopStyleColor(3);
        }
        ImGui::EndChild();

        // The rule between who is being looked at and what is being looked at.
        ImGui::SameLine(0.f, 14.f);
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(p.x - 7.f, p.y), ImVec2(p.x - 7.f, p.y + ImGui::GetContentRegionAvail().y),
                kSheetRule);
        }

        if (!ard)
        {
            ImGui::TextColored(muted, "Pick a player.");
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(9);
            continue;
        }

        // The panel reads in three columns: who is being looked at, the character sheet, and
        // what the tool had to work out for itself. Runes and insignias are that third column and
        // not part of the sheet, because the recording never sends them - keeping the deduction
        // in a column of its own is the difference between reading a fact and reading a guess.

        // The attribute solve for this player, or nothing when he was never solved. Read once: the
        // runes column names its runes from it, and the sheet prints its ranks.
        const AttributeModel::PlayerBuild* build = nullptr;
        if (auto bit = m_attrProfiles.find(ard->agent_id); bit != m_attrProfiles.end())
            build = &bit->second;

        if (m_hudWeaponSets.agentId != ard->agent_id)
            BuildWeaponSets(ard->agent_id, m_hudWeaponSets);

        const float sheetW = kColItems + kNameW;

        ImGui::BeginChild("##sheet", ImVec2(sheetW, 0.f));
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float lineH = ImGui::GetTextLineHeight();

            // ── Who this is ──────────────────────────────────────────────────────────────────
            //
            // The window title carries the name too, but a panel that can be opened four times
            // over needs to say whose sheet it is inside its own frame, where the eye already is.
            if (dev && ard->primaryProf >= 1)
                if (ImTextureID prof = LoadProfIcon(dev, ard->primaryProf))
                {
                    ImGui::Image(prof, ImVec2(lineH + 3.f, lineH + 3.f));
                    ImGui::SameLine(0.f, 6.f);
                }
            ImGui::TextColored(ImVec4(0.92f, 0.94f, 0.97f, 1.f), "%s", ard->playerName.c_str());

            std::string subtitle = ProfessionName(ard->primaryProf);
            if (ard->secondaryProf > 0)
                subtitle += std::string(" / ") + ProfessionName(ard->secondaryProf);
            if (!ard->guildTag.empty()) subtitle += "  -  [" + ard->guildTag + "]";
            ImGui::TextColored(muted, "%s", subtitle.c_str());

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // ── The bar he was seen playing ──────────────────────────────────────────────────
            //
            // The match metadata lists the skills the recording saw this player use, which is not
            // the same as the eight he brought: a skill he never pressed leaves no trace at all.
            // So the row is eight slots whatever happens and the count beside it says how many of
            // them are known, rather than quietly showing a five-skill bar as if it were the build.
            std::vector<int> bar;
            {
                std::unordered_set<int> placed;
                const PlayerMeta* meta = nullptr;
                for (const auto& [pid, party] : m_matchMeta.parties)
                {
                    for (const auto& pm : party.players)
                        if (pm.id == ard->agent_id) { meta = &pm; break; }
                    if (!meta)
                        for (const auto& pm : party.others)
                            if (pm.id == ard->agent_id) { meta = &pm; break; }
                    if (meta) break;
                }

                if (meta)
                    for (int sid : meta->used_skills)
                    {
                        const int resolved = m_skillView.ResolvePvpSkillId(sid);
                        if (resolved > 0 && placed.insert(resolved).second) bar.push_back(resolved);
                    }

                // Anything he was seen casting that the metadata missed still belongs on the bar.
                for (const auto& use : ard->skillUseHistory)
                {
                    const int resolved = m_skillView.ResolvePvpSkillId(use.skillId);
                    if (resolved > 0 && placed.insert(resolved).second) bar.push_back(resolved);
                }

                if (bar.size() > 1)
                    bar = m_skillView.SortSkillsForDisplay(bar, ard->primaryProf,
                                                           ard->secondaryProf);
                if (bar.size() > 8) bar.resize(8);
            }

            const ImVec2 skHead = ImGui::GetCursorScreenPos();
            dl->AddText(skHead, kHeadingGold, "Skills");

            const std::string seen = std::format("{} of 8 seen this match", bar.size());
            dl->AddText(ImVec2(skHead.x + sheetW - ImGui::CalcTextSize(seen.c_str()).x - 14.f,
                               skHead.y), ImGui::GetColorU32(muted), seen.c_str());

            constexpr float kSkillCell = 40.f;
            constexpr float kSkillGap  = 6.f;
            const float skTop = skHead.y + ImGui::GetTextLineHeightWithSpacing();

            for (int i = 0; i < 8; ++i)
            {
                const ImVec2 tl(skHead.x + i * (kSkillCell + kSkillGap), skTop);
                const ImVec2 br(tl.x + kSkillCell, tl.y + kSkillCell);
                dl->AddRectFilled(tl, br, IM_COL32(18, 26, 24, 210), 2.f);
                dl->AddRect(tl, br, IM_COL32(118, 138, 128, 200), 2.f);

                const int sid = (i < (int)bar.size()) ? bar[i] : 0;
                const SkillInfo* si = (sid > 0) ? m_skillView.Get(sid) : nullptr;

                if (ImTextureID icon = SkillIcon(dev, sid, m_skillView))
                    dl->AddImage(icon, ImVec2(tl.x + 2.f, tl.y + 2.f),
                                 ImVec2(br.x - 2.f, br.y - 2.f));
                else if (!si)
                    dl->AddText(ImVec2(tl.x + kSkillCell * 0.5f - 4.f,
                                       tl.y + kSkillCell * 0.5f - lineH * 0.5f),
                                ImGui::GetColorU32(muted), "?");

                // No name under the icon: eight of them at forty pixels wide is two lines of
                // shrunken text apiece, which reads as clutter and says nothing the art does not.
                // The card on hover says the rest, and says it properly.
                ImGui::PushID(3000 + i);
                ImGui::SetCursorScreenPos(tl);
                ImGui::InvisibleButton("##skill", ImVec2(kSkillCell, kSkillCell));
                if (ImGui::IsItemHovered())
                {
                    dl->AddRect(tl, br, IM_COL32(255, 215, 100, 200), 2.f, 0, 1.6f);
                    if (si) DrawSkillCard(dev, *si, m_skillView, build, muted);
                    else    ImGui::SetTooltip("This player was never seen using an eighth skill.");
                }
                ImGui::PopID();
            }

            // ── Weapon sets and armour, sharing one bar per row ──────────────────────────────
            //
            // One bar, not two: the client's bag rows run the full width of the window and this
            // reads as one piece of furniture rather than two lists that happen to be side by
            // side. Weapon sets hang off the left end of each bar and armour off the right, so the
            // two columns stay legible while the art stays whole.
            SheetRule(dl, skHead.x, skTop + kSkillCell + 9.f, sheetW - 14.f);
            const ImVec2 origin(skHead.x, skTop + kSkillCell + 20.f);

            const float barX    = origin.x;
            const float mainX   = barX + kWeaponPad;
            const float offX    = mainX + kCellSize + kCellGap;
            const float armourX = barX + kColItems - kCellSize - kArmourPad;

            dl->AddText(ImVec2(mainX, origin.y), kHeadingGold, "Weapon sets");
            HelpMarker(ImVec2(mainX, origin.y), "Weapon sets", TipWeaponSets);

            dl->AddText(ImVec2(armourX - 6.f, origin.y), kHeadingGold, "Armour");
            HelpMarker(ImVec2(armourX - 6.f, origin.y), "Armour", TipArmour);

            const float rowTop   = origin.y + ImGui::GetTextLineHeightWithSpacing();
            const int   setCount = (int)m_hudWeaponSets.sets.size();
            const int   rows     = std::max(setCount, (int)std::size(kArmourSlots));

            for (int i = 0; i < rows; ++i)
            {
                const ImVec2 tl(barX, rowTop + i * kRowPitch);
                drawBand(dl, i, tl, kColItems, kRowHeight);

                const float cellY = tl.y + (kRowHeight - kCellSize) * 0.5f;
                const float textY = tl.y + kRowHeight * 0.5f - 7.f;

                // One weapon set, with its mods spelled out in the tooltip on each cell.
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

                    const ArmourNames::Piece* piece =
                        item ? ArmourNames::Find(item->modelFileId) : nullptr;
                    dl->AddText(ImVec2(barX + kColItems + 10.f, textY),
                                IM_COL32(232, 236, 242, 255),
                                piece ? piece->name : kArmourSlots[i].label);
                }
            }

            SheetRule(dl, barX, rowTop + rows * kRowPitch + 3.f, sheetW - 14.f);

            // The ranks, under the sheet they were read off.
            const float attrBottom = DrawAttributeBlock(
                dev, dl, ImVec2(barX, rowTop + rows * kRowPitch + kAttrHeadGap), sheetW - 20.f,
                build, ard->primaryProf, m_skillView, muted);

            // Reserve the space the rows and the attribute block were drawn into, so the child
            // scrolls to its content rather than clipping it.
            ImGui::SetCursorScreenPos(ImVec2(origin.x, attrBottom + 4.f));
            ImGui::Dummy(ImVec2(sheetW - 8.f, 1.f));
        }
        ImGui::EndChild();

        // ── What the tool worked out: runes, and the one insignia it can name ────────────────
        // The rule that marks where measurement stops and deduction starts.
        ImGui::SameLine(0.f, kRunesGap);
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(p.x - kRunesGap * 0.5f, p.y),
                ImVec2(p.x - kRunesGap * 0.5f, p.y + ImGui::GetContentRegionAvail().y),
                kSheetRule);
        }
        ImGui::BeginChild("##runes", ImVec2(kColRunes, 0.f));
        {
            ImDrawList* rdl = ImGui::GetWindowDrawList();
            const float colW = ImGui::GetContentRegionAvail().x;

            const ImVec2 headPos = ImGui::GetCursorScreenPos();
            ImGui::TextColored(ImVec4(225 / 255.f, 190 / 255.f, 80 / 255.f, 1.f),
                               "Runes & insignias");
            const ImVec2 afterHead = ImGui::GetCursorScreenPos();
            HelpMarker(headPos, "Runes & insignias", TipRunes);
            ImGui::SetCursorScreenPos(afterHead);

            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.f);

            if (!ard->armourSolved)
            {
                ImGui::TextColored(muted, "Not enough readings to measure this player's runes.");
            }
            else
            {
                // The measurement the whole column rests on, in a frame of its own so it reads as
                // the input to what follows rather than as the first line of a list.
                const float boxH = ImGui::GetTextLineHeightWithSpacing() * 2.f +
                                   ImGui::GetStyle().WindowPadding.y * 2.f;
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.10f, 0.12f, 0.85f));
                ImGui::BeginChild("##armourhealth", ImVec2(0.f, boxH), ImGuiChildFlags_Border);
                ImGui::Text("%+d max health from runes", ard->solvedArmourHealth);
                ImGui::TextDisabled("measured from %d readings", ard->armourObservations);
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::Spacing();

                // What the ranks require, handed to the health side. A rank above 12 can only have
                // come from a rune, so the attribute solve knows runes must be there that the
                // health could never point to on its own -- a minor rune costs nothing at all, and
                // two superiors cost the same 150 whichever attributes they sit on.
                HealthModel::RuneConstraints runes;
                if (build)
                {
                    for (const auto& [attrId, tier] : build->runeTier)
                        if (tier > 0) runes.tiers.emplace_back(attrId, tier);
                    runes.headgearAttribute = build->headgearAttribute;

                    // The solve stores them in a hash map, so sort: a list that reorders itself
                    // between two players reads as two different answers.
                    std::sort(runes.tiers.begin(), runes.tiers.end());
                }

                auto builds = HealthModel::ArmourCandidates(ard->solvedArmourHealth, 6, runes);

                // The ranks ask for runes this armour cannot host. Both halves are worth showing:
                // the sets that do reach the measured health, and the sentence saying why they
                // disagree with the ranks beside them.
                std::string runeClash;
                if (builds.empty() && !runes.tiers.empty())
                {
                    builds = HealthModel::ArmourCandidates(ard->solvedArmourHealth, 6);
                    if (!builds.empty())
                    {
                        runeClash = "The ranks need runes this armour health cannot pay for.";
                        if (build)
                            for (const std::string& line : build->contradictions)
                                if (line.rfind("Runes needed for", 0) == 0) { runeClash = line; break; }
                    }
                }

                if (builds.empty())
                {
                    ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f),
                                       "No rune set adds %+d health.", ard->solvedArmourHealth);
                }
                else
                {
                    const ImVec2 setHead = ImGui::GetCursorScreenPos();
                    ImGui::TextDisabled(builds.size() == 1 ? "Only one possible setup"
                                                           : "Most likely setup");
                    if (builds.size() > 1)
                    {
                        const std::string badge = std::format("1 of {}", builds.size());
                        rdl->AddText(ImVec2(setHead.x + colW -
                                                ImGui::CalcTextSize(badge.c_str()).x, setHead.y),
                                     ImGui::GetColorU32(muted), badge.c_str());
                    }

                    for (const auto& e : BuildEntries(dev, builds.front(), ard->primaryProf))
                    {
                        ImGui::BeginGroup();
                        if (e.icon) ImGui::Image(e.icon, RuneIconSize(e.square));
                        else        ImGui::Dummy(RuneIconSize(e.square));
                        ImGui::EndGroup();

                        ImGui::SameLine(0.f, 8.f);
                        ImGui::BeginGroup();
                        ImGui::TextColored(RarityVec(e.rarity), "%s", e.name.c_str());
                        if (!e.gain.empty())
                            ImGui::TextColored(RarityVec(e.rarity), "%s", e.gain.c_str());
                        // The health cost still has to be visible where there is one - it is what
                        // the whole armour solve is measured in - but a minor rune has none, and
                        // saying "+0 health" about it three times over read as a bug.
                        if (e.health != 0 || e.gain.empty())
                            ImGui::TextColored(e.health >= 0 ? ImVec4(0.55f, 0.85f, 0.60f, 1.f)
                                                             : ImVec4(0.90f, 0.55f, 0.55f, 1.f),
                                               "%+d health", e.health);
                        ImGui::EndGroup();
                    }

                    if (builds.size() > 1)
                    {
                        const std::string more =
                            std::format("{} other possible sets", builds.size() - 1);
                        if (ImGui::TreeNode(more.c_str()))
                        {
                            for (size_t i = 1; i < builds.size(); ++i)
                                ImGui::BulletText("%s", builds[i].label.c_str());
                            ImGui::TreePop();
                        }
                    }

                    if (!runeClash.empty())
                    {
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", runeClash.c_str());
                    }
                }
            }

            // The one piece of armour we can name outright. Runes are inferred from health and
            // stay possibilities; a Stonefist Insignia is measured directly, because a knockdown
            // that lasts three seconds instead of two can be nothing else. It sits below the runes
            // behind a rule for that reason, and it is drawn whether or not the armour health
            // solved, since the knockdowns say it on their own. Primary Warriors only: the
            // insignia is Warrior armour, and one character wears at most one of it.
            if (build && ard->primaryProf == 1 && build->stonefist.detected)
            {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::BeginGroup();
                if (ImTextureID icon = StonefistIcon(dev)) ImGui::Image(icon, RuneIconSize(true));
                else                                       ImGui::Dummy(RuneIconSize(true));
                ImGui::SameLine(0.f, 8.f);
                ImGui::BeginGroup();
                ImGui::TextColored(RarityVec(Equipment::Rarity::Blue), "Stonefist Insignia");
                ImGui::TextDisabled("knockdowns last 3s, not 2s");
                ImGui::EndGroup();
                ImGui::EndGroup();

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Measured, not guessed: %d of the %d knockdowns this player caused that\n"
                        "we could time lasted the extra second.\n\n"
                        "Only knockdowns from skills whose length is the game's standard two\n"
                        "seconds are counted, so Backbreaker and the 2...3 second skills are\n"
                        "left out of it.",
                        build->stonefist.longOnes, build->stonefist.knockdowns);
            }

            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();

        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(9);
    }

    // Closing a panel drops it, so the vector holds only live ones and the ids stay unique.
    std::erase_if(m_characterPanels, [](const CharacterPanelInstance& c) { return !c.open; });
}
