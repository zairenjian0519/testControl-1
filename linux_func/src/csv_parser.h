#ifndef CSV_PARSER_H
#define CSV_PARSER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

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

typedef enum {
    REGISTER_TYPE_DISCRETE_INPUT,
    REGISTER_TYPE_COIL,
    REGISTER_TYPE_INPUT_REGISTER,
    REGISTER_TYPE_HOLDING_REGISTER,
    REGISTER_TYPE_UNKNOWN
} RegisterType;

typedef struct {
    int modbusAddr;
    char name[100];
    RegisterType registerType;
    PLCDatatype plcDatatype;
    char desc[100];
    char deviceName[50];
    char nodeName[100];
} CSVRecord;

typedef struct {
    CSVRecord *records;
    int count;
} CSVParseResult;

CSVParseResult parseCSV(const char *filename);
void freeCSVResult(CSVParseResult *result);
PLCDatatype getPLCDatatype(const char *str);
RegisterType getRegisterType(const char *str);

#ifdef __cplusplus
}
#endif

#endif
