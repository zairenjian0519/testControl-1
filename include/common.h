#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

// Error codes
enum class ErrorCode {
    SUCCESS = 0,
    SOCKET_CREATE = 1,
    SOCKET_BIND = 2,
    SOCKET_OPTIONS = 3,
    RECEIVE = 4,
    SEND = 5,
    PARSE = 6
};

// ADDP protocol constants
namespace ADDP {
    constexpr uint16_t PROTOCOL_ID = 0xABDE;
    constexpr uint8_t VERSION = 0x01;
    
    // Message types
    constexpr uint8_t SCAN_REQUEST = 0x01;
    constexpr uint8_t DEVICE_RESPONSE = 0x02;
    
    constexpr const char* MULTICAST_ADDRESS = "ff03::c";
    constexpr uint16_t UDP_PORT = 6060;
    constexpr uint32_t TIMEOUT_MS = 1000;
    
    // Message lengths
    constexpr size_t HEADER_SIZE = 32;
    constexpr size_t MAX_DEVICE_NAME_C = 32;
    constexpr size_t MAX_MODEL_NAME = 32;
    constexpr size_t MAX_OPCUA_PATH = 32;
    constexpr size_t BUS_INFO_SIZE = 34;
}

// Device type
enum class DeviceType {
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
class DeviceInfo {
public:
    DeviceInfo() : deviceType(static_cast<uint8_t>(DeviceType::CONTROLLER)), busCount(0), opcuaPort(4840), buses(nullptr) {
        memset(deviceName, 0, sizeof(deviceName));
        memset(model, 0, sizeof(model));
        memset(opcuaPath, 0, sizeof(opcuaPath));
        memset(ipv6Address, 0, sizeof(ipv6Address));
        memset(macAddress, 0, sizeof(macAddress));
    }
    
    ~DeviceInfo() {
        if (buses) {
            delete[] buses;
        }
    }
    
    void init(const char* deviceName, const char* model, const char* ipv6Address, const char* macAddress);
    void addBus(uint8_t busId, const char* description, uint8_t nodeCount);
    void setOpcuaInfo(uint16_t port, const char* path);
    void printInfo() const;
    
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

// Network interface info
struct NetworkInterface {
    char* name;
    char* ipv6Address;
    char* macAddress;
};

#endif // COMMON_H
