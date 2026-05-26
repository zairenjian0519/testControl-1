#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <thread>

#include "ipv6_manager.h"

#define ADDP_PROTOCOL_ID 0xABDE
#define ADDP_VERSION 0x01
#define ADDP_SCAN_REQUEST 0x01
#define ADDP_DEVICE_RESPONSE 0x02
#define ADDP_UDP_PORT 6060

enum ErrorCode {
    SUCCESS = 0,
    SOCKET_CREATE = 1,
    SOCKET_BIND = 2,
    SOCKET_OPTIONS = 3,
    RECEIVE = 4,
    SEND = 5,
    PARSE = 6
};

enum DeviceType {
    CONTROLLER = 0x01
};

struct BusInfo {
    uint8_t busId;
    uint8_t enabled;
    uint8_t nodeCount;
    char description[31];
};

struct DeviceInfo {
    uint8_t deviceType;
    uint8_t ipv6Address[16];
    uint8_t macAddress[6];
    char deviceName[32];
    char model[32];
    uint8_t busCount;
    uint16_t opcuaPort;
    char opcuaPath[32];
    BusInfo* buses;
};

struct Network {
    int multicastSocket;
    struct sockaddr_in6 multicastAddr;
};

static uint16_t readUint16LE(const uint8_t* buffer) {
    return static_cast<uint16_t>((buffer[1] << 8) | buffer[0]);
}

static uint32_t readUint32LE(const uint8_t* buffer) {
    return (static_cast<uint32_t>(buffer[3]) << 24) |
           (static_cast<uint32_t>(buffer[2]) << 16) |
           (static_cast<uint32_t>(buffer[1]) << 8) |
           static_cast<uint32_t>(buffer[0]);
}

static void writeUint16LE(uint8_t* buffer, uint16_t value) {
    buffer[0] = value & 0xFF;
    buffer[1] = (value >> 8) & 0xFF;
}

static void writeUint32LE(uint8_t* buffer, uint32_t value) {
    buffer[0] = value & 0xFF;
    buffer[1] = (value >> 8) & 0xFF;
    buffer[2] = (value >> 16) & 0xFF;
    buffer[3] = (value >> 24) & 0xFF;
}

static const char* macToString(const uint8_t* mac, char* buffer, size_t bufferSize) {
    if (bufferSize < 18) {
        return nullptr;
    }
    snprintf(buffer, bufferSize, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buffer;
}

static const char* ipv6ToString(const uint8_t* ipv6, char* buffer, size_t bufferSize) {
    if (bufferSize < INET6_ADDRSTRLEN) {
        return nullptr;
    }
    struct in6_addr addr;
    memcpy(&addr, ipv6, 16);
    return inet_ntop(AF_INET6, &addr, buffer, bufferSize);
}

static bool get_interface_mac_address(int nic_index, char* mac_address, size_t mac_address_size) {
    char ifname[IF_NAMESIZE] = {0};
    if (!if_indextoname(static_cast<unsigned int>(nic_index), ifname)) {
        return false;
    }

    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);

    FILE* fp = fopen(path, "r");
    if (!fp) {
        return false;
    }

    char line[32] = {0};
    bool ok = fgets(line, sizeof(line), fp) != nullptr;
    fclose(fp);

    if (!ok) {
        return false;
    }

    line[strcspn(line, "\r\n")] = '\0';
    if (strlen(line) >= mac_address_size) {
        return false;
    }

    strcpy(mac_address, line);
    return true;
}

static ErrorCode networkInit(Network* network) {
    network->multicastSocket = -1;
    memset(&network->multicastAddr, 0, sizeof(network->multicastAddr));
    return SUCCESS;
}

static ErrorCode networkReceiveData(Network* network, uint8_t* buffer, int* size, struct sockaddr_in6* senderAddr) {
    socklen_t addrLen = sizeof(*senderAddr);
    ssize_t rc = recvfrom(network->multicastSocket, buffer, 1024, 0,
                          reinterpret_cast<struct sockaddr*>(senderAddr), &addrLen);

    if (rc < 0) {
        if (errno == EINTR) {
            return RECEIVE;
        }
        std::cerr << "recvfrom failed: " << strerror(errno) << std::endl;
        return RECEIVE;
    }

    *size = static_cast<int>(rc);
    return SUCCESS;
}

static ErrorCode networkSendData(Network* network, const uint8_t* buffer, int size, const struct sockaddr_in6* destAddr) {
    ssize_t sent = sendto(network->multicastSocket, buffer, size, 0,
                          reinterpret_cast<const struct sockaddr*>(destAddr), sizeof(*destAddr));

    if (sent < 0) {
        std::cerr << "sendto failed: " << strerror(errno) << std::endl;
        return SEND;
    }

    return SUCCESS;
}

static void networkCleanup(Network* network) {
    network->multicastSocket = -1;
}

