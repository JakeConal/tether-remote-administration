#include "syscalls.h"

bool shutdown(std::string& result) {
	std::string command = "shutdown /s /f /t 15";
	int ret = system(command.c_str());
	if (ret == 0) {
		result = "Shutdown command executed successfully.\n";
		return true;
	}
	else {
		result = "Failed to execute shutdown command.\n";
		return false;
	}
}

bool restart(std::string& result) {
	std::string command = "shutdown /r /f /t 15";
	int ret = system(command.c_str());
	if (ret == 0) {
		result = "Restart command executed successfully.\n";
		return true;
	}
	else {
		result = "Failed to execute restart command.\n";
		return false;
	}
}