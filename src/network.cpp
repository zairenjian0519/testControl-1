#include "network.h"
#include <iostream>
#include <cstring>

ErrorCode Network::init() {
    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed: " << WSAGetLastError() << std::endl;
        return ErrorCode::SOCKET_CREATE;
    }
    
    return ErrorCode::SUCCESS;
}

ErrorCode Network::createMulticastSocket() {
    // Create IPv6 UDP socket
    multicastSocket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (multicastSocket == INVALID_SOCKET) {
        std::cerr << "socket creation failed: " << WSAGetLastError() << std::endl;
        return ErrorCode::SOCKET_CREATE;
    }
    
    // Set reuse address
    int opt = 1;
    if (setsockopt(multicastSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
        std::cerr << "setsockopt SO_REUSEADDR failed: " << WSAGetLastError() << std::endl;
        return ErrorCode::SOCKET_OPTIONS;
    }
    
    // Bind to port
    memset(&multicastAddr, 0, sizeof(multicastAddr));
    multicastAddr.sin6_family = AF_INET6;
    multicastAddr.sin6_port = htons(ADDP::UDP_PORT);
    multicastAddr.sin6_addr = in6addr_any;
    
    if (bind(multicastSocket, (sockaddr*)&multicastAddr, sizeof(multicastAddr)) == SOCKET_ERROR) {
        std::cerr << "bind failed: " << WSAGetLastError() << std::endl;
        return ErrorCode::SOCKET_BIND;
    }
    
    // Try to join multicast group
    sockaddr_in6 groupAddr;
    memset(&groupAddr, 0, sizeof(groupAddr));
    groupAddr.sin6_family = AF_INET6;
    
    // Use a more reliable way to set multicast address
    if (InetPton(AF_INET6, ADDP::MULTICAST_ADDRESS, &groupAddr.sin6_addr) != 1) {
        std::cerr << "Invalid multicast address: " << ADDP::MULTICAST_ADDRESS << std::endl;
        return ErrorCode::SOCKET_OPTIONS;
    }
    
    // Set interface index for Windows compatibility
    // Get interface index for the first IPv6-enabled interface
    ULONG ifIndex = 0;
    ULONG outBufLen = 0;
    if (GetAdaptersAddresses(AF_INET6, GAA_FLAG_INCLUDE_PREFIX, NULL, NULL, &outBufLen) == ERROR_BUFFER_OVERFLOW) {
        PIP_ADAPTER_ADDRESSES pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(outBufLen);
        if (pAddresses) {
            if (GetAdaptersAddresses(AF_INET6, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == NO_ERROR) {
                PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses;
                while (pCurrAddresses) {
                    if (pCurrAddresses->OperStatus == IfOperStatusUp) {
                        // Skip loopback interfaces by checking if the name contains "Loopback"
                        if (pCurrAddresses->AdapterName && 
                            strstr(pCurrAddresses->AdapterName, "Loopback") == NULL) {
                            ifIndex = pCurrAddresses->IfIndex;
                            break;
                        }
                    }
                    pCurrAddresses = pCurrAddresses->Next;
                }
            }
            free(pAddresses);
        }
    }
    
    if (ifIndex > 0) {
        groupAddr.sin6_scope_id = ifIndex;
        std::cout << "Using interface index: " << ifIndex << std::endl;
    }
    
    // Try to join multicast group
    if (setsockopt(multicastSocket, IPPROTO_IPV6, IPV6_JOIN_GROUP, (char*)&groupAddr, sizeof(groupAddr)) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        std::cerr << "setsockopt IPV6_JOIN_GROUP failed: " << error << std::endl;
        
        // Handle common errors
        switch (error) {
            case WSAEADDRNOTAVAIL:
                std::cerr << "Error: Address not available. Check if IPv6 is enabled on your network interface." << std::endl;
                break;
            case WSAEINVAL:
                std::cerr << "Error: Invalid parameter. Check multicast address format." << std::endl;
                break;
            case WSAEAFNOSUPPORT:
                std::cerr << "Error: Address family not supported. IPv6 may be disabled." << std::endl;
                break;
            default:
                std::cerr << "Error: Unexpected error code." << std::endl;
                break;
        }
        
        // Try alternative approach: don't join multicast group, just listen for all packets
        std::cout << "Trying alternative approach: listening for all UDP packets on port " << ADDP::UDP_PORT << std::endl;
        // Continue without joining multicast group
    }

    std::cout << "Multicast socket created and bound to port " << ADDP::UDP_PORT << std::endl;
    return ErrorCode::SUCCESS;
}

ErrorCode Network::receiveData(uint8_t* buffer, int& size, sockaddr_in6* senderAddr) {
    int addrLen = sizeof(*senderAddr);
    size = recvfrom(multicastSocket, (char*)buffer, 1024, 0, (sockaddr*)senderAddr, &addrLen);
    
    if (size == SOCKET_ERROR) {
        std::cerr << "recvfrom failed: " << WSAGetLastError() << std::endl;
        return ErrorCode::RECEIVE;
    }
    
    return ErrorCode::SUCCESS;
}

ErrorCode Network::sendData(const uint8_t* buffer, int size, const sockaddr_in6* destAddr) {
    int sent = sendto(multicastSocket, (const char*)buffer, size, 0, (sockaddr*)destAddr, sizeof(*destAddr));
    
    if (sent == SOCKET_ERROR) {
        std::cerr << "sendto failed: " << WSAGetLastError() << std::endl;
        return ErrorCode::SEND;
    }
    
    return ErrorCode::SUCCESS;
}

void Network::cleanup() {
    if (multicastSocket != INVALID_SOCKET) {
        closesocket(multicastSocket);
        multicastSocket = INVALID_SOCKET;
    }
    WSACleanup();
}

const char* Network::macToString(const uint8_t* mac, char* buffer, size_t bufferSize) {
    if (bufferSize < 18) {
        return nullptr;
    }
    sprintf(buffer, "%02x:%02x:%02x:%02x:%02x:%02x", 
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buffer;
}

const char* Network::ipv6ToString(const uint8_t* ipv6, char* buffer, size_t bufferSize) {
    if (bufferSize < INET6_ADDRSTRLEN) {
        return nullptr;
    }
    in6_addr addr;
    memcpy(&addr, ipv6, 16);
    InetNtop(AF_INET6, &addr, buffer, bufferSize);
    return buffer;
}
