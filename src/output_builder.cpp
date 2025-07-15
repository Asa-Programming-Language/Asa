#include "output_builder.h"

std::vector<std::string> boilerPlate = std::vector<std::string>();
std::vector<std::string> prototypes = std::vector<std::string>();
std::vector<std::string> fnPointers = std::vector<std::string>();
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
#define ADDF(s) fnPointers.push_back(s)

#define PUSH(s) tempStack[0].push(s)
#define POP()                \
	ADD(tempStack[0].top()); \
	tempStack[0].pop();
#define PUSHP(s) tempStack[1].push(s)
#define POPP()                \
	ADDP(tempStack[1].top()); \
	tempStack[1].pop();
#define PUSHF(s) tempStack[2].push(s)
#define POPF()                \
	ADDF(tempStack[2].top()); \
	tempStack[2].pop();

void build(ASTNode*& node)
{


	int nodeOffset = 0;
	std::stack<std::string> tempStack[3] = {std::stack<std::string>(), std::stack<std::string>(), std::stack<std::string>()};
	switch (node->nodeType) {
		case Compiler_Define_Function: {
			if (FIRST_TOKEN == "main") {
				ADD("int main(){");
				ADDP("int main();");
			}
			// Handle any other function definitions
			else {
				// Add function name
				PUSH(FIRST_TOKEN + "ORIGINAL");
				PUSHP(FIRST_TOKEN + "ORIGINAL");
				PUSHF(FIRST_TOKEN);

				if (SECOND->nodeType == Type_Node) {
					ADD(SECOND->childNodes[0]->token.first);
					ADDP(SECOND->childNodes[0]->token.first);
					ADDF(SECOND->childNodes[0]->token.first);
					nodeOffset++;
				}
				else {
					ADD("void");
					ADDP("void");
					ADDF("void");
				}

				// Actually add name
				POP();
				POPP();
				ADDF("(*");
				POPF();
				ADDF(")");

				// Add arguments
				ADD("(");
				ADDP("(");
				ADDF("(");
				for (int i = 0; i < SECOND->childNodes.size(); i++) {
					ASTNode* a = SECOND->childNodes[i];
					if (a->childNodes.size() == 2) {
						ADD(a->childNodes[0]->token.first);
						ADDP(a->childNodes[0]->token.first);
						ADDF(a->childNodes[0]->token.first);
						ADD(a->childNodes[1]->token.first);
						ADDP(a->childNodes[1]->token.first);
						if (i < SECOND->childNodes.size() - 1) {
							ADD(",");
							ADDP(",");
							ADDF(",");
						}
					}
					else if (a->childNodes.size() == 1) {
						ADD(a->childNodes[0]->token.first);
						ADDP(a->childNodes[0]->token.first);
						ADDF(a->childNodes[0]->token.first);
						if (i < SECOND->childNodes.size() - 1) {
							ADDP(",");
							ADDF(",");
						}
					}
				}
				ADD(")");
				ADDP(");");
				ADDF(");");

				// Add scope body
				ADD("{");
			}

			ADD("}\n");
			break;
		}

		case Function_Call: {
			// Add function name
			ADD(node->token.first);

			// Add arguments
			ADD("(");
			for (int i = 0; i < FIRST->childNodes[0]->childNodes.size(); i++) {
				ASTNode* a = FIRST->childNodes[0]->childNodes[i];
				ADD(a->token.first);
			}
			ADD(")");

			break;
		}

		default:
			break;
	}

	for (int i = 0; i < node->childNodes.size(); i++)
		build(node->childNodes[i]);
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

	//// Traverse global scope tree
	//for (int i = 0; i < node->childNodes.size(); i++) {
	//	build(node->childNodes[i]);
	//}
	build(node);

	std::string outStr = "";
	for (const auto& s : boilerPlate)
		outStr += s + "\n";
	outStr += "\n\n\n";
	for (const auto& s : prototypes)
		outStr += s + "\n";
	outStr += "\n\n\n";
	for (const auto& s : fnPointers)
		outStr += s + "\n";
	outStr += "\n\n\n";
	for (const auto& s : outputCode)
		outStr += s + "\n";

	saveStringToFile(projectDirectory + "build/" + baseFileName + ".cpp", outStr);

	return 0;
}
