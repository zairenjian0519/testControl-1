#define _WIN32_WINNT 0x0600

#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <atomic>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <signal.h>
#include <sstream>
#include <string>
#include <thread>

#include "cJSON.h"
#include "ipv6_manager.h"
#include "log_manager.h"

extern std::atomic_bool g_main_loop_running;
SOCKET g_udp_socket = INVALID_SOCKET;

void request_program_stop(void)
{
    g_main_loop_running.store(false);
}

typedef struct {
    bool enable_log;
    std::string log_level;
    std::string log_file;
    int log_file_count;
    long log_file_size;
} LogConfigFromFile;

typedef struct {
    std::string name;
    std::string model;
    int opcua_port;
    std::string opcua_path;
} DeviceConfigFromFile;

typedef struct {
    int listen_port;
    std::string multicast_ip;
    int nic_index;
    std::string ipv6_start_address;
    std::string ipv6_end_address;
    int ipv6_prefix_length;
    int ipv6_max_lease_count;
    bool ipv6_skip_as_source;
    unsigned int ipv6_batch_add_limit;
    unsigned int ipv6_batch_add_delay_ms;
    DeviceConfigFromFile device_config;
    LogConfigFromFile log_config;
} Ipv6MulticastConfig;

Ipv6MulticastConfig read_ipv6_config(const std::string& config_file)
{
    Ipv6MulticastConfig config;
    config.listen_port = 0;
    config.multicast_ip = "";
    config.nic_index = 0;
    config.ipv6_start_address = "";
    config.ipv6_end_address = "";
    config.ipv6_prefix_length = 0;
    config.ipv6_max_lease_count = 0;
    config.ipv6_skip_as_source = true;
    config.ipv6_batch_add_limit = 300;
    config.ipv6_batch_add_delay_ms = 4000;
    config.device_config.name = "spssps";
    config.device_config.model = "ATB-5000";
    config.device_config.opcua_port = 4840;
    config.device_config.opcua_path = "/autbus/controller";
    config.log_config.enable_log = false;
    config.log_config.log_level = "INFO";
    config.log_config.log_file = "application.log";
    config.log_config.log_file_count = 2;
    config.log_config.log_file_size = 1024 * 1024;

    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cerr << "Failed to open configuration file: " << config_file << std::endl;
        return config;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    cJSON *root = cJSON_Parse(content.c_str());
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            std::cerr << "JSON parsing error: " << error_ptr << std::endl;
        }
        cJSON_Delete(root);
        return config;
    }

    cJSON *device = cJSON_GetObjectItem(root, "device");
    if (device != NULL) {
        cJSON *name = cJSON_GetObjectItem(device, "name");
        if (name != NULL && cJSON_IsString(name) && name->valuestring[0] != '\0') {
            config.device_config.name = name->valuestring;
        }

        cJSON *model = cJSON_GetObjectItem(device, "model");
        if (model != NULL && cJSON_IsString(model) && model->valuestring[0] != '\0') {
            config.device_config.model = model->valuestring;
        }

        cJSON *opcua_port = cJSON_GetObjectItem(device, "opcua_port");
        if (opcua_port != NULL && cJSON_IsNumber(opcua_port)) {
            config.device_config.opcua_port = opcua_port->valueint;
        }

        cJSON *opcua_path = cJSON_GetObjectItem(device, "opcua_path");
        if (opcua_path != NULL && cJSON_IsString(opcua_path) && opcua_path->valuestring[0] != '\0') {
            config.device_config.opcua_path = opcua_path->valuestring;
        }
    }

    cJSON *ipv6_multicast = cJSON_GetObjectItem(root, "ipv6_multicast");
    if (ipv6_multicast != NULL) {
        cJSON *port = cJSON_GetObjectItem(ipv6_multicast, "listen_port");
        if (port != NULL && cJSON_IsNumber(port)) {
            config.listen_port = port->valueint;
        }

        cJSON *ip = cJSON_GetObjectItem(ipv6_multicast, "multicast_ip");
        if (ip != NULL && cJSON_IsString(ip)) {
            config.multicast_ip = ip->valuestring;
        }

        cJSON *index = cJSON_GetObjectItem(ipv6_multicast, "nic_index");
        if (index != NULL && cJSON_IsNumber(index)) {
            config.nic_index = index->valueint;
        }
    }

    cJSON *ipv6_pool = cJSON_GetObjectItem(root, "ipv6_address_pool");
    if (ipv6_pool != NULL) {
        cJSON *start_address = cJSON_GetObjectItem(ipv6_pool, "start_address");
        if (start_address != NULL && cJSON_IsString(start_address)) {
            config.ipv6_start_address = start_address->valuestring;
        }

        cJSON *end_address = cJSON_GetObjectItem(ipv6_pool, "end_address");
        if (end_address != NULL && cJSON_IsString(end_address)) {
            config.ipv6_end_address = end_address->valuestring;
        }

        cJSON *prefix_length = cJSON_GetObjectItem(ipv6_pool, "prefix_length");
        if (prefix_length != NULL && cJSON_IsNumber(prefix_length)) {
            config.ipv6_prefix_length = prefix_length->valueint;
        }

        cJSON *max_lease_count = cJSON_GetObjectItem(ipv6_pool, "max_lease_count");
        if (max_lease_count != NULL && cJSON_IsNumber(max_lease_count)) {
            config.ipv6_max_lease_count = max_lease_count->valueint;
        }

        cJSON *skip_as_source = cJSON_GetObjectItem(ipv6_pool, "skip_as_source");
        if (skip_as_source != NULL && cJSON_IsBool(skip_as_source)) {
            config.ipv6_skip_as_source = cJSON_IsTrue(skip_as_source);
        }

        cJSON *batch_add_limit = cJSON_GetObjectItem(ipv6_pool, "batch_add_limit");
        if (batch_add_limit != NULL && cJSON_IsNumber(batch_add_limit) && batch_add_limit->valueint >= 0) {
            config.ipv6_batch_add_limit = static_cast<unsigned int>(batch_add_limit->valueint);
        }

        cJSON *batch_add_delay_ms = cJSON_GetObjectItem(ipv6_pool, "batch_add_delay_ms");
        if (batch_add_delay_ms != NULL && cJSON_IsNumber(batch_add_delay_ms) && batch_add_delay_ms->valueint >= 0) {
            config.ipv6_batch_add_delay_ms = static_cast<unsigned int>(batch_add_delay_ms->valueint);
        }
    }

    cJSON *log_config = cJSON_GetObjectItem(root, "log");
    if (log_config != NULL) {
        cJSON *enable_log = cJSON_GetObjectItem(log_config, "enable_log");
        if (enable_log != NULL && cJSON_IsBool(enable_log)) {
            config.log_config.enable_log = cJSON_IsTrue(enable_log);
        }

        cJSON *log_level = cJSON_GetObjectItem(log_config, "log_level");
        if (log_level != NULL && cJSON_IsString(log_level)) {
            config.log_config.log_level = log_level->valuestring;
        }

        cJSON *log_file = cJSON_GetObjectItem(log_config, "log_file");
        if (log_file != NULL && cJSON_IsString(log_file)) {
            config.log_config.log_file = log_file->valuestring;
        }

        cJSON *log_file_count = cJSON_GetObjectItem(log_config, "log_file_count");
        if (log_file_count != NULL && cJSON_IsNumber(log_file_count)) {
            config.log_config.log_file_count = log_file_count->valueint;
            if (config.log_config.log_file_count < 1) {
                config.log_config.log_file_count = 1;
            }
        }

        cJSON *log_file_size = cJSON_GetObjectItem(log_config, "log_file_size");
        if (log_file_size != NULL && cJSON_IsNumber(log_file_size)) {
            config.log_config.log_file_size = log_file_size->valueint;
            if (config.log_config.log_file_size < 1024) {
                config.log_config.log_file_size = 1024;
            }
        }
    }

    cJSON_Delete(root);
    return config;
}

