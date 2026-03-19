// Compile miniz as a C++ translation unit without precompiled headers.
// This file provides the implementation; other files just #include "miniz.h".
#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.c"
