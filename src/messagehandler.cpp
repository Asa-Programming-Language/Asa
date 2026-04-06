
#include "messagehandler.h"

namespace messageSystem {

	//ErrorNode* errorTree = nullptr;
	ErrorNode* currentNode = nullptr;

	bool suppressErrors = false;

	void startBlock(ASTNode* node, std::string message, const char* sourceFunction, uint32_t sourceLine, const char* sourcePath)
	{
		if (currentNode == nullptr) {
			currentNode = new ErrorNode(node, message, sourceFunction, sourceLine, sourcePath);
		}
		else {
			ErrorNode* errorNode = new ErrorNode(node, message, sourceFunction, sourceLine, sourcePath);
			errorNode->parentNode = currentNode;
			//currentNode->childNodes.push_back(errorNode);
			currentNode = errorNode;
		}
	}

	void endBlock()
	{
		if (currentNode) {
			ErrorNode* parentNode = currentNode->parentNode;
			delete currentNode;

			if (parentNode)
				currentNode = parentNode;
			else
				currentNode = nullptr;
		}
	}

	void* error(std::string message)
	{
		wasError = true;

		if (suppressErrors)
			return nullptr;

		console::PrintError(message);

		if (!currentNode)
			console::PrintError("Unexpected end of error tree.");

		//console::indentation = errorDepth;
		for (int i = 0; i < maxErrorTraceDepth; i++) {
			console::indentation = i;
			if (currentNode->sourceFunction != "" && currentNode->sourceLine != 0 && currentNode->sourcePath != "")
				printErrorMessage(currentNode->astNode, currentNode->message, currentNode->sourceFunction, currentNode->sourceLine, currentNode->sourcePath);
			if (currentNode->parentNode == nullptr)
				break;
			currentNode = currentNode->parentNode;
		}

		console::indentation = 0;
		console::WriteLine("Errors were encountered during compilation.", console::redFGColor);

		exit(1);

		return nullptr;
	}

	void printErrorMessage(ASTNode*& node, std::string message, std::string sourceFunction, uint32_t sourceLine, std::string sourcePath)
	{
		tokenRange tokRange = getASTTokenRange(node);
		tokenPair* startToken = tokRange.first;
		tokenPair* endToken = tokRange.second;
		if (verbosity >= 5) {
			if (currentNode->sourcePath != "" && currentNode->sourcePath != "\0")
				std::cerr << "Source file: " << currentNode->sourcePath << std::endl;
			if (currentNode->sourceLine > 0)
				std::cerr << "Line: " << currentNode->sourceLine << std::endl;
		}
		console::ApplyIndent();
		console::Write("While: " + message + "\n", console::blueFGColor);
		if (!startToken || !startToken->filePath || !startToken->lineValue) {
			console::WriteLine("(no source location available)");  // This should never happen
			return;
		}
		console::ApplyIndent();
		console::Write("In: ", console::yellowFGColor);
		console::Write(*(startToken->filePath), console::yellowFGColor);
		console::Write("\n");
		std::string lineNumberStr = std::to_string(startToken->lineNumber + 1);
		console::ApplyIndent();
		console::Write(lineNumberStr + " |  ", console::yellowFGColor);
		console::Write(*(startToken->lineValue));
		console::Write("\n");
		console::ApplyIndent();
		for (int i = 0; i < lineNumberStr.size() + 4 + startToken->indexInLine - 1; i++)
			console::Write(" ");
		for (int i = 0; i < endToken->indexInLine + endToken->length - startToken->indexInLine; i++)
			console::Write("^", console::redFGColor);
		console::Write("\n");
		console::ApplyIndent();
		for (int i = 0; i < lineNumberStr.size() + 4 + startToken->indexInLine - 1; i++)
			console::Write(" ");
		console::Write("here", console::redFGColor);
		console::WriteLine("\n");
		// If debugging the compiler, throw so that the call can be traced
		if (compilerFlags & Flags_CompilerDebug)
			throw;
		//exit(1);
	}


}  // namespace messageSystem
