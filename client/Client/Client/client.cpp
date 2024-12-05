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

#pragma comment(lib, "Ws2_32.lib")

#define DEFAULT_PORT "12345"
#define DEFAULT_BUFLEN 1024

/*------------------------------------------------------------------------------------------------------------

   GMail API

 ------------------------------------------------------------------------------------------------------------*/

using json = nlohmann::json;

std::string base64_encode(const std::string& input)
{
	static const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string encoded;
	int val = 0, valb = -6;
	for (unsigned char c : input)
	{
		val = (val << 8) + c;
		valb += 8;
		while (valb >= 0)
		{
			encoded.push_back(base64_chars[(val >> valb) & 0x3F]);
			valb -= 6;
		}
	}
	if (valb > -6)
		encoded.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
	while (encoded.size() % 4)
		encoded.push_back('=');
	return encoded;
}

std::string base64_decode(const std::string& input)
{
	static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::vector<int> T(256, -1);
	for (int i = 0; i < 64; i++)
		T[base64_chars[i]] = i;

	std::string decoded;
	int val = 0, valb = -8;
	for (unsigned char c : input)
	{
		if (T[c] == -1)
			break;
		val = (val << 6) + T[c];
		valb += 6;
		if (valb >= 0)
		{
			decoded.push_back(char((val >> valb) & 0xFF));
			valb -= 8;
		}
	}
	return decoded;
}

std::string getAccessToken(const std::string& refreshToken, const std::string& clientId, const std::string& clientSecret)
{
	cpr::Response r = cpr::Post(cpr::Url{ "https://oauth2.googleapis.com/token" },
		cpr::Payload{ {"client_id", clientId},
					 {"client_secret", clientSecret},
					 {"refresh_token", refreshToken},
					 {"grant_type", "refresh_token"} });
	if (r.status_code != 200)
	{
		std::cerr << "Failed to get access token: " << r.status_code << std::endl;
		return "";
	}

	auto jsonData = json::parse(r.text);
	return jsonData["access_token"].get<std::string>();
}

void sendEmail(const std::string& accessToken, const std::string& to, const std::string& subject, const std::string& body)
{
	std::string email = "From: jakeva123kl@gmail.com\r\nTo: " + to + "\r\nSubject: " + subject + "\r\n\r\n" + body;
	std::string rawMessage = "{\"raw\": \"" + base64_encode(email) + "\"}";

	cpr::Response r = cpr::Post(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages/send" },
		cpr::Header{ {"Authorization", "Bearer " + accessToken},
					{"Content-Type", "application/json"} },
		cpr::Body{ rawMessage });
	if (r.status_code != 200)
	{
		std::cerr << "Failed to send email: " << r.status_code << std::endl;
	}
	else
	{
		std::cout << "Email sent successfully" << std::endl;
	}
}

std::string getEmail(const std::string& accessToken, const std::string& messageId)
{
	cpr::Response r = cpr::Get(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages/" + messageId },
		cpr::Header{ {"Authorization", "Bearer " + accessToken} });
	if (r.status_code != 200)
	{
		std::cerr << "Failed to get email: " << r.status_code << std::endl;
		return "";
	}
	else
	{
		auto jsonData = json::parse(r.text);
		std::string encodedBody = jsonData["payload"]["parts"][0]["body"]["data"].get<std::string>();
		return base64_decode(encodedBody);
	}
}

std::string listEmails(const std::string& accessToken)
{
	cpr::Response r = cpr::Get(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages" },
		cpr::Header{ {"Authorization", "Bearer " + accessToken} });
	if (r.status_code != 200)
	{
		std::cerr << "Failed to list emails: " << r.status_code << std::endl;
		return "";
	}
	else
	{
		return r.text;
	}
}

std::string findEmailFromSender(const std::string& accessToken, const std::string& senderEmail, std::unordered_set<std::string>& processedMessageIds)
{
	std::string emailList = listEmails(accessToken);
	auto jsonData = json::parse(emailList);

	for (const auto& message : jsonData["messages"])
	{
		std::string messageId = message["id"].get<std::string>();
		if (processedMessageIds.find(messageId) != processedMessageIds.end())
		{
			continue; // Skip already processed emails
		}

		cpr::Response r = cpr::Get(cpr::Url{ "https://www.googleapis.com/gmail/v1/users/me/messages/" + messageId },
			cpr::Header{ {"Authorization", "Bearer " + accessToken} });
		if (r.status_code == 200)
		{
			auto emailData = json::parse(r.text);
			for (const auto& header : emailData["payload"]["headers"])
			{
				if (header["name"] == "From" && header["value"].get<std::string>().find(senderEmail) != std::string::npos)
				{
					processedMessageIds.insert(messageId); // Mark email as processed
					return messageId;
				}
			}
		}
	}
	return "";
}

/*------------------------------------------------------------------------------------------------------------

	1. Initialize WSA - WSAStartup()
	2. Create a socket - socket()
	3. Connect to the server - connect()
	4. Send and receive data - recv(), send(), recvfrom(), sendto()
	5. Disconnect - closesocket()

------------------------------------------------------------------------------------------------------------*/

