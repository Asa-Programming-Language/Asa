#include "parser.h"

void* (ASTNode::*codegen)() = nullptr;

tokenPair* getNextNonNothingToken(const std::vector<tokenPair*>& tokens, int& i)
{
	tokenPair* t = new tokenPair();
	for (;;) {
		if (i >= tokens.size() - 1 || tokens[i]->second == EndOfFile) {
			return t;
		}
		tokenPair* t = NEXT_TOKEN(tokens, i);

		if (t->second != Nothing && t->second != EndOfLine) {
			return t;
		}
	}
	return t;
}

void setChildrenAsExtern(ASTNode*& node)
{
	for (auto& c : node->childNodes)
		setChildrenAsExtern(c);
	node->isExtern = true;
}

bool GATHER_SCOPE_BODY(const std::vector<tokenPair*>& tokens, std::vector<tokenPair*>& subTokens, int brLevel, int& i, bool preserveBraces = false)
{
	int braceLevel = brLevel;
	subTokens = std::vector<tokenPair*>();
	tokenPair* firstToken;
	if (i < tokens.size() - 1) {
		firstToken = NEXT_TOKEN(tokens, i);
		//if (firstToken.second == Semi_Colon)
		//	return true;
	}
	else {
		return true;
	}
	i--;
	for (;;) {
		if (i >= tokens.size() - 1 || tokens[i]->second == EndOfFile) {
			printTokenError(firstToken, "Unmatched brace", __LINE__);
			exit(1);
			break;
		}
		tokenPair* t = NEXT_TOKEN(tokens, i);

		if (t->second == Nothing)
			continue;

		if (braceLevel <= 0 && t->second == Semi_Colon)
			return true;
		if (t->second == Left_Brace) {
			if (braceLevel != 0 || preserveBraces)
				subTokens.push_back(t);
			braceLevel++;
		}
		else if (t->second == Right_Brace) {
			braceLevel--;
			if (braceLevel != 0 || preserveBraces)
				subTokens.push_back(t);
		}
		else
			subTokens.push_back(t);

		if (braceLevel <= 0)
			break;
	}
	return false;
}

bool GATHER_SCOPE_BODY_APPEND(const std::vector<tokenPair*>& tokens, std::vector<tokenPair*>& subTokens, int brLevel, int& i, bool preserveBraces = false)
{
	int braceLevel = brLevel;
	tokenPair* firstToken = NEXT_TOKEN(tokens, i);
	i--;
	for (;;) {
		if (i >= tokens.size() - 1 || tokens[i]->second == EndOfFile) {
			printTokenError(firstToken, "Unmatched brace");
			exit(1);
			break;
		}
		tokenPair* t = NEXT_TOKEN(tokens, i);

		if (t->second == Nothing)
			continue;

		if (braceLevel <= 0 && t->second == Semi_Colon)
			return true;
		if (t->second == Left_Brace) {
			if (braceLevel != 0 || preserveBraces)
				subTokens.push_back(t);
			braceLevel++;
		}
		else if (t->second == Right_Brace) {
			braceLevel--;
			if (braceLevel != 0 || preserveBraces)
				subTokens.push_back(t);
		}
		else
			subTokens.push_back(t);

		if (braceLevel <= 0)
			break;
	}
	return false;
}


void GATHER_PAREN_EXPRESSION(const std::vector<tokenPair*>& tokens, std::vector<tokenPair*>& subTokens, int pLevel, int& i, bool preserveBraces = false)
{
	int parenLevel = pLevel;
	if (pLevel == 1)
		i--;
	tokenPair* firstToken = NEXT_TOKEN(tokens, i);
	if (pLevel != 1)
		i--;
	for (;;) {
		if (i >= tokens.size() - 1 || tokens[i]->second == EndOfFile || tokens[i]->second == Semi_Colon) {
			printTokenError(firstToken, "Unmatched parenthesis");
			exit(1);
			break;
		}
		tokenPair* t = NEXT_TOKEN(tokens, i);

		if (t->second == Nothing)
			continue;

		if (t->second == Left_Paren) {
			if (parenLevel != 0 || preserveBraces)
				subTokens.push_back(t);
			parenLevel++;
		}
		else if (t->second == Right_Paren) {
			parenLevel--;
			if (parenLevel != 0 || preserveBraces)
				subTokens.push_back(t);
		}
		else
			subTokens.push_back(t);

		if (parenLevel <= 0)
			break;
	}
}

bool GATHER_TO_SEMICOLON(const std::vector<tokenPair*>& tokens, std::vector<tokenPair*>& subTokens, int& i, bool includeLast = false, bool allowRunOut = false)
{
	tokenPair* firstToken;
	if (i < tokens.size() - 1) {
		firstToken = NEXT_TOKEN(tokens, i);
		//if (firstToken.second == Semi_Colon)
		//	return true;
	}
	else {
		return true;
	}
	i--;
	for (;;) {
		if (i >= tokens.size() - 1 || tokens[i]->second == EndOfLine) {
			if (!allowRunOut) {
				printTokenError(firstToken, "Missing semicolon");
				exit(1);
			}
			return true;
		}
		tokenPair* t = NEXT_TOKEN(tokens, i);

		if (t->second == Nothing)
			continue;

		if (t->second == Semi_Colon) {
			if (includeLast)
				subTokens.push_back(t);
			break;
		}

		subTokens.push_back(t);
	}
	return false;
}

bool GATHER_TO_SEMICOLON_MULTI_LINE(const std::vector<tokenPair*>& tokens, std::vector<tokenPair*>& subTokens, int& i, bool includeLast = false, bool allowRunOut = false)
{
	tokenPair* firstToken;
	if (i < tokens.size() - 1) {
		firstToken = NEXT_TOKEN(tokens, i);
		//if (firstToken.second == Semi_Colon)
		//	return true;
	}
	else {
		return true;
	}
	i--;
	for (;;) {
		if (i >= tokens.size() - 1) {
			if (!allowRunOut) {
				printTokenError(firstToken, "Missing semicolon");
				exit(1);
			}
			return true;
		}
		tokenPair* t = NEXT_TOKEN(tokens, i);

		if (t->second == Nothing)
			continue;

		if (t->second == Semi_Colon) {
			if (includeLast)
				subTokens.push_back(t);
			break;
		}

		subTokens.push_back(t);
	}
	return false;
}

bool GATHER_TO_SEMICOLON_OR_OTHER(const std::vector<tokenPair*>& tokens, std::vector<tokenPair*>& subTokens, int& i, TokenType other, bool includeLast = false, bool allowRunOut = false)
{
	tokenPair* firstToken;
	if (i < tokens.size() - 1) {
		firstToken = NEXT_TOKEN(tokens, i);
		//if (firstToken.second == Semi_Colon)
		//	return true;
	}
	else {
		return true;
	}
	i--;
	for (;;) {
		if (i >= tokens.size() - 1 || tokens[i]->second == EndOfLine) {
			if (!allowRunOut) {
				printTokenError(firstToken, "Missing semicolon");
				exit(1);
			}
			break;
		}
		tokenPair* t = NEXT_TOKEN(tokens, i);

		if (t->second == Nothing)
			continue;

		if (t->second == Semi_Colon || t->second == other) {
			if (includeLast)
				subTokens.push_back(t);
			break;
		}

		subTokens.push_back(t);
	}
	return false;
}

bool GATHER_TO_A_OR_B(const std::vector<tokenPair*>& tokens, std::vector<tokenPair*>& subTokens, int pLevel, int& i, TokenType a, TokenType b, bool includeLast)
{
	tokenPair* firstToken;
	if (i < tokens.size() - 1) {
		firstToken = NEXT_TOKEN(tokens, i);
		//if (firstToken.second == Semi_Colon)
		//	return true;
	}
	else {
		return true;
	}
	i--;
	for (;;) {
		if (i >= tokens.size() - 1) {
			printTokenError(firstToken, "End of file reached before expected " + tokenAsString(a) + " or " + tokenAsString(b));
			exit(1);
			break;
		}
		tokenPair* t = NEXT_TOKEN(tokens, i);

		if (t->second == Nothing)
			continue;

		if (t->second == a || t->second == b) {
			if (includeLast)
				subTokens.push_back(t);
			break;
		}

		subTokens.push_back(t);
	}
	return false;
}

bool GATHER_TO_TOKEN(const std::vector<tokenPair*>& tokens, std::vector<tokenPair*>& subTokens, int pLevel, int& i, TokenType a, bool includeLast = false, bool allowRunOut = false)
{
	tokenPair* firstToken;
	if (i < tokens.size() - 1) {
		firstToken = NEXT_TOKEN(tokens, i);
		//if (firstToken.second == Semi_Colon)
		//	return true;
	}
	else {
		return true;
	}
	i--;
	for (;;) {
		if (i >= tokens.size() - 1) {
			if (!allowRunOut) {
				printTokenError(firstToken, "End of file reached before expected " + tokenAsString(a));
				exit(1);
			}
			break;
		}
		tokenPair* t = NEXT_TOKEN(tokens, i);

		if (t->second == a) {
			if (includeLast)
				subTokens.push_back(t);
			break;
		}

		subTokens.push_back(t);
	}
	return false;
}

void printTokenMarked(tokenPair*& token, std::string msgString, int sourceLineNumber, const char* fileName)
{
	if (verbosity >= 5) {
		if (fileName != "" && fileName != "\0")
			std::cerr << "Source file: " << fileName << std::endl;
		if (sourceLineNumber > 0)
			std::cerr << "Line: " << sourceLineNumber << std::endl;
	}
	if (msgString != "")
		console::WriteLine(msgString);
	console::Write("In: ", console::yellowFGColor);
	console::WriteLine(*(token->filePath), console::yellowFGColor);
	std::string lineNumberStr = std::to_string(token->lineNumber);
	console::Write(lineNumberStr + " |  ", console::yellowFGColor);
	console::WriteLine(*(token->lineValue));
	for (int i = 0; i < lineNumberStr.size() + 4 + token->indexInLine - 1 + console::indentation * 4; i++)
		console::Write(" ");
	for (int i = 0; i < token->length; i++)
		console::Write("^", console::blueFGColor);
	console::WriteLine();
	for (int i = 0; i < lineNumberStr.size() + 4 + token->indexInLine - 1 + console::indentation * 4; i++)
		console::Write(" ");
	console::Write("here", console::blueFGColor);
	console::WriteLine("\n");
}

void printTokenError(tokenPair*& token, std::string errorString, int sourceLineNumber, const char* fileName)
{
	if (verbosity >= 5) {
		if (fileName != "" && fileName != "\0")
			std::cerr << "Source file: " << fileName << std::endl;
		if (sourceLineNumber > 0)
			std::cerr << "Line: " << sourceLineNumber << std::endl;
	}
	console::PrintError(errorString);
	console::Write("In: ", console::yellowFGColor);
	console::WriteLine(*(token->filePath), console::yellowFGColor);
	std::string lineNumberStr = std::to_string(token->lineNumber);
	console::Write(lineNumberStr + " |  ", console::yellowFGColor);
	console::WriteLine(*(token->lineValue));
	for (int i = 0; i < lineNumberStr.size() + 4 + token->indexInLine - 1 + console::indentation * 4; i++)
		console::Write(" ");
	for (int i = 0; i < token->length; i++)
		console::Write("^", console::redFGColor);
	console::WriteLine();
	for (int i = 0; i < lineNumberStr.size() + 4 + token->indexInLine - 1 + console::indentation * 4; i++)
		console::Write(" ");
	console::Write("here", console::redFGColor);
	console::WriteLine("\n");
	// If debugging the compiler, throw so that the call can be traced
	if (compilerFlags & Flags_CompilerDebug)
		throw;
	//exit(1);
}

