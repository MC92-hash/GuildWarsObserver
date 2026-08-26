#include "pch.h"
#include "ReplayLibrary.h"
#include <json.hpp>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <algorithm>

#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

using json = nlohmann::json;

static bool IsStandaloneCommaLine(const std::string& line)
{
    bool foundComma = false;
    for (auto c : line)
    {
        if (c == ' ' || c == '\t' || c == '\r') continue;
        if (c == ',' && !foundComma) { foundComma = true; continue; }
        return false;
    }
    return foundComma;
}

static std::string SanitizeJson(const std::string& raw)
{
    // The recorder produces infos.json with commas on their own lines.
    // Two cases:
    //   1) Previous line already ends with ',' -> standalone comma is duplicate -> remove it
    //   2) Previous line does NOT end with ',' -> standalone comma is a field separator -> append to prev line
    std::vector<std::string> lines;
    std::istringstream stream(raw);
    std::string line;
    while (std::getline(stream, line))
        lines.push_back(line);

    std::string result;
    result.reserve(raw.size());

    for (size_t i = 0; i < lines.size(); i++)
    {
        if (IsStandaloneCommaLine(lines[i]))
        {
            // Check if previous content line already ends with a comma
            // If so, this is a duplicate comma -> skip entirely
            // If not, append comma to the previous line
            if (!result.empty())
            {
                // Find the last non-whitespace char in result before the trailing newline
                size_t pos = result.size();
                while (pos > 0 && (result[pos - 1] == '\n' || result[pos - 1] == '\r'
                    || result[pos - 1] == ' ' || result[pos - 1] == '\t'))
                    pos--;

                if (pos > 0 && result[pos - 1] == ',')
                {
                    // Previous line already has trailing comma -> skip this duplicate
                    continue;
                }
                else
                {
                    // Insert comma after previous content, before the trailing newline
                    result.insert(pos, ",");
                    continue;
                }
            }
            continue;
        }

        result += lines[i];
        result += '\n';
    }

    return result;
}

// --- LocalReplayProvider ---

void LocalReplayProvider::SetFolder(const std::string& path)
{
    m_folder_path = path;
}

void LocalReplayProvider::ParsePlayerArray(const void* jsonArrayPtr, std::vector<PlayerMeta>& out)
{
    const json& arr = *static_cast<const json*>(jsonArrayPtr);
    if (!arr.is_array()) return;

    out.reserve(arr.size());
    for (const auto& p : arr)
    {
        PlayerMeta pm;
        pm.id = p.value("id", 0);
        pm.primary = p.value("primary", 0);
        pm.secondary = p.value("secondary", 0);
        pm.level = p.value("level", 0);
        pm.team_id = p.value("team_id", 0);
        pm.player_number = p.value("player_number", 0);
        pm.guild_id = p.value("guild_id", 0);
        pm.model_id = p.value("model_id", 0);
        pm.gadget_id = p.value("gadget_id", 0);
        pm.encoded_name = p.value("encoded_name", std::string());
        pm.gender = p.value("gender", std::string());
        pm.total_damage = p.value("total_damage", 0);
        pm.total_damage_received = p.value("total_damage_received", 0);
        pm.total_healing_dealt = p.value("total_healing_dealt", 0);
        pm.total_healing_received = p.value("total_healing_received", 0);
        pm.attacks_started = p.value("attacks_started", 0);
        pm.attacks_finished = p.value("attacks_finished", 0);
        pm.attacks_stopped = p.value("attacks_stopped", 0);
        pm.skills_activated = p.value("skills_activated", 0);
        pm.skills_finished = p.value("skills_finished", 0);
        pm.skills_stopped = p.value("skills_stopped", 0);
        pm.attack_skills_activated = p.value("attack_skills_activated", 0);
        pm.attack_skills_finished = p.value("attack_skills_finished", 0);
        pm.attack_skills_stopped = p.value("attack_skills_stopped", 0);
        pm.interrupted_count = p.value("interrupted_count", 0);
        pm.interrupted_skills_count = p.value("interrupted_skills_count", 0);
        pm.cancelled_attacks_count = p.value("cancelled_attacks_count", 0);
        pm.cancelled_skills_count = p.value("cancelled_skills_count", 0);
        pm.crits_dealt = p.value("crits_dealt", 0);
        pm.crits_received = p.value("crits_received", 0);
        pm.deaths = p.value("deaths", 0);
        pm.kills = p.value("kills", 0);
        pm.skill_template_code = p.value("skill_template_code", std::string());

        if (p.contains("used_skills") && p["used_skills"].is_array())
        {
            for (const auto& s : p["used_skills"])
                pm.used_skills.push_back(s.get<int>());
        }

        out.push_back(std::move(pm));
    }
}

