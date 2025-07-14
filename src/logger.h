#pragma once

#include <stdbool.h>

#include "status.h"

Status logger_init(const char* file_name);
Status logger_shutdown();

void logger_log(const char* fmt, ...);

#ifdef LOG_ENABLED
    #define LOG(FMT, ...) logger_log(FMT, __VA_ARGS__)
#else
    #define LOG(FMT, ...)
#endif
