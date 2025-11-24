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


echo "Starting YOLO jobs on all Raspberry Pis..."

# Start ./start.sh on each Pi
sshpass -p "$PI1_PASS" ssh -o StrictHostKeyChecking=no \
    $PI1_USER@$PI1_IP "cd YOLOV8-NCNN-RASPI && nohup ./start.sh > out.log 2>&1 &"

sshpass -p "$PI2_PASS" ssh -o StrictHostKeyChecking=no \
    $PI2_USER@$PI2_IP "cd YOLOV8-NCNN-RASPI && nohup ./start.sh > out.log 2>&1 &"

sshpass -p "$PI3_PASS" ssh -o StrictHostKeyChecking=no \
    $PI3_USER@$PI3_IP "cd YOLOV8-NCNN-RASPI && nohup ./start.sh > out.log 2>&1 &"

echo "All Pis started successfully!"
