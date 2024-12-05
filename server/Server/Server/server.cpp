/*
 * 1. Initialize WSA - WSAStartup()
 * 2. Create a socket - socket()
 * 3. Bind the socket - bind()
 * 4. Listen on the socket - listen()
 * 5. Accept a connection - accept(), connect()
 * 6. Send and receive data - recv(), send(), recvfrom(), sendto()
 * 7. Disconnect - closesocket()
 */

#include <iostream>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "syscalls.h"
#include <minwindef.h>
#include <ws2def.h>
#include <ws2ipdef.h>
#include <minwinbase.h>
#include <exception>
#include <iosfwd>
#include "const.h"

#pragma comment(lib, "Ws2_32.lib")

 // Request

static void handleRequest(std::string& request, std::string& response, std::string params, std::ifstream& file) {
	COMMANDS command = commandMap.find(request) != commandMap.end() ? commandMap.at(request) : INVALID;
	try {
		switch (command) {
		case LIST_PROCESS:
			listProcess(response);
			break;
		case START_PROCESS:
			startProcess(params, response);
			break;
		case STOP_PROCESS:
			stopProcess(params, response);
			break;
		case SHUTDOWN:
			shutdown(response);
			break;
		case RESTART:
			restart(response);
			break;
		case DELETE_FILE:
			deleteFile(params, response);
			break;
		case MOVE_FILE:
			moveFile(params, response);
			break;
		case COPY_FILE:
			copyFile(params, response);
			break;
		case CAPTURE_SCREENSHOT:
			captureScreenshot("screenshot.png", response, file);
			break;
		case START_WEBCAM:
			startWebcam(response);
			break;
		case STOP_WEBCAM:
			stopWebcam(response);
			break;
		case LIST_APPS:
			listApps(response);
			break;
		case START_APP:
			startApp(params, response);
			break;
		default:
			response = "Invalid command.";
			break;
		}
	}
	catch (std::exception& e) {
		response = "An error occurred: " + std::string(e.what()) + "\n";
	}
}

static void startServer()
{
	WSADATA wsaData;
	SOCKET ListenSocket = INVALID_SOCKET, ClientSocket = INVALID_SOCKET;
	struct addrinfo* result = NULL, hints;
	int iResult;
	char recvbuf[DEFAULT_BUFLEN];
	int recvbuflen = DEFAULT_BUFLEN;

	// Initialize Winsock
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0)
	{
		std::cerr << "WSAStartup failed with error: " << iResult << std::endl;
		return;
	}

	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	// Resolve the server address and port
	iResult = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result);
	if (iResult != 0)
	{
		std::cerr << "getaddrinfo failed with error: " << iResult << std::endl;
		WSACleanup();
		return;
	}

	// Create a socket for the server
	ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (ListenSocket == INVALID_SOCKET)
	{
		std::cerr << "Socket failed with error: " << WSAGetLastError() << std::endl;
		freeaddrinfo(result);
		WSACleanup();
		return;
	}

	// Bind the socket
	iResult = bind(ListenSocket, result->ai_addr, (int)result->ai_addrlen);
	if (iResult == SOCKET_ERROR)
	{
		std::cerr << "Bind failed with error: " << WSAGetLastError() << std::endl;
		freeaddrinfo(result);
		closesocket(ListenSocket);
		WSACleanup();
		return;
	}

	freeaddrinfo(result);

	// Listen on the socket
	iResult = listen(ListenSocket, SOMAXCONN);
	if (iResult == SOCKET_ERROR)
	{
		std::cerr << "Listen failed with error: " << WSAGetLastError() << std::endl;
		closesocket(ListenSocket);
		WSACleanup();
		return;
	}

	// Print out listening IP address and port
	struct sockaddr_in addr;
	int addrlen = sizeof(addr);
	getsockname(ListenSocket, (struct sockaddr*)&addr, &addrlen);

	char ipStr[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &addr.sin_addr, ipStr, sizeof(ipStr));
	std::string ipAddr(ipStr);
	if (ipAddr == "0.0.0.0")
		ipAddr = "localhost";
	std::cout << "Listening on " << ipAddr << ":" << ntohs(addr.sin_port) << std::endl;

	// Accept a client socket
	std::cout << "Waiting for client connection...\n";
	ClientSocket = accept(ListenSocket, NULL, NULL);
	if (ClientSocket == INVALID_SOCKET)
	{
		std::cerr << "Accept failed with error: " << WSAGetLastError() << std::endl;
		closesocket(ListenSocket);
		WSACleanup();
		return;
	}
	else
	{
		std::cout << "Connection established!" << std::endl;
	}

	closesocket(ListenSocket); // No longer need server socket

	//-------------------------------------------------------------
	// Change the code here
	// Chat loop

	std::string sendbuf;
	while (1)
	{
		// Receive message from client
		iResult = recv(ClientSocket, recvbuf, recvbuflen, 0);
		if (iResult > 0)
		{
			std::string request(recvbuf, iResult);
			std::cout << "Client: " << request << std::endl;
			std::string response = "";

			// Split the request into command and parameters
			std::size_t pos = request.find(' ');
			std::string command = request.substr(0, pos);
			std::string params = request.substr(pos + 1);
			std::ifstream file;
			handleRequest(command, response, params, file);

			// Send file if it's open
			if (file.is_open()) {
				// Send file size first
				//std::cout << "sth\n";
				file.seekg(0, std::ios::end);
				int fileSize = static_cast<int>(file.tellg());
				send(ClientSocket, (char*)&fileSize, sizeof(fileSize), 0);

				file.seekg(0, std::ios::beg);  // Rewind the file to start sending

				char buffer[DEFAULT_BUFLEN];
				
				int i = 0;
				while (file.read(buffer, DEFAULT_BUFLEN)) {
					
					iResult = send(ClientSocket, buffer, DEFAULT_BUFLEN, 0);
					if (iResult == SOCKET_ERROR) {
						std::cerr << "Send failed with error: " << WSAGetLastError() << std::endl;
						break;
					}
					//std::cout << i++ << ' ';
				}
				// Send any remaining bytes
				iResult = send(ClientSocket, buffer, file.gcount(), 0);
				file.close();
			}
			else {
				iResult = send(ClientSocket, response.c_str(), response.size(), 0);
			}

			// Error checking
			if (iResult == SOCKET_ERROR)
			{
				std::cerr << "Send failed with error: " << WSAGetLastError() << std::endl;
				break;
			}
		}
		else if (iResult == 0)
		{
			std::cout << "Connection closing...\n";
			break;
		}
		else
		{
			std::cerr << "Recv failed with error: " << WSAGetLastError() << std::endl;
			break;
		}
	}

	//--------------------------------------------------------------

	// Shutdown and cleanup
	closesocket(ClientSocket);
	WSACleanup();
}

int main()
{
	std::cout << "Server started." << std::endl;
	try {
		startServer();
	}
	catch (std::exception& e) {
		std::cerr << "An error occurred: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}