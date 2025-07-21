#include "codegen.h"


std::unique_ptr<LLVMContext> TheContext;
std::unique_ptr<Module> TheModule;
std::unique_ptr<IRBuilder<>> Builder;
static std::unique_ptr<FunctionPassManager> TheFPM;
static std::unique_ptr<LoopAnalysisManager> TheLAM;
static std::unique_ptr<FunctionAnalysisManager> TheFAM;
static std::unique_ptr<CGSCCAnalysisManager> TheCGAM;
static std::unique_ptr<ModuleAnalysisManager> TheMAM;
static std::unique_ptr<PassInstrumentationCallbacks> ThePIC;
static std::unique_ptr<StandardInstrumentations> TheSI;

std::unordered_map<std::string, bool> typeSigns = {
	{"int128", true},
	{"int64", true},
	{"int32", true},
	{"int", true},
	{"int16", true},
	{"int8", true},

	{"uint128", false},
	{"uint64", false},
	{"uint32", false},
	{"uint", false},
	{"uint16", false},
	{"uint8", false},

	{"char", false},
	{"uchar", false},
	{"bool", false},

	{"double", false},
	{"float", false},
	{"half", false},
};

typedef std::vector<std::pair<std::string, ASTNodeType>> argumentList;

struct functionID {
	std::string name = "";
	std::string mangledName = "";
	std::string returnType = "";
	argumentList arguments = argumentList();
	Function* fnValue = nullptr;
	functionID() {}
	functionID(std::string n, std::string mN, std::string r, argumentList a, Function* f)
	{
		name = n;
		mangledName = mN;
		returnType = r;
		arguments = a;
		fnValue = f;
	}
	uint16_t compareMatch(std::string n, argumentList a)
	{
		uint16_t differences = 0;
		if (n != name)
			return 1000;
		if (arguments.size() != a.size())
			return 1000 - 1;
		for (int i = 0; i < arguments.size(); i++) {
			ASTNodeType t1 = arguments[i].second;
			ASTNodeType t2 = a[i].second;
			// If t1 is an integer type, make sure t2 is also
			// Difference points are given the further the types are
			if (t1 >= Integer_Node && t1 <= Boolean_Node) {
				if (t2 >= Integer_Node && t2 <= Boolean_Node)  // If similar type
					differences += abs(t1 - t2);
				else
					differences += Boolean_Node - Integer_Node;
				// TODO: also give points if there exists a cast function
			}
			else if (t1 >= Float_Node && t1 <= Half_Type) {
				if (t2 >= Float_Node && t2 <= Half_Type)  // If similar type
					differences += abs(t1 - t2);
				else
					differences += Half_Type - Float_Node;
				// TODO: also give points if there exists a cast function
			}
			else if (t1 == t2)
				differences += 0;
		}
		return differences;
	}
	uint16_t compareMatch(std::string n, std::vector<ASTNode*> a)
	{
		uint16_t differences = 0;
		if (n != name)
			return 1000;
		if (arguments.size() != a.size())
			return 1000 - 1;
		//for (int i = 0; i < arguments.size(); i++) {
		//	ASTNodeType t1 = arguments[i].second;
		//	ASTNodeType t2 = a[i].second;
		//	// If t1 is an integer type, make sure t2 is also
		//	// more points for similarity are given the closer the types are
		//	if (t1 >= Integer_Node && t1 <= Boolean_Node) {
		//		if (t2 >= Integer_Node && t2 <= Boolean_Node)
		//			total += (Boolean_Node - Integer_Node) - abs(t1 - t2);
		//		// TODO: also give points if there exists a cast function
		//	}
		//	else if (t1 >= Float_Node && t1 <= Half_Type) {
		//		if (t2 >= Float_Node && t2 <= Half_Type)
		//			total += (Half_Type - Float_Node) - abs(t1 - t2);
		//	}
		//	else if (t1 == t2)
		//		total += 1;
		//}
		return differences;
	}
	uint16_t compareMatch(std::string n)
	{
		uint16_t differences = 0;
		if (n != name)
			return 1000;
		return 1000 - 1;
	}
};

std::vector<functionID> functionIDs = std::vector<functionID>();

