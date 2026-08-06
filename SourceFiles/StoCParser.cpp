#include "pch.h"
#include "StoCParser.h"
#include <fstream>
#include <sstream>
#include <charconv>
#include <thread>
#include <algorithm>

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
                    else if (ev.type == "HEAL" && n >= 5)
                    {
                        ev.caster_id   = ToInt(tok[1].begin, tok[1].end);
                        ev.target_id   = ToInt(tok[2].begin, tok[2].end);
                        ev.value       = ToFloat(tok[3].begin, tok[3].end);
                        std::string_view dmgToken(tok[4].begin, tok[4].end - tok[4].begin);
                        ev.damage_type = (dmgToken == "ARMORIGNORING") ? 55 : ToInt(tok[4].begin, tok[4].end);
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
                    else if (n >= 3)
                    {
                        ev.caster_id = ToInt(tok[1].begin, tok[1].end);
                        ev.target_id = (n >= 4) ? ToInt(tok[2].begin, tok[2].end) : 0;
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
    case 11: return "NEUTRALIZED_SHRINE";
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

static void ParseLordEvents(const std::string& content, StoCData& data)
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
                Token tok[10];
                int n = Tokenize(li.dataStart, li.lineEnd, tok, 10);
                if (n >= 9)
                {
                    std::string_view typeName(tok[0].begin,
                                              tok[0].end - tok[0].begin);
                    if (typeName == "LORD_DAMAGE")
                    {
                        StoCLordDamageEvent ev;
                        ev.time           = li.time;
                        ev.caster_id      = ToInt(tok[1].begin, tok[1].end);
                        ev.target_id      = ToInt(tok[2].begin, tok[2].end);
                        ev.value          = ToFloat(tok[3].begin, tok[3].end);
                        ev.damage_type    = ToInt(tok[4].begin, tok[4].end);
                        ev.attacking_team = ToInt(tok[5].begin, tok[5].end);
                        ev.damage         = ToInt(tok[6].begin, tok[6].end);
                        ev.damage_after   = ToInt(tok[8].begin, tok[8].end);
                        data.lordDamage.push_back(std::move(ev));
                    }
                }
            }
        }
        ptr = lineEnd + 1;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle events (AGENT_ADD / AGENT_REMOVE)
// ---------------------------------------------------------------------------

static void ParseLifecycleEvents(const std::string& content, StoCData& data)
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
                Token tok[8];
                int n = Tokenize(li.dataStart, li.lineEnd, tok, 8);
                if (n >= 2)
                {
                    std::string_view typeName(tok[0].begin,
                                              tok[0].end - tok[0].begin);
                    if (typeName == "AGENT_ADD" && n >= 7)
                    {
                        LifecycleEvent ev;
                        ev.time       = li.time;
                        ev.isAdd      = true;
                        ev.agent_id   = ToInt(tok[1].begin, tok[1].end);
                        ev.agent_type = static_cast<uint32_t>(
                            strtoul(std::string(tok[2].begin, tok[2].end).c_str(), nullptr, 10));
                        ev.type_code  = ToInt(tok[3].begin, tok[3].end);
                        ev.x          = ToFloat(tok[4].begin, tok[4].end);
                        ev.y          = ToFloat(tok[5].begin, tok[5].end);
                        ev.speed      = ToFloat(tok[6].begin, tok[6].end);
                        data.lifecycle.push_back(std::move(ev));
                    }
                    else if (typeName == "AGENT_REMOVE" && n >= 2)
                    {
                        LifecycleEvent ev;
                        ev.time     = li.time;
                        ev.isAdd    = false;
                        ev.agent_id = ToInt(tok[1].begin, tok[1].end);
                        data.lifecycle.push_back(std::move(ev));
                    }
                }
            }
        }
        ptr = lineEnd + 1;
    }
}

// ---------------------------------------------------------------------------
// Map object manipulation events (MAP_OBJECT / MAP_OBJECT_STATE)
// ---------------------------------------------------------------------------

static void ParseMapObjectEvents(const std::string& content, StoCData& data)
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
                if (n >= 4)
                {
                    std::string_view typeName(tok[0].begin,
                                              tok[0].end - tok[0].begin);
                    if (typeName == "MAP_OBJECT" && n >= 4)
                    {
                        MapObjectEvent ev;
                        ev.time            = li.time;
                        ev.isState         = false;
                        ev.object_id       = static_cast<uint32_t>(
                            strtoul(std::string(tok[1].begin, tok[1].end).c_str(), nullptr, 10));
                        ev.animation_type  = ToInt(tok[2].begin, tok[2].end);
                        ev.animation_stage = ToInt(tok[3].begin, tok[3].end);
                        data.mapObject.push_back(std::move(ev));
                    }
                    else if (typeName == "MAP_OBJECT_STATE" && n >= 4)
                    {
                        MapObjectEvent ev;
                        ev.time      = li.time;
                        ev.isState   = true;
                        ev.object_id = static_cast<uint32_t>(
                            strtoul(std::string(tok[1].begin, tok[1].end).c_str(), nullptr, 10));
                        ev.unk1      = ToInt(tok[2].begin, tok[2].end);
                        ev.state     = ToInt(tok[3].begin, tok[3].end);
                        data.mapObject.push_back(std::move(ev));
                    }
                }
            }
        }
        ptr = lineEnd + 1;
    }
}

