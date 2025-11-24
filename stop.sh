#!/bin/bash
#
# SCRIPT: stop.sh
# DESCRIPTION: Reads stored PIDs and kills all background processes
# started by start.sh (GStreamer, YOLO, RTSP Server).

PID_FILE="./logs/pids.txt"

echo "Stopping YOLO/GStreamer processes..."

if [ -f "$PID_FILE" ]; then
    # Read PIDs line by line
    PIDS=($(cat "$PID_FILE"))
    
    echo "Found PIDs to kill: ${PIDS[*]}"
    
    # Kill all processes started by the script
    for pid in "${PIDS[@]}"; do
        if kill "$pid" 2>/dev/null; then
            echo "Killed process $pid."
        else
            echo "Process $pid was not running or could not be killed."
        fi
    done
    
    # Remove the PID file after cleanup
    rm -f "$PID_FILE"
else
    echo "PID file not found ($PID_FILE). No processes to kill from this script."
fi

# Optional: Unload v4l2loopback module
if lsmod | grep -q v4l2loopback; then
    echo "Unloading v4l2loopback module..."
    sudo rmmod v4l2loopback 2>/dev/null
fi

echo "Cleanup complete."