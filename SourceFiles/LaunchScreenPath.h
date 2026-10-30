#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include "imgui.h"
#include "SeasonalEvents.h"
#include <ctime>
#include <filesystem>
#include <string>

// Main splash image, picked by the user's local date:
//   Halloween  Oct 18 - Nov 2   -> GWOBS_Loading_Screen_Halloween.png
//   Wintersday Dec 19 - Jan 2   -> GWOBS_Loading_Screen_Wintersday.png
//   otherwise                   -> GWOBS_Loading_Screen_1.png
// A missing seasonal file falls back to the normal image.
// Test override: GWO_LAUNCH_SEASON=halloween | wintersday | normal ignores the date.
inline const char* GetSeasonalLaunchScreenFile()
{
    char forced[32] = {};
    if (GetEnvironmentVariableA("GWO_LAUNCH_SEASON", forced, sizeof(forced)) > 0)
    {
        if (_stricmp(forced, "halloween") == 0)  return "GWOBS_Loading_Screen_Halloween.png";
        if (_stricmp(forced, "wintersday") == 0) return "GWOBS_Loading_Screen_Wintersday.png";
        if (_stricmp(forced, "normal") == 0)     return nullptr;
    }

    std::time_t t = std::time(nullptr);
    std::tm lt{};
    localtime_s(&lt, &t);
    if (IsHalloweenDate(lt.tm_mon + 1, lt.tm_mday))
        return "GWOBS_Loading_Screen_Halloween.png";
    if (IsWintersdayDate(lt.tm_mon + 1, lt.tm_mday))
        return "GWOBS_Loading_Screen_Wintersday.png";
    return nullptr;
}

// Walks up from the exe folder (dev builds run from x64/Release) looking for
// Textures/Launch_screen/<file>. Returns "" if no level has it.
inline std::string FindLaunchScreenFile(const char* file)
{
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return "";
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 6; i++)
    {
        auto p = dir / "Textures" / "Launch_screen" / file;
        if (std::filesystem::exists(p))
            return p.string();
        if (!dir.has_parent_path() || dir == dir.parent_path())
            break;
        dir = dir.parent_path();
    }
    return "";
}

// The seasonal image is searched at every level before the normal one, so a
// stale Textures copy beside the exe cannot hide it.
inline std::string GetLaunchScreenPath()
{
    if (const char* seasonal = GetSeasonalLaunchScreenFile())
    {
        std::string p = FindLaunchScreenFile(seasonal);
        if (!p.empty())
            return p;
    }
    return FindLaunchScreenFile("GWOBS_Loading_Screen_1.png");
}

// Draws the splash over the whole window without stretching it: scaled to
// cover, the overflow trimmed evenly from both sides (via UVs).
inline void DrawLaunchScreenCover(ImDrawList* dl, ImTextureID tex, ImVec2 display)
{
    float imgW = display.x, imgH = display.y;
    auto* srv = static_cast<ID3D11ShaderResourceView*>(tex);
    Microsoft::WRL::ComPtr<ID3D11Resource> res;
    srv->GetResource(res.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex2d;
    res.As(&tex2d);
    if (tex2d)
    {
        D3D11_TEXTURE2D_DESC desc;
        tex2d->GetDesc(&desc);
        imgW = static_cast<float>(desc.Width);
        imgH = static_cast<float>(desc.Height);
    }

    ImVec2 uv0(0, 0), uv1(1, 1);
    const float imgAspect = imgW / imgH;
    const float winAspect = display.x / display.y;
    if (imgAspect > winAspect)
    {
        const float keep = winAspect / imgAspect; // fraction of width shown
        uv0.x = (1.f - keep) * 0.5f;
        uv1.x = 1.f - uv0.x;
    }
    else if (imgAspect < winAspect)
    {
        const float keep = imgAspect / winAspect; // fraction of height shown
        uv0.y = (1.f - keep) * 0.5f;
        uv1.y = 1.f - uv0.y;
    }
    dl->AddImage(tex, ImVec2(0, 0), display, uv0, uv1);
}
