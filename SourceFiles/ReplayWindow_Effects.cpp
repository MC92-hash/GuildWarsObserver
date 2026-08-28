#include "pch.h"
#include "ReplayWindow.h"
#include "AssetBlacklist.h"
#include "MatchRatings.h"
#include "MatchNotes.h"
#include "MatchBookmarks.h"
#include "AgentSnapshotParser.h"
#include "StoCParser.h"
#include "SkillDatabase.h"
#include "MaxHpSolver.h"
#include "DXMathHelpers.h"
#include "FontConfig.h"
#include "GuiGlobalConstants.h"
#include "MapBrowser.h"
#include "TextureCache.h"
#include "CursorSystem.h"
#include "SpatialAudioEngine.h"
#include "SoundCache.h"
#include "Parsers/BB9AnimationParser.h"
#include "Parsers/FileReferenceParser.h"
#include "ReplayWindow_Internal.h"
#include "../ThirdParty/nanosvg/nanosvg.h"
#include "../ThirdParty/nanosvg/nanosvgrast.h"
#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <random>
#include <algorithm>
#include <numeric>
#include <json.hpp>
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// Extracted from ReplayWindow.cpp (partial-class split). These remain
// ReplayWindow:: member functions; only their definitions live here.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Incoming effect display — floating damage/heal numbers on focused agent
// ---------------------------------------------------------------------------

int ReplayWindow::GetFocusedAgentId() const
{
    if (m_cameraMode == CameraMode::FollowAgent && m_followedAgentId >= 0)
        return m_followedAgentId;
    return -1;
}


