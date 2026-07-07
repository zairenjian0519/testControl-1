#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include "ipv6_manager.h"
#include "log_manager.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#else
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netlink/netlink.h>
#include <netlink/route/addr.h>
#include <netlink/route/link.h>
#include <netlink/route/route.h>
#endif

// 已分配地址的链表节点
typedef struct AllocatedAddrNode {
    char addr[40];
    struct AllocatedAddrNode *next;
} AllocatedAddrNode;

// 地址池管理结构体
typedef struct {
    char start_addr[40];
    char end_addr[40];
    int prefix_len;
    AllocatedAddrNode *allocated_addrs;  // 已分配地址链表
    AllocatedAddrNode *interface_addrs;  // 本程序实际添加到网口的地址链表
    uint32_t next_addr_counter;           // 下一个要分配的地址计数器
    pthread_mutex_t lock;                 // 互斥锁，保护地址池的并发访问
} IPv6AddressPool;

static IPv6AddressPool g_ipv6_pool = {0};

typedef struct {
    bool skip_as_source;
    unsigned int batch_add_limit;
    unsigned int batch_add_delay_ms;
    unsigned int added_since_last_delay;
    bool delay_pending;
    pthread_mutex_t lock;
} IPv6AddOptions;

static IPv6AddOptions g_ipv6_add_options = {
    true,
    300,
    4000,
    0,
    false,
    PTHREAD_MUTEX_INITIALIZER
};

static bool ipv6_manager_get_skip_as_source(void)
{
    bool skip_as_source;

    pthread_mutex_lock(&g_ipv6_add_options.lock);
    skip_as_source = g_ipv6_add_options.skip_as_source;
    pthread_mutex_unlock(&g_ipv6_add_options.lock);

    return skip_as_source;
}

void ipv6_manager_set_add_options(bool skip_as_source,
                                  unsigned int batch_add_limit,
                                  unsigned int batch_add_delay_ms)
{
    pthread_mutex_lock(&g_ipv6_add_options.lock);
    g_ipv6_add_options.skip_as_source = skip_as_source;
    g_ipv6_add_options.batch_add_limit = batch_add_limit;
    g_ipv6_add_options.batch_add_delay_ms = batch_add_delay_ms;
    g_ipv6_add_options.added_since_last_delay = 0;
    g_ipv6_add_options.delay_pending = false;
    pthread_mutex_unlock(&g_ipv6_add_options.lock);

    log_info("IPv6 add options: skip_as_source=%s batch_add_limit=%u batch_add_delay_ms=%u",
             skip_as_source ? "true" : "false",
             batch_add_limit,
             batch_add_delay_ms);
}

void ipv6_manager_note_address_added(void)
{
    pthread_mutex_lock(&g_ipv6_add_options.lock);

    if (g_ipv6_add_options.batch_add_limit > 0 &&
        g_ipv6_add_options.batch_add_delay_ms > 0) {
        g_ipv6_add_options.added_since_last_delay++;
        if (g_ipv6_add_options.added_since_last_delay >= g_ipv6_add_options.batch_add_limit) {
            g_ipv6_add_options.added_since_last_delay = 0;
            g_ipv6_add_options.delay_pending = true;
        }
    }

    pthread_mutex_unlock(&g_ipv6_add_options.lock);
}

void ipv6_manager_delay_before_next_device_if_needed(void)
{
    bool should_delay = false;
    unsigned int batch_add_limit = 0;
    unsigned int batch_add_delay_ms = 0;

    pthread_mutex_lock(&g_ipv6_add_options.lock);
    if (g_ipv6_add_options.delay_pending &&
        g_ipv6_add_options.batch_add_delay_ms > 0) {
        should_delay = true;
        batch_add_limit = g_ipv6_add_options.batch_add_limit;
        batch_add_delay_ms = g_ipv6_add_options.batch_add_delay_ms;
        g_ipv6_add_options.delay_pending = false;
    }
    pthread_mutex_unlock(&g_ipv6_add_options.lock);

    if (!should_delay) {
        return;
    }

    log_info("IPv6 batch add threshold reached: limit=%u, delaying %u ms before next online device",
             batch_add_limit,
             batch_add_delay_ms);
#ifdef _WIN32
    Sleep(batch_add_delay_ms);
#else
    usleep((useconds_t)batch_add_delay_ms * 1000);
#endif
}

static bool addr_list_contains(AllocatedAddrNode *head, const char *addr)
{
    while (head != NULL) {
        if (strcmp(head->addr, addr) == 0) {
            return true;
        }
        head = head->next;
    }
    return false;
}

static bool addr_list_add_unique(AllocatedAddrNode **head, const char *addr)
{
    if (!head || !addr || addr[0] == '\0') {
        return false;
    }

    if (addr_list_contains(*head, addr)) {
        return true;
    }

    AllocatedAddrNode *new_node = (AllocatedAddrNode *)malloc(sizeof(AllocatedAddrNode));
    if (new_node == NULL) {
        return false;
    }

    strncpy(new_node->addr, addr, sizeof(new_node->addr) - 1);
    new_node->addr[sizeof(new_node->addr) - 1] = '\0';
    new_node->next = *head;
    *head = new_node;
    return true;
}

static void addr_list_remove(AllocatedAddrNode **head, const char *addr)
{
    if (!head || !addr) {
        return;
    }

    AllocatedAddrNode **pp = head;
    while (*pp != NULL) {
        if (strcmp((*pp)->addr, addr) == 0) {
            AllocatedAddrNode *temp = *pp;
            *pp = (*pp)->next;
            free(temp);
            return;
        }
        pp = &((*pp)->next);
    }
}

static void addr_list_clear(AllocatedAddrNode **head)
{
    if (!head) {
        return;
    }

    while (*head != NULL) {
        AllocatedAddrNode *temp = *head;
        *head = temp->next;
        free(temp);
    }
}

#ifdef _WIN32
static int g_netsh_add_consecutive_failures = 0;
static pthread_mutex_t g_netsh_add_retry_lock = PTHREAD_MUTEX_INITIALIZER;

static bool should_retry_netsh_add(void)
{
    bool retry;

    pthread_mutex_lock(&g_netsh_add_retry_lock);
    retry = g_netsh_add_consecutive_failures < 3;
    pthread_mutex_unlock(&g_netsh_add_retry_lock);

    return retry;
}

static void record_netsh_add_result(int result)
{
    pthread_mutex_lock(&g_netsh_add_retry_lock);
    if (result == 0) {
        g_netsh_add_consecutive_failures = 0;
    } else if (g_netsh_add_consecutive_failures < 3) {
        g_netsh_add_consecutive_failures++;
    }
    pthread_mutex_unlock(&g_netsh_add_retry_lock);
}

static void sleep_before_netsh_add_retry(void)
{
    log_info("Waiting 2000 ms before retrying IPv6 netsh add command");
    Sleep(2000);
}

typedef struct {
    bool initialized;
    int nic_index;
    bool interface_found;
    AllocatedAddrNode *system_addrs;
    AllocatedAddrNode *program_addrs;
    pthread_mutex_t lock;
} IPv6InterfaceCache;

static IPv6InterfaceCache g_ipv6_interface_cache = {
    false,
    0,
    false,
    NULL,
    NULL,
    PTHREAD_MUTEX_INITIALIZER
};

