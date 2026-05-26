#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_NONE
} LogLevel;

typedef struct {
    bool enable_log;
    LogLevel log_level;
    bool log_to_file;
    const char* log_file;
    int log_file_count;
    long log_file_size;
    FILE* log_fp;
} LogConfig;

bool log_manager_init(LogConfig* config);
void log_manager_cleanup(void);
void log_debug(const char* format, ...);
void log_info(const char* format, ...);
void log_warn(const char* format, ...);
void log_error(const char* format, ...);
LogLevel log_get_current_level(void);

#ifdef __cplusplus
}
#endif

#endif
