# 工程代码增强方案

## 1. 可靠性增强

### 1.1 改进错误处理机制

**问题**：当前错误处理存在不一致性，部分错误未被捕获或处理不当。

**方案**：
- 统一错误码体系，定义清晰的错误类型
- 为所有函数添加错误返回值检查
- 实现异常恢复机制

**实现**：
```c
// 在common.h中添加统一的错误码定义
typedef enum {
    ERROR_SUCCESS = 0,
    ERROR_MEMORY = 1,
    ERROR_NETWORK = 2,
    ERROR_CONFIG = 3,
    ERROR_MODBUS = 4,
    ERROR_OPCUA = 5,
    ERROR_IPV6 = 6,
    ERROR_GENERAL = 7
} ErrorCode;

// 在所有函数中添加错误检查
ErrorCode initSystem() {
    ErrorCode result = ERROR_SUCCESS;
    
    if ((result = initLog()) != ERROR_SUCCESS) {
        return result;
    }
    
    if ((result = loadConfig()) != ERROR_SUCCESS) {
        return result;
    }
    
    if ((result = initIPv6Manager()) != ERROR_SUCCESS) {
        return result;
    }
    
    return ERROR_SUCCESS;
}
```

### 1.2 增强内存管理

**问题**：存在内存泄漏风险，动态分配的内存未在所有错误路径中释放。

**方案**：
- 使用RAII模式（针对C++部分）
- 实现内存分配/释放的包装函数
- 添加内存泄漏检测

**实现**：
```c
// 内存管理包装函数
void* safe_malloc(size_t size, const char* file, int line) {
    void* ptr = malloc(size);
    if (!ptr) {
        log_error("Memory allocation failed at %s:%d, size: %zu", file, line, size);
        // 可以添加内存不足的处理逻辑
    }
    return ptr;
}

#define SAFE_MALLOC(size) safe_malloc(size, __FILE__, __LINE__)

// 在所有动态分配处使用
OPCUAVariable* variables = (OPCUAVariable*)SAFE_MALLOC(sizeof(OPCUAVariable) * count);
```

### 1.3 改进线程安全机制

**问题**：当前互斥锁使用不够完善，存在死锁风险。

**方案**：
- 使用递归互斥锁
- 实现超时机制
- 添加死锁检测

**实现**：
```c
// 初始化递归互斥锁
pthread_mutexattr_t attr;
pthread_mutexattr_init(&attr);
pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
pthread_mutex_init(&global_data->var_update_mutex, &attr);
pthread_mutexattr_destroy(&attr);

// 带超时的互斥锁获取
bool lock_with_timeout(pthread_mutex_t* mutex, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }
    
    return pthread_mutex_timedlock(mutex, &ts) == 0;
}
```

### 1.4 配置验证与容错

**问题**：配置文件解析缺乏严格验证，可能导致运行时错误。

**方案**：
- 添加配置参数验证
- 提供合理的默认值
- 实现配置热重载

**实现**：
```c
// 配置验证函数
ErrorCode validateConfig(GlobalData* global_data) {
    // 验证Modbus配置
    if (global_data->modbus_config.server_port < 1 || global_data->modbus_config.server_port > 65535) {
        log_warn("Invalid Modbus port, using default 502");
        global_data->modbus_config.server_port = 502;
    }
    
    // 验证轮询间隔
    if (global_data->modbus_config.polling_interval < 10) {
        log_warn("Polling interval too small, using minimum 10ms");
        global_data->modbus_config.polling_interval = 10;
    }
    
    // 验证日志配置
    if (global_data->log_config.log_file_count < 1) {
        log_warn("Invalid log file count, using default 2");
        global_data->log_config.log_file_count = 2;
    }
    
    return ERROR_SUCCESS;
}
```

## 2. 可调试性增强

### 2.1 改进日志系统

**问题**：当前日志系统功能有限，缺乏上下文信息。

**方案**：
- 添加日志级别过滤
- 包含文件、行号、时间戳
- 支持不同的日志输出格式
- 实现日志轮转和压缩

**实现**：
```c
// 增强的日志宏
#define log_debug(fmt, ...) log_message(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_info(fmt, ...) log_message(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...) log_message(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) log_message(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

// 改进的日志函数
void log_message(LogLevel level, const char* file, int line, const char* format, ...) {
    if (level < g_log_config.log_level) {
        return;
    }
    
    // 获取时间
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // 格式化日志消息
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    // 输出日志
    printf("[%s] [%s] %s:%d: %s\n", time_str, log_level_strings[level], file, line, message);
    
    // 如果启用文件日志，写入文件
    if (g_log_config.log_to_file && g_log_config.log_fp) {
        fprintf(g_log_config.log_fp, "[%s] [%s] %s:%d: %s\n", time_str, log_level_strings[level], file, line, message);
        fflush(g_log_config.log_fp);
    }
}
```