Function* getFunctionFromID(std::string& name, argumentList& arguments)
{
	functionID best;
	int bestScore = 1000;
	for (auto& f : functionIDs) {
		uint16_t score = f.compareMatch(name, arguments);
		if (score < bestScore) {
			best = f;
			bestScore = score;
		}
	}
	if (bestScore < 1000)
		return best.fnValue;
	return nullptr;
}
Function* getExactFunctionFromID(std::string& name, argumentList& arguments)
{
	functionID best;
	int bestScore = 1000;
	for (auto& f : functionIDs) {
		uint16_t score = f.compareMatch(name, arguments);
		if (score < bestScore) {
			best = f;
			bestScore = score;
		}
	}
	if (bestScore == 0)
		return best.fnValue;
	return nullptr;
}
Function* getFunctionFromID(std::string& name, std::vector<ASTNode*>& argValues)
{
	functionID best;
	int bestScore = 1000;
	for (auto& f : functionIDs) {
		uint16_t score = f.compareMatch(name, argValues);
		if (score < bestScore) {
			best = f;
			bestScore = score;
		}
	}
	if (bestScore < 1000)
		return best.fnValue;
	return nullptr;
}
Function* getFunctionFromID(std::string& name)
{
	functionID best;
	int bestScore = 1000;
	for (auto& f : functionIDs) {
		uint16_t score = f.compareMatch(name);
		if (score < bestScore) {
			best = f;
			bestScore = score;
		}
	}
	if (bestScore < 1000)
		return best.fnValue;
	return nullptr;
}

Value* LogErrorV(const char* Str)
{
	console::PrintError(Str);
	return nullptr;
}

/// CreateEntryBlockAlloca - Create an alloca instruction in the entry block of
/// the function.  This is used for mutable variables etc.
static AllocaInst* CreateEntryBlockAlloca(Function* TheFunction, Type* t, StringRef VarName)
{
	IRBuilder<> TmpB(&TheFunction->getEntryBlock(), TheFunction->getEntryBlock().begin());
	return TmpB.CreateAlloca(t, nullptr, VarName);
}

Type* getLLVMTypeFromString(std::string typeName)
{
	Type* aType;
	// Integer types
	if (typeName == "int" || typeName == "int32" || typeName == "uint" || typeName == "uint32")
		aType = Type::getInt32Ty(*TheContext);
	else if (typeName == "int16" || typeName == "uint16")
		aType = Type::getInt16Ty(*TheContext);
	else if (typeName == "int8" || typeName == "uint8" || typeName == "uchar" || typeName == "char")
		aType = Type::getInt8Ty(*TheContext);
	else if (typeName == "int64" || typeName == "uint64")
		aType = Type::getInt64Ty(*TheContext);
	else if (typeName == "int128" || typeName == "uint128")
		aType = Type::getInt128Ty(*TheContext);

	// Floats
	else if (typeName == "float")
		aType = Type::getFloatTy(*TheContext);
	else if (typeName == "half")
		aType = Type::getHalfTy(*TheContext);
	else if (typeName == "double")
		aType = Type::getDoubleTy(*TheContext);

	// Bool
	else if (typeName == "bool")
		aType = Type::getInt1Ty(*TheContext);

	else {
		throw 1;
	}
	return aType;
}

void initializeCodeGenerator()
{
	// Open a new context and module.
	TheContext = std::make_unique<LLVMContext>();
	TheModule = std::make_unique<Module>("asa_global", *TheContext);

	// Create a new builder for the module.
	Builder = std::make_unique<IRBuilder<>>(*TheContext);

	// Create new pass and analysis managers.
	TheFPM = std::make_unique<FunctionPassManager>();
	TheLAM = std::make_unique<LoopAnalysisManager>();
	TheFAM = std::make_unique<FunctionAnalysisManager>();
	TheCGAM = std::make_unique<CGSCCAnalysisManager>();
	TheMAM = std::make_unique<ModuleAnalysisManager>();
	ThePIC = std::make_unique<PassInstrumentationCallbacks>();
	TheSI = std::make_unique<StandardInstrumentations>(*TheContext, /*DebugLogging*/ true);
	TheSI->registerCallbacks(*ThePIC, TheMAM.get());

	// Add transform passes.
	// Do simple "peephole" optimizations and bit-twiddling optzns.
	TheFPM->addPass(InstCombinePass());
	// Reassociate expressions.
	TheFPM->addPass(ReassociatePass());
	// Eliminate Common SubExpressions.
	TheFPM->addPass(GVNPass());
	// Simplify the control flow graph (deleting unreachable blocks, etc).
	TheFPM->addPass(SimplifyCFGPass());

	// Register analysis passes used in these transform passes.
	PassBuilder PB;
	PB.registerModuleAnalyses(*TheMAM);
	PB.registerFunctionAnalyses(*TheFAM);
	PB.crossRegisterProxies(*TheLAM, *TheFAM, *TheCGAM, *TheMAM);
}