void ReplayWindow::UpdateIncomingEffects()
{
    float now = m_debugTimeline;
    int focused = GetFocusedAgentId();

    if (focused != m_focusedAgentId || now < m_lastEffectScanTime - 0.5f)
    {
        m_incomingEffects.clear();
        m_focusedAgentId = focused;
        m_lastEffectScanTime = now;
        return;
    }
    m_focusedAgentId = focused;
    if (focused < 0) { m_incomingEffects.clear(); return; }

    std::erase_if(m_incomingEffects, [&](const IncomingEffect& e) {
        return (now - e.spawnTime) >= kEffectLifetime;
    });

    float scanFrom = m_lastEffectScanTime;
    float scanTo   = now;
    m_lastEffectScanTime = now;
    if (scanTo <= scanFrom) return;

    auto findAgentMaxHp = [&](int agentId, float t) -> uint32_t {
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return 0;
        const auto& snaps = it->second.snapshots;
        if (snaps.empty()) return 0;
        const AgentSnapshot* s = FindSnapshotAtTime(it->second, t);
        if (s && s->max_hp > 0) return s->max_hp;
        auto sit = std::lower_bound(snaps.begin(), snaps.end(), t,
            [](const AgentSnapshot& a, float b) { return a.time < b; });
        for (; sit != snaps.end(); ++sit)
            if (sit->max_hp > 0) return sit->max_hp;
        // A guild hall NPC's maximum is a stated constant, so it is known even when the camera
        // never looked at it. Without this a Guild Lord fell through to the player-only solver,
        // which returns 0 for an NPC, and his damage numbers were dropped for want of a
        // denominator. Deep Wound is the only thing that moves it.
        if (uint32_t fixed = LookupGuildNpcMaxHealth(it->second.modelId))
            return (s && s->has_deep_wound)
                       ? AgentReplayData::ApplyDeepWound(fixed) : fixed;
        // Recorded value first (it is the effective max HP); solved per
        // weapon set only when the recording carries none.
        return it->second.solvedMaxHpAtTime(t);
    };

    const auto& combatVec = m_replayCtx.stocData.combat;
    std::unordered_set<size_t> consumedCombat;
    std::unordered_map<size_t, int> consumedSkillMap;

    struct CombatMatch { bool found; float value; float time; size_t index; };
    auto findCombatValue = [&](int casterId, int targetId, float skillEndTime) -> CombatMatch {
        for (size_t i = 0; i < combatVec.size(); ++i)
        {
            const auto& ce = combatVec[i];
            if (!ce.IsDamageOrHeal()) continue;
            if (ce.caster_id != casterId) continue;
            if (ce.target_id != targetId) continue;
            if (consumedCombat.count(i)) continue;
            float dt = ce.time - skillEndTime;
            if (dt < -0.1f) continue;
            if (dt > 1.5f) break;

            CombatMatch result = { true, ce.value, ce.time, i };
            consumedCombat.insert(i);
            for (size_t j = i + 1; j < combatVec.size(); ++j)
            {
                const auto& ce2 = combatVec[j];
                if (ce2.type != "DAMAGE") continue;
                if (ce2.caster_id != casterId || ce2.target_id != targetId) continue;
                if (ce2.time - ce.time > 0.15f) break;
                if (consumedCombat.count(j)) continue;
                result.value += ce2.value;
                consumedCombat.insert(j);
            }
            return result;
        }
        return { false, 0.f, 0.f, 0 };
    };

    const auto& db = m_skillView;

    auto pushEffect = [&](IncomingEffect eff) {
        constexpr float kMinSep = 35.f;
        constexpr float kTimeWindow = 0.4f;
        float bestOff = 0.f;
        float bestDist = 0.f;
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            float candidate = (float)(rand() % 181) - 90.f;
            float minDist = 999.f;
            for (const auto& ex : m_incomingEffects)
            {
                if (std::abs(ex.spawnTime - eff.spawnTime) > kTimeWindow) continue;
                minDist = std::min(minDist, std::abs(candidate - ex.xOffset));
            }
            if (minDist > bestDist) { bestDist = minDist; bestOff = candidate; }
            if (bestDist >= kMinSep) break;
        }
        eff.xOffset = bestOff;
        m_incomingEffects.push_back(std::move(eff));
    };

    // Skills that target a foe but only deal damage to the caster (self-damage).
    // These must never consume combat damage events on the target.
    auto isCasterDamageOnly = [](int skillId) -> bool {
        switch (skillId) {
        case 141:   // Rend Enchantments — "you lose 55..25 Health" per monk enchantment removed
        case 863:   // Order of Apostasy — "you lose 25..15% max Health" per monk enchantment removed
            return true;
        default:
            return false;
        }
    };

    // Delayed damage pass (runs FIRST to prevent the primary scan's
    // findCombatValue from stealing delayed hits). For each unconsumed damage
    // event, finds the skill whose endTime is closest to exactly 3.0s before
    // the damage (±0.5s). The [2.5, 3.5] window doesn't overlap with
    // findCombatValue's [−0.1, 1.5] window, so ordering is safe.
    constexpr float kDelayCenter = 3.0f;
    constexpr float kDelayHalf   = 0.5f;
    auto castKey = [](int casterId, float endTime) -> uint64_t {
        uint32_t a = (uint32_t)casterId;
        uint32_t b; std::memcpy(&b, &endTime, sizeof(b));
        return ((uint64_t)a << 32) | b;
    };
    std::unordered_set<uint64_t> delayConsumedCasts;
    for (size_t ci = 0; ci < combatVec.size(); ++ci)
    {
        if (consumedCombat.count(ci)) continue;
        const auto& dce = combatVec[ci];
        if (!dce.IsDamageOrHeal()) continue;
        if (dce.target_id != focused) continue;
        if (dce.caster_id == focused) continue;
        if (dce.time <= scanFrom || dce.time > scanTo) continue;

        auto casterIt = m_replayCtx.agents.find(dce.caster_id);
        if (casterIt == m_replayCtx.agents.end()) continue;

        int bestSkillId = 0;
        float bestDist2 = 1e9f;
        float bestEndTime = 0.f;
        for (const auto& su : casterIt->second.skillUseHistory)
        {
            if (su.wasCancelled) continue;
            if (su.targetId != focused) continue;
            if (isCasterDamageOnly(su.skillId)) continue;
            if (delayConsumedCasts.count(castKey(dce.caster_id, su.endTime))) continue;
            {
                const SkillInfo* dsi = db.Get(su.skillId);
                if (dsi && !dsi->description.empty() &&
                    dsi->description.find("damage") == std::string::npos)
                    continue;
            }
            float ddt = dce.time - su.endTime;
            if (ddt < kDelayCenter - kDelayHalf || ddt > kDelayCenter + kDelayHalf) continue;
            float dist = std::abs(ddt - kDelayCenter);
            if (dist < bestDist2)
            {
                bestDist2 = dist;
                bestSkillId = su.skillId;
                bestEndTime = su.endTime;
            }
        }
        if (bestSkillId == 0) continue;

        delayConsumedCasts.insert(castKey(dce.caster_id, bestEndTime));
        float totalValue = dce.value;
        consumedCombat.insert(ci);
        consumedSkillMap[ci] = bestSkillId;
        for (size_t cj = ci + 1; cj < combatVec.size(); ++cj)
        {
            const auto& ce2 = combatVec[cj];
            if (ce2.time - dce.time > 0.15f) break;
            if (ce2.type != "DAMAGE") continue;
            if (ce2.caster_id != dce.caster_id || ce2.target_id != focused) continue;
            if (consumedCombat.count(cj)) continue;
            totalValue += ce2.value;
            consumedCombat.insert(cj);
            consumedSkillMap[cj] = bestSkillId;
        }

        bool dHeal = (totalValue > 0.f);
        IncomingEffect deff;
        deff.spawnTime = dce.time;
        deff.skillId = bestSkillId;
        deff.type = dHeal ? IncomingEffectType::Heal : IncomingEffectType::Damage;
        uint32_t dmhp = findAgentMaxHp(focused, dce.time);
        int dRaw = (dmhp > 0) ? (int)std::round(std::abs(totalValue) * dmhp) : 0;
        if (dRaw > 0)
            deff.label = std::format("{}{}", dHeal ? "+" : "-", dRaw);
        else
            deff.label = std::format("{}{:.0f}%", dHeal ? "+" : "-", std::abs(totalValue) * 100.f);
        pushEffect(std::move(deff));
    }

    // Primary source: two-pass approach that collects all skill→damage candidates
    // then resolves conflicts so each damage event is attributed to the skill
    // whose cast end time is closest to the damage impact time.
    constexpr float kProjectileWindow = 1.5f;

    struct PrimaryCandidate {
        int agentId = 0;
        int skillId = 0;
        float endTime = 0.f;
        int skillType = 0;
        const SkillInfo* si = nullptr;
        size_t primaryHitIdx = SIZE_MAX;
        float hitTime = 0.f;
        float timeDelta = 0.f;
    };
    std::vector<PrimaryCandidate> primCandidates;

    // Pass 1: collect candidates, peek combat matches without consuming.
    // Hexes and caster-damage-only skills are emitted immediately.
    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (agentId == focused) continue;
        for (const auto& su : ard.skillUseHistory)
        {
            if (su.wasCancelled) continue;
            if (su.targetId != focused) continue;
            float endT = su.endTime;
            if (endT > scanTo || endT < scanFrom - kProjectileWindow) continue;

            const SkillInfo* si = db.Get(su.skillId);
            int skillType = si ? si->type : 0;

            if (skillType == 24)
            {
                if (endT > scanFrom && endT <= scanTo)
                {
                    IncomingEffect hexEff;
                    hexEff.spawnTime = endT;
                    hexEff.skillId = su.skillId;
                    hexEff.type = IncomingEffectType::Hex;
                    pushEffect(std::move(hexEff));
                }
                continue;
            }

            if (isCasterDamageOnly(su.skillId))
            {
                if (endT > scanFrom && endT <= scanTo)
                {
                    IncomingEffect eff;
                    eff.spawnTime = endT;
                    eff.skillId = su.skillId;
                    eff.type = IncomingEffectType::Condition;
                    pushEffect(std::move(eff));
                }
                continue;
            }

            // Skills whose description never mentions "damage" cannot produce
            // DAMAGE combat events on the target (e.g. Gale = pure knockdown).
            // Show icon only so they don't steal damage events from real damage skills.
            if (si && !si->description.empty() &&
                si->description.find("damage") == std::string::npos)
            {
                if (endT > scanFrom && endT <= scanTo)
                {
                    IncomingEffect eff;
                    eff.spawnTime = endT;
                    eff.skillId = su.skillId;
                    eff.type = IncomingEffectType::Condition;
                    pushEffect(std::move(eff));
                }
                continue;
            }

            PrimaryCandidate cand;
            cand.agentId = agentId;
            cand.skillId = su.skillId;
            cand.endTime = endT;
            cand.skillType = skillType;
            cand.si = si;

            for (size_t i = 0; i < combatVec.size(); ++i)
            {
                const auto& ce = combatVec[i];
                if (!ce.IsDamageOrHeal()) continue;
                if (ce.caster_id != agentId) continue;
                if (ce.target_id != focused) continue;
                if (consumedCombat.count(i)) continue;
                float dt = ce.time - endT;
                if (dt < -0.1f) continue;
                if (dt > 1.5f) break;
                cand.primaryHitIdx = i;
                cand.hitTime = ce.time;
                cand.timeDelta = std::abs(dt);
                break;
            }

            primCandidates.push_back(cand);
        }
    }

    // Pass 2: resolve conflicts — when multiple skills claim the same damage
    // event, the one with the smallest timeDelta (closest cast end to impact)
    // wins; losers fall through to name-only display.
    {
        std::unordered_map<size_t, size_t> bestForHit;
        for (size_t ci = 0; ci < primCandidates.size(); ++ci)
        {
            auto& c = primCandidates[ci];
            if (c.primaryHitIdx == SIZE_MAX) continue;
            auto it = bestForHit.find(c.primaryHitIdx);
            if (it == bestForHit.end())
            {
                bestForHit[c.primaryHitIdx] = ci;
            }
            else
            {
                if (c.timeDelta < primCandidates[it->second].timeDelta)
                {
                    primCandidates[it->second].primaryHitIdx = SIZE_MAX;
                    it->second = ci;
                }
                else
                {
                    c.primaryHitIdx = SIZE_MAX;
                }
            }
        }
    }

    // Pass 3: emit effects — winners consume their damage event, losers show name only.
    for (auto& cand : primCandidates)
    {
        if (cand.primaryHitIdx != SIZE_MAX)
        {
            const auto& primaryHit = combatVec[cand.primaryHitIdx];
            float totalValue = primaryHit.value;
            consumedCombat.insert(cand.primaryHitIdx);
            consumedSkillMap[cand.primaryHitIdx] = cand.skillId;

            for (size_t j = cand.primaryHitIdx + 1; j < combatVec.size(); ++j)
            {
                const auto& ce2 = combatVec[j];
                if (ce2.type != "DAMAGE") continue;
                if (ce2.caster_id != cand.agentId || ce2.target_id != focused) continue;
                if (ce2.time - primaryHit.time > 0.15f) break;
                if (consumedCombat.count(j)) continue;
                totalValue += ce2.value;
                consumedCombat.insert(j);
                consumedSkillMap[j] = cand.skillId;
            }

            float showTime = primaryHit.time;
            if (showTime <= scanFrom || showTime > scanTo) continue;

            bool isHeal = (totalValue > 0.f);
            IncomingEffect eff;
            eff.spawnTime = showTime;
            eff.skillId = cand.skillId;
            eff.type = isHeal ? IncomingEffectType::Heal : IncomingEffectType::Damage;
            uint32_t mhp = findAgentMaxHp(focused, primaryHit.time);
            int rawVal = (mhp > 0) ? (int)std::round(std::abs(totalValue) * mhp) : 0;
            if (rawVal > 0)
                eff.label = std::format("{}{}", isHeal ? "+" : "-", rawVal);
            else
                eff.label = std::format("{}{:.0f}%", isHeal ? "+" : "-", std::abs(totalValue) * 100.f);

            pushEffect(std::move(eff));
        }
        else
        {
            // Damage-dealing skills that reached the candidate pool but had no
            // matching DAMAGE event were blocked, dodged, or absorbed — hide them.
            // Only enchantments (friendly buffs applied without a combat value)
            // are still worth showing.
            if (cand.skillType != 23 && cand.skillType != 33 && cand.skillType != 34)
                continue;

            float showTime = cand.endTime;
            if (showTime <= scanFrom || showTime > scanTo) continue;

            IncomingEffect eff;
            eff.spawnTime = showTime;
            eff.skillId = cand.skillId;
            eff.type = IncomingEffectType::Heal;
            eff.label = cand.si ? cand.si->name : "Enchantment";

            pushEffect(std::move(eff));
        }
    }

    // Also scan self-cast skills that target self (heals, enchantments on self)
    {
        auto it = m_replayCtx.agents.find(focused);
        if (it != m_replayCtx.agents.end())
        {
            for (const auto& su : it->second.skillUseHistory)
            {
                if (su.wasCancelled) continue;
                if (su.targetId != focused && su.targetId > 0) continue;
                float endT = su.endTime;
                if (endT <= scanFrom || endT > scanTo) continue;

                auto cm = findCombatValue(focused, focused, endT);
                if (!cm.found) continue;
                if (cm.value <= 0.f) continue;  // only show self-heals

                float showTime = cm.time;
                if (showTime > scanTo) continue;

                consumedCombat.insert(cm.index);
                consumedSkillMap[cm.index] = su.skillId;

                IncomingEffect eff;
                eff.spawnTime = showTime;
                eff.skillId = su.skillId;
                eff.type = IncomingEffectType::Heal;

                uint32_t mhp = CorrectMaxHpForPacket(
                    findAgentMaxHp(focused, cm.time), cm.value);
                int rawVal = (mhp > 0) ? (int)std::round(std::abs(cm.value) * mhp) : 0;
                if (rawVal > 0)
                    eff.label = std::format("+{}", rawVal);
                else
                    eff.label = std::format("+{:.0f}%", cm.value * 100.f);

                pushEffect(std::move(eff));
            }
        }
    }

    // Interrupt events from combat log
    for (const auto& ce : m_replayCtx.stocData.combat)
    {
        if (ce.time <= scanFrom || ce.time > scanTo) continue;
        if (ce.target_id != focused) continue;
        if (ce.type != "INTERRUPTED") continue;

        IncomingEffect eff;
        eff.spawnTime = ce.time;
        eff.skillId = (int)ce.value;
        eff.type = IncomingEffectType::Interrupt;
        eff.label = "INTERRUPT";
        pushEffect(std::move(eff));
    }

    // GenericValueID constants from the GW StoC protocol
    constexpr int kDmgType_Normal        = 16;
    constexpr int kDmgType_Critical      = 17;
    constexpr int kDmgType_ArmorIgnoring = 55;

    // Basic attack (auto-attack) damage on the focused agent.
    // Collects both weapon damage and vampiric (ARMORIGNORING) in a tight
    // sub-window, then displays them together (e.g. "-35 -5").
    struct AutoAttackMatch {
        bool               found       = false;
        float              weaponValue = 0.f;
        float              vampValue   = 0.f;
        float              time        = 0.f;
        std::vector<size_t> indices;
    };

    auto findAutoAttackDamage = [&](int casterId, int targetId, float attackTime) -> AutoAttackMatch {
        AutoAttackMatch m;
        for (size_t i = 0; i < combatVec.size(); ++i)
        {
            const auto& ce = combatVec[i];
            if (ce.type != "DAMAGE") continue;
            if (ce.caster_id != casterId) continue;
            if (ce.target_id != targetId) continue;
            float dt = ce.time - attackTime;
            if (dt < -0.1f) continue;
            if (dt > 3.0f) break;
            if (consumedCombat.count(i)) continue;

            if (!m.found) m.time = ce.time;
            if (m.found && (ce.time - m.time) > 0.15f) break;

            m.found = true;
            m.indices.push_back(i);
            if (ce.damage_type == kDmgType_ArmorIgnoring)
                m.vampValue += ce.value;
            else
                m.weaponValue += ce.value;
        }
        return m;
    };

    constexpr float kProjectileTravelMax = 3.0f;
    for (const auto& ev : m_replayCtx.stocData.basicAttack)
    {
        if (ev.type != "ATTACK_FINISHED") continue;
        if (ev.target_id != focused) continue;
        if (ev.time < scanTo - kProjectileTravelMax || ev.time > scanTo) continue;

        auto am = findAutoAttackDamage(ev.caster_id, focused, ev.time);
        if (!am.found) continue;
        if (am.weaponValue > 0.f && am.vampValue > 0.f) continue;
        if (am.time <= scanFrom || am.time > scanTo) continue;

        for (size_t idx : am.indices)
            consumedCombat.insert(idx);

        IncomingEffect eff;
        eff.spawnTime = am.time;
        eff.skillId = 0;
        eff.type = IncomingEffectType::BasicAttack;

        float primary = (am.weaponValue != 0.f) ? am.weaponValue : am.vampValue;
        uint32_t mhp = CorrectMaxHpForPacket(findAgentMaxHp(focused, am.time), primary);
        int rawPrimary = (mhp > 0) ? (int)std::round(std::abs(primary) * mhp) : 0;

        std::string label;
        if (rawPrimary > 0)
            label = std::format("-{}", rawPrimary);
        else
            label = std::format("-{:.0f}%", std::abs(primary) * 100.f);

        if (am.weaponValue != 0.f && am.vampValue != 0.f)
        {
            int rawVamp = (mhp > 0) ? (int)std::round(std::abs(am.vampValue) * mhp) : 0;
            if (rawVamp > 0)
                label += std::format(" -{}", rawVamp);
            else
                label += std::format(" -{:.0f}%", std::abs(am.vampValue) * 100.f);
        }

        eff.label = std::move(label);
        pushEffect(std::move(eff));
    }

    // Final pass: unattributed damage and heals.
    // Short-delay triggers (~3s) are already handled in the delayed damage pass above.
    // This pass catches truly long-delayed triggers (Mind Wrack at +6-40s) by
    // searching the caster's skillUseHistory for a hex (type 24) or unknown skill.
    //
    // Two-tier attribution: if damage coincides with the focused agent attacking,
    // prefer hexes whose description mentions "attack" (e.g. Empathy) over other
    // hexes (e.g. Mind Wrack).  Fallback prefers the earliest active hex.
    constexpr int   kSkillType_Hex    = 24;
    constexpr float kDelayedHexWindow = 60.f;

    std::vector<float> focusedAttackTimes;
    for (const auto& ev : m_replayCtx.stocData.basicAttack) {
        if (ev.type != "ATTACK_FINISHED") continue;
        if (ev.caster_id != focused) continue;
        focusedAttackTimes.push_back(ev.time);
    }

    auto coincideWithAttack = [&](float dmgTime) -> bool {
        auto it = std::lower_bound(focusedAttackTimes.begin(),
                                   focusedAttackTimes.end(), dmgTime - 0.3f);
        return it != focusedAttackTimes.end() && (*it - dmgTime) < 0.3f;
    };

    for (size_t i = 0; i < combatVec.size(); ++i)
    {
        const auto& ce = combatVec[i];
        if (!ce.IsDamageOrHeal()) continue;
        if (ce.target_id != focused) continue;
        if (ce.time <= scanFrom || ce.time > scanTo) continue;
        if (ce.caster_id == focused) continue;
        if (consumedCombat.count(i)) continue;

        int attrSkillId = 0;

        auto casterIt = m_replayCtx.agents.find(ce.caster_id);
        if (casterIt != m_replayCtx.agents.end())
        {
            bool attackCorrelated = coincideWithAttack(ce.time);

            int   bestAttackHex   = 0;
            float bestAttackDt    = 1e9f;
            int   bestFallbackHex = 0;
            float bestFallbackDt  = -1.f;

            for (const auto& su : casterIt->second.skillUseHistory)
            {
                if (su.wasCancelled) continue;
                if (su.targetId != focused) continue;
                if (su.endTime > ce.time) continue;
                if (ce.time - su.endTime > kDelayedHexWindow) continue;

                const SkillInfo* si_db = db.Get(su.skillId);
                if (si_db && si_db->type != kSkillType_Hex) continue;

                float dt = ce.time - su.endTime;

                if (attackCorrelated && si_db &&
                    si_db->description.find("attack") != std::string::npos)
                {
                    if (dt < bestAttackDt)
                    {
                        bestAttackDt  = dt;
                        bestAttackHex = su.skillId;
                    }
                }

                if (dt > bestFallbackDt)
                {
                    bestFallbackDt  = dt;
                    bestFallbackHex = su.skillId;
                }
            }

            attrSkillId = (bestAttackHex != 0) ? bestAttackHex : bestFallbackHex;
        }

        bool isHeal = (ce.value > 0.f);
        IncomingEffect eff;
        eff.spawnTime = ce.time;
        eff.skillId = attrSkillId;
        eff.type = isHeal ? IncomingEffectType::Heal : IncomingEffectType::Damage;

        uint32_t mhp = CorrectMaxHpForPacket(findAgentMaxHp(focused, ce.time), ce.value);
        int rawVal = (mhp > 0) ? (int)std::round(std::abs(ce.value) * mhp) : 0;
        if (rawVal > 0)
            eff.label = std::format("{}{}", isHeal ? "+" : "-", rawVal);
        else
            eff.label = std::format("{}{:.0f}%", isHeal ? "+" : "-", std::abs(ce.value) * 100.f);

        consumedCombat.insert(i);
        consumedSkillMap[i] = attrSkillId;
        pushEffect(std::move(eff));
    }
}


