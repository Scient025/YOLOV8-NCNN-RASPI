// yolov8_dualcam.cpp
// Dual-camera real-time human-only detector with async logging.
// Requires yolov8.cpp/yolov8.h (Qengineering / your working YoloV8 class).
// Compile with: g++ yolov8.cpp yolov8_dualcam.cpp -o YoloV8Dual `pkg-config --cflags --libs opencv4` -I /home/pi/ncnn/build/install/include/ncnn -L /home/pi/ncnn/build/install/lib -lncnn -fopenmp -lpthread -O3 -std=c++17

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
    std::string cam;
    int human_count;
    std::vector<PersonInfo> persons;
    long long ts_ms;        // timestamp when detection finished (ms since epoch)
    double capture_ms;      // capture -> detect start (approx)
    double infer_ms;        // inference time (ms)
    double total_ms;        // capture -> finished (ms)
    cv::Mat frame_for_save; // only small crops will be used by logger (move semantics)
};

std::mutex q_mutex;
std::deque<FrameResult> results_q;
std::atomic<bool> stop_all(false);

// Make dir for detections
void ensure_dirs() {
    fs::create_directories("detections");
}

// timestamp helpers
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

// Safe minimal JSON writer (no dependency)
std::string make_json(const FrameResult &r) {
    std::ostringstream j;
    j << "{";
    j << "\"timestamp_ms\":" << r.ts_ms << ",";
    j << "\"timestamp\": \"" << ts_to_str(r.ts_ms) << "\",";
    j << "\"camera\": \"" << r.cam << "\",";
    j << "\"human_count\": " << r.human_count << ",";
    j << "\"capture_ms\": " << std::fixed << std::setprecision(2) << r.capture_ms << ",";
    j << "\"infer_ms\": " << std::fixed << std::setprecision(2) << r.infer_ms << ",";
    j << "\"total_ms\": " << std::fixed << std::setprecision(2) << r.total_ms << ",";
    j << "\"persons\": [";
    for (size_t i = 0; i < r.persons.size(); ++i) {
        const auto &p = r.persons[i];
        j << "{";
        j << "\"bbox\":[" << p.bbox.x << "," << p.bbox.y << "," << p.bbox.width << "," << p.bbox.height << "],";
        j << "\"conf\": " << std::fixed << std::setprecision(3) << p.conf;
        j << "}";
        if (i + 1 < r.persons.size()) j << ",";
    }
    j << "]";
    j << "}";
    return j.str();
}

// Logger thread: writes JSON and saves person crop images
void logger_thread_func() {
    ensure_dirs();
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

        // Only save when human detected > 0
        if (item.human_count > 0) {
            std::string ts = ts_to_str(item.ts_ms);
            // Write JSON
            std::string json_fname = "detections/" + item.cam + "_" + ts + ".json";
            std::ofstream jf(json_fname);
            if (jf.is_open()) {
                jf << make_json(item);
                jf.close();
            }

            // Save cropped person images to save space - each crop saved with index
            for (size_t i = 0; i < item.persons.size(); ++i) {
                const auto &p = item.persons[i];
                // Crop from frame_for_save (which may be full frame or smaller if memory)
                if (!item.frame_for_save.empty()) {
                    cv::Rect r = p.bbox & cv::Rect(0,0,item.frame_for_save.cols, item.frame_for_save.rows);
                    if (r.width > 4 && r.height > 4) {
                        cv::Mat crop = item.frame_for_save(r).clone();
                        std::ostringstream fname;
                        fname << "detections/" << item.cam << "_" << ts << "_" << i << ".jpg";
                        // use modest JPEG quality to save disk IO
                        std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 75 };
                        cv::imwrite(fname.str(), crop, params);
                    }
                }
            }
        }
        // else skip saving to disk to reduce IO
    }
}