static void format_in6_addr(const IN6_ADDR *addr, char *buffer, size_t buffer_size)
{
    const unsigned char *bytes = (const unsigned char *)addr;

    snprintf(buffer, buffer_size, "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
             ((unsigned int)bytes[0] << 8) | bytes[1],
             ((unsigned int)bytes[2] << 8) | bytes[3],
             ((unsigned int)bytes[4] << 8) | bytes[5],
             ((unsigned int)bytes[6] << 8) | bytes[7],
             ((unsigned int)bytes[8] << 8) | bytes[9],
             ((unsigned int)bytes[10] << 8) | bytes[11],
             ((unsigned int)bytes[12] << 8) | bytes[13],
             ((unsigned int)bytes[14] << 8) | bytes[15]);
}

static bool canonicalize_ipv6_address(const char *ipv6_address, char *canonical, size_t canonical_size)
{
    IN6_ADDR addr;

    if (!ipv6_address || !canonical || canonical_size < 40) {
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    if (InetPton(AF_INET6, ipv6_address, &addr) != 1) {
        return false;
    }

    format_in6_addr(&addr, canonical, canonical_size);
    return true;
}

static bool init_unicast_ipv6_row(MIB_UNICASTIPADDRESS_ROW *row,
                                  int nic_index,
                                  const char *ipv6_address,
                                  int prefix_len)
{
    if (!row || nic_index <= 0 || !ipv6_address) {
        return false;
    }

    InitializeUnicastIpAddressEntry(row);
    row->Address.Ipv6.sin6_family = AF_INET6;
    if (InetPton(AF_INET6, ipv6_address, &row->Address.Ipv6.sin6_addr) != 1) {
        return false;
    }

    row->InterfaceIndex = (NET_IFINDEX)nic_index;
    if (prefix_len >= 0) {
        row->OnLinkPrefixLength = (UINT8)prefix_len;
    }
    row->PrefixOrigin = IpPrefixOriginManual;
    row->SuffixOrigin = IpSuffixOriginManual;
    row->ValidLifetime = 0xffffffffUL;
    row->PreferredLifetime = 0xffffffffUL;
    row->SkipAsSource = ipv6_manager_get_skip_as_source() ? TRUE : FALSE;
    return true;
}

static bool create_unicast_ipv6_address(int nic_index,
                                        const char *ipv6_address,
                                        int prefix_len,
                                        bool *already_exists)
{
    MIB_UNICASTIPADDRESS_ROW row;
    char canonical_addr[40];
    NETIO_STATUS status;

    if (already_exists) {
        *already_exists = false;
    }

    if (!canonicalize_ipv6_address(ipv6_address, canonical_addr, sizeof(canonical_addr))) {
        log_error("Invalid IPv6 address: %s", ipv6_address ? ipv6_address : "(null)");
        return false;
    }

    if (!init_unicast_ipv6_row(&row, nic_index, canonical_addr, prefix_len)) {
        log_error("Failed to initialize IPv6 address row for %s on interface %d",
                  canonical_addr, nic_index);
        return false;
    }

    status = CreateUnicastIpAddressEntry(&row);
    if (status == NO_ERROR) {
        log_debug("Created IPv6 address %s/%d on interface %d via IP Helper API",
                  canonical_addr, prefix_len, nic_index);
        return true;
    }

    if (status == ERROR_OBJECT_ALREADY_EXISTS || status == ERROR_ALREADY_EXISTS) {
        if (already_exists) {
            *already_exists = true;
        }
        log_info("IPv6 address %s already exists on interface %d, keeping allocation cache",
                 canonical_addr, nic_index);
        return true;
    }

    log_error("CreateUnicastIpAddressEntry failed for %s/%d on interface %d: %lu",
              canonical_addr, prefix_len, nic_index, (unsigned long)status);
    return false;
}

static bool delete_unicast_ipv6_address(int nic_index, const char *ipv6_address)
{
    MIB_UNICASTIPADDRESS_ROW row;
    char canonical_addr[40];
    NETIO_STATUS status;

    if (!canonicalize_ipv6_address(ipv6_address, canonical_addr, sizeof(canonical_addr))) {
        log_error("Invalid IPv6 address: %s", ipv6_address ? ipv6_address : "(null)");
        return false;
    }

    if (!init_unicast_ipv6_row(&row, nic_index, canonical_addr, -1)) {
        log_error("Failed to initialize IPv6 delete row for %s on interface %d",
                  canonical_addr, nic_index);
        return false;
    }

    status = DeleteUnicastIpAddressEntry(&row);
    if (status == NO_ERROR) {
        log_debug("Deleted IPv6 address %s from interface %d via IP Helper API",
                  canonical_addr, nic_index);
        return true;
    }

    if (status == ERROR_NOT_FOUND || status == ERROR_OBJECT_NOT_FOUND) {
        log_warn("IPv6 address %s was not found on interface %d during cleanup",
                 canonical_addr, nic_index);
        return true;
    }

    log_error("DeleteUnicastIpAddressEntry failed for %s on interface %d: %lu",
              canonical_addr, nic_index, (unsigned long)status);
    return false;
}

static void clear_ipv6_interface_cache_locked(void)
{
    addr_list_clear(&g_ipv6_interface_cache.system_addrs);
    addr_list_clear(&g_ipv6_interface_cache.program_addrs);
    g_ipv6_interface_cache.initialized = false;
    g_ipv6_interface_cache.nic_index = 0;
    g_ipv6_interface_cache.interface_found = false;
}

static void reset_ipv6_interface_cache(void)
{
    pthread_mutex_lock(&g_ipv6_interface_cache.lock);
    clear_ipv6_interface_cache_locked();
    pthread_mutex_unlock(&g_ipv6_interface_cache.lock);
}

static bool refresh_ipv6_interface_cache_locked(int nic_index)
{
    DWORD result;
    ULONG out_buf_len = 15000;
    IP_ADAPTER_ADDRESSES *adapter_addresses = NULL;
    IP_ADAPTER_ADDRESSES *adapter = NULL;
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;

    if (g_ipv6_interface_cache.nic_index != nic_index) {
        clear_ipv6_interface_cache_locked();
    } else {
        addr_list_clear(&g_ipv6_interface_cache.system_addrs);
        g_ipv6_interface_cache.interface_found = false;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        log_error("WSAStartup failed while refreshing IPv6 cache: %d", WSAGetLastError());
        return false;
    }

    for (;;) {
        adapter_addresses = (IP_ADAPTER_ADDRESSES *)malloc(out_buf_len);
        if (adapter_addresses == NULL) {
            log_error("Memory allocation failed while refreshing IPv6 cache");
            WSACleanup();
            return false;
        }

        result = GetAdaptersAddresses(AF_INET6, flags, NULL, adapter_addresses, &out_buf_len);
        if (result != ERROR_BUFFER_OVERFLOW) {
            break;
        }

        free(adapter_addresses);
        adapter_addresses = NULL;
    }

    if (result != NO_ERROR) {
        log_error("GetAdaptersAddresses failed while refreshing IPv6 cache: %lu", result);
        free(adapter_addresses);
        WSACleanup();
        return false;
    }

    adapter = adapter_addresses;
    while (adapter != NULL) {
        if (adapter->IfIndex == (IF_INDEX)nic_index) {
            IP_ADAPTER_UNICAST_ADDRESS_LH *unicast_addr = adapter->FirstUnicastAddress;

            g_ipv6_interface_cache.interface_found = true;
            while (unicast_addr != NULL) {
                if (unicast_addr->Address.lpSockaddr &&
                    unicast_addr->Address.lpSockaddr->sa_family == AF_INET6) {
                    SOCKADDR_IN6 *current_addr = (SOCKADDR_IN6 *)unicast_addr->Address.lpSockaddr;
                    char canonical[40];

                    format_in6_addr(&current_addr->sin6_addr, canonical, sizeof(canonical));
                    if (!addr_list_add_unique(&g_ipv6_interface_cache.system_addrs, canonical)) {
                        log_error("Memory allocation failed while caching IPv6 address %s", canonical);
                        free(adapter_addresses);
                        WSACleanup();
                        return false;
                    }
                }
                unicast_addr = unicast_addr->Next;
            }
            break;
        }
        adapter = adapter->Next;
    }

    free(adapter_addresses);
    WSACleanup();

    g_ipv6_interface_cache.initialized = true;
    g_ipv6_interface_cache.nic_index = nic_index;

    if (!g_ipv6_interface_cache.interface_found) {
        log_error("Network interface with index %d not found", nic_index);
        return false;
    }

    log_debug("IPv6 interface cache refreshed for interface %d", nic_index);
    return true;
}

static bool ensure_ipv6_interface_cache(int nic_index)
{
    bool ok = true;

    pthread_mutex_lock(&g_ipv6_interface_cache.lock);
    if (!g_ipv6_interface_cache.initialized ||
        g_ipv6_interface_cache.nic_index != nic_index ||
        !g_ipv6_interface_cache.interface_found) {
        ok = refresh_ipv6_interface_cache_locked(nic_index);
    }
    pthread_mutex_unlock(&g_ipv6_interface_cache.lock);

    return ok;
}

static bool ipv6_interface_cache_contains(int nic_index, const char *ipv6_address)
{
    char canonical[40];
    bool found = false;

    if (!canonicalize_ipv6_address(ipv6_address, canonical, sizeof(canonical))) {
        return false;
    }

    pthread_mutex_lock(&g_ipv6_interface_cache.lock);
    if (!g_ipv6_interface_cache.initialized ||
        g_ipv6_interface_cache.nic_index != nic_index ||
        !g_ipv6_interface_cache.interface_found) {
        refresh_ipv6_interface_cache_locked(nic_index);
    }

    found = g_ipv6_interface_cache.interface_found &&
            (addr_list_contains(g_ipv6_interface_cache.system_addrs, canonical) ||
             addr_list_contains(g_ipv6_interface_cache.program_addrs, canonical));
    pthread_mutex_unlock(&g_ipv6_interface_cache.lock);

    return found;
}

static bool refresh_ipv6_interface_cache_and_contains(int nic_index, const char *ipv6_address)
{
    char canonical[40];
    bool found = false;

    if (!canonicalize_ipv6_address(ipv6_address, canonical, sizeof(canonical))) {
        return false;
    }

    pthread_mutex_lock(&g_ipv6_interface_cache.lock);
    if (refresh_ipv6_interface_cache_locked(nic_index)) {
        found = addr_list_contains(g_ipv6_interface_cache.system_addrs, canonical) ||
                addr_list_contains(g_ipv6_interface_cache.program_addrs, canonical);
    }
    pthread_mutex_unlock(&g_ipv6_interface_cache.lock);

    return found;
}

static bool mark_ipv6_interface_address_added(int nic_index, const char *ipv6_address)
{
    char canonical[40];
    bool ok;

    if (!canonicalize_ipv6_address(ipv6_address, canonical, sizeof(canonical))) {
        return false;
    }

    pthread_mutex_lock(&g_ipv6_interface_cache.lock);
    if (!g_ipv6_interface_cache.initialized ||
        g_ipv6_interface_cache.nic_index != nic_index ||
        !g_ipv6_interface_cache.interface_found) {
        if (!refresh_ipv6_interface_cache_locked(nic_index)) {
            pthread_mutex_unlock(&g_ipv6_interface_cache.lock);
            return false;
        }
    }

    ok = addr_list_add_unique(&g_ipv6_interface_cache.system_addrs, canonical) &&
         addr_list_add_unique(&g_ipv6_interface_cache.program_addrs, canonical);
    pthread_mutex_unlock(&g_ipv6_interface_cache.lock);

    return ok;
}

static void mark_ipv6_interface_address_removed(const char *ipv6_address)
{
    char canonical[40];

    if (!canonicalize_ipv6_address(ipv6_address, canonical, sizeof(canonical))) {
        return;
    }

    pthread_mutex_lock(&g_ipv6_interface_cache.lock);
    addr_list_remove(&g_ipv6_interface_cache.system_addrs, canonical);
    addr_list_remove(&g_ipv6_interface_cache.program_addrs, canonical);
    pthread_mutex_unlock(&g_ipv6_interface_cache.lock);
}
#endif

// Device-IPv6映射节点
typedef struct DeviceIPv6MapNode {
    char device_name[64];
    char ipv6_address[40];
    struct DeviceIPv6MapNode *next;
} DeviceIPv6MapNode;

// Device-IPv6映射表
static DeviceIPv6MapNode *g_device_ipv6_map = NULL;
static pthread_mutex_t g_device_ipv6_map_lock = PTHREAD_MUTEX_INITIALIZER;

// 添加设备-IPv6映射
static bool add_device_ipv6_mapping(const char *device_name, const char *ipv6_address)
{
    if (!device_name || !ipv6_address) {
        return false;
    }
    
    pthread_mutex_lock(&g_device_ipv6_map_lock);
    
    // 先检查是否已存在该设备的映射
    DeviceIPv6MapNode *current = g_device_ipv6_map;
    while (current != NULL) {
        if (strcmp(current->device_name, device_name) == 0) {
            // 更新现有映射
            strncpy(current->ipv6_address, ipv6_address, sizeof(current->ipv6_address) - 1);
            current->ipv6_address[sizeof(current->ipv6_address) - 1] = '\0';
            pthread_mutex_unlock(&g_device_ipv6_map_lock);
            return true;
        }
        current = current->next;
    }
    
    // 创建新节点
    DeviceIPv6MapNode *new_node = (DeviceIPv6MapNode *)malloc(sizeof(DeviceIPv6MapNode));
    if (new_node == NULL) {
        pthread_mutex_unlock(&g_device_ipv6_map_lock);
        return false;
    }
    
    strncpy(new_node->device_name, device_name, sizeof(new_node->device_name) - 1);
    new_node->device_name[sizeof(new_node->device_name) - 1] = '\0';
    strncpy(new_node->ipv6_address, ipv6_address, sizeof(new_node->ipv6_address) - 1);
    new_node->ipv6_address[sizeof(new_node->ipv6_address) - 1] = '\0';
    new_node->next = g_device_ipv6_map;
    g_device_ipv6_map = new_node;
    
    pthread_mutex_unlock(&g_device_ipv6_map_lock);
    return true;
}

// 删除设备-IPv6映射
static bool remove_device_ipv6_mapping(const char *device_name)
{
    if (!device_name) {
        return false;
    }
    
    pthread_mutex_lock(&g_device_ipv6_map_lock);
    
    DeviceIPv6MapNode **pp = &g_device_ipv6_map;
    while (*pp != NULL) {
        if (strcmp((*pp)->device_name, device_name) == 0) {
            DeviceIPv6MapNode *temp = *pp;
            *pp = (*pp)->next;
            free(temp);
            pthread_mutex_unlock(&g_device_ipv6_map_lock);
            return true;
        }
        pp = &((*pp)->next);
    }
    
    pthread_mutex_unlock(&g_device_ipv6_map_lock);
    return false;
}

// 获取设备的IPv6地址
bool ipv6_get_device_address(const char *device_name, char *ipv6_address, size_t max_len)
{
    if (!device_name || !ipv6_address || max_len == 0) {
        return false;
    }
    
    pthread_mutex_lock(&g_device_ipv6_map_lock);
    
    DeviceIPv6MapNode *current = g_device_ipv6_map;
    while (current != NULL) {
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

// 打印所有设备-IPv6映射
void ipv6_print_device_mappings(void)
{
    pthread_mutex_lock(&g_device_ipv6_map_lock);
    
    log_debug("=== Device-IPv6 Mapping Table ===");
    if (g_device_ipv6_map == NULL) {
        log_debug("  (empty)");
        pthread_mutex_unlock(&g_device_ipv6_map_lock);
        return;
    }
    
    DeviceIPv6MapNode *current = g_device_ipv6_map;
    while (current != NULL) {
        log_debug("  Device: %s -> IPv6: %s", current->device_name, current->ipv6_address);
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_device_ipv6_map_lock);
}

// 检查IPv6地址是否已分配
static bool is_address_allocated(const char *addr)
{
    bool found = false;
    
    // 加锁保护
    pthread_mutex_lock(&g_ipv6_pool.lock);
    
    AllocatedAddrNode *current = g_ipv6_pool.allocated_addrs;
    while (current != NULL) {
        if (strcmp(current->addr, addr) == 0) {
            found = true;
            break;
        }
        current = current->next;
    }
    
    // 解锁
    pthread_mutex_unlock(&g_ipv6_pool.lock);
    
    return found;
}

// 将地址添加到已分配列表
static bool add_address_to_allocated(const char *addr)
{
    AllocatedAddrNode *new_node = (AllocatedAddrNode *)malloc(sizeof(AllocatedAddrNode));
    if (new_node == NULL) {
        log_error("Memory allocation failed for allocated address list");
        return false;
    }
    
    strncpy(new_node->addr, addr, sizeof(new_node->addr) - 1);
    new_node->addr[sizeof(new_node->addr) - 1] = '\0';
    
    // 加锁保护
    pthread_mutex_lock(&g_ipv6_pool.lock);
    
    new_node->next = g_ipv6_pool.allocated_addrs;
    g_ipv6_pool.allocated_addrs = new_node;
    
    // 解锁
    pthread_mutex_unlock(&g_ipv6_pool.lock);
    
    return true;
}

// 从已分配列表中删除地址
static bool add_address_to_interface_allocated(const char *addr)
{
    bool added;

    pthread_mutex_lock(&g_ipv6_pool.lock);
    added = addr_list_add_unique(&g_ipv6_pool.interface_addrs, addr);
    pthread_mutex_unlock(&g_ipv6_pool.lock);

    if (!added) {
        log_error("Memory allocation failed for interface address list");
    }

    return added;
}

static void remove_address_from_interface_allocated(const char *addr)
{
#ifdef _WIN32
    char canonical[40];

    if (canonicalize_ipv6_address(addr, canonical, sizeof(canonical))) {
        addr = canonical;
    }
#endif

    pthread_mutex_lock(&g_ipv6_pool.lock);
    addr_list_remove(&g_ipv6_pool.interface_addrs, addr);
    pthread_mutex_unlock(&g_ipv6_pool.lock);
}

static bool remember_interface_address_added_by_program(const char *addr)
{
#ifdef _WIN32
    char canonical[40];

    if (canonicalize_ipv6_address(addr, canonical, sizeof(canonical))) {
        addr = canonical;
    }
#endif

    if (!add_address_to_interface_allocated(addr)) {
        return false;
    }

    return true;
}

static void remove_address_from_allocated(const char *addr)
{
    AllocatedAddrNode *current = NULL;
    AllocatedAddrNode *prev = NULL;
    
    // 加锁保护
    pthread_mutex_lock(&g_ipv6_pool.lock);
    
    current = g_ipv6_pool.allocated_addrs;
    while (current != NULL) {
        if (strcmp(current->addr, addr) == 0) {
            if (prev == NULL) {
                g_ipv6_pool.allocated_addrs = current->next;
            } else {
                prev->next = current->next;
            }
            free(current);
            break;
        }
        prev = current;
        current = current->next;
    }
    
    // 解锁
    pthread_mutex_unlock(&g_ipv6_pool.lock);
}

// 比较两个IPv6地址，返回0表示相等，1表示addr1大于addr2，-1表示addr1小于addr2
static int compare_ipv6_address(const struct in6_addr *addr1, const struct in6_addr *addr2)
{
    for (int i = 0; i < 16; i++) {
        if (addr1->s6_addr[i] > addr2->s6_addr[i]) {
            return 1;
        } else if (addr1->s6_addr[i] < addr2->s6_addr[i]) {
            return -1;
        }
    }
    return 0;
}

// 递增IPv6地址（从最低位开始）
// IPv6 parsing and pool-range helpers.
static bool parse_ipv6_address_for_compare(const char *text, struct in6_addr *addr)
{
    if (!text || !addr) {
        return false;
    }

#ifdef _WIN32
    return InetPton(AF_INET6, text, addr) == 1;
#else
    return inet_pton(AF_INET6, text, addr) == 1;
#endif
}

static bool ipv6_address_in_range(const char *addr_text,
                                  const char *start_text,
                                  const char *end_text)
{
    struct in6_addr addr;
    struct in6_addr start;
    struct in6_addr end;

    if (!parse_ipv6_address_for_compare(addr_text, &addr) ||
        !parse_ipv6_address_for_compare(start_text, &start) ||
        !parse_ipv6_address_for_compare(end_text, &end)) {
        return false;
    }

    return compare_ipv6_address(&addr, &start) >= 0 &&
           compare_ipv6_address(&addr, &end) <= 0;
}

static void increment_ipv6_address(struct in6_addr *addr)
{
    // 从最低位字节开始递增
    for (int i = 15; i >= 0; i--) {
        if (++addr->s6_addr[i] != 0) {
            // 如果当前字节没有溢出，递增结束
            break;
        }
        // 如果当前字节溢出，继续递增下一个更高位字节
    }
}

// IPv6地址生成函数
static bool generate_ipv6_address(const char *device_name, char *ipv6_address, size_t max_len)
{
    // 从配置文件的地址池动态分配IPv6地址
    // 从起始地址开始，从最低位开始递增分配
    
    struct in6_addr start_in6;
    struct in6_addr end_in6;
    struct in6_addr next_in6;
    bool success = false;
    
    // 解析起始地址
    if (inet_pton(AF_INET6, g_ipv6_pool.start_addr, &start_in6) != 1) {
        log_error("Invalid start IPv6 address: %s", g_ipv6_pool.start_addr);
        return false;
    }
    
    // 解析结束地址
    if (inet_pton(AF_INET6, g_ipv6_pool.end_addr, &end_in6) != 1) {
        log_error("Invalid end IPv6 address: %s", g_ipv6_pool.end_addr);
        return false;
    }
    
    // 加锁保护地址池的并发访问
    pthread_mutex_lock(&g_ipv6_pool.lock);
    
    // 计算下一个要分配的地址
    memcpy(&next_in6, &start_in6, sizeof(struct in6_addr));
    
    // 从起始地址开始，根据计数器递增地址
    // 这里实现完整的128位地址递增
    for (uint32_t i = 0; i < g_ipv6_pool.next_addr_counter; i++) {
        increment_ipv6_address(&next_in6);
    }
    
    // 检查地址是否在范围内
    if (compare_ipv6_address(&next_in6, &end_in6) > 0) {
        log_error("IPv6 address pool exhausted: %s exceeds end address %s",
                 g_ipv6_pool.start_addr, g_ipv6_pool.end_addr);
        pthread_mutex_unlock(&g_ipv6_pool.lock);
        return false;
    }
    
    // 将地址转换为字符串（确保每个16位段都显示4个字符，包含前导零）
    uint16_t *segments = (uint16_t *)next_in6.s6_addr;
    snprintf(ipv6_address, max_len, "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x",
            ntohs(segments[0]), ntohs(segments[1]), ntohs(segments[2]), ntohs(segments[3]),
            ntohs(segments[4]), ntohs(segments[5]), ntohs(segments[6]), ntohs(segments[7]));
    
    // 检查地址是否已分配
    AllocatedAddrNode *current = g_ipv6_pool.allocated_addrs;
    bool is_allocated = false;
    while (current != NULL) {
        if (strcmp(current->addr, ipv6_address) == 0) {
            is_allocated = true;
            break;
        }
        current = current->next;
    }
    
    if (is_allocated) {
        // 如果已分配，递增计数器并解锁后重试
        g_ipv6_pool.next_addr_counter++;
        pthread_mutex_unlock(&g_ipv6_pool.lock);
        return generate_ipv6_address(device_name, ipv6_address, max_len);
    }
    
    // 添加到已分配列表
    AllocatedAddrNode *new_node = (AllocatedAddrNode *)malloc(sizeof(AllocatedAddrNode));
    if (new_node == NULL) {
        log_error("Memory allocation failed for allocated address list");
        pthread_mutex_unlock(&g_ipv6_pool.lock);
        return false;
    }
    
    strncpy(new_node->addr, ipv6_address, sizeof(new_node->addr) - 1);
    new_node->addr[sizeof(new_node->addr) - 1] = '\0';
    new_node->next = g_ipv6_pool.allocated_addrs;
    g_ipv6_pool.allocated_addrs = new_node;
    
    // 递增计数器
    g_ipv6_pool.next_addr_counter++;
    
    // 解锁
    pthread_mutex_unlock(&g_ipv6_pool.lock);
    
    return true;
}

#ifdef _WIN32
// Windows平台实现：检查系统中是否已存在指定的IPv6地址
static bool is_ipv6_address_exists(int nic_index, const char *ipv6_address)
{
    return ipv6_interface_cache_contains(nic_index, ipv6_address);

    DWORD dwRetVal = 0;
    IP_ADAPTER_ADDRESSES *adapter_addresses = NULL;
    ULONG out_buf_len = 0;
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    IP_ADAPTER_ADDRESSES *adapter = NULL;
    SOCKADDR_IN6 addr;

    // 初始化Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        log_error("WSAStartup failed: %d", WSAGetLastError());
        return false; // 发生错误时，假设地址不存在
    }

    // 获取适配器信息
    if (GetAdaptersAddresses(AF_INET6, flags, NULL, NULL, &out_buf_len) == ERROR_BUFFER_OVERFLOW) {
        adapter_addresses = (IP_ADAPTER_ADDRESSES *)malloc(out_buf_len);
        if (adapter_addresses == NULL) {
            log_error("Memory allocation failed");
            WSACleanup();
            return false; // 发生错误时，假设地址不存在
        }
    }

    if ((dwRetVal = GetAdaptersAddresses(AF_INET6, flags, NULL, adapter_addresses, &out_buf_len)) != NO_ERROR) {
        log_error("GetAdaptersAddresses failed: %d", dwRetVal);
        free(adapter_addresses);
        WSACleanup();
        return false; // 发生错误时，假设地址不存在
    }

    // 查找指定索引的网络接口
    adapter = adapter_addresses;
    while (adapter) {
        if (adapter->IfIndex == nic_index) {
            // 解析要检查的IPv6地址
            memset(&addr, 0, sizeof(addr));
            addr.sin6_family = AF_INET6;
            if (InetPton(AF_INET6, ipv6_address, &addr.sin6_addr) != 1) {
                log_error("Invalid IPv6 address: %s", ipv6_address);
                free(adapter_addresses);
                WSACleanup();
                return false;
            }

            // 检查接口上是否已存在该地址
            IP_ADAPTER_UNICAST_ADDRESS_LH *unicast_addr = adapter->FirstUnicastAddress;
            while (unicast_addr) {
                if (unicast_addr->Address.lpSockaddr->sa_family == AF_INET6) {
                    SOCKADDR_IN6 *current_addr = (SOCKADDR_IN6 *)unicast_addr->Address.lpSockaddr;
                    if (memcmp(&current_addr->sin6_addr, &addr.sin6_addr, sizeof(current_addr->sin6_addr)) == 0) {
                        free(adapter_addresses);
                        WSACleanup();
                        return true; // 地址已存在
                    }
                }
                unicast_addr = unicast_addr->Next;
            }
            break;
        }
        adapter = adapter->Next;
    }

    free(adapter_addresses);
    WSACleanup();
    return false; // 地址不存在
}

static bool ipv6_remove_address_from_interface_cached(int nic_index, const char *ipv6_address);

static bool ipv6_add_address_to_interface_cached(int nic_index, const char *ipv6_address, int prefix_len)
{
    char canonical_addr[40];
    bool already_exists = false;

    if (!canonicalize_ipv6_address(ipv6_address, canonical_addr, sizeof(canonical_addr))) {
        log_error("Invalid IPv6 address: %s", ipv6_address ? ipv6_address : "(null)");
        return false;
    }

    if (!create_unicast_ipv6_address(nic_index, canonical_addr, prefix_len, &already_exists)) {
        return false;
    }

    if (!already_exists) {
        log_info("IPv6 address %s/%d added to interface %d",
                 canonical_addr, prefix_len, nic_index);
        if (!remember_interface_address_added_by_program(canonical_addr)) {
            log_warn("Failed to remember IPv6 address %s for interface cleanup",
                     canonical_addr);
        }
        ipv6_manager_note_address_added();
    }
    return true;
}

// Windows平台实现：添加IPv6地址到网络接口
bool ipv6_add_address_to_interface(int nic_index, const char *ipv6_address, int prefix_len)
{
    return ipv6_add_address_to_interface_cached(nic_index, ipv6_address, prefix_len);
#if 0

    DWORD dwRetVal = 0;
    IP_ADAPTER_ADDRESSES *adapter_addresses = NULL;
    ULONG out_buf_len = 0;
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    IP_ADAPTER_ADDRESSES *adapter = NULL;
    SOCKADDR_IN6 addr;
    
    // 初始化Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        log_error("WSAStartup failed: %d", WSAGetLastError());
        return false;
    }

    // 获取适配器信息
    if (GetAdaptersAddresses(AF_INET6, flags, NULL, NULL, &out_buf_len) == ERROR_BUFFER_OVERFLOW) {
        adapter_addresses = (IP_ADAPTER_ADDRESSES *)malloc(out_buf_len);
        if (adapter_addresses == NULL) {
            log_error("Memory allocation failed");
            WSACleanup();
            return false;
        }
    }

    if ((dwRetVal = GetAdaptersAddresses(AF_INET6, flags, NULL, adapter_addresses, &out_buf_len)) != NO_ERROR) {
        log_error("GetAdaptersAddresses failed: %d", dwRetVal);
        free(adapter_addresses);
        WSACleanup();
        return false;
    }

    // 查找指定索引的网络接口
    adapter = adapter_addresses;
    while (adapter) {
        if (adapter->IfIndex == nic_index) {
            break;
        }
        adapter = adapter->Next;
    }

    if (adapter == NULL) {
        log_error("Network interface with index %d not found", nic_index);
        free(adapter_addresses);
        WSACleanup();
        return false;
    }

    // 解析IPv6地址
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    if (InetPton(AF_INET6, ipv6_address, &addr.sin6_addr) != 1) {
        log_error("Invalid IPv6 address: %s", ipv6_address);
        free(adapter_addresses);
        WSACleanup();
        return false;
    }
    
    // 由于AddIPAddress只支持IPv4，我们使用netsh命令来添加IPv6地址
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "netsh interface ipv6 add address %d %s/%d", nic_index, ipv6_address, prefix_len);
    
    // 执行命令
    log_debug("Executing system command: %s", cmd);
    int result = system(cmd);
    log_debug("System command exit code: %d, command: %s", result, cmd);
    if (result != 0) {
        record_netsh_add_result(result);
        if (should_retry_netsh_add()) {
            log_warn("IPv6 netsh add failed, will retry once after 2000 ms. Exit code: %d, command: %s",
                     result, cmd);
            sleep_before_netsh_add_retry();
            log_debug("Retrying system command: %s", cmd);
            result = system(cmd);
            log_debug("System command retry exit code: %d, command: %s", result, cmd);
            record_netsh_add_result(result);
        } else {
            log_warn("Skipping IPv6 netsh add retry after consecutive failures. Exit code: %d, command: %s",
                     result, cmd);
        }
    } else {
        record_netsh_add_result(result);
    }
    if (result != 0) {
        log_error("Failed to add IPv6 address using netsh, exit code: %d, command: %s", result, cmd);
        free(adapter_addresses);
        WSACleanup();
        return false;
    }

    add_address_to_interface_allocated(ipv6_address);

    free(adapter_addresses);
    WSACleanup();
    return true;
#endif
}

