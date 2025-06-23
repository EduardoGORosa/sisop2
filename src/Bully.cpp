#include "Bully.hpp"
#include "Server.hpp"
#include "Connection.hpp"

#include <iostream>
#include <chrono>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

Bully::Bully(int myId, std::map<int, ServerInfo> servers, Server* serverInstance)
    : myId_(myId),
      servers_(servers),
      leaderId_(-1),
      electionInProgress_(false),
      running_(false),
      server_(serverInstance),
      heartbeatReceived_(false),
      coordinatorReceived_(false) {
    
    // Initially, the leader is the server with the highest ID
    leaderId_ = servers_.rbegin()->first;
    lastHeartbeatTime_ = std::chrono::steady_clock::now();
}

Bully::~Bully() {
    stop();
}

void Bully::start() {
    running_ = true;
    
    if (myId_ == leaderId_) {
        // I am the initial leader, start sending heartbeats
        std::cout << "[BULLY] Starting as initial leader " << myId_ << "\n";
        heartbeatThread_ = std::thread(&Bully::heartbeatLoop, this);
    } else {
        // I am not the leader, start monitoring for leader failure
        std::cout << "[BULLY] Starting as backup, monitoring leader " << leaderId_ << "\n";
        resetHeartbeatTimer(); // Initialize timer
        failureDetectionThread_ = std::thread(&Bully::failureDetectionLoop, this);
    }
}

void Bully::stop() {
    running_ = false;
    
    // Wake up all waiting threads
    heartbeatCondition_.notify_all();
    electionCondition_.notify_all();
    
    if (heartbeatThread_.joinable()) {
        heartbeatThread_.join();
    }
    if (failureDetectionThread_.joinable()) {
        failureDetectionThread_.join();
    }
    if (coordinatorTimeoutThread_.joinable()) {
        coordinatorTimeoutThread_.join();
    }
}

void Bully::startElection() {
    if (electionInProgress_.exchange(true)) {
        std::cout << "[BULLY] Election already in progress, skipping\n";
        return;
    }
    
    std::cout << "[BULLY] Server " << myId_ << " starting election\n";
    
    bool higherServerExists = false;
    std::vector<int> higherServers;
    
    // Send ELECTION messages to all servers with higher IDs
    for (const auto& [id, info] : servers_) {
        if (id > myId_) {
            higherServerExists = true;
            higherServers.push_back(id);
            
            Packet electionPkt{ELECTION, 0, {}};
            std::cout << "[BULLY] Sending ELECTION to server " << id << "\n";
            sendPacketTo(id, electionPkt);
        }
    }
    
    if (!higherServerExists) {
        // No higher servers, I become the leader immediately
        std::cout << "[BULLY] No higher servers, becoming leader\n";
        leaderId_ = myId_;
        electionInProgress_ = false;
        
        // Announce leadership
        Packet coordinatorPkt{COORDINATOR, sizeof(int), {}};
        coordinatorPkt.payload.resize(sizeof(int));
        memcpy(coordinatorPkt.payload.data(), &myId_, sizeof(int));
        broadcast(coordinatorPkt);
        
        // Stop failure detection and start heartbeat
        if (failureDetectionThread_.joinable()) {
            failureDetectionThread_.join();
        }
        heartbeatThread_ = std::thread(&Bully::heartbeatLoop, this);
        
        server_->becomeLeader();
    } else {
        // Wait for responses from higher servers
        std::cout << "[BULLY] Waiting for responses from higher servers\n";
        coordinatorReceived_ = false; // Reset flag
        coordinatorTimeoutThread_ = std::thread(&Bully::waitForCoordinator, this);
    }
}

void Bully::waitForCoordinator() {
    std::unique_lock<std::mutex> lock(electionMutex_);
    
    // Wait for COORDINATOR message or timeout
    bool coordinatorReceived = electionCondition_.wait_for(
        lock, 
        std::chrono::milliseconds(COORDINATOR_TIMEOUT_MS),
        [this] { return coordinatorReceived_.load() || !running_.load(); }
    );
    
    if (!running_) {
        return;
    }
    
    if (!coordinatorReceived) {
        std::cout << "[BULLY] Timeout waiting for COORDINATOR, becoming leader\n";
        
        // Timeout occurred, become leader
        leaderId_ = myId_;
        electionInProgress_ = false;
        coordinatorReceived_ = false;
        
        // Announce leadership
        Packet coordinatorPkt{COORDINATOR, sizeof(int), {}};
        coordinatorPkt.payload.resize(sizeof(int));
        memcpy(coordinatorPkt.payload.data(), &myId_, sizeof(int));
        broadcast(coordinatorPkt);
        
        // Stop failure detection and start heartbeat
        if (failureDetectionThread_.joinable()) {
            failureDetectionThread_.join();
        }
        heartbeatThread_ = std::thread(&Bully::heartbeatLoop, this);
        
        server_->becomeLeader();
    }
}

