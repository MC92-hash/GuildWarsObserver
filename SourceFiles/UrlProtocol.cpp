#include "pch.h"
#include "UrlProtocol.h"
#include "GuiGlobalConstants.h"
#include "RunLog.h"
#include "DirectXTex/DirectXTex.h"
#include "MatchLink.h"
#include "GuildBannerComposer.h"
#include "GuildBannerAssetProvider.h"
#include "ReplayLibrary.h"
#pragma comment(lib, "msimg32.lib")   // AlphaBlend (the splash's capes)
#include <shellapi.h>
#include <vector>

namespace
{
    std::optional<MatchLink> s_pending;
    HWND s_mainWindow = nullptr;
    int  s_libraryShowCmd = SW_SHOWNORMAL;
    bool s_libraryHidden = false;

    std::string s_toast;
    bool        s_toastIsError = false;
    ULONGLONG   s_toastUntil = 0;

    std::function<bool(const std::string&)> s_publishedLookup;

    std::string Utf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }

    std::wstring Wide(const std::string& s)
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
        return w;
    }

    std::wstring ExePath()
    {
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
        return std::wstring(buf, n);
    }

    bool LooksLikeLinkArg(const wchar_t* a)
    {
        return a && MatchLinks::LooksLikeLink(Utf8(a));
    }
}

namespace UrlProtocol
{
// ---------------------------------------------------------------------------
// Command line and the running instance
// ---------------------------------------------------------------------------

const wchar_t* MainWindowClass()
{
    static const std::wstring name = []
    {
        std::wstring n = L"GuildWarsObserverWindowClass";
        wchar_t tag[64] = {};
        if (GuiGlobalConstants::IsDeveloperMode() && GetEnvironmentVariableW(L"GWO_INSTANCE_TAG", tag, 64) > 0)
            n += L"_" + std::wstring(tag);
        return n;
    }();
    return name.c_str();
}

bool QuietTestInstance()
{
    static const bool quiet = []
    {
        wchar_t tag[64] = {};
        return GuiGlobalConstants::IsDeveloperMode() && GetEnvironmentVariableW(L"GWO_INSTANCE_TAG", tag, 64) > 0;
    }();
    return quiet;
}

std::wstring FindLinkArgument(const wchar_t* cmdLine)
{
    if (!cmdLine || !*cmdLine) return {};
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    if (!argv) return {};
    std::wstring found;
    for (int i = 0; i < argc && found.empty(); i++)
    {
        if (_wcsicmp(argv[i], L"--open-link") == 0 && i + 1 < argc)
            found = argv[i + 1];
        else if (_wcsnicmp(argv[i], L"gwobserver:", 11) == 0 || LooksLikeLinkArg(argv[i]))
            found = argv[i];
    }
    LocalFree(argv);
    return found;
}

bool ForwardToRunningInstance(const std::wstring& link)
{
    // NOTE: no RunLog here. The log is truncated by the first line a process writes, and this
    // process exits at once: writing would wipe the log of the instance it forwards to.
    HWND target = FindWindowW(MainWindowClass(), nullptr);
    if (!target) return false;

    // This process has the foreground right the user's click gave it: hand it over, so the
    // running instance can bring the right window up (its replay, or its Library).
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    if (pid) AllowSetForegroundWindow(pid);

    const std::string utf8 = Utf8(link);
    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataTag;
    cds.cbData = static_cast<DWORD>(utf8.size());
    cds.lpData = const_cast<char*>(utf8.data());
    DWORD_PTR result = 0;
    const LRESULT sent = SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result);
    return sent != 0 && result == TRUE;
}

bool HandleCopyData(const COPYDATASTRUCT* cds)
{
    if (!cds || cds->dwData != kCopyDataTag) return false;
    if (!cds->lpData || cds->cbData == 0 || cds->cbData > 8192)
    {
        RunLog::Line("link: a forwarded link was empty or too long (%lu bytes), ignored",
                     cds ? cds->cbData : 0ul);
        return true;
    }
    const std::string text(static_cast<const char*>(cds->lpData), cds->cbData);
    RunLog::Line("link: forwarded by a second GW Observer process");
    SubmitLinkText(text, "forwarded");
    return true;
}

bool SubmitLinkText(const std::string& text, const char* source)
{
    MatchLinks::ParseError err = MatchLinks::ParseError::None;
    const auto link = MatchLinks::Parse(text, &err);
    if (!link)
    {
        const char* why = err == MatchLinks::ParseError::NewerVersion ? "made by a newer GW Observer"
                        : err == MatchLinks::ParseError::NoMatch      ? "names no match"
                                                                       : "not a share link";
        RunLog::Line("link: received (%s) but FAILED to read it - %s (%zu chars)", source, why, text.size());
        ShowLibrary("the link could not be read");
        Toast(err == MatchLinks::ParseError::NewerVersion ? "Update GW Observer to open this link"
                                                          : "This is not a GW Observer match link",
              true, 10.0);
        return false;
    }
    RunLog::Line("link: received (%s) - match '%s' at %.1f s, agent %d, camera %s, %s", source,
                 link->folder.c_str(), link->t, link->agent,
                 link->cam == MatchLink::Cam::Follow ? "follow" : link->cam == MatchLink::Cam::Free ? "free" : "default",
                 link->paused ? "paused" : "playing");
    SetPendingLink(*link);
    return true;
}

