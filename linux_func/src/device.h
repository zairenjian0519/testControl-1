#ifndef DEVICE_H
#define DEVICE_H

#include "common.h"

class Device {
public:
    Device();
    
    // Initialize device information
    void init(const std::string& deviceName, const std::string& model, const std::string& ipv6Address, const std::string& macAddress);
    
    // Add bus
    void addBus(uint8_t busId, const std::string& description, uint8_t nodeCount);
    
    // Get device information
    const DeviceInfo& getDeviceInfo() const;
    
    // Set OPC UA information
    void setOPCUAInfo(uint16_t port, const std::string& path);
    
    // Print device information
    void printInfo();
    
private:
    DeviceInfo deviceInfo;
};

#endif // DEVICE_H
