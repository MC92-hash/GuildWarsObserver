#include "pch.h"
#include "ReplayWindow.h"
#include "ReplayWindow_LiveWeaponSets.h"
#include "EquipmentComponents.h"
#include "EquipmentData.h"
#include "EquipmentIcons.h"
#include "GuiGlobalConstants.h"
#include "LiveSource.h"
#include "ReplayWindow_Internal.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

// -------------------------------------------------------------------------------------------------
// THE WEAPON SET BAR OF A LIVE MATCH. The game's art, from Gw.dat:
//   ovals    144758 (Weaponbar/ui_weapon_set_slots.png): two 64x64 cells, the idle oval on the left and
//            the lit one on the right; the oval fills rows 0-55 of its cell
//   keycaps  143032 (Skillbar/ui_skillbar_keycaps.png, the skill bar's sheet): F1-F4 are the third row,
//            cells x 4-23 / 28-47 / 52-71 / 76-94, y 54-70
// Layout as in the game (the owner's gw681.jpg): a 2x2 block, F1 F2 on top, F3 F4 below, the set in
// hand lit and a little larger, each oval's keycap at its lower left, the weapon icons over the oval
// (the offhand behind and down-right, the main hand in front). The block sits in the window's
// bottom-right corner (the owner's choice). A click asks for that set (TakeLiveWeaponSetClick).
// -------------------------------------------------------------------------------------------------
namespace
{
constexpr float kOvalV = 56.f / 64.f;          // the oval's rows in its cell
constexpr float kIdleW = 46.f;                 // an oval's width (the height follows the art)
constexpr float kActiveMul = 1.18f;            // the set in hand
constexpr float kIconMul = 1.15f;              // icons overhang the oval, as in the game
constexpr float kCellW = kIdleW * kActiveMul;  // the 2x2 grid's pitch fits the larger oval
constexpr float kCellH = kCellW * kOvalV;
constexpr float kMargin = 16.f;

struct KeyCell
{
    float x0, x1;
};
constexpr KeyCell kKeys[4] = { { 4.f, 23.f }, { 28.f, 47.f }, { 52.f, 71.f }, { 76.f, 94.f } };
constexpr float kKeyY0 = 54.f, kKeyY1 = 70.f, kKeySheet = 128.f;

int s_clicked = -1;

// -------------------------------------------------------------------------------------------------
// THE EQUIPMENT WINDOW: a set's items and upgrades, as the game's PvP equipment window chooses them (a
// weapon type, its requirement, then the upgrades that fit). Opened by a right click on a set, or a left
// click on an empty one. Every change goes to the source at once (ILiveSource::EditPlayerWeaponSet); the
// stats are what the source computed. The chrome is the Trials skills window's (the game's team frame,
// title bar and close button); the item tooltip is the replay's weapon set tooltip
// (ReplayWindow::DrawWeaponSetTooltip), built here from the live set instead of a recorded item.
//
// Upgrades come from the game's PvP upgrade table (EquipmentComponents.h, read only):
//   prefix / suffix   by the part's name ("Sword Hilt" a sword's prefix, "Sword Pommel" its suffix ...)
//   inscriptions      by the game's four groups (GWToolbox CompletionWindow_Constants.h): all weapons
//                     348, 350; martial weapons 355-362; spellcasting weapons 470-477; focus items or
//                     shields, every other inscription (438-469)
// Icons are Gw.dat inventory icons (EquipmentIcons.h): the item's skin, and each upgrade by its part
// (the GWToolbox ids). The Dagger Handle has no icon of its own: it shows the Dagger Tang's.
// -------------------------------------------------------------------------------------------------
struct TypeInfo
{
    int type;
    const char* name;
    bool twoHanded;
    int attribute;            // a martial weapon's fixed requirement, -1 = chosen
    uint32_t skin;            // the default skin (a PvP base model, Gw.dat file id)
    const char* prefixPart;   // nullptr = no prefix slot
    const char* suffixPart;
};
constexpr TypeInfo kMainTypes[] = {
    { 2, "Axe", false, 18, 9364, "Axe Haft", "Axe Grip" },
    { 5, "Bow", true, 25, 9507, "Bowstring", "Bow Grip" },
    { 32, "Daggers", true, 29, 175552, "Dagger Tang", "Dagger Handle" },
    { 15, "Hammer", true, 19, 39779, "Hammer Haft", "Hammer Grip" },
    { 35, "Scythe", true, 41, 205952, "Scythe Snathe", "Scythe Grip" },
    { 36, "Spear", false, 37, 205960, "Spearhead", "Spear Grip" },
    { 27, "Sword", false, 20, 14401, "Sword Hilt", "Sword Pommel" },
    { 26, "Staff", true, -1, 143090, "Staff Head", "Staff Wrapping" },
    { 22, "Wand", false, -1, 176583, nullptr, "Wand Wrapping" },
};
constexpr TypeInfo kOffTypes[] = {
    { 12, "Focus", false, -1, 92509, nullptr, "Focus Core" },
    { 24, "Shield", false, -1, 14372, nullptr, "Shield Handle" },
};

const TypeInfo* FindType(int type)
{
    for (const TypeInfo& t : kMainTypes)
        if (t.type == type) return &t;
    for (const TypeInfo& t : kOffTypes)
        if (t.type == type) return &t;
    return nullptr;
}

bool Martial(int type) { return type == 2 || type == 5 || type == 32 || type == 15 || type == 35 || type == 36 || type == 27; }
bool Spellcasting(int type) { return type == 22 || type == 26; }
bool FocusOrShield(int type) { return type == 12 || type == 24; }

// The upgrade parts' inventory icons (Gw.dat file ids, GWToolbox CompletionWindow_Constants.h).
struct PartIcon
{
    const char* part;
    uint32_t file;
};
constexpr PartIcon kPartIcons[] = {
    { "Axe Grip", 0x16602 },      { "Axe Haft", 0x4da0 },        { "Bow Grip", 0x16605 },      { "Bowstring", 0x16607 },
    { "Hammer Grip", 0x16606 },   { "Hammer Haft", 0x4da1 },     { "Sword Pommel", 0x16608 },  { "Sword Hilt", 0x4e2f },
    { "Focus Core", 0x32459 },    { "Wand Wrapping", 0x3245c },  { "Shield Handle", 0x32460 }, { "Staff Head", 0x283bb },
    { "Staff Wrapping", 0x283bc }, { "Scythe Grip", 0x3245d },   { "Scythe Snathe", 0x32447 }, { "Spear Grip", 0x32461 },
    { "Spearhead", 0x32448 },     { "Dagger Tang", 0x283e5 },
    { "Dagger Handle", 0x283e5 },   // no icon of its own: the Dagger Tang's
};

uint32_t PartIconId(const char* part)
{
    if (!part) return 0;
    for (const PartIcon& p : kPartIcons)
        if (std::strcmp(p.part, part) == 0) return p.file;
    return 0;
}

// The game's inscription groups and their icons.
enum class InscriptionGroup { None, AllWeapons, Martial, Spellcasting, FocusOrShield };

InscriptionGroup GroupOf(int id)
{
    if (id == 348 || id == 350) return InscriptionGroup::AllWeapons;
    if (id >= 355 && id <= 362) return InscriptionGroup::Martial;
    if (id >= 470 && id <= 477) return InscriptionGroup::Spellcasting;
    if (id >= 438 && id <= 469) return InscriptionGroup::FocusOrShield;
    return InscriptionGroup::None;
}

// The group an item type's own inscriptions come from (the empty slot's icon).
InscriptionGroup GroupOfItem(int type)
{
    if (Martial(type)) return InscriptionGroup::Martial;
    if (Spellcasting(type)) return InscriptionGroup::Spellcasting;
    if (FocusOrShield(type)) return InscriptionGroup::FocusOrShield;
    return InscriptionGroup::None;
}

uint32_t InscriptionIconId(InscriptionGroup g)
{
    switch (g)
    {
    case InscriptionGroup::AllWeapons: return 0x32444;
    case InscriptionGroup::Martial: return 0x32442;
    case InscriptionGroup::Spellcasting: return 0x32441;
    case InscriptionGroup::FocusOrShield: return 0x32443;
    default: return 0;
    }
}

// The inscriptions an item type takes, strictly by the game's groups.
bool InscriptionFits(int id, int type)
{
    switch (GroupOf(id))
    {
    case InscriptionGroup::AllWeapons: return Martial(type) || Spellcasting(type);
    case InscriptionGroup::Martial: return Martial(type);
    case InscriptionGroup::Spellcasting: return Spellcasting(type);
    case InscriptionGroup::FocusOrShield: return FocusOrShield(type);
    default: return false;
    }
}

bool PartFits(const Equipment::ComponentInfo& c, const char* part)
{
    return part && c.cls == Equipment::ComponentClass::Upgrade && std::string_view(c.name).find(part) != std::string_view::npos;
}

const Equipment::ComponentInfo* Component(int id)
{
    if (id <= 0) return nullptr;
    for (const Equipment::ComponentInfo& c : Equipment::kComponents)
        if (c.id == id) return &c;
    return nullptr;
}

// -------------------------------------------------------------------------------------------------
// Colours and the item's text, as the replay's weapon set tooltip writes a recorded item.
// -------------------------------------------------------------------------------------------------
constexpr Equipment::Rarity kPvpRarity = Equipment::Rarity::Green;   // PvP items are green in the game
const ImVec4 kGameText(0.96f, 0.89f, 0.71f, 1.f);
const ImVec4 kMuted(124 / 255.f, 126 / 255.f, 126 / 255.f, 1.f);
constexpr ImU32 kTitleText = IM_COL32(0xDC, 0xDC, 0xDC, 0xFF);
constexpr ImU32 kGoldLine = IM_COL32(0xD4, 0xA1, 0x21, 0x60);

ImVec4 ToVec(Equipment::Rgb c) { return ImVec4(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f); }
ImVec4 NameColour() { return ToVec(Equipment::RarityColor(kPvpRarity)); }
ImVec4 ModColour() { return ToVec(Equipment::ModTextColor(kPvpRarity)); }

const char* AttributeLabel(int attribute)
{
    const char* n = attribute >= 0 ? Equipment::AttributeName(static_cast<uint32_t>(attribute)) : nullptr;
    return n ? n : "(no attribute)";
}

// One hand of a set, read from or written to a LiveWeaponSet.
struct Hand
{
    int* type;
    uint32_t* model;
    int* attribute;
    int* requirement;
    int* prefix;   // nullptr: the off hand has no prefix
    int* suffix;
    int* inscription;
};

Hand MainHand(LiveWeaponSet& s)
{
    return { &s.mainType, &s.mainModel, &s.mainAttribute, &s.mainRequirement, &s.mainPrefix, &s.mainSuffix, &s.mainInscription };
}
Hand OffHand(LiveWeaponSet& s)
{
    return { &s.offType, &s.offModel, &s.offAttribute, &s.offRequirement, nullptr, &s.offSuffix, &s.offInscription };
}

// The game's name of a PvP item: its prefix's adjective, the type, its suffix's "of ...".
std::string ItemName(const TypeInfo* t, int prefix, int suffix)
{
    if (!t) return {};
    std::string name;
    if (const Equipment::ComponentInfo* c = Component(prefix))
        name = std::string(c->name.substr(0, c->name.find(' '))) + " ";
    name += t->name;
    if (const Equipment::ComponentInfo* c = Component(suffix))
        if (const size_t of = c->name.find(" of "); of != std::string_view::npos) name += std::string(c->name.substr(of));
    return name;
}

// The headline stat of an item at its requirement. A shield's armour follows the requirement
// (min(16, rank + 8), the owner's rule, as the sim computes it); a focus's and a staff's energy and
// the weapons' damage are the PvP maximum at every requirement for now.
struct StatLine
{
    std::string stat, second;   // second: a staff's energy, under its damage
};
StatLine ItemStat(int type, int attribute, int requirement)
{
    auto damage = [](const char* kind, int lo, int hi) {
        return kind ? std::format("{} Dmg: {}-{}", kind, lo, hi) : std::format("Damage: {}-{}", lo, hi);
    };
    // A staff's or wand's bolt is the profession's element (an Elementalist's by the attribute).
    auto bolt = [&]() -> const char* {
        if (attribute >= 0 && attribute <= 3) return "Chaos";
        if (attribute >= 4 && attribute <= 7) return "Dark";
        if (attribute == 8) return "Lightning";
        if (attribute == 9) return "Earth";
        if (attribute == 10) return "Fire";
        if (attribute == 11) return "Cold";
        if (attribute >= 13 && attribute <= 16) return "Holy";
        return nullptr;
    };
    switch (type)
    {
    case 2: return { damage("Slashing", 6, 28), {} };
    case 5: return { damage("Piercing", 15, 28), {} };
    case 32: return { damage("Piercing", 7, 17), {} };
    case 15: return { damage("Blunt", 19, 35), {} };
    case 35: return { damage("Slashing", 9, 41), {} };
    case 36: return { damage("Piercing", 14, 27), {} };
    case 27: return { damage("Slashing", 15, 22), {} };
    case 26: return { damage(bolt(), 11, 22), "Energy +10" };
    case 22: return { damage(bolt(), 11, 22), {} };
    case 12: return { "Energy +12", {} };
    case 24: return { std::format("Armor: {}", (std::min)(16, requirement + 8)), {} };
    default: return {};
    }
}

// An effect line with its grey qualifier; the qualifier drops to the next line when both do not fit.
void EffectLine(const Equipment::DescLine& d, const ImVec4& colour)
{
    ImGui::TextColored(colour, "%s", d.effect.c_str());
    if (d.condition.empty()) return;
    const float wrap = ImGui::GetCurrentWindow()->DC.TextWrapPos;
    const float lineEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
    if (wrap <= 0.f || lineEnd + 6.f + ImGui::CalcTextSize(d.condition.c_str()).x <= wrap) ImGui::SameLine(0.f, 6.f);
    ImGui::TextColored(kMuted, "%s", d.condition.c_str());
}

// -------------------------------------------------------------------------------------------------
// THE TOOLTIP: the replay's weapon set tooltip (dark glass, a gold border, the item's name in its
// rarity colour, the stat line in white with the requirement muted, each upgrade's lines in the mod
// colour with their conditions muted).
// -------------------------------------------------------------------------------------------------
void PushGameTooltipStyle()
{
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.039f, 0.055f, 0.071f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.63f, 0.13f, 0.3f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
}
void PopGameTooltipStyle()
{
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void ItemBlock(const LiveWeaponSet& s, bool mainHand, float scale)
{
    const int type = mainHand ? s.mainType : s.offType;
    const TypeInfo* t = FindType(type);
    if (!t) return;
    const int attribute = mainHand ? s.mainAttribute : s.offAttribute;
    const int requirement = mainHand ? s.mainRequirement : s.offRequirement;
    const int prefix = mainHand ? s.mainPrefix : 0;
    const int suffix = mainHand ? s.mainSuffix : s.offSuffix;
    const int inscription = mainHand ? s.mainInscription : s.offInscription;

    ImGui::SetWindowFontScale(scale * 1.15f);
    ImGui::TextColored(NameColour(), "%s", ItemName(t, prefix, suffix).c_str());
    ImGui::SetWindowFontScale(scale);

    const StatLine stat = ItemStat(type, attribute, requirement);
    const int reqAttribute = t->attribute >= 0 ? t->attribute : attribute;
    if (!stat.stat.empty())
    {
        ImGui::TextColored(ToVec(Equipment::kStatColor), "%s", stat.stat.c_str());
        if (reqAttribute >= 0) ImGui::SameLine(0.f, 6.f);
    }
    if (reqAttribute >= 0) ImGui::TextColored(kMuted, "(Requires %d %s)", requirement, AttributeLabel(reqAttribute));
    if (!stat.second.empty()) ImGui::TextColored(ToVec(Equipment::kStatColor), "%s", stat.second.c_str());

    for (int id : { prefix, suffix, inscription })
        if (const Equipment::ComponentInfo* c = Component(id))
            for (const Equipment::DescLine& d : Equipment::DescriptionLines(c->description))
            {
                ImGui::TextColored(ModColour(), "%s", d.effect.c_str());
                if (!d.condition.empty())
                {
                    ImGui::SameLine(0.f, 6.f);
                    ImGui::TextColored(kMuted, "%s", d.condition.c_str());
                }
            }
}

// which: 0 both hands, 1 the main hand, 2 the off hand. `footer` is a muted last line (may be null).
// `at`: a fixed place (a picture without a mouse), else at the mouse.
void ShowSetTooltip(const LiveWeaponSet& s, int which, const char* footer, const ImVec2* at = nullptr)
{
    const float size = ImGui::GetFontSize();
    const float fontScale = size > 1.f ? (size - 0.5f) / size : 1.f;
    PushGameTooltipStyle();
    if (at) ImGui::SetNextWindowPos(*at, ImGuiCond_Always);
    ImGui::BeginTooltip();
    ImGui::SetWindowFontScale(fontScale);
    const bool main = which != 2 && s.mainType != 0;
    const bool off = which != 1 && s.offType != 0;
    if (main) ItemBlock(s, true, fontScale);
    if (off)
    {
        if (main)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }
        ItemBlock(s, false, fontScale);
    }
    if (footer)
    {
        if (main || off) ImGui::Spacing();
        ImGui::TextColored(kMuted, "%s", footer);
    }
    ImGui::SetWindowFontScale(1.f);
    ImGui::EndTooltip();
    PopGameTooltipStyle();
}

// -------------------------------------------------------------------------------------------------
// The window's pieces.
// -------------------------------------------------------------------------------------------------
// The weapon set oval with its item icons and keycap (the bar's drawing, shared with the window).
void DrawSetOval(ImDrawList* dl, DATManager* dat, ID3D11Device* dev, const LiveWeaponSet& set, int index, ImVec2 c, float w, bool lit,
                 bool inHand)
{
    ImTextureID ovals = LoadGameUITexture(dev, "Weaponbar\\ui_weapon_set_slots.png");
    ImTextureID keys = LoadGameUITexture(dev, "Skillbar\\ui_skillbar_keycaps.png");
    const float h = w * kOvalV;
    const ImVec2 o0(c.x - w * 0.5f, c.y - h * 0.5f), o1(c.x + w * 0.5f, c.y + h * 0.5f);
    if (ovals)
        dl->AddImage(ovals, o0, o1, ImVec2(lit ? 0.5f : 0.f, 0.f), ImVec2(lit ? 1.f : 0.5f, kOvalV),
                     set.used ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 150));
    else
        dl->AddEllipseFilled(c, w * 0.5f, h * 0.5f, lit ? IM_COL32(120, 112, 80, 220) : IM_COL32(70, 72, 76, 200));

