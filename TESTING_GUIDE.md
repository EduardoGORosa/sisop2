# Complete Testing Guide

## Prerequisites

1. **Extract the files**:
```bash
tar -xzf dropbox_improved_implementation.tar.gz
cd <extracted_directory>
```

2. **Compile everything**:
```bash
make clean
make all
```

You should see three executables: `server_improved`, `frontend`, and `client`.

## Test 1: Basic Compilation and Help Messages

```bash
# Test that executables work
./server_improved
./frontend  
./client
```

Expected output: Usage messages for each program.

## Test 2: Normal Operation Test

### Step 1: Start the Servers (3 separate terminals)

**Terminal 1** (Server ID 30 - will be initial leader):
```bash
mkdir -p storage_30
./server_improved 30 127.0.0.1 8003 ./storage_30
```

**Terminal 2** (Server ID 20):
```bash
mkdir -p storage_20
./server_improved 20 127.0.0.1 8002 ./storage_20
```

**Terminal 3** (Server ID 10):
```bash
mkdir -p storage_10
./server_improved 10 127.0.0.1 8001 ./storage_10
```

**Expected logs**: You should see:
- Server 30 becomes leader (highest ID)
- Servers 20 and 10 start monitoring leader
- Heartbeat messages every second

### Step 2: Start the Front-End

**Terminal 4**:
```bash
./frontend 127.0.0.1 9000
```

**Expected logs**: 
- FE starts listening on port 9000
- FE discovers leader (server 30)

### Step 3: Connect Client Through Front-End

**Terminal 5**:
```bash
./client 127.0.0.1 9000
```

**Expected behavior**:
- Client connects to FE (not directly to server)
- FE forwards connection to current leader
- Client can upload/download files normally

### Step 4: Test File Operations

In the client terminal, try:
```
upload <some_file>
list_server
download <file>
```

**What to verify**:
- Files appear in `storage_30/<username>/` (primary)
- Files also appear in `storage_20/<username>/` and `storage_10/<username>/` (backups)
- Server logs show backup operations and acknowledgments

## Test 3: Passive Replication Verification

### Monitor Server Logs During Upload

When you upload a file, watch the server logs carefully:

**Expected sequence**:
1. **Primary (Server 30)** receives upload from client
2. **Primary** saves file locally
3. **Primary** sends `BACKUP_OPERATION` to servers 20 and 10
4. **Backups** process the operation and send `BACKUP_ACK`
5. **Primary** waits for all ACKs before confirming to client
6. **Client** receives confirmation only after all backups are updated

**Key log messages to look for**:
```
[SERVER] Broadcasting with ACK - Operation ID: 30_<timestamp>_<counter>
[SERVER] Sending backup operation to server 20
[SERVER] Sending backup operation to server 10
[SERVER] Received ACK for operation <id> from server 20
[SERVER] Received ACK for operation <id> from server 10
[SERVER] All acknowledgments received for operation <id>
```

## Test 4: Leader Election Test

### Step 1: Kill Current Leader

While the system is running, kill server 30 (current leader):
```bash
# In terminal 1, press Ctrl+C to kill server 30
```

### Step 2: Observe Election Process

**Expected behavior**:
- Servers 20 and 10 detect leader failure (no heartbeats)
- Server 20 (higher ID) starts election
- Server 20 becomes new leader
- FE detects leader change and updates connection

**Key log messages**:
```
[BULLY] Leader 30 failure detected, starting election
[BULLY] Server 20 starting election
[BULLY] Server 20 elected as new leader
[FE] Leader changed from 30 to 20
```

### Step 3: Test Client Operations Continue

- Client should still work normally
- New uploads go to server 20 (new leader)
- Files are replicated to server 10

## Test 5: Front-End Transparency Test

### Test 1: Direct vs FE Connection

**Try connecting directly to server** (should fail if not leader):
```bash
./client 127.0.0.1 8001  # Server 10 (backup)
```
**Expected**: Connection refused with message about being backup.

**Connect through FE** (should work):
```bash
./client 127.0.0.1 9000  # Front-End
```
**Expected**: Connection works regardless of which server is leader.

### Test 2: Leader Change Transparency

1. Start client through FE
2. Kill current leader
3. Wait for election to complete
4. Try file operations in client

**Expected**: Client operations continue working without reconnection.

## Test 6: Multiple Clients Test

Start multiple clients simultaneously:

**Terminal 6**:
```bash
./client 127.0.0.1 9000
# Register as user "alice"
```

**Terminal 7**:
```bash
./client 127.0.0.1 9000  
# Register as user "bob"
```

**Test**:
- Both clients can upload files
- Files are properly isolated by user
- All operations are replicated to backups

## Test 7: Backup Failure Test

### Step 1: Kill a Backup Server

Kill one backup (e.g., server 10):
```bash
# Kill server 10
```

### Step 2: Test Operations Continue

- Upload files through client
- Check server logs for timeout messages
- System should continue working with remaining servers

**Expected logs**:
```
[SERVER] Timeout or incomplete acknowledgments for operation <id>
[SERVER] Missing ACKs from servers: 10
```

## Test 8: Network Partition Simulation

### Simulate Network Issues

You can test network resilience by:

1. **Blocking ports** (requires root):
```bash
sudo iptables -A INPUT -p tcp --dport 8001 -j DROP  # Block server 10
```

2. **Restore connectivity**:
```bash
sudo iptables -D INPUT -p tcp --dport 8001 -j DROP
```

## Verification Checklist

After running tests, verify:

- ✅ **Passive Replication**: Files exist in all server storage directories
- ✅ **Leader Election**: System recovers from leader failures
- ✅ **FE Transparency**: Clients work without knowing server topology
- ✅ **Acknowledgments**: Primary waits for backup confirmations
- ✅ **Consistency**: All replicas have identical files

## Troubleshooting

### Common Issues:

1. **"Address already in use"**: Kill existing processes or wait a moment
2. **"Connection refused"**: Ensure servers are started before FE and clients
3. **Permission denied**: Check directory permissions for storage folders

### Debug Commands:

```bash
# Check running processes
ps aux | grep -E "(server_improved|frontend|client)"

# Check port usage
netstat -tlnp | grep -E "(800[1-3]|9000)"

# Monitor logs in real-time
tail -f <terminal_output>
```

## Expected Performance

- **Startup time**: ~1-2 seconds per component
- **Election time**: ~5-15 seconds (depending on timeouts)
- **File operations**: Should complete within seconds
- **Replication delay**: Minimal (< 1 second for small files)

This testing guide covers all the key functionality and edge cases to verify that your implementation meets the assignment requirements!

