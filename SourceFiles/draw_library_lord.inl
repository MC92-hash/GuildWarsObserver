// =================================================================================================
// THE LORD DAMAGE VIEW  [2026-11-28]
//
// The Statistics page's third view (Explore | Table | Lord damage), opened from the Library sidebar's
// Lord damage "Details" link. The approved mock-up: claude.ai artifact NHVWaDWN5krcJL4D6jrMny, board
// "Lord damage page".
//
//   The verdict      each guild's score (its cape, "on <lord>"), the recorded lord damage, the gap.
//   The score        Score race (the two scores over time; at the right of each, nothing) or Tug of
//                    war (each guild's damage by kind and what the defenders prevented, by mechanism);
//                    "Show estimated damage without protection" adds the dotted lines.
//   Two panels       per guild: a split bar (counted / taken off / prevented), the damage by kind with
//                    every skill as an icon and what went through armour penetration, the ladder from
//                    damage to score, and what the defenders prevented, by mechanism, at their ranks.
//   Skills on the    per lord: the damage on it by skill, and the defenders' skills on it: cost, damage
//   lords            prevented, score benefit, casters - damage and prevention on ONE scale.
//
// Data: s_analysis.lordEvents (the aggressiveness model's events: every hit, Entourage hit, life
// steal, own-lord cast), lordParts and the ledger's Damage / Prevented lines (a prevented line names
// the hit it came off and what armour penetration got through). Every modelled value carries ≈;
// recorded damage, the score and cast costs do not. Never an annotation the recording does not show.
//
// Included by draw_library_stats.inl's page (namespace libpage), after the Statistics page's helpers.
// =================================================================================================

namespace lordv
{
// Colours: the guilds, the four ways damage is prevented (each with its own pattern), the rest muted.
constexpr ImU32 TeamCol[3]  = { 0, Hex(0x86acd0), Hex(0xcf958b) };
constexpr ImU32 TeamLine[3] = { 0, Hex(0xd6e7f7), Hex(0xf7d6d0) };
constexpr ImU32 TagCol[3]   = { 0, Hex(0xa6cdf2), Hex(0xf2b3a8) };
constexpr ImU32 MechCol[4]  = { Hex(0x7a9a55), Hex(0x9576bd), Hex(0xc4843c), Hex(0x2f98a0) };   // blocked, armour, reduction, absorbed
constexpr ImU32 Dmg         = Hex(0xa08a5c);
constexpr ImU32 Cost        = Hex(0x8b8478);
constexpr ImU32 Lost        = Hex(0x161614);
constexpr ImU32 Head        = Hex(0xe1dacd);
constexpr ImU32 Body        = Hex(0xc9beaa);
constexpr ImU32 Faint       = Hex(0x8f897b);
constexpr ImU32 Strong      = Hex(0xece6d9);
constexpr ImU32 Cream       = Hex(0xf6ecd2);
constexpr ImU32 GoldText    = Hex(0xd5bc80);
constexpr ImU32 Grid        = Hex(0xffffff, 0x10);
constexpr ImU32 PanelBg     = Hex(0x10100e, 0x6b);
constexpr ImU32 PanelLine   = Hex(0xffffff, 0x14);
constexpr const char* MechName[4] = { "Blocked", "Armour", "Damage reduction", "Hit absorbed" };
constexpr float kShade[5] = { 0.93f, 0.66f, 0.44f, 0.27f, 0.16f };   // the guild's colour, lighter for smaller kinds
enum Kind { kWeapon, kAttack, kSpell, kNone, kSteal, kKinds };
constexpr const char* KindName[kKinds] = { "Weapon hits", "Attack skills", "Spells and signets", "No skill named",
                                           "Life steal (doesn\xE2\x80\x99t count)" };
enum Cause { cDamage, cEntourage, cSteal, cBlind, cBlock, cEnchant, cWeaponSpell };

constexpr const char* kMinus  = "\xE2\x88\x92";
constexpr const char* kApprox = "\xE2\x89\x88";
constexpr const char* kTimes  = "\xC3\x97";
constexpr const char* kDot    = " \xC2\xB7 ";
constexpr const char* kApos   = "\xE2\x80\x99";

static std::string Num(long v) { return (v < 0 ? std::string(kMinus) : std::string()) + Thousands((size_t)std::labs(v)); }
static std::string Signed(long v) { return (v < 0 ? std::string(kMinus) : std::string("+")) + Thousands((size_t)std::labs(v)); }
static long Round10(double v) { return (long)std::lround(v / 10.0) * 10; }

// ─── The model, built once per match ─────────────────────────────────────────────────────────────

struct Row { long amount = 0; int hits = 0; std::map<std::string, int> users; };
struct SkillCast { int count = 0; long points = 0; std::map<int, int> by; };
struct PrevSkill { double amount = 0, scored = 0; int blocks = 0; };

struct Side   // a guild attacking the other guild's lord
{
    int lord = 0;                       // the lord it hits
    long recorded = 0, score = 0;       // Health on that lord; the aggressiveness score
    long kind[kKinds] = {};
    std::map<int, long> kindSkills[kKinds];                 // skill -> Health, per kind
    std::map<int, long> penBy[kKinds];                      // attacker -> Health through armour penetration
    long penKind[kKinds] = {};
    std::map<int, SkillCast> casts;                         // own-lord casts by skill
    long castPoints = 0; int enchants = 0, weaponSpells = 0;
    long steal = 0, entHealth = 0, entPoints = 0, rest = 0;
    double mech[4] = {};                                    // prevented by the OTHER guild on this lord
    std::map<int, PrevSkill> mechSkills[4];
    double prevented = 0;
    std::vector<float> score_s, possible_s;                 // per second from t0
    std::vector<float> kind_s[kKinds], mech_s[4];
    std::map<int, Row> lordRows;                            // the damage on the lord, by key (-1 weapon, 0 none, skill)
};

struct Model
{
    std::string folder;
    size_t stamp = 0;
    bool valid = false;
    int t0 = 0, t1 = 0;                // the chart's seconds, match clock
    Side side[3];                       // [team 1 / 2]: that guild's attack on the other's lord
    std::vector<std::pair<int, int>> pushes[3];   // seconds a guild was hitting the enemy lord
    int leadChange = -1, leadTeam = 0;
};
static Model s_lm;

static int MechOf(uint8_t preventGroup)
{
    switch (preventGroup)   // 1 + DamageAttribution::PreventGroup {Flat, Percent, Armour, Cap, Block}; 0 with a heal
    {
    case 5:  return 0;
    case 3:  return 1;
    case 1: case 2: case 4: return 2;
    default: return 3;
    }
}

static bool IsAttackType(int type) { return (type >= 2 && type <= 12) || type == 31; }

static int KindOfKey(const SkillDatabaseView& view, bool weapon, int skill, int rowKey)
{
    if (weapon) return kWeapon;
    if (skill <= 0 || rowKey != 0) return kNone;
    const SkillInfo* si = view.Get(skill);
    return si && IsAttackType(si->type) ? kAttack : kSpell;
}

static bool HealsAfterHit(BattleLedger::Host& host, int skill)   // Spirit Bond: the hit still counts
{
    return host.SkillName(skill).rfind("Spirit Bond", 0) == 0;
}

static void Build(const MatchMeta& m, const BattleLedger::Data& d, BattleLedger::Host& host)
{
    const MatchAnalysisJob& job = s_analysis;
    const size_t stamp = job.lordEvents.size() * 131 + d.lines.size();
    if (s_lm.folder == m.folder_name && s_lm.stamp == stamp) return;
    s_lm = Model();
    s_lm.folder = m.folder_name;
    s_lm.stamp = stamp;
    if (job.lordEvents.empty() || !job.lordAgent[1] || !job.lordAgent[2]) return;
    const SkillDatabaseView& view = GetMatchSkillView(m);

    float first = 1e9f, last = 0.f;
    for (const auto& e : job.lordEvents) { first = std::min(first, e.t); last = std::max(last, e.t); }
    s_lm.t0 = std::max(0, (int)std::floor(first / 60.f) * 60);
    s_lm.t1 = std::max((int)std::ceil(last), s_lm.t0 + 60);
    const int n = s_lm.t1 - s_lm.t0 + 1;
    auto sec = [&](float t) { return std::clamp((int)std::floor(t) - s_lm.t0, 0, n - 1); };

    for (int tm = 1; tm <= 2; ++tm)
    {
        Side& s = s_lm.side[tm];
        s.lord = job.lordAgent[3 - tm];
        s.score = job.lordDamage[tm - 1];
        s.steal = job.lordParts[tm][cSteal][1];
        s.entHealth = job.lordParts[tm][cEntourage][1];
        s.entPoints = job.lordParts[tm][cEntourage][2];
        s.score_s.assign(n, 0.f); s.possible_s.assign(n, 0.f);
        for (auto& v : s.kind_s) v.assign(n, 0.f);
        for (auto& v : s.mech_s) v.assign(n, 0.f);
    }

    // The score events: the race, the own-lord casts, the life steal to find among the ledger's hits.
    std::multimap<std::pair<int, long>, int> stealAt;           // (actor, 10 ms tick) -> health
    for (const auto& e : job.lordEvents)
    {
        if (e.team < 1 || e.team > 2) continue;
        Side& s = s_lm.side[e.team];
        s.score_s[sec(e.t)] += e.points;
        if (e.cause == cEnchant || e.cause == cWeaponSpell)
        {
            SkillCast& c = s.casts[e.skill];
            c.count++; c.points += (long)std::lround(e.points); c.by[e.actor]++;
            s.castPoints += (long)std::lround(e.points);
            (e.cause == cEnchant ? s.enchants : s.weaponSpells)++;
        }
        else if (e.cause == cSteal)
            stealAt.insert({ { e.actor, (long)std::lround(e.t * 100.f) }, (int)std::lround(e.health) });
    }

    // The ledger: the damage on each lord by kind (life steal its own), and what was prevented on it.
    for (const auto& ln : d.lines)
    {
        int tm = 0;
        if (ln.dst == s_lm.side[1].lord) tm = 1;
        else if (ln.dst == s_lm.side[2].lord) tm = 2;
        if (!tm) continue;
        Side& s = s_lm.side[tm];
        if (ln.kind == BattleLedger::Kind::Damage)
        {
            int k = KindOfKey(view, ln.weaponHit, ln.skillId, ln.rowKey);
            const long tick = (long)std::lround(ln.t * 100.f);
            for (long dt = -6; dt <= 6 && k != kSteal; ++dt)
            {
                auto range = stealAt.equal_range({ ln.src, tick + dt });
                for (auto it = range.first; it != range.second; ++it)
                    if (std::abs(it->second - ln.amount) <= 1) { k = kSteal; stealAt.erase(it); break; }
            }
            s.recorded += ln.amount;
            s.kind[k] += ln.amount;
            const int key = ln.weaponHit ? -1 : (ln.skillId > 0 && ln.rowKey == 0 ? ln.skillId : 0);
            if (key > 0) s.kindSkills[k][key] += ln.amount;
            s.kind_s[k][sec(ln.t)] += (float)ln.amount;
            s.possible_s[sec(ln.t)] += (float)ln.amount;
            Row& r = s.lordRows[key];
            r.amount += ln.amount; r.hits++; r.users[AgentName(d, ln.src)]++;
        }
        else if (ln.kind == BattleLedger::Kind::Prevented && !HealsAfterHit(host, ln.skillId))
        {
            const int mech = MechOf(ln.preventGroup);
            s.mech[mech] += ln.amount;
            s.prevented += ln.amount;
            PrevSkill& p = s.mechSkills[mech][ln.skillId];
            p.amount += ln.amount;
            if (mech == 0) p.blocks += std::max(1, ln.count);
            s.mech_s[mech][sec(ln.t)] += (float)ln.amount;
            s.possible_s[sec(ln.t)] += (float)ln.amount;
            if (ln.penetrated > 0)
            {
                const SkillInfo* hs = ln.hitKey > 0 ? view.Get(ln.hitKey) : nullptr;
                const int k = ln.hitKey == -1 ? kWeapon : hs ? (IsAttackType(hs->type) ? kAttack : kSpell) : kNone;
                s.penKind[k] += ln.penetrated;
                s.penBy[k][ln.hitBy] += ln.penetrated;
            }
        }
    }

    // A prevented line in score points: the rate its hit scored at (x0.2 under Entourage, 0 life steal).
    std::map<std::pair<int, long>, float> rate;   // (actor, 10 ms tick) -> points / Health
    for (const auto& e : job.lordEvents)
        if ((e.cause == cDamage || e.cause == cEntourage || e.cause == cSteal) && e.health > 0.f)
            rate[{ e.actor, (long)std::lround(e.t * 100.f) }] = e.points / e.health;
    for (const auto& ln : d.lines)
    {
        if (ln.kind != BattleLedger::Kind::Prevented || HealsAfterHit(host, ln.skillId)) continue;
        int tm = ln.dst == s_lm.side[1].lord ? 1 : ln.dst == s_lm.side[2].lord ? 2 : 0;
        if (!tm) continue;
        float r = 1.f;
        if (MechOf(ln.preventGroup) != 0)
        {
            const long tick = (long)std::lround(ln.t * 100.f);
            for (long dt = -6; dt <= 6; ++dt)
                if (auto it = rate.find({ ln.hitBy, tick + dt }); it != rate.end()) { r = it->second; break; }
        }
        s_lm.side[tm].mechSkills[MechOf(ln.preventGroup)][ln.skillId].scored += ln.amount * r;
    }

    // Running totals; pushes (hits on the enemy lord no more than 12 s apart, worth 400+); the lead.
    for (int tm = 1; tm <= 2; ++tm)
    {
        Side& s = s_lm.side[tm];
        for (int i = 1; i < n; ++i)
        {
            s.score_s[i] += s.score_s[i - 1];
            s.possible_s[i] += s.possible_s[i - 1];
            for (auto& v : s.kind_s) v[i] += v[i - 1];
            for (auto& v : s.mech_s) v[i] += v[i - 1];
        }
        s.rest = s.score - (s.recorded + s.castPoints - s.steal - (s.entHealth - s.entPoints));
        float a = -1.f, b = -1.f, pts = 0.f;
        for (const auto& e : job.lordEvents)
        {
            if (e.team != tm || (e.cause != cDamage && e.cause != cEntourage)) continue;
            if (a >= 0.f && e.t - b <= 12.f) { b = e.t; pts += e.points; continue; }
            if (a >= 0.f && pts >= 400.f) s_lm.pushes[tm].push_back({ (int)a, (int)b });
            a = b = e.t; pts = e.points;
        }
        if (a >= 0.f && pts >= 400.f) s_lm.pushes[tm].push_back({ (int)a, (int)b });
    }
    int lead = 0;
    for (int i = 0; i < n; ++i)
    {
        const float d1 = s_lm.side[1].score_s[i], d2 = s_lm.side[2].score_s[i];
        const int now = d1 > d2 ? 1 : d2 > d1 ? 2 : lead;
        if (now != lead && lead != 0) { s_lm.leadChange = s_lm.t0 + i; s_lm.leadTeam = now; }
        if (now != 0) lead = now;
    }
    s_lm.valid = true;
}

// ─── Small parts ────────────────────────────────────────────────────────────────────────────────

static std::string Tag(const BattleLedger::Data& d, int team)   // "HC", without the brackets
{
    std::string t = SideTag(d, team - 1);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') t = t.substr(1, t.size() - 2);
    return t;
}

// "HC’s" in the guild's colour, then the rest; returns the x after it.
static float TagText(ImDrawList* dl, const Type& t, float x, float y, const BattleLedger::Data& d, int team, const std::string& rest, ImU32 c)
{
    const std::string tag = Tag(d, team);
    Text(dl, t, ImVec2(x, y), TagCol[team], tag.c_str());
    x += TextW(t, tag.c_str());
    Text(dl, t, ImVec2(x, y), c, rest.c_str());
    return x + TextW(t, rest.c_str());
}

// The fills of the four ways damage is prevented, and of "taken off the score": a pattern each.
static void Pattern(ImDrawList* dl, ImVec2 a, ImVec2 b, int mech)
{
    if (b.x - a.x < 0.5f || b.y - a.y < 0.5f) return;
    dl->PushClipRect(a, b, true);
    const float h = b.y - a.y;
    if (mech < 0)   // taken off the score: dark diagonal stripes over the guild's colour beneath
    {
        dl->AddRectFilled(a, b, WithAlpha(Lost, 0.45f));
        for (float x = a.x - h; x < b.x; x += Px(5))
            dl->AddLine(ImVec2(x, b.y), ImVec2(x + h, a.y), WithAlpha(Lost, 0.9f), Px(2.2f));
    }
    else
    {
        const ImU32 c = MechCol[mech];
        dl->AddRectFilled(a, b, WithAlpha(c, 0.26f));
        if (mech == 0)
            for (float x = a.x - h; x < b.x; x += Px(7)) dl->AddLine(ImVec2(x, b.y), ImVec2(x + h, a.y), c, Px(3));
        else if (mech == 1)
            for (float y = a.y + Px(3); y < b.y; y += Px(6))
                for (float x = a.x + Px(3); x < b.x; x += Px(6)) dl->AddCircleFilled(ImVec2(x, y), Px(1.6f), c, 8);
        else if (mech == 2)
            for (float y = a.y + Px(1); y < b.y; y += Px(5)) dl->AddRectFilled(ImVec2(a.x, y), ImVec2(b.x, y + Px(1.6f)), c);
        else
            for (float x = a.x - h; x < b.x; x += Px(6))
            {
                dl->AddLine(ImVec2(x, b.y), ImVec2(x + h, a.y), c, Px(1));
                dl->AddLine(ImVec2(x, a.y), ImVec2(x + h, b.y), c, Px(1));
            }
    }
    dl->PopClipRect();
}

static ImTextureID GameUi(const char* rel)
{
    EnsureTextureBasePath();
    return GetTextureCache().GetTexture(ui::TextureBasePath() + "\\Textures\\" + rel);
}
static ImTextureID IconDamage(BattleLedger::Host& h)    { return h.UiIcon("damagedone.png"); }
static ImTextureID IconWeapon(BattleLedger::Host& h)    { return h.UiIcon("kill.png"); }
static ImTextureID IconEnchant()                       { return GameUi("Game_UI\\Enchanted.png"); }
static ImTextureID IconEntourage()                     { return GameUi("Skill_Icons\\Monster_skill.jpg"); }
static ImTextureID IconSteal()                         { return GetSkillIcon(657); }
static ImTextureID LordIcon(BattleLedger::Host& h, int team) { return h.UiIcon(Team::IsRed(team) ? "redguildlord.png" : "blueguildlord.png"); }

static void Icon(ImDrawList* dl, ImTextureID tex, ImVec2 p, float s, const char* tip = nullptr)
{
    if (!tex) { dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), Hex(0x3a3730), Px(2)); }
    else dl->AddImage(tex, p, ImVec2(p.x + s, p.y + s));
    if (tip && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + s, p.y + s)) && ImGui::IsWindowHovered()) ImGui::SetTooltip("%s", tip);
}

