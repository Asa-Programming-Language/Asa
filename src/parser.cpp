#include "parser.h"

void* (ASTNode::*codegen)() = nullptr;

static bool canInheritAttribute(ASTNode* node, ASTNode* attr);
static void inheritAttributeIfCompatible(ASTNode* node, ASTNode* attr);
static bool tryConsumeVariantParams(const std::vector<asaToken*>& tokens, int& i, std::vector<std::vector<asaToken*>>& groups);

// Returns true if an expression is allowed to continue past a newline, based
// on the last collected token (trailing operator) or the next token (leading
// operator on the next line).
static bool isLineContinuation(TokenType lastTok, TokenType nextTok)
{
    switch (lastTok) {
        case Plus:
        case Minus:
        case Star:
        case Slash:
        case Percent:
        case Equal:
        case Plus_Equal:
        case Minus_Equal:
        case Times_Equal:
        case Slash_Equal:
        case Ampersand_Equal:
        case Bar_Equal:
        case Caret_Equal:
        case Shift_Left_Equal:
        case Shift_Right_Equal:
        case Less:
        case Greater:
        case Less_Equal:
        case Greater_Equal:
        case Equal_Equal:
        case Bang_Equal:
        case Ampersand_Ampersand:
        case Bar_Bar:
        case Ampersand:
        case Bar:
        case Caret:
        case Shift_Left:
        case Shift_Right:
        case Comma:
        case Colon:
        case Colon_Colon:
        case Left_Paren:
        case Left_Bracket:
        case Dot:
        case Dot_Dot:
            return true;
        default:
            break;
    }
    switch (nextTok) {
        case Plus:
        case Minus:
        case Star:
        case Slash:
        case Percent:
        case Less:
        case Greater:
        case Less_Equal:
        case Greater_Equal:
        case Equal_Equal:
        case Bang_Equal:
        case Ampersand_Ampersand:
        case Bar_Bar:
        case Ampersand:
        case Bar:
        case Caret:
        case Shift_Left:
        case Shift_Right:
        case Dot:
        case Dot_Dot:
            return true;
        default:
            break;
    }
    return false;
}

// OPENCODE:
// Creates a new token with the given text and type, copying source-location
// info (line, column, file, indent) from sourceToken. Used by f-string
// desugaring and other places that synthesize tokens not present in the input.
static asaToken* makeSyntheticToken(const std::string& text, TokenType tokenType, asaToken* sourceToken)
{
    return new asaToken(text, tokenType,
        sourceToken ? sourceToken->lineNumber : 0,
        sourceToken ? sourceToken->indexInLine : 0,
        sourceToken ? sourceToken->lineValue : nullptr,
        sourceToken ? sourceToken->filePath : nullptr,
        sourceToken ? sourceToken->lineIndent : 0);
}

// OPENCODE:
// Returns true if prefixToken is the identifier "f" immediately adjacent to
// stringToken (same line, no gap), indicating an f-string prefix like f"...".
static bool isAdjacentFStringPrefix(asaToken* prefixToken, asaToken* stringToken)
{
    return prefixToken &&
           stringToken &&
           prefixToken->tokenStr == "f" &&
           prefixToken->lineNumber == stringToken->lineNumber &&
           prefixToken->indexInLine + prefixToken->length == stringToken->indexInLine;
}

// OPENCODE:
// Tokenizes an embedded f-string expression (the text inside {...}), fixes up
// source locations to point back at the original f-string token, and appends
// the resulting tokens to outTokens. Used during f-string desugaring.
static void appendTokenizedFStringExpression(std::vector<asaToken*>& outTokens, const std::string& expr, asaToken* sourceToken, int exprStartInString)
{
    std::vector<asaToken*> exprTokens;
    std::vector<std::string*> exprLines;
    std::vector<std::string*> exprFileNames;
    std::string fileName = sourceToken && sourceToken->filePath ? *sourceToken->filePath : std::string("f-string");
    std::string exprSource = expr;

    tokenize(exprSource, exprTokens, fileName, exprLines, exprFileNames);
    labelSubTokens(exprTokens);
    joinCommentTokens(exprTokens);

    for (auto* t : exprTokens) {
        if (t->tokenType == EndOfFile || t->tokenType == EndOfLine || t->tokenType == Nothing)
            continue;
        int localLineNumber = t->lineNumber;
        if (sourceToken) {
            t->filePath = sourceToken->filePath;
            t->lineValue = sourceToken->lineValue;
            t->lineNumber = sourceToken->lineNumber + localLineNumber - 1;
            if (localLineNumber == 1)
                t->indexInLine = sourceToken->indexInLine + exprStartInString + t->indexInLine;
        }
        outTokens.push_back(t);
    }
}

// OPENCODE:
// Desugars an f-string literal (f"...{expr}...") into a concatenation of
// string literals and string(expr) calls joined by '+', then re-parses the
// synthetic token stream via generateAST. Returns the resulting AST node.
static ASTNode* parseFStringLiteral(asaToken* prefixToken, asaToken* stringToken, int depth)
{
    const std::string& raw = stringToken->tokenStr;
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
        printTokenError(tokenRange {stringToken, stringToken}, "Invalid formatted string literal");
        wasError = true;
        return nullptr;
    }

    struct FStringPart {
        bool isExpression = false;
        std::string text;
        int startInString = 0;
    };

    std::vector<FStringPart> parts;
    std::string textPart;
    std::string content = raw.substr(1, raw.size() - 2);

    for (int pos = 0; pos < (int)content.size();) {
        char c = content[pos];
        if (c == '\\') {
            textPart += c;
            if (pos + 1 < (int)content.size())
                textPart += content[pos + 1];
            pos += 2;
            continue;
        }
        if (c == '}') {
            printTokenError(tokenRange {stringToken, stringToken}, "Unmatched '}' in formatted string literal");
            wasError = true;
            return nullptr;
        }
        if (c != '{') {
            textPart += c;
            pos++;
            continue;
        }

        if (!textPart.empty()) {
            parts.push_back({false, textPart, pos - (int)textPart.size()});
            textPart.clear();
        }

        int exprStart = pos + 1;
        int exprEnd = exprStart;
        int braceDepth = 1;
        bool inQuote = false;
        char quoteChar = 0;
        bool escaped = false;
        for (; exprEnd < (int)content.size(); exprEnd++) {
            char ec = content[exprEnd];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ec == '\\') {
                escaped = true;
                continue;
            }
            if (inQuote) {
                if (ec == quoteChar)
                    inQuote = false;
                continue;
            }
            if (ec == '"' || ec == '\'') {
                inQuote = true;
                quoteChar = ec;
                continue;
            }
            if (ec == '{') {
                braceDepth++;
                continue;
            }
            if (ec == '}') {
                braceDepth--;
                if (braceDepth == 0)
                    break;
            }
        }

        if (braceDepth != 0) {
            printTokenError(tokenRange {stringToken, stringToken}, "Unclosed '{' in formatted string literal");
            wasError = true;
            return nullptr;
        }

        std::string expr = content.substr(exprStart, exprEnd - exprStart);
        if (TrimString(expr).empty()) {
            printTokenError(tokenRange {stringToken, stringToken}, "Formatted string expression cannot be empty");
            wasError = true;
            return nullptr;
        }
        parts.push_back({true, expr, exprStart});
        pos = exprEnd + 1;
    }

    if (!textPart.empty())
        parts.push_back({false, textPart, (int)content.size() - (int)textPart.size()});

    std::vector<asaToken*> loweredTokens;
    for (int partIndex = 0; partIndex < (int)parts.size(); partIndex++) {
        if (partIndex > 0)
            loweredTokens.push_back(makeSyntheticToken("+", Plus, prefixToken));

        const FStringPart& part = parts[partIndex];
        if (!part.isExpression) {
            loweredTokens.push_back(makeSyntheticToken("\"" + part.text + "\"", String, stringToken));
            continue;
        }

        loweredTokens.push_back(makeSyntheticToken("string", Identifier, stringToken));
        loweredTokens.push_back(makeSyntheticToken("(", Left_Paren, stringToken));
        appendTokenizedFStringExpression(loweredTokens, part.text, stringToken, part.startInString);
        loweredTokens.push_back(makeSyntheticToken(")", Right_Paren, stringToken));
    }

    if (loweredTokens.empty())
        loweredTokens.push_back(makeSyntheticToken("\"\"", String, stringToken));

    ASTNode* parsed = generateAST(loweredTokens, depth + 1);
    if (!parsed || parsed->childNodes.empty()) {
        printTokenError(tokenRange {prefixToken, stringToken}, "Failed to parse formatted string literal");
        wasError = true;
        return nullptr;
    }

    return parsed->childNodes[0];
}


// OPENCODE:
// Advances i past Nothing/EOL/Comment tokens and returns the next meaningful
// token. Returns an empty token if the end of the stream is reached.
asaToken* getNextNonNothingToken(const std::vector<asaToken*>& tokens, int& i)
{
    asaToken* t = new asaToken();
    for (;;) {
        if (i >= tokens.size() - 1 || tokens[i]->tokenType == EndOfFile) {
            return t;
        }
        asaToken* t = NEXT_TOKEN(tokens, i);

        if (t->tokenType != Nothing && t->tokenType != EndOfLine && t->tokenType != Comment) {
            return t;
        }
    }
    return t;
}

// OPENCODE:
// Recursively marks node and all its children as extern (isExtern = true).
// Used by #extern directive handling.
void setChildrenAsExtern(ASTNode*& node)
{
    for (auto& c : node->childNodes)
        setChildrenAsExtern(c);
    node->isExtern = true;
}

// OPENCODE:
// Collects tokens from the stream into subTokens until braces balance at
// brLevel. Used to gather { ... } scope bodies for functions, structs,
// modules, if/while/for blocks, etc. Returns true on success, false on
// unmatched braces.
bool GATHER_SCOPE_BODY(const std::vector<asaToken*>& tokens, std::vector<asaToken*>& subTokens, int brLevel, int& i, bool preserveBraces = false, bool append = false)
{
    int braceLevel = brLevel;
    if (!append)
        subTokens = std::vector<asaToken*>();
    asaToken* firstToken;
    if (i < tokens.size() - 1) {
        firstToken = NEXT_TOKEN(tokens, i);
    }
    else {
        return true;
    }
    i--;
    for (;;) {
        if (i >= tokens.size() - 1 || tokens[i]->tokenType == EndOfFile) {
            printTokenError(tokenRange {firstToken, firstToken}, "Unmatched brace", __LINE__);
            exit(1);
            break;
        }
        asaToken* t = NEXT_TOKEN(tokens, i);

        if (t->tokenType == Nothing)
            continue;

        if (braceLevel <= 0 && t->tokenType == Semi_Colon)
            return true;
        if (t->tokenType == Left_Brace) {
            if (braceLevel != 0 || preserveBraces)
                subTokens.push_back(t);
            braceLevel++;
        }
        else if (t->tokenType == Right_Brace) {
            braceLevel--;
            if (braceLevel != 0 || preserveBraces)
                subTokens.push_back(t);
        }
        else
            subTokens.push_back(t);

        if (braceLevel <= 0)
            break;
    }
    return false;
}


// OPENCODE:
// Collects tokens from the stream into subTokens until parentheses balance at
// pLevel. Tracks brace depth so { } inside ( ) is handled correctly. Used to
// gather parenthesized expressions like if/for/while conditions and argument
// lists.
void GATHER_PAREN_EXPRESSION(const std::vector<asaToken*>& tokens, std::vector<asaToken*>& subTokens, int pLevel, int& i, bool preserveBraces = false)
{
    int parenLevel = pLevel;
    int braceDepth = 0;
    if (pLevel == 1)
        i--;
    asaToken* firstToken = NEXT_TOKEN(tokens, i);
    if (pLevel != 1)
        i--;
    for (;;) {
        if (i >= tokens.size() - 1 || tokens[i]->tokenType == EndOfFile ||
            (tokens[i]->tokenType == Semi_Colon && braceDepth == 0)) {
            printTokenError(tokenRange {firstToken, firstToken}, "Unmatched parenthesis");
            exit(1);
            break;
        }
        asaToken* t = NEXT_TOKEN(tokens, i);

        if (t->tokenType == Nothing || t->tokenType == EndOfLine || t->tokenType == Comment)
            continue;

        if (t->tokenType == Left_Paren) {
            if (parenLevel != 0 || preserveBraces)
                subTokens.push_back(t);
            parenLevel++;
        }
        else if (t->tokenType == Right_Paren) {
            parenLevel--;
            if (parenLevel != 0 || preserveBraces)
                subTokens.push_back(t);
        }
        else {
            if (t->tokenType == Left_Brace)
                braceDepth++;
            else if (t->tokenType == Right_Brace)
                braceDepth--;
            subTokens.push_back(t);
        }

        if (parenLevel <= 0)
            break;
    }
}

// OPENCODE:
// Splits a parenthesized token list by top-level commas into a vector of
// ASTNode expression nodes. Variant-aware: tracks <...> depth so commas inside
// generic arguments don't split. Used for function call arguments and
// directive argument lists.
static std::vector<ASTNode*> parseParenthesizedArgumentNodes(const std::vector<asaToken*>& tokens, int& i, int depth, bool& isLeaf)
{
    std::vector<ASTNode*> insideNodes;
    isLeaf = true;

    int parenLevel = 1;
    int braceDepth = 0;
    int variantCloseIdx = -1;
    TokenType prevMeaningfulToken = Nothing;
    std::vector<asaToken*> subTokens;
    for (;;) {
        if (i >= tokens.size() - 1)
            break;
        asaToken* t = NEXT_TOKEN(tokens, i);

        if (t->tokenType == Left_Paren)
            parenLevel++;
        if (t->tokenType == Right_Paren)
            parenLevel--;
        if (t->tokenType == Left_Brace)
            braceDepth++;
        if (t->tokenType == Right_Brace)
            braceDepth--;
        if (t->tokenType == Less && prevMeaningfulToken == Identifier && variantCloseIdx < 0) {
            std::vector<std::vector<asaToken*>> _dummyGroups;
            int jj = i;
            if (tryConsumeVariantParams(tokens, jj, _dummyGroups))
                variantCloseIdx = jj;
        }
        if (i > variantCloseIdx)
            variantCloseIdx = -1;
        if (t->tokenType != Nothing && t->tokenType != EndOfLine)
            prevMeaningfulToken = t->tokenType;

        if (parenLevel == 0)
            break;
        if (braceDepth == 0 && (t->tokenType == EndOfLine || t->tokenType == Semi_Colon)) {
            if (t->tokenType == EndOfLine) {
                if (subTokens.empty())
                    continue;
                TokenType lastTok = subTokens.back()->tokenType;
                TokenType nextTok = (i + 1 < (int)tokens.size()) ? tokens[i + 1]->tokenType : Nothing;
                if (isLineContinuation(lastTok, nextTok))
                    continue;
                int peek = i + 1;
                while (peek < (int)tokens.size() && (tokens[peek]->tokenType == EndOfLine || tokens[peek]->tokenType == Nothing))
                    peek++;
                if (peek < (int)tokens.size() && tokens[peek]->tokenType == Right_Paren)
                    continue;
            }
            isLeaf = false;
            break;
        }
        if (t->tokenType == Comma && parenLevel == 1 && braceDepth == 0 && variantCloseIdx < 0) {
            ASTNode* newNode = new ASTNode();
            generateAST(subTokens, depth + 1, newNode);
            newNode->nodeType = Expression_Term;
            newNode->codegen = &ASTNode::generateExpression;
            insideNodes.push_back(newNode);
            subTokens.clear();
            continue;
        }

        subTokens.push_back(t);
    }

    ASTNode* newNode = new ASTNode();
    generateAST(subTokens, depth + 1, newNode);
    newNode->nodeType = Expression_Term;
    newNode->codegen = &ASTNode::generateExpression;
    insideNodes.push_back(newNode);
    return insideNodes;
}

// OPENCODE:
// Collects tokens from the stream into subTokens up to a semicolon, with
// implicit line-continuation awareness (via isLineContinuation) and brace-
// depth tracking. The primary statement-gathering helper. Returns true on
// success, false if the stream runs out unexpectedly.
bool GATHER_TO_SEMICOLON(const std::vector<asaToken*>& tokens, std::vector<asaToken*>& subTokens, int& i, bool includeLast = false, bool allowRunOut = false)
{
    asaToken* firstToken;
    if (i < tokens.size() - 1) {
        firstToken = NEXT_TOKEN(tokens, i);
        //if (firstToken.tokenType == Semi_Colon)
        //  return true;
    }
    else {
        return true;
    }
    i--;
    int braceDepth = 0;
    for (;;) {
        if (i >= tokens.size() - 1) {
            if (!allowRunOut) {
                printTokenError(tokenRange {firstToken, firstToken}, "Missing semicolon");
                exit(1);
            }
            return true;
        }
        asaToken* t = NEXT_TOKEN(tokens, i);

        if (t->tokenType == Nothing)
            continue;

        if (t->tokenType == EndOfLine) {
            // Inside a brace block the newline is always a continuation.
            if (braceDepth > 0)
                continue;
            TokenType lastTok = subTokens.empty() ? Nothing : subTokens.back()->tokenType;
            TokenType nextTok = (i + 1 < (int)tokens.size()) ? tokens[i + 1]->tokenType : Nothing;
            if (subTokens.empty() || isLineContinuation(lastTok, nextTok))
                continue;
            if (!allowRunOut) {
                printTokenError(tokenRange {firstToken, firstToken}, "Missing semicolon");
                exit(1);
            }
            return true;
        }

        if (t->tokenType == Left_Brace)
            braceDepth++;
        if (t->tokenType == Right_Brace)
            braceDepth--;

        // Only treat ';' as a terminator at the top brace level.
        if (t->tokenType == Semi_Colon && braceDepth == 0) {
            if (includeLast)
                subTokens.push_back(t);
            break;
        }

        subTokens.push_back(t);
    }
    return false;
}

// OPENCODE:
// Like GATHER_TO_SEMICOLON but ignores EOL tokens entirely, allowing
// multi-line statements without line-continuation checks. Simpler variant.
bool GATHER_TO_SEMICOLON_MULTI_LINE(const std::vector<asaToken*>& tokens, std::vector<asaToken*>& subTokens, int& i, bool includeLast = false, bool allowRunOut = false)
{
    asaToken* firstToken;
    if (i < tokens.size() - 1) {
        firstToken = NEXT_TOKEN(tokens, i);
        //if (firstToken.tokenType == Semi_Colon)
        //  return true;
    }
    else {
        return true;
    }
    i--;
    for (;;) {
        if (i >= tokens.size() - 1) {
            if (!allowRunOut) {
                printTokenError(tokenRange {firstToken, firstToken}, "Missing semicolon");
                exit(1);
            }
            return true;
        }
        asaToken* t = NEXT_TOKEN(tokens, i);

        if (t->tokenType == Nothing)
            continue;

        if (t->tokenType == Semi_Colon) {
            if (includeLast)
                subTokens.push_back(t);
            break;
        }

        subTokens.push_back(t);
    }
    return false;
}

// OPENCODE:
// Collects tokens into subTokens up to either a semicolon or the specified
// 'other' token type. Used when a statement can be terminated by more than
// just ';' (e.g. by a closing brace or keyword).
bool GATHER_TO_SEMICOLON_OR_OTHER(const std::vector<asaToken*>& tokens, std::vector<asaToken*>& subTokens, int& i, TokenType other, bool includeLast = false, bool allowRunOut = false)
{
    asaToken* firstToken;
    if (i < tokens.size() - 1) {
        firstToken = NEXT_TOKEN(tokens, i);
        //if (firstToken.tokenType == Semi_Colon)
        //  return true;
    }
    else {
        return true;
    }
    i--;
    for (;;) {
        if (i >= tokens.size() - 1) {
            if (!allowRunOut) {
                printTokenError(tokenRange {firstToken, firstToken}, "Missing semicolon");
                exit(1);
            }
            break;
        }
        asaToken* t = NEXT_TOKEN(tokens, i);

        if (t->tokenType == Nothing)
            continue;

        if (t->tokenType == EndOfLine) {
            TokenType lastTok = subTokens.empty() ? Nothing : subTokens.back()->tokenType;
            TokenType nextTok = (i + 1 < (int)tokens.size()) ? tokens[i + 1]->tokenType : Nothing;
            if (subTokens.empty() || isLineContinuation(lastTok, nextTok))
                continue;
            if (!allowRunOut) {
                printTokenError(tokenRange {firstToken, firstToken}, "Missing semicolon");
                exit(1);
            }
            break;
        }

        if (t->tokenType == Semi_Colon || t->tokenType == other) {
            if (includeLast)
                subTokens.push_back(t);
            break;
        }

        subTokens.push_back(t);
    }
    return false;
}

// OPENCODE:
// Collects tokens into subTokens until encountering token type 'a' or 'b'
// at the given paren/brace depth level. Generic stop-on-either-token gatherer.
bool GATHER_TO_A_OR_B(const std::vector<asaToken*>& tokens, std::vector<asaToken*>& subTokens, int pLevel, int& i, TokenType a, TokenType b, bool includeLast)
{
    asaToken* firstToken;
    if (i < tokens.size() - 1) {
        firstToken = NEXT_TOKEN(tokens, i);
        //if (firstToken.tokenType == Semi_Colon)
        //  return true;
    }
    else {
        return true;
    }
    i--;
    for (;;) {
        if (i >= tokens.size() - 1) {
            printTokenError(tokenRange {firstToken, firstToken}, "End of file reached before expected " + tokenAsString(a) + " or " + tokenAsString(b));
            exit(1);
            break;
        }
        asaToken* t = NEXT_TOKEN(tokens, i);

        if (t->tokenType == Nothing)
            continue;

        if (t->tokenType == a || t->tokenType == b) {
            if (includeLast)
                subTokens.push_back(t);
            break;
        }

        subTokens.push_back(t);
    }
    return false;
}

