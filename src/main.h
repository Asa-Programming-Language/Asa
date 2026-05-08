#ifndef MAIN_H
#define MAIN_H

#include "codegen.h"
#include "console.h"
#include "dependencies.h"
#include "filemanager.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "pch.h"
#include "settings.h"
#include "tests.h"
#include "tokenizer.h"

int verbosity = 2;
std::string optimizationLevel = "0";
int maxErrorTraceDepth = 1;
bool compact = false;


CompilerFlags compilerFlags = Flags_None;
WarningFlags warningFlags = W_None;

#endif
