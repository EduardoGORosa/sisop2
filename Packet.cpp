#include "Packet.hpp"
#include <cstring>
#include <arpa/inet.h>

std::vector<char> Packet::serialize() const {
    // 2 bytes de tipo + 4 bytes de length + payload
    std::vector<char> buf(6 + length);
    uint16_t nt = htons(type);
    uint32_t nl = htonl(length);

    // copia cabeçalho
    std::memcpy(buf.data(),      &nt, 2);
    std::memcpy(buf.data() + 2,  &nl, 4);

    // copia payload, se houver
    if (length > 0) {
        std::memcpy(buf.data() + 6, payload.data(), length);
    }

    return buf;
}

bool Packet::tryDeserializeHeader(const char* hdr,
                                  uint16_t& outType,
                                  uint32_t& outLen) {
    uint16_t nt;
    uint32_t nl;

    // lê bytes da rede
    std::memcpy(&nt,  hdr,     2);
    std::memcpy(&nl,  hdr + 2, 4);

    // converte para host order
    outType = ntohs(nt);
    outLen  = ntohl(nl);
    return true;
}

