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
// UI Layout persistence (JSON)
// ---------------------------------------------------------------------------

static std::filesystem::path GetUILayoutFilePath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    auto settingsDir = dir / "settings";
    if (!std::filesystem::exists(settingsDir))
        std::filesystem::create_directories(settingsDir);
    return settingsDir / "ui_layout.json";
}

void ReplayWindow::SaveUILayout()
{
    auto path = GetUILayoutFilePath();
    std::ofstream f(path);
    if (!f.is_open()) return;
    f << "{\n"
      << "  \"useCustom\": "  << (m_uiLayout.useCustom ? "true" : "false") << ",\n"
      << "  \"jumboX\": "     << m_uiLayout.jumboX   << ",\n"
      << "  \"jumboY\": "     << m_uiLayout.jumboY   << ",\n"
      << "  \"moBlueX\": "    << m_uiLayout.moBlueX  << ",\n"
      << "  \"moBlueY\": "    << m_uiLayout.moBlueY  << ",\n"
      << "  \"moRedX\": "     << m_uiLayout.moRedX   << ",\n"
      << "  \"moRedY\": "     << m_uiLayout.moRedY   << ",\n"
      << "  \"timerX\": "     << m_uiLayout.timerX   << ",\n"
      << "  \"timerY\": "     << m_uiLayout.timerY   << ",\n"
      << "  \"lodEnabled\": " << (m_uiLayout.lodEnabled ? "true" : "false") << ",\n"
      << "  \"lodDotDist\": " << m_uiLayout.lodDotDist << ",\n"
      << "  \"lodPillarDist\": " << m_uiLayout.lodPillarDist << "\n"
      << "}\n";
}

void ReplayWindow::LoadUILayout()
{
    auto path = GetUILayoutFilePath();
    std::ifstream f(path);
    if (!f.is_open()) return;
    try {
        nlohmann::json j;
        f >> j;
        if (j.contains("useCustom")) m_uiLayout.useCustom = j["useCustom"].get<bool>();
        if (j.contains("jumboX"))    m_uiLayout.jumboX   = j["jumboX"].get<float>();
        if (j.contains("jumboY"))    m_uiLayout.jumboY   = j["jumboY"].get<float>();
        if (j.contains("moBlueX"))   m_uiLayout.moBlueX  = j["moBlueX"].get<float>();
        if (j.contains("moBlueY"))   m_uiLayout.moBlueY  = j["moBlueY"].get<float>();
        if (j.contains("moRedX"))    m_uiLayout.moRedX   = j["moRedX"].get<float>();
        if (j.contains("moRedY"))    m_uiLayout.moRedY   = j["moRedY"].get<float>();
        if (j.contains("timerX"))    m_uiLayout.timerX   = j["timerX"].get<float>();
        if (j.contains("timerY"))    m_uiLayout.timerY   = j["timerY"].get<float>();
        if (j.contains("lodEnabled"))    m_uiLayout.lodEnabled    = j["lodEnabled"].get<bool>();
        if (j.contains("lodDotDist"))    m_uiLayout.lodDotDist    = j["lodDotDist"].get<float>();
        if (j.contains("lodPillarDist")) m_uiLayout.lodPillarDist = j["lodPillarDist"].get<float>();
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
    wcex.hCursor       = nullptr;
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

    // Merge symbol glyphs so arrows (U+2190-21FF) and misc symbols render
    {
        ImFontConfig mergeConfig;
        mergeConfig.MergeMode = true;
        mergeConfig.PixelSnapH = true;
        static const ImWchar symbolRanges[] = {
            0x2190, 0x21FF,   // Arrows (includes → U+2192)
            0x2500, 0x257F,   // Box Drawing
            0x25A0, 0x25FF,   // Geometric Shapes
            0x2600, 0x26FF,   // Miscellaneous Symbols (includes ⚔ U+2694)
            0, 0
        };
        const char* symbolFonts[] = {
            "C:\\Windows\\Fonts\\seguisym.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\arial.ttf",
        };
        for (const char* path : symbolFonts)
        {
            if (std::filesystem::exists(path))
            {
                io.Fonts->AddFontFromFileTTF(path, fontSize, &mergeConfig, symbolRanges);
                break;
            }
        }
    }

    // Load Lato fonts for scene overlays (timer, jumbo, morale)
    {
        std::string base = GetReplayFontBasePath();
        if (!base.empty())
        {
            std::string latoRegPath = base + "\\Lato-Regular.ttf";
            std::string latoBoldPath = base + "\\Lato-Bold.ttf";
            if (std::filesystem::exists(latoRegPath))
                m_latoRegular = io.Fonts->AddFontFromFileTTF(latoRegPath.c_str(), 19.f);
            if (std::filesystem::exists(latoBoldPath))
            {
                m_latoBold    = io.Fonts->AddFontFromFileTTF(latoBoldPath.c_str(), 18.f);
                m_latoBoldBig = io.Fonts->AddFontFromFileTTF(latoBoldPath.c_str(), 40.f);
            }
        }
    }

    io.Fonts->Build();

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_deviceResources->GetD3DDevice(),
                        m_deviceResources->GetD3DDeviceContext());

    m_imguiInitialized = true;
    LoadUILayout();

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

    // Build per-agent skill use timeline for icon display + laser lines
    if (m_castIntervalsBuilt && !m_skillUseTimelineBuilt)
    {
        auto resolveTarget = [](int tid, int cid) { return (tid == 0) ? cid : tid; };

        // Track open casts to pair ACTIVATED→FINISHED with their target
        struct OpenCast { float start; int skillId; int targetId; };
        std::unordered_map<int, OpenCast> openCasts;

        // 1) Process StoC skill events (activated, instant, finished, stopped)
        for (auto& ev : m_replayCtx.stocData.skill)
        {
            if (ev.type == "SKILL_ACTIVATED" && ev.skill_id > 0)
            {
                int tid = resolveTarget(ev.target_id, ev.caster_id);
                openCasts[ev.caster_id] = { ev.time, ev.skill_id, tid };
            }
            else if (ev.type == "SKILL_FINISHED" || ev.type == "SKILL_STOPPED")
            {
                auto oc = openCasts.find(ev.caster_id);
                if (oc != openCasts.end())
                {
                    bool stopped = (ev.type == "SKILL_STOPPED");
                    auto it = m_replayCtx.agents.find(ev.caster_id);
                    if (it != m_replayCtx.agents.end())
                        it->second.skillUseHistory.push_back(
                            { oc->second.start, ev.time, 0.f, oc->second.skillId,
                              oc->second.targetId, false, stopped });
                    openCasts.erase(oc);
                }
            }
            else if (ev.type == "INSTANT_SKILL_USED" && ev.skill_id > 0)
            {
                int tid = resolveTarget(ev.target_id, ev.caster_id);
                auto it = m_replayCtx.agents.find(ev.caster_id);
                if (it != m_replayCtx.agents.end())
                    it->second.skillUseHistory.push_back(
                        { ev.time, ev.time, 0.f, ev.skill_id, tid, true, false });
            }
        }

        // 2) Process StoC attack skill events
        openCasts.clear();
        for (auto& ev : m_replayCtx.stocData.attackSkill)
        {
            if (ev.type == "ATTACK_SKILL_ACTIVATED" && ev.skill_id > 0)
            {
                int tid = resolveTarget(ev.target_id, ev.caster_id);
                openCasts[ev.caster_id] = { ev.time, ev.skill_id, tid };
            }
            else if (ev.type == "ATTACK_SKILL_FINISHED" || ev.type == "ATTACK_SKILL_STOPPED")
            {
                bool stopped = (ev.type == "ATTACK_SKILL_STOPPED");
                auto oc = openCasts.find(ev.caster_id);
                if (oc != openCasts.end())
                {
                    auto it = m_replayCtx.agents.find(ev.caster_id);
                    if (it != m_replayCtx.agents.end())
                        it->second.skillUseHistory.push_back(
                            { oc->second.start, ev.time, 0.f, oc->second.skillId,
                              oc->second.targetId, false, stopped });
                    openCasts.erase(oc);
                }
            }
        }

        // 3) Sort each agent's timeline by startTime
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            std::sort(ard.skillUseHistory.begin(), ard.skillUseHistory.end(),
                      [](const SkillUseEvent& a, const SkillUseEvent& b) {
                          return a.startTime < b.startTime;
                      });
        }

        // 4) Compute fullCastDuration per skillId from successful (non-cancelled) casts
        std::unordered_map<int, float> skillFullDur;
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            for (auto& ev : ard.skillUseHistory)
            {
                if (ev.isInstant || ev.wasCancelled) continue;
                float d = ev.endTime - ev.startTime;
                if (d > 0.001f)
                {
                    auto it = skillFullDur.find(ev.skillId);
                    if (it == skillFullDur.end() || d > it->second)
                        skillFullDur[ev.skillId] = d;
                }
            }
        }
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            for (auto& ev : ard.skillUseHistory)
            {
                if (ev.isInstant) continue;
                auto it = skillFullDur.find(ev.skillId);
                if (it != skillFullDur.end())
                    ev.fullCastDuration = it->second;
                else
                    ev.fullCastDuration = ev.endTime - ev.startTime;
            }
        }

        m_skillUseTimelineBuilt = true;
    }

    // Build knockdown intervals from snapshot is_knocked transitions
    if (m_agentsClassified && !m_knockdownIntervalsBuilt)
    {
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            ard.knockdownIntervals.clear();
            bool wasKnocked = false;
            float kdStart = 0.f;
            for (auto& snap : ard.snapshots)
            {
                if (snap.is_knocked && !wasKnocked)
                    kdStart = snap.time;
                else if (!snap.is_knocked && wasKnocked)
                    ard.knockdownIntervals.push_back({ kdStart, snap.time });
                wasKnocked = snap.is_knocked;
            }
            if (wasKnocked && !ard.snapshots.empty())
                ard.knockdownIntervals.push_back({ kdStart, ard.snapshots.back().time });
        }
        m_knockdownIntervalsBuilt = true;
    }

    // Build combat log from merged StoC streams
    if (m_skillUseTimelineBuilt && m_replayCtx.stocLoaded && !m_combatLogBuilt)
    {
        m_combatLog.clear();

        auto findMaxHp = [&](int agentId, float t) -> uint32_t {
            auto it = m_replayCtx.agents.find(agentId);
            if (it == m_replayCtx.agents.end() || it->second.snapshots.empty()) return 0;
            auto& snaps = it->second.snapshots;
            if (t <= snaps.front().time) return snaps.front().max_hp;
            if (t >= snaps.back().time)  return snaps.back().max_hp;
            int lo = 0, hi = (int)snaps.size() - 1;
            while (lo < hi) { int mid = lo + (hi - lo + 1) / 2; if (snaps[mid].time <= t) lo = mid; else hi = mid - 1; }
            return snaps[lo].max_hp;
        };

        // Pass 1: skill events -> primary rows
        {
            struct OpenCast { float start; int skillId; int targetId; };
            std::unordered_map<int, OpenCast> open;

            for (auto& ev : m_replayCtx.stocData.skill)
            {
                if (ev.type == "SKILL_ACTIVATED" && ev.skill_id > 0)
                {
                    open[ev.caster_id] = { ev.time, ev.skill_id, ev.target_id };
                }
                else if (ev.type == "SKILL_FINISHED" || ev.type == "SKILL_STOPPED")
                {
                    auto oc = open.find(ev.caster_id);
                    if (oc != open.end())
                    {
                        CombatLogRow r;
                        r.time = oc->second.start;
                        r.casterId = ev.caster_id;
                        r.targetId = oc->second.targetId;
                        r.skillId = oc->second.skillId;
                        r.cancelled = (ev.type == "SKILL_STOPPED");
                        r.category = CombatLogCategory::Skill;
                        m_combatLog.push_back(std::move(r));
                        open.erase(oc);
                    }
                }
                else if (ev.type == "INSTANT_SKILL_USED" && ev.skill_id > 0)
                {
                    CombatLogRow r;
                    r.time = ev.time;
                    r.casterId = ev.caster_id;
                    r.targetId = ev.target_id;
                    r.skillId = ev.skill_id;
                    r.category = CombatLogCategory::Skill;
                    m_combatLog.push_back(std::move(r));
                }
            }

            open.clear();
            for (auto& ev : m_replayCtx.stocData.attackSkill)
            {
                if (ev.type == "ATTACK_SKILL_ACTIVATED" && ev.skill_id > 0)
                {
                    open[ev.caster_id] = { ev.time, ev.skill_id, ev.target_id };
                }
                else if (ev.type == "ATTACK_SKILL_FINISHED" || ev.type == "ATTACK_SKILL_STOPPED")
                {
                    auto oc = open.find(ev.caster_id);
                    if (oc != open.end())
                    {
                        CombatLogRow r;
                        r.time = oc->second.start;
                        r.casterId = ev.caster_id;
                        r.targetId = oc->second.targetId;
                        r.skillId = oc->second.skillId;
                        r.cancelled = (ev.type == "ATTACK_SKILL_STOPPED");
                        r.category = CombatLogCategory::Skill;
                        m_combatLog.push_back(std::move(r));
                        open.erase(oc);
                    }
                }
            }
        }

        // Sort skill rows from Pass 1 so backward search reliably finds
        // the most recent cast when merging damage events.
        std::sort(m_combatLog.begin(), m_combatLog.end(),
            [](const CombatLogRow& a, const CombatLogRow& b) { return a.time < b.time; });

        // Pass 2: combat events -> enrich skill rows or create standalone rows
        {
            std::vector<bool> matched(m_combatLog.size(), false);
            const int skillCount = (int)m_combatLog.size();

            for (auto& ce : m_replayCtx.stocData.combat)
            {
                if (ce.type == "DAMAGE")
                {
                    int bestIdx = -1;
                    float bestDt = 99.f;

                    for (int i = skillCount - 1; i >= 0; --i)
                    {
                        if (matched[i]) continue;
                        auto& r = m_combatLog[i];
                        if (r.category != CombatLogCategory::Skill) continue;
                        if (r.cancelled) continue;
                        if (r.casterId != ce.caster_id) continue;

                        bool targetMatch = (r.targetId == ce.target_id);
                        bool targetOpen  = (r.targetId <= 0 || r.targetId == r.casterId);
                        if (!targetMatch && !targetOpen) continue;

                        float dt = ce.time - r.time;
                        if (dt < -0.1f || dt > 1.5f) continue;

                        if (targetMatch && dt < bestDt) {
                            bestDt = dt;
                            bestIdx = i;
                        } else if (bestIdx < 0 && targetOpen && dt < bestDt) {
                            bestDt = dt;
                            bestIdx = i;
                        }
                    }

                    if (bestIdx >= 0)
                    {
                        auto& r = m_combatLog[bestIdx];
                        if (r.targetId <= 0 || r.targetId == r.casterId)
                            r.targetId = ce.target_id;
                        r.valuePct = ce.value;
                        uint32_t mhp = findMaxHp(ce.target_id, ce.time);
                        r.valueAbs = (mhp > 0) ? (int)(ce.value * mhp) : 0;
                        r.category = (ce.value < 0.f) ? CombatLogCategory::Damage
                                                      : CombatLogCategory::Heal;
                        matched[bestIdx] = true;
                    }
                    else
                    {
                        CombatLogRow r;
                        r.time = ce.time;
                        r.casterId = ce.caster_id;
                        r.targetId = ce.target_id;
                        r.valuePct = ce.value;
                        uint32_t mhp = findMaxHp(ce.target_id, ce.time);
                        r.valueAbs = (mhp > 0) ? (int)(ce.value * mhp) : 0;
                        r.category = (ce.value < 0.f) ? CombatLogCategory::Damage
                                                      : CombatLogCategory::Heal;
                        m_combatLog.push_back(std::move(r));
                    }
                }
                else if (ce.type == "INTERRUPTED")
                {
                    // ce.caster_id = the victim (whose cast was interrupted)
                    // ce.target_id = the interrupter
                    // ce.value     = skill ID of the interrupted spell
                    int interruptedSkillId = (int)ce.value;
                    bool merged = false;
                    bool passedNewerCast = false;
                    for (int i = skillCount - 1; i >= 0; --i)
                    {
                        auto& r = m_combatLog[i];
                        float dt = ce.time - r.time;
                        if (dt > 4.0f) break;  // too old, stop scanning

                        if (r.casterId != ce.caster_id) continue;
                        if (r.category != CombatLogCategory::Skill) continue;

                        // If we already passed a newer cast from this caster
                        // (cancelled or not), any older cancelled row is stale.
                        if (passedNewerCast) continue;

                        if (!r.cancelled || r.interrupted) {
                            passedNewerCast = true;
                            continue;
                        }

                        if (interruptedSkillId > 0 && r.skillId != interruptedSkillId) {
                            passedNewerCast = true;
                            continue;
                        }

                        if (dt < -0.2f || dt > 3.0f) continue;

                        r.interrupted = true;
                        r.interrupterId = ce.target_id;
                        merged = true;
                        break;
                    }
                    if (!merged)
                    {
                        CombatLogRow r;
                        r.time = ce.time;
                        r.casterId = ce.caster_id;
                        r.targetId = ce.target_id;
                        r.skillId = interruptedSkillId;
                        r.category = CombatLogCategory::Interrupt;
                        m_combatLog.push_back(std::move(r));
                    }
                }
                else if (ce.type == "KNOCKED_DOWN")
                {
                    CombatLogRow r;
                    r.time = ce.time;
                    r.casterId = ce.caster_id;
                    r.targetId = ce.target_id;
                    r.category = CombatLogCategory::KnockDown;
                    m_combatLog.push_back(std::move(r));
                }
                else
                {
                    CombatLogRow r;
                    r.time = ce.time;
                    r.casterId = ce.caster_id;
                    r.targetId = ce.target_id;
                    r.category = CombatLogCategory::Other;
                    r.eventType = ce.type;
                    m_combatLog.push_back(std::move(r));
                }
            }
        }

        // Pass 3: basic attacks
        for (auto& ev : m_replayCtx.stocData.basicAttack)
        {
            if (ev.type == "ATTACK_STARTED")
            {
                CombatLogRow r;
                r.time = ev.time;
                r.casterId = ev.caster_id;
                r.targetId = ev.target_id;
                r.category = CombatLogCategory::BasicAttack;
                m_combatLog.push_back(std::move(r));
            }
        }

        // Pass 4: death events from snapshot is_dead transitions
        for (auto& [agentId, ard] : m_replayCtx.agents)
        {
            if (ard.type != AgentType::Player) continue;
            for (size_t si = 1; si < ard.snapshots.size(); ++si)
            {
                if (ard.snapshots[si].is_dead && !ard.snapshots[si - 1].is_dead)
                {
                    CombatLogRow r;
                    r.time = ard.snapshots[si].time;
                    r.casterId = agentId;
                    r.category = CombatLogCategory::Death;
                    m_combatLog.push_back(std::move(r));
                }
            }
        }

        // Pass 5: jumbo messages (flag captures, morale boosts, etc.)
        for (auto& ev : m_replayCtx.stocData.jumbo)
        {
            CombatLogRow r;
            r.time = ev.time;
            r.jumboTeam = (ev.party_value == 1635021873) ? 1
                        : (ev.party_value == 1635021874) ? 2 : 0;
            r.eventType = ev.message;
            r.category = CombatLogCategory::Jumbo;
            m_combatLog.push_back(std::move(r));
        }

        std::sort(m_combatLog.begin(), m_combatLog.end(),
            [](const CombatLogRow& a, const CombatLogRow& b) { return a.time < b.time; });
        m_combatLogBuilt = true;
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
    if (m_inputManager)
        m_inputManager->SetSuppressKeyPolling(m_clSkillSearchFocused);
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

    DrawFogOfWar();

    DrawAgentCylinders();

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
                if (ImGui::MenuItem("Interface"))
                    m_showInterfacePrefs = true;
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

            if (ImGui::MenuItem("Distance LOD (dots/cylinders)",
                                nullptr, m_uiLayout.lodEnabled))
            {
                m_uiLayout.lodEnabled = !m_uiLayout.lodEnabled;
                SaveUILayout();
            }

            ImGui::Separator();
            ImGui::MenuItem("Skill Icons", nullptr, &m_showSkillIcons);
            ImGui::MenuItem("Skill Lasers", nullptr, &m_showSkillLasers);
            ImGui::MenuItem("Range Rings (R)", nullptr, &m_showRangeRings);
            {
                bool fogOn = (m_fogPerspective > 0);
                if (ImGui::MenuItem("Fog of War (F)", nullptr, &fogOn)) {
                    if (fogOn) m_fogPerspective = m_fogLastActive;
                    else { m_fogLastActive = m_fogPerspective; m_fogPerspective = 0; m_fogPlayerAgent = -1; }
                }
            }
            ImGui::MenuItem("Morale (M)", nullptr, &m_showMoralePanel);
            ImGui::MenuItem("Team 1 Party", nullptr, &m_showTeam1Party);
            ImGui::MenuItem("Team 2 Party", nullptr, &m_showTeam2Party);
            ImGui::Separator();
            ImGui::MenuItem("Combat Log", nullptr, &m_showCombatLog);
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

    if (m_showInterfacePrefs)
    {
        ImGui::OpenPopup("Interface Preferences");
        m_showInterfacePrefs = false;
    }
    DrawInterfacePreferences();

    DrawPartyWindows();
    DrawCombatLog();

    DrawAgentOverlay();
    DrawFlags();
    DrawSkillLasers();
    DrawRangeRings();
    DrawRangeRingToolbar();
    DrawFogOfWarToolbar();
    DrawMoralePanel();

    DrawMatchTimer();
    DrawJumboMessages();
    DrawMoraleBoostTimers();

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
    if (!ImGui::GetIO().WantCaptureKeyboard && !m_clSkillSearchFocused)
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

        if (ImGui::IsKeyPressed(ImGuiKey_R))
        {
            m_showRangeRings = !m_showRangeRings;
            if (!m_showRangeRings)
                m_ringAgentFilter = -1;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_M))
            m_showMoralePanel = !m_showMoralePanel;

        if (ImGui::IsKeyPressed(ImGuiKey_F))
        {
            if (m_fogPerspective > 0) {
                m_fogLastActive = m_fogPerspective;
                m_fogPerspective = 0;
                m_fogPlayerAgent = -1;
            } else {
                m_fogPerspective = m_fogLastActive;
            }
        }
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

// ---------------------------------------------------------------------------
// Fog of War — full-screen shader overlay
// ---------------------------------------------------------------------------

static const char kFogHLSL[] = R"(
cbuffer FogCB : register(b0)
{
    float4x4 invViewProj;
    float4 playerPos[8];
    float  compassRadius;
    float  fogOpacity;
    float  edgeSoftness;
    float  refHeight;
    float  viewportW;
    float  viewportH;
    int    playerCount;
    float  pad;
};

struct VS_OUT
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VS_OUT VSMain(uint id : SV_VertexID)
{
    VS_OUT o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
    o.uv  = uv;
    return o;
}

float4 PSMain(VS_OUT i) : SV_Target
{
    float2 ndc;
    ndc.x =  (i.pos.x / viewportW) * 2.0 - 1.0;
    ndc.y = 1.0 - (i.pos.y / viewportH) * 2.0;

    float4 nearH = mul(float4(ndc, 0, 1), invViewProj);
    float4 farH  = mul(float4(ndc, 1, 1), invViewProj);

    if (abs(nearH.w) < 1e-6 || abs(farH.w) < 1e-6)
        return float4(0, 0, 0, fogOpacity);

    float3 nearW = nearH.xyz / nearH.w;
    float3 farW  = farH.xyz / farH.w;

    float3 dir = farW - nearW;
    if (abs(dir.y) < 1e-6)
        return float4(0, 0, 0, fogOpacity);

    float t = (refHeight - nearW.y) / dir.y;
    if (t < 0)
        return float4(0, 0, 0, fogOpacity);

    float3 hit = nearW + dir * t;

    float minDist = 1e6;
    [loop] for (int p = 0; p < playerCount; ++p)
    {
        float dx = hit.x - playerPos[p].x;
        float dz = hit.z - playerPos[p].z;
        minDist = min(minDist, sqrt(dx * dx + dz * dz));
    }

    float edge = max(edgeSoftness, 0.1);
    float fog = smoothstep(compassRadius - edge, compassRadius, minDist);
    return float4(0, 0, 0, fogOpacity * saturate(fog));
}
)";

void ReplayWindow::InitFogRenderer()
{
    if (m_fogInitialized) return;
    m_fogInitialized = true;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif

    HRESULT hr = D3DCompile(kFogHLSL, sizeof(kFogHLSL), nullptr, nullptr, nullptr,
                            "VSMain", "vs_5_0", flags, 0, vsBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return;

    hr = D3DCompile(kFogHLSL, sizeof(kFogHLSL), nullptr, nullptr, nullptr,
                    "PSMain", "ps_5_0", flags, 0, psBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return;

    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_fogVS.GetAddressOf());
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_fogPS.GetAddressOf());

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(FogCBData);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&cbd, nullptr, m_fogCB.GetAddressOf());

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dev->CreateDepthStencilState(&dsd, m_fogDSS.GetAddressOf());

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, m_fogBS.GetAddressOf());

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, m_fogRS.GetAddressOf());
}