// OPENCODE:
// Collects tokens into subTokens until encountering the single token type 'a'
// at the given paren/brace depth level. Generic stop-on-token gatherer.
bool GATHER_TO_TOKEN(const std::vector<asaToken*>& tokens, std::vector<asaToken*>& subTokens, int pLevel, int& i, TokenType a, bool includeLast = false, bool allowRunOut = false)
{
    asaToken* firstToken;
    if (i < tokens.size() - 1) {
        firstToken = NEXT_TOKEN(tokens, i);
        //if (firstToken.tokenType == Semi_Colon)
        //  return true;
    }
    else {
        return true;
    }
    i--;
    for (;;) {
        if (i >= tokens.size() - 1) {
            if (!allowRunOut) {
                printTokenError(tokenRange {firstToken, firstToken}, "End of file reached before expected " + tokenAsString(a));
                exit(1);
            }
            break;
        }
        asaToken* t = NEXT_TOKEN(tokens, i);

        if (t->tokenType == a) {
            if (includeLast)
                subTokens.push_back(t);
            break;
        }

        subTokens.push_back(t);
    }
    return false;
}

// Checks whether tokens[i] is '<' and begins a valid variant list '<...>' followed by '(' or '::'.
// If so, advances i to point at '>' and fills groups with the comma-separated token groups inside.
// Returns false and leaves i unchanged if this does not look like a variant.
static bool tryConsumeVariantParams(const std::vector<asaToken*>& tokens, int& i, std::vector<std::vector<asaToken*>>& groups)
{
    // tokens[i] must be '<'
    int depth = 1;
    int lookahead = i + 1;
    while (lookahead < (int)tokens.size()) {
        TokenType tt = tokens[lookahead]->tokenType;
        if (tt == Nothing) {
            lookahead++;
            continue;
        }
        if (depth == 1 && tt == EndOfLine)
            return false;
        if (tt == EndOfLine) {
            lookahead++;
            continue;
        }
        if (tt == Less) {
            depth++;
            lookahead++;
            continue;
        }
        if (tt == Greater) {
            if (--depth == 0) {
                // Check what follows '>'
                int next = lookahead + 1;
                while (next < (int)tokens.size() && (tokens[next]->tokenType == Nothing || tokens[next]->tokenType == EndOfLine))
                    next++;
                TokenType following = (next < (int)tokens.size()) ? tokens[next]->tokenType : Nothing;
                if (following != Left_Paren && following != Colon_Colon && following != Dot)
                    return false;
                // Collect comma-separated token groups between '<' and '>'
                std::vector<asaToken*> current;
                for (int j = i + 1; j < lookahead; j++) {
                    TokenType jtt = tokens[j]->tokenType;
                    if (jtt == Nothing || jtt == EndOfLine)
                        continue;
                    if (jtt == Comma) {
                        groups.push_back(current);
                        current.clear();
                    }
                    else
                        current.push_back(tokens[j]);
                }
                groups.push_back(current);
                i = lookahead;  // advance i to '>'
                return true;
            }
            lookahead++;
            continue;
        }
        if (depth == 1 && tt == Semi_Colon)
            return false;
        lookahead++;
    }
    return false;
}

// OPENCODE:
// Expands the [start, end] token range to include tok, comparing by line
// number then column. Used to compute the source span of an AST subtree.
static void updateTokenRange(asaToken* tok, asaToken*& start, asaToken*& end)
{
    if (!tok || !tok->filePath || !tok->lineValue)
        return;
    if (!start || tok->lineNumber < start->lineNumber || (tok->lineNumber == start->lineNumber && tok->indexInLine < start->indexInLine))
        start = tok;
    if (!end || tok->lineNumber > end->lineNumber || (tok->lineNumber == end->lineNumber && tok->indexInLine + tok->length > end->indexInLine + end->length))
        end = tok;
}

// OPENCODE:
// Recursive helper for getASTTokenRange: walks the subtree and updates the
// min/max token range using updateTokenRange, including closingToken.
static void getASTTokenRangeHelper(ASTNode* node, asaToken*& start, asaToken*& end)
{
    updateTokenRange(node->token, start, end);
    updateTokenRange(node->closingToken, start, end);

    for (auto& c : node->childNodes)
        getASTTokenRangeHelper(c, start, end);
}

// OPENCODE:
// Computes the min/max source token (start/end) of an AST subtree for
// diagnostic rendering. Returns a tokenRange pair.
tokenRange getASTTokenRange(ASTNode* node)
{
    asaToken* start = nullptr;
    asaToken* end = nullptr;
    getASTTokenRangeHelper(node, start, end);
    return {start, end};
}

// Collects one lineValue* per unique line number from allTokens, for the file
// and line range of [startToken, endToken].
static std::map<int, std::string*> collectSourceLines(asaToken* startToken, asaToken* endToken)
{
    std::map<int, std::string*> lines;
    for (auto* tok : allTokens) {
        if (tok->lineValue != nullptr && tok->lineNumber >= startToken->lineNumber && tok->lineNumber <= endToken->lineNumber) {
            lines[tok->lineNumber] = tok->lineValue;
        }
    }
    if (startToken->lineValue)
        lines[startToken->lineNumber] = startToken->lineValue;
    if (endToken->lineValue)
        lines[endToken->lineNumber] = endToken->lineValue;
    return lines;
}

// Collects the lineIndent (tab-expanded leading whitespace width) for each
// line in [startLine, endLine] by scanning allTokens.
// EndOfLine tokens are skipped: they are assigned the *next* line's lineNumber
// and a reset (zero) lineIndent, so they would corrupt the first-seen value for
// whatever line they land on.
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

// Count leading space chars in a lineValue string (tabs are excluded by the
// tokenizer so this is just a space count).
static int countLeadingSpaces(const std::string& s)
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

// Prints source lines from startToken to endToken with line numbers and underlines.
// On single-line ranges the underline spans [startToken, endToken].
// On multi-line ranges the start token is underlined on its line, then all
// intermediate lines are printed, and the end token is underlined on its line.
void printSourceLines(asaToken* startToken, asaToken* endToken, std::string underlineColor, std::string underlineMessage)
{
    auto lines = collectSourceLines(startToken, endToken);
    auto indents = collectLineIndents(startToken->lineNumber, endToken->lineNumber);
    // Ensure start/end token lines are always present in the indent map.
    indents[startToken->lineNumber] = startToken->lineIndent;
    indents[endToken->lineNumber] = endToken->lineIndent;

    // Find the minimum indent across all lines in the range so indentation is
    // shown relatively (the least-indented line starts at column 0).
    int minIndent = INT_MAX;
    for (auto& [ln, ind] : indents)
        minIndent = std::min(minIndent, ind);
    if (minIndent == INT_MAX)
        minIndent = 0;

    // Consistent line-number column width across all printed lines.
    int lineNumWidth = (int)std::to_string(endToken->lineNumber + 1).size();

    bool multiLine = (startToken->lineNumber != endToken->lineNumber);

    // Compute the visual column (0-indexed, relative to minIndent) of a token.
    // indexInLine counts only non-tab chars; tabs contribute to lineIndent only.
    auto visCol = [&](asaToken* tok) -> int {
        int leading = countLeadingSpaces(*tok->lineValue);
        // Absolute visual column = tab-expanded indent + non-tab content offset.
        int abs = tok->lineIndent + (tok->indexInLine - 1 - leading);
        return std::max(0, abs - minIndent);
    };

    for (auto& [lineNum, lineVal] : lines) {
        std::string lineNumStr = std::to_string(lineNum + 1);
        while ((int)lineNumStr.size() < lineNumWidth)
            lineNumStr = " " + lineNumStr;

        // Display the line with indentation relative to minIndent.
        // Strip all leading spaces from lineValue (tabs were already excluded by
        // the tokenizer), then prepend relInd spaces for the relative indent.
        int lineInd = indents.count(lineNum) ? indents[lineNum] : minIndent;
        int relInd = std::max(0, lineInd - minIndent);
        int leading = countLeadingSpaces(*lineVal);
        std::string displayLine = std::string(relInd, ' ') + lineVal->substr(leading);

        console::applyIndent();
        console::write(lineNumStr + " |  ", console::yellowFGColor);
        console::write(displayLine);
        console::write("\n");

        if (!multiLine) {
            // Single-line: underline from start token to end of end token.
            int sc = visCol(startToken);
            int ec = visCol(endToken) + (int)endToken->length;
            console::applyIndent();
            for (int i = 0; i < lineNumWidth + 4 + sc; i++)
                console::write(" ");
            for (int i = 0; i < ec - sc; i++)
                console::write("^", underlineColor);
            console::write("\n");
            console::applyIndent();
            for (int i = 0; i < lineNumWidth + 4 + sc; i++)
                console::write(" ");
            console::write(underlineMessage, underlineColor);
            console::write("\n");
        }
        else if (lineNum == startToken->lineNumber) {
            // First line of multi-line range: underline the start token.
            int sc = visCol(startToken);
            console::applyIndent();
            for (int i = 0; i < lineNumWidth + 4 + sc; i++)
                console::write(" ");
            for (int i = 0; i < (int)startToken->length; i++)
                console::write("^", underlineColor);
            console::write("\n");
        }
        else if (lineNum == endToken->lineNumber) {
            // Last line of multi-line range: underline the end token + message.
            int ec = visCol(endToken);
            console::applyIndent();
            for (int i = 0; i < lineNumWidth + 4 + ec; i++)
                console::write(" ");
            for (int i = 0; i < (int)endToken->length; i++)
                console::write("^", underlineColor);
            console::write("\n");
            console::applyIndent();
            for (int i = 0; i < lineNumWidth + 4 + ec; i++)
                console::write(" ");
            console::write(underlineMessage, underlineColor);
            console::write("\n");
        }
    }
}

// OPENCODE:
// Prints a source location marker (file path + underlined source lines) with
// the given message. A lighter diagnostic used for "here" annotations, not
// errors or warnings.
void printTokenMarked(tokenRange tokRange, std::string msgString, int sourceLineNumber, const char* fileName)
{
    asaToken* startToken = tokRange.first;
    asaToken* endToken = tokRange.second;
    if (verbosity >= 5) {
        if (fileName != "" && fileName != "\0")
            std::cerr << "Source file: " << fileName << std::endl;
        if (sourceLineNumber > 0)
            std::cerr << "Line: " << sourceLineNumber << std::endl;
    }
    if (msgString != "")
        console::writeLine(msgString);
    console::applyIndent();
    console::write("In: ", console::yellowFGColor);
    console::write(*(startToken->filePath), console::yellowFGColor);
    console::write("\n");
    printSourceLines(startToken, endToken, console::blueFGColor, "here");
}

// OPENCODE:
// Prints an error message with the file path and underlined source lines
// (red). Respects suppressErrors. In compiler-debug mode, throws after
// printing so the call stack is traceable.
void printTokenError(tokenRange tokRange, std::string errorString, int sourceLineNumber, const char* fileName)
{
    asaToken* startToken = tokRange.first;
    asaToken* endToken = tokRange.second;
    if (messageSystem::suppressErrors)
        return;
    if (verbosity >= 5) {
        if (fileName != "" && fileName != "\0")
            std::cerr << "Source file: " << fileName << std::endl;
        if (sourceLineNumber > 0)
            std::cerr << "Line: " << sourceLineNumber << std::endl;
    }
    console::applyIndent();
    console::printError(errorString);
    if (!startToken || !startToken->filePath || !startToken->lineValue) {
        console::writeLine("(no source location available)");  // This should never happen
        return;
    }
    console::applyIndent();
    console::write("In: ", console::yellowFGColor);
    console::write(*(startToken->filePath), console::yellowFGColor);
    console::write("\n");
    printSourceLines(startToken, endToken, console::redFGColor, "here");
    // If debugging the compiler, throw so that the call can be traced
    if (compilerFlags & Flags_CompilerDebug)
        throw;
    //exit(1);
}

// OPENCODE:
// Prints a warning message with the file path and underlined source lines
// (red). Respects suppressErrors. Unlike printTokenError, does not set
// wasError or throw.
void printTokenWarning(tokenRange tokRange, std::string errorString, int sourceLineNumber, const char* fileName)
{
    asaToken* startToken = tokRange.first;
    asaToken* endToken = tokRange.second;
    if (messageSystem::suppressErrors)
        return;
    if (verbosity >= 5) {
        if (fileName != "")
            std::cerr << "Source file: " << fileName << std::endl;
        if (sourceLineNumber > 0)
            std::cerr << "Line: " << sourceLineNumber << std::endl;
    }
    console::applyIndent();
    console::printWarning(errorString);
    console::write("In: ", console::yellowFGColor);
    console::write(*(startToken->filePath), console::yellowFGColor);
    console::write("\n");
    printSourceLines(startToken, endToken, console::yellowFGColor, "here");
    //throw;
}

// OPENCODE:
// Prints a verbose message that a module was imported, including its path
// at verbosity >= 3.
void printModuleLoaded(std::string& moduleName, std::string& modulePath)
{
    std::cout << "Module \"" << moduleName << "\" imported";
    if (verbosity >= 3)
        std::cout << " from: " << modulePath;
    std::cout << std::endl;
}

// OPENCODE:
// The final parser validation gate: recurses the entire AST and reports a
// "Failed to compile" error for any token left in leafNodes (tokens that were
// never incorporated into a construct). Short-circuits once wasError is set.
void findUnusedLeafNodes(ASTNode*& node)
{
    for (auto& l : node->leafNodes) {
        printTokenError(getASTTokenRange(l), "Failed to compile, parser failed");
        wasError = true;
    }
    if (wasError)
        return;
    for (auto& c : node->childNodes)
        findUnusedLeafNodes(c);
}

std::vector<ASTNode*> ASTNodes = std::vector<ASTNode*>();

std::map<TokenType, ASTNodeType> operatorDefaultNodeType = {
    {Colon, Colon_Separator_Node},
    {Dot_Dot, Range_Node},
    {Comma, Comma_Node},
    {Bang_Equal, Compare_Not},
    {Equal_Equal, Compare_Equal},
    {Less, Compare_Less},
    {Greater, Compare_Greater},
    {Less_Equal, Compare_LessEqual},
    {Greater_Equal, Compare_GreaterEqual},
    {Plus, Expression_Plus},
    {Minus, Expression_Minus},
    {Star, Expression_Times},
    {Slash, Expression_Divide},
    {Ampersand, Bitwise_And},
    {Bar, Bitwise_Or},
    {Caret, Bitwise_Xor},
    {Tilde, Bitwise_Not},
    {Shift_Left, Bitwise_Shift_Left},
    {Shift_Right, Bitwise_Shift_Right},
    {Ref, Reference_Operation},
    {Const, Const_Keyword},
    {Exact, Exact_Type_Node},
    {Left_Bracket, Access_Operation},
    {Dot, Member_Access},
    {Dot_At, Attribute_Access},
    {Arrow_Right, Pipe_Operation},
    {Percent, Expression_Modulo},
    {At, Expression_Modulo},
    {Ampersand_Ampersand, Logical_And},
    {Bar_Bar, Logical_Or},
    {Bang, Logical_Not},
    {Bang_Bang, Logical_Not},
};

std::map<ASTNodeType, int> operatorPrecedence = {
    {Operator_Overload_Node, 100},  // anything else
    {Colon_Separator_Node, 99},     // :
    {Member_Access, 90},            // .
    {Attribute_Access, 90},         // .@
    {Access_Operation, 90},         // []
    {Expression_Paren_Term, 70},    // ()
    {Address_Of_Operation, 50},     // &
    {Dereference_Operation, 50},    // * (unary)
    {Expression_Times, 40},         // *
    {Expression_Divide, 40},        // /
    {Expression_Modulo, 40},        // %
    {Expression_Plus, 30},          // +
    {Expression_Minus, 30},         // -
    {Bitwise_Shift_Left, 25},       // <<
    {Bitwise_Shift_Right, 25},      // >>
    {Bitwise_And, 24},              // &
    {Bitwise_Xor, 23},              // ^
    {Bitwise_Or, 22},               // |
    {Compare_Equal, 20},            // ==
    {Compare_Not, 20},              // !=
    {Compare_Less, 20},             // <
    {Compare_LessEqual, 20},        // <=
    {Compare_Greater, 20},          // >
    {Compare_GreaterEqual, 20},     // >=
    {Logical_And, 18},              // &&
    {Logical_Or, 16},               // ||
    {Range_Node, 15},               // ..
    {Comma_Node, 12},               // ,
    {Pipe_Operation, 10},           // ->
};

std::unordered_set<ASTNodeType> leftAssociativeOperators = {
    Member_Access,
    Attribute_Access,
    Access_Operation,
    Pipe_Operation,
};

std::unordered_set<ASTNodeType> literals = {
    Integer_Node,
    Float_Node,
    Boolean_Node,
    String_Node,
};

std::unordered_set<TokenType> compileTimeDefinable = {
    While_Statement,
    For_Statement,
    If_Statement,
    Struct_Define,
    Module_Define,
    Enum_Define,
};

ASTNode* rootNode = new ASTNode();
std::vector<ASTNode*> importedNodes = std::vector<ASTNode*>();
std::unordered_set<std::string> importedModuleNames = std::unordered_set<std::string>();
std::unordered_set<std::string> importedFileNames = std::unordered_set<std::string>();

std::unordered_map<std::string, AsaBaseType*> asaBaseTypes;
//std::unordered_map<llvm::Type*, AsaBaseType*> asaBaseTypesFromLLVM;

AsaBaseType* CreateNewAsaType(std::string name, llvm::Type* (*baseLLVMTypeFn)(), bool isSigned){
    AsaBaseType* asaType = new AsaBaseType(name, baseLLVMTypeFn(), isSigned);

    return (asaBaseTypes[name] = std::move(asaType));
}
AsaBaseType* CreateNewAsaType(std::string name, llvm::Type* baseLLVMType, bool isSigned){
    AsaBaseType* asaType = new AsaBaseType(name, baseLLVMType, isSigned);

    return (asaBaseTypes[name] = std::move(asaType));
}
AsaBaseType* CreateNewAsaType(std::string name){
    AsaBaseType* asaType = new AsaBaseType(name);

    return (asaBaseTypes[name] = std::move(asaType));
}

AsaTypeInstance* CreateAsaTypeInstanceFromASTNode(ASTNode*& node){
    AsaTypeInstance* asaTypeInstance = new AsaTypeInstance;

    node = ExtractTypeModifiersFromType(node, asaTypeInstance->typeModifiers);
    std::string typeName = node->token->tokenStr;

    // If the base type already exists, use it:
    if(asaBaseTypes.count(typeName) > 0){
        asaTypeInstance->baseType = asaBaseTypes[typeName];
    }
    // Otherwise, create it as a struct, and mark it as not yet defined:
    else{
        asaTypeInstance->baseType = CreateNewAsaType(typeName);
    }

    // Then, calculate the llvmType by applying the modifiers to the base type:
    for(const auto& mod : asaTypeInstance->typeModifiers){
        if(mod == Pointer_Node){
            asaTypeInstance->llvmType = PointerType::get(*llvmCompileContext, 0);
            asaTypeInstance->pointerLevel++;
        }
    }

    return std::move(asaTypeInstance);
}

ASTNode* ExtractTypeModifiersFromType(ASTNode* node, std::vector<ASTNodeType>& modifiers){
    messageSystem::startBlock(node, "Extracting type modifiers", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
    while (true) 
    {
        if (node->token->tokenStr == "*"){
            modifiers.push_back(Pointer_Node);
            goto cont;
        }
        else if (node->token->tokenStr == "const"){
            modifiers.push_back(Const_Keyword);
            goto cont;
        }
        else if (node->token->tokenStr == "ref"){
            modifiers.push_back(Reference_Operation);
            goto cont;
        } 
        else if (node->token->tokenStr == "exact"){
            modifiers.push_back(Exact_Type_Node);
            goto cont;
        } 
        break;
cont:
        if(node->childNodes.size() > 0)
            node = node->childNodes[0];
        else{
            messageSystem::error("Expected type name after type modifiers");
        }
    }
    return node;
}

// Parses a type expression from a flat token list.
// Used for the right-hand side of ':' declarations where '*' means pointer level,
// not multiply, and const/ref/exact are always type modifiers regardless of order.
// Returns the root of a right-nested modifier chain, e.g.:
//   [*, ref, *, const, *, char] -> * { ref { * { const { * { char } } } } }
static ASTNode* parseType(const std::vector<asaToken*>& tokens, int depth)
{
    if (depth >= MAX_AST_DEPTH) {
        printf("Error: Max AST Depth of %d Reached\n", MAX_AST_DEPTH);
        exit(1);
    }
    if (tokens.empty())
        return nullptr;

    asaToken* tok = tokens[0];
    ASTNodeType nodeType = Nothing_Node;
    bool isModifier = false;

    switch (tok->tokenType) {
        case Const:
            nodeType = Const_Keyword;
            isModifier = true;
            break;
        case Ref:
            nodeType = Reference_Operation;
            isModifier = true;
            break;
        case Exact:
            nodeType = Exact_Type_Node;
            isModifier = true;
            break;
        case Star:
            nodeType = Dereference_Operation;
            isModifier = true;
            break;
        case Star_Star:
            nodeType = Dereference_Operation;
            isModifier = true;
            break;
        default:
            break;
    }

    ASTNode* node = new ASTNode();
    ASTNodes.push_back(node);
    node->token = tok;
    node->lineNumber = tok->lineNumber;

    if (isModifier) {
        node->nodeType = nodeType;
        node->codegen = &ASTNode::generateUnaryExpression;

        std::vector<asaToken*> rest;
        if (tok->tokenType == Star_Star) {
            // `**T` expands to two pointer levels. Both nodes must use "*" as their token
            // value so that gatherTypeModifiers in codegen recognises each as a pointer level.
            asaToken* syntheticStar = new asaToken("*", Star);
            node->token = syntheticStar;
            rest.push_back(syntheticStar);
            rest.insert(rest.end(), tokens.begin() + 1, tokens.end());
        }
        else {
            rest = std::vector<asaToken*>(tokens.begin() + 1, tokens.end());
        }

        ASTNode* child = parseType(rest, depth + 1);
        if (!child) {
            printTokenError(tokenRange {tok, tok}, "Expected type after modifier");
            exit(1);
        }
        node->childNodes.push_back(child);
    }
    else {
        node->nodeType = Identifier_Node;
        node->codegen = &ASTNode::generateVariableExpression;
        // Handle variant type application: e.g. array<int> -> token "array.int"
        if (tokens.size() > 1 && tokens[1]->tokenType == Less) {
            std::string mangled = tok->tokenStr;
            int angleDepth = 0;
            for (size_t j = 1; j < tokens.size(); j++) {
                if (tokens[j]->tokenType == Less) {
                    angleDepth++;
                }
                else if (tokens[j]->tokenType == Greater) {
                    if (--angleDepth == 0)
                        break;
                }
                else if (angleDepth == 1 &&
                         (tokens[j]->tokenType == Identifier ||
                             tokens[j]->tokenType == Integer ||
                             tokens[j]->tokenType == Float ||
                             tokens[j]->tokenType == String ||
                             tokens[j]->tokenType == Character ||
                             tokens[j]->tokenType == True_Literal ||
                             tokens[j]->tokenType == False_Literal)) {
                    mangled += "." + tokens[j]->tokenStr;
                }
            }
            asaToken* synth = new asaToken(*tok);
            synth->tokenStr = mangled;
            node->token = synth;
        }
    }
    return node;
}

// Collapse identifier<type> sequences into identifier.type tokens in-place.
// Used for return type token lists and similar non-parseType contexts.
static void collapseVariantTypeTokens(std::vector<asaToken*>& tokens)
{
    for (int i = 0; i < (int)tokens.size(); i++) {
        if (tokens[i]->tokenType == Identifier &&
            i + 1 < (int)tokens.size() && tokens[i + 1]->tokenType == Less) {
            std::string mangled = tokens[i]->tokenStr;
            int angleDepth = 0;
            int j = i + 1;
            for (; j < (int)tokens.size(); j++) {
                if (tokens[j]->tokenType == Less) {
                    angleDepth++;
                }
                else if (tokens[j]->tokenType == Greater) {
                    if (--angleDepth == 0) {
                        j++;
                        break;
                    }
                }
                else if (angleDepth == 1 &&
                         (tokens[j]->tokenType == Identifier ||
                             tokens[j]->tokenType == Integer ||
                             tokens[j]->tokenType == Float ||
                             tokens[j]->tokenType == String ||
                             tokens[j]->tokenType == Character ||
                             tokens[j]->tokenType == True_Literal ||
                             tokens[j]->tokenType == False_Literal)) {
                    mangled += "." + tokens[j]->tokenStr;
                }
            }
            asaToken* synth = new asaToken(*tokens[i]);
            synth->tokenStr = mangled;
            tokens.erase(tokens.begin() + i, tokens.begin() + j);
            tokens.insert(tokens.begin() + i, synth);
        }
    }
}

// Gather a loop/conditional body: either a {} block or a single-line statement
// wrapped in synthetic braces. subTokens is cleared and filled.
static void gatherOptionalBraceBody(
    const std::vector<asaToken*>& tokens,
    std::vector<asaToken*>& subTokens,
    int& i)
{
    asaToken* firstNextToken = getNextNonNothingToken(tokens, i);
    i--;
    if (firstNextToken->tokenType == Left_Brace)
        GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);
    else {
        subTokens = {};
        GATHER_TO_SEMICOLON_MULTI_LINE(tokens, subTokens, i, true, false);
        subTokens.insert(subTokens.begin(), new asaToken("{", Left_Brace));
        subTokens.push_back(new asaToken("}", Right_Brace));
    }
}

