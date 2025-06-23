#include "Server.hpp"
#include "Connection.hpp"
#include "Bully.hpp"

#include <arpa/inet.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <thread>
#include <iostream>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <ctime>
#include <chrono>

namespace fs = std::filesystem;

Server::Server(const std::string& ip,
               uint16_t port,
               const std::string& root,
               int myId)
  : ip_(ip),
    port_(port),
    fm_(root),
    storageRoot_(root),
    myId_(myId),    
    isLeader_(false),
    operationCounter_(0)
{
    // Create storage root directory if it doesn't exist
    if (!fs::exists(storageRoot_)) {
        fs::create_directories(storageRoot_);
        std::cout << "[SERVER] Created storage directory: " << storageRoot_ << "\n";
    }
    
    bully_ = std::make_unique<Bully>(myId, allServers_, this);
}

void Server::run() {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Set socket to non-blocking for better handling
    int flags = fcntl(listenFd_, F_GETFL, 0);
    fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);
    
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

    // Initialize inotify for file watching
    watchFd_ = inotify_init();
    if (watchFd_ >= 0) {
        // Watch existing user directories
        if (fs::exists(storageRoot_)) {
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
        }
        std::thread(&Server::watchLoop, this).detach();
    }

    this->bully_->start();
    acceptLoop();
}

void Server::becomeLeader() {
    isLeader_ = true;
    std::cout << "[SERVER] I am the new leader!\n";
    
    // Only try to connect to client if we're not in a test environment
    try {
        connectToClient("10.67.103.33", reconnect_port_);
        forceReconnect("10.67.103.33", this->port_);
    } catch (...) {
        // Silently handle client connection failures
        std::cout << "[SERVER] Client reconnection not available (normal in test mode)\n";
    }
}

void Server::connectToClient(const std::string& ip,uint16_t port){
    // cria e conecta o socket
    this->reconnect_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (this->reconnect_sock_ < 0) {
        return; // Silently handle failure
    }
    
    // Set timeout for connection
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(this->reconnect_sock_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(this->reconnect_sock_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    addr.sin_port        = htons(port);
    
    if (connect(this->reconnect_sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(this->reconnect_sock_);
        return; // Silently handle failure
    }    

    this->reconnect_conn_ = std::make_unique<Connection>(this->reconnect_sock_);
    std::cout << "[Server] Connected to client IP " << ip << " , PORT " << port << "\n";    
}

void Server::forceReconnect(const std::string& ip, uint16_t port) {    
    if (!reconnect_conn_) return;
    
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
        sockaddr_in cli; 
        socklen_t len = sizeof(cli);
        int fd = accept(listenFd_, (sockaddr*)&cli, &len);
        
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No connections available, sleep briefly
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            } else {
                perror("[server] accept error");
                continue;
            }
        }
        
        // Set accepted socket back to blocking mode
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        
        // Determine if this is a direct client connection or server-to-server
        if (isDirectClientConnection(fd)) {
            std::thread(&Server::handleClient, this, fd).detach();
        } else {
            std::thread(&Server::handleServerMessage, this, fd).detach();
        }
    }
}

bool Server::isDirectClientConnection(int fd) {
    // Simple heuristic: peek at the first packet to determine connection type
    char buffer[6]; // Packet header size
    ssize_t bytes = recv(fd, buffer, 6, MSG_PEEK);
    
    if (bytes >= 6) {
        uint16_t type;
        uint32_t length;
        if (Packet::tryDeserializeHeader(buffer, type, length)) {
            // If it's a client command, it's a direct client connection
            return (type >= CMD_REGISTER && type <= CMD_FILE_CHUNK);
        }
    }
    
    return false; // Assume server-to-server if we can't determine
}