static bool ipv6_remove_address_from_interface_cached(int nic_index, const char *ipv6_address)
{
    char canonical_addr[40];

    if (!canonicalize_ipv6_address(ipv6_address, canonical_addr, sizeof(canonical_addr))) {
        log_error("Invalid IPv6 address: %s", ipv6_address ? ipv6_address : "(null)");
        return false;
    }

    if (!delete_unicast_ipv6_address(nic_index, canonical_addr)) {
        return false;
    }

    remove_address_from_interface_allocated(canonical_addr);
    log_debug("Removed IPv6 address %s from interface %d", canonical_addr, nic_index);
    return true;
}

// Windows平台实现：从网络接口删除IPv6地址
bool ipv6_remove_address_from_interface(int nic_index, const char *ipv6_address)
{
    return ipv6_remove_address_from_interface_cached(nic_index, ipv6_address);
#if 0

    DWORD dwRetVal = 0;
    IP_ADAPTER_ADDRESSES *adapter_addresses = NULL;
    ULONG out_buf_len = 0;
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    IP_ADAPTER_ADDRESSES *adapter = NULL;
    SOCKADDR_IN6 addr;

    // 初始化Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        log_error("WSAStartup failed: %d", WSAGetLastError());
        return false;
    }

    // 获取适配器信息
    if (GetAdaptersAddresses(AF_INET6, flags, NULL, NULL, &out_buf_len) == ERROR_BUFFER_OVERFLOW) {
        adapter_addresses = (IP_ADAPTER_ADDRESSES *)malloc(out_buf_len);
        if (adapter_addresses == NULL) {
            log_error("Memory allocation failed");
            WSACleanup();
            return false;
        }
    }

    if ((dwRetVal = GetAdaptersAddresses(AF_INET6, flags, NULL, adapter_addresses, &out_buf_len)) != NO_ERROR) {
        log_error("GetAdaptersAddresses failed: %d", dwRetVal);
        free(adapter_addresses);
        WSACleanup();
        return false;
    }

    // 查找指定索引的网络接口
    adapter = adapter_addresses;
    while (adapter) {
        if (adapter->IfIndex == nic_index) {
            break;
        }
        adapter = adapter->Next;
    }

    if (adapter == NULL) {
        log_error("Network interface with index %d not found", nic_index);
        free(adapter_addresses);
        WSACleanup();
        return false;
    }

    // 解析要删除的IPv6地址
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    if (InetPton(AF_INET6, ipv6_address, &addr.sin6_addr) != 1) {
        log_error("Invalid IPv6 address: %s", ipv6_address);
        free(adapter_addresses);
        WSACleanup();
        return false;
    }

    // 由于DeleteIPAddress只支持IPv4，我们使用netsh命令来删除IPv6地址
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "netsh interface ipv6 delete address %d %s", nic_index, ipv6_address);
    
    // 执行命令
    log_debug("Executing system command: %s", cmd);
    int result = system(cmd);
    log_debug("System command exit code: %d, command: %s", result, cmd);
    if (result != 0) {
        log_error("Failed to delete IPv6 address using netsh, exit code: %d, command: %s", result, cmd);
        free(adapter_addresses);
        WSACleanup();
        return false;
    }
    
    log_debug("Removed IPv6 address %s from interface %d", ipv6_address, nic_index);

    free(adapter_addresses);
    WSACleanup();
    return true;
