#include "pch.h"
#include "AgentSnapshotParser.h"
#include <fstream>
#include <sstream>
#include <charconv>
#include <thread>

#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

// ---------------------------------------------------------------------------
// Gzip decompression using miniz (handles all DEFLATE block types correctly)
// ---------------------------------------------------------------------------
namespace {

static std::string DecompressGzipBuffer(const std::vector<uint8_t>& data)
{
    if (data.size() < 18) return {};
    if (data[0] != 0x1F || data[1] != 0x8B) return {};
    if (data[2] != 0x08) return {};

    uint8_t flags = data[3];
    size_t pos = 10;
    size_t fileSize = data.size();

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
    // -MZ_DEFAULT_WINDOW_BITS = raw deflate (no zlib/gzip wrapper)
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

// ---------------------------------------------------------------------------
// Timestamp parsing: [MM:SS.ms] -> seconds as float
// ---------------------------------------------------------------------------
static float ParseTimestamp(const char* begin, const char* end)
{
    // Handles both [MM:SS] and [MM:SS.ms]
    if (begin >= end || *begin != '[') return -1.f;
    begin++;

    const char* closeBracket = static_cast<const char*>(memchr(begin, ']', end - begin));
    if (!closeBracket) return -1.f;

    const char* colon = static_cast<const char*>(memchr(begin, ':', closeBracket - begin));
    if (!colon) return -1.f;

    int minutes = 0, seconds = 0, millis = 0;
    std::from_chars(begin, colon, minutes);

    const char* dot = static_cast<const char*>(memchr(colon, '.', closeBracket - colon));
    if (dot)
    {
        std::from_chars(colon + 1, dot, seconds);
        std::from_chars(dot + 1, closeBracket, millis);
    }
    else
    {
        std::from_chars(colon + 1, closeBracket, seconds);
    }

    return static_cast<float>(minutes) * 60.f + static_cast<float>(seconds) +
           static_cast<float>(millis) / 1000.f;
}

// ---------------------------------------------------------------------------
// Fast field tokenizer: splits on ';', returns pointers into the source string
// ---------------------------------------------------------------------------
static constexpr int kExpectedFields = 46;

struct FieldView { const char* begin; const char* end; };

static int TokenizeFields(const char* begin, const char* end,
                          FieldView* out, int maxFields)
{
    int count = 0;
    const char* fieldStart = begin;
    for (const char* p = begin; p < end; ++p)
    {
        if (*p == ';')
        {
            if (count < maxFields)
            {
                out[count].begin = fieldStart;
                out[count].end = p;
                count++;
            }
            fieldStart = p + 1;
        }
    }
    if (count < maxFields && fieldStart <= end)
    {
        out[count].begin = fieldStart;
        out[count].end = end;
        count++;
    }
    return count;
}

static float FieldToFloat(const FieldView& f)
{
    float v = 0.f;
    std::from_chars(f.begin, f.end, v);
    return v;
}

static uint32_t FieldToU32(const FieldView& f)
{
    uint32_t v = 0;
    std::from_chars(f.begin, f.end, v);
    return v;
}

static uint16_t FieldToU16(const FieldView& f)
{
    uint16_t v = 0;
    std::from_chars(f.begin, f.end, v);
    return v;
}

static uint8_t FieldToU8(const FieldView& f)
{
    uint8_t v = 0;
    std::from_chars(f.begin, f.end, v);
    return v;
}

static bool FieldToBool(const FieldView& f)
{
    return (f.begin < f.end && *f.begin != '0');
}

// ---------------------------------------------------------------------------
// Parse a single snapshot line into an AgentSnapshot.
// Returns false if the line is malformed.
// ---------------------------------------------------------------------------
static bool ParseSnapshotLine(const char* lineBegin, const char* lineEnd,
                              AgentSnapshot& snap)
{
    // Find end of timestamp: '] '
    const char* bracketClose = static_cast<const char*>(
        memchr(lineBegin, ']', lineEnd - lineBegin));
    if (!bracketClose) return false;

    snap.time = ParseTimestamp(lineBegin, bracketClose + 1);
    if (snap.time < 0.f) return false;

    // Data starts after '] '
    const char* dataStart = bracketClose + 1;
    while (dataStart < lineEnd && (*dataStart == ' ' || *dataStart == '\t'))
        dataStart++;

    FieldView fields[kExpectedFields + 4];
    int nFields = TokenizeFields(dataStart, lineEnd, fields, kExpectedFields + 4);

    constexpr int kMinFields = 10; // x,y,z,rotation,weapon_id,model_id,gadget_id,alive,dead,health
    if (nFields < kMinFields) return false;

    int i = 0;
    snap.x                    = FieldToFloat(fields[i++]);
    snap.y                    = FieldToFloat(fields[i++]);
    snap.z                    = FieldToFloat(fields[i++]);
    snap.rotation             = FieldToFloat(fields[i++]);
    snap.weapon_id            = FieldToU32  (fields[i++]);
    snap.model_id             = FieldToU32  (fields[i++]);
    snap.gadget_id            = FieldToU32  (fields[i++]);
    snap.is_alive             = FieldToBool (fields[i++]);
    snap.is_dead              = FieldToBool (fields[i++]);
    snap.health_pct           = FieldToFloat(fields[i++]);

    // Remaining fields are optional; AgentSnapshot members are zero-initialized.
    if (i < nFields) snap.is_knocked           = FieldToBool (fields[i++]);
    if (i < nFields) snap.max_hp               = FieldToU32  (fields[i++]);
    if (i < nFields) snap.has_condition         = FieldToBool (fields[i++]);
    if (i < nFields) snap.has_deep_wound        = FieldToBool (fields[i++]);
    if (i < nFields) snap.has_bleeding          = FieldToBool (fields[i++]);
    if (i < nFields) snap.has_crippled          = FieldToBool (fields[i++]);
    if (i < nFields) snap.has_blind             = FieldToBool (fields[i++]);
    if (i < nFields) snap.has_poison            = FieldToBool (fields[i++]);
    if (i < nFields) snap.has_hex               = FieldToBool (fields[i++]);
    if (i < nFields) snap.has_degen_hex         = FieldToBool (fields[i++]);
    if (i < nFields) snap.has_enchantment       = FieldToBool (fields[i++]);
    if (i < nFields) snap.has_weapon_spell      = FieldToBool (fields[i++]);
    if (i < nFields) snap.is_holding            = FieldToBool (fields[i++]);
    if (i < nFields) snap.is_casting            = FieldToBool (fields[i++]);
    if (i < nFields) snap.skill_id             = FieldToU32  (fields[i++]);
    if (i < nFields) snap.weapon_item_type     = FieldToU8   (fields[i++]);
    if (i < nFields) snap.offhand_item_type    = FieldToU8   (fields[i++]);
    if (i < nFields) snap.weapon_item_id       = FieldToU16  (fields[i++]);
    if (i < nFields) snap.offhand_item_id      = FieldToU16  (fields[i++]);
    if (i < nFields) snap.move_x               = FieldToFloat(fields[i++]);
    if (i < nFields) snap.move_y               = FieldToFloat(fields[i++]);
    if (i < nFields) snap.visual_effects       = FieldToU16  (fields[i++]);
    if (i < nFields) snap.team_id              = FieldToU8   (fields[i++]);
    if (i < nFields) snap.weapon_type          = FieldToU16  (fields[i++]);
    if (i < nFields) snap.weapon_attack_speed  = FieldToFloat(fields[i++]);
    if (i < nFields) snap.attack_speed_modifier = FieldToFloat(fields[i++]);
    if (i < nFields) snap.dagger_status        = FieldToU8   (fields[i++]);
    if (i < nFields) snap.hp_pips              = FieldToFloat(fields[i++]);
    if (i < nFields) snap.model_state          = FieldToU32  (fields[i++]);
    if (i < nFields) snap.animation_code       = FieldToU32  (fields[i++]);
    if (i < nFields) snap.animation_id         = FieldToU32  (fields[i++]);
    if (i < nFields) snap.animation_speed      = FieldToFloat(fields[i++]);
    if (i < nFields) snap.animation_type       = FieldToFloat(fields[i++]);
    if (i < nFields) snap.in_spirit_range      = FieldToU32  (fields[i++]);
    if (i < nFields) snap.agent_model_type     = FieldToU16  (fields[i++]);
    if (i < nFields) snap.item_id              = FieldToU32  (fields[i++]);
    if (i < nFields) snap.item_extra_type      = FieldToU32  (fields[i++]);
    if (i < nFields) snap.gadget_extra_type    = FieldToU32  (fields[i++]);

    return true;
}

// ---------------------------------------------------------------------------
// Parse a single agent file (either .txt.gz or .txt) -> AgentReplayData
// ---------------------------------------------------------------------------
static bool ParseAgentFile(const std::filesystem::path& filePath,
                           int agentId, AgentReplayData& out)
{
    std::string content;

    if (filePath.extension() == ".gz")
    {
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            OutputDebugStringA(std::format("[AgentParse] Agent {}: failed to open {}\n",
                agentId, filePath.string()).c_str());
            return false;
        }
        auto sz = static_cast<size_t>(file.tellg());
        std::vector<uint8_t> buf(sz);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(buf.data()), sz);
        file.close();
        content = DecompressGzipBuffer(buf);
        if (content.empty())
        {
            OutputDebugStringA(std::format("[AgentParse] Agent {}: gz decompression failed "
                "(compressed={} bytes, file={})\n", agentId, sz, filePath.filename().string()).c_str());
            return false;
        }
    }
    else
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            OutputDebugStringA(std::format("[AgentParse] Agent {}: failed to open {}\n",
                agentId, filePath.string()).c_str());
            return false;
        }
        std::stringstream ss;
        ss << file.rdbuf();
        content = ss.str();
    }

    if (content.empty()) return false;

    out.agent_id = agentId;
    out.snapshots.reserve(content.size() / 120);

    const char* ptr = content.data();
    const char* end = ptr + content.size();

    int totalLines = 0;
    int acceptedLines = 0;
    std::string firstRejected;

    while (ptr < end)
    {
        const char* lineEnd = static_cast<const char*>(memchr(ptr, '\n', end - ptr));
        if (!lineEnd) lineEnd = end;

        const char* effectiveEnd = lineEnd;
        if (effectiveEnd > ptr && *(effectiveEnd - 1) == '\r')
            effectiveEnd--;

        if (effectiveEnd > ptr)
        {
            // Detect recorder incarnation break markers
            constexpr const char kBreakMarker[] = "# INCARNATION_BREAK";
            size_t lineLen = static_cast<size_t>(effectiveEnd - ptr);
            if (lineLen >= sizeof(kBreakMarker) - 1
                && memcmp(ptr, kBreakMarker, sizeof(kBreakMarker) - 1) == 0)
            {
                out.incarnationBreaks.push_back(static_cast<int>(out.snapshots.size()));
                ptr = lineEnd + 1;
                continue;
            }

            totalLines++;
            AgentSnapshot snap;
            snap.raw_line.assign(ptr, effectiveEnd);
            if (ParseSnapshotLine(ptr, effectiveEnd, snap))
            {
                out.snapshots.push_back(std::move(snap));
                acceptedLines++;
            }
            else if (firstRejected.empty())
            {
                size_t previewLen = std::min<size_t>(120, effectiveEnd - ptr);
                firstRejected.assign(ptr, previewLen);
            }
        }

        ptr = lineEnd + 1;
    }

    if (acceptedLines == 0 && totalLines > 0)
    {
        OutputDebugStringA(std::format("[AgentParse] Agent {}: ALL {} lines rejected! "
            "First rejected: \"{}\"\n", agentId, totalLines, firstRejected).c_str());
    }
    else if (acceptedLines < totalLines)
    {
        OutputDebugStringA(std::format("[AgentParse] Agent {}: {}/{} lines accepted\n",
            agentId, acceptedLines, totalLines).c_str());
    }

    return !out.snapshots.empty();
}

