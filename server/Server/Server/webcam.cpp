#include <opencv2/opencv.hpp>
#include "syscalls.h"

std::atomic<bool> keepRunning(true);

void displayWebcam() {
    // Create a VideoCapture object to access the webcam
    cv::VideoCapture cap(0); // 0 is the index of the webcam

    // Check if the webcam is opened successfully
    if (!cap.isOpened()) {
        std::cerr << "Error: Could not access webcam." << std::endl;
        return -1;
    }

    cv::Mat frame;
    while (keepRunning.load()) {
        // Capture a frame from the webcam
        cap >> frame;

        // Display the frame in a window
        cv::imshow("Webcam", frame);
    }

    return 0;
}

bool startWebcam(std::string& result)
{
    std::thread webcamThread(displayWebcam);
    webcamThread.detach();
    result = "Webcam started successfully.";
    return true;
}

bool stopWebcam(std::string& result) {
	// Close all windows
    keepRunning = false;
	result = "Webcam stopped successfully.\n";
	return true;
}
