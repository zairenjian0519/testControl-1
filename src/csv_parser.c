#include "csv_parser.h"

// 解析一行CSV数据
static int parseCSVLine(char *line, CSVRecord *record, char *lastValidName) {
    if (!line || !record) {
        return 0;
    }

    memset(record, 0, sizeof(*record));

    char *token;
    char *saveptr;
    int fieldIndex = 0;
    
    // 解析modbusAddr
    token = strtok_r(line, ",", &saveptr);
    if (!token) return 0;
    
    // 将厂家扩展的6位地址转换为标准5位地址
    // 30xxxx -> 3xxxx, 10xxxx -> 1xxxx, 20xxxx -> 2xxxx, 40xxxx -> 4xxxx
    int addr = atoi(token);
    if (addr >= 100000 && addr <= 499999) {
        // 对于6位地址，去掉中间的0，保留前5位有效数字
        // 例如：300001 -> 30001, 100002 -> 10002, 400003 -> 40003
        int prefix = addr / 100000; // 获取第一位数字 (1,2,3,4)
        int suffix = addr % 10000;  // 获取最后4位数字
        addr = prefix * 10000 + suffix;
    }
    record->modbusAddr = addr;
    fieldIndex++;
    
    // 解析name
    token = strtok_r(NULL, ",", &saveptr);
    if (!token) return 0;
    strncpy(record->name, token, sizeof(record->name) - 1);
    record->name[sizeof(record->name) - 1] = '\0';
    fieldIndex++;
    
    // 处理null.null情况
    if (strcmp(record->name, "null.null") == 0) {
        if (lastValidName) {
            strncpy(record->name, lastValidName, sizeof(record->name) - 1);
            record->name[sizeof(record->name) - 1] = '\0';
        }
    } else {
        // 更新最后一个有效的name
        if (lastValidName) {
            strncpy(lastValidName, record->name, 99);
            lastValidName[99] = '\0';
        }
    }
    
    // 解析registerType
    token = strtok_r(NULL, ",", &saveptr);
    if (!token) return 0;
    record->registerType = getRegisterType(token);
    fieldIndex++;
    
    // 解析datatype
    token = strtok_r(NULL, ",", &saveptr);
    if (!token) return 0;
    record->plcDatatype = getPLCDatatype(token);
    fieldIndex++;
    
    // 解析desc
    token = strtok_r(NULL, ",", &saveptr);
    if (!token) return 0;
    // 记录原始desc值
    int isDescUndefined = (strcmp(token, "undefined") == 0);
    // 如果desc为undefined，用status代替
    if (isDescUndefined) {
        strncpy(record->desc, "status", sizeof(record->desc) - 1);
    } else {
        strncpy(record->desc, token, sizeof(record->desc) - 1);
    }
    record->desc[sizeof(record->desc) - 1] = '\0';
    fieldIndex++;
    
    // 提取设备名称（name的前两个字段，通过_分隔）
    char *dot = strchr(record->name, '.');
    if (dot) {
        // 复制第一个点之前的内容
        char temp_name[200];
        size_t len = (size_t)(dot - record->name);
        if (len >= sizeof(temp_name)) {
            len = sizeof(temp_name) - 1;
        }
        memcpy(temp_name, record->name, len);
        temp_name[len] = '\0';
        
        // 提取第一个_分隔的字段作为设备名称
        char *first_underscore = strchr(temp_name, '_');
        if (first_underscore) {
            size_t device_len = (size_t)(first_underscore - temp_name);
            if (device_len >= sizeof(record->deviceName)) {
                device_len = sizeof(record->deviceName) - 1;
            }
            memcpy(record->deviceName, temp_name, device_len);
            record->deviceName[device_len] = '\0';
        } else {
            strncpy(record->deviceName, temp_name, sizeof(record->deviceName) - 1);
            record->deviceName[sizeof(record->deviceName) - 1] = '\0';
        }
        
        // 构建节点名称
        // 如果原始desc为undefined，用设备名称 + "_status"代替
        if (isDescUndefined) {
            snprintf(record->nodeName, sizeof(record->nodeName), "%s_status", record->deviceName);
        } else {
            // 对于非undefined的desc值，使用name的前两个_分隔的字段 + desc
            char *first_underscore = strchr(temp_name, '_');
            if (first_underscore) {
                char *second_underscore = strchr(first_underscore + 1, '_');
                if (second_underscore) {
                    size_t name_len = (size_t)(second_underscore - temp_name);
                    char temp_name_prefix[200];
                    if (name_len >= sizeof(temp_name_prefix)) {
                        name_len = sizeof(temp_name_prefix) - 1;
                    }
                    memcpy(temp_name_prefix, temp_name, name_len);
                    temp_name_prefix[name_len] = '\0';
                    snprintf(record->nodeName, sizeof(record->nodeName), "%s_%s", temp_name_prefix, record->desc);
                } else {
                    // 如果没有第二个下划线，使用整个temp_name
                    snprintf(record->nodeName, sizeof(record->nodeName), "%s_%s", temp_name, record->desc);
                }
            } else {
                // 如果没有下划线，使用设备名称
                snprintf(record->nodeName, sizeof(record->nodeName), "%s_%s", record->deviceName, record->desc);
            }
        }
    }
    
    if (record->deviceName[0] == '\0' || record->nodeName[0] == '\0') {
        return 0;
    }

    return fieldIndex == 5;
}

