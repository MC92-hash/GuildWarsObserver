#include "pch.h"
#include "ReplayWindow.h"
#include "AgentSnapshotParser.h"
#include "StoCParser.h"
#include "SkillDatabase.h"
#include "DXMathHelpers.h"
#include "FontConfig.h"
#include "GuiGlobalConstants.h"
#include "TextureCache.h"
#include "CursorSystem.h"

#define NANOSVG_IMPLEMENTATION
#include "../ThirdParty/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../ThirdParty/nanosvg/nanosvgrast.h"
#include <d3dcompiler.h>
#include <fstream>
#include <json.hpp>
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

static void SaveMapTransform(int mapId, const MapTransform& t);
static MapTransform LoadMapTransform(int mapId, bool* found = nullptr);

// ---------------------------------------------------------------------------
// Hotkey persistence (singleton, JSON)
// ---------------------------------------------------------------------------

static std::filesystem::path GetHotkeysFilePath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    auto settingsDir = dir / "settings";
    if (!std::filesystem::exists(settingsDir))
        std::filesystem::create_directories(settingsDir);
    return settingsDir / "hotkeys.json";
}

ReplayHotkeys& ReplayHotkeys::Get()
{
    static ReplayHotkeys instance;
    static bool loaded = false;
    if (!loaded) { instance.Load(); loaded = true; }
    return instance;
}

void ReplayHotkeys::Save() const
{
    auto path = GetHotkeysFilePath();
    std::ofstream f(path);
    if (!f.is_open()) return;
    f << "{\n"
      << "  \"rewind5s\": "  << rewind5s  << ",\n"
      << "  \"forward5s\": " << forward5s << ",\n"
      << "  \"playPause\": " << playPause << "\n"
      << "}\n";
}

void ReplayHotkeys::Load()
{
    auto path = GetHotkeysFilePath();
    std::ifstream f(path);
    if (!f.is_open()) return;
    try {
        nlohmann::json j;
        f >> j;
        if (j.contains("rewind5s"))  rewind5s  = j["rewind5s"].get<int>();
        if (j.contains("forward5s")) forward5s = j["forward5s"].get<int>();
        if (j.contains("playPause")) playPause = j["playPause"].get<int>();
    } catch (...) {}
}

// ---------------------------------------------------------------------------

bool ReplayWindow::s_classRegistered = false;

// ---------------------------------------------------------------------------
// Inline HLSL for the 2D loading overlay
// ---------------------------------------------------------------------------

static const char kOverlayHLSL[] = R"(
struct VS_IN  { float2 pos : POSITION; float4 col : COLOR; };
struct VS_OUT { float4 pos : SV_Position; float4 col : COLOR; };

VS_OUT VSMain(VS_IN i) {
    VS_OUT o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.col = i.col;
    return o;
}

float4 PSMain(VS_OUT i) : SV_Target { return i.col; }
)";

// ---------------------------------------------------------------------------
// Window class registration
// ---------------------------------------------------------------------------

bool ReplayWindow::RegisterWindowClass(HINSTANCE hInstance)
{
    if (s_classRegistered) return true;

    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(WNDCLASSEXW);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = ReplayWindow::WndProc;
    wcex.hInstance     = hInstance;
    wcex.hIcon         = LoadIconW(hInstance, L"IDI_ICON");
    wcex.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wcex.lpszClassName = kWindowClassName;
    wcex.hIconSm       = LoadIconW(hInstance, L"IDI_ICON");

    if (!RegisterClassExW(&wcex))
        return false;

    s_classRegistered = true;
    return true;
}

// ---------------------------------------------------------------------------
// Build the window title
// ---------------------------------------------------------------------------

static std::wstring BuildWindowTitle(const MatchMeta& match)
{
    auto getGuildLabel = [&](const std::string& partyId) -> std::pair<std::string, std::string>
    {
        auto pit = match.parties.find(partyId);
        if (pit == match.parties.end()) return { "Unknown", "?" };

        std::map<int, int> guildCounts;
        for (const auto& p : pit->second.players)
            if (p.guild_id > 0) guildCounts[p.guild_id]++;

        int bestGuildId = 0, bestCount = 0;
        for (const auto& [gid, cnt] : guildCounts)
            if (cnt > bestCount) { bestGuildId = gid; bestCount = cnt; }

        if (bestGuildId == 0) return { "Unknown", "?" };

        auto git = match.guilds.find(std::to_string(bestGuildId));
        if (git != match.guilds.end())
            return { git->second.name, git->second.tag };

        return { "Guild #" + std::to_string(bestGuildId), "?" };
    };

    auto [name1, tag1] = getGuildLabel("1");
    auto [name2, tag2] = getGuildLabel("2");

    std::string title = std::format("Guild Wars Observer - {:04d}/{:02d}/{:02d} {} [{}] vs {} [{}]",
        match.year, match.month, match.day,
        name1, tag1, name2, tag2);

    return std::wstring(title.begin(), title.end());
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

ReplayWindow* ReplayWindow::Create(HINSTANCE hInstance, const MatchMeta& match,
                                    DATManager* sharedDatManager,
                                    const std::unordered_map<int, std::vector<int>>& hashIndex)
{
    auto* rw = new ReplayWindow();
    rw->m_matchMeta   = match;
    rw->m_datManager   = sharedDatManager;
    rw->m_hashIndex    = &hashIndex;

    rw->m_replayCtx.mapId       = match.map_id;
    rw->m_replayCtx.datMapId    = GetDatMapId(match.map_id);
    rw->m_replayCtx.matchFolderPath = match.folder_path;

    if (!RegisterWindowClass(hInstance))
    {
        delete rw;
        return nullptr;
    }

    std::wstring title = BuildWindowTitle(match);
    if (!rw->InitWindow(hInstance, L"Loading... " + title))
    {
        delete rw;
        return nullptr;
    }

    if (!rw->InitGraphics())
    {
        DestroyWindow(rw->m_hwnd);
        delete rw;
        return nullptr;
    }

    if (!rw->InitLoadingOverlay())
    {
        DestroyWindow(rw->m_hwnd);
        delete rw;
        return nullptr;
    }

    rw->m_alive = true;
    rw->m_loadingPhase = LoadingPhase::Validate;

    ShowWindow(rw->m_hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(rw->m_hwnd);
    return rw;
}

// ---------------------------------------------------------------------------

ReplayWindow::~ReplayWindow()
{
    ShutdownImGui();
    if (m_hwnd)
        SetWindowLongPtr(m_hwnd, GWLP_USERDATA, 0);
}

// ---------------------------------------------------------------------------
// Window creation
// ---------------------------------------------------------------------------

bool ReplayWindow::InitWindow(HINSTANCE hInstance, const std::wstring& title)
{
    int w = 1280, h = 720;
    RECT rc = { 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(0, kWindowClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);

    if (!m_hwnd) return false;

    SetWindowLongPtr(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

// ---------------------------------------------------------------------------
// Graphics init
// ---------------------------------------------------------------------------

bool ReplayWindow::InitGraphics()
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    int width  = rc.right  - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0) width = 1280;
    if (height <= 0) height = 720;

    m_deviceResources = std::make_unique<DX::DeviceResources>(DXGI_FORMAT_R8G8B8A8_UNORM);
    m_deviceResources->SetWindow(m_hwnd, width, height);
    m_deviceResources->CreateDeviceResources();
    m_deviceResources->CreateWindowSizeDependentResources();

    m_inputManager = std::make_unique<InputManager>(m_hwnd);

    m_mapRenderer = std::make_unique<MapRenderer>(
        m_deviceResources->GetD3DDevice(),
        m_deviceResources->GetD3DDeviceContext(),
        m_inputManager.get());
    m_mapRenderer->Initialize(static_cast<float>(width), static_cast<float>(height));

    m_deviceResources->RegisterDeviceNotify(this);
    return true;
}

// ---------------------------------------------------------------------------
// Loading overlay GPU resources (simple 2D colored quad shader)
// ---------------------------------------------------------------------------

bool ReplayWindow::InitLoadingOverlay()
{
    auto* device = m_deviceResources->GetD3DDevice();
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif

    // Compile vertex shader
    ComPtr<ID3DBlob> vsBlob, errBlob;
    HRESULT hr = D3DCompile(kOverlayHLSL, sizeof(kOverlayHLSL), nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", flags, 0, vsBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
        m_overlayVS.GetAddressOf());
    if (FAILED(hr)) return false;

    // Input layout
    D3D11_INPUT_ELEMENT_DESC ilDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device->CreateInputLayout(ilDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        m_overlayIL.GetAddressOf());
    if (FAILED(hr)) return false;

    // Compile pixel shader
    ComPtr<ID3DBlob> psBlob;
    hr = D3DCompile(kOverlayHLSL, sizeof(kOverlayHLSL), nullptr, nullptr, nullptr,
        "PSMain", "ps_5_0", flags, 0, psBlob.GetAddressOf(), errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
        m_overlayPS.GetAddressOf());
    if (FAILED(hr)) return false;

    // Dynamic vertex buffer (enough for bar background + bar fill = 12 vertices)
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(OverlayVertex) * 12;
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&vbDesc, nullptr, m_overlayVB.GetAddressOf());
    if (FAILED(hr)) return false;

    // Depth stencil state: no depth test
    D3D11_DEPTH_STENCIL_DESC dssDesc = {};
    dssDesc.DepthEnable = FALSE;
    dssDesc.StencilEnable = FALSE;
    hr = device->CreateDepthStencilState(&dssDesc, m_overlayDSS.GetAddressOf());
    if (FAILED(hr)) return false;

    // Rasterizer state: no culling
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rsDesc, m_overlayRS.GetAddressOf());
    if (FAILED(hr)) return false;

    // Blend state: opaque
    D3D11_BLEND_DESC bsDesc = {};
    bsDesc.RenderTarget[0].BlendEnable = FALSE;
    bsDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&bsDesc, m_overlayBS.GetAddressOf());
    if (FAILED(hr)) return false;

    return true;
}

// ---------------------------------------------------------------------------
// ImGui init / shutdown (private context for this window)
// ---------------------------------------------------------------------------

static std::string GetReplayFontBasePath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "Fonts"))
            return (dir / "Textures" / "Fonts").string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return "";
}

void ReplayWindow::InitImGui()
{
    if (m_imguiInitialized) return;

    ImGuiContext* prevCtx = ImGui::GetCurrentContext();

    m_imguiContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_imguiContext);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;

    // Load the same font as the main UI
    float fontSize = GuiGlobalConstants::saved_font_size;
    int fontIdx = GuiGlobalConstants::saved_font_index;
    if (fontIdx < 0 || fontIdx >= g_fontTableCount) fontIdx = 2;
    const FontEntry& fe = g_fontTable[fontIdx];
    bool fontLoaded = false;

    if (fe.fileName)
    {
        std::string fullPath;
        if (fe.isSystemFont)
            fullPath = std::string("C:\\Windows\\Fonts\\") + fe.fileName;
        else
        {
            std::string base = GetReplayFontBasePath();
            if (!base.empty()) fullPath = base + "\\" + fe.fileName;
        }
        if (!fullPath.empty() && std::filesystem::exists(fullPath))
        {
            io.Fonts->AddFontFromFileTTF(fullPath.c_str(), fontSize);
            fontLoaded = true;
        }
    }
    if (!fontLoaded)
        io.Fonts->AddFontDefault();

    io.Fonts->Build();

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_deviceResources->GetD3DDevice(),
                        m_deviceResources->GetD3DDeviceContext());

    m_imguiInitialized = true;

    ImGui::SetCurrentContext(prevCtx);
}

void ReplayWindow::ShutdownImGui()
{
    if (!m_imguiInitialized) return;

    ImGuiContext* prevCtx = ImGui::GetCurrentContext();
    bool needRestore = (prevCtx != m_imguiContext);

    ImGui::SetCurrentContext(m_imguiContext);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(m_imguiContext);
    m_imguiContext = nullptr;
    m_imguiInitialized = false;

    if (needRestore)
        ImGui::SetCurrentContext(prevCtx);
    else
        ImGui::SetCurrentContext(nullptr);
}

// ---------------------------------------------------------------------------
// Loading phase: Validate
// ---------------------------------------------------------------------------

void ReplayWindow::StepValidate()
{
    if (!m_datManager || !m_hashIndex)
    {
        m_errorMsg = "gw.dat is missing or unreadable.";
        m_loadingPhase = LoadingPhase::Error;
        return;
    }

    if (m_datManager->m_initialization_state != InitializationState::Completed)
    {
        m_errorMsg = "gw.dat is still loading. Please wait and try again.";
        m_loadingPhase = LoadingPhase::Error;
        return;
    }

    uint32_t datFileHash = m_replayCtx.datMapId;
    if (datFileHash == 0)
    {
        m_errorMsg = std::format("Unknown map_id {}. No dat mapping available.", m_replayCtx.mapId);
        m_loadingPhase = LoadingPhase::Error;
        return;
    }

    auto it = m_hashIndex->find(static_cast<int>(datFileHash));
    if (it == m_hashIndex->end() || it->second.empty())
    {
        m_errorMsg = std::format("Unable to load map for map_id {} (dat ID 0x{:X}).",
                                 m_replayCtx.mapId, datFileHash);
        m_loadingPhase = LoadingPhase::Error;
        return;
    }

    // Launch async agent snapshot parsing in parallel with map loading
    if (!m_replayCtx.agentParseProgress)
    {
        m_replayCtx.agentParseProgress = std::make_shared<AgentParseProgress>();
        LaunchAgentSnapshotParsing(m_replayCtx.matchFolderPath,
                                   m_replayCtx.agentParseProgress);
    }

    // Launch async StoC event parsing in parallel
    if (!m_replayCtx.stocParseProgress)
    {
        m_replayCtx.stocParseProgress = std::make_shared<StoCParseProgress>();
        LaunchStoCParsing(m_replayCtx.matchFolderPath, m_replayCtx.stocParseProgress);
    }

    m_loadingPhase = LoadingPhase::Init;
}

// ---------------------------------------------------------------------------
// Loading phase: Init (parse map, terrain, env, sky, water, fog)
// ---------------------------------------------------------------------------