bool LocalReplayProvider::ParseInfosJson(const std::filesystem::path& jsonPath, MatchMeta& out)
{
    try
    {
        std::ifstream file(jsonPath);
        if (!file.is_open()) return false;

        std::stringstream buffer;
        buffer << file.rdbuf();
        file.close();

        std::string sanitized = SanitizeJson(buffer.str());

        json j = json::parse(sanitized, nullptr, false, true);
        if (j.is_discarded()) return false;

        out.map_id = j.value("map_id", 0);
        out.flux = j.value("flux", std::string());
        out.day = j.value("day", 0);
        out.month = j.value("month", 0);
        out.year = j.value("year", 0);
        out.occasion = j.value("occasion", std::string());
        out.match_duration = j.value("match_duration", std::string());
        out.match_original_duration = j.value("match_original_duration", std::string());
        out.match_end_time_ms = j.value("match_end_time_ms", 0);
        out.match_end_time_formatted = j.value("match_end_time_formatted", std::string());
        out.winner_party_id = j.value("winner_party_id", 0);
        out.recording_version = j.value("recording_version", 0);

        if (j.contains("team_kills") && j["team_kills"].is_object())
        {
            for (auto& [key, val] : j["team_kills"].items())
                out.team_kills[key] = val.get<int>();
        }

        if (j.contains("team_damage") && j["team_damage"].is_object())
        {
            for (auto& [key, val] : j["team_damage"].items())
                out.team_damage[key] = val.get<int>();
        }

        if (j.contains("parties") && j["parties"].is_object())
        {
            for (auto& [partyId, partyObj] : j["parties"].items())
            {
                PartyMeta party;
                if (partyObj.contains("PLAYER"))
                    ParsePlayerArray(static_cast<const void*>(&partyObj["PLAYER"]), party.players);
                if (partyObj.contains("OTHER"))
                    ParsePlayerArray(static_cast<const void*>(&partyObj["OTHER"]), party.others);
                out.parties[partyId] = std::move(party);
            }
        }

        if (j.contains("guilds") && j["guilds"].is_object())
        {
            for (auto& [guildId, gObj] : j["guilds"].items())
            {
                GuildMeta gm;
                gm.id = gObj.value("id", 0);
                gm.name = gObj.value("name", std::string());
                gm.tag = gObj.value("tag", std::string());
                gm.rank = gObj.value("rank", 0);
                gm.features = gObj.value("features", 0);
                gm.rating = gObj.value("rating", 0);
                gm.faction = gObj.value("faction", 0);
                gm.faction_points = gObj.value("faction_points", 0);
                gm.qualifier_points = gObj.value("qualifier_points", 0);

                if (gObj.contains("cape") && gObj["cape"].is_object())
                {
                    const auto& c = gObj["cape"];
                    gm.cape.bg_color = c.value("bg_color", 0);
                    gm.cape.detail_color = c.value("detail_color", 0);
                    gm.cape.emblem_color = c.value("emblem_color", 0);
                    gm.cape.shape = c.value("shape", 0);
                    gm.cape.detail = c.value("detail", 0);
                    gm.cape.emblem = c.value("emblem", 0);
                    gm.cape.trim = c.value("trim", 0);
                }

                out.guilds[guildId] = std::move(gm);
            }
        }

        out.folder_name = jsonPath.parent_path().filename().string();
        out.folder_path = jsonPath.parent_path().string();

        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

static std::string DecompressGzipFile(const std::filesystem::path& gzPath)
{
    std::ifstream file(gzPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};

    auto fileSize = static_cast<size_t>(file.tellg());
    if (fileSize < 18) return {};

    std::vector<uint8_t> data(fileSize);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    file.close();

    if (data[0] != 0x1F || data[1] != 0x8B) return {};
    if (data[2] != 0x08) return {};

    uint8_t flags = data[3];
    size_t pos = 10;

    if (flags & 0x04)
    {
        if (pos + 2 > fileSize) return {};
        uint16_t xlen = data[pos] | (data[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flags & 0x08)
    {
        while (pos < fileSize && data[pos] != 0) pos++;
        pos++;
    }
    if (flags & 0x10)
    {
        while (pos < fileSize && data[pos] != 0) pos++;
        pos++;
    }
    if (flags & 0x02)
        pos += 2;

    if (pos >= fileSize - 8) return {};

    size_t deflateLen = fileSize - 8 - pos;
    if (deflateLen == 0) return {};

    uint32_t origSize = data[fileSize - 4] | (data[fileSize - 3] << 8) |
        (data[fileSize - 2] << 16) | (data[fileSize - 1] << 24);
    if (origSize == 0) return {};

    mz_stream stream{};
    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
        return {};

    std::string out(origSize, '\0');
    stream.next_in = data.data() + pos;
    stream.avail_in = static_cast<unsigned int>(deflateLen);
    stream.next_out = reinterpret_cast<unsigned char*>(out.data());
    stream.avail_out = origSize;

    int ret = mz_inflate(&stream, MZ_FINISH);
    mz_inflateEnd(&stream);

    if (ret != MZ_STREAM_END)
        return {};

    out.resize(stream.total_out);
    return out;
}

static void ParseLordEventsFromString(const std::string& content, LordDamageData& out)
{
    long max_damage_after_blue = 0;
    long max_damage_after_red = 0;

    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.empty()) continue;

        size_t bracket_close = line.find(']');
        if (bracket_close == std::string::npos) continue;

        std::string timestamp = line.substr(0, bracket_close + 1);
        std::string rest = line.substr(bracket_close + 1);

        size_t start = rest.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        rest = rest.substr(start);

        if (rest.rfind("LORD_DAMAGE;", 0) != 0) continue;
        rest = rest.substr(12);

        std::vector<std::string> fields;
        std::istringstream ss(rest);
        std::string token;
        while (std::getline(ss, token, ';'))
            fields.push_back(token);

        if (fields.size() < 8) continue;

        try
        {
            LordDamageEvent evt;
            evt.timestamp = timestamp;
            evt.caster_id = std::stoi(fields[0]);
            evt.target_id = std::stoi(fields[1]);
            evt.value = std::stof(fields[2]);
            evt.damage_type = std::stoi(fields[3]);
            evt.attacking_team = std::stoi(fields[4]);
            evt.damage = std::stol(fields[5]);
            evt.damage_before = std::stol(fields[6]);
            evt.damage_after = std::stol(fields[7]);

            if (evt.attacking_team == 1)
                max_damage_after_blue = (std::max)(max_damage_after_blue, evt.damage_after);
            else if (evt.attacking_team == 2)
                max_damage_after_red = (std::max)(max_damage_after_red, evt.damage_after);

            out.events.push_back(std::move(evt));
        }
        catch (const std::exception&)
        {
            continue;
        }
    }

    out.total_lord_damage_blue = max_damage_after_blue;
    out.total_lord_damage_red = max_damage_after_red;
    out.has_data = !out.events.empty();
}

void LocalReplayProvider::ParseLordEvents(const std::filesystem::path& matchFolder, LordDamageData& out)
{
    out = LordDamageData{};

    std::filesystem::path candidates[] = {
        matchFolder / "StoC" / "lord_events.txt",
        matchFolder / "lord_events.txt",
    };
    std::filesystem::path gz_candidates[] = {
        matchFolder / "StoC" / "lord_events.txt.gz",
        matchFolder / "lord_events.txt.gz",
    };

    std::string content;
    std::string source_path;

    for (const auto& p : candidates)
    {
        if (std::filesystem::exists(p))
        {
            std::ifstream file(p);
            if (!file.is_open()) continue;
            std::ostringstream buf;
            buf << file.rdbuf();
            content = buf.str();
            source_path = p.string();
            break;
        }
    }

    if (content.empty())
    {
        for (const auto& p : gz_candidates)
        {
            if (std::filesystem::exists(p))
            {
                content = DecompressGzipFile(p);
                source_path = p.string();
                if (content.empty())
                {
                    out.debug_status = "Failed to decompress: " + p.string();
                    return;
                }
                break;
            }
        }
    }

    if (content.empty())
    {
        auto stocDir = matchFolder / "StoC";
        if (std::filesystem::exists(stocDir) && std::filesystem::is_directory(stocDir))
        {
            std::string listing = "StoC/ exists but no lord_events file. Contents: ";
            int count = 0;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(stocDir, ec))
            {
                if (count > 0) listing += ", ";
                listing += e.path().filename().string();
                if (++count >= 15) { listing += " ..."; break; }
            }
            if (count == 0) listing += "(empty)";
            out.debug_status = listing;
        }
        else
        {
            out.debug_status = "No StoC/ folder found in: " + matchFolder.string();
        }
        return;
    }

    out.debug_status = "Loaded: " + source_path;
    ParseLordEventsFromString(content, out);
}

std::vector<MatchMeta> LocalReplayProvider::GetAvailableReplays()
{
    std::vector<MatchMeta> results;

    if (m_folder_path.empty() || !std::filesystem::exists(m_folder_path))
        return results;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(m_folder_path, ec))
    {
        if (!entry.is_directory()) continue;

        auto infosPath = entry.path() / "infos.json";
        if (!std::filesystem::exists(infosPath)) continue;

        MatchMeta meta;
        if (ParseInfosJson(infosPath, meta))
        {
            ParseLordEvents(entry.path(), meta.lord_damage);
            results.push_back(std::move(meta));
        }
    }

    return results;
}