    // The weapon icons, the item's own inventory icon from Gw.dat (EquipmentIcons.h, read only).
    ImTextureID mainTex = set.mainModel ? EquipmentIcons::Get(dat, dev, set.mainModel) : nullptr;
    ImTextureID offTex = set.offModel ? EquipmentIcons::Get(dat, dev, set.offModel) : nullptr;
    const float icon = h * kIconMul;
    auto blit = [&](ImTextureID tex, float sz, float dx, float dy, ImU32 tint) {
        dl->AddImage(tex, ImVec2(c.x + dx - sz * 0.5f, c.y + dy - sz * 0.5f), ImVec2(c.x + dx + sz * 0.5f, c.y + dy + sz * 0.5f),
                     ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), tint);
    };
    const ImU32 tint = inHand ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 235);
    if (mainTex && offTex)
    {
        const float d = icon * 0.09f;
        blit(offTex, icon * 0.85f, d, d, tint);
        blit(mainTex, icon, -d, -d, tint);
    }
    else if (mainTex || offTex)
        blit(mainTex ? mainTex : offTex, icon, 0.f, 0.f, tint);

    // The keycap at the oval's lower left, over the icons.
    if (keys)
    {
        const KeyCell& k = kKeys[index];
        const float kw = (k.x1 - k.x0 + 1.f), kh = (kKeyY1 - kKeyY0 + 1.f);
        const ImVec2 k0(o0.x + w * 0.04f, o1.y - kh * 0.85f);
        dl->AddImage(keys, k0, ImVec2(k0.x + kw, k0.y + kh), ImVec2(k.x0 / kKeySheet, kKeyY0 / kKeySheet),
                     ImVec2((k.x1 + 1.f) / kKeySheet, (kKeyY1 + 1.f) / kKeySheet));
    }
}

