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


ASTNode* rootNode = new ASTNode();

ASTNode* generateAST(const std::vector<tokenPair>& tokens, int depth, bool hasParent)
{
	if (depth == MAX_AST_DEPTH) {
		printf("Error: Max AST Depth of %d Reached", MAX_AST_DEPTH);
		exit(1);
	}

	ASTNode* parentNode = new ASTNode();

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

		if (tokenType == If_Statement) {
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
			conditionNode = generateAST(subTokens, depth + 1, true);
			conditionNode->nodeType = Condition;

			// Step through all tokens to gather body until braces are closed
			GATHER_SCOPE_BODY(subTokens, 0, i);
			PRINT_SUBTOKENS(subTokens);

			bodyNode = generateAST(subTokens, depth + 1, true);
			bodyNode->nodeType = Scope_Body;

			node->childNodes.push_back(conditionNode);
			node->childNodes.push_back(bodyNode);
		}
		else if (tokenType == While_Statement) {
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
			conditionNode = generateAST(subTokens, depth + 1, true);
			conditionNode->nodeType = Condition;

			// Step through all tokens to gather body until braces are closed
			GATHER_SCOPE_BODY(subTokens, 0, i);

			bodyNode = generateAST(subTokens, depth + 1, true);
			bodyNode->nodeType = Scope_Body;

			node->childNodes.push_back(conditionNode);
			node->childNodes.push_back(bodyNode);
		}
		else if (tokenType == For_Statement) {
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
					iteratorNode = generateAST(subTokens, depth + 1, true);
					iteratorNode->nodeType = Iterator;
					subTokens = std::vector<tokenPair>();  // Clear subtokens
					continue;
				}

				subTokens.push_back(t);

				if (parenLevel == 0 || t.second == EndOfLine || t.second == Semi_Colon)
					break;
			}
			rangeNode = generateAST(subTokens, depth + 1, true);
			rangeNode->nodeType = Range_Node;

			// Step through all tokens to gather body until braces are closed
			GATHER_SCOPE_BODY(subTokens, 0, i);

			bodyNode = generateAST(subTokens, depth + 1, true);
			bodyNode->nodeType = Scope_Body;

			// Only add iterator if one is explicity defined
			if (iteratorNode->nodeType == Iterator)
				node->childNodes.push_back(iteratorNode);
			node->childNodes.push_back(rangeNode);
			node->childNodes.push_back(bodyNode);
		}
		else if (tokenType == Dot_Dot) {
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
			secondTerm = generateAST(subTokens, depth + 1, true);
			secondTerm->nodeType = Range_End;

			node->childNodes.push_back(firstTerm);
			node->childNodes.push_back(secondTerm);
		}
		else if (tokenType == Plus) {
			node->nodeType = Expression_Plus;

			ASTNode* firstTerm = new ASTNode();
			ASTNode* secondTerm = new ASTNode();

			// Instead of backtracking to get the first term, pop the leafNodes vector =)
			firstTerm->childNodes.push_back(parentNode->leafNodes.back());
			parentNode->leafNodes.pop_back();
			firstTerm->nodeType = Expression_Term;

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
			secondTerm = generateAST(subTokens, depth + 1, true);
			secondTerm->nodeType = Expression_Term;

			node->childNodes.push_back(firstTerm);
			node->childNodes.push_back(secondTerm);
		}
		else if (tokenType == Equal) {
			node->nodeType = Expression_Statement;

			ASTNode* firstTerm = new ASTNode();
			ASTNode* secondTerm = new ASTNode();

			// Instead of backtracking to get the first term, pop the leafNodes vector =)
			printAST(parentNode);
			firstTerm->childNodes.push_back(parentNode->leafNodes.back());
			parentNode->leafNodes.pop_back();
			firstTerm->nodeType = Identifier_Node;

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
			secondTerm = generateAST(subTokens, depth + 1, true);
			secondTerm->nodeType = Expression_Term;

			node->childNodes.push_back(firstTerm);
			node->childNodes.push_back(secondTerm);
		}
		else if (tokenType == Hash) {
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
			bodyNode = generateAST(subTokens, depth + 1, true);
			bodyNode->nodeType = Scope_Body;

			node->token = "<#" + identifier->token + ">";
			node->childNodes.push_back(identifier);
			node->childNodes.push_back(bodyNode);
		}
		else if (tokenType == Colon_Colon) {
			node->nodeType = Compiler_Define;
			printAST(parentNode);

			ASTNode* identifier = new ASTNode();
			ASTNode* returnType = new ASTNode();
			ASTNode* arguments = new ASTNode();
			ASTNode* bodyNode = new ASTNode();

			identifier = parentNode->leafNodes.back();
			parentNode->leafNodes.pop_back();
			identifier->nodeType = Identifier_Node;

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
			returnType = generateAST(subTokens, depth + 1, true);
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

				if (t.second == EndOfLine || t.second == Semi_Colon)
					break;

				subTokens.push_back(t);

				if (parenLevel == 0)
					break;
			}
			arguments = generateAST(subTokens, depth + 1, true);
			arguments->nodeType = Arguments;

			// Step through all tokens to gather body until braces are closed
			GATHER_SCOPE_BODY(subTokens, 0, i);

			bodyNode = generateAST(subTokens, depth + 1, true);
			bodyNode->nodeType = Scope_Body;

			node->token = "<" + identifier->token + ">";
			node->childNodes.push_back(identifier);
			if (returnType->childNodes.size() > 0)
				node->childNodes.push_back(returnType);
			node->childNodes.push_back(arguments);
			node->childNodes.push_back(bodyNode);
		}
		else if (tokenType == Left_Paren) {
			node->nodeType = Expression_Term;

			ASTNode* previousTerm = new ASTNode();
			std::vector<ASTNode*> insideNodes = std::vector<ASTNode*>();
			bool isFunction = false;

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

				if ((t.second == EndOfLine || t.second == Semi_Colon)) {
					break;
				}
				// If comma and parenLevel is in same scope
				if ((t.second == Comma && parenLevel == 1)) {
					ASTNode* newNode = new ASTNode();
					newNode = generateAST(subTokens, depth + 1, true);
					newNode->nodeType = Expression_Term;
					insideNodes.push_back(newNode);
					subTokens = std::vector<tokenPair>();
					continue;
				}

				subTokens.push_back(t);
			}
			ASTNode* newNode = new ASTNode();
			newNode = generateAST(subTokens, depth + 1, true);
			newNode->nodeType = Expression_Term;
			insideNodes.push_back(newNode);

			if (isFunction) {
				ASTNode* argumentsNode = new ASTNode();
				argumentsNode->nodeType = Arguments;
				for (int a = 0; a < insideNodes.size(); a++)
					argumentsNode->childNodes.push_back(insideNodes[a]);
				node->childNodes.push_back(argumentsNode);
			}
			else {
				insideNodes[0]->nodeType = Expression_Term;
				node->childNodes.push_back(insideNodes[0]);
			}
		}
		else if (tokenType == Number) {
			node->nodeType = Number_Node;
			parentNode->leafNodes.push_back(node);
			goto dontAddNode;
		}
		else if (tokenType == String) {
			node->nodeType = String_Node;
			parentNode->leafNodes.push_back(node);
			goto dontAddNode;
		}
		else if (tokenType == True_Literal) {
			node->nodeType = Boolean_Node;
			parentNode->leafNodes.push_back(node);
			goto dontAddNode;
		}
		else if (tokenType == False_Literal) {
			node->nodeType = Boolean_Node;
			parentNode->leafNodes.push_back(node);
			goto dontAddNode;
		}
		else if (tokenType == Comment) {
			goto dontAddNodeForce;
		}
		else if (tokenType == Identifier) {
			node->nodeType = Identifier_Node;
			parentNode->leafNodes.push_back(node);
			goto dontAddNode;
		}
		else {
			printf("Warning: Undefined node, %s\n", tokenValue.c_str());
			goto dontAddNodeForce;
		}

	addNode:
		parentNode->childNodes.push_back(node);
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