void ReplayWindow::EnsureBitmapFontsLoaded()
{
    if (m_damageBitmapFont.loaded && m_healBitmapFont.loaded) return;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    auto ddsDir = FindTexturesDDSDir();
    if (ddsDir.empty()) return;

    if (!m_damageBitmapFont.loaded)
        m_damageBitmapFont.Load(dev, (ddsDir / L"GW.EXE_0x753F0FB5.dds").c_str());
    if (!m_healBitmapFont.loaded)
        m_healBitmapFont.Load(dev, (ddsDir / L"GW.EXE_0xA0629E7F.dds").c_str());
}


void ReplayWindow::RenderIncomingEffects()
{
    if (m_incomingEffects.empty()) return;
    int focused = m_focusedAgentId;
    if (focused < 0) return;

    auto it = m_replayCtx.agents.find(focused);
    if (it == m_replayCtx.agents.end()) return;
    const auto& ard = it->second;
    if (ard.snapshots.empty()) return;

    float now = m_debugTimeline;
    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;

    XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    auto* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x, vpH = vp->Size.y;

    float sx, sy, sz;
    InterpolateAgentPosition(ard, now, m_replayCtx.interpSettings, sx, sy, sz);
    XMFLOAT3 worldPos = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);

    constexpr float FLOAT_DISTANCE   = 90.f;
    constexpr float ICON_SZ   = 26.f;
    constexpr float GAP       = 4.f;

    // Compute model-top Y using the same logic as the profession icon in DrawAgentOverlay,
    // so the floating numbers anchor just above the icon regardless of camera zoom/angle.
    float modelTopY = AgentModelTopY(focused, ard, worldPos.y, now);
    if (modelTopY <= worldPos.y)
        modelTopY = worldPos.y + 120.f;

    XMFLOAT3 topPos = { worldPos.x, modelTopY, worldPos.z };
    float anchorX, anchorY;
    if (!ProjectToScreen(viewProj, vpW, vpH, topPos, anchorX, anchorY)) return;

    // Offset upward in screen space: skip past the profession icon (iconSz + padding)
    // plus a small gap so numbers don't overlap the icon.
    float iconSz = std::clamp(vpH * 0.020f, 12.f, 20.f);
    constexpr float kScreenPad = 4.f;
    anchorY -= iconSz + kScreenPad * 2.f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    EnsureSkillIconIndex();
    EnsureBitmapFontsLoaded();

    const float glyphHeight = std::clamp(vpH * 0.013f, 9.f, 18.f);

    // Sort render order: oldest first (background) → newest last (foreground).
    // ImGui draws later calls on top, so newest effects appear in front.
    static std::vector<size_t> sortedIdx;
    sortedIdx.clear();
    for (size_t i = 0; i < m_incomingEffects.size(); ++i)
    {
        float age = now - m_incomingEffects[i].spawnTime;
        if (age >= 0.f && age < kEffectLifetime)
            sortedIdx.push_back(i);
    }
    std::sort(sortedIdx.begin(), sortedIdx.end(), [&](size_t a, size_t b) {
        return m_incomingEffects[a].spawnTime < m_incomingEffects[b].spawnTime;
    });

    const size_t totalActive = sortedIdx.size();

    for (size_t si = 0; si < totalActive; ++si)
    {
        const auto& e = m_incomingEffects[sortedIdx[si]];
        float age = now - e.spawnTime;

        float t = age / kEffectLifetime;
        float opacity = (t < 0.65f) ? 1.f : 1.f - ((t - 0.65f) / 0.35f);
        opacity = std::clamp(opacity, 0.f, 1.f);

        // Depth effect: older effects (drawn first) appear receded.
        // depthRank 0.0 = oldest, 1.0 = newest.
        float depthRank = (totalActive > 1)
            ? static_cast<float>(si) / static_cast<float>(totalActive - 1)
            : 1.f;
        float depthDim     = 0.55f + 0.45f * depthRank;
        opacity *= depthDim;

        uint8_t alpha = static_cast<uint8_t>(opacity * 255.f);

        float fy = anchorY - t * FLOAT_DISTANCE;
        float fx = anchorX + e.xOffset;

        bool isBasicAttack = (e.type == IncomingEffectType::BasicAttack);
        bool hasIcon = (e.skillId > 0) || isBasicAttack;
        constexpr float ATK_ICON_SZ = 18.f;
        float iconW = isBasicAttack ? ATK_ICON_SZ : ICON_SZ;

        bool useBitmapFont = !e.label.empty()
            && (e.type == IncomingEffectType::Damage
                || e.type == IncomingEffectType::Heal
                || e.type == IncomingEffectType::BasicAttack);

        const BitmapFont* bmFont = nullptr;
        if (useBitmapFont)
        {
            bmFont = (e.type == IncomingEffectType::Heal)
                         ? &m_healBitmapFont
                         : &m_damageBitmapFont;
            if (!bmFont->srv.Get()) bmFont = nullptr;
        }

        float labelW = 0.f;
        if (bmFont && !e.label.empty())
            labelW = bmFont->MeasureString(e.label.c_str(), glyphHeight);

        float totalW = 0.f;
        if (hasIcon) totalW += iconW;
        if (hasIcon && labelW > 0.f) totalW += GAP;
        if (labelW > 0.f) totalW += labelW;

        if (!bmFont && !e.label.empty())
        {
            const float labelFs = 13.f;
            ImFont* font = ImGui::GetFont();
            ImVec2 tsz = font->CalcTextSizeA(labelFs, FLT_MAX, 0.f, e.label.c_str());
            constexpr float PILL_PAD = 4.f;
            totalW = 0.f;
            if (hasIcon) totalW += iconW;
            if (hasIcon && tsz.x > 0) totalW += GAP;
            if (tsz.x > 0) totalW += PILL_PAD * 2.f + tsz.x;

            float startX = fx - totalW * 0.5f;
            float curX = startX;

            if (hasIcon)
            {
                ImTextureID tex = nullptr;
                if (isBasicAttack)
                    tex = LoadFlagIcon(dev, "kill.png");
                else
                    tex = LoadSkillIcon(this, dev, e.skillId, m_skillIconIndex, m_skillIconCache);
                ImVec2 iconTL(curX, fy - iconW * 0.5f);
                ImVec2 iconBR(curX + iconW, fy + iconW * 0.5f);
                if (tex)
                    dl->AddImage(tex, iconTL, iconBR,
                        ImVec2(0,0), ImVec2(1,1), IM_COL32(255, 255, 255, alpha));
                curX += iconW + GAP;
            }

            ImU32 labelCol;
            switch (e.type) {
            case IncomingEffectType::Interrupt:    labelCol = IM_COL32(0xE0, 0x70, 0x30, alpha); break;
            case IncomingEffectType::Condition:    labelCol = IM_COL32(0xE0, 0x70, 0x30, alpha); break;
            case IncomingEffectType::Hex:         labelCol = IM_COL32(0x90, 0x40, 0xC0, alpha); break;
            default:                              labelCol = IM_COL32(0xFF, 0xFF, 0xFF, alpha); break;
            }

            float pillX = curX;
            float pillY = fy - (tsz.y + PILL_PAD * 2.f) * 0.5f;
            float pillW = tsz.x + PILL_PAD * 2.f;
            float pillH = tsz.y + PILL_PAD * 2.f;

            dl->AddRectFilled(ImVec2(pillX, pillY), ImVec2(pillX + pillW, pillY + pillH),
                IM_COL32(0, 0, 0, (uint8_t)(0.55f * 255.f * opacity)), 3.f);
            dl->AddText(font, labelFs,
                ImVec2(pillX + PILL_PAD, fy - tsz.y * 0.5f), labelCol, e.label.c_str());
            continue;
        }

        float startX = fx - totalW * 0.5f;
        float curX = startX;

        if (hasIcon)
        {
            ImTextureID tex = nullptr;
            if (isBasicAttack)
                tex = LoadFlagIcon(dev, "kill.png");
            else
                tex = LoadSkillIcon(this, dev, e.skillId, m_skillIconIndex, m_skillIconCache);
            ImVec2 iconTL(curX, fy - iconW * 0.5f);
            ImVec2 iconBR(curX + iconW, fy + iconW * 0.5f);
            if (tex)
                dl->AddImage(tex, iconTL, iconBR,
                    ImVec2(0,0), ImVec2(1,1), IM_COL32(255, 255, 255, alpha));
            curX += iconW + GAP;
        }

        if (e.label.empty()) continue;

        float labelCenterX = curX + labelW * 0.5f;
        bmFont->DrawString(dl, e.label.c_str(), labelCenterX, fy, glyphHeight, alpha);
    }
}


