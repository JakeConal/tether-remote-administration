#include "syscalls.h"
#include <windows.h>

bool deleteFile(std::string& fileName, std::string& result) {
    // Convert std::string to std::wstring
    std::wstring wFileName(fileName.begin(), fileName.end());
    LPCWSTR file = wFileName.c_str();
    if (DeleteFile(file)) {
        result = "File deleted successfully.\n";
        return true;
    }
    else {
        result = "Failed to delete file.\n";
        return false;
    }
}

bool moveFile(std::string& filenames, std::string& result) {
	std::string from, to;
	std::size_t pos = filenames.find('\n');
	from = filenames.substr(0, pos);
	to = filenames.substr(pos + 1);
	// Convert std::string to std::wstring
	std::wstring wFrom(from.begin(), from.end());
	std::wstring wTo(to.begin(), to.end());
	LPCWSTR src = wFrom.c_str();
	LPCWSTR dest = wTo.c_str();
	if (MoveFile(src, dest)) {
		result = "File moved successfully.\n";
		return true;
	}
	else {
		result = "Failed to move file.\n";
		return false;
	}
}

bool copyFile(std::string& filenames, std::string& result) {
	std::string from, to;
	std::size_t pos = filenames.find('\n');
	from = filenames.substr(0, pos);
	to = filenames.substr(pos + 1);
	// Convert std::string to std::wstring
	std::wstring wFrom(from.begin(), from.end());
	std::wstring wTo(to.begin(), to.end());
	LPCWSTR src = wFrom.c_str();
	LPCWSTR dest = wTo.c_str();
	if (CopyFile(src, dest, FALSE)) {
		result = "File copied successfully.\n";
		return true;
	}
	else {
		result = "Failed to copy file.\n";
		return false;
	}
}