// A player's solved rank in an attribute (the attribute solver's best), -1 unknown.
static int RankOf(const MatchMeta& m, const BattleLedger::Data& d, int agent, int attribute)
{
    const std::string name = AgentName(d, agent);
    for (const auto& [partyId, party] : m.parties)
        for (const auto& p : party.players)
        {
            if (p.encoded_name.empty() || SanitizePlayerName(p.encoded_name) != name) continue;
            auto it = s_analysis.builds.find(partyId + ":" + std::to_string(p.player_number));
            if (it == s_analysis.builds.end()) return -1;
            for (const SolvedAttribute& a : it->second)
                if (a.id == attribute) return a.best;
            return -1;
        }
    return -1;
}

// The skill's numbers at a rank, as the defenders cast it: "−17 per hit", "+43 armour · +10 regen".
// Each format names the description's "A...B" ranges by index; unknown skills show nothing.
static std::string ValueAtRank(const SkillInfo* si, int skill, int rank)
{
    if (!si || rank < 0) return {};
    std::vector<std::pair<float, float>> ranges;
    const std::string& ds = si->description;
    for (size_t i = 0; (i = ds.find("...", i)) != std::string::npos; i += 3)
    {
        size_t a = i;
        while (a > 0 && std::isdigit((unsigned char)ds[a - 1])) --a;
        size_t b = i + 3;
        while (b < ds.size() && std::isdigit((unsigned char)ds[b])) ++b;
        if (a < i && b > i + 3) ranges.push_back({ std::stof(ds.substr(a, i - a)), std::stof(ds.substr(i + 3, b - i - 3)) });
    }
    auto v = [&](size_t i) { return i < ranges.size() ? std::to_string((int)std::lround(ranges[i].first + (ranges[i].second - ranges[i].first) * rank / 15.f)) : std::string("?"); };
    const std::string name = si->name;
    auto is = [&](const char* n) { return name.rfind(n, 0) == 0; };
    if (is("Shielding Hands"))        return std::string(kMinus) + v(0) + " per hit";
    if (is("Reversal of Fortune"))    return "up to " + v(0);
    if (is("Life Sheath"))            return "up to " + v(1);
    if (is("Reversal of Damage"))     return "up to " + v(0);
    if (is("Vengeful Weapon"))        return "steals up to " + v(0);
    if (is("Spirit Bond"))            return "heals " + v(0);
    if (is("Shield of Deflection"))   return "75% block \xC2\xB7 +" + v(1) + " armour";
    if (is("Shield of Regeneration")) return "+" + v(2) + " armour \xC2\xB7 +" + v(1) + " regen";
    if (is("Guardian"))               return "50% block \xC2\xB7 " + v(0) + " s";
    if (is("Resilient Weapon"))       return "+24 armour if hexed";
    if (is("Balthazar's Aura"))       return v(0) + " damage/s to foes";
    (void)skill;
    return {};
}

