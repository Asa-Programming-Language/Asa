#include "codegen.h"


std::unique_ptr<LLVMContext> TheContext;
std::unique_ptr<Module> TheModule;
std::unique_ptr<DIBuilder> DBuilder;
static DICompileUnit* TheCU;
static DIFile* TheFile;
static std::vector<DIScope*> LexicalBlocks;
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
std::map<std::string, std::string> compilerDefines;

struct GlobalInit {
	llvm::GlobalVariable* gv;
	ASTNode* exprStmtNode;
};

Function* globalInitFn = nullptr;
std::vector<GlobalInit> globalInitList;

// Module registry: module name -> its Compiler_Define ASTNode.
// Used by member-access codegen (Fore.black -> look in registry["Fore"]->namedValues).
std::unordered_map<std::string, ASTNode*> moduleRegistry;

// Returns the allocated element type for either an AllocaInst or GlobalVariable.
inline Type* getValueStoredType(Value* ptr)
{
	if (auto* A = dyn_cast<AllocaInst>(ptr))
		return A->getAllocatedType();
	if (auto* G = dyn_cast<GlobalVariable>(ptr))
		return G->getValueType();
	return ptr->getType();
}

llvm::Value* castValue(llvm::Value* value, llvm::Type* destType, bool isSrcSigned, bool isToSigned, tokenPair*& token, bool destTypeIsStruct = false);

std::unordered_map<std::string, Type*> unresolvedTypes;
std::stack<ASAType*> lastRetrievedElementType;
std::stack<Value*> pipeOperationValue;

// Stack to track loop contexts (for break/continue)
struct LoopContext {
	BasicBlock* continueBB;	 // Block to jump to for continue
	BasicBlock* breakBB;	 // Block to jump to for break
	std::string label;		 // Optional label for labeled break/continue
};
std::stack<LoopContext> loopContextStack;

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
	bool isConstant = false;
	bool mustBeExactType = false;
	argType(std::string ts, ASTNodeType bT, uint8_t pL = 0, bool r = false, bool ex = false, bool c = false)
	{
		typeString = ts;
		baseASTType = bT;
		pointerLevel = pL;
		isReference = r;
		isConstant = c;
		mustBeExactType = ex;
	}
};

typedef std::vector<argType> argumentList;

