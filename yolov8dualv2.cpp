// yolov8dualv2.cpp
// Dual-camera YOLOv8 headless version (fixed names cam1, cam2)

#include "yoloV8.h"
#include <opencv2/opencv.hpp>
#include <chrono>
#include <thread>
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

std::string make_json(const std::string& cam_name, int human_count,
                      const std::vector<PersonInfo>& persons,
                      double capture_ms, double infer_ms, double total_ms, long long ts_ms)
{
    std::ostringstream j;
    j << "{";
    j << "\"timestamp_ms\":" << ts_ms << ",";
    j << "\"timestamp\":\"" << ts_to_str(ts_ms) << "\",";
    j << "\"camera\":\"" << cam_name << "\",";
    j << "\"human_count\":" << human_count << ",";
    j << "\"capture_ms\":" << std::fixed << std::setprecision(2) << capture_ms << ",";
    j << "\"infer_ms\":" << infer_ms << ",";
    j << "\"total_ms\":" << total_ms << ",";
    j << "\"persons\":[";
    for (size_t i = 0; i < persons.size(); ++i) {
        const auto& p = persons[i];
        j << "{\"bbox\":[" << p.bbox.x << "," << p.bbox.y << "," << p.bbox.width << "," << p.bbox.height
          << "],\"conf\":" << std::fixed << std::setprecision(3) << p.conf << "}";
        if (i + 1 < persons.size()) j << ",";
    }
    j << "]}";
    return j.str();
}

void camera_thread_func(const std::string cam_dev, const std::string cam_name,
                        int target_size = 416, float conf_thresh = 0.35f)
{
    try {
        fs::create_directories("detections");
        std::string log_path = "detections/" + cam_name + ".json";

        std::ofstream jf(log_path, std::ios::app);
        if (!jf.is_open()) {
            std::cerr << "[ERR] Cannot open " << log_path << " for writing\n";
            return;
        }

        jf << "[\n";
        bool first_entry = true;

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

            std::vector<PersonInfo> persons;
            for (auto& o : objs) {
                if (o.label == 0) {
                    persons.push_back({ o.rect, o.prob });
                }
            }

            if (!persons.empty()) {
                long long ts_ms = now_ms();
                double total_ms = duration_cast<microseconds>(high_resolution_clock::now() - t0).count() / 1000.0;
                std::string js = make_json(cam_name, persons.size(), persons, capture_ms, infer_ms, total_ms, ts_ms);
                if (!first_entry) jf << ",\n";
                first_entry = false;
                jf << js;
                jf.flush();
            }

            frame_count++;
            auto now = high_resolution_clock::now();
            if (duration_cast<seconds>(now - t_last).count() >= 1) {
                double fps = frame_count / std::max(1.0, duration_cast<milliseconds>(now - t_last).count() / 1000.0);
                std::cout << "[CAM " << cam_name << "] FPS:" << std::fixed << std::setprecision(1)
                          << fps << " infer:" << infer_ms << "ms\n";
                frame_count = 0;
                t_last = now;
            }
        }

        jf << "\n]\n";
        jf.close();
    }
    catch (const std::exception& e) {
        std::cerr << "[EXC] " << cam_name << ": " << e.what() << std::endl;
    }
}

int main()
{
    std::string cam0 = "/dev/video0";
    std::string name0 = "cam1";
    std::string cam1 = "/dev/video2";
    std::string name1 = "cam2";

    stop_all = false;

    std::thread t0(camera_thread_func, cam0, name0, 416, 0.35f);
    std::thread t1(camera_thread_func, cam1, name1, 416, 0.35f);

    std::cout << "Press Ctrl-C to stop\n";

    t0.join();
    t1.join();

    stop_all = true;
    return 0;
}