static std::string RankChip(const MatchMeta& m, const BattleLedger::Data& d, const SkillDatabaseView& view, int skill, const SkillCast* cast)
{
    const SkillInfo* si = view.Get(skill);
    if (!si || !cast) return {};
    std::vector<std::string> vals;
    for (const auto& [agent, n] : cast->by)
    {
        const std::string v = ValueAtRank(si, skill, RankOf(m, d, agent, si->attribute));
        if (!v.empty() && std::find(vals.begin(), vals.end(), v) == vals.end()) vals.push_back(v);
    }
    if (vals.empty()) return {};
    std::string out = vals[0];
    if (vals.size() == 2)
    {
        // Only the number differs ("−17 per hit", "−18 per hit"): "−17 / 18 per hit".
        auto split = [](const std::string& v) {
            size_t i = v.rfind(kMinus, 0) == 0 ? strlen(kMinus) : (v[0] == '+' ? 1 : 0), j = i;
            while (j < v.size() && std::isdigit((unsigned char)v[j])) ++j;
            return std::make_pair(v.substr(i, j - i), v.substr(j));
        };
        const auto a = split(vals[0]), b = split(vals[1]);
        out = !a.first.empty() && !b.first.empty() && a.second == b.second
            ? vals[0].substr(0, vals[0].size() - a.second.size()) + " / " + b.first + a.second
            : vals[0] + " / " + vals[1];
    }
    return out;
}

// Players with their primary profession's icon, "Name ×N" or "Name +N", wrapped at xmax.
static float People(ImDrawList* dl, BattleLedger::Host& host, const BattleLedger::Data& d, float x, float y, float xmax,
                    const std::vector<std::pair<int, std::string>>& items, const Type& t, ImU32 c)
{
    float xi = x;
    const float ico = Px(11), lh = LineH(t.size / s_k);
    for (size_t i = 0; i < items.size(); ++i)
    {
        const float w = ico + Px(3) + TextW(t, items[i].second.c_str()) + (i ? Px(12) : 0.f);
        if (i && xi + w > xmax) { xi = x; y += lh + Px(2); }
        else if (i) { Text(dl, t, ImVec2(xi + Px(3), y), Hex(0x6f6a60), "\xC2\xB7"); xi += Px(12); }
        const auto it = d.agents.find(items[i].first);
        if (it != d.agents.end())
            if (ImTextureID p = host.ProfIcon(it->second.prof)) dl->AddImage(p, ImVec2(xi, y + (lh - ico) * 0.5f), ImVec2(xi + ico, y + (lh + ico) * 0.5f));
        Text(dl, t, ImVec2(xi + ico + Px(3), y), c, items[i].second.c_str());
        xi += ico + Px(3) + TextW(t, items[i].second.c_str());
    }
    return y + lh;
}

// Skill icons on a fixed grid (every icon on a column), each with its label; returns the y below.
static float IconGrid(ImDrawList* dl, BattleLedger::Host& host, float x, float y, float xmax, float cell,
                      const std::vector<std::pair<int, std::string>>& items)
{
    const int per = std::max(1, (int)((xmax - x) / cell));
    const float ico = Px(16);
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i && i % per == 0) y += Px(21);
        const float xi = x + (i % per) * cell;
        const std::string tip = host.SkillName(items[i].first) + kDot + items[i].second;
        Icon(dl, host.SkillIcon(items[i].first), ImVec2(xi, y), ico, tip.c_str());
        Text(dl, T(10.5f), ImVec2(xi + ico + Px(4), y + (ico - LineH(10.5f)) * 0.5f), Hex(0xa8a092), items[i].second.c_str());
    }
    return y + ico;
}

// A two- or three-way switch with the Statistics page's look.
static int Switch(ImDrawList* dl, const char* id, ImVec2 p, const char* const* labels, int n, int on, float* widthOut)
{
    const Type t = T(12);
    const float inner = Px(3), padX = Px(14), padY = Px(5);
    float w = 2.0f + inner * (n + 1);
    for (int k = 0; k < n; ++k) w += padX * 2.0f + TextW(t, labels[k]);
    const float h = padY * 2.0f + t.size;
    const ImVec2 box1(p.x + w, p.y + 2.0f + inner * 2.0f + h);
    dl->AddRectFilled(p, box1, v48::ToggleBg, Px(6));
    Outline(dl, p, box1, v48::ToggleLine, Px(6));
    int clicked = -1;
    float x = p.x + 1.0f + inner;
    for (int k = 0; k < n; ++k)
    {
        const float bw = padX * 2.0f + TextW(t, labels[k]);
        const ImVec2 q(x, p.y + 1.0f + inner);
        ImGui::SetCursorScreenPos(q);
        ImGui::PushID(id); ImGui::PushID(k);
        if (ImGui::InvisibleButton("##sw", ImVec2(bw, h))) clicked = k;
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID(); ImGui::PopID();
        if (hov && k != on) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (k == on) dl->AddRectFilled(q, ImVec2(q.x + bw, q.y + h), Hex(0xd9b45e, 0x29), Px(5));
        else if (hov) dl->AddRectFilled(q, ImVec2(q.x + bw, q.y + h), v48::HoverBg, Px(5));
        Text(dl, t, ImVec2(q.x + padX, R(q.y + (h - t.size) * 0.5f)), k == on ? Hex(0xf1e3c2) : hov ? v48::Text : Hex(0xa8a092), labels[k]);
        x += bw + inner;
    }
    if (widthOut) *widthOut = w;
    return clicked;
}

static bool Check(ImDrawList* dl, const char* id, ImVec2 p, const char* label, bool on)
{
    const Type t = T(12);
    const float box = Px(11), w = Px(12) + box + Px(8) + Px(26) + Px(8) + TextW(t, label) + Px(12), h = Px(26);
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), on ? Hex(0xd9b45e, 0x29) : hov ? v48::HoverBg : 0, Px(4));
    Outline(dl, p, ImVec2(p.x + w, p.y + h), Hex(0xffffff, 0x24), Px(4));
    const ImVec2 b0(p.x + Px(12), p.y + (h - box) * 0.5f);
    if (on) dl->AddRectFilled(b0, ImVec2(b0.x + box, b0.y + box), Hex(0xd9b45e), Px(2));
    else Outline(dl, b0, ImVec2(b0.x + box, b0.y + box), Faint, Px(2));
    const float lx = b0.x + box + Px(8), ly = p.y + h * 0.5f;
    for (float x = lx; x < lx + Px(26); x += Px(6)) dl->AddCircleFilled(ImVec2(x + Px(1), ly), Px(1.1f), Hex(0xe8e2d6), 6);
    Text(dl, t, ImVec2(lx + Px(34), R(p.y + (h - LineH(12)) * 0.5f)), on ? Hex(0xf1e3c2) : Hex(0xa8a092), label);
    return clicked;
}

static void DashedH(ImDrawList* dl, float x0, float x1, float y, ImU32 c, float dash, float gap, float th)
{
    for (float x = x0; x < x1; x += dash + gap) dl->AddLine(ImVec2(x, y), ImVec2(std::min(x1, x + dash), y), c, th);
}

// Labels at the right of a chart, pushed apart so none touch.
static std::vector<float> Spread(std::vector<float> want, float lo, float hi, float gap)
{
    std::vector<size_t> order(want.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return want[a] < want[b]; });
    std::vector<float> out(want.size());
    float last = -1e9f;
    for (size_t i : order) { const float y = std::max({ want[i], last + gap, lo }); out[i] = y; last = y; }
    const float over = std::max(0.f, last - hi);
    for (float& y : out) y -= over;
    return out;
}

// ─── The verdict ────────────────────────────────────────────────────────────────────────────────

static float DrawVerdict(ImDrawList* dl, const MatchMeta& m, const BattleLedger::Data& d, BattleLedger::Host& host, float x0, float x1, float y)
{
    std::string ft1, ft2;
    ParseFolderTags(m.folder_name, ft1, ft2);
    const GuildLabel g[2] = { GetPartyGuild(m, "1", ft1), GetPartyGuild(m, "2", ft2) };
    const float h = Px(118), midW = Px(460);
    const ImVec2 a(x0, y), b(x1, y + h);
    dl->AddRectFilled(a, b, Hex(0x141412, 0x6b), Px(6));
    Outline(dl, a, b, PanelLine, Px(6));
    const long s1 = s_lm.side[1].score, s2 = s_lm.side[2].score;
    const int win = s1 > s2 ? 1 : s2 > s1 ? 2 : 0;
    const float colW = (x1 - x0 - midW) * 0.5f;
    for (int tm = 1; tm <= 2; ++tm)
    {
        const Side& s = s_lm.side[tm];
        const bool right = tm == 2;
        const float cx0 = right ? x1 - colW : x0, cx1 = right ? x1 : x0 + colW;
        FillLinear(dl, ImVec2(cx0, a.y), ImVec2(cx1, b.y), 0.f, right ? 270.f : 90.f, WithAlpha(TeamCol[tm], 0.10f), WithAlpha(TeamCol[tm], 0.f));
        dl->AddRectFilled(ImVec2(right ? cx1 - Px(3) : cx0, a.y), ImVec2(right ? cx1 : cx0 + Px(3), b.y), TeamCol[tm], Px(2));
        const float capeX = right ? cx1 - Px(30) - Px(34) : cx0 + Px(30);
        if (ImTextureID cape = GetGuildCape(m, g[tm - 1])) ImageContain(dl, cape, 96.f, 192.f, ImVec2(capeX, a.y + Px(24)), ImVec2(capeX + Px(34), a.y + Px(92)));
        const float tx = right ? capeX - Px(18) : capeX + Px(34) + Px(18);
        const std::string name = (g[tm - 1].name.empty() ? g[tm - 1].display : g[tm - 1].name) + (g[tm - 1].tag.empty() ? "" : " [" + g[tm - 1].tag + "]");
        const std::string on = std::string("on ") + Tag(d, 3 - tm) + kApos + "s lord";
        const Type tn = T(13), to = T(11);
        const float nw = TextW(tn, name.c_str()), ow = Px(16) + Px(4) + TextW(to, on.c_str());
        float lx = right ? tx - nw - Px(10) - ow : tx;
        Text(dl, tn, ImVec2(lx, a.y + Px(22)), TagCol[tm], name.c_str());
        lx += nw + Px(10);
        Icon(dl, LordIcon(host, 3 - tm), ImVec2(lx, a.y + Px(22) + (LineH(13) - Px(16)) * 0.5f), Px(16));
        Text(dl, to, ImVec2(lx + Px(20), a.y + Px(22) + LineH(13) - LineH(11)), Faint, on.c_str());
        const Type ts = T(10), tb = T(38);
        const std::string score = Thousands((size_t)std::max(0L, s.score));
        const float sw = TextW(tb, score.c_str());
        Text(dl, ts, ImVec2(right ? tx - TextW(ts, "SCORE") : tx, a.y + Px(44)), Hex(0xa39883), "SCORE");
        float sx = right ? tx - sw : tx;
        if (tm == win && right)
            if (ImTextureID cup = GetCupIcon()) ImageContain(dl, cup, 64.f, 109.f, ImVec2(sx - Px(32), a.y + Px(60)), ImVec2(sx - Px(8), a.y + Px(88)));
        Text(dl, tb, ImVec2(sx, a.y + Px(54)), Strong, score.c_str());
        if (tm == win && !right)
            if (ImTextureID cup = GetCupIcon()) ImageContain(dl, cup, 64.f, 109.f, ImVec2(sx + sw + Px(8), a.y + Px(60)), ImVec2(sx + sw + Px(32), a.y + Px(88)));
        char sub[96];
        snprintf(sub, sizeof sub, "Recorded lord damage %s", Thousands((size_t)s.recorded).c_str());
        Text(dl, T(11), ImVec2(right ? tx - TextW(T(11), sub) : tx, a.y + Px(96)), Hex(0xaaa08d), sub);
    }
    // The gap, the rules.
    const float mx = x0 + colW, mc = mx + midW * 0.5f;
    dl->AddRectFilled(ImVec2(mx, a.y + Px(1)), ImVec2(mx + 1.f, b.y - Px(1)), Hex(0xffffff, 0x0f));
    dl->AddRectFilled(ImVec2(mx + midW, a.y + Px(1)), ImVec2(mx + midW + 1.f, b.y - Px(1)), Hex(0xffffff, 0x0f));
    const char* cap = "LORD DAMAGE";
    TextSpaced(dl, T(10), ImVec2(R(mc - (TextW(T(10), cap) + Px(1.4f) * 10) * 0.5f), a.y + Px(24)), Hex(0xa39883), cap, Px(1.4f));
    if (win)
    {
        const std::string lead = Tag(d, win) + " ahead by ";
        const std::string gap = Thousands((size_t)std::labs(s1 - s2));
        const Type t1 = T(21), t2 = T(21, true);
        const float w = TextW(t1, lead.c_str()) + TextW(t2, gap.c_str());
        Text(dl, t1, ImVec2(R(mc - w * 0.5f), a.y + Px(44)), TagCol[win], lead.c_str());
        Text(dl, t2, ImVec2(R(mc - w * 0.5f + TextW(t1, lead.c_str())), a.y + Px(44)), Strong, gap.c_str());
    }
    const char* how = "How the score is counted";
    const float hw = TextW(T(11), how);
    const ImVec2 hp(R(mc - hw * 0.5f), a.y + Px(84));
    Text(dl, T(11), hp, Hex(0xd7c08d), how);
    if (ImGui::IsMouseHoveringRect(hp, ImVec2(hp.x + hw, hp.y + LineH(11))) && ImGui::IsWindowHovered()) DrawLordRulesTooltip();
    return b.y;
}

