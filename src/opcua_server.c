// server.c
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdbool.h>
#include <errno.h>

#include "open62541.h"
#include "csv_parser.h"
#include "modbus_client.h"
#include "cJSON.h"
#include "ipv6_manager.h"
#include "log_manager.h"

// 设备配置结构体
typedef struct {
    char server_ip[16];
    int server_port;
    int polling_interval;
    int reconnect_interval;
} ModbusConfig;

// IPv6组播配置
typedef struct {
    int listen_port;
    char multicast_ip[20];
    int nic_index;
} IPv6MulticastConfig;

// IPv6地址池配置
typedef struct {
    char start_address[40];
    char end_address[40];
    int prefix_length;
    int max_lease_count;
    bool skip_as_source;
    unsigned int batch_add_limit;
    unsigned int batch_add_delay_ms;
} IPv6AddressPool;

typedef struct {
    char name[50];
    char model[50];
    int opcua_port;
    char opcua_path[64];
} DeviceConfig;

// OPC UA变量结构体
typedef struct {
    UA_NodeId node_id;
    int modbus_addr;
    RegisterType register_type;
    PLCDatatype plc_datatype;
    char name[100];
    char ipv6_address[40];  // 存储分配的IPv6地址
    void *value;
} OPCUAVariable;

// 设备节点结构体
typedef struct {
    UA_NodeId node_id;
    char name[50];
    char ipv6_address[40];  // 存储分配的IPv6地址
    OPCUAVariable *variables;
    int variable_count;
} DeviceNode;

typedef struct {
    RegisterType register_type;
    int start_addr;
    int count;
} ModbusReadBlock;

typedef struct {
    ModbusReadBlock *blocks;
    int block_count;
    int block_capacity;
} ModbusReadPlan;

// 全局数据
typedef struct {
    ModbusConfig modbus_config;
    DeviceConfig device_config;
    IPv6MulticastConfig ipv6_multicast;
    IPv6AddressPool ipv6_pool;
    CSVParseResult csv_result;
    DeviceNode *devices;
    int device_count;
    ModbusClient modbus_client;
    ModbusReadPlan modbus_read_plan;
    UA_Server *server;
    UA_NodeId nodesNodeId;  // 用于动态添加设备节点
    bool is_modbus_updating;  // 标志位，用于区分Modbus更新和外部OPC UA客户端更新
    pthread_mutex_t var_update_mutex;  // 保护变量更新的互斥锁（支持递归）
} GlobalData;

static bool ensurePersistentIPv6OnInterface(GlobalData *global_data, const char *owner_name, const char *ipv6_address) {
    if (!global_data || !owner_name || !ipv6_address || ipv6_address[0] == '\0') {
        return false;
    }

    if (ipv6_add_address_to_interface(
            global_data->ipv6_multicast.nic_index,
            ipv6_address,
            global_data->ipv6_pool.prefix_length)) {
        log_debug("Added persistent IPv6 address %s for %s to interface %d",
                 ipv6_address, owner_name, global_data->ipv6_multicast.nic_index);
        return true;
    }

    log_debug("Persistent IPv6 address %s for %s may already exist on interface %d",
             ipv6_address, owner_name, global_data->ipv6_multicast.nic_index);
    return true;
}

static bool allocatePersistentIPv6(GlobalData *global_data, const char *owner_name, char *ipv6_address, size_t ipv6_address_size) {
    if (!global_data || !owner_name || !ipv6_address || ipv6_address_size == 0) {
        return false;
    }

    ipv6_address[0] = '\0';
    if (!ipv6_allocate_address(owner_name, global_data->ipv6_multicast.nic_index, ipv6_address)) {
        log_error("Failed to allocate IPv6 address for %s", owner_name);
        return false;
    }

    ipv6_address[ipv6_address_size - 1] = '\0';
    return true;
}

static void removePersistentIPv6FromInterface(GlobalData *global_data, const char *owner_name, const char *ipv6_address) {
    if (!global_data || !owner_name || !ipv6_address || ipv6_address[0] == '\0') {
        return;
    }

    if (ipv6_remove_address_from_interface(global_data->ipv6_multicast.nic_index, ipv6_address)) {
        log_debug("Removed persistent IPv6 address %s for %s from interface %d",
                 ipv6_address, owner_name, global_data->ipv6_multicast.nic_index);
    } else {
        log_warn("Failed to remove persistent IPv6 address %s for %s from interface %d",
                 ipv6_address, owner_name, global_data->ipv6_multicast.nic_index);
    }
}

// 函数声明
UA_Guid ipv6ToGuid(const char *ipv6Addr);
UA_NodeId createNodeWithIPv6Id(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId);
UA_NodeId createObjectNode(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId);

// 定时打印节点变量信息的线程函数
void* printNodeVariablesThread(void* arg);

// 读写回调函数声明
static UA_StatusCode readVariableCallback(UA_Server *server, const UA_NodeId *sessionId, void *sessionContext,
                                         const UA_NodeId *nodeId, void *nodeContext, UA_Boolean sourceTimeStamp,
                                         const UA_NumericRange *range, UA_DataValue *dataValue);

static UA_StatusCode writeVariableCallback(UA_Server *server, const UA_NodeId *sessionId, void *sessionContext,
                                         const UA_NodeId *nodeId, void *nodeContext, const UA_NumericRange *range,
                                         const UA_DataValue *dataValue);

UA_NodeId createVariableNode(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId, 
                             UA_DataType *dataType, UA_Variant *value, UA_Byte accessLevel, GlobalData *global_data);

// 新函数声明
int loadConfig(GlobalData *global_data);
int parseIPv6Address(const char *addr, uint16_t *segments);
char *generateIPv6Address(uint16_t *segments, int index);
UA_DataType *getOPCUAType(PLCDatatype plc_type);
UA_Byte getAccessLevel(RegisterType reg_type);
int createDeviceNodes(UA_Server *server, GlobalData *global_data, UA_NodeId nodesNodeId);
void updateOPCUAVariables(UA_Server *server, GlobalData *global_data);
void *modbusPollingThread(void *arg);
UA_Variant *createDefaultValue(PLCDatatype plc_type);
int isDeviceStatusVariable(CSVRecord *record);
int checkDeviceStatus(GlobalData *global_data, const char *device_name);
int deleteDeviceNodes(UA_Server *server, GlobalData *global_data, int device_index);
int addDeviceNodes(UA_Server *server, GlobalData *global_data, int device_index, UA_NodeId nodesNodeId);
static UA_StatusCode writeCachedValueToNode(UA_Server *server, OPCUAVariable *var);

#define MODBUS_PDU_READ_BASE_ADDR 0
#define MODBUS_MAX_READ_REGISTERS 125
#define MODBUS_MAX_READ_BITS 2000

typedef struct {
    RegisterType register_type;
    int start_addr;
    int end_addr;
} ModbusReadRange;

static const char *register_type_to_string(RegisterType type)
{
    switch (type) {
        case REGISTER_TYPE_DISCRETE_INPUT:
            return "Discrete Inputs";
        case REGISTER_TYPE_COIL:
            return "Coils";
        case REGISTER_TYPE_INPUT_REGISTER:
            return "Input Registers";
        case REGISTER_TYPE_HOLDING_REGISTER:
            return "Holding Registers";
        default:
            return "Unknown";
    }
}

static int modbus_reference_to_pdu_addr(RegisterType type, int ref_addr)
{
    switch (type) {
        case REGISTER_TYPE_COIL:
            if (ref_addr >= 200001) {
                return ref_addr - 200001;
            }
            if (ref_addr >= 20001) {
                return ref_addr - 20001;
            }
            return ref_addr - 1;

        case REGISTER_TYPE_DISCRETE_INPUT:
            if (ref_addr >= 100001) {
                return ref_addr - 100001;
            }
            if (ref_addr >= 10001) {
                return ref_addr - 10001;
            }
            return ref_addr - 1;

        case REGISTER_TYPE_INPUT_REGISTER:
            if (ref_addr >= 300001) {
                return ref_addr - 300001;
            }
            if (ref_addr >= 30001) {
                return ref_addr - 30001;
            }
            return ref_addr - 1;

        case REGISTER_TYPE_HOLDING_REGISTER:
            if (ref_addr >= 400001) {
                return ref_addr - 400001;
            }
            if (ref_addr >= 40001) {
                return ref_addr - 40001;
            }
            return ref_addr - 1;

        default:
            return -1;
    }
}

static int modbus_read_max_count(RegisterType type)
{
    switch (type) {
        case REGISTER_TYPE_INPUT_REGISTER:
        case REGISTER_TYPE_HOLDING_REGISTER:
            return MODBUS_MAX_READ_REGISTERS;
        case REGISTER_TYPE_DISCRETE_INPUT:
        case REGISTER_TYPE_COIL:
            return MODBUS_MAX_READ_BITS;
        default:
            return 0;
    }
}

static int modbus_datatype_point_width(RegisterType type, PLCDatatype plc_type)
{
    if (type == REGISTER_TYPE_DISCRETE_INPUT || type == REGISTER_TYPE_COIL) {
        return 1;
    }

    switch (plc_type) {
        case PLC_TYPE_REAL:
        case PLC_TYPE_DINT:
            return 2;
        case PLC_TYPE_ULINT:
            return 4;
        case PLC_TYPE_BOOL:
        case PLC_TYPE_USINT:
        case PLC_TYPE_UINT:
        case PLC_TYPE_INT:
            return 1;
        default:
            return 1;
    }
}

static int compare_modbus_read_ranges(const void *lhs, const void *rhs)
{
    const ModbusReadRange *a = (const ModbusReadRange *)lhs;
    const ModbusReadRange *b = (const ModbusReadRange *)rhs;

    if (a->register_type != b->register_type) {
        return (int)a->register_type - (int)b->register_type;
    }
    if (a->start_addr != b->start_addr) {
        return a->start_addr - b->start_addr;
    }
    return a->end_addr - b->end_addr;
}

static bool append_modbus_read_block(ModbusReadPlan *plan, RegisterType type, int start_addr, int count)
{
    if (!plan || count <= 0) {
        return false;
    }

    if (plan->block_count >= plan->block_capacity) {
        int new_capacity = plan->block_capacity == 0 ? 16 : plan->block_capacity * 2;
        ModbusReadBlock *new_blocks = (ModbusReadBlock *)realloc(
            plan->blocks, sizeof(ModbusReadBlock) * new_capacity);
        if (!new_blocks) {
            return false;
        }
        plan->blocks = new_blocks;
        plan->block_capacity = new_capacity;
    }

    ModbusReadBlock *block = &plan->blocks[plan->block_count++];
    block->register_type = type;
    block->start_addr = start_addr;
    block->count = count;
    return true;
}

static bool append_modbus_read_range(ModbusReadRange **ranges,
                                     int *range_count,
                                     int *range_capacity,
                                     RegisterType type,
                                     int start_addr,
                                     int width)
{
    if (!ranges || !range_count || !range_capacity || width <= 0) {
        return false;
    }

    if (*range_count >= *range_capacity) {
        int new_capacity = *range_capacity == 0 ? 64 : (*range_capacity * 2);
        ModbusReadRange *new_ranges = (ModbusReadRange *)realloc(
            *ranges, sizeof(ModbusReadRange) * new_capacity);
        if (!new_ranges) {
            return false;
        }
        *ranges = new_ranges;
        *range_capacity = new_capacity;
    }

    ModbusReadRange *range = &(*ranges)[(*range_count)++];
    range->register_type = type;
    range->start_addr = start_addr;
    range->end_addr = start_addr + width - 1;
    return true;
}

static bool flush_modbus_read_range(ModbusReadPlan *plan, RegisterType type, int start_addr, int end_addr)
{
    int max_count = modbus_read_max_count(type);
    if (!plan || max_count <= 0 || start_addr < 0 || end_addr < start_addr) {
        return false;
    }

    int next_start = start_addr;
    while (next_start <= end_addr) {
        int remaining = end_addr - next_start + 1;
        int count = remaining > max_count ? max_count : remaining;
        if (!append_modbus_read_block(plan, type, next_start, count)) {
            return false;
        }
        next_start += count;
    }

    return true;
}

static void free_modbus_read_plan(ModbusReadPlan *plan)
{
    if (!plan) {
        return;
    }

    free(plan->blocks);
    plan->blocks = NULL;
    plan->block_count = 0;
    plan->block_capacity = 0;
}