// ---------------------------------------------------------------------------
// Shout speech bubbles — update + render
// ---------------------------------------------------------------------------

void ReplayWindow::UpdateSpeechBubbles()
{
    float now = m_debugTimeline;

    if (now < m_lastShoutScanTime - 0.5f)
    {
        m_speechBubbles.clear();
        m_shoutScanCursor.clear();
        m_lastShoutScanTime = now;
        return;
    }

    float scanFrom = m_lastShoutScanTime;
    m_lastShoutScanTime = now;
    if (now <= scanFrom) return;

    for (auto it = m_speechBubbles.begin(); it != m_speechBubbles.end(); )
    {
        if ((now - it->second.spawnTime) >= kSpeechBubbleLifetime)
            it = m_speechBubbles.erase(it);
        else
            ++it;
    }

    const auto& db = m_skillView;
    if (!db.IsLoaded()) return;

    constexpr int kShoutType = 20;

    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Player && ard.type != AgentType::NPC) continue;
        if (ard.skillUseHistory.empty()) continue;

        size_t& cursor = m_shoutScanCursor[agentId];
        if (cursor >= ard.skillUseHistory.size()) continue;

        for (; cursor < ard.skillUseHistory.size(); ++cursor)
        {
            const auto& ev = ard.skillUseHistory[cursor];
            if (ev.startTime > now) break;
            if (ev.startTime <= scanFrom) continue;

            const SkillInfo* si = db.Get(ev.skillId);
            if (!si || si->type != kShoutType) continue;

            SpeechBubble sb;
            sb.agentId   = agentId;
            sb.skillId   = ev.skillId;
            sb.text      = StripPvpSuffix(si->name);
            sb.spawnTime = ev.startTime;
            m_speechBubbles[agentId] = std::move(sb);
        }
    }
}


