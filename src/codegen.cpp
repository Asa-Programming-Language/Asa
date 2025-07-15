#include "codegen.h"


std::unique_ptr<LLVMContext> TheContext;
std::unique_ptr<Module> TheModule;
std::unique_ptr<IRBuilder<>> Builder;
std::map<std::string, Value*> NamedValues;
BasicBlock* prototypesBlock;

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

	prototypesBlock = BasicBlock::Create(*TheContext, "prototypes");
}

Value* findNamedValue(ASTNode* node, ASTNode* childNode, std::string& identifier)
{
	// First look in self
	if (node->namedValues.find(identifier) != node->namedValues.end())
		return node->namedValues[identifier];
	for (auto& c : node->childNodes) {
		if (node->depth > 0)	 // only go past use if in global scope
			if (c == childNode)	 // dont go past the node where it is used
				break;
		if (c->namedValues.find(identifier) != c->namedValues.end())
			return c->namedValues[identifier];
	}
	if (node->depth > 0)  // search the parent recursively until found or end of global scope is reached
		return findNamedValue(node->parentNode, node, identifier);

	return nullptr;
}

//bool findVariableDeclaration(ASTNode*& node, ASTNode*& childNode, std::string& identifier)
//{
//	for (auto& c : node->childNodes) {
//		if (node->depth > 0)	 // only go past use if in global scope
//			if (c == childNode)	 // dont go past the node where it is used
//				break;
//		if (c->nodeType == Expression_Statement && c->childNodes.size() > 0)
//			if (c->childNodes[0]->token.first == identifier)
//				return true;
//	}
//	if (node->depth > 0)  // search the parent recursively until found or end of global scope is reached
//		return findVariableDeclaration(node->parentNode, node, identifier);
//
//	return false;
//}

// Value*
void* ASTNode::generateConstant(int pass)
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
void* ASTNode::generateVariableExpression(int pass)
{
	// Look this variable up in the function.
	Value* V = findNamedValue(parentNode, this, token.first);
	//Value* V = NamedValues[token.first];
	if (!V) {
		printTokenError(token, "Undefined variable name");
		return nullptr;
	}
	return V;
}

void* ASTNode::generateReturn(int pass)
{
	ASTNode* exprNode = childNodes[0];
	if (exprNode->codegen == nullptr) {
		printTokenError(exprNode->token, "Node `" + ASTNodeTypeAsString(exprNode->nodeType) + "` does not have a code generator");
		return nullptr;
	}
	Value* RetVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	Builder->CreateRet(RetVal);
	return nullptr;
}

// Value*
void* ASTNode::generateExpression(int pass)
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
	Value* exprVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	return exprVal;
}

// Value*
void* ASTNode::generateExpressionStatement(int pass)
{
	ASTNode* identifierNode = childNodes[0];
	ASTNode* exprNode = childNodes[1];
	if (exprNode->codegen == nullptr) {
		printTokenError(exprNode->token, "Node `" + ASTNodeTypeAsString(exprNode->nodeType) + "` does not have a code generator");
		return nullptr;
	}
	Value* var = findNamedValue(parentNode, this, token.first);
	//Value* var = NamedValues[token.first];
	if (!var) {
		// Allocate memory TODO: Expand for any type
		Type* type = llvm::Type::getInt32Ty(*TheContext);
		var = Builder->CreateAlloca(type, nullptr, identifierNode->token.first);
	}
	// Set the value
	Value* exprVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	if (!exprVal) {
		printTokenError(token, "Set expression requires right argument");
		return nullptr;
	}
	Builder->CreateStore(exprVal, var);
	return exprVal;
}

// Value*
void* ASTNode::generateBinaryExpression(int pass)
{
	if (childNodes.size() == 0) {
		printTokenError(token, "Binary expression reqires a left and right argument");
		return nullptr;
	}
	if (childNodes[0]->codegen == nullptr) {
		printTokenError(childNodes[0]->token, "Node `" + ASTNodeTypeAsString(childNodes[0]->nodeType) + "` does not have a code generator");
		return nullptr;
	}
	if (childNodes[1]->codegen == nullptr) {
		printTokenError(childNodes[0]->token, "Node `" + ASTNodeTypeAsString(childNodes[1]->nodeType) + "` does not have a code generator");
		return nullptr;
	}
	Value* L = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
	Value* R = (Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);
	if (!L || !R)
		return nullptr;
	if (L->getType() != R->getType()) {
		printTokenError(token, "Left and right arguments of operator must be the same type");
		exit(1);
	}

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

		default:
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
	}

	return nullptr;
}

// Value*
void* ASTNode::generateScopeBody(int pass)
{
	for (auto& c : childNodes) {
		if (c->codegen != nullptr)
			Value* cCode = (Value*)(c->*(c->codegen))(pass);
		else
			printTokenError(c->token, "Node `" + ASTNodeTypeAsString(c->nodeType) + "` does not have a code generator");
	}

	return nullptr;
}