// ---------------------------------------------------------------------------
// Extract agent_id from filename. E.g. "42.txt.gz" -> 42, "42.txt" -> 42
// ---------------------------------------------------------------------------
static int ExtractAgentId(const std::filesystem::path& filePath)
{
    std::string stem = filePath.stem().string();
    // If stem is like "42.txt" (from 42.txt.gz), strip trailing .txt
    if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".txt")
        stem = stem.substr(0, stem.size() - 4);

    int id = 0;
    std::from_chars(stem.data(), stem.data() + stem.size(), id);
    return id;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void LaunchAgentSnapshotParsing(const std::filesystem::path& matchFolder,
                                std::shared_ptr<AgentParseProgress> progress)
{
    auto agentsDir = matchFolder / "Agents";
    OutputDebugStringA(std::format("[AgentParse] Scanning: {}\n", agentsDir.string()).c_str());
    if (!std::filesystem::exists(agentsDir) || !std::filesystem::is_directory(agentsDir))
    {
        OutputDebugStringA("[AgentParse] Agents directory not found!\n");
        progress->finished.store(true);
        return;
    }

    struct AgentFile { int id; std::filesystem::path path; };
    std::vector<AgentFile> files;
    std::unordered_map<int, bool> seenGz;

    for (const auto& entry : std::filesystem::directory_iterator(agentsDir))
    {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        auto stem = entry.path().stem().string();

        bool isGz = (ext == ".gz" && stem.size() > 4 && stem.substr(stem.size() - 4) == ".txt");
        bool isTxt = (ext == ".txt");
        if (!isGz && !isTxt) continue;

        int agentId = ExtractAgentId(entry.path());
        if (agentId <= 0) continue;

        if (isGz)
        {
            seenGz[agentId] = true;
            files.push_back({ agentId, entry.path() });
        }
        else if (!seenGz.count(agentId))
        {
            files.push_back({ agentId, entry.path() });
        }
    }

    // Prefer .gz over .txt when both exist for the same agent
    std::unordered_map<int, size_t> bestIdx;
    for (size_t i = 0; i < files.size(); i++)
    {
        auto it = bestIdx.find(files[i].id);
        if (it == bestIdx.end())
        {
            bestIdx[files[i].id] = i;
        }
        else
        {
            bool curIsGz = files[i].path.extension().string() == ".gz";
            bool prevIsGz = files[it->second].path.extension().string() == ".gz";
            if (curIsGz && !prevIsGz)
                it->second = i;
        }
    }

    std::vector<AgentFile> uniqueFiles;
    uniqueFiles.reserve(bestIdx.size());
    for (auto& [id, idx] : bestIdx)
        uniqueFiles.push_back(files[idx]);

    progress->files_total.store(static_cast<int>(uniqueFiles.size()));
    OutputDebugStringA(std::format("[AgentParse] Found {} agent files\n",
        uniqueFiles.size()).c_str());

    if (uniqueFiles.empty())
    {
        progress->finished.store(true);
        return;
    }

    std::thread([progress, uniqueFiles = std::move(uniqueFiles)]()
    {
        int loaded = 0;
        for (const auto& af : uniqueFiles)
        {
            AgentReplayData ard;
            try
            {
                if (ParseAgentFile(af.path, af.id, ard))
                {
                    std::lock_guard<std::mutex> lock(progress->mutex);
                    progress->agents[af.id] = std::move(ard);
                    loaded++;
                }
            }
            catch (const std::exception& e)
            {
                std::lock_guard<std::mutex> lock(progress->mutex);
                progress->errors.push_back(
                    std::format("Agent {}: {}", af.id, e.what()));
                progress->has_error.store(true);
            }

            progress->files_done.fetch_add(1);
        }

        OutputDebugStringA(std::format("[AgentParse] Done: {}/{} agents loaded successfully\n",
            loaded, uniqueFiles.size()).c_str());
        progress->finished.store(true);
    }).detach();
}