// ---------------------------------------------------------------------------
// Registration (HKCU, no admin)
// ---------------------------------------------------------------------------

void RegisterIfNeeded()
{
    if (GuiGlobalConstants::IsDeveloperMode())
    {
        char v[8] = {};
        if (GetEnvironmentVariableA("GWO_REGISTER_LINKS", v, (DWORD)sizeof(v)) == 0 || strcmp(v, "1") != 0)
        {
            RunLog::Line("link: developer build - the gwobserver:// link type is NOT registered"
                         " (set GWO_REGISTER_LINKS=1 to register this exe)");
            return;
        }
    }

    const std::wstring exe = ExePath();
    if (exe.empty()) return;
    const std::wstring command = L"\"" + exe + L"\" --open-link \"%1\"";
    const wchar_t* kRoot = L"Software\\Classes\\gwobserver";
    const wchar_t* kCommand = L"Software\\Classes\\gwobserver\\shell\\open\\command";

    wchar_t current[MAX_PATH * 3] = {};
    DWORD size = sizeof(current);
    if (RegGetValueW(HKEY_CURRENT_USER, kCommand, nullptr, RRF_RT_REG_SZ, nullptr, current, &size) == ERROR_SUCCESS &&
        command == current)
    {
        RunLog::Line("link: the gwobserver:// link type already points at this exe");
        return;
    }

    auto set = [](const wchar_t* key, const wchar_t* name, const std::wstring& value)
    {
        return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, value.c_str(),
                               static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    };
    const bool ok = set(kRoot, nullptr, L"URL:GW Observer") && set(kRoot, L"URL Protocol", L"") &&
                    set(L"Software\\Classes\\gwobserver\\DefaultIcon", nullptr, L"\"" + exe + L"\",0") &&
                    set(kCommand, nullptr, command);
    RunLog::Line("link: %s the gwobserver:// link type -> '%s'", ok ? "registered" : "FAILED to register",
                 Utf8(exe).c_str());
}

// ---------------------------------------------------------------------------
// The slot
// ---------------------------------------------------------------------------

void SetPendingLink(const MatchLink& link) { s_pending = link; }
const MatchLink* PeekPendingLink() { return s_pending ? &*s_pending : nullptr; }
void ClearPendingLink() { s_pending.reset(); }

// ---------------------------------------------------------------------------
// Link-only start
// ---------------------------------------------------------------------------

void SetMainWindow(HWND hwnd, int libraryShowCmd)
{
    s_mainWindow = hwnd;
    s_libraryShowCmd = (libraryShowCmd == SW_HIDE) ? SW_SHOWNORMAL : libraryShowCmd;
}

void SetLibraryHidden(bool hidden) { s_libraryHidden = hidden; }
bool LibraryHidden() { return s_libraryHidden; }

void ShowLibrary(const char* why)
{
    if (!s_libraryHidden) return;
    s_libraryHidden = false;
    HideSplash();
    RunLog::Line("link: showing the Library - %s", why ? why : "asked");
    if (s_mainWindow)
    {
        if (QuietTestInstance())
            ShowWindow(s_mainWindow, SW_SHOWMINNOACTIVE);
        else
        {
            ShowWindow(s_mainWindow, s_libraryShowCmd);
            SetForegroundWindow(s_mainWindow);
        }
    }
}

// ---------------------------------------------------------------------------
// Library toast
// ---------------------------------------------------------------------------

void Toast(const std::string& text, bool isError, double seconds)
{
    s_toast = text;
    s_toastIsError = isError;
    s_toastUntil = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0);
}