static constexpr float kFogCompassRadius = 5020.f;

void ReplayWindow::DrawFogOfWar()
{
    if (m_fogPerspective == 0) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    InitFogRenderer();
    if (!m_fogVS || !m_fogPS) return;

    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;
    XMMATRIX viewProj = cam->GetView() * cam->GetProj();

    auto dvp = m_deviceResources->GetScreenViewport();
    float vpW = dvp.Width;
    float vpH = dvp.Height;

    const MapTransform& mt = m_replayCtx.mapTransform;
    const InterpolationSettings& is = m_replayCtx.interpSettings;
    Terrain* terrain = m_mapRenderer->GetTerrain();

    FogCBData cb = {};

    XMVECTOR det;
    XMMATRIX invVP = XMMatrixInverse(&det, viewProj);
    XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(invVP));

    XMFLOAT3 origin = ApplyMapTransformToPos(0, 0, 0, mt);
    XMFLOAT3 xOff   = ApplyMapTransformToPos(kFogCompassRadius, 0, 0, mt);
    float wdx = xOff.x - origin.x, wdz = xOff.z - origin.z;
    float worldCompassRadius = sqrtf(wdx * wdx + wdz * wdz);

    constexpr float kFogEdgeSoftness = 200.f;
    XMFLOAT3 eOff = ApplyMapTransformToPos(kFogEdgeSoftness, 0, 0, mt);
    float edx = eOff.x - origin.x, edz = eOff.z - origin.z;
    float worldEdgeSoftness = sqrtf(edx * edx + edz * edz);

    float avgHeight = 0.f;
    int playerCount = 0;
    bool isPlayerMode = (m_fogPlayerAgent >= 0);

    if (isPlayerMode)
    {
        auto pit = m_replayCtx.agents.find(m_fogPlayerAgent);
        if (pit != m_replayCtx.agents.end() && !pit->second.snapshots.empty()
            && !pit->second.isDeadAtTime(m_debugTimeline))
        {
            float sx, sy, sz;
            InterpolateAgentPosition(pit->second, m_debugTimeline, is, sx, sy, sz);
            XMFLOAT3 wpos = ApplyMapTransformToPos(sx, sy, sz, mt);
            if (terrain) wpos.y = terrain->get_height_at(wpos.x, wpos.z);
            cb.playerPos[0] = { wpos.x, wpos.y, wpos.z, 0.f };
            avgHeight = wpos.y;
            playerCount = 1;
        }
    }
    else
    {
        for (auto& [agentId, ard] : m_replayCtx.agents)
        {
            if (playerCount >= 8) break;
            if (ard.teamId != m_fogPerspective) continue;
            if (ard.type != AgentType::Player) continue;
            if (ard.snapshots.empty()) continue;
            if (ard.isDeadAtTime(m_debugTimeline)) continue;

            float sx, sy, sz;
            InterpolateAgentPosition(ard, m_debugTimeline, is, sx, sy, sz);
            XMFLOAT3 wpos = ApplyMapTransformToPos(sx, sy, sz, mt);
            if (terrain) wpos.y = terrain->get_height_at(wpos.x, wpos.z);

            cb.playerPos[playerCount] = { wpos.x, wpos.y, wpos.z, 0.f };
            avgHeight += wpos.y;
            playerCount++;
        }
    }

    float fogOpacity = isPlayerMode ? 0.82f : 0.72f;

    cb.playerCount    = playerCount;
    cb.compassRadius  = worldCompassRadius;
    cb.fogOpacity     = (playerCount > 0) ? fogOpacity : 1.0f;
    cb.edgeSoftness   = worldEdgeSoftness;
    cb.refHeight      = (playerCount > 0) ? (avgHeight / playerCount) : 0.f;
    cb.viewportW      = vpW;
    cb.viewportH      = vpH;

    auto* ctx = m_deviceResources->GetD3DDeviceContext();

    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   prevRS;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> prevDSS;
    UINT prevStencilRef;
    Microsoft::WRL::ComPtr<ID3D11BlendState>        prevBS;
    FLOAT prevBF[4]; UINT prevSM;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      prevVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       prevPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>       prevIL;
    D3D11_PRIMITIVE_TOPOLOGY prevTopo;
    Microsoft::WRL::ComPtr<ID3D11Buffer> prevPSCB0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> prevVSCB0;

    ctx->RSGetState(prevRS.GetAddressOf());
    ctx->OMGetDepthStencilState(prevDSS.GetAddressOf(), &prevStencilRef);
    ctx->OMGetBlendState(prevBS.GetAddressOf(), prevBF, &prevSM);
    ctx->VSGetShader(prevVS.GetAddressOf(), nullptr, nullptr);
    ctx->PSGetShader(prevPS.GetAddressOf(), nullptr, nullptr);
    ctx->IAGetInputLayout(prevIL.GetAddressOf());
    ctx->IAGetPrimitiveTopology(&prevTopo);
    ctx->PSGetConstantBuffers(0, 1, prevPSCB0.GetAddressOf());
    ctx->VSGetConstantBuffers(0, 1, prevVSCB0.GetAddressOf());

    D3D11_MAPPED_SUBRESOURCE mapped;
    ctx->Map(m_fogCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(m_fogCB.Get(), 0);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(m_fogVS.Get(), nullptr, 0);
    ctx->PSSetShader(m_fogPS.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, m_fogCB.GetAddressOf());

    ctx->RSSetState(m_fogRS.Get());
    ctx->OMSetDepthStencilState(m_fogDSS.Get(), 0);
    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(m_fogBS.Get(), blendFactor, 0xFFFFFFFF);

    ctx->Draw(3, 0);

    ctx->RSSetState(prevRS.Get());
    ctx->OMSetDepthStencilState(prevDSS.Get(), prevStencilRef);
    ctx->OMSetBlendState(prevBS.Get(), prevBF, prevSM);
    ctx->VSSetShader(prevVS.Get(), nullptr, 0);
    ctx->PSSetShader(prevPS.Get(), nullptr, 0);
    ctx->IASetInputLayout(prevIL.Get());
    ctx->IASetPrimitiveTopology(prevTopo);
    ctx->PSSetConstantBuffers(0, 1, prevPSCB0.GetAddressOf());
    ctx->VSSetConstantBuffers(0, 1, prevVSCB0.GetAddressOf());
}