bool PollAgentParseCompletion(ReplayContext& ctx)
{
    if (ctx.agentsLoaded) return true;
    if (!ctx.agentParseProgress) return false;
    if (!ctx.agentParseProgress->finished.load()) return false;

    {
        std::lock_guard<std::mutex> lock(ctx.agentParseProgress->mutex);
        ctx.agents = std::move(ctx.agentParseProgress->agents);
    }

    float maxTime = 0.f;
    for (auto& [id, ard] : ctx.agents)
    {
        if (!ard.snapshots.empty())
            maxTime = std::max(maxTime, ard.snapshots.back().time);
    }
    ctx.maxReplayTime = maxTime;
    ctx.agentsLoaded = true;
    return true;
}

// ---------------------------------------------------------------------------
// ClassifyAgents: match parsed agents against MatchMeta and NPC/Gadget tables
// ---------------------------------------------------------------------------

void ClassifyAgents(std::unordered_map<int, AgentReplayData>& agents,
                    const MatchMeta& meta, int mapId,
                    const FlagItemRegistry* flagItems)
{
    // Build model_id -> PlayerMeta lookup from both parties
    std::unordered_map<uint32_t, const PlayerMeta*> playerByModelId;
    for (auto& [partyId, party] : meta.parties)
    {
        for (auto& p : party.players)
            playerByModelId[static_cast<uint32_t>(p.model_id)] = &p;
    }

    for (auto& [agentId, ard] : agents)
    {
        if (ard.snapshots.empty()) continue;

        const auto& first = ard.snapshots[0];
        ard.modelId        = first.model_id;
        ard.agentModelType = first.agent_model_type;
        ard.teamId         = first.team_id;

        // Carryable check — must come early. Which carryable an item id stands
        // for changes over the match as the server recycles ids, so this is
        // asked about the moment this agent came into existence.
        BundleType carried = flagItems
            ? flagItems->Classify(first.item_id, first.time)
            : LookupBundleType(mapId, first.item_id);

        if (carried == BundleType::Flag)
        {
            ard.type         = AgentType::Flag;
            ard.categoryName = "Flag";
            continue;
        }
        if (carried != BundleType::Unknown)
        {
            ard.type         = AgentType::Item;
            ard.categoryName = BundleTypeName(carried);
            ard.teamId       = 0;
            continue;
        }

        // Player: agent_model_type == 0x3000 AND model_id matches metadata
        if (first.agent_model_type == 0x3000)
        {
            auto it = playerByModelId.find(first.model_id);
            if (it != playerByModelId.end())
            {
                ard.type         = AgentType::Player;
                ard.playerName   = it->second->encoded_name;
                ard.teamId       = static_cast<uint8_t>(it->second->team_id);
                ard.categoryName = it->second->encoded_name;

                // Look up guild tag
                if (it->second->guild_id > 0) {
                    auto git = meta.guilds.find(std::to_string(it->second->guild_id));
                    if (git != meta.guilds.end() && !git->second.tag.empty())
                        ard.guildTag = git->second.tag;
                }
                continue;
            }
        }

        // Spirit check (model_id-based)
        const SpiritInfo* spirit = LookupSpirit(first.model_id);
        if (spirit)
        {
            ard.type            = AgentType::Spirit;
            ard.categoryName    = spirit->name;
            ard.spiritSkillId   = spirit->skillId;
            ard.spiritSkillName = spirit->name;
            continue;
        }

        // NPC check
        const char* npcName = LookupNpcName(first.model_id);
        if (npcName)
        {
            ard.type         = AgentType::NPC;
            ard.categoryName = npcName;
            continue;
        }

        // Obelisk Flag Stand check (gadget_id 4720, Isle of Meditation)
        if (first.gadget_id == 4720)
        {
            ard.type         = AgentType::ObeliskFlagStand;
            ard.categoryName = "Obelisk Flag Stand";
            continue;
        }

        // Gadget check (use gadget_id field from the snapshot)
        const char* gadgetName = LookupGadgetName(first.gadget_id);
        if (gadgetName)
        {
            ard.type         = AgentType::Gadget;
            ard.categoryName = gadgetName;
            continue;
        }

        ard.type         = AgentType::Unknown;
        ard.categoryName = "Unknown";
    }
}