// ─── The score race ─────────────────────────────────────────────────────────────────────────────

static float DrawRace(ImDrawList* dl, const BattleLedger::Data& d, float x0, float x1, float y, bool possible)
{
    const float cx0 = x0 + Px(50), cx1 = x1 - Px(190), top = y + Px(30), bot = y + Px(370);
    const int n = s_lm.t1 - s_lm.t0 + 1;
    float vmax = 1.f;
    for (int tm = 1; tm <= 2; ++tm)
        for (int i = 0; i < n; ++i)
            vmax = std::max(vmax, possible ? s_lm.side[tm].possible_s[i] : s_lm.side[tm].score_s[i]);
    const float step = vmax > 30000.f ? 10000.f : vmax > 12000.f ? 5000.f : 2000.f;
    const float vm = std::ceil(vmax * 1.06f / step) * step;
    auto X = [&](float t) { return cx0 + (t - s_lm.t0) / (float)(s_lm.t1 - s_lm.t0) * (cx1 - cx0); };
    auto Y = [&](float v) { return bot - (v + vm * 0.06f) / (vm * 1.06f) * (bot - top); };
    for (float v = 0.f; v <= vm + 1.f; v += step)
    {
        dl->AddLine(ImVec2(cx0, R(Y(v))), ImVec2(cx1, R(Y(v))), v == 0.f ? Hex(0xffffff, 0x38) : Grid);
        const std::string lab = v == 0.f ? std::string("0") : std::to_string((int)(v / 1000.f)) + "k";
        Text(dl, T(10.5f), ImVec2(cx0 - Px(8) - TextW(T(10.5f), lab.c_str()), Y(v) - LineH(10.5f) * 0.5f), Faint, lab.c_str());
    }
    auto line = [&](const std::vector<float>& s, ImU32 c, float th, bool dotted) {
        std::vector<ImVec2> pts;
        for (int i = 0; i < n; ++i) pts.push_back(ImVec2(X((float)(s_lm.t0 + i)), Y(s[i])));
        if (!dotted)
        {
            dl->AddPolyline(pts.data(), (int)pts.size(), Lost, 0, th + Px(3.5f));
            dl->AddPolyline(pts.data(), (int)pts.size(), c, 0, th);
        }
        else
            for (size_t i = 0; i + 1 < pts.size(); i += 4) dl->AddCircleFilled(pts[i], th * 0.55f, c, 6);
    };
    if (possible)
        for (int tm = 1; tm <= 2; ++tm) line(s_lm.side[tm].possible_s, TeamLine[tm], Px(2.2f), true);
    for (int tm = 1; tm <= 2; ++tm) line(s_lm.side[tm].score_s, TeamCol[tm], Px(2.6f), false);

    // At the line ends: each score, its guild; the dotted line's value; the gap between them.
    std::vector<float> want;
    for (int tm = 1; tm <= 2; ++tm) want.push_back(Y(s_lm.side[tm].score_s[n - 1]));
    const std::vector<float> ys = Spread(want, top, bot, Px(34));
    for (int tm = 1; tm <= 2; ++tm)
    {
        const float ly = ys[tm - 1];
        Text(dl, T(14, true), ImVec2(cx1 + Px(12), ly - Px(12)), Strong, Thousands((size_t)std::max(0L, s_lm.side[tm].score)).c_str());
        TagText(dl, T(10.5f), cx1 + Px(12), ly + Px(4), d, tm, " score", TagCol[tm]);
    }
    if (possible)
    {
        std::vector<float> pw;
        for (int tm = 1; tm <= 2; ++tm) pw.push_back(Y(s_lm.side[tm].possible_s[n - 1]));
        const std::vector<float> py = Spread(pw, top, bot, Px(30));
        for (int tm = 1; tm <= 2; ++tm)
        {
            const std::string v = std::string(kApprox) + " " + Thousands((size_t)Round10(s_lm.side[tm].possible_s[n - 1]));
            Text(dl, T(12.5f, true), ImVec2(cx1 + Px(12), py[tm - 1] - Px(12)), Strong, v.c_str());
            TagText(dl, T(10.5f), cx1 + Px(12), py[tm - 1] + Px(3), d, tm, " without protection", TagCol[tm]);
        }
    }
    const long s1 = s_lm.side[1].score, s2 = s_lm.side[2].score;
    if (s1 != s2)
    {
        const int win = s1 > s2 ? 1 : 2;
        const float ya = Y(s_lm.side[win].score_s[n - 1]), yb = Y(s_lm.side[3 - win].score_s[n - 1]);
        const float bx = X((float)s_lm.t1) - Px(6);
        dl->AddLine(ImVec2(bx - Px(4), ya), ImVec2(bx, ya), GoldText, Px(1.2f));
        dl->AddLine(ImVec2(bx, ya), ImVec2(bx, yb), GoldText, Px(1.2f));
        dl->AddLine(ImVec2(bx - Px(4), yb), ImVec2(bx, yb), GoldText, Px(1.2f));
        const std::string gap = Tag(d, win) + " +" + Thousands((size_t)std::labs(s1 - s2));
        const float gw = TextW(T(11, true), gap.c_str());
        const ImVec2 gp(bx - Px(10) - gw, (ya + yb) * 0.5f - LineH(11) * 0.5f);
        dl->AddRectFilled(ImVec2(gp.x - Px(5), gp.y - Px(2)), ImVec2(gp.x + gw + Px(5), gp.y + LineH(11) + Px(2)), Hex(0x1d1c19, 0xeb), Px(3));
        Text(dl, T(11, true), gp, GoldText, gap.c_str());
    }
    if (s_lm.leadChange >= 0)
    {
        const int i = s_lm.leadChange - s_lm.t0;
        const ImVec2 c(X((float)s_lm.leadChange), Y(s_lm.side[s_lm.leadTeam].score_s[i]));
        dl->AddCircle(c, Px(6), GoldText, 16, Px(2));
        const std::string clock = ClockText((float)s_lm.leadChange);
        const std::string rest = " " + Tag(d, s_lm.leadTeam) + " take the lead";
        const float w = TextW(T(11, true), clock.c_str()) + TextW(T(11), rest.c_str());
        Text(dl, T(11, true), ImVec2(c.x - Px(8) - w, c.y - Px(26)), GoldText, clock.c_str());
        Text(dl, T(11), ImVec2(c.x - Px(8) - w + TextW(T(11, true), clock.c_str()), c.y - Px(26)), Hex(0xd7d2c7), rest.c_str());
    }
    // When each guild was hitting the other's lord.
    for (int tm = 1; tm <= 2; ++tm)
    {
        const float ly = bot + Px(22) + (tm - 1) * Px(16);
        Text(dl, T(10.5f, true), ImVec2(cx0 - Px(8) - TextW(T(10.5f, true), Tag(d, tm).c_str()), ly - Px(2)), TagCol[tm], Tag(d, tm).c_str());
        dl->AddRectFilled(ImVec2(cx0, ly), ImVec2(cx1, ly + Px(9)), Hex(0xffffff, 0x08));
        for (const auto& [a, b] : s_lm.pushes[tm])
            dl->AddRectFilled(ImVec2(X((float)a), ly), ImVec2(std::max(X((float)a) + Px(2), X((float)b)), ly + Px(9)), WithAlpha(TeamCol[tm], 0.7f), Px(2));
    }
    Text(dl, T(10.5f), ImVec2(cx1 + Px(10), bot + Px(28)), Faint, "on the enemy lord");
    for (int mnt = (s_lm.t0 + 119) / 120 * 2; mnt * 60 <= s_lm.t1; mnt += 2)
    {
        const std::string lab = std::to_string(mnt) + ":00";
        Text(dl, T(10.5f), ImVec2(X(mnt * 60.f) - TextW(T(10.5f), lab.c_str()) * 0.5f, bot + Px(62)), mnt * 60 >= s_lm.t1 - 30 ? GoldText : Faint, lab.c_str());
    }
    return bot + Px(84);
}

// ─── The tug of war ─────────────────────────────────────────────────────────────────────────────