static bool build_modbus_read_plan(GlobalData *global_data)
{
    if (!global_data) {
        return false;
    }

    free_modbus_read_plan(&global_data->modbus_read_plan);

    ModbusReadRange *ranges = NULL;
    int range_count = 0;
    int range_capacity = 0;

    for (int i = 0; i < global_data->device_count; i++) {
        DeviceNode *device = &global_data->devices[i];
        for (int j = 0; j < device->variable_count; j++) {
            OPCUAVariable *var = &device->variables[j];
            int max_count = modbus_read_max_count(var->register_type);
            if (max_count <= 0 || !var->value) {
                continue;
            }

            int pdu_addr = modbus_reference_to_pdu_addr(var->register_type, var->modbus_addr);
            if (pdu_addr < MODBUS_PDU_READ_BASE_ADDR) {
                log_warn("Skipping invalid Modbus read address: variable=%s modbus_addr=%d register_type=%s pdu_addr=%d",
                         var->name, var->modbus_addr, register_type_to_string(var->register_type), pdu_addr);
                continue;
            }

            int width = modbus_datatype_point_width(var->register_type, var->plc_datatype);
            if (!append_modbus_read_range(&ranges, &range_count, &range_capacity,
                                          var->register_type, pdu_addr, width)) {
                free(ranges);
                return false;
            }
        }
    }

    if (range_count == 0) {
        log_warn("Modbus read plan is empty; no readable variables were found in uploadtable.csv");
        return true;
    }

    qsort(ranges, (size_t)range_count, sizeof(ModbusReadRange), compare_modbus_read_ranges);

    RegisterType current_type = ranges[0].register_type;
    int current_start = ranges[0].start_addr;
    int current_end = ranges[0].end_addr;

    for (int i = 1; i < range_count; i++) {
        ModbusReadRange *range = &ranges[i];
        int max_count = modbus_read_max_count(current_type);
        bool same_type = range->register_type == current_type;
        bool fits_in_block = same_type && (range->end_addr - current_start + 1 <= max_count);

        if (fits_in_block) {
            if (range->end_addr > current_end) {
                current_end = range->end_addr;
            }
            continue;
        }

        if (!flush_modbus_read_range(&global_data->modbus_read_plan,
                                     current_type, current_start, current_end)) {
            free(ranges);
            free_modbus_read_plan(&global_data->modbus_read_plan);
            return false;
        }

        current_type = range->register_type;
        current_start = range->start_addr;
        current_end = range->end_addr;
    }

    if (!flush_modbus_read_range(&global_data->modbus_read_plan,
                                 current_type, current_start, current_end)) {
        free(ranges);
        free_modbus_read_plan(&global_data->modbus_read_plan);
        return false;
    }

    log_info("Built Modbus read plan with %d block(s) from %d variable range(s)",
             global_data->modbus_read_plan.block_count, range_count);
    for (int i = 0; i < global_data->modbus_read_plan.block_count; i++) {
        ModbusReadBlock *block = &global_data->modbus_read_plan.blocks[i];
        log_debug("Modbus read block %d: type=%s start=%d count=%d end=%d",
                  i + 1,
                  register_type_to_string(block->register_type),
                  block->start_addr,
                  block->count,
                  block->start_addr + block->count - 1);
    }

    free(ranges);
    return true;
}

static void update_variable_from_registers(OPCUAVariable *var,
                                           const uint16_t *registers,
                                           int index,
                                           int available)
{
    if (!var || !var->value || !registers || index < 0 || index >= available) {
        return;
    }

    switch (var->plc_datatype) {
        case PLC_TYPE_BOOL:
            *((UA_Boolean *)var->value) = registers[index] != 0;
            break;
        case PLC_TYPE_USINT:
            *((UA_Byte *)var->value) = registers[index] & 0xFF;
            break;
        case PLC_TYPE_UINT:
            *((UA_UInt16 *)var->value) = registers[index];
            break;
        case PLC_TYPE_ULINT:
            if (index + 3 < available) {
                *((UA_UInt64 *)var->value) = ((UA_UInt64)registers[index + 3] << 48) |
                                             ((UA_UInt64)registers[index + 2] << 32) |
                                             ((UA_UInt64)registers[index + 1] << 16) |
                                             (UA_UInt64)registers[index];
            }
            break;
        case PLC_TYPE_INT:
            *((UA_Int16 *)var->value) = (UA_Int16)registers[index];
            break;
        case PLC_TYPE_DINT:
            if (index + 1 < available) {
                uint32_t raw = ((uint32_t)registers[index + 1] << 16) | registers[index];
                *((UA_Int32 *)var->value) = (UA_Int32)raw;
            }
            break;
        case PLC_TYPE_REAL:
            if (index + 1 < available) {
                uint32_t raw = ((uint32_t)registers[index + 1] << 16) | registers[index];
                float float_value = 0.0f;
                memcpy(&float_value, &raw, sizeof(float_value));
                *((UA_Float *)var->value) = float_value;
            }
            break;
        default:
            break;
    }
}

static void update_variables_from_register_block(GlobalData *global_data,
                                                 const ModbusReadBlock *block,
                                                 const uint16_t *registers)
{
    if (!global_data || !block || !registers) {
        return;
    }

    int block_end = block->start_addr + block->count - 1;
    for (int i = 0; i < global_data->device_count; i++) {
        DeviceNode *device = &global_data->devices[i];
        for (int j = 0; j < device->variable_count; j++) {
            OPCUAVariable *var = &device->variables[j];
            if (var->register_type != block->register_type || !var->value) {
                continue;
            }

            int pdu_addr = modbus_reference_to_pdu_addr(var->register_type, var->modbus_addr);
            int width = modbus_datatype_point_width(var->register_type, var->plc_datatype);
            if (pdu_addr < block->start_addr || pdu_addr + width - 1 > block_end) {
                continue;
            }

            update_variable_from_registers(var, registers, pdu_addr - block->start_addr, block->count);
        }
    }
}

static void update_variables_from_bit_block(GlobalData *global_data,
                                            const ModbusReadBlock *block,
                                            const uint8_t *bits)
{
    if (!global_data || !block || !bits) {
        return;
    }

    int block_end = block->start_addr + block->count - 1;
    for (int i = 0; i < global_data->device_count; i++) {
        DeviceNode *device = &global_data->devices[i];
        for (int j = 0; j < device->variable_count; j++) {
            OPCUAVariable *var = &device->variables[j];
            if (var->register_type != block->register_type || !var->value) {
                continue;
            }

            int pdu_addr = modbus_reference_to_pdu_addr(var->register_type, var->modbus_addr);
            if (pdu_addr < block->start_addr || pdu_addr > block_end) {
                continue;
            }

            *((UA_Boolean *)var->value) = bits[pdu_addr - block->start_addr] != 0;
        }
    }
}

static bool read_modbus_block(ModbusClient *client, const ModbusReadBlock *block, GlobalData *global_data)
{
    if (!client || !block || !global_data) {
        return false;
    }

    uint16_t registers[MODBUS_MAX_READ_REGISTERS];
    uint8_t bits[MODBUS_MAX_READ_BITS];
    int result = MODBUS_ERROR_READ;

    switch (block->register_type) {
        case REGISTER_TYPE_INPUT_REGISTER:
            result = modbus_client_read_input_registers(client, block->start_addr, block->count, registers);
            if (result == MODBUS_SUCCESS) {
                update_variables_from_register_block(global_data, block, registers);
            }
            break;
        case REGISTER_TYPE_HOLDING_REGISTER:
            result = modbus_client_read_holding_registers(client, block->start_addr, block->count, registers);
            if (result == MODBUS_SUCCESS) {
                update_variables_from_register_block(global_data, block, registers);
            }
            break;
        case REGISTER_TYPE_DISCRETE_INPUT:
            result = modbus_client_read_discrete_inputs(client, block->start_addr, block->count, bits);
            if (result == MODBUS_SUCCESS) {
                update_variables_from_bit_block(global_data, block, bits);
            }
            break;
        case REGISTER_TYPE_COIL:
            result = modbus_client_read_coils(client, block->start_addr, block->count, bits);
            if (result == MODBUS_SUCCESS) {
                update_variables_from_bit_block(global_data, block, bits);
            }
            break;
        default:
            return true;
    }

    if (result != MODBUS_SUCCESS) {
        log_error("Failed to read Modbus block: type=%s start=%d count=%d end=%d",
                  register_type_to_string(block->register_type),
                  block->start_addr,
                  block->count,
                  block->start_addr + block->count - 1);
        return false;
    }

    return true;
}

UA_Boolean running = true;
extern void request_program_stop(void);

void stopHandler(int sign) {
    (void)sign;
    running = false;
    request_program_stop();
}

#ifdef __cplusplus
extern "C"
#endif
void opcua_server_stop(void) {
    running = false;
    request_program_stop();
}

// IPv6地址转GUID函数
static bool ipv6ToGuidChecked(const char *ipv6Addr, UA_Guid *guid) {
    if (!ipv6Addr || !guid) {
        return false;
    }

    memset(guid, 0, sizeof(UA_Guid));

    unsigned int a, b, c, d, e, f, g, h;
    if (sscanf(ipv6Addr, "%x:%x:%x:%x:%x:%x:%x:%x", &a, &b, &c, &d, &e, &f, &g, &h) != 8) {
        return false;
    }

    if (a > 0xFFFF || b > 0xFFFF || c > 0xFFFF || d > 0xFFFF ||
        e > 0xFFFF || f > 0xFFFF || g > 0xFFFF || h > 0xFFFF) {
        return false;
    }

    guid->data1 = (a << 16) | b;
    guid->data2 = (UA_UInt16)c;
    guid->data3 = (UA_UInt16)d;
    guid->data4[0] = (UA_Byte)((e >> 8) & 0xFF);
    guid->data4[1] = (UA_Byte)(e & 0xFF);
    guid->data4[2] = (UA_Byte)((f >> 8) & 0xFF);
    guid->data4[3] = (UA_Byte)(f & 0xFF);
    guid->data4[4] = (UA_Byte)((g >> 8) & 0xFF);
    guid->data4[5] = (UA_Byte)(g & 0xFF);
    guid->data4[6] = (UA_Byte)((h >> 8) & 0xFF);
    guid->data4[7] = (UA_Byte)(h & 0xFF);

    return true;
}

UA_Guid ipv6ToGuid(const char *ipv6Addr) {
    UA_Guid guid;
    memset(&guid, 0, sizeof(UA_Guid));
    
    // 解析IPv6地址字符串
    unsigned int a, b, c, d, e, f, g, h;
    if (sscanf(ipv6Addr, "%x:%x:%x:%x:%x:%x:%x:%x", &a, &b, &c, &d, &e, &f, &g, &h) == 8) {
        // 将IPv6地址的8个16位部分转换为GUID的各个字段
        guid.data1 = (a << 16) | b;
        guid.data2 = c;
        guid.data3 = d;
        guid.data4[0] = (e >> 8) & 0xFF;
        guid.data4[1] = e & 0xFF;
        guid.data4[2] = (f >> 8) & 0xFF;
        guid.data4[3] = f & 0xFF;
        guid.data4[4] = (g >> 8) & 0xFF;
        guid.data4[5] = g & 0xFF;
        guid.data4[6] = (h >> 8) & 0xFF;
        guid.data4[7] = h & 0xFF;
    }
    
    return guid;
}

// 创建带有IPv6地址作为GUID的节点
UA_NodeId createNodeWithIPv6Id(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId) {
    UA_Guid guid;
    if (!ipv6ToGuidChecked(ipv6Addr, &guid)) {
        log_error("Cannot create object node %s with invalid IPv6 address: %s",
                  name ? name : "(null)", ipv6Addr ? ipv6Addr : "(null)");
        return UA_NODEID_NULL;
    }
    UA_NodeId nodeId = UA_NODEID_GUID(1, guid);
    
    UA_ObjectAttributes objAttr = UA_ObjectAttributes_default;
    objAttr.displayName = UA_LOCALIZEDTEXT((char*)"en-US", (char*)name);
    
    UA_QualifiedName browseName = UA_QUALIFIEDNAME(1, (char*)name);
    UA_NodeId referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    
    UA_NodeId outNodeId;
    UA_Server_addObjectNode(server, nodeId, parentNodeId, referenceTypeId, browseName, 
                           UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), objAttr, NULL, &outNodeId);
    
    return outNodeId;
}

// 创建对象节点
UA_NodeId createObjectNode(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId) {
    return createNodeWithIPv6Id(server, ipv6Addr, name, parentNodeId);
}

// 创建变量节点
UA_NodeId createVariableNode(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId, 
                             UA_DataType *dataType, UA_Variant *value, UA_Byte accessLevel, GlobalData *global_data) {
    UA_Guid guid;
    if (!ipv6ToGuidChecked(ipv6Addr, &guid)) {
        log_error("Cannot create variable node %s with invalid IPv6 address: %s",
                  name ? name : "(null)", ipv6Addr ? ipv6Addr : "(null)");
        return UA_NODEID_NULL;
    }
    UA_NodeId nodeId = UA_NODEID_GUID(1, guid);
    
    UA_VariableAttributes varAttr = UA_VariableAttributes_default;
    varAttr.displayName = UA_LOCALIZEDTEXT((char*)"en-US", (char*)name);
    
    // 确保数据类型不会为NULL
    if (dataType != NULL) {
        varAttr.dataType = dataType->typeId;
    } else {
        // 如果数据类型为NULL，使用默认的UInt16类型
        varAttr.dataType = UA_TYPES[UA_TYPES_UINT16].typeId;
        log_warn("Warning: Data type is NULL for variable %s, using default UInt16 type", name);
    }
    
    varAttr.accessLevel = accessLevel;
    varAttr.userAccessLevel = accessLevel;
    varAttr.value = *value;
    
    UA_QualifiedName browseName = UA_QUALIFIEDNAME(1, (char*)name);
    UA_NodeId referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    
    UA_NodeId outNodeId;
    
    // 添加变量节点
    UA_Server_addVariableNode(server, nodeId, parentNodeId, referenceTypeId, browseName, 
                             UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), varAttr, NULL, &outNodeId);
    
    // 如果变量是可写的且提供了global_data，则添加读写回调函数
    if ((accessLevel & UA_ACCESSLEVELMASK_WRITE) && global_data) {

		#if 1
		// 设置节点上下文，传递GlobalData指针
        UA_Server_setNodeContext(server, outNodeId, global_data);
        
        // 使用UA_DataSource设置读写回调函数
        UA_DataSource dataSource;
        dataSource.read = readVariableCallback;
        dataSource.write = writeVariableCallback;
        
        // 设置变量的DataSource
        UA_Server_setVariableNode_dataSource(server, outNodeId, dataSource);
		#endif
    }

    return outNodeId;
}