void ReplayWindow::RenderSpeechBubbles()
{
    if (m_speechBubbles.empty()) return;
    if (!m_showAgentOverlay) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;

    XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    auto* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x, vpH = vp->Size.y;
    float now = m_debugTimeline;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImTextureID bubbleTex = LoadSpeechBubbleTexture(dev);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();

    const InterpolationSettings& is = m_replayCtx.interpSettings;
    const MapTransform& mt = m_replayCtx.mapTransform;

    for (auto& [agentId, sb] : m_speechBubbles)
    {
        float age = now - sb.spawnTime;
        if (age < 0.f || age >= kSpeechBubbleLifetime) continue;

        auto ait = m_replayCtx.agents.find(agentId);
        if (ait == m_replayCtx.agents.end()) continue;
        const auto& ard = ait->second;
        if (ard.snapshots.empty()) continue;

        float sx, sy, sz;
        InterpolateAgentPosition(ard, now, is, sx, sy, sz);
        XMFLOAT3 worldPos = ApplyMapTransformToPos(sx, sy, sz, mt);

        float modelTopY = AgentModelTopY(agentId, ard, worldPos.y, now);
        if (modelTopY <= worldPos.y)
            modelTopY = worldPos.y + 120.f;

        XMFLOAT3 topPos = { worldPos.x, modelTopY, worldPos.z };
        float anchorX, anchorY;
        if (!ProjectToScreen(viewProj, vpW, vpH, topPos, anchorX, anchorY)) continue;

        float iconSz = std::clamp(vpH * 0.020f, 12.f, 20.f);
        constexpr float kScreenPad = 4.f;
        anchorY -= iconSz + kScreenPad;

        float t = age / kSpeechBubbleLifetime;
        float opacity = (t < 0.65f) ? 1.f : 1.f - ((t - 0.65f) / 0.35f);
        opacity = std::clamp(opacity, 0.f, 1.f);
        uint8_t alpha = static_cast<uint8_t>(opacity * 255.f);

        float fontSize = std::clamp(vpH * 0.011f, 9.f, 13.f);
        ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, sb.text.c_str());

        float padH = fontSize * 0.6f;
        float padV = fontSize * 0.35f;
        float bubbleW = textSize.x + padH * 2.f;
        float bubbleH = textSize.y + padV * 2.f;
        float tailH   = fontSize * 0.45f;

        float bx = anchorX - bubbleW * 0.5f;
        float by = anchorY - bubbleH - tailH;

        if (bubbleTex)
        {
            ImVec2 bubbleTL(bx, by);
            ImVec2 bubbleBR(bx + bubbleW, anchorY);
            dl->AddImage(bubbleTex, bubbleTL, bubbleBR,
                         ImVec2(0, 0), ImVec2(1, 1),
                         IM_COL32(255, 255, 255, alpha));
        }
        else
        {
            ImVec2 rectTL(bx, by);
            ImVec2 rectBR(bx + bubbleW, by + bubbleH);
            dl->AddRectFilled(rectTL, rectBR, IM_COL32(20, 20, 20, (uint8_t)(0.85f * alpha)), 3.f);
            dl->AddRect(rectTL, rectBR, IM_COL32(200, 200, 200, alpha), 3.f, 0, 1.f);

            float triCx = anchorX;
            float triTop = by + bubbleH;
            dl->AddTriangleFilled(
                ImVec2(triCx - tailH * 0.4f, triTop),
                ImVec2(triCx + tailH * 0.4f, triTop),
                ImVec2(triCx, triTop + tailH),
                IM_COL32(20, 20, 20, (uint8_t)(0.85f * alpha)));
        }

        float textX = bx + (bubbleW - textSize.x) * 0.5f;
        float textY = by + (bubbleH - textSize.y) * 0.5f;
        dl->AddText(font, fontSize, ImVec2(textX, textY),
                    IM_COL32(255, 255, 255, alpha), sb.text.c_str());
    }
}