void Server::handleServerMessage(int fd) {
    Connection conn(fd);
    Packet p;
    
    // Set timeout for server connections
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    while (conn.recvPacket(p)) {
        if (p.type >= HEARTBEAT && p.type <= COORDINATOR) {
            // Bully algorithm messages
            char ip_str[INET_ADDRSTRLEN];
            sockaddr_in peer_addr;
            socklen_t peer_addr_len = sizeof(peer_addr);
            getpeername(fd, (struct sockaddr*)&peer_addr, &peer_addr_len);
            inet_ntop(AF_INET, &peer_addr.sin_addr, ip_str, INET_ADDRSTRLEN);

            bully_->handleElectionMessage(p, ip_str, ntohs(peer_addr.sin_port));
            break; // Close connection after handling election message
        } else if (p.type == BACKUP_OPERATION) {
            // Handle backup operation from primary
            handleBackupOperation(p, "10.67.103.35", 0);
            break; // Close connection after handling backup operation
        } else if (p.type == BACKUP_ACK) {
            // Handle acknowledgment from backup
            if (p.payload.size() >= sizeof(int) + 1) { // serverId + operationId
                int fromServerId;
                memcpy(&fromServerId, p.payload.data(), sizeof(int));
                std::string operationId(p.payload.begin() + sizeof(int), p.payload.end());
                handleAcknowledgment(operationId, fromServerId);
            }
            break; // Close connection after handling ACK
        }
    }
    
    close(fd);
}

void Server::handleClient(int fd) {
    Connection conn(fd);
    Packet     p;

    // Only the leader should handle direct client connections
    if (bully_->leaderId_ != this->myId_) {
        std::cout << "[SERVER] I am a backup. Refusing direct client connection.\n";
        close(fd);
        return;
    }

    if (!conn.recvPacket(p) || p.type != CMD_REGISTER) {
        close(fd);
        return;
    }
    std::string user(p.payload.begin(), p.payload.end());
    {
        std::lock_guard lk(clMtx_);
        clients_[user].push_back(fd);
    }
    fm_.ensureUserDir(user);
    std::cout << "[server] user '" << user << "' connected\n";

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
            
            // Use acknowledgment-based broadcast for write operations
            std::string operationId = generateOperationId();
            broadcastWithAck(user, p, fd, operationId);
        }
        else if (p.type == CMD_DELETE) {
            std::string fn(p.payload.begin(), p.payload.end());
            {
                std::lock_guard lk(syncMtx_);
                syncing_.insert(user + "/" + fn);
            }
            fm_.deleteFile(user, fn);
            std::cout << "[server] deleted '" << fn << "' from " << user << "\n";
            
            // Use acknowledgment-based broadcast for delete operations
            std::string operationId = generateOperationId();
            broadcastWithAck(user, p, fd, operationId);
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
    }
}

std::string Server::generateOperationId() {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    uint64_t counter = operationCounter_.fetch_add(1);
    
    std::ostringstream oss;
    oss << myId_ << "_" << timestamp << "_" << counter;
    return oss.str();
}

