#pragma once

#include <iostream>
#include <stack>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "codegen.h"
#include "filemanager.h"
#include "settings.h"
#include "tokenizer.h"

#define MAX_AST_DEPTH -1

#define PRINT_SUBTOKENS(subTokens)                  \
	for (int ST = 0; ST < subTokens.size(); ST++)   \
		printf("%s ", subTokens[ST].first.c_str()); \
	printf("\n");

using namespace llvm;

void GO_BACK_TO_BEGINNING_OF_TERM(int& i);


enum ASTNodeType {
	// Null type
	Nothing_Node,

	Identifier_Node,
	Integer_Node,
	Float_Node,
	Boolean_Node,
	String_Node,
	Type_Node,

	Module_Scope,

	Expression_Term,
	Expression_Paren_Term,
	Expression_Statement,
	Expression_Plus,
	Expression_Minus,
	Expression_Times,
	Expression_Divided,

	Range_Node,

	Argument_List,

	Function_Call,

	If_Statement_Node,

	For_Statement_Node,

	While_Statement_Node,

	Struct_Define_Node,

	Module_Define_Node,

	Return_Node,
	Continue_Node,
	Break_Node,
	Goto_Node,

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
	Compiler_Define_Function,
	Compile_Time_Directive,
	Arguments,

	// Nothing below this
	LastASTNodeType
};

extern std::map<TokenType, ASTNodeType> binaryOperatorExType;

const std::string ASTNodeTypeStrings[] = {
	"Nothing_Node",

	"Identifier_Node",
	"Integer_Node",
	"Float_Node",
	"Boolean_Node",
	"String_node",
	"Type_Node",

	"Module_Scope",

	"Expression_Term",
	"Expression_Paren_Term",
	"Expression_Statement",
	"Expression_Plus",
	"Expression_Minus",
	"Expression_Times",
	"Expression_Divided",

	"Range_Node",

	"Argument_List",

	"Function_Call",

	"If_Statement_Node",

	"For_Statement_Node",

	"While_Statement_Node",

	"Struct_Define_Node",

	"Module_Define_Node",

	"Return_Node",
	"Continue_Node",
	"Break_Node",
	"Goto_Node",

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
	"Compiler_Define_Function",
	"Compile_Time_Directive",
	"Arguments",
};


struct ASTNode {
	ASTNode* parentNode = nullptr;
	std::vector<ASTNode*> childNodes = std::vector<ASTNode*>();
	ASTNodeType nodeType = Nothing_Node;
	tokenPair token = tokenPair();
	int lineNumber = 0;
	// Add leaf nodes here as they are still yet to be used.
	std::vector<ASTNode*> leafNodes = std::vector<ASTNode*>();

	bool compareTokens = false;

	void* generateConstant();
	void* generateVariableExpression();
	void* generateBinaryExpression();
	void* generateScopeBody();
	void* generateFunction();
	void* generateCallExpression();
	void* generatePrototype();

	void* (ASTNode::*codegen)() = nullptr;

	//std::unique_ptr<PrototypeAST> Proto;
	//std::unique_ptr<ExprAST> Body;

	ASTNode() {}
	ASTNode(ASTNodeType nType)
	{
		nodeType = nType;
	}
	ASTNode(ASTNodeType nType, std::vector<ASTNode*> cNodes)
	{
		childNodes = cNodes;
		nodeType = nType;
	}
	ASTNode(std::vector<ASTNode*> cNodes, ASTNodeType nType)
	{
		childNodes = cNodes;
		nodeType = nType;
	}
	ASTNode(ASTNodeType nType, std::vector<ASTNode*> cNodes, tokenPair t, bool cmpTokens = false)
	{
		childNodes = cNodes;
		nodeType = nType;
		token = t;
		compareTokens = cmpTokens;
	}
	ASTNode(std::vector<ASTNode*> cNodes, ASTNodeType nType, tokenPair t, bool cmpTokens = false)
	{
		childNodes = cNodes;
		nodeType = nType;
		token = t;
		compareTokens = cmpTokens;
	}

	bool operator==(ASTNode other)
	{
		// Make sure children are the same as well
		if (childNodes.size() == other.childNodes.size())
			for (int i = 0; i < childNodes.size(); i++) {
				if ((*(childNodes[i]) == *(other.childNodes[i])) == false)
					return false;
			}
		else
			return false;

		// If child nodes match, then we have to check the current node
		if (compareTokens)
			if (!(token == other.token))
				return false;
		if (nodeType == other.nodeType /* && token == other.token &&
			lineNumber == other.lineNumber*/
		)
			return true;

		// Otherwise false
		return false;
	}
};


extern ASTNode* rootNode;
extern std::vector<ASTNode*> importedNodes;

int beginParse(const std::vector<tokenPair>& tokens);
ASTNode* generateAST(const std::vector<tokenPair>& tokens, int depth = 0, ASTNode* parentNodePtr = nullptr);
void orderASTOperations(ASTNode* startNode);
const std::string ASTNodeTypeAsString(ASTNodeType t);
int printAST(ASTNode* startNode, int depth = 0);
void fixPrecedence(ASTNode*& node);
void optimizeASTNode(ASTNode*& node);
void addFileIncludes(ASTNode*& node);
void addModuleImports(ASTNode*& node);
void assignParentNodes(ASTNode*& node);
void printTokenError(tokenPair& token, std::string errorString = "", int sourceLineNumber = 0);
void printModuleLoaded(std::string& moduleName, std::string& modulePath);
void generateOutputCode(ASTNode*& node, int depth = 0);
//std::vector<tokenPair> GATHER_SCOPE_BODY(int brLevel, int& i);
