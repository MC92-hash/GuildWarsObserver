#include "pch.h"
#include "TeamColors.h"
#include "ReplayWindow.h"
#include "SkillDatabase.h"
#include "InterruptCauses.h"
#include "ReplayWindow_Internal.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Extracted from ReplayWindow.cpp (partial-class split). These remain
// ReplayWindow:: member functions; only their definitions live here.
// ---------------------------------------------------------------------------

namespace
{
    // The Battle Ledger's palette (BattleLedgerView.cpp), so the two panels read alike.
    constexpr ImU32 kGold      = IM_COL32(225, 190, 80, 255);
    constexpr ImU32 kNameText  = IM_COL32(216, 220, 226, 255);
    constexpr ImU32 kMuted     = IM_COL32(122, 128, 135, 255);
    constexpr ImU32 kSoft      = IM_COL32(170, 176, 184, 255);
    constexpr ImU32 kBlueText  = IM_COL32(74, 200, 255, 255);
    constexpr ImU32 kRedText   = IM_COL32(255, 107, 107, 255);
    constexpr ImU32 kDamage    = IM_COL32(255, 115, 102, 255);
    constexpr ImU32 kHealing   = IM_COL32(140, 230, 128, 255);
    constexpr ImU32 kFeint     = IM_COL32(230, 205, 110, 255);
    // GW's own cast bar: purple is an interrupted cast (GmSkWarmup, docs gw1-cast-bar).
    constexpr ImU32 kInterrupt = IM_COL32(191, 97, 255, 255);
    constexpr ImU32 kCancelled = IM_COL32(122, 128, 135, 255);
    constexpr ImU32 kHover     = IM_COL32(255, 255, 255, 12);
    constexpr ImU32 kSelFill   = IM_COL32(225, 190, 80, 30);

    long long Key2(int a, int b) { return (static_cast<long long>(a) << 32) | static_cast<unsigned>(b); }
    int Ms(float t) { return static_cast<int>(std::lround(t * 1000.f)); }

    // INTERRUPTED / KNOCKED_DOWN stamps per victim, each claimed once.
    struct StampSet
    {
        std::unordered_map<int, std::vector<std::pair<float, bool>>> byAgent;
        void Add(int agent, float t) { byAgent[agent].push_back({ t, false }); }
        // Claims the nearest unclaimed stamp within `tol`; its time, or -1.
        float Claim(int agent, float t, float tol)
        {
            auto it = byAgent.find(agent);
            if (it == byAgent.end()) return -1.f;
            std::pair<float, bool>* best = nullptr;
            for (auto& s : it->second)
                if (!s.second && std::fabs(s.first - t) <= tol && (!best || std::fabs(s.first - t) < std::fabs(best->first - t)))
                    best = &s;
            if (!best) return -1.f;
            best->second = true;
            return best->first;
        }
    };
}

// ---------------------------------------------------------------------------
// The panel's entries. Every cast reads its outcome from the cast history - the same one the cast
// bars draw, so a cast stopped on a knockdown's own millisecond is an interrupt here too - every
// number its skill from DamageAttribution, every interrupt its interrupter from InterruptCauses.
// ---------------------------------------------------------------------------