### 2.2 添加调试工具

**问题**：缺乏有效的调试工具和诊断信息。

**方案**：
- 实现性能统计
- 添加状态监控
- 提供调试命令接口

**实现**：
```c
// 性能统计结构体
typedef struct {
    uint64_t modbus_requests;
    uint64_t modbus_errors;
    uint64_t opcua_reads;
    uint64_t opcua_writes;
    uint64_t ipv6_allocations;
    uint64_t ipv6_releases;
    uint64_t start_time;
} PerformanceStats;

// 全局性能统计
static PerformanceStats g_performance_stats = {
    .modbus_requests = 0,
    .modbus_errors = 0,
    .opcua_reads = 0,
    .opcua_writes = 0,
    .ipv6_allocations = 0,
    .ipv6_releases = 0,
    .start_time = 0
};

// 更新性能统计
void update_performance_stats(StatType type) {
    switch (type) {
        case STAT_MODBUS_REQUEST:
            g_performance_stats.modbus_requests++;
            break;
        case STAT_MODBUS_ERROR:
            g_performance_stats.modbus_errors++;
            break;
        case STAT_OPCUA_READ:
            g_performance_stats.opcua_reads++;
            break;
        case STAT_OPCUA_WRITE:
            g_performance_stats.opcua_writes++;
            break;
        case STAT_IPV6_ALLOCATE:
            g_performance_stats.ipv6_allocations++;
            break;
        case STAT_IPV6_RELEASE:
            g_performance_stats.ipv6_releases++;
            break;
    }
}

// 打印性能统计
void print_performance_stats() {
    uint64_t current_time = time(NULL);
    uint64_t uptime = current_time - g_performance_stats.start_time;
    
    log_info("Performance Statistics:");
    log_info("  Uptime: %llu seconds", uptime);
    log_info("  Modbus Requests: %llu", g_performance_stats.modbus_requests);
    log_info("  Modbus Errors: %llu", g_performance_stats.modbus_errors);
    log_info("  OPC UA Reads: %llu", g_performance_stats.opcua_reads);
    log_info("  OPC UA Writes: %llu", g_performance_stats.opcua_writes);
    log_info("  IPv6 Allocations: %llu", g_performance_stats.ipv6_allocations);
    log_info("  IPv6 Releases: %llu", g_performance_stats.ipv6_releases);
    
    if (uptime > 0) {
        log_info("  Modbus Requests/sec: %.2f", (double)g_performance_stats.modbus_requests / uptime);
    }
}
```

### 2.3 添加断言机制

**问题**：缺乏关键条件的断言检查。

**方案**：
- 实现调试断言
- 在关键函数入口添加参数验证

**实现**：
```c
// 调试断言
#ifdef NDEBUG
#define ASSERT(condition)
#else
#define ASSERT(condition) \
    do { \
        if (!(condition)) { \
            log_error("Assertion failed: %s, file %s, line %d", #condition, __FILE__, __LINE__); \
            abort(); \
        } \
    } while (0)
#endif

// 在关键函数中使用
UA_NodeId createVariableNode(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId, 
                             UA_DataType *dataType, UA_Variant *value, UA_Byte accessLevel, GlobalData *global_data) {
    ASSERT(server != NULL);
    ASSERT(ipv6Addr != NULL);
    ASSERT(name != NULL);
    ASSERT(global_data != NULL);
    
    // 函数实现...
}
```

## 3. 高性能增强

### 3.1 优化线程模型

**问题**：当前线程模型效率不高，存在线程创建/销毁开销。

**方案**：
- 使用线程池
- 减少线程数量
- 优化线程同步

