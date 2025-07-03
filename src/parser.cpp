#include "parser.h"

int beginParse(const std::vector<std::pair<std::string, TokenType>>& tokens)
{
	return 0;
}

//std::vector<ASTNode> ASTNodes = std::vector<ASTNode>();

// Add leaf nodes here as they are still yet to be used.
std::stack<ASTNode*> unusedNodes = std::stack<ASTNode*>();

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
		//node.prevNode = &parentNode;
		std::string tokenValue = tokens[i].first;
		TokenType tokenType = tokens[i].second;
		int lineNumber = tokens[i].lineNumber;
		bool unusedNode = false;

		node->tokenType = tokenType;
		node->token = tokenValue;

		if (tokenType == If_Statement) {
			node->nodeType = If_Statement_Node;

			ASTNode* conditionNode = new ASTNode();
			ASTNode* bodyNode = new ASTNode();

			// Step through all tokens until parens are closed
			int parenLevel = 0;
			std::vector<tokenPair> subTokens = std::vector<tokenPair>();
			for (;;) {
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
			conditionNode->nodeType = If_Cond;

			// Step through all tokens to gather body until braces are closed
			subTokens = GATHER_SCOPE_BODY(0, i);

			bodyNode = generateAST(subTokens, depth + 1, true);
			bodyNode->nodeType = If_Body;

			node->childNodes.push_back(conditionNode);
			node->childNodes.push_back(bodyNode);
		}
		else if (tokenType == While_Statement) {
			node->nodeType = While_Statement_Node;

			ASTNode* conditionNode = new ASTNode();
			ASTNode* bodyNode = new ASTNode();

			// Step through all tokens until parens are closed
			int parenLevel = 0;
			std::vector<tokenPair> subTokens = std::vector<tokenPair>();
			for (;;) {
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
			conditionNode->nodeType = While_Cond;

			// Step through all tokens to gather body until braces are closed
			subTokens = GATHER_SCOPE_BODY(0, i);

			bodyNode = generateAST(subTokens, depth + 1, true);
			bodyNode->nodeType = While_Body;

			node->childNodes.push_back(conditionNode);
			node->childNodes.push_back(bodyNode);
		}
		else if (tokenType == For_Statement) {
			node->nodeType = For_Statement_Node;

			ASTNode* iteratorNode = new ASTNode();
			ASTNode* rangeNode = new ASTNode();
			ASTNode* bodyNode = new ASTNode();

			// Step through all tokens until parens are closed
			int parenLevel = 0;
			std::vector<tokenPair> subTokens = std::vector<tokenPair>();
			for (;;) {
				tokenPair t = NEXT_TOKEN(i);

				if (t.second == Left_Paren)
					parenLevel++;
				if (t.second == Right_Paren)
					parenLevel--;

				// If colon like =>  for(i : 0..10)
				// generate AST for i, and set iteratorNode as the output
				if (t.second == Colon) {
					iteratorNode = generateAST(subTokens, depth + 1, true);
					iteratorNode->nodeType = For_It;
					subTokens = std::vector<tokenPair>();  // Clear subtokens
					continue;
				}

				subTokens.push_back(t);

				if (parenLevel == 0 || t.second == EndOfLine)
					break;
			}
			rangeNode = generateAST(subTokens, depth + 1, true);
			rangeNode->nodeType = For_Range;

			// Step through all tokens to gather body until braces are closed
			subTokens = GATHER_SCOPE_BODY(0, i);

			bodyNode = generateAST(subTokens, depth + 1, true);
			bodyNode->nodeType = For_Body;

			// Only add iterator if one is explicity defined
			if (iteratorNode->nodeType == For_It)
				node->childNodes.push_back(iteratorNode);
			node->childNodes.push_back(rangeNode);
			node->childNodes.push_back(bodyNode);
		}
		else if (tokenType == Plus) {
			//continue;
			node->nodeType = Expression_Plus;

			ASTNode* firstTerm = new ASTNode();
			ASTNode* secondTerm = new ASTNode();

			// Instead of backtracking to get the first term, pop the unusedNodes vector =)
			if (unusedNodes.size() == 0) {
				std::cerr << "Error: `unusedNodes` variable was empty when it was expected to contain an element\nLine: " << lineNumber << std::endl;
				std::cerr << unusedNodes.top();

				//printf("\nTokens:\n");
				//for (int tok = 0; tok < tokens.size(); tok++) {
				//	if (tokens[tok].second != EndOfLine) {
				//		printf("%dT:%d: ", tok, tokens[tok].lineNumber);
				//		printf("[%s]\t[%s]\n", tokens[tok].first.c_str(), tokenAsString(tokens[tok].second).c_str());
				//	}
				//}
				throw;
			}
			firstTerm->childNodes.push_back(unusedNodes.top());
			unusedNodes.pop();
			firstTerm->nodeType = Expression_Term;

			// Step through all following tokens until parens are closed
			int parenLevel = 1;
			std::vector<tokenPair> subTokens = std::vector<tokenPair>();
			for (;;) {
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
				tokenPair t = NEXT_TOKEN(i);
				printf("token %d value: `%s`\n", i, t.first.c_str());

				if (t.second == Semi_Colon)
					break;
				if (t.second == EndOfLine)
					throw std::runtime_error("Error: Missing semicolon to specify end of line");


				printf("subtoken added of value: `%s`\n", t.first.c_str());
				//subTokens.push_back(t);
			}
			bodyNode = generateAST(subTokens, depth + 1, true);
			bodyNode->nodeType = Scope_Body;

			node->token = "<#" + identifier->token + ">";
			node->childNodes.push_back(identifier);
			node->childNodes.push_back(bodyNode);
		}
		else if (tokenType == Colon_Colon) {
			node->nodeType = Compiler_Define;

			ASTNode* identifier = new ASTNode();
			ASTNode* returnType = new ASTNode();
			ASTNode* arguments = new ASTNode();
			ASTNode* bodyNode = new ASTNode();

			identifier = unusedNodes.top();
			unusedNodes.pop();
			identifier->nodeType = Identifier_Node;

			// Step through all following tokens until parens start
			std::vector<tokenPair> subTokens = std::vector<tokenPair>();
			for (;;) {
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
			subTokens = GATHER_SCOPE_BODY(0, i);

			bodyNode = generateAST(subTokens, depth + 1, true);
			bodyNode->nodeType = Scope_Body;

			node->token = "<" + identifier->token + ">";
			node->childNodes.push_back(identifier);
			if (returnType->childNodes.size() > 0)
				node->childNodes.push_back(returnType);
			node->childNodes.push_back(arguments);
			node->childNodes.push_back(bodyNode);
		}
		else if (tokenType == Number) {
			node->nodeType = Number_Node;
			unusedNodes.push(node);
			goto dontAddNode;
		}
		else if (tokenType == String) {
			node->nodeType = String_Node;
			unusedNodes.push(node);
			goto dontAddNode;
		}
		else if (tokenType == True_Literal) {
			node->nodeType = Boolean_Node;
			unusedNodes.push(node);
			goto dontAddNode;
		}
		else if (tokenType == False_Literal) {
			node->nodeType = Boolean_Node;
			unusedNodes.push(node);
			goto dontAddNode;
		}
		else if (tokenType == Identifier) {
			node->nodeType = Identifier_Node;
			unusedNodes.push(node);
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
		printf("unusedNodes top: ");
		printf("%s ", unusedNodes.top()->token.c_str());
		printf("\n");
		// If it has a parent waiting for results, dont skip adding node
		if (hasParent) {
			unusedNodes.pop();
			printf("removed\n");
			goto addNode;
		}
	dontAddNodeForce:
		continue;
	}


	return parentNode;
}

void GO_BACK_TO_BEGINNING_OF_TERM(int& i)
{
	int PAREN_LEVEL = 1;
	for (;;) {
		tokenPair TOKENPAIR = tokens[--i];
		if (TOKENPAIR.second == Left_Paren || TOKENPAIR.second == Left_Bracket)
			PAREN_LEVEL--;
		if (TOKENPAIR.second == Right_Paren || TOKENPAIR.second == Right_Bracket)
			PAREN_LEVEL++;
		if (PAREN_LEVEL == 0 || TOKENPAIR.second == EndOfLine || TOKENPAIR.second == Equal)
			break;
	}
}

inline void indent(int& depth)
{
	for (int i = 0; i < depth; i++)
		printf("\t");
}

std::vector<tokenPair> GATHER_SCOPE_BODY(int braceLevel, int& i)
{
	std::vector<tokenPair> subTokens = std::vector<tokenPair>();
	for (;;) {
		tokenPair t = NEXT_TOKEN(i);

		if (t.second == Left_Brace)
			braceLevel++;
		if (t.second == Right_Brace)
			braceLevel--;

		subTokens.push_back(t);

		if (braceLevel == 0 || t.second == EndOfFile)
			break;
	}
	return subTokens;
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
	indent(depth);

	try {
		printf("%s:(%s){", startNode->token.c_str(), ASTNodeTypeAsString(startNode->nodeType).c_str());

		if (startNode->childNodes.size() > 0)
			printf("\n");

		for (int c = 0; c < startNode->childNodes.size(); c++) {
			printAST(startNode->childNodes[c], depth + 1);
		}
	}
	catch (std::exception& e) {
		printf(e.what());
	}

	if (startNode->childNodes.size() > 0)
		indent(depth);
	printf("}\n");

	return 0;
}
