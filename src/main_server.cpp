// File: src/main_server.cpp
#include "Server.hpp"
#include <iostream>

int main(int argc, char** argv){
    if(argc!=3){
        std::cerr<<"Usage: "<<argv[0]<<" <ip> <port>\n";
        return 1;
    }
    std::string ip=argv[1];
    uint16_t port=std::stoi(argv[2]);
    Server s(ip,port,"server_storage");
    s.run();
    return 0;
}

