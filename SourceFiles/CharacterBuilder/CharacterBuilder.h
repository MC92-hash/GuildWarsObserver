#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <array>

struct CharacterSlotData
{
	std::string name;
	uint32_t dat_file_id = 0;
	uint32_t composite_id = 0;
	std::array<uint32_t, 11> composite_file_ids = {};
	std::array<uint32_t, 4> dyes = {};
	uint32_t dye_tint = 0;

	bool loaded = false;
	std::vector<int> mesh_ids;
};

struct CharacterData
{
	std::string name;
	uint32_t profession = 0;
	uint32_t secondary = 0;
	uint32_t campaign = 0;
	uint32_t sex = 0;
	uint32_t level = 0;
	bool is_npc = false;
	uint32_t appearance_bitmap = 0;

	struct AppearanceDecode
	{
		uint32_t hair_style = 0;
		uint32_t hair_color = 0;
		uint32_t face = 0;
		uint32_t skin_color = 0;
		uint32_t height = 0;
	} appearance;

	uint32_t npc_model_file_id = 0;
	uint32_t npc_skin_file_id = 0;
	std::vector<uint32_t> npc_model_files;

	// Face and hair DAT file IDs (resolved from composite model table by Model Inspector)
	uint32_t face_file_id = 0;
	uint32_t hair_file_id = 0;

	enum SlotIndex
	{
		Weapon = 0,
		Offhand,
		Chest,
		Legs,
		Head,
		Feet,
		Hands,
		CostumeBody,
		CostumeHead,
		SlotCount
	};

	std::array<CharacterSlotData, SlotCount> slots;

	// Auto-assigned body slot file IDs (one per body slot type: Feet/Legs/Chest/Hands/Head).
	// Populated from "body_slot_assignments" in the JSON export (campaign + appearance match).
	static constexpr int BodySlotCount = 5;
	std::array<uint32_t, BodySlotCount> body_slot_assignments = {};

	// Flat list of body model candidate DAT file hashes from composite model table scan.
	// The user assigns candidates to body slots visually (Chest/Legs/Head/Feet/Hands).
	std::vector<uint32_t> body_candidates;

	bool valid = false;

	void Clear();
	bool ParseFromJson(const std::string& json);
};

const char* CharacterSlotName(int index);
const char* ProfessionName(uint32_t id);
