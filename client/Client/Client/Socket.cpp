#include "socket.h"
#include "gmail.h"
#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <fstream>
#include <vector>
#include <algorithm>	
#include <thread>
#include <chrono>

#pragma comment(lib, "Ws2_32.lib")

#define DEFAULT_PORT "12345"
#define DEFAULT_BUFLEN 1024

void printProgressBar(int progress, int total)
{
	float percentage = (float)progress / total;
	int barWidth = 70;
	std::cout << "[";
	int pos = barWidth * percentage;
	for (int i = 0; i < barWidth; ++i) {
		if (i < pos) std::cout << "=";
		else if (i == pos) std::cout << ">";
		else std::cout << " ";
	}
	std::cout << "] " << int(percentage * 100.0) << " %\r";
	std::cout.flush();
}

bool initializeWinsock(WSADATA& wsaData) {
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0)
	{
		std::cerr << "WSAStartup failed with error: " << iResult << std::endl;
		return false;
	}
	return true;
}

std::string getServerAddressInput()
{
	std::string serverAddress;
	std::cout << "Enter server IP address: ";
	std::getline(std::cin, serverAddress);
	return serverAddress;
}

addrinfo* getServerAddress(const std::string& serverAddress, const std::string& port, const addrinfo& hints)
{
	addrinfo* result = NULL;
	int iResult = getaddrinfo(serverAddress.c_str(), port.c_str(), &hints, &result);
	if (iResult != 0)
	{
		std::cerr << "getaddrinfo failed with error: " << iResult << std::endl;
		return NULL;
	}
	return result;
}

bool connectToServer(SOCKET& ConnectSocket, addrinfo* result)
{
	addrinfo* ptr = NULL;
	for (ptr = result; ptr != NULL; ptr = ptr->ai_next)
	{
		ConnectSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
		if (ConnectSocket == INVALID_SOCKET)
		{
			std::cerr << "Socket failed with error: " << WSAGetLastError() << std::endl;
			continue;
		}

		if (connect(ConnectSocket, ptr->ai_addr, (int)ptr->ai_addrlen) == SOCKET_ERROR)
		{
			closesocket(ConnectSocket);
			ConnectSocket = INVALID_SOCKET;
			continue;
		}
		break;
	}

	freeaddrinfo(result);
	return ConnectSocket != INVALID_SOCKET;
}

bool sendClientRequest(SOCKET& ConnectSocket, const std::string& request)
{
	int iResult = send(ConnectSocket, request.c_str(), request.size(), 0);
	if (iResult == SOCKET_ERROR)
	{
		std::cerr << "Send failed with error: " << WSAGetLastError() << std::endl;
		return false;
	}
	return true;
}

std::string receiveResponseType(SOCKET& ConnectSocket, char* recvbuf, int recvbuflen)
{
	int iResult = 0;
	int responseTypeSize = 0;
	iResult = recv(ConnectSocket, reinterpret_cast<char*>(&responseTypeSize), sizeof(responseTypeSize), 0);
	iResult = recv(ConnectSocket, recvbuf, responseTypeSize, 0);
	if (iResult > 0)
	{
		return std::string(recvbuf, responseTypeSize);
	}
	else if (iResult == 0)
	{
		std::cout << "Connection closed by server.\n";
	}
	else
	{
		std::cerr << "Recv failed with error: " << WSAGetLastError() << std::endl;
	}
	return "";
}