void ReplayWindow::BuildCombatLogView()
{
    m_combatLogView.clear();
    m_clExpanded.clear();
    m_clSelectedRowIdx = -1;
    m_combatLogViewBuilt = true;
    m_combatLogViewUsedAttributes = m_attributesDeduced;

    const auto& agents = m_replayCtx.agents;
    const StoCData& stoc = m_replayCtx.stocData;
    auto& view = m_combatLogView;

    auto teamOf = [&](int id) -> int {
        auto it = agents.find(id);
        return it == agents.end() ? 0 : it->second.teamId;
    };
    auto enemies = [&](int a, int b) {
        const int ta = teamOf(a), tb = teamOf(b);
        return Team::IsSide(ta) && Team::IsSide(tb) && ta != tb;
    };

    const InterruptCauses::Table causes = InterruptCauses::Build(agents, stoc, m_skillView);

    StampSet interruptStamps, knockStamps;
    std::unordered_map<long long, int> knockCause;   // (victim, ms) -> the cause the packet names
    for (const CombatEvent& c : stoc.combat)
    {
        if (c.type == "INTERRUPTED") interruptStamps.Add(c.caster_id, c.time);
        else if (c.type == "KNOCKED_DOWN")
        {
            // KNOCKED_DOWN;victim;cause - older recordings write 0;victim.
            const int victim = c.target_id > 0 ? c.target_id : c.caster_id;
            const int cause  = c.target_id > 0 ? c.caster_id : 0;
            knockStamps.Add(victim, c.time);
            if (cause > 0 && cause != victim) knockCause[Key2(victim, Ms(c.time))] = cause;
        }
    }

    // Interrupting uses aimed at each agent, for telling a fake cast from a plain cancel.
    struct Threat { int caster; int skillId; float start; float land; };
    std::unordered_map<int, std::vector<Threat>> threats;
    for (const auto& [casterId, ard] : agents)
        for (size_t i = 0; i < ard.skillUseHistory.size(); ++i)
        {
            const SkillUseEvent& u = ard.skillUseHistory[i];
            if (u.wasCancelled || u.targetId <= 0 || u.targetId == casterId) continue;
            if (!InterruptCauses::Interrupts(m_skillView.Get(u.skillId))) continue;
            if (causes.ForUse(casterId, static_cast<int>(i))) continue;   // it did interrupt something
            if (!enemies(casterId, u.targetId)) continue;
            threats[u.targetId].push_back({ casterId, u.skillId, u.startTime, u.isInstant ? u.startTime : u.endTime });
        }

    // The skill a knockdown came from: its cause's latest knocking-down use on that victim.
    auto knockSkill = [&](int cause, int victim, float t) -> int {
        auto it = agents.find(cause);
        if (it == agents.end()) return 0;
        const auto& h = it->second.skillUseHistory;
        for (auto u = h.rbegin(); u != h.rend(); ++u)
        {
            const float end = u->isInstant ? u->startTime : u->endTime;
            if (end > t + 0.05f) continue;
            if (t - end > 1.5f) break;
            if (u->wasCancelled || (u->targetId != victim && u->targetId != cause)) continue;
            const SkillInfo* si = m_skillView.Get(u->skillId);
            if (si && InterruptCauses::Detail::Lower(si->description).find("knock") != std::string::npos) return u->skillId;
        }
        return 0;
    };

    // --- Casts ---
    std::unordered_map<long long, int> castEntry;                 // (caster, use) -> entry
    std::unordered_map<long long, std::vector<int>> castsBySkill; // (caster, skill) -> entries, by time
    std::unordered_set<long long> cutUses;                        // (victim, use) shown as an interrupt
    for (const InterruptCauses::Cause& c : causes.causes)
        if (c.victimUse >= 0) cutUses.insert(Key2(c.victim, c.victimUse));

    for (const auto& [agentId, ard] : agents)
    {
        int lastCancel = -1;
        float lastCancelEnd = 0.f;
        const auto& h = ard.skillUseHistory;
        for (size_t i = 0; i < h.size(); ++i)
        {
            const SkillUseEvent& u = h[i];
            if (u.skillId <= 0) continue;
            const int target = (u.targetId > 0 && u.targetId != agentId) ? u.targetId : 0;
            if (u.wasInterrupted)
            {
                lastCancel = -1;
                if (cutUses.count(Key2(agentId, static_cast<int>(i)))) continue;   // from its cause below
                CombatLogEntry e;
                e.kind = CombatLogKind::Interrupt;
                e.time = u.endTime;
                e.start = u.startTime;
                e.targetId = agentId;
                e.cutSkillId = u.skillId;
                if (const float kt = knockStamps.Claim(agentId, u.endTime, 0.015f); kt >= 0.f)
                {
                    e.knockdown = true;
                    auto kc = knockCause.find(Key2(agentId, Ms(kt)));
                    if (kc != knockCause.end()) { e.sourceId = kc->second; e.skillId = knockSkill(e.sourceId, agentId, kt); }
                }
                interruptStamps.Claim(agentId, u.endTime, 0.05f);
                view.push_back(std::move(e));
                continue;
            }
            if (u.wasCancelled)
            {
                CombatLogEntry e;
                e.kind = CombatLogKind::Cancel;
                e.time = u.endTime;
                e.start = u.startTime;
                e.sourceId = agentId;
                e.targetId = target;
                e.skillId = u.skillId;
                if (auto th = threats.find(agentId); th != threats.end())
                    for (const Threat& t : th->second)
                        // Begun while he was casting, landing at or after his cancel: the cancel
                        // made it miss.
                        if (t.start >= u.startTime - 0.5f && t.start <= u.endTime + 0.05f &&
                            t.land >= u.endTime - 0.05f && t.land <= u.endTime + 1.5f)
                        {
                            e.feint = true;
                            e.threatId = t.caster;
                            e.threatSkillId = t.skillId;
                            break;
                        }
                if (!e.feint && lastCancel >= 0 && view[lastCancel].skillId == u.skillId &&
                    !view[lastCancel].feint && u.startTime - lastCancelEnd <= 10.f)
                {
                    ++view[lastCancel].repeat;
                    lastCancelEnd = u.endTime;
                    continue;
                }
                lastCancel = e.feint ? -1 : static_cast<int>(view.size());
                lastCancelEnd = u.endTime;
                view.push_back(std::move(e));
                continue;
            }
            lastCancel = -1;
            CombatLogEntry e;
            e.kind = CombatLogKind::Cast;
            e.time = u.isInstant ? u.startTime : u.endTime;
            e.start = u.startTime;
            e.sourceId = agentId;
            e.targetId = target;
            e.skillId = u.skillId;
            e.fromCast = true;
            castEntry[Key2(agentId, static_cast<int>(i))] = static_cast<int>(view.size());
            castsBySkill[Key2(agentId, m_skillView.ResolvePvpSkillId(u.skillId))].push_back(static_cast<int>(view.size()));
            view.push_back(std::move(e));
        }
    }
    for (auto& [k, v] : castsBySkill)
        std::sort(v.begin(), v.end(), [&](int a, int b) { return view[a].time < view[b].time; });

    // --- Interrupts with a known interrupter ---
    for (const InterruptCauses::Cause& c : causes.causes)
    {
        CombatLogEntry e;
        e.kind = CombatLogKind::Interrupt;
        e.time = c.time;
        e.start = c.time;
        e.sourceId = c.caster;
        e.skillId = c.skillId;
        e.targetId = c.victim;
        if (c.victimUse >= 0)
            if (auto it = agents.find(c.victim); it != agents.end() && c.victimUse < (int)it->second.skillUseHistory.size())
            {
                e.cutSkillId = it->second.skillUseHistory[c.victimUse].skillId;
                e.start = it->second.skillUseHistory[c.victimUse].startTime;
            }
        interruptStamps.Claim(c.victim, c.time, 0.05f);
        knockStamps.Claim(c.victim, c.time, 0.015f);
        view.push_back(std::move(e));
    }

    // --- Knockdowns that cut no cast, and interrupts of an attack nobody claimed ---
    for (auto& [victim, stamps] : knockStamps.byAgent)
        for (auto& [t, claimed] : stamps)
        {
            if (claimed) continue;
            claimed = true;
            CombatLogEntry e;
            e.kind = CombatLogKind::Interrupt;
            e.time = e.start = t;
            e.targetId = victim;
            e.knockdown = true;
            if (auto kc = knockCause.find(Key2(victim, Ms(t))); kc != knockCause.end())
            {
                e.sourceId = kc->second;
                e.skillId = knockSkill(e.sourceId, victim, t);
            }
            interruptStamps.Claim(victim, t, 0.015f);
            view.push_back(std::move(e));
        }
    for (auto& [victim, stamps] : interruptStamps.byAgent)
        for (auto& [t, claimed] : stamps)
        {
            if (claimed) continue;
            CombatLogEntry e;
            e.kind = CombatLogKind::Interrupt;
            e.time = e.start = t;
            e.targetId = victim;
            view.push_back(std::move(e));
        }

    // --- Damage and healing: every packet with its attributed skill, on the cast that carried it ---
    const DamageAttribution::Table* table = DamageAttributionTable();
    std::unordered_map<long long, int> tickGroup;                // (source, skill, heal, ms) -> entry
    std::unordered_map<long long, std::pair<int, int>> hitAt;    // (source, target, ms) -> entry, hit (heals)
    auto groupKey = [](int src, int skill, bool heal, int ms) {
        return (static_cast<long long>(src) << 40) ^ (static_cast<long long>(skill) << 20) ^ (heal ? 1LL << 62 : 0) ^ ms;
    };
    auto addHit = [&](int entry, int target, int amount, float pct) {
        CombatLogEntry& e = view[entry];
        for (CombatLogHit& h : e.hits)
            if (h.targetId == target) { h.amount += amount; h.pct += pct; return; }   // a hit and its vampiric part
        e.hits.push_back({ target, amount, pct, 0 });
    };

    const auto& combat = stoc.combat;
    for (size_t i = 0; i < combat.size(); ++i)
    {
        const CombatEvent& ce = combat[i];
        if (!ce.IsDamageOrHeal()) continue;
        const bool heal = ce.type == "HEAL";
        DamageAttribution::Result r;
        if (table && i < table->packets.size()) r = table->packets[i];
        int src = ce.caster_id;
        if (src <= 0 && r.creditTo > 0) src = r.creditTo;
        // The Health a resurrected player comes back with is no one's heal (the Battle Ledger
        // leaves it out too).
        if (heal && src <= 0 && r.rule == DamageAttribution::Rule::None) continue;

        const uint32_t fix = table && i < table->maxHpFix.size() ? table->maxHpFix[i] : 0;
        const uint32_t mhp = fix ? fix : PacketMaxHp(ce.target_id, ce.time, ce.value);
        const int amount = mhp ? static_cast<int>(std::lround(std::fabs(ce.value) * static_cast<double>(mhp))) : 0;
        const float pct = std::fabs(ce.value);
        const int skill = r.skillId > 0 ? m_skillView.ResolvePvpSkillId(r.skillId) : 0;
        const int ms = Ms(ce.time);

        // Divine Favor's bonus rides on the heal it came with.
        if (heal && r.rule == DamageAttribution::Rule::DivineFavor)
            if (auto it = hitAt.find(groupKey(src, ce.target_id, true, ms)); it != hitAt.end())
            {
                CombatLogHit& h = view[it->second.first].hits[it->second.second];
                h.amount += amount;
                h.bonus += amount;
                h.pct += pct;
                continue;
            }

        int entry = -1;
        const auto gk = groupKey(src, skill, heal, ms);
        if (auto g = tickGroup.find(gk); g != tickGroup.end()) entry = g->second;
        else if (skill > 0)
        {
            // The cast's own landing: in its completing tick, or (a shot, a projectile) up to 2.5 s
            // later. A later tick of a lasting effect (a hex's ticks, a pulse) is its own line.
            using R = DamageAttribution::Rule;
            const bool sameTickOnly = r.rule == R::SameTick;
            const bool landing = sameTickOnly || r.rule == R::Tag || r.rule == R::Projectile ||
                                 r.rule == R::RangedAttack || r.rule == R::RangedHit || r.rule == R::HundredBlades;
            if (landing)
                if (auto cs = castsBySkill.find(Key2(src, skill)); cs != castsBySkill.end())
                    for (auto it = cs->second.rbegin(); it != cs->second.rend(); ++it)
                    {
                        const CombatLogEntry& c = view[*it];
                        if (c.time > ce.time + 0.05f) continue;
                        if (ce.time - c.time > (sameTickOnly ? 0.05f : 2.5f)) break;
                        if (!c.hits.empty()) break;   // already landed; this is a later packet
                        entry = *it;
                        break;
                    }
        }
        if (entry < 0)
        {
            CombatLogEntry e;
            e.time = e.start = ce.time;
            e.sourceId = src > 0 ? src : 0;
            e.skillId = skill;
            e.weaponHit = r.weaponHit && skill <= 0;
            e.kind = heal ? CombatLogKind::Heal : CombatLogKind::Damage;
            entry = static_cast<int>(view.size());
            view.push_back(std::move(e));
        }
        else
            view[entry].kind = heal ? CombatLogKind::Heal : CombatLogKind::Damage;
        tickGroup[gk] = entry;
        addHit(entry, ce.target_id, amount, pct);
        const auto& hits = view[entry].hits;
        for (int k = 0; k < (int)hits.size(); ++k)
            if (hits[k].targetId == ce.target_id) { hitAt[groupKey(src, ce.target_id, heal, ms)] = { entry, k }; break; }
    }
    // A single target reads as the entry's target; several are a group.
    for (CombatLogEntry& e : view)
        if (e.hits.size() == 1) e.targetId = (e.hits[0].targetId == e.sourceId) ? 0 : e.hits[0].targetId;
        else if (e.hits.size() > 1) e.targetId = 0;

    // --- Deaths and objectives, as the raw log reads them ---
    for (const CombatLogRow& row : m_combatLog)
    {
        if (row.category == CombatLogCategory::Death)
        {
            CombatLogEntry e;
            e.kind = CombatLogKind::Death;
            e.time = e.start = row.time;
            e.sourceId = row.casterId;
            view.push_back(std::move(e));
        }
        else if (row.category == CombatLogCategory::Jumbo)
        {
            CombatLogEntry e;
            e.kind = CombatLogKind::Jumbo;
            e.time = e.start = row.time;
            e.jumbo = row.eventType;
            e.jumboTeam = row.jumboTeam;
            view.push_back(std::move(e));
        }
    }

    std::stable_sort(view.begin(), view.end(),
        [](const CombatLogEntry& a, const CombatLogEntry& b) { return a.time < b.time; });
}

