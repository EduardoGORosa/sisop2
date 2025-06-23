#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <condition_variable>
#include <atomic>

#include "FileManager.hpp"
#include "Packet.hpp"
#include "Connection.hpp"
#include "Bully.hpp"

struct PendingOperation {
    std::string operationId;
    std::string user;
    Packet originalPacket;
    int clientFd;
    std::unordered_set<int> pendingAcks;
    std::mutex ackMutex;
    std::condition_variable ackCondition;
    bool completed;
    
    PendingOperation(const std::string& id, const std::string& u, const Packet& pkt, int fd)
        : operationId(id), user(u), originalPacket(pkt), clientFd(fd), completed(false) {}
};


class Server {
public:
    Server(const std::string& ip,
           uint16_t port,
           const std::string& storageRoot,
           int myId);
    void run();
    void becomeLeader();
    void handleBackupOperation(const Packet& packet, const std::string& sourceIp, uint16_t sourcePort);

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

    // Passive replication acknowledgment
    std::map<std::string, std::shared_ptr<PendingOperation>> pendingOps_;
    std::mutex pendingOpsMutex_;
    std::atomic<uint64_t> operationCounter_;


    // conexão com o cliente para pedir reconexões quando o servidor cai
    int reconnect_sock_;    
    std::unique_ptr<Connection> reconnect_conn_;
    const int reconnect_port_ = 1212;

    // Bully variables
    std::unique_ptr<Bully> bully_;
    int myId_;
    std::map<int, ServerInfo> allServers_ = {
        {10, {"127.0.0.1", 8001}},
        {20, {"127.0.0.1", 8002}},
        {30, {"127.0.0.1", 8003}}
    };
    std::atomic<bool> isLeader_;

    void acceptLoop();
    void handleClient(int fd);
    void handleFrontEndClient(int fd);
    void broadcast(const std::string& user,
                   const Packet& pkt,
                   int exceptFd);
    void broadcastWithAck(const std::string& user,
                         const Packet& pkt,
                         int clientFd,
                         const std::string& operationId);
    void watchLoop();
    void connectToClient(const std::string& ip,uint16_t port);
    void forceReconnect(const std::string& ip, uint16_t port);  
    void handleServerMessage(int fd);
    std::string generateOperationId();
    void sendAcknowledgment(const std::string& operationId, int sourceServerId);
    void handleAcknowledgment(const std::string& operationId, int fromServerId);
    bool isDirectClientConnection(int fd);
};

