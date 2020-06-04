#include <stdio.h>

#include <km_common/km_debug.h>
#include <km_common/km_defines.h>
#include <km_common/km_lib.h>

#define LOG_ERROR(format, ...) fprintf(stderr, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...)  fprintf(stdout, format, ##__VA_ARGS__)
#define LOG_FLUSH() fflush(stderr); fflush(stdout)

const int WINDOW_START_WIDTH  = 1600;
const int WINDOW_START_HEIGHT = 900;

const uint64 PERMANENT_MEMORY_SIZE = MEGABYTES(1);
const uint64 TRANSIENT_MEMORY_SIZE = MEGABYTES(32);

#if GAME_WIN32
#include "win32_main.cpp"
#else
#error "Unsupported platform"
#endif

#include <km_common/km_lib.cpp>
#include <km_common/km_memory.cpp>
#include <km_common/km_string.cpp>
