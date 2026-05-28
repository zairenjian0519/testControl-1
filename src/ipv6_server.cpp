#define _WIN32_WINNT 0x0600  // 目标是 Windows Vista 及以上

#include <stdio.h>
#include <Ws2tcpip.h>
#include <winsock2.h>  
#pragma comment(lib,"ws2_32.lib")
#include <iostream>
#include <thread>  // C++ 标准线程
#include <fstream>
#include <string>
#include <sstream>
#include <getopt.h>
#include <signal.h>
#include "cJSON.h"
#include "log_manager.h"
#include "ipv6_manager.h"

extern volatile bool g_main_loop_running;
int g_udp_socket = -1;

void request_program_stop(void)
{
    g_main_loop_running = false;
    if (g_udp_socket != -1) {
        closesocket(g_udp_socket);
        g_udp_socket = -1;
    }
}

#define BUF_LEN 256  

// 日志配置结构体
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

// 简单的JSON配置结构体
typedef struct {
    int listen_port;
    std::string multicast_ip;
    int nic_index;
    std::string ipv6_start_address;
    std::string ipv6_end_address;
    int ipv6_prefix_length;
    int ipv6_max_lease_count;
    DeviceConfigFromFile device_config;
    LogConfigFromFile log_config;
} Ipv6MulticastConfig;

// 从JSON文件中读取配置
Ipv6MulticastConfig read_ipv6_config(const std::string& config_file) {
    Ipv6MulticastConfig config = {0};
    
    // 初始化日志配置默认值
    config.log_config.enable_log = false;
    config.log_config.log_level = "INFO";
    config.log_config.log_file = "application.log";
    config.log_config.log_file_count = 2;
    config.log_config.log_file_size = 1024 * 1024; // 默认1MB
    config.device_config.name = "spssps";
    config.device_config.model = "ATB-5000";
    config.device_config.opcua_port = 4840;
    config.device_config.opcua_path = "/autbus/controller";
    config.ipv6_prefix_length = 0;
    config.ipv6_max_lease_count = 0;
    std::string content;
    std::ifstream file(config_file);
    
    if (!file.is_open()) {
        std::cerr << "无法打开配置文件: " << config_file << std::endl;
        return config;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    content = buffer.str();
    file.close();
    
    // 使用cJSON解析JSON内容
    cJSON *root = cJSON_Parse(content.c_str());
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            std::cerr << "JSON解析错误: " << error_ptr << std::endl;
        }
        cJSON_Delete(root);
        return config;
    }
    
    // 获取ipv6_multicast对象
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
        // 获取listen_port
        cJSON *port = cJSON_GetObjectItem(ipv6_multicast, "listen_port");
        if (port != NULL && cJSON_IsNumber(port)) {
            config.listen_port = port->valueint;
        }
        
        // 获取multicast_ip
        cJSON *ip = cJSON_GetObjectItem(ipv6_multicast, "multicast_ip");
        if (ip != NULL && cJSON_IsString(ip)) {
            config.multicast_ip = ip->valuestring;
        }
        
        // 获取nic_index
        cJSON *index = cJSON_GetObjectItem(ipv6_multicast, "nic_index");
        if (index != NULL && cJSON_IsNumber(index)) {
            config.nic_index = index->valueint;
        }
    }
    
    // 获取log对象
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
    }

    cJSON *log_config = cJSON_GetObjectItem(root, "log");
    if (log_config != NULL) {
        // 获取enable_log
        cJSON *enable_log = cJSON_GetObjectItem(log_config, "enable_log");
        if (enable_log != NULL && cJSON_IsBool(enable_log)) {
            config.log_config.enable_log = cJSON_IsTrue(enable_log);
        }
        
        // 获取log_level
        cJSON *log_level = cJSON_GetObjectItem(log_config, "log_level");
        if (log_level != NULL && cJSON_IsString(log_level)) {
            config.log_config.log_level = log_level->valuestring;
        }
        
        // 获取log_file
        cJSON *log_file = cJSON_GetObjectItem(log_config, "log_file");
        if (log_file != NULL && cJSON_IsString(log_file)) {
            config.log_config.log_file = log_file->valuestring;
        }
        
        // 获取log_file_count
        cJSON *log_file_count = cJSON_GetObjectItem(log_config, "log_file_count");
        if (log_file_count != NULL && cJSON_IsNumber(log_file_count)) {
            config.log_config.log_file_count = log_file_count->valueint;
            // 确保日志文件数量至少为1
            if (config.log_config.log_file_count < 1) {
                config.log_config.log_file_count = 1;
            }
        }
        
        // 获取log_file_size
        cJSON *log_file_size = cJSON_GetObjectItem(log_config, "log_file_size");
        if (log_file_size != NULL && cJSON_IsNumber(log_file_size)) {
            config.log_config.log_file_size = log_file_size->valueint;
            // 确保日志文件大小至少为1KB
            if (config.log_config.log_file_size < 1024) {
                config.log_config.log_file_size = 1024;
            }
        }
    }
    
    // 释放cJSON对象
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