// An icon in a dark slot with a thin gold edge (the game's inventory slot look).
void DrawIconBox(ImDrawList* dl, ImVec2 p0, float size, ImTextureID tex, bool dim, bool hovered)
{
    const ImVec2 p1(p0.x + size, p0.y + size);
    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 150), 3.f);
    if (tex) dl->AddImage(tex, p0, p1, ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), dim ? IM_COL32(120, 120, 120, 120) : IM_COL32_WHITE);
    dl->AddRect(p0, p1, hovered ? IM_COL32(0xF0, 0xC8, 0x60, 0xD0) : IM_COL32(0xB0, 0x90, 0x50, 0x70), 3.f, 0, 1.f);
}

// A section title: light grey text over a faint gold rule, as the game's panels head a block.
void SectionTitle(const char* text, float width)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTitleText));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + width, p.y), kGoldLine, 1.f);
    ImGui::Dummy(ImVec2(width, 4.f));
}

// One row: an icon, a name, and the description lines in the mod colour. True when clicked.
bool UpgradeRow(const char* id, ImTextureID icon, const char* name, std::string_view description, const char* note, bool empty,
                bool selected, float width, float iconSize = 32.f)
{
    ImGui::PushID(id);
    const ImVec2 r0 = ImGui::GetCursorScreenPos();
    ImGui::BeginGroup();
    const ImVec2 iconPos(r0.x + 3.f, r0.y + 3.f);
    ImGui::Dummy(ImVec2(iconSize + 6.f, iconSize + 6.f));
    ImGui::SameLine(0.f, 6.f);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0.f, 1.f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (width - iconSize - 18.f));
    ImGui::TextColored(empty ? kMuted : kGameText, "%s", name);
    for (const Equipment::DescLine& d : Equipment::DescriptionLines(description)) EffectLine(d, ModColour());
    if (note && *note) ImGui::TextColored(kMuted, "%s", note);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::EndGroup();
    const ImVec2 r1(r0.x + width, (std::max)(ImGui::GetItemRectMax().y, r0.y + iconSize + 6.f));
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(r0, r1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered || selected)
        dl->AddRectFilled(r0, r1, selected ? IM_COL32(0xD4, 0xA1, 0x21, 0x22) : IM_COL32(255, 255, 255, 16), 4.f);
    DrawIconBox(dl, iconPos, iconSize, icon, empty, hovered);
    ImGui::SetCursorScreenPos(ImVec2(r0.x, r1.y + 3.f));
    ImGui::Dummy(ImVec2(width, 0.f));
    ImGui::PopID();
    return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// The game-like popup: the tooltip's dark glass, anchored under what opened it.
