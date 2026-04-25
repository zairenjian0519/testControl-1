// server.c
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>

#include "open62541.h"

// 函数声明
UA_Guid ipv6ToGuid(const char *ipv6Addr);
UA_NodeId createNodeWithIPv6Id(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId);
UA_NodeId createObjectNode(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId);
UA_NodeId createVariableNode(UA_Server *server, const char *ipv6Addr, const char *name, UA_NodeId parentNodeId, UA_DataType *dataType, UA_Variant *value, UA_Byte accessLevel);

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
    createVariableNode(server, (char*)"2001:eaca:101:0:001E:CD00:0201:0001", (char*)"温度1", mnNodeId, 
                      &UA_TYPES[UA_TYPES_DOUBLE], &temp1Value, UA_ACCESSLEVELMASK_READ);
    
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
    
    // 创建控制1变量（带数据源）
    UA_VariableAttributes control1Attr = UA_VariableAttributes_default;
    control1Attr.displayName = UA_LOCALIZEDTEXT((char*)"en-US", (char*)"控制1");
    control1Attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
    control1Attr.dataType = UA_TYPES[UA_TYPES_DOUBLE].typeId;
    
    UA_Guid control1Guid = ipv6ToGuid((char*)"2001:eaca:101:0:001E:CD00:0201:0005");
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
    
    // 创建MN主节点及其温度、湿度变量
    UA_NodeId mnNodeId = createMNNode(server, nodesNodeId);
    
    UA_StatusCode retval = UA_Server_run(server, &running);
    
    UA_Server_delete(server);
    
    return retval == UA_STATUSCODE_GOOD ? EXIT_SUCCESS : EXIT_FAILURE;
}

