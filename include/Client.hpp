#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <unordered_set>
#include <vector>

#include "FileManager.hpp"
#include "Packet.hpp"
#include "Connection.hpp"

class Client {
public:
    Client(const std::string& user,
           const std::string& ip,
           uint16_t port);
    ~Client();
    void run();

private:
    // rede
    std::string       user_, ip_;
    uint16_t          port_;
    int               sock_;
    int               reconnect_sock_;
    const int         reconnect_port_ = 1212;

    // storage local
    FileManager       fm_;
    std::string       syncDir_;
    std::atomic<bool> running_;

    // inotify
    int               inotifyFd_;
    int               inotifyWd_;

    // conexão com o servidor
    std::thread srv_;
    std::unique_ptr<Connection> conn_;

    // evita eco dos próprios syncs
    std::unordered_set<std::string> syncing_;
    std::mutex                     syncMtx_;

    // loops
    void sendRegister();
    void userLoop();
    void serverLoop();
    void watchLoop();
    void handleServerPacket(const Packet& p);

    // helpers
    void sendUpload(const std::string& path);
    void sendDelete(const std::string& fn);

    // communication
    void connectToServer(const std::string& ip,uint16_t port);
    void listenReconnection();
    void acceptLoop();
};

