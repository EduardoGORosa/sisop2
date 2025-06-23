// src/Client.cpp

#include "Client.hpp"
#include "Connection.hpp"

#include <arpa/inet.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

Client::Client(const std::string& u,
               const std::string& ip,
               uint16_t p)
  : user_(u),
    ip_(ip),
    port_(p),
    fm_("client_storage"),
    syncDir_("client_storage/sync_dir_" + u),
    running_(true)
{
    // garante que o diretório de sync exista
    fs::create_directories(syncDir_); 
    
    listenReconnection();

    sock_ = -1;   // para dizer a função connectToServer que esta é a primeira conexão
    connectToServer(ip, p);
    
    // inicializa inotify (não‐bloqueante)
    inotifyFd_ = inotify_init1(IN_NONBLOCK);
    if (inotifyFd_ < 0) {
        perror("[client] inotify_init1");
        std::exit(1);
    }
    inotifyWd_ = inotify_add_watch(
        inotifyFd_,
        syncDir_.c_str(),
        IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM
    );
    if (inotifyWd_ < 0) {
        perror("[client] inotify_add_watch");
        std::exit(1);
    }
}

Client::~Client() {
    running_ = false;
    close(sock_);
    inotify_rm_watch(inotifyFd_, inotifyWd_);
    close(inotifyFd_);
}

