#!/usr/bin/env bash
set -euo pipefail
trap 'echo ""; echo "[CTRL+C] Stopping all background tasks..."; kill 0' INT


# ============================
# 🔧 CONFIG
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

declare -A CAMERA_NAMES=(
  ["pi"]="/dev/video0 CAM0 /dev/video2 CAM1"
  ["rpi1"]="/dev/video0 CAM3 /dev/video2 CAM4"
  ["rpi2"]="/dev/video0 CAM5 /dev/video2 CAM6"
)

declare -A JSON_FILES=(
  ["pi"]="cam1.json cam2.json"
  ["rpi1"]="cam3.json cam4.json"
  ["rpi2"]="cam5.json cam6.json"
)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCAL_LOG_DIR="$SCRIPT_DIR/logs"
PID_DIR="$SCRIPT_DIR/pids"

mkdir -p "$LOCAL_LOG_DIR" "$PID_DIR"

SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=5 -o LogLevel=ERROR -o ServerAliveInterval=1 -o ServerAliveCountMax=3"

echo "========== CLUSTER STREAM START =========="

# ============================
# 🚀 START PARALLEL
# ============================

for NODE in "${!HOSTS[@]}"; do
{
    HOST="${HOSTS[$NODE]}"
    USERNAME="${USER[$NODE]}"
    PASSWORD="${PASS[$NODE]}"
    BINARY="${REMOTE_BIN[$NODE]}"
    CAM_ARGS="${CAMERA_NAMES[$NODE]}"
    FILES="${JSON_FILES[$NODE]}"

    echo ""
    echo "[INFO] ($NODE) Connecting to $HOST..."

    REMOTE_BASE=$(sshpass -p "$PASSWORD" ssh $SSH_OPTS $USERNAME@$HOST "ls -d /home/$USERNAME/YOLO* | head -n1" || true)

    if [[ -z "$REMOTE_BASE" ]]; then
        echo "[ERR] ($NODE) Could not find remote directory"
        exit 1
    fi

    echo "[INFO] ($NODE) Found base: $REMOTE_BASE"
    echo "[INFO] ($NODE) Launching $BINARY..."

    sshpass -p "$PASSWORD" ssh -n $SSH_OPTS $USERNAME@$HOST \
      "cd '$REMOTE_BASE' && mkdir -p detections && nohup ./$BINARY $CAM_ARGS > detections/${BINARY}.stdout.log 2>&1 &" || true

    sleep 2

    REMOTE_FILES=""
    for f in $FILES; do
        REMOTE_FILES="$REMOTE_FILES $REMOTE_BASE/detections/$f"
    done

    # HIGH PERFORMANCE UNBUFFERED PIPELINE
    REMOTE_TAIL_CMD="stdbuf -o0 -e0 tail -n0 -F $REMOTE_FILES"

    LOCAL_FILE="$LOCAL_LOG_DIR/${NODE}_${HOST}.json.log"

    echo "[INFO] ($NODE) Streaming → $LOCAL_FILE"

    # Run streaming line-by-line, minimal latency
    stdbuf -o0 -e0 sshpass -p "$PASSWORD" ssh -T $SSH_OPTS $USERNAME@$HOST "$REMOTE_TAIL_CMD" \
        | stdbuf -o0 -e0 tee -a "$LOCAL_FILE" >/dev/null 2>&1 &

    echo $! > "$PID_DIR/${NODE}_${HOST}.pid"
    echo "[INFO] ($NODE) tail PID=$!"
} &
done

# ============================
# 🧩 COMBINED REAL-TIME LOG
# ============================

sleep 2

COMBINED="$LOCAL_LOG_DIR/cluster_combined.log"
echo ""
echo "[INFO] Starting combined aggregator → $COMBINED"

stdbuf -o0 -e0 tail -n0 -F $LOCAL_LOG_DIR/*.json.log \
    | stdbuf -o0 -e0 tee -a "$COMBINED" >/dev/null 2>&1 &

echo $! > "$PID_DIR/aggregator.pid"

echo ""
echo "[INFO] 🚀 Everything is LIVE with LOW LATENCY"
echo "[INFO] Logs:"
for NODE in "${!HOSTS[@]}"; do
  echo "   $LOCAL_LOG_DIR/${NODE}_${HOSTS[$NODE]}.json.log"
done
echo "[INFO] Combined log: $COMBINED"
echo "==========================================="
