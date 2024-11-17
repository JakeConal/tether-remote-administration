#pragma once

#include <iostream>
#include <map>
#include <string>

constexpr auto DEFAULT_PORT = "12345";
constexpr auto DEFAULT_BUFLEN = 1024;

enum COMMANDS {
	LIST_PROCESS,
	START_PROCESS,
	STOP_PROCESS,
	SHUTDOWN,
	RESTART,
	DELETE_FILE,
	MOVE_FILE,
	COPY_FILE,
	CAPTURE_SCREENSHOT
};

const std::map<std::string, COMMANDS> commandMap = {
	{"list", LIST_PROCESS},
	{"start", START_PROCESS},
	{"stop", STOP_PROCESS},
	{"shutdown", SHUTDOWN},
	{"restart", RESTART},
	{"delete", DELETE_FILE},
	{"move", MOVE_FILE},
	{"copy", COPY_FILE},
	{"screenshot", CAPTURE_SCREENSHOT}
};