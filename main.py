#!/usr/bin/env python3
import subprocess
import os
import signal
import tkinter as tk
from tkinter import messagebox

pipeline_cmd = """
gst-launch-1.0 -e \
compositor name=comp background=black \
sink_0::xpos=0   sink_0::ypos=0   sink_0::width=640  sink_0::height=360 \
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
"""

gst_process = None

def start_stream():
    global gst_process
    if gst_process is None:
        try:
            gst_process = subprocess.Popen(
                pipeline_cmd,
                shell=True,
                preexec_fn=os.setsid,  # Start new process group
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE
            )
            status_label.config(text="Status: Streaming", fg="green")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to start pipeline:\n{e}")

def stop_stream():
    global gst_process
    if gst_process is not None:
        try:
            os.killpg(os.getpgid(gst_process.pid), signal.SIGKILL)  # Kill full process group
        except Exception as e:
            print("Kill error:", e)
        gst_process = None

    status_label.config(text="Status: Stopped", fg="red")
    root.quit()

# GUI Window
root = tk.Tk()
root.title("GStreamer Multi-Cam Viewer")
root.geometry("340x160")

status_label = tk.Label(root, text="Status: Not Running", font=("Arial", 13))
status_label.pack(pady=15)

btn_start = tk.Button(root, text="START", font=("Arial", 14), bg="#3bb143", fg="white", width=12, command=start_stream)
btn_start.pack(pady=5)

btn_stop = tk.Button(root, text="STOP / EXIT", font=("Arial", 14), bg="#d62828", fg="white", width=12, command=stop_stream)
btn_stop.pack(pady=5)

root.mainloop()

