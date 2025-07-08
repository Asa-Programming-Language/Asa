#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
	Integer,
	Float,
	String,

	// Punctuation and its subtypes
	Punctuation,
	Left_Paren,
	Right_Paren,
	Left_Bracket,
	Right_Bracket,
	Left_Brace,
	Right_Brace,
	Comma,
	Dot,
	Dot_Dot,
	Hash,

	Plus,
	Minus,
	Slash,
	Star,

	Colon,
	Colon_Colon,
	Semi_Colon,

	Bang,
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
	Goto_Statement,
	Struct_Define,
	Module_Define,

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

	"identifier",
	"Integer",
	"Float",
	"string",

	"punctuation",

	"left_paren",
	"right_paren",
	"left_bracket",
	"right_bracket",
	"left_brace",
	"right_brace",
	"comma",
	"dot",
	"Dot_Dot",
	"Hash",

	"plus",
	"minus",
	"slash",
	"star",

	"Colon",
	"Colon_Colon",
	"Semi_Colon",

	"bang",
	"bang_equal",
	"Plus_Equal",
	"Plus_Plus",
	"Minus_Equal",
	"Minus_Minus",
	"Times_Equal",
	"Slash_Equal",
	"Equal",
	"equal_equal",
	"less",
	"less_equal",
	"greater",
	"greater_equal",

	"if_statement",
	"for_statement",
	"while_statement",

	"Return_Statement",
	"Continue_Statement",
	"Break_Statement",
	"Goto_Statement",
	"Struct_Define",
	"Module_Define",

	"true_literal",
	"false_literal",
};

extern std::string nullStr;
struct tokenDataType {
	std::string first = "";
	TokenType second = Nothing;
	int lineNumber = 0;
	std::string* lineValue = nullptr;
	int indexInLine = 0;

	tokenDataType(std::string f, TokenType s, int l, int i, std::string* lV)
	{
		first = f;
		second = s;
		lineNumber = l;
		indexInLine = i;
		lineValue = lV;
	}
};

typedef tokenDataType tokenPair;
//typedef std::pair<std::string, TokenType> tokenPair;

extern std::vector<tokenPair> allTokens;
extern std::vector<std::string*> lines;

#define NEXT_TOKEN(tokens, i) tokens[++i];
//tokenPair NEXT_TOKEN(int& i);

int tokenize(std::string& rawFile);
int labelSubTokens(std::vector<tokenPair>& tokens);
int joinCommentTokens(std::vector<tokenPair>& tokens);
int removeCommentTokens(std::vector<tokenPair>& tokens);
const std::string tokenAsString(TokenType t);
