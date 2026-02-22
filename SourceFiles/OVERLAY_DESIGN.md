# Agent Overlay – Design Summary

Guild Wars Observer (based on Guild Wars Map Browser) uses **DirectX 11** (not OpenGL). The runtime overlay that draws agent positions as colored spheres is implemented in **AgentOverlay** and is already integrated into the app.

---

## 1. Codebase structure (relevant parts)

- **Entry / loop:** `Main.cpp` → `MapBrowser::Tick()` → `Update()` + `Render()`.
- **Render path:** `MapBrowser::Render()` → shadows/reflection → clear → **MapRenderer::Render()** (sky, terrain, props, water, shore, pathfinding) → **AgentOverlay::Render()** → picking readback → ImGui → Present.
- **Overlay module:** `AgentOverlay.h` / `AgentOverlay.cpp` – owns sphere mesh, overlay shaders, JSON load, and periodic reload.

The overlay runs as a **separate pass after the main scene**, before picking and UI, so it does not affect picking and stays on top of the 3D scene.

---

## 2. Marker data

**`AgentMarker`** (in `AgentOverlay.h`):

- `int id`
- `DirectX::XMFLOAT3 position` (world x, y, z)
- `DirectX::XMFLOAT4 color` (RGBA)

Markers are stored in `std::vector<AgentMarker> m_markers` inside `AgentOverlay`.

---

## 3. Data source: JSON file

- **File:** `agents.json`, by default next to the executable (e.g. `Debug/agents.json` or `Release/agents.json` when running from Visual Studio). You can change it with `SetJsonPath()`.
- **Format:** JSON array of objects. Each object can have:
  - `id` (optional, number)
  - `x`, `y`, `z` (floats, world position)
  - **Either** `team` (string: `"red"`, `"blue"`, `"green"`, `"yellow"`, `"white"`, `"purple"`, `"orange"`, `"cyan"`) **or** explicit color:
    - `color`: `[r, g, b]` (floats 0–1), or
    - `r`, `g`, `b`, `a` (floats)

Example (see project root `agents.json`):

```json
[
  { "id": 1, "x": 1000, "y": 6000, "z": 1000, "team": "red" },
  { "id": 2, "x": 1200, "y": 6010, "z": 1100, "team": "blue" },
  { "id": 4, "x": 1500, "y": 6020, "z": 1200, "color": [1.0, 1.0, 0.0] }
]
```

---

## 4. Periodic reload

- **`AgentOverlay::Update()`** is called every frame from `MapBrowser::Update()`.
- Reload interval is **150 ms** by default; set with **`SetReloadIntervalMs(int)`**.
- **`LoadMarkersFromJson()`** is called when the interval has elapsed. It also uses **file write time** so the file is only re-parsed when it has changed, avoiding unnecessary work when the file is static.

---

## 5. Rendering behavior

- Markers are drawn as **small spheres** (unit sphere scaled by `m_markerRadius`, default 75 world units).
- **Y offset:** positions are lifted by `m_yOffset` (default 50) so markers sit slightly above the ground.
- **Pipeline:** overlay uses its own VS/PS and a single constant buffer (world-view-proj + color). It uses **depth test disabled** so markers stay visible; rasterizer has culling disabled so spheres look correct from any angle.
- **Render:** After `MapRenderer::Render()`, the same RTV/DSV are still bound; `AgentOverlay::Render(view, proj)` draws one indexed draw per marker, then **restores** the previous pipeline state (VS, PS, input layout, depth-stencil, rasterizer, blend) so the rest of the app is unchanged.

---

## 6. Where to hook into the render loop

In **`MapBrowser::Render()`** (around lines 196–206):

1. `m_map_renderer->Render(GetRenderTargetView(), GetPickingRenderTargetView(), GetDepthStencilView());`
2. **Agent overlay:**  
   `if (m_agent_overlay && m_agent_overlay->IsEnabled())`  
   `m_agent_overlay->Render(camera->GetView(), camera->GetProj());`
3. Then picking resolve/copy and ImGui.

So the overlay is a single block **after** the main scene and **before** picking/UI.

---

## 7. Optional settings (AgentOverlay)

- `SetJsonPath(path)` – path to `agents.json`.
- `SetMarkerRadius(float)` – sphere size in world units.
- `SetReloadIntervalMs(int)` – reload interval (e.g. 100–200 ms).
- `SetYOffset(float)` – height above the given y.
- `SetEnabled(bool)` / `IsEnabled()` – turn overlay on/off.

---

## 8. Using the overlay

1. Copy **`agents.json`** from `sample_data/` into your build output directory (e.g. `x64\Debug\` or `x64\Release\`) so it sits next to the `.exe`, **or** set a full path with `SetJsonPath()`.
2. Run the app and load a map. Agent spheres will appear at the positions in the JSON and will refresh when you save `agents.json` (within the reload interval).

No UI, replay, or networking is required for this MVP; the overlay is a minimal proof-of-concept that reads from a file and draws colored spheres in the 3D world.
