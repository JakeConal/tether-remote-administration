#include "syscalls.h"
#include <opencv2/opencv.hpp>

std::atomic<bool> keepRunning(false);

void displayWebcam() {
	// Create a VideoCapture object to access the webcam
	cv::VideoCapture cap(0); // 0 is the index of the webcam

	// Check if the webcam is opened successfully
	if (!cap.isOpened()) {
		std::cerr << "Error: Could not access webcam." << std::endl;
		return;
	}

	cv::Mat frame;

	while (keepRunning.load()) {
		// Capture a frame from the webcam
		cap >> frame;
		cv::imshow("Webcam", frame);
		cv::waitKey(1);
	}

	cv::destroyAllWindows();

	return;
}

bool startWebcam(std::string& result)
{
	if (keepRunning) {
		result = "Webcam is already running.";
		return false;
	}

	keepRunning = true;

	std::thread webcamThread(displayWebcam);
	webcamThread.detach();
	result = "Webcam started successfully.";
	return true;
}

bool stopWebcam(std::string& result) {
	if (!keepRunning) {
		result = "Webcam is not running.";
		return false;
	}
	// Close all windows
	keepRunning = false;
	result = "Webcam stopped successfully.";
	return true;
}

