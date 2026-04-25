#ifndef NETWORK_H
#define NETWORK_H

#include "common.h"

class Network {
public:
    Network() : wsaData(), multicastSocket(INVALID_SOCKET) {
        memset(&multicastAddr, 0, sizeof(multicastAddr));
    }
    
    ~Network() {
        cleanup();
    }
    
    ErrorCode init();
    ErrorCode createMulticastSocket();
    ErrorCode receiveData(uint8_t* buffer, int& size, sockaddr_in6* senderAddr);
    ErrorCode sendData(const uint8_t* buffer, int size, const sockaddr_in6* destAddr);
    void cleanup();
    
    static const char* macToString(const uint8_t* mac, char* buffer, size_t bufferSize);
    static const char* ipv6ToString(const uint8_t* ipv6, char* buffer, size_t bufferSize);
    
private:
    WSADATA wsaData;
    SOCKET multicastSocket;
    sockaddr_in6 multicastAddr;
};

#endif // NETWORK_H