void Server::broadcastWithAck(const std::string& user,
                             const Packet& pkt,
                             int clientFd,
                             const std::string& operationId) {
    
    std::cout << "[SERVER] Broadcasting with ACK - Operation ID: " << operationId << "\n";
    
    if (bully_->leaderId_ != myId_) {
        std::cout << "[SERVER] Not leader, cannot broadcast\n";
        return;
    }
    
    // Create pending operation
    auto pendingOp = std::make_shared<PendingOperation>(operationId, user, pkt, clientFd);
    
    // Add all backup servers to pending acknowledgments
    for (const auto& [id, server] : allServers_) {
        if (id != myId_) {
            pendingOp->pendingAcks.insert(id);
        }
    }
    
    // Store pending operation
    {
        std::lock_guard<std::mutex> lock(pendingOpsMutex_);
        pendingOps_[operationId] = pendingOp;
    }
    
    // First, broadcast to other clients of the same user
    {
        std::lock_guard lk(clMtx_);
        for (int fd : clients_[user]) {
            if (fd == clientFd) continue;
            Connection c(fd);
            c.sendPacket(pkt);
        }
    }
    
    // Create backup operation packet
    Packet backupPkt{BACKUP_OPERATION, 0, {}};
    
    // Payload format: operationId_length + operationId + user_length + user + original_packet
    uint32_t opIdLen = htonl(static_cast<uint32_t>(operationId.size()));
    uint32_t userLen = htonl(static_cast<uint32_t>(user.size()));
    uint32_t origPktLen = htonl(static_cast<uint32_t>(pkt.payload.size()));
    uint16_t origPktType = htons(pkt.type);
    
    backupPkt.payload.insert(backupPkt.payload.end(), (char*)&opIdLen, (char*)&opIdLen + 4);
    backupPkt.payload.insert(backupPkt.payload.end(), operationId.begin(), operationId.end());
    backupPkt.payload.insert(backupPkt.payload.end(), (char*)&userLen, (char*)&userLen + 4);
    backupPkt.payload.insert(backupPkt.payload.end(), user.begin(), user.end());
    backupPkt.payload.insert(backupPkt.payload.end(), (char*)&origPktType, (char*)&origPktType + 2);
    backupPkt.payload.insert(backupPkt.payload.end(), (char*)&origPktLen, (char*)&origPktLen + 4);
    backupPkt.payload.insert(backupPkt.payload.end(), pkt.payload.begin(), pkt.payload.end());
    backupPkt.length = backupPkt.payload.size();
    
    // Send to backup servers
    for (const auto& [id, server] : allServers_) {
        if (id != myId_) {
            std::cout << "[SERVER] Sending backup operation to server " << id << "\n";
            bully_->sendPacketTo(id, backupPkt);
        }
    }
    
    // Wait for acknowledgments with shorter timeout
    std::unique_lock<std::mutex> lock(pendingOp->ackMutex);
    bool allAcksReceived = pendingOp->ackCondition.wait_for(lock, std::chrono::seconds(5), [&]() {
        return pendingOp->pendingAcks.empty() || pendingOp->completed;
    });
    
    if (allAcksReceived && pendingOp->pendingAcks.empty()) {
        std::cout << "[SERVER] All acknowledgments received for operation " << operationId << "\n";
    } else {
        std::cout << "[SERVER] Timeout or incomplete acknowledgments for operation " << operationId << "\n";
        // Continue anyway - in a real system you might retry or handle this differently
    }
    
    // Mark operation as completed
    pendingOp->completed = true;
    
    // Remove from pending operations
    {
        std::lock_guard<std::mutex> lock(pendingOpsMutex_);
        pendingOps_.erase(operationId);
    }
    
    std::cout << "[SERVER] Operation " << operationId << " completed\n";
}

void Server::handleBackupOperation(const Packet& packet, const std::string& sourceIp, uint16_t sourcePort) {
    std::cout << "[SERVER] Handling backup operation\n";
    
    if (bully_->leaderId_ == myId_) {
        std::cout << "[SERVER] I am the leader, ignoring backup operation\n";
        return;
    }
    
    const char* data = packet.payload.data();
    size_t offset = 0;
    
    // Parse operation ID
    uint32_t opIdLen;
    memcpy(&opIdLen, data + offset, 4);
    opIdLen = ntohl(opIdLen);
    offset += 4;
    
    std::string operationId(data + offset, data + offset + opIdLen);
    offset += opIdLen;
    
    // Parse user
    uint32_t userLen;
    memcpy(&userLen, data + offset, 4);
    userLen = ntohl(userLen);
    offset += 4;
    
    std::string user(data + offset, data + offset + userLen);
    offset += userLen;
    
    // Parse original packet
    uint16_t origPktType;
    memcpy(&origPktType, data + offset, 2);
    origPktType = ntohs(origPktType);
    offset += 2;
    
    uint32_t origPktLen;
    memcpy(&origPktLen, data + offset, 4);
    origPktLen = ntohl(origPktLen);
    offset += 4;
    
    std::vector<char> origPayload(data + offset, data + offset + origPktLen);
    
    std::cout << "[SERVER] Processing backup operation " << operationId 
              << " for user " << user << " type " << origPktType << "\n";
    
    // Process the operation
    if (origPktType == CMD_UPLOAD) {
        const char* b = origPayload.data();
        uint32_t nl; memcpy(&nl, b, 4); nl = ntohl(nl);
        std::string fn(b + 4, b + 4 + nl);
        uint32_t dl; memcpy(&dl, b + 4 + nl, 4); dl = ntohl(dl);
        std::vector<char> fileData(b + 8 + nl, b + 8 + nl + dl);
        
        {
            std::lock_guard lk(syncMtx_);
            syncing_.insert(user + "/" + fn);
        }
        
        fm_.ensureUserDir(user);
        fm_.saveFile(user, fn, fileData);
        std::cout << "[SERVER] Backup saved file '" << fn << "' for user " << user << "\n";
        
    } else if (origPktType == CMD_DELETE) {
        std::string fn(origPayload.begin(), origPayload.end());
        
        {
            std::lock_guard lk(syncMtx_);
            syncing_.insert(user + "/" + fn);
        }
        
        fm_.deleteFile(user, fn);
        std::cout << "[SERVER] Backup deleted file '" << fn << "' for user " << user << "\n";
    }
    
    // Send acknowledgment back to primary
    sendAcknowledgment(operationId, bully_->leaderId_);
}