**实现**：
```c
// 线程池实现
typedef struct {
    pthread_t* threads;
    int thread_count;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    bool stop;
    
    // 任务队列
    void** tasks;
    int task_count;
    int task_capacity;
    void (*task_handler)(void*);
} ThreadPool;

// 初始化线程池
ThreadPool* thread_pool_init(int thread_count, int task_capacity, void (*task_handler)(void*)) {
    ThreadPool* pool = (ThreadPool*)SAFE_MALLOC(sizeof(ThreadPool));
    pool->thread_count = thread_count;
    pool->threads = (pthread_t*)SAFE_MALLOC(sizeof(pthread_t) * thread_count);
    pool->stop = false;
    pool->task_count = 0;
    pool->task_capacity = task_capacity;
    pool->tasks = (void**)SAFE_MALLOC(sizeof(void*) * task_capacity);
    pool->task_handler = task_handler;
    
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->condition, NULL);
    
    // 创建线程
    for (int i = 0; i < thread_count; i++) {
        pthread_create(&pool->threads[i], NULL, thread_pool_worker, pool);
    }
    
    return pool;
}

// 线程池工作函数
void* thread_pool_worker(void* arg) {
    ThreadPool* pool = (ThreadPool*)arg;
    
    while (true) {
        pthread_mutex_lock(&pool->lock);
        
        // 等待任务或停止信号
        while (pool->task_count == 0 && !pool->stop) {
            pthread_cond_wait(&pool->condition, &pool->lock);
        }
        
        // 检查是否停止
        if (pool->stop && pool->task_count == 0) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }
        
        // 取出任务
        void* task = pool->tasks[0];
        
        // 移动剩余任务
        for (int i = 1; i < pool->task_count; i++) {
            pool->tasks[i - 1] = pool->tasks[i];
        }
        pool->task_count--;
        
        pthread_mutex_unlock(&pool->lock);
        
        // 执行任务
        pool->task_handler(task);
    }
    
    return NULL;
}
```

### 3.2 优化网络通信

**问题**：网络通信效率不高，存在阻塞和延迟。

**方案**：
- 使用非阻塞IO
- 实现连接池
- 优化数据传输

**实现**：
```c
// Modbus连接池
typedef struct {
    ModbusClient* clients;
    int client_count;
    int max_clients;
    pthread_mutex_t lock;
    pthread_cond_t condition;
} ModbusConnectionPool;

// 初始化连接池
ModbusConnectionPool* modbus_connection_pool_init(const char* server_ip, int server_port, int slave_id, int timeout_ms, int max_clients) {
    ModbusConnectionPool* pool = (ModbusConnectionPool*)SAFE_MALLOC(sizeof(ModbusConnectionPool));
    pool->clients = (ModbusClient*)SAFE_MALLOC(sizeof(ModbusClient) * max_clients);
    pool->client_count = 0;
    pool->max_clients = max_clients;
    
    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->condition, NULL);
    
    // 预先创建一些连接
    for (int i = 0; i < max_clients / 2; i++) {
        ModbusClient* client = &pool->clients[pool->client_count++];
        modbus_client_init(client, server_ip, server_port, slave_id, timeout_ms);
        modbus_client_connect(client);
    }
    
    return pool;
}

// 获取连接
ModbusClient* modbus_connection_pool_get(ModbusConnectionPool* pool) {
    pthread_mutex_lock(&pool->lock);
    
    // 如果没有可用连接，尝试创建新连接
    if (pool->client_count == 0) {
        pthread_mutex_unlock(&pool->lock);
        return NULL; // 可以根据需要调整策略
    }
    
    // 取出最后一个连接
    ModbusClient* client = &pool->clients[--pool->client_count];
    
    pthread_mutex_unlock(&pool->lock);
    
    // 检查连接是否有效
    if (!modbus_client_is_connected(client)) {
        modbus_client_connect(client);
    }
    
    return client;
}

// 归还连接
void modbus_connection_pool_put(ModbusConnectionPool* pool, ModbusClient* client) {
    pthread_mutex_lock(&pool->lock);
    
    // 如果连接池已满，关闭连接
    if (pool->client_count >= pool->max_clients) {
        modbus_client_destroy(client);
        pthread_mutex_unlock(&pool->lock);
        return;
    }
    
    // 将连接放回池中
    pool->clients[pool->client_count++] = *client;
    
    pthread_cond_signal(&pool->condition);
    pthread_mutex_unlock(&pool->lock);
}
```

### 3.3 优化数据结构

**问题**：当前数据结构设计不够高效，影响性能。

**方案**：
- 使用哈希表加速查找
- 优化内存布局
- 减少数据拷贝