static bool addpParseScanRequest(const uint8_t* buffer, int size, uint32_t* sequenceNumber, uint8_t* clientMac, uint8_t* clientIpv6) {
    if (size < 32) {
        return false;
    }

    if (readUint16LE(buffer) != ADDP_PROTOCOL_ID) {
        return false;
    }

    if (buffer[2] != ADDP_VERSION || buffer[3] != ADDP_SCAN_REQUEST) {
        return false;
    }

    *sequenceNumber = readUint32LE(buffer + 6);
    memcpy(clientMac, buffer + 10, 6);
    memcpy(clientIpv6, buffer + 16, 16);
    return true;
}

static int addpBuildDeviceResponse(const DeviceInfo* device, uint32_t sequenceNumber, const uint8_t* clientMac, const uint8_t* clientIpv6, uint8_t* buffer) {
    int responseSize = 32 + 122 + (device->busCount * 34);

    writeUint16LE(buffer, ADDP_PROTOCOL_ID);
    buffer[2] = ADDP_VERSION;
    buffer[3] = ADDP_DEVICE_RESPONSE;
    writeUint16LE(buffer + 4, responseSize);
    writeUint32LE(buffer + 6, sequenceNumber);
    memcpy(buffer + 10, clientMac, 6);
    memcpy(buffer + 16, clientIpv6, 16);

    uint8_t* payload = buffer + 32;
    payload[0] = device->deviceType;
    memcpy(payload + 1, device->ipv6Address, 16);
    memcpy(payload + 17, device->macAddress, 6);
    memcpy(payload + 23, device->deviceName, 32);
    memcpy(payload + 55, device->model, 32);
    payload[87] = device->busCount;
    writeUint16LE(payload + 88, device->opcuaPort);
    memcpy(payload + 90, device->opcuaPath, 32);

    uint8_t* busInfo = payload + 122;
    for (uint8_t i = 0; i < device->busCount; i++) {
        busInfo[0] = device->buses[i].busId;
        busInfo[1] = 1;
        busInfo[2] = device->buses[i].nodeCount;
        memset(busInfo + 3, 0, 31);
        strncpy(reinterpret_cast<char*>(busInfo + 3), device->buses[i].description, 30);
        busInfo += 34;
    }

    return responseSize;
}

