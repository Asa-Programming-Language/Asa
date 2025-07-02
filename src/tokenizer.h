#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

enum TokenType {
	// Null type
	Nothing,

	// Literals
	Identifier,
	Number,
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

	Plus,
	Minus,
	Slash,
	Star,

	Colon,
	Semi_Colon,

	Bang,
	Bang_Equal,
	Equal,
	Equal_Equal,
	Less,
	Less_Equal,
	Greater,
	Greater_Equal
};

const std::string tokenTypeStrings[] = {
	"nothing",

	"identifier",
	"number",
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

	"plus",
	"minus",
	"slash",
	"star",

	"colon",
	"semi_colon",

	"bang",
	"bang_equal",
	"less",
	"less_equal",
	"greater",
	"greater_equal",
};

extern std::vector<std::pair<std::string, TokenType>> tokens;

int tokenize(std::string& rawFile);
int labelSubTokens(std::vector<std::pair<std::string, TokenType>>& tokens);
const std::string tokenAsString(TokenType t);
