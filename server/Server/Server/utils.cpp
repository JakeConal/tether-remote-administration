#include "utils.h"

bool isElevated() {
	HANDLE hToken = GetCurrentProcessToken();
	if (!hToken) return false;

	DWORD dwSize = 0;
	GetTokenInformation(hToken, TokenElevation, NULL, 0, &dwSize);

	BOOL isElevated = false;
	TOKEN_ELEVATION elevation;

	if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) {
		isElevated = elevation.TokenIsElevated;
	}

	CloseHandle(hToken);

	return isElevated;
}

bool restartAsAdmin() {
	TCHAR szPath[MAX_PATH];
	if (!GetModuleFileName(NULL, szPath, MAX_PATH)) {
		std::cerr << "Failed to get module file name" << std::endl;
		return false;
	}

	SHELLEXECUTEINFO sei = { sizeof(sei) };
	sei.lpVerb = _T("runas");
	sei.lpFile = szPath;
	sei.hwnd = NULL;
	sei.nShow = SW_SHOWNORMAL;

	if (!ShellExecuteEx(&sei)) {
		std::cerr << "Failed to restart as administrator" << std::endl;
		return false;
	}

	return true;
}

std::string exec(const char* cmd) {
	std::array<char, 128> buffer;
	std::string result;
	std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd, "r"), _pclose);
	if (!pipe) {
		throw std::runtime_error("popen() failed!");
	}
	while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
		result += buffer.data();
	}
	return result;
}

std::vector<std::string> findAllLines(const std::string& str, const std::string& find) {
	std::vector<std::string> lines;
	size_t pos = 0;
	while ((pos = str.find(find, pos)) != std::string::npos) {
		size_t end = str.find("\n", pos);
		lines.push_back(str.substr(pos, end - pos));
		pos = end;
	}
	return lines;
}