bool BeginGamePopup(const char* id, ImVec2 anchor, float width, float maxHeight = 430.f)
{
    if (!ImGui::IsPopupOpen(id)) return false;
    PushGameTooltipStyle();
    ImGui::PushStyleColor(ImGuiCol_Text, kGameText);
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.83f, 0.63f, 0.13f, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.f, 1.f, 1.f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.83f, 0.63f, 0.13f, 0.30f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.f);
    // Held under what opened it every frame (the window may still be settling when it opens), and
    // kept on the screen: a list that would run off the bottom opens upwards.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float bottom = vp->WorkPos.y + vp->WorkSize.y - 8.f;
    const ImGuiWindow* last = ImGui::FindWindowByName(std::format("##Popup_{:08x}", ImGui::GetID(id)).c_str());
    const float height = last ? last->Size.y : 0.f;   // last frame's; 0 the frame it opens
    if (anchor.y + height > bottom) anchor.y = (std::max)(vp->WorkPos.y + 8.f, bottom - height);
    anchor.x = (std::min)(anchor.x, vp->WorkPos.x + vp->WorkSize.x - width - 8.f);
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.f), ImVec2(width, maxHeight));
    const bool open = ImGui::BeginPopup(id, ImGuiWindowFlags_NoSavedSettings);
    if (!open)
    {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        PopGameTooltipStyle();
    }
    return open;
}

