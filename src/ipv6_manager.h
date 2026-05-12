#ifndef IPV6_MANAGER_H
#define IPV6_MANAGER_H

#include <stdbool.h>

// IPv6地址管理函数声明

/**
 * @brief 为设备分配IPv6地址
 * @param device_name 设备名称
 * @param nic_index 网络接口索引
 * @param ipv6_address 输出参数，存储分配的IPv6地址
 * @return true表示分配成功，false表示失败
 */
bool ipv6_allocate_address(const char *device_name, int nic_index, char *ipv6_address);

/**
 * @brief 释放设备的IPv6地址
 * @param device_name 设备名称
 * @param nic_index 网络接口索引
 * @param ipv6_address 要释放的IPv6地址
 * @return true表示释放成功，false表示失败
 */
bool ipv6_release_address(const char *device_name, int nic_index, const char *ipv6_address);

/**
 * @brief 初始化IPv6地址管理器
 * @param start_addr 地址池起始地址
 * @param end_addr 地址池结束地址
 * @param prefix_len 前缀长度
 * @return true表示初始化成功，false表示失败
 */
bool ipv6_manager_init(const char *start_addr, const char *end_addr, int prefix_len);

/**
 * @brief 清理IPv6地址管理器
 */
void ipv6_manager_cleanup(void);

#endif // IPV6_MANAGER_H
