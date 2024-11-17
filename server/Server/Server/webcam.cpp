//#include <opencv2/opencv.hpp>
//#include "syscalls.h"
//
//bool startWebcam(std::string& result) {
//    // Create a VideoCapture object to access the webcam
//    cv::VideoCapture cap(0); // 0 is the index of the webcam
//
//    // Check if the webcam is opened successfully
//    if (!cap.isOpened()) {
//        std::cerr << "Error: Could not access webcam." << std::endl;
//        return -1;
//    }
//
//    cv::Mat frame;
//    while (true) {
//        // Capture a frame from the webcam
//        cap >> frame;
//
//        // Display the frame in a window
//        cv::imshow("Webcam", frame);
//
//        // Break the loop if 'q' is pressed
//        if (cv::waitKey(1) == 'q') {
//            break;
//        }
//    }
//
//    // Release the VideoCapture object and close all windows
//    cap.release();
//    cv::destroyAllWindows();
//
//    return 0;
//}
//
//bool stopWebcam(std::string& result) {
//	// Close all windows
//	cv::destroyAllWindows();
//	result = "Webcam stopped successfully.\n";
//	return true;
//}
