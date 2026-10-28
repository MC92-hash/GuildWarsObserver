#include "pch.h"
#include "TeamColors.h"
#include "ReplayWindow.h"
#include "ReplayWindow_Internal.h"
#include "SkillDatabase.h"
#include "FontConfig.h"
#include "ui/ui_kit.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <array>
#include <map>
#include <numeric>

// ---------------------------------------------------------------------------
// Damage & Healing (the Battle Ledger, gwobserver-private docs/BATTLE_LEDGER_PLAN.md).
//
// One line per damage, heal or prevention packet and one per interrupt attempt, built once over
// the whole match from the damage attribution - the table the overhead numbers read - so every
// total here is the sum of the numbers that floated over the players. The panel adds the lines
// up for any stretch of the match: the full match, up to the playhead, the last 60 s, or a
// selection dragged on the time strip.
// ---------------------------------------------------------------------------

namespace
{
    using Kind = ReplayWindow::LedgerKind;

    // The client's condition "skills" by their own names (the skill data has none for them).
    std::string ConditionName(int id)
    {
        switch (id)
        {
        case 478:  return "Bleeding";
        case 479:  return "Blind";
        case 480:  return "Burning";
        case 481:  return "Crippled";
        case 482:  return "Deep Wound";
        case 483:  return "Disease";
        case 484:  return "Poison";
        case 485:  return "Dazed";
        case 486:  return "Weakness";
        case 2077: return "Cracked Armor";
        default:   return "Condition " + std::to_string(id);
        }
    }

    // A skill that interrupts: its text says so ("Interrupts a spell", "interrupts an action").
    // A skill that interrupts its target when it lands, whatever else happens: a sentence of its
    // concise text that STARTS with "Interrupts an action / a spell / a skill / a spell or chant /
    // an attack / target foe / actions", with no condition in it ("if you land a critical hit",
    // "if target foe has a Deep Wound", "next attack") and none after it ("No effect unless").
    // A hex or enchantment that interrupts later ("(4 seconds.) ... Interrupts next attack",
    // Spinal Shivers, Web of Disruption), an incidental one (Rust's under Overcast, Maelstrom,
    // Lightning Javelin "if your target is attacking", Choking Gas) and a party buff (Anthem of
    // Disruption) are not interrupt attempts. "Interruption effect: ... if the action was a spell"
    // is the effect's, not a condition. The set: SKILL_RULES.md, Interrupt attempt.
    bool TextInterrupts(const SkillInfo* si)
    {
        if (!si) return false;
        const std::string& c = si->concise;
        if (c.empty() || c[0] == '(') return false;                 // an effect with a duration: later, not on use
        if (c.find("No effect unless") != std::string::npos) return false;   // Disrupting Throw
        static const char* kOpen[] = { "Interrupts an action", "Interrupts actions", "Interrupts a spell",
                                       "Interrupts a skill", "Interrupts an attack", "Interrupts target foe" };
        size_t from = 0;
        while (from < c.size())
        {
            size_t end = c.find(". ", from);
            if (end == std::string::npos) end = c.size();
            const std::string sentence = c.substr(from, end - from);
            for (const char* o : kOpen)
                if (sentence.rfind(o, 0) == 0)
                    return sentence.find(" if ") == std::string::npos && sentence.find(" next ") == std::string::npos;
            from = end + 2;
        }
        return false;
    }
}

