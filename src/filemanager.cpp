#include "filemanager.h"

std::string initialFileString;
std::string executableDirectory;
std::string projectDirectory;

int loadFile(const std::string& fileName, std::string& outStr)
{
	outStr = "";
	try {
		std::ifstream fileStream(fileName);
		std::string str = "";
		if (fileStream.is_open() == false)
			return 1;
		while (std::getline(fileStream, str))
			outStr += str + "\n";
		fileStream.close();
	}
	catch (std::exception& e) {
		return 1;
	}

	return 0;
}

//std::vector<std::string> getFilesInDirectory(std::string directoryPath)
//{
//	std::vector<std::string> outVec = std::vector<std::string>();
//}

//int loadModule(const std::string& fileName, std::string& outStr)
//{
//	outStr = "";
//	std::string searchPath[2] = {projectDirectory + fileName, executableDirectory + "modules/" + fileName};
//	if (directoryExists(searchPath[0]))
//		loadFile(searchPath[0], outStr);
//	else if (directoryExists(searchPath[1]))
//		loadFile(searchPath[1], outStr);
//	else
//		return 1;
//
//	return 0;
//}

bool directoryExists(std::string& path)
{
	return std::filesystem::exists(path) && std::filesystem::is_directory(path);
}
