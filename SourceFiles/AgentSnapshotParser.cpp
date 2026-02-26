#include "pch.h"
#include "AgentSnapshotParser.h"
#include <fstream>
#include <sstream>
#include <charconv>
#include <thread>

// ---------------------------------------------------------------------------
// Self-contained DEFLATE decompressor (same as ReplayLibrary.cpp)
// Duplicated here to keep the parser self-contained; both files share the same
// anonymous-namespace implementation with no cross-TU symbol leakage.
// ---------------------------------------------------------------------------
namespace {

struct BitStream
{
    const uint8_t* src;
    size_t len;
    size_t pos = 0;
    uint32_t buf = 0;
    int bits = 0;

    void fill()
    {
        while (bits <= 24 && pos < len)
        {
            buf |= static_cast<uint32_t>(src[pos++]) << bits;
            bits += 8;
        }
    }

    uint32_t read(int n)
    {
        if (bits < n) fill();
        uint32_t val = buf & ((1U << n) - 1);
        buf >>= n;
        bits -= n;
        return val;
    }

    void align()
    {
        int discard = bits & 7;
        buf >>= discard;
        bits -= discard;
    }
};

static constexpr int kMaxBits = 15;
static constexpr int kMaxLitLenSyms = 288;
static constexpr int kMaxDistSyms = 32;

struct HuffTable
{
    uint16_t counts[kMaxBits + 1] = {};
    uint16_t symbols[kMaxLitLenSyms] = {};
};

static bool BuildHuff(HuffTable& t, const uint8_t* lengths, int num)
{
    memset(t.counts, 0, sizeof(t.counts));
    for (int i = 0; i < num; i++)
        t.counts[lengths[i]]++;
    t.counts[0] = 0;

    uint16_t offsets[kMaxBits + 1];
    offsets[0] = 0;
    offsets[1] = 0;
    for (int i = 1; i < kMaxBits; i++)
        offsets[i + 1] = offsets[i] + t.counts[i];

    for (int i = 0; i < num; i++)
        if (lengths[i])
            t.symbols[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
    return true;
}

static int DecodeSymbol(BitStream& bs, const HuffTable& t)
{
    bs.fill();
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; len++)
    {
        code |= (bs.buf & 1);
        bs.buf >>= 1;
        bs.bits--;
        int count = t.counts[len];
        if (code < first + count)
            return t.symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t kLenBase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t kLenExtra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t kDistBase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t kDistExtra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static bool InflateRaw(const uint8_t* src, size_t srcLen,
    std::vector<uint8_t>& out, size_t sizeHint)
{
    out.clear();
    out.reserve(sizeHint ? sizeHint : srcLen * 4);

    BitStream bs;
    bs.src = src;
    bs.len = srcLen;

    int bfinal;
    do
    {
        bfinal = bs.read(1);
        int btype = bs.read(2);

        if (btype == 0)
        {
            bs.align();
            if (bs.pos + 4 > bs.len) return false;
            uint16_t len = bs.src[bs.pos] | (bs.src[bs.pos + 1] << 8);
            uint16_t nlen = bs.src[bs.pos + 2] | (bs.src[bs.pos + 3] << 8);
            bs.pos += 4;
            bs.buf = 0;
            bs.bits = 0;
            if ((uint16_t)(~nlen) != len) return false;
            if (bs.pos + len > bs.len) return false;
            out.insert(out.end(), bs.src + bs.pos, bs.src + bs.pos + len);
            bs.pos += len;
        }
        else if (btype == 1 || btype == 2)
        {
            HuffTable litLen, dist;

            if (btype == 1)
            {
                uint8_t lengths[kMaxLitLenSyms];
                int i = 0;
                for (; i < 144; i++) lengths[i] = 8;
                for (; i < 256; i++) lengths[i] = 9;
                for (; i < 280; i++) lengths[i] = 7;
                for (; i < 288; i++) lengths[i] = 8;
                BuildHuff(litLen, lengths, 288);

                uint8_t dlengths[kMaxDistSyms];
                for (i = 0; i < 32; i++) dlengths[i] = 5;
                BuildHuff(dist, dlengths, 32);
            }
            else
            {
                int hlit = bs.read(5) + 257;
                int hdist = bs.read(5) + 1;
                int hclen = bs.read(4) + 4;

                static const int kCodeOrder[19] = {
                    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
                };
                uint8_t clLengths[19] = {};
                for (int i = 0; i < hclen; i++)
                    clLengths[kCodeOrder[i]] = static_cast<uint8_t>(bs.read(3));

                HuffTable clTable;
                BuildHuff(clTable, clLengths, 19);

                uint8_t lengths[kMaxLitLenSyms + kMaxDistSyms] = {};
                int total = hlit + hdist;
                for (int i = 0; i < total;)
                {
                    int sym = DecodeSymbol(bs, clTable);
                    if (sym < 0) return false;
                    if (sym < 16)
                    {
                        lengths[i++] = static_cast<uint8_t>(sym);
                    }
                    else if (sym == 16)
                    {
                        if (i == 0) return false;
                        int rep = bs.read(2) + 3;
                        uint8_t prev = lengths[i - 1];
                        for (int j = 0; j < rep && i < total; j++)
                            lengths[i++] = prev;
                    }
                    else if (sym == 17)
                    {
                        int rep = bs.read(3) + 3;
                        for (int j = 0; j < rep && i < total; j++)
                            lengths[i++] = 0;
                    }
                    else if (sym == 18)
                    {
                        int rep = bs.read(7) + 11;
                        for (int j = 0; j < rep && i < total; j++)
                            lengths[i++] = 0;
                    }
                    else return false;
                }

                BuildHuff(litLen, lengths, hlit);
                BuildHuff(dist, lengths + hlit, hdist);
            }

            for (;;)
            {
                int sym = DecodeSymbol(bs, litLen);
                if (sym < 0) return false;
                if (sym < 256)
                {
                    out.push_back(static_cast<uint8_t>(sym));
                }
                else if (sym == 256)
                {
                    break;
                }
                else
                {
                    sym -= 257;
                    if (sym >= 29) return false;
                    int length = kLenBase[sym] + bs.read(kLenExtra[sym]);

                    int dsym = DecodeSymbol(bs, dist);
                    if (dsym < 0 || dsym >= 30) return false;
                    int distance = kDistBase[dsym] + bs.read(kDistExtra[dsym]);

                    if (distance > static_cast<int>(out.size())) return false;
                    size_t srcOff = out.size() - distance;
                    for (int j = 0; j < length; j++)
                        out.push_back(out[srcOff + j]);
                }
            }
        }
        else
        {
            return false;
        }
    } while (!bfinal);

    return true;
}

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

    int deflateLen = static_cast<int>(fileSize - 8 - pos);
    if (deflateLen <= 0) return {};

    uint32_t origSize = data[fileSize - 4] | (data[fileSize - 3] << 8) |
        (data[fileSize - 2] << 16) | (data[fileSize - 1] << 24);

    std::vector<uint8_t> inflated;
    if (!InflateRaw(data.data() + pos, static_cast<size_t>(deflateLen),
        inflated, origSize))
        return {};

    if (inflated.empty()) return {};
    return std::string(reinterpret_cast<const char*>(inflated.data()), inflated.size());
}

// ---------------------------------------------------------------------------
// Timestamp parsing: [MM:SS.ms] -> seconds as float
// ---------------------------------------------------------------------------
static float ParseTimestamp(const char* begin, const char* end)
{
    // Expected: [MM:SS.ms]  e.g. [01:23.456]
    if (begin >= end || *begin != '[') return -1.f;
    begin++;

    const char* closeBracket = static_cast<const char*>(memchr(begin, ']', end - begin));
    if (!closeBracket) return -1.f;

    const char* colon = static_cast<const char*>(memchr(begin, ':', closeBracket - begin));
    if (!colon) return -1.f;

    const char* dot = static_cast<const char*>(memchr(colon, '.', closeBracket - colon));
    if (!dot) return -1.f;

    int minutes = 0, seconds = 0, millis = 0;
    std::from_chars(begin, colon, minutes);
    std::from_chars(colon + 1, dot, seconds);
    std::from_chars(dot + 1, closeBracket, millis);

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
    if (nFields < kExpectedFields) return false;

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
    snap.is_knocked           = FieldToBool (fields[i++]);
    snap.max_hp               = FieldToU32  (fields[i++]);
    snap.has_condition         = FieldToBool (fields[i++]);
    snap.has_deep_wound        = FieldToBool (fields[i++]);
    snap.has_bleeding          = FieldToBool (fields[i++]);
    snap.has_crippled          = FieldToBool (fields[i++]);
    snap.has_blind             = FieldToBool (fields[i++]);
    snap.has_poison            = FieldToBool (fields[i++]);
    snap.has_hex               = FieldToBool (fields[i++]);
    snap.has_degen_hex         = FieldToBool (fields[i++]);
    snap.has_enchantment       = FieldToBool (fields[i++]);
    snap.has_weapon_spell      = FieldToBool (fields[i++]);
    snap.is_holding            = FieldToBool (fields[i++]);
    snap.is_casting            = FieldToBool (fields[i++]);
    snap.skill_id             = FieldToU32  (fields[i++]);
    snap.weapon_item_type     = FieldToU8   (fields[i++]);
    snap.offhand_item_type    = FieldToU8   (fields[i++]);
    snap.weapon_item_id       = FieldToU16  (fields[i++]);
    snap.offhand_item_id      = FieldToU16  (fields[i++]);
    snap.move_x               = FieldToFloat(fields[i++]);
    snap.move_y               = FieldToFloat(fields[i++]);
    snap.visual_effects       = FieldToU16  (fields[i++]);
    snap.team_id              = FieldToU8   (fields[i++]);
    snap.weapon_type          = FieldToU16  (fields[i++]);
    snap.weapon_attack_speed  = FieldToFloat(fields[i++]);
    snap.attack_speed_modifier = FieldToFloat(fields[i++]);
    snap.dagger_status        = FieldToU8   (fields[i++]);
    snap.hp_pips              = FieldToFloat(fields[i++]);
    snap.model_state          = FieldToU32  (fields[i++]);
    snap.animation_code       = FieldToU32  (fields[i++]);
    snap.animation_id         = FieldToU32  (fields[i++]);
    snap.animation_speed      = FieldToFloat(fields[i++]);
    snap.animation_type       = FieldToFloat(fields[i++]);
    snap.in_spirit_range      = FieldToU32  (fields[i++]);
    snap.agent_model_type     = FieldToU16  (fields[i++]);
    snap.item_id              = FieldToU32  (fields[i++]);
    snap.item_extra_type      = FieldToU32  (fields[i++]);

    // Last field may contain trailing \r or whitespace
    if (i < nFields)
        snap.gadget_extra_type = FieldToU32(fields[i++]);

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
        if (!file.is_open()) return false;
        auto sz = static_cast<size_t>(file.tellg());
        std::vector<uint8_t> buf(sz);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(buf.data()), sz);
        file.close();
        content = DecompressGzipBuffer(buf);
    }
    else
    {
        std::ifstream file(filePath);
        if (!file.is_open()) return false;
        std::stringstream ss;
        ss << file.rdbuf();
        content = ss.str();
    }

