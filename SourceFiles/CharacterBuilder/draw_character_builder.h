#pragma once

#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

class DATManager;
class MapRenderer;

void draw_character_builder(
	std::map<int, std::unique_ptr<DATManager>>& dat_managers,
	int dat_manager_index,
	MapRenderer* map_renderer,
	std::unordered_map<int, std::vector<int>>& hash_index);
