#include "pch.h"
#include "StoCParser.h"
#include <fstream>
#include <sstream>
#include <charconv>
#include <thread>
#include <algorithm>

// ---------------------------------------------------------------------------
// Self-contained DEFLATE decompressor (same as AgentSnapshotParser.cpp)
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
// Helpers
// ---------------------------------------------------------------------------

static std::string ReadFileContent(const std::filesystem::path& filePath)
{
    auto ext = filePath.extension().string();
    if (ext == ".gz")
    {
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return {};
        auto sz = static_cast<size_t>(file.tellg());
        std::vector<uint8_t> buf(sz);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(buf.data()), sz);
        file.close();
        return DecompressGzipBuffer(buf);
    }
    else
    {
        std::ifstream file(filePath);
        if (!file.is_open()) return {};
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }
}

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

static int ToInt(const char* begin, const char* end)
{
    int v = 0;
    std::from_chars(begin, end, v);
    return v;
}

static float ToFloat(const char* begin, const char* end)
{
    float v = 0.f;
    std::from_chars(begin, end, v);
    return v;
}

struct Token { const char* begin; const char* end; };

static int Tokenize(const char* begin, const char* end, Token* out, int maxTokens)
{
    int count = 0;
    const char* fieldStart = begin;
    for (const char* p = begin; p < end; ++p)
    {
        if (*p == ';')
        {
            if (count < maxTokens)
            {
                out[count].begin = fieldStart;
                out[count].end = p;
                count++;
            }
            fieldStart = p + 1;
        }
    }
    if (count < maxTokens && fieldStart <= end)
    {
        out[count].begin = fieldStart;
        out[count].end = end;
        count++;
    }
    return count;
}

struct LineInfo
{
    float       time;
    const char* dataStart;
    const char* lineEnd;
};

static bool ParseLineHeader(const char* lineBegin, const char* lineEnd, LineInfo& info)
{
    const char* bracketClose = static_cast<const char*>(
        memchr(lineBegin, ']', lineEnd - lineBegin));
    if (!bracketClose) return false;

    info.time = ParseTimestamp(lineBegin, bracketClose + 1);
    if (info.time < 0.f) return false;

    info.dataStart = bracketClose + 1;
    while (info.dataStart < lineEnd && (*info.dataStart == ' ' || *info.dataStart == '\t'))
        info.dataStart++;
    info.lineEnd = lineEnd;
    return true;
}

// ---------------------------------------------------------------------------
// Per-file parsers
// ---------------------------------------------------------------------------

static void ParseAgentEvents(const std::string& content, StoCData& data)
{
    const char* ptr = content.data();
    const char* end = ptr + content.size();

    while (ptr < end)
    {
        const char* lineEnd = static_cast<const char*>(memchr(ptr, '\n', end - ptr));
        if (!lineEnd) lineEnd = end;
        const char* effectiveEnd = lineEnd;
        if (effectiveEnd > ptr && *(effectiveEnd - 1) == '\r') effectiveEnd--;

        if (effectiveEnd > ptr)
        {
            LineInfo li;
            if (ParseLineHeader(ptr, effectiveEnd, li))
            {
                // GAME_SMSG_AGENT_MOVE_TO_POINT;agent_id;x;y;plane
                Token tok[6];
                int n = Tokenize(li.dataStart, li.lineEnd, tok, 6);
                if (n >= 5)
                {
                    AgentMovementEvent ev;
                    ev.time     = li.time;
                    ev.agent_id = ToInt(tok[1].begin, tok[1].end);
                    ev.x        = ToFloat(tok[2].begin, tok[2].end);
                    ev.y        = ToFloat(tok[3].begin, tok[3].end);
                    ev.plane    = ToFloat(tok[4].begin, tok[4].end);
                    ev.raw_line.assign(ptr, effectiveEnd);
                    data.agentMovement.push_back(std::move(ev));
                }
            }
        }
        ptr = lineEnd + 1;
    }
}