Value* findNamedValue(ASTNode* node, ASTNode* childNode, std::string& identifier)
{
	// First look in self
	if (node->namedValues.find(identifier) != node->namedValues.end())
		return node->namedValues[identifier];
	if (node->depth > 0)  // Dont look in child nodes of global TODO: organize vars better than this
		for (auto& c : node->childNodes) {
			if (node->depth > 0)	 // only go past position it is used if in global scope
				if (c == childNode)	 // otherwise dont go past the node where it is used
					break;
			if (c->namedValues.find(identifier) != c->namedValues.end())
				return c->namedValues[identifier];
		}
	if (node->depth > 0)  // search recursively upward until found or end of global scope is reached
		return findNamedValue(node->parentNode, node, identifier);

	return nullptr;
}


llvm::Value* castValue(llvm::Value* value, llvm::Type* destType, bool isSrcSigned, bool isToSigned, tokenPair& token)
{
	llvm::Type* srcType = value->getType();
	if (srcType == destType)
		return value;

	if (srcType->isIntegerTy() && destType->isIntegerTy())
		return Builder->CreateIntCast(value, destType, isSrcSigned);

	if (srcType->isIntegerTy() && destType->isFloatingPointTy())
		return isSrcSigned ? Builder->CreateSIToFP(value, destType)
						   : Builder->CreateUIToFP(value, destType);

	if (srcType->isFloatingPointTy() && destType->isIntegerTy())
		return isToSigned ? Builder->CreateFPToSI(value, destType)
						  : Builder->CreateFPToUI(value, destType);

	if (srcType->isFloatingPointTy() && destType->isFloatingPointTy())
		return Builder->CreateFPCast(value, destType);

	if (srcType->isPointerTy() && destType->isPointerTy())
		return Builder->CreatePointerCast(value, destType);

	if (srcType->isPointerTy() && destType->isIntegerTy())
		return Builder->CreatePtrToInt(value, destType);

	if (srcType->isIntegerTy() && destType->isPointerTy())
		return Builder->CreateIntToPtr(value, destType);

	// Use bitcast only if size matches and none of the above applies
	if (llvm::CastInst::isBitOrNoopPointerCastable(srcType, destType, TheModule->getDataLayout()))
		return Builder->CreateBitCast(value, destType);


	printTokenError(token, "Unsupported cast");

	srcType->print(llvm::outs());
	destType->print(llvm::outs());
	exit(1);
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
		return ConstantInt::get(*TheContext, APInt(1, token.first == "true" ? 1 : 0, false));
	else if (nodeType == Float_Node)
		return ConstantFP::get(*TheContext, APFloat(stod(token.first)));

	printTokenError(token, "Value could not be parsed as constant");
	return nullptr;
}

// Value*
void* ASTNode::generateVariableExpression(int pass)
{
	// Look this variable up in the function.
	AllocaInst* A = (AllocaInst*)findNamedValue(parentNode, this, token.first);
	if (!A) {
		printTokenError(token, "Undefined variable name");
		exit(1);
	}

	return Builder->CreateLoad(A->getAllocatedType(), A, token.first + "_load");
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
	Function* theFunction = Builder->GetInsertBlock()->getParent();
	if (exprNode->codegen == nullptr) {
		printTokenError(exprNode->token, "Node `" + ASTNodeTypeAsString(exprNode->nodeType) + "` does not have a code generator");
		return nullptr;
	}
	Value* var = findNamedValue(parentNode, this, identifierNode->token.first);

	ASTNode* typeNode;
	Type* type = nullptr;
	int pointerLevel = 0;
	if (identifierNode->childNodes.size() > 0) {
		typeNode = identifierNode->childNodes[0];
		if (typeNode->token.first == "*") {
			pointerLevel = typeNode->token.first.size();
			typeNode = typeNode->childNodes[0];
		}
		type = getLLVMTypeFromString(typeNode->token.first);
		for (int pL = 0; pL < pointerLevel; pL++)
			type = type->getPointerTo();
	}

	// Get the value
	Value* exprVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	if (!exprVal) {
		printTokenError(token, "Set expression requires right argument");
		exit(1);
	}
	// Automatically resolve type from expression if not already set
	if (type == nullptr) {
		type = exprVal->getType();
	}
	// Otherwise, the type is explicit, and builtin types should be cast automatically
	else {
		exprVal = castValue(exprVal, type, true, typeSigns[typeNode->token.first], exprNode->token);
	}

	if (exprVal->getType() != type) {
		printTokenError(token, "Type mismatch in set expression");
		exprVal->getType()->print(llvm::outs());
		type->print(llvm::outs());
		//printTokenError(token, "Type mismatch in set expression.\nTypes are \"" + exprVal->getType()->getAsString() + "\" and \"" + type->getAsString() + "\"");
		exit(1);
	}
	if (!var) {
		AllocaInst* Alloca = CreateEntryBlockAlloca(theFunction, type, identifierNode->token.first);
		Builder->CreateStore(exprVal, Alloca);
		namedValues[identifierNode->token.first] = Alloca;
	}
	else
		Builder->CreateStore(exprVal, var);
	//Builder->CreateStore(exprVal, var);
	return exprVal;
}

