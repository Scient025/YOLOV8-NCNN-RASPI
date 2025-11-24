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
#include <queue>
#include <condition_variable>
#include <pthread.h>
#include <sys/syscall.h> // optional for tid printing

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
struct LogEntry {
    std::string json;
    std::string cam_name;
    long long ts;
};

class AsyncLogger {
public:
    AsyncLogger(const std::string& outdir = "detections", size_t max_q = 2048)
        : max_queue(max_q), stop(false)
    {
        fs::create_directories(outdir);
        // We will create one file per camera on demand
        thread = std::thread(&AsyncLogger::worker, this);
    }

    ~AsyncLogger() {
        {
            std::unique_lock<std::mutex> lk(mtx);
            stop = true;
            cv.notify_all();
        }
        thread.join();
        // close files
        for (auto &kv : files) {
            auto &f = kv.second;
            if (f.is_open()) {
                f << "\n]\n";
                f.close();
            }
        }
    }

    // non-blocking push: if queue full, drop oldest entry
    void push(const std::string& cam_name, std::string json, long long ts) {
        std::unique_lock<std::mutex> lk(mtx);
        if (queue.size() >= max_queue) {
            // drop oldest to make room (log a single drop event)
            queue.pop();
            dropped++;
        }
        queue.push({json, cam_name, ts});
        cv.notify_one();
    }

    void worker() {
        while (true) {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait(lk, [&]{ return stop || !queue.empty(); });
            if (stop && queue.empty()) break;

            auto e = queue.front();
            queue.pop();
            lk.unlock();

            // open file for camera if needed
            std::string path = std::string("detections/") + e.cam_name + ".json";
            std::ofstream &jf = files[e.cam_name];
            if (!jf.is_open()) {
                jf.open(path, std::ios::app);
                if (!jf.is_open()) {
                    std::cerr << "[LOGERR] cannot open " << path << "\n";
                    continue;
                }
                // If file is new (empty), write opening array (try to detect)
                // (simple heuristic: if file position == 0)
                jf.seekp(0, std::ios::end);
                if (jf.tellp() == 0) {
                    jf << "[\n";
                    first_written[e.cam_name] = false;
                } else {
                    // file non-empty -> assume it already ends with ]\n; so we remove last char... skip complexity
                    // To keep simple, we'll append comma-prefixed entries.
                    first_written[e.cam_name] = true;
                }
            }

            if (first_written[e.cam_name]) {
                jf << ",\n";
            } else {
                first_written[e.cam_name] = true;
            }
            jf << e.json;
            // occasional flush to ensure disk safety but not for every entry
            static int counter = 0;
            if ((++counter & 0x3) == 0) jf.flush(); // flush every 4 writes globally
        }
    }

    size_t dropped_count() {
        std::unique_lock<std::mutex> lk(mtx);
        return dropped;
    }

private:
    std::unordered_map<std::string, std::ofstream> files;
    std::unordered_map<std::string, bool> first_written;
    std::queue<LogEntry> queue;
    std::mutex mtx;
    std::condition_variable cv;
    std::thread thread;
    size_t max_queue;
    bool stop;
    size_t dropped = 0;
};

// Global logger (single instance)
static std::unique_ptr<AsyncLogger> g_logger;


// Helper: pin current thread to core_id
static void pin_thread_to_core(int core_id) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_id, &set);
    pthread_t thr = pthread_self();
    int rc = pthread_setaffinity_np(thr, sizeof(cpu_set_t), &set);
    if (rc != 0) {
        std::cerr << "[WARN] pthread_setaffinity_np failed: " << rc << std::endl;
    }
}

void camera_thread_func(const std::string cam_dev, const std::string cam_name,
                        int target_size = 416, float conf_thresh = 0.35f,
                        int core_id = -1, int cap_w = 640, int cap_h = 480)
{
    try {
        if (!g_logger) g_logger = std::make_unique<AsyncLogger>("detections", 4096);

        YoloV8 yolo;
        yolo.load(target_size); // leave this as your original loader

        // If your YoloV8 wrapper exposes a method to limit NCNN threads, call it here:
        // e.g. yolo.setNumThreads(2);
        // If you don't have such a method, add it inside your YoloV8 wrapper (recommended).
        // Using fewer threads per model avoids thrashing when running multiple instances.
        //
        // Example (uncomment if your wrapper supports it):
        // yolo.setNumThreads(2);

        // Pin thread to core if requested (do early)
        if (core_id >= 0) pin_thread_to_core(core_id);

        // Setup VideoCapture
        cv::VideoCapture cap(cam_dev, cv::CAP_V4L2);

        // Force MJPEG to minimize USB bandwidth and CPU copies (if camera supports it)
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
        cap.set(cv::CAP_PROP_FRAME_WIDTH, cap_w);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, cap_h);
        cap.set(cv::CAP_PROP_FPS, 30);

        if (!cap.isOpened()) {
            std::cerr << "[ERR] Cannot open " << cam_dev << std::endl;
            return;
        }

        std::cout << "[INFO] Camera " << cam_name << " (" << cam_dev << ") started on core "
                  << (core_id >= 0 ? std::to_string(core_id) : std::string("auto")) << "\n";

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
                // push to async logger (non-blocking)
                g_logger->push(cam_name, js, ts_ms);
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

        // thread exit -> logger will close files in destructor
    }
    catch (const std::exception& e) {
        std::cerr << "[EXC] " << cam_name << ": " << e.what() << std::endl;
    }
}

int main()
{
    // Camera mapping (your detected mapping)
    std::string cam0 = "/dev/video0";  // cam1
    std::string name0 = "cam1";

    std::string cam1 = "/dev/video2";  // cam2
    std::string name1 = "cam2";

    std::string cam2 = "/dev/video4";  // cam3
    std::string name2 = "cam3";

    // choose capture resolution: try 320x240 for best perf, or 640x480 for quality
    int cap_w = 320;
    int cap_h = 240;

    // set target size (yolo input) - keep 416 or 320 depending on model/tradeoff
    int target_size = 416;

    // per-camera confidence threshold
    float conf_thresh = 0.35f;

    // Optional: how many cores to allocate. Pi 5 has 4 or more logical cores.
    // Assign one core per camera to reduce contention. Example: cores 1,2,3
    int core0 = 1;
    int core1 = 2;
    int core2 = 3;

    stop_all = false;

    // Make threads and pass core ids + capture resolution
    std::thread t0(camera_thread_func, cam0, name0, target_size, conf_thresh, core0, cap_w, cap_h);
    std::thread t1(camera_thread_func, cam1, name1, target_size, conf_thresh, core1, cap_w, cap_h);
    std::thread t2(camera_thread_func, cam2, name2, target_size, conf_thresh, core2, cap_w, cap_h);

    std::cout << "Press Ctrl-C to stop\n";

    t0.join();
    t1.join();
    t2.join();

    stop_all = true;
    return 0;
}