// Value*
void* ASTNode::generateCast(int pass)
{
	if (childNodes.size() < 2 || childNodes[1]->childNodes.size() == 0 || childNodes[1]->childNodes[0]->childNodes.size() == 0) {
		printTokenError(token, "Cast expression expected new type followed by name like: #cast float x;");
		exit(1);
	}
	Value* var = findNamedValue(parentNode, this, childNodes[1]->childNodes[0]->token.first);
	if (!var) {
		printTokenError(childNodes[1]->childNodes[0]->token, "Unknown variable name used");
		exit(1);
	}
	ASTNodeType oldType;
	var->getType()->dump();
	switch (var->getType()->getTypeID()) {
		case Type::IntegerTyID:
			oldType = Integer_Node;
			break;
		case Type::FloatTyID:
			oldType = Float_Node;
			break;
		default:
			break;
	}
	ASTNodeType newType;
	std::string tyVal = childNodes[1]->childNodes[0]->childNodes[0]->token.first;
	if (tyVal == "int")
		newType = Integer_Node;
	else if (tyVal == "float")
		newType = Float_Node;


	if (oldType == Integer_Node && newType == Float_Node)
		return Builder->CreateSIToFP(var, Builder->getDoubleTy());
	else if (oldType == Float_Node && newType == Integer_Node)
		return Builder->CreateFPToSI(var, Builder->getInt32Ty());
	else {
		printTokenError(childNodes[1]->childNodes[0]->token, "This compiler expression only works for builtin types.");
		exit(1);
	}

	return nullptr;
}

// Value*
void* ASTNode::generateCallExpression(int pass)
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
		ArgsV.push_back((Value*)(args[i]->*(args[i]->codegen))(pass));
		if (!ArgsV.back())
			return nullptr;
	}

	return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

// Function*
void* ASTNode::generatePrototype(int pass)
{
	// Don't add another prototype if already defined
	Function* theFunction = TheModule->getFunction(token.first);
	if (theFunction)
		return theFunction;

	std::vector<Type*> argTypes = std::vector<Type*>();
	ASTNode* argsNode = childNodes[2];
	ASTNode* modifiersNode = childNodes[3];
	std::vector<std::string> argNames = std::vector<std::string>();
	std::string fnName = token.first;
	bool variableNumArguments = false;
	for (auto& a : argsNode->childNodes) {
		if (a->childNodes.size() > 0) {
			if (a->childNodes[0]->nodeType != Argument_List) {
				std::string typeName = a->childNodes[0]->childNodes[0]->token.first;
				Type* aType = nullptr;
				if (typeName == "int")
					aType = Type::getInt32Ty(*TheContext);
				//aType = Type::getPrimitiveType(*TheContext, Type::IntegerTyID);
				else if (typeName == "float")
					aType = Type::getFloatTy(*TheContext);
				else if (typeName == "double")
					aType = Type::getDoubleTy(*TheContext);
				else
					goto invalidArgument;
				//else if (typeName == "string")
				//Type::getStringTy(*TheContext);
				argTypes.push_back(aType);
				argNames.push_back(a->childNodes[0]->token.first);
				std::cout << "Arg added: '" << a->childNodes[0]->token.first << "' of type: '" << typeName << "'\n";
				continue;
			invalidArgument:
				printTokenError(a->token, "Invalid argument given");
				exit(1);
			}
			// Handle ellipses ...
			else if (a->childNodes[0]->nodeType == Argument_List) {
				//std::string typeName = a->childNodes[0]->childNodes[0]->token.first;
				variableNumArguments = true;
			}
		}
	}
	bool isAlwaysInline = false;
	for (auto& m : modifiersNode->childNodes) {
		if (m->token.first == "#inline")
			isAlwaysInline = true;
	}
	Type* retType = Type::getVoidTy(*TheContext);
	ASTNode* typeNode = childNodes[1];
	if (typeNode->childNodes.size() > 0) {
		typeNode = typeNode->childNodes[0];
		if (typeNode->token.first == "int")
			retType = Type::getInt32Ty(*TheContext);
		else if (typeNode->token.first == "float")
			retType = Type::getFloatTy(*TheContext);
		else if (typeNode->token.first == "bool")
			retType = Type::getInt1Ty(*TheContext);
	}
	FunctionType* FT = FunctionType::get(retType, argTypes, variableNumArguments);

	Function* fn = Function::Create(FT, Function::ExternalLinkage, fnName, TheModule.get());
	if (isAlwaysInline)
		fn->addFnAttr(llvm::Attribute::AlwaysInline);

	unsigned Idx = 0;
	for (auto& arg : fn->args()) {
		arg.setName(argNames[Idx++]);
		namedValues[std::string(arg.getName())] = &arg;
	}

	return fn;
}

// Function*
void* ASTNode::generateFunction(int pass)
{
	// First, check for an existing function from a previous 'extern' declaration.
	Function* theFunction = TheModule->getFunction(token.first);

	if (!theFunction)
		theFunction = (Function*)generatePrototype();

	if (!theFunction)
		return nullptr;

	if (!theFunction->empty())
		return (Function*)LogErrorV("Function cannot be redefined.");

	if (pass == 0)
		return theFunction;

	// Create a new basic block to start insertion into.
	BasicBlock* fnBlock = BasicBlock::Create(*TheContext, "entry", theFunction);
	Builder->SetInsertPoint(fnBlock);

	//// Record the function arguments in the NamedValues map.
	////NamedValues.clear();
	//for (auto& arg : theFunction->args())
	//	namedValues[std::string(arg.getName())] = &arg;
	////NamedValues[std::string(Arg.getName())] = &Arg;

	ASTNode* body;
	for (auto& n : childNodes)
		if (n->nodeType == Scope_Body)
			body = n;

	if (body->codegen == nullptr) {
		printTokenError(body->token, "Node `" + ASTNodeTypeAsString(body->nodeType) + "` does not have a code generator");
		return nullptr;
	}

	(body->*(body->codegen))(pass);
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
