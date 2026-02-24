#include "pch.h"
#include "ReplayWindow.h"
#include "AgentSnapshotParser.h"
#include "StoCParser.h"
#include "SkillDatabase.h"
#include "DXMathHelpers.h"
#include <d3dcompiler.h>
#include <fstream>
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

static void SaveMapTransform(int mapId, const MapTransform& t);
static MapTransform LoadMapTransform(int mapId, bool* found = nullptr);

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

void ReplayWindow::InitImGui()
{
    if (m_imguiInitialized) return;

    ImGuiContext* prevCtx = ImGui::GetCurrentContext();

    m_imguiContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_imguiContext);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;

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
        ClassifyAgents(m_replayCtx.agents, m_matchMeta);

        m_sortedAgentIds.reserve(m_replayCtx.agents.size());
        for (auto& [id, ard] : m_replayCtx.agents)
        {
            m_sortedAgentIds.push_back(id);
            switch (ard.type) {
            case AgentType::Player:  m_playerIds.push_back(id);  break;
            case AgentType::NPC:     m_npcIds.push_back(id);     break;
            case AgentType::Gadget:  m_gadgetIds.push_back(id);  break;
            default:                 m_unknownIds.push_back(id);  break;
            }
        }
        std::sort(m_sortedAgentIds.begin(), m_sortedAgentIds.end());
        std::sort(m_playerIds.begin(),  m_playerIds.end());
        std::sort(m_npcIds.begin(),     m_npcIds.end());
        std::sort(m_gadgetIds.begin(),  m_gadgetIds.end());
        std::sort(m_unknownIds.begin(), m_unknownIds.end());

        m_agentsClassified = true;
    }

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
    m_mapRenderer->Update(elapsedMs / 1000.0);
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
            if (ImGui::MenuItem("Close Replay"))
            {
                PostMessage(m_hwnd, WM_CLOSE, 0, 0);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Agent Overlay", nullptr, &m_showAgentOverlay);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug"))
        {
            ImGui::MenuItem("Agent Data", nullptr, &m_showAgentDataWindow);
            ImGui::MenuItem("Map Calibration", nullptr, &m_showMapCalibrationWindow);
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

    if (m_showAgentDataWindow)
        DrawAgentDataWindow();

    if (m_showMapCalibrationWindow)
        DrawMapCalibrationWindow();

    if (m_showStoCWindow)
        DrawStoCWindow();

    DrawAgentOverlay();

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

static void InterpolateAgentPosition(const AgentReplayData& ard, float t,
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
    int lo = 0, hi = static_cast<int>(snaps.size()) - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (snaps[mid].time <= t) lo = mid; else hi = mid - 1;
    }
    auto& s0 = snaps[lo];
    if (lo + 1 < static_cast<int>(snaps.size())) {
        auto& s1 = snaps[lo + 1];
        float dt = s1.time - s0.time;
        float a = (dt > 0.001f) ? (t - s0.time) / dt : 0.f;
        outX = s0.x + (s1.x - s0.x) * a;
        outY = s0.y + (s1.y - s0.y) * a;
        outZ = s0.z + (s1.z - s0.z) * a;
    } else {
        outX = s0.x; outY = s0.y; outZ = s0.z;
    }
}

static std::string GetAgentLabel(const AgentReplayData& ard)
{
    switch (ard.type) {
    case AgentType::Player: return ard.playerName;
    case AgentType::NPC:    return ard.categoryName;
    case AgentType::Gadget: return ard.categoryName;
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

    for (auto& [agentId, ard] : m_replayCtx.agents)
    {
        if (ard.snapshots.empty()) continue;

        float sx, sy, sz;
        InterpolateAgentPosition(ard, m_debugTimeline, sx, sy, sz);

        // Optional: show raw axis-remapped position (no transform)
        if (m_showRawPositions) {
            XMFLOAT3 rawPos = { sx, sz, sy };
            float rsx, rsy;
            if (ProjectToScreen(viewProj, vpW, vpH, rawPos, rsx, rsy))
                dl->AddCircle(ImVec2(rsx, rsy), 3.f, IM_COL32(255, 255, 0, 120), 0, 1.f);
        }

        XMFLOAT3 pos = ApplyMapTransformToPos(sx, sy, sz, t);
        float scrX, scrY;
        if (!ProjectToScreen(viewProj, vpW, vpH, pos, scrX, scrY)) continue;

        ImU32 dotColor = GetAgentTeamColor(ard.teamId);
        dl->AddCircleFilled(ImVec2(scrX, scrY), dotRadius, dotColor);
        dl->AddCircle(ImVec2(scrX, scrY), dotRadius, IM_COL32(0, 0, 0, 180), 0, 1.5f);

        std::string label = GetAgentLabel(ard);
        ImVec2 textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, label.c_str());
        float lx = scrX - textSize.x * 0.5f;
        float ly = scrY + dotRadius + labelOffY;
        dl->AddText(ImVec2(lx + 1.f, ly + 1.f), IM_COL32(0, 0, 0, 200), label.c_str());
        dl->AddText(ImVec2(lx, ly), IM_COL32(255, 255, 255, 230), label.c_str());
    }
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
    ImGui::Text("Timeline:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##timeline", &m_debugTimeline, 0.f, maxT, "%.1fs");

    ImGui::Text("Players: %d  |  NPCs: %d  |  Gadgets: %d  |  Unknown: %d  |  Total: %d",
                static_cast<int>(m_playerIds.size()),
                static_cast<int>(m_npcIds.size()),
                static_cast<int>(m_gadgetIds.size()),
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
        // Blue team
        bool anyBlue = false;
        for (int id : m_playerIds)
        {
            auto& ard = m_replayCtx.agents[id];
            if (ard.teamId == 1) { anyBlue = true; break; }
        }
        if (anyBlue && ImGui::TreeNodeEx("Blue Team", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (int id : m_playerIds)
            {
                auto& ard = m_replayCtx.agents[id];
                if (ard.teamId == 1) DrawAgentEntry(id, ard);
            }
            ImGui::TreePop();
        }

        // Red team
        bool anyRed = false;
        for (int id : m_playerIds)
        {
            auto& ard = m_replayCtx.agents[id];
            if (ard.teamId == 2) { anyRed = true; break; }
        }
        if (anyRed && ImGui::TreeNodeEx("Red Team", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (int id : m_playerIds)
            {
                auto& ard = m_replayCtx.agents[id];
                if (ard.teamId == 2) DrawAgentEntry(id, ard);
            }
            ImGui::TreePop();
        }

        // Other teams (if any)
        for (int id : m_playerIds)
        {
            auto& ard = m_replayCtx.agents[id];
            if (ard.teamId != 1 && ard.teamId != 2) DrawAgentEntry(id, ard);
        }

        ImGui::TreePop();
    }

    // --- NPCs section ---
    if (!m_npcIds.empty() && ImGui::TreeNodeEx("NPCs", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (int id : m_npcIds)
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
        if (mouseAllowed && rw->m_inputManager)
            rw->m_inputManager->ProcessRawInput(lParam);
        break;

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (mouseAllowed && rw->m_inputManager)
            rw->m_inputManager->OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, hWnd, message);
        break;

    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        if (mouseAllowed && rw->m_inputManager)
            rw->m_inputManager->OnMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), wParam, hWnd, message);
        break;

    case WM_MOUSEWHEEL:
        if (mouseAllowed && rw->m_inputManager)
            rw->m_inputManager->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam), hWnd);
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
