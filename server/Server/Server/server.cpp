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
#include <thread>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")
#define DEFAULT_PORT "12345"
#define DEFAULT_BUFLEN 512

void startServer()
{
    WSADATA wsaData;
    SOCKET ListenSocket = INVALID_SOCKET, ClientSocket = INVALID_SOCKET;
    struct addrinfo *result = NULL, hints;
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

    closesocket(ListenSocket); // No longer need server socket

    //-------------------------------------------------------------
    // Change the code here
    // Chat loop

    std::vector<std::thread> clientThreads;
    std::string sendbuf;
    while (1)
    {
        // Receive message from client
        iResult = recv(ClientSocket, recvbuf, recvbuflen, 0);
        if (iResult > 0)
        {
            recvbuf[iResult] = '\0'; // Null-terminate the received string
            std::cout << "Client: " << recvbuf << std::endl;

            // Get server's reply
            std::cout << "Server: ";
            std::getline(std::cin, sendbuf);
            iResult = send(ClientSocket, sendbuf.c_str(), sendbuf.size(), 0);
            if (iResult == SOCKET_ERROR)
            {
                std::cerr << "Send failed with error: " << WSAGetLastError() << std::endl;
                break;
            }

            // clientThreads.emplace_back()
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
    startServer();
    return 0;
}