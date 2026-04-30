#ifndef console_h
#define console_h

#include "indicators.hpp"
#include "pch.h"
#include "settings.h"
#include "strops.h"


namespace console {
    // Declare the global print queue
    extern std::queue<std::ostream> printQueue;

    // Foreground colors
    const std::string blackFGColor = "\x1B[30m";
    const std::string redFGColor = "\x1B[31m";
    const std::string greenFGColor = "\x1B[32m";
    const std::string yellowFGColor = "\x1B[33m";
    const std::string blueFGColor = "\x1B[34m";
    const std::string magentaFGColor = "\x1B[35m";
    const std::string cyanFGColor = "\x1B[36m";
    const std::string whiteFGColor = "\x1B[37m";
    const std::string brightBlackFGColor = "\x1B[90m";
    const std::string brightRedFGColor = "\x1B[91m";
    const std::string brightGreenFGColor = "\x1B[92m";
    const std::string brightYellowFGColor = "\x1B[93m";
    const std::string brightBlueFGColor = "\x1B[94m";
    const std::string brightMagentaFGColor = "\x1B[95m";
    const std::string brightCyanFGColor = "\x1B[96m";
    const std::string brightWhiteFGColor = "\x1B[97m";

    //Background colors
    const std::string blackBGColor = "\x1B[40m";
    const std::string redBGColor = "\x1B[41m";
    const std::string greenBGColor = "\x1B[42m";
    const std::string yellowBGColor = "\x1B[43m";
    const std::string blueBGColor = "\x1B[44m";
    const std::string magentaBGColor = "\x1B[45m";
    const std::string cyanBGColor = "\x1B[46m";
    const std::string whiteBGColor = "\x1B[47m";
    const std::string brightBlackBGColor = "\x1B[100m";
    const std::string brightRedBGColor = "\x1B[101m";
    const std::string brightGreenBGColor = "\x1B[102m";
    const std::string brightYellowBGColor = "\x1B[103m";
    const std::string brightBlueBGColor = "\x1B[104m";
    const std::string brightMagentaBGColor = "\x1B[105m";
    const std::string brightCyanBGColor = "\x1B[106m";
    const std::string brightWhiteBGColor = "\x1B[107m";

    // Reset colors
    const std::string resetColor = "\033[0m";

    struct colorstr {
        std::string value = "";
        std::string color = "";
    };

    extern bool useColor;
    extern uint8_t indentation;

    bool consoleSupportsColor();

    void printError(std::string s, int lineNumber = 0, std::string fileName = "");
    void printWarning(std::string s, int lineNumber = 0, std::string fileName = "");
    std::string colorText(std::string name, std::string color);
    std::string colorText(std::string name, std::string fgColor, std::string bgColor);

    void setColor(std::string fgColor);
    void resetColors();
    void printColored(std::string text, std::string fgColor = "", std::string bgColor = "");

    void writeLine();
    void writeLine(std::string message);
    void writeLine(std::string message, std::string fgColor, std::string bgColor = "");

    void write();
    void write(std::string message);
    void write(std::string message, std::string color);
    void write(std::string message, std::string fgColor, std::string bgColor);
    void applyIndent();
    void printIndent(int depth);
    void writeDialogueAuthor(std::string coloredType);
    void writeIndented(std::string message, std::string fgColor, std::string bgColor, int indents);
    void writeLineIndented(std::string message, std::string fgColor, std::string bgColor, int indents);
    void writeBulleted(std::string message, std::string fgColor, std::string bgColor, int indents, std::string bullet);
    void writeBulleted(std::string message, std::string fgColor, std::string bgColor, int indents);
    void writeBulleted(std::string message, int indents, std::string bullet);
    void writeBulleted(std::string message, int indents);

    void writeLineCharArrayOfLen(char* message, int len);
    //void writeTable(std::vector<std::string>& headers, std::vector<std::vector<colorstr>>& items, int maxWidths[], int indentation = 0, bool autoExpansion = true);
    //void writeTable(std::vector<std::string>& headers, std::vector<std::vector<std::string>>& items, int maxWidths[], std::vector<std::vector<std::string>>& itemColors);

    std::string readLine();

    void exitError(std::string errMessage);


    //Console();
}  // namespace console


#endif
