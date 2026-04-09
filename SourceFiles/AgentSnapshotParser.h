#pragma once
#include "ReplayMapData.h"
#include "ReplayLibrary.h"
#include <filesystem>
#include <memory>
#include <thread>
#include <unordered_set>

// Launches async parsing of all agent snapshot files inside matchFolder/Agents/.
// Results are accumulated in the shared AgentParseProgress and later moved
// into the ReplayContext when finished.
void LaunchAgentSnapshotParsing(const std::filesystem::path& matchFolder,
                                std::shared_ptr<AgentParseProgress> progress);

// Call once per frame. Returns true when parsing is done and results have been
// transferred into ctx.agents.
bool PollAgentParseCompletion(ReplayContext& ctx);

// After agents are loaded, match them against the metadata from infos.json
// and classify each agent as Player / NPC / Gadget / Flag / Unknown.
void ClassifyAgents(std::unordered_map<int, AgentReplayData>& agents,
                    const MatchMeta& meta, int mapId);

// Split agent files that contain mixed incarnations due to agent ID recycling.
// Uses lifecycle AGENT_ADD/AGENT_REMOVE events to define incarnation windows.
// Falls back to detecting item_id/model_id changes when lifecycle data is absent.
// New incarnations get synthetic IDs starting at kSyntheticIdBase.
void SplitRecycledAgents(std::unordered_map<int, AgentReplayData>& agents,
                         const std::vector<LifecycleEvent>& lifecycle,
                         const std::unordered_set<int>* skipAgentIds = nullptr);
