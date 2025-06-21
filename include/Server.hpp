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
#include "Connection.hpp"
#include "Bully.hpp"

class Server {
public:
    Server(const std::string& ip,
           uint16_t port,
           const std::string& storageRoot);
    void run();
    void becomeLeader();

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

    // conexão com o cliente para pedir reconexões quando o servidor cai
    int reconnect_sock_;    
    std::unique_ptr<Connection> reconnect_conn_;
    const int reconnect_port_ = 1212;

    // Bully variables
    std::unique_ptr<Bully> bully_;
    int myId_;
    std::map<int, ServerInfo> allServers_;
    std::atomic<bool> isLeader_;

    void acceptLoop();
    void handleClient(int fd);
    void broadcast(const std::string& user,
                   const Packet& pkt,
                   int exceptFd);
    void watchLoop();
    void connectToClient(const std::string& ip,uint16_t port);
    void forceReconnect(const std::string& ip, uint16_t port);  
    void handleServerMessage(int fd);  
};

