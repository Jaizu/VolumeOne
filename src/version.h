#pragma once

// Single source of the app version (used by the code and res\app.rc).
// The GitHub release tag must be "v" + this, e.g. v1.1.0.
#define VERSION_MAJOR 1
#define VERSION_MINOR 4
#define VERSION_PATCH 0

#define VERSION_STR2(a, b, c) #a "." #b "." #c
#define VERSION_STR(a, b, c) VERSION_STR2(a, b, c)
#define APP_VERSION_A VERSION_STR(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH)
