#ifndef MESSAGEHANDLER_H
#define MESSAGEHANDLER_H

#include <string>
#include <vector>

#include "console.h"
#include "parser.h"
#include "settings.h"
#include "tokenizer.h"

struct ASTNode;

namespace messageSystem {
	struct ErrorNode {
		ErrorNode* parentNode = nullptr;
		std::vector<ErrorNode*> childNodes = std::vector<ErrorNode*>();

		ASTNode* astNode = nullptr;
		std::string message = "";
		std::string sourceFunction = "";
		std::string sourcePath = "";
		uint32_t sourceLine = 0;

		ErrorNode(ASTNode*& node, std::string message, const char* sourceFunction, uint32_t sourceLine, const char* sourcePath)
		{
			astNode = node;
			this->message = message;
			this->sourceFunction = std::string(sourceFunction);
			this->sourcePath = std::string(sourcePath);
			this->sourceLine = sourceLine;
			childNodes = std::vector<ErrorNode*>();
		}
	};

	extern ErrorNode* errorTree;
	extern ErrorNode* currentNode;

	extern bool suppressErrors;

	void startBlock(ASTNode* node, std::string message, const char* sourceFunction, uint32_t sourceLine, const char* sourcePath);
	void endBlock();
	void* error(std::string message);
	void printErrorMessage(ASTNode*& node, std::string message, std::string sourceFunction, uint32_t sourceLine, std::string sourcePath);

}  // namespace messageSystem

#endif