struct functionID {
	std::string name = "";
	std::string mangledName = "";
	std::string returnType = "";
	tokenPair* token = nullptr;
	argumentList arguments = argumentList();
	argumentList userArguments = argumentList();
	bool variableNumArguments = false;
	bool isStructReturn = false;
	uint32_t uses = 0;
	bool isMemberFunction = false;
	Function* fnValue = nullptr;
	functionID() {}
	functionID(std::string n, tokenPair* t, std::string mN, std::string r, argumentList llvmArgs, argumentList userArgs, Function* f, bool vA = false, bool mF = false, bool sRet = false)
	{
		name = n;
		token = t;
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
		if (returnType != "")
			console::Write(returnType + " ", console::blueFGColor);
		console::Write(name, console::greenFGColor);

		console::Write("(");
		for (int i = 0; i < userArguments.size(); i++) {
			if (userArguments[i].isReference)
				console::Write("ref ", console::magentaFGColor);
			if (userArguments[i].isConstant)
				console::Write("const ", console::magentaFGColor);
			console::Write(userArguments[i].typeString, console::blueFGColor);
			if (i < userArguments.size() - 1)
				console::Write(", ");
		}
		console::Write(")");

		console::Write("(");
		for (int i = 0; i < arguments.size(); i++) {
			if (arguments[i].isReference)
				console::Write("ref ", console::magentaFGColor);
			if (arguments[i].isConstant)
				console::Write("const ", console::magentaFGColor);
			console::Write(arguments[i].typeString, console::blueFGColor);
			if (i < arguments.size() - 1)
				console::Write(", ");
		}
		console::Write(")");

		if (isStructReturn) {
			console::Write(" (");
			console::Write("returns struct", console::yellowFGColor);
			console::Write(")");
		}
		console::WriteLine();
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
	// Check if two type strings are equivalent (handling type synonyms)
	bool areTypesEquivalent(const std::string& type1, const std::string& type2)
	{
		if (type1 == type2)
			return true;

		// Handle int8/char synonyms
		if ((type1 == "int8" || type1 == "char") && (type2 == "int8" || type2 == "char"))
			return true;
		if ((type1 == "uint8" || type1 == "uchar") && (type2 == "uint8" || type2 == "uchar"))
			return true;

		// Handle int/int32 synonyms
		if ((type1 == "int" || type1 == "int32") && (type2 == "int" || type2 == "int32"))
			return true;
		if ((type1 == "uint" || type1 == "uint32") && (type2 == "uint" || type2 == "uint32"))
			return true;

		return false;
	}

	uint16_t compareMatch(std::string n, argumentList a, bool wereTypesInferred = false)
	{
		uint16_t differences = 0;
		if (n != name)
			return 1000;
		if (userArguments.size() != a.size())
			return 1000 - 1;
		if (verbosity >= 6)
			console::WriteLine("Comparing: " + name, console::blueFGColor);
		console::indentation++;
		for (int i = 0; i < userArguments.size(); i++) {
			ASTNodeType t1 = userArguments[i].baseASTType;
			ASTNodeType t2 = a[i].baseASTType;
			bool mustBeExactType = userArguments[i].mustBeExactType;

			// First check if both the base type AND pointer level match exactly
			if (areTypesEquivalent(userArguments[i].typeString, a[i].typeString) &&
				userArguments[i].pointerLevel == a[i].pointerLevel) {
				differences += 0;
				continue;
			}
			if (verbosity >= 6) {
				if (userArguments[i].typeString != a[i].typeString)
					console::WriteLine("[" + std::to_string(i) + "] typeString: " + userArguments[i].typeString + "!=" + a[i].typeString);
				if (userArguments[i].pointerLevel != a[i].pointerLevel)
					console::WriteLine("[" + std::to_string(i) + "] pointerLevel: " + std::to_string(userArguments[i].pointerLevel) + "!=" + std::to_string(a[i].pointerLevel));
			}

			// If pointer levels differ, these are fundamentally different types —
			// with two implicit conversion exceptions involving string:
			//   string → *char  (extracts .address at call site)
			//   *char  → string (wraps in struct at call site)
			if (userArguments[i].pointerLevel != a[i].pointerLevel) {
				bool isStringToCharPtr =
					userArguments[i].pointerLevel == 1 &&
					(userArguments[i].typeString == "char" || userArguments[i].typeString == "int8") &&
					a[i].pointerLevel == 0 && a[i].typeString == "string";
				bool isCharPtrToString =
					userArguments[i].pointerLevel == 0 && userArguments[i].typeString == "string" &&
					a[i].pointerLevel == 1 && (a[i].typeString == "char" || a[i].typeString == "int8");
				if (isStringToCharPtr || isCharPtrToString) {
					differences += 10;
					continue;
				}
				differences = 1000;
				goto returnDifferences;
			}

			// If they are the same base type (ignoring signedness for LLVM types)
			if (compareASTNodeTypes(t1, t2, wereTypesInferred)) {
				differences += 0;
			}
			// Else if they are both integer types (and same pointer level)
			else if (t1 >= Integer_Node && t1 <= Boolean_Node) {
				if (mustBeExactType) {	// If the argument type must be exact, but aren't
					differences = 500;
					goto returnDifferences;
				}
				if (t2 >= Integer_Node && t2 <= Boolean_Node)  // If similar type
					differences += abs(t1 - t2);
				else {
					differences = 600;	// Trying to match integer with non-integer
					goto returnDifferences;
				}
			}
			// Else if they are both float types (and same pointer level)
			else if (t1 >= Double_Type && t1 <= Half_Type) {
				if (mustBeExactType) {	// If the argument type must be exact but aren't
					differences = 500;
					goto returnDifferences;
				}
				if (t2 >= Double_Type && t2 <= Half_Type)  // If similar type
					differences += abs(t1 - t2);
				else {
					differences = 600;	// Trying to match float with non-float
					goto returnDifferences;
				}
			}
			// If we get here, the types don't match at all
			else {
				differences = 800;
				goto returnDifferences;
			}
		}
	returnDifferences:
		if (verbosity >= 6)
			console::WriteLine("returning difference of: " + std::to_string(differences));
		console::indentation--;
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
	std::unordered_map<std::string, ASTNode*> memberDefaultNodes;  // member name → default value AST node
	uint32_t uses = 0;
	std::vector<functionID*> memberFunctions;
	StructType* structVal = nullptr;
	tokenPair* token = nullptr;
	ASTNode* sourceNode = nullptr;
	ASAType* asaType = nullptr;
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

DIType* createDIType(Type* llvmType, const std::string& typeString)
{
	if (!llvmType || !DBuilder)
		return nullptr;

	// Handle basic types
	if (llvmType->isIntegerTy()) {
		unsigned bitWidth = llvmType->getIntegerBitWidth();
		unsigned encoding = dwarf::DW_ATE_signed;

		// Check if unsigned
		if (typeString.find("uint") != std::string::npos ||
			typeString == "bool" || typeString == "char" || typeString == "uchar") {
			encoding = dwarf::DW_ATE_unsigned;
		}

		return DBuilder->createBasicType(typeString, bitWidth, encoding);
	}
	else if (llvmType->isFloatTy()) {
		return DBuilder->createBasicType("float", 32, dwarf::DW_ATE_float);
	}
	else if (llvmType->isDoubleTy()) {
		return DBuilder->createBasicType("double", 64, dwarf::DW_ATE_float);
	}
	else if (llvmType->isPointerTy()) {
		// For pointers, create a pointer type
		DIType* pointeeTy = DBuilder->createBasicType("void", 8, dwarf::DW_ATE_address);
		return DBuilder->createPointerType(pointeeTy,
			TheModule->getDataLayout().getPointerSizeInBits());
	}

	// Default fallback
	return nullptr;
}

structType* getStructTypeFromLLVMType(Type*& t)
{
	for (const auto& [key, value] : structDefinitions) {
		if ((Type*)(value->structVal) == (Type*)t) {
			return value;
		}
	}
	return nullptr;
}

void printFunctionPrototypes()
{
	console::WriteLine("\nFunction Prototypes:", console::greenFGColor);
	console::indentation++;
	for (const auto& f : functionIDs) {
		console::Write("> ");
		f->print();
	}
	console::indentation--;
}

void printFunctionDifferences(argumentList& arguments, functionID*& other)
{
	console::Write(other->name + " :: (");
	for (int i = 0; i < arguments.size(); i++) {
		argType a = arguments[i];
		argType b = other->arguments[i];
		if (a.typeString == b.typeString)
			console::Write(a.typeString, console::greenFGColor);
		else
			console::Write(a.typeString + " != " + b.typeString, console::redFGColor);
		if (i < arguments.size() - 1)
			console::Write(", ");
	}
	console::WriteLine(")");
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
	if (bestScore > 500)
		return nullptr;
	// If the best function match requires exact typing (and different types are passed) throw error
	if (requiresExact) {
		printTokenError(t, "Function match not found, closest prototype requires exact types. Did you try casting?");
		printFunctionDifferences(arguments, best);
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
		if (verbosity >= 6)
			console::Write("score: " + std::to_string(score) + "  ");
		if (score < bestScore) {
			best = f;
			bestScore = score;
			if (score == 500)
				requiresExact = true;
			else
				requiresExact = false;
		}
	}
	if (bestScore > 500)
		goto exactFnNotFound;
	// If the best function match requires exact typing (and different types are passed) throw error
	if (requiresExact) {
		goto exactFnNotFound;
		//printTokenError(t, "Function match not found, closest prototype requires exact types.\nDid you try casting?");
	}
	if (bestScore == 0) {
		return best;
		if (verbosity >= 6)
			console::WriteLine("Exact function match found");
	}
exactFnNotFound:
	if (verbosity >= 6)
		console::WriteLine("Exact function match not found");
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
		printTokenError(t, "Function match not found, closest prototype requires exact types. Did you try casting?");
		//printFunctionDifferences(arguments, best);
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
		printTokenError(t, "Function match not found, closest prototype requires exact types. Did you try casting?");
		//printFunctionDifferences(arguments, best);
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
	wasError = true;
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
					return nullptr;
				}
			}
			// If this type is inside of a struct and the type *is* the struct,
			// throw an error (nested structs aren't allowed)
			else {
				wasDefined = false;
				printTokenError(token, "Cannot nest struct in self");
				wasError = true;
				return nullptr;
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

	// For opaque pointers in LLVM 15+, we can't introspect pointer element types
	// So we skip the dereference loop for now - just count that it's a pointer
	// This means pointer types will show as "unknown" which is a limitation
	// that needs to be addressed by tracking type info separately
	while (baseType->isPointerTy()) {
		pointerLevel++;
		// getPointerElementType() is deprecated in LLVM 15+
		// baseType = baseType->getPointerElementType();
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

		// For struct types, get the name directly from LLVM
		if (baseType->isStructTy()) {
			llvm::StructType* structType = static_cast<llvm::StructType*>(baseType);
			if (structType->hasName()) {
				std::string fullName = structType->getName().str();
				// Strip "struct." prefix if present
				if (fullName.rfind("struct.", 0) == 0) {
					baseTypeName = SplitString(fullName, ".")[1];
				}
				else {
					baseTypeName = fullName;
				}
			}
		}
	}
	if (verbosity >= 6)
		console::WriteLine("Returning: " + baseTypeName);

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
	return;
}

void initializeCodeGenerator()
{
	// Open a new context and module.
	TheContext = std::make_unique<LLVMContext>();
	TheModule = std::make_unique<Module>("asa_global", *TheContext);

	TheModule->addModuleFlag(Module::Warning, "Debug Info Version", DEBUG_METADATA_VERSION);
	// For Darwin/macOS compatibility (optional but recommended)
	TheModule->addModuleFlag(Module::Warning, "Dwarf Version", 4);
	DBuilder = std::make_unique<DIBuilder>(*TheModule);

	// Create debug builder
	DBuilder = std::make_unique<DIBuilder>(*TheModule);

	// Create compile unit
	TheFile = DBuilder->createFile(baseFileName, projectDirectory);
	TheCU = DBuilder->createCompileUnit(
		dwarf::DW_LANG_C,  // Or create your own language constant
		TheFile,
		COMPILER_PRINTOUT,		// Producer
		optimizationLevel > 0,	// IsOptimized
		"",						// Flags
		0						// Runtime Version
	);

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

valueType* findNamedValue(ASTNode* node, ASTNode* childNode, std::string& identifier, tokenPair*& token)
{
	// First look in self
	if (node->namedValues.find(identifier) != node->namedValues.end()) {
		return node->namedValues[identifier];
	}

	// Search through child nodes, but with proper scoping rules
	for (auto& c : node->childNodes) {
		// At nested scopes (depth > 0), only search up to the point where it's used
		if (node->depth > 0 && c == childNode) {
			break;
		}

		// Module-scope nodes (Fore, Back, etc.) hold their variables privately;
		// those are only reachable via explicit Module.member access, not by name.
		if (c->isModuleScope)
			continue;

		// At global scope (depth == 0), only search in direct children that are
		// expression statements or variable declarations, not in function bodies
		if (node->depth == 0) {
			// Function definitions store their parameters in namedValues, but those
			// are local to the function and must NOT be visible at global scope.
			if (c->nodeType == Compiler_Define_Function ||
				c->nodeType == Compiler_Define_Cast ||
				c->nodeType == Compiler_Define_Struct)
				continue;
			// Only check the child's own namedValues, don't recurse into it
			if (c->namedValues.find(identifier) != c->namedValues.end()) {
				return c->namedValues[identifier];
			}
		}
		else {
			// At nested scopes, check child and its descendants
			if (c->namedValues.find(identifier) != c->namedValues.end()) {
				return c->namedValues[identifier];
			}
		}
	}

	// Search recursively upward, but stop at global scope
	if (node->depth > 0)
		return findNamedValue(node->parentNode, node, identifier, token);

	// Variable not found - check if we're in a member function
	if (!currentStructName.empty()) {
		std::string structName = currentStructName.top();
		if (structDefinitions.find(structName) != structDefinitions.end()) {
			structType* structDef = structDefinitions[structName];

			if (structDef->memberNameIndexes.find(identifier) != structDef->memberNameIndexes.end()) {
				printTokenError(token, "Variable '" + identifier + "' not found. Did you mean 'this." + identifier + "'?");
				console::Write("Tip: ", console::yellowFGColor);
				console::WriteLine("Member functions must use 'this' to access member variables.\n");
				wasError = true;
				return nullptr;
			}
		}
	}

	return nullptr;
}

std::string getMemberAccessTypeString(ASTNode* node, ASTNode* parentNode, tokenPair*& token)
{
	// Base case: if it's just an identifier, look it up normally
	if (node->nodeType == Identifier_Node) {
		// Check if it's a module name (not a variable)
		auto modIt = moduleRegistry.find(node->token->first);
		if (modIt != moduleRegistry.end())
			return "__module__:" + node->token->first;

		valueType* val = findNamedValue(parentNode, nullptr, node->token->first, token);
		if (!val && !wasError) {
			printTokenError(token, "Unknown variable name: " + node->token->first);
			wasError = true;
			return "";
		}
		return val ? val->type : "";
	}

	// Handle member access: left.right
	if (node->nodeType == Member_Access) {
		if (node->childNodes.size() < 2) {
			printTokenError(token, "Invalid member access expression");
			wasError = true;
			return "";
		}

		ASTNode* leftNode = node->childNodes[0];
		ASTNode* rightNode = node->childNodes[1];

		// Get the type of the left side (recursively handles nested member access)
		std::string leftType = getMemberAccessTypeString(leftNode, parentNode, token);
		if (leftType.empty() || wasError)
			return "";

		// Check if left side is a module (type string "__module__:ModuleName")
		if (leftType.size() > 11 && leftType.substr(0, 11) == "__module__:") {
			std::string modName = leftType.substr(11);
			auto modIt = moduleRegistry.find(modName);
			if (modIt != moduleRegistry.end()) {
				std::string memberName = rightNode->token->first;
				auto varIt = modIt->second->namedValues.find(memberName);
				if (varIt != modIt->second->namedValues.end())
					return varIt->second->type;
				// Also check nested sub-modules
				auto subModIt = moduleRegistry.find(modName + "." + memberName);
				if (subModIt != moduleRegistry.end())
					return "__module__:" + modName + "." + memberName;
			}
			printTokenError(token, "Module has no member '" + rightNode->token->first + "'");
			wasError = true;
			return "";
		}

		// Remove pointer markers to get the struct name
		std::string structName = leftType;
		while (structName[0] == '*') {
			structName = structName.substr(1);
		}

		// Look up the struct definition
		if (structDefinitions.find(structName) == structDefinitions.end()) {
			printTokenError(token, "Type \"" + structName + "\" is not a defined struct");
			wasError = true;
			return "";
		}

		structType* structDef = structDefinitions[structName];

		// If the struct body hasn't been generated yet, we might not have member info
		if (structDef->members.empty()) {
			printTokenError(token, "Struct \"" + structName + "\" has no defined members");
			wasError = true;
			return "";
		}

		// Get the member name
		std::string memberName = rightNode->token->first;

		// Look up the member in the struct
		if (structDef->memberNameIndexes.find(memberName) == structDef->memberNameIndexes.end()) {
			printTokenError(rightNode->token, "Struct \"" + structName + "\" has no member named \"" + memberName + "\"");
			wasError = true;
			return "";
		}

		uint16_t memberIndex = structDef->memberNameIndexes[memberName];

		// Build the member type string with pointer levels
		std::string memberType = "";
		for (int i = 0; i < structDef->members[memberIndex].pointerLevel; i++) {
			memberType += "*";
		}
		memberType += structDef->members[memberIndex].typeString;

		return memberType;
	}

	// Handle 'this' keyword in member functions
	if (node->token->first == "this" && !currentStructName.empty()) {
		return currentStructName.top();
	}

	printTokenError(token, "Cannot determine type of expression");
	wasError = true;
	return "";
}


// Declare one Expression_Statement as an LLVM global.
// ownerNode: the node in whose namedValues the valueType is stored.
//   Root-level vars: ownerNode == exprStmtNode (so findNamedValue can find it).
//   Module vars: ownerNode == the Compiler_Define module node.
void declareModuleScopeVariable(ASTNode* exprStmtNode, ASTNode* ownerNode, bool isModuleVar)
{
	if (exprStmtNode->childNodes.size() < 2)
		return;
	ASTNode* leftNode = exprStmtNode->childNodes[0];
	if (leftNode->nodeType != Colon_Separator_Node || leftNode->childNodes.size() < 2)
		return;

	ASTNode* nameNode = leftNode->childNodes[0];
	ASTNode* typeNode = leftNode->childNodes[1];
	std::string varName = nameNode->token->first;

	// Walk pointer stars
	int pointerLevel = 0;
	while (typeNode->token->first == "*" && !typeNode->childNodes.empty()) {
		pointerLevel++;
		typeNode = typeNode->childNodes[0];
	}
	std::string typeName = typeNode->token->first;

	bool wasDefined = true;
	int pass = 1;
	Type* llvmType = getLLVMTypeFromString(typeName, 0, typeNode->token, wasDefined, pass);
	if (!llvmType || !wasDefined)
		return;
	for (int i = 0; i < pointerLevel; i++)
		llvmType = llvmType->getPointerTo();

	std::string globalName = varName;
	GlobalVariable* gv = new GlobalVariable(
		*TheModule, llvmType, false,
		GlobalValue::InternalLinkage,
		Constant::getNullValue(llvmType),
		globalName);

	std::string actualType = std::string(pointerLevel, '*') + typeName;
	valueType* vt = new valueType(varName, actualType, gv);

	// For root-level vars, store in the expression stmt's own namedValues so
	// findNamedValue (which checks direct children of root) can find it.
	// For module vars, store in the module node's namedValues (accessible only
	// via Fore.black member access, not by direct lookup because isModuleScope
	// makes findNamedValue skip it).
	ownerNode->namedValues[varName] = vt;

	globalInitList.push_back({gv, exprStmtNode});

	if (!globalInitFn) {
		FunctionType* ft = FunctionType::get(Type::getVoidTy(*TheContext), false);
		globalInitFn = Function::Create(
			ft, Function::InternalLinkage, "__asa_global_init", *TheModule);
	}
}

// Register a Compiler_Define (module) node and declare all its variable globals.
void processModuleForDeclarations(ASTNode* moduleCompilerDefineNode, std::string parentName)
{
	std::string moduleName = moduleCompilerDefineNode->token->first;
	std::string fullName = parentName.empty() ? moduleName : (parentName + "." + moduleName);
	moduleCompilerDefineNode->isModuleScope = true;
	moduleRegistry[fullName] = moduleCompilerDefineNode;

	// Navigate: Compiler_Define -> Scope_Body -> Module_Define_Node -> inner Scope_Body
	if (moduleCompilerDefineNode->childNodes.empty())
		return;
	ASTNode* outerScope = moduleCompilerDefineNode->childNodes[0];
	if (outerScope->nodeType != Scope_Body || outerScope->childNodes.empty())
		return;
	ASTNode* moduleDef = outerScope->childNodes[0];
	if (moduleDef->nodeType != Module_Define_Node || moduleDef->childNodes.empty())
		return;
	ASTNode* innerScope = moduleDef->childNodes[0];
	if (innerScope->nodeType != Scope_Body)
		return;

	for (auto& child : innerScope->childNodes) {
		if (child->nodeType == Expression_Statement)
			declareModuleScopeVariable(child, moduleCompilerDefineNode, true);
		// Recurse into nested sub-modules, registering with compound name (e.g. "Nested.Inner")
		else if (child->nodeType == Compiler_Define)
			processModuleForDeclarations(child, fullName);
	}
}

// Fill in __asa_global_init's body and finalize it.
void finalizeGlobalInit()
{
	if (!globalInitFn || globalInitList.empty())
		return;

	BasicBlock* BB = BasicBlock::Create(*TheContext, "entry", globalInitFn);
	Builder->SetInsertPoint(BB);

	for (auto& gi : globalInitList) {
		ASTNode* node = gi.exprStmtNode;
		GlobalVariable* gv = gi.gv;

		if (node->childNodes.size() < 2)
			continue;
		ASTNode* exprTerm = node->childNodes[1];
		if (!exprTerm || !exprTerm->codegen)
			continue;

		Value* initVal = (Value*)(exprTerm->*(exprTerm->codegen))(2);
		if (wasError) {
			wasError = false;
			continue;
		}
		if (!initVal)
			continue;

		Type* gvType = gv->getValueType();
		if (initVal->getType() != gvType) {
			if (gvType->isStructTy() && initVal->getType()->isPointerTy()) {
				// If it's a string struct { ptr, i32 } and we have a *char, build it properly
				StructType* st = cast<StructType>(gvType);
				if (st->getNumElements() == 2 &&
					st->getElementType(0)->isPointerTy() &&
					st->getElementType(1)->isIntegerTy(32)) {
					FunctionCallee strlenFn = TheModule->getOrInsertFunction("strlen",
						FunctionType::get(Type::getInt64Ty(*TheContext),
							{PointerType::getUnqual(*TheContext)}, false));
					Value* lenVal = Builder->CreateCall(strlenFn, {initVal}, "strlen");
					Value* lenTrunc = Builder->CreateTrunc(lenVal, Type::getInt32Ty(*TheContext), "len");
					Value* strStruct = UndefValue::get(gvType);
					strStruct = Builder->CreateInsertValue(strStruct, initVal, {0});
					strStruct = Builder->CreateInsertValue(strStruct, lenTrunc, {1});
					initVal = strStruct;
				}
				else {
					initVal = Builder->CreateLoad(gvType, initVal, "gv_load");
				}
			}
			else {
				initVal = castValue(initVal, gvType, false, false, node->token);
				if (wasError || !initVal) {
					wasError = false;
					continue;
				}
			}
		}
		Builder->CreateStore(initVal, gv);
	}

	Builder->CreateRetVoid();
	Builder->ClearInsertionPoint();
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
		return Builder->CreateIntCast(value, destType, isSrcSigned, "cast");

	if (srcType->isIntegerTy() && destType->isFloatingPointTy())
		return isSrcSigned ? Builder->CreateSIToFP(value, destType, "cast")
						   : Builder->CreateUIToFP(value, destType, "cast");

	if (srcType->isFloatingPointTy() && destType->isIntegerTy())
		return isToSigned ? Builder->CreateFPToSI(value, destType, "cast")
						  : Builder->CreateFPToUI(value, destType, "cast");

	if (srcType->isFloatingPointTy() && destType->isFloatingPointTy())
		return Builder->CreateFPCast(value, destType, "cast");

	if (srcType->isPointerTy() && destType->isPointerTy())
		return Builder->CreatePointerCast(value, destType, "cast");

	if (srcType->isPointerTy() && destType->isIntegerTy())
		return Builder->CreatePtrToInt(value, destType, "cast");

	if (srcType->isIntegerTy() && destType->isPointerTy())
		return Builder->CreateIntToPtr(value, destType, "cast");

	// Use bitcast only if size matches and none of the above applies
	if (llvm::CastInst::isBitOrNoopPointerCastable(srcType, destType, TheModule->getDataLayout()))
		return Builder->CreateBitCast(value, destType, "cast");


	printTokenError(token, "Unsupported cast");
	wasError = true;
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
	if (nodeType == Integer_Node) {
		try {
			int intVal = stoi(token->first);
			return ConstantInt::get(*TheContext, APInt(32, intVal, true));
		}
		catch (...) {
			long intVal = stol(token->first);
			return ConstantInt::get(*TheContext, APInt(64, intVal, true));
		}
	}
	else if (nodeType == Boolean_Node)
		return ConstantInt::get(*TheContext, APInt(1, token->first == "true" ? 1 : 0, false));
	else if (nodeType == Float_Node)
		return ConstantFP::get(*TheContext, APFloat(stod(token->first)));
	else if (nodeType == Void_Node) {
		// Return a null pointer constant (can be cast to any pointer type)
		return ConstantPointerNull::get(PointerType::getUnqual(*TheContext));
	}
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

		// Return a string struct { ptr, length } unless we're inside the string
		// module itself (where raw *char is needed for bootstrapping).
		if (compilerDefines["IN_STRING_MODULE"] != "true" &&
			structDefinitions.count("string") && structDefinitions["string"]->structVal) {
			StructType* strTy = cast<StructType>((Type*)structDefinitions["string"]->structVal);
			Constant* lenConst = ConstantInt::get(Type::getInt32Ty(*TheContext), (uint32_t)strValue.size());
			return ConstantStruct::get(strTy, {strPtr, lenConst});
		}

		return strPtr;	// Fallback: returns i8* pointing to the string
	}
	else if (nodeType == Character_Constant_Node) {
		std::string strValue = unescapeString(token->first.substr(1, token->first.size() - 2), token);	// remove quotes from token

		if (strValue.size() > 1) {
			printTokenError(token, "Character constant may contain only a single character");
			wasError = true;
			return nullptr;
		}

		return ConstantInt::get(*TheContext, APInt(8, strValue[0], false));
	}

	printTokenError(token, "Value could not be parsed as constant");
	wasError = true;
	return nullptr;
}

// Value*
void* ASTNode::generateVariableExpression(int pass)
{
	// Look this variable up in the function.
	valueType* val = findNamedValue(parentNode, this, token->first, token);
	if (wasError)
		return nullptr;
	if (!val) {
		Value* exprVal = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
		Type* llvmType = nullptr;
		Function* theFunction = Builder->GetInsertBlock()->getParent();
		uint16_t pointerLevel = 0;
		std::string typeName = "";

		// If variable does not have type, it is a used undefined variable
		if (childNodes.size() == 0) {
			printTokenError(token, "Undefined variable name");
			wasError = true;
			return nullptr;
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
			llvmType = getLLVMTypeFromString(typeName, 0, typeNode->token, wasDefined, pass);
			for (int i = 0; i < pointerLevel; i++)
				llvmType = llvmType->getPointerTo();
			if (typeSigns.find(typeName) != typeSigns.end())  // If builtin type
				exprVal = castValue(exprVal, llvmType, true, typeSigns[typeNode->token->first], token);
			else if (structDefinitions.find(typeName) != structDefinitions.end())  // If defined struct
				exprVal = castValue(exprVal, llvmType, true, false, token, true);
			else {
				printTokenError(token, "Unknown type");
				wasError = true;
				return nullptr;
			}
			if (wasError || exprVal == nullptr) {
				printTokenError(token, "Unable to cast");
				wasError = true;
				return nullptr;
			}
		}
		if (!asaType)
			asaType = new ASAType(llvmType);
		else
			asaType->baseLLVMType = llvmType;
		AllocaInst* targetPtr = CreateEntryBlockAlloca(theFunction, llvmType, token->first);
		std::string actualType = (pointerLevel > 0 ? std::string(pointerLevel, '*') : "") + typeName;
		namedValues[token->first] = new valueType(token->first, actualType, targetPtr);

		Builder->CreateStore(exprVal, targetPtr);

		//// Add debug info for the variable
		//if (!LexicalBlocks.empty()) {
		//	DIScope* Scope = LexicalBlocks.back();
		//	unsigned LineNo = token->lineNumber;

		//	DILocalVariable* D = DBuilder->createAutoVariable(
		//		Scope,					// Scope
		//		token->first,			// Name
		//		TheFile,				// File
		//		LineNo,					// Line
		//		/* DIType* */ nullptr,	// Type (you'll need to create appropriate DIType)
		//		true					// AlwaysPreserve
		//	);

		//	DBuilder->insertDeclare(
		//		targetPtr,												 // Storage
		//		D,														 // Variable descriptor
		//		DBuilder->createExpression(),							 // Expression
		//		DILocation::get(Scope->getContext(), LineNo, 0, Scope),	 // Location
		//		Builder->GetInsertBlock()								 // Insert at end of block
		//	);
		//}

		if (isRef || lvalue)
			return targetPtr;
		else
			return Builder->CreateLoad(targetPtr->getAllocatedType(), targetPtr, token->first + "_load");
	}
	Value* A = val->val;
	Type* valType = getValueStoredType(A);
	if (!asaType)
		asaType = new ASAType(valType);
	else
		asaType->baseLLVMType = valType;
	// Store the type string so pointer element types can be resolved later (e.g., for c[i])
	asaType->strVal = val->type;

	// Handle references: need to dereference when used as rvalue
	if (val->isReference && !isRef && !lvalue) {
		// valType is the pointer type (e.g. int*); load the pointer, then deref through it using the base type
		Value* ptr = Builder->CreateLoad(valType, A, token->first + "_ref_ptr");
		bool wasDefined = true;
		Type* baseType = getLLVMTypeFromString(val->type, 0, token, wasDefined, pass);
		if (!baseType || !wasDefined) {
			printTokenError(token, "Cannot resolve ref base type for dereference");
			wasError = true;
			return nullptr;
		}
		if (!asaType)
			asaType = new ASAType(baseType);
		else
			asaType->baseLLVMType = baseType;
		return Builder->CreateLoad(baseType, ptr, token->first + "_ref_deref");
	}

	if (isRef || lvalue)
		return A;
	else
		return Builder->CreateLoad(valType, A, token->first + "_load");
}

void* ASTNode::generateThrow(int pass)
{
	// If it has a child node, we will output it's value as a string
	ASTNode* exprNode = nullptr;
	Value* outVal = nullptr;
	if (childNodes.size() > 0) {
		exprNode = childNodes[0]->childNodes[0];
		if (exprNode->codegen == nullptr) {
			printTokenError(exprNode->token, "Node `" + ASTNodeTypeAsString(exprNode->nodeType) + "` does not have a code generator");
			wasError = true;
			return nullptr;
		}
		outVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	}
	if (wasError) {
		return nullptr;
	}

	// Create and print the prefix message first
	std::string throwPrefix = "Exception:  file: \"" + *(token->filePath) + "\"   line: " + std::to_string(token->lineNumber) + "\n    ";
	Value* prefixStr = Builder->CreateGlobalStringPtr(throwPrefix);

	// Look up print function for the prefix (char* type)
	argumentList prefixArgList;
	prefixArgList.push_back(argType("char", Char_Type, 1));
	std::string printFnName = "print";
	functionID* prefixPrintFnID = getFunctionFromID(functionIDs, printFnName, prefixArgList, token, true, false);

	if (prefixPrintFnID) {
		std::vector<Value*> prefixArgs;
		prefixArgs.push_back(prefixStr);
		Builder->CreateCall(prefixPrintFnID->fnValue, prefixArgs);
		prefixPrintFnID->uses++;
	}

	// If we have a value to print, call the builtin print function
	if (outVal) {
		// Build argument list for print function lookup
		argumentList argList;
		std::string typeStr = "";

		// TODO: Make this better for non-high-level expressions
		if (exprNode->nodeType == String_Constant_Node) {
			typeStr = "*char";
		}
		else {
			typeStr = getStringTypeFromLLVMType(outVal->getType());
		}

		// Extract pointer level from typeStr
		uint8_t pointerLevel = 0;
		std::string baseTypeStr = typeStr;
		while (baseTypeStr.length() > 0 && baseTypeStr[0] == '*') {
			pointerLevel++;
			baseTypeStr = baseTypeStr.substr(1);
		}

		argList.push_back(argType(baseTypeStr, getASTNodeTypeFromString(baseTypeStr), pointerLevel));

		// Look up the print function
		std::string printFnName = "printl";
		functionID* printFnID = getFunctionFromID(functionIDs, printFnName, argList, token, true, false);

		if (printFnID) {
			// Call the print function
			std::vector<Value*> printArgs;
			printArgs.push_back(outVal);
			Builder->CreateCall(printFnID->fnValue, printArgs);
			printFnID->uses++;
		}
	}

	// Declare exit function if not already declared
	FunctionType* exitFuncType = FunctionType::get(
		Type::getVoidTy(*TheContext),
		{Type::getInt32Ty(*TheContext)},
		false);
	FunctionCallee exitFunc = TheModule->getOrInsertFunction("exit", exitFuncType);

	// Call exit(1) to terminate the program
	Builder->CreateCall(exitFunc, {ConstantInt::get(Type::getInt32Ty(*TheContext), 1)});

	// Create an unreachable instruction since exit() doesn't return
	Builder->CreateUnreachable();

	return nullptr;
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
		return nullptr;
	}
	Function* currentFunc = Builder->GetInsertBlock()->getParent();
	functionID* fnID = getFunctionIDFromFunctionPointer(functionIDs, currentFunc);
	// Check if we're returning a struct
	Type* returnType = Builder->GetInsertBlock()->getParent()->getReturnType();
	if (fnID->isStructReturn) {
		////if (returnType->isStructTy()) {
		//// For struct returns, we need to handle this specially
		//// Option 1: If the function uses sret, copy to the sret parameter
		//if (currentFunc->hasStructRetAttr()) {
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
		//}
		//else {
		//	// Option 2: Direct struct return (for small structs)
		//	if (RetVal->getType()->isPointerTy()) {
		//		// Load the struct value from the pointer
		//		RetVal = Builder->CreateLoad(returnType, RetVal, "struct_ret_load");
		//	}
		//	Builder->CreateRet(RetVal);
		//}
	}
	else {
		// Non-struct return, handle normally
		Type* retType = Builder->GetInsertBlock()->getParent()->getReturnType();
		if (!RetVal) {
			if (retType->isVoidTy()) {
				Builder->CreateRetVoid();
			}
			else {
				// main gets implicit return 0; all other typed functions warn
				Function* currentFn = Builder->GetInsertBlock()->getParent();
				if (currentFn->getName() != "main")
					printTokenWarning(token, "returning void in function that expects a return value");
				Builder->CreateRet(Constant::getNullValue(retType));
			}
		}
		else {
			if (RetVal->getType() != retType) {
				Function* currentFn = Builder->GetInsertBlock()->getParent();
				functionID* fnID = getFunctionIDFromFunctionPointer(functionIDs, currentFn);
				bool isSigned = fnID ? typeSigns.count(fnID->returnType) && typeSigns[fnID->returnType] : true;
				RetVal = castValue(RetVal, retType, true, isSigned, token);
				if (wasError)
					return nullptr;
			}
			Builder->CreateRet(RetVal);
		}
	}

	return nullptr;
}

// Value*
void* ASTNode::generateExpression(int pass)
{
	if (!LexicalBlocks.empty() && token) {
		Builder->SetCurrentDebugLocation(
			DILocation::get(LexicalBlocks.back()->getContext(),
				token->lineNumber,	 // Line
				token->indexInLine,	 // Column
				LexicalBlocks.back()));
	}
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
		return nullptr;
	}
	// Propagate asaType from child so access operations (c[i]) can resolve element type
	if (!asaType && exprNode->asaType)
		asaType = exprNode->asaType;
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