// ---------------------------------------------------------------------------
// SplitRecycledAgents — detect agent ID recycling and split mixed agent files
// into separate incarnations so each entry has consistent item_id / model_id.
// ---------------------------------------------------------------------------

static constexpr int kSyntheticIdBase = 1'000'000;

void SplitRecycledAgents(std::unordered_map<int, AgentReplayData>& agents,
                         const std::vector<LifecycleEvent>& lifecycle,
                         const std::unordered_set<int>* skipAgentIds)
{
    // Phase 1: Build lifecycle windows per agent_id
    struct Window { float addTime; float removeTime; };
    std::unordered_map<int, std::vector<Window>> windowsMap;

    for (auto& ev : lifecycle)
    {
        if (ev.isAdd)
        {
            windowsMap[ev.agent_id].push_back({ ev.time, FLT_MAX });
        }
        else
        {
            auto& wins = windowsMap[ev.agent_id];
            if (!wins.empty() && wins.back().removeTime == FLT_MAX)
                wins.back().removeTime = ev.time;
            else
                wins.push_back({ 0.f, ev.time }); // pre-existing agent removed (no ADD seen)
        }
    }

    int nextSyntheticId = kSyntheticIdBase;

    // Phase 2: For agents with lifecycle data, split by windows
    std::vector<std::pair<int, AgentReplayData>> toInsert;

    for (auto& [agentId, ard] : agents)
    {
        if (ard.snapshots.empty()) continue;
        if (skipAgentIds && skipAgentIds->count(agentId)) continue;

        auto wit = windowsMap.find(agentId);
        bool hasLifecycle = (wit != windowsMap.end() && wit->second.size() > 1);

        if (hasLifecycle)
        {
            auto& windows = wit->second;
            constexpr float kTolerance = 0.5f;

            // Sort windows by addTime
            std::sort(windows.begin(), windows.end(),
                      [](const Window& a, const Window& b) { return a.addTime < b.addTime; });

            // Assign snapshots to windows
            std::vector<std::vector<AgentSnapshot>> perWindow(windows.size());
            for (auto& snap : ard.snapshots)
            {
                int bestWin = -1;
                float bestDist = FLT_MAX;
                for (int w = 0; w < static_cast<int>(windows.size()); w++)
                {
                    float lo = windows[w].addTime - kTolerance;
                    float hi = (windows[w].removeTime < FLT_MAX)
                             ? windows[w].removeTime + kTolerance
                             : FLT_MAX;
                    if (snap.time >= lo && snap.time <= hi)
                    {
                        float dist = (snap.time < windows[w].addTime)
                                   ? windows[w].addTime - snap.time
                                   : 0.f;
                        if (dist < bestDist)
                        {
                            bestDist = dist;
                            bestWin = w;
                        }
                    }
                }
                if (bestWin >= 0)
                    perWindow[bestWin].push_back(snap);
            }

            // Keep window 0 in the original agent entry
            ard.lifecycleStart = windows[0].addTime;
            ard.lifecycleEnd   = (windows[0].removeTime < FLT_MAX) ? windows[0].removeTime : -1.f;
            if (!perWindow[0].empty())
            {
                ard.snapshots = std::move(perWindow[0]);
                ard.modelId = ard.snapshots.front().model_id;
            }
            else
            {
                ard.snapshots.clear();
            }

            // Create new entries for windows 1+
            for (int w = 1; w < static_cast<int>(windows.size()); w++)
            {
                if (perWindow[w].empty()) continue;
                AgentReplayData newArd;
                newArd.agent_id = nextSyntheticId++;
                newArd.originalAgentId = agentId;
                newArd.lifecycleStart = windows[w].addTime;
                newArd.lifecycleEnd   = (windows[w].removeTime < FLT_MAX) ? windows[w].removeTime : -1.f;
                newArd.snapshots = std::move(perWindow[w]);
                newArd.modelId = newArd.snapshots.front().model_id;
                toInsert.push_back({ newArd.agent_id, std::move(newArd) });
            }
        }
        else
        {
            // Use incarnation break markers and/or item_id/model_id changes
            // to detect recycling when lifecycle events are absent or incomplete
            std::vector<size_t> splitPoints;

            // First, use explicit break markers from the recorder
            for (int bp : ard.incarnationBreaks)
            {
                if (bp > 0 && bp < static_cast<int>(ard.snapshots.size()))
                    splitPoints.push_back(static_cast<size_t>(bp));
            }

            // Also detect item_id or model_id changes (handles old recordings)
            for (size_t i = 1; i < ard.snapshots.size(); i++)
            {
                auto& prev = ard.snapshots[i - 1];
                auto& cur  = ard.snapshots[i];
                bool itemChanged  = (prev.item_id != 0 || cur.item_id != 0)
                                  && prev.item_id != cur.item_id;
                bool modelChanged = (prev.model_id != 0 || cur.model_id != 0)
                                  && prev.model_id != cur.model_id;
                if (itemChanged || modelChanged)
                    splitPoints.push_back(i);
            }

            // Deduplicate and sort
            std::sort(splitPoints.begin(), splitPoints.end());
            splitPoints.erase(std::unique(splitPoints.begin(), splitPoints.end()),
                              splitPoints.end());

            if (!splitPoints.empty())
            {
                std::vector<AgentSnapshot> origSnaps;
                origSnaps.assign(ard.snapshots.begin(),
                                 ard.snapshots.begin() + splitPoints[0]);

                for (size_t s = 0; s < splitPoints.size(); s++)
                {
                    size_t from = splitPoints[s];
                    size_t to = (s + 1 < splitPoints.size())
                              ? splitPoints[s + 1]
                              : ard.snapshots.size();
                    AgentReplayData newArd;
                    newArd.agent_id = nextSyntheticId++;
                    newArd.originalAgentId = agentId;
                    newArd.snapshots.assign(ard.snapshots.begin() + from,
                                            ard.snapshots.begin() + to);
                    if (!newArd.snapshots.empty())
                        newArd.modelId = newArd.snapshots.front().model_id;
                    toInsert.push_back({ newArd.agent_id, std::move(newArd) });
                }

                ard.snapshots = std::move(origSnaps);
                if (!ard.snapshots.empty())
                    ard.modelId = ard.snapshots.front().model_id;
            }
        }
    }

    // Phase 3: Insert new incarnation entries
    for (auto& [id, newArd] : toInsert)
        agents[id] = std::move(newArd);

    if (!toInsert.empty())
    {
        OutputDebugStringA(std::format(
            "[SplitRecycledAgents] Created {} new incarnation entries\n",
            toInsert.size()).c_str());
    }
}
