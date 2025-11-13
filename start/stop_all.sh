#!/usr/bin/env bash
set -euo pipefail

pkill -f "start_all.sh" 2>/dev/null || true

echo "========== CLUSTER SHUTDOWN =========="

# ============================
# CONFIG (same as start script)
# ============================

declare -A HOSTS=(
  ["pi"]="192.168.1.163"
  ["rpi1"]="192.168.1.181"
  ["rpi2"]="192.168.1.158"
)

declare -A USER=(
  ["pi"]="pi"
  ["rpi1"]="olee"
  ["rpi2"]="olee"
)

declare -A PASS=(
  ["pi"]="pi"
  ["rpi1"]="Citoto@321"
  ["rpi2"]="Citoto@321"
)

declare -A REMOTE_BIN=(
  ["pi"]="YoloV8DB0"
  ["rpi1"]="YoloV8DB1"
  ["rpi2"]="YoloV8DB2"
)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PID_DIR="$SCRIPT_DIR/pids"

SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=5 -o LogLevel=ERROR"

# ============================
# 🛑 STOP REMOTE NODES
# ============================

for NODE in "${!HOSTS[@]}"; do
{
    HOST="${HOSTS[$NODE]}"
    USERNAME="${USER[$NODE]}"
    PASSWORD="${PASS[$NODE]}"
    BINARY="${REMOTE_BIN[$NODE]}"

    echo ""
    echo "[INFO] ($NODE) Stopping $BINARY on $HOST..."

    # Kill YOLO binary if running
    sshpass -p "$PASSWORD" ssh $SSH_OPTS $USERNAME@$HOST \
      "pkill -9 -f $BINARY || true" \
      >/dev/null 2>&1 || echo "[WARN] ($NODE) Could not reach $HOST"

    sleep 0.5

    # Kill tail processes on remote Pi
    sshpass -p "$PASSWORD" ssh $SSH_OPTS $USERNAME@$HOST \
      "pkill -9 -f 'tail -n0 -F' || true" \
      >/dev/null 2>&1 || true

    # Check if still running
    STILL_RUNNING=$(sshpass -p "$PASSWORD" ssh $SSH_OPTS $USERNAME@$HOST "pgrep -f $BINARY || true")

    if [[ -n "$STILL_RUNNING" ]]; then
        echo "[WARN] ($NODE) $BINARY still running (PID $STILL_RUNNING)"
    else
        echo "[INFO] ($NODE) $BINARY stopped"
    fi
} &
done

wait

# ============================
# 🛑 STOP LOCAL PROCESSES
# ============================

echo ""
echo "[INFO] Stopping local tail + aggregator..."

# Kill local aggregator
if [[ -f "$PID_DIR/aggregator.pid" ]]; then
    AGG_PID=$(cat "$PID_DIR/aggregator.pid")
    kill -9 "$AGG_PID" >/dev/null 2>&1 || true
    rm -f "$PID_DIR/aggregator.pid"
    echo "[INFO] Killed aggregator PID=$AGG_PID"
fi

# Kill local SSH tail streams
for PIDFILE in "$PID_DIR"/*.pid; do
    [[ "$PIDFILE" == "$PID_DIR/aggregator.pid" ]] && continue
    if [[ -f "$PIDFILE" ]]; then
        PID=$(cat "$PIDFILE")
        kill -9 "$PID" >/dev/null 2>&1 || true
        rm -f "$PIDFILE"
        echo "[INFO] Killed process PID=$PID"
    fi
done

echo ""
echo "[INFO] 🧹 Cluster stopped cleanly."
echo "==========================================="
