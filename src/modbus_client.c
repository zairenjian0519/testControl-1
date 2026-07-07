#include "modbus_client.h"
#include "log_manager.h"
#include <errno.h>


// 初始化Modbus客户端
int modbus_client_init(ModbusClient *client, const char *server_ip, int server_port, int slave_id, int timeout_ms) {
    if (!client || !server_ip) {
        return MODBUS_ERROR_INIT;
    }
    // 创建Modbus TCP上下文
    fprintf(stdout, "Debug: modbus_new_tcp with server_ip=%s, server_port=%d\n", server_ip, server_port);
    // 直接使用modbus_new_tcp，并确保参数正确
    client->ctx = modbus_new_tcp(server_ip, server_port);
    if (client->ctx == NULL) {
        fprintf(stderr, "modbus_new_tcp failed: %s\n", modbus_strerror(errno));
        return MODBUS_ERROR_INIT;
    }



	#if 0
    // 检查是否需要设置套接字选项
    int sockfd = modbus_get_socket(client->ctx);
    if (sockfd == -1) {
        fprintf(stdout, "Debug: Socket not yet created, that's normal\n");
    } else {
        fprintf(stdout, "Debug: Socket already created: %d\n", sockfd);
    }
    
    // 在Modbus TCP中，slave ID通常不需要设置，或者由协议处理
    // 注释掉slave ID设置，避免可能的参数错误
    // fprintf(stdout, "Debug: modbus_set_slave with slave_id=%d\n", slave_id);
    // if (modbus_set_slave(client->ctx, slave_id) != 0) {
    //     fprintf(stderr, "modbus_set_slave failed: %s\n", modbus_strerror(errno));
    //     modbus_free(client->ctx);
    //     client->ctx = NULL;
    //     return MODBUS_ERROR_INIT;
    // }
    
    // 设置超时
    uint32_t to_sec = timeout_ms / 1000;
    uint32_t to_usec = (timeout_ms % 1000) * 1000;
    fprintf(stdout, "Debug: modbus_set_response_timeout with to_sec=%u, to_usec=%u\n", to_sec, to_usec);
    if (modbus_set_response_timeout(client->ctx, to_sec, to_usec) != 0) {
        fprintf(stderr, "modbus_set_response_timeout failed: %s\n", modbus_strerror(errno));
        modbus_free(client->ctx);
        client->ctx = NULL;
        return MODBUS_ERROR_INIT;
    }
    fprintf(stdout, "Debug: modbus_set_byte_timeout with to_sec=%u, to_usec=%u\n", to_sec, to_usec);
    if (modbus_set_byte_timeout(client->ctx, to_sec, to_usec) != 0) {
        fprintf(stderr, "modbus_set_byte_timeout failed: %s\n", modbus_strerror(errno));
        modbus_free(client->ctx);
        client->ctx = NULL;
        return MODBUS_ERROR_INIT;
    }

	#endif
	
    // 保存服务器信息
    strncpy(client->server_ip, server_ip, sizeof(client->server_ip) - 1);
    client->server_ip[sizeof(client->server_ip) - 1] = '\0';
    client->server_port = server_port;
    client->slave_id = slave_id;
    client->timeout_ms = timeout_ms;
    
    return MODBUS_SUCCESS;
}



// 连接到Modbus服务器
int modbus_client_connect(ModbusClient *client) {
    log_debug("modbus_client_connect entered");

	
    if (!client) {
        log_debug("client is NULL");
        return MODBUS_ERROR_CONNECT;
    }
    
    if (client->ctx == NULL) {
        log_debug("client->ctx is NULL");
        return MODBUS_ERROR_CONNECT;
    }
    
    log_debug("client->ctx is not NULL, attempting modbus_connect");
	
    // 只在调试级别下开启Modbus调试
    if (log_get_current_level() == LOG_LEVEL_DEBUG) {
        modbus_set_debug(client->ctx, 1);
    }
    
	modbus_set_slave(client->ctx, 1);
	
    // 直接调用modbus_connect，不需要额外的套接字处理
    if (modbus_connect(client->ctx) == -1) {
        log_error("modbus_connect failed: %s", modbus_strerror(errno));
        log_debug("errno=%d", errno);
        return MODBUS_ERROR_CONNECT;
    }
    
    log_debug("modbus_connect succeeded");
    return MODBUS_SUCCESS;
}

