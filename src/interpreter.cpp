
#include "interpreter.h"

std::stack<ASTNode*> callStack = new std::stack<ASTNode*>();

void startTreeWalkExection(ASTNode*& node)
{
	while (true)
		runCycle();
}

void runCycle()
{
}
