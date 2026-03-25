#pragma once


#include "console.h"
#include "llvm.h"
#include "parser.h"
#include "pch.h"


using namespace llvm;
using namespace llvm::sys;

struct ASTNode;


extern std::unique_ptr<LLVMContext> TheContext;
extern std::unique_ptr<Module> TheModule;
extern std::unique_ptr<DIBuilder> DBuilder;
extern std::unique_ptr<IRBuilder<>> Builder;
extern std::map<std::string, Value*> NamedValues;


void initializeCodeGenerator();
int outputObjectFile(std::string& objectFilePath);
int generateExecutable(const std::string& irFilePath, const std::string& exeFilePath, const std::string& clangOptions);
void removeUnusedPrototypes();
void printFunctionPrototypes();

extern std::map<std::string, std::string> compilerDefines;
extern std::vector<std::string> linkedLibraries;
extern std::vector<std::string> linkedStaticLibraries;

extern Function* globalInitFn;
void declareModuleScopeVariable(ASTNode* exprStmtNode, ASTNode* ownerNode, bool isModuleVar);
void processModuleForDeclarations(ASTNode* moduleCompilerDefineNode, std::string parentName = "");
void finalizeGlobalInit();
