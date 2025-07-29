#pragma once


#include "console.h"
#include "llvm.h"
#include "parser.h"
#include "pch.h"


using namespace llvm;
using namespace llvm::sys;


extern std::unique_ptr<LLVMContext> TheContext;
extern std::unique_ptr<Module> TheModule;
extern std::unique_ptr<IRBuilder<>> Builder;
extern std::map<std::string, Value*> NamedValues;


extern bool wasError;


void initializeCodeGenerator();
int outputObjectFile(std::string& objectFilePath);
int generateExecutable(const std::string& objectFilePath, const std::string& exeFilePath);
void removeUnusedPrototypes();
