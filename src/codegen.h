#pragma once


#include "console.h"
#include "llvm.h"
#include "messagehandler.h"
#include "parser.h"
#include "pch.h"
#include "settings.h"


struct ASTNode;
struct argType;
typedef std::vector<argType> argumentList;
struct AsaFunctionDefinition;


extern std::unique_ptr<llvm::LLVMContext> llvmCompileContext;
extern std::unique_ptr<llvm::Module> llvmCompileModule;
extern std::unique_ptr<llvm::DIBuilder> llvmDebugBuilder;
extern std::unique_ptr<llvm::IRBuilder<>> llvmIRBuilder;
extern std::unordered_map<std::string, llvm::Value*> NamedValues;
extern std::unordered_map<std::string, bool> compilerDirectiveFlags;
extern std::unordered_map<std::string, bool> commandLineCompilerDirectiveFlags;
extern std::unordered_map<std::string, std::stack<ASTNode*>> compilerStacks;


bool isOptimizing();
llvm::Value* castValue(llvm::Value* value, llvm::Type* destType, bool isSrcSigned, bool isToSigned, ASTNode* node, bool destTypeIsStruct = false);
void initializeCodeGenerator();
void resetCodeGenerator();
int outputObjectFile(std::string& objectFilePath);
int generateExecutable(const std::string& irFilePath, const std::string& exeFilePath, const std::string& clangOptions);
void optimizeFunctions();
void printFunctionPrototypes();
void printFunctionDifferences(argumentList* arguments, AsaFunctionDefinition* other);
void printFunctionCandidate(AsaFunctionDefinition* fn);
llvm::Type* getLLVMTypeFromString(std::string typeName, int pointerLevelOffset, ASTNode* contextNode, bool& wasDefined, int& pass);
void CreateBuiltinAsaBaseTypes();

extern std::unordered_map<std::string, std::string> typeAliasMap;
std::string resolveTypeAlias(const std::string& name, int depth = 0);
void registerTypeAlias(const std::string& aliasName, const std::string& targetName);
bool areTypesEquivalent(const std::string& type1, const std::string& type2);
extern std::vector<std::string> linkedLibraries;
extern std::vector<std::string> linkedStaticLibraries;

extern llvm::Function* globalInitFn;
void declareModuleScopeVariable(ASTNode* exprStmtNode, ASTNode* ownerNode, bool isModuleVar);
void declareModuleScopeVariableFromColon(ASTNode* colonNode, ASTNode* ownerNode);
void processModuleForDeclarations(ASTNode* moduleCompilerDefineNode, std::string parentName = "");
bool finalizeGlobalInit();
void warnAboutUnusedVariables(ASTNode* rootNode);
