
#include "messagehandler.h"

#include <climits>
#include <optional>
#include <set>

namespace messageSystem {

    const ConnectorCapSet CONNECTOR_CAP_SET_NONE = {"", "", "", ""};
    const ConnectorCapSet CONNECTOR_CAP_SET_SPACE = {" ", " ", " ", " "};
    const ConnectorCapSet CONNECTOR_CAP_SET_HORIZONTAL_SPACE = {"", "", " ", " "};
    const ConnectorCapSet CONNECTOR_CAP_SET_TEE = {"┬", "┴", "├", "┤"};
    const ConnectorCapSet CONNECTOR_CAP_SET_ARROWS = {"^", "v", "<", ">"};
    const ConnectorCapSet CONNECTOR_CAP_SET_TRIANGLES = {"▲", "▼", "◀", "▶"};

    //MessageBlockNode* errorTree = nullptr;
    MessageBlockNode* currentNode = nullptr;

    bool suppressErrors = false;
    bool exitOnError = true;

    // Value which stores if it has printed a message before.
    // Makes spacing better if it has
    bool hasPrintedMessage = false;

    static const std::string GUTTER_COLOR = console::yellowFGColor;
    static const std::string CONTEXT_INFO_COLOR = console::brightBlackFGColor;
    static const std::string NEUTRAL_HIGHLIGHT_COLOR = console::cyanFGColor;
    static const std::string ERROR_COLOR = console::redFGColor;
    static const std::string WARNING_COLOR = console::brightYellowFGColor;

    static const std::string CONNECTOR_VERTICAL = "│";
    static const std::string CONNECTOR_HORIZONTAL = "─";
    static const std::string CONNECTOR_TR = "┌";
    static const std::string CONNECTOR_TL = "┐";
    static const std::string CONNECTOR_BR = "└";
    static const std::string CONNECTOR_BL = "┘";
    static const std::string CONNECTOR_TEE_UP = "┴";
    static const std::string CONNECTOR_TEE_DOWN = "┬";
    static const std::string CONNECTOR_TEE_RIGHT = "├";
    static const std::string CONNECTOR_TEE_LEFT = "┤";
    static const int MESSAGE_RENDER_FALLBACK_WIDTH = 80;
    static const int MESSAGE_RENDER_MIN_CONTENT_WIDTH = 24;


    void clearNodes(MessageBlockNode* node)
    {
        if (node == nullptr)
            node = currentNode;
        // Walk up the parentNode chain and delete every node
        while (node) {
            MessageBlockNode* parent = node->parentNode;
            delete node;
            node = parent;
        }
        currentNode = nullptr;
    }

    void printMessageSystemError(std::string messageString)
    {
        console::writeLine("Fatal Compiler Error: " + messageString, ERROR_COLOR);
        console::writeLine("Last added node:     Line: " + std::to_string(currentNode->sourceLine) + " | " + currentNode->sourcePath, ERROR_COLOR);
    }

    void startBlock(ASTNode* node, std::string messageString, const char* sourceFunction, uint32_t sourceLine, const char* sourcePath, BlockType blockType)
    {
        if (currentNode == nullptr) {
            currentNode = new MessageBlockNode(node, messageString, sourceFunction, sourceLine, sourcePath, blockType);
        }
        else {
            //if (currentNode->blockType != blockType) {
            //    printMessageSystemError("New message block type started before old type completed.");
            //    console::writeLine("Current added node:  Line: " + std::to_string(sourceLine) + " | " + sourcePath, ERROR_COLOR);
            //    console::writeLine("Block type:  " + std::to_string(blockType) + " != " + std::to_string(currentNode->blockType), ERROR_COLOR);
            //    exit(1);
            //}

            MessageBlockNode* messageBlock = new MessageBlockNode(node, messageString, sourceFunction, sourceLine, sourcePath, blockType);
            messageBlock->parentNode = currentNode;
            //currentNode->childNodes.push_back(messageBlock);
            currentNode = messageBlock;
        }
    }

    void endBlock()
    {
        if (currentNode) {
            MessageBlockNode* parentNode = currentNode->parentNode;
            delete currentNode;

            if (parentNode)
                currentNode = parentNode;
            else
                currentNode = nullptr;
        }
    }

    void addAttributes(std::vector<ASTNode*>& nodes, std::string messageString)
    {
        if (!currentNode) {
            printMessageSystemError("Unexpected end of error tree.");
            exit(1);
        }
        for (const auto& n : nodes)
            currentNode->attributeNodes.emplace_back((void*)n, messageString);
    }

    void addAttribute(void* item, std::string messageString)
    {
        if (!currentNode) {
            printMessageSystemError("Unexpected end of error tree.");
            exit(1);
        }
        currentNode->attributeNodes.emplace_back((void*)item, messageString);
    }

    void addAttribute(ASTNode*& node, std::string messageString)
    {
        if (!currentNode) {
            printMessageSystemError("Unexpected end of error tree.");
            exit(1);
        }
        currentNode->attributeNodes.emplace_back((void*)node, messageString);
    }

    void* error(std::string messageString, ErrorMessageType messageType)
    {
        wasError = true;
        int errorTraceOffset = 0;
        std::string messageTypeNumString = std::to_string(messageType);

        if (suppressErrors)
            return nullptr;

        if (!currentNode) {
            printMessageSystemError("Unexpected end of error tree.");
            exit(1);
        }

        if (hasPrintedMessage)
            console::writeLine();
        hasPrintedMessage = true;

        if (verbosity >= 3)
            if (currentNode->blockType == Parser_Block)
                console::write("compilation->parser\n", ERROR_COLOR);
            else if (currentNode->blockType == Codegen_Block)
                console::write("compilation->codegen\n", ERROR_COLOR);
        console::write("error", ERROR_COLOR);
        if (messageType == Default_Error) {
            console::write("[");
            console::write("E???", ERROR_COLOR);
            console::write("]:  ");
        }
        else {
            console::write("[");
            console::write("E" + PadString(messageTypeNumString, '0', 3), ERROR_COLOR);
            console::write("]:  ");
        }
        console::write(messageString + "\n");

        Message msg;
        if (currentNode->messageString != "")
            msg.addSegment(ContextSegment("(while: " + currentNode->messageString + ")"));

        // Additional handling for different message types:
        switch (messageType) {
            case Undefined_Member_Error:
            case Undefined_Symbol_Error:
            case Undefined_Function_Error:
            case Undefined_Variable_Error: {
                SourceCodeSegment src(UnderlinedSegment(currentNode->astNode, "used here", ERROR_COLOR, "^"));
                src.addContextNode(currentNode->astNode->parentNode);
                msg.addSegment(src);
                msg.print();
                if (currentNode->attributeNodes.size() > 0) {
                    console::write("\nCould you have possibly meant one of the following?:\n");
                    console::write("Candidates:\n", NEUTRAL_HIGHLIGHT_COLOR);
                    console::indentation = 1;
                    for (int i = 0; i < (int)currentNode->attributeNodes.size(); i++)
                        printNode((ASTNode*)currentNode->attributeNodes[i].first);
                    console::indentation = 0;
                }
                break;
            }

            case Undefined_Function_Exact_Error: {
                SourceCodeSegment src(UnderlinedSegment(currentNode->astNode, "used here", ERROR_COLOR, "^"));
                src.addContextNode(currentNode->astNode->parentNode);
                msg.addSegment(src);
                msg.print();
                console::write("\nCould you have possibly meant this?:\n");
                console::write("Candidate:\n", NEUTRAL_HIGHLIGHT_COLOR);
                if (currentNode->attributeNodes.size() == 2) {
                    console::indentation = 1;
                    argumentList* argList = (argumentList*)currentNode->attributeNodes[0].first;
                    printFunctionDifferences(argList, (functionID*)currentNode->attributeNodes[1].first);
                    //printNode((ASTNode*)currentNode->attributeNodes[0].first);
                    console::indentation = 0;
                }
                else
                    printMessageSystemError("Missing required error message attributes");

                break;
            }

            case Incompatible_Attribute_Error: {
                SourceCodeSegment src(UnderlinedSegment(currentNode->astNode, "belongs to this", ERROR_COLOR, "^"));
                src.addContextNode(currentNode->astNode->parentNode);
                for (int i = 0; i < currentNode->attributeNodes.size(); i++) {
                    ArrowSegment a = ArrowSegment((ASTNode*)currentNode->attributeNodes[i].first, OrdinalSuffixString(i + 1) + " here", NEUTRAL_HIGHLIGHT_COLOR, Hide_Scope_Body_Contents);
                    a.verticallyUnlocked = true;
                    src.addArrow(a);
                }
                msg.addSegment(src);
                msg.print();
                break;
            }

            case Removed_Attribute_Error: {
                SourceCodeSegment src(UnderlinedSegment(currentNode->astNode, "used here", ERROR_COLOR, "^"));
                src.addContextNode(currentNode->astNode->parentNode);
                if (currentNode->attributeNodes.size() > 0)
                    src.addUnderline(UnderlinedSegment((ASTNode*)currentNode->attributeNodes[0].first, "defined here", NEUTRAL_HIGHLIGHT_COLOR, "~", Hide_Scope_Body_Contents));
                msg.addSegment(src);
                msg.print();
                break;
            }

            case Redefined_Error: {
                if (currentNode->attributeNodes.size() == 0) {
                    printMessageSystemError("Message expected at least one attribute node, got zero");
                    exit(1);
                }
                if (currentNode->attributeNodes[0].first == nullptr) {
                    printMessageSystemError("Message first attribute node is null");
                    exit(1);
                }
                SourceCodeSegment src;
                src.addUnderline(UnderlinedSegment(currentNode->astNode, "redefined here", ERROR_COLOR, "^"));
                src.addUnderline(UnderlinedSegment((ASTNode*)currentNode->attributeNodes[0].first, currentNode->attributeNodes[0].second, NEUTRAL_HIGHLIGHT_COLOR, "~", Hide_Scope_Body_Contents));
                msg.addSegment(src);
                msg.print();
                break;
            }

            case Type_Inference_From_Void_Error:
            case Syntax_Error: {
                SourceCodeSegment src(UnderlinedSegment(currentNode->astNode, "here", ERROR_COLOR, "^"));
                src.addContextNode(currentNode->astNode->parentNode);
                msg.addSegment(src);
                msg.print();
                break;
            }

            case Invalid_Compiler_Directive_Arguments_Error:
            default: {
                msg.addSegment(SourceCodeSegment(UnderlinedSegment(currentNode->astNode, "", ERROR_COLOR, "^")));
                msg.print();
                break;
            }
        }
        errorTraceOffset++;
        currentNode = currentNode->parentNode;

        // Trace back up the tree of blocks
        if (currentNode)
            for (int i = errorTraceOffset; i < maxErrorTraceDepth; i++) {
                console::indentation = i;
                if (currentNode->sourceFunction != "" && currentNode->sourceLine != 0 && currentNode->sourcePath != "") {
                    Message traceMsg;
                    if (currentNode->messageString != "")
                        traceMsg.addSegment(ContextSegment("(while: " + currentNode->messageString + ")"));
                    traceMsg.addSegment(SourceCodeSegment(UnderlinedSegment(currentNode->astNode, "", ERROR_COLOR, "^")));
                    traceMsg.print();
                }
                if (currentNode->parentNode == nullptr)
                    break;
                currentNode = currentNode->parentNode;
            }

        console::indentation = 0;
        console::writeLine("\nErrors were encountered during compilation.", ERROR_COLOR);

        if (exitOnError)
            exit(messageType);

        return nullptr;
    }