// --- ReplayLibrary ---

ReplayLibrary::ReplayLibrary()
    : m_provider(std::make_unique<LocalReplayProvider>())
{
}

void ReplayLibrary::SetProvider(std::unique_ptr<IReplayProvider> provider)
{
    if (provider)
        m_provider = std::move(provider);
}

void ReplayLibrary::SetMatchDataFolder(const std::string& path)
{
    m_folder_path = path;
    // SetFolder is only available on LocalReplayProvider
    if (auto* local = dynamic_cast<LocalReplayProvider*>(m_provider.get()))
        local->SetFolder(path);
}

void ReplayLibrary::ScanFolder()
{
    m_matches.clear();
    m_loaded = false;

    if (m_folder_path.empty()) return;

    m_matches = m_provider->GetAvailableReplays();

    // Sort by date descending (newest first) to match RescanDiff order
    std::sort(m_matches.begin(), m_matches.end(), [](const MatchMeta& a, const MatchMeta& b)
    {
        if (a.year != b.year) return a.year > b.year;
        if (a.month != b.month) return a.month > b.month;
        if (a.day != b.day) return a.day > b.day;
        return a.folder_path < b.folder_path;
    });

    m_loaded = true;
    m_generation++;
}

int ReplayLibrary::RescanDiff()
{
    m_newMatchFolders.clear();
    if (m_folder_path.empty()) return 0;

    auto fresh = m_provider->GetAvailableReplays();

    // Build set of existing folder_path values for fast lookup
    std::unordered_set<std::string> existingSet;
    existingSet.reserve(m_matches.size());
    for (auto& m : m_matches)
        existingSet.insert(m.folder_path);

    // Build set of fresh folder_path values
    std::unordered_set<std::string> freshSet;
    freshSet.reserve(fresh.size());
    for (auto& m : fresh)
        freshSet.insert(m.folder_path);

    // Remove matches no longer on disk
    m_matches.erase(
        std::remove_if(m_matches.begin(), m_matches.end(),
            [&](const MatchMeta& m) { return freshSet.find(m.folder_path) == freshSet.end(); }),
        m_matches.end());

    // Sync cloud/local status: replace entries whose cloud state has changed
    {
        std::unordered_map<std::string, const MatchMeta*> freshByPath;
        for (const auto& m : fresh)
            freshByPath[m.folder_path] = &m;

        for (auto& existing : m_matches)
        {
            auto it = freshByPath.find(existing.folder_path);
            if (it == freshByPath.end()) continue;
            // Replace if cloud status changed in either direction
            if (existing.is_cloud_only != it->second->is_cloud_only)
                existing = *(it->second);
        }
    }

    // Add new matches
    int added = 0;
    for (auto& m : fresh)
    {
        if (existingSet.find(m.folder_path) == existingSet.end())
        {
            m_newMatchFolders.push_back(m.folder_path);
            m_matches.push_back(std::move(m));
            added++;
        }
    }

    // Sort by date descending (newest first), tiebreak by folder_path for determinism
    std::sort(m_matches.begin(), m_matches.end(), [](const MatchMeta& a, const MatchMeta& b)
    {
        if (a.year != b.year) return a.year > b.year;
        if (a.month != b.month) return a.month > b.month;
        if (a.day != b.day) return a.day > b.day;
        return a.folder_path < b.folder_path;
    });

    m_loaded = true;
    m_generation++;
    return added;
}

void ReplayLibrary::Clear()
{
    m_matches.clear();
    m_loaded = false;
    m_folder_path.clear();
}
