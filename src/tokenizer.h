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
    Struct_Define,
    Module_Define,
    Enum_Define,

    Operator_Keyword,

    True_Literal,
    False_Literal,

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
    "Struct_Define",
    "Module_Define",
    "Enum_Define",

    "Operator_Keyword",

    "True_Literal",
    "False_Literal",
};

extern std::string nullStr;
struct asaToken {
    std::string tokenStr = "";
    TokenType tokenType = Nothing;
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

// Increments i, then returns the next token in `tokens`
#define NEXT_TOKEN(tokens, i) tokens[++i];

int tokenize(std::string& rawFile, std::vector<asaToken*>& tokens, std::string& fileName);
int labelSubTokens(std::vector<asaToken*>& tokens);
int joinCommentTokens(std::vector<asaToken*>& tokens);
int joinDotAtTokens(std::vector<asaToken*>& tokens);
int removeCommentTokens(std::vector<asaToken*>& tokens);
const std::string tokenAsString(TokenType t);
