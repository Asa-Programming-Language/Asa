#pragma once

#include "codegen.h"
#include "filemanager.h"
#include "pch.h"
#include "settings.h"
#include "strops.h"
#include "tokenizer.h"

#define MAX_AST_DEPTH 20

#define PRINT_SUBTOKENS(subTokens)                  \
	for (int ST = 0; ST < subTokens.size(); ST++)   \
		printf("%s ", subTokens[ST].first.c_str()); \
	printf("\n");

using namespace llvm;

void GO_BACK_TO_BEGINNING_OF_TERM(int& i);

extern bool wasError;

enum ASTNodeType {
	// Null type
	Nothing_Node,

	Identifier_Node,
	Integer_Node,
	SInt128_Type,
	SInt64_Type,
	SInt32_Type,
	SInt16_Type,
	SInt8_Type,
	Begin_Unsigned_Integers,
	UInt128_Type,
	UInt64_Type,
	UInt32_Type,
	UInt16_Type,
	UInt8_Type,
	Char_Type,
	Boolean_Node,
	Double_Type,
	Float_Node,
	Half_Type,
	String_Node,
	String_Constant_Node,
	Character_Constant_Node,
	Type_Node,
	Void_Node,

	Module_Scope,

	Redefined_Operator_Expr,
	Operator_Type_Node,
	Expression_Term,
	Expression_Paren_Term,
	Expression_Statement,
	Expression_Plus,
	Expression_Minus,
	Expression_Times,
	Expression_Divide,
	Expression_Modulo,
	Bitwise_And,
	Bitwise_Or,
	Bitwise_Xor,
	Bitwise_Shift_Left,
	Bitwise_Shift_Right,
	Pipe_Operation,
	Pipe_Placeholder,
	Comma_Node,
	Colon_Separator_Node,

	Range_Node,

	Argument_List,

	Function_Call,

	If_Statement_Node,

	Else_Statement_Node,

	For_Statement_Node,

	While_Statement_Node,

	Struct_Define_Node,
	Struct_Type,

	Module_Define_Node,

	Operator_Overload_Node,

	Access_Operation,
	Member_Access,
	Reference_Operation,
	Const_Keyword,
	Exact_Type_Node,
	Address_Of_Operation,
	Dereference_Operation,
	Pointer_Node,

	Return_Node,
	Continue_Node,
	Break_Node,
	Goto_Node,

	Throw_Node,

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
	Compiler_Define_Cast,
	Compiler_Define_Struct,
	Compile_Time_Directive,
	Labeled_Loop,
	Arguments,
	Compiler_Modifiers,

	Attribute_Node,
	Comment_Node,

	Fully_Defined,

	// Nothing below this
	LastASTNodeType
};

extern std::map<TokenType, ASTNodeType> binaryOperatorExType;

const std::string ASTNodeTypeStrings[] = {
	"Nothing_Node",

	"Identifier_Node",
	"Integer_Node",
	"SInt128_Type",
	"SInt64_Type",
	"SInt32_Type",
	"SInt16_Type",
	"SInt8_Type",
	"Begin_Unsigned_Integers",
	"UInt128_Type",
	"UInt64_Type",
	"UInt32_Type",
	"UInt16_Type",
	"UInt8_Type",
	"Char_Type",
	"Boolean_Node",
	"Double_Type",
	"Float_Node",
	"Half_Type",
	"String_Node",
	"String_Constant_Node",
	"Character_Constant_Node",
	"Type_Node",
	"Void_Node",

	"Module_Scope",

	"Redefined_Operator_Expr",
	"Operator_Type_Node",
	"Expression_Term",
	"Expression_Paren_Term",
	"Expression_Statement",
	"Expression_Plus",
	"Expression_Minus",
	"Expression_Times",
	"Expression_Divide",
	"Expression_Modulo",
	"Bitwise_And",
	"Bitwise_Or",
	"Bitwise_Xor",
	"Bitwise_Shift_Left",
	"Bitwise_Shift_Right",
	"Pipe_Operation",
	"Pipe_Placeholder",
	"Comma_Node",
	"Colon_Separator_Node",

	"Range_Node",

	"Argument_List",

	"Function_Call",

	"If_Statement_Node",

	"Else_Statement_Node",

	"For_Statement_Node",

	"While_Statement_Node",

	"Struct_Define_Node",
	"Struct_Type",

	"Module_Define_Node",

	"Operator_Overload_Node",

	"Access_Operation",
	"Member_Access",
	"Reference_Operation",
	"Const_Keyword",
	"Exact_Type_Node",
	"Address_Of_Operation",
	"Dereference_Operation",
	"Pointer_Node",

	"Return_Node",
	"Continue_Node",
	"Break_Node",
	"Goto_Node",

	"Throw_Node",

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
	"Compiler_Define_Cast",
	"Compiler_Define_Struct",
	"Compile_Time_Directive",
	"Labeled_Loop",
	"Arguments",
	"Compiler_Modifiers",

	"Attribute_Node",
	"Comment_Node",

	"Fully_Defined",
};

struct valueType {
	std::string name;
	std::string type;
	bool isFunctionArgument = false;
	bool isConstant = false;
	bool isReference = false;
	Value* val;
	valueType(std::string n, std::string t, Value* v, bool arg = false, bool ref = false)
		: name(n), type(t), val(v), isFunctionArgument(arg), isReference(ref) {};
};

struct ASAType {
	Type* llvmType = nullptr;
	Type* baseLLVMType = nullptr;
	bool inferredType = false;
	bool isRef = false;
	bool isConst = false;
	std::string strVal = "";
	uint8_t pointerLevel = 0;

