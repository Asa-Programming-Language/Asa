#pragma once

#include <string>
#include <utility>
#include <vector>

#include "console.h"
#include "parser.h"
#include "settings.h"
#include "tokenizer.h"

struct ASTNode;

// defer macro definition

template<typename F>
struct privDefer {
    F f;
    privDefer(F f)
        : f(f) {}
    ~privDefer() { f(); }
};

template<typename F>
privDefer<F> defer_func(F f)
{
    return privDefer<F>(f);
}

#define DEFER_1(x, y) x##y
#define DEFER_2(x, y) DEFER_1(x, y)
#define DEFER_3(x) DEFER_2(x, __COUNTER__)
#define defer(code) auto DEFER_3(_defer_) = defer_func([&]() { code; })


enum PrintStyle {
    Show_Self_And_Contents,  // Show all lines contained in the node
    Show_Endpoints,          // Show first line + ellipsis (if hidden) + last line
    Show_In_Context,         // Show node within its parent's line range
};

// message system

namespace messageSystem {

    enum BlockType {
        Default_Block,
        Parser_Block,
        Codegen_Block
    };
    enum ErrorMessageType {
        Default_Error = 1,

        Undefined_Symbol_Error,
        Undefined_Function_Error,
        Undefined_Variable_Error,
        Undefined_Member_Error,
        Redefined_Error,
        Invalid_Compiler_Directive_Arguments_Error,
        Variable_Declaration_Error,
        Incorrect_Number_Of_Function_Arguments_Error,
        Removed_Attribute_Error,
    };
    enum WarningMessageType {
        Default_Warning = 1,

        Deprecated_Attribute_Warning,
    };

    struct MessageBlockNode {
        MessageBlockNode* parentNode = nullptr;
        std::vector<MessageBlockNode*> childNodes = std::vector<MessageBlockNode*>();

        ASTNode* astNode = nullptr;
        std::vector<std::pair<ASTNode*, std::string>> attributeNodes;
        std::string messageString = "";
        std::string sourceFunction = "";
        std::string sourcePath = "";
        uint32_t sourceLine = 0;
        BlockType blockType = Default_Block;

        MessageBlockNode(ASTNode*& node, std::string messageString, const char* sourceFunction, uint32_t sourceLine, const char* sourcePath, BlockType blockType)
        {
            this->messageString = messageString;
            this->sourceFunction = std::string(sourceFunction);
            this->sourcePath = std::string(sourcePath);
            this->sourceLine = sourceLine;
            this->blockType = blockType;
            astNode = node;
            attributeNodes = std::vector<std::pair<ASTNode*, std::string>>();
            childNodes = std::vector<MessageBlockNode*>();
        }
    };

    struct UnderlinedSegment {
        ASTNode* focusedNode = nullptr;

        std::string underlineMessage = "";
        std::string underlineColor = console::cyanFGColor;
        std::string underlineChar = "^";

        // vector of other AST nodes which should be shown as context
        // (must be in the same file as the focusedNode, at least for the current system)
        std::vector<ASTNode*> contextNodes = {};

        // Extra fine-tuning values which can be set to show a number of lines above or below the focused Node.
        // Does not apply to the contextNodes
        uint8_t aboveContextLines = 0;
        uint8_t belowContextLines = 0;


        UnderlinedSegment() {}
        UnderlinedSegment(ASTNode*& focusedNode, std::string underlineMessage = "", std::string underlineColor = console::cyanFGColor, std::string underlineChar = "^", std::vector<ASTNode*> contextNodes = {})
        {
            this->focusedNode = focusedNode;

            this->underlineMessage = underlineMessage;
            this->underlineColor = underlineColor;
            this->underlineChar = underlineChar;

            this->contextNodes = contextNodes;
        }
    };

    struct Message {
        std::string messageString = "";
        std::vector<UnderlinedSegment> underlinedSegments = {};

        Message() {}
        Message(std::string messageString, std::vector<UnderlinedSegment> underlinedSegments = {})
        {
            this->messageString = messageString;
            this->underlinedSegments = underlinedSegments;
        }

        void addUnderline(UnderlinedSegment u)
        {
            underlinedSegments.push_back(u);
        }
    };

    extern MessageBlockNode* errorTree;
    extern MessageBlockNode* currentNode;

    extern bool suppressErrors;
    extern bool exitOnError;

    void clearNodes(MessageBlockNode* node = nullptr);
    void startBlock(ASTNode* node, std::string messageString, const char* sourceFunction, uint32_t sourceLine, const char* sourcePath, BlockType blocktype);
    void endBlock();
    void addAttribute(ASTNode*& node, std::string messageString = "");
    void addAttribute(std::string strVal, std::string messageString = "");
    void* error(std::string messageString, ErrorMessageType messageType = Default_Error);
    void* warning(std::string messageString, WarningMessageType messageType = Default_Warning);
    void printNode(ASTNode*& node);
    void printNodeSource(ASTNode*& node, std::string underlineColor, std::string underlineMessage, PrintStyle style = Show_Self_And_Contents, int contextLines = 0, std::string underlineChar = "^");
    //void printTokenMessage(ASTNode*& node, std::string messageString, std::string underlineColor = console::redFGColor);
    //void printTokenMessage(ASTNode*& node, std::string messageString, std::string underlineMessage = "here", const std::string& underlineColor = console::redFGColor, std::string underlineChar = "^", PrintStyle style = Show_Self_And_Contents, int contextLines = 0);
    void printTokenMessage(ASTNode*& node, std::string messageString, std::string underlineMessage = "here", const std::string& underlineColor = console::redFGColor, std::string underlineChar = "^", PrintStyle style = Show_Self_And_Contents, int contextLines = 0);
    //void printErrorMessage(ASTNode*& node, std::string messageString, std::string underlineMessage, std::string sourceFunction, uint32_t sourceLine, std::string sourcePath);

}  // namespace messageSystem
