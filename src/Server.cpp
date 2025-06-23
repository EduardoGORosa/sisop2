#include "Server.hpp"
#include "Connection.hpp"
#include "Bully.hpp"

#include <arpa/inet.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <thread>
#include <iostream>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <ctime>

namespace fs = std::filesystem;

Server::Server(const std::string& ip,
               uint16_t port,
               const std::string& root,
               int myId)
  : ip_(ip),
    port_(port),
    fm_(root),
    storageRoot_(root),
    myId_(myId)    
{
    bully_ = std::make_unique<Bully>(myId, allServers_, this);
}

void Server::run() {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip_.c_str());
    addr.sin_port        = htons(port_);

    if(bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[server] Error on bind:");  
        exit(1);
    }
    if(listen(listenFd_, 10) < 0) {
        perror("[server] Error on listen:");  
        exit(1);
    }        
    std::cout << "[server] listening on " << ip_ << ":" << port_ << "\n";

    watchFd_ = inotify_init();
    if (watchFd_ >= 0) {
        for (auto& e : fs::directory_iterator(storageRoot_)) {
            if (!e.is_directory()) continue;
            std::string user = e.path().filename().string();
            int wd = inotify_add_watch(
                watchFd_,
                e.path().c_str(),
                IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM
            );
            if (wd >= 0) {
                std::lock_guard lk(watchMtx_);
                wdToUser_[wd]    = user;
                watchedUsers_.insert(user);
            }
        }
        std::thread(&Server::watchLoop, this).detach();
    }

    this->bully_->start();
    acceptLoop();
}

void Server::becomeLeader() {
    bully_->leaderId_ = this->myId_;    
    std::cout << "[SERVER] I am the new leader!\n";
    std::cout << "[SERVER] tentando conectar no cliente na porta " << reconnect_port_ << " , mapeando para porta do servidor "<<port_<<" \n";
    connectToClient("127.0.0.1", reconnect_port_);
    forceReconnect(ip_, port_);
}

