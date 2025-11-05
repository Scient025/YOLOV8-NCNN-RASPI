#include "yoloV8.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <chrono>

int main(int argc, char** argv)
{
    YoloV8 yolo;
    yolo.load(640);  // target input size

    std::string cam_path = (argc > 1) ? argv[1] : "/dev/video0";
    cv::VideoCapture cap(cam_path, cv::CAP_V4L2);
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    cap.set(cv::CAP_PROP_FPS, 30);

    if (!cap.isOpened()) {
        std::cerr << "❌ Cannot open camera: " << cam_path << std::endl;
        return -1;
    }

    std::cout << "📷 Camera opened: " << cam_path << std::endl;
    cv::Mat frame;
    while (true)
    {
        cap >> frame;
        if (frame.empty()) continue;

        auto start = std::chrono::steady_clock::now();

        std::vector<Object> objects;
        yolo.detect(frame, objects, 0.35f, 0.45f);  // conf, nms

        // optional: only show humans
        std::vector<Object> persons;
        for (auto& obj : objects)
            if (obj.label == 0)
                persons.push_back(obj);

        yolo.draw(frame, persons);

        auto end = std::chrono::steady_clock::now();
        double fps = 1000.0 / std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        cv::putText(frame, cv::format("FPS: %.1f", fps), cv::Point(20, 40),
                    cv::FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(255, 0, 0), 2);

        cv::imshow("YOLOv8 NCNN Camera", frame);
        if (cv::waitKey(1) == 27) break;  // ESC
    }

    return 0;
}