// Detection thread for one camera
void camera_thread_func(const std::string cam_dev, int thread_id, int target_size=640, float conf_thresh=0.35f) {
    try {
        // Each thread uses its own YoloV8 instance (avoids locking issues)
        YoloV8 yolo;
        yolo.load(target_size);
        // configure internal net threads if exposed (not in all ports)
        // Example: yolo.net.opt.num_threads = 4; -> depends on implementation

        cv::VideoCapture cap(cam_dev, cv::CAP_V4L2);
        cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
        cap.set(cv::CAP_PROP_FPS, 30);

        if (!cap.isOpened()) {
            std::cerr << "[ERR] Cannot open camera " << cam_dev << std::endl;
            return;
        }
        std::cout << "[INFO] Camera thread " << thread_id << " opened " << cam_dev << std::endl;

        cv::Mat frame;
        // Minimal local counters for FPS smoothing
        int frame_count = 0;
        double avg_infer_ms = 0.0;
        double avg_total_ms = 0.0;
        double avg_fps = 0.0;
        auto t_last_fps = high_resolution_clock::now();

        while (!stop_all) {
            // Capture
            auto t0 = high_resolution_clock::now();
            bool ok = cap.read(frame);
            if (!ok || frame.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            // Keep a light-weight copy for saving crops (logger will clone as needed)
            // Resize small preview copy to reduce logger IO size (optional)
            cv::Mat save_frame_small;
            double save_scale = 1.0;
            if (frame.cols > 960) { save_scale = 0.6; }
            if (save_scale != 1.0) {
                cv::resize(frame, save_frame_small, cv::Size(), save_scale, save_scale, cv::INTER_LINEAR);
            } else {
                save_frame_small = frame;
            }

            auto t_capture_done = high_resolution_clock::now();
            double capture_ms = duration_cast<microseconds>(t_capture_done - t0).count() / 1000.0;

            // Perform detection (this is the main cost)
            auto t_infer_start = high_resolution_clock::now();
            std::vector<Object> objs;
            yolo.detect(frame, objs, conf_thresh, 0.45f); // conf, nms
            auto t_infer_end = high_resolution_clock::now();
            double infer_ms = duration_cast<microseconds>(t_infer_end - t_infer_start).count() / 1000.0;

            // Build FrameResult
            FrameResult res;
            res.cam = fs::path(cam_dev).filename().string(); // e.g. "video0"
            res.ts_ms = now_ms();
            res.capture_ms = capture_ms;
            res.infer_ms = infer_ms;
            res.total_ms = duration_cast<microseconds>(high_resolution_clock::now() - t0).count() / 1000.0;

            // Extract only humans
            for (auto &o : objs) {
                if (o.label == 0) {
                    PersonInfo pi;
                    pi.bbox = o.rect;
                    pi.conf = o.prob;
                    res.persons.push_back(pi);
                }
            }
            res.human_count = (int)res.persons.size();

            // Put small frame for saving (move)
            res.frame_for_save = std::move(save_frame_small);

            {
                std::lock_guard<std::mutex> lock(q_mutex);
                results_q.push_back(std::move(res));
                // keep queue bounded to avoid memory growth
                if (results_q.size() > 200) results_q.pop_front();
            }

            // Local debug print: one line per second
            frame_count++;
            auto now = high_resolution_clock::now();
            if (duration_cast<seconds>(now - t_last_fps).count() >= 1) {
                // compute FPS ~ frames processed per sec
                double fps = frame_count / std::max(1.0, duration_cast<milliseconds>(now - t_last_fps).count() / 1000.0);
                std::cout << "[CAM " << cam_dev << "] FPS: " << std::fixed << std::setprecision(1) << fps
                          << " | infer_ms: " << std::fixed << std::setprecision(1) << infer_ms
                          << " | humans: " << res.human_count << std::endl;
                frame_count = 0;
                t_last_fps = now;
            }

            // Minimal delay: let thread yield
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception &e) {
        std::cerr << "[EXC] camera thread " << cam_dev << " : " << e.what() << std::endl;
    }
}

int main(int argc, char** argv) {
    std::string cam0 = (argc > 1) ? argv[1] : "/dev/video0";
    std::string cam1 = (argc > 2) ? argv[2] : "/dev/video2";

    ensure_dirs();
    stop_all = false;

    // Launch logger thread
    std::thread logger_thread(logger_thread_func);

    // Launch two camera threads
    std::thread t0(camera_thread_func, cam0, 0, 640, 0.35f);
    std::thread t1(camera_thread_func, cam1, 1, 640, 0.35f);

    std::cout << "Press Ctrl-C to stop\n";

    // wait for Ctrl-C (or manual interrupt)
    // simple join loop; set stop_all=true externally if you prefer signals
    t0.join();
    t1.join();

    stop_all = true;
    logger_thread.join();

    return 0;
}
