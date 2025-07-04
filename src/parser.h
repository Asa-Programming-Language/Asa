#pragma once

#include <iostream>
#include <stack>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "settings.h"
#include "tokenizer.h"

#define MAX_AST_DEPTH -1

#define PRINT_SUBTOKENS(subTokens)                  \
	for (int ST = 0; ST < subTokens.size(); ST++)   \
		printf("%s ", subTokens[ST].first.c_str()); \
	printf("\n");

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

	Range_Node,
	Range_Begin,
	Range_End,

	Function_Call,

	If_Statement_Node,

	For_Statement_Node,

	While_Statement_Node,

	Iterator,
	Condition,
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

const std::map<tokenType, nodeType> binaryOperatorExType = {
	{Plus, Expression_Plus},
	{Minus, Expression_Minus},
	{Star, Expression_Times},
	{Slash, Expression_Divided},
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

	"Range_Node",
	"Range_Begin",
	"Range_End",

	"Function_Call",

	"If_Statement_Node",

	"For_Statement_Node",

	"While_Statement_Node",

	"Iterator",
	"Condition",
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
	int lineNumber = 0;
	// Add leaf nodes here as they are still yet to be used.
	std::vector<ASTNode*> leafNodes = std::vector<ASTNode*>();
};


extern ASTNode* rootNode;

int beginParse(const std::vector<tokenPair>& tokens);
ASTNode* generateAST(const std::vector<tokenPair>& tokens, int depth = 0, ASTNode* parentNodePtr = nullptr);
const std::string ASTNodeTypeAsString(ASTNodeType t);
int printAST(ASTNode* startNode, int depth = 0);
//std::vector<tokenPair> GATHER_SCOPE_BODY(int brLevel, int& i);