static float DrawTug(ImDrawList* dl, const BattleLedger::Data& d, BattleLedger::Host& host, float x0, float x1, float y)
{
    const float cx0 = x0 + Px(60), cx1 = x1 - Px(330), mid = y + Px(230), half = Px(195);
    const int n = s_lm.t1 - s_lm.t0 + 1;
    float vmax = 1.f;
    for (int tm = 1; tm <= 2; ++tm) vmax = std::max(vmax, s_lm.side[tm].possible_s[n - 1]);
    const float vm = std::ceil(vmax * 1.05f / 10000.f) * 10000.f, k = half / vm;
    auto X = [&](int i) { return cx0 + (float)i / (float)(n - 1) * (cx1 - cx0); };
    for (float v = 10000.f; v <= vm; v += 10000.f)
        for (int sgn = -1; sgn <= 1; sgn += 2)
        {
            dl->AddLine(ImVec2(cx0, R(mid - sgn * v * k)), ImVec2(cx1, R(mid - sgn * v * k)), Grid);
            const std::string lab = std::to_string((int)(v / 1000.f)) + "k";
            Text(dl, T(10), ImVec2(cx0 - Px(8) - TextW(T(10), lab.c_str()), mid - sgn * v * k - LineH(10) * 0.5f), Faint, lab.c_str());
        }
    struct Lab { ImTextureID icon; std::string value, text; ImU32 col; float want; };
    for (int tm = 1; tm <= 2; ++tm)
    {
        const Side& s = s_lm.side[tm];
        const float sgn = tm == 1 ? 1.f : -1.f;
        auto yAt = [&](float v) { return mid - sgn * v * k; };
        // Layers out from the line: damage by kind, then what was prevented by mechanism. Each layer
        // is joined quads from one second to the next (no overlap, no anti-aliased seams).
        std::vector<float> lo(n, 0.f), hi(n, 0.f);
        const ImDrawListFlags flags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        auto layer = [&](ImU32 col) {
            for (int i = 0; i + 1 < n; ++i)
            {
                if (hi[i] <= lo[i] && hi[i + 1] <= lo[i + 1]) continue;
                dl->AddQuadFilled(ImVec2(X(i), yAt(lo[i])), ImVec2(X(i + 1), yAt(lo[i + 1])), ImVec2(X(i + 1), yAt(hi[i + 1])), ImVec2(X(i), yAt(hi[i])), col);
            }
        };
        std::vector<float> acc(n, 0.f);
        for (int kd = 0; kd < kKinds; ++kd)
        {
            for (int i = 0; i < n; ++i) { lo[i] = acc[i]; acc[i] += s.kind_s[kd][i]; hi[i] = acc[i]; }
            layer(WithAlpha(TeamCol[tm], kShade[kd]));
        }
        const std::vector<float> dmgTop = acc;
        for (int mc = 0; mc < 4; ++mc)
        {
            for (int i = 0; i < n; ++i) { lo[i] = acc[i]; acc[i] += s.mech_s[mc][i]; hi[i] = acc[i]; }
            layer(WithAlpha(MechCol[mc], 0.55f));
        }
        for (int i = 0; i < n; ++i) { lo[i] = std::max(0.f, s.score_s[i]); hi[i] = std::max(lo[i], dmgTop[i]); }
        layer(WithAlpha(Lost, 0.55f));
        dl->Flags = flags;
        std::vector<ImVec2> pts;
        for (int i = 0; i < n; ++i) pts.push_back(ImVec2(X(i), yAt(std::max(0.f, s.score_s[i]))));
        dl->AddPolyline(pts.data(), (int)pts.size(), Lost, 0, Px(5));
        dl->AddPolyline(pts.data(), (int)pts.size(), TeamLine[tm], 0, Px(2.2f));

        // Every layer named at the right.
        std::vector<Lab> labs;
        float base = 0.f;
        for (int kd = 0; kd < kKinds; ++kd)
        {
            const float v = s.kind_s[kd][n - 1];
            if (v >= 150.f && kd != kSteal) labs.push_back({ kd == kWeapon ? IconWeapon(host) : nullptr, Thousands((size_t)v), KindName[kd], Strong, base + v * 0.5f });
            base += v;
        }
        for (int mc = 0; mc < 4; ++mc)
        {
            const float v = s.mech_s[mc][n - 1];
            if (v >= 40.f)
            {
                int top = 0; double best = -1;
                for (const auto& [sk, p] : s.mechSkills[mc]) if (p.amount > best) { best = p.amount; top = sk; }
                std::string text = Lower(MechName[mc]) + kDot + host.SkillName(top);
                if (s.mechSkills[mc].size() > 1) text += " +" + std::to_string(s.mechSkills[mc].size() - 1);
                labs.push_back({ host.SkillIcon(top), std::string(kApprox) + " +" + Thousands((size_t)Round10(v)), text, MechCol[mc], base + v * 0.5f });
            }
            base += v;
        }
        labs.push_back({ IconEnchant(), Num(-(long)(s.recorded - s.score)), "not in the score", Hex(0xb3ada2), (s.score + (float)s.recorded) * 0.5f });
        labs.push_back({ LordIcon(host, 3 - tm), Thousands((size_t)std::max(0L, s.score)), "score", Cream, (float)s.score });
        std::vector<float> want;
        for (const Lab& l : labs) want.push_back(yAt(l.want));
        const std::vector<float> ys = tm == 1 ? Spread(want, y + Px(20), mid - Px(10), Px(19)) : Spread(want, mid + Px(10), mid + half + Px(25), Px(19));
        const float lx = cx1 + Px(34);
        for (size_t i = 0; i < labs.size(); ++i)
        {
            dl->AddLine(ImVec2(cx1 + Px(2), want[i]), ImVec2(cx1 + Px(12), want[i]), Hex(0xffffff, 0x2e));
            dl->AddLine(ImVec2(cx1 + Px(12), want[i]), ImVec2(lx - Px(5), ys[i]), Hex(0xffffff, 0x2e));
            if (labs[i].icon) dl->AddImage(labs[i].icon, ImVec2(lx, ys[i] - Px(8)), ImVec2(lx + Px(15), ys[i] + Px(7)));
            const Type tv = T(10.5f, true), tt = T(10.5f);
            Text(dl, tv, ImVec2(lx + Px(21), ys[i] - LineH(10.5f) * 0.5f), labs[i].col, labs[i].value.c_str());
            Text(dl, tt, ImVec2(lx + Px(21) + TextW(tv, labs[i].value.c_str()) + Px(6), ys[i] - LineH(10.5f) * 0.5f), Hex(0xa8a092), labs[i].text.c_str());
        }
    }
    dl->AddLine(ImVec2(cx0, mid), ImVec2(cx1, mid), Hex(0xffffff, 0x80));
    Text(dl, T(13, true), ImVec2(cx0 - Px(10) - TextW(T(13, true), Tag(d, 1).c_str()), mid - half - Px(26)), TagCol[1], Tag(d, 1).c_str());
    Text(dl, T(13, true), ImVec2(cx0 - Px(10) - TextW(T(13, true), Tag(d, 2).c_str()), mid + half + Px(10)), TagCol[2], Tag(d, 2).c_str());
    for (int mnt = (s_lm.t0 + 119) / 120 * 2; mnt * 60 <= s_lm.t1; mnt += 2)
    {
        const std::string lab = std::to_string(mnt) + ":00";
        Text(dl, T(10.5f), ImVec2(X(mnt * 60 - s_lm.t0) - TextW(T(10.5f), lab.c_str()) * 0.5f, mid + half + Px(36)), Faint, lab.c_str());
    }
    return mid + half + Px(58);
}

// ─── A guild's panel: bar, damage, ladder, prevented ────────────────────────────────────────────

