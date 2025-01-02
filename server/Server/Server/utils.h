#pragma once

#include <iostream>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>	
#include <vector>
#include <windows.h>
#include <tchar.h>

bool isElevated();
bool restartAsAdmin();
std::string exec(const char* cmd);
std::vector<std::string> findAllLines(const std::string& str, const std::string& find);