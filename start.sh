#!/bin/bash
#
# SCRIPT: start_rpi_processing.sh
# DESCRIPTION: Starts two GStreamer pipelines for video splitting, the YOLO
# execution process, and the Python streaming server concurrently.
# Includes cleanup on exit (Ctrl+C).

# --- Configuration ---
PIDFILE="./yolo_pids.txt"

YOLO_EXECUTABLE="./YoloV8TR"
GST_COMMAND="gst-launch-1.0 -v"
STREAMER_EXECUTABLE="./rstp_server.py" # Corrected Streaming Script Name
LOG_DIR="./logs"
VIRTUAL_DEVICES=4 # Number of virtual cameras needed (10, 11, 12, 13)
# ---------------------

# MANDATORY STEP 1: Create the log directory if it does not exist
mkdir -p $LOG_DIR
if [ $? -ne 0 ]; then
    echo "ERROR: Could not create log directory $LOG_DIR. Exiting."
    exit 1
fi

# MANDATORY STEP 2: Load the v4l2loopback module to create virtual video devices.
echo "-> Setting up virtual video devices (/dev/video10, /dev/video11, etc.)..."
# We explicitly set the device numbers to ensure they match the sink targets (10, 11, 12, 13)
if ! sudo modprobe v4l2loopback devices=$VIRTUAL_DEVICES video_nr=10,11,12,13 card_label="YOLO_Source_1","Stream_Source_1","YOLO_Source_2","Stream_Source_2" 2>/dev/null; then
    echo "WARNING: Failed to load v4l2loopback module. You may need to run this script with 'sudo' or install the module."
    echo "Attempting to continue, but GStreamer pipelines might fail."
fi

# Give the system a moment to create the device files
sleep 0.5

echo "Starting RPi Camera Stream & YOLO Processing Script..."
echo "All processes will run in the background. Press Ctrl+C to stop."

# Array to hold all background Process IDs (PIDS)
PIDS=()

# Function to stop all background processes
cleanup() {
    echo -e "\n\nStopping all background processes..."
    # Kill all processes started by this script
    for pid in "${PIDS[@]}"; do
        if kill "$pid" 2>/dev/null; then
            echo "Killed process $pid."
        fi
    done
    
    # Optional: Unload the module on exit
    # echo "Unloading v4l2loopback module..."
    # sudo rmmod v4l2loopback 2>/dev/null

    echo "Cleanup complete."
    exit 0
}

# Trap Ctrl+C (SIGINT) and run the cleanup function
trap cleanup SIGINT

# ----------------------------------------------------
# 1. Start GStreamer Pipeline 1 (/dev/video0 -> 10 & 11)
# ----------------------------------------------------
echo "-> Starting GStreamer Pipeline 1 (video0 -> 10/11)..."
# /dev/video10: YOLO input (source 1)
# /dev/video11: Stream input (source 1)
$GST_COMMAND \
    v4l2src device=/dev/video0 ! \
    video/x-raw, width=640, height=480, framerate=30/1 ! \
    videoconvert ! tee name=t1 \
    t1. ! queue max-size-buffers=1 leaky=downstream ! v4l2sink device=/dev/video10 \
    t1. ! queue max-size-buffers=1 leaky=downstream ! v4l2sink device=/dev/video11 \
    > $LOG_DIR/gst_cam3.log 2>&1 &

# Store PID
PIDS+=($!)
echo "   PID: ${PIDS[-1]}"

# ----------------------------------------------------
# 2. Start GStreamer Pipeline 2 (/dev/video2 -> 12 & 13)
# ----------------------------------------------------
echo "-> Starting GStreamer Pipeline 2 (video2 -> 12/13)..."
# /dev/video12: YOLO input (source 2)
# /dev/video13: Stream input (source 2)
$GST_COMMAND \
    v4l2src device=/dev/video2 ! \
    video/x-raw, width=640, height=480, framerate=30/1 ! \
    videoconvert ! tee name=t2 \
    t2. ! queue max-size-buffers=1 leaky=downstream ! v4l2sink device=/dev/video12 \
    t2. ! queue max-size-buffers=1 leaky=downstream ! v4l2sink device=/dev/video13 \
    > $LOG_DIR/gst_cam4.log 2>&1 &

# Store PID
PIDS+=($!)
echo "   PID: ${PIDS[-1]}"

# ----------------------------------------------------
# 3. Start YOLO Execution
# ----------------------------------------------------
echo "-> Starting YOLO Detection ($YOLO_EXECUTABLE execute)..."
$YOLO_EXECUTABLE execute > $LOG_DIR/yolo.log 2>&1 &

# Store PID
PIDS+=($!)
echo "   PID: ${PIDS[-1]}"

# ----------------------------------------------------
# 4. Start Python Streaming Script
# ----------------------------------------------------
echo "-> Starting Python Streaming Server ($STREAMER_EXECUTABLE)..."
# *** EDITED LINE: Use python3 to execute the script ***
python3 $STREAMER_EXECUTABLE > $LOG_DIR/stream.log 2>&1 &

# Store PID
PIDS+=($!)
echo "   PID: ${PIDS[-1]}"

echo "----------------------------------------------------"
echo "All 4 processes running. Check log files in $LOG_DIR for output."

# Wait for all background processes to finish (which won't happen unless they crash,
# but this keeps the script alive until Ctrl+C is pressed)
wait