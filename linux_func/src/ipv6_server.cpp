#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "cJSON.h"
#include "ipv6_manager.h"
#include "log_manager.h"

#define BUF_LEN 256

static std::atomic<bool> g_running(true);

static void linux_stop_handler(int) {
    g_running.store(false);
}

typedef struct {
    bool enable_log;
    std::string log_level;
    std::string log_file;
    int log_file_count;
    long log_file_size;
} LogConfigFromFile;

typedef struct {
    int listen_port;
    std::string multicast_ip;
    int nic_index;
    std::string ipv6_start_address;
    std::string ipv6_end_address;
    int ipv6_prefix_length;
    int ipv6_max_lease_count;
    LogConfigFromFile log_config;
} Ipv6MulticastConfig;

static Ipv6MulticastConfig read_ipv6_config(const std::string& config_file) {
    Ipv6MulticastConfig config = {};
    config.log_config.enable_log = false;
    config.log_config.log_level = "INFO";
    config.log_config.log_file = "application.log";
    config.log_config.log_file_count = 2;
    config.log_config.log_file_size = 1024 * 1024;

    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cerr << "Unable to open config file: " << config_file << std::endl;
        return config;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            std::cerr << "JSON parse error: " << error_ptr << std::endl;
        }
        return config;
    }

    cJSON* ipv6_multicast = cJSON_GetObjectItem(root, "ipv6_multicast");
    if (ipv6_multicast) {
        cJSON* port = cJSON_GetObjectItem(ipv6_multicast, "listen_port");
        if (port && cJSON_IsNumber(port)) {
            config.listen_port = port->valueint;
        }

        cJSON* ip = cJSON_GetObjectItem(ipv6_multicast, "multicast_ip");
        if (ip && cJSON_IsString(ip)) {
            config.multicast_ip = ip->valuestring;
        }

        cJSON* index = cJSON_GetObjectItem(ipv6_multicast, "nic_index");
        if (index && cJSON_IsNumber(index)) {
            config.nic_index = index->valueint;
        }
    }

    cJSON* ipv6_pool = cJSON_GetObjectItem(root, "ipv6_address_pool");
    if (ipv6_pool) {
        cJSON* start_address = cJSON_GetObjectItem(ipv6_pool, "start_address");
        if (start_address && cJSON_IsString(start_address)) {
            config.ipv6_start_address = start_address->valuestring;
        }

        cJSON* end_address = cJSON_GetObjectItem(ipv6_pool, "end_address");
        if (end_address && cJSON_IsString(end_address)) {
            config.ipv6_end_address = end_address->valuestring;
        }

        cJSON* prefix_length = cJSON_GetObjectItem(ipv6_pool, "prefix_length");
        if (prefix_length && cJSON_IsNumber(prefix_length)) {
            config.ipv6_prefix_length = prefix_length->valueint;
        }

        cJSON* max_lease_count = cJSON_GetObjectItem(ipv6_pool, "max_lease_count");
        if (max_lease_count && cJSON_IsNumber(max_lease_count)) {
            config.ipv6_max_lease_count = max_lease_count->valueint;
        }
    }

    cJSON* log_config = cJSON_GetObjectItem(root, "log");
    if (log_config) {
        cJSON* enable_log = cJSON_GetObjectItem(log_config, "enable_log");
        if (enable_log && cJSON_IsBool(enable_log)) {
            config.log_config.enable_log = cJSON_IsTrue(enable_log);
        }

        cJSON* log_level = cJSON_GetObjectItem(log_config, "log_level");
        if (log_level && cJSON_IsString(log_level)) {
            config.log_config.log_level = log_level->valuestring;
        }

        cJSON* log_file = cJSON_GetObjectItem(log_config, "log_file");
        if (log_file && cJSON_IsString(log_file)) {
            config.log_config.log_file = log_file->valuestring;
        }

        cJSON* log_file_count = cJSON_GetObjectItem(log_config, "log_file_count");
        if (log_file_count && cJSON_IsNumber(log_file_count)) {
            config.log_config.log_file_count = log_file_count->valueint;
            if (config.log_config.log_file_count < 1) {
                config.log_config.log_file_count = 1;
            }
        }

        cJSON* log_file_size = cJSON_GetObjectItem(log_config, "log_file_size");
        if (log_file_size && cJSON_IsNumber(log_file_size)) {
            config.log_config.log_file_size = log_file_size->valueint;
            if (config.log_config.log_file_size < 1024) {
                config.log_config.log_file_size = 1024;
            }
        }
    }

    cJSON_Delete(root);
    return config;
}