// 创建spssps设备节点及其子节点
UA_NodeId createSpsspsDevice(UA_Server *server, GlobalData *global_data, UA_NodeId *busesNodeIdOut) {
    if (!server || !global_data || !busesNodeIdOut) {
        return UA_NODEID_NULL;
    }

    *busesNodeIdOut = UA_NODEID_NULL;
    char owner_name[128];
    char spssps_ipv6[40];
    char device_info_ipv6[40];
    char buses_ipv6[40];

    snprintf(owner_name, sizeof(owner_name), "%s", global_data->device_config.name);
    if (!ipv6_get_device_address(owner_name, spssps_ipv6, sizeof(spssps_ipv6)) &&
        !allocatePersistentIPv6(global_data, owner_name, spssps_ipv6, sizeof(spssps_ipv6))) {
        return UA_NODEID_NULL;
    }
    // 创建spssps设备节点
    UA_NodeId spsspsNodeId = createObjectNode(server, spssps_ipv6, global_data->device_config.name,
                                             UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER));
    
    // 创建DeviceInfo属性
    UA_Variant deviceInfoValue;
    UA_Variant_init(&deviceInfoValue);
    UA_String deviceInfoString = UA_STRING((char*)"Device Information");
    UA_Variant_setScalar(&deviceInfoValue, &deviceInfoString, &UA_TYPES[UA_TYPES_STRING]);
    snprintf(owner_name, sizeof(owner_name), "opcua.%s.DeviceInfo", global_data->device_config.name);
    if (!ipv6_get_device_address(owner_name, device_info_ipv6, sizeof(device_info_ipv6)) &&
        !allocatePersistentIPv6(global_data, owner_name, device_info_ipv6, sizeof(device_info_ipv6))) {
        return spsspsNodeId;
    }
    createVariableNode(server, device_info_ipv6, (char*)"DeviceInfo", spsspsNodeId,
                      &UA_TYPES[UA_TYPES_STRING], &deviceInfoValue, UA_ACCESSLEVELMASK_READ, NULL);
    
    // 创建Buses对象
    snprintf(owner_name, sizeof(owner_name), "opcua.%s.Buses", global_data->device_config.name);
    if (!ipv6_get_device_address(owner_name, buses_ipv6, sizeof(buses_ipv6)) &&
        !allocatePersistentIPv6(global_data, owner_name, buses_ipv6, sizeof(buses_ipv6))) {
        return spsspsNodeId;
    }
    *busesNodeIdOut = createObjectNode(server, buses_ipv6, (char*)"Buses", spsspsNodeId);
    
    return spsspsNodeId;
}

// 创建Bus 0对象及其子节点
UA_NodeId createBus0(UA_Server *server, GlobalData *global_data, UA_NodeId busesNodeId, UA_NodeId *nodesNodeIdOut) {
    if (!server || !global_data || !nodesNodeIdOut) {
        return UA_NODEID_NULL;
    }

    *nodesNodeIdOut = UA_NODEID_NULL;
    char owner_name[128];
    char bus0_ipv6[40];
    char bus_status_ipv6[40];
    char nodes_ipv6[40];

    // 创建Bus 0对象
    snprintf(owner_name, sizeof(owner_name), "opcua.%s.Bus0", global_data->device_config.name);
    if (!allocatePersistentIPv6(global_data, owner_name, bus0_ipv6, sizeof(bus0_ipv6))) {
        return UA_NODEID_NULL;
    }
    UA_NodeId bus0NodeId = createObjectNode(server, bus0_ipv6, (char*)"Bus 0", busesNodeId);
    
    // 创建BusStatus变量
    UA_Variant busStatusValue;
    UA_Variant_init(&busStatusValue);
    UA_Int32 status = 0;
    UA_Variant_setScalar(&busStatusValue, &status, &UA_TYPES[UA_TYPES_INT32]);
    snprintf(owner_name, sizeof(owner_name), "opcua.%s.Bus0.BusStatus", global_data->device_config.name);
    if (allocatePersistentIPv6(global_data, owner_name, bus_status_ipv6, sizeof(bus_status_ipv6))) {
        createVariableNode(server, bus_status_ipv6, (char*)"BusStatus", bus0NodeId,
                          &UA_TYPES[UA_TYPES_INT32], &busStatusValue, UA_ACCESSLEVELMASK_READ, NULL);
    }
    
    // 创建Nodes对象
    snprintf(owner_name, sizeof(owner_name), "opcua.%s.Bus0.Nodes", global_data->device_config.name);
    if (!allocatePersistentIPv6(global_data, owner_name, nodes_ipv6, sizeof(nodes_ipv6))) {
        return bus0NodeId;
    }
    *nodesNodeIdOut = createObjectNode(server, nodes_ipv6, (char*)"Nodes", bus0NodeId);
    
    return bus0NodeId;
}

// 控制1变量的全局存储
static double control1Value = 0.0;

// 读取控制1变量的函数
static UA_StatusCode readControl1(UA_Server *server, 
                 const UA_NodeId *sessionId, void *sessionContext, 
                 const UA_NodeId *nodeId, void *nodeContext, 
                 UA_Boolean sourceTimeStamp, const UA_NumericRange *range, 
                 UA_DataValue *dataValue) 
{
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "read control1 value");
    UA_Variant_setScalarCopy(&dataValue->value, &control1Value, 
                             &UA_TYPES[UA_TYPES_DOUBLE]);
    dataValue->hasValue = true;
    return UA_STATUSCODE_GOOD;
}

// 写入控制1变量的函数
static UA_StatusCode writeControl1(UA_Server *server, 
                  const UA_NodeId *sessionId, void *sessionContext, 
                  const UA_NodeId *nodeId, void *nodeContext, 
                  const UA_NumericRange *range, const UA_DataValue *data) 
{
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_USERLAND, "write control1 value");
    if(data->value.type != &UA_TYPES[UA_TYPES_DOUBLE]) {
        return UA_STATUSCODE_BADTYPEMISMATCH;
    }
    control1Value = *(UA_Double*)data->value.data;
    return UA_STATUSCODE_GOOD;
}

// 创建MN主节点及其温度、湿度变量
UA_NodeId createMNNode(UA_Server *server, UA_NodeId nodesNodeId) {
    // 创建MN主节点
    UA_NodeId mnNodeId = createObjectNode(server, (char*)"2001:eaca:101:0:001E:CD00:0201:0000", (char*)"MN", nodesNodeId);
    
    // 创建温度1变量
    UA_Variant temp1Value;
    UA_Variant_init(&temp1Value);
    double temp1 = 25.0;
    UA_Variant_setScalar(&temp1Value, &temp1, &UA_TYPES[UA_TYPES_DOUBLE]);
    createVariableNode(server, (char*)"2001:eaca:0101:0000:001E:CD00:0201:0001", (char*)"温度1", mnNodeId, 
                      &UA_TYPES[UA_TYPES_DOUBLE], &temp1Value, UA_ACCESSLEVELMASK_READ, NULL);
    #if 0
    // 创建温度2变量
    UA_Variant temp2Value;
    UA_Variant_init(&temp2Value);
    double temp2 = 26.0;
    UA_Variant_setScalar(&temp2Value, &temp2, &UA_TYPES[UA_TYPES_DOUBLE]);
    createVariableNode(server, (char*)"2001:eaca:101:0:001E:CD00:0201:0002", (char*)"温度2", mnNodeId, 
                      &UA_TYPES[UA_TYPES_DOUBLE], &temp2Value, UA_ACCESSLEVELMASK_READ);
    
    // 创建湿度1变量
    UA_Variant humidity1Value;
    UA_Variant_init(&humidity1Value);
    double humidity1 = 50.0;
    UA_Variant_setScalar(&humidity1Value, &humidity1, &UA_TYPES[UA_TYPES_DOUBLE]);
    createVariableNode(server, (char*)"2001:eaca:101:0:001E:CD00:0201:0003", (char*)"湿度1", mnNodeId, 
                      &UA_TYPES[UA_TYPES_DOUBLE], &humidity1Value, UA_ACCESSLEVELMASK_READ);
    
    // 创建湿度2变量
    UA_Variant humidity2Value;
    UA_Variant_init(&humidity2Value);
    double humidity2 = 55.0;
    UA_Variant_setScalar(&humidity2Value, &humidity2, &UA_TYPES[UA_TYPES_DOUBLE]);
    createVariableNode(server, (char*)"2001:eaca:101:0:001E:CD00:0201:0004", (char*)"湿度2", mnNodeId, 
                      &UA_TYPES[UA_TYPES_DOUBLE], &humidity2Value, UA_ACCESSLEVELMASK_READ);
	#endif
    
    // 创建控制1变量（带数据源）
    UA_VariableAttributes control1Attr = UA_VariableAttributes_default;
    control1Attr.displayName = UA_LOCALIZEDTEXT((char*)"en-US", (char*)"控制1");
    control1Attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    control1Attr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
    
    UA_Guid control1Guid = ipv6ToGuid((char*)"2001:eaca:0101:0000:001E:CD00:0201:0005");
    UA_NodeId control1NodeId = UA_NODEID_GUID(1, control1Guid);
    UA_QualifiedName control1Name = UA_QUALIFIEDNAME(1, (char*)"控制1");
    UA_NodeId parentReferenceNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    UA_NodeId variableTypeNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE);
    
    UA_DataSource control1DataSource;
    control1DataSource.read = readControl1;
    control1DataSource.write = writeControl1;
    UA_Server_addDataSourceVariableNode(server, control1NodeId, mnNodeId, 
                                        parentReferenceNodeId, control1Name, 
                                        variableTypeNodeId, control1Attr, 
                                        control1DataSource, NULL, NULL);
    
    return mnNodeId;
}



UA_NodeId addTheAnswerVariable(UA_Server *server) 
{
    /* Define the attribute of the myInteger variable node */
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Int32 myInteger = 1;
    UA_Variant_setScalar(&attr.value, &myInteger, &UA_TYPES[UA_TYPES_INT32]);
    attr.description = UA_LOCALIZEDTEXT((char*)"en-US", (char*)"the answer");
    attr.displayName = UA_LOCALIZEDTEXT((char*)"en-US", (char*)"the answer");
    attr.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

    /* Add the variable node to the information model */
    UA_NodeId theAnswerNodeId = UA_NODEID_STRING(1, (char*)"the.answer");
    UA_QualifiedName myIntegerName = UA_QUALIFIEDNAME(1, (char*)"the answer");
    UA_NodeId parentNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId parentReferenceNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    UA_Server_addVariableNode(server, theAnswerNodeId, parentNodeId,
                              parentReferenceNodeId, myIntegerName,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, NULL, NULL);
    
    return theAnswerNodeId;
}

UA_NodeId addTheAnswer2Variable(UA_Server *server) 
{
    /* Define the attribute of the myInteger variable node */
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Int32 myInteger = 1;
    UA_Variant_setScalar(&attr.value, &myInteger, &UA_TYPES[UA_TYPES_INT32]);
    attr.description = UA_LOCALIZEDTEXT((char*)"en-US", (char*)"the answer2");
    attr.displayName = UA_LOCALIZEDTEXT((char*)"en-US", (char*)"the answer2");
    attr.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

    /* Add the variable node to the information model */
    UA_NodeId theAnswerNodeId = UA_NODEID_STRING(1, (char*)"the.answer2");
    UA_QualifiedName myIntegerName = UA_QUALIFIEDNAME(1, (char*)"the answer2");
    UA_NodeId parentNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
    UA_NodeId parentReferenceNodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    UA_Server_addVariableNode(server, theAnswerNodeId, parentNodeId,
                              parentReferenceNodeId, myIntegerName,
                              UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, NULL, NULL);
    
    return theAnswerNodeId;
}

void cycleCallback(UA_Server *server, void *data) 
{
    static UA_Int32 update = 2;
    static UA_Int32 update2 = 8;
        
    UA_NodeId * idArray = (UA_NodeId*)data;

    UA_Variant myVar;
    UA_Variant_init(&myVar);
    UA_Variant_setScalar(&myVar, &update, &UA_TYPES[UA_TYPES_INT32]);
    UA_Server_writeValue(server, idArray[0], myVar);

    UA_Variant_init(&myVar);
    UA_Variant_setScalar(&myVar, &update2, &UA_TYPES[UA_TYPES_INT32]);
    UA_Server_writeValue(server, idArray[1], myVar);
    
    update++;
    update2++;
    
    if (update == 100)
    {
        update = 2;
    }

    if (update2 == 200)
    {
        update = 8;
    }
}


