#pragma once

#include <cstdint>
#include <vector>
#include <arpa/inet.h>
#include <cstring>    // para htons, htonl, ntohs, ntohl

enum PacketType : uint16_t {
    CMD_REGISTER    = 0,
    CMD_UPLOAD      = 1,
    CMD_DOWNLOAD    = 2,
    CMD_DELETE      = 3,
    CMD_LIST_SERVER = 4,
    CMD_EXIT        = 5,
    CMD_FILE_CHUNK  = 6
};

struct Packet {
    uint16_t type;
    uint32_t length;           // tamanho do payload
    std::vector<char> payload; // dados

    // Declarações apenas:
    std::vector<char> serialize() const;
    static bool tryDeserializeHeader(const char* hdr,
                                     uint16_t& outType,
                                     uint32_t& outLen);
};