bool ReplayWindow::IsAgentInFog(int agentId) const
{
    if (m_fogPerspective == 0) return false;

    auto eit = m_replayCtx.agents.find(agentId);
    if (eit == m_replayCtx.agents.end()) return true;
    const auto& enemyArd = eit->second;

    if (m_fogPlayerAgent < 0 && enemyArd.teamId == m_fogPerspective) return false;

    float ex, ey, ez;
    InterpolateAgentPosition(enemyArd, m_debugTimeline, m_replayCtx.interpSettings, ex, ey, ez);

    if (m_fogPlayerAgent >= 0)
    {
        if (agentId == m_fogPlayerAgent) return false;
        auto pit = m_replayCtx.agents.find(m_fogPlayerAgent);
        if (pit == m_replayCtx.agents.end()) return true;
        if (pit->second.snapshots.empty() || pit->second.isDeadAtTime(m_debugTimeline))
            return true;
        float fx, fy, fz;
        InterpolateAgentPosition(pit->second, m_debugTimeline, m_replayCtx.interpSettings, fx, fy, fz);
        float dx = ex - fx, dy = ey - fy;
        return (dx * dx + dy * dy > kFogCompassRadius * kFogCompassRadius);
    }

    for (auto& [fid, fard] : m_replayCtx.agents)
    {
        if (fard.teamId != m_fogPerspective) continue;
        if (fard.type != AgentType::Player) continue;
        if (fard.snapshots.empty()) continue;
        if (fard.isDeadAtTime(m_debugTimeline)) continue;

        float fx, fy, fz;
        InterpolateAgentPosition(fard, m_debugTimeline, m_replayCtx.interpSettings, fx, fy, fz);

        float dx = ex - fx, dy = ey - fy;
        if (dx * dx + dy * dy <= kFogCompassRadius * kFogCompassRadius)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Cylinder agent marker — 3D renderer
// ---------------------------------------------------------------------------

static const char kCylinderHLSL[] = R"(
cbuffer CBPerFrame : register(b0)
{
    float4x4 gViewProj;
    float4   gCamPos;
};
cbuffer CBPerInstance : register(b1)
{
    float4x4 gWorld;
    float4   gTeamColor;
};

struct VS_IN  { float3 pos : POSITION; float3 nrm : NORMAL; float height01 : HEIGHT; };
struct VS_OUT { float4 pos : SV_Position; float3 worldPos : TEXCOORD0;
                float3 worldNrm : TEXCOORD1; float  height01 : TEXCOORD2; };

VS_OUT VSMain(VS_IN i)
{
    VS_OUT o;
    float4 wp = mul(float4(i.pos, 1), gWorld);
    o.pos      = mul(wp, gViewProj);
    o.worldPos = wp.xyz;
    o.worldNrm = normalize(mul(float4(i.nrm, 0), gWorld).xyz);
    o.height01 = i.height01;
    return o;
}

float4 PSMain(VS_OUT i) : SV_Target
{
    float3 baseColor = gTeamColor.rgb;

    // Ambient light from above (Y is up in GWMB render space)
    float3 lightDir = normalize(float3(0.2, 1.0, 0.3));
    float ndl = saturate(dot(i.worldNrm, lightDir));

    // View direction for specular on top cap
    float3 viewDir = normalize(gCamPos.xyz - i.worldPos);
    float3 halfVec = normalize(lightDir + viewDir);
    float spec = pow(saturate(dot(i.worldNrm, halfVec)), 32.0);

    // Vertical gradient: darker at bottom, brighter toward top
    float vertBright = lerp(0.55, 1.0, i.height01);

    // Facing camera brightness (body sides brighter at center)
    float facing = saturate(dot(i.worldNrm, viewDir));
    float faceBright = lerp(0.6, 1.0, facing * facing);

    // Top cap gets extra brightness + specular (Y is up)
    float isTop = saturate(i.worldNrm.y * 4.0 - 3.0);
    float topBright = lerp(1.0, 1.3, isTop);

    float3 color = baseColor * (0.35 + 0.65 * ndl) * vertBright * faceBright * topBright;
    color += spec * isTop * 0.4;

    // Base glow: additive team color emission at the very bottom
    float baseGlow = saturate(1.0 - i.height01 * 8.0) * 0.35;
    color += baseColor * baseGlow;

    return float4(saturate(color), gTeamColor.a);
}
)";

void ReplayWindow::InitCylinderRenderer()
{
    if (m_cylInitialized) return;
    m_cylInitialized = true;

    ID3D11Device* dev = m_deviceResources->GetD3DDevice();

    // --- Compile shaders ---
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif

    HRESULT hr = D3DCompile(kCylinderHLSL, sizeof(kCylinderHLSL), nullptr, nullptr, nullptr,
                            "VSMain", "vs_5_0", flags, 0, vsBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return;

    hr = D3DCompile(kCylinderHLSL, sizeof(kCylinderHLSL), nullptr, nullptr, nullptr,
                    "PSMain", "ps_5_0", flags, 0, psBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) return;

    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_cylVS.GetAddressOf());
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_cylPS.GetAddressOf());

    // --- Input layout ---
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "HEIGHT",   0, DXGI_FORMAT_R32_FLOAT,           0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    dev->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_cylIL.GetAddressOf());

    // --- Generate cylinder geometry ---
    constexpr int   SEG = 16;
    constexpr float R   = 30.f;
    constexpr float H   = 120.f;
    constexpr float PI2 = 6.2831853f;

    // Vertices: bottom ring + top ring + bottom center + top center
    // bottom ring: SEG verts, top ring: SEG verts
    // Each side quad uses 2 bottom + 2 top (shared normals per segment)
    // Plus 2 cap centers
    // Total: SEG*2 (body) + 1 (bottom center) + SEG (bottom fan) + 1 (top center) + SEG (top fan)
    // Simplify: body ring duplicated for caps to have different normals

    std::vector<CylVertex> verts;
    std::vector<uint16_t> indices;

    // Body vertices — Y is UP in GWMB render space (ApplyMapTransformToPos already converts)
    int bodyBase = 0;
    for (int i = 0; i <= SEG; ++i)
    {
        float a = (float(i) / SEG) * PI2;
        float cs = cosf(a), sn = sinf(a);
        float nx = cs, nz = sn;
        // Bottom (y=0)
        verts.push_back({ R * cs, 0.f, R * sn,  nx, 0, nz,  0.f });
        // Top (y=H)
        verts.push_back({ R * cs, H,   R * sn,  nx, 0, nz,  1.f });
    }

    // Body indices (triangle strip as quads)
    for (int i = 0; i < SEG; ++i)
    {
        int b0 = bodyBase + i * 2;
        int b1 = bodyBase + i * 2 + 1;
        int b2 = bodyBase + (i + 1) * 2;
        int b3 = bodyBase + (i + 1) * 2 + 1;
        indices.push_back((uint16_t)b0); indices.push_back((uint16_t)b1); indices.push_back((uint16_t)b2);
        indices.push_back((uint16_t)b2); indices.push_back((uint16_t)b1); indices.push_back((uint16_t)b3);
    }

    // Bottom cap (normal pointing down: -Y)
    int botCenter = (int)verts.size();
    verts.push_back({ 0, 0, 0,  0, -1, 0,  0.f });
    int botRing = (int)verts.size();
    for (int i = 0; i < SEG; ++i)
    {
        float a = (float(i) / SEG) * PI2;
        verts.push_back({ R * cosf(a), 0, R * sinf(a),  0, -1, 0,  0.f });
    }
    for (int i = 0; i < SEG; ++i)
    {
        indices.push_back((uint16_t)botCenter);
        indices.push_back((uint16_t)(botRing + (i + 1) % SEG));
        indices.push_back((uint16_t)(botRing + i));
    }

    // Top cap (normal pointing up: +Y)
    int topCenter = (int)verts.size();
    verts.push_back({ 0, H, 0,  0, 1, 0,  1.f });
    int topRing = (int)verts.size();
    for (int i = 0; i < SEG; ++i)
    {
        float a = (float(i) / SEG) * PI2;
        verts.push_back({ R * cosf(a), H, R * sinf(a),  0, 1, 0,  1.f });
    }
    for (int i = 0; i < SEG; ++i)
    {
        indices.push_back((uint16_t)topCenter);
        indices.push_back((uint16_t)(topRing + i));
        indices.push_back((uint16_t)(topRing + (i + 1) % SEG));
    }

    m_cylIndexCount = (UINT)indices.size();

    // Vertex buffer
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = (UINT)(verts.size() * sizeof(CylVertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = { verts.data(), 0, 0 };
    dev->CreateBuffer(&vbd, &vsd, m_cylVB.GetAddressOf());

    // Index buffer
    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = (UINT)(indices.size() * sizeof(uint16_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd = { indices.data(), 0, 0 };
    dev->CreateBuffer(&ibd, &isd, m_cylIB.GetAddressOf());

    // Constant buffers
    D3D11_BUFFER_DESC cbd = {};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    cbd.ByteWidth = sizeof(CylPerFrame);
    dev->CreateBuffer(&cbd, nullptr, m_cylCBFrame.GetAddressOf());

    cbd.ByteWidth = sizeof(CylPerInst);
    dev->CreateBuffer(&cbd, nullptr, m_cylCBInst.GetAddressOf());

    // Rasterizer: solid, no culling (overlay cylinders visible from all angles)
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthBias = 0;
    rd.DepthBiasClamp = 0.f;
    rd.SlopeScaledDepthBias = 0.f;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, m_cylRS.GetAddressOf());

    // Depth-stencil: depth disabled — cylinders render as 3D overlay, always visible
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dev->CreateDepthStencilState(&dsd, m_cylDSS.GetAddressOf());

    // Blend: disabled — cylinders are fully opaque
    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, m_cylBS.GetAddressOf());

    // --- Generate pillar geometry (thin cylinder for medium LOD) ---
    {
        constexpr int   P_SEG = 8;
        constexpr float P_R   = 6.f;
        constexpr float P_H   = 120.f;

        std::vector<CylVertex> pv;
        std::vector<uint16_t>  pi;

        int pBase = 0;
        for (int i = 0; i <= P_SEG; ++i)
        {
            float a = (float(i) / P_SEG) * PI2;
            float cs = cosf(a), sn = sinf(a);
            pv.push_back({ P_R * cs, 0.f,  P_R * sn,  cs, 0, sn,  0.f });
            pv.push_back({ P_R * cs, P_H,  P_R * sn,  cs, 0, sn,  1.f });
        }
        for (int i = 0; i < P_SEG; ++i)
        {
            int b0 = pBase + i * 2, b1 = b0 + 1, b2 = pBase + (i + 1) * 2, b3 = b2 + 1;
            pi.push_back((uint16_t)b0); pi.push_back((uint16_t)b1); pi.push_back((uint16_t)b2);
            pi.push_back((uint16_t)b2); pi.push_back((uint16_t)b1); pi.push_back((uint16_t)b3);
        }

        int pBotC = (int)pv.size();
        pv.push_back({ 0, 0, 0,  0, -1, 0,  0.f });
        int pBotR = (int)pv.size();
        for (int i = 0; i < P_SEG; ++i) {
            float a = (float(i) / P_SEG) * PI2;
            pv.push_back({ P_R * cosf(a), 0, P_R * sinf(a),  0, -1, 0,  0.f });
        }
        for (int i = 0; i < P_SEG; ++i) {
            pi.push_back((uint16_t)pBotC);
            pi.push_back((uint16_t)(pBotR + (i + 1) % P_SEG));
            pi.push_back((uint16_t)(pBotR + i));
        }

        int pTopC = (int)pv.size();
        pv.push_back({ 0, P_H, 0,  0, 1, 0,  1.f });
        int pTopR = (int)pv.size();
        for (int i = 0; i < P_SEG; ++i) {
            float a = (float(i) / P_SEG) * PI2;
            pv.push_back({ P_R * cosf(a), P_H, P_R * sinf(a),  0, 1, 0,  1.f });
        }
        for (int i = 0; i < P_SEG; ++i) {
            pi.push_back((uint16_t)pTopC);
            pi.push_back((uint16_t)(pTopR + i));
            pi.push_back((uint16_t)(pTopR + (i + 1) % P_SEG));
        }

        m_pillarIndexCount = (UINT)pi.size();

        D3D11_BUFFER_DESC pvbd = {};
        pvbd.ByteWidth = (UINT)(pv.size() * sizeof(CylVertex));
        pvbd.Usage = D3D11_USAGE_IMMUTABLE;
        pvbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA pvsd = { pv.data(), 0, 0 };
        dev->CreateBuffer(&pvbd, &pvsd, m_pillarVB.GetAddressOf());

        D3D11_BUFFER_DESC pibd = {};
        pibd.ByteWidth = (UINT)(pi.size() * sizeof(uint16_t));
        pibd.Usage = D3D11_USAGE_IMMUTABLE;
        pibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA pisd = { pi.data(), 0, 0 };
        dev->CreateBuffer(&pibd, &pisd, m_pillarIB.GetAddressOf());
    }
}

void ReplayWindow::DrawAgentCylinders()
{
    if (!m_showAgentOverlay) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    InitCylinderRenderer();
    if (!m_cylVS || !m_cylPS) return;

    ID3D11DeviceContext* ctx = m_deviceResources->GetD3DDeviceContext();

    // ---- Save ALL D3D11 state so we don't corrupt MapRenderer / ImGui ----
    Microsoft::WRL::ComPtr<ID3D11VertexShader>   oldVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>    oldPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout>    oldIL;
    Microsoft::WRL::ComPtr<ID3D11Buffer>         oldVB;
    Microsoft::WRL::ComPtr<ID3D11Buffer>         oldIB;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> oldRS;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> oldDSS;
    Microsoft::WRL::ComPtr<ID3D11BlendState>     oldBS;
    UINT oldStencilRef = 0;
    float oldBlendFactor[4]; UINT oldSampleMask = 0;
    UINT oldVBStride = 0, oldVBOffset = 0;
    DXGI_FORMAT oldIBFormat = DXGI_FORMAT_UNKNOWN; UINT oldIBOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Microsoft::WRL::ComPtr<ID3D11Buffer> oldVSCB0, oldVSCB1, oldPSCB0, oldPSCB1;

    ctx->VSGetShader(oldVS.GetAddressOf(), nullptr, nullptr);
    ctx->PSGetShader(oldPS.GetAddressOf(), nullptr, nullptr);
    ctx->IAGetInputLayout(oldIL.GetAddressOf());
    ctx->IAGetVertexBuffers(0, 1, oldVB.GetAddressOf(), &oldVBStride, &oldVBOffset);
    ctx->IAGetIndexBuffer(oldIB.GetAddressOf(), &oldIBFormat, &oldIBOffset);
    ctx->IAGetPrimitiveTopology(&oldTopo);
    ctx->RSGetState(oldRS.GetAddressOf());
    ctx->OMGetDepthStencilState(oldDSS.GetAddressOf(), &oldStencilRef);
    ctx->OMGetBlendState(oldBS.GetAddressOf(), oldBlendFactor, &oldSampleMask);
    ctx->VSGetConstantBuffers(0, 1, oldVSCB0.GetAddressOf());
    ctx->VSGetConstantBuffers(1, 1, oldVSCB1.GetAddressOf());
    ctx->PSGetConstantBuffers(0, 1, oldPSCB0.GetAddressOf());
    ctx->PSGetConstantBuffers(1, 1, oldPSCB1.GetAddressOf());

    // ---- Set up cylinder pipeline ----
    Camera* cam = m_mapRenderer->GetCamera();
    const MapTransform& t = m_replayCtx.mapTransform;
    const InterpolationSettings& is = m_replayCtx.interpSettings;

    XMMATRIX vp = cam->GetView() * cam->GetProj();
    XMFLOAT3 camP = cam->GetPosition3f();

    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        ctx->Map(m_cylCBFrame.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        CylPerFrame* cb = (CylPerFrame*)mapped.pData;
        XMStoreFloat4x4(&cb->viewProj, XMMatrixTranspose(vp));
        cb->camPos = { camP.x, camP.y, camP.z, 1.f };
        ctx->Unmap(m_cylCBFrame.Get(), 0);
    }

    UINT stride = sizeof(CylVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, m_cylVB.GetAddressOf(), &stride, &offset);
    ctx->IASetIndexBuffer(m_cylIB.Get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetInputLayout(m_cylIL.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(m_cylVS.Get(), nullptr, 0);
    ctx->PSSetShader(m_cylPS.Get(), nullptr, 0);

    ID3D11Buffer* cbs[] = { m_cylCBFrame.Get(), m_cylCBInst.Get() };
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetConstantBuffers(0, 2, cbs);

    ctx->RSSetState(m_cylRS.Get());
    ctx->OMSetDepthStencilState(m_cylDSS.Get(), 0);
    float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(m_cylBS.Get(), blendFactor, 0xFFFFFFFF);

    Terrain* terrain = m_mapRenderer->GetTerrain();
    bool hasBounds = (terrain != nullptr);
    float bMinX = 0, bMaxX = 0, bMinZ = 0, bMaxZ = 0;
    if (hasBounds) {
        bMinX = terrain->m_bounds.map_min_x;
        bMaxX = terrain->m_bounds.map_max_x;
        bMinZ = terrain->m_bounds.map_min_z;
        bMaxZ = terrain->m_bounds.map_max_z;
    }

    const bool lodOn = m_uiLayout.lodEnabled;
    const float lodDot = m_uiLayout.lodDotDist;

    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;
        if (ard.type != AgentType::Player && ard.type != AgentType::NPC) continue;

        float sx, sy, sz;
        InterpolateAgentPosition(ard, m_debugTimeline, is, sx, sy, sz);
        XMFLOAT3 pos = ApplyMapTransformToPos(sx, sy, sz, t);

        if (hasBounds) {
            pos.x = std::clamp(pos.x, bMinX, bMaxX);
            pos.z = std::clamp(pos.z, bMinZ, bMaxZ);
        }

        // LOD: beyond lodDot distance → dot only (skip 3D draw)
        bool isDot = false;
        if (lodOn) {
            float dx = pos.x - camP.x, dy = pos.y - camP.y, dz = pos.z - camP.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            isDot = (dist > lodDot);
        }
        ard.currentLOD = isDot ? 0 : 2;  // 0=Dot, 2=Cylinder

        if (isDot) continue;

        bool inFog = (m_fogPerspective > 0 && ard.teamId != m_fogPerspective && IsAgentInFog(agentId));
        if (inFog && !m_fogGhostMode) continue;

        bool dead = ard.isDeadAtTime(m_debugTimeline);

        float tiltDeg = dead ? 90.f : ard.knockdownTiltAtTime(m_debugTimeline);
        XMMATRIX world;
        if (tiltDeg > 0.01f)
        {
            float tiltRad = XMConvertToRadians(tiltDeg);
            world = XMMatrixRotationZ(tiltRad) * XMMatrixTranslation(pos.x, pos.y, pos.z);
        }
        else
        {
            world = XMMatrixTranslation(pos.x, pos.y, pos.z);
        }

        XMFLOAT4 color;
        if (inFog)
        {
            color = { 0.4f, 0.4f, 0.4f, 0.30f };
        }
        else if (ard.teamId == 1)  color = { 0.290f, 0.565f, 0.847f, 1.f };
        else if (ard.teamId == 2)  color = { 0.816f, 0.282f, 0.282f, 1.f };
        else                       color = { 0.7f, 0.7f, 0.7f, 1.f };

        if (dead)
        {
            float deathTime = ard.deathTransitionTime(m_debugTimeline);
            float fadeIn = std::clamp((m_debugTimeline - deathTime) / 0.25f, 0.f, 1.f);
            fadeIn = fadeIn * fadeIn * (3.f - 2.f * fadeIn);
            float lum = color.x * 0.299f + color.y * 0.587f + color.z * 0.114f;
            float desatR = lum + (color.x - lum) * 0.35f;
            float desatG = lum + (color.y - lum) * 0.35f;
            float desatB = lum + (color.z - lum) * 0.35f;
            color = { desatR * 0.45f, desatG * 0.45f, desatB * 0.45f, 0.6f * fadeIn };
        }

        {
            D3D11_MAPPED_SUBRESOURCE mapped;
            ctx->Map(m_cylCBInst.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            CylPerInst* cb = (CylPerInst*)mapped.pData;
            XMStoreFloat4x4(&cb->world, XMMatrixTranspose(world));
            cb->teamColor = color;
            ctx->Unmap(m_cylCBInst.Get(), 0);
        }
        ctx->DrawIndexed(m_cylIndexCount, 0, 0);
    }

    // ---- Restore ALL saved D3D11 state ----
    ctx->VSSetShader(oldVS.Get(), nullptr, 0);
    ctx->PSSetShader(oldPS.Get(), nullptr, 0);
    ctx->IASetInputLayout(oldIL.Get());
    ctx->IASetVertexBuffers(0, 1, oldVB.GetAddressOf(), &oldVBStride, &oldVBOffset);
    ctx->IASetIndexBuffer(oldIB.Get(), oldIBFormat, oldIBOffset);
    ctx->IASetPrimitiveTopology(oldTopo);
    ctx->RSSetState(oldRS.Get());
    ctx->OMSetDepthStencilState(oldDSS.Get(), oldStencilRef);
    ctx->OMSetBlendState(oldBS.Get(), oldBlendFactor, oldSampleMask);
    ID3D11Buffer* restoreVSCBs[] = { oldVSCB0.Get(), oldVSCB1.Get() };
    ctx->VSSetConstantBuffers(0, 2, restoreVSCBs);
    ID3D11Buffer* restorePSCBs[] = { oldPSCB0.Get(), oldPSCB1.Get() };
    ctx->PSSetConstantBuffers(0, 2, restorePSCBs);
}

// Forward declarations for icon loaders (defined later)
static ImTextureID LoadProfIcon(ID3D11Device* device, int profId);
static ImTextureID LoadProfStylized(ID3D11Device* device, int profId);
static ImTextureID LoadSkillIcon(ReplayWindow* rw, ID3D11Device* device,
                                 int skillId,
                                 std::unordered_map<int, std::string>& index,
                                 std::unordered_map<int, ComPtr<ID3D11ShaderResourceView>>& cache);
static ImTextureID LoadFlagIcon(ID3D11Device* device, const char* filename);
static std::string GetSkillDisplayName(int skillId);

// Gradient helpers (used by cast bar rendering + texture building)
struct GradStop { float pos; uint8_t r, g, b; };
static float LerpGradChannel(float a, float b, float t) { return a + (b - a) * t; }
static void SampleGradient(const GradStop* stops, int n, float t,
                            float& outR, float& outG, float& outB)
{
    const GradStop* a = &stops[0];
    const GradStop* b = &stops[n - 1];
    for (int i = 0; i < n - 1; ++i)
    {
        if (t >= stops[i].pos && t <= stops[i + 1].pos)
        { a = &stops[i]; b = &stops[i + 1]; break; }
    }
    float lt = (b->pos - a->pos > 0.0001f) ? (t - a->pos) / (b->pos - a->pos) : 0.f;
    outR = LerpGradChannel(a->r, b->r, lt);
    outG = LerpGradChannel(a->g, b->g, lt);
    outB = LerpGradChannel(a->b, b->b, lt);
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

        if (ard.type == AgentType::Flag) continue;

        if (m_fogPerspective > 0 && ard.teamId != m_fogPerspective && IsAgentInFog(agentId))
        {
            if (!m_fogGhostMode) continue;
        }

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

        // Determine if this agent has a 3D representation or needs a 2D dot
        bool is3DAgent = (ard.type == AgentType::Player || ard.type == AgentType::NPC);
        bool showDot = !is3DAgent;

        // For players/NPCs, check LOD: if Dot mode, use stylized profession icon for players
        if (is3DAgent && ard.currentLOD == 0)
            showDot = true;

        // For players with cylinders (not dot LOD), draw floating profession icon above cylinder
        if (ard.type == AgentType::Player && !showDot && ard.primaryProf >= 1)
        {
            constexpr float CYL_H = 120.f;
            XMFLOAT3 abovePos = { pos.x, pos.y + CYL_H + 60.f, pos.z };
            float abvX, abvY;
            if (ProjectToScreen(viewProj, vpW, vpH, abovePos, abvX, abvY))
            {
                ID3D11Device* dev = m_deviceResources->GetD3DDevice();
                ImTextureID profTex = LoadProfIcon(dev, ard.primaryProf);
                if (profTex)
                {
                    float iconSz = std::clamp(vpH * 0.020f, 12.f, 20.f);
                    ImVec2 iconTL(abvX - iconSz * 0.5f, abvY - iconSz * 0.5f);
                    ImVec2 iconBR(abvX + iconSz * 0.5f, abvY + iconSz * 0.5f);
                    dl->AddImage(profTex, iconTL, iconBR);
                }
            }
        }

        // Floating skill icon + cast bar above agent (players/NPCs, alive only)
        if (m_showSkillIcons && !dead && (ard.type == AgentType::Player || ard.type == AgentType::NPC))
        {
            auto sv = ard.skillVisualAtTime(m_debugTimeline);
            if (sv.skillId > 0 && sv.alpha > 0.f)
            {
                EnsureSkillIconIndex();
                ID3D11Device* dev = m_deviceResources->GetD3DDevice();
                ImTextureID skillTex = LoadSkillIcon(this, dev, sv.skillId,
                                                     m_skillIconIndex, m_skillIconCache);
                constexpr float SKILL_WORLD_OFFSET = 250.f;
                XMFLOAT3 skillPos = { pos.x, pos.y + SKILL_WORLD_OFFSET, pos.z };
                float skX, skY;
                if (ProjectToScreen(viewProj, vpW, vpH, skillPos, skX, skY))
                {
                    float dpiScale = std::max(1.f, vpH / 1080.f);
                    ImU8 alpha = (ImU8)(sv.alpha * 255.f);
                    float iconSz = std::clamp(vpH * 0.028f, 20.f, 32.f);

                    // Skill icon
                    if (skillTex)
                    {
                        ImVec2 iconTL(skX - iconSz * 0.5f, skY - iconSz * 0.5f);
                        ImVec2 iconBR(skX + iconSz * 0.5f, skY + iconSz * 0.5f);
                        dl->AddImage(skillTex, iconTL, iconBR,
                                     ImVec2(0, 0), ImVec2(1, 1),
                                     IM_COL32(255, 255, 255, alpha));
                    }

                    // Cast bar (non-instant skills: casting, cancelled, or just completed)
                    bool showBar = sv.isCasting || sv.cancelled;
                    if (showBar)
                    {
                        float barW = iconSz * 1.6f;
                        float barH = 6.f  * dpiScale;
                        float gap  = 2.f  * dpiScale;

                        ImVec2 barMin(skX - barW * 0.5f, skY + iconSz * 0.5f + gap);
                        ImVec2 barMax(barMin.x + barW, barMin.y + barH);
                        float pct   = sv.progress;
                        float midY  = barMin.y + barH * 0.5f;

                        // Background: procedural vertical gradient (black→gray→black)
                        {
                            ImU32 bgD = IM_COL32(0, 0, 0, alpha);
                            ImU32 bgM = IM_COL32(36, 36, 36, alpha);
                            dl->AddRectFilledMultiColor(barMin, ImVec2(barMax.x, midY),
                                bgD, bgD, bgM, bgM);
                            dl->AddRectFilledMultiColor(ImVec2(barMin.x, midY), barMax,
                                bgM, bgM, bgD, bgD);
                        }

                        // Fill: procedural horizontal gradient + vertical vignette
                        float fillW = barW * pct;
                        if (pct > 0.005f)
                        {
                            static const GradStop sGreenH[] = {
                                { 0.000f,  10, 10, 10 }, { 0.200f,  26, 58, 10 },
                                { 0.400f,  64,176, 32 }, { 0.600f, 168,240, 80 },
                                { 0.800f, 200,255,112 }, { 1.000f, 144,224, 64 }
                            };
                            static const GradStop sOrangeH[] = {
                                { 0.000f,  10,  8,  0 }, { 0.143f,  58, 30,  0 },
                                { 0.286f, 122, 58,  0 }, { 0.429f, 192, 96,  0 },
                                { 0.571f, 232,144, 16 }, { 0.714f, 255,184, 32 },
                                { 0.857f, 255,208, 64 }, { 1.000f, 232,160, 16 }
                            };
                            const GradStop* hS = sv.cancelled ? sOrangeH  : sGreenH;
                            int              nH = sv.cancelled ? 8         : 6;
                            float         topV = sv.cancelled ? 0.58f     : 0.55f;
                            float         botV = sv.cancelled ? 0.52f     : 0.50f;

                            int nSegs = std::clamp((int)(fillW / 3.f), 4, 24);
                            for (int si = 0; si < nSegs; ++si)
                            {
                                float u0 = (float)si / nSegs;
                                float u1 = (float)(si + 1) / nSegs;
                                float r0, g0, b0, r1, g1, b1;
                                SampleGradient(hS, nH, u0 * pct, r0, g0, b0);
                                SampleGradient(hS, nH, u1 * pct, r1, g1, b1);

                                float x0 = barMin.x + fillW * u0;
                                float x1 = barMin.x + fillW * u1;

                                auto vig = [&](float r, float g, float b, float d) -> ImU32 {
                                    float m = 1.f - d;
                                    return IM_COL32((ImU8)(r * m), (ImU8)(g * m), (ImU8)(b * m), alpha);
                                };
                                ImU32 tl = vig(r0,g0,b0, topV);
                                ImU32 tr = vig(r1,g1,b1, topV);
                                ImU32 ml = IM_COL32((ImU8)r0,(ImU8)g0,(ImU8)b0, alpha);
                                ImU32 mr = IM_COL32((ImU8)r1,(ImU8)g1,(ImU8)b1, alpha);
                                ImU32 bl = vig(r0,g0,b0, botV);
                                ImU32 br = vig(r1,g1,b1, botV);

                                dl->AddRectFilledMultiColor(
                                    ImVec2(x0, barMin.y), ImVec2(x1, midY),
                                    tl, tr, mr, ml);
                                dl->AddRectFilledMultiColor(
                                    ImVec2(x0, midY), ImVec2(x1, barMax.y),
                                    ml, mr, br, bl);
                            }
                        }

                        // Outer glow at leading edge
                        float fillX = barMin.x + fillW;
                        if (pct > 0.01f)
                        {
                            float gw = 6.f * dpiScale;
                            ImU8 glA1 = (ImU8)(140 * sv.alpha);
                            ImU8 glA2 = (ImU8)( 60 * sv.alpha);
                            ImU32 gc1, gc2;
                            if (sv.cancelled)
                            {
                                gc1 = IM_COL32(192,120,  0, glA1);
                                gc2 = IM_COL32(192,120,  0, glA2);
                            }
                            else
                            {
                                gc1 = IM_COL32( 96,208, 32, glA1);
                                gc2 = IM_COL32( 96,208, 32, glA2);
                            }
                            dl->AddRectFilled(
                                ImVec2(fillX - gw * 0.5f, barMin.y),
                                ImVec2(fillX + gw * 0.5f, barMax.y), gc1);
                            dl->AddRectFilled(
                                ImVec2(fillX - gw, barMin.y - 1.f * dpiScale),
                                ImVec2(fillX + gw, barMax.y + 1.f * dpiScale), gc2);
                        }
                    }
                }
            }
        }


        if (showDot)
        {
            // For players in dot LOD, use stylized profession icon instead of plain dot
            bool usedProfIcon = false;
            if (ard.type == AgentType::Player && ard.primaryProf >= 1)
            {
                ID3D11Device* dev = m_deviceResources->GetD3DDevice();
                ImTextureID stylTex = LoadProfStylized(dev, ard.primaryProf);
                if (stylTex)
                {
                    float iconSz = std::clamp(vpH * 0.020f, 12.f, 22.f);
                    ImVec2 iconTL(scrX - iconSz * 0.5f, scrY - iconSz * 0.5f);
                    ImVec2 iconBR(scrX + iconSz * 0.5f, scrY + iconSz * 0.5f);
                    dl->AddImage(stylTex, iconTL, iconBR);
                    usedProfIcon = true;
                }
            }
            if (!usedProfIcon)
            {
                bool isSpecialGadget = (ard.categoryName == "Repair Kit" ||
                                        ard.categoryName == "Tower Flag Stand" ||
                                        ard.categoryName == "Obelisk Flag Stand" ||
                                        ard.categoryName == "Resurrection Shrine" ||
                                        ard.categoryName == "Dwarven Resurrection Shrine" ||
                                        ard.categoryName == "Lever");
                ImU32 dotColor;
                if (isSpecialGadget)
                    dotColor = IM_COL32(220, 200, 120, 255);
                else if (ard.type == AgentType::Spirit)
                    dotColor = IM_COL32(0x80, 0xFF, 0x80, 0xFF);
                else if (ard.type == AgentType::Item)
                    dotColor = IM_COL32(0xFF, 0xA5, 0x00, 0xFF);
                else
                    dotColor = GetAgentTeamColor(ard.teamId);
                dl->AddCircleFilled(ImVec2(scrX, scrY), dotRadius, dotColor);
                dl->AddCircle(ImVec2(scrX, scrY), dotRadius, IM_COL32(0, 0, 0, 180), 0, 1.5f);
            }
        }

        // Dead freeze indicator: black X over the dot (dot-mode agents only)
        if (showDot && is.showDeadFreeze && dead &&
            ard.type != AgentType::Flag && ard.type != AgentType::Spirit)
        {
            float r = dotRadius + 2.f;
            dl->AddLine(ImVec2(scrX - r, scrY - r), ImVec2(scrX + r, scrY + r),
                        IM_COL32(0, 0, 0, 240), 2.f);
            dl->AddLine(ImVec2(scrX + r, scrY - r), ImVec2(scrX - r, scrY + r),
                        IM_COL32(0, 0, 0, 240), 2.f);
        }

        // Casting freeze indicator: purple ring (dot-mode agents only)
        if (showDot && is.showCastingFreeze && casting && !dead &&
            ard.type != AgentType::Flag && ard.type != AgentType::Spirit)
        {
            dl->AddCircle(ImVec2(scrX, scrY), dotRadius + 3.f,
                          IM_COL32(180, 60, 255, 220), 0, 2.f);
        }

        // Neon-green dashed ring + glow at cylinder base for followed agent (counter-clockwise spin)
        if (m_cameraMode == CameraMode::FollowAgent && agentId == m_followedAgentId)
        {
            constexpr int   NUM_DASHES = 16;
            constexpr float DASH_FRAC  = 0.70f;   // 70% visible, 30% gap
            constexpr float CYL_R      = 30.f;
            constexpr float RING_R     = CYL_R + 4.f;
            constexpr float PI2        = 6.2831853f;
            constexpr int   PTS_PER_DASH = 8;      // smooth arc per dash
            const ImU32 ringCol = IM_COL32(0, 255, 120, 255);
            const ImU32 glowCol = IM_COL32(0, 255, 120, 80);

            float baseY = pos.y + 0.05f;
            float spinOffset = -fmodf((float)ImGui::GetTime() / 15.f, 1.f) * PI2;
            float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 2.5f);
            float pulseR = RING_R + pulse * 3.f;
            ImU8  pulseA = (ImU8)(200 + (int)(55.f * pulse));

            auto projectRingPt = [&](float angle, float r, float& ox, float& oy) -> bool {
                XMFLOAT3 wp = { pos.x + r * cosf(angle), baseY, pos.z + r * sinf(angle) };
                return ProjectToScreen(viewProj, vpW, vpH, wp, ox, oy);
            };

            // Glow layers (pulsating)
            for (int g = 1; g <= 3; ++g)
            {
                float gr = pulseR + g * 3.f;
                ImU32 gc = IM_COL32(0, 255, 120, (ImU8)((int)(80 * (0.5f + 0.5f * pulse)) / g));
                float prevX, prevY;
                bool first = true;
                for (int i = 0; i <= 64; ++i)
                {
                    float a = (float(i) / 64) * PI2;
                    float rx, ry;
                    if (!projectRingPt(a, gr, rx, ry)) { first = true; continue; }
                    if (!first)
                        dl->AddLine(ImVec2(prevX, prevY), ImVec2(rx, ry), gc, 2.f);
                    prevX = rx; prevY = ry;
                    first = false;
                }
            }

            // Dashed ring with counter-clockwise rotation (pulsating)
            ImU32 pulseRingCol = IM_COL32(0, 255, 120, pulseA);
            float dashArc = (PI2 / NUM_DASHES) * DASH_FRAC;
            float segStep = PI2 / NUM_DASHES;
            for (int d = 0; d < NUM_DASHES; ++d)
            {
                float dashStart = spinOffset + d * segStep;
                float prevX, prevY;
                bool first = true;
                for (int p = 0; p <= PTS_PER_DASH; ++p)
                {
                    float a = dashStart + (float(p) / PTS_PER_DASH) * dashArc;
                    float rx, ry;
                    if (!projectRingPt(a, pulseR, rx, ry)) { first = true; continue; }
                    if (!first)
                        dl->AddLine(ImVec2(prevX, prevY), ImVec2(rx, ry), pulseRingCol, 2.5f);
                    prevX = rx; prevY = ry;
                    first = false;
                }
            }
        }

        // Hover detection + click-to-follow
        if (canClickAgents)
        {
            float dx = mousePos.x - scrX;
            float dy = mousePos.y - scrY;
            if (dx * dx + dy * dy <= clickRadius * clickRadius)
            {
                m_hoveredAgentId = agentId;
                dl->AddCircle(ImVec2(scrX, scrY), dotRadius + 4.f,
                              IM_COL32(255, 255, 255, 100), 0, 1.5f);

                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    EnterFollowMode(agentId);
                    if (m_showRangeRings)
                        m_ringAgentFilter = (m_ringAgentFilter == agentId) ? -1 : agentId;
                    if (m_fogPerspective > 0)
                        m_fogPlayerAgent = (m_fogPlayerAgent == agentId) ? -1 : agentId;
                }
            }
        }

        std::string label = GetAgentLabel(ard);
        ImVec2 textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, label.c_str());
        float lx = scrX - textSize.x * 0.5f;
        float ly = scrY + dotRadius + labelOffY;

        bool isSpecialLabel = (ard.categoryName == "Repair Kit" ||
                               ard.categoryName == "Tower Flag Stand" ||
                               ard.categoryName == "Obelisk Flag Stand" ||
                               ard.categoryName == "Resurrection Shrine" ||
                               ard.categoryName == "Dwarven Resurrection Shrine" ||
                               ard.categoryName == "Lever");
        float pad = 2.f;
        dl->AddRectFilled(ImVec2(lx - pad, ly - pad),
                          ImVec2(lx + textSize.x + pad, ly + textSize.y + pad),
                          IM_COL32(0, 0, 0, 13), 3.f);
        if (isSpecialLabel)
        {
            dl->AddText(ImVec2(lx, ly), IM_COL32(245, 228, 180, 255), label.c_str());
        }
        else
        {
            dl->AddText(ImVec2(lx + 1.f, ly + 1.f), IM_COL32(0, 0, 0, 200), label.c_str());
            dl->AddText(ImVec2(lx, ly), IM_COL32(255, 255, 255, 230), label.c_str());
        }
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

                // Subtle pulsing glow behind the captured flag
                ImVec2 center((iconTL.x + iconBR.x) * 0.5f, (iconTL.y + iconBR.y) * 0.5f);
                float glowRadius = iconSz * 0.75f;
                float pulse = 0.6f + 0.4f * sinf((float)ImGui::GetTime() * 1.8f);
                ImU32 glowCol = (standTeam == 0)
                    ? IM_COL32(60, 130, 255, (int)(50 * pulse))
                    : IM_COL32(255, 60, 50,  (int)(50 * pulse));
                dl->AddCircleFilled(center, glowRadius, glowCol, 32);

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
                if (standTeam == ti && (m_debugTimeline - standCaptureTime) < 1.f)
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
// Skill Lasers: animated dashed line from caster → target
// ---------------------------------------------------------------------------

void ReplayWindow::DrawSkillLasers()
{
    if (!m_showSkillLasers) return;
    if (!m_skillUseTimelineBuilt) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    Camera* cam = m_mapRenderer->GetCamera();
    XMMATRIX viewProj = cam->GetView() * cam->GetProj();
    auto vp = m_deviceResources->GetScreenViewport();
    float vpW = vp.Width, vpH = vp.Height;
    const auto& t = m_replayCtx.mapTransform;

    const InterpolationSettings& is = m_replayCtx.interpSettings;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float dpi = std::max(1.f, vpH / 1080.f);
    float curTime = (float)ImGui::GetTime();

    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Player && ard.type != AgentType::NPC) continue;
        if (ard.isDeadAtTime(m_debugTimeline)) continue;

        auto laser = ard.skillLaserAtTime(m_debugTimeline);
        if (laser.targetId <= 0 || laser.alpha <= 0.f) continue;

        auto tit = m_replayCtx.agents.find(laser.targetId);
        if (tit == m_replayCtx.agents.end()) continue;
        auto& targ = tit->second;

        // Caster position
        float cx, cy, cz;
        InterpolateAgentPosition(ard, m_debugTimeline, is, cx, cy, cz);
        XMFLOAT3 casterWorld = ApplyMapTransformToPos(cx, cy, cz, t);
        casterWorld.y += 60.f;

        float cScrX, cScrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, casterWorld, cScrX, cScrY)) continue;

        // Target position
        float txp, typ, tzp;
        InterpolateAgentPosition(targ, m_debugTimeline, is, txp, typ, tzp);
        XMFLOAT3 targetWorld = ApplyMapTransformToPos(txp, typ, tzp, t);
        targetWorld.y += 60.f;

        float tScrX, tScrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, targetWorld, tScrX, tScrY)) continue;

        // LOD check: skip if caster is far and in dot LOD
        if (m_uiLayout.lodEnabled && ard.currentLOD == 0)
        {
            XMFLOAT3 camPos;
            XMStoreFloat3(&camPos, cam->GetPosition());
            float dx = casterWorld.x - camPos.x;
            float dy = casterWorld.y - camPos.y;
            float dz2 = casterWorld.z - camPos.z;
            float dist = sqrtf(dx * dx + dy * dy + dz2 * dz2);
            if (dist > m_uiLayout.lodDotDist * 1.5f) continue;
        }

        // Determine ally vs enemy
        bool isEnemy = (ard.teamId != targ.teamId);
        ImU32 laserCol, glowCol;
        int lR, lG, lB;
        if (isEnemy)
        {
            lR = 255; lG = 60; lB = 60;
            laserCol = IM_COL32(255, 60, 60, (ImU8)(255 * laser.alpha));
            glowCol  = IM_COL32(255, 60, 60, (ImU8)(120 * laser.alpha));
        }
        else
        {
            lR = 60; lG = 255; lB = 120;
            laserCol = IM_COL32(60, 255, 120, (ImU8)(255 * laser.alpha));
            glowCol  = IM_COL32(60, 255, 120, (ImU8)(120 * laser.alpha));
        }

        ImVec2 A(cScrX, cScrY), B(tScrX, tScrY);
        float lineLen = sqrtf((B.x - A.x) * (B.x - A.x) + (B.y - A.y) * (B.y - A.y));
        if (lineLen < 2.f) continue;

        ImVec2 dir((B.x - A.x) / lineLen, (B.y - A.y) / lineLen);

        // Glow layers (continuous line behind dashes)
        for (int g = 1; g <= 3; ++g)
        {
            float thick = (1.0f + g * 0.8f) * dpi;
            ImU32 gc = IM_COL32(lR, lG, lB, (ImU8)(std::max(0.f, 30.f / g * laser.alpha)));
            dl->AddLine(A, B, gc, thick);
        }

        // Flowing dashes (caster → target direction)
        float dashLen = 14.f * dpi;
        float gapLen  = 8.f * dpi;
        float period  = dashLen + gapLen;
        float anim    = fmodf(curTime * 0.8f, 1.f);
        float offset  = anim * period;

        for (float d = offset - period; d < lineLen; d += period)
        {
            float s0 = std::clamp(d, 0.f, lineLen);
            float s1 = std::clamp(d + dashLen, 0.f, lineLen);
            if (s1 - s0 < 1.f) continue;
            ImVec2 p0(A.x + dir.x * s0, A.y + dir.y * s0);
            ImVec2 p1(A.x + dir.x * s1, A.y + dir.y * s1);
            dl->AddLine(p0, p1, laserCol, 1.2f * dpi);
        }

        // Arrowhead at target
        float arrowSz = 10.f * dpi;
        ImVec2 perp(-dir.y, dir.x);
        ImVec2 tip = B;
        ImVec2 left(B.x - dir.x * arrowSz + perp.x * arrowSz * 0.6f,
                     B.y - dir.y * arrowSz + perp.y * arrowSz * 0.6f);
        ImVec2 right(B.x - dir.x * arrowSz - perp.x * arrowSz * 0.6f,
                      B.y - dir.y * arrowSz - perp.y * arrowSz * 0.6f);
        dl->AddTriangleFilled(tip, left, right, laserCol);

        // Breathing target highlight ring at cylinder base (3D projected)
        constexpr float CYL_R       = 30.f;
        constexpr float RING_R      = CYL_R + 5.f;
        constexpr int   RING_SEGS   = 48;
        constexpr float DASH_RATIO  = 0.35f;
        constexpr float PI2         = 6.2831853f;

        float pulse = 0.5f + 0.5f * sinf(curTime * 4.f);
        ImU32 ringCol = IM_COL32(
            (int)(lR * (0.7f + 0.3f * pulse)),
            (int)(lG * (0.7f + 0.3f * pulse)),
            (int)(lB * (0.7f + 0.3f * pulse)),
            (ImU8)(220 * laser.alpha));

        // Base of the cylinder (targetWorld.y was offset +60, remove it and add small lift)
        float baseY = targetWorld.y - 60.f + 0.05f;

        for (int i = 0; i < RING_SEGS; ++i)
        {
            float a0 = (float(i) / RING_SEGS) * PI2;
            float a1 = (float(i + 1) / RING_SEGS) * PI2;
            float mid = (a0 + a1) * 0.5f;
            float sa = a0 + (mid - a0) * (1.f - DASH_RATIO);
            float ea = mid + (a1 - mid) * DASH_RATIO;

            XMFLOAT3 wp0 = { targetWorld.x + RING_R * cosf(sa), baseY, targetWorld.z + RING_R * sinf(sa) };
            XMFLOAT3 wp1 = { targetWorld.x + RING_R * cosf(ea), baseY, targetWorld.z + RING_R * sinf(ea) };
            float sx0, sy0, sx1, sy1;
            if (ProjectToScreen(viewProj, vpW, vpH, wp0, sx0, sy0) &&
                ProjectToScreen(viewProj, vpW, vpH, wp1, sx1, sy1))
            {
                dl->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), ringCol, 1.5f * dpi);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Range Ring Rendering