// Build a Scope_Body AST node from the given token list.
static ASTNode* makeScopeBodyNode(
    const std::vector<asaToken*>& subTokens,
    int depth)
{
    ASTNode* node = generateAST(subTokens, depth + 1, nullptr, true);
    node->nodeType = Scope_Body;
    node->codegen = &ASTNode::generateScopeBody;
    return node;
}

// Parse a function parameter (with optional default value = expr) and append
// the result to arguments / argumentDefaults.
static void collapseVariantTypeTokens(std::vector<asaToken*>& tokens);

// OPENCODE:
// Parses a function parameter from a token sub-list. Splits on top-level '='
// to separate the type/name portion from the default expression, stores the
// raw default tokens in defaultRawTokens for call-site re-parsing, and appends
// parsed argument nodes to arguments and default nodes to argumentDefaults.
static void parseParamWithDefault(
    const std::vector<asaToken*>& subTokens,
    int depth,
    std::vector<ASTNode*>& arguments,
    std::vector<ASTNode*>& argumentDefaults)
{
    std::vector<asaToken*> paramTokens = subTokens;
    ASTNode* defaultArgNode = nullptr;
    int d = 0;
    int angleDepth = 0;
    for (int si = 0; si < (int)subTokens.size(); si++) {
        TokenType tt = subTokens[si]->tokenType;
        if (tt == Left_Paren || tt == Left_Bracket || tt == Left_Brace)
            d++;
        else if (tt == Right_Paren || tt == Right_Bracket || tt == Right_Brace)
            d--;
        else if (tt == Less)
            angleDepth++;
        else if (tt == Greater && angleDepth > 0)
            angleDepth--;
        else if (tt == Equal && d == 0 && angleDepth == 0) {
            paramTokens = {subTokens.begin(), subTokens.begin() + si};
            std::vector<asaToken*> defaultTokens(subTokens.begin() + si + 1, subTokens.end());
            defaultArgNode = new ASTNode();
            generateAST(defaultTokens, depth + 1, defaultArgNode);
            defaultArgNode->nodeType = Expression_Term;
            defaultArgNode->codegen = &ASTNode::generateExpression;
            defaultArgNode->defaultRawTokens = defaultTokens;
            break;
        }
    }
    collapseVariantTypeTokens(paramTokens);
    ASTNode* newNode = new ASTNode();
    generateAST(paramTokens, depth + 1, newNode);
    newNode->nodeType = Expression_Term;
    newNode->codegen = &ASTNode::generateExpression;
    arguments.push_back(newNode);
    argumentDefaults.push_back(defaultArgNode);
}

static ASTNode* getQualifiedLocalNameNode(ASTNode* node);
static ASTNode* makeQualifiedNameMarker(ASTNode* qualifiedName);

