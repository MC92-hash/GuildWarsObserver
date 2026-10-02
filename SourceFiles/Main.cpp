//
// Main.cpp
//
#include "pch.h"
#include "MapBrowser.h"
#include "GuiGlobalConstants.h"
#include "InputManager.h"
#include "ModelViewer/ModelViewer.h"
#include "Extract_BASS_DLL_resource.h"
#include "imgui.h"
#include "CursorSystem.h"
#include "RunLog.h"
#include <filesystem>
#include <DbgHelp.h>
#include <shellapi.h>
#include "ReplayWindow.h"
#include "ReplayLibrary.h"
#include "SkillDatabase.h"
#include <cstdio>
#include <unordered_map>
#include <vector>

// =================================================================================================
// WHERE A CRASH LEAVES ITS EVIDENCE
//
// Two things used to make a crash here expensive to read, and both are fixed below.
//
// 1. THE DUMP LANDED IN THE WORKING DIRECTORY, under a fixed name. The working directory of this
//    application is not reliably its own: it is whatever the thing that launched it happened to be
//    in, which has already been observed to be a different repository's build output. And a fixed
//    name means the next crash overwrites the one somebody was about to report. So the dump now
//    goes next to the run log, in the profile directory this process can always write, under a name
//    stamped with the date and time; the old working-directory name is only a fallback.
//
// 2. NOTHING SAID WHERE IT FAULTED unless a symbol file that matched exactly was still on disk.
//    The three facts that identify a fault - the exception code, the address, and WHICH MODULE OWNS
//    that address with its load base, so the offset within it can be computed - are all knowable
//    inside the handler and none of them need symbols. They now go in the run log, together with
//    the breadcrumb the draw pass leaves, which names the step that was running.
// =================================================================================================
namespace
{
std::wstring CrashDumpPath()
{
    const std::filesystem::path& log = RunLog::Path();
    if (log.empty())
        return L"CrashDump.dmp";

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t stamp[32] = {};
    std::wcsftime(stamp, 32, L"%Y-%m-%d_%H%M%S", &tm);

    std::filesystem::path dump = log.parent_path() / (std::wstring(L"CrashDump_") + stamp + L".dmp");
    return dump.wstring();
}

// Everything the run log can say about a fault without a symbol file: the code, the address, and
// the module that address belongs to with the offset inside it. An offset into a named module is
// what a debugger needs to place the fault, and it survives the binary being rebuilt.
void LogFaultSite(EXCEPTION_POINTERS* pointers, const std::wstring& dumpPath, const char* kind)
{
    RunLog::Line("crash: *** %s ***", kind);

    if (pointers != nullptr && pointers->ExceptionRecord != nullptr)
    {
        const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
        const void* address = record->ExceptionAddress;

        RunLog::Line("crash: code 0x%08X at address 0x%p",
                     static_cast<unsigned>(record->ExceptionCode), address);

        HMODULE owner = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(address), &owner) && owner != nullptr)
        {
            wchar_t name[MAX_PATH] = {};
            GetModuleFileNameW(owner, name, MAX_PATH);
            const auto base = reinterpret_cast<const unsigned char*>(owner);
            const auto offset = static_cast<const unsigned char*>(address) - base;
            RunLog::Line("crash: in '%s' loaded at 0x%p, offset 0x%llX",
                         std::filesystem::path(name).filename().string().c_str(),
                         static_cast<const void*>(base),
                         static_cast<unsigned long long>(offset));
        }
        else
        {
            RunLog::Line("crash: the address belongs to no loaded module");
        }

        // An access violation records what it was reaching for, which is usually the whole answer.
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            record->NumberParameters >= 2)
        {
            RunLog::Line("crash: %s address 0x%p",
                         record->ExceptionInformation[0] == 0 ? "reading" : "writing",
                         reinterpret_cast<const void*>(record->ExceptionInformation[1]));
        }
    }

    const std::string crumb = RunLog::Read();
    if (!crumb.empty())
        RunLog::Line("crash: the last step that reported itself was %s", crumb.c_str());
    else
        RunLog::Line("crash: no step had reported itself yet");

    RunLog::Line("crash: dump written to '%s'", std::filesystem::path(dumpPath).string().c_str());
}
} // namespace

LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* pExceptionPointers) {
    // Create mini dump file
    const std::wstring dumpPath = CrashDumpPath();
    HANDLE hDumpFile = CreateFile(dumpPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    MINIDUMP_EXCEPTION_INFORMATION dumpInfo;
    dumpInfo.ExceptionPointers = pExceptionPointers;
    dumpInfo.ThreadId = GetCurrentThreadId();
    dumpInfo.ClientPointers = TRUE;

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile, 
        static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs), 
        &dumpInfo, NULL, NULL);

    CloseHandle(hDumpFile);

    // Into the run log FIRST: the message box below waits for somebody to click it, and the stack
    // walk that fills it needs a symbol file it may not have. This does not.
    LogFaultSite(pExceptionPointers, dumpPath, "unhandled exception");

    // Show error info to user
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    CONTEXT context = *pExceptionPointers->ContextRecord;

    STACKFRAME64 stackFrame;
    memset(&stackFrame, 0, sizeof(STACKFRAME64));

#ifdef _M_IX86 // Check whether the build is 32 or 64 bits
    int machineType = IMAGE_FILE_MACHINE_I386;
    stackFrame.AddrPC.Offset = context.Eip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context.Ebp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context.Esp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#elif _M_X64
    int machineType = IMAGE_FILE_MACHINE_AMD64;
    stackFrame.AddrPC.Offset = context.Rip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context.Rsp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context.Rsp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#endif

    SymInitialize(process, NULL, TRUE);

    std::stringstream ss;
    ss << "Sorry! Guild Wars Observer just crashed unexpectedly.\n";
    ss << "A dump file has been created:\n" << std::filesystem::path(dumpPath).string() << "\n";
    ss << "The run log beside it says where the fault was.\n";
    ss << "Please contact the developers or create an issue on Github with both files attached if possible.\n\n";
    ss << "-------------------------------------------------------------------------------\n";
    ss << "Unhandled exception occurred.\nException Code: " << std::hex << pExceptionPointers->ExceptionRecord->ExceptionCode << std::endl;
    ss << "Call Stack:\n";

    SYMBOL_INFO* symbol = (SYMBOL_INFO*)calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    IMAGEHLP_LINE64 line;
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    DWORD displacement;

    while (StackWalk64(
        machineType, process, thread, &stackFrame, &context, NULL,
        SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {

        // Obtain the symbol for the address
        if (SymFromAddr(process, stackFrame.AddrPC.Offset, 0, symbol)) {
            ss << "Function: " << symbol->Name << " - Address: 0x" << std::hex << symbol->Address << std::endl;
        }

        // Try to obtain the file and line number for the address
        if (SymGetLineFromAddr64(process, stackFrame.AddrPC.Offset, &displacement, &line)) {
            ss << "File: " << line.FileName << " - Line: 0x" << std::hex << line.LineNumber << std::endl;
        }

        // Check for end of stack or invalid frame
        if (stackFrame.AddrPC.Offset == 0) {
            break;
        }
    }

    free(symbol);
    SymCleanup(process);

    MessageBoxA(NULL, ss.str().c_str(), "Critical Error", MB_ICONERROR | MB_OK);

    ExitProcess(pExceptionPointers->ExceptionRecord->ExceptionCode);

    return EXCEPTION_EXECUTE_HANDLER;
}

// Reports a C++ exception that escaped a frame update, writes a dump, and exits.
// Unlike the SEH filter above this has the exception message, which is what
// actually identifies the failure in a release build without symbols.
void ReportFatalCppException(const char* what)
{
    const std::wstring dumpPath = CrashDumpPath();
    HANDLE hDumpFile = CreateFile(dumpPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDumpFile != INVALID_HANDLE_VALUE)
    {
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile,
            static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs),
            nullptr, NULL, NULL);
        CloseHandle(hDumpFile);
    }

    RunLog::Line("crash: *** a C++ exception escaped the frame loop ***");
    RunLog::Line("crash: %s", what != nullptr ? what : "(no message)");
    LogFaultSite(nullptr, dumpPath, "unhandled C++ exception");

    std::string msg =
        "Sorry! Guild Wars Observer just crashed unexpectedly.\n"
        "A dump file has been created:\n";
    msg += std::filesystem::path(dumpPath).string();
    msg +=
        "\nThe run log beside it says where the fault was.\n"
        "Please contact the developers or create an issue on Github with both files attached if possible.\n\n"
        "-------------------------------------------------------------------------------\n"
        "Unhandled C++ exception (0xE06D7363).\n\n";
    msg += what ? what : "(no message)";

    MessageBoxA(NULL, msg.c_str(), "Critical Error", MB_ICONERROR | MB_OK);
    ExitProcess(0xE06D7363);
}