void ReplayWindow::StepLoadInit()
{
    uint32_t datFileHash = m_replayCtx.datMapId;
    auto it = m_hashIndex->find(static_cast<int>(datFileHash));
    int mftIndex = it->second.at(0);

    m_mapFile = m_datManager->parse_ffna_map_file(mftIndex);

    if (m_mapFile.terrain_chunk.terrain_heightmap.empty() ||
        m_mapFile.terrain_chunk.terrain_heightmap.size() !=
        m_mapFile.terrain_chunk.terrain_x_dims * m_mapFile.terrain_chunk.terrain_y_dims)
    {
        m_errorMsg = std::format("Unable to load map for map_id {} (dat ID 0x{:X}). Terrain data missing.",
                                 m_replayCtx.mapId, datFileHash);
        m_loadingPhase = LoadingPhase::Error;
        return;
    }

    auto* map_renderer = m_mapRenderer.get();
    map_renderer->GetTextureManager()->Clear();
    map_renderer->ClearSceneForModeSwitch();
    map_renderer->SetShouldRenderShadowsForModels(true);

    // --- Environment setup (lighting, sky, fog, water) ---
    const auto& envChunk = m_mapFile.environment_info_chunk;
    const EnvSubChunk8* env8 = envChunk.env_sub_chunk8.empty() ? nullptr : &envChunk.env_sub_chunk8[0];

    PerSkyCB sky_cb = map_renderer->GetPerSkyCB();

    // Brightness/saturation
    {
        float brightness = 1.0f, saturation = 1.0f, bias_add = 0.0f;
        if (!envChunk.env_sub_chunk1.empty()) {
            size_t idx = (env8 && env8->sky_settings_index < envChunk.env_sub_chunk1.size())
                ? env8->sky_settings_index : 0u;
            const auto& sub1 = envChunk.env_sub_chunk1[idx];
            brightness = std::clamp(sub1.sky_brightness_maybe / 128.0f, 0.0f, 2.0f);
            saturation = std::clamp(sub1.sky_saturaion_maybe / 128.0f, 0.0f, 2.0f);
        }
        if (env8) {
            bias_add = (static_cast<int>(env8->sky_brightness_bias) - 128) / 128.0f;
            bias_add = std::clamp(bias_add * 0.15f, -0.25f, 0.25f);
        }
        sky_cb.color_params = XMFLOAT4(brightness, saturation, bias_add, 0.0f);
    }

    // Lighting
    if (!envChunk.env_sub_chunk3.empty()) {
        size_t idx = (env8 && env8->lighting_settings_index < envChunk.env_sub_chunk3.size())
            ? env8->lighting_settings_index : 0u;
        const auto& sub3 = envChunk.env_sub_chunk3[idx];
        float light_div = 2.0f;
        float ambient_intensity = sub3.ambient_intensity / 255.0f;
        float diffuse_intensity = sub3.sun_intensity / 255.0f;

        DirectionalLight dl = map_renderer->GetDirectionalLight();
        dl.ambient.x = sub3.ambient_red / (255.0f * light_div);
        dl.ambient.y = sub3.ambient_green / (255.0f * light_div);
        dl.ambient.z = sub3.ambient_blue / (255.0f * light_div);
        dl.diffuse.x = sub3.sun_red / (255.0f * light_div);
        dl.diffuse.y = sub3.sun_green / (255.0f * light_div);
        dl.diffuse.z = sub3.sun_blue / (255.0f * light_div);

        auto ahls = RGBAtoHSL(dl.ambient);
        auto dhls = RGBAtoHSL(dl.diffuse);
        ahls.z = std::max(ambient_intensity * 0.9f, 0.7f);
        dhls.z = std::max(diffuse_intensity * 0.9f, 0.5f);
        dl.ambient = HSLtoRGBA(ahls);
        dl.diffuse = HSLtoRGBA(dhls);
        map_renderer->SetDirectionalLight(dl);
    }

    // Sky texture settings
    uint16_t sky_bg_idx = 0xFFFF;
    uint16_t sky_clouds0_idx = 0xFFFF, sky_clouds1_idx = 0xFFFF;
    uint16_t sky_sun_idx = 0xFFFF;
    uint16_t water_color_idx = 0xFFFF, water_distort_idx = 0xFFFF;

    const uint16_t selSkyTexIdx   = env8 ? env8->sky_texture_settings_index : 0u;
    const uint16_t selWaterIdx    = env8 ? env8->water_settings_index : 0u;
    const uint16_t selWindIdx     = env8 ? env8->wind_settings_index : 0u;

    if (!envChunk.env_sub_chunk5.empty()) {
        size_t si = (selSkyTexIdx < envChunk.env_sub_chunk5.size()) ? selSkyTexIdx : 0u;
        const auto& sub5 = envChunk.env_sub_chunk5[si];
        sky_bg_idx      = sub5.sky_background_texture_index;
        sky_clouds0_idx = sub5.sky_clouds_texture_index0;
        sky_clouds1_idx = sub5.sky_clouds_texture_index1;
        sky_sun_idx     = sub5.sky_sun_texture_index;

        const float uv_scale = std::max(1.0f, std::round(sub5.unknown0 / 32.0f));
        const float kDenom = 16777216.0f;
        const float scroll_u = static_cast<float>(sub5.unknown1) / kDenom;
        const float scroll_v = static_cast<float>(sub5.unknown2) / kDenom;
        const float sun_scale = sub5.unknown3 / 255.0f;
        const float sun_disk_radius = (0.01f + sun_scale * 0.05f) * 3.0f;
        sky_cb.cloud0_params = XMFLOAT4(uv_scale, scroll_u, scroll_v, 1.0f);
        sky_cb.cloud1_params = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        sky_cb.sun_params.x = sun_disk_radius;
    }
    else if (!envChunk.env_sub_chunk5_other.empty()) {
        size_t si = (selSkyTexIdx < envChunk.env_sub_chunk5_other.size()) ? selSkyTexIdx : 0u;
        const auto& sub5 = envChunk.env_sub_chunk5_other[si];
        sky_bg_idx      = sub5.sky_background_texture_index;
        sky_clouds0_idx = sub5.sky_clouds_texture_index0;
        sky_clouds1_idx = sub5.sky_clouds_texture_index1;
        sky_sun_idx     = sub5.sky_sun_texture_index;

        uint8_t scale_byte = 0; int16_t s0 = 0, s1 = 0; uint8_t sun_byte = 0;
        std::memcpy(&scale_byte, &sub5.unknown[0], 1);
        std::memcpy(&s0, &sub5.unknown[1], 2);
        std::memcpy(&s1, &sub5.unknown[3], 2);
        std::memcpy(&sun_byte, &sub5.unknown[5], 1);
        const float uv_scale = std::max(1.0f, std::round(scale_byte / 32.0f));
        const float kDenom = 16777216.0f;
        sky_cb.cloud0_params = XMFLOAT4(uv_scale, s0 / kDenom, s1 / kDenom, 1.0f);
        sky_cb.cloud1_params = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        sky_cb.sun_params.x  = (0.01f + sun_byte / 255.0f * 0.05f) * 3.0f;
    }
    map_renderer->SetPerSkyCB(sky_cb);

    // Water settings
    if (!envChunk.env_sub_chunk6.empty()) {
        size_t wi = (selWaterIdx < envChunk.env_sub_chunk6.size()) ? selWaterIdx : 0u;
        water_color_idx   = envChunk.env_sub_chunk6[wi].water_color_texture_index;
        water_distort_idx = envChunk.env_sub_chunk6[wi].water_distortion_texture_index;
    }

    // Helper to load a texture from the dat by env filename index
    const auto& envFilenames = m_mapFile.environment_info_filenames_chunk;
    auto loadEnvTexture = [&](uint16_t filenameIndex) -> ID3D11ShaderResourceView*
    {
        if (filenameIndex >= envFilenames.filenames.size()) return nullptr;
        const auto& fn = envFilenames.filenames[filenameIndex];
        auto decoded = decode_filename(fn.filename.id0, fn.filename.id1);
        auto mit = m_hashIndex->find(decoded);
        if (mit == m_hashIndex->end()) return nullptr;
        int ti = mit->second.at(0);
        auto type = m_datManager->get_MFT()[ti].type;
        int texId = -1;
        if (type == DDS) {
            auto ddsData = m_datManager->parse_dds_file(ti);
            DatTexture dt;
            auto hr = map_renderer->GetTextureManager()->CreateTextureFromDDSInMemory(
                ddsData.data(), ddsData.size(), &texId, &dt.width, &dt.height, dt.rgba_data, decoded);
            if (SUCCEEDED(hr) && texId >= 0)
                return map_renderer->GetTextureManager()->GetTexture(texId);
        }
        else {
            DatTexture dt = m_datManager->parse_ffna_texture_file(ti);
            if (dt.width > 0 && dt.height > 0) {
                auto hr = map_renderer->GetTextureManager()->CreateTextureFromRGBA(
                    dt.width, dt.height, dt.rgba_data.data(), &texId, decoded);
                if (SUCCEEDED(hr) && texId >= 0)
                    return map_renderer->GetTextureManager()->GetTexture(texId);
            }
        }
        return nullptr;
    };

    // --- Sky mesh ---
    std::vector<uint16_t> skyIndices{ sky_bg_idx, sky_clouds0_idx, sky_clouds1_idx, sky_sun_idx };
    std::vector<ID3D11ShaderResourceView*> skyTextures(skyIndices.size(), nullptr);
    for (size_t i = 0; i < skyIndices.size(); i++)
        skyTextures[i] = loadEnvTexture(skyIndices[i]);

    if (!skyTextures.empty()) {
        int skyMeshId = map_renderer->GetMeshManager()->AddGwSkyCylinder(67723.75f / 2.0f, 33941.0f);
        map_renderer->SetSkyMeshId(skyMeshId);
        map_renderer->GetMeshManager()->SetMeshShouldCull(skyMeshId, false);
        map_renderer->SetMeshShouldRender(skyMeshId, false);

        const auto& mapBounds = m_mapFile.map_info_chunk.map_bounds;
        float cx = (mapBounds.map_min_x + mapBounds.map_max_x) / 2.0f;
        float cz = (mapBounds.map_min_z + mapBounds.map_max_z) / 2.0f;

        XMFLOAT4X4 skyWorld;
        XMStoreFloat4x4(&skyWorld, XMMatrixTranslation(cx, map_renderer->GetSkyHeight(), cz));
        PerObjectCB skyObj;
        skyObj.world = skyWorld;
        for (int i = 0; i < (int)skyTextures.size(); i++) {
            skyObj.texture_indices[i / 4][i % 4] = 0;
            skyObj.texture_types[i / 4][i % 4] = skyTextures[i] == nullptr ? 0xFF : 0;
        }
        skyObj.num_uv_texture_pairs = (uint32_t)skyTextures.size();
        map_renderer->GetMeshManager()->UpdateMeshPerObjectData(skyMeshId, skyObj);
        map_renderer->GetMeshManager()->SetTexturesForMesh(skyMeshId, skyTextures, 3);
    }

    // --- Water mesh ---
    std::vector<uint16_t> waterTexIndices{ water_color_idx, water_distort_idx };
    std::vector<ID3D11ShaderResourceView*> waterTextures(waterTexIndices.size(), nullptr);
    for (size_t i = 0; i < waterTexIndices.size(); i++)
        waterTextures[i] = loadEnvTexture(waterTexIndices[i]);

    if (!waterTextures.empty()) {
        int waterMeshId = map_renderer->GetMeshManager()->AddGwSkyCircle(70000, PixelShaderType::Water);
        map_renderer->SetWaterMeshId(waterMeshId);
        map_renderer->GetMeshManager()->SetMeshShouldCull(waterMeshId, false);
        map_renderer->SetMeshShouldRender(waterMeshId, false);

        const auto& mapBounds = m_mapFile.map_info_chunk.map_bounds;
        float cx = (mapBounds.map_min_x + mapBounds.map_max_x) / 2.0f;
        float cz = (mapBounds.map_min_z + mapBounds.map_max_z) / 2.0f;

        XMFLOAT4X4 waterWorld;
        XMStoreFloat4x4(&waterWorld, XMMatrixTranslation(cx, 0, cz));
        PerObjectCB waterObj;
        waterObj.world = waterWorld;
        for (int i = 0; i < (int)waterTextures.size(); i++) {
            waterObj.texture_indices[i / 4][i % 4] = 0;
            waterObj.texture_types[i / 4][i % 4] = waterTextures[i] == nullptr ? 0xFF : 0;
        }
        waterObj.num_uv_texture_pairs = (uint32_t)waterTextures.size();
        map_renderer->GetMeshManager()->UpdateMeshPerObjectData(waterMeshId, waterObj);
        map_renderer->GetMeshManager()->SetTexturesForMesh(waterMeshId, waterTextures, 0);
        map_renderer->GetMeshManager()->SetTexturesForMesh(
            waterMeshId, { map_renderer->GetWaterFresnelLUTSRV() }, 3);
    }

    // --- Terrain textures ---
    auto& terrainTexNames = m_mapFile.terrain_texture_filenames.array;
    std::vector<DatTexture> terrainDatTextures;
    for (size_t i = 0; i < terrainTexNames.size(); i++)
    {
        auto decoded = decode_filename(terrainTexNames[i].filename.id0, terrainTexNames[i].filename.id1);
        if (decoded == 0x25e09 || decoded == 0x00028615 || decoded == 0x46db6)
            continue;

        auto mit = m_hashIndex->find(decoded);
        if (mit != m_hashIndex->end()) {
            DatTexture dt = m_datManager->parse_ffna_texture_file(mit->second.at(0));
            if (dt.width > 0 && dt.height > 0)
                terrainDatTextures.push_back(dt);
        }
    }

    if (terrainDatTextures.empty())
    {
        m_errorMsg = std::format("Unable to load map for map_id {} (dat ID 0x{:X}). No terrain textures.",
                                 m_replayCtx.mapId, datFileHash);
        m_loadingPhase = LoadingPhase::Error;
        return;
    }

    std::vector<void*> rawPtrs;
    for (auto& dt : terrainDatTextures)
        rawPtrs.push_back(dt.rgba_data.data());

    const auto terrainTexId = map_renderer->GetTextureManager()->AddTextureArray(
        rawPtrs, terrainDatTextures[0].width, terrainDatTextures[0].height,
        DXGI_FORMAT_B8G8R8A8_UNORM, static_cast<int>(datFileHash), true);

    // --- Terrain mesh ---
    auto terrain = std::make_unique<Terrain>(
        m_mapFile.terrain_chunk.terrain_x_dims,
        m_mapFile.terrain_chunk.terrain_y_dims,
        m_mapFile.terrain_chunk.terrain_heightmap,
        m_mapFile.terrain_chunk.terrain_texture_indices_maybe,
        m_mapFile.terrain_chunk.terrain_shadow_map,
        m_mapFile.map_info_chunk.map_bounds);
    map_renderer->SetTerrain(terrain.get(), terrainTexId);

    // Water properties
    if (!envChunk.env_sub_chunk6.empty()) {
        size_t wi = (selWaterIdx < envChunk.env_sub_chunk6.size()) ? selWaterIdx : 0u;
        const EnvSubChunk7* wind = nullptr;
        if (!envChunk.env_sub_chunk7.empty()) {
            size_t wii = (selWindIdx < envChunk.env_sub_chunk7.size()) ? selWindIdx : 0u;
            wind = &envChunk.env_sub_chunk7[wii];
        }
        map_renderer->UpdateWaterProperties(envChunk.env_sub_chunk6[wi], wind);
    }

    // Cloud mesh
    if (!envFilenames.filenames.empty()) {
        auto* cloudTex = loadEnvTexture(0);
        if (cloudTex) {
            int cloudId = map_renderer->GetMeshManager()->AddGwSkyCircle(100000.0f);
            map_renderer->SetCloudsMeshId(cloudId);
            map_renderer->GetMeshManager()->SetMeshShouldCull(cloudId, true);
            map_renderer->SetMeshShouldRender(cloudId, false);

            float cx = (terrain->m_bounds.map_min_x + terrain->m_bounds.map_max_x) / 2.0f;
            float cz = (terrain->m_bounds.map_min_z + terrain->m_bounds.map_max_z) / 2.0f;

            XMFLOAT4X4 cWorld;
            XMStoreFloat4x4(&cWorld, XMMatrixTranslation(cx, terrain->m_bounds.map_max_y + 2400, cz));
            PerObjectCB cObj;
            cObj.world = cWorld;
            cObj.texture_indices[0][0] = 0;
            cObj.texture_types[0][0] = 0;
            cObj.num_uv_texture_pairs = 1;
            map_renderer->GetMeshManager()->UpdateMeshPerObjectData(cloudId, cObj);
            map_renderer->GetMeshManager()->SetTexturesForMesh(cloudId, { cloudTex }, 3);
        }
    }

    // Fog and clear color
    if (!envChunk.env_sub_chunk2.empty()) {
        size_t fi = (env8 && env8->fog_settings_index < envChunk.env_sub_chunk2.size())
            ? env8->fog_settings_index : 0u;
        const auto& sub2 = envChunk.env_sub_chunk2[fi];
        XMFLOAT4 clearColor{
            sub2.fog_red / 255.0f, sub2.fog_green / 255.0f, sub2.fog_blue / 255.0f, 1.0f
        };
        float fogStart = static_cast<float>(sub2.fog_distance_start);
        float fogEndRaw = static_cast<float>(sub2.fog_distance_end);
        float fogEnd = (fogEndRaw > fogStart + 1.0f) ? fogEndRaw : (fogStart + 1.0f);
        map_renderer->SetFogStart(fogStart);
        map_renderer->SetFogEnd(fogEnd);
        map_renderer->SetFogStartY(static_cast<float>(sub2.fog_z_start_maybe));
        map_renderer->SetFogEndY(static_cast<float>(sub2.fog_z_end_maybe));
        map_renderer->SetClearColor(clearColor);
    }
    map_renderer->SetSkyHeight(0);

    m_terrain = std::move(terrain);

    // Prepare for prop loading phases
    m_totalPropFilenames = static_cast<int>(m_mapFile.prop_filenames_chunk.array.size()
        + m_mapFile.more_filnames_chunk.array.size());
    m_totalPropInstances = static_cast<int>(m_mapFile.props_info_chunk.prop_array.props_info.size());
    m_propModelLoadIndex = 0;
    m_propPlaceIndex = 0;
    m_propModelFiles.clear();
    m_propModelFiles.reserve(m_totalPropFilenames);

    m_loadProgress = 0.05f;
    m_loadingPhase = LoadingPhase::PropModels;
}

// ---------------------------------------------------------------------------
// Loading phase: parse prop model files from DAT (batched)
// ---------------------------------------------------------------------------

void ReplayWindow::StepLoadPropModels()
{
    const auto& propFN = m_mapFile.prop_filenames_chunk.array;
    const auto& moreFN = m_mapFile.more_filnames_chunk.array;
    int total = m_totalPropFilenames;
    int end = std::min(m_propModelLoadIndex + kPropModelBatchSize, total);

    for (int i = m_propModelLoadIndex; i < end; i++)
    {
        const auto& fn = (i < (int)propFN.size())
            ? propFN[i]
            : moreFN[i - (int)propFN.size()];

        auto decoded = decode_filename(fn.filename.id0, fn.filename.id1);
        auto mit = m_hashIndex->find(decoded);
        if (mit != m_hashIndex->end()) {
            auto type = m_datManager->get_MFT()[mit->second.at(0)].type;
            if (type == FFNA_Type2)
                m_propModelFiles.emplace_back(m_datManager->parse_ffna_model_file(mit->second.at(0)));
        }
    }

    m_propModelLoadIndex = end;

    if (total > 0)
        m_loadProgress = 0.05f + 0.25f * (static_cast<float>(m_propModelLoadIndex) / total);

    if (m_propModelLoadIndex >= total)
    {
        m_loadProgress = 0.30f;
        m_loadingPhase = LoadingPhase::PlaceProps;
    }
}

// ---------------------------------------------------------------------------
// Loading phase: place prop instances (batched)
// ---------------------------------------------------------------------------

void ReplayWindow::StepPlaceProps()
{
    auto* map_renderer = m_mapRenderer.get();
    const auto& propsInfo = m_mapFile.props_info_chunk.prop_array.props_info;
    int total = m_totalPropInstances;
    int end = std::min(m_propPlaceIndex + kPropPlaceBatchSize, total);

    for (int i = m_propPlaceIndex; i < end; i++)
    {
        PropInfo prop_info = propsInfo[i];

        if (prop_info.filename_index >= m_propModelFiles.size()) continue;
        auto* modelFilePtr = std::get_if<FFNA_ModelFile>(&m_propModelFiles[prop_info.filename_index]);
        if (!modelFilePtr || !modelFilePtr->parsed_correctly) continue;

        const auto& geom = modelFilePtr->geometry_chunk;
        std::vector<Mesh> propMeshes;
        for (size_t j = 0; j < geom.models.size(); j++)
        {
            AMAT_file amat;
            if (!modelFilePtr->AMAT_filenames_chunk.texture_filenames.empty()) {
                int subIdx = geom.models[j].unknown;
                if (!geom.tex_and_vertex_shader_struct.uts0.empty())
                    subIdx %= (int)geom.tex_and_vertex_shader_struct.uts0.size();
                const auto& uts1 = geom.uts1[subIdx % geom.uts1.size()];
                int amatIdx = ((uts1.some_flags0 >> 8) & 0xFF) % (int)modelFilePtr->AMAT_filenames_chunk.texture_filenames.size();
                auto amatFn = modelFilePtr->AMAT_filenames_chunk.texture_filenames[amatIdx];
                auto amatHash = decode_filename(amatFn.id0, amatFn.id1);
                auto aIt = m_hashIndex->find(amatHash);
                if (aIt != m_hashIndex->end())
                    amat = m_datManager->parse_amat_file(aIt->second.at(0));
            }
            Mesh mesh = modelFilePtr->GetMesh((int)j, amat);
            if (mesh.indices.size() % 3 == 0)
                propMeshes.push_back(mesh);
        }
        if (propMeshes.empty()) continue;

        // Load textures for this model
        std::vector<int> textureIds;
        if (modelFilePtr->textures_parsed_correctly) {
            for (size_t t = 0; t < modelFilePtr->texture_filenames_chunk.texture_filenames.size(); t++) {
                auto tf = modelFilePtr->texture_filenames_chunk.texture_filenames[t];
                auto decoded = decode_filename(tf.id0, tf.id1);
                int texId = map_renderer->GetTextureManager()->GetTextureIdByHash(decoded);
                if (texId >= 0) { textureIds.push_back(texId); continue; }
                auto mit = m_hashIndex->find(decoded);
                if (mit != m_hashIndex->end()) {
                    DatTexture dt = m_datManager->parse_ffna_texture_file(mit->second.at(0));
                    if (dt.width > 0 && dt.height > 0) {
                        map_renderer->GetTextureManager()->CreateTextureFromRGBA(
                            dt.width, dt.height, dt.rgba_data.data(), &texId, decoded);
                    }
                    textureIds.push_back(texId);
                }
            }
        }

        // Remap per-mesh texture indices
        std::vector<std::vector<int>> perMeshTexIds(propMeshes.size());
        for (size_t k = 0; k < propMeshes.size(); k++) {
            std::vector<uint8_t> remappedIndices;
            for (size_t ti = 0; ti < propMeshes[k].tex_indices.size(); ti++) {
                int idx = std::min((int)propMeshes[k].tex_indices[ti], (int)textureIds.size() - 1);
                if (idx >= 0 && idx < (int)textureIds.size()) {
                    perMeshTexIds[k].push_back(textureIds[idx]);
                    remappedIndices.push_back((uint8_t)ti);
                }
            }
            propMeshes[k].tex_indices = remappedIndices;
        }

        // Build per-object constant buffers with correct transform
        std::vector<PerObjectCB> perObjectCBs(propMeshes.size());
        for (size_t j = 0; j < propMeshes.size(); j++) {
            XMFLOAT3 translation(prop_info.x, prop_info.y, prop_info.z);
            XMFLOAT3 vec1{ prop_info.f4, -prop_info.f6, prop_info.f5 };
            XMFLOAT3 vec2{ prop_info.sin_angle, -prop_info.f9, prop_info.cos_angle };

            XMVECTOR v2 = XMLoadFloat3(&vec1);
            XMVECTOR v3 = XMLoadFloat3(&vec2);
            XMVECTOR v1 = XMVector3Cross(v3, v2);
            v1 = XMVector3Normalize(v1);
            v2 = XMVector3Normalize(v2);
            v3 = XMVector3Normalize(v3);

            auto rotation_matrix = XMMATRIX(
                -XMVectorGetX(v1), -XMVectorGetY(v1),  XMVectorGetZ(v1), 0.0f,
                 XMVectorGetX(v2),  XMVectorGetY(v2),  XMVectorGetZ(v2), 0.0f,
                -XMVectorGetX(v3), -XMVectorGetY(v3),  XMVectorGetZ(v3), 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f);

            float scale = prop_info.scaling_factor;
            XMMATRIX scaling_matrix = XMMatrixScaling(scale, scale, scale);
            XMMATRIX translation_matrix = XMMatrixTranslationFromVector(XMLoadFloat3(&translation));
            XMMATRIX transform = scaling_matrix * XMMatrixTranspose(rotation_matrix) * translation_matrix;
            XMStoreFloat4x4(&perObjectCBs[j].world, transform);

            auto& mesh = propMeshes[j];
            if (mesh.uv_coord_indices.size() == mesh.tex_indices.size() &&
                mesh.uv_coord_indices.size() < MAX_NUM_TEX_INDICES &&
                modelFilePtr->textures_parsed_correctly) {
                perObjectCBs[j].num_uv_texture_pairs = (uint32_t)mesh.uv_coord_indices.size();
                for (size_t k = 0; k < mesh.uv_coord_indices.size(); k++) {
                    perObjectCBs[j].uv_indices[k / 4][k % 4] = (uint32_t)mesh.uv_coord_indices[k];
                    perObjectCBs[j].texture_indices[k / 4][k % 4] = (uint32_t)mesh.tex_indices[k];
                    perObjectCBs[j].blend_flags[k / 4][k % 4] = (uint32_t)mesh.blend_flags[k];
                    perObjectCBs[j].texture_types[k / 4][k % 4] = (uint32_t)mesh.texture_types[k];
                }
            }
        }

        auto pst = geom.unknown_tex_stuff1.empty() ? PixelShaderType::OldModel : PixelShaderType::NewModel;
        auto meshIds = map_renderer->AddProp(propMeshes, perObjectCBs, (uint32_t)i, pst);

        if (modelFilePtr->textures_parsed_correctly) {
            for (size_t l = 0; l < meshIds.size() && l < perMeshTexIds.size(); l++) {
                map_renderer->GetMeshManager()->SetTexturesForMesh(
                    meshIds[l], map_renderer->GetTextureManager()->GetTextures(perMeshTexIds[l]), 3);
            }
        }
    }

    m_propPlaceIndex = end;

    if (total > 0)
        m_loadProgress = 0.30f + 0.70f * (static_cast<float>(m_propPlaceIndex) / total);

    if (m_propPlaceIndex >= total)
    {
        m_replayCtx.mapLoaded = true;
        m_loadProgress = 1.0f;
        m_loadingPhase = LoadingPhase::Ready;

        SetWindowTextW(m_hwnd, BuildWindowTitle(m_matchMeta).c_str());
    }
}

// ---------------------------------------------------------------------------
// Tick / Update / Render
// ---------------------------------------------------------------------------

