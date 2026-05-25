#pragma once

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "codegen.h"
#include "console.h"
#include "parser.h"
#include "settings.h"
#include "tokenizer.h"

struct ASTNode;
struct argType;
//typedef std::vector<argType> argumentList;
struct functionID;

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


// message system

namespace messageSystem {

    enum BlockType {
        Default_Block,
        Parser_Block,
        Codegen_Block
    };
    enum ErrorMessageType {
        // General errors:
        Default_Error = 1,
        Syntax_Error,

        // Undefined errors:
        Undefined_Symbol_Error,
        Undefined_Function_Error,
        Undefined_Function_Exact_Error,
        Undefined_Variable_Error,
        Undefined_Member_Error,
        // Redefined errors:
        Redefined_Error,
        // Compiler directive errors:
        Invalid_Compiler_Directive_Arguments_Error,
        // Declaration errors:
        Variable_Declaration_Error,
        Type_Inference_From_Void_Error,
        // Function errors:
        Incorrect_Number_Of_Function_Arguments_Error,
        // Attribute errors:
        Incompatible_Attribute_Error,
        Invalid_Attribute_Arguments_Error,
        Duplicate_Attribute_Error,
        // User defined errors:
        Removed_Attribute_Error,
    };
    enum WarningMessageType {
        // General errors:
        Default_Warning = 1,

        // User defined warnings:
        Deprecated_Attribute_Warning,
    };

    struct MessageBlockNode {
        MessageBlockNode* parentNode = nullptr;
        std::vector<MessageBlockNode*> childNodes = std::vector<MessageBlockNode*>();