void Server::connectToClient(const std::string& ip,uint16_t port){
    // cria e conecta o socket
    this->reconnect_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (this->reconnect_sock_ < 0) {
        perror("[Server] socket");
        std::exit(1);
    }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    addr.sin_port        = htons(port);
    
    if (connect(this->reconnect_sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[Server] connect");
        std::exit(1);
    }    

    this->reconnect_conn_ = std::make_unique<Connection>(this->reconnect_sock_);
    std::cout << "[Server] Connected to client IP " << ip << " , PORT " << port << "\n";    
}

void Server::forceReconnect(const std::string& ip, uint16_t port) {    
    std::vector<char> payload;
    
    uint8_t ip_len = static_cast<uint8_t>(ip.size());
    payload.push_back(static_cast<char>(ip_len));
    payload.insert(payload.end(), ip.begin(), ip.end());
    
    uint16_t net_port = htons(port);  
    const char* port_bytes = reinterpret_cast<const char*>(&net_port);
    payload.insert(payload.end(), port_bytes, port_bytes + sizeof(net_port));

    Packet p{
        CMD_REGISTER,
        static_cast<uint32_t>(payload.size()),
        payload
    };
    this->reconnect_conn_->sendPacket(p);
    std::cout << "[client] registration sent\n";
}

void Server::acceptLoop() {
    while (true) {
        sockaddr_in cli; socklen_t len = sizeof(cli);
        int fd = accept(listenFd_, (sockaddr*)&cli, &len);
        std::thread(&Server::handleClient, this, fd).detach();
    }
}

void Server::handleServerMessage(int fd) {
    Connection conn(fd);
    Packet p;
    // Tenta receber um pacote. Se não conseguir em um certo tempo, assume que é um cliente.
    // Uma implementação robusta teria um handshake.
    if (conn.recvPacket(p)) {
        char ip_str[INET_ADDRSTRLEN];
        sockaddr_in peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        getpeername(fd, (struct sockaddr*)&peer_addr, &peer_addr_len);
        inet_ntop(AF_INET, &peer_addr.sin_addr, ip_str, INET_ADDRSTRLEN);

        bully_->handleElectionMessage(p, ip_str, ntohs(peer_addr.sin_port));
        close(fd);
    } else {
        // Assume que é um cliente se não enviar um pacote de eleição rapidamente
        handleClient(fd);
    }
}

void Server::handleClient(int fd) {
    Connection conn(fd);
    Packet     p;

    if (bully_->leaderId_ == this->myId_){
        if (!conn.recvPacket(p) || p.type != CMD_REGISTER) {
            close(fd);
            return;
        }
    }
    std::string user(p.payload.begin(), p.payload.end());
    {
        std::lock_guard lk(clMtx_);
        clients_[user].push_back(fd);
    }
    fm_.ensureUserDir(user);

    if (bully_->leaderId_ == this->myId_){
        std::cout << "[server] user '" << user << "' connected\n";
    }

    {
        std::lock_guard lk(watchMtx_);
        if (!watchedUsers_.count(user)) {
            std::string dir = storageRoot_ + "/" + user;
            int wd = inotify_add_watch(
                watchFd_, dir.c_str(),
                IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM
            );
            if (wd >= 0) {
                wdToUser_[wd]    = user;
                watchedUsers_.insert(user);
            }
        }
    }

    // initial sync
    for (auto& fn : fm_.listFiles(user)) {
        std::vector<char> data;
        fm_.loadFile(user, fn, data);
        Packet up{ CMD_UPLOAD, 0, {} };
        uint32_t nl = htonl((uint32_t)fn.size()),
                 dl = htonl((uint32_t)data.size());
        up.payload.insert(up.payload.end(), (char*)&nl,(char*)&nl+4);
        up.payload.insert(up.payload.end(), fn.begin(),fn.end());
        up.payload.insert(up.payload.end(), (char*)&dl,(char*)&dl+4);
        up.payload.insert(up.payload.end(), data.begin(),data.end());
        up.length = up.payload.size();
        conn.sendPacket(up);
    }

    while (conn.recvPacket(p)) {
        if (p.type == CMD_EXIT) {
            {
                std::lock_guard lk(clMtx_);
                auto& v = clients_[user];
                v.erase(std::remove(v.begin(), v.end(), fd), v.end());
            }
            close(fd);
            std::cout << "[server] user '" << user << "' disconnected\n";
            return;
        }
        else if (p.type == CMD_UPLOAD) {
            const char* b = p.payload.data();
            uint32_t nl; memcpy(&nl,b,4); nl=ntohl(nl);
            std::string fn(b+4,b+4+nl);
            uint32_t dl; memcpy(&dl,b+4+nl,4); dl=ntohl(dl);
            std::vector<char> data(b+8+nl,b+8+nl+dl);

            std::string full = storageRoot_ + "/" + user + "/" + fn;
            if (fs::exists(full) && fs::file_size(full) == data.size()) {
                std::ifstream ex(full, std::ios::binary|std::ios::ate);
                std::vector<char> buf(data.size());
                ex.seekg(0);
                ex.read(buf.data(), buf.size());
                if (buf == data) {
                    std::lock_guard lk(syncMtx_);
                    syncing_.erase(user + "/" + fn);
                    continue;
                }
            }

            {
                std::lock_guard lk(syncMtx_);
                syncing_.insert(user + "/" + fn);
            }
            fm_.saveFile(user, fn, data);
            std::cout << "[server] saved '" << fn << "' from " << user << "\n";
            broadcast(user, p, fd);
        }
        else if (p.type == CMD_DELETE) {
            std::string fn(p.payload.begin(), p.payload.end());
            {
                std::lock_guard lk(syncMtx_);
                syncing_.insert(user + "/" + fn);
            }
            fm_.deleteFile(user, fn);
            std::cout << "[server] deleted '" << fn << "' from " << user << "\n";
            broadcast(user, p, fd);
        }
        else if (p.type == CMD_LIST_SERVER) {
            std::ostringstream oss;
            for (auto& fn : fm_.listFiles(user)) {
                std::string full = storageRoot_ + "/" + user + "/" + fn;
                struct stat st;
                if (stat(full.c_str(), &st) == 0) {
                    auto at = std::localtime(&st.st_atime);
                    auto mt = std::localtime(&st.st_mtime);
                    auto ct = std::localtime(&st.st_ctime);
                    oss << fn
                        << "  size=" << st.st_size
                        << "  atime=" << std::put_time(at, "%F %T")
                        << "  mtime=" << std::put_time(mt, "%F %T")
                        << "  ctime=" << std::put_time(ct, "%F %T")
                        << "\n";
                }
            }
            auto s = oss.str();
            Packet rp{ CMD_LIST_SERVER,
                       static_cast<uint32_t>(s.size()),
                       std::vector<char>(s.begin(), s.end()) };
            conn.sendPacket(rp);
        }
        else if (p.type == CMD_DOWNLOAD) {
            std::string fn(p.payload.begin(), p.payload.end());
            std::vector<char> data;
            if (fm_.loadFile(user, fn, data)) {
                Packet sp{ CMD_FILE_CHUNK, 0, {} };
                uint32_t nl = htonl((uint32_t)fn.size()),
                         dl = htonl((uint32_t)data.size());
                sp.payload.insert(sp.payload.end(), (char*)&nl,(char*)&nl+4);
                sp.payload.insert(sp.payload.end(), fn.begin(),fn.end());
                sp.payload.insert(sp.payload.end(), (char*)&dl,(char*)&dl+4);
                sp.payload.insert(sp.payload.end(), data.begin(),data.end());
                sp.length = sp.payload.size();
                conn.sendPacket(sp);
            }
        }
        else if (p.type == HEARTBEAT) {
            std::cout << "[BULLY] Recebeu HEARTBEAT.\n";
            bully_->leaderAlive_ = true;
        }
    }
}

void Server::broadcast(const std::string& user,
                       const Packet& pkt,
                       int exceptFd)
{
    std::cout << "[SERVER] Meu id: " << myId_ << " , leader id: "<< bully_->leaderId_ << "\n";
    if(bully_->leaderId_ == myId_){   // backups não fazem broadcast        
        std::lock_guard lk(clMtx_);
        for (int fd : clients_[user]) {
            if (fd == exceptFd) continue;
            Connection c(fd);
            c.sendPacket(pkt);
        }        
        for (const auto& [id, server] : allServers_) {            
            if (server.ip == ip_ && server.port == port_) continue;        
            std::cout << "[SERVER] Enviando broadcast para ID " << id << "\n";
            bully_->sendPacketTo(id, pkt);        
        }  
    }
}

void Server::watchLoop() {
    char buf[4096];
    while (true) {
        int len = read(watchFd_, buf, sizeof(buf));
        if (len < 0) { perror("[server] inotify read"); continue; }
        int i = 0;
        while (i < len) {
            auto* e = (inotify_event*)(buf + i);
            std::string user;
            {
                std::lock_guard lk(watchMtx_);
                user = wdToUser_[e->wd];
            }
            std::string fn(e->name);
            std::string key = user + "/" + fn;
            {
                std::lock_guard lk(syncMtx_);
                if (syncing_.erase(key)) {
                    i += sizeof(*e) + e->len;
                    continue;
                }
            }
            if (e->mask & IN_CLOSE_WRITE) {
                std::vector<char> data;
                if (fm_.loadFile(user, fn, data)) {
                    Packet p{ CMD_UPLOAD, 0, {} };
                    uint32_t nl = htonl((uint32_t)fn.size()),
                             dl = htonl((uint32_t)data.size());
                    p.payload.insert(p.payload.end(), (char*)&nl,(char*)&nl+4);
                    p.payload.insert(p.payload.end(), fn.begin(),fn.end());
                    p.payload.insert(p.payload.end(), (char*)&dl,(char*)&dl+4);
                    p.payload.insert(p.payload.end(), data.begin(),data.end());
                    p.length = p.payload.size();
                    broadcast(user, p, -1);
                    std::cout << "[server] broadcast UPLOAD '" << fn << "'\n";
                }
            }
            else if (e->mask & (IN_DELETE | IN_MOVED_FROM)) {
                Packet p{ CMD_DELETE,
                          static_cast<uint32_t>(fn.size()),
                          std::vector<char>(fn.begin(), fn.end()) };
                broadcast(user, p, -1);
                std::cout << "[server] broadcast DELETE '" << fn << "'\n";
            }
            i += sizeof(*e) + e->len;
        }
    }
}