void ReplayWindow::Tick()
{
    if (!m_alive) return;

    // Poll async agent snapshot parsing
    PollAgentParseCompletion(m_replayCtx);

    // Poll async StoC event parsing
    PollStoCParseCompletion(m_replayCtx);

    // Once agents are loaded, classify and build per-category lists
    if (m_replayCtx.agentsLoaded && !m_agentsClassified && !m_replayCtx.agents.empty())
    {
        ClassifyAgents(m_replayCtx.agents, m_matchMeta, m_replayCtx.mapId);

        m_sortedAgentIds.reserve(m_replayCtx.agents.size());
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            m_sortedAgentIds.push_back(id);
            switch (ard.type) {
            case AgentType::Player:  m_playerIds.push_back(id);  break;
            case AgentType::NPC:     m_npcIds.push_back(id);     break;
            case AgentType::Gadget:  m_gadgetIds.push_back(id);  break;
            case AgentType::Flag:    m_flagIds.push_back(id);    break;
            case AgentType::Spirit:  m_spiritIds.push_back(id);  break;
            case AgentType::Item:    m_itemIds.push_back(id);    break;
            default:                 m_unknownIds.push_back(id);  break;
            }
        }
        std::sort(m_sortedAgentIds.begin(), m_sortedAgentIds.end());
        std::sort(m_playerIds.begin(),  m_playerIds.end());
        std::sort(m_npcIds.begin(),     m_npcIds.end());
        std::sort(m_gadgetIds.begin(),  m_gadgetIds.end());
        std::sort(m_flagIds.begin(),    m_flagIds.end());
        std::sort(m_spiritIds.begin(),  m_spiritIds.end());
        std::sort(m_itemIds.begin(),    m_itemIds.end());
        std::sort(m_unknownIds.begin(), m_unknownIds.end());

        // Build modelId -> PlayerMeta* lookup for player metadata
        std::unordered_map<uint32_t, const PlayerMeta*> modelToPlayer;
        for (auto& [partyId, party] : m_matchMeta.parties)
            for (auto& p : party.players)
                modelToPlayer[static_cast<uint32_t>(p.model_id)] = &p;

        auto ProfShort = [](int id) -> const char* {
            switch (id) {
            case 1: return "W"; case 2: return "R"; case 3: return "Mo";
            case 4: return "N"; case 5: return "Me"; case 6: return "E";
            case 7: return "A"; case 8: return "Rt"; case 9: return "P";
            case 10: return "D"; default: return "?";
            }
        };

        for (int id : m_playerIds)
        {
            auto& ard = m_replayCtx.agents[id];
            auto it = modelToPlayer.find(ard.modelId);
            if (it != modelToPlayer.end())
            {
                const PlayerMeta* pm = it->second;
                ard.playerNumber  = pm->player_number;
                ard.primaryProf   = pm->primary;
                ard.secondaryProf = pm->secondary;
                ard.playerLevel   = pm->level;

                char buf[256];
                snprintf(buf, sizeof(buf), "%s/%s%d %s",
                         ProfShort(pm->primary), ProfShort(pm->secondary),
                         pm->level, ard.playerName.c_str());
                ard.partyBarLabel = buf;
            }
            else
            {
                ard.partyBarLabel = ard.playerName;
            }

            if (ard.teamId == 1)
                m_team1PlayerIds.push_back(id);
            else if (ard.teamId == 2)
                m_team2PlayerIds.push_back(id);
        }

        auto sortByPlayerNum = [&](std::vector<int>& ids) {
            std::sort(ids.begin(), ids.end(), [&](int a, int b) {
                return m_replayCtx.agents[a].playerNumber
                     < m_replayCtx.agents[b].playerNumber;
            });
        };
        sortByPlayerNum(m_team1PlayerIds);
        sortByPlayerNum(m_team2PlayerIds);

        // Build guild header strings
        auto BuildGuildHeader = [&](const std::string& partyId) -> std::string {
            auto pit = m_matchMeta.parties.find(partyId);
            if (pit == m_matchMeta.parties.end()) return "Unknown";
            std::map<int, int> guildCounts;
            for (auto& p : pit->second.players)
                if (p.guild_id > 0) guildCounts[p.guild_id]++;
            int bestGuildId = 0, bestCount = 0;
            for (auto& [gid, cnt] : guildCounts)
                if (cnt > bestCount) { bestGuildId = gid; bestCount = cnt; }
            if (bestGuildId == 0) return "Unknown";
            auto git = m_matchMeta.guilds.find(std::to_string(bestGuildId));
            if (git != m_matchMeta.guilds.end())
                return git->second.name + " [" + git->second.tag + "]";
            return "Unknown";
        };
        m_team1GuildHeader = BuildGuildHeader("1");
        m_team2GuildHeader = BuildGuildHeader("2");

        // Build NPC + Spirit team lists for Allies section
        auto NpcSortOrder = [](const std::string& cat) -> int {
            if (cat == "Guild Lord")    return 0;
            if (cat == "Bodyguard")     return 1;
            if (cat == "Knight")        return 2;
            if (cat == "Archer")        return 3;
            if (cat == "Footman")       return 4;
            return 5; // Pets, Spirits, other NPCs
        };

        for (int id : m_npcIds)
        {
            auto& ard = m_replayCtx.agents[id];
            ard.partyBarLabel = ard.categoryName;
            if (ard.teamId == 1)      m_team1NpcIds.push_back(id);
            else if (ard.teamId == 2) m_team2NpcIds.push_back(id);
        }
        for (int id : m_spiritIds)
        {
            auto& ard = m_replayCtx.agents[id];
            ard.partyBarLabel = ard.categoryName;
            if (ard.teamId == 1)      m_team1NpcIds.push_back(id);
            else if (ard.teamId == 2) m_team2NpcIds.push_back(id);
        }

        auto sortNpcs = [&](std::vector<int>& ids) {
            std::sort(ids.begin(), ids.end(), [&](int a, int b) {
                auto& aa = m_replayCtx.agents[a];
                auto& bb = m_replayCtx.agents[b];
                int oa = NpcSortOrder(aa.categoryName);
                int ob = NpcSortOrder(bb.categoryName);
                if (oa != ob) return oa < ob;
                return aa.agent_id < bb.agent_id;
            });
        };
        sortNpcs(m_team1NpcIds);
        sortNpcs(m_team2NpcIds);

        m_agentsClassified = true;
    }

    // Once both agents and StoC data are loaded, distribute MOVE_TO_POINT
    // events from the global StoC list into per-agent moveEvents vectors.
    if (m_agentsClassified && m_replayCtx.stocLoaded && !m_moveEventsBuilt)
    {
        for (auto& ev : m_replayCtx.stocData.agentMovement)
        {
            auto it = m_replayCtx.agents.find(ev.agent_id);
            if (it != m_replayCtx.agents.end())
            {
                it->second.moveEvents.push_back(
                    MoveToPointEvent{ ev.time, ev.x, ev.y });
            }
        }
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            std::sort(ard.moveEvents.begin(), ard.moveEvents.end(),
                      [](const MoveToPointEvent& a, const MoveToPointEvent& b) {
                          return a.time < b.time;
                      });
        }
        m_moveEventsBuilt = true;
    }

    // Build per-agent casting intervals from StoC skill/attack-skill events.
    // A SKILL_ACTIVATED opens an interval; SKILL_FINISHED / SKILL_STOPPED closes it.
    // INSTANT_SKILL_USED has no cast time so we skip it.
    if (m_agentsClassified && m_replayCtx.stocLoaded && !m_castIntervalsBuilt)
    {
        // Track open (unfinished) casts per caster_id
        std::unordered_map<int, CastInterval> openCasts;

        auto processStart = [&](int casterId, float time, int skillId) {
            openCasts[casterId] = CastInterval{ time, time, skillId };
        };
        auto processEnd = [&](int casterId, float time) {
            auto oc = openCasts.find(casterId);
            if (oc != openCasts.end()) {
                oc->second.end = time;
                auto it = m_replayCtx.agents.find(casterId);
                if (it != m_replayCtx.agents.end())
                    it->second.castHistory.push_back(oc->second);
                openCasts.erase(oc);
            }
        };

        for (auto& ev : m_replayCtx.stocData.skill)
        {
            if (ev.type == "SKILL_ACTIVATED")
                processStart(ev.caster_id, ev.time, ev.skill_id);
            else if (ev.type == "SKILL_FINISHED" || ev.type == "SKILL_STOPPED")
                processEnd(ev.caster_id, ev.time);
        }

        for (auto& ev : m_replayCtx.stocData.attackSkill)
        {
            if (ev.type == "ATTACK_SKILL_ACTIVATED")
                processStart(ev.caster_id, ev.time, ev.skill_id);
            else if (ev.type == "ATTACK_SKILL_FINISHED" || ev.type == "ATTACK_SKILL_STOPPED")
                processEnd(ev.caster_id, ev.time);
        }

        // Sort each agent's cast history by start time
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            std::sort(ard.castHistory.begin(), ard.castHistory.end(),
                      [](const CastInterval& a, const CastInterval& b) {
                          return a.start < b.start;
                      });
        }
        m_castIntervalsBuilt = true;
    }

    // Build flag state timeline (needs both agents and StoC for jumbo events)
    if (m_agentsClassified && m_replayCtx.stocLoaded && !m_flagStateBuilt)
        BuildFlagStateTimeline();

    // Auto-load saved calibration transform for this map, or fall back to
    // WebGL-derived defaults if no saved data exists.
    if (!m_calibrationLoaded && m_replayCtx.mapLoaded)
    {
        bool found = false;
        MapTransform saved = LoadMapTransform(m_replayCtx.mapId, &found);
        m_replayCtx.mapTransform = found ? saved : GetDefaultMapTransform();
        m_calibrationLoaded = true;
    }

    m_timer.Tick([this]()
    {
        if (m_loadingPhase == LoadingPhase::Ready)
            Update(m_timer.GetElapsedSeconds() * 1000.0);
    });

    // Playback engine: advance timeline when playing
    if (m_replayCtx.isPlaying && m_loadingPhase == LoadingPhase::Ready)
    {
        float dt = static_cast<float>(m_timer.GetElapsedSeconds());
        float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
        m_debugTimeline += dt * m_replayCtx.playbackSpeed;

        if (m_debugTimeline >= maxT)
        {
            if (m_replayCtx.loopPlayback) {
                m_debugTimeline = 0.f;
            } else {
                m_debugTimeline = maxT;
                m_replayCtx.isPlaying = false;
            }
        }
    }

    switch (m_loadingPhase)
    {
    case LoadingPhase::Validate:
        StepValidate();
        RenderLoadingScreen();
        break;

    case LoadingPhase::Init:
        StepLoadInit();
        RenderLoadingScreen();
        break;

    case LoadingPhase::PropModels:
        StepLoadPropModels();
        RenderLoadingScreen();
        break;

    case LoadingPhase::PlaceProps:
        StepPlaceProps();
        RenderLoadingScreen();
        break;

    case LoadingPhase::Ready:
        Render();
        break;

    case LoadingPhase::Error:
        SetWindowTextA(m_hwnd, ("Replay - ERROR: " + m_errorMsg).c_str());
        RenderLoadingScreen();
        break;
    }
}

void ReplayWindow::Update(double elapsedMs)
{
    float dt = static_cast<float>(elapsedMs / 1000.0);
    UpdateFollowCamera(dt);
    m_mapRenderer->Update(dt);
}

void ReplayWindow::Render()
{
    Clear();

    m_mapRenderer->Render(
        m_deviceResources->GetRenderTargetView(),
        nullptr,
        m_deviceResources->GetDepthStencilView());

    DrawImGuiOverlay();

    m_deviceResources->Present();
}

// ---------------------------------------------------------------------------
// Loading screen overlay (dark background + animated progress bar)
// ---------------------------------------------------------------------------

void ReplayWindow::RenderLoadingScreen()
{
    auto* ctx = m_deviceResources->GetD3DDeviceContext();
    auto* rtv = m_deviceResources->GetRenderTargetView();
    auto* dsv = m_deviceResources->GetDepthStencilView();

    float darkBg[4] = { 0.06f, 0.06f, 0.09f, 1.0f };
    ctx->ClearRenderTargetView(rtv, darkBg);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);

    auto vp = m_deviceResources->GetScreenViewport();
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    // Build overlay quads in NDC space
    const float barHalfW = 0.20f;
    const float barHalfH = 0.004f;
    const float barY     = 0.0f;

    float fillRight = -barHalfW + 2.0f * barHalfW * std::clamp(m_loadProgress, 0.0f, 1.0f);

    auto makeQuad = [](OverlayVertex* out, float l, float t, float r, float b,
                       float cr, float cg, float cb, float ca)
    {
        out[0] = { l, t, cr, cg, cb, ca };
        out[1] = { r, t, cr, cg, cb, ca };
        out[2] = { l, b, cr, cg, cb, ca };
        out[3] = { l, b, cr, cg, cb, ca };
        out[4] = { r, t, cr, cg, cb, ca };
        out[5] = { r, b, cr, cg, cb, ca };
    };

    OverlayVertex verts[12];
    makeQuad(&verts[0], -barHalfW, barY + barHalfH, barHalfW, barY - barHalfH,
             0.15f, 0.15f, 0.20f, 1.0f);
    makeQuad(&verts[6], -barHalfW, barY + barHalfH, fillRight, barY - barHalfH,
             0.25f, 0.50f, 1.00f, 1.0f);

    // Upload vertices
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(m_overlayVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, verts, sizeof(verts));
        ctx->Unmap(m_overlayVB.Get(), 0);
    }

    // Set pipeline state for 2D overlay
    UINT stride = sizeof(OverlayVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, m_overlayVB.GetAddressOf(), &stride, &offset);
    ctx->IASetInputLayout(m_overlayIL.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_overlayVS.Get(), nullptr, 0);
    ctx->PSSetShader(m_overlayPS.Get(), nullptr, 0);
    ctx->RSSetState(m_overlayRS.Get());
    ctx->OMSetDepthStencilState(m_overlayDSS.Get(), 0);
    float blendFactor[4] = {};
    ctx->OMSetBlendState(m_overlayBS.Get(), blendFactor, 0xFFFFFFFF);

    ctx->Draw(12, 0);

    m_deviceResources->Present();
}

// ---------------------------------------------------------------------------
// ImGui overlay: menu bar + debug windows
// ---------------------------------------------------------------------------

void ReplayWindow::DrawImGuiOverlay()
{
    if (!m_imguiInitialized)
        InitImGui();

    ImGuiContext* prevCtx = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_imguiContext);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Top menu bar
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::BeginMenu("Preferences"))
            {
                if (ImGui::MenuItem("Shortcuts"))
                    m_showShortcutPreferences = true;
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Close Replay"))
            {
                PostMessage(m_hwnd, WM_CLOSE, 0, 0);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Agent Overlay", nullptr, &m_showAgentOverlay);
            ImGui::Separator();
            ImGui::MenuItem("Team 1 Party", nullptr, &m_showTeam1Party);
            ImGui::MenuItem("Team 2 Party", nullptr, &m_showTeam2Party);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug"))
        {
            ImGui::MenuItem("Agent Data", nullptr, &m_showAgentDataWindow);
            ImGui::MenuItem("Map Calibration", nullptr, &m_showMapCalibrationWindow);
            ImGui::MenuItem("Interpolation", nullptr, &m_showInterpolationWindow);
            ImGui::MenuItem("StoC Events", nullptr, &m_showStoCWindow);
            ImGui::EndMenu();
        }

        if (m_replayCtx.agentParseProgress && !m_replayCtx.agentsLoaded)
        {
            int done = m_replayCtx.agentParseProgress->files_done.load();
            int total = m_replayCtx.agentParseProgress->files_total.load();
            auto label = std::format("  Parsing agents... {}/{}", done, total);
            ImGui::TextDisabled("%s", label.c_str());
        }
        if (m_replayCtx.stocParseProgress && !m_replayCtx.stocLoaded)
        {
            int done = m_replayCtx.stocParseProgress->files_done.load();
            int total = m_replayCtx.stocParseProgress->files_total.load();
            auto label = std::format("  Parsing StoC... {}/{}", done, total);
            ImGui::TextDisabled("%s", label.c_str());
        }

        ImGui::EndMainMenuBar();
    }

    DrawTimelineController();

    if (m_showAgentDataWindow)
        DrawAgentDataWindow();

    if (m_showMapCalibrationWindow)
        DrawMapCalibrationWindow();

    if (m_showInterpolationWindow)
        DrawInterpolationWindow();

    if (m_showStoCWindow)
        DrawStoCWindow();

    if (m_showShortcutPreferences)
    {
        ImGui::OpenPopup("Shortcut Preferences");
        m_showShortcutPreferences = false;
    }
    DrawShortcutPreferences();

    DrawPartyWindows();

    DrawAgentOverlay();
    DrawFlags();

    // Commit deferred left-click to pan if no agent was clicked
    if (m_leftClickPending)
    {
        m_leftClickPending = false;
        m_leftMouseDown = true;
        if (!m_rightMouseDown)
        {
            ShowCursor(FALSE);
            SetCapture(m_hwnd);
        }
        else
        {
            ClipCursor(nullptr);
            ShowCursor(FALSE);
        }
    }

    // Keyboard shortcuts (checked after all windows so WantCaptureKeyboard is accurate)
    if (!ImGui::GetIO().WantCaptureKeyboard)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && m_cameraMode == CameraMode::FollowAgent)
            ExitFollowMode();

        const auto& hk = ReplayHotkeys::Get();
        float maxT = std::max(1.f, m_replayCtx.maxReplayTime);

        if (ImGui::IsKeyPressed((ImGuiKey)hk.rewind5s))
            m_debugTimeline = std::max(0.f, m_debugTimeline - 5.f);

        if (ImGui::IsKeyPressed((ImGuiKey)hk.forward5s))
            m_debugTimeline = std::min(maxT, m_debugTimeline + 5.f);

        if (ImGui::IsKeyPressed((ImGuiKey)hk.playPause))
            m_replayCtx.isPlaying = !m_replayCtx.isPlaying;
    }

    // Determine cursor mode, then apply drag overrides before committing
    UpdateCursorMode();
    if (m_leftMouseDown)
        g_CurrentCursor = CursorMode::Hidden;
    else if (m_rightMouseDown)
        g_CurrentCursor = CursorMode::Precision;
    ApplyCursor();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    ImGui::SetCurrentContext(prevCtx);
}

// ---------------------------------------------------------------------------
// Map calibration transform: save / load
// ---------------------------------------------------------------------------

static constexpr const char* kCalibrationFile = "map_transforms.txt";

static void SaveMapTransform(int mapId, const MapTransform& t)
{
    std::map<int, MapTransform> all;
    {
        std::ifstream in(kCalibrationFile);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            int id;
            float ox, oy, oz, sx, sy, sz, rot;
            int fx, fy, fz, syz, sxz, sxy;
            if (sscanf_s(line.c_str(), "%d %f %f %f %f %f %f %f %d %d %d %d %d %d",
                         &id, &ox, &oy, &oz, &sx, &sy, &sz, &rot,
                         &fx, &fy, &fz, &syz, &sxz, &sxy) == 14)
            {
                all[id] = { ox, oy, oz, sx, sy, sz, rot,
                            fx != 0, fy != 0, fz != 0,
                            syz != 0, sxz != 0, sxy != 0 };
            }
        }
    }
    all[mapId] = t;
    {
        std::ofstream out(kCalibrationFile);
        out << "# map_id offX offY offZ scaleX scaleY scaleZ rotation flipX flipY flipZ swapYZ swapXZ swapXY\n";
        for (auto& [id, mt] : all)
        {
            char buf[512];
            snprintf(buf, sizeof(buf),
                     "%d %.4f %.4f %.4f %.6f %.6f %.6f %.4f %d %d %d %d %d %d\n",
                     id, mt.offsetX, mt.offsetY, mt.offsetZ,
                     mt.scaleX, mt.scaleY, mt.scaleZ, mt.rotationDegrees,
                     mt.flipX ? 1 : 0, mt.flipY ? 1 : 0, mt.flipZ ? 1 : 0,
                     mt.swapYZ ? 1 : 0, mt.swapXZ ? 1 : 0, mt.swapXY ? 1 : 0);
            out << buf;
        }
    }
}

static MapTransform LoadMapTransform(int mapId, bool* found)
{
    if (found) *found = false;
    std::ifstream in(kCalibrationFile);
    if (!in.is_open()) return {};
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        int id;
        float ox, oy, oz, sx, sy, sz, rot;
        int fx, fy, fz, syz, sxz, sxy;
        if (sscanf_s(line.c_str(), "%d %f %f %f %f %f %f %f %d %d %d %d %d %d",
                     &id, &ox, &oy, &oz, &sx, &sy, &sz, &rot,
                     &fx, &fy, &fz, &syz, &sxz, &sxy) == 14)
        {
            if (id == mapId) {
                if (found) *found = true;
                return { ox, oy, oz, sx, sy, sz, rot,
                         fx != 0, fy != 0, fz != 0,
                         syz != 0, sxz != 0, sxy != 0 };
            }
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Agent Overlay: full calibration transform pipeline + rendering
// ---------------------------------------------------------------------------

static ImU32 GetAgentTeamColor(uint8_t teamId)
{
    switch (teamId) {
    case 1:  return IM_COL32(0x2A, 0x8C, 0xFF, 0xFF);
    case 2:  return IM_COL32(0xFF, 0x4A, 0x4A, 0xFF);
    default: return IM_COL32(0xAA, 0xAA, 0xAA, 0xFF);
    }
}

// Binary search: find index of last snapshot with time <= t
static int FindSnapshotIndex(const std::vector<AgentSnapshot>& snaps, float t)
{
    int lo = 0, hi = static_cast<int>(snaps.size()) - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (snaps[mid].time <= t) lo = mid; else hi = mid - 1;
    }
    return lo;
}

// Snap to the nearest snapshot <= t (no blending). Used for flags and
// when interpolation is disabled.
static void SnapAgentPosition(const AgentReplayData& ard, float t,
                              float& outX, float& outY, float& outZ)
{
    const auto& snaps = ard.snapshots;
    if (snaps.empty()) { outX = outY = outZ = 0.f; return; }
    if (t <= snaps.front().time) {
        outX = snaps.front().x; outY = snaps.front().y; outZ = snaps.front().z; return;
    }
    if (t >= snaps.back().time) {
        outX = snaps.back().x; outY = snaps.back().y; outZ = snaps.back().z; return;
    }
    int idx = FindSnapshotIndex(snaps, t);
    outX = snaps[idx].x; outY = snaps[idx].y; outZ = snaps[idx].z;
}

// Stationary threshold: if both bracketing snapshots are within this distance
// per axis (game units), skip interpolation to avoid micro-jitter from data noise.
// Matches the RAW_COORDINATE_EPSILON from the working website implementation.
static constexpr float kStationaryEpsilon = 1.0f;

// Original linear interpolation (legacy behavior).
static void LinearInterpolatePosition(const AgentReplayData& ard, float t,
                                      float& outX, float& outY, float& outZ)
{
    const auto& snaps = ard.snapshots;
    if (snaps.empty()) { outX = outY = outZ = 0.f; return; }
    if (t <= snaps.front().time) {
        outX = snaps.front().x; outY = snaps.front().y; outZ = snaps.front().z; return;
    }
    if (t >= snaps.back().time) {
        outX = snaps.back().x; outY = snaps.back().y; outZ = snaps.back().z; return;
    }
    int lo = FindSnapshotIndex(snaps, t);
    auto& s0 = snaps[lo];
    if (lo + 1 < static_cast<int>(snaps.size())) {
        auto& s1 = snaps[lo + 1];

        if (fabsf(s1.x - s0.x) <= kStationaryEpsilon &&
            fabsf(s1.y - s0.y) <= kStationaryEpsilon &&
            fabsf(s1.z - s0.z) <= kStationaryEpsilon) {
            outX = s0.x; outY = s0.y; outZ = s0.z;
            return;
        }

        float dt = s1.time - s0.time;
        float a = (dt > 0.001f) ? (t - s0.time) / dt : 0.f;
        outX = s0.x + (s1.x - s0.x) * a;
        outY = s0.y + (s1.y - s0.y) * a;
        outZ = s0.z + (s1.z - s0.z) * a;
    } else {
        outX = s0.x; outY = s0.y; outZ = s0.z;
    }
}

// Find the last MOVE_TO_POINT event at or before time t (binary search).
// Returns -1 if none exists.
static int FindMoveEventIndex(const std::vector<MoveToPointEvent>& moves, float t)
{
    if (moves.empty()) return -1;
    int lo = 0, hi = static_cast<int>(moves.size()) - 1, result = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (moves[mid].time <= t) { result = mid; lo = mid + 1; }
        else { hi = mid - 1; }
    }
    return result;
}

// Improved interpolation: MOVE_TO_POINT aware.
//   - For small gaps (<= gapThreshold): pure linear lerp
//   - For large gaps: use MOVE_TO_POINT target as movement direction anchor
//     blended with linear interpolation via velocityInfluence slider.
//   - If no MOVE_TO_POINT exists for the interval, pure linear interpolation.
static void ImprovedInterpolatePosition(const AgentReplayData& ard, float t,
                                        const InterpolationSettings& s,
                                        float& outX, float& outY, float& outZ)
{
    const auto& snaps = ard.snapshots;
    if (snaps.empty()) { outX = outY = outZ = 0.f; return; }
    if (t <= snaps.front().time) {
        outX = snaps.front().x; outY = snaps.front().y; outZ = snaps.front().z; return;
    }
    if (t >= snaps.back().time) {
        outX = snaps.back().x; outY = snaps.back().y; outZ = snaps.back().z; return;
    }

    int lo = FindSnapshotIndex(snaps, t);
    auto& prev = snaps[lo];

    if (lo + 1 >= static_cast<int>(snaps.size())) {
        outX = prev.x; outY = prev.y; outZ = prev.z;
        return;
    }
    auto& next = snaps[lo + 1];

    if (fabsf(next.x - prev.x) <= kStationaryEpsilon &&
        fabsf(next.y - prev.y) <= kStationaryEpsilon &&
        fabsf(next.z - prev.z) <= kStationaryEpsilon) {
        outX = prev.x; outY = prev.y; outZ = prev.z;
        return;
    }

    float gap = next.time - prev.time;
    float alpha = (gap > 0.001f) ? (t - prev.time) / gap : 0.f;

    // Base linear interpolation
    float lx = prev.x + (next.x - prev.x) * alpha;
    float ly = prev.y + (next.y - prev.y) * alpha;
    float lz = prev.z + (next.z - prev.z) * alpha;

    // MOVE_TO_POINT directional prediction for large gaps
    if (gap > s.gapThreshold && s.velocityInfluence > 0.f)
    {
        int moveIdx = FindMoveEventIndex(ard.moveEvents, t);
        if (moveIdx >= 0)
        {
            auto& move = ard.moveEvents[moveIdx];

            // Direction from previous snapshot position toward the MOVE_TO_POINT target
            float dx = move.targetX - prev.x;
            float dy = move.targetY - prev.y;
            float dist = sqrtf(dx * dx + dy * dy);

            if (dist > 1.f)
            {
                // Estimate speed from the distance between the two bracketing snapshots
                float speed = sqrtf((next.x - prev.x) * (next.x - prev.x) +
                                    (next.y - prev.y) * (next.y - prev.y)) / gap;

                float dirX = dx / dist;
                float dirY = dy / dist;
                float dt = t - prev.time;

                float px = prev.x + dirX * speed * dt;
                float py = prev.y + dirY * speed * dt;
                float pz = prev.z;

                float beta = (gap - s.gapThreshold) / 1.0f;
                if (beta > 1.f) beta = 1.f;
                beta *= s.velocityInfluence;

                outX = lx + (px - lx) * beta;
                outY = ly + (py - ly) * beta;
                outZ = lz + (pz - lz) * beta;
                return;
            }
        }
        // No applicable MOVE_TO_POINT — fall through to pure linear
    }

    outX = lx;
    outY = ly;
    outZ = lz;
}

