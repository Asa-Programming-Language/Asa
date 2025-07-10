#pragma once

#include <iostream>
#include <string>

#define ERROR(s) std::cerr << "Error: " << s << std::endl;
void printError(std::string s, int lineNumber = 0, std::string fileName = "");
