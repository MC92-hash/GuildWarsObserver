#include "pch.h"
#include "CharacterBuilder.h"
#include <algorithm>
#include <charconv>

static const char* s_slotNames[] = {
	"Weapon", "Offhand", "Chest", "Legs", "Head",
	"Feet", "Hands", "Costume Body", "Costume Head"
};

static const char* s_professionNames[] = {
	"None", "Warrior", "Ranger", "Monk", "Necromancer",
	"Mesmer", "Elementalist", "Assassin", "Ritualist",
	"Paragon", "Dervish"
};

const char* CharacterSlotName(int index)
{
	if (index >= 0 && index < CharacterData::SlotCount)
		return s_slotNames[index];
	return "Unknown";
}

const char* ProfessionName(uint32_t id)
{
	if (id < std::size(s_professionNames))
		return s_professionNames[id];
	return "Unknown";
}

void CharacterData::Clear()
{
	name.clear();
	profession = secondary = campaign = sex = level = 0;
	is_npc = false;
	appearance_bitmap = 0;
	appearance = {};
	npc_model_file_id = npc_skin_file_id = 0;
	npc_model_files.clear();
	face_file_id = hair_file_id = 0;
	body_slot_assignments = {};
	body_candidates.clear();
	for (auto& s : slots)
	{
		s = {};
	}
	valid = false;
}

// Minimal JSON helpers (no external lib dependency)
namespace
{
	void SkipWhitespace(const char*& p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }

	bool Expect(const char*& p, char c)
	{
		SkipWhitespace(p);
		if (*p == c) { p++; return true; }
		return false;
	}

	bool ParseString(const char*& p, std::string& out)
	{
		SkipWhitespace(p);
		if (*p != '"') return false;
		p++;
		out.clear();
		while (*p && *p != '"')
		{
			if (*p == '\\' && *(p + 1))
			{
				p++;
				if (*p == '"') out += '"';
				else if (*p == '\\') out += '\\';
				else if (*p == 'n') out += '\n';
				else out += *p;
			}
			else
			{
				out += *p;
			}
			p++;
		}
		if (*p == '"') { p++; return true; }
		return false;
	}

	bool ParseUint(const char*& p, uint32_t& out)
	{
		SkipWhitespace(p);
		const char* start = p;
		while (*p >= '0' && *p <= '9') p++;
		if (p == start) return false;
		auto [ptr, ec] = std::from_chars(start, p, out);
		return ec == std::errc{};
	}

	bool ParseBool(const char*& p, bool& out)
	{
		SkipWhitespace(p);
		if (strncmp(p, "true", 4) == 0) { out = true; p += 4; return true; }
		if (strncmp(p, "false", 5) == 0) { out = false; p += 5; return true; }
		return false;
	}

