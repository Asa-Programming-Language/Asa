
#include "interpreter.h"

std::stack<ASTNode*> callStack = std::stack<ASTNode*>();
std::unordered_map<std::string, ASTNode*> functions = std::unordered_map<std::string, ASTNode*>();
ASTNode* currentExecutingNode = nullptr;

void runCycle();

void getFunctions(ASTNode*& node)
{
	if (node->nodeType == Compiler_Define_Function) {
		if (functions.count(node->token->first) == 0) {
			functions[node->token->first] = node;
		}
	}

	for (int l = 0; l < node->childNodes.size(); l++) {
		getFunctions(node->childNodes[l]);
	}
}

void executeScope(ASTNode*& scope)
{
	for (ASTNode*& child : scope->childNodes) {
		if (verbosity >= 3)
			console::Write("L" + std::to_string(child->lineNumber) + ": ");
		switch (child->nodeType) {
			case Return_Node:
				console::WriteLine("RETURN");
				return;
			default:
				break;
		}
	}
}

void executeFunctionNode(ASTNode*& node)
{
	ASTNode* body = node->childNodes[4];
	body->interpreterScopeValues = node->interpreterScopeValues;
	executeScope(body);
}

void startTreeWalkExecution(ASTNode*& node)
{
	console::WriteLine("Starting code execution at `main`:");
	getFunctions(node);
	if (functions.count("main")) {
		executeFunctionNode(functions["main"]);
	}
	else {
		console::WriteLine("ERROR: There was no entry `main` function", console::redFGColor);
		exit(1);
	}
}

void runCycle()
{
}