// BASS
extern LPFNBASSSTREAMCREATEFILE lpfnBassStreamCreateFile = nullptr;
extern LPFNBASSCHANNELPLAY lpfnBassChannelPlay = nullptr;
extern LPFNBASSCHANNELPAUSE lpfnBassChannelPause = nullptr;
extern LPFNBASSCHANNELSTOP lpfnBassChannelStop = nullptr;
extern LPFNBASSCHANNELBYTES2SECONDS lpfnBassChannelBytes2Seconds = nullptr;
extern LPFNBASSCHANNELGETLENGTH lpfnBassChannelGetLength = nullptr;
extern LPFNBASSSTREAMGETFILEPOSITION lpfnBassStreamGetFilePosition = nullptr;
extern LPFNBASSCHANNELGETINFO lpfnBassChannelGetInfo = nullptr;
extern LPFNBASSCHANNELFLAGS lpfnBassChannelFlags = nullptr;
extern LPFNBASSSTREAMFREE lpfnBassStreamFree = nullptr;
extern LPFNBASSCHANNELSETPOSITION lpfnBassChannelSetPosition = nullptr;
extern LPFNBASSCHANNELGETPOSITION lpfnBassChannelGetPosition = nullptr;
extern LPFNBASSCHANNELSECONDS2BYTES lpfnBassChannelSeconds2Bytes = nullptr;
extern LPFNBASSCHANNELSETATTRIBUTE lpfnBassChannelSetAttribute = nullptr;

// BASS_FX
extern LPFNBASSFXTMPOCREATE lpfnBassFxTempoCreate = nullptr;

using namespace DirectX;

#ifdef __clang__
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wswitch-enum"
#endif

#pragma warning(disable : 4061)

extern bool is_bass_working = false;
extern HMODULE hBassDll = 0;
extern HMODULE hBassFxDll = 0;

namespace
{
    std::unique_ptr<MapBrowser> g_map_browser;
    std::unique_ptr<InputManager> g_input_manager;

    // Model viewer mouse state
    bool g_modelViewerLeftDown = false;
    bool g_modelViewerRightDown = false;
    POINT g_modelViewerLastMousePos = { 0, 0 };
    POINT g_modelViewerClickStartPos = { 0, 0 };  // For detecting clicks vs drags
}

LPCWSTR g_szAppName = L"Guild Wars Observer";

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void ExitMapBrowser() noexcept;