static void deviceInit(DeviceInfo* device, const char* deviceName, const char* model, const char* ipv6Address, const char* macAddress) {
    device->deviceType = CONTROLLER;

    strncpy(device->deviceName, deviceName, 31);
    device->deviceName[31] = '\0';

    strncpy(device->model, model, 31);
    device->model[31] = '\0';

    struct in6_addr addr;
    if (inet_pton(AF_INET6, ipv6Address, &addr) == 1) {
        memcpy(device->ipv6Address, &addr, 16);
    }

    unsigned int mac[6] = {0};
    if (sscanf(macAddress, "%02x:%02x:%02x:%02x:%02x:%02x",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
        for (int i = 0; i < 6; ++i) {
            device->macAddress[i] = static_cast<uint8_t>(mac[i]);
        }
    }

    device->busCount = 0;
    device->opcuaPort = 4840;
    strcpy(device->opcuaPath, "/autbus/controller");
    device->buses = nullptr;
}

static void deviceAddBus(DeviceInfo* device, uint8_t busId, const char* description, uint8_t nodeCount) {
    BusInfo* newBuses = new BusInfo[device->busCount + 1];
    if (device->buses) {
        memcpy(newBuses, device->buses, device->busCount * sizeof(BusInfo));
        delete[] device->buses;
    }

    newBuses[device->busCount].busId = busId;
    newBuses[device->busCount].enabled = 1;
    newBuses[device->busCount].nodeCount = nodeCount;
    memset(newBuses[device->busCount].description, 0, 31);
    strncpy(newBuses[device->busCount].description, description, 30);

    device->buses = newBuses;
    device->busCount++;
}

static void deviceSetOpcuaInfo(DeviceInfo* device, uint16_t port, const char* path) {
    device->opcuaPort = port;
    strncpy(device->opcuaPath, path, 31);
    device->opcuaPath[31] = '\0';
}

static void devicePrintInfo(const DeviceInfo* device) {
    std::cout << "Device Information:" << std::endl;
    std::cout << "  Name: " << device->deviceName << std::endl;
    std::cout << "  Model: " << device->model << std::endl;

    char ipv6Str[INET6_ADDRSTRLEN];
    std::cout << "  IPv6 Address: " << ipv6ToString(device->ipv6Address, ipv6Str, sizeof(ipv6Str)) << std::endl;

    char macStr[18];
    std::cout << "  MAC Address: " << macToString(device->macAddress, macStr, sizeof(macStr)) << std::endl;

    std::cout << "  OPC UA Port: " << device->opcuaPort << std::endl;
    std::cout << "  OPC UA Path: " << device->opcuaPath << std::endl;
    std::cout << "  Buses: " << static_cast<int>(device->busCount) << std::endl;

    for (uint8_t i = 0; i < device->busCount; i++) {
        std::cout << "    Bus " << static_cast<int>(device->buses[i].busId) << ": "
                  << device->buses[i].description << " (" << static_cast<int>(device->buses[i].nodeCount)
                  << " nodes)" << std::endl;
    }
}

int main_loop(int serversocket, int nic_index, const std::atomic<bool>* running) {
    std::cout << "=== AUTBUS Controller Simulator (Linux) ===" << std::endl;

    Network network;
    if (networkInit(&network) != SUCCESS) {
        std::cerr << "Network initialization failed" << std::endl;
        return 1;
    }

    network.multicastSocket = serversocket;

    if (nic_index == 0) {
        std::cerr << "Invalid nic_index" << std::endl;
        networkCleanup(&network);
        return 1;
    }

    char ipv6_address[INET6_ADDRSTRLEN] = {0};
    if (!ipv6_allocate_address("spssps", nic_index, ipv6_address)) {
        std::cerr << "Failed to allocate IPv6 address" << std::endl;
        networkCleanup(&network);
        return 1;
    }

    char mac_address[18] = {0};
    if (!get_interface_mac_address(nic_index, mac_address, sizeof(mac_address))) {
        std::cerr << "Failed to get MAC address for interface index " << nic_index << ", using default" << std::endl;
        strcpy(mac_address, "00:0C:8F:00:01:01");
    }

    DeviceInfo device;
    memset(&device, 0, sizeof(device));

    deviceInit(&device, "spssps", "ATB-5000", ipv6_address, mac_address);
    deviceAddBus(&device, 0, "AUTBUS Bus 0", 3);
    deviceAddBus(&device, 1, "AUTBUS Bus 1", 2);
    deviceSetOpcuaInfo(&device, 4840, "/autbus/controller");
    devicePrintInfo(&device);

    uint8_t buffer[1024];
    struct sockaddr_in6 senderAddr;

    while (!running || running->load()) {
        int size = 0;
        ErrorCode result = networkReceiveData(&network, buffer, &size, &senderAddr);

        if (result == SUCCESS && size > 0) {
            std::cout << "\nReceived UDP packet:" << std::endl;
            std::cout << "  Size: " << size << " bytes" << std::endl;

            char senderAddrStr[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &senderAddr.sin6_addr, senderAddrStr, sizeof(senderAddrStr));
            std::cout << "  Sender: " << senderAddrStr << ":" << ntohs(senderAddr.sin6_port) << std::endl;

            std::cout << "  Packet hex: ";
            for (int i = 0; i < size && i < 64; i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(buffer[i]) << " ";
                if ((i + 1) % 16 == 0) {
                    std::cout << std::endl << "  ";
                }
            }
            std::cout << std::dec << std::endl;

            uint32_t sequenceNumber;
            uint8_t clientMac[6];
            uint8_t clientIpv6[16];

            std::cout << "  Attempting to parse ADDP scan request..." << std::endl;
            if (addpParseScanRequest(buffer, size, &sequenceNumber, clientMac, clientIpv6)) {
                char macStr[18];
                char ipv6Str[INET6_ADDRSTRLEN];
                std::cout << "  Successfully parsed ADDP scan request:" << std::endl;
                std::cout << "  Sequence: " << sequenceNumber << std::endl;
                std::cout << "  Client MAC: " << macToString(clientMac, macStr, sizeof(macStr)) << std::endl;
                std::cout << "  Client IPv6: " << ipv6ToString(clientIpv6, ipv6Str, sizeof(ipv6Str)) << std::endl;

                int responseSize = addpBuildDeviceResponse(&device, sequenceNumber, clientMac, clientIpv6, buffer);

                std::cout << "  Sending device response (" << responseSize << " bytes)..." << std::endl;
                if (networkSendData(&network, buffer, responseSize, &senderAddr) == SUCCESS) {
                    std::cout << "  Sent device response successfully" << std::endl;
                } else {
                    std::cerr << "  Failed to send response" << std::endl;
                }
            } else {
                std::cout << "  Failed to parse ADDP scan request" << std::endl;

                if (size >= 32) {
                    uint16_t protocolId = readUint16LE(buffer);
                    uint8_t version = buffer[2];
                    uint8_t messageType = buffer[3];
                    std::cout << "  Header analysis:" << std::endl;
                    std::cout << "  Protocol ID: 0x" << std::hex << protocolId << std::dec << std::endl;
                    std::cout << "  Version: " << static_cast<int>(version) << std::endl;
                    std::cout << "  Message Type: " << static_cast<int>(messageType) << std::endl;
                } else {
                    std::cout << "  Packet too short for ADDP header" << std::endl;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    char ipv6_str[INET6_ADDRSTRLEN] = {0};
    ipv6ToString(device.ipv6Address, ipv6_str, sizeof(ipv6_str));
    ipv6_release_address("spssps", nic_index, ipv6_str);

    networkCleanup(&network);
    delete[] device.buses;
    return 0;
}
