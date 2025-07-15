#include "codegen.h"


std::unique_ptr<LLVMContext> TheContext;
std::unique_ptr<Module> TheModule;
std::unique_ptr<IRBuilder<>> Builder;
std::map<std::string, Value*> NamedValues;

Value* LogErrorV(const char* Str)
{
	printError(Str);
	return nullptr;
}

void initializeCodeGenerator()
{
	// Open a new context and module.
	TheContext = std::make_unique<LLVMContext>();
	TheModule = std::make_unique<Module>("asa_global", *TheContext);

	// Create a new builder for the module.
	Builder = std::make_unique<IRBuilder<>>(*TheContext);
}

// Value*
void* ASTNode::generateConstant()
{
	if (nodeType == Integer_Node)
		return ConstantInt::get(*TheContext, APInt(32, stoi(token.first), true));
	else if (nodeType == Boolean_Node)
		return ConstantInt::get(*TheContext, APInt(1, stoi(token.first), false));
	else if (nodeType == Float_Node)
		return ConstantFP::get(*TheContext, APFloat(stod(token.first)));

	return nullptr;
}

// Value*
void* ASTNode::generateVariableExpression()
{
	// Look this variable up in the function.
	Value* V = NamedValues[token.first];
	if (!V)
		return LogErrorV("Unknown variable name");
	return V;
}

// Value*
void* ASTNode::generateBinaryExpression()
{
	Value* L = (Value*)(childNodes[0]->*(childNodes[0]->codegen))();
	Value* R = (Value*)(childNodes[1]->*(childNodes[1]->codegen))();
	if (!L || !R)
		return nullptr;

	ASTNodeType t = childNodes[0]->nodeType;

	switch (t) {
		case Integer_Node:
			switch (nodeType) {
				case Expression_Plus:
					return Builder->CreateAdd(L, R, "addtmp");
				case Expression_Minus:
					return Builder->CreateSub(L, R, "subtmp");
				case Expression_Times:
					return Builder->CreateMul(L, R, "multmp");
				case Compare_Less:
					return Builder->CreateICmpULT(L, R, "cmptmp");
				default:
					return LogErrorV("Invalid binary operator");
			}

		case Float_Node:
			switch (nodeType) {
				case Expression_Plus:
					return Builder->CreateFAdd(L, R, "addtmp");
				case Expression_Minus:
					return Builder->CreateFSub(L, R, "subtmp");
				case Expression_Times:
					return Builder->CreateFMul(L, R, "multmp");
				case Compare_Less:
					return Builder->CreateFCmpULT(L, R, "cmptmp");
				default:
					return LogErrorV("Invalid binary operator");
			}
	}

	return nullptr;
}

// Value*
void* ASTNode::generateScopeBody()
{
	for (auto& c : childNodes) {
		if (c->codegen != nullptr)
			Value* cCode = (Value*)(c->*(c->codegen))();
		else
			printError("Node `" + ASTNodeTypeAsString(c->nodeType) + "` does not have a code generator");
	}

	return nullptr;
}

// Value*
void* ASTNode::generateCallExpression()
{
	// Look up the name in the global module table.
	Function* CalleeF = TheModule->getFunction(token.first);
	if (!CalleeF)
		return LogErrorV("Unknown function referenced");

	ASTNode* argsNode = childNodes[0];
	std::vector<ASTNode*> args = std::vector<ASTNode*>();
	for (auto& a : argsNode->childNodes)
		if (a->childNodes.size() > 0)
			args.push_back(a);

	// If argument mismatch error.
	if (CalleeF->arg_size() != args.size())
		return LogErrorV("Incorrect number of arguments passed");

	std::vector<Value*> ArgsV;
	for (unsigned i = 0, e = args.size(); i != e; ++i) {
		ArgsV.push_back((Value*)(args[i]->*(args[i]->codegen))());
		if (!ArgsV.back())
			return nullptr;
	}

	return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

// Function*
void* ASTNode::generatePrototype()
{
	std::vector<Type*> ArgTypes = std::vector<Type*>();
	ASTNode* argsNode = childNodes[0];
	std::vector<std::string> argNames = std::vector<std::string>();
	std::string fnName = token.first;
	for (auto& a : argsNode->childNodes) {
		if (a->childNodes.size() == 2) {
			std::string typeName = a->childNodes[0]->token.first;
			argNames.push_back(a->childNodes[1]->token.first);
			if (typeName == "int")
				Type::getPrimitiveType(*TheContext, Type::IntegerTyID);
			else if (typeName == "float")
				Type::getFloatTy(*TheContext);
			else if (typeName == "double")
				Type::getDoubleTy(*TheContext);
			//else if (typeName == "string")
			//Type::getStringTy(*TheContext);
		}
		// Handle ellipses ...
		else if (a->childNodes.size() == 1) {
			std::string typeName = a->childNodes[0]->token.first;
		}
	}
	FunctionType* FT = FunctionType::get(Type::getDoubleTy(*TheContext), ArgTypes, false);

	Function* F = Function::Create(FT, Function::ExternalLinkage, fnName, TheModule.get());

	unsigned Idx = 0;
	for (auto& Arg : F->args())
		Arg.setName(argNames[Idx++]);

	return F;
}

// Function*
void* ASTNode::generateFunction()
{
	// First, check for an existing function from a previous 'extern' declaration.
	Function* theFunction = TheModule->getFunction(token.first);

	if (!theFunction)
		theFunction = (Function*)generatePrototype();

	if (!theFunction)
		return nullptr;

	if (!theFunction->empty())
		return (Function*)LogErrorV("Function cannot be redefined.");

	// Create a new basic block to start insertion into.
	BasicBlock* BB = BasicBlock::Create(*TheContext, "entry", theFunction);
	Builder->SetInsertPoint(BB);

	// Record the function arguments in the NamedValues map.
	NamedValues.clear();
	for (auto& Arg : theFunction->args())
		NamedValues[std::string(Arg.getName())] = &Arg;

	ASTNode* body;
	for (auto& n : childNodes)
		if (n->nodeType == Scope_Body)
			body = n;

	if (body->codegen == nullptr) {
		printError("Node `" + ASTNodeTypeAsString(body->nodeType) + "` does not have a code generator");
	}

	else if (Value* RetVal = (Value*)(body->*(body->codegen))()) {
		// Finish off the function.
		Builder->CreateRet(RetVal);

		// Validate the generated code, checking for consistency.
		verifyFunction(*theFunction);

		return theFunction;
	}

	// Error reading body, remove function.
	theFunction->eraseFromParent();

	return nullptr;
}
