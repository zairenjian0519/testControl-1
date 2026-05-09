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

#include "open62541.h"
#include "csv_parser.h"
#include "modbus_client.h"
#include "cJSON.h"

// 设备配置结构体
typedef struct {
    char server_ip[16];
    int server_port;
    int polling_interval;
} ModbusConfig;

// IPv6地址池结构体
typedef struct {
    char start_address[40];
    char end_address[40];
    int prefix_length;
    int max_lease_count;
} IPv6AddressPool;

// OPC UA变量结构体
typedef struct {
    UA_NodeId node_id;
    int modbus_addr;
    RegisterType register_type;
    PLCDatatype plc_datatype;
    char name[100];
    void *value;
} OPCUAVariable;

// 设备节点结构体
typedef struct {
    UA_NodeId node_id;
    char name[50];
    OPCUAVariable *variables;
    int variable_count;
} DeviceNode;

// 全局数据
typedef struct {
    ModbusConfig modbus_config;
    IPv6AddressPool ipv6_pool;
    CSVParseResult csv_result;
    DeviceNode *devices;
    int device_count;
    ModbusClient modbus_client;
    UA_Server *server;
} GlobalData;

// 函数声明
UA_Guid ipv6ToGuid(const char *ipv6Addr);
UA_NodeId createNodeWithIPv6Id(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId);
UA_NodeId createObjectNode(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId);
UA_NodeId createVariableNode(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId, UA_DataType *dataType, UA_Variant *value, UA_Byte accessLevel);

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

UA_Boolean running = true;

void stopHandler(int sign) {
    UA_LOG_INFO(UA_Log_Stdout, UA_LOGCATEGORY_SERVER, "received ctrl-c");
    running = false;
}

// IPv6地址转GUID函数
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
    UA_Guid guid = ipv6ToGuid(ipv6Addr);
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
                             UA_DataType *dataType, UA_Variant *value, UA_Byte accessLevel) {
    UA_Guid guid = ipv6ToGuid(ipv6Addr);
    UA_NodeId nodeId = UA_NODEID_GUID(1, guid);
    
    UA_VariableAttributes varAttr = UA_VariableAttributes_default;
    varAttr.displayName = UA_LOCALIZEDTEXT((char*)"en-US", (char*)name);
    varAttr.dataType = dataType->typeId;
    varAttr.accessLevel = accessLevel;
    varAttr.value = *value;
    
    UA_QualifiedName browseName = UA_QUALIFIEDNAME(1, (char*)name);
    UA_NodeId referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES);
    
    UA_NodeId outNodeId;
    UA_Server_addVariableNode(server, nodeId, parentNodeId, referenceTypeId, browseName, 
                             UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), varAttr, NULL, &outNodeId);
    
    return outNodeId;
}

// 创建spssps设备节点及其子节点
UA_NodeId createSpsspsDevice(UA_Server *server) {
    // 创建spssps设备节点
    UA_NodeId spsspsNodeId = createObjectNode(server, (char*)"2001:eaca:101:0:001E:CD00:0100:0000", (char*)"spssps", 
                                             UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER));
    
    // 创建DeviceInfo属性
    UA_Variant deviceInfoValue;
    UA_Variant_init(&deviceInfoValue);
    UA_String deviceInfoString = UA_STRING((char*)"Device Information");
    UA_Variant_setScalar(&deviceInfoValue, &deviceInfoString, &UA_TYPES[UA_TYPES_STRING]);
    createVariableNode(server, (char*)"2001:eaca:101:0:001E:CD00:0100:0001", (char*)"DeviceInfo", spsspsNodeId, 
                      &UA_TYPES[UA_TYPES_STRING], &deviceInfoValue, UA_ACCESSLEVELMASK_READ);
    
    // 创建Buses对象
    UA_NodeId busesNodeId = createObjectNode(server, (char*)"2001:eaca:101:0:001E:CD00:0100:0002", (char*)"Buses", spsspsNodeId);
    
    return spsspsNodeId;
}

