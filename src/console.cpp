#include "console.h"

void printError(std::string s, int lineNumber, std::string fileName)
{
	std::cerr << "Error";
	if (fileName.size() > 0)
		std::cerr << " from file: \"" << fileName << "\"";
	if (lineNumber > 0)
		std::cerr << " On line: " << lineNumber;
	std::cerr << ":" << std::endl;

	std::cerr << s << std::endl;
}