#ifdef __cplusplus
extern "C"
#endif
int opcua_server_main(const char *device_name) 
{
    UA_Server *server = UA_Server_new();
    UA_ServerConfig_setDefault(UA_Server_getConfig(server));
    
    // 创建spssps设备节点及其子节点
    UA_NodeId spsspsNodeId = UA_NODEID_NULL;
    
    // 获取Buses节点ID
    UA_NodeId busesNodeId = UA_NODEID_NULL;
    
    // 创建Bus 0对象及其子节点
    UA_NodeId bus0NodeId = UA_NODEID_NULL;
    
    // 获取Nodes节点ID
    UA_NodeId nodesNodeId = UA_NODEID_NULL;
    
    // 初始化全局数据
    GlobalData global_data;
    memset(&global_data, 0, sizeof(global_data));
    global_data.server = server;
    global_data.nodesNodeId = nodesNodeId;
    strncpy(global_data.device_config.name,
            (device_name && device_name[0] != '\0') ? device_name : "spssps",
            sizeof(global_data.device_config.name) - 1);
    strncpy(global_data.device_config.model, "ATB-5000", sizeof(global_data.device_config.model) - 1);
    global_data.device_config.opcua_port = 4840;
    strncpy(global_data.device_config.opcua_path, "/autbus/controller", sizeof(global_data.device_config.opcua_path) - 1);
    
    // 初始化支持递归的互斥锁
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&global_data.var_update_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    
    // 加载配置文件
    if (loadConfig(&global_data) != 0) {
        log_error("Failed to load configuration file");
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }
    
    // 解析CSV文件 - 尝试多种路径
    spsspsNodeId = createSpsspsDevice(server, &global_data, &busesNodeId);
    if (UA_NodeId_isNull(&spsspsNodeId) || UA_NodeId_isNull(&busesNodeId)) {
        log_error("Failed to create root OPC UA device nodes");
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }

    bus0NodeId = createBus0(server, &global_data, busesNodeId, &nodesNodeId);
    if (UA_NodeId_isNull(&bus0NodeId) || UA_NodeId_isNull(&nodesNodeId)) {
        log_error("Failed to create Bus 0 or Nodes OPC UA nodes");
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }
    global_data.nodesNodeId = nodesNodeId;

    CSVParseResult csv_result = parseCSV("./uploadtable.csv");
    if (csv_result.count == 0) {
        csv_result = parseCSV("../uploadtable.csv");
        if (csv_result.count == 0) {
            csv_result = parseCSV("../../uploadtable.csv");
            if (csv_result.count == 0) {
                log_error("Failed to parse CSV file or no data");
                UA_Server_delete(server);
                return EXIT_FAILURE;
            }
        }
    }
    global_data.csv_result = csv_result;
    
    log_debug("CSV file parsed successfully, total %d records", csv_result.count);
    
    // 创建设备节点
    log_debug("Starting to create device nodes");
    if (createDeviceNodes(server, &global_data, nodesNodeId) != 0) {
        log_error("Failed to create device nodes");
        freeCSVResult(&global_data.csv_result);
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }
    log_debug("Device nodes created successfully");

    if (!build_modbus_read_plan(&global_data)) {
        log_error("Failed to build Modbus read plan");
        freeCSVResult(&global_data.csv_result);
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }

    // 启动Modbus轮询线程
    log_debug("Starting to create Modbus polling thread");
    pthread_t modbus_thread;
    if (pthread_create(&modbus_thread, NULL, modbusPollingThread, (void *)&global_data) != 0) {
        log_error("Failed to create Modbus polling thread");
        free_modbus_read_plan(&global_data.modbus_read_plan);
        freeCSVResult(&global_data.csv_result);
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }
    log_debug("Modbus polling thread created successfully");
    
    // 启动定时打印节点变量信息的线程
    log_debug("Skipping node variables printing thread startup");
    pthread_t print_thread = 0;
    bool print_thread_created = false;
    log_debug("Node variables printing thread disabled");
#if 0
    if (pthread_create(&print_thread, NULL, printNodeVariablesThread, (void *)&global_data) != 0) {
        log_error("Failed to create node variables printing thread");
        // 不影响主程序运行，仅记录错误
        log_warn("Node variables printing function will be unavailable");
    } else {
        print_thread_created = true;
        log_debug("Node variables printing thread created successfully");
    }
#endif

    // 运行OPC UA服务器
    log_debug("Starting OPC UA server");
    UA_StatusCode retval = UA_Server_run(server, &running);
    log_debug("OPC UA server stopped, status code: %d", retval);
    
    // 清理资源
    pthread_join(modbus_thread, NULL);
    free_modbus_read_plan(&global_data.modbus_read_plan);
    
    // 取消并清理定时打印线程
    if (print_thread_created && pthread_cancel(print_thread) == 0) {
        pthread_join(print_thread, NULL);
        log_debug("Node variables printing thread stopped");
    }
	
    
    // 释放设备和变量内存
    for (int i = 0; i < global_data.device_count; i++) {
        DeviceNode *device = &global_data.devices[i];
        for (int j = 0; j < device->variable_count; j++) {
            free(device->variables[j].value);
        }
        free(device->variables);
    }
    free(global_data.devices);
    
    // 释放CSV解析结果
    freeCSVResult(&global_data.csv_result);
    
    // 关闭服务器
    UA_Server_delete(server);
    
    return retval == UA_STATUSCODE_GOOD ? EXIT_SUCCESS : EXIT_FAILURE;
}

// 加载配置文件
int loadConfig(GlobalData *global_data) {
    if (!global_data) {
        return -1;
    }

    global_data->ipv6_pool.skip_as_source = true;
    global_data->ipv6_pool.batch_add_limit = 300;
    global_data->ipv6_pool.batch_add_delay_ms = 4000;
    
    // 打开配置文件 - 尝试多种路径
    FILE *file = fopen("./config.json", "r");
    if (!file) {
        file = fopen("../config.json", "r");
        if (!file) {
            file = fopen("../../config.json", "r");
            if (!file) {
                log_error("Failed to open configuration file: config.json");
                return -1;
            }
        }
    }
    
    // 读取文件内容
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char *content = (char *)malloc(length + 1);
    if (!content) {
        fclose(file);
        return -1;
    }
    
    fread(content, 1, length, file);
    content[length] = '\0';
    fclose(file);
    
    // 解析JSON
    cJSON *root = cJSON_Parse(content);
    free(content);
    
    if (!root) {
        log_error("JSON parsing error");
    return -1;
    }
    
    // 解析modbus_tcp配置
    cJSON *device = cJSON_GetObjectItem(root, "device");
    if (device) {
        cJSON *name = cJSON_GetObjectItem(device, "name");
        if (name && cJSON_IsString(name) && name->valuestring[0] != '\0') {
            strncpy(global_data->device_config.name, name->valuestring, sizeof(global_data->device_config.name) - 1);
            global_data->device_config.name[sizeof(global_data->device_config.name) - 1] = '\0';
        }

        cJSON *model = cJSON_GetObjectItem(device, "model");
        if (model && cJSON_IsString(model) && model->valuestring[0] != '\0') {
            strncpy(global_data->device_config.model, model->valuestring, sizeof(global_data->device_config.model) - 1);
            global_data->device_config.model[sizeof(global_data->device_config.model) - 1] = '\0';
        }

        cJSON *opcua_port = cJSON_GetObjectItem(device, "opcua_port");
        if (opcua_port && cJSON_IsNumber(opcua_port)) {
            global_data->device_config.opcua_port = opcua_port->valueint;
        }

        cJSON *opcua_path = cJSON_GetObjectItem(device, "opcua_path");
        if (opcua_path && cJSON_IsString(opcua_path) && opcua_path->valuestring[0] != '\0') {
            strncpy(global_data->device_config.opcua_path, opcua_path->valuestring, sizeof(global_data->device_config.opcua_path) - 1);
            global_data->device_config.opcua_path[sizeof(global_data->device_config.opcua_path) - 1] = '\0';
        }
    }

    cJSON *modbus_tcp = cJSON_GetObjectItem(root, "modbus_tcp");
    if (modbus_tcp) {
        cJSON *server_ip = cJSON_GetObjectItem(modbus_tcp, "server_ip");
        if (server_ip && cJSON_IsString(server_ip)) {
            strncpy(global_data->modbus_config.server_ip, server_ip->valuestring, sizeof(global_data->modbus_config.server_ip) - 1);
            global_data->modbus_config.server_ip[sizeof(global_data->modbus_config.server_ip) - 1] = '\0';
        }
        
        cJSON *server_port = cJSON_GetObjectItem(modbus_tcp, "server_port");
        if (server_port && cJSON_IsNumber(server_port)) {
            global_data->modbus_config.server_port = server_port->valueint;
        }
        
        cJSON *polling_interval = cJSON_GetObjectItem(modbus_tcp, "polling_interval");
        if (polling_interval && cJSON_IsNumber(polling_interval)) {
            global_data->modbus_config.polling_interval = polling_interval->valueint;
        }
        
        cJSON *reconnect_interval = cJSON_GetObjectItem(modbus_tcp, "reconnect_interval");
        if (reconnect_interval && cJSON_IsNumber(reconnect_interval)) {
            global_data->modbus_config.reconnect_interval = reconnect_interval->valueint;
        } else {
            // 默认重连间隔为5秒
            global_data->modbus_config.reconnect_interval = 5000;
        }
    }
    
    // 解析ipv6_multicast配置
    cJSON *ipv6_multicast = cJSON_GetObjectItem(root, "ipv6_multicast");
    if (ipv6_multicast) {
        cJSON *listen_port = cJSON_GetObjectItem(ipv6_multicast, "listen_port");
        if (listen_port && cJSON_IsNumber(listen_port)) {
            global_data->ipv6_multicast.listen_port = listen_port->valueint;
        }
        
        cJSON *multicast_ip = cJSON_GetObjectItem(ipv6_multicast, "multicast_ip");
        if (multicast_ip && cJSON_IsString(multicast_ip)) {
            strncpy(global_data->ipv6_multicast.multicast_ip, multicast_ip->valuestring, sizeof(global_data->ipv6_multicast.multicast_ip) - 1);
            global_data->ipv6_multicast.multicast_ip[sizeof(global_data->ipv6_multicast.multicast_ip) - 1] = '\0';
        }
        
        cJSON *nic_index = cJSON_GetObjectItem(ipv6_multicast, "nic_index");
        if (nic_index && cJSON_IsNumber(nic_index)) {
            global_data->ipv6_multicast.nic_index = nic_index->valueint;
        }
    }
    
    // 解析ipv6_address_pool配置
    cJSON *ipv6_pool = cJSON_GetObjectItem(root, "ipv6_address_pool");
    if (ipv6_pool) {
        cJSON *start_address = cJSON_GetObjectItem(ipv6_pool, "start_address");
        if (start_address && cJSON_IsString(start_address)) {
            strncpy(global_data->ipv6_pool.start_address, start_address->valuestring, sizeof(global_data->ipv6_pool.start_address) - 1);
            global_data->ipv6_pool.start_address[sizeof(global_data->ipv6_pool.start_address) - 1] = '\0';
        }
        
        cJSON *end_address = cJSON_GetObjectItem(ipv6_pool, "end_address");
        if (end_address && cJSON_IsString(end_address)) {
            strncpy(global_data->ipv6_pool.end_address, end_address->valuestring, sizeof(global_data->ipv6_pool.end_address) - 1);
            global_data->ipv6_pool.end_address[sizeof(global_data->ipv6_pool.end_address) - 1] = '\0';
        }
        
        cJSON *prefix_length = cJSON_GetObjectItem(ipv6_pool, "prefix_length");
        if (prefix_length && cJSON_IsNumber(prefix_length)) {
            global_data->ipv6_pool.prefix_length = prefix_length->valueint;
        }
        
        cJSON *max_lease_count = cJSON_GetObjectItem(ipv6_pool, "max_lease_count");
        if (max_lease_count && cJSON_IsNumber(max_lease_count)) {
            global_data->ipv6_pool.max_lease_count = max_lease_count->valueint;
        }

        cJSON *skip_as_source = cJSON_GetObjectItem(ipv6_pool, "skip_as_source");
        if (skip_as_source && cJSON_IsBool(skip_as_source)) {
            global_data->ipv6_pool.skip_as_source = cJSON_IsTrue(skip_as_source);
        }

        cJSON *batch_add_limit = cJSON_GetObjectItem(ipv6_pool, "batch_add_limit");
        if (batch_add_limit && cJSON_IsNumber(batch_add_limit) && batch_add_limit->valueint >= 0) {
            global_data->ipv6_pool.batch_add_limit = (unsigned int)batch_add_limit->valueint;
        }

        cJSON *batch_add_delay_ms = cJSON_GetObjectItem(ipv6_pool, "batch_add_delay_ms");
        if (batch_add_delay_ms && cJSON_IsNumber(batch_add_delay_ms) && batch_add_delay_ms->valueint >= 0) {
            global_data->ipv6_pool.batch_add_delay_ms = (unsigned int)batch_add_delay_ms->valueint;
        }
    }

    ipv6_manager_set_add_options(global_data->ipv6_pool.skip_as_source,
                                  global_data->ipv6_pool.batch_add_limit,
                                  global_data->ipv6_pool.batch_add_delay_ms);
    
    cJSON_Delete(root);
    
    return 0;
}

// 解析IPv6地址为8个16位段
int parseIPv6Address(const char *addr, uint16_t *segments) {
    if (!addr || !segments) {
        return -1;
    }
    
    // 简单的IPv6地址解析
    int count = sscanf(addr, "%hx:%hx:%hx:%hx:%hx:%hx:%hx:%hx", 
                     &segments[0], &segments[1], &segments[2], &segments[3],
                     &segments[4], &segments[5], &segments[6], &segments[7]);
    
    return (count == 8) ? 0 : -1;
}

// 根据索引生成IPv6地址
char *generateIPv6Address(uint16_t *segments, int index) {
    if (!segments) {
        return NULL;
    }
    
    static char addr[40];
    
    // 基于起始地址生成新地址
    uint16_t new_segments[8];
    memcpy(new_segments, segments, sizeof(new_segments));
    
    // 简单地递增最后两个16位段
    new_segments[6] += (index >> 16) & 0xFFFF;
    new_segments[7] += index & 0xFFFF;
    
    sprintf(addr, "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
            new_segments[0], new_segments[1], new_segments[2], new_segments[3],
            new_segments[4], new_segments[5], new_segments[6], new_segments[7]);
    
    return addr;
}

