// Standalone identity regression test: compile with SkillDatabase.cpp without
// its pch include. The classifier is stubbed because these checks concern skill
// identity, patch loading and sorting, not description scale extraction.
#include "SkillDatabase.h"
#include "HistoricalSkillIds.h"
#include "json.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <fstream>

void SkillDatabase::ParseScalesFromDescription(SkillInfo&) {}
void SkillDatabase::ClassifyDeductionUsability(SkillInfo&) {}

int main(int argc, char** argv)
{
    assert(argc == 2);
    SkillDatabase db;
    assert(db.Load(argv[1]));
    db.LoadPatches(argv[1]);

    // The exact bars reported in the August 2 BcS vs Free match.
    const std::vector<int> warrior{869, 1547, 1550, 1603, 349, 1404, 1141, 3458};
    const std::vector<int> mesmer{3466, 49, 24, 42, 68, 932, 1057};
    const auto old = db.GetView(2026, 8, 2);
    auto elites = [](const auto& view, const auto& bar) {
        int count = 0;
        for (int id : bar)
            if (const auto* skill = view.Get(id); skill && skill->is_elite) ++count;
        return count;
    };
    assert(elites(old, warrior) == 1);
    assert(elites(old, mesmer) == 1);
    assert(old.Get(869)->name == "\"Coward!\"");
    assert(old.Get(1057)->name == "Psychic Instability (PvP)");
    assert(!old.Get(3458) && !old.Get(3466));
    assert(old.IsUnresolvedHistoricalId(3458));
    assert(old.ResolvePvpSkillId(3458) == 3458); // preserve the evidence
    auto sorted = old.SortSkillsForDisplay(warrior, 1, 9);
    assert(sorted.front() == 869);
    assert(sorted.back() == 3458);
    assert(sorted.size() == warrior.size()); // unknown slots are not discarded

    // Both fresh and cached views must retain their era.
    assert(!db.GetView(2026, 8, 2).Get(3466));
    assert(!db.GetView(2026, 4, 2).Get(3460));
    assert(!db.GetView(2026, 8, 25).Get(3458));
    assert(!db.GetView(0, 0, 0).Get(3458));
    assert(!old.IsUnresolvedHistoricalId(3431));
    assert(old.IsUnresolvedHistoricalId(3495)); // absent from today's table too
    old.ForEachSkill([](const SkillInfo& skill) { assert(skill.id <= 3431); });

    // Actual post-update skills and undated catalogue lookups still work.
    const auto current = db.GetView(2026, 8, 26);
    assert(current.Get(3458)->name == "Primal Rage (PvP)");
    assert(current.Get(3466)->name == "Infuriating Heat (PvP)");
    assert(current.Get(3458)->is_elite && current.Get(3466)->is_elite);
    assert(current.ResolvePvpSkillId(831) == 3458);
    assert(!current.IsUnresolvedHistoricalId(3458));
    assert(db.Get(3458) && db.GetBaseView().Get(3458));
    assert(!db.GetView(2026, 8, 2).Get(3458)); // current lookup must not taint old view

    // The skill-filter eligibility rule must exclude the collision before
    // canonicalisation, even if a modern match makes the same ID selectable.
    assert(IsUnresolvedHistoricalSkillId(3458, 20260802));
    assert(!IsUnresolvedHistoricalSkillId(3458, 20260826));
    assert(!IsUnresolvedHistoricalSkillId(869, 20260802));

    auto repairedWarrior = warrior;
    auto repairedMesmer = mesmer;
    assert(RepairHistoricalSkillBar(repairedWarrior, 20260802));
    assert(RepairHistoricalSkillBar(repairedMesmer, 20260802));
    assert(repairedWarrior.back() == 2);
    assert(repairedMesmer.front() == 69);
    assert(elites(old, repairedWarrior) == 1 && elites(old, repairedMesmer) == 1);
    for (int id : repairedWarrior) assert(old.Get(id));
    for (int id : repairedMesmer) assert(old.Get(id));
    assert(!RepairHistoricalSkillBar(repairedWarrior, 20260802)); // idempotent
    auto modernWarrior = warrior;
    assert(!RepairHistoricalSkillBar(modernWarrior, 20260826));
    assert(modernWarrior == warrior);
    assert(ResolveHistoricalBarSkillId(3448, 20260501) == 2);
    assert(ResolveHistoricalBarSkillId(3448, 20260607) == 13);
    assert(ResolveHistoricalBarSkillId(3492, 20260412) == 16);
    assert(ResolveHistoricalBarSkillId(3492, 20260630) == 1);
    assert(ResolveHistoricalBarSkillId(3458, 0) == 3458);
    assert(ResolveHistoricalBarSkillId(3448, 20260520) == 3448); // unverified gap
    std::vector<int> duplicate{869, 2, 3458};
    assert(RepairHistoricalSkillBar(duplicate, 20260802));
    assert((duplicate == std::vector<int>{869, 2}));
    assert(old.Get(3442)->id == 1547); // legitimate historical PvP event
    assert(!old.IsUnresolvedHistoricalId(3442));
    assert(current.Get(3442)->id == 3442);

    // Every sampled player's repaired IDs must exactly account for the IDs
    // missing from their bar but present in their original cast events.
    std::ifstream evidenceFile(std::string(argv[1]) + "/../scripts/data/historical_skill_id_evidence.json");
    assert(evidenceFile);
    const auto evidence = nlohmann::json::parse(evidenceFile);
    for (const auto& row : evidence)
    {
        const std::string match = row.at("match");
        const int date = std::stoi(match.substr(0, 4)) * 10000
            + std::stoi(match.substr(5, 2)) * 100 + std::stoi(match.substr(8, 2));
        std::vector<int> recovered, expected;
        for (int id : row.at("unresolved"))
            recovered.push_back(ResolveHistoricalBarSkillId(id, date));
        for (const auto& [key, count] : row.at("candidates").items())
            expected.push_back(std::stoi(key));
        std::sort(recovered.begin(), recovered.end());
        std::sort(expected.begin(), expected.end());
        assert(recovered == expected);
    }
    std::cout << "Historical skill identity regression checks passed\n";
}