#endif
}
#else
// Linux平台实现：添加IPv6地址到网络接口
bool ipv6_add_address_to_interface(int nic_index, const char *ipv6_address, int prefix_len)
{
    char ifname[16];
    struct ifreq ifr;
    struct sockaddr_in6 addr;
    int sockfd;

    // 查找网络接口名称
    snprintf(ifname, sizeof(ifname), "eth%d", nic_index);

    // 创建套接字
    sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return false;
    }

    // 配置IPv6地址
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    if (inet_pton(AF_INET6, ipv6_address, &addr.sin6_addr) != 1) {
        log_error("Invalid IPv6 address: %s", ipv6_address);
        close(sockfd);
        return false;
    }

    // 设置接口名称
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ);
    ifr.ifr_name[IFNAMSIZ-1] = '\0';

    // 设置IPv6地址
    memcpy(&ifr.ifr_addr, &addr, sizeof(struct sockaddr_in6));

    // 添加IPv6地址
    if (ioctl(sockfd, SIOCSIFADDR, &ifr) < 0) {
        perror("ioctl SIOCSIFADDR");
        close(sockfd);
        return false;
    }

    close(sockfd);
    return true;
}

// Linux平台实现：从网络接口删除IPv6地址
bool ipv6_remove_address_from_interface(int nic_index, const char *ipv6_address)
{
    char ifname[16];
    struct ifreq ifr;
    struct sockaddr_in6 addr;
    int sockfd;

    // 查找网络接口名称
    snprintf(ifname, sizeof(ifname), "eth%d", nic_index);

    // 创建套接字
    sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return false;
    }

    // 配置IPv6地址
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    if (inet_pton(AF_INET6, ipv6_address, &addr.sin6_addr) != 1) {
        log_error("Invalid IPv6 address: %s", ipv6_address);
        close(sockfd);
        return false;
    }

    // 设置接口名称
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ);
    ifr.ifr_name[IFNAMSIZ-1] = '\0';

    // 设置IPv6地址
    memcpy(&ifr.ifr_addr, &addr, sizeof(struct sockaddr_in6));

    // 删除IPv6地址
    if (ioctl(sockfd, SIOCDELIFADDR, &ifr) < 0) {
        perror("ioctl SIOCDELIFADDR");
        close(sockfd);
        return false;
    }

    close(sockfd);
    return true;
}
#endif