// ---------------------------------------------------------------------------
// Door events (door_events.txt)
// ---------------------------------------------------------------------------

static void ParseDoorEvents(const std::string& content, StoCData& data)
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
                    std::string_view typeName(tok[0].begin,
                                              tok[0].end - tok[0].begin);
                    if (typeName == "DOOR_ANIMATION" && n >= 5)
                    {
                        DoorEvent ev;
                        ev.time            = li.time;
                        ev.isState         = false;
                        ev.object_id       = static_cast<uint32_t>(
                            strtoul(std::string(tok[1].begin, tok[1].end).c_str(), nullptr, 10));
                        ev.animation_type  = ToInt(tok[2].begin, tok[2].end);
                        ev.animation_stage = ToInt(tok[3].begin, tok[3].end);
                        ev.status          = ToInt(tok[4].begin, tok[4].end);
                        data.doorEvents.push_back(std::move(ev));
                    }
                    else if (typeName == "DOOR_STATE" && n >= 3)
                    {
                        DoorEvent ev;
                        ev.time      = li.time;
                        ev.isState   = true;
                        ev.object_id = static_cast<uint32_t>(
                            strtoul(std::string(tok[1].begin, tok[1].end).c_str(), nullptr, 10));
                        ev.state     = ToInt(tok[2].begin, tok[2].end);
                        data.doorEvents.push_back(std::move(ev));
                    }
                }
            }
        }
        ptr = lineEnd + 1;
    }

    std::sort(data.doorEvents.begin(), data.doorEvents.end(),
              [](const DoorEvent& a, const DoorEvent& b) { return a.time < b.time; });
}

// ---------------------------------------------------------------------------
// Flag events (flag_events.txt — GvG flag StoC packets, codes 0-6)
// ---------------------------------------------------------------------------

static void ParseFlagEvents(const std::string& content, StoCData& data)
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
                if (n >= 1)
                {
                    int code = ToInt(tok[0].begin, tok[0].end);
                    switch (code)
                    {
                    case 0: // FLAG_PICKUP: 0;item_id;player_agent_id;team_code
                        if (n >= 4) {
                            FlagPickupEvent ev;
                            ev.time             = li.time;
                            ev.item_id          = ToInt(tok[1].begin, tok[1].end);
                            ev.player_agent_id  = ToInt(tok[2].begin, tok[2].end);
                            ev.team_code        = ToInt(tok[3].begin, tok[3].end);
                            ev.raw_line.assign(ptr, effectiveEnd);
                            data.flagEvents.pickups.push_back(std::move(ev));
                        }
                        break;

                    case 1: // FLAG_DROP: 1;player_agent_id;team_code
                        if (n >= 3) {
                            FlagDropEvent ev;
                            ev.time             = li.time;
                            ev.player_agent_id  = ToInt(tok[1].begin, tok[1].end);
                            ev.team_code        = ToInt(tok[2].begin, tok[2].end);
                            ev.raw_line.assign(ptr, effectiveEnd);
                            data.flagEvents.drops.push_back(std::move(ev));
                        }
                        break;

                    case 2: // FLAG_STATE: 2;team_code;item_id;state
                        if (n >= 4) {
                            FlagStateEvent ev;
                            ev.time      = li.time;
                            ev.team_code = ToInt(tok[1].begin, tok[1].end);
                            ev.item_id   = ToInt(tok[2].begin, tok[2].end);
                            ev.state     = static_cast<uint32_t>(
                                strtoul(std::string(tok[3].begin, tok[3].end).c_str(), nullptr, 10));
                            ev.raw_line.assign(ptr, effectiveEnd);
                            data.flagEvents.states.push_back(std::move(ev));
                        }
                        break;

                    case 3: // FLAG_ITEM: 3;item_id;model_id;extra_id;type
                        if (n >= 5) {
                            FlagItemEvent ev;
                            ev.time     = li.time;
                            ev.item_id  = ToInt(tok[1].begin, tok[1].end);
                            ev.model_id = ToInt(tok[2].begin, tok[2].end);
                            ev.extra_id = static_cast<uint32_t>(
                                strtoul(std::string(tok[3].begin, tok[3].end).c_str(), nullptr, 10));
                            ev.type     = ToInt(tok[4].begin, tok[4].end);
                            ev.raw_line.assign(ptr, effectiveEnd);
                            data.flagEvents.items.push_back(std::move(ev));
                        }
                        break;

                    case 4: // FLAG_STAND: 4;stand_agent_id;sub_field;value
                        if (n >= 4) {
                            FlagStandEvent ev;
                            ev.time           = li.time;
                            ev.stand_agent_id = ToInt(tok[1].begin, tok[1].end);
                            ev.sub_field      = ToInt(tok[2].begin, tok[2].end);
                            ev.value          = ToInt(tok[3].begin, tok[3].end);
                            ev.raw_line.assign(ptr, effectiveEnd);
                            data.flagEvents.stands.push_back(std::move(ev));
                        }
                        break;

                    case 5: // FLAG_SPAWN: 5;agent_id;unk;object_id
                        if (n >= 4) {
                            FlagSpawnEvent ev;
                            ev.time      = li.time;
                            ev.agent_id  = ToInt(tok[1].begin, tok[1].end);
                            ev.unk       = ToInt(tok[2].begin, tok[2].end);
                            ev.object_id = ToInt(tok[3].begin, tok[3].end);
                            ev.raw_line.assign(ptr, effectiveEnd);
                            data.flagEvents.spawns.push_back(std::move(ev));
                        }
                        break;

                    case 6: // FLAG_ANNOUNCE: 6;action;template_id;team
                        if (n >= 4) {
                            FlagAnnounceEvent ev;
                            ev.time        = li.time;
                            ev.action      = ToInt(tok[1].begin, tok[1].end);
                            ev.template_id = ToInt(tok[2].begin, tok[2].end);
                            ev.team        = ToInt(tok[3].begin, tok[3].end);
                            ev.raw_line.assign(ptr, effectiveEnd);
                            data.flagEvents.announces.push_back(std::move(ev));
                        }
                        break;
                    }
                }
            }
        }
        ptr = lineEnd + 1;
    }
}