	// Set debug location if available
	if (!LexicalBlocks.empty() && token) {
		Builder->SetCurrentDebugLocation(
			DILocation::get(LexicalBlocks.back()->getContext(),
				token->lineNumber,	// Line
				0,					// Column (use 0 if you don't track columns)
				LexicalBlocks.back()));
	}

	// Evaluate right side (rvalue)
	Value* exprVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	if (wasError) {
		return nullptr;
	}
	if (!exprVal) {
		printTokenError(token, "Set expression requires right argument");
		wasError = true;
		return nullptr;
	}

	Value* targetPtr = nullptr;
	Type* targetType = nullptr;

	// If the left side is a pointer lvalue
	if (leftNode->nodeType != Identifier_Node && leftNode->nodeType != Colon_Separator_Node) {
		leftNode->lvalue = true;
		// left side is an expression, evaluate to pointer (lvalue address)
		if (leftNode->codegen == nullptr) {
			printTokenError(leftNode->token, "Node `" + ASTNodeTypeAsString(leftNode->nodeType) + "` does not have a code generator");
			wasError = true;
			return nullptr;
		}
		targetPtr = (Value*)(leftNode->*(leftNode->codegen))(pass);
		if (wasError) {
			return nullptr;
		}
		if (!targetPtr || !targetPtr->getType()->isPointerTy()) {
			printTokenError(token, "Left side must evaluate to a pointer for assignment");
			wasError = true;
			return nullptr;
		}
		//targetType = cast<PointerType>(targetPtr->getType())->getElementType();
	}
	else {
		//targetPtr = (Value*)(leftNode->*(leftNode->codegen))(pass);
		//if (!targetPtr || !targetPtr->getType()->isPointerTy()) {
		//	printTokenError(token, "Left side must evaluate to a pointer");
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
			typeNode = leftNode->childNodes[1];
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
		}
		if (wasError || exprVal == nullptr) {
			printTokenError(token, "Unable to cast");
			wasError = true;
			return nullptr;
		}
	}

	// If the left side is a typed identifier, like: `name : int`, then set leftNode equal to just the identifier
	if (leftNode->nodeType == Colon_Separator_Node)
		if (leftNode->childNodes.size() > 0)
			leftNode = leftNode->childNodes[0];


	// If the left side is an identifier
	if (leftNode->nodeType == Identifier_Node) {
		// Simple variable: find alloca and use it as targetPtr
		valueType* val = findNamedValue(parentNode, this, leftNode->token->first, token);
		if (!val) {
			targetPtr = CreateEntryBlockAlloca(theFunction, type, leftNode->token->first);
			std::string actualType = "*int";
			if (!typeNode)
				actualType = getStringTypeFromLLVMType(type);
			else
				actualType = (pointerLevel > 0 ? std::string(pointerLevel, '*') : "") + typeNode->token->first;
			namedValues[leftNode->token->first] = new valueType(leftNode->token->first, actualType, targetPtr);

			// Add debug info ONLY if we have a valid scope and the stack is not empty
			if (DBuilder && !LexicalBlocks.empty() && token) {
				DIScope* Scope = LexicalBlocks.back();
				unsigned LineNo = token->lineNumber;

				// Create DIType
				DIType* DebugType = createDIType(type, actualType);

				if (DebugType) {
					DILocalVariable* D = DBuilder->createAutoVariable(
						Scope,
						leftNode->token->first,
						TheFile,  // Make sure TheFile is accessible
						LineNo,
						DebugType,
						true  // AlwaysPreserve
					);

					DBuilder->insertDeclare(
						targetPtr,
						D,
						DBuilder->createExpression(),
						DILocation::get(Scope->getContext(), LineNo, 0, Scope),
						Builder->GetInsertBlock());
				}
			}
		}
		else {
			// Check if trying to modify a const variable
			if (val->isConstant) {
				printTokenError(token, "Cannot modify const variable '" + leftNode->token->first + "'");
				wasError = true;
				return nullptr;
			}

			targetPtr = val->val;

			// If this is a reference, load the pointer before storing through it
			if (val->isReference) {
				targetPtr = Builder->CreateLoad(getValueStoredType(val->val), targetPtr, leftNode->token->first + "_ref_store_ptr");
				targetType = getValueStoredType(val->val);
			}
			else {
				targetType = getValueStoredType(targetPtr);
			}
		}
	}
	else
		targetType = type;

	//if (exprVal->getType() != targetType) {
	//	printTokenError(token, "Type mismatch in set expression");
	//	exprVal->getType()->print(llvm::outs());
	//	type->print(llvm::outs());
	//	//printTokenError(token, "Type mismatch in set expression.\nTypes are \"" + exprVal->getType()->getAsString() + "\" and \"" + type->getAsString() + "\"");
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
		return nullptr;
	}
	if (!R)
		return nullptr;

	ASTNodeType t = childNodes[0]->nodeType;

	switch (nodeType) {
		case Address_Of_Operation: {
			ASTNode* varNode = childNodes[0];
			valueType* val = findNamedValue(parentNode, this, varNode->token->first, token);
			if (!val && !wasError) {
				printTokenError(token, "Unknown variable name for address-of");
				wasError = true;
				return nullptr;
			}
			Value* var = (Value*)(val->val);
			return var;
		}

		case Dereference_Operation: {
			ASTNode* ptrNode = childNodes[0];
			Value* ptrVal = (Value*)(ptrNode->*(ptrNode->codegen))(pass);
			if (wasError) {
				return nullptr;
			}
			if (!ptrVal) {
				printTokenError(token, "Dereference of null pointer");
				wasError = true;
				return nullptr;
			}
			// Determine element type from allocation when available
			Type* elementType;
			if (AllocaInst* allocaVal = dyn_cast<AllocaInst>(ptrVal)) {
				elementType = allocaVal->getAllocatedType();
			}
			else {
				elementType = Type::getInt32Ty(*TheContext);  // TODO: proper type tracking for non-alloca pointers
			}
			return Builder->CreateLoad(elementType, ptrVal, "deref_tmp");
		}

		case Expression_Minus: {
			ASTNode* valueNode = childNodes[0];
			Value* v = (Value*)(valueNode->*(valueNode->codegen))(pass);
			if (wasError) {
				return nullptr;
			}
			if (!v) {
				printTokenError(token, "Cannot take negative of value");
				wasError = true;
				return nullptr;
			}
			if (v->getType()->isIntegerTy())
				return Builder->CreateNeg(v);
			else if (v->getType()->isFloatingPointTy())
				return Builder->CreateFNeg(v);
			else {
				printTokenError(token, "Cannot take negative of value");
				wasError = true;
				return nullptr;
			}
		}

		default:
			printTokenError(token, "Unknown or undefined operator");
			wasError = true;
			return nullptr;
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
		wasError = true;
		return nullptr;
	}

	if (!childNodes[0]->codegen || !childNodes[1]->codegen) {
		printTokenError(token, "Binary expression operands missing code generators");
		wasError = true;
		return nullptr;
	}

	// Check if it is the pipe operator first
	if (nodeType == Pipe_Operation) {
		// add L to stack
		Value* L = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
		pipeOperationValue.push(L);
		// then process R
		Value* R = (Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);
		// pop stack
		pipeOperationValue.pop();

		return R;
	}

	Value* L = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
	Value* R = (Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);

	if (!L || !R) {
		printTokenError(token, "Error generating term");
		wasError = true;
		return nullptr;
	}

	// Check for operator overloads first
	if (nodeType == Redefined_Operator_Expr || checkForOperatorOverload(L, R)) {
		return generateOperatorOverloadCall(L, R);
	}

	// Auto-cast to highest precision if types differ
	if (L->getType() != R->getType()) {
		if (warningFlags == W_Conversion)
			printTokenWarning(token, "Operand type mismatch, performing implicit conversion");
		castToHighestAccuracy(L, R, token);
		if (wasError) {
			return nullptr;
		}
		if (L->getType() != R->getType()) {
			printTokenError(token, "Operands to multiply are not the same type (after automatic cast)");
			wasError = true;
			return nullptr;
		}
	}

	ValueCategory category = getValueCategory(L->getType());

	switch (category) {
		case ValueCategory::Integer:
			if (!L->getType()->isIntegerTy() || !R->getType()->isIntegerTy()) {
				printTokenError(token, "Multiply: operands are not both integers");
				wasError = true;
				return nullptr;
			}
			return generateIntegerBinaryOp(L, R);
		case ValueCategory::Float:
			if (!L->getType()->isFloatTy() || !R->getType()->isFloatTy()) {
				printTokenError(token, "Multiply: operands are not both integers");
				wasError = true;
				return nullptr;
			}
			return generateFloatBinaryOp(L, R);
		case ValueCategory::Pointer:
			return generatePointerBinaryOp(L, R);
		default:
			printTokenError(token, "Unsupported operand types for binary operation");
			wasError = true;
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

void* ASTNode::generatePipePlaceholder(int pass)
{
	if (pipeOperationValue.size() > 0) {
		return pipeOperationValue.top();
	}
	printTokenError(token, "Pipe operation placeholder '%' can only be used after a pipe operation");
	return nullptr;
}

// TODO: Broken operator overload
bool ASTNode::checkForOperatorOverload(Value* L, Value* R)
{
	// Operator overloads have the format: operator.<OperatorName>
	//                             like: operator.Add(string, string)
	//                             for:  "h" + "i"
	std::string operatorName = "operator." + tokenAsString(token->second);

	// Build argumentList from L and R types
	argumentList argList;
	std::string lTypeStr = getStringTypeFromLLVMType(L->getType());
	uint8_t lPointerLevel = 0;
	std::string lBaseTypeStr = lTypeStr;
	while (lBaseTypeStr.length() > 0 && lBaseTypeStr[0] == '*') {
		lPointerLevel++;
		lBaseTypeStr = lBaseTypeStr.substr(1);
	}
	argList.push_back(argType(lBaseTypeStr, getASTNodeTypeFromString(lBaseTypeStr), lPointerLevel));

	std::string rTypeStr = getStringTypeFromLLVMType(R->getType());
	uint8_t rPointerLevel = 0;
	std::string rBaseTypeStr = rTypeStr;
	while (rBaseTypeStr.length() > 0 && rBaseTypeStr[0] == '*') {
		rPointerLevel++;
		rBaseTypeStr = rBaseTypeStr.substr(1);
	}
	argList.push_back(argType(rBaseTypeStr, getASTNodeTypeFromString(rBaseTypeStr), rPointerLevel));

	//printFunctionPrototypes();

	functionID* calleeID = getExactFunctionFromID(functionIDs, operatorName, argList, token, true);
	//functionID* calleeID = getFunctionFromID(functionIDs, operatorName, argList, token, true);
	if (calleeID != nullptr && calleeID->fnValue != nullptr)
		return true;
	else {
		if (verbosity >= 6) {
			if (calleeID == nullptr)
				console::WriteLine("No calleeID");
			else if (calleeID->fnValue == nullptr)
				console::WriteLine("No calleeID->fnValue");
		}
		return false;
	}
}

Value* ASTNode::generateOperatorOverloadCall(Value* L, Value* R)
{
	std::string operatorName = "operator." + tokenAsString(token->second);

	// Build argumentList from L and R types
	argumentList argList;
	std::string lTypeStr = getStringTypeFromLLVMType(L->getType());
	uint8_t lPointerLevel = 0;
	std::string lBaseTypeStr = lTypeStr;
	while (lBaseTypeStr.length() > 0 && lBaseTypeStr[0] == '*') {
		lPointerLevel++;
		lBaseTypeStr = lBaseTypeStr.substr(1);
	}
	argList.push_back(argType(lBaseTypeStr, getASTNodeTypeFromString(lBaseTypeStr), lPointerLevel));

	std::string rTypeStr = getStringTypeFromLLVMType(R->getType());
	uint8_t rPointerLevel = 0;
	std::string rBaseTypeStr = rTypeStr;
	while (rBaseTypeStr.length() > 0 && rBaseTypeStr[0] == '*') {
		rPointerLevel++;
		rBaseTypeStr = rBaseTypeStr.substr(1);
	}
	argList.push_back(argType(rBaseTypeStr, getASTNodeTypeFromString(rBaseTypeStr), rPointerLevel));

	functionID* calleeID = getFunctionFromID(functionIDs, operatorName, argList, token, true);

	if (!calleeID || !calleeID->fnValue) {
		if (!wasError) {
			wasError = true;
			printTokenError(token, "Expected operator overload for undefined operator `" + tokenAsString(token->second) + "`, but none were not found");
		}
		return nullptr;
	}

	calleeID->uses++;

	std::vector<Value*> ArgsV = {L, R};

	// If the operator returns a struct, we need to pass an sret pointer as the first arg
	if (calleeID->isStructReturn) {
		auto structIt = structDefinitions.find(calleeID->returnType);
		if (structIt == structDefinitions.end() || !structIt->second->structVal) {
			printTokenError(token, "Struct return type not defined for operator overload");
			wasError = true;
			return nullptr;
		}
		AllocaInst* sretAlloc = Builder->CreateAlloca(structIt->second->structVal, nullptr, "op_sret");
		ArgsV.insert(ArgsV.begin(), sretAlloc);
		Builder->CreateCall(calleeID->fnValue, ArgsV);
		return Builder->CreateLoad(structIt->second->structVal, sretAlloc, "op_overload");
	}

	return Builder->CreateCall(calleeID->fnValue, ArgsV, "op_overload");
}

// Value*
void* ASTNode::generateAccessOperation(int pass)
{
	if (childNodes.size() == 0) {
		printTokenError(token, "Access operation requires a left and right argument");
		wasError = true;
		return nullptr;
	}

	childNodes[0]->lvalue = true;  // Set flag for base to return address if needed
	Value* L = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
	if (wasError) {
		return nullptr;
	}
	Value* R = (Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);
	if (wasError) {
		return nullptr;
	}
	if (!L || !R)
		return nullptr;

	// Check if R is an integer
	if (!R->getType()->isIntegerTy()) {
		printTokenError(token, "Right argument of access operator must be an integer");
		wasError = true;
		return nullptr;
	}

	// Inherit asaType from the child if not already set
	if (!asaType && childNodes[0]->asaType)
		asaType = childNodes[0]->asaType;

	if (!asaType) {
		printTokenError(token, "Was unable to resolve type");
		wasError = true;
		return nullptr;
	}

	// Get the element type from baseType (set by member access or previous operations)
	Type* elementType = asaType->baseLLVMType;

	// If baseType is a pointer, we need to determine what it points to
	if (asaType->baseLLVMType && asaType->baseLLVMType->isPointerTy()) {
		// Try to resolve element type from the type string (e.g., "*char" -> "char")
		std::string typeStr = asaType->strVal;
		while (!typeStr.empty() && typeStr[0] == '*')
			typeStr = typeStr.substr(1);
		if (!typeStr.empty()) {
			bool wd = true;
			int resolvePass = 2;
			Type* resolved = getLLVMTypeFromString(typeStr, 0, token, wd, resolvePass);
			if (resolved)
				elementType = resolved;
		}
		else if (!lastRetrievedElementType.empty()) {
			elementType = lastRetrievedElementType.top()->baseLLVMType;
		}
		else {
			printTokenError(token, "Was unable to resolve type");
			wasError = true;
			return nullptr;
		}
	}
	// If baseType is a struct, find the first pointer member and use its element type
	else if (asaType->baseLLVMType && asaType->baseLLVMType->isStructTy()) {
		auto structIt = structDefinitions.find(asaType->strVal);
		if (structIt != structDefinitions.end() && structIt->second) {
			for (const auto& member : structIt->second->members) {
				if (member.pointerLevel > 0) {
					bool wd = true;
					int resolvePass = 2;
					Type* resolved = getLLVMTypeFromString(member.typeString, 0, token, wd, resolvePass);
					if (resolved)
						elementType = resolved;
					break;
				}
			}
		}
	}

	// Determine if we need to load the pointer first
	// L is a pointer-to-pointer when it comes from a struct member (alloca of pointer)
	// L is a direct pointer when it's a function parameter
	Value* actualPtr = L;

	// Check if L is an alloca instruction or a pointer to a pointer
	// In that case, we need to load the actual pointer value
	if (AllocaInst* allocaInst = dyn_cast<AllocaInst>(L)) {
		// L is an alloca, so we need to load the pointer value stored in it
		Type* ptrType = PointerType::getUnqual(*TheContext);
		actualPtr = Builder->CreateLoad(ptrType, L, "ptr_deref");
	}
	else if (L->getType()->isPointerTy()) {
		// Check if this is coming from a struct member access (GEP instruction)
		// by checking if the last operation was a struct GEP
		if (GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(L)) {
			// This is a GEP from struct member access, load the pointer
			Type* ptrType = PointerType::getUnqual(*TheContext);
			actualPtr = Builder->CreateLoad(ptrType, L, "ptr_deref");
		}
		// Otherwise, L is already a direct pointer (e.g., function parameter)
		// so we use it as-is
	}

	// Create GEP instruction
	Value* gep = Builder->CreateGEP(elementType, actualPtr, R, "arrayidx");

	// If this is an lvalue (for assignment), return the pointer gep
	if (lvalue)
		return gep;

	// If rvalue, load and return the value
	return Builder->CreateLoad(elementType, gep, "accessop_load");
}

// Value*
void* ASTNode::generateMemberAccess(int pass)
{
	if (childNodes.size() == 0) {
		printTokenError(token, "Member access operation reqires a left and right argument");
		wasError = true;
		return nullptr;
	}
	// Module member access: resolve via module registry, not struct GEP.
	// Handles both single-level (Mod.member) and chained (Mod.Sub.member) access.
	{
		std::string leftModType;
		if (childNodes[0]->nodeType == Identifier_Node) {
			auto modIt = moduleRegistry.find(childNodes[0]->token->first);
			if (modIt != moduleRegistry.end())
				leftModType = "__module__:" + childNodes[0]->token->first;
		}
		else if (childNodes[0]->nodeType == Member_Access) {
			// Use type resolution to detect chained module access (e.g. Nested.Inner)
			bool savedError = wasError;
			leftModType = getMemberAccessTypeString(childNodes[0], parentNode, token);
			if (wasError && leftModType.empty()) {
				wasError = savedError;	// Reset error if not a module chain
				leftModType = "";
			}
		}
		if (!leftModType.empty() && leftModType.size() > 11 && leftModType.substr(0, 11) == "__module__:") {
			std::string modName = leftModType.substr(11);
			std::string memberName = childNodes[1]->token->first;
			auto modIt = moduleRegistry.find(modName);
			if (modIt != moduleRegistry.end()) {
				ASTNode* modNode = modIt->second;
				auto varIt = modNode->namedValues.find(memberName);
				if (varIt != modNode->namedValues.end()) {
					valueType* vt = varIt->second;
					Value* gv = vt->val;
					Type* gvType = getValueStoredType(gv);
					asaType = new ASAType(gvType);
					lastRetrievedElementType.push(asaType);
					if (lvalue)
						return gv;
					return Builder->CreateLoad(gvType, gv, memberName + "_load");
				}
			}
			printTokenError(childNodes[1]->token,
				"Module '" + modName + "' has no member '" + memberName + "'");
			wasError = true;
			return nullptr;
		}
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
		return nullptr;
	}
	//Type* lastMemberType = lastRetrievedElementType.top();
	//lastRetrievedElementType.pop();
	//if (!L)
	//	return nullptr;

	//if (L->getType()->isPointerTy() == false) {
	//	printTokenError(token, "Left argument of member access operator must be a pointer type");
	//}
	//if (R->getType()->isIntegerTy() == false) {
	//	printTokenError(token, "Right argument of member access operator must be an integer");
	//}

	// Evaluate base pointer
	Value* basePtr = L;
	if (!basePtr || !basePtr->getType()->isPointerTy()) {
		printTokenError(token, "Base must be a pointer for access");
		wasError = true;
		return nullptr;
	}

	if (childNodes[0]->nodeType == Identifier_Node) {
		valueType* v = findNamedValue(this, nullptr, childNodes[0]->token->first, token);

		if (!v && !wasError) {
			printTokenError(childNodes[0]->token, "Unknown variable name used");
			wasError = true;
			return nullptr;
		}

		if (structDefinitions.find(v->type) == structDefinitions.end()) {
			printTokenError(token, "Type \"" + v->type + "\" has not been defined");
			wasError = true;
			return nullptr;
		}
		structType* structDefinition = structDefinitions[v->type];

		// If the struct body hasn't been generated yet, generate it
		if (structDefinition->structVal == nullptr)
			Value* argVal = (Value*)(structDefinition->sourceNode->*(structDefinition->sourceNode->codegen))(pass);
		if (wasError) {
			return nullptr;
		}

		// Get member name and index
		std::string memberName = childNodes[1]->token->first;
		// Handle if member variable access/set
		if (childNodes[1]->nodeType == Identifier_Node) {
			if (structDefinition->memberNameIndexes.find(memberName) == structDefinition->memberNameIndexes.end()) {
				printTokenError(childNodes[1]->token, "Struct definition does not contain member");
				console::indentation++;
				console::WriteLine("It does have:");
				console::indentation++;
				for (const auto& name : structDefinition->memberNameIndexes)
					console::WriteLine(name.first, console::cyanFGColor);
				console::indentation -= 2;
				wasError = true;
				return nullptr;
			}


			uint16_t memberIndex = structDefinition->memberNameIndexes[memberName];

			bool wasDefined = true;
			Type* elementType = getLLVMTypeFromString(structDefinition->members[memberIndex].typeString, 0, token, wasDefined, pass);
			// Apply the stored pointer level for this member (fixes pointer members like *char -> i8*)
			for (int _p = 0; _p < structDefinition->members[memberIndex].pointerLevel; ++_p) {
				elementType = elementType->getPointerTo();
			}
			//if (wasDefined == false)
			//	return nullptr;
			//Type* elementType = getLLVMTypeFromString(v->type, -1, childNodes[0]->token);
			//Type* elementType = getLLVMTypeFromString(baseType);
			if (!elementType) {
				printTokenError(token, "Invalid element type");
				wasError = true;
				return nullptr;
			}

			if (structDefinition->members[memberIndex].isConstant && lvalue) {
				printTokenError(token, "Cannot modify const member '" + memberName + "'");
				wasError = true;
				return nullptr;
			}


			//// Create GEP to compute the address
			//Value* gep = Builder->CreateGEP(elementType, basePtr, index, "arrayidx");

			auto gep = Builder->CreateStructGEP(structDefinition->structVal, basePtr, memberIndex, "struct_member");
			std::string memberStrVal = "";
			for (int _p = 0; _p < structDefinition->members[memberIndex].pointerLevel; ++_p)
				memberStrVal += "*";
			memberStrVal += structDefinition->members[memberIndex].typeString;
			asaType = new ASAType(elementType, false, structDefinition->members[memberIndex].isConstant, memberStrVal, (uint8_t)structDefinition->members[memberIndex].pointerLevel);
			lastRetrievedElementType.push(asaType);
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
					return nullptr;
				}
				ArgsV.push_back(argVal);
				argList.push_back(argType(getStringTypeFromLLVMType(argVal->getType()), getASTNodeTypeFromString(getStringTypeFromLLVMType(argVal->getType())), 0));
				if (!ArgsV.back())
					return nullptr;
			}

			// Look up the id in the struct function.
			functionID* CalleeFID = getFunctionFromID(structDefinition->memberFunctions, memberName, argList, token, true, true);
			if (!CalleeFID) {
				if (!wasError)
					printTokenError(childNodes[1]->token, "Struct definition does not contain member function");
				wasError = true;
				return nullptr;
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
				}
				// If variable arguments, make sure the amount in call are <= the required amount
				else if (CalleeF->arg_size() > argList.size()) {
					printTokenError(token, "Incorrect number of arguments passed to function", __LINE__);
					wasError = true;
					return nullptr;
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
				if (CalleeFID->userArguments[i].isReference) {
					if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
						printTokenError(token, "Cannot pass value as reference");
						wasError = true;
						return nullptr;
					}
					args[i]->childNodes[0]->isRef = true;
				}
				Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
				if (wasError) {
					return nullptr;
				}
				ArgsV.push_back(argVal);
				if (!ArgsV.back())
					return nullptr;
			}
			bool wasDefined = true;
			lastRetrievedElementType.push(new ASAType(getLLVMTypeFromString(CalleeFID->returnType, 0, token, wasDefined, pass)));

			isCallMemberFunction = false;
			asaType = lastRetrievedElementType.top();

			Value* callResult = nullptr;
			Type* retType = CalleeF->getReturnType();

			// Create the call
			if (retType->isVoidTy())
				Builder->CreateCall(CalleeF, ArgsV);
			else
				callResult = Builder->CreateCall(CalleeF, ArgsV, "calltmp");
			return callResult;

			//return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
		}
	}
	// If left is not pointer, assume another member access or index operator
	else {
		structType* structDefinition = getStructTypeFromLLVMType(lastRetrievedElementType.top()->baseLLVMType);
		lastRetrievedElementType.pop();

		if (structDefinition == nullptr) {
			wasError = true;
			return nullptr;
		}

		// Get member name and index
		std::string memberName = childNodes[1]->token->first;
		// Handle if member variable access/set
		if (childNodes[1]->nodeType == Identifier_Node) {
			if (structDefinition->memberNameIndexes.find(memberName) == structDefinition->memberNameIndexes.end()) {
				printTokenError(childNodes[1]->token, "Struct definition does not contain member");
				console::indentation++;
				console::WriteLine("It does have:");
				console::indentation++;
				for (const auto& name : structDefinition->memberNameIndexes)
					console::WriteLine(name.first, console::cyanFGColor);
				console::indentation -= 2;
				wasError = true;
				return nullptr;
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
			}

			//// Create GEP to compute the address
			//Value* gep = Builder->CreateGEP(elementType, basePtr, index, "arrayidx");

			auto gep = Builder->CreateStructGEP(structDefinition->structVal, basePtr, memberIndex, "struct_member");
			asaType = new ASAType(elementType);

			lastRetrievedElementType.push(asaType);

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
					return nullptr;
				}
				ArgsV.push_back(argVal);
				argList.push_back(argType(getStringTypeFromLLVMType(argVal->getType()), getASTNodeTypeFromString(getStringTypeFromLLVMType(argVal->getType())), 0));
				if (!ArgsV.back())
					return nullptr;
			}

			// Look up the id in the struct function.
			functionID* CalleeFID = getFunctionFromID(structDefinition->memberFunctions, memberName, argList, token, true, true);
			if (!CalleeFID) {
				if (!wasError)
					printTokenError(childNodes[1]->token, "Struct definition does not contain member function");
				wasError = true;
				return nullptr;
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
				}
				// If variable arguments, make sure the amount in call are <= the required amount
				else if (CalleeF->arg_size() > argList.size()) {
					printTokenError(token, "Incorrect number of arguments passed to function", __LINE__);
					wasError = true;
					return nullptr;
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
					if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
						printTokenError(token, "Cannot pass value as reference");
						wasError = true;
						return nullptr;
					}
					args[i]->childNodes[0]->isRef = true;
				}
				Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
				if (wasError) {
					return nullptr;
				}
				ArgsV.push_back(argVal);
				if (!ArgsV.back())
					return nullptr;
			}
			bool wasDefined = true;
			lastRetrievedElementType.push(new ASAType(getLLVMTypeFromString(CalleeFID->returnType, 0, token, wasDefined, pass)));

			isCallMemberFunction = false;
			asaType = lastRetrievedElementType.top();

			Value* callResult = nullptr;
			Type* retType = CalleeF->getReturnType();

			// Create the call
			if (retType->isVoidTy())
				Builder->CreateCall(CalleeF, ArgsV);
			else
				callResult = Builder->CreateCall(CalleeF, ArgsV, "calltmp");
			return callResult;

			//return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
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
				return nullptr;
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
	if (childNodes.size() < 2 ||
		childNodes[1]->nodeType != Scope_Body ||
		childNodes[1]->childNodes.size() == 0 ||
		childNodes[1]->childNodes[0]->nodeType != Colon_Separator_Node ||
		childNodes[1]->childNodes[0]->childNodes.size() == 0) {

		printTokenError(token, "Cast expression expected name followed by new type like: #cast x : float;");
		printAST(this);
		wasError = true;
		return nullptr;
	}
	ASTNode* colonNode = childNodes[1]->childNodes[0];
	std::string varName = colonNode->childNodes[0]->token->first;
	valueType* val = findNamedValue(parentNode, this, varName, token);
	if (!val && !wasError) {
		printTokenError(colonNode->childNodes[0]->token, "Unknown variable name used");
		printAST(this);
		wasError = true;
		return nullptr;
	}
	Value* var = val->val;

	Value* value = Builder->CreateLoad(getValueStoredType(var), var, varName + "_load");

	std::string tyVal = colonNode->childNodes[1]->token->first;

	bool wasDefined = true;
	Type* toType = getLLVMTypeFromString(tyVal, 0, colonNode->childNodes[1]->token, wasDefined, pass);
	//if (wasDefined == false)
	//	return nullptr;

	Value* casted = castValue(value, toType, true, typeSigns[tyVal], token);

	if (wasError)
		return nullptr;

	return casted;

	//return Builder->CreateStore(castedValue, var);
}

