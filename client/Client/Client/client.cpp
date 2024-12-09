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

int main()
{
	startClient();
	return 0;
}