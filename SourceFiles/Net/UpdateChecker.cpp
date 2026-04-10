#include "pch.h"
#include "Net/UpdateChecker.h"
#include "Net/HttpClient.h"
#include "json.hpp"

using json = nlohmann::json;

UpdateChecker::~UpdateChecker()
{
    if (m_thread.joinable())
        m_thread.join();
}

void UpdateChecker::Check(const std::string& currentVersion,
                           const std::string& repo)
{
    if (m_thread.joinable())
        m_thread.join();

    m_currentVersion = currentVersion;
    m_repo = repo;
    m_complete.store(false);
    m_hasUpdate.store(false);

    m_thread = std::thread(&UpdateChecker::CheckThread, this);
}

std::string UpdateChecker::GetLatestVersion() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_latestVersion;
}

std::string UpdateChecker::GetReleaseUrl() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_releaseUrl;
}

std::string UpdateChecker::GetCurrentVersion() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentVersion;
}

bool UpdateChecker::IsNewer(const std::string& latest, const std::string& current)
{
    // Parse "major.minor.patch" from both strings (strip leading 'v' if present)
    auto parse = [](const std::string& s, int& maj, int& min, int& pat)
    {
        const char* p = s.c_str();
        if (*p == 'v' || *p == 'V') ++p;
        maj = min = pat = 0;
        sscanf_s(p, "%d.%d.%d", &maj, &min, &pat);
    };

    int lMaj, lMin, lPat;
    int cMaj, cMin, cPat;
    parse(latest, lMaj, lMin, lPat);
    parse(current, cMaj, cMin, cPat);

    if (lMaj != cMaj) return lMaj > cMaj;
    if (lMin != cMin) return lMin > cMin;
    return lPat > cPat;
}

void UpdateChecker::CheckThread()
{
    HttpClient http;
    http.SetBaseUrl(L"api.github.com", true);

    // GET /repos/{owner}/{repo}/releases/latest
    std::string pathStr = "/repos/" + m_repo + "/releases/latest";
    std::wstring path(pathStr.begin(), pathStr.end());

    auto resp = http.Get(path);

    if (!resp.IsOk())
    {
        m_complete.store(true);
        return;
    }

    std::string body(resp.body.begin(), resp.body.end());

    try
    {
        auto j = json::parse(body);

        std::string tagName = j.value("tag_name", "");
        std::string htmlUrl = j.value("html_url", "");

        if (!tagName.empty())
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_latestVersion = tagName;
            m_releaseUrl = htmlUrl;

            if (IsNewer(tagName, m_currentVersion))
                m_hasUpdate.store(true);
        }
    }
    catch (...)
    {
        // JSON parse failure — silently ignore
    }

    m_complete.store(true);
}
