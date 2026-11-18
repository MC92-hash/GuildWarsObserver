#include "pch.h"
#include "MatchLink.h"
#include "RunLog.h"
#include "ReplayLibrary.h"
#include <map>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    std::string_view Trim(std::string_view s)
    {
        while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.front())) || s.front() == '"' ||
                              s.front() == '\'' || s.front() == '<'))
            s.remove_prefix(1);
        while (!s.empty() && (std::isspace(static_cast<unsigned char>(s.back())) || s.back() == '"' ||
                              s.back() == '\'' || s.back() == '>'))
            s.remove_suffix(1);
        return s;
    }

    bool StartsWithNoCase(std::string_view s, std::string_view prefix)
    {
        if (s.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); i++)
            if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        return true;
    }

    // Strips the scheme, host and /watch path. Returns the query (after '?'), or nullopt when the
    // text is not one of the accepted forms.
    std::optional<std::string_view> QueryOf(std::string_view s)
    {
        s = Trim(s);
        if (StartsWithNoCase(s, "gwobserver://"))
            s.remove_prefix(13);
        else
        {
            if (StartsWithNoCase(s, "https://")) s.remove_prefix(8);
            else if (StartsWithNoCase(s, "http://")) s.remove_prefix(7);
            if (StartsWithNoCase(s, "www.")) s.remove_prefix(4);
            if (!StartsWithNoCase(s, "gwobserver.com/")) return std::nullopt;
            s.remove_prefix(15);
        }
        if (!StartsWithNoCase(s, "watch")) return std::nullopt;
        s.remove_prefix(5);
        if (StartsWithNoCase(s, ".html")) s.remove_prefix(5);
        if (!s.empty() && s.front() == '/') s.remove_prefix(1);
        if (s.empty() || s.front() != '?') return std::nullopt;
        s.remove_prefix(1);
        // A fragment is not part of the query.
        const size_t hash = s.find('#');
        if (hash != std::string_view::npos) s = s.substr(0, hash);
        return s;
    }

    int HexVal(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    std::string PercentDecode(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++)
        {
            if (s[i] == '%' && i + 2 < s.size())
            {
                const int hi = HexVal(s[i + 1]), lo = HexVal(s[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    out.push_back(static_cast<char>(hi * 16 + lo));
                    i += 2;
                    continue;
                }
            }
            out.push_back(s[i] == '+' ? ' ' : s[i]);
        }
        return out;
    }

    // keepTilde = false for the display fields, where '~' separates the parts.
    std::string PercentEncode(std::string_view s, bool keepTilde = true)
    {
        static const char* kHex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : s)
        {
            if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || (c == '~' && keepTilde))
                out.push_back(static_cast<char>(c));
            else
            {
                out.push_back('%');
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 15]);
            }
        }
        return out;
    }

    // A whole string as a finite float.
    bool ToFloat(const std::string& s, float& out)
    {
        if (s.empty()) return false;
        char* end = nullptr;
        const float v = std::strtof(s.c_str(), &end);
        if (end != s.c_str() + s.size() || !std::isfinite(v)) return false;
        out = v;
        return true;
    }

    bool ToInt(const std::string& s, int& out)
    {
        if (s.empty() || s.size() > 9) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        out = std::atoi(s.c_str());
        return true;
    }

    // 754, 754.5, 12:34, 1:02:03, 12m34s, 1h2m3s, 34s.
    bool ParseTime(const std::string& s, float& out)
    {
        float v = 0.f;
        if (ToFloat(s, v)) { if (v < 0.f) return false; out = v; return true; }

        if (s.find(':') != std::string::npos)
        {
            float total = 0.f;
            size_t start = 0;
            int parts = 0;
            while (true)
            {
                const size_t colon = s.find(':', start);
                const std::string part = s.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
                float pv = 0.f;
                if (!ToFloat(part, pv) || pv < 0.f) return false;
                total = total * 60.f + pv;
                if (++parts > 3) return false;
                if (colon == std::string::npos) break;
                start = colon + 1;
            }
            out = total;
            return true;
        }

        // Unit suffixes, in h > m > s order.
        float total = 0.f;
        std::string num;
        int lastUnit = 4;   // h = 3, m = 2, s = 1
        bool any = false;
        for (char c : s)
        {
            const char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if ((c >= '0' && c <= '9') || c == '.') { num.push_back(c); continue; }
            const int unit = lc == 'h' ? 3 : lc == 'm' ? 2 : lc == 's' ? 1 : 0;
            if (unit == 0 || unit >= lastUnit) return false;
            float pv = 0.f;
            if (!ToFloat(num, pv)) return false;
            total += pv * (unit == 3 ? 3600.f : unit == 2 ? 60.f : 1.f);
            num.clear();
            lastUnit = unit;
            any = true;
        }
        if (!num.empty() || !any) return false;
        out = total;
        return true;
    }

    std::vector<std::string> SplitComma(const std::string& s)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true)
        {
            const size_t comma = s.find(',', start);
            parts.push_back(s.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return parts;
    }

    // Folder names are used as a key and as one path component of the match cache: nothing that
    // can climb out of it.
    bool SafeFolder(const std::string& f)
    {
        if (f.empty() || f.size() > 200 || f == "." || f == "..") return false;
        if (f.find("..") != std::string::npos) return false;
        for (unsigned char c : f)
            if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
                c == '<' || c == '>' || c == '|')
                return false;
        return true;
    }

    // Seven decimal numbers joined by '-'.
    bool ValidCapeId(const std::string& id)
    {
        if (id.empty() || id.size() > 48) return false;
        int dashes = 0;
        bool digit = false;
        for (char c : id)
        {
            if (c == '-') { if (!digit) return false; dashes++; digit = false; }
            else if (c >= '0' && c <= '9') digit = true;
            else return false;
        }
        return digit && dashes == 6;
    }

    std::string FormatFloat(float v, int decimals)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", decimals, v);
        std::string s = buf;
        if (s == "-0" || s.rfind("-0.", 0) == 0)
        {
            bool allZero = true;
            for (char c : s) if (c >= '1' && c <= '9') allZero = false;
            if (allZero) s.erase(0, 1);
        }
        return s;
    }
}

