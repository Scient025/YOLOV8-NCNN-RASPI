// yolov8z_gst.cpp
// Dual-camera YOLOv8 with ZeroMQ + GStreamer tee (YOLO + MJPEG in same pipeline)

#include "yoloV8.h"
#include <opencv2/opencv.hpp>
#include <chrono>
#include <thread>
#include <atomic>
#include <zmq.hpp>
#include <cstring>
#include <iomanip>
#include <sstream>

using namespace std::chrono;

std::atomic<bool> stop_all(false);

#pragma pack(push,1)
struct DetectionPacket {
    uint8_t camera_id;
    uint8_t count;
    uint16_t reserved;
    float bboxes[6][4];
    float conf[6];
};
#pragma pack(pop)

struct PersonInfo {
    cv::Rect bbox;
    float conf;
};

long long now_ms() {
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void camera_thread_gst(const std::string& cam_dev,
                       uint8_t cam_id,
                       const std::string& cam_name,
                       int zmq_port,
                       int mjpeg_port,
                       int target_size = 416,
                       float conf_thresh = 0.35f)
{
    try {
        // ---------------- YOLO LOAD ----------------
        YoloV8 yolo;
        yolo.load(target_size);

        // ---------------- ZeroMQ ----------------
        zmq::context_t ctx(1);
        zmq::socket_t pub(ctx, ZMQ_PUB);

        std::string url = "tcp://*:" + std::to_string(zmq_port);
        pub.bind(url);

        std::cout << "[ZMQ] " << cam_name
                  << " publishing detections on " << url << "\n";

        // ---------------- Build GStreamer pipeline ----------------
        std::ostringstream oss;
        oss << "v4l2src device=" << cam_dev << " ! "
            << "video/x-raw,format=YUY2,width=640,height=480,framerate=30/1 ! "
            << "tee name=t "
            // Branch A → YOLO
            << "t. ! queue ! videoconvert ! video/x-raw,format=BGR ! "
            << "appsink name=mysink sync=false max-buffers=2 drop=true "
            // Branch B → MJPEG streaming
            << "t. ! queue ! videoconvert ! jpegenc ! multipartmux boundary=spionisto ! "
            << "tcpserversink host=0.0.0.0 port=" << mjpeg_port;

        // YOLO reads MJPEG stream instead of the raw camera
        std::string mjpeg_url = "http://127.0.0.1:" + std::to_string(mjpeg_port) + "/?action=stream";
        std::cout << "[MJPEG] Reading from " << mjpeg_url << "\n";

        cv::VideoCapture cap(mjpeg_url);
        if (!cap.isOpened()) {
            std::cerr << "[ERR] Cannot open MJPEG stream at " << mjpeg_url << "\n";
            return;
        }

        std::cout << "[INFO] " << cam_name << " started | MJPEG port " << mjpeg_port << "\n";

        cv::Mat frame;
        int frame_count = 0;
        double infer_ms = 0;
        auto t_last = high_resolution_clock::now();

        // ---------------- Main Loop ----------------
        while (!stop_all) {

            if (!cap.read(frame) || frame.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // YOLO inference
            auto t_infer0 = high_resolution_clock::now();
            std::vector<Object> objs;
            yolo.detect(frame, objs, conf_thresh, 0.45f);
            auto t_infer1 = high_resolution_clock::now();
            infer_ms = duration_cast<microseconds>(t_infer1 - t_infer0).count() / 1000.0;

            // Get only persons
            std::vector<PersonInfo> persons;
            for (auto& o : objs) {
                if (o.label == 0)
                    persons.push_back({o.rect, o.prob});
            }

            if (!persons.empty()) {
                DetectionPacket pkt {};
                pkt.camera_id = cam_id;

                int idx = 0;
                for (auto& p : persons) {
                    if (idx >= 6) break;
                    pkt.bboxes[idx][0] = p.bbox.x;
                    pkt.bboxes[idx][1] = p.bbox.y;
                    pkt.bboxes[idx][2] = p.bbox.x + p.bbox.width;
                    pkt.bboxes[idx][3] = p.bbox.y + p.bbox.height;
                    pkt.conf[idx] = p.conf;
                    idx++;
                }
                pkt.count = idx;

                zmq::message_t msg(sizeof(pkt));
                memcpy(msg.data(), &pkt, sizeof(pkt));
                pub.send(msg, zmq::send_flags::dontwait);
            }

            // FPS logging
            frame_count++;
            auto now = high_resolution_clock::now();
            if (duration_cast<seconds>(now - t_last).count() >= 1) {
                double fps = frame_count /
                             std::max(1.0,
                                 duration_cast<milliseconds>(now - t_last).count() / 1000.0);

                std::cout << "[CAM " << cam_name << "] FPS:" << fps
                          << " infer:" << infer_ms << "ms\n";
                frame_count = 0;
                t_last = now;
            }
        }

    } catch (std::exception& e) {
        std::cerr << "[EXC] " << cam_name << ": " << e.what() << "\n";
    }
}

int main() {
    stop_all = false;

    std::thread t0(camera_thread_gst,
                   "/dev/video0", 1, "cam1",
                   5555, 8080,   // zmq_port, mjpeg_port
                   416, 0.35f);

    std::thread t1(camera_thread_gst,
                   "/dev/video2", 2, "cam2",
                   5556, 8081,
                   416, 0.35f);

    std::cout << "Publisher running (GStreamer). Press Ctrl+C to exit.\n";

    t0.join();
    t1.join();

    stop_all = true;
    return 0;
}
