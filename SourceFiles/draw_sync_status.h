#pragma once

#include <cstdint>
#include <string>

class SyncEngine;

void draw_sync_status(SyncEngine* syncEngine);
void draw_play_download_progress(uint64_t bytesReceived, uint64_t bytesTotal,
                                  bool isError, const std::string& errorMsg);