// ---------------------------------------------------------------------------
// Combat Log panel
// ---------------------------------------------------------------------------

void ReplayWindow::DrawCombatLog()
{
    if (!m_showCombatLog || !m_combatLogBuilt) return;
    if (!m_combatLogViewBuilt || (!m_combatLogViewUsedAttributes && m_attributesDeduced))
        BuildCombatLogView();

    ImGuiIO& io = ImGui::GetIO();
    float vpW = io.DisplaySize.x;
    float vpH = io.DisplaySize.y;

    ImGui::SetNextWindowSizeConstraints(ImVec2(420.f, 200.f), ImVec2(vpW, vpH));
    if (m_panelLayout.HasSavedSize("combat_log"))
        m_panelLayout.ApplySize("combat_log");
    else
        ImGui::SetNextWindowSize(ImVec2(760.f, 460.f), ImGuiCond_FirstUseEver);
    m_panelLayout.ApplyPosition("combat_log");

    // Gold-accented dark panel styling
    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,      ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,    ImVec4(1.f, 1.f, 1.f, 0.04f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,  ImVec4(0.80f, 0.68f, 0.30f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1.f, 0.84f, 0.39f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  ImVec4(1.f, 0.84f, 0.39f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 4.f);

    if (!ImGui::Begin("Combat Log", &m_showCombatLog))
    {
        m_panelLayout.TrackWindow("combat_log");
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(9);
        return;
    }

    m_panelLayout.TrackWindow("combat_log");

    {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, 0.f, std::max(0.f, vpW - sz.x));
        float cy = std::clamp(pos.y, 0.f, std::max(0.f, vpH - sz.y));
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));
    }

    ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;
    EnsureSkillIconIndex();

    auto teamOf = [&](int agentId) -> int {
        auto it = m_replayCtx.agents.find(agentId);
        return it == m_replayCtx.agents.end() ? 0 : it->second.teamId;
    };
    auto teamColor = [&](int agentId) -> ImU32 {
        const int t = teamOf(agentId);
        return Team::IsRed(t) ? kRedText : Team::IsBlue(t) ? kBlueText : kSoft;
    };
    auto agentName = [&](int agentId) -> std::string {
        if (agentId <= 0) return "";
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return std::format("#{}", agentId);
        auto& a = it->second;
        if (a.type == AgentType::Player) return a.playerName;
        if (!a.categoryName.empty()) return a.categoryName;
        return std::format("#{}", agentId);
    };
    const int followed = (m_cameraMode == CameraMode::FollowAgent) ? m_followedAgentId : -1;

    // --- Filter bar ---
    {
        bool prev[5] = { m_clFilterDamage, m_clFilterHeals, m_clFilterSkills, m_clFilterInterrupt, m_clFilterDeaths };
        const bool prevCancels = m_clShowCancels, prevFollow = m_clFollowCamera;
        const int prevTeam = m_clTeamFilter, prevPlayer = m_clFilterPlayerId;

        auto FilterPill = [](const char* label, bool active) -> bool {
            ImVec4 bg  = active ? ImVec4(0.14f, 0.11f, 0.04f, 1.f)
                                : ImVec4(1.f, 1.f, 1.f, 0.05f);
            ImVec4 tx  = active ? ImVec4(1.f, 0.91f, 0.69f, 1.f)
                                : ImVec4(0.60f, 0.64f, 0.69f, 1.f);
            ImVec4 hov = active ? ImVec4(0.20f, 0.16f, 0.06f, 1.f)
                                : ImVec4(1.f, 1.f, 1.f, 0.12f);
            ImVec4 bdr = active ? ImVec4(1.f, 0.84f, 0.39f, 0.80f)
                                : ImVec4(1.f, 1.f, 1.f, 0.08f);
            ImGui::PushStyleColor(ImGuiCol_Button, bg);
            ImGui::PushStyleColor(ImGuiCol_Text, tx);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(bg.x * 0.85f, bg.y * 0.85f, bg.z * 0.85f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, bdr);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(5);
            return clicked;
        };
        // Wraps to the next line when the pill would not fit.
        auto Flow = [&](const char* nextLabel) {
            const float w = ImGui::CalcTextSize(nextLabel).x + 24.f;
            const float right = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w;
            if (right < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
        };

        const bool allOn = m_clFilterDamage && m_clFilterHeals && m_clFilterSkills &&
                           m_clFilterInterrupt && m_clShowCancels && m_clFilterDeaths;
        if (FilterPill("All", allOn))
        {
            const bool v = !allOn;
            m_clFilterDamage = m_clFilterHeals = m_clFilterSkills = m_clFilterInterrupt = m_clShowCancels = m_clFilterDeaths = v;
        }
        struct { const char* label; bool* flag; const char* tip; } pills[] = {
            { "Damage", &m_clFilterDamage, nullptr }, { "Healing", &m_clFilterHeals, nullptr },
            { "Skills", &m_clFilterSkills, nullptr },
            { "Interrupts", &m_clFilterInterrupt, "Interrupts, knockdowns that cut a cast, and fake casts\n(cancels that made an enemy interrupt miss)." },
            { "Cancels", &m_clShowCancels, "Every cancelled cast. Fake casts are marked in gold." },
            { "Deaths & objectives", &m_clFilterDeaths, nullptr },
        };
        for (auto& p : pills)
        {
            Flow(p.label);
            if (FilterPill(p.label, *p.flag)) *p.flag = !*p.flag;
            if (p.tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.tip);
        }

        // Second line: whose side (in the team's colour, with its guild), the followed player, cancels.
        auto TeamPill = [](const char* label, ImU32 teamCol, bool active) -> bool {
            const ImVec4 c = ImGui::ColorConvertU32ToFloat4(teamCol);
            ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(c.x * 0.22f, c.y * 0.22f, c.z * 0.22f, 1.f) : ImVec4(1.f, 1.f, 1.f, 0.05f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? ImVec4(c.x * 0.30f, c.y * 0.30f, c.z * 0.30f, 1.f) : ImVec4(1.f, 1.f, 1.f, 0.12f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.x * 0.18f, c.y * 0.18f, c.z * 0.18f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, active ? c : ImVec4(c.x, c.y, c.z, 0.65f));
            ImGui::PushStyleColor(ImGuiCol_Border, active ? ImVec4(c.x, c.y, c.z, 0.85f) : ImVec4(c.x, c.y, c.z, 0.25f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            const bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(5);
            return clicked;
        };
        if (FilterPill("Both teams", m_clTeamFilter == 0)) m_clTeamFilter = 0;
        for (int t = 1; t <= 2; ++t)
        {
            // Party 1 is Blue, party 2 Red (TeamColors.h); the header is "Name [TAG]".
            const std::string& guild = (t == 1) ? m_team1GuildHeader : m_team2GuildHeader;
            const std::string label = (guild.empty() || guild == "Unknown" ? std::string(t == 1 ? "Blue" : "Red") : guild)
                                    + "##team" + std::to_string(t);
            Flow(label.c_str());
            if (TeamPill(label.c_str(), t == 1 ? kBlueText : kRedText, m_clTeamFilter == t)) m_clTeamFilter = t;
        }
        Flow("Follow camera");
        if (FilterPill("Follow camera", m_clFollowCamera)) m_clFollowCamera = !m_clFollowCamera;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Only what involves the player the camera follows.");

        if (m_clFilterPlayerId >= 0)
        {
            const std::string label = std::format("{}  x", agentName(m_clFilterPlayerId));
            Flow(label.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(teamColor(m_clFilterPlayerId)));
            if (FilterPill(label.c_str(), true)) m_clFilterPlayerId = -1;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only this player, as source or target. Click to clear.");
        }

        const bool changed =
            prev[0] != m_clFilterDamage || prev[1] != m_clFilterHeals || prev[2] != m_clFilterSkills ||
            prev[3] != m_clFilterInterrupt || prev[4] != m_clFilterDeaths || prevCancels != m_clShowCancels ||
            prevFollow != m_clFollowCamera || prevTeam != m_clTeamFilter || prevPlayer != m_clFilterPlayerId;
        if (changed)
            m_clScrollToSelected = true;
    }

    // --- Skill name filter (autocomplete multi-select) ---
    bool inputFocused = false, inputHovered = false;
    {
        if (!m_clFilterSkillIds.empty())
        {
            // A chip per chosen skill: its icon, its name, x to remove it.
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            for (int idx = 0; idx < (int)m_clFilterSkillIds.size(); ++idx)
            {
                const int sid = m_clFilterSkillIds[idx];
                const std::string name = GetSkillDisplayName(sid) + "  x";
                ImTextureID icon = dev ? LoadSkillIcon(this, dev, sid, m_skillIconIndex, m_skillIconCache) : nullptr;
                const float iconS = 18.f, h = iconS + 6.f;
                const ImVec2 ts = ImGui::CalcTextSize(name.c_str());
                const float w = ts.x + 18.f + (icon ? iconS + 5.f : 0.f);
                if (idx > 0)
                {
                    const float right = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w;
                    if (right < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
                }
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::PushID(idx);
                const bool clicked = ImGui::InvisibleButton("##chip", ImVec2(w, h));
                ImGui::PopID();
                const bool hov = ImGui::IsItemHovered();
                const ImVec2 q(p.x + w, p.y + h);
                cdl->AddRectFilled(p, q, hov ? IM_COL32(51, 41, 15, 255) : IM_COL32(36, 28, 10, 255), h * 0.5f);
                cdl->AddRect(p, q, IM_COL32(255, 214, 100, 204), h * 0.5f);
                float x = p.x + 4.f;
                if (icon)
                {
                    cdl->AddImageRounded(icon, ImVec2(x, p.y + 3.f), ImVec2(x + iconS, p.y + 3.f + iconS),
                                         ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, iconS * 0.5f);
                    x += iconS + 5.f;
                }
                else x += 6.f;
                cdl->AddText(ImVec2(x, p.y + (h - ts.y) * 0.5f), IM_COL32(255, 232, 176, 255), name.c_str());
                if (hov) ImGui::SetTooltip("Remove %s from the filter", GetSkillDisplayName(sid).c_str());
                if (clicked)
                {
                    m_clFilterSkillIds.erase(m_clFilterSkillIds.begin() + idx);
                    m_clFilterSkillSet.erase(sid);
                    --idx;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear all##clsk"))
            {
                m_clFilterSkillIds.clear();
                m_clFilterSkillSet.clear();
            }
        }

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputTextWithHint("##skillsearch",
            "Filter by skill name...", m_clSkillSearchBuf, sizeof(m_clSkillSearchBuf));
        inputFocused = ImGui::IsItemActive();
        inputHovered = ImGui::IsItemHovered();
        ImVec2 inputMin = ImGui::GetItemRectMin();
        ImVec2 inputMax = ImGui::GetItemRectMax();
        float dropdownW = inputMax.x - inputMin.x;

        if (inputHovered || inputFocused)
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

        if (ImGui::IsItemActivated()) m_clSkillSearchFocused = true;

        bool showDropdown = m_clSkillSearchFocused && m_clSkillSearchBuf[0] != '\0';
        bool dropdownHovered = false;

        if (showDropdown)
        {
            std::string query(m_clSkillSearchBuf);
            for (auto& c : query) c = (char)std::tolower((unsigned char)c);

            struct Match { int id; std::string name; };
            std::vector<Match> matches;

            const auto& db = m_skillView;
            if (db.IsLoaded())
            {
                db.ForEachSkill([&](const SkillInfo& si) {
                    if (si.name.empty()) return;
                    if (m_clFilterSkillSet.count(si.id)) return;
                    std::string lower = si.name;
                    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                    if (lower.find(query) != std::string::npos)
                        matches.push_back({si.id, si.name});
                });
            }

            for (auto& e : m_combatLogView)
            {
                if (e.skillId <= 0) continue;
                if (m_clFilterSkillSet.count(e.skillId)) continue;
                if (db.IsLoaded() && db.Get(e.skillId)) continue;
                std::string sn = GetSkillDisplayName(e.skillId);
                if (sn.empty() || sn.rfind("Skill ", 0) == 0) continue;
                std::string lower = sn;
                for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                if (lower.find(query) != std::string::npos)
                {
                    bool dup = false;
                    for (auto& m : matches) if (m.id == e.skillId) { dup = true; break; }
                    if (!dup) matches.push_back({e.skillId, sn});
                }
            }

            std::sort(matches.begin(), matches.end(),
                [](const Match& a, const Match& b) { return a.name < b.name; });

            if (!matches.empty())
            {
                int maxShow = std::min((int)matches.size(), 10);
                float rowH = ImGui::GetTextLineHeightWithSpacing();
                float popH = rowH * (float)maxShow + 12.f;
                if ((int)matches.size() > maxShow) popH += rowH;

                ImGui::SetNextWindowPos(ImVec2(inputMin.x, inputMax.y + 2.f));
                ImGui::SetNextWindowSize(ImVec2(dropdownW, popH));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 0.97f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.35f, 1.f));

                ImGui::Begin("##skill_dropdown", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoFocusOnAppearing);

                ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

                dropdownHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

                int shown = 0;
                for (auto& m : matches)
                {
                    if (shown >= 10) { ImGui::TextDisabled("... %d more", (int)matches.size() - 10); break; }
                    ImGui::PushID(m.id);

                    if (dev)
                    {
                        ImTextureID tex = LoadSkillIcon(this, dev, m.id,
                            m_skillIconIndex, m_skillIconCache);
                        if (tex)
                        {
                            ImGui::Image(tex, ImVec2(16, 16));
                            ImGui::SameLine();
                        }
                    }

                    if (ImGui::Selectable(m.name.c_str()))
                    {
                        m_clFilterSkillIds.push_back(m.id);
                        m_clFilterSkillSet.insert(m.id);
                        m_clSkillSearchBuf[0] = '\0';
                        m_clSkillSearchFocused = false;
                    }

                    ImGui::PopID();
                    ++shown;
                }

                ImGui::End();
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);
            }
        }

        if (!inputFocused && !dropdownHovered)
            m_clSkillSearchFocused = false;
    }

    // --- Which entries show ---
    auto involves = [](const CombatLogEntry& e, int agent) {
        if (e.sourceId == agent || e.targetId == agent) return true;
        for (const CombatLogHit& h : e.hits) if (h.targetId == agent) return true;
        return false;
    };
    auto sideOf = [&](const CombatLogEntry& e) -> int {
        switch (e.kind)
        {
        case CombatLogKind::Jumbo: return e.jumboTeam;
        case CombatLogKind::Interrupt: return e.sourceId > 0 ? teamOf(e.sourceId) : Team::Other(teamOf(e.targetId));
        default: return e.sourceId > 0 ? teamOf(e.sourceId) : 0;
        }
    };
    const bool skillFilterActive = !m_clFilterSkillSet.empty();
    // A recording names a skill's PvP version, the search may offer either: compare both resolved.
    std::unordered_set<int> skillFilter;
    for (int id : m_clFilterSkillSet) skillFilter.insert(m_skillView.ResolvePvpSkillId(id));
    auto skillMatches = [&](int id) { return id > 0 && skillFilter.count(m_skillView.ResolvePvpSkillId(id)) != 0; };
    struct Line { int entry; int hit; };   // hit -1: the entry itself, else one target of a group
    std::vector<Line> lines;
    lines.reserve(m_combatLogView.size());
    for (int i = 0; i < (int)m_combatLogView.size(); ++i)
    {
        const CombatLogEntry& e = m_combatLogView[i];
        if (e.time > m_debugTimeline) break;

        bool show = false;
        switch (e.kind)
        {
        case CombatLogKind::Damage:    show = m_clFilterDamage || (e.fromCast && m_clFilterSkills); break;
        case CombatLogKind::Heal:      show = m_clFilterHeals  || (e.fromCast && m_clFilterSkills); break;
        case CombatLogKind::Cast:      show = m_clFilterSkills; break;
        case CombatLogKind::Interrupt:
            // A knockdown that cut no cast is a skill's effect, not an interrupt.
            show = (e.knockdown && e.cutSkillId == 0) ? m_clFilterSkills : m_clFilterInterrupt;
            break;
        case CombatLogKind::Cancel:
            show = m_clShowCancels || (e.feint && m_clFilterInterrupt);
            break;
        case CombatLogKind::Death:
        case CombatLogKind::Jumbo:     show = m_clFilterDeaths; break;
        }
        if (!show) continue;

        if (e.kind != CombatLogKind::Jumbo)
        {
            if (m_clFilterPlayerId >= 0 && !involves(e, m_clFilterPlayerId)) continue;
            if (m_clFollowCamera && followed >= 0 && !involves(e, followed)) continue;
            if (skillFilterActive && !skillMatches(e.skillId) && !skillMatches(e.cutSkillId) && !skillMatches(e.threatSkillId))
                continue;
        }
        if (m_clTeamFilter != 0 && sideOf(e) != m_clTeamFilter) continue;

        lines.push_back({ i, -1 });
        if (e.hits.size() > 1 && m_clExpanded.count(i))
            for (int k = 0; k < (int)e.hits.size(); ++k) lines.push_back({ i, k });
    }

    // --- The table ---
    constexpr float kRowH   = 22.f;
    constexpr float kIconSz = 18.f;
    constexpr float kProfSz = 16.f;
    enum Col { ColTime, ColSource, ColAction, ColTarget, ColResult };

    const float footerH = ImGui::GetFrameHeightWithSpacing();
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(1.f, 1.f, 1.f, 0.05f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImGui::ColorConvertU32ToFloat4(kSelFill));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::ColorConvertU32ToFloat4(kHover));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::ColorConvertU32ToFloat4(kSelFill));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.f, 0.f));

    const ImGuiTableFlags tflags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV |
                                   ImGuiTableFlags_NoPadOuterX;
    bool scrollHovered = false;
    if (ImGui::BeginTable("##cltable", 5, tflags, ImVec2(0.f, -footerH)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time",   ImGuiTableColumnFlags_WidthFixed, 74.f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.15f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 0.95f);

        // Header in the panel's gold.
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers, kRowH);
        for (int c = 0; c < 5; ++c)
        {
            ImGui::TableSetColumnIndex(c);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kGold));
            ImGui::TableHeader(ImGui::TableGetColumnName(c));
            ImGui::PopStyleColor();
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float fontSize = ImGui::GetFontSize();

        // Cell helpers: each draws from the cell's top-left, clipped to its width.
        auto cellOrigin = [&]() { return ImGui::GetCursorScreenPos(); };
        auto cellWidth  = [&]() { return ImGui::GetContentRegionAvail().x; };
        auto text = [&](ImVec2 p, float maxX, ImU32 col, const char* s, bool strike = false) -> float {
            const float y = p.y + (kRowH - fontSize) * 0.5f;
            dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(maxX, p.y + kRowH), true);
            dl->AddText(ImVec2(p.x, y), col, s);
            const float w = ImGui::CalcTextSize(s).x;
            if (strike)
                dl->AddLine(ImVec2(p.x, y + fontSize * 0.55f), ImVec2(std::min(p.x + w, maxX), y + fontSize * 0.55f), col, 1.f);
            dl->PopClipRect();
            return w;
        };
        auto agentCell = [&](int agentId, bool skull = false) {
            ImVec2 p = cellOrigin();
            const float maxX = p.x + cellWidth();
            if (skull)
                if (ImTextureID t = LoadFlagIcon(dev, "death.png"))
                {
                    dl->AddImage(t, ImVec2(p.x, p.y + (kRowH - kIconSz) * 0.5f), ImVec2(p.x + kIconSz, p.y + (kRowH + kIconSz) * 0.5f));
                    p.x += kIconSz + 4.f;
                }
            auto it = m_replayCtx.agents.find(agentId);
            if (dev && it != m_replayCtx.agents.end() && it->second.primaryProf > 0)
                if (ImTextureID t = LoadProfIcon(dev, it->second.primaryProf))
                {
                    dl->AddImage(t, ImVec2(p.x, p.y + (kRowH - kProfSz) * 0.5f), ImVec2(p.x + kProfSz, p.y + (kRowH + kProfSz) * 0.5f));
                    p.x += kProfSz + 4.f;
                }
            text(p, maxX, teamColor(agentId), agentName(agentId).c_str());
        };
        // style 0 normal, 1 cut short (purple, struck), 2 cancelled (grey, struck)
        auto skillCell = [&](int skillId, int style) {
            ImVec2 p = cellOrigin();
            const float maxX = p.x + cellWidth();
            if (dev && skillId > 0)
                if (ImTextureID t = LoadSkillIcon(this, dev, skillId, m_skillIconIndex, m_skillIconCache))
                {
                    const ImU32 tint = style == 0 ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 110);
                    dl->AddImageRounded(t, ImVec2(p.x, p.y + (kRowH - kIconSz) * 0.5f),
                        ImVec2(p.x + kIconSz, p.y + (kRowH + kIconSz) * 0.5f), ImVec2(0, 0), ImVec2(1, 1), tint, 2.f);
                    p.x += kIconSz + 5.f;
                }
            const ImU32 col = style == 1 ? kInterrupt : style == 2 ? kCancelled : kNameText;
            text(p, maxX, col, GetSkillDisplayName(skillId).c_str(), style != 0);
        };
        auto mutedCell = [&](const char* s) {
            const ImVec2 p = cellOrigin();
            text(p, p.x + cellWidth(), kMuted, s);
        };
        // A value right-aligned: the Health bold-coloured, its share of the maximum dim after it.
        auto valueCell = [&](bool heal, int amount, float pct) {
            const ImVec2 p = cellOrigin();
            const float w = cellWidth();
            char main[24], dim[16];
            const int pctI = static_cast<int>(std::lround(pct * 100.f));
            if (amount > 0) snprintf(main, sizeof main, "%s%d", heal ? "+" : "-", amount);
            else            snprintf(main, sizeof main, "%s%d%%", heal ? "+" : "-", pctI);
            if (amount > 0) snprintf(dim, sizeof dim, "  %d%%", pctI); else dim[0] = '\0';
            const float wm = ImGui::CalcTextSize(main).x, wd = ImGui::CalcTextSize(dim).x;
            const float x = std::max(p.x, p.x + w - wm - wd);
            text(ImVec2(x, p.y), p.x + w, heal ? kHealing : kDamage, main);
            if (dim[0]) text(ImVec2(x + wm, p.y), p.x + w, kMuted, dim);
        };
        auto rightText = [&](ImU32 col, const char* s) {
            const ImVec2 p = cellOrigin();
            const float w = cellWidth();
            const float tw = ImGui::CalcTextSize(s).x;
            text(ImVec2(std::max(p.x, p.x + w - tw), p.y), p.x + w, col, s);
        };
        auto skillTooltip = [&](int skillId) {
            const SkillInfo* si = m_skillView.Get(skillId);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGold), "%s", GetSkillDisplayName(skillId).c_str());
            if (!si) return;
            std::string facts;
            if (si->energy) facts += std::format("{} energy  ", si->energy);
            if (si->adrenaline) facts += std::format("{} adrenaline  ", si->adrenaline);
            if (si->activation > 0.f) facts += std::format("{:g} s cast  ", si->activation);
            if (si->recharge > 0.f) facts += std::format("{:g} s recharge", si->recharge);
            if (!facts.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kSoft), "%s", facts.c_str());
            const std::string& d = !si->concise.empty() ? si->concise : si->description;
            if (!d.empty())
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
                ImGui::TextUnformatted(d.c_str());
                ImGui::PopTextWrapPos();
            }
        };

        ImGuiListClipper clipper;
        clipper.Begin((int)lines.size(), kRowH);
        while (clipper.Step())
        {
            for (int li = clipper.DisplayStart; li < clipper.DisplayEnd; ++li)
            {
                const Line ln = lines[li];
                const CombatLogEntry& e = m_combatLogView[ln.entry];
                ImGui::TableNextRow(0, kRowH);
                ImGui::PushID(li);

                // Row bands: a death in red, an objective in its team's colour.
                if (ln.hit < 0 && e.kind == CombatLogKind::Death)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(208, 72, 72, 31));
                else if (ln.hit < 0 && e.kind == CombatLogKind::Jumbo)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                        e.jumbo == "NEUTRALIZED_SHRINE" ? IM_COL32(180, 180, 180, 18)
                        : Team::IsRed(e.jumboTeam)  ? IM_COL32(255, 107, 107, 20)
                        : Team::IsBlue(e.jumboTeam) ? IM_COL32(74, 200, 255, 20)
                                                    : IM_COL32(255, 215, 100, 20));

                // Time + the row's hit area across every column.
                ImGui::TableSetColumnIndex(ColTime);
                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                const bool selected = ln.entry == m_clSelectedRowIdx && ln.hit < 0;
                const bool clicked = ImGui::Selectable("##row", selected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.f, kRowH));
                const bool rowHovered = ImGui::IsItemHovered();
                const int hoveredCol = ImGui::TableGetHoveredColumn();
                ImGui::SetCursorScreenPos(rowMin);

                // The outcome mark on the left edge: purple interrupted, grey cancelled, gold fake cast.
                ImU32 mark = 0;
                if (ln.hit < 0)
                {
                    if (e.kind == CombatLogKind::Interrupt) mark = (e.knockdown && e.cutSkillId == 0) ? 0 : kInterrupt;
                    else if (e.kind == CombatLogKind::Cancel) mark = e.feint ? kFeint : kCancelled;
                    else if (e.kind == CombatLogKind::Death) mark = IM_COL32(204, 48, 48, 255);
                    else if (selected) mark = IM_COL32(255, 215, 100, 200);
                }
                if (mark)
                    dl->AddRectFilled(ImVec2(rowMin.x, rowMin.y + 1.f), ImVec2(rowMin.x + 2.f, rowMin.y + kRowH - 1.f), mark);

                if (ln.hit < 0)
                {
                    const float matchTime = e.time - m_displayTimeOffset;
                    const int totalMs = (int)(std::abs(matchTime) * 1000.f);
                    char ts[24];
                    snprintf(ts, sizeof ts, "%s%02d:%02d.%03d", matchTime < 0.f ? "-" : "",
                             totalMs / 60000, (totalMs / 1000) % 60, totalMs % 1000);
                    text(ImVec2(rowMin.x + 7.f, rowMin.y), rowMin.x + cellWidth(), kMuted, ts);
                }

                if (ln.hit >= 0)
                {
                    // One target of an opened group.
                    const CombatLogHit& h = e.hits[ln.hit];
                    ImGui::TableSetColumnIndex(ColTarget);
                    {
                        ImVec2 p = cellOrigin();
                        ImGui::SetCursorScreenPos(ImVec2(p.x + 12.f, p.y));
                        agentCell(h.targetId);
                    }
                    ImGui::TableSetColumnIndex(ColResult);
                    valueCell(e.kind == CombatLogKind::Heal, h.amount, h.pct);
                }
                else if (e.kind == CombatLogKind::Death)
                {
                    ImGui::TableSetColumnIndex(ColSource);  agentCell(e.sourceId, true);
                    ImGui::TableSetColumnIndex(ColAction);  mutedCell("died");
                }
                else if (e.kind == CombatLogKind::Jumbo)
                {
                    ImGui::TableSetColumnIndex(ColSource);
                    ImVec2 p = cellOrigin();
                    const char* icon = nullptr;
                    if (e.jumbo == "BASE_UNDER_ATTACK") icon = "damagedone.png";
                    else if (e.jumbo == "GUILD_LORD_UNDER_ATTACK") icon = "kill.png";
                    else if (e.jumbo == "CAPTURED_SHRINE" || e.jumbo == "NEUTRALIZED_SHRINE") icon = "Health_Shrine_Bonus.jpg";
                    else if (e.jumbo == "CAPTURED_TOWER") icon = Team::IsRed(e.jumboTeam) ? "Red_flag_waving.svg.png" : "Blue_flag_waving.svg.png";
                    else if (e.jumbo == "PARTY_DEFEATED") icon = "death2.png";
                    else if (e.jumbo == "MORALE_BOOST") icon = "Morale_10.png";
                    else if (e.jumbo == "VICTORY" || e.jumbo == "FLAWLESS_VICTORY") icon = "cup.webp";
                    if (icon)
                        if (ImTextureID t = LoadFlagIcon(dev, icon))
                        {
                            dl->AddImage(t, ImVec2(p.x, p.y + (kRowH - kIconSz) * 0.5f), ImVec2(p.x + kIconSz, p.y + (kRowH + kIconSz) * 0.5f));
                            p.x += kIconSz + 6.f;
                        }
                    const ImU32 col = e.jumbo == "NEUTRALIZED_SHRINE" ? IM_COL32(0xBB, 0xBB, 0xBB, 0xFF)
                                    : Team::IsRed(e.jumboTeam)  ? kRedText
                                    : Team::IsBlue(e.jumboTeam) ? kBlueText : kGold;
                    // The message runs across the row; the other cells are empty.
                    const float rowMaxX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
                    dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(rowMaxX, p.y + kRowH), false);
                    dl->AddText(ImVec2(p.x, p.y + (kRowH - fontSize) * 0.5f), col, JumboMessageDisplayText(e.jumbo, e.jumboTeam));
                    dl->PopClipRect();
                }
                else if (e.kind == CombatLogKind::Interrupt)
                {
                    ImGui::TableSetColumnIndex(ColSource);
                    if (e.sourceId > 0) agentCell(e.sourceId); else mutedCell("unknown");
                    ImGui::TableSetColumnIndex(ColAction);
                    if (e.skillId > 0) skillCell(e.skillId, 0);
                    else mutedCell(e.knockdown ? "Knockdown" : "-");
                    ImGui::TableSetColumnIndex(ColTarget);
                    agentCell(e.targetId);
                    ImGui::TableSetColumnIndex(ColResult);
                    if (e.cutSkillId > 0) skillCell(e.cutSkillId, 1);
                    else
                    {
                        const ImVec2 p = cellOrigin();
                        if (e.knockdown) rightText(kSoft, "knocked down");
                        else text(p, p.x + cellWidth(), kInterrupt, "action interrupted");
                    }
                }
                else
                {
                    // Cast, Cancel, Damage, Heal.
                    ImGui::TableSetColumnIndex(ColSource);
                    if (e.sourceId > 0) agentCell(e.sourceId); else mutedCell("-");
                    ImGui::TableSetColumnIndex(ColAction);
                    if (e.skillId > 0) skillCell(e.skillId, e.kind == CombatLogKind::Cancel ? 2 : 0);
                    else mutedCell(e.weaponHit ? "Attack" : "-");

                    ImGui::TableSetColumnIndex(ColTarget);
                    if (e.hits.size() > 1)
                    {
                        // A group: the count, opened by clicking it.
                        const ImVec2 p = cellOrigin();
                        const bool open = m_clExpanded.count(ln.entry) != 0;
                        const float cy = p.y + kRowH * 0.5f;
                        if (open) dl->AddTriangleFilled(ImVec2(p.x, cy - 3.f), ImVec2(p.x + 8.f, cy - 3.f), ImVec2(p.x + 4.f, cy + 3.f), kSoft);
                        else      dl->AddTriangleFilled(ImVec2(p.x + 1.f, cy - 4.f), ImVec2(p.x + 7.f, cy), ImVec2(p.x + 1.f, cy + 4.f), kSoft);
                        bool allies = true;
                        for (const CombatLogHit& h : e.hits)
                            if (teamOf(h.targetId) != teamOf(e.sourceId)) { allies = false; break; }
                        const std::string s = std::format("{} {}", e.hits.size(), allies ? "allies" : "targets");
                        text(ImVec2(p.x + 13.f, p.y), p.x + cellWidth(), kSoft, s.c_str());
                    }
                    else if (e.targetId > 0)
                        agentCell(e.targetId);

                    ImGui::TableSetColumnIndex(ColResult);
                    if (e.kind == CombatLogKind::Damage || e.kind == CombatLogKind::Heal)
                    {
                        int amount = 0;
                        float pct = 0.f;
                        for (const CombatLogHit& h : e.hits) { amount += h.amount; pct += h.pct; }
                        valueCell(e.kind == CombatLogKind::Heal, amount, pct);
                    }
                    else if (e.kind == CombatLogKind::Cancel)
                    {
                        const std::string s = e.feint ? std::string("fake cast")
                                            : e.repeat > 1 ? std::format("cancelled x{}", e.repeat) : std::string("cancelled");
                        rightText(e.feint ? kFeint : kCancelled, s.c_str());
                    }
                }

                // Hover: what the skill is and how the line was read.
                if (rowHovered && ln.hit < 0)
                {
                    const bool onAction = hoveredCol == ColAction, onResult = hoveredCol == ColResult;
                    const bool any = onAction || onResult;
                    if (any && (e.skillId > 0 || e.cutSkillId > 0 || e.kind == CombatLogKind::Interrupt ||
                                (e.kind == CombatLogKind::Heal && !e.hits.empty())))
                    {
                        ImGui::BeginTooltip();
                        if (e.kind == CombatLogKind::Interrupt)
                        {
                            if (onResult && e.cutSkillId > 0) skillTooltip(e.cutSkillId);
                            else if (e.skillId > 0) skillTooltip(e.skillId);
                            ImGui::Separator();
                            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
                            if (e.sourceId > 0 && e.skillId > 0)
                                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kSoft),
                                    "Interrupter: the interrupting skill aimed at %s landed on the interrupt's own millisecond.",
                                    agentName(e.targetId).c_str());
                            else if (e.knockdown)
                                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kSoft), e.cutSkillId > 0
                                    ? "The cast stopped on the knockdown's own millisecond."
                                    : "Knocked down while not casting.");
                            else
                                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kSoft),
                                    "The game does not say who interrupted, and no interrupting skill landed at that moment (a spirit, an area effect, an attack).");
                            ImGui::PopTextWrapPos();
                        }
                        else if (e.kind == CombatLogKind::Cancel)
                        {
                            skillTooltip(e.skillId);
                            ImGui::Separator();
                            if (e.feint)
                                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kFeint), "Fake cast: %s's %s was on its way and missed.",
                                    agentName(e.threatId).c_str(), GetSkillDisplayName(e.threatSkillId).c_str());
                            else if (e.repeat > 1)
                                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kSoft), "Cancelled %d times in a row.", e.repeat);
                            else
                                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kSoft), "Cancelled by the caster.");
                        }
                        else
                        {
                            if (e.skillId > 0) skillTooltip(e.skillId);
                            int bonus = 0;
                            for (const CombatLogHit& h : e.hits) bonus += h.bonus;
                            if (bonus > 0)
                            {
                                if (e.skillId > 0) ImGui::Separator();
                                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kHealing), "Includes +%d Divine Favor", bonus);
                            }
                            if (e.fromCast && e.time - e.start > 0.05f)
                                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kMuted), "Cast %.2f s", e.time - e.start);
                        }
                        ImGui::EndTooltip();
                    }
                }

                if (clicked && ln.hit < 0)
                {
                    m_clSelectedRowIdx = ln.entry;
                    if (hoveredCol == ColTarget && e.hits.size() > 1)
                    {
                        if (!m_clExpanded.erase(ln.entry)) m_clExpanded.insert(ln.entry);
                    }
                    else if (hoveredCol == ColSource && e.sourceId > 0 && e.kind != CombatLogKind::Jumbo)
                        m_clFilterPlayerId = e.sourceId;
                    else
                    {
                        m_debugTimeline = e.start;
                        const int follow = e.sourceId > 0 ? e.sourceId : e.targetId;
                        if (follow > 0) EnterFollowMode(follow);
                    }
                }
                else if (clicked && ln.hit >= 0)
                {
                    m_debugTimeline = e.time;
                    EnterFollowMode(e.hits[ln.hit].targetId);
                }

                ImGui::PopID();
            }
        }
        clipper.End();

        // Scroll to the selected entry when the filters change.
        if (m_clScrollToSelected)
        {
            bool found = false;
            if (m_clSelectedRowIdx >= 0)
                for (int i = 0; i < (int)lines.size(); ++i)
                    if (lines[i].entry == m_clSelectedRowIdx && lines[i].hit < 0)
                    {
                        ImGui::SetScrollY((float)i * kRowH - ImGui::GetWindowHeight() * 0.5f);
                        m_clAutoScroll = false;
                        found = true;
                        break;
                    }
            if (!found) m_clSelectedRowIdx = -1;
            m_clScrollToSelected = false;
        }

        // Follow the playhead while playing; pause when the user scrolls up while stopped.
        if (m_replayCtx.isPlaying)
            m_clAutoScroll = true;
        if (m_clAutoScroll)
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
        if (!m_replayCtx.isPlaying &&
            ImGui::GetScrollMaxY() > 0.f &&
            ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 20.f)
            m_clAutoScroll = false;

        scrollHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);

    if (scrollHovered && !inputHovered && !inputFocused)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);

    // Footer: resume button when auto-scroll paused
    if (!m_clAutoScroll)
    {
        if (ImGui::Button("Resume"))
            m_clAutoScroll = true;
    }
    else
    {
        ImGui::TextDisabled("Auto-scrolling...");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  %d lines", (int)lines.size());

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(9);
}

