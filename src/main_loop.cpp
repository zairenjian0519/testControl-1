#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <cstring>
#include <iomanip>
#include <atomic>
#include <stdint.h>  // C / C++ 通用
#include "ipv6_manager.h"
#include "log_manager.h"

std::atomic_bool g_main_loop_running(true);


// Link with ws2_32.lib
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

// 获取指定网络接口的MAC地址
bool get_interface_mac_address(int nic_index, char* mac_address, size_t mac_address_size) {
    ULONG outBufLen = 0;
    
    // 获取所需缓冲区大小
    if (GetAdaptersAddresses(AF_INET6, GAA_FLAG_INCLUDE_PREFIX, NULL, NULL, &outBufLen) == ERROR_BUFFER_OVERFLOW) {
        PIP_ADAPTER_ADDRESSES pAddresses = (PIP_ADAPTER_ADDRESSES)malloc(outBufLen);
        if (pAddresses) {
            if (GetAdaptersAddresses(AF_INET6, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == NO_ERROR) {
                PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses;
                while (pCurrAddresses) {
                    if (pCurrAddresses->IfIndex == nic_index) {
                        // 将MAC地址转换为字符串格式
                        if (pCurrAddresses->PhysicalAddressLength == 6) {
                            snprintf(mac_address, mac_address_size, "%02X:%02X:%02X:%02X:%02X:%02X",
                                    pCurrAddresses->PhysicalAddress[0],
                                    pCurrAddresses->PhysicalAddress[1],
                                    pCurrAddresses->PhysicalAddress[2],
                                    pCurrAddresses->PhysicalAddress[3],
                                    pCurrAddresses->PhysicalAddress[4],
                                    pCurrAddresses->PhysicalAddress[5]);
                            free(pAddresses);
                            return true;
                        }
                    }
                    pCurrAddresses = pCurrAddresses->Next;
                }
            }
            free(pAddresses);
        }
    }
    
    return false;
}

// ADDP protocol constants
#define ADDP_PROTOCOL_ID 0xABDE
#define ADDP_VERSION 0x01
#define ADDP_SCAN_REQUEST 0x01
#define ADDP_DEVICE_RESPONSE 0x02
#define ADDP_MULTICAST_ADDRESS "ff03::c"
#define ADDP_UDP_PORT 6060

// Error codes
enum ErrorCode {
    SUCCESS = 0,
    SOCKET_CREATE = 1,
    SOCKET_BIND = 2,
    SOCKET_OPTIONS = 3,
    RECEIVE = 4,
    SEND = 5,
    PARSE = 6
};

// Device type
enum DeviceType {
    CONTROLLER = 0x01
};

// Bus info structure
struct BusInfo {
    uint8_t busId;
    uint8_t enabled;
    uint8_t nodeCount;
    char description[31];
};

// Device info structure
struct DeviceInfo {
    uint8_t deviceType;
    uint8_t ipv6Address[16];
    uint8_t macAddress[12];
    char deviceName[32];
    char model[32];
    uint8_t busCount;
    uint16_t opcuaPort;
    char opcuaPath[32];
    BusInfo* buses;
};

// Network structure
struct Network {
    WSADATA wsaData;
    SOCKET multicastSocket;
    struct sockaddr_in6 multicastAddr;
    bool ownsSocket;
};

// Helper functions
uint16_t readUint16LE(const uint8_t* buffer) {
    return (buffer[1] << 8) | buffer[0];
}

uint32_t readUint32LE(const uint8_t* buffer) {
    return (buffer[3] << 24) | (buffer[2] << 16) | (buffer[1] << 8) | buffer[0];
}

void writeUint16LE(uint8_t* buffer, uint16_t value) {
    buffer[0] = value & 0xFF;
    buffer[1] = (value >> 8) & 0xFF;
}

void writeUint32LE(uint8_t* buffer, uint32_t value) {
    buffer[0] = value & 0xFF;
    buffer[1] = (value >> 8) & 0xFF;
    buffer[2] = (value >> 16) & 0xFF;
    buffer[3] = (value >> 24) & 0xFF;
}

const char* macToString(const uint8_t* mac, char* buffer, size_t bufferSize) {
    if (bufferSize < 18) {
        return nullptr;
    }
    sprintf(buffer, "%02x:%02x:%02x:%02x:%02x:%02x", 
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buffer;
}

const char* ipv6ToString(const uint8_t* ipv6, char* buffer, size_t bufferSize) {
    if (bufferSize < INET6_ADDRSTRLEN) {
        return nullptr;
    }
    struct in6_addr addr;
    memcpy(&addr, ipv6, 16);
    InetNtop(AF_INET6, &addr, buffer, bufferSize);
    return buffer;
}

// Network functions
ErrorCode networkInit(Network* network) 
{
#if 0
	// Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &network->wsaData) != 0) {
        std::cerr << "WSAStartup failed: " << WSAGetLastError() << std::endl;
        return SOCKET_CREATE;
    }
#endif
    network->multicastSocket = INVALID_SOCKET;
    network->ownsSocket = false;
    return SUCCESS;
}

ErrorCode networkCreateMulticastSocket(Network* network) {
    // Create IPv6 UDP socket
    network->multicastSocket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (network->multicastSocket == INVALID_SOCKET) {
        std::cerr << "socket creation failed: " << WSAGetLastError() << std::endl;
        return SOCKET_CREATE;
    }
    network->ownsSocket = true;
    
    // Set reuse address
    int opt = 1;
    if (setsockopt(network->multicastSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
        std::cerr << "setsockopt SO_REUSEADDR failed: " << WSAGetLastError() << std::endl;
        return SOCKET_OPTIONS;
    }
    
    // Bind to port
    memset(&network->multicastAddr, 0, sizeof(network->multicastAddr));
    network->multicastAddr.sin6_family = AF_INET6;
    network->multicastAddr.sin6_port = htons(ADDP_UDP_PORT);
    network->multicastAddr.sin6_addr = in6addr_any;
    
    if (bind(network->multicastSocket, (struct sockaddr*)&network->multicastAddr, sizeof(network->multicastAddr)) == SOCKET_ERROR) {
        std::cerr << "bind failed: " << WSAGetLastError() << std::endl;
        return SOCKET_BIND;
    }
    
    // Try to join multicast group
    struct sockaddr_in6 groupAddr;
    memset(&groupAddr, 0, sizeof(groupAddr));
    groupAddr.sin6_family = AF_INET6;
    
    // Use a more reliable way to set multicast address
    if (InetPton(AF_INET6, ADDP_MULTICAST_ADDRESS, &groupAddr.sin6_addr) != 1) {
        std::cerr << "Invalid multicast address: " << ADDP_MULTICAST_ADDRESS << std::endl;
        return SOCKET_OPTIONS;
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
    if (setsockopt(network->multicastSocket, IPPROTO_IPV6, IPV6_JOIN_GROUP, (char*)&groupAddr, sizeof(groupAddr)) == SOCKET_ERROR) {
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
        std::cout << "Trying alternative approach: listening for all UDP packets on port " << ADDP_UDP_PORT << std::endl;
        // Continue without joining multicast group
    }

    std::cout << "Multicast socket created and bound to port " << ADDP_UDP_PORT << std::endl;
    return SUCCESS;
}

ErrorCode networkReceiveData(Network* network, uint8_t* buffer, int* size, struct sockaddr_in6* senderAddr) {
    int addrLen = sizeof(*senderAddr);
    *size = recvfrom(network->multicastSocket, (char*)buffer, 1024, 0, (struct sockaddr*)senderAddr, &addrLen);
    
    if (*size == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSAETIMEDOUT && error != WSAEWOULDBLOCK && error != WSAENOTSOCK && g_main_loop_running.load()) {
            log_warn("recvfrom failed: %d", error);
        }
        return RECEIVE;
    }
    
    return SUCCESS;
}

ErrorCode networkSendData(Network* network, const uint8_t* buffer, int size, const struct sockaddr_in6* destAddr) {
    int sent = sendto(network->multicastSocket, (const char*)buffer, size, 0, (struct sockaddr*)destAddr, sizeof(*destAddr));
    
    if (sent == SOCKET_ERROR) {
        log_warn("sendto failed: %d", WSAGetLastError());
        return SEND;
    }
    
    return SUCCESS;
}

void networkCleanup(Network* network) {
    if (network->ownsSocket && network->multicastSocket != INVALID_SOCKET) {
        closesocket(network->multicastSocket);
    }
    network->multicastSocket = INVALID_SOCKET;
    network->ownsSocket = false;
}

// ADDP protocol functions
bool addpParseScanRequest(const uint8_t* buffer, int size, uint32_t* sequenceNumber, uint8_t* clientMac, uint8_t* clientIpv6) {
    if (size < 32) {
        return false;
    }
    
    // Check protocol ID
    uint16_t protocolId = readUint16LE(buffer);
    if (protocolId != ADDP_PROTOCOL_ID) {
        return false;
    }
    
    // Check version
    uint8_t version = buffer[2];
    if (version != ADDP_VERSION) {
        return false;
    }
    
    // Check message type
    uint8_t messageType = buffer[3];
    if (messageType != ADDP_SCAN_REQUEST) {
        return false;
    }
    
    // Read sequence number
    *sequenceNumber = readUint32LE(buffer + 6);
    
    // Read client MAC address
    memcpy(clientMac, buffer + 10, 6);
    
    // Read client IPv6 address
    memcpy(clientIpv6, buffer + 16, 16);
    
    return true;
}

int addpBuildDeviceResponse(const DeviceInfo* device, uint32_t sequenceNumber, const uint8_t* clientMac, const uint8_t* clientIpv6, uint8_t* buffer) {
    // Calculate response size
    int responseSize = 32 + 122 + (device->busCount * 34);
    if (responseSize > 1024) {
        return 0;
    }
    
    // Build header
    writeUint16LE(buffer, ADDP_PROTOCOL_ID);
    buffer[2] = ADDP_VERSION;
    buffer[3] = ADDP_DEVICE_RESPONSE;
    writeUint16LE(buffer + 4, responseSize);
    writeUint32LE(buffer + 6, sequenceNumber);
    memcpy(buffer + 10, clientMac, 6);
    memcpy(buffer + 16, clientIpv6, 16);
    
    // Build device info
    uint8_t* payload = buffer + 32;
    payload[0] = device->deviceType;
    memcpy(payload + 1, device->ipv6Address, 16);
    memcpy(payload + 17, device->macAddress, 6);
    memcpy(payload + 23, device->deviceName, 32);
    memcpy(payload + 55, device->model, 32);
    payload[87] = device->busCount;
    writeUint16LE(payload + 88, device->opcuaPort);
    memcpy(payload + 90, device->opcuaPath, 32);
    
    // Build bus info
    uint8_t* busInfo = payload + 122;
    for (uint8_t i = 0; i < device->busCount; i++) {
        busInfo[0] = device->buses[i].busId;
        busInfo[1] = 1; // Enabled
        busInfo[2] = device->buses[i].nodeCount;
        memset(busInfo + 3, 0, 31);
        strncpy((char*)busInfo + 3, device->buses[i].description, 30);
        busInfo += 34;
    }
    
    return responseSize;
}

// Device functions
void deviceInit(DeviceInfo* device, const char* deviceName, const char* model, const char* ipv6Address, const char* macAddress) {
	std::cout <<deviceName<<std::endl;
	std::cout <<"111111111111111"<<std::endl;
// Set device type
    device->deviceType = CONTROLLER;
    
    // Set device name
    strncpy(device->deviceName, deviceName, 31);
    device->deviceName[31] = '\0';
    std::cout <<device->deviceName<<std::endl;
    // Set model
    strncpy(device->model, model, 31);
    device->model[31] = '\0';
    
    // Set IPv6 address
    struct in6_addr addr;
    if (InetPton(AF_INET6, ipv6Address, &addr) == 1) {
        memcpy(device->ipv6Address, &addr, 16);
    }
    
    unsigned int mac[6] = {0};
    if (sscanf(macAddress, "%02x:%02x:%02x:%02x:%02x:%02x",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            device->macAddress[i] = static_cast<uint8_t>(mac[i] & 0xFF);
        }
    }
    
    // Initialize other fields
    device->busCount = 0;
    device->opcuaPort = 4840;
    strcpy(device->opcuaPath, "/autbus/controller");
    device->buses = nullptr;
}

void deviceAddBus(DeviceInfo* device, uint8_t busId, const char* description, uint8_t nodeCount) {
    // Resize buses array
    BusInfo* newBuses = new BusInfo[device->busCount + 1];
    if (device->buses) {
        memcpy(newBuses, device->buses, device->busCount * sizeof(BusInfo));
        delete[] device->buses;
    }
    
    // Add new bus
    newBuses[device->busCount].busId = busId;
    newBuses[device->busCount].enabled = 1;
    newBuses[device->busCount].nodeCount = nodeCount;
    memset(newBuses[device->busCount].description, 0, 31);
    strncpy(newBuses[device->busCount].description, description, 30);
    
    device->buses = newBuses;
    device->busCount++;
}

void deviceSetOpcuaInfo(DeviceInfo* device, uint16_t port, const char* path) {
    device->opcuaPort = port;
    strncpy(device->opcuaPath, path, 31);
    device->opcuaPath[31] = '\0';
}

void devicePrintInfo(const DeviceInfo* device) {
    std::cout << "Device Information:" << std::endl;
    std::cout << "  Name: " << device->deviceName << std::endl;
    std::cout << "  Model: " << device->model << std::endl;
    
    // Print IPv6 address
    char ipv6Str[INET6_ADDRSTRLEN];
    std::cout << "  IPv6 Address: " << ipv6ToString(device->ipv6Address, ipv6Str, sizeof(ipv6Str)) << std::endl;
    
    // Print MAC address
    char macStr[18];
    std::cout << "  MAC Address: " << macToString(device->macAddress, macStr, sizeof(macStr)) << std::endl;
    
    std::cout << "  OPC UA Port: " << device->opcuaPort << std::endl;
    std::cout << "  OPC UA Path: " << device->opcuaPath << std::endl;
    std::cout << "  Buses: " << static_cast<int>(device->busCount) << std::endl;
    
    for (uint8_t i = 0; i < device->busCount; i++) {
        std::cout << "    Bus " << static_cast<int>(device->buses[i].busId) << ": " 
                  << device->buses[i].description << " (" << static_cast<int>(device->buses[i].nodeCount) << " nodes)" << std::endl;
    }
}

int main_loop(SOCKET serversocket, int nic_index, const char *device_name, const char *device_model, int opcua_port, const char *opcua_path) 
{
    std::cout << "=== AUTBUS Controller Simulator ===" << std::endl;
    const char *effective_device_name = (device_name && device_name[0] != '\0') ? device_name : "spssps";
    const char *effective_device_model = (device_model && device_model[0] != '\0') ? device_model : "ATB-5000";
    const char *effective_opcua_path = (opcua_path && opcua_path[0] != '\0') ? opcua_path : "/autbus/controller";
    
    // Initialize network
    Network network;
    if (networkInit(&network) != SUCCESS) {
        std::cerr << "Network initialization failed" << std::endl;
        return 1;
    }

	#if 0
    // Create multicast socket
    if (networkCreateMulticastSocket(&network) != SUCCESS) {
        std::cerr << "Multicast socket creation failed" << std::endl;
        networkCleanup(&network);
        return 1;
    }
    #endif

	network.multicastSocket = serversocket;
    
    if (nic_index == 0) {
        std::cerr << "Invalid nic_index" << std::endl;
        networkCleanup(&network);
        return 1;
    }
    
    // 从IPv6内存池分配地址
    char ipv6_address[INET6_ADDRSTRLEN] = {0};
    if (!ipv6_allocate_address(effective_device_name, nic_index, ipv6_address)) {
        std::cerr << "Failed to allocate IPv6 address" << std::endl;
        networkCleanup(&network);
        return 1;
    }
    
    // 获取指定网络接口的MAC地址
    char mac_address[18] = {0};
    if (!get_interface_mac_address(nic_index, mac_address, sizeof(mac_address))) {
        std::cerr << "Failed to get MAC address for interface " << nic_index << ", using default" << std::endl;
        strcpy(mac_address, "00:0C:8F:00:01:01");
    }
    
    // Initialize device information
    DeviceInfo device;
	memset(&device, 0, sizeof(device));
	
    // 使用动态分配的IPv6地址和获取的MAC地址初始化设备
    deviceInit(&device, effective_device_name, effective_device_model, ipv6_address, mac_address);
    
    // Add buses
    deviceAddBus(&device, 0, "AUTBUS Bus 0", 3); // 1 MN + 2 TN
    deviceAddBus(&device, 1, "AUTBUS Bus 1", 2); // 1 MN + 1 TN
    
    // Set OPC UA information
    deviceSetOpcuaInfo(&device, static_cast<uint16_t>(opcua_port), effective_opcua_path);
    
    // Print device information
    devicePrintInfo(&device);
    
    
    uint8_t buffer[1024];
    struct sockaddr_in6 senderAddr;
    
    unsigned int invalid_packet_log_count = 0;

    while (g_main_loop_running.load()) {
        int size = 0;
        ErrorCode result = networkReceiveData(&network, buffer, &size, &senderAddr);

        if (!g_main_loop_running.load()) {
            break;
        }
        
        if (result == SUCCESS && size > 0) {
            char senderAddrStr[INET6_ADDRSTRLEN];
            InetNtop(AF_INET6, &senderAddr.sin6_addr, senderAddrStr, sizeof(senderAddrStr));
            log_debug("Received UDP packet: size=%d sender=[%s]:%d",
                      size, senderAddrStr, ntohs(senderAddr.sin6_port));
            
            // Parse scan request
            uint32_t sequenceNumber;
            uint8_t clientMac[6];
            uint8_t clientIpv6[16];
            
            log_debug("Attempting to parse ADDP scan request");
            if (addpParseScanRequest(buffer, size, &sequenceNumber, clientMac, clientIpv6)) {
                char macStr[18];
                char ipv6Str[INET6_ADDRSTRLEN];
                log_debug("Parsed ADDP scan request: sequence=%u client_mac=%s client_ipv6=%s",
                          sequenceNumber,
                          macToString(clientMac, macStr, sizeof(macStr)),
                          ipv6ToString(clientIpv6, ipv6Str, sizeof(ipv6Str)));
                
                // Build response message
                int responseSize = addpBuildDeviceResponse(
                    &device,
                    sequenceNumber,
                    clientMac,
                    clientIpv6,
                    buffer
                );
                if (responseSize <= 0) {
                    log_warn("Failed to build ADDP response: buffer too small");
                    continue;
                }
                
                // Send response
                if (networkSendData(&network, buffer, responseSize, &senderAddr) == SUCCESS) {
                    log_debug("Sent ADDP device response: %d bytes", responseSize);
                } else {
                    log_warn("Failed to send ADDP device response");
                }
            } else {
                invalid_packet_log_count++;
                if (invalid_packet_log_count <= 10 || (invalid_packet_log_count % 100) == 0) {
                    uint16_t protocolId = size >= 2 ? readUint16LE(buffer) : 0;
                    uint8_t version = size >= 3 ? buffer[2] : 0;
                    uint8_t messageType = size >= 4 ? buffer[3] : 0;
                    log_warn("Invalid ADDP scan packet count=%u size=%d sender=[%s]:%d protocol=0x%04x version=%u type=%u",
                             invalid_packet_log_count,
                             size,
                             senderAddrStr,
                             ntohs(senderAddr.sin6_port),
                             protocolId,
                             version,
                             messageType);
                }
            }
        }
        
        // Short sleep to avoid high CPU usage
        Sleep(100);
    }
    
    // Cleanup
    // 释放分配的IPv6地址
    networkCleanup(&network);
    if (device.buses) {
        delete[] device.buses;
    }
    return 0;
}