namespace MatchLinks
{
std::string FormatQuery(const MatchLink& link)
{
    std::string q = "v=" + std::to_string(link.version > 0 ? link.version : kVersion);
    q += "&m=" + PercentEncode(link.folder);

    // Whole seconds read best; a tenth is kept when the moment is not on one.
    const float t = std::max(0.f, link.t);
    const float tenths = std::round(t * 10.f) / 10.f;
    q += "&t=" + (std::fabs(tenths - std::round(tenths)) < 0.05f ? std::to_string(static_cast<int>(std::round(tenths)))
                                                                    : FormatFloat(tenths, 1));
    if (link.agent >= 0)
        q += "&p=" + std::to_string(link.agent);
    if (link.cam == MatchLink::Cam::Follow)
        q += "&cam=f," + FormatFloat(link.yaw, 3) + "," + FormatFloat(link.pitch, 3) + "," +
             std::to_string(static_cast<int>(std::lround(link.dist)));
    else if (link.cam == MatchLink::Cam::Free)
        q += "&cam=c," + std::to_string(static_cast<int>(std::lround(link.pos.x))) + "," +
             std::to_string(static_cast<int>(std::lround(link.pos.y))) + "," +
             std::to_string(static_cast<int>(std::lround(link.pos.z))) + "," + FormatFloat(link.yaw, 3) + "," +
             FormatFloat(link.pitch, 3);
    q += link.paused ? "&pause=1" : "&pause=0";

    // Display only (the website's card); never read back when the link is opened.
    for (int i = 0; i < 2; i++)
    {
        const MatchLink::Team& team = link.teams[i];
        if (!team.present) continue;
        q += (i == 0 ? "&g1=" : "&g2=") + PercentEncode(team.name, false) + "~" + std::to_string(team.rank) + "~" +
             std::to_string(team.rating) + "~" + team.capeId + "~" + team.profs;
    }
    if (!link.occasion.empty())
        q += "&o=" + PercentEncode(link.occasion);
    return q;
}

std::string FormatHttps(const MatchLink& link)
{
    return std::string(kWebBase) + FormatQuery(link);
}

bool LooksLikeLink(std::string_view text)
{
    return QueryOf(text).has_value();
}

std::optional<MatchLink> Parse(std::string_view text, ParseError* error)
{
    auto fail = [&](ParseError e) -> std::optional<MatchLink>
    {
        if (error) *error = e;
        return std::nullopt;
    };
    if (error) *error = ParseError::None;

    const auto query = QueryOf(text);
    if (!query) return fail(ParseError::NotALink);

    MatchLink link;
    bool haveVersion = false, haveFolder = false;
    std::string camValue;

    std::string_view q = *query;
    while (!q.empty())
    {
        const size_t amp = q.find('&');
        const std::string_view pair = q.substr(0, amp);
        q = amp == std::string_view::npos ? std::string_view{} : q.substr(amp + 1);
        if (pair.empty()) continue;

        const size_t eq = pair.find('=');
        const std::string key = PercentDecode(pair.substr(0, eq));
        const std::string value = eq == std::string_view::npos ? std::string() : PercentDecode(pair.substr(eq + 1));

        if (key == "v")
        {
            int v = 0;
            if (!ToInt(value, v) || v < 1) return fail(ParseError::NotALink);
            link.version = v;
            haveVersion = true;
        }
        else if (key == "m")
        {
            if (SafeFolder(value)) { link.folder = value; haveFolder = true; }
        }
        else if (key == "t")
        {
            float t = 0.f;
            if (ParseTime(value, t) && t < 24.f * 3600.f) link.t = t;
        }
        else if (key == "p")
        {
            int p = -1;
            if (ToInt(value, p)) link.agent = p;
        }
        else if (key == "g1" || key == "g2")
        {
            // <name>~<rank>~<rating>~<capeId>~<profs>, split BEFORE decoding (a name may hold '~').
            const std::string_view raw = eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1);
            std::vector<std::string_view> parts;
            size_t from = 0;
            while (true)
            {
                const size_t tilde = raw.find('~', from);
                parts.push_back(raw.substr(from, tilde == std::string_view::npos ? std::string_view::npos : tilde - from));
                if (tilde == std::string_view::npos) break;
                from = tilde + 1;
            }
            if (parts.size() == 5)
            {
                MatchLink::Team& team = link.teams[key == "g1" ? 0 : 1];
                team = {};
                team.present = true;
                team.name = PercentDecode(parts[0]);
                int v = 0;
                if (ToInt(std::string(parts[1]), v)) team.rank = v;
                if (ToInt(std::string(parts[2]), v)) team.rating = v;
                const std::string cape(parts[3]);
                if (ValidCapeId(cape)) team.capeId = cape;
                const std::string profs(parts[4]);
                bool hex = profs.size() <= 16;
                for (char c : profs)
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) hex = false;
                if (hex) team.profs = profs;
            }
        }
        else if (key == "o")
        {
            if (value.size() <= 64) link.occasion = value;
        }
        else if (key == "cam")
            camValue = value;
        else if (key == "pause")
        {
            if (value == "0") link.paused = false;
            else if (value == "1") link.paused = true;
        }
        // Anything else: a field from a later format, ignored.
    }

    if (!haveVersion) return fail(ParseError::NotALink);
    if (link.version > kVersion) return fail(ParseError::NewerVersion);
    if (!haveFolder) return fail(ParseError::NoMatch);

    if (!camValue.empty())
    {
        const auto parts = SplitComma(camValue);
        float a = 0.f, b = 0.f, c = 0.f, d = 0.f, e = 0.f;
        if (parts.size() == 4 && parts[0] == "f" && link.agent >= 0 && ToFloat(parts[1], a) &&
            ToFloat(parts[2], b) && ToFloat(parts[3], c) && c > 0.f && std::fabs(b) < 2.f)
        {
            link.cam = MatchLink::Cam::Follow;
            link.yaw = a;
            link.pitch = b;
            link.dist = c;
        }
        else if (parts.size() == 6 && parts[0] == "c" && ToFloat(parts[1], a) && ToFloat(parts[2], b) &&
                 ToFloat(parts[3], c) && ToFloat(parts[4], d) && ToFloat(parts[5], e) && std::fabs(e) < 2.f &&
                 std::fabs(a) < 1e6f && std::fabs(b) < 1e6f && std::fabs(c) < 1e6f)
        {
            link.cam = MatchLink::Cam::Free;
            link.pos = DirectX::XMFLOAT3(a, b, c);
            link.yaw = d;
            link.pitch = e;
        }
    }
    return link;
}