// Internal dispatch: run the active interpolation mode (or snap when disabled).
static void DispatchInterpolation(const AgentReplayData& ard, float t,
                                  const InterpolationSettings& is,
                                  float& outX, float& outY, float& outZ)
{
    if (!is.enabled) {
        SnapAgentPosition(ard, t, outX, outY, outZ);
        return;
    }
    if (is.mode == InterpolationMode::OriginalLinear)
        LinearInterpolatePosition(ard, t, outX, outY, outZ);
    else
        ImprovedInterpolatePosition(ard, t, is, outX, outY, outZ);
}

// Unified entry point: routes through flag snap / disabled snap /
// original linear / improved, based on agent type and settings.
//
// Casting freeze is intentionally NOT applied here.  The snapshot data
// already reflects the game's movement freeze during casts — consecutive
// snapshots during a cast have nearly identical positions, so the
// stationary-detection epsilon in the lerp functions keeps the agent
// still without introducing a position discontinuity at cast boundaries.
//
// Death freeze interpolates to the moment the agent died instead of
// snapping to a raw snapshot, avoiding a teleport at the alive→dead edge.
static void InterpolateAgentPosition(const AgentReplayData& ard, float t,
                                     const InterpolationSettings& is,
                                     float& outX, float& outY, float& outZ)
{
    // Flags and Spirits never interpolate — snap to nearest recorded position
    if (ard.type == AgentType::Flag || ard.type == AgentType::Spirit) {
        SnapAgentPosition(ard, t, outX, outY, outZ);
        return;
    }

    // Death freeze: interpolate to the moment of death so there is no
    // position jump at the alive→dead boundary.
    if (ard.isDeadAtTime(t)) {
        float deathT = ard.deathTransitionTime(t);
        DispatchInterpolation(ard, deathT, is, outX, outY, outZ);
        return;
    }

    DispatchInterpolation(ard, t, is, outX, outY, outZ);
}

static std::string GetAgentLabel(const AgentReplayData& ard)
{
    switch (ard.type) {
    case AgentType::Player: return ard.playerName;
    case AgentType::NPC:    return ard.categoryName;
    case AgentType::Gadget: return ard.categoryName;
    case AgentType::Flag:   return "Flag";
    case AgentType::Spirit: return ard.categoryName;
    case AgentType::Item:   return ard.categoryName;
    default:                return std::format("Agent {}", ard.agent_id);
    }
}

static XMFLOAT3 ApplyMapTransformToPos(float snapX, float snapY, float snapZ,
                                        const MapTransform& t)
{
    // 0. Base axis remap: GWCA (x,y,z_height) → GWMB (x, z_height, y)
    float px = snapX;
    float py = snapZ;
    float pz = snapY;

    // 1. Axis swaps
    if (t.swapYZ) { float tmp = py; py = pz; pz = tmp; }
    if (t.swapXZ) { float tmp = px; px = pz; pz = tmp; }
    if (t.swapXY) { float tmp = px; px = py; py = tmp; }

    // 2. Axis flips
    if (t.flipX) px = -px;
    if (t.flipY) py = -py;
    if (t.flipZ) pz = -pz;

    // 3. Rotation around Y axis
    if (t.rotationDegrees != 0.f) {
        float rad = t.rotationDegrees * (XM_PI / 180.f);
        float c = cosf(rad), s = sinf(rad);
        float rx = px * c - pz * s;
        float rz = px * s + pz * c;
        px = rx;
        pz = rz;
    }

    // 4. Offset
    px += t.offsetX;
    py += t.offsetY;
    pz += t.offsetZ;

    // 5. Scale
    px *= t.scaleX;
    py *= t.scaleY;
    pz *= t.scaleZ;

    return { px, py, pz };
}

static bool ProjectToScreen(XMMATRIX viewProj, float vpW, float vpH,
                             const XMFLOAT3& worldPos, float& scrX, float& scrY)
{
    XMVECTOR clip = XMVector4Transform(
        XMVectorSet(worldPos.x, worldPos.y, worldPos.z, 1.f), viewProj);
    float w = XMVectorGetW(clip);
    if (w <= 0.001f) return false;
    float ndcX = XMVectorGetX(clip) / w;
    float ndcY = XMVectorGetY(clip) / w;
    scrX = (ndcX + 1.f) * 0.5f * vpW;
    scrY = (1.f - ndcY) * 0.5f * vpH;
    return (scrX > -200.f && scrX < vpW + 200.f &&
            scrY > -200.f && scrY < vpH + 200.f);
}

// ---------------------------------------------------------------------------
// Flag state machine — pre-compute a timeline of flag events per team
// ---------------------------------------------------------------------------

void ReplayWindow::BuildFlagStateTimeline()
{
    m_flagStateBuilt = true;
    m_flagState[0] = {};
    m_flagState[1] = {};
    m_flagStandFound = false;
    m_captureEvents.clear();

    // 1. Find Guild Lord positions per team
    float gl1x = 0, gl1y = 0, gl2x = 0, gl2y = 0;
    bool gl1found = false, gl2found = false;
    for (int id : m_npcIds)
    {
        auto& ard = m_replayCtx.agents[id];
        if (ard.categoryName != "Guild Lord" || ard.snapshots.empty()) continue;
        const auto& s = ard.snapshots.front();
        if (ard.teamId == 1 && !gl1found) { gl1x = s.x; gl1y = s.y; gl1found = true; }
        if (ard.teamId == 2 && !gl2found) { gl2x = s.x; gl2y = s.y; gl2found = true; }
    }
    if (!gl1found || !gl2found) return;

    // 2. Find Tower Flag Stand gadget position
    for (int id : m_gadgetIds)
    {
        auto& ard = m_replayCtx.agents[id];
        if (ard.categoryName == "Tower Flag Stand" && !ard.snapshots.empty())
        {
            const auto& s = ard.snapshots.front();
            m_flagStandX = s.x;
            m_flagStandY = s.y;
            m_flagStandZ = s.z;
            m_flagStandFound = true;
            break;
        }
    }

    // 3. Assign flag agents to teams via Guild Lord proximity
    for (int id : m_flagIds)
    {
        auto& ard = m_replayCtx.agents[id];
        if (ard.snapshots.empty()) continue;
        float fx = ard.snapshots.front().x;
        float fy = ard.snapshots.front().y;
        float d1 = (fx - gl1x) * (fx - gl1x) + (fy - gl1y) * (fy - gl1y);
        float d2 = (fx - gl2x) * (fx - gl2x) + (fy - gl2y) * (fy - gl2y);
        int teamIdx = (d1 < d2) ? 0 : 1;
        m_flagState[teamIdx].flagAgentIds.push_back(id);
    }

    // 4. Build timeline per team
    constexpr float kBaseDistSq  = 300.f * 300.f;
    constexpr float kStandDistSq = 300.f * 300.f;

    for (int ti = 0; ti < 2; ti++)
    {
        auto& fs = m_flagState[ti];
        if (fs.flagAgentIds.empty()) continue;
        fs.valid = true;

        // Sort flag agents by first snapshot time
        std::sort(fs.flagAgentIds.begin(), fs.flagAgentIds.end(),
                  [&](int a, int b) {
                      return m_replayCtx.agents[a].snapshots.front().time <
                             m_replayCtx.agents[b].snapshots.front().time;
                  });

        // Base position = first snapshot of the team's first flag agent
        {
            auto& first = m_replayCtx.agents[fs.flagAgentIds[0]];
            fs.baseX = first.snapshots.front().x;
            fs.baseY = first.snapshots.front().y;
            fs.baseZ = first.snapshots.front().z;
        }

        // For each flag agent, emit appear/disappear events
        for (int flagId : fs.flagAgentIds)
        {
            auto& ard = m_replayCtx.agents[flagId];
            float t0 = ard.snapshots.front().time;
            float t1 = ard.snapshots.back().time;
            float sx = ard.snapshots.front().x;
            float sy = ard.snapshots.front().y;
            float sz = ard.snapshots.front().z;

            // Classify the appear location
            float dBaseSq = (sx - fs.baseX) * (sx - fs.baseX) + (sy - fs.baseY) * (sy - fs.baseY);
            FlagLocationType loc = FlagLocationType::Ground;
            if (dBaseSq < kBaseDistSq)
                loc = FlagLocationType::Base;
            else if (m_flagStandFound)
            {
                float dStandSq = (sx - m_flagStandX) * (sx - m_flagStandX) + (sy - m_flagStandY) * (sy - m_flagStandY);
                if (dStandSq < kStandDistSq)
                    loc = FlagLocationType::Stand;
            }

            // Flag appears — store flagAgentId so we use its live position (moves with carrier)
            FlagEvent appearEv = { t0, loc, sx, sy, sz, -1, flagId };
            fs.timeline.push_back(appearEv);

            // Flag disappears — try to find carrier (same-team player with weapon_type 0)
            float ex = ard.snapshots.back().x;
            float ey = ard.snapshots.back().y;
            float ez = ard.snapshots.back().z;

            int carrierId = -1;
            float bestDistSq = FLT_MAX;
            int teamId = ti + 1;
            constexpr float kCarrierDistSqBuild = 1500.f * 1500.f;
            auto tryPlayerSnapshot = [&](const AgentReplayData& pard, const AgentSnapshot* psnap, int playerId) {
                if (!psnap || psnap->weapon_type != 0 || psnap->is_dead) return;
                if (pard.teamId != teamId) return;
                float dx = psnap->x - ex;
                float dy = psnap->y - ey;
                float dsq = dx * dx + dy * dy;
                if (dsq < bestDistSq && dsq < kCarrierDistSqBuild)
                {
                    bestDistSq = dsq;
                    carrierId = playerId;
                }
            };

            const std::vector<int>* teamPlayers = (ti == 0) ? &m_team1PlayerIds : &m_team2PlayerIds;
            for (int pid : *teamPlayers)
            {
                auto it = m_replayCtx.agents.find(pid);
                if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) continue;
                auto& pard = it->second;

                const AgentSnapshot* psnap = nullptr;
                {
                    int lo = 0, hi = static_cast<int>(pard.snapshots.size()) - 1;
                    if (t1 >= pard.snapshots.back().time) psnap = &pard.snapshots.back();
                    else if (t1 <= pard.snapshots.front().time) psnap = &pard.snapshots.front();
                    else {
                        while (lo < hi) {
                            int mid = lo + (hi - lo + 1) / 2;
                            if (pard.snapshots[mid].time <= t1) lo = mid; else hi = mid - 1;
                        }
                        psnap = &pard.snapshots[lo];
                    }
                }
                tryPlayerSnapshot(pard, psnap, pid);

                if (carrierId < 0 && t1 < pard.snapshots.back().time)
                {
                    int idx = 0;
                    while (idx < static_cast<int>(pard.snapshots.size()) && pard.snapshots[idx].time <= t1)
                        idx++;
                    if (idx < static_cast<int>(pard.snapshots.size()) &&
                        pard.snapshots[idx].time <= t1 + 0.5f)
                    {
                        tryPlayerSnapshot(pard, &pard.snapshots[idx], pid);
                    }
                }
            }

            // Always add Carried event when flag disappears (so it doesn't stay at base)
            fs.timeline.push_back({ t1 + 0.001f, FlagLocationType::Carried, ex, ey, ez, carrierId });
        }

        // 5. CAPTURED_TOWER: flag stays on stand until other team captures; new flag spawns at base
        int teamPartyValue = (ti == 0) ? 1635021873 : 1635021874;
        for (auto& ev : m_replayCtx.stocData.jumbo)
        {
            if (ev.message != "CAPTURED_TOWER") continue;
            if (ev.party_value != teamPartyValue) continue;
            m_captureEvents.push_back({ ev.time, ti });
            // New flag spawns at base (the respawned one) — at capture time
            fs.timeline.push_back({ ev.time, FlagLocationType::Base,
                                    fs.baseX, fs.baseY, fs.baseZ, -1, -1 });
        }

        // Sort the final timeline by time
        std::sort(fs.timeline.begin(), fs.timeline.end(),
                  [](const FlagEvent& a, const FlagEvent& b) {
                      return a.time < b.time;
                  });
    }

    // Sort capture events by time (only one team's flag on stand at a time)
    std::sort(m_captureEvents.begin(), m_captureEvents.end(),
              [](const std::pair<float, int>& a, const std::pair<float, int>& b) {
                  return a.first < b.first;
              });
}

// ---------------------------------------------------------------------------
// Per-frame flag state evaluation — binary search into pre-computed timeline
// ---------------------------------------------------------------------------

ReplayWindow::FlagEvent ReplayWindow::EvaluateFlagState(int teamIdx, float time) const
{
    const auto& fs = m_flagState[teamIdx];
    if (!fs.valid || fs.timeline.empty())
        return { time, FlagLocationType::Base, fs.baseX, fs.baseY, fs.baseZ, -1 };

    // Before first event → flag at base
    if (time < fs.timeline.front().time)
        return { time, FlagLocationType::Base, fs.baseX, fs.baseY, fs.baseZ, -1 };

    // Binary search for the latest event with time <= current
    int lo = 0, hi = static_cast<int>(fs.timeline.size()) - 1;
    while (lo < hi)
    {
        int mid = lo + (hi - lo + 1) / 2;
        if (fs.timeline[mid].time <= time)
            lo = mid;
        else
            hi = mid - 1;
    }
    return fs.timeline[lo];
}

void ReplayWindow::DrawAgentOverlay()
{
    if (!m_showAgentOverlay) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    Camera* cam = m_mapRenderer->GetCamera();
    XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    auto vp = m_deviceResources->GetScreenViewport();
    float vpW = vp.Width, vpH = vp.Height;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float dotRadius = 6.f;
    const float labelOffY = 8.f;
    const MapTransform& t = m_replayCtx.mapTransform;

    const bool canClickAgents = !ImGui::GetIO().WantCaptureMouse
                                && !m_rightMouseDown;
    const ImVec2 mousePos = ImGui::GetIO().MousePos;
    const float clickRadius = 14.f;
    m_hoveredAgentId = -1;

    // Map boundary clamping: use terrain bounds if available.
    // Bounds are in GWMB mesh coordinates (post-transform), so we clamp after
    // applying the map transform.
    Terrain* terrain = m_mapRenderer->GetTerrain();
    bool hasBounds = (terrain != nullptr);
    float bMinX = 0, bMaxX = 0, bMinZ = 0, bMaxZ = 0;
    if (hasBounds) {
        bMinX = terrain->m_bounds.map_min_x;
        bMaxX = terrain->m_bounds.map_max_x;
        bMinZ = terrain->m_bounds.map_min_z;
        bMaxZ = terrain->m_bounds.map_max_z;
    }

    // Optional: draw origin axes
    if (m_showMapOriginAxes)
    {
        const float axisLen = 2000.f;
        struct { XMFLOAT3 end; ImU32 col; } axes[] = {
            { { axisLen, 0, 0 }, IM_COL32(255, 60, 60, 200) },
            { { 0, axisLen, 0 }, IM_COL32(60, 255, 60, 200) },
            { { 0, 0, axisLen }, IM_COL32(60, 100, 255, 200) },
        };
        float ox, oy;
        if (ProjectToScreen(viewProj, vpW, vpH, { 0, 0, 0 }, ox, oy))
        {
            for (auto& a : axes) {
                float ax, ay;
                if (ProjectToScreen(viewProj, vpW, vpH, a.end, ax, ay))
                    dl->AddLine(ImVec2(ox, oy), ImVec2(ax, ay), a.col, 2.f);
            }
        }
    }

    const InterpolationSettings& is = m_replayCtx.interpSettings;

    // --- Spirit overlap pass: determine which spirits are hidden ---
    // Group spirits by (team, model_id). Within each group, only the newest
    // spirit is unconditionally visible; older ones are hidden if they are
    // within 2.7 × the spirit's danger-zone radius of the newest.
    {
        struct SpiritEntry { int agentId; float spawnTime; float px, py; };
        // Key: (teamId << 32) | modelId
        std::unordered_map<uint64_t, std::vector<SpiritEntry>> groups;

        for (int id : m_spiritIds)
        {
            auto& ard = m_replayCtx.agents[id];
            ard.overlapHidden     = false;
            ard.overlapIsNewest   = false;
            ard.overlapDistNewest = 0.f;
            ard.overlapThreshold  = 0.f;

            if (ard.snapshots.empty()) continue;
            if (m_debugTimeline < ard.snapshots.front().time ||
                m_debugTimeline > ard.snapshots.back().time)
                continue;

            float sx, sy, sz;
            SnapAgentPosition(ard, m_debugTimeline, sx, sy, sz);

            uint64_t key = (static_cast<uint64_t>(ard.teamId) << 32) | ard.modelId;
            groups[key].push_back({ id, ard.snapshots.front().time, sx, sy });
        }

        for (auto& [key, entries] : groups)
        {
            if (entries.size() <= 1) {
                if (!entries.empty()) {
                    auto& a = m_replayCtx.agents[entries[0].agentId];
                    a.overlapIsNewest  = true;
                    a.overlapThreshold = GetSpiritOverwriteDist(a.modelId);
                }
                continue;
            }

            // Sort newest first (highest spawnTime)
            std::sort(entries.begin(), entries.end(),
                      [](const SpiritEntry& a, const SpiritEntry& b) {
                          return a.spawnTime > b.spawnTime;
                      });

            auto& newest = entries[0];
            auto& newestArd = m_replayCtx.agents[newest.agentId];
            float threshold = GetSpiritOverwriteDist(newestArd.modelId);

            newestArd.overlapIsNewest  = true;
            newestArd.overlapThreshold = threshold;

            for (size_t i = 1; i < entries.size(); ++i)
            {
                auto& e = entries[i];
                auto& a = m_replayCtx.agents[e.agentId];
                float dx = e.px - newest.px;
                float dy = e.py - newest.py;
                float dist = sqrtf(dx * dx + dy * dy);

                a.overlapThreshold  = threshold;
                a.overlapDistNewest = dist;
                a.overlapHidden     = (dist < threshold);
            }
        }
    }

    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;

        // Flags are drawn by DrawFlags() using the state machine
        if (ard.type == AgentType::Flag) continue;

        // Spirits only exist within their snapshot time range
        if (ard.type == AgentType::Spirit)
        {
            if (m_debugTimeline < ard.snapshots.front().time ||
                m_debugTimeline > ard.snapshots.back().time)
                continue;
        }

        // Spirit visibility: hide overwritten, dead, or not-alive spirits immediately
        if (ard.type == AgentType::Spirit)
        {
            if (ard.overlapHidden) continue;
            if (ard.isDeadAtTime(m_debugTimeline) || !ard.isAliveAtTime(m_debugTimeline))
                continue;
        }

        float sx, sy, sz;
        InterpolateAgentPosition(ard, m_debugTimeline, is, sx, sy, sz);

        // Optional: show raw axis-remapped position (no transform) — from calibration panel
        if (m_showRawPositions) {
            XMFLOAT3 rawPos = { sx, sz, sy };
            float rsx, rsy;
            if (ProjectToScreen(viewProj, vpW, vpH, rawPos, rsx, rsy))
                dl->AddCircle(ImVec2(rsx, rsy), 3.f, IM_COL32(255, 255, 0, 120), 0, 1.f);
        }

        // Optional: show raw snapshot position as grey dot (from interp panel)
        float rawScrX = 0.f, rawScrY = 0.f;
        bool rawOnScreen = false;
        if (is.showRawSnapshots && ard.type != AgentType::Flag && ard.type != AgentType::Spirit)
        {
            float rx, ry, rz;
            SnapAgentPosition(ard, m_debugTimeline, rx, ry, rz);
            XMFLOAT3 rawPos = ApplyMapTransformToPos(rx, ry, rz, t);
            rawOnScreen = ProjectToScreen(viewProj, vpW, vpH, rawPos, rawScrX, rawScrY);
            if (rawOnScreen)
                dl->AddCircleFilled(ImVec2(rawScrX, rawScrY), 3.f, IM_COL32(160, 160, 160, 180));
        }

        // Optional: draw MOVE_TO_POINT anchors (yellow diamond + line from snap)
        if (is.showMoveAnchors && !ard.moveEvents.empty() &&
            ard.type != AgentType::Flag && ard.type != AgentType::Spirit)
        {
            int moveIdx = FindMoveEventIndex(ard.moveEvents, m_debugTimeline);
            if (moveIdx >= 0) {
                auto& move = ard.moveEvents[moveIdx];
                XMFLOAT3 mpos = ApplyMapTransformToPos(move.targetX, move.targetY, 0.f, t);
                float msx, msy;
                if (ProjectToScreen(viewProj, vpW, vpH, mpos, msx, msy)) {
                    dl->AddCircleFilled(ImVec2(msx, msy), 4.f, IM_COL32(255, 255, 0, 200));
                    float snapScrX, snapScrY;
                    float rx, ry, rz;
                    SnapAgentPosition(ard, m_debugTimeline, rx, ry, rz);
                    XMFLOAT3 spos = ApplyMapTransformToPos(rx, ry, rz, t);
                    if (ProjectToScreen(viewProj, vpW, vpH, spos, snapScrX, snapScrY))
                        dl->AddLine(ImVec2(snapScrX, snapScrY), ImVec2(msx, msy),
                                    IM_COL32(255, 255, 0, 100), 1.f);
                }
            }
        }

        XMFLOAT3 pos = ApplyMapTransformToPos(sx, sy, sz, t);

        // Clamp to map boundaries to prevent out-of-bounds drift
        if (hasBounds) {
            if (pos.x < bMinX) pos.x = bMinX;
            if (pos.x > bMaxX) pos.x = bMaxX;
            if (pos.z < bMinZ) pos.z = bMinZ;
            if (pos.z > bMaxZ) pos.z = bMaxZ;
        }

        float scrX, scrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, pos, scrX, scrY)) continue;

        // Debug line between raw snapshot and interpolated position
        if (is.showInterpolatedLine && rawOnScreen &&
            ard.type != AgentType::Flag && ard.type != AgentType::Spirit)
            dl->AddLine(ImVec2(rawScrX, rawScrY), ImVec2(scrX, scrY),
                        IM_COL32(255, 255, 255, 80), 1.f);

        bool casting = ard.isCastingAtTime(m_debugTimeline);
        bool dead    = ard.isDeadAtTime(m_debugTimeline);

        ImU32 dotColor;
        if (ard.type == AgentType::Spirit)
            dotColor = IM_COL32(0x80, 0xFF, 0x80, 0xFF);      // light green
        else if (ard.type == AgentType::Item)
            dotColor = IM_COL32(0xFF, 0xA5, 0x00, 0xFF);      // orange
        else
            dotColor = GetAgentTeamColor(ard.teamId);
        dl->AddCircleFilled(ImVec2(scrX, scrY), dotRadius, dotColor);
        dl->AddCircle(ImVec2(scrX, scrY), dotRadius, IM_COL32(0, 0, 0, 180), 0, 1.5f);

        // Dead freeze indicator: black X over the dot
        if (is.showDeadFreeze && dead &&
            ard.type != AgentType::Flag && ard.type != AgentType::Spirit)
        {
            float r = dotRadius + 2.f;
            dl->AddLine(ImVec2(scrX - r, scrY - r), ImVec2(scrX + r, scrY + r),
                        IM_COL32(0, 0, 0, 240), 2.f);
            dl->AddLine(ImVec2(scrX + r, scrY - r), ImVec2(scrX - r, scrY + r),
                        IM_COL32(0, 0, 0, 240), 2.f);
        }

        // Casting freeze indicator: purple ring around frozen agents
        if (is.showCastingFreeze && casting && !dead &&
            ard.type != AgentType::Flag && ard.type != AgentType::Spirit)
        {
            dl->AddCircle(ImVec2(scrX, scrY), dotRadius + 3.f,
                          IM_COL32(180, 60, 255, 220), 0, 2.f);
        }

        // Follow-camera highlight for the currently followed agent
        if (m_cameraMode == CameraMode::FollowAgent && agentId == m_followedAgentId)
        {
            dl->AddCircle(ImVec2(scrX, scrY), dotRadius + 5.f,
                          IM_COL32(77, 142, 240, 200), 0, 2.5f);
        }

        // Hover detection + click-to-follow (only for Players and NPCs)
        if (canClickAgents && (ard.type == AgentType::Player || ard.type == AgentType::NPC))
        {
            float dx = mousePos.x - scrX;
            float dy = mousePos.y - scrY;
            if (dx * dx + dy * dy <= clickRadius * clickRadius)
            {
                m_hoveredAgentId = agentId;
                dl->AddCircle(ImVec2(scrX, scrY), dotRadius + 4.f,
                              IM_COL32(255, 255, 255, 100), 0, 1.5f);

                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    EnterFollowMode(agentId);
            }
        }

        std::string label = GetAgentLabel(ard);
        ImVec2 textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, label.c_str());
        float lx = scrX - textSize.x * 0.5f;
        float ly = scrY + dotRadius + labelOffY;
        dl->AddText(ImVec2(lx + 1.f, ly + 1.f), IM_COL32(0, 0, 0, 200), label.c_str());
        dl->AddText(ImVec2(lx, ly), IM_COL32(255, 255, 255, 230), label.c_str());
    }
}

