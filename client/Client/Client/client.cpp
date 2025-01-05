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
#include <conio.h>
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

void menu() {

	int option = 1;

	bool quit = false;

	while (!quit) {
		system("cls");
		std::cout << "\n\n";
		std::cout << "\t\t\t\t   \033[93m /$$$$$$  /$$ /$$                       /$$    \033[0m" << '\n';
		std::cout << "\t\t\t\t   \033[93m /$$__  $$| $$|__/                      | $$    \033[0m" << '\n';
		std::cout << "\t\t\t\t   \033[93m| $$  \\__/| $$ /$$  /$$$$$$  /$$$$$$$  /$$$$$$  \033[0m" << '\n';
		std::cout << "\t\t\t\t   \033[93m| $$      | $$| $$ /$$__  $$| $$__  $$|_  $$_/  \033[0m" << '\n';
		std::cout << "\t\t\t\t   \033[93m| $$      | $$| $$| $$$$$$$$| $$  \\ $$  | $$    \033[0m" << '\n';
		std::cout << "\t\t\t\t   \033[93m| $$    $$| $$| $$| $$_____/| $$  | $$  | $$ /$$\033[0m" << '\n';
		std::cout << "\t\t\t\t   \033[93m|  $$$$$$/| $$| $$|  $$$$$$$| $$  | $$  |  $$$$/\033[0m" << '\n';
		std::cout << "\t\t\t\t   \033[93m \\______/ |__/|__/ \\_______/|__/  |__/   \\___/  \033[0m" << '\n';
		std::cout << "\n\n\n";



		std::cout << "\t\t\t\t\t ----------====****====----------\n\n";
		if (option == 1)
			std::cout << "\t\t\t\t\t\t \x1B[33m-*   START   *-\33[0m\n\n";
		else
			std::cout << "\t\t\t\t\t\t      START\n\n";

		if (option == 2)
			std::cout << "\t\t\t\t\t\t \x1B[33m-* ADD GMAIL *-\33[0m\n\n";
		else
			std::cout << "\t\t\t\t\t\t    ADD GMAIL\n\n";

		if (option == 3)
			std::cout << "\t\t\t\t\t\t \x1B[33m-*INSTRUCTION*-\33[0m\n\n";
		else
			std::cout << "\t\t\t\t\t\t   INSTRUCTION\n\n";
		std::cout << "\t\t\t\t\t ----------====****====----------\n";


		//get option
		char key = _getch();

		if (key == 'w' && option > 1) {
			option--;
		}
		else if (key == 's' && option < 3) {
			option++;
		}
		else if (key == ' ') {
			switch (option) {
			case 1:
				system("cls");
				startClient();
				break;
			case 2:
				system("cls");
				std::cout << "\n\n";
				std::cout << "\t\t\t\t\033[92mAdd Admin Gmail: \033[0m\n";
				std::cout << "\t\t\t\tEnter the new admin Gmail: ";
				{
					std::string newAdminGmail;
					std::cin >> newAdminGmail;
					std::fstream adminGmailList("admin.txt", std::ios::app);
					adminGmailList << '\n' << newAdminGmail << '\n';
					std::cout << "\t\t\t\tAdmin Gmail added successfully.\n";
					adminGmailList.close();
					char x = _getch();
				}
				break;
			case 3:
				system("cls");
				std::cout << "\n\n";
				std::cout << "\t\t\t\t\033[92m[How to use] \033[0m\n";
				std::cout << "\t\t\t\tUse one of the admin accounts to compose an email\n\t\t\t\twith the content being one of the commands below\n\t\t\t\tand send it to the system mail\n\n";
				std::cout << "\t\t\t\t\033[92m[Admin Gmail] \033[0m\n";
				std::fstream adminGmailList("admin.txt", std::ios::in);

				std::string buffer;
				while (getline(adminGmailList, buffer)) {
					if (buffer.empty())
						continue;

					else std::cout << "\t\t\t\t" << buffer << '\n';
				}
				std::cout << "\n";

				std::cout << "\t\t\t\t\033[92m[System Gmail] \033[0m\n";
				std::cout << "\t\t\t\tjakeva123kl@gmail.com\n";
				std::cout << "\n";
				std::cout << "\t\t\t\t\033[92m[Command - Discription] \033[0m\n";
				std::cout << "\t\t\t\tlist - LIST_PROCESS\n";
				std::cout << "\t\t\t\tstart - START_PROCESS\n";
				std::cout << "\t\t\t\tstop - STOP_PROCESS\n";
				std::cout << "\t\t\t\tshutdown - SHUTDOWN\n";
				std::cout << "\t\t\t\trestart - RESTART\n";
				std::cout << "\t\t\t\tls - LIST_FILES\n";
				std::cout << "\t\t\t\tdelete - DELETE_FILE\n";
				std::cout << "\t\t\t\tmove - MOVE_FILE\n";
				std::cout << "\t\t\t\tcopy - COPY_FILE\n";
				std::cout << "\t\t\t\tscreenshot - CAPTURE_SCREENSHOT\n";
				std::cout << "\t\t\t\tstart_webcam - START_WEBCAM\n";
				std::cout << "\t\t\t\tstop_webcam - STOP_WEBCAM\n";
				std::cout << "\t\t\t\tlist_app - LIST_APPS\n";
				std::cout << "\t\t\t\tstart_app - START_APP\n";
				std::cout << "\t\t\t\tstop_app - STOP_APP\n";
				std::cout << "\t\t\t\tstart_keylogger - START_KEYLOGGER\n";
				std::cout << "\t\t\t\tstop_keylogger - STOP_KEYLOGGER\n";
				std::cout << "\t\t\t\tlock_keyboard - LOCK_KEYBOARD\n";
				std::cout << "\t\t\t\tunlock_keyboard - UNLOCK_KEYBOARD\n";
				char x = _getch();
				break;
			}
		}
	}

	system("cls");
}
int main()
{
	menu();
	return 0;
}