std::string CapeId(const CapeData& c)
{
    return std::to_string(c.shape) + "-" + std::to_string(c.detail) + "-" + std::to_string(c.emblem) + "-" +
           std::to_string(c.trim) + "-" + std::to_string(c.bg_color) + "-" + std::to_string(c.detail_color) + "-" +
           std::to_string(c.emblem_color);
}

void FillDisplay(MatchLink& link, const MatchMeta& m)
{
    // The folder name's tags, "[tag1]vs[tag2]" (ReplayWindow::ResolveCapeTextures).
    std::string tag[2];
    const std::string& fn = m.folder_name;
    const size_t vs = fn.find("]vs[");
    if (vs != std::string::npos)
    {
        const size_t open1 = fn.rfind('[', vs);
        const size_t close2 = fn.find(']', vs + 4);
        if (open1 != std::string::npos && close2 != std::string::npos)
        {
            tag[0] = fn.substr(open1 + 1, vs - open1 - 1);
            tag[1] = fn.substr(vs + 4, close2 - (vs + 4));
        }
    }

    // The same order as the loading screen's getTeamCape: the folder tag, then the party's
    // dominant guild, then the guild filed under the party id.
    auto teamGuild = [&](const std::string& partyId, const std::string& folderTag) -> const GuildMeta*
    {
        if (!folderTag.empty())
            for (const auto& [id, gm] : m.guilds)
                if (gm.tag == folderTag) return &gm;
        auto pit = m.parties.find(partyId);
        if (pit == m.parties.end() || pit->second.players.empty())
        {
            auto git = m.guilds.find(partyId);
            return git != m.guilds.end() && !git->second.tag.empty() ? &git->second : nullptr;
        }
        std::map<int, int> counts;
        for (const auto& p : pit->second.players)
            if (p.guild_id > 0) counts[p.guild_id]++;
        int best = 0, bestCount = 0;
        for (const auto& [gid, n] : counts)
            if (n > bestCount) { best = gid; bestCount = n; }
        if (!bestCount) return nullptr;
        auto git = m.guilds.find(std::to_string(best));
        return git != m.guilds.end() ? &git->second : nullptr;
    };

    for (int i = 0; i < 2; i++)
    {
        const std::string partyId = i == 0 ? "1" : "2";
        MatchLink::Team team;
        if (const GuildMeta* g = teamGuild(partyId, tag[i]))
        {
            team.present = true;
            team.name = g->name;
            team.rank = g->rank;
            team.rating = g->rating;
            team.capeId = CapeId(g->cape);
        }
        auto pit = m.parties.find(partyId);
        if (pit != m.parties.end())
            for (const auto& p : pit->second.players)
            {
                team.present = true;
                team.profs.push_back("0123456789abcdef"[(p.primary >= 1 && p.primary <= 15) ? p.primary : 0]);
            }
        link.teams[i] = team;
    }
    link.occasion = m.occasion;
}

