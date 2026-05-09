#include "modbus_client.h"

// 初始化Modbus客户端
int modbus_client_init(ModbusClient *client, const char *server_ip, int server_port, int slave_id, int timeout_ms) {
    if (!client || !server_ip) {
        return MODBUS_ERROR_SOCKET;
    }
    
    // 初始化Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return MODBUS_ERROR_SOCKET;
    }
    
    // 创建套接字
    client->sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client->sockfd == INVALID_SOCKET) {
        WSACleanup();
        return MODBUS_ERROR_SOCKET;
    }
    
    // 设置服务器信息
    strncpy(client->server_ip, server_ip, sizeof(client->server_ip) - 1);
    client->server_ip[sizeof(client->server_ip) - 1] = '\0';
    client->server_port = server_port;
    client->slave_id = slave_id;
    client->timeout_ms = timeout_ms;
    
    // 设置超时
    DWORD timeout = timeout_ms;
    if (setsockopt(client->sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        closesocket(client->sockfd);
        WSACleanup();
        return MODBUS_ERROR_SOCKET;
    }
    
    if (setsockopt(client->sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout)) == SOCKET_ERROR) {
        closesocket(client->sockfd);
        WSACleanup();
        return MODBUS_ERROR_SOCKET;
    }
    
    return MODBUS_SUCCESS;
}

