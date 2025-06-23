#include "Connection.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

Connection::Connection(int fd)
  : fd_(fd)
{}

bool Connection::readAll(char* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t r = recv(fd_, buf + off, len - off, 0);
        if (r <= 0) return false;
        off += r;
    }
    return true;
}

bool Connection::writeAll(const char* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = send(fd_, buf + off, len - off, 0);
        if (w <= 0) return false;
        off += w;
    }
    return true;
}

bool Connection::sendPacket(const Packet& p) {
    auto buf = p.serialize();
    return writeAll(buf.data(), buf.size());
}

bool Connection::recvPacket(Packet& p) {
    char hdr[6];
    if (!readAll(hdr, 6)) return false;

    uint16_t tp; uint32_t ln;
    Packet::tryDeserializeHeader(hdr, tp, ln);

    p.type   = tp;
    p.length = ln;
    p.payload.resize(ln);

    return (ln == 0) ? true
                    : readAll(p.payload.data(), ln);
}

