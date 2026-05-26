#include "ipv6_manager.h"
#include "log_manager.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct AllocatedAddrNode {
    char addr[40];
    struct AllocatedAddrNode *next;
} AllocatedAddrNode;

typedef struct {
    char start_addr[40];
    char end_addr[40];
    int prefix_len;
    AllocatedAddrNode *allocated_addrs;
    uint32_t next_addr_counter;
    pthread_mutex_t lock;
} IPv6AddressPool;

typedef struct DeviceIPv6MapNode {
    char device_name[64];
    char ipv6_address[40];
    struct DeviceIPv6MapNode *next;
} DeviceIPv6MapNode;

static IPv6AddressPool g_ipv6_pool = {0};
static DeviceIPv6MapNode *g_device_ipv6_map = NULL;
static pthread_mutex_t g_device_ipv6_map_lock = PTHREAD_MUTEX_INITIALIZER;

static bool add_device_ipv6_mapping(const char *device_name, const char *ipv6_address) {
    if (!device_name || !ipv6_address) {
        return false;
    }

    pthread_mutex_lock(&g_device_ipv6_map_lock);

    DeviceIPv6MapNode *current = g_device_ipv6_map;
    while (current) {
        if (strcmp(current->device_name, device_name) == 0) {
            strncpy(current->ipv6_address, ipv6_address, sizeof(current->ipv6_address) - 1);
            current->ipv6_address[sizeof(current->ipv6_address) - 1] = '\0';
            pthread_mutex_unlock(&g_device_ipv6_map_lock);
            return true;
        }
        current = current->next;
    }

    DeviceIPv6MapNode *node = (DeviceIPv6MapNode *)calloc(1, sizeof(DeviceIPv6MapNode));
    if (!node) {
        pthread_mutex_unlock(&g_device_ipv6_map_lock);
        return false;
    }

    strncpy(node->device_name, device_name, sizeof(node->device_name) - 1);
    strncpy(node->ipv6_address, ipv6_address, sizeof(node->ipv6_address) - 1);
    node->next = g_device_ipv6_map;
    g_device_ipv6_map = node;

    pthread_mutex_unlock(&g_device_ipv6_map_lock);
    return true;
}

static bool remove_device_ipv6_mapping(const char *device_name) {
    if (!device_name) {
        return false;
    }

    pthread_mutex_lock(&g_device_ipv6_map_lock);

    DeviceIPv6MapNode **pp = &g_device_ipv6_map;
    while (*pp) {
        if (strcmp((*pp)->device_name, device_name) == 0) {
            DeviceIPv6MapNode *node = *pp;
            *pp = node->next;
            free(node);
            pthread_mutex_unlock(&g_device_ipv6_map_lock);
            return true;
        }
        pp = &((*pp)->next);
    }

    pthread_mutex_unlock(&g_device_ipv6_map_lock);
    return false;
}