int main_loop(int serversocket, int nic_index, const char *device_name, const char *device_model, int opcua_port, const char *opcua_path) ;
extern "C" int opcua_server_main(const char *device_name);
extern "C" void opcua_server_stop(void);

static void stop_program(int signum)
{
    (void)signum;
    request_program_stop();
}

// 打印帮助信息
void print_help(const char* program_name)
{
    std::cout << "Usage: " << program_name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help          显示帮助信息" << std::endl;
    std::cout << "  -l, --log           启用日志输出" << std::endl;
    std::cout << "  -f, --log-file      设置日志文件路径" << std::endl;
    std::cout << "  -v, --verbose       启用详细日志（DEBUG级别）" << std::endl;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, stop_program);
    signal(SIGTERM, stop_program);

    // 先从配置文件读取配置
    Ipv6MulticastConfig config = read_ipv6_config("config.json");
    
    // 命令行参数处理 - 默认值来自配置文件
    bool enable_log = config.log_config.enable_log;
    bool log_to_file = false; // 如果指定了日志文件或者配置中启用了日志，则设置为true
    std::string log_file = config.log_config.log_file;
    LogLevel log_level = LOG_LEVEL_INFO;
    
    // 解析日志级别
    if (config.log_config.log_level == "DEBUG") {
        log_level = LOG_LEVEL_DEBUG;
    } else if (config.log_config.log_level == "INFO") {
        log_level = LOG_LEVEL_INFO;
    } else if (config.log_config.log_level == "WARN") {
        log_level = LOG_LEVEL_WARN;
    } else if (config.log_config.log_level == "ERROR") {
        log_level = LOG_LEVEL_ERROR;
    }

    // 定义长选项
    static struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"log", no_argument, NULL, 'l'},
        {"log-file", required_argument, NULL, 'f'},
        {"verbose", no_argument, NULL, 'v'},
        {NULL, 0, NULL, 0}
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
    
    // 如果配置中启用了日志但没有指定输出文件，则默认输出到文件
    if (enable_log && !log_to_file) {
        log_to_file = true;
    }

    // 初始化日志管理器
    LogConfig log_config = {
        .enable_log = enable_log,
        .log_level = log_level,
        .log_to_file = log_to_file,
        .log_file = log_file.c_str(),
        .log_file_count = config.log_config.log_file_count,
        .log_file_size = config.log_config.log_file_size,
        .log_fp = NULL
    };

    if (!log_manager_init(&log_config)) {
        std::cerr << "日志管理器初始化失败" << std::endl;
        return 1;
    }

    log_ipv6_config_debug(config);

    log_info("程序启动，初始化Winsock...");

    WSADATA     wsaData;
    WORD wVersionRequested;                   // 版本
    wVersionRequested = MAKEWORD(1, 1);       //版本信息
    WSAStartup(wVersionRequested, &wsaData);  //初始化Windows套接字库
    
    // 验证IPv6组播配置是否有效
    if (config.listen_port == 0 || config.multicast_ip.empty() || config.nic_index == 0) {
        log_error("IPv6组播配置无效，请检查config.json文件");
        log_manager_cleanup();
        return -1;
    }

    //使用此结构来指定将套接字连接到的本地或远程端点地址
    if (config.ipv6_start_address.empty() || config.ipv6_end_address.empty() || config.ipv6_prefix_length == 0) {
        log_error("IPv6 address pool configuration is invalid, please check config.json");
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

    struct sockaddr_in6 addr = { 0 };
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(config.listen_port);
    addr.sin6_addr = in6addr_any;    // 必须用这个，不能空初始化
    
    
    //创建一个UDP套接字
    int l_nServer;
    if ((l_nServer = socket(AF_INET6, SOCK_DGRAM, 0)) < 0)
    {
        perror("创建失败");
        ipv6_remove_all_allocated_addresses(config.nic_index);
        ipv6_manager_cleanup();
        log_manager_cleanup();
        return -1;
    }
    g_udp_socket = l_nServer;
    bind(l_nServer, (struct sockaddr*)&addr, sizeof(addr));
    //ipv6_mreq结构提供了用于IPv6地址的多播组的信息。
    struct ipv6_mreq group;
    //将接口索引指定为0，则使用默认的多播接口。
    group.ipv6mr_interface = config.nic_index; // 从配置文件获取网卡索引
    //IPv6组播组的地址。
    #if 0
    inet_pton(AF_INET6, config.multicast_ip.c_str(), &group.ipv6mr_multiaddr);
	#else
	
	struct addrinfo hints = {}, *res = NULL;
	hints.ai_family = AF_INET6;
	hints.ai_flags = AI_NUMERICHOST;
	
    if (getaddrinfo(config.multicast_ip.c_str(), NULL, &hints, &res) != 0) {
		log_error("getaddrinfo failed for %s", config.multicast_ip.c_str());
        ipv6_remove_all_allocated_addresses(config.nic_index);
        ipv6_manager_cleanup();
        log_manager_cleanup();
        return 1;
	}
	
	memcpy(&group.ipv6mr_multiaddr, 
		   &((struct sockaddr_in6*)res->ai_addr)->sin6_addr, 
		   sizeof(group.ipv6mr_multiaddr));
	freeaddrinfo(res);
	#endif
    //将套接字加入到指定接口上提供的多播组。此选项仅对数据报和原始套接字有效（套接字类>型必须为SOCK_DGRAM或SOCK_RAW）。
    setsockopt(l_nServer, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, (char*)&group, sizeof(group));

    int l_naddLen = sizeof(addr);
    int l_nReadLen = 0;
    char msgbuf[BUF_LEN];
    log_info("等待接收数据...");
	
	log_info("=== OPC UA 服务线程已启动 ===");

	// ==============================
	// 创建 C++ 线程，运行 opcua_server
	// ==============================
	std::thread opcua_thread(opcua_server_main, config.device_config.name.c_str());
	

    // Main loop
    log_info("\n监听ADDP扫描请求...");
    log_info("多播地址: %s", config.multicast_ip.c_str());
    log_info("UDP端口: %d", config.listen_port);
    log_info("按Ctrl+C退出\n");

	main_loop(l_nServer,
              config.nic_index,
              config.device_config.name.c_str(),
              config.device_config.model.c_str(),
              config.device_config.opcua_port,
              config.device_config.opcua_path.c_str());

    opcua_server_stop();
    if (opcua_thread.joinable()) {
        opcua_thread.join();
    }

	#if 0
    while (1)
    {
        l_nReadLen = recvfrom(l_nServer, msgbuf, BUF_LEN, 0, (struct sockaddr*)&addr, &l_naddLen);
        if (l_nReadLen < 0)
        {
            perror("接收失败");
            exit(1);
        }
        msgbuf[l_nReadLen] = '\0';
        printf("%s\n", msgbuf);

        strcpy(msgbuf, "world");
        int l_nLen = sendto(l_nServer, msgbuf, strlen(msgbuf), 0, (struct sockaddr*)&addr, sizeof(addr));
        if (l_nLen < 0)
        {
            perror("发送失败");
            exit(1);
        }
        printf("Send %s\n", msgbuf);
    }

	#endif

    // 清理日志管理器
    ipv6_remove_all_allocated_addresses(config.nic_index);
    ipv6_manager_cleanup();
    log_manager_cleanup();
    
    return 0;
}
