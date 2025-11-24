#!/bin/bash

#############################
# EDIT THESE BEFORE RUNNING #
#############################

# Raspberry Pi 1
PI1_USER="pi"
PI1_IP="192.168.0.186"
PI1_PASS="pi"

# Raspberry Pi 2
PI2_USER="olee"
PI2_IP="192.168.0.16"
PI2_PASS="Citoto@321"

# Raspberry Pi 3
PI3_USER="olee"
PI3_IP="192.168.0.158"
PI3_PASS="Citoto@321"

# --- Function to execute the stop script on a remote host ---
function stop_pi() {
    local user=$1
    local ip=$2
    local pass=$3
    local pi_name=$4

    echo "Attempting to stop processes on $pi_name ($ip)..."
    
    # Execute the local 'stop.sh' script remotely using sshpass and nohup
    # We use sudo because 'stop.sh' needs it for rmmod v4l2loopback
    sshpass -p "$pass" ssh -o StrictHostKeyChecking=no $user@$ip \
        "cd YOLOV8-NCNN-RASPI && sudo ./stop.sh"
    
    if [ $? -eq 0 ]; then
        echo "SUCCESS: $pi_name stopped cleanly."
    else
        echo "WARNING: Failed to execute stop command on $pi_name. Check logs manually."
    fi
}

# --- Execution ---

stop_pi "$PI1_USER" "$PI1_IP" "$PI1_PASS" "Raspberry Pi 1"
stop_pi "$PI2_USER" "$PI2_IP" "$PI2_PASS" "Raspberry Pi 2"
stop_pi "$PI3_USER" "$PI3_IP" "$PI3_PASS" "Raspberry Pi 3"

echo "Remote shutdown sequence complete."