void DrawLibraryToast()
{
    if (s_toast.empty()) return;
    if (GetTickCount64() > s_toastUntil) { s_toast.clear(); return; }

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y - 48.f), ImGuiCond_Always, ImVec2(0.5f, 1.f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##LinkToast", nullptr, kFlags))
    {
        if (s_toastIsError)
            ImGui::TextColored(ImVec4(0.95f, 0.50f, 0.42f, 1.f), "%s", s_toast.c_str());
        else
            ImGui::TextUnformatted(s_toast.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ---------------------------------------------------------------------------
// Published matches, clipboard
// ---------------------------------------------------------------------------

void SetPublishedLookup(std::function<bool(const std::string&)> lookup) { s_publishedLookup = std::move(lookup); }

bool IsPublished(const std::string& folder)
{
    return !folder.empty() && s_publishedLookup && s_publishedLookup(folder);
}

std::string ToUtf8(const std::wstring& text) { return Utf8(text); }

bool CopyToClipboard(const std::string& text, HWND owner)
{
    const std::wstring w = Wide(text);
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t)))
    {
        if (void* p = GlobalLock(mem))
        {
            memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

// ---------------------------------------------------------------------------
// Splash: a plain GDI popup, so it needs no device and shows before anything else is ready.
// ---------------------------------------------------------------------------

namespace
{
    // A small copy of the match loading screen: the map behind, date / occasion / map, both
    // guilds with their capes, the shared moment, then the status and a progress bar.
    constexpr int kSplashW = 640, kSplashH = 360;
    constexpr int kCapeW = 56, kCapeH = 112;

    struct SplashTeam
    {
        std::wstring name;      // guild name, or "[TAG]" when the link carries no name
        std::wstring tag;       // "[TAG]" (empty when the name line already shows it)
        std::wstring stats;     // "Rank #12  ·  Rating 1420"
        HBITMAP      cape = nullptr;   // premultiplied BGRA, kCapeW x kCapeH
    };

    struct Splash
    {
        HWND         hwnd = nullptr;
        std::wstring title, status;    // title: the caller's fallback headline
        float        fraction = -1.f;
        int          mapId = 0;
        int          pictureMapId = -1;    // the map the picture below was built for
        HBITMAP      picture = nullptr;    // kSplashW x kSplashH, cropped and darkened
        // From the link (SetSplashLink)
        bool         haveLink = false;
        std::string  linkFolder;
        float        linkT = -1.f;
        int          linkMapId = 0;
        std::wstring headline;   // "2026/07/08 · A AT · Nomad's Isle"
        std::wstring kicker;     // "Someone shared a moment from this match at 2:00"
        SplashTeam   teams[2];
    };
    Splash s_splash;

    // Recordings are named in the recording client's language. English and German names -> map id.
    struct MapAlias { const char* name; int id; };
    constexpr MapAlias kMapAliases[] = {
        {"Warrior's Isle", 171}, {"Insel des Kriegers", 171}, {"Hunter's Isle", 172}, {"Wizard's Isle", 173},
        {"Burning Isle", 167},   {"Brennende Insel", 167},    {"Frozen Isle", 170},   {"Vereiste Insel", 170},
        {"Nomad's Isle", 174},   {"Nomadeninsel", 174},       {"Druid's Isle", 168},  {"Druideninsel", 168},
        {"Isle of the Dead", 175}, {"Insel der Toten", 175},
        {"Isle of Meditation", 358}, {"Insel der Meditation", 358},
        {"Isle of Weeping Stone", 355}, {"Isle of the Weeping Stone", 355}, {"Insel des trauernden Steins", 355},
        {"Isle of Jade", 356},   {"Jadeinsel", 356},          {"Imperial Isle", 357}, {"Kaiserinsel", 357},
        {"Uncharted Isle", 533}, {"Unbekannte Insel", 533},   {"Isle of Wurms", 534}, {"Insel der W__rmer", 534},
        {"Corrupted Isle", 541}, {"Verdorbene Insel", 541},   {"Isle of Solitude", 542}, {"Insel der Einsamkeit", 542},
    };

    const wchar_t* EnglishMapName(int id)
    {
        switch (id)
        {
        case 171: return L"Warrior's Isle";      case 172: return L"Hunter's Isle";
        case 173: return L"Wizard's Isle";       case 167: return L"Burning Isle";
        case 170: return L"Frozen Isle";         case 174: return L"Nomad's Isle";
        case 168: return L"Druid's Isle";        case 175: return L"Isle of the Dead";
        case 358: return L"Isle of Meditation";  case 355: return L"Isle of Weeping Stone";
        case 356: return L"Isle of Jade";        case 357: return L"Imperial Isle";
        case 533: return L"Uncharted Isle";      case 534: return L"Isle of Wurms";
        case 541: return L"Corrupted Isle";      case 542: return L"Isle of Solitude";
        default:  return nullptr;
        }
    }

    // "YYYY-MM-DD_<Map_Name>_<mm.ss>[_[tag1]vs[tag2]]" -> its parts. False when it has another shape.
    bool SplitFolder(const std::string& folder, std::string& date, std::string& map, std::string tags[2])
    {
        if (folder.size() < 12 || folder[4] != '-' || folder[7] != '-' || folder[10] != '_') return false;
        date = folder.substr(0, 4) + "/" + folder.substr(5, 2) + "/" + folder.substr(8, 2);
        std::vector<std::string> tokens;
        for (size_t start = 11;;)
        {
            const size_t us = folder.find('_', start);
            tokens.push_back(folder.substr(start, us == std::string::npos ? std::string::npos : us - start));
            if (us == std::string::npos) break;
            start = us + 1;
        }
        auto isClock = [](const std::string& t)
        {
            return t.size() == 5 && isdigit((unsigned char)t[0]) && isdigit((unsigned char)t[1]) && t[2] == '.' &&
                   isdigit((unsigned char)t[3]) && isdigit((unsigned char)t[4]);
        };
        size_t clock = tokens.size();
        for (size_t i = 0; i < tokens.size(); i++)
            if (isClock(tokens[i])) { clock = i; break; }
        map.clear();
        for (size_t i = 0; i < clock && i < tokens.size(); i++)
            map += (map.empty() ? "" : " ") + tokens[i];
        const size_t vs = folder.find("]vs[");
        if (vs != std::string::npos)
        {
            const size_t open1 = folder.rfind('[', vs), close2 = folder.find(']', vs + 4);
            if (open1 != std::string::npos && close2 != std::string::npos)
            {
                tags[0] = folder.substr(open1 + 1, vs - open1 - 1);
                tags[1] = folder.substr(vs + 4, close2 - vs - 4);
            }
        }
        return !map.empty();
    }

    // A 32-bit top-down DIB section; *bits receives its pixels.
    HBITMAP MakeDib(int w, int h, uint8_t** bits)
    {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* p = nullptr;
        HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
        if (!bmp || !p) { if (bmp) DeleteObject(bmp); return nullptr; }
        *bits = static_cast<uint8_t*>(p);
        return bmp;
    }

    // The map picture, cropped to fill the whole splash and darkened so the text reads on it.
    void LoadSplashPicture(int mapId)
    {
        if (s_splash.pictureMapId == mapId) return;
        s_splash.pictureMapId = mapId;
        if (s_splash.picture) { DeleteObject(s_splash.picture); s_splash.picture = nullptr; }
        if (mapId <= 0) return;

        // The loading screen's painting of the map (Textures/Loading_Screen/<English name>.jpg|webp,
        // as ReplayWindow::GetMatchLoadingBgPath finds it), else the overview picture.
        std::filesystem::path path;
        std::error_code ec;
        if (const wchar_t* english = EnglishMapName(mapId))
        {
            std::filesystem::path dir = GuiGlobalConstants::GetExeDir();
            for (int i = 0; i < 6 && path.empty() && !dir.empty(); i++)
            {
                for (const wchar_t* ext : {L".jpg", L".webp"})
                {
                    const auto candidate = dir / L"Textures" / L"Loading_Screen" / (std::wstring(english) + ext);
                    if (std::filesystem::exists(candidate, ec)) { path = candidate; break; }
                }
                if (dir == dir.parent_path()) break;
                dir = dir.parent_path();
            }
        }
        if (path.empty())
            path = GuiGlobalConstants::GetExeDir() / L"Textures" / L"Map_Pictures" / (std::to_wstring(mapId) + L".jpg");
        if (!std::filesystem::exists(path, ec)) return;
        DirectX::ScratchImage img, bgra;
        if (FAILED(DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, img))) return;
        const DirectX::Image* src = img.GetImage(0, 0, 0);
        if (!src || src->width == 0 || src->height == 0) return;
        if (src->format != DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            if (FAILED(DirectX::Convert(*src, DXGI_FORMAT_B8G8R8A8_UNORM, DirectX::TEX_FILTER_DEFAULT,
                                        DirectX::TEX_THRESHOLD_DEFAULT, bgra)))
                return;
            src = bgra.GetImage(0, 0, 0);
        }
        // The full picture as a DIB, then scaled (cover crop) into the splash-sized DIB by GDI.
        uint8_t* fullBits = nullptr;
        HBITMAP full = MakeDib(static_cast<int>(src->width), static_cast<int>(src->height), &fullBits);
        if (!full) return;
        for (size_t y = 0; y < src->height; y++)
            memcpy(fullBits + y * src->width * 4, src->pixels + y * src->rowPitch, src->width * 4);
        uint8_t* bits = nullptr;
        HBITMAP bmp = MakeDib(kSplashW, kSplashH, &bits);
        if (!bmp) { DeleteObject(full); return; }
        {
            HDC sdc = CreateCompatibleDC(nullptr), ddc = CreateCompatibleDC(nullptr);
            HGDIOBJ os = SelectObject(sdc, full), od = SelectObject(ddc, bmp);
            const float s = std::max(static_cast<float>(kSplashW) / src->width, static_cast<float>(kSplashH) / src->height);
            const int cw = static_cast<int>(kSplashW / s), ch = static_cast<int>(kSplashH / s);
            SetStretchBltMode(ddc, HALFTONE);
            SetBrushOrgEx(ddc, 0, 0, nullptr);
            StretchBlt(ddc, 0, 0, kSplashW, kSplashH, sdc, (static_cast<int>(src->width) - cw) / 2,
                       (static_cast<int>(src->height) - ch) / 2, cw, ch, SRCCOPY);
            SelectObject(sdc, os); SelectObject(ddc, od);
            DeleteDC(sdc); DeleteDC(ddc);
        }
        DeleteObject(full);
        GdiFlush();
        for (int y = 0; y < kSplashH; y++)
        {
            // Darker towards the bottom, where the status and the bar sit.
            const float k = 0.42f - 0.24f * (static_cast<float>(y) / kSplashH);
            uint8_t* row = bits + static_cast<size_t>(y) * kSplashW * 4;
            for (int x = 0; x < kSplashW; x++)
            {
                row[x * 4 + 0] = static_cast<uint8_t>(row[x * 4 + 0] * k + 32 * (1.f - k) * 0.6f);
                row[x * 4 + 1] = static_cast<uint8_t>(row[x * 4 + 1] * k + 24 * (1.f - k) * 0.6f);
                row[x * 4 + 2] = static_cast<uint8_t>(row[x * 4 + 2] * k + 19 * (1.f - k) * 0.6f);
                row[x * 4 + 3] = 255;
            }
        }
        s_splash.picture = bmp;
    }

    // Textures/CapeAssets next to the exe or up to five folders above it (as the loading screen finds it).
    std::filesystem::path FindCapeAssetsDir()
    {
        std::filesystem::path dir = GuiGlobalConstants::GetExeDir();
        for (int i = 0; i < 6 && !dir.empty(); i++)
        {
            std::error_code ec;
            const auto candidate = dir / "Textures" / "CapeAssets";
            if (std::filesystem::is_directory(candidate, ec)) return candidate;
            if (dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
        return {};
    }

    // The guild's banner from the link's cape id, drawn by the loading screen's own composer.
    HBITMAP MakeCapeBitmap(const std::string& capeId)
    {
        int v[7] = {};
        if (capeId.empty() ||
            sscanf_s(capeId.c_str(), "%d-%d-%d-%d-%d-%d-%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7)
            return nullptr;
        CapeData cape;
        cape.shape = v[0]; cape.detail = v[1]; cape.emblem = v[2]; cape.trim = v[3];
        cape.bg_color = v[4]; cape.detail_color = v[5]; cape.emblem_color = v[6];

        static GuildBannerAssetProvider assets;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            const auto root = FindCapeAssetsDir();
            if (!root.empty()) assets.Init(root);
        }
        if (!assets.IsInitialized()) return nullptr;

        std::vector<uint8_t> px = GuildBannerComposer::ComposeBanner(cape, assets);
        constexpr int W = GuildBannerComposer::kWidth, H = GuildBannerComposer::kHeight;
        if (px.size() < static_cast<size_t>(W) * H * 4) return nullptr;
        uint8_t* bits = nullptr;
        HBITMAP bmp = MakeDib(kCapeW, kCapeH, &bits);
        if (!bmp) return nullptr;
        // Area average of the source pixels under each target pixel, in premultiplied alpha.
        const float fx = static_cast<float>(W) / kCapeW, fy = static_cast<float>(H) / kCapeH;
        for (int y = 0; y < kCapeH; y++)
            for (int x = 0; x < kCapeW; x++)
            {
                const int sx0 = static_cast<int>(x * fx), sx1 = std::max(sx0 + 1, static_cast<int>((x + 1) * fx));
                const int sy0 = static_cast<int>(y * fy), sy1 = std::max(sy0 + 1, static_cast<int>((y + 1) * fy));
                float r = 0, g = 0, bl = 0, al = 0;
                int n = 0;
                for (int sy = sy0; sy < sy1 && sy < H; sy++)
                    for (int sx = sx0; sx < sx1 && sx < W; sx++)
                    {
                        const uint8_t* p = px.data() + (static_cast<size_t>(sy) * W + sx) * 4;
                        const float a = p[3] / 255.f;
                        r += p[0] * a; g += p[1] * a; bl += p[2] * a; al += p[3];
                        n++;
                    }
                uint8_t* o = bits + (static_cast<size_t>(y) * kCapeW + x) * 4;
                if (n == 0) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
                o[0] = static_cast<uint8_t>(bl / n + 0.5f);   // BGRA, premultiplied for AlphaBlend
                o[1] = static_cast<uint8_t>(g / n + 0.5f);
                o[2] = static_cast<uint8_t>(r / n + 0.5f);
                o[3] = static_cast<uint8_t>(al / n + 0.5f);
            }
        return bmp;
    }

    void ClearSplashLink()
    {
        for (SplashTeam& t : s_splash.teams)
        {
            if (t.cape) DeleteObject(t.cape);
            t = SplashTeam{};
        }
        s_splash.haveLink = false;
        s_splash.linkFolder.clear();
        s_splash.linkT = -1.f;
        s_splash.linkMapId = 0;
        s_splash.headline.clear();
        s_splash.kicker.clear();
    }

    HFONT Font(int px, int weight)
    {
        return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }

    enum Align { kLeft, kCenter, kRight };

    // One line of text in r, vertically centred, cut with an ellipsis when too wide (the
    // precompiled header turns DrawText off, NODRAWTEXT).
    void Text(HDC dc, HFONT font, COLORREF col, const std::wstring& s, RECT r, Align align)
    {
        HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, col);
        const LONG width = r.right - r.left;
        std::wstring t = s;
        SIZE sz{};
        GetTextExtentPoint32W(dc, t.c_str(), static_cast<int>(t.size()), &sz);
        if (sz.cx > width)
        {
            while (!t.empty())
            {
                t.pop_back();
                const std::wstring cut = t + L"…";
                GetTextExtentPoint32W(dc, cut.c_str(), static_cast<int>(cut.size()), &sz);
                if (sz.cx <= width) { t = cut; break; }
            }
        }
        const int x = align == kLeft ? r.left : align == kCenter ? r.left + (width - sz.cx) / 2 : r.right - sz.cx;
        const int y = r.top + ((r.bottom - r.top) - sz.cy) / 2;
        TextOutW(dc, x, y, t.c_str(), static_cast<int>(t.size()));
        SelectObject(dc, old);
    }

    void Fill(HDC dc, RECT r, COLORREF col)
    {
        HBRUSH b = CreateSolidBrush(col);
        FillRect(dc, &r, b);
        DeleteObject(b);
    }

    void DrawCape(HDC dc, int x, int y, const SplashTeam& team, COLORREF teamColour)
    {
        if (team.cape)
        {
            HDC cdc = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(cdc, team.cape);
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dc, x, y, kCapeW, kCapeH, cdc, 0, 0, kCapeW, kCapeH, bf);
            SelectObject(cdc, old);
            DeleteDC(cdc);
            return;
        }
        // No cape image: a plain banner in the team colour, as the website shows it.
        POINT pts[5] = {{x, y}, {x + kCapeW, y}, {x + kCapeW, y + kCapeH * 88 / 100},
                        {x + kCapeW / 2, y + kCapeH}, {x, y + kCapeH * 88 / 100}};
        HBRUSH b = CreateSolidBrush(teamColour);
        HPEN p = CreatePen(PS_SOLID, 1, teamColour);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
        Polygon(dc, pts, 5);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(b); DeleteObject(p);
    }

    void PaintSplash(HDC target)
    {
        HDC dc = CreateCompatibleDC(target);
        HBITMAP buf = CreateCompatibleBitmap(target, kSplashW, kSplashH);
        HGDIOBJ oldBuf = SelectObject(dc, buf);
        SetBkMode(dc, TRANSPARENT);

        // Background: the darkened map, or the app's panel colour.
        Fill(dc, {0, 0, kSplashW, kSplashH}, RGB(19, 24, 32));
        if (s_splash.picture)
        {
            HDC pdc = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(pdc, s_splash.picture);
            BitBlt(dc, 0, 0, kSplashW, kSplashH, pdc, 0, 0, SRCCOPY);
            SelectObject(pdc, old);
            DeleteDC(pdc);
        }

        HFONT fBrand = Font(14, FW_SEMIBOLD), fKicker = Font(15, FW_SEMIBOLD), fHead = Font(28, FW_BOLD);
        HFONT fName = Font(19, FW_SEMIBOLD), fDetail = Font(14, FW_NORMAL), fVs = Font(14, FW_SEMIBOLD);
        HFONT fStatus = Font(14, FW_NORMAL);

        Text(dc, fBrand, RGB(200, 184, 140), L"GW Observer", {20, 14, 300, 34}, kLeft);

        const std::wstring& head = s_splash.haveLink && !s_splash.headline.empty() ? s_splash.headline : s_splash.title;
        const std::wstring kicker = s_splash.haveLink ? s_splash.kicker : L"Opening a shared match";
        Text(dc, fKicker, RGB(232, 200, 96), kicker, {20, 58, kSplashW - 20, 80}, kCenter);
        Text(dc, fHead, RGB(234, 223, 192), head, {20, 84, kSplashW - 20, 124}, kCenter);

        // The two guilds: blue on the left, red on the right, capes on the outside.
        if (s_splash.haveLink)
        {
            const int top = 150;
            const COLORREF blue = RGB(30, 120, 224), red = RGB(208, 40, 40);
            DrawCape(dc, 44, top, s_splash.teams[0], blue);
            DrawCape(dc, kSplashW - 44 - kCapeW, top, s_splash.teams[1], red);
            // Three lines beside each cape: name, [TAG], rank and rating.
            const int textTop = top + 18;
            const LONG l0 = 44 + kCapeW + 14, r0 = 300, l1 = 340, r1 = kSplashW - 44 - kCapeW - 14;
            const COLORREF grey = RGB(150, 154, 164), gold = RGB(232, 200, 96);
            Text(dc, fName, RGB(111, 180, 255), s_splash.teams[0].name, {l0, textTop, r0, textTop + 26}, kLeft);
            Text(dc, fDetail, grey, s_splash.teams[0].tag, {l0, textTop + 28, r0, textTop + 48}, kLeft);
            Text(dc, fDetail, gold, s_splash.teams[0].stats, {l0, textTop + 50, r0, textTop + 70}, kLeft);
            Text(dc, fName, RGB(255, 122, 122), s_splash.teams[1].name, {l1, textTop, r1, textTop + 26}, kRight);
            Text(dc, fDetail, grey, s_splash.teams[1].tag, {l1, textTop + 28, r1, textTop + 48}, kRight);
            Text(dc, fDetail, gold, s_splash.teams[1].stats, {l1, textTop + 50, r1, textTop + 70}, kRight);
            Text(dc, fVs, RGB(125, 125, 126), L"VS", {300, top + 44, 340, top + 68}, kCenter);
        }

        // Status and progress along the bottom.
        std::wstring pct;
        if (s_splash.fraction >= 0.f)
            pct = std::to_wstring(static_cast<int>(std::clamp(s_splash.fraction, 0.f, 1.f) * 100.f + 0.5f)) + L"%";
        Text(dc, fStatus, RGB(190, 194, 204), s_splash.status, {24, 300, kSplashW - 90, 322}, kLeft);
        if (!pct.empty())
            Text(dc, fStatus, RGB(232, 200, 96), pct, {kSplashW - 90, 300, kSplashW - 24, 322}, kRight);
        const RECT track{24, 330, kSplashW - 24, 334};
        Fill(dc, track, RGB(48, 54, 64));
        if (s_splash.fraction < 0.f)
        {
            // No size known yet: a short block sliding along the track.
            const float phase = static_cast<float>(GetTickCount64() % 1600) / 1600.f;
            const LONG left = track.left + static_cast<LONG>((track.right - track.left - 90) * phase);
            Fill(dc, {left, track.top, left + 90, track.bottom}, RGB(212, 160, 32));
        }
        else
        {
            RECT f = track;
            f.right = track.left + static_cast<LONG>((track.right - track.left) * std::clamp(s_splash.fraction, 0.f, 1.f));
            Fill(dc, f, RGB(212, 160, 32));
        }

        // A thin gold frame, like the website's cards.
        HBRUSH frame = CreateSolidBrush(RGB(106, 88, 48));
        RECT all{0, 0, kSplashW, kSplashH};
        FrameRect(dc, &all, frame);
        DeleteObject(frame);

        for (HFONT f : {fBrand, fKicker, fHead, fName, fDetail, fVs, fStatus}) DeleteObject(f);

        // DEV TOOL (GWO_SPLASH_CAPTURE=<file.bmp>): what the splash draws, saved on every paint, so
        // the splash can be checked without a screenshot of the desktop.
        if (GuiGlobalConstants::IsDeveloperMode())
        {
            wchar_t capPath[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"GWO_SPLASH_CAPTURE", capPath, MAX_PATH) > 0)
            {
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = kSplashW;
                bi.bmiHeader.biHeight = kSplashH;   // bottom-up, as a .bmp stores it
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                std::vector<uint8_t> px(static_cast<size_t>(kSplashW) * kSplashH * 4);
                SelectObject(dc, oldBuf);
                if (GetDIBits(dc, buf, 0, kSplashH, px.data(), &bi, DIB_RGB_COLORS) == kSplashH)
                {
                    BITMAPFILEHEADER fh{};
                    fh.bfType = 0x4D42;
                    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
                    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(px.size());
                    FILE* f = nullptr;
                    if (_wfopen_s(&f, capPath, L"wb") == 0 && f)
                    {
                        fwrite(&fh, sizeof(fh), 1, f);
                        fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
                        fwrite(px.data(), 1, px.size(), f);
                        fclose(f);
                    }
                }
                SelectObject(dc, buf);
            }
        }

        BitBlt(target, 0, 0, kSplashW, kSplashH, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBuf);
        DeleteObject(buf);
        DeleteDC(dc);
    }

    LRESULT CALLBACK SplashProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hWnd, &ps);
            PaintSplash(dc);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_KEYDOWN:
            // Escape: stop waiting in the dark - the Library comes up, the link keeps opening.
            if (wParam == VK_ESCAPE)
                UrlProtocol::ShowLibrary("the splash was dismissed");
            return 0;
        case WM_CLOSE:
            UrlProtocol::ShowLibrary("the splash was closed");
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

void ShowSplash(const std::string& title, const std::string& status, float fraction, int mapId)
{
    const std::wstring wTitle = Wide(title), wStatus = Wide(status);
    if (!s_splash.hwnd)
    {
        static bool registered = false;
        HINSTANCE inst = GetModuleHandleW(nullptr);
        if (!registered)
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = SplashProc;
            wc.hInstance = inst;
            wc.hIcon = LoadIconW(inst, L"IDI_ICON");
            wc.hIconSm = wc.hIcon;
            wc.hCursor = LoadCursorW(nullptr, IDC_APPSTARTING);
            wc.lpszClassName = L"GWObserverLinkSplash";
            registered = RegisterClassExW(&wc) != 0;
        }
        RECT work{0, 0, 1280, 720};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const bool quiet = QuietTestInstance();
        const int x = quiet ? 20000 : work.left + (work.right - work.left - kSplashW) / 2;
        const int y = quiet ? 20000 : work.top + (work.bottom - work.top - kSplashH) / 2;
        s_splash.hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"GWObserverLinkSplash", L"GW Observer - opening shared match",
                                        WS_POPUP, x, y, kSplashW, kSplashH, nullptr, nullptr, inst, nullptr);
        if (!s_splash.hwnd) return;
        RunLog::Line("link: splash shown");
        ShowWindow(s_splash.hwnd, quiet ? SW_SHOWNOACTIVATE : SW_SHOW);
        if (!quiet)
            SetForegroundWindow(s_splash.hwnd);
        UpdateWindow(s_splash.hwnd);
    }
    LoadSplashPicture(mapId > 0 ? mapId : s_splash.linkMapId);
    const bool changed = wTitle != s_splash.title || wStatus != s_splash.status ||
                         std::fabs(fraction - s_splash.fraction) > 0.004f || fraction < 0.f;
    s_splash.title = wTitle;
    s_splash.status = wStatus;
    s_splash.fraction = fraction;
    s_splash.mapId = mapId;
    if (changed)
        InvalidateRect(s_splash.hwnd, nullptr, FALSE);
}