std::string handleFileResponse(SOCKET& ConnectSocket, char* recvbuf, int recvbuflen)
{
	// Receive file name	
	int fileNameLength = 0;
	recv(ConnectSocket, (char*)&fileNameLength, sizeof(fileNameLength), 0);

	char fileNameBuffer[DEFAULT_BUFLEN]; // Allocate extra space for null-terminator
	int iResult = recv(ConnectSocket, fileNameBuffer, fileNameLength, 0);
	if (iResult == SOCKET_ERROR) {
		std::cerr << "Recv failed with error: " << WSAGetLastError() << std::endl;
		return "";
	}

	std::string fileName(fileNameBuffer, fileNameLength);

	std::cout << "Receiving file: " << fileName << std::endl;

	// Receive file size
	unsigned long long fileSize;
	iResult = recv(ConnectSocket, reinterpret_cast<char*>(&fileSize), sizeof(fileSize), 0);
	if (iResult == SOCKET_ERROR) {
		std::cerr << "Recv failed with error: " << WSAGetLastError() << std::endl;
		return "";
	}

	std::cout << "Expecting a file of size: ";
	if (fileSize > 1024 * 1024) {
		std::cout << fileSize / (1024 * 1024) << " MB" << std::endl;
	}
	else if (fileSize > 1024) {
		std::cout << fileSize / 1024 << " KB" << std::endl;
	}
	else {
		std::cout << fileSize << " bytes" << std::endl;
	}

	// Allocate buffer for receiving the file
	unsigned long long totalReceived = 0;
	std::wstring wFileName(fileName.begin(), fileName.end());
	HANDLE hFile = CreateFile(wFileName.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		std::cerr << "CreateFile failed with error: " << GetLastError() << std::endl;
		return "";
	}

	while (totalReceived < fileSize) {
		// Use TransmitFile to receive the file
		WSABUF wsabuf;
		char buffer[DEFAULT_BUFLEN];
		wsabuf.buf = buffer;
		wsabuf.len = DEFAULT_BUFLEN;

		DWORD flags = 0;
		DWORD bytesReceived = 0;
		iResult = WSARecv(ConnectSocket, &wsabuf, 1, &bytesReceived, &flags, NULL, NULL);
		if (iResult == SOCKET_ERROR) {
			std::cerr << "WSARecv failed with error: " << WSAGetLastError() << std::endl;
			CloseHandle(hFile);
			return "";
		}

		DWORD bytesWritten = 0;
		if (!WriteFile(hFile, buffer, bytesReceived, &bytesWritten, NULL)) {
			std::cerr << "WriteFile failed with error: " << GetLastError() << std::endl;
			CloseHandle(hFile);
			return "";
		}

		totalReceived += bytesReceived;
		// Print progress bar
		printProgressBar(totalReceived, fileSize);
	}
	std::cout << std::endl;
	CloseHandle(hFile);

	std::cout << "File received successfully!" << std::endl;

	return fileName;
}

std::string handleTextResponse(SOCKET& ConnectSocket, char* recvbuf, int recvbuflen) {
	int responseSize;
	int iResult = recv(ConnectSocket, (char*)&responseSize, sizeof(responseSize), 0);
	if (iResult > 0)
	{
		// Allocate buffer for receiving the response
		char* responseBuffer = new char[responseSize];
		int bytesReceived = 0;
		int totalReceived = 0;

		while (totalReceived < responseSize)
		{
			bytesReceived = recv(ConnectSocket, responseBuffer + totalReceived, responseSize - totalReceived, 0);
			if (bytesReceived == SOCKET_ERROR)
			{
				std::cerr << "Recv failed with error: " << WSAGetLastError() << std::endl;
				delete[] responseBuffer;
				break;
			}
			totalReceived += bytesReceived;
		}

		std::string response(responseBuffer, totalReceived);
		delete[] responseBuffer;
		std::cout << "Server: " << response << std::endl;

		return response;
	}
	return "";
}

void chatLoop(SOCKET& ConnectSocket, char* recvbuf, int recvbuflen)
{
	std::string request;
	std::string accessToken = getAccessToken();
	std::vector<std::string> adminMail = { "hieunguyen.jc@gmail.com", "tiendat1243oo@gmail.com", "phamnamanh25@gmail.com" };
	std::string messageId = getMessageId(accessToken);
	std::string processedId = messageId;
	std::string senderMail;

	while (true)
	{
		if (!accessToken.empty()) {
			while (true) {
				messageId = getMessageId(accessToken);

				if (messageId != processedId && isAdmin(accessToken, adminMail, messageId, senderMail)) {
					processedId = messageId;
					request = getEmail(accessToken, messageId);
					request.pop_back();
					request.pop_back();
					break;
				}
			}
		}

		if (!sendClientRequest(ConnectSocket, request))
			break;

		if (request == "exit")
		{
			std::cout << "Client is exiting..." << std::endl;
			break;
		}

		std::string responseType = receiveResponseType(ConnectSocket, recvbuf, recvbuflen);
		if (responseType == "file")
		{
			std::string response = handleFileResponse(ConnectSocket, recvbuf, recvbuflen);
			sendGmailWithAttachment(accessToken, senderMail, "Command Result", "", response);
		}
		else if (responseType == "text")
		{
			std::string response = handleTextResponse(ConnectSocket, recvbuf, recvbuflen);
			sendEmail(accessToken, senderMail, "Command Result", response);
		}
		else
		{
			std::cerr << "Unknown response type: " << responseType << std::endl;
		}
		std::this_thread::sleep_for(std::chrono::seconds(10));
	}
}

void cleanup(SOCKET& ConnectSocket)
{
	closesocket(ConnectSocket);
	WSACleanup();
}