std::string FormatClock(float seconds)
{
    const int s = std::max(0, static_cast<int>(seconds));
    char buf[32];
    if (s >= 3600)
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

std::string DescribeFolder(const std::string& folder)
{
    // YYYY-MM-DD_<Map_Name>_<hh.mm>[_<[tag]vs[tag]>]
    if (folder.size() > 11 && folder[4] == '-' && folder[7] == '-' && folder[10] == '_')
    {
        const std::string date = folder.substr(0, 10);
        std::vector<std::string> tokens;
        size_t start = 11;
        while (start <= folder.size())
        {
            const size_t us = folder.find('_', start);
            tokens.push_back(folder.substr(start, us == std::string::npos ? std::string::npos : us - start));
            if (us == std::string::npos) break;
            start = us + 1;
        }
        auto isClock = [](const std::string& t)
        {
            return t.size() == 5 && std::isdigit(static_cast<unsigned char>(t[0])) &&
                   std::isdigit(static_cast<unsigned char>(t[1])) && t[2] == '.' &&
                   std::isdigit(static_cast<unsigned char>(t[3])) && std::isdigit(static_cast<unsigned char>(t[4]));
        };
        size_t clock = tokens.size();
        for (size_t i = 0; i < tokens.size(); i++)
            if (isClock(tokens[i])) clock = i;
        std::string map, tags;
        for (size_t i = 0; i < std::min(clock, tokens.size()); i++)
            map += (map.empty() ? "" : " ") + tokens[i];
        for (size_t i = clock + 1; i < tokens.size(); i++)
            tags += (tags.empty() ? "" : " ") + tokens[i];
        const size_t vs = tags.find("]vs[");
        if (vs != std::string::npos) tags.replace(vs, 4, "] vs [");
        if (!map.empty())
            return map + ", " + date + (tags.empty() ? "" : " - " + tags);
    }
    return folder;
}

// ---------------------------------------------------------------------------
// Self-test (GWO_LINK_SELFTEST)
// ---------------------------------------------------------------------------

int RunSelfTest()
{
    int failures = 0, cases = 0;
    auto check = [&](const char* name, bool ok)
    {
        cases++;
        if (!ok) failures++;
        RunLog::Line("link-selftest: %-34s %s", name, ok ? "ok" : "FAILED");
    };
    auto approx = [](float a, float b, float eps) { return std::fabs(a - b) <= eps; };
    const std::string folder = "2026-11-15_Burning_Isle_21.04";

    // 1. Follow camera round trip.
    {
        MatchLink l;
        l.folder = folder; l.t = 754.f; l.agent = 12; l.cam = MatchLink::Cam::Follow;
        l.yaw = 1.2344f; l.pitch = -0.9401f; l.dist = 1450.4f; l.paused = true;
        const std::string s = FormatHttps(l);
        RunLog::Line("link-selftest: follow link %s", s.c_str());
        const auto p = Parse(s);
        check("follow round trip", p && p->folder == folder && approx(p->t, 754.f, 0.01f) && p->agent == 12 &&
                                       p->cam == MatchLink::Cam::Follow && approx(p->yaw, 1.234f, 0.0006f) &&
                                       approx(p->pitch, -0.940f, 0.0006f) && approx(p->dist, 1450.f, 0.5f) && p->paused);
        check("follow link text",
              s == "https://gwobserver.com/watch?v=1&m=2026-11-15_Burning_Isle_21.04&t=754&p=12&cam=f,1.234,-0.940,1450&pause=1");
    }
    // 2. Free camera, playing, fractional time.
    {
        MatchLink l;
        l.folder = folder; l.t = 61.26f; l.cam = MatchLink::Cam::Free;
        l.pos = DirectX::XMFLOAT3(-1234.4f, 800.6f, 5120.f); l.yaw = -3.1f; l.pitch = 0.5f; l.paused = false;
        const std::string s = FormatHttps(l);
        RunLog::Line("link-selftest: free link %s", s.c_str());
        const auto p = Parse(s);
        check("free round trip", p && p->cam == MatchLink::Cam::Free && approx(p->pos.x, -1234.f, 0.5f) &&
                                     approx(p->pos.y, 801.f, 0.5f) && approx(p->pos.z, 5120.f, 0.5f) &&
                                     approx(p->yaw, -3.1f, 0.0006f) && approx(p->pitch, 0.5f, 0.0006f) &&
                                     approx(p->t, 61.3f, 0.01f) && !p->paused && p->agent == -1);
    }
    // 3. No camera, defaults.
    {
        const auto p = Parse("https://gwobserver.com/watch?v=1&m=" + folder);
        check("no camera defaults", p && p->cam == MatchLink::Cam::None && p->t == 0.f && p->agent == -1 && p->paused);
    }
    // 4. Clock forms.
    {
        const auto a = Parse("https://gwobserver.com/watch?v=1&m=" + folder + "&t=12:34");
        const auto b = Parse("https://gwobserver.com/watch?v=1&m=" + folder + "&t=12m34s");
        const auto c = Parse("https://gwobserver.com/watch?v=1&m=" + folder + "&t=1:02:03");
        const auto d = Parse("https://gwobserver.com/watch?v=1&m=" + folder + "&t=45s");
        check("t=12:34", a && approx(a->t, 754.f, 0.01f));
        check("t=12m34s", b && approx(b->t, 754.f, 0.01f));
        check("t=1:02:03", c && approx(c->t, 3723.f, 0.01f));
        check("t=45s", d && approx(d->t, 45.f, 0.01f));
    }
    // 5. Bad values dropped one by one.
    {
        const auto a = Parse("https://gwobserver.com/watch?v=1&m=" + folder + "&t=300&p=4&cam=f,abc,0.1,900");
        check("bad cam keeps time", a && a->cam == MatchLink::Cam::None && approx(a->t, 300.f, 0.01f) && a->agent == 4);
        const auto b = Parse("https://gwobserver.com/watch?v=1&m=" + folder + "&t=soon&p=-3&pause=maybe");
        check("bad t/p/pause dropped", b && b->t == 0.f && b->agent == -1 && b->paused);
        const auto c = Parse("https://gwobserver.com/watch?v=1&m=" + folder + "&cam=f,1,0.2,900");
        check("follow cam needs p", c && c->cam == MatchLink::Cam::None);
        const auto d = Parse("https://gwobserver.com/watch?v=1&m=" + folder + "&cam=c,1,2,3,4");
        check("short free cam dropped", d && d->cam == MatchLink::Cam::None);
    }
    // 6. Newer version refused.
    {
        ParseError e = ParseError::None;
        const auto p = Parse("https://gwobserver.com/watch?v=2&m=" + folder, &e);
        check("newer v refused", !p && e == ParseError::NewerVersion);
    }
    // 7. Accepted forms.
    {
        const std::string q = "v=1&m=" + folder + "&t=10";
        check("gwobserver:// form", Parse("gwobserver://watch?" + q).has_value());
        check("gwobserver://watch/ form", Parse("gwobserver://watch/?" + q).has_value());
        check("www. form", Parse("https://www.gwobserver.com/watch?" + q).has_value());
        check("watch.html form", Parse("https://gwobserver.com/watch.html?" + q).has_value());
        check("quoted, blanks", Parse("  \"https://gwobserver.com/watch?" + q + "\"\r\n").has_value());
        check("unknown field ignored", Parse("https://gwobserver.com/watch?" + q + "&zz=1&ref=discord").has_value());
    }
    // 8. Percent-encoding of a folder with spaces.
    {
        MatchLink l;
        l.folder = "2026-01-02_Isle of the Dead_10.00";
        const std::string s = FormatHttps(l);
        const auto p = Parse(s);
        check("spaces encoded", s.find(' ') == std::string::npos && p && p->folder == l.folder);
        MatchLink r;
        r.folder = "2026-03-21_Warrior's_Isle_07.26_[xxx]vs[LaG]";
        const std::string rs = FormatHttps(r);
        RunLog::Line("link-selftest: real folder link %s", rs.c_str());
        const auto rp = Parse(rs);
        check("apostrophe + brackets", rp && rp->folder == r.folder && rs.find('\'') == std::string::npos &&
                                           rs.find('[') == std::string::npos);
    }
    // 9. Junk.
    {
        ParseError e = ParseError::None;
        check("junk text", !Parse("hello world", &e) && e == ParseError::NotALink);
        check("other site", !Parse("https://example.com/watch?v=1&m=" + folder));
        check("no folder", !Parse("https://gwobserver.com/watch?v=1&t=3", &e) && e == ParseError::NoMatch);
        check("no version", !Parse("https://gwobserver.com/watch?m=" + folder, &e) && e == ParseError::NotALink);
        check("path in folder refused", !Parse("https://gwobserver.com/watch?v=1&m=..%5C..%5Cx", &e));
        check("empty", !Parse(""));
        check("LooksLikeLink", LooksLikeLink("gwobserver://watch?v=1") && !LooksLikeLink("Burning Isle"));
    }
    // 10. Display-only fields (the website's card): written, read back, never needed to open.
    {
        MatchLink l;
        l.folder = folder; l.t = 30.f;
        l.teams[0].present = true; l.teams[0].name = "Stoned Summit"; l.teams[0].rank = 12; l.teams[0].rating = 1203;
        l.teams[0].capeId = "5-10-43-2-75-64-96"; l.teams[0].profs = "1236a8";
        l.teams[1].present = true;
        l.teams[1].name = "Ordre de l'\xC3\x89toile ~ & = %";   // unicode, '~' and query characters
        l.teams[1].rank = 0; l.teams[1].rating = 0; l.teams[1].capeId = ""; l.teams[1].profs = "0000";
        l.occasion = "A AT";
        const std::string s = FormatHttps(l);
        RunLog::Line("link-selftest: card link %s", s.c_str());
        const auto p = Parse(s);
        check("card round trip", p && p->teams[0].present && p->teams[0].name == "Stoned Summit" &&
                                     p->teams[0].rank == 12 && p->teams[0].rating == 1203 &&
                                     p->teams[0].capeId == "5-10-43-2-75-64-96" && p->teams[0].profs == "1236a8" &&
                                     p->occasion == "A AT" && approx(p->t, 30.f, 0.01f));
        check("card unicode name, no cape", p && p->teams[1].present && p->teams[1].name == l.teams[1].name &&
                                                p->teams[1].capeId.empty() && p->teams[1].profs == "0000");
        const auto bad = Parse("https://gwobserver.com/watch?v=1&m=" + folder +
                               "&t=40&g1=Only~two&g2=X~a~b~1-2-3~ZZ&o=" + std::string(80, 'x'));
        check("malformed card fields tolerated", bad && approx(bad->t, 40.f, 0.01f) && !bad->teams[0].present &&
                                                     bad->teams[1].present && bad->teams[1].name == "X" &&
                                                     bad->teams[1].rank == 0 && bad->teams[1].capeId.empty() &&
                                                     bad->teams[1].profs.empty() && bad->occasion.empty());
        const auto none = Parse("https://gwobserver.com/watch?v=1&m=" + folder);
        check("card fields absent", none && !none->teams[0].present && !none->teams[1].present && none->occasion.empty());
        CapeData c; c.shape = 5; c.detail = 10; c.emblem = 43; c.trim = 2; c.bg_color = 75; c.detail_color = 64; c.emblem_color = 96;
        check("CapeId order", CapeId(c) == "5-10-43-2-75-64-96");

        MatchMeta mm;
        mm.folder_name = "2026-03-21_Warrior's_Isle_07.26_[xxx]vs[LaG]";
        mm.occasion = "A AT";
        GuildMeta ga; ga.name = "Guild A"; ga.tag = "LaG"; ga.rank = 3; ga.rating = 1500; ga.cape = c;
        GuildMeta gb; gb.name = "Guild B"; gb.tag = "xxx";
        mm.guilds["7"] = ga; mm.guilds["9"] = gb;
        PlayerMeta pa; pa.primary = 10; PlayerMeta pb; pb.primary = 3;
        mm.parties["1"].players = { pa, pb };
        MatchLink f;
        FillDisplay(f, mm);
        check("FillDisplay by folder tag", f.teams[0].name == "Guild B" && f.teams[1].name == "Guild A" &&
                                               f.teams[1].capeId == "5-10-43-2-75-64-96" && f.teams[1].rank == 3 &&
                                               f.teams[0].profs == "a3" && f.occasion == "A AT");
    }
    // 11. Labels.
    {
        check("DescribeFolder", DescribeFolder(folder) == "Burning Isle, 2026-11-15" &&
                                    DescribeFolder("odd name") == "odd name" &&
                                    DescribeFolder("2026-03-21_Warrior's_Isle_07.26_[xxx]vs[LaG]") ==
                                        "Warrior's Isle, 2026-03-21 - [xxx] vs [LaG]");
        check("FormatClock", FormatClock(754.f) == "12:34" && FormatClock(3723.f) == "1:02:03");
    }

    RunLog::Line("link-selftest: %d case(s), %d failure(s) -> %s", cases, failures, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
}
