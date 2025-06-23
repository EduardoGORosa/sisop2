#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>

#include "Packet.hpp"
#include "Connection.hpp"

struct ServerInfo {
    std::string ip;
    uint16_t port;
};

class FrontEnd {
public:
    FrontEnd(const std::string& ip, uint16_t port);
    ~FrontEnd();
    
    void run();
    void stop();
    void updateLeader(int newLeaderId);
    
private:
    std::string ip_;
    uint16_t port_;
    int listenFd_;
    std::atomic<bool> running_;
    
    // Leader information
    std::atomic<int> currentLeaderId_;
    std::map<int, ServerInfo> allServers_;
    std::mutex leaderMutex_;
    
    // Client connections
    std::map<std::string, std::vector<int>> clients_;
    std::mutex clientMutex_;
    
    void acceptLoop();
    void handleClient(int clientFd);
    bool forwardToLeader(const Packet& packet, Connection& clientConn, const std::string& user);
    int connectToServer(int serverId);
    void discoverLeader();
    void monitorLeader();
    
    std::thread monitorThread_;
};

