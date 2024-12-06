#include "syscalls.h"

bool listProcess(std::string& result) {
	std::string command = "tasklist /FO CSV";
	std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(command.c_str(), "r"), _pclose);
	if (!pipe) {
		std::cerr << "Failed to run tasklist command." << std::endl;
		result = "Failed to retrieve application list.\n";
		return false;
	}

	char buffer[DEFAULT_BUFLEN];

	while (fgets(buffer, DEFAULT_BUFLEN, pipe.get()) != NULL) {
		std::string line(buffer);

		std::size_t firstComma = line.find(',');
		if (firstComma != std::string::npos) {
			std::string processName = line.substr(1, firstComma - 2);
			result += processName + "\n";
		}
	}

	return true;
}

bool startProcess(std::string& processName, std::string& result) {
	std::string command = "start " + processName;
	int ret = system(command.c_str());
	if (ret == 0) {
		result = "Process started successfully.";
		return true;
	}
	else {
		result = "Failed to start process.";
		return false;
	}
}

bool stopProcess(std::string& processName, std::string& result) {
	std::string command = "taskkill /IM " + processName + " /F";
	int ret = system(command.c_str());
	if (ret == 0) {
		result = "Process stopped successfully.";
		return true;
	}
	else if (ret == 1) {
		result = "Failed to stop process. Reason: Access denied.";
		return false;
	}
	else if (ret == 128) {
		result = "Failed to stop process. Reason: Process not found.";
		return false;
	}
	else {
		result = "Failed to stop process.";
		return false;
	}
}