// Closes a popup BeginGamePopup opened.
void EndGamePopup()
{
    ImGui::EndPopup();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    PopGameTooltipStyle();
}

// A small button in the game's gold-on-dark: the requirement rank, the attribute.
bool ChooserButton(const char* label)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.17f, 0.11f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.26f, 0.15f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.42f, 0.34f, 0.18f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.63f, 0.13f, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, 1.f));
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    return clicked;
}

// DEV pictures without real input (developer builds only): GWO_TRIALS_EQUIPMENT_DEMO=tooltip shows the
// edited set's tooltip beside the window; =picker opens the main hand's suffix picker; =types the main
// hand's item list; =rank the main hand's requirement list.
std::string s_demo;
bool s_demoOpenPending = false;

bool DemoOpens(const char* what, bool mainHand)
{
    if (!s_demoOpenPending || !mainHand || s_demo != what) return false;
    s_demoOpenPending = false;
    return true;
}

// One hand: the item, its requirement, its upgrade slots, and their pickers. True when anything
// changed. `blockedBy`: a two-handed main hand greys this hand (its icon shown dim).
bool HandPanel(const char* id, const char* title, bool mainHand, Hand h, const LiveWeaponSet& view, uint32_t blockedBy, float width,
               DATManager* dat, ID3D11Device* dev)
{
    bool changed = false;
    ImGui::PushID(id);
    ImGui::BeginGroup();
    SectionTitle(title, width);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float kBig = 64.f;

    if (blockedBy)
    {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        DrawIconBox(dl, p0, kBig, EquipmentIcons::Get(dat, dev, blockedBy), true, false);
        ImGui::Dummy(ImVec2(kBig, kBig));
        ImGui::SameLine(0.f, 12.f);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0.f, 4.f));
        ImGui::TextColored(kMuted, "Two-handed weapon");
        ImGui::TextColored(kMuted, "The main hand holds it");
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(width, 0.f));
        ImGui::EndGroup();
        ImGui::PopID();
        return false;
    }

    const TypeInfo* t = FindType(*h.type);
    // The item: a large icon (a click lists the item types), its name, stat and requirement.
    const ImVec2 b0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##item", ImVec2(kBig, kBig));
    const bool boxHovered = ImGui::IsItemHovered();
    DrawIconBox(dl, b0, kBig, t ? EquipmentIcons::Get(dat, dev, *h.model) : nullptr, false, boxHovered);
    if (!t) dl->AddText(ImVec2(b0.x + kBig * 0.5f - ImGui::CalcTextSize("+").x * 0.5f, b0.y + (kBig - ImGui::GetFontSize()) * 0.5f),
                        IM_COL32(0xB0, 0x90, 0x50, 0xC0), "+");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || DemoOpens("types", mainHand)) ImGui::OpenPopup("##types");
    if (boxHovered && t && !ImGui::IsPopupOpen("##types")) ShowSetTooltip(view, mainHand ? 1 : 2, nullptr);
    ImGui::SameLine(0.f, 12.f);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - kBig - 12.f);
    if (t)
    {
        ImGui::SetWindowFontScale(1.1f);
        ImGui::TextColored(NameColour(), "%s", ItemName(t, h.prefix ? *h.prefix : 0, *h.suffix).c_str());
        ImGui::SetWindowFontScale(1.f);
        const StatLine stat = ItemStat(*h.type, *h.attribute, *h.requirement);
        ImGui::TextColored(ToVec(Equipment::kStatColor), "%s", stat.stat.c_str());
        if (!stat.second.empty())
        {
            ImGui::SameLine(0.f, 10.f);
            ImGui::TextColored(ToVec(Equipment::kStatColor), "%s", stat.second.c_str());
        }
        // "Requires [rank] [attribute]": the rank always chosen, the attribute only for caster and off-hand items.
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMuted, "Requires");
        ImGui::SameLine(0.f, 5.f);
        if (ChooserButton(std::format("{}##rank", *h.requirement).c_str()) || DemoOpens("rank", mainHand)) ImGui::OpenPopup("##rank");
        if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("##rank")) ImGui::SetTooltip("The requirement rank");
        const ImVec2 rankAnchor(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2.f);
        ImGui::SameLine(0.f, 5.f);
        ImVec2 attrAnchor = rankAnchor;
        if (t->attribute >= 0)
            ImGui::TextColored(kGameText, "%s", AttributeLabel(t->attribute));
        else
        {
            if (ChooserButton(std::format("{}##attr", AttributeLabel(*h.attribute)).c_str())) ImGui::OpenPopup("##attr");
            attrAnchor = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2.f);
        }
        if (BeginGamePopup("##rank", rankAnchor, 70.f))
        {
            for (int r = 5; r <= 13; ++r)
                if (ImGui::Selectable(std::format("{}", r).c_str(), r == *h.requirement))
                {
                    *h.requirement = r;
                    changed = true;
                }
            EndGamePopup();
        }
        if (BeginGamePopup("##attr", attrAnchor, 200.f, 380.f))
        {
            for (uint32_t a = 0; a < 45; ++a)
                if (const char* n = Equipment::AttributeName(a))
                    if (ImGui::Selectable(n, static_cast<int>(a) == *h.attribute))
                    {
                        *h.attribute = static_cast<int>(a);
                        changed = true;
                    }
            EndGamePopup();
        }
    }
    else
    {
        ImGui::Dummy(ImVec2(0.f, 6.f));
        ImGui::TextColored(kMuted, "(no item)");
        ImGui::TextColored(kMuted, "Click the slot to choose one");
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(b0.x, (std::max)(ImGui::GetItemRectMax().y, b0.y + kBig) + 8.f));

    // The item types.
    if (BeginGamePopup("##types", ImVec2(b0.x, b0.y + kBig + 2.f), 230.f))
    {
        auto pick = [&](const TypeInfo* n) {
            if ((n ? n->type : 0) == *h.type) return;
            *h.type = n ? n->type : 0;
            *h.model = n ? n->skin : 0;
            *h.attribute = n ? n->attribute : -1;
            if (n && *h.attribute < 0) *h.attribute = n->type == 24 ? 21 : 13;   // a shield Tactics, a caster item Healing Prayers
            if (h.prefix) *h.prefix = 0;
            *h.suffix = 0;
            *h.inscription = 0;
            changed = true;
            ImGui::CloseCurrentPopup();
        };
        const float w = ImGui::GetContentRegionAvail().x;
        if (UpgradeRow("none", nullptr, "(none)", {}, nullptr, true, *h.type == 0, w)) pick(nullptr);
        auto list = [&](const TypeInfo* begin, const TypeInfo* end) {
            for (const TypeInfo* n = begin; n != end; ++n)
                if (UpgradeRow(n->name, EquipmentIcons::Get(dat, dev, n->skin), n->name, {}, n->twoHanded ? "Two-handed" : nullptr, false,
                               n->type == *h.type, w))
                    pick(n);
        };
        if (mainHand) list(std::begin(kMainTypes), std::end(kMainTypes));
        else list(std::begin(kOffTypes), std::end(kOffTypes));
        EndGamePopup();
    }

    // The upgrade slots.
    t = FindType(*h.type);
    if (t)
    {
        struct SlotDef
        {
            const char* key;
            const char* part;   // nullptr: the inscription
            int* id;
        };
        SlotDef slots[3];
        int count = 0;
        if (h.prefix && t->prefixPart) slots[count++] = { "p", t->prefixPart, h.prefix };
        slots[count++] = { "s", t->suffixPart, h.suffix };
        slots[count++] = { "i", nullptr, h.inscription };
        const int type = *h.type;
        for (int i = 0; i < count; ++i)
        {
            const SlotDef& s = slots[i];
            auto fits = [&](const Equipment::ComponentInfo& c) {
                return s.part ? PartFits(c, s.part) : c.cls == Equipment::ComponentClass::Inscription && InscriptionFits(c.id, type);
            };
            auto iconOf = [&](int componentId) -> ImTextureID {
                const uint32_t file = s.part ? PartIconId(s.part)
                                             : InscriptionIconId(componentId ? GroupOf(componentId) : GroupOfItem(type));
                return file ? EquipmentIcons::Get(dat, dev, file) : nullptr;
            };
            const Equipment::ComponentInfo* cur = Component(*s.id);
            const std::string empty = std::format("{} (empty)", s.part ? s.part : "Inscription");
            const ImVec2 rowTop = ImGui::GetCursorScreenPos();
            const std::string popup = std::format("##pick_{}", s.key);
            if (UpgradeRow(s.key, iconOf(*s.id), cur ? std::string(cur->name).c_str() : empty.c_str(), cur ? cur->description : std::string_view(),
                           nullptr, !cur, false, width) ||
                (std::strcmp(s.key, "s") == 0 && DemoOpens("picker", mainHand)))
                ImGui::OpenPopup(popup.c_str());
            if (BeginGamePopup(popup.c_str(), ImVec2(rowTop.x + 20.f, ImGui::GetCursorScreenPos().y), 420.f))
            {
                const float w = ImGui::GetContentRegionAvail().x;
                ImGui::TextColored(kMuted, "%s", s.part ? s.part : "Inscription");
                if (UpgradeRow("none", nullptr, "(none)", {}, nullptr, true, *s.id == 0, w))
                {
                    if (*s.id != 0) changed = true;
                    *s.id = 0;
                    ImGui::CloseCurrentPopup();
                }
                for (const Equipment::ComponentInfo& c : Equipment::kComponents)
                {
                    if (!fits(c)) continue;
                    if (UpgradeRow(std::format("{}", c.id).c_str(), iconOf(c.id), std::string(c.name).c_str(), c.description, nullptr, false,
                                   c.id == *s.id, w))
                    {
                        if (*s.id != c.id) changed = true;
                        *s.id = c.id;
                        ImGui::CloseCurrentPopup();
                    }
                }
                EndGamePopup();
            }
        }
    }
    ImGui::Dummy(ImVec2(width, 0.f));
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

