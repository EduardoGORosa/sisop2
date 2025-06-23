CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -O2
INCLUDES = -I.

# Executables
TARGETS = server_improved frontend client

.PHONY: all clean test copy_sources setup_test help

all: copy_sources $(TARGETS)

# Copy necessary source files from upload directory
copy_sources:
	@echo "Copying source files..."
	@cp /home/ubuntu/upload/Connection.cpp . 2>/dev/null || true
	@cp /home/ubuntu/upload/Connection.hpp . 2>/dev/null || true
	@cp /home/ubuntu/upload/FileManager.cpp . 2>/dev/null || true
	@cp /home/ubuntu/upload/FileManager.hpp . 2>/dev/null || true
	@cp /home/ubuntu/upload/Client.cpp . 2>/dev/null || true
	@cp /home/ubuntu/upload/Client.hpp . 2>/dev/null || true
	@cp /home/ubuntu/upload/main_client.cpp . 2>/dev/null || true
	@cp /home/ubuntu/Packet_improved.hpp Packet.hpp 2>/dev/null || true
	@echo "Source files copied."

# Build improved server
server_improved: copy_sources
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c Connection.cpp -o Connection.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c FileManager.cpp -o FileManager.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c Packet.cpp -o Packet.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c Server_improved.cpp -o Server_improved.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c Bully_improved.cpp -o Bully_improved.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c main_server_improved.cpp -o main_server_improved.o
	$(CXX) $(CXXFLAGS) -o $@ Connection.o FileManager.o Packet.o Server_improved.o Bully_improved.o main_server_improved.o

# Build front-end
frontend: copy_sources
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c Connection.cpp -o Connection.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c Packet.cpp -o Packet.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c FrontEnd.cpp -o FrontEnd.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c main_frontend.cpp -o main_frontend.o
	$(CXX) $(CXXFLAGS) -o $@ Connection.o Packet.o FrontEnd.o main_frontend.o

# Build client (using original client code)
client: copy_sources
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c Connection.cpp -o Connection.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c FileManager.cpp -o FileManager.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c Packet.cpp -o Packet.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c Client.cpp -o Client.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c main_client.cpp -o main_client.o
	$(CXX) $(CXXFLAGS) -o $@ Connection.o FileManager.o Packet.o Client.o main_client.o

# Test targets
test: all
	@echo "Running basic compilation test..."
	@./test_basic.sh

# Clean build artifacts
clean:
	rm -f *.o $(TARGETS)
	rm -f Connection.cpp Connection.hpp FileManager.cpp FileManager.hpp
	rm -f Client.cpp Client.hpp main_client.cpp Packet.hpp
	rm -rf storage_*
	rm -f test_*.log

# Create storage directories for testing
setup_test:
	mkdir -p storage_10 storage_20 storage_30
	mkdir -p storage_10/testuser storage_20/testuser storage_30/testuser

# Help target
help:
	@echo "Available targets:"
	@echo "  all          - Build all executables"
	@echo "  server_improved - Build improved server"
	@echo "  frontend     - Build front-end"
	@echo "  client       - Build client"
	@echo "  test         - Run basic tests"
	@echo "  setup_test   - Create test storage directories"
	@echo "  clean        - Clean build artifacts"
	@echo "  help         - Show this help message"