// 为设备分配IPv6地址
bool ipv6_allocate_address(const char *device_name, int nic_index, char *ipv6_address)
{
    if (!device_name || !ipv6_address) {
        return false;
    }

    // 生成唯一的IPv6地址
    if (!generate_ipv6_address(device_name, ipv6_address, 40)) {
        return false;
    }

    // 检查地址是否已经存在于系统中
    // The generated pool address is already in allocated_addrs; keep it unless add fails.
    if (!ipv6_add_address_to_interface(nic_index, ipv6_address, g_ipv6_pool.prefix_len)) {
        remove_address_from_allocated(ipv6_address);
        return false;
    }

    add_device_ipv6_mapping(device_name, ipv6_address);
    log_info("Successfully allocated IPv6 address %s for device %s on interface %d",
            ipv6_address, device_name, nic_index);
    return true;

#if 0
    bool address_exists = false;
    
    #ifdef _WIN32
    // Windows平台检查
    address_exists = is_ipv6_address_exists(nic_index, ipv6_address);
    #else
    // Linux平台检查（可以实现类似的检查）
    // 这里简化处理，假设Linux平台不会出现这种情况
    address_exists = false;
    #endif
    
    if (address_exists) {
        // 如果地址已经存在，仍然将其添加到已分配列表中（尽管它已经在那里了）
        log_info("IPv6 address %s already exists on interface %d, skipping addition",
                ipv6_address, nic_index);
    } else {
        // 将IPv6地址添加到指定的网络接口
        if (!ipv6_add_address_to_interface(nic_index, ipv6_address, g_ipv6_pool.prefix_len)) {
            // 如果添加失败，从已分配列表中删除该地址
            remove_address_from_allocated(ipv6_address);
            return false;
        }
    }

    // 添加设备-IPv6映射
    add_device_ipv6_mapping(device_name, ipv6_address);
    
    log_info("Successfully allocated IPv6 address %s for device %s on interface %d",
            ipv6_address, device_name, nic_index);
    return true;
#endif
}