// Value*
void* ASTNode::generateIterator(int pass)
{
	ASTNode* identifierNode = childNodes[0];
	Value* var = findNamedValue(parentNode, this, token.first);
	if (!var) {
		Type* type = llvm::Type::getInt32Ty(*TheContext);
		var = Builder->CreateAlloca(type, nullptr, identifierNode->token.first);
	}
	return var;
}

// Value*
void* ASTNode::generateUnaryExpression(int pass)
{
	if (childNodes.size() == 0) {
		printTokenError(token, "Unary expression reqires an argument");
		return nullptr;
	}
	if (childNodes[0]->codegen == nullptr) {
		printTokenError(childNodes[0]->token, "Node `" + ASTNodeTypeAsString(childNodes[0]->nodeType) + "` does not have a code generator");
		return nullptr;
	}
	Value* R = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
	if (!R)
		return nullptr;

	ASTNodeType t = childNodes[0]->nodeType;

	switch (nodeType) {
		case Address_Of_Operation: {
			ASTNode* varNode = childNodes[0];
			Value* var = findNamedValue(parentNode, this, varNode->token.first);
			if (!var) {
				printTokenError(token, "Unknown variable name for address-of");
				exit(1);
			}
			return var;
		}

		case Dereference_Operation: {
			ASTNode* ptrNode = childNodes[0];
			Value* ptrVal = (Value*)(ptrNode->*(ptrNode->codegen))(pass);
			if (!ptrVal) {
				printTokenError(token, "Dereference of null pointer");
				exit(1);
			}
			// Load the value from the pointer
			Type* elementType = Type::getInt32Ty(*TheContext);	// TODO: whatever type is appropriate
			return Builder->CreateLoad(elementType, ptrVal, "deref_tmp");
		}

		default:
			printTokenError(token, "Unknown or undefined operator");
			exit(1);
	}

	switch (t) {
		//	case Integer_Node:
		//		switch (nodeType) {
		//			case Expression_Plus:
		//				return Builder->CreateAdd(L, R, "addtmp");
		//			case Expression_Minus:
		//				return Builder->CreateSub(L, R, "subtmp");
		//			case Expression_Times:
		//				return Builder->CreateMul(L, R, "multmp");
		//			case Compare_Less:
		//				return Builder->CreateICmpULT(L, R, "cmptmp");
		//			default:
		//				printTokenError(childNodes[0]->token, "Unknown unary operator \"" + childNodes[0]->token.first + "\"");
		//				exit(1);
		//		}

		//	case Float_Node:
		//		switch (nodeType) {
		//			case Expression_Plus:
		//				return Builder->CreateFAdd(L, R, "addtmp");
		//			case Expression_Minus:
		//				return Builder->CreateFSub(L, R, "subtmp");
		//			case Expression_Times:
		//				return Builder->CreateFMul(L, R, "multmp");
		//			case Compare_Less:
		//				return Builder->CreateFCmpULT(L, R, "cmptmp");
		//			default:
		//				printTokenError(childNodes[0]->token, "Unknown unary operator \"" + childNodes[0]->token.first + "\"");
		//				exit(1);
		//		}

		//	default:
		//		switch (nodeType) {
		//			case Expression_Plus:
		//				return Builder->CreateAdd(L, R, "addtmp");
		//			case Expression_Minus:
		//				return Builder->CreateSub(L, R, "subtmp");
		//			case Expression_Times:
		//				return Builder->CreateMul(L, R, "multmp");
		//			case Compare_Less:
		//				return Builder->CreateICmpULT(L, R, "cmptmp");
		//			default:
		//				printTokenError(childNodes[0]->token, "Unknown unary operator \"" + childNodes[0]->token.first + "\"");
		//				exit(1);
		//		}
	}

	return nullptr;
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

	bool operatorOverloaded = nodeType == Redefined_Operator_Expr;
	std::string operatorOverloadName = "binary." + tokenAsString(token.second);
	Function* CalleeF = nullptr;

	// Check to see if the operator actually has an overload
	if ((CalleeF = getFunctionFromID(operatorOverloadName)) != nullptr)
		operatorOverloaded = true;
	else if (operatorOverloaded) {	// If it was expected to,
		printTokenError(token, "Binary operator \"" + token.first + "\" between values");
		exit(1);
	}

	// If this is an operator overload, create function call to it
	if (operatorOverloaded) {
		if (!CalleeF) {
			printTokenError(token, "Undefined function");
			return nullptr;
		}

		std::vector<Value*> ArgsV;
		ArgsV.push_back(L);
		ArgsV.push_back(R);

		return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
	}
	// Otherwise, it is a regular builtin operator
	else
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
						printTokenError(childNodes[0]->token, "Unknown binary operator \"" + childNodes[0]->token.first + "\"");
						exit(1);
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
						printTokenError(childNodes[0]->token, "Unknown binary operator \"" + childNodes[0]->token.first + "\"");
						exit(1);
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
						printTokenError(childNodes[0]->token, "Unknown binary operator \"" + childNodes[0]->token.first + "\"");
						exit(1);
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
		printTokenError(token, "Cast expression expected name followed by new type like: #cast x : float;");
		exit(1);
	}
	std::string varName = childNodes[1]->childNodes[0]->token.first;
	AllocaInst* var = (AllocaInst*)findNamedValue(parentNode, this, varName);
	if (!var) {
		printTokenError(childNodes[1]->childNodes[0]->token, "Unknown variable name used");
		exit(1);
	}

	Value* value = Builder->CreateLoad(var->getAllocatedType(), var, varName + "_load");

	std::string tyVal = childNodes[1]->childNodes[0]->childNodes[0]->token.first;

	Type* toType = getLLVMTypeFromString(tyVal);

	return castValue(value, toType, true, typeSigns[tyVal], token);

	//return Builder->CreateStore(castedValue, var);
}