// OPENCODE:
// The core recursive-descent parser. Consumes a flat token list and builds a
// tree of ASTNode children under parentNodePtr. Uses a two-phase approach:
// operands accumulate in parentNode->leafNodes, operators combine them, and at
// statement boundaries (;) or block ends remaining leaves are flushed into
// childNodes. The resulting tree is naively left-to-right; fixPrecedence
// rotates it later. Dispatches on tokenType via a large switch. Each case
// assigns the node's codegen member-function pointer for later LLVM lowering.
ASTNode* generateAST(const std::vector<asaToken*>& tokens, int depth, ASTNode* parentNodePtr, bool isScopeBody)
{
    if (depth == MAX_AST_DEPTH) {
        printf("Error: Max AST Depth of %d Reached", MAX_AST_DEPTH);
        exit(1);
    }

    ASTNode* parentNode = parentNodePtr;
    if (parentNodePtr == nullptr)
        parentNode = new ASTNode();

    // If root node
    if (depth == 0) {
        parentNode->token = new asaToken("global", Nothing, 0, 0, nullptr, nullptr);
        //parentNode->tokenType = Nothing;
        parentNode->nodeType = Scope_Body;
    }

    // Iterate all tokens
    std::vector<ASTNode*> pendingAttributes;
    std::string pendingCommentText;
    int pendingCommentLastLine = -10;
    for (int i = 0; i < tokens.size(); i++) {
        ASTNode* node = new ASTNode();
        ASTNodes.push_back(node);
        //node.prevNode = &parentNode;
        asaToken* token = tokens[i];
        std::string tokenValue = tokens[i]->tokenStr;
        TokenType tokenType = tokens[i]->tokenType;
        int lineNumber = tokens[i]->lineNumber;
        bool unusedNode = false;

        //node->tokenType = tokenType;
        node->token = token;
        node->lineNumber = lineNumber;

        switch (tokenType) {
            case If_Statement: {
                node->nodeType = If_Statement_Node;
                node->codegen = &ASTNode::generateIf;
                messageSystem::startBlock(node, "Parsing if statement", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
                defer(messageSystem::endBlock());

                ASTNode* conditionNode = new ASTNode();
                ASTNode* bodyNode = new ASTNode();
                //ASTNode* elseNode = new ASTNode();

                std::vector<asaToken*> subTokens = std::vector<asaToken*>();

                // Step through all tokens until parens are closed
                GATHER_PAREN_EXPRESSION(tokens, subTokens, 0, i, true);

                conditionNode = generateAST(subTokens, depth + 1);
                messageSystem::startBlock(conditionNode, "Parsing if condition", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
                if (conditionNode->childNodes[0]->nodeType != Expression_Paren_Term)
                    messageSystem::error("Expected condition surrounded by parens. (Got " + ASTNodeTypeAsString(conditionNode->childNodes[0]->nodeType) + ")", messageSystem::Syntax_Error);

                conditionNode->nodeType = Condition;
                messageSystem::endBlock();

                gatherOptionalBraceBody(tokens, subTokens, i);
                bodyNode = makeScopeBodyNode(subTokens, depth);

                node->childNodes.push_back(conditionNode);
                node->childNodes.push_back(bodyNode);
                ASTNode* blankElse = new ASTNode();  // default else
                blankElse->nodeType = Else_Statement_Node;
                blankElse->codegen = &ASTNode::generateScopeBody;
                node->childNodes.push_back(blankElse);

                ASTNode* prevNode = node;

                messageSystem::startBlock(prevNode, "Parsing else/else-if chain", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
                defer(messageSystem::endBlock());
                // Handle else/else if chain
                for (;;) {
                    subTokens = std::vector<asaToken*>();
                    int sI = i;
                    asaToken* nextToken = getNextNonNothingToken(tokens, i);  // i will next point to { or if
                    if (nextToken->tokenType == Else_Statement) {
                        int sI2 = i;
                        asaToken* nextToken2 = getNextNonNothingToken(tokens, i);  // Gets the { or if
                        // Else If
                        if (nextToken2->tokenType == If_Statement) {
                            // Parse as regular if
                            subTokens.push_back(nextToken2);  // add `if`
                            GATHER_PAREN_EXPRESSION(tokens, subTokens, 0, i, true);

                            std::vector<asaToken*> bodyTokens;
                            gatherOptionalBraceBody(tokens, bodyTokens, i);
                            subTokens.insert(subTokens.end(), bodyTokens.begin(), bodyTokens.end());

                            ASTNode* elseNode = generateAST(subTokens, depth + 1)->childNodes[0];
                            prevNode->childNodes[2] = elseNode;
                            prevNode = elseNode;
                            continue;
                        }
                        // Regular Else
                        else {
                            i = sI2;
                            gatherOptionalBraceBody(tokens, subTokens, i);
                            ASTNode* elseNode = generateAST(subTokens, depth + 1, nullptr, true);
                            elseNode->nodeType = Else_Statement_Node;
                            elseNode->codegen = &ASTNode::generateScopeBody;
                            prevNode->childNodes[2] = elseNode;
                            prevNode = elseNode;
                            break;  // else must be the end of chain
                        }
                    }
                    else {
                        i = sI;
                        break;
                    }
                }

                break;
            }

            case Else_Statement: {

                messageSystem::startBlock(node, "Parsing else/else-if", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
                defer(messageSystem::endBlock());

                messageSystem::error("'else' statement must be preceded by at least one 'if' statement", messageSystem::Syntax_Error);

                exit(1);
                goto dontAddNodeForce;
            }

            case While_Statement: {
                node->nodeType = While_Statement_Node;
                node->codegen = &ASTNode::generateWhile;
                messageSystem::startBlock(node, "Parsing while loop", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
                defer(messageSystem::endBlock());

                ASTNode* conditionNode = new ASTNode();
                ASTNode* bodyNode = new ASTNode();

                std::vector<asaToken*> subTokens = std::vector<asaToken*>();

                // Step through all tokens until parens are closed
                GATHER_PAREN_EXPRESSION(tokens, subTokens, 0, i, true);

                conditionNode = generateAST(subTokens, depth + 1);
                messageSystem::startBlock(conditionNode, "Parsing while condition", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
                if (conditionNode->childNodes[0]->nodeType != Expression_Paren_Term)
                    messageSystem::error("Expected condition surrounded by parens. (Got " + ASTNodeTypeAsString(conditionNode->childNodes[0]->nodeType) + ")", messageSystem::Syntax_Error);
                conditionNode->nodeType = Condition;
                messageSystem::endBlock();

                gatherOptionalBraceBody(tokens, subTokens, i);
                bodyNode = makeScopeBodyNode(subTokens, depth);

                node->childNodes.push_back(conditionNode);
                node->childNodes.push_back(bodyNode);

                break;
            }

            case For_Statement: {
                node->nodeType = For_Statement_Node;
                node->codegen = &ASTNode::generateFor;

                messageSystem::startBlock(node, "Parsing for loop", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
                defer(messageSystem::endBlock());

                ASTNode* iteratorNode = new ASTNode();
                ASTNode* rangeNode = new ASTNode();
                ASTNode* bodyNode = new ASTNode();

                i++;  // skip past '('

                // Collect all tokens inside the parens, splitting on the 'in' keyword:
                //   for(i in 0..100)           -> iterTokens=[i],          rangeTokens=[0..100]
                //   for(i : uint16 in 0..65534) -> iterTokens=[i,:,uint16], rangeTokens=[0..65534]
                int parenLevel = 1;
                std::vector<asaToken*> iterTokens;
                std::vector<asaToken*> subTokens = std::vector<asaToken*>();
                bool foundIn = false;
                for (;;) {
                    if (i >= (int)tokens.size() - 1)
                        break;
                    asaToken* t = NEXT_TOKEN(tokens, i);

                    if (t->tokenType == Nothing || t->tokenType == EndOfLine || t->tokenType == Comment)
                        continue;
                    if (t->tokenType == Left_Paren)
                        parenLevel++;
                    if (t->tokenType == Right_Paren) {
                        parenLevel--;
                        if (parenLevel == 0)
                            break;
                    }
                    if (t->tokenType == Semi_Colon)
                        break;

                    if (t->tokenType == In_Keyword && parenLevel == 1 && !foundIn) {
                        foundIn = true;
                        iterTokens = subTokens;
                        subTokens = std::vector<asaToken*>();
                        continue;
                    }

                    subTokens.push_back(t);
                }

                // Build iterator node only when 'in' was present;
                // otherwise leave it as Nothing_Node (range-only loop like for(0..100))
                if (foundIn) {
                    iteratorNode->nodeType = Iterator;
                    iteratorNode->codegen = &ASTNode::generateIterator;

                    // Find colon to separate name from optional type annotation
                    int colonIdx = -1;
                    for (int j = 0; j < (int)iterTokens.size(); j++) {
                        if (iterTokens[j]->tokenType == Colon) {
                            colonIdx = j;
                            break;
                        }
                    }
                    std::vector<asaToken*> nameToks(iterTokens.begin(),
                        colonIdx >= 0 ? iterTokens.begin() + colonIdx
                                      : iterTokens.end());
                    ASTNode* nameAST = generateAST(nameToks, depth + 1);
                    ASTNode* nameNode = nameAST->childNodes.empty() ? nameAST : nameAST->childNodes[0];
                    iteratorNode->childNodes.push_back(nameNode);

                    if (colonIdx >= 0) {
                        std::vector<asaToken*> typeToks(iterTokens.begin() + colonIdx + 1, iterTokens.end());
                        ASTNode* typeAST = generateAST(typeToks, depth + 1);
                        ASTNode* typeNode = typeAST->childNodes.empty() ? typeAST : typeAST->childNodes[0];
                        iteratorNode->childNodes.push_back(typeNode);
                    }
                }

                // Build range node from subTokens (collected after 'in')
                rangeNode = generateAST(subTokens, depth + 1)->childNodes[0];
                rangeNode->nodeType = Range_Node;

                gatherOptionalBraceBody(tokens, subTokens, i);
                bodyNode = makeScopeBodyNode(subTokens, depth);

                //// Only add iterator if one is explicity defined
                //if (iteratorNode->nodeType != Nothing_Node)
                node->childNodes.push_back(iteratorNode);
                node->childNodes.push_back(rangeNode);
                node->childNodes.push_back(bodyNode);

                break;
            }

            case Struct_Define: {
                node->nodeType = Struct_Define_Node;

                std::vector<asaToken*> subTokens = std::vector<asaToken*>();
                GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);
                node->childNodes.push_back(makeScopeBodyNode(subTokens, depth));
                break;
            }

            case Enum_Define: {
                // Gather the enum body between { } and parse each comma-separated enumVal.
                // Each enumVal is stored as a child of this Enum_Define_Node:
                //   - token = enumVal name
                //   - childNodes[0] = integer value node (if explicitly assigned)
                node->nodeType = Enum_Define_Node;

                std::vector<asaToken*> bodyTokens = std::vector<asaToken*>();
                GATHER_SCOPE_BODY(tokens, bodyTokens, 0, i, true);

                // Create a Scope_Body child to hold the { } tokens and enumVal nodes
                ASTNode* enumScopeBody = new ASTNode();
                ASTNodes.push_back(enumScopeBody);
                enumScopeBody->nodeType = Scope_Body;
                enumScopeBody->token = bodyTokens.front();        // {
                enumScopeBody->closingToken = bodyTokens.back();  // }
                node->childNodes.push_back(enumScopeBody);

                // Split inner tokens (excluding surrounding { }) by commas at depth 0
                int depth2 = 0;
                std::vector<asaToken*> enumValTokens;
                auto flushVariant = [&]() {
                    // Strip leading/trailing EOL/Nothing tokens
                    int s = 0, e = (int)enumValTokens.size() - 1;
                    while (s <= e && (enumValTokens[s]->tokenType == EndOfLine || enumValTokens[s]->tokenType == Nothing))
                        s++;
                    while (e >= s && (enumValTokens[e]->tokenType == EndOfLine || enumValTokens[e]->tokenType == Nothing))
                        e--;
                    if (s > e) {
                        enumValTokens.clear();
                        return;
                    }

                    ASTNode* enumValNode = new ASTNode();
                    ASTNodes.push_back(enumValNode);
                    // Find '=' if present
                    int eqIdx = -1;
                    for (int k = s; k <= e; k++)
                        if (enumValTokens[k]->tokenType == Equal) {
                            eqIdx = k;
                            break;
                        }

                    if (eqIdx == -1) {
                        // No explicit value: just the name
                        enumValNode->token = enumValTokens[s];
                        enumValNode->nodeType = Identifier_Node;
                    }
                    else {
                        // Name = value
                        enumValNode->token = enumValTokens[s];
                        enumValNode->nodeType = Identifier_Node;
                        // Parse the value expression
                        std::vector<asaToken*> valTokens(enumValTokens.begin() + eqIdx + 1, enumValTokens.begin() + e + 1);
                        ASTNode* valNode = generateAST(valTokens, depth + 1);
                        valNode->nodeType = Expression_Term;
                        valNode->codegen = &ASTNode::generateExpression;
                        enumValNode->childNodes.push_back(valNode);
                    }
                    enumScopeBody->childNodes.push_back(enumValNode);
                    enumValTokens.clear();
                };

                // Iterate inner tokens only (skip leading { and trailing })
                for (int bi = 1; bi < (int)bodyTokens.size() - 1; bi++) {
                    asaToken* t = bodyTokens[bi];
                    if (t->tokenType == Left_Brace || t->tokenType == Left_Paren || t->tokenType == Left_Bracket)
                        depth2++;
                    else if (t->tokenType == Right_Brace || t->tokenType == Right_Paren || t->tokenType == Right_Bracket)
                        depth2--;
                    else if (t->tokenType == Comma && depth2 == 0) {
                        flushVariant();
                        continue;
                    }
                    enumValTokens.push_back(t);
                }
                flushVariant();  // handle last enumVal (no trailing comma)

                break;
            }

            case Module_Define: {

                //ASTNode* bodyNode = new ASTNode();

                std::vector<asaToken*> subTokens = std::vector<asaToken*>();

                // Step through all tokens to gather body until braces are closed
                GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);

                generateAST(subTokens, depth + 1, node);

                node->nodeType = Module_Define_Node;
                node->token = token;
                node->codegen = &ASTNode::generateScopeBody;

                break;
            }


            // Operators:
            // builtin:
            case Colon:
            case Comma:
            case Dot_Dot:
            case Dot:
            case Dot_At:
            case Bang_Equal:
            case Equal_Equal:
            case Bang:
            case Bang_Bang:
            case Less:
            case Less_Equal:
            case Greater:
            case Greater_Equal:
            case Plus:
            case Minus:
            case Star:
            case Star_Star:
            case Slash:
            case Ref:
            case Const:
            case Exact:
            case Left_Bracket:
            case Arrow_Right:
            // general:
            case Bar:
            case Bar_Bar:
            case Ampersand:
            case Ampersand_Ampersand:
            case Tilde:
            case Tilde_Tilde:
            case Caret:
            case Shift_Left:
            case Shift_Right:
            case Caret_Caret:
            case Percent:
            case Percent_Percent:
            case Dollar:
            case At: {
                // Attribute syntax: @name: or @name(args):  - only at statement start
                if (tokenType == At && parentNode->leafNodes.size() == 0 &&
                    i + 1 < (int)tokens.size() && tokens[i + 1]->tokenType == Identifier) {

                    messageSystem::startBlock(node, "Parsing attribute", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
                    defer(messageSystem::endBlock());

                    // Consume the attribute name
                    asaToken* nameTok = NEXT_TOKEN(tokens, i);
                    node->nodeType = Attribute_Node;
                    node->token = nameTok;
                    // Optional argument list
                    if (i + 1 < (int)tokens.size() && tokens[i + 1]->tokenType == Left_Paren) {
                        std::vector<asaToken*> argTokens;
                        GATHER_PAREN_EXPRESSION(tokens, argTokens, 0, i, false);
                        ASTNode* argNode = generateAST(argTokens, depth + 1);
                        argNode->nodeType = Scope_Body;
                        argNode->codegen = &ASTNode::generateScopeBody;
                        node->childNodes.push_back(argNode);
                    }
                    // Consume the required trailing colon or semicolon
                    if (i + 1 < (int)tokens.size() && tokens[i + 1]->tokenType == Colon) {
                        i++;
                        pendingAttributes.push_back(node);
                        if (token->lineNumber > pendingCommentLastLine)
                            pendingCommentLastLine = token->lineNumber;
                        goto dontAddNodeForce;
                    }
                    else if (i + 1 < (int)tokens.size() && tokens[i + 1]->tokenType == Semi_Colon) {
                        i++;
                        node->nodeType = Standalone_Attribute_Node;
                        goto addNode;
                    }
                    else {
                        messageSystem::error("Expected ':' or ';' after attribute name", messageSystem::Syntax_Error);
                        wasError = true;
                        return nullptr;
                    }
                }
                // Fall through to operator handling
                [[fallthrough]];
            }
            case At_At: {
                bool isUnaryR = false;  // Operates on right
                bool isUnaryL = false;  // Left
                bool noOp = false;
                bool isGeneralOperator = false;
                if (operatorDefaultNodeType.find(tokenType) != operatorDefaultNodeType.end()) {
                    node->nodeType = operatorDefaultNodeType[tokenType];
                }
                else {
                    isGeneralOperator = true;
                    node->nodeType = Redefined_Operator_Expr;
                }

                ASTNode* firstTerm = new ASTNode();
                ASTNode* secondTerm = new ASTNode();
                bool isLeaf = true;
                bool isAccessOperation = node->nodeType == Access_Operation;

                // Instead of backtracking to get the first term, pop the leafNodes vector
                if (parentNode->leafNodes.size() == 0) {
                    // if there are no leaf nodes, then assume this is a unary operator on R
                    //printTokenError(tokenRange{token, token}, "Binary operator expected left argument");
                    //exit(1);
                    isUnaryR = true;
                    node->codegen = &ASTNode::generateUnaryExpression;
                }
                else {
                    firstTerm = parentNode->leafNodes.back();
                    parentNode->leafNodes.pop_back();
                    node->codegen = &ASTNode::generateBinaryExpression;
                }

                if (isUnaryR && tokenType == Ampersand)
                    node->nodeType = Address_Of_Operation;
                else if (isUnaryR && (tokenType == Star || tokenType == Star_Star))
                    node->nodeType = Dereference_Operation;

                // Step through all following tokens until parens are closed
                int parenLevel = 1;
                int braceLevel = 1;
                int bracketLevel = 1;
                std::vector<asaToken*> subTokens = std::vector<asaToken*>();
                if (!(isUnaryL == false && isUnaryR == false && isAccessOperation)) {
                    isAccessOperation = false;
                    for (;;) {
                        if (i >= tokens.size() - 1)
                            break;
                        asaToken* t = NEXT_TOKEN(tokens, i);

                        if (t->tokenType == Left_Paren)
                            parenLevel++;
                        if (t->tokenType == Right_Paren)
                            parenLevel--;
                        if (t->tokenType == Left_Brace)
                            braceLevel++;
                        if (t->tokenType == Right_Brace)
                            braceLevel--;
                        if (t->tokenType == Left_Bracket)
                            bracketLevel++;
                        if (t->tokenType == Right_Bracket)
                            bracketLevel--;

                        if (parenLevel == 0 && braceLevel == 0 && bracketLevel == 0)
                            break;
                        if (parenLevel == 1 && braceLevel == 1 && bracketLevel == 1 &&
                            (t->tokenType == Equal || t->tokenType == Plus_Equal || t->tokenType == Minus_Equal ||
                                t->tokenType == Times_Equal || t->tokenType == Slash_Equal ||
                                t->tokenType == Ampersand_Equal || t->tokenType == Bar_Equal ||
                                t->tokenType == Caret_Equal || t->tokenType == Shift_Left_Equal ||
                                t->tokenType == Shift_Right_Equal ||
                                t->tokenType == Colon_Colon)) {
                            i--;
                            break;
                        }
                        // For unary operators: stop at binary infix operators once we have a primary operand.
                        // This prevents `-5 == -5` from parsing as `-(5 == -5)`.
                        // Stopping only after the first token allows `- -5` and `-*ptr` to work correctly.
                        bool isTypeModifier = (node->nodeType == Const_Keyword || node->nodeType == Reference_Operation || node->nodeType == Exact_Type_Node);
                        if (isUnaryR && !subTokens.empty() &&
                            parenLevel == 1 && braceLevel == 1 && bracketLevel == 1 &&
                            ((!isTypeModifier && t->tokenType == Star) ||
                                t->tokenType == Plus || t->tokenType == Minus ||
                                t->tokenType == Slash || t->tokenType == Percent ||
                                t->tokenType == Equal_Equal || t->tokenType == Bang_Equal ||
                                t->tokenType == Less || t->tokenType == Greater ||
                                t->tokenType == Less_Equal || t->tokenType == Greater_Equal ||
                                t->tokenType == Ampersand_Ampersand || t->tokenType == Bar_Bar ||
                                t->tokenType == Ampersand || t->tokenType == Bar || t->tokenType == Caret ||
                                t->tokenType == Shift_Left || t->tokenType == Shift_Right ||
                                t->tokenType == Comma || t->tokenType == Dot_Dot ||
                                t->tokenType == Arrow_Right || t->tokenType == Colon_Colon)) {
                            i--;
                            break;
                        }
                        if (t->tokenType == EndOfLine || t->tokenType == Semi_Colon) {
                            if (braceLevel > 1) {
                                // Inside a { block } - keep token and continue collecting
                                subTokens.push_back(t);
                                continue;
                            }
                            if (t->tokenType == EndOfLine) {
                                TokenType lastTok = subTokens.empty() ? Nothing : subTokens.back()->tokenType;
                                TokenType nextTok = (i + 1 < (int)tokens.size()) ? tokens[i + 1]->tokenType : Nothing;
                                if (isLineContinuation(lastTok, nextTok))
                                    continue;
                            }
                            isLeaf = false;
                            break;
                        }

                        subTokens.push_back(t);
                    }
                }
                // If an access operation with brackets like: arr[i]
                else if (isAccessOperation) {
                    node->codegen = &ASTNode::generateAccessOperation;
                    for (;;) {
                        if (i >= tokens.size() - 1)
                            break;
                        asaToken* t = NEXT_TOKEN(tokens, i);

                        if (t->tokenType == Left_Paren)
                            parenLevel++;
                        if (t->tokenType == Right_Paren)
                            parenLevel--;
                        if (t->tokenType == Left_Brace)
                            braceLevel++;
                        if (t->tokenType == Right_Brace)
                            braceLevel--;
                        if (t->tokenType == Left_Bracket)
                            bracketLevel++;
                        if (t->tokenType == Right_Bracket)
                            bracketLevel--;

                        if (bracketLevel == 0)
                            break;
                        if (parenLevel == 1 && braceLevel == 1 && bracketLevel == 1 && t->tokenType == Equal) {
                            printTokenError(tokenRange {token, token}, "Missing closing bracket");
                            exit(1);
                        }

                        subTokens.push_back(t);
                    }
                }
                if (node->nodeType == Member_Access)
                    node->codegen = &ASTNode::generateMemberAccess;
                if (subTokens.size() == 0) {
                    if (!isUnaryR && tokenType == Star) {
                        node->nodeType = Pointer_Node;
                        isUnaryL = true;
                    }
                    // No expression operator $ when used in conjunction with pipe operator
                    else if (isUnaryR && tokenType == Dollar) {
                        node->nodeType = Pipe_Placeholder;
                        node->codegen = &ASTNode::generatePipePlaceholder;
                        noOp = true;
                    }
                    else {
                        printTokenError(tokenRange {token, token}, "Operator expected right argument");
                        exit(1);
                    }
                }
                else {
                    // For ':' (type annotation), use the dedicated type parser so that
                    // modifier combinations like `const exact ref *int` or `* ref * const *char`
                    // are handled correctly regardless of ordering and pointer depth.
                    if (tokenType == Colon) {
                        secondTerm = parseType(subTokens, depth + 1);
                        if (!secondTerm) {
                            printTokenError(tokenRange {subTokens[0], subTokens[0]}, "Expected type after ':'");
                            exit(1);
                        }
                    }
                    else {
                        ASTNode* secondAST = generateAST(subTokens, depth + 1);
                        if (secondAST->childNodes.size() > 0)
                            secondTerm = secondAST->childNodes[0];
                        else {
                            printTokenError(tokenRange {subTokens[0], subTokens[0]}, "Unexpected expression");
                            printAST(secondAST);
                            exit(1);
                        }
                    }
                }
                //secondTerm->nodeType = Expression_Term;

                if (!noOp) {
                    if (!isUnaryR)
                        node->childNodes.push_back(firstTerm);
                    if (!isUnaryL)
                        node->childNodes.push_back(secondTerm);
                }
                // ** and !! as double operators: wrap the inner op in an outer one of the same kind
                if ((tokenType == Star_Star || tokenType == Bang_Bang) && isUnaryR) {
                    ASTNode* outerOp = new ASTNode();
                    ASTNodes.push_back(outerOp);
                    outerOp->token = token;
                    outerOp->lineNumber = node->lineNumber;
                    outerOp->nodeType = node->nodeType;
                    outerOp->codegen = node->codegen;
                    outerOp->childNodes.push_back(node);
                    node = outerOp;
                }
                if (isLeaf)
                    goto addNodeAsLeaf;
                break;
            }

            case Plus_Plus:
            case Minus_Minus: {
                node->nodeType = Expression_Statement;
                node->codegen = &ASTNode::generateIncDecrement;

                if (parentNode->leafNodes.size() > 0) {
                    // Postfix: x++ / x--
                    node->isPostfix = true;
                    ASTNode* operand = parentNode->leafNodes.back();
                    parentNode->leafNodes.pop_back();
                    node->childNodes.push_back(operand);
                }
                else {
                    // Prefix: ++x / --x
                    node->isPostfix = false;
                    int parenLevel = 1;
                    int braceLevel = 1;
                    int bracketLevel = 1;
                    std::vector<asaToken*> subTokens;
                    for (;;) {
                        if (i >= (int)tokens.size() - 1)
                            break;
                        asaToken* t = NEXT_TOKEN(tokens, i);

                        if (t->tokenType == Left_Paren)
                            parenLevel++;
                        if (t->tokenType == Right_Paren)
                            parenLevel--;
                        if (t->tokenType == Left_Brace)
                            braceLevel++;
                        if (t->tokenType == Right_Brace)
                            braceLevel--;
                        if (t->tokenType == Left_Bracket)
                            bracketLevel++;
                        if (t->tokenType == Right_Bracket)
                            bracketLevel--;

                        if (!subTokens.empty() &&
                            parenLevel == 1 && braceLevel == 1 && bracketLevel == 1 &&
                            (t->tokenType == Plus || t->tokenType == Minus || t->tokenType == Star ||
                                t->tokenType == Slash || t->tokenType == Percent ||
                                t->tokenType == Equal_Equal || t->tokenType == Bang_Equal ||
                                t->tokenType == Less || t->tokenType == Greater ||
                                t->tokenType == Less_Equal || t->tokenType == Greater_Equal ||
                                t->tokenType == Ampersand_Ampersand || t->tokenType == Bar_Bar ||
                                t->tokenType == Ampersand || t->tokenType == Bar || t->tokenType == Caret ||
                                t->tokenType == Shift_Left || t->tokenType == Shift_Right ||
                                t->tokenType == Comma || t->tokenType == Dot_Dot || t->tokenType == Arrow_Right)) {
                            i--;
                            break;
                        }
                        if (t->tokenType == EndOfLine || t->tokenType == Semi_Colon) {
                            if (t->tokenType == EndOfLine) {
                                TokenType lastTok = subTokens.empty() ? Nothing : subTokens.back()->tokenType;
                                TokenType nextTok = (i + 1 < (int)tokens.size()) ? tokens[i + 1]->tokenType : Nothing;
                                if (isLineContinuation(lastTok, nextTok))
                                    continue;
                            }
                            break;
                        }

                        subTokens.push_back(t);
                    }
                    ASTNode* operand = generateAST(subTokens, depth + 1);
                    if (!operand || operand->childNodes.empty()) {
                        printTokenError(tokenRange {token, token}, "Operator expected argument");
                        wasError = true;
                        return nullptr;
                    }
                    node->childNodes.push_back(operand->childNodes[0]);
                }
                break;
            }

            case Plus_Equal:
            case Minus_Equal:
            case Times_Equal:
            case Slash_Equal:
            case Ampersand_Equal:
            case Bar_Equal:
            case Caret_Equal:
            case Shift_Left_Equal:
            case Shift_Right_Equal: {
                node->nodeType = Expression_Statement;
                node->codegen = &ASTNode::generateExpressionStatement;

                ASTNode* firstTerm = parentNode->leafNodes.back();
                parentNode->leafNodes.pop_back();

                std::vector<asaToken*> subTokens;
                GATHER_TO_SEMICOLON(tokens, subTokens, i, false);
                ASTNode* secondTerm = generateAST(subTokens, depth + 1);
                secondTerm->nodeType = Expression_Term;
                secondTerm->codegen = &ASTNode::generateExpression;

                node->childNodes.push_back(firstTerm);
                node->childNodes.push_back(secondTerm);
                break;
            }

            case Equal: {
                node->nodeType = Expression_Statement;
                node->codegen = &ASTNode::generateExpressionStatement;

                ASTNode* firstTerm = new ASTNode();
                ASTNode* secondTerm = new ASTNode();

                // Instead of backtracking to get the first term, pop the leafNodes vector =)
                firstTerm = parentNode->leafNodes.back();
                parentNode->leafNodes.pop_back();

                //firstTerm->lvalue = true;

                std::vector<asaToken*> subTokens = std::vector<asaToken*>();

                // Step through all following tokens until end of term
                GATHER_TO_SEMICOLON(tokens, subTokens, i, false);
                //int parenLevel = 1;
                //for (;;) {
                //  if (i >= tokens.size() - 1)
                //      break;
                //  asaToken t = NEXT_TOKEN(tokens, i);

                //  if (t->second == Left_Paren)
                //      parenLevel++;
                //  if (t->second == Right_Paren)
                //      parenLevel--;

                //  if ((t->second == EndOfLine || t->second == Semi_Colon))
                //      break;

                //  subTokens.push_back(t);
                //}
                secondTerm = generateAST(subTokens, depth + 1);
                secondTerm->nodeType = Expression_Term;
                secondTerm->codegen = &ASTNode::generateExpression;

                node->childNodes.push_back(firstTerm);
                node->childNodes.push_back(secondTerm);
                break;
            }

            case Hash: {
                node->nodeType = Compile_Time_Directive;

                ASTNode* identifier = new ASTNode();
                ASTNode* bodyNode = new ASTNode();
                bool isLeafNode = false;

                asaToken* tt = NEXT_TOKEN(tokens, i);
                identifier->token = tt;
                //identifier->tokenType = tt->second;
                identifier->nodeType = Identifier_Node;

                // Paren-enclosed argument - function-call-style inline directive (e.g. #directive(arg)).
                // Parse the parenthesized argument list with the same path as normal function calls.
                if (i + 1 < (int)tokens.size() && tokens[i + 1]->tokenType == Left_Paren) {
                    i++;
                    bool argsAreLeaf = true;
                    std::vector<ASTNode*> parsedArgs = parseParenthesizedArgumentNodes(tokens, i, depth + 1, argsAreLeaf);
                    ASTNode* argNode = new ASTNode();
                    argNode->nodeType = Scope_Body;
                    argNode->codegen = &ASTNode::generateScopeBody;
                    for (auto* arg : parsedArgs) {
                        arg->parentNode = argNode;
                        argNode->childNodes.push_back(arg);
                    }
                    node->token->tokenStr = "#" + identifier->token->tokenStr;
                    node->childNodes.push_back(identifier);
                    node->childNodes.push_back(argNode);
                    if (identifier->token->tokenStr == "cast")
                        node->codegen = &ASTNode::generateCast;
                    else if (identifier->token->tokenStr == "bitcast")
                        node->codegen = &ASTNode::generateBitcast;
                    else if (identifier->token->tokenStr == "stack_push")
                        node->codegen = &ASTNode::generateCompilerStackPushDirective;
                    else if (identifier->token->tokenStr == "stack_pop")
                        node->codegen = &ASTNode::generateCompilerStackPopDirective;
                    else if (identifier->token->tokenStr == "stack_last") {
                        node->codegen = &ASTNode::generateCompilerStackLastDirective;
                        node->returnsASTNode = true;
                        node->resolveASTNode = &ASTNode::resolveCompilerStackLastASTNode;
                    }
                    else if (identifier->token->tokenStr == "parent") {
                        node->returnsASTNode = true;
                        node->resolveASTNode = &ASTNode::resolveCompilerParentASTNode;
                    }
                    else if (identifier->token->tokenStr == "func_ast") {
                        node->returnsASTNode = true;
                        node->resolveASTNode = &ASTNode::resolveCompilerFuncASTNode;
                    }
                    else if (identifier->token->tokenStr == "context") {
                        node->returnsASTNode = true;
                        node->resolveASTNode = &ASTNode::resolveCompilerContextASTNode;
                    }
                    else if (identifier->token->tokenStr == "print_ast")
                        node->codegen = &ASTNode::generateCompilerPrintASTDirective;
                    else if (identifier->token->tokenStr == "print")
                        node->codegen = &ASTNode::generateCompilerPrintDirective;
                    else if (identifier->token->tokenStr == "printl")
                        node->codegen = &ASTNode::generateCompilerPrintLineDirective;
                    else if (identifier->token->tokenStr == "if")
                        node->codegen = &ASTNode::generateCompilerIfDirective;
                    else if (identifier->token->tokenStr == "error")
                        node->codegen = &ASTNode::generateCompilerErrorDirective;
                    else if (identifier->token->tokenStr == "warning")
                        node->codegen = &ASTNode::generateCompilerWarningDirective;
                    else if (identifier->token->tokenStr == "set_attribute")
                        node->codegen = &ASTNode::generateCompilerSetAttributeDirective;
                    else if (identifier->token->tokenStr == "getflag")
                        node->codegen = &ASTNode::generateCompilerGetFlagDirective;
                    else if (identifier->token->tokenStr == "nameof")
                        node->codegen = &ASTNode::generateNameofDirective;
                    else if (identifier->token->tokenStr == "make_directive" ||
                             identifier->token->tokenStr == "return" ||
                             identifier->token->tokenStr == "context")
                        node->codegen = &ASTNode::generateNothing;
                    goto addNodeAsLeaf;
                }

                // Only gather a body when the next token could plausibly start one
                // (identifier, literal, or opening brace).  Operator symbols,
                // commas, closing parens, and statement-enders mean the directive is
                // being used as a value expression (e.g. `#linenum > 0`) - consuming
                // further tokens would silently absorb the surrounding expression.
                if (i + 1 < (int)tokens.size()) {
                    TokenType nextTok = tokens[i + 1]->tokenType;
                    bool couldStartBody =
                        nextTok == Identifier || nextTok == Integer || nextTok == Float ||
                        nextTok == String || nextTok == Character ||
                        nextTok == Left_Brace;
                    if (!couldStartBody) {
                        node->token->tokenStr = "#" + identifier->token->tokenStr;
                        node->childNodes.push_back(identifier);
                        goto addNodeAsLeaf;  // treat as expression leaf so operators can use it as left operand
                    }
                }

                std::vector<asaToken*> subTokens = std::vector<asaToken*>();

                // Step through all following tokens until end of line via semicolon
                isLeafNode = GATHER_TO_SEMICOLON(tokens, subTokens, i, true, true);
                //GATHER_TO_SEMICOLON_OR_OTHER(tokens, subTokens, i, Left_Brace, true);

                // For #import / #import_qualified: convert .* into .(identifier "*") so wildcards parse correctly
                if (identifier->token->tokenStr == "import" || identifier->token->tokenStr == "import_qualified") {
                    for (int j = 1; j < (int)subTokens.size(); j++) {
                        if (subTokens[j]->tokenType == Star && subTokens[j - 1]->tokenType == Dot)
                            subTokens[j] = new asaToken("*", Identifier, subTokens[j]->lineNumber,
                                subTokens[j]->indexInLine,
                                subTokens[j]->lineValue,
                                subTokens[j]->filePath);
                    }
                }

                // For #extern, detect alias syntax: `CSymbol as AsaName :: (...)`
                // Strip the C symbol and `as` keyword so the AST sees only the ASA name.
                std::string externSymbolOverride = "";
                if (identifier->token->tokenStr == "extern") {
                    for (int j = 1; j < (int)subTokens.size(); j++) {
                        if (subTokens[j]->tokenStr == "as" && subTokens[j]->tokenType == Identifier) {
                            externSymbolOverride = subTokens[j - 1]->tokenStr;
                            subTokens.erase(subTokens.begin() + j - 1, subTokens.begin() + j + 1);
                            break;
                        }
                    }
                }

                bodyNode = makeScopeBodyNode(subTokens, depth);

                if (identifier->token->tokenStr == "cast") {
                    node->codegen = &ASTNode::generateCast;
                }
                if (identifier->token->tokenStr == "bitcast") {
                    node->codegen = &ASTNode::generateBitcast;
                }
                if (identifier->token->tokenStr == "extern") {
                    setChildrenAsExtern(bodyNode);
                    node->isExtern = true;
                    identifier->codegen = &ASTNode::generateNothing;
                    node->codegen = &ASTNode::generateScopeBody;
                    // Propagate the C symbol name to function declaration nodes
                    if (!externSymbolOverride.empty()) {
                        for (auto& child : bodyNode->childNodes)
                            child->externSymbolName = externSymbolOverride;
                    }
                }
                if (identifier->token->tokenStr == "new") {
                    node->codegen = &ASTNode::generateTypeInstance;
                }
                if (identifier->token->tokenStr == "setflag") {
                    node->codegen = &ASTNode::generateCompilerFlagDirective;
                    // Leaf nodes in a #setflag body are intentional tokens (name + value),
                    // not parse errors - move them into childNodes so findUnusedLeafNodes
                    // doesn't flag them, and collectTokens in generateCompilerFlagDirective finds them.
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "getflag") {
                    node->codegen = &ASTNode::generateCompilerGetFlagDirective;
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "nameof") {
                    node->codegen = &ASTNode::generateNameofDirective;
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "make_directive" || identifier->token->tokenStr == "return") {
                    node->codegen = &ASTNode::generateNothing;
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "stack_push") {
                    node->codegen = &ASTNode::generateCompilerStackPushDirective;
                    // Leaf nodes in a #stack_push body are intentional tokens (stack name + AST),
                    // not parse errors - move them into childNodes so findUnusedLeafNodes
                    // doesn't flag them, and collectTokens in generateCompilerStackPushDirective finds them.
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "stack_pop") {
                    node->codegen = &ASTNode::generateCompilerStackPopDirective;
                    // Leaf nodes in a #stack_pop body are intentional tokens (stack name),
                    // not parse errors - move them into childNodes so findUnusedLeafNodes
                    // doesn't flag them, and collectTokens in generateCompilerStackPopDirective finds them.
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "stack_last") {
                    node->codegen = &ASTNode::generateCompilerStackLastDirective;
                    node->returnsASTNode = true;
                    node->resolveASTNode = &ASTNode::resolveCompilerStackLastASTNode;
                    // Leaf nodes in a #stack_last body are intentional tokens (stack name),
                    // not parse errors - move them into childNodes so findUnusedLeafNodes
                    // doesn't flag them, and collectTokens in generateCompilerStackLastDirective finds them.
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "parent") {
                    node->returnsASTNode = true;
                    node->resolveASTNode = &ASTNode::resolveCompilerParentASTNode;
                }
                if (identifier->token->tokenStr == "func_ast") {
                    node->returnsASTNode = true;
                    node->resolveASTNode = &ASTNode::resolveCompilerFuncASTNode;
                }
                if (identifier->token->tokenStr == "context") {
                    node->returnsASTNode = true;
                    node->resolveASTNode = &ASTNode::resolveCompilerContextASTNode;
                }
                if (identifier->token->tokenStr == "print_ast") {
                    node->codegen = &ASTNode::generateCompilerPrintASTDirective;
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "print") {
                    node->codegen = &ASTNode::generateCompilerPrintDirective;
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "printl") {
                    node->codegen = &ASTNode::generateCompilerPrintLineDirective;
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "if") {
                    node->codegen = &ASTNode::generateCompilerIfDirective;
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "set_attribute") {
                    node->codegen = &ASTNode::generateCompilerSetAttributeDirective;
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "library") {
                    node->codegen = &ASTNode::generateLibraryDirective;
                    // The library name is a string literal - move it from leafNodes so
                    // findUnusedLeafNodes doesn't flag it and generateLibraryDirective can find it.
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }
                if (identifier->token->tokenStr == "library_static") {
                    node->codegen = &ASTNode::generateLibraryStaticDirective;
                    for (auto& l : bodyNode->leafNodes)
                        bodyNode->childNodes.push_back(l);
                    bodyNode->leafNodes.clear();
                }

                node->token->tokenStr = "#" + identifier->token->tokenStr;
                node->childNodes.push_back(identifier);
                node->childNodes.push_back(bodyNode);
                //if (isLeafNode)
                //  goto addNodeAsLeaf;
                break;
            }

            case Colon_Colon: {
                node->nodeType = Compiler_Define;

                ASTNode* identifier = new ASTNode();
                ASTNode* secondPart = new ASTNode();
                ASTNode* argumentsNode = new ASTNode();
                ASTNode* modifiersNode = new ASTNode();
                ASTNode* bodyNode = new ASTNode();
                std::vector<ASTNode*> arguments = std::vector<ASTNode*>();

                if (parentNode->leafNodes.size() > 0)
                    identifier = parentNode->leafNodes.back();
                else {
                    printTokenError(tokenRange {token, token}, "Expected a leaf node, but none were found", __LINE__);
                    exit(1);
                }
                parentNode->leafNodes.pop_back();
                ASTNode* qualifiedNameMarker = nullptr;
                if (identifier->nodeType == Member_Access) {
                    ASTNode* localName = getQualifiedLocalNameNode(identifier);
                    if (!localName) {
                        messageSystem::error("Invalid qualified compiler definition name", messageSystem::Syntax_Error);
                        wasError = true;
                        return nullptr;
                    }
                    qualifiedNameMarker = makeQualifiedNameMarker(identifier);
                    identifier = localName;
                }
                else if (identifier->nodeType != Operator_Overload_Node) {
                    identifier->nodeType = Identifier_Node;
                }
                argumentsNode->nodeType = Arguments;
                modifiersNode->nodeType = Compiler_Modifiers;

                // Based on the next token, decide what this compiler define does
                // (if function, or if/for/while etc. or any line of code)
                bool nonFunction = false;
                bool lParenReached = false;
                bool lParenClosed = false;
                bool parenHasContent = false;
                bool parenHasParamSyntax = false;
                bool tokensAfterParen = false;
                bool hasBrace = false;
                int signatureParenDepth = 0;
                for (int j = 0; j < tokens.size() - i; j++) {
                    TokenType lookaheadType = tokens[i + j]->tokenType;

                    if (lookaheadType == Left_Paren) {
                        if (!lParenReached) {
                            lParenReached = true;
                            signatureParenDepth = 1;
                        }
                        else if (!lParenClosed) {
                            signatureParenDepth++;
                            parenHasContent = true;
                        }
                    }
                    else if (lookaheadType == Right_Paren && lParenReached && !lParenClosed) {
                        signatureParenDepth--;
                        if (signatureParenDepth == 0)
                            lParenClosed = true;
                    }
                    else if (lParenReached && !lParenClosed &&
                             lookaheadType != Nothing && lookaheadType != EndOfLine && lookaheadType != Comment) {
                        parenHasContent = true;
                        if (signatureParenDepth == 1 &&
                            (lookaheadType == Colon || lookaheadType == Dot_Dot_Dot))
                            parenHasParamSyntax = true;
                    }
                    else if (lParenClosed &&
                             lookaheadType != Nothing && lookaheadType != EndOfLine && lookaheadType != Comment &&
                             lookaheadType != Semi_Colon && lookaheadType != Left_Brace) {
                        tokensAfterParen = true;
                    }

                    if (lookaheadType == Left_Brace) {
                        hasBrace = true;
                        if (lParenReached) {  // If first ( then {, this is a function
                            nonFunction = false;
                            break;
                        }
                        else {
                            nonFunction = true;
                            break;
                        }
                    }
                    // If semicolon without any brace: alias/define (no paren seen) or prototype (paren seen)
                    else if (lookaheadType == Semi_Colon) {
                        bool looksLikeParameterList = !parenHasContent || parenHasParamSyntax;
                        nonFunction = !lParenReached || !lParenClosed || tokensAfterParen || !looksLikeParameterList;
                        break;
                    }
                }

                // If the first token after :: is not a paren, or the token is in the map of non-function compiler defines
                asaToken* tt = NEXT_TOKEN(tokens, i);
                if (nonFunction || compileTimeDefinable.find(tt->tokenType) != compileTimeDefinable.end()) {
                    i--;  // NEXT_TOKEN==tt starts on first token after :: <here>
                    bool isCompileTimeDefinableKeyword = compileTimeDefinable.find(tt->tokenType) != compileTimeDefinable.end();
                    std::vector<asaToken*> subTokens = std::vector<asaToken*>();

                    if (!hasBrace && !isCompileTimeDefinableKeyword) {
                        // Simple compile-time alias/define: name :: expr;
                        // Gather the expression up to the semicolon (no brace body)
                        GATHER_TO_SEMICOLON(tokens, subTokens, i, false);
                        bool isSingleIdentifier = subTokens.size() == 1 && subTokens[0]->tokenType == Identifier;
                        bodyNode = generateAST(subTokens, depth + 1);
                        bodyNode->nodeType = Expression_Term;
                        bodyNode->codegen = &ASTNode::generateExpression;

                        node->token = identifier->token;
                        node->nodeType = Compiler_Define;
                        node->codegen = &ASTNode::generateNothing;
                        node->childNodes.push_back(bodyNode);
                        if (!qualifiedNameMarker)
                            parentNode->compilerDefinitions[identifier->token->tokenStr] = bodyNode;
                        // If the target is a plain identifier, treat it as a type alias
                        if (!qualifiedNameMarker && isSingleIdentifier)
                            registerTypeAlias(identifier->token->tokenStr, subTokens[0]->tokenStr);
                    }
                    else {
                        // Brace body: labeled loop, struct, block macro, or module
                        // Step through all following tokens until parens start OR braces start
                        int tokenNum = 0;
                        for (;;) {
                            if (i >= tokens.size() - 1) {
                                printTokenError(tokenRange {tt, tt}, "Unmatched parenthesis", __LINE__);
                                exit(1);
                            }
                            asaToken* t = NEXT_TOKEN(tokens, i);

                            if (t->tokenType == Left_Brace) {
                                i--;
                                break;
                            }

                            subTokens.push_back(t);

                            if (t->tokenType == Left_Paren) {
                                break;
                            }
                            if (tt->tokenType != EndOfLine)
                                tokenNum++;
                        }

                        // Step through all following tokens until braces start
                        tokenNum = 0;
                        for (;;) {
                            if (i >= tokens.size() - 1) {
                                break;
                                printTokenError(tokenRange {tt, tt}, "Unmatched brace", __LINE__);
                                exit(1);
                            }
                            asaToken* t = NEXT_TOKEN(tokens, i);

                            if (t->tokenType == Left_Brace) {
                                break;
                            }

                            subTokens.push_back(t);
                            if (tt->tokenType != EndOfLine)
                                tokenNum++;
                        }
                        i--;
                        // Step through all tokens to gather body until braces are closed
                        GATHER_SCOPE_BODY(tokens, subTokens, 0, i, isCompileTimeDefinableKeyword, true);

                        bodyNode = makeScopeBodyNode(subTokens, depth);
                        node->token = identifier->token;
                        if (tt->tokenType == Module_Define)
                            node->isModuleScope = true;
                        node->childNodes.push_back(bodyNode);

                        // Check for special definition types
                        if (bodyNode->childNodes.size() > 0) {
                            asaToken* s = bodyNode->childNodes[0]->token;

                            if (s->tokenStr == "struct") {
                                asaToken* structName = node->token;
                                node = bodyNode->childNodes[0];
                                node->token = structName;
                                node->nodeType = Compiler_Define_Struct;
                                node->codegen = &ASTNode::generateStruct;
                                // Carry variant params from identifier to struct node as childNodes[1]
                                {
                                    ASTNode* variantsNode = new ASTNode();
                                    variantsNode->nodeType = Variants_Node;
                                    if (!identifier->childNodes.empty() && identifier->childNodes[0]->nodeType == Variants_Node) {
                                        variantsNode = identifier->childNodes[0];
                                        identifier->childNodes.clear();
                                    }
                                    node->childNodes.push_back(variantsNode);
                                }
                            }
                            else if (s->tokenStr == "enum") {
                                asaToken* enumName = node->token;
                                node = bodyNode->childNodes[0];
                                node->token = enumName;
                                node->nodeType = Compiler_Define_Enum;
                                node->codegen = &ASTNode::generateEnum;
                            }
                            else if (s->tokenStr == "for" || s->tokenStr == "while") {
                                asaToken* labelName = node->token;
                                node = bodyNode;
                                node->token = labelName;
                                node->nodeType = Labeled_Loop;
                                node->codegen = &ASTNode::generateLabeledLoop;
                            }
                            else if (!node->isModuleScope) {
                                // General block macro: register in the enclosing scope
                                if (!qualifiedNameMarker)
                                    parentNode->compilerDefinitions[identifier->token->tokenStr] = bodyNode;
                                node->codegen = &ASTNode::generateNothing;
                            }
                        }
                    }
                }
                else {
                    node->nodeType = Compiler_Define_Function;
                    node->codegen = &ASTNode::generateFunction;
                    i--;
                    // Step through all following tokens until parens start
                    std::vector<asaToken*> subTokens = std::vector<asaToken*>();
                    for (;;) {
                        if (i >= tokens.size() - 1)
                            break;
                        asaToken* t = NEXT_TOKEN(tokens, i);

                        if (t->tokenType == Left_Paren)
                            break;

                        subTokens.push_back(t);
                    }
                    collapseVariantTypeTokens(subTokens);
                    secondPart = generateAST(subTokens, depth + 1);
                    secondPart->nodeType = Type_Node;
                    // Step through all following tokens until parens are closed
                    int parenLevel = 1;
                    int angleLevel = 0;
                    subTokens = std::vector<asaToken*>();
                    std::vector<ASTNode*> argumentDefaults;  // parallel to arguments, nullptr if no default
                    for (;;) {
                        if (i >= tokens.size() - 1)
                            break;
                        asaToken* t = NEXT_TOKEN(tokens, i);

                        if (t->tokenType == Left_Paren)
                            parenLevel++;
                        if (t->tokenType == Right_Paren)
                            parenLevel--;
                        if (t->tokenType == Less)
                            angleLevel++;
                        if (t->tokenType == Greater && angleLevel > 0)
                            angleLevel--;

                        if (parenLevel == 0 || t->tokenType == Semi_Colon)
                            break;
                        if (t->tokenType == EndOfLine)
                            continue;
                        // If comma and parenLevel is in same scope
                        if ((t->tokenType == Comma && parenLevel == 1 && angleLevel == 0)) {
                            parseParamWithDefault(subTokens, depth, arguments, argumentDefaults);
                            subTokens = std::vector<asaToken*>();
                            continue;
                        }

                        subTokens.push_back(t);
                    }
                    parseParamWithDefault(subTokens, depth, arguments, argumentDefaults);
                    subTokens = std::vector<asaToken*>();
                    //arguments = generateAST(subTokens, depth + 1);
                    //arguments->nodeType = Arguments;
                    for (int a = 0; a < arguments.size(); a++) {
                        //argumentsNode->childNodes.push_back(arguments[a]);
                        if (arguments[a]->leafNodes.size() == 2) {  // Make sure follows: <type> <identifier>  pattern
                            arguments[a]->leafNodes[0]->nodeType = Type_Node;
                            arguments[a]->childNodes.push_back(arguments[a]->leafNodes[0]);
                            arguments[a]->childNodes.push_back(arguments[a]->leafNodes[1]);
                            arguments[a]->leafNodes.pop_back();
                            arguments[a]->leafNodes.pop_back();
                        }
                        else if (arguments[a]->leafNodes.size() == 1 && arguments[a]->leafNodes[0]->nodeType == Argument_List) {  // If ...  pattern
                            arguments[a]->nodeType = Expression_Term;
                            arguments[a]->codegen = &ASTNode::generateExpression;
                            arguments[a]->childNodes = {arguments[a]->leafNodes[0]};
                            arguments[a]->leafNodes.pop_back();
                        }
                        else if (arguments[a]->leafNodes.size() == 0) {
                        }
                        else {
                            printTokenError(getASTTokenRange(arguments[a]->leafNodes[0]), "Expected type followed by identifier");
                            exit(1);
                        }
                        if (a < (int)argumentDefaults.size() && argumentDefaults[a] != nullptr)
                            arguments[a]->childNodes.push_back(argumentDefaults[a]);
                        argumentsNode->childNodes.push_back(arguments[a]);
                    }

                    // Get modifiers between ) and {
                    bool noModifiers = GATHER_TO_TOKEN(tokens, subTokens, 0, i, Left_Brace, true, true);
                    i--;
                    if (!noModifiers) {
                        modifiersNode = generateAST(subTokens, depth + 1);
                        modifiersNode->nodeType = Compiler_Modifiers;
                    }

                    // Step through all tokens to gather body until braces are closed
                    subTokens = std::vector<asaToken*>();
                    bool endedEarly = GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);

                    if (!endedEarly)
                        bodyNode = makeScopeBodyNode(subTokens, depth);
                    else
                        node->codegen = &ASTNode::generatePrototype;

                    node->token = identifier->token;
                    node->childNodes.push_back(identifier);
                    node->childNodes.push_back(secondPart);
                    node->childNodes.push_back(argumentsNode);
                    node->childNodes.push_back(modifiersNode);
                    // Variant params node [4] - always present, empty if no <...> were specified
                    {
                        ASTNode* variantsNode = new ASTNode();
                        variantsNode->nodeType = Variants_Node;
                        if (!identifier->childNodes.empty() && identifier->childNodes[0]->nodeType == Variants_Node) {
                            variantsNode = identifier->childNodes[0];
                            identifier->childNodes.clear();  // remove from identifier; lives on the function node
                        }
                        node->childNodes.push_back(variantsNode);
                    }
                    if (!endedEarly)
                        node->childNodes.push_back(bodyNode);

                    // Check for special function types
                    if (identifier->token->tokenStr == "cast") {
                        if (secondPart->childNodes.size() > 0)
                            node->token = secondPart->childNodes[0]->token;
                        else {
                            printTokenError(getASTTokenRange(secondPart), "Cast function must have a return type");
                            exit(1);
                        }
                        node->nodeType = Compiler_Define_Cast;
                    }
                    else if (identifier->token->tokenStr == "create") {
                        if (secondPart->childNodes.size() > 0) {
                            asaToken* rtTok = secondPart->childNodes[0]->token;
                            // Strip any variant-param suffix from the return type name
                            // so the function is stored under its raw struct name.
                            // e.g. "array.T" -> "array" for create<T> :: array<T>(...)
                            size_t dotPos = rtTok->tokenStr.find('.');
                            if (dotPos != std::string::npos) {
                                asaToken* baseTok = new asaToken(*rtTok);
                                baseTok->tokenStr = rtTok->tokenStr.substr(0, dotPos);
                                node->token = baseTok;
                            }
                            else {
                                node->token = rtTok;
                            }
                        }
                        else {
                            printTokenError(getASTTokenRange(secondPart), "Struct initializer function must have a return type");
                            exit(1);
                        }
                        node->nodeType = Compiler_Define_Function;
                    }
                }
                if (qualifiedNameMarker)
                    node->childNodes.push_back(qualifiedNameMarker);
                break;
            }

            case Operator_Keyword: {
                node->nodeType = Operator_Overload_Node;

                asaToken* openParen = NEXT_TOKEN(tokens, i);
                if (openParen->tokenType != Left_Paren) {
                    printTokenError(tokenRange {openParen, openParen}, "Operator overload must use parenthesized syntax: operator(<operator>)");
                    exit(1);
                }

                asaToken* t = NEXT_TOKEN(tokens, i);
                if (t->tokenType == Right_Paren || t->tokenType == EndOfLine || t->tokenType == EndOfFile) {
                    printTokenError(tokenRange {t, t}, "Operator overload requires an operator inside parentheses");
                    exit(1);
                }

                if (t->tokenType == Left_Bracket && i + 1 < (int)tokens.size() && tokens[i + 1]->tokenType == Right_Bracket) {
                    asaToken* syntheticBothBrackets = new asaToken(*t);
                    syntheticBothBrackets->tokenStr = tokenAsString(Both_Brackets);
                    syntheticBothBrackets->tokenType = Both_Brackets;
                    t = syntheticBothBrackets;
                    i++;
                }

                asaToken* closeParen = NEXT_TOKEN(tokens, i);
                if (closeParen->tokenType != Right_Paren) {
                    printTokenError(tokenRange {closeParen, closeParen}, "Operator overload expected ')' after operator token");
                    exit(1);
                }

                ASTNode* operatorNode = new ASTNode(Operator_Type_Node, {}, t);

                node->childNodes.push_back(operatorNode);
                goto addNodeAsLeaf;
            }

            case Left_Paren: {
                node->nodeType = Expression_Paren_Term;
                node->codegen = &ASTNode::generateExpression;

                ASTNode* previousTerm = new ASTNode();
                std::vector<ASTNode*> insideNodes = std::vector<ASTNode*>();
                bool isFunction = false;
                bool isLeaf = true;

                // Check the previous node, if it is an identifier then this must be a function call
                if (parentNode->leafNodes.size() > 0) {
                    previousTerm = parentNode->leafNodes.back();
                    parentNode->leafNodes.pop_back();
                    if (previousTerm->nodeType == Identifier_Node) {
                        node->nodeType = Function_Call;
                        node->codegen = &ASTNode::generateCallExpression;
                        node->token = previousTerm->token;
                        isFunction = true;
                    }
                }

                insideNodes = parseParenthesizedArgumentNodes(tokens, i, depth, isLeaf);

                if (isFunction) {
                    ASTNode* argumentsNode = new ASTNode();
                    argumentsNode->nodeType = Arguments;
                    for (int a = 0; a < insideNodes.size(); a++) {
                        argumentsNode->childNodes.push_back(insideNodes[a]);
                    }
                    node->childNodes.push_back(argumentsNode);
                    // Variant params node [1] - always present, empty if no <...> were specified
                    {
                        ASTNode* variantsNode = new ASTNode();
                        variantsNode->nodeType = Variants_Node;
                        if (previousTerm && !previousTerm->childNodes.empty() &&
                            previousTerm->childNodes[0]->nodeType == Variants_Node)
                            variantsNode = previousTerm->childNodes[0];
                        node->childNodes.push_back(variantsNode);
                    }
                }
                else {
                    if (insideNodes.size() == 1) {
                        insideNodes[0]->nodeType = Expression_Paren_Term;
                        insideNodes[0]->codegen = &ASTNode::generateExpression;
                        node = insideNodes[0];
                    }
                    else {
                        node->childNodes = insideNodes;
                    }
                }
                if (isLeaf)
                    goto addNodeAsLeaf;
                break;
            }

            case Right_Paren: {
                printTokenError(tokenRange {token, token}, "Unmatched paren", __LINE__, __FILE__);
                exit(1);
                break;
            }

            case Left_Brace: {
                asaToken* openBrace = token;
                std::vector<asaToken*> subTokens = std::vector<asaToken*>();

                // Gather inner content; i lands on } after this call
                GATHER_SCOPE_BODY(tokens, subTokens, 1, i, false);
                asaToken* closeBrace = tokens[i];  // } token

                // Pass end brace
                i++;

                node = generateAST(subTokens, depth + 1);
                node->nodeType = Scope_Body;
                node->codegen = &ASTNode::generateScopeBody;
                node->token = openBrace;
                node->closingToken = closeBrace;

                // If this block contains a `result` statement it produces a value and
                // must be treated as an expression leaf so binary operators (*, +, etc.)
                // can use it as their left operand.
                {
                    bool isValueBlock = false;
                    for (auto& c : node->childNodes) {
                        if (c->nodeType == Result_Node) {
                            isValueBlock = true;
                            break;
                        }
                    }
                    if (isValueBlock && !isScopeBody) {
                        node->isValueBlock = true;
                        i--;  // undo explicit i++ so for-loop's i++ lands on the next token
                        goto addNodeAsLeaf;
                    }
                }
                break;
            }
            case Right_Brace: {
                printTokenError(tokenRange {token, token}, "Unmatched brace", __LINE__, __FILE__);
                exit(1);
                break;
            }

            case Dot_Dot_Dot: {
                node->nodeType = Argument_List;
                parentNode->leafNodes.push_back(node);
                break;
            }

            // Constant literals
            case Integer: {
                node->nodeType = Integer_Node;
                goto generateConstantLiteral;
            }

            case Float: {
                node->nodeType = Float_Node;
                goto generateConstantLiteral;
            }

            case String: {
                node->nodeType = String_Constant_Node;
                goto generateConstantLiteral;
            }

            case Character: {
                node->nodeType = Character_Constant_Node;
                goto generateConstantLiteral;
            }

            case Question: {
                node->nodeType = Undefined_Initializer_Node;
                node->codegen = &ASTNode::generateConstant;
                parentNode->leafNodes.push_back(node);
                goto dontAddNode;
            }

            case Default_Keyword: {
                node->nodeType = Default_Initializer_Node;
                node->codegen = &ASTNode::generateConstant;
                parentNode->leafNodes.push_back(node);
                goto dontAddNode;
            }

            case Initial_Keyword: {
                node->nodeType = Initial_Initializer_Node;
                node->codegen = &ASTNode::generateConstant;
                parentNode->leafNodes.push_back(node);
                goto dontAddNode;
            }

            case True_Literal:
            case False_Literal: {
                node->nodeType = Boolean_Node;

            generateConstantLiteral:
                node->codegen = &ASTNode::generateConstant;
                parentNode->leafNodes.push_back(node);
                goto dontAddNode;
            }

            case Comment: {
                int commentLine = token->lineNumber;
                // Gap since last comment - reset block
                if (commentLine > pendingCommentLastLine + 1)
                    pendingCommentText.clear();
                // Strip // or /* */ markers
                std::string raw = token->tokenStr;
                std::string text;
                if (raw.size() >= 2 && raw[0] == '/' && raw[1] == '/') {
                    text = raw.substr(2);
                    while (!text.empty() && text.back() == ' ')
                        text.pop_back();
                }
                else if (raw.size() >= 2 && raw[0] == '/' && raw[1] == '*') {
                    text = raw.substr(2);
                    size_t endPos = text.rfind("*/");
                    if (endPos != std::string::npos)
                        text = text.substr(0, endPos);
                    while (!text.empty() && text.back() == ' ')
                        text.pop_back();
                }
                else {
                    text = raw;
                }
                if (!pendingCommentText.empty())
                    pendingCommentText += "\n" + text;
                else
                    pendingCommentText = text;
                pendingCommentLastLine = commentLine;
                goto dontAddNodeForce;
            }

            case Identifier: {
                if (i + 1 < (int)tokens.size() &&
                    tokens[i + 1]->tokenType == String &&
                    isAdjacentFStringPrefix(token, tokens[i + 1])) {
                    ASTNode* formattedStringNode = parseFStringLiteral(token, tokens[i + 1], depth);
                    if (formattedStringNode == nullptr)
                        return nullptr;
                    node = formattedStringNode;
                    i++;
                    goto addNodeAsLeaf;
                }

                node->nodeType = Identifier_Node;
                node->codegen = &ASTNode::generateVariableExpression;
                node->returnsASTNode = true;
                node->resolveASTNode = &ASTNode::resolveCompilerDefinitionASTNode;

                //// If this is identifer, and is preceded by identifier, then that one is type, and this is name
                //if (parentNode->leafNodes.size() >= 1) {
                //  if (parentNode->leafNodes.back()->nodeType == Identifier_Node) {
                //      ASTNode* typeNode = parentNode->leafNodes.back();
                //      parentNode->leafNodes.pop_back();
                //      typeNode->nodeType = Type_Node;
                //      typeNode->codegen = nullptr;
                //      node->childNodes.push_back(typeNode);
                //  }
                //}

                // Check for variant params <...> following this identifier (e.g. foo<int, 42>(...) or foo<T : int> :: ...)
                if (i + 1 < (int)tokens.size() && tokens[i + 1]->tokenType == Less) {
                    std::vector<std::vector<asaToken*>> variantGroups;
                    int j = i + 1;  // j points to '<'
                    if (tryConsumeVariantParams(tokens, j, variantGroups)) {
                        i = j;  // advance past '>'
                        ASTNode* variantsNode = new ASTNode();
                        variantsNode->nodeType = Variants_Node;
                        for (auto& grp : variantGroups) {
                            if (grp.empty())
                                continue;
                            ASTNode* paramNode = new ASTNode();
                            generateAST(grp, depth + 1, paramNode);
                            paramNode->nodeType = Variant_Param_Node;
                            paramNode->codegen = &ASTNode::generateExpression;
                            variantsNode->childNodes.push_back(paramNode);
                        }
                        node->childNodes.push_back(variantsNode);
                    }
                }

                parentNode->leafNodes.push_back(node);
                goto dontAddNode;
            }

            case Throw_Statement:
                node->nodeType = Throw_Node;
                node->codegen = &ASTNode::generateThrow;
                goto getStatementArgument;
            case Throw_Caller_Statement:
                node->nodeType = Throw_Caller_Node;
                node->codegen = &ASTNode::generateThrowCaller;
                goto getStatementArgument;
            case Return_Statement:
                node->nodeType = Return_Node;
                node->codegen = &ASTNode::generateReturn;
                goto getStatementArgument;
            case Result_Statement:
                node->nodeType = Result_Node;
                node->codegen = &ASTNode::generateResult;
                goto getStatementArgument;
            case Break_Statement:
                node->nodeType = Break_Node;
                node->codegen = &ASTNode::generateBreak;
                goto getStatementArgument;
            case Continue_Statement:
                node->nodeType = Continue_Node;
                node->codegen = &ASTNode::generateContinue;
                goto getStatementArgument;
            case Goto_Statement: {
                node->nodeType = Goto_Node;
            getStatementArgument:

                // Check if the next token is ':', which means someone wrote `keyword : type`
                // (using a reserved keyword as a variable name).
                {
                    int next = i + 1;
                    while (next < (int)tokens.size() &&
                           (tokens[next]->tokenType == Nothing || tokens[next]->tokenType == EndOfLine))
                        next++;
                    if (next < (int)tokens.size() && tokens[next]->tokenType == Colon) {
                        printTokenError(tokenRange {token, token},
                            "'" + token->tokenStr + "' is a reserved keyword and cannot be used as a variable name");
                        exit(1);
                    }
                }

                ASTNode* argumentTerm = new ASTNode();
                std::vector<asaToken*> subTokens = std::vector<asaToken*>();

                // Step through all following tokens until end of term
                GATHER_TO_SEMICOLON(tokens, subTokens, i, true);
                //for (;;) {
                //  if (i >= tokens.size() - 1)
                //      break;
                //  asaToken t = NEXT_TOKEN(tokens, i);

                //  if (t->second == Left_Paren)
                //      parenLevel++;
                //  if (t->second == Right_Paren)
                //      parenLevel--;

                //  if ((t->second == EndOfLine || t->second == Semi_Colon))
                //      break;

                //  subTokens.push_back(t);
                //}
                argumentTerm = generateAST(subTokens, depth + 1);
                argumentTerm->nodeType = Expression_Term;
                argumentTerm->codegen = &ASTNode::generateExpression;

                node->childNodes.push_back(argumentTerm);
                break;
            }

            case Semi_Colon: {
                for (int l = 0; l < parentNode->leafNodes.size(); l++) {
                    parentNode->childNodes.push_back(parentNode->leafNodes[l]);
                }
                for (int l = 0; l < parentNode->leafNodes.size(); l++)
                    parentNode->leafNodes.pop_back();
                goto dontAddNodeForce;
            }


            case EndOfFile:
            case EndOfLine:
            case Nothing: {
                goto dontAddNodeForce;
            }

            default: {
                printTokenError(tokenRange {token, token}, "No parser handling for token type: \"" + tokenAsString(tokenType) + "\"");
                wasError = true;
                return nullptr;
            }
        }


    addNode:
        if (!pendingCommentText.empty()) {
            if (node->lineNumber <= pendingCommentLastLine + 1) {
                ASTNode* commentNode = new ASTNode();
                commentNode->nodeType = Comment_Node;
                commentNode->token = new asaToken(toCStringLiteral(pendingCommentText), Comment);
                node->docComment = commentNode;
            }
            pendingCommentText.clear();
        }
        // When attributes are applied to a standalone scope body, distribute them
        // to each child rather than keeping them on the scope body itself.
        if (node->nodeType == Scope_Body && !pendingAttributes.empty()) {
            for (auto* child : node->childNodes) {
                for (auto* a : pendingAttributes)
                    inheritAttributeIfCompatible(child, a);
            }
            pendingAttributes.clear();
        }
        else {
            for (auto& a : pendingAttributes)
                node->attributes.push_back(a);
            pendingAttributes.clear();
        }
        parentNode->childNodes.push_back(node);
        continue;

    addNodeAsLeaf:
        parentNode->leafNodes.push_back(node);
        continue;

    dontAddNode:
        //// If it has a parent waiting for results, dont skip adding node
        //if (hasParent) {
        //  leafNodes.pop();
        //  printf("removed\n");
        //  goto addNode;
        //}
    dontAddNodeForce:
        continue;
    }

    // Warn about any attributes that were never attached to a node
    for (auto& a : pendingAttributes) {
        printTokenWarning(getASTTokenRange(a), "Attribute '@" + a->token->tokenStr + "' has nothing to attach to");
    }

    // If only one leaf node and no child nodes, it can be added as child instead
    if (parentNode->leafNodes.size() == 1 && parentNode->childNodes.size() == 0) {
        parentNode->childNodes.push_back(parentNode->leafNodes.back());
        parentNode->leafNodes.pop_back();
    }


    return parentNode;
}


// OPENCODE:
// Post-order tree rotation to respect operator precedence and associativity.
// Recurses children first, then rotates binary nodes: left-associative ops
// get left-rotated; general rotation checks left child (rotate right if
// currPrec > leftPrec) and right child (rotate left if currPrec >= rightPrec).
// Access_Operation is excluded from right-child rotation so a[i][j] stays
// nested. After each rotation, recurses on the new sub-node to cascade.
void fixPrecedence(ASTNode*& node)
{
    if (!node)
        return;

    // Recurse first on children
    for (auto& child : node->childNodes)
        fixPrecedence(child);

    // Handle only binary ops with two children
    if (operatorPrecedence.find(node->nodeType) != operatorPrecedence.end()) {
        if (node->childNodes.size() != 2)
            return;
        ASTNode* leftChild = node->childNodes[0];
        ASTNode* rightChild = node->childNodes[1];

        // Special handling: force left-associativity for these ops
        if (leftAssociativeOperators.count(node->nodeType)) {
            // If right child is same op: rotate left so (a op (b op c)) -> ((a op b) op c)
            if (rightChild->nodeType == node->nodeType && rightChild->childNodes.size() == 2) {
                ASTNode* A = leftChild;
                ASTNode* B = rightChild->childNodes[0];
                ASTNode* C = rightChild->childNodes[1];

                // New left: (A op B)
                ASTNode* newLeft = new ASTNode();
                *newLeft = *node;  // Copy node info (type, codegen, etc.)
                newLeft->childNodes.clear();
                newLeft->childNodes.push_back(A);
                newLeft->childNodes.push_back(B);

                // Rebuild this node as: (A op B) op C
                node->childNodes.clear();
                node->childNodes.push_back(newLeft);
                node->childNodes.push_back(C);

                fixPrecedence(newLeft);
            }
            //return;    // Finished for left-associative
        }

        // Usual precedence fix: check left, then right, just as before
        if (leftChild->childNodes.size() == 2 &&
            operatorPrecedence.find(leftChild->nodeType) != operatorPrecedence.end()) {
            int currPrec = operatorPrecedence[node->nodeType];
            int leftPrec = operatorPrecedence[leftChild->nodeType];
            // Rotate right for tighter right
            if (currPrec > leftPrec) {
                ASTNode* A = leftChild->childNodes[0];
                ASTNode* B = leftChild->childNodes[1];
                ASTNode* C = rightChild;

                ASTNode* newRight = new ASTNode();
                *newRight = *node;
                newRight->childNodes.clear();
                newRight->childNodes.push_back(B);
                newRight->childNodes.push_back(C);

                node->nodeType = leftChild->nodeType;
                node->codegen = leftChild->codegen;
                node->token = leftChild->token;
                node->childNodes.clear();
                node->childNodes.push_back(A);
                node->childNodes.push_back(newRight);

                fixPrecedence(newRight);
            }
        }
        if (node->nodeType != Access_Operation &&
            rightChild->childNodes.size() == 2 &&
            operatorPrecedence.find(rightChild->nodeType) != operatorPrecedence.end()) {
            int currPrec = operatorPrecedence[node->nodeType];
            int rightPrec = operatorPrecedence[rightChild->nodeType];
            // Rotate left: also handles equal-precedence to enforce left-to-right associativity
            if (currPrec >= rightPrec) {
                ASTNode* A = leftChild;
                ASTNode* B = rightChild->childNodes[0];
                ASTNode* C = rightChild->childNodes[1];

                ASTNode* newLeft = new ASTNode();
                *newLeft = *node;
                newLeft->childNodes.clear();
                newLeft->childNodes.push_back(A);
                newLeft->childNodes.push_back(B);

                node->nodeType = rightChild->nodeType;
                node->codegen = rightChild->codegen;
                node->token = rightChild->token;
                node->childNodes.clear();
                node->childNodes.push_back(newLeft);
                node->childNodes.push_back(C);

                fixPrecedence(newLeft);
            }
        }
    }
}


// OPENCODE:
// Collapses redundant single-child wrapper nodes: if a node's parent has only
// this one child and the same nodeType (and the child isn't a unary expression
// generator, to avoid merging *ptr dereference chains), the child replaces the
// parent in the grandparent's child list. Flattens chains like nested
// Scope_Body / Expression_Term wrappers produced by the greedy parse.
void unifyNodes(ASTNode*& node)
{
    if (node->parentNode != nullptr) {
        // If the parent only has one child (this) and is the same type, make them the same
        ASTNode* oldP = node->parentNode;
        if (oldP->childNodes.size() == 1 && node->nodeType == oldP->nodeType && node->codegen != &ASTNode::generateUnaryExpression) {
            ASTNode* p = node->parentNode->parentNode;
            node->parentNode = p;
            // Set oldP in new parents childnodes to this
            if (p != nullptr)
                for (auto& n : p->childNodes)
                    if (n == oldP)
                        n = node;
        }
    }

    for (int i = 0; i < node->childNodes.size(); i++) {
        unifyNodes(node->childNodes[i]);
    }
}

static void buildDeclMap(ASTNode* node, std::map<std::string, ASTNode*>& map);

// OPENCODE:
// Walks the AST propagating module/function context (for #modulename /
// #funcname). Rewrites compile-time directive nodes in-place into literal
// nodes: #linenum -> Integer_Node, #line -> String_Constant_Node, #filepath,
// #linecol, #funcname, #modulename, #asaversion, #counter. Also sets codegen
// or resolveASTNode pointers for #typeof, #sizeof, #compiles, #nameof,
// #parent, #func_ast, #context, #caller_*.
// TODO: This should not be its own pass in the future.
void resolveCompileTimeDirectives(ASTNode*& node, std::string moduleCtx, std::string funcCtx)
{
    // Propagate context downward: update for children before recursing
    std::string childModuleCtx = moduleCtx;
    std::string childFuncCtx = funcCtx;
    if (node->nodeType == Compiler_Define && node->isModuleScope)
        childModuleCtx = node->token->tokenStr;
    else if (!node->enclosingModule.empty())
        childModuleCtx = node->enclosingModule;
    if (node->nodeType == Compiler_Define_Function)
        childFuncCtx = node->token->tokenStr;

    for (int i = 0; i < node->childNodes.size(); i++) {
        resolveCompileTimeDirectives(node->childNodes[i], childModuleCtx, childFuncCtx);
    }

    if (node->nodeType == Compile_Time_Directive && node->childNodes.size() > 0) {

        messageSystem::startBlock(node, "Resolving compile time directive", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
        defer(messageSystem::endBlock());

        const std::string& name = node->childNodes[0]->token->tokenStr;
        if (name == "linenum") {
            node->nodeType = Integer_Node;
            node->token->tokenStr = std::to_string(node->token->lineNumber);
            node->codegen = &ASTNode::generateConstant;
            node->childNodes.clear();
        }
        else if (name == "line") {
            std::string lineStr = node->token->lineValue ? *node->token->lineValue : "";
            node->nodeType = String_Constant_Node;
            node->token->tokenStr = "\"" + lineStr + "\"";
            node->codegen = &ASTNode::generateConstant;
            node->childNodes.clear();
        }
        else if (name == "filepath") {
            std::string filePath = node->token->filePath ? *node->token->filePath : "";
            node->nodeType = String_Constant_Node;
            node->token->tokenStr = "\"" + filePath + "\"";
            node->codegen = &ASTNode::generateConstant;
            node->childNodes.clear();
        }
        else if (name == "linecol") {
            node->nodeType = Integer_Node;
            node->token->tokenStr = std::to_string(node->token->indexInLine);
            node->codegen = &ASTNode::generateConstant;
            node->childNodes.clear();
        }
        else if (name == "funcname") {
            if (funcCtx.empty()) {
                messageSystem::error("#funcname used outside of a function", messageSystem::Context_Info_Invalid_Location);
                return;
            }
            node->nodeType = String_Constant_Node;
            node->token->tokenStr = "\"" + funcCtx + "\"";
            node->codegen = &ASTNode::generateConstant;
            node->childNodes.clear();
        }
        else if (name == "modulename") {
            if (moduleCtx.empty()) {
                messageSystem::error("#modulename used outside of a module", messageSystem::Context_Info_Invalid_Location);
                return;
            }
            node->nodeType = String_Constant_Node;
            node->token->tokenStr = "\"" + moduleCtx + "\"";
            node->codegen = &ASTNode::generateConstant;
            node->childNodes.clear();
        }
        else if (name == "asaversion") {
            node->nodeType = String_Constant_Node;
            node->token->tokenStr = "\"" VERSION "\"";
            node->codegen = &ASTNode::generateConstant;
            node->childNodes.clear();
        }
        else if (name == "counter") {
            static int counterValue = 0;
            node->nodeType = Integer_Node;
            node->token->tokenStr = std::to_string(counterValue++);
            node->codegen = &ASTNode::generateConstant;
            node->childNodes.clear();
        }
        else if (name == "typeof") {
            node->codegen = &ASTNode::generateTypeofDirective;
        }
        else if (name == "sizeof") {
            node->codegen = &ASTNode::generateSizeofDirective;
        }
        else if (name == "compiles") {
            node->codegen = &ASTNode::generateCompilesDirective;
        }
        else if (name == "nameof") {
            node->codegen = &ASTNode::generateNameofDirective;
        }
        else if (name == "parent") {
            node->returnsASTNode = true;
            node->resolveASTNode = &ASTNode::resolveCompilerParentASTNode;
        }
        else if (name == "func_ast") {
            node->returnsASTNode = true;
            node->resolveASTNode = &ASTNode::resolveCompilerFuncASTNode;
        }
        else if (name == "context") {
            node->returnsASTNode = true;
            node->resolveASTNode = &ASTNode::resolveCompilerContextASTNode;
        }
        else if (name == "caller_filepath") {
            node->codegen = &ASTNode::generateCallerFilepathDirective;
        }
        else if (name == "caller_linenum") {
            node->codegen = &ASTNode::generateCallerLineNumDirective;
        }
        else if (name == "caller_line") {
            node->codegen = &ASTNode::generateCallerLineDirective;
        }
    }
}

// OPENCODE:
// Recursively builds a name -> ASTNode* map of all declarations
// (Compiler_Define, Compiler_Define_Function, Module_Define_Node,
// Struct_Define_Node, Compiler_Define_Enum) in the subtree. Used by
// resolveAttributeAccessImpl to look up symbols for .@ attribute access.
static void buildDeclMap(ASTNode* node, std::map<std::string, ASTNode*>& map)
{
    if (node->token && !node->token->tokenStr.empty()) {
        if (node->nodeType == Compiler_Define ||
            node->nodeType == Compiler_Define_Function ||
            node->nodeType == Module_Define_Node ||
            node->nodeType == Struct_Define_Node ||
            node->nodeType == Compiler_Define_Enum)
            map[node->token->tokenStr] = node;
    }
    for (auto* child : node->childNodes)
        buildDeclMap(child, map);
}

// OPENCODE:
// Recursive implementation of attribute-access resolution. For each
// Attribute_Access (.@) node with identifier.@identifier, looks up the symbol
// in declMap, finds the attribute on it, and rewrites the node to false
// (absent), true (present, no arg), or the constant argument value.
static void resolveAttributeAccessImpl(ASTNode*& node, const std::map<std::string, ASTNode*>& declMap)
{
    for (int i = 0; i < (int)node->childNodes.size(); i++)
        resolveAttributeAccessImpl(node->childNodes[i], declMap);

    if (node->nodeType != Attribute_Access || node->childNodes.size() < 2)
        return;

    ASTNode* leftNode = node->childNodes[0];
    ASTNode* attrNameNode = node->childNodes[1];

    if (leftNode->nodeType != Identifier_Node || attrNameNode->nodeType != Identifier_Node)
        return;

    const std::string& symName = leftNode->token->tokenStr;
    const std::string& attrName = attrNameNode->token->tokenStr;

    auto it = declMap.find(symName);
    if (it == declMap.end()) {
        printTokenError(getASTTokenRange(node), "Cannot find declaration '" + symName + "' for attribute access");
        return;
    }
    ASTNode* decl = it->second;

    // Find attribute by name
    ASTNode* foundAttr = nullptr;
    for (auto* attr : decl->attributes) {
        if (attr->token && attr->token->tokenStr == attrName) {
            foundAttr = attr;
            break;
        }
    }

    node->childNodes.clear();

    if (foundAttr == nullptr) {
        // Attribute not present -> false
        node->nodeType = Boolean_Node;
        node->token->tokenStr = "false";
        node->codegen = &ASTNode::generateConstant;
        return;
    }

    if (foundAttr->childNodes.empty()) {
        // Attribute present, no argument -> true
        node->nodeType = Boolean_Node;
        node->token->tokenStr = "true";
        node->codegen = &ASTNode::generateConstant;
        return;
    }

    // Attribute has argument: extract constant from Scope_Body child
    ASTNode* scopeBody = foundAttr->childNodes[0];
    if (scopeBody->childNodes.empty()) {
        node->nodeType = Boolean_Node;
        node->token->tokenStr = "true";
        node->codegen = &ASTNode::generateConstant;
        return;
    }

    ASTNode* constNode = scopeBody->childNodes[0];
    node->nodeType = constNode->nodeType;
    node->token->tokenStr = constNode->token->tokenStr;
    node->codegen = &ASTNode::generateConstant;
}

// OPENCODE:
// Entry point for attribute-access resolution: builds a declaration map from
// the root, then runs resolveAttributeAccessImpl to rewrite all sym.@attr
// nodes to their constant values.
void resolveAttributeAccess(ASTNode*& node)
{
    std::map<std::string, ASTNode*> declMap;
    buildDeclMap(node, declMap);
    resolveAttributeAccessImpl(node, declMap);
}


// Sets of attributes that cannot coexist on the same declaration.
// If any node has two or more attributes from the same set, it is an error.
static const std::vector<std::vector<std::string>> incompatibleAttributeSets = {
    {"public", "private"},
    {"inline", "noinline"},
    {"deprecated", "removed"},
    {"external", "internal"},
};

// OPENCODE:
// Returns true if two attribute names conflict — either they're the same, or
// they belong to the same incompatible set (e.g. public/private).
static bool attributeNamesConflict(const std::string& left, const std::string& right)
{
    if (left == right)
        return true;

    for (const auto& group : incompatibleAttributeSets) {
        bool hasLeft = false;
        bool hasRight = false;
        for (const auto& name : group) {
            hasLeft = hasLeft || name == left;
            hasRight = hasRight || name == right;
        }
        if (hasLeft && hasRight)
            return true;
    }

    return false;
}

// OPENCODE:
// Returns true if node can accept attr without conflict — checks against all
// existing attributes on the node using attributeNamesConflict.
static bool canInheritAttribute(ASTNode* node, ASTNode* attr)
{
    if (!node || !attr || !attr->token)
        return false;

    for (auto* existingAttr : node->attributes) {
        if (!existingAttr || !existingAttr->token)
            continue;
        if (attributeNamesConflict(existingAttr->token->tokenStr, attr->token->tokenStr))
            return false;
    }

    return true;
}

// OPENCODE:
// Adds attr to node's attributes list if canInheritAttribute says it's
// compatible. Used when distributing pending attributes across scope-body
// children.
static void inheritAttributeIfCompatible(ASTNode* node, ASTNode* attr)
{
    if (canInheritAttribute(node, attr))
        node->attributes.push_back(attr);
}

// OPENCODE:
// Extracts the rightmost (local) identifier from a Member_Access chain
// (e.g. Owner.member -> member). Used during qualified-definition parsing.
static ASTNode* getQualifiedLocalNameNode(ASTNode* node)
{
    if (!node || node->nodeType != Member_Access || node->childNodes.size() != 2)
        return nullptr;

    ASTNode* localName = node->childNodes[1];
    while (localName && localName->nodeType == Member_Access && localName->childNodes.size() == 2)
        localName = localName->childNodes[1];
    return localName;
}

// OPENCODE:
// Creates a __qualified_define_name marker node that tags a qualified
// definition (Owner.member :: ...) so normalizeQualifiedCompilerDefinitions
// can later relocate it into the owner's scope.
static ASTNode* makeQualifiedNameMarker(ASTNode* qualifiedName)
{
    ASTNode* marker = new ASTNode();
    ASTNodes.push_back(marker);
    marker->nodeType = Nothing_Node;
    marker->label = "__qualified_define_name";
    marker->childNodes.push_back(qualifiedName);
    return marker;
}

// OPENCODE:
// Navigates the module nesting: Compiler_Define -> Scope_Body ->
// Module_Define_Node -> inner Scope_Body. Returns the inner scope body of a
// named module, or nullptr if the structure doesn't match.
static ASTNode* getModuleInnerScope(ASTNode* node)
{
    if (!node || !node->isModuleScope || node->childNodes.empty())
        return nullptr;

    ASTNode* outerScope = node->childNodes[0];
    if (!outerScope || outerScope->nodeType != Scope_Body || outerScope->childNodes.empty())
        return nullptr;

    ASTNode* moduleNode = outerScope->childNodes[0];
    if (!moduleNode || moduleNode->nodeType != Module_Define_Node || moduleNode->childNodes.empty())
        return nullptr;

    ASTNode* innerScope = moduleNode->childNodes[0];
    return innerScope && innerScope->nodeType == Scope_Body ? innerScope : nullptr;
}

// OPENCODE:
// Gets the inner Scope_Body of a struct or module node. Handles
// Compiler_Define_Struct, Compiler_Define (module), and Module_Define_Node.
// Returns nullptr if the node is neither or has no body.
static ASTNode* getContainerScope(ASTNode* node)
{
    if (!node)
        return nullptr;

    if (node->nodeType == Compiler_Define_Struct) {
        if (!node->childNodes.empty() && node->childNodes[0]->nodeType == Scope_Body)
            return node->childNodes[0];
        return nullptr;
    }

    if (node->nodeType == Compiler_Define && node->isModuleScope)
        return getModuleInnerScope(node);

    if (node->nodeType == Module_Define_Node && !node->childNodes.empty() && node->childNodes[0]->nodeType == Scope_Body)
        return node->childNodes[0];

    return nullptr;
}

// OPENCODE:
// Searches scope's childNodes (up to beforeNode) for an earlier definition
// matching name. Used by resolveQualifiedOwnerInContainer to find the owner
// of a qualified definition.
static ASTNode* findEarlierContainedDefinition(ASTNode* scope, ASTNode* beforeNode, const std::string& name)
{
    if (!scope)
        return nullptr;

    for (ASTNode* child : scope->childNodes) {
        if (child == beforeNode)
            break;
        if (child && child->token && child->token->tokenStr == name &&
            (child->nodeType == Compiler_Define ||
                child->nodeType == Compiler_Define_Struct ||
                child->nodeType == Compiler_Define_Enum ||
                child->nodeType == Compiler_Define_Function ||
                child->nodeType == Compiler_Define_Cast))
            return child;
    }

    return nullptr;
}

// OPENCODE:
// Recursively resolves an owner expression (Identifier or Member_Access chain)
// within a container scope, descending into sub-scopes via getContainerScope.
// Returns the ASTNode of the owner definition, or nullptr if not found.
static ASTNode* resolveQualifiedOwnerInContainer(ASTNode* container, ASTNode* ownerExpr)
{
    if (!container || !ownerExpr)
        return nullptr;

    if (ownerExpr->nodeType == Identifier_Node && ownerExpr->token)
        return findEarlierContainedDefinition(container, nullptr, ownerExpr->token->tokenStr);

    if (ownerExpr->nodeType != Member_Access || ownerExpr->childNodes.size() != 2)
        return nullptr;

    ASTNode* leftOwner = resolveQualifiedOwnerInContainer(container, ownerExpr->childNodes[0]);
    ASTNode* leftScope = getContainerScope(leftOwner);
    if (!leftScope)
        return nullptr;

    ASTNode* right = ownerExpr->childNodes[1];
    if (!right || right->nodeType != Identifier_Node || !right->token)
        return nullptr;

    return findEarlierContainedDefinition(leftScope, nullptr, right->token->tokenStr);
}

// OPENCODE:
// Walks up the scope chain from definitionNode searching for the owner of a
// qualified definition (Owner.member :: ...). Tries each enclosing scope
// until the owner is found or the root is reached.
static ASTNode* resolveQualifiedOwner(ASTNode* definitionNode, ASTNode* ownerExpr)
{
    if (!definitionNode || !ownerExpr)
        return nullptr;

    if (ownerExpr->nodeType != Identifier_Node && ownerExpr->nodeType != Member_Access)
        return nullptr;

    ASTNode* scope = definitionNode->parentNode;
    ASTNode* beforeNode = definitionNode;

    while (scope) {
        if (ownerExpr->nodeType == Identifier_Node && ownerExpr->token) {
            ASTNode* found = findEarlierContainedDefinition(scope, beforeNode, ownerExpr->token->tokenStr);
            if (found)
                return found;
        }
        else if (ownerExpr->nodeType == Member_Access) {
            ASTNode* found = resolveQualifiedOwnerInContainer(scope, ownerExpr);
            if (found)
                return found;
        }

        beforeNode = scope;
        scope = scope->parentNode;
    }

    return nullptr;
}

// OPENCODE:
// Recursively builds a dotted path string from an Identifier or Member_Access
// chain (e.g. A.B.C -> "A.B.C"). Returns true on success.
static bool getQualifiedPathString(ASTNode* node, std::string& out)
{
    if (!node || !node->token)
        return false;

    if (node->nodeType == Identifier_Node) {
        out = node->token->tokenStr;
        return true;
    }

    if (node->nodeType != Member_Access || node->childNodes.size() != 2)
        return false;

    std::string left;
    std::string right;
    if (!getQualifiedPathString(node->childNodes[0], left) ||
        !getQualifiedPathString(node->childNodes[1], right))
        return false;

    out = left + "." + right;
    return true;
}

// OPENCODE:
// Finds and removes the __qualified_define_name marker from node's children,
// returning it. Used by normalizeQualifiedCompilerDefinitions to extract the
// owner expression from a qualified definition.
static ASTNode* takeQualifiedNameMarker(ASTNode* node)
{
    if (!node)
        return nullptr;

    for (int i = 0; i < (int)node->childNodes.size(); i++) {
        ASTNode* child = node->childNodes[i];
        if (child && child->nodeType == Nothing_Node && child->label == "__qualified_define_name") {
            node->childNodes.erase(node->childNodes.begin() + i);
            return child;
        }
    }

    return nullptr;
}

// OPENCODE:
// Re-registers a moved definition in the target scope's compilerDefinitions
// map so compile-time name resolution finds it at its new location.
static void registerMovedCompilerDefinition(ASTNode* targetScope, ASTNode* definitionNode)
{
    if (!targetScope || !definitionNode || !definitionNode->token)
        return;

    if (definitionNode->nodeType == Compiler_Define && !definitionNode->childNodes.empty())
        targetScope->compilerDefinitions[definitionNode->token->tokenStr] = definitionNode->childNodes[0];
}

// OPENCODE:
// Relocates qualified definitions (Owner.member :: ...) into their owner's
// scope. For each child, checks for a __qualified_define_name marker; if
// found, resolves the owner via resolveQualifiedOwner, moves the definition
// node from its current parent into the owner's inner scope, sets
// enclosingModule for module members, and re-registers it via
// registerMovedCompilerDefinition.
void normalizeQualifiedCompilerDefinitions(ASTNode*& node)
{
    if (!node)
        return;

    for (int i = 0; i < (int)node->childNodes.size();) {
        ASTNode* child = node->childNodes[i];
        ASTNode* marker = takeQualifiedNameMarker(child);

        if (!marker) {
            normalizeQualifiedCompilerDefinitions(child);
            i++;
            continue;
        }

        if (marker->childNodes.empty() ||
            marker->childNodes[0]->nodeType != Member_Access ||
            marker->childNodes[0]->childNodes.size() != 2) {
            messageSystem::error("Invalid qualified compiler definition name", messageSystem::Syntax_Error);
            wasError = true;
            i++;
            continue;
        }

        ASTNode* ownerExpr = marker->childNodes[0]->childNodes[0];
        ASTNode* owner = resolveQualifiedOwner(child, ownerExpr);
        ASTNode* targetScope = getContainerScope(owner);
        if (!owner || !targetScope) {
            messageSystem::error("Qualified compiler definition owner must be an earlier struct or module definition", messageSystem::Syntax_Error);
            wasError = true;
            i++;
            continue;
        }

        node->childNodes.erase(node->childNodes.begin() + i);
        child->parentNode = targetScope;
        if (owner->nodeType == Compiler_Define && owner->isModuleScope &&
            (child->nodeType == Compiler_Define_Function || child->nodeType == Compiler_Define_Cast)) {
            std::string ownerPath;
            if (getQualifiedPathString(ownerExpr, ownerPath))
                child->enclosingModule = ownerPath;
        }
        targetScope->childNodes.push_back(child);
        registerMovedCompilerDefinition(targetScope, child);
    }
}

static const std::unordered_set<ASTNodeType> literalNodeTypes = {
    Integer_Node,
    Float_Node,
    Boolean_Node,
    String_Constant_Node,
};

// OPENCODE:
// Validates attributes on every node in the tree: (1) attribute arguments
// must be literal constants, (2) no two attributes from the same incompatible
// set (public/private, inline/noinline, etc.), (3) no duplicate attributes.
// Run twice from main.cpp — before and after processCompilerDirectives.
void checkAttributeCompatibility(ASTNode* node)
{
    messageSystem::startBlock(node, "Checking attribute compatability", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);

    for (auto* child : node->childNodes) {
        checkAttributeCompatibility(child);
    }

    if (node->attributes.empty()) {
        messageSystem::endBlock();
        return;
    }

    // Validate attribute arguments
    for (auto* attr : node->attributes) {
        if (!attr->token || attr->token->tokenStr.empty())
            continue;

        // Ensure argument is a literal constant if one is present
        if (!attr->childNodes.empty()) {
            ASTNode* scopeBody = attr->childNodes[0];

            messageSystem::startBlock(scopeBody, "Checking attribute arguments", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);

            if (scopeBody->childNodes.empty() || literalNodeTypes.find(scopeBody->childNodes[0]->nodeType) == literalNodeTypes.end()) {
                messageSystem::error("Argument to attribute '@" + attr->token->tokenStr + "' must be a compile-time constant (int, float, bool, or string)", messageSystem::Invalid_Attribute_Arguments_Error);
                return;
            }

            messageSystem::endBlock();
        }
    }

    // Then make sure the node doesnt have attributes that are incompatible with each other:
    for (const auto& group : incompatibleAttributeSets) {
        std::vector<ASTNode*> conflicts;
        for (const auto& name : group)
            for (const auto& a : node->attributes) {
                if (a->token->tokenStr == name)
                    conflicts.push_back(a);
            }

        if (conflicts.size() >= 2) {
            std::string msg = "Incompatible attributes on '" + node->token->tokenStr + "': @" + conflicts[0]->token->tokenStr;
            for (int i = 1; i < (int)conflicts.size(); i++)
                msg += " and @" + conflicts[i]->token->tokenStr;

            messageSystem::addAttributes(conflicts);
            messageSystem::error(msg, messageSystem::Incompatible_Attribute_Error);
            return;
        }
    }

    // Finally, error if the node uses an attribute more than once
    {
        std::vector<std::string> conflicts;
        for (const auto& a : node->attributes)
            for (const auto& b : node->attributes) {
                if (a == b)
                    continue;
                if (a->token->tokenStr == b->token->tokenStr)
                    conflicts.push_back(a->token->tokenStr);
            }

        if (conflicts.size() > 0) {
            std::string msg = "Duplicate attributes on '" + node->token->tokenStr + "': @" + conflicts[0];
            for (int i = 1; i < (int)conflicts.size(); i++)
                msg += ", and @" + conflicts[i];

            messageSystem::error(msg, messageSystem::Duplicate_Attribute_Error);
            return;
        }
    }

    messageSystem::endBlock();
    return;
}

// OPENCODE:
// Constant-folding pass: if both children of a binary op are literals
// (Integer/Float/Boolean), folds +, -, *, / into a single literal node.
// Unwraps single-child paren terms wrapping literals. Currently disabled
// (commented out in main.cpp).
void optimizeASTNode(ASTNode*& node)
{
    for (int i = 0; i < node->childNodes.size(); i++) {
        optimizeASTNode(node->childNodes[i]);
    }

    // If binary operator
    if (node->childNodes.size() >= 2) {
        if (operatorPrecedence.find(node->nodeType) != operatorPrecedence.end()) {
            ASTNode* first = node->childNodes[0];
            ASTNode* second = node->childNodes[1];

            bool first_b = false;
            int first_i = 0;
            double first_f = 0;
            void* first_val_ptr = &first_b;
            ASTNodeType first_type = Boolean_Node;

            bool second_b = false;
            int second_i = 0;
            double second_f = 0;
            void* second_val_ptr = &second_b;
            ASTNodeType second_type = Boolean_Node;

            bool output_b = false;
            int output_i = 0;
            double output_f = 0;
            void* output_val_ptr = &output_b;
            ASTNodeType output_type = Boolean_Node;

            bool firstIsLiteral = false;
            bool secondIsLiteral = false;

            // If the first node is a literal
            if (literals.find(first->nodeType) != literals.end()) {
                firstIsLiteral = true;
                switch (first->nodeType) {
                    case Boolean_Node:
                        first_val_ptr = &first_b;
                        first_b = first->token->tokenStr == "true" ? true : false;
                        output_val_ptr = &output_b;
                        output_type = Boolean_Node;
                        first_type = Boolean_Node;
                        break;
                    case Integer_Node:
                        first_val_ptr = &first_i;
                        first_i = std::stoi(first->token->tokenStr);
                        output_val_ptr = &output_i;
                        output_type = Integer_Node;
                        first_type = Integer_Node;
                        break;
                    case Float_Node:
                        first_val_ptr = &first_f;
                        first_f = std::stod(first->token->tokenStr);
                        output_val_ptr = &output_f;
                        output_type = Float_Node;
                        first_type = Float_Node;
                        break;
                    default:
                        firstIsLiteral = false;
                }
            }
            // If the second node is a literal
            if (literals.find(second->nodeType) != literals.end()) {
                secondIsLiteral = true;
                switch (second->nodeType) {
                    case Boolean_Node:
                        second_val_ptr = &second_b;
                        second_b = second->token->tokenStr == "true" ? true : false;
                        second_type = Boolean_Node;
                        break;
                    case Integer_Node:
                        second_val_ptr = &second_i;
                        second_i = std::stoi(second->token->tokenStr);
                        if (output_type != Float_Node) {
                            output_val_ptr = &output_i;
                            output_type = Integer_Node;
                        }
                        second_type = Integer_Node;
                        break;
                    case Float_Node:
                        second_val_ptr = &second_f;
                        second_f = std::stod(second->token->tokenStr);
                        output_val_ptr = &output_f;
                        output_type = Float_Node;
                        second_type = Float_Node;
                        break;
                    default:
                        secondIsLiteral = false;
                }
            }

            if (output_type == Float_Node) {
                if (first_type == Boolean_Node) {
                    first_type = Float_Node;
                    first_f = (double)first_b;
                    first_val_ptr = &first_f;
                }
                if (first_type == Integer_Node) {
                    first_type = Float_Node;
                    first_f = (double)first_i;
                    first_val_ptr = &first_f;
                }
                if (second_type == Boolean_Node) {
                    second_type = Float_Node;
                    second_f = (double)second_b;
                    second_val_ptr = &second_f;
                }
                if (second_type == Integer_Node) {
                    second_type = Float_Node;
                    second_f = (double)second_i;
                    second_val_ptr = &second_f;
                }
            }
            else if (output_type == Integer_Node) {
                if (first_type == Boolean_Node) {
                    first_type = Integer_Node;
                    first_i = (int)first_b;
                    first_val_ptr = &first_i;
                }
                if (second_type == Boolean_Node) {
                    second_type = Integer_Node;
                    second_i = (int)second_b;
                    second_val_ptr = &second_i;
                }
            }

            if (firstIsLiteral && secondIsLiteral) {

                std::string outString;

                // If float output
                if (output_type == Float_Node) {
                    switch (node->nodeType) {
                        case (Expression_Times):
                            output_f = (*(double*)first_val_ptr) * (*(double*)second_val_ptr);
                            break;
                        case (Expression_Divide):
                            output_f = (*(double*)first_val_ptr) / (*(double*)second_val_ptr);
                            break;
                        case (Expression_Plus):
                            output_f = (*(double*)first_val_ptr) + (*(double*)second_val_ptr);
                            break;
                        case (Expression_Minus):
                            output_f = (*(double*)first_val_ptr) - (*(double*)second_val_ptr);
                            break;
                    }
                    outString = std::to_string(output_f);
                }
                // If int output
                else if (output_type == Integer_Node) {
                    switch (node->nodeType) {
                        case (Expression_Times):
                            output_i = (*(int*)first_val_ptr) * (*(int*)second_val_ptr);
                            break;
                        case (Expression_Divide):
                            output_i = (*(int*)first_val_ptr) / (*(int*)second_val_ptr);
                            break;
                        case (Expression_Plus):
                            output_i = (*(int*)first_val_ptr) + (*(int*)second_val_ptr);
                            break;
                        case (Expression_Minus):
                            output_i = (*(int*)first_val_ptr) - (*(int*)second_val_ptr);
                            break;
                    }
                    outString = std::to_string(output_i);
                }


                node->childNodes = std::vector<ASTNode*>();
                node->nodeType = output_type;
                switch (output_type) {
                    case Integer_Node:
                    case Boolean_Node:
                        node->token->tokenType = Integer;
                        break;
                    case Float_Node:
                        node->token->tokenType = Float;
                        break;
                    default:
                        break;
                }
                node->token->tokenStr = outString;
            }
        }
    }
    // Else if a paren term
    else if (node->nodeType == Expression_Paren_Term) {
        // If this paren term only has one child, and that child is any type of literal,
        // the paren can be removed
        if (node->childNodes.size() == 1 && literals.find(node->childNodes[0]->nodeType) != literals.end()) {
            node = node->childNodes[0];
        }
    }
    // Else if it is an expression term
    else if (node->nodeType == Expression_Term) {
        // If there are no children, this expression can be removed
    }
}

// OPENCODE:
// Processes #embed "file" directives: loads the file, tokenizes and parses it,
// and pushes its top-level child nodes into the importedNodes accumulator.
// Dedups via importedFileNames. Recurses into children first.
void addFileIncludes(ASTNode*& node)
{
    for (int i = 0; i < node->childNodes.size(); i++)
        addFileIncludes(node->childNodes[i]);

    if (node->childNodes.size() > 0)
        if (node->nodeType == Compile_Time_Directive) {
            if (node->childNodes[0]->token->tokenStr == "embed") {
                std::string fileString = "";
                std::string fileName = "";

                // Load file if provided
                if (node->childNodes.size() > 1) {
                    ASTNode* strChild = node->childNodes[1]->childNodes[0];
                    if (strChild->nodeType == String_Node) {
                        fileName = decodeQuotedStringToken(strChild->token);

                        fileName = std::filesystem::weakly_canonical(std::filesystem::path(projectDirectory + fileName)).string();

                        if (importedFileNames.find(fileName) != importedFileNames.end()) {
                            node->nodeType = Nothing_Node;
                            return;
                        }

                        int e = loadFile(fileName, fileString);
                        if (e != 0) {
                            printTokenError(getASTTokenRange(strChild), "Failed to include file from given path", __LINE__);
                            exit(1);
                        }

                        importedFileNames.insert(fileName);
                    }
                    else {
                        printTokenError(getASTTokenRange(strChild), "Expected string literal", __LINE__);
                        exit(1);
                    }
                }
                else {
                    printTokenError(getASTTokenRange(node->childNodes[0]), "Expected string literal", __LINE__);
                    exit(1);
                }

                std::vector<asaToken*> localTokens = std::vector<asaToken*>();
                std::vector<std::string*> localLines;
                std::vector<std::string*> localFileNames;

                // Begin tokenizing file
                int e = tokenize(fileString, localTokens, fileName, localLines, localFileNames);
                if (e != 0) {
                    std::cerr << "Invalid tokens met\n";
                    exit(1);
                }
                // Now change any tokens to their subtoken type if applicable
                e = labelSubTokens(localTokens);
                if (e != 0) {
                    std::cerr << "Invalid tokens met\n";
                    exit(1);
                }
                e = joinCommentTokens(localTokens);
                if (e != 0) {
                    std::cerr << "Invalid tokens met\n";
                    exit(1);
                }
                allTokens.insert(allTokens.end(), localTokens.begin(), localTokens.end());
                lines.insert(lines.end(), localLines.begin(), localLines.end());
                fileNames.insert(fileNames.end(), localFileNames.begin(), localFileNames.end());

                // Generate AST
                ASTNode* localRoot = generateAST(localTokens);
                for (int i = 0; i < localRoot->childNodes.size(); i++) {
                    importedNodes.push_back(localRoot->childNodes[i]);
                    //rootNode->childNodes.push_back(localRoot->childNodes[i]);
                }


                node->nodeType = Nothing_Node;
            }
        }
}

// Load every module found in modulePath directory (wildcard import)
void loadAllModulesInDir(const std::string& modulePath)
{
    for (const auto& p : std::filesystem::directory_iterator(modulePath)) {
        std::string outStr = "";
        std::string pathStr = p.path();
        loadFile(pathStr, outStr);

        std::vector<asaToken*> localTokens = std::vector<asaToken*>();
        std::vector<std::string*> localLines;
        std::vector<std::string*> localFileNames;

        int e = tokenize(outStr, localTokens, pathStr, localLines, localFileNames);
        if (e != 0) {
            std::cerr << "Invalid tokens met\n";
            exit(1);
        }
        e = labelSubTokens(localTokens);
        if (e != 0) {
            std::cerr << "Invalid tokens met\n";
            exit(1);
        }
        e = joinCommentTokens(localTokens);
        if (e != 0) {
            std::cerr << "Invalid tokens met\n";
            exit(1);
        }
        allTokens.insert(allTokens.end(), localTokens.begin(), localTokens.end());
        lines.insert(lines.end(), localLines.begin(), localLines.end());
        fileNames.insert(fileNames.end(), localFileNames.begin(), localFileNames.end());

        ASTNode* localRoot = generateAST(localTokens);

        for (int j = 0; j < (int)localRoot->childNodes.size(); j++) {
            if (localRoot->childNodes[j]->nodeType == Compiler_Define &&
                localRoot->childNodes[j]->childNodes.size() >= 1 &&
                localRoot->childNodes[j]->childNodes[0]->childNodes.size() >= 1 &&
                localRoot->childNodes[j]->childNodes[0]->childNodes[0]->nodeType == Module_Define_Node) {

                ASTNode* moduleNode = localRoot->childNodes[j]->childNodes[0]->childNodes[0];
                std::string thisModuleName = localRoot->childNodes[j]->token->tokenStr;

                if (importedModuleNames.find(thisModuleName) != importedModuleNames.end())
                    continue;

                ASTNode* importedModule = localRoot->childNodes[j];
                importedModule->importedChildren = true;
                importedNodes.push_back(importedModule);
                importedModuleNames.insert(thisModuleName);
                if (verbosity >= 3)
                    printModuleLoaded(thisModuleName, pathStr);
            }
        }
    }
}

// OPENCODE:
// Loads a single module by name from the given directory path. Tokenizes and
// parses each file in the directory, looking for a Compiler_Define whose name
// matches moduleName and whose body is a Module_Define_Node. On match, marks
// importedChildren = true (unqualified access), pushes to importedNodes.
// Returns true on success.
bool loadModule(std::string& modulePath, std::string& moduleName)
{
    for (const auto& p : std::filesystem::directory_iterator(modulePath)) {
        std::string outStr = "";
        std::string pathStr = p.path();
        loadFile(pathStr, outStr);

        std::vector<asaToken*> localTokens = std::vector<asaToken*>();
        std::vector<std::string*> localLines;
        std::vector<std::string*> localFileNames;

        // Begin tokenizing file
        int e = tokenize(outStr, localTokens, pathStr, localLines, localFileNames);
        if (e != 0) {
            std::cerr << "Invalid tokens met\n";
            exit(1);
        }
        // Now change any tokens to their subtoken type if applicable
        e = labelSubTokens(localTokens);
        if (e != 0) {
            std::cerr << "Invalid tokens met\n";
            exit(1);
        }
        e = joinCommentTokens(localTokens);
        if (e != 0) {
            std::cerr << "Invalid tokens met\n";
            exit(1);
        }
        // Generate AST
        ASTNode* localRoot = generateAST(localTokens);

        // Look through file to see if it contains the desired module
        for (int j = 0; j < localRoot->childNodes.size(); j++) {
            if (localRoot->childNodes[j]->nodeType == Compiler_Define)
                if (localRoot->childNodes[j]->childNodes.size() >= 1 &&
                    localRoot->childNodes[j]->childNodes[0]->childNodes.size() >= 1 &&
                    localRoot->childNodes[j]->childNodes[0]->childNodes[0]->nodeType == Module_Define_Node) {

                    ASTNode* moduleNode = localRoot->childNodes[j]->childNodes[0]->childNodes[0];
                    if (localRoot->childNodes[j]->token->tokenStr == moduleName) {
                        allTokens.insert(allTokens.end(), localTokens.begin(), localTokens.end());
                        lines.insert(lines.end(), localLines.begin(), localLines.end());
                        fileNames.insert(fileNames.end(), localFileNames.begin(), localFileNames.end());
                        ASTNode* importedModule = localRoot->childNodes[j];
                        importedModule->importedChildren = true;
                        importedNodes.push_back(importedModule);
                        if (verbosity >= 3)
                            printModuleLoaded(moduleName, pathStr);
                        return true;
                    }
                }
        }
    }
    return false;
}

// OPENCODE:
// Converts a Member_Access chain (A.B.C) into a filesystem path (A/B/C) and
// the final module name (C). Used by loadModule / loadModuleQualified.
void getModuleNameAndPath(ASTNode*& node, std::string& modulePath, std::string& moduleName)
{
    if (node->nodeType == Member_Access) {
        // Right-associative: A.B.C -> Member_Access(A, Member_Access(B, C))
        modulePath = node->childNodes[0]->token->tokenStr;
        ASTNode* rest = node->childNodes[1];
        while (rest->nodeType == Member_Access) {
            modulePath += "/" + rest->childNodes[0]->token->tokenStr;
            rest = rest->childNodes[1];
        }
        moduleName = rest->token->tokenStr;
    }
    else if (node->childNodes.size() == 2) {
        ASTNode* firstExpression = node->childNodes[0];
        std::string tmpPath = "";
        getModuleNameAndPath(firstExpression, tmpPath, moduleName);
        modulePath = tmpPath + "/" + modulePath;

        ASTNode* secondExpression = node->childNodes[1];
        moduleName = secondExpression->token->tokenStr;
    }
    else if (node->childNodes.size() == 0)
        modulePath = node->token->tokenStr;
    else {
        printTokenError(getASTTokenRange(node), "Invalid module name expression");
        wasError = true;
        return;
    }
}

// Load a module as a whole and push the Compiler_Define node itself (qualified import).
// The module retains its isModuleScope flag and is accessible via Module.member syntax,
// exactly like an inline module definition.
bool loadModuleQualified(std::string& modulePath, std::string& moduleName)
{
    for (const auto& p : std::filesystem::directory_iterator(modulePath)) {
        std::string outStr = "";
        std::string pathStr = p.path();
        loadFile(pathStr, outStr);

        std::vector<asaToken*> localTokens;
        std::vector<std::string*> localLines;
        std::vector<std::string*> localFileNames;

        int e = tokenize(outStr, localTokens, pathStr, localLines, localFileNames);
        if (e != 0) {
            std::cerr << "Invalid tokens met\n";
            exit(1);
        }
        e = labelSubTokens(localTokens);
        if (e != 0) {
            std::cerr << "Invalid tokens met\n";
            exit(1);
        }
        e = joinCommentTokens(localTokens);
        if (e != 0) {
            std::cerr << "Invalid tokens met\n";
            exit(1);
        }
        ASTNode* localRoot = generateAST(localTokens);

        for (int j = 0; j < (int)localRoot->childNodes.size(); j++) {
            if (localRoot->childNodes[j]->nodeType == Compiler_Define)
                if (localRoot->childNodes[j]->childNodes.size() >= 1 &&
                    localRoot->childNodes[j]->childNodes[0]->childNodes.size() >= 1 &&
                    localRoot->childNodes[j]->childNodes[0]->childNodes[0]->nodeType == Module_Define_Node) {
                    if (localRoot->childNodes[j]->token->tokenStr == moduleName) {
                        allTokens.insert(allTokens.end(), localTokens.begin(), localTokens.end());
                        lines.insert(lines.end(), localLines.begin(), localLines.end());
                        fileNames.insert(fileNames.end(), localFileNames.begin(), localFileNames.end());
                        importedNodes.push_back(localRoot->childNodes[j]);
                        if (verbosity >= 3)
                            printModuleLoaded(moduleName, pathStr);
                        return true;
                    }
                }
        }
    }
    return false;
}

// OPENCODE:
// Processes #import / #import_qualified directives: resolves the module path
// (searching projectDirectory then executableDirectory/modules/), delegates to
// loadModule (flat) or loadModuleQualified, and handles wildcard .* imports
// via loadAllModulesInDir. Dedups via importedModuleNames. Recurses children
// first.
void addModuleImports(ASTNode*& node)
{
    for (int i = 0; i < node->childNodes.size(); i++)
        addModuleImports(node->childNodes[i]);

    if (node->childNodes.size() > 0)
        if (node->nodeType == Compile_Time_Directive) {
            const std::string& directive = node->childNodes[0]->token->tokenStr;
            // #import = flat (module contents brought directly into scope)
            // #import_qualified = qualified (module kept as a whole, accessed via Module.member)
            bool isImport = directive == "import";
            bool isImportQualified = directive == "import_qualified";
            if (!isImport && !isImportQualified)
                return;

            if (node->childNodes.size() > 1) {
                ASTNode* moduleNameNode = node->childNodes[1]->childNodes[0];
                if (moduleNameNode->nodeType == Identifier_Node || moduleNameNode->nodeType == Member_Access) {
                    std::string modulePath = ".";
                    std::string moduleName = "";
                    bool moduleFound = false;

                    getModuleNameAndPath(moduleNameNode, modulePath, moduleName);
                    modulePath = std::filesystem::path(modulePath).lexically_normal().string();

                    if (importedModuleNames.find(moduleName) != importedModuleNames.end()) {
                        node->nodeType = Nothing_Node;
                        return;
                    }

                    std::string searchPath[2] = {projectDirectory + modulePath, executableDirectory + "modules/" + modulePath};

                    // Wildcard: #import Some.Dir.*; loads all modules in that directory (flat)
                    if (moduleName == "*") {
                        if (isImportQualified) {
                            printTokenError(getASTTokenRange(moduleNameNode), "Directory wildcard is not supported with #import_qualified; use #import for flat wildcard loading", __LINE__);
                            exit(1);
                        }
                        for (int i = 0; i < (int)(sizeof(searchPath) / sizeof(searchPath[0])); i++) {
                            if (directoryExists(searchPath[i])) {
                                loadAllModulesInDir(searchPath[i]);
                                moduleFound = true;
                                break;
                            }
                        }
                        if (!moduleFound) {
                            printTokenError(getASTTokenRange(moduleNameNode), "Wildcard import: directory not found: \"" + modulePath + "\"", __LINE__);
                            exit(1);
                        }
                        node->nodeType = Nothing_Node;
                        return;
                    }

                    for (int i = 0; i < (int)(sizeof(searchPath) / sizeof(searchPath[0])); i++) {
                        if (directoryExists(searchPath[i])) {
                            if (isImportQualified)
                                moduleFound = loadModuleQualified(searchPath[i], moduleName);
                            else
                                moduleFound = loadModule(searchPath[i], moduleName);
                            break;
                        }
                    }

                    if (!moduleFound) {
                        printTokenError(getASTTokenRange(moduleNameNode), "Failed to " + directive + " module with name: \"" + moduleName + "\" and expected path: \"" + modulePath + "\", not found", __LINE__);
                        console::writeLine("Looked in the following directories:", console::yellowFGColor);
                        console::indentation++;
                        for (int i = 0; i < (int)(sizeof(searchPath) / sizeof(searchPath[0])); i++)
                            console::writeLine(searchPath[i], console::redFGColor);
                        console::indentation--;
                        if (verbosity >= 5)
                            printAST(node);
                        exit(1);
                    }

                    importedModuleNames.insert(moduleName);
                }
                else {
                    printTokenError(getASTTokenRange(moduleNameNode), "Expected module name", __LINE__);
                    printAST(node);
                    exit(1);
                }
            }
            else {
                printTokenError(getASTTokenRange(node->childNodes[0]), "Expected module name", __LINE__);
                printAST(node);
                exit(1);
            }

            node->nodeType = Nothing_Node;
        }
}

// OPENCODE:
// Pre-order walk setting parentNode and depth on every child. Called twice
// from main.cpp — before and after normalizeQualifiedCompilerDefinitions
// (which physically relocates nodes).
void assignParentNodes(ASTNode*& node, int depth)
{
    for (int i = 0; i < node->childNodes.size(); i++) {
        node->childNodes[i]->parentNode = node;
        node->childNodes[i]->depth = depth + 1;
        assignParentNodes(node->childNodes[i], depth + 1);
    }
}


// OPENCODE:
// Returns the string name of an ASTNodeType enum value via the
// ASTNodeTypeStrings array, with bounds checking.
const std::string ASTNodeTypeAsString(ASTNodeType t)
{
    if (t < LastASTNodeType)
        return ASTNodeTypeStrings[t];
    else {
        printf("Error: Undefined AST Node type `%d`", t);
        return "UNDEFINED AST NODE TYPE";
    }
}

// OPENCODE:
// Prints an indented tree dump of the AST from startNode. Shows token text,
// node type, attributes, doc comments, and child nodes. Respects @hideast
// to elide subtrees (unless --printast). Also prints leftover !unusedLeafNodes!.
int printAST(ASTNode* startNode, int depth)
{
    // Print each node

    console::printIndent(depth);

    if (startNode->token != nullptr) {
        console::write(startNode->token->tokenStr, console::greenFGColor);
        if (verbosity >= 5)
            if (startNode->token->tokenStr.size() > 0)
                printf(":L%d", startNode->lineNumber);
    }
    if (getInheritedAttributeValue(startNode, "hideast") != "true" || compilerFlags == Flags_PrintAST) {
        console::write(":(");
        console::write(ASTNodeTypeAsString(startNode->nodeType), console::yellowFGColor);
        console::write("){");
    }
    else {
        console::write(":(");
        console::write(ASTNodeTypeAsString(startNode->nodeType), console::yellowFGColor);
        console::write(")");
        console::writeLine("{...}");
        return 0;
    }

    bool hasContent = startNode->attributes.size() > 0 ||
                      startNode->docComment != nullptr ||
                      startNode->childNodes.size() > 0 ||
                      startNode->leafNodes.size() > 0;
    if (hasContent)
        printf("\n");

    for (int c = 0; c < startNode->attributes.size(); c++)
        printAST(startNode->attributes[c], depth + 1);

    if (startNode->docComment != nullptr) {
        console::printIndent(depth + 1);
        console::writeLine("doc:" + startNode->docComment->token->tokenStr, console::cyanFGColor);
    }

    for (int c = 0; c < startNode->childNodes.size(); c++) {
        printAST(startNode->childNodes[c], depth + 1);
    }

    // Print unused leaf nodes
    depth++;
    if (startNode->leafNodes.size() > 0) {
        console::printIndent(depth);
        printf("!unusedLeafNodes!:{\n");
        for (int c = 0; c < startNode->leafNodes.size(); c++) {
            printAST(startNode->leafNodes[c], depth + 1);
        }
        console::printIndent(depth);
        printf("}\n");
    }
    depth--;

    if (startNode->childNodes.size() > 0 || startNode->leafNodes.size() > 0)
        console::printIndent(depth);
    printf("}\n");


    return 0;
}

// OPENCODE:
// The codegen dispatch driver: switches on nodeType and invokes each node's
// codegen member-function pointer (node->*(node->codegen))(pass). Called 3
// times from main.cpp (pass 0: struct/enum types, pass 1: prototypes + struct
// bodies + globals, pass 2: function bodies + variable contents). Checks
// wasError after each node and jumps to errorDuringCodegen on failure.
int generateOutputCode(ASTNode*& node, int depth, int pass)
{
    switch (node->nodeType) {
        case Compiler_Define_Struct: {
            if (verbosity >= 4 && getInheritedAttributeValue(node, "hideast") != "true") {
                console::printIndent(depth + 1);
                console::write("pass ");
                console::write(std::to_string(pass), console::greenFGColor);
                console::write(": generating struct for: ");
                console::writeLine(node->token->tokenStr, console::yellowFGColor);
            }
            if (node->codegen != nullptr) {
                (node->*(node->codegen))(pass);
                if (wasError)
                    goto errorDuringCodegen;
            }
            break;
        }

        case Compiler_Define_Enum: {
            if (verbosity >= 4 && getInheritedAttributeValue(node, "hideast") != "true") {
                console::printIndent(depth + 1);
                console::write("pass ");
                console::write(std::to_string(pass), console::greenFGColor);
                console::write(": generating enum for: ");
                console::writeLine(node->token->tokenStr, console::yellowFGColor);
            }
            if (node->codegen != nullptr) {
                (node->*(node->codegen))(pass);
                if (wasError)
                    goto errorDuringCodegen;
            }
            break;
        }

        case Compiler_Define_Cast: {
            if (pass == 0)
                break;
            if (verbosity >= 4 && getInheritedAttributeValue(node, "hideast") != "true") {
                console::printIndent(depth + 1);
                console::write("pass ");
                console::write(std::to_string(pass), console::greenFGColor);
                console::write(": generating cast for: ");
                console::writeLine(node->token->tokenStr, console::yellowFGColor);
            }
            if (node->codegen != nullptr) {
                auto fnVal = (Function*)(node->*(node->codegen))(pass);
                if (wasError)
                    goto errorDuringCodegen;
            }
            break;
        }

        case Compiler_Define_Function: {
            if (pass == 0)
                break;
            if (verbosity >= 4 && getInheritedAttributeValue(node, "hideast") != "true") {
                console::printIndent(depth + 1);
                console::write("pass ");
                console::write(std::to_string(pass), console::greenFGColor);
                console::write(": generating for: ");
                console::writeLine(node->token->tokenStr, console::yellowFGColor);
            }
            if (node->codegen != nullptr) {
                auto fnVal = (Function*)(node->*(node->codegen))(pass);
                if (wasError)
                    goto errorDuringCodegen;
            }
            break;
        }

        case Compile_Time_Directive: {
            if (pass == 0)
                break;
            if (verbosity >= 4 && getInheritedAttributeValue(node, "hideast") != "true") {
                console::printIndent(depth + 1);
                console::write("pass ");
                console::write(std::to_string(pass), console::greenFGColor);
                console::write(": generating for: ");
                console::writeLine(node->token->tokenStr, console::yellowFGColor);
            }
            if (node->codegen != nullptr) {
                auto fnVal = (Function*)(node->*(node->codegen))(pass);
                if (wasError)
                    goto errorDuringCodegen;
            }
            break;
        }

        case Expression_Statement: {
            // Module-scope (root-level) variable declaration: compile as LLVM global.
            if (pass == 1)
                declareModuleScopeVariable(node, node, false);
            break;
        }

        case Colon_Separator_Node: {
            if (node->currentNodeDoneGenerating)
                break;
            if (pass == 1 && node->parentNode == rootNode) {
                declareModuleScopeVariable(node, node, false);
                break;
            }
            if (pass == 2 && node->parentNode != rootNode && node->codegen)
                (Value*)(node->*(node->codegen))(pass);
            break;
        }

        case Compiler_Define: {
            // Named module node: register and declare globals.
            if (pass == 1 && node->isModuleScope)
                processModuleForDeclarations(node);
            if (node->isModuleScope) {
                ASTNode* innerScope = getModuleInnerScope(node);
                if (innerScope) {
                    for (auto* child : innerScope->childNodes) {
                        if (child->nodeType == Expression_Statement || child->nodeType == Colon_Separator_Node)
                            continue;
                        generateOutputCode(child, depth + 1, pass);
                        if (wasError)
                            goto errorDuringCodegen;
                    }
                }
            }
            break;
        }

        case Scope_Body: {
            if (verbosity >= 4 && getInheritedAttributeValue(node, "hideast") != "true") {
                console::printIndent(depth + 1);
                console::write("pass ");
                console::write(std::to_string(pass), console::greenFGColor);
                console::writeLine(": generating scope body");
            }
            for (auto& c : node->childNodes) {
                // Variable declarations inside a root-level Scope_Body (e.g. @public: { X : T = v; })
                // must be stored in the Scope_Body node itself so that findNamedValue can find them
                // via the depth-0 direct-child scan (the Scope_Body IS a direct child of rootNode,
                // but the Expression_Statement grandchildren are not).
                if (pass == 1 && (c->nodeType == Expression_Statement || c->nodeType == Colon_Separator_Node)) {
                    declareModuleScopeVariable(c, node, false);
                    continue;
                }
                generateOutputCode(c, depth + 1, pass);
                if (wasError)
                    goto errorDuringCodegen;
            }

            break;
        }

        default:
            break;
    }
    return 0;

errorDuringCodegen:
    //exit(1);
    return 1;
}