// 将PLC数据类型转换为OPC UA数据类型
UA_DataType *getOPCUAType(PLCDatatype plc_type) {
    switch (plc_type) {
        case PLC_TYPE_BOOL:
            return &UA_TYPES[UA_TYPES_BOOLEAN];
        case PLC_TYPE_USINT:
            return &UA_TYPES[UA_TYPES_BYTE];
        case PLC_TYPE_UINT:
            return &UA_TYPES[UA_TYPES_UINT16];
        case PLC_TYPE_ULINT:
            return &UA_TYPES[UA_TYPES_UINT64];
        case PLC_TYPE_INT:
            return &UA_TYPES[UA_TYPES_INT16];
        case PLC_TYPE_DINT:
            return &UA_TYPES[UA_TYPES_INT32];
        case PLC_TYPE_REAL:
            return &UA_TYPES[UA_TYPES_FLOAT];
        default:
            return &UA_TYPES[UA_TYPES_UINT16];
    }
}



// 根据寄存器类型获取访问级别
UA_Byte getAccessLevel(RegisterType reg_type) {
    switch (reg_type) {
        case REGISTER_TYPE_DISCRETE_INPUT:
        case REGISTER_TYPE_INPUT_REGISTER:
            return UA_ACCESSLEVELMASK_READ;
        case REGISTER_TYPE_COIL:
        case REGISTER_TYPE_HOLDING_REGISTER:
            return UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
        default:
            return UA_ACCESSLEVELMASK_READ;
    }
}

