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

    FillLedgerData();

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

// The view's copy of the ledger: the lines, every agent they name, the players' skill uses.
void ReplayWindow::FillLedgerData() const
{
    BattleLedger::Data& d = m_ledgerData;
    d = BattleLedger::Data();
    d.lines = m_ledger;
    d.ints  = m_ledgerInts;
    for (int pid : m_playerIds)
    {
        auto it = m_replayCtx.agents.find(pid);
        if (it == m_replayCtx.agents.end() || it->second.type != AgentType::Player) continue;
        d.players.push_back(pid);
        auto& uses = d.uses[pid];
        for (const SkillUseEvent& su : it->second.skillUseHistory)
            uses.push_back({ m_skillView.ResolvePvpSkillId(su.skillId), su.targetId, su.startTime, su.endTime,
                             su.wasInterrupted, su.wasCancelled });
    }
    const int lordIds[2] = { m_lordDmg[0].lordAgentId, m_lordDmg[1].lordAgentId };
    for (const auto& [id, a] : m_replayCtx.agents)
    {
        BattleLedger::Agent ag;
        const bool player = a.type == AgentType::Player;
        ag.name = player && !a.partyBarLabel.empty() ? a.partyBarLabel
                : !a.partyBarLabel.empty() ? a.partyBarLabel : GetAgentLabel(a);
        ag.team = a.teamId;
        ag.prof = a.primaryProf;
        ag.sec  = a.secondaryProf;
        ag.lord = id == lordIds[0] || id == lordIds[1];
        d.agents.emplace(id, std::move(ag));
    }
    for (int side = 0; side < 2; ++side)
    {
        const std::string& tag = side == 0 ? m_folderTag1 : m_folderTag2;
        if (const GuildMeta* g = FindGuildByTagStatic(m_matchMeta, tag)) d.guild[side] = g->name + " [" + g->tag + "]";
        d.tag[side] = tag;
    }
}

// ---------------------------------------------------------------------------
// The panel: the window chrome, then the shared view (BattleLedgerView.cpp) with the replay's
// icons and digits, and its time strip.
// ---------------------------------------------------------------------------
namespace
{
    class ReplayLedgerHost : public BattleLedger::Host
    {
    public:
        ReplayLedgerHost(ReplayWindow* rw, ID3D11Device* dev, std::unordered_map<int, std::string>& index,
                         std::unordered_map<int, ComPtr<ID3D11ShaderResourceView>>& cache,
                         const BitmapFont* dmg, const BitmapFont* heal, const BitmapFont* energy)
            : m_rw(rw), m_dev(dev), m_index(index), m_cache(cache), m_dmg(dmg), m_heal(heal), m_energy(energy) {}
        ImTextureID SkillIcon(int skillId) override { return LoadSkillIcon(m_rw, m_dev, skillId, m_index, m_cache); }
        ImTextureID ProfIcon(int prof) override { return LoadProfIcon(m_dev, prof); }
        ImTextureID UiIcon(const char* file) override { return LoadFlagIcon(m_dev, file); }
        std::string SkillName(int skillId) override { return m_rw->GetSkillDisplayName(skillId); }
        const BitmapFont* Digits(int m) override
        {
            using namespace BattleLedger;
            return m == kDealt || m == kTaken ? m_dmg : m == kInts || m == kIntsRecv ? m_energy : m_heal;
        }
    private:
        ReplayWindow* m_rw;
        ID3D11Device* m_dev;
        std::unordered_map<int, std::string>& m_index;
        std::unordered_map<int, ComPtr<ID3D11ShaderResourceView>>& m_cache;
        const BitmapFont *m_dmg, *m_heal, *m_energy;
    };

    BattleLedger::ViewState sLedgerView;
    const char* kScopeName[] = { "Full match", "Up to now", "Last 60 s", "Selection" };

    std::string Clock(float t)
    {
        const int s = std::max(0, (int)t);
        char b[16]; snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60); return b;
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
    if (visible)
    {
        EnsureSkillIconIndex();
        EnsureBitmapFontsLoaded();
        ReplayLedgerHost host(this, m_deviceResources->GetD3DDevice(), m_skillIconIndex, m_skillIconCache,
                              &m_damageBitmapFont, &m_healBitmapFont, &m_energyBitmapFont);
        BattleLedger::StripInputs strip;
        strip.tEnd = std::max(1.f, m_replayCtx.maxReplayTime);
        strip.tNow = m_debugTimeline;
        for (const SpikeInfo& s : m_spikes) strip.spikes.push_back({ s.firstHit, s.target });
        strip.deaths = &m_moraleDeaths;

        BattleLedger::DrawMetricTabs(host, sLedgerView);
        float r0 = 0.f, r1 = strip.tEnd;
        BattleLedger::DrawTimeStrip(m_ledgerData, sLedgerView, strip, r0, r1);
        ImGui::BeginChild("##ledger_body", ImVec2(0, 0), false);
        BattleLedger::DrawBody(m_ledgerData, host, sLedgerView, r0, r1,
                               std::string(kScopeName[sLedgerView.scope]) + " " + Clock(r0) + "-" + Clock(r1));
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(9);
}
