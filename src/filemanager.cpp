#include "filemanager.h"

std::string initialFileString;
std::string executableDirectory;
std::string projectDirectory;
std::string baseFileName;
std::string outputFileName = "";

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

int saveStringToFile(const std::string& fileName, std::string& s)
{
    try {
        std::ofstream fileStream(fileName);
        std::string str = "";
        if (fileStream.is_open() == false)
            return 1;
        fileStream << s;
        fileStream.close();
    }
    catch (std::exception& e) {
        return 1;
    }
    return 0;
}

int saveVectorToFile(const std::string& fileName, std::vector<std::string>& v)
{
    try {
        std::ofstream fileStream(fileName);
        std::string str = "";
        if (fileStream.is_open() == false)
            return 1;
        for (const auto& s : v)
            fileStream << s << "\n";
        fileStream.close();
    }
    catch (std::exception& e) {
        return 1;
    }
    return 0;
}

//std::vector<std::string> getFilesInDirectory(std::string directoryPath)
//{
//  std::vector<std::string> outVec = std::vector<std::string>();
//}

//int loadModule(const std::string& fileName, std::string& outStr)
//{
//  outStr = "";
//  std::string searchPath[2] = {projectDirectory + fileName, executableDirectory + "modules/" + fileName};
//  if (directoryExists(searchPath[0]))
//      loadFile(searchPath[0], outStr);
//  else if (directoryExists(searchPath[1]))
//      loadFile(searchPath[1], outStr);
//  else
//      return 1;
//
//  return 0;
//}

bool directoryExists(std::string& path)
{
    return std::filesystem::exists(path) && std::filesystem::is_directory(path);
}

std::string truncatePath(const std::string& path)
{
    // Case 1: path lives under a "modules" directory at any depth
    std::string modulesMarker = "/modules/";
    size_t pos = path.find(modulesMarker);
    if (pos != std::string::npos)
        return path.substr(pos + 1);  // drop everything before "modules/"

    if (path.size() >= 8 && path.substr(0, 8) == "modules/")
        return path;

    // Case 2: path lives inside the project directory
    if (!projectDirectory.empty()) {
        std::string projDir = projectDirectory;
        if (projDir.back() != '/')
            projDir += '/';

        if (path.size() >= projDir.size() && path.substr(0, projDir.size()) == projDir)
            return "./" + path.substr(projDir.size());
    }

    // Case 3: fallback — return full path unchanged
    return path;
}
