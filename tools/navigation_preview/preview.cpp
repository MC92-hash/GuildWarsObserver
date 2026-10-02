// Offscreen visual/interaction check of the production navigation, using D3D WARP.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cassert>
#include <iostream>
#include <vector>
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "ui/app_navigation.h"
using Microsoft::WRL::ComPtr;
static ComPtr<ID3D11ShaderResourceView> art, logo;
static ImFont* serif;
static TextureCache cache;
TextureCache& GetTextureCache() { return cache; }
ImTextureID TextureCache::GetTexture(const std::string& path) { return path.find("gw_observer_logo") != std::string::npos ? logo.Get() : art.Get(); }
namespace ui {
void EnsureTextureBasePath() {}
const std::string& TextureBasePath() { static std::string root = "."; return root; }
ImFont* DisplayFont(float, int) { return serif; }
}
static void Check(HRESULT hr) { if (FAILED(hr)) throw hr; }
int main()
{
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ComPtr<IWICImagingFactory> wic;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context));
    D3D11_TEXTURE2D_DESC td = {};
    auto loadArt = [&](const wchar_t* path) {
    ComPtr<IWICBitmapDecoder> decoder;
    Check(wic->CreateDecoderFromFilename(path, nullptr,
        GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder));
    ComPtr<IWICBitmapFrameDecode> frame;
    Check(decoder->GetFrame(0, &frame));
    ComPtr<IWICFormatConverter> converter;
    Check(wic->CreateFormatConverter(&converter));
    Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    UINT aw, ah; converter->GetSize(&aw, &ah);
    std::vector<unsigned char> pixels(aw * ah * 4);
    Check(converter->CopyPixels(nullptr, aw * 4, UINT(pixels.size()), pixels.data()));
    td.Width = aw; td.Height = ah; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = { pixels.data(), aw * 4, 0 };
    ComPtr<ID3D11Texture2D> atlas;
    Check(device->CreateTexture2D(&td, &data, &atlas));
    ComPtr<ID3D11ShaderResourceView> result;
    Check(device->CreateShaderResourceView(atlas.Get(), nullptr, &result));
    return result;
    };
    art = loadArt(L"Textures/Toolbar/navigation_d1_atlas.png");
    logo = loadArt(L"Textures/Toolbar/gw_observer_logo.png");
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontFromFileTTF("Textures/Fonts/Inter-Regular.otf", 15.f);
    serif = io.Fonts->AddFontFromFileTTF("Textures/Fonts/friz-quadrata-std-medium-5870338ec7ef8.otf", 28.f);
    assert(serif);
    ImGui_ImplDX11_Init(device.Get(), context.Get());
    ImGui::StyleColorsDark();
    auto draw = [&](int width, AppTab selected, float mouseX, bool down) {
        io.DisplaySize = ImVec2(float(width), 150.f); io.DeltaTime = 1.f / 60;
        io.AddMousePosEvent(mouseX, 30); io.AddMouseButtonEvent(0, down);
        ImGui_ImplDX11_NewFrame(); ImGui::NewFrame();
        AppTab clicked = draw_app_ribbon(selected);
        bool open = app_navigation::BeginMenus();
        if (open) {
            if (ImGui::BeginMenu("File")) { ImGui::MenuItem("Settings..."); ImGui::EndMenu(); }
            if (GuiGlobalConstants::IsDeveloperMode() && ImGui::BeginMenu("Debug")) { ImGui::MenuItem("Test"); ImGui::EndMenu(); }
            if (ImGui::BeginMenu("Help")) { ImGui::MenuItem("Credits"); ImGui::EndMenu(); }
        }
        app_navigation::EndMenus(open);
        ImGui::Render();
        return clicked;
    };
    auto save = [&](int width, const wchar_t* name) {
        td.Width = width; td.Height = 150; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        Check(device->CreateTexture2D(&td, nullptr, &target));
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        auto* view = rtv.Get(); context->OMSetRenderTargets(1, &view, nullptr);
        const float bg[] = {.065f, .065f, .065f, 1}; context->ClearRenderTargetView(view, bg);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging; Check(device->CreateTexture2D(&td, nullptr, &staging));
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped; Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        ComPtr<IWICStream> stream; Check(wic->CreateStream(&stream));
        Check(stream->InitializeFromFilename(name, GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder; Check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
        Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> out; Check(encoder->CreateNewFrame(&out, nullptr));
        Check(out->Initialize(nullptr)); Check(out->SetSize(width, 150));
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA; Check(out->SetPixelFormat(&format));
        std::vector<BYTE> bgra(width * 150 * 4);
        for (int y = 0; y < 150; ++y) for (int x = 0; x < width; ++x) {
            const BYTE* src = (BYTE*)mapped.pData + y * mapped.RowPitch + x * 4;
            BYTE* dst = bgra.data() + (y * width + x) * 4;
            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
        }
        Check(out->WritePixels(150, width * 4, UINT(bgra.size()), bgra.data()));
        Check(out->Commit()); Check(encoder->Commit()); context->Unmap(staging.Get(), 0);
        td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0;
    };
    for (int width : {1536, 800, 320}) {
        for (int i = 0; i < 3; ++i) {
            draw(width, AppTab(i), -100, false); draw(width, AppTab(i), -100, false);
            std::wstring name = L"tools/navigation_preview/" + std::to_wstring(width) + L"_" + std::to_wstring(i) + L".png";
            save(width, name.c_str());
        }
        draw(width, AppTab::Library, -100, false);
        const auto layout = app_navigation::GetLayout();
        for (int i = 0; i < 3; ++i) {
            const float x = layout.navX + (i + .5f) * layout.tabWidth;
            draw(width, AppTab::Library, x, false);
            draw(width, AppTab::Library, x, true);
            assert(draw(width, AppTab::Library, x, false) == AppTab(i));
        }
    }
    draw(1536, AppTab::Library, -100, false);
    const auto layout = app_navigation::GetLayout();
    const float scoutX = layout.navX + 1.5f * layout.tabWidth;
    draw(1536, AppTab::Library, scoutX, false);
    save(1536, L"tools/navigation_preview/hover.png");
    draw(1536, AppTab::Library, scoutX, true);
    save(1536, L"tools/navigation_preview/pressed.png");
    assert(draw(1536, AppTab::Library, scoutX, false) == AppTab::Scout);
    draw(1536, AppTab::Scout, -100, false);
    // Arrow navigation enters keyboard focus mode after pointer interaction.
    io.AddKeyEvent(ImGuiKey_LeftArrow, true); draw(1536, AppTab::Scout, -100, false);
    io.AddKeyEvent(ImGuiKey_LeftArrow, false); draw(1536, AppTab::Scout, -100, false);
    io.AddKeyEvent(ImGuiKey_Space, true);
    const AppTab keyDown = draw(1536, AppTab::Scout, -100, false);
    io.AddKeyEvent(ImGuiKey_Space, false);
    const AppTab keyUp = draw(1536, AppTab::Scout, -100, false);
    assert(keyDown == AppTab::Library || keyUp == AppTab::Library);
    save(1536, L"tools/navigation_preview/keyboard.png");
    // Verify that integrated menus still open and accept pointer input.
    const float fileX = layout.menuX + 15;
    draw(1536, AppTab::Library, fileX, false);
    draw(1536, AppTab::Library, fileX, true);
    draw(1536, AppTab::Library, fileX, false);
    assert(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    save(1536, L"tools/navigation_preview/file_menu.png");
    io.AddKeyEvent(ImGuiKey_Escape, true); draw(1536, AppTab::Library, -100, false);
    io.AddKeyEvent(ImGuiKey_Escape, false); draw(1536, AppTab::Library, -100, false);
    GuiGlobalConstants::saved_font_size = 24.f; io.FontGlobalScale = 1.6f;
    for (int width : {1536, 800, 320}) {
        draw(width, AppTab::Wardrobe, -100, false); draw(width, AppTab::Wardrobe, -100, false);
        std::wstring name = L"tools/navigation_preview/large_font_" + std::to_wstring(width) + L".png";
        save(width, name.c_str());
    }
    std::cout << "PASS: nine mouse activation checks, keyboard activation, File menu popup, normal and large-font renders.\n";
    ImGui_ImplDX11_Shutdown(); ImGui::DestroyContext();
}