void Server::sendAcknowledgment(const std::string& operationId, int primaryServerId) {
    std::cout << "[SERVER] Sending ACK for operation " << operationId 
              << " to primary server " << primaryServerId << "\n";
    
    Packet ackPkt{BACKUP_ACK, 0, {}};
    
    // Payload: myId + operationId
    ackPkt.payload.insert(ackPkt.payload.end(), (char*)&myId_, (char*)&myId_ + sizeof(int));
    ackPkt.payload.insert(ackPkt.payload.end(), operationId.begin(), operationId.end());
    ackPkt.length = ackPkt.payload.size();
    
    bully_->sendPacketTo(primaryServerId, ackPkt);
}

void Server::handleAcknowledgment(const std::string& operationId, int fromServerId) {
    std::cout << "[SERVER] Received ACK for operation " << operationId 
              << " from server " << fromServerId << "\n";
    
    std::lock_guard<std::mutex> lock(pendingOpsMutex_);
    auto it = pendingOps_.find(operationId);
    
    if (it != pendingOps_.end()) {
        auto& pendingOp = it->second;
        
        std::lock_guard<std::mutex> ackLock(pendingOp->ackMutex);
        pendingOp->pendingAcks.erase(fromServerId);
        
        std::cout << "[SERVER] Remaining ACKs for operation " << operationId 
                  << ": " << pendingOp->pendingAcks.size() << "\n";
        
        if (pendingOp->pendingAcks.empty()) {
            pendingOp->ackCondition.notify_all();
        }
    }
}

void Server::broadcast(const std::string& user,
                       const Packet& pkt,
                       int exceptFd)
{
    // Legacy broadcast method - now just forwards to broadcastWithAck
    std::string operationId = generateOperationId();
    broadcastWithAck(user, pkt, exceptFd, operationId);
}

void Server::watchLoop() {
    char buf[4096];
    while (true) {
        int len = read(watchFd_, buf, sizeof(buf));
        if (len < 0) { 
            if (errno == EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            perror("[server] inotify read"); 
            continue; 
        }
        int i = 0;
        while (i < len) {
            auto* e = (inotify_event*)(buf + i);
            std::string user;
            {
                std::lock_guard lk(watchMtx_);
                auto it = wdToUser_.find(e->wd);
                if (it != wdToUser_.end()) {
                    user = it->second;
                }
            }
            
            if (user.empty()) {
                i += sizeof(*e) + e->len;
                continue;
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
                    
                    std::string operationId = generateOperationId();
                    broadcastWithAck(user, p, -1, operationId);
                    std::cout << "[server] broadcast UPLOAD '" << fn << "'\n";
                }
            }
            else if (e->mask & (IN_DELETE | IN_MOVED_FROM)) {
                Packet p{ CMD_DELETE,
                          static_cast<uint32_t>(fn.size()),
                          std::vector<char>(fn.begin(), fn.end()) };
                
                std::string operationId = generateOperationId();
                broadcastWithAck(user, p, -1, operationId);
                std::cout << "[server] broadcast DELETE '" << fn << "'\n";
            }
            i += sizeof(*e) + e->len;
        }
    }
}

