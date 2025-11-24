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

// dual_yolo_zmq.cpp
// Dual-camera YOLOv8 detection with ZeroMQ binary PUB output (no JSON file)

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
    uint8_t camera_id;    // 1 or 2
    uint8_t count;        // number of persons
    uint16_t reserved;    // unused
    float bboxes[6][4];   // x1,y1,x2,y2
    float conf[6];        // confidence
};
#pragma pack(pop)

struct PersonInfo {
    cv::Rect bbox;
    float conf;
};

long long now_ms() {
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string ts_to_str(long long ms) {
    std::time_t t = ms / 1000;
    std::tm tm = *std::localtime(&t);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
    long long rem = ms % 1000;
    oss << "." << std::setw(3) << std::setfill('0') << rem;
    return oss.str();
}

void camera_thread_func(const std::string cam_dev,
                        uint8_t cam_id, const std::string cam_name,
                        int zmq_port,
                        int target_size = 416, float conf_thresh = 0.70f)
{
    try {
        // -------- YOLO LOAD --------
        YoloV8 yolo;
        yolo.load(target_size);

        // -------- ZeroMQ PUB SETUP --------
        zmq::context_t ctx(1);
        zmq::socket_t pub(ctx, ZMQ_PUB);
        std::string url = "tcp://*:" + std::to_string(zmq_port);
        pub.bind(url);

        std::cout << "[ZMQ] Camera " << cam_name
                  << " publishing on " << url << "\n";

        // -------- CAMERA --------
        cv::VideoCapture cap(cam_dev, cv::CAP_V4L2);
        cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
        cap.set(cv::CAP_PROP_FPS, 30);

        if (!cap.isOpened()) {
            std::cerr << "[ERR] Cannot open " << cam_dev << std::endl;
            return;
        }

        std::cout << "[INFO] Camera " << cam_name
                  << " (" << cam_dev << ") started\n";

        cv::Mat frame;
        int frame_count = 0;
        auto t_last = high_resolution_clock::now();

        while (!stop_all) {
            auto t0 = high_resolution_clock::now();

            if (!cap.read(frame) || frame.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            auto t_cap = high_resolution_clock::now();
            double capture_ms =
                duration_cast<microseconds>(t_cap - t0).count() / 1000.0;

            // -------- YOLO INFERENCE --------
            std::vector<Object> objs;
            auto t_infer0 = high_resolution_clock::now();
            yolo.detect(frame, objs, conf_thresh, 0.45f);
            auto t_infer1 = high_resolution_clock::now();

            double infer_ms =
                duration_cast<microseconds>(t_infer1 - t_infer0).count() / 1000.0;

            // -------- FILTER PERSONS --------
            std::vector<PersonInfo> persons;
            for (auto& o : objs) {
                if (o.label == 0) {
                    persons.push_back({ o.rect, o.prob });
                }
            }

            // -------- ONLY PUBLISH IF PERSON DETECTED --------
            if (!persons.empty()) {
                // Prepare binary packet
                DetectionPacket pkt;
                memset(&pkt, 0, sizeof(pkt));
                pkt.camera_id = cam_id;

                int idx = 0;
                for (auto &p : persons) {
                    if (idx >= 6) break;
                    pkt.bboxes[idx][0] = p.bbox.x;
                    pkt.bboxes[idx][1] = p.bbox.y;
                    pkt.bboxes[idx][2] = p.bbox.x + p.bbox.width;
                    pkt.bboxes[idx][3] = p.bbox.y + p.bbox.height;
                    pkt.conf[idx]      = p.conf;
                    idx++;
                }
                pkt.count = idx;

                // Send via ZeroMQ
                zmq::message_t msg(sizeof(pkt));
                memcpy(msg.data(), &pkt, sizeof(pkt));
                pub.send(msg, zmq::send_flags::dontwait);
            }

            // -------- FPS LOGGING --------
            frame_count++;
            auto now = high_resolution_clock::now();
            if (duration_cast<seconds>(now - t_last).count() >= 1) {
                double fps = frame_count /
                             std::max(1.0,
                                      duration_cast<milliseconds>(now - t_last).count() /
                                      1000.0);

                std::cout << "[CAM " << cam_name << "] FPS:" << std::fixed
                          << std::setprecision(1) << fps
                          << " infer:" << infer_ms << "ms\n";

                frame_count = 0;
                t_last = now;
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[EXC] " << cam_name << ": " << e.what() << std::endl;
    }
}

int main()
{
    stop_all = false;

    // Camera mapping
    std::string cam0 = "/dev/video0";
    std::string cam1 = "/dev/video2";

    std::thread t0(camera_thread_func, cam0, 1, "cam1", 5555, 416, 0.70f);
    std::thread t1(camera_thread_func, cam1, 2, "cam2", 5556, 416, 0.70f);

    std::cout << "Publisher running. Press Ctrl+C to exit.\n";

    t0.join();
    t1.join();

    stop_all = true;
    return 0;
}

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

    std::thread t0(camera_thread_tcp, 1, "cam1", 5555, 9001, 416, 0.35f);
    std::thread t1(camera_thread_tcp, 2, "cam2", 5556, 9002, 416, 0.35f);

    t0.join();
    t1.join();

    stop_all = true;
    return 0;
}
