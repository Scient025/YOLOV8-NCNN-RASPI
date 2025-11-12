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

PID_DIR="./pids"
LOG_DIR="./logs"

SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=5"

echo "========== CLUSTER SHUTDOWN =========="

# ============================
# 🧹 Stop remote YOLO binaries
# ============================

for NODE in "${!HOSTS[@]}"; do
{
    HOST="${HOSTS[$NODE]}"
    USERNAME="${USER[$NODE]}"
    PASSWORD="${PASS[$NODE]}"
    BINARY="${REMOTE_BIN[$NODE]}"

    echo "[INFO] (${NODE}) Stopping ${BINARY} on ${HOST}..."

    sshpass -p "${PASSWORD}" ssh -n ${SSH_OPTS} ${USERNAME}@${HOST} \
      "pkill -f ${BINARY} || true" \
      >/dev/null 2>&1 || echo "[WARN] (${NODE}) Failed to connect to ${HOST}"

    echo "[INFO] (${NODE}) ${BINARY} stopped (if running)"
} &
done
wait

# ============================
# 🧩 Stop local tail + aggregator
# ============================

echo ""
echo "[INFO] Stopping local tail and aggregator processes..."

if [[ -d "${PID_DIR}" ]]; then
    for PID_FILE in "${PID_DIR}"/*.pid; do
        if [[ -f "${PID_FILE}" ]]; then
            PID=$(cat "${PID_FILE}")
            if ps -p "${PID}" > /dev/null 2>&1; then
                kill "${PID}" >/dev/null 2>&1 || true
                echo "[INFO] Killed process PID=${PID}"
            fi
            rm -f "${PID_FILE}"
        fi
    done
else
    echo "[WARN] No PID directory found."
fi

# ============================
# ✅ Verify everything stopped
# ============================

echo ""
for NODE in "${!HOSTS[@]}"; do
{
    HOST="${HOSTS[$NODE]}"
    USERNAME="${USER[$NODE]}"
    PASSWORD="${PASS[$NODE]}"
    BINARY="${REMOTE_BIN[$NODE]}"
    
    STATUS=$(sshpass -p "${PASSWORD}" ssh -n ${SSH_OPTS} ${USERNAME}@${HOST} "pgrep -f ${BINARY} || true")
    if [[ -z "${STATUS}" ]]; then
        echo "[INFO] (${NODE}) ${BINARY} is not running ✅"
    else
        echo "[WARN] (${NODE}) ${BINARY} still running (PID ${STATUS})"
    fi
} &
done
wait

echo ""
echo "[INFO] 🧹 Cluster stopped cleanly."
echo "====================================="
    