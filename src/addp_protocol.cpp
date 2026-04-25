#include "addp_protocol.h"
#include <cstring>

namespace ADDP {
    bool parseScanRequest(const uint8_t* buffer, int size, uint32_t& sequenceNumber, uint8_t* clientMac, uint8_t* clientIpv6) {
        if (size < 32) {
            return false;
        }
        
        // Check protocol ID
        uint16_t protocolId = readUint16LE(buffer);
        if (protocolId != PROTOCOL_ID) {
            return false;
        }
        
        // Check version
        uint8_t version = buffer[2];
        if (version != VERSION) {
            return false;
        }
        
        // Check message type
        uint8_t messageType = buffer[3];
        if (messageType != SCAN_REQUEST) {
            return false;
        }
        
        // Read sequence number
        sequenceNumber = readUint32LE(buffer + 6);
        
        // Read client MAC address
        memcpy(clientMac, buffer + 10, 6);
        
        // Read client IPv6 address
        memcpy(clientIpv6, buffer + 16, 16);
        
        return true;
    }

    int buildDeviceResponse(const DeviceInfo& device, uint32_t sequenceNumber, const uint8_t* clientMac, const uint8_t* clientIpv6, uint8_t* buffer) {
        // Calculate response size
        int responseSize = 32 + 90 + (device.busCount * 34);
        
        // Build header
        writeUint16LE(buffer, PROTOCOL_ID);
        buffer[2] = VERSION;
        buffer[3] = DEVICE_RESPONSE;
        writeUint16LE(buffer + 4, responseSize);
        writeUint32LE(buffer + 6, sequenceNumber);
        memcpy(buffer + 10, clientMac, 6);
        memcpy(buffer + 16, clientIpv6, 16);
        
        // Build device info
        uint8_t* payload = buffer + 32;
        payload[0] = device.deviceType;
        memcpy(payload + 1, device.ipv6Address, 16);
        memcpy(payload + 17, device.macAddress, 6);
        memcpy(payload + 23, device.deviceName, 32);
        memcpy(payload + 55, device.model, 32);
        payload[87] = device.busCount;
        writeUint16LE(payload + 88, device.opcuaPort);
        memcpy(payload + 90, device.opcuaPath, 32);
        
        // Build bus info
        uint8_t* busInfo = payload + 122;
        for (uint8_t i = 0; i < device.busCount; i++) {
            busInfo[0] = device.buses[i].busId;
            busInfo[1] = 1; // Enabled
            busInfo[2] = device.buses[i].nodeCount;
            memset(busInfo + 3, 0, 31);
            strncpy((char*)busInfo + 3, device.buses[i].description, 30);
            busInfo += 34;
        }
        
        return responseSize;
    }

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
}
