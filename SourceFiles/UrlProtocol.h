#pragma once
// ---------------------------------------------------------------------------
// UrlProtocol - share links reaching the app (SHARE_LINKS_PLAN.md sections 2-4b)
//
//   - the gwobserver:// link type, registered under HKCU by release builds;
//   - a second process started by a link hands it to the running one (WM_COPYDATA) and exits;
//   - one process-wide slot holds the link until MapBrowser::ProcessPendingLink can open it;
//   - "link-only" starts: a start BY a link keeps the Library window hidden and shows a small
//     splash until the replay opens; closing that replay quits the app.
//
// Everything here runs on the main thread.
// ---------------------------------------------------------------------------
#include "MatchLink.h"
#include <functional>
#include <optional>
#include <string>
#include <windows.h>

namespace UrlProtocol
{
    // The Library window's class, which is how a second process finds the running one.
    // DEV TOOL: GWO_INSTANCE_TAG=<tag> (developer builds) appends _<tag>, so a test instance
    // neither finds nor is found by the copy somebody is using.
    const wchar_t* MainWindowClass();
    // The same tag also makes the instance QUIET: its windows open off-screen or minimised and
    // never take the foreground, and questions get their default answer (hidden test runs).
    bool QuietTestInstance();

    // WM_COPYDATA dwData tag of a forwarded link ('GWOL'). The payload is the link, UTF-8.
    constexpr ULONG_PTR kCopyDataTag = 0x474F574C;

    // --open-link <url>, or a bare argument that is a share link. Empty when there is none.
    std::wstring FindLinkArgument(const wchar_t* cmdLine);

    // Hands `link` to an already running GW Observer. True when one took it.
    bool ForwardToRunningInstance(const std::wstring& link);

    // Writes HKCU\Software\Classes\gwobserver when it does not point at this exe yet. Release
    // builds only: a developer build skips it unless GWO_REGISTER_LINKS=1.
    void RegisterIfNeeded();

    // WM_COPYDATA from ForwardToRunningInstance: parses and queues the link. True when it was ours.
    bool HandleCopyData(const COPYDATASTRUCT* cds);

    // Text from anywhere (command line, a forwarded message, the Library search box): parsed and
    // queued, or a toast explaining why not. `source` names it in the run log.
    bool SubmitLinkText(const std::string& text, const char* source);

    // The slot. ProcessPendingLink peeks, and clears once it acted.
    void SetPendingLink(const MatchLink& link);
    const MatchLink* PeekPendingLink();
    void ClearPendingLink();

    // --- Link-only start (no Library window) ---
    void SetMainWindow(HWND hwnd, int libraryShowCmd);
    void SetLibraryHidden(bool hidden);
    bool LibraryHidden();
    // Shows the Library (and brings it to the front). No-op when it is already shown.
    void ShowLibrary(const char* why);

    // --- Library toast (drawn by MapBrowser::Render over the Library) ---
    void Toast(const std::string& text, bool isError = false, double seconds = 5.0);
    void DrawLibraryToast();

    // --- Which matches can be shared: the ones in the cloud index. MapBrowser installs the
    // lookup; without one nothing can be shared. ---
    void SetPublishedLookup(std::function<bool(const std::string&)> lookup);
    bool IsPublished(const std::string& folder);
    inline const char* kNotPublishedTip = "Only published matches can be shared";

    std::string ToUtf8(const std::wstring& text);

    // UTF-8 text onto the clipboard.
    bool CopyToClipboard(const std::string& text, HWND owner);

    // --- The link-only start's splash: "Opening shared match... 40%" ---
    // fraction < 0: no percentage yet. mapId > 0 draws Textures/Map_Pictures/<mapId>.jpg.
    void ShowSplash(const std::string& title, const std::string& status, float fraction, int mapId);
    // The link the splash shows: guilds with their capes (link g1/g2), date, occasion, English map
    // name, the shared moment. Call before ShowSplash; kept until HideSplash.
    void SetSplashLink(const MatchLink& link);
    void HideSplash();
}
