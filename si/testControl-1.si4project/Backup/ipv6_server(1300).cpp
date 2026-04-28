#define _WIN32_WINNT 0x0600  // 目标是 Windows Vista 及以上

#include <stdio.h>
#include <Ws2tcpip.h>
#include <winsock2.h>  
#pragma comment(lib,"ws2_32.lib")
#include <iostream>
#include <thread>  // C++ 标准线程

#define PORT  6060   
#define IP "ff03::c"    
#define BUF_LEN 256  

int main_loop(int serversocket) ;
int opcua_server_main(void);

int main(int argc, char* argv[])
{
    WSADATA     wsaData;
    WORD wVersionRequested;                   // 版本
    wVersionRequested = MAKEWORD(1, 1);       //版本信息
    WSAStartup(wVersionRequested, &wsaData);  //初始化Windows套接字库

    //使用此结构来指定将套接字连接到的本地或远程端点地址
    struct sockaddr_in6 addr = { AF_INET6, htons(PORT) };
    
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
    group.ipv6mr_interface = 0;
    //IPv6组播组的地址。
    #if 0
    inet_pton(AF_INET6, IP, &group.ipv6mr_multiaddr);
	#else
	
	struct addrinfo hints = {}, *res = NULL;
	hints.ai_family = AF_INET6;
	hints.ai_flags = AI_NUMERICHOST;
	
	if (getaddrinfo(IP, NULL, &hints, &res) != 0) {
		fprintf(stderr, "getaddrinfo failed for %s\n", IP);
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
	

	main_loop(l_nServer);
		
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


   
    return 0;
}
