
#if defined(__unix__)
    #define UNIX true
    #define WINDOWS false
#elif defined(_MSC_VER)
    #define UNIX false
    #define WINDOWS true
#endif

#define MULTITHREADED_SAFE false


#include "console.h"

#include <iostream>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#if WINDOWS
    #include <windows.h>

    #include "include/color.hpp"
#else
    #include <unistd.h>
#endif


namespace console {


    bool useColor = true;
    uint8_t indentation = 0;


    bool consoleSupportsColor()
    {
        if (compilerFlags & Flags_Force_Enable_Color)
            return true;
#if defined(_WIN32)
        // Windows 10 and later support ANSI escape sequences via ENABLE_VIRTUAL_TERMINAL_PROCESSING
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut == INVALID_HANDLE_VALUE)
            return false;

        DWORD dwMode = 0;
        if (!GetConsoleMode(hOut, &dwMode))
            return false;

        // Try to enable virtual terminal processing if not already enabled
        if ((dwMode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
            if (!SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                return false;
            }
        }
        return true;
#else
        // On Unix, check if stdout is a tty
        if (!isatty(fileno(stdout)))
            return false;
        const char* term = getenv("TERM");
        if (!term)
            return false;

        std::string termStr(term);
        // Common terminal types that support colors
        static const char* colorTerms[] = {
            "xterm", "color", "ansi", "cygwin", "linux"};
        for (const auto& colorTerm : colorTerms) {
            if (termStr.find(colorTerm) != std::string::npos)
                return true;
        }
        return false;
#endif
    }

    std::string colorText(std::string name, std::string color)
    {
        return color + name + resetColor;
    }
    std::string colorText(std::string name, std::string fgColor, std::string bgColor)
    {
        return fgColor + bgColor + name + resetColor;
    }

    void printError(std::string s, int lineNumber, std::string fileName)
    {
        write("Error", redFGColor);
        if (fileName.size() > 0)
            write(std::string(" from file: \"") + fileName + "\"");
        if (lineNumber > 0)
            write(std::string(" on line: \"") + std::to_string(lineNumber) + "\"");
        write(":  ");

        write(s + "\n");
    }

    void printWarning(std::string s, int lineNumber, std::string fileName)
    {
        write("Warning", yellowFGColor);
        if (fileName.size() > 0)
            write(std::string(" from file: \"") + fileName + "\"");
        if (lineNumber > 0)
            write(std::string(" on line: \"") + std::to_string(lineNumber) + "\"");
        write(":  ");

        write(s + "\n");
    }


    void setColor(std::string fgColor)
    {
#if WINDOWS
        auto fg = dye::white(text);
        if (fgColor == blackFGColor)
            fg = dye::black(text);
        else if (fgColor == redFGColor)
            fg = dye::red(text);
        else if (fgColor == greenFGColor)
            fg = dye::green(text);
        else if (fgColor == yellowFGColor)
            fg = dye::yellow(text);
        else if (fgColor == blueFGColor)
            fg = dye::blue(text);
        else if (fgColor == magentaFGColor)
            fg = dye::purple(text);
        else if (fgColor == cyanFGColor)
            fg = dye::aqua(text);
        else if (fgColor == whiteFGColor)
            fg = dye::white(text);
        if (useColor)
            std::cout << fg;
#else
        if (useColor)
            std::cout << fgColor;
#endif
    }

    void resetColors()
    {
#if WINDOWS
        auto fg = dye::white(text);
        if (useColor)
            std::cout << fg;
#else
        if (useColor)
            std::cout << resetColor;
#endif
    }


    void printColored(std::string text, std::string fgColor, std::string bgColor)
    {
#if WINDOWS
        auto fg = dye::white(text);
        if (fgColor == blackFGColor)
            fg = dye::black(text);
        else if (fgColor == redFGColor)
            fg = dye::red(text);
        else if (fgColor == greenFGColor)
            fg = dye::green(text);
        else if (fgColor == yellowFGColor)
            fg = dye::yellow(text);
        else if (fgColor == blueFGColor)
            fg = dye::blue(text);
        else if (fgColor == magentaFGColor)
            fg = dye::purple(text);
        else if (fgColor == cyanFGColor)
            fg = dye::aqua(text);
        else if (fgColor == whiteFGColor)
            fg = dye::white(text);
        if (useColor)
            std::cout << fg;
#else
        if (useColor)
            std::cout << fgColor + bgColor + text + resetColor;
        else
            std::cout << text;
#endif
    }

    void writeLine()
    {
        //printIndent(indentation);
        std::cout << std::endl;
    }
    void writeLine(std::string message)
    {
        //printIndent(indentation);
        std::cout << message << std::endl;
    }
    void writeLine(std::string message, std::string fgColor, std::string bgColor)
    {
        //printIndent(indentation);
        printColored(message, fgColor, bgColor);
        writeLine();
    }

