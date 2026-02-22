#include "pch.h"
#include "MatchReplay.h"
#include <json.hpp>
#include <fstream>
#include <algorithm>

using namespace DirectX;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Color mapping (shared with AgentOverlay)
// ---------------------------------------------------------------------------

XMFLOAT4 MatchReplay::TeamNameToColor(const std::string& team)
{
    if (team == "red")    return { 1.0f, 0.2f, 0.2f, 0.85f };
    if (team == "blue")   return { 0.2f, 0.4f, 1.0f, 0.85f };
    if (team == "green")  return { 0.2f, 0.9f, 0.3f, 0.85f };
    if (team == "yellow") return { 1.0f, 0.9f, 0.1f, 0.85f };
    if (team == "white")  return { 1.0f, 1.0f, 1.0f, 0.85f };
    if (team == "purple") return { 0.7f, 0.2f, 0.9f, 0.85f };
    if (team == "orange") return { 1.0f, 0.5f, 0.0f, 0.85f };
    if (team == "cyan")   return { 0.0f, 0.9f, 0.9f, 0.85f };
    return { 1.0f, 1.0f, 1.0f, 0.85f };
}

// ---------------------------------------------------------------------------
// JSON loading
// Format:
// {
//   "duration": 120.0,
//   "frames": [
//     { "time": 0.0,  "agents": [ { "id":1, "x":..., "y":..., "z":..., "team":"red" }, ... ] },
//     { "time": 1.0,  "agents": [ ... ] },
//     ...
//   ]
// }
// ---------------------------------------------------------------------------