void printTokenWarning(tokenPair*& token, std::string errorString, int sourceLineNumber, const char* fileName)
{
	if (verbosity >= 5) {
		if (fileName != "")
			std::cerr << "Source file: " << fileName << std::endl;
		if (sourceLineNumber > 0)
			std::cerr << "Line: " << sourceLineNumber << std::endl;
	}
	console::PrintWarning(errorString);
	console::Write("In: ", console::yellowFGColor);
	console::WriteLine(*(token->filePath), console::yellowFGColor);
	std::string lineNumberStr = std::to_string(token->lineNumber);
	console::Write(lineNumberStr + " |  ", console::yellowFGColor);
	console::WriteLine(*(token->lineValue));
	for (int i = 0; i < lineNumberStr.size() + 4 + token->indexInLine - 1 + console::indentation * 4; i++)
		console::Write(" ");
	for (int i = 0; i < token->length; i++)
		console::Write("^", console::yellowFGColor);
	console::WriteLine();
	for (int i = 0; i < lineNumberStr.size() + 4 + token->indexInLine - 1 + console::indentation * 4; i++)
		console::Write(" ");
	console::Write("here", console::yellowFGColor);
	console::WriteLine("\n");
	//throw;
}

void printModuleLoaded(std::string& moduleName, std::string& modulePath)
{
	std::cout << "Module \"" << moduleName << "\" imported";
	if (verbosity >= 3)
		std::cout << " from: " << modulePath;
	std::cout << std::endl;
}

void findUnusedLeafNodes(ASTNode*& node)
{
	for (auto& l : node->leafNodes) {
		printTokenError(l->token, "Failed to compile");
		wasError = true;
	}
	if (wasError)
		return;
	for (auto& c : node->childNodes)
		findUnusedLeafNodes(c);
}

std::vector<ASTNode*> ASTNodes = std::vector<ASTNode*>();

std::map<TokenType, ASTNodeType> operatorDefaultNodeType = {
	{Colon, Colon_Separator_Node},
	{Dot_Dot, Range_Node},
	{Comma, Comma_Node},
	{Bang_Equal, Compare_Not},
	{Equal_Equal, Compare_Equal},
	{Less, Compare_Less},
	{Greater, Compare_Greater},
	{Less_Equal, Compare_LessEqual},
	{Greater_Equal, Compare_GreaterEqual},
	{Plus, Expression_Plus},
	{Minus, Expression_Minus},
	{Star, Expression_Times},
	{Slash, Expression_Divide},
	{Ampersand, Address_Of_Operation},
	{Ref, Reference_Operation},
	{Const, Const_Keyword},
	{Exact, Exact_Type_Node},
	{Left_Bracket, Access_Operation},
	{Dot, Member_Access},
	{Arrow_Right, Pipe_Operation},
	{Percent, Expression_Modulo},
	{At, Expression_Modulo},
};

std::map<ASTNodeType, int> operatorPrecedence = {
	{Operator_Overload_Node, 100},	// anything else
	{Colon_Separator_Node, 99},		// :
	{Member_Access, 90},			// .
	{Access_Operation, 80},			// []
	{Expression_Paren_Term, 70},	// ()
	{Address_Of_Operation, 50},		// &
	{Expression_Times, 40},			// *
	{Expression_Divide, 40},		// /
	{Expression_Modulo, 40},		// %
	{Expression_Plus, 30},			// +
	{Expression_Minus, 30},			// -
	{Compare_Equal, 20},			// ==
	{Compare_Not, 20},				// !=
	{Compare_Less, 20},				// <
	{Compare_LessEqual, 20},		// <=
	{Compare_Greater, 20},			// >
	{Compare_GreaterEqual, 20},		// >=
	{Range_Node, 15},				// ..
	{Comma_Node, 12},				// ,
	{Pipe_Operation, 10},			// ->
};

std::unordered_set<ASTNodeType> leftAssociativeOperators = {
	Member_Access,
	Access_Operation,
	Pipe_Operation,
};

std::unordered_set<ASTNodeType> literals = {
	Integer_Node,
	Float_Node,
	Boolean_Node,
	String_Node,
};

std::unordered_set<TokenType> compileTimeDefinable = {
	While_Statement,
	For_Statement,
	If_Statement,
	Struct_Define,
	Module_Define,
};

ASTNode* rootNode = new ASTNode();
std::vector<ASTNode*> importedNodes = std::vector<ASTNode*>();
std::unordered_set<std::string> importedModuleNames = std::unordered_set<std::string>();
std::unordered_set<std::string> importedFileNames = std::unordered_set<std::string>();

