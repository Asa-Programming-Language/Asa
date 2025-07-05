#include "parser.h"

#define GATHER_SCOPE_BODY(subTokens, brLevel, i)                  \
	{                                                             \
		int braceLevel = brLevel;                                 \
		subTokens = std::vector<tokenPair>();                     \
		for (;;) {                                                \
			tokenPair TOKENPAIR = NEXT_TOKEN(i);                  \
                                                                  \
			if (TOKENPAIR.second == Left_Brace)                   \
				braceLevel++;                                     \
			if (TOKENPAIR.second == Right_Brace)                  \
				braceLevel--;                                     \
                                                                  \
			subTokens.push_back(TOKENPAIR);                       \
                                                                  \
			if (braceLevel == 0 || TOKENPAIR.second == EndOfFile) \
				break;                                            \
		}                                                         \
	}

int beginParse(const std::vector<std::pair<std::string, TokenType>>& tokens)
{
	return 0;
}

std::vector<ASTNode*> ASTNodes = std::vector<ASTNode*>();

std::map<TokenType, ASTNodeType> binaryOperatorExType = {
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

ASTNode* rootNode = new ASTNode();

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
		parentNode->token = "global";
		parentNode->tokenType = Nothing;
		parentNode->nodeType = Scope_Body;
	}

	// Iterate all tokens
	for (int i = 0; i < tokens.size(); i++) {
		ASTNode* node = new ASTNode();
		ASTNodes.push_back(node);
		//node.prevNode = &parentNode;
		std::string tokenValue = tokens[i].first;
		TokenType tokenType = tokens[i].second;
		int lineNumber = tokens[i].lineNumber;
		bool unusedNode = false;

		node->tokenType = tokenType;
		node->token = tokenValue;
		node->lineNumber = lineNumber;

		switch (tokenType) {
			case If_Statement: {
				node->nodeType = If_Statement_Node;

				ASTNode* conditionNode = new ASTNode();
				ASTNode* bodyNode = new ASTNode();

				i++;

				// Step through all tokens until parens are closed
				int parenLevel = 1;
				std::vector<tokenPair> subTokens = std::vector<tokenPair>();
				for (;;) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair t = NEXT_TOKEN(i);

					if (t.second == Left_Paren)
						parenLevel++;
					if (t.second == Right_Paren)
						parenLevel--;

					subTokens.push_back(t);

					if (parenLevel == 0 || t.second == EndOfLine)
						break;
				}
				conditionNode = generateAST(subTokens, depth + 1);
				conditionNode->nodeType = Condition;

				// Step through all tokens to gather body until braces are closed
				GATHER_SCOPE_BODY(subTokens, 0, i);
				PRINT_SUBTOKENS(subTokens);

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;

				node->childNodes.push_back(conditionNode);
				node->childNodes.push_back(bodyNode);
				break;
			}

			case While_Statement: {
				node->nodeType = While_Statement_Node;

				ASTNode* conditionNode = new ASTNode();
				ASTNode* bodyNode = new ASTNode();

				i++;

				// Step through all tokens until parens are closed
				int parenLevel = 1;
				std::vector<tokenPair> subTokens = std::vector<tokenPair>();
				for (;;) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair t = NEXT_TOKEN(i);

					if (t.second == Left_Paren)
						parenLevel++;
					if (t.second == Right_Paren)
						parenLevel--;

					subTokens.push_back(t);

					if (parenLevel == 0 || t.second == EndOfLine)
						break;
				}
				conditionNode = generateAST(subTokens, depth + 1);
				conditionNode->nodeType = Condition;

				// Step through all tokens to gather body until braces are closed
				GATHER_SCOPE_BODY(subTokens, 0, i);

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;

				node->childNodes.push_back(conditionNode);
				node->childNodes.push_back(bodyNode);
				break;
			}

			case For_Statement: {
				node->nodeType = For_Statement_Node;

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
					tokenPair t = NEXT_TOKEN(i);

					if (t.second == Left_Paren)
						parenLevel++;
					if (t.second == Right_Paren)
						parenLevel--;

					// If colon like =>  for(i : 0..10)
					// generate AST for i, and set iteratorNode as the output
					if (t.second == Colon) {
						iteratorNode = generateAST(subTokens, depth + 1);
						iteratorNode->nodeType = Iterator;
						subTokens = std::vector<tokenPair>();  // Clear subtokens
						continue;
					}

					subTokens.push_back(t);

					if (parenLevel == 0 || t.second == EndOfLine || t.second == Semi_Colon)
						break;
				}
				rangeNode = generateAST(subTokens, depth + 1);
				rangeNode->nodeType = Range_Node;

				// Step through all tokens to gather body until braces are closed
				GATHER_SCOPE_BODY(subTokens, 0, i);

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;

				// Only add iterator if one is explicity defined
				if (iteratorNode->nodeType == Iterator)
					node->childNodes.push_back(iteratorNode);
				node->childNodes.push_back(rangeNode);
				node->childNodes.push_back(bodyNode);
				break;
			}

			case Dot_Dot: {
				node->nodeType = Range_Node;

				ASTNode* firstTerm = new ASTNode();
				ASTNode* secondTerm = new ASTNode();

				// Instead of backtracking to get the first term, pop the leafNodes vector =)
				firstTerm->childNodes.push_back(parentNode->leafNodes.back());
				parentNode->leafNodes.pop_back();
				firstTerm->nodeType = Range_Begin;

				// Step through all following tokens until parens are closed
				int parenLevel = 1;
				std::vector<tokenPair> subTokens = std::vector<tokenPair>();
				for (;;) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair t = NEXT_TOKEN(i);

					if (t.second == Left_Paren)
						parenLevel++;
					if (t.second == Right_Paren)
						parenLevel--;

					if (t.second == EndOfLine || t.second == Semi_Colon)
						break;

					subTokens.push_back(t);

					if (parenLevel == 0)
						break;
				}
				secondTerm = generateAST(subTokens, depth + 1);
				secondTerm->nodeType = Range_End;

				node->childNodes.push_back(firstTerm);
				node->childNodes.push_back(secondTerm);
				break;
			}

			// Two component operations
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

				ASTNode* firstTerm = new ASTNode();
				ASTNode* secondTerm = new ASTNode();
				bool isLeaf = true;

				// Instead of backtracking to get the first term, pop the leafNodes vector =)
				firstTerm = parentNode->leafNodes.back();
				parentNode->leafNodes.pop_back();
				//firstTerm->nodeType = Expression_Term;

				// Step through all following tokens until parens are closed
				int parenLevel = 1;
				std::vector<tokenPair> subTokens = std::vector<tokenPair>();
				for (;;) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair t = NEXT_TOKEN(i);

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

				ASTNode* firstTerm = new ASTNode();
				ASTNode* secondTerm = new ASTNode();

				// Instead of backtracking to get the first term, pop the leafNodes vector =)
				firstTerm = parentNode->leafNodes.back();
				parentNode->leafNodes.pop_back();

				// Step through all following tokens until end of term
				int parenLevel = 1;
				std::vector<tokenPair> subTokens = std::vector<tokenPair>();
				for (;;) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair t = NEXT_TOKEN(i);

					if (t.second == Left_Paren)
						parenLevel++;
					if (t.second == Right_Paren)
						parenLevel--;

					if ((t.second == EndOfLine || t.second == Semi_Colon))
						break;

					subTokens.push_back(t);
				}
				secondTerm = generateAST(subTokens, depth + 1);
				secondTerm->nodeType = Expression_Term;

				node->childNodes.push_back(firstTerm);
				node->childNodes.push_back(secondTerm);
				break;
			}

			case Hash: {
				node->nodeType = Compile_Time_Directive;

				ASTNode* identifier = new ASTNode();
				ASTNode* bodyNode = new ASTNode();

				tokenPair tt = NEXT_TOKEN(i);
				identifier->token = tt.first;
				identifier->tokenType = tt.second;
				identifier->nodeType = Identifier_Node;

				// Step through all following tokens until end of line via semicolon
				std::vector<tokenPair> subTokens = std::vector<tokenPair>();
				while (i < tokens.size() - 1) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair t = NEXT_TOKEN(i);
					printf("token %d value: `%s`\n", i, t.first.c_str());

					if (t.second == Semi_Colon)
						break;
					if (t.second == EndOfLine)
						throw std::runtime_error("Error: Missing semicolon to specify end of line");


					printf("subtoken added of value: `%s`\n", t.first.c_str());
					subTokens.push_back(t);
				}
				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;

				node->token = "<#" + identifier->token + ">";
				node->childNodes.push_back(identifier);
				node->childNodes.push_back(bodyNode);
				break;
			}

			case Colon_Colon: {
				node->nodeType = Compiler_Define;

				ASTNode* identifier = new ASTNode();
				ASTNode* returnType = new ASTNode();
				ASTNode* argumentsNode = new ASTNode();
				ASTNode* bodyNode = new ASTNode();
				std::vector<ASTNode*> arguments = std::vector<ASTNode*>();

				identifier = parentNode->leafNodes.back();
				parentNode->leafNodes.pop_back();
				identifier->nodeType = Identifier_Node;
				argumentsNode->nodeType = Arguments;

				// Step through all following tokens until parens start
				std::vector<tokenPair> subTokens = std::vector<tokenPair>();
				for (;;) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair t = NEXT_TOKEN(i);

					if (t.second == Left_Paren)
						break;

					subTokens.push_back(t);
				}
				returnType = generateAST(subTokens, depth + 1);
				returnType->nodeType = Type;
				// Step through all following tokens until parens are closed
				int parenLevel = 1;
				subTokens = std::vector<tokenPair>();
				for (;;) {
					if (i >= tokens.size() - 1)
						break;
					tokenPair t = NEXT_TOKEN(i);

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
						arguments.push_back(newNode);
						subTokens = std::vector<tokenPair>();
						continue;
					}

					subTokens.push_back(t);
				}
				ASTNode* newNode = new ASTNode();
				generateAST(subTokens, depth + 1, newNode);
				newNode->nodeType = Expression_Term;
				arguments.push_back(newNode);
				subTokens = std::vector<tokenPair>();
				//arguments = generateAST(subTokens, depth + 1);
				//arguments->nodeType = Arguments;
				for (int a = 0; a < arguments.size(); a++) {
					//argumentsNode->childNodes.push_back(arguments[a]);
					if (arguments[a]->leafNodes.size() == 2) {	// Make sure follows: <type> <identifier>  pattern
						arguments[a]->leafNodes[0]->nodeType = Type;
						arguments[a]->childNodes.push_back(arguments[a]->leafNodes[0]);
						arguments[a]->childNodes.push_back(arguments[a]->leafNodes[1]);
						arguments[a]->leafNodes.pop_back();
						arguments[a]->leafNodes.pop_back();
					}
					else if (arguments[a]->leafNodes.size() == 0) {
					}
					else {
						printf("Error: Expected type followed by identifier\n");
						exit(1);
					}
					argumentsNode->childNodes.push_back(arguments[a]);
				}

				// Step through all tokens to gather body until braces are closed
				GATHER_SCOPE_BODY(subTokens, 0, i);

				bodyNode = generateAST(subTokens, depth + 1);
				bodyNode->nodeType = Scope_Body;

				node->token = "<" + identifier->token + ">";
				node->childNodes.push_back(identifier);
				if (returnType->childNodes.size() > 0)
					node->childNodes.push_back(returnType);
				node->childNodes.push_back(argumentsNode);
				node->childNodes.push_back(bodyNode);
				break;
			}

			case Left_Paren: {
				node->nodeType = Expression_Paren_Term;

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
					tokenPair t = NEXT_TOKEN(i);

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
						insideNodes.push_back(newNode);
						subTokens = std::vector<tokenPair>();
						continue;
					}

					subTokens.push_back(t);
				}
				ASTNode* newNode = new ASTNode();
				generateAST(subTokens, depth + 1, newNode);
				newNode->nodeType = Expression_Term;
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
					insideNodes[0]->nodeType = Expression_Term;
					//node = insideNodes[0];
					node->childNodes.push_back(insideNodes[0]);
				}
				if (isLeaf)
					goto addNodeAsLeaf;
				break;
			}

			case Number: {
				node->nodeType = Number_Node;
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
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case False_Literal: {
				node->nodeType = Boolean_Node;
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

			case Comment: {
				goto dontAddNodeForce;
			}

			case Identifier: {
				node->nodeType = Identifier_Node;
				parentNode->leafNodes.push_back(node);
				goto dontAddNode;
			}

				//case Semi_Colon: {
				//	for (int l = 0; l < parentNode->leafNodes.size(); l++) {
				//		parentNode->childNodes.push_back(parentNode->leafNodes[l]);
				//	}
				//	for (int l = 0; l < parentNode->leafNodes.size(); l++)
				//		parentNode->leafNodes.pop_back();
				//}

			default: {
				printf("Warning: Undefined node, %s\n", tokenValue.c_str());
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


void fixPrecedence(ASTNode* node)
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
				newRight->tokenType = node->tokenType;
				newRight->lineNumber = node->lineNumber;
				newRight->childNodes.push_back(B);
				newRight->childNodes.push_back(C);

				// Update current node to be: A op1 (B op2 C)
				node->nodeType = leftChild->nodeType;
				node->token = leftChild->token;
				node->tokenType = leftChild->tokenType;
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
				newLeft->tokenType = node->tokenType;
				newLeft->lineNumber = node->lineNumber;
				newLeft->childNodes.push_back(A);
				newLeft->childNodes.push_back(B);

				// Update current node to be: (A op1 B) op2 C
				node->nodeType = rightChild->nodeType;
				node->token = rightChild->token;
				node->tokenType = rightChild->tokenType;
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

inline void indent(int& depth)
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

	indent(depth);

	printf("%s", startNode->token.c_str());
	if (verbosity >= 3)
		if (startNode->token.size() > 0)
			printf(":L%d", startNode->lineNumber);
	printf(":(%s){", ASTNodeTypeAsString(startNode->nodeType).c_str());

	if (startNode->childNodes.size() > 0 || startNode->leafNodes.size() > 0)
		printf("\n");

	for (int c = 0; c < startNode->childNodes.size(); c++) {
		printAST(startNode->childNodes[c], depth + 1);
	}

	// Print unused leaf nodes
	depth++;
	if (startNode->leafNodes.size() > 0) {
		indent(depth);
		printf("!unusedLeafNodes!:{\n");
		for (int c = 0; c < startNode->leafNodes.size(); c++) {
			printAST(startNode->leafNodes[c], depth + 1);
		}
		indent(depth);
		printf("}\n");
	}
	depth--;

	if (startNode->childNodes.size() > 0 || startNode->leafNodes.size() > 0)
		indent(depth);
	printf("}\n");


	return 0;
}
