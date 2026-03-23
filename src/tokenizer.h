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

	Return_Statement,
	Continue_Statement,
	Break_Statement,
	Else_Statement,
	Goto_Statement,
	Throw_Statement,
	Struct_Define,
	Module_Define,

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

	"Return_Statement",
	"Continue_Statement",
	"Break_Statement",
	"Else_Statement",
	"Goto_Statement",
	"Throw_Statement",
	"Struct_Define",
	"Module_Define",

	"Operator_Keyword",

	"True_Literal",
	"False_Literal",
};

extern std::string nullStr;
struct tokenDataType {
	std::string first = "";
	TokenType second = Nothing;
	int lineNumber = 0;
	std::string* lineValue = nullptr;
	std::string* filePath = nullptr;
	int indexInLine = 0;
	uint16_t length = 0;

	tokenDataType() {}
	tokenDataType(std::string f, TokenType s)
	{
		first = f;
		length = first.size();
		second = s;
	}
	tokenDataType(std::string f, TokenType s, int l, int i, std::string* lV, std::string* fP)
	{
		first = f;
		length = first.size();
		second = s;
		lineNumber = l;
		indexInLine = i;
		lineValue = lV;
		filePath = fP;
	}

	bool operator==(tokenDataType other)
	{
		if (first == other.first && second == other.second)
			return true;
		else
			return false;
	}
};

typedef tokenDataType tokenPair;
//typedef std::pair<std::string, TokenType> tokenPair;

extern std::vector<tokenPair*> allTokens;
extern std::vector<std::string*> lines;

// Increments i, then returns the next token in `tokens`
#define NEXT_TOKEN(tokens, i) tokens[++i];

int tokenize(std::string& rawFile, std::vector<tokenPair*>& tokens, std::string& fileName);
int labelSubTokens(std::vector<tokenPair*>& tokens);
int joinCommentTokens(std::vector<tokenPair*>& tokens);
int joinDotAtTokens(std::vector<tokenPair*>& tokens);
int removeCommentTokens(std::vector<tokenPair*>& tokens);
const std::string tokenAsString(TokenType t);