static void ParseSkillEvents(const std::string& content, StoCData& data)
{
    const char* ptr = content.data();
    const char* end = ptr + content.size();

    while (ptr < end)
    {
        const char* lineEnd = static_cast<const char*>(memchr(ptr, '\n', end - ptr));
        if (!lineEnd) lineEnd = end;
        const char* effectiveEnd = lineEnd;
        if (effectiveEnd > ptr && *(effectiveEnd - 1) == '\r') effectiveEnd--;

        if (effectiveEnd > ptr)
        {
            LineInfo li;
            if (ParseLineHeader(ptr, effectiveEnd, li))
            {
                Token tok[5];
                int n = Tokenize(li.dataStart, li.lineEnd, tok, 5);
                if (n >= 4)
                {
                    SkillActivationEvent ev;
                    ev.time = li.time;
                    ev.type.assign(tok[0].begin, tok[0].end);
                    ev.raw_line.assign(ptr, effectiveEnd);

                    // SKILL_ACTIVATED / INSTANT_SKILL_USED: type;skill_id;caster_id;target_id
                    // SKILL_FINISHED / SKILL_STOPPED:       type;caster_id;skill_id;target_id
                    if (ev.type == "SKILL_ACTIVATED" || ev.type == "INSTANT_SKILL_USED")
                    {
                        ev.skill_id  = ToInt(tok[1].begin, tok[1].end);
                        ev.caster_id = ToInt(tok[2].begin, tok[2].end);
                        ev.target_id = ToInt(tok[3].begin, tok[3].end);
                    }
                    else
                    {
                        ev.caster_id = ToInt(tok[1].begin, tok[1].end);
                        ev.skill_id  = ToInt(tok[2].begin, tok[2].end);
                        ev.target_id = ToInt(tok[3].begin, tok[3].end);
                    }

                    data.skill.push_back(std::move(ev));
                }
            }
        }
        ptr = lineEnd + 1;
    }
}

static void ParseAttackSkillEvents(const std::string& content, StoCData& data)
{
    const char* ptr = content.data();
    const char* end = ptr + content.size();

    while (ptr < end)
    {
        const char* lineEnd = static_cast<const char*>(memchr(ptr, '\n', end - ptr));
        if (!lineEnd) lineEnd = end;
        const char* effectiveEnd = lineEnd;
        if (effectiveEnd > ptr && *(effectiveEnd - 1) == '\r') effectiveEnd--;

        if (effectiveEnd > ptr)
        {
            LineInfo li;
            if (ParseLineHeader(ptr, effectiveEnd, li))
            {
                Token tok[5];
                int n = Tokenize(li.dataStart, li.lineEnd, tok, 5);
                if (n >= 4)
                {
                    AttackSkillEvent ev;
                    ev.time = li.time;
                    ev.type.assign(tok[0].begin, tok[0].end);
                    ev.raw_line.assign(ptr, effectiveEnd);

                    // ATTACK_SKILL_ACTIVATED: type;skill_id;caster_id;target_id
                    // ATTACK_SKILL_FINISHED / STOPPED: type;caster_id;skill_id;target_id
                    if (ev.type == "ATTACK_SKILL_ACTIVATED")
                    {
                        ev.skill_id  = ToInt(tok[1].begin, tok[1].end);
                        ev.caster_id = ToInt(tok[2].begin, tok[2].end);
                        ev.target_id = ToInt(tok[3].begin, tok[3].end);
                    }
                    else
                    {
                        ev.caster_id = ToInt(tok[1].begin, tok[1].end);
                        ev.skill_id  = ToInt(tok[2].begin, tok[2].end);
                        ev.target_id = ToInt(tok[3].begin, tok[3].end);
                    }

                    data.attackSkill.push_back(std::move(ev));
                }
            }
        }
        ptr = lineEnd + 1;
    }
}

static void ParseBasicAttackEvents(const std::string& content, StoCData& data)
{
    const char* ptr = content.data();
    const char* end = ptr + content.size();

    while (ptr < end)
    {
        const char* lineEnd = static_cast<const char*>(memchr(ptr, '\n', end - ptr));
        if (!lineEnd) lineEnd = end;
        const char* effectiveEnd = lineEnd;
        if (effectiveEnd > ptr && *(effectiveEnd - 1) == '\r') effectiveEnd--;

        if (effectiveEnd > ptr)
        {
            LineInfo li;
            if (ParseLineHeader(ptr, effectiveEnd, li))
            {
                Token tok[5];
                int n = Tokenize(li.dataStart, li.lineEnd, tok, 5);
                if (n >= 3)
                {
                    BasicAttackEvent ev;
                    ev.time = li.time;
                    ev.type.assign(tok[0].begin, tok[0].end);
                    ev.raw_line.assign(ptr, effectiveEnd);

                    if (ev.type == "ATTACK_STARTED")
                    {
                        // ATTACK_STARTED;caster_id;target_id
                        ev.caster_id = ToInt(tok[1].begin, tok[1].end);
                        ev.target_id = ToInt(tok[2].begin, tok[2].end);
                        ev.skill_id  = 0;
                    }
                    else
                    {
                        // ATTACK_FINISHED/STOPPED;caster_id;skill_id;target_id
                        if (n >= 4)
                        {
                            ev.caster_id = ToInt(tok[1].begin, tok[1].end);
                            ev.skill_id  = ToInt(tok[2].begin, tok[2].end);
                            ev.target_id = ToInt(tok[3].begin, tok[3].end);
                        }
                        else
                        {
                            ev.caster_id = ToInt(tok[1].begin, tok[1].end);
                            ev.target_id = ToInt(tok[2].begin, tok[2].end);
                        }
                    }

                    data.basicAttack.push_back(std::move(ev));
                }
            }
        }
        ptr = lineEnd + 1;
    }
}

