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
sshpass -p "$PI1_PASS" ssh -o StrictHostKeyChecking=no $PI1_USER@$PI1_IP "cd YOLOV8-NCNN-RASPI && nohup ./start.sh > out.log 2>&1 &"
sshpass -p "$PI2_PASS" ssh -o StrictHostKeyChecking=no $PI2_USER@$PI2_IP "cd YOLOV8-NCNN-RASPI && nohup ./start.sh > out.log 2>&1 &"
sshpass -p "$PI3_PASS" ssh -o StrictHostKeyChecking=no $PI3_USER@$PI3_IP "cd YOLOV8-NCNN-RASPI && nohup ./start.sh > out.log 2>&1 &"

echo "All Pis started!"
echo "Launching 6-camera GStreamer compositor..."


#######################################
# 6-CAMERA GSTREAMER COMPOSITOR PIPE  #
#######################################

gst-launch-1.0 -e compositor name=comp background=black \
    sink_0::xpos=0   sink_0::ypos=0   sink_0::width=640 sink_0::height=360 \
    sink_1::xpos=640 sink_1::ypos=0   sink_1::width=640 sink_1::height=360 \
    sink_2::xpos=1280 sink_2::ypos=0  sink_2::width=640 sink_2::height=360 \
    sink_3::xpos=0   sink_3::ypos=360 sink_3::width=640 sink_3::height=360 \
    sink_4::xpos=640 sink_4::ypos=360 sink_4::width=640 sink_4::height=360 \
    sink_5::xpos=1280 sink_5::ypos=360 sink_5::width=640 sink_5::height=360 \
    ! videoconvert ! autovideosink sync=false \
    rtspsrc location=rtsp://192.168.0.186:8556/cam1 latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! comp. \
    rtspsrc location=rtsp://192.168.0.186:8556/cam2 latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! comp. \
    rtspsrc location=rtsp://192.168.0.16:8555/cam3 latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! comp. \
    rtspsrc location=rtsp://192.168.0.16:8555/cam4 latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! comp. \
    rtspsrc location=rtsp://192.168.0.158:8554/cam5 latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! comp. \
    rtspsrc location=rtsp://192.168.0.158:8554/cam6 latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! comp.

