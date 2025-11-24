#!/bin/bash

# ===== CAMERA 1 =====
gst-launch-1.0 -v \
  v4l2src device=/dev/video0 ! \
  image/jpeg,width=640,height=480,framerate=30/1 ! \
  jpegdec ! videoconvert ! \
  x264enc tune=zerolatency key-int-max=15 speed-preset=ultrafast bitrate=1200 ! \
  h264parse ! video/x-h264,stream-format=byte-stream ! \
  tcpserversink host=0.0.0.0 port=9001 sync=false async=false \
  > /tmp/cam1.log 2>&1 &

# ===== CAMERA 2 =====
gst-launch-1.0 -v \
  v4l2src device=/dev/video2 ! \
  image/jpeg,width=640,height=480,framerate=30/1 ! \
  jpegdec ! videoconvert ! \
  x264enc tune=zerolatency key-int-max=15 speed-preset=ultrafast bitrate=1200 ! \
  h264parse ! video/x-h264,stream-format=byte-stream ! \
  tcpserversink host=0.0.0.0 port=9002 sync=false async=false \
  > /tmp/cam2.log 2>&1 &

echo "Started both camera streams."
