#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>

#include "FileManager.hpp"
#include "Packet.hpp"

class Server {
public:
    Server(const std::string& ip,
           uint16_t port,
           const std::string& storageRoot);
    void run();

private:
    std::string ip_;
    uint16_t    port_;
    int         listenFd_;

    FileManager fm_;
    std::string storageRoot_;

    // inotify
    int                                        watchFd_;
    std::unordered_map<int,std::string>        wdToUser_;
    std::unordered_set<std::string>            watchedUsers_;
    std::mutex                                 watchMtx_;

    // clientes
    std::map<std::string,std::vector<int>>     clients_;
    std::mutex                                 clMtx_;

    // filtra eventos que vieram de handleClient()
    std::unordered_set<std::string>            syncing_;
    std::mutex                                 syncMtx_;

    void acceptLoop();
    void handleClient(int fd);
    void broadcast(const std::string& user,
                   const Packet& pkt,
                   int exceptFd);
    void watchLoop();
};