// ---------------------------------------------------------------------------

static const ReplayWindow::RingDef kRingDefs[ReplayWindow::kRingTypeCount] = {
    //                                                    thick  solid  dash       fillA
    { "Touch",      144.f,  IM_COL32(255, 64, 64, 255),  1.5f, true,  0,0,       0.15f },
    { "Adjacent",   166.f,  IM_COL32(255,112, 32, 255),  1.5f, true,  0,0,       0.15f },
    { "Nearby",     240.f,  IM_COL32(255,176, 32, 255),  1.0f, true,  0,0,       0.12f },
    { "In Area",    322.f,  IM_COL32(255,255, 64, 255),  1.0f, true,  0,0,       0.10f },
    { "Earshot",   1000.f,  IM_COL32( 64,255,128, 255),  1.0f, true,  0,0,       0.06f },
    { "Cast Range",1248.f,  IM_COL32( 64,192,255, 255),  2.0f, true,  0,0,       0.05f },
    { "Passive",   2512.f,  IM_COL32(192, 64,255, 255),  1.0f, true,  0,0,       0.03f },
    { "Compass",   5020.f,  IM_COL32(128,128,128, 255),  0.5f, false, 8,4,       0.02f },
};

static constexpr float kRingFillAlpha    = 0.15f;
static constexpr float kRingSelectFill   = 0.22f;
static constexpr float kRingDimFactor    = 0.50f;
static const ImU32 kRingSelectEdge = IM_COL32(128, 255, 128, 255);

static ImU32 ScaleAlpha(ImU32 col, float factor)
{
    ImU8 a = (ImU8)((col >> 24) & 0xFF);
    a = (ImU8)(a * factor);
    return (col & 0x00FFFFFF) | ((ImU32)a << 24);
}

void ReplayWindow::DrawRangeRings()
{
    if (!m_showRangeRings) return;
    if (!m_agentsClassified || m_replayCtx.agents.empty()) return;

    Camera* cam = m_mapRenderer->GetCamera();
    if (!cam) return;
    XMMATRIX viewProj = cam->GetView() * cam->GetProj();

    auto* vp = ImGui::GetMainViewport();
    float vpW = vp->Size.x;
    float vpH = vp->Size.y;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const MapTransform& t = m_replayCtx.mapTransform;
    const InterpolationSettings& is = m_replayCtx.interpSettings;
    Terrain* terrain = m_mapRenderer->GetTerrain();

    constexpr int kSamples = 64;
    constexpr float kPI2 = 6.28318530718f;
    constexpr float kZOffset = 2.f;

    bool hasSelection = (m_ringAgentFilter >= 0);

    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.type != AgentType::Player) continue;
        if (ard.snapshots.empty()) continue;
        if (ard.isDeadAtTime(m_debugTimeline)) continue;

        if (m_fogPerspective > 0 && ard.teamId != m_fogPerspective && IsAgentInFog(agentId))
            continue;

        bool isSelected = (m_ringAgentFilter == agentId);

        if (!isSelected)
        {
            if (hasSelection)
            {
                bool teamEnabled = (ard.teamId == 1 && m_ringShowBlue)
                                || (ard.teamId == 2 && m_ringShowRed);
                if (!teamEnabled) continue;
            }
            else
            {
                if (ard.teamId == 1 && !m_ringShowBlue) continue;
                if (ard.teamId == 2 && !m_ringShowRed)  continue;
            }
        }

        float sx, sy, sz;
        InterpolateAgentPosition(ard, m_debugTimeline, is, sx, sy, sz);

        for (int ri = 0; ri < kRingTypeCount; ++ri)
        {
            bool showRing = m_ringType[ri];
            bool isHoverPreview = (!showRing && m_ringHoveredType == ri);
            if (!showRing && !isHoverPreview) continue;

            const auto& def = kRingDefs[ri];
            float radius = def.radius;

            float dimFactor = (!isSelected && hasSelection) ? 0.25f : 1.f;
            float thickMul  = (isSelected && hasSelection) ? 1.6f : 1.f;
            ImU32 edgeCol = def.color;
            if (dimFactor < 1.f) edgeCol = ScaleAlpha(edgeCol, dimFactor);
            if (isHoverPreview) edgeCol = ScaleAlpha(def.color, 0.40f);

            ImVec2 pts[kSamples];
            bool vis[kSamples];
            int visCount = 0;

            for (int i = 0; i < kSamples; ++i)
            {
                float angle = (float(i) / kSamples) * kPI2;
                float wx = sx + cosf(angle) * radius;
                float wy = sy + sinf(angle) * radius;

                XMFLOAT3 mp = ApplyMapTransformToPos(wx, wy, sz, t);

                if (terrain)
                    mp.y = terrain->get_height_at(mp.x, mp.z) + kZOffset;

                float scrX, scrY;
                vis[i] = ProjectToScreen(viewProj, vpW, vpH, mp, scrX, scrY);
                pts[i] = ImVec2(scrX, scrY);
                if (vis[i]) visCount++;
            }

            if (visCount < 3) continue;

            // Fill pass (brighter when selected, heavily dimmed for teammates)
            {
                float fa = isHoverPreview ? 0.10f : (isSelected ? def.fillAlpha * 2.0f : def.fillAlpha);
                if (dimFactor < 1.f) fa *= dimFactor;
                ImU32 baseRGB = def.color;
                ImU32 fillCol = (baseRGB & 0x00FFFFFF) | ((ImU32)(fa * 255.f) << 24);
                ImVector<ImVec2> polyPts;
                for (int i = 0; i < kSamples; ++i)
                    if (vis[i]) polyPts.push_back(pts[i]);
                if (polyPts.Size >= 3)
                    dl->AddConvexPolyFilled(polyPts.Data, polyPts.Size, fillCol);
            }

            // Ring edge pass
            float thick = def.thickness * thickMul;
            if (def.solid)
            {
                for (int i = 0; i < kSamples; ++i)
                {
                    int j = (i + 1) % kSamples;
                    if (vis[i] && vis[j])
                        dl->AddLine(pts[i], pts[j], edgeCol, thick);
                }
            }
            else
            {
                float dashOn  = def.dashOn;
                float dashOff = def.dashOff;
                float cycle = dashOn + dashOff;
                float accum = 0.f;
                for (int i = 0; i < kSamples; ++i)
                {
                    int j = (i + 1) % kSamples;
                    float dx = pts[j].x - pts[i].x;
                    float dy = pts[j].y - pts[i].y;
                    float segLen = sqrtf(dx * dx + dy * dy);

                    if (vis[i] && vis[j])
                    {
                        float pos = fmodf(accum, cycle);
                        if (pos < dashOn)
                            dl->AddLine(pts[i], pts[j], edgeCol, thick);
                    }
                    accum += segLen;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Range Ring Toolbar UI
// ---------------------------------------------------------------------------

void ReplayWindow::DrawRangeRingToolbar()
{
    if (!m_showRangeRings) return;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,      ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.f, 0.f), ImVec2(vp->Size.x, vp->Size.y));
    ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
    if (ImGui::Begin("Range Rings", &m_showRangeRings,
        ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, vp->Pos.x, vp->Pos.x + vp->Size.x - sz.x);
        float cy = std::clamp(pos.y, vp->Pos.y, vp->Pos.y + vp->Size.y - sz.y);
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));
        auto RingPill = [](const char* label, bool active, ImU32 accent = 0) -> bool {
            ImVec4 bg, tx, hov, bdr;
            if (active) {
                bg  = ImVec4(0.18f, 0.14f, 0.05f, 1.f);
                tx  = ImVec4(1.f, 0.91f, 0.69f, 1.f);
                hov = ImVec4(0.23f, 0.19f, 0.08f, 1.f);
                bdr = ImVec4(1.f, 0.84f, 0.39f, 0.85f);
            } else {
                bg  = ImVec4(1.f, 1.f, 1.f, 0.05f);
                tx  = ImVec4(0.60f, 0.64f, 0.69f, 1.f);
                hov = ImVec4(1.f, 1.f, 1.f, 0.12f);
                bdr = ImVec4(1.f, 1.f, 1.f, 0.08f);
            }
            ImGui::PushStyleColor(ImGuiCol_Button, bg);
            ImGui::PushStyleColor(ImGuiCol_Text, tx);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(bg.x * 0.85f, bg.y * 0.85f, bg.z * 0.85f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, bdr);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(5);
            return clicked;
        };

        auto TeamPill = [](const char* label, bool active, int team) -> bool {
            ImVec4 bg, tx, hov, bdr;
            if (active) {
                if (team == 1) {
                    bg  = ImVec4(0.05f, 0.12f, 0.25f, 1.f);
                    tx  = ImVec4(0.29f, 0.78f, 1.f, 1.f);
                    hov = ImVec4(0.08f, 0.16f, 0.30f, 1.f);
                    bdr = ImVec4(0.29f, 0.78f, 1.f, 0.85f);
                } else if (team == 2) {
                    bg  = ImVec4(0.25f, 0.06f, 0.06f, 1.f);
                    tx  = ImVec4(1.f, 0.42f, 0.42f, 1.f);
                    hov = ImVec4(0.30f, 0.10f, 0.10f, 1.f);
                    bdr = ImVec4(1.f, 0.42f, 0.42f, 0.85f);
                } else {
                    bg  = ImVec4(0.18f, 0.14f, 0.05f, 1.f);
                    tx  = ImVec4(1.f, 0.91f, 0.69f, 1.f);
                    hov = ImVec4(0.23f, 0.19f, 0.08f, 1.f);
                    bdr = ImVec4(1.f, 0.84f, 0.39f, 0.85f);
                }
            } else {
                bg  = ImVec4(1.f, 1.f, 1.f, 0.05f);
                tx  = ImVec4(0.60f, 0.64f, 0.69f, 1.f);
                hov = ImVec4(1.f, 1.f, 1.f, 0.12f);
                bdr = ImVec4(1.f, 1.f, 1.f, 0.08f);
            }
            ImGui::PushStyleColor(ImGuiCol_Button, bg);
            ImGui::PushStyleColor(ImGuiCol_Text, tx);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(bg.x * 0.85f, bg.y * 0.85f, bg.z * 0.85f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, bdr);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(5);
            return clicked;
        };

        // None / All buttons
        if (RingPill("None", false))
            for (int i = 0; i < kRingTypeCount; ++i) m_ringType[i] = false;
        ImGui::SameLine();
        if (RingPill("All", false))
            for (int i = 0; i < kRingTypeCount; ++i) m_ringType[i] = true;

        ImGui::Separator();

        // Ring type pills (two rows: 4 + 4)
        m_ringHoveredType = -1;
        for (int i = 0; i < kRingTypeCount; ++i)
        {
            if (i == 4) {} // new line
            else if (i > 0) ImGui::SameLine();

            ImGui::PushID(i);
            bool clicked = RingPill(kRingDefs[i].name, m_ringType[i], kRingDefs[i].color);

            if (ImGui::IsItemHovered())
            {
                m_ringHoveredType = i;
                ImGui::SetTooltip("%s \xe2\x80\x94 %.0f units", kRingDefs[i].name, kRingDefs[i].radius);
            }

            if (clicked)
            {
                if (ImGui::GetIO().MouseDoubleClicked[0])
                {
                    if (m_ringSoloActive && m_ringType[i])
                    {
                        for (int k = 0; k < kRingTypeCount; ++k)
                            m_ringType[k] = m_ringSoloPrev[k];
                        m_ringSoloActive = false;
                    }
                    else
                    {
                        for (int k = 0; k < kRingTypeCount; ++k)
                            m_ringSoloPrev[k] = m_ringType[k];
                        for (int k = 0; k < kRingTypeCount; ++k)
                            m_ringType[k] = (k == i);
                        m_ringSoloActive = true;
                    }
                }
                else
                {
                    m_ringType[i] = !m_ringType[i];
                    m_ringSoloActive = false;
                }
            }
            ImGui::PopID();
        }

        ImGui::Separator();

        // Team filter (independent toggles)
        if (TeamPill("Blue", m_ringShowBlue, 1))
            m_ringShowBlue = !m_ringShowBlue;
        ImGui::SameLine();
        if (TeamPill("Red", m_ringShowRed, 2))
            m_ringShowRed = !m_ringShowRed;

        if (m_ringAgentFilter >= 0)
        {
            ImGui::SameLine();
            auto it = m_replayCtx.agents.find(m_ringAgentFilter);
            ImVec4 agentCol(1.f, 0.91f, 0.69f, 1.f);
            if (it != m_replayCtx.agents.end()) {
                if (it->second.teamId == 1) agentCol = ImVec4(0.29f, 0.78f, 1.f, 1.f);
                else if (it->second.teamId == 2) agentCol = ImVec4(1.f, 0.42f, 0.42f, 1.f);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, agentCol);
            std::string lbl = (it != m_replayCtx.agents.end())
                ? std::format("Agent: {} [x]", it->second.playerName)
                : std::format("Agent: #{} [x]", m_ringAgentFilter);
            if (ImGui::SmallButton(lbl.c_str()))
                m_ringAgentFilter = -1;
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
}

// ---------------------------------------------------------------------------
// Fog of War Toolbar UI
// ---------------------------------------------------------------------------

void ReplayWindow::DrawFogOfWarToolbar()
{
    if (m_fogPerspective == 0) return;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,      ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));

    auto* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.f, 0.f), ImVec2(240.f, vp->Size.y));
    static bool fogFirstOpen = true;
    if (fogFirstOpen) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 8.f, vp->Pos.y + 8.f), ImGuiCond_Once);
        fogFirstOpen = false;
    }

    bool fogOpen = true;
    if (ImGui::Begin("Fog of War", &fogOpen,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize))
    {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, vp->Pos.x, vp->Pos.x + vp->Size.x - sz.x);
        float cy = std::clamp(pos.y, vp->Pos.y, vp->Pos.y + vp->Size.y - sz.y);
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));

        auto FogPill = [](const char* label, bool active, int team) -> bool {
            ImVec4 bg, tx, hov, bdr;
            if (active) {
                if (team == 1) {
                    bg  = ImVec4(0.05f, 0.12f, 0.25f, 1.f);
                    tx  = ImVec4(0.29f, 0.78f, 1.f, 1.f);
                    hov = ImVec4(0.08f, 0.16f, 0.30f, 1.f);
                    bdr = ImVec4(0.29f, 0.78f, 1.f, 0.85f);
                } else if (team == 2) {
                    bg  = ImVec4(0.25f, 0.06f, 0.06f, 1.f);
                    tx  = ImVec4(1.f, 0.42f, 0.42f, 1.f);
                    hov = ImVec4(0.30f, 0.10f, 0.10f, 1.f);
                    bdr = ImVec4(1.f, 0.42f, 0.42f, 0.85f);
                } else {
                    bg  = ImVec4(0.18f, 0.14f, 0.05f, 1.f);
                    tx  = ImVec4(1.f, 0.91f, 0.69f, 1.f);
                    hov = ImVec4(0.23f, 0.19f, 0.08f, 1.f);
                    bdr = ImVec4(1.f, 0.84f, 0.39f, 0.85f);
                }
            } else {
                bg  = ImVec4(1.f, 1.f, 1.f, 0.05f);
                tx  = ImVec4(0.60f, 0.64f, 0.69f, 1.f);
                hov = ImVec4(1.f, 1.f, 1.f, 0.12f);
                bdr = ImVec4(1.f, 1.f, 1.f, 0.08f);
            }
            ImGui::PushStyleColor(ImGuiCol_Button, bg);
            ImGui::PushStyleColor(ImGuiCol_Text, tx);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(bg.x * 0.85f, bg.y * 0.85f, bg.z * 0.85f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, bdr);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(5);
            return clicked;
        };

        ImGui::TextColored(ImVec4(0.78f, 0.72f, 0.55f, 1.f), "Perspective");
        ImGui::SameLine();
        if (FogPill("Off", m_fogPerspective == 0 && m_fogPlayerAgent < 0, 0))
        { m_fogPerspective = 0; m_fogPlayerAgent = -1; }
        ImGui::SameLine();
        if (FogPill("Blue", m_fogPerspective == 1 && m_fogPlayerAgent < 0, 1))
        { m_fogPerspective = (m_fogPerspective == 1 && m_fogPlayerAgent < 0) ? 0 : 1; m_fogPlayerAgent = -1; }
        ImGui::SameLine();
        if (FogPill("Red", m_fogPerspective == 2 && m_fogPlayerAgent < 0, 2))
        { m_fogPerspective = (m_fogPerspective == 2 && m_fogPlayerAgent < 0) ? 0 : 2; m_fogPlayerAgent = -1; }

        if (m_fogPlayerAgent >= 0)
        {
            auto pit = m_replayCtx.agents.find(m_fogPlayerAgent);
            std::string pname = (pit != m_replayCtx.agents.end())
                ? pit->second.partyBarLabel : std::format("#{}", m_fogPlayerAgent);
            std::string pillLabel = pname + "  \xc3\x97";
            if (FogPill(pillLabel.c_str(), true, 0))
                m_fogPlayerAgent = -1;
        }

        ImGui::Separator();

        ImGui::TextColored(ImVec4(0.78f, 0.72f, 0.55f, 1.f), "Enemies");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.10f, 0.12f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.15f, 0.14f, 0.10f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.f, 0.84f, 0.39f, 1.f));
        if (ImGui::RadioButton("Hide", !m_fogGhostMode)) m_fogGhostMode = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Ghost", m_fogGhostMode)) m_fogGhostMode = true;
        ImGui::PopStyleColor(3);

        ImGui::Separator();

        bool inPlayerMode = (m_fogPlayerAgent >= 0);
        int sourceCount = 0;
        bool playerDead = false;
        std::string playerDeadName;

        if (inPlayerMode)
        {
            auto pit = m_replayCtx.agents.find(m_fogPlayerAgent);
            if (pit != m_replayCtx.agents.end() && !pit->second.snapshots.empty())
            {
                if (pit->second.isDeadAtTime(m_debugTimeline))
                {
                    playerDead = true;
                    playerDeadName = pit->second.partyBarLabel;
                }
                else
                    sourceCount = 1;
            }
        }
        else
        {
            for (auto& [aid, ard] : m_replayCtx.agents) {
                if (ard.teamId != m_fogPerspective) continue;
                if (ard.type != AgentType::Player) continue;
                if (ard.snapshots.empty()) continue;
                if (!ard.isDeadAtTime(m_debugTimeline)) sourceCount++;
            }
        }

        ImU32 dotCol;
        if (playerDead || sourceCount == 0)
            dotCol = IM_COL32(220, 60, 60, 255);
        else if (inPlayerMode)
            dotCol = IM_COL32(64, 220, 80, 255);
        else if (sourceCount >= 8)
            dotCol = IM_COL32(64, 220, 80, 255);
        else if (sourceCount >= 5)
            dotCol = IM_COL32(230, 180, 40, 255);
        else
            dotCol = IM_COL32(220, 60, 60, 255);

        ImVec2 cur = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float dotR = 4.f;
        float textH = ImGui::GetTextLineHeight();
        dl->AddCircleFilled(ImVec2(cur.x + dotR, cur.y + textH * 0.5f), dotR, dotCol);
        ImGui::Dummy(ImVec2(dotR * 2.f + 4.f, 0));
        ImGui::SameLine();

        if (playerDead)
            ImGui::Text("0 / 8 -- %s is dead", playerDeadName.c_str());
        else if (inPlayerMode)
            ImGui::Text("1 / 8 players as source");
        else
            ImGui::Text("%d / 8 players visible", sourceCount);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);

    if (!fogOpen)
        m_fogPerspective = 0;

    if (m_fogPerspective > 0 && m_fogPlayerAgent >= 0)
    {
        auto pit = m_replayCtx.agents.find(m_fogPlayerAgent);
        if (pit != m_replayCtx.agents.end() && !pit->second.snapshots.empty()
            && pit->second.isDeadAtTime(m_debugTimeline))
        {
            std::string msg = pit->second.partyBarLabel + " \xe2\x80\x94 No vision";
            ImVec2 txtSz = ImGui::CalcTextSize(msg.c_str());
            auto* fgDl = ImGui::GetForegroundDrawList();
            auto* mvp  = ImGui::GetMainViewport();
            ImVec2 center(mvp->Pos.x + mvp->Size.x * 0.5f, mvp->Pos.y + mvp->Size.y * 0.5f);
            fgDl->AddText(nullptr, 13.f,
                ImVec2(center.x - txtSz.x * 0.5f, center.y - txtSz.y * 0.5f),
                IM_COL32(255, 255, 255, 102), msg.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Morale Panel
// ---------------------------------------------------------------------------

void ReplayWindow::DrawMoralePanel()
{
    if (!m_showMoralePanel) return;

    const float curTime = m_debugTimeline;
    const auto* vp = ImGui::GetMainViewport();

    struct PlayerMorale {
        int    agentId = 0;
        std::string name;
        int    primaryProf = 0;
        int    secondaryProf = 0;
        int    morale = 0;
        int    deathCount = 0;
        bool   dead = false;
    };

    std::vector<PlayerMorale> blueTeam, redTeam;

    int blueBoosts = 0, redBoosts = 0;
    for (auto& ev : m_replayCtx.stocData.jumbo) {
        if (ev.time > curTime) break;
        if (ev.message == "MORALE_BOOST") {
            if (ev.party_value == 1635021873) ++blueBoosts;
            else if (ev.party_value == 1635021874) ++redBoosts;
        }
    }

    auto countDeaths = [&](const AgentReplayData& ard) -> int {
        int count = 0;
        for (size_t i = 1; i < ard.snapshots.size(); ++i) {
            if (ard.snapshots[i].time > curTime) break;
            if (ard.snapshots[i].is_dead && !ard.snapshots[i - 1].is_dead)
                ++count;
        }
        return count;
    };

    auto buildTeam = [&](const std::vector<int>& ids, int boosts) {
        std::vector<PlayerMorale> result;
        for (int id : ids) {
            auto it = m_replayCtx.agents.find(id);
            if (it == m_replayCtx.agents.end()) continue;
            const auto& ard = it->second;
            if (ard.type != AgentType::Player) continue;

            PlayerMorale pm;
            pm.agentId = id;
            pm.name = ard.playerName;
            pm.primaryProf = ard.primaryProf;
            pm.secondaryProf = ard.secondaryProf;
            pm.deathCount = countDeaths(ard);
            pm.morale = std::clamp(boosts * 10 - pm.deathCount * 15, -60, 10);
            pm.dead = ard.isDeadAtTime(curTime);
            result.push_back(std::move(pm));
        }
        return result;
    };

    blueTeam = buildTeam(m_team1PlayerIds, blueBoosts);
    redTeam  = buildTeam(m_team2PlayerIds, redBoosts);

    auto getGuildLabel = [&](const std::string& partyId) -> std::string {
        auto pit = m_matchMeta.parties.find(partyId);
        if (pit == m_matchMeta.parties.end()) return "?";
        std::map<int, int> guildCounts;
        for (const auto& p : pit->second.players)
            if (p.guild_id > 0) guildCounts[p.guild_id]++;
        int bestId = 0, bestCnt = 0;
        for (const auto& [gid, cnt] : guildCounts)
            if (cnt > bestCnt) { bestId = gid; bestCnt = cnt; }
        if (bestId == 0) return "?";
        auto git = m_matchMeta.guilds.find(std::to_string(bestId));
        if (git != m_matchMeta.guilds.end())
            return git->second.name + " [" + git->second.tag + "]";
        return "?";
    };

    std::string blueLabel = getGuildLabel("1");
    std::string redLabel  = getGuildLabel("2");

    constexpr float kPanelW = 520.f;
    constexpr float kRowH = 22.f;
    constexpr float kIconSz = 16.f;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,      ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));

    ImGui::SetNextWindowPos(
        ImVec2(vp->Pos.x + (vp->Size.x - kPanelW) * 0.5f,
               vp->Pos.y + vp->Size.y - 300.f),
        ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(kPanelW, 0.f), ImVec2(kPanelW, vp->Size.y));

    if (!ImGui::Begin("Morale##morale_panel", &m_showMoralePanel,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize))
    {
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(5);
        return;
    }

    {
        ImVec2 wPos = ImGui::GetWindowPos();
        ImVec2 wSz  = ImGui::GetWindowSize();
        float cx = std::clamp(wPos.x, vp->Pos.x, vp->Pos.x + vp->Size.x - wSz.x);
        float cy = std::clamp(wPos.y, vp->Pos.y, vp->Pos.y + vp->Size.y - wSz.y);
        if (cx != wPos.x || cy != wPos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fontSize = ImGui::GetFontSize();
    ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;

    constexpr ImU32 kBlueTeam = IM_COL32(0x4A, 0xC8, 0xFF, 0xFF);
    constexpr ImU32 kRedTeam  = IM_COL32(0xFF, 0x6B, 0x6B, 0xFF);
    constexpr ImU32 kGreen    = IM_COL32(0x40, 0xE0, 0x80, 0xFF);
    constexpr ImU32 kMuted    = IM_COL32(0x70, 0x7D, 0x88, 0xFF);
    constexpr ImU32 kAmber    = IM_COL32(0xFF, 0xA0, 0x30, 0xFF);
    constexpr ImU32 kRed      = IM_COL32(0xFF, 0x50, 0x50, 0xFF);
    constexpr ImU32 kText     = IM_COL32(0xE8, 0xEC, 0xF2, 0xFF);
    constexpr ImU32 kTextDead = IM_COL32(0x60, 0x60, 0x60, 0xFF);

    auto moraleColor = [&](int m) -> ImU32 {
        if (m > 0)    return kGreen;
        if (m == 0)   return kMuted;
        if (m >= -15) return kAmber;
        if (m >= -30) return IM_COL32(0xFF, 0x80, 0x30, 0xFF);
        return kRed;
    };

    auto avgColor = [&](float avg) -> ImU32 {
        if (avg > 0.f)    return kGreen;
        if (avg >= -5.f)  return kMuted;
        if (avg >= -15.f) return kAmber;
        return kRed;
    };

    const float contentW = kPanelW - 20.f;
    const float colW = (contentW - 1.f) * 0.5f;
    const float startX = ImGui::GetCursorScreenPos().x;
    const float startY = ImGui::GetCursorScreenPos().y;

    size_t maxRows = std::max(blueTeam.size(), redTeam.size());

    // Column headers: guild name [tag] in team color, standard font
    {
        ImVec2 p(startX, startY);
        dl->AddText(ImVec2(p.x + 2.f, p.y), kBlueTeam, blueLabel.c_str());
        dl->AddText(ImVec2(p.x + colW + 1.f + 2.f, p.y), kRedTeam, redLabel.c_str());
        ImGui::Dummy(ImVec2(0.f, fontSize + 4.f));
    }

    float rowStartY = ImGui::GetCursorScreenPos().y;

    auto drawPlayerRow = [&](const PlayerMorale& pm, float x, float y) {
        float cx = x;

        // Primary profession icon
        if (dev && pm.primaryProf >= 1) {
            ImTextureID tex = LoadProfIcon(dev, pm.primaryProf);
            if (tex) {
                float iy = y + (kRowH - kIconSz) * 0.5f;
                dl->AddImage(tex, ImVec2(cx, iy), ImVec2(cx + kIconSz, iy + kIconSz));
            }
        }
        cx += kIconSz + 1.f;

        // Secondary profession icon
        if (dev && pm.secondaryProf >= 1) {
            ImTextureID tex = LoadProfIcon(dev, pm.secondaryProf);
            if (tex) {
                float iy = y + (kRowH - kIconSz) * 0.5f;
                dl->AddImage(tex, ImVec2(cx, iy), ImVec2(cx + kIconSz, iy + kIconSz));
            }
        }
        cx += kIconSz + 3.f;

        // Player name in standard white/grey
        ImU32 nameCol = pm.dead ? kTextDead : kText;
        dl->AddText(ImVec2(cx, y + (kRowH - fontSize) * 0.5f), nameCol, pm.name.c_str());

        // Right side: morale value + dots or star
        float rightEdge = x + colW;
        char valBuf[16];

        if (pm.morale > 0) {
            snprintf(valBuf, sizeof(valBuf), "+%d%%", pm.morale);
            ImVec2 valSz = ImGui::CalcTextSize(valBuf);
            float valX = rightEdge - valSz.x - 2.f;
            dl->AddText(ImVec2(valX, y + (kRowH - fontSize) * 0.5f), kGreen, valBuf);
            float starX = valX - fontSize;
            dl->AddText(ImVec2(starX, y + (kRowH - fontSize) * 0.5f), kGreen, "\xe2\x98\x85");
        }
        else if (pm.morale == 0) {
            ImVec2 zSz = ImGui::CalcTextSize("0%");
            dl->AddText(ImVec2(rightEdge - zSz.x - 2.f, y + (kRowH - fontSize) * 0.5f), kMuted, "0%");
        }
        else {
            snprintf(valBuf, sizeof(valBuf), "%d%%", pm.morale);
            ImU32 valCol = moraleColor(pm.morale);
            ImVec2 valSz = ImGui::CalcTextSize(valBuf);

            int dots = std::min(4, pm.deathCount);
            float dotSpacing = 10.f;
            float dotsWidth = dots > 0 ? (dots * dotSpacing) : 0.f;

            float totalW = valSz.x + 4.f + dotsWidth + 2.f;
            float vx = rightEdge - totalW;

            dl->AddText(ImVec2(vx, y + (kRowH - fontSize) * 0.5f), valCol, valBuf);
            vx += valSz.x + 4.f;

            for (int d = 0; d < dots; ++d) {
                dl->AddCircleFilled(
                    ImVec2(vx + d * dotSpacing + 3.f, y + kRowH * 0.5f),
                    3.f, valCol);
            }
        }
    };

    for (size_t i = 0; i < maxRows; ++i) {
        float rowY = rowStartY + i * kRowH;
        if (i < blueTeam.size())
            drawPlayerRow(blueTeam[i], startX, rowY);
        if (i < redTeam.size())
            drawPlayerRow(redTeam[i], startX + colW + 1.f, rowY);
    }

    float divX = startX + colW;
    float divTop = rowStartY;
    float divBot = rowStartY + maxRows * kRowH;
    dl->AddLine(ImVec2(divX, divTop), ImVec2(divX, divBot),
                IM_COL32(0xFF, 0xD7, 0x64, 0x30), 1.f);

    ImGui::Dummy(ImVec2(0.f, maxRows * kRowH + 4.f));

    // Footer divider
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + contentW, p.y),
                    IM_COL32(0xFF, 0xD7, 0x64, 0x20), 1.f);
        ImGui::Dummy(ImVec2(0.f, 4.f));
    }

    auto computeAvg = [](const std::vector<PlayerMorale>& team) -> float {
        if (team.empty()) return 0.f;
        float sum = 0.f;
        for (auto& pm : team) sum += static_cast<float>(pm.morale);
        return sum / static_cast<float>(team.size());
    };

    float blueAvg = computeAvg(blueTeam);
    float redAvg  = computeAvg(redTeam);

    // Footer: team averages
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        char buf[32];

        snprintf(buf, sizeof(buf), "Avg  %.0f%%", blueAvg);
        dl->AddText(ImVec2(p.x + 2.f, p.y), avgColor(blueAvg), buf);

        snprintf(buf, sizeof(buf), "Avg  %.0f%%", redAvg);
        dl->AddText(ImVec2(p.x + colW + 1.f + 2.f, p.y), avgColor(redAvg), buf);

        ImGui::Dummy(ImVec2(0.f, fontSize + 4.f));
    }

    // Bottom bar
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float barW = contentW;
        float barH = 8.f;
        float halfW = barW * 0.5f;

        dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + barW, p.y + barH),
                          IM_COL32(255, 255, 255, 15), 3.f);

        float blueLen = std::min(1.f, std::abs(blueAvg) / 60.f) * halfW;
        if (blueLen > 1.f) {
            dl->AddRectFilled(
                ImVec2(p.x + halfW - blueLen, p.y),
                ImVec2(p.x + halfW, p.y + barH),
                IM_COL32(0x4A, 0xC8, 0xFF, 0xB3), 2.f);
        }

        float redLen = std::min(1.f, std::abs(redAvg) / 60.f) * halfW;
        if (redLen > 1.f) {
            dl->AddRectFilled(
                ImVec2(p.x + halfW, p.y),
                ImVec2(p.x + halfW + redLen, p.y + barH),
                IM_COL32(0xFF, 0x6B, 0x6B, 0xB3), 2.f);
        }

        dl->AddLine(ImVec2(p.x + halfW, p.y), ImVec2(p.x + halfW, p.y + barH),
                    IM_COL32(0xFF, 0xD7, 0x64, 0x60), 1.f);

        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f%%", blueAvg);
        dl->AddText(ImVec2(p.x + 2.f, p.y + barH + 2.f), avgColor(blueAvg), buf);

        snprintf(buf, sizeof(buf), "%.0f%%", redAvg);
        ImVec2 rSz = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(p.x + barW - rSz.x - 2.f, p.y + barH + 2.f), avgColor(redAvg), buf);

        ImGui::Dummy(ImVec2(0.f, barH + fontSize + 4.f));
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}