bool s_editorOpen = false;
int s_editorSet = 0;

// DEV: writes whether every icon the window uses decodes (the self-test folder, equipment_icons.txt).
void LogIconCheck(DATManager* dat, ID3D11Device* dev)
{
    char folder[MAX_PATH] = {};
    if (GetEnvironmentVariableA("GWO_TRIALS_SELFTEST", folder, sizeof(folder)) == 0) return;
    std::ofstream f(std::filesystem::path(folder) / "equipment_icons.txt");
    if (!f) return;
    int bad = 0;
    auto check = [&](const char* what, uint32_t file) {
        const bool ok = EquipmentIcons::Get(dat, dev, file) != nullptr;
        bad += ok ? 0 : 1;
        f << std::format("{} 0x{:x} {}\n", ok ? "ok  " : "FAIL", file, what);
    };
    for (const TypeInfo& t : kMainTypes) check(t.name, t.skin);
    for (const TypeInfo& t : kOffTypes) check(t.name, t.skin);
    for (const PartIcon& p : kPartIcons) check(p.part, p.file);
    check("Inscription: all weapons", InscriptionIconId(InscriptionGroup::AllWeapons));
    check("Inscription: martial weapons", InscriptionIconId(InscriptionGroup::Martial));
    check("Inscription: spellcasting weapons", InscriptionIconId(InscriptionGroup::Spellcasting));
    check("Inscription: focus items or shields", InscriptionIconId(InscriptionGroup::FocusOrShield));
    f << std::format("{} failed\n", bad);
}

