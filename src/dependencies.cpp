#include "dependencies.h"

int resolveDependencies(ASTNode*& node)
{
	for (int i = 0; i < node->childNodes.size(); i++)
		resolveDependencies(node->childNodes[i]);

	if (node->nodeType == Function_Call) {
		std::string identifier = node->token->first;
		bool foundDefinition = false;
		ASTNode* parent = node->parentNode;
		// Work backwards up node tree
		for (;;) {
			if (parent == nullptr) {
				printTokenError(node->token, "Function definition not found");
				exit(1);
			}

			for (int i = 0; i < parent->childNodes.size(); i++) {
				ASTNode* childNode = parent->childNodes[i];
				if (childNode->nodeType == Compiler_Define_Function || childNode->nodeType == Compiler_Define || childNode->nodeType == Compiler_Define_Cast)
					if (childNode->token->first == identifier) {
						foundDefinition = true;
						goto exitLoop;
					}
			}
			parent = parent->parentNode;
		}
	exitLoop:
		if (!foundDefinition) {
			printTokenError(node->token, "Function definition not found");
			exit(1);
		}
	}

	return 1;
}