// Value*
void* ASTNode::generateTypeInstance(int pass)
{
	if (childNodes.size() < 2 || childNodes[1]->childNodes.size() == 0) {
		printTokenError(token, "New expression requires type name, like #new ty;");
		wasError = true;
		return nullptr;
	}

	std::string typeName = childNodes[1]->childNodes[0]->token->first;
	if (structDefinitions.find(typeName) == structDefinitions.end()) {
		printTokenError(childNodes[1]->childNodes[0]->token, "Unknown type name used");
		wasError = true;
		return nullptr;
	}

	structType* typeVal = structDefinitions[typeName];

	// If the struct body hasn't been generated yet, generate it
	if (typeVal->structVal == nullptr)
		Value* argVal = (Value*)(typeVal->sourceNode->*(typeVal->sourceNode->codegen))(pass);
	if (wasError) {
		return nullptr;
	}

	AllocaInst* var = Builder->CreateAlloca(typeVal->structVal, nullptr, "struct_alloc");

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

	// Apply non-zero default values for members that declare them
	for (auto& [memberName, defaultNode] : typeVal->memberDefaultNodes) {
		auto idxIt = typeVal->memberNameIndexes.find(memberName);
		if (idxIt == typeVal->memberNameIndexes.end())
			continue;
		uint16_t idx = idxIt->second;
		Value* memberPtr = Builder->CreateStructGEP(typeVal->structVal, var, idx, memberName + "_init");
		Value* defaultVal = (Value*)(defaultNode->*(defaultNode->codegen))(pass);
		if (wasError)
			return nullptr;
		if (!defaultVal)
			continue;
		Type* memberType = typeVal->structVal->getElementType(idx);
		bool isSigned = typeSigns.count(typeVal->members[idx].typeString) ? typeSigns[typeVal->members[idx].typeString] : false;
		defaultVal = castValue(defaultVal, memberType, true, isSigned, token);
		if (wasError)
			return nullptr;
		Builder->CreateStore(defaultVal, memberPtr);
	}

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
			return nullptr;
		}
		ArgsV.push_back(argVal);

		std::string typeStr;
		ASTNode* identifierNode = args[i];
		if (args[i]->childNodes.size() == 1)
			identifierNode = args[i]->childNodes[0];

		// TODO: Make this better for non-high-level expressions
		// Try to get the type string from the expression
		if (identifierNode->nodeType == Identifier_Node) {
			valueType* val = findNamedValue(parentNode, this, identifierNode->token->first, token);
			if (val) {
				typeStr = val->type;
			}
			else if (!wasError) {
				typeStr = getStringTypeFromLLVMType(argVal->getType());
			}
		}
		else if (identifierNode->nodeType == Member_Access) {
			// Use the new helper function to resolve member access types
			typeStr = getMemberAccessTypeString(identifierNode, parentNode, token);
			if (typeStr.empty() && !wasError) {
				typeStr = getStringTypeFromLLVMType(argVal->getType());
			}
		}
		else if (identifierNode->nodeType == String_Constant_Node) {
			// String literals produce a string struct unless inside the string module
			if (compilerDefines["IN_STRING_MODULE"] != "true" &&
				structDefinitions.count("string") && structDefinitions["string"]->structVal)
				typeStr = "string";
			else
				typeStr = "*char";
		}
		else {
			typeStr = getStringTypeFromLLVMType(argVal->getType());
		}

		// Extract pointer level from typeStr
		uint8_t pointerLevel = 0;
		std::string baseTypeStr = typeStr;
		while (baseTypeStr.length() > 0 && baseTypeStr[0] == '*') {
			pointerLevel++;
			baseTypeStr = baseTypeStr.substr(1);
		}

		argList.push_back(argType(baseTypeStr, getASTNodeTypeFromString(baseTypeStr), pointerLevel));

		if (!ArgsV.back())
			return nullptr;
	}

	// Look up the function ID using the caller's argList (without sret)
	if (verbosity >= 6) {
		console::WriteLine("\nLooking for function: " + token->first);
		console::WriteLine("Arguments passed:");
		for (size_t i = 0; i < argList.size(); i++) {
			console::WriteLine("  [" + std::to_string(i) + "] type: " + argList[i].typeString +
							   ", pointerLevel: " + std::to_string(argList[i].pointerLevel));
		}
	}
	functionID* CalleeFID = getFunctionFromID(functionIDs, token->first, argList, token, true, shouldBeMemberFunction);
	if (!CalleeFID) {
		if (shouldBeMemberFunction)
			printf("Should be member function\n");
		if (!wasError)
			printTokenError(token, "Undefined function");
		wasError = true;
		return nullptr;
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
		}
	}
	else if (CalleeF->arg_size() > ArgsV.size()) {
		printTokenError(token, "Incorrect number of arguments passed to function", __LINE__);
		CalleeFID->print();
		wasError = true;
		return nullptr;
	}

	ArgsV.clear();
	for (int i = 0; i < args.size(); i++) {									   // Start from caller's args (sret is already handled)
		if (CalleeFID->arguments[i + (isStructReturn ? 1 : 0)].isReference) {  // Offset by 1 if sret
			if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
				printTokenError(token, "Cannot pass value as reference");
				wasError = true;
				return nullptr;
			}
			args[i]->childNodes[0]->isRef = true;
		}
		Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
		if (wasError) {
			return nullptr;
		}
		// Implicit string ↔ *char conversions at call sites
		int formalIdx = i + (isStructReturn ? 1 : 0);
		const argType& formal = CalleeFID->arguments[formalIdx];
		if (argVal && argVal->getType()->isStructTy() &&
			formal.pointerLevel == 1 &&
			(formal.typeString == "char" || formal.typeString == "int8")) {
			// string → *char: extract .address (element 0)
			argVal = Builder->CreateExtractValue(argVal, {0}, "str_addr");
		}
		else if (argVal && argVal->getType()->isPointerTy() &&
				 formal.pointerLevel == 0 && formal.typeString == "string" &&
				 structDefinitions.count("string") && structDefinitions["string"]->structVal) {
			// *char → string: build string struct with strlen
			StructType* strTy = cast<StructType>((Type*)structDefinitions["string"]->structVal);
			FunctionCallee strlenFn = TheModule->getOrInsertFunction("strlen",
				FunctionType::get(Type::getInt64Ty(*TheContext), {PointerType::getUnqual(*TheContext)}, false));
			Value* lenVal = Builder->CreateCall(strlenFn, {argVal}, "strlen");
			Value* lenTrunc = Builder->CreateTrunc(lenVal, Type::getInt32Ty(*TheContext), "len");
			Value* strStruct = UndefValue::get(strTy);
			strStruct = Builder->CreateInsertValue(strStruct, argVal, {0});
			strStruct = Builder->CreateInsertValue(strStruct, lenTrunc, {1});
			argVal = strStruct;
		}
		ArgsV.push_back(argVal);
		if (!ArgsV.back())
			return nullptr;
	}

	// If struct return, re-insert sret as first arg (after rebuilding)
	if (isStructReturn) {
		ArgsV.insert(ArgsV.begin(), sretAlloc);
	}

	Value* callResult = nullptr;
	Type* retType = CalleeF->getReturnType();

	// Create the call
	if (retType->isVoidTy())
		Builder->CreateCall(CalleeF, ArgsV);
	else
		callResult = Builder->CreateCall(CalleeF, ArgsV, "calltmp");

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
	}
	condExpr = condExpr->childNodes[0];

	if (condExpr->codegen == nullptr) {
		printTokenError(condExpr->token, "Node `" + ASTNodeTypeAsString(condExpr->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}

	Value* CondV = (Value*)(condExpr->*(condExpr->codegen))(pass);
	if (wasError) {
		return nullptr;
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
		return nullptr;
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
		return nullptr;
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
		}
	}

	currentStructName.push(structName);
	uint8_t generatingType = 0;	 // Generate all member variables first (0), then functions (1)
	argumentList members = argumentList();
	std::vector<Type*> fieldTypes = std::vector<Type*>();
	std::vector<std::string> fieldNames = std::vector<std::string>();
	std::vector<functionID*> memberFunctions = std::vector<functionID*>();
	std::unordered_map<std::string, uint16_t> memberNameIndexes = std::unordered_map<std::string, uint16_t>();
	std::unordered_map<std::string, ASTNode*> memberDefaultNodes = std::unordered_map<std::string, ASTNode*>();
	uint16_t i = 0;
	for (; generatingType < 2; generatingType++)
		for (auto fieldNode : childNodes[0]->childNodes) {

			// Unwrap member declaration with default value: Expression_Statement(Colon(name, type), defaultVal)
			ASTNode* defaultValNode = nullptr;
			if (fieldNode->nodeType == Expression_Statement &&
				fieldNode->childNodes.size() >= 2 &&
				fieldNode->childNodes[0]->nodeType == Colon_Separator_Node) {
				defaultValNode = fieldNode->childNodes[1];
				fieldNode = fieldNode->childNodes[0];
			}

			// If it is a member variable declaration
			if ((fieldNode->nodeType == Identifier_Node || fieldNode->nodeType == Colon_Separator_Node) && generatingType == 0 && pass > 0) {
				if (fieldNode->childNodes.size() == 0) {
					printTokenError(fieldNode->token, "Member declaration must have type");
					printAST(fieldNode);
					wasError = true;
					currentStructName.pop();
					return nullptr;
				}

				ASTNode* typeNode = fieldNode->childNodes[1];
				std::string memberType = typeNode->token->first;

				fieldNode = fieldNode->childNodes[0];

				std::string memberName = fieldNode->token->first;
				int pointerLevel = 0;

			recurseAddMemberPointer:

				if (typeNode->token->first == "*") {
					pointerLevel++;
					typeNode = typeNode->childNodes[0];
					goto recurseAddMemberPointer;
				}

				memberType = typeNode->token->first;

				bool wasDefined = true;
				Type* fieldType = getLLVMTypeFromString(memberType, 0, typeNode->token,
					wasDefined, pass);
				for (int i = 0; i < pointerLevel; i++)
					fieldType = fieldType->getPointerTo();
				fieldTypes.push_back(fieldType);
				fieldNames.push_back(memberName);
				memberNameIndexes[memberName] = i;

				if (defaultValNode != nullptr)
					memberDefaultNodes[memberName] = defaultValNode;

				members.push_back(argType(memberType, getASTNodeTypeFromString(memberType), pointerLevel));
				i++;
			}
			// Else it is a member function definition
			else if (fieldNode->nodeType == Compiler_Define_Function &&
					 generatingType == 1 && pass > 1) {
				// Generate function
				Function* memberFunction = (Function*)(fieldNode->*(fieldNode->codegen))(pass);
				if (wasError) {
					currentStructName.pop();
					return nullptr;
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
		structDefinitions[structName]->memberDefaultNodes = memberDefaultNodes;
		structDefinitions[structName]->structVal->setBody(fieldTypes, false);
		return nullptr;
	}

	// Pass 1: If struct already exists (created in pass 0 as empty), update its body
	// in-place so all existing references (global vars, etc.) see the correct fields.
	if (pass == 1 && structDefinitions.count(structName)) {
		StructType* existingTy = (StructType*)structDefinitions[structName]->structVal;
		existingTy->setBody(fieldTypes, false);
		currentStructName.pop();
		structDefinitions[structName]->members = members;
		structDefinitions[structName]->memberNameIndexes = memberNameIndexes;
		structDefinitions[structName]->memberDefaultNodes = memberDefaultNodes;
		return existingTy;
	}

	StructType* structTy = StructType::create(*TheContext, fieldTypes, "struct." + structName);

	currentStructName.pop();
	auto newStructDef = new structType(structName, token, structTy, members, memberFunctions, memberNameIndexes);
	newStructDef->memberDefaultNodes = memberDefaultNodes;
	structDefinitions[structName] = newStructDef;

	return structTy;
}

// Value*
void* ASTNode::generateBreak(int pass)
{
	// Check if we have a label
	std::string targetLabel = "";
	if (childNodes.size() > 0 && childNodes[0]->nodeType == Identifier_Node) {
		targetLabel = childNodes[0]->token->first;
	}

	// Make sure we're inside a loop
	if (loopContextStack.empty()) {
		printTokenError(token, "Break statement must be inside a loop");
		wasError = true;
		return nullptr;
	}

	// If no label, break from the innermost loop
	if (targetLabel.empty()) {
		BasicBlock* breakBB = loopContextStack.top().breakBB;
		Builder->CreateBr(breakBB);

		// Create a new unreachable block for any code after the break
		Function* TheFunction = Builder->GetInsertBlock()->getParent();
		BasicBlock* afterBreak = BasicBlock::Create(*TheContext, "after_break", TheFunction);
		Builder->SetInsertPoint(afterBreak);

		return nullptr;
	}

	// Labeled break - search through the stack for the matching label
	std::stack<LoopContext> tempStack = loopContextStack;
	bool found = false;
	BasicBlock* targetBreakBB = nullptr;

	while (!tempStack.empty()) {
		LoopContext ctx = tempStack.top();
		if (ctx.label == targetLabel) {
			targetBreakBB = ctx.breakBB;
			found = true;
			break;
		}
		tempStack.pop();
	}

	if (!found) {
		printTokenError(token, "Break label \"" + targetLabel + "\" not found in enclosing loops");
		wasError = true;
		return nullptr;
	}

	Builder->CreateBr(targetBreakBB);

	// Create a new unreachable block for any code after the break
	Function* TheFunction = Builder->GetInsertBlock()->getParent();
	BasicBlock* afterBreak = BasicBlock::Create(*TheContext, "after_break", TheFunction);
	Builder->SetInsertPoint(afterBreak);

	return nullptr;
}

// Value*
void* ASTNode::generateContinue(int pass)
{
	// Check if we have a label
	std::string targetLabel = "";
	if (childNodes.size() > 0 && childNodes[0]->nodeType == Identifier_Node) {
		targetLabel = childNodes[0]->token->first;
	}

	// Make sure we're inside a loop
	if (loopContextStack.empty()) {
		printTokenError(token, "Continue statement must be inside a loop");
		wasError = true;
		return nullptr;
	}

	// If no label, continue to the innermost loop
	if (targetLabel.empty()) {
		BasicBlock* continueBB = loopContextStack.top().continueBB;
		Builder->CreateBr(continueBB);

		// Create a new unreachable block for any code after the continue
		Function* TheFunction = Builder->GetInsertBlock()->getParent();
		BasicBlock* afterContinue = BasicBlock::Create(*TheContext, "after_continue", TheFunction);
		Builder->SetInsertPoint(afterContinue);

		return nullptr;
	}

	// Labeled continue - search through the stack for the matching label
	std::stack<LoopContext> tempStack = loopContextStack;
	bool found = false;
	BasicBlock* targetContinueBB = nullptr;

	while (!tempStack.empty()) {
		LoopContext ctx = tempStack.top();
		if (ctx.label == targetLabel) {
			targetContinueBB = ctx.continueBB;
			found = true;
			break;
		}
		tempStack.pop();
	}

	if (!found) {
		printTokenError(token, "Continue label \"" + targetLabel + "\" not found in enclosing loops");
		wasError = true;
		return nullptr;
	}

	Builder->CreateBr(targetContinueBB);

	// Create a new unreachable block for any code after the continue
	Function* TheFunction = Builder->GetInsertBlock()->getParent();
	BasicBlock* afterContinue = BasicBlock::Create(*TheContext, "after_continue", TheFunction);
	Builder->SetInsertPoint(afterContinue);

	return nullptr;
}

// Add a generator for Labeled_Loop:
// Value*
void* ASTNode::generateLabeledLoop(int pass)
{
	std::string label = token->first;

	// Find the loop
	if (childNodes.size() == 0) {
		printTokenError(token, "Labeled loop is empty");
		wasError = true;
		return nullptr;
	}

	ASTNode* loopNode = childNodes[0];

	// Verify it's actually a loop
	if (loopNode->nodeType != For_Statement_Node && loopNode->nodeType != While_Statement_Node) {
		printTokenError(token, "Label can only be applied to for or while loops");
		wasError = true;
		return nullptr;
	}

	// Store the label in the loop node and generate it
	loopNode->label = label;
	return (loopNode->*(loopNode->codegen))(pass);
}

// Now update the generateFor function to use the label:
// Value*
void* ASTNode::generateFor(int pass)
{
	std::string varName = "_iterator";
	std::string label = "";	 // Optional label for the loop

	// Check if this loop has a label (set by generateLabeledLoop)
	if (!this->label.empty()) {
		label = this->label;
	}

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
		return nullptr;
	}
	if (!StartVal)
		return nullptr;

	// Make the new basic block for the loop header, inserting after current block.
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
		return nullptr;
	}

	Value* CurVar = Builder->CreateLoad(Alloca->getAllocatedType(), Alloca, varName.c_str());

	// Compare: exclusive (i < N)
	Value* Cond = Builder->CreateICmpSLT(CurVar, EndVal, "loopcond");

	// Conditional branch
	Builder->CreateCondBr(Cond, LoopBB, AfterBB);

	// Start insertion in LoopBB.
	Builder->SetInsertPoint(LoopBB);

	// Push loop context for break/continue support
	LoopContext ctx;
	ctx.continueBB = LoopCondBB;  // Continue goes back to condition check
	ctx.breakBB = AfterBB;		  // Break goes to after the loop
	ctx.label = label;			  // Empty for unlabeled loops
	loopContextStack.push(ctx);

	namedValues[varName] = new valueType(varName, "int32", Alloca);

	// Emit the body of the loop
	ASTNode* scopeBody = childNodes[2];

	if (scopeBody->codegen == nullptr) {
		printTokenError(scopeBody->token, "Node `" + ASTNodeTypeAsString(scopeBody->nodeType) + "` does not have a code generator");
		wasError = true;
		loopContextStack.pop();	 // Clean up context
		return nullptr;
	}
	(scopeBody->*(scopeBody->codegen))(pass);
	if (wasError) {
		loopContextStack.pop();	 // Clean up context
		return nullptr;
	}

	// Pop loop context
	loopContextStack.pop();

	// Emit the step value.
	Value* StepVal = ConstantInt::get(*TheContext, APInt(32, 1));

	Value* NextVar = Builder->CreateAdd(CurVar, StepVal, "nextvar");
	Builder->CreateStore(NextVar, Alloca);

	// Jump back to condition
	Builder->CreateBr(LoopCondBB);

	// After loop
	Builder->SetInsertPoint(AfterBB);

	return nullptr;
}