void ReplayWindow::BuildBattleLedger() const
{
    m_ledger.clear();
    m_ledgerInts.clear();
    const DamageAttribution::Table* table = DamageAttributionTable();
    if (!table) return;
    m_ledgerBuilt = true;
    m_ledgerUsedAttributes = m_attributesDeduced;

    // --- Damage, heal and prevention lines: the floaters' packets with the floaters' numbers ---
    const auto& combat = m_replayCtx.stocData.combat;
    std::vector<DamageAttribution::PreventHit> preventHits;     // every hit, for the protection estimates
    std::unordered_set<uint64_t> struckTicks;                    // (bearer, ms) where a struck heal paid
    for (size_t i = 0; i < combat.size() && i < table->packets.size(); ++i)
        if (table->packets[i].rule == DamageAttribution::Rule::StruckHeal)
            struckTicks.insert((uint64_t)(uint32_t)combat[i].target_id << 32 | (uint32_t)std::llround(combat[i].time * 1000.0));
    for (size_t i = 0; i < combat.size() && i < table->packets.size(); ++i)
    {
        const CombatEvent& ce = combat[i];
        if (!ce.IsDamageOrHeal()) continue;
        const DamageAttribution::Result& r = table->packets[i];
        const uint32_t fix = i < table->maxHpFix.size() ? table->maxHpFix[i] : 0;
        const uint32_t mhp = fix ? fix : PacketMaxHp(ce.target_id, ce.time, ce.value);
        if (!mhp) continue;
        LedgerLine ln;
        ln.t       = ce.time;
        ln.src     = ce.caster_id;
        ln.dst     = ce.target_id;
        ln.amount  = (int)std::lround(std::fabs(ce.value) * (double)mhp);
        ln.skillId = r.skillId > 0 ? m_skillView.ResolvePvpSkillId(r.skillId) : r.skillId;
        ln.weaponHit = r.weaponHit;
        if (r.creditTo) ln.src = r.creditTo;          // a cause-0 heal the rules gave to a player (a party shout)
        // The Health a player comes back with when resurrected is not counted at all (owner): a cause-0
        // heal the rules left unnamed, on a player dead in the second before it.
        if (ce.type == "HEAL" && ce.caster_id == 0 && r.rule == DamageAttribution::Rule::None)
            if (auto ait = m_replayCtx.agents.find(ce.target_id); ait != m_replayCtx.agents.end())
            {
                const auto& sn = ait->second.snapshots;
                auto it = std::lower_bound(sn.begin(), sn.end(), ce.time - 1.f,
                                           [](const AgentSnapshot& a, float x) { return a.time < x; });
                bool wasDead = false;
                const AgentSnapshot* last = nullptr;          // his last snapshot before the heal
                for (; it != sn.end() && it->time < ce.time; ++it)
                {
                    if (it->is_dead) wasDead = true;
                    last = &*it;
                }
                // ...or brought back at full health: 0 Health (or not alive) in his last snapshot before it,
                // however long ago (a dead player's snapshots can stop for half a minute).
                if (!last)
                {
                    auto at = std::lower_bound(sn.begin(), sn.end(), ce.time,
                                               [](const AgentSnapshot& a, float x) { return a.time < x; });
                    if (at != sn.begin()) last = &*std::prev(at);
                }
                if (last && (last->health_pct <= 0.f || !last->is_alive || last->is_dead)) wasDead = true;
                if (wasDead) continue;
                // The victory heal (owner): when a team wins, each of its players is healed for 300, with
                // no cause, 3.0 s before the recording's match end (435 heals over the 58 recordings
                // since 2026-10-15, all on the winners, all at end - 3.0 s). Not counted either.
                const float endT = (float)m_matchMeta.match_end_time_ms / 1000.f;
                const int amount = (int)std::lround(std::fabs(ce.value) * (double)mhp);
                if (m_matchMeta.winner_party_id > 0 && endT > 0.f && std::fabs(ce.time - (endT - 3.f)) <= 1.5f &&
                    ait->second.teamId == m_matchMeta.winner_party_id && amount >= 290 && amount <= 310)
                    continue;
            }
        // Every hit, a whole one (0) too: a stacking guard counts it.
        if (ce.type == "DAMAGE")
            preventHits.push_back({ ce.time, ce.caster_id, ce.target_id, r.skillId > 0 ? r.skillId : r.weaponHit ? -1 : 0,
                                    std::max(0, ln.amount), (int)mhp, (int)ce.damage_type,
                                    struckTicks.count((uint64_t)(uint32_t)ce.target_id << 32 | (uint32_t)std::llround(ce.time * 1000.0)) != 0 });
        if (ln.amount <= 0) continue;
        if (ce.type == "DAMAGE")
        {
            ln.kind = r.rule == DamageAttribution::Rule::SelfCost ? Kind::SelfCost : Kind::Damage;
            // A map hazard (spiked coral, acid trap, Minion Apocalypse): the map's, never a player's.
            if (r.rule == DamageAttribution::Rule::Hazard)
            {
                ln.src = 0;
                ln.skillId = r.skillId;     // the hazard's own icon id
                ln.rowKey = r.skillId == DamageAttribution::kSpikedCoral ? kLedgerRowCoral
                          : r.skillId == DamageAttribution::kAcidTrap    ? kLedgerRowAcid
                                                                         : kLedgerRowFlux;
            }
            // A minion's hit is its master's, on his "Minions" row.
            if (const auto* mn = table->MinionAt(ce.caster_id, ce.time))
            {
                ln.src = mn->master;
                ln.rowKey = kLedgerRowMinions;
                ln.skillId = m_skillView.ResolvePvpSkillId(mn->skillId);
                ln.weaponHit = false;
            }
        }
        else
            ln.kind = r.rule == DamageAttribution::Rule::StruckHeal  ? Kind::Prevented
                    : r.rule == DamageAttribution::Rule::DivineFavor ? Kind::DivineFavor
                                                                     : Kind::Heal;
        m_ledger.push_back(ln);
    }

    // --- Degeneration: the Health each player lost to a negative regeneration slope, per source ---
    // hp_pips is his net slope in maximum Health per second (pips = round(hp_pips x max / 2), a pip
    // being 2 Health a second, at most 10), so regeneration is already taken off.
    if (const EffectTimeline::Table* fx = EffectTimelineTable())
    {
        DamageAttribution::DegenInputs din;
        for (int pid : m_playerIds) din.agents.push_back(pid);
        din.samples = [&](int agent, std::vector<DamageAttribution::DegenSample>& out) {
            auto it = m_replayCtx.agents.find(agent);
            if (it == m_replayCtx.agents.end()) return;
            for (const AgentSnapshot& s : it->second.snapshots)
            {
                float loss = 0.f;
                if (s.hp_pips < 0.f && s.is_alive && !s.is_dead && s.health_pct > 0.f)
                    if (const uint32_t mhp = FloaterMaxHp(agent, s.time))
                    {
                        const int pips = std::clamp((int)std::lround(s.hp_pips * (float)mhp / 2.f), -10, 10);
                        if (pips < 0) loss = -2.f * (float)pips;
                    }
                out.push_back({ s.time, loss });
            }
        };
        din.effectsOn = [fx](int agent, float t, std::vector<DamageAttribution::DegenEffect>& out) {
            for (const EffectTimeline::Effect* e : fx->ActiveAt(agent, t))
            {
                if (e->kind == EffectTimeline::Kind::Condition) out.push_back({ e->skillId, e->causeSkill, e->caster, true });
                else if (e->kind == EffectTimeline::Kind::Hex)  out.push_back({ e->skillId, 0, e->caster, false });
            }
        };
        std::unordered_map<int, DamageAttribution::SkillFacts> facts;
        din.skill = [&](int skillId) -> const DamageAttribution::SkillFacts* {
            auto it = facts.find(skillId);
            if (it != facts.end()) return &it->second;
            const SkillInfo* si = m_skillView.Get(skillId);
            if (!si) return nullptr;
            return &facts.emplace(skillId, DamageAttribution::SkillFacts{ si->type, si->profession, si->name, si->description, si->sacrifice, si->attribute }).first->second;
        };
        for (const DamageAttribution::DegenLine& d : DamageAttribution::SplitDegeneration(din))
        {
            LedgerLine ln;
            ln.t      = d.time;
            ln.dst    = d.victim;
            ln.src    = d.caster;
            ln.amount = d.health;
            ln.kind   = d.caster != 0 && d.caster == d.victim ? Kind::SelfCost : Kind::Damage;
            if (d.skillId == 0)
                ln.rowKey = kLedgerRowDegenUnknown;
            else if (d.condition && d.skillId == DamageAttribution::kMiasma)
            {
                ln.src = 0;                 // the Isle of Meditation's Miasma: the map's
                ln.kind = Kind::Damage;
                ln.rowKey = kLedgerRowMiasma;
                ln.skillId = d.skillId;
            }
            else if (d.condition)
            {
                const int cause = d.causeSkill > 0 ? m_skillView.ResolvePvpSkillId(d.causeSkill) : 0;
                ln.rowKey  = LedgerDegenRow(d.skillId, cause);
                ln.skillId = cause > 0 ? cause : d.skillId;
            }
            else
                ln.skillId = m_skillView.ResolvePvpSkillId(d.skillId);   // a degeneration hex: its own row
            m_ledger.push_back(ln);
        }
    }
    // --- Damage prevented without a heal: flat, percentage and armour guards on the hits their
    //     bearer took (exact), hits a cap held down and melee attacks a block stopped (estimates) ---
    if (const EffectTimeline::Table* fx = EffectTimelineTable())
    {
        DamageAttribution::PreventInputs pin;
        pin.hits = std::move(preventHits);
        std::stable_sort(pin.hits.begin(), pin.hits.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        for (const BasicAttackEvent& b : m_replayCtx.stocData.basicAttack)
            if (b.type == "ATTACK_FINISHED") pin.swings.push_back({ b.time, b.caster_id, b.target_id, -1 });
        for (const auto& [agentId, ard] : m_replayCtx.agents)
            for (const SkillUseEvent& su : ard.skillUseHistory)
            {
                if (su.wasCancelled || su.wasInterrupted || su.targetId <= 0 || su.targetId == agentId) continue;
                const SkillInfo* si = m_skillView.Get(su.skillId);
                const bool melee = si && si->type >= 3 && si->type <= 11;   // attack skills, bows / spears / ranged left out
                if (melee) pin.swings.push_back({ su.endTime, agentId, su.targetId, su.skillId });
            }
        std::stable_sort(pin.swings.begin(), pin.swings.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        pin.effectsOn = [fx](int agent, float t, std::vector<DamageAttribution::PreventEffect>& out) {
            for (const EffectTimeline::Effect* e : fx->ActiveAt(agent, t)) out.push_back({ e->skillId, e->caster, e->start });
        };
        pin.blindAt = [this](int agent, float t) {
            auto it = m_replayCtx.agents.find(agent);
            if (it == m_replayCtx.agents.end()) return false;
            const AgentSnapshot* s = FindSnapshotAtTime(it->second, t);
            return s && s->has_blind;
        };
        std::unordered_map<int, DamageAttribution::SkillFacts> pfacts;
        pin.skill = [&](int skillId) -> const DamageAttribution::SkillFacts* {
            auto it = pfacts.find(skillId);
            if (it != pfacts.end()) return &it->second;
            const SkillInfo* si = m_skillView.Get(skillId);
            if (!si) return nullptr;
            return &pfacts.emplace(skillId, DamageAttribution::SkillFacts{ si->type, si->profession, si->name, si->description, si->sacrifice, si->attribute }).first->second;
        };
        pin.rank = [this](int agentId, int attribute) -> int {
            auto it = m_attrProfiles.find(agentId);
            if (it == m_attrProfiles.end()) return -1;
            const AttributeModel::AttributeRange* a = AttributeModel::SolvedRange(&it->second, attribute);
            return a ? a->best : -1;
        };
        for (const DamageAttribution::Prevention& pv : DamageAttribution::EstimatePrevention(pin))
        {
            if (pv.health <= 0 && pv.count == 0) continue;
            LedgerLine ln;
            ln.t = pv.time; ln.src = pv.protector; ln.dst = pv.bearer;
            ln.amount = pv.health;
            ln.skillId = m_skillView.ResolvePvpSkillId(pv.skillId);
            ln.kind = Kind::Prevented;
            ln.estimated = pv.estimated;
            ln.count = pv.count;
            ln.preventGroup = (uint8_t)pv.group + 1;
            m_ledger.push_back(ln);
        }
    }
    // --- Map effects with no packet: the catapult's hits (read from the health snapshots, as the
    //     overhead numbers show them) and the healing water's regeneration. Never a player's. ---
    for (int pid : m_playerIds)
    {
        std::vector<DamagePop> hits;
        AppendCatapultHits(pid, hits);
        for (const DamagePop& h : hits)
        {
            const uint32_t mhp = FloaterMaxHp(pid, h.time);
            const int amount = mhp ? (int)std::lround(std::fabs(h.value) * (double)mhp) : 0;
            if (amount <= 0) continue;
            LedgerLine ln;
            ln.t = h.time; ln.src = 0; ln.dst = pid; ln.amount = amount;
            ln.kind = Kind::Damage; ln.rowKey = kLedgerRowCatapult;
            m_ledger.push_back(ln);
        }
    }
    // The water pays while the effect timeline has its Terrain effect on him (the zone it located
    // from his position), he is alive and not full, and his slope shows the pip: 2 Health a second,
    // summed into 1 s lines with the remainder carried.
    if (const EffectTimeline::Table* fx = EffectTimelineTable())
        for (int pid : m_playerIds)
        {
            auto it = m_replayCtx.agents.find(pid);
            if (it == m_replayCtx.agents.end()) continue;
            const auto& snaps = it->second.snapshots;
            double acc = 0.0;
            long long bucket = -1;
            float last = 0.f;
            auto flush = [&]() {
                const int whole = (int)std::floor(acc);
                if (whole <= 0) return;
                acc -= whole;
                LedgerLine ln;
                ln.t = last; ln.src = 0; ln.dst = pid; ln.amount = whole;
                ln.kind = Kind::Heal; ln.rowKey = kLedgerRowWater;
                m_ledger.push_back(ln);
            };
            for (size_t k = 0; k + 1 < snaps.size(); ++k)
            {
                const AgentSnapshot& s = snaps[k];
                if (s.hp_pips <= 0.f || !s.is_alive || s.is_dead || s.health_pct <= 0.f || s.health_pct >= 0.999f) continue;
                bool water = false;
                for (const EffectTimeline::Effect* e : fx->ActiveAt(pid, s.time))
                    if (e->kind == EffectTimeline::Kind::Terrain) { water = true; break; }
                if (!water) continue;
                const uint32_t mhp = FloaterMaxHp(pid, s.time);
                if (!mhp || std::lround(s.hp_pips * (float)mhp / 2.f) < 1) continue;
                const long long b = (long long)std::floor(s.time);
                if (b != bucket) { flush(); bucket = b; }
                acc += 2.0 * (double)std::min(1.f, snaps[k + 1].time - s.time);
                last = s.time;
            }
            flush();
        }
    std::stable_sort(m_ledger.begin(), m_ledger.end(), [](const LedgerLine& a, const LedgerLine& b) { return a.t < b.t; });

    // --- Interrupt attempts: every completed cast of an interrupting skill on a foe ---
    // An INTERRUPTED packet names the agent who lost his cast; it belongs to the attempt on him that
    // finished last before it, inside a second (a shot still has to fly).
    std::unordered_map<int, std::vector<float>> stopped;      // victim -> INTERRUPTED times
    for (const CombatEvent& ce : combat)
        if (ce.type == "INTERRUPTED") stopped[ce.caster_id].push_back(ce.time);
    std::unordered_map<int, bool> interrupts;
    auto IsInterruptSkill = [&](int skillId) {
        auto it = interrupts.find(skillId);
        if (it != interrupts.end()) return it->second;
        return interrupts[skillId] = TextInterrupts(m_skillView.Get(skillId));
    };
    struct Attempt { int src; const SkillUseEvent* use; };
    std::vector<Attempt> attempts;
    for (int pid : m_playerIds)
    {
        auto ait = m_replayCtx.agents.find(pid);
        if (ait == m_replayCtx.agents.end()) continue;
        for (const SkillUseEvent& su : ait->second.skillUseHistory)
        {
            if (su.wasCancelled || su.wasInterrupted || su.targetId < 0 || su.targetId == pid) continue;
            if (!IsInterruptSkill(su.skillId)) continue;
            auto tit = m_replayCtx.agents.find(su.targetId);
            if (tit == m_replayCtx.agents.end() || tit->second.teamId == ait->second.teamId) continue;
            attempts.push_back({ pid, &su });
        }
    }
    std::sort(attempts.begin(), attempts.end(), [](const Attempt& a, const Attempt& b) { return a.use->endTime < b.use->endTime; });
    std::unordered_map<int, size_t> nextStop;                 // victim -> first INTERRUPTED not yet claimed
    for (const Attempt& at : attempts)
    {
        const SkillUseEvent& su = *at.use;
        LedgerInterrupt li;
        li.t       = su.endTime;
        li.src     = at.src;
        li.dst     = su.targetId;
        li.skillId = m_skillView.ResolvePvpSkillId(su.skillId);
        li.outcome = LedgerInterrupt::Missed;
        const auto& victim = m_replayCtx.agents.at(su.targetId);

        auto sit = stopped.find(su.targetId);
        if (sit != stopped.end())
        {
            size_t& k = nextStop[su.targetId];
            while (k < sit->second.size() && sit->second[k] < su.endTime - 0.05f) ++k;
            if (k < sit->second.size() && sit->second[k] <= su.endTime + 1.0f)
            {
                const float hit = sit->second[k++];
                li.outcome = LedgerInterrupt::Landed;
                li.t = hit;
                // The cast he lost: his interrupted use that ends on the packet.
                for (const SkillUseEvent& v : victim.skillUseHistory)
                {
                    if (v.startTime > hit) break;
                    if (v.wasInterrupted && std::fabs(v.endTime - hit) < 0.3f)
                    {
                        li.lostSkill = m_skillView.ResolvePvpSkillId(v.skillId);
                        li.afterMs   = (int)std::lround((hit - v.startTime) * 1000.f);
                        const float rx = su.startTime - v.startTime;
                        if (rx >= 0.f) li.reflexMs = (int)std::lround(rx * 1000.f);
                    }
                }
            }
        }
        if (li.outcome == LedgerInterrupt::Missed)
        {
            // Faked: the target stopped his own cast in the quarter second before it would have landed.
            for (const SkillUseEvent& v : victim.skillUseHistory)
            {
                if (v.startTime > su.endTime + 0.05f) break;
                if (v.wasCancelled && !v.wasInterrupted && v.endTime >= su.endTime - 0.25f && v.endTime <= su.endTime + 0.05f)
                {
                    li.outcome   = LedgerInterrupt::Faked;
                    li.lostSkill = m_skillView.ResolvePvpSkillId(v.skillId);
                }
            }
        }
        m_ledgerInts.push_back(li);
    }

    // Developer dump beside the damage one: %TEMP%\gwo_ledger_<match>.txt whenever GWO_DAMAGE_DEBUG
    // is set. One line per ledger line.
    char flag[16] = {};
    char tempDir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("GWO_DAMAGE_DEBUG", flag, (DWORD)sizeof(flag)) == 0 || GetTempPathA(MAX_PATH, tempDir) == 0)
        return;
    std::string folder = m_replayCtx.matchFolderPath.filename().string();
    if (folder.empty()) folder = "replay";
    for (char& c : folder)
        if (!std::isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') c = '_';
    FILE* f = nullptr;
    if (fopen_s(&f, (std::string(tempDir) + "gwo_ledger_" + folder + ".txt").c_str(), "w") != 0 || !f) return;
    static const char* kKind[] = { "damage", "self_cost", "heal", "divine_favor", "prevented" };
    fprintf(f, "# time\tkind\tsrc\tdst\tamount\tskill\trow\tweapon_hit\testimated\tcount\tprevent_group\n");
    for (const LedgerLine& ln : m_ledger)
        fprintf(f, "%.3f\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", ln.t, kKind[(int)ln.kind], ln.src, ln.dst, ln.amount,
                ln.skillId, ln.rowKey, ln.weaponHit ? 1 : 0, ln.estimated ? 1 : 0, ln.count, (int)ln.preventGroup);
    fclose(f);
}

// ---------------------------------------------------------------------------
// The panel. Same face as the Character panel and the Piano Roll: the bronze chrome, gold
// headings, hand-drawn rows on hairlines, flat bars on a faint track, team colours on names.
// ---------------------------------------------------------------------------
namespace
{
    enum Metric { kDealt, kTaken, kHealed, kReceived, kPrevented, kInts, kIntsRecv, kMetricCount };
    const char* kMetricName[kMetricCount] = { "Damage dealt", "Damage taken", "Healing done", "Healing received",
                                              "Damage prevented", "Interrupts", "Interrupts received" };
    enum Scope { kFull, kPlayhead, kLast60, kSelection };
    const char* kScopeName[] = { "Full match", "Up to now", "Last 60 s", "Selection" };

    // The Character panel's palette (ReplayWindow_CharacterPanel.cpp, ReplayWindow_PianoRoll.cpp).
    constexpr ImU32 kGold      = IM_COL32(225, 190, 80, 255);
    constexpr ImU32 kNameText  = IM_COL32(216, 220, 226, 255);
    constexpr ImU32 kMuted     = IM_COL32(122, 128, 135, 255);
    constexpr ImU32 kSoft      = IM_COL32(170, 176, 184, 255);
    constexpr ImU32 kTabText   = IM_COL32(153, 160, 170, 255);
    constexpr ImU32 kRule      = IM_COL32(255, 255, 255, 26);
    constexpr ImU32 kBronze    = IM_COL32(0x66, 0x54, 0x26, 0x66);
    constexpr ImU32 kHairline  = IM_COL32(255, 255, 255, 8);
    constexpr ImU32 kTrack     = IM_COL32(255, 255, 255, 15);
    constexpr ImU32 kHover     = IM_COL32(255, 255, 255, 12);
    constexpr ImU32 kSelFill   = IM_COL32(225, 190, 80, 30);
    // Team name colours of the Combat Log and the Piano Roll.
    constexpr ImU32 kBlueText  = IM_COL32(74, 200, 255, 255);
    constexpr ImU32 kRedText   = IM_COL32(255, 107, 107, 255);
    constexpr ImU32 kBlueBar   = IM_COL32(74, 144, 216, 178);
    constexpr ImU32 kRedBar    = IM_COL32(208, 72, 72, 178);
    constexpr ImU32 kDamage    = IM_COL32(255, 115, 102, 255);
    constexpr ImU32 kHealing   = IM_COL32(140, 230, 128, 255);
    constexpr ImU32 kPrevent   = IM_COL32(230, 205, 110, 255);
    constexpr ImU32 kPurple    = IM_COL32(191, 97, 255, 255);   // mockup / Combat Log interrupt purple
    constexpr ImU32 kOrange    = IM_COL32(255, 159, 64, 255);

    int   sMetric = kDealt;
    bool  sMatrix = false;
    int   sScope  = kFull;
    float sSelA = 0.f, sSelB = 0.f;
    bool  sHasSel = false;
    int   sSelPlayer = -1;
    bool  sShowAll = false;
    int   sMxSide = 0;                       // 0: Blue's players give, 1: Red's
    std::unordered_set<int> sOpenRows;       // expanded rows of the selected player's table
    float sDragFrom = -1.f;
    double sSelHintUntil = 0.0;              // "drag on the strip" shown after clicking Selection with none made

    ImU32 TeamText(int team) { return Team::IsBlue(team) ? kBlueText : Team::IsRed(team) ? kRedText : kNameText; }
    ImU32 TeamBar(int team)  { return Team::IsBlue(team) ? kBlueBar : Team::IsRed(team) ? kRedBar : IM_COL32(200, 200, 200, 150); }
    ImU32 WithAlpha(ImU32 c, int a) { return (c & 0x00FFFFFF) | ((ImU32)a << 24); }
    ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

    std::string FmtN(long long v)
    {
        std::string raw = std::to_string(v < 0 ? -v : v), out;
        for (int i = (int)raw.size() - 1, n = 0; i >= 0; --i, ++n)
        {
            if (n && n % 3 == 0) out.insert(out.begin(), ',');
            out.insert(out.begin(), raw[i]);
        }
        return v < 0 ? "-" + out : out;
    }
    std::string FmtT(float t)
    {
        const int s = std::max(0, (int)t);
        char b[16]; snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60); return b;
    }
    std::string FmtMs(int ms) { return ms < 0 ? std::string("-") : FmtN(ms) + " ms"; }
    std::string Pct(long long a, long long b) { return b ? std::to_string((int)std::lround(100.0 * a / b)) + "%" : std::string("-"); }
    int Median(std::vector<int> v)
    {
        if (v.empty()) return -1;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    void Help(const char* text)
    {
        ImGui::TextDisabled("(?)");
        if (!ImGui::IsItemHovered()) return;
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // A heading tab: plain text, gold and underlined when chosen, like the panel headings.
    bool TextTab(const char* label, bool active)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 ts = ImGui::CalcTextSize(label), p = ImGui::GetCursorScreenPos();
        const ImVec2 sz(ts.x + 14.f, ts.y + 9.f);
        const bool clicked = ImGui::InvisibleButton(label, sz);
        const bool hov = ImGui::IsItemHovered();
        if (hov && !active) dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), kHover, 3.f);
        dl->AddText(ImVec2(p.x + 7.f, p.y + 3.f), active ? kGold : hov ? kNameText : kTabText, label);
        if (active) dl->AddRectFilled(ImVec2(p.x + 7.f, p.y + sz.y - 2.f), ImVec2(p.x + sz.x - 7.f, p.y + sz.y), kGold, 1.f);
        return clicked;
    }

    // The Combat Log's filter pill (ReplayWindow_CombatLog.cpp FilterPill), with an optional icon:
    // round, faint when off, dark gold with a gold edge when on.
    bool IconTab(const char* label, ImTextureID icon, bool active)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float iconS = 16.f, h = ImGui::GetTextLineHeight() + 10.f;
        const ImVec2 ts = ImGui::CalcTextSize(label), p = ImGui::GetCursorScreenPos();
        const float w = ts.x + 22.f + (icon ? iconS + 5.f : 0.f);
        const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
        const bool hov = ImGui::IsItemHovered();
        const ImVec2 q(p.x + w, p.y + h);
        dl->AddRectFilled(p, q, active ? (hov ? IM_COL32(51, 41, 15, 255) : IM_COL32(36, 28, 10, 255))
                                       : IM_COL32(255, 255, 255, hov ? 31 : 13), h * 0.5f);
        dl->AddRect(p, q, active ? IM_COL32(255, 214, 100, 204) : IM_COL32(255, 255, 255, 20), h * 0.5f);
        float x = p.x + 11.f;
        if (icon)
        {
            dl->AddImage(icon, ImVec2(x, p.y + (h - iconS) * 0.5f), ImVec2(x + iconS, p.y + (h + iconS) * 0.5f));
            x += iconS + 5.f;
        }
        dl->AddText(ImVec2(x, p.y + (h - ts.y) * 0.5f), active ? IM_COL32(255, 232, 176, 255) : IM_COL32(153, 163, 176, 255), label);
        return clicked;
    }

    // A number in the client's own digits (the overhead damage / heal / energy fonts), thousands
    // grouped by a thin gap since the atlas has no comma. Returns the width drawn.
    float GlyphNumber(ImDrawList* dl, const BitmapFont& bf, long long v, float x, float yTop, float h, bool alignRight)
    {
        std::string raw = std::to_string(v < 0 ? -v : v);
        std::vector<std::string> groups;
        for (int end = (int)raw.size(); end > 0; end -= 3) groups.insert(groups.begin(), raw.substr(std::max(0, end - 3), end - std::max(0, end - 3)));
        if (v < 0) groups.front() = "-" + groups.front();
        if (!bf.srv.Get())
        {
            const std::string s = (v < 0 ? "-" : "") + raw;
            const float w = ImGui::CalcTextSize(s.c_str()).x;
            dl->AddText(ImVec2(alignRight ? x - w : x, yTop), kNameText, s.c_str());
            return w;
        }
        const float gap = h * 0.22f;
        float w = gap * (groups.size() - 1);
        for (const std::string& g : groups) w += bf.MeasureString(g.c_str(), h);
        float cx = alignRight ? x - w : x;
        for (const std::string& g : groups)
        {
            const float gw = bf.MeasureString(g.c_str(), h);
            bf.DrawString(dl, g.c_str(), cx + gw * 0.5f, yTop + h * 0.5f, h, 255);
            cx += gw + gap;
        }
        return w;
    }

    // Headline figures (KPIs) on one faint panel: a small uppercase label, then the number in the
    // client's digits with a short note after it.
    struct Kpi { long long value; std::string suffix, label; bool none = false; };
    void KpiRow(const BitmapFont& bf, const std::vector<Kpi>& kpis)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float fs = ImGui::GetFontSize(), digitH = 22.f, h = fs + digitH + 26.f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float W = ImGui::GetContentRegionAvail().x;
        dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + h), IM_COL32(255, 255, 255, 10), 4.f);
        ImFont* lf = ImGui::GetFont();
        float x = p0.x + 14.f;
        for (const Kpi& k : kpis)
        {
            std::string up = k.label;
            for (char& c : up) c = (char)std::toupper((unsigned char)c);
            dl->AddText(lf, fs, ImVec2(x, p0.y + 10.f), kSoft, up.c_str());
            const float ny = p0.y + 14.f + fs;
            float w = 0.f;
            if (k.none) { dl->AddText(ImVec2(x, ny + (digitH - fs) * 0.5f), kMuted, "-"); w = 10.f; }
            else w = GlyphNumber(dl, bf, k.value, x, ny, digitH, false);
            if (!k.none && !k.suffix.empty())
            {
                dl->AddText(ImVec2(x + w + 5.f, ny + digitH - fs - 2.f), kSoft, k.suffix.c_str());
                w += 5.f + ImGui::CalcTextSize(k.suffix.c_str()).x;
            }
            w = std::max(w, lf->CalcTextSizeA(fs, FLT_MAX, 0.f, up.c_str()).x);
            x += w + 40.f;
        }
        ImGui::Dummy(ImVec2(W, h));
    }

    // A row of pills (the Combat Log's filter pills); returns the clicked index, or -1. A disabled
    // choice draws dim but still reports its click.
    int Segmented(const char* id, const std::vector<std::string>& labels, int active, const std::vector<bool>* enabled = nullptr)
    {
        ImGui::PushID(id);
        int clicked = -1;
        for (size_t i = 0; i < labels.size(); ++i)
        {
            ImGui::PushID((int)i);
            const bool en = !enabled || (*enabled)[i];
            if (!en) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f);
            if (IconTab(labels[i].c_str(), nullptr, (int)i == active)) clicked = (int)i;
            if (!en) ImGui::PopStyleVar();
            ImGui::PopID();
            if (i + 1 < labels.size()) ImGui::SameLine(0.f, 4.f);
        }
        ImGui::PopID();
        return clicked;
    }

    // The player's number in every tab as small boxes (the mockup's stat tiles): an uppercase label
    // over the number in the client's digits; the current tab dark gold with a gold edge.
    int StatStrip(const char* const* labels, const std::vector<long long>& values, const std::vector<const BitmapFont*>& fonts, int active)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int n = (int)values.size();
        const float fs = ImGui::GetFontSize();
        ImFont* lf = ImGui::GetFont();
        const float digitH = 14.f, h = fs + digitH + 16.f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        float x = p0.x;
        int clicked = -1;
        for (int i = 0; i < n; ++i)
        {
            std::string up = labels[i];
            for (char& c : up) c = (char)std::toupper((unsigned char)c);
            const float w = std::max(92.f, lf->CalcTextSizeA(fs, FLT_MAX, 0.f, up.c_str()).x + 18.f);
            const ImVec2 a(x, p0.y), b(x + w, p0.y + h);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##stat", ImVec2(w, h))) clicked = i;
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool on = i == active;
            dl->AddRectFilled(a, b, on ? IM_COL32(36, 28, 10, 255) : IM_COL32(255, 255, 255, hov ? 18 : 8), 2.f);
            dl->AddRect(a, b, on ? IM_COL32(255, 214, 100, 204) : IM_COL32(255, 255, 255, 20), 2.f);
            dl->AddText(lf, fs, ImVec2(a.x + 8.f, a.y + 5.f), on ? IM_COL32(255, 232, 176, 255) : kSoft, up.c_str());
            GlyphNumber(dl, *fonts[i], values[i], a.x + 8.f, a.y + 8.f + fs, digitH, false);
            x += w + 5.f;
        }
        ImGui::SetCursorScreenPos(p0);
        ImGui::Dummy(ImVec2(x - p0.x, h));
        return clicked;
    }

    std::string Ellipsize(std::string s, float width)
    {
        if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
        while (!s.empty() && ImGui::CalcTextSize((s + "...").c_str()).x > width) s.pop_back();
        return s + "...";
    }
    // At most two lines of `width`; the second ends in "..." when the rest does not fit.
    std::pair<std::string, std::string> TwoLines(const std::string& s, float width)
    {
        std::vector<std::string> words;
        for (size_t i = 0; i < s.size();)
        {
            const size_t j = s.find(' ', i);
            words.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
            if (j == std::string::npos) break;
            i = j + 1;
        }
        std::string l1, l2;
        size_t k = 0;
        for (; k < words.size(); ++k)
        {
            const std::string t = l1.empty() ? words[k] : l1 + " " + words[k];
            if (!l1.empty() && ImGui::CalcTextSize(t.c_str()).x > width) break;
            l1 = t;
        }
        l1 = Ellipsize(l1, width);
        for (; k < words.size(); ++k) l2 += (l2.empty() ? "" : " ") + words[k];
        return { l1, Ellipsize(l2, width) };
    }
    // A party label without its "Mo/A20" build prefix, which the profession icon already shows.
    std::string ShortName(const std::string& n)
    {
        const size_t sp = n.find(' ');
        return sp != std::string::npos && n.find('/') < sp ? n.substr(sp + 1) : n;
    }

    void Bar(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, ImU32 fill)
    {
        if (b.x <= a.x) return;
        const float r = std::min(3.f, (b.y - a.y) * 0.5f);
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 18), r);
        if (frac > 0.f)
            dl->AddRectFilled(a, ImVec2(a.x + std::max(3.f, (b.x - a.x) * std::min(1.f, frac)), b.y), (fill & 0x00FFFFFF) | 0xFF000000, r);
    }

    // Gold UPPERCASE section label over a bronze rule, like the Piano Roll card's sections.
    void Section(const char* label, const char* caption)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        dl->AddText(p, kGold, label);
        if (caption)
        {
            const float cw = ImGui::CalcTextSize(caption).x;
            dl->AddText(ImVec2(p.x + w - cw, p.y), kMuted, caption);
        }
        const float y = p.y + ImGui::GetTextLineHeight() + 3.f;
        dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + w, y), kBronze);
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight() + 7.f));
    }

    // Right-aligned text ending at xr.
    void RText(ImDrawList* dl, float xr, float y, ImU32 col, const std::string& s)
    {
        dl->AddText(ImVec2(xr - ImGui::CalcTextSize(s.c_str()).x, y), col, s.c_str());
    }

    bool GivingMetric(int m) { return m == kDealt || m == kHealed || m == kPrevented || m == kInts; }

    int LineMetricGiver(ReplayWindow::LedgerKind k)
    {
        switch (k)
        {
        case ReplayWindow::LedgerKind::Damage:      return kDealt;
        case ReplayWindow::LedgerKind::Heal:
        case ReplayWindow::LedgerKind::DivineFavor: return kHealed;
        case ReplayWindow::LedgerKind::Prevented:   return kPrevented;
        default:                                    return -1;
        }
    }
    int LineMetricTaker(ReplayWindow::LedgerKind k)
    {
        switch (k)
        {
        case ReplayWindow::LedgerKind::Damage:
        case ReplayWindow::LedgerKind::SelfCost:    return kTaken;
        case ReplayWindow::LedgerKind::Heal:
        case ReplayWindow::LedgerKind::DivineFavor: return kReceived;
        default:                                    return -1;
        }
    }
}

