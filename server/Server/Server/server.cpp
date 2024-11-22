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
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>
 //#include <nlohmann/json.hpp>
 //#include <cpr/cpr.h>
#include <vector>
#include <memory>
#include "syscalls.h"

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
			captureScreenshot(params, response, file);
			break;
		case START_WEBCAM:
			startWebcam(response);
			break;
		case STOP_WEBCAM:
			stopWebcam(response);
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

			// Split the request into command and parameters separated by newline
			std::size_t pos = request.find(' ');
			std::string command = request.substr(0, pos);
			std::string params = request.substr(pos + 1);
			std::ifstream file;
			handleRequest(command, response, params, file);

			// Get server's reply
			//std::cout << "Server: ";
			//std::getline(std::cin, sendbuf);

			// Send the reply to the client
			if (file.is_open()) {
				char buffer[DEFAULT_BUFLEN];
				while (file.read(buffer, DEFAULT_BUFLEN)) {
					iResult = send(ClientSocket, buffer, DEFAULT_BUFLEN, 0);
					if (iResult == SOCKET_ERROR) {
						std::cerr << "Send failed with error: " << WSAGetLastError() << std::endl;
						break;
					}
				}
				iResult = send(ClientSocket, buffer, file.gcount(), 0);
				iResult = send(ClientSocket, response.c_str(), response.size(), 0);
				file.close();
			}
			else {
				iResult = send(ClientSocket, response.c_str(), response.size(), 0);
			}

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