    if (content.empty()) return false;

    out.agent_id = agentId;
    out.snapshots.reserve(content.size() / 120);

    const char* ptr = content.data();
    const char* end = ptr + content.size();

    while (ptr < end)
    {
        const char* lineEnd = static_cast<const char*>(memchr(ptr, '\n', end - ptr));
        if (!lineEnd) lineEnd = end;

        const char* effectiveEnd = lineEnd;
        if (effectiveEnd > ptr && *(effectiveEnd - 1) == '\r')
            effectiveEnd--;

        if (effectiveEnd > ptr)
        {
            AgentSnapshot snap;
            snap.raw_line.assign(ptr, effectiveEnd);
            if (ParseSnapshotLine(ptr, effectiveEnd, snap))
                out.snapshots.push_back(std::move(snap));
        }

        ptr = lineEnd + 1;
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
    if (!std::filesystem::exists(agentsDir) || !std::filesystem::is_directory(agentsDir))
    {
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

    if (uniqueFiles.empty())
    {
        progress->finished.store(true);
        return;
    }

    std::thread([progress, uniqueFiles = std::move(uniqueFiles)]()
    {
        for (const auto& af : uniqueFiles)
        {
            AgentReplayData ard;
            try
            {
                if (ParseAgentFile(af.path, af.id, ard))
                {
                    std::lock_guard<std::mutex> lock(progress->mutex);
                    progress->agents[af.id] = std::move(ard);
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
                    const MatchMeta& meta, int mapId)
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

        // Flag check (item_id-based, per map) — must come early
        if (IsFlagItemId(mapId, first.item_id))
        {
            ard.type         = AgentType::Flag;
            ard.categoryName = "Flag";
            continue;
        }

        // Map-specific item check (Vine Seed, Repair Kit, etc.)
        const char* itemName = LookupMapItem(mapId, first.item_id);
        if (itemName)
        {
            ard.type         = AgentType::Item;
            ard.categoryName = itemName;
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
