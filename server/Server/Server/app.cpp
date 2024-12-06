#include "syscalls.h"

static void TraverseDirectory(const std::wstring& directory, std::vector<std::wstring>& fileList) {
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
				fileList.push_back(fileOrDir);
			}
		}
	} while (FindNextFile(hFind, &findFileData) != 0);

	FindClose(hFind);
}

// Resolve .lnk file to the actual executable path
static std::wstring ResolveShortcut(const std::wstring& shortcutPath) {
	IShellLink* psl;
	IPersistFile* ppf;
	WCHAR szGotPath[MAX_PATH];
	WIN32_FIND_DATA wfd;
	HRESULT hres;
	CoInitialize(NULL);
	hres = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLink, (LPVOID*)&psl);
	if (SUCCEEDED(hres)) {
		hres = psl->QueryInterface(IID_IPersistFile, (LPVOID*)&ppf);
		if (SUCCEEDED(hres)) {
			hres = ppf->Load(shortcutPath.c_str(), STGM_READ);
			if (SUCCEEDED(hres)) {
				hres = psl->GetPath(szGotPath, MAX_PATH, &wfd, SLGP_UNCPRIORITY);
				if (SUCCEEDED(hres)) {
					ppf->Release();
					psl->Release();
					CoUninitialize();
					return szGotPath;
				}
			}
		}
	}
	psl->Release();
	CoUninitialize();
	return L"";
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
		std::wstring name = app.substr(app.find_last_of(L"\\") + 1, app.find(L".lnk") - app.find_last_of(L"\\") - 1).c_str();
		result += std::string(name.begin(), name.end()) + '\n';
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

bool stopApp(std::string name, std::string& result) {
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
			// Get app name
			std::wstring appName = app.substr(app.find_last_of(L"\\") + 1, app.find(L".lnk") - app.find_last_of(L"\\") - 1).c_str();
			// Get app path
			std::wstring appPath = ResolveShortcut(app);
			// Get app process name including .exe
			std::wstring processName = appPath.substr(appPath.find_last_of(L"\\") + 1, appPath.size());
			std::string processNameStr(processName.begin(), processName.end());
			// Kill process
			stopProcess(processNameStr, result);
			return true;
		}
	}
	result = "App not found.";
	return false;
}