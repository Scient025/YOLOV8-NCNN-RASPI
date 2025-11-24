import subprocess
import os
import signal
import time
import sys

# --- CONFIGURATION ---

# Raspberry Pi Cluster Credentials
RPI_CONFIG = [
    {"user": "pi", "ip": "192.168.0.186", "pass": "pi"},
    {"user": "olee", "ip": "192.168.0.16", "pass": "Citoto@321"},
    {"user": "olee", "ip": "192.168.0.158", "pass": "Citoto@321"},
]

# GStreamer Pipeline for 6-camera video composition
GST_PIPE = """
gst-launch-1.0 -e compositor name=comp background=black \\
    sink_0::xpos=0   sink_0::ypos=0   sink_0::width=640 sink_0::height=360 \\
    sink_1::xpos=640 sink_1::ypos=0   sink_1::width=640 sink_1::height=360 \\
    sink_2::xpos=1280 sink_2::ypos=0   sink_2::width=640 sink_2::height=360 \\
    sink_3::xpos=0   sink_3::ypos=360 sink_3::width=640 sink_3::height=360 \\
    sink_4::xpos=640 sink_4::ypos=360 sink_4::width=640 sink_4::height=360 \\
    sink_5::xpos=1280 sink_5::ypos=360 sink_5::width=640 sink_5::height=360 \\
    ! videoconvert ! autovideosink sync=false \\
    rtspsrc location=rtsp://192.168.0.186:8556/cam1 latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! comp. \\
    rtspsrc location=rtsp://192.168.0.186:8556/cam2 latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! comp. \\
    rtspsrc location=rtsp://192.168.0.16:8555/cam3 latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! comp. \\
    rtspsrc location=rtsp://192.168.0.16:8555/cam4 latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! comp. \\
    rtspsrc location=rtsp://192.168.0.158:8554/cam5 latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! comp. \\
    rtspsrc location=rtsp://192.168.0.158:8554/cam6 latency=0 protocols=tcp ! rtph264depay ! h264parse ! avdec_h264 ! comp.
"""

# Global process variables
gst_process = None

# --- CORE FUNCTIONS (YOLO/GStreamer Control) ---

def execute_remote_script(pi_config, script_name):
    """Executes start.sh or stop.sh on a remote Raspberry Pi."""
    
    # Command to change directory and execute the script in the background with nohup 
    # (only for start.sh, stop.sh runs in foreground)
    if script_name == "start.sh":
        remote_cmd = f'cd YOLOV8-NCNN-RASPI && nohup ./{script_name} > {script_name}.log 2>&1 &'
    else: # stop.sh
        remote_cmd = f'cd YOLOV8-NCNN-RASPI && ./{script_name}'
        
    ssh_cmd = [
        'sshpass', '-p', pi_config["pass"], 
        'ssh', '-o', 'StrictHostKeyChecking=no', 
        f'{pi_config["user"]}@{pi_config["ip"]}', remote_cmd
    ]
    
    try:
        # Use check=False because sshpass or stop.sh might return non-zero (e.g., if process wasn't running)
        subprocess.run(ssh_cmd, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        print(f"✅ Executed {script_name} successfully on {pi_config['ip']}")
    except FileNotFoundError:
         print("❌ ERROR: 'sshpass' command not found. Install it: sudo apt install sshpass")
         sys.exit(1)
    except subprocess.TimeoutExpired:
         print(f"⚠️ Warning: Timeout executing {script_name} on {pi_config['ip']}")


def start_yolo_jobs():
    """Starts the YOLO process on all remote Raspberry Pis using start.sh."""
    print("🚀 Starting YOLO jobs (running start.sh remotely)...")
    for pi in RPI_CONFIG:
        execute_remote_script(pi, "start.sh")

def stop_yolo_jobs():
    """Stops the YOLO process on all remote Raspberry Pis using stop.sh."""
    print("🛑 Stopping YOLO jobs (running stop.sh remotely)...")
    for pi in RPI_CONFIG:
        execute_remote_script(pi, "stop.sh")


def start_gstreamer():
    """Launches the 6-camera GStreamer compositor pipe."""
    global gst_process
    print("\n🎥 Launching 6-camera GStreamer compositor...")
    try:
        # Use shell=True for the complex pipeline string and preexec_fn to create a process group
        gst_process = subprocess.Popen(GST_PIPE, shell=True, preexec_fn=os.setsid)
        print(f"✅ GStreamer started with PID: {gst_process.pid}")
    except FileNotFoundError:
        print("❌ ERROR: 'gst-launch-1.0' command not found. Install gstreamer-1.0.")
        
def stop_gstreamer():
    """Terminates the GStreamer process."""
    global gst_process
    if gst_process and gst_process.poll() is None:
        print("🛑 Stopping GStreamer compositor...")
        try:
            # Send SIGTERM to the process group to ensure all child elements are killed
            os.killpg(os.getpgid(gst_process.pid), signal.SIGTERM)
            gst_process.wait(timeout=5)
            print("✅ GStreamer stopped.")
        except Exception as e:
            print(f"❌ Error stopping GStreamer: {e}")

# --- C++ SUBSCRIBER CONTROL ---

def subscriber_worker():
    """Runs the compiled C++ subscriber executable in the foreground for live output."""
    try:
        # Execute the compiled C++ application
        subprocess.run(["./subscriber_app"], check=True)
    except subprocess.CalledProcessError as e:
        print(f"❌ C++ Subscriber exited with error: {e}")
    except FileNotFoundError:
        print("\n❌ ERROR: 'subscriber_app' not found. Please compile the C++ code first.")
        
# --- UI & MAIN CONTROL ---

def display_ui_and_run():
    """A text-based 'UI' for control."""
    global gst_process
    
    while True:
        print("\n" + "="*50)
        print("🎥 Multi-Camera YOLO Control Console 🤖")
        print("="*50)
        print("Status:")
        # Check if the GStreamer process is running
        gst_status = 'RUNNING' if gst_process and gst_process.poll() is None else 'STOPPED'
        print(f"  System Jobs: {gst_status}")
        print("\nOptions:")
        print("  [1] Start All (YOLO (start.sh) + GStreamer)")
        print("  [2] Stop All (YOLO (stop.sh) + GStreamer)")
        print("  [3] Show Subscriber Output (Runs C++ app live)")
        print("  [4] Exit")
        print("="*50)
        
        choice = input("Enter your choice: ").strip()

        if choice == '1':
            if gst_status == 'RUNNING':
                print("Jobs appear to be running. Please stop them first before restarting.")
                continue
                
            start_yolo_jobs()
            start_gstreamer()
            
        elif choice == '2':
            stop_gstreamer()
            stop_yolo_jobs()
            gst_process = None # Clear the global variable reference
            
        elif choice == '3':
            print("\n--- Live C++ Subscriber Feed (Press Ctrl+C to return to menu) ---")
            try:
                subscriber_worker()
            except KeyboardInterrupt:
                print("\nReturning to main menu...")
            
        elif choice == '4':
            print("Exiting. Stopping all running processes...")
            stop_gstreamer()
            stop_yolo_jobs()
            break
            
        else:
            print("Invalid choice. Please try again.")

if __name__ == "__main__":
    try:
        display_ui_and_run()
    except Exception as e:
        print(f"\nAn unexpected error occurred: {e}")
    finally:
        # Ensure everything is killed on script exit
        stop_gstreamer()
        stop_yolo_jobs()