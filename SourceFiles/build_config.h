#pragma once

#define GWO_CLOUD_ENABLED 1
#define GWO_CLOUD_HOST "matches.gwobserver.com"

// Local override (gitignored) — if it exists, it redefines the values above
#if __has_include("build_config.local.h")
#undef GWO_CLOUD_ENABLED
#undef GWO_CLOUD_HOST
#include "build_config.local.h"
#endif
