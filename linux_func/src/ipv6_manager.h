#ifndef IPV6_MANAGER_H
#define IPV6_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ipv6_allocate_address(const char *device_name, int nic_index, char *ipv6_address);
bool ipv6_release_address(const char *device_name, int nic_index, const char *ipv6_address);
bool ipv6_add_address_to_interface(int nic_index, const char *ipv6_address, int prefix_len);
bool ipv6_remove_address_from_interface(int nic_index, const char *ipv6_address);
bool ipv6_manager_init(const char *start_addr, const char *end_addr, int prefix_len);
void ipv6_manager_cleanup(void);
bool ipv6_get_device_address(const char *device_name, char *ipv6_address, size_t max_len);
void ipv6_print_device_mappings(void);

#ifdef __cplusplus
}
#endif

#endif