// Forward declarations for functions defined later in this file
static ImTextureID LoadFlagIcon(ID3D11Device* device, const char* filename);
static const AgentSnapshot* FindSnapshotAtTime(const AgentReplayData& ard, float t);

// ---------------------------------------------------------------------------
// Flag rendering — draw team-colored flag PNG icons based on state machine
// ---------------------------------------------------------------------------

void ReplayWindow::DrawFlags()
{
    if (!m_flagStateBuilt) return;
    if (!m_agentsClassified) return;

    const auto& t = m_replayCtx.mapTransform;
    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;

    auto* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x;
    float vpH = vp->Size.y;

    XMMATRIX viewProj = cam->GetView() * cam->GetProj();

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImTextureID texBlue = LoadFlagIcon(dev, "Blue_flag_waving.svg.png");
    ImTextureID texRed  = LoadFlagIcon(dev, "Red_flag_waving.svg.png");

    const float iconSz = std::clamp(vpH * 0.035f, 18.f, 32.f);

    // Helper: which team's flag is on the stand at time t, and when was it captured?
    int standTeam = -1;
    float standCaptureTime = 0.f;
    if (!m_captureEvents.empty() && m_debugTimeline >= m_captureEvents.front().first)
    {
        int lo = 0, hi = static_cast<int>(m_captureEvents.size()) - 1;
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            if (m_captureEvents[mid].first <= m_debugTimeline) lo = mid; else hi = mid - 1;
        }
        if (m_captureEvents[lo].first <= m_debugTimeline)
        {
            standTeam = m_captureEvents[lo].second;
            standCaptureTime = m_captureEvents[lo].first;
        }
    }
    if (m_flagStandFound && standTeam >= 0)
    {
        ImTextureID standTex = (standTeam == 0) ? texBlue : texRed;
        if (standTex)
        {
            XMFLOAT3 standPos = ApplyMapTransformToPos(m_flagStandX, m_flagStandY, m_flagStandZ, t);
            float standScrX, standScrY;
            if (ProjectToScreen(viewProj, vpW, vpH, standPos, standScrX, standScrY))
            {
                float offsetY = iconSz * 0.8f;
                ImVec2 iconTL(standScrX - iconSz * 0.5f, standScrY - offsetY - iconSz);
                ImVec2 iconBR(iconTL.x + iconSz, iconTL.y + iconSz);
                dl->AddImage(standTex, iconTL, iconBR);
            }
        }
    }

    for (int ti = 0; ti < 2; ti++)
    {
        if (!m_flagState[ti].valid) continue;

        ImTextureID tex = (ti == 0) ? texBlue : texRed;
        if (!tex) continue;

        // 2. Draw active flag (Base / Carried / Ground / Stand)
        FlagEvent ev = EvaluateFlagState(ti, m_debugTimeline);
        float worldX = ev.x, worldY = ev.y, worldZ = ev.z;
        bool isCarried = false;

        // Check if any same-team player has weapon_type == 0 (carrying the flag).
        {
            const std::vector<int>* teamPlayers = (ti == 0) ? &m_team1PlayerIds : &m_team2PlayerIds;
            int carrierId = -1;
            float carrierX = 0, carrierY = 0, carrierZ = 0;
            for (int pid : *teamPlayers)
            {
                auto it = m_replayCtx.agents.find(pid);
                if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) continue;
                const auto& pard = it->second;
                const AgentSnapshot* psnap = FindSnapshotAtTime(pard, m_debugTimeline);
                if (!psnap || psnap->weapon_type != 0 || psnap->is_dead) continue;

                // After this team captures (jumbo CAPTURED_TOWER), skip carrier
                // detection for 5s — the player just delivered, weapon_type lags
                if (standTeam == ti && (m_debugTimeline - standCaptureTime) < 5.f)
                    continue;

                float cx, cy, cz;
                InterpolateAgentPosition(pard, m_debugTimeline,
                                         m_replayCtx.interpSettings, cx, cy, cz);
                carrierId = pid;
                carrierX = cx; carrierY = cy; carrierZ = cz;
                break;
            }
            if (carrierId >= 0)
            {
                worldX = carrierX;
                worldY = carrierY;
                worldZ = carrierZ;
                isCarried = true;
            }
        }

        // If not carried, use the flag agent's live position when available
        if (!isCarried && ev.flagAgentId >= 0)
        {
            auto it = m_replayCtx.agents.find(ev.flagAgentId);
            if (it != m_replayCtx.agents.end() && !it->second.snapshots.empty())
            {
                const auto& fard = it->second;
                float curT = m_debugTimeline;
                if (curT >= fard.snapshots.front().time && curT <= fard.snapshots.back().time)
                {
                    float fx, fy, fz;
                    SnapAgentPosition(fard, curT, fx, fy, fz);
                    worldX = fx;
                    worldY = fy;
                    worldZ = fz;
                }
                else
                {
                    // Flag agent snapshots ended — find the drop location by
                    // detecting the weapon_type transition: 0 → non-0 means drop.
                    const std::vector<int>* teamPlayers = (ti == 0) ? &m_team1PlayerIds : &m_team2PlayerIds;
                    float bestDropTime = -1.f;
                    for (int pid : *teamPlayers)
                    {
                        auto pit = m_replayCtx.agents.find(pid);
                        if (pit == m_replayCtx.agents.end() || pit->second.snapshots.empty()) continue;
                        const auto& pard = pit->second;
                        const AgentSnapshot* psnap = FindSnapshotAtTime(pard, m_debugTimeline);
                        if (!psnap || psnap->is_dead) continue;
                        if (psnap->weapon_type == 0) continue; // still carrying, not a drop
                        int idx = static_cast<int>(psnap - &pard.snapshots[0]);
                        for (int k = idx - 1; k >= 0 && pard.snapshots[k].time > m_debugTimeline - 15.f; --k)
                        {
                            if (pard.snapshots[k].weapon_type == 0 && !pard.snapshots[k].is_dead)
                            {
                                // k+1 is the first snapshot with weapon back = drop moment
                                float dropT = pard.snapshots[k + 1].time;
                                if (dropT > bestDropTime)
                                {
                                    bestDropTime = dropT;
                                    worldX = pard.snapshots[k + 1].x;
                                    worldY = pard.snapshots[k + 1].y;
                                    worldZ = pard.snapshots[k + 1].z;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        // Same fallback when no flag agent at all (event position is stale)
        else if (!isCarried && ev.flagAgentId < 0 && ev.location != FlagLocationType::Base)
        {
            const std::vector<int>* teamPlayers = (ti == 0) ? &m_team1PlayerIds : &m_team2PlayerIds;
            float bestDropTime = -1.f;
            for (int pid : *teamPlayers)
            {
                auto pit = m_replayCtx.agents.find(pid);
                if (pit == m_replayCtx.agents.end() || pit->second.snapshots.empty()) continue;
                const auto& pard = pit->second;
                const AgentSnapshot* psnap = FindSnapshotAtTime(pard, m_debugTimeline);
                if (!psnap || psnap->is_dead) continue;
                if (psnap->weapon_type == 0) continue; // still carrying
                int idx = static_cast<int>(psnap - &pard.snapshots[0]);
                for (int k = idx - 1; k >= 0 && pard.snapshots[k].time > m_debugTimeline - 15.f; --k)
                {
                    if (pard.snapshots[k].weapon_type == 0 && !pard.snapshots[k].is_dead)
                    {
                        float dropT = pard.snapshots[k + 1].time;
                        if (dropT > bestDropTime)
                        {
                            bestDropTime = dropT;
                            worldX = pard.snapshots[k + 1].x;
                            worldY = pard.snapshots[k + 1].y;
                            worldZ = pard.snapshots[k + 1].z;
                        }
                        break;
                    }
                }
            }
        }

        XMFLOAT3 pos = ApplyMapTransformToPos(worldX, worldY, worldZ, t);

        float scrX, scrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, pos, scrX, scrY)) continue;

        // Draw the flag icon centered above the position
        float offsetY = iconSz * 0.8f;
        ImVec2 iconTL(scrX - iconSz * 0.5f, scrY - offsetY - iconSz);
        ImVec2 iconBR(iconTL.x + iconSz, iconTL.y + iconSz);
        dl->AddImage(tex, iconTL, iconBR);

        // Only show a label for non-obvious states (Carried / Dropped)
        const char* locLabel = nullptr;
        if (isCarried)
            locLabel = "Flag (Carried)";
        else if (ev.location == FlagLocationType::Ground)
            locLabel = "Flag (Dropped)";

        if (locLabel)
        {
            ImFont* font = ImGui::GetFont();
            ImVec2 textSz = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, locLabel);
            float tx = scrX - textSz.x * 0.5f;
            float ty = iconBR.y + 2.f;
            dl->AddText(ImVec2(tx + 1.f, ty + 1.f), IM_COL32(0, 0, 0, 200), locLabel);
            dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 230), locLabel);
        }
    }
}

// ---------------------------------------------------------------------------
// Follow-agent camera
// ---------------------------------------------------------------------------

void ReplayWindow::EnterFollowMode(int agentId)
{
    auto it = m_replayCtx.agents.find(agentId);
    if (it == m_replayCtx.agents.end()) return;

    const auto& ard = it->second;
    if (ard.snapshots.empty()) return;

    Camera* cam = m_mapRenderer->GetCamera();
    XMFLOAT3 camPos = cam->GetPosition3f();

    float sx, sy, sz;
    InterpolateAgentPosition(ard, m_debugTimeline, m_replayCtx.interpSettings, sx, sy, sz);
    XMFLOAT3 agentWorld = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);

    float dx = camPos.x - agentWorld.x;
    float dy = camPos.y - agentWorld.y;
    float dz = camPos.z - agentWorld.z;

    m_followDist = sqrtf(dx * dx + dy * dy + dz * dz);
    m_followYaw  = atan2f(dx, dz);
    m_followPitch = (m_followDist > 0.001f) ? asinf(std::clamp(dy / m_followDist, -1.f, 1.f)) : 0.3f;
    m_followDistTarget = std::clamp(m_followDist * kFollowZoomFactor, kFollowMinDist, kFollowMaxDist);

    m_followedAgentId = agentId;
    m_cameraMode = CameraMode::FollowAgent;
    m_mapRenderer->m_disableMovementInput = true;
    m_leftClickPending = false;
}

void ReplayWindow::ExitFollowMode()
{
    m_cameraMode = CameraMode::Free;
    m_followedAgentId = -1;
    m_mapRenderer->m_disableMovementInput = false;
}

void ReplayWindow::UpdateFollowCamera(float dt)
{
    if (m_cameraMode != CameraMode::FollowAgent) return;

    auto it = m_replayCtx.agents.find(m_followedAgentId);
    if (it == m_replayCtx.agents.end()) { ExitFollowMode(); return; }

    const auto& ard = it->second;
    if (ard.snapshots.empty()) { ExitFollowMode(); return; }

    if (ard.isDeadAtTime(m_debugTimeline) &&
        m_debugTimeline > ard.snapshots.back().time)
    {
        ExitFollowMode();
        return;
    }

    // Smooth zoom toward target distance
    float t = 1.0f - expf(-kFollowLerpSpeed * dt);
    m_followDist += (m_followDistTarget - m_followDist) * t;
    m_followDist = std::clamp(m_followDist, kFollowMinDist, kFollowMaxDist);

    float sx, sy, sz;
    InterpolateAgentPosition(ard, m_debugTimeline, m_replayCtx.interpSettings, sx, sy, sz);
    XMFLOAT3 agentWorld = ApplyMapTransformToPos(sx, sy, sz, m_replayCtx.mapTransform);

    // Spherical offset from agent
    float cosP = cosf(m_followPitch);
    float offX = m_followDist * sinf(m_followYaw) * cosP;
    float offY = m_followDist * sinf(m_followPitch);
    float offZ = m_followDist * cosf(m_followYaw) * cosP;

    // Set camera position and orientation BEFORE MapRenderer::Update()
    // so the constant buffer gets the correct view/projection matrices.
    Camera* cam = m_mapRenderer->GetCamera();
    cam->SetPosition(agentWorld.x + offX, agentWorld.y + offY, agentWorld.z + offZ);

    // Camera looks from offset toward agent: orientation is the inverse of the orbit angles
    cam->SetOrientation(-m_followPitch, m_followYaw + XM_PI);
}

// ---------------------------------------------------------------------------
// Debug window: Map Calibration
// ---------------------------------------------------------------------------

