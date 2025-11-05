// yolov8_dualcam_headless.cpp
// Dual-camera headless YOLOv8 human detector with JSON logging only.
// Compile:
// g++ yolov8.cpp yolov8_dualcam_headless.cpp -o YoloV8DualHeadless \
//     `pkg-config --cflags --libs opencv4` \
//     -I /home/pi/ncnn/build/install/include/ncnn \
//     -L /home/pi/ncnn/build/install/lib -lncnn -fopenmp -lpthread -O3 -std=c++17

#include "yoloV8.h"
#include <opencv2/opencv.hpp>
#include <chrono>
#include <thread>
#include <mutex>
#include <deque>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;
using namespace std::chrono;

struct PersonInfo {
    cv::Rect bbox;
    float conf;
};
struct FrameResult {
    std::string cam_name;
    int human_count;
    std::vector<PersonInfo> persons;
    long long ts_ms;
    double capture_ms;
    double infer_ms;
    double total_ms;
};

std::mutex q_mutex;
std::deque<FrameResult> results_q;
std::atomic<bool> stop_all(false);

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
std::string make_json(const FrameResult& r) {
    std::ostringstream j;
    j << "{";
    j << "\"timestamp_ms\":" << r.ts_ms << ",";
    j << "\"timestamp\":\"" << ts_to_str(r.ts_ms) << "\",";
    j << "\"camera\":\"" << r.cam_name << "\",";
    j << "\"human_count\":" << r.human_count << ",";
    j << "\"capture_ms\":" << std::fixed << std::setprecision(2) << r.capture_ms << ",";
    j << "\"infer_ms\":" << r.infer_ms << ",";
    j << "\"total_ms\":" << r.total_ms << ",";
    j << "\"persons\":[";
    for (size_t i = 0; i < r.persons.size(); ++i) {
        const auto& p = r.persons[i];
        j << "{\"bbox\":[" << p.bbox.x << "," << p.bbox.y << "," << p.bbox.width
          << "," << p.bbox.height << "],\"conf\":" << std::fixed << std::setprecision(3) << p.conf << "}";
        if (i + 1 < r.persons.size()) j << ",";
    }
    j << "]}";
    return j.str();
}

void logger_thread_func() {
    fs::create_directories("detections");
    while (!stop_all) {
        FrameResult item;
        bool has = false;
        {
            std::lock_guard<std::mutex> lock(q_mutex);
            if (!results_q.empty()) {
                item = std::move(results_q.front());
                results_q.pop_front();
                has = true;
            }
        }
        if (!has) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (item.human_count > 0) {
            std::string ts = ts_to_str(item.ts_ms);
            std::string json_fname = "detections/" + item.cam_name + "_" + ts + ".json";
            std::ofstream jf(json_fname);
            if (jf.is_open()) {
                jf << make_json(item);
                jf.close();
            }
        }
    }
}

void camera_thread_func(const std::string cam_dev, const std::string cam_name,
                        int target_size = 416, float conf_thresh = 0.35f) {
    try {
        YoloV8 yolo;
        yolo.load(target_size);

        cv::VideoCapture cap(cam_dev, cv::CAP_V4L2);
        cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
        cap.set(cv::CAP_PROP_FPS, 30);
        if (!cap.isOpened()) {
            std::cerr << "[ERR] Cannot open " << cam_dev << std::endl;
            return;
        }
        std::cout << "[INFO] Camera " << cam_name << " (" << cam_dev << ") started\n";

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
            double capture_ms = duration_cast<microseconds>(t_cap - t0).count() / 1000.0;

            std::vector<Object> objs;
            auto t_infer0 = high_resolution_clock::now();
            yolo.detect(frame, objs, conf_thresh, 0.45f);
            auto t_infer1 = high_resolution_clock::now();
            double infer_ms = duration_cast<microseconds>(t_infer1 - t_infer0).count() / 1000.0;

            FrameResult res;
            res.cam_name = cam_name;
            res.ts_ms = now_ms();
            res.capture_ms = capture_ms;
            res.infer_ms = infer_ms;
            res.total_ms = duration_cast<microseconds>(high_resolution_clock::now() - t0).count() / 1000.0;

            for (auto& o : objs) {
                if (o.label == 0) {
                    PersonInfo p{ o.rect, o.prob };
                    res.persons.push_back(p);
                }
            }
            res.human_count = (int)res.persons.size();

            if (res.human_count > 0) {
                std::lock_guard<std::mutex> lock(q_mutex);
                results_q.push_back(std::move(res));
                if (results_q.size() > 200) results_q.pop_front();
            }

            frame_count++;
            auto now = high_resolution_clock::now();
            if (duration_cast<seconds>(now - t_last).count() >= 1) {
                double fps = frame_count / std::max(1.0, duration_cast<milliseconds>(now - t_last).count() / 1000.0);
                std::cout << "[CAM " << cam_name << "] FPS:" << std::fixed << std::setprecision(1) << fps
                          << " infer:" << infer_ms << "ms" << std::endl;
                frame_count = 0;
                t_last = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& e) {
        std::cerr << "[EXC] " << cam_name << ": " << e.what() << std::endl;
    }
}

int main(int argc, char** argv) {
    std::string cam0 = (argc > 1) ? argv[1] : "/dev/video0";
    std::string name0 = (argc > 2) ? argv[2] : "cam0";
    std::string cam1 = (argc > 3) ? argv[3] : "/dev/video2";
    std::string name1 = (argc > 4) ? argv[4] : "cam1";

    stop_all = false;
    std::thread logger(logger_thread_func);
    std::thread t0(camera_thread_func, cam0, name0, 416, 0.35f);
    std::thread t1(camera_thread_func, cam1, name1, 416, 0.35f);

    std::cout << "Press Ctrl-C to stop\n";
    t0.join();
    t1.join();
    stop_all = true;
    logger.join();
    return 0;
}
