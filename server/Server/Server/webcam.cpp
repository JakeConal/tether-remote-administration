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

	// Define the codec and create a VideoWriter object
	int frame_width = static_cast<int>(cap.get(CV_CAP_PROP_FRAME_WIDTH));
	int frame_height = static_cast<int>(cap.get(CV_CAP_PROP_FRAME_HEIGHT));
	cv::VideoWriter video(WEBCAM_PATH, CV_FOURCC('a', 'v', 'c', '1'), 15, cv::Size(frame_width, frame_height));

	// Check if the VideoWriter object is initialized successfully
	if (!video.isOpened()) {
		std::cerr << "Error: Could not create video file." << std::endl;
		return;
	}

	cv::Mat frame;

	while (keepRunning.load()) {
		// Capture a frame from the webcam
		cap >> frame;

		// If the frame is empty, break immediately
		if (frame.empty())
			break;

		// Display the frame
		cv::imshow("Webcam", frame);
		cv::waitKey(1);

		cv::flip(frame, frame, 0);

		// Write the frame to the video file
		video.write(frame);
	}

	// Release the video writer and destroy all OpenCV windows
	video.release();
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

bool stopWebcam(std::string& result, std::ifstream& output) {
	if (!keepRunning) {
		result = "Webcam is not running.";
		return false;
	}
	// Close all windows
	keepRunning = false;
	result = "Webcam stopped successfully.";

	// Open the video file
	output.open(WEBCAM_PATH, std::ios::binary);
	return true;
}

