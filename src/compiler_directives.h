#pragma once

#include "parser.h"

void processCompilerDirectives(ASTNode*& node);
void pushCompilerDirectiveCall(const std::string& directiveName, ASTNode* sourceNode = nullptr);
void pushCompilerControlFlowResult(bool result, ASTNode* sourceNode = nullptr);