void Client::connectToServer(const std::string& ip,uint16_t port){
    if (sock_ >= 0) {                
        close(this->sock_);
    }
    
    // cria e conecta o socket
    this->sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (this->sock_ < 0) {
        perror("[client] socket");
        std::exit(1);
    }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    addr.sin_port        = htons(port);
    if (connect(this->sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[client] connect");
        std::exit(1);
    }    

    this->conn_ = std::make_unique<Connection>(this->sock_);
    std::cout << "[client] Connected to IP " << ip << " , PORT " << port << "\n";
    sendRegister();

    if (srv_.joinable()) {  // Caso de reconexão, encerrar a thread anterior
        srv_.detach();
    }
    srv_ = std::thread(&Client::serverLoop, this);    
}

void Client::run() {    
    std::thread wch(&Client::watchLoop,  this);
    userLoop();    
    wch.join();
}

void Client::sendRegister() {    
    Packet p{
        CMD_REGISTER,
        static_cast<uint32_t>(user_.size()),
        std::vector<char>(user_.begin(), user_.end())
    };
    conn_->sendPacket(p);
    std::cout << "[client] registration sent\n";
}

void Client::listenReconnection() {
    this->reconnect_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(this->reconnect_sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip_.c_str());
    addr.sin_port        = htons(reconnect_port_);

    if(bind(this->reconnect_sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[Client] Error on bind:");  
        exit(1);
    }
    if(listen(this->reconnect_sock_, 10) < 0) {
        perror("[Client] Error on listen:");  
        exit(1);
    }        
    std::thread(&Client::acceptLoop, this).detach();
    std::cout << "[Client] listening on " << ip_ << ":" << reconnect_port_ << "\n";
}

void Client::acceptLoop() {
    while (true) {
        sockaddr_in cli; 
        socklen_t len = sizeof(cli);
        int fd = accept(reconnect_sock_, (sockaddr*)&cli, &len);

        Connection reconnect_conn(fd);
        Packet p;
        if (reconnect_conn.recvPacket(p) && p.type == CMD_REGISTER) {            
                        
            uint8_t ip_len = static_cast<uint8_t>(p.payload[0]);
            std::string new_ip(p.payload.begin() + 1, p.payload.begin() + 1 + ip_len);

            uint16_t new_port;
            std::memcpy(&new_port, &p.payload[1 + ip_len], sizeof(new_port));
            new_port = ntohs(new_port);  // se você usou htons para enviar
            
            connectToServer(new_ip, new_port);
        }
        
    }
}

void Client::userLoop() {    
    while (running_) {
        std::cout << "client> ";
        std::string line;
        if (!std::getline(std::cin, line)) break;

        std::istringstream iss(line);
        std::string cmd; iss >> cmd;

        if (cmd == "help") {
            std::cout << "Commands:\n"
                      << "  get_sync_dir         Show sync directory path\n"
                      << "  upload <path>\n"
                      << "  download <filename>\n"
                      << "  delete <filename>\n"
                      << "  list_server\n"
                      << "  list_client\n"
                      << "  exit\n";
        }
        else if (cmd == "get_sync_dir") {
            std::cout << "[client] sync directory: " << syncDir_ << "\n";
        }
        else if (cmd == "upload") {
            std::string path;
            if (!(iss >> path)) {
                std::cout << "Usage: upload <path>\n";
            } else {
                sendUpload(path);
            }
        }
        else if (cmd == "download") {
            std::string fn;
            if (!(iss >> fn)) {
                std::cout << "Usage: download <filename>\n";
            } else {
                Packet p{
                    CMD_DOWNLOAD,
                    static_cast<uint32_t>(fn.size()),
                    std::vector<char>(fn.begin(), fn.end())
                };
                conn_->sendPacket(p);
            }
        }
        else if (cmd == "delete") {
            std::string fn;
            if (!(iss >> fn)) {
                std::cout << "Usage: delete <filename>\n";
            } else {
                sendDelete(fn);
            }
        }
        else if (cmd == "list_server") {
            Packet p{ CMD_LIST_SERVER, 0, {} };
            conn_->sendPacket(p);
        }
        else if (cmd == "list_client") {
            std::cout << "[client] local files:\n";
            for (auto& e : fs::directory_iterator(syncDir_)) {
                if (!e.is_regular_file()) continue;
                auto path = e.path();
                struct stat st;
                if (stat(path.c_str(), &st) == 0) {
                    auto at = std::localtime(&st.st_atime);
                    auto mt = std::localtime(&st.st_mtime);
                    auto ct = std::localtime(&st.st_ctime);
                    std::cout << "  " << path.filename()
                              << "  size=" << st.st_size
                              << "  atime=" << std::put_time(at, "%F %T")
                              << "  mtime=" << std::put_time(mt, "%F %T")
                              << "  ctime=" << std::put_time(ct, "%F %T")
                              << "\n";
                }
            }
        }
        else if (cmd == "exit") {
            Packet p{ CMD_EXIT, 0, {} };
            conn_->sendPacket(p);
            break;
        }
        else if (cmd == "reconnect") {            
            std::uint16_t new_port;
            if (!(iss >> new_port)) {
                std::cout << "Usage: reconnect <port>\n";
            } else {
                connectToServer("10.67.103.33", new_port);                
            }
        }
        else if (!cmd.empty()) {
            std::cout << "[client] unknown command: " << cmd << "\n";
        }
    }
    running_ = false;
}

void Client::serverLoop() {    
    Packet p;
    while (running_ && conn_->recvPacket(p)) {
        handleServerPacket(p);
    }
    std::cout << "[client] serverLoop exiting\n";
}

void Client::handleServerPacket(const Packet& p) {
    const char* b = p.payload.data();    
    
    if (p.type == CMD_UPLOAD) {
        
        if (p.payload.size() < 8) {
            std::cerr << "[client] pacote upload malformado (sem espaço p/ cabeçalho)\n";
            return;
        }
        
        uint32_t nl; std::memcpy(&nl,b,4); nl = ntohl(nl);
        if (p.payload.size() < 8 + nl) {
            std::cerr << "[client] nome do arquivo fora do payload\n";
            return;
        }
        
        std::string fn(b+4, b+4+nl);
        uint32_t dl; std::memcpy(&dl,b+4+nl,4); dl = ntohl(dl);
        if (p.payload.size() < 8 + nl + dl) {
            std::cerr << "[client] conteúdo do arquivo fora do payload\n";
            return;
        }
        
        std::vector<char> dat(b+8+nl, b+8+nl+dl);

        std::string full = syncDir_ + "/" + fn;
        // se já existe e é igual, ignora
        if (fs::exists(full) && fs::is_regular_file(full) && fs::file_size(full) == dat.size()) {
            std::ifstream ex(full, std::ios::binary|std::ios::ate);
            std::vector<char> buf(dat.size());
            ex.seekg(0);
            ex.read(buf.data(), buf.size());
            if (buf == dat) {
                std::lock_guard lk(syncMtx_);
                syncing_.insert(fn);
                return;
            }
        }

        {
            std::lock_guard lk(syncMtx_);
            syncing_.insert(fn);
        }
        std::ofstream ofs(full, std::ios::binary);
        ofs.write(dat.data(), dat.size());
        std::cout << "[client] synced: " << fn << "\n";
    }
    else if (p.type == CMD_DELETE) {
        std::string fn(p.payload.begin(), p.payload.end());
        std::string full = syncDir_ + "/" + fn;
        {
            std::lock_guard lk(syncMtx_);
            syncing_.erase(fn);
        }
        if (fs::exists(full)) {
            fs::remove(full);
            std::cout << "[client] deleted: " << fn << "\n";
        }
    }
    else if (p.type == CMD_LIST_SERVER) {
        // payload já contém linhas com name, size, atime, mtime, ctime        
        std::cout.write(p.payload.data(), p.length);
    }
    else if (p.type == CMD_FILE_CHUNK) {
        uint32_t nl; std::memcpy(&nl,b,4); nl = ntohl(nl);
        std::string fn(b+4, b+4+nl);
        uint32_t dl; std::memcpy(&dl,b+4+nl,4); dl = ntohl(dl);
        std::vector<char> dat(b+8+nl, b+8+nl+dl);

        // grava em cwd
        std::ofstream ofs(fn, std::ios::binary);
        ofs.write(dat.data(), dat.size());
        ofs.close();

        // imprime metadados do download
        struct stat st;
        if (stat(fn.c_str(), &st) == 0) {
            auto at = std::localtime(&st.st_atime);
            auto mt = std::localtime(&st.st_mtime);
            auto ct = std::localtime(&st.st_ctime);
            std::cout << "[client] downloaded: " << fn
                      << "  size=" << st.st_size
                      << "  atime=" << std::put_time(at, "%F %T")
                      << "  mtime=" << std::put_time(mt, "%F %T")
                      << "  ctime=" << std::put_time(ct, "%F %T")
                      << "\n";
        }
    }
}

void Client::sendDelete(const std::string& fn) {    
    Packet p{
        CMD_DELETE,
        static_cast<uint32_t>(fn.size()),
        std::vector<char>(fn.begin(), fn.end())
    };
    conn_->sendPacket(p);
    std::cout << "[client] delete sent: " << fn << "\n";
}

void Client::sendUpload(const std::string& path) {
    std::string fn = fs::path(path).filename().string();
    std::ifstream ifs(path, std::ios::binary|std::ios::ate);
    if (!ifs) {
        std::cout << "[client] cannot open: " << path << "\n";
        return;
    }
    auto sz = ifs.tellg(); ifs.seekg(0);
    std::vector<char> dat(sz);
    ifs.read(dat.data(), sz);
    
    Packet p{ CMD_UPLOAD, 0, {} };
    uint32_t nl = htonl((uint32_t)fn.size()),
             dl = htonl((uint32_t)dat.size());
    p.payload.insert(p.payload.end(), (char*)&nl, (char*)&nl+4);
    p.payload.insert(p.payload.end(), fn.begin(), fn.end());
    p.payload.insert(p.payload.end(), (char*)&dl, (char*)&dl+4);
    p.payload.insert(p.payload.end(), dat.begin(), dat.end());
    p.length = p.payload.size();
    conn_->sendPacket(p);
    std::cout << "[client] upload sent: " << fn << "\n";
}

void Client::watchLoop() {
    constexpr size_t BUF_SZ = 4096;
    std::vector<char> buf(BUF_SZ);

    while (running_) {
        int len = read(inotifyFd_, buf.data(), BUF_SZ);
        if (len < 0) {
            if (errno == EINTR) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (len == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        auto* e = reinterpret_cast<inotify_event*>(buf.data());
        if (e->len) {
            std::string fn(e->name);
            {
                std::lock_guard lk(syncMtx_);
                if (syncing_.erase(fn))
                    continue;
            }
            if (e->mask & IN_CLOSE_WRITE) {
                sendUpload(syncDir_ + "/" + fn);
            }
            else if (e->mask & (IN_DELETE | IN_MOVED_FROM)) {
                sendDelete(fn);
            }
        }
    }
}