static float DrawSidePanel(ImDrawList* dl, const MatchMeta& m, const BattleLedger::Data& d, BattleLedger::Host& host, int tm,
                           float x, float w, float y)
{
    const Side& s = s_lm.side[tm];
    const int o = 3 - tm;
    const SkillDatabaseView& view = GetMatchSkillView(m);
    std::string ft1, ft2;
    ParseFolderTags(m.folder_name, ft1, ft2);
    const GuildLabel g = GetPartyGuild(m, tm == 1 ? "1" : "2", tm == 1 ? ft1 : ft2);

    // Heading.
    if (ImTextureID cape = GetGuildCape(m, g)) ImageContain(dl, cape, 96.f, 192.f, ImVec2(x, y - Px(4)), ImVec2(x + Px(15), y + Px(26)));
    TagText(dl, T(14, true), x + Px(22), y - Px(2), d, tm, std::string(kApos) + "s score", Head);
    Icon(dl, LordIcon(host, o), ImVec2(x + Px(22), y + Px(19)), Px(13));
    Text(dl, T(10.5f), ImVec2(x + Px(39), y + Px(18)), Faint, (std::string("on ") + Tag(d, o) + kApos + "s lord").c_str());

    // The split bar: counted, taken off, prevented by mechanism; the brackets above it.
    double poss = s.recorded + s.prevented;
    double wm = 1.0;
    for (int t = 1; t <= 2; ++t) wm = std::max(wm, (double)s_lm.side[t].recorded + s_lm.side[t].prevented);
    wm *= 1.02;
    const float bx0 = x, bx1 = x + w;
    auto XW = [&](double v) { return bx0 + (float)(v / wm) * (bx1 - bx0); };
    const float by = y + Px(68), bh = Px(16);
    const long score = std::max(0L, s.score);
    // ≈ estimated without protection (dashed), counted in the score (solid)
    DashedH(dl, XW(0), XW(poss), by - Px(28), Hex(0xcfc4ab), Px(4), Px(3), Px(1.2f));
    dl->AddLine(ImVec2(XW(0), by - Px(28)), ImVec2(XW(0), by - Px(22)), Hex(0xcfc4ab), Px(1.2f));
    dl->AddLine(ImVec2(XW(poss), by - Px(28)), ImVec2(XW(poss), by - Px(22)), Hex(0xcfc4ab), Px(1.2f));
    {
        const std::string v = std::string(kApprox) + " " + Thousands((size_t)Round10(poss)), t2 = " estimated without protection";
        const float tw = TextW(T(11, true), v.c_str()) + TextW(T(11), t2.c_str());
        Text(dl, T(11, true), ImVec2(XW(poss) - tw, by - Px(46)), Hex(0xddd3bd), v.c_str());
        Text(dl, T(11), ImVec2(XW(poss) - tw + TextW(T(11, true), v.c_str()), by - Px(46)), Body, t2.c_str());
    }
    dl->AddLine(ImVec2(XW(0), by - Px(12)), ImVec2(XW(score), by - Px(12)), TeamLine[tm], Px(1.4f));
    dl->AddLine(ImVec2(XW(0), by - Px(12)), ImVec2(XW(0), by - Px(6)), TeamLine[tm], Px(1.4f));
    dl->AddLine(ImVec2(XW(score), by - Px(12)), ImVec2(XW(score), by - Px(6)), TeamLine[tm], Px(1.4f));
    {
        const std::string v = Thousands((size_t)score), t2 = " counted in the score";
        const float tw = TextW(T(11, true), v.c_str()) + TextW(T(11), t2.c_str());
        Text(dl, T(11, true), ImVec2(XW(score) - Px(4) - tw, by - Px(28)), Cream, v.c_str());
        Text(dl, T(11), ImVec2(XW(score) - Px(4) - tw + TextW(T(11, true), v.c_str()), by - Px(28)), Body, t2.c_str());
    }
    double acc = 0;
    for (int kd = 0; kd < kKinds; ++kd)
    {
        if (s.kind[kd] > 0) dl->AddRectFilled(ImVec2(XW(acc), by), ImVec2(XW(acc + s.kind[kd]), by + bh), WithAlpha(TeamCol[tm], kShade[kd]));
        acc += s.kind[kd];
    }
    for (int mc = 0; mc < 4; ++mc)
    {
        Pattern(dl, ImVec2(XW(acc), by), ImVec2(XW(acc + s.mech[mc]), by + bh), mc);
        acc += s.mech[mc];
    }
    Pattern(dl, ImVec2(XW(score), by), ImVec2(XW(s.recorded), by + bh), -1);
    dl->AddLine(ImVec2(XW(score), by - Px(2)), ImVec2(XW(score), by + bh + Px(2)), TeamLine[tm], Px(2));
    // under the hatched stretch: what it is, in the ladder's words
    dl->AddLine(ImVec2(XW(score), by + bh + Px(5)), ImVec2(XW(score), by + bh + Px(9)), Faint);
    dl->AddLine(ImVec2(XW(score), by + bh + Px(9)), ImVec2(XW(s.recorded), by + bh + Px(9)), Faint);
    dl->AddLine(ImVec2(XW(s.recorded), by + bh + Px(5)), ImVec2(XW(s.recorded), by + bh + Px(9)), Faint);
    {
        const std::string v = std::string(kMinus) + " " + Thousands((size_t)std::max(0L, s.recorded - score));
        Text(dl, T(10.5f, true), ImVec2(XW(score) + Px(2), by + bh + Px(12)), Body, v.c_str());
        Text(dl, T(10.5f), ImVec2(XW(score) + Px(2) + TextW(T(10.5f, true), v.c_str()), by + bh + Px(12)), Faint, " taken off the score");
    }

    // Two columns: the damage and the ladder | what the defenders prevented.
    const float DX = x, DV = x + w * 0.46f, PX = x + w * 0.5f, PV = x + w;
    const float ry = by + Px(56);
    {
        const std::string pre = "Damage on ";
        Text(dl, T(10.5f), ImVec2(DX, ry), Faint, pre.c_str());
        TagText(dl, T(10.5f), DX + TextW(T(10.5f), pre.c_str()), ry, d, o, std::string(kApos) + "s lord", Faint);
    }
    {
        const std::string pre = "Prevented by ";
        Text(dl, T(10.5f), ImVec2(PX, ry), Faint, pre.c_str());
        TagText(dl, T(10.5f), PX + TextW(T(10.5f), pre.c_str()), ry, d, o, "", Faint);
        Text(dl, T(10.5f), ImVec2(PV - TextW(T(10.5f), "damage prevented"), ry), Faint, "damage prevented");
    }
    dl->AddLine(ImVec2(DX, ry + LineH(10.5f) + Px(3)), ImVec2(DV, ry + LineH(10.5f) + Px(3)), Hex(0xffffff, 0x14));
    dl->AddLine(ImVec2(PX, ry + LineH(10.5f) + Px(3)), ImVec2(PV, ry + LineH(10.5f) + Px(3)), Hex(0xffffff, 0x14));

    // Damage by kind: every skill as an icon; what of it went through armour penetration.
    float yl = ry + LineH(10.5f) + Px(14);
    for (int kd = 0; kd < kKinds; ++kd)
    {
        if (s.kind[kd] <= 0) continue;
        if (kd == kSteal) Icon(dl, IconSteal(), ImVec2(DX - Px(1), yl), Px(14));
        else dl->AddRectFilled(ImVec2(DX, yl + Px(1)), ImVec2(DX + Px(12), yl + Px(13)), WithAlpha(TeamCol[tm], kShade[kd]), Px(2));
        Text(dl, T(12, true), ImVec2(DX + Px(22), yl), Head, KindName[kd]);
        const std::string v = Thousands((size_t)s.kind[kd]);
        Text(dl, T(12, true), ImVec2(DV - TextW(T(12, true), v.c_str()), yl), Strong, v.c_str());
        yl += LineH(12) + Px(4);
        std::vector<std::pair<long, int>> sk;
        for (const auto& [id, a] : s.kindSkills[kd]) sk.push_back({ a, id });
        std::sort(sk.rbegin(), sk.rend());
        if (!sk.empty() && kd != kSteal)
        {
            std::vector<std::pair<int, std::string>> items;
            for (const auto& [a, id] : sk) items.push_back({ id, Thousands((size_t)a) });
            yl = IconGrid(dl, host, DX + Px(22), yl, DV + Px(4), Px(63), items) + Px(6);
        }
        if (s.penKind[kd] > 0)
        {
            const ImVec2 p0(DX + Px(22), yl + Px(2));
            Pattern(dl, p0, ImVec2(p0.x + Px(11), p0.y + Px(10)), 1);
            dl->AddLine(ImVec2(p0.x, p0.y + Px(10)), ImVec2(p0.x + Px(11), p0.y), Hex(0xe8e2d6), Px(1.3f));
            const std::string a = "of which ", pv = "+" + Thousands((size_t)s.penKind[kd]), b = " through armour penetration";
            float tx = DX + Px(39);
            Text(dl, T(10.5f), ImVec2(tx, yl), Body, a.c_str()); tx += TextW(T(10.5f), a.c_str());
            Text(dl, T(10.5f, true), ImVec2(tx, yl), Head, pv.c_str()); tx += TextW(T(10.5f, true), pv.c_str());
            Text(dl, T(10.5f), ImVec2(tx, yl), Body, b.c_str());
            yl += LineH(10.5f) + Px(2);
            std::vector<std::pair<long, int>> pens;
            for (const auto& [ag, a2] : s.penBy[kd]) if (a2 > 0) pens.push_back({ a2, ag });
            std::sort(pens.rbegin(), pens.rend());
            std::vector<std::pair<int, std::string>> people;
            for (const auto& [a2, ag] : pens) people.push_back({ ag, AgentName(d, ag) + " +" + Thousands((size_t)a2) });
            yl = People(dl, host, d, DX + Px(39), yl, DV, people, T(10), Faint) + Px(2);
        }
        yl += Px(14);
    }

    // The ladder: damage, then what is taken off the score below the line, then the score.
    auto ladderRow = [&](ImTextureID icon, float yy, auto&& label, const std::string& value, ImU32 vc, bool bold) {
        if (icon) Icon(dl, icon, ImVec2(DX - Px(1), yy), Px(14));
        label(yy);
        Text(dl, T(12, bold), ImVec2(DV - TextW(T(12, bold), value.c_str()), yy), vc, value.c_str());
    };
    yl += Px(4);
    ladderRow(IconDamage(host), yl, [&](float yy) {
        float tx = TagText(dl, T(12, true), DX + Px(22), yy, d, tm, std::string(kApos) + "s damage on ", Head);
        return TagText(dl, T(12, true), tx, yy, d, o, std::string(kApos) + "s lord", Head);
    }, Thousands((size_t)s.recorded), Strong, true);
    yl += LineH(12) + Px(4);
    dl->AddLine(ImVec2(DX, yl), ImVec2(DV, yl), WithAlpha(Body, 0.55f), Px(1.2f));
    yl += Px(6);
    dl->AddRectFilled(ImVec2(DX, yl + Px(1)), ImVec2(DX + Px(13), yl + Px(13)), WithAlpha(TeamCol[tm], 0.66f), Px(1.5f));
    Pattern(dl, ImVec2(DX, yl + Px(1)), ImVec2(DX + Px(13), yl + Px(13)), -1);
    Text(dl, T(10.5f), ImVec2(DX + Px(22), yl), Body, "taken off the score");
    yl += LineH(10.5f) + Px(10);
    ladderRow(IconEnchant(), yl, [&](float yy) {
        float tx = TagText(dl, T(12), DX + Px(22), yy, d, tm, std::string(kApos) + "s casts on ", Body);
        return TagText(dl, T(12), tx, yy, d, tm, std::string(kApos) + "s own lord, " + kMinus + "50 each", Body);
    }, std::string(kMinus) + " " + Thousands((size_t)std::labs(s.castPoints)), Body, false);
    yl += LineH(12) + Px(2);
    {
        std::string what = std::to_string(s.enchants) + " enchantments";
        if (s.weaponSpells) what += " + " + std::to_string(s.weaponSpells) + " weapon spells";
        Text(dl, T(10), ImVec2(DX + Px(22), yl), Faint, what.c_str());
        yl += LineH(10) + Px(4);
        std::vector<std::pair<long, int>> order;
        for (const auto& [sk, c] : s.casts) order.push_back({ c.points, sk });
        std::sort(order.begin(), order.end());
        std::vector<std::pair<int, std::string>> items;
        for (const auto& [pts, sk] : order) items.push_back({ sk, std::string(kTimes) + std::to_string(s.casts.at(sk).count) + " " + Num(pts) });
        yl = IconGrid(dl, host, DX + Px(22), yl, DV + Px(4), Px(94), items) + Px(14);
    }
    if (s.steal)
    {
        ladderRow(IconSteal(), yl, [&](float yy) {
            float tx = TagText(dl, T(12), DX + Px(22), yy, d, tm, std::string(kApos) + "s life steal on ", Body);
            return TagText(dl, T(12), tx, yy, d, o, std::string(kApos) + "s lord (doesn\xE2\x80\x99t count)", Body);
        }, std::string(kMinus) + " " + Thousands((size_t)s.steal), Body, false);
        yl += LineH(12) + Px(8);
    }
    if (s.entHealth)
    {
        ladderRow(IconEntourage(), yl, [&](float yy) {
            return TagText(dl, T(12), DX + Px(22), yy, d, tm, std::string(kApos) + "s hits under Entourage (count 20%)", Body);
        }, std::string(kMinus) + " " + Thousands((size_t)(s.entHealth - s.entPoints)), Body, false);
        yl += LineH(12) + Px(8);
    }
    if (s.rest)
    {
        ladderRow(nullptr, yl, [&](float yy) { Text(dl, T(12), ImVec2(DX + Px(22), yy), Faint, "not yet matched (being checked)"); return 0.f; },
                  (s.rest > 0 ? std::string("+ ") : std::string(kMinus) + " ") + Thousands((size_t)std::labs(s.rest)), Faint, false);
        yl += LineH(12) + Px(8);
    }
    dl->AddLine(ImVec2(DV - Px(90), yl - Px(2)), ImVec2(DV, yl - Px(2)), Hex(0xffffff, 0x59));
    yl += Px(4);
    TagText(dl, T(13, true), DX + Px(22), yl, d, tm, std::string(kApos) + "s score", Cream);
    {
        const std::string v = "= " + Thousands((size_t)score);
        Text(dl, T(13, true), ImVec2(DV - TextW(T(13, true), v.c_str()), yl), Cream, v.c_str());
    }
    yl += LineH(13) + Px(10);

    // Prevented by the defenders: one block per mechanism, a line per skill, its casters below.
    float yr = ry + LineH(10.5f) + Px(14);
    const Side& def = s_lm.side[o];   // the defenders' own-lord casts name their casters
    std::vector<std::pair<double, int>> mechs;
    for (int mc = 0; mc < 4; ++mc) if (s.mech[mc] > 0) mechs.push_back({ s.mech[mc], mc });
    std::sort(mechs.rbegin(), mechs.rend());
    for (const auto& [amt, mc] : mechs)
    {
        Pattern(dl, ImVec2(PX, yr + Px(1)), ImVec2(PX + Px(12), yr + Px(13)), mc);
        Outline(dl, ImVec2(PX, yr + Px(1)), ImVec2(PX + Px(12), yr + Px(13)), WithAlpha(MechCol[mc], 0.6f), 0.f);
        Text(dl, T(12, true), ImVec2(PX + Px(22), yr), Head, MechName[mc]);
        const std::string v = std::string(kApprox) + " +" + Thousands((size_t)Round10(amt));
        Text(dl, T(12, true), ImVec2(PV - TextW(T(12, true), v.c_str()), yr), MechCol[mc], v.c_str());
        yr += LineH(12) + Px(4);
        std::vector<std::pair<double, int>> sk;
        for (const auto& [id, p] : s.mechSkills[mc]) sk.push_back({ p.amount, id });
        std::sort(sk.rbegin(), sk.rend());
        for (const auto& [a, id] : sk)
        {
            const auto cIt = def.casts.find(id);
            const SkillCast* cast = cIt == def.casts.end() ? nullptr : &cIt->second;
            std::string chip = RankChip(m, d, view, id, cast);
            if (mc == 0 && s.mechSkills[mc].at(id).blocks > 0) chip = std::to_string(s.mechSkills[mc].at(id).blocks) + " blocks" + (chip.empty() ? std::string() : kDot + chip);
            Icon(dl, host.SkillIcon(id), ImVec2(PX + Px(22), yr), Px(14), host.SkillName(id).c_str());
            const std::string nm = host.SkillName(id);
            Text(dl, T(10.5f), ImVec2(PX + Px(42), yr + Px(1)), Hex(0xd8d0bf), nm.c_str());
            if (!chip.empty()) Text(dl, T(10.5f), ImVec2(PX + Px(42) + TextW(T(10.5f), nm.c_str()) + Px(8), yr + Px(1)), Faint, chip.c_str());
            if (sk.size() > 1)
            {
                const std::string sv = std::string(kApprox) + " +" + Thousands((size_t)std::lround(a));
                Text(dl, T(10.5f), ImVec2(PV - TextW(T(10.5f), sv.c_str()), yr + Px(1)), Hex(0xa8a092), sv.c_str());
            }
            yr += LineH(10.5f) + Px(3);
            if (cast)
            {
                std::vector<std::pair<int, int>> casters;
                for (const auto& [ag, cnt] : cast->by) casters.push_back({ cnt, ag });
                std::sort(casters.rbegin(), casters.rend());
                std::vector<std::pair<int, std::string>> people;
                for (const auto& [cnt, ag] : casters) people.push_back({ ag, AgentName(d, ag) + " " + kTimes + std::to_string(cnt) });
                yr = People(dl, host, d, PX + Px(42), yr, PV, people, T(10), Faint) + Px(4);
            }
        }
        yr += Px(14);
    }
    return std::max(yl, yr);
}

