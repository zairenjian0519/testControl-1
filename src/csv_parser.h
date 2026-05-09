#ifndef CSV_PARSER_H
#define CSV_PARSER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// PLC数据类型枚举
typedef enum {
    PLC_TYPE_BOOL,
    PLC_TYPE_USINT,
    PLC_TYPE_UINT,
    PLC_TYPE_ULINT,
    PLC_TYPE_INT,
    PLC_TYPE_DINT,
    PLC_TYPE_REAL,
    PLC_TYPE_UNKNOWN
} PLCDatatype;

// Modbus寄存器类型枚举
typedef enum {
    REGISTER_TYPE_DISCRETE_INPUT,
    REGISTER_TYPE_COIL,
    REGISTER_TYPE_INPUT_REGISTER,
    REGISTER_TYPE_HOLDING_REGISTER,
    REGISTER_TYPE_UNKNOWN
} RegisterType;

// CSV记录结构体
typedef struct {
    int modbusAddr;
    char name[100];
    RegisterType registerType;
    PLCDatatype plcDatatype;
    char desc[100];
    char deviceName[50];
    char nodeName[100];
} CSVRecord;

// CSV解析结果结构体
typedef struct {
    CSVRecord *records;
    int count;
} CSVParseResult;

// 函数声明
CSVParseResult parseCSV(const char *filename);
void freeCSVResult(CSVParseResult *result);
PLCDatatype getPLCDatatype(const char *str);
RegisterType getRegisterType(const char *str);

#endif // CSV_PARSER_H