bool MatchReplay::LoadFromFile(const std::filesystem::path& path)
{
    Unload();

    if (!std::filesystem::exists(path))
        return false;

    try
    {
        std::ifstream file(path);
        if (!file.is_open()) return false;

        json root = json::parse(file);

        m_duration = root.value("duration", 0.0f);

        if (!root.contains("frames") || !root["frames"].is_array())
            return false;

        for (auto& frameJson : root["frames"])
        {
            ReplayKeyframe kf;
            kf.time = frameJson.value("time", 0.0f);

            if (frameJson.contains("agents") && frameJson["agents"].is_array())
            {
                for (auto& agentJson : frameJson["agents"])
                {
                    ReplayAgent agent;
                    agent.id = agentJson.value("id", 0);
                    agent.x  = agentJson.value("x", 0.0f);
                    agent.y  = agentJson.value("y", 0.0f);
                    agent.z  = agentJson.value("z", 0.0f);

                    if (agentJson.contains("team"))
                        agent.color = TeamNameToColor(agentJson["team"].get<std::string>());
                    else
                    {
                        agent.color.x = agentJson.value("r", 1.0f);
                        agent.color.y = agentJson.value("g", 1.0f);
                        agent.color.z = agentJson.value("b", 1.0f);
                        agent.color.w = agentJson.value("a", 0.85f);
                    }
                    kf.agents.push_back(agent);
                }
            }
            m_keyframes.push_back(std::move(kf));
        }

        // Sort keyframes by time
        std::sort(m_keyframes.begin(), m_keyframes.end(),
            [](const ReplayKeyframe& a, const ReplayKeyframe& b) { return a.time < b.time; });

        // Auto-detect duration from last keyframe if not explicitly set
        if (m_duration <= 0.f && !m_keyframes.empty())
            m_duration = m_keyframes.back().time;

        m_loaded = true;
        m_currentTime = 0.f;
        m_state = PlaybackState::Paused;
        Interpolate();
        return true;
    }
    catch (const std::exception& e)
    {
        OutputDebugStringA("MatchReplay: JSON parse error: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        return false;
    }
}

void MatchReplay::Unload()
{
    m_keyframes.clear();
    m_interpolatedAgents.clear();
    m_duration = 0.f;
    m_currentTime = 0.f;
    m_speed = 1.0f;
    m_loaded = false;
    m_state = PlaybackState::Stopped;
}

// ---------------------------------------------------------------------------
// Playback controls
// ---------------------------------------------------------------------------

void MatchReplay::Play()  { if (m_loaded) m_state = PlaybackState::Playing; }
void MatchReplay::Pause() { if (m_loaded) m_state = PlaybackState::Paused; }
void MatchReplay::Stop()  { m_state = PlaybackState::Stopped; m_currentTime = 0.f; Interpolate(); }

void MatchReplay::TogglePlayPause()
{
    if (m_state == PlaybackState::Playing) Pause();
    else Play();
}

void MatchReplay::SetTime(float t)
{
    m_currentTime = std::clamp(t, 0.f, m_duration);
    Interpolate();
}

void MatchReplay::StepForward(float seconds)
{
    SetTime(m_currentTime + seconds);
}

void MatchReplay::StepBackward(float seconds)
{
    SetTime(m_currentTime - seconds);
}

// ---------------------------------------------------------------------------
// Update (call each frame with delta time in seconds)
// ---------------------------------------------------------------------------

void MatchReplay::Update(float deltaSeconds)
{
    if (!m_loaded || m_state != PlaybackState::Playing)
        return;

    m_currentTime += deltaSeconds * m_speed;

    if (m_currentTime >= m_duration)
    {
        if (m_looping)
            m_currentTime = fmodf(m_currentTime, m_duration);
        else
        {
            m_currentTime = m_duration;
            m_state = PlaybackState::Paused;
        }
    }

    if (m_currentTime < 0.f)
        m_currentTime = 0.f;

    Interpolate();
}

// ---------------------------------------------------------------------------
// Interpolation between keyframes
// ---------------------------------------------------------------------------

void MatchReplay::Interpolate()
{
    m_interpolatedAgents.clear();

    if (m_keyframes.empty()) return;

    // Find the two keyframes surrounding m_currentTime
    const ReplayKeyframe* kfBefore = nullptr;
    const ReplayKeyframe* kfAfter = nullptr;

    for (size_t i = 0; i < m_keyframes.size(); ++i)
    {
        if (m_keyframes[i].time <= m_currentTime)
            kfBefore = &m_keyframes[i];
        if (m_keyframes[i].time >= m_currentTime)
        {
            kfAfter = &m_keyframes[i];
            break;
        }
    }

    // Edge cases: before first keyframe or after last
    if (!kfBefore && kfAfter) { m_interpolatedAgents = kfAfter->agents; return; }
    if (kfBefore && !kfAfter) { m_interpolatedAgents = kfBefore->agents; return; }
    if (!kfBefore && !kfAfter) return;

    // Same keyframe (or exact match)
    if (kfBefore == kfAfter || kfBefore->time == kfAfter->time)
    {
        m_interpolatedAgents = kfBefore->agents;
        return;
    }

    // Lerp factor
    float t = (m_currentTime - kfBefore->time) / (kfAfter->time - kfBefore->time);
    t = std::clamp(t, 0.f, 1.f);

    // Build a map from agent ID -> index in kfAfter for fast lookup
    // (for small agent counts, linear search is fine)
    for (const auto& agentBefore : kfBefore->agents)
    {
        ReplayAgent interpolated = agentBefore;

        // Find matching agent in kfAfter by ID
        for (const auto& agentAfter : kfAfter->agents)
        {
            if (agentAfter.id == agentBefore.id)
            {
                interpolated.x = agentBefore.x + (agentAfter.x - agentBefore.x) * t;
                interpolated.y = agentBefore.y + (agentAfter.y - agentBefore.y) * t;
                interpolated.z = agentBefore.z + (agentAfter.z - agentBefore.z) * t;
                // Keep color from the "before" keyframe (snaps at keyframe boundaries)
                break;
            }
        }

        m_interpolatedAgents.push_back(interpolated);
    }
}
