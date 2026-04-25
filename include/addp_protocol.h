#ifndef ADDP_PROTOCOL_H
#define ADDP_PROTOCOL_H

#include "common.h"

namespace ADDP {
    // Parse ADDP scan request
    bool parseScanRequest(const uint8_t* buffer, int size, uint32_t& sequenceNumber, uint8_t* clientMac, uint8_t* clientIpv6);
    
    // Build ADDP device response
    int buildDeviceResponse(const DeviceInfo& device, uint32_t sequenceNumber, const uint8_t* clientMac, const uint8_t* clientIpv6, uint8_t* buffer);
    
    // Helper functions
    uint16_t readUint16LE(const uint8_t* buffer);
    uint32_t readUint32LE(const uint8_t* buffer);
    void writeUint16LE(uint8_t* buffer, uint16_t value);
    void writeUint32LE(uint8_t* buffer, uint32_t value);
}

#endif // ADDP_PROTOCOL_H
