// ---------------------------------------------------------
// YoloV8ZMQS.cpp  (TCP H264 + YOLO + ZMQ + TIMESTAMP FIXED)
// ---------------------------------------------------------

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

// ------------- FIXED STRUCT (Pi -> Jetson) ----------------
struct DetectionPacket {
    uint8_t  camera_id;        
    uint8_t  count;            
    uint16_t reserved;         
    float    bboxes[6][4];     
    float    conf[6];          
    uint64_t timestamp_ms;     // Pi timestamp (for latency)
} __attribute__((packed));

static_assert(sizeof(DetectionPacket) == 132, "DetectionPacket MUST be 132 bytes!");


long long now_ms() {
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct PersonInfo {
    cv::Rect bbox;
    float conf;
};


void camera_thread_tcp(uint8_t cam_id,
                       const std::string& cam_name,
                       int zmq_port,
                       int tcp_port,
                       int target_size = 416,
                       float conf_thresh = 0.35f)
{
    try {
        std::cout << "\n------------------------------\n";
        std::cout << "[START] " << cam_name << " TCP H264\n";
        std::cout << "------------------------------\n";

        // ---------------- YOLO ----------------
        YoloV8 yolo;
        yolo.load(target_size);

        // ---------------- ZMQ ----------------
        zmq::context_t ctx(1);
        zmq::socket_t pub(ctx, ZMQ_PUB);
        std::string url = "tcp://*:" + std::to_string(zmq_port);
        pub.bind(url);

        std::cout << "[ZMQ] " << cam_name
                  << " publishing on " << url << "\n";

        // ---------------- GStreamer pipeline ----------------
        std::ostringstream oss;
        oss << "tcpclientsrc host=127.0.0.1 port=" << tcp_port << " ! "
            << "h264parse ! avdec_h264 ! videoconvert ! "
            << "video/x-raw,format=BGR ! "
            << "appsink drop=true sync=false max-buffers=2";

        std::string gst_pipeline = oss.str();

        std::cout << "[GST] Pipeline: " << gst_pipeline << "\n";

        // Delay for encoder stabilization
        std::cout << "[GST] Waiting 1.5s for encoder to start...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        // ---------------- Retry loop ----------------
        cv::VideoCapture cap;
        bool opened = false;

        for (int i = 0; i < 20; i++) {
            std::cout << "[GST] Trying to connect to port " << tcp_port << " ("
                      << (i+1) << "/20)...\n";

            if (cap.open(gst_pipeline, cv::CAP_GSTREAMER)) {
                opened = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        if (!opened) {
            std::cerr << "[ERR] " << cam_name << " unable to open TCP camera stream\n";
            return;
        }

        std::cout << "[INFO] " << cam_name << " started | TCP port " << tcp_port << "\n";

        // ---------------- Main Loop ----------------
        cv::Mat frame;
        int frame_count = 0;
        double infer_ms = 0;
        auto t_last = high_resolution_clock::now();

        while (!stop_all) {
            if (!cap.read(frame) || frame.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            auto t0 = high_resolution_clock::now();
            std::vector<Object> objs;
            yolo.detect(frame, objs, conf_thresh, 0.45f);
            auto t1 = high_resolution_clock::now();
            infer_ms = duration_cast<microseconds>(t1 - t0).count() / 1000.0;

            // Extract people
            std::vector<PersonInfo> persons;
            for (auto& o : objs)
                if (o.label == 0)
                    persons.push_back({ o.rect, o.prob });

            // If detections found → send packet
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
                pkt.timestamp_ms = now_ms();

                zmq::message_t msg(sizeof(pkt));
                memcpy(msg.data(), &pkt, sizeof(pkt));
                pub.send(msg, zmq::send_flags::dontwait);
            }

            // FPS log
            frame_count++;
            auto now = high_resolution_clock::now();
            if (duration_cast<seconds>(now - t_last).count() >= 1) {
                float fps = frame_count /
                    (duration_cast<milliseconds>(now - t_last).count() / 1000.0);

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

    std::cout << "Publisher running (TCP H.264 + YOLO). Press Ctrl+C to exit.\n";

    std::thread t0(camera_thread_tcp, 1, "cam5", 5557, 9005, 416, 0.35f);
    std::thread t1(camera_thread_tcp, 2, "cam6", 5558, 9006, 416, 0.35f);

    t0.join();
    t1.join();

    stop_all = true;
    return 0;
}