bool ipv6_get_device_address(const char *device_name, char *ipv6_address, size_t max_len) {
    if (!device_name || !ipv6_address || max_len == 0) {
        return false;
    }

    pthread_mutex_lock(&g_device_ipv6_map_lock);

    DeviceIPv6MapNode *current = g_device_ipv6_map;
    while (current) {
        if (strcmp(current->device_name, device_name) == 0) {
            strncpy(ipv6_address, current->ipv6_address, max_len - 1);
            ipv6_address[max_len - 1] = '\0';
            pthread_mutex_unlock(&g_device_ipv6_map_lock);
            return true;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&g_device_ipv6_map_lock);
    return false;
}

void ipv6_print_device_mappings(void) {
    pthread_mutex_lock(&g_device_ipv6_map_lock);

    log_debug("=== Device-IPv6 Mapping Table ===");
    if (!g_device_ipv6_map) {
        log_debug("  (empty)");
        pthread_mutex_unlock(&g_device_ipv6_map_lock);
        return;
    }

    DeviceIPv6MapNode *current = g_device_ipv6_map;
    while (current) {
        log_debug("  Device: %s -> IPv6: %s", current->device_name, current->ipv6_address);
        current = current->next;
    }

    pthread_mutex_unlock(&g_device_ipv6_map_lock);
}

static void remove_address_from_allocated(const char *addr) {
    pthread_mutex_lock(&g_ipv6_pool.lock);

    AllocatedAddrNode **pp = &g_ipv6_pool.allocated_addrs;
    while (*pp) {
        if (strcmp((*pp)->addr, addr) == 0) {
            AllocatedAddrNode *node = *pp;
            *pp = node->next;
            free(node);
            break;
        }
        pp = &((*pp)->next);
    }

    pthread_mutex_unlock(&g_ipv6_pool.lock);
}

static int compare_ipv6_address(const struct in6_addr *addr1, const struct in6_addr *addr2) {
    for (int i = 0; i < 16; ++i) {
        if (addr1->s6_addr[i] > addr2->s6_addr[i]) {
            return 1;
        }
        if (addr1->s6_addr[i] < addr2->s6_addr[i]) {
            return -1;
        }
    }
    return 0;
}

static void increment_ipv6_address(struct in6_addr *addr) {
    for (int i = 15; i >= 0; --i) {
        if (++addr->s6_addr[i] != 0) {
            break;
        }
    }
}

static bool generate_ipv6_address(const char *device_name, char *ipv6_address, size_t max_len) {
    (void)device_name;

    struct in6_addr start_in6;
    struct in6_addr end_in6;
    struct in6_addr next_in6;

    if (inet_pton(AF_INET6, g_ipv6_pool.start_addr, &start_in6) != 1) {
        log_error("Invalid start IPv6 address: %s", g_ipv6_pool.start_addr);
        return false;
    }

    if (inet_pton(AF_INET6, g_ipv6_pool.end_addr, &end_in6) != 1) {
        log_error("Invalid end IPv6 address: %s", g_ipv6_pool.end_addr);
        return false;
    }

    pthread_mutex_lock(&g_ipv6_pool.lock);

    memcpy(&next_in6, &start_in6, sizeof(next_in6));
    for (uint32_t i = 0; i < g_ipv6_pool.next_addr_counter; ++i) {
        increment_ipv6_address(&next_in6);
    }

    if (compare_ipv6_address(&next_in6, &end_in6) > 0) {
        log_error("IPv6 address pool exhausted");
        pthread_mutex_unlock(&g_ipv6_pool.lock);
        return false;
    }

    if (!inet_ntop(AF_INET6, &next_in6, ipv6_address, max_len)) {
        log_error("inet_ntop failed while generating IPv6 address: %s", strerror(errno));
        pthread_mutex_unlock(&g_ipv6_pool.lock);
        return false;
    }

    AllocatedAddrNode *current = g_ipv6_pool.allocated_addrs;
    while (current) {
        if (strcmp(current->addr, ipv6_address) == 0) {
            g_ipv6_pool.next_addr_counter++;
            pthread_mutex_unlock(&g_ipv6_pool.lock);
            return generate_ipv6_address(device_name, ipv6_address, max_len);
        }
        current = current->next;
    }

    AllocatedAddrNode *node = (AllocatedAddrNode *)calloc(1, sizeof(AllocatedAddrNode));
    if (!node) {
        log_error("Memory allocation failed for allocated address list");
        pthread_mutex_unlock(&g_ipv6_pool.lock);
        return false;
    }

    strncpy(node->addr, ipv6_address, sizeof(node->addr) - 1);
    node->next = g_ipv6_pool.allocated_addrs;
    g_ipv6_pool.allocated_addrs = node;
    g_ipv6_pool.next_addr_counter++;

    pthread_mutex_unlock(&g_ipv6_pool.lock);
    return true;
}

static bool get_ifname_from_index(int nic_index, char *ifname, size_t ifname_size) {
    if (!ifname || ifname_size == 0 || nic_index <= 0) {
        return false;
    }

    char tmp[IF_NAMESIZE] = {0};
    if (!if_indextoname((unsigned int)nic_index, tmp)) {
        log_error("if_indextoname(%d) failed: %s", nic_index, strerror(errno));
        return false;
    }

    if (strlen(tmp) >= ifname_size) {
        return false;
    }

    strcpy(ifname, tmp);
    return true;
}

static bool validate_ipv6_address(const char *ipv6_address) {
    struct in6_addr addr;
    return ipv6_address && inet_pton(AF_INET6, ipv6_address, &addr) == 1;
}

static bool ipv6_address_exists_on_interface(int nic_index, const char *ipv6_address, int prefix_len) {
    char ifname[IF_NAMESIZE] = {0};
    if (!get_ifname_from_index(nic_index, ifname, sizeof(ifname))) {
        return false;
    }

    if (!validate_ipv6_address(ipv6_address)) {
        return false;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ip -6 addr show dev %s | grep -qw '%s/%d'", ifname, ipv6_address, prefix_len);
    return system(cmd) == 0;
}

static int run_ip_command(const char *action, int nic_index, const char *ipv6_address, int prefix_len) {
    char ifname[IF_NAMESIZE] = {0};
    if (!get_ifname_from_index(nic_index, ifname, sizeof(ifname))) {
        return -1;
    }

    if (!validate_ipv6_address(ipv6_address)) {
        log_error("Invalid IPv6 address: %s", ipv6_address ? ipv6_address : "(null)");
        return -1;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ip -6 addr %s %s/%d dev %s", action, ipv6_address, prefix_len, ifname);

    log_debug("Running command: %s", cmd);
    int rc = system(cmd);
    if (rc != 0) {
        log_error("Command failed with rc=%d: %s", rc, cmd);
    }

    return rc;
}

bool ipv6_add_address_to_interface(int nic_index, const char *ipv6_address, int prefix_len) {
    if (ipv6_address_exists_on_interface(nic_index, ipv6_address, prefix_len)) {
        log_info("IPv6 address %s/%d already exists on interface index %d",
                 ipv6_address, prefix_len, nic_index);
        return true;
    }

    if (run_ip_command("add", nic_index, ipv6_address, prefix_len) == 0) {
        log_info("Added IPv6 address %s/%d to interface index %d", ipv6_address, prefix_len, nic_index);
        return true;
    }

    return false;
}

bool ipv6_remove_address_from_interface(int nic_index, const char *ipv6_address) {
    int prefix_len = g_ipv6_pool.prefix_len > 0 ? g_ipv6_pool.prefix_len : 64;
    if (run_ip_command("del", nic_index, ipv6_address, prefix_len) == 0) {
        log_info("Removed IPv6 address %s/%d from interface index %d", ipv6_address, prefix_len, nic_index);
        return true;
    }

    return false;
}

bool ipv6_allocate_address(const char *device_name, int nic_index, char *ipv6_address) {
    if (!device_name || !ipv6_address) {
        return false;
    }

    if (!generate_ipv6_address(device_name, ipv6_address, 40)) {
        return false;
    }

    if (!ipv6_add_address_to_interface(nic_index, ipv6_address, g_ipv6_pool.prefix_len)) {
        remove_address_from_allocated(ipv6_address);
        return false;
    }

    add_device_ipv6_mapping(device_name, ipv6_address);

    log_info("Successfully allocated IPv6 address %s for device %s on interface %d",
             ipv6_address, device_name, nic_index);
    return true;
}

bool ipv6_release_address(const char *device_name, int nic_index, const char *ipv6_address) {
    if (!device_name || !ipv6_address) {
        return false;
    }

    bool removed = ipv6_remove_address_from_interface(nic_index, ipv6_address);
    remove_address_from_allocated(ipv6_address);
    remove_device_ipv6_mapping(device_name);

    if (removed) {
        log_info("Successfully released IPv6 address %s from device %s on interface %d",
                 ipv6_address, device_name, nic_index);
    }

    return removed;
}

bool ipv6_manager_init(const char *start_addr, const char *end_addr, int prefix_len) {
    if (!start_addr || !end_addr || prefix_len <= 0 || prefix_len > 128) {
        return false;
    }

    memset(&g_ipv6_pool, 0, sizeof(g_ipv6_pool));

    if (pthread_mutex_init(&g_ipv6_pool.lock, NULL) != 0) {
        log_error("Failed to initialize IPv6 pool mutex");
        return false;
    }

    strncpy(g_ipv6_pool.start_addr, start_addr, sizeof(g_ipv6_pool.start_addr) - 1);
    strncpy(g_ipv6_pool.end_addr, end_addr, sizeof(g_ipv6_pool.end_addr) - 1);
    g_ipv6_pool.prefix_len = prefix_len;

    log_info("IPv6 address manager initialized, range %s/%d to %s/%d",
             start_addr, prefix_len, end_addr, prefix_len);
    return true;
}

void ipv6_manager_cleanup(void) {
    pthread_mutex_lock(&g_ipv6_pool.lock);

    while (g_ipv6_pool.allocated_addrs) {
        AllocatedAddrNode *node = g_ipv6_pool.allocated_addrs;
        g_ipv6_pool.allocated_addrs = node->next;
        free(node);
    }

    pthread_mutex_unlock(&g_ipv6_pool.lock);
    pthread_mutex_destroy(&g_ipv6_pool.lock);

    pthread_mutex_lock(&g_device_ipv6_map_lock);
    while (g_device_ipv6_map) {
        DeviceIPv6MapNode *node = g_device_ipv6_map;
        g_device_ipv6_map = node->next;
        free(node);
    }
    pthread_mutex_unlock(&g_device_ipv6_map_lock);

    memset(&g_ipv6_pool, 0, sizeof(g_ipv6_pool));
    log_info("IPv6 address manager cleaned up");
}
