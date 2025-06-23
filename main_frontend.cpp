#include "FrontEnd.hpp"
#include <iostream>
#include <signal.h>

FrontEnd* frontEnd = nullptr;

void signalHandler(int signal) {
    if (frontEnd) {
        std::cout << "\n[FE] Shutting down..." << std::endl;
        frontEnd->stop();
    }
    exit(0);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "Usage: " << argv[0] << " <ip> <port>" << std::endl;
        return 1;
    }
    
    std::string ip = argv[1];
    uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
    
    // Set up signal handler for graceful shutdown
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    frontEnd = new FrontEnd(ip, port);
    
    std::cout << "[FE] Starting Front-End on " << ip << ":" << port << std::endl;
    
    frontEnd->run();
    
    delete frontEnd;
    return 0;
}

