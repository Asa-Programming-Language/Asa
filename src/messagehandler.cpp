
#include "messagehandler.h"

#include <climits>
#include <set>

namespace messageSystem {

    //MessageBlockNode* errorTree = nullptr;
    MessageBlockNode* currentNode = nullptr;

    bool suppressErrors = false;
    bool exitOnError = true;

    // Value which stores if it has printed a message before.
    // Makes spacing better if it has
    bool hasPrintedMessage = false;

    static const std::string GUTTER_COLOR = console::yellowFGColor;
    static const std::string CONTEXT_INFO_COLOR = console::whiteFGColor;
    static const std::string NEUTRAL_HIGHLIGHT_COLOR = console::cyanFGColor;
    static const std::string ERROR_COLOR = console::redFGColor;
    static const std::string WARNING_COLOR = console::brightYellowFGColor;


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
        console::writeLine("Fatal Error: " + messageString, ERROR_COLOR);
        console::writeLine("Last added node:     Line: " + std::to_string(currentNode->sourceLine) + " | " + currentNode->sourcePath, ERROR_COLOR);
    }

    void startBlock(ASTNode* node, std::string messageString, const char* sourceFunction, uint32_t sourceLine, const char* sourcePath, BlockType blockType)
    {
        if (currentNode == nullptr) {
            currentNode = new MessageBlockNode(node, messageString, sourceFunction, sourceLine, sourcePath, blockType);
        }
        else {
            if (currentNode->blockType != blockType) {
                printMessageSystemError("New message block type started before old type completed.");
                console::writeLine("Current added node:  Line: " + std::to_string(sourceLine) + " | " + sourcePath, ERROR_COLOR);
                console::writeLine("Block type:  " + std::to_string(blockType) + " != " + std::to_string(currentNode->blockType), ERROR_COLOR);
                exit(1);
            }

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

    void addAttribute(ASTNode*& node, std::string messageString)
    {
        if (!currentNode) {
            printMessageSystemError("Unexpected end of error tree.");
            exit(1);
        }
        currentNode->attributeNodes.emplace_back(node, messageString);
    }

    // TODO: This function is probably too unsafe:
    void addAttribute(std::string strVal, std::string messageString)
    {
        if (!currentNode) {
            printMessageSystemError("Unexpected end of error tree.");
            exit(1);
        }
        currentNode->attributeNodes.emplace_back(new ASTNode(Identifier_Node, {}, new asaToken(strVal, Identifier)), messageString);
    }

    void* error(std::string messageString, ErrorMessageType messageType)
    {
        wasError = true;
        int errorTraceOffset = 0;
        std::string messageTypeNumString = std::to_string(messageType);
        Message msg = Message(messageString);

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

        // Additional handling for different message types:
        switch (messageType) {
            case Undefined_Member_Error:
            case Undefined_Symbol_Error:
            case Undefined_Function_Error:
            case Undefined_Variable_Error: {
                //msg.addUnderline(currentNode->astNode, );
                // Show where the symbol is attempting to be used
                printTokenMessage(currentNode->astNode, currentNode->messageString, "used here", ERROR_COLOR, "^", Show_In_Context);
                // Then print possible candidates, if any exist
                if (currentNode->attributeNodes.size() > 0) {
                    console::write("\nCould you have possibly meant one of the following?:\n");
                    console::write("Candidates:\n", NEUTRAL_HIGHLIGHT_COLOR);
                    console::indentation = 1;
                    for (int i = 0; i < currentNode->attributeNodes.size(); i++)
                        printNode(currentNode->attributeNodes[i].first);
                    console::indentation = 0;
                }

                errorTraceOffset++;
                currentNode = currentNode->parentNode;
                break;
            }

            case Removed_Attribute_Error: {
                // Show where the symbol is attempting to be used
                printTokenMessage(currentNode->astNode, currentNode->messageString, "used here", ERROR_COLOR, "^", Show_In_Context);
                // Then print where it is defined
                if (currentNode->attributeNodes.size() > 0) {
                    printTokenMessage(currentNode->attributeNodes[0].first, "", "defined here", NEUTRAL_HIGHLIGHT_COLOR, "~", Show_Endpoints);
                }

                errorTraceOffset++;
                currentNode = currentNode->parentNode;
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
                // Show where the symbol is attempting redefinition
                printTokenMessage(currentNode->astNode, currentNode->messageString, "redefined here", ERROR_COLOR, "^");
                // Then show the line and file of where the symbol is previously defined, (ASTNode*, message, underline color)
                printTokenMessage(currentNode->attributeNodes[0].first, "", currentNode->attributeNodes[0].second, NEUTRAL_HIGHLIGHT_COLOR, "~");

                errorTraceOffset++;
                currentNode = currentNode->parentNode;
                break;
            }

            case Invalid_Compiler_Directive_Arguments_Error:
            default: {
                printTokenMessage(currentNode->astNode, currentNode->messageString);
                errorTraceOffset++;
                currentNode = currentNode->parentNode;
                break;
            }
        }

        // Trace back up the tree of blocks (not sure how necessary this is at this point)
        if (currentNode)
            for (int i = errorTraceOffset; i < maxErrorTraceDepth; i++) {
                console::indentation = i;
                if (currentNode->sourceFunction != "" && currentNode->sourceLine != 0 && currentNode->sourcePath != "")
                    printTokenMessage(currentNode->astNode, currentNode->messageString);
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

        // Additional handling for different message types:
        switch (messageType) {

            case Deprecated_Attribute_Warning: {
                // Show where the symbol is attempting to be used
                printTokenMessage(currentNode->astNode, currentNode->messageString, "used here", WARNING_COLOR, "~", Show_In_Context);
                // Then print where it is defined
                if (currentNode->attributeNodes.size() > 0) {
                    printTokenMessage(currentNode->attributeNodes[0].first, "", "defined here", NEUTRAL_HIGHLIGHT_COLOR, "~", Show_Endpoints);
                }

                errorTraceOffset++;
                currentNode = currentNode->parentNode;
                break;
            }

            default: {
                printTokenMessage(currentNode->astNode, currentNode->messageString, "here", WARNING_COLOR, "~", Show_Self_And_Contents, 1);
                errorTraceOffset++;
                currentNode = currentNode->parentNode;
                break;
            }
        }

        // Trace back up the tree of blocks (not sure how necessary this is at this point)
        if (currentNode)
            for (int i = errorTraceOffset; i < maxErrorTraceDepth; i++) {
                console::indentation = i;
                if (currentNode->sourceFunction != "" && currentNode->sourceLine != 0 && currentNode->sourcePath != "")
                    printTokenMessage(currentNode->astNode, currentNode->messageString, "here", WARNING_COLOR, "~");
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

    void printNode(ASTNode*& node)
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

    static std::map<int, int> collectLineIndents(int startLine, int endLine)
    {
        std::map<int, int> indents;
        for (auto* tok : allTokens) {
            if (tok->tokenType == EndOfLine)
                continue;
            if (tok->lineNumber >= startLine && tok->lineNumber <= endLine) {
                if (indents.find(tok->lineNumber) == indents.end())
                    indents[tok->lineNumber] = tok->lineIndent;
            }
        }
        return indents;
    }

    static std::map<int, std::string*> collectSourceLines(int startLine, int endLine)
    {
        std::map<int, std::string*> result;
        for (auto* tok : allTokens) {
            if (tok->tokenType == EndOfLine)
                continue;
            if (tok->lineValue != nullptr && tok->lineNumber >= startLine && tok->lineNumber <= endLine)
                result[tok->lineNumber] = tok->lineValue;
        }
        return result;
    }

    void printNodeSource(ASTNode*& node, std::string underlineColor, std::string underlineMessage, PrintStyle style, int contextLines, std::string underlineChar)
    {
        tokenRange tokRange = getASTTokenRange(node);
        asaToken* startToken = tokRange.first;
        asaToken* endToken = tokRange.second;

        if (!startToken || !startToken->lineValue)
            return;

        int focusStart = startToken->lineNumber;
        int focusEnd = endToken->lineNumber;
        bool multiLine = (focusStart != focusEnd);

        // Parent info for Show_In_Context
        asaToken* parentStartTok = nullptr;
        asaToken* parentEndTok = nullptr;
        int parentStartLine = -1, parentEndLine = -1;

        if (style == Show_In_Context && node->parentNode != nullptr) {
            // Walk up the parent chain (never reaching the root) to find the
            // nearest ancestor whose line span is strictly wider than the focus
            // node.  This skips over same-line parents (e.g. a binary expression
            // that lives on the same line as the token being highlighted) and also
            // avoids using the root global scope as context, which would pull in
            // unrelated top-level definitions.
            ASTNode* p = node->parentNode;
            while (p && p->parentNode != nullptr) {
                tokenRange pr = getASTTokenRange(p);
                if (pr.first && pr.second && pr.first->lineValue) {
                    int pStart = pr.first->lineNumber;
                    int pEnd = pr.second->lineNumber;
                    if (pStart < focusStart || pEnd > focusEnd) {
                        parentStartTok = pr.first;
                        parentEndTok = pr.second;
                        parentStartLine = pStart;
                        parentEndLine = pEnd;
                        break;
                    }
                }
                p = p->parentNode;
            }
        }

        // Build the set of visible line numbers
        std::set<int> visibleSet;

        switch (style) {
            case Show_Self_And_Contents:
                for (int i = std::max(0, focusStart - contextLines); i <= focusEnd + contextLines; i++)
                    visibleSet.insert(i);
                break;

            case Show_Endpoints:
                // Context before + start line
                for (int i = std::max(0, focusStart - contextLines); i <= focusStart; i++)
                    visibleSet.insert(i);
                // End line + context after (only useful when multi-line)
                if (multiLine) {
                    visibleSet.insert(focusEnd);
                    for (int i = focusEnd + 1; i <= focusEnd + contextLines; i++)
                        visibleSet.insert(i);
                }
                else {
                    for (int i = focusStart + 1; i <= focusStart + contextLines; i++)
                        visibleSet.insert(i);
                }
                break;

            case Show_In_Context:
                if (parentStartLine >= 0) {
                    // Show parent open/close boundaries and the focus node.
                    // contextLines expands each anchor independently.
                    for (int i = std::max(0, parentStartLine - contextLines); i <= parentStartLine + contextLines; i++)
                        visibleSet.insert(i);
                    for (int i = std::max(0, focusStart - contextLines); i <= focusEnd + contextLines; i++)
                        visibleSet.insert(i);
                    for (int i = std::max(0, parentEndLine - contextLines); i <= parentEndLine + contextLines; i++)
                        visibleSet.insert(i);
                }
                else {
                    // No meaningful parent: show focus only with context.
                    for (int i = std::max(0, focusStart - contextLines); i <= focusEnd + contextLines; i++)
                        visibleSet.insert(i);
                }
                break;
        }

        if (visibleSet.empty())
            return;

        int minVisible = *visibleSet.begin();
        int maxVisible = *visibleSet.rbegin();

        // Collect line text and indents across the full visible span
        auto lineValues = collectSourceLines(minVisible, maxVisible);
        if (startToken->lineValue)
            lineValues[focusStart] = startToken->lineValue;
        if (endToken->lineValue)
            lineValues[focusEnd] = endToken->lineValue;
        if (parentStartTok && parentStartTok->lineValue)
            lineValues[parentStartLine] = parentStartTok->lineValue;
        if (parentEndTok && parentEndTok->lineValue)
            lineValues[parentEndLine] = parentEndTok->lineValue;

        auto indents = collectLineIndents(minVisible, maxVisible);
        indents[focusStart] = startToken->lineIndent;
        indents[focusEnd] = endToken->lineIndent;
        if (parentStartTok)
            indents[parentStartLine] = parentStartTok->lineIndent;
        if (parentEndTok)
            indents[parentEndLine] = parentEndTok->lineIndent;

        // Trim blank (no-token) lines from the edges of the visible set
        while (!visibleSet.empty() && !lineValues.count(*visibleSet.begin()))
            visibleSet.erase(visibleSet.begin());
        while (!visibleSet.empty() && !lineValues.count(*visibleSet.rbegin()))
            visibleSet.erase(std::prev(visibleSet.end()));
        if (visibleSet.empty())
            return;

        // Minimum indent across all visible lines (for relative display)
        int minIndent = INT_MAX;
        for (int ln : visibleSet) {
            if (indents.count(ln))
                minIndent = std::min(minIndent, indents[ln]);
        }
        if (minIndent == INT_MAX)
            minIndent = 0;

        int lineNumWidth = std::max(3, (int)std::to_string(maxVisible + 1).size());

        // Blank spacer line: just the gutter bar, no line number or content
        console::applyIndent();
        console::write(std::string(lineNumWidth, ' ') + " |", GUTTER_COLOR);
        console::write("\n");

        // Visual column of a token, relative to minIndent
        auto visCol = [&](asaToken* tok) -> int {
            int leading = countLeadingSpaces(*tok->lineValue);
            int abs = tok->lineIndent + (tok->indexInLine - 1 - leading);
            return std::max(0, abs - minIndent);
        };

        auto formatLineNum = [&](int n) -> std::string {
            std::string s = std::to_string(n + 1);
            while ((int)s.size() < lineNumWidth)
                s = " " + s;
            return s;
        };

        int connectorCol = -1;
        bool pastFocusLine = false;

        auto printEllipsis = [&]() {
            std::string dots(lineNumWidth, '.');
            console::applyIndent();
            console::write(dots + " |  ", GUTTER_COLOR);
            console::write("...", GUTTER_COLOR);
            if (pastFocusLine && connectorCol >= 0 && !underlineMessage.empty()) {
                int spaces = connectorCol - 3;
                for (int i = 0; i < spaces; i++)
                    console::write(" ");
                console::write("│", underlineColor);
            }
            console::write("\n");
        };

        std::vector<int> sortedLines(visibleSet.begin(), visibleSet.end());
        bool isSingleFocus = !multiLine;

        static const int LINE_MAX_WIDTH = 80;

        // Builds the display string for a source line (relative indent + content),
        // truncated to LINE_MAX_WIDTH characters with a " ..." suffix if needed.
        auto getDisplayLine = [&](int ln) -> std::string {
            int lInd = indents.count(ln) ? indents[ln] : minIndent;
            int rInd = std::max(0, lInd - minIndent);
            auto it2 = lineValues.find(ln);
            if (it2 == lineValues.end() || !it2->second)
                return std::string(rInd, ' ');
            int lead = countLeadingSpaces(*it2->second);
            std::string s = std::string(rInd, ' ') + it2->second->substr(lead);
            if ((int)s.size() > LINE_MAX_WIDTH)
                s = s.substr(0, LINE_MAX_WIDTH - 4) + " ...";
            return s;
        };

        // Returns a syntax-highlighted version of rawSub, where colStart is the
        // 0-based display column where rawSub begins within line lineNum.
        // Uses allTokens to find token spans and applies colors by type.
        // Falls back to rawSub unchanged when color is disabled.
        auto highlightDisplay = [&](const std::string& rawSub, int ln, int colStart) -> std::string {
            if (!console::useColor)
                return rawSub;


            int lInd = indents.count(ln) ? indents[ln] : minIndent;
            int rInd = std::max(0, lInd - minIndent);
            auto it2 = lineValues.find(ln);
            int lead = (it2 != lineValues.end() && it2->second) ? countLeadingSpaces(*it2->second) : 0;

            // Collect token spans mapped to display coordinates
            struct Span {
                int s, e;
                const std::string* color;
            };
            std::vector<Span> spans;
            for (auto* tok : allTokens) {
                if (tok->lineNumber != ln)
                    continue;
                const std::string& col = getTokenColor(tok->tokenType);
                if (col.empty())
                    continue;
                int dispPos = rInd + (tok->indexInLine - 1) - lead;
                int dispEnd = dispPos + (int)tok->length;
                // Clamp to rawSub window
                int s = dispPos - colStart;
                int e = dispEnd - colStart;
                if (e <= 0 || s >= (int)rawSub.size())
                    continue;
                spans.push_back({s, e, &col});
            }
            if (spans.empty())
                return rawSub;

            std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.s < b.s; });

            std::string result;
            int pos = 0;
            for (auto& sp : spans) {
                int s = std::max(0, sp.s);
                int e = std::min((int)rawSub.size(), sp.e);
                if (s > pos)
                    result += rawSub.substr(pos, s - pos);
                if (s < e) {
                    result += *sp.color;
                    result += rawSub.substr(s, e - s);
                    result += console::resetColor;
                }
                pos = e;
            }
            if (pos < (int)rawSub.size())
                result += rawSub.substr(pos);
            return result;
        };

        // Returns the display content width of a source line (after truncation)
        auto contentWidth = [&](int ln) -> int {
            return (int)getDisplayLine(ln).size();
        };

        auto isLineTruncated = [&](int ln) -> bool {
            std::string d = getDisplayLine(ln);
            return d.size() >= 4 && d.substr(d.size() - 4) == " ...";
        };

        // Annotation row: blank number + | + underline carets.
        // When bridgeToCol >= 0 a horizontal bridge " <---+" is appended,
        // with the + landing at column bridgeToCol.
        auto printAnnotationLine = [&](int col, int caretLen, int bridgeToCol = -1) {
            console::applyIndent();
            console::write(std::string(lineNumWidth, ' ') + " |  ", GUTTER_COLOR);
            for (int i = 0; i < col; i++)
                console::write(" ");
            for (int i = 0; i < caretLen; i++)
                console::write(underlineChar, underlineColor);
            if (bridgeToCol > col + caretLen) {
                // " " + dashes + "┐" - the ┐ corner lands exactly at bridgeToCol
                console::write(" ", underlineColor);
                int dashCount = bridgeToCol - (col + caretLen) - 1;
                for (int i = 0; i < dashCount; i++)
                    console::write("─", underlineColor);
                console::write("┐", underlineColor);
            }
            console::write("\n");
        };

        // Plain underline row (after the last source line, no gutter needed)
        auto printUnderlineLine = [&](int col, int caretLen) {
            console::applyIndent();
            for (int i = 0; i < lineNumWidth + 4 + col; i++)
                console::write(" ");
            for (int i = 0; i < caretLen; i++)
                console::write(underlineChar, underlineColor);
            console::write("\n");
        };

        // Pre-compute the underline that will carry the connector/message.
        // For a single-focus node this is the only underline; for multi-line it is the end underline.
        bool hasUnderline = !underlineChar.empty();

        int preUlCol = 0, preUlLen = 0;
        int ulLineIdx = -1;
        int gutterMarkerIdx = -1;
        if (hasUnderline) {
            if (isSingleFocus) {
                int sc = visCol(startToken);
                int ec = visCol(endToken) + (int)endToken->length;
                preUlCol = sc;
                preUlLen = std::max(1, ec - sc);
            }
            else {
                preUlCol = visCol(endToken);
                preUlLen = (int)endToken->length;
            }
            for (int i = 0; i < (int)sortedLines.size(); i++) {
                if (sortedLines[i] == focusEnd) {
                    ulLineIdx = i;
                    break;
                }
            }
            for (int i = 0; i < (int)sortedLines.size(); i++) {
                if (sortedLines[i] == focusStart) {
                    gutterMarkerIdx = i;
                    break;
                }
            }
        }
        int messageCol = -1;  // computed after strategy loop

        // Connector strategy: determine the column the vertical | travels down,
        // and whether a horizontal bridge is needed on the annotation row.
        //
        // connectorCol starts at preUlCol + padding.  For every source line that
        // appears after the underline line we first look for a whitespace gap wide
        // enough to thread | through.  Failing that we shift right to
        // contentWidth + padding.  A horizontal bridge (<---+) is drawn on the
        // annotation row whenever the final connectorCol ends up past the right
        // edge of the carets.
        static const int CONNECTOR_PADDING = 2;

        // Returns the column at which | can be threaded through a whitespace gap
        // in dispLine, or -1 if no suitable gap exists.
        // Rules (with CONNECTOR_PADDING = P):
        //   gap < P            -> invalid
        //   P  <= gap < 2*P    -> | at gapStart + P/2  (left-biased)
        //   gap >= 2*P         -> | at gapStart + P    (full padding on left)
        auto findGapConnectorCol = [&](const std::string& dispLine) -> int {
            const int halfPad = CONNECTOR_PADDING / 2;
            bool hadContent = false;
            int i = 0;
            while (i < (int)dispLine.size()) {
                if (dispLine[i] != ' ') {
                    hadContent = true;
                    i++;
                }
                else if (hadContent) {
                    int gapStart = i;
                    int gapLen = 0;
                    while (i < (int)dispLine.size() && dispLine[i] == ' ') {
                        gapLen++;
                        i++;
                    }
                    // Valid gap: wide enough AND content follows (not end-of-line)
                    if (gapLen >= CONNECTOR_PADDING && i < (int)dispLine.size()) {
                        int leftPad = (gapLen >= CONNECTOR_PADDING * 2) ? CONNECTOR_PADDING : halfPad;
                        return gapStart + leftPad;
                    }
                }
                else {
                    i++;  // leading space, not a gap
                }
            }
            return -1;
        };

        // For short underlines clamp so | stays within the caret span rather
        // than landing past its right edge (e.g. a single ^ would otherwise
        // have | two columns to its right with no visual connection).
        connectorCol = (hasUnderline && ulLineIdx >= 0)
                           ? preUlCol + std::min(CONNECTOR_PADDING, std::max(0, preUlLen - 1))
                           : -1;
        bool connectorShifted = false;
        bool connectorUnderpass = false;
        if (ulLineIdx >= 0) {
            for (int i = ulLineIdx + 1; i < (int)sortedLines.size(); i++) {
                int ln = sortedLines[i];
                int cw = contentWidth(ln);
                if (connectorCol < cw) {
                    if (isLineTruncated(ln)) {
                        // Truncated line: go "under" it with ┴/┬ instead of bridging around.
                        connectorUnderpass = true;
                    }
                    else {
                        // Try threading through a whitespace gap first.
                        int gapCol = findGapConnectorCol(getDisplayLine(ln));
                        if (gapCol >= 0 && gapCol >= connectorCol) {
                            connectorCol = gapCol;
                        }
                        else {
                            connectorCol = cw + CONNECTOR_PADDING;
                            connectorShifted = true;
                        }
                    }
                }
            }
        }
        // Bridge shown when connector had to move past the end of the carets.
        bool connectorBridge = connectorShifted && (connectorCol > preUlCol + preUlLen);

        // Message sits CONNECTOR_PADDING to the left of the final connector column.
        if (ulLineIdx >= 0)
            messageCol = connectorCol - CONNECTOR_PADDING;

        // Pending inline underline: appended to the next source line if it fits
        bool hasPendingUnderline = false;
        int pendingUlCol = 0;
        int pendingUlLen = 0;
        bool skipNextEllipsis = false;

        for (int idx = 0; idx < (int)sortedLines.size(); idx++) {
            int lineNum = sortedLines[idx];
            bool isLastLine = (idx == (int)sortedLines.size() - 1);

            // Ellipsis for any gap between consecutive visible lines
            if (idx > 0 && lineNum - sortedLines[idx - 1] > 1) {
                if (!skipNextEllipsis)
                    printEllipsis();
                skipNextEllipsis = false;
            }

            // Print the source line
            auto it = lineValues.find(lineNum);
            std::string* lineVal = (it != lineValues.end()) ? it->second : nullptr;

            std::string lineNumStr = formatLineNum(lineNum);

            if (idx == ulLineIdx)
                pastFocusLine = true;

            console::applyIndent();
            if (hasUnderline && idx == gutterMarkerIdx) {
                console::write(lineNumStr + " ", GUTTER_COLOR);
                console::write(">", underlineColor);
                console::write("  ", GUTTER_COLOR);
            }
            else {
                console::write(lineNumStr + " |  ", GUTTER_COLOR);
            }
            std::string display = getDisplayLine(lineNum);
            int thisContentLen = (int)display.size();
            if (lineVal) {
                // Gap-threaded lines: | sits inside the whitespace gap, so split
                // the display string around connectorCol instead of appending.
                bool isGapThreaded = (idx > ulLineIdx && connectorCol >= 0 && !underlineMessage.empty() && connectorCol < thisContentLen && !connectorUnderpass);
                if (isGapThreaded) {
                    console::write(highlightDisplay(display.substr(0, connectorCol), lineNum, 0));
                    console::write("│", underlineColor);
                    console::write(highlightDisplay(display.substr(connectorCol), lineNum, connectorCol));
                }
                else {
                    console::write(highlightDisplay(display, lineNum, 0));
                }
            }

            // Append any pending inline underline from the previous source line.
            // When carets are appended inline we skip the connector | for that line.
            bool appendedPendingUl = false;
            if (hasPendingUnderline) {
                int spaces = pendingUlCol - thisContentLen;
                for (int i = 0; i < spaces; i++)
                    console::write(" ");
                for (int i = 0; i < pendingUlLen; i++)
                    console::write(underlineChar, underlineColor);
                hasPendingUnderline = false;
                appendedPendingUl = true;
            }

            // Append connector | for every source line the vertical travels through.
            // Skipped on lines that already received inline carets or had | inserted
            // inside a gap (gap-threaded lines, where connectorCol < thisContentLen).
            if (!appendedPendingUl && connectorCol >= 0 && !underlineMessage.empty() && idx > ulLineIdx && connectorCol >= thisContentLen) {
                int spaces = connectorCol - thisContentLen;
                for (int i = 0; i < spaces; i++)
                    console::write(" ");
                console::write("│", underlineColor);
            }
            console::write("\n");

            // For truncated post-underline lines in underpass mode, print ┬ below the source line
            if (connectorUnderpass && idx > ulLineIdx && isLineTruncated(lineNum) && connectorCol >= 0 && !underlineMessage.empty()) {
                int gutterWidth = lineNumWidth + 4;
                console::applyIndent();
                for (int i = 0; i < gutterWidth + connectorCol; i++)
                    console::write(" ");
                console::write("┬", underlineColor);
                console::write("\n");
            }

            // Determine underline parameters for this focus line
            bool isFocusStart = (lineNum == focusStart);
            bool isFocusEnd = (lineNum == focusEnd);

            int ulCol = -1, ulLen = 0;
            if (isSingleFocus && isFocusStart) {
                ulCol = preUlCol;
                ulLen = preUlLen;
            }
            else if (!isSingleFocus && isFocusStart) {
                ulCol = visCol(startToken);
                ulLen = (int)startToken->length;
            }
            else if (!isSingleFocus && isFocusEnd) {
                ulCol = preUlCol;
                ulLen = preUlLen;
            }

            if (ulCol >= 0) {
                if (isLastLine) {
                    bool needsBridge = (idx == ulLineIdx) && connectorBridge && !underlineMessage.empty();
                    printAnnotationLine(ulCol, ulLen, needsBridge ? connectorCol : -1);
                }
                else {
                    int nextLineNum = sortedLines[idx + 1];
                    bool consecutive = (nextLineNum == lineNum + 1);
                    if (consecutive && ulCol >= contentWidth(nextLineNum)) {
                        hasPendingUnderline = true;
                        pendingUlCol = ulCol;
                        pendingUlLen = ulLen;
                    }
                    else {
                        bool nextIsEllipsis = (nextLineNum - lineNum > 1);
                        bool needsBridge = (idx == ulLineIdx) && connectorBridge && !underlineMessage.empty();
                        if (nextIsEllipsis && idx != ulLineIdx) {
                            // Merge annotation into the ellipsis row: show "..." in gutter but underline in content
                            std::string dots(lineNumWidth, '.');
                            console::applyIndent();
                            console::write(dots + " |  ", GUTTER_COLOR);
                            for (int i = 0; i < ulCol; i++)
                                console::write(" ");
                            for (int i = 0; i < ulLen; i++)
                                console::write(underlineChar, underlineColor);
                            console::write("\n");
                            skipNextEllipsis = true;
                        }
                        else {
                            printAnnotationLine(ulCol, ulLen, needsBridge ? connectorCol : -1);
                        }
                    }
                }
                // After the annotation row for the focus underline, print ┴ in underpass mode
                if (connectorUnderpass && idx == ulLineIdx && !underlineMessage.empty()) {
                    console::applyIndent();
                    console::write(std::string(lineNumWidth, ' ') + " |  ", GUTTER_COLOR);
                    for (int i = 0; i < connectorCol; i++)
                        console::write(" ");
                    console::write("┴", underlineColor);
                    console::write("\n");
                }
            }
        }

        // Draw the connector: two rows of | then the message label.
        // The | travels at connectorCol; the message sits CONNECTOR_PADDING chars to its left.
        if (connectorCol >= 0 && !underlineMessage.empty()) {
            int gutterWidth = lineNumWidth + 4;
            for (int row = 0; row < 2; row++) {
                console::applyIndent();
                for (int i = 0; i < gutterWidth + connectorCol; i++)
                    console::write(" ");
                console::write("│", underlineColor);
                console::write("\n");
            }
            // Message is CONNECTOR_PADDING chars left of the connector column (= messageCol).
            console::applyIndent();
            for (int i = 0; i < gutterWidth + messageCol; i++)
                console::write(" ");
            console::write(underlineMessage, underlineColor);
            console::write("\n");
        }
        else if (!hasUnderline && !underlineMessage.empty()) {
            int gutterWidth = lineNumWidth + 4;
            console::applyIndent();
            for (int i = 0; i < gutterWidth; i++)
                console::write(" ");
            console::write(underlineMessage, underlineColor);
            console::write("\n");
        }
    }

    void printTokenMessage(ASTNode*& node, std::string messageString, std::string underlineMessage, const std::string& underlineColor, std::string underlineChar, PrintStyle style, int contextLines)
    {
        tokenRange tokRange = getASTTokenRange(node);
        asaToken* startToken = tokRange.first;
        asaToken* endToken = tokRange.second;
        if (verbosity >= 5) {
            if (currentNode->sourcePath != "" && currentNode->sourcePath != "\0")
                std::cerr << "Source file: " << currentNode->sourcePath << std::endl;
            if (currentNode->sourceLine > 0)
                std::cerr << "Line: " << currentNode->sourceLine << std::endl;
        }
        console::writeLine();
        if (messageString != "") {
            console::applyIndent();
            console::write("(while: " + messageString + ")\n", CONTEXT_INFO_COLOR);
        }
        if (!startToken || !startToken->filePath || !startToken->lineValue) {
            console::writeLine("(no source location available)");  // This should never happen
            return;
        }
        console::applyIndent();
        console::write(" --> ", GUTTER_COLOR);
        std::string filePath = *(startToken->filePath);
        console::write(truncatePath(filePath), GUTTER_COLOR);
        console::write("\n");
        printNodeSource(node, underlineColor, underlineMessage, style, contextLines, underlineChar);
    }

}  // namespace messageSystem