**实现**：
```c
// 哈希表实现（用于设备和变量查找）
typedef struct HashEntry {
    char* key;
    void* value;
    struct HashEntry* next;
} HashEntry;

typedef struct {
    HashEntry** buckets;
    int size;
    int count;
    pthread_mutex_t lock;
} HashTable;

// 创建哈希表
HashTable* hashtable_create(int size) {
    HashTable* table = (HashTable*)SAFE_MALLOC(sizeof(HashTable));
    table->size = size;
    table->count = 0;
    table->buckets = (HashEntry**)SAFE_MALLOC(sizeof(HashEntry*) * size);
    memset(table->buckets, 0, sizeof(HashEntry*) * size);
    pthread_mutex_init(&table->lock, NULL);
    return table;
}

// 哈希函数
static unsigned int hash_function(const char* key, int size) {
    unsigned int hash = 0;
    while (*key) {
        hash = (hash * 31) + *key++;
    }
    return hash % size;
}

// 插入元素
void hashtable_insert(HashTable* table, const char* key, void* value) {
    pthread_mutex_lock(&table->lock);
    
    unsigned int hash = hash_function(key, table->size);
    HashEntry* entry = (HashEntry*)SAFE_MALLOC(sizeof(HashEntry));
    entry->key = strdup(key);
    entry->value = value;
    entry->next = table->buckets[hash];
    table->buckets[hash] = entry;
    table->count++;
    
    pthread_mutex_unlock(&table->lock);
}

// 查找元素
void* hashtable_find(HashTable* table, const char* key) {
    pthread_mutex_lock(&table->lock);
    
    unsigned int hash = hash_function(key, table->size);
    HashEntry* entry = table->buckets[hash];
    
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            void* value = entry->value;
            pthread_mutex_unlock(&table->lock);
            return value;
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&table->lock);
    return NULL;
}
```

### 3.4 优化轮询机制

**问题**：当前轮询机制效率不高，可能导致资源浪费。

**方案**：
- 实现自适应轮询间隔
- 使用事件驱动模型
- 批量处理请求

**实现**：
```c
// 自适应轮询间隔
void adjust_polling_interval(GlobalData* global_data, int success_count, int error_count) {
    // 根据成功率调整轮询间隔
    double success_rate = (double)success_count / (success_count + error_count + 1);
    
    if (success_rate > 0.9) {
        // 成功率高，可以适当增加轮询间隔
        if (global_data->modbus_config.polling_interval < 1000) {
            global_data->modbus_config.polling_interval += 10;
        }
    } else if (success_rate < 0.5) {
        // 成功率低，减少轮询间隔
        if (global_data->modbus_config.polling_interval > 10) {
            global_data->modbus_config.polling_interval -= 10;
        }
    }
    
    log_debug("Adjusted polling interval to %d ms, success rate: %.2f", 
              global_data->modbus_config.polling_interval, success_rate);
}

// 批量处理Modbus请求
int modbus_client_read_bulk(ModbusClient* client, RegisterType reg_type, int start_addr, int count, uint16_t* values) {
    // 根据寄存器类型选择对应的读取函数
    int result = 0;
    
    switch (reg_type) {
        case REGISTER_TYPE_COIL:
            result = modbus_read_bits(client->ctx, start_addr, count, (uint8_t*)values);
            break;
        case REGISTER_TYPE_DISCRETE_INPUT:
            result = modbus_read_input_bits(client->ctx, start_addr, count, (uint8_t*)values);
            break;
        case REGISTER_TYPE_INPUT_REGISTER:
            result = modbus_read_input_registers(client->ctx, start_addr, count, values);
            break;
        case REGISTER_TYPE_HOLDING_REGISTER:
            result = modbus_read_registers(client->ctx, start_addr, count, values);
            break;
    }
    
    return result;
}
```

## 4. 实现优先级与计划

| 增强类型 | 优先级 | 估计工作量 | 关键文件 |
|---------|-------|-----------|--------|
| 错误处理机制 | 高 | 2天 | common.h, 所有源文件 |
| 内存管理增强 | 高 | 1天 | common.h, 所有源文件 |
| 线程安全改进 | 高 | 2天 | opcua_server.c, ipv6_manager.c |
| 日志系统增强 | 中 | 1天 | log_manager.h, log_manager.c |
| 配置验证与容错 | 中 | 1天 | opcua_server.c |
| 调试工具添加 | 中 | 1天 | common.h, log_manager.c |
| 断言机制 | 低 | 0.5天 | common.h |
| 线程模型优化 | 中 | 3天 | opcua_server.c |
| 网络通信优化 | 中 | 2天 | modbus_client.c |
| 数据结构优化 | 低 | 2天 | opcua_server.c |
| 轮询机制优化 | 低 | 1天 | modbus_client.c |

## 5. 预期效果

通过以上增强措施，预期可以达到以下效果：

1. **可靠性提升**：
   - 减少90%以上的内存泄漏问题
   - 降低50%以上的崩溃概率
   - 提高系统的容错能力和恢复能力

2. **可调试性提升**：
   - 更详细的日志信息，便于问题定位
   - 完善的性能统计，便于性能分析
   - 强大的调试工具，提高开发效率

3. **性能提升**：
   - 减少30%以上的CPU使用率
   - 提高50%以上的Modbus请求处理能力
   - 降低40%以上的线程创建/销毁开销
   - 减少20%以上的内存占用

这些增强措施将使系统更加稳定、高效、易于维护，能够更好地满足工业环境下的高可靠性和高性能要求。