// One line per entry: time, kind, source, target, skill, the cast cut short, flags, then each hit
// as target:amount(+bonus).
bool ReplayWindow::ExportCombatLogView(const std::filesystem::path& outPath)
{
    if (!m_combatLogBuilt) return false;
    BuildCombatLogView();
    FILE* f = nullptr;
    if (_wfopen_s(&f, outPath.c_str(), L"w") != 0 || !f) return false;
    static const char* kKind[] = { "CAST", "DAMAGE", "HEAL", "INTERRUPT", "CANCEL", "DEATH", "JUMBO" };
    fprintf(f, "# time\tkind\tsource\ttarget\tskill\tcut\tflags\thits\n");
    for (const CombatLogEntry& e : m_combatLogView)
    {
        std::string flags;
        if (e.knockdown) flags += "knockdown ";
        if (e.feint) flags += std::format("feint({}:{}) ", e.threatId, e.threatSkillId);
        if (e.repeat > 1) flags += std::format("x{} ", e.repeat);
        if (e.fromCast) flags += "cast ";
        if (e.weaponHit) flags += "weapon ";
        if (!e.jumbo.empty()) flags += e.jumbo;
        std::string hits;
        for (const CombatLogHit& h : e.hits)
            hits += h.bonus ? std::format("{}:{}(+{}) ", h.targetId, h.amount, h.bonus) : std::format("{}:{} ", h.targetId, h.amount);
        fprintf(f, "%.3f\t%s\t%d\t%d\t%d\t%d\t%s\t%s\n", e.time, kKind[(int)e.kind], e.sourceId, e.targetId,
                e.skillId, e.cutSkillId, flags.c_str(), hits.c_str());
    }
    fclose(f);
    return true;
}
