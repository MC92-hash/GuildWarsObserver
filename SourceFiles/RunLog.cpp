#include "pch.h"

#include "RunLog.h"

#include <shlobj.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace RunLog
{
namespace
{
using Clock = std::chrono::steady_clock;

std::mutex g_mutex;
bool g_started = false;
Clock::time_point g_start;

std::filesystem::path resolve_path()
{
    wchar_t* appData = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData)
        dir = std::filesystem::path(appData) / L"GWObserver";
    if (appData)
        CoTaskMemFree(appData);
    if (dir.empty())
        return {};

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / L"PlayerVisuals.log";
}

// Truncates on the first write of the run and stamps a header, so a log handed back is one launch
// and says which build wrote it.
std::ofstream open_stream()
{
    const std::filesystem::path& path = Path();
    if (path.empty())
        return {};

    if (!g_started)
    {
        g_started = true;
        g_start = Clock::now();

        std::ofstream head(path, std::ios::trunc);
        if (head)
        {
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);

            const auto now = std::chrono::system_clock::now();
            const std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
            localtime_s(&tm, &t);
            char stamp[32] = {};
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

            head << "GW Observer run log\n";
            head << "started   " << stamp << "\n";
            head << "executable " << std::filesystem::path(exe).string() << "\n";
#if defined(_DEBUG)
            head << "build     Debug\n";
#else
            head << "build     Release\n";
#endif
            std::error_code ec;
            head << "directory " << std::filesystem::current_path(ec).string() << "\n";

            // EVERYTHING BEFORE THE FIRST LOGGED LINE, in one number: the image load, the C
            // runtime and every global constructor in every translation unit. A Debug build of a
            // large binary spends real time here and no code of ours runs during it, so this is
            // the number that separates "the application is slow to start" from "something the
            // application does at start-up is slow".
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            {
                FILETIME nowFt{};
                GetSystemTimeAsFileTime(&nowFt);
                const ULONGLONG a = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) |
                    created.dwLowDateTime;
                const ULONGLONG b = (static_cast<ULONGLONG>(nowFt.dwHighDateTime) << 32) |
                    nowFt.dwLowDateTime;
                if (b > a)
                {
                    head << "before this line " << ((b - a) / 10000ULL)
                         << " ms (process creation to the first logged event: image load, C"
                            " runtime, global constructors)\n";
                }
            }
            head << "-----------------------------------------------------------------------\n";
        }
    }

    return std::ofstream(path, std::ios::app);
}

void write_locked(const char* text)
{
    std::ofstream stream = open_stream();
    if (!stream)
        return;

    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - g_start).count();
    char prefix[32] = {};
    std::snprintf(prefix, sizeof(prefix), "[%9.1f ms] ", ms);
    stream << prefix << text << "\n";
}
} // namespace

const std::filesystem::path& Path()
{
    static const std::filesystem::path path = resolve_path();
    return path;
}

double ElapsedMs()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_started)
        return 0.0;
    return std::chrono::duration<double, std::milli>(Clock::now() - g_start).count();
}

void Line(const char* format, ...)
{
    if (format == nullptr)
        return;

    char buffer[2048];
    va_list args;
    va_start(args, format);
    const int written = _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);
    if (written < 0)
        buffer[sizeof(buffer) - 1] = '\0';

    std::lock_guard<std::mutex> lock(g_mutex);
    write_locked(buffer);
}

void Block(const std::string& text)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    size_t begin = 0;
    while (begin <= text.size())
    {
        const size_t end = text.find('\n', begin);
        const std::string line = text.substr(begin, end == std::string::npos ? std::string::npos
                                                                             : end - begin);
        if (!line.empty())
            write_locked(line.c_str());
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
}
} // namespace RunLog
