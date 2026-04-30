#pragma once
#include "pch.h"

extern std::string initialFileString;
extern std::string executableDirectory;
extern std::string projectDirectory;
extern std::string baseFileName;
extern std::string outputFileName;

int loadFile(const std::string& fileName, std::string& outStr);
int saveStringToFile(const std::string& fileName, std::string& s);
int saveVectorToFile(const std::string& fileName, std::vector<std::string>& v);
//int loadModule(const std::string& fileName, std::string& outStr);
bool directoryExists(std::string& path);
std::string truncatePath(const std::string& path);
