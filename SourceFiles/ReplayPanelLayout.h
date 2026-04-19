#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <json.hpp>

class ReplayPanelLayout
{
public:
	struct PanelState
	{
		float x = 0.f, y = 0.f;
		float w = 0.f, h = 0.f;
		bool  visible = true;
		bool  hasSavedPosition = false;
		bool  hasSavedSize     = false;
	};

	struct PanelRegistration
	{
		std::string key;
		std::string displayName;
		bool*       visiblePtr  = nullptr;
		bool        defaultVisible = false;
		bool        trackPosition  = true;
	};

	void RegisterPanel(const char* key, const char* displayName,
	                    bool* visiblePtr, bool defaultVisible,
	                    bool trackPosition = true)
	{
		PanelRegistration reg;
		reg.key            = key;
		reg.displayName    = displayName;
		reg.visiblePtr     = visiblePtr;
		reg.defaultVisible = defaultVisible;
		reg.trackPosition  = trackPosition;
		m_registrations.push_back(std::move(reg));

		if (m_states.find(key) == m_states.end())
		{
			PanelState st;
			st.visible = defaultVisible;
			m_states[key] = st;
		}
	}

	bool HasSavedPosition(const char* key) const
	{
		auto it = m_states.find(key);
		return it != m_states.end() && it->second.hasSavedPosition;
	}

	bool HasSavedSize(const char* key) const
	{
		auto it = m_states.find(key);
		return it != m_states.end() && it->second.hasSavedSize;
	}

	ImVec2 GetSavedPosition(const char* key) const
	{
		auto it = m_states.find(key);
		if (it != m_states.end())
			return ImVec2(it->second.x, it->second.y);
		return ImVec2(0, 0);
	}

	ImVec2 GetSavedSize(const char* key) const
	{
		auto it = m_states.find(key);
		if (it != m_states.end())
			return ImVec2(it->second.w, it->second.h);
		return ImVec2(0, 0);
	}

	void ApplyPosition(const char* key)
	{
		auto it = m_states.find(key);
		if (it != m_states.end() && it->second.hasSavedPosition)
		{
			ImVec2 pos = ClampToScreen(it->second.x, it->second.y,
			                           it->second.w, it->second.h);
			ImGui::SetNextWindowPos(pos, ImGuiCond_Once);
		}
	}

	void ApplySize(const char* key)
	{
		auto it = m_states.find(key);
		if (it != m_states.end() && it->second.hasSavedSize)
			ImGui::SetNextWindowSize(ImVec2(it->second.w, it->second.h), ImGuiCond_Once);
	}

	void TrackWindowByName(const char* key, const char* imguiWindowName)
	{
		ImGuiWindow* win = ImGui::FindWindowByName(imguiWindowName);
		if (!win) return;
		TrackWindowDirect(key, win->Pos, win->Size);
	}

	void TrackWindow(const char* key)
	{
		TrackWindowDirect(key, ImGui::GetWindowPos(), ImGui::GetWindowSize());
	}

	void TrackWindowDirect(const char* key, ImVec2 pos, ImVec2 size)
	{
		auto it = m_states.find(key);
		if (it == m_states.end())
		{
			PanelState st;
			st.x = pos.x;  st.y = pos.y;
			st.w = size.x;  st.h = size.y;
			st.hasSavedPosition = true;
			st.hasSavedSize     = true;
			m_states[key] = st;
			m_dirty = true;
			return;
		}

		auto& st = it->second;
		constexpr float kEps = 0.5f;
		bool moved  = std::abs(st.x - pos.x) > kEps || std::abs(st.y - pos.y) > kEps;
		bool resized = std::abs(st.w - size.x) > kEps || std::abs(st.h - size.y) > kEps;

		if (moved || resized || !st.hasSavedPosition)
		{
			st.x = pos.x;   st.y = pos.y;
			st.w = size.x;  st.h = size.y;
			st.hasSavedPosition = true;
			st.hasSavedSize     = true;
			m_dirty = true;
		}
	}

	void SyncVisibilityFromPointers()
	{
		for (auto& reg : m_registrations)
		{
			if (!reg.visiblePtr) continue;
			auto it = m_states.find(reg.key);
			if (it != m_states.end())
			{
				bool prev = it->second.visible;
				it->second.visible = *reg.visiblePtr;
				if (prev != it->second.visible)
					m_dirty = true;
			}
		}
	}

	void SyncVisibilityToPointers()
	{
		for (auto& reg : m_registrations)
		{
			if (!reg.visiblePtr) continue;
			auto it = m_states.find(reg.key);
			if (it != m_states.end())
				*reg.visiblePtr = it->second.visible;
		}
	}

	bool IsDirty() const { return m_dirty; }
	void ClearDirty() { m_dirty = false; }

	void ResetToDefaults()
	{
		m_states.clear();
		for (auto& reg : m_registrations)
		{
			PanelState st;
			st.visible = reg.defaultVisible;
			m_states[reg.key] = st;
			if (reg.visiblePtr)
				*reg.visiblePtr = reg.defaultVisible;
		}
		m_dirty = true;
	}

	const std::vector<PanelRegistration>& GetRegistrations() const { return m_registrations; }
	const std::unordered_map<std::string, PanelState>& GetStates() const { return m_states; }

	void SaveToJson(nlohmann::json& panels) const
	{
		panels = nlohmann::json::object();
		for (auto& reg : m_registrations)
		{
			auto it = m_states.find(reg.key);
			if (it == m_states.end()) continue;
			auto& st = it->second;

			nlohmann::json p;
			p["visible"] = st.visible;
			if (reg.trackPosition && st.hasSavedPosition)
			{
				p["x"] = st.x;
				p["y"] = st.y;
			}
			if (reg.trackPosition && st.hasSavedSize)
			{
				p["w"] = st.w;
				p["h"] = st.h;
			}
			panels[reg.key] = p;
		}
	}

	void LoadFromJson(const nlohmann::json& panels)
	{
		if (!panels.is_object()) return;

		for (auto& [key, val] : panels.items())
		{
			PanelState st;
			if (val.contains("visible")) st.visible = val["visible"].get<bool>();
			if (val.contains("x") && val.contains("y"))
			{
				st.x = val["x"].get<float>();
				st.y = val["y"].get<float>();
				st.hasSavedPosition = true;
			}
			if (val.contains("w") && val.contains("h"))
			{
				st.w = val["w"].get<float>();
				st.h = val["h"].get<float>();
				st.hasSavedSize = true;
			}
			m_states[key] = st;
		}
	}

private:
	static ImVec2 ClampToScreen(float x, float y, float w, float h)
	{
		ImVec2 display = ImGui::GetIO().DisplaySize;
		const float margin = 50.f;

		if (w < 1.f) w = 200.f;
		if (h < 1.f) h = 100.f;

		if (x + w < margin)       x = margin - w + 100.f;
		if (x > display.x - margin) x = display.x - margin - 100.f;
		if (y < 0.f)                y = 10.f;
		if (y > display.y - margin)  y = display.y - margin - 100.f;

		return ImVec2(x, y);
	}

	std::vector<PanelRegistration>                  m_registrations;
	std::unordered_map<std::string, PanelState>     m_states;
	bool                                            m_dirty = false;
};
