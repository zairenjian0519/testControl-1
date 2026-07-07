#include "log_manager.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>

static LogConfig g_log_config = {
    .enable_log = false,
    .log_level = LOG_LEVEL_INFO,
    .log_to_file = false,
    .log_file = NULL,
    .log_file_count = 2,
    .log_file_size = 1024 * 1024, // 默认1MB
    .log_fp = NULL
};

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char* log_level_strings[] = {
    "DEBUG",
    "INFO", 
    "WARN",
    "ERROR"
};

// 日志文件信息结构体
typedef struct {
    char filename[256];
    time_t mtime; // 修改时间
} LogFileInfo;

/**
 * @brief 比较两个日志文件的修改时间，用于排序
 * @param a 第一个日志文件
 * @param b 第二个日志文件
 * @return 比较结果
 */
static int compare_log_files(const void* a, const void* b) {
    const LogFileInfo* file_a = (const LogFileInfo*)a;
    const LogFileInfo* file_b = (const LogFileInfo*)b;
    
    // 按修改时间升序排序，最旧的文件在前面
    if (file_a->mtime < file_b->mtime) {
        return -1;
    } else if (file_a->mtime > file_b->mtime) {
        return 1;
    }
    return 0;
}

/**
 * @brief 清理旧的日志文件，保持日志文件数量不超过配置的log_file_count
 */
static void cleanup_old_log_files(void) {
    if (!g_log_config.log_file) {
        return;
    }
    
    // 提取日志文件目录
    char log_dir[256];
    const char* last_slash = strrchr(g_log_config.log_file, '/');
    if (!last_slash) {
        last_slash = strrchr(g_log_config.log_file, '\\');
    }
    
    if (last_slash) {
        // 有目录部分
        size_t dir_len = last_slash - g_log_config.log_file;
        if (dir_len < sizeof(log_dir)) {
            strncpy(log_dir, g_log_config.log_file, dir_len);
            log_dir[dir_len] = '\0';
        } else {
            return;
        }
    } else {
        // 没有目录部分，使用当前目录
        strcpy(log_dir, ".");
    }
    
    // 提取日志文件名前缀
    char log_prefix[256];
    const char* base_name = last_slash ? last_slash + 1 : g_log_config.log_file;
    strcpy(log_prefix, base_name);
    
    // 打开目录
    DIR* dir = opendir(log_dir);
    if (!dir) {
        return;
    }
    
    // 获取所有日志文件
    LogFileInfo* log_files = NULL;
    int log_file_count = 0;
    struct dirent* entry;
    
    while ((entry = readdir(dir)) != NULL) {
        // 跳过当前目录和上级目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // 检查是否是日志文件（包含日志文件名前缀）
        if (strstr(entry->d_name, log_prefix) != NULL) {
            // 获取文件的完整路径
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", log_dir, entry->d_name);
            
            // 获取文件的修改时间
            struct stat file_stat;
            if (stat(full_path, &file_stat) == 0) {
                // 扩展日志文件列表
                log_files = (LogFileInfo*)realloc(log_files, sizeof(LogFileInfo) * (log_file_count + 1));
                if (log_files) {
                    strcpy(log_files[log_file_count].filename, full_path);
                    log_files[log_file_count].mtime = file_stat.st_mtime;
                    log_file_count++;
                }
            }
        }
    }
    
    closedir(dir);
    
    // 如果日志文件数量超过配置的限制，删除最旧的文件
    if (log_files && log_file_count > g_log_config.log_file_count) {
        // 按修改时间升序排序（最旧的文件在前面）
        qsort(log_files, log_file_count, sizeof(LogFileInfo), compare_log_files);
        
        // 删除超出限制的最旧文件
        int files_to_delete = log_file_count - g_log_config.log_file_count;
        for (int i = 0; i < files_to_delete; i++) {
            if (remove(log_files[i].filename) != 0) {
                fprintf(stderr, "Failed to remove old log file %s: errno=%d (%s)\n",
                        log_files[i].filename, errno, strerror(errno));
            }
        }
    }
    
    // 释放内存
    if (log_files) {
        free(log_files);
    }
}

/**
 * @brief 获取当前时间字符串
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return 时间字符串
 */
static const char* get_current_time(char* buffer, size_t buffer_size)
{
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    if (tm_info) {
        strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", tm_info);
        return buffer;
    }
    return "";
}

/**
 * @brief 生成带有时间戳的日志文件名
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @param base_name 基础文件名
 * @return 带时间戳的文件名
 */
static const char* get_timestamped_filename(char* buffer, size_t buffer_size, const char* base_name)
{
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    if (tm_info) {
        // 从基础文件名中提取目录和文件名部分
        const char* last_slash = strrchr(base_name, '/');
        if (!last_slash) {
            last_slash = strrchr(base_name, '\\');
        }
        
        if (last_slash) {
            // 有目录部分
            size_t dir_len = last_slash - base_name + 1;
            if (dir_len < buffer_size) {
                strncpy(buffer, base_name, dir_len);
                buffer[dir_len] = '\0';
                strftime(buffer + dir_len, buffer_size - dir_len, "%Y%m%d_%H%M%S_", tm_info);
                strncat(buffer, last_slash + 1, buffer_size - strlen(buffer) - 1);
            }
        } else {
            // 没有目录部分
            strftime(buffer, buffer_size, "%Y%m%d_%H%M%S_", tm_info);
            strncat(buffer, base_name, buffer_size - strlen(buffer) - 1);
        }
        return buffer;
    }
    return base_name;
}