        ASTNode* astNode = nullptr;
        std::vector<std::pair<void*, std::string>> attributeNodes;
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
            attributeNodes = std::vector<std::pair<void*, std::string>>();
            childNodes = std::vector<MessageBlockNode*>();
        }
    };

    // Base type for all message segments
    struct Segment {
        virtual void print() const = 0;
        virtual ~Segment() = default;
    };

    // A plain text segment, e.g. "(while: ...)" context lines
    struct ContextSegment : Segment {
        std::string text;
        std::string color;

        ContextSegment() {}
        ContextSegment(std::string text, std::string color = console::brightBlackFGColor)
            : text(std::move(text)), color(std::move(color)) {}

        void print() const override;
    };

    enum PrintFlags : uint32_t {
        Print_Default = 0,
        Hide_Scope_Body_Contents = 1 << 0,
    };

    enum AnnotationRenderStyle : uint8_t {
        Render_Underline,
        Render_Arrow,
    };

    using ConnectorCapSet = std::array<std::string, 4>;

    extern const ConnectorCapSet CONNECTOR_CAP_SET_NONE;
    extern const ConnectorCapSet CONNECTOR_CAP_SET_SPACE;
    extern const ConnectorCapSet CONNECTOR_CAP_SET_HORIZONTAL_SPACE;
    extern const ConnectorCapSet CONNECTOR_CAP_SET_ARROWS;
    extern const ConnectorCapSet CONNECTOR_CAP_SET_TEE;
    extern const ConnectorCapSet CONNECTOR_CAP_SET_TRIANGLES;

    // Underline annotation - a child of SourceCodeSegment, not added to Message directly
    struct UnderlinedSegment {
        ASTNode* focusedNode = nullptr;

        std::string underlineMessage = "";
        std::string underlineColor = console::cyanFGColor;
        std::string underlineChar = "^";
        ConnectorCapSet connectorStartCaps = CONNECTOR_CAP_SET_NONE;
        ConnectorCapSet connectorEndCaps = CONNECTOR_CAP_SET_NONE;
        AnnotationRenderStyle renderStyle = Render_Underline;
        bool verticallyUnlocked = false;

        // Extra source lines to show above and below this underline's focus node
        uint8_t aboveContextLines = 0;
        uint8_t belowContextLines = 0;

        uint32_t printFlags = Print_Default;

        UnderlinedSegment() {}
        UnderlinedSegment(ASTNode* focusedNode, std::string underlineMessage = "", std::string underlineColor = console::cyanFGColor, std::string underlineChar = "^", uint32_t printFlags = Print_Default)
        {
            this->focusedNode = focusedNode;
            this->underlineMessage = underlineMessage;
            this->underlineColor = underlineColor;
            this->underlineChar = underlineChar;
            this->renderStyle = Render_Underline;
            this->printFlags = printFlags;
        }
    };

    // Arrow annotations reuse the same routing rules as underlines, but their
    // connector starts at the right side of the focused source row.
    struct ArrowSegment : UnderlinedSegment {
        ArrowSegment()
        {
            renderStyle = Render_Arrow;
            connectorStartCaps = CONNECTOR_CAP_SET_ARROWS;
            connectorEndCaps = CONNECTOR_CAP_SET_SPACE;
        }
        ArrowSegment(ASTNode* focusedNode, std::string underlineMessage = "", std::string underlineColor = console::cyanFGColor, uint32_t printFlags = Print_Default)
            : UnderlinedSegment(focusedNode, std::move(underlineMessage), std::move(underlineColor), "", printFlags)
        {
            renderStyle = Render_Arrow;
            connectorStartCaps = CONNECTOR_CAP_SET_ARROWS;
            connectorEndCaps = CONNECTOR_CAP_SET_SPACE;
        }
    };

    // A source code segment: renders one or more underlined locations in a file,
    // shown alongside any explicitly added context nodes.
    struct SourceCodeSegment : Segment {
        std::vector<ASTNode*> contextNodes;
        std::vector<UnderlinedSegment> underlines;

        SourceCodeSegment() {}
        explicit SourceCodeSegment(UnderlinedSegment underlineSegment) { underlines.push_back(std::move(underlineSegment)); }
        explicit SourceCodeSegment(ArrowSegment arrowSegment) { underlines.push_back(std::move(arrowSegment)); }
        SourceCodeSegment(std::initializer_list<UnderlinedSegment> underlineSegments)
            : underlines(underlineSegments) {}

        SourceCodeSegment& addContextNode(ASTNode* node)
        {
            contextNodes.push_back(node);
            return *this;
        }
        SourceCodeSegment& addUnderline(UnderlinedSegment underlineSegment)
        {
            underlines.push_back(std::move(underlineSegment));
            return *this;
        }
        SourceCodeSegment& addArrow(ArrowSegment arrowSegment)
        {
            underlines.push_back(std::move(arrowSegment));
            return *this;
        }

        void print() const override;
    };

    // A message is a sequence of segments printed in order
    struct Message {
        std::string messageString = "";
        std::vector<std::unique_ptr<Segment>> segments;

        Message() {}
        explicit Message(std::string messageString)
            : messageString(std::move(messageString)) {}

        template<typename T>
        void addSegment(T segment)
        {
            segments.emplace_back(std::make_unique<T>(std::move(segment)));
        }

        void print() const
        {
            for (const auto& segment : segments)
                segment->print();
        }
    };

    using PrintRow = std::vector<asaToken*>;
    using PrintGrid = std::vector<PrintRow>;

    extern MessageBlockNode* currentNode;

    extern bool suppressErrors;
    extern bool exitOnError;

    void clearNodes(MessageBlockNode* node = nullptr);
    void startBlock(ASTNode* node, std::string messageString, const char* sourceFunction, uint32_t sourceLine, const char* sourcePath, BlockType blocktype);
    void endBlock();
    void addAttribute(void* item, std::string messageString = "");
    void addAttribute(ASTNode*& node, std::string messageString = "");
    void addAttributes(std::vector<ASTNode*>& nodes, std::string messageString = "");
    void* error(std::string messageString, ErrorMessageType messageType = Default_Error);
    void* warning(std::string messageString, WarningMessageType messageType = Default_Warning);
    void printNode(ASTNode* node);

}  // namespace messageSystem