// 断开与Modbus服务器的连接
int modbus_client_disconnect(ModbusClient *client) {
    if (!client || client->ctx == NULL) {
        return MODBUS_SUCCESS;
    }
    
    modbus_close(client->ctx);
    return MODBUS_SUCCESS;
}

// 读取线圈
int modbus_client_read_coils(ModbusClient *client, int addr, int count, uint8_t *dest) {
    if (!client || client->ctx == NULL || !dest) {
        return MODBUS_ERROR_READ;
    }
    
    int rc = modbus_read_bits(client->ctx, addr, count, dest);
    if (rc == -1) {
        fprintf(stderr, "modbus_read_bits failed: %s\n", modbus_strerror(errno));
        return MODBUS_ERROR_READ;
    }
    
    return MODBUS_SUCCESS;
}

// 读取离散输入
int modbus_client_read_discrete_inputs(ModbusClient *client, int addr, int count, uint8_t *dest) {
    if (!client || client->ctx == NULL || !dest) {
        return MODBUS_ERROR_READ;
    }
    
    int rc = modbus_read_input_bits(client->ctx, addr, count, dest);
    if (rc == -1) {
        fprintf(stderr, "modbus_read_input_bits failed: %s\n", modbus_strerror(errno));
        return MODBUS_ERROR_READ;
    }
    
    return MODBUS_SUCCESS;
}

// 读取保持寄存器
int modbus_client_read_holding_registers(ModbusClient *client, int addr, int count, uint16_t *dest) {
    if (!client || client->ctx == NULL || !dest) {
        return MODBUS_ERROR_READ;
    }
    
    int rc = modbus_read_registers(client->ctx, addr, count, dest);
    if (rc == -1) {
        fprintf(stderr, "modbus_read_registers failed: %s\n", modbus_strerror(errno));
        return MODBUS_ERROR_READ;
    }
    
    return MODBUS_SUCCESS;
}

// 读取输入寄存器
int modbus_client_read_input_registers(ModbusClient *client, int addr, int count, uint16_t *dest) {
    if (!client || client->ctx == NULL || !dest) {
        return MODBUS_ERROR_READ;
    }
    
    int rc = modbus_read_input_registers(client->ctx, addr, count, dest);
    if (rc == -1) {
        fprintf(stderr, "modbus_read_input_registers failed: %s\n", modbus_strerror(errno));
        return MODBUS_ERROR_READ;
    }
    
    return MODBUS_SUCCESS;
}

// 写入单个线圈
int modbus_client_write_single_coil(ModbusClient *client, int addr, int value) {
    if (!client || client->ctx == NULL) {
        return MODBUS_ERROR_WRITE;
    }
    
    int rc = modbus_write_bit(client->ctx, addr, value);
    if (rc == -1) {
        log_error("modbus_write_bit failed: addr=%d errno=%d (%s)",
                  addr, errno, modbus_strerror(errno));
        return MODBUS_ERROR_WRITE;
    }
    
    return MODBUS_SUCCESS;
}

// 写入单个寄存器
int modbus_client_write_single_register(ModbusClient *client, int addr, int value) {
    if (!client || client->ctx == NULL) {
        return MODBUS_ERROR_WRITE;
    }
    
    int rc = modbus_write_register(client->ctx, addr, value);
    if (rc == -1) {
        log_error("modbus_write_register failed: addr=%d errno=%d (%s)",
                  addr, errno, modbus_strerror(errno));
        return MODBUS_ERROR_WRITE;
    }
    
    return MODBUS_SUCCESS;
}

// 清理Modbus客户端
void modbus_client_cleanup(ModbusClient *client) {
    if (client) {
        if (client->ctx != NULL) {
            modbus_free(client->ctx);
            client->ctx = NULL;
        }
    }
}
