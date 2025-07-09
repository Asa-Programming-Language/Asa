#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

extern std::string initialFileString;
extern std::string executableDirectory;
extern std::string projectDirectory;

int loadFile(const std::string& fileName, std::string& outStr);
//int loadModule(const std::string& fileName, std::string& outStr);
bool directoryExists(std::string& path);
