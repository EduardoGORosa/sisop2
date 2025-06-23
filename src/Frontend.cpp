#include "Frontend.hpp"
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <algorithm>

FrontEnd::FrontEnd(const std::string& ip, uint16_t port)
    : ip_(ip), port_(port), listenFd_(-1), running_(false), currentLeaderId_(-1) {
    
    // Initialize server list (same as in Server.hpp)
    allServers_ = {
        {10, {"10.67.103.36", 8001}},
        {20, {"10.67.103.35", 8002}},
        {30, {"10.67.103.33", 8003}}
    };
    
    // Initially assume the highest ID server is the leader
    currentLeaderId_ = allServers_.rbegin()->first;
}

FrontEnd::~FrontEnd() {
    stop();
}

void FrontEnd::run() {
    // Create listening socket
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        perror("[FE] socket creation failed");
        return;
    }
    
    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip_.c_str());
    addr.sin_port = htons(port_);
    
    if (bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[FE] bind failed");
        close(listenFd_);
        return;
    }
    
    if (listen(listenFd_, 10) < 0) {
        perror("[FE] listen failed");
        close(listenFd_);
        return;
    }
    
    std::cout << "[FE] Front-End listening on " << ip_ << ":" << port_ << std::endl;
    
    running_ = true;
    
    // Start leader monitoring thread
    monitorThread_ = std::thread(&FrontEnd::monitorLeader, this);
    
    // Start accepting client connections
    acceptLoop();
}

void FrontEnd::stop() {
    running_ = false;
    
    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }
    
    if (monitorThread_.joinable()) {
        monitorThread_.join();
    }
}

void FrontEnd::updateLeader(int newLeaderId) {
    std::lock_guard<std::mutex> lock(leaderMutex_);
    if (currentLeaderId_ != newLeaderId) {
        std::cout << "[FE] Leader changed from " << currentLeaderId_ 
                  << " to " << newLeaderId << std::endl;
        currentLeaderId_ = newLeaderId;
    }
}

void FrontEnd::acceptLoop() {
    while (running_) {
        sockaddr_in clientAddr;
        socklen_t len = sizeof(clientAddr);
        int clientFd = accept(listenFd_, (sockaddr*)&clientAddr, &len);
        
        if (clientFd < 0) {
            if (running_) {
                perror("[FE] accept failed");
            }
            continue;
        }
        
        // Handle client in a separate thread
        std::thread(&FrontEnd::handleClient, this, clientFd).detach();
    }
}

void FrontEnd::handleClient(int clientFd) {
    Connection clientConn(clientFd);
    Packet packet;
    
    // First packet should be registration
    if (!clientConn.recvPacket(packet) || packet.type != CMD_REGISTER) {
        std::cout << "[FE] Invalid registration packet" << std::endl;
        close(clientFd);
        return;
    }
    
    std::string user(packet.payload.begin(), packet.payload.end());
    std::cout << "[FE] User '" << user << "' connected" << std::endl;
    
    // Store client connection
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        clients_[user].push_back(clientFd);
    }
    
    // Forward registration to current leader
    if (!forwardToLeader(packet, clientConn, user)) {
        std::cout << "[FE] Failed to forward registration to leader" << std::endl;
        close(clientFd);
        return;
    }
    
    // Handle subsequent packets from client
    while (running_ && clientConn.recvPacket(packet)) {
        if (packet.type == CMD_EXIT) {
            // Remove client from list
            {
                std::lock_guard<std::mutex> lock(clientMutex_);
                auto& clientList = clients_[user];
                clientList.erase(
                    std::remove(clientList.begin(), clientList.end(), clientFd),
                    clientList.end()
                );
            }
            
            // Forward exit to leader
            forwardToLeader(packet, clientConn, user);
            break;
        }
        
        // Forward all other packets to the current leader
        if (!forwardToLeader(packet, clientConn, user)) {
            std::cout << "[FE] Failed to forward packet to leader, disconnecting client" << std::endl;
            break;
        }
    }
    
    close(clientFd);
    std::cout << "[FE] User '" << user << "' disconnected" << std::endl;
}

bool FrontEnd::forwardToLeader(const Packet& packet, Connection& clientConn, const std::string& user) {
    int leaderId = currentLeaderId_.load();
    
    // Try to connect to current leader
    int serverFd = connectToServer(leaderId);
    if (serverFd < 0) {
        // Leader might be down, try to discover new leader
        discoverLeader();
        leaderId = currentLeaderId_.load();
        serverFd = connectToServer(leaderId);
        
        if (serverFd < 0) {
            std::cout << "[FE] Cannot connect to any server" << std::endl;
            return false;
        }
    }
    
    Connection serverConn(serverFd);
    
    // If this is not a registration packet, send registration first
    if (packet.type != CMD_REGISTER) {
        Packet regPacket{CMD_REGISTER, static_cast<uint32_t>(user.size()), 
                        std::vector<char>(user.begin(), user.end())};
        if (!serverConn.sendPacket(regPacket)) {
            close(serverFd);
            return false;
        }
        
        // Receive initial sync packets and forward to client
        Packet syncPacket;
        while (serverConn.recvPacket(syncPacket)) {
            if (syncPacket.type == CMD_UPLOAD) {
                clientConn.sendPacket(syncPacket);
            } else {
                break; // End of initial sync
            }
        }
    }
    
    // Send the actual packet
    if (!serverConn.sendPacket(packet)) {
        close(serverFd);
        return false;
    }
    
    // For packets that expect a response, forward the response back to client
    if (packet.type == CMD_LIST_SERVER || packet.type == CMD_DOWNLOAD) {
        Packet response;
        if (serverConn.recvPacket(response)) {
            clientConn.sendPacket(response);
        }
    }
    
    close(serverFd);
    return true;
}

int FrontEnd::connectToServer(int serverId) {
    auto it = allServers_.find(serverId);
    if (it == allServers_.end()) {
        return -1;
    }
    
    const ServerInfo& server = it->second;
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(server.ip.c_str());
    addr.sin_port = htons(server.port);
    
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}

void FrontEnd::discoverLeader() {
    std::cout << "[FE] Discovering new leader..." << std::endl;
    
    // Try to connect to each server in descending order of ID
    for (auto it = allServers_.rbegin(); it != allServers_.rend(); ++it) {
        int serverId = it->first;
        int sock = connectToServer(serverId);
        
        if (sock >= 0) {
            // Try to send a simple packet to see if this server accepts clients
            Connection conn(sock);
            Packet testPacket{CMD_REGISTER, 4, {'t', 'e', 's', 't'}};
            
            if (conn.sendPacket(testPacket)) {
                // If server accepts the packet, it's likely the leader
                updateLeader(serverId);
                std::cout << "[FE] New leader discovered: " << serverId << std::endl;
                close(sock);
                return;
            }
            close(sock);
        }
    }
    
    std::cout << "[FE] No leader found!" << std::endl;
}

void FrontEnd::monitorLeader() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        if (!running_) break;
        
        // Try to connect to current leader to check if it's still alive
        int leaderId = currentLeaderId_.load();
        int sock = connectToServer(leaderId);
        
        if (sock < 0) {
            std::cout << "[FE] Leader " << leaderId << " appears to be down" << std::endl;
            discoverLeader();
        } else {
            close(sock);
        }
    }
}

