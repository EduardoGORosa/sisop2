#!/usr/bin/env bash
set -euo pipefail

ROOT=$(pwd)
LOGDIR="$ROOT/test"
SERVER_LOG="$ROOT/server.log"

# Expected file counts per user
declare -A EXPECTED_FILES=( [user1]=10 [user2]=10 )
TIMEOUT=10  # seconds
SLEEP_INTERVAL=1   # integer seconds

# 1) Compile
echo "[1] Compiling..."
make clean && make

# 2) Start server
echo "[2] Starting server..."
$ROOT/bin/server > "$SERVER_LOG" 2>&1 &
SPID=$!
echo "[2] Server PID=$SPID"
sleep 1

# 3) Function to start client + tail
start_client() {
  user=$1; session=$2
  mkdir -p "$LOGDIR/$session/sync_dir"
  pushd "$LOGDIR/$session" > /dev/null
    tail -f /dev/null > /dev/null 2>&1 &
    TAIL_PID=$!
    $ROOT/bin/client "$user" 127.0.0.1 12345 > "$LOGDIR/${session}.log" 2>&1 &
    CLIENT_PID=$!
  popd > /dev/null
  echo "$TAIL_PID:$CLIENT_PID"
}

# 4) Launch clients
echo "[3] Launching clients..."
mapfile -t PIDS < <(
  start_client user1 u1a
  start_client user1 u1b
  start_client user2 u2a
  start_client user2 u2b
)
echo "[3] Clients (tail:client): ${PIDS[*]}"
sleep 1

# 5) Concurrent file creation
echo "[4] Creating files concurrently..."
for user in user1 user2; do
  for session in u1a u1b; do
    if [[ $user == user2 && $session =~ u1 ]]; then continue; fi
    if [[ $user == user1 && $session =~ u2 ]]; then continue; fi
    (
      for i in {1..5}; do
        F="$LOGDIR/$session/sync_dir/${user}_${session}_file_$i.txt"
        echo "Content $i" > "$F"
        echo "[LOG] Created $F"
        sleep 0.1
      done
    ) &
  done
done

# allow creation to complete
sleep 1

echo "[5] Waiting for synchronization..."
# 6) Poll until expected count or timeout
for user in "${!EXPECTED_FILES[@]}"; do
  expected=${EXPECTED_FILES[$user]}
  elapsed=0
  while (( $(ls storage/$user/sync_dir 2>/dev/null | wc -l) < expected )); do
    sleep $SLEEP_INTERVAL
    elapsed=$((elapsed + SLEEP_INTERVAL))
    if (( elapsed >= TIMEOUT )); then
      echo "[ERROR] Timeout waiting for $user: expected $expected, got $(ls storage/$user/sync_dir | wc -l)"
      break
    fi
  done
  count=$(ls storage/$user/sync_dir | wc -l || echo 0)
  echo "[RESULT] storage/$user/sync_dir => $count files (expected $expected)"
done

# 7) Cleanup
echo "[6] Cleaning up processes..."
kill "$SPID" 2>/dev/null || :
kill_pair() { IFS=":" read TP CP <<< "$1"; kill "$TP" "$CP" 2>/dev/null || :; }
for pair in "${PIDS[@]}"; do
  kill_pair "$pair"
done

echo "[7] Test complete. Logs available: server.log and ${LOGDIR}/*.log"