// Indicates to hybrid graphics systems to prefer the discrete part by default
extern "C"
{
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// Entry point

// ---------------------------------------------------------------------------
// Headless attribute export
//
//   GuildWarsObserver.exe --export-attributes <match folder> [--out <file>]
//
// Runs the replay analysis with no window, no device and no map, and writes the solved
// attribute and rune builds as JSON. This is what puts runes and attributes on the website
// automatically: `upload_to_r2.py` shells out to it for every match it publishes, the same way
// it already builds the equipment object, so a recording made overnight arrives in Scout with
// its builds solved and nobody has to open a replay.
//
// Exit codes are the contract the publish step reads:
//   0  wrote the file
//   2  bad usage
//   3  the folder or its infos.json is unreadable
//   4  no skill database, so no breakpoints and nothing to solve against
//   5  the analysis stalled (a parser failed, or the recording carries no combat)
//   6  solved nothing worth writing, or the file could not be written
// ---------------------------------------------------------------------------

static int RunHeadlessAttributeExport(const std::wstring& folderArg, const std::wstring& outArg)
{
    // A GUI-subsystem binary has no stdout of its own. Borrowing the caller's console makes the
    // diagnostics visible when a human runs this by hand and costs nothing when Python does.
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }

    const std::filesystem::path folder = folderArg;
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec))
    {
        std::fprintf(stderr, "export-attributes: not a directory: %ls\n", folder.c_str());
        return 3;
    }

    MatchMeta match;
    if (!LocalReplayProvider::ParseInfosJson(folder / "infos.json", match))
    {
        std::fprintf(stderr, "export-attributes: cannot read %ls/infos.json\n", folder.c_str());
        return 3;
    }
    match.folder_path = folder.string();
    match.folder_name = folder.filename().string();

    // The solver reads skill descriptions for their breakpoint endpoints; without the database
    // every attribute would come back budget-only, which is worse than refusing outright.
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto dir = std::filesystem::path(exePath).parent_path();
        bool loaded = false;
        for (int i = 0; i < 5; i++)
        {
            if (std::filesystem::exists(dir / "Data" / "skilldata.json"))
            {
                GetSkillDatabase().Load((dir / "Data").string());
                GetSkillDatabase().LoadPatches((dir / "Data").string());
                loaded = true;
                break;
            }
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
        if (!loaded)
        {
            std::fprintf(stderr, "export-attributes: no Data/skilldata.json near the exe\n");
            return 4;
        }
    }

    // No dat manager and an empty hash index: both are the map's business, and the analysis
    // never asks about geometry.
    static const std::unordered_map<int, std::vector<int>> kNoHashIndex;
    ReplayWindow* rw = ReplayWindow::CreateHeadless(match, nullptr, kNoHashIndex);
    if (!rw)
    {
        std::fprintf(stderr, "export-attributes: could not open the replay\n");
        return 3;
    }

    while (!rw->AnalysisComplete() && !rw->AnalysisStalled())
    {
        rw->TickHeadless();
        // The parsers run on their own threads and the passes are gated on their completion, so
        // this loop is mostly waiting. Sleeping keeps it off a core it cannot use.
        Sleep(10);
    }

    int rc = 6;
    if (rw->AnalysisComplete())
    {
        const std::filesystem::path out =
            outArg.empty() ? (folder / "attributes.json") : std::filesystem::path(outArg);
        if (rw->ExportAttributes(out))
        {
            std::printf("export-attributes: wrote %ls\n", out.c_str());
            rc = 0;
        }
        else
        {
            std::fprintf(stderr, "export-attributes: nothing solved for %s\n",
                         match.folder_name.c_str());
        }
    }
    else
    {
        std::fprintf(stderr, "export-attributes: analysis stalled for %s\n",
                     match.folder_name.c_str());
        rc = 5;
    }

    delete rw;
    return rc;
}