static void log_ipv6_config_debug(const Ipv6MulticastConfig& config) {
    log_debug("=== Parsed IPv6 configuration ===");
    log_debug("ipv6_multicast.listen_port: %d", config.listen_port);
    log_debug("ipv6_multicast.multicast_ip: %s", config.multicast_ip.c_str());
    log_debug("ipv6_multicast.nic_index: %d", config.nic_index);
    log_debug("ipv6_address_pool.start_address: %s", config.ipv6_start_address.c_str());
    log_debug("ipv6_address_pool.end_address: %s", config.ipv6_end_address.c_str());
    log_debug("ipv6_address_pool.prefix_length: %d", config.ipv6_prefix_length);
    log_debug("ipv6_address_pool.max_lease_count: %d", config.ipv6_max_lease_count);
    log_debug("log.enable_log: %s", config.log_config.enable_log ? "true" : "false");
    log_debug("log.log_level: %s", config.log_config.log_level.c_str());
    log_debug("log.log_file: %s", config.log_config.log_file.c_str());
    log_debug("log.log_file_count: %d", config.log_config.log_file_count);
    log_debug("log.log_file_size: %ld", config.log_config.log_file_size);
}

static int create_multicast_socket(const Ipv6MulticastConfig& config) {
    int sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        log_error("socket(AF_INET6, SOCK_DGRAM) failed: %s", strerror(errno));
        return -1;
    }

    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        log_warn("setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
    }

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(static_cast<uint16_t>(config.listen_port));
    addr.sin6_addr = in6addr_any;

    if (bind(sockfd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_error("bind UDP/%d failed: %s", config.listen_port, strerror(errno));
        close(sockfd);
        return -1;
    }

    struct ipv6_mreq group;
    memset(&group, 0, sizeof(group));
    group.ipv6mr_interface = static_cast<unsigned int>(config.nic_index);

    if (inet_pton(AF_INET6, config.multicast_ip.c_str(), &group.ipv6mr_multiaddr) != 1) {
        log_error("Invalid IPv6 multicast address: %s", config.multicast_ip.c_str());
        close(sockfd);
        return -1;
    }

    if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &group, sizeof(group)) < 0) {
        log_error("setsockopt(IPV6_JOIN_GROUP) failed on ifindex %d: %s",
                 config.nic_index, strerror(errno));
        close(sockfd);
        return -1;
    }

    return sockfd;
}

int main_loop(int serversocket, int nic_index, const std::atomic<bool>* running);
extern "C" int opcua_server_main(void);

static void print_help(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help          Show help" << std::endl;
    std::cout << "  -l, --log           Enable log output" << std::endl;
    std::cout << "  -f, --log-file      Set log file path" << std::endl;
    std::cout << "  -v, --verbose       Enable DEBUG log level" << std::endl;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, linux_stop_handler);
    signal(SIGTERM, linux_stop_handler);

    Ipv6MulticastConfig config = read_ipv6_config("config.json");

    bool enable_log = config.log_config.enable_log;
    bool log_to_file = false;
    std::string log_file = config.log_config.log_file;
    LogLevel log_level = LOG_LEVEL_INFO;

    if (config.log_config.log_level == "DEBUG") {
        log_level = LOG_LEVEL_DEBUG;
    } else if (config.log_config.log_level == "WARN") {
        log_level = LOG_LEVEL_WARN;
    } else if (config.log_config.log_level == "ERROR") {
        log_level = LOG_LEVEL_ERROR;
    }

    static struct option long_options[] = {
        {"help", no_argument, nullptr, 'h'},
        {"log", no_argument, nullptr, 'l'},
        {"log-file", required_argument, nullptr, 'f'},
        {"verbose", no_argument, nullptr, 'v'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    int option_index = 0;
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
    log_config.enable_log = enable_log;
    log_config.log_level = log_level;
    log_config.log_to_file = log_to_file;
    log_config.log_file = log_file.c_str();
    log_config.log_file_count = config.log_config.log_file_count;
    log_config.log_file_size = config.log_config.log_file_size;
    log_config.log_fp = nullptr;

    if (!log_manager_init(&log_config)) {
        std::cerr << "Failed to initialize log manager" << std::endl;
        return 1;
    }

    log_ipv6_config_debug(config);
    log_info("Program started on Linux");

    if (config.listen_port == 0 || config.multicast_ip.empty() || config.nic_index == 0) {
        log_error("Invalid IPv6 multicast config, please check config.json");
        log_manager_cleanup();
        return -1;
    }

    if (config.ipv6_start_address.empty() || config.ipv6_end_address.empty() || config.ipv6_prefix_length == 0) {
        log_error("Invalid IPv6 address pool config, please check config.json");
        log_manager_cleanup();
        return -1;
    }

    if (!ipv6_manager_init(
            config.ipv6_start_address.c_str(),
            config.ipv6_end_address.c_str(),
            config.ipv6_prefix_length)) {
        log_error("Failed to initialize IPv6 manager");
        log_manager_cleanup();
        return -1;
    }

    int server_socket = create_multicast_socket(config);
    if (server_socket < 0) {
        ipv6_manager_cleanup();
        log_manager_cleanup();
        return -1;
    }

    log_info("=== OPC UA service thread starting ===");
    std::thread opcua_thread(opcua_server_main);
    opcua_thread.detach();

    log_info("Listening for ADDP scan requests");
    log_info("Multicast address: %s", config.multicast_ip.c_str());
    log_info("UDP port: %d", config.listen_port);
    log_info("Interface index: %d", config.nic_index);

    main_loop(server_socket, config.nic_index, &g_running);

    close(server_socket);
    ipv6_manager_cleanup();
    log_manager_cleanup();
    return 0;
}
