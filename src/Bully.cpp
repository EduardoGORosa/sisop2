#include "Bully.hpp"
#include "Server.hpp"
#include <iostream>
#include <chrono>
#include <unistd.h>

Bully::Bully(int myId, std::map<int, ServerInfo> servers, Server* serverInstance)
    : myId_(myId),
      servers_(servers),
      leaderId_(-1),
      electionInProgress_(false),
      running_(false),
      server_(serverInstance),
      leaderAlive_(true) {
    // Inicialmente, o líder é o servidor com o maior ID
    leaderId_ = servers_.rbegin()->first;
}

void Bully::start() {
    running_ = true;
    // Se eu sou o líder, começo a enviar heartbeats
    if (myId_ == leaderId_) {
        heartbeatThread_ = std::thread(&Bully::heartbeatLoop, this);
    } else {
    // Se não sou o líder, começo a verificar se ele está vivo
        heartbeatThread_ = std::thread(&Bully::checkForLeaderFailure, this);
    }
}

void Bully::stop() {
    running_ = false;
    if (heartbeatThread_.joinable()) {
        heartbeatThread_.join();
    }
    if (electionTimerThread_.joinable()) {
        electionTimerThread_.join();
    }
}

void Bully::startElection() {
    if (electionInProgress_.exchange(true)) {
        return; // Eleição já em andamento
    }
    std::cout << "[BULLY] Server " << myId_ << " starting an election.\n";

    bool higherServerExists = false;
    for (auto const& [id, info] : servers_) {
        if (id > myId_) {
            higherServerExists = true;
            Packet electionPkt{ELECTION, 0, {}};
            sendPacketTo(id, electionPkt);
        }
    }

    if (!higherServerExists) {
        // Se não há servidores com ID maior, eu sou o novo líder.
        leaderId_ = myId_;
        std::cout << "[BULLY] Server " << myId_ << " elected as new leader.\n";
        Packet coordinatorPkt{COORDINATOR, sizeof(int), {}};
        memcpy(coordinatorPkt.payload.data(), &myId_, sizeof(int));
        broadcast(coordinatorPkt);
        electionInProgress_ = false;
        // Iniciar tarefas de líder (como enviar heartbeats)
        if(heartbeatThread_.joinable()) heartbeatThread_.join();
        heartbeatThread_ = std::thread(&Bully::heartbeatLoop, this);
        server_->becomeLeader(); // Notifica a instância do servidor
    } else {
        // Inicia um timer para esperar por uma resposta (ANSWER)
        if(electionTimerThread_.joinable()) electionTimerThread_.join();
        electionTimerThread_ = std::thread([this](){
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (electionInProgress_) {
                // Timeout, ninguém respondeu. Eu sou o líder.
                leaderId_ = myId_;
                std::cout << "[BULLY] Server " << myId_ << " elected as new leader after timeout.\n";
                Packet coordinatorPkt{COORDINATOR, sizeof(int), {}};
                char* p = (char*)&myId_;
                coordinatorPkt.payload.assign(p, p+sizeof(int));
                broadcast(coordinatorPkt);
                electionInProgress_ = false;

                if(heartbeatThread_.joinable()) heartbeatThread_.join();
                heartbeatThread_ = std::thread(&Bully::heartbeatLoop, this);
                server_->becomeLeader();
            }
        });
    }
}

void Bully::handleElectionMessage(const Packet& p, const std::string& senderIp, uint16_t senderPort) {
    std::cout << "[BULLY] Encontrou uma mensagem do tipo " << p.type << " \n";
    if (p.type == ELECTION) {
        int senderId = -1;
        // Descobrir o ID do remetente
        for(auto const& [id, info] : servers_){
            if(info.ip == senderIp && info.port == senderPort){
                senderId = id;
                break;
            }
        }

        if (senderId != -1 && myId_ > senderId) {
            std::cout << "[BULLY] Received ELECTION from " << senderId << ". Sending ANSWER.\n";
            Packet answerPkt{ANSWER, 0, {}};
            sendPacketTo(senderId, answerPkt);
            startElection(); // Eu tenho um ID maior, então começo minha própria eleição
        }
    } else if (p.type == ANSWER) {
        std::cout << "[BULLY] Received ANSWER. I will not be the leader.\n";
        electionInProgress_ = false; // Alguém com ID maior está ativo
        // Iniciar timer para esperar a mensagem COORDINATOR
    } else if (p.type == COORDINATOR) {
        int newLeaderId;
        memcpy(&newLeaderId, p.payload.data(), sizeof(int));
        leaderId_ = newLeaderId;
        electionInProgress_ = false;
        std::cout << "[BULLY] New leader is " << leaderId_ << ".\n";

        // Mudar para o modo de verificação de falha do líder
        if(heartbeatThread_.joinable()) heartbeatThread_.join();
        if(myId_ != leaderId_){
             heartbeatThread_ = std::thread(&Bully::checkForLeaderFailure, this);
        } else {
             server_->becomeLeader();
        }

    } else if (p.type == HEARTBEAT) {
        std::cout << "[BULLY] Entrou nesta condicional. \n";
        // Resetar o timer de falha do líder (implementado em checkForLeaderFailure)
    }
}

void Bully::heartbeatLoop() {
    while (running_ && myId_ == leaderId_) {
        std::cout << "[BULLY] Leader " << myId_ << " sending heartbeats.\n";
        Packet heartbeatPkt{HEARTBEAT, 0, {}};
        broadcast(heartbeatPkt);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void Bully::checkForLeaderFailure() {
    while (running_ && myId_ != leaderId_) {
        // Esta é uma implementação simplificada. Uma real usaria um timer que é resetado ao receber um heartbeat.
        if(leaderAlive_){
            std::cout << "[BULLY] Leader is still alive. \n";
            leaderAlive_ = false;
        }
        else{
            std::cout << "[BULLY] Server " << myId_ << " hasn't received a heartbeat from leader " << leaderId_ << ". Starting election.\n";
            startElection();
        }
        std::this_thread::sleep_for(std::chrono::seconds(5)); 
    }
}

void Bully::sendPacketTo(int serverId, const Packet& p) {
    ServerInfo dest = servers_[serverId];
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(dest.ip.c_str());
    addr.sin_port = htons(dest.port);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) >= 0) {
        Connection conn(sock);
        conn.sendPacket(p);
    }
    close(sock);
}

void Bully::broadcast(const Packet& p) {
    for (auto const& [id, info] : servers_) {
        if (id != myId_) {
            sendPacketTo(id, p);
        }
    }
}