// 释放设备的IPv6地址
bool ipv6_release_address(const char *device_name, int nic_index, const char *ipv6_address) // 修改测试
{
    if (!device_name || !ipv6_address) {
        return false;
    }

    // 从网络接口删除IPv6地址
    if (!ipv6_remove_address_from_interface(nic_index, ipv6_address)) {
        return false;
    }

    // 从已分配列表中删除地址
    remove_address_from_allocated(ipv6_address);
    
    // 删除设备-IPv6映射
    remove_device_ipv6_mapping(device_name);

    log_info("Successfully released IPv6 address %s from device %s on interface %d",
            ipv6_address, device_name, nic_index);
    return true;
}

// 初始化IPv6地址管理器
void ipv6_remove_all_allocated_addresses(int nic_index)
{
    AllocatedAddrNode *cleanup_addrs = NULL;
    AllocatedAddrNode *current = NULL;

    if (nic_index <= 0) {
        return;
    }

    size_t cleanup_count = 0;
    size_t success_count = 0;
    size_t failure_count = 0;
#ifdef _WIN32
    ULONGLONG cleanup_start_ms = GetTickCount64();
#endif
    char start_addr[40] = {0};
    char end_addr[40] = {0};

    pthread_mutex_lock(&g_ipv6_pool.lock);
    strncpy(start_addr, g_ipv6_pool.start_addr, sizeof(start_addr) - 1);
    start_addr[sizeof(start_addr) - 1] = '\0';
    strncpy(end_addr, g_ipv6_pool.end_addr, sizeof(end_addr) - 1);
    end_addr[sizeof(end_addr) - 1] = '\0';

    current = g_ipv6_pool.interface_addrs;
    while (current != NULL) {
        if (!addr_list_add_unique(&cleanup_addrs, current->addr)) {
            log_error("Failed to add interface IPv6 cleanup snapshot address %s", current->addr);
        }
        current = current->next;
    }

    current = g_ipv6_pool.allocated_addrs;
    while (current != NULL) {
        if (!addr_list_add_unique(&cleanup_addrs, current->addr)) {
            log_error("Failed to add allocated IPv6 cleanup snapshot address %s", current->addr);
        }
        current = current->next;
    }
    pthread_mutex_unlock(&g_ipv6_pool.lock);

#ifdef _WIN32
    if (start_addr[0] != '\0' && end_addr[0] != '\0') {
        pthread_mutex_lock(&g_ipv6_interface_cache.lock);
        if (refresh_ipv6_interface_cache_locked(nic_index)) {
            current = g_ipv6_interface_cache.system_addrs;
            while (current != NULL) {
                if (ipv6_address_in_range(current->addr, start_addr, end_addr)) {
                    if (!addr_list_add_unique(&cleanup_addrs, current->addr)) {
                        log_error("Failed to add pool-range IPv6 cleanup snapshot address %s",
                                  current->addr);
                    }
                }
                current = current->next;
            }
        } else {
            log_warn("Failed to refresh IPv6 addresses on interface %d for pool cleanup",
                     nic_index);
        }
        pthread_mutex_unlock(&g_ipv6_interface_cache.lock);
    } else {
        log_warn("IPv6 pool range is empty, only removing addresses recorded by this process");
    }
#endif

    current = cleanup_addrs;
    while (current != NULL) {
        cleanup_count++;
        current = current->next;
    }

    if (start_addr[0] != '\0' && end_addr[0] != '\0') {
        log_info("Cleaning %zu IPv6 addresses from interface %d in pool range %s - %s",
                 cleanup_count, nic_index, start_addr, end_addr);
    } else {
        log_info("Cleaning %zu tracked IPv6 addresses from interface %d",
                 cleanup_count, nic_index);
    }

    current = cleanup_addrs;
    while (current != NULL) {
        AllocatedAddrNode *next = current->next;

        if (ipv6_remove_address_from_interface(nic_index, current->addr)) {
            success_count++;
            log_debug("Removed allocated IPv6 address %s from interface %d",
                      current->addr, nic_index);
        } else {
            failure_count++;
            log_warn("Failed to remove allocated IPv6 address %s from interface %d",
                     current->addr, nic_index);
        }

        current = next;
    }

    addr_list_clear(&cleanup_addrs);

    pthread_mutex_lock(&g_ipv6_pool.lock);
    addr_list_clear(&g_ipv6_pool.allocated_addrs);
    addr_list_clear(&g_ipv6_pool.interface_addrs);
    pthread_mutex_unlock(&g_ipv6_pool.lock);

#ifdef _WIN32
    log_info("IPv6 allocation cache cleanup finished: total=%zu success=%zu failure=%zu elapsed=%llu ms",
             cleanup_count, success_count, failure_count,
             (unsigned long long)(GetTickCount64() - cleanup_start_ms));
#else
    log_info("IPv6 allocation cache cleanup finished: total=%zu success=%zu failure=%zu",
             cleanup_count, success_count, failure_count);
#endif
    return;

#if 0
    pthread_mutex_lock(&g_ipv6_pool.lock);

    strncpy(start_addr, g_ipv6_pool.start_addr, sizeof(start_addr) - 1);
    start_addr[sizeof(start_addr) - 1] = '\0';
    strncpy(end_addr, g_ipv6_pool.end_addr, sizeof(end_addr) - 1);
    end_addr[sizeof(end_addr) - 1] = '\0';

    current = g_ipv6_pool.interface_addrs;
    while (current != NULL) {
        if (!addr_list_add_unique(&cleanup_addrs, current->addr)) {
            log_error("Failed to add IPv6 cleanup snapshot address %s", current->addr);
        }
        current = current->next;
    }

    pthread_mutex_unlock(&g_ipv6_pool.lock);

#ifdef _WIN32
    if (start_addr[0] != '\0' && end_addr[0] != '\0') {
        pthread_mutex_lock(&g_ipv6_interface_cache.lock);
        if (refresh_ipv6_interface_cache_locked(nic_index)) {
            current = g_ipv6_interface_cache.system_addrs;
            while (current != NULL) {
                if (ipv6_address_in_range(current->addr, start_addr, end_addr)) {
                    if (!addr_list_add_unique(&cleanup_addrs, current->addr)) {
                        log_error("Failed to add IPv6 pool cleanup snapshot address %s",
                                  current->addr);
                    }
                }
                current = current->next;
            }
        } else {
            log_warn("Failed to refresh IPv6 addresses on interface %d for pool cleanup",
                     nic_index);
        }
        pthread_mutex_unlock(&g_ipv6_interface_cache.lock);
    } else {
        log_warn("IPv6 pool range is empty, only removing addresses recorded by this process");
    }
#endif

    if (start_addr[0] != '\0' && end_addr[0] != '\0') {
        log_info("Cleaning IPv6 addresses on interface %d in pool range %s - %s",
                 nic_index, start_addr, end_addr);
    }

    current = cleanup_addrs;
    while (current != NULL) {
        AllocatedAddrNode *next = current->next;

        if (ipv6_remove_address_from_interface(nic_index, current->addr)) {
            log_debug("Removed allocated IPv6 address %s from interface %d",
                      current->addr, nic_index);
        } else {
            log_warn("Failed to remove allocated IPv6 address %s from interface %d",
                     current->addr, nic_index);
        }

        current = next;
    }

    addr_list_clear(&cleanup_addrs);

    pthread_mutex_lock(&g_ipv6_pool.lock);
    while (g_ipv6_pool.interface_addrs != NULL) {
        AllocatedAddrNode *temp = g_ipv6_pool.interface_addrs;
        g_ipv6_pool.interface_addrs = temp->next;
        free(temp);
    }
    pthread_mutex_unlock(&g_ipv6_pool.lock);
#endif
}

