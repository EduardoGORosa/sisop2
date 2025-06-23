#pragma once

#include <string>
#include <map>
#include <thread>
#include <atomic>
#include <condition_variable>
#include "Connection.hpp"

struct ServerInfo {
    std::string ip;
    uint16_t port;
};

class Server;

class Bully {
public:
    int myId_;
    int leaderId_;
    
    // heartbeat
    std::atomic<bool>  leaderAlive_;
    std::condition_variable heartbeatCv_;
    std::mutex heartbeatMtx_;
    std::chrono::milliseconds heartbeatTimeout_{5000};

    Bully(int myId, std::map<int, ServerInfo> servers, Server* serverInstance);
    void start();
    void handleElectionMessage(const Packet& p, const std::string& senderIp, uint16_t senderPort);
    void stop();
    void sendPacketTo(int serverId, const Packet& p);

private:
    std::map<int, ServerInfo> servers_;
    std::atomic<bool> electionInProgress_;
    std::atomic<bool> running_;
    Server* server_;
    std::thread heartbeatThread_;
    std::thread electionTimerThread_;

    void startElection();
    void broadcast(const Packet& p);
    void heartbeatLoop();
    void checkForLeaderFailure();
};