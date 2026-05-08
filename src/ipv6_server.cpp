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
#include "cJSON.h"

#define BUF_LEN 256  

// 简单的JSON配置结构体
typedef struct {
    int listen_port;
    std::string multicast_ip;
    int nic_index;
} Ipv6MulticastConfig;

// 从JSON文件中读取配置
Ipv6MulticastConfig read_ipv6_config(const std::string& config_file) {
    Ipv6MulticastConfig config = {0};
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
    
    // 释放cJSON对象
    cJSON_Delete(root);
    
    return config;
}

int main_loop(int serversocket) ;
int opcua_server_main(void);

int main(int argc, char* argv[])
{
    WSADATA     wsaData;
    WORD wVersionRequested;                   // 版本
    wVersionRequested = MAKEWORD(1, 1);       //版本信息
    WSAStartup(wVersionRequested, &wsaData);  //初始化Windows套接字库

    // 从配置文件读取IPv6组播配置
    Ipv6MulticastConfig config = read_ipv6_config("config.json");
    
    // 验证配置是否有效
    if (config.listen_port == 0 || config.multicast_ip.empty() || config.nic_index == 0) {
        std::cerr << "配置无效，请检查config.json文件" << std::endl;
        return -1;
    }

    //使用此结构来指定将套接字连接到的本地或远程端点地址
    struct sockaddr_in6 addr = { 0 };
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(config.listen_port);
    addr.sin6_addr = in6addr_any;    // 必须用这个，不能空初始化
    
    
    //创建一个UDP套接字
    int l_nServer;
    if ((l_nServer = socket(AF_INET6, SOCK_DGRAM, 0)) < 0)
    {
        perror("创建失败");
        return -1;
    }
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
		fprintf(stderr, "getaddrinfo failed for %s\n", config.multicast_ip.c_str());
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
    printf("waiting receive\n");
	
	printf("=== OPC UA 服务线程已启动 ===\n") ;

	// ==============================
	// 创建 C++ 线程，运行 opcua_server
	// ==============================
	std::thread opcua_thread(opcua_server_main);
	

    // Main loop
    std::cout << "\nListening for ADDP scan requests..." << std::endl;
    std::cout << "Multicast address: " << config.multicast_ip << std::endl;
    std::cout << "UDP port: " << config.listen_port << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl << std::endl;

	main_loop(l_nServer);

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

    return 0;
}