// ---------------------------------------------------------------------------
// Scene overlays: Match Timer, Jumbo Messages, Morale Boost Timers
// ---------------------------------------------------------------------------

static float sVw(float pct) { return ImGui::GetMainViewport()->Size.x * pct; }
static float sVh(float pct) { return ImGui::GetMainViewport()->Size.y * pct; }

static void FormatMMSS(char* buf, size_t bufSz, float seconds)
{
    int s = std::max(0, static_cast<int>(seconds));
    int m = s / 60;
    int ss = s % 60;
    snprintf(buf, bufSz, "%02d:%02d", m, ss);
}

static int JumboPartyToTeam(int partyValue)
{
    if (partyValue == 1635021873) return 1;
    if (partyValue == 1635021874) return 2;
    return 0;
}

static const char* JumboMessageDisplayText(const std::string& msgType, int team)
{
    const char* side = (team == 1) ? "Blue" : "Red";
    static char buf[128];
    if      (msgType == "BASE_UNDER_ATTACK")       snprintf(buf, sizeof(buf), "%s Base Under Attack",       side);
    else if (msgType == "GUILD_LORD_UNDER_ATTACK")  snprintf(buf, sizeof(buf), "%s Guild Lord Under Attack",  side);
    else if (msgType == "CAPTURED_SHRINE")          snprintf(buf, sizeof(buf), "%s Captured Shrine",          side);
    else if (msgType == "CAPTURED_TOWER")           snprintf(buf, sizeof(buf), "%s Captured Tower",           side);
    else if (msgType == "PARTY_DEFEATED")           snprintf(buf, sizeof(buf), "%s Party Defeated",           side);
    else if (msgType == "MORALE_BOOST")             snprintf(buf, sizeof(buf), "%s Morale Boost",             side);
    else if (msgType == "VICTORY")                  snprintf(buf, sizeof(buf), "%s Victory!",                 side);
    else if (msgType == "FLAWLESS_VICTORY")         snprintf(buf, sizeof(buf), "%s Flawless Victory!",        side);
    else                                            snprintf(buf, sizeof(buf), "%s %s",                       side, msgType.c_str());
    return buf;
}

// Helper: handle drag mode for an overlay element.
// Returns true if currently being dragged; updates the fraction-based position.
bool ReplayWindow::HandleOverlayDrag(int elementIdx, float* fracX, float* fracY,
                                      ImVec2 boxTL, ImVec2 boxBR)
{
    if (m_draggingUIElement != elementIdx) return false;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRect(ImVec2(boxTL.x - 2, boxTL.y - 2), ImVec2(boxBR.x + 2, boxBR.y + 2),
                IM_COL32(0xF5, 0xE4, 0x5A, 180), 4.f, 0, 2.f);
    dl->AddRectFilled(boxTL, boxBR, IM_COL32(0xF5, 0xE4, 0x5A, 30), 4.f);

    const ImGuiViewport* vp = ImGui::GetMainViewport();

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        *fracX += delta.x / vp->Size.x;
        *fracY += delta.y / vp->Size.y;
        *fracX = std::clamp(*fracX, 0.f, 1.f);
        *fracY = std::clamp(*fracY, 0.f, 1.f);
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        m_draggingUIElement = -1;
        SaveUILayout();
    }
    return true;
}

void ReplayWindow::DrawMatchTimer()
{
    ImFont* font = m_latoRegular ? m_latoRegular : ImGui::GetFont();
    float fontSize = font->FontSize;

    float curTime = m_debugTimeline;
    float matchTime = curTime - m_matchStartOffset;

    bool isCountdown = matchTime < 0.f;
    float absTime = fabsf(matchTime);
    FormatMMSS(m_timerBuf, sizeof(m_timerBuf), absTime);

    const char* label = isCountdown ? "Time to start:" : "Time Elapsed:";

    float labelFs = fontSize;
    float timeFs  = fontSize;

    ImVec2 labelSize = font->CalcTextSizeA(labelFs, FLT_MAX, 0.f, label);
    ImVec2 timeSize  = font->CalcTextSizeA(timeFs, FLT_MAX, 0.f, m_timerBuf);
    float boxW = std::max(labelSize.x, timeSize.x);
    float lineH = labelFs + 2.f;
    float boxH = lineH * 2.f;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float posX = m_uiLayout.useCustom ? m_uiLayout.timerX : 0.50f;
    float posY = m_uiLayout.useCustom ? m_uiLayout.timerY : 0.12f;
    float cx   = vp->Pos.x + vp->Size.x * posX;
    float topY = vp->Pos.y + vp->Size.y * posY;

    float padX, padY;
    if (isCountdown) { padX = 12.f; padY = 10.f; }
    else             { padX = 8.f;  padY = 4.f; }

    ImVec2 boxTL(cx - boxW * 0.5f - padX, topY);
    ImVec2 boxBR(cx + boxW * 0.5f + padX, topY + boxH + padY * 2.f);

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    if (isCountdown)
    {
        dl->AddRectFilled(boxTL, boxBR, IM_COL32(0, 0, 0, 204), 8.f);
        dl->AddRect(boxTL, boxBR, IM_COL32(0xB7, 0xB8, 0xB3, 0xFF), 8.f, 0, 1.f);
    }

    ImU32 goldCol = IM_COL32(0xF5, 0xE4, 0xB4, 0xFF);
    ImU32 shA     = IM_COL32(0, 0, 0, 204);
    ImU32 shB     = IM_COL32(0, 0, 0, 230);

    auto drawShadowedText = [&](ImFont* f, float fs, ImVec2 pos, ImU32 col, const char* txt)
    {
        dl->AddText(f, fs, ImVec2(pos.x, pos.y + 1), shA, txt);
        dl->AddText(f, fs, ImVec2(pos.x, pos.y + 1), shB, txt);
        dl->AddText(f, fs, pos, col, txt);
    };

    float labelX = cx - labelSize.x * 0.5f;
    float labelY = boxTL.y + padY;
    drawShadowedText(font, labelFs, ImVec2(labelX, labelY), goldCol, label);

    float timeX = cx - timeSize.x * 0.5f;
    float timeY = labelY + lineH;
    drawShadowedText(font, timeFs, ImVec2(timeX, timeY), goldCol, m_timerBuf);

    HandleOverlayDrag(3, &m_uiLayout.timerX, &m_uiLayout.timerY, boxTL, boxBR);
}

void ReplayWindow::DrawJumboMessages()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float posX = m_uiLayout.useCustom ? m_uiLayout.jumboX : 0.50f;
    float posY = m_uiLayout.useCustom ? m_uiLayout.jumboY : 0.30f;

    // Show drag preview even without an active jumbo message
    if (m_draggingUIElement == 0)
    {
        ImFont* font = m_latoBoldBig ? m_latoBoldBig : ImGui::GetFont();
        float fontSize = font->FontSize;
        const char* preview = "Blue Captured Tower";
        ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, preview);

        float cx = vp->Pos.x + vp->Size.x * posX;
        float ty = vp->Pos.y + vp->Size.y * posY;
        float tx = cx - textSize.x * 0.5f;

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddText(font, fontSize, ImVec2(tx, ty + 1), IM_COL32(0, 0, 0, 150), preview);
        dl->AddText(font, fontSize, ImVec2(tx, ty), IM_COL32(0x99, 0xCB, 0xFD, 180), preview);

        ImVec2 boxTL(tx - 4, ty - 4);
        ImVec2 boxBR(tx + textSize.x + 4, ty + textSize.y + 4);
        HandleOverlayDrag(0, &m_uiLayout.jumboX, &m_uiLayout.jumboY, boxTL, boxBR);
        return;
    }

    if (!m_replayCtx.stocLoaded) return;

    const auto& jumbos = m_replayCtx.stocData.jumbo;
    if (jumbos.empty()) return;

    float curTime = m_debugTimeline;

    const JumboMessageEvent* best = nullptr;
    float bestAge = 999.f;
    for (auto& ev : jumbos)
    {
        float age = curTime - ev.time;
        if (age < 0.f || age > 6.f) continue;
        if (!best || ev.time > best->time)
        {
            best = &ev;
            bestAge = age;
        }
    }

    if (!best) return;

    float alpha = 1.f;
    if (bestAge > 5.f)
        alpha = std::clamp(6.f - bestAge, 0.f, 1.f);
    if (alpha <= 0.f) return;

    int team = JumboPartyToTeam(best->party_value);
    const char* text = JumboMessageDisplayText(best->message, team);

    int a = static_cast<int>(alpha * 255);
    ImU32 teamCol;
    if (team == 1)      teamCol = IM_COL32(0x99, 0xCB, 0xFD, a);
    else if (team == 2) teamCol = IM_COL32(0xFF, 0x99, 0x9A, a);
    else                teamCol = IM_COL32(0xFF, 0xFF, 0xFF, a);

    ImFont* font = m_latoBoldBig ? m_latoBoldBig : ImGui::GetFont();
    float fontSize = font->FontSize;
    ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, text);

    float cx = vp->Pos.x + vp->Size.x * posX;
    float topY = vp->Pos.y + vp->Size.y * posY;

    float tx = cx - textSize.x * 0.5f;
    float ty = topY;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(alpha * 230));
    dl->AddText(font, fontSize, ImVec2(tx, ty + 1), shadow, text);
    dl->AddText(font, fontSize, ImVec2(tx, ty), teamCol, text);
}

void ReplayWindow::DrawMoraleBoostTimers()
{
    if (m_captureEvents.empty() && m_draggingUIElement != 1 && m_draggingUIElement != 2)
        return;

    float curTime = m_debugTimeline;

    float lastCapTime[2] = { -1.f, -1.f };
    int   lastCapTeam = -1;

    for (auto& [t, teamIdx] : m_captureEvents)
    {
        if (t > curTime) break;
        lastCapTime[teamIdx] = t;
        lastCapTeam = teamIdx;
    }

    ImFont* font = m_latoBold ? m_latoBold : ImGui::GetFont();
    float fontSize = font->FontSize;
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    ImU32 shA     = IM_COL32(0, 0, 0, 204);
    ImU32 shB     = IM_COL32(0, 0, 0, 230);

    auto drawShadowed = [&](ImVec2 pos, ImU32 col, const char* txt)
    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddText(font, fontSize, ImVec2(pos.x, pos.y + 2), shA, txt);
        dl->AddText(font, fontSize, ImVec2(pos.x, pos.y + 1), shB, txt);
        dl->AddText(font, fontSize, pos, col, txt);
    };

    auto DrawOneMorale = [&](int teamIdx01, int dragIdx, float* fracX, float* fracY,
                             float defaultX, float defaultY)
    {
        int team = teamIdx01 + 1;
        float px = m_uiLayout.useCustom ? *fracX : defaultX;
        float py = m_uiLayout.useCustom ? *fracY : defaultY;

        bool hasCap = (lastCapTeam >= 0 && lastCapTeam == teamIdx01);

        if (!hasCap && m_draggingUIElement != dragIdx)
            return;

        const char* teamLabel = (team == 1) ? "Blue Morale Boost" : "Red Morale Boost";
        char buf[32] = "02:00";

        if (hasCap)
        {
            float secondsSince = curTime - lastCapTime[teamIdx01];
            if (secondsSince < 0.f && m_draggingUIElement != dragIdx) return;
            int cyclePos = static_cast<int>(floorf(std::max(0.f, secondsSince))) % 120;
            int remaining = 120 - cyclePos;
            FormatMMSS(buf, sizeof(buf), static_cast<float>(remaining));
        }

        ImVec2 labelSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, teamLabel);
        ImVec2 timerSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, buf);
        float blockW = std::max(labelSize.x, timerSize.x);
        float lineH = fontSize + 4.f;

        float anchorX = vp->Pos.x + vp->Size.x * px;
        float anchorY = vp->Pos.y + vp->Size.y * py;
        float x = anchorX - blockW * 0.5f;

        ImU32 teamCol = (team == 1) ? IM_COL32(0x99, 0xCB, 0xFD, 0xFF)
                                    : IM_COL32(0xFF, 0x99, 0x9A, 0xFF);
        ImU32 goldCol = IM_COL32(0xF5, 0xE4, 0xB4, 0xFF);

        float labelX = x + (blockW - labelSize.x) * 0.5f;
        float timerX = x + (blockW - timerSize.x) * 0.5f;

        drawShadowed(ImVec2(labelX, anchorY), teamCol, teamLabel);
        drawShadowed(ImVec2(timerX, anchorY + lineH), goldCol, buf);

        ImVec2 boxTL(x - 2, anchorY - 2);
        ImVec2 boxBR(x + blockW + 2, anchorY + lineH * 2 + 2);
        HandleOverlayDrag(dragIdx, fracX, fracY, boxTL, boxBR);
    };

    DrawOneMorale(0, 1, &m_uiLayout.moBlueX, &m_uiLayout.moBlueY, 0.65f, 0.22f);
    DrawOneMorale(1, 2, &m_uiLayout.moRedX,  &m_uiLayout.moRedY,  0.35f, 0.22f);
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
// Interface Preferences modal — reposition scene overlays
// ---------------------------------------------------------------------------