// Value*
void* ASTNode::generateCallExpression(int pass)
{

	ASTNode* argsNode = childNodes[0];
	std::vector<ASTNode*> args = std::vector<ASTNode*>();
	for (auto& a : argsNode->childNodes)
		if (a->childNodes.size() > 0) {
			args.push_back(a);
			//aList.push_back(std::make_pair());
		}

	// Look up the id in the global module table.
	Function* CalleeF = getFunctionFromID(token.first, args);
	//Function* CalleeF = TheModule->getFunction(token.first);
	if (!CalleeF) {
		printTokenError(token, "Undefined function");
		return nullptr;
	}

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

// Value*
void* ASTNode::generateIf(int pass)
{
	ASTNode* condExpr = childNodes[0];
	if (condExpr->childNodes.size() == 0) {
		printTokenError(condExpr->token, "Expected condition expression");
		exit(1);
	}
	condExpr = condExpr->childNodes[0];

	if (condExpr->codegen == nullptr) {
		printTokenError(condExpr->token, "Node `" + ASTNodeTypeAsString(condExpr->nodeType) + "` does not have a code generator");
		return nullptr;
	}

	Value* CondV = (Value*)(condExpr->*(condExpr->codegen))(pass);
	if (!CondV)
		return nullptr;

	// Convert condition to a bool by comparing non-equal to i1 1.
	CondV = Builder->CreateICmpNE(CondV, ConstantInt::get(*TheContext, APInt(1, 0)), "ifcond");

	Function* TheFunction = Builder->GetInsertBlock()->getParent();

	// Create blocks for the then and else cases.  Insert the 'then' block at the
	// end of the function.
	BasicBlock* ThenBB = BasicBlock::Create(*TheContext, "then", TheFunction);
	BasicBlock* ElseBB = BasicBlock::Create(*TheContext, "else");
	BasicBlock* MergeBB = BasicBlock::Create(*TheContext, "ifcont");

	Builder->CreateCondBr(CondV, ThenBB, ElseBB);

	// Emit if body
	Builder->SetInsertPoint(ThenBB);

	ASTNode* scopeBody = childNodes[1];

	if (scopeBody->codegen == nullptr) {
		printTokenError(scopeBody->token, "Node `" + ASTNodeTypeAsString(scopeBody->nodeType) + "` does not have a code generator");
		return nullptr;
	}

	Value* ThenV = (Value*)(scopeBody->*(scopeBody->codegen))(pass);

	Builder->CreateBr(MergeBB);
	// Codegen of 'scopeBody' can change the current block, update ThenBB for the PHI.
	ThenBB = Builder->GetInsertBlock();


	// Emit else block.
	TheFunction->insert(TheFunction->end(), ElseBB);
	Builder->SetInsertPoint(ElseBB);

	ASTNode* elseBody = childNodes[2];

	if (elseBody->codegen == nullptr) {
		printTokenError(token, "Node `" + ASTNodeTypeAsString(elseBody->nodeType) + "` does not have a code generator");
		return nullptr;
	}

	Value* ElseV = (Value*)(elseBody->*(elseBody->codegen))(pass);

	Builder->CreateBr(MergeBB);
	// codegen of 'Else' can change the current block, update ElseBB for the PHI.
	ElseBB = Builder->GetInsertBlock();


	// Emit merge block.
	TheFunction->insert(TheFunction->end(), MergeBB);
	Builder->SetInsertPoint(MergeBB);
	//PHINode* PN =
	//	Builder->CreatePHI(Type::getDoubleTy(*TheContext), 2, "iftmp");

	//PN->addIncoming(ThenV, ThenBB);
	//PN->addIncoming(ElseV, ElseBB);
	//return PN;
	return nullptr;
}

// Value*
void* ASTNode::generateStruct(int pass)
{

	std::string structName = token.first;

	std::vector<Type*> fieldTypes;
	std::vector<std::string> fieldNames;
	for (auto& fieldNode : childNodes[0]->childNodes) {
		// fieldNode->childNodes[0]: type (as string or as node)
		// fieldNode->childNodes[1]: name
		std::string typeName = fieldNode->childNodes[0]->token.first;

		Type* fieldType = getLLVMTypeFromString(typeName);

		fieldTypes.push_back(fieldType);
		fieldNames.push_back(fieldNode->token.first);
	}

	// 3. Create struct type
	StructType* structType = StructType::create(*TheContext, fieldTypes, structName);

	//// (Optional) Register type in a map for future lookup
	//NamedValues[structName] = structType;

	// (Optional) Store field names somewhere for member access
	// You could maintain a separate map: structFieldNames[structName] = fieldNames;

	return structType;
}

// Value*
void* ASTNode::generateFor(int pass)
{
	std::string varName = "_iterator";

	if (childNodes[0]->nodeType == Iterator) {
		varName = childNodes[0]->childNodes[0]->token.first;
	}

	ASTNode* rangeStart = childNodes[1]->childNodes[0];

	// Compute the start value.
	if (rangeStart->codegen == nullptr) {
		printTokenError(rangeStart->token, "Node `" + ASTNodeTypeAsString(rangeStart->nodeType) + "` does not have a code generator");
		return nullptr;
	}
	Value* StartVal = (Value*)(rangeStart->*(rangeStart->codegen))(pass);
	if (!StartVal)
		return nullptr;


	// Make the new basic block for the loop header, inserting after current
	// block.
	Function* TheFunction = Builder->GetInsertBlock()->getParent();
	BasicBlock* PreheaderBB = Builder->GetInsertBlock();
	BasicBlock* LoopCondBB = BasicBlock::Create(*TheContext, "loopcond", TheFunction);
	BasicBlock* LoopBB = BasicBlock::Create(*TheContext, "loop", TheFunction);
	BasicBlock* AfterBB = BasicBlock::Create(*TheContext, "afterloop", TheFunction);

	AllocaInst* Alloca = CreateEntryBlockAlloca(TheFunction, Type::getInt32Ty(*TheContext), varName);
	// Store the value into the alloca.
	Builder->CreateStore(StartVal, Alloca);

	// Branch to loop condition check
	Builder->CreateBr(LoopCondBB);

	Builder->SetInsertPoint(LoopCondBB);

	// Get loop limit
	ASTNode* rangeEnd = childNodes[1]->childNodes[1];
	Value* EndVal = (Value*)(rangeEnd->*(rangeEnd->codegen))(pass);

	Value* CurVar = Builder->CreateLoad(Alloca->getAllocatedType(), Alloca, varName.c_str());

	// Compare: exclusive (i < N)
	Value* Cond = Builder->CreateICmpSLT(CurVar, EndVal, "loopcond");

	// Conditional branch
	Builder->CreateCondBr(Cond, LoopBB, AfterBB);


	// Start insertion in LoopBB.
	Builder->SetInsertPoint(LoopBB);

	//// Within the loop, the variable is defined equal to the PHI node.  If it
	//// shadows an existing variable, we have to restore it, so save it now.
	//Value* OldVal = namedValues[varName];
	namedValues[varName] = Alloca;

	// Emit the body of the loop
	ASTNode* scopeBody = childNodes[2];

	if (scopeBody->codegen == nullptr) {
		printTokenError(scopeBody->token, "Node `" + ASTNodeTypeAsString(scopeBody->nodeType) + "` does not have a code generator");
		return nullptr;
	}
	(scopeBody->*(scopeBody->codegen))(pass);

	// Emit the step value.
	Value* StepVal = nullptr;
	//if (Step) {
	//	StepVal = (Value*)(Step->*(Step->codegen))(pass);
	//	if (!StepVal)
	//		return nullptr;
	//}
	//else {
	// If not specified, use 1
	StepVal = ConstantInt::get(*TheContext, APInt(32, 1));
	//}

	Value* NextVar = Builder->CreateAdd(CurVar, StepVal, "nextvar");
	Builder->CreateStore(NextVar, Alloca);

	//// Add incoming for PHI: from loopbody to next iteration
	//Variable->addIncoming(NextVar, Builder->GetInsertBlock());

	// Jump back to condition
	Builder->CreateBr(LoopCondBB);


	// After loop
	Builder->SetInsertPoint(AfterBB);

	// Create the "after loop" block and insert it.
	BasicBlock* LoopEndBB = Builder->GetInsertBlock();

	//// Restore the unshadowed variable.
	//if (OldVal)
	//	namedValues[varName] = OldVal;
	//else
	//	namedValues.erase(varName);


	return nullptr;
}

// Function*
void* ASTNode::generatePrototype(int pass)
{
	argumentList argList = argumentList();
	std::vector<Type*> argTypes = std::vector<Type*>();
	ASTNode* argsNode = childNodes[2];
	ASTNode* modifiersNode = childNodes[3];
	std::vector<std::string> argNames = std::vector<std::string>();
	std::string fnName = token.first;
	std::string mangledName = token.first;

	if (fnName == "binary" || fnName == "unary") {
		fnName = fnName + "." + tokenAsString(childNodes[0]->childNodes[0]->token.second);
		mangledName = fnName + "." + tokenAsString(childNodes[0]->childNodes[0]->token.second);
	}
	//else
	//	fnName = fnName;

	// Get return type
	Type* retType = Type::getVoidTy(*TheContext);
	std::string rTypeString = "";
	ASTNode* typeNode = childNodes[1];
	if (typeNode->childNodes.size() > 0) {
	recurseAddPointer:
		typeNode = typeNode->childNodes[0];
		mangledName += "." + typeNode->token.first;
		rTypeString += typeNode->token.first;

		if (typeNode->token.first == "*") {
			goto recurseAddPointer;
		}

		retType = getLLVMTypeFromString(typeNode->token.first);
	}

	// Get function arguments
	bool variableNumArguments = false;
	for (auto& a : argsNode->childNodes) {
		if (a->childNodes.size() > 0) {
			if (variableNumArguments)  // If there is a named argument after ... then it is invalid
				goto invalidArgument;
			if (a->childNodes[0]->nodeType != Argument_List) {
				std::string typeName = a->childNodes[0]->childNodes[0]->token.first;
				mangledName += "." + typeName;
				Type* aType = nullptr;
				argList.push_back(std::make_pair(typeName, a->childNodes[0]->childNodes[0]->nodeType));

				try {
					aType = getLLVMTypeFromString(typeName);
				}
				catch (...) {
					goto invalidArgument;
				}

				// Unknown type name
				// TODO: Add handling for custom structs as well

				//else if (typeName == "string")
				//Type::getStringTy(*TheContext);
				argTypes.push_back(aType);
				argNames.push_back(a->childNodes[0]->token.first);
				//std::cout << "Arg added: '" << a->childNodes[0]->token.first << "' of type: '" << typeName << "'\n";
			}
			// Handle ellipses ...
			else if (a->childNodes[0]->nodeType == Argument_List) {
				//std::string typeName = a->childNodes[0]->childNodes[0]->token.first;
				variableNumArguments = true;
			}
			continue;
		invalidArgument:
			printTokenError(a->token, "Invalid argument type given");
			exit(1);
		}
	}
	bool isAlwaysInline = false;
	for (auto& m : modifiersNode->childNodes) {
		if (m->token.first == "#inline")
			isAlwaysInline = true;
	}


	// Don't add another prototype if the exact same one is already defined
	//Function* theFunction = TheModule->getFunction(token.first);
	Function* theFunction = getExactFunctionFromID(fnName, argList);
	if (theFunction)
		return theFunction;


	FunctionType* FT = FunctionType::get(retType, argTypes, variableNumArguments);

	Function* fn = Function::Create(FT, Function::ExternalLinkage, fnName, TheModule.get());
	if (isAlwaysInline)
		fn->addFnAttr(llvm::Attribute::AlwaysInline);

	unsigned Idx = 0;
	for (auto& arg : fn->args()) {
		arg.setName(argNames[Idx++]);
		//namedValues[std::string(arg.getName())] = &arg;
	}

	functionIDs.emplace_back(fnName, mangledName, rTypeString, argList, fn);

	return fn;
}

// Function*
void* ASTNode::generateFunction(int pass)
{
	// First, check for an existing function from a previous declaration.
	//Function* theFunction = TheModule->getFunction(token.first);
	Function* theFunction = getFunctionFromID(token.first);

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

	// Record the function arguments in the NamedValues map.
	for (auto& arg : theFunction->args()) {
		AllocaInst* Alloca = CreateEntryBlockAlloca(theFunction, arg.getType(), arg.getName());
		// Store the initial value into the alloca.
		Builder->CreateStore(&arg, Alloca);

		namedValues[std::string(arg.getName())] = Alloca;
	}
	//NamedValues[std::string(Arg.getName())] = &Arg;

	ASTNode* body;
	for (auto& n : childNodes)
		if (n->nodeType == Scope_Body)
			body = n;

	if (body->codegen == nullptr) {
		printTokenError(body->token, "Node `" + ASTNodeTypeAsString(body->nodeType) + "` does not have a code generator");
		return nullptr;
	}

	(body->*(body->codegen))(pass);

	// Ensure there is always a return
	// TODO: Add check for return directly in current scope
	Builder->CreateRet(nullptr);

	// Validate the generated code, checking for consistency.
	verifyFunction(*theFunction);

	// Optimize the function.
	if (optimizationLevel >= 1)
		TheFPM->run(*theFunction, *TheFAM);

	return theFunction;

	//// Error reading body, remove function.
	//theFunction->eraseFromParent();
	//printTokenError(token, "Function is missing a return statement");

	//return nullptr;
}

int outputObjectFile(std::string& objectFilePath)
{

	// Initialize the target registry etc.
	InitializeAllTargetInfos();
	InitializeAllTargets();
	InitializeAllTargetMCs();
	InitializeAllAsmParsers();
	InitializeAllAsmPrinters();

	auto TargetTriple = sys::getDefaultTargetTriple();
	TheModule->setTargetTriple(Triple(TargetTriple));

	std::string Error;
	auto Target = TargetRegistry::lookupTarget(TargetTriple, Error);

	// Print an error and exit if we couldn't find the requested target.
	// This generally occurs if we've forgotten to initialise the
	// TargetRegistry or we have a bogus target triple.
	if (!Target) {
		errs() << Error;
		return 1;
	}

	auto CPU = "generic";
	auto Features = "";

	TargetOptions opt;
	auto TheTargetMachine = Target->createTargetMachine(Triple(TargetTriple), CPU, Features, opt, Reloc::PIC_);

	TheModule->setDataLayout(TheTargetMachine->createDataLayout());

	//std::string objectFilePath = projectDirectory + "build/" + baseFileName + ".o";
	std::error_code EC;
	raw_fd_ostream dest(objectFilePath, EC, sys::fs::OF_None);

	if (EC) {
		errs() << "Could not open file: " << EC.message();
		return 1;
	}

	legacy::PassManager pass;
	auto FileType = CodeGenFileType::ObjectFile;

	if (TheTargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
		errs() << "TheTargetMachine can't emit a file of this type";
		return 1;
	}

	pass.run(*TheModule);
	dest.flush();

	return 0;
}

int generateExecutable(const std::string& objectFilePath, const std::string& exeFilePath)
{
	// Example using clang as the linker
	std::string command = "clang -o " + exeFilePath + " " + objectFilePath;
	int result = std::system(command.c_str());
	return result;
}
