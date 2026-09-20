#include "pch.h"

#include "RunLog.h"

#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
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

// The breadcrumb. No lock: see Set().
std::atomic<const char*> g_crumb_where{nullptr};
std::atomic<int> g_crumb_a{-1};
std::atomic<int> g_crumb_b{-1};
std::atomic<int> g_crumb_c{-1};
std::atomic<unsigned long long> g_crumb_count{0};

// WHICH BUILD WROTE THIS, in the terms a debugger matches on.
//
// A crash dump is only readable with the symbol file of THE BINARY THAT CRASHED. The build id below
// is what a debugger compares: the image's own link stamp and size, and the identity of the symbol
// file the linker recorded inside it (a guid and an age, from the image's debug directory). With
// these three in the log, whoever holds a dump can say in one look whether the symbol file they
// have is the right one - instead of loading a near-miss and reading someone else's function names.
std::string image_identity()
{
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    if (base == nullptr)
        return {};

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return {};

    char text[512] = {};
    std::snprintf(text, sizeof(text), "stamp 0x%08X size 0x%08X",
                  static_cast<unsigned>(nt->FileHeader.TimeDateStamp),
                  static_cast<unsigned>(nt->OptionalHeader.SizeOfImage));
    std::string out = text;

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dir.VirtualAddress == 0 || dir.Size < sizeof(IMAGE_DEBUG_DIRECTORY))
        return out;

    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
    const size_t count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (size_t i = 0; i < count; i++)
    {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || entries[i].AddressOfRawData == 0 ||
            entries[i].SizeOfData < 24)
            continue;

        // The CodeView "RSDS" record: signature, guid, age, then the symbol file's path.
        const unsigned char* rec = base + entries[i].AddressOfRawData;
        if (std::memcmp(rec, "RSDS", 4) != 0)
            continue;

        GUID guid{};
        std::memcpy(&guid, rec + 4, sizeof(GUID));
        unsigned age = 0;
        std::memcpy(&age, rec + 20, sizeof(unsigned));

        char id[160] = {};
        std::snprintf(id, sizeof(id),
                      " symbols %08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X age %u",
                      static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3,
                      guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4],
                      guid.Data4[5], guid.Data4[6], guid.Data4[7], age);
        out += id;
        break;
    }

    return out;
}

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

        // THE PREVIOUS RUN IS KEPT. This file is one launch, which is what makes it readable - but
        // the launch that matters after a crash is the one BEFORE the launch someone makes to
        // report it, and truncating on the first write is exactly what destroys it. So the old file
        // moves aside first. One generation is enough: the run that crashed, and the run after it.
        {
            std::error_code ec;
            if (std::filesystem::exists(path, ec))
            {
                std::filesystem::path previous = path;
                previous.replace_extension();
                previous += ".prev.log";
                std::filesystem::remove(previous, ec);
                std::filesystem::rename(path, previous, ec);
            }
        }

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
            head << "image     " << image_identity() << "\n";

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

void Set(const char* where, int a, int b, int c)
{
    // Deliberately lock-free and allocation-free: this is called from the draw pass, and a step
    // that has to take a lock to record where it is would not be recorded at all. Relaxed stores
    // are enough - only a crash handler reads this, after everything else has stopped, and a torn
    // read of four independent scalars still names the step.
    g_crumb_where.store(where, std::memory_order_relaxed);
    g_crumb_a.store(a, std::memory_order_relaxed);
    g_crumb_b.store(b, std::memory_order_relaxed);
    g_crumb_c.store(c, std::memory_order_relaxed);
    g_crumb_count.fetch_add(1, std::memory_order_relaxed);
}

std::string Read()
{
    const char* where = g_crumb_where.load(std::memory_order_relaxed);
    if (where == nullptr)
        return {};

    char text[512] = {};
    std::snprintf(text, sizeof(text), "%s (%d, %d, %d) after %llu step(s)", where,
                  g_crumb_a.load(std::memory_order_relaxed),
                  g_crumb_b.load(std::memory_order_relaxed),
                  g_crumb_c.load(std::memory_order_relaxed),
                  static_cast<unsigned long long>(g_crumb_count.load(std::memory_order_relaxed)));
    return text;
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
