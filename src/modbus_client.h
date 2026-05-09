#ifndef MODBUS_CLIENT_H
#define MODBUS_CLIENT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

// Modbus功能码
#define MODBUS_FC_READ_COILS                0x01
#define MODBUS_FC_READ_DISCRETE_INPUTS      0x02
#define MODBUS_FC_READ_HOLDING_REGISTERS    0x03
#define MODBUS_FC_READ_INPUT_REGISTERS      0x04
#define MODBUS_FC_WRITE_SINGLE_COIL         0x05
#define MODBUS_FC_WRITE_SINGLE_REGISTER     0x06
#define MODBUS_FC_WRITE_MULTIPLE_COILS      0x0F
#define MODBUS_FC_WRITE_MULTIPLE_REGISTERS  0x10

// Modbus错误码
#define MODBUS_SUCCESS                      0x00
#define MODBUS_ERROR_SOCKET                 0x01
#define MODBUS_ERROR_CONNECT                0x02
#define MODBUS_ERROR_SEND                   0x03
#define MODBUS_ERROR_RECV                   0x04
#define MODBUS_ERROR_CRC                    0x05
#define MODBUS_ERROR_RESPONSE               0x06

// Modbus客户端上下文
typedef struct {
    SOCKET sockfd;
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