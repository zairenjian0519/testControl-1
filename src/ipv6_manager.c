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
#include <iphlpapi.h>
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
    uint32_t next_addr_counter;           // 下一个要分配的地址计数器
    pthread_mutex_t lock;                 // 互斥锁，保护地址池的并发访问
} IPv6AddressPool;

static IPv6AddressPool g_ipv6_pool = {0};

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
    
    strcpy(new_node->addr, addr);
    
    // 加锁保护
    pthread_mutex_lock(&g_ipv6_pool.lock);
    
    new_node->next = g_ipv6_pool.allocated_addrs;
    g_ipv6_pool.allocated_addrs = new_node;
    
    // 解锁
    pthread_mutex_unlock(&g_ipv6_pool.lock);
    
    return true;
}

// 从已分配列表中删除地址
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
    
    strcpy(new_node->addr, ipv6_address);
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

// Windows平台实现：添加IPv6地址到网络接口
bool ipv6_add_address_to_interface(int nic_index, const char *ipv6_address, int prefix_len)
{
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
    sprintf(cmd, "netsh interface ipv6 add address %d %s/%d", nic_index, ipv6_address, prefix_len);
    
    // 执行命令
    int result = system(cmd);
    if (result != 0) {
        log_error("Failed to add IPv6 address using netsh: %d", result);
        free(adapter_addresses);
        WSACleanup();
        return false;
    }

    free(adapter_addresses);
    WSACleanup();
    return true;
}

// Windows平台实现：从网络接口删除IPv6地址
bool ipv6_remove_address_from_interface(int nic_index, const char *ipv6_address)
{
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
    sprintf(cmd, "netsh interface ipv6 delete address %d %s", nic_index, ipv6_address);
    
    // 执行命令
    int result = system(cmd);
    if (result != 0) {
        log_error("Failed to delete IPv6 address using netsh: %d", result);
        free(adapter_addresses);
        WSACleanup();
        return false;
    }
    
    log_info("Removed IPv6 address %s from interface %d", ipv6_address, nic_index);

    free(adapter_addresses);
    WSACleanup();
    return true;
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

    log_info("Successfully allocated IPv6 address %s for device %s on interface %d",
            ipv6_address, device_name, nic_index);
    return true;
}

// 释放设备的IPv6地址
bool ipv6_release_address(const char *device_name, int nic_index, const char *ipv6_address)
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

    log_info("Successfully released IPv6 address %s from device %s on interface %d",
            ipv6_address, device_name, nic_index);
    return true;
}

// 初始化IPv6地址管理器
bool ipv6_manager_init(const char *start_addr, const char *end_addr, int prefix_len)
{
    if (!start_addr || !end_addr) {
        return false;
    }

    // 初始化互斥锁
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

    // 重置计数器
    g_ipv6_pool.next_addr_counter = 0;

    // 设置地址池范围
    strncpy(g_ipv6_pool.start_addr, start_addr, sizeof(g_ipv6_pool.start_addr));
    strncpy(g_ipv6_pool.end_addr, end_addr, sizeof(g_ipv6_pool.end_addr));
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
    // 加锁保护
    pthread_mutex_lock(&g_ipv6_pool.lock);

    // 清空已分配地址列表
    while (g_ipv6_pool.allocated_addrs != NULL) {
        AllocatedAddrNode *temp = g_ipv6_pool.allocated_addrs;
        g_ipv6_pool.allocated_addrs = temp->next;
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