void Bully::handleElectionMessage(const Packet& p, const std::string& senderIp, uint16_t senderPort) {
    int senderId = findServerIdByAddress(senderIp, senderPort);
    
    if (senderId == -1) {
        std::cout << "[BULLY] Unknown sender: " << senderIp << ":" << senderPort << "\n";
        return;
    }
    
    if (p.type == ELECTION) {
        std::cout << "[BULLY] Received ELECTION from server " << senderId << "\n";
        
        if (myId_ > senderId) {
            // Send ANSWER back
            std::cout << "[BULLY] Sending ANSWER to server " << senderId << "\n";
            Packet answerPkt{ANSWER, 0, {}};
            sendPacketTo(senderId, answerPkt);
            
            // Start my own election if not already in progress
            if (!electionInProgress_.load()) {
                std::thread(&Bully::startElection, this).detach();
            }
        }
    } else if (p.type == ANSWER) {
        std::cout << "[BULLY] Received ANSWER from server " << senderId << "\n";
        
        // A higher server is active, I will not be the leader
        // The election is still in progress, wait for COORDINATOR
        std::cout << "[BULLY] Higher server is active, waiting for COORDINATOR\n";
        
    } else if (p.type == COORDINATOR) {
        int newLeaderId = -1;
        if (p.payload.size() >= sizeof(int)) {
            memcpy(&newLeaderId, p.payload.data(), sizeof(int));
        }
        
        std::cout << "[BULLY] Received COORDINATOR, new leader is " << newLeaderId << "\n";
        
        leaderId_ = newLeaderId;
        electionInProgress_ = false;
        
        // Signal coordinator received
        {
            std::lock_guard<std::mutex> lock(electionMutex_);
            coordinatorReceived_ = true;
        }
        electionCondition_.notify_all();
        
        if (myId_ == newLeaderId) {
            // I am the new leader
            if (failureDetectionThread_.joinable()) {
                failureDetectionThread_.join();
            }
            heartbeatThread_ = std::thread(&Bully::heartbeatLoop, this);
            server_->becomeLeader();
        } else {
            // Someone else is the leader, start monitoring
            resetHeartbeatTimer();
            if (heartbeatThread_.joinable()) {
                heartbeatThread_.join();
            }
            failureDetectionThread_ = std::thread(&Bully::failureDetectionLoop, this);
        }
        
    } else if (p.type == HEARTBEAT) {
        if (senderId == leaderId_.load()) {
            std::cout << "[BULLY] Received HEARTBEAT from leader " << senderId << "\n";
            resetHeartbeatTimer();
        }
    }
}

void Bully::heartbeatLoop() {
    std::cout << "[BULLY] Starting heartbeat loop for leader " << myId_ << "\n";
    
    while (running_ && myId_ == leaderId_) {
        Packet heartbeatPkt{HEARTBEAT, 0, {}};
        broadcast(heartbeatPkt);
        std::cout << "[BULLY] Leader " << myId_ << " sent heartbeat\n";
        
        std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
    }
    
    std::cout << "[BULLY] Heartbeat loop ended for " << myId_ << "\n";
}

void Bully::failureDetectionLoop() {
    std::cout << "[BULLY] Starting failure detection loop, monitoring leader " << leaderId_ << "\n";
    
    while (running_ && myId_ != leaderId_) {
        std::unique_lock<std::mutex> lock(heartbeatMutex_);
        
        // Wait for heartbeat or timeout
        bool heartbeatReceived = heartbeatCondition_.wait_for(
            lock,
            std::chrono::milliseconds(FAILURE_TIMEOUT_MS),
            [this] { return heartbeatReceived_.load() || !running_.load() || myId_ == leaderId_.load(); }
        );
        
        if (!running_ || myId_ == leaderId_) {
            break;
        }
        
        if (!heartbeatReceived) {
            std::cout << "[BULLY] Leader " << leaderId_ << " failure detected, starting election\n";
            
            // Start election in a separate thread to avoid blocking
            std::thread(&Bully::startElection, this).detach();
            break;
        }
        
        // Reset for next iteration
        heartbeatReceived_ = false;
    }
    
    std::cout << "[BULLY] Failure detection loop ended for " << myId_ << "\n";
}

void Bully::resetHeartbeatTimer() {
    std::lock_guard<std::mutex> lock(heartbeatMutex_);
    lastHeartbeatTime_ = std::chrono::steady_clock::now();
    heartbeatReceived_ = true;
    heartbeatCondition_.notify_all();
}

int Bully::findServerIdByAddress(const std::string& ip, uint16_t port) {
    for (const auto& [id, info] : servers_) {
        if (info.ip == ip && info.port == port) {
            return id;
        }
    }
    return -1;
}

void Bully::sendPacketTo(int serverId, const Packet& p) {
    auto it = servers_.find(serverId);
    if (it == servers_.end()) {
        return; // Silently ignore unknown servers
    }
    
    const ServerInfo& dest = it->second;
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return; // Silently handle socket creation failure
    }
    
    // Set shorter socket timeout to avoid hanging
    struct timeval timeout;
    timeout.tv_sec = 1;  // Reduced from 2 seconds
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(dest.ip.c_str());
    addr.sin_port = htons(dest.port);
    
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) >= 0) {
        Connection conn(sock);
        conn.sendPacket(p); // Don't log failures for heartbeats
    }
    // Silently handle connection failures - they're expected when servers are down
    
    close(sock);
}

void Bully::broadcast(const Packet& p) {
    for (const auto& [id, info] : servers_) {
        if (id != myId_) {
            sendPacketTo(id, p);
        }
    }
}