void ReplayWindow::DrawInterfacePreferences()
{
    ImGui::SetNextWindowSize(ImVec2(480, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Interface Preferences", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::Text("Scene Overlay Positions");
    ImGui::SameLine();
    ImGui::TextDisabled("(stored as viewport %%)");
    ImGui::Separator();
    ImGui::Spacing();

    bool changed = false;

    if (ImGui::Checkbox("Enable custom positions", &m_uiLayout.useCustom))
        changed = true;

    ImGui::Spacing();

    if (!m_uiLayout.useCustom)
        ImGui::BeginDisabled();

    auto PositionRow = [&](const char* label, float* px, float* py, int dragIdx)
    {
        ImGui::PushID(label);
        float vals[2] = { *px * 100.f, *py * 100.f };
        ImGui::SetNextItemWidth(200);
        if (ImGui::DragFloat2("##pos", vals, 0.1f, 0.f, 100.f, "%.1f%%"))
        {
            *px = vals[0] / 100.f;
            *py = vals[1] / 100.f;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        if (ImGui::SmallButton("Move"))
        {
            m_draggingUIElement = dragIdx;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
    };

    PositionRow("Match Timer",      &m_uiLayout.timerX,  &m_uiLayout.timerY,  3);
    PositionRow("Jumbo Message",    &m_uiLayout.jumboX,  &m_uiLayout.jumboY,  0);
    PositionRow("Morale (Blue)",    &m_uiLayout.moBlueX, &m_uiLayout.moBlueY, 1);
    PositionRow("Morale (Red)",     &m_uiLayout.moRedX,  &m_uiLayout.moRedY,  2);

    if (!m_uiLayout.useCustom)
        ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Agent LOD System");
    ImGui::SameLine();
    ImGui::TextDisabled("(distance-based detail levels)");
    ImGui::Spacing();

    if (ImGui::Checkbox("Enable LOD system", &m_uiLayout.lodEnabled))
        changed = true;

    if (!m_uiLayout.lodEnabled)
        ImGui::BeginDisabled();

    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("Cylinder distance", &m_uiLayout.lodDotDist, 1000.f, 10000.f, "%.0f"))
        changed = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(beyond -> dots)");

    if (!m_uiLayout.lodEnabled)
        ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Reset to Defaults", ImVec2(160, 0)))
    {
        m_uiLayout = UILayoutConfig{};
        changed = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(120, 0)))
        ImGui::CloseCurrentPopup();

    if (changed)
        SaveUILayout();

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

    const float sf = 1.f;

    const float barH     = 76.f;
    const float PAD      = 12.f;
    const float PADY     = 6.f;
    const float TRKH     = 4.f;
    const float BTN_H    = 30.f;
    const float PLAY_SZ  = 36.f;
    const float BGAP     = 2.f;
    const float GDIV     = 10.f;
    const float ROW2_OFS = 28.f;
    const float d2r      = IM_PI / 180.f;

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vpH - barH));
    ImGui::SetNextWindowSize(ImVec2(vpW, barH));

    constexpr ImGuiWindowFlags kBarFlags =
        ImGuiWindowFlags_NoTitleBar      | ImGuiWindowFlags_NoResize       |
        ImGuiWindowFlags_NoMove          | ImGuiWindowFlags_NoScrollbar    |
        ImGuiWindowFlags_NoCollapse      | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground    | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,       ImVec4(0.07f, 0.06f, 0.04f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.78f, 0.66f, 0.29f, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.78f, 0.66f, 0.29f, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.60f, 0.54f, 0.41f, 1.0f));

    if (!ImGui::Begin("PlaybackBar", nullptr, kBarFlags))
    {
        ImGui::End();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        return;
    }

    ImDrawList* dl   = ImGui::GetWindowDrawList();
    const ImVec2 O   = ImGui::GetWindowPos();
    ImFont* font     = ImGui::GetFont();
    const float fs   = font->FontSize;
    const float fsSm = fs * 0.82f;

    // ── GW1 Dark-Glass Gold Palette ──────────────────────────────────────
    const ImU32 cBg         = IM_COL32(  8,   9,  12, 140);
    const ImU32 cGlass      = IM_COL32( 18,  16,  10, 150);
    const ImU32 cBorder     = IM_COL32(160, 120,  40,  46);
    const ImU32 cBorderHi   = IM_COL32(200, 168,  75,  82);
    const ImU32 cBorderGlow = IM_COL32(200, 168,  75, 140);
    const ImU32 cGold       = IM_COL32(200, 168,  75, 255);
    const ImU32 cGoldBright = IM_COL32(226, 194, 106, 255);
    const ImU32 cGoldDim    = IM_COL32(122,  96,  32, 255);
    const ImU32 cGoldFill   = IM_COL32(200, 168,  75,  20);
    const ImU32 cText       = IM_COL32(232, 223, 200, 255);
    const ImU32 cTextMid    = IM_COL32(154, 138, 104, 255);
    const ImU32 cTextDim    = IM_COL32(120, 108,  80, 255);
    const ImU32 cDanger     = IM_COL32(192,  80,  74, 255);
    const ImU32 cGreen      = IM_COL32( 90, 170, 120, 255);
    const ImU32 cGreenDim   = IM_COL32( 90, 170, 120,  80);
    const ImU32 cTrackBg    = IM_COL32(200, 168,  75,  18);

    // ── Background: dark bg + glass panel + border + shimmer ─────────────
    dl->AddRectFilled(O, ImVec2(O.x + vpW, O.y + barH), cBg);

    const float px = O.x + 2.f, py = O.y + 2.f;
    const float pw = vpW - 4.f, ph = barH - 4.f;
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph), cGlass, 8.f);
    dl->AddRect(ImVec2(px, py), ImVec2(px + pw, py + ph), cBorderHi, 8.f);

    {
        float sx = px + 12.f * sf, sw = pw - 24.f * sf, sy = py;
        float mx = sx + sw * 0.5f;
        dl->AddRectFilledMultiColor(
            ImVec2(sx, sy), ImVec2(mx, sy + 1.f),
            IM_COL32(200,168,75, 0), IM_COL32(200,168,75,100),
            IM_COL32(200,168,75,100), IM_COL32(200,168,75, 0));
        dl->AddRectFilledMultiColor(
            ImVec2(mx, sy), ImVec2(sx + sw, sy + 1.f),
            IM_COL32(200,168,75,100), IM_COL32(200,168,75, 0),
            IM_COL32(200,168,75, 0), IM_COL32(200,168,75,100));
    }

    // ── State ────────────────────────────────────────────────────────────
    float maxT = std::max(1.f, m_replayCtx.maxReplayTime);
    auto& ctx  = m_replayCtx;

    static const float  speeds[]      = { 0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 4.0f, 8.0f };
    static const char*  speedLabels[] = { "0.25x","0.5x","1x","1.5x","2x","4x","8x" };
    constexpr int       speedCount    = 7;

    const float x0 = px + PAD;
    const float y0 = py + PADY;

    // ══════════════════════════════════════════════════════════════════════
    // ROW 1: SCRUBBER
    // ══════════════════════════════════════════════════════════════════════
    {
        char curBuf[16], totBuf[16];
        FormatTime(m_debugTimeline, curBuf, sizeof(curBuf));
        FormatTime(maxT, totBuf, sizeof(totBuf));

        dl->AddText(font, fs, ImVec2(x0, y0 + 1.f), cText, curBuf);
        float curW = font->CalcTextSizeA(fs, FLT_MAX, 0.f, curBuf).x;

        float sepY = y0 + (fs - fsSm) * 0.5f + 1.f;
        dl->AddText(font, fsSm, ImVec2(x0 + curW + 2.f, sepY), cBorderHi, " / ");
        float sepW = font->CalcTextSizeA(fsSm, FLT_MAX, 0.f, " / ").x;
        dl->AddText(font, fsSm, ImVec2(x0 + curW + 2.f + sepW, sepY), cTextMid, totBuf);
    }

    const float timeW     = 80.f * sf;
    const float badgeW    = 64.f * sf;
    const float trackX    = x0 + timeW + 6.f * sf;
    const float trackW    = pw - PAD * 2.f - timeW - 6.f * sf - badgeW - 6.f * sf;
    const float trackMidY = y0 + 10.f * sf;
    const float trackBarY = trackMidY - TRKH * 0.5f;

    dl->AddRectFilled(
        ImVec2(trackX, trackBarY), ImVec2(trackX + trackW, trackBarY + TRKH), cTrackBg, 2.f);
    dl->AddRect(
        ImVec2(trackX, trackBarY), ImVec2(trackX + trackW, trackBarY + TRKH), cBorder, 2.f);

    float pct  = maxT > 0.f ? std::clamp(m_debugTimeline / maxT, 0.f, 1.f) : 0.f;
    float fillW = trackW * pct;
    if (fillW > 2.f)
    {
        dl->AddRectFilledMultiColor(
            ImVec2(trackX, trackBarY), ImVec2(trackX + fillW, trackBarY + TRKH),
            cGoldDim, cGold, cGold, cGoldDim);
    }

    float hx   = std::clamp(trackX + fillW, trackX + 5.f * sf, trackX + trackW - 5.f * sf);
    float dotR = 5.f * sf;
    dl->AddCircleFilled(ImVec2(hx, trackMidY), dotR, cGoldBright);
    dl->AddCircle(ImVec2(hx, trackMidY), dotR, IM_COL32(255,255,220,100), 0, 1.5f);

    for (int m = 1; (float)m * 60.f <= maxT; m++)
    {
        float tx = trackX + trackW * ((float)m * 60.f / maxT);
        bool  maj = (m % 5 == 0);
        float th  = (maj ? 7.f : 5.f) * sf;
        dl->AddLine(
            ImVec2(tx, trackBarY + TRKH + 2.f * sf),
            ImVec2(tx, trackBarY + TRKH + 2.f * sf + th),
            maj ? cGoldDim : cBorder, 1.f);
    }

    ImGui::SetCursorScreenPos(ImVec2(trackX, y0));
    ImGui::InvisibleButton("##Scrub", ImVec2(trackW, 20.f * sf));
    if (ImGui::IsItemActive())
    {
        float mouseX = ImGui::GetIO().MousePos.x;
        float t = (mouseX - trackX) / trackW;
        m_debugTimeline = std::clamp(t, 0.f, 1.f) * maxT;
    }

    // ── Frame badge (right of track) ─────────────────────────────────────
    {
        long frame = (long)(m_debugTimeline * 30.0);
        char fBuf[24];
        snprintf(fBuf, sizeof(fBuf), "F %ld", frame);
        float bx = trackX + trackW + 6.f * sf;
        float by = y0;
        float bh = 16.f * sf;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + badgeW, by + bh),
                          IM_COL32(0,0,0,100), 3.f);
        dl->AddRect(ImVec2(bx, by), ImVec2(bx + badgeW, by + bh), cBorder, 3.f);
        dl->AddText(font, fsSm,
            ImVec2(bx + 5.f * sf, by + (bh - fsSm) * 0.5f), cTextMid, fBuf);
    }

    // ══════════════════════════════════════════════════════════════════════
    // ROW 2: CONTROLS  (transport buttons centered, chip left, speed right)
    // ══════════════════════════════════════════════════════════════════════
    const float cy = y0 + ROW2_OFS;

    auto FillBtn = [&](float bx, float by, float bw, float bh, bool hov,
                       ImU32 bg0, ImU32 bdr0) {
        ImU32 bg  = hov ? cGoldFill : bg0;
        ImU32 bdr = hov ? cBorderHi : bdr0;
        if ((bg >> 24) > 0)
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), bg, 5.f);
        if ((bdr >> 24) > 0)
            dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), bdr, 5.f);
    };

    auto Divider = [&](float dx) {
        dl->AddLine(ImVec2(dx, cy + 3.f * sf), ImVec2(dx, cy + BTN_H - 3.f * sf), cBorder);
    };

    // Pre-compute transport group width to center it
    const float bk30W = 40.f * sf, bk5W = 34.f * sf;
    const float fw5W  = 34.f * sf, fw30W = 40.f * sf;
    const float stopW = 24.f * sf;
    const float transportW = bk30W + BGAP + bk5W + GDIV + PLAY_SZ + BGAP + stopW
                           + GDIV + fw5W + BGAP + fw30W;
    const float transportX = O.x + (vpW - transportW) * 0.5f;

    // ── Status chip (left-aligned) ───────────────────────────────────────
    {
        float chipW = 66.f;
        float chipX = x0;
        dl->AddRectFilled(ImVec2(chipX, cy), ImVec2(chipX + chipW, cy + BTN_H),
                          IM_COL32(0,0,0,76), 5.f);
        dl->AddRect(ImVec2(chipX, cy), ImVec2(chipX + chipW, cy + BTN_H), cBorder, 5.f);

        bool blinkOn = ((int)(ImGui::GetTime() / 0.65) % 2 == 0);
        ImU32 dotCol = ctx.isPlaying ? (blinkOn ? cGreen : cGreenDim) : cTextDim;
        dl->AddCircleFilled(ImVec2(chipX + 8.f, cy + BTN_H*0.5f), 2.5f, dotCol);

        const char* stLabel = ctx.isPlaying ? "PLAYING" : "STOPPED";
        dl->PushClipRect(ImVec2(chipX, cy), ImVec2(chipX + chipW, cy + BTN_H), true);
        dl->AddText(font, fsSm,
            ImVec2(chipX + 16.f, cy + (BTN_H - fsSm)*0.5f), cTextDim, stLabel);
        dl->PopClipRect();
    }

    // ── Centered transport group ─────────────────────────────────────────
    float cx = transportX;

    auto DrawStepBtn = [&](const char* id, float bw, const char* label,
                           float stepSec, bool forward) {
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::InvisibleButton(id, ImVec2(bw, BTN_H));
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();
        FillBtn(cx, cy, bw, BTN_H, hov, 0, 0);

        ImU32 fg    = hov ? cGoldBright : cTextMid;
        float thick = 1.3f * sf;
        float rad   = 4.f * sf;

        if (!forward)
        {
            float icx = cx + bw - 10.f * sf;
            float icy = cy + BTN_H * 0.5f;
            dl->PathArcTo(ImVec2(icx, icy), rad, 270.f*d2r, (270.f+300.f)*d2r, 24);
            dl->PathStroke(fg, false, thick);
            float tipX = icx + rad - 0.5f*sf, tipY = icy - 1.f*sf;
            dl->AddLine(ImVec2(tipX, tipY), ImVec2(tipX-2.5f*sf, tipY-2.f*sf), fg, thick);
            dl->AddLine(ImVec2(tipX, tipY), ImVec2(tipX+0.8f*sf, tipY-2.5f*sf), fg, thick);
            dl->AddText(font, fsSm,
                ImVec2(cx + 4.f*sf, cy + (BTN_H - fsSm)*0.5f), fg, label);
        }
        else
        {
            float icx = cx + 10.f * sf;
            float icy = cy + BTN_H * 0.5f;
            dl->PathArcTo(ImVec2(icx, icy), rad, 30.f*d2r, (30.f+300.f)*d2r, 24);
            dl->PathStroke(fg, false, thick);
            float tipX = icx - rad + 0.5f*sf, tipY = icy - 1.f*sf;
            dl->AddLine(ImVec2(tipX, tipY), ImVec2(tipX+2.5f*sf, tipY-2.f*sf), fg, thick);
            dl->AddLine(ImVec2(tipX, tipY), ImVec2(tipX-0.8f*sf, tipY-2.5f*sf), fg, thick);
            dl->AddText(font, fsSm,
                ImVec2(cx + 16.f*sf, cy + (BTN_H - fsSm)*0.5f), fg, label);
        }

        if (clk) m_debugTimeline = std::clamp(m_debugTimeline + stepSec, 0.f, maxT);
        if (hov) ImGui::SetTooltip(forward ? "Forward %s" : "Back %s", label);
        cx += bw + BGAP;
    };

    DrawStepBtn("##Bk30", bk30W, "30s", -30.f, false);
    DrawStepBtn("##Bk5",  bk5W,  "5s",  -5.f,  false);

    cx += GDIV;
    Divider(cx - GDIV * 0.5f);

    // ── Play / Pause ─────────────────────────────────────────────────────
    {
        float plaY = cy - (PLAY_SZ - BTN_H) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(cx, plaY));
        ImGui::InvisibleButton("##PlayPause", ImVec2(PLAY_SZ, PLAY_SZ));
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();

        ImU32 bg  = hov ? IM_COL32(200,168,75, 33) : IM_COL32(200,168,75, 18);
        ImU32 bdr = hov ? cBorderGlow : cBorderHi;
        ImU32 fg  = hov ? cGoldBright : cGold;

        dl->AddRectFilled(ImVec2(cx, plaY), ImVec2(cx+PLAY_SZ, plaY+PLAY_SZ), bg, 6.f);
        dl->AddRect(ImVec2(cx, plaY), ImVec2(cx+PLAY_SZ, plaY+PLAY_SZ), bdr, 6.f);

        float pcx = cx + PLAY_SZ * 0.5f;
        float pcy = plaY + PLAY_SZ * 0.5f;

        if (!ctx.isPlaying)
        {
            dl->AddTriangleFilled(
                ImVec2(pcx - 4.f*sf, pcy - 5.f*sf),
                ImVec2(pcx + 5.f*sf, pcy),
                ImVec2(pcx - 4.f*sf, pcy + 5.f*sf), fg);
        }
        else
        {
            dl->AddRectFilled(
                ImVec2(pcx - 4.5f*sf, pcy - 5.f*sf),
                ImVec2(pcx - 1.5f*sf, pcy + 5.f*sf), fg);
            dl->AddRectFilled(
                ImVec2(pcx + 1.5f*sf, pcy - 5.f*sf),
                ImVec2(pcx + 4.5f*sf, pcy + 5.f*sf), fg);
        }

        if (clk) ctx.isPlaying = !ctx.isPlaying;
        if (hov) ImGui::SetTooltip(ctx.isPlaying ? "Pause" : "Play");
        cx += PLAY_SZ + BGAP;
    }

    // ── Stop ─────────────────────────────────────────────────────────────
    {
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::InvisibleButton("##Stop", ImVec2(stopW, BTN_H));
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();

        ImU32 fg  = hov ? cDanger : cTextMid;
        ImU32 bdr = hov ? IM_COL32(192,80,74,100) : IM_COL32(0,0,0,0);
        FillBtn(cx, cy, stopW, BTN_H, hov, 0, bdr);

        float sq  = 8.f * sf;
        float scx = cx + (stopW - sq) * 0.5f;
        float scy = cy + (BTN_H - sq) * 0.5f;
        dl->AddRectFilled(ImVec2(scx, scy), ImVec2(scx+sq, scy+sq), fg);

        if (clk) { m_debugTimeline = 0.f; ctx.isPlaying = false; }
        if (hov) ImGui::SetTooltip("Stop");
        cx += stopW + BGAP;
    }

    cx += GDIV;
    Divider(cx - GDIV * 0.5f);

    DrawStepBtn("##Fw5",  fw5W,  "5s",  +5.f,  true);
    DrawStepBtn("##Fw30", fw30W, "30s", +30.f, true);

    // ── Loop (right of transport) ────────────────────────────────────────
    cx += GDIV;
    Divider(cx - GDIV * 0.5f);
    {
        float loopW = 46.f * sf;
        ImGui::SetCursorScreenPos(ImVec2(cx, cy));
        ImGui::InvisibleButton("##Loop", ImVec2(loopW, BTN_H));
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();

        bool on   = ctx.loopPlayback;
        ImU32 fg  = (hov || on) ? cGoldBright : cTextMid;
        ImU32 bg  = on ? cGoldFill  : IM_COL32(0,0,0,0);
        ImU32 bdr = on ? cBorderHi  : IM_COL32(0,0,0,0);
        FillBtn(cx, cy, loopW, BTN_H, hov, bg, bdr);

        float thick = 1.3f * sf;
        float ox = cx + 4.f*sf, oy = cy + BTN_H * 0.5f;
        dl->AddLine(ImVec2(ox,          oy-3.f*sf), ImVec2(ox+8.f*sf, oy-3.f*sf), fg, thick);
        dl->AddLine(ImVec2(ox+6.f*sf,   oy-6.f*sf), ImVec2(ox+8.f*sf, oy-3.f*sf), fg, thick);
        dl->AddLine(ImVec2(ox+6.f*sf,   oy-0.f*sf), ImVec2(ox+8.f*sf, oy-3.f*sf), fg, thick);
        dl->AddLine(ImVec2(ox+8.f*sf,   oy+3.f*sf), ImVec2(ox,        oy+3.f*sf), fg, thick);
        dl->AddLine(ImVec2(ox+2.f*sf,   oy+0.f*sf), ImVec2(ox,        oy+3.f*sf), fg, thick);
        dl->AddLine(ImVec2(ox+2.f*sf,   oy+6.f*sf), ImVec2(ox,        oy+3.f*sf), fg, thick);

        dl->AddText(font, fsSm,
            ImVec2(cx + 17.f*sf, cy + (BTN_H - fsSm)*0.5f), fg, "Loop");

        if (clk) ctx.loopPlayback = !ctx.loopPlayback;
        if (hov) ImGui::SetTooltip(ctx.loopPlayback ? "Loop: ON" : "Loop: OFF");
    }

    // ── Speed (right-aligned) ────────────────────────────────────────────
    {
        float speedW = 54.f * sf;
        float speedX = px + pw - PAD - speedW;

        dl->AddText(font, fsSm,
            ImVec2(speedX - 34.f*sf, cy + (BTN_H - fsSm)*0.5f + 1.f), cTextDim, "SPEED");

        ImGui::SetCursorScreenPos(ImVec2(speedX, cy));
        ImGui::InvisibleButton("##Speed", ImVec2(speedW, BTN_H));
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();

        FillBtn(speedX, cy, speedW, BTN_H, hov, 0, cBorderHi);

        ImU32 sfg = hov ? cGoldBright : cTextMid;
        const char* sLbl = speedLabels[ctx.speedIndex];
        ImVec2 tsz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, sLbl);
        float tx = speedX + (speedW - tsz.x - 8.f*sf) * 0.5f;
        float ty = cy + (BTN_H - fs) * 0.5f;
        dl->AddText(font, fs, ImVec2(tx, ty), sfg, sLbl);

        float arX = tx + tsz.x + 3.f*sf;
        float arY = ty + fs * 0.35f;
        dl->AddTriangleFilled(
            ImVec2(arX, arY), ImVec2(arX + 4.f*sf, arY),
            ImVec2(arX + 2.f*sf, arY + 3.f*sf), sfg);

        if (clk) ImGui::OpenPopup("SpeedMenu");
        float popupH = speedCount * ImGui::GetFrameHeightWithSpacing() + 8.f;
        ImGui::SetNextWindowPos(ImVec2(speedX, cy - popupH));
        ImGui::SetNextWindowSizeConstraints(ImVec2(speedW, 0), ImVec2(speedW * 2.f, FLT_MAX));
        if (ImGui::BeginPopup("SpeedMenu"))
        {
            for (int i = 0; i < speedCount; i++)
            {
                bool sel = (i == ctx.speedIndex);
                if (ImGui::Selectable(speedLabels[i], sel))
                {
                    ctx.speedIndex    = i;
                    ctx.playbackSpeed = speeds[i];
                }
            }
            ImGui::EndPopup();
        }
        if (hov) ImGui::SetTooltip("Playback speed");
    }

    m_debugTimeline = std::clamp(m_debugTimeline, 0.f, maxT);

    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
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

static std::filesystem::path GetEffectsBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "effects"))
        {
            cached = dir / "Textures" / "effects";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "effects";
    return cached;
}

// ---------------------------------------------------------------------------
// Cast bar gradient textures (1-pixel wide, built once)
// ---------------------------------------------------------------------------

static ComPtr<ID3D11ShaderResourceView> BuildGradientTex1xN(
    ID3D11Device* device, int height, const GradStop* stops, int nStops)
{
    std::vector<uint32_t> pixels(height);
    for (int y = 0; y < height; ++y)
    {
        float t = (height > 1) ? float(y) / float(height - 1) : 0.f;
        float R, G, B;
        SampleGradient(stops, nStops, t, R, G, B);
        pixels[y] = IM_COL32((uint8_t)R, (uint8_t)G, (uint8_t)B, 255);
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1;
    td.Height = (UINT)height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = pixels.data();
    sd.SysMemPitch = sizeof(uint32_t);

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &sd, &tex))) return nullptr;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return nullptr;
    return srv;
}

