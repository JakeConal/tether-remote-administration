#include <chrono>
#include <cpr/cpr.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "socket.h"

void startClient()
{
    WSADATA wsaData;
    SOCKET ConnectSocket = INVALID_SOCKET;
    addrinfo hints, * result = NULL;
    char recvbuf[DEFAULT_BUFLEN];
    int recvbuflen = DEFAULT_BUFLEN;

    if (!initializeWinsock(wsaData))
        return;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    while (ConnectSocket == INVALID_SOCKET)
    {
        std::string serverAddress;
        std::cout << "Enter server IP address: ";
        std::getline(std::cin, serverAddress);

        result = getServerAddress(serverAddress, DEFAULT_PORT, hints);
        if (result == NULL)
        {
            std::cerr << "Failed to resolve server address. Try again." << std::endl;
            continue;
        }

		if (!connectToServer(ConnectSocket, result))
        {
            std::cerr << "Unable to connect to server. Try again." << std::endl;
        }
        else
        {
            std::cout << "Connected to server!" << std::endl;
        }
    }

    chatLoop(ConnectSocket, recvbuf, recvbuflen);

    cleanup(ConnectSocket);
}

int main()
{
	startClient();
	return 0;
}