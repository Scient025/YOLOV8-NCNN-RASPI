#!/bin/bash
#
# SCRIPT: start_rpi_processing.sh
# DESCRIPTION: Starts two GStreamer pipelines for video splitting, the YOLO
# execution process, and the Python streaming server concurrently.
# Includes cleanup on exit (Ctrl+C) and now SAVES PIDs for remote stopping.
# FIX: Added stdbuf and PYTHONUNBUFFERED to force log files to write live.

# --- Configuration ---
YOLO_EXECUTABLE="./YoloV8TR"
GST_COMMAND="gst-launch-1.0 -v"
STREAMER_EXECUTABLE="./rstp_server.py"
LOG_DIR="./logs"
PID_FILE="$LOG_DIR/pids.txt" # New file path for PIDs
VIRTUAL_DEVICES=4 
# ---------------------

# MANDATORY STEP 1: Create the log directory and clear the old PID file
mkdir -p $LOG_DIR
rm -f "$PID_FILE" # Clear old PID list

if [ $? -ne 0 ]; then
    echo "ERROR: Could not setup log directory. Exiting."
    exit 1
fi

# MANDATORY STEP 2: Load the v4l2loopback module
echo "-> Setting up virtual video devices (/dev/video10, /dev/video11, etc.)..."
if ! sudo modprobe v4l2loopback devices=$VIRTUAL_DEVICES video_nr=10,11,12,13 card_label="YOLO_Source_1","Stream_Source_1","YOLO_Source_2","Stream_Source_2" 2>/dev/null; then
    echo "WARNING: Failed to load v4l2loopback module. You may need to run this script with 'sudo' or install the module."
    echo "Attempting to continue, but GStreamer pipelines might fail."
fi
# Fixed syntax and separated from the if block
sleep 0.5

echo "Starting RPi Camera Stream & YOLO Processing Script..."

# Function to start a process and record its PID
start_process_and_save_pid() {
    local command="$1"
    local logfile="$2"
    local message="$3"
    
    echo "-> Starting $message"
    
    # Execute command in background and redirect output to log
    # Using eval here is necessary for command strings with pipes/redirections
    eval "$command > $logfile 2>&1 &"
    
    local pid=$!
    echo "   PID: $pid"
    
    # Save PID to the file
    echo "$pid" >> "$PID_FILE"
}

# --- PROCESS START ---

# 1. Start GStreamer Pipeline 1 (No buffering needed for GST)
start_process_and_save_pid \
    "$GST_COMMAND v4l2src device=/dev/video0 ! video/x-raw, width=640, height=480, framerate=30/1 ! videoconvert ! tee name=t1 t1. ! queue max-size-buffers=1 leaky=downstream ! v4l2sink device=/dev/video10 t1. ! queue max-size-buffers=1 leaky=downstream ! v4l2sink device=/dev/video11" \
    "$LOG_DIR/gst_cam1.log" \
    "GStreamer Pipeline 1 (video0 -> 10/11)"

# 2. Start GStreamer Pipeline 2 (No buffering needed for GST)
start_process_and_save_pid \
    "$GST_COMMAND v4l2src device=/dev/video2 ! video/x-raw, width=640, height=480, framerate=30/1 ! videoconvert ! tee name=t2 t2. ! queue max-size-buffers=1 leaky=downstream ! v4l2sink device=/dev/video12 t2. ! queue max-size-buffers=1 leaky=downstream ! v4l2sink device=/dev/video13" \
    "$LOG_DIR/gst_cam2.log" \
    "GStreamer Pipeline 2 (video2 -> 12/13)"

# 3. Start YOLO Execution - FORCING LIVE LINE BUFFERING
start_process_and_save_pid \
    "stdbuf -oL $YOLO_EXECUTABLE execute" \
    "$LOG_DIR/yolo.log" \
    "YOLO Detection (LIVE LOGGING ENABLED)"

# 4. Start Python Streaming Script - FORCING UNBUFFERED OUTPUT
start_process_and_save_pid \
    "PYTHONUNBUFFERED=1 python3 $STREAMER_EXECUTABLE" \
    "$LOG_DIR/stream.log" \
    "Python Streaming Server (LIVE LOGGING ENABLED)"


echo "----------------------------------------------------"
echo "All processes started. PIDs saved to $PID_FILE."
echo "Use './stop.sh' locally or 'stop_remote.sh' on the PC to shut down."

# The main script must wait or the 'nohup' will immediately finish.
# We don't use 'wait' here because we want the remote session to exit immediately.
# The 'nohup' wrapper in the remote start script handles keeping them alive.