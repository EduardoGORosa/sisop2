// File: src/main_client.cpp
#include "Client.hpp"
#include <iostream>
#include "Packet.hpp"

int main(int argc, char** argv){
    if(argc!=4){
        std::cerr<<"Usage: "<<argv[0]<<" <username> <server_ip> <port>\n";
        return 1;
    }
    std::string u=argv[1], ip=argv[2];
    uint16_t port=std::stoi(argv[3]);
    Client c(u,ip,port);
    c.run();
    return 0;
}

