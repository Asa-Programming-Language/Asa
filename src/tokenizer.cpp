#include "tokenizer.h"

std::vector<asaToken*> allTokens = std::vector<asaToken*>();
std::vector<std::string*> lines = std::vector<std::string*>();
std::vector<std::string*> fileNames = std::vector<std::string*>();
std::string nullStr = "";

bool charInString(char x, const std::string a)
{
    for (int i = 0; a[i] != '\0'; i++) {
        if (a[i] == x)
            return true;
    }
    return false;
}

//asaToken NEXT_TOKEN(int& i)
//{
//  if (i + 1 < tokens.size())
//      return tokens[++i];
//  else
//      printf("Error: Out of bounds token\n");
//  throw std::runtime_error("Error: Out of bounds token from NEXT_TOKEN\n" __FILE__);
//}

const std::map<TokenType, const std::string> tokenStarts = {
    {Identifier, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_"},
    {Integer, "0123456789"},
    {String, "\""},
    {Character, "'"},
    {Punctuation, ".,/;()+=-\\|<>?:!@#$%^&*{}[]`~"},
    {EndOfLine, "\n"},
    {Nothing, " \t"},
};

const std::map<TokenType, const std::string> tokenEscapes = {
    {Identifier, " .,/;()+=-\\|<>?:!@#$%^&*{}[]\"'\n\t\r"},
    {Integer, " ,/;()+=-\\|<>?:!@#$%^&*{}[]\"'\n\t\r"},
    {String, "\""},
    {Character, "'\n"},
    {Punctuation, " ,/;()+=-\\|<>?:!@#$%^&*{}[]\"'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789.\n\t\r"},
    {EndOfLine, " ,/;()+=-\\|<>?:!@#$%^&*{}[]\"'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789.\n\t\r"},
};


// If the first char is in the first string, and the following character is in the second string, keep adding to token type
const std::vector<std::pair<const std::string, const std::string>> tokenEscapeCancels = {
    {"+-*/:^|!&~<>=", "="},
    {"+-", "+-"},
    {".", "."},
    {"-", ">"},
    {".", "@"},
    {"/", "/*"},
    {"*", "*/"},
    {":", ":"},
    {"|", "|"},
    {"&", "&"},
    {"~", "~"},
    {"^", "^"},
    {"%", "%"},
    {"@", "@"},
    {"!", "!"},
    {"[", "]"},
};

// If this and the following character match for the specific token type, immediately end
const std::map<TokenType, const std::string> tokenForceEscapes = {
    {Integer, ".."},
};

// If tokenType value contains string, change to new tokenType
std::map<TokenType, std::pair<const std::string, TokenType>> tokenContainsSwap = {
    {Integer, {".", Float}},
};

std::map<const std::string, const TokenType> subTokenTypes = {
    // Symbols
    {"(", Left_Paren},
    {")", Right_Paren},
    {"[", Left_Bracket},
    {"]", Right_Bracket},
    {"[]", Both_Brackets},
    {"{", Left_Brace},
    {"}", Right_Brace},
    {",", Comma},
    {".", Dot},
    {"..", Dot_Dot},
    {"...", Dot_Dot_Dot},
    {"+", Plus},
    {"-", Minus},
    {"/", Slash},
    {"*", Star},
    {"**", Star_Star},
    {"#", Hash},
    {":", Colon},
    {"::", Colon_Colon},
    {";", Semi_Colon},
    {"!", Bang},
    {"!!", Bang_Bang},
    {"!=", Bang_Equal},
    {"+=", Plus_Equal},
    {"++", Plus_Plus},
    {"-=", Minus_Equal},
    {"--", Minus_Minus},
    {"*=", Times_Equal},
    {"/=", Slash_Equal},
    {"=", Equal},
    {"==", Equal_Equal},
    {"<", Less},
    {"<=", Less_Equal},
    {">", Greater},
    {">=", Greater_Equal},
    {"|", Bar},
    {"||", Bar_Bar},
    {"|||", Bar_Bar_Bar},
    {"&", Ampersand},
    {"&&", Ampersand_Ampersand},
    {"~", Tilde},
    {"~~", Tilde_Tilde},
    {"^", Caret},
    {"^^", Caret_Caret},
    {"%", Percent},
    {"%%", Percent_Percent},
    {"@", At},
    {"@@", At_At},
    {".@", Dot_At},
    {"$", Dollar},
    {"->", Arrow_Right},
    {"<-", Arrow_Left},

    // Keywords
    {"if", If_Statement},
    {"else", Else_Statement},
    {"for", For_Statement},
    {"while", While_Statement},
    {"in", In_Keyword},
    {"return", Return_Statement},
    {"result", Result_Statement},
    {"break", Break_Statement},
    {"continue", Continue_Statement},
    {"goto", Goto_Statement},
    {"throw", Throw_Statement},
    {"struct", Struct_Define},
    {"module", Module_Define},
    {"enum", Enum_Define},
    {"operator", Operator_Keyword},
    {"ref", Ref},
    {"const", Const},
    {"exact", Exact},
    {"void", Void},
    //{"switch", },
    //{"case", },
    //{"constant", },
    //{"int", },

    // Literals
    {"true", True_Literal},
    {"false", False_Literal},
};

const std::string tokenAsString(TokenType t)
{
    if (t < LastTokenType)
        return tokenTypeStrings[t];
    else {
        printf("Error: Undefined token type `%d`", t);
        return "UNDEFINED TOKEN TYPE";
    }
}

TokenType currentToken = Nothing;
std::string tokenContent = "";
int tokenize(std::string& rawFile, std::vector<asaToken*>& tokens, std::string& fileName, std::vector<std::string*>& outLines, std::vector<std::string*>& outFileNames)
{
    // First remove carriage returns if they exist
    std::string output = "";
    for (char c : rawFile) {
        if (c != '\r') {
            output += c;
        }
    }
    rawFile = output;

    outLines.push_back(new std::string(""));
    outFileNames.push_back(new std::string(fileName));

    // Then start making tokens
    int lineNumber = 0;
    int indexInLine = 0;
    int startIndexInLine = 0;
    std::string* lineValue = new std::string("");
    rawFile = "\n" + rawFile + "\n";  // add extra character at end as buffer for lookahead
    bool inLineComment = false;
    bool inBlockComment = false;
    std::string commentContent = "";
    int commentStartLine = 0;
    int lineIndent = 0;     // leading whitespace width for current line (tabs=4)
    bool inLeading = true;  // still in the leading-whitespace portion of this line
    for (int i = 1; i < rawFile.size(); i++) {
        char c = rawFile[i];
        char nextChar = rawFile[i + 1];
        char lastChar = rawFile[i - 1];
        //if (lastChar == '\n')
        //  lineNumber++;
        // If no token is building, check what the new one should be
        if (currentToken == Nothing) {
            // Track comment state so special chars inside comments don't start new tokens
            if (!inLineComment && !inBlockComment) {
                if (c == '/' && nextChar == '/') {
                    inLineComment = true;
                    commentContent = "//";
                    commentStartLine = lineNumber;
                    startIndexInLine = indexInLine + 1;
                    *lineValue += "//";
                    indexInLine += 2;
                    i++;  // consume the second '/'
                    continue;
                }
                else if (c == '/' && nextChar == '*') {
                    inBlockComment = true;
                    commentContent = "/*";
                    commentStartLine = lineNumber;
                    startIndexInLine = indexInLine + 1;
                    *lineValue += "/*";
                    indexInLine += 2;
                    i++;  // consume the '*'
                    continue;
                }
            }
            if (inLineComment) {
                if (c == '\n') {
                    inLineComment = false;
                    tokens.push_back(new asaToken(commentContent, Comment, commentStartLine, startIndexInLine, lineValue, outFileNames.back(), lineIndent));
                    commentContent = "";
                }
                else {
                    commentContent += c;
                    if (c != '\t') {
                        *lineValue += c;
                        indexInLine++;
                    }
                    continue;
                }
            }
            if (inBlockComment) {
                if (c == '*' && nextChar == '/') {
                    inBlockComment = false;
                    commentContent += "*/";
                    i++;  // consume the '/'
                    tokens.push_back(new asaToken(commentContent, Comment, commentStartLine, startIndexInLine, lineValue, outFileNames.back(), lineIndent));
                    commentContent = "";
                    continue;
                }
                commentContent += c;
                if (c != '\t') {
                    *lineValue += c;
                    indexInLine++;
                }
                continue;
            }

            // Track leading whitespace for indentation calculation.
            if (inLeading) {
                if (c == ' ')
                    lineIndent++;
                else if (c == '\t')
                    lineIndent += 4;
                else if (c != '\n')
                    inLeading = false;
            }

            // Look up which tokenType this token should be
            for (auto const& [tokenType, str] : tokenStarts) {
                if (charInString(c, str)) {
                    if (tokenType == Nothing)
                        break;
                    currentToken = tokenType;
                    tokenContent += c;
                    startIndexInLine = indexInLine + 1;
                    break;
                }
            }
            if (c != '\t' && !(inLeading && c == ' ')) {
                *lineValue += c;
                indexInLine++;
            }
        }
        // If we are already building the current token, continue
        else {
            // Handle escape sequences in strings and characters
            if ((currentToken == String || currentToken == Character) && lastChar == '\\') {
                tokenContent += c;
                if (c != '\t') {
                    *lineValue += c;
                    indexInLine++;
                }
                continue;  // Skip the escape check below
            }

            // Check if it should end, by looking up the tokenType in the tokenEscapes
            for (auto const& [tokenType, str] : tokenEscapes) {
                if (tokenType == currentToken) {
                    // If the current character is in the token escape string, we should end this token
                    if (charInString(c, str)) {

                        // But first:
                        // Check if 2 characters create a match to cancel ending this token
                        for (auto const& [firstGroup, secondGroup] : tokenEscapeCancels) {
                            if (charInString(lastChar, firstGroup) && charInString(c, secondGroup)) {
                                goto cancelEnd;
                            }
                        }

                    endToken:
                        // If a string or character, then include the ending token, which would be the closing quote " or '
                        if (currentToken == String || currentToken == Character) {
                            tokenContent += c;
                            if (c != '\t') {
                                *lineValue += c;
                                indexInLine++;
                            }
                        }
                        // Otherwise
                        else
                            i--;  // Dont include first character of next token
                        if (currentToken == EndOfLine) {
                            lineNumber++;
                            *lineValue = (*lineValue).substr(0, (*lineValue).size() - 1);
                            while (!lineValue->empty() && lineValue->back() == ' ')
                                lineValue->pop_back();
                            outLines.push_back(lineValue);
                            lineValue = new std::string("");
                            indexInLine = 0;
                            lineIndent = 0;
                            inLeading = true;
                        }

                        // If the current tokenType is in the swap map, and the current character is the required character to swap,
                        // then swap the current token type to be the new one.
                        // For example, if this is: currentToken=Integer, tokenContent="1."
                        // Then change the currentToken to Float
                        if (tokenContainsSwap.find(currentToken) != tokenContainsSwap.end()) {
                            if (tokenContent.find(tokenContainsSwap[currentToken].first[0]) != std::string::npos) {
                                currentToken = tokenContainsSwap[currentToken].second;
                            }
                        }


                        // Add tokenContent as element to tokens, and clear it
                        tokens.push_back(new asaToken(tokenContent, currentToken, lineNumber, startIndexInLine, lineValue, outFileNames.back(), lineIndent));
                        tokenContent = "";

                        currentToken = Nothing;
                        break;
                    }
                    else {
                        break;
                    }
                }
                // Check if 2 characters create a match to force ending this token early
                for (auto const& [tok, c2] : tokenForceEscapes) {
                    if (currentToken == tok)
                        if (c == c2[0] && nextChar == c2[1]) {
                            goto endToken;
                        }
                }
            }
        cancelEnd:
            if (currentToken != Nothing) {
                // Count newlines inside multi-line string/char literals so subsequent
                // tokens get correct line numbers for debug info.
                if (c == '\n' && (currentToken == String || currentToken == Character)) {
                    lineNumber++;
                    outLines.push_back(lineValue);
                    lineValue = new std::string("");
                    indexInLine = 0;
                    lineIndent = 0;
                    inLeading = true;
                }
                tokenContent += c;
                if (c != '\t') {
                    *lineValue += c;
                    indexInLine++;
                }
            }
        }
    }
    tokens.push_back(new asaToken("", EndOfFile, lineNumber + 1, 0, &nullStr, &nullStr));

    return 0;
}

int labelSubTokens(std::vector<asaToken*>& tokens)
{
    for (int i = 0; i < (int)tokens.size(); i++) {
        std::string t = tokens[i]->tokenStr;
        TokenType tt = tokens[i]->tokenType;
        // If token has a known subtype, set TokenType to that instead
        if (subTokenTypes.find(t) != subTokenTypes.end()) {
            const TokenType newType = subTokenTypes[t];
            tokens[i]->tokenType = newType;
        }
        // Split runs of 3+ '!' into '!!' (Bang_Bang) + '!' (Bang) tokens.
        // The tokenizer greedily reads '!!!' as one Punctuation token; split it here.
        else if (tt == Punctuation && t.size() >= 3 && t.find_first_not_of('!') == std::string::npos) {
            std::string bangs = t;
            std::vector<asaToken*> split;
            while (bangs.size() >= 2) {
                split.push_back(new asaToken("!!", Bang_Bang,
                    tokens[i]->lineNumber, tokens[i]->indexInLine,
                    tokens[i]->lineValue, tokens[i]->filePath));
                bangs = bangs.substr(2);
            }
            if (!bangs.empty()) {
                split.push_back(new asaToken("!", Bang,
                    tokens[i]->lineNumber, tokens[i]->indexInLine,
                    tokens[i]->lineValue, tokens[i]->filePath));
            }
            tokens.erase(tokens.begin() + i);
            tokens.insert(tokens.begin() + i, split.begin(), split.end());
            // Re-examine at i (now Bang_Bang), outer loop will advance past these
            i--;
        }
    }

    return 0;
}

int joinCommentTokens(std::vector<asaToken*>& tokens)
{
    tokens.insert(tokens.begin(), new asaToken());
    int i = 0;
    bool inComment = false;
    bool multiLineComment = false;
    int startIndex = 0;
    std::string newTokenContents = "";
    while (i < tokens.size() - 1) {
        asaToken* t = NEXT_TOKEN(tokens, i);
        if (!inComment) {
            if (t->tokenType != Comment &&
                (t->tokenStr.substr(0, 2) == "//" || t->tokenStr.substr(0, 2) == "/*")) {
                inComment = true;
                if (t->tokenStr.substr(0, 2) == "/*")
                    multiLineComment = true;
                startIndex = i;
                newTokenContents = t->tokenStr + " ";
            }
        }
        else if (inComment) {
            // Check if this is the end of comment
            if (multiLineComment) {
                if (t->tokenStr.substr(0, 2) == "*/") {
                    newTokenContents += t->tokenStr + " ";
                    i++;
                    goto endComment;
                }
            }
            else if (t->tokenType == EndOfLine || t->tokenType == EndOfFile) {
                goto endComment;
            }

            // If not end of comment, append to contents
            newTokenContents += t->tokenStr + " ";
            continue;

        endComment:
            inComment = false;
            multiLineComment = false;
            tokens[startIndex]->tokenStr = newTokenContents;
            tokens[startIndex]->tokenType = Comment;
            tokens.erase(tokens.begin() + startIndex + 1, tokens.begin() + i);
            i = startIndex;
        }
    }
    return 0;
}

int removeCommentTokens(std::vector<asaToken*>& tokens)
{
    int i = 0;
    while (i < tokens.size() - 1) {
        asaToken* t = NEXT_TOKEN(tokens, i);
        if (t->tokenType == Comment) {
            tokens.erase(tokens.begin() + i);
            i--;
        }
    }
    return 0;
}