void ReplayWindow::DrawBattleLedgerPanel()
{
    if (!m_showSkillAnalytics) return;
    if (!m_agentsClassified) return;
    if (!m_ledgerBuilt || (!m_ledgerUsedAttributes && m_attributesDeduced)) BuildBattleLedger();
    if (!m_spikesBuilt) BuildSpikes();
    if (!m_moraleTimelineBuilt) BuildMoraleTimelines();

    ImGuiIO& io = ImGui::GetIO();
    const float vpW = io.DisplaySize.x, vpH = io.DisplaySize.y;
    ImGui::SetNextWindowSizeConstraints(ImVec2(760.f, 420.f), ImVec2(vpW, vpH));
    if (m_panelLayout.HasSavedSize("skill_analytics"))
        m_panelLayout.ApplySize("skill_analytics");
    else
        ImGui::SetNextWindowSize(ImVec2(std::min(1040.f, vpW), std::min(820.f, vpH * 0.9f)), ImGuiCond_FirstUseEver);
    m_panelLayout.ApplyPosition("skill_analytics");

    // The app's window chrome (Character panel, Piano Roll, Morale, Combat Log).
    ImGui::PushStyleColor(ImGuiCol_WindowBg,             ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,              ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,        ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,               ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,            ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          ImVec4(1.f, 1.f, 1.f, 0.04f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        ImVec4(0.80f, 0.68f, 0.30f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1.f, 0.84f, 0.39f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  ImVec4(1.f, 0.84f, 0.39f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    const bool visible = ImGui::Begin("Damage & Healing###battle_ledger", &m_showSkillAnalytics);
    m_panelLayout.TrackWindow("skill_analytics");
    if (!visible)
    {
        ImGui::End();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(9);
        return;
    }

    EnsureSkillIconIndex();
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    const float tEnd = std::max(1.f, m_replayCtx.maxReplayTime);
    const float tNow = std::clamp(m_debugTimeline, 0.f, tEnd);

    // Map effects (IsLedgerMapRow): their names and icons.
    auto MapName = [](int key) -> const char* {
        switch (key)
        {
        case kLedgerRowCatapult: return "Catapult";
        case kLedgerRowCoral:    return "Spiked Coral";
        case kLedgerRowAcid:     return "Acid Trap";
        case kLedgerRowFlux:     return "Minion Apocalypse";
        case kLedgerRowMiasma:   return "Miasma";
        case kLedgerRowWater:    return "Healing water";
        default:                 return "Map";
        }
    };
    auto MapIcon = [&](int key) -> ImTextureID {
        switch (key)
        {
        case kLedgerRowCatapult: return LoadFlagIcon(dev, CatapultDangerZone::kHitIconFile);
        case kLedgerRowCoral:    return LoadSkillIcon(this, dev, DamageAttribution::kSpikedCoral, m_skillIconIndex, m_skillIconCache);
        case kLedgerRowAcid:     return LoadSkillIcon(this, dev, DamageAttribution::kAcidTrap, m_skillIconIndex, m_skillIconCache);
        case kLedgerRowFlux:     return LoadSkillIcon(this, dev, DamageAttribution::kMinionApocalypse, m_skillIconIndex, m_skillIconCache);
        case kLedgerRowMiasma:   return LoadSkillIcon(this, dev, DamageAttribution::kMiasma, m_skillIconIndex, m_skillIconCache);
        case kLedgerRowWater:    return LoadSkillIcon(this, dev, 290, m_skillIconIndex, m_skillIconCache);   // Mending, as the effect bar shows it
        default:                 return nullptr;
        }
    };

    // --- Who is who ---
    struct Who { int id; std::string name; int team; int prof, sec; };
    std::vector<Who> players;
    std::unordered_map<int, size_t> whoIx;
    for (int pid : m_playerIds)
    {
        auto it = m_replayCtx.agents.find(pid);
        if (it == m_replayCtx.agents.end() || it->second.type != AgentType::Player) continue;
        const auto& a = it->second;
        whoIx[pid] = players.size();
        players.push_back({ pid, a.partyBarLabel.empty() ? a.playerName : a.partyBarLabel, a.teamId, a.primaryProf, a.secondaryProf });
    }
    auto TeamOf = [&](int agent) -> int {
        auto it = m_replayCtx.agents.find(agent);
        return it == m_replayCtx.agents.end() ? 0 : it->second.teamId;
    };
    auto NameOf = [&](int agent) -> std::string {
        auto it = m_replayCtx.agents.find(agent);
        if (it == m_replayCtx.agents.end()) return agent ? "Agent " + std::to_string(agent) : std::string("The map");
        return it->second.partyBarLabel.empty() ? GetAgentLabel(it->second) : it->second.partyBarLabel;
    };
    auto ProfOf = [&](int agent) -> int {
        auto it = m_replayCtx.agents.find(agent);
        return it == m_replayCtx.agents.end() ? 0 : it->second.primaryProf;
    };
    auto GuildLabel = [&](int team) -> std::string {
        const std::string& tag = Team::IsBlue(team) ? m_folderTag1 : m_folderTag2;
        if (const GuildMeta* g = FindGuildByTagStatic(m_matchMeta, tag)) return g->name + " [" + g->tag + "]";
        return Team::IsBlue(team) ? "Blue team" : "Red team";
    };
    auto TagOf = [&](int team) -> std::string {
        const std::string& tag = Team::IsBlue(team) ? m_folderTag1 : m_folderTag2;
        return tag.empty() ? std::string(Team::IsBlue(team) ? "Blue" : "Red") : tag;
    };

    // --- The counted stretch ---
    float r0 = 0.f, r1 = tEnd;
    if (sScope == kPlayhead) r1 = tNow;
    else if (sScope == kLast60) { r1 = tNow; r0 = std::max(0.f, tNow - 60.f); }
    else if (sScope == kSelection && sHasSel) { r0 = sSelA; r1 = sSelB; }
    auto In = [&](float t) { return t >= r0 && t <= r1; };

    // --- Totals per player for every tab ---
    struct Totals { long long v[kMetricCount] = {}; int intAtt = 0, intFake = 0, recvAtt = 0, recvFake = 0; };
    std::unordered_map<int, Totals> tot;
    for (const LedgerLine& ln : m_ledger)
    {
        if (ln.t < r0) continue;
        if (ln.t > r1) break;
        if (int m = LineMetricGiver(ln.kind); m >= 0 && whoIx.count(ln.src)) tot[ln.src].v[m] += ln.amount;
        if (int m = LineMetricTaker(ln.kind); m >= 0 && whoIx.count(ln.dst)) tot[ln.dst].v[m] += ln.amount;
    }
    for (const LedgerInterrupt& li : m_ledgerInts)
    {
        if (!In(li.t)) continue;
        Totals& s = tot[li.src];
        Totals& d = tot[li.dst];
        s.intAtt++; d.recvAtt++;
        if (li.outcome == LedgerInterrupt::Landed) { s.v[kInts]++; d.v[kIntsRecv]++; }
        if (li.outcome == LedgerInterrupt::Faked)  { s.intFake++; d.recvFake++; }
    }
    const bool intsTab = sMetric == kInts || sMetric == kIntsRecv;
    auto MetricCol = [&](int m) {
        return m == kDealt || m == kTaken ? kDamage : m == kHealed || m == kReceived ? kHealing : m == kPrevented ? kPrevent : kPurple;
    };

    // The client's digits for each tab: damage red, heal blue (prevented too), energy purple for interrupts.
    EnsureBitmapFontsLoaded();
    auto GlyphFont = [&](int m) -> const BitmapFont& {
        return m == kDealt || m == kTaken ? m_damageBitmapFont : m == kInts || m == kIntsRecv ? m_energyBitmapFont : m_healBitmapFont;
    };

    // ===================== Tabs =====================
    // On a band of their own, each with the Match Library's icon for the same figure.
    const ImTextureID tabIcon[kMetricCount] = {
        LoadFlagIcon(dev, "..\\Match library\\Dmg_Out-removebg-preview.png"),
        LoadFlagIcon(dev, "..\\Match library\\Dmg_In-removebg-preview.png"),
        LoadFlagIcon(dev, "..\\Match library\\Heal_Out-removebg-preview.png"),
        LoadFlagIcon(dev, "..\\Match library\\Heal_In-removebg-preview.png"),
        LoadSkillIcon(this, dev, 245, m_skillIconIndex, m_skillIconCache),     // Protective Spirit
        LoadFlagIcon(dev, "..\\Match library\\Rupts_Out.png"),
        LoadFlagIcon(dev, "..\\Match library\\Rupt_In-removebg-preview.png"),
    };
    for (int m = 0; m < kMetricCount; ++m)
    {
        ImGui::PushID(m);
        if (IconTab(kMetricName[m], tabIcon[m], sMetric == m)) { sMetric = m; sOpenRows.clear(); }
        ImGui::PopID();
        ImGui::SameLine(0.f, m == kDealt + 1 || m == kPrevented ? 12.f : 4.f);
    }
    {
        const float w = ImGui::CalcTextSize("Board").x + ImGui::CalcTextSize("Team matrix").x + 50.f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.f, ImGui::GetWindowContentRegionMax().x - w));
        if (IconTab("Board", nullptr, !sMatrix)) sMatrix = false;
        ImGui::SameLine(0.f, 4.f);
        if (IconTab("Team matrix", nullptr, sMatrix)) sMatrix = true;
    }
    ImGui::Dummy(ImVec2(0.f, 4.f));
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x, p.y - 1.f), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 1.f), kRule);
        ImGui::Dummy(ImVec2(0.f, 4.f));
    }

    // ===================== Time strip =====================
    {
        // The stretch to count, first: it decides every number below.
        {
            std::vector<std::string> labels;
            std::vector<bool> enabled;
            for (int s = 0; s < 4; ++s)
            {
                labels.push_back(s == kSelection && sHasSel ? "Selection " + FmtT(sSelA) + "-" + FmtT(sSelB) : std::string(kScopeName[s]));
                enabled.push_back(s != kSelection || sHasSel);
            }
            const int c = Segmented("##scope", labels, sScope, &enabled);
            if (c == kSelection && !sHasSel) sSelHintUntil = ImGui::GetTime() + 3.0;
            else if (c >= 0) sScope = c;
        }
        ImGui::SameLine(0.f, 14.f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(V4(kSoft), "Playback %s   ·   counting %s-%s", FmtT(tNow).c_str(), FmtT(r0).c_str(), FmtT(r1).c_str());
        if (ImGui::GetTime() < sSelHintUntil)
        {
            ImGui::SameLine(0.f, 12.f);
            ImGui::TextColored(V4(kGold), "Drag on the strip to make a selection.");
        }
        ImGui::SameLine();
        Help("Both teams over the match, six seconds per bar: Blue above the line, Red below. Drag on the "
             "strip to count only that stretch; double-click it to go back to the full match. A triangle is a "
             "spike (three or more enemy damage skills on one player within 1.5 s), a cross a death.");

        const float W = ImGui::GetContentRegionAvail().x, H = 76.f, padL = 40.f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##strip", ImVec2(W, H));
        const bool hov = ImGui::IsItemHovered();
        auto xOf = [&](float t) { return p.x + padL + t / tEnd * (W - padL - 4.f); };
        auto tOf = [&](float x) { return std::clamp((x - p.x - padL) / (W - padL - 4.f) * tEnd, 0.f, tEnd); };
        if (ImGui::IsItemActivated()) sDragFrom = tOf(io.MousePos.x);
        if (ImGui::IsItemActive() && sDragFrom >= 0.f)
        {
            const float a = std::min(sDragFrom, tOf(io.MousePos.x)), b = std::max(sDragFrom, tOf(io.MousePos.x));
            if (b - a > 2.f) { sSelA = a; sSelB = b; sHasSel = true; sScope = kSelection; }
        }
        if (ImGui::IsItemDeactivated()) sDragFrom = -1.f;
        if (hov && ImGui::IsMouseDoubleClicked(0)) { sHasSel = false; sScope = kFull; }

        dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), IM_COL32(0, 0, 0, 110), 3.f);
        const float mid = p.y + H * 0.5f;
        constexpr float kBin = 6.f;
        const int n = std::max(1, (int)std::ceil(tEnd / kBin));
        std::vector<float> top(n), bot(n);
        const bool byTaker = sMetric == kTaken || sMetric == kReceived || sMetric == kIntsRecv;
        auto Put = [&](float t, int agent, float v) {
            const int team = TeamOf(agent);
            if (!Team::IsSide(team)) return;
            const int b = std::clamp((int)(t / kBin), 0, n - 1);
            (Team::IsBlue(team) ? top : bot)[b] += v;
        };
        if (intsTab)
        {
            for (const LedgerInterrupt& li : m_ledgerInts)
                if (li.outcome == LedgerInterrupt::Landed) Put(li.t, byTaker ? li.dst : li.src, 1.f);
        }
        else
            for (const LedgerLine& ln : m_ledger)
            {
                const int m = byTaker ? LineMetricTaker(ln.kind) : LineMetricGiver(ln.kind);
                if (m == sMetric) Put(ln.t, byTaker ? ln.dst : ln.src, (float)ln.amount);
            }
        float mx = 1.f;
        for (int i = 0; i < n; ++i) mx = std::max({ mx, top[i], bot[i] });
        for (float t = 120.f; t < tEnd; t += 120.f)
        {
            const float x = xOf(t);
            dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + H), IM_COL32(255, 255, 255, 14));
            dl->AddText(ImVec2(x + 3.f, p.y + 1.f), IM_COL32(150, 156, 165, 160), FmtT(t).c_str());
        }
        const float bw = (W - padL - 4.f) / n;
        for (int i = 0; i < n; ++i)
        {
            const float x = p.x + padL + i * bw;
            const float a = top[i] / mx * (H * 0.5f - 4.f), b = bot[i] / mx * (H * 0.5f - 4.f);
            if (a > 0.f) dl->AddRectFilled(ImVec2(x, mid - a), ImVec2(x + std::max(1.f, bw - 0.6f), mid), kBlueBar);
            if (b > 0.f) dl->AddRectFilled(ImVec2(x, mid), ImVec2(x + std::max(1.f, bw - 0.6f), mid + b), kRedBar);
        }
        dl->AddLine(ImVec2(p.x + padL, mid), ImVec2(p.x + W, mid), IM_COL32(0, 0, 0, 160));
        dl->AddText(ImVec2(p.x + 5.f, mid - lineH - 6.f), kBlueText, TagOf(Team::Blue).c_str());
        dl->AddText(ImVec2(p.x + 5.f, mid + 6.f), kRedText, TagOf(Team::Red).c_str());
        if (sMetric == kDealt || sMetric == kTaken)
            for (const SpikeInfo& s : m_spikes)
            {
                // On the side that took it.
                const bool blueHit = Team::IsBlue(TeamOf(s.target));
                const float x = xOf(s.firstHit);
                if (blueHit) dl->AddTriangleFilled(ImVec2(x - 3.f, p.y + 3.f), ImVec2(x + 3.f, p.y + 3.f), ImVec2(x, p.y + 8.f), IM_COL32(220, 224, 230, 200));
                else         dl->AddTriangleFilled(ImVec2(x - 3.f, p.y + H - 3.f), ImVec2(x + 3.f, p.y + H - 3.f), ImVec2(x, p.y + H - 8.f), IM_COL32(220, 224, 230, 200));
            }
        for (const auto& [aid, times] : m_moraleDeaths)
        {
            if (!whoIx.count(aid)) continue;
            const bool blue = Team::IsBlue(TeamOf(aid));
            for (float t : times)
            {
                const float x = xOf(t), y = blue ? mid - 7.f : mid + 7.f;
                dl->AddLine(ImVec2(x - 3.f, y - 3.f), ImVec2(x + 3.f, y + 3.f), IM_COL32(255, 255, 255, 220), 1.5f);
                dl->AddLine(ImVec2(x + 3.f, y - 3.f), ImVec2(x - 3.f, y + 3.f), IM_COL32(255, 255, 255, 220), 1.5f);
            }
        }
        if (sScope != kFull)
        {
            const float x0 = xOf(r0), x1 = xOf(r1);
            dl->AddRectFilled(ImVec2(p.x + padL, p.y), ImVec2(x0, p.y + H), IM_COL32(0, 0, 0, 140));
            dl->AddRectFilled(ImVec2(x1, p.y), ImVec2(p.x + W, p.y + H), IM_COL32(0, 0, 0, 140));
            dl->AddRect(ImVec2(x0, p.y), ImVec2(x1, p.y + H), IM_COL32(225, 190, 80, 0xB0));
        }
        dl->AddLine(ImVec2(xOf(tNow), p.y), ImVec2(xOf(tNow), p.y + H), IM_COL32(225, 190, 80, 0xD0), 1.5f);
        if (hov && !ImGui::IsItemActive())
        {
            const int i = std::clamp((int)(tOf(io.MousePos.x) / kBin), 0, n - 1);
            ImGui::BeginTooltip();
            ImGui::Text("%s-%s", FmtT(i * kBin).c_str(), FmtT((i + 1) * kBin).c_str());
            ImGui::TextColored(V4(kBlueText), "%s  %s", TagOf(Team::Blue).c_str(), FmtN((long long)top[i]).c_str());
            ImGui::TextColored(V4(kRedText), "%s  %s", TagOf(Team::Red).c_str(), FmtN((long long)bot[i]).c_str());
            ImGui::EndTooltip();
        }
        ImGui::Dummy(ImVec2(0.f, 4.f));
    }

    ImGui::BeginChild("##ledger_body", ImVec2(0, 0), false);
    dl = ImGui::GetWindowDrawList();

    if (!sMatrix)
    {
        // ===================== Board =====================
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(V4(kMuted), "Rows");
        ImGui::SameLine();
        if (const int c = Segmented("##rows", { "Contributors only", "All players" }, sShowAll ? 1 : 0); c >= 0) sShowAll = c == 1;
        ImGui::Dummy(ImVec2(0.f, 2.f));

        const float colGap = 18.f;
        const float colW = (ImGui::GetContentRegionAvail().x - colGap) * 0.5f - 20.f;   // inside the box's 10 px margins
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        float colBottom[2] = { origin.y, origin.y };
        long long boardMax = 1;
        for (const Who& w : players) boardMax = std::max(boardMax, tot[w.id].v[sMetric]);

        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        for (int side = 0; side < 2; ++side)
        {
            const uint8_t team = Team::FromIndex01(side);
            std::vector<const Who*> rows;
            for (const Who& w : players) if (w.team == team) rows.push_back(&w);
            std::stable_sort(rows.begin(), rows.end(), [&](const Who* a, const Who* b) { return tot[a->id].v[sMetric] > tot[b->id].v[sMetric]; });
            long long teamTotal = 0;
            for (const Who* w : rows) teamTotal += tot[w->id].v[sMetric];

            const float x = origin.x + side * (colW + 20.f + colGap) + 10.f;
            float y = origin.y + 10.f;
            // Team heading: the guild in team colour over a team rule, its total at the right.
            const float totH = 19.f;
            dl->AddText(ImVec2(x, y + (totH - lineH) * 0.5f), TeamText(team), GuildLabel(team).c_str());
            GlyphNumber(dl, GlyphFont(sMetric), teamTotal, x + colW - 6.f, y, totH, true);
            y += totH + 4.f;
            dl->AddLine(ImVec2(x, y), ImVec2(x + colW, y), WithAlpha(TeamText(team), 0x55));
            y += 4.f;

            int hidden = 0;
            std::vector<int> hiddenProfs;
            const float rowH = 40.f, valH = 15.f;
            for (const Who* w : rows)
            {
                const Totals& t = tot[w->id];
                const long long v = t.v[sMetric];
                const bool any = v > 0 || (sMetric == kInts && t.intAtt) || (sMetric == kIntsRecv && t.recvAtt);
                if (!any && !sShowAll) { ++hidden; hiddenProfs.push_back(w->prof); continue; }
                ImGui::SetCursorScreenPos(ImVec2(x, y));
                ImGui::PushID(w->id);
                ImGui::InvisibleButton("##row", ImVec2(colW, rowH));
                const bool hov = ImGui::IsItemHovered(), sel = sSelPlayer == w->id;
                if (ImGui::IsItemClicked()) { sSelPlayer = w->id; sOpenRows.clear(); }
                ImGui::PopID();
                if (sel)
                {
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + colW, y + rowH), kSelFill, 3.f);
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 2.5f, y + rowH), kGold);
                }
                else if (hov) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + colW, y + rowH), kHover, 3.f);
                if (ImTextureID pt = w->prof ? LoadProfIcon(dev, w->prof) : nullptr)
                    dl->AddImage(pt, ImVec2(x + 7.f, y + 5.f), ImVec2(x + 25.f, y + 23.f));
                dl->AddText(ImVec2(x + 31.f, y + 4.f), sel ? kGold : kNameText, w->name.c_str());

                // The value, and a note under it.
                std::string note;
                if (sMetric == kInts)          note = t.intAtt ? Pct(v, t.intAtt) + " of " + std::to_string(t.intAtt) : "no attempts";
                else if (sMetric == kIntsRecv) note = t.recvAtt ? std::to_string(t.recvFake) + " faked of " + std::to_string(t.recvAtt) : "no attempts";
                else                           note = teamTotal ? Pct(v, teamTotal) + " of team" : "";
                GlyphNumber(dl, GlyphFont(sMetric), v, x + colW - 6.f, y + 3.f, valH, true);
                RText(dl, x + colW - 6.f, y + 5.f + valH, kSoft, note);
                const float barR = x + colW - 6.f - std::max(ImGui::CalcTextSize(note.c_str()).x, 84.f) - 12.f;

                // Top three skills beside the name, then the bar under it.
                if (!intsTab)
                {
                    std::unordered_map<int, long long> bySkill;
                    const bool taker = !GivingMetric(sMetric);
                    for (const LedgerLine& ln : m_ledger)
                    {
                        if (ln.t < r0) continue;
                        if (ln.t > r1) break;
                        const int m = taker ? LineMetricTaker(ln.kind) : LineMetricGiver(ln.kind);
                        if (m == sMetric && (taker ? ln.dst : ln.src) == w->id && ln.skillId > 0) bySkill[ln.skillId] += ln.amount;
                    }
                    std::vector<std::pair<long long, int>> topSk;
                    for (auto& [k, a] : bySkill) topSk.push_back({ a, k });
                    std::sort(topSk.rbegin(), topSk.rend());
                    const float ix = barR - 3 * 23.f;
                    for (size_t k = 0; k < topSk.size() && k < 3; ++k)
                        if (ImTextureID st = LoadSkillIcon(this, dev, topSk[k].second, m_skillIconIndex, m_skillIconCache))
                            dl->AddImageRounded(st, ImVec2(ix + k * 23.f, y + 2.f), ImVec2(ix + k * 23.f + 21.f, y + 23.f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 2.f);
                }
                Bar(dl, ImVec2(x + 31.f, y + rowH - 13.f), ImVec2(barR, y + rowH - 5.f), (float)v / (float)boardMax, MetricCol(sMetric));
                dl->AddLine(ImVec2(x, y + rowH), ImVec2(x + colW, y + rowH), kHairline);
                y += rowH + 1.f;
            }
            if (hidden)
            {
                char b[96];
                snprintf(b, sizeof b, "%d player%s with no %s", hidden, hidden == 1 ? "" : "s", intsTab ? "attempts" : "share");
                float hx = x + 7.f;
                for (int pr : hiddenProfs)
                    if (ImTextureID pt = pr ? LoadProfIcon(dev, pr) : nullptr)
                    { dl->AddImage(pt, ImVec2(hx, y + 5.f), ImVec2(hx + 14.f, y + 19.f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 150)); hx += 15.f; }
                dl->AddText(ImVec2(hx + 6.f, y + 4.f), kMuted, b);
                ImGui::SetCursorScreenPos(ImVec2(hx + 6.f + ImGui::CalcTextSize(b).x + 4.f, y + 1.f));
                ImGui::PushID(side);
                if (TextTab("Show", false)) sShowAll = true;
                ImGui::PopID();
                y += 26.f;
            }
            colBottom[side] = y + 6.f;
        }
        for (int side = 0; side < 2; ++side)
        {
            dl->ChannelsSetCurrent(0);
            const float bx = origin.x + side * (colW + 20.f + colGap);
            const ImVec2 a(bx, origin.y), b(bx + colW + 20.f, colBottom[side]);
            dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 6), 3.f);
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 15), 3.f);
            dl->AddRectFilled(a, ImVec2(b.x, a.y + 2.f), WithAlpha(TeamText(Team::FromIndex01(side)), 0x8C), 2.f);
        }
        dl->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(origin.x, std::max(colBottom[0], colBottom[1]) + 12.f));
        ImGui::Dummy(ImVec2(0.f, 0.f));

        // ===================== Map effects =====================
        // A strip under both teams: what the map itself did to each team in this tab (damage taken,
        // healing received) - never any player's. Other tabs show none.
        if (sMetric == kTaken || sMetric == kReceived)
        {
            std::map<int, std::array<long long, 2>> fx;      // map row -> per team (blue, red)
            for (const LedgerLine& ln : m_ledger)
            {
                if (ln.t < r0) continue;
                if (ln.t > r1) break;
                if (!IsLedgerMapRow(ln.rowKey) || LineMetricTaker(ln.kind) != sMetric || !whoIx.count(ln.dst)) continue;
                const int team = TeamOf(ln.dst);
                if (Team::IsBlue(team)) fx[ln.rowKey][0] += ln.amount;
                else if (Team::IsRed(team)) fx[ln.rowKey][1] += ln.amount;
            }
            if (!fx.empty())
            {
                const float fullW = (colW + 20.f) * 2.f + colGap, rowH = 24.f;
                const ImVec2 a0 = ImGui::GetCursorScreenPos();
                const float h = 10.f + lineH + 6.f + rowH * (float)fx.size() + 6.f;
                dl->AddRectFilled(a0, ImVec2(a0.x + fullW, a0.y + h), IM_COL32(255, 255, 255, 6), 3.f);
                dl->AddRect(a0, ImVec2(a0.x + fullW, a0.y + h), IM_COL32(255, 255, 255, 15), 3.f);
                const float smallFs = ImGui::GetFontSize() * 0.85f;
                dl->AddText(ImGui::GetFont(), smallFs, ImVec2(a0.x + 10.f, a0.y + 8.f), kGold, "MAP EFFECTS");
                float y = a0.y + 10.f + lineH + 6.f;
                for (const auto& [key, v] : fx)
                {
                    float x = a0.x + 10.f;
                    if (ImTextureID ic = MapIcon(key))
                        dl->AddImageRounded(ic, ImVec2(x, y + 1.f), ImVec2(x + 20.f, y + 21.f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 2.f);
                    x += 28.f;
                    dl->AddText(ImVec2(x, y + 3.f), kNameText, MapName(key));
                    x = a0.x + std::max(190.f, ImGui::CalcTextSize(MapName(key)).x + 50.f);
                    for (int side = 0; side < 2; ++side)
                    {
                        const uint8_t team = Team::FromIndex01(side);
                        const std::string tag = TagOf(team) + " ";
                        dl->AddText(ImVec2(x, y + 3.f), TeamText(team), tag.c_str());
                        x += ImGui::CalcTextSize(tag.c_str()).x;
                        const std::string val = FmtN(v[side]);
                        dl->AddText(ImVec2(x, y + 3.f), v[side] ? MetricCol(sMetric) : kMuted, val.c_str());
                        x += ImGui::CalcTextSize(val.c_str()).x;
                        if (side == 0) { dl->AddText(ImVec2(x + 6.f, y + 3.f), kMuted, "\xC2\xB7"); x += 20.f; }
                    }
                    y += rowH;
                }
                ImGui::Dummy(ImVec2(fullW, h + 12.f));
            }
        }

        // ===================== Detail =====================
        if (sSelPlayer < 0 || !whoIx.count(sSelPlayer))
            for (const Who& w : players) if (tot[w.id].v[sMetric] > 0) { sSelPlayer = w.id; break; }
        if (sSelPlayer >= 0 && whoIx.count(sSelPlayer))
        {
            const Who& me = players[whoIx[sSelPlayer]];
            const Totals& mt = tot[me.id];
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), kRule);
                ImGui::Dummy(ImVec2(0.f, 8.f));
            }
            // Header: the professions, the name in team colour, the tab, the stretch.
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImTextureID pt = me.prof ? LoadProfIcon(dev, me.prof) : nullptr) dl->AddImage(pt, p, ImVec2(p.x + 26.f, p.y + 26.f));
                if (ImTextureID st = me.sec ? LoadProfIcon(dev, me.sec) : nullptr)
                    dl->AddImage(st, ImVec2(p.x + 18.f, p.y + 12.f), ImVec2(p.x + 34.f, p.y + 28.f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 210));
                // A baked bold rung (crisp, not a scaled font), one step above the body text.
                // The window's own font: a font from the main window's atlas does not draw here.
                ImFont* nf = ImGui::GetFont();
                const float big = ImGui::GetFontSize();
                dl->AddText(nf, big, ImVec2(p.x + 42.f, p.y), TeamText(me.team), me.name.c_str());
                const float nw = nf->CalcTextSizeA(big, FLT_MAX, 0.f, me.name.c_str()).x;
                dl->AddText(ImVec2(p.x + 42.f + nw + 12.f, p.y + (big - lineH)), kGold, kMetricName[sMetric]);
                const std::string sub = std::string(kScopeName[sScope]) + " " + FmtT(r0) + "-" + FmtT(r1) + "   " + GuildLabel(me.team);
                dl->AddText(ImVec2(p.x + 42.f, p.y + big + 3.f), kSoft, sub.c_str());
                ImGui::Dummy(ImVec2(0.f, big + lineH + 12.f));
            }

            // Headline: the tab's number large, what it means beside it, on one line.
            std::vector<int> rxs, afters;
            for (const LedgerInterrupt& li : m_ledgerInts)
            {
                if (!In(li.t) || li.outcome != LedgerInterrupt::Landed) continue;
                if (li.src == me.id && li.reflexMs >= 0) rxs.push_back(li.reflexMs);
                if (li.dst == me.id && li.afterMs >= 0) afters.push_back(li.afterMs);
            }
            // Headline figures (KPIs) in the client's digits, each with what it is under it.
            std::string what = kMetricName[sMetric];
            what[0] = (char)std::tolower((unsigned char)what[0]);
            auto PctOf = [](long long a, long long b) { return b ? (long long)std::lround(100.0 * a / b) : 0LL; };
            const BitmapFont& kf = GlyphFont(sMetric);
            const ImVec2 kpiAt = ImGui::GetCursorScreenPos();
            const float kpiW = ImGui::GetContentRegionAvail().x;
            if (sMetric == kInts)
            {
                const int rx = Median(rxs);
                KpiRow(kf, { { PctOf(mt.v[kInts], mt.intAtt), "%", "accuracy", mt.intAtt == 0 },
                             { mt.v[kInts], "", "landed" }, { mt.intAtt, "", "attempts" }, { mt.intFake, "", "faked by the target" },
                             { rx, " ms", "median reaction", rx < 0 } });
            }
            else if (sMetric == kIntsRecv)
            {
                const int af = Median(afters);
                KpiRow(kf, { { mt.v[kIntsRecv], "", "interrupted" }, { mt.recvAtt, "", "attempts on him" },
                             { mt.recvFake, "", "faked away" }, { af, " ms", "into the cast, median", af < 0 } });
            }
            else
            {
                long long teamTotal = 0;
                int rank = 1, count = 0;
                for (const Who& w : players)
                    if (w.team == me.team) { teamTotal += tot[w.id].v[sMetric]; ++count; if (tot[w.id].v[sMetric] > mt.v[sMetric]) ++rank; }
                KpiRow(kf, { { mt.v[sMetric], "", what }, { PctOf(mt.v[sMetric], teamTotal), "%", "of the team" },
                             { rank, " of " + std::to_string(count), "rank in the team" } });
            }
            if (intsTab)
            {
                const ImVec2 after = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(kpiAt.x + kpiW - 26.f, kpiAt.y + 6.f));
                Help("Accuracy: the attempts that stopped a cast. An attempt is a completed cast of an interrupting "
                     "skill on a foe; faked means the foe stopped his own cast in the quarter second before it would "
                     "have landed. Reaction time: from the start of the cast that was stopped to the start of the interrupt.");
                ImGui::SetCursorScreenPos(after);
                ImGui::Dummy(ImVec2(0.f, 0.f));
            }

            // Every tab for this player in one strip; a click switches the tab.
            ImGui::Dummy(ImVec2(0.f, 6.f));
            {
                std::vector<long long> values;
                std::vector<const BitmapFont*> fonts;
                for (int m = 0; m < kMetricCount; ++m) { values.push_back(mt.v[m]); fonts.push_back(&GlyphFont(m)); }
                if (const int c = StatStrip(kMetricName, values, fonts, sMetric); c >= 0) { sMetric = c; sOpenRows.clear(); }
            }
            ImGui::Dummy(ImVec2(0.f, 10.f));

            // --- The breakdown table ---
            const float W = ImGui::GetContentRegionAvail().x;
            auto RowFrame = [&](int key, float rh, bool& open) -> ImVec2 {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushID(key);
                ImGui::InvisibleButton("##r", ImVec2(W, rh));
                open = sOpenRows.count(key) != 0;
                if (ImGui::IsItemClicked()) { if (open) sOpenRows.erase(key); else sOpenRows.insert(key); open = !open; }
                if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + W, p.y + rh), kHover, 3.f);
                ImGui::PopID();
                // The game-style caret: a small triangle, like the Morale and Piano Roll headers.
                const float cx = p.x + 6.f, cy = p.y + rh * 0.5f;
                if (open) dl->AddTriangleFilled(ImVec2(cx - 4.f, cy - 2.f), ImVec2(cx + 4.f, cy - 2.f), ImVec2(cx, cy + 3.f), kMuted);
                else      dl->AddTriangleFilled(ImVec2(cx - 2.f, cy - 4.f), ImVec2(cx - 2.f, cy + 4.f), ImVec2(cx + 3.f, cy), kMuted);
                dl->AddLine(ImVec2(p.x, p.y + rh), ImVec2(p.x + W, p.y + rh), kHairline);
                return p;
            };
            auto SubLineHead = [&](int id) -> ImVec2 {
                const ImVec2 q = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(W, 20.f));
                // A Guild Lord has no profession: his own lord art, in his team's colour.
                const bool lord = id >= 0 && (id == m_lordDmg[0].lordAgentId || id == m_lordDmg[1].lordAgentId || NameOf(id) == "Guild Lord");
                ImTextureID pt = lord ? LoadFlagIcon(dev, Team::IsRed(TeamOf(id)) ? "redguildlord.png" : "blueguildlord.png")
                               : ProfOf(id) ? LoadProfIcon(dev, ProfOf(id)) : nullptr;
                if (pt) dl->AddImage(pt, ImVec2(q.x + 44.f, q.y + 2.f), ImVec2(q.x + 60.f, q.y + 18.f));
                dl->AddText(ImVec2(q.x + 66.f, q.y + 2.f), TeamText(TeamOf(id)), NameOf(id).c_str());
                return q;
            };
            // iconSkill: the skill whose icon a special row shows (the Animate skill, the cause of a
            // condition), 0 for its own.
            auto SkillCell = [&](ImVec2 p, int key, const std::string& name, int iconSkill = 0) {
                ImTextureID ic = IsLedgerMapRow(key) ? MapIcon(key)
                               : iconSkill > 0 ? LoadSkillIcon(this, dev, iconSkill, m_skillIconIndex, m_skillIconCache)
                               : key > 0 ? LoadSkillIcon(this, dev, key, m_skillIconIndex, m_skillIconCache)
                               : key == -1 || key == kLedgerRowMinions ? LoadFlagIcon(dev, "kill.png")
                               : key == -2 ? LoadProfIcon(dev, 3) : nullptr;   // Divine Favor bonus: the Monk icon
                if (ic) dl->AddImageRounded(ic, ImVec2(p.x + 16.f, p.y + 2.f), ImVec2(p.x + 38.f, p.y + 24.f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 2.f);
                else    dl->AddRectFilled(ImVec2(p.x + 16.f, p.y + 2.f), ImVec2(p.x + 38.f, p.y + 24.f), IM_COL32(40, 40, 40, 255), 2.f);
                dl->AddText(ImVec2(p.x + 46.f, p.y + 5.f), kNameText, name.c_str());
            };

            if (!intsTab)
            {
                const bool taker = !GivingMetric(sMetric);
                // Row key: the skill, or -1 weapon hits, -2 the Divine Favor bonus, -3 nothing named,
                // or the line's own row (minions, degeneration: LedgerLine::rowKey).
                struct Row
                {
                    int key = 0, icon = 0; long long amount = 0; int casts = 0, intr = 0, canc = 0;
                    bool est = false; int capped = 0, blocked = 0;   // prevention estimates
                    std::map<int, long long> per; std::map<int, int> castsOn, intrOn, cancOn;
                };
                std::map<int, Row> rows;
                long long sum = 0;
                for (const LedgerLine& ln : m_ledger)
                {
                    if (ln.t < r0) continue;
                    if (ln.t > r1) break;
                    const int m = taker ? LineMetricTaker(ln.kind) : LineMetricGiver(ln.kind);
                    if (m != sMetric || (taker ? ln.dst : ln.src) != me.id) continue;
                    const int key = ln.rowKey ? ln.rowKey : ln.kind == LedgerKind::DivineFavor ? -2 : ln.skillId > 0 ? ln.skillId : ln.weaponHit ? -1 : -3;
                    Row& r = rows[key];
                    if (ln.rowKey && ln.skillId > 0) r.icon = ln.skillId;
                    if (ln.estimated) r.est = true;
                    if (ln.preventGroup == 1 + (uint8_t)DamageAttribution::PreventGroup::Cap)   r.capped += ln.count;
                    if (ln.preventGroup == 1 + (uint8_t)DamageAttribution::PreventGroup::Block) r.blocked += ln.count;
                    r.key = key; r.amount += ln.amount; r.per[taker ? ln.src : ln.dst] += ln.amount; sum += ln.amount;
                }
                if (!taker)
                    if (auto ait = m_replayCtx.agents.find(me.id); ait != m_replayCtx.agents.end())
                        for (const SkillUseEvent& su : ait->second.skillUseHistory)
                        {
                            if (!In(su.startTime)) continue;
                            auto rit = rows.find(m_skillView.ResolvePvpSkillId(su.skillId));
                            if (rit == rows.end()) continue;
                            Row& r = rit->second;
                            const int tg = su.targetId >= 0 ? su.targetId : me.id;
                            r.casts++; r.castsOn[tg]++;
                            if (su.wasInterrupted)    { r.intr++; r.intrOn[tg]++; }
                            else if (su.wasCancelled) { r.canc++; r.cancOn[tg]++; }
                        }
                std::vector<Row*> order;
                for (auto& [k, r] : rows) order.push_back(&r);
                std::sort(order.begin(), order.end(), [](const Row* a, const Row* b) { return a->amount > b->amount; });

                const float xCasts = W * 0.46f, xInt = W * 0.555f, xCanc = W * 0.645f, xAmt = W * 0.75f, xBar0 = W * 0.78f, xPct = W - 4.f;
                Section(taker ? "BY SKILL" : "SKILLS", taker ? "click a row for who it came from" : "click a row for who it was cast on");
                const ImVec2 h = ImGui::GetCursorScreenPos();
                dl->AddText(ImVec2(h.x + 46.f, h.y), kMuted, "Skill");
                if (!taker)
                {
                    RText(dl, h.x + xCasts, h.y, kMuted, "Casts");
                    RText(dl, h.x + xInt, h.y, kMuted, "Interrupted");
                    RText(dl, h.x + xCanc, h.y, kMuted, "Cancelled");
                }
                RText(dl, h.x + xAmt, h.y, kMuted, "Amount");
                dl->AddText(ImVec2(h.x + xBar0, h.y), kMuted, "Share");
                ImGui::Dummy(ImVec2(W, lineH + 4.f));
                if (order.empty()) ImGui::TextColored(V4(kMuted), "Nothing in this stretch.");
                for (Row* r : order)
                {
                    bool open = false;
                    const ImVec2 p = RowFrame(r->key, 26.f, open);
                    std::string nm;
                    if (r->key == 657) nm = "Life Draining";      // the vampiric weapon's hidden skill
                    else if (r->key > 0) nm = GetSkillDisplayName(r->key);
                    else if (IsLedgerMapRow(r->key)) nm = MapName(r->key);
                    else if (IsLedgerDegenRow(r->key))
                    {
                        // "Poison from Apply Poison", or the condition alone when its cause is unknown.
                        const int cond = LedgerDegenCondition(r->key), cause = LedgerDegenCause(r->key);
                        nm = ConditionName(cond);
                        if (cause > 0) nm += " from " + GetSkillDisplayName(cause);
                    }
                    else nm = r->key == -1 ? "Weapon hits" : r->key == -2 ? "Divine Favor bonus" : r->key == kLedgerRowMinions ? "Minions"
                            : r->key == kLedgerRowDegenUnknown ? "Degeneration (source unknown)" : "No skill named";
                    int icon = r->icon;
                    if (IsLedgerDegenRow(r->key) && m_skillIconIndex.count(LedgerDegenCondition(r->key))) icon = LedgerDegenCondition(r->key);
                    SkillCell(p, r->key, nm, icon);
                    if (r->capped || r->blocked)
                    {
                        // An estimate: what the capped hits and stopped attacks would have dealt.
                        std::string note = r->capped ? FmtN(r->capped) + " capped" : std::string();
                        if (r->blocked) note += (note.empty() ? "" : ", ") + FmtN(r->blocked) + " blocked";
                        dl->AddText(ImVec2(p.x + 46.f + ImGui::CalcTextSize(nm.c_str()).x + 8.f, p.y + 5.f), kMuted, note.c_str());
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Estimated: each capped hit or stopped attack counts the attacker's median\nhit with the same skill or weapon in this match.");
                    }
                    if (!taker && r->key > 0)
                    {
                        RText(dl, p.x + xCasts, p.y + 5.f, kNameText, std::to_string(r->casts));
                        RText(dl, p.x + xInt, p.y + 5.f, r->intr ? kPurple : kMuted, r->intr ? std::to_string(r->intr) : "-");
                        RText(dl, p.x + xCanc, p.y + 5.f, r->canc ? kOrange : kMuted, r->canc ? std::to_string(r->canc) : "-");
                    }
                    RText(dl, p.x + xAmt, p.y + 5.f, MetricCol(sMetric), (r->est ? std::string("~") : std::string()) + FmtN(r->amount));
                    Bar(dl, ImVec2(p.x + xBar0, p.y + 9.f), ImVec2(p.x + xPct - 44.f, p.y + 17.f), sum ? (float)r->amount / (float)sum : 0.f, MetricCol(sMetric));
                    RText(dl, p.x + xPct, p.y + 5.f, kMuted, Pct(r->amount, sum));
                    if (!open) continue;
                    // One line per target (or source), with the casts aimed at it.
                    std::vector<int> list;
                    for (auto& [id, _] : r->per) list.push_back(id);
                    for (auto& [id, _] : r->castsOn) if (!r->per.count(id)) list.push_back(id);
                    std::sort(list.begin(), list.end(), [&](int a, int b) { return r->per[a] != r->per[b] ? r->per[a] > r->per[b] : r->castsOn[a] > r->castsOn[b]; });
                    long long pmax = 1;
                    for (int id : list) pmax = std::max(pmax, r->per[id]);
                    for (int id : list)
                    {
                        const ImVec2 q = SubLineHead(id);
                        if (!taker && r->key > 0)
                        {
                            RText(dl, q.x + xCasts, q.y + 2.f, r->castsOn[id] ? kNameText : kMuted, r->castsOn[id] ? std::to_string(r->castsOn[id]) : "-");
                            RText(dl, q.x + xInt, q.y + 2.f, r->intrOn[id] ? kPurple : kMuted, r->intrOn[id] ? std::to_string(r->intrOn[id]) : "-");
                            RText(dl, q.x + xCanc, q.y + 2.f, r->cancOn[id] ? kOrange : kMuted, r->cancOn[id] ? std::to_string(r->cancOn[id]) : "-");
                        }
                        RText(dl, q.x + xAmt, q.y + 2.f, r->per[id] ? kNameText : kMuted, r->per[id] ? FmtN(r->per[id]) : "-");
                        Bar(dl, ImVec2(q.x + xBar0, q.y + 8.f), ImVec2(q.x + xPct - 44.f, q.y + 12.f), (float)r->per[id] / (float)pmax, WithAlpha(MetricCol(sMetric), 150));
                    }
                    ImGui::Dummy(ImVec2(W, 4.f));
                }

                // Everyone this player's figures went to (or came from), all skills together: the
                // mockup's target grid, four to a row.
                std::map<int, long long> byOther;
                for (Row* r : order) for (auto& [id, a] : r->per)                 // -5: degeneration, source unknown; map effects by their own row
                    byOther[r->key == -5 || IsLedgerMapRow(r->key) ? r->key : id] += a;
                std::vector<std::pair<long long, int>> others;
                for (auto& [id, a] : byOther) if (a > 0) others.push_back({ a, id });
                std::sort(others.rbegin(), others.rend());
                if (!others.empty())
                {
                    ImGui::Dummy(ImVec2(0.f, 10.f));
                    const char* head = sMetric == kDealt ? "TARGETS" : sMetric == kTaken ? "HIT BY" : sMetric == kHealed ? "HEALED"
                                     : sMetric == kReceived ? "HEALED BY" : "PROTECTED";
                    Section(head, nullptr);
                    const float gap = 22.f, cw4 = (W - gap * 3.f) / 4.f, rh = 34.f;
                    const ImVec2 g0 = ImGui::GetCursorScreenPos();
                    const long long omax = others.front().first;
                    for (size_t i = 0; i < others.size(); ++i)
                    {
                        const int id = others[i].second;
                        const float x = g0.x + (i % 4) * (cw4 + gap), y = g0.y + (i / 4) * rh;
                        const bool lord = id == m_lordDmg[0].lordAgentId || id == m_lordDmg[1].lordAgentId || NameOf(id) == "Guild Lord";
                        ImTextureID ic = lord ? LoadFlagIcon(dev, Team::IsRed(TeamOf(id)) ? "redguildlord.png" : "blueguildlord.png")
                                       : IsLedgerMapRow(id) ? MapIcon(id)
                                       : id == -5 ? nullptr : ProfOf(id) ? LoadProfIcon(dev, ProfOf(id)) : nullptr;
                        if (ic) dl->AddImage(ic, ImVec2(x, y + 2.f), ImVec2(x + 20.f, y + 22.f));
                        const std::string val = FmtN(others[i].first);
                        const float vw = ImGui::CalcTextSize(val.c_str()).x;
                        dl->PushClipRect(ImVec2(x + 26.f, y), ImVec2(x + cw4 - vw - 8.f, y + rh), true);
                        dl->AddText(ImVec2(x + 26.f, y + 2.f), id == -5 || IsLedgerMapRow(id) ? kSoft : TeamText(TeamOf(id)),
                                    IsLedgerMapRow(id) ? MapName(id) : id == -5 ? "Degeneration, source unknown" : NameOf(id).c_str());
                        dl->PopClipRect();
                        RText(dl, x + cw4, y + 2.f, kNameText, val);
                        Bar(dl, ImVec2(x + 26.f, y + lineH + 6.f), ImVec2(x + cw4, y + lineH + 12.f), (float)others[i].first / (float)omax, MetricCol(sMetric));
                    }
                    ImGui::Dummy(ImVec2(W, ((others.size() + 3) / 4) * rh + 4.f));
                }
            }
            else
            {
                // Interrupts: one row per interrupting skill (or per skill lost), one line per foe inside.
                const bool recv = sMetric == kIntsRecv;
                struct IRow
                {
                    int key = 0, att = 0, landed = 0, faked = 0, missed = 0; std::vector<int> ms;
                    std::map<int, std::array<int, 4>> per; std::map<int, std::vector<int>> perMs;
                    std::map<int, std::map<int, int>> lost;      // foe -> skill he lost -> times (giving side)
                };
                std::map<int, IRow> rows;
                std::map<int, std::map<int, int>> stopped;       // foe -> skill he lost -> times, every skill together
                for (const LedgerInterrupt& li : m_ledgerInts)
                {
                    if (!In(li.t) || (recv ? li.dst : li.src) != me.id) continue;
                    if (recv && li.outcome == LedgerInterrupt::Missed) continue;
                    const int key = recv ? li.lostSkill : li.skillId;
                    IRow& r = rows[key];
                    r.key = key; r.att++;
                    auto& pc = r.per[recv ? li.src : li.dst];
                    pc[0]++;
                    if (li.outcome == LedgerInterrupt::Landed)     { r.landed++; pc[1]++; }
                    else if (li.outcome == LedgerInterrupt::Faked) { r.faked++;  pc[2]++; }
                    else                                           { r.missed++; pc[3]++; }
                    if (!recv && li.outcome == LedgerInterrupt::Landed && li.lostSkill > 0)
                    { r.lost[li.dst][li.lostSkill]++; stopped[li.dst][li.lostSkill]++; }
                    const int ms = recv ? li.afterMs : li.reflexMs;
                    if (li.outcome == LedgerInterrupt::Landed && ms >= 0) { r.ms.push_back(ms); r.perMs[recv ? li.src : li.dst].push_back(ms); }
                }
                std::vector<IRow*> order;
                for (auto& [k, r] : rows) order.push_back(&r);
                std::sort(order.begin(), order.end(), [](const IRow* a, const IRow* b) { return a->landed != b->landed ? a->landed > b->landed : a->att > b->att; });
                const float xTry = W * 0.42f, xLand = W * 0.50f, xFake = W * 0.58f, xMiss = W * 0.66f, xAcc = W * 0.76f, xMs = W * 0.90f;
                Section(recv ? "SKILLS LOST" : "INTERRUPTING SKILLS", recv ? "click a row for who interrupted" : "click a row for the foes it was used on");
                const ImVec2 h = ImGui::GetCursorScreenPos();
                dl->AddText(ImVec2(h.x + 46.f, h.y), kMuted, "Skill");
                if (recv)
                {
                    RText(dl, h.x + xLand, h.y, kMuted, "Interrupted");
                    RText(dl, h.x + xFake, h.y, kMuted, "Faked");
                    RText(dl, h.x + xMs, h.y, kMuted, "After (median)");
                }
                else
                {
                    RText(dl, h.x + xTry, h.y, kMuted, "Tries");
                    RText(dl, h.x + xLand, h.y, kMuted, "Landed");
                    RText(dl, h.x + xFake, h.y, kMuted, "Faked");
                    RText(dl, h.x + xMiss, h.y, kMuted, "Missed");
                    RText(dl, h.x + xAcc, h.y, kMuted, "Accuracy");
                    RText(dl, h.x + xMs, h.y, kMuted, "Reaction (median)");
                }
                ImGui::Dummy(ImVec2(W, lineH + 4.f));
                if (order.empty())
                    ImGui::TextColored(V4(kMuted), "%s", recv ? "No interrupt attempts on this player in this stretch." : "No interrupt attempts in this stretch.");
                // A skill icon with a small count badge in its corner (dark, rounded, white digits).
                auto IconBadge = [&](float x, float y, float sz, int skill, int count) {
                    if (ImTextureID ic = LoadSkillIcon(this, dev, skill, m_skillIconIndex, m_skillIconCache))
                        dl->AddImageRounded(ic, ImVec2(x, y), ImVec2(x + sz, y + sz), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 2.f);
                    else
                        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + sz, y + sz), IM_COL32(40, 40, 40, 255), 2.f);
                    if (ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + sz, y + sz)))
                        ImGui::SetTooltip("%s x%d", GetSkillDisplayName(skill).c_str(), count);
                    const std::string n = std::to_string(count);
                    const float fs = ImGui::GetFontSize() * 0.72f;
                    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, n.c_str());
                    const ImVec2 b1(x + sz + 2.f, y + sz + 2.f), b0(b1.x - ts.x - 6.f, b1.y - ts.y - 1.f);
                    dl->AddRectFilled(b0, b1, IM_COL32(12, 12, 14, 230), 3.f);
                    dl->AddText(ImGui::GetFont(), fs, ImVec2(b0.x + 3.f, b0.y), IM_COL32_WHITE, n.c_str());
                };
                auto SortedLost = [](const std::map<int, int>& m) {
                    std::vector<std::pair<int, int>> v;
                    for (auto& [sk, n] : m) v.push_back({ n, sk });
                    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
                    return v;
                };
                auto Figures = [&](ImVec2 p, float y, int att, int landed, int faked, int missed, int ms) {
                    if (!recv)
                    {
                        RText(dl, p.x + xTry, y, kNameText, std::to_string(att));
                        RText(dl, p.x + xMiss, y, kMuted, missed ? std::to_string(missed) : "-");
                        RText(dl, p.x + xAcc, y, kNameText, Pct(landed, att));
                    }
                    RText(dl, p.x + xLand, y, landed ? kPurple : kMuted, std::to_string(landed));
                    RText(dl, p.x + xFake, y, faked ? kOrange : kMuted, faked ? std::to_string(faked) : "-");
                    RText(dl, p.x + xMs, y, kNameText, FmtMs(ms));
                };
                for (IRow* r : order)
                {
                    bool open = false;
                    const ImVec2 p = RowFrame(r->key, 26.f, open);
                    SkillCell(p, r->key, r->key > 0 ? GetSkillDisplayName(r->key) : std::string("Unknown skill"));
                    Figures(p, p.y + 5.f, r->att, r->landed, r->faked, r->missed, Median(r->ms));
                    if (!open) continue;
                    for (auto& [id, c] : r->per)
                    {
                        const ImVec2 q = SubLineHead(id);
                        Figures(q, q.y + 2.f, c[0], c[1], c[2], c[3], Median(r->perMs[id]));
                        // What he lost to it: small icons with counts after his name.
                        if (auto lit = r->lost.find(id); lit != r->lost.end())
                        {
                            float ix = q.x + 66.f + ImGui::CalcTextSize(NameOf(id).c_str()).x + 10.f;
                            for (auto& [n, sk] : SortedLost(lit->second))
                            {
                                if (ix + 16.f > q.x + xTry - 40.f) break;
                                IconBadge(ix, q.y + 2.f, 16.f, sk, n);
                                ix += 24.f;
                            }
                        }
                    }
                    ImGui::Dummy(ImVec2(W, 4.f));
                }

                // What was interrupted: one row per foe, the skills he lost (with counts), his total.
                if (!recv && !stopped.empty())
                {
                    ImGui::Dummy(ImVec2(0.f, 10.f));
                    Section("SKILLS STOPPED", "what each foe lost to this player's interrupts");
                    std::vector<std::pair<int, int>> foes;            // total, foe
                    for (auto& [id, m] : stopped)
                    {
                        int n = 0;
                        for (auto& [sk, c] : m) n += c;
                        foes.push_back({ n, id });
                    }
                    std::sort(foes.begin(), foes.end(), [](auto& a, auto& b) { return a.first > b.first; });
                    const float gap = 22.f, cw = (W - gap) * 0.5f, rh = 36.f, icon = 26.f;
                    const ImVec2 g0 = ImGui::GetCursorScreenPos();
                    for (size_t i = 0; i < foes.size(); ++i)
                    {
                        const int id = foes[i].second;
                        const float x = g0.x + (i % 2) * (cw + gap), y = g0.y + (i / 2) * rh;
                        const bool lord = id == m_lordDmg[0].lordAgentId || id == m_lordDmg[1].lordAgentId || NameOf(id) == "Guild Lord";
                        ImTextureID pt = lord ? LoadFlagIcon(dev, Team::IsRed(TeamOf(id)) ? "redguildlord.png" : "blueguildlord.png")
                                       : ProfOf(id) ? LoadProfIcon(dev, ProfOf(id)) : nullptr;
                        if (pt) dl->AddImage(pt, ImVec2(x, y + 5.f), ImVec2(x + 18.f, y + 23.f));
                        const float nameW = std::min(cw * 0.32f, ImGui::CalcTextSize(NameOf(id).c_str()).x);
                        dl->PushClipRect(ImVec2(x + 24.f, y), ImVec2(x + 24.f + cw * 0.32f, y + rh), true);
                        dl->AddText(ImVec2(x + 24.f, y + 6.f), TeamText(TeamOf(id)), NameOf(id).c_str());
                        dl->PopClipRect();
                        const std::string tot = std::to_string(foes[i].first);
                        RText(dl, x + cw, y + 6.f, kPurple, tot);
                        float ix = x + 24.f + nameW + 12.f;
                        const float stop = x + cw - ImGui::CalcTextSize(tot.c_str()).x - 12.f;
                        for (auto& [n, sk] : SortedLost(stopped[id]))
                        {
                            if (ix + icon > stop) break;
                            IconBadge(ix, y + 2.f, icon, sk, n);
                            ix += icon + 8.f;
                        }
                        dl->AddLine(ImVec2(x, y + rh - 2.f), ImVec2(x + cw, y + rh - 2.f), kHairline);
                    }
                    ImGui::Dummy(ImVec2(W, ((foes.size() + 1) / 2) * rh + 4.f));
                }
            }
        }
    }
    else
    {
        // ===================== Team matrix =====================
        const int kind = sMetric == kDealt || sMetric == kTaken ? 0 : sMetric == kHealed || sMetric == kReceived ? 1 : sMetric == kPrevented ? 2 : 3;
        const bool within = kind == 1 || kind == 2;
        const uint8_t giveTeam = Team::FromIndex01(sMxSide);
        const uint8_t takeTeam = within ? giveTeam : Team::Other(giveTeam);
        const char* kAxes[4][2] = { { "attackers", "targets" }, { "healers", "recipients" }, { "protectors", "protected" }, { "interrupters", "interrupted" } };
        const char* kMost[4] = { "most hit", "most healed", "most protected", "most stopped" };
        const char* kVerb[4] = { "damage", "healing", "damage prevented", "interrupts landed" };

        {
            std::vector<std::string> labels;
            for (int s = 0; s < 2; ++s)
            {
                const uint8_t g = Team::FromIndex01(s);
                labels.push_back(within ? GuildLabel(g) : GuildLabel(g) + "   to   " + GuildLabel(Team::Other(g)));
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(V4(kGold), "%s", within ? "TEAM" : "WHO HITS WHOM");
            ImGui::SameLine(0.f, 12.f);
            // Pills like the other choices, but each guild name in its team's colour.
            for (int s = 0; s < 2; ++s)
            {
                const uint8_t g = Team::FromIndex01(s), o = Team::Other(g);
                std::vector<std::pair<std::string, ImU32>> parts = { { GuildLabel(g), TeamText(g) } };
                if (!within) { parts.push_back({ "  to  ", kSoft }); parts.push_back({ GuildLabel(o), TeamText(o) }); }
                float tw = 0.f;
                for (auto& [s2, c] : parts) tw += ImGui::CalcTextSize(s2.c_str()).x;
                const float h = lineH + 12.f, w = tw + 26.f;
                const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + w, p.y + h);
                ImGui::PushID(320 + s);
                if (ImGui::InvisibleButton("##side", ImVec2(w, h))) sMxSide = s;
                const bool hov = ImGui::IsItemHovered(), on = sMxSide == s;
                ImGui::PopID();
                dl->AddRectFilled(p, q, on ? IM_COL32(36, 28, 10, 255) : IM_COL32(255, 255, 255, hov ? 31 : 13), h * 0.5f);
                dl->AddRect(p, q, on ? IM_COL32(255, 214, 100, 204) : IM_COL32(255, 255, 255, 20), h * 0.5f, 0, on ? 1.5f : 1.f);
                float x = p.x + 13.f;
                for (auto& [s2, c] : parts)
                {
                    dl->AddText(ImVec2(x, p.y + 6.f), on || hov ? c : WithAlpha(c, 170), s2.c_str());
                    x += ImGui::CalcTextSize(s2.c_str()).x;
                }
                if (s == 0) ImGui::SameLine(0.f, 8.f);
            }
            ImGui::Dummy(ImVec2(0.f, 4.f));
        }
        {
            const std::string what = within ? GuildLabel(giveTeam) + ", " + kVerb[kind]
                                            : GuildLabel(giveTeam) + " to " + GuildLabel(takeTeam) + ", " + kVerb[kind];
            ImGui::TextColored(V4(kMuted), "%s.", what.c_str());
            ImGui::SameLine();
            ImGui::TextColored(V4(kNameText), "Rows: %s   Columns: %s", kAxes[kind][0], kAxes[kind][1]);
            ImGui::SameLine();
            Help("Each cell is what the row's player gave the column's player in the counted stretch. "
                 "Hover a cell to trace its row and column, or a column head for the full name. A dash means none. "
                 "The tagged column is the player who received the most.");
        }
        ImGui::Dummy(ImVec2(0.f, 4.f));
        std::vector<const Who*> rowsP, colsP;
        for (const Who& w : players) { if (w.team == giveTeam) rowsP.push_back(&w); if (w.team == takeTeam) colsP.push_back(&w); }
        std::map<std::pair<int, int>, long long> cell;
        long long mx = 1;
        if (kind == 3)
        {
            for (const LedgerInterrupt& li : m_ledgerInts)
                if (In(li.t) && li.outcome == LedgerInterrupt::Landed && TeamOf(li.src) == giveTeam)
                    mx = std::max(mx, ++cell[{ li.src, li.dst }]);
        }
        else
            for (const LedgerLine& ln : m_ledger)
            {
                if (ln.t < r0) continue;
                if (ln.t > r1) break;
                const bool match = kind == 0 ? ln.kind == LedgerKind::Damage
                                 : kind == 1 ? (ln.kind == LedgerKind::Heal || ln.kind == LedgerKind::DivineFavor)
                                             : ln.kind == LedgerKind::Prevented;
                if (!match || TeamOf(ln.src) != giveTeam || TeamOf(ln.dst) != takeTeam) continue;
                mx = std::max(mx, cell[{ ln.src, ln.dst }] += ln.amount);
            }
        std::vector<long long> colTot(colsP.size());
        for (size_t c = 0; c < colsP.size(); ++c)
            for (const Who* r : rowsP) colTot[c] += cell[{ r->id, colsP[c]->id }];
        const size_t peak = colTot.empty() ? 0 : (size_t)(std::max_element(colTot.begin(), colTot.end()) - colTot.begin());
        const ImU32 tint = kind == 0 ? IM_COL32(226, 80, 63, 0) : kind == 1 ? IM_COL32(108, 196, 86, 0)
                         : kind == 2 ? IM_COL32(230, 200, 70, 0) : IM_COL32(170, 110, 220, 0);

        // Column heads: a tag row, the profession icon, then the name (without its build prefix) on at
        // most two lines; hovering a head gives the full name.
        const float nameW = 190.f, cw = 92.f, ch = 28.f, iconS = 22.f;
        const float tagY = 0.f, iconY = lineH + 8.f, nameY = iconY + iconS + 4.f, headH = nameY + lineH * 2.f + 8.f;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(nameW + cw * (colsP.size() + 1) + 10.f, headH + ch * (rowsP.size() + 1) + 10.f));
        const ImVec2 mouse = io.MousePos;
        int hovR = -1, hovC = -1, hovHead = -1;
        if (ImGui::IsWindowHovered())
            for (size_t c = 0; c < colsP.size(); ++c)
            {
                const float x = o.x + nameW + c * cw;
                if (mouse.x >= x && mouse.x < x + cw - 2.f && mouse.y >= o.y && mouse.y < o.y + headH) { hovHead = (int)c; hovC = (int)c; }
                for (size_t r = 0; r < rowsP.size(); ++r)
                {
                    const float y = o.y + headH + r * ch;
                    if (mouse.x >= x && mouse.x < x + cw - 2.f && mouse.y >= y && mouse.y < y + ch - 2.f) { hovR = (int)r; hovC = (int)c; }
                }
            }
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        for (size_t c = 0; c < colsP.size(); ++c)
        {
            const float x = o.x + nameW + c * cw, cx = x + (cw - 2.f) * 0.5f;
            if (c == peak && colTot[c] > 0)
            {
                const float ts = fs * 0.85f, tw = font->CalcTextSizeA(ts, FLT_MAX, 0.f, kMost[kind]).x;
                dl->AddRectFilled(ImVec2(cx - tw * 0.5f - 6.f, o.y + tagY), ImVec2(cx + tw * 0.5f + 6.f, o.y + tagY + ts + 5.f), IM_COL32(255, 255, 255, 28), 3.f);
                dl->AddText(font, ts, ImVec2(cx - tw * 0.5f, o.y + tagY + 2.f), kNameText, kMost[kind]);
            }
            if (ImTextureID pt = colsP[c]->prof ? LoadProfIcon(dev, colsP[c]->prof) : nullptr)
                dl->AddImage(pt, ImVec2(cx - iconS * 0.5f, o.y + iconY), ImVec2(cx + iconS * 0.5f, o.y + iconY + iconS));
            const auto [l1, l2] = TwoLines(ShortName(colsP[c]->name), cw - 8.f);
            const ImU32 nc = (int)c == hovC ? kNameText : TeamText(takeTeam);
            dl->AddText(ImVec2(cx - ImGui::CalcTextSize(l1.c_str()).x * 0.5f, o.y + nameY), nc, l1.c_str());
            if (!l2.empty()) dl->AddText(ImVec2(cx - ImGui::CalcTextSize(l2.c_str()).x * 0.5f, o.y + nameY + lineH), nc, l2.c_str());
        }
        if (hovHead >= 0 && hovR < 0) ImGui::SetTooltip("%s", colsP[hovHead]->name.c_str());
        dl->AddText(ImVec2(o.x + nameW + colsP.size() * cw + 6.f, o.y + headH - lineH - 4.f), kMuted, "Total");
        for (size_t r = 0; r < rowsP.size(); ++r)
        {
            const float y = o.y + headH + r * ch;
            if (ImTextureID pt = rowsP[r]->prof ? LoadProfIcon(dev, rowsP[r]->prof) : nullptr)
                dl->AddImage(pt, ImVec2(o.x, y + 4.f), ImVec2(o.x + 20.f, y + 24.f));
            dl->AddText(ImVec2(o.x + 26.f, y + 6.f), (int)r == hovR ? kNameText : TeamText(giveTeam), rowsP[r]->name.c_str());
            long long rt = 0;
            for (size_t c = 0; c < colsP.size(); ++c)
            {
                const long long v = cell[{ rowsP[r]->id, colsP[c]->id }];
                rt += v;
                const float x = o.x + nameW + c * cw;
                const bool traced = (int)r == hovR || (int)c == hovC;
                const int a = std::min(255, (int)(255.f * (0.10f + 0.70f * (float)v / (float)mx)) + (traced ? 40 : 0));
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw - 2.f, y + ch - 2.f), v ? (tint | ((ImU32)a << 24)) : IM_COL32(255, 255, 255, traced ? 16 : 8), 2.f);
                if ((int)r == hovR && (int)c == hovC) dl->AddRect(ImVec2(x, y), ImVec2(x + cw - 2.f, y + ch - 2.f), IM_COL32(225, 190, 80, 0xE0), 2.f);
                const std::string s = v ? FmtN(v) : "-";
                const float tw = ImGui::CalcTextSize(s.c_str()).x;
                dl->AddText(ImVec2(x + (cw - 2.f - tw) * 0.5f, y + 6.f), v ? kNameText : IM_COL32(111, 106, 94, 255), s.c_str());
            }
            dl->AddText(ImVec2(o.x + nameW + colsP.size() * cw + 6.f, y + 6.f), kMuted, FmtN(rt).c_str());
        }
        {
            const float y = o.y + headH + rowsP.size() * ch + 2.f;
            dl->AddText(ImVec2(o.x + 26.f, y + 6.f), kMuted, "Total");
            for (size_t c = 0; c < colsP.size(); ++c)
            {
                const std::string s = FmtN(colTot[c]);
                const float tw = ImGui::CalcTextSize(s.c_str()).x;
                dl->AddText(ImVec2(o.x + nameW + c * cw + (cw - 2.f - tw) * 0.5f, y + 6.f), kMuted, s.c_str());
            }
        }
        if (hovR >= 0)
        {
            const long long v = cell[{ rowsP[hovR]->id, colsP[hovC]->id }];
            ImGui::BeginTooltip();
            ImGui::TextColored(V4(TeamText(giveTeam)), "%s", rowsP[hovR]->name.c_str());
            ImGui::SameLine(0.f, 4.f); ImGui::TextUnformatted("to"); ImGui::SameLine(0.f, 4.f);
            ImGui::TextColored(V4(TeamText(takeTeam)), "%s", colsP[hovC]->name.c_str());
            ImGui::Text("%s %s", v ? FmtN(v).c_str() : "No", kVerb[kind]);
            ImGui::EndTooltip();
        }
        ImGui::TextColored(V4(kMuted), "-  none in this stretch");
    }

    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(9);
}
