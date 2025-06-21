// File: src/main_server.cpp
#include "Server.hpp"
#include <iostream>

int main(int argc, char** argv){
    if(argc!=4){
        std::cerr<<"Usage: "<<argv[0]<<" <ip> <port>\n";
        return 1;
    }
    std::string ip=argv[1];
    uint16_t port=std::stoi(argv[2]);
    int new_leader = std::stoi(argv[3]);
    Server s(ip,port,"server_storage");
    s.run(new_leader);
    return 0;
}

