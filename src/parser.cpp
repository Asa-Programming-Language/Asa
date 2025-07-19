#include "parser.h"

void* (ASTNode::*codegen)() = nullptr;

tokenPair getNextNonNothingToken(const std::vector<tokenPair>& tokens, int& i)
{
	tokenPair t = tokenPair();
	for (;;) {
		if (i >= tokens.size() - 1 || tokens[i].second == EndOfFile) {
			return t;
		}
		tokenPair TOKENPAIR = NEXT_TOKEN(tokens, i);

		if (TOKENPAIR.second != Nothing && TOKENPAIR.second != EndOfLine) {
			return TOKENPAIR;
		}
	}
	return t;
}

bool GATHER_SCOPE_BODY(const std::vector<tokenPair>& tokens, std::vector<tokenPair>& subTokens, int brLevel, int& i, bool preserveBraces = false)
{
	int braceLevel = brLevel;
	subTokens = std::vector<tokenPair>();
	tokenPair firstToken;
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
		if (i >= tokens.size() - 1 || tokens[i].second == EndOfFile) {
			printTokenError(firstToken, "Unmatched brace", __LINE__);
			exit(1);
			break;
		}
		tokenPair t = NEXT_TOKEN(tokens, i);

		if (t.second == Nothing)
			continue;

		if (braceLevel <= 0 && t.second == Semi_Colon)
			return true;
		if (t.second == Left_Brace) {
			if (braceLevel != 0 || preserveBraces)
				subTokens.push_back(t);
			braceLevel++;
		}
		else if (t.second == Right_Brace) {
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

bool GATHER_SCOPE_BODY_APPEND(const std::vector<tokenPair>& tokens, std::vector<tokenPair>& subTokens, int brLevel, int& i, bool preserveBraces = false)
{
	int braceLevel = brLevel;
	tokenPair firstToken = NEXT_TOKEN(tokens, i);
	i--;
	for (;;) {
		if (i >= tokens.size() - 1 || tokens[i].second == EndOfFile) {
			printTokenError(firstToken, "Unmatched brace");
			exit(1);
			break;
		}
		tokenPair t = NEXT_TOKEN(tokens, i);

		if (t.second == Nothing)
			continue;

		if (braceLevel <= 0 && t.second == Semi_Colon)
			return true;
		if (t.second == Left_Brace) {
			if (braceLevel != 0 || preserveBraces)
				subTokens.push_back(t);
			braceLevel++;
		}
		else if (t.second == Right_Brace) {
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


void GATHER_PAREN_EXPRESSION(const std::vector<tokenPair>& tokens, std::vector<tokenPair>& subTokens, int pLevel, int& i, bool preserveBraces = false)
{
	int parenLevel = pLevel;
	if (pLevel == 1)
		i--;
	tokenPair firstToken = NEXT_TOKEN(tokens, i);
	if (pLevel != 1)
		i--;
	for (;;) {
		if (i >= tokens.size() - 1 || tokens[i].second == EndOfFile || tokens[i].second == Semi_Colon) {
			printTokenError(firstToken, "Unmatched parenthesis");
			exit(1);
			break;
		}
		tokenPair t = NEXT_TOKEN(tokens, i);

		if (t.second == Nothing)
			continue;

		if (t.second == Left_Paren) {
			if (parenLevel != 0 || preserveBraces)
				subTokens.push_back(t);
			parenLevel++;
		}
		else if (t.second == Right_Paren) {
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

bool GATHER_TO_SEMICOLON(const std::vector<tokenPair>& tokens, std::vector<tokenPair>& subTokens, int& i, bool includeLast = false, bool allowRunOut = false)
{
	tokenPair firstToken;
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
		if (i >= tokens.size() - 1 || tokens[i].second == EndOfLine) {
			if (!allowRunOut) {
				printTokenError(firstToken, "Missing semicolon");
				exit(1);
			}
			break;
		}
		tokenPair t = NEXT_TOKEN(tokens, i);

		if (t.second == Nothing)
			continue;

		if (t.second == Semi_Colon) {
			if (includeLast)
				subTokens.push_back(t);
			break;
		}

		subTokens.push_back(t);
	}
	return false;
}

bool GATHER_TO_SEMICOLON_OR_OTHER(const std::vector<tokenPair>& tokens, std::vector<tokenPair>& subTokens, int pLevel, int& i, TokenType other, bool includeLast)
{
	tokenPair firstToken;
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
		if (i >= tokens.size() - 1 || tokens[i].second == EndOfLine) {
			printTokenError(firstToken, "Missing semicolon or " + tokenAsString(other));
			exit(1);
			break;
		}
		tokenPair t = NEXT_TOKEN(tokens, i);

		if (t.second == Nothing)
			continue;

		if (t.second == Semi_Colon || t.second == other) {
			if (includeLast)
				subTokens.push_back(t);
			break;
		}

		subTokens.push_back(t);
	}
	return false;
}

bool GATHER_TO_A_OR_B(const std::vector<tokenPair>& tokens, std::vector<tokenPair>& subTokens, int pLevel, int& i, TokenType a, TokenType b, bool includeLast)
{
	tokenPair firstToken;
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
		tokenPair t = NEXT_TOKEN(tokens, i);

		if (t.second == Nothing)
			continue;

		if (t.second == a || t.second == b) {
			if (includeLast)
				subTokens.push_back(t);
			break;
		}

		subTokens.push_back(t);
	}
	return false;
}

bool GATHER_TO_TOKEN(const std::vector<tokenPair>& tokens, std::vector<tokenPair>& subTokens, int pLevel, int& i, TokenType a, bool includeLast = false, bool allowRunOut = false)
{
	tokenPair firstToken;
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
		tokenPair t = NEXT_TOKEN(tokens, i);

		if (t.second == a) {
			if (includeLast)
				subTokens.push_back(t);
			break;
		}

		subTokens.push_back(t);
	}
	return false;
}

void printTokenError(tokenPair& token, std::string errorString, int sourceLineNumber, const char* fileName)
{
	if (verbosity >= 5) {
		if (fileName != "")
			std::cerr << "Source file: " << fileName << std::endl;
		if (sourceLineNumber > 0)
			std::cerr << "Line: " << sourceLineNumber << std::endl;
	}
	console::PrintError(errorString);
	std::string lineNumberStr = std::to_string(token.lineNumber);
	console::Write(lineNumberStr + " |  ", console::yellowFGColor);
	console::WriteLine(*(token.lineValue));
	for (int i = 0; i < lineNumberStr.size() + 4 + token.indexInLine - 1; i++)
		console::Write(" ");
	for (int i = 0; i < token.first.size(); i++)
		console::Write("^", console::redFGColor);
	console::WriteLine();
	for (int i = 0; i < lineNumberStr.size() + 4 + token.indexInLine - 1; i++)
		console::Write(" ");
	console::Write("here", console::redFGColor);
	console::WriteLine("\n");
	//throw;
	exit(1);
}

void printTokenWarning(tokenPair& token, std::string errorString, int sourceLineNumber, const char* fileName)
{
	if (verbosity >= 5) {
		if (fileName != "")
			std::cerr << "Source file: " << fileName << std::endl;
		if (sourceLineNumber > 0)
			std::cerr << "Line: " << sourceLineNumber << std::endl;
	}
	console::PrintWarning(errorString);
	std::string lineNumberStr = std::to_string(token.lineNumber);
	console::Write(lineNumberStr + " |  ", console::yellowFGColor);
	console::WriteLine(*(token.lineValue));
	for (int i = 0; i < lineNumberStr.size() + 4 + token.indexInLine - 1; i++)
		console::Write(" ");
	for (int i = 0; i < token.first.size(); i++)
		console::Write("^", console::yellowFGColor);
	console::WriteLine();
	for (int i = 0; i < lineNumberStr.size() + 4 + token.indexInLine - 1; i++)
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

int beginParse(const std::vector<std::pair<std::string, TokenType>>& tokens)
{
	return 0;
}

std::vector<ASTNode*> ASTNodes = std::vector<ASTNode*>();

std::map<TokenType, ASTNodeType> binaryOperatorExType = {
	{Dot_Dot, Range_Node},
	{Dot, Module_Scope},
	{Bang_Equal, Compare_Not},
	{Equal_Equal, Compare_Equal},
	{Less, Compare_Less},
	{Greater, Compare_Greater},
	{Greater_Equal, Compare_GreaterEqual},
	{Plus, Expression_Plus},
	{Minus, Expression_Minus},
	{Star, Expression_Times},
	{Slash, Expression_Divided},
};

std::map<ASTNodeType, int> operatorPrecedence = {
	{Expression_Paren_Term, 5},	 // ()
	{Expression_Times, 4},		 // *
	{Expression_Divided, 4},	 // /
	{Expression_Plus, 3},		 // +
	{Expression_Minus, 3},		 // -
	{Compare_Equal, 2},			 // ==
	{Compare_Not, 2},			 // !=
	{Compare_Less, 2},			 // <
	{Compare_LessEqual, 2},		 // <=
	{Compare_Greater, 2},		 // >
	{Compare_GreaterEqual, 2},	 // >=
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

ASTNode* generateAST(const std::vector<tokenPair>& tokens, int depth, ASTNode* parentNodePtr)
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
		parentNode->token = tokenPair("global", Nothing, 0, 0, nullptr);
		//parentNode->tokenType = Nothing;
		parentNode->nodeType = Scope_Body;
	}

	// Iterate all tokens
	for (int i = 0; i < tokens.size(); i++) {
		ASTNode* node = new ASTNode();
		ASTNodes.push_back(node);
		//node.prevNode = &parentNode;
		tokenPair token = tokens[i];
		std::string tokenValue = tokens[i].first;
		TokenType tokenType = tokens[i].second;
		int lineNumber = tokens[i].lineNumber;
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

				std::vector<tokenPair> subTokens = std::vector<tokenPair>();

				// Step through all tokens until parens are closed
				GATHER_PAREN_EXPRESSION(tokens, subTokens, 0, i, false);

				conditionNode = generateAST(subTokens, depth + 1);
				conditionNode->nodeType = Condition;

				// Step through all tokens to gather body until braces are closed
				GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;
				bodyNode->codegen = &ASTNode::generateScopeBody;

				//				// If the next token is Else_Statement gather body also and generate AST
				//				tokenPair nextToken = getNextNonNothingToken(tokens, i);
				//				if (nextToken.second == Else_Statement) {
				//					subTokens = std::vector<tokenPair>();
				//
				//				gatherAnotherElseIf:
				//					tokenPair lookahead = getNextNonNothingToken(tokens, i);  // Look at the token after 'else'
				//					// Else If
				//					if (lookahead.second == If_Statement) {
				//						// Parse as regular if
				//						subTokens.push_back(lookahead);
				//						GATHER_PAREN_EXPRESSION(tokens, subTokens, 0, i, true);
				//						GATHER_SCOPE_BODY_APPEND(tokens, subTokens, 0, i, true);
				//						// If the token after the body is another else/else if, get another
				//						if ((lookahead = getNextNonNothingToken(tokens, i)).second == Else_Statement)
				//							goto gatherAnotherElseIf;
				//						i--;
				//						printf("cToken: \"%s\"\n", lookahead.first.c_str());
				//						printf("gathered else if:\n");
				//						for (const auto& s : subTokens)
				//							printf("%s", s.first.c_str());
				//						printf("\n");
				//						elseNode = generateAST(subTokens, depth + 1);
				//					}
				//					// Regular Else
				//					else {
				//						i--;
				//						// Otherwise, gather the else-body block
				//						//subTokens.push_back(tokens[i]);
				//						GATHER_SCOPE_BODY_APPEND(tokens, subTokens, 0, i, true);
				//						printf("gathered else:\n");
				//						for (const auto& s : subTokens)
				//							printf("%s", s.first.c_str());
				//						printf("\n");
				//						elseNode = generateAST(subTokens, depth + 1);
				//					}
				//				}
				//				else {
				//					i--;
				//					elseNode = new ASTNode();  // No else
				//				}

				node->childNodes.push_back(conditionNode);
				node->childNodes.push_back(bodyNode);
				ASTNode* blankElse = new ASTNode();	 // default else
				blankElse->nodeType = Else_Statement_Node;
				blankElse->codegen = &ASTNode::generateScopeBody;
				node->childNodes.push_back(blankElse);

				ASTNode* prevNode = node;
				//printAST(prevNode);
				for (;;) {
					subTokens = std::vector<tokenPair>();
					int sI = i;
					tokenPair nextToken = getNextNonNothingToken(tokens, i);  // i will next point to { or if
					if (nextToken.second == Else_Statement) {
						int sI2 = i;
						tokenPair nextToken2 = getNextNonNothingToken(tokens, i);  // Gets the { or if
						printf("token: %s\n", nextToken2.first.c_str());
						//tokenPair nextToken = NEXT_TOKEN(tokens, i);  // Gets the { or if
						// Else If
						if (nextToken2.second == If_Statement) {
							// Parse as regular if
							subTokens.push_back(nextToken2);  // add `if`
							GATHER_PAREN_EXPRESSION(tokens, subTokens, 0, i, true);
							GATHER_SCOPE_BODY_APPEND(tokens, subTokens, 0, i, true);
							//subTokens.pop_back();
							//i++;
							printf("gathered else if:\n");
							for (const auto& s : subTokens)
								printf("%s", s.first.c_str());
							printf("\n");
							ASTNode* elseNode = generateAST(subTokens, depth + 1)->childNodes[0];
							//elseNode->nodeType = Else_Statement_Node;
							//elseNode->codegen = &ASTNode::generateScopeBody;
							prevNode->childNodes[2] = elseNode;
							printf("new prevNode:\n");
							printAST(prevNode);
							prevNode = elseNode;
							continue;
						}
						// Regular Else
						else {
							//i--;
							i = sI2;
							//// Otherwise, gather the else-body block
							//subTokens.push_back(tokens[i]);
							GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);
							printf("gathered else:\n");
							for (const auto& s : subTokens)
								printf("%s", s.first.c_str());
							printf("\n");
							ASTNode* elseNode = generateAST(subTokens, depth + 1);
							elseNode->nodeType = Else_Statement_Node;
							elseNode->codegen = &ASTNode::generateScopeBody;
							prevNode->childNodes[2] = elseNode;
							printf("new prevNode:\n");
							printAST(prevNode);
							prevNode = elseNode;
							break;	// else must be the end of chain
						}
					}
					else {
						i = sI;
						//i--;
						//ASTNode* elseNode = new ASTNode();	// No else
						//elseNode->nodeType = Else_Statement_Node;
						//elseNode->codegen = &ASTNode::generateScopeBody;
						//prevNode->childNodes[2] = elseNode;
						//prevNode = elseNode;
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

				ASTNode* conditionNode = new ASTNode();
				ASTNode* bodyNode = new ASTNode();

				std::vector<tokenPair> subTokens = std::vector<tokenPair>();

				// Step through all tokens until parens are closed
				GATHER_PAREN_EXPRESSION(tokens, subTokens, 0, i, false);

				conditionNode = generateAST(subTokens, depth + 1);
				conditionNode->nodeType = Condition;

				// Step through all tokens to gather body until braces are closed
				GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);

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
				std::vector<tokenPair> subTokens = std::vector<tokenPair>();
				for (;;) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair t = NEXT_TOKEN(tokens, i);

					if (t.second == Left_Paren)
						parenLevel++;
					if (t.second == Right_Paren) {
						parenLevel--;
						if (parenLevel == 0)
							break;
					}

					// If colon like =>  for(i : 0..10)
					// generate AST for i, and set iteratorNode as the output
					if (t.second == Colon && iteratorNode->nodeType == Nothing_Node) {
						iteratorNode = generateAST(subTokens, depth + 1);
						iteratorNode->nodeType = Iterator;
						iteratorNode->codegen = &ASTNode::generateIterator;
						subTokens = std::vector<tokenPair>();  // Clear subtokens
						//ASTNode* exprStatement = new ASTNode();
						//exprStatement->nodeType = Expression_Statement;
						//exprStatement->childNodes.push_back(iteratorNode);
						//iteratorNode = exprStatement;
						continue;
					}

					subTokens.push_back(t);

					if (parenLevel == 0 || t.second == EndOfLine || t.second == Semi_Colon)
						break;
				}
				rangeNode = generateAST(subTokens, depth + 1)->childNodes[0];
				rangeNode->nodeType = Range_Node;

				// Step through all tokens to gather body until braces are closed
				GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);

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

				std::vector<tokenPair> subTokens = std::vector<tokenPair>();

				// Step through all tokens to gather body until braces are closed
				GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;
				bodyNode->codegen = &ASTNode::generateScopeBody;

				node->childNodes.push_back(bodyNode);
				break;
			}

			case Module_Define: {
				node->nodeType = Module_Define_Node;

				ASTNode* bodyNode = new ASTNode();

				std::vector<tokenPair> subTokens = std::vector<tokenPair>();

				// Step through all tokens to gather body until braces are closed
				GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;
				bodyNode->codegen = &ASTNode::generateScopeBody;

				node->childNodes.push_back(bodyNode);
				break;
			}

			// Two component operations
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
			case Slash: {
				node->nodeType = binaryOperatorExType[tokenType];
				node->codegen = &ASTNode::generateBinaryExpression;

				ASTNode* firstTerm = new ASTNode();
				ASTNode* secondTerm = new ASTNode();
				bool isLeaf = true;

				// Instead of backtracking to get the first term, pop the leafNodes vector =)
				if (parentNode->leafNodes.size() == 0) {
					printTokenError(token, "Binary operator expected left argument");
					exit(1);
				}
				firstTerm = parentNode->leafNodes.back();
				parentNode->leafNodes.pop_back();
				//firstTerm->nodeType = Expression_Term;

				// Step through all following tokens until parens are closed
				int parenLevel = 1;
				std::vector<tokenPair> subTokens = std::vector<tokenPair>();
				for (;;) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair t = NEXT_TOKEN(tokens, i);

					if (t.second == Left_Paren)
						parenLevel++;
					if (t.second == Right_Paren)
						parenLevel--;

					if (parenLevel == 0)
						break;
					if (t.second == EndOfLine || t.second == Semi_Colon) {
						isLeaf = false;
						break;
					}

					subTokens.push_back(t);
				}
				if (subTokens.size() == 0) {
					printTokenError(token, "Binary operator expected right argument");
					exit(1);
				}
				secondTerm = generateAST(subTokens, depth + 1)->childNodes[0];
				//secondTerm->nodeType = Expression_Term;

				node->childNodes.push_back(firstTerm);
				node->childNodes.push_back(secondTerm);
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

				std::vector<tokenPair> subTokens = std::vector<tokenPair>();

				// Step through all following tokens until end of term
				GATHER_TO_SEMICOLON(tokens, subTokens, i, false);
				//int parenLevel = 1;
				//for (;;) {
				//	if (i >= tokens.size() - 1)
				//		break;
				//	tokenPair t = NEXT_TOKEN(tokens, i);

				//	if (t.second == Left_Paren)
				//		parenLevel++;
				//	if (t.second == Right_Paren)
				//		parenLevel--;

				//	if ((t.second == EndOfLine || t.second == Semi_Colon))
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

				tokenPair tt = NEXT_TOKEN(tokens, i);
				identifier->token = tt;
				//identifier->tokenType = tt.second;
				identifier->nodeType = Identifier_Node;

				std::vector<tokenPair> subTokens = std::vector<tokenPair>();

				// Step through all following tokens until end of line via semicolon
				GATHER_TO_SEMICOLON(tokens, subTokens, i, true, true);
				//GATHER_TO_SEMICOLON_OR_OTHER(tokens, subTokens, 1, i, Left_Brace, true);

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;
				bodyNode->codegen = &ASTNode::generateScopeBody;

				if (identifier->token.first == "cast") {
					node->codegen = &ASTNode::generateCast;
				}

				node->token.first = "#" + identifier->token.first;
				node->childNodes.push_back(identifier);
				node->childNodes.push_back(bodyNode);
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

				identifier = parentNode->leafNodes.back();
				parentNode->leafNodes.pop_back();
				identifier->nodeType = Identifier_Node;
				argumentsNode->nodeType = Arguments;
				modifiersNode->nodeType = Compiler_Modifiers;

				// Based on the next token, decide what this compiler define does
				// (if function, or if/for/while etc. or any line of code)

				// If the first token after :: is not a paren, or the token is in the map of non-function compiler defines
				tokenPair tt = NEXT_TOKEN(tokens, i);
				if (tt.second != Left_Paren && tokens[i + 1].second != Left_Paren || compileTimeDefinable.find(tt.second) != compileTimeDefinable.end()) {
					i--;  // NEXT_TOKEN==tt starts on first token after :: <here>
					bool isCompileTimeDefinableKeyword = compileTimeDefinable.find(tt.second) != compileTimeDefinable.end();
					// Step through all following tokens until parens start OR braces start
					int tokenNum = 0;
					std::vector<tokenPair> subTokens = std::vector<tokenPair>();
					for (;;) {
						if (i >= tokens.size() - 1) {
							printTokenError(tt, "Unmatched parenthesis", __LINE__);
							exit(1);
						}
						tokenPair t = NEXT_TOKEN(tokens, i);

						if (t.second == Left_Brace) {
							i--;
							break;
						}

						subTokens.push_back(t);

						if (t.second == Left_Paren) {
							//i--;
							break;
						}
						if (tt.second != EndOfLine)
							tokenNum++;
					}
					// Step through all following tokens until braces start
					tokenNum = 0;
					for (;;) {
						if (i >= tokens.size() - 1) {
							printTokenError(tt, "Unmatched brace", __LINE__);
							exit(1);
						}
						tokenPair t = NEXT_TOKEN(tokens, i);
						//if (tokenNum >= 2 && t.second != Left_Brace) {
						//	printTokenError(tt, "Unmatched brace");
						//	exit(1);
						//}

						if (t.second == Left_Brace) {
							break;
						}

						subTokens.push_back(t);
						if (tt.second != EndOfLine)
							tokenNum++;
					}
					i--;
					// Step through all tokens to gather body until braces are closed
					//GATHER_SCOPE_BODY_APPEND(tokens, subTokens, 0, i, true);
					GATHER_SCOPE_BODY_APPEND(tokens, subTokens, 0, i, isCompileTimeDefinableKeyword);

					bodyNode = generateAST(subTokens, depth + 1);
					bodyNode->nodeType = Scope_Body;
					bodyNode->codegen = &ASTNode::generateScopeBody;

					node->token.first = identifier->token.first;
					node->childNodes.push_back(bodyNode);

					// Check if extra type following :: but before {
					if (bodyNode->childNodes.size() > 0) {
						tokenPair s = bodyNode->childNodes[0]->token;

						// Check for special definition types
						if (s.first == "struct") {
							tokenPair structName = node->token;
							node = bodyNode->childNodes[0];
							node->token = structName;
							node->nodeType = Compiler_Define_Struct;
							node->codegen = &ASTNode::generateStruct;
						}
					}
				}
				else {
					node->nodeType = Compiler_Define_Function;
					node->codegen = &ASTNode::generateFunction;
					i--;
					// Step through all following tokens until parens start
					std::vector<tokenPair> subTokens = std::vector<tokenPair>();
					for (;;) {
						if (i >= tokens.size() - 1)
							break;
						tokenPair t = NEXT_TOKEN(tokens, i);

						if (t.second == Left_Paren)
							break;

						subTokens.push_back(t);
					}
					secondPart = generateAST(subTokens, depth + 1);
					secondPart->nodeType = Type_Node;
					// Step through all following tokens until parens are closed
					int parenLevel = 1;
					subTokens = std::vector<tokenPair>();
					for (;;) {
						if (i >= tokens.size() - 1)
							break;
						tokenPair t = NEXT_TOKEN(tokens, i);

						if (t.second == Left_Paren)
							parenLevel++;
						if (t.second == Right_Paren)
							parenLevel--;

						if (parenLevel == 0 || t.second == EndOfLine || t.second == Semi_Colon)
							break;
						// If comma and parenLevel is in same scope
						if ((t.second == Comma && parenLevel == 1)) {
							ASTNode* newNode = new ASTNode();
							generateAST(subTokens, depth + 1, newNode);
							newNode->nodeType = Expression_Term;
							newNode->codegen = &ASTNode::generateExpression;
							arguments.push_back(newNode);
							subTokens = std::vector<tokenPair>();
							continue;
						}

						subTokens.push_back(t);
					}
					ASTNode* newNode = new ASTNode();
					generateAST(subTokens, depth + 1, newNode);
					newNode->nodeType = Expression_Term;
					newNode->codegen = &ASTNode::generateExpression;
					arguments.push_back(newNode);
					subTokens = std::vector<tokenPair>();
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
					}

					// Step through all tokens to gather body until braces are closed
					subTokens = std::vector<tokenPair>();
					bool endedEarly = GATHER_SCOPE_BODY(tokens, subTokens, 0, i, true);

					if (!endedEarly) {
						bodyNode = generateAST(subTokens, depth + 1);
						bodyNode->nodeType = Scope_Body;
						bodyNode->codegen = &ASTNode::generateScopeBody;
					}
					else
						node->codegen = &ASTNode::generatePrototype;

					node->token.first = identifier->token.first;
					node->childNodes.push_back(identifier);
					node->childNodes.push_back(secondPart);
					node->childNodes.push_back(argumentsNode);
					node->childNodes.push_back(modifiersNode);
					if (!endedEarly)
						node->childNodes.push_back(bodyNode);

					// Check for special function types
					if (identifier->token.first == "cast") {
						if (secondPart->childNodes.size() > 0)
							node->token.first = secondPart->childNodes[0]->token.first;
						else {
							printTokenError(secondPart->token, "Cast function must have a return type");
							exit(1);
						}
						node->nodeType = Compiler_Define_Cast;
					}
				}
				break;
			}

			case Binary:
			case Unary: {
				node->nodeType = Operator_Overload_Node;

				// Get next token, which should be the operator
				tokenPair t = NEXT_TOKEN(tokens, i);

				ASTNode* operatorNode = new ASTNode(Operator_Type_Node, {}, t);

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
				std::vector<tokenPair> subTokens = std::vector<tokenPair>();
				for (;;) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair t = NEXT_TOKEN(tokens, i);

					if (t.second == Left_Paren)
						parenLevel++;
					if (t.second == Right_Paren)
						parenLevel--;

					if (parenLevel == 0)
						break;
					if (t.second == EndOfLine || t.second == Semi_Colon) {
						isLeaf = false;
						break;
					}
					// If comma and parenLevel is in same scope
					if ((t.second == Comma && parenLevel == 1)) {
						ASTNode* newNode = new ASTNode();
						generateAST(subTokens, depth + 1, newNode);
						newNode->nodeType = Expression_Term;
						newNode->codegen = &ASTNode::generateExpression;
						insideNodes.push_back(newNode);
						subTokens = std::vector<tokenPair>();
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
				node->nodeType = String_Node;
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case True_Literal: {
				node->nodeType = Boolean_Node;
				node->codegen = &ASTNode::generateConstant;
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case False_Literal: {
				node->nodeType = Boolean_Node;
				node->codegen = &ASTNode::generateConstant;
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case Comment: {
				goto dontAddNodeForce;
			}

			case Identifier: {
				node->nodeType = Identifier_Node;
				node->codegen = &ASTNode::generateVariableExpression;

				// If this is identifer, and is preceded by identifier, then that one is type, and this is name
				if (parentNode->leafNodes.size() >= 1) {
					if (parentNode->leafNodes.back()->nodeType == Identifier_Node) {
						ASTNode* typeNode = parentNode->leafNodes.back();
						parentNode->leafNodes.pop_back();
						typeNode->nodeType = Type_Node;
						typeNode->codegen = nullptr;
						node->childNodes.push_back(typeNode);
					}
				}
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case Return_Statement:
				node->nodeType = Return_Node;
				node->codegen = &ASTNode::generateReturn;
				goto positionChangeStatement;
			case Break_Statement:
				node->nodeType = Break_Node;
				goto positionChangeStatement;
			case Continue_Statement:
				node->nodeType = Continue_Node;
				goto positionChangeStatement;
			case Goto_Statement: {
				node->nodeType = Goto_Node;
			positionChangeStatement:

				ASTNode* argumentTerm = new ASTNode();
				std::vector<tokenPair> subTokens = std::vector<tokenPair>();

				// Step through all following tokens until end of term
				GATHER_TO_SEMICOLON(tokens, subTokens, i, true);
				//for (;;) {
				//	if (i >= tokens.size() - 1)
				//		break;
				//	tokenPair t = NEXT_TOKEN(tokens, i);

				//	if (t.second == Left_Paren)
				//		parenLevel++;
				//	if (t.second == Right_Paren)
				//		parenLevel--;

				//	if ((t.second == EndOfLine || t.second == Semi_Colon))
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
				std::vector<tokenPair> subTokens = std::vector<tokenPair>();

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
					printTokenWarning(token, "Undefined node");
				goto dontAddNodeForce;
			}
		}


	addNode:
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

	// First, recursively fix all child nodes
	for (auto& child : node->childNodes) {
		fixPrecedence(child);
	}

	// Check if this node is a binary operator that needs precedence fixing
	if (operatorPrecedence.find(node->nodeType) != operatorPrecedence.end()) {

		// Must have exactly 2 children for binary operators
		if (node->childNodes.size() != 2)
			return;

		ASTNode* leftChild = node->childNodes[0];
		ASTNode* rightChild = node->childNodes[1];

		// Check if left child is a binary operator with lower precedence
		if (leftChild->childNodes.size() == 2 &&
			operatorPrecedence.find(leftChild->nodeType) != operatorPrecedence.end()) {

			int currentPrecedence = operatorPrecedence[node->nodeType];
			int leftPrecedence = operatorPrecedence[leftChild->nodeType];

			// If current operator has higher precedence than left child, rotate right
			if (currentPrecedence > leftPrecedence) {
				// Rotate: (A op1 B) op2 C  ->  A op1 (B op2 C)
				// where op2 has higher precedence than op1

				ASTNode* A = leftChild->childNodes[0];
				ASTNode* B = leftChild->childNodes[1];
				ASTNode* C = rightChild;

				// Create new subtree: B op2 C
				ASTNode* newRight = new ASTNode();
				newRight->nodeType = node->nodeType;
				newRight->token = node->token;
				//newRight->tokenType = node->tokenType;
				newRight->lineNumber = node->lineNumber;
				newRight->childNodes.push_back(B);
				newRight->childNodes.push_back(C);

				// Update current node to be: A op1 (B op2 C)
				node->nodeType = leftChild->nodeType;
				node->token = leftChild->token;
				//node->tokenType = leftChild->tokenType;
				node->lineNumber = leftChild->lineNumber;

				// Clear and rebuild children
				node->childNodes.clear();
				node->childNodes.push_back(A);
				node->childNodes.push_back(newRight);

				// Recursively fix the new subtree
				fixPrecedence(newRight);
			}
		}

		// Check if right child is a binary operator with lower or equal precedence
		if (rightChild->childNodes.size() == 2 &&
			operatorPrecedence.find(rightChild->nodeType) != operatorPrecedence.end()) {

			int currentPrecedence = operatorPrecedence[node->nodeType];
			int rightPrecedence = operatorPrecedence[rightChild->nodeType];

			// If current operator has higher precedence than right child, rotate left
			if (currentPrecedence > rightPrecedence) {
				// Rotate: A op1 (B op2 C)  ->  (A op1 B) op2 C
				// where op1 has higher precedence than op2

				ASTNode* A = leftChild;
				ASTNode* B = rightChild->childNodes[0];
				ASTNode* C = rightChild->childNodes[1];

				// Create new subtree: A op1 B
				ASTNode* newLeft = new ASTNode();
				newLeft->nodeType = node->nodeType;
				newLeft->token = node->token;
				//newLeft->tokenType = node->tokenType;
				newLeft->lineNumber = node->lineNumber;
				newLeft->childNodes.push_back(A);
				newLeft->childNodes.push_back(B);

				// Update current node to be: (A op1 B) op2 C
				node->nodeType = rightChild->nodeType;
				node->token = rightChild->token;
				//node->tokenType = rightChild->tokenType;
				node->lineNumber = rightChild->lineNumber;

				// Clear and rebuild children
				node->childNodes.clear();
				node->childNodes.push_back(newLeft);
				node->childNodes.push_back(C);

				// Recursively fix the new subtree
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
						first_b = first->token.first == "true" ? true : false;
						output_val_ptr = &output_b;
						output_type = Boolean_Node;
						first_type = Boolean_Node;
						break;
					case Integer_Node:
						first_val_ptr = &first_i;
						first_i = std::stoi(first->token.first);
						output_val_ptr = &output_i;
						output_type = Integer_Node;
						first_type = Integer_Node;
						break;
					case Float_Node:
						first_val_ptr = &first_f;
						first_f = std::stod(first->token.first);
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
						second_b = second->token.first == "true" ? true : false;
						second_type = Boolean_Node;
						break;
					case Integer_Node:
						second_val_ptr = &second_i;
						second_i = std::stoi(second->token.first);
						if (output_type != Float_Node) {
							output_val_ptr = &output_i;
							output_type = Integer_Node;
						}
						second_type = Integer_Node;
						break;
					case Float_Node:
						second_val_ptr = &second_f;
						second_f = std::stod(second->token.first);
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
						case (Expression_Divided):
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
						case (Expression_Divided):
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
						node->token.second = Integer;
						break;
					case Float_Node:
						node->token.second = Float;
						break;
					default:
						break;
				}
				node->token.first = outString;
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
			if (node->childNodes[0]->token.first == "file") {
				std::string fileString = "";

				// Load file if provided
				if (node->childNodes.size() > 1) {
					ASTNode* strChild = node->childNodes[1]->childNodes[0];
					if (strChild->nodeType == String_Node) {
						std::string fileName = strChild->token.first.substr(1, strChild->token.first.size() - 2);

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

				std::vector<tokenPair> localTokens = std::vector<tokenPair>();

				// Begin tokenizing file
				int e = tokenize(fileString, localTokens);
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
				e = removeCommentTokens(localTokens);

				// Generate AST
				ASTNode* localRoot = generateAST(localTokens);
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

		std::vector<tokenPair> localTokens = std::vector<tokenPair>();

		// Begin tokenizing file
		int e = tokenize(outStr, localTokens);
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
		e = removeCommentTokens(localTokens);

		// Generate AST
		ASTNode* localRoot = generateAST(localTokens);

		// Look through file to see if it contains the desired module
		for (int j = 0; j < localRoot->childNodes.size(); j++) {
			if (localRoot->childNodes[j]->nodeType == Compiler_Define)
				if (localRoot->childNodes[j]->childNodes.size() >= 1)
					if (localRoot->childNodes[j]->childNodes[0]->childNodes.size() >= 1)
						if (localRoot->childNodes[j]->childNodes[0]->childNodes[0]->nodeType == Module_Define_Node) {
							ASTNode* moduleNode = localRoot->childNodes[j]->childNodes[0]->childNodes[0];
							if (localRoot->childNodes[j]->token.first == moduleName) {
								for (int i = 0; i < moduleNode->childNodes[0]->childNodes.size(); i++) {
									importedNodes.push_back(moduleNode->childNodes[0]->childNodes[i]);
									//rootNode->childNodes.push_back(localRoot->childNodes[i]);
								}
								if (verbosity >= 2)
									printModuleLoaded(moduleName, pathStr);
								return true;
							}
						}
		}
	}
	return false;
}

void addModuleImports(ASTNode*& node)
{
	for (int i = 0; i < node->childNodes.size(); i++)
		addModuleImports(node->childNodes[i]);

	if (node->childNodes.size() > 0)
		if (node->nodeType == Compile_Time_Directive) {
			if (node->childNodes[0]->token.first == "import") {
				std::string fileString = "";

				// Load module if module name is provided
				if (node->childNodes.size() > 1) {
					ASTNode* moduleNameNode = node->childNodes[1]->childNodes[0];
					if (moduleNameNode->nodeType == Module_Scope) {
						std::string modulePath = "";
						bool moduleFound = false;
						for (int i = 0; i < moduleNameNode->childNodes.size() - 1; i++)
							modulePath += moduleNameNode->childNodes[i]->token.first;
						std::string moduleName = moduleNameNode->childNodes.back()->token.first;

						if (importedModuleNames.find(moduleName) != importedModuleNames.end()) {
							node->nodeType = Nothing_Node;
							return;
						}

						std::string searchPath[2] = {projectDirectory + modulePath, executableDirectory + "modules/" + modulePath};
						if (directoryExists(searchPath[0]))
							moduleFound = loadModule(searchPath[0], moduleName);
						else if (directoryExists(searchPath[1]))
							moduleFound = loadModule(searchPath[1], moduleName);

						importedModuleNames.insert(moduleName);

						if (!moduleFound) {
							printTokenError(moduleNameNode->token, "Failed to import module, not found", __LINE__);
							exit(1);
						}
					}
					else if (moduleNameNode->nodeType == Identifier_Node) {
						bool moduleFound = false;
						std::string moduleName = moduleNameNode->token.first;

						if (importedModuleNames.find(moduleName) != importedModuleNames.end()) {
							node->nodeType = Nothing_Node;
							return;
						}

						std::string searchPath[2] = {projectDirectory, executableDirectory + "modules/"};
						if (directoryExists(searchPath[0]))
							moduleFound = loadModule(searchPath[0], moduleName);
						else if (directoryExists(searchPath[1]))
							moduleFound = loadModule(searchPath[1], moduleName);

						importedModuleNames.insert(moduleName);

						if (!moduleFound) {
							printTokenError(moduleNameNode->token, "Failed to import module, not found", __LINE__);
							exit(1);
						}
					}
					else {
						printTokenError(moduleNameNode->token, "Expected module name", __LINE__);
						exit(1);
					}
				}
				else {
					printTokenError(node->childNodes[0]->token, "Expected module name", __LINE__);
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

inline void printIndent(int& depth)
{
	for (int i = 0; i < depth; i++)
		printf("    ");
}

//std::vector<tokenPair> GATHER_SCOPE_BODY(int brLevel, int& i)
//{
//	int braceLevel = brLevel;
//	std::vector<tokenPair> subTokens = std::vector<tokenPair>();
//	for (;;) {
//		tokenPair t = NEXT_TOKEN(i);
//
//		if (t.second == Left_Brace)
//			braceLevel++;
//		if (t.second == Right_Brace)
//			braceLevel--;
//
//		subTokens.push_back(t);
//
//		if (braceLevel == 0 || t.second == EndOfFile)
//			break;
//	}
//	return subTokens;
//}

void orderASTOperations(ASTNode* startNode)
{
	for (int i = 0; i < startNode->childNodes.size(); i++) {
	}
}

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

	printIndent(depth);

	console::Write(startNode->token.first, console::greenFGColor);
	if (verbosity >= 5)
		if (startNode->token.first.size() > 0)
			printf(":L%d", startNode->lineNumber);
	console::Write(":(");
	console::Write(ASTNodeTypeAsString(startNode->nodeType), console::yellowFGColor);
	console::Write("){");

	if (startNode->childNodes.size() > 0 || startNode->leafNodes.size() > 0)
		printf("\n");

	for (int c = 0; c < startNode->childNodes.size(); c++) {
		printAST(startNode->childNodes[c], depth + 1);
	}

	// Print unused leaf nodes
	depth++;
	if (startNode->leafNodes.size() > 0) {
		printIndent(depth);
		printf("!unusedLeafNodes!:{\n");
		for (int c = 0; c < startNode->leafNodes.size(); c++) {
			printAST(startNode->leafNodes[c], depth + 1);
		}
		printIndent(depth);
		printf("}\n");
	}
	depth--;

	if (startNode->childNodes.size() > 0 || startNode->leafNodes.size() > 0)
		printIndent(depth);
	printf("}\n");


	return 0;
}


void generateOutputCode(ASTNode*& node, int depth, int pass)
{
	switch (node->nodeType) {
		case Compiler_Define_Cast:
		case Compiler_Define_Function: {
			console::Write("pass ");
			console::Write(std::to_string(pass), console::greenFGColor);
			console::Write(": generating for: ");
			console::WriteLine(node->token.first, console::yellowFGColor);
			if (node->codegen != nullptr)
				auto fnVal = (Function*)(node->*(node->codegen))(pass);
		}

		default:
			break;
	}
	for (auto& c : node->childNodes)
		generateOutputCode(c, depth + 1, pass);
}
