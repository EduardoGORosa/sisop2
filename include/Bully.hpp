#pragma once

#include <string>
#include <map>
#include <thread>
#include <atomic>
#include "Connection.hpp"

struct ServerInfo {
    std::string ip;
    uint16_t port;
};

class Server;

class Bully {
public:
    Bully(int myId, std::map<int, ServerInfo> servers, Server* serverInstance);
    void start();
    void handleElectionMessage(const Packet& p, const std::string& senderIp, uint16_t senderPort);
    void stop();

private:
    int myId_;
    std::map<int, ServerInfo> servers_;
    int leaderId_;
    std::atomic<bool> electionInProgress_;
    std::atomic<bool> running_;
    Server* server_;
    std::thread heartbeatThread_;
    std::thread electionTimerThread_;

    void startElection();
    void sendPacketTo(int serverId, const Packet& p);
    void broadcast(const Packet& p);
    void heartbeatLoop();
    void checkForLeaderFailure();
};