#include "socket.h"
#include "gmail.h"
#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <fstream>
#include <vector>

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

/*------------------------------------------------------------------------------------------------------------

	1. Initialize WSA - WSAStartup()
	2. Create a socket - socket()
	3. Connect to the server - connect()
	4. Send and receive data - recv(), send(), recvfrom(), sendto()
	5. Disconnect - closesocket()

------------------------------------------------------------------------------------------------------------*/

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
	int iResult = recv(ConnectSocket, recvbuf, recvbuflen, 0);
	if (iResult > 0)
	{
		return std::string(recvbuf, iResult);
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
	char* fileNameBuffer = new char[recvbuflen];
	int iResult;
	iResult = recv(ConnectSocket, fileNameBuffer, recvbuflen, 0);
	if (iResult == SOCKET_ERROR) {
		std::cerr << "Recv failed with error: " << WSAGetLastError() << std::endl;
		return "";
	}

	std::string fileName(fileNameBuffer, iResult);
	std::cout << "Receiving file: " << fileName << std::endl;

	// Receive file size
	unsigned long long fileSize;
	iResult = recv(ConnectSocket, (char*)&fileSize, sizeof(fileSize), 0);
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
	char* fileBuffer = new char[fileSize];
	unsigned long long bytesReceived = 0;
	unsigned long long totalReceived = 0;

	while (totalReceived < fileSize) {
		bytesReceived = recv(ConnectSocket, fileBuffer + totalReceived, fileSize - totalReceived, 0);
		if (bytesReceived == SOCKET_ERROR) {
			std::cerr << "Recv failed with error: " << WSAGetLastError() << std::endl;
			delete[] fileBuffer;
			break;
		}
		totalReceived += bytesReceived;
		// Print progress bar
		printProgressBar(totalReceived, fileSize);
	}
	std::cout << std::endl;

	// Save the received data to a file
	std::ofstream outFile(fileName, std::ios::binary);
	outFile.write(fileBuffer, fileSize);
	outFile.close();

	std::cout << "File received successfully!" << std::endl;

	return fileName;

	delete[] fileBuffer;
	delete[] fileNameBuffer;
}

std::string handleTextResponse(SOCKET& ConnectSocket, char* recvbuf, int recvbuflen) {
	int responseSize;
	int iResult;
	iResult = recv(ConnectSocket, (char*)&responseSize, sizeof(responseSize), 0);
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

		std::cout << "Server: " << std::string(responseBuffer, totalReceived) << std::endl;

		return std::string(responseBuffer, totalReceived);
	}
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
					//request.erase(request.find('\n'));
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
			std::string respone = handleFileResponse(ConnectSocket, recvbuf, recvbuflen);
			sendGmailWithAttachment(accessToken, senderMail, "Command Result", "", respone);
		}
		else if (responseType == "text")
		{
			std::string respone = handleTextResponse(ConnectSocket, recvbuf, recvbuflen);
			sendEmail(accessToken, senderMail, "Command Result", respone);
		}
		else
		{
			std::cerr << "Unknown response type: " << responseType << std::endl;
		}
	}
}

void cleanup(SOCKET& ConnectSocket)
{
	closesocket(ConnectSocket);
	WSACleanup();
}