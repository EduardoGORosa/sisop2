# Dropbox-like Service - Final Implementation

This directory contains the complete, corrected implementation for the assignment.

## Quick Start

1. **Compile everything**:
```bash
make clean && make all
```

2. **Start the system**:
```bash
./start_system.sh
```

3. **Connect a client** (in another terminal):
```bash
./client 127.0.0.1 9000
```

## What's Included

### Core Implementation Files:
- `Server_improved.hpp/cpp` - Enhanced server with passive replication
- `Bully_improved.hpp/cpp` - Robust Bully algorithm implementation  
- `FrontEnd.hpp/cpp` - Front-End component for client transparency
- `Packet.hpp/cpp` - Enhanced packet system with new types
- `main_server_improved.cpp` - Server main program
- `main_frontend.cpp` - Front-End main program

### Original Files (Reused):
- `Connection.hpp/cpp` - Network communication
- `FileManager.hpp/cpp` - File operations
- `Client.hpp/cpp` - Client implementation
- `main_client.cpp` - Client main program

### Build & Test:
- `Makefile` - Complete build system
- `start_system.sh` - Automated startup script

### Documentation:
- `README.md` - Comprehensive documentation
- `CHANGES_SUMMARY.md` - Summary of all fixes made
- `TESTING_GUIDE.md` - Detailed testing instructions

## Key Features Implemented

✅ **Front-End (FE)** - Provides client transparency
✅ **Passive Replication** - Primary waits for backup acknowledgments  
✅ **Leader Election** - Robust Bully algorithm with proper failure detection
✅ **Automatic Setup** - No manual directory creation needed

## Architecture

```
Client → Front-End → Primary RM → Backup RMs
                         ↓
                    Wait for ACKs
                         ↓
                    Confirm to Client
```

This implementation fully satisfies all assignment requirements!