static ComPtr<ID3D11ShaderResourceView> BuildCastBarFillTex2D(
    ID3D11Device* device, int width, int height,
    const GradStop* hStops, int nH,
    float topBlackAlpha, float botBlackAlpha)
{
    std::vector<uint32_t> pixels(width * height);
    for (int y = 0; y < height; ++y)
    {
        float v = (height > 1) ? float(y) / float(height - 1) : 0.5f;
        float dark;
        if (v < 0.5f)
            dark = topBlackAlpha * (1.0f - v * 2.0f);
        else
            dark = botBlackAlpha * ((v - 0.5f) * 2.0f);

        for (int x = 0; x < width; ++x)
        {
            float u = (width > 1) ? float(x) / float(width - 1) : 0.f;
            float R, G, B;
            SampleGradient(hStops, nH, u, R, G, B);
            R *= (1.0f - dark);
            G *= (1.0f - dark);
            B *= (1.0f - dark);
            pixels[y * width + x] = IM_COL32(
                (uint8_t)std::clamp(R, 0.f, 255.f),
                (uint8_t)std::clamp(G, 0.f, 255.f),
                (uint8_t)std::clamp(B, 0.f, 255.f), 255);
        }
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)width;
    td.Height = (UINT)height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = pixels.data();
    sd.SysMemPitch = (UINT)(width * sizeof(uint32_t));

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &sd, &tex))) return nullptr;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return nullptr;
    return srv;
}

void ReplayWindow::EnsureCastBarTextures()
{
    if (m_castBarBgTex) return;
    ID3D11Device* dev = m_deviceResources->GetD3DDevice();
    if (!dev) return;

    constexpr int H = 64;
    constexpr int W = 512;
    m_castBarTexH = H;

    // Background: symmetric black vignette (1xN, vertical only)
    static const GradStop bgStops[] = {
        { 0.00f,  0,  0,  0 },
        { 0.25f, 18, 18, 18 },
        { 0.50f, 36, 36, 36 },
        { 0.75f, 18, 18, 18 },
        { 1.00f,  0,  0,  0 }
    };
    m_castBarBgTex = BuildGradientTex1xN(dev, H, bgStops, 5);

    // Green casting fill (horizontal gradient + vertical vignette 55%/50%)
    static const GradStop greenH[] = {
        { 0.000f,  10, 10, 10 },
        { 0.200f,  26, 58, 10 },
        { 0.400f,  64,176, 32 },
        { 0.600f, 168,240, 80 },
        { 0.800f, 200,255,112 },
        { 1.000f, 144,224, 64 }
    };
    m_castBarFillTex = BuildCastBarFillTex2D(dev, W, H, greenH, 6, 0.55f, 0.50f);

    // Orange cancelled fill (horizontal gradient + vertical vignette 58%/52%)
    static const GradStop orangeH[] = {
        { 0.000f,  10,  8,  0 },
        { 0.143f,  58, 30,  0 },
        { 0.286f, 122, 58,  0 },
        { 0.429f, 192, 96,  0 },
        { 0.571f, 232,144, 16 },
        { 0.714f, 255,184, 32 },
        { 0.857f, 255,208, 64 },
        { 1.000f, 232,160, 16 }
    };
    m_castBarCancelTex = BuildCastBarFillTex2D(dev, W, H, orangeH, 8, 0.58f, 0.52f);
}

// ---------------------------------------------------------------------------
// Skill icon index & loader (Textures/Skill_Icons/[ID] - Name.jpg)
// ---------------------------------------------------------------------------

static std::filesystem::path GetSkillIconsBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "Skill_Icons"))
        {
            cached = dir / "Textures" / "Skill_Icons";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "Skill_Icons";
    return cached;
}

void ReplayWindow::EnsureSkillIconIndex()
{
    if (m_skillIconIndexBuilt) return;
    m_skillIconIndexBuilt = true;

    // Primary folder: Textures/Skill_Icons/ with "[ID] - Name.jpg" naming
    auto folder = GetSkillIconsBasePath();
    if (std::filesystem::exists(folder))
    {
        for (const auto& entry : std::filesystem::directory_iterator(folder))
        {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            if (name.size() < 4 || name[0] != '[') continue;
            size_t closeBracket = name.find(']', 1);
            if (closeBracket == std::string::npos) continue;
            int skillId = 0;
            try { skillId = std::stoi(name.substr(1, closeBracket - 1)); }
            catch (...) { continue; }
            m_skillIconIndex[skillId] = entry.path().string();
        }
    }

    // Fallback folder: Textures/skills/ with "{id}.jpg" naming
    wchar_t exeBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    auto exeDir = std::filesystem::path(exeBuf).parent_path();
    for (auto dir = exeDir; ; dir = dir.parent_path())
    {
        auto alt = dir / "Textures" / "skills";
        if (std::filesystem::exists(alt))
        {
            for (const auto& entry : std::filesystem::directory_iterator(alt))
            {
                if (!entry.is_regular_file()) continue;
                auto stem = entry.path().stem().string();
                int skillId = 0;
                try { skillId = std::stoi(stem); }
                catch (...) { continue; }
                if (m_skillIconIndex.find(skillId) == m_skillIconIndex.end())
                    m_skillIconIndex[skillId] = entry.path().string();
            }
            break;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
    }
}

static ImTextureID LoadSkillIcon(ReplayWindow* rw, ID3D11Device* device,
                                 int skillId,
                                 std::unordered_map<int, std::string>& index,
                                 std::unordered_map<int, ComPtr<ID3D11ShaderResourceView>>& cache)
{
    auto cit = cache.find(skillId);
    if (cit != cache.end()) return (ImTextureID)cit->second.Get();

    auto iit = index.find(skillId);
    if (iit == index.end()) return nullptr;

    std::wstring wpath(iit->second.begin(), iit->second.end());
    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(wpath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) return nullptr;

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) return nullptr;

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImages(), DXGI_FORMAT_R8G8B8A8_UNORM,
                              DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) return nullptr;
    }
    const DirectX::Image* src = (converted.GetImageCount() > 0) ? converted.GetImages() : image.GetImages();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)src->width;
    td.Height = (UINT)src->height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = src->pixels;
    sd.SysMemPitch = (UINT)src->rowPitch;

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&td, &sd, &tex);
    if (FAILED(hr)) return nullptr;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (FAILED(hr)) return nullptr;

    cache[skillId] = srv;
    return (ImTextureID)srv.Get();
}

// ---------------------------------------------------------------------------
// Profession icon paths & loaders
// ---------------------------------------------------------------------------

static std::filesystem::path GetProfIconsBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "Professions_Icons"))
        {
            cached = dir / "Textures" / "Professions_Icons";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "Professions_Icons";
    return cached;
}

static std::filesystem::path GetProfStylizedBasePath()
{
    static std::filesystem::path cached;
    if (!cached.empty()) return cached;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 5; i++)
    {
        if (std::filesystem::exists(dir / "Textures" / "professions" / "Profession stylized"))
        {
            cached = dir / "Textures" / "professions" / "Profession stylized";
            return cached;
        }
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    cached = std::filesystem::path(exePath).parent_path() / "Textures" / "professions" / "Profession stylized";
    return cached;
}

static const char* ProfIconFileName(int profId)
{
    switch (profId) {
    case 1:  return "[1] - Warrior.png";
    case 2:  return "[2] - Ranger.png";
    case 3:  return "[3] - Monk.png";
    case 4:  return "[4] - Necromancer.png";
    case 5:  return "[5] - Mesmer.png";
    case 6:  return "[6] - Elementalist.png";
    case 7:  return "[7] - Assassin.png";
    case 8:  return "[8] - Ritualist.png";
    case 9:  return "[9] - Paragon.png";
    case 10: return "[10] - Dervish.png";
    default: return nullptr;
    }
}

static ImTextureID LoadProfIconGeneric(ID3D11Device* device,
                                       const std::filesystem::path& basePath,
                                       const std::string& key)
{
    static ID3D11Device* s_dev = nullptr;
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_dev) { s_cache.clear(); s_dev = device; }

    auto it = s_cache.find(key);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    auto fullPath = basePath / std::filesystem::path(key);
    if (!std::filesystem::exists(fullPath)) { s_cache[key] = nullptr; return nullptr; }

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) { s_cache[key] = nullptr; return nullptr; }

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) { s_cache[key] = nullptr; return nullptr; }

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) { s_cache[key] = nullptr; return nullptr; }
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
    if (FAILED(hr)) { s_cache[key] = nullptr; return nullptr; }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), &srvDesc, srv.GetAddressOf());
    if (FAILED(hr)) { s_cache[key] = nullptr; return nullptr; }

    s_cache[key] = srv;
    return (ImTextureID)srv.Get();
}

static ImTextureID LoadProfIcon(ID3D11Device* device, int profId)
{
    const char* fn = ProfIconFileName(profId);
    if (!fn) return nullptr;
    return LoadProfIconGeneric(device, GetProfIconsBasePath(), fn);
}

static ImTextureID LoadProfStylized(ID3D11Device* device, int profId)
{
    if (profId < 1 || profId > 10) return nullptr;
    char fn[16]; snprintf(fn, sizeof(fn), "%d.png", profId);
    return LoadProfIconGeneric(device, GetProfStylizedBasePath(), fn);
}

static ImTextureID LoadProfIconCL(ID3D11Device* device, int profId)
{
    if (profId < 1 || profId > 10) return nullptr;

    static ID3D11Device* s_dev = nullptr;
    static std::unordered_map<int, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_dev) { s_cache.clear(); s_dev = device; }

    auto it = s_cache.find(profId);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    static std::filesystem::path basePath;
    if (basePath.empty())
    {
        wchar_t exeBuf[MAX_PATH];
        GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
        auto dir = std::filesystem::path(exeBuf).parent_path();
        for (int i = 0; i < 5; i++)
        {
            if (std::filesystem::exists(dir / "Textures" / "professions"))
            { basePath = dir / "Textures" / "professions"; break; }
            if (!dir.has_parent_path() || dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
        if (basePath.empty())
            basePath = std::filesystem::path(exeBuf).parent_path() / "Textures" / "professions";
    }

    char fn[16];
    snprintf(fn, sizeof(fn), "%d.png", profId);
    auto fullPath = basePath / fn;
    if (!std::filesystem::exists(fullPath)) { s_cache[profId] = nullptr; return nullptr; }

    DirectX::ScratchImage image;
    HRESULT hr = DirectX::LoadFromWICFile(fullPath.c_str(), DirectX::WIC_FLAGS_NONE, nullptr, image);
    if (FAILED(hr)) { s_cache[profId] = nullptr; return nullptr; }

    const auto& meta = image.GetMetadata();
    if (meta.width == 0 || meta.height == 0) { s_cache[profId] = nullptr; return nullptr; }

    DirectX::ScratchImage converted;
    if (meta.format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        hr = DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) { s_cache[profId] = nullptr; return nullptr; }
    }
    const auto* img = (converted.GetImageCount() > 0 ? converted : image).GetImage(0, 0, 0);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)img->width; td.Height = (UINT)img->height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = img->pixels; sd.SysMemPitch = (UINT)img->rowPitch;

    ComPtr<ID3D11Texture2D> tex;
    hr = device->CreateTexture2D(&td, &sd, &tex);
    if (FAILED(hr)) { s_cache[profId] = nullptr; return nullptr; }

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (FAILED(hr)) { s_cache[profId] = nullptr; return nullptr; }

    s_cache[profId] = srv;
    return (ImTextureID)srv.Get();
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

static ImTextureID LoadEffectIcon(ID3D11Device* device, const char* filename)
{
    static ID3D11Device* s_cachedDevice = nullptr;
    static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> s_cache;
    if (device != s_cachedDevice) { s_cache.clear(); s_cachedDevice = device; }

    auto it = s_cache.find(filename);
    if (it != s_cache.end()) return (ImTextureID)it->second.Get();

    auto fullPath = GetEffectsBasePath() / std::filesystem::path(filename);
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
    icons.weaponSpell = LoadEffectIcon(dev, "WeaponSpell.png");
    icons.enchanted   = LoadPartyIcon(dev, "Enchanted.png");
    icons.condition   = LoadPartyIcon(dev, "Condition.png");
    icons.hexed       = LoadPartyIcon(dev, "Hexed.png");
    return icons;
}

