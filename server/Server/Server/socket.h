#pragma once

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <string>
#include <iostream>
#include <fstream>

#pragma comment(lib, "Ws2_32.lib")

/*
 * 1. Initialize WSA - WSAStartup()
 * 2. Create a socket - socket()
 * 3. Bind the socket - bind()
 * 4. Listen on the socket - listen()
 * 5. Accept a connection - accept(), connect()
 * 6. Send and receive data - recv(), send(), recvfrom(), sendto()
 * 7. Disconnect - closesocket()
 */

bool initializeWinsock(WSADATA& wsaData);
bool setupSocket(SOCKET& ListenSocket, addrinfo*& result);
void printListeningInfo(SOCKET& ListenSocket);
SOCKET acceptClient(SOCKET& ListenSocket);

void processRequest(SOCKET& ClientSocket, std::string& request);
void sendFile(SOCKET& ClientSocket, std::ifstream& file, std::string command);
void sendResponse(SOCKET& ClientSocket, std::string& response);
void handleClient(SOCKET& ClientSocket);

void cleanup(SOCKET& ClientSocket);


static void startServer();