void DrawEquipmentWindow(ILiveSource& live, const LiveWeaponSet (&sets)[4], int active, ID3D11Device* dev, DATManager* dat)
{
    // DEV: GWO_TRIALS_OPEN_EQUIPMENT=<1..4> opens the window on that set at the start (a picture of it
    // without real input; developer builds only). GWO_TRIALS_EQUIPMENT_DEMO: see s_demo.
    static bool s_envChecked = false;
    static bool s_logIcons = false;
    if (!s_envChecked)
    {
        s_envChecked = true;
        char v[16] = {};
        if (GuiGlobalConstants::IsDeveloperMode() && GetEnvironmentVariableA("GWO_TRIALS_OPEN_EQUIPMENT", v, sizeof(v)) > 0 && v[0] >= '1' && v[0] <= '4')
        {
            s_editorOpen = true;
            s_editorSet = v[0] - '1';
            s_logIcons = true;
        }
        char d[16] = {};
        if (GuiGlobalConstants::IsDeveloperMode() && GetEnvironmentVariableA("GWO_TRIALS_EQUIPMENT_DEMO", d, sizeof(d)) > 0)
        {
            s_demo = d;
            s_demoOpenPending = true;
        }
    }
    if (!s_editorOpen) return;
    if (s_logIcons && dat)
    {
        s_logIcons = false;
        LogIconCheck(dat, dev);
    }

    constexpr float kColW = 300.f, kGap = 22.f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.42f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Text, kGameText);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.f, 14.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    bool open = true;
    if (ImGui::Begin("Equipment##live_equipment", &open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize))
    {
        // The frame: dark body, the team frame art, the title bar with its close button (the skills window's).
        const ImVec2 A0 = ImGui::GetWindowPos();
        const ImVec2 A1(A0.x + ImGui::GetWindowSize().x, A0.y + ImGui::GetWindowSize().y);
        ImDrawList* fdl = ImGui::GetWindowDrawList();
        fdl->PushClipRect(A0, A1, false);
        fdl->AddRectFilled(ImVec2(A0.x + 10.f, A0.y + 32.f), ImVec2(A1.x - 10.f, A1.y - 14.f), IM_COL32(14, 14, 18, 0xEE));
        DrawGameNineSlice(fdl, LoadGameUITexture(dev, "Window\\ui_window_frame_team.png"), ImVec2(A0.x, A0.y + 32.f), A1, 128.f, 128.f);
        DrawGameNineSlice(fdl, LoadGameUITexture(dev, "Window\\ui_window_titlebar.png"), A0, ImVec2(A1.x, A0.y + 32.f), 128.f, 32.f);
        const float bandY = A0.y + 12.f;
        fdl->AddText(ImVec2(A0.x + 19.f, bandY + (20.f - ImGui::GetFontSize()) * 0.5f), kTitleText, "Equipment");
        const ImVec2 x0(A1.x - 3.f - 24.f, bandY + 2.f), x1(x0.x + 16.f, x0.y + 16.f);
        const ImGuiIO& io = ImGui::GetIO();
        const bool xHover = ImGui::IsWindowHovered() && io.MousePos.x >= x0.x && io.MousePos.x < x1.x && io.MousePos.y >= x0.y && io.MousePos.y < x1.y;
        if (ImTextureID btn = LoadGameUITexture(dev, "Window\\ui_window_buttons.png"))
        {
            const int cell = xHover ? (io.MouseDown[0] ? 2 : 1) : 0;
            fdl->AddImage(btn, x0, x1, ImVec2(cell * 16.f / 128.f, 0.f), ImVec2((cell + 1) * 16.f / 128.f, 1.f));
        }
        if (xHover && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) open = false;
        fdl->PopClipRect();

        const float contentW = kColW * 2.f + kGap;
        ImGui::SetCursorPosY(42.f);

        // The four sets, as the bar draws them: the set in hand lit, the edited one underlined in gold.
        {
            constexpr float kPitch = 78.f;
            const float ovalW = kIdleW * kActiveMul;
            const float rowH = ovalW * kOvalV + 14.f;
            const ImVec2 r0 = ImGui::GetCursorScreenPos();
            const float startX = r0.x + (contentW - kPitch * 4.f) * 0.5f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int i = 0; i < 4; ++i)
            {
                const LiveWeaponSet& set = sets[i];
                const ImVec2 c(startX + kPitch * (static_cast<float>(i) + 0.5f), r0.y + ovalW * kOvalV * 0.5f + 2.f);
                const float w = i == active ? ovalW : kIdleW;
                ImGui::SetCursorScreenPos(ImVec2(c.x - kPitch * 0.5f, r0.y));
                ImGui::InvisibleButton(std::format("##eset{}", i).c_str(), ImVec2(kPitch, rowH));
                const bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) s_editorSet = i;
                if (i == s_editorSet)
                    dl->AddRectFilled(ImVec2(c.x - 22.f, r0.y + rowH - 5.f), ImVec2(c.x + 22.f, r0.y + rowH - 3.f), IM_COL32(0xE8, 0xC0, 0x40, 0xE0),
                                      1.f);
                DrawSetOval(dl, dat, dev, set, i, c, w, i == active || hovered, i == active);
                if (hovered)
                {
                    const std::string footer = std::format("Weapon set {}{}", i + 1, i == active ? " (in hand)" : set.used ? "" : " (empty)");
                    ShowSetTooltip(set, 0, footer.c_str());
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + rowH + 6.f));
        }

        // The edited set's two hands, side by side.
        LiveWeaponSet set = sets[s_editorSet];
        bool changed = false;
        const ImVec2 handsTop = ImGui::GetCursorScreenPos();
        changed |= HandPanel("main", "Main hand", true, MainHand(set), sets[s_editorSet], 0, kColW, dat, dev);
        const float mainBottom = ImGui::GetItemRectMax().y;
        const TypeInfo* mainInfo = FindType(set.mainType);
        const bool twoHanded = mainInfo && mainInfo->twoHanded;
        if (twoHanded && set.offType != 0)
        {
            set.offType = 0;   // a two-handed weapon takes the off hand
            set.offModel = 0;
            set.offSuffix = set.offInscription = 0;
            changed = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(handsTop.x + kColW + kGap, handsTop.y));
        changed |= HandPanel("off", "Off hand", false, OffHand(set), sets[s_editorSet], twoHanded ? set.mainModel : 0, kColW, dat, dev);
        const float offBottom = ImGui::GetItemRectMax().y;
        ImGui::SetCursorScreenPos(ImVec2(handsTop.x, (std::max)(mainBottom, offBottom) + 10.f));

        // The set in hand cannot be emptied (the source refuses it too).
        if (changed && s_editorSet == active && set.mainType == 0 && set.offType == 0) changed = false;
        if (changed) live.EditPlayerWeaponSet(s_editorSet, set);

        SectionTitle("This set adds", contentW);
        const LiveWeaponSet& now = sets[s_editorSet];
        std::string stats;
        auto add = [&](const char* what, int v, const char* when = "") {
            if (v != 0) stats += std::format("{}{} {:+d}{}", stats.empty() ? "" : "    ", what, v, when);
        };
        add("Health", now.health);
        add("Health", now.healthWhenEnchanted, " (while Enchanted)");
        add("Health", now.healthWhenHexed, " (while Hexed)");
        add("Health", now.healthWhenInStance, " (while in a Stance)");
        add("Energy", now.energy);
        add("Energy regeneration", now.energyRegen);
        add("Armor", now.armour);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + contentW);
        ImGui::TextColored(stats.empty() ? kMuted : ToVec(Equipment::kStatColor), "%s", stats.empty() ? "Nothing" : stats.c_str());
        ImGui::Dummy(ImVec2(0.f, 2.f));
        ImGui::TextColored(kMuted, "Only Health, Energy, Energy regeneration and Armor are simulated so far.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(contentW, 10.f));   // clear of the frame's lower edge

        if (s_demo == "tooltip")
        {
            const ImVec2 at(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x + 12.f, ImGui::GetWindowPos().y + 40.f);
            ShowSetTooltip(now, 0, std::format("Weapon set {}{}", s_editorSet + 1, s_editorSet == active ? " (in hand)" : "").c_str(), &at);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!open) s_editorOpen = false;
}
}

