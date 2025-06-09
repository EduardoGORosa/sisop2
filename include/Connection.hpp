// File: include/Connection.hpp
#pragma once

#include "Packet.hpp"

class Connection {
public:
    explicit Connection(int fd);
    bool sendPacket(const Packet& p);
    bool recvPacket(Packet& p);
private:
    int  fd_;
    bool readAll(char* buf, size_t len);
    bool writeAll(const char* buf, size_t len);
};

