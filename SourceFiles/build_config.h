#pragma once

#define GWO_VERSION "1.1.0"

// Local override — included first so it can selectively override defaults.
// Found via gwobserver-private/ (adjacent repo) or a local gitignored copy.
#if __has_include("build_config.local.h")
#include "build_config.local.h"
#endif

// Defaults for anything the local header didn't provide
#ifndef GWO_CLOUD_ENABLED
#define GWO_CLOUD_ENABLED 0
#endif
#ifndef GWO_CLOUD_HOST
#define GWO_CLOUD_HOST ""
#endif
#ifndef GWO_DEVELOPER
#define GWO_DEVELOPER 0
#endif
#ifndef GWO_R2_ENDPOINT
#define GWO_R2_ENDPOINT ""
#endif
#ifndef GWO_R2_BUCKET
#define GWO_R2_BUCKET ""
#endif
#ifndef GWO_R2_ACCESS_KEY
#define GWO_R2_ACCESS_KEY ""
#endif
#ifndef GWO_R2_SECRET_KEY
#define GWO_R2_SECRET_KEY ""
#endif
#ifndef GWO_R2_READ_ACCESS_KEY
#define GWO_R2_READ_ACCESS_KEY ""
#endif
#ifndef GWO_R2_READ_SECRET_KEY
#define GWO_R2_READ_SECRET_KEY ""
#endif
