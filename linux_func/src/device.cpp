#include "common.h"
#include <arpa/inet.h>
#include <iostream>
#include <iomanip>
#include <cstring>

void DeviceInfo::init(const char* deviceName, const char* model, const char* ipv6Address, const char* macAddress) {
    // Set device name
    strncpy(this->deviceName, deviceName, 31);
    this->deviceName[31] = '\0';
    
    // Set model
    strncpy(this->model, model, 31);
    this->model[31] = '\0';
    
    // Set IPv6 address
    in6_addr addr;
    if (inet_pton(AF_INET6, ipv6Address, &addr) == 1) {
        memcpy(this->ipv6Address, &addr, 16);
    }
    
    // Set MAC address
    unsigned int mac[6] = {0};
    if (sscanf(macAddress, "%02x:%02x:%02x:%02x:%02x:%02x",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
        for (int i = 0; i < 6; ++i) {
            this->macAddress[i] = static_cast<uint8_t>(mac[i]);
        }
    }
}

void DeviceInfo::addBus(uint8_t busId, const char* description, uint8_t nodeCount) {
    // Resize buses array
    BusInfo* newBuses = new BusInfo[busCount + 1];
    if (buses) {
        memcpy(newBuses, buses, busCount * sizeof(BusInfo));
        delete[] buses;
    }
    
    // Add new bus
    newBuses[busCount].busId = busId;
    newBuses[busCount].enabled = 1;
    newBuses[busCount].nodeCount = nodeCount;
    memset(newBuses[busCount].description, 0, 31);
    strncpy(newBuses[busCount].description, description, 30);
    
    buses = newBuses;
    busCount++;
}

void DeviceInfo::setOpcuaInfo(uint16_t port, const char* path) {
    opcuaPort = port;
    strncpy(opcuaPath, path, 31);
    opcuaPath[31] = '\0';
}

void DeviceInfo::printInfo() const {
    std::cout << "Device Information:" << std::endl;
    std::cout << "  Name: " << deviceName << std::endl;
    std::cout << "  Model: " << model << std::endl;
    
    // Print IPv6 address
    char ipv6Str[INET6_ADDRSTRLEN];
    in6_addr addr;
    memcpy(&addr, ipv6Address, 16);
    inet_ntop(AF_INET6, &addr, ipv6Str, sizeof(ipv6Str));
    std::cout << "  IPv6 Address: " << ipv6Str << std::endl;
    
    // Print MAC address
    std::cout << "  MAC Address: " << std::hex << std::setw(2) << std::setfill('0') 
              << static_cast<int>(macAddress[0]) << ":" 
              << std::setw(2) << static_cast<int>(macAddress[1]) << ":"
              << std::setw(2) << static_cast<int>(macAddress[2]) << ":"
              << std::setw(2) << static_cast<int>(macAddress[3]) << ":"
              << std::setw(2) << static_cast<int>(macAddress[4]) << ":"
              << std::setw(2) << static_cast<int>(macAddress[5]) << std::dec << std::endl;
    
    std::cout << "  OPC UA Port: " << opcuaPort << std::endl;
    std::cout << "  OPC UA Path: " << opcuaPath << std::endl;
    std::cout << "  Buses: " << static_cast<int>(busCount) << std::endl;
    
    for (uint8_t i = 0; i < busCount; i++) {
        std::cout << "    Bus " << static_cast<int>(buses[i].busId) << ": " 
                  << buses[i].description << " (" << static_cast<int>(buses[i].nodeCount) << " nodes)" << std::endl;
    }
}