	ASAType(Type* baseLLVMType, bool isRef, bool isConst, std::string strVal, uint8_t pointerLevel)
		: baseLLVMType(baseLLVMType), isRef(isRef), isConst(isConst), strVal(strVal), pointerLevel(pointerLevel) {};
	ASAType(Type* baseLLVMType)
		: baseLLVMType(baseLLVMType) {};
};

struct ASTNode {
	ASTNode* parentNode = nullptr;
	std::vector<ASTNode*> childNodes = std::vector<ASTNode*>();
	ASTNodeType nodeType = Nothing_Node;
	tokenPair* token = new tokenPair();
	int lineNumber = 0;
	uint16_t depth = 0;
	bool isRef = false;
	bool isConst = false;
	bool isExtern = false;
	bool lvalue = false;
	std::string label = "";
	bool showInASTOutput = true;
	bool replaceableDefinition = false;
	bool isModuleScope = false;		   // true for Compiler_Define nodes representing named modules
	std::string enclosingModule = "";  // set on imported nodes to record source module name
	bool currentNodeDoneGenerating = false;
	//Type* llvmType;
	//Type* baseType = nullptr;
	ASAType* asaType = nullptr;
	// Add leaf nodes here as they are still yet to be used.
	std::vector<ASTNode*> leafNodes = std::vector<ASTNode*>();
	std::vector<ASTNode*> attributes = std::vector<ASTNode*>();

	std::unordered_map<std::string, ASTNode*> interpreterScopeValues = std::unordered_map<std::string, ASTNode*>();

	std::map<std::string, valueType*> namedValues = std::map<std::string, valueType*>();

	bool compareTokens = false;

	void* generateConstant(int pass = 0);
	void* generateVariableExpression(int pass = 0);
	void* generateReturn(int pass = 0);
	void* generateBreak(int pass = 0);
	void* generateContinue(int pass = 0);
	void* generateExpression(int pass = 0);
	void* generateExpressionStatement(int pass = 0);
	void* generateIterator(int pass = 0);
	void* generateUnaryExpression(int pass = 0);
	void* generateBinaryExpression(int pass = 0);
	void* generatePipePlaceholder(int pass = 0);
	void* generateAccessOperation(int pass = 0);
	void* generateMemberAccess(int pass = 0);
	void* generateScopeBody(int pass = 0);
	void* generateIf(int pass = 0);
	void* generateStruct(int pass);
	void* generateLabeledLoop(int pass = 0);
	void* generateFor(int pass = 0);
	void* generateWhile(int pass = 0);
	void* generatePrototype(int pass = 0);
	void* generateFunction(int pass = 0);
	void* generateCast(int pass = 0);
	void* generateThrow(int pass = 0);
	void* generateTypeInstance(int pass = 0);
	void* generateCallExpression(int pass = 0);
	void* generateNothing(int pass = 0);
	void* generateCompilerDefine(int pass = 0);
	void* generateTypeofDirective(int pass = 0);
	void* generateSizeofDirective(int pass = 0);

	void* (ASTNode::*codegen)(int pass) = nullptr;

	// Helper functions:

	Value* generateOperatorOverloadCall(Value* L, Value* R);
	bool checkForOperatorOverload(Value* L, Value* R);
	Value* generatePointerBinaryOp(Value* L, Value* R);
	Value* generateFloatBinaryOp(Value* L, Value* R);
	Value* generateIntegerBinaryOp(Value* L, Value* R);

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
	ASTNode(ASTNodeType nType, std::vector<ASTNode*> cNodes, tokenPair* t, bool cmpTokens = false)
	{
		childNodes = cNodes;
		nodeType = nType;
		token = t;
		compareTokens = cmpTokens;
	}
	ASTNode(std::vector<ASTNode*> cNodes, ASTNodeType nType, tokenPair* t, bool cmpTokens = false)
	{
		childNodes = cNodes;
		nodeType = nType;
		token = t;
		compareTokens = cmpTokens;
	}

	bool empty()
	{
		return childNodes.size() == 0;
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
			if (!(*token == *(other.token)))
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

int beginParse(const std::vector<tokenPair*>& tokens);
ASTNode* generateAST(const std::vector<tokenPair*>& tokens, int depth = 0, ASTNode* parentNodePtr = nullptr);
void orderASTOperations(ASTNode* startNode);
const std::string ASTNodeTypeAsString(ASTNodeType t);
int printAST(ASTNode* startNode, int depth = 0);
void stripCommentNodes(ASTNode* node);
void fixPrecedence(ASTNode*& node);
void unifyNodes(ASTNode*& node);
void optimizeASTNode(ASTNode*& node);
void resolveCompileTimeDirectives(ASTNode*& node, std::string moduleCtx = "", std::string funcCtx = "");
void addFileIncludes(ASTNode*& node);
void addModuleImports(ASTNode*& node);
void assignParentNodes(ASTNode*& node, int depth = 0);
void printTokenMarked(tokenPair*& token, std::string msgString = "", int sourceLineNumber = 0, const char* fileName = "");
void printTokenError(tokenPair*& token, std::string errorString = "", int sourceLineNumber = 0, const char* fileName = "");
void printTokenWarning(tokenPair*& token, std::string errorString = "", int sourceLineNumber = 0, const char* fileName = "");
void findUnusedLeafNodes(ASTNode*& node);
void printModuleLoaded(std::string& moduleName, std::string& modulePath);
void generateOutputCode(ASTNode*& node, int depth = 0, int pass = 0);
//std::vector<tokenPair> GATHER_SCOPE_BODY(int brLevel, int& i);
