#!/usr/bin/env bash
set -euo pipefail

# ============================
# 🔧 CLUSTER CONFIGURATION
# ============================

declare -A HOSTS=(
  ["pi"]="192.168.1.163"
  ["rpi2"]="192.168.1.158"
)

declare -A USER=(
  ["pi"]="pi"
  ["rpi2"]="olee"
)

declare -A PASS=(
  ["pi"]="pi"
  ["rpi2"]="Citoto@321"
)

declare -A REMOTE_BIN=(
  ["pi"]="YoloV8DB0"
  ["rpi2"]="YoloV8DB2"
)

declare -A CAMERA_NAMES=(
  ["pi"]="/dev/video0 CAM0 /dev/video2 CAM1"
  ["rpi2"]="/dev/video0 CAM5 /dev/video2 CAM6"
)

declare -A JSON_FILES=(
  ["pi"]="cam1.json cam2.json"
  ["rpi2"]="cam5.json cam6.json"
)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="${SCRIPT_DIR}"
LOCAL_LOG_DIR="${BASE_DIR}/logs"
PID_DIR="${BASE_DIR}/pids"

mkdir -p "${LOCAL_LOG_DIR}" "${PID_DIR}"

SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=5"

echo "========== CLUSTER STREAM START =========="

# ============================
# 🚀 LAUNCH ALL IN PARALLEL
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
    echo "[INFO] (${NODE}) Connecting to ${HOST}..."

    # Auto-detect YOLO directory remotely
    REMOTE_BASE=$(sshpass -p "${PASSWORD}" ssh ${SSH_OPTS} ${USERNAME}@${HOST} "ls -d /home/${USERNAME}/YOLO* | head -n1" 2>/dev/null || true)
    if [[ -z "${REMOTE_BASE}" ]]; then
        echo "[ERR] (${NODE}) Could not locate YOLO directory on ${HOST}"
        exit 1
    fi

    echo "[INFO] (${NODE}) Found base: ${REMOTE_BASE}"
    echo "[INFO] (${NODE}) Launching ${BINARY} on ${HOST} with cams ${CAM_ARGS}"

    # Start the YOLO process on the Pi
    sshpass -p "${PASSWORD}" ssh -n ${SSH_OPTS} ${USERNAME}@${HOST} \
      "cd '${REMOTE_BASE}' && mkdir -p detections && nohup ./${BINARY} ${CAM_ARGS} > detections/${BINARY}.stdout.log 2>&1 &" \
      >/dev/null 2>&1 || echo "[WARN] (${NODE}) Failed to start ${BINARY} on ${HOST}"

    sleep 3  # allow YOLO to start writing JSONs

    # build explicit file paths for tail
    REMOTE_FILES=()
    for f in ${FILES}; do
        REMOTE_FILES+=( "${REMOTE_BASE}/detections/${f}" )
    done
    REMOTE_TAIL_CMD="stdbuf -oL -eL tail -n0 -F ${REMOTE_FILES[*]}"

    LOCAL_FILE="${LOCAL_LOG_DIR}/${NODE}_${HOST}.json.log"
    PID_FILE="${PID_DIR}/${NODE}_${HOST}.pid"
    echo "[INFO] (${NODE}) Streaming live to ${LOCAL_FILE}"

    # use stdbuf to make SSH + tail line-buffered (no delay)
    setsid bash -c "stdbuf -oL sshpass -p \"${PASSWORD}\" ssh ${SSH_OPTS} ${USERNAME}@${HOST} \"${REMOTE_TAIL_CMD}\" | stdbuf -oL tee -a '${LOCAL_FILE}' >/dev/null" >/dev/null 2>&1 &
    tail_pid=$!
    echo "${tail_pid}" > "${PID_FILE}"
    echo "[INFO] (${NODE}) tail PID=${tail_pid}"
} &
done

# ============================
# 🧩 REAL-TIME COMBINED LOG
# ============================

sleep 5
COMBINED_FILE="${LOCAL_LOG_DIR}/cluster_combined.log"
echo ""
echo "[INFO] Starting combined aggregator -> ${COMBINED_FILE}"

# line-buffered aggregator: real-time append
setsid bash -c "stdbuf -oL tail -n0 -F ${LOCAL_LOG_DIR}/*.json.log | stdbuf -oL tee -a '${COMBINED_FILE}' >/dev/null" >/dev/null 2>&1 &
echo $! > "${PID_DIR}/aggregator.pid"

echo ""
echo "[INFO] ✅ All nodes streaming in real-time!"
echo "[INFO] Individual logs:"
for NODE in "${!HOSTS[@]}"; do
    echo "   ${LOCAL_LOG_DIR}/${NODE}_${HOSTS[$NODE]}.json.log"
done
echo "[INFO] Combined log: ${COMBINED_FILE}"
echo "[INFO] View live feed instantly with:"
echo "       tail -f ${COMBINED_FILE}"
echo "==========================================="