ASTNode* generateAST(const std::vector<tokenPair*>& tokens, int depth, ASTNode* parentNodePtr)
{
	if (depth == MAX_AST_DEPTH) {
		printf("Error: Max AST Depth of %d Reached", MAX_AST_DEPTH);
		exit(1);
	}

	ASTNode* parentNode = parentNodePtr;
	if (parentNodePtr == nullptr)
		parentNode = new ASTNode();

	// If root node
	if (depth == 0) {
		parentNode->token = new tokenPair("global", Nothing, 0, 0, nullptr, nullptr);
		//parentNode->tokenType = Nothing;
		parentNode->nodeType = Scope_Body;
	}

	// Iterate all tokens
	std::vector<ASTNode*> pendingAttributes;
	std::string pendingCommentText;
	int pendingCommentLastLine = -10;
	for (int i = 0; i < tokens.size(); i++) {
		ASTNode* node = new ASTNode();
		ASTNodes.push_back(node);
		//node.prevNode = &parentNode;
		tokenPair* token = tokens[i];
		std::string tokenValue = tokens[i]->first;
		TokenType tokenType = tokens[i]->second;
		int lineNumber = tokens[i]->lineNumber;
		bool unusedNode = false;

		//node->tokenType = tokenType;
		node->token = token;
		node->lineNumber = lineNumber;

		switch (tokenType) {
			case If_Statement: {
				node->nodeType = If_Statement_Node;
				node->codegen = &ASTNode::generateIf;

				ASTNode* conditionNode = new ASTNode();
				ASTNode* bodyNode = new ASTNode();
				//ASTNode* elseNode = new ASTNode();

				std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();

				// Step through all tokens until parens are closed
				GATHER_PAREN_EXPRESSION(tokens, subTokens, 0, i, false);

				conditionNode = generateAST(subTokens, depth + 1);
				conditionNode->nodeType = Condition;

				// Check if the next token is a left curly brace, and if not handle single line
				tokenPair* firstNextToken = getNextNonNothingToken(tokens, i);
				i--;
				if (firstNextToken->second == Left_Brace)
					// Step through all tokens to gather body until braces are closed
					GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);
				else {
					// Otherwise just get the next line until semicolon
					subTokens = std::vector<tokenPair*>();
					GATHER_TO_SEMICOLON_MULTI_LINE(tokens, subTokens, i, true, false);
					subTokens.insert(subTokens.begin(), new tokenPair("{", Left_Brace));
					subTokens.push_back(new tokenPair("}", Right_Brace));
				}

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;
				bodyNode->codegen = &ASTNode::generateScopeBody;


				node->childNodes.push_back(conditionNode);
				node->childNodes.push_back(bodyNode);
				ASTNode* blankElse = new ASTNode();	 // default else
				blankElse->nodeType = Else_Statement_Node;
				blankElse->codegen = &ASTNode::generateScopeBody;
				node->childNodes.push_back(blankElse);

				ASTNode* prevNode = node;
				//printAST(prevNode);
				// Handle else/else if chain
				for (;;) {
					subTokens = std::vector<tokenPair*>();
					int sI = i;
					tokenPair* nextToken = getNextNonNothingToken(tokens, i);  // i will next point to { or if
					if (nextToken->second == Else_Statement) {
						int sI2 = i;
						tokenPair* nextToken2 = getNextNonNothingToken(tokens, i);	// Gets the { or if
						// Else If
						if (nextToken2->second == If_Statement) {
							// Parse as regular if
							subTokens.push_back(nextToken2);  // add `if`
							GATHER_PAREN_EXPRESSION(tokens, subTokens, 0, i, true);

							// Check if the next token is a left curly brace, and if not handle single line
							tokenPair* firstNextToken = getNextNonNothingToken(tokens, i);
							i--;
							if (firstNextToken->second == Left_Brace)
								// Step through all tokens to gather body until braces are closed
								GATHER_SCOPE_BODY_APPEND(tokens, subTokens, 0, i, true);
							else {
								// Otherwise just get the next line until semicolon
								GATHER_TO_SEMICOLON_MULTI_LINE(tokens, subTokens, i, true, false);
								subTokens.insert(subTokens.begin(), new tokenPair("{", Left_Brace));
								subTokens.push_back(new tokenPair("}", Right_Brace));
							}

							ASTNode* elseNode = generateAST(subTokens, depth + 1)->childNodes[0];
							prevNode->childNodes[2] = elseNode;
							prevNode = elseNode;
							continue;
						}
						// Regular Else
						else {
							i = sI2;
							// Check if the next token is a left curly brace, and if not handle single line
							tokenPair* firstNextToken = getNextNonNothingToken(tokens, i);
							i--;
							if (firstNextToken->second == Left_Brace)
								// Step through all tokens to gather body until braces are closed
								GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);
							else {
								// Otherwise just get the next line until semicolon
								subTokens = std::vector<tokenPair*>();
								GATHER_TO_SEMICOLON_MULTI_LINE(tokens, subTokens, i, true, false);
								subTokens.insert(subTokens.begin(), new tokenPair("{", Left_Brace));
								subTokens.push_back(new tokenPair("}", Right_Brace));
							}
							ASTNode* elseNode = generateAST(subTokens, depth + 1);
							elseNode->nodeType = Else_Statement_Node;
							elseNode->codegen = &ASTNode::generateScopeBody;
							prevNode->childNodes[2] = elseNode;
							prevNode = elseNode;
							break;	// else must be the end of chain
						}
					}
					else {
						i = sI;
						break;
					}
				}
				break;
			}

			case Else_Statement: {

				printTokenError(token, "'else' statement must be preceded by at least one 'if' statement");
				exit(1);
				goto dontAddNodeForce;
			}

			case While_Statement: {
				node->nodeType = While_Statement_Node;
				node->codegen = &ASTNode::generateWhile;

				ASTNode* conditionNode = new ASTNode();
				ASTNode* bodyNode = new ASTNode();

				std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();

				// Step through all tokens until parens are closed
				GATHER_PAREN_EXPRESSION(tokens, subTokens, 0, i, false);

				conditionNode = generateAST(subTokens, depth + 1);
				conditionNode->nodeType = Condition;

				// Check if the next token is a left curly brace, and if not handle single line
				tokenPair* firstNextToken = getNextNonNothingToken(tokens, i);
				i--;
				if (firstNextToken->second == Left_Brace)
					// Step through all tokens to gather body until braces are closed
					GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);
				else {
					// Otherwise just get the next line until semicolon
					subTokens = std::vector<tokenPair*>();
					GATHER_TO_SEMICOLON_MULTI_LINE(tokens, subTokens, i, true, false);
					subTokens.insert(subTokens.begin(), new tokenPair("{", Left_Brace));
					subTokens.push_back(new tokenPair("}", Right_Brace));
				}

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;
				bodyNode->codegen = &ASTNode::generateScopeBody;

				node->childNodes.push_back(conditionNode);
				node->childNodes.push_back(bodyNode);
				break;
			}

			case For_Statement: {
				node->nodeType = For_Statement_Node;
				node->codegen = &ASTNode::generateFor;

				ASTNode* iteratorNode = new ASTNode();
				ASTNode* rangeNode = new ASTNode();
				ASTNode* bodyNode = new ASTNode();

				i++;

				// Step through all tokens until parens are closed
				int parenLevel = 1;
				std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();
				for (;;) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair* t = NEXT_TOKEN(tokens, i);

					if (t->second == Left_Paren)
						parenLevel++;
					if (t->second == Right_Paren) {
						parenLevel--;
						if (parenLevel == 0)
							break;
					}

					// If colon like =>  for(i : 0..10)
					// generate AST for i, and set iteratorNode as the output
					if (t->second == Colon && iteratorNode->nodeType == Nothing_Node) {
						iteratorNode = generateAST(subTokens, depth + 1);
						iteratorNode->nodeType = Iterator;
						iteratorNode->codegen = &ASTNode::generateIterator;
						subTokens = std::vector<tokenPair*>();	// Clear subtokens
						//ASTNode* exprStatement = new ASTNode();
						//exprStatement->nodeType = Expression_Statement;
						//exprStatement->childNodes.push_back(iteratorNode);
						//iteratorNode = exprStatement;
						continue;
					}

					subTokens.push_back(t);

					if (parenLevel == 0 || t->second == EndOfLine || t->second == Semi_Colon)
						break;
				}
				rangeNode = generateAST(subTokens, depth + 1)->childNodes[0];
				rangeNode->nodeType = Range_Node;

				// Check if the next token is a left curly brace, and if not handle single line
				tokenPair* firstNextToken = getNextNonNothingToken(tokens, i);
				i--;
				if (firstNextToken->second == Left_Brace)
					// Step through all tokens to gather body until braces are closed
					GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);
				else {
					// Otherwise just get the next line until semicolon
					subTokens = std::vector<tokenPair*>();
					GATHER_TO_SEMICOLON_MULTI_LINE(tokens, subTokens, i, true, false);
					subTokens.insert(subTokens.begin(), new tokenPair("{", Left_Brace));
					subTokens.push_back(new tokenPair("}", Right_Brace));
				}

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;
				bodyNode->codegen = &ASTNode::generateScopeBody;

				//// Only add iterator if one is explicity defined
				//if (iteratorNode->nodeType != Nothing_Node)
				node->childNodes.push_back(iteratorNode);
				node->childNodes.push_back(rangeNode);
				node->childNodes.push_back(bodyNode);
				break;
			}

			case Struct_Define: {
				node->nodeType = Struct_Define_Node;

				ASTNode* bodyNode = new ASTNode();

				std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();

				// Step through all tokens to gather body until braces are closed
				GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;
				bodyNode->codegen = &ASTNode::generateScopeBody;

				node->childNodes.push_back(bodyNode);
				break;
			}

			case Module_Define: {

				//ASTNode* bodyNode = new ASTNode();

				std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();

				// Step through all tokens to gather body until braces are closed
				GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);

				generateAST(subTokens, depth + 1, node);

				node->nodeType = Module_Define_Node;
				node->token = token;
				node->codegen = &ASTNode::generateScopeBody;

				break;
			}

			//case Colon: {
			//	ASTNode* firstTerm = new ASTNode();
			//	ASTNode* secondTerm = new ASTNode();

			//	// Instead of backtracking to get the first term, pop the leafNodes vector
			//	if (parentNode->leafNodes.size() == 0) {
			//		printTokenError(token, "Binary operator expected left argument");
			//		exit(1);
			//	}
			//	else {
			//		firstTerm = parentNode->leafNodes.back();
			//		parentNode->leafNodes.pop_back();
			//		node = firstTerm;
			//		node->nodeType = Identifier_Node;
			//	}

			//	// Step through all following tokens until parens are closed
			//	int parenLevel = 1;
			//	std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();
			//	for (;;) {
			//		if (i >= tokens.size() - 1)
			//			break;
			//		tokenPair* t = NEXT_TOKEN(tokens, i);

			//		if (t->second == Left_Paren)
			//			parenLevel++;
			//		if (t->second == Right_Paren)
			//			parenLevel--;

			//		if (parenLevel == 0)
			//			break;
			//		if (t->second == Equal || t->second == Semi_Colon) {
			//			i--;
			//			break;
			//		}
			//		if (t->second == EndOfLine) {
			//			break;
			//		}

			//		subTokens.push_back(t);
			//	}
			//	if (subTokens.size() == 0) {
			//		printTokenError(token, "Operator expected right argument");
			//		exit(1);
			//	}
			//	secondTerm = generateAST(subTokens, depth + 1)->childNodes[0];
			//	secondTerm->nodeType = Type_Node;

			//	node->childNodes.push_back(secondTerm);

			//	goto addNodeAsLeaf;
			//	break;
			//}

			// Operators:
			// builtin:
			case Colon:
			case Comma:
			case Dot_Dot:
			case Dot:
			case Bang_Equal:
			case Equal_Equal:
			case Less:
			case Less_Equal:
			case Greater:
			case Greater_Equal:
			case Plus:
			case Minus:
			case Star:
			case Slash:
			case Ref:
			case Const:
			case Exact:
			case Left_Bracket:
			case Arrow_Right:
			// general:
			case Minus_Equal:
			case Plus_Equal:
			case Minus_Minus:
			case Plus_Plus:
			case Bar:
			case Bar_Bar:
			case Ampersand:
			case Ampersand_Ampersand:
			case Tilde:
			case Tilde_Tilde:
			case Caret:
			case Caret_Caret:
			case Percent:
			case Percent_Percent:
			case At: {
				// Attribute syntax: @name: or @name(args):  — only at statement start
				if (tokenType == At && parentNode->leafNodes.size() == 0 &&
					i + 1 < (int)tokens.size() && tokens[i + 1]->second == Identifier) {
					// Consume the attribute name
					tokenPair* nameTok = NEXT_TOKEN(tokens, i);
					node->nodeType = Attribute_Node;
					node->token = nameTok;
					// Optional argument list
					if (i + 1 < (int)tokens.size() && tokens[i + 1]->second == Left_Paren) {
						std::vector<tokenPair*> argTokens;
						GATHER_PAREN_EXPRESSION(tokens, argTokens, 0, i, false);
						ASTNode* argNode = generateAST(argTokens, depth + 1);
						argNode->nodeType = Scope_Body;
						argNode->codegen = &ASTNode::generateScopeBody;
						node->childNodes.push_back(argNode);
					}
					// Consume the required trailing colon
					if (i + 1 < (int)tokens.size() && tokens[i + 1]->second == Colon)
						i++;
					else {
						printTokenError(nameTok, "Expected ':' after attribute name");
						wasError = true;
						return nullptr;
					}
					pendingAttributes.push_back(node);
					goto dontAddNodeForce;
				}
				// Fall through to operator handling
				[[fallthrough]];
			}
			case At_At: {
				bool isUnaryR = false;	// Operates on right
				bool isUnaryL = false;	// Left
				bool noOp = false;
				bool isGeneralOperator = false;
				if (operatorDefaultNodeType.find(tokenType) != operatorDefaultNodeType.end()) {
					node->nodeType = operatorDefaultNodeType[tokenType];
				}
				else {
					isGeneralOperator = true;
					node->nodeType = Redefined_Operator_Expr;
				}

				ASTNode* firstTerm = new ASTNode();
				ASTNode* secondTerm = new ASTNode();
				bool isLeaf = true;
				bool isAccessOperation = node->nodeType == Access_Operation;

				// Instead of backtracking to get the first term, pop the leafNodes vector
				if (parentNode->leafNodes.size() == 0) {
					// if there are no leaf nodes, then assume this is a unary operator on R
					//printTokenError(token, "Binary operator expected left argument");
					//exit(1);
					isUnaryR = true;
					node->codegen = &ASTNode::generateUnaryExpression;
				}
				else {
					firstTerm = parentNode->leafNodes.back();
					parentNode->leafNodes.pop_back();
					node->codegen = &ASTNode::generateBinaryExpression;
				}

				if (isUnaryR && tokenType == Ampersand)
					node->nodeType = Address_Of_Operation;
				else if (isUnaryR && tokenType == Star)
					node->nodeType = Dereference_Operation;

				// Step through all following tokens until parens are closed
				int parenLevel = 1;
				int braceLevel = 1;
				int bracketLevel = 1;
				std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();
				if (!(isUnaryL == false && isUnaryR == false && isAccessOperation)) {
					isAccessOperation = false;
					for (;;) {
						if (i >= tokens.size() - 1)
							break;
						tokenPair* t = NEXT_TOKEN(tokens, i);

						if (t->second == Left_Paren)
							parenLevel++;
						if (t->second == Right_Paren)
							parenLevel--;
						if (t->second == Left_Brace)
							braceLevel++;
						if (t->second == Right_Brace)
							braceLevel--;
						if (t->second == Left_Bracket)
							bracketLevel++;
						if (t->second == Right_Bracket)
							bracketLevel--;

						if (parenLevel == 0 && braceLevel == 0 && bracketLevel == 0)
							break;
						if (parenLevel == 1 && braceLevel == 1 && bracketLevel == 1 && t->second == Equal) {
							i--;
							break;
						}
						if (t->second == EndOfLine || t->second == Semi_Colon) {
							isLeaf = false;
							break;
						}

						subTokens.push_back(t);
					}
				}
				// If an access operation with brackets like: arr[i]
				else if (isAccessOperation) {
					node->codegen = &ASTNode::generateAccessOperation;
					for (;;) {
						if (i >= tokens.size() - 1)
							break;
						tokenPair* t = NEXT_TOKEN(tokens, i);

						if (t->second == Left_Paren)
							parenLevel++;
						if (t->second == Right_Paren)
							parenLevel--;
						if (t->second == Left_Brace)
							braceLevel++;
						if (t->second == Right_Brace)
							braceLevel--;
						if (t->second == Left_Bracket)
							bracketLevel++;
						if (t->second == Right_Bracket)
							bracketLevel--;

						if (bracketLevel == 0)
							break;
						if (parenLevel == 1 && braceLevel == 1 && bracketLevel == 1 && t->second == Equal) {
							printTokenError(token, "Missing closing bracket");
							exit(1);
						}

						subTokens.push_back(t);
					}
				}
				if (node->nodeType == Member_Access)
					node->codegen = &ASTNode::generateMemberAccess;
				if (subTokens.size() == 0) {
					if (!isUnaryR && tokenType == Star) {
						node->nodeType = Pointer_Node;
						isUnaryL = true;
					}
					// No expression operator @ when used in conjunction with pipe operator
					else if (isUnaryR && tokenType == At) {
						node->nodeType = Pipe_Placeholder;
						node->codegen = &ASTNode::generatePipePlaceholder;
						noOp = true;
					}
					else {
						printTokenError(token, "Operator expected right argument");
						exit(1);
					}
				}
				else {
					ASTNode* secondAST = generateAST(subTokens, depth + 1);
					if (secondAST->childNodes.size() > 0)
						secondTerm = secondAST->childNodes[0];
					else {
						printTokenError(subTokens[0], "Unexpected expression");
						printAST(secondAST);
						exit(1);
					}
				}
				//secondTerm->nodeType = Expression_Term;

				if (!noOp) {
					if (!isUnaryR)
						node->childNodes.push_back(firstTerm);
					if (!isUnaryL)
						node->childNodes.push_back(secondTerm);
				}
				if (isLeaf)
					goto addNodeAsLeaf;
				break;
			}

			case Equal: {
				node->nodeType = Expression_Statement;
				node->codegen = &ASTNode::generateExpressionStatement;

				ASTNode* firstTerm = new ASTNode();
				ASTNode* secondTerm = new ASTNode();

				// Instead of backtracking to get the first term, pop the leafNodes vector =)
				firstTerm = parentNode->leafNodes.back();
				parentNode->leafNodes.pop_back();

				//firstTerm->lvalue = true;

				std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();

				// Step through all following tokens until end of term
				GATHER_TO_SEMICOLON(tokens, subTokens, i, false);
				//int parenLevel = 1;
				//for (;;) {
				//	if (i >= tokens.size() - 1)
				//		break;
				//	tokenPair t = NEXT_TOKEN(tokens, i);

				//	if (t->second == Left_Paren)
				//		parenLevel++;
				//	if (t->second == Right_Paren)
				//		parenLevel--;

				//	if ((t->second == EndOfLine || t->second == Semi_Colon))
				//		break;

				//	subTokens.push_back(t);
				//}
				secondTerm = generateAST(subTokens, depth + 1);
				secondTerm->nodeType = Expression_Term;
				secondTerm->codegen = &ASTNode::generateExpression;

				node->childNodes.push_back(firstTerm);
				node->childNodes.push_back(secondTerm);
				break;
			}

			case Hash: {
				node->nodeType = Compile_Time_Directive;

				ASTNode* identifier = new ASTNode();
				ASTNode* bodyNode = new ASTNode();
				bool isLeafNode = false;

				tokenPair* tt = NEXT_TOKEN(tokens, i);
				identifier->token = tt;
				//identifier->tokenType = tt->second;
				identifier->nodeType = Identifier_Node;

				// Paren-enclosed argument — function-call-style inline directive (e.g. #nameof(x)).
				// Gather only the paren contents and treat the whole directive as a value leaf,
				// so it composes with surrounding expressions without consuming them.
				if (i + 1 < (int)tokens.size() && tokens[i + 1]->second == Left_Paren) {
					std::vector<tokenPair*> argTokens;
					GATHER_PAREN_EXPRESSION(tokens, argTokens, 0, i, false);
					ASTNode* argNode = generateAST(argTokens, depth + 1);
					argNode->nodeType = Scope_Body;
					argNode->codegen = &ASTNode::generateScopeBody;
					node->token->first = "#" + identifier->token->first;
					node->childNodes.push_back(identifier);
					node->childNodes.push_back(argNode);
					goto addNodeAsLeaf;
				}

				// Only gather a body when the next token could plausibly start one
				// (identifier, literal, or opening brace).  Operator symbols,
				// commas, closing parens, and statement-enders mean the directive is
				// being used as a value expression (e.g. `#linenum > 0`) — consuming
				// further tokens would silently absorb the surrounding expression.
				if (i + 1 < (int)tokens.size()) {
					TokenType nextTok = tokens[i + 1]->second;
					bool couldStartBody =
						nextTok == Identifier || nextTok == Integer || nextTok == Float ||
						nextTok == String || nextTok == Character ||
						nextTok == Left_Brace;
					if (!couldStartBody) {
						node->token->first = "#" + identifier->token->first;
						node->childNodes.push_back(identifier);
						goto addNodeAsLeaf;	 // treat as expression leaf so operators can use it as left operand
					}
				}

				std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();

				// Step through all following tokens until end of line via semicolon
				isLeafNode = GATHER_TO_SEMICOLON(tokens, subTokens, i, true, true);
				//GATHER_TO_SEMICOLON_OR_OTHER(tokens, subTokens, i, Left_Brace, true);

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;
				bodyNode->codegen = &ASTNode::generateScopeBody;

				if (identifier->token->first == "cast") {
					node->codegen = &ASTNode::generateCast;
				}
				if (identifier->token->first == "extern") {
					setChildrenAsExtern(bodyNode);
					node->isExtern = true;
					identifier->codegen = &ASTNode::generateNothing;
					node->codegen = &ASTNode::generateScopeBody;
				}
				if (identifier->token->first == "new") {
					node->codegen = &ASTNode::generateTypeInstance;
				}
				if (identifier->token->first == "define") {
					node->codegen = &ASTNode::generateCompilerDefine;
					// Leaf nodes in a #define body are intentional tokens (name + value),
					// not parse errors — move them into childNodes so findUnusedLeafNodes
					// doesn't flag them, and collectTokens in generateCompilerDefine finds them.
					for (auto& l : bodyNode->leafNodes)
						bodyNode->childNodes.push_back(l);
					bodyNode->leafNodes.clear();
				}

				node->token->first = "#" + identifier->token->first;
				node->childNodes.push_back(identifier);
				node->childNodes.push_back(bodyNode);
				//if (isLeafNode)
				//	goto addNodeAsLeaf;
				break;
			}

			case Colon_Colon: {
				node->nodeType = Compiler_Define;

				ASTNode* identifier = new ASTNode();
				ASTNode* secondPart = new ASTNode();
				ASTNode* argumentsNode = new ASTNode();
				ASTNode* modifiersNode = new ASTNode();
				ASTNode* bodyNode = new ASTNode();
				std::vector<ASTNode*> arguments = std::vector<ASTNode*>();

				if (parentNode->leafNodes.size() > 0)
					identifier = parentNode->leafNodes.back();
				else {
					printTokenError(token, "Expected a leaf node, but none were found", __LINE__);
					exit(1);
				}
				parentNode->leafNodes.pop_back();
				identifier->nodeType = Identifier_Node;
				argumentsNode->nodeType = Arguments;
				modifiersNode->nodeType = Compiler_Modifiers;

				// Based on the next token, decide what this compiler define does
				// (if function, or if/for/while etc. or any line of code)
				bool nonFunction = false;
				bool lParenReached = false;
				for (int j = 0; j < tokens.size() - i; j++) {
					if (tokens[i + j]->second == Left_Paren)
						lParenReached = true;
					else if (tokens[i + j]->second == Left_Brace) {
						if (lParenReached) {  // If first ( then {, this is a function
							nonFunction = false;
							break;
						}
						else {
							nonFunction = true;
							break;
						}
					}
					// If semicolon, function prototype
					else if (tokens[i + j]->second == Semi_Colon) {
						nonFunction = false;
						break;
					}
				}

				// If the first token after :: is not a paren, or the token is in the map of non-function compiler defines
				tokenPair* tt = NEXT_TOKEN(tokens, i);
				if (nonFunction || compileTimeDefinable.find(tt->second) != compileTimeDefinable.end()) {
					i--;  // NEXT_TOKEN==tt starts on first token after :: <here>
					bool isCompileTimeDefinableKeyword = compileTimeDefinable.find(tt->second) != compileTimeDefinable.end();
					// Step through all following tokens until parens start OR braces start
					int tokenNum = 0;
					std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();
					for (;;) {
						if (i >= tokens.size() - 1) {
							printTokenError(tt, "Unmatched parenthesis", __LINE__);
							exit(1);
						}
						tokenPair* t = NEXT_TOKEN(tokens, i);

						if (t->second == Left_Brace) {
							i--;
							break;
						}

						subTokens.push_back(t);

						if (t->second == Left_Paren) {
							//i--;
							break;
						}
						if (tt->second != EndOfLine)
							tokenNum++;
					}

					// Step through all following tokens until braces start
					tokenNum = 0;
					for (;;) {
						if (i >= tokens.size() - 1) {
							break;
							printTokenError(tt, "Unmatched brace", __LINE__);
							exit(1);
						}
						tokenPair* t = NEXT_TOKEN(tokens, i);
						//if (tokenNum >= 2 && t->second != Left_Brace) {
						//	printTokenError(tt, "Unmatched brace");
						//	exit(1);
						//}

						if (t->second == Left_Brace) {
							break;
						}

						subTokens.push_back(t);
						if (tt->second != EndOfLine)
							tokenNum++;
					}
					i--;
					// Step through all tokens to gather body until braces are closed
					//GATHER_SCOPE_BODY_APPEND(tokens, subTokens, 0, i, true);
					GATHER_SCOPE_BODY_APPEND(tokens, subTokens, 0, i, isCompileTimeDefinableKeyword);

					bodyNode = generateAST(subTokens, depth + 1);
					bodyNode->nodeType = Scope_Body;
					bodyNode->codegen = &ASTNode::generateScopeBody;

					node->token = identifier->token;
					if (tt->second == Module_Define)
						node->isModuleScope = true;
					node->childNodes.push_back(bodyNode);

					// Check if extra type following :: but before {
					if (bodyNode->childNodes.size() > 0) {
						tokenPair* s = bodyNode->childNodes[0]->token;

						// Check for special definition types
						if (s->first == "struct") {
							tokenPair* structName = node->token;
							node = bodyNode->childNodes[0];
							node->token = structName;
							node->nodeType = Compiler_Define_Struct;
							node->codegen = &ASTNode::generateStruct;
						}
						else if (s->first == "for" || s->first == "while") {
							tokenPair* labelName = node->token;
							node = bodyNode;
							node->token = labelName;
							node->nodeType = Labeled_Loop;
							node->codegen = &ASTNode::generateLabeledLoop;
						}
					}
				}
				else {
					node->nodeType = Compiler_Define_Function;
					node->codegen = &ASTNode::generateFunction;
					i--;
					// Step through all following tokens until parens start
					std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();
					for (;;) {
						if (i >= tokens.size() - 1)
							break;
						tokenPair* t = NEXT_TOKEN(tokens, i);

						if (t->second == Left_Paren)
							break;

						subTokens.push_back(t);
					}
					secondPart = generateAST(subTokens, depth + 1);
					secondPart->nodeType = Type_Node;
					// Step through all following tokens until parens are closed
					int parenLevel = 1;
					subTokens = std::vector<tokenPair*>();
					for (;;) {
						if (i >= tokens.size() - 1)
							break;
						tokenPair* t = NEXT_TOKEN(tokens, i);

						if (t->second == Left_Paren)
							parenLevel++;
						if (t->second == Right_Paren)
							parenLevel--;

						if (parenLevel == 0 || t->second == EndOfLine || t->second == Semi_Colon)
							break;
						// If comma and parenLevel is in same scope
						if ((t->second == Comma && parenLevel == 1)) {
							ASTNode* newNode = new ASTNode();
							generateAST(subTokens, depth + 1, newNode);
							newNode->nodeType = Expression_Term;
							newNode->codegen = &ASTNode::generateExpression;
							arguments.push_back(newNode);
							subTokens = std::vector<tokenPair*>();
							continue;
						}

						subTokens.push_back(t);
					}
					ASTNode* newNode = new ASTNode();
					generateAST(subTokens, depth + 1, newNode);
					newNode->nodeType = Expression_Term;
					newNode->codegen = &ASTNode::generateExpression;
					arguments.push_back(newNode);
					subTokens = std::vector<tokenPair*>();
					//arguments = generateAST(subTokens, depth + 1);
					//arguments->nodeType = Arguments;
					for (int a = 0; a < arguments.size(); a++) {
						//argumentsNode->childNodes.push_back(arguments[a]);
						if (arguments[a]->leafNodes.size() == 2) {	// Make sure follows: <type> <identifier>  pattern
							arguments[a]->leafNodes[0]->nodeType = Type_Node;
							arguments[a]->childNodes.push_back(arguments[a]->leafNodes[0]);
							arguments[a]->childNodes.push_back(arguments[a]->leafNodes[1]);
							arguments[a]->leafNodes.pop_back();
							arguments[a]->leafNodes.pop_back();
						}
						else if (arguments[a]->leafNodes.size() == 1 && arguments[a]->leafNodes[0]->nodeType == Argument_List) {  // If ...  pattern
							arguments[a]->nodeType = Expression_Term;
							arguments[a]->codegen = &ASTNode::generateExpression;
							arguments[a]->childNodes = {arguments[a]->leafNodes[0]};
							arguments[a]->leafNodes.pop_back();
						}
						else if (arguments[a]->leafNodes.size() == 0) {
						}
						else {
							printTokenError(arguments[a]->leafNodes[0]->token, "Expected type followed by identifier");
							exit(1);
						}
						argumentsNode->childNodes.push_back(arguments[a]);
					}

					// Get modifiers between ) and {
					bool noModifiers = GATHER_TO_TOKEN(tokens, subTokens, 0, i, Left_Brace, true, true);
					i--;
					if (!noModifiers) {
						modifiersNode = generateAST(subTokens, depth + 1);
						modifiersNode->nodeType = Compiler_Modifiers;

						for (const auto& m : modifiersNode->childNodes) {
							if (m->token != nullptr)
								if (m->token->first == "#hideast")
									node->showInASTOutput = false;
						}
					}

					// Step through all tokens to gather body until braces are closed
					subTokens = std::vector<tokenPair*>();
					bool endedEarly = GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);

					if (!endedEarly) {
						bodyNode = generateAST(subTokens, depth + 1);
						bodyNode->nodeType = Scope_Body;
						bodyNode->codegen = &ASTNode::generateScopeBody;
					}
					else
						node->codegen = &ASTNode::generatePrototype;

					node->token = identifier->token;
					node->childNodes.push_back(identifier);
					node->childNodes.push_back(secondPart);
					node->childNodes.push_back(argumentsNode);
					node->childNodes.push_back(modifiersNode);
					if (!endedEarly)
						node->childNodes.push_back(bodyNode);

					// Check for special function types
					if (identifier->token->first == "cast") {
						if (secondPart->childNodes.size() > 0)
							node->token = secondPart->childNodes[0]->token;
						else {
							printTokenError(secondPart->token, "Cast function must have a return type");
							exit(1);
						}
						node->nodeType = Compiler_Define_Cast;
					}
					else if (identifier->token->first == "create") {
						if (secondPart->childNodes.size() > 0)
							node->token = secondPart->childNodes[0]->token;
						else {
							printTokenError(secondPart->token, "Struct initializer function must have a return type");
							exit(1);
						}
						node->nodeType = Compiler_Define_Function;
					}
				}
				break;
			}

			case Operator_Keyword: {
				node->nodeType = Operator_Overload_Node;

				// Get next token, which should be the operator
				tokenPair* t = NEXT_TOKEN(tokens, i);

				ASTNode* operatorNode = new ASTNode(Operator_Type_Node, {}, t);

				node->token->first = node->token->first + "." + tokenAsString(t->second);

				node->childNodes.push_back(operatorNode);
				goto addNodeAsLeaf;
			}

			case Left_Paren: {
				node->nodeType = Expression_Paren_Term;
				node->codegen = &ASTNode::generateExpression;

				ASTNode* previousTerm = new ASTNode();
				std::vector<ASTNode*> insideNodes = std::vector<ASTNode*>();
				bool isFunction = false;
				bool isLeaf = true;

				// Check the previous node, if it is an identifier then this must be a function call
				if (parentNode->leafNodes.size() > 0) {
					previousTerm = parentNode->leafNodes.back();
					parentNode->leafNodes.pop_back();
					if (previousTerm->nodeType == Identifier_Node) {
						node->nodeType = Function_Call;
						node->codegen = &ASTNode::generateCallExpression;
						node->token = previousTerm->token;
						isFunction = true;
					}
				}

				// Collect all comma-separated terms
				// Step through all following tokens until last paren
				int parenLevel = 1;
				std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();
				for (;;) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair* t = NEXT_TOKEN(tokens, i);

					if (t->second == Left_Paren)
						parenLevel++;
					if (t->second == Right_Paren)
						parenLevel--;

					if (parenLevel == 0)
						break;
					if (t->second == EndOfLine || t->second == Semi_Colon) {
						isLeaf = false;
						break;
					}
					// If comma and parenLevel is in same scope
					if ((t->second == Comma && parenLevel == 1)) {
						ASTNode* newNode = new ASTNode();
						generateAST(subTokens, depth + 1, newNode);
						newNode->nodeType = Expression_Term;
						newNode->codegen = &ASTNode::generateExpression;
						insideNodes.push_back(newNode);
						subTokens = std::vector<tokenPair*>();
						continue;
					}

					subTokens.push_back(t);
				}
				ASTNode* newNode = new ASTNode();
				generateAST(subTokens, depth + 1, newNode);
				newNode->nodeType = Expression_Term;
				newNode->codegen = &ASTNode::generateExpression;
				insideNodes.push_back(newNode);

				if (isFunction) {
					ASTNode* argumentsNode = new ASTNode();
					argumentsNode->nodeType = Arguments;
					for (int a = 0; a < insideNodes.size(); a++) {
						argumentsNode->childNodes.push_back(insideNodes[a]);
					}
					node->childNodes.push_back(argumentsNode);
				}
				else {
					insideNodes[0]->nodeType = Expression_Paren_Term;
					insideNodes[0]->codegen = &ASTNode::generateExpression;
					node = insideNodes[0];
					//node->childNodes.push_back(insideNodes[0]);
				}
				if (isLeaf)
					goto addNodeAsLeaf;
				break;
			}

			case Right_Brace: {
				printTokenError(token, "Unmatched brace", __LINE__, __FILE__);
				exit(1);
				break;
			}

			case Dot_Dot_Dot: {
				node->nodeType = Argument_List;
				parentNode->leafNodes.push_back(node);
				break;
			}

			case Void: {
				node->nodeType = Void_Node;
				node->codegen = &ASTNode::generateConstant;
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case Integer: {
				node->nodeType = Integer_Node;
				node->codegen = &ASTNode::generateConstant;
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case Float: {
				node->nodeType = Float_Node;
				node->codegen = &ASTNode::generateConstant;
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case String: {
				node->nodeType = String_Constant_Node;
				node->codegen = &ASTNode::generateConstant;
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case Character: {
				node->nodeType = Character_Constant_Node;
				node->codegen = &ASTNode::generateConstant;
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case True_Literal:
			case False_Literal: {
				node->nodeType = Boolean_Node;
				node->codegen = &ASTNode::generateConstant;
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case Comment: {
				int commentLine = token->lineNumber;
				// Gap since last comment — reset block
				if (commentLine > pendingCommentLastLine + 1)
					pendingCommentText.clear();
				// Strip // or /* */ markers
				std::string raw = token->first;
				std::string text;
				if (raw.size() >= 2 && raw[0] == '/' && raw[1] == '/') {
					text = raw.substr(2);
					while (!text.empty() && text.back() == ' ') text.pop_back();
				} else if (raw.size() >= 2 && raw[0] == '/' && raw[1] == '*') {
					text = raw.substr(2);
					size_t endPos = text.rfind("*/");
					if (endPos != std::string::npos) text = text.substr(0, endPos);
					while (!text.empty() && text.back() == ' ') text.pop_back();
				} else {
					text = raw;
				}
				if (!pendingCommentText.empty())
					pendingCommentText += "\n" + text;
				else
					pendingCommentText = text;
				pendingCommentLastLine = commentLine;
				goto dontAddNodeForce;
			}

			case Identifier: {
				node->nodeType = Identifier_Node;
				node->codegen = &ASTNode::generateVariableExpression;

				//// If this is identifer, and is preceded by identifier, then that one is type, and this is name
				//if (parentNode->leafNodes.size() >= 1) {
				//	if (parentNode->leafNodes.back()->nodeType == Identifier_Node) {
				//		ASTNode* typeNode = parentNode->leafNodes.back();
				//		parentNode->leafNodes.pop_back();
				//		typeNode->nodeType = Type_Node;
				//		typeNode->codegen = nullptr;
				//		node->childNodes.push_back(typeNode);
				//	}
				//}
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case Throw_Statement:
				node->nodeType = Throw_Node;
				node->codegen = &ASTNode::generateThrow;
				goto getStatementArgument;
			case Return_Statement:
				node->nodeType = Return_Node;
				node->codegen = &ASTNode::generateReturn;
				goto getStatementArgument;
			case Break_Statement:
				node->nodeType = Break_Node;
				node->codegen = &ASTNode::generateBreak;
				goto getStatementArgument;
			case Continue_Statement:
				node->nodeType = Continue_Node;
				node->codegen = &ASTNode::generateContinue;
				goto getStatementArgument;
			case Goto_Statement: {
				node->nodeType = Goto_Node;
			getStatementArgument:

				ASTNode* argumentTerm = new ASTNode();
				std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();

				// Step through all following tokens until end of term
				GATHER_TO_SEMICOLON(tokens, subTokens, i, true);
				//for (;;) {
				//	if (i >= tokens.size() - 1)
				//		break;
				//	tokenPair t = NEXT_TOKEN(tokens, i);

				//	if (t->second == Left_Paren)
				//		parenLevel++;
				//	if (t->second == Right_Paren)
				//		parenLevel--;

				//	if ((t->second == EndOfLine || t->second == Semi_Colon))
				//		break;

				//	subTokens.push_back(t);
				//}
				argumentTerm = generateAST(subTokens, depth + 1);
				argumentTerm->nodeType = Expression_Term;
				argumentTerm->codegen = &ASTNode::generateExpression;

				node->childNodes.push_back(argumentTerm);
				break;
			}

			case Semi_Colon: {
				for (int l = 0; l < parentNode->leafNodes.size(); l++) {
					parentNode->childNodes.push_back(parentNode->leafNodes[l]);
				}
				for (int l = 0; l < parentNode->leafNodes.size(); l++)
					parentNode->leafNodes.pop_back();
				goto dontAddNodeForce;
			}

			case Left_Brace: {
				std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();

				// Step through all tokens to gather scope body (excluding braces) until braces are closed
				GATHER_SCOPE_BODY(tokens, subTokens, 1, i, false);

				// Pass end brace
				i++;

				node = generateAST(subTokens, depth + 1);
				node->nodeType = Scope_Body;
				node->codegen = &ASTNode::generateScopeBody;
				break;
			}

			case EndOfFile:
			case EndOfLine:
			case Nothing: {
				goto dontAddNodeForce;
			}

			default: {
				if (verbosity >= 4)
					printTokenWarning(token, "Undefined node, token type: \"" + tokenAsString(tokenType) + "\"");
				goto dontAddNodeForce;
			}
		}


	addNode:
		if (!pendingCommentText.empty()) {
			if (node->lineNumber <= pendingCommentLastLine + 1) {
				ASTNode* commentNode = new ASTNode();
				commentNode->nodeType = Comment_Node;
				commentNode->token = new tokenPair(toCStringLiteral(pendingCommentText), Comment);
				node->childNodes.insert(node->childNodes.begin(), commentNode);
			}
			pendingCommentText.clear();
		}
		for (auto& a : pendingAttributes)
			node->attributes.push_back(a);
		pendingAttributes.clear();
		parentNode->childNodes.push_back(node);
		continue;

	addNodeAsLeaf:
		parentNode->leafNodes.push_back(node);
		continue;

	dontAddNode:
		//// If it has a parent waiting for results, dont skip adding node
		//if (hasParent) {
		//	leafNodes.pop();
		//	printf("removed\n");
		//	goto addNode;
		//}
	dontAddNodeForce:
		continue;
	}

	// Warn about any attributes that were never attached to a node
	for (auto& a : pendingAttributes) {
		printTokenWarning(a->token, "Attribute '@" + a->token->first + "' has nothing to attach to");
	}

	// If only one leaf node and no child nodes, it can be added as child instead
	if (parentNode->leafNodes.size() == 1 && parentNode->childNodes.size() == 0) {
		parentNode->childNodes.push_back(parentNode->leafNodes.back());
		parentNode->leafNodes.pop_back();
	}


	return parentNode;
}


void fixPrecedence(ASTNode*& node)
{
	if (!node)
		return;

	// Recurse first on children
	for (auto& child : node->childNodes)
		fixPrecedence(child);

	// Handle only binary ops with two children
	if (operatorPrecedence.find(node->nodeType) != operatorPrecedence.end()) {
		if (node->childNodes.size() != 2)
			return;
		ASTNode* leftChild = node->childNodes[0];
		ASTNode* rightChild = node->childNodes[1];

		// Special handling: force left-associativity for these ops
		if (leftAssociativeOperators.count(node->nodeType)) {
			// If right child is same op: rotate left so (a op (b op c)) -> ((a op b) op c)
			if (rightChild->nodeType == node->nodeType && rightChild->childNodes.size() == 2) {
				ASTNode* A = leftChild;
				ASTNode* B = rightChild->childNodes[0];
				ASTNode* C = rightChild->childNodes[1];

				// New left: (A op B)
				ASTNode* newLeft = new ASTNode();
				*newLeft = *node;  // Copy node info (type, codegen, etc.)
				newLeft->childNodes.clear();
				newLeft->childNodes.push_back(A);
				newLeft->childNodes.push_back(B);

				// Rebuild this node as: (A op B) op C
				node->childNodes.clear();
				node->childNodes.push_back(newLeft);
				node->childNodes.push_back(C);

				fixPrecedence(newLeft);
			}
			//return;	 // Finished for left-associative
		}

		// Usual precedence fix: check left, then right, just as before
		if (leftChild->childNodes.size() == 2 &&
			operatorPrecedence.find(leftChild->nodeType) != operatorPrecedence.end()) {
			int currPrec = operatorPrecedence[node->nodeType];
			int leftPrec = operatorPrecedence[leftChild->nodeType];
			// Rotate right for tighter right
			if (currPrec > leftPrec) {
				ASTNode* A = leftChild->childNodes[0];
				ASTNode* B = leftChild->childNodes[1];
				ASTNode* C = rightChild;

				ASTNode* newRight = new ASTNode();
				*newRight = *node;
				newRight->childNodes.clear();
				newRight->childNodes.push_back(B);
				newRight->childNodes.push_back(C);

				node->nodeType = leftChild->nodeType;
				node->codegen = leftChild->codegen;
				node->token = leftChild->token;
				node->childNodes.clear();
				node->childNodes.push_back(A);
				node->childNodes.push_back(newRight);

				fixPrecedence(newRight);
			}
		}
		if (rightChild->childNodes.size() == 2 &&
			operatorPrecedence.find(rightChild->nodeType) != operatorPrecedence.end()) {
			int currPrec = operatorPrecedence[node->nodeType];
			int rightPrec = operatorPrecedence[rightChild->nodeType];
			// Rotate left for tighter left
			if (currPrec > rightPrec) {
				ASTNode* A = leftChild;
				ASTNode* B = rightChild->childNodes[0];
				ASTNode* C = rightChild->childNodes[1];

				ASTNode* newLeft = new ASTNode();
				*newLeft = *node;
				newLeft->childNodes.clear();
				newLeft->childNodes.push_back(A);
				newLeft->childNodes.push_back(B);

				node->nodeType = rightChild->nodeType;
				node->codegen = rightChild->codegen;
				node->token = rightChild->token;
				node->childNodes.clear();
				node->childNodes.push_back(newLeft);
				node->childNodes.push_back(C);

				fixPrecedence(newLeft);
			}
		}
	}
}


void unifyNodes(ASTNode*& node)
{
	if (node->parentNode != nullptr) {
		// If the parent only has one child (this) and is the same type, make them the same
		ASTNode* oldP = node->parentNode;
		if (oldP->childNodes.size() == 1 && node->nodeType == oldP->nodeType) {
			ASTNode* p = node->parentNode->parentNode;
			node->parentNode = p;
			// Set oldP in new parents childnodes to this
			if (p != nullptr)
				for (auto& n : p->childNodes)
					if (n == oldP)
						n = node;
		}
	}

	for (int i = 0; i < node->childNodes.size(); i++) {
		unifyNodes(node->childNodes[i]);
	}
}


void resolveCompileTimeDirectives(ASTNode*& node, std::string moduleCtx, std::string funcCtx)
{
	// Propagate context downward: update for children before recursing
	std::string childModuleCtx = moduleCtx;
	std::string childFuncCtx = funcCtx;
	if (node->nodeType == Compiler_Define && node->isModuleScope)
		childModuleCtx = node->token->first;
	else if (!node->enclosingModule.empty())
		childModuleCtx = node->enclosingModule;
	if (node->nodeType == Compiler_Define_Function)
		childFuncCtx = node->token->first;

	for (int i = 0; i < node->childNodes.size(); i++)
		resolveCompileTimeDirectives(node->childNodes[i], childModuleCtx, childFuncCtx);

	if (node->nodeType == Compile_Time_Directive && node->childNodes.size() > 0) {
		const std::string& name = node->childNodes[0]->token->first;
		if (name == "linenum") {
			node->nodeType = Integer_Node;
			node->token->first = std::to_string(node->token->lineNumber);
			node->codegen = &ASTNode::generateConstant;
			node->childNodes.clear();
		}
		else if (name == "line") {
			std::string lineStr = node->token->lineValue ? *node->token->lineValue : "";
			node->nodeType = String_Constant_Node;
			node->token->first = "\"" + lineStr + "\"";
			node->codegen = &ASTNode::generateConstant;
			node->childNodes.clear();
		}
		else if (name == "filename") {
			std::string filePath = node->token->filePath ? *node->token->filePath : "";
			node->nodeType = String_Constant_Node;
			node->token->first = "\"" + filePath + "\"";
			node->codegen = &ASTNode::generateConstant;
			node->childNodes.clear();
		}
		else if (name == "linecol") {
			node->nodeType = Integer_Node;
			node->token->first = std::to_string(node->token->indexInLine);
			node->codegen = &ASTNode::generateConstant;
			node->childNodes.clear();
		}
		else if (name == "funcname") {
			if (funcCtx.empty()) {
				printTokenError(node->token, "#funcname used outside of a function");
				exit(1);
			}
			node->nodeType = String_Constant_Node;
			node->token->first = "\"" + funcCtx + "\"";
			node->codegen = &ASTNode::generateConstant;
			node->childNodes.clear();
		}
		else if (name == "modulename") {
			if (moduleCtx.empty()) {
				printTokenError(node->token, "#modulename used outside of a module");
				exit(1);
			}
			node->nodeType = String_Constant_Node;
			node->token->first = "\"" + moduleCtx + "\"";
			node->codegen = &ASTNode::generateConstant;
			node->childNodes.clear();
		}
		else if (name == "asaversion") {
			node->nodeType = String_Constant_Node;
			node->token->first = "\"" VERSION "\"";
			node->codegen = &ASTNode::generateConstant;
			node->childNodes.clear();
		}
		else if (name == "counter") {
			static int counterValue = 0;
			node->nodeType = Integer_Node;
			node->token->first = std::to_string(counterValue++);
			node->codegen = &ASTNode::generateConstant;
			node->childNodes.clear();
		}
		else if (name == "typeof") {
			node->codegen = &ASTNode::generateTypeofDirective;
		}
		else if (name == "sizeof") {
			node->codegen = &ASTNode::generateSizeofDirective;
		}
		else if (name == "nameof") {
			if (node->childNodes.size() < 2 || node->childNodes[1]->childNodes.empty()) {
				printTokenError(node->token, "#nameof requires an expression argument");
				exit(1);
			}
			// Walk to the rightmost leaf to get the simple name (e.g. A.B.C → "C")
			ASTNode* cur = node->childNodes[1]->childNodes[0];
			while (cur->childNodes.size() >= 2)
				cur = cur->childNodes.back();
			node->nodeType = String_Constant_Node;
			node->token->first = "\"" + cur->token->first + "\"";
			node->codegen = &ASTNode::generateConstant;
			node->childNodes.clear();
		}
	}
}

void optimizeASTNode(ASTNode*& node)
{
	for (int i = 0; i < node->childNodes.size(); i++) {
		optimizeASTNode(node->childNodes[i]);
	}

	// If binary operator
	if (node->childNodes.size() >= 2) {
		if (operatorPrecedence.find(node->nodeType) != operatorPrecedence.end()) {
			ASTNode* first = node->childNodes[0];
			ASTNode* second = node->childNodes[1];

			bool first_b = false;
			int first_i = 0;
			double first_f = 0;
			void* first_val_ptr = &first_b;
			ASTNodeType first_type = Boolean_Node;

			bool second_b = false;
			int second_i = 0;
			double second_f = 0;
			void* second_val_ptr = &second_b;
			ASTNodeType second_type = Boolean_Node;

			bool output_b = false;
			int output_i = 0;
			double output_f = 0;
			void* output_val_ptr = &output_b;
			ASTNodeType output_type = Boolean_Node;

			bool firstIsLiteral = false;
			bool secondIsLiteral = false;

			// If the first node is a literal
			if (literals.find(first->nodeType) != literals.end()) {
				firstIsLiteral = true;
				switch (first->nodeType) {
					case Boolean_Node:
						first_val_ptr = &first_b;
						first_b = first->token->first == "true" ? true : false;
						output_val_ptr = &output_b;
						output_type = Boolean_Node;
						first_type = Boolean_Node;
						break;
					case Integer_Node:
						first_val_ptr = &first_i;
						first_i = std::stoi(first->token->first);
						output_val_ptr = &output_i;
						output_type = Integer_Node;
						first_type = Integer_Node;
						break;
					case Float_Node:
						first_val_ptr = &first_f;
						first_f = std::stod(first->token->first);
						output_val_ptr = &output_f;
						output_type = Float_Node;
						first_type = Float_Node;
						break;
					default:
						firstIsLiteral = false;
				}
			}
			// If the second node is a literal
			if (literals.find(second->nodeType) != literals.end()) {
				secondIsLiteral = true;
				switch (second->nodeType) {
					case Boolean_Node:
						second_val_ptr = &second_b;
						second_b = second->token->first == "true" ? true : false;
						second_type = Boolean_Node;
						break;
					case Integer_Node:
						second_val_ptr = &second_i;
						second_i = std::stoi(second->token->first);
						if (output_type != Float_Node) {
							output_val_ptr = &output_i;
							output_type = Integer_Node;
						}
						second_type = Integer_Node;
						break;
					case Float_Node:
						second_val_ptr = &second_f;
						second_f = std::stod(second->token->first);
						output_val_ptr = &output_f;
						output_type = Float_Node;
						second_type = Float_Node;
						break;
					default:
						secondIsLiteral = false;
				}
			}

			if (output_type == Float_Node) {
				if (first_type == Boolean_Node) {
					first_type = Float_Node;
					first_f = (double)first_b;
					first_val_ptr = &first_f;
				}
				if (first_type == Integer_Node) {
					first_type = Float_Node;
					first_f = (double)first_i;
					first_val_ptr = &first_f;
				}
				if (second_type == Boolean_Node) {
					second_type = Float_Node;
					second_f = (double)second_b;
					second_val_ptr = &second_f;
				}
				if (second_type == Integer_Node) {
					second_type = Float_Node;
					second_f = (double)second_i;
					second_val_ptr = &second_f;
				}
			}
			else if (output_type == Integer_Node) {
				if (first_type == Boolean_Node) {
					first_type = Integer_Node;
					first_i = (int)first_b;
					first_val_ptr = &first_i;
				}
				if (second_type == Boolean_Node) {
					second_type = Integer_Node;
					second_i = (int)second_b;
					second_val_ptr = &second_i;
				}
			}

			if (firstIsLiteral && secondIsLiteral) {

				std::string outString;

				// If float output
				if (output_type == Float_Node) {
					switch (node->nodeType) {
						case (Expression_Times):
							output_f = (*(double*)first_val_ptr) * (*(double*)second_val_ptr);
							break;
						case (Expression_Divide):
							output_f = (*(double*)first_val_ptr) / (*(double*)second_val_ptr);
							break;
						case (Expression_Plus):
							output_f = (*(double*)first_val_ptr) + (*(double*)second_val_ptr);
							break;
						case (Expression_Minus):
							output_f = (*(double*)first_val_ptr) - (*(double*)second_val_ptr);
							break;
					}
					outString = std::to_string(output_f);
				}
				// If int output
				else if (output_type == Integer_Node) {
					switch (node->nodeType) {
						case (Expression_Times):
							output_i = (*(int*)first_val_ptr) * (*(int*)second_val_ptr);
							break;
						case (Expression_Divide):
							output_i = (*(int*)first_val_ptr) / (*(int*)second_val_ptr);
							break;
						case (Expression_Plus):
							output_i = (*(int*)first_val_ptr) + (*(int*)second_val_ptr);
							break;
						case (Expression_Minus):
							output_i = (*(int*)first_val_ptr) - (*(int*)second_val_ptr);
							break;
					}
					outString = std::to_string(output_i);
				}


				node->childNodes = std::vector<ASTNode*>();
				node->nodeType = output_type;
				switch (output_type) {
					case Integer_Node:
					case Boolean_Node:
						node->token->second = Integer;
						break;
					case Float_Node:
						node->token->second = Float;
						break;
					default:
						break;
				}
				node->token->first = outString;
			}
		}
	}
	// Else if a paren term
	else if (node->nodeType == Expression_Paren_Term) {
		// If this paren term only has one child, and that child is any type of literal,
		// the paren can be removed
		if (node->childNodes.size() == 1 && literals.find(node->childNodes[0]->nodeType) != literals.end()) {
			node = node->childNodes[0];
		}
	}
	// Else if it is an expression term
	else if (node->nodeType == Expression_Term) {
		// If there are no children, this expression can be removed
	}
}

void addFileIncludes(ASTNode*& node)
{
	for (int i = 0; i < node->childNodes.size(); i++)
		addFileIncludes(node->childNodes[i]);

	if (node->childNodes.size() > 0)
		if (node->nodeType == Compile_Time_Directive) {
			if (node->childNodes[0]->token->first == "file") {
				std::string fileString = "";
				std::string fileName = "";

				// Load file if provided
				if (node->childNodes.size() > 1) {
					ASTNode* strChild = node->childNodes[1]->childNodes[0];
					if (strChild->nodeType == String_Node) {
						fileName = strChild->token->first.substr(1, strChild->token->first.size() - 2);

						fileName = std::filesystem::weakly_canonical(std::filesystem::path(projectDirectory + fileName)).string();

						if (importedFileNames.find(fileName) != importedFileNames.end()) {
							node->nodeType = Nothing_Node;
							return;
						}

						int e = loadFile(fileName, fileString);
						if (e != 0) {
							printTokenError(strChild->token, "Failed to include file from given path", __LINE__);
							exit(1);
						}

						importedFileNames.insert(fileName);
					}
					else {
						printTokenError(strChild->token, "Expected string literal", __LINE__);
						exit(1);
					}
				}
				else {
					printTokenError(node->childNodes[0]->token, "Expected string literal", __LINE__);
					exit(1);
				}

				std::vector<tokenPair*> localTokens = std::vector<tokenPair*>();

				// Begin tokenizing file
				int e = tokenize(fileString, localTokens, fileName);
				if (e != 0) {
					std::cerr << "Invalid tokens met\n";
					exit(1);
				}
				// Now change any tokens to their subtoken type if applicable
				e = labelSubTokens(localTokens);
				if (e != 0) {
					std::cerr << "Invalid tokens met\n";
					exit(1);
				}
				e = joinCommentTokens(localTokens);
				if (e != 0) {
					std::cerr << "Invalid tokens met\n";
					exit(1);
				}

				// Generate AST
				ASTNode* localRoot = generateAST(localTokens);
				stripCommentNodes(localRoot);
				for (int i = 0; i < localRoot->childNodes.size(); i++) {
					importedNodes.push_back(localRoot->childNodes[i]);
					//rootNode->childNodes.push_back(localRoot->childNodes[i]);
				}


				node->nodeType = Nothing_Node;
			}
		}
}

bool loadModule(std::string& modulePath, std::string& moduleName)
{
	for (const auto& p : std::filesystem::directory_iterator(modulePath)) {
		std::string outStr = "";
		std::string pathStr = p.path();
		loadFile(pathStr, outStr);

		std::vector<tokenPair*> localTokens = std::vector<tokenPair*>();

		// Begin tokenizing file
		int e = tokenize(outStr, localTokens, pathStr);
		if (e != 0) {
			std::cerr << "Invalid tokens met\n";
			exit(1);
		}
		// Now change any tokens to their subtoken type if applicable
		e = labelSubTokens(localTokens);
		if (e != 0) {
			std::cerr << "Invalid tokens met\n";
			exit(1);
		}
		e = joinCommentTokens(localTokens);
		if (e != 0) {
			std::cerr << "Invalid tokens met\n";
			exit(1);
		}

		// Generate AST
		ASTNode* localRoot = generateAST(localTokens);
		stripCommentNodes(localRoot);

		// Look through file to see if it contains the desired module
		for (int j = 0; j < localRoot->childNodes.size(); j++) {
			if (localRoot->childNodes[j]->nodeType == Compiler_Define)
				if (localRoot->childNodes[j]->childNodes.size() >= 1 &&
					localRoot->childNodes[j]->childNodes[0]->childNodes.size() >= 1 &&
					localRoot->childNodes[j]->childNodes[0]->childNodes[0]->nodeType == Module_Define_Node) {

					ASTNode* moduleNode = localRoot->childNodes[j]->childNodes[0]->childNodes[0];
					if (localRoot->childNodes[j]->token->first == moduleName) {
						for (int i = 0; i < moduleNode->childNodes[0]->childNodes.size(); i++) {
							ASTNode* importedNode = moduleNode->childNodes[0]->childNodes[i];
							if (importedNode->enclosingModule.empty())
								importedNode->enclosingModule = moduleName;
							importedNodes.push_back(importedNode);
							//rootNode->childNodes.push_back(localRoot->childNodes[i]);
						}
						if (verbosity >= 3)
							printModuleLoaded(moduleName, pathStr);
						return true;
					}
				}
		}
	}
	return false;
}

void getModuleNameAndPath(ASTNode*& node, std::string& modulePath, std::string& moduleName)
{
	if (node->nodeType == Member_Access) {
		// Right-associative: A.B.C -> Member_Access(A, Member_Access(B, C))
		modulePath = node->childNodes[0]->token->first;
		ASTNode* rest = node->childNodes[1];
		while (rest->nodeType == Member_Access) {
			modulePath += "/" + rest->childNodes[0]->token->first;
			rest = rest->childNodes[1];
		}
		moduleName = rest->token->first;
	}
	else if (node->childNodes.size() == 2) {
		ASTNode* firstExpression = node->childNodes[0];
		std::string tmpPath = "";
		getModuleNameAndPath(firstExpression, tmpPath, moduleName);
		modulePath = tmpPath + "/" + modulePath;

		ASTNode* secondExpression = node->childNodes[1];
		moduleName = secondExpression->token->first;
	}
	else if (node->childNodes.size() == 0)
		modulePath = node->token->first;
	else {
		printTokenError(node->token, "Invalid module name expression");
		wasError = true;
		return;
	}
}

void addModuleImports(ASTNode*& node)
{
	for (int i = 0; i < node->childNodes.size(); i++)
		addModuleImports(node->childNodes[i]);

	if (node->childNodes.size() > 0)
		if (node->nodeType == Compile_Time_Directive) {
			if (node->childNodes[0]->token->first == "import") {
				std::string fileString = "";

				// Load module if module name is provided
				if (node->childNodes.size() > 1) {
					// Get node within the import scope body
					ASTNode* moduleNameNode = node->childNodes[1]->childNodes[0];
					if (moduleNameNode->nodeType == Identifier_Node || moduleNameNode->nodeType == Member_Access) {
						std::string modulePath = ".";
						std::string moduleName = "";
						bool moduleFound = false;

						getModuleNameAndPath(moduleNameNode, modulePath, moduleName);
						modulePath = std::filesystem::path(modulePath).lexically_normal().string();

						if (importedModuleNames.find(moduleName) != importedModuleNames.end()) {
							node->nodeType = Nothing_Node;
							node->showInASTOutput = false;
							return;
						}

						std::string searchPath[2] = {projectDirectory + modulePath, executableDirectory + "modules/" + modulePath};
						for (int i = 0; i < sizeof(searchPath) / sizeof(searchPath[0]); i++) {
							if (directoryExists(searchPath[i])) {
								moduleFound = loadModule(searchPath[i], moduleName);
								break;
							}
						}

						if (!moduleFound) {
							printTokenError(moduleNameNode->token, "Failed to import module with name: \"" + moduleName + "\" and expected path: \"" + modulePath + "\", not found", __LINE__);
							console::WriteLine("Looked in the following directories:", console::yellowFGColor);
							console::indentation++;
							for (int i = 0; i < sizeof(searchPath) / sizeof(searchPath[0]); i++)
								console::WriteLine(searchPath[i], console::redFGColor);
							console::indentation--;
							if (verbosity >= 5) {
								printAST(node);
							}
							exit(1);
						}

						importedModuleNames.insert(moduleName);
					}
					//else if (moduleNameNode->nodeType == Colon_Separator_Node) {
					//	bool moduleFound = false;
					//	std::string moduleName = moduleNameNode->token->first;
					//	ASTNode* secondExpression = moduleNameNode->childNodes[0];
					//	// Get sub components
					//	while (secondExpression->childNodes.size() > 0) {
					//		modulePath += "/" + secondExpression->token->first;
					//		secondExpression = secondExpression->childNodes[0];
					//	}

					//	if (importedModuleNames.find(moduleName) != importedModuleNames.end()) {
					//		node->nodeType = Nothing_Node;
					//		return;
					//	}

					//	std::string searchPath[2] = {projectDirectory, executableDirectory + "modules/"};
					//	if (directoryExists(searchPath[0]))
					//		moduleFound = loadModule(searchPath[0], moduleName);
					//	if (!moduleFound && directoryExists(searchPath[1]))
					//		moduleFound = loadModule(searchPath[1], moduleName);

					//	importedModuleNames.insert(moduleName);

					//	if (!moduleFound) {
					//		printTokenError(moduleNameNode->token, "Failed to import module, not found", __LINE__);
					//		exit(1);
					//	}
					//}
					else {
						printTokenError(moduleNameNode->token, "Expected module name", __LINE__);
						printAST(node);
						exit(1);
					}
				}
				else {
					printTokenError(node->childNodes[0]->token, "Expected module name", __LINE__);
					printAST(node);
					exit(1);
				}

				node->nodeType = Nothing_Node;
			}
		}
}

void assignParentNodes(ASTNode*& node, int depth)
{
	for (int i = 0; i < node->childNodes.size(); i++) {
		node->childNodes[i]->parentNode = node;
		node->childNodes[i]->depth = depth + 1;
		assignParentNodes(node->childNodes[i], depth + 1);
	}
}


//void GO_BACK_TO_BEGINNING_OF_TERM(int& i)
//{
//	int PAREN_LEVEL = 1;
//	for (;;) {
//		tokenPair TOKENPAIR = tokens[--i];
//		if (TOKENPAIR.second == Left_Paren || TOKENPAIR.second == Left_Bracket)
//			PAREN_LEVEL--;
//		if (TOKENPAIR.second == Right_Paren || TOKENPAIR.second == Right_Bracket)
//			PAREN_LEVEL++;
//		if (PAREN_LEVEL == 0 || TOKENPAIR.second == EndOfLine || TOKENPAIR.second == Equal)
//			break;
//	}
//}

//std::vector<tokenPair*> GATHER_SCOPE_BODY(int brLevel, int& i)
//{
//	int braceLevel = brLevel;
//	std::vector<tokenPair*> subTokens = std::vector<tokenPair*>();
//	for (;;) {
//		tokenPair t = NEXT_TOKEN(i);
//
//		if (t->second == Left_Brace)
//			braceLevel++;
//		if (t->second == Right_Brace)
//			braceLevel--;
//
//		subTokens.push_back(t);
//
//		if (braceLevel == 0 || t->second == EndOfFile)
//			break;
//	}
//	return subTokens;
//}

const std::string ASTNodeTypeAsString(ASTNodeType t)
{
	if (t < LastASTNodeType)
		return ASTNodeTypeStrings[t];
	else {
		printf("Error: Undefined AST Node type `%d`", t);
		return "UNDEFINED AST NODE TYPE";
	}
}

int printAST(ASTNode* startNode, int depth)
{
	// Print each node

	console::printIndent(depth);

	if (startNode->token != nullptr) {
		console::Write(startNode->token->first, console::greenFGColor);
		if (verbosity >= 5)
			if (startNode->token->first.size() > 0)
				printf(":L%d", startNode->lineNumber);
	}
	if (startNode->showInASTOutput) {
		console::Write(":(");
		console::Write(ASTNodeTypeAsString(startNode->nodeType), console::yellowFGColor);
		console::Write("){");
	}
	else {
		console::Write(":(");
		console::Write(ASTNodeTypeAsString(startNode->nodeType), console::yellowFGColor);
		console::Write(")");
		console::WriteLine("{...}");
		return 0;
	}

	if (startNode->attributes.size() > 0) {
		console::Write(" @[");
		for (int c = 0; c < startNode->attributes.size(); c++) {
			console::Write(startNode->attributes[c]->token->first, console::cyanFGColor);
			if (startNode->attributes[c]->childNodes.size() > 0)
				console::Write("(...)");
			if (c < (int)startNode->attributes.size() - 1)
				console::Write(", ");
		}
		console::Write("]");
	}

	if (startNode->childNodes.size() > 0 || startNode->leafNodes.size() > 0)
		printf("\n");

	for (int c = 0; c < startNode->childNodes.size(); c++) {
		printAST(startNode->childNodes[c], depth + 1);
	}

	// Print unused leaf nodes
	depth++;
	if (startNode->leafNodes.size() > 0) {
		console::printIndent(depth);
		printf("!unusedLeafNodes!:{\n");
		for (int c = 0; c < startNode->leafNodes.size(); c++) {
			printAST(startNode->leafNodes[c], depth + 1);
		}
		console::printIndent(depth);
		printf("}\n");
	}
	depth--;

	if (startNode->childNodes.size() > 0 || startNode->leafNodes.size() > 0)
		console::printIndent(depth);
	printf("}\n");


	return 0;
}

void stripCommentNodes(ASTNode* node)
{
	std::vector<ASTNode*>& children = node->childNodes;
	int i = 0;
	while (i < (int)children.size()) {
		if (children[i]->nodeType == Comment_Node)
			children.erase(children.begin() + i);
		else
			i++;
	}
	for (int c = 0; c < (int)node->childNodes.size(); c++)
		stripCommentNodes(node->childNodes[c]);
}

void generateOutputCode(ASTNode*& node, int depth, int pass)
{
	switch (node->nodeType) {
		case Compiler_Define_Struct: {
			if (verbosity >= 4) {
				console::printIndent(depth + 1);
				console::Write("pass ");
				console::Write(std::to_string(pass), console::greenFGColor);
				console::Write(": generating struct for: ");
				console::WriteLine(node->token->first, console::yellowFGColor);
			}
			if (node->codegen != nullptr) {
				(node->*(node->codegen))(pass);
				if (wasError)
					goto errorDuringCodegen;
			}
			break;
		}

		case Compiler_Define_Cast: {
			if (pass == 0)
				break;
			if (verbosity >= 4) {
				console::printIndent(depth + 1);
				console::Write("pass ");
				console::Write(std::to_string(pass), console::greenFGColor);
				console::Write(": generating cast for: ");
				console::WriteLine(node->token->first, console::yellowFGColor);
			}
			if (node->codegen != nullptr) {
				auto fnVal = (Function*)(node->*(node->codegen))(pass);
				if (wasError)
					goto errorDuringCodegen;
			}
			break;
		}

		case Compiler_Define_Function: {
			if (pass == 0)
				break;
			if (verbosity >= 4) {
				console::printIndent(depth + 1);
				console::Write("pass ");
				console::Write(std::to_string(pass), console::greenFGColor);
				console::Write(": generating for: ");
				console::WriteLine(node->token->first, console::yellowFGColor);
			}
			if (node->codegen != nullptr) {
				auto fnVal = (Function*)(node->*(node->codegen))(pass);
				if (wasError)
					goto errorDuringCodegen;
			}
			break;
		}

		case Compile_Time_Directive: {
			if (pass == 0)
				break;
			if (verbosity >= 4) {
				console::printIndent(depth + 1);
				console::Write("pass ");
				console::Write(std::to_string(pass), console::greenFGColor);
				console::Write(": generating for: ");
				console::WriteLine(node->token->first, console::yellowFGColor);
			}
			if (node->codegen != nullptr) {
				auto fnVal = (Function*)(node->*(node->codegen))(pass);
				if (wasError)
					goto errorDuringCodegen;
			}
			break;
		}

		case Expression_Statement: {
			// Module-scope (root-level) variable declaration: compile as LLVM global.
			if (pass == 1)
				declareModuleScopeVariable(node, node, false);
			break;
		}

		case Compiler_Define: {
			// Named module node (e.g. Fore :: module { ... }): register and declare globals.
			if (pass == 1)
				processModuleForDeclarations(node);
			break;
		}

		case Scope_Body: {
			if (verbosity >= 4) {
				console::printIndent(depth + 1);
				console::Write("pass ");
				console::Write(std::to_string(pass), console::greenFGColor);
				console::WriteLine(": generating scope body");
			}
			for (auto& c : node->childNodes) {
				generateOutputCode(c, depth + 1, pass);
				if (wasError)
					goto errorDuringCodegen;
			}

			break;
		}

		default:
			break;
	}
	return;

errorDuringCodegen:
	exit(1);
}