void startClient()
{
	WSADATA wsaData;
	SOCKET ConnectSocket = INVALID_SOCKET;
	struct addrinfo* result = NULL, * ptr = NULL, hints;
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
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	while (ConnectSocket == INVALID_SOCKET)
	{
		// Get server address from user input
		std::string addr;
		std::cout << "Enter server IP address: ";
		std::getline(std::cin, addr);
		const char* serverAddress = addr.c_str();

		try {
			// Resolve the server address and port
			iResult = getaddrinfo(serverAddress, DEFAULT_PORT, &hints, &result);
			if (iResult != 0)
			{
				std::cerr << "getaddrinfo failed with error: " << iResult << std::endl;
				WSACleanup();
				return;
			}

			// Attempt to connect to the first result
			for (ptr = result; ptr != NULL; ptr = ptr->ai_next)
			{
				// Create a socket for connecting to server
				ConnectSocket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
				if (ConnectSocket == INVALID_SOCKET)
				{
					std::cerr << "Socket failed with error: " << WSAGetLastError() << std::endl;
					WSACleanup();
					return;
				}

				// Connect to server
				iResult = connect(ConnectSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
				if (iResult == SOCKET_ERROR)
				{
					closesocket(ConnectSocket);
					ConnectSocket = INVALID_SOCKET;
					continue;
				}
				break;
			}
		}
		catch (const std::exception& e) {
			std::cerr << e.what() << std::endl;
		}

		freeaddrinfo(result);

		if (ConnectSocket == INVALID_SOCKET)
		{
			std::cerr << "Unable to connect to server!" << std::endl;
		}
		else
		{
			std::cout << "Connected to server!" << std::endl;
		}
	}

	//------------------------------------
	// Change the code here
	// Chat loop
	std::string request;
	while (1)
	{
		// Get client's message
		std::cout << "Client: ";
		std::getline(std::cin, request);

		iResult = send(ConnectSocket, request.c_str(), request.size(), 0);
		if (iResult == SOCKET_ERROR)
		{
			std::cerr << "Send failed with error: " << WSAGetLastError() << std::endl;
			break;
		}

		// Receive response from server
		std::ofstream outFile;
		outFile.open("receive.png", std::ios::binary);
		
		if (request == "screenshot") {
			// Receive file size first
			int fileSize;
			iResult = recv(ConnectSocket, (char*)&fileSize, sizeof(fileSize), 0);
			if (iResult == SOCKET_ERROR) {
				std::cerr << "Recv failed with error: " << WSAGetLastError() << std::endl;
				break;
			}

			std::cout << "Expecting a file of size: " << fileSize << " bytes." << std::endl;

			// Allocate buffer for receiving the file
			char* fileBuffer = new char[fileSize];
			int bytesReceived = 0;
			int totalReceived = 0;

			while (totalReceived < fileSize) {
				bytesReceived = recv(ConnectSocket, fileBuffer + totalReceived, fileSize - totalReceived, 0);
				if (bytesReceived == SOCKET_ERROR) {
					std::cerr << "Recv failed with error: " << WSAGetLastError() << std::endl;
					delete[] fileBuffer;
					break;
				}
				totalReceived += bytesReceived;
			}

			// Save the received data to a file
			std::ofstream outFile("receive.png", std::ios::binary);
			outFile.write(fileBuffer, totalReceived);
			outFile.close();

			std::cout << "File received successfully!" << std::endl;

			delete[] fileBuffer;
		}
		else {
			iResult = recv(ConnectSocket, recvbuf, recvbuflen, 0);
			if (iResult > 0)
			{
				std::string res(recvbuf, iResult);
				std::cout << "Server: " << res << std::endl;
			}
			else if (iResult == 0)
			{
				std::cout << "Connection closed by server.\n";
				break;
			}
			else
			{
				std::cerr << "Recv failed with error: " << WSAGetLastError() << std::endl;
				break;
			}
		}
		
		outFile.close();
	}
	//--------------------------------------

	// Cleanup
	closesocket(ConnectSocket);
	WSACleanup();
}
void send_request(const std::string& server, const std::string& request, std::string& response)
{
	// Socket programming
}

int main()
{
	/*
	std::string clientId = "";
	std::string clientSecret = "";
	std::string refreshToken = "";
	std::string accessToken = getAccessToken(refreshToken, clientId, clientSecret);

	if (!accessToken.empty())
	{
		std::string senderEmail = "hieunguyen.jc@gmail.com";
		std::unordered_set<std::string> processedMessageIds;

		while (true)
		{
			std::string messageId = findEmailFromSender(accessToken, senderEmail, processedMessageIds);

			if (!messageId.empty())
			{
				std::string emailContent = getEmail(accessToken, messageId);

				std::istringstream iss(emailContent);
				std::string command, filename;
				iss >> command >> filename;

				std::string request = command + " " + filename;
				std::string response;
				send_request("127.0.0.1", request, response);

				sendEmail(accessToken, "hieunguyen.jc@gmail.com", "Command Result", response);
			}
			else
			{
				std::cout << "No new email from " << senderEmail << std::endl;
			}

			std::this_thread::sleep_for(std::chrono::minutes(1)); // Delay checking mail
		}
	}
	else
	{
		std::cerr << "Failed to get access token" << std::endl;
	}*/

	startClient();
	return 0;
}