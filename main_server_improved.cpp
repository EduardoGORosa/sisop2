#include "Server_improved.hpp"
#include <iostream>
#include <signal.h>

Server* server = nullptr;

void signalHandler(int signal) {
    if (server) {
        std::cout << "\n[SERVER] Shutting down..." << std::endl;
        // In a real implementation, you would call server->stop()
    }
    exit(0);
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cout << "Usage: " << argv[0] << " <server_id> <ip> <port> <storage_root>" << std::endl;
        std::cout << "Example: " << argv[0] << " 10 127.0.0.1 8001 ./storage_10" << std::endl;
        return 1;
    }
    
    int serverId = std::stoi(argv[1]);
    std::string ip = argv[2];
    uint16_t port = static_cast<uint16_t>(std::stoi(argv[3]));
    std::string storageRoot = argv[4];
    
    // Validate server ID
    if (serverId != 10 && serverId != 20 && serverId != 30) {
        std::cout << "Error: Server ID must be 10, 20, or 30" << std::endl;
        return 1;
    }
    
    // Set up signal handler for graceful shutdown
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    std::cout << "[SERVER] Starting server " << serverId 
              << " on " << ip << ":" << port 
              << " with storage: " << storageRoot << std::endl;
    
    server = new Server(ip, port, storageRoot, serverId);
    server->run();
    
    delete server;
    return 0;
}