// Similarly, if you have a while loop generator, update it too:
// Value*
void* ASTNode::generateWhile(int pass)
{
	std::string label = "";	 // Optional label

	// Check if this loop has a label (set by generateCompilerDefine)
	if (!this->label.empty()) {
		label = this->label;
	}

	Function* TheFunction = Builder->GetInsertBlock()->getParent();

	BasicBlock* LoopCondBB = BasicBlock::Create(*TheContext, "whilecond", TheFunction);
	BasicBlock* LoopBB = BasicBlock::Create(*TheContext, "whileloop", TheFunction);
	BasicBlock* AfterBB = BasicBlock::Create(*TheContext, "afterwhile", TheFunction);

	// Branch to condition check
	Builder->CreateBr(LoopCondBB);
	Builder->SetInsertPoint(LoopCondBB);

	// Evaluate condition
	ASTNode* condExpr = childNodes[0];
	if (condExpr->childNodes.size() == 0) {
		printTokenError(condExpr->token, "Expected condition expression");
		wasError = true;
		return nullptr;
	}
	condExpr = condExpr->childNodes[0];

	if (condExpr->codegen == nullptr) {
		printTokenError(condExpr->token, "Node `" + ASTNodeTypeAsString(condExpr->nodeType) + "` does not have a code generator");
		wasError = true;
		return nullptr;
	}

	Value* CondV = (Value*)(condExpr->*(condExpr->codegen))(pass);
	if (wasError) {
		return nullptr;
	}
	if (!CondV)
		return nullptr;

	// Convert condition to bool
	CondV = Builder->CreateICmpNE(CondV, ConstantInt::get(*TheContext, APInt(1, 0)), "whilecond");

	// Conditional branch
	Builder->CreateCondBr(CondV, LoopBB, AfterBB);

	// Loop body
	Builder->SetInsertPoint(LoopBB);

	// Push loop context
	LoopContext ctx;
	ctx.continueBB = LoopCondBB;
	ctx.breakBB = AfterBB;
	ctx.label = label;
	loopContextStack.push(ctx);

	// Generate loop body
	ASTNode* scopeBody = childNodes[1];
	if (scopeBody->codegen == nullptr) {
		printTokenError(scopeBody->token, "Node `" + ASTNodeTypeAsString(scopeBody->nodeType) + "` does not have a code generator");
		wasError = true;
		loopContextStack.pop();
		return nullptr;
	}

	(scopeBody->*(scopeBody->codegen))(pass);
	if (wasError) {
		loopContextStack.pop();
		return nullptr;
	}

	// Pop loop context
	loopContextStack.pop();

	// Jump back to condition
	Builder->CreateBr(LoopCondBB);

	// After loop
	Builder->SetInsertPoint(AfterBB);

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

	// Like C, main always returns i32 even when declared without a return type.
	if (fnName == "main" && retType->isVoidTy())
		retType = Type::getInt32Ty(*TheContext);

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
		int pointerLevel = 0;  // References don't count as pointer level

		Type* aType = nullptr;
		argList.push_back(argType(typeStr, Struct_Type, pointerLevel, isReference, false, false));

		try {
			bool wasDefined = true;
			aType = getLLVMTypeFromString(typeStr, 0, token, wasDefined, pass);
			//if (wasDefined == false)
			//	return nullptr;
			for (int i = 0; i < pointerLevel; i++) {
				aType = aType->getPointerTo();
			}
			// Add pointer level for reference (LLVM representation)
			if (isReference) {
				aType = aType->getPointerTo();
			}
		}
		catch (...) {
			printTokenError(token, "Invalid argument type given");
			wasError = true;
			return nullptr;
		}

		// Unknown type name
		// TODO: Add handling for custom structs as well

		//else if (typeName == "string")
		//Type::getStringTy(*TheContext);
		argTypes.push_back(aType);
		argNames.push_back("this");
	}
	bool variableNumArguments = false;
	for (auto& a : argsNode->childNodes) {	// `a` is the expression term containing the entire argument as an expression
		if (a->childNodes.size() > 0) {
			if (variableNumArguments)  // If there is a named argument after ... then it is invalid
				goto invalidArgument;
			if (a->childNodes[0]->nodeType == Colon_Separator_Node) {
				ASTNode* nameNode = a->childNodes[0]->childNodes[0];
				ASTNode* typeNode = a->childNodes[0]->childNodes[1];
				std::string typeStr = "";
				bool isReference = false;
				bool mustBeExactType = false;
				bool isConstant = false;
				int pointerLevel = 0;

			gatherTypeModifiers:
				if (typeNode->token->first == "ref") {
					isReference = true;
					mangledName += ".ref";
					// Don't modify typeStr or pointerLevel - references are tracked separately
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
				if (typeNode->token->first == "const") {
					isConstant = true;
					typeNode = typeNode->childNodes[0];
					goto gatherTypeModifiers;
				}
				if (typeNode->token->first == "*") {
					pointerLevel++;
					mangledName += ".ptr";
					// pointerLevel tracks pointer depth; typeStr holds only the base type name
					typeNode = typeNode->childNodes[0];
					goto gatherTypeModifiers;
				}

				//// Function arguments should only be constant if they are non-local, so a reference
				//if (isConstant && !isReference)
				//	isConstant = false;

				mangledName += "." + typeNode->token->first;
				typeStr += typeNode->token->first;


				Type* aType = nullptr;
				argType arg = argType(typeStr, getASTNodeTypeFromString(typeNode->token->first), pointerLevel, isReference, mustBeExactType, isConstant);
				argList.push_back(arg);
				userArgList.push_back(arg);

				try {
					bool wasDefined = true;
					aType = getLLVMTypeFromString(typeNode->token->first, 0, typeNode->token, wasDefined, pass);

					for (int i = 0; i < pointerLevel; i++) {
						aType = aType->getPointerTo();
					}

					// If this is a reference, add an extra pointer level for LLVM representation
					// (references are implemented as pointers in LLVM)
					if (isReference) {
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
				argNames.push_back(nameNode->token->first);
				//std::cout << "Arg added: '" << a->childNodes[0]->token->first << "' of type: '" << typeName << "'\n";
			}
			// Handle ellipses ...
			else if (a->childNodes[0]->nodeType == Argument_List) {
				//std::string typeName = a->childNodes[0]->childNodes[0]->token->first;
				variableNumArguments = true;
			}
			else
				goto invalidArgument;
			continue;
		invalidArgument:
			printTokenError(a->token, "Invalid argument type given");
			wasError = true;
			return nullptr;
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
			//theFunctionID->print();
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

		if (argList[Idx].isConstant) {
			Type* argType = arg.getType();

			// readonly can only be applied to pointer types
			if (argType->isPointerTy()) {
				arg.addAttr(llvm::Attribute::ReadOnly);
				// Optionally also add NoCapture to indicate the pointer isn't stored
				// arg.addAttr(llvm::Attribute::NoCapture);
			}
		}
	}

	functionIDs.push_back(new functionID(fnName, token, mangledName, rTypeString, argList, userArgList, fn, variableNumArguments, isStruct, isStructReturn));
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
		printTokenError(token, "There was a failure to create a function");
		wasError = true;
		return nullptr;
	}

	if (!theFunction->empty() && replaceableDefinition == false) {
		printTokenError(token, "Function cannot be redefined, requires unique identity");
		theFunctionID = getFunctionIDFromFunctionPointer(functionIDs, theFunction);
		console::indentation++;
		printTokenMarked(theFunctionID->token, "Previously defined here:");
		console::indentation--;
		wasError = true;
		return nullptr;
	}

	if (pass <= 1)
		return theFunction;

	theFunctionID = getFunctionIDFromFunctionPointer(functionIDs, theFunction);
	if (!theFunctionID) {
		printTokenError(token, "There was a failure to create a function");
		wasError = true;
		return nullptr;
	}

	// Create debug info for the function
	unsigned LineNo = token->lineNumber;  // Assuming token has line info
	unsigned ScopeLine = LineNo;

	// Create subroutine type
	SmallVector<Metadata*, 8> EltTys;
	DIType* DblTy = DBuilder->createBasicType("double", 64, dwarf::DW_ATE_float);
	EltTys.push_back(DblTy);  // Add return type and parameters as needed

	DISubroutineType* SubroutineType = DBuilder->createSubroutineType(
		DBuilder->getOrCreateTypeArray(EltTys));

	// Create function debug info
	DISubprogram* SP = DBuilder->createFunction(
		TheFile,						// Scope
		functionName,					// Name
		StringRef(),					// Linkage name
		TheFile,						// File
		LineNo,							// Line number
		SubroutineType,					// Type
		ScopeLine,						// Scope line
		DINode::FlagPrototyped,			// Flags
		DISubprogram::SPFlagDefinition	// SP Flags
	);

	theFunction->setSubprogram(SP);
	LexicalBlocks.push_back(SP);

	// Create entry block and emit debug location
	BasicBlock* fnBlock = BasicBlock::Create(*TheContext, "entry", theFunction);
	// Clear any stale debug location from a previously generated function before
	// setting the insert point, so no instructions inherit the wrong subprogram.
	Builder->SetCurrentDebugLocation(DebugLoc());
	Builder->SetInsertPoint(fnBlock);

	// Call the global variable init function at the start of main
	if (functionName == "main" && globalInitFn)
		Builder->CreateCall(globalInitFn, {});

	// Set debug location for entry
	Builder->SetCurrentDebugLocation(
		DILocation::get(SP->getContext(), LineNo, 0, SP));


	//// Create a new basic block to start insertion into.
	//BasicBlock* fnBlock = BasicBlock::Create(*TheContext, "entry", theFunction);
	//Builder->SetInsertPoint(fnBlock);

	// Record the function arguments in the NamedValues map.
	// If it is a struct, first add a "this" argument like: (this : ref structName, ...)
	int i = 0;
	//if (isStruct) {
	//	if (i >= theFunctionID->arguments.size()) {
	//		printTokenError(token, "Mismatch in number of arguments, expected " + std::to_string(theFunctionID->arguments.size()), __LINE__);
	//	return nullptr;
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
			printTokenError(token, "Mismatch in number of arguments vs function signature: " + std::to_string(theFunctionID->arguments.size()), __LINE__);
			wasError = true;
			return nullptr;
		}

		// Always create an alloca for the incoming argument so we'll have addressable storage
		AllocaInst* Alloca = CreateEntryBlockAlloca(theFunction, arg.getType(), arg.getName());
		// Store the incoming argument value into the alloca
		Builder->CreateStore(&arg, Alloca);

		// Build the "actual type" string the rest of your compiler expects (pointer stars + type name)
		std::string baseType = "";
		for (int p = 0; p < theFunctionID->arguments[i].pointerLevel; p++)
			baseType += "*";

		valueType* vt = new valueType(std::string(arg.getName()), baseType + theFunctionID->arguments[i].typeString, Alloca, true, theFunctionID->arguments[i].isReference);

		vt->isConstant = theFunctionID->arguments[i].isConstant;

		namedValues[std::string(arg.getName())] = vt;

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
		return nullptr;
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

	// Pop this function's scope now that its body is fully generated.
	LexicalBlocks.pop_back();

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

// #define NAME VALUE;
// Sets a compiler-time flag in compilerDefines. Processed on every pass so
// that flags are correctly scoped during each codegen phase.
void* ASTNode::generateCompilerDefine(int pass)
{
	if (childNodes.size() < 2)
		return nullptr;

	// childNodes[0] = "define" keyword node
	// childNodes[1] = scope body containing NAME and VALUE tokens
	// Walk the body's tree collecting leaf tokens in order
	std::vector<std::string> tokens;
	std::function<void(ASTNode*)> collectTokens = [&](ASTNode* n) {
		if (n->token && !n->token->first.empty() && n->childNodes.empty())
			tokens.push_back(n->token->first);
		for (auto& c : n->childNodes)
			collectTokens(c);
	};
	collectTokens(childNodes[1]);

	if (tokens.size() >= 2)
		compilerDefines[tokens[0]] = tokens[1];
	return nullptr;
}

// Returns a string value (struct or *char fallback) for a compile-time string constant,
// using the same global cache and struct-building logic as String_Constant_Node.
static Value* makeStringConstant(const std::string& str)
{
	GlobalVariable* globalStr = nullptr;
	if (globalStringLiteralConstants.count(str)) {
		globalStr = globalStringLiteralConstants[str];
	}
	else {
		Constant* strConst = ConstantDataArray::getString(*TheContext, str, true);
		globalStr = new GlobalVariable(
			*TheModule,
			strConst->getType(),
			true,
			GlobalValue::PrivateLinkage,
			strConst,
			"str");
		globalStr->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
		globalStr->setAlignment(Align(1));
		globalStringLiteralConstants[str] = globalStr;
	}

	Constant* zero = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
	std::vector<Constant*> indices = {zero, zero};
	Constant* strPtr = ConstantExpr::getGetElementPtr(globalStr->getValueType(), globalStr, indices);

	// Return a string struct when the string type is defined (matches String_Constant_Node behavior)
	if (compilerDefines["IN_STRING_MODULE"] != "true" &&
		structDefinitions.count("string") && structDefinitions["string"]->structVal) {
		StructType* strTy = cast<StructType>((Type*)structDefinitions["string"]->structVal);
		Constant* lenConst = ConstantInt::get(Type::getInt32Ty(*TheContext), (uint32_t)str.size());
		return ConstantStruct::get(strTy, {strPtr, lenConst});
	}
	return strPtr;
}

// #typeof(expr) — returns the ASA type name of expr as a string.
// For identifiers, looks up namedValues to avoid emitting any IR.
// For other expressions, generates the expression and reads the LLVM type.
void* ASTNode::generateTypeofDirective(int pass)
{
	if (pass == 0)
		return nullptr;
	if (childNodes.size() < 2 || childNodes[1]->childNodes.empty()) {
		printTokenError(token, "#typeof requires an expression argument");
		wasError = true;
		return nullptr;
	}

	ASTNode* argExpr = childNodes[1]->childNodes[0];
	std::string typeStr;

	// Fast path: identifier — look up in namedValues, no IR emitted
	if (argExpr->nodeType == Identifier_Node) {
		valueType* val = findNamedValue(parentNode, this, argExpr->token->first, token);
		if (val)
			typeStr = val->type;
	}

	// Fallback: generate the expression and read the LLVM type
	if (typeStr.empty()) {
		Value* val = (Value*)(argExpr->*(argExpr->codegen))(pass);
		if (!val || wasError)
			return nullptr;
		if (argExpr->asaType && argExpr->asaType->baseLLVMType)
			typeStr = getStringTypeFromLLVMType(argExpr->asaType->baseLLVMType);
		else
			typeStr = getStringTypeFromLLVMType(val->getType());
	}

	Value* result = makeStringConstant(typeStr);
	if (!asaType)
		asaType = new ASAType(result->getType());
	else
		asaType->baseLLVMType = result->getType();
	return result;
}

// #sizeof(T) — returns the alloc size in bytes of T as an int64 constant.
// T can be a type name (e.g. int, string, MyStruct) or a variable name.
void* ASTNode::generateSizeofDirective(int pass)
{
	if (pass == 0)
		return nullptr;
	if (childNodes.size() < 2 || childNodes[1]->childNodes.empty()) {
		printTokenError(token, "#sizeof requires a type or variable argument");
		wasError = true;
		return nullptr;
	}

	ASTNode* argNode = childNodes[1]->childNodes[0];
	Type* llvmType = nullptr;

	if (argNode->nodeType == Identifier_Node) {
		// Try as a direct type name first
		bool wasDefined = true;
		llvmType = getLLVMTypeFromString(argNode->token->first, 0, token, wasDefined, pass);
		if (!wasDefined || !llvmType) {
			// Try as a variable — use its stored type string
			valueType* val = findNamedValue(parentNode, this, argNode->token->first, token);
			if (val) {
				wasDefined = true;
				llvmType = getLLVMTypeFromString(val->type, 0, token, wasDefined, pass);
				if (!wasDefined)
					llvmType = nullptr;
			}
		}
	}

	// Fallback: generate the expression and read the LLVM type
	if (!llvmType) {
		Value* val = (Value*)(argNode->*(argNode->codegen))(pass);
		if (val)
			llvmType = val->getType();
	}

	if (!llvmType) {
		printTokenError(token, "#sizeof: cannot determine type of argument");
		wasError = true;
		return nullptr;
	}

	const DataLayout& DL = TheModule->getDataLayout();
	uint64_t size = DL.getTypeAllocSize(llvmType);
	Value* sizeVal = ConstantInt::get(Type::getInt64Ty(*TheContext), size);
	if (!asaType)
		asaType = new ASAType(sizeVal->getType());
	else
		asaType->baseLLVMType = sizeVal->getType();
	return sizeVal;
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

int generateExecutable(const std::string& irFilePath, const std::string& exeFilePath, const std::string& clangOptions)
{
	// llc to convert <name>.ll to assembly
	std::string commandLLC = "llc -relocation-model=pic " + irFilePath + " -o " + irFilePath + ".s";
	int result = std::system(commandLLC.c_str());
	if (result != 0)
		exit(1);
	// clang as the linker
	std::string commandClang = "clang -fPIE -o " + clangOptions + " " + exeFilePath + " " + irFilePath + ".s -g";
	result = std::system(commandClang.c_str());
	if (result != 0)
		exit(1);
	return result;
}

void removeUnusedPrototypes()
{
	for (auto& fn : functionIDs)
		if (fn->name != "main")
			if (fn->uses == 0)
				fn->fnValue->eraseFromParent();
}
