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

    // cria e conecta o socket
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
        perror("[client] socket");
        std::exit(1);
    }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    addr.sin_port        = htons(port_);
    if (connect(sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[client] connect");
        std::exit(1);
    }

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

void Client::run() {
    sendRegister();
    std::thread srv(&Client::serverLoop, this);
    std::thread wch(&Client::watchLoop,  this);
    userLoop();
    srv.join();
    wch.join();
}

void Client::sendRegister() {
    Connection c(sock_);
    Packet p{
        CMD_REGISTER,
        static_cast<uint32_t>(user_.size()),
        std::vector<char>(user_.begin(), user_.end())
    };
    c.sendPacket(p);
    std::cout << "[client] registration sent\n";
}

void Client::userLoop() {
    Connection c(sock_);
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
                c.sendPacket(p);
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
            c.sendPacket(p);
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
            c.sendPacket(p);
            break;
        }
        else if (!cmd.empty()) {
            std::cout << "[client] unknown command: " << cmd << "\n";
        }
    }
    running_ = false;
}

void Client::serverLoop() {
    Connection c(sock_);
    Packet p;
    while (running_ && c.recvPacket(p)) {
        handleServerPacket(p);
    }
    std::cout << "[client] serverLoop exiting\n";
}

void Client::handleServerPacket(const Packet& p) {
    const char* b = p.payload.data();

    if (p.type == CMD_UPLOAD) {
        uint32_t nl; std::memcpy(&nl,b,4); nl = ntohl(nl);
        std::string fn(b+4, b+4+nl);
        uint32_t dl; std::memcpy(&dl,b+4+nl,4); dl = ntohl(dl);
        std::vector<char> dat(b+8+nl, b+8+nl+dl);

        std::string full = syncDir_ + "/" + fn;
        // se já existe e é igual, ignora
        if (fs::exists(full) && fs::file_size(full) == dat.size()) {
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
    Connection c(sock_);
    Packet p{
        CMD_DELETE,
        static_cast<uint32_t>(fn.size()),
        std::vector<char>(fn.begin(), fn.end())
    };
    c.sendPacket(p);
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

    Connection c(sock_);
    Packet p{ CMD_UPLOAD, 0, {} };
    uint32_t nl = htonl((uint32_t)fn.size()),
             dl = htonl((uint32_t)dat.size());
    p.payload.insert(p.payload.end(), (char*)&nl, (char*)&nl+4);
    p.payload.insert(p.payload.end(), fn.begin(), fn.end());
    p.payload.insert(p.payload.end(), (char*)&dl, (char*)&dl+4);
    p.payload.insert(p.payload.end(), dat.begin(), dat.end());
    p.length = p.payload.size();
    c.sendPacket(p);
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