int TakeLiveWeaponSetClick()
{
    const int c = s_clicked;
    s_clicked = -1;
    return c;
}

void DrawLiveWeaponSetBar(ILiveSource& live, ID3D11Device* device, DATManager* dat)
{
    LiveWeaponSet sets[4];
    const int active = live.PlayerWeaponSets(sets);
    if (active < 0 || !device) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size(kCellW * 2.f, kCellH * 2.f + kCellH * 0.25f);
    ImVec2 pos(vp->WorkPos.x + vp->WorkSize.x - size.x - kMargin, vp->WorkPos.y + vp->WorkSize.y - size.y - kMargin);
    // A panel already in the corner (the party window, last frame's place): the bar stands on top of it,
    // still against the right edge.
    for (int pass = 0; pass < 4; ++pass)
    {
        bool moved = false;
        for (ImGuiWindow* w : GImGui->Windows)
        {
            if (!w->WasActive || w->Hidden || (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup)) ||
                std::strcmp(w->Name, "##live_weapon_sets") == 0 || std::strstr(w->Name, "##live_equipment") ||
                w->Size.x > vp->WorkSize.x * 0.6f)
                continue;   // a full-screen overlay is not a panel, nor is the equipment window
            const ImRect r(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y));
            if (r.Overlaps(ImRect(pos, ImVec2(pos.x + size.x, pos.y + size.y))) && r.Max.x > vp->WorkPos.x + vp->WorkSize.x * 0.5f)
            {
                pos.y = r.Min.y - size.y - 4.f;
                moved = true;
            }
        }
        if (!moved) break;
    }
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##live_weapon_sets", nullptr, flags))
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < 4; ++i)
        {
            const LiveWeaponSet& set = sets[i];
            const bool inHand = i == active;
            const float w = inHand ? kCellW : kIdleW;
            const float h = w * kOvalV;
            // The cell's centre: F1 F2 on top, F3 F4 below.
            const ImVec2 c(pos.x + kCellW * (0.5f + static_cast<float>(i % 2)), pos.y + kCellH * (0.5f + static_cast<float>(i / 2)));
            const ImVec2 o0(c.x - w * 0.5f, c.y - h * 0.5f);

            ImGui::SetCursorScreenPos(o0);
            ImGui::InvisibleButton(std::format("##wset{}", i).c_str(), ImVec2(w, h));
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && set.used && !inHand) s_clicked = i;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !set.used))
            {
                s_editorOpen = true;
                s_editorSet = i;
            }

            DrawSetOval(dl, dat, device, set, i, c, w, inHand || (hovered && set.used), inHand);

            if (hovered)
            {
                const std::string footer = set.used ? std::format("Weapon set {}{} - right-click to change", i + 1, inHand ? " (in hand)" : "")
                                                    : std::format("Weapon set {} (empty): click to equip", i + 1);
                ShowSetTooltip(set, 0, footer.c_str());
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    DrawEquipmentWindow(live, sets, active, device, dat);
}