// ---------------------------------------------------------------------------
// Top-of-screen HUD for the followed agent: health bar (with name inside)
// and current skill cast bar.
// ---------------------------------------------------------------------------

void ReplayWindow::DrawFollowedAgentHUD()
{
    int focused = GetFocusedAgentId();
    if (focused < 0) return;

    auto it = m_replayCtx.agents.find(focused);
    if (it == m_replayCtx.agents.end()) return;
    const auto& ard = it->second;
    if (ard.snapshots.empty()) return;

    const AgentSnapshot* snap = FindSnapshotAtTime(ard, m_debugTimeline);
    if (!snap) return;

    auto* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();

    // --- Panel background (only around health bar) ---
    constexpr float BAR_W   = 320.f;
    constexpr float BAR_H   = 26.f;
    constexpr float PAD     = 6.f;
    constexpr float TOP_Y   = 36.f;   // below the menu bar
    constexpr float PANEL_R = 5.f;
    constexpr float RIBBON_GAP = 6.f; // breathing room under the ribbon strip

    float panelW = BAR_W + PAD * 2.f;
    float panelH = PAD + BAR_H + PAD;
    float panelX = (vpW - panelW) * 0.5f;

    // Slide clear of the ribbon toolbar instead of being drawn over it. The
    // ribbon republishes its animated bottom edge each frame, so the bar rides
    // the collapse/expand tween; once the strip is collapsed (or closed) the
    // edge falls above TOP_Y and the bar settles back to its usual place.
    float baseY = std::max(TOP_Y, m_ribbonBottomY + RIBBON_GAP);

    // The drawing strip is a movable window and by default sits exactly where the
    // bar rests under an expanded ribbon, so drop below it when it covers the bar.
    // Tested against baseY, not the animated position, so clearing the strip
    // cannot make the test flip back and leave the bar oscillating.
    float dropTarget = 0.f;
    const auto strip = m_annotationMgr.ToolbarRect();
    if (strip.valid() &&
        strip.x1 > panelX && strip.x0 < panelX + panelW &&
        strip.y1 > baseY  && strip.y0 < baseY + panelH)
    {
        dropTarget = strip.y1 + RIBBON_GAP - baseY;
    }

    // Same exponential tween the ribbon uses for its own reveal, so the bar moves
    // at one speed whichever strip it is dodging.
    constexpr float DROP_RATE = 12.f;
    const int  hudFrame  = ImGui::GetFrameCount();
    const bool justShown = (m_followedHudLastFrame != hudFrame - 1);
    m_followedHudLastFrame = hudFrame;

    if (justShown)
    {
        m_followedHudDropY = dropTarget;
    }
    else
    {
        const float dt = ImGui::GetIO().DeltaTime;
        m_followedHudDropY += (dropTarget - m_followedHudDropY) *
                              std::min(1.f, dt * DROP_RATE);
        if (std::abs(dropTarget - m_followedHudDropY) < 0.5f)
            m_followedHudDropY = dropTarget;
    }

    float panelY = baseY + m_followedHudDropY;

    bool isDead = snap->is_dead;
    float healthPct = std::clamp(snap->health_pct, 0.f, 1.f);
    uint8_t teamId = (uint8_t)ard.teamId;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    PartyIcons hudIcons = LoadAllPartyIcons(dev);

    ImTextureID carriedFlagTex = CarriedBundleIcon(dev, focused);

    ImVec2 panelTL(panelX, panelY);
    ImVec2 panelBR(panelX + panelW, panelY + panelH);

    ImU32 panelBg = (teamId == 1)
        ? IM_COL32(0x28, 0x0A, 0x0A, 0xA0)   // dark red, more transparent
        : IM_COL32(0x0A, 0x12, 0x28, 0xA0);   // dark blue, more transparent
    dl->AddRectFilled(panelTL, panelBR, panelBg, PANEL_R);
    dl->AddRect(panelTL, panelBR, IM_COL32(0xA0, 0xA0, 0xA0, 0x90), PANEL_R, 0, 1.0f);

    // Health bar inside the panel
    float barX = panelX + PAD;
    float barY = panelY + PAD;
    ImVec2 barTL(barX, barY);
    ImVec2 barBR(barX + BAR_W, barY + BAR_H);

    auto sv = ard.skillVisualAtTime(m_debugTimeline);
    bool showCast = (sv.skillId > 0 && sv.alpha > 0.f);
    constexpr float CAST_ICON  = 34.f;
    constexpr float CAST_GAP   = 5.f;
    constexpr float CAST_BAR_H = 18.f;

    const Gradient5* deadGrad = (teamId == 1) ? &kDeadRed : &kDeadBlue;
    const Gradient5* fillGrad = nullptr;
    if (isDead)
        fillGrad = deadGrad;
    else if (snap->has_degen_hex)
        fillGrad = &kDegenHex;
    else if (snap->has_poison)
        fillGrad = &kPoison;
    else if (snap->has_bleeding)
        fillGrad = &kBleeding;
    else
        fillGrad = (teamId == 1) ? &kAliveRed : &kAliveBlue;

    ImVec2 innerTL(barTL.x + 1, barTL.y + 1);
    ImVec2 innerBR(barBR.x - 1, barBR.y - 1);
    float innerW = innerBR.x - innerTL.x;

    dl->AddRectFilled(barTL, barBR, IM_COL32(0, 0, 0, 0xFF), 2.f);

    if (isDead)
    {
        DrawGradientRect(dl, innerTL, innerBR, *fillGrad);
    }
    else
    {
        DrawGradientRect(dl, innerTL, innerBR, *deadGrad);
        if (healthPct > 0.f)
        {
            bool dw = snap->has_deep_wound && !isDead;
            float fp = dw ? std::min(healthPct, 0.80f) : healthPct;
            DrawGradientRect(dl, innerTL, ImVec2(innerTL.x + innerW * fp, innerBR.y), *fillGrad);
            if (dw)
            {
                float dwX = innerTL.x + innerW * 0.80f;
                DrawGradientRect(dl, ImVec2(dwX, innerTL.y), innerBR, kDeepWound);
            }
        }
    }

    dl->AddRect(barTL, barBR, IM_COL32(0x50, 0x50, 0x50, 0xFF), 2.f);

    // Agent name inside the health bar
    const std::string& nameLabel = ard.partyBarLabel.empty() ? GetAgentLabel(ard) : ard.partyBarLabel;

    ImVec2 nameSz = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, nameLabel.c_str());
    float nameX = barTL.x + 5.f;
    float nameY = barTL.y + (BAR_H - nameSz.y) * 0.5f;
    dl->AddText(ImVec2(nameX + 1.f, nameY + 1.f), IM_COL32(0, 0, 0, 0xCC), nameLabel.c_str());
    ImU32 nameCol = isDead ? IM_COL32(0x80, 0x80, 0x80, 0xFF) : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
    dl->AddText(ImVec2(nameX, nameY), nameCol, nameLabel.c_str());

    // Status effect icons (right-aligned, hidden when dead) — matches party window
    if (!isDead)
    {
        float innerH = innerBR.y - innerTL.y;
        const float iconSz = std::min(innerH - 2.f, 18.f);
        float iconX = innerBR.x - 2.f;
        float iconY = innerTL.y + (innerH - iconSz) * 0.5f;

        if (snap->has_weapon_spell && hudIcons.weaponSpell)
        {
            iconX -= iconSz;
            dl->AddImage(hudIcons.weaponSpell, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if (snap->has_enchantment && hudIcons.enchanted)
        {
            iconX -= iconSz;
            dl->AddImage(hudIcons.enchanted, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if ((snap->has_condition || snap->has_deep_wound || snap->has_bleeding || snap->has_poison) && hudIcons.condition)
        {
            iconX -= iconSz;
            dl->AddImage(hudIcons.condition, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if (snap->has_hex && hudIcons.hexed)
        {
            iconX -= iconSz;
            dl->AddImage(hudIcons.hexed, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if (carriedFlagTex)
        {
            iconX -= iconSz;
            dl->AddImage(carriedFlagTex, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
        }
    }

    // --- Current skill cast bar (icon left-aligned under health bar) ---
    if (showCast)
    {
        const auto& db = m_skillView;
        const SkillInfo* si = db.Get(sv.skillId);
        uint8_t castAlpha = (uint8_t)(sv.alpha * 255.f);

        float castY = panelBR.y + CAST_GAP;
        float castBarW = BAR_W - CAST_ICON - CAST_GAP;

        EnsureSkillIconIndex();
        ImTextureID skillTex = LoadSkillIcon(this, dev, sv.skillId,
                                             m_skillIconIndex, m_skillIconCache);
        if (skillTex)
        {
            ImVec2 icoTL(barX, castY);
            ImVec2 icoBR(barX + CAST_ICON, castY + CAST_ICON);
            dl->AddImage(skillTex, icoTL, icoBR,
                ImVec2(0,0), ImVec2(1,1), IM_COL32(255, 255, 255, castAlpha));
        }

        float cbX = barX + CAST_ICON + CAST_GAP;
        float cbY = castY + (CAST_ICON - CAST_BAR_H) * 0.5f;
        constexpr float CB_R = 6.f;
        ImVec2 cbTL(cbX, cbY);
        ImVec2 cbBR(cbX + castBarW, cbY + CAST_BAR_H);
        float midY = cbTL.y + CAST_BAR_H * 0.5f;

        dl->AddRectFilled(cbTL, cbBR, IM_COL32(0x0C, 0x0C, 0x0C, castAlpha), CB_R);

        float pct = sv.progress;
        float fillW = castBarW * pct;
        if (pct > 0.005f)
        {
            static const GradStop sGreenH[] = {
                { 0.000f,  10, 10, 10 }, { 0.200f,  26, 58, 10 },
                { 0.400f,  64,176, 32 }, { 0.600f, 168,240, 80 },
                { 0.800f, 200,255,112 }, { 1.000f, 144,224, 64 }
            };
            static const GradStop sOrangeH[] = {
                { 0.000f,  10,  8,  0 }, { 0.143f,  58, 30,  0 },
                { 0.286f, 122, 58,  0 }, { 0.429f, 192, 96,  0 },
                { 0.571f, 232,144, 16 }, { 0.714f, 255,184, 32 },
                { 0.857f, 255,208, 64 }, { 1.000f, 232,160, 16 }
            };
            static const GradStop sPurpleH[] = {
                { 0.000f,  10, 10, 10 }, { 0.300f, 120, 32,192 },
                { 0.600f, 224,160,255 }, { 1.000f, 160, 80,224 }
            };
            const GradStop* hS;
            int nH;
            float topV, botV;
            if (sv.interrupted) {
                hS = sPurpleH; nH = 4; topV = 0.58f; botV = 0.52f;
            } else if (sv.cancelled) {
                hS = sOrangeH; nH = 8; topV = 0.58f; botV = 0.52f;
            } else {
                hS = sGreenH; nH = 6; topV = 0.55f; botV = 0.50f;
            }

            // Clip gradient to inset rect so it doesn't overflow rounded corners
            ImVec2 inTL(cbTL.x + CB_R, cbTL.y + 1.f);
            ImVec2 inBR(cbBR.x - CB_R, cbBR.y - 1.f);
            float clipRight = std::min(cbTL.x + fillW, inBR.x);

            dl->PushClipRect(inTL, ImVec2(clipRight, inBR.y), true);
            int nSegs = std::clamp((int)(fillW / 3.f), 4, 24);
            for (int seg = 0; seg < nSegs; ++seg)
            {
                float u0 = (float)seg / nSegs;
                float u1 = (float)(seg + 1) / nSegs;
                float r0, g0, b0, r1, g1, b1;
                SampleGradient(hS, nH, u0 * pct, r0, g0, b0);
                SampleGradient(hS, nH, u1 * pct, r1, g1, b1);
                float x0 = cbTL.x + fillW * u0;
                float x1 = cbTL.x + fillW * u1;

                auto vig = [&](float rv, float gv, float bv, float d) -> ImU32 {
                    float m = 1.f - d;
                    return IM_COL32((ImU8)(rv * m), (ImU8)(gv * m), (ImU8)(bv * m), castAlpha);
                };
                ImU32 tl = vig(r0,g0,b0, topV);
                ImU32 tr = vig(r1,g1,b1, topV);
                ImU32 ml = IM_COL32((ImU8)r0,(ImU8)g0,(ImU8)b0, castAlpha);
                ImU32 mr = IM_COL32((ImU8)r1,(ImU8)g1,(ImU8)b1, castAlpha);
                ImU32 bl = vig(r0,g0,b0, botV);
                ImU32 br = vig(r1,g1,b1, botV);

                dl->AddRectFilledMultiColor(ImVec2(x0, cbTL.y), ImVec2(x1, midY), tl, tr, mr, ml);
                dl->AddRectFilledMultiColor(ImVec2(x0, midY), ImVec2(x1, cbBR.y), ml, mr, br, bl);
            }
            dl->PopClipRect();

            // Leading edge glow (also clipped)
            float fillX = cbTL.x + fillW;
            if (pct > 0.01f && fillX > inTL.x && fillX < inBR.x)
            {
                ImU8 glA1 = (ImU8)(140 * sv.alpha);
                ImU8 glA2 = (ImU8)( 60 * sv.alpha);
                ImU32 gc1, gc2;
                if (sv.interrupted) {
                    gc1 = IM_COL32(128, 48,192, glA1); gc2 = IM_COL32(128, 48,192, glA2);
                } else if (sv.cancelled) {
                    gc1 = IM_COL32(192,120,  0, glA1); gc2 = IM_COL32(192,120,  0, glA2);
                } else {
                    gc1 = IM_COL32( 96,208, 32, glA1); gc2 = IM_COL32( 96,208, 32, glA2);
                }
                dl->PushClipRect(inTL, inBR, true);
                dl->AddRectFilled(ImVec2(fillX - 3.f, cbTL.y), ImVec2(fillX + 3.f, cbBR.y), gc1);
                dl->AddRectFilled(ImVec2(fillX - 5.f, cbTL.y - 1.f), ImVec2(fillX + 5.f, cbBR.y + 1.f), gc2);
                dl->PopClipRect();
            }
        }

        dl->AddRect(cbTL, cbBR, IM_COL32(0x60, 0x60, 0x60, castAlpha), CB_R);

        if (si && !si->name.empty())
        {
            float fs = 11.f;
            ImVec2 snSz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, si->name.c_str());
            float snX = cbTL.x + (castBarW - snSz.x) * 0.5f;
            float snY = cbTL.y + (CAST_BAR_H - snSz.y) * 0.5f;
            dl->AddText(font, fs, ImVec2(snX + 1.f, snY + 1.f),
                IM_COL32(0, 0, 0, castAlpha), si->name.c_str());
            dl->AddText(font, fs, ImVec2(snX, snY),
                IM_COL32(0xFF, 0xFF, 0xFF, castAlpha), si->name.c_str());
        }
    }
}
