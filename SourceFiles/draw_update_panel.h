#pragma once

#include "Net/UpdateChecker.h"

void draw_update_notification(UpdateChecker* checker, HWND appWindow);

// Set to true from the Help menu to trigger a manual update check
inline bool g_checkForUpdatesRequested = false;

// Debug: set to a non-Idle value from the Debug menu to simulate that state
inline UpdateChecker::State g_debugSimulateUpdateState = UpdateChecker::State::Idle;

// Debug: set to true to run the full test (copy exe + real restart)
inline bool g_debugFullUpdateTest = false;
