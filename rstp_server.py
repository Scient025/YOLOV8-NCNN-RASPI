import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstRtspServer', '1.0')
from gi.repository import Gst, GstRtspServer, GLib

def main():
    Gst.init(None)

    # 1. Create the Server
    server = GstRtspServer.RTSPServer()
    server.set_service("8555")  # Port 8555
    mounts = server.get_mount_points()

    # --- UPDATED PIPELINE FOR LOW LATENCY ---
    
    # 2. Create Stream for Camera 1 (Virtual Device 11)
    factory1 = GstRtspServer.RTSPMediaFactory()
    # Optimized for low latency, reduced bitrate, and fast recovery
    factory1.set_launch("( v4l2src device=/dev/video11 ! video/x-raw,width=640,height=480,framerate=30/1 ! videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=2048 key-int-max=10 intra-refresh=true vbv-buf-capacity=50 ! rtph264pay name=pay0 pt=96 config-interval=1 )")
    factory1.set_shared(True)
    mounts.add_factory("/cam3", factory1)

    # 3. Create Stream for Camera 2 (Virtual Device 13)
    factory2 = GstRtspServer.RTSPMediaFactory()
    # Optimized for low latency, reduced bitrate, and fast recovery
    factory2.set_launch("( v4l2src device=/dev/video13 ! video/x-raw,width=640,height=480,framerate=30/1 ! videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=2048 key-int-max=10 intra-refresh=true vbv-buf-capacity=50 ! rtph264pay name=pay0 pt=96 config-interval=1 )")
    factory2.set_shared(True)
    mounts.add_factory("/cam4", factory2)

    # 4. Start the Server
    server.attach(None)
    print("RTSP Server is running...")
    print("Stream 1: rtsp://192.168.0.16:8555/cam3")
    print("Stream 2: rtsp://192.168.0.16:8555/cam4")

    loop = GLib.MainLoop()
    loop.run()

if __name__ == '__main__':
    main()