    void write()
    {
    }
    void write(std::string message)
    {
        std::cout << message;
    }
    void write(std::string message, std::string color)
    {
        printColored(message, color, "");
    }
    void write(std::string message, std::string fgColor, std::string bgColor)
    {
        //printIndent(indentation);
        printColored(message, fgColor, bgColor);
    }
    void applyIndent()
    {
        printIndent(indentation);
    }
    void printIndent(int depth)
    {
        for (int i = 0; i < depth; i++)
            write("    ");
    }
    void writeIndented(std::string message, std::string fgColor, std::string bgColor, int indents)
    {
        std::string ind = "";
        for (size_t i = 0; i < indents; i++)
            ind += "    ";
        printColored(ind + "  " + message, fgColor, bgColor);
    }
    void writeLineIndented(std::string message, std::string fgColor, std::string bgColor, int indents)
    {
        std::string ind = "";
        for (size_t i = 0; i < indents; i++)
            ind += "    ";
        printColored(ind + "  " + message + "\n", fgColor, bgColor);
    }
    void writeBulleted(std::string message, std::string fgColor, std::string bgColor, int indents, std::string bullet)
    {
        std::string ind = "";
        for (size_t i = 0; i < indentation + indents; i++)
            ind += "    ";
        printColored(ind + bullet + " " + message, fgColor, bgColor);
    }
    void writeBulleted(std::string message, std::string fgColor, std::string bgColor, int indents)
    {
        std::string ind = "";
        for (size_t i = 0; i < indentation + indents; i++)
            ind += "    ";
        printColored(ind + "- " + message, fgColor, bgColor);
    }
    void writeBulleted(std::string message, int indents, std::string bullet)
    {
        std::string ind = "";
        for (size_t i = 0; i < indentation + indents; i++)
            ind += "    ";
        printColored(ind + bullet + " " + message, "", "");
    }
    void writeBulleted(std::string message, int indents)
    {
        std::string ind = "";
        for (size_t i = 0; i < indentation + indents; i++)
            ind += "    ";
        printColored(ind + "- " + message, "", "");
    }
    void writeLineCharArrayOfLen(char* message, int len)
    {
        for (size_t i = 0; i < len; i++)
            std::cout << message[i];

        std::cout << std::endl;
    }
    //void writeTable(std::vector<std::string>& headers, std::vector<std::vector<colorstr>>& items, int maxWidths[], int indentation, bool autoExpansion)
    //{

    //  // If any of the data values are too large, expand the width of the column
    //  if (autoExpansion)
    //      for (int i = 0; i < items.size(); i++)
    //          for (int k = 0; k < items[i].size(); k++)
    //              maxWidths[k] = std::max((int)(items[i][k].value.size()), maxWidths[k]);

    //  // Also size the verticalSeparator based on this, taking into
    //  // account the header lengths as well
    //  std::string verticalSeparator = "";
    //  for (int i = 0; i < headers.size(); i++) {
    //      maxWidths[i] = std::max(maxWidths[i], (int)(headers[i].length()));
    //      verticalSeparator += "+";
    //      for (int j = 0; j < maxWidths[i] + 2; j++)
    //          verticalSeparator += "-";
    //      if (i == headers.size() - 1)
    //          verticalSeparator += "+";
    //  }

    //  // Print header:
    //  writeLineIndented(verticalSeparator, "", "", indentation);
    //  writeIndented("", "", "", indentation);
    //  for (int i = 0; i < headers.size(); i++) {
    //      write("| ");
    //      std::string totalHeader = headers[i];
    //      for (int j = headers[i].length(); j < maxWidths[i]; j++) {
    //          totalHeader += " ";
    //      }
    //      write(totalHeader + " ", cyanFGColor);
    //      if (i == headers.size() - 1)
    //          write("|");
    //  }
    //  writeLine();
    //  writeLineIndented(verticalSeparator, "", "", indentation);

    //  // Print items:
    //  for (int i = 0; i < items.size(); i++) {
    //      writeIndented("", "", "", indentation);
    //      for (int k = 0; k < items[i].size(); k++) {
    //          write("|");
    //          std::string totalItem = PadString(items[i][k].value + " ", ' ', maxWidths[k] + 2);
    //          write(totalItem, items[i][k].color);

    //          if (k == items[i].size() - 1)
    //              write("|");
    //      }
    //      writeLine();
    //      writeLineIndented(verticalSeparator, "", "", indentation);
    //  }
    //}
    //void writeDialogueAuthor(std::string coloredType)
    //{
    //  printColored(coloredType, "", "");
    //}

    std::string readLine()
    {
        std::string s;
        std::getline(std::cin, s);
        return s;
    }

    void exitError(std::string errMessage)
    {
        writeLine();
        writeLine(errMessage, redFGColor);
        std::cout << "Press Enter to Exit";
        std::cin.ignore();
        exit(1);
    }

}  // namespace console