// Returns true when the command line asked for a batch export, in which case `exitCode` is the
// process's result and no window should be created.
static bool TryHeadlessCommandLine(LPWSTR lpCmdLine, int& exitCode)
{
    if (!lpCmdLine || !*lpCmdLine) return false;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(lpCmdLine, &argc);
    if (!argv) return false;

    std::wstring folder, out;
    for (int i = 0; i < argc; i++)
    {
        if (wcscmp(argv[i], L"--export-attributes") == 0 && i + 1 < argc) folder = argv[++i];
        else if (wcscmp(argv[i], L"--out") == 0 && i + 1 < argc)          out    = argv[++i];
    }
    LocalFree(argv);

    if (folder.empty()) return false;
    exitCode = RunHeadlessAttributeExport(folder, out);
    return true;
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);

    UNREFERENCED_PARAMETER(hPrevInstance);

    // Batch export: no window, no device, no message loop. Checked before anything else so
    // a headless run costs none of the GUI setup below.
    {
        int exitCode = 0;
        if (TryHeadlessCommandLine(lpCmdLine, exitCode))
            return exitCode;
    }

    if (! XMVerifyCPUSupport())
        return 1;

    HRESULT hr = CoInitializeEx(nullptr, COINITBASE_MULTITHREADED);
    if (FAILED(hr))
        return 1;

    // Clean up leftover files from a previous in-app update
    {
        std::error_code ec;
        auto exeDir = std::filesystem::current_path();
        std::filesystem::remove(exeDir / "_gwobs_update.bat", ec);
        std::filesystem::remove(exeDir / "GWObserver_update.exe", ec);
        std::filesystem::remove(exeDir / "GWObserver_update.zip", ec);
    }

    // Register class and create window
    {
        // Register class
        WNDCLASSEXW wcex = {};
        wcex.cbSize = sizeof(WNDCLASSEXW);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = WndProc;
        wcex.hInstance = hInstance;
        wcex.hIcon = LoadIconW(hInstance, L"IDI_ICON");
        wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wcex.hbrBackground = CreateSolidBrush(RGB(20, 24, 30));
        wcex.lpszClassName = L"GuildWarsObserverWindowClass";
        wcex.hIconSm = LoadIconW(wcex.hInstance, L"IDI_ICON");
        if (! RegisterClassExW(&wcex))
            return 1;

        // Load settings
        GuiGlobalConstants::LoadSettings();

        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;
        int w, h;

        if (GuiGlobalConstants::window_width != -1) {
            w = GuiGlobalConstants::window_width;
            h = GuiGlobalConstants::window_height;
            x = GuiGlobalConstants::window_pos_x;
            y = GuiGlobalConstants::window_pos_y;
        } else {
            g_map_browser->GetDefaultSize(w, h);
            RECT rc = { 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
            AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
            w = rc.right - rc.left;
            h = rc.bottom - rc.top;
        }

        // Create window
        HWND hwnd = CreateWindowExW(0, L"GuildWarsObserverWindowClass", g_szAppName, WS_OVERLAPPEDWINDOW,
            x, y, w, h,
            nullptr, nullptr, hInstance, nullptr);
        // TODO: Change to CreateWindowExW(WS_EX_TOPMOST, L"GuildWarsObserverWindowClass", g_szAppName, WS_POPUP,
        // to default to fullscreen.

        g_input_manager = std::make_unique<InputManager>(hwnd);
        g_map_browser = std::make_unique<MapBrowser>(g_input_manager.get());

        if (! hwnd)
            return 1;

        int showCmd = nCmdShow;
        if (GuiGlobalConstants::window_width != -1 && GuiGlobalConstants::window_maximized) {
            showCmd = SW_SHOWMAXIMIZED;
        }
        ShowWindow(hwnd, showCmd);
        // TODO: Change nCmdShow to SW_SHOWMAXIMIZED to default to fullscreen.

        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(g_map_browser.get()));

        RECT rc;
        GetClientRect(hwnd, &rc);

        std::filesystem::path exePath = std::filesystem::current_path();
        std::filesystem::path bass_dllPath = exePath / "bass.dll";
        if (std::filesystem::exists(bass_dllPath) || extract_bass_dll_resource())
        {
            std::filesystem::path bass_fx_dllPath = exePath / "bass_fx.dll";
            if (std::filesystem::exists(bass_fx_dllPath) || extract_bass_fx_dll_resource())
            {

                hBassDll = LoadLibrary(TEXT("bass.dll"));
                hBassFxDll = LoadLibrary(TEXT("bass_fx.dll"));

                // Load the DLL
                if (hBassDll != NULL && hBassFxDll != NULL)
                {
                    // Get a pointer to the BASS_Init function
                    LPFNBASSINIT lpfnBassInit = (LPFNBASSINIT)GetProcAddress(hBassDll, "BASS_Init");
                    if (lpfnBassInit != NULL)
                    {
                        // Call BASS_Init through the function pointer
                        is_bass_working = lpfnBassInit(-1, 44100, 0, hwnd, NULL);
                    }

                    if (is_bass_working)
                    {
                        lpfnBassStreamCreateFile =
                            (LPFNBASSSTREAMCREATEFILE)GetProcAddress(hBassDll, "BASS_StreamCreateFile");
                        lpfnBassChannelPlay =
                            (LPFNBASSCHANNELPLAY)GetProcAddress(hBassDll, "BASS_ChannelPlay");
                        lpfnBassChannelPause =
                            (LPFNBASSCHANNELPAUSE)GetProcAddress(hBassDll, "BASS_ChannelPause");
                        lpfnBassChannelStop =
                            (LPFNBASSCHANNELSTOP)GetProcAddress(hBassDll, "BASS_ChannelStop");
                        lpfnBassChannelBytes2Seconds =
                            (LPFNBASSCHANNELBYTES2SECONDS)GetProcAddress(hBassDll, "BASS_ChannelBytes2Seconds");
                        lpfnBassChannelGetLength =
                            (LPFNBASSCHANNELGETLENGTH)GetProcAddress(hBassDll, "BASS_ChannelGetLength");
                        lpfnBassStreamGetFilePosition = (LPFNBASSSTREAMGETFILEPOSITION)GetProcAddress(
                            hBassDll, "BASS_StreamGetFilePosition");
                        lpfnBassChannelGetInfo =
                            (LPFNBASSCHANNELGETINFO)GetProcAddress(hBassDll, "BASS_ChannelGetInfo");
                        lpfnBassChannelFlags =
                            (LPFNBASSCHANNELFLAGS)GetProcAddress(hBassDll, "BASS_ChannelFlags");
                        lpfnBassStreamFree = (LPFNBASSSTREAMFREE)GetProcAddress(hBassDll, "BASS_StreamFree");
                        lpfnBassChannelSetPosition =
                            (LPFNBASSCHANNELSETPOSITION)GetProcAddress(hBassDll, "BASS_ChannelSetPosition");
                        lpfnBassChannelGetPosition =
                            (LPFNBASSCHANNELGETPOSITION)GetProcAddress(hBassDll, "BASS_ChannelGetPosition");
                        lpfnBassChannelSeconds2Bytes =
                            (LPFNBASSCHANNELSECONDS2BYTES)GetProcAddress(hBassDll, "BASS_ChannelSeconds2Bytes");
                        lpfnBassChannelSetAttribute =
                            (LPFNBASSCHANNELSETATTRIBUTE)GetProcAddress(hBassDll, "BASS_ChannelSetAttribute");
                        lpfnBassFxTempoCreate =
                            (LPFNBASSFXTMPOCREATE)GetProcAddress(hBassFxDll, "BASS_FX_TempoCreate");
                    }
                }
            }
        }

        g_map_browser->Initialize(hwnd, rc.right - rc.left, rc.bottom - rc.top);
    }

    // Main message loop
    MSG msg = {};
    while (WM_QUIT != msg.message)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            // A C++ exception escaping a frame reaches the unhandled exception
            // filter as code 0xE06D7363, where the reported call stack is
            // unsymbolised and useless. Catch it here while the message is
            // still available so crash reports say what actually failed.
            try
            {
                g_map_browser->Tick();
            }
            catch (const std::exception& e)
            {
                ReportFatalCppException(e.what());
            }
            catch (...)
            {
                ReportFatalCppException("Unknown exception type (not derived from std::exception).");
            }
        }
    }

    g_map_browser.reset();

    CoUninitialize();

    return static_cast<int>(msg.wParam);
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
    LPARAM lParam);