	void SkipValue(const char*& p)
	{
		SkipWhitespace(p);
		if (*p == '"')
		{
			p++;
			while (*p && *p != '"') { if (*p == '\\' && *(p + 1)) p++; p++; }
			if (*p == '"') p++;
		}
		else if (*p == '{')
		{
			int depth = 1; p++;
			while (*p && depth > 0)
			{
				if (*p == '{') depth++;
				else if (*p == '}') depth--;
				else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && *(p + 1)) p++; p++; } }
				p++;
			}
		}
		else if (*p == '[')
		{
			int depth = 1; p++;
			while (*p && depth > 0)
			{
				if (*p == '[') depth++;
				else if (*p == ']') depth--;
				else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && *(p + 1)) p++; p++; } }
				p++;
			}
		}
		else
		{
			while (*p && *p != ',' && *p != '}' && *p != ']') p++;
		}
	}

	bool ParseUintArray(const char*& p, uint32_t* out, size_t maxCount, size_t& count)
	{
		SkipWhitespace(p);
		if (*p != '[') return false;
		p++;
		count = 0;
		while (true)
		{
			SkipWhitespace(p);
			if (*p == ']') { p++; return true; }
			if (count > 0 && !Expect(p, ',')) return false;
			uint32_t v = 0;
			if (!ParseUint(p, v)) return false;
			if (count < maxCount) out[count] = v;
			count++;
		}
	}

	int BodySlotIndexFromKey(const std::string& key)
	{
		if (key == "feet")  return 0;
		if (key == "legs")  return 1;
		if (key == "chest") return 2;
		if (key == "hands") return 3;
		if (key == "head")  return 4;
		return -1;
	}

	bool ParseBodySlotAssignments(const char*& p, std::array<uint32_t, CharacterData::BodySlotCount>& out)
	{
		if (!Expect(p, '{')) return false;
		while (true)
		{
			SkipWhitespace(p);
			if (*p == '}') { p++; return true; }
			if (*p == ',') p++;

			std::string key;
			if (!ParseString(p, key)) return false;
			if (!Expect(p, ':')) return false;

			int idx = BodySlotIndexFromKey(key);
			if (idx >= 0)
			{
				uint32_t v = 0;
				if (ParseUint(p, v)) out[idx] = v;
				else SkipValue(p);
			}
			else SkipValue(p);
		}
	}

	int SlotIndexFromKey(const std::string& key)
	{
		if (key == "weapon") return CharacterData::Weapon;
		if (key == "offhand") return CharacterData::Offhand;
		if (key == "chest") return CharacterData::Chest;
		if (key == "legs") return CharacterData::Legs;
		if (key == "head") return CharacterData::Head;
		if (key == "feet") return CharacterData::Feet;
		if (key == "hands") return CharacterData::Hands;
		if (key == "costume_body") return CharacterData::CostumeBody;
		if (key == "costume_head") return CharacterData::CostumeHead;
		return -1;
	}

	bool ParseSlotObject(const char*& p, CharacterSlotData& slot)
	{
		if (!Expect(p, '{')) return false;
		while (true)
		{
			SkipWhitespace(p);
			if (*p == '}') { p++; return true; }
			if (*p == ',') p++;

			std::string key;
			if (!ParseString(p, key)) return false;
			if (!Expect(p, ':')) return false;

			if (key == "dat_file_id") { ParseUint(p, slot.dat_file_id); }
			else if (key == "composite_id") { ParseUint(p, slot.composite_id); }
			else if (key == "dye_tint") { ParseUint(p, slot.dye_tint); }
			else if (key == "dyes")
			{
				size_t cnt = 0;
				ParseUintArray(p, slot.dyes.data(), slot.dyes.size(), cnt);
			}
			else if (key == "composite_file_ids")
			{
				size_t cnt = 0;
				ParseUintArray(p, slot.composite_file_ids.data(), slot.composite_file_ids.size(), cnt);
			}
			else SkipValue(p);
		}
	}

	bool ParseSlotsObject(const char*& p, CharacterData& cd)
	{
		if (!Expect(p, '{')) return false;
		while (true)
		{
			SkipWhitespace(p);
			if (*p == '}') { p++; return true; }
			if (*p == ',') p++;

			std::string slotKey;
			if (!ParseString(p, slotKey)) return false;
			if (!Expect(p, ':')) return false;

			int idx = SlotIndexFromKey(slotKey);
			if (idx >= 0)
			{
				CharacterSlotData slot;
				slot.name = slotKey;
				if (!ParseSlotObject(p, slot)) return false;
				cd.slots[idx] = slot;
			}
			else
			{
				SkipValue(p);
			}
		}
	}

	bool ParseAppearanceObject(const char*& p, CharacterData::AppearanceDecode& a)
	{
		if (!Expect(p, '{')) return false;
		while (true)
		{
			SkipWhitespace(p);
			if (*p == '}') { p++; return true; }
			if (*p == ',') p++;

			std::string key;
			if (!ParseString(p, key)) return false;
			if (!Expect(p, ':')) return false;

			if (key == "hair_style") ParseUint(p, a.hair_style);
			else if (key == "hair_color") ParseUint(p, a.hair_color);
			else if (key == "face") ParseUint(p, a.face);
			else if (key == "skin_color") ParseUint(p, a.skin_color);
			else if (key == "height") ParseUint(p, a.height);
			else SkipValue(p);
		}
	}
}

bool CharacterData::ParseFromJson(const std::string& json)
{
	Clear();
	const char* p = json.c_str();
	if (!Expect(p, '{')) return false;

	while (true)
	{
		SkipWhitespace(p);
		if (*p == '}') { valid = true; return true; }
		if (*p == ',') p++;
		if (*p == '\0') break;

		std::string key;
		if (!ParseString(p, key)) break;
		if (!Expect(p, ':')) break;

		if (key == "name") { ParseString(p, name); }
		else if (key == "profession") { ParseUint(p, profession); }
		else if (key == "secondary") { ParseUint(p, secondary); }
		else if (key == "campaign") { ParseUint(p, campaign); }
		else if (key == "sex") { ParseUint(p, sex); }
		else if (key == "level") { ParseUint(p, level); }
		else if (key == "is_npc") { ParseBool(p, is_npc); }
		else if (key == "appearance_bitmap") { ParseUint(p, appearance_bitmap); }
		else if (key == "npc_model_file_id") { ParseUint(p, npc_model_file_id); }
		else if (key == "npc_skin_file_id") { ParseUint(p, npc_skin_file_id); }
		else if (key == "face_file_id") { ParseUint(p, face_file_id); }
		else if (key == "hair_file_id") { ParseUint(p, hair_file_id); }
		else if (key == "appearance") { ParseAppearanceObject(p, appearance); }
		else if (key == "slots") { ParseSlotsObject(p, *this); }
		else if (key == "npc_model_files")
		{
			SkipWhitespace(p);
			if (*p == '[')
			{
				p++;
				while (true)
				{
					SkipWhitespace(p);
					if (*p == ']') { p++; break; }
					if (!npc_model_files.empty()) { if (*p == ',') p++; }
					uint32_t v = 0;
					if (ParseUint(p, v)) npc_model_files.push_back(v);
					else break;
				}
			}
			else SkipValue(p);
		}
		else if (key == "body_slot_assignments") { ParseBodySlotAssignments(p, body_slot_assignments); }
		else if (key == "body_candidates")
		{
			SkipWhitespace(p);
			if (*p == '[')
			{
				p++;
				while (true)
				{
					SkipWhitespace(p);
					if (*p == ']') { p++; break; }
					if (!body_candidates.empty()) { if (*p == ',') p++; }
					uint32_t v = 0;
					if (ParseUint(p, v)) body_candidates.push_back(v);
					else break;
				}
			}
			else SkipValue(p);
		}
		else SkipValue(p);
	}
	return valid;
}