    void* warning(std::string messageString, WarningMessageType messageType)
    {
        int errorTraceOffset = 0;
        std::string messageTypeNumString = std::to_string(messageType);

        if (suppressErrors)
            return nullptr;

        if (!currentNode) {
            printMessageSystemError("Unexpected end of error tree.");
            exit(1);
        }

        if (hasPrintedMessage)
            console::writeLine();
        hasPrintedMessage = true;

        if (verbosity >= 3)
            if (currentNode->blockType == Parser_Block)
                console::write("compilation->parser\n", WARNING_COLOR);
            else if (currentNode->blockType == Codegen_Block)
                console::write("compilation->codegen\n", WARNING_COLOR);
        console::write("warning", WARNING_COLOR);
        if (messageType == Default_Warning) {
            console::write("[");
            console::write("W???", WARNING_COLOR);
            console::write("]:  ");
        }
        else {
            console::write("[");
            console::write("W" + PadString(messageTypeNumString, '0', 3), WARNING_COLOR);
            console::write("]:  ");
        }
        console::write(messageString + "\n");

        Message msg;
        if (currentNode->messageString != "")
            msg.addSegment(ContextSegment("(while: " + currentNode->messageString + ")"));

        // Additional handling for different message types:
        switch (messageType) {

            case Deprecated_Attribute_Warning: {
                SourceCodeSegment src(UnderlinedSegment(currentNode->astNode, "used here", WARNING_COLOR, "~"));
                src.addContextNode(currentNode->astNode->parentNode);
                if (currentNode->attributeNodes.size() > 0)
                    src.addUnderline(UnderlinedSegment((ASTNode*)currentNode->attributeNodes[0].first, "defined here", NEUTRAL_HIGHLIGHT_COLOR, "~", Hide_Scope_Body_Contents));
                msg.addSegment(src);
                msg.print();
                break;
            }

            default: {
                UnderlinedSegment warningUnderline(currentNode->astNode, "here", WARNING_COLOR, "~");
                warningUnderline.aboveContextLines = 1;
                warningUnderline.belowContextLines = 1;
                msg.addSegment(SourceCodeSegment(warningUnderline));
                msg.print();
                break;
            }
        }
        errorTraceOffset++;
        currentNode = currentNode->parentNode;

        // Trace back up the tree of blocks
        if (currentNode)
            for (int i = errorTraceOffset; i < maxErrorTraceDepth; i++) {
                console::indentation = i;
                if (currentNode->sourceFunction != "" && currentNode->sourceLine != 0 && currentNode->sourcePath != "") {
                    Message traceMsg;
                    if (currentNode->messageString != "")
                        traceMsg.addSegment(ContextSegment("(while: " + currentNode->messageString + ")"));
                    traceMsg.addSegment(SourceCodeSegment(UnderlinedSegment(currentNode->astNode, "here", WARNING_COLOR, "~")));
                    traceMsg.print();
                }
                if (currentNode->parentNode == nullptr)
                    break;
                currentNode = currentNode->parentNode;
            }

        console::writeLine();

        return nullptr;
    }

    // Returns the ANSI color for a token type used by the syntax highlighter.
    // Returns an empty string for tokens that should not be colored.
    const std::string& getTokenColor(TokenType tt)
    {
        static const std::string none = "";
        switch (tt) {
            case If_Statement:
            case Else_Statement:
            case For_Statement:
            case While_Statement:
            case In_Keyword:
            case Return_Statement:
            case Result_Statement:
            case Break_Statement:
            case Continue_Statement:
            case Goto_Statement:
            case Throw_Statement:
            case Struct_Define:
            case Module_Define:
            case Enum_Define:
            case Operator_Keyword:
            case Ref:
            case Const:
            case Exact:
            case Void:
                return console::cyanFGColor;
            case String:
            case Character:
                return console::brightYellowFGColor;
            case Integer:
            case Float:
            case True_Literal:
            case False_Literal:
                return console::magentaFGColor;
            case Comment:
                return console::brightBlackFGColor;
            case Plus:
            case Minus:
            case Slash:
            case Star:
            case Star_Star:
            case Bar:
            case Bar_Bar:
            case Bar_Bar_Bar:
            case Ampersand:
            case Ampersand_Ampersand:
            case Tilde:
            case Tilde_Tilde:
            case Caret:
            case Caret_Caret:
            case Percent:
            case Percent_Percent:
            case Arrow_Left:
            case Arrow_Right:
            case Bang:
            case Bang_Bang:
            case Bang_Equal:
            case Plus_Equal:
            case Plus_Plus:
            case Minus_Equal:
            case Minus_Minus:
            case Times_Equal:
            case Slash_Equal:
            case Equal:
            case Equal_Equal:
            case Less:
            case Less_Equal:
            case Greater:
            case Greater_Equal:
            case Dot_Dot:
            case Dot:
            case Dot_At:
            case Colon:
            case Colon_Colon:
                return console::redFGColor;
            case Left_Paren:
            case Right_Paren:
            case Left_Bracket:
            case Right_Bracket:
            case Both_Brackets:
            case Left_Brace:
            case Right_Brace:
            case Comma:
            case Semi_Colon:
                return console::brightBlackFGColor;
            case Hash:
            case At:
            case At_At:
            case Dollar:
            case Dot_Dot_Dot:
            case Identifier:
                return console::whiteFGColor;
            case _Print_Gutter:
            case _Print_Gutter_Ellipsis:
            case _Print_Ellipsis:
            case _Print_Truncation_Ellipsis:
                return GUTTER_COLOR;
            default:
                return none;
        }
    }

