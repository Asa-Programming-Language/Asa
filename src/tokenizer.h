#pragma once

#include "pch.h"

enum TokenType {
    // Null type
    Nothing,

    // Line and file end
    EndOfLine,
    EndOfFile,

    // Comment
    Comment,

    // Literals
    Identifier,
    Void,
    Integer,
    Float,
    String,
    Character,

    // Punctuation and its subtypes
    Punctuation,
    Left_Paren,
    Right_Paren,
    Left_Bracket,
    Right_Bracket,
    Both_Brackets,
    Left_Brace,
    Right_Brace,
    Comma,
    Dot,
    Dot_Dot,
    Dot_Dot_Dot,
    Dot_At,
    Question,
    Hash,

    Plus,
    Minus,
    Slash,
    Star,
    Star_Star,

    Colon,
    Colon_Colon,
    Semi_Colon,

    Bar,
    Bar_Bar,
    Bar_Bar_Bar,
    Ampersand,
    Ampersand_Ampersand,
    Tilde,
    Tilde_Tilde,
    Caret,
    Caret_Caret,
    Percent,
    Percent_Percent,
    At,
    At_At,
    Dollar,
    Arrow_Left,
    Arrow_Right,
    Shift_Left,
    Shift_Right,

    Ref,
    Const,
    Exact,

    Bang,
    Bang_Bang,
    Bang_Equal,
    Plus_Equal,
    Plus_Plus,
    Minus_Equal,
    Minus_Minus,
    Times_Equal,
    Slash_Equal,
    Ampersand_Equal,
    Bar_Equal,
    Caret_Equal,
    Shift_Left_Equal,
    Shift_Right_Equal,
    Equal,
    Equal_Equal,
    Less,
    Less_Equal,
    Greater,
    Greater_Equal,

    If_Statement,
    For_Statement,
    While_Statement,
    In_Keyword,

    Return_Statement,
    Result_Statement,
    Continue_Statement,
    Break_Statement,
    Else_Statement,
    Goto_Statement,
    Throw_Statement,
    Throw_Caller_Statement,
    Struct_Define,
    Module_Define,
    Enum_Define,

    Operator_Keyword,
    Default_Keyword,
    Initial_Keyword,

    True_Literal,
    False_Literal,

    // Print formatting tokens (used by message renderer only)
    _Print_Gutter,
    _Print_Gutter_Marker,
    _Print_Gutter_Ellipsis,
    _Print_Ellipsis,
    _Print_Truncation_Ellipsis,
    _Print_Underline,
    _Print_Connector_V,
    _Print_Connector_H,
    _Print_Connector_Corner,
    _Print_Connector_Over,
    _Print_Connector_Under,
    _Print_Space,
    _Print_Message,

    // Nothing below this
    LastTokenType,
};

const std::string tokenTypeStrings[] = {
    "nothing",

    "EOL",
    "EOF",

    "Comment",

    "Identifier",
    "Void",
    "Integer",
    "Float",
    "String",
    "Character",

    "Punctuation",

    "Left_Paren",
    "Right_Paren",
    "Left_Bracket",
    "Right_Bracket",
    "Both_Brackets",
    "Left_Brace",
    "Right_Brace",
    "Comma",
    "Dot",
    "Dot_Dot",
    "Dot_Dot_Dot",
    "Dot_At",
    "Question",
    "Hash",

    "Plus",
    "Minus",
    "Slash",
    "Star",
    "Star_Star",

    "Colon",
    "Colon_Colon",
    "Semi_Colon",

    "Bar",
    "Bar_Bar",
    "Bar_Bar_Bar",
    "Ampersand",
    "Ampersand_Ampersand",
    "Tilde",
    "Tilde_Tilde",
    "Caret",
    "Caret_Caret",
    "Percent",
    "Percent_Percent",
    "At",
    "At_At",
    "Dollar",
    "Arrow_Left",
    "Arrow_Right",
    "Shift_Left",
    "Shift_Right",

    "Ref",
    "Const",
    "Exact",

    "Bang",
    "Bang_Bang",
    "Bang_Equal",
    "Plus_Equal",
    "Plus_Plus",
    "Minus_Equal",
    "Minus_Minus",
    "Times_Equal",
    "Slash_Equal",
    "Ampersand_Equal",
    "Bar_Equal",
    "Caret_Equal",
    "Shift_Left_Equal",
    "Shift_Right_Equal",
    "Equal",
    "Equal_Equal",
    "Less",
    "Less_Equal",
    "Greater",
    "Greater_Equal",

    "If_Statement",
    "For_Statement",
    "While_Statement",
    "In_Keyword",

    "Return_Statement",
    "Result_Statement",
    "Continue_Statement",
    "Break_Statement",
    "Else_Statement",
    "Goto_Statement",
    "Throw_Statement",
    "Throw_Caller_Statement",
    "Struct_Define",
    "Module_Define",
    "Enum_Define",

    "Operator_Keyword",
    "Default_Keyword",
    "Initial_Keyword",

    "True_Literal",
    "False_Literal",

    // Print formatting
    "print_gutter",
    "print_gutter_marker",
    "print_gutter_ellipsis",
    "print_ellipsis",
    "print_truncation_ellipsis",
    "print_underline",
    "print_connector_v",
    "print_connector_h",
    "print_connector_corner",
    "print_connector_over",
    "print_connector_under",
    "print_space",
    "print_message",
};

extern std::string nullStr;
struct asaToken {
    std::string tokenStr = "";
    TokenType tokenType = Nothing;
    const std::string* color = nullptr;
    int lineNumber = 0;
    std::string* lineValue = nullptr;
    std::string* filePath = nullptr;
    int indexInLine = 0;
    uint16_t length = 0;
    int lineIndent = 0;  // leading whitespace width of this line (tabs count as 4)

    asaToken() {}
    asaToken(std::string f, TokenType s)
    {
        tokenStr = f;
        length = tokenStr.size();
        tokenType = s;
    }
    asaToken(std::string f, TokenType s, int l, int i, std::string* lV, std::string* fP, int indent = 0)
    {
        tokenStr = f;
        length = tokenStr.size();
        tokenType = s;
        lineNumber = l;
        indexInLine = i;
        lineValue = lV;
        filePath = fP;
        lineIndent = indent;
    }

    bool operator==(asaToken other)
    {
        if (tokenStr == other.tokenStr && tokenType == other.tokenType)
            return true;
        else
            return false;
    }
};


typedef std::pair<asaToken*, asaToken*> tokenRange;

extern std::vector<asaToken*> allTokens;
extern std::vector<std::string*> lines;
extern std::vector<std::string*> fileNames;

// Increments i, then returns the next token in `tokens`
#define NEXT_TOKEN(tokens, i) tokens[++i];

int tokenize(std::string& rawFile, std::vector<asaToken*>& tokens, std::string& fileName, std::vector<std::string*>& outLines, std::vector<std::string*>& outFileNames);
int labelSubTokens(std::vector<asaToken*>& tokens);
int joinCommentTokens(std::vector<asaToken*>& tokens);
int removeCommentTokens(std::vector<asaToken*>& tokens);
const std::string tokenTypeAsString(TokenType t);
std::string decodeQuotedStringToken(asaToken* token);