void ReplayWindow::DrawMapCalibrationWindow()
{
    ImGui::SetNextWindowSize(ImVec2(400, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Map Calibration", &m_showMapCalibrationWindow))
    {
        ImGui::End();
        return;
    }

    MapTransform& t = m_replayCtx.mapTransform;
    ImGui::Text("Map ID: %d", m_replayCtx.mapId);
    ImGui::TextDisabled("Base remap: x=snap.x  y=snap.z(height)  z=snap.y");
    ImGui::Separator();

    ImGui::Text("1. Axis Swaps");
    ImGui::Checkbox("Swap Y / Z", &t.swapYZ);
    ImGui::SameLine();
    ImGui::Checkbox("Swap X / Z", &t.swapXZ);
    ImGui::SameLine();
    ImGui::Checkbox("Swap X / Y", &t.swapXY);
    ImGui::Separator();

    ImGui::Text("2. Axis Flips");
    ImGui::Checkbox("Flip X", &t.flipX);
    ImGui::SameLine();
    ImGui::Checkbox("Flip Y", &t.flipY);
    ImGui::SameLine();
    ImGui::Checkbox("Flip Z", &t.flipZ);
    ImGui::Separator();

    ImGui::Text("3. Rotation (Y axis)");
    ImGui::SliderFloat("Rotation", &t.rotationDegrees, 0.f, 360.f, "%.1f deg");
    ImGui::Separator();

    ImGui::Text("4. Offset");
    ImGui::DragFloat("Offset X", &t.offsetX, 10.f, -100000.f, 100000.f, "%.1f");
    ImGui::DragFloat("Offset Y", &t.offsetY, 10.f, -100000.f, 100000.f, "%.1f");
    ImGui::DragFloat("Offset Z", &t.offsetZ, 10.f, -100000.f, 100000.f, "%.1f");
    ImGui::Separator();

    ImGui::Text("5. Scale");
    ImGui::DragFloat("Scale X", &t.scaleX, 0.005f, 0.01f, 10.f, "%.4f");
    ImGui::DragFloat("Scale Y", &t.scaleY, 0.005f, 0.01f, 10.f, "%.4f");
    ImGui::DragFloat("Scale Z", &t.scaleZ, 0.005f, 0.01f, 10.f, "%.4f");
    ImGui::Separator();

    ImGui::Text("Visualization");
    ImGui::Checkbox("Show raw positions", &m_showRawPositions);
    ImGui::SameLine();
    ImGui::Checkbox("Show origin axes", &m_showMapOriginAxes);
    ImGui::Separator();

    if (ImGui::Button("Reset (identity)"))
        t = {};

    ImGui::SameLine();
    if (ImGui::Button("Reset (default)"))
        t = GetDefaultMapTransform();

    ImGui::SameLine();
    if (ImGui::Button("Save"))
        SaveMapTransform(m_replayCtx.mapId, t);

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Debug window: Interpolation Settings
// ---------------------------------------------------------------------------

void ReplayWindow::DrawInterpolationWindow()
{
    ImGui::SetNextWindowSize(ImVec2(340, 280), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Interpolation", &m_showInterpolationWindow))
    {
        ImGui::End();
        return;
    }

    InterpolationSettings& s = m_replayCtx.interpSettings;

    ImGui::Checkbox("Enable Interpolation", &s.enabled);
    if (!s.enabled) ImGui::TextDisabled("(all agents snap to nearest snapshot)");
    ImGui::Separator();

    ImGui::Text("Mode");
    int mode = static_cast<int>(s.mode);
    ImGui::RadioButton("Original (Linear)", &mode, 0);
    ImGui::RadioButton("Improved (MOVE_TO_POINT Aware)", &mode, 1);
    s.mode = static_cast<InterpolationMode>(mode);
    ImGui::Separator();

    bool improved = (s.mode == InterpolationMode::Improved);
    if (!improved) ImGui::BeginDisabled();

    ImGui::Text("Improved Mode Settings");
    ImGui::SliderFloat("Gap Threshold", &s.gapThreshold, 0.1f, 2.0f, "%.2f s");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Minimum time gap before MOVE_TO_POINT prediction is used");
    ImGui::SliderFloat("Velocity Influence", &s.velocityInfluence, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Blending weight between linear and MOVE_TO_POINT prediction");

    if (!improved) ImGui::EndDisabled();
    ImGui::Separator();

    ImGui::Text("Movement Freeze Rules");
    ImGui::TextDisabled("Agents freeze when casting or dead.");
    ImGui::TextDisabled("Applies to both interpolation modes.");
    ImGui::Checkbox("Show Casting Freeze", &s.showCastingFreeze);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Purple ring around agents that are currently casting");
    ImGui::Checkbox("Show Dead Freeze", &s.showDeadFreeze);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Black X over agents that are dead");
    ImGui::Separator();

    ImGui::Text("Visualization");
    ImGui::Checkbox("Show raw snapshot positions", &s.showRawSnapshots);
    ImGui::Checkbox("Show interp debug lines", &s.showInterpolatedLine);
    ImGui::Checkbox("Show MOVE_TO_POINT anchors", &s.showMoveAnchors);
    ImGui::Separator();

    const char* modeLabel = (s.mode == InterpolationMode::OriginalLinear)
        ? "Original (Linear)" : "Improved (MOVE_TO_POINT)";
    ImGui::TextDisabled("Active: %s  |  %s",
                        modeLabel, s.enabled ? "ON" : "OFF");

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Shortcut Preferences modal
// ---------------------------------------------------------------------------

static bool HotkeyInput(const char* label, int* key)
{
    ImGui::Text("%s", label);
    ImGui::SameLine(200);

    char buf[64];
    if (*key != 0)
        snprintf(buf, sizeof(buf), "%s", ImGui::GetKeyName((ImGuiKey)*key));
    else
        snprintf(buf, sizeof(buf), "Press a key...");

    ImGui::PushID(label);
    ImGui::Button(buf, ImVec2(150, 0));

    if (ImGui::IsItemActive())
    {
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
        {
            if (ImGui::IsKeyPressed((ImGuiKey)k))
            {
                *key = k;
                ImGui::PopID();
                return true;
            }
        }
    }
    ImGui::PopID();
    return false;
}

void ReplayWindow::DrawShortcutPreferences()
{
    ImGui::SetNextWindowSize(ImVec2(420, 260), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Shortcut Preferences", nullptr, ImGuiWindowFlags_NoResize))
        return;

    static ReplayHotkeys editing;
    static bool needsInit = true;
    if (needsInit) { editing = ReplayHotkeys::Get(); needsInit = false; }

    ImGui::Text("Replay Controls");
    ImGui::Separator();
    ImGui::Spacing();

    HotkeyInput("Rewind 5 seconds",  &editing.rewind5s);
    HotkeyInput("Forward 5 seconds", &editing.forward5s);
    HotkeyInput("Play / Pause",      &editing.playPause);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Save", ImVec2(120, 0)))
    {
        ReplayHotkeys::Get() = editing;
        ReplayHotkeys::Get().Save();
        needsInit = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
    {
        needsInit = true;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Timeline Controller — fixed bottom playback bar
// Styled to match the GW Observer design system:
//   bg1 #111213  bg2 #161718  bg3 #1c1d1e  bg4 #212324
//   line #252627  line2 #2e2f30  line3 #3a3b3c
//   t1 #e2e3e4  t2 #909294  t3 #55575a  t4 #363739
//   acc #4d8ef0
// ---------------------------------------------------------------------------

static std::string GetSvgIconBasePath()
{
    static std::string cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "timebar_UI"))
        {
            cached = (dir / "Textures" / "timebar_UI").string();
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path().string();
    return cached;
}

// Rasterize an SVG to a white-on-transparent RGBA texture for use on a dark bar.
// Cached after first load.
static std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> s_svgIconCache;
static ID3D11Device* s_svgIconCacheDevice = nullptr;

static ImTextureID LoadSvgIcon(ID3D11Device* device, const char* filename, int rasterSize = 64)
{
    // Invalidate cache when the device changes (new replay window)
    if (device != s_svgIconCacheDevice)
    {
        s_svgIconCache.clear();
        s_svgIconCacheDevice = device;
    }

    auto it = s_svgIconCache.find(filename);
    if (it != s_svgIconCache.end())
        return (ImTextureID)it->second.Get();

    if (!device) return nullptr;

    std::string fullPath = GetSvgIconBasePath() + "\\" + filename;
    if (!std::filesystem::exists(fullPath)) return nullptr;

    NSVGimage* svg = nsvgParseFromFile(fullPath.c_str(), "px", 96.0f);
    if (!svg) return nullptr;

    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(svg); return nullptr; }

    float scale = (float)rasterSize / std::max(svg->width, svg->height);
    int w = rasterSize;
    int h = rasterSize;

    std::vector<uint8_t> rgba(w * h * 4, 0);
    nsvgRasterize(rast, svg, 0, 0, scale, rgba.data(), w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);

    // Recolor: any visible pixel → white, preserving its alpha.
    // The original SVG fill is dark (#1C274C), but we want white icons on dark bg.
    for (int i = 0; i < w * h; i++)
    {
        uint8_t a = rgba[i * 4 + 3];
        if (a > 0)
        {
            rgba[i * 4 + 0] = 255;
            rgba[i * 4 + 1] = 255;
            rgba[i * 4 + 2] = 255;
        }
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width     = w;
    texDesc.Height    = h;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage     = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem     = rgba.data();
    initData.SysMemPitch = w * 4;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, &srv);
    if (FAILED(hr)) return nullptr;

    s_svgIconCache[filename].Attach(srv);
    return (ImTextureID)srv;
}

static void FormatTime(float seconds, char* buf, size_t bufSize)
{
    int totalSec = static_cast<int>(seconds);
    if (totalSec < 0) totalSec = 0;
    int m = totalSec / 60;
    int s = totalSec % 60;
    snprintf(buf, bufSize, "%02d:%02d", m, s);
}

void ReplayWindow::DrawTimelineController()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float vpW = vp->Size.x;
    const float vpH = vp->Size.y;
    const float barH = std::clamp(vpH * 0.038f, 28.0f, 40.0f);

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vpH - barH));
    ImGui::SetNextWindowSize(ImVec2(vpW, barH));

    constexpr ImGuiWindowFlags kBarFlags =
        ImGuiWindowFlags_NoTitleBar      | ImGuiWindowFlags_NoResize       |
        ImGuiWindowFlags_NoMove          | ImGuiWindowFlags_NoScrollbar    |
        ImGuiWindowFlags_NoCollapse      | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground    | ImGuiWindowFlags_NoFocusOnAppearing;

    const float spacing = std::max(vpW * 0.003f, 2.0f);
    const float pad     = std::max(vpW * 0.006f, 8.0f);

    // Design system colors
    const ImU32 cBg1   = IM_COL32(17,  18,  19,  230);
    const ImU32 cLine2 = IM_COL32(46,  47,  48,  255);
    const ImU32 cBg3   = IM_COL32(28,  29,  30,  255);
    const ImU32 cT1    = IM_COL32(226, 227, 228, 255);
    const ImU32 cT2    = IM_COL32(144, 146, 148, 255);
    const ImU32 cT3    = IM_COL32(85,  87,  90,  255);
    const ImU32 cAcc   = IM_COL32(77,  142, 240, 255);
    const ImU32 cAccDim= IM_COL32(77,  142, 240, 31);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(spacing, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,  3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(6, 2));

    ImGui::PushStyleColor(ImGuiCol_Text,           ImVec4(0.56f, 0.57f, 0.58f, 1.0f));  // t2
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.09f, 0.09f, 0.09f, 1.0f));  // bg2
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.11f, 0.11f, 0.12f, 1.0f));  // bg3
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.13f, 0.14f, 0.14f, 1.0f));  // bg4
    ImGui::PushStyleColor(ImGuiCol_PopupBg,        ImVec4(0.07f, 0.07f, 0.07f, 0.97f)); // bg1
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.18f, 0.18f, 0.19f, 1.0f));  // line2
    ImGui::PushStyleColor(ImGuiCol_Header,         ImVec4(0.11f, 0.11f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  ImVec4(0.13f, 0.14f, 0.14f, 1.0f));

    if (!ImGui::Begin("PlaybackBar", nullptr, kBarFlags))
    {
        ImGui::End();
        ImGui::PopStyleColor(8);
        ImGui::PopStyleVar(4);
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wPos = ImGui::GetWindowPos();
    ImVec2 wEnd(wPos.x + vpW, wPos.y + barH);

    dl->AddRectFilled(wPos, wEnd, cBg1);
    dl->AddLine(wPos, ImVec2(wEnd.x, wPos.y), cLine2, 1.0f);

    float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
    auto& ctx  = m_replayCtx;

    static const float  speeds[]      = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
    static const char*  speedLabels[] = { "0.25x","0.5x","1x","2x","4x","8x" };
    constexpr int       speedCount    = 6;

    const float btn = barH * 0.65f;

    auto VCenter = [&](float h) {
        float curY = ImGui::GetCursorScreenPos().y;
        float offset = (barH - h) * 0.5f - (curY - wPos.y);
        if (offset > 0.f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
    };

    // SVG icon textures (rasterized to white-on-transparent, cached after first call)
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    ImTextureID texStop  = LoadSvgIcon(dev, "stop.svg");
    ImTextureID texBk30  = LoadSvgIcon(dev, "backward.svg");
    ImTextureID texBk5   = LoadSvgIcon(dev, "rewind-5-seconds-svgrepo-com.svg");
    ImTextureID texPlay  = LoadSvgIcon(dev, "play.svg");
    ImTextureID texPause = LoadSvgIcon(dev, "pause.svg");
    ImTextureID texFw5   = LoadSvgIcon(dev, "forward-5-seconds-svgrepo-com.svg");
    ImTextureID texFw30  = LoadSvgIcon(dev, "forward.svg");

    // SVG icon button: InvisibleButton + AddImage + hover highlight
    auto IconButton = [&](const char* id, ImTextureID tex, float size) -> bool {
        VCenter(size);
        ImGui::InvisibleButton(id, ImVec2(size, size));
        bool clicked = ImGui::IsItemClicked();
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered())
            dl->AddRectFilled(mn, mx, cBg3, 3.0f);
        if (tex) {
            float inset = size * 0.08f;
            ImU32 tint = ImGui::IsItemHovered() ? cT1 : cT2;
            dl->AddImage(tex,
                ImVec2(mn.x + inset, mn.y + inset),
                ImVec2(mx.x - inset, mx.y - inset),
                ImVec2(0, 0), ImVec2(1, 1), tint);
        }
        return clicked;
    };

    // --- 1. Stop ---
    if (IconButton("##Stop", texStop, btn)) { m_debugTimeline = 0.0f; ctx.isPlaying = false; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop");
    ImGui::SameLine();

    // --- 2. Back 30s ---
    if (IconButton("##Bk30", texBk30, btn)) m_debugTimeline = std::max(0.f, m_debugTimeline - 30.f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back 30s");
    ImGui::SameLine();

    // --- 3. Back 5s ---
    if (IconButton("##Bk5", texBk5, btn)) m_debugTimeline = std::max(0.f, m_debugTimeline - 5.f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back 5s");
    ImGui::SameLine();

    // --- 4. Play / Pause ---
    if (IconButton("##PP", ctx.isPlaying ? texPause : texPlay, btn)) ctx.isPlaying = !ctx.isPlaying;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(ctx.isPlaying ? "Pause" : "Play");
    ImGui::SameLine();

    // --- 5. Forward 5s ---
    if (IconButton("##Fw5", texFw5, btn)) m_debugTimeline = std::min(maxT, m_debugTimeline + 5.f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward 5s");
    ImGui::SameLine();

    // --- 6. Forward 30s ---
    if (IconButton("##Fw30", texFw30, btn)) m_debugTimeline = std::min(maxT, m_debugTimeline + 30.f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forward 30s");
    ImGui::SameLine();

    // --- 7. Speed dropdown ---
    {
        float fh = ImGui::GetFrameHeight();
        VCenter(fh);
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("0.25x").x + 28.0f);
        if (ImGui::Combo("##Speed", &ctx.speedIndex, speedLabels, speedCount))
            ctx.playbackSpeed = speeds[ctx.speedIndex];
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback speed");
    }
    ImGui::SameLine();

    // --- 8. Loop checkbox ---
    {
        float cbH = ImGui::GetFrameHeight();
        float boxSz = cbH * 0.60f;
        float totalW = boxSz + 5.0f + ImGui::CalcTextSize("Loop").x;
        VCenter(cbH);
        ImGui::InvisibleButton("##Loop", ImVec2(totalW, cbH));
        if (ImGui::IsItemClicked()) ctx.loopPlayback = !ctx.loopPlayback;
        if (ImGui::IsItemHovered()) {
            ImVec2 mn2 = ImGui::GetItemRectMin();
            ImVec2 mx2 = ImGui::GetItemRectMax();
            dl->AddRectFilled(mn2, mx2, cBg3, 3.0f);
        }

        ImVec2 mn = ImGui::GetItemRectMin();
        float bx = mn.x;
        float by = mn.y + (cbH - boxSz) * 0.5f;

        ImU32 boxCol = ImGui::IsItemHovered() ? cT2 : cT3;
        dl->AddRect(ImVec2(bx, by), ImVec2(bx + boxSz, by + boxSz), boxCol, 2.0f, 0, 1.2f);

        if (ctx.loopPlayback) {
            ImU32 chk = ctx.loopPlayback && ImGui::IsItemHovered() ? cAcc : cT2;
            float m = boxSz * 0.22f;
            dl->AddLine(ImVec2(bx + m, by + boxSz * 0.52f), ImVec2(bx + boxSz * 0.40f, by + boxSz - m), chk, 1.5f);
            dl->AddLine(ImVec2(bx + boxSz * 0.40f, by + boxSz - m), ImVec2(bx + boxSz - m, by + m), chk, 1.5f);
        }

        float textY = mn.y + (cbH - ImGui::GetFontSize()) * 0.5f;
        dl->AddText(ImVec2(bx + boxSz + 5.0f, textY), cT3, "Loop");
    }
    ImGui::SameLine();

    // --- 9. Time label ---
    {
        char curBuf[16], totBuf[16];
        FormatTime(m_debugTimeline, curBuf, sizeof(curBuf));
        FormatTime(maxT,            totBuf, sizeof(totBuf));
        char timeBuf[40];
        snprintf(timeBuf, sizeof(timeBuf), "%s / %s", curBuf, totBuf);

        float tw = ImGui::CalcTextSize(timeBuf).x;
        float th = ImGui::GetFontSize();
        VCenter(th);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        dl->AddText(pos, cT2, timeBuf);
        ImGui::Dummy(ImVec2(tw, th));
    }
    ImGui::SameLine();

    // --- 10. Timeline scrubber ---
    {
        float sliderW = ImGui::GetContentRegionAvail().x;
        if (sliderW < 30.f) sliderW = 30.f;

        const float trackH  = 2.0f;
        const float handleR = barH * 0.11f;

        VCenter(btn);
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##TimelineScrub", ImVec2(sliderW, btn));
        bool active = ImGui::IsItemActive();

        if (active)
        {
            float mouseX = ImGui::GetIO().MousePos.x;
            float t = (mouseX - cursor.x) / sliderW;
            m_debugTimeline = std::clamp(t, 0.0f, 1.0f) * maxT;
        }

        float progress = maxT > 0.f ? std::clamp(m_debugTimeline / maxT, 0.0f, 1.0f) : 0.0f;

        float trackY = cursor.y + btn * 0.5f - trackH * 0.5f;
        ImVec2 trackMin(cursor.x, trackY);
        ImVec2 trackMax(cursor.x + sliderW, trackY + trackH);

        dl->AddRectFilled(trackMin, trackMax, cLine2, 1.0f);

        ImVec2 fillMax(trackMin.x + sliderW * progress, trackMax.y);
        dl->AddRectFilled(trackMin, fillMax, IM_COL32(77, 142, 240, 128), 1.0f);

        float hx = cursor.x + sliderW * progress;
        float hy = cursor.y + btn * 0.5f;
        dl->AddCircleFilled(ImVec2(hx, hy), handleR, cAcc);
        dl->AddCircle(ImVec2(hx, hy), handleR, IM_COL32(17, 18, 19, 200), 0, 1.5f);
    }

    m_debugTimeline = std::clamp(m_debugTimeline, 0.f, maxT);


    ImGui::End();
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(4);
}

// ---------------------------------------------------------------------------
// Debug window: Agent Data Viewer
// ---------------------------------------------------------------------------

static const char* GetWeaponTypeName(uint16_t wt)
{
    switch (wt) {
    case 1:  return "Bow";
    case 2:  return "Axe";
    case 3:  return "Hammer";
    case 4:  return "Daggers";
    case 5:  return "Scythe";
    case 6:  return "Spear";
    case 7:  return "Sword";
    case 8:  return "Staff";
    case 10: return "Wand";
    default: return "Unknown";
    }
}

static const char* GetTeamName(uint8_t tid)
{
    switch (tid) {
    case 0: return "None";
    case 1: return "Blue";
    case 2: return "Red";
    case 3: return "Yellow";
    default: return "?";
    }
}

static const char* GetDaggerStatusName(uint8_t ds)
{
    switch (ds) {
    case 0: return "None";
    case 1: return "Lead";
    case 2: return "Offhand";
    case 3: return "Dual";
    default: return "?";
    }
}

static const AgentSnapshot* FindSnapshotAtTime(const AgentReplayData& ard, float t)
{
    if (ard.snapshots.empty()) return nullptr;
    // Binary search for the last snapshot with time <= t
    int lo = 0, hi = static_cast<int>(ard.snapshots.size()) - 1;
    if (t < ard.snapshots[0].time) return &ard.snapshots[0];
    if (t >= ard.snapshots.back().time) return &ard.snapshots.back();
    while (lo < hi)
    {
        int mid = lo + (hi - lo + 1) / 2;
        if (ard.snapshots[mid].time <= t)
            lo = mid;
        else
            hi = mid - 1;
    }
    return &ard.snapshots[lo];
}

// ---------------------------------------------------------------------------
// Party Windows (Phase 5+6) — dockable health bar panels
// ---------------------------------------------------------------------------

struct Gradient5 { ImU32 c[5]; };

static constexpr Gradient5 kAliveBlue   = {{ IM_COL32(0x4A,0x6B,0xA3,0xFF), IM_COL32(0x3D,0x5F,0x98,0xFF), IM_COL32(0x30,0x5A,0x90,0xFF), IM_COL32(0x29,0x4E,0x7A,0xFF), IM_COL32(0x22,0x42,0x65,0xFF) }};
static constexpr Gradient5 kDeadBlue    = {{ IM_COL32(0x3A,0x4A,0x66,0xFF), IM_COL32(0x33,0x42,0x59,0xFF), IM_COL32(0x2C,0x3A,0x4D,0xFF), IM_COL32(0x25,0x32,0x40,0xFF), IM_COL32(0x1E,0x29,0x33,0xFF) }};
static constexpr Gradient5 kAliveRed    = {{ IM_COL32(0xCE,0x0C,0x0C,0xFF), IM_COL32(0xD6,0x34,0x34,0xFF), IM_COL32(0xD9,0x43,0x43,0xFF), IM_COL32(0xB2,0x00,0x00,0xFF), IM_COL32(0x7E,0x00,0x00,0xFF) }};
static constexpr Gradient5 kDeadRed     = {{ IM_COL32(0x47,0x1C,0x17,0xFF), IM_COL32(0x53,0x24,0x1B,0xFF), IM_COL32(0x52,0x24,0x1C,0xFF), IM_COL32(0x3C,0x19,0x14,0xFF), IM_COL32(0x2F,0x13,0x0F,0xFF) }};
static constexpr Gradient5 kDegenHex    = {{ IM_COL32(0xC9,0x47,0x9E,0xFF), IM_COL32(0xCE,0x5A,0xA8,0xFF), IM_COL32(0xD3,0x6C,0xB1,0xFF), IM_COL32(0xB5,0x33,0x8A,0xFF), IM_COL32(0x86,0x26,0x66,0xFF) }};
static constexpr Gradient5 kPoison      = {{ IM_COL32(0x7F,0x7F,0x3F,0xFF), IM_COL32(0x8B,0x8B,0x50,0xFF), IM_COL32(0x94,0x94,0x5F,0xFF), IM_COL32(0x66,0x66,0x2B,0xFF), IM_COL32(0x4E,0x4E,0x1D,0xFF) }};
static constexpr Gradient5 kBleeding    = {{ IM_COL32(0xDF,0x71,0x70,0xFF), IM_COL32(0xE1,0x7B,0x7B,0xFF), IM_COL32(0xE2,0x7E,0x7E,0xFF), IM_COL32(0xBC,0x59,0x59,0xFF), IM_COL32(0xA9,0x4F,0x50,0xFF) }};
static constexpr Gradient5 kDeepWound   = {{ IM_COL32(0x92,0x92,0x92,0xFF), IM_COL32(0x9F,0x9F,0x9F,0xFF), IM_COL32(0xAB,0xAB,0xAB,0xFF), IM_COL32(0x84,0x84,0x84,0xFF), IM_COL32(0x73,0x73,0x73,0xFF) }};

static void DrawGradientRect(ImDrawList* dl, ImVec2 tl, ImVec2 br, const Gradient5& g)
{
    float h = br.y - tl.y;
    float segH = h * 0.25f;
    for (int i = 0; i < 4; ++i)
    {
        float y0 = tl.y + segH * i;
        float y1 = (i == 3) ? br.y : (y0 + segH);
        dl->AddRectFilledMultiColor(
            ImVec2(tl.x, y0), ImVec2(br.x, y1),
            g.c[i], g.c[i], g.c[i + 1], g.c[i + 1]);
    }
}

static std::filesystem::path GetGameUIBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "Game_UI"))
        {
            cached = dir / "Textures" / "Game_UI";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "Game_UI";
    return cached;
}

static ImTextureID LoadPartyIcon(ID3D11Device* device, const char* filename)
{
    static ID3D11Device* s_cachedDevice = nullptr;
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_cachedDevice) { s_cache.clear(); s_cachedDevice = device; }

    auto it = s_cache.find(filename);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    auto fullPath = GetGameUIBasePath() / std::filesystem::path(filename);
    if (!std::filesystem::exists(fullPath)) return nullptr;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    s_cache[filename] = srv;
    return (ImTextureID)srv.Get();
}

static std::filesystem::path GetOthersUIBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "Others_UI"))
        {
            cached = dir / "Textures" / "Others_UI";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "Others_UI";
    return cached;
}

static ImTextureID LoadFlagIcon(ID3D11Device* device, const char* filename)
{
    static ID3D11Device* s_cachedDevice = nullptr;
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_cachedDevice) { s_cache.clear(); s_cachedDevice = device; }

    auto it = s_cache.find(filename);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    auto fullPath = GetOthersUIBasePath() / std::filesystem::path(filename);
    if (!std::filesystem::exists(fullPath)) return nullptr;

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::ScratchImage& src = converted.GetImageCount() > 0 ? converted : image;
    const auto* img = src.GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = static_cast<UINT>(img->width);
    texDesc.Height = static_cast<UINT>(img->height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = img->pixels;
    initData.SysMemPitch = static_cast<UINT>(img->rowPitch);

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&texDesc, &initData, tex.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf());
    if (FAILED(hr)) return nullptr;

    s_cache[filename] = srv;
    return (ImTextureID)srv.Get();
}

struct PartyIcons {
    ImTextureID weaponSpell = nullptr;
    ImTextureID enchanted   = nullptr;
    ImTextureID condition   = nullptr;
    ImTextureID hexed       = nullptr;
};

static PartyIcons LoadAllPartyIcons(ID3D11Device* dev)
{
    PartyIcons icons;
    icons.weaponSpell = LoadPartyIcon(dev, "WeaponSpell.png");
    icons.enchanted   = LoadPartyIcon(dev, "Enchanted.png");
    icons.condition   = LoadPartyIcon(dev, "Condition.png");
    icons.hexed       = LoadPartyIcon(dev, "Hexed.png");
    return icons;
}

