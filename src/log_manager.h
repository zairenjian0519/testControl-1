#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

// 日志级别
typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_NONE
} LogLevel;

// 日志配置
typedef struct {
    bool enable_log;          // 是否启用日志
    LogLevel log_level;       // 日志级别
    bool log_to_file;         // 是否输出到文件
    const char* log_file;     // 日志文件路径
    int log_file_count;       // 日志文件个数
    long log_file_size;       // 日志文件大小限制（字节）
    FILE* log_fp;             // 日志文件指针
} LogConfig;

/**
 * @brief 初始化日志管理器
 * @param config 日志配置
 * @return true表示初始化成功，false表示失败
 */
bool log_manager_init(LogConfig* config);

/**
 * @brief 清理日志管理器
 */
void log_manager_cleanup(void);

/**
 * @brief 打印调试日志
 * @param format 格式化字符串
 * @param ... 可变参数
 */
void log_debug(const char* format, ...);

/**
 * @brief 打印信息日志
 * @param format 格式化字符串
 * @param ... 可变参数
 */
void log_info(const char* format, ...);

/**
 * @brief 打印警告日志
 * @param format 格式化字符串
 * @param ... 可变参数
 */
void log_warn(const char* format, ...);

/**
 * @brief 打印错误日志
 * @param format 格式化字符串
 * @param ... 可变参数
 */
void log_error(const char* format, ...);

/**
 * @brief 获取当前日志级别
 * @return 当前日志级别
 */
LogLevel log_get_current_level(void);

#endif // LOG_MANAGER_H
