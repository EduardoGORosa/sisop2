#!/bin/bash

echo "=== Dropbox System Startup Script ==="

# Function to cleanup on exit
cleanup() {
    echo
    echo "🛑 Shutting down all services..."
    if [ ! -z "$SERVER_30_PID" ]; then kill $SERVER_30_PID 2>/dev/null; fi
    if [ ! -z "$SERVER_20_PID" ]; then kill $SERVER_20_PID 2>/dev/null; fi
    if [ ! -z "$SERVER_10_PID" ]; then kill $SERVER_10_PID 2>/dev/null; fi
    if [ ! -z "$FRONTEND_PID" ]; then kill $FRONTEND_PID 2>/dev/null; fi
    echo "✅ All services stopped"
    exit 0
}

# Set up signal handlers
trap cleanup SIGINT SIGTERM

# Check if executables exist
if [ ! -f "./server_improved" ] || [ ! -f "./frontend" ] || [ ! -f "./client" ]; then
    echo "❌ Executables not found. Running make..."
    make clean && make all
    if [ $? -ne 0 ]; then
        echo "❌ Compilation failed!"
        exit 1
    fi
fi

echo "✅ All executables found"

# Kill any existing processes on our ports
echo "🧹 Cleaning up any existing processes..."
pkill -f "server_improved" 2>/dev/null
pkill -f "frontend" 2>/dev/null
sleep 2

# Create storage directories automatically
echo "📁 Setting up storage directories..."
for server_id in 10 20 30; do
    mkdir -p "storage_${server_id}"
    echo "  Created storage_${server_id}/"
done

echo "🚀 Starting servers..."

# Start servers with better error handling
echo "  Starting Server 30 (Initial Leader)..."
./server_improved 30 127.0.0.1 8003 ./storage_30 > server_30.log 2>&1 &
SERVER_30_PID=$!
sleep 3

if ! ps -p $SERVER_30_PID > /dev/null; then
    echo "❌ Server 30 failed to start. Check server_30.log"
    cat server_30.log
    exit 1
fi

echo "  Starting Server 20 (Backup)..."
./server_improved 20 127.0.0.1 8002 ./storage_20 > server_20.log 2>&1 &
SERVER_20_PID=$!
sleep 3

if ! ps -p $SERVER_20_PID > /dev/null; then
    echo "❌ Server 20 failed to start. Check server_20.log"
    cat server_20.log
    cleanup
fi

echo "  Starting Server 10 (Backup)..."
./server_improved 10 127.0.0.1 8001 ./storage_10 > server_10.log 2>&1 &
SERVER_10_PID=$!
sleep 3

if ! ps -p $SERVER_10_PID > /dev/null; then
    echo "❌ Server 10 failed to start. Check server_10.log"
    cat server_10.log
    cleanup
fi

echo "🌐 Starting Front-End..."
./frontend 127.0.0.1 9000 > frontend.log 2>&1 &
FRONTEND_PID=$!
sleep 3

if ! ps -p $FRONTEND_PID > /dev/null; then
    echo "❌ Front-End failed to start. Check frontend.log"
    cat frontend.log
    cleanup
fi

echo
echo "✅ System Status:"
echo "  🟢 Server 30 (Leader): PID $SERVER_30_PID - Port 8003"
echo "  🟡 Server 20 (Backup): PID $SERVER_20_PID - Port 8002" 
echo "  🟡 Server 10 (Backup): PID $SERVER_10_PID - Port 8001"
echo "  🔵 Front-End: PID $FRONTEND_PID - Port 9000"
echo

# Wait a bit for system to stabilize
echo "⏳ Waiting for system to stabilize..."
sleep 5

# Check if all services are still running
echo "🔍 Health Check:"
if ps -p $SERVER_30_PID > /dev/null; then echo "  ✅ Server 30 healthy"; else echo "  ❌ Server 30 died"; fi
if ps -p $SERVER_20_PID > /dev/null; then echo "  ✅ Server 20 healthy"; else echo "  ❌ Server 20 died"; fi
if ps -p $SERVER_10_PID > /dev/null; then echo "  ✅ Server 10 healthy"; else echo "  ❌ Server 10 died"; fi
if ps -p $FRONTEND_PID > /dev/null; then echo "  ✅ Front-End healthy"; else echo "  ❌ Front-End died"; fi

echo
echo "🎯 System Ready! You can now:"
echo "  📱 Connect client: ./client 127.0.0.1 9000"
echo "  📊 Monitor logs: tail -f server_30.log"
echo "  🧪 Test election: kill $SERVER_30_PID"
echo
echo "📋 Log files created:"
echo "  server_30.log, server_20.log, server_10.log, frontend.log"
echo
echo "Press Ctrl+C to stop all services"

# Keep script running and monitor processes
while true; do
    sleep 10
    
    # Check if any process died
    if ! ps -p $SERVER_30_PID > /dev/null 2>&1; then
        echo "⚠️  Server 30 died unexpectedly!"
    fi
    if ! ps -p $SERVER_20_PID > /dev/null 2>&1; then
        echo "⚠️  Server 20 died unexpectedly!"
    fi
    if ! ps -p $SERVER_10_PID > /dev/null 2>&1; then
        echo "⚠️  Server 10 died unexpectedly!"
    fi
    if ! ps -p $FRONTEND_PID > /dev/null 2>&1; then
        echo "⚠️  Front-End died unexpectedly!"
    fi
done