static void DrawPartyHealthBar(
    ImDrawList* dl, ImVec2 barTL, float barW, float barH,
    const AgentSnapshot* snap, uint8_t teamId, bool isDead,
    const char* name, const PartyIcons& icons,
    int followedAgentId, int agentId, bool fogHidden = false)
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

    if (isFollowed)
    {
        dl->AddRectFilled(ImVec2(barTL.x - 2, barTL.y - 2), ImVec2(barBR.x + 2, barBR.y + 2),
                          IM_COL32(0xD8, 0xD0, 0x73, 0x3C), 3.f);
        dl->AddRect(barTL, barBR, borderCol, 0.f, 0, 2.0f);
        dl->AddRect(ImVec2(barTL.x - 1, barTL.y - 1), ImVec2(barBR.x + 1, barBR.y + 1),
                    IM_COL32(0xD8, 0xD0, 0x73, 0x80), 0.f, 0, 1.0f);
    }
    else
    {
        dl->AddRect(barTL, barBR, borderCol, 0.f, 0, 1.0f);
    }

    // Inner area (1px inset from border)
    ImVec2 innerTL(barTL.x + 1, barTL.y + 1);
    ImVec2 innerBR(barBR.x - 1, barBR.y - 1);
    float innerW = innerBR.x - innerTL.x;
    float innerH = innerBR.y - innerTL.y;

    if (!snap) return;

    if (fogHidden)
    {
        dl->AddRectFilled(innerTL, innerBR, IM_COL32(0x18, 0x18, 0x1C, 0xFF));
        const char* fogText = "???";
        ImVec2 ts = ImGui::CalcTextSize(fogText);
        ImVec2 tp(innerTL.x + (innerW - ts.x) * 0.5f, innerTL.y + (innerH - ts.y) * 0.5f);
        dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 0xCC), fogText);
        dl->AddText(tp, IM_COL32(0x80, 0x80, 0x80, 0xFF), fogText);
        return;
    }

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

    // Player name (text with shadow) + eye icon when followed
    if (name && name[0])
    {
        float textOffsetX = 4.f;
        ImVec2 textPos(innerTL.x + textOffsetX, innerTL.y + (innerH - ImGui::GetFontSize()) * 0.5f);
        ImU32 textCol = isDead ? IM_COL32(0x80, 0x80, 0x80, 0xFF) : IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
        if (isFollowed)
            textCol = IM_COL32(0xF5, 0xE4, 0x5A, 0xFF);
        dl->AddText(ImVec2(textPos.x + 1, textPos.y + 1), IM_COL32(0, 0, 0, 0xCC), name);
        dl->AddText(textPos, textCol, name);

        if (isFollowed)
        {
            ImVec2 nameSize = ImGui::CalcTextSize(name);
            float eyeCx = textPos.x + nameSize.x + 10.f;
            float eyeCy = textPos.y + ImGui::GetFontSize() * 0.5f;
            float sz = 5.f;
            ImU32 eyeCol = IM_COL32(0xCB, 0xAA, 0x09, 0xFF);
            // Diamond / crosshair-style focus icon
            ImVec2 top(eyeCx, eyeCy - sz);
            ImVec2 right(eyeCx + sz, eyeCy);
            ImVec2 bottom(eyeCx, eyeCy + sz);
            ImVec2 left(eyeCx - sz, eyeCy);
            dl->AddQuadFilled(top, right, bottom, left, IM_COL32(0xCB, 0xAA, 0x09, 0x60));
            dl->AddQuad(top, right, bottom, left, eyeCol, 1.5f);
            dl->AddCircleFilled(ImVec2(eyeCx, eyeCy), 1.5f, eyeCol, 8);
        }
    }

    // Status icons (right-aligned, hidden when dead)
    // Order right-to-left: WeaponSpell, Enchanted, Condition, Hexed
    if (!isDead)
    {
        const float iconSz = std::min(innerH - 2.f, 18.f);
        float iconX = innerBR.x - 2.f;
        float iconY = innerTL.y + (innerH - iconSz) * 0.5f;

        if (snap->has_weapon_spell && icons.weaponSpell)
        {
            iconX -= iconSz;
            dl->AddImage(icons.weaponSpell, ImVec2(iconX, iconY), ImVec2(iconX + iconSz, iconY + iconSz));
            iconX -= 1.f;
        }

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

// ---------------------------------------------------------------------------
// Combat Log panel
// ---------------------------------------------------------------------------

void ReplayWindow::DrawCombatLog()
{
    if (!m_showCombatLog || !m_combatLogBuilt) return;

    ImGuiIO& io = ImGui::GetIO();
    float vpW = io.DisplaySize.x;
    float vpH = io.DisplaySize.y;

    float minW = 360.f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(minW, 200.f), ImVec2(vpW, vpH));
    ImGui::SetNextWindowSize(ImVec2(720.f, 440.f), ImGuiCond_FirstUseEver);

    // Gold-accented dark panel styling
    ImGui::PushStyleColor(ImGuiCol_WindowBg,       ImVec4(0.055f, 0.063f, 0.078f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,        ImVec4(0.07f, 0.08f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.10f, 0.09f, 0.06f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.16f, 0.12f, 0.06f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Separator,      ImVec4(0.40f, 0.33f, 0.15f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,    ImVec4(1.f, 1.f, 1.f, 0.04f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,  ImVec4(0.80f, 0.68f, 0.30f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1.f, 0.84f, 0.39f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  ImVec4(1.f, 0.84f, 0.39f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 4.f);

    if (!ImGui::Begin("Combat Log", &m_showCombatLog))
    {
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(9);
        return;
    }

    {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        float cx = std::clamp(pos.x, 0.f, std::max(0.f, vpW - sz.x));
        float cy = std::clamp(pos.y, 0.f, std::max(0.f, vpH - sz.y));
        if (cx != pos.x || cy != pos.y)
            ImGui::SetWindowPos(ImVec2(cx, cy));
    }

    const ImU32 uBlue    = IM_COL32(74, 200, 255, 255);
    const ImU32 uRed     = IM_COL32(255, 107, 107, 255);
    const ImU32 uGray    = IM_COL32(154, 164, 177, 255);
    const ImU32 uWhite   = IM_COL32(232, 236, 242, 255);
    const ImU32 uGreen   = IM_COL32(64, 224, 128, 255);
    const ImU32 uOrange  = IM_COL32(255, 159, 64, 255);
    const ImU32 uPurple  = IM_COL32(191, 97, 255, 255);
    const ImU32 uMuted   = IM_COL32(200, 176, 128, 255);
    const ImU32 uKillRed = IM_COL32(255, 80, 80, 255);
    const ImU32 uKillBg  = IM_COL32(208, 72, 72, 31);
    const ImU32 uKillBdr = IM_COL32(204, 48, 48, 255);
    const ImU32 uHoverBg   = IM_COL32(255, 215, 100, 15);
    const ImU32 uSelectBg  = IM_COL32(255, 230, 120, 38);
    const ImU32 uSelectBdr = IM_COL32(255, 215, 100, 200);
    const ImU32 uGoldDim   = IM_COL32(255, 215, 100, 60);
    const ImU32 uTsCol     = IM_COL32(200, 176, 128, 255);
    const ImU32 uDmgRed    = IM_COL32(255, 128, 128, 255);

    auto teamColorU32 = [&](int agentId) -> ImU32 {
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return uGray;
        if (it->second.teamId == 1) return uBlue;
        if (it->second.teamId == 2) return uRed;
        return uGray;
    };

    auto agentNameStr = [&](int agentId) -> std::string {
        if (agentId <= 0) return "";
        auto it = m_replayCtx.agents.find(agentId);
        if (it == m_replayCtx.agents.end()) return std::format("#{}", agentId);
        auto& a = it->second;
        if (a.type == AgentType::Player) return a.playerName;
        if (!a.categoryName.empty()) return a.categoryName;
        return std::format("#{}", agentId);
    };

    // --- Filter bar ---
    {
        // Snapshot to detect changes
        bool prevDmg = m_clFilterDamage, prevHeal = m_clFilterHeals;
        bool prevSkill = m_clFilterSkills, prevIntr = m_clFilterInterrupt, prevCanc = m_clFilterCancel;
        bool prevDeath = m_clFilterDeaths, prevAtk = m_clFilterAttacks, prevJumbo = m_clFilterJumbo;
        int  prevPlayer = m_clFilterPlayerId;

        auto FilterPill = [](const char* label, bool active) -> bool {
            ImVec4 bg  = active ? ImVec4(0.14f, 0.11f, 0.04f, 1.f)
                                : ImVec4(1.f, 1.f, 1.f, 0.05f);
            ImVec4 tx  = active ? ImVec4(1.f, 0.91f, 0.69f, 1.f)
                                : ImVec4(0.60f, 0.64f, 0.69f, 1.f);
            ImVec4 hov = active ? ImVec4(0.20f, 0.16f, 0.06f, 1.f)
                                : ImVec4(1.f, 1.f, 1.f, 0.12f);
            ImVec4 bdr = active ? ImVec4(1.f, 0.84f, 0.39f, 0.80f)
                                : ImVec4(1.f, 1.f, 1.f, 0.08f);
            ImGui::PushStyleColor(ImGuiCol_Button, bg);
            ImGui::PushStyleColor(ImGuiCol_Text, tx);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(bg.x * 0.85f, bg.y * 0.85f, bg.z * 0.85f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border, bdr);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active ? 1.f : 1.f);
            bool clicked = ImGui::Button(label);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(5);
            return clicked;
        };

        float availW = ImGui::GetContentRegionAvail().x;
        float lineX = 0.f;

        auto MaybeSameLine = [&](const char* nextLabel) {
            float est = ImGui::CalcTextSize(nextLabel).x + 24.f;
            if (lineX + est < availW) {
                ImGui::SameLine();
            } else {
                lineX = 0.f;
            }
        };

        // ALL is visually active when every category filter is on
        bool allOn = m_clFilterDamage && m_clFilterHeals && m_clFilterSkills &&
                     m_clFilterInterrupt && m_clFilterCancel &&
                     m_clFilterDeaths && m_clFilterAttacks && m_clFilterJumbo;
        if (FilterPill("All", allOn))
        {
            bool target = !allOn;
            m_clFilterDamage = m_clFilterHeals = m_clFilterSkills = target;
            m_clFilterInterrupt = m_clFilterCancel = m_clFilterDeaths = m_clFilterAttacks = m_clFilterJumbo = target;
        }
        lineX = ImGui::GetItemRectSize().x;

        MaybeSameLine("Damage");
        if (FilterPill("Damage", m_clFilterDamage)) m_clFilterDamage = !m_clFilterDamage;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Heals");
        if (FilterPill("Heals", m_clFilterHeals)) m_clFilterHeals = !m_clFilterHeals;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Skills");
        if (FilterPill("Skills", m_clFilterSkills)) m_clFilterSkills = !m_clFilterSkills;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Interrupt");
        if (FilterPill("Interrupt", m_clFilterInterrupt)) m_clFilterInterrupt = !m_clFilterInterrupt;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Cancel");
        if (FilterPill("Cancel", m_clFilterCancel)) m_clFilterCancel = !m_clFilterCancel;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Deaths");
        if (FilterPill("Deaths", m_clFilterDeaths)) m_clFilterDeaths = !m_clFilterDeaths;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Attacks");
        if (FilterPill("Attacks", m_clFilterAttacks)) m_clFilterAttacks = !m_clFilterAttacks;
        lineX += ImGui::GetItemRectSize().x;
        MaybeSameLine("Jumbo");
        if (FilterPill("Jumbo", m_clFilterJumbo)) m_clFilterJumbo = !m_clFilterJumbo;

        if (m_clFilterPlayerId >= 0)
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.85f, 0.f, 1.f));
            std::string filterLabel = std::format("Filtering: {} [x]",
                agentNameStr(m_clFilterPlayerId));
            if (ImGui::SmallButton(filterLabel.c_str()))
                m_clFilterPlayerId = -1;
            ImGui::PopStyleColor();
        }

        bool filterChanged =
            m_clFilterDamage != prevDmg || m_clFilterHeals != prevHeal ||
            m_clFilterSkills != prevSkill || m_clFilterInterrupt != prevIntr ||
            m_clFilterCancel != prevCanc || m_clFilterDeaths != prevDeath ||
            m_clFilterAttacks != prevAtk || m_clFilterJumbo != prevJumbo ||
            m_clFilterPlayerId != prevPlayer;
        if (filterChanged)
            m_clScrollToSelected = true;
    }

    // --- Skill name filter (autocomplete multi-select) ---
    bool inputFocused = false, inputHovered = false;
    {
        EnsureSkillIconIndex();
        ID3D11Device* sDev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;

        // Show selected skill chips
        if (!m_clFilterSkillIds.empty())
        {
            for (int idx = 0; idx < (int)m_clFilterSkillIds.size(); ++idx)
            {
                int sid = m_clFilterSkillIds[idx];
                std::string chipLabel = GetSkillDisplayName(sid) + " x##sk" + std::to_string(idx);

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.82f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 2));
                if (ImGui::SmallButton(chipLabel.c_str()))
                {
                    m_clFilterSkillIds.erase(m_clFilterSkillIds.begin() + idx);
                    m_clFilterSkillSet.erase(sid);
                    --idx;
                }
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Clear all##clsk"))
            {
                m_clFilterSkillIds.clear();
                m_clFilterSkillSet.clear();
            }
        }

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputTextWithHint("##skillsearch",
            "Filter by skill name...", m_clSkillSearchBuf, sizeof(m_clSkillSearchBuf));
        inputFocused = ImGui::IsItemActive();
        inputHovered = ImGui::IsItemHovered();
        ImVec2 inputMin = ImGui::GetItemRectMin();
        ImVec2 inputMax = ImGui::GetItemRectMax();
        float dropdownW = inputMax.x - inputMin.x;

        if (inputHovered || inputFocused)
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

        if (ImGui::IsItemActivated()) m_clSkillSearchFocused = true;

        bool showDropdown = m_clSkillSearchFocused && m_clSkillSearchBuf[0] != '\0';
        bool dropdownHovered = false;

        if (showDropdown)
        {
            std::string query(m_clSkillSearchBuf);
            for (auto& c : query) c = (char)std::tolower((unsigned char)c);

            struct Match { int id; std::string name; };
            std::vector<Match> matches;

            auto& db = GetSkillDatabase();
            if (db.IsLoaded())
            {
                db.ForEachSkill([&](const SkillInfo& si) {
                    if (si.name.empty()) return;
                    if (m_clFilterSkillSet.count(si.id)) return;
                    std::string lower = si.name;
                    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                    if (lower.find(query) != std::string::npos)
                        matches.push_back({si.id, si.name});
                });
            }

            for (auto& row : m_combatLog)
            {
                if (row.skillId <= 0) continue;
                if (m_clFilterSkillSet.count(row.skillId)) continue;
                if (db.IsLoaded() && db.Get(row.skillId)) continue;
                std::string sn = GetSkillDisplayName(row.skillId);
                if (sn.empty() || sn.rfind("Skill ", 0) == 0) continue;
                std::string lower = sn;
                for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                if (lower.find(query) != std::string::npos)
                {
                    bool dup = false;
                    for (auto& m : matches) if (m.id == row.skillId) { dup = true; break; }
                    if (!dup) matches.push_back({row.skillId, sn});
                }
            }

            std::sort(matches.begin(), matches.end(),
                [](const Match& a, const Match& b) { return a.name < b.name; });

            if (!matches.empty())
            {
                int maxShow = std::min((int)matches.size(), 10);
                float rowH = ImGui::GetTextLineHeightWithSpacing();
                float popH = rowH * (float)maxShow + 12.f;
                if ((int)matches.size() > maxShow) popH += rowH;

                ImGui::SetNextWindowPos(ImVec2(inputMin.x, inputMax.y + 2.f));
                ImGui::SetNextWindowSize(ImVec2(dropdownW, popH));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 0.97f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.35f, 1.f));

                ImGui::Begin("##skill_dropdown", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoFocusOnAppearing);

                ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

                dropdownHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

                int shown = 0;
                for (auto& m : matches)
                {
                    if (shown >= 10) { ImGui::TextDisabled("... %d more", (int)matches.size() - 10); break; }
                    ImGui::PushID(m.id);

                    if (sDev)
                    {
                        ImTextureID tex = LoadSkillIcon(this, sDev, m.id,
                            m_skillIconIndex, m_skillIconCache);
                        if (tex)
                        {
                            ImGui::Image(tex, ImVec2(16, 16));
                            ImGui::SameLine();
                        }
                    }

                    if (ImGui::Selectable(m.name.c_str()))
                    {
                        m_clFilterSkillIds.push_back(m.id);
                        m_clFilterSkillSet.insert(m.id);
                        m_clSkillSearchBuf[0] = '\0';
                        m_clSkillSearchFocused = false;
                    }

                    ImGui::PopID();
                    ++shown;
                }

                ImGui::End();
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);
            }
        }

        if (!inputFocused && !dropdownHovered)
            m_clSkillSearchFocused = false;
    }

    ImGui::Separator();

    // --- Pre-filter visible rows ---
    bool skillFilterActive = !m_clFilterSkillSet.empty();
    std::vector<int> filtered;
    filtered.reserve(m_combatLog.size());
    for (int i = 0; i < (int)m_combatLog.size(); ++i)
    {
        auto& row = m_combatLog[i];
        if (row.time > m_debugTimeline) break;

        if (row.category != CombatLogCategory::Jumbo &&
            m_clFilterPlayerId >= 0 &&
            row.casterId != m_clFilterPlayerId &&
            row.targetId != m_clFilterPlayerId)
            continue;

        if (row.category != CombatLogCategory::Jumbo &&
            skillFilterActive && !m_clFilterSkillSet.count(row.skillId))
            continue;

        // No filters active → show everything
        bool noFilter = !m_clFilterDamage && !m_clFilterHeals && !m_clFilterSkills &&
                        !m_clFilterInterrupt && !m_clFilterCancel &&
                        !m_clFilterDeaths && !m_clFilterAttacks && !m_clFilterJumbo;
        bool show = noFilter;
        if (!show) {
            if (row.category == CombatLogCategory::Skill && row.interrupted)
                show = m_clFilterInterrupt || m_clFilterSkills;
            else if (row.category == CombatLogCategory::Skill && row.cancelled)
                show = m_clFilterCancel || m_clFilterSkills;
            else switch (row.category) {
            case CombatLogCategory::Damage:      show = m_clFilterDamage || (row.skillId > 0 && m_clFilterSkills); break;
            case CombatLogCategory::Heal:        show = m_clFilterHeals;  break;
            case CombatLogCategory::Skill:       show = m_clFilterSkills; break;
            case CombatLogCategory::Death:       show = m_clFilterDeaths; break;
            case CombatLogCategory::Interrupt:   show = m_clFilterInterrupt; break;
            case CombatLogCategory::KnockDown:   show = m_clFilterSkills; break;
            case CombatLogCategory::Block:       show = m_clFilterSkills; break;
            case CombatLogCategory::BasicAttack: show = m_clFilterAttacks; break;
            case CombatLogCategory::Jumbo:       show = m_clFilterJumbo;  break;
            case CombatLogCategory::Other:       show = true;             break;
            }
        }
        if (show) filtered.push_back(i);
    }

    // --- Column layout ---
    constexpr float kRowH      = 20.f;
    constexpr float kColTs     = 0.f;
    constexpr float kTsW       = 92.f;
    constexpr float kColCProf  = 92.f;    // caster profession icon
    constexpr float kProfSz    = 16.f;
    constexpr float kColCast   = 110.f;   // 92 + 16 + 2 gap
    constexpr float kCastW     = 130.f;
    constexpr float kColArr1   = 240.f;
    constexpr float kArr1W     = 16.f;
    constexpr float kColIcon   = 256.f;
    constexpr float kIconSz    = 18.f;
    constexpr float kColSkill  = 278.f;   // 256 + 18 + 4 gap
    constexpr float kSkillW    = 140.f;
    constexpr float kColArr2   = 418.f;
    constexpr float kArr2W     = 16.f;
    constexpr float kColTProf  = 434.f;   // target profession icon
    constexpr float kColTgt    = 452.f;   // 434 + 16 + 2 gap
    constexpr float kTgtW      = 130.f;
    constexpr float kColVal    = 582.f;
    constexpr float kValW      = 100.f;
    constexpr float kRowW      = 682.f;

    // --- Column headers ---
    {
        ImDrawList* hdl = ImGui::GetWindowDrawList();
        float hx = ImGui::GetCursorScreenPos().x;
        float hy = ImGui::GetCursorScreenPos().y;
        const ImU32 hdrCol = IM_COL32(200, 176, 128, 220);
        float hdrY = hy + 1.f;

        hdl->AddText(ImVec2(hx + kColTs,    hdrY), hdrCol, "Time");
        hdl->AddText(ImVec2(hx + kColCast,  hdrY), hdrCol, "Caster");
        hdl->AddText(ImVec2(hx + kColSkill, hdrY), hdrCol, "Skill");
        hdl->AddText(ImVec2(hx + kColTgt,   hdrY), hdrCol, "Target");
        hdl->AddText(ImVec2(hx + kColVal,   hdrY), hdrCol, "Value");

        float lineY = hy + ImGui::GetTextLineHeightWithSpacing();
        hdl->AddLine(ImVec2(hx, lineY), ImVec2(hx + kRowW, lineY),
            IM_COL32(255, 215, 100, 40));

        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeightWithSpacing() + 2.f));
    }

    // --- Scrolling log region ---
    float footerH = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##logscroll", ImVec2(0, -footerH), false,
        ImGuiWindowFlags_HorizontalScrollbar);

    EnsureSkillIconIndex();
    ID3D11Device* dev = m_deviceResources ? m_deviceResources->GetD3DDevice() : nullptr;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize();

    ImGuiListClipper clipper;
    clipper.Begin((int)filtered.size(), kRowH);
    while (clipper.Step())
    {
        for (int idx = clipper.DisplayStart; idx < clipper.DisplayEnd; ++idx)
        {
            auto& row = m_combatLog[filtered[idx]];
            ImGui::PushID(idx);

            float startX = ImGui::GetCursorScreenPos().x;
            float startY = ImGui::GetCursorScreenPos().y;

            if (ImGui::InvisibleButton("##r", ImVec2(kRowW, kRowH)))
            {
                m_clSelectedRowIdx = filtered[idx];
                float relX = io.MousePos.x - startX;
                if (relX >= kColCast && relX < kColCast + kCastW)
                    m_clFilterPlayerId = row.casterId;
                else
                {
                    m_debugTimeline = row.time;
                    if (row.casterId > 0)
                        EnterFollowMode(row.casterId);
                }
            }
            bool hovered = ImGui::IsItemHovered();
            bool selected = (filtered[idx] == m_clSelectedRowIdx);

            if (row.category == CombatLogCategory::Death)
            {
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + kRowW, startY + kRowH), uKillBg);
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + 2.f, startY + kRowH), uKillBdr);
            }
            else if (row.category == CombatLogCategory::Jumbo)
            {
                ImU32 jbBg = (row.jumboTeam == 1)
                    ? IM_COL32(74, 200, 255, 20)
                    : (row.jumboTeam == 2)
                        ? IM_COL32(255, 107, 107, 20)
                        : IM_COL32(255, 215, 100, 20);
                ImU32 jbBdr = (row.jumboTeam == 1)
                    ? IM_COL32(74, 200, 255, 160)
                    : (row.jumboTeam == 2)
                        ? IM_COL32(255, 107, 107, 160)
                        : IM_COL32(255, 215, 100, 160);
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + kRowW, startY + kRowH), jbBg);
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + 2.f, startY + kRowH), jbBdr);
            }

            if (selected)
            {
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + kRowW, startY + kRowH), uSelectBg);
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + 2.f, startY + kRowH), uSelectBdr);
            }

            if (hovered)
                dl->AddRectFilled(
                    ImVec2(startX, startY),
                    ImVec2(startX + kRowW, startY + kRowH), uHoverBg);

            float textY = startY + (kRowH - fontSize) * 0.5f;

            // Timestamp (left-aligned, bracketed)
            {
                float matchTime = row.time - 60.f;
                int totalMs = (int)(std::abs(matchTime) * 1000.f);
                char tsBuf[20];
                snprintf(tsBuf, sizeof(tsBuf), "[%02d:%02d.%03d]",
                         totalMs / 60000, (totalMs / 1000) % 60, totalMs % 1000);
                dl->PushClipRect(
                    ImVec2(startX + kColTs, startY),
                    ImVec2(startX + kColTs + kTsW, startY + kRowH), true);
                dl->AddText(
                    ImVec2(startX + kColTs, textY), uTsCol, tsBuf);
                dl->PopClipRect();
            }

            // --- Death row: [Skull] [ProfIcon] PlayerName died ---
            if (row.category == CombatLogCategory::Death)
            {
                float cx = startX + kColCProf;

                ImTextureID skullTex = LoadFlagIcon(dev, "death.png");
                if (skullTex)
                {
                    float iy = startY + (kRowH - kIconSz) * 0.5f;
                    dl->AddImage(skullTex,
                        ImVec2(cx, iy),
                        ImVec2(cx + kIconSz, iy + kIconSz));
                    cx += kIconSz + 4.f;
                }

                auto cIt = m_replayCtx.agents.find(row.casterId);
                if (cIt != m_replayCtx.agents.end() && cIt->second.primaryProf > 0 && dev)
                {
                    ImTextureID pTex = LoadProfIcon(dev, cIt->second.primaryProf);
                    if (pTex)
                    {
                        float iy = startY + (kRowH - kProfSz) * 0.5f;
                        dl->AddImage(pTex,
                            ImVec2(cx, iy),
                            ImVec2(cx + kProfSz, iy + kProfSz));
                        cx += kProfSz + 4.f;
                    }
                }

                std::string name = agentNameStr(row.casterId);
                ImU32 col = teamColorU32(row.casterId);
                std::string deathText = name + " died";
                dl->PushClipRect(
                    ImVec2(cx, startY),
                    ImVec2(startX + kColVal + kValW, startY + kRowH), true);
                dl->AddText(ImVec2(cx, textY), col, deathText.c_str());
                dl->PopClipRect();

                ImGui::PopID();
                continue;
            }

            // --- Jumbo row: [Icon] Message ---
            if (row.category == CombatLogCategory::Jumbo)
            {
                float cx = startX + kColCProf;

                const char* jIcon = nullptr;
                if (row.eventType == "BASE_UNDER_ATTACK")
                    jIcon = "damagedone.png";
                else if (row.eventType == "GUILD_LORD_UNDER_ATTACK")
                    jIcon = "kill.png";
                else if (row.eventType == "CAPTURED_SHRINE")
                    jIcon = "Health_Shrine_Bonus.jpg";
                else if (row.eventType == "CAPTURED_TOWER")
                    jIcon = (row.jumboTeam == 1) ? "Blue_flag_waving.svg.png"
                                                 : "Red_flag_waving.svg.png";
                else if (row.eventType == "PARTY_DEFEATED")
                    jIcon = "death2.png";
                else if (row.eventType == "MORALE_BOOST")
                    jIcon = "Morale_10.png";
                else if (row.eventType == "VICTORY" || row.eventType == "FLAWLESS_VICTORY")
                    jIcon = "cup.webp";

                if (jIcon)
                {
                    ImTextureID jTex = LoadFlagIcon(dev, jIcon);
                    if (jTex)
                    {
                        float iy = startY + (kRowH - kIconSz) * 0.5f;
                        dl->AddImage(jTex,
                            ImVec2(cx, iy),
                            ImVec2(cx + kIconSz, iy + kIconSz));
                        cx += kIconSz + 6.f;
                    }
                }

                ImU32 jCol = (row.jumboTeam == 1) ? uBlue
                           : (row.jumboTeam == 2) ? uRed
                           : uTsCol;
                const char* jText = JumboMessageDisplayText(row.eventType, row.jumboTeam);

                dl->PushClipRect(
                    ImVec2(cx, startY),
                    ImVec2(startX + kColVal + kValW, startY + kRowH), true);
                dl->AddText(ImVec2(cx, textY), jCol, jText);
                dl->PopClipRect();

                ImGui::PopID();
                continue;
            }

            // Caster profession icon (16x16)
            {
                auto cIt = m_replayCtx.agents.find(row.casterId);
                if (cIt != m_replayCtx.agents.end() && cIt->second.primaryProf > 0 && dev)
                {
                    ImTextureID pTex = LoadProfIcon(dev, cIt->second.primaryProf);
                    if (pTex)
                    {
                        float iy = startY + (kRowH - kProfSz) * 0.5f;
                        dl->AddImage(pTex,
                            ImVec2(startX + kColCProf, iy),
                            ImVec2(startX + kColCProf + kProfSz, iy + kProfSz));
                    }
                }
            }

            // Caster name (team-colored, 130px)
            {
                std::string name = agentNameStr(row.casterId);
                ImU32 col = teamColorU32(row.casterId);
                dl->PushClipRect(
                    ImVec2(startX + kColCast, startY),
                    ImVec2(startX + kColCast + kCastW, startY + kRowH), true);
                dl->AddText(
                    ImVec2(startX + kColCast, textY), col, name.c_str());
                dl->PopClipRect();
            }

            // Arrow 1 (16px)
            {
                dl->PushClipRect(
                    ImVec2(startX + kColArr1, startY),
                    ImVec2(startX + kColArr1 + kArr1W, startY + kRowH), true);
                dl->AddText(
                    ImVec2(startX + kColArr1, textY), uMuted,
                    "\xe2\x86\x92");
                dl->PopClipRect();
            }

            // Skill icon (18x18) with gold border
            if (row.skillId > 0 && dev)
            {
                ImTextureID tex = LoadSkillIcon(this, dev, row.skillId,
                    m_skillIconIndex, m_skillIconCache);
                if (tex)
                {
                    float iy = startY + (kRowH - kIconSz) * 0.5f;
                    dl->AddImage(tex,
                        ImVec2(startX + kColIcon, iy),
                        ImVec2(startX + kColIcon + kIconSz, iy + kIconSz));
                    dl->AddRect(
                        ImVec2(startX + kColIcon - 0.5f, iy - 0.5f),
                        ImVec2(startX + kColIcon + kIconSz + 0.5f, iy + kIconSz + 0.5f),
                        uGoldDim);
                }
            }
            else if (row.category == CombatLogCategory::BasicAttack)
            {
                dl->PushClipRect(
                    ImVec2(startX + kColIcon, startY),
                    ImVec2(startX + kColIcon + kIconSz, startY + kRowH), true);
                dl->AddText(
                    ImVec2(startX + kColIcon, textY), uGray,
                    "\xe2\x9a\x94");
                dl->PopClipRect();
            }

            bool selfCast = (row.targetId <= 0 || row.targetId == row.casterId);
            float skillColW = selfCast
                ? (kColVal - kColSkill)
                : kSkillW;

            // Skill name (140px, or extended for self-cast)
            {
                const char* snText = nullptr;
                std::string snBuf;
                ImU32 snCol = uWhite;
                if (row.skillId > 0) {
                    snBuf = GetSkillDisplayName(row.skillId);
                    snText = snBuf.c_str();
                    if (row.interrupted)     snCol = uOrange;
                    else if (row.cancelled)  snCol = uPurple;
                } else if (row.category == CombatLogCategory::BasicAttack) {
                    snText = "Attack";
                    snCol = uGray;
                } else if (!row.eventType.empty()) {
                    snText = row.eventType.c_str();
                    snCol = uGray;
                }
                if (snText) {
                    dl->PushClipRect(
                        ImVec2(startX + kColSkill, startY),
                        ImVec2(startX + kColSkill + skillColW, startY + kRowH),
                        true);
                    dl->AddText(
                        ImVec2(startX + kColSkill, textY), snCol, snText);
                    dl->PopClipRect();
                }
            }

            if (!selfCast)
            {
                // Arrow 2 (16px)
                dl->PushClipRect(
                    ImVec2(startX + kColArr2, startY),
                    ImVec2(startX + kColArr2 + kArr2W, startY + kRowH), true);
                dl->AddText(
                    ImVec2(startX + kColArr2, textY), uMuted,
                    "\xe2\x86\x92");
                dl->PopClipRect();

                // Target profession icon (16x16)
                {
                    auto tIt = m_replayCtx.agents.find(row.targetId);
                    if (tIt != m_replayCtx.agents.end() && tIt->second.primaryProf > 0 && dev)
                    {
                        ImTextureID pTex = LoadProfIcon(dev, tIt->second.primaryProf);
                        if (pTex)
                        {
                            float iy = startY + (kRowH - kProfSz) * 0.5f;
                            dl->AddImage(pTex,
                                ImVec2(startX + kColTProf, iy),
                                ImVec2(startX + kColTProf + kProfSz, iy + kProfSz));
                        }
                    }
                }

                // Target name (team-colored, 130px)
                std::string tn = agentNameStr(row.targetId);
                ImU32 tCol = teamColorU32(row.targetId);
                dl->PushClipRect(
                    ImVec2(startX + kColTgt, startY),
                    ImVec2(startX + kColTgt + kTgtW, startY + kRowH), true);
                dl->AddText(
                    ImVec2(startX + kColTgt, textY), tCol, tn.c_str());
                dl->PopClipRect();
            }

            // Value (right-aligned, 60px)
            {
                char valBuf[32] = {};
                ImU32 valCol = uWhite;

                switch (row.category) {
                case CombatLogCategory::Damage:
                    snprintf(valBuf, sizeof(valBuf), "-%d%%",
                        (int)(std::abs(row.valuePct) * 100.f));
                    valCol = uDmgRed;
                    break;
                case CombatLogCategory::Heal:
                    snprintf(valBuf, sizeof(valBuf), "+%d%%",
                        (int)(std::abs(row.valuePct) * 100.f));
                    valCol = uGreen;
                    break;
                case CombatLogCategory::Interrupt:
                    snprintf(valBuf, sizeof(valBuf), "INTERRUPTED");
                    valCol = uKillRed;
                    break;
                case CombatLogCategory::KnockDown:
                    snprintf(valBuf, sizeof(valBuf), "KNOCKED DOWN");
                    valCol = uOrange;
                    break;
                case CombatLogCategory::Death:
                    snprintf(valBuf, sizeof(valBuf), "KILLED");
                    valCol = uKillRed;
                    break;
                case CombatLogCategory::Block:
                    snprintf(valBuf, sizeof(valBuf), "BLOCKED");
                    valCol = uGray;
                    break;
                case CombatLogCategory::Skill:
                    if (row.interrupted) {
                        snprintf(valBuf, sizeof(valBuf), "INTERRUPTED");
                        valCol = uKillRed;
                    } else if (row.cancelled) {
                        snprintf(valBuf, sizeof(valBuf), "CANCELLED");
                        valCol = uOrange;
                    }
                    break;
                default:
                    break;
                }

                if (valBuf[0])
                {
                    float tw = font->CalcTextSizeA(
                        fontSize, FLT_MAX, 0.f, valBuf).x;
                    float tx = (tw <= kValW)
                        ? (startX + kColVal + kValW - tw)
                        : (startX + kColVal);
                    dl->PushClipRect(
                        ImVec2(startX + kColVal, startY),
                        ImVec2(startX + kColVal + kValW, startY + kRowH),
                        true);
                    dl->AddText(ImVec2(tx, textY), valCol, valBuf);
                    dl->PopClipRect();
                }
            }

            ImGui::PopID();
        }
    }
    clipper.End();

    // Scroll to selected row when filters change
    if (m_clScrollToSelected)
    {
        bool found = false;
        if (m_clSelectedRowIdx >= 0)
        {
            for (int i = 0; i < (int)filtered.size(); ++i)
            {
                if (filtered[i] == m_clSelectedRowIdx)
                {
                    float targetY = (float)i * kRowH;
                    float viewH = ImGui::GetWindowHeight();
                    ImGui::SetScrollY(targetY - viewH * 0.5f);
                    m_clAutoScroll = false;
                    found = true;
                    break;
                }
            }
            if (!found)
                m_clSelectedRowIdx = -1;
        }
        m_clScrollToSelected = false;
    }

    // Re-enable auto-scroll whenever playback is active
    if (m_replayCtx.isPlaying)
        m_clAutoScroll = true;

    // Auto-scroll to bottom
    if (m_clAutoScroll)
        ImGui::SetScrollHereY(1.0f);

    // Pause auto-scroll when user scrolls up while NOT playing
    if (!m_replayCtx.isPlaying &&
        ImGui::GetScrollMaxY() > 0.f &&
        ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 20.f)
        m_clAutoScroll = false;

    bool scrollHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    ImGui::EndChild();

    if (scrollHovered && !inputHovered && !inputFocused)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);

    // Footer: resume button when auto-scroll paused
    if (!m_clAutoScroll)
    {
        if (ImGui::Button("Resume"))
            m_clAutoScroll = true;
    }
    else
    {
        ImGui::TextDisabled("Auto-scrolling...");
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(9);
}

// ---------------------------------------------------------------------------

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

            char btnId[32];
            snprintf(btnId, sizeof(btnId), "##PB%d", agentId);
            ImGui::InvisibleButton(btnId, ImVec2(availW, barH));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                EnterFollowMode(agentId);
                if (m_showRangeRings)
                    m_ringAgentFilter = (m_ringAgentFilter == agentId) ? -1 : agentId;
                if (m_fogPerspective > 0)
                    m_fogPlayerAgent = (m_fogPlayerAgent == agentId) ? -1 : agentId;
            }

            bool isFogHidden = (m_fogPerspective > 0 && ard.teamId != m_fogPerspective && IsAgentInFog(agentId));
            DrawPartyHealthBar(dl, cursor, availW, barH,
                               snap, ard.teamId, isDead,
                               ard.partyBarLabel.c_str(), icons,
                               m_followedAgentId, agentId, isFogHidden);
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

    // NPC / Guild Lord skills missing from the skill database
    static const std::unordered_map<int, const char*> s_overrides = {
        {3205, "Entourage"},
    };
    auto ov = s_overrides.find(skillId);
    if (ov != s_overrides.end()) return ov->second;

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
        imguiCaptureKeys  = ImGui::GetIO().WantTextInput || rw->m_clSkillSearchFocused;
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
