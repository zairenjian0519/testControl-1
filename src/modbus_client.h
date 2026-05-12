#ifndef MODBUS_CLIENT_H
#define MODBUS_CLIENT_H

#define _WIN32_WINNT 0x0600   // 必须放在最前面！
#define FD_SETSIZE 1024       // 把上限从 64 改大！

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "modbus.h"

#pragma comment(lib, "ws2_32.lib")

// 使用libmodbus的功能码
// Modbus错误码
#define MODBUS_SUCCESS                      0x00
#define MODBUS_ERROR_INIT                   0x01
#define MODBUS_ERROR_CONNECT                0x02
#define MODBUS_ERROR_READ                   0x03
#define MODBUS_ERROR_WRITE                  0x04

// Modbus客户端上下文
typedef struct {
    modbus_t *ctx;
    char server_ip[16];
    int server_port;
    int slave_id;
    int timeout_ms;
} ModbusClient;

// 函数声明
int modbus_client_init(ModbusClient *client, const char *server_ip, int server_port, int slave_id, int timeout_ms);
int modbus_client_connect(ModbusClient *client);
int modbus_client_disconnect(ModbusClient *client);
int modbus_client_read_coils(ModbusClient *client, int addr, int count, uint8_t *dest);
int modbus_client_read_discrete_inputs(ModbusClient *client, int addr, int count, uint8_t *dest);
int modbus_client_read_holding_registers(ModbusClient *client, int addr, int count, uint16_t *dest);
int modbus_client_read_input_registers(ModbusClient *client, int addr, int count, uint16_t *dest);
int modbus_client_write_single_coil(ModbusClient *client, int addr, int value);
int modbus_client_write_single_register(ModbusClient *client, int addr, int value);
void modbus_client_cleanup(ModbusClient *client);

#endif // MODBUS_CLIENT_H