static void DrawPartyHealthBar(
    ImDrawList* dl, ImVec2 barTL, float barW, float barH,
    const AgentSnapshot* snap, uint8_t teamId, bool isDead,
    const char* name, const PartyIcons& icons,
    int followedAgentId, int agentId)
{
    ImVec2 barBR(barTL.x + barW, barTL.y + barH);

    // Border
    bool isHovered = ImGui::IsMouseHoveringRect(barTL, barBR);
    bool isFollowed = (followedAgentId == agentId);
    ImU32 borderCol = IM_COL32(0x4E, 0x4D, 0x48, 0xFF);
    if (isFollowed)
        borderCol = IM_COL32(0xCB, 0xAA, 0x09, 0xFF);
    else if (isHovered)
        borderCol = IM_COL32(0x9A, 0x8A, 0x3E, 0xFF);

    dl->AddRect(barTL, barBR, borderCol, 0.f, 0, 1.0f);

    if (isFollowed)
        dl->AddRect(ImVec2(barTL.x - 1, barTL.y - 1), ImVec2(barBR.x + 1, barBR.y + 1),
                    IM_COL32(0xD8, 0xD0, 0x73, 0x80), 0.f, 0, 1.0f);

    // Inner area (1px inset from border)
    ImVec2 innerTL(barTL.x + 1, barTL.y + 1);
    ImVec2 innerBR(barBR.x - 1, barBR.y - 1);
    float innerW = innerBR.x - innerTL.x;
    float innerH = innerBR.y - innerTL.y;

    if (!snap) return;

    float healthPct = std::clamp(snap->health_pct, 0.f, 1.f);
    bool hasDeepWound = snap->has_deep_wound && !isDead;

    // Choose gradient by priority
    const Gradient5* fillGrad = nullptr;
    if (isDead)
        fillGrad = (teamId == 1) ? &kDeadBlue : &kDeadRed;
    else if (snap->has_degen_hex)
        fillGrad = &kDegenHex;
    else if (snap->has_poison)
        fillGrad = &kPoison;
    else if (snap->has_bleeding)
        fillGrad = &kBleeding;
    else
        fillGrad = (teamId == 1) ? &kAliveBlue : &kAliveRed;

    // Dead background fills full width
    if (isDead)
    {
        DrawGradientRect(dl, innerTL, innerBR, *fillGrad);
    }
    else
    {
        // Background: dark fill for empty portion
        const Gradient5* deadGrad = (teamId == 1) ? &kDeadBlue : &kDeadRed;
        DrawGradientRect(dl, innerTL, innerBR, *deadGrad);

        // Health fill
        float fillPct = hasDeepWound ? std::min(healthPct, 0.80f) : healthPct;
        if (fillPct > 0.f)
        {
            float fillW = innerW * fillPct;
            DrawGradientRect(dl, innerTL, ImVec2(innerTL.x + fillW, innerBR.y), *fillGrad);
        }

        // Deep wound overlay on rightmost 20%
        if (hasDeepWound)
        {
            float dwStart = innerTL.x + innerW * 0.80f;
            DrawGradientRect(dl, ImVec2(dwStart, innerTL.y), innerBR, kDeepWound);
        }
    }

    // Weapon spell icon (left side)
    if (snap->has_weapon_spell && !isDead && icons.weaponSpell)
    {
        float iconSz = std::min(innerH, 20.f);
        ImVec2 iconTL(innerTL.x + 2, innerTL.y + (innerH - iconSz) * 0.5f);
        ImVec2 iconBR(iconTL.x + iconSz, iconTL.y + iconSz);
        dl->AddImage(icons.weaponSpell, iconTL, iconBR);
    }

    // Player name (text with shadow)
    if (name && name[0])
    {
        float textOffsetX = (snap->has_weapon_spell && !isDead && icons.weaponSpell)
                            ? 24.f : 4.f;
        ImVec2 textPos(innerTL.x + textOffsetX, innerTL.y + (innerH - ImGui::GetFontSize()) * 0.5f);
        ImU32 textCol = isDead ? IM_COL32(0x80, 0x80, 0x80, 0xFF) : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
        dl->AddText(ImVec2(textPos.x + 1, textPos.y + 1), IM_COL32(0, 0, 0, 0xCC), name);
        dl->AddText(textPos, textCol, name);
    }

    // Status icons (right-aligned, hidden when dead)
    // Order right-to-left: Enchanted, Condition, Hexed
    if (!isDead)
    {
        const float iconSz = std::min(innerH - 2.f, 18.f);
        float iconX = innerBR.x - 2.f;
        float iconY = innerTL.y + (innerH - iconSz) * 0.5f;

        if (snap->has_enchantment && icons.enchanted)
        {
            iconX -= iconSz;
            dl->AddImage(icons.enchanted, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if ((snap->has_condition || snap->has_deep_wound || snap->has_bleeding || snap->has_poison) && icons.condition)
        {
            iconX -= iconSz;
            dl->AddImage(icons.condition, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

        if (snap->has_hex && icons.hexed)
        {
            iconX -= iconSz;
            dl->AddImage(icons.hexed, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
        }
    }
}

void ReplayWindow::DrawPartyWindows()
{
    if (!m_replayCtx.agentsLoaded || !m_agentsClassified) return;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    PartyIcons icons = LoadAllPartyIcons(dev);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x;
    float vpH = vp->Size.y;

    constexpr float kBarHeight    = 23.f;
    constexpr float kNpcBarHeight = 17.f;
    constexpr float kBarSpacing   = 4.f;
    constexpr float kDeathGraceSec = 5.f;
    float padY = ImGui::GetStyle().WindowPadding.y;
    float titleBarH = ImGui::GetFrameHeight();
    float treeNodeH = ImGui::GetFrameHeight() + kBarSpacing;
    float curTime = m_debugTimeline;

    auto IsSpiritHidden = [&](const AgentReplayData& ard) -> bool {
        if (ard.type != AgentType::Spirit) return false;
        if (ard.overlapHidden) return true;
        if (ard.snapshots.empty()) return true;

        // Spirit outside its snapshot time range is gone
        if (curTime < ard.snapshots.front().time ||
            curTime > ard.snapshots.back().time)
            return true;

        const AgentSnapshot* snap = FindSnapshotAtTime(ard, curTime);
        if (!snap) return true;

        // Primary check: is_alive == false means the spirit no longer exists
        if (!snap->is_alive)
        {
            float goneStart = ard.notAliveTransitionTime(curTime);
            if (curTime - goneStart > kDeathGraceSec)
                return true;
        }

        // Secondary check: is_dead flag (covers explicit kills)
        if (snap->is_dead)
        {
            float deathStart = ard.deathTransitionTime(curTime);
            if (curTime - deathStart > kDeathGraceSec)
                return true;
        }

        return false;
    };

    auto DrawBars = [&](ImDrawList* dl, float availW,
                        const std::vector<int>& ids, float barH,
                        bool filterSpirits)
    {
        int n = static_cast<int>(ids.size());
        for (int i = 0; i < n; ++i)
        {
            int agentId = ids[i];
            auto it = m_replayCtx.agents.find(agentId);
            if (it == m_replayCtx.agents.end()) continue;

            const AgentReplayData& ard = it->second;
            if (filterSpirits && IsSpiritHidden(ard))
                continue;

            const AgentSnapshot* snap = FindSnapshotAtTime(ard, m_debugTimeline);
            bool isDead = snap ? snap->is_dead : false;

            ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(availW, barH));

            DrawPartyHealthBar(dl, cursor, availW, barH,
                               snap, ard.teamId, isDead,
                               ard.partyBarLabel.c_str(), icons,
                               m_followedAgentId, agentId);
        }
    };

    auto CountVisibleNpcs = [&](const std::vector<int>& npcIds) -> int {
        int count = 0;
        for (int id : npcIds)
        {
            auto it = m_replayCtx.agents.find(id);
            if (it == m_replayCtx.agents.end()) continue;
            const AgentReplayData& ard = it->second;
            if (IsSpiritHidden(ard)) continue;
            ++count;
        }
        return count;
    };

    static bool s_alliesOpenTeam1 = false;
    static bool s_alliesOpenTeam2 = false;

    auto DrawTeamPanel = [&](const char* title, bool* show,
                             const std::vector<int>& playerIds,
                             const std::vector<int>& npcIds,
                             ImVec4 bgCol, bool leftSide,
                             bool* prevAlliesOpen)
    {
        if (!*show || playerIds.empty()) return;

        int nPlayers = static_cast<int>(playerIds.size());
        bool hasNpcs = !npcIds.empty();
        float panelW = std::clamp(vpW * 0.18f, 220.f, 350.f);

        float playersH = nPlayers * kBarHeight + (nPlayers - 1) * kBarSpacing;
        float alliesHeaderH = hasNpcs ? treeNodeH + kBarSpacing : 0.f;
        float collapsedH = titleBarH + padY * 2 + playersH + alliesHeaderH + 4.f;

        if (!m_partyWindowsPositioned)
        {
            float midY = vp->Pos.y + vpH * 0.5f;
            float marginX = vpW * 0.02f;
            ImVec2 pos;
            if (leftSide)
                pos = ImVec2(vp->Pos.x + marginX, midY - collapsedH * 0.5f);
            else
                pos = ImVec2(vp->Pos.x + vpW - panelW - marginX, midY - collapsedH * 0.5f);
            ImGui::SetNextWindowPos(pos);
            ImGui::SetNextWindowSize(ImVec2(panelW, collapsedH));
        }

        ImGui::PushStyleColor(ImGuiCol_WindowBg, bgCol);
        ImGui::PushStyleColor(ImGuiCol_TitleBg,       ImVec4(0.06f, 0.06f, 0.08f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.08f, 0.08f, 0.10f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.89f, 0.71f, 1.0f));

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, kBarSpacing));

        if (ImGui::Begin(title, show))
        {
            ImGui::PopStyleColor();

            // Clamp window within viewport
            ImVec2 wPos = ImGui::GetWindowPos();
            ImVec2 wSize = ImGui::GetWindowSize();
            bool clamped = false;
            if (wPos.x < vp->Pos.x) { wPos.x = vp->Pos.x; clamped = true; }
            if (wPos.y < vp->Pos.y) { wPos.y = vp->Pos.y; clamped = true; }
            if (wPos.x + wSize.x > vp->Pos.x + vpW) { wPos.x = vp->Pos.x + vpW - wSize.x; clamped = true; }
            if (wPos.y + wSize.y > vp->Pos.y + vpH) { wPos.y = vp->Pos.y + vpH - wSize.y; clamped = true; }
            if (clamped) ImGui::SetWindowPos(wPos);

            float availW = ImGui::GetContentRegionAvail().x;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            DrawBars(dl, availW, playerIds, kBarHeight, false);

            // Allies collapsible section
            if (hasNpcs)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
                ImGui::Spacing();
                ImGui::PopStyleVar();

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.72f, 0.60f, 1.0f));
                bool alliesOpen = ImGui::TreeNodeEx("Allies",
                    ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_NoAutoOpenOnLog);
                ImGui::PopStyleColor();

                // Auto-resize on open/close transition
                if (alliesOpen != *prevAlliesOpen)
                {
                    *prevAlliesOpen = alliesOpen;
                    if (alliesOpen)
                    {
                        int visNpcs = CountVisibleNpcs(npcIds);
                        float npcsH = visNpcs * kNpcBarHeight + std::max(0, visNpcs - 1) * kBarSpacing;
                        float expandedH = collapsedH + npcsH + kBarSpacing;
                        ImGui::SetWindowSize(ImVec2(wSize.x, expandedH));
                    }
                    else
                    {
                        ImGui::SetWindowSize(ImVec2(wSize.x, collapsedH));
                    }
                }

                if (alliesOpen)
                {
                    DrawBars(dl, availW, npcIds, kNpcBarHeight, true);
                }
            }
        }
        else
        {
            ImGui::PopStyleColor();
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    };

    DrawTeamPanel(m_team1GuildHeader.c_str(), &m_showTeam1Party, m_team1PlayerIds,
                  m_team1NpcIds,
                  ImVec4(11.f/255.f, 8.f/255.f, 38.f/255.f, 0.10f), true,
                  &s_alliesOpenTeam1);

    DrawTeamPanel(m_team2GuildHeader.c_str(), &m_showTeam2Party, m_team2PlayerIds,
                  m_team2NpcIds,
                  ImVec4(44.f/255.f, 8.f/255.f, 5.f/255.f, 0.10f), false,
                  &s_alliesOpenTeam2);

    if (!m_partyWindowsPositioned)
        m_partyWindowsPositioned = true;
}

void ReplayWindow::DrawAgentDataWindow()
{
    if (!m_replayCtx.agentsLoaded || m_replayCtx.agents.empty())
    {
        ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Agent Data", &m_showAgentDataWindow))
        {
            if (!m_replayCtx.agentsLoaded)
                ImGui::TextWrapped("Agent data is still loading...");
            else
                ImGui::TextWrapped("No agent data found in the match folder.");
        }
        ImGui::End();
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(960, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Agent Data", &m_showAgentDataWindow))
    {
        ImGui::End();
        return;
    }

    // ---- Top bar: timeline + stats ----
    float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
    char curBuf[16], totBuf[16];
    FormatTime(m_debugTimeline, curBuf, sizeof(curBuf));
    FormatTime(maxT, totBuf, sizeof(totBuf));
    ImGui::Text("%s / %s", curBuf, totBuf);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##timeline", &m_debugTimeline, 0.f, maxT, ""))
        m_replayCtx.isPlaying = false;

    ImGui::Text("Players:%d  Flags:%d  NPCs:%d  Spirits:%d  Gadgets:%d  Items:%d  Unknown:%d  Total:%d",
                static_cast<int>(m_playerIds.size()),
                static_cast<int>(m_flagIds.size()),
                static_cast<int>(m_npcIds.size()),
                static_cast<int>(m_spiritIds.size()),
                static_cast<int>(m_gadgetIds.size()),
                static_cast<int>(m_itemIds.size()),
                static_cast<int>(m_unknownIds.size()),
                static_cast<int>(m_replayCtx.agents.size()));

    ImGui::Checkbox("Parsed View", &m_showParsedView);
    ImGui::SameLine();
    ImGui::TextDisabled("(uncheck for raw text)");
    ImGui::Separator();

    // ---- Left pane: categorized agent list (resizable) ----
    ImGui::BeginChild("AgentList", ImVec2(m_agentListWidth, 0), true);

    // Helper lambda to draw a selectable agent row inside a category
    auto DrawAgentEntry = [&](int agentId, const AgentReplayData& ard)
    {
        ImVec4 color(1, 1, 1, 1);
        if (ard.teamId == 1) color = ImVec4(0.4f, 0.6f, 1.0f, 1.0f);
        else if (ard.teamId == 2) color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        else if (ard.teamId == 3) color = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, color);

        std::string label;
        if (ard.type == AgentType::Player)
            label = std::format("[{}] {}", agentId, ard.playerName);
        else if (!ard.categoryName.empty() && ard.categoryName != "Unknown")
            label = std::format("[{}] {}", agentId, ard.categoryName);
        else
            label = std::format("[{}] id:{}", agentId, ard.modelId);

        if (ImGui::Selectable(label.c_str(), m_selectedAgentId == agentId))
            m_selectedAgentId = agentId;

        ImGui::PopStyleColor();
    };

    // --- PLAYERS section (grouped by team) ---
    if (!m_playerIds.empty() && ImGui::TreeNodeEx("Players", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool anyBlue = false;
        for (int id : m_playerIds)
            if (m_replayCtx.agents[id].teamId == 1) { anyBlue = true; break; }
        if (anyBlue && ImGui::TreeNodeEx("Blue Team", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (int id : m_playerIds)
                if (m_replayCtx.agents[id].teamId == 1) DrawAgentEntry(id, m_replayCtx.agents[id]);
            ImGui::TreePop();
        }

        bool anyRed = false;
        for (int id : m_playerIds)
            if (m_replayCtx.agents[id].teamId == 2) { anyRed = true; break; }
        if (anyRed && ImGui::TreeNodeEx("Red Team", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (int id : m_playerIds)
                if (m_replayCtx.agents[id].teamId == 2) DrawAgentEntry(id, m_replayCtx.agents[id]);
            ImGui::TreePop();
        }

        for (int id : m_playerIds)
            if (m_replayCtx.agents[id].teamId != 1 && m_replayCtx.agents[id].teamId != 2)
                DrawAgentEntry(id, m_replayCtx.agents[id]);

        ImGui::TreePop();
    }

    // --- Flags section ---
    if (!m_flagIds.empty() && ImGui::TreeNodeEx("Flags", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (int id : m_flagIds)
            DrawAgentEntry(id, m_replayCtx.agents[id]);
        ImGui::TreePop();
    }

    // --- NPCs section ---
    if (!m_npcIds.empty() && ImGui::TreeNodeEx("NPCs", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (int id : m_npcIds)
            DrawAgentEntry(id, m_replayCtx.agents[id]);
        ImGui::TreePop();
    }

    // --- Spirits section (grouped by team) ---
    if (!m_spiritIds.empty() && ImGui::TreeNodeEx("Spirits", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool anyT1 = false;
        for (int id : m_spiritIds)
            if (m_replayCtx.agents[id].teamId == 1) { anyT1 = true; break; }
        if (anyT1 && ImGui::TreeNodeEx("Team 1 Spirits", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (int id : m_spiritIds)
                if (m_replayCtx.agents[id].teamId == 1) DrawAgentEntry(id, m_replayCtx.agents[id]);
            ImGui::TreePop();
        }

        bool anyT2 = false;
        for (int id : m_spiritIds)
            if (m_replayCtx.agents[id].teamId == 2) { anyT2 = true; break; }
        if (anyT2 && ImGui::TreeNodeEx("Team 2 Spirits", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (int id : m_spiritIds)
                if (m_replayCtx.agents[id].teamId == 2) DrawAgentEntry(id, m_replayCtx.agents[id]);
            ImGui::TreePop();
        }

        for (int id : m_spiritIds)
            if (m_replayCtx.agents[id].teamId != 1 && m_replayCtx.agents[id].teamId != 2)
                DrawAgentEntry(id, m_replayCtx.agents[id]);

        ImGui::TreePop();
    }

    // --- Gadgets section ---
    if (!m_gadgetIds.empty() && ImGui::TreeNode("Gadgets"))
    {
        for (int id : m_gadgetIds)
            DrawAgentEntry(id, m_replayCtx.agents[id]);
        ImGui::TreePop();
    }

    // --- Items section ---
    if (!m_itemIds.empty() && ImGui::TreeNode("Items"))
    {
        for (int id : m_itemIds)
            DrawAgentEntry(id, m_replayCtx.agents[id]);
        ImGui::TreePop();
    }

    // --- Unknown section ---
    if (!m_unknownIds.empty() && ImGui::TreeNode("Unknown"))
    {
        for (int id : m_unknownIds)
            DrawAgentEntry(id, m_replayCtx.agents[id]);
        ImGui::TreePop();
    }

    ImGui::EndChild();

    // Vertical drag splitter between left and right panes
    ImGui::SameLine();
    {
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::Button("##splitter", ImVec2(4.0f, -1));
        if (ImGui::IsItemActive())
            m_agentListWidth += ImGui::GetIO().MouseDelta.x;
        m_agentListWidth = std::clamp(m_agentListWidth, 120.f, avail - 200.f);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    ImGui::SameLine();

    // ---- Right pane: agent detail ----
    ImGui::BeginChild("AgentDetail", ImVec2(0, 0), true);

    if (m_selectedAgentId >= 0 && m_replayCtx.agents.count(m_selectedAgentId))
    {
        auto& ard = m_replayCtx.agents[m_selectedAgentId];

        // Header with classification info
        ImGui::TextColored(ImVec4(1, 0.9f, 0.4f, 1), "Agent %d  [%s]",
                           ard.agent_id, AgentTypeName(ard.type));
        ImGui::SameLine();
        ImGui::Text(" |  %d snapshots  |  Model: %u  |  Team: %s (%u)",
                    static_cast<int>(ard.snapshots.size()), ard.modelId,
                    GetTeamName(ard.teamId), ard.teamId);

        if (ard.type == AgentType::Player)
        {
            ImGui::Text("Player: %s", ard.playerName.c_str());
        }
        else if (ard.type == AgentType::Spirit)
        {
            ImGui::Text("Spirit: %s  |  Skill ID: %d", ard.categoryName.c_str(), ard.spiritSkillId);
            ImGui::Text("Overlap Hidden: %s  |  Newest: %s",
                        ard.overlapHidden ? "Yes" : "No",
                        ard.overlapIsNewest ? "Yes" : "No");
            ImGui::Text("Overlap Threshold: %.0f  |  Dist to Newest: %.0f",
                        ard.overlapThreshold, ard.overlapDistNewest);
        }
        else if (ard.type == AgentType::Item)
        {
            ImGui::Text("Item: %s  |  item_id: %u", ard.categoryName.c_str(),
                        ard.snapshots.empty() ? 0u : ard.snapshots[0].item_id);
        }
        else if (ard.type == AgentType::Gadget)
        {
            ImGui::Text("Gadget: %s  |  gadget_id: %u", ard.categoryName.c_str(),
                        ard.snapshots.empty() ? 0u : ard.snapshots[0].gadget_id);
        }
        else if (!ard.categoryName.empty() && ard.categoryName != "Unknown")
        {
            ImGui::Text("Category: %s", ard.categoryName.c_str());
        }
        else
        {
            ImGui::Text("agent_model_type: 0x%X  |  model_id: %u  |  gadget_id: %u",
                        ard.agentModelType, ard.modelId,
                        ard.snapshots.empty() ? 0u : ard.snapshots[0].gadget_id);
        }

        ImGui::Separator();

        // Snapshot at current timeline
        const AgentSnapshot* snap = FindSnapshotAtTime(ard, m_debugTimeline);
        if (snap)
        {
            ImGui::Text("Snapshot at t=%.3fs:", snap->time);
            ImGui::Separator();

            if (m_showParsedView)
            {
                if (ImGui::BeginTable("SnapFields", 2,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY,
                    ImVec2(0, 260)))
                {
                    ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 200);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    auto Row = [](const char* field, const char* fmt, auto... args)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(field);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text(fmt, args...);
                    };

                    Row("Position", "%.3f, %.3f, %.3f", snap->x, snap->y, snap->z);
                    Row("Rotation", "%.3f rad", snap->rotation);
                    Row("Alive / Dead", "%s / %s", snap->is_alive ? "Yes" : "No", snap->is_dead ? "Yes" : "No");
                    Row("Health", "%.1f%%  (max %u)", snap->health_pct * 100.f, snap->max_hp);
                    Row("HP Pips", "%.3f", snap->hp_pips);
                    Row("Is Knocked", "%s", snap->is_knocked ? "Yes" : "No");
                    Row("Model ID", "%u", snap->model_id);
                    Row("Gadget ID", "%u", snap->gadget_id);
                    Row("Team", "%s (%u)", GetTeamName(snap->team_id), snap->team_id);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "--- Conditions ---");

                    Row("Condition", "%s", snap->has_condition ? "Yes" : "No");
                    Row("Deep Wound", "%s", snap->has_deep_wound ? "Yes" : "No");
                    Row("Bleeding", "%s", snap->has_bleeding ? "Yes" : "No");
                    Row("Crippled", "%s", snap->has_crippled ? "Yes" : "No");
                    Row("Blind", "%s", snap->has_blind ? "Yes" : "No");
                    Row("Poison", "%s", snap->has_poison ? "Yes" : "No");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.6f, 0.3f, 1, 1), "--- Hex/Enchant ---");

                    Row("Hex", "%s", snap->has_hex ? "Yes" : "No");
                    Row("Degen Hex", "%s", snap->has_degen_hex ? "Yes" : "No");
                    Row("Enchantment", "%s", snap->has_enchantment ? "Yes" : "No");
                    Row("Weapon Spell", "%s", snap->has_weapon_spell ? "Yes" : "No");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.3f, 1, 0.6f, 1), "--- Casting ---");

                    Row("Is Casting", "%s", snap->is_casting ? "Yes" : "No");
                    Row("Skill ID", "%u", snap->skill_id);
                    Row("Is Holding", "%s", snap->is_holding ? "Yes" : "No");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1), "--- Weapon ---");

                    Row("Weapon Type", "%s (%u)", GetWeaponTypeName(snap->weapon_type), snap->weapon_type);
                    Row("Weapon Item Type", "%u", snap->weapon_item_type);
                    Row("Offhand Item Type", "%u", snap->offhand_item_type);
                    Row("Weapon Item ID", "%u", snap->weapon_item_id);
                    Row("Offhand Item ID", "%u", snap->offhand_item_id);
                    Row("Weapon Attack Spd", "%.3f", snap->weapon_attack_speed);
                    Row("Attack Spd Mod", "%.3f", snap->attack_speed_modifier);
                    Row("Dagger Status", "%s (%u)", GetDaggerStatusName(snap->dagger_status), snap->dagger_status);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 1, 1), "--- Movement ---");

                    Row("Velocity", "%.3f, %.3f", snap->move_x, snap->move_y);
                    float speed = std::sqrtf(snap->move_x * snap->move_x + snap->move_y * snap->move_y);
                    Row("Speed", "%.1f", speed);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(1, 0.6f, 0.8f, 1), "--- Animation ---");

                    Row("Model State", "%u", snap->model_state);
                    Row("Animation Code", "%u", snap->animation_code);
                    Row("Animation ID", "%u", snap->animation_id);
                    Row("Animation Speed", "%.3f", snap->animation_speed);
                    Row("Animation Type", "%.3f", snap->animation_type);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "--- Other ---");

                    Row("Visual Effects", "%u", snap->visual_effects);
                    Row("In Spirit Range", "%u", snap->in_spirit_range);
                    Row("Agent Model Type", "0x%X", snap->agent_model_type);
                    Row("Item ID", "%u", snap->item_id);
                    Row("Item Extra Type", "%u", snap->item_extra_type);
                    Row("Gadget Extra Type", "%u", snap->gadget_extra_type);

                    ImGui::EndTable();
                }
            }
            else
            {
                ImGui::TextWrapped("Raw: %s", snap->raw_line.c_str());
            }
        }

        // Scrollable snapshot table
        ImGui::Separator();
        ImGui::Text("All Snapshots:");
        if (ImGui::BeginTable("SnapTable", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Pos X");
            ImGui::TableSetupColumn("Pos Y");
            ImGui::TableSetupColumn("HP%");
            ImGui::TableSetupColumn("Alive");
            ImGui::TableSetupColumn("Casting");
            ImGui::TableSetupColumn("Skill");
            ImGui::TableSetupColumn("Speed");
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(ard.snapshots.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& s = ard.snapshots[row];
                    ImGui::TableNextRow();

                    bool isNearTimeline = std::fabsf(s.time - m_debugTimeline) < 0.15f;
                    if (isNearTimeline)
                    {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                            ImGui::GetColorU32(ImVec4(0.3f, 0.5f, 0.8f, 0.4f)));
                    }

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%.3f", s.time);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.1f", s.x);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.1f", s.y);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.1f%%", s.health_pct * 100.f);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(s.is_alive ? "Y" : "N");
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(s.is_casting ? "Y" : "N");
                    ImGui::TableSetColumnIndex(6);
                    if (s.skill_id > 0) ImGui::Text("%u", s.skill_id);
                    else ImGui::TextUnformatted("-");
                    ImGui::TableSetColumnIndex(7);
                    float spd = std::sqrtf(s.move_x * s.move_x + s.move_y * s.move_y);
                    ImGui::Text("%.0f", spd);
                }
            }

            ImGui::EndTable();
        }
    }
    else
    {
        ImGui::TextWrapped("Select an agent from the list on the left.");
    }

    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Debug window: StoC Events Viewer
