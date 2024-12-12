#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <fstream>
#include <iostream>

#define DEFAULT_PORT "12345"
#define DEFAULT_BUFLEN 1024

#pragma comment(lib, "Ws2_32.lib")

void printProgressBar(int progress, int total);

void startClient();

bool initializeWinsock(WSADATA& wsaData);
std::string getServerAddressInput();
addrinfo* getServerAddress(const std::string& serverAddress, const std::string& port, const addrinfo& hints);
bool connectToServer(SOCKET& ConnectSocket, addrinfo* result);	
bool sendClientRequest(SOCKET& ConnectSocket, const std::string& request);
std::string receiveResponseType(SOCKET& ConnectSocket, char* recvbuf, int recvbuflen);
void handleFileResponse(SOCKET& ConnectSocket, char* recvbuf, int recvbuflen, std::string sender);
void handleTextResponse(SOCKET& ConnectSocket, char* recvbuf, int recvbuflen, std::string sender);
void chatLoop(SOCKET& ConnectSocket, char* recvbuf, int recvbuflen);
void cleanup(SOCKET& ConnectSocket);