static void ParseCombatEvents(const std::string& content, StoCData& data)
{
    const char* ptr = content.data();
    const char* end = ptr + content.size();

    while (ptr < end)
    {
        const char* lineEnd = static_cast<const char*>(memchr(ptr, '\n', end - ptr));
        if (!lineEnd) lineEnd = end;
        const char* effectiveEnd = lineEnd;
        if (effectiveEnd > ptr && *(effectiveEnd - 1) == '\r') effectiveEnd--;

        if (effectiveEnd > ptr)
        {
            LineInfo li;
            if (ParseLineHeader(ptr, effectiveEnd, li))
            {
                Token tok[6];
                int n = Tokenize(li.dataStart, li.lineEnd, tok, 6);
                if (n >= 3)
                {
                    CombatEvent ev;
                    ev.time = li.time;
                    ev.type.assign(tok[0].begin, tok[0].end);
                    ev.raw_line.assign(ptr, effectiveEnd);

                    if (ev.type == "DAMAGE" && n >= 5)
                    {
                        ev.caster_id   = ToInt(tok[1].begin, tok[1].end);
                        ev.target_id   = ToInt(tok[2].begin, tok[2].end);
                        ev.value       = ToFloat(tok[3].begin, tok[3].end);
                        ev.damage_type = ToInt(tok[4].begin, tok[4].end);
                    }
                    else if (ev.type == "KNOCKED_DOWN" && n >= 3)
                    {
                        ev.target_id = ToInt(tok[1].begin, tok[1].end);
                        ev.caster_id = ToInt(tok[2].begin, tok[2].end);
                    }
                    else if (ev.type == "INTERRUPTED" && n >= 4)
                    {
                        ev.caster_id = ToInt(tok[1].begin, tok[1].end);
                        ev.value     = static_cast<float>(ToInt(tok[2].begin, tok[2].end));
                        ev.target_id = ToInt(tok[3].begin, tok[3].end);
                    }

                    data.combat.push_back(std::move(ev));
                }
            }
        }
        ptr = lineEnd + 1;
    }
}

static const char* JumboTypeName(int typeId)
{
    switch (typeId) {
    case 0:  return "BASE_UNDER_ATTACK";
    case 1:  return "GUILD_LORD_UNDER_ATTACK";
    case 3:  return "CAPTURED_SHRINE";
    case 5:  return "CAPTURED_TOWER";
    case 6:  return "PARTY_DEFEATED";
    case 9:  return "MORALE_BOOST";
    case 16: return "VICTORY";
    case 17: return "FLAWLESS_VICTORY";
    default: return "UNKNOWN";
    }
}

static void ParseJumboMessages(const std::string& content, StoCData& data)
{
    const char* ptr = content.data();
    const char* end = ptr + content.size();

    while (ptr < end)
    {
        const char* lineEnd = static_cast<const char*>(memchr(ptr, '\n', end - ptr));
        if (!lineEnd) lineEnd = end;
        const char* effectiveEnd = lineEnd;
        if (effectiveEnd > ptr && *(effectiveEnd - 1) == '\r') effectiveEnd--;

        if (effectiveEnd > ptr)
        {
            LineInfo li;
            if (ParseLineHeader(ptr, effectiveEnd, li))
            {
                // Actual format: GAME_SMSG_JUMBO_MESSAGE;type_id;party_value (Party X)
                Token tok[4];
                int n = Tokenize(li.dataStart, li.lineEnd, tok, 4);
                if (n >= 3)
                {
                    JumboMessageEvent ev;
                    ev.time = li.time;
                    ev.raw_line.assign(ptr, effectiveEnd);

                    int typeId = ToInt(tok[1].begin, tok[1].end);
                    ev.message = JumboTypeName(typeId);

                    // tok[2] is "party_value (Party X)" — extract the integer before the space
                    const char* valEnd = tok[2].begin;
                    while (valEnd < tok[2].end && *valEnd != ' ')
                        valEnd++;
                    ev.party_value = ToInt(tok[2].begin, valEnd);

                    data.jumbo.push_back(std::move(ev));
                }
            }
        }
        ptr = lineEnd + 1;
    }
}