// 读取值回调函数
static UA_StatusCode readVariableCallback(UA_Server *server, const UA_NodeId *sessionId, void *sessionContext,
                                         const UA_NodeId *nodeId, void *nodeContext, UA_Boolean sourceTimeStamp,
                                         const UA_NumericRange *range, UA_DataValue *dataValue) {
    // 检查参数有效性
    if (!nodeContext || !dataValue) {
        log_error("readVariableCallback: Invalid parameters");
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    
    GlobalData *global_data = (GlobalData *)nodeContext;
    OPCUAVariable *var = NULL;
    pthread_mutex_lock(&global_data->var_update_mutex);
    
    // 查找对应的变量
    for (int i = 0; i < global_data->device_count; i++) {
        DeviceNode *device = &global_data->devices[i];
        for (int j = 0; j < device->variable_count; j++) {
            if (UA_NodeId_equal(&device->variables[j].node_id, nodeId)) {
                var = &device->variables[j];
                break;
            }
        }
        if (var) {
            break;
        }
    }
    
    if (!var) {
        log_error("readVariableCallback: Variable not found for nodeId");
        pthread_mutex_unlock(&global_data->var_update_mutex);
        return UA_STATUSCODE_BADNODEIDUNKNOWN;
    }
    
    // 确保变量值不为空
    if (!var->value) {
        log_error("readVariableCallback: Variable value is NULL for %s", var->name);
        // 如果变量值为空，使用默认值
        // 先清理旧值
        UA_Variant_clear(&dataValue->value);
        
        UA_UInt16 default_val = 0;
        UA_Variant_setScalarCopy(&dataValue->value, &default_val, &UA_TYPES[UA_TYPES_UINT16]);
        dataValue->hasValue = true;
        pthread_mutex_unlock(&global_data->var_update_mutex);
        return UA_STATUSCODE_GOOD;
    }
    
    // 根据数据类型设置值
    // 先清理旧值
    UA_Variant_clear(&dataValue->value);
    
    switch (var->plc_datatype) {
        case PLC_TYPE_BOOL: {
            UA_Boolean val = *((UA_Boolean *)var->value);
            UA_Variant_setScalarCopy(&dataValue->value, &val, &UA_TYPES[UA_TYPES_BOOLEAN]);
            break;
        }
        case PLC_TYPE_USINT: {
            UA_Byte val = *((UA_Byte *)var->value);
            UA_Variant_setScalarCopy(&dataValue->value, &val, &UA_TYPES[UA_TYPES_BYTE]);
            break;
        }
        case PLC_TYPE_UINT: {
            UA_UInt16 val = *((UA_UInt16 *)var->value);
            UA_Variant_setScalarCopy(&dataValue->value, &val, &UA_TYPES[UA_TYPES_UINT16]);
            break;
        }
        case PLC_TYPE_ULINT: {
            UA_UInt64 val = *((UA_UInt64 *)var->value);
            UA_Variant_setScalarCopy(&dataValue->value, &val, &UA_TYPES[UA_TYPES_UINT64]);
            break;
        }
        case PLC_TYPE_INT: {
            UA_Int16 val = *((UA_Int16 *)var->value);
            UA_Variant_setScalarCopy(&dataValue->value, &val, &UA_TYPES[UA_TYPES_INT16]);
            break;
        }
        case PLC_TYPE_DINT: {
            UA_Int32 val = *((UA_Int32 *)var->value);
            UA_Variant_setScalarCopy(&dataValue->value, &val, &UA_TYPES[UA_TYPES_INT32]);
            break;
        }
        case PLC_TYPE_REAL: {
            UA_Float val = *((UA_Float *)var->value);
            UA_Variant_setScalarCopy(&dataValue->value, &val, &UA_TYPES[UA_TYPES_FLOAT]);
            break;
        }
        default: {
            UA_UInt16 val = *((UA_UInt16 *)var->value);
            UA_Variant_setScalarCopy(&dataValue->value, &val, &UA_TYPES[UA_TYPES_UINT16]);
            break;
        }
    }
    dataValue->hasValue = true;
    
    log_debug("readVariableCallback: Successfully read value for %s", var->name);
    pthread_mutex_unlock(&global_data->var_update_mutex);
    return UA_STATUSCODE_GOOD;
}

// 写入值回调函数
static UA_StatusCode writeVariableCallback(UA_Server *server, const UA_NodeId *sessionId, void *sessionContext,
                                         const UA_NodeId *nodeId, void *nodeContext, const UA_NumericRange *range,
                                         const UA_DataValue *dataValue) {
    if (!nodeContext || !dataValue->hasValue) {
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    
    GlobalData *global_data = (GlobalData *)nodeContext;
    
    // 检查是否是Modbus更新，如果是则不执行写操作，防止循环调用
    if (global_data->is_modbus_updating) {
        return UA_STATUSCODE_GOOD;
    }
    
    // 申请互斥锁
    pthread_mutex_lock(&global_data->var_update_mutex);
    
    OPCUAVariable *var = NULL;
    
    // 查找对应的变量
    for (int i = 0; i < global_data->device_count; i++) {
        DeviceNode *device = &global_data->devices[i];
        for (int j = 0; j < device->variable_count; j++) {
            if (UA_NodeId_equal(&device->variables[j].node_id, nodeId)) {
                var = &device->variables[j];
                break;
            }
        }
        if (var) {
            break;
        }
    }
    
    if (!var) {
        // 释放互斥锁
        pthread_mutex_unlock(&global_data->var_update_mutex);
        return UA_STATUSCODE_BADNODEIDUNKNOWN;
    }
    
    // 根据数据类型处理写入操作
    bool modbus_write_success = false;
    int pdu_addr = modbus_reference_to_pdu_addr(var->register_type, var->modbus_addr);
    if (pdu_addr < 0) {
        log_error("Invalid Modbus address for variable %s, modbus_addr=%d, register_type=%s",
                  var->name, var->modbus_addr, register_type_to_string(var->register_type));
        pthread_mutex_unlock(&global_data->var_update_mutex);
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    
    switch (var->register_type) {
        case REGISTER_TYPE_HOLDING_REGISTER: {
            switch (var->plc_datatype) {
                case PLC_TYPE_BOOL:
                    // 布尔值作为16位寄存器处理
                    if (dataValue->value.type == &UA_TYPES[UA_TYPES_BOOLEAN]) {
                        UA_Boolean bool_val = *(UA_Boolean *)dataValue->value.data;
                        uint16_t reg_val = bool_val ? 1 : 0;
                        modbus_write_success = (modbus_client_write_single_register(&global_data->modbus_client, pdu_addr, reg_val) == MODBUS_SUCCESS);
                    }
                    break;
                case PLC_TYPE_USINT:
                    if (dataValue->value.type == &UA_TYPES[UA_TYPES_BYTE]) {
                        UA_Byte byte_val = *(UA_Byte *)dataValue->value.data;
                        uint16_t reg_val = byte_val;
                        modbus_write_success = (modbus_client_write_single_register(&global_data->modbus_client, pdu_addr, reg_val) == MODBUS_SUCCESS);
                    }
                    break;
                case PLC_TYPE_UINT:
                    if (dataValue->value.type == &UA_TYPES[UA_TYPES_UINT16]) {
                        UA_UInt16 uint16_val = *(UA_UInt16 *)dataValue->value.data;
                        modbus_write_success = (modbus_client_write_single_register(&global_data->modbus_client, pdu_addr, uint16_val) == MODBUS_SUCCESS);
                    }
                    break;
                case PLC_TYPE_REAL:
                    if (dataValue->value.type == &UA_TYPES[UA_TYPES_FLOAT]) {
                        UA_Float float_val = *(UA_Float *)dataValue->value.data;
                        uint32_t raw = *((uint32_t *)&float_val);
                        // 小端序，先写低16位，再写高16位
                        uint16_t reg_val_low = raw & 0xFFFF;
                        uint16_t reg_val_high = (raw >> 16) & 0xFFFF;
                        int reg_addr = pdu_addr;
                        
                        // 分别写入两个寄存器
                        modbus_write_success = (modbus_client_write_single_register(&global_data->modbus_client, reg_addr, reg_val_low) == MODBUS_SUCCESS);
                        if (modbus_write_success) {
                            modbus_write_success = (modbus_client_write_single_register(&global_data->modbus_client, reg_addr + 1, reg_val_high) == MODBUS_SUCCESS);
                        }
                    }
                    break;
            }
            break;
        }
        case REGISTER_TYPE_COIL:
            if (var->plc_datatype == PLC_TYPE_BOOL && dataValue->value.type == &UA_TYPES[UA_TYPES_BOOLEAN]) {
                UA_Boolean bool_val = *(UA_Boolean *)dataValue->value.data;
                modbus_write_success = (modbus_client_write_single_coil(&global_data->modbus_client, pdu_addr, bool_val) == MODBUS_SUCCESS);
            }
            break;
    }
    
    if (!modbus_write_success) {
        int saved_errno = errno;
        log_error("Failed to write value to Modbus server for variable %s, modbus_addr=%d, pdu_addr=%d, register_type=%s, errno=%d (%s)",
                  var->name, var->modbus_addr, pdu_addr,
                  register_type_to_string(var->register_type),
                  saved_errno, modbus_strerror(saved_errno));
        // 释放互斥锁
        pthread_mutex_unlock(&global_data->var_update_mutex);
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    
    // 更新本地变量值
    if (var->value != NULL) {
        switch (var->plc_datatype) {
            case PLC_TYPE_BOOL:
                if (dataValue->value.type == &UA_TYPES[UA_TYPES_BOOLEAN]) {
                    *((UA_Boolean *)var->value) = *(UA_Boolean *)dataValue->value.data;
                }
                break;
            case PLC_TYPE_USINT:
                if (dataValue->value.type == &UA_TYPES[UA_TYPES_BYTE]) {
                    *((UA_Byte *)var->value) = *(UA_Byte *)dataValue->value.data;
                }
                break;
            case PLC_TYPE_UINT:
                if (dataValue->value.type == &UA_TYPES[UA_TYPES_UINT16]) {
                    *((UA_UInt16 *)var->value) = *(UA_UInt16 *)dataValue->value.data;
                }
                break;
            case PLC_TYPE_ULINT:
                if (dataValue->value.type == &UA_TYPES[UA_TYPES_UINT64]) {
                    *((UA_UInt64 *)var->value) = *(UA_UInt64 *)dataValue->value.data;
                }
                break;
            case PLC_TYPE_INT:
                if (dataValue->value.type == &UA_TYPES[UA_TYPES_INT16]) {
                    *((UA_Int16 *)var->value) = *(UA_Int16 *)dataValue->value.data;
                }
                break;
            case PLC_TYPE_DINT:
                if (dataValue->value.type == &UA_TYPES[UA_TYPES_INT32]) {
                    *((UA_Int32 *)var->value) = *(UA_Int32 *)dataValue->value.data;
                }
                break;
            case PLC_TYPE_REAL:
                if (dataValue->value.type == &UA_TYPES[UA_TYPES_FLOAT]) {
                    *((UA_Float *)var->value) = *(UA_Float *)dataValue->value.data;
                }
                break;
            default:
                break;
        }
    }
    
    log_info("Successfully wrote value to Modbus server for variable %s", var->name);
    
    // 释放互斥锁
    pthread_mutex_unlock(&global_data->var_update_mutex);
    return UA_STATUSCODE_GOOD;
}

// 创建默认值
UA_Variant *createDefaultValue(PLCDatatype plc_type) {
    UA_Variant *variant = (UA_Variant *)malloc(sizeof(UA_Variant));
    UA_Variant_init(variant);
    
    switch (plc_type) {
        case PLC_TYPE_BOOL: {
            UA_Boolean bool_val = false;
            UA_Variant_setScalar(variant, &bool_val, &UA_TYPES[UA_TYPES_BOOLEAN]);
            break;
        }
        case PLC_TYPE_USINT: {
            UA_Byte byte_val = 0;
            UA_Variant_setScalar(variant, &byte_val, &UA_TYPES[UA_TYPES_BYTE]);
            break;
        }
        case PLC_TYPE_UINT: {
            UA_UInt16 uint16_val = 0;
            UA_Variant_setScalar(variant, &uint16_val, &UA_TYPES[UA_TYPES_UINT16]);
            break;
        }
        case PLC_TYPE_ULINT: {
            UA_UInt64 uint64_val = 0;
            UA_Variant_setScalar(variant, &uint64_val, &UA_TYPES[UA_TYPES_UINT64]);
            break;
        }
        case PLC_TYPE_INT: {
            UA_Int16 int16_val = 0;
            UA_Variant_setScalar(variant, &int16_val, &UA_TYPES[UA_TYPES_INT16]);
            break;
        }
        case PLC_TYPE_DINT: {
            UA_Int32 int32_val = 0;
            UA_Variant_setScalar(variant, &int32_val, &UA_TYPES[UA_TYPES_INT32]);
            break;
        }
        case PLC_TYPE_REAL: {
            UA_Float float_val = 0.0f;
            UA_Variant_setScalar(variant, &float_val, &UA_TYPES[UA_TYPES_FLOAT]);
            break;
        }
        default: {
            UA_UInt16 default_val = 0;
            UA_Variant_setScalar(variant, &default_val, &UA_TYPES[UA_TYPES_UINT16]);
            break;
        }
    }
    
    return variant;
}

// 初始化设备节点结构（不创建OPC UA节点）
int createDeviceNodes(UA_Server *server, GlobalData *global_data, UA_NodeId nodesNodeId) {
    if (!server || !global_data) {
        return -1;
    }
    
    // 跳过解析起始IPv6地址，直接使用硬编码格式生成节点ID
    // 后续可以根据需要重新实现IPv6地址解析功能
    
    // 统计设备数量
    int device_count = 0;
    char current_device[50] = "";
    
    log_debug("Starting to count devices");
    for (int i = 0; i < global_data->csv_result.count; i++) {
        CSVRecord *record = &global_data->csv_result.records[i];
        if (strcmp(record->deviceName, current_device) != 0) {
            device_count++;
            strncpy(current_device, record->deviceName, sizeof(current_device) - 1);
            current_device[sizeof(current_device) - 1] = '\0';
            log_debug("Found device %s, total devices: %d", current_device, device_count);
        }
    }
    
    log_debug("Device count completed, total %d devices", device_count);
    
    // 分配设备节点内存
    log_debug("Starting to allocate device node memory, size: %lld bytes", (long long)sizeof(DeviceNode) * device_count);
    global_data->devices = (DeviceNode *)malloc(sizeof(DeviceNode) * device_count);
    if (!global_data->devices) {
        log_error("Failed to allocate device node memory");
        return -1;
    }
    memset(global_data->devices, 0, sizeof(DeviceNode) * device_count);
    log_debug("Device node memory allocated successfully");
    
    global_data->device_count = device_count;
    
    // 初始化设备节点结构
    int device_index = 0;
    
    log_debug("Starting to initialize device nodes");
    for (int i = 0; i < global_data->csv_result.count; i++) {
        CSVRecord *record = &global_data->csv_result.records[i];
        
        // 跳过设备名称为空的记录
        if (record->deviceName[0] == '\0') {
            continue;
        }
        
        // 检查是否是新设备
        if (device_index == 0 || strcmp(record->deviceName, global_data->devices[device_index - 1].name) != 0) {
            // 初始化设备节点结构，但不创建OPC UA节点
            strncpy(global_data->devices[device_index].name, record->deviceName, sizeof(global_data->devices[device_index].name) - 1);
            global_data->devices[device_index].name[sizeof(global_data->devices[device_index].name) - 1] = '\0';
            global_data->devices[device_index].variable_count = 0;
            global_data->devices[device_index].variables = NULL;
            
            // 初始化节点ID（后面会被动态创建时覆盖）
            global_data->devices[device_index].node_id = UA_NODEID_NULL;
            
            device_index++;
        }
    }
    log_debug("Device nodes initialized successfully");
    
    // 为每个设备初始化变量结构
    for (int i = 0; i < global_data->device_count; i++) {
        DeviceNode *device = &global_data->devices[i];
        
        // 统计该设备的变量数量
        int var_count = 0;
        for (int j = 0; j < global_data->csv_result.count; j++) {
            CSVRecord *record = &global_data->csv_result.records[j];
            if (strcmp(record->deviceName, device->name) == 0) {
                var_count++;
            }
        }
        
        // 分配变量内存
        device->variables = (OPCUAVariable *)malloc(sizeof(OPCUAVariable) * var_count);
        if (!device->variables) {
            return -1;
        }
        
        device->variable_count = var_count;
        
        // 初始化变量结构
        int var_index = 0;
        for (int j = 0; j < global_data->csv_result.count; j++) {
            CSVRecord *record = &global_data->csv_result.records[j];
            if (strcmp(record->deviceName, device->name) == 0) {
                // 设置变量属性
                device->variables[var_index].modbus_addr = record->modbusAddr;
                device->variables[var_index].register_type = record->registerType;
                device->variables[var_index].plc_datatype = record->plcDatatype;
                strncpy(device->variables[var_index].name, record->nodeName, sizeof(device->variables[var_index].name) - 1);
                device->variables[var_index].name[sizeof(device->variables[var_index].name) - 1] = '\0';
                
                // 初始化IPv6地址为空字符串
                device->variables[var_index].ipv6_address[0] = '\0';
                
                // 初始化节点ID（后面会被动态创建时覆盖）
                device->variables[var_index].node_id = UA_NODEID_NULL;
                
                // 保存值指针并初始化
                if (strcmp(record->desc, "undefined") == 0) {
                    // 如果desc为undefined，使用UInt16类型
                    device->variables[var_index].value = malloc(sizeof(UA_UInt16));
                    *((UA_UInt16 *)device->variables[var_index].value) = 0;
                } else {
                    switch (record->plcDatatype) {
                        case PLC_TYPE_BOOL:
                            device->variables[var_index].value = malloc(sizeof(UA_Boolean));
                            *((UA_Boolean *)device->variables[var_index].value) = false;
                            break;
                        case PLC_TYPE_USINT:
                            device->variables[var_index].value = malloc(sizeof(UA_Byte));
                            *((UA_Byte *)device->variables[var_index].value) = 0;
                            break;
                        case PLC_TYPE_UINT:
                            device->variables[var_index].value = malloc(sizeof(UA_UInt16));
                            *((UA_UInt16 *)device->variables[var_index].value) = 0;
                            break;
                        case PLC_TYPE_ULINT:
                            device->variables[var_index].value = malloc(sizeof(UA_UInt64));
                            *((UA_UInt64 *)device->variables[var_index].value) = 0;
                            break;
                        case PLC_TYPE_INT:
                            device->variables[var_index].value = malloc(sizeof(UA_Int16));
                            *((UA_Int16 *)device->variables[var_index].value) = 0;
                            break;
                        case PLC_TYPE_DINT:
                            device->variables[var_index].value = malloc(sizeof(UA_Int32));
                            *((UA_Int32 *)device->variables[var_index].value) = 0;
                            break;
                        case PLC_TYPE_REAL:
                            device->variables[var_index].value = malloc(sizeof(UA_Float));
                            *((UA_Float *)device->variables[var_index].value) = 0.0f;
                            break;
                        default:
                            device->variables[var_index].value = NULL;
                            break;
                    }
                }
                
                var_index++;
            }
        }
    }

	 log_debug("createDeviceNodes function executed successfully");
	
	return 0;
}

// 检查记录是否为设备状态变量
int isDeviceStatusVariable(CSVRecord *record) {
    if (!record) {
        return 0;
    }
    
    // 检查nodeName是否为"设备名_status"格式
    char status_node_name[150];
    snprintf(status_node_name, sizeof(status_node_name), "%s_status", record->deviceName);
    
    return strcmp(record->nodeName, status_node_name) == 0;
}

// 检查设备状态
int checkDeviceStatus(GlobalData *global_data, const char *device_name) {
    if (!global_data || !device_name) {
        return 0;
    }
    
    // 查找设备
    for (int i = 0; i < global_data->device_count; i++) {
        DeviceNode *device = &global_data->devices[i];
        if (strcmp(device->name, device_name) == 0) {
            // 查找设备状态变量
            char status_var_name[150];
            snprintf(status_var_name, sizeof(status_var_name), "%s_status", device_name);
            
            for (int j = 0; j < device->variable_count; j++) {
                OPCUAVariable *var = &device->variables[j];
                if (strcmp(var->name, status_var_name) == 0) {
                    // 检查状态值
                    if (var->value) {
                        // 状态变量应该是UInt16类型
                        return *((UA_UInt16 *)var->value) == 0;
                    }
                }
            }
        }
    }
    
    return 0;
}

// 删除设备节点
int deleteDeviceNodes(UA_Server *server, GlobalData *global_data, int device_index) {
    if (!server || !global_data || device_index < 0 || device_index >= global_data->device_count) {
        return -1;
    }
    
    // 申请互斥锁，保护设备列表和变量资源访问
    //pthread_mutex_lock(&global_data->var_update_mutex);
    
    DeviceNode *device = &global_data->devices[device_index];
    log_debug("Deleting device %s and its variables", device->name);
    
    // 删除设备的所有变量节点
    for (int j = 0; j < device->variable_count; j++) {
        OPCUAVariable *var = &device->variables[j];
        // 删除OPC UA变量节点
        if (!UA_NodeId_isNull(&var->node_id)) {
            UA_Server_deleteNode(server, var->node_id, true);
            // 重置变量节点ID
            var->node_id = UA_NODEID_NULL;
        }
        // 从网卡删除变量IPv6地址，但保留分配关系，重复上线时继续使用同一个地址
        if (var->ipv6_address[0] != '\0') {
            removePersistentIPv6FromInterface(global_data, var->name, var->ipv6_address);
        }
    }
    
    // 删除设备对象节点
    if (!UA_NodeId_isNull(&device->node_id)) {
        UA_Server_deleteNode(server, device->node_id, true);
        // 重置设备节点ID
        device->node_id = UA_NODEID_NULL;
    }
    
    // 释放variables数组内存并重置指针
    
    log_debug("Device %s deleted successfully, cached variables retained for reconnection", device->name);
    
    // 释放互斥锁
    //pthread_mutex_unlock(&global_data->var_update_mutex);
    
    return 0;
}

// 添加设备节点
int addDeviceNodes(UA_Server *server, GlobalData *global_data, int device_index, UA_NodeId nodesNodeId) {
    if (!server || !global_data || device_index < 0 || device_index >= global_data->device_count) {
        return -1;
    }
    
    // 申请互斥锁，保护设备列表和变量资源访问
    //pthread_mutex_lock(&global_data->var_update_mutex);
    
    DeviceNode *device = &global_data->devices[device_index];
    
    // 如果设备名称为空，说明已被删除，无法添加
    if (device->name[0] == '\0') {
        //pthread_mutex_unlock(&global_data->var_update_mutex);
        return -1;
    }
    
    // 检查设备节点是否已经存在，如果存在则直接返回
    if (!UA_NodeId_isNull(&device->node_id)) {
        log_debug("Device node for %s already exists", device->name);
        //pthread_mutex_unlock(&global_data->var_update_mutex);
        return 0;
    }
    
    log_debug("Adding device %s and its variables", device->name);
    
    // 设备的IPv6地址已经在modbusPollingThread中分配，直接使用
    if (device->ipv6_address[0] == '\0') {
        log_error("Device %s has no IPv6 address allocated", device->name);
        //pthread_mutex_unlock(&global_data->var_update_mutex);
        return -1;
    }
    
    // 创建设备节点
    device->node_id = createObjectNode(server, device->ipv6_address, device->name, nodesNodeId);
    
    log_debug("Successfully created device node for %s with IPv6 address %s",
             device->name, device->ipv6_address);
    
    // 移除手动添加的Status变量，因为状态变量会通过CSV记录创建
    
    // 统计该设备的变量数量
    int var_count = 0;
    for (int j = 0; j < global_data->csv_result.count; j++) {
        CSVRecord *record = &global_data->csv_result.records[j];
        if (strcmp(record->deviceName, device->name) == 0) {
            var_count++;
        }
    }
    
    // 检查变量内存是否已经分配，如果已经分配则直接使用
    if (device->variables != NULL) {
        log_debug("Variable memory for device %s already allocated", device->name);
    } else {
        // 分配变量内存
        device->variables = (OPCUAVariable *)malloc(sizeof(OPCUAVariable) * var_count);
        if (!device->variables) {
            log_error("Failed to allocate variable memory for device %s", device->name);
            //pthread_mutex_unlock(&global_data->var_update_mutex);
            return -1;
        }
        memset(device->variables, 0, sizeof(OPCUAVariable) * var_count);
    }
    
    device->variable_count = var_count;
    
    // 创建变量节点
    int var_index = 0;
    for (int j = 0; j < global_data->csv_result.count; j++) {
        CSVRecord *record = &global_data->csv_result.records[j];
        if (strcmp(record->deviceName, device->name) == 0) {
            // 获取OPC UA类型和访问级别
            UA_DataType *opcua_type;
            
            // 检查是否为设备状态变量
            int is_status_var = isDeviceStatusVariable(record);
            
            // 如果是状态变量，数据类型为UInt16
            if (is_status_var) {
                opcua_type = &UA_TYPES[UA_TYPES_UINT16];
            } else {
                opcua_type = getOPCUAType(record->plcDatatype);
            }
            UA_Byte access_level = getAccessLevel(record->registerType);
            
            // 直接创建UA_Variant对象（不使用动态分配）
            UA_Variant default_value;
            UA_Variant_init(&default_value);
            
            // 设置默认值
            if (is_status_var) {
                // 如果是状态变量，使用UInt16类型的默认值
                UA_UInt16 uint16_val = 0;
                UA_Variant_setScalar(&default_value, &uint16_val, opcua_type);
            } else {
                switch (record->plcDatatype) {
                    case PLC_TYPE_BOOL: {
                        UA_Boolean bool_val = false;
                        UA_Variant_setScalar(&default_value, &bool_val, opcua_type);
                        break;
                    }
                    case PLC_TYPE_USINT: {
                        UA_Byte byte_val = 0;
                        UA_Variant_setScalar(&default_value, &byte_val, opcua_type);
                        break;
                    }
                    case PLC_TYPE_UINT: {
                        UA_UInt16 uint16_val = 0;
                        UA_Variant_setScalar(&default_value, &uint16_val, opcua_type);
                        break;
                    }
                    case PLC_TYPE_ULINT: {
                        UA_UInt64 uint64_val = 0;
                        UA_Variant_setScalar(&default_value, &uint64_val, opcua_type);
                        break;
                    }
                    case PLC_TYPE_INT: {
                        UA_Int16 int16_val = 0;
                        UA_Variant_setScalar(&default_value, &int16_val, opcua_type);
                        break;
                    }
                    case PLC_TYPE_DINT: {
                        UA_Int32 int32_val = 0;
                        UA_Variant_setScalar(&default_value, &int32_val, opcua_type);
                        break;
                    }
                    case PLC_TYPE_REAL: {
                        UA_Float float_val = 0.0f;
                        UA_Variant_setScalar(&default_value, &float_val, opcua_type);
                        break;
                    }
                    default: {
                        UA_UInt16 default_val = 0;
                        UA_Variant_setScalar(&default_value, &default_val, &UA_TYPES[UA_TYPES_UINT16]);
                        break;
                    }
                }
            }
            
            // 设置变量属性
            device->variables[var_index].modbus_addr = record->modbusAddr;
            device->variables[var_index].register_type = record->registerType;
            device->variables[var_index].plc_datatype = record->plcDatatype;
            strncpy(device->variables[var_index].name, record->nodeName, sizeof(device->variables[var_index].name) - 1);
            device->variables[var_index].name[sizeof(device->variables[var_index].name) - 1] = '\0';
            
            if (device->variables[var_index].ipv6_address[0] != '\0') {
                log_debug("Reusing IPv6 address %s for variable %s",
                         device->variables[var_index].ipv6_address, record->nodeName);
                ensurePersistentIPv6OnInterface(global_data, record->nodeName,
                                                device->variables[var_index].ipv6_address);
            } else if (ipv6_allocate_address(
                    record->nodeName,
                    global_data->ipv6_multicast.nic_index,
                    device->variables[var_index].ipv6_address
                )) {
                log_debug("Successfully allocated IPv6 address %s for variable %s",
                         device->variables[var_index].ipv6_address, record->nodeName);
            } else {
                log_error("Failed to allocate IPv6 address for variable %s", record->nodeName);
                device->variables[var_index].ipv6_address[0] = '\0';
            }
                
            // 创建变量节点
            if (device->variables[var_index].ipv6_address[0] != '\0') {
                device->variables[var_index].node_id = createVariableNode(
                    server, device->variables[var_index].ipv6_address, record->nodeName, device->node_id,
                    opcua_type, &default_value, access_level, global_data);
            } else {
                // 仍然创建节点，但使用默认的IPv6地址
                char default_ipv6_addr[40] = "2001:eaca:101:0:001E:CD00:0201:0000";
                device->variables[var_index].node_id = createVariableNode(
                    server, default_ipv6_addr, record->nodeName, device->node_id,
                    opcua_type, &default_value, access_level, global_data);
            }
            
            // 保存值指针
            // 先释放旧的内存（如果存在）
            if (device->variables[var_index].value == NULL) {
            if (is_status_var) {
                // 如果是状态变量，使用UInt16类型
                device->variables[var_index].value = malloc(sizeof(UA_UInt16));
                if (device->variables[var_index].value != NULL) {
                    *((UA_UInt16 *)device->variables[var_index].value) = 0;
                }
            } else {
                switch (record->plcDatatype) {
                    case PLC_TYPE_BOOL:
                        device->variables[var_index].value = malloc(sizeof(UA_Boolean));
                        if (device->variables[var_index].value != NULL) {
                            *((UA_Boolean *)device->variables[var_index].value) = false;
                        }
                        break;
                    case PLC_TYPE_USINT:
                        device->variables[var_index].value = malloc(sizeof(UA_Byte));
                        if (device->variables[var_index].value != NULL) {
                            *((UA_Byte *)device->variables[var_index].value) = 0;
                        }
                        break;
                    case PLC_TYPE_UINT:
                        device->variables[var_index].value = malloc(sizeof(UA_UInt16));
                        if (device->variables[var_index].value != NULL) {
                            *((UA_UInt16 *)device->variables[var_index].value) = 0;
                        }
                        break;
                    case PLC_TYPE_ULINT:
                        device->variables[var_index].value = malloc(sizeof(UA_UInt64));
                        if (device->variables[var_index].value != NULL) {
                            *((UA_UInt64 *)device->variables[var_index].value) = 0;
                        }
                        break;
                    case PLC_TYPE_INT:
                        device->variables[var_index].value = malloc(sizeof(UA_Int16));
                        if (device->variables[var_index].value != NULL) {
                            *((UA_Int16 *)device->variables[var_index].value) = 0;
                        }
                        break;
                    case PLC_TYPE_DINT:
                        device->variables[var_index].value = malloc(sizeof(UA_Int32));
                        if (device->variables[var_index].value != NULL) {
                            *((UA_Int32 *)device->variables[var_index].value) = 0;
                        }
                        break;
                    case PLC_TYPE_REAL:
                        device->variables[var_index].value = malloc(sizeof(UA_Float));
                        if (device->variables[var_index].value != NULL) {
                            *((UA_Float *)device->variables[var_index].value) = 0.0f;
                        }
                        break;
                    default:
                        // 为默认情况分配内存，避免NULL值
                        device->variables[var_index].value = malloc(sizeof(UA_UInt16));
                        if (device->variables[var_index].value != NULL) {
                            *((UA_UInt16 *)device->variables[var_index].value) = 0;
                        }
                        break;
                }
            }
            }

            if (!UA_NodeId_isNull(&device->variables[var_index].node_id) &&
                device->variables[var_index].value != NULL) {
                writeCachedValueToNode(server, &device->variables[var_index]);
            }

            var_index++;
        }
    }
    
    log_debug("Device %s added successfully", device->name);
    
    // 释放互斥锁
   // pthread_mutex_unlock(&global_data->var_update_mutex);
    
    return 0;
}

static UA_StatusCode writeCachedValueToNode(UA_Server *server, OPCUAVariable *var) {
    if (!server || !var || !var->value || UA_NodeId_isNull(&var->node_id)) {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    }

    UA_Variant variant;
    UA_Variant_init(&variant);

    switch (var->plc_datatype) {
        case PLC_TYPE_BOOL:
            UA_Variant_setScalar(&variant, (UA_Boolean *)var->value, &UA_TYPES[UA_TYPES_BOOLEAN]);
            break;
        case PLC_TYPE_USINT:
            UA_Variant_setScalar(&variant, (UA_Byte *)var->value, &UA_TYPES[UA_TYPES_BYTE]);
            break;
        case PLC_TYPE_UINT:
            UA_Variant_setScalar(&variant, (UA_UInt16 *)var->value, &UA_TYPES[UA_TYPES_UINT16]);
            break;
        case PLC_TYPE_ULINT:
            UA_Variant_setScalar(&variant, (UA_UInt64 *)var->value, &UA_TYPES[UA_TYPES_UINT64]);
            break;
        case PLC_TYPE_INT:
            UA_Variant_setScalar(&variant, (UA_Int16 *)var->value, &UA_TYPES[UA_TYPES_INT16]);
            break;
        case PLC_TYPE_DINT:
            UA_Variant_setScalar(&variant, (UA_Int32 *)var->value, &UA_TYPES[UA_TYPES_INT32]);
            break;
        case PLC_TYPE_REAL:
            UA_Variant_setScalar(&variant, (UA_Float *)var->value, &UA_TYPES[UA_TYPES_FLOAT]);
            break;
        default:
            return UA_STATUSCODE_BADTYPEMISMATCH;
    }

    return UA_Server_writeValue(server, var->node_id, variant);
}

// 更新OPC UA变量值
void updateOPCUAVariables(UA_Server *server, GlobalData *global_data) {
    if (!server || !global_data) {
        return;
    }
    
    // 申请互斥锁
    //pthread_mutex_lock(&global_data->var_update_mutex);
    
    // 设置标志位，表示这是Modbus更新
    global_data->is_modbus_updating = true;
    
    for (int i = 0; i < global_data->device_count; i++) {
        DeviceNode *device = &global_data->devices[i];
        
        // 只有设备节点存在时才更新变量
        if (!UA_NodeId_isNull(&device->node_id)) {
            for (int j = 0; j < device->variable_count; j++) {
                OPCUAVariable *var = &device->variables[j];
                
                if (!var->value) {
                    continue;
                }
                
                // 只有变量节点存在时才更新
                if (!UA_NodeId_isNull(&var->node_id)) {
                    UA_Variant variant;
                    UA_Variant_init(&variant);
                    
                    // 根据数据类型更新值
                    switch (var->plc_datatype) {
                        case PLC_TYPE_BOOL:
                            UA_Variant_setScalar(&variant, (UA_Boolean *)var->value, &UA_TYPES[UA_TYPES_BOOLEAN]);
                            break;
                        case PLC_TYPE_USINT:
                            UA_Variant_setScalar(&variant, (UA_Byte *)var->value, &UA_TYPES[UA_TYPES_BYTE]);
                            break;
                        case PLC_TYPE_UINT:
                            UA_Variant_setScalar(&variant, (UA_UInt16 *)var->value, &UA_TYPES[UA_TYPES_UINT16]);
                            break;
                        case PLC_TYPE_ULINT:
                            UA_Variant_setScalar(&variant, (UA_UInt64 *)var->value, &UA_TYPES[UA_TYPES_UINT64]);
                            break;
                        case PLC_TYPE_INT:
                            UA_Variant_setScalar(&variant, (UA_Int16 *)var->value, &UA_TYPES[UA_TYPES_INT16]);
                            break;
                        case PLC_TYPE_DINT:
                            UA_Variant_setScalar(&variant, (UA_Int32 *)var->value, &UA_TYPES[UA_TYPES_INT32]);
                            break;
                        case PLC_TYPE_REAL:
                            UA_Variant_setScalar(&variant, (UA_Float *)var->value, &UA_TYPES[UA_TYPES_FLOAT]);
                            break;
                        default:
                            continue;
                    }
                    
                    // 更新OPC UA节点值
                    UA_StatusCode status = UA_Server_writeValue(server, var->node_id, variant);
                    if (status != UA_STATUSCODE_GOOD) {
                        log_warn("Failed to update OPC UA value for %s, modbus_addr=%d, status=0x%08x",
                                 var->name, var->modbus_addr, status);
                    }
                }
            }
        }
    }
    
    // 重置标志位
    global_data->is_modbus_updating = false;
    
    // 释放互斥锁
    //pthread_mutex_unlock(&global_data->var_update_mutex);
}

// Modbus轮询线程
void *modbusPollingThread(void *arg) {
    GlobalData *global_data = (GlobalData *)arg;
    if (!global_data) {
        return NULL;
    }
    
    // 初始化Modbus客户端
    if (modbus_client_init(&global_data->modbus_client, 
                          global_data->modbus_config.server_ip, 
                          global_data->modbus_config.server_port, 
                          1, 1000) != MODBUS_SUCCESS) {
        log_error("Failed to initialize Modbus client");
        return NULL;
    }
    
    int connected = 0;
    
    // 轮询循环
    while (running) {
        // 检查连接状态，如果断开则尝试重连
        if (!connected) {
            if (modbus_client_connect(&global_data->modbus_client) == MODBUS_SUCCESS) {
                log_info("Modbus slave connected successfully");
                connected = 1;
            } else {
                log_warn("Failed to connect to Modbus server, will retry in %d milliseconds", 
                        global_data->modbus_config.reconnect_interval);
                Sleep(global_data->modbus_config.reconnect_interval);
                continue;
            }
        }
        
        // 按寄存器类型分组读取
        bool read_failed = false;
        for (int i = 0; running && i < global_data->modbus_read_plan.block_count; i++) {
            ModbusReadBlock *block = &global_data->modbus_read_plan.blocks[i];
            if (!read_modbus_block(&global_data->modbus_client, block, global_data)) {
                read_failed = true;
                break;
            }
        }

        if (read_failed) {
            connected = 0;
            modbus_client_disconnect(&global_data->modbus_client);
            continue;
        }

#if 0
        uint16_t input_registers[100];
        uint16_t holding_registers[100];
        uint8_t discrete_inputs[100];
        uint8_t coils[100];

		#if 1
        // 读取输入寄存器
        if (modbus_client_read_input_registers(&global_data->modbus_client, MODBUS_PDU_READ_BASE_ADDR, 100, input_registers) == MODBUS_SUCCESS) {
            // 更新相关变量
            //pthread_mutex_lock(&global_data->var_update_mutex);
            for (int i = 0; i < global_data->device_count; i++) {
                DeviceNode *device = &global_data->devices[i];
                for (int j = 0; j < device->variable_count; j++) {
                    OPCUAVariable *var = &device->variables[j];
                    if (var->register_type == REGISTER_TYPE_INPUT_REGISTER && var->value) {
                        int reg_index = modbus_pdu_addr_to_poll_index(
                            modbus_reference_to_pdu_addr(var->register_type, var->modbus_addr));
                        if (reg_index >= 0 && reg_index < 100) {
                            switch (var->plc_datatype) {
                                case PLC_TYPE_USINT:
                                    // 根据Modbus设备的实际存储方式，对于300001地址的数据，USINT存储在整个16位寄存器中
                                    // 这里处理的是特殊情况：某些设备可能将USINT值存储为完整的16位寄存器值
                                    *((UA_Byte *)var->value) = input_registers[reg_index] & 0xFF;
                                    // 如果需要使用整个16位值（如300001地址需要显示345），请启用下面的代码
                                    // *((UA_UInt16 *)var->value) = input_registers[reg_index];
                                    break;
                                case PLC_TYPE_UINT:
                                    *((UA_UInt16 *)var->value) = input_registers[reg_index];
                                    break;
                                case PLC_TYPE_ULINT:
                                    // 合并两个寄存器
                                    if (reg_index + 3 < 100) {
                                        *((UA_UInt64 *)var->value) = ((UA_UInt64)input_registers[reg_index + 3] << 48) | 
                                                                   ((UA_UInt64)input_registers[reg_index + 2] << 32) |
                                                                   ((UA_UInt64)input_registers[reg_index + 1] << 16) |
                                                                   (UA_UInt64)input_registers[reg_index];
                                    }
                                    break;
                                case PLC_TYPE_REAL:
                                    // 转换为浮点数
                                    // Modbus协议通常是小端序，需要交换两个寄存器的顺序
                                    if (reg_index + 1 < 100) {
                                        uint32_t raw = ((uint32_t)input_registers[reg_index + 1] << 16) | input_registers[reg_index];
                                        *((UA_Float *)var->value) = *((float *)&raw);
                                    }
                                    break;
                                default:
                                    break;
                            }
                        }
                    }
                }
            }
            //pthread_mutex_unlock(&global_data->var_update_mutex);
        } else {
            log_error("Failed to read input registers, connection may be lost");
            connected = 0;
            // 清理连接
            modbus_client_disconnect(&global_data->modbus_client);
            continue;
        }
		#endif

		#if 1
        // 读取保持寄存器
        if (modbus_client_read_holding_registers(&global_data->modbus_client, MODBUS_PDU_READ_BASE_ADDR, 100, holding_registers) == MODBUS_SUCCESS) {
            // 更新相关变量
            //pthread_mutex_lock(&global_data->var_update_mutex);
            for (int i = 0; i < global_data->device_count; i++) {
                DeviceNode *device = &global_data->devices[i];
                for (int j = 0; j < device->variable_count; j++) {
                    OPCUAVariable *var = &device->variables[j];
                    if (var->register_type == REGISTER_TYPE_HOLDING_REGISTER && var->value) {
                        int reg_index = modbus_pdu_addr_to_poll_index(
                            modbus_reference_to_pdu_addr(var->register_type, var->modbus_addr));
                        if (reg_index >= 0 && reg_index < 100) {
                            switch (var->plc_datatype) {
                                case PLC_TYPE_USINT:
                                    // USINT是8位无符号整数，取寄存器值的低8位
                                    *((UA_Byte *)var->value) = holding_registers[reg_index] & 0xFF;
                                    break;
                                case PLC_TYPE_UINT:
                                    *((UA_UInt16 *)var->value) = holding_registers[reg_index];
                                    break;
                                case PLC_TYPE_ULINT:
                                    // 合并两个寄存器
                                    if (reg_index + 3 < 100) {
                                        *((UA_UInt64 *)var->value) = ((UA_UInt64)holding_registers[reg_index + 3] << 48) | 
                                                                   ((UA_UInt64)holding_registers[reg_index + 2] << 32) |
                                                                   ((UA_UInt64)holding_registers[reg_index + 1] << 16) |
                                                                   (UA_UInt64)holding_registers[reg_index];
                                    }
                                    break;
                                case PLC_TYPE_REAL:
                                    // 转换为浮点数
                                    // Modbus协议通常是小端序，需要交换两个寄存器的顺序
                                    if (reg_index + 1 < 100) {
                                        uint32_t raw = ((uint32_t)holding_registers[reg_index + 1] << 16) | holding_registers[reg_index];
                                        *((UA_Float *)var->value) = *((float *)&raw);
                                    }
                                    break;
                                default:
                                    break;
                            }
                        }
                    }
                }
            }
            //pthread_mutex_unlock(&global_data->var_update_mutex);
        } else {
            log_error("Failed to read holding registers, connection may be lost");
            connected = 0;
            // 清理连接
            modbus_client_disconnect(&global_data->modbus_client);
            continue;
        }
        #endif
		
        // 读取离散输入
        if (modbus_client_read_discrete_inputs(&global_data->modbus_client, MODBUS_PDU_READ_BASE_ADDR, 100, discrete_inputs) == MODBUS_SUCCESS) {
            // 更新相关变量
            //pthread_mutex_lock(&global_data->var_update_mutex);
            for (int i = 0; i < global_data->device_count; i++) {
                DeviceNode *device = &global_data->devices[i];
                for (int j = 0; j < device->variable_count; j++) {
                    OPCUAVariable *var = &device->variables[j];
                    if (var->register_type == REGISTER_TYPE_DISCRETE_INPUT && var->value) {
                        int bit_index = modbus_pdu_addr_to_poll_index(
                            modbus_reference_to_pdu_addr(var->register_type, var->modbus_addr));
                        if (bit_index >= 0 && bit_index < 100) {
                            int byte_index = bit_index / 8;
                            int bit_pos = bit_index % 8;
                            *((UA_Boolean *)var->value) = (discrete_inputs[byte_index] >> bit_pos) & 0x01;
                        }
                    }
                }
            }
            //pthread_mutex_unlock(&global_data->var_update_mutex);
        } else {
            log_error("Failed to read discrete inputs, connection may be lost");
            connected = 0;
            // 清理连接
            modbus_client_disconnect(&global_data->modbus_client);
            continue;
        }
        
        // 更新OPC UA变量
#endif
        updateOPCUAVariables(global_data->server, global_data);
        
        // 动态管理设备节点
        for (int i = 0; i < global_data->device_count; i++) {
            DeviceNode *device = &global_data->devices[i];
            
            // 跳过已删除的设备
            if (device->name[0] == '\0') {
                continue;
            }
            
            // 查找设备状态变量
            int device_online = 0;
            char status_var_name[150];
            snprintf(status_var_name, sizeof(status_var_name), "%s_status", device->name);
            
            for (int j = 0; j < device->variable_count; j++) {
                OPCUAVariable *var = &device->variables[j];
                // 检查是否为状态变量
                if (strcmp(var->name, status_var_name) == 0) {
                    // 检查状态值
                    if (var->value) {
                        device_online = *((UA_UInt16 *)var->value) == 0;
                    }
                    break;
                }
            }
            
            // 检查设备是否在线
            if (device_online) {
                // 如果设备在线，且设备节点不存在，则添加设备节点
                if (UA_NodeId_isNull(&device->node_id)) {
                    ipv6_manager_delay_before_next_device_if_needed();

                    if (device->ipv6_address[0] != '\0') {
                        log_debug("Reusing IPv6 address %s for device %s",
                                 device->ipv6_address, device->name);
                        ensurePersistentIPv6OnInterface(global_data, device->name, device->ipv6_address);
                        // 设备节点不存在，添加设备节点
                        addDeviceNodes(global_data->server, global_data, i, global_data->nodesNodeId);
                    } else if (ipv6_allocate_address(
                            device->name,
                            global_data->ipv6_multicast.nic_index,
                            device->ipv6_address
                        )) {
                        // 设备节点不存在，添加设备节点
                        addDeviceNodes(global_data->server, global_data, i, global_data->nodesNodeId);
                    } else {
                        log_error("Failed to allocate IPv6 address for device %s", device->name);
                    }
                }
            } else {
                // 如果设备不在线，且设备节点存在，则删除设备节点
                if (!UA_NodeId_isNull(&device->node_id)) {
                    if (device->ipv6_address[0] != '\0') {
                        removePersistentIPv6FromInterface(global_data, device->name, device->ipv6_address);
                    }
                    deleteDeviceNodes(global_data->server, global_data, i);
                }
            }
        }
        
        // 等待下一个轮询周期
        Sleep(global_data->modbus_config.polling_interval);
    }
    
    // 清理资源
    modbus_client_disconnect(&global_data->modbus_client);
    modbus_client_cleanup(&global_data->modbus_client);
    
    return NULL;
}

// 定时打印节点变量信息的线程函数
void* printNodeVariablesThread(void* arg) {
    GlobalData* global_data = (GlobalData*)arg;
    
    while (1) {
        // 每隔10秒执行一次
        Sleep(10000);
        
        log_debug("=== Node Variables Information (updated every 10 seconds) ===");
        
        // 检查全局数据指针有效性
        if (!global_data) {
            log_error("Global data is NULL in printNodeVariablesThread");
            continue;
        }
        
        // 申请互斥锁，保护设备列表访问
        //pthread_mutex_lock(&global_data->var_update_mutex);
        
        // 遍历所有设备
        for (int i = 0; i < global_data->device_count; i++) {
            DeviceNode* device = &global_data->devices[i];
            
            // 检查设备指针有效性
            if (!device) {
                continue;
            }
            
            // 打印设备信息
            log_debug("Device: %s, IPv6 Address: %s", device->name, device->ipv6_address);
            
            // 检查设备变量列表有效性
            if (!device->variables || device->variable_count <= 0) {
                log_debug("  No variables for this device");
                continue;
            }
            
            // 遍历设备的所有变量
            for (int j = 0; j < device->variable_count; j++) {
                OPCUAVariable* var = &device->variables[j];
                
                // 检查变量指针有效性
                if (!var) {
                    continue;
                }
                
                // 根据变量类型打印值，增加变量值指针检查
                if (var->plc_datatype == PLC_TYPE_BOOL) {
                    bool* value = (bool*)var->value;
                    if (value) {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: BOOL, Value: %s", 
                                 var->name, var->ipv6_address, var->modbus_addr, *value ? "TRUE" : "FALSE");
                    } else {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: BOOL, Value: NULL", 
                                 var->name, var->ipv6_address, var->modbus_addr);
                    }
                } else if (var->plc_datatype == PLC_TYPE_USINT) {
                    uint8_t* value = (uint8_t*)var->value;
                    if (value) {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: USINT, Value: %u", 
                                 var->name, var->ipv6_address, var->modbus_addr, *value);
                    } else {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: USINT, Value: NULL", 
                                 var->name, var->ipv6_address, var->modbus_addr);
                    }
                } else if (var->plc_datatype == PLC_TYPE_UINT) {
                    uint16_t* value = (uint16_t*)var->value;
                    if (value) {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: UINT, Value: %u", 
                                 var->name, var->ipv6_address, var->modbus_addr, *value);
                    } else {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: UINT, Value: NULL", 
                                 var->name, var->ipv6_address, var->modbus_addr);
                    }
                } else if (var->plc_datatype == PLC_TYPE_INT) {
                    int16_t* value = (int16_t*)var->value;
                    if (value) {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: INT, Value: %d", 
                                 var->name, var->ipv6_address, var->modbus_addr, *value);
                    } else {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: INT, Value: NULL", 
                                 var->name, var->ipv6_address, var->modbus_addr);
                    }
                } else if (var->plc_datatype == PLC_TYPE_ULINT) {
                    uint64_t* value = (uint64_t*)var->value;
                    if (value) {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: ULINT, Value: %llu", 
                                 var->name, var->ipv6_address, var->modbus_addr, *value);
                    } else {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: ULINT, Value: NULL", 
                                 var->name, var->ipv6_address, var->modbus_addr);
                    }
                } else if (var->plc_datatype == PLC_TYPE_DINT) {
                    int32_t* value = (int32_t*)var->value;
                    if (value) {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: DINT, Value: %d", 
                                 var->name, var->ipv6_address, var->modbus_addr, *value);
                    } else {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: DINT, Value: NULL", 
                                 var->name, var->ipv6_address, var->modbus_addr);
                    }
                } else if (var->plc_datatype == PLC_TYPE_REAL) {
                    float* value = (float*)var->value;
                    if (value) {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: REAL, Value: %.2f", 
                                 var->name, var->ipv6_address, var->modbus_addr, *value);
                    } else {
                        log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: REAL, Value: NULL", 
                                 var->name, var->ipv6_address, var->modbus_addr);
                    }
                } else {
                    log_debug("  Variable: %s, IPv6 Address: %s, Address: %d, Type: Unknown", 
                             var->name, var->ipv6_address, var->modbus_addr);
                }
            }
        }
        
        // 释放互斥锁
        //pthread_mutex_unlock(&global_data->var_update_mutex);
        
        log_debug("===================================");
        
        // 打印设备-IPv6映射关系
        ipv6_print_device_mappings();
    }
    
    return NULL;
}