    // Syntax-highlights a raw lineValue string (no indent adjustment).
    // Token positions are looked up via allTokens by lineNum.
    std::string syntaxHighlightRaw(const std::string& raw, int lineNum)
    {
        if (!console::useColor)
            return raw;

        struct Span {
            int s, e;
            const std::string* color;
        };
        std::vector<Span> spans;
        for (auto* tok : allTokens) {
            if (tok->lineNumber != lineNum)
                continue;
            const std::string& col = getTokenColor(tok->tokenType);
            if (col.empty())
                continue;
            int s = tok->indexInLine - 1;
            int e = s + (int)tok->length;
            if (e <= 0 || s >= (int)raw.size())
                continue;
            spans.push_back({s, e, &col});
        }
        if (spans.empty())
            return raw;

        std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.s < b.s; });

        std::string result;
        int pos = 0;
        for (auto& sp : spans) {
            int s = std::max(0, sp.s);
            int e = std::min((int)raw.size(), sp.e);
            if (s > pos)
                result += raw.substr(pos, s - pos);
            if (s < e) {
                result += *sp.color;
                result += raw.substr(s, e - s);
                result += console::resetColor;
            }
            pos = e;
        }
        if (pos < (int)raw.size())
            result += raw.substr(pos);
        return result;
    }

    void printNode(ASTNode* node)
    {
        tokenRange tokRange = getASTTokenRange(node);
        asaToken* startToken = tokRange.first;
        asaToken* endToken = tokRange.second;

        if (!startToken || !startToken->lineValue)
            return;

        const std::string& line = *(startToken->lineValue);
        int startCol = startToken->indexInLine - 1;  // 0-based
        int endCol = (endToken && endToken->lineNumber == startToken->lineNumber)
                         ? (endToken->indexInLine - 1 + endToken->length)
                         : (int)line.size();
        startCol = std::max(0, std::min(startCol, (int)line.size()));
        endCol = std::max(startCol, std::min(endCol, (int)line.size()));

        std::string slice = line.substr(startCol, endCol - startCol);

        console::applyIndent();
        console::write(syntaxHighlightRaw(slice, startToken->lineNumber));
        console::write("\n");
    }


    int countLeadingSpaces(const std::string& s)
    {
        int n = 0;
        for (char c : s) {
            if (c == ' ')
                n++;
            else
                break;
        }
        return n;
    }

    static std::map<int, int> collectLineIndents(int startLine, int endLine, std::string* filePath)
    {
        std::map<int, int> indents;
        for (auto* tok : allTokens) {
            if (tok->tokenType == EndOfLine)
                continue;
            if (filePath && tok->filePath != filePath)
                continue;
            if (tok->lineNumber >= startLine && tok->lineNumber <= endLine) {
                if (indents.find(tok->lineNumber) == indents.end())
                    indents[tok->lineNumber] = tok->lineIndent;
            }
        }
        return indents;
    }

    static std::map<int, std::string*> collectSourceLines(int startLine, int endLine, std::string* filePath)
    {
        std::map<int, std::string*> result;
        for (auto* tok : allTokens) {
            if (tok->tokenType == EndOfLine)
                continue;
            if (filePath && tok->filePath != filePath)
                continue;
            if (tok->lineValue != nullptr && tok->lineNumber >= startLine && tok->lineNumber <= endLine)
                result[tok->lineNumber] = tok->lineValue;
        }
        return result;
    }

    void ContextSegment::print() const
    {
        console::applyIndent();
        console::write(text + "\n", color);
    }

    static std::string formatLineNum(int lineNum, int width)
    {
        std::string s = std::to_string(lineNum + 1);
        while ((int)s.size() < width)
            s = " " + s;
        return s;
    }

    static int computeVisCol(asaToken* tok, int minIndent)
    {
        int leading = countLeadingSpaces(*tok->lineValue);
        int absoluteCol = tok->lineIndent + (tok->indexInLine - 1 - leading);
        return std::max(0, absoluteCol - minIndent);
    }

    static asaToken* makeToken(std::deque<asaToken>& ownedTokens, std::string str, TokenType type, const std::string* color = nullptr, uint16_t displayWidth = 0)
    {
        ownedTokens.push_back(asaToken(std::move(str), type));
        ownedTokens.back().color = color;
        if (displayWidth > 0)
            ownedTokens.back().length = displayWidth;
        return &ownedTokens.back();
    }

    static void renderGrid(const PrintGrid& grid)
    {
        for (const PrintRow& row : grid) {
            console::applyIndent();
            int lastNonSpace = -1;
            for (int i = 0; i < (int)row.size(); i++)
                if (row[i]->tokenType != _Print_Space || row[i]->tokenStr != " ")
                    lastNonSpace = i;
            for (int i = 0; i <= lastNonSpace; i++) {
                asaToken* tok = row[i];
                if (tok->color)
                    console::write(tok->tokenStr, *tok->color);
                else {
                    const std::string& autoColor = getTokenColor(tok->tokenType);
                    if (!autoColor.empty())
                        console::write(tok->tokenStr, autoColor);
                    else
                        console::write(tok->tokenStr);
                }
            }
            console::write("\n");
        }
    }

    struct RenderUnderline {
        const UnderlinedSegment* underlineSegment;
        asaToken* startToken;
        asaToken* endToken;
    };

    // Region: diagnostic renderer layout and routing helpers
    // Records a placed underline or arrow attachment in absolute grid coordinates.
    struct PlacedUnderline {
        const UnderlinedSegment* underlineSegment = nullptr;
        int row = 0;
        int startCol = 0;
        int endCol = 0;
        bool startsOnSourceRow = false;
        bool verticallyUnlocked = false;
        bool compactForcedUnlocked = false;
        // Last source line that the label is allowed to belong to.
        int labelAfterSrcLine = 0;
        // Earliest legal label row after accounting for trailing underline rows.
        int minLabelRow = 0;
    };

    // One cell on the routing canvas.
    struct CellPoint {
        int row = 0;
        int col = 0;

        CellPoint() = default;
        CellPoint(int row, int col)
            : row(row), col(col) {}

        bool operator==(const CellPoint& other) const
        {
            return row == other.row && col == other.col;
        }
    };

    // One routed connector step, optionally recording a skipped truncated source row.
    struct PathStep {
        CellPoint point;
        bool skippedSourceRow = false;
        int skippedRow = -1;

        PathStep() = default;
        PathStep(CellPoint point)
            : point(point) {}
    };

    // Source-line interval used when deciding where to insert spacer rows.
    struct FocusRange {
        int start = 0;
        int end = 0;
    };

    // Routing metadata for one canvas row.
    struct RoutingRowState {
        bool hasSource = false;
        bool hasUnderline = false;
        bool isTruncated = false;
        int rightEdge = -1;
    };

    // Expanded one-cell-per-token routing surface and its derived limits.
    struct RoutingCanvas {
        PrintGrid canvas;
        std::vector<RoutingRowState> rows;
        int farRight = 0;
        int connectorMinCol = 6;
        int connectorPadding = 2;
        int connectorMaxCol = 0;
    };

    // Chosen row/column/path for one label after scoring candidates.
    struct LabelPlacement {
        int row = -1;
        int col = -1;
        int score = INT_MAX;
        std::vector<PathStep> path;
        bool isProjectedHorizontal = false;
    };

    // Inputs derived from the visible source window before any annotation rows are inserted.
    struct SourceRenderWindow {
        std::string* renderFilePath = nullptr;
        std::vector<int> sortedLines;
        std::set<int> separatorAfterLines;
        std::map<int, std::string*> lineValues;
        std::map<int, int> indents;
        int minIndent = 0;
        int maxVisible = 0;
        int lineNumWidth = 3;
        int renderMaxWidth = MESSAGE_RENDER_FALLBACK_WIDTH;
        int labelAfterLine = 0;
    };

    // Source rows plus line-to-row mapping before label routing starts.
    struct SourceGridLayout {
        PrintGrid grid;
        std::map<int, int> lineToGridRow;
    };

    // Diagnostic rendering uses dedicated print tokens for gutters, connectors, and labels.
    static bool isPrintToken(TokenType tt)
    {
        return tt >= _Print_Gutter && tt <= _Print_Message;
    }

    // Arrow annotations route from the source row rather than an underline row.
    static bool isArrowSegment(const UnderlinedSegment* underlineSegment)
    {
        return underlineSegment && underlineSegment->renderStyle == Render_Arrow;
    }

    // Underline annotations require a visible underline glyph.
    static bool isUnderlineSegment(const UnderlinedSegment* underlineSegment)
    {
        return underlineSegment && underlineSegment->renderStyle == Render_Underline && !underlineSegment->underlineChar.empty();
    }

    // Only arrow and underline annotations participate in source rendering.
    static bool isRenderableSegment(const UnderlinedSegment* underlineSegment)
    {
        return isArrowSegment(underlineSegment) || isUnderlineSegment(underlineSegment);
    }

    static bool rowContainsSourceTokens(const PrintRow& row)
    {
        // Source rows are the ones connectors must avoid or route under.
        for (asaToken* tok : row)
            if (!isPrintToken(tok->tokenType))
                return true;
        return false;
    }

    // Content width excludes gutter-only tokens.
    static int getRowContentWidth(const PrintRow& row)
    {
        int width = 0;
        for (asaToken* tok : row) {
            TokenType tt = tok->tokenType;
            if (tt == _Print_Gutter || tt == _Print_Gutter_Marker || tt == _Print_Gutter_Ellipsis)
                continue;
            width += (int)tok->length;
        }
        return width;
    }

    static bool rowHasEllipsis(const PrintRow& row)
    {
        // Truncated source rows use a dedicated render token.
        for (asaToken* tok : row)
            if (tok->tokenType == _Print_Truncation_Ellipsis)
                return true;
        return false;
    }

    // Returns true only for dedicated underline rows that contain no source text.
    static bool rowHasUnderlinesOnly(const PrintRow& row)
    {
        bool hasUnderline = false;
        for (asaToken* tok : row) {
            if (!isPrintToken(tok->tokenType))
                return false;
            if (tok->tokenType == _Print_Underline)
                hasUnderline = true;
            if (tok->tokenType == _Print_Ellipsis || tok->tokenType == _Print_Truncation_Ellipsis)
                return false;
        }
        return hasUnderline;
    }

    // Blank gutter rows are safe places to reuse as anchor rows.
    static bool rowIsBlankGutter(const PrintRow& row)
    {
        for (asaToken* tok : row)
            if (tok->tokenType != _Print_Gutter && tok->tokenType != _Print_Space)
                return false;
        return true;
    }

    static void appendUnderlineAnnotation(PrintRow& row, std::deque<asaToken>& ownedTokens, const UnderlinedSegment* underlineSegment, int underlineCol, int underlineLen, int currentWidth)
    {
        // Underlines are drawn on their own annotation rows, never into source text.
        if (underlineCol > currentWidth)
            row.push_back(makeToken(ownedTokens, std::string(underlineCol - currentWidth, ' '), _Print_Space));
        for (int i = 0; i < std::max(1, underlineLen); i++)
            row.push_back(makeToken(ownedTokens, underlineSegment->underlineChar, _Print_Underline, &underlineSegment->underlineColor));
    }

    // Display width includes every token, including gutter and connector cells.
    static int getRowDisplayWidth(const PrintRow& row)
    {
        int width = 0;
        for (asaToken* tok : row)
            width += (int)tok->length;
        return width;
    }

    // Keep underline bookkeeping aligned when rows are inserted into the grid.
    static void shiftPlacedUnderlineRows(std::vector<PlacedUnderline>& placed, int insertedAfterRow)
    {
        // Keep recorded underline rows aligned after inserting render rows.
        for (PlacedUnderline& item : placed) {
            if (item.row > insertedAfterRow)
                item.row++;
            if (item.minLabelRow > insertedAfterRow)
                item.minLabelRow++;
        }
    }

    // Keep source-line lookups aligned with later row insertions.
    static void shiftLineToGridRows(std::map<int, int>& lineToGridRow, int insertedAfterRow)
    {
        for (auto& [ln, ri] : lineToGridRow)
            if (ri > insertedAfterRow)
                ri++;
    }

    // Multi-line spans update the earliest row where their shared label may appear.
    static void updatePlacedUnderlineMinLabelRow(std::vector<PlacedUnderline>& placed, const UnderlinedSegment* underlineSegment, int minLabelRow)
    {
        // Multi-line underlines may extend the earliest legal label row.
        for (PlacedUnderline& item : placed)
            if (item.underlineSegment == underlineSegment)
                item.minLabelRow = std::max(item.minLabelRow, minLabelRow);
    }

    // Some following source rows can be reused as anchor rows if the connector stays to the right.
    static bool rowAllowsStraightAnchorReuse(const PrintRow& row, int absStartCol, int absEndCol, int padding)
    {
        if (!rowContainsSourceTokens(row) || rowHasEllipsis(row))
            return false;

        int first = absEndCol - absStartCol + 1 <= padding * 2 ? absStartCol : absStartCol + padding;
        int last = absEndCol - absStartCol + 1 <= padding * 2 ? absEndCol : absEndCol - padding;
        int blockedThroughCol = getRowDisplayWidth(row) + 1;
        return last > blockedThroughCol && first <= last;
    }

    // Labels attach from a row below the underline, reusing existing rows when possible.
    static int insertAnchorRow(PrintGrid& grid, std::deque<asaToken>& ownedTokens, std::map<int, int>& lineToGridRow, std::vector<PlacedUnderline>& placed, int lineNumWidth, int afterRow, int absStartCol, int absEndCol)
    {
        // Reuse a following row when the connector can continue through it without an extra turn.
        if (afterRow + 1 < (int)grid.size()) {
            const PrintRow& nextRow = grid[afterRow + 1];
            if (rowIsBlankGutter(nextRow) || rowAllowsStraightAnchorReuse(nextRow, absStartCol, absEndCol, 2))
                return afterRow + 1;
        }

        // Labels route from a blank row below the underline row.
        PrintRow anchorRow;
        anchorRow.push_back(makeToken(ownedTokens, std::string(lineNumWidth, ' ') + " |  ", _Print_Gutter));
        grid.insert(grid.begin() + afterRow + 1, std::move(anchorRow));
        shiftPlacedUnderlineRows(placed, afterRow);
        shiftLineToGridRows(lineToGridRow, afterRow);
        return afterRow + 1;
    }

    // Truncation may need a sliced token that preserves the original token metadata.
    static asaToken* makeSlicedToken(std::deque<asaToken>& ownedTokens, asaToken* srcTok, int startOffset, int sliceWidth)
    {
        std::string slice = srcTok->tokenStr.substr(startOffset, sliceWidth);
        ownedTokens.push_back(*srcTok);
        asaToken& tok = ownedTokens.back();
        tok.tokenStr = slice;
        tok.length = sliceWidth;
        return &tok;
    }

    // Routing runs on a fully expanded one-cell canvas, not on variable-width tokens.
    static PrintGrid expandGridToCanvas(const PrintGrid& grid, std::deque<asaToken>& ownedTokens, int minWidth)
    {
        // The router works on single-cell canvas coordinates.
        int width = minWidth;
        for (const PrintRow& row : grid)
            width = std::max(width, getRowDisplayWidth(row));

        PrintGrid canvas;
        for (const PrintRow& row : grid) {
            PrintRow canvasRow;
            for (asaToken* tok : row) {
                TokenType type = tok->tokenType;
                const std::string* color = tok->color;
                if (tok->tokenStr.empty()) {
                    canvasRow.push_back(makeToken(ownedTokens, " ", _Print_Space, color, 1));
                    continue;
                }
                for (char ch : tok->tokenStr)
                    canvasRow.push_back(makeToken(ownedTokens, std::string(1, ch), type, color, 1));
            }
            while ((int)canvasRow.size() < width)
                canvasRow.push_back(makeToken(ownedTokens, " ", _Print_Space, nullptr, 1));
            canvas.push_back(std::move(canvasRow));
        }
        return canvas;
    }

    // Grow the routing canvas without disturbing existing token cells.
    static void ensureCanvasSize(PrintGrid& canvas, std::deque<asaToken>& ownedTokens, int minRows, int minCols)
    {
        int currentCols = 0;
        for (const PrintRow& row : canvas)
            currentCols = std::max(currentCols, (int)row.size());
        currentCols = std::max(currentCols, minCols);

        for (PrintRow& row : canvas)
            while ((int)row.size() < currentCols)
                row.push_back(makeToken(ownedTokens, " ", _Print_Space, nullptr, 1));

        while ((int)canvas.size() < minRows) {
            PrintRow row;
            for (int i = 0; i < currentCols; i++)
                row.push_back(makeToken(ownedTokens, " ", _Print_Space, nullptr, 1));
            canvas.push_back(std::move(row));
        }
    }

    // Any non-space token blocks routing or label placement.
    static bool canvasCellOccupied(const PrintGrid& canvas, int row, int col)
    {
        if (row < 0 || row >= (int)canvas.size() || col < 0 || col >= (int)canvas[row].size())
            return true;
        asaToken* tok = canvas[row][col];
        return tok->tokenType != _Print_Space || tok->tokenStr != " ";
    }

    // Connectors may legally reuse existing connector glyph cells when routing is shared.
    static bool isConnectorToken(TokenType tt)
    {
        return tt == _Print_Connector_V ||
               tt == _Print_Connector_H ||
               tt == _Print_Connector_Corner ||
               tt == _Print_Connector_Over ||
               tt == _Print_Connector_Under;
    }

    // Labels reserve a padded box, but only the label row blocks the full horizontal padding.
    static bool labelFitsAt(const PrintGrid& canvas, int row, int col, int len, int padding)
    {
        if (row < 0 || row >= (int)canvas.size())
            return false;
        int horizontalPadding = padding * 2;
        int startRow = std::max(0, row - padding);
        int endRow = std::min((int)canvas.size() - 1, row + padding);
        for (int r = startRow; r <= endRow; r++) {
            // Source rows may approach the label box vertically. On neighboring rows,
            // only other label text reserves the full horizontal padding. Connector
            // columns may pass closer so shared label rows do not get stretched apart.
            int startCol = col;
            int endCol = col + len - 1;
            if (r == row) {
                startCol = std::max(0, col - horizontalPadding);
                endCol = col + len - 1 + horizontalPadding;
            }
            if (endCol >= (int)canvas[r].size())
                return false;
            int paddedStartCol = std::max(0, col - horizontalPadding);
            int paddedEndCol = col + len - 1 + horizontalPadding;
            for (int c = startCol; c <= endCol; c++)
                if (canvasCellOccupied(canvas, r, c))
                    return false;
            if (r == row)
                continue;
            if (paddedEndCol >= (int)canvas[r].size())
                return false;
            for (int c = paddedStartCol; c <= paddedEndCol; c++) {
                if (c >= startCol && c <= endCol)
                    continue;
                asaToken* tok = canvas[r][c];
                if (tok->tokenType == _Print_Message)
                    return false;
            }
        }
        return true;
    }

    // Connector cells must stay clear of protected source/underline rows unless they route past the row edge.
    static bool cellAllowsPathRouting(const PrintGrid& canvas, const std::vector<RoutingRowState>& rowStates, int row, int col, CellPoint start, CellPoint goal, bool allowConnectorReuse)
    {
        // Connectors stay out of source and underline rows unless they are past the row's rendered right edge.
        if (row != start.row && row < (int)rowStates.size() && rowStates[row].hasUnderline) {
            int limit = rowStates[row].rightEdge + 2;
            if (col <= limit)
                return false;
        }
        if (row != start.row && row < (int)rowStates.size() && rowStates[row].hasSource) {
            int limit = rowStates[row].rightEdge + 2;
            if (col <= limit)
                return false;
        }
        for (int c = col - 2; c <= col + 2; c++) {
            if (c < 0 || c >= (int)canvas[row].size())
                continue;
            if ((row == start.row && c == start.col) || (row == goal.row && c == goal.col))
                continue;
            asaToken* tok = canvas[row][c];
            bool occupied = tok->tokenType != _Print_Space || tok->tokenStr != " ";
            if (occupied && !(allowConnectorReuse && isConnectorToken(tok->tokenType)))
                return false;
        }
        return true;
    }

    // Breadth-first search preserves the original shortest-path routing semantics.
    static bool findPath(const PrintGrid& canvas, const std::vector<RoutingRowState>& rowStates, CellPoint start, CellPoint goal, bool allowConnectorReuse, std::vector<PathStep>& outPath)
    {
        // Breadth-first search gives the shortest routed connector under the current movement rules.
        outPath.clear();
        if (start.row < 0 || goal.row < 0)
            return false;
        if (start.row >= (int)canvas.size() || goal.row >= (int)canvas.size())
            return false;
        if (start.col < 0 || goal.col < 0)
            return false;
        if (start.col >= (int)canvas[start.row].size() || goal.col >= (int)canvas[goal.row].size())
            return false;
        if (canvasCellOccupied(canvas, goal.row, goal.col) && !(allowConnectorReuse && isConnectorToken(canvas[goal.row][goal.col]->tokenType)))
            return false;

        std::vector<std::vector<uint8_t>> visited(canvas.size());
        std::vector<std::vector<CellPoint>> prev(canvas.size());
        std::vector<std::vector<int>> prevSkippedRow(canvas.size());
        for (int r = 0; r < (int)canvas.size(); r++) {
            visited[r].assign(canvas[r].size(), 0);
            prev[r].assign(canvas[r].size(), CellPoint(-1, -1));
            prevSkippedRow[r].assign(canvas[r].size(), -1);
        }

        std::deque<CellPoint> queue;
        queue.push_back(start);
        visited[start.row][start.col] = 1;

        static const int DR[4] = {0, 1, 0, -1};
        static const int DC[4] = {1, 0, -1, 0};

        while (!queue.empty()) {
            CellPoint cur = queue.front();
            queue.pop_front();
            if (cur == goal)
                break;

            for (int dir = -1; dir <= 1; dir += 2) {
                // Truncated source rows may be crossed by jumping from one side row to the other.
                int skippedRow = cur.row + dir;
                int nr = cur.row + dir * 2;
                int nc = cur.col;
                if (skippedRow < 0 || skippedRow >= (int)canvas.size() || nr < 0 || nr >= (int)canvas.size())
                    continue;
                if (nc < 0 || nc >= (int)canvas[nr].size())
                    continue;
                if (visited[nr][nc])
                    continue;
                if (skippedRow >= (int)rowStates.size() || !rowStates[skippedRow].isTruncated)
                    continue;
                if (canvasCellOccupied(canvas, nr, nc) && !(allowConnectorReuse && isConnectorToken(canvas[nr][nc]->tokenType)))
                    continue;
                visited[nr][nc] = 1;
                prev[nr][nc] = cur;
                prevSkippedRow[nr][nc] = skippedRow;
                queue.push_back(CellPoint(nr, nc));
            }

            for (int i = 0; i < 4; i++) {
                int nr = cur.row + DR[i];
                int nc = cur.col + DC[i];
                if (nr < 0 || nr >= (int)canvas.size())
                    continue;
                if (nc < 0 || nc >= (int)canvas[nr].size())
                    continue;
                if (visited[nr][nc])
                    continue;
                if (!(nr == goal.row && nc == goal.col) && !cellAllowsPathRouting(canvas, rowStates, nr, nc, start, goal, allowConnectorReuse))
                    continue;
                visited[nr][nc] = 1;
                prev[nr][nc] = cur;
                queue.push_back(CellPoint(nr, nc));
            }
        }

        if (!visited[goal.row][goal.col])
            return false;

        for (CellPoint cur = goal; !(cur == start); cur = prev[cur.row][cur.col]) {
            PathStep step(cur);
            step.skippedSourceRow = prevSkippedRow[cur.row][cur.col] != -1;
            step.skippedRow = prevSkippedRow[cur.row][cur.col];
            outPath.push_back(step);
        }
        outPath.push_back(PathStep(start));
        std::reverse(outPath.begin(), outPath.end());
        return true;
    }

    // Convert a routed path segment into the matching box-drawing glyph.
    static bool hasConnectorCapSet(const ConnectorCapSet& capSet)
    {
        for (const std::string& glyph : capSet)
            if (!glyph.empty())
                return true;
        return false;
    }

    static int directionIndexFromDelta(int dr, int dc)
    {
        if (dr < 0)
            return 0;
        if (dr > 0)
            return 1;
        if (dc < 0)
            return 2;
        return 3;
    }

    static std::string pickConnectorGlyph(const std::vector<PathStep>& path, int index)
    {
        // Each routed cell is converted into the box-drawing glyph implied by its neighbors.
        CellPoint cur = path[index].point;
        CellPoint prev = index > 0 ? path[index - 1].point : cur;
        CellPoint next = index + 1 < (int)path.size() ? path[index + 1].point : cur;

        if (index == 0 && index + 1 < (int)path.size()) {
            int dr = next.row - cur.row;
            int dc = next.col - cur.col;
            if (dr == 0 && dc > 0)
                return CONNECTOR_BR;
            if (dr == 0 && dc < 0)
                return CONNECTOR_BL;
            return CONNECTOR_VERTICAL;
        }

        int dr1 = cur.row - prev.row;
        int dc1 = cur.col - prev.col;
        int dr2 = next.row - cur.row;
        int dc2 = next.col - cur.col;

        if (dr1 == 0 && dr2 == 0)
            return CONNECTOR_HORIZONTAL;
        if (dc1 == 0 && dc2 == 0)
            return CONNECTOR_VERTICAL;
        if ((dr1 < 0 && dc2 > 0) || (dc1 < 0 && dr2 > 0))
            return CONNECTOR_TR;
        if ((dr1 < 0 && dc2 < 0) || (dc1 > 0 && dr2 > 0))
            return CONNECTOR_TL;
        if ((dr1 > 0 && dc2 > 0) || (dc1 < 0 && dr2 < 0))
            return CONNECTOR_BR;
        return CONNECTOR_BL;
    }

    static std::string pickStartConnectorCap(const ConnectorCapSet& capSet, const std::vector<PathStep>& path)
    {
        if ((int)path.size() < 2 || !hasConnectorCapSet(capSet))
            return "";
        int dr = path[1].point.row - path[0].point.row;
        int dc = path[1].point.col - path[0].point.col;
        int directionIndex = directionIndexFromDelta(-dr, -dc);
        return capSet[directionIndex];
    }

    static std::string pickEndConnectorCap(const ConnectorCapSet& capSet, const std::vector<PathStep>& path)
    {
        if ((int)path.size() < 2 || !hasConnectorCapSet(capSet))
            return "";
        int lastIndex = (int)path.size() - 1;
        int dr = path[lastIndex].point.row - path[lastIndex - 1].point.row;
        int dc = path[lastIndex].point.col - path[lastIndex - 1].point.col;
        int directionIndex = directionIndexFromDelta(dr, dc);
        return capSet[directionIndex];
    }

    // Paint the final connector glyphs and underpass markers onto the routing canvas.
    static void paintPath(PrintGrid& canvas, std::deque<asaToken>& ownedTokens, const std::vector<PathStep>& path, const std::string* color, const UnderlinedSegment* underlineSegment)
    {
        // Underpasses preserve the source row and mark the crossing above and below it.
        std::string startCap = underlineSegment ? pickStartConnectorCap(underlineSegment->connectorStartCaps, path) : "";
        std::string endCap = underlineSegment ? pickEndConnectorCap(underlineSegment->connectorEndCaps, path) : "";
        for (int i = 0; i < (int)path.size(); i++) {
            int row = path[i].point.row;
            int col = path[i].point.col;
            std::string glyph = pickConnectorGlyph(path, i);
            if (i == 0 && !startCap.empty())
                glyph = startCap;
            else if (i == (int)path.size() - 1 && !endCap.empty())
                glyph = endCap;
            TokenType type = _Print_Connector_Corner;
            if (glyph == CONNECTOR_VERTICAL)
                type = _Print_Connector_V;
            else if (glyph == CONNECTOR_HORIZONTAL)
                type = _Print_Connector_H;
            else if (glyph == "<")
                type = _Print_Connector_H;
            canvas[row][col] = makeToken(ownedTokens, glyph, type, color, 1);
            if (path[i].skippedSourceRow) {
                int prevRow = i > 0 ? path[i - 1].point.row : row;
                bool movedDown = row > prevRow;
                int upperRow = movedDown ? prevRow : row;
                int lowerRow = movedDown ? row : prevRow;
                canvas[upperRow][col] = makeToken(ownedTokens, CONNECTOR_TEE_UP, _Print_Connector_Over, color, 1);
                canvas[lowerRow][col] = makeToken(ownedTokens, CONNECTOR_TEE_DOWN, _Print_Connector_Under, color, 1);
            }
        }
    }

    // Fewer turns are preferred when ranking label placements.
    static int getPathTurnCount(const std::vector<PathStep>& path)
    {
        int turns = 0;
        for (int i = 1; i + 1 < (int)path.size(); i++) {
            int dr1 = path[i].point.row - path[i - 1].point.row;
            int dc1 = path[i].point.col - path[i - 1].point.col;
            int dr2 = path[i + 1].point.row - path[i].point.row;
            int dc2 = path[i + 1].point.col - path[i].point.col;
            if (dr1 != dr2 || dc1 != dc2)
                turns++;
        }
        return turns;
    }

    // Horizontal bounds of a routed connector path.
    static int getPathMinCol(const std::vector<PathStep>& path)
    {
        int minCol = INT_MAX;
        for (const PathStep& step : path)
            minCol = std::min(minCol, step.point.col);
        return minCol == INT_MAX ? 0 : minCol;
    }

    static int getPathMaxCol(const std::vector<PathStep>& path)
    {
        int maxCol = INT_MIN;
        for (const PathStep& step : path)
            maxCol = std::max(maxCol, step.point.col);
        return maxCol == INT_MIN ? 0 : maxCol;
    }

    // The score preserves the previous ordering: shorter path, fewer turns, tighter box, then farther-left goal.
    static int scoreLabelPath(const PlacedUnderline& item, int labelCol, int messageLen, int goalCol, const std::vector<PathStep>& path)
    {
        int bboxLeft = std::min(item.startCol, std::min(labelCol, getPathMinCol(path)));
        int bboxRight = std::max(item.endCol, std::max(labelCol + messageLen - 1, getPathMaxCol(path)));
        int bboxWidth = bboxRight - bboxLeft;
        return (int)path.size() * 1000000 + getPathTurnCount(path) * 10000 + bboxWidth * 100 + goalCol;
    }

    // Unlocked labels project onto the row where the locked route begins its final
    // vertical descent toward the bottom label row.
    static int getProjectedUnlockedRow(const LabelPlacement& lockedPlacement)
    {
        if (lockedPlacement.path.empty())
            return -1;

        int goalCol = lockedPlacement.path.back().point.col;
        int projectedRow = lockedPlacement.path.back().point.row;
        for (int i = (int)lockedPlacement.path.size() - 2; i >= 0; i--) {
            if (lockedPlacement.path[i].point.col != goalCol)
                break;
            projectedRow = lockedPlacement.path[i].point.row;
        }
        return projectedRow;
    }

    // Split-line unlocked labels can draw directly into the label from their source row.
    static bool buildDirectUnlockedPath(const PrintGrid& canvas, int labelRow, int labelCol, const std::vector<int>& startCols, std::vector<PathStep>& projectedPath)
    {
        for (int startCol : startCols) {
            int connectorEndCol = labelCol - 1;
            if (connectorEndCol < startCol + 3)
                continue;

            bool pathIsClear = true;
            for (int col = startCol + 1; col <= connectorEndCol; col++) {
                asaToken* tok = canvas[labelRow][col];
                bool occupied = tok->tokenType != _Print_Space || tok->tokenStr != " ";
                if (occupied && !isConnectorToken(tok->tokenType)) {
                    pathIsClear = false;
                    break;
                }
            }
            if (!pathIsClear)
                continue;

            projectedPath.clear();
            projectedPath.push_back(PathStep(CellPoint(labelRow, startCol)));
            for (int col = startCol + 1; col <= connectorEndCol; col++)
                projectedPath.push_back(PathStep(CellPoint(labelRow, col)));
            return true;
        }

        return false;
    }

    // Keep the original routed connector shape, but stop on the projected row instead
    // of continuing all the way down to the locked bottom label row.
    static int getProjectedConnectorPrefixEndIndex(const LabelPlacement& lockedPlacement, int projectedRow)
    {
        if (lockedPlacement.path.empty())
            return -1;

        for (int i = 0; i < (int)lockedPlacement.path.size(); i++) {
            const CellPoint& point = lockedPlacement.path[i].point;
            if (point.row == projectedRow)
                return i;
        }
        return -1;
    }

    static bool buildProjectedUnlockedPath(const PrintGrid& canvas, int projectedRow, int labelCol, const LabelPlacement& lockedPlacement, std::vector<PathStep>& projectedPath)
    {
        if (lockedPlacement.path.empty())
            return false;

        int prefixEndIndex = getProjectedConnectorPrefixEndIndex(lockedPlacement, projectedRow);
        if (prefixEndIndex == -1)
            return false;
        int connectorStartCol = lockedPlacement.path[prefixEndIndex].point.col;
        int connectorEndCol = labelCol - 1;
        if (connectorEndCol < connectorStartCol)
            return false;

        for (int col = connectorStartCol + 1; col <= connectorEndCol; col++) {
            asaToken* tok = canvas[projectedRow][col];
            bool occupied = tok->tokenType != _Print_Space || tok->tokenStr != " ";
            if (occupied && !isConnectorToken(tok->tokenType))
                return false;
        }

        projectedPath.assign(lockedPlacement.path.begin(), lockedPlacement.path.begin() + prefixEndIndex + 1);
        for (int col = connectorStartCol + 1; col <= connectorEndCol; col++)
            projectedPath.push_back(PathStep(CellPoint(projectedRow, col)));
        if ((int)projectedPath.size() < 4)
            return false;
        return true;
    }

    // Unlocked labels stay on the connector start row, but choose the first slot that
    // would be open on the locked label row without reserving it for later unlocked labels.
    static bool tryProjectHorizontalLabelPlacement(const RoutingCanvas& routing, const PlacedUnderline& item, const std::vector<int>& startCols, int messageLen, const LabelPlacement& lockedPlacement, LabelPlacement& projectedPlacement)
    {
        if (!item.verticallyUnlocked || lockedPlacement.score == INT_MAX)
            return false;
        if (lockedPlacement.row < 0 || lockedPlacement.row >= (int)routing.canvas.size())
            return false;

        int labelRow = getProjectedUnlockedRow(lockedPlacement);
        if (labelRow < 0 || labelRow >= (int)routing.canvas.size())
            return false;
        int projectedRowPadding = item.compactForcedUnlocked ? routing.connectorPadding : 0;
        if (labelRow == item.row) {
            for (int labelCol = routing.connectorMinCol; labelCol + messageLen - 1 <= routing.connectorMaxCol; labelCol++) {
                if (!labelFitsAt(routing.canvas, lockedPlacement.row, labelCol, messageLen, routing.connectorPadding))
                    continue;
                if (!labelFitsAt(routing.canvas, labelRow, labelCol, messageLen, projectedRowPadding))
                    continue;

                std::vector<PathStep> projectedPath;
                if (!buildDirectUnlockedPath(routing.canvas, labelRow, labelCol, startCols, projectedPath))
                    continue;

                projectedPlacement = lockedPlacement;
                projectedPlacement.row = labelRow;
                projectedPlacement.col = labelCol;
                projectedPlacement.path = std::move(projectedPath);
                projectedPlacement.isProjectedHorizontal = true;
                return true;
            }
        }
        else {
            int prefixEndIndex = getProjectedConnectorPrefixEndIndex(lockedPlacement, labelRow);
            if (prefixEndIndex == -1)
                return false;
            int connectorStartCol = lockedPlacement.path[prefixEndIndex].point.col;

            for (int labelCol = connectorStartCol + 3; labelCol + messageLen - 1 <= routing.connectorMaxCol; labelCol++) {
                if (!labelFitsAt(routing.canvas, labelRow, labelCol, messageLen, projectedRowPadding))
                    continue;

                std::vector<PathStep> projectedPath;
                if (!buildProjectedUnlockedPath(routing.canvas, labelRow, labelCol, lockedPlacement, projectedPath))
                    continue;

                projectedPlacement = lockedPlacement;
                projectedPlacement.row = labelRow;
                projectedPlacement.col = labelCol;
                projectedPlacement.path = std::move(projectedPath);
                projectedPlacement.isProjectedHorizontal = true;
                return true;
            }
        }

        return false;
    }

    // Fallback routing escapes to the right with a deterministic three-segment path.
    static std::vector<PathStep> buildForcedPath(CellPoint start, CellPoint goal, int escapeCol)
    {
        std::vector<PathStep> path;
        path.push_back(PathStep(start));

        int colStep = escapeCol >= start.col ? 1 : -1;
        for (int col = start.col + colStep; col != escapeCol + colStep; col += colStep)
            path.push_back(PathStep(CellPoint(start.row, col)));

        int rowStep = goal.row >= start.row ? 1 : -1;
        for (int row = start.row + rowStep; row != goal.row + rowStep; row += rowStep)
            path.push_back(PathStep(CellPoint(row, escapeCol)));

        if (goal.col != escapeCol) {
            int goalColStep = goal.col >= escapeCol ? 1 : -1;
            for (int col = escapeCol + goalColStep; col != goal.col + goalColStep; col += goalColStep)
                path.push_back(PathStep(CellPoint(goal.row, col)));
        }

        return path;
    }

    // Labels are painted only after a winning placement has been chosen.
    static void paintLabel(PrintGrid& canvas, std::deque<asaToken>& ownedTokens, int row, int col, const std::string& text, const std::string* color)
    {
        for (int i = 0; i < (int)text.size(); i++)
            canvas[row][col + i] = makeToken(ownedTokens, std::string(1, text[i]), _Print_Message, color, 1);
    }

    // Used to seed the fallback routing area to the right of existing content.
    static int getRightmostOccupiedCol(const PrintGrid& canvas)
    {
        int col = 0;
        for (int r = 0; r < (int)canvas.size(); r++)
            for (int c = (int)canvas[r].size() - 1; c >= 0; c--)
                if (canvasCellOccupied(canvas, r, c)) {
                    col = std::max(col, c);
                    break;
                }
        return col;
    }

    // Routing may allocate extra blank rows that should not reach the final renderer.
    static void trimTrailingEmptyRows(PrintGrid& grid)
    {
        while (!grid.empty()) {
            bool hasContent = false;
            for (asaToken* tok : grid.back())
                if (tok->tokenType != _Print_Space || tok->tokenStr != " ") {
                    hasContent = true;
                    break;
                }
            if (hasContent)
                break;
            grid.pop_back();
        }
    }

    // Base row for one label after respecting anchor rows and trailing underline constraints.
    static int labelBaseRowForItem(const PlacedUnderline& item, const std::map<int, int>& lineToGridRow)
    {
        int preferredLabelBaseRow = item.row + (item.startsOnSourceRow ? 1 : 2);
        auto it = lineToGridRow.find(item.labelAfterSrcLine);
        if (it != lineToGridRow.end())
            preferredLabelBaseRow = std::max(preferredLabelBaseRow, it->second + (item.startsOnSourceRow ? 2 : 3));
        return std::max(preferredLabelBaseRow, item.minLabelRow);
    }

    // Search a single candidate label row for the best goal column and path.
    static void tryRouteLabelOnRow(RoutingCanvas& routing, std::deque<asaToken>& ownedTokens, const PlacedUnderline& item, const std::vector<int>& startCols, int messageLen, int labelRow, bool allowConnectorReuse, LabelPlacement& bestPlacement)
    {
        ensureCanvasSize(routing.canvas, ownedTokens, labelRow + 1, routing.connectorMaxCol + 4);
        for (int goalCol = routing.connectorMinCol + routing.connectorPadding; goalCol <= routing.connectorMaxCol; goalCol++) {
            int labelCol = goalCol - routing.connectorPadding;
            if (labelCol < routing.connectorMinCol)
                continue;
            if (labelCol + messageLen - 1 > routing.connectorMaxCol)
                continue;
            if (!labelFitsAt(routing.canvas, labelRow, labelCol, messageLen, routing.connectorPadding))
                continue;

            CellPoint goal(labelRow - 1, goalCol);
            for (int startCol : startCols) {
                std::vector<PathStep> path;
                if (!findPath(routing.canvas, routing.rows, CellPoint(item.row, startCol), goal, allowConnectorReuse, path))
                    continue;

                int score = scoreLabelPath(item, labelCol, messageLen, goalCol, path);
                if (score < bestPlacement.score) {
                    bestPlacement.row = labelRow;
                    bestPlacement.col = labelCol;
                    bestPlacement.score = score;
                    bestPlacement.path = std::move(path);
                }
            }
        }
    }

    // Attachment columns prefer padded interior points so connectors do not hug underline edges.
    static std::vector<int> collectAttachmentColumns(const PlacedUnderline& item, int padding)
    {
        std::vector<int> cols;
        if (item.underlineSegment && item.underlineSegment->renderStyle == Render_Arrow) {
            cols.push_back(item.startCol);
            return cols;
        }
        int width = item.endCol - item.startCol + 1;
        if (width <= 0)
            return cols;
        int first = width <= padding * 2 ? item.startCol : item.startCol + padding;
        int last = width <= padding * 2 ? item.endCol : item.endCol - padding;
        if (width <= padding * 2) {
            // Short underlines prefer the best available interior padding.
            int bestPad = -1;
            for (int col = first; col <= last; col++) {
                int availablePad = std::min(col - item.startCol, item.endCol - col);
                bestPad = std::max(bestPad, availablePad);
            }
            for (int col = first; col <= last; col++) {
                int availablePad = std::min(col - item.startCol, item.endCol - col);
                if (availablePad == bestPad)
                    cols.push_back(col);
            }
        }
        else {
            // Wide underlines can use any fully padded attachment point.
            for (int col = first; col <= last; col++)
                cols.push_back(col);
        }
        return cols;
    }

    // Build per-row routing metadata once from the post-underline source grid.
    static void buildRoutingState(RoutingCanvas& routing, const PrintGrid& grid, std::deque<asaToken>& ownedTokens, const std::vector<PlacedUnderline>& orderedPlaced, int renderMaxWidth, const std::map<int, int>& lineToGridRow)
    {
        routing.canvas = expandGridToCanvas(grid, ownedTokens, 0);
        routing.rows.assign(routing.canvas.size(), {});

        for (int i = 0; i < (int)grid.size(); i++) {
            RoutingRowState& rowState = routing.rows[i];
            if (rowContainsSourceTokens(grid[i])) {
                rowState.hasSource = true;
                rowState.isTruncated = rowHasEllipsis(grid[i]);
                rowState.rightEdge = getRowDisplayWidth(grid[i]) - 1;
            }
            for (asaToken* tok : grid[i])
                if (tok->tokenType == _Print_Underline) {
                    rowState.hasUnderline = true;
                    break;
                }
        }

        routing.farRight = getRightmostOccupiedCol(routing.canvas);

        int totalMessageWidth = 0;
        int maxPreferredLabelBaseRow = 0;
        for (const PlacedUnderline& item : orderedPlaced) {
            if (item.underlineSegment)
                totalMessageWidth += (int)item.underlineSegment->underlineMessage.size();
            maxPreferredLabelBaseRow = std::max(maxPreferredLabelBaseRow, labelBaseRowForItem(item, lineToGridRow));
        }

        routing.connectorMaxCol = std::max(routing.connectorMinCol + 8,
            std::max(renderMaxWidth + 7,
                routing.farRight + totalMessageWidth + (int)orderedPlaced.size() * (routing.connectorPadding * 4 + 4)));
        ensureCanvasSize(routing.canvas, ownedTokens, maxPreferredLabelBaseRow + (int)orderedPlaced.size() * 6 + 8, routing.connectorMaxCol + 4);
        routing.rows.resize(routing.canvas.size());
    }

    // Try shared-row reuse first, then search downward, then fall back to a forced escape path.
    static LabelPlacement findBestLabelPlacement(RoutingCanvas& routing, std::deque<asaToken>& ownedTokens, const PlacedUnderline& item, const std::vector<int>& startCols, const std::map<int, int>& lineToGridRow, bool allowConnectorReuse, int orderedPlacedCount, int sharedLabelRow)
    {
        LabelPlacement bestPlacement;
        int messageLen = (int)item.underlineSegment->underlineMessage.size();
        int preferredLabelBaseRow = labelBaseRowForItem(item, lineToGridRow);

        if (sharedLabelRow != -1)
            tryRouteLabelOnRow(routing, ownedTokens, item, startCols, messageLen, sharedLabelRow, allowConnectorReuse, bestPlacement);
        if (bestPlacement.score == INT_MAX) {
            int searchStartRow = std::max(preferredLabelBaseRow, 0);
            ensureCanvasSize(routing.canvas, ownedTokens, searchStartRow + orderedPlacedCount * 6 + 8, routing.connectorMaxCol + 4);
            for (int labelRow = searchStartRow; labelRow < (int)routing.canvas.size(); labelRow++) {
                tryRouteLabelOnRow(routing, ownedTokens, item, startCols, messageLen, labelRow, allowConnectorReuse, bestPlacement);
                if (bestPlacement.score != INT_MAX)
                    break;
            }
        }

        if (bestPlacement.score == INT_MAX) {
            int fallbackEscapeCol = std::max(routing.farRight + routing.connectorPadding + messageLen + 6, item.endCol + routing.connectorPadding + 4);
            int fallbackLabelRowStart = std::max(preferredLabelBaseRow, sharedLabelRow != -1 ? sharedLabelRow : preferredLabelBaseRow);
            routing.connectorMaxCol = std::max(routing.connectorMaxCol, fallbackEscapeCol + routing.connectorPadding + 4);
            ensureCanvasSize(routing.canvas, ownedTokens, fallbackLabelRowStart + 8, routing.connectorMaxCol + 4);

            for (int labelRow = fallbackLabelRowStart; labelRow < (int)routing.canvas.size(); labelRow++) {
                int labelCol = fallbackEscapeCol - routing.connectorPadding;
                if (!labelFitsAt(routing.canvas, labelRow, labelCol, messageLen, routing.connectorPadding))
                    continue;

                CellPoint goal(labelRow - 1, fallbackEscapeCol);
                int startCol = startCols.empty() ? item.endCol : startCols.front();
                std::vector<PathStep> path = buildForcedPath(CellPoint(item.row, startCol), goal, fallbackEscapeCol);
                bestPlacement.row = labelRow;
                bestPlacement.col = labelCol;
                bestPlacement.score = scoreLabelPath(item, labelCol, messageLen, fallbackEscapeCol, path);
                bestPlacement.path = std::move(path);
                break;
            }
        }

        LabelPlacement projectedPlacement;
        if (tryProjectHorizontalLabelPlacement(routing, item, startCols, messageLen, bestPlacement, projectedPlacement))
            return projectedPlacement;

        return bestPlacement;
    }

    // Commit the chosen path and label text back onto the routing canvas.
    static void commitLabelPlacement(RoutingCanvas& routing, std::deque<asaToken>& ownedTokens, const PlacedUnderline& item, const LabelPlacement& placement)
    {
        int messageLen = (int)item.underlineSegment->underlineMessage.size();
        paintPath(routing.canvas, ownedTokens, placement.path, &item.underlineSegment->underlineColor, item.underlineSegment);
        paintLabel(routing.canvas, ownedTokens, placement.row, placement.col, item.underlineSegment->underlineMessage, &item.underlineSegment->underlineColor);
        routing.farRight = std::max(routing.farRight, placement.col + messageLen);
    }

    // Final label-routing pass after all source and underline rows are fixed in place.
    static void routeUnderlineMessages(PrintGrid& grid, std::deque<asaToken>& ownedTokens, const std::vector<PlacedUnderline>& placed, const std::map<int, int>& lineToGridRow, int renderMaxWidth)
    {
        if (placed.empty())
            return;

        std::vector<PlacedUnderline> orderedPlaced = placed;
        std::sort(orderedPlaced.begin(), orderedPlaced.end(), [](const PlacedUnderline& a, const PlacedUnderline& b) {
            if (a.row != b.row)
                return a.row > b.row;
            if (a.startCol != b.startCol)
                return a.startCol < b.startCol;
            return a.endCol < b.endCol;
        });

        // Labels are placed after source and underline rows are finalized, then routed on a cell canvas.
        RoutingCanvas routing;
        buildRoutingState(routing, grid, ownedTokens, orderedPlaced, renderMaxWidth, lineToGridRow);
        int sharedLabelRow = -1;
        for (const PlacedUnderline& item : orderedPlaced) {
            if (!item.underlineSegment || item.underlineSegment->underlineMessage.empty())
                continue;

            bool allowConnectorReuse = false;
            if (item.startsOnSourceRow) {
                int sameRowArrowCount = 0;
                for (const PlacedUnderline& other : orderedPlaced)
                    if (other.startsOnSourceRow && other.row == item.row)
                        sameRowArrowCount++;
                allowConnectorReuse = sameRowArrowCount > 1;
            }
            std::vector<int> startCols = collectAttachmentColumns(item, routing.connectorPadding);
            if (startCols.empty())
                startCols.push_back(item.endCol);

            LabelPlacement bestPlacement = findBestLabelPlacement(routing, ownedTokens, item, startCols, lineToGridRow, allowConnectorReuse, (int)orderedPlaced.size(), sharedLabelRow);

            if (bestPlacement.score != INT_MAX) {
                commitLabelPlacement(routing, ownedTokens, item, bestPlacement);
                if (!bestPlacement.isProjectedHorizontal)
                    sharedLabelRow = bestPlacement.row;
            }
        }

        trimTrailingEmptyRows(routing.canvas);
        grid = std::move(routing.canvas);
    }

    // Record one underline's absolute span and allocate an anchor row if it owns a label.
    static int finalizeUnderlinePlacement(PrintGrid& grid, std::deque<asaToken>& ownedTokens, std::map<int, int>& lineToGridRow, std::vector<PlacedUnderline>& placed, const UnderlinedSegment* underlineSegment, int lineNumWidth, int landingRow, int underlineCol, int underlineLen, bool recordPlacement, int labelAfterSrcLine)
    {
        int minLabelRow = landingRow + 2;
        if (!recordPlacement) {
            updatePlacedUnderlineMinLabelRow(placed, underlineSegment, minLabelRow);
            return landingRow;
        }

        int absStartCol = lineNumWidth + 4 + underlineCol;
        int absEndCol = lineNumWidth + 4 + underlineCol + std::max(1, underlineLen) - 1;
        int recordRow = landingRow;
        if (!underlineSegment->underlineMessage.empty())
            recordRow = insertAnchorRow(grid, ownedTokens, lineToGridRow, placed, lineNumWidth, landingRow, absStartCol, absEndCol);
        placed.push_back({underlineSegment, recordRow, absStartCol, absEndCol, false, underlineSegment->verticallyUnlocked || compact, compact && !underlineSegment->verticallyUnlocked, labelAfterSrcLine, minLabelRow});
        return landingRow;
    }

    // Inserts underline chars for `underlineSegment` at content column `underlineCol` (length `underlineLen`) after
    // the source row for `srcLine`. Three cases:
    //   - Next row is an ellipsis: replace the "..." with the underline chars inline.
    //   - Next row is the immediately following source line and underlineCol >= its content width:
    //     append underline chars directly to that row.
    //   - Otherwise: insert a new blank annotation row after the source row.
    // Returns the grid row index where the annotation landed, or -1 if srcLine is not in the grid.
    static int insertAnnotation(PrintGrid& grid, std::deque<asaToken>& ownedTokens, std::map<int, int>& lineToGridRow, std::vector<PlacedUnderline>& placed, const UnderlinedSegment* underlineSegment, int lineNumWidth, int srcLine, int underlineCol, int underlineLen, bool recordPlacement, int labelAfterSrcLine, int renderMaxWidth)
    {
        if (!lineToGridRow.count(srcLine))
            return -1;
        int srcRow = lineToGridRow[srcLine];
        int landingRow = srcRow + 1;
        auto finalizeOnRow = [&](PrintRow& row, int rowIndex, int currentWidth) {
            appendUnderlineAnnotation(row, ownedTokens, underlineSegment, underlineCol, underlineLen, currentWidth);
            return finalizeUnderlinePlacement(grid, ownedTokens, lineToGridRow, placed, underlineSegment, lineNumWidth, rowIndex, underlineCol, underlineLen, recordPlacement, labelAfterSrcLine);
        };

        // Gap rows may absorb an underline so the underline visually bridges the hidden lines.
        if (srcRow + 1 < (int)grid.size()) {
            bool nextIsEllipsis = false;
            for (asaToken* tok : grid[srcRow + 1])
                if (tok->tokenType == _Print_Ellipsis) {
                    nextIsEllipsis = true;
                    break;
                }
            if (nextIsEllipsis) {
                PrintRow& ellipsisRow = grid[srcRow + 1];
                ellipsisRow.erase(std::remove_if(ellipsisRow.begin(), ellipsisRow.end(),
                                      [](asaToken* t) { return t->tokenType == _Print_Ellipsis; }),
                    ellipsisRow.end());
                return finalizeOnRow(ellipsisRow, srcRow + 1, getRowContentWidth(ellipsisRow));
            }
        }

        // Same-line underlines may share one annotation row if they do not overlap.
        if (srcRow + 1 < (int)grid.size() && rowHasUnderlinesOnly(grid[srcRow + 1])) {
            PrintRow& annotRow = grid[srcRow + 1];
            int annotWidth = getRowContentWidth(annotRow);
            if (underlineCol >= annotWidth)
                return finalizeOnRow(annotRow, srcRow + 1, annotWidth);
        }

        // Very short following source rows can host the underline inline if there is horizontal space.
        if (lineToGridRow.count(srcLine + 1) && lineToGridRow[srcLine + 1] == srcRow + 1) {
            PrintRow& nextRow = grid[srcRow + 1];
            if (rowContainsSourceTokens(nextRow) && !rowHasEllipsis(nextRow)) {
                int nextWidth = getRowContentWidth(nextRow);
                if (nextWidth <= 4 && nextWidth < renderMaxWidth && underlineCol < renderMaxWidth && underlineCol >= nextWidth)
                    return finalizeOnRow(nextRow, srcRow + 1, nextWidth);
            }
        }

        // Otherwise the underline gets its own dedicated row directly below the source row.
        PrintRow annotRow;
        annotRow.push_back(makeToken(ownedTokens, std::string(lineNumWidth, ' ') + " |  ", _Print_Gutter));
        appendUnderlineAnnotation(annotRow, ownedTokens, underlineSegment, underlineCol, underlineLen, 0);
        grid.insert(grid.begin() + srcRow + 1, std::move(annotRow));
        shiftPlacedUnderlineRows(placed, srcRow);
        shiftLineToGridRows(lineToGridRow, srcRow);
        landingRow = srcRow + 1;
        return finalizeUnderlinePlacement(grid, ownedTokens, lineToGridRow, placed, underlineSegment, lineNumWidth, landingRow, underlineCol, underlineLen, recordPlacement, labelAfterSrcLine);
    }

    // Expand one underline/arrow render request into concrete underline rows and attachment records.
    static void renderUnderlinePlacement(const RenderUnderline& renderUnderline, int minIndent, int lineNumWidth, int labelAfterLine, int renderMaxWidth, PrintGrid& grid, std::deque<asaToken>& ownedTokens, std::map<int, int>& lineToGridRow, std::vector<PlacedUnderline>& placedUnderlines)
    {
        if (!isRenderableSegment(renderUnderline.underlineSegment))
            return;

        int focusStart = renderUnderline.startToken->lineNumber;
        int focusEnd = renderUnderline.endToken->lineNumber;
        if (isArrowSegment(renderUnderline.underlineSegment)) {
            if (lineToGridRow.count(focusStart)) {
                int srcRow = lineToGridRow[focusStart];
                int startCol = getRowDisplayWidth(grid[srcRow]) + 2;
                placedUnderlines.push_back({renderUnderline.underlineSegment, srcRow, startCol, startCol, true, renderUnderline.underlineSegment->verticallyUnlocked || compact, compact && !renderUnderline.underlineSegment->verticallyUnlocked, labelAfterLine, srcRow + 2});
            }
            return;
        }

        auto placeTokenSpan = [&](asaToken* token, bool recordPlacement) {
            int underlineCol = computeVisCol(token, minIndent);
            int underlineLen = (int)token->length;
            insertAnnotation(grid, ownedTokens, lineToGridRow, placedUnderlines, renderUnderline.underlineSegment, lineNumWidth, token->lineNumber, underlineCol, underlineLen, recordPlacement, labelAfterLine, renderMaxWidth);
        };

        if (focusStart != focusEnd) {
            // Multi-line spans draw a start underline and an end underline.
            placeTokenSpan(renderUnderline.startToken, true);
            placeTokenSpan(renderUnderline.endToken, false);
            return;
        }

        // Single-line spans underline the full token range on one line.
        int underlineCol = computeVisCol(renderUnderline.startToken, minIndent);
        int underlineLen = std::max(1, computeVisCol(renderUnderline.endToken, minIndent) + (int)renderUnderline.endToken->length - underlineCol);
        insertAnnotation(grid, ownedTokens, lineToGridRow, placedUnderlines, renderUnderline.underlineSegment, lineNumWidth, focusEnd, underlineCol, underlineLen, true, labelAfterLine, renderMaxWidth);
    }

    // Pass 1: collect token ranges for all renderable annotations.
    static std::vector<RenderUnderline> collectRenderableUnderlines(const std::vector<UnderlinedSegment>& underlines)
    {
        std::vector<RenderUnderline> renderUnderlines;
        for (const auto& underlineSegment : underlines) {
            if (!underlineSegment.focusedNode)
                continue;
            tokenRange tokenRangeForSegment = getASTTokenRange(underlineSegment.focusedNode);
            if (!tokenRangeForSegment.first || !tokenRangeForSegment.first->lineValue)
                continue;
            renderUnderlines.push_back({&underlineSegment, tokenRangeForSegment.first, tokenRangeForSegment.second});
        }
        return renderUnderlines;
    }

    // Diagnostics are rendered per file; mixed-file messages recurse into per-file sub-segments.
    static bool printSplitDiagnosticsByFile(const std::vector<RenderUnderline>& renderUnderlines, const std::vector<ASTNode*>& contextNodes)
    {
        if (renderUnderlines.empty())
            return false;

        for (int i = 1; i < (int)renderUnderlines.size(); i++)
            if (renderUnderlines[i].startToken->filePath != renderUnderlines[0].startToken->filePath) {
                std::vector<std::string*> seenFiles;
                for (const RenderUnderline& renderUnderline : renderUnderlines) {
                    bool found = false;
                    for (auto* fp : seenFiles)
                        if (fp == renderUnderline.startToken->filePath || *fp == *renderUnderline.startToken->filePath) {
                            found = true;
                            break;
                        }
                    if (!found)
                        seenFiles.push_back(renderUnderline.startToken->filePath);
                }
                for (auto* filePtr : seenFiles) {
                    SourceCodeSegment sub;
                    for (ASTNode* contextNode : contextNodes) {
                        tokenRange contextRange = getASTTokenRange(contextNode);
                        if (contextRange.first && contextRange.first->filePath == filePtr)
                            sub.addContextNode(contextNode);
                    }
                    for (const RenderUnderline& renderUnderline : renderUnderlines)
                        if (renderUnderline.startToken->filePath == filePtr)
                            sub.addUnderline(*renderUnderline.underlineSegment);
                    sub.print();
                }
                return true;
            }

        return false;
    }

    // Pass 1: determine visible lines, relative indentation, and effective content width.
    static bool computeVisibleSourceWindow(const std::vector<RenderUnderline>& renderUnderlines, const std::vector<ASTNode*>& contextNodes, SourceRenderWindow& window)
    {
        if (renderUnderlines.empty())
            return false;

        int globalFocusStart = INT_MAX;
        int globalFocusEnd = -1;
        for (const RenderUnderline& renderUnderline : renderUnderlines) {
            globalFocusStart = std::min(globalFocusStart, renderUnderline.startToken->lineNumber);
            globalFocusEnd = std::max(globalFocusEnd, renderUnderline.endToken->lineNumber);
        }

        std::set<int> visibleSet;
        for (const RenderUnderline& renderUnderline : renderUnderlines) {
            int focusStart = renderUnderline.startToken->lineNumber;
            int focusEnd = renderUnderline.endToken->lineNumber;
            int above = (int)renderUnderline.underlineSegment->aboveContextLines;
            int below = (int)renderUnderline.underlineSegment->belowContextLines;
            if (renderUnderline.underlineSegment->printFlags & Hide_Scope_Body_Contents) {
                for (int i = std::max(0, focusStart - above); i <= focusStart; i++)
                    visibleSet.insert(i);
                for (int i = focusEnd; i <= focusEnd + below; i++)
                    visibleSet.insert(i);
            }
            else {
                for (int i = std::max(0, focusStart - above); i <= focusEnd + below; i++)
                    visibleSet.insert(i);
            }
        }

        for (ASTNode* ctxNode : contextNodes) {
            if (!ctxNode)
                continue;
            ASTNode* ancestorNode = ctxNode;
            while (ancestorNode && ancestorNode->parentNode) {
                tokenRange ancestorRange = getASTTokenRange(ancestorNode);
                if (ancestorRange.first && ancestorRange.second && ancestorRange.first->lineValue) {
                    int pStart = ancestorRange.first->lineNumber;
                    int pEnd = ancestorRange.second->lineNumber;
                    if (pStart < globalFocusStart || pEnd > globalFocusEnd) {
                        visibleSet.insert(pStart);
                        visibleSet.insert(pEnd);
                        break;
                    }
                }
                ancestorNode = ancestorNode->parentNode;
            }
        }

        for (ASTNode* ctxNode : contextNodes) {
            if (!ctxNode)
                continue;
            tokenRange contextRange = getASTTokenRange(ctxNode);
            if (!contextRange.first || !contextRange.second)
                continue;
            int ctxStart = contextRange.first->lineNumber;
            int ctxEnd = contextRange.second->lineNumber;
            for (const RenderUnderline& renderUnderline : renderUnderlines) {
                if ((renderUnderline.underlineSegment->printFlags & Hide_Scope_Body_Contents) &&
                    ctxStart > renderUnderline.startToken->lineNumber && ctxEnd < renderUnderline.endToken->lineNumber) {
                    for (int i = ctxStart; i <= ctxEnd; i++)
                        visibleSet.insert(i);
                    break;
                }
            }
        }

        if (visibleSet.empty())
            return false;

        int minVisible = *visibleSet.begin();
        window.maxVisible = *visibleSet.rbegin();
        window.renderFilePath = renderUnderlines[0].startToken->filePath;
        window.lineValues = collectSourceLines(minVisible, window.maxVisible, window.renderFilePath);
        window.indents = collectLineIndents(minVisible, window.maxVisible, window.renderFilePath);

        for (const RenderUnderline& renderUnderline : renderUnderlines) {
            window.lineValues[renderUnderline.startToken->lineNumber] = renderUnderline.startToken->lineValue;
            window.lineValues[renderUnderline.endToken->lineNumber] = renderUnderline.endToken->lineValue;
            window.indents[renderUnderline.startToken->lineNumber] = renderUnderline.startToken->lineIndent;
            window.indents[renderUnderline.endToken->lineNumber] = renderUnderline.endToken->lineIndent;
        }

        while (!visibleSet.empty() && !window.lineValues.count(*visibleSet.begin()))
            visibleSet.erase(visibleSet.begin());
        while (!visibleSet.empty() && !window.lineValues.count(*visibleSet.rbegin()))
            visibleSet.erase(std::prev(visibleSet.end()));
        if (visibleSet.empty())
            return false;

        int minIndent = INT_MAX;
        for (int ln : visibleSet)
            if (window.indents.count(ln))
                minIndent = std::min(minIndent, window.indents[ln]);
        window.minIndent = minIndent == INT_MAX ? 0 : minIndent;

        window.maxVisible = *visibleSet.rbegin();
        window.lineNumWidth = std::max(3, (int)std::to_string(window.maxVisible + 1).size());

        int indentWidth = (int)console::indentation * 4;
        int gutterWidth = window.lineNumWidth + 4;
        int terminalWidth = console::getTerminalWidth();
        int maxUsefulWidth = indentWidth + gutterWidth + MESSAGE_RENDER_FALLBACK_WIDTH;
        window.renderMaxWidth = MESSAGE_RENDER_FALLBACK_WIDTH;
        if (terminalWidth < maxUsefulWidth)
            window.renderMaxWidth = terminalWidth - indentWidth - gutterWidth;
        window.renderMaxWidth = std::max(MESSAGE_RENDER_MIN_CONTENT_WIDTH, window.renderMaxWidth);

        window.sortedLines.assign(visibleSet.begin(), visibleSet.end());
        window.labelAfterLine = window.sortedLines.empty() ? window.maxVisible : window.sortedLines.back();

        std::vector<FocusRange> ranges;
        for (const RenderUnderline& renderUnderline : renderUnderlines) {
            if (!isUnderlineSegment(renderUnderline.underlineSegment))
                continue;
            ranges.push_back({renderUnderline.startToken->lineNumber, renderUnderline.endToken->lineNumber});
        }
        std::sort(ranges.begin(), ranges.end(), [](const FocusRange& a, const FocusRange& b) {
            if (a.start != b.start)
                return a.start < b.start;
            return a.end < b.end;
        });
        for (int i = 0; i + 1 < (int)ranges.size(); i++)
            if (ranges[i].end < ranges[i + 1].start)
                window.separatorAfterLines.insert(ranges[i].end);

        return true;
    }

    // Pass 2: build the source rows and gap rows before any underline rows are inserted.
    static SourceGridLayout buildSourceGrid(const std::vector<RenderUnderline>& renderUnderlines, const SourceRenderWindow& window, std::deque<asaToken>& ownedTokens)
    {
        SourceGridLayout layout;

        for (int lineIdx = 0; lineIdx < (int)window.sortedLines.size(); lineIdx++) {
            int lineNum = window.sortedLines[lineIdx];

            if (lineIdx > 0 && lineNum - window.sortedLines[lineIdx - 1] > 1) {
                PrintRow ellipsisRow;
                ellipsisRow.push_back(makeToken(ownedTokens, std::string(window.lineNumWidth, '.') + " |  ", _Print_Gutter_Ellipsis));
                ellipsisRow.push_back(makeToken(ownedTokens, "...", _Print_Ellipsis));
                layout.grid.push_back(std::move(ellipsisRow));
            }

            const std::string* markerColor = nullptr;
            for (const RenderUnderline& renderUnderline : renderUnderlines)
                if (isRenderableSegment(renderUnderline.underlineSegment) && renderUnderline.startToken->lineNumber == lineNum) {
                    markerColor = &renderUnderline.underlineSegment->underlineColor;
                    break;
                }

            PrintRow row;
            if (markerColor) {
                row.push_back(makeToken(ownedTokens, formatLineNum(lineNum, window.lineNumWidth) + " ", _Print_Gutter));
                row.push_back(makeToken(ownedTokens, ">", _Print_Gutter_Marker, markerColor));
                row.push_back(makeToken(ownedTokens, "  ", _Print_Gutter));
            }
            else {
                row.push_back(makeToken(ownedTokens, formatLineNum(lineNum, window.lineNumWidth) + " |  ", _Print_Gutter));
            }

            int lineIndent = window.indents.count(lineNum) ? window.indents.at(lineNum) : window.minIndent;
            int relativeIndent = std::max(0, lineIndent - window.minIndent);

            auto lineValIt = window.lineValues.find(lineNum);
            if (lineValIt != window.lineValues.end() && lineValIt->second) {
                std::string* lineVal = lineValIt->second;
                int leadingSpaces = countLeadingSpaces(*lineVal);
                bool truncated = (relativeIndent + (int)lineVal->size() - leadingSpaces > window.renderMaxWidth);
                int maxContentCol = truncated ? (window.renderMaxWidth - 4 - relativeIndent) : INT_MAX;

                if (relativeIndent > 0)
                    row.push_back(makeToken(ownedTokens, std::string(relativeIndent, ' '), _Print_Space));

                std::vector<asaToken*> lineTokens;
                for (auto* tok : allTokens) {
                    if (tok->lineNumber != lineNum)
                        continue;
                    if (window.renderFilePath && tok->filePath != window.renderFilePath)
                        continue;
                    if (tok->tokenType == EndOfLine)
                        continue;
                    lineTokens.push_back(tok);
                }
                std::sort(lineTokens.begin(), lineTokens.end(),
                    [](asaToken* a, asaToken* b) { return a->indexInLine < b->indexInLine; });

                int currentCol = 0;
                for (asaToken* tok : lineTokens) {
                    int dispCol = (tok->indexInLine - 1) - leadingSpaces;
                    int tokDisplayWidth = (int)tok->tokenStr.size();
                    if (dispCol < 0 || dispCol < currentCol)
                        continue;
                    if (dispCol >= maxContentCol)
                        break;
                    if (dispCol > currentCol)
                        row.push_back(makeToken(ownedTokens, std::string(dispCol - currentCol, ' '), _Print_Space));
                    int remainingWidth = maxContentCol - dispCol;
                    if (remainingWidth <= 0)
                        break;
                    if (tokDisplayWidth > remainingWidth) {
                        row.push_back(makeSlicedToken(ownedTokens, tok, 0, remainingWidth));
                        currentCol = dispCol + remainingWidth;
                        truncated = true;
                        break;
                    }
                    row.push_back(tok);
                    currentCol = dispCol + tokDisplayWidth;
                    if (currentCol >= maxContentCol) {
                        truncated = true;
                        break;
                    }
                }

                if (truncated) {
                    int clampedCol = std::min(currentCol, maxContentCol);
                    if (clampedCol < maxContentCol)
                        row.push_back(makeToken(ownedTokens, std::string(maxContentCol - clampedCol, ' '), _Print_Space));
                    row.push_back(makeToken(ownedTokens, " ...", _Print_Truncation_Ellipsis));
                }
            }

            layout.lineToGridRow[lineNum] = (int)layout.grid.size();
            layout.grid.push_back(std::move(row));

            if (window.separatorAfterLines.count(lineNum)) {
                PrintRow spacerRow;
                spacerRow.push_back(makeToken(ownedTokens, std::string(window.lineNumWidth, ' ') + " |", _Print_Gutter));
                layout.grid.push_back(std::move(spacerRow));
            }
        }

        return layout;
    }

    // Pass 3: insert underline rows and collect label attachment geometry.
    static std::vector<PlacedUnderline> placeUnderlines(const std::vector<RenderUnderline>& renderUnderlines, const SourceRenderWindow& window, SourceGridLayout& layout, std::deque<asaToken>& ownedTokens)
    {
        std::vector<PlacedUnderline> placedUnderlines;
        std::vector<uint8_t> processed(renderUnderlines.size(), 0);
        for (int i = 0; i < (int)renderUnderlines.size(); i++) {
            if (processed[i] || !isRenderableSegment(renderUnderlines[i].underlineSegment))
                continue;

            int focusStart = renderUnderlines[i].startToken->lineNumber;
            int focusEnd = renderUnderlines[i].endToken->lineNumber;
            if (isArrowSegment(renderUnderlines[i].underlineSegment) || focusStart != focusEnd) {
                renderUnderlinePlacement(renderUnderlines[i], window.minIndent, window.lineNumWidth, window.labelAfterLine, window.renderMaxWidth, layout.grid, ownedTokens, layout.lineToGridRow, placedUnderlines);
                processed[i] = 1;
                continue;
            }

            std::vector<int> group;
            for (int j = i; j < (int)renderUnderlines.size(); j++) {
                if (processed[j] || !isRenderableSegment(renderUnderlines[j].underlineSegment))
                    continue;
                if (isArrowSegment(renderUnderlines[j].underlineSegment))
                    continue;
                if (renderUnderlines[j].startToken->filePath != renderUnderlines[i].startToken->filePath)
                    continue;
                if (renderUnderlines[j].startToken->lineNumber != focusStart || renderUnderlines[j].endToken->lineNumber != focusEnd)
                    continue;
                group.push_back(j);
            }
            std::sort(group.begin(), group.end(), [&](int a, int b) {
                return computeVisCol(renderUnderlines[a].startToken, window.minIndent) < computeVisCol(renderUnderlines[b].startToken, window.minIndent);
            });
            for (int idx : group) {
                renderUnderlinePlacement(renderUnderlines[idx], window.minIndent, window.lineNumWidth, window.labelAfterLine, window.renderMaxWidth, layout.grid, ownedTokens, layout.lineToGridRow, placedUnderlines);
                processed[idx] = 1;
            }
        }

        return placedUnderlines;
    }

    // Pass 5: emit the final header and rendered grid.
    static void printFinalGrid(const PrintGrid& grid, const SourceRenderWindow& window)
    {
        console::applyIndent();
        console::write("\n --> ", GUTTER_COLOR);
        console::write(truncatePath(window.renderFilePath ? *window.renderFilePath : ""), GUTTER_COLOR);
        console::write("\n");
        console::applyIndent();
        console::write(std::string(window.lineNumWidth, ' ') + " |", GUTTER_COLOR);
        console::write("\n");
        renderGrid(grid);
    }
    // End region: diagnostic renderer layout and routing helpers

    void SourceCodeSegment::print() const
    {
        if (underlines.empty())
            return;

        std::vector<RenderUnderline> renderUnderlines = collectRenderableUnderlines(underlines);
        if (renderUnderlines.empty())
            return;

        if (printSplitDiagnosticsByFile(renderUnderlines, contextNodes))
            return;

        SourceRenderWindow window;
        if (!computeVisibleSourceWindow(renderUnderlines, contextNodes, window))
            return;
        std::deque<asaToken> ownedTokens;
        SourceGridLayout layout = buildSourceGrid(renderUnderlines, window, ownedTokens);
        std::vector<PlacedUnderline> placedUnderlines = placeUnderlines(renderUnderlines, window, layout, ownedTokens);
        routeUnderlineMessages(layout.grid, ownedTokens, placedUnderlines, layout.lineToGridRow, window.renderMaxWidth);
        printFinalGrid(layout.grid, window);
    }


}  // namespace messageSystem
