#include "filemanager.h"

std::string initialFileString;

int loadFile(const std::string& fileName)
{
	initialFileString = "";
	try {
		std::ifstream fileStream(fileName);
		std::string str = "";
		if (fileStream.is_open() == false)
			return 1;
		while (std::getline(fileStream, str))
			initialFileString += str + "\n";
		fileStream.close();
	}
	catch (std::exception& e) {
		return 1;
	}

	return 0;
}