// ─── Skills on the lords: damage and prevention on one scale ────────────────────────────────────

static float DrawLordCard(ImDrawList* dl, const MatchMeta& m, const BattleLedger::Data& d, BattleLedger::Host& host, int tl,
                          float x, float w, float y, float scaleMax)
{
    const Side& att = s_lm.side[3 - tl];     // the guild hitting this lord
    const Side& own = s_lm.side[tl];         // this lord's guild: its casts on its own lord
    const SkillDatabaseView& view = GetMatchSkillView(m);
    const float pad = Px(18), x0 = x + pad, x1 = x + w - pad;
    // columns: icon, name, detail, bar, value, players
    const float cIcon = x0, cName = x0 + Px(30), cDet = cName + Px(160), cBar = cDet + Px(130);
    const float cPeople = x1 - Px(190), cVal = cPeople - Px(10), cBarEnd = cVal - Px(72);
    const float costMax = 2500.f, zero = cBar + (cBarEnd - cBar) * costMax / (costMax + scaleMax);
    auto BW = [&](double v) { return (float)(v / (costMax + scaleMax)) * (cBarEnd - cBar); };
    const float rowH = Px(27);

    float yy = y + Px(16);
    Icon(dl, LordIcon(host, tl), ImVec2(x0, yy), Px(34));
    TagText(dl, T(15, true), x0 + Px(44), yy + Px(8), d, tl, std::string(kApos) + "s lord", Head);
    yy += Px(48);

    // The damage on it.
    Icon(dl, IconDamage(host), ImVec2(x0, yy), Px(15));
    float tx = TagText(dl, T(12, true), x0 + Px(22), yy, d, 3 - tl, std::string(kApos) + "s damage", Head);
    Text(dl, T(10.5f), ImVec2(tx + Px(8), yy + Px(1)), Faint, "\xC2\xB7 hits per player shown as \xC3\x97N");
    const std::string tot = Thousands((size_t)att.recorded);
    Text(dl, T(12), ImVec2(x1 - TextW(T(12), tot.c_str()), yy), Strong, tot.c_str());
    yy += Px(24);
    std::vector<std::pair<long, int>> rows;
    for (const auto& [key, r] : att.lordRows) rows.push_back({ r.amount, key });
    std::sort(rows.rbegin(), rows.rend());
    long more = 0; int moreN = 0;
    auto zeroLine = [&](float ya, float yb) { dl->AddLine(ImVec2(zero, ya), ImVec2(zero, yb), Hex(0xffffff, 0x38)); };
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const int key = rows[i].second;
        const Row& r = att.lordRows.at(key);
        if (i >= 7) { more += r.amount; moreN++; continue; }
        const float cy = yy + rowH * 0.5f;
        ImTextureID ic = key == -1 ? IconWeapon(host) : key > 0 ? host.SkillIcon(key) : nullptr;
        if (ic) dl->AddImage(ic, ImVec2(cIcon, cy - Px(10)), ImVec2(cIcon + Px(20), cy + Px(10)));
        const std::string nm = key == -1 ? "Weapon hits" : key > 0 ? host.SkillName(key) : "No skill named";
        TextFit(dl, T(12), ImVec2(cName, cy - LineH(12) * 0.5f), cDet - cName - Px(8), Hex(0xe4ded2), nm.c_str());
        const std::string hits = std::to_string(r.hits) + " hits";
        Text(dl, T(11), ImVec2(cDet, cy - LineH(11) * 0.5f), Faint, hits.c_str());
        zeroLine(yy, yy + rowH);
        dl->AddRectFilled(ImVec2(zero, cy - Px(4)), ImVec2(zero + BW(r.amount), cy + Px(4)), WithAlpha(Dmg, 0.8f), Px(3), ImDrawFlags_RoundCornersRight);
        const std::string v = Thousands((size_t)r.amount);
        Text(dl, T(11.5f), ImVec2(zero + BW(r.amount) + Px(5), cy - LineH(11.5f) * 0.5f), Strong, v.c_str());
        std::vector<std::pair<int, std::string>> users;
        for (const auto& [name, c] : r.users) users.push_back({ c, name });
        std::sort(users.rbegin(), users.rend());
        std::vector<std::pair<int, std::string>> ppl;
        for (size_t u = 0; u < users.size() && u < 2; ++u)
        {
            int agent = 0;
            for (const auto& [id, ag] : d.agents) if (ag.name == users[u].second) { agent = id; break; }
            ppl.push_back({ agent, users[u].second + " " + kTimes + std::to_string(users[u].first) });
        }
        if (users.size() > 2) ppl.push_back({ 0, "+" + std::to_string(users.size() - 2) + " more" });
        dl->PushClipRect(ImVec2(cPeople, yy), ImVec2(x1, yy + rowH), true);
        People(dl, host, d, cPeople, cy - LineH(11) * 0.5f, 1e9f, ppl, T(11), Faint);
        dl->PopClipRect();
        yy += rowH;
    }
    if (moreN)
    {
        const float cy = yy + Px(12);
        const std::string a = std::to_string(moreN) + " more skills";
        Text(dl, T(11.5f), ImVec2(cName, cy - LineH(11.5f) * 0.5f), Hex(0xa8a092), a.c_str());
        Text(dl, T(11.5f), ImVec2(zero, cy - LineH(11.5f) * 0.5f), Hex(0xa8a092), Thousands((size_t)more).c_str());
        yy += Px(24);
    }

    // The guild's skills on its own lord: cost, damage prevented, score benefit, casters.
    yy += Px(14);
    struct DefRow { int skill; int casts; long cost; double prevented, scored; bool est; std::map<int, int> by; };
    std::map<int, DefRow> def;
    for (const auto& [sk, c] : own.casts) { DefRow& r = def[sk]; r.skill = sk; r.casts = c.count; r.cost = c.points; r.by = c.by; }
    for (int mc = 0; mc < 4; ++mc)
        for (const auto& [sk, p] : att.mechSkills[mc])
        {
            DefRow& r = def[sk]; r.skill = sk; r.prevented += p.amount; r.scored += p.scored;
            if (mc == 0) r.est = true;
        }
    double tCost = 0, tPrev = 0, tBen = 0;
    for (const auto& [sk, r] : def) { tCost += r.cost; tPrev += r.prevented; tBen += r.scored + r.cost; }
    const double entBen = (double)(att.entHealth - att.entPoints);
    tPrev += entBen; tBen += entBen;
    Icon(dl, IconEnchant(), ImVec2(x0, yy), Px(14));
    TagText(dl, T(12, true), x0 + Px(22), yy, d, tl, std::string(kApos) + "s skills on the lord", Head);
    {
        const std::string a = "cost " + Num((long)tCost) + kDot + "prevented " + kApprox + " " + Thousands((size_t)Round10(tPrev)) + kDot;
        const std::string b = std::string("score benefit ") + kApprox + " " + Signed(Round10(tBen));
        const float wa = TextW(T(12), a.c_str()), wb = TextW(T(12, true), b.c_str());
        Text(dl, T(12), ImVec2(x1 - wa - wb, yy), Strong, a.c_str());
        Text(dl, T(12, true), ImVec2(x1 - wb, yy), Strong, b.c_str());
    }
    yy += Px(24);
    Text(dl, T(10.5f), ImVec2(cDet, yy), Faint, "At the caster\xE2\x80\x99s rank");
    Text(dl, T(10.5f), ImVec2(zero - Px(4) - TextW(T(10.5f), "cost"), yy), Faint, "cost");
    Text(dl, T(10.5f), ImVec2(zero + Px(6), yy), Faint, "damage prevented");
    Text(dl, T(10.5f), ImVec2(cVal - TextW(T(10.5f), "Score benefit"), yy), Faint, "Score benefit");
    Text(dl, T(10.5f), ImVec2(cPeople, yy), Faint, "Casts per player");
    yy += Px(18);
    std::vector<const DefRow*> order;
    for (const auto& [sk, r] : def) order.push_back(&r);
    std::sort(order.begin(), order.end(), [](const DefRow* a, const DefRow* b) {
        return a->prevented + a->cost != b->prevented + b->cost ? a->prevented + a->cost > b->prevented + b->cost : a->casts > b->casts; });
    auto defRow = [&](ImTextureID ic, const std::string& nm, const std::string& chip, double cost, double prev, bool est, double ben,
                      const std::vector<std::pair<int, std::string>>& ppl, const char* tip) {
        const float cy = yy + rowH * 0.5f;
        Icon(dl, ic, ImVec2(cIcon, cy - Px(10)), Px(20), tip);
        TextFit(dl, T(12), ImVec2(cName, cy - LineH(12) * 0.5f), cDet - cName - Px(8), Hex(0xe4ded2), nm.c_str());
        TextFit(dl, T(11), ImVec2(cDet, cy - LineH(11) * 0.5f), cBar - cDet - Px(8), Body, chip.c_str());
        zeroLine(yy, yy + rowH);
        if (cost < 0)
        {
            const ImVec2 a0(zero - BW(-cost), cy - Px(5)), a1(zero, cy + Px(5));
            dl->PushClipRect(a0, a1, true);
            for (float sx = a0.x - Px(10); sx < a1.x; sx += Px(5)) dl->AddLine(ImVec2(sx, a1.y), ImVec2(sx + Px(10), a0.y), Cost, Px(2));
            dl->PopClipRect();
        }
        if (prev > 0)
        {
            dl->AddRectFilled(ImVec2(zero, cy - Px(4)), ImVec2(zero + BW(prev), cy + Px(4)), WithAlpha(Hex(0xcfc4ab), 0.55f), Px(3), ImDrawFlags_RoundCornersRight);
            const std::string pv = std::string(kApprox) + " " + Thousands((size_t)(est ? Round10(prev) : std::lround(prev)));
            Text(dl, T(10.5f), ImVec2(zero + BW(prev) + Px(5), cy - LineH(10.5f) * 0.5f), Body, pv.c_str());
        }
        const long b = prev > 0 ? Round10(ben) : (long)std::lround(ben);
        const std::string bs = b == 0 ? std::string("0") : (prev > 0 ? std::string(kApprox) + " " : std::string()) + Signed(b);
        Text(dl, T(12, std::labs(b) >= 2000), ImVec2(cVal - TextW(T(12, std::labs(b) >= 2000), bs.c_str()), cy - LineH(12) * 0.5f), b == 0 ? Hex(0x6f6a60) : Strong, bs.c_str());
        dl->PushClipRect(ImVec2(cPeople, yy), ImVec2(x1, yy + rowH), true);
        People(dl, host, d, cPeople, cy - LineH(11) * 0.5f, 1e9f, ppl, T(11), Faint);
        dl->PopClipRect();
        yy += rowH;
    };
    for (const DefRow* r : order)
    {
        const auto cIt = own.casts.find(r->skill);
        const std::string chip = RankChip(m, d, view, r->skill, cIt == own.casts.end() ? nullptr : &cIt->second);
        std::vector<std::pair<int, int>> by;
        for (const auto& [ag, n] : r->by) by.push_back({ n, ag });
        std::sort(by.rbegin(), by.rend());
        std::vector<std::pair<int, std::string>> ppl;
        for (size_t u = 0; u < by.size() && u < 2; ++u) ppl.push_back({ by[u].second, AgentName(d, by[u].second) + " " + kTimes + std::to_string(by[u].first) });
        if (by.size() > 2) ppl.push_back({ 0, "+" + std::to_string(by.size() - 2) + " more" });
        const std::string nm = host.SkillName(r->skill);
        defRow(host.SkillIcon(r->skill), nm, chip, (double)r->cost, r->prevented, r->est, r->scored + r->cost, ppl, nm.c_str());
    }
    defRow(IconEntourage(), "Entourage", std::string(kTimes) + "0.2 for 10.5 s", 0.0, entBen, false, entBen, { { 0, "the Guild Lord" } }, "Entourage: the Guild Lord's own skill, no cost");
    return yy + Px(14);
}

} // namespace lordv

