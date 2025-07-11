#include "output_builder.h"

std::vector<std::string> boilerPlate = std::vector<std::string>();
std::vector<std::string> prototypes = std::vector<std::string>();
std::vector<std::string> outputCode = std::vector<std::string>();

#define FIRST node->childNodes[0 + nodeOffset]
#define FIRST_TOKEN FIRST->token.first
#define SECOND node->childNodes[1 + nodeOffset]
#define SECOND_TOKEN SECOND->token.first
#define THIRD node->childNodes[2 + nodeOffset]
#define THIRD_TOKEN THIRD->token.first

#define ADD(s) outputCode.push_back(s)
#define ADDB(s) boilerPlate.push_back(s)
#define ADDP(s) prototypes.push_back(s)
#define PUSH(s) tempStack.push(s)
#define PUSHP(s) tempStack2.push(s)
#define POP()             \
	ADD(tempStack.top()); \
	tempStack.pop();
#define POPP()              \
	ADDP(tempStack2.top()); \
	tempStack2.pop();

void build(ASTNode*& node)
{
	int nodeOffset = 0;
	std::stack<std::string> tempStack = std::stack<std::string>();
	std::stack<std::string> tempStack2 = std::stack<std::string>();
	switch (node->nodeType) {
		case Compiler_Define_Function: {
			std::string prototype = "";
			if (FIRST_TOKEN == "main") {
				ADD("int main(){");
				ADDP("int main();");
			}
			// Handle any other function definitions
			else {
				// Add function name
				PUSH(FIRST_TOKEN);
				PUSHP(FIRST_TOKEN);

				if (SECOND->nodeType == Type) {
					ADD(SECOND->childNodes[0]->token.first);
					ADDP(SECOND->childNodes[0]->token.first);
					nodeOffset++;
				}
				else {
					ADD("void");
					ADDP("void");
				}

				// Actually add name
				POP();
				POPP();

				// Add arguments
				ADD("(");
				ADDP("(");
				for (int i = 0; i < SECOND->childNodes.size(); i++) {
					ASTNode* a = SECOND->childNodes[i];
					if (a->childNodes.size() == 2) {
						ADD(a->childNodes[0]->token.first);
						ADDP(a->childNodes[0]->token.first);
						ADD(a->childNodes[1]->token.first);
						ADDP(a->childNodes[1]->token.first);
						if (i < SECOND->childNodes.size() - 1) {
							ADD(",");
							ADDP(",");
						}
					}
					else if (a->childNodes.size() == 1) {
						ADD(a->childNodes[0]->token.first);
						ADDP(a->childNodes[0]->token.first);
						if (i < SECOND->childNodes.size() - 1) {
							ADDP(",");
						}
					}
				}
				ADD(")");
				ADDP(");");

				// Add scope body
				ADD("{");
			}

			ADD("}\n");
			break;
		}

		default:
			break;
	}
}

int buildOutput(ASTNode*& node)
{
	// Boilerplate
	ADDB(
		R"(
			// Boilerplate:
	)");
	ADDB("#include <iostream>");
	ADDB("#include <string>");
	ADDB("#include <vector>");
	ADDB("using namespace std;");
	ADDB("\n\n\n");

	// Traverse global scope tree
	for (int i = 0; i < node->childNodes.size(); i++) {
		build(node->childNodes[i]);
	}

	std::string outStr = "";
	for (const auto& s : boilerPlate)
		outStr += s + "\n";
	for (const auto& s : prototypes)
		outStr += s + "\n";
	outStr += "\n\n\n";
	for (const auto& s : outputCode)
		outStr += s + "\n";

	saveStringToFile(projectDirectory + "build/" + baseFileName + ".cpp", outStr);

	return 0;
}