void SetSplashLink(const MatchLink& link)
{
    if (s_splash.haveLink && s_splash.linkFolder == link.folder && s_splash.linkT == link.t) return;
    ClearSplashLink();
    s_splash.haveLink = true;
    s_splash.linkFolder = link.folder;
    s_splash.linkT = link.t;

    std::string date, map, tags[2];
    const bool split = SplitFolder(link.folder, date, map, tags);
    if (split)
        for (const MapAlias& m : kMapAliases)
            if (map == m.name) { s_splash.linkMapId = m.id; break; }
    const wchar_t* english = EnglishMapName(s_splash.linkMapId);

    // "2026/07/08 · A AT · Nomad's Isle", as the loading screen and the website write it.
    std::wstring head;
    auto add = [&](const std::wstring& part) { if (!part.empty()) head += (head.empty() ? L"" : L"  \u00b7  ") + part; };
    if (split)
    {
        add(Wide(date));
        add(Wide(link.occasion));
        add(english ? std::wstring(english) : Wide(map));
    }
    s_splash.headline = head.empty() ? Wide(MatchLinks::DescribeFolder(link.folder)) : head;
    s_splash.kicker = link.t > 0.5f
        ? L"Someone shared a moment from this match at " + Wide(MatchLinks::FormatClock(link.t))
        : std::wstring(L"Someone shared this match");

    for (int i = 0; i < 2; i++)
    {
        const MatchLink::Team& in = link.teams[i];
        SplashTeam& out = s_splash.teams[i];
        const std::wstring tag = tags[i].empty() ? L"" : L"[" + Wide(tags[i]) + L"]";
        if (in.present && !in.name.empty())
        {
            out.name = Wide(in.name);
            out.tag = tag;
        }
        else
            out.name = tag.empty() ? std::wstring(L"Unknown guild") : tag;
        std::wstring stats;
        auto put = [&](const std::wstring& part) { if (!part.empty()) stats += (stats.empty() ? L"" : L"  \u00b7  ") + part; };
        if (in.rank > 0) put(L"Rank #" + std::to_wstring(in.rank));
        if (in.rating > 0) put(L"Rating " + std::to_wstring(in.rating));
        out.stats = stats;
        out.cape = MakeCapeBitmap(in.capeId);
    }
    LoadSplashPicture(s_splash.linkMapId);
    if (s_splash.hwnd) InvalidateRect(s_splash.hwnd, nullptr, FALSE);
}

void HideSplash()
{
    if (!s_splash.hwnd) return;
    DestroyWindow(s_splash.hwnd);
    s_splash.hwnd = nullptr;
    s_splash.title.clear();
    s_splash.status.clear();
    if (s_splash.picture) { DeleteObject(s_splash.picture); s_splash.picture = nullptr; }
    s_splash.pictureMapId = -1;
    ClearSplashLink();
    RunLog::Line("link: splash closed");
}
}
