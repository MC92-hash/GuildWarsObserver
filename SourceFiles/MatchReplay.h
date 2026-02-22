#pragma once
#include <vector>
#include <string>
#include <filesystem>
#include <DirectXMath.h>

struct ReplayAgent
{
    int id = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    DirectX::XMFLOAT4 color = { 1.f, 1.f, 1.f, 0.85f };
};

struct ReplayKeyframe
{
    float time = 0.f;
    std::vector<ReplayAgent> agents;
};

enum class PlaybackState { Stopped, Playing, Paused };

class MatchReplay
{
public:
    bool LoadFromFile(const std::filesystem::path& path);
    void Unload();

    void Update(float deltaSeconds);

    void Play();
    void Pause();
    void Stop();
    void TogglePlayPause();
    void SetTime(float t);
    void StepForward(float seconds = 1.0f);
    void StepBackward(float seconds = 1.0f);

    void SetSpeed(float speed) { m_speed = speed; }
    void SetLooping(bool loop) { m_looping = loop; }

    float GetCurrentTime()  const { return m_currentTime; }
    float GetDuration()     const { return m_duration; }
    float GetSpeed()        const { return m_speed; }
    bool  IsLooping()       const { return m_looping; }
    bool  IsLoaded()        const { return m_loaded; }
    PlaybackState GetState() const { return m_state; }

    // Returns interpolated agent positions at the current playback time
    const std::vector<ReplayAgent>& GetCurrentAgents() const { return m_interpolatedAgents; }

private:
    void Interpolate();
    static DirectX::XMFLOAT4 TeamNameToColor(const std::string& team);

    std::vector<ReplayKeyframe> m_keyframes;
    std::vector<ReplayAgent> m_interpolatedAgents;

    float m_duration = 0.f;
    float m_currentTime = 0.f;
    float m_speed = 1.0f;
    bool  m_looping = false;
    bool  m_loaded = false;
    PlaybackState m_state = PlaybackState::Stopped;
};