// 创建Bus 0对象及其子节点
UA_NodeId createBus0(UA_Server *server, UA_NodeId busesNodeId) {
    // 创建Bus 0对象
    UA_NodeId bus0NodeId = createObjectNode(server, (char*)"2001:eaca:101:0:001E:CD00:0200:0000", (char*)"Bus 0", busesNodeId);
    
    // 创建BusStatus变量
    UA_Variant busStatusValue;
    UA_Variant_init(&busStatusValue);
    UA_Int32 status = 0;
    UA_Variant_setScalar(&busStatusValue, &status, &UA_TYPES[UA_TYPES_INT32]);
    createVariableNode(server, (char*)"2001:eaca:101:0:001E:CD00:0200:0001", (char*)"BusStatus", bus0NodeId, 
                      &UA_TYPES[UA_TYPES_INT32], &busStatusValue, UA_ACCESSLEVELMASK_READ);
    
    // 创建Nodes对象
    UA_NodeId nodesNodeId = createObjectNode(server, (char*)"2001:eaca:101:0:001E:CD00:0200:0002", (char*)"Nodes", bus0NodeId);
    
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
                      &UA_TYPES[UA_TYPES_DOUBLE], &temp1Value, UA_ACCESSLEVELMASK_READ);
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


int opcua_server_main(void) 
{
    signal(SIGINT, stopHandler);
    signal(SIGTERM, stopHandler);

    UA_Server *server = UA_Server_new();
    UA_ServerConfig_setDefault(UA_Server_getConfig(server));
    
    // 创建spssps设备节点及其子节点
    UA_NodeId spsspsNodeId = createSpsspsDevice(server);
    
    // 获取Buses节点ID
    UA_NodeId busesNodeId = UA_NODEID_GUID(1, ipv6ToGuid((char*)"2001:eaca:101:0:001E:CD00:0100:0002"));
    
    // 创建Bus 0对象及其子节点
    UA_NodeId bus0NodeId = createBus0(server, busesNodeId);
    
    // 获取Nodes节点ID
    UA_NodeId nodesNodeId = UA_NODEID_GUID(1, ipv6ToGuid((char*)"2001:eaca:101:0:001E:CD00:0200:0002"));
    
    // 初始化全局数据
    GlobalData global_data;
    memset(&global_data, 0, sizeof(global_data));
    global_data.server = server;
    
    // 加载配置文件
    if (loadConfig(&global_data) != 0) {
        fprintf(stderr, "Failed to load configuration file\n");
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }
    
    // 解析CSV文件 - 尝试多种路径
    CSVParseResult csv_result = parseCSV("./uploadtable.csv");
    if (csv_result.count == 0) {
        csv_result = parseCSV("../uploadtable.csv");
        if (csv_result.count == 0) {
            csv_result = parseCSV("../../uploadtable.csv");
            if (csv_result.count == 0) {
                fprintf(stderr, "Failed to parse CSV file or no data\n");
                UA_Server_delete(server);
                return EXIT_FAILURE;
            }
        }
    }
    global_data.csv_result = csv_result;
    
    fprintf(stderr, "Debug: CSV file parsed, total %d records\n", csv_result.count);
    
    // 创建设备节点和变量节点
    fprintf(stderr, "Debug: Creating device nodes start\n");
    if (createDeviceNodes(server, &global_data, nodesNodeId) != 0) {
        fprintf(stderr, "Failed to create device nodes\n");
        freeCSVResult(&global_data.csv_result);
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Debug: Creating device nodes end\n");

    // 启动Modbus轮询线程
    fprintf(stderr, "Debug: Creating Modbus polling thread start\n");
    pthread_t modbus_thread;
    if (pthread_create(&modbus_thread, NULL, modbusPollingThread, (void *)&global_data) != 0) {
        fprintf(stderr, "Failed to create Modbus polling thread\n");
        freeCSVResult(&global_data.csv_result);
        UA_Server_delete(server);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Debug: Modbus polling thread created successfully\n");
	
    
    // 运行OPC UA服务器
    fprintf(stderr, "Debug: Starting OPC UA server\n");
    UA_StatusCode retval = UA_Server_run(server, &running);
    fprintf(stderr, "Debug: OPC UA server stopped, status code: %d\n", retval);
    
    // 清理资源
    pthread_join(modbus_thread, NULL);
	
    
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
    
    // 打开配置文件 - 尝试多种路径
    FILE *file = fopen("./config.json", "r");
    if (!file) {
        file = fopen("../config.json", "r");
        if (!file) {
            file = fopen("../../config.json", "r");
            if (!file) {
                fprintf(stderr, "Cannot open configuration file: config.json\n");
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
        fprintf(stderr, "JSON parsing error\n");
        return -1;
    }
    
    // 解析modbus_tcp配置
    cJSON *modbus_tcp = cJSON_GetObjectItem(root, "modbus_tcp");
    if (modbus_tcp) {
        cJSON *server_ip = cJSON_GetObjectItem(modbus_tcp, "server_ip");
        if (server_ip && cJSON_IsString(server_ip)) {
            strncpy(global_data->modbus_config.server_ip, server_ip->valuestring, sizeof(global_data->modbus_config.server_ip) - 1);
        }
        
        cJSON *server_port = cJSON_GetObjectItem(modbus_tcp, "server_port");
        if (server_port && cJSON_IsNumber(server_port)) {
            global_data->modbus_config.server_port = server_port->valueint;
        }
        
        cJSON *polling_interval = cJSON_GetObjectItem(modbus_tcp, "polling_interval");
        if (polling_interval && cJSON_IsNumber(polling_interval)) {
            global_data->modbus_config.polling_interval = polling_interval->valueint;
        }
    }
    
    // 解析ipv6_address_pool配置
    cJSON *ipv6_pool = cJSON_GetObjectItem(root, "ipv6_address_pool");
    if (ipv6_pool) {
        cJSON *start_address = cJSON_GetObjectItem(ipv6_pool, "start_address");
        if (start_address && cJSON_IsString(start_address)) {
            strncpy(global_data->ipv6_pool.start_address, start_address->valuestring, sizeof(global_data->ipv6_pool.start_address) - 1);
        }
        
        cJSON *end_address = cJSON_GetObjectItem(ipv6_pool, "end_address");
        if (end_address && cJSON_IsString(end_address)) {
            strncpy(global_data->ipv6_pool.end_address, end_address->valuestring, sizeof(global_data->ipv6_pool.end_address) - 1);
        }
        
        cJSON *prefix_length = cJSON_GetObjectItem(ipv6_pool, "prefix_length");
        if (prefix_length && cJSON_IsNumber(prefix_length)) {
            global_data->ipv6_pool.prefix_length = prefix_length->valueint;
        }
        
        cJSON *max_lease_count = cJSON_GetObjectItem(ipv6_pool, "max_lease_count");
        if (max_lease_count && cJSON_IsNumber(max_lease_count)) {
            global_data->ipv6_pool.max_lease_count = max_lease_count->valueint;
        }
    }
    
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

// 创建设备节点和变量节点
int createDeviceNodes(UA_Server *server, GlobalData *global_data, UA_NodeId nodesNodeId) {
    if (!server || !global_data) {
        return -1;
    }
    
    // 跳过解析起始IPv6地址，直接使用硬编码格式生成节点ID
    // 后续可以根据需要重新实现IPv6地址解析功能
    
    // 统计设备数量
    int device_count = 0;
    char current_device[50] = "";
    
    fprintf(stderr, "Debug: Starting to count devices\n");
    for (int i = 0; i < global_data->csv_result.count; i++) {
        CSVRecord *record = &global_data->csv_result.records[i];
        if (strcmp(record->deviceName, current_device) != 0) {
            device_count++;
            strcpy(current_device, record->deviceName);
            fprintf(stderr, "Debug: Found device %s, total %d devices\n", current_device, device_count);
        }
    }
    
    fprintf(stderr, "Debug: Device count completed, total %d devices\n", device_count);
    
    // 分配设备节点内存
    fprintf(stderr, "Debug: Starting to allocate memory for device nodes, size %d\n", sizeof(DeviceNode) * device_count);
    global_data->devices = (DeviceNode *)malloc(sizeof(DeviceNode) * device_count);
    if (!global_data->devices) {
        fprintf(stderr, "Debug: Failed to allocate memory for device nodes\n");
        return -1;
    }
    fprintf(stderr, "Debug: Memory allocated for device nodes successfully\n");
    
    global_data->device_count = device_count;
    
    // 创建设备节点
    int device_index = 0;
    int ipv6_index = 0;
    
    fprintf(stderr, "Debug: Starting to create device nodes\n");
    for (int i = 0; i < global_data->csv_result.count; i++) {
        CSVRecord *record = &global_data->csv_result.records[i];
        
        // 跳过设备名称为空的记录
        if (record->deviceName[0] == '\0') {
            continue;
        }
        
        // 检查是否是新设备
        if (device_index == 0 || strcmp(record->deviceName, global_data->devices[device_index - 1].name) != 0) {
            // 创建设备节点
            char ipv6_addr[40];
            sprintf(ipv6_addr, "2001:eaca:101:0:001E:CD00:0201:%04x", ipv6_index);
            
            fprintf(stderr, "Debug: Creating device %s, IPv6 address %s\n", record->deviceName, ipv6_addr);
            
            global_data->devices[device_index].node_id = createObjectNode(server, ipv6_addr, record->deviceName, nodesNodeId);
            strcpy(global_data->devices[device_index].name, record->deviceName);
            global_data->devices[device_index].variable_count = 0;
            global_data->devices[device_index].variables = NULL;
            
            // 为设备节点添加Byte类型的状态变量（只读）
            char status_ipv6_addr[40];
            sprintf(status_ipv6_addr, "2001:eaca:101:0:001E:CD00:0201:%04x", ipv6_index++);
            UA_Variant status_value;
            UA_Variant_init(&status_value);
            UA_Byte status_val = 0;
            UA_Variant_setScalar(&status_value, &status_val, &UA_TYPES[UA_TYPES_BYTE]);
            createVariableNode(server, status_ipv6_addr, "Status", global_data->devices[device_index].node_id, 
                              &UA_TYPES[UA_TYPES_BYTE], &status_value, UA_ACCESSLEVELMASK_READ);
            
            device_index++;
            ipv6_index++;
        }
    }
    fprintf(stderr, "Debug: Device nodes created successfully\n");
    
    // 为每个设备创建变量
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
        
        // 创建变量节点
        int var_index = 0;
        for (int j = 0; j < global_data->csv_result.count; j++) {
            CSVRecord *record = &global_data->csv_result.records[j];
            if (strcmp(record->deviceName, device->name) == 0) {
                // 生成IPv6地址
                char ipv6_addr[40];
                sprintf(ipv6_addr, "2001:eaca:101:0:001E:CD00:0201:%04x", ipv6_index++);
                
                // 获取OPC UA类型和访问级别
                UA_DataType *opcua_type;
                // 如果desc为undefined，数据类型为UInt16
                if (strcmp(record->desc, "undefined") == 0) {
                    opcua_type = &UA_TYPES[UA_TYPES_UINT16];
                } else {
                    opcua_type = getOPCUAType(record->plcDatatype);
                }
                UA_Byte access_level = getAccessLevel(record->registerType);
                
                // 直接创建UA_Variant对象（不使用动态分配）
                UA_Variant default_value;
                UA_Variant_init(&default_value);
                
                // 设置默认值
                if (strcmp(record->desc, "undefined") == 0) {
                    // 如果desc为undefined，使用UInt16类型的默认值
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
                
                // 创建变量节点
                device->variables[var_index].node_id = createVariableNode(
                    server, ipv6_addr, record->nodeName, device->node_id,
                    opcua_type, &default_value, access_level);
                
                // 设置变量属性
                device->variables[var_index].modbus_addr = record->modbusAddr;
                device->variables[var_index].register_type = record->registerType;
                device->variables[var_index].plc_datatype = record->plcDatatype;
                strcpy(device->variables[var_index].name, record->nodeName);
                
                // 保存值指针
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
                
                // 清除UA_Variant对象
                //UA_Variant_clear(&default_value);
                
                var_index++;
            }
        }
    }

	 fprintf(stderr, "Debug: createDeviceNodes end\n");
	
    return 0;
}

// 更新OPC UA变量值
void updateOPCUAVariables(UA_Server *server, GlobalData *global_data) {
    if (!server || !global_data) {
        return;
    }
    
    for (int i = 0; i < global_data->device_count; i++) {
        DeviceNode *device = &global_data->devices[i];
        
        for (int j = 0; j < device->variable_count; j++) {
            OPCUAVariable *var = &device->variables[j];
            
            if (!var->value) {
                continue;
            }
            
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
            UA_Server_writeValue(server, var->node_id, variant);
        }
    }
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
        fprintf(stderr, "Failed to initialize Modbus client\n");
        return NULL;
    }
    
    // 连接到Modbus服务器
    if (modbus_client_connect(&global_data->modbus_client) != MODBUS_SUCCESS) {
        fprintf(stderr, "Failed to connect to Modbus server\n");
        modbus_client_cleanup(&global_data->modbus_client);
        return NULL;
    }
    
    fprintf(stdout, "Modbus客户端连接成功\n");
    
    // 轮询循环
    while (running) {
        // 按寄存器类型分组读取
        uint16_t input_registers[100];
        uint16_t holding_registers[100];
        uint8_t discrete_inputs[100];
        uint8_t coils[100];
        
        // 读取输入寄存器
        if (modbus_client_read_input_registers(&global_data->modbus_client, 0, 100, input_registers) == MODBUS_SUCCESS) {
            // 更新相关变量
            for (int i = 0; i < global_data->device_count; i++) {
                DeviceNode *device = &global_data->devices[i];
                for (int j = 0; j < device->variable_count; j++) {
                    OPCUAVariable *var = &device->variables[j];
                    if (var->register_type == REGISTER_TYPE_INPUT_REGISTER && var->value) {
                        int reg_index = var->modbus_addr - 300001;
                        if (reg_index >= 0 && reg_index < 100) {
                            switch (var->plc_datatype) {
                                case PLC_TYPE_UINT:
                                    *((UA_UInt16 *)var->value) = input_registers[reg_index];
                                    break;
                                case PLC_TYPE_ULINT:
                                    // 合并两个寄存器
                                    if (reg_index + 1 < 100) {
                                        *((UA_UInt64 *)var->value) = ((UA_UInt64)input_registers[reg_index] << 48) | 
                                                                   ((UA_UInt64)input_registers[reg_index + 1] << 32) |
                                                                   ((UA_UInt64)input_registers[reg_index + 2] << 16) |
                                                                   (UA_UInt64)input_registers[reg_index + 3];
                                    }
                                    break;
                                case PLC_TYPE_REAL:
                                    // 转换为浮点数
                                    if (reg_index + 1 < 100) {
                                        uint32_t raw = ((uint32_t)input_registers[reg_index] << 16) | input_registers[reg_index + 1];
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
        }
        
        // 读取保持寄存器
        if (modbus_client_read_holding_registers(&global_data->modbus_client, 0, 100, holding_registers) == MODBUS_SUCCESS) {
            // 更新相关变量
            for (int i = 0; i < global_data->device_count; i++) {
                DeviceNode *device = &global_data->devices[i];
                for (int j = 0; j < device->variable_count; j++) {
                    OPCUAVariable *var = &device->variables[j];
                    if (var->register_type == REGISTER_TYPE_HOLDING_REGISTER && var->value) {
                        int reg_index = var->modbus_addr - 400001;
                        if (reg_index >= 0 && reg_index < 100) {
                            switch (var->plc_datatype) {
                                case PLC_TYPE_UINT:
                                    *((UA_UInt16 *)var->value) = holding_registers[reg_index];
                                    break;
                                case PLC_TYPE_ULINT:
                                    // 合并两个寄存器
                                    if (reg_index + 1 < 100) {
                                        *((UA_UInt64 *)var->value) = ((UA_UInt64)holding_registers[reg_index] << 48) | 
                                                                   ((UA_UInt64)holding_registers[reg_index + 1] << 32) |
                                                                   ((UA_UInt64)holding_registers[reg_index + 2] << 16) |
                                                                   (UA_UInt64)holding_registers[reg_index + 3];
                                    }
                                    break;
                                case PLC_TYPE_REAL:
                                    // 转换为浮点数
                                    if (reg_index + 1 < 100) {
                                        uint32_t raw = ((uint32_t)holding_registers[reg_index] << 16) | holding_registers[reg_index + 1];
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
        }
        
        // 读取离散输入
        if (modbus_client_read_discrete_inputs(&global_data->modbus_client, 0, 100, discrete_inputs) == MODBUS_SUCCESS) {
            // 更新相关变量
            for (int i = 0; i < global_data->device_count; i++) {
                DeviceNode *device = &global_data->devices[i];
                for (int j = 0; j < device->variable_count; j++) {
                    OPCUAVariable *var = &device->variables[j];
                    if (var->register_type == REGISTER_TYPE_DISCRETE_INPUT && var->value) {
                        int bit_index = var->modbus_addr - 100001;
                        if (bit_index >= 0 && bit_index < 100) {
                            int byte_index = bit_index / 8;
                            int bit_pos = bit_index % 8;
                            *((UA_Boolean *)var->value) = (discrete_inputs[byte_index] >> bit_pos) & 0x01;
                        }
                    }
                }
            }
        }
        
        // 更新OPC UA变量
        updateOPCUAVariables(global_data->server, global_data);
        
        // 等待下一个轮询周期
        Sleep(global_data->modbus_config.polling_interval);
    }
    
    // 清理资源
    modbus_client_disconnect(&global_data->modbus_client);
    modbus_client_cleanup(&global_data->modbus_client);
    
    return NULL;
}