/**
 * @brief 检查并进行日志文件轮转
 */
static void check_and_rotate_log(void)
{
    if (!g_log_config.log_to_file || !g_log_config.log_fp) {
        return;
    }
    
    // 检查文件大小
    fseek(g_log_config.log_fp, 0, SEEK_END);
    long current_size = ftell(g_log_config.log_fp);
    
    if (current_size < g_log_config.log_file_size) {
        return; // 文件大小未超过限制，无需轮转
    }
    
    // 关闭当前日志文件
    fclose(g_log_config.log_fp);
    g_log_config.log_fp = NULL;
    
    // 生成带有时间戳的文件名
    char timestamped_file[256];
    get_timestamped_filename(timestamped_file, sizeof(timestamped_file), g_log_config.log_file);
    
    // 重命名当前日志文件
    if (rename(g_log_config.log_file, timestamped_file) != 0) {
        fprintf(stderr, "Failed to rotate log file from %s to %s: errno=%d (%s)\n",
                g_log_config.log_file, timestamped_file, errno, strerror(errno));
    }
    
    // 打开新的日志文件
    g_log_config.log_fp = fopen(g_log_config.log_file, "a");
    if (!g_log_config.log_fp) {
        // 如果文件打开失败，回退到不输出到文件
        g_log_config.log_to_file = false;
        return;
    }
    
    // 清理旧的日志文件，保持日志文件数量不超过配置的log_file_count
    cleanup_old_log_files();
}

/**
 * @brief 打印日志
 * @param level 日志级别
 * @param format 格式化字符串
 * @param args 可变参数
 */
static void log_print(LogLevel level, const char* format, va_list args)
{
    pthread_mutex_lock(&g_log_mutex);

    if (!g_log_config.enable_log || level < g_log_config.log_level) {
        pthread_mutex_unlock(&g_log_mutex);
        return;
    }

    char time_buffer[20];
    const char* time_str = get_current_time(time_buffer, sizeof(time_buffer));

    // 构建日志消息
    char log_buffer[1024];
    int prefix_len = snprintf(log_buffer, sizeof(log_buffer), "[%s] [%s] ", 
                             time_str, log_level_strings[level]);
    
    if (prefix_len < sizeof(log_buffer)) {
        vsnprintf(log_buffer + prefix_len, sizeof(log_buffer) - prefix_len, format, args);
        
        // 输出到标准输出
        printf("%s\n", log_buffer);
        fflush(stdout);
        
        // 输出到文件
        if (g_log_config.log_to_file && g_log_config.log_fp) {
            // 检查是否需要进行日志文件轮转
            check_and_rotate_log();
            
            // 如果轮转后文件指针仍然有效，写入日志
            if (g_log_config.log_to_file && g_log_config.log_fp) {
                fprintf(g_log_config.log_fp, "%s\n", log_buffer);
                fflush(g_log_config.log_fp);
            }
        }
    }

    pthread_mutex_unlock(&g_log_mutex);
}

bool log_manager_init(LogConfig* config)
{
    if (!config) {
        return false;
    }

    pthread_mutex_lock(&g_log_mutex);

    // 复制配置
    memcpy(&g_log_config, config, sizeof(LogConfig));
    
    // 设置默认值（如果未指定）
    if (g_log_config.log_file_count < 1) {
        g_log_config.log_file_count = 2;
    }
    if (g_log_config.log_file_size < 1024) {
        g_log_config.log_file_size = 1024 * 1024; // 默认1MB
    }

    // 如果需要输出到文件，打开日志文件
    if (g_log_config.log_to_file && g_log_config.log_file) {
        g_log_config.log_fp = fopen(g_log_config.log_file, "a");
        if (!g_log_config.log_fp) {
            // 如果文件打开失败，回退到不输出到文件
            g_log_config.log_to_file = false;
            pthread_mutex_unlock(&g_log_mutex);
            return false;
        }
    }

    pthread_mutex_unlock(&g_log_mutex);
    return true;
}

void log_manager_cleanup(void)
{
    pthread_mutex_lock(&g_log_mutex);

    // 关闭日志文件
    if (g_log_config.log_fp) {
        fclose(g_log_config.log_fp);
        g_log_config.log_fp = NULL;
    }

    pthread_mutex_unlock(&g_log_mutex);
}

void log_debug(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    log_print(LOG_LEVEL_DEBUG, format, args);
    va_end(args);
}

void log_info(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    log_print(LOG_LEVEL_INFO, format, args);
    va_end(args);
}

void log_warn(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    log_print(LOG_LEVEL_WARN, format, args);
    va_end(args);
}

void log_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log_print(LOG_LEVEL_ERROR, format, args);
    va_end(args);
}

/**
 * @brief 获取当前日志级别
 * @return 当前日志级别
 */
LogLevel log_get_current_level(void) {
    pthread_mutex_lock(&g_log_mutex);
    LogLevel level = g_log_config.log_level;
    pthread_mutex_unlock(&g_log_mutex);
    return level;
}