bool ipv6_manager_init(const char *start_addr, const char *end_addr, int prefix_len)
{
    if (!start_addr || !end_addr) {
        return false;
    }

#ifdef _WIN32
    reset_ipv6_interface_cache();
#endif

    // 初始化互斥锁
    pthread_mutex_lock(&g_ipv6_add_options.lock);
    g_ipv6_add_options.added_since_last_delay = 0;
    g_ipv6_add_options.delay_pending = false;
    pthread_mutex_unlock(&g_ipv6_add_options.lock);

    if (pthread_mutex_init(&g_ipv6_pool.lock, NULL) != 0) {
        log_error("Failed to initialize mutex for IPv6 address pool");
        return false;
    }

    // 加锁保护
    pthread_mutex_lock(&g_ipv6_pool.lock);

    // 清空已分配地址列表
    while (g_ipv6_pool.allocated_addrs != NULL) {
        AllocatedAddrNode *temp = g_ipv6_pool.allocated_addrs;
        g_ipv6_pool.allocated_addrs = temp->next;
        free(temp);
    }

    while (g_ipv6_pool.interface_addrs != NULL) {
        AllocatedAddrNode *temp = g_ipv6_pool.interface_addrs;
        g_ipv6_pool.interface_addrs = temp->next;
        free(temp);
    }

    // 重置计数器
    g_ipv6_pool.next_addr_counter = 0;

    // 设置地址池范围
    strncpy(g_ipv6_pool.start_addr, start_addr, sizeof(g_ipv6_pool.start_addr) - 1);
    g_ipv6_pool.start_addr[sizeof(g_ipv6_pool.start_addr) - 1] = '\0';
    strncpy(g_ipv6_pool.end_addr, end_addr, sizeof(g_ipv6_pool.end_addr) - 1);
    g_ipv6_pool.end_addr[sizeof(g_ipv6_pool.end_addr) - 1] = '\0';
    g_ipv6_pool.prefix_len = prefix_len;

    // 解锁
    pthread_mutex_unlock(&g_ipv6_pool.lock);

    log_info("IPv6 address manager initialized, address range from %s/%d to %s/%d",
            start_addr, prefix_len, end_addr, prefix_len);
    return true;
}

// 清理IPv6地址管理器
void ipv6_manager_cleanup(void)
{
#ifdef _WIN32
    reset_ipv6_interface_cache();
#endif

    // 加锁保护
    pthread_mutex_lock(&g_ipv6_pool.lock);

    // 清空已分配地址列表
    while (g_ipv6_pool.allocated_addrs != NULL) {
        AllocatedAddrNode *temp = g_ipv6_pool.allocated_addrs;
        g_ipv6_pool.allocated_addrs = temp->next;
        free(temp);
    }

    while (g_ipv6_pool.interface_addrs != NULL) {
        AllocatedAddrNode *temp = g_ipv6_pool.interface_addrs;
        g_ipv6_pool.interface_addrs = temp->next;
        free(temp);
    }

    // 解锁
    pthread_mutex_unlock(&g_ipv6_pool.lock);

    // 销毁互斥锁
    pthread_mutex_destroy(&g_ipv6_pool.lock);

    // 清理资源
    memset(&g_ipv6_pool, 0, sizeof(g_ipv6_pool));
    log_info("IPv6 address manager cleaned up");
}
