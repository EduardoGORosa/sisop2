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

    // storage local
    FileManager       fm_;
    std::string       syncDir_;
    std::atomic<bool> running_;

    // inotify
    int               inotifyFd_;
    int               inotifyWd_;

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
};

