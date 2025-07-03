#pragma once

#include <iostream>
#include <stack>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "tokenizer.h"

#define MAX_AST_DEPTH -1

//#define NEXT_TOKEN(i) (if (i + 1 < tokens.size()) tokens[++i] else "ERROR");
void GO_BACK_TO_BEGINNING_OF_TERM(int& i);

enum ASTNodeType {
	// Null type
	Nothing_Node,

	Identifier_Node,
	Number_Node,
	Boolean_Node,
	String_Node,
	Type,

	Expression_Term,
	Expression_Statement,
	Expression_Plus,
	Expression_Minus,
	Expression_Times,
	Expression_Divided,

	If_Statement_Node,
	If_Cond,
	If_Body,

	For_Statement_Node,
	For_It,
	For_Range,
	For_Body,

	While_Statement_Node,
	While_Cond,
	While_Body,

	Scope_Body,

	Compare_Equal,
	Compare_Not,
	Compare_Less,
	Compare_Greater,
	Compare_LessEqual,
	Compare_GreaterEqual,

	Compiler_Define,
	Compile_Time_Directive,
	Arguments,

	// Nothing below this
	LastASTNodeType
};

const std::string ASTNodeTypeStrings[] = {
	"Nothing_Node",

	"Identifier_Node",
	"Number_Node",
	"Boolean_Node",
	"String_node",
	"Type",

	"Expression_Term",
	"Expression_Statement",
	"Expression_Plus",
	"Expression_Minus",
	"Expression_Times",
	"Expression_Divided",

	"If_Statement_Node",
	"If_Cond",
	"If_Body",

	"For_Statement_Node",
	"For_It",
	"For_Range",
	"For_Body",

	"While_Statement_Node",
	"While_Cond",
	"While_Body",

	"Scope_Body",

	"Compare_Equal",
	"Compare_Not",
	"Compare_Less",
	"Compare_Greater",
	"Compare_LessEqual",
	"Compare_GreaterEqual",

	"Compiler_Define",
	"Compile_Time_Directive",
	"Arguments",
};


struct ASTNode {
	ASTNode* prevNode;
	std::vector<ASTNode*> childNodes = std::vector<ASTNode*>();
	ASTNodeType nodeType = Nothing_Node;
	TokenType tokenType = Nothing;
	std::string token;
};


extern ASTNode* rootNode;

int beginParse(const std::vector<tokenPair>& tokens);
ASTNode* generateAST(const std::vector<tokenPair>& tokens, int depth = 0, bool hasParent = false);
const std::string ASTNodeTypeAsString(ASTNodeType t);
int printAST(ASTNode* startNode, int depth = 0);
inline std::vector<tokenPair> GATHER_SCOPE_BODY(int braceLevel, int& i);