static void UpdateWindowSettings(HWND hWnd)
{
    WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
    if (GetWindowPlacement(hWnd, &wp)) {
        // Only update if we have valid data (not completely zero)
        // Check if window is visible? No, GetWindowPlacement works.
        
        // Don't save if minimized, keep last known restored pos
        if (wp.showCmd != SW_SHOWMINIMIZED) {
            GuiGlobalConstants::window_maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
            
            // rcNormalPosition gives the restored position even if maximized
            GuiGlobalConstants::window_width = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
            GuiGlobalConstants::window_height = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
            GuiGlobalConstants::window_pos_x = wp.rcNormalPosition.left;
            GuiGlobalConstants::window_pos_y = wp.rcNormalPosition.top;
        }
    }
}

// Windows procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
        return true;

    static bool s_in_sizemove = false;
    static bool s_in_suspend = false;
    static bool s_minimized = false;
    static bool s_fullscreen = false;
    // TODO: Set s_fullscreen to true if defaulting to fullscreen.

    auto map_browser = reinterpret_cast<MapBrowser*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (message)
    {
    case WM_KEYDOWN:
        // Handle F key for model viewer fit-to-model
        if (g_modelViewerState.isActive && wParam == 'F' && !ImGui::GetIO().WantCaptureKeyboard)
        {
            g_modelViewerState.camera->FitToBounds(
                g_modelViewerState.boundsMin,
                g_modelViewerState.boundsMax);
        }
        g_input_manager->OnKeyDown(static_cast<UINT>(wParam), hWnd);
        break;

    case WM_KEYUP:
        g_input_manager->OnKeyUp(static_cast<UINT>(wParam), hWnd);
        break;

    case WM_MOUSEMOVE:
        // Handle model viewer mouse drag
        if (g_modelViewerState.isActive && (g_modelViewerLeftDown || g_modelViewerRightDown))
        {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            float deltaX = static_cast<float>(x - g_modelViewerLastMousePos.x);
            float deltaY = static_cast<float>(y - g_modelViewerLastMousePos.y);
            g_modelViewerLastMousePos.x = x;
            g_modelViewerLastMousePos.y = y;

            if (g_modelViewerLeftDown)
            {
                g_modelViewerState.camera->OnOrbitDrag(deltaX, deltaY);
            }
            else if (g_modelViewerRightDown)
            {
                g_modelViewerState.camera->OnPanDrag(deltaX, deltaY);
            }
        }
        break;
    case WM_INPUT:
    {
        // Skip raw input when model viewer is active
        if (!g_modelViewerState.isActive)
        {
            g_input_manager->ProcessRawInput(lParam);
        }
        break;
    }
    case WM_LBUTTONDOWN:
        if (g_modelViewerState.isActive && !ImGui::GetIO().WantCaptureMouse)
        {
            g_modelViewerLeftDown = true;
            g_modelViewerLastMousePos.x = GET_X_LPARAM(lParam);
            g_modelViewerLastMousePos.y = GET_Y_LPARAM(lParam);
            g_modelViewerClickStartPos = g_modelViewerLastMousePos;  // Store start pos for click detection
            SetCapture(hWnd);
        }
        else if (!g_modelViewerState.isActive)
        {
            g_input_manager->OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, hWnd, message);
        }
        break;
    case WM_MBUTTONDOWN:
        g_input_manager->OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, hWnd, message);
        break;
    case WM_RBUTTONDOWN:
        if (g_modelViewerState.isActive && !ImGui::GetIO().WantCaptureMouse)
        {
            g_modelViewerRightDown = true;
            g_modelViewerLastMousePos.x = GET_X_LPARAM(lParam);
            g_modelViewerLastMousePos.y = GET_Y_LPARAM(lParam);
            SetCapture(hWnd);
        }
        else if (!g_modelViewerState.isActive)
        {
            g_input_manager->OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, hWnd, message);
        }
        break;

    case WM_LBUTTONUP:
        if (g_modelViewerState.isActive)
        {
            // Check for bone picking if it was a click (not a drag)
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            int dx = x - g_modelViewerClickStartPos.x;  // Compare against start pos, not last pos
            int dy = y - g_modelViewerClickStartPos.y;
            bool wasClick = (abs(dx) < 5 && abs(dy) < 5);

            if (wasClick && !ImGui::GetIO().WantCaptureMouse)
            {
                RECT rect;
                GetClientRect(hWnd, &rect);
                float width = static_cast<float>(rect.right - rect.left);
                float height = static_cast<float>(rect.bottom - rect.top);
                int pickedBone = PickBoneAtScreenPos(static_cast<float>(x), static_cast<float>(y), width, height);
                g_modelViewerState.SelectBone(pickedBone);
            }

            g_modelViewerLeftDown = false;
            if (!g_modelViewerRightDown) ReleaseCapture();
        }
        else
        {
            g_input_manager->OnMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, hWnd, message);
        }
        break;
    case WM_MBUTTONUP:
        g_input_manager->OnMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, hWnd, message);
        break;
    case WM_RBUTTONUP:
        if (g_modelViewerState.isActive)
        {
            g_modelViewerRightDown = false;
            if (!g_modelViewerLeftDown) ReleaseCapture();
        }
        else
        {
            g_input_manager->OnMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, hWnd, message);
        }
        break;
    case WM_MOUSEWHEEL:
        if (g_modelViewerState.isActive && !ImGui::GetIO().WantCaptureMouse)
        {
            float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            g_modelViewerState.camera->OnZoom(delta);
        }
        else if (!g_modelViewerState.isActive)
        {
            g_input_manager->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam), hWnd);
        }
        break;
    case WM_MOUSELEAVE:
        g_input_manager->OnMouseLeave(hWnd);
        break;
    case WM_PAINT:
        if (s_in_sizemove && map_browser)
        {
        }
        else
        {
            PAINTSTRUCT ps;
            std::ignore = BeginPaint(hWnd, &ps);
            EndPaint(hWnd, &ps);
        }
        break;

    case WM_DISPLAYCHANGE:
        if (map_browser)
        {
            map_browser->OnDisplayChange();
        }
        break;

    case WM_MOVE:
        if (map_browser)
        {
            map_browser->OnWindowMoved();
        }
        UpdateWindowSettings(hWnd);
        break;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
        {
            if (! s_minimized)
            {
                s_minimized = true;
                if (! s_in_suspend && map_browser)
                    map_browser->OnSuspending();
                s_in_suspend = true;
            }
        }
        else if (s_minimized)
        {
            s_minimized = false;
            if (s_in_suspend && map_browser)
                map_browser->OnResuming();
            s_in_suspend = false;
        }
        else if (! s_in_sizemove && map_browser)
        {
            map_browser->OnWindowSizeChanged(LOWORD(lParam), HIWORD(lParam));
        }
        UpdateWindowSettings(hWnd);
        break;

    case WM_ENTERSIZEMOVE:
        s_in_sizemove = true;
        break;

    case WM_EXITSIZEMOVE:
        s_in_sizemove = false;
        if (map_browser)
        {
            RECT rc;
            GetClientRect(hWnd, &rc);

            map_browser->OnWindowSizeChanged(rc.right - rc.left, rc.bottom - rc.top);
        }
        UpdateWindowSettings(hWnd);
        break;

    case WM_GETMINMAXINFO:
        if (lParam)
        {
            auto info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 320;
            info->ptMinTrackSize.y = 200;
        }
        break;

    case WM_ACTIVATEAPP:
        if (map_browser)
        {
            if (wParam)
            {
                map_browser->OnActivated();
            }
            else
            {
                map_browser->OnDeactivated();
            }
        }
        break;

    case WM_ACTIVATE:
        if (g_input_manager)
        {
            if (LOWORD(wParam) != WA_INACTIVE)
                g_input_manager->ReRegisterRawInput();
            else
                g_input_manager->OnFocusLost();
        }
        break;

    case WM_POWERBROADCAST:
        switch (wParam)
        {
        case PBT_APMQUERYSUSPEND:
            if (! s_in_suspend && map_browser)
                map_browser->OnSuspending();
            s_in_suspend = true;
            return TRUE;

        case PBT_APMRESUMESUSPEND:
            if (! s_minimized)
            {
                if (s_in_suspend && map_browser)
                    map_browser->OnResuming();
                s_in_suspend = false;
            }
            return TRUE;
        }
        break;

    case WM_SETCURSOR:
        g_CursorInClientArea = (LOWORD(lParam) == HTCLIENT);
        if (g_CursorInClientArea && g_Cursors.loaded)
        {
            if (g_AppBusy)
            {
                if (HCURSOR w = g_Cursors.Get(CursorMode::Wait))
                { ::SetCursor(w); return TRUE; }
            }
            if (g_DraggingWindow)
            {
                ::SetCursor(g_Cursors.Get(CursorMode::Move));
                return TRUE;
            }
            HCURSOR cur = g_Cursors.Get(g_CurrentCursor);
            if (cur) { ::SetCursor(cur); return TRUE; }
        }
        break;

    case WM_DESTROY:
        UpdateWindowSettings(hWnd);
        GuiGlobalConstants::SaveSettings();
        PostQuitMessage(0);
        break;

    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN && (lParam & 0x60000000) == 0x20000000)
        {
            // Implements the classic ALT+ENTER fullscreen toggle
            if (s_fullscreen)
            {
                SetWindowLongPtr(hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
                SetWindowLongPtr(hWnd, GWL_EXSTYLE, 0);

                int width = 800;
                int height = 600;
                if (map_browser)
                    map_browser->GetDefaultSize(width, height);

                ShowWindow(hWnd, SW_SHOWNORMAL);

                SetWindowPos(hWnd, HWND_TOP, 0, 0, width, height,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
            }
            else
            {
                SetWindowLongPtr(hWnd, GWL_STYLE, WS_POPUP);
                SetWindowLongPtr(hWnd, GWL_EXSTYLE, WS_EX_TOPMOST);

                SetWindowPos(hWnd, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

                ShowWindow(hWnd, SW_SHOWMAXIMIZED);
            }

            s_fullscreen = ! s_fullscreen;
        }
        break;

    case WM_MENUCHAR:
        // A menu is active and the user presses a key that does not correspond
        // to any mnemonic or accelerator key. Ignore so we don't produce an error beep.
        return MAKELRESULT(0, MNC_CLOSE);
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

// Exit helper
void ExitMapBrowser() noexcept { PostQuitMessage(0); }
