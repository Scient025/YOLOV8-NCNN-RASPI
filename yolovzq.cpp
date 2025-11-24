// Dual-camera YOLOv8 with ZeroMQ binary publisher (Frame Packet mode)
// Based on the original uploaded file. :contentReference[oaicite:1]{index=1}

#include "yoloV8.h"
#include <opencv2/opencv.hpp>
#include <chrono>
#include <thread>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <vector>
#include <zmq.hpp>

using namespace std;
using namespace std::chrono;

// ---------------------- Time Helper ----------------------
long long now_ms() {
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ---------------------- Atomic stop flag -----------------
std::atomic<bool> stop_all(false);

// ---------------------- Binary Structs --------------------
#pragma pack(push, 1)
struct Person {
    float x, y, w, h;   // bbox
    float cx, cy;       // center
    float conf;         // confidence
};

struct FramePacketHeader {
    uint8_t cam_id;
    uint8_t count;       // number of persons
    uint64_t ts_ms;      // timestamp
    // Followed by count × Person structs
};
#pragma pack(pop)

// Assign ports for cameras
int port_for_camera(const string& cam_name) {
    if (cam_name == "cam3") return 5556;
    if (cam_name == "cam4") return 5557;
    return 5566; // fallback
}

// ---------------------- Camera Thread ----------------------
void camera_thread_func(const string cam_dev, const string cam_name,
                        int cam_id, int target_size = 416, float conf_thresh = 0.35f)
{
    try {
        // ZMQ context + PUB socket
        zmq::context_t context(1);
        zmq::socket_t pub(context, zmq::socket_type::pub);

        int port = port_for_camera(cam_name);
        string bind_addr = "tcp://*:" + to_string(port);

        try {
            pub.bind(bind_addr);
            cout << "[ZMQ] " << cam_name << " bound to " << bind_addr << endl;
        } catch (const zmq::error_t &e) {
            cerr << "[ZMQ ERR] Cannot bind " << bind_addr << ": " << e.what() << endl;
            return;
        }

        // Load YOLO model
        YoloV8 yolo;
        yolo.load(target_size);

        // Open video
        cv::VideoCapture cap(cam_dev, cv::CAP_V4L2);
        cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
        cap.set(cv::CAP_PROP_FPS, 30);

        if (!cap.isOpened()) {
            cerr << "[ERR] Cannot open " << cam_dev << endl;
            return;
        }

        cout << "[INFO] Camera " << cam_name << " started (" << cam_dev << ")" << endl;

        cv::Mat frame;
        int frame_count = 0;
        auto t_last = high_resolution_clock::now();

        // Main loop
        while (!stop_all.load()) {
            auto t0 = high_resolution_clock::now();

            if (!cap.read(frame) || frame.empty()) {
                this_thread::sleep_for(2ms);
                continue;
            }

            vector<Object> objs;
            auto t_infer0 = high_resolution_clock::now();
            yolo.detect(frame, objs, conf_thresh, 0.45f);
            auto t_infer1 = high_resolution_clock::now();

            // Build list of Person structs
            vector<Person> persons;
            for (auto &o : objs) {
                if (o.label == 0) {  // only person class
                    Person p;
                    p.x = o.rect.x;
                    p.y = o.rect.y;
                    p.w = o.rect.width;
                    p.h = o.rect.height;
                    p.cx = p.x + p.w * 0.5f;
                    p.cy = p.y + p.h * 0.5f;
                    p.conf = o.prob;
                    persons.push_back(p);
                }
            }

            // Prepare binary frame packet
            FramePacketHeader header;
            header.cam_id = cam_id;
            header.count  = persons.size();
            header.ts_ms  = now_ms();

            // Final binary buffer = header + array of Person structs
            size_t total_size = sizeof(FramePacketHeader) + persons.size() * sizeof(Person);
            zmq::message_t msg(total_size);

            // Copy header + persons into message buffer
            uint8_t* ptr = (uint8_t*)msg.data();
            memcpy(ptr, &header, sizeof(FramePacketHeader));
            memcpy(ptr + sizeof(FramePacketHeader), persons.data(), persons.size() * sizeof(Person));

            // Send packet
            try {
                pub.send(msg, zmq::send_flags::none);
            } 
            catch (const zmq::error_t &e) {
                cerr << "[ZMQ ERR] send: " << e.what() << endl;
            }

            // FPS logging
            frame_count++;
            auto now = high_resolution_clock::now();
            if (duration_cast<seconds>(now - t_last).count() >= 1) {
                double infer_ms = duration_cast<microseconds>(t_infer1 - t_infer0).count() / 1000.0;
                cout << "[FPS] " << cam_name << " " << frame_count
                     << "fps infer=" << infer_ms << "ms persons=" << persons.size() << endl;
                frame_count = 0;
                t_last = now;
            }
        }

    } catch (const exception &e) {
        cerr << "[EXC] " << cam_name << ": " << e.what() << endl;
    }
}

// ---------------------- MAIN ----------------------
int main() {
    stop_all = false;

    thread t0(camera_thread_func, "/dev/video10", "cam3", 0);
    thread t1(camera_thread_func, "/dev/video12", "cam4", 1);

    cout << "YOLO ZMQ Publisher running. Press Ctrl+C to stop." << endl;

    t0.join();
    t1.join();

    stop_all = true;
    return 0;
}
