#include "pch.h"
#include "CapeBannerExport.h"
#include "GuildBannerAssetProvider.h"
#include "GuildBannerComposer.h"
#include "MatchLink.h"
#include "ReplayLibrary.h"
#include "DirectXTex/DirectXTex.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <json.hpp>

namespace
{
    // stdout of a GUI-subsystem process: the handle the caller gave it (a pipe from the script),
    // written directly because the C runtime's stdout is not attached in a windowed program.
    void Say(const std::string& text)
    {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!out || out == INVALID_HANDLE_VALUE) return;
        const std::string line = text + "\n";
        DWORD written = 0;
        WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }

    // Textures/CapeAssets next to the exe or up to five folders above it (ReplayWindow::InitCapeCache).
    std::filesystem::path FindCapeAssets()
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        auto dir = std::filesystem::path(exePath).parent_path();
        for (int i = 0; i < 6; i++)
        {
            const auto candidate = dir / "Textures" / "CapeAssets";
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec))
                return candidate;
            if (!dir.has_parent_path() || dir == dir.parent_path())
                break;
            dir = dir.parent_path();
        }
        return {};
    }
}

int RunCapeBannerExport(const std::wstring& inJson, const std::wstring& outDir)
{
    const std::filesystem::path assetRoot = FindCapeAssets();
    if (assetRoot.empty())
    {
        Say("cape-banners: Textures/CapeAssets not found next to the exe");
        return 1;
    }

    nlohmann::json list;
    try
    {
        std::ifstream in{std::filesystem::path(inJson)};
        if (!in) { Say("cape-banners: cannot read the input file"); return 1; }
        list = nlohmann::json::parse(in);
    }
    catch (const std::exception& e)
    {
        Say(std::string("cape-banners: the input is not JSON - ") + e.what());
        return 1;
    }
    if (!list.is_array())
    {
        Say("cape-banners: the input must be a JSON array of capes");
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    // WIC writes the PNGs.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    GuildBannerAssetProvider assets;
    assets.Init(assetRoot);

    int written = 0, existing = 0, failed = 0, invalid = 0;
    std::set<std::string> seen;
    for (const auto& item : list)
    {
        if (!item.is_object()) { invalid++; continue; }
        CapeData cape;
        auto field = [&](const char* name, int& out)
        {
            const auto it = item.find(name);
            if (it != item.end() && it->is_number_integer()) out = it->get<int>();
        };
        field("bg_color", cape.bg_color);
        field("detail_color", cape.detail_color);
        field("emblem_color", cape.emblem_color);
        field("shape", cape.shape);
        field("detail", cape.detail);
        field("emblem", cape.emblem);
        field("trim", cape.trim);

        const std::string id = MatchLinks::CapeId(cape);
        if (!seen.insert(id).second) continue;
        const std::filesystem::path out = std::filesystem::path(outDir) / (id + ".png");
        if (std::filesystem::exists(out, ec)) { existing++; continue; }

        std::vector<uint8_t> rgba = GuildBannerComposer::ComposeBanner(cape, assets);
        const size_t expect = static_cast<size_t>(GuildBannerComposer::kWidth) * GuildBannerComposer::kHeight * 4;
        if (rgba.size() != expect) { failed++; continue; }

        DirectX::Image img{};
        img.width = GuildBannerComposer::kWidth;
        img.height = GuildBannerComposer::kHeight;
        img.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        img.rowPitch = static_cast<size_t>(GuildBannerComposer::kWidth) * 4;
        img.slicePitch = expect;
        img.pixels = rgba.data();
        if (SUCCEEDED(DirectX::SaveToWICFile(img, DirectX::WIC_FLAGS_NONE,
                                             DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), out.c_str())))
            written++;
        else
            failed++;
    }

    if (SUCCEEDED(co)) CoUninitialize();

    Say("cape-banners: " + std::to_string(written) + " written, " + std::to_string(existing) + " already there, " +
        std::to_string(failed) + " failed, " + std::to_string(invalid) + " invalid entr" + (invalid == 1 ? "y" : "ies") +
        " (" + std::to_string(seen.size()) + " distinct cape(s))");
    return failed == 0 && invalid == 0 ? 0 : 1;
}
