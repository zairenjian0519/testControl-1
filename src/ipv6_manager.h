#ifndef IPV6_MANAGER_H
#define IPV6_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

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

bool ipv6_add_address_to_interface(int nic_index, const char *ipv6_address, int prefix_len);

bool ipv6_remove_address_from_interface(int nic_index, const char *ipv6_address);

void ipv6_remove_all_allocated_addresses(int nic_index);

void ipv6_manager_set_add_options(bool skip_as_source,
                                  unsigned int batch_add_limit,
                                  unsigned int batch_add_delay_ms);

void ipv6_manager_note_address_added(void);

void ipv6_manager_delay_before_next_device_if_needed(void);

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

/**
 * @brief 获取设备的IPv6地址
 * @param device_name 设备名称
 * @param ipv6_address 输出参数，存储获取的IPv6地址
 * @param max_len 输出缓冲区最大长度
 * @return true表示获取成功，false表示失败
 */
bool ipv6_get_device_address(const char *device_name, char *ipv6_address, size_t max_len);

/**
 * @brief 打印所有设备-IPv6映射
 */
void ipv6_print_device_mappings(void);

#endif // IPV6_MANAGER_H
