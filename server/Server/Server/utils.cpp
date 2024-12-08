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