static void log_ipv6_config_debug(const Ipv6MulticastConfig& config)
{
    log_debug("=== Parsed IPv6 configuration ===");
    log_debug("ipv6_multicast.listen_port: %d", config.listen_port);
    log_debug("ipv6_multicast.multicast_ip: %s", config.multicast_ip.c_str());
    log_debug("ipv6_multicast.nic_index: %d", config.nic_index);
    log_debug("ipv6_address_pool.start_address: %s", config.ipv6_start_address.c_str());
    log_debug("ipv6_address_pool.end_address: %s", config.ipv6_end_address.c_str());
    log_debug("ipv6_address_pool.prefix_length: %d", config.ipv6_prefix_length);
    log_debug("ipv6_address_pool.max_lease_count: %d", config.ipv6_max_lease_count);
    log_debug("ipv6_address_pool.skip_as_source: %s", config.ipv6_skip_as_source ? "true" : "false");
    log_debug("ipv6_address_pool.batch_add_limit: %u", config.ipv6_batch_add_limit);
    log_debug("ipv6_address_pool.batch_add_delay_ms: %u", config.ipv6_batch_add_delay_ms);
    log_debug("device.name: %s", config.device_config.name.c_str());
    log_debug("device.model: %s", config.device_config.model.c_str());
    log_debug("device.opcua_port: %d", config.device_config.opcua_port);
    log_debug("device.opcua_path: %s", config.device_config.opcua_path.c_str());
    log_debug("log.enable_log: %s", config.log_config.enable_log ? "true" : "false");
    log_debug("log.log_level: %s", config.log_config.log_level.c_str());
    log_debug("log.log_file: %s", config.log_config.log_file.c_str());
    log_debug("log.log_file_count: %d", config.log_config.log_file_count);
    log_debug("log.log_file_size: %ld", config.log_config.log_file_size);
}

