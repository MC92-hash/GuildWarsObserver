#pragma once

#define GWO_VERSION "1.0.2"

#define GWO_CLOUD_ENABLED 0
#define GWO_CLOUD_HOST ""
#define GWO_DEVELOPER 0
#define GWO_R2_ENDPOINT ""
#define GWO_R2_BUCKET ""
#define GWO_R2_ACCESS_KEY ""
#define GWO_R2_SECRET_KEY ""
#define GWO_R2_READ_ACCESS_KEY ""
#define GWO_R2_READ_SECRET_KEY ""

// Local override (gitignored) — if it exists, it redefines the values above
#if __has_include("build_config.local.h")
#undef GWO_CLOUD_ENABLED
#undef GWO_CLOUD_HOST
#undef GWO_DEVELOPER
#undef GWO_R2_ENDPOINT
#undef GWO_R2_BUCKET
#undef GWO_R2_ACCESS_KEY
#undef GWO_R2_SECRET_KEY
#undef GWO_R2_READ_ACCESS_KEY
#undef GWO_R2_READ_SECRET_KEY
#include "build_config.local.h"
#endif