// The page's state for this view (the Statistics page keeps the rest).
struct LordViewState { bool tug = false; bool possible = false; float scrollReq = -1.f; };
static LordViewState s_lv;

static float DrawLordDamageView(ImDrawList* dl, StatsFrame& f, float x0, float x1, float y)
{
    using namespace lordv;
    const MatchMeta& m = *f.m;
    const BattleLedger::Data& d = *f.d;
    BattleLedger::Host& host = *f.host;
    Build(m, d, host);
    y += Px(18);
    if (!s_lm.valid)
    {
        DrawStatsCentered(dl, x0, x1, y + Px(80), "No lord damage in this match",
                          s_analysis.hasLordDamage ? "Its analysis has no score events; reopen the match to read it again." : "Neither Guild Lord is in the recording.");
        return y + Px(200);
    }
    y = DrawVerdict(dl, m, d, host, x0, x1, y) + Px(26);

    // The score through the match: Score race (with the two panels) or Tug of war.
    const float pTop = y;
    const ImU32 panelBg = PanelBg;
    const float ix0 = x0 + Px(22), ix1 = x1 - Px(22);
    float yy = y + Px(16);
    Text(dl, T(14, true), ImVec2(ix0, yy + Px(4)), Head, "The score through the match");
    float sw = 0.f;
    static const char* const kViews[] = { "Score race", "Tug of war" };
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    const int pick = Switch(dl, "##lv_view", ImVec2(ix0 + TextW(T(14, true), "The score through the match") + Px(16), yy), kViews, 2, s_lv.tug ? 1 : 0, &sw);
    if (pick >= 0) s_lv.tug = pick == 1;
    yy += Px(36);
    // The legend: damage by kind, the four ways damage is prevented, what is not in the score.
    {
        float lx = ix0;
        const float ly = yy;
        Icon(dl, IconDamage(host), ImVec2(lx, ly), Px(16)); lx += Px(20);
        for (int kd = 0; kd < 4; ++kd)
            for (int tm = 1; tm <= 2; ++tm)
            {
                dl->AddRectFilled(ImVec2(lx, ly + Px(2)), ImVec2(lx + Px(14), ly + Px(14)), WithAlpha(TeamCol[tm], kShade[kd]), Px(2));
                lx += Px(16);
            }
        lx += Px(4);
        const char* dmgText = "Damage: weapon hits, attack skills, spells and signets, no skill named";
        Text(dl, T(11.5f), ImVec2(lx, ly + Px(1)), Body, dmgText); lx += TextW(T(11.5f), dmgText) + Px(20);
        Text(dl, T(11.5f), ImVec2(lx, ly + Px(1)), Faint, "Prevented:"); lx += TextW(T(11.5f), "Prevented:") + Px(10);
        for (int mc = 0; mc < 4; ++mc)
        {
            Pattern(dl, ImVec2(lx, ly + Px(2)), ImVec2(lx + Px(16), ly + Px(15)), mc);
            lx += Px(22);
            const std::string nm = Lower(MechName[mc]);
            Text(dl, T(11.5f), ImVec2(lx, ly + Px(1)), Body, nm.c_str()); lx += TextW(T(11.5f), nm.c_str()) + Px(18);
        }
        Icon(dl, IconEnchant(), ImVec2(lx, ly + Px(1)), Px(14)); lx += Px(18);
        Icon(dl, IconSteal(), ImVec2(lx, ly), Px(16)); lx += Px(20);
        dl->AddRectFilled(ImVec2(lx, ly + Px(2)), ImVec2(lx + Px(16), ly + Px(15)), WithAlpha(TeamCol[1], 0.66f), Px(2));
        Pattern(dl, ImVec2(lx, ly + Px(2)), ImVec2(lx + Px(16), ly + Px(15)), -1);
        lx += Px(22);
        Text(dl, T(11.5f), ImVec2(lx, ly + Px(1)), Body, "Not in the score"); lx += TextW(T(11.5f), "Not in the score") + Px(18);
        dl->AddLine(ImVec2(lx, ly + Px(8)), ImVec2(lx + Px(22), ly + Px(8)), Hex(0xe8e2d6), Px(2.2f)); lx += Px(28);
        Text(dl, T(11.5f), ImVec2(lx, ly + Px(1)), Body, "Score");
    }
    yy += Px(28);
    if (!s_lv.tug)
    {
        if (Check(dl, "##lv_poss", ImVec2(ix0, yy), "Show estimated damage without protection", s_lv.possible)) s_lv.possible = !s_lv.possible;
        yy += Px(34);
    }
    dl->ChannelsSetCurrent(1);
    if (s_lv.tug) yy = DrawTug(dl, d, host, ix0, ix1, yy);
    else
    {
        yy = DrawRace(dl, d, ix0, ix1, yy, s_lv.possible) + Px(36);
        const float half = (ix1 - ix0) * 0.5f;
        const float l = DrawSidePanel(dl, m, d, host, 1, ix0, half - Px(36), yy);
        const float r = DrawSidePanel(dl, m, d, host, 2, ix0 + half + Px(36), half - Px(36), yy);
        dl->AddLine(ImVec2(ix0 + half, yy - Px(24)), ImVec2(ix0 + half, std::max(l, r) - Px(6)), Hex(0xffffff, 0x12));
        yy = std::max(l, r);
    }
    yy += Px(12);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(ImVec2(x0, pTop), ImVec2(x1, yy), panelBg, Px(6));
    Outline(dl, ImVec2(x0, pTop), ImVec2(x1, yy), PanelLine, Px(6));
    dl->ChannelsMerge();
    y = yy + Px(30);

    // Skills on the lords.
    Text(dl, T(14, true), ImVec2(x0, y), Head, "Skills on the lords");
    y += Px(28);
    float scaleMax = 1.f;
    for (int t = 1; t <= 2; ++t)
    {
        for (const auto& [k, r] : s_lm.side[t].lordRows) scaleMax = std::max(scaleMax, (float)r.amount);
        for (int mc = 0; mc < 4; ++mc)
        {
            std::map<int, double> per;
            for (const auto& [sk, p] : s_lm.side[t].mechSkills[mc]) per[sk] += p.amount;
            for (const auto& [sk, v] : per) scaleMax = std::max(scaleMax, (float)v);
        }
    }
    scaleMax = std::ceil(scaleMax * 1.12f / 500.f) * 500.f;
    const float cw = (x1 - x0 - Px(20)) * 0.5f;
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    const float l = DrawLordCard(dl, m, d, host, 2, x0, cw, y, scaleMax);
    const float r = DrawLordCard(dl, m, d, host, 1, x0 + cw + Px(20), cw, y, scaleMax);
    const float bottom = std::max(l, r);
    dl->ChannelsSetCurrent(0);
    for (int i = 0; i < 2; ++i)
    {
        const float cx = x0 + i * (cw + Px(20));
        dl->AddRectFilled(ImVec2(cx, y), ImVec2(cx + cw, bottom), Hex(0xffffff, 0x05), Px(6));
        Outline(dl, ImVec2(cx, y), ImVec2(cx + cw, bottom), PanelLine, Px(6));
    }
    dl->ChannelsMerge();
    return bottom + Px(10);
}