int main_loop(SOCKET serversocket, int nic_index, const char *device_name,
              const char *device_model, int opcua_port, const char *opcua_path);
extern "C" int opcua_server_main(const char *device_name);
extern "C" void opcua_server_stop(void);

static void stop_program(int signum)
{
    (void)signum;
    request_program_stop();
}

void print_help(const char* program_name)
{
    std::cout << "Usage: " << program_name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help          Show help information" << std::endl;
    std::cout << "  -l, --log           Enable log output" << std::endl;
    std::cout << "  -f, --log-file      Set log file path" << std::endl;
    std::cout << "  -v, --verbose       Enable verbose logging (DEBUG level)" << std::endl;
}

int main(int argc, char* argv[])
{
    int exit_code = 1;
    bool log_initialized = false;
    bool wsa_initialized = false;
    bool ipv6_manager_initialized = false;
    bool socket_created = false;
    bool opcua_thread_started = false;
    SOCKET l_nServer = INVALID_SOCKET;
    WSADATA wsaData;
    struct sockaddr_in6 addr;
    struct ipv6_mreq group;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    DWORD multicast_if = 0;
    GROUP_REQ group_req;
    struct sockaddr_in6 *group_addr = NULL;
    int join_error = 0;
    DWORD recv_timeout_ms = 1000;
    int reuse = 1;
    int opt = 0;
    int option_index = 0;
    std::thread opcua_thread;

    signal(SIGINT, stop_program);
    signal(SIGTERM, stop_program);

    Ipv6MulticastConfig config = read_ipv6_config("config.json");
    bool enable_log = config.log_config.enable_log;
    bool log_to_file = false;
    std::string log_file = config.log_config.log_file;
    LogLevel log_level = LOG_LEVEL_INFO;

    static struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"log", no_argument, NULL, 'l'},
        {"log-file", required_argument, NULL, 'f'},
        {"verbose", no_argument, NULL, 'v'},
        {NULL, 0, NULL, 0}
    };

    if (config.log_config.log_level == "DEBUG") {
        log_level = LOG_LEVEL_DEBUG;
    } else if (config.log_config.log_level == "INFO") {
        log_level = LOG_LEVEL_INFO;
    } else if (config.log_config.log_level == "WARN") {
        log_level = LOG_LEVEL_WARN;
    } else if (config.log_config.log_level == "ERROR") {
        log_level = LOG_LEVEL_ERROR;
    }

    while ((opt = getopt_long(argc, argv, "hlf:v", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h':
                print_help(argv[0]);
                return 0;
            case 'l':
                enable_log = true;
                break;
            case 'f':
                log_to_file = true;
                log_file = optarg;
                break;
            case 'v':
                log_level = LOG_LEVEL_DEBUG;
                break;
            default:
                print_help(argv[0]);
                return 1;
        }
    }

    if (enable_log && !log_to_file) {
        log_to_file = true;
    }

    LogConfig log_config;
    memset(&log_config, 0, sizeof(log_config));
    log_config.enable_log = enable_log;
    log_config.log_level = log_level;
    log_config.log_to_file = log_to_file;
    log_config.log_file = log_file.c_str();
    log_config.log_file_count = config.log_config.log_file_count;
    log_config.log_file_size = config.log_config.log_file_size;
    log_config.log_fp = NULL;

    if (!log_manager_init(&log_config)) {
        std::cerr << "Failed to initialize log manager" << std::endl;
        goto cleanup;
    }
    log_initialized = true;

    log_ipv6_config_debug(config);
    log_info("Program started, initializing Winsock...");

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        log_error("WSAStartup failed: %d", WSAGetLastError());
        goto cleanup;
    }
    wsa_initialized = true;

    if (config.listen_port == 0 || config.multicast_ip.empty() || config.nic_index == 0) {
        log_error("Invalid IPv6 multicast configuration, please check config.json");
        goto cleanup;
    }

    if (config.ipv6_start_address.empty() ||
        config.ipv6_end_address.empty() ||
        config.ipv6_prefix_length == 0) {
        log_error("IPv6 address pool configuration is invalid, please check config.json");
        goto cleanup;
    }

    if (!ipv6_manager_init(config.ipv6_start_address.c_str(),
                           config.ipv6_end_address.c_str(),
                           config.ipv6_prefix_length)) {
        log_error("Failed to initialize IPv6 manager");
        goto cleanup;
    }
    ipv6_manager_initialized = true;
    ipv6_manager_set_add_options(config.ipv6_skip_as_source,
                                  config.ipv6_batch_add_limit,
                                  config.ipv6_batch_add_delay_ms);

    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(config.listen_port);
    addr.sin6_addr = in6addr_any;

    l_nServer = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (l_nServer == INVALID_SOCKET) {
        perror("socket creation failed");
        log_error("Failed to create IPv6 UDP socket: %d", WSAGetLastError());
        goto cleanup;
    }
    socket_created = true;
    g_udp_socket = l_nServer;

    if (setsockopt(l_nServer, SOL_SOCKET, SO_REUSEADDR,
                   (const char*)&reuse, sizeof(reuse)) == SOCKET_ERROR) {
        log_error("setsockopt SO_REUSEADDR failed: %d", WSAGetLastError());
        goto cleanup;
    }

    if (bind(l_nServer, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_error("bind IPv6 multicast socket failed on port %d: %d",
                  config.listen_port, WSAGetLastError());
        goto cleanup;
    }

    memset(&group, 0, sizeof(group));
    group.ipv6mr_interface = config.nic_index;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    hints.ai_flags = AI_NUMERICHOST;

    if (getaddrinfo(config.multicast_ip.c_str(), NULL, &hints, &res) != 0) {
        log_error("getaddrinfo failed for %s", config.multicast_ip.c_str());
        goto cleanup;
    }

    memcpy(&group.ipv6mr_multiaddr,
           &((struct sockaddr_in6*)res->ai_addr)->sin6_addr,
           sizeof(group.ipv6mr_multiaddr));
    freeaddrinfo(res);
    res = NULL;

    multicast_if = (DWORD)config.nic_index;
    if (setsockopt(l_nServer, IPPROTO_IPV6, IPV6_MULTICAST_IF,
                   (const char*)&multicast_if, sizeof(multicast_if)) == SOCKET_ERROR) {
        log_warn("setsockopt IPV6_MULTICAST_IF failed for interface %d: %d",
                 config.nic_index, WSAGetLastError());
    }

    memset(&group_req, 0, sizeof(group_req));
    group_req.gr_interface = config.nic_index;
    group_addr = (struct sockaddr_in6 *)&group_req.gr_group;
    group_addr->sin6_family = AF_INET6;
    group_addr->sin6_addr = group.ipv6mr_multiaddr;
    group_addr->sin6_scope_id = config.nic_index;

    if (setsockopt(l_nServer, IPPROTO_IPV6, MCAST_JOIN_GROUP,
                   (char*)&group_req, sizeof(group_req)) == SOCKET_ERROR) {
        join_error = WSAGetLastError();
        log_warn("MCAST_JOIN_GROUP failed for %s on interface %d: %d",
                 config.multicast_ip.c_str(), config.nic_index, join_error);
    }

    if (join_error != 0 &&
        setsockopt(l_nServer, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                   (char*)&group, sizeof(group)) == SOCKET_ERROR) {
        log_warn("join IPv6 multicast group %s on interface %d failed: MCAST_JOIN_GROUP=%d, IPV6_JOIN_GROUP=%d",
                 config.multicast_ip.c_str(), config.nic_index, join_error, WSAGetLastError());
        log_warn("Continuing with UDP port listening only; multicast packets may not be received on this host.");
    }

    if (setsockopt(l_nServer, SOL_SOCKET, SO_RCVTIMEO,
                   (const char*)&recv_timeout_ms, sizeof(recv_timeout_ms)) == SOCKET_ERROR) {
        log_warn("setsockopt SO_RCVTIMEO failed: %d", WSAGetLastError());
    }

    log_info("Waiting for incoming data...");
    log_info("=== OPC UA server thread started ===");
    opcua_thread = std::thread(opcua_server_main, config.device_config.name.c_str());
    opcua_thread_started = true;

    log_info("\nListening for ADDP scan requests...");
    log_info("Multicast address: %s", config.multicast_ip.c_str());
    log_info("UDP port: %d", config.listen_port);
    log_info("Press Ctrl+C to exit\n");

    exit_code = main_loop(l_nServer,
                          config.nic_index,
                          config.device_config.name.c_str(),
                          config.device_config.model.c_str(),
                          config.device_config.opcua_port,
                          config.device_config.opcua_path.c_str());

cleanup:
    request_program_stop();
    opcua_server_stop();

    if (res != NULL) {
        freeaddrinfo(res);
        res = NULL;
    }

    if (socket_created && l_nServer != INVALID_SOCKET) {
        closesocket(l_nServer);
        l_nServer = INVALID_SOCKET;
        socket_created = false;
    }
    g_udp_socket = INVALID_SOCKET;

    if (opcua_thread_started && opcua_thread.joinable()) {
        opcua_thread.join();
    }

    if (ipv6_manager_initialized) {
        ipv6_remove_all_allocated_addresses(config.nic_index);
        ipv6_manager_cleanup();
        ipv6_manager_initialized = false;
    }

    if (wsa_initialized) {
        WSACleanup();
        wsa_initialized = false;
    }

    if (log_initialized) {
        log_manager_cleanup();
        log_initialized = false;
    }

    return exit_code;
}