// 解析CSV文件
CSVParseResult parseCSV(const char *filename) {
    CSVParseResult result;
    result.records = NULL;
    result.count = 0;
    
    FILE *file = fopen(filename, "r");
    if (!file) {
        return result;
    }
    
    // 计算文件行数（不包括表头）
    char buffer[1024];
    int lineCount = 0;
    
    // 跳过表头
    if (fgets(buffer, sizeof(buffer), file) == NULL) {
        fclose(file);
        return result;
    }
    
    while (fgets(buffer, sizeof(buffer), file) != NULL) {
        lineCount++;
    }
    
    // 分配内存
    result.records = (CSVRecord *)malloc(sizeof(CSVRecord) * lineCount);
    if (!result.records) {
        fclose(file);
        return result;
    }
    
    // 重置文件指针
    fseek(file, 0, SEEK_SET);
    
    // 再次跳过表头
    fgets(buffer, sizeof(buffer), file);
    
    // 解析每一行
    char lastValidName[100] = "";
    int i = 0;
    while (fgets(buffer, sizeof(buffer), file) != NULL) {
        // 去除换行符
        buffer[strcspn(buffer, "\n\r")] = '\0';
        
        if (parseCSVLine(buffer, &result.records[i], lastValidName)) {
            result.count++;
            i++;
        }
    }
    
    fclose(file);
    
    return result;
}

// 释放CSV解析结果
void freeCSVResult(CSVParseResult *result) {
    if (result && result->records) {
        free(result->records);
        result->records = NULL;
        result->count = 0;
    }
}

// 将字符串转换为PLC数据类型
PLCDatatype getPLCDatatype(const char *str) {
    if (strcmp(str, "BOOL") == 0) return PLC_TYPE_BOOL;
    if (strcmp(str, "USINT") == 0) return PLC_TYPE_USINT;
    if (strcmp(str, "UINT") == 0) return PLC_TYPE_UINT;
    if (strcmp(str, "ULINT") == 0) return PLC_TYPE_ULINT;
    if (strcmp(str, "INT") == 0) return PLC_TYPE_INT;
    if (strcmp(str, "DINT") == 0) return PLC_TYPE_DINT;
    if (strcmp(str, "REAL") == 0) return PLC_TYPE_REAL;
    return PLC_TYPE_UNKNOWN;
}

// 将字符串转换为寄存器类型
RegisterType getRegisterType(const char *str) {
    if (strcmp(str, "Discretes Input") == 0) return REGISTER_TYPE_DISCRETE_INPUT;
    if (strcmp(str, "Coils") == 0) return REGISTER_TYPE_COIL;
    if (strcmp(str, "Input Registers") == 0) return REGISTER_TYPE_INPUT_REGISTER;
    if (strcmp(str, "Holding Registers") == 0) return REGISTER_TYPE_HOLDING_REGISTER;
    return REGISTER_TYPE_UNKNOWN;
}