// ---------------------------------------------------------------------------

static ImU32 StoCCategoryColor(StoCCategory cat)
{
    switch (cat) {
    case StoCCategory::AgentMovement: return IM_COL32(160, 160, 160, 255);
    case StoCCategory::Skill:         return IM_COL32(80,  140, 255, 255);
    case StoCCategory::AttackSkill:   return IM_COL32(255, 165, 60,  255);
    case StoCCategory::BasicAttack:   return IM_COL32(240, 220, 60,  255);
    case StoCCategory::Combat:        return IM_COL32(255, 70,  70,  255);
    case StoCCategory::Jumbo:         return IM_COL32(180, 100, 255, 255);
    case StoCCategory::Unknown:       return IM_COL32(220, 220, 220, 255);
    default:                          return IM_COL32(255, 255, 255, 255);
    }
}

static int StoCCategoryCount(const StoCData& d, StoCCategory cat)
{
    switch (cat) {
    case StoCCategory::AgentMovement: return static_cast<int>(d.agentMovement.size());
    case StoCCategory::Skill:         return static_cast<int>(d.skill.size());
    case StoCCategory::AttackSkill:   return static_cast<int>(d.attackSkill.size());
    case StoCCategory::BasicAttack:   return static_cast<int>(d.basicAttack.size());
    case StoCCategory::Combat:        return static_cast<int>(d.combat.size());
    case StoCCategory::Jumbo:         return static_cast<int>(d.jumbo.size());
    case StoCCategory::Unknown:       return static_cast<int>(d.unknown.size());
    default: return 0;
    }
}

static std::string GetAgentDisplayName(const ReplayContext& ctx, int agentId)
{
    if (agentId <= 0)
        return std::format("Agent {} (Missing)", agentId);

    auto it = ctx.agents.find(agentId);
    if (it == ctx.agents.end())
        return std::format("Agent {} (Missing)", agentId);

    auto& ard = it->second;
    switch (ard.type) {
    case AgentType::Player: return std::format("{} (Player)", ard.playerName);
    case AgentType::NPC:    return std::format("{} (NPC)", ard.categoryName);
    case AgentType::Gadget: return std::format("{} (Gadget)", ard.categoryName);
    default:                return std::format("Agent {} (Unknown)", agentId);
    }
}

static std::string GetSkillDisplayName(int skillId)
{
    if (skillId <= 0)
        return "None";

    auto& db = GetSkillDatabase();
    if (db.IsLoaded())
    {
        const SkillInfo* si = db.Get(skillId);
        if (si && !si->name.empty())
            return si->name;
    }
    return std::format("Skill {}", skillId);
}

static int ResolveTarget(int targetId, int casterId)
{
    return (targetId == 0) ? casterId : targetId;
}

static const char* JumboPartyLabel(int partyValue)
{
    if (partyValue == 1635021873) return "Party 1";
    if (partyValue == 1635021874) return "Party 2";
    return "Unknown";
}

void ReplayWindow::DrawStoCWindow()
{
    if (!m_replayCtx.stocLoaded)
    {
        ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("StoC Events", &m_showStoCWindow))
            ImGui::TextWrapped("StoC event data is still loading...");
        ImGui::End();
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(1100, 660), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("StoC Events", &m_showStoCWindow))
    {
        ImGui::End();
        return;
    }

    auto& sd = m_replayCtx.stocData;
    float maxT = std::max(1.f, m_replayCtx.maxReplayTime);

    // ---- Event timeline bar ----
    {
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        float canvasW = ImGui::GetContentRegionAvail().x;
        float canvasH = 32.f;

        ImGui::InvisibleButton("##timeline_canvas", ImVec2(canvasW, canvasH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasW, canvasPos.y + canvasH),
                          IM_COL32(30, 30, 30, 255));
        dl->AddRect(canvasPos, ImVec2(canvasPos.x + canvasW, canvasPos.y + canvasH),
                    IM_COL32(80, 80, 80, 255));

        auto PlotEvents = [&](const auto& events, StoCCategory cat)
        {
            ImU32 col = StoCCategoryColor(cat);
            for (auto& ev : events)
            {
                float xp = canvasPos.x + (ev.time / maxT) * canvasW;
                dl->AddLine(ImVec2(xp, canvasPos.y), ImVec2(xp, canvasPos.y + canvasH), col, 1.0f);
            }
        };

        if (m_selectedStoCCategory == StoCCategory::AgentMovement || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.agentMovement, StoCCategory::AgentMovement);
        if (m_selectedStoCCategory == StoCCategory::Skill || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.skill, StoCCategory::Skill);
        if (m_selectedStoCCategory == StoCCategory::AttackSkill || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.attackSkill, StoCCategory::AttackSkill);
        if (m_selectedStoCCategory == StoCCategory::BasicAttack || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.basicAttack, StoCCategory::BasicAttack);
        if (m_selectedStoCCategory == StoCCategory::Combat || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.combat, StoCCategory::Combat);
        if (m_selectedStoCCategory == StoCCategory::Jumbo || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.jumbo, StoCCategory::Jumbo);
        if (m_selectedStoCCategory == StoCCategory::Unknown || m_selectedStoCCategory == StoCCategory::_Count)
            PlotEvents(sd.unknown, StoCCategory::Unknown);

        if (ImGui::IsItemClicked())
            m_debugTimeline = ((ImGui::GetIO().MousePos.x - canvasPos.x) / canvasW) * maxT;
    }

    ImGui::Checkbox("Show Raw", &m_stocShowRaw);
    ImGui::Separator();

    // ---- Left pane: category list ----
    ImGui::BeginChild("StoCCatList", ImVec2(m_stocListWidth, 0), true);

    for (int i = 0; i < static_cast<int>(StoCCategory::_Count); i++)
    {
        auto cat = static_cast<StoCCategory>(i);
        int count = StoCCategoryCount(sd, cat);
        ImU32 col = StoCCategoryColor(cat);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        auto label = std::format("{} ({})", StoCCategoryName(cat), count);
        if (ImGui::Selectable(label.c_str(), m_selectedStoCCategory == cat))
        {
            m_selectedStoCCategory = cat;
            m_selectedStoCEventIdx = -1;
        }
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();

    // Splitter
    ImGui::SameLine();
    {
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::Button("##stoc_splitter", ImVec2(4.0f, -1));
        if (ImGui::IsItemActive())
            m_stocListWidth += ImGui::GetIO().MouseDelta.x;
        m_stocListWidth = std::clamp(m_stocListWidth, 120.f, avail - 200.f);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    ImGui::SameLine();

    // ---- Right pane: event table ----
    ImGui::BeginChild("StoCDetail", ImVec2(0, 0), true);

    const auto& rctx = m_replayCtx;

    switch (m_selectedStoCCategory)
    {
    // ====================== AGENT MOVEMENT ======================
    case StoCCategory::AgentMovement:
    {
        ImGui::Text("Agent Movement Events: %d", static_cast<int>(sd.agentMovement.size()));
        if (ImGui::BeginTable("AMTable", 6,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",  ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Agent ID", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Agent Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("X",     ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Y",     ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Plane", ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.agentMovement.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.agentMovement[row];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##am{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", ev.agent_id);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, ev.agent_id).c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.1f", ev.x);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.1f", ev.y);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%.0f", ev.plane);
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(sd.agentMovement.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", sd.agentMovement[m_selectedStoCEventIdx].raw_line.c_str());
        }
        break;
    }

    // ====================== SKILL EVENTS ======================
    case StoCCategory::Skill:
    {
        ImGui::Text("Skill Events: %d", static_cast<int>(sd.skill.size()));
        if (ImGui::BeginTable("SKTable", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",       ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Type",        ImGuiTableColumnFlags_WidthFixed, 130);
            ImGui::TableSetupColumn("Skill",       ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Skill Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Caster",      ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Caster Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Target",      ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Target Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.skill.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.skill[row];
                    int tid = ResolveTarget(ev.target_id, ev.caster_id);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##sk{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.type.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", ev.skill_id);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(GetSkillDisplayName(ev.skill_id).c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%d", ev.caster_id);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, ev.caster_id).c_str());
                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%d", tid);
                    ImGui::TableSetColumnIndex(7);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, tid).c_str());
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(sd.skill.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", sd.skill[m_selectedStoCEventIdx].raw_line.c_str());
        }
        break;
    }

    // ====================== ATTACK SKILL EVENTS ======================
    case StoCCategory::AttackSkill:
    {
        ImGui::Text("Attack Skill Events: %d", static_cast<int>(sd.attackSkill.size()));
        if (ImGui::BeginTable("ASKTable", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",       ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Type",        ImGuiTableColumnFlags_WidthFixed, 150);
            ImGui::TableSetupColumn("Skill",       ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Skill Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Caster",      ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Caster Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Target",      ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Target Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.attackSkill.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.attackSkill[row];
                    int tid = ResolveTarget(ev.target_id, ev.caster_id);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##ask{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.type.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", ev.skill_id);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(GetSkillDisplayName(ev.skill_id).c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%d", ev.caster_id);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, ev.caster_id).c_str());
                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%d", tid);
                    ImGui::TableSetColumnIndex(7);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, tid).c_str());
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(sd.attackSkill.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", sd.attackSkill[m_selectedStoCEventIdx].raw_line.c_str());
        }
        break;
    }

    // ====================== BASIC ATTACK EVENTS ======================
    case StoCCategory::BasicAttack:
    {
        ImGui::Text("Basic Attack Events: %d", static_cast<int>(sd.basicAttack.size()));
        if (ImGui::BeginTable("BATable", 6,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",        ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Type",         ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Caster",       ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Caster Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Target",       ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Target Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.basicAttack.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.basicAttack[row];
                    int tid = ResolveTarget(ev.target_id, ev.caster_id);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##ba{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.type.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", ev.caster_id);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, ev.caster_id).c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%d", tid);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, tid).c_str());
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(sd.basicAttack.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", sd.basicAttack[m_selectedStoCEventIdx].raw_line.c_str());
        }
        break;
    }

    // ====================== COMBAT EVENTS ======================
    case StoCCategory::Combat:
    {
        ImGui::Text("Combat Events: %d", static_cast<int>(sd.combat.size()));
        if (ImGui::BeginTable("CMTable", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",        ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Type",         ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Caster",       ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Caster Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Target",       ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Target Name",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Value",        ImGuiTableColumnFlags_WidthFixed, 65);
            ImGui::TableSetupColumn("Dmg Type",     ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.combat.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.combat[row];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##cm{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.type.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", ev.caster_id);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, ev.caster_id).c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%d", ev.target_id);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(GetAgentDisplayName(rctx, ev.target_id).c_str());
                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%.2f", ev.value);
                    ImGui::TableSetColumnIndex(7);
                    ImGui::Text("%d", ev.damage_type);
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(sd.combat.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", sd.combat[m_selectedStoCEventIdx].raw_line.c_str());
        }
        break;
    }

    // ====================== JUMBO MESSAGES ======================
    case StoCCategory::Jumbo:
    {
        ImGui::Text("Jumbo Messages: %d", static_cast<int>(sd.jumbo.size()));
        if (ImGui::BeginTable("JMBTable", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",        ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Message",      ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Party Value",   ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Party",         ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.jumbo.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.jumbo[row];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##jmb{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.message.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%d", ev.party_value);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(JumboPartyLabel(ev.party_value));
                }
            }
            ImGui::EndTable();
        }
        if (m_stocShowRaw && m_selectedStoCEventIdx >= 0 &&
            m_selectedStoCEventIdx < static_cast<int>(sd.jumbo.size()))
        {
            ImGui::Separator();
            ImGui::TextWrapped("Raw: %s", sd.jumbo[m_selectedStoCEventIdx].raw_line.c_str());
        }
        break;
    }

    // ====================== UNKNOWN EVENTS ======================
    case StoCCategory::Unknown:
    {
        ImGui::Text("Unknown Events: %d", static_cast<int>(sd.unknown.size()));
        if (ImGui::BeginTable("UNKTable", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Raw Line", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(sd.unknown.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
                {
                    auto& ev = sd.unknown[row];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Selectable(std::format("{:.1f}##unk{}", ev.time, row).c_str(),
                                          m_selectedStoCEventIdx == row,
                                          ImGuiSelectableFlags_SpanAllColumns))
                        m_selectedStoCEventIdx = row;
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(ev.raw_line.c_str());
                }
            }
            ImGui::EndTable();
        }
        break;
    }

    default:
        ImGui::TextWrapped("Select a category from the left.");
        break;
    }

    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------

void ReplayWindow::Clear()
{
    auto* context = m_deviceResources->GetD3DDeviceContext();
    auto* rtv     = m_deviceResources->GetRenderTargetView();
    auto* dsv     = m_deviceResources->GetDepthStencilView();

    const auto& clearColor = m_mapRenderer->GetClearColor();
    float color[4] = { clearColor.x, clearColor.y, clearColor.z, clearColor.w };
    context->ClearRenderTargetView(rtv, color);
    context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);

    auto vp = m_deviceResources->GetScreenViewport();
    context->RSSetViewports(1, &vp);
}

// ---------------------------------------------------------------------------
// IDeviceNotify
// ---------------------------------------------------------------------------

void ReplayWindow::OnDeviceLost()   {}
void ReplayWindow::OnDeviceRestored() {}

// ---------------------------------------------------------------------------
// Window sizing
// ---------------------------------------------------------------------------

void ReplayWindow::OnWindowSizeChanged(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (!m_deviceResources) return;

    if (m_deviceResources->WindowSizeChanged(width, height))
    {
        if (m_mapRenderer)
            m_mapRenderer->OnViewPortChanged(static_cast<float>(width), static_cast<float>(height));
    }
}

void ReplayWindow::OnDestroy()
{
    m_alive = false;
}

// ---------------------------------------------------------------------------
// Phase 2+ stub
// ---------------------------------------------------------------------------

void ReplayWindow::LoadReplayData(const std::filesystem::path& matchFolderPath)
{
    m_replayCtx.matchFolderPath = matchFolderPath;
}

// ---------------------------------------------------------------------------
// Win32 message handler
// ---------------------------------------------------------------------------

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK ReplayWindow::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* rw = reinterpret_cast<ReplayWindow*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    // Forward to ImGui if initialized (save/restore main app context).
    // Split capture checks: keyboard only blocks when a text input widget is
    // active (WantTextInput), so WASD camera movement works even when an ImGui
    // window (e.g. Agent Offset) has focus. Mouse is blocked only when the
    // cursor is over an ImGui window (WantCaptureMouse).
    bool imguiCaptureMouse = false;
    bool imguiCaptureKeys  = false;
    if (rw && rw->m_imguiInitialized)
    {
        ImGuiContext* prevCtx = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(rw->m_imguiContext);

        if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
        {
            ImGui::SetCurrentContext(prevCtx);
            return true;
        }

        imguiCaptureMouse = ImGui::GetIO().WantCaptureMouse;
        imguiCaptureKeys  = ImGui::GetIO().WantTextInput;
        ImGui::SetCurrentContext(prevCtx);
    }

    bool isReady = rw && rw->m_loadingPhase == LoadingPhase::Ready;
    bool keyAllowed   = isReady && !imguiCaptureKeys;
    bool mouseAllowed = isReady && !imguiCaptureMouse;

    switch (message)
    {
    case WM_KEYDOWN:
        if (keyAllowed && rw->m_inputManager)
            rw->m_inputManager->OnKeyDown(wParam, hWnd);
        break;

    case WM_KEYUP:
        if (keyAllowed && rw->m_inputManager)
            rw->m_inputManager->OnKeyUp(wParam, hWnd);
        break;

    case WM_INPUT:
        if (mouseAllowed && rw->m_mapRenderer)
        {
            bool dragging = rw->m_rightMouseDown || rw->m_leftMouseDown;
            if (dragging)
            {
                UINT dwSize = sizeof(RAWINPUT);
                BYTE lpb[sizeof(RAWINPUT)];
                GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER));
                auto* raw = reinterpret_cast<RAWINPUT*>(lpb);
                if (raw->header.dwType == RIM_TYPEMOUSE)
                {
                    float dx = static_cast<float>(raw->data.mouse.lLastX);
                    float dy = static_cast<float>(raw->data.mouse.lLastY);
                    Camera* cam = rw->m_mapRenderer->GetCamera();

                    if (rw->m_leftMouseDown)
                    {
                        XMFLOAT3 right = cam->GetRight3f();
                        XMFLOAT3 up    = cam->GetUp3f();
                        XMFLOAT3 pos   = cam->GetPosition3f();
                        float s = rw->m_panSpeed;
                        pos.x += (-dx * right.x + dy * up.x) * s;
                        pos.y += (-dx * right.y + dy * up.y) * s;
                        pos.z += (-dx * right.z + dy * up.z) * s;
                        cam->SetPosition(pos.x, pos.y, pos.z);
                    }

                    if (rw->m_rightMouseDown)
                    {
                        float radX = DirectX::XMConvertToRadians(0.25f * dx);
                        float radY = DirectX::XMConvertToRadians(0.25f * dy);

                        if (rw->m_cameraMode == CameraMode::FollowAgent)
                        {
                            rw->m_followYaw   += radX;
                            rw->m_followPitch  = std::clamp(rw->m_followPitch + radY,
                                                            rw->kFollowMinPitch, rw->kFollowMaxPitch);
                        }
                        else
                        {
                            cam->OnMouseMove(radX, radY);
                        }
                    }
                }
                if (rw->m_leftMouseDown)
                    SetCursorPos(rw->m_mouseDragOrigin.x, rw->m_mouseDragOrigin.y);
            }
        }
        break;

    case WM_LBUTTONDOWN:
        if (mouseAllowed && rw && rw->m_cameraMode != CameraMode::FollowAgent)
        {
            rw->m_leftClickPending = true;
            GetCursorPos(&rw->m_mouseDragOrigin);
        }
        break;

    case WM_LBUTTONUP:
        if (rw)
        {
            if (rw->m_leftClickPending)
                rw->m_leftClickPending = false;

            if (rw->m_leftMouseDown)
            {
                rw->m_leftMouseDown = false;
                if (!rw->m_rightMouseDown)
                {
                    SetCursorPos(rw->m_mouseDragOrigin.x, rw->m_mouseDragOrigin.y);
                    ShowCursor(TRUE);
                    ReleaseCapture();
                }
                else
                {
                    ShowCursor(TRUE);
                    RECT clip = { rw->m_mouseDragOrigin.x, rw->m_mouseDragOrigin.y,
                                  rw->m_mouseDragOrigin.x + 1, rw->m_mouseDragOrigin.y + 1 };
                    ClipCursor(&clip);
                }
            }
        }
        break;

    case WM_RBUTTONDOWN:
        if (mouseAllowed && rw)
        {
            rw->m_rightMouseDown = true;
            if (!rw->m_leftMouseDown)
            {
                GetCursorPos(&rw->m_mouseDragOrigin);
                RECT clip = { rw->m_mouseDragOrigin.x, rw->m_mouseDragOrigin.y,
                              rw->m_mouseDragOrigin.x + 1, rw->m_mouseDragOrigin.y + 1 };
                ClipCursor(&clip);
                SetCapture(hWnd);
            }
        }
        break;

    case WM_RBUTTONUP:
        if (rw && rw->m_rightMouseDown)
        {
            rw->m_rightMouseDown = false;
            ClipCursor(nullptr);
            if (!rw->m_leftMouseDown)
                ReleaseCapture();
        }
        break;

    case WM_MBUTTONDOWN:
        if (mouseAllowed && rw->m_inputManager)
            rw->m_inputManager->OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, hWnd, message);
        break;

    case WM_MBUTTONUP:
        if (mouseAllowed && rw->m_inputManager)
            rw->m_inputManager->OnMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, hWnd, message);
        break;

    case WM_MOUSEWHEEL:
        if (mouseAllowed && rw->m_mapRenderer)
        {
            float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;

            if (rw->m_cameraMode == CameraMode::FollowAgent)
            {
                float factor = (delta > 0) ? 0.85f : 1.18f;
                rw->m_followDistTarget = std::clamp(
                    rw->m_followDistTarget * factor,
                    rw->kFollowMinDist, rw->kFollowMaxDist);
            }
            else
            {
                Camera* cam = rw->m_mapRenderer->GetCamera();
                XMFLOAT3 look = cam->GetLook3f();
                XMFLOAT3 pos  = cam->GetPosition3f();
                float z = delta * rw->m_zoomSpeed;
                pos.x += look.x * z;
                pos.y += look.y * z;
                pos.z += look.z * z;
                cam->SetPosition(pos.x, pos.y, pos.z);
            }
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && g_Cursors.loaded)
        {
            if (rw && rw->m_leftMouseDown)
                return TRUE;  // cursor hidden during left-drag pan
            if (rw && rw->m_rightMouseDown)
            {
                ::SetCursor(g_Cursors.Get(CursorMode::Precision));
                return TRUE;
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

    case WM_MOUSELEAVE:
        if (rw && rw->m_inputManager)
            rw->m_inputManager->OnMouseLeave(hWnd);
        break;

    case WM_ACTIVATE:
        if (rw && rw->m_inputManager)
        {
            if (LOWORD(wParam) != WA_INACTIVE)
            {
                rw->m_inputManager->ReRegisterRawInput();
                OutputDebugStringA("ReplayWindow: Activated (input re-attached)\n");
            }
            else
            {
                rw->m_inputManager->OnFocusLost();
                OutputDebugStringA("ReplayWindow: Deactivated (input detached)\n");
            }
        }
        break;

    case WM_SIZE:
        if (rw && wParam != SIZE_MINIMIZED)
            rw->OnWindowSizeChanged(LOWORD(lParam), HIWORD(lParam));
        break;

    case WM_DESTROY:
        if (rw) rw->OnDestroy();
        break;

    case WM_GETMINMAXINFO:
        if (lParam)
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 320;
            info->ptMinTrackSize.y = 200;
        }
        break;
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}
