#include "syscalls.h"

void TraverseDirectory(const std::wstring& directory, std::vector<std::wstring>& fileList) {
	WIN32_FIND_DATA findFileData;
	HANDLE hFind = FindFirstFile((directory + L"\\*").c_str(), &findFileData);
	if (hFind == INVALID_HANDLE_VALUE) {
		return;
	}

	do {
		const std::wstring fileOrDir = directory + L"\\" + findFileData.cFileName;
		if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (wcscmp(findFileData.cFileName, L".") != 0 && wcscmp(findFileData.cFileName, L"..") != 0) {
				TraverseDirectory(fileOrDir, fileList);
			}
		}
		else {
			// Get only .lnk files, and push only the app name
			if (fileOrDir.find(L".lnk") != std::wstring::npos) {
				fileList.push_back(fileOrDir.substr(fileOrDir.find_last_of(L"\\") + 1, fileOrDir.find(L".lnk") - fileOrDir.find_last_of(L"\\") - 1));
			}
		}
	} while (FindNextFile(hFind, &findFileData) != 0);

	FindClose(hFind);
}

bool listApps(std::string& result) {
	PWSTR startMenuPath;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_StartMenu, 0, NULL, &startMenuPath))) {
		return false;
	}

	std::wstring directory(startMenuPath);
	CoTaskMemFree(startMenuPath);

	WIN32_FIND_DATA findFileData;
	HANDLE hFind = FindFirstFile((directory + L"\\*").c_str(), &findFileData);
	if (hFind == INVALID_HANDLE_VALUE) {
		return false;
	}

	std::vector<std::wstring> appList;
	TraverseDirectory(directory, appList);

	FindClose(hFind);

	for (const auto& app : appList) {
		result += std::string(app.begin(), app.end()) + "\n";
	}

	return true;
}

bool startApp(std::string name, std::string& result) {
	PWSTR startMenuPath;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_StartMenu, 0, NULL, &startMenuPath))) {
		return false;
	}
	std::wstring directory(startMenuPath);
	CoTaskMemFree(startMenuPath);
	WIN32_FIND_DATA findFileData;
	HANDLE hFind = FindFirstFile((directory + L"\\*").c_str(), &findFileData);
	if (hFind == INVALID_HANDLE_VALUE) {
		return false;
	}
	std::vector<std::wstring> appList;

	TraverseDirectory(directory, appList);

	FindClose(hFind);
	for (const auto& app : appList) {
		if (app.find(std::wstring(name.begin(), name.end())) != std::wstring::npos) {
			ShellExecute(NULL, L"open", app.c_str(), NULL, NULL, SW_SHOW);
			result = "App started.";
			return true;
		}
	}
	result = "App not found.";
	return false;
}