static void ParseUnknownEvents(const std::string& content, StoCData& data)
{
    const char* ptr = content.data();
    const char* end = ptr + content.size();

    while (ptr < end)
    {
        const char* lineEnd = static_cast<const char*>(memchr(ptr, '\n', end - ptr));
        if (!lineEnd) lineEnd = end;
        const char* effectiveEnd = lineEnd;
        if (effectiveEnd > ptr && *(effectiveEnd - 1) == '\r') effectiveEnd--;

        if (effectiveEnd > ptr)
        {
            LineInfo li;
            if (ParseLineHeader(ptr, effectiveEnd, li))
            {
                UnknownEvent ev;
                ev.time = li.time;
                ev.raw_line.assign(ptr, effectiveEnd);
                data.unknown.push_back(std::move(ev));
            }
        }
        ptr = lineEnd + 1;
    }
}

// ---------------------------------------------------------------------------
// File dispatch table
// ---------------------------------------------------------------------------

struct StoCFileEntry
{
    const char* filename;
    void (*parser)(const std::string& content, StoCData& data);
};

static const StoCFileEntry kStoCFiles[] = {
    { "agent_events",       ParseAgentEvents },
    { "skill_events",       ParseSkillEvents },
    { "attack_skill_events", ParseAttackSkillEvents },
    { "basic_attack_events", ParseBasicAttackEvents },
    { "combat_events",      ParseCombatEvents },
    { "jumbo_messages",     ParseJumboMessages },
    { "unknown_events",     ParseUnknownEvents },
};

static constexpr int kNumStoCFiles = static_cast<int>(sizeof(kStoCFiles) / sizeof(kStoCFiles[0]));

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void LaunchStoCParsing(const std::filesystem::path& matchFolder,
                       std::shared_ptr<StoCParseProgress> progress)
{
    auto stocDir = matchFolder / "StoC";
    if (!std::filesystem::exists(stocDir) || !std::filesystem::is_directory(stocDir))
    {
        progress->finished.store(true);
        return;
    }

    progress->files_total.store(kNumStoCFiles);

    std::thread([progress, stocDir]()
    {
        StoCData localData;

        for (int i = 0; i < kNumStoCFiles; i++)
        {
            try
            {
                auto gzPath  = stocDir / (std::string(kStoCFiles[i].filename) + ".txt.gz");
                auto txtPath = stocDir / (std::string(kStoCFiles[i].filename) + ".txt");

                std::filesystem::path filePath;
                if (std::filesystem::exists(gzPath))
                    filePath = gzPath;
                else if (std::filesystem::exists(txtPath))
                    filePath = txtPath;

                if (!filePath.empty())
                {
                    std::string content = ReadFileContent(filePath);
                    if (!content.empty())
                        kStoCFiles[i].parser(content, localData);
                }
            }
            catch (const std::exception& e)
            {
                std::lock_guard<std::mutex> lock(progress->mutex);
                progress->errors.push_back(
                    std::format("{}: {}", kStoCFiles[i].filename, e.what()));
                progress->has_error.store(true);
            }

            progress->files_done.fetch_add(1);
        }

        {
            std::lock_guard<std::mutex> lock(progress->mutex);
            progress->data = std::move(localData);
        }
        progress->finished.store(true);

    }).detach();
}

bool PollStoCParseCompletion(ReplayContext& ctx)
{
    if (ctx.stocLoaded) return true;
    if (!ctx.stocParseProgress) return false;
    if (!ctx.stocParseProgress->finished.load()) return false;

    {
        std::lock_guard<std::mutex> lock(ctx.stocParseProgress->mutex);
        ctx.stocData = std::move(ctx.stocParseProgress->data);
    }

    ctx.stocLoaded = true;
    return true;
}
