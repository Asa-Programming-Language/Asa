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

	printTokenError(token, "Value could not be parsed as constant");
	return nullptr;
}

// Value*
void* ASTNode::generateVariableExpression()
{
	// Look this variable up in the function.
	Value* V = NamedValues[token.first];
	if (!V) {
		printTokenError(token, "Undefined variable name");
		return nullptr;
	}
	return V;
}

void* ASTNode::generateReturn()
{
	ASTNode* exprNode = childNodes[0];
	if (exprNode->codegen == nullptr) {
		printTokenError(exprNode->token, "Node `" + ASTNodeTypeAsString(exprNode->nodeType) + "` does not have a code generator");
		return nullptr;
	}
	Value* RetVal = (Value*)(exprNode->*(exprNode->codegen))();
	Builder->CreateRet(RetVal);
	return nullptr;
}

// Value*
void* ASTNode::generateExpression()
{
	if (childNodes.size() == 0)
		return nullptr;
	ASTNode* exprNode = childNodes[0];
	if (exprNode->codegen == nullptr) {
		printTokenError(exprNode->token, "Node `" + ASTNodeTypeAsString(exprNode->nodeType) + "` does not have a code generator");
		return nullptr;
	}
	//if (exprNode->childNodes.size() == 0)
	//	return nullptr;
	Value* exprVal = (Value*)(exprNode->*(exprNode->codegen))();
	return exprVal;
}

// Value*
void* ASTNode::generateExpressionStatement()
{
	ASTNode* identifierNode = childNodes[0];
	ASTNode* exprNode = childNodes[1];
	if (exprNode->codegen == nullptr) {
		printTokenError(exprNode->token, "Node `" + ASTNodeTypeAsString(exprNode->nodeType) + "` does not have a code generator");
		return nullptr;
	}
	Value* var = NamedValues[token.first];
	if (!var) {
		// Allocate memory TODO: Expand for any type
		Type* type = llvm::Type::getInt32Ty(*TheContext);
		var = Builder->CreateAlloca(type, nullptr, identifierNode->token.first);
	}
	// Set the value
	Value* exprVal = (Value*)(exprNode->*(exprNode->codegen))();
	if (!exprVal) {
		printTokenError(token, "Set expression requires right argument");
		return nullptr;
	}
	Builder->CreateStore(exprVal, var);
	return exprVal;
}

// Value*
void* ASTNode::generateBinaryExpression()
{
	if (childNodes.size() == 0)
		return nullptr;
	if (childNodes[0]->codegen == nullptr) {
		printTokenError(childNodes[0]->token, "Node `" + ASTNodeTypeAsString(childNodes[0]->nodeType) + "` does not have a code generator");
		return nullptr;
	}
	if (childNodes[1]->codegen == nullptr) {
		printTokenError(childNodes[0]->token, "Node `" + ASTNodeTypeAsString(childNodes[1]->nodeType) + "` does not have a code generator");
		return nullptr;
	}
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
			printTokenError(c->token, "Node `" + ASTNodeTypeAsString(c->nodeType) + "` does not have a code generator");
	}

	return nullptr;
}

// Value*
void* ASTNode::generateCallExpression()
{
	// Look up the name in the global module table.
	Function* CalleeF = TheModule->getFunction(token.first);
	if (!CalleeF) {
		printTokenError(token, "Undefined function");
		return nullptr;
	}

	ASTNode* argsNode = childNodes[0];
	std::vector<ASTNode*> args = std::vector<ASTNode*>();
	for (auto& a : argsNode->childNodes)
		if (a->childNodes.size() > 0)
			args.push_back(a);

	// If argument mismatch error.
	if (CalleeF->arg_size() != args.size()) {
		printTokenError(token, "Incorrect number of arguments passed to function");
		return nullptr;
	}

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
	ASTNode* argsNode = childNodes[childNodes.size() == 1 ? 0 : 1];
	std::vector<std::string> argNames = std::vector<std::string>();
	std::string fnName = token.first;
	bool variableNumArguments = false;
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
			variableNumArguments = true;
		}
	}
	Type* retType = Type::getVoidTy(*TheContext);
	ASTNode* typeNode = childNodes[0];
	if (typeNode->nodeType == Type_Node) {
		if (typeNode->token.first == "int")
			retType = Type::getInt32Ty(*TheContext);
		else if (typeNode->token.first == "float")
			retType = Type::getFloatTy(*TheContext);
		else if (typeNode->token.first == "bool")
			retType = Type::getInt1Ty(*TheContext);
	}
	FunctionType* FT = FunctionType::get(retType, ArgTypes, variableNumArguments);

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
		printTokenError(body->token, "Node `" + ASTNodeTypeAsString(body->nodeType) + "` does not have a code generator");
		return nullptr;
	}

	(body->*(body->codegen))();
	//else if (Value* RetVal = (Value*)(body->*(body->codegen))()) {
	//	// Finish off the function.
	//	Builder->CreateRet(RetVal);

	// Validate the generated code, checking for consistency.
	verifyFunction(*theFunction);

	return theFunction;
	//}

	// Error reading body, remove function.
	theFunction->eraseFromParent();
	printTokenError(token, "Function is missing a return statement");

	return nullptr;
}
