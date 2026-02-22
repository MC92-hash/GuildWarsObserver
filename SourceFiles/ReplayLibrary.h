#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <filesystem>

struct CapeData
{
    int bg_color = 0;
    int detail_color = 0;
    int emblem_color = 0;
    int shape = 0;
    int detail = 0;
    int emblem = 0;
    int trim = 0;
};

struct GuildMeta
{
    int id = 0;
    std::string name;
    std::string tag;
    int rank = 0;
    int features = 0;
    int rating = 0;
    int faction = 0;
    int faction_points = 0;
    int qualifier_points = 0;
    CapeData cape;
};

struct PlayerMeta
{
    int id = 0;
    int primary = 0;
    int secondary = 0;
    int level = 0;
    int team_id = 0;
    int player_number = 0;
    int guild_id = 0;
    int model_id = 0;
    int gadget_id = 0;
    std::string encoded_name;
    int total_damage = 0;
    int attacks_started = 0;
    int attacks_finished = 0;
    int attacks_stopped = 0;
    int skills_activated = 0;
    int skills_finished = 0;
    int skills_stopped = 0;
    int attack_skills_activated = 0;
    int attack_skills_finished = 0;
    int attack_skills_stopped = 0;
    int interrupted_count = 0;
    int interrupted_skills_count = 0;
    int cancelled_attacks_count = 0;
    int cancelled_skills_count = 0;
    int crits_dealt = 0;
    int crits_received = 0;
    int deaths = 0;
    int kills = 0;
    std::string skill_template_code;
    std::vector<int> used_skills;
};

struct PartyMeta
{
    std::vector<PlayerMeta> players;
    std::vector<PlayerMeta> others;
};

struct MatchMeta
{
    int map_id = 0;
    std::string flux;
    int day = 0;
    int month = 0;
    int year = 0;
    std::string occasion;
    std::string match_duration;
    std::string match_original_duration;
    int match_end_time_ms = 0;
    std::string match_end_time_formatted;
    int winner_party_id = 0;
    std::map<std::string, int> team_kills;
    std::map<std::string, int> team_damage;
    std::map<std::string, PartyMeta> parties;
    std::map<std::string, GuildMeta> guilds;

    std::string folder_name;
    std::string folder_path;
};

class IReplayProvider
{
public:
    virtual ~IReplayProvider() = default;
    virtual std::vector<MatchMeta> GetAvailableReplays() = 0;
};

class LocalReplayProvider : public IReplayProvider
{
public:
    void SetFolder(const std::string& path);
    std::vector<MatchMeta> GetAvailableReplays() override;

private:
    std::string m_folder_path;
    static bool ParseInfosJson(const std::filesystem::path& jsonPath, MatchMeta& out);
    static void ParsePlayerArray(const void* jsonArray, std::vector<PlayerMeta>& out);
};

class ReplayLibrary
{
public:
    void SetMatchDataFolder(const std::string& path);
    void ScanFolder();
    void Clear();

    const std::vector<MatchMeta>& GetMatches() const { return m_matches; }
    bool IsLoaded() const { return m_loaded; }
    const std::string& GetFolderPath() const { return m_folder_path; }
    int GetMatchCount() const { return static_cast<int>(m_matches.size()); }

private:
    LocalReplayProvider m_provider;
    std::vector<MatchMeta> m_matches;
    std::string m_folder_path;
    bool m_loaded = false;
};