// 连接到Modbus服务器
int modbus_client_connect(ModbusClient *client) {
    if (!client || client->sockfd == INVALID_SOCKET) {
        return MODBUS_ERROR_CONNECT;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(client->server_port);
    
    // 转换IP地址
    if (inet_pton(AF_INET, client->server_ip, &server_addr.sin_addr) <= 0) {
        return MODBUS_ERROR_CONNECT;
    }
    
    // 连接服务器
    if (connect(client->sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        return MODBUS_ERROR_CONNECT;
    }
    
    return MODBUS_SUCCESS;
}

// 断开与Modbus服务器的连接
int modbus_client_disconnect(ModbusClient *client) {
    if (!client || client->sockfd == INVALID_SOCKET) {
        return MODBUS_ERROR_SOCKET;
    }
    
    if (closesocket(client->sockfd) == SOCKET_ERROR) {
        return MODBUS_ERROR_SOCKET;
    }
    
    client->sockfd = INVALID_SOCKET;
    return MODBUS_SUCCESS;
}

// 发送Modbus请求并接收响应
static int modbus_client_transfer(ModbusClient *client, uint8_t *request, int request_len, uint8_t *response, int response_len) {
    if (!client || client->sockfd == INVALID_SOCKET || !request || !response) {
        return MODBUS_ERROR_SOCKET;
    }
    
    // 发送请求
    int bytes_sent = send(client->sockfd, (const char*)request, request_len, 0);
    if (bytes_sent != request_len) {
        return MODBUS_ERROR_SEND;
    }
    
    // 接收响应
    int bytes_recv = recv(client->sockfd, (char*)response, response_len, 0);
    if (bytes_recv <= 0) {
        return MODBUS_ERROR_RECV;
    }
    
    // 检查响应状态
    if (response[7] != 0x00) {
        return MODBUS_ERROR_RESPONSE;
    }
    
    return MODBUS_SUCCESS;
}

// 读取线圈
int modbus_client_read_coils(ModbusClient *client, int addr, int count, uint8_t *dest) {
    if (!client || client->sockfd == INVALID_SOCKET || !dest) {
        return MODBUS_ERROR_SOCKET;
    }
    
    // 构建请求
    uint8_t request[12] = {
        0x00, 0x01,  // 事务ID
        0x00, 0x00,  // 协议ID
        0x00, 0x06,  // 长度
        client->slave_id,  // 从站地址
        MODBUS_FC_READ_COILS,  // 功能码
        (addr >> 8) & 0xFF, addr & 0xFF,  // 起始地址
        (count >> 8) & 0xFF, count & 0xFF   // 数量
    };
    
    // 接收响应
    uint8_t response[256];
    int result = modbus_client_transfer(client, request, sizeof(request), response, sizeof(response));
    if (result != MODBUS_SUCCESS) {
        return result;
    }
    
    // 解析响应
    int byte_count = response[8];
    memcpy(dest, &response[9], byte_count);
    
    return MODBUS_SUCCESS;
}

// 读取离散输入
int modbus_client_read_discrete_inputs(ModbusClient *client, int addr, int count, uint8_t *dest) {
    if (!client || client->sockfd == INVALID_SOCKET || !dest) {
        return MODBUS_ERROR_SOCKET;
    }
    
    // 构建请求
    uint8_t request[12] = {
        0x00, 0x01,  // 事务ID
        0x00, 0x00,  // 协议ID
        0x00, 0x06,  // 长度
        client->slave_id,  // 从站地址
        MODBUS_FC_READ_DISCRETE_INPUTS,  // 功能码
        (addr >> 8) & 0xFF, addr & 0xFF,  // 起始地址
        (count >> 8) & 0xFF, count & 0xFF   // 数量
    };
    
    // 接收响应
    uint8_t response[256];
    int result = modbus_client_transfer(client, request, sizeof(request), response, sizeof(response));
    if (result != MODBUS_SUCCESS) {
        return result;
    }
    
    // 解析响应
    int byte_count = response[8];
    memcpy(dest, &response[9], byte_count);
    
    return MODBUS_SUCCESS;
}

// 读取保持寄存器
int modbus_client_read_holding_registers(ModbusClient *client, int addr, int count, uint16_t *dest) {
    if (!client || client->sockfd == INVALID_SOCKET || !dest) {
        return MODBUS_ERROR_SOCKET;
    }
    
    // 构建请求
    uint8_t request[12] = {
        0x00, 0x01,  // 事务ID
        0x00, 0x00,  // 协议ID
        0x00, 0x06,  // 长度
        client->slave_id,  // 从站地址
        MODBUS_FC_READ_HOLDING_REGISTERS,  // 功能码
        (addr >> 8) & 0xFF, addr & 0xFF,  // 起始地址
        (count >> 8) & 0xFF, count & 0xFF   // 数量
    };
    
    // 接收响应
    uint8_t response[256];
    int result = modbus_client_transfer(client, request, sizeof(request), response, sizeof(response));
    if (result != MODBUS_SUCCESS) {
        return result;
    }
    
    // 解析响应
    int byte_count = response[8];
    int reg_count = byte_count / 2;
    for (int i = 0; i < reg_count; i++) {
        dest[i] = (response[9 + i * 2] << 8) | response[10 + i * 2];
    }
    
    return MODBUS_SUCCESS;
}

// 读取输入寄存器
int modbus_client_read_input_registers(ModbusClient *client, int addr, int count, uint16_t *dest) {
    if (!client || client->sockfd == INVALID_SOCKET || !dest) {
        return MODBUS_ERROR_SOCKET;
    }
    
    // 构建请求
    uint8_t request[12] = {
        0x00, 0x01,  // 事务ID
        0x00, 0x00,  // 协议ID
        0x00, 0x06,  // 长度
        client->slave_id,  // 从站地址
        MODBUS_FC_READ_INPUT_REGISTERS,  // 功能码
        (addr >> 8) & 0xFF, addr & 0xFF,  // 起始地址
        (count >> 8) & 0xFF, count & 0xFF   // 数量
    };
    
    // 接收响应
    uint8_t response[256];
    int result = modbus_client_transfer(client, request, sizeof(request), response, sizeof(response));
    if (result != MODBUS_SUCCESS) {
        return result;
    }
    
    // 解析响应
    int byte_count = response[8];
    int reg_count = byte_count / 2;
    for (int i = 0; i < reg_count; i++) {
        dest[i] = (response[9 + i * 2] << 8) | response[10 + i * 2];
    }
    
    return MODBUS_SUCCESS;
}

// 写入单个线圈
int modbus_client_write_single_coil(ModbusClient *client, int addr, int value) {
    if (!client || client->sockfd == INVALID_SOCKET) {
        return MODBUS_ERROR_SOCKET;
    }
    
    uint16_t coil_value = value ? 0xFF00 : 0x0000;
    
    // 构建请求
    uint8_t request[12] = {
        0x00, 0x01,  // 事务ID
        0x00, 0x00,  // 协议ID
        0x00, 0x06,  // 长度
        client->slave_id,  // 从站地址
        MODBUS_FC_WRITE_SINGLE_COIL,  // 功能码
        (addr >> 8) & 0xFF, addr & 0xFF,  // 地址
        (coil_value >> 8) & 0xFF, coil_value & 0xFF   // 值
    };
    
    // 接收响应
    uint8_t response[12];
    return modbus_client_transfer(client, request, sizeof(request), response, sizeof(response));
}

// 写入单个寄存器
int modbus_client_write_single_register(ModbusClient *client, int addr, int value) {
    if (!client || client->sockfd == INVALID_SOCKET) {
        return MODBUS_ERROR_SOCKET;
    }
    
    // 构建请求
    uint8_t request[12] = {
        0x00, 0x01,  // 事务ID
        0x00, 0x00,  // 协议ID
        0x00, 0x06,  // 长度
        client->slave_id,  // 从站地址
        MODBUS_FC_WRITE_SINGLE_REGISTER,  // 功能码
        (addr >> 8) & 0xFF, addr & 0xFF,  // 地址
        ((uint16_t)value >> 8) & 0xFF, ((uint16_t)value) & 0xFF   // 值
    };
    
    // 接收响应
    uint8_t response[12];
    return modbus_client_transfer(client, request, sizeof(request), response, sizeof(response));
}

// 清理Modbus客户端
void modbus_client_cleanup(ModbusClient *client) {
    if (client) {
        if (client->sockfd != INVALID_SOCKET) {
            closesocket(client->sockfd);
            client->sockfd = INVALID_SOCKET;
        }
        WSACleanup();
    }
}