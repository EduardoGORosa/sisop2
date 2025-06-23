#pragma once

#include <map>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "Packet.hpp"

struct ServerInfo {
    std::string ip;
    uint16_t port;
};

class Server; // Forward declaration

class Bully {
public:
    Bully(int myId, std::map<int, ServerInfo> servers, Server* serverInstance);
    ~Bully();
    
    void start();
    void stop();
    void startElection();
    void handleElectionMessage(const Packet& p, const std::string& senderIp, uint16_t senderPort);
    void sendPacketTo(int serverId, const Packet& p);
    void broadcast(const Packet& p);
    
    std::atomic<int> leaderId_;
    
private:
    int myId_;
    std::map<int, ServerInfo> servers_;
    std::atomic<bool> electionInProgress_;
    std::atomic<bool> running_;
    Server* server_;
    
    // Heartbeat and failure detection
    std::thread heartbeatThread_;
    std::thread failureDetectionThread_;
    std::thread coordinatorTimeoutThread_;
    
    std::mutex heartbeatMutex_;
    std::condition_variable heartbeatCondition_;
    std::chrono::steady_clock::time_point lastHeartbeatTime_;
    std::atomic<bool> heartbeatReceived_;
    
    // Election timeout handling
    std::mutex electionMutex_;
    std::condition_variable electionCondition_;
    std::atomic<bool> coordinatorReceived_;
    
    static const int HEARTBEAT_INTERVAL_MS = 1000;
    static const int FAILURE_TIMEOUT_MS = 5000;
    static const int ELECTION_TIMEOUT_MS = 10000;
    static const int COORDINATOR_TIMEOUT_MS = 15000;
    
    void heartbeatLoop();
    void failureDetectionLoop();
    void resetHeartbeatTimer();
    void waitForCoordinator();
    int findServerIdByAddress(const std::string& ip, uint16_t port);
};