// ---------------------------------------------------------------------------
// Sound events (sound_events.txt — captured PlaySound/PlayMusic calls)
// ---------------------------------------------------------------------------

static void ParseSoundEvents(const std::string& content, StoCData& data)
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
                // SOUND;file_id;sound_type;flags;pos_x;pos_y;pos_z;cause_agent_id;cause_skill_id;cam_dist;cam_angle
                Token tok[11];
                int n = Tokenize(li.dataStart, li.lineEnd, tok, 11);
                if (n >= 9)
                {
                    SoundLogEvent ev;
                    ev.time           = li.time;
                    ev.file_id        = static_cast<uint32_t>(ToInt(tok[1].begin, tok[1].end));
                    ev.sound_type     = static_cast<uint8_t>(ToInt(tok[2].begin, tok[2].end));
                    ev.flags          = static_cast<uint32_t>(ToInt(tok[3].begin, tok[3].end));
                    ev.x              = ToFloat(tok[4].begin, tok[4].end);
                    ev.y              = ToFloat(tok[5].begin, tok[5].end);
                    ev.z              = ToFloat(tok[6].begin, tok[6].end);
                    ev.cause_agent_id = ToInt(tok[7].begin, tok[7].end);
                    ev.cause_skill_id = ToInt(tok[8].begin, tok[8].end);
                    ev.positional     = (ev.flags & 0x1400u) == 0x1400u;
                    data.soundEvents.push_back(std::move(ev));
                }
            }
        }
        ptr = lineEnd + 1;
    }
}

// ---------------------------------------------------------------------------
// Equipment (weapon/armour skins, dyes and raw mod words)
// ---------------------------------------------------------------------------

static void ParseEquipmentEvents(const std::string& content, StoCData& data)
{
    Equipment::ParseContent(content, data.equipment);
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
    { "agent_events",                  ParseAgentEvents },
    { "skill_events",                  ParseSkillEvents },
    { "attack_skill_events",           ParseAttackSkillEvents },
    { "basic_attack_events",           ParseBasicAttackEvents },
    { "combat_events",                 ParseCombatEvents },
    { "jumbo_messages",                ParseJumboMessages },
    { "unknown_events",                ParseUnknownEvents },
    { "lord_events",                   ParseLordEvents },
    { "lifecycle_events",              ParseLifecycleEvents },
    { "manipulate_map_object_events",  ParseMapObjectEvents },
    { "door_events",                   ParseDoorEvents },
    { "flag_events",                   ParseFlagEvents },
    { "sound_events",                  ParseSoundEvents },
    { "equipment_events",              ParseEquipmentEvents },
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
