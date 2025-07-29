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

std::unordered_map<std::string, llvm::GlobalVariable*> globalStringLiteralConstants;
bool isCallMemberFunction = false;
bool wasError = false;

llvm::Value* castValue(llvm::Value* value, llvm::Type* destType, bool isSrcSigned, bool isToSigned, tokenPair*& token, bool destTypeIsStruct = false);

std::unordered_map<std::string, Type*> unresolvedTypes;
std::stack<Type*> lastRetrievedElementType;

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


struct argType {
	std::string typeString = "";
	ASTNodeType baseASTType;
	uint8_t pointerLevel = 0;
	bool isReference = false;
	bool mustBeExactType = false;
	argType(std::string ts, ASTNodeType bT, uint8_t pL = 0, bool r = false, bool ex = false)
	{
		typeString = ts;
		baseASTType = bT;
		pointerLevel = pL;
		isReference = r;
		mustBeExactType = ex;
	}
};

typedef std::vector<argType> argumentList;

struct functionID {
	std::string name = "";
	std::string mangledName = "";
	std::string returnType = "";
	argumentList arguments = argumentList();
	argumentList userArguments = argumentList();
	bool variableNumArguments = false;
	bool isStructReturn = false;
	uint32_t uses = 0;
	bool isMemberFunction = false;
	Function* fnValue = nullptr;
	functionID() {}
	functionID(std::string n, std::string mN, std::string r, argumentList llvmArgs, argumentList userArgs, Function* f, bool vA = false, bool mF = false, bool sRet = false)
	{
		name = n;
		mangledName = mN;
		returnType = r;
		arguments = llvmArgs;
		userArguments = userArgs;
		fnValue = f;
		variableNumArguments = vA;
		isMemberFunction = mF;
		isStructReturn = sRet;
	}
	void print()
	{
		console::Write(returnType + " ", console::blueFGColor);
		console::Write(name, console::greenFGColor);
		console::Write("(");
		for (int i = 0; i < arguments.size(); i++) {
			if (arguments[i].isReference)
				console::Write("ref ", console::magentaFGColor);
			console::Write(arguments[i].typeString, console::blueFGColor);
			if (i < arguments.size() - 1)
				console::Write(", ");
		}
		console::WriteLine(")");
	}
	bool compareASTNodeTypes(ASTNodeType& a, ASTNodeType& b, bool wereTypesInferred = false)
	{
		if (a == b)
			return true;
		// If types were inferred from llvm::Type*, it drops the sign
		if (wereTypesInferred) {
			if (a < Begin_Unsigned_Integers && b > Begin_Unsigned_Integers && b <= UInt8_Type) {
				// LLVM does not differentiate between signed and unsigned types, so return true
				// if a is b with flipped signs
				if (a == b - (Begin_Unsigned_Integers + 1))
					return true;
			}
			else if (b < Begin_Unsigned_Integers && a > Begin_Unsigned_Integers && a <= UInt8_Type) {
				// LLVM does not differentiate between signed and unsigned types, so return true
				// if a is b with flipped signs
				if (b == a - (Begin_Unsigned_Integers + 1))
					return true;
			}
		}
		return false;
	}
	uint16_t compareMatch(std::string n, argumentList a, bool wereTypesInferred = false)
	{
		uint16_t differences = 0;
		if (n != name)
			return 1000;
		if (userArguments.size() != a.size())
			return 1000 - 1;
		for (int i = 0; i < userArguments.size(); i++) {
			ASTNodeType t1 = userArguments[i].baseASTType;
			ASTNodeType t2 = a[i].baseASTType;
			bool mustBeExactType = userArguments[i].mustBeExactType;
			// If t1 is an integer type, make sure t2 is also
			// Difference points are given the further the types are

			// If they are the same, return no diff
			if (compareASTNodeTypes(t1, t2, wereTypesInferred))
				differences += 0;
			// Else if they are both integer types
			else if (t1 >= Integer_Node && t1 <= Boolean_Node) {
				if (mustBeExactType)  // If the argument type must be exact
					return 500;
				if (t2 >= Integer_Node && t2 <= Boolean_Node)  // If similar type
					differences += abs(t1 - t2);
				else
					differences += Boolean_Node - Integer_Node;
				// TODO: also give points if there exists a cast function
			}
			// Else if they are both float types
			else if (t1 >= Double_Type && t1 <= Half_Type) {
				if (mustBeExactType)  // If the argument type must be exact
					return 500;
				if (t2 >= Double_Type && t2 <= Half_Type)  // If similar type
					differences += abs(t1 - t2);
				else
					differences += Half_Type - Double_Type;
				// TODO: also give points if there exists a cast function
			}
		}
		return differences;
	}
	uint16_t compareMatch(std::string n, std::vector<ASTNode*> a)
	{
		uint16_t differences = 0;
		if (n != name)
			return 1000;
		if (userArguments.size() != a.size())
			return 1000 - 1;
		for (int i = 0; i < userArguments.size(); i++) {
			ASTNodeType t1 = userArguments[i].baseASTType;
			ASTNodeType t2 = a[i]->nodeType;
			bool mustBeExactType = userArguments[i].mustBeExactType;
			// If t1 is an integer type, make sure t2 is also
			// Difference points are given the further the types are

			// If they are the same, return no diff
			if (compareASTNodeTypes(t1, t2))
				differences += 0;
			// Else if they are both integer types
			else if (t1 >= Integer_Node && t1 <= Boolean_Node) {
				if (mustBeExactType)  // If the argument type must be exact
					return 500;
				if (t2 >= Integer_Node && t2 <= Boolean_Node)  // If similar type
					differences += abs(t1 - t2);
				else
					differences += Boolean_Node - Integer_Node;
				// TODO: also give points if there exists a cast function
			}
			// Else if they are both float types
			else if (t1 >= Double_Type && t1 <= Half_Type) {
				if (mustBeExactType)  // If the argument type must be exact
					return 500;
				if (t2 >= Double_Type && t2 <= Half_Type)  // If similar type
					differences += abs(t1 - t2);
				else
					differences += Half_Type - Double_Type;
				// TODO: also give points if there exists a cast function
			}
		}
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

struct structType {
	std::string name = "";
	argumentList members = argumentList();
	std::unordered_map<std::string, uint16_t> memberNameIndexes;
	uint32_t uses = 0;
	std::vector<functionID*> memberFunctions;
	StructType* structVal = nullptr;
	tokenPair* token = nullptr;
	ASTNode* sourceNode = nullptr;
	structType() {}
	structType(std::string& n, tokenPair*& tP, StructType*& sT, argumentList a, std::vector<functionID*>& mF, std::unordered_map<std::string, uint16_t>& mI)
	{
		name = n;
		token = tP;
		structVal = sT;
		members = a;
		memberFunctions = mF;
		memberNameIndexes = mI;
	}
	structType(std::string& n, tokenPair*& tP, ASTNode* sN)
	{
		name = n;
		token = tP;
		sourceNode = sN;
	}
};

std::vector<functionID*> functionIDs = std::vector<functionID*>();
std::unordered_map<std::string, structType*> structDefinitions = std::unordered_map<std::string, structType*>();
std::stack<std::string> currentStructName = std::stack<std::string>();

structType* getStructTypeFromLLVMType(Type*& t)
{
	for (const auto& [key, value] : structDefinitions) {
		if ((Type*)(value->structVal) == (Type*)t) {
			return value;
		}
	}
	return nullptr;
}

functionID* getFunctionFromID(std::vector<functionID*>& fnIDs, std::string& name, argumentList& arguments, tokenPair*& t, bool wereTypesInferred = false, bool isMemberFunction = false)
{
	functionID* best;
	int bestScore = 1000;
	bool requiresExact = false;
	for (auto& f : fnIDs) {
		uint16_t score = f->compareMatch(name, arguments, wereTypesInferred);
		if (isMemberFunction != f->isMemberFunction)
			continue;
		if (score < bestScore) {
			best = f;
			bestScore = score;
			if (score == 500)
				requiresExact = true;
			else
				requiresExact = false;
		}
	}
	// If the best function match requires exact typing (and different types are passed) throw error
	if (requiresExact) {
		printTokenError(t, "Function match not found, closest prototype requires exact types.\nDid you try casting?");
		wasError = true;
		return nullptr;
	}
	if (bestScore < 1000)
		return best;
	return nullptr;
}
functionID* getExactFunctionFromID(std::vector<functionID*>& fnIDs, std::string& name, argumentList& arguments, tokenPair*& t, bool wereTypesInferred = false)
{
	functionID* best;
	int bestScore = 1000;
	bool requiresExact = false;
	for (auto& f : fnIDs) {
		uint16_t score = f->compareMatch(name, arguments, wereTypesInferred);
		if (score < bestScore) {
			best = f;
			bestScore = score;
			if (score == 500)
				requiresExact = true;
			else
				requiresExact = false;
		}
	}
	// If the best function match requires exact typing (and different types are passed) throw error
	if (requiresExact) {
		//printTokenError(t, "Function match not found, closest prototype requires exact types.\nDid you try casting?");
		return nullptr;
	}
	if (bestScore == 0)
		return best;
	return nullptr;
}
functionID* getFunctionFromID(std::vector<functionID*>& fnIDs, std::string& name, std::vector<ASTNode*>& argValues, tokenPair*& t)
{
	functionID* best;
	int bestScore = 1000;
	bool requiresExact = false;
	for (auto& f : fnIDs) {
		uint16_t score = f->compareMatch(name, argValues);
		if (score < bestScore) {
			best = f;
			bestScore = score;
			if (score == 500)
				requiresExact = true;
			else
				requiresExact = false;
		}
	}
	// If the best function match requires exact typing (and different types are passed) throw error
	if (requiresExact) {
		printTokenError(t, "Function match not found, closest prototype requires exact types.\nDid you try casting?");
		wasError = true;
		return nullptr;
	}
	if (bestScore < 1000)
		return best;
	return nullptr;
}
functionID* getFunctionFromID(std::vector<functionID*>& fnIDs, std::string& name, tokenPair*& t)
{
	functionID* best;
	int bestScore = 1000;
	bool requiresExact = false;
	for (auto& f : fnIDs) {
		uint16_t score = f->compareMatch(name);
		if (score < bestScore) {
			best = f;
			bestScore = score;
			if (score == 500)
				requiresExact = true;
			else
				requiresExact = false;
		}
	}
	// If the best function match requires exact typing (and different types are passed) throw error
	if (requiresExact) {
		printTokenError(t, "Function match not found, closest prototype requires exact types.\nDid you try casting?");
		wasError = true;
		return nullptr;
	}
	if (bestScore < 1000)
		return best;
	return nullptr;
}
functionID* getFunctionIDFromFunctionPointer(std::vector<functionID*>& fnIDs, Function*& fnPtr)
{
	for (auto& f : fnIDs) {
		if (fnPtr == f->fnValue)
			return f;
	}
	console::PrintError("Function could not be resolved from Function*");
	exit(1);
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

Type* getLLVMTypeFromString(std::string typeName, int pointerLevelOffset, tokenPair*& token, bool& wasDefined, int& pass)
{
	Type* aType;
	uint16_t pointerLevel = 0;
	while (typeName[0] == '*') {
		typeName = typeName.substr(1);
		pointerLevel++;
	}
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

	// Otherwise, look in struct definitions
	else if (structDefinitions.find(typeName) != structDefinitions.end()) {
		// If the struct body hasn't been generated yet, generate it
		if (structDefinitions[typeName]->structVal == nullptr) {
			if (currentStructName.size() == 0 || currentStructName.top() != typeName) {
				aType = (Type*)(structDefinitions[typeName]->sourceNode->*(structDefinitions[typeName]->sourceNode->codegen))(pass);
				if (wasError) {
					exit(1);
				}
			}
			// If this type is inside of a struct and the type *is* the struct,
			// throw an error (nested structs aren't allowed)
			else {
				wasDefined = false;
				printTokenError(token, "Cannot nest struct in self");
				wasError = true;
				return nullptr;
				//exit(1);
			}
		}
		else
			aType = (Type*)(structDefinitions[typeName]->structVal);
	}

	// If still not found, return null and add type string to global unresolved types map
	else {
		unresolvedTypes[typeName] = nullptr;
		wasDefined = false;
		return (unresolvedTypes[typeName]);
		//printTokenError(token, "Unknown type \"" + typeName + "\"", __LINE__);
		//exit(1);
	}
	for (int i = 0; i < pointerLevel + pointerLevelOffset; i++)
		aType = aType->getPointerTo();
	return aType;
}

std::string getStringTypeFromLLVMType(llvm::Type* type)
{
	// Count pointer indirections
	int pointerLevel = 0;
	llvm::Type* baseType = type;
	while (baseType->isPointerTy()) {
		pointerLevel++;
		//baseType = baseType->getPointerElementType();
		break;
	}

	// Identify base type and map to string
	std::string baseTypeName;
	if (baseType->isIntegerTy(1)) {
		baseTypeName = "bool";
	}
	else if (baseType->isIntegerTy(8)) {
		baseTypeName = "int8";
	}
	else if (baseType->isIntegerTy(16)) {
		baseTypeName = "int16";
	}
	else if (baseType->isIntegerTy(32)) {
		baseTypeName = "int32";
	}
	else if (baseType->isIntegerTy(64)) {
		baseTypeName = "int64";
	}
	else if (baseType->isIntegerTy(128)) {
		baseTypeName = "int128";
	}
	else if (baseType->isFloatTy()) {
		baseTypeName = "float";
	}
	else if (baseType->isDoubleTy()) {
		baseTypeName = "double";
	}
	else if (baseType->isHalfTy()) {
		baseTypeName = "half";
	}
	else {
		baseTypeName = "unknown";

		// Finally, search through all defined struct types
		for (const auto& [key, value] : structDefinitions) {
			if ((Type*)(value->structVal) == (Type*)baseType) {
				baseTypeName = value->name;
				break;
			}
		}
	}

	// Prefix with pointer asterisks
	std::string pointerPrefix(pointerLevel, '*');
	return pointerPrefix + baseTypeName;
}

ASTNodeType getASTNodeTypeFromString(const std::string& typeName)
{
	if (typeName == "int" || typeName == "int32")
		return SInt32_Type;
	if (typeName == "uint" || typeName == "uint32")
		return UInt32_Type;
	if (typeName == "int8")
		return SInt8_Type;
	if (typeName == "uint8")
		return UInt8_Type;
	if (typeName == "int16")
		return SInt16_Type;
	if (typeName == "uint16")
		return UInt16_Type;
	if (typeName == "int64")
		return SInt64_Type;
	if (typeName == "uint64")
		return UInt64_Type;
	if (typeName == "int128")
		return SInt128_Type;
	if (typeName == "uint128")
		return UInt128_Type;
	if (typeName == "char")
		return Char_Type;
	if (typeName == "bool")
		return Boolean_Node;
	if (typeName == "float")
		return Float_Node;
	if (typeName == "double")
		return Double_Type;
	if (typeName == "half")
		return Half_Type;
	if (typeName == "string")
		return String_Node;
	if (typeName == "type")
		return Type_Node;

	return Identifier_Node;
}

void castToHighestAccuracy(Value*& L, Value*& R, tokenPair*& token)
{
	std::string lTyStr = getStringTypeFromLLVMType(L->getType());
	std::string rTyStr = getStringTypeFromLLVMType(R->getType());
	ASTNodeType LType = getASTNodeTypeFromString(lTyStr);
	ASTNodeType RType = getASTNodeTypeFromString(rTyStr);
	// If signed, L more accurate
	if (LType < RType && RType < Begin_Unsigned_Integers) {
		R = castValue(R, L->getType(), typeSigns[lTyStr], typeSigns[rTyStr], token);
		return;
	}
	// If signed, R more accurate
	else if (RType < LType && LType < Begin_Unsigned_Integers) {
		L = castValue(L, R->getType(), typeSigns[rTyStr], typeSigns[lTyStr], token);
		return;
	}
	// If unsigned, L more accurate
	else if (LType < RType && RType < Double_Type) {
		R = castValue(R, L->getType(), typeSigns[lTyStr], typeSigns[rTyStr], token);
		return;
	}
	// If unsigned, R more accurate
	else if (RType < LType && LType < Double_Type) {
		L = castValue(L, R->getType(), typeSigns[rTyStr], typeSigns[lTyStr], token);
		return;
	}
	// If floats, L more accurate
	else if (LType < RType && RType < Half_Type) {
		R = castValue(R, L->getType(), typeSigns[lTyStr], typeSigns[rTyStr], token);
		return;
	}
	// If unsigned, R more accurate
	else if (RType < LType && LType < Half_Type) {
		L = castValue(L, R->getType(), typeSigns[rTyStr], typeSigns[lTyStr], token);
		return;
	}

	printTokenError(token, "Unsupported cast");
	wasError = true;
	//exit(1);
	return;
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

valueType* findNamedValue(ASTNode* node, ASTNode* childNode, std::string& identifier)
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


llvm::Value* castValue(llvm::Value* value, llvm::Type* destType, bool isSrcSigned, bool isToSigned, tokenPair*& token, bool destTypeIsStruct)
{
	llvm::Type* srcType = value->getType();

	if (destTypeIsStruct) {
		return value;
	}

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
	//exit(1);
	return nullptr;
}

//bool findVariableDeclaration(ASTNode*& node, ASTNode*& childNode, std::string& identifier)
//{
//	for (auto& c : node->childNodes) {
//		if (node->depth > 0)	 // only go past use if in global scope
//			if (c == childNode)	 // dont go past the node where it is used
//				break;
//		if (c->nodeType == Expression_Statement && c->childNodes.size() > 0)
//			if (c->childNodes[0]->token->first == identifier)
//				return true;
//	}
//	if (node->depth > 0)  // search the parent recursively until found or end of global scope is reached
//		return findVariableDeclaration(node->parentNode, node, identifier);
//
//	return false;
//}

std::string unescapeString(const std::string& src, tokenPair*& token)
{
	std::string result;
	result.reserve(src.size());

	for (size_t i = 0; i < src.length(); ++i) {
		char c = src[i];
		if (c != '\\') {
			result.push_back(c);
		}
		else {
			if (i + 1 >= src.length()) {
				printTokenError(token, "Incomplete escape sequence at end of string");
				wasError = true;
				return "";
				//exit(1);
			}

			char esc = src[++i];
			switch (esc) {
				case 'a':
					result.push_back('\a');
					break;
				case 'b':
					result.push_back('\b');
					break;
				case 'f':
					result.push_back('\f');
					break;
				case 'n':
					result.push_back('\n');
					break;
				case 'r':
					result.push_back('\r');
					break;
				case 't':
					result.push_back('\t');
					break;
				case 'v':
					result.push_back('\v');
					break;
				case '\\':
					result.push_back('\\');
					break;
				case '\'':
					result.push_back('\'');
					break;
				case '"':
					result.push_back('\"');
					break;
				case '?':
					result.push_back('\?');
					break;
				// Hexadecimal: \xhh...
				case 'x': {
					int value = 0;
					int digits = 0;
					while (i + 1 < src.length() && std::isxdigit(src[i + 1])) {
						++i;
						value *= 16;
						char hc = src[i];
						if (hc >= '0' && hc <= '9')
							value += hc - '0';
						else if (hc >= 'a' && hc <= 'f')
							value += 10 + (hc - 'a');
						else if (hc >= 'A' && hc <= 'F')
							value += 10 + (hc - 'A');
						++digits;
					}
					if (digits == 0)
						throw std::runtime_error("Invalid \\x escape");
					result.push_back(static_cast<char>(value));
					break;
				}
				// Universal character: \uFFFF or \UFFFFFFFF
				case 'u':
				case 'U': {
					int maxlen = (esc == 'u') ? 4 : 8;
					int value = 0;
					int digits = 0;
					while (digits < maxlen && i + 1 < src.length() && std::isxdigit(src[i + 1])) {
						++i;
						char hc = src[i];
						value *= 16;
						if (hc >= '0' && hc <= '9')
							value += hc - '0';
						else if (hc >= 'a' && hc <= 'f')
							value += 10 + (hc - 'a');
						else if (hc >= 'A' && hc <= 'F')
							value += 10 + (hc - 'A');
						++digits;
					}
					if (digits != maxlen)
						throw std::runtime_error("Invalid \\u or \\U escape");
					// For simplicity, only support basic multilingual plane
					if (value <= 0x7F)
						result.push_back(static_cast<char>(value));
					else if (value <= 0x7FF) {
						result.push_back(static_cast<char>(0xC0 | ((value >> 6) & 0x1F)));
						result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
					}
					else if (value <= 0xFFFF) {
						result.push_back(static_cast<char>(0xE0 | ((value >> 12) & 0x0F)));
						result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
						result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
					}
					else if (value <= 0x10FFFF) {
						result.push_back(static_cast<char>(0xF0 | ((value >> 18) & 0x07)));
						result.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
						result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
						result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
					}
					else {
						throw std::runtime_error("Unicode code point out of range in escape");
					}
					break;
				}
				// Octal: up to 3 octal digits \nnn
				default:
					if (esc >= '0' && esc <= '7') {
						int value = esc - '0';
						int digits = 1;
						while (digits < 3 && i + 1 < src.length() && src[i + 1] >= '0' && src[i + 1] <= '7') {
							++i;
							value = value * 8 + (src[i] - '0');
							++digits;
						}
						result.push_back(static_cast<char>(value));
					}
					else {
						// Anything else: treat as literal character
						result.push_back(esc);
					}
					break;
			}
		}
	}

	return result;
}

// Value*
void* ASTNode::generateConstant(int pass)
{
	if (nodeType == Integer_Node)
		return ConstantInt::get(*TheContext, APInt(32, stoi(token->first), true));
	else if (nodeType == Boolean_Node)
		return ConstantInt::get(*TheContext, APInt(1, token->first == "true" ? 1 : 0, false));
	else if (nodeType == Float_Node)
		return ConstantFP::get(*TheContext, APFloat(stod(token->first)));
	else if (nodeType == String_Constant_Node) {
		std::string strValue = unescapeString(token->first.substr(1, token->first.size() - 2), token);	// remove quotes from token

		GlobalVariable* globalStr = nullptr;
		if (globalStringLiteralConstants.find(strValue) != globalStringLiteralConstants.end())
			globalStr = globalStringLiteralConstants[strValue];
		else {
			// Create constant data array (i8 array)
			Constant* strConst = ConstantDataArray::getString(*TheContext, strValue, true);

			// Create global variable to hold the string
			globalStr = new GlobalVariable(
				*TheModule,
				strConst->getType(),
				true,						  // Constant
				GlobalValue::PrivateLinkage,  // Or InternalLinkage for hidden
				strConst,
				"str");
			globalStr->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);  // Allow merging
			globalStr->setAlignment(Align(1));

			globalStringLiteralConstants[strValue] = globalStr;
		}

		// Get pointer to the first element (i8*)
		Constant* zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
		std::vector<Constant*> indices = {zero, zero};
		Constant* strPtr = ConstantExpr::getGetElementPtr(
			globalStr->getValueType(),
			globalStr,
			indices);

		return strPtr;	// Returns i8* pointing to the string
	}
	else if (nodeType == Character_Constant_Node) {
		std::string strValue = unescapeString(token->first.substr(1, token->first.size() - 2), token);	// remove quotes from token

		if (strValue.size() > 1) {
			printTokenError(token, "Character constant may contain only a single character");
			wasError = true;
			return nullptr;
			//exit(1);
		}

		return ConstantInt::get(*TheContext, APInt(8, strValue[0], true));
	}

	printTokenError(token, "Value could not be parsed as constant");
	wasError = true;
	return nullptr;
}

// Value*
void* ASTNode::generateVariableExpression(int pass)
{
	// Look this variable up in the function.
	valueType* val = findNamedValue(parentNode, this, token->first);
	if (!val) {
		Value* exprVal = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
		Type* type = nullptr;
		Function* theFunction = Builder->GetInsertBlock()->getParent();
		uint16_t pointerLevel = 0;
		std::string typeName = "";

		// If variable does not have type, it is a used undefined variable
		if (childNodes.size() == 0) {
			printTokenError(token, "Undefined variable name");
			wasError = true;
			return nullptr;
			//exit(1);
		}
		// If variable does have type, it is a declaration
		else {
			ASTNode* typeNode = childNodes[0];
			typeName = typeNode->token->first;
		getNextPointerLevel:
			if (typeNode->token->first == "*") {
				pointerLevel++;
				typeNode = typeNode->childNodes[0];
				goto getNextPointerLevel;
			}
			bool wasDefined = true;
			type = getLLVMTypeFromString(typeName, 0, typeNode->token, wasDefined, pass);
			for (int i = 0; i < pointerLevel; i++)
				type = type->getPointerTo();
			if (typeSigns.find(typeName) != typeSigns.end())  // If builtin type
				exprVal = castValue(exprVal, type, true, typeSigns[typeNode->token->first], token);
			else if (structDefinitions.find(typeName) != structDefinitions.end())  // If defined struct
				exprVal = castValue(exprVal, type, true, false, token, true);
			else {
				printTokenError(token, "Unknown type");
				wasError = true;
				return nullptr;
				//exit(1);
			}
		}
		baseType = type;
		AllocaInst* targetPtr = CreateEntryBlockAlloca(theFunction, type, token->first);
		std::string actualType = (pointerLevel > 0 ? std::string(pointerLevel, '*') : "") + typeName;
		namedValues[token->first] = new valueType(token->first, actualType, targetPtr);

		Builder->CreateStore(exprVal, targetPtr);

		if (isRef || lvalue)
			return targetPtr;
		else
			return Builder->CreateLoad(targetPtr->getAllocatedType(), targetPtr, token->first + "_load");
	}
	AllocaInst* A = (AllocaInst*)(val->val);
	baseType = A->getAllocatedType();

	if (isRef || lvalue)
		return A;
	else
		return Builder->CreateLoad(A->getAllocatedType(), A, token->first + "_load");
}

void* ASTNode::generateReturn(int pass)
{
	ASTNode* exprNode = childNodes[0];
	if (exprNode->codegen == nullptr) {
		printTokenError(exprNode->token, "Node `" + ASTNodeTypeAsString(exprNode->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}
	Value* RetVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	if (wasError) {
		exit(1);
	}
	// Check if we're returning a struct
	Type* returnType = Builder->GetInsertBlock()->getParent()->getReturnType();
	if (returnType->isStructTy()) {
		// For struct returns, we need to handle this specially
		// Option 1: If the function uses sret, copy to the sret parameter
		Function* currentFunc = Builder->GetInsertBlock()->getParent();
		if (currentFunc->hasStructRetAttr()) {
			// Get the sret parameter (first parameter)
			Value* sretPtr = &*currentFunc->arg_begin();

			// Copy the struct value to the sret location
			if (RetVal->getType()->isPointerTy()) {
				// If RetVal is a pointer to struct, memcpy from it
				Value* structSize = ConstantInt::get(Type::getInt64Ty(*TheContext),
					TheModule->getDataLayout().getTypeAllocSize(returnType));

				// Create memcpy call
				Function* memcpyFunc = Intrinsic::getDeclaration(TheModule.get(),
					Intrinsic::memcpy, {sretPtr->getType(), RetVal->getType(), Type::getInt64Ty(*TheContext)});
				Builder->CreateCall(memcpyFunc, {sretPtr, RetVal, structSize, ConstantInt::get(Type::getInt1Ty(*TheContext), 0)});
			}
			else {
				// If RetVal is a struct value, store it
				Builder->CreateStore(RetVal, sretPtr);
			}


			Builder->CreateRetVoid();
		}
		else {
			// Option 2: Direct struct return (for small structs)
			if (RetVal->getType()->isPointerTy()) {
				// Load the struct value from the pointer
				RetVal = Builder->CreateLoad(returnType, RetVal, "struct_ret_load");
			}
			Builder->CreateRet(RetVal);
		}
	}
	else {
		// Non-struct return, handle normally
		Builder->CreateRet(RetVal);
	}

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
		wasError = true;
		return nullptr;
	}
	//if (exprNode->childNodes.size() == 0)
	//	return nullptr;
	Value* exprVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	if (wasError) {
		exit(1);
	}
	return exprVal;
}

// Value*
void* ASTNode::generateExpressionStatement(int pass)
{
	ASTNode* leftNode = childNodes[0];
	ASTNode* exprNode = childNodes[1];
	Function* theFunction = Builder->GetInsertBlock()->getParent();

	if (exprNode->codegen == nullptr) {
		printTokenError(exprNode->token, "Node `" + ASTNodeTypeAsString(exprNode->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}

	// Evaluate right side (rvalue)
	Value* exprVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	if (wasError) {
		exit(1);
	}
	if (!exprVal) {
		printTokenError(token, "Set expression requires right argument");
		wasError = true;
		return nullptr;
		//exit(1);
	}

	Value* targetPtr = nullptr;
	Type* targetType = nullptr;

	// If the left side is a pointer lvalue
	if (leftNode->nodeType != Identifier_Node) {
		leftNode->lvalue = true;
		// Otherwise left side is an expression, evaluate to pointer (lvalue address)
		if (leftNode->codegen == nullptr) {
			printTokenError(leftNode->token, "Node `" + ASTNodeTypeAsString(leftNode->nodeType) + "` does not have a code generator");
			wasError = true;
			return nullptr;
		}
		targetPtr = (Value*)(leftNode->*(leftNode->codegen))(pass);
		if (wasError) {
			exit(1);
		}
		if (!targetPtr || !targetPtr->getType()->isPointerTy()) {
			printTokenError(token, "Left side must evaluate to a pointer for assignment");
			wasError = true;
			return nullptr;
			//exit(1);
		}
		//targetType = cast<PointerType>(targetPtr->getType())->getElementType();
	}
	else {
		//targetPtr = (Value*)(leftNode->*(leftNode->codegen))(pass);
		//if (!targetPtr || !targetPtr->getType()->isPointerTy()) {
		//	printTokenError(token, "Left side must evaluate to a pointer");
		//	exit(1);
		//}
		//// For safety: insert runtime null check (optional, but recommended)
		//Value* nullPtr = ConstantPointerNull::get(targetPtr->getType());
		//Value* isNull = Builder->CreateICmpEQ(targetPtr, nullPtr, "nullcheck");
		//BasicBlock* currentBB = Builder->GetInsertBlock();
		//BasicBlock* validBB = BasicBlock::Create(*TheContext, "validstore", currentBB->getParent());
		//BasicBlock* errorBB = BasicBlock::Create(*TheContext, "nullerror", currentBB->getParent());
		//Builder->CreateCondBr(isNull, errorBB, validBB);
		//Builder->SetInsertPoint(errorBB);
		//// Call some error handler or abort
		//Builder->CreateUnreachable();  // Or print error and exit
		//Builder->SetInsertPoint(validBB);
	}

	ASTNode* typeNode = nullptr;
	Type* type = nullptr;
	int pointerLevel = 0;
	if (!leftNode->lvalue) {
		if (leftNode->childNodes.size() > 0) {
			typeNode = leftNode->childNodes[0];
		getNextPointerLevel:
			if (typeNode->token->first == "*") {
				pointerLevel++;
				//pointerLevel = typeNode->token->first.size();
				typeNode = typeNode->childNodes[0];
				goto getNextPointerLevel;
			}
			bool wasDefined = true;
			type = getLLVMTypeFromString(typeNode->token->first, 0, typeNode->token, wasDefined, pass);
			//if (wasDefined == false)
			//	return nullptr;
			for (int pL = 0; pL < pointerLevel; pL++)
				type = type->getPointerTo();
		}
	}
	//else
	//		type = var->getType();

	// Automatically resolve type from expression if not already set
	if (type == nullptr) {
		type = exprVal->getType();
	}
	// Otherwise, the type is explicit, and builtin types should be cast automatically
	else {
		std::string typeName = typeNode->token->first;
		if (typeSigns.find(typeName) != typeSigns.end())  // If builtin type
			exprVal = castValue(exprVal, type, true, typeSigns[typeNode->token->first], exprNode->token);
		else if (structDefinitions.find(typeName) != structDefinitions.end())  // If defined struct
			exprVal = castValue(exprVal, type, true, false, exprNode->token, true);
		else {
			printTokenError(token, "Unknown type");
			wasError = true;
			return nullptr;
			//exit(1);
		}
	}

	// If the left side is an identifier
	if (leftNode->nodeType == Identifier_Node) {
		// Simple variable: find alloca and use it as targetPtr
		valueType* val = findNamedValue(parentNode, this, leftNode->token->first);
		if (!val) {
			targetPtr = CreateEntryBlockAlloca(theFunction, type, leftNode->token->first);
			std::string actualType = "*int";
			if (!typeNode)
				actualType = getStringTypeFromLLVMType(type);
			else
				actualType = (pointerLevel > 0 ? std::string(pointerLevel, '*') : "") + typeNode->token->first;
			namedValues[leftNode->token->first] = new valueType(leftNode->token->first, actualType, targetPtr);
		}
		else
			targetPtr = (AllocaInst*)(val->val);
		targetType = ((AllocaInst*)targetPtr)->getAllocatedType();
	}
	else
		targetType = type;

	//if (exprVal->getType() != targetType) {
	//	printTokenError(token, "Type mismatch in set expression");
	//	exprVal->getType()->print(llvm::outs());
	//	type->print(llvm::outs());
	//	//printTokenError(token, "Type mismatch in set expression.\nTypes are \"" + exprVal->getType()->getAsString() + "\" and \"" + type->getAsString() + "\"");
	//	exit(1);
	//}

	Builder->CreateStore(exprVal, targetPtr);

	return exprVal;
}

// Value*
void* ASTNode::generateIterator(int pass)
{
	ASTNode* identifierNode = childNodes[0];
	Value* var = nullptr;
	//Value* var = (Value*)(findNamedValue(parentNode, this, token->first)->val);
	//if (!var) {
	Type* type = llvm::Type::getInt32Ty(*TheContext);
	var = Builder->CreateAlloca(type, nullptr, identifierNode->token->first);
	//}
	return var;
}

// Value*
void* ASTNode::generateUnaryExpression(int pass)
{
	if (childNodes.size() == 0) {
		printTokenError(token, "Unary expression reqires an argument");
		wasError = true;
		return nullptr;
	}
	if (childNodes[0]->codegen == nullptr) {
		printTokenError(childNodes[0]->token, "Node `" + ASTNodeTypeAsString(childNodes[0]->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}
	Value* R = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
	if (wasError) {
		exit(1);
	}
	if (!R)
		return nullptr;

	ASTNodeType t = childNodes[0]->nodeType;

	switch (nodeType) {
		case Address_Of_Operation: {
			ASTNode* varNode = childNodes[0];
			valueType* val = findNamedValue(parentNode, this, varNode->token->first);
			if (!val) {
				printTokenError(token, "Unknown variable name for address-of");
				wasError = true;
				return nullptr;
				//exit(1);
			}
			Value* var = (Value*)(val->val);
			return var;
		}

		case Dereference_Operation: {
			ASTNode* ptrNode = childNodes[0];
			Value* ptrVal = (Value*)(ptrNode->*(ptrNode->codegen))(pass);
			if (wasError) {
				exit(1);
			}
			if (!ptrVal) {
				printTokenError(token, "Dereference of null pointer");
				wasError = true;
				return nullptr;
				//exit(1);
			}
			// Load the value from the pointer
			Type* elementType = Type::getInt32Ty(*TheContext);	// TODO: whatever type is appropriate
			return Builder->CreateLoad(elementType, ptrVal, "deref_tmp");
		}

		default:
			printTokenError(token, "Unknown or undefined operator");
			wasError = true;
			return nullptr;
			//exit(1);
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
		//				printTokenError(childNodes[0]->token, "Unknown operator \"" + childNodes[0]->token->first + "\"");
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
		//				printTokenError(childNodes[0]->token, "Unknown operator \"" + childNodes[0]->token->first + "\"");
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
		//				printTokenError(childNodes[0]->token, "Unknown operator \"" + childNodes[0]->token->first + "\"");
		//				exit(1);
		//		}
	}

	return nullptr;
}

// Integer operations
static const std::unordered_map<ASTNodeType, llvm::Instruction::BinaryOps> integerOps = {
	{Expression_Plus, llvm::Instruction::Add},
	{Expression_Minus, llvm::Instruction::Sub},
	{Expression_Times, llvm::Instruction::Mul},
	{Expression_Divide, llvm::Instruction::SDiv},  // or UDiv for unsigned
	{Expression_Modulo, llvm::Instruction::SRem},  // or URem for unsigned
	{Bitwise_And, llvm::Instruction::And},
	{Bitwise_Or, llvm::Instruction::Or},
	{Bitwise_Xor, llvm::Instruction::Xor},
	{Bitwise_Shift_Left, llvm::Instruction::Shl},
	{Bitwise_Shift_Right, llvm::Instruction::LShr},	 // or AShr for arithmetic shift
};

// Float operations
static const std::unordered_map<ASTNodeType, llvm::Instruction::BinaryOps> floatOps = {
	{Expression_Plus, llvm::Instruction::FAdd},
	{Expression_Minus, llvm::Instruction::FSub},
	{Expression_Times, llvm::Instruction::FMul},
	{Expression_Divide, llvm::Instruction::FDiv},
};

// Comparison operations (return i1)
static const std::unordered_map<ASTNodeType, llvm::CmpInst::Predicate> intCompareOps = {
	{Compare_Equal, llvm::CmpInst::ICMP_EQ},
	{Compare_Not, llvm::CmpInst::ICMP_NE},
	{Compare_Less, llvm::CmpInst::ICMP_SLT},  // or ICMP_ULT for unsigned
	{Compare_LessEqual, llvm::CmpInst::ICMP_SLE},
	{Compare_Greater, llvm::CmpInst::ICMP_SGT},
	{Compare_GreaterEqual, llvm::CmpInst::ICMP_SGE},
};

static const std::unordered_map<ASTNodeType, llvm::CmpInst::Predicate> floatCompareOps = {
	{Compare_Equal, llvm::CmpInst::FCMP_OEQ},
	{Compare_Not, llvm::CmpInst::FCMP_ONE},
	{Compare_Less, llvm::CmpInst::FCMP_OLT},
	{Compare_LessEqual, llvm::CmpInst::FCMP_OLE},
	{Compare_Greater, llvm::CmpInst::FCMP_OGT},
	{Compare_GreaterEqual, llvm::CmpInst::FCMP_OGE},
};

enum class ValueCategory {
	Integer,
	Float,
	Pointer,
	Unknown
};

ValueCategory getValueCategory(llvm::Type* type)
{
	if (type->isIntegerTy())
		return ValueCategory::Integer;
	if (type->isFloatingPointTy())
		return ValueCategory::Float;
	if (type->isPointerTy())
		return ValueCategory::Pointer;
	return ValueCategory::Unknown;
}

bool isSignedType(const std::string& typeStr)
{
	return typeSigns.count(typeStr) && typeSigns[typeStr];
}

// Value*
void* ASTNode::generateBinaryExpression(int pass)
{
	if (childNodes.size() < 2) {
		printTokenError(token, "Binary expression requires left and right arguments");
		return nullptr;
	}

	if (!childNodes[0]->codegen || !childNodes[1]->codegen) {
		printTokenError(token, "Binary expression operands missing code generators");
		return nullptr;
	}

	Value* L = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
	Value* R = (Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);

	if (!L || !R)
		return nullptr;

	// Check for operator overloads first
	if (nodeType == Redefined_Operator_Expr || checkForOperatorOverload()) {
		return generateOperatorOverloadCall(L, R);
	}

	// Auto-cast to highest precision if types differ
	if (L->getType() != R->getType()) {
		printTokenWarning(token, "Operand type mismatch, performing implicit conversion");
		castToHighestAccuracy(L, R, token);
	}

	ValueCategory category = getValueCategory(L->getType());

	switch (category) {
		case ValueCategory::Integer:
			return generateIntegerBinaryOp(L, R);
		case ValueCategory::Float:
			return generateFloatBinaryOp(L, R);
		case ValueCategory::Pointer:
			return generatePointerBinaryOp(L, R);
		default:
			printTokenError(token, "Unsupported operand types for binary operation");
			return nullptr;
	}
}

Value* ASTNode::generateIntegerBinaryOp(Value* L, Value* R)
{
	// Check for comparison operations first
	auto compIt = intCompareOps.find(nodeType);
	if (compIt != intCompareOps.end()) {
		return Builder->CreateICmp(compIt->second, L, R, "icmp_tmp");
	}

	// Regular arithmetic/bitwise operations
	auto opIt = integerOps.find(nodeType);
	if (opIt != integerOps.end()) {
		return Builder->CreateBinOp(opIt->second, L, R, "int_op");
	}

	printTokenError(token, "Unknown integer binary operator");
	return nullptr;
}

Value* ASTNode::generateFloatBinaryOp(Value* L, Value* R)
{
	// Check for comparison operations first
	auto compIt = floatCompareOps.find(nodeType);
	if (compIt != floatCompareOps.end()) {
		return Builder->CreateFCmp(compIt->second, L, R, "fcmp_tmp");
	}

	// Regular arithmetic operations
	auto opIt = floatOps.find(nodeType);
	if (opIt != floatOps.end()) {
		return Builder->CreateBinOp(opIt->second, L, R, "float_op");
	}

	printTokenError(token, "Unknown float binary operator");
	return nullptr;
}

Value* ASTNode::generatePointerBinaryOp(Value* L, Value* R)
{
	//// Handle pointer arithmetic (ptr + int, ptr - int, ptr - ptr)
	//if (nodeType == Expression_Plus && R->getType()->isIntegerTy()) {
	//	return Builder->CreateGEP(L->getType()->getPointerElementType(), L, R, "ptr_add");
	//}
	//if (nodeType == Expression_Minus && R->getType()->isIntegerTy()) {
	//	Value* negR = Builder->CreateNeg(R, "neg_offset");
	//	return Builder->CreateGEP(L->getType()->getPointerElementType(), L, negR, "ptr_sub");
	//}
	//if (nodeType == Expression_Minus && R->getType()->isPointerTy()) {
	//	return Builder->CreatePtrDiff(L->getType()->getPointerElementType(), L, R, "ptr_diff");
	//}

	//// Pointer comparisons
	//auto compIt = intCompareOps.find(nodeType);
	//if (compIt != intCompareOps.end()) {
	//	return Builder->CreateICmp(compIt->second, L, R, "ptr_cmp");
	//}

	printTokenError(token, "Invalid pointer operation");
	return nullptr;
}

bool ASTNode::checkForOperatorOverload()
{
	std::string operatorName = "operator." + tokenAsString(token->second);
	functionID* calleeID = getFunctionFromID(functionIDs, operatorName, token);
	return calleeID != nullptr;
}

Value* ASTNode::generateOperatorOverloadCall(Value* L, Value* R)
{
	std::string operatorName = "operator." + tokenAsString(token->second);
	functionID* calleeID = getFunctionFromID(functionIDs, operatorName, token);

	if (!calleeID) {
		printTokenError(token, "Expected operator overload not found");
		return nullptr;
	}

	std::vector<Value*> ArgsV = {L, R};
	return Builder->CreateCall(calleeID->fnValue, ArgsV, "op_overload");
}

// Value*
void* ASTNode::generateAccessOperation(int pass)
{
	printf("Generating access operation\n");
	if (childNodes.size() == 0) {
		printTokenError(token, "Access operation reqires a left and right argument");
		wasError = true;
		return nullptr;
	}
	if (childNodes[0]->codegen == nullptr) {
		printTokenError(childNodes[0]->token, "Node `" + ASTNodeTypeAsString(childNodes[0]->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}
	if (childNodes[1]->codegen == nullptr) {
		printTokenError(childNodes[0]->token, "Node `" + ASTNodeTypeAsString(childNodes[1]->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}
	childNodes[0]->lvalue = true;  // Set flag for base to return address if needed
	Value* L = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
	if (wasError) {
		exit(1);
	}
	Value* R = (Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);
	if (wasError) {
		exit(1);
	}
	if (!L || !R)
		return nullptr;

	ASTNodeType t = childNodes[0]->nodeType;

	Type* lType = L->getType();
	if (t != Identifier_Node) {
		//if (childNodes[0]->baseType != nullptr)
		//	lType = childNodes[0]->baseType;
		lType = lastRetrievedElementType.top()->getPointerTo();
		lastRetrievedElementType.pop();
		//lastRetrievedElementType.push(getLLVMTypeFromString(CalleeFID->returnType, 0, token, wasDefined, pass));
	}

	bool operatorOverloaded = false;
	std::string operatorOverloadName = "operator." + tokenAsString(Both_Brackets);
	argumentList argList = argumentList();
	argList.push_back(argType(getStringTypeFromLLVMType(lType), getASTNodeTypeFromString(getStringTypeFromLLVMType(lType)), 0, false, true));
	argList.push_back(argType(getStringTypeFromLLVMType(R->getType()), getASTNodeTypeFromString(getStringTypeFromLLVMType(R->getType())), 0));
	Function* CalleeF = nullptr;
	// TODO: make operator overloads for exact matches only
	functionID* calleeID = getExactFunctionFromID(functionIDs, operatorOverloadName, argList, token, true);

	// Check to see if the operator actually has an overload
	if (calleeID != nullptr) {
		CalleeF = calleeID->fnValue;
		operatorOverloaded = true;
	}

	// If this is an operator overload, create function call to it
	if (operatorOverloaded) {
		if (!CalleeF) {
			printTokenError(token, "Undefined function");
			wasError = true;
			return nullptr;
		}

		std::vector<Value*> ArgsV;
		ArgsV.push_back(L);
		ArgsV.push_back(R);

		return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
	}
	// Otherwise, it is a regular builtin operator
	else {
		if (lType->isPointerTy() == false) {
			printTokenError(token, "Left argument of access operator must be a pointer type");
			wasError = true;
			return nullptr;
			//exit(1);
		}
		if (R->getType()->isIntegerTy() == false) {
			printTokenError(token, "Right argument of access operator must be an integer");
			wasError = true;
			return nullptr;
			//exit(1);
		}

		// Evaluate base pointer
		Value* basePtr = L;
		if (!basePtr || !lType->isPointerTy()) {
			printTokenError(token, "Base must be a pointer for access");
			wasError = true;
			return nullptr;
			//exit(1);
		}

		// Evaluate index (should be integer)
		Value* index = R;
		if (!index || !index->getType()->isIntegerTy()) {
			printTokenError(token, "Index must be an integer");
			wasError = true;
			return nullptr;
			//exit(1);
		}

		//valueType* v = findNamedValue(this, nullptr, childNodes[0]->token->first);
		//printf("ACCESS OPERATOR type string: %s\n", v->type.c_str());
		//Type* elementType = getLLVMTypeFromString(v->type, -1, childNodes[0]->token);
		Type* elementType = nullptr;

		if (childNodes[0]->baseType != nullptr)
			elementType = childNodes[0]->baseType;
		//Type* elementType = getLLVMTypeFromString(baseType);
		if (!elementType) {
			printTokenError(token, "Invalid element type");
			wasError = true;
			return nullptr;
			//exit(1);
		}

		// Create GEP to compute the address
		Value* gep = Builder->CreateGEP(elementType, basePtr, index, "arrayidx");

		// If this is an lvalue (for assignment), return the pointer gep
		if (lvalue) {
			printf("gep returned\n");
			return gep;
		}
		// If rvalue, return value
		else {
			printf("value returned\n");
			return Builder->CreateLoad(elementType, gep, "accessop_load");
		}


		//Value* newAddr = Builder->CreateAdd(L, R, "ptraddtmp");
		//return newAddr;
	}

	return nullptr;
}

// Value*
void* ASTNode::generateMemberAccess(int pass)
{
	if (childNodes.size() == 0) {
		printTokenError(token, "Member access operation reqires a left and right argument");
		wasError = true;
		return nullptr;
	}
	if (childNodes[0]->codegen == nullptr) {
		printTokenError(childNodes[0]->token, "Node `" + ASTNodeTypeAsString(childNodes[0]->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}
	if (childNodes[1]->codegen == nullptr) {
		printTokenError(childNodes[0]->token, "Node `" + ASTNodeTypeAsString(childNodes[1]->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}
	childNodes[0]->lvalue = true;  // Set flag for base to return address if needed

	ASTNodeType t = childNodes[0]->nodeType;

	bool operatorOverloaded = false;
	std::string operatorOverloadName = "operator." + tokenAsString(Dot);
	argumentList argList = argumentList();


	Value* L = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
	if (wasError) {
		exit(1);
	}
	//Type* lastMemberType = lastRetrievedElementType.top();
	//lastRetrievedElementType.pop();
	//if (!L)
	//	return nullptr;

	//if (L->getType()->isPointerTy() == false) {
	//	printTokenError(token, "Left argument of member access operator must be a pointer type");
	//	exit(1);
	//}
	//if (R->getType()->isIntegerTy() == false) {
	//	printTokenError(token, "Right argument of member access operator must be an integer");
	//	exit(1);
	//}

	// Evaluate base pointer
	Value* basePtr = L;
	if (!basePtr || !basePtr->getType()->isPointerTy()) {
		printTokenError(token, "Base must be a pointer for access");
		wasError = true;
		return nullptr;
	}

	if (childNodes[0]->nodeType == Identifier_Node) {
		valueType* v = findNamedValue(this, nullptr, childNodes[0]->token->first);

		if (structDefinitions.find(v->type) == structDefinitions.end()) {
			printTokenError(token, "Type \"" + v->type + "\" has not been defined");
			wasError = true;
			return nullptr;
			//exit(1);
		}
		structType* structDefinition = structDefinitions[v->type];

		// If the struct body hasn't been generated yet, generate it
		if (structDefinition->structVal == nullptr)
			Value* argVal = (Value*)(structDefinition->sourceNode->*(structDefinition->sourceNode->codegen))(pass);
		if (wasError) {
			exit(1);
		}

		// Get member name and index
		std::string memberName = childNodes[1]->token->first;
		// Handle if member variable access/set
		if (childNodes[1]->nodeType == Identifier_Node) {
			if (structDefinition->memberNameIndexes.find(memberName) == structDefinition->memberNameIndexes.end()) {
				printTokenError(childNodes[1]->token, "Struct definition does not contain member");
				wasError = true;
				return nullptr;
				//exit(1);
			}


			uint16_t memberIndex = structDefinition->memberNameIndexes[memberName];

			bool wasDefined = true;
			Type* elementType = getLLVMTypeFromString(structDefinition->members[memberIndex].typeString, 0, token, wasDefined, pass);
			//if (wasDefined == false)
			//	return nullptr;
			//Type* elementType = getLLVMTypeFromString(v->type, -1, childNodes[0]->token);
			//Type* elementType = getLLVMTypeFromString(baseType);
			if (!elementType) {
				printTokenError(token, "Invalid element type");
				wasError = true;
				return nullptr;
				//exit(1);
			}
			lastRetrievedElementType.push(elementType);

			//// Create GEP to compute the address
			//Value* gep = Builder->CreateGEP(elementType, basePtr, index, "arrayidx");

			auto gep = Builder->CreateStructGEP(structDefinition->structVal, basePtr, memberIndex, "struct_member");
			baseType = elementType;

			// If this is an lvalue (for assignment), return the pointer gep
			if (lvalue)
				return gep;
			// If rvalue, return value
			else
				return Builder->CreateLoad(elementType, gep, "member_load");
		}
		// Handle if member function call
		else if (childNodes[1]->nodeType == Function_Call) {
			memberName = structDefinition->name + "." + memberName;
			childNodes[1]->token->first = memberName;

			ASTNode* argsNode = childNodes[1]->childNodes[0];
			std::vector<ASTNode*> args = std::vector<ASTNode*>();
			for (auto& a : argsNode->childNodes)
				if (a->childNodes.size() > 0) {
					args.push_back(a);
				}

			std::vector<Value*> ArgsV = std::vector<Value*>();
			argumentList argList = argumentList();
			// Add first argument, like: (this : ref structName, ...)
			ArgsV.push_back(basePtr);
			argList.push_back(argType(structDefinition->name, Struct_Type, 1, true));
			if (!ArgsV.back())
				return nullptr;
			// Add rest of argument values
			for (int i = 0; i < args.size(); i++) {
				Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
				if (wasError) {
					exit(1);
				}
				ArgsV.push_back(argVal);
				argList.push_back(argType(getStringTypeFromLLVMType(argVal->getType()), getASTNodeTypeFromString(getStringTypeFromLLVMType(argVal->getType())), 0));
				if (!ArgsV.back())
					return nullptr;
			}

			// Look up the id in the struct function.
			functionID* CalleeFID = getFunctionFromID(structDefinition->memberFunctions, memberName, argList, token, true, true);
			if (!CalleeFID) {
				printTokenError(childNodes[1]->token, "Struct definition does not contain member function");
				wasError = true;
				return nullptr;
				//exit(1);
			}

			// Call function
			Function* CalleeF = CalleeFID->fnValue;
			CalleeFID->uses++;
			isCallMemberFunction = true;

			// If argument mismatch error.
			if (CalleeFID->variableNumArguments == false)
				if (CalleeF->arg_size() != argList.size()) {
					printTokenError(token, "Incorrect number of arguments passed to function", __LINE__);
					wasError = true;
					return nullptr;
					//exit(1);
				}
				// If variable arguments, make sure the amount in call are <= the required amount
				else if (CalleeF->arg_size() > argList.size()) {
					printTokenError(token, "Incorrect number of arguments passed to function", __LINE__);
					wasError = true;
					return nullptr;
					//exit(1);
				}

			// Clear arg values list to get values correctly
			ArgsV = std::vector<Value*>();
			// Add first argument, like: (this : ref structName, ...)
			ArgsV.push_back(basePtr);
			//argList.push_back(argType(structDefinition->name, Struct_Type, 1, true));
			if (!ArgsV.back())
				return nullptr;
			// Add rest of argument values
			for (int i = 0; i < args.size(); i++) {
				if (CalleeFID->arguments[i].isReference) {
					if (args[0]->childNodes.size() != 1 || args[0]->childNodes[0]->nodeType != Identifier_Node) {
						printTokenError(token, "Cannot pass value as reference");
						wasError = true;
						return nullptr;
						//exit(1);
					}
					args[i]->childNodes[0]->isRef = true;
				}
				Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
				if (wasError) {
					exit(1);
				}
				ArgsV.push_back(argVal);
				if (!ArgsV.back())
					return nullptr;
			}
			bool wasDefined = true;
			lastRetrievedElementType.push(getLLVMTypeFromString(CalleeFID->returnType, 0, token, wasDefined, pass));

			isCallMemberFunction = false;
			baseType = lastRetrievedElementType.top();

			return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
		}
	}
	// If left is not pointer, assume another member access or index operator
	else {
		structType* structDefinition = getStructTypeFromLLVMType(lastRetrievedElementType.top());
		lastRetrievedElementType.pop();

		if (structDefinition == nullptr)
			exit(1);

		// Get member name and index
		std::string memberName = childNodes[1]->token->first;
		// Handle if member variable access/set
		if (childNodes[1]->nodeType == Identifier_Node) {
			if (structDefinition->memberNameIndexes.find(memberName) == structDefinition->memberNameIndexes.end()) {
				printTokenError(childNodes[1]->token, "Struct definition does not contain member");
				wasError = true;
				return nullptr;
				//exit(1);
			}


			uint16_t memberIndex = structDefinition->memberNameIndexes[memberName];

			bool wasDefined = true;
			Type* elementType = getLLVMTypeFromString(structDefinition->members[memberIndex].typeString, 0, token, wasDefined, pass);
			//if (wasDefined == false)
			//	return nullptr;
			//Type* elementType = getLLVMTypeFromString(v->type, -1, childNodes[0]->token);
			//Type* elementType = getLLVMTypeFromString(baseType);
			if (!elementType) {
				printTokenError(token, "Invalid element type");
				wasError = true;
				return nullptr;
				//exit(1);
			}
			lastRetrievedElementType.push(elementType);

			//// Create GEP to compute the address
			//Value* gep = Builder->CreateGEP(elementType, basePtr, index, "arrayidx");

			auto gep = Builder->CreateStructGEP(structDefinition->structVal, basePtr, memberIndex, "struct_member");
			baseType = elementType;

			// If this is an lvalue (for assignment), return the pointer gep
			if (lvalue)
				return gep;
			// If rvalue, return value
			else
				return Builder->CreateLoad(elementType, gep, "member_load");
		}
		// Handle if member function call
		else if (childNodes[1]->nodeType == Function_Call) {
			memberName = structDefinition->name + "." + memberName;
			childNodes[1]->token->first = memberName;

			ASTNode* argsNode = childNodes[1]->childNodes[0];
			std::vector<ASTNode*> args = std::vector<ASTNode*>();
			for (auto& a : argsNode->childNodes)
				if (a->childNodes.size() > 0) {
					args.push_back(a);
				}

			std::vector<Value*> ArgsV = std::vector<Value*>();
			argumentList argList = argumentList();
			// Add first argument, like: (this : ref structName, ...)
			ArgsV.push_back(basePtr);
			argList.push_back(argType(structDefinition->name, Struct_Type, 1, true));
			if (!ArgsV.back())
				return nullptr;
			// Add rest of argument values
			for (int i = 0; i < args.size(); i++) {
				Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
				if (wasError) {
					exit(1);
				}
				ArgsV.push_back(argVal);
				argList.push_back(argType(getStringTypeFromLLVMType(argVal->getType()), getASTNodeTypeFromString(getStringTypeFromLLVMType(argVal->getType())), 0));
				if (!ArgsV.back())
					return nullptr;
			}

			// Look up the id in the struct function.
			functionID* CalleeFID = getFunctionFromID(structDefinition->memberFunctions, memberName, argList, token, true, true);
			if (!CalleeFID) {
				printTokenError(childNodes[1]->token, "Struct definition does not contain member function");
				wasError = true;
				return nullptr;
				//exit(1);
			}

			// Call function
			Function* CalleeF = CalleeFID->fnValue;
			CalleeFID->uses++;
			isCallMemberFunction = true;

			// If argument mismatch error.
			if (CalleeFID->variableNumArguments == false)
				if (CalleeF->arg_size() != argList.size()) {
					printTokenError(token, "Incorrect number of arguments passed to function", __LINE__);
					wasError = true;
					return nullptr;
					//exit(1);
				}
				// If variable arguments, make sure the amount in call are <= the required amount
				else if (CalleeF->arg_size() > argList.size()) {
					printTokenError(token, "Incorrect number of arguments passed to function", __LINE__);
					wasError = true;
					return nullptr;
					//exit(1);
				}

			// Clear arg values list to get values correctly
			ArgsV = std::vector<Value*>();
			// Add first argument, like: (this : ref structName, ...)
			ArgsV.push_back(basePtr);
			//argList.push_back(argType(structDefinition->name, Struct_Type, 1, true));
			if (!ArgsV.back())
				return nullptr;
			// Add rest of argument values
			for (int i = 0; i < args.size(); i++) {
				if (CalleeFID->arguments[i].isReference) {
					if (args[0]->childNodes.size() != 1 || args[0]->childNodes[0]->nodeType != Identifier_Node) {
						printTokenError(token, "Cannot pass value as reference");
						wasError = true;
						return nullptr;
						//exit(1);
					}
					args[i]->childNodes[0]->isRef = true;
				}
				Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
				if (wasError) {
					exit(1);
				}
				ArgsV.push_back(argVal);
				if (!ArgsV.back())
					return nullptr;
			}
			bool wasDefined = true;
			lastRetrievedElementType.push(getLLVMTypeFromString(CalleeFID->returnType, 0, token, wasDefined, pass));

			isCallMemberFunction = false;
			baseType = lastRetrievedElementType.top();

			return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
		}
	}


	return nullptr;
}


// Value*
void* ASTNode::generateScopeBody(int pass)
{
	for (auto& c : childNodes) {
		if (c->codegen != nullptr) {
			Value* cCode = (Value*)(c->*(c->codegen))(pass);
			if (wasError) {
				exit(1);
			}
		}
		else {
			printTokenError(c->token, "Node `" + ASTNodeTypeAsString(c->nodeType) + "` does not have a code generator");
			wasError = true;
			return nullptr;
		}
	}

	return nullptr;
}

// Value*
void* ASTNode::generateCast(int pass)
{
	if (childNodes.size() < 2 || childNodes[1]->childNodes.size() == 0 || childNodes[1]->childNodes[0]->childNodes.size() == 0) {
		printTokenError(token, "Cast expression expected name followed by new type like: #cast x : float;");
		wasError = true;
		return nullptr;
		//exit(1);
	}
	std::string varName = childNodes[1]->childNodes[0]->token->first;
	valueType* val = findNamedValue(parentNode, this, varName);
	if (!val) {
		printTokenError(childNodes[1]->childNodes[0]->token, "Unknown variable name used");
		//exit(1);
	}
	AllocaInst* var = (AllocaInst*)(val->val);

	Value* value = Builder->CreateLoad(var->getAllocatedType(), var, varName + "_load");

	std::string tyVal = childNodes[1]->childNodes[0]->childNodes[0]->token->first;

	bool wasDefined = true;
	Type* toType = getLLVMTypeFromString(tyVal, 0, childNodes[1]->childNodes[0]->childNodes[0]->token, wasDefined, pass);
	//if (wasDefined == false)
	//	return nullptr;

	return castValue(value, toType, true, typeSigns[tyVal], token);

	//return Builder->CreateStore(castedValue, var);
}

// Value*
void* ASTNode::generateTypeInstance(int pass)
{
	if (childNodes.size() < 2 || childNodes[1]->childNodes.size() == 0) {
		printTokenError(token, "New expression requires type name, like #new ty;");
		wasError = true;
		return nullptr;
		//exit(1);
	}

	std::string typeName = childNodes[1]->childNodes[0]->token->first;
	if (structDefinitions.find(typeName) == structDefinitions.end()) {
		printTokenError(childNodes[1]->childNodes[0]->token, "Unknown type name used");
		wasError = true;
		return nullptr;
		//exit(1);
	}

	structType* typeVal = structDefinitions[typeName];

	// If the struct body hasn't been generated yet, generate it
	if (typeVal->structVal == nullptr)
		Value* argVal = (Value*)(typeVal->sourceNode->*(typeVal->sourceNode->codegen))(pass);
	if (wasError) {
		exit(1);
	}

	AllocaInst* var = Builder->CreateAlloca(typeVal->structVal, nullptr, "struct_alloc");

	// NEW: Zero-initialize the entire struct
	Value* structSize = ConstantInt::get(Type::getInt64Ty(*TheContext),
		TheModule->getDataLayout().getTypeAllocSize(typeVal->structVal));

	// Create memset call to zero the memory
	Function* memsetFunc = Intrinsic::getDeclaration(TheModule.get(),
		Intrinsic::memset, {var->getType(), Type::getInt64Ty(*TheContext)});

	Builder->CreateCall(memsetFunc, {
										var,												// dest
										ConstantInt::get(Type::getInt8Ty(*TheContext), 0),	// value (zero)
										structSize,											// size
										ConstantInt::get(Type::getInt1Ty(*TheContext), 0)	// is_volatile
									});

	return var;
}

// Value*
void* ASTNode::generateCallExpression(int pass)
{
	ASTNode* argsNode = childNodes[0];
	bool shouldBeMemberFunction = isCallMemberFunction;
	isCallMemberFunction = false;

	std::vector<ASTNode*> args = std::vector<ASTNode*>();
	for (auto& a : argsNode->childNodes)
		if (a->childNodes.size() > 0) {
			args.push_back(a);
		}

	std::vector<Value*> ArgsV = std::vector<Value*>();
	argumentList argList = argumentList();

	// Build argList and ArgsV WITHOUT sret initially
	for (int i = 0; i < args.size(); i++) {
		Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
		if (wasError) {
			exit(1);
		}
		ArgsV.push_back(argVal);
		argList.push_back(argType(getStringTypeFromLLVMType(argVal->getType()), getASTNodeTypeFromString(getStringTypeFromLLVMType(argVal->getType())), 0));
		if (!ArgsV.back())
			return nullptr;
	}

	// Look up the function ID using the caller's argList (without sret)
	functionID* CalleeFID = getFunctionFromID(functionIDs, token->first, argList, token, true, shouldBeMemberFunction);
	if (!CalleeFID) {
		if (shouldBeMemberFunction)
			printf("Should be member function\n");
		printTokenError(token, "Undefined function");
		for (const auto& n : functionIDs) {
			console::WriteLine(n->name + " => " + n->mangledName);
		}
		wasError = true;
		return nullptr;
		//exit(1);
	}

	Function* CalleeF = CalleeFID->fnValue;
	CalleeFID->uses++;


	bool isStructReturn = CalleeFID->isStructReturn;
	AllocaInst* sretAlloc = nullptr;
	if (isStructReturn) {
		// Get the struct type from definitions
		structType* retStruct = structDefinitions[CalleeFID->returnType];
		if (retStruct->structVal == nullptr) {
			printTokenError(token, "Struct return type not fully defined");
			wasError = true;
			return nullptr;
			//exit(1);
		}

		// Allocate space for the returned struct on the caller's stack
		sretAlloc = Builder->CreateAlloca(retStruct->structVal, nullptr, "sret_alloc");

		// Insert the sret pointer as the FIRST argument in ArgsV
		ArgsV.insert(ArgsV.begin(), sretAlloc);

		// For argList matching: Temporarily add sret to argList for validation
		// (This matches how it's stored in functionID)
		argType sretArg("*" + CalleeFID->returnType, Struct_Type, 1, false, true);
		argList.insert(argList.begin(), sretArg);
	}

	// Validate argument count (now including sret if applicable)
	if (CalleeFID->variableNumArguments == false) {
		if (CalleeF->arg_size() != ArgsV.size()) {	// Use ArgsV.size() which includes sret
			printTokenError(token, "Incorrect number of arguments passed to function (expected " + std::to_string(CalleeF->arg_size()) + ")", __LINE__);
			CalleeFID->print();
			wasError = true;
			return nullptr;
			//exit(1);
		}
	}
	else if (CalleeF->arg_size() > ArgsV.size()) {
		printTokenError(token, "Incorrect number of arguments passed to function", __LINE__);
		CalleeFID->print();
		wasError = true;
		return nullptr;
		//exit(1);
	}

	ArgsV.clear();
	for (int i = 0; i < args.size(); i++) {									   // Start from caller's args (sret is already handled)
		if (CalleeFID->arguments[i + (isStructReturn ? 1 : 0)].isReference) {  // Offset by 1 if sret
			if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
				printTokenError(token, "Cannot pass value as reference");
				wasError = true;
				return nullptr;
				//exit(1);
			}
			args[i]->childNodes[0]->isRef = true;
		}
		Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
		if (wasError) {
			exit(1);
		}
		ArgsV.push_back(argVal);
		if (!ArgsV.back())
			return nullptr;
	}

	// If struct return, re-insert sret as first arg (after rebuilding)
	if (isStructReturn) {
		ArgsV.insert(ArgsV.begin(), sretAlloc);
	}

	// Create the call (returns void for struct returns)
	Value* callResult = Builder->CreateCall(CalleeF, ArgsV, "calltmp");

	// For struct returns, return the loaded struct value (or pointer if lvalue)
	if (isStructReturn) {
		if (lvalue) {
			return sretAlloc;  // Return pointer for lvalue contexts (e.g., assignment)
		}
		else {
			structType* retStruct = structDefinitions[CalleeFID->returnType];
			return Builder->CreateLoad(retStruct->structVal, sretAlloc, "sret_load");
		}
	}

	// Non-struct: Return the call result directly
	return callResult;
}


// Value*
void* ASTNode::generateIf(int pass)
{
	ASTNode* condExpr = childNodes[0];
	if (condExpr->childNodes.size() == 0) {
		printTokenError(condExpr->token, "Expected condition expression");
		wasError = true;
		return nullptr;
		//exit(1);
	}
	condExpr = condExpr->childNodes[0];

	if (condExpr->codegen == nullptr) {
		printTokenError(condExpr->token, "Node `" + ASTNodeTypeAsString(condExpr->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}

	Value* CondV = (Value*)(condExpr->*(condExpr->codegen))(pass);
	if (wasError) {
		exit(1);
	}
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
		wasError = true;
		return nullptr;
	}

	Value* ThenV = (Value*)(scopeBody->*(scopeBody->codegen))(pass);
	if (wasError) {
		exit(1);
	}

	Builder->CreateBr(MergeBB);
	// Codegen of 'scopeBody' can change the current block, update ThenBB for the PHI.
	ThenBB = Builder->GetInsertBlock();


	// Emit else block.
	TheFunction->insert(TheFunction->end(), ElseBB);
	Builder->SetInsertPoint(ElseBB);

	ASTNode* elseBody = childNodes[2];

	if (elseBody->codegen == nullptr) {
		printTokenError(token, "Node `" + ASTNodeTypeAsString(elseBody->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}

	Value* ElseV = (Value*)(elseBody->*(elseBody->codegen))(pass);
	if (wasError) {
		exit(1);
	}

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
	std::string structName = token->first;

	// Do not create a struct with the same name
	if (structDefinitions.find(structName) != structDefinitions.end()) {
		if (structDefinitions[structName]->token != token) {
			printTokenError(token, "Struct cannot be redefined");
			wasError = true;
			return nullptr;
			//exit(1);
		}
	}

	//// On pass 0, only declare the struct without it's body
	//if (pass == 0) {
	//	structDefinitions[structName] = new structType(structName, token, this);
	//	return nullptr;
	//}

	currentStructName.push(structName);
	uint8_t generatingType = 0;	 // Generate all member variables first (0), then functions (1)
	argumentList members = argumentList();
	std::vector<Type*> fieldTypes = std::vector<Type*>();
	std::vector<std::string> fieldNames = std::vector<std::string>();
	std::vector<functionID*> memberFunctions = std::vector<functionID*>();
	std::unordered_map<std::string, uint16_t> memberNameIndexes = std::unordered_map<std::string, uint16_t>();
	uint16_t i = 0;
	for (; generatingType < 2; generatingType++)
		for (auto& fieldNode : childNodes[0]->childNodes) {
			// If it is a member variable declaration
			if (fieldNode->nodeType == Identifier_Node && generatingType == 0 && pass > 0) {
				if (fieldNode->childNodes.size() == 0) {
					printTokenError(fieldNode->token, "Member declaration must have type");
					wasError = true;
					return nullptr;
					//exit(1);
				}
				std::string memberName = fieldNode->token->first;
				ASTNode* typeNode = fieldNode;
				std::string memberType = fieldNode->childNodes[0]->token->first;
				int pointerLevel = 0;

			recurseAddMemberPointer:
				typeNode = typeNode->childNodes[0];

				if (typeNode->token->first == "*") {
					pointerLevel++;
					goto recurseAddMemberPointer;
				}

				memberType = typeNode->token->first;

				bool wasDefined = true;
				Type* fieldType = getLLVMTypeFromString(memberType, 0, typeNode->token, wasDefined, pass);
				//if (wasDefined == false)
				//	return nullptr;
				for (int i = 0; i < pointerLevel; i++)
					fieldType = fieldType->getPointerTo();
				fieldTypes.push_back(fieldType);
				fieldNames.push_back(memberName);
				memberNameIndexes[memberName] = i;

				members.push_back(argType(memberType, getASTNodeTypeFromString(memberType), pointerLevel));
				i++;
			}
			// Else it is a member function definition
			else if (fieldNode->nodeType == Compiler_Define_Function && generatingType == 1 && pass > 1) {
				// Generate function
				Function* memberFunction = (Function*)(fieldNode->*(fieldNode->codegen))(pass);
				if (wasError) {
					exit(1);
				}
				// Get pointer to generated function from global
				functionID* fnID = getFunctionIDFromFunctionPointer(functionIDs, memberFunction);
				memberFunctions.push_back(fnID);
			}
		}
	// pass 0 declare struct name,
	// pass 1 struct body and function prototypes,
	// pass 2 function bodies
	if (pass > 1) {
		currentStructName.pop();
		structDefinitions[structName]->members = members;
		structDefinitions[structName]->memberFunctions = memberFunctions;
		structDefinitions[structName]->memberNameIndexes = memberNameIndexes;
		structDefinitions[structName]->structVal->setBody(fieldTypes, false);
		return nullptr;
	}

	// Make node not be regenerated
	//currentNodeDoneGenerating = true;
	//nodeType = Fully_Defined;
	//codegen = nullptr;

	StructType* structTy = StructType::create(*TheContext, fieldTypes, "struct." + structName);


	currentStructName.pop();
	structDefinitions[structName] = new structType(structName, token, structTy, members, memberFunctions, memberNameIndexes);

	return structTy;
}

// Value*
void* ASTNode::generateFor(int pass)
{
	std::string varName = "_iterator";

	if (childNodes[0]->nodeType == Iterator) {
		varName = childNodes[0]->childNodes[0]->token->first;
	}

	ASTNode* rangeStart = childNodes[1]->childNodes[0];

	// Compute the start value.
	if (rangeStart->codegen == nullptr) {
		printTokenError(rangeStart->token, "Node `" + ASTNodeTypeAsString(rangeStart->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}
	Value* StartVal = (Value*)(rangeStart->*(rangeStart->codegen))(pass);
	if (wasError) {
		exit(1);
	}
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
	if (wasError) {
		exit(1);
	}

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
	namedValues[varName] = new valueType(varName, "int32", Alloca);

	// Emit the body of the loop
	ASTNode* scopeBody = childNodes[2];

	if (scopeBody->codegen == nullptr) {
		printTokenError(scopeBody->token, "Node `" + ASTNodeTypeAsString(scopeBody->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}
	(scopeBody->*(scopeBody->codegen))(pass);
	if (wasError) {
		exit(1);
	}

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
	std::string fnName = token->first;
	std::string mangledName = token->first;
	bool isStruct = false;
	bool isSpecial = false;

	if (fnName == "operator") {
		isSpecial = true;
		fnName = fnName + "." + tokenAsString(childNodes[0]->childNodes[0]->token->second);
		mangledName = fnName + "." + tokenAsString(childNodes[0]->childNodes[0]->token->second);
	}
	else if (fnName == "cast" || fnName == "create" || fnName == "destroy") {
		isSpecial = true;
		fnName = fnName + "." + tokenAsString(childNodes[0]->childNodes[0]->token->second);
		mangledName = fnName + "." + tokenAsString(childNodes[0]->childNodes[0]->token->second);
	}
	else if (currentStructName.size() != 0) {
		fnName = currentStructName.top() + "." + fnName;
		mangledName = currentStructName.top() + "." + mangledName;
		isStruct = true;
	}
	//else
	//	fnName = fnName;

	// Get return type
	Type* retType = Type::getVoidTy(*TheContext);
	std::string rTypeString = "";
	ASTNode* typeNode = childNodes[1];
	bool isStructReturn = false;
	if (typeNode->childNodes.size() > 0) {
	recurseAddPointer:
		typeNode = typeNode->childNodes[0];
		if (typeNode->token->first == "*")
			mangledName += ".ptr";
		else
			mangledName += "." + typeNode->token->first;
		rTypeString += typeNode->token->first;

		if (typeNode->token->first == "*") {
			goto recurseAddPointer;
		}

		bool wasDefined = true;
		retType = getLLVMTypeFromString(rTypeString, 0, typeNode->token, wasDefined, pass);

		// If the return type is a struct
		if (retType && retType->isStructTy()) {
			isStructReturn = true;
		}
	}

	argumentList userArgList = argList;

	// Handle struct return by modifying function signature
	Type* actualRetType = retType;
	if (isStructReturn) {
		// For struct returns, add sret parameter as first argument
		argTypes.push_back(retType->getPointerTo());  // sret parameter (pointer to struct)
		argNames.push_back("sret");
		argList.insert(argList.begin(), argType("*" + rTypeString, getASTNodeTypeFromString(rTypeString), 1, false, true));

		// Change actual return type to void
		actualRetType = Type::getVoidTy(*TheContext);
	}

	// Get function arguments
	// If it is a struct member function, first add a "this" argument like: (this : ref structName, ...)
	if (currentStructName.size() > 0) {
		std::string typeStr = currentStructName.top();
		bool isReference = true;
		int pointerLevel = 1;

		Type* aType = nullptr;
		argList.push_back(argType(typeStr, Struct_Type, pointerLevel, isReference, false));

		try {
			bool wasDefined = true;
			aType = getLLVMTypeFromString(typeStr, 0, token, wasDefined, pass);
			//if (wasDefined == false)
			//	return nullptr;
			for (int i = 0; i < pointerLevel; i++) {
				aType = aType->getPointerTo();
			}
		}
		catch (...) {
			printTokenError(token, "Invalid argument type given");
			wasError = true;
			return nullptr;
			//exit(1);
		}

		// Unknown type name
		// TODO: Add handling for custom structs as well

		//else if (typeName == "string")
		//Type::getStringTy(*TheContext);
		argTypes.push_back(aType);
		argNames.push_back("this");
	}
	bool variableNumArguments = false;
	for (auto& a : argsNode->childNodes) {
		if (a->childNodes.size() > 0) {
			if (variableNumArguments)  // If there is a named argument after ... then it is invalid
				goto invalidArgument;
			if (a->childNodes[0]->nodeType != Argument_List) {
				ASTNode* typeNode = a->childNodes[0]->childNodes[0];
				std::string typeStr = "";
				bool isReference = false;
				bool mustBeExactType = false;
				int pointerLevel = 0;

			gatherTypeModifiers:
				if (typeNode->token->first == "ref") {
					isReference = true;
					mangledName += ".ref";
					typeStr += "*";
					pointerLevel++;
					typeNode = typeNode->childNodes[0];
					goto gatherTypeModifiers;
				}
				if (typeNode->token->first == "exact") {
					//mangledName += ".exact";
					//typeStr += ".exact";
					mustBeExactType = true;
					typeNode = typeNode->childNodes[0];
					goto gatherTypeModifiers;
				}
				if (typeNode->token->first == "*") {
					pointerLevel++;
					mangledName += ".ptr";
					typeStr += "*";
					typeNode = typeNode->childNodes[0];
					goto gatherTypeModifiers;
				}

				mangledName += "." + typeNode->token->first;
				typeStr += typeNode->token->first;


				Type* aType = nullptr;
				argList.push_back(argType(typeStr, getASTNodeTypeFromString(typeNode->token->first), pointerLevel, isReference, mustBeExactType));
				userArgList.push_back(argType(typeStr, getASTNodeTypeFromString(typeNode->token->first), pointerLevel, isReference, mustBeExactType));

				try {
					bool wasDefined = true;
					aType = getLLVMTypeFromString(typeNode->token->first, 0, typeNode->token, wasDefined, pass);
					for (int i = 0; i < pointerLevel; i++) {
						aType = aType->getPointerTo();
					}
				}
				catch (...) {
					goto invalidArgument;
				}

				// Unknown type name
				// TODO: Add handling for custom structs as well

				//else if (typeName == "string")
				//Type::getStringTy(*TheContext);
				argTypes.push_back(aType);
				argNames.push_back(a->childNodes[0]->token->first);
				//std::cout << "Arg added: '" << a->childNodes[0]->token->first << "' of type: '" << typeName << "'\n";
			}
			// Handle ellipses ...
			else if (a->childNodes[0]->nodeType == Argument_List) {
				//std::string typeName = a->childNodes[0]->childNodes[0]->token->first;
				variableNumArguments = true;
			}
			continue;
		invalidArgument:
			printTokenError(a->token, "Invalid argument type given");
			wasError = true;
			return nullptr;
			//exit(1);
		}
	}
	bool isAlwaysInline = false;
	for (auto& m : modifiersNode->childNodes) {
		if (m->token->first == "#inline")
			isAlwaysInline = true;
		if (m->token->first == "#replaceable")
			replaceableDefinition = true;
	}


	// Don't add another prototype if the exact same one is already defined
	//Function* theFunction = TheModule->getFunction(token->first);
	functionID* theFunctionID = getExactFunctionFromID(functionIDs, fnName, userArgList, token);
	if (theFunctionID) {
		if (verbosity >= 5) {
			console::printIndent(2);
			console::Write("-- Pre-existing function definition found for: ");
			console::Write(fnName, console::yellowFGColor);
			console::WriteLine(" (" + theFunctionID->mangledName + ")", console::yellowFGColor);
		}
		return theFunctionID->fnValue;
	}


	FunctionType* FT = FunctionType::get(actualRetType, argTypes, variableNumArguments);

	Function* fn = nullptr;
	// If extern declaration, dont mangle name
	if (isExtern)
		fn = Function::Create(FT, Function::ExternalLinkage, fnName, TheModule.get());
	else
		fn = Function::Create(FT, Function::ExternalLinkage, mangledName, TheModule.get());
	if (isAlwaysInline)
		fn->addFnAttr(llvm::Attribute::AlwaysInline);

	uint16_t Idx = 0;
	for (auto& arg : fn->args()) {
		arg.setName(argNames[Idx++]);
		//namedValues[std::string(arg.getName())] = &arg;
	}

	functionIDs.push_back(new functionID(fnName, mangledName, rTypeString, argList, userArgList, fn, variableNumArguments, isStruct, isStructReturn));
	if (verbosity >= 5) {
		console::printIndent(depth + 2);
		console::WriteLine("-- Added function \"" + fnName + "\" to functionIDs");
	}

	return fn;
}

// Function*
void* ASTNode::generateFunction(int pass)
{

	// First, check for an existing function from a previous declaration.
	//Function* theFunction = TheModule->getFunction(token->first);
	functionID* theFunctionID = nullptr;
	Function* theFunction = nullptr;
	std::string functionName = token->first;
	bool isStruct = false;

	if (currentStructName.size() > 0) {
		functionName = currentStructName.top() + "." + functionName;
		isStruct = true;
	}

	//if (!theFunctionID)
	theFunction = (Function*)this->generatePrototype(pass);
	//else
	//	theFunction = theFunctionID->fnValue;

	// If the function wasn't generated, try later
	if (!theFunction) {
		return nullptr;
		//printTokenError(token, "There was a failure to create a function");
		//exit(1);
	}

	if (!theFunction->empty() && replaceableDefinition == false) {
		printTokenError(token, "Function cannot be redefined, requires unique identity");
		wasError = true;
		return nullptr;
		//exit(1);
	}

	if (pass <= 1)
		return theFunction;

	theFunctionID = getFunctionIDFromFunctionPointer(functionIDs, theFunction);
	if (!theFunctionID) {
		printTokenError(token, "There was a failure to create a function");
		wasError = true;
		return nullptr;
		//exit(1);
	}

	// Create a new basic block to start insertion into.
	BasicBlock* fnBlock = BasicBlock::Create(*TheContext, "entry", theFunction);
	Builder->SetInsertPoint(fnBlock);

	// Record the function arguments in the NamedValues map.
	// If it is a struct, first add a "this" argument like: (this : ref structName, ...)
	int i = 0;
	//if (isStruct) {
	//	if (i >= theFunctionID->arguments.size()) {
	//		printTokenError(token, "Mismatch in number of arguments, expected " + std::to_string(theFunctionID->arguments.size()), __LINE__);
	//		exit(1);
	//	}
	//	// It's a pointer/ref value, dont copy
	//	std::string baseType = "*" + currentStructName;
	//	namedValues["this"] = new valueType("this", baseType + theFunctionID->arguments[i].typeString, &(*theFunction->arg_begin()));

	//	i++;
	//}
	// Add remaining arguments
	for (auto& arg : theFunction->args()) {
		if (i >= theFunctionID->arguments.size()) {
			theFunctionID->print();
			printTokenError(token, "Mismatch in number of arguments, expected " + std::to_string(theFunctionID->arguments.size()), __LINE__);
			wasError = true;
			return nullptr;
			//exit(1);
		}
		// If regular value, create copy
		if (theFunctionID->arguments[i].pointerLevel == 0) {
			AllocaInst* Alloca = CreateEntryBlockAlloca(theFunction, arg.getType(), arg.getName());
			// Store the initial value into the alloca.
			Builder->CreateStore(&arg, Alloca);
			namedValues[std::string(arg.getName())] = new valueType(std::string(arg.getName()), theFunctionID->arguments[i].typeString, Alloca);
		}
		// If a pointer/ref value, dont copy
		else {
			std::string baseType = "";
			for (int p = 0; p < theFunctionID->arguments[i].pointerLevel - 1; p++)
				baseType += "*";
			namedValues[std::string(arg.getName())] = new valueType(std::string(arg.getName()), baseType + theFunctionID->arguments[i].typeString, &arg);
		}

		i++;
	}
	//NamedValues[std::string(Arg.getName())] = &Arg;

	ASTNode* body;
	for (auto& n : childNodes)
		if (n->nodeType == Scope_Body)
			body = n;

	if (body->codegen == nullptr) {
		printTokenError(body->token, "Node `" + ASTNodeTypeAsString(body->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}

	(body->*(body->codegen))(pass);
	if (wasError) {
		exit(1);
	}

	// Create default return at end of function
	if (theFunction->getReturnType()->isVoidTy()) {
		Builder->CreateRetVoid();
	}
	else {
		// Default return value (adapt based on return type, e.g., 0 for i32)
		Value* defaultRet = ConstantInt::get(theFunction->getReturnType(), 0);
		Builder->CreateRet(defaultRet);
	}

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

// Nothing
void* ASTNode::generateNothing(int pass)
{
	return nullptr;
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

void removeUnusedPrototypes()
{
	for (auto& fn : functionIDs)
		if (fn->name != "main")
			if (fn->uses == 0)
				fn->fnValue->eraseFromParent();
}
