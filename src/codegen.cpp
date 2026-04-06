#include "codegen.h"


std::unique_ptr<LLVMContext> TheContext;
std::unique_ptr<Module> TheModule;
std::unique_ptr<DIBuilder> DBuilder;
static DICompileUnit* TheCU;
static DIFile* TheFile;
static std::map<std::string, DIFile*> DIFileCache;
static std::vector<DIScope*> LexicalBlocks;
std::unique_ptr<IRBuilder<>> Builder;

std::unordered_map<std::string, llvm::GlobalVariable*> globalStringLiteralConstants;
bool isCallMemberFunction = false;
bool wasError = false;
uint8_t errorDepth = 0;
//bool suppressCodegenErrors = false;
std::map<std::string, std::string> compilerDefines;
std::vector<std::string> linkedLibraries;
std::vector<std::string> linkedStaticLibraries;

// Check @deprecated / @removed attributes on a symbol's declaration.
// Emits a warning for @deprecated and sets wasError+returns false for @removed.
// Returns false if the symbol is @removed (caller should bail out).
static bool checkUsageAttrs(tokenPair* usageTok, const std::vector<ASTNode*>& attrs, const std::string& symName)
{
	for (auto* attr : attrs) {
		if (!attr->token)
			continue;
		const std::string& attrName = attr->token->first;
		std::string msg;
		// Extract optional string argument from Scope_Body child
		auto getMsg = [&]() -> std::string {
			for (auto* ac : attr->childNodes)
				if (ac->nodeType == Scope_Body && !ac->childNodes.empty() && ac->childNodes[0]->token) {
					const std::string& raw = ac->childNodes[0]->token->first;
					return raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : raw;
				}
			return "";
		};
		if (attrName == "deprecated") {
			msg = "'" + symName + "' is deprecated";
			std::string detail = getMsg();
			if (!detail.empty())
				msg += ": " + detail;
			printTokenWarning(tokenRange {usageTok, usageTok}, msg);
		}
		else if (attrName == "removed") {
			msg = "'" + symName + "' has been removed";
			std::string detail = getMsg();
			if (!detail.empty())
				msg += ": " + detail;
			console::indentation = errorDepth;
			if (errorDepth < maxErrorTraceDepth)
				printTokenError(tokenRange {usageTok, usageTok}, msg);
			wasError = true;
			errorDepth++;
			return false;
		}
	}
	return true;
}

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

// Stack to track value-block contexts (for result)
struct ResultContext {
	AllocaInst* resultSlot;	 // Stack slot that holds the block's result value (set by generateResult)
	BasicBlock* mergeBB;	 // Block to branch to after a result statement
};
static std::stack<ResultContext> resultContextStack;

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
	bool hasDefault = false;
	ASTNode* defaultNode = nullptr;			   // Expression_Term node (already resolved at definition site)
	std::vector<tokenPair*> defaultRawTokens;  // raw tokens for re-parsing at call site
	// Extern ABI coercion: how to split this struct arg for the x86-64 SysV ABI
	int8_t externCoercionCount = 0;		 // 0=none, N=split into N primitives (doubles or i64s)
	bool externCoercionIsFloat = false;	 // true=doubles (SSE/XMM), false=i64s (INTEGER)
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
	ASTNode* declNode = nullptr;
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

		if (isMemberFunction) {
			console::Write(" (");
			console::Write("is a member function", console::yellowFGColor);
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
		if (a.size() > userArguments.size() && !variableNumArguments)
			return 1000 - 1;
		if (a.size() < userArguments.size()) {
			// Allow if all extra params have defaults
			for (size_t i = a.size(); i < userArguments.size(); i++)
				if (!userArguments[i].hasDefault)
					return 1000 - 1;
		}
		if (verbosity >= 6)
			console::WriteLine("Comparing: " + name, console::blueFGColor);
		console::indentation++;
		int compareCount = variableNumArguments ? (int)userArguments.size() : (int)a.size();
		for (int i = 0; i < compareCount; i++) {
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

			// If pointer levels differ, these are fundamentally different types -
			// with two implicit conversion exceptions involving string:
			//   string -> *char  (extracts .address at call site)
			//   *char  -> string (wraps in struct at call site)
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
				// For struct/unknown types both sides resolve to Identifier_Node;
				// require the type strings to also match, otherwise Vector2 and Vector3
				// would be treated as equivalent.
				if (t1 == Identifier_Node && userArguments[i].typeString != a[i].typeString) {
					differences = 800;
					goto returnDifferences;
				}
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
				else if (t2 >= Double_Type && t2 <= Half_Type)	// float -> int implicit
					differences += 50;
				else {
					differences = 600;
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
				else if (t2 >= Integer_Node && t2 <= Boolean_Node)	// int -> float implicit
					differences += 50;
				else {
					differences = 600;
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
		if (a.size() > userArguments.size())
			return 1000 - 1;
		if (a.size() < userArguments.size()) {
			for (size_t i = a.size(); i < userArguments.size(); i++)
				if (!userArguments[i].hasDefault)
					return 1000 - 1;
		}
		for (int i = 0; i < (int)a.size(); i++) {
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
	std::unordered_map<std::string, ASTNode*> memberDefaultNodes;  // member name -> default value AST node
	uint32_t uses = 0;
	std::vector<functionID*> memberFunctions;
	StructType* structVal = nullptr;
	tokenPair* token = nullptr;
	ASTNode* sourceNode = nullptr;
	ASAType* asaType = nullptr;
	bool isPacked = false;	   // @packed: force integer packing in extern calls
	bool isNotPacked = false;  // @notpacked: force auto-detect ABI even if @packed is set
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

static bool hasCastBetween(const std::string& fromType, uint8_t fromPtrLevel, const std::string& toType, uint8_t toPtrLevel)
{
	// Only consider non-pointer casts
	if (fromPtrLevel != 0 || toPtrLevel != 0)
		return false;
	// Both numeric: #cast always works between any two numeric types
	if (typeSigns.count(fromType) && typeSigns.count(toType))
		return true;
	// User-defined: look for a cast function with matching signature
	for (auto* fid : functionIDs) {
		if (fid->name != "cast" || fid->returnType != toType)
			continue;
		if (fid->userArguments.size() == 1 &&
			fid->userArguments[0].typeString == fromType &&
			fid->userArguments[0].pointerLevel == 0)
			return true;
	}
	return false;
}

std::string formatCallSignature(const std::string& name, const argumentList& args)
{
	std::string s = name + "(";
	for (size_t i = 0; i < args.size(); i++) {
		s += "<" + std::string(args[i].pointerLevel, '*') + args[i].typeString + ">";
		if (i + 1 < args.size())
			s += ", ";
	}
	s += ")";
	return s;
}

void printFunctionDifferences(argumentList& arguments, functionID*& other)
{
	console::Write(other->name + " :: (");
	for (int i = 0; i < arguments.size(); i++) {
		argType a = arguments[i];
		argType b = other->userArguments[i];
		std::string aStr = std::string(a.pointerLevel, '*') + a.typeString;
		std::string bStr = std::string(b.pointerLevel, '*') + b.typeString;
		if ((a.typeString == b.typeString || other->areTypesEquivalent(a.typeString, b.typeString)) && a.pointerLevel == b.pointerLevel)
			console::Write(aStr, console::greenFGColor);
		else if (hasCastBetween(a.typeString, a.pointerLevel, b.typeString, b.pointerLevel))
			console::Write(aStr + " ~= " + bStr, console::yellowFGColor);
		else
			console::Write(aStr + " != " + bStr, console::redFGColor);
		if (i < arguments.size() - 1)
			console::Write(", ");
	}
	console::Write(")\n");
}

void printUndefinedFunctionError(tokenPair*& token, std::string& name, argumentList& args, std::vector<functionID*>& fnIDs)
{
	printTokenError(tokenRange {token, token}, "Undefined function: " + formatCallSignature(name, args));
	// Show any candidates with the same name
	bool anyFound = false;
	for (auto& f : fnIDs) {
		if (f->name != name)
			continue;
		if (!anyFound) {
			console::indentation++;
			console::WriteLine("Candidates:", console::yellowFGColor);
			console::indentation++;
			anyFound = true;
		}
		console::printIndent(console::indentation);
		//f->print();
		printFunctionDifferences(args, f);
	}
	if (anyFound) {
		console::indentation--;
		console::indentation--;
	}
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
		console::indentation = errorDepth;
		if (errorDepth < maxErrorTraceDepth)
			printTokenError(tokenRange {t, t}, "Function match not found, closest prototype requires exact types. Did you try casting?");
		if (errorDepth < maxErrorTraceDepth)
			printFunctionDifferences(arguments, best);
		wasError = true;
		errorDepth++;
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
	if (bestScore > 500)
		goto exactFnNotFound;
	// If the best function match requires exact typing (and different types are passed) throw error
	if (requiresExact) {
		goto exactFnNotFound;
		//printTokenError(tokenRange{t, t}, "Function match not found, closest prototype requires exact types.\nDid you try casting?");
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
		console::indentation = errorDepth;
		if (errorDepth < maxErrorTraceDepth)
			printTokenError(tokenRange {t, t}, "Function match not found, closest prototype requires exact types. Did you try casting?");
		//printFunctionDifferences(arguments, best);
		wasError = true;
		errorDepth++;
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
		console::indentation = errorDepth;
		if (errorDepth < maxErrorTraceDepth)
			printTokenError(tokenRange {t, t}, "Function match not found, closest prototype requires exact types. Did you try casting?");
		//printFunctionDifferences(arguments, best);
		wasError = true;
		errorDepth++;
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
	console::indentation = errorDepth;
	if (errorDepth < maxErrorTraceDepth)
		console::PrintError("Function could not be resolved from Function*");
	wasError = true;
	errorDepth++;
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
				console::indentation = errorDepth;
				if (errorDepth < maxErrorTraceDepth)
					printTokenError(tokenRange {token, token}, "Cannot nest struct in self");
				wasError = true;
				errorDepth++;
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
		//printTokenError(tokenRange{token, token}, "Unknown type \"" + typeName + "\"", __LINE__);
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
		baseTypeName = "char";	// char/uint8 are unsigned in ASA; use unsigned default for i8
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
	bool lIsFloat = false;
	bool rIsFloat = false;
	bool lIsInt = false;
	bool rIsInt = false;
	// If signed, L more accurate
	if (LType < RType && RType < Begin_Unsigned_Integers) {
		R = castValue(R, L->getType(), typeSigns[rTyStr], typeSigns[lTyStr], token);
		if (R == nullptr)
			goto wasCastError;
		return;
	}
	// If signed, R more accurate
	else if (RType < LType && LType < Begin_Unsigned_Integers) {
		L = castValue(L, R->getType(), typeSigns[lTyStr], typeSigns[rTyStr], token);
		if (L == nullptr)
			goto wasCastError;
		return;
	}
	// If unsigned, L more accurate
	else if (LType < RType && RType < Double_Type) {
		R = castValue(R, L->getType(), typeSigns[rTyStr], typeSigns[lTyStr], token);
		if (R == nullptr)
			goto wasCastError;
		return;
	}
	// If unsigned, R more accurate
	else if (RType < LType && LType < Double_Type) {
		L = castValue(L, R->getType(), typeSigns[lTyStr], typeSigns[rTyStr], token);
		if (L == nullptr)
			goto wasCastError;
		return;
	}
	// Mixed float/int: promote the integer to the float type
	lIsFloat = (LType >= Double_Type && LType <= Half_Type);
	rIsFloat = (RType >= Double_Type && RType <= Half_Type);
	lIsInt = (LType >= Integer_Node && LType < Double_Type);
	rIsInt = (RType >= Integer_Node && RType < Double_Type);
	if (lIsFloat && rIsInt) {
		R = castValue(R, L->getType(), typeSigns[rTyStr], typeSigns[lTyStr], token);
		if (R == nullptr)
			goto wasCastError;
		return;
	}
	if (rIsFloat && lIsInt) {
		L = castValue(L, R->getType(), typeSigns[lTyStr], typeSigns[rTyStr], token);
		if (L == nullptr)
			goto wasCastError;
		return;
	}
	// If floats, L more accurate
	else if (LType < RType && RType < Half_Type) {
		R = castValue(R, L->getType(), typeSigns[rTyStr], typeSigns[lTyStr], token);
		if (R == nullptr)
			goto wasCastError;
		return;
	}
	// If floats, R more accurate
	else if (RType < LType && LType < Half_Type) {
		L = castValue(L, R->getType(), typeSigns[lTyStr], typeSigns[rTyStr], token);
		if (L == nullptr)
			goto wasCastError;
		return;
	}

wasCastError:
	console::indentation = errorDepth;
	if (errorDepth < maxErrorTraceDepth)
		printTokenError(tokenRange {token, token}, "Unsupported cast: cannot implicitly convert '" + lTyStr + "' to '" + rTyStr + "' (or vice versa)");
	wasError = true;
	errorDepth++;
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
}

static DIFile* getDIFile(const std::string& fullPath)
{
	auto it = DIFileCache.find(fullPath);
	if (it != DIFileCache.end())
		return it->second;
	std::string dir = std::filesystem::path(fullPath).parent_path().string();
	std::string name = std::filesystem::path(fullPath).filename().string();
	DIFile* f = DBuilder->createFile(name, dir);
	DIFileCache[fullPath] = f;
	return f;
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
				console::indentation = errorDepth;
				if (errorDepth < maxErrorTraceDepth)
					printTokenError(tokenRange {token, token}, "Variable '" + identifier + "' not found. Did you mean 'this." + identifier + "'?");
				if (errorDepth < maxErrorTraceDepth)
					console::Write("Tip: ", console::yellowFGColor);
				if (errorDepth < maxErrorTraceDepth)
					console::WriteLine("Member functions must use 'this' to access member variables.\n");
				wasError = true;
				errorDepth++;
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
			console::indentation = errorDepth;
			if (errorDepth < maxErrorTraceDepth)
				printTokenError(tokenRange {token, token}, "Unknown variable name: " + node->token->first);
			wasError = true;
			errorDepth++;
			return "";
		}
		return val ? val->type : "";
	}

	// Handle array indexing: arr[i], element type is array type with one pointer stripped
	if (node->nodeType == Access_Operation) {
		if (node->childNodes.size() < 1)
			return "";
		std::string arrayType = getMemberAccessTypeString(node->childNodes[0], parentNode, token);
		if (arrayType.empty() || wasError)
			return "";
		if (!arrayType.empty() && arrayType[0] == '*')
			return arrayType.substr(1);
		return "";
	}

	// Handle member access: left.right
	if (node->nodeType == Member_Access) {
		if (node->childNodes.size() < 2) {
			console::indentation = errorDepth;
			if (errorDepth < maxErrorTraceDepth)
				printTokenError(tokenRange {token, token}, "Invalid member access expression");
			wasError = true;
			errorDepth++;
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
			console::indentation = errorDepth;
			if (errorDepth < maxErrorTraceDepth)
				printTokenError(tokenRange {token, token}, "Module has no member '" + rightNode->token->first + "'");
			wasError = true;
			errorDepth++;
			return "";
		}

		// Remove pointer markers to get the struct name
		std::string structName = leftType;
		while (structName[0] == '*') {
			structName = structName.substr(1);
		}

		// Look up the struct definition
		if (structDefinitions.find(structName) == structDefinitions.end()) {
			console::indentation = errorDepth;
			if (errorDepth < maxErrorTraceDepth)
				printTokenError(tokenRange {token, token}, "Type \"" + structName + "\" is not a defined struct");
			wasError = true;
			errorDepth++;
			return "";
		}

		structType* structDef = structDefinitions[structName];

		// If the struct body hasn't been generated yet, we might not have member info
		if (structDef->members.empty()) {
			console::indentation = errorDepth;
			if (errorDepth < maxErrorTraceDepth)
				printTokenError(tokenRange {token, token}, "Struct \"" + structName + "\" has no defined members");
			wasError = true;
			errorDepth++;
			return "";
		}

		// Get the member name
		std::string memberName = rightNode->token->first;

		// If right side is a function call, look up the return type from memberFunctions.
		// The token may already be mutated to "StructName.fnName" by a prior codegen pass.
		if (rightNode->nodeType == Function_Call) {
			std::string fnName = memberName;
			// Strip any struct-name prefix added by a prior pass
			std::string prefix = structName + ".";
			if (fnName.size() > prefix.size() && fnName.substr(0, prefix.size()) == prefix)
				fnName = fnName.substr(prefix.size());
			std::string fullName = structName + "." + fnName;
			for (auto& f : structDef->memberFunctions) {
				if (f->name == fullName)
					return f->returnType;
			}
			console::indentation = errorDepth;
			if (errorDepth < maxErrorTraceDepth)
				printTokenError(getASTTokenRange(rightNode), "Struct \"" + structName + "\" has no member function named \"" + fnName + "\"");
			wasError = true;
			errorDepth++;
			return "";
		}

		// Look up the member in the struct
		if (structDef->memberNameIndexes.find(memberName) == structDef->memberNameIndexes.end()) {
			console::indentation = errorDepth;
			if (errorDepth < maxErrorTraceDepth)
				printTokenError(getASTTokenRange(rightNode), "Struct \"" + structName + "\" has no member named \"" + memberName + "\"");
			wasError = true;
			errorDepth++;
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

	console::indentation = errorDepth;
	if (errorDepth < maxErrorTraceDepth)
		printTokenError(tokenRange {token, token}, "Cannot determine type of expression");
	wasError = true;
	errorDepth++;
	return "";
}


// Declare one Expression_Statement as an LLVM global.
// ownerNode: the node in whose namedValues the valueType is stored.
//   Root-level vars: ownerNode == exprStmtNode (so findNamedValue can find it).
//   Module vars: ownerNode == the Compiler_Define module node.
void declareModuleScopeVariable(ASTNode* exprStmtNode, ASTNode* ownerNode, bool isModuleVar)
{
	if (exprStmtNode->childNodes.empty())
		return;
	ASTNode* leftNode = exprStmtNode->childNodes[0];

	// A typed declaration with no initializer has only
	// one child (the Colon_Separator_Node).  Allow that through; untyped (inferred)
	// declarations still require a right-hand-side child.
	bool hasRHS = exprStmtNode->childNodes.size() >= 2;
	if (!hasRHS && leftNode->nodeType != Colon_Separator_Node)
		return;

	std::string varName;
	std::string typeName;
	int pointerLevel = 0;
	bool isConst = false;
	Type* llvmType = nullptr;

	if (leftNode->nodeType == Colon_Separator_Node) {
		// Typed declaration: x : int = 5  OR  x : int  (no initializer)
		if (leftNode->childNodes.size() < 2)
			return;
		ASTNode* nameNode = leftNode->childNodes[0];
		ASTNode* typeNode = leftNode->childNodes[1];
		varName = nameNode->token->first;

	getNextPointerLevel:
		if (typeNode->token->first == "const") {
			isConst = true;
			typeNode = typeNode->childNodes[0];
			goto getNextPointerLevel;
		}
		if (typeNode->token->first == "ref" || typeNode->token->first == "exact") {
			typeNode = typeNode->childNodes[0];
			goto getNextPointerLevel;
		}
		if (typeNode->token->first == "*") {
			pointerLevel++;
			typeNode = typeNode->childNodes[0];
			goto getNextPointerLevel;
		}
		typeName = typeNode->token->first;

		bool wasDefined = true;
		int pass = 1;
		llvmType = getLLVMTypeFromString(typeName, 0, typeNode->token, wasDefined, pass);
		if (!llvmType || !wasDefined)
			return;
		for (int i = 0; i < pointerLevel; i++)
			llvmType = llvmType->getPointerTo();
	}
	else if (leftNode->nodeType == Identifier_Node) {
		// Untyped declaration: infer type by speculatively evaluating rhs
		if (!hasRHS)
			return;
		varName = leftNode->token->first;
		ASTNode* exprNode = exprStmtNode->childNodes[1];
		if (!exprNode || !exprNode->codegen)
			return;

		// Save builder state
		BasicBlock* savedBB = Builder->GetInsertBlock();
		BasicBlock::iterator savedPt = savedBB ? Builder->GetInsertPoint() : BasicBlock::iterator();
		bool savedError = wasError;

		// Create a temporary function+block to probe the expression's type
		FunctionType* ft = FunctionType::get(Type::getVoidTy(*TheContext), false);
		Function* probeF = Function::Create(ft, Function::PrivateLinkage, "__type_probe__", TheModule.get());
		BasicBlock* probeBB = BasicBlock::Create(*TheContext, "probe", probeF);
		Builder->SetInsertPoint(probeBB);

		wasError = false;
		messageSystem::suppressErrors = true;
		Value* probeVal = (Value*)(exprNode->*(exprNode->codegen))(1);
		messageSystem::suppressErrors = false;

		if (!wasError && probeVal)
			llvmType = probeVal->getType();
		wasError = savedError;

		probeF->eraseFromParent();

		if (savedBB)
			Builder->SetInsertPoint(savedBB, savedPt);

		if (!llvmType)
			return;	 // Could not infer type; skip (will be caught as undefined if used)

		typeName = getStringTypeFromLLVMType(llvmType);
	}
	else {
		return;
	}

	std::string globalName = varName;
	GlobalVariable* gv = new GlobalVariable(
		*TheModule, llvmType, isConst,
		GlobalValue::InternalLinkage,
		Constant::getNullValue(llvmType),
		globalName);

	std::string actualType = std::string(pointerLevel, '*') + typeName;
	valueType* vt = new valueType(varName, actualType, gv);
	vt->isConstant = isConst;
	vt->declNode = exprStmtNode;  // the expression statement node that owns the attributes

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

// Handle a bare Colon_Separator_Node (typed declaration with no initializer).
// E.g. `g_intbuf : *int;` parses as Colon_Separator_Node{name, type} without an
// enclosing Expression_Statement because there is no `=` assignment.
void declareModuleScopeVariableFromColon(ASTNode* colonNode, ASTNode* ownerNode)
{
	if (colonNode->childNodes.size() < 2)
		return;
	ASTNode* nameNode = colonNode->childNodes[0];
	ASTNode* typeNode = colonNode->childNodes[1];
	if (!nameNode || !typeNode)
		return;

	std::string varName = nameNode->token->first;
	int pointerLevel = 0;
	bool isConst = false;
	while (!typeNode->childNodes.empty() &&
		   (typeNode->token->first == "*" || typeNode->token->first == "const" ||
			   typeNode->token->first == "ref" || typeNode->token->first == "exact")) {
		if (typeNode->token->first == "*")
			pointerLevel++;
		if (typeNode->token->first == "const")
			isConst = true;
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

	std::string actualType = std::string(pointerLevel, '*') + typeName;
	GlobalVariable* gv = new GlobalVariable(
		*TheModule, llvmType, isConst,
		GlobalValue::InternalLinkage,
		Constant::getNullValue(llvmType),
		varName);

	valueType* vt = new valueType(varName, actualType, gv);
	vt->isConstant = isConst;
	vt->declNode = colonNode;
	ownerNode->namedValues[varName] = vt;
	// No entry in globalInitList: null initializer is already set above, nothing to run.
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

	// Helper to process one child node of the module's inner scope.
	// Declared as a std::function so it can recurse into Scope_Body wrappers.
	std::function<void(ASTNode*)> processChild = [&](ASTNode* child) {
		if (child->nodeType == Expression_Statement)
			declareModuleScopeVariable(child, moduleCompilerDefineNode, true);
		// Typed declaration without initializer: `g_intbuf : *int;` parses as a bare
		// Colon_Separator_Node (no Expression_Statement wrapper because there's no `=`).
		else if (child->nodeType == Colon_Separator_Node)
			declareModuleScopeVariableFromColon(child, moduleCompilerDefineNode);
		// Recurse into nested sub-modules, registering with compound dot-separated names.
		else if (child->nodeType == Compiler_Define)
			processModuleForDeclarations(child, fullName);
		// Scope_Body used as an attribute group (e.g. @public: { KEY_A : ...; })
		// Flatten its children into the enclosing module scope.
		else if (child->nodeType == Scope_Body) {
			for (auto& scopeChild : child->childNodes)
				processChild(scopeChild);
		}
	};

	for (auto& child : innerScope->childNodes)
		processChild(child);
}

// Fill in __asa_global_init's body and finalize it. returns false on error/failure
bool finalizeGlobalInit()
{
	if (!globalInitFn || globalInitList.empty())
		return true;


	BasicBlock* BB = BasicBlock::Create(*TheContext, "entry", globalInitFn);
	// Clear any stale debug location so __asa_global_init instructions don't
	// inherit a scope from the last user-function that was generated.
	Builder->SetCurrentDebugLocation(DebugLoc());
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
		if (wasError || !initVal) {
			return false;
		}

		messageSystem::startBlock(node, "Initializing global variable", __func__, __LINE__, __FILE__);

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
					messageSystem::endBlock();
					return false;
				}
			}
		}
		if (gv->isConstant()) {
			if (Constant* c = dyn_cast<Constant>(initVal))
				gv->setInitializer(c);
			else {
				messageSystem::error("Const variable must have a compile-time constant initializer");
				messageSystem::endBlock();
				return false;
			}
		}
		else {
			Builder->CreateStore(initVal, gv);
		}

		messageSystem::endBlock();
	}

	Builder->CreateRetVoid();
	Builder->ClearInsertionPoint();

	return true;
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

	std::string lTyStr = getStringTypeFromLLVMType(srcType);
	std::string rTyStr = getStringTypeFromLLVMType(destType);

	console::indentation = errorDepth;
	if (errorDepth < maxErrorTraceDepth)
		printTokenError(tokenRange {token, token}, "Unsupported cast: cannot implicitly convert '" + lTyStr + "' to '" + rTyStr + "' (or vice versa)");
	wasError = true;
	errorDepth++;
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
				console::indentation = errorDepth;
				if (errorDepth < maxErrorTraceDepth)
					printTokenError(tokenRange {token, token}, "Incomplete escape sequence at end of string");
				wasError = true;
				errorDepth++;
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
	messageSystem::startBlock(this, "Generating constant", __func__, __LINE__, __FILE__);

	if (nodeType == Integer_Node) {
		try {
			int intVal = stoi(token->first);
			messageSystem::endBlock();
			return ConstantInt::get(*TheContext, APInt(32, intVal, true));
		}
		catch (...) {
			long intVal = stol(token->first);
			messageSystem::endBlock();
			return ConstantInt::get(*TheContext, APInt(64, intVal, true));
		}
	}
	else if (nodeType == Boolean_Node) {
		messageSystem::endBlock();
		return ConstantInt::get(*TheContext, APInt(1, token->first == "true" ? 1 : 0, false));
	}
	else if (nodeType == Float_Node) {
		messageSystem::endBlock();
		return ConstantFP::get(*TheContext, APFloat(stod(token->first)));
	}
	else if (nodeType == Void_Node) {
		messageSystem::endBlock();
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
			messageSystem::endBlock();
			return ConstantStruct::get(strTy, {strPtr, lenConst});
		}

		messageSystem::endBlock();
		return strPtr;	// Fallback: returns i8* pointing to the string
	}
	else if (nodeType == Character_Constant_Node) {
		std::string strValue = unescapeString(token->first.substr(1, token->first.size() - 2), token);	// remove quotes from token

		if (strValue.size() > 1) {
			return messageSystem::error("Character constant may contain only a single character");
		}

		messageSystem::endBlock();
		return ConstantInt::get(*TheContext, APInt(8, strValue[0], false));
	}

	return messageSystem::error("Value could not be parsed as constant");
	return nullptr;
}

// Value*
void* ASTNode::generateVariableExpression(int pass)
{
	messageSystem::startBlock(this, "Generating variable expression", __func__, __LINE__, __FILE__);

	// Look this variable up in the function.
	valueType* val = findNamedValue(parentNode, this, token->first, token);
	if (wasError) {
		messageSystem::endBlock();
		return nullptr;
	}
	if (!val) {
		Value* exprVal = ConstantInt::get(Type::getInt32Ty(*TheContext), 0);
		Type* llvmType = nullptr;
		Function* theFunction = Builder->GetInsertBlock()->getParent();
		uint16_t pointerLevel = 0;
		std::string typeName = "";

		// If variable does not have type, it is a used undefined variable
		if (childNodes.size() == 0) {
			return messageSystem::error("Use of undefined variable");
		}
		// If variable does have type, it is a declaration
		else {
			ASTNode* typeNode = childNodes[0];
			typeName = typeNode->token->first;

			messageSystem::startBlock(typeNode, "Generating type", __func__, __LINE__, __FILE__);

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
				return messageSystem::error("Unknown type");
			}

			if (wasError || exprVal == nullptr) {
				return messageSystem::error("Unable to automatically cast");
			}

			messageSystem::endBlock();
		}
		if (!asaType)
			asaType = new ASAType(llvmType);
		else
			asaType->baseLLVMType = llvmType;
		AllocaInst* targetPtr = CreateEntryBlockAlloca(theFunction, llvmType, token->first);
		std::string actualType = (pointerLevel > 0 ? std::string(pointerLevel, '*') : "") + typeName;
		namedValues[token->first] = new valueType(token->first, actualType, targetPtr);
		namedValues[token->first]->declNode = this;

		Builder->CreateStore(exprVal, targetPtr);

		messageSystem::endBlock();

		if (isRef || lvalue)
			return targetPtr;
		else
			return Builder->CreateLoad(targetPtr->getAllocatedType(), targetPtr, token->first + "_load");
	}
	// Check @deprecated / @removed on the variable declaration
	if (val->declNode)
		if (!checkUsageAttrs(token, val->declNode->attributes, token->first)) {
			messageSystem::endBlock();
			return nullptr;
		}

	Value* A = val->val;
	Type* valType = getValueStoredType(A);
	if (!asaType)
		asaType = new ASAType(valType);
	else
		asaType->baseLLVMType = valType;
	// Store the type string so pointer element types can be resolved later for array subscripts
	asaType->strVal = val->type;
	asaType->isRef = val->isReference;

	// Handle references: need to dereference when used as rvalue
	if (val->isReference && !isRef && !lvalue) {
		messageSystem::startBlock(this, "Generating reference usage", __func__, __LINE__, __FILE__);

		// valType is the pointer type; load the pointer, then deref through it using the base type
		Value* ptr = Builder->CreateLoad(valType, A, token->first + "_ref_ptr");
		bool wasDefined = true;
		Type* baseType = getLLVMTypeFromString(val->type, 0, token, wasDefined, pass);
		if (!baseType || !wasDefined) {
			return messageSystem::error("Cannot resolve ref base type for dereference");
		}
		if (!asaType)
			asaType = new ASAType(baseType);
		else
			asaType->baseLLVMType = baseType;
		messageSystem::endBlock();

		messageSystem::endBlock();
		return Builder->CreateLoad(baseType, ptr, token->first + "_ref_deref");
	}

	messageSystem::endBlock();
	if (isRef || lvalue)
		return A;
	else
		return Builder->CreateLoad(valType, A, token->first + "_load");
}

void* ASTNode::generateThrow(int pass)
{
	messageSystem::startBlock(this, "Generating throw statement", __func__, __LINE__, __FILE__);

	// If it has a child node, we will output it's value as a string
	ASTNode* exprNode = nullptr;
	Value* outVal = nullptr;
	if (childNodes.size() > 0) {
		exprNode = childNodes[0]->childNodes[0];
		if (exprNode->codegen == nullptr) {
			return messageSystem::error("Node `" + ASTNodeTypeAsString(exprNode->nodeType) + "` does not have a code generator");
		}
		outVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	}
	if (wasError) {
		messageSystem::endBlock();
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

	messageSystem::endBlock();

	return nullptr;
}

void* ASTNode::generateReturn(int pass)
{
	messageSystem::startBlock(this, "Generating return", __func__, __LINE__, __FILE__);

	if (!LexicalBlocks.empty() && token && token->filePath)
		Builder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));
	ASTNode* exprNode = childNodes[0];

	messageSystem::startBlock(exprNode, "Generating return expression value", __func__, __LINE__, __FILE__);

	Value* RetVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	if (wasError) {
		return messageSystem::error("Failed to generate return expression value");
	}
	Function* currentFunc = Builder->GetInsertBlock()->getParent();
	functionID* fnID = getFunctionIDFromFunctionPointer(functionIDs, currentFunc);
	// Check if we're returning a struct
	Type* returnType = Builder->GetInsertBlock()->getParent()->getReturnType();
	if (fnID->isStructReturn) {
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
					printTokenWarning(getASTTokenRange(this), "returning void in function that expects a return value");
				Builder->CreateRet(Constant::getNullValue(retType));
			}
		}
		else {
			if (retType->isVoidTy()) {
				return messageSystem::error("Returning a value is a function with no return type.");
			}
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

	messageSystem::endBlock();

	messageSystem::endBlock();
	return nullptr;
}

// Value*
void* ASTNode::generateResult(int pass)
{
	messageSystem::startBlock(this, "Generating result statement", __func__, __LINE__, __FILE__);

	if (resultContextStack.empty()) {
		return messageSystem::error("'result' used outside a value block");
	}

	ASTNode* exprNode = childNodes[0];
	Value* val = (Value*)(exprNode->*(exprNode->codegen))(pass);
	if (wasError || !val) {
		messageSystem::endBlock();
		return nullptr;
	}

	ResultContext& ctx = resultContextStack.top();

	// Allocate the result slot in the function entry block the first time we see a result.
	if (!ctx.resultSlot) {
		Function* theFunction = Builder->GetInsertBlock()->getParent();
		IRBuilder<> entryBuilder(&theFunction->getEntryBlock(),
			theFunction->getEntryBlock().begin());
		ctx.resultSlot = entryBuilder.CreateAlloca(val->getType(), nullptr, "result_slot");
	}

	Builder->CreateStore(val, ctx.resultSlot);
	Builder->CreateBr(ctx.mergeBB);

	// Any code after 'result' is unreachable; give LLVM a valid insertion point.
	Function* theFunction = Builder->GetInsertBlock()->getParent();
	BasicBlock* afterResult = BasicBlock::Create(*TheContext, "after_result", theFunction);
	Builder->SetInsertPoint(afterResult);

	messageSystem::endBlock();
	return nullptr;
}

// Value*
void* ASTNode::generateExpression(int pass)
{
	messageSystem::startBlock(this, "Generating expression", __func__, __LINE__, __FILE__);

	if (!LexicalBlocks.empty() && token && token->filePath) {
		Builder->SetCurrentDebugLocation(
			DILocation::get(LexicalBlocks.back()->getContext(),
				token->lineNumber + 1,	// Line (DWARF is 1-indexed)
				token->indexInLine,		// Column
				LexicalBlocks.back()));
	}
	if (childNodes.size() == 0)
		return nullptr;
	ASTNode* exprNode = childNodes[0];

	if (lvalue)
		exprNode->lvalue = true;
	Value* exprVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	if (exprVal == nullptr) {
		return messageSystem::error("Unable to compile");
	}
	// Propagate asaType from child so access operations (c[i]) can resolve element type
	if (!asaType && exprNode->asaType)
		asaType = exprNode->asaType;

	messageSystem::endBlock();
	return exprVal;
}

// Value*
void* ASTNode::generateIncDecrement(int pass)
{
	messageSystem::startBlock(this, "Generating unary increment/decrement operation", __func__, __LINE__, __FILE__);

	ASTNode* operand = childNodes[0];
	Value* targetPtr = nullptr;
	Type* targetType = nullptr;

	messageSystem::startBlock(operand, "Generating variable", __func__, __LINE__, __FILE__);

	if (operand->nodeType == Identifier_Node) {
		valueType* val = findNamedValue(parentNode, this, operand->token->first, token);
		if (!val) {
			return messageSystem::error("Unknown variable '" + operand->token->first + "'");
		}
		if (val->isConstant) {
			return messageSystem::error("Cannot modify const variable '" + operand->token->first + "'");
		}
		targetPtr = val->val;
		targetType = getValueStoredType(targetPtr);
	}
	else {
		operand->lvalue = true;
		targetPtr = (Value*)(operand->*(operand->codegen))(pass);
		if (wasError)
			return nullptr;
		if (!targetPtr || !targetPtr->getType()->isPointerTy()) {
			return messageSystem::error("Operand of ++/-- must be an lvalue");
		}
		targetType = getValueStoredType(targetPtr);
	}

	messageSystem::endBlock();


	Value* current = Builder->CreateLoad(targetType, targetPtr, "incdec_load");
	Value* updated = nullptr;
	bool isFloat = targetType->isFloatingPointTy();
	if (token->second == Plus_Plus)
		updated = isFloat ? Builder->CreateFAdd(current, ConstantFP::get(targetType, 1.0), "incr")
						  : Builder->CreateAdd(current, ConstantInt::get(targetType, 1), "incr");
	else if (token->second == Minus_Minus)
		updated = isFloat ? Builder->CreateFSub(current, ConstantFP::get(targetType, 1.0), "decr")
						  : Builder->CreateSub(current, ConstantInt::get(targetType, 1), "decr");
	else
		return messageSystem::error("Unknown operator");
	Builder->CreateStore(updated, targetPtr);


	messageSystem::endBlock();
	return isPostfix ? current : updated;
}

// Value*
void* ASTNode::generateExpressionStatement(int pass)
{
	messageSystem::startBlock(this, "Generating runtime statement expression", __func__, __LINE__, __FILE__);

	ASTNode* leftNode = childNodes[0];
	ASTNode* exprNode = childNodes[1];
	Function* theFunction = Builder->GetInsertBlock()->getParent();

	messageSystem::startBlock(exprNode, "Generating expression right side", __func__, __LINE__, __FILE__);

	// Set debug location if available
	if (!LexicalBlocks.empty() && token && token->filePath) {
		Builder->SetCurrentDebugLocation(
			DILocation::get(LexicalBlocks.back()->getContext(),
				token->lineNumber + 1,	// Line (DWARF is 1-indexed)
				0,						// Column
				LexicalBlocks.back()));
	}

	// Evaluate right side (rvalue)
	Value* exprVal = (Value*)(exprNode->*(exprNode->codegen))(pass);
	if (wasError) {
		return nullptr;
	}
	if (!exprVal) {
		return messageSystem::error("Set expression requires right argument");
	}

	messageSystem::endBlock();


	messageSystem::startBlock(leftNode, "Generating expression left side", __func__, __LINE__, __FILE__);

	Value* targetPtr = nullptr;
	Type* targetType = nullptr;

	// If the left side is a pointer lvalue
	if (leftNode->nodeType != Identifier_Node && leftNode->nodeType != Colon_Separator_Node) {
		leftNode->lvalue = true;
		// left side is an expression, evaluate to pointer (lvalue address)
		size_t stackDepthBefore = lastRetrievedElementType.size();
		targetPtr = (Value*)(leftNode->*(leftNode->codegen))(pass);
		if (wasError) {
			return nullptr;
		}
		if (!targetPtr || !targetPtr->getType()->isPointerTy()) {
			return messageSystem::error("Left side must evaluate to a pointer for assignment, is type: \"" + getStringTypeFromLLVMType(targetPtr->getType()) + "\"");
		}
		// Pop the element type if the lvalue codegen pushed one (member access does, array access does not)
		if (lastRetrievedElementType.size() > stackDepthBefore) {
			targetType = lastRetrievedElementType.top()->baseLLVMType;
			lastRetrievedElementType.pop();
		}
	}

	ASTNode* typeNode = nullptr;
	Type* type = nullptr;
	int pointerLevel = 0;
	bool isConst = false;
	if (!leftNode->lvalue) {
		if (leftNode->childNodes.size() > 0) {
			typeNode = leftNode->childNodes[1];
		getNextPointerLevel:
			if (typeNode->token->first == "const") {
				isConst = true;
				typeNode = typeNode->childNodes[0];
				goto getNextPointerLevel;
			}
			if (typeNode->token->first == "ref" || typeNode->token->first == "exact") {
				typeNode = typeNode->childNodes[0];
				goto getNextPointerLevel;
			}
			if (typeNode->token->first == "*") {
				pointerLevel++;
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
		messageSystem::startBlock(typeNode, "Generating type", __func__, __LINE__, __FILE__);

		std::string typeName = typeNode->token->first;
		if (typeSigns.find(typeName) != typeSigns.end())  // If builtin type
			exprVal = castValue(exprVal, type, true, typeSigns[typeNode->token->first], exprNode->token);
		else if (structDefinitions.find(typeName) != structDefinitions.end())  // If defined struct
			exprVal = castValue(exprVal, type, true, false, exprNode->token, true);
		else {
			return messageSystem::error("Unknown type");
		}
		if (wasError || exprVal == nullptr) {
			return messageSystem::error("Unable to automatically cast");
		}

		messageSystem::endBlock();
	}

	// If the left side is a typed identifier, like: `name : int`, then set leftNode equal to just the identifier
	if (leftNode->nodeType == Colon_Separator_Node)
		if (leftNode->childNodes.size() > 0)
			leftNode = leftNode->childNodes[0];


	// If the left side is an identifier
	bool newConstLocal = false;
	if (leftNode->nodeType == Identifier_Node) {
		// Simple variable: find alloca and use it as targetPtr.
		// If there is an explicit type annotation, this is always a new declaration (shadowing).
		valueType* val = typeNode ? nullptr : findNamedValue(parentNode, this, leftNode->token->first, token);
		if (!val) {
			targetPtr = CreateEntryBlockAlloca(theFunction, type, leftNode->token->first);
			std::string actualType = "*int";
			if (!typeNode) {
				actualType = getStringTypeFromLLVMType(type);
				// Fall back to the declared type string if LLVM type inference fails
				if (actualType.find("unknown") != std::string::npos && exprNode->asaType && !exprNode->asaType->strVal.empty())
					actualType = exprNode->asaType->strVal;
			}
			else
				actualType = (pointerLevel > 0 ? std::string(pointerLevel, '*') : "") + typeNode->token->first;
			namedValues[leftNode->token->first] = new valueType(leftNode->token->first, actualType, targetPtr);
			namedValues[leftNode->token->first]->isConstant = isConst;
			namedValues[leftNode->token->first]->declNode = this->parentNode;
			newConstLocal = isConst;

			// Add debug info ONLY if we have a valid scope and the stack is not empty
			if (DBuilder && !LexicalBlocks.empty() && token && token->filePath) {
				DIScope* Scope = LexicalBlocks.back();
				unsigned LineNo = token->lineNumber + 1;  // DWARF is 1-indexed

				// Create DIType
				DIType* DebugType = createDIType(type, actualType);

				DIFile* VarFile = (token->filePath && !token->filePath->empty())
									  ? getDIFile(*token->filePath)
									  : TheFile;
				if (DebugType) {
					DILocalVariable* D = DBuilder->createAutoVariable(
						Scope,
						leftNode->token->first,
						VarFile,
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
				return messageSystem::error("Cannot modify const variable '" + leftNode->token->first + "'");
			}

			targetPtr = val->val;

			// If this is a reference, load the pointer before storing through it
			if (val->isReference) {
				targetPtr = Builder->CreateLoad(getValueStoredType(val->val), targetPtr, leftNode->token->first + "_ref_store_ptr");
				// Resolve element type from the type string rather than the alloca type
				std::string refTypeStr = val->type;
				int refPtrLvl = 0;
				while (!refTypeStr.empty() && refTypeStr[0] == '*') {
					refTypeStr = refTypeStr.substr(1);
					refPtrLvl++;
				}
				bool refWd = true;
				targetType = getLLVMTypeFromString(refTypeStr, 0, token, refWd, pass);
				if (targetType && refPtrLvl > 0)
					targetType = PointerType::getUnqual(*TheContext);
				if (!targetType)
					targetType = getValueStoredType(val->val);
			}
			else {
				targetType = getValueStoredType(targetPtr);
			}
		}
	}
	else if (targetType == nullptr)
		targetType = type;

	messageSystem::endBlock();


	messageSystem::startBlock(this, "Generating compound assignment operation", __func__, __LINE__, __FILE__);

	// Compound assignment: load current value, apply op, then store result
	if (token->second == Plus_Equal || token->second == Minus_Equal ||
		token->second == Times_Equal || token->second == Slash_Equal) {
		// Use targetType directly; LLVM may fold GEPs, making instruction introspection unreliable.
		Type* loadType = targetType;
		Value* currentVal = Builder->CreateLoad(loadType, targetPtr, "cmpd_load");

		bool isFloat = loadType->isFloatingPointTy();
		bool isInt = loadType->isIntegerTy();

		if (isFloat || isInt) {
			// Cast RHS to the variable's type so the result stays the same type
			if (exprVal->getType() != loadType) {
				exprVal = castValue(exprVal, loadType, true, false, token);
				if (wasError)
					return nullptr;
			}
			if (token->second == Plus_Equal)
				exprVal = isFloat ? Builder->CreateFAdd(currentVal, exprVal, "cmpd_add")
								  : Builder->CreateAdd(currentVal, exprVal, "cmpd_add");
			else if (token->second == Minus_Equal)
				exprVal = isFloat ? Builder->CreateFSub(currentVal, exprVal, "cmpd_sub")
								  : Builder->CreateSub(currentVal, exprVal, "cmpd_sub");
			else if (token->second == Times_Equal)
				exprVal = isFloat ? Builder->CreateFMul(currentVal, exprVal, "cmpd_mul")
								  : Builder->CreateMul(currentVal, exprVal, "cmpd_mul");
			else if (token->second == Slash_Equal)
				exprVal = isFloat ? Builder->CreateFDiv(currentVal, exprVal, "cmpd_div")
								  : Builder->CreateSDiv(currentVal, exprVal, "cmpd_div");
		}
		else {
			// Non-scalar: delegate to operator overload for custom types
			static const std::unordered_map<TokenType, std::string> compoundOpName = {
				{Plus_Equal, "operator." + tokenAsString(Plus)},
				{Minus_Equal, "operator." + tokenAsString(Minus)},
				{Times_Equal, "operator." + tokenAsString(Star)},
				{Slash_Equal, "operator." + tokenAsString(Slash)},
			};
			std::string operatorName = compoundOpName.at(token->second);
			argumentList argList;
			std::string lTypeStr = getStringTypeFromLLVMType(currentVal->getType());
			argList.push_back(argType(lTypeStr, getASTNodeTypeFromString(lTypeStr), 0));
			std::string rTypeStr = getStringTypeFromLLVMType(exprVal->getType());
			argList.push_back(argType(rTypeStr, getASTNodeTypeFromString(rTypeStr), 0));
			functionID* calleeID = getFunctionFromID(functionIDs, operatorName, argList, token, true);
			if (!calleeID || !calleeID->fnValue) {
				return messageSystem::error("No operator overload '" + operatorName + "' found for compound assignment");
			}
			calleeID->uses++;
			std::vector<Value*> ArgsV = {currentVal, exprVal};
			if (calleeID->isStructReturn) {
				auto structIt = structDefinitions.find(calleeID->returnType);
				if (structIt == structDefinitions.end() || !structIt->second->structVal) {
					return messageSystem::error("Struct return type not found for compound operator overload");
				}
				AllocaInst* sretAlloc = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(), structIt->second->structVal, "cmpd_sret");
				ArgsV.insert(ArgsV.begin(), sretAlloc);
				Builder->CreateCall(calleeID->fnValue, ArgsV);
				exprVal = Builder->CreateLoad(structIt->second->structVal, sretAlloc, "cmpd_struct");
			}
			else {
				exprVal = Builder->CreateCall(calleeID->fnValue, ArgsV, "cmpd_struct");
			}
		}
	}
	messageSystem::endBlock();


	messageSystem::startBlock(this, "Existing variable assignment", __func__, __LINE__, __FILE__);

	// If assigning to an existing variable and types differ, cast to the stored type
	if (targetType && exprVal->getType() != targetType) {
		exprVal = castValue(exprVal, targetType, true, false, token);
		if (wasError)
			return nullptr;

		if (exprVal == nullptr)
			return messageSystem::error("Could not implicitly cast expression to variable type.");
	}

	StoreInst* storeInst = Builder->CreateStore(exprVal, targetPtr);

	// For a newly declared const local, tell the optimizer this memory is
	// invariant after initialization so it can treat reads as constants.
	// Only emit invariant.start when in the entry block: non-entry blocks
	// (e.g. after a branch) can cause numbering conflicts in LLVM 21.
	if (newConstLocal && theFunction) {
		BasicBlock* curBB = Builder->GetInsertBlock();
		BasicBlock* entryBB = &theFunction->getEntryBlock();
		if (curBB == entryBB) {
			Type* storedType = exprVal->getType();
			uint64_t typeSize = TheModule->getDataLayout().getTypeAllocSize(storedType);
			Function* invariantStartFn = Intrinsic::getDeclaration(
				TheModule.get(), Intrinsic::invariant_start,
				{PointerType::getUnqual(*TheContext)});
			Builder->CreateCall(invariantStartFn,
				{ConstantInt::get(Type::getInt64Ty(*TheContext), typeSize), targetPtr});
		}
	}

	messageSystem::endBlock();


	messageSystem::endBlock();
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
	var = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(), type, identifierNode->token->first);
	//}
	return var;
}

// Value*
void* ASTNode::generateUnaryExpression(int pass)
{
	messageSystem::startBlock(this, "Generating unary expression", __func__, __LINE__, __FILE__);

	if (childNodes.size() == 0) {
		return messageSystem::error("Unary expression requires argument");
	}
	if (childNodes[0]->codegen == nullptr) {
		return messageSystem::error("Node `" + ASTNodeTypeAsString(childNodes[0]->nodeType) + "` does not have a code generator");
	}
	Value* R = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
	if (wasError) {
		messageSystem::endBlock();
		return nullptr;
	}
	if (!R) {
		messageSystem::endBlock();
		return nullptr;
	}

	ASTNodeType t = childNodes[0]->nodeType;

	switch (nodeType) {
		case Address_Of_Operation: {
			ASTNode* child = childNodes[0];
			if (child->nodeType == Identifier_Node) {
				// Plain variable: return the alloca directly without touching asaType
				valueType* val = findNamedValue(parentNode, this, child->token->first, token);
				if (!val && !wasError) {
					return messageSystem::error("Unknown variable name for address-of");
				}
				messageSystem::endBlock();
				return (Value*)(val->val);
			}
			// For member access, array subscript, etc.: evaluate as lvalue to get pointer
			child->lvalue = true;
			Value* ptr = (Value*)(child->*(child->codegen))(pass);
			if (wasError) {
				messageSystem::endBlock();
				return nullptr;
			}
			if (!ptr) {
				return messageSystem::error("Cannot take address of expression");
			}
			messageSystem::endBlock();
			return ptr;
		}

		case Dereference_Operation: {
			ASTNode* ptrNode = childNodes[0];
			Value* ptrVal = (Value*)(ptrNode->*(ptrNode->codegen))(pass);
			if (wasError) {
				messageSystem::endBlock();
				return nullptr;
			}
			if (!ptrVal) {
				return messageSystem::error("Dereference of null pointer");
			}
			// Determine element type from allocation when available
			Type* elementType = nullptr;
			std::string elementTypeStr;
			if (AllocaInst* allocaVal = dyn_cast<AllocaInst>(ptrVal)) {
				elementType = allocaVal->getAllocatedType();
			}
			else if (ptrNode->asaType && !ptrNode->asaType->strVal.empty() && ptrNode->asaType->strVal[0] == '*') {
				// Resolve the pointee type from the recorded ASA type string
				elementTypeStr = ptrNode->asaType->strVal.substr(1);
				bool wasDefined = true;
				elementType = getLLVMTypeFromString(elementTypeStr, 0, token, wasDefined, pass);
			}
			if (!elementType)
				elementType = Type::getInt32Ty(*TheContext);

			// If used as lvalue (*ptr = val), return the pointer so the caller stores through it
			if (lvalue) {
				if (!asaType)
					asaType = new ASAType(elementType);
				else
					asaType->baseLLVMType = elementType;
				asaType->strVal = elementTypeStr;
				lastRetrievedElementType.push(asaType);
				messageSystem::endBlock();
				return ptrVal;
			}

			messageSystem::endBlock();
			return Builder->CreateLoad(elementType, ptrVal, "deref_tmp");
		}

		case Expression_Minus: {
			ASTNode* valueNode = childNodes[0];
			Value* v = (Value*)(valueNode->*(valueNode->codegen))(pass);
			if (wasError) {
				messageSystem::endBlock();
				return nullptr;
			}
			if (!v) {
				return messageSystem::error("Cannot take negative of value");
			}
			if (v->getType()->isIntegerTy()) {
				messageSystem::endBlock();
				return Builder->CreateNeg(v, "neg_tmp");
			}
			else if (v->getType()->isFloatingPointTy()) {
				messageSystem::endBlock();
				return Builder->CreateFNeg(v, "fneg_tmp");
			}
			else {
				return messageSystem::error("Cannot take negative of value");
			}
		}

		case Logical_Not: {
			Value* boolVal = R->getType()->isIntegerTy(1) ? R : Builder->CreateICmpNE(R, Constant::getNullValue(R->getType()), "tobool");
			Value* result = Builder->CreateNot(boolVal, "not_tmp");
			messageSystem::endBlock();
			return result;
		}

		default:
			return messageSystem::error("Unknown or undefined operator");
	}

	messageSystem::endBlock();
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
	messageSystem::startBlock(this, "Generating binary expression", __func__, __LINE__, __FILE__);

	if (!LexicalBlocks.empty() && token && token->filePath)
		Builder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));
	if (childNodes.size() < 2) {
		return messageSystem::error("Binary expression requires left and right arguments");
	}

	if (!childNodes[0]->codegen || !childNodes[1]->codegen) {
		return messageSystem::error("Binary expression operands missing code generators");
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

		messageSystem::endBlock();
		return R;
	}

	if (nodeType == Logical_And || nodeType == Logical_Or) {
		auto toBool = [&](Value* V, const std::string& name) -> Value* {
			return V->getType()->isIntegerTy(1) ? V : Builder->CreateICmpNE(V, Constant::getNullValue(V->getType()), name);
		};

		Value* L = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
		if (!L) {
			return messageSystem::error("Error generating left side of logical expression");
		}

		Value* lBool = toBool(L, "tobool_l");
		Function* TheFunction = Builder->GetInsertBlock()->getParent();
		BasicBlock* LhsBB = Builder->GetInsertBlock();
		BasicBlock* RhsBB = BasicBlock::Create(*TheContext, nodeType == Logical_And ? "land.rhs" : "lor.rhs", TheFunction);
		BasicBlock* MergeBB = BasicBlock::Create(*TheContext, nodeType == Logical_And ? "land.end" : "lor.end");

		if (nodeType == Logical_And)
			Builder->CreateCondBr(lBool, RhsBB, MergeBB);
		else
			Builder->CreateCondBr(lBool, MergeBB, RhsBB);

		Builder->SetInsertPoint(RhsBB);
		Value* R = (Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);
		if (!R) {
			return messageSystem::error("Error generating right side of logical expression");
		}

		Value* rBool = toBool(R, "tobool_r");
		Builder->CreateBr(MergeBB);
		BasicBlock* RhsEvalBB = Builder->GetInsertBlock();

		TheFunction->insert(TheFunction->end(), MergeBB);
		Builder->SetInsertPoint(MergeBB);

		PHINode* Phi = Builder->CreatePHI(Type::getInt1Ty(*TheContext), 2, nodeType == Logical_And ? "and_tmp" : "or_tmp");
		if (nodeType == Logical_And) {
			Phi->addIncoming(ConstantInt::getFalse(*TheContext), LhsBB);
			Phi->addIncoming(rBool, RhsEvalBB);
		}
		else {
			Phi->addIncoming(ConstantInt::getTrue(*TheContext), LhsBB);
			Phi->addIncoming(rBool, RhsEvalBB);
		}
		messageSystem::endBlock();
		return Phi;
	}

	Value* L = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
	Value* R = (Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);

	if (!L || !R) {
		return messageSystem::error("Error generating term");
	}

	// Check for operator overloads first (skip for pointer operands)
	bool eitherIsPointer = L->getType()->isPointerTy() || R->getType()->isPointerTy();
	if (!eitherIsPointer && (nodeType == Redefined_Operator_Expr || checkForOperatorOverload(L, R))) {
		messageSystem::endBlock();
		return generateOperatorOverloadCall(L, R);
	}

	// Auto-cast to highest precision if types differ
	if (L->getType() != R->getType()) {
		if (warningFlags == W_Conversion)
			printTokenWarning(getASTTokenRange(this), "Operand type mismatch, performing implicit conversion.  (-wconversion)");
		castToHighestAccuracy(L, R, token);
		if (wasError) {
			messageSystem::endBlock();
			return nullptr;
		}
		if (L->getType() != R->getType()) {
			return messageSystem::error("Operands to multiply are not the same type (after automatic cast)");
		}
	}

	ValueCategory category = getValueCategory(L->getType());

	switch (category) {
		case ValueCategory::Integer:
			if (!L->getType()->isIntegerTy() || !R->getType()->isIntegerTy()) {
				return messageSystem::error("Integer operation: operands are not both integers");
			}
			messageSystem::endBlock();
			return generateIntegerBinaryOp(L, R);
		case ValueCategory::Float:
			if (!L->getType()->isFloatingPointTy() || !R->getType()->isFloatingPointTy()) {
				return messageSystem::error("Float operation: operands are not both floating-point");
			}
			messageSystem::endBlock();
			return generateFloatBinaryOp(L, R);
		case ValueCategory::Pointer:
			messageSystem::endBlock();
			return generatePointerBinaryOp(L, R);
		default:
			return messageSystem::error("Unsupported operand types for binary operation, <" + getStringTypeFromLLVMType(L->getType()) + "> and <" + getStringTypeFromLLVMType(R->getType()) + ">");
	}

	messageSystem::endBlock();
	return nullptr;
}

Value* ASTNode::generateIntegerBinaryOp(Value* L, Value* R)
{
	messageSystem::startBlock(this, "Generating integer binary operation", __func__, __LINE__, __FILE__);

	// Check for comparison operations first
	auto compIt = intCompareOps.find(nodeType);
	if (compIt != intCompareOps.end()) {
		messageSystem::endBlock();
		return Builder->CreateICmp(compIt->second, L, R, "icmp_tmp");
	}

	// Regular arithmetic/bitwise operations
	auto opIt = integerOps.find(nodeType);
	if (opIt != integerOps.end()) {
		messageSystem::endBlock();
		return Builder->CreateBinOp(opIt->second, L, R, "int_op");
	}

	return (Value*)messageSystem::error("Unknown integer binary operator");
}

Value* ASTNode::generateFloatBinaryOp(Value* L, Value* R)
{
	messageSystem::startBlock(this, "Generating float binary operation", __func__, __LINE__, __FILE__);

	// Check for comparison operations first
	auto compIt = floatCompareOps.find(nodeType);
	if (compIt != floatCompareOps.end()) {
		messageSystem::endBlock();
		return Builder->CreateFCmp(compIt->second, L, R, "fcmp_tmp");
	}

	// Regular arithmetic operations
	auto opIt = floatOps.find(nodeType);
	if (opIt != floatOps.end()) {
		messageSystem::endBlock();
		return Builder->CreateBinOp(opIt->second, L, R, "float_op");
	}

	return (Value*)messageSystem::error("Unknown float binary operator");
}

Value* ASTNode::generatePointerBinaryOp(Value* L, Value* R)
{
	messageSystem::startBlock(this, "Generating pointer binary operation", __func__, __LINE__, __FILE__);

	// Pointer comparisons (== and !=, e.g. ptr == void / ptr != void)
	auto compIt = intCompareOps.find(nodeType);
	if (compIt != intCompareOps.end()) {
		messageSystem::endBlock();
		return Builder->CreateICmp(compIt->second, L, R, "ptr_cmp");
	}

	return (Value*)messageSystem::error("Invalid pointer operation");
}

void* ASTNode::generatePipePlaceholder(int pass)
{
	messageSystem::startBlock(this, "Generating pipe operation placeholder", __func__, __LINE__, __FILE__);

	if (pipeOperationValue.size() > 0) {
		messageSystem::endBlock();
		return pipeOperationValue.top();
	}

	return messageSystem::error("Pipe operation placeholder '%' can only be used after a pipe operation");
}

// Get the type string for one operand of a binary expression.
// Uses AST identifier/member-access info when available, so that types like
// char vs uint8 (both i8 in LLVM) are resolved correctly.
static std::string getOperandTypeString(ASTNode* binaryNode, int childIdx, Value* llvmVal)
{
	if (binaryNode && binaryNode->parentNode && (int)binaryNode->childNodes.size() > childIdx) {
		ASTNode* child = binaryNode->childNodes[childIdx];
		ASTNode* inner = nullptr;
		// Direct identifier: use it
		if (child && child->nodeType == Identifier_Node && child->token) {
			inner = child;
		}
		// Single-child wrapper around a plain identifier:
		// peek inside, but do NOT unwrap multi-child nodes like member access (a.b)
		// because that would return the type of the base 'a', not of 'a.b'.
		else if (child && child->childNodes.size() == 1 &&
				 child->nodeType != Dereference_Operation &&
				 child->nodeType != Address_Of_Operation) {
			ASTNode* c0 = child->childNodes[0];
			if (c0 && c0->nodeType == Identifier_Node && c0->token)
				inner = c0;
		}
		if (inner) {
			valueType* val = findNamedValue(binaryNode->parentNode, binaryNode, inner->token->first, binaryNode->token);
			if (val && !val->type.empty())
				return val->type;
		}
	}
	return getStringTypeFromLLVMType(llvmVal->getType());
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
	std::string lTypeStr = getOperandTypeString(this, 0, L);
	uint8_t lPointerLevel = 0;
	std::string lBaseTypeStr = lTypeStr;
	while (lBaseTypeStr.length() > 0 && lBaseTypeStr[0] == '*') {
		lPointerLevel++;
		lBaseTypeStr = lBaseTypeStr.substr(1);
	}
	argList.push_back(argType(lBaseTypeStr, getASTNodeTypeFromString(lBaseTypeStr), lPointerLevel));

	std::string rTypeStr = getOperandTypeString(this, 1, R);
	uint8_t rPointerLevel = 0;
	std::string rBaseTypeStr = rTypeStr;
	while (rBaseTypeStr.length() > 0 && rBaseTypeStr[0] == '*') {
		rPointerLevel++;
		rBaseTypeStr = rBaseTypeStr.substr(1);
	}
	argList.push_back(argType(rBaseTypeStr, getASTNodeTypeFromString(rBaseTypeStr), rPointerLevel));

	//printFunctionPrototypes();

	functionID* calleeID = getFunctionFromID(functionIDs, operatorName, argList, token, true);
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
	messageSystem::startBlock(this, "Generating operator overload call", __func__, __LINE__, __FILE__);

	std::string operatorName = "operator." + tokenAsString(token->second);

	// Build argumentList from L and R types
	argumentList argList;
	std::string lTypeStr = getOperandTypeString(this, 0, L);
	uint8_t lPointerLevel = 0;
	std::string lBaseTypeStr = lTypeStr;
	while (lBaseTypeStr.length() > 0 && lBaseTypeStr[0] == '*') {
		lPointerLevel++;
		lBaseTypeStr = lBaseTypeStr.substr(1);
	}
	argList.push_back(argType(lBaseTypeStr, getASTNodeTypeFromString(lBaseTypeStr), lPointerLevel));

	std::string rTypeStr = getOperandTypeString(this, 1, R);
	uint8_t rPointerLevel = 0;
	std::string rBaseTypeStr = rTypeStr;
	while (rBaseTypeStr.length() > 0 && rBaseTypeStr[0] == '*') {
		rPointerLevel++;
		rBaseTypeStr = rBaseTypeStr.substr(1);
	}
	argList.push_back(argType(rBaseTypeStr, getASTNodeTypeFromString(rBaseTypeStr), rPointerLevel));

	functionID* calleeID = getFunctionFromID(functionIDs, operatorName, argList, token, true);

	if (!calleeID || !calleeID->fnValue) {
		return (Value*)messageSystem::error("Expected operator overload for undefined operator `" + tokenAsString(token->second) + "`, but none were not found");
	}

	calleeID->uses++;

	// Implicit numeric coercion: cast each argument to the formal parameter type
	std::vector<Value*> ArgsV = {L, R};
	for (int i = 0; i < 2; i++) {
		int formalIdx = i + (calleeID->isStructReturn ? 1 : 0);
		if (formalIdx < (int)calleeID->fnValue->getFunctionType()->getNumParams()) {
			Type* formalType = calleeID->fnValue->getFunctionType()->getParamType(formalIdx);
			if (formalType && ArgsV[i]->getType() != formalType &&
				!ArgsV[i]->getType()->isStructTy() && !ArgsV[i]->getType()->isPointerTy() &&
				!formalType->isStructTy() && !formalType->isPointerTy()) {
				ArgsV[i] = castValue(ArgsV[i], formalType, true, false, token);
				if (wasError) {
					messageSystem::endBlock();
					return nullptr;
				}
			}
		}
	}

	// If the operator returns a struct, we need to pass an sret pointer as the first arg
	if (calleeID->isStructReturn) {
		auto structIt = structDefinitions.find(calleeID->returnType);
		if (structIt == structDefinitions.end() || !structIt->second->structVal) {
			return (Value*)messageSystem::error("Struct return type not defined for operator overload");
		}
		AllocaInst* sretAlloc = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(), structIt->second->structVal, "op_sret");
		ArgsV.insert(ArgsV.begin(), sretAlloc);
		Builder->CreateCall(calleeID->fnValue, ArgsV);

		messageSystem::endBlock();
		return Builder->CreateLoad(structIt->second->structVal, sretAlloc, "op_overload");
	}

	messageSystem::endBlock();
	return Builder->CreateCall(calleeID->fnValue, ArgsV, "op_overload");
}

// Value*
void* ASTNode::generateAccessOperation(int pass)
{
	messageSystem::startBlock(this, "Generating access operation", __func__, __LINE__, __FILE__);

	if (childNodes.size() == 0) {
		return messageSystem::error("Access operation requires a left and right argument");
	}

	childNodes[0]->lvalue = true;  // Set flag for base to return address if needed
	Value* L = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
	if (wasError) {
		messageSystem::endBlock();
		return nullptr;
	}
	Value* R = (Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);
	if (wasError) {
		messageSystem::endBlock();
		return nullptr;
	}
	if (!L || !R) {
		messageSystem::endBlock();
		return nullptr;
	}

	// Check if R is an integer
	if (!R->getType()->isIntegerTy()) {
		return messageSystem::error("Right argument of access operator must be an integer");
	}

	// Inherit asaType from the child if not already set
	if (!asaType && childNodes[0]->asaType)
		asaType = childNodes[0]->asaType;

	if (!asaType) {
		return messageSystem::error("Was unable to resolve type of base being accessed");
	}

	// Get the element type from baseType (set by member access or previous operations)
	Type* elementType = asaType->baseLLVMType;
	std::string resolvedElementTypeStr;

	// If baseType is a pointer, we need to determine what it points to
	bool isRefToStruct = false;
	if (asaType->baseLLVMType && asaType->baseLLVMType->isPointerTy()) {
		std::string typeStr = asaType->strVal;
		// Strip exactly one leading '*' to get the element type string for this subscript.
		// getLLVMTypeFromString handles any further '*' prefixes in the element type string.
		std::string elementTypeStr = (!typeStr.empty() && typeStr[0] == '*') ? typeStr.substr(1) : typeStr;
		resolvedElementTypeStr = elementTypeStr;
		// If no stars were present and the base names a known struct, this is ref-to-struct
		if (elementTypeStr == typeStr && !typeStr.empty() && structDefinitions.count(typeStr)) {
			// ref T where T is a struct: resolve element type from struct members
			isRefToStruct = true;
			auto structIt = structDefinitions.find(typeStr);
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
		else if (!elementTypeStr.empty()) {
			bool wd = true;
			int resolvePass = 2;
			Type* resolved = getLLVMTypeFromString(elementTypeStr, 0, token, wd, resolvePass);
			if (resolved)
				elementType = resolved;
		}
		else if (!lastRetrievedElementType.empty()) {
			elementType = lastRetrievedElementType.top()->baseLLVMType;
		}
		else {
			return messageSystem::error("Was unable to resolve type of base being accessed");
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
	Type* ptrType = PointerType::getUnqual(*TheContext);

	// Check if L is an alloca instruction or a pointer to a pointer
	// In that case, we need to load the actual pointer value
	if (AllocaInst* allocaInst = dyn_cast<AllocaInst>(L)) {
		actualPtr = Builder->CreateLoad(ptrType, L, "ptr_deref");
		if (isRefToStruct)
			actualPtr = Builder->CreateLoad(ptrType, actualPtr, "ref_struct_deref");
		else if (asaType->isRef)
			actualPtr = Builder->CreateLoad(ptrType, actualPtr, "ref_ptr_deref");
	}
	else if (GlobalVariable* gv = dyn_cast<GlobalVariable>(L)) {
		// Global pointer variables need a load to get the heap pointer
		if (!isRefToStruct)
			actualPtr = Builder->CreateLoad(ptrType, L, "global_ptr_deref");
	}
	else if (L->getType()->isPointerTy()) {
		// Check if this is coming from a struct member access (GEP instruction)
		// by checking if the last operation was a struct GEP
		if (GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(L)) {
			// This is a GEP from struct member access, load the pointer
			actualPtr = Builder->CreateLoad(ptrType, L, "ptr_deref");
		}
		// Otherwise, L is already a direct pointer (e.g., function parameter)
		// so we use it as-is
	}

	// Create GEP instruction
	Value* gep = Builder->CreateGEP(elementType, actualPtr, R, "arrayidx");

	// If this is an lvalue (for assignment), push element type so compound assignment can load it
	if (lvalue) {
		asaType = new ASAType(elementType);
		asaType->strVal = resolvedElementTypeStr;
		lastRetrievedElementType.push(asaType);

		messageSystem::endBlock();
		return gep;
	}

	// Propagate element type string for rvalue uses (e.g. member access on subscript result)
	if (!asaType)
		asaType = new ASAType(elementType);
	else
		asaType->baseLLVMType = elementType;
	asaType->strVal = resolvedElementTypeStr;

	messageSystem::endBlock();

	// If rvalue, load and return the value
	return Builder->CreateLoad(elementType, gep, "accessop_load");
}

// Value*
void* ASTNode::generateMemberAccess(int pass)
{
	messageSystem::startBlock(this, "Generating member access operation", __func__, __LINE__, __FILE__);

	if (childNodes.size() == 0) {
		return messageSystem::error("Member access operation requires a left and right argument");
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
					asaType->strVal = vt->type;
					lastRetrievedElementType.push(asaType);
					messageSystem::endBlock();
					if (lvalue)
						return gv;
					return Builder->CreateLoad(gvType, gv, memberName + "_load");
				}
			}
			return messageSystem::error("Module '" + modName + "' has no member '" + memberName + "'");
		}
	}

	if (childNodes[0]->codegen == nullptr) {
		return messageSystem::error("Node `" + ASTNodeTypeAsString(childNodes[0]->nodeType) + "` does not have a code generator");
	}
	if (childNodes[1]->codegen == nullptr) {
		return messageSystem::error("Node `" + ASTNodeTypeAsString(childNodes[1]->nodeType) + "` does not have a code generator");
	}
	childNodes[0]->lvalue = true;  // Set flag for base to return address if needed

	ASTNodeType t = childNodes[0]->nodeType;

	bool operatorOverloaded = false;
	std::string operatorOverloadName = "operator." + tokenAsString(Dot);
	argumentList argList = argumentList();

	size_t stackDepthBefore = lastRetrievedElementType.size();
	Value* L = (Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
	if (wasError) {
		messageSystem::endBlock();
		return nullptr;
	}

	// Evaluate base pointer
	Value* basePtr = L;
	if (!basePtr || !basePtr->getType()->isPointerTy()) {
		return messageSystem::error("Base must be a pointer for access");
	}

	if (childNodes[0]->nodeType == Identifier_Node) {
		valueType* v = findNamedValue(this, nullptr, childNodes[0]->token->first, token);

		if (!v && !wasError) {
			return messageSystem::error("Unknown variable name used");
		}

		// Resolve through pointer indirection if needed
		std::string structTypeName = v->type;
		{
			int ptrDepth = 0;
			while (!structTypeName.empty() && structTypeName[0] == '*') {
				structTypeName = structTypeName.substr(1);
				ptrDepth++;
			}
			if (ptrDepth > 0 && structDefinitions.count(structTypeName)) {
				for (int i = 0; i < ptrDepth; i++)
					basePtr = Builder->CreateLoad(PointerType::getUnqual(*TheContext), basePtr, "ptr_deref");
			}
			else {
				structTypeName = v->type;
			}
		}

		if (structDefinitions.find(structTypeName) == structDefinitions.end()) {
			return messageSystem::error("Type \"" + v->type + "\" has not been defined");
		}
		structType* structDefinition = structDefinitions[structTypeName];

		// If the struct body hasn't been generated yet, generate it
		if (structDefinition->structVal == nullptr)
			Value* argVal = (Value*)(structDefinition->sourceNode->*(structDefinition->sourceNode->codegen))(pass);
		if (wasError) {
			messageSystem::endBlock();
			return nullptr;
		}

		// Get member name and index
		std::string memberName = childNodes[1]->token->first;
		// Handle if member variable access/set
		if (childNodes[1]->nodeType == Identifier_Node) {
			if (structDefinition->memberNameIndexes.find(memberName) == structDefinition->memberNameIndexes.end()) {
				console::indentation = errorDepth;
				if (errorDepth < maxErrorTraceDepth)
					printTokenError(getASTTokenRange(childNodes[1]), "Struct definition does not contain member");	// TODO
				console::indentation++;
				if (errorDepth < maxErrorTraceDepth)
					console::WriteLine("It does have:");
				console::indentation++;
				if (errorDepth < maxErrorTraceDepth)
					for (const auto& name : structDefinition->memberNameIndexes)
						console::WriteLine(name.first, console::cyanFGColor);
				console::indentation -= 2;
				wasError = true;
				errorDepth++;
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
				return messageSystem::error("Invalid element type");
			}

			if (structDefinition->members[memberIndex].isConstant && lvalue) {
				return messageSystem::error("Cannot modify const member '" + memberName + "'");
			}


			//// Create GEP to compute the address
			//Value* gep = Builder->CreateGEP(elementType, basePtr, index, "arrayidx");

			if (v->isReference)
				basePtr = Builder->CreateLoad(PointerType::getUnqual(*TheContext), basePtr, "ref_struct_ptr");
			auto gep = Builder->CreateStructGEP(structDefinition->structVal, basePtr, memberIndex, "struct_member");
			std::string memberStrVal = "";
			for (int _p = 0; _p < structDefinition->members[memberIndex].pointerLevel; ++_p)
				memberStrVal += "*";
			memberStrVal += structDefinition->members[memberIndex].typeString;
			asaType = new ASAType(elementType, false, structDefinition->members[memberIndex].isConstant, memberStrVal, (uint8_t)structDefinition->members[memberIndex].pointerLevel);
			lastRetrievedElementType.push(asaType);

			messageSystem::endBlock();
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
			// 'this' goes in ArgsV (LLVM call) but NOT in argList (lookup uses userArguments which excludes 'this')
			ArgsV.push_back(basePtr);
			if (!ArgsV.back()) {
				messageSystem::endBlock();
				return nullptr;
			}
			// Add rest of argument values
			for (int i = 0; i < args.size(); i++) {
				Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
				if (wasError) {
					messageSystem::endBlock();
					return nullptr;
				}
				ArgsV.push_back(argVal);
				argList.push_back(argType(getStringTypeFromLLVMType(argVal->getType()), getASTNodeTypeFromString(getStringTypeFromLLVMType(argVal->getType())), 0));
				if (!ArgsV.back()) {
					messageSystem::endBlock();
					return nullptr;
				}
			}

			// Look up the id in the struct function (against userArguments, which excludes 'this').
			functionID* CalleeFID = getFunctionFromID(structDefinition->memberFunctions, memberName, argList, token, true, true);
			if (!CalleeFID) {
				console::indentation = errorDepth;
				if (!wasError) {
					printTokenError(getASTTokenRange(childNodes[1]), "Struct definition does not contain member function \"" + memberName + "\"");	// TODO
					bool anyFound = false;
					for (auto& f : structDefinition->memberFunctions) {
						if (f->name != memberName)
							continue;
						if (!anyFound) {
							console::indentation++;
							console::WriteLine("Candidates:", console::yellowFGColor);
							console::indentation++;
							anyFound = true;
						}
						console::printIndent(console::indentation);
						f->print();
					}
				}
				wasError = true;
				errorDepth++;
				return nullptr;
			}

			// Call function
			Function* CalleeF = CalleeFID->fnValue;
			CalleeFID->uses++;
			isCallMemberFunction = true;

			// Handle sret for struct-returning member functions
			bool memberIsStructReturn = CalleeFID->isStructReturn;
			AllocaInst* memberSretAlloc = nullptr;
			if (memberIsStructReturn) {
				structType* retStruct = structDefinitions[CalleeFID->returnType];
				if (!retStruct || retStruct->structVal == nullptr) {
					return messageSystem::error("Struct return type not fully defined");
				}
				memberSretAlloc = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(), retStruct->structVal, "member_sret");
				ArgsV.insert(ArgsV.begin(), memberSretAlloc);
			}

			// If argument mismatch error. ArgsV has [sret?] + 'this' + user args = full LLVM arg count.
			if (CalleeFID->variableNumArguments == false)
				if (CalleeF->arg_size() != ArgsV.size()) {
					return messageSystem::error("Incorrect number of arguments passed to function");
				}
				// If variable arguments, make sure the amount in call are <= the required amount
				else if (CalleeF->arg_size() > ArgsV.size()) {
					return messageSystem::error("Incorrect number of arguments passed to function");
				}

			// Rebuild ArgsV for the actual call
			ArgsV = std::vector<Value*>();
			ArgsV.push_back(basePtr);
			if (!ArgsV.back()) {
				messageSystem::endBlock();
				return nullptr;
			}
			// Add rest of argument values
			for (int i = 0; i < args.size(); i++) {
				if (CalleeFID->userArguments[i].isReference) {
					if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
						messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__);
						return messageSystem::error("Cannot pass value as reference");
					}
					// Check that the argument type matches the ref parameter type
					{
						ASTNode* identNode = args[i]->childNodes[0];
						valueType* argVar = findNamedValue(parentNode, this, identNode->token->first, token);
						if (argVar) {
							Type* actualType = getValueStoredType(argVar->val);
							const argType& fa = CalleeFID->userArguments[i];
							bool wasDef = true;
							Type* formalType = getLLVMTypeFromString(fa.typeString, 0, token, wasDef, pass);
							if (formalType && actualType && !actualType->isPointerTy() && !formalType->isPointerTy() && actualType != formalType) {
								messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__);
								return messageSystem::error("Cannot pass '" + getStringTypeFromLLVMType(actualType) +
															"' as 'ref " + fa.typeString + "': implicit cast to reference is not allowed");
							}
						}
					}
					args[i]->childNodes[0]->isRef = true;
				}
				Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
				if (wasError) {
					messageSystem::endBlock();
					return nullptr;
				}
				ArgsV.push_back(argVal);
				if (!ArgsV.back()) {
					messageSystem::endBlock();
					return nullptr;
				}
			}
			if (memberIsStructReturn)
				ArgsV.insert(ArgsV.begin(), memberSretAlloc);

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

			messageSystem::endBlock();
			if (memberIsStructReturn) {
				structType* retStruct = structDefinitions[CalleeFID->returnType];
				return Builder->CreateLoad(retStruct->structVal, memberSretAlloc, "member_sret_load");
			}
			return callResult;
		}
	}
	// If left is not pointer, assume another member access or index operator
	else {
		structType* structDefinition = nullptr;

		if (lastRetrievedElementType.size() > stackDepthBefore) {
			// Left child pushed a type (e.g. chained member access) -- use it
			ASAType* poppedType = lastRetrievedElementType.top();
			lastRetrievedElementType.pop();
			structDefinition = getStructTypeFromLLVMType(poppedType->baseLLVMType);
			// If LLVM type is opaque (e.g. element from a double-pointer subscript), fall back to strVal
			if (!structDefinition && !poppedType->strVal.empty()) {
				std::string typeName = poppedType->strVal;
				int ptrDepth = 0;
				while (!typeName.empty() && typeName[0] == '*') {
					typeName = typeName.substr(1);
					ptrDepth++;
				}
				// Load through each pointer level to reach the struct pointer
				for (int i = 0; i < ptrDepth; i++)
					basePtr = Builder->CreateLoad(PointerType::getUnqual(*TheContext), basePtr, "elem_ptr_deref");
				auto sdIt = structDefinitions.find(typeName);
				if (sdIt != structDefinitions.end())
					structDefinition = sdIt->second;
			}
		}
		else if (childNodes[0]->asaType && !childNodes[0]->asaType->strVal.empty()) {
			// Left child is a function call or similar -- resolve from declared return type
			std::string typeName = childNodes[0]->asaType->strVal;
			int ptrDepth = 0;
			while (!typeName.empty() && typeName[0] == '*') {
				typeName = typeName.substr(1);
				ptrDepth++;
			}
			// For sret (ptrDepth=0) and single-pointer returns (ptrDepth=1), basePtr is already
			// a pointer to the struct. Only extra levels beyond that require loading.
			for (int i = 1; i < ptrDepth; i++)
				basePtr = Builder->CreateLoad(PointerType::getUnqual(*TheContext), basePtr, "call_ptr_deref");
			auto sdIt = structDefinitions.find(typeName);
			if (sdIt != structDefinitions.end())
				structDefinition = sdIt->second;
		}

		if (structDefinition == nullptr) {
			messageSystem::startBlock(childNodes[0], "Generating struct access", __func__, __LINE__, __FILE__);
			return messageSystem::error("Struct could not be found");
		}

		// Get member name and index
		std::string memberName = childNodes[1]->token->first;
		// Handle if member variable access/set
		if (childNodes[1]->nodeType == Identifier_Node) {
			if (structDefinition->memberNameIndexes.find(memberName) == structDefinition->memberNameIndexes.end()) {
				console::indentation = errorDepth;
				if (errorDepth < maxErrorTraceDepth)
					printTokenError(getASTTokenRange(childNodes[1]), "Struct definition does not contain member");	// TODO
				console::indentation++;
				if (errorDepth < maxErrorTraceDepth)
					console::WriteLine("It does have:");
				console::indentation++;
				if (errorDepth < maxErrorTraceDepth)
					for (const auto& name : structDefinition->memberNameIndexes)
						console::WriteLine(name.first, console::cyanFGColor);
				console::indentation -= 2;
				wasError = true;
				errorDepth++;
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
				return messageSystem::error("Invalid element type");
			}

			//// Create GEP to compute the address
			//Value* gep = Builder->CreateGEP(elementType, basePtr, index, "arrayidx");

			auto gep = Builder->CreateStructGEP(structDefinition->structVal, basePtr, memberIndex, "struct_member");
			asaType = new ASAType(elementType);

			lastRetrievedElementType.push(asaType);

			messageSystem::endBlock();

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
			// 'this' goes in ArgsV (LLVM call) but NOT in argList (lookup uses userArguments which excludes 'this')
			ArgsV.push_back(basePtr);
			if (!ArgsV.back()) {
				messageSystem::endBlock();
				return nullptr;
			}
			// Add rest of argument values
			for (int i = 0; i < args.size(); i++) {
				Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
				if (wasError) {
					messageSystem::endBlock();
					return nullptr;
				}
				ArgsV.push_back(argVal);
				argList.push_back(argType(getStringTypeFromLLVMType(argVal->getType()), getASTNodeTypeFromString(getStringTypeFromLLVMType(argVal->getType())), 0));
				if (!ArgsV.back()) {
					messageSystem::endBlock();
					return nullptr;
				}
			}

			// Look up the id in the struct function (against userArguments, which excludes 'this').
			functionID* CalleeFID = getFunctionFromID(structDefinition->memberFunctions, memberName, argList, token, true, true);
			if (!CalleeFID) {
				messageSystem::startBlock(childNodes[1], "Generating struct function call", __func__, __LINE__, __FILE__);
				return messageSystem::error("Struct definition does not contain member function");
			}

			// Call function
			Function* CalleeF = CalleeFID->fnValue;
			CalleeFID->uses++;
			isCallMemberFunction = true;

			// Handle sret for struct-returning member functions
			bool memberIsStructReturn = CalleeFID->isStructReturn;
			AllocaInst* memberSretAlloc = nullptr;
			if (memberIsStructReturn) {
				structType* retStruct = structDefinitions[CalleeFID->returnType];
				if (!retStruct || retStruct->structVal == nullptr) {
					return messageSystem::error("Struct return type not fully defined");
				}
				memberSretAlloc = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(), retStruct->structVal, "member_sret");
				ArgsV.insert(ArgsV.begin(), memberSretAlloc);
			}

			// If argument mismatch error. ArgsV has [sret?] + 'this' + user args = full LLVM arg count.
			if (CalleeFID->variableNumArguments == false)
				if (CalleeF->arg_size() != ArgsV.size()) {
					return messageSystem::error("Incorrect number of arguments passed to function");
				}
				// If variable arguments, make sure the amount in call are <= the required amount
				else if (CalleeF->arg_size() > ArgsV.size()) {
					return messageSystem::error("Incorrect number of arguments passed to function");
				}

			// Rebuild ArgsV for the actual call
			ArgsV = std::vector<Value*>();
			ArgsV.push_back(basePtr);
			if (!ArgsV.back()) {
				messageSystem::endBlock();
				return nullptr;
			}
			// Add rest of argument values
			for (int i = 0; i < args.size(); i++) {
				if (CalleeFID->userArguments[i].isReference) {
					if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
						messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__);
						return messageSystem::error("Cannot pass value as reference");
					}
					// Check that the argument type matches the ref parameter type
					{
						ASTNode* identNode = args[i]->childNodes[0];
						valueType* argVar = findNamedValue(parentNode, this, identNode->token->first, token);
						if (argVar) {
							Type* actualType = getValueStoredType(argVar->val);
							const argType& fa = CalleeFID->userArguments[i];
							bool wasDef = true;
							Type* formalType = getLLVMTypeFromString(fa.typeString, 0, token, wasDef, pass);
							if (formalType && actualType && !actualType->isPointerTy() && !formalType->isPointerTy() && actualType != formalType) {
								messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__);
								return messageSystem::error("Cannot pass '" + getStringTypeFromLLVMType(actualType) +
															"' as 'ref " + fa.typeString + "': implicit cast to reference is not allowed");
							}
						}
					}
					args[i]->childNodes[0]->isRef = true;
				}
				Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
				if (wasError) {
					messageSystem::endBlock();
					return nullptr;
				}
				ArgsV.push_back(argVal);
				if (!ArgsV.back()) {
					messageSystem::endBlock();
					return nullptr;
				}
			}
			if (memberIsStructReturn)
				ArgsV.insert(ArgsV.begin(), memberSretAlloc);

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


			messageSystem::endBlock();

			if (memberIsStructReturn) {
				structType* retStruct = structDefinitions[CalleeFID->returnType];
				return Builder->CreateLoad(retStruct->structVal, memberSretAlloc, "member_sret_load");
			}
			return callResult;

			//return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
		}
	}


	messageSystem::endBlock();
	return nullptr;
}


// Value*
void* ASTNode::generateScopeBody(int pass)
{
	messageSystem::startBlock(this, "Generating scope body", __func__, __LINE__, __FILE__);

	// Only value blocks (parser-marked via isValueBlock) get their own ResultContext.
	// Control-flow bodies (if/for bodies) that contain `result` propagate to the
	// enclosing value block's context on the stack instead.
	bool isValueBlock = this->isValueBlock;

	BasicBlock* mergeBB = nullptr;
	if (isValueBlock) {
		Function* theFunction = Builder->GetInsertBlock()->getParent();
		mergeBB = BasicBlock::Create(*TheContext, "result_merge", theFunction);
		resultContextStack.push({nullptr, mergeBB});
	}

	for (auto& c : childNodes) {
		if (c->codegen != nullptr) {
			(void)(Value*)(c->*(c->codegen))(pass);
			if (wasError) {
				if (isValueBlock)
					resultContextStack.pop();
				messageSystem::endBlock();
				return nullptr;
			}
		}
		else {
			if (isValueBlock)
				resultContextStack.pop();
			return messageSystem::error("Node `" + ASTNodeTypeAsString(c->nodeType) + "` does not have a code generator");
		}
	}

	if (isValueBlock) {
		// Branch to merge in case the result was inside a conditional and this path
		// has no terminator (keeps the IR well-formed).
		if (!Builder->GetInsertBlock()->getTerminator())
			Builder->CreateBr(mergeBB);

		ResultContext ctx = resultContextStack.top();
		resultContextStack.pop();

		Builder->SetInsertPoint(mergeBB);

		if (!ctx.resultSlot) {
			return messageSystem::error("Value block has no reachable 'result' statement");
		}

		Value* resultVal = Builder->CreateLoad(
			ctx.resultSlot->getAllocatedType(), ctx.resultSlot, "result_val");
		messageSystem::endBlock();
		return resultVal;
	}

	messageSystem::endBlock();
	return nullptr;
}

// Value*
void* ASTNode::generateCast(int pass)
{
	messageSystem::startBlock(this, "Generating cast", __func__, __LINE__, __FILE__);

	if (childNodes.size() < 2 ||
		childNodes[1]->nodeType != Scope_Body ||
		childNodes[1]->childNodes.size() == 0 ||
		childNodes[1]->childNodes[0]->nodeType != Comma_Node ||
		childNodes[1]->childNodes[0]->childNodes.size() < 2) {

		return messageSystem::error("Cast expression expected: `#cast(var, type)`");
	}
	ASTNode* argsNode = childNodes[1]->childNodes[0];
	std::string varName = argsNode->childNodes[0]->token->first;
	std::string tyVal = argsNode->childNodes[1]->token->first;
	tokenPair* typeToken = argsNode->childNodes[1]->token;

	valueType* val = findNamedValue(parentNode, this, varName, token);
	if (!val && !wasError) {
		return messageSystem::error("Unknown variable name used");
	}
	Value* var = val->val;
	Value* value = Builder->CreateLoad(getValueStoredType(var), var, varName + "_load");
	bool wasDefined = true;
	Type* toType = getLLVMTypeFromString(tyVal, 0, typeToken, wasDefined, pass);
	// Determine source signedness from the variable's declared type
	std::string srcTypeStr = val->type;
	while (!srcTypeStr.empty() && srcTypeStr[0] == '*')
		srcTypeStr = srcTypeStr.substr(1);
	bool isSrcSigned = typeSigns.count(srcTypeStr) ? typeSigns[srcTypeStr] : true;
	Value* casted = castValue(value, toType, isSrcSigned, typeSigns[tyVal], token);

	messageSystem::endBlock();

	if (wasError)
		return nullptr;
	return casted;
}

// Value*
static std::string typeStringFromNode(ASTNode* node)
{
	if (!node)
		return "";
	if (node->nodeType == Identifier_Node)
		return node->token->first;
	if (node->nodeType == Pointer_Node || node->nodeType == Dereference_Operation) {
		if (!node->childNodes.empty())
			return "*" + typeStringFromNode(node->childNodes[0]);
		return "*";
	}
	return node->token->first;
}

void* ASTNode::generateBitcast(int pass)
{
	messageSystem::startBlock(this, "Generating bitcast", __func__, __LINE__, __FILE__);

	if (childNodes.size() < 2 ||
		childNodes[1]->nodeType != Scope_Body ||
		childNodes[1]->childNodes.size() == 0 ||
		childNodes[1]->childNodes[0]->nodeType != Comma_Node ||
		childNodes[1]->childNodes[0]->childNodes.size() < 2) {

		return messageSystem::error("Bitcast expression expected: `#bitcast(var, type)`");
	}
	ASTNode* argsNode = childNodes[1]->childNodes[0];
	std::string varName = argsNode->childNodes[0]->token->first;
	std::string tyVal = typeStringFromNode(argsNode->childNodes[1]);

	valueType* val = findNamedValue(parentNode, this, varName, token);
	if (!val && !wasError) {
		return messageSystem::error("Unknown variable name used");
	}
	Value* var = val->val;
	Value* value = Builder->CreateLoad(getValueStoredType(var), var, varName + "_load");

	bool wasDefined = true;
	Type* toType = getLLVMTypeFromString(tyVal, 0, argsNode->childNodes[1]->token, wasDefined, pass);
	if (!toType) {
		return messageSystem::error("Unknown type name used in #bitcast");
	}

	// Pointer <-> integer conversions (ptrtoint / inttoptr)
	if (value->getType()->isPointerTy() && toType->isIntegerTy()) {
		messageSystem::endBlock();
		return Builder->CreatePtrToInt(value, toType, "ptrtoint");
	}
	if (value->getType()->isIntegerTy() && toType->isPointerTy()) {
		messageSystem::endBlock();
		return Builder->CreateIntToPtr(value, toType, "inttoptr");
	}

	uint64_t srcBits = value->getType()->getPrimitiveSizeInBits();
	uint64_t dstBits = toType->getPrimitiveSizeInBits();
	if (srcBits == 0 || dstBits == 0 || srcBits != dstBits) {
		return messageSystem::error("Bitcast requires source and destination types to have the same bit width. (" + std::to_string(srcBits) + " != " + std::to_string(dstBits) + ").");
	}

	messageSystem::endBlock();
	return Builder->CreateBitCast(value, toType, "bitcast");
}

// Value*
void* ASTNode::generateTypeInstance(int pass)
{
	messageSystem::startBlock(this, "Generating struct instance", __func__, __LINE__, __FILE__);

	if (childNodes.size() < 2 || childNodes[1]->childNodes.size() == 0) {
		return messageSystem::error("New expression requires type name, like `#new type;`");
	}

	std::string typeName = childNodes[1]->childNodes[0]->token->first;
	if (structDefinitions.find(typeName) == structDefinitions.end()) {
		messageSystem::startBlock(childNodes[1]->childNodes[0], "Generating type from name", __func__, __LINE__, __FILE__);
		return messageSystem::error("Unknown type name used");
	}

	structType* typeVal = structDefinitions[typeName];

	// If the struct body hasn't been generated yet, generate it
	if (typeVal->structVal == nullptr)
		Value* argVal = (Value*)(typeVal->sourceNode->*(typeVal->sourceNode->codegen))(pass);
	if (wasError) {
		messageSystem::endBlock();
		return nullptr;
	}

	AllocaInst* var = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(), typeVal->structVal, "struct_alloc");

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
		if (wasError) {
			messageSystem::endBlock();
			return nullptr;
		}
		Builder->CreateStore(defaultVal, memberPtr);
	}

	messageSystem::endBlock();
	return var;
}

// Value*
void* ASTNode::generateCallExpression(int pass)
{
	messageSystem::startBlock(this, "Generating function call", __func__, __LINE__, __FILE__);

	if (!LexicalBlocks.empty() && token && token->filePath)
		Builder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));
	ASTNode* argsNode = childNodes[0];
	bool shouldBeMemberFunction = isCallMemberFunction;
	isCallMemberFunction = false;

	std::vector<ASTNode*> args = std::vector<ASTNode*>();
	for (auto& a : argsNode->childNodes)
		if (a->childNodes.size() > 0) {
			args.push_back(a);
		}

	std::vector<Value*> ArgsV = std::vector<Value*>();
	std::vector<Value*> cachedArgVals = std::vector<Value*>();
	argumentList argList = argumentList();

	// Build argList and ArgsV WITHOUT sret initially
	for (int i = 0; i < args.size(); i++) {
		Value* argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
		if (wasError) {
			return nullptr;
		}
		cachedArgVals.push_back(argVal);
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
		console::indentation = errorDepth;
		if (errorDepth < maxErrorTraceDepth)
			if (shouldBeMemberFunction)
				printf("Should be member function\n");
		if (!wasError)
			printUndefinedFunctionError(token, token->first, argList, functionIDs);
		wasError = true;
		errorDepth++;
		return nullptr;
	}

	// Check @deprecated / @removed on the declaration
	if (CalleeFID->declNode)
		if (!checkUsageAttrs(token, CalleeFID->declNode->attributes, CalleeFID->name))
			return nullptr;

	Function* CalleeF = CalleeFID->fnValue;
	CalleeFID->uses++;

	// Fill in default argument values for any missing arguments
	{
		size_t numFormalArgs = CalleeFID->userArguments.size();
		for (size_t di = args.size(); di < numFormalArgs; di++) {
			ASTNode* defNode = CalleeFID->userArguments[di].defaultNode;
			const std::vector<tokenPair*>& rawToks = CalleeFID->userArguments[di].defaultRawTokens;
			if (!defNode) {
				return messageSystem::error("Missing argument with no default value");
			}
			Value* defVal = nullptr;
			if (!rawToks.empty()) {
				// Re-parse with call-site file/line info for #filepath/#linenum
				std::vector<tokenPair*> cloned;
				for (int ti = 0; ti < (int)rawToks.size(); ti++) {
					tokenPair* c = new tokenPair(*rawToks[ti]);
					if (rawToks[ti]->second == Hash && ti + 1 < (int)rawToks.size()) {
						const std::string& next = rawToks[ti + 1]->first;
						if (next == "filepath") {
							c->filePath = token->filePath;
							c->lineNumber = token->lineNumber;
						}
						else if (next == "linenum") {
							c->lineNumber = token->lineNumber;
						}
					}
					cloned.push_back(c);
				}
				ASTNode* tempNode = new ASTNode();
				generateAST(cloned, 0, tempNode);
				tempNode->nodeType = Expression_Term;
				tempNode->codegen = &ASTNode::generateExpression;
				resolveCompileTimeDirectives(tempNode);
				tempNode->parentNode = parentNode;
				defVal = (Value*)(tempNode->*(tempNode->codegen))(pass);
			}
			else {
				defNode->parentNode = parentNode;
				defVal = (Value*)(defNode->*(defNode->codegen))(pass);
			}
			if (wasError)
				return nullptr;
			cachedArgVals.push_back(defVal);
			ArgsV.push_back(defVal);
			std::string typeStr = getStringTypeFromLLVMType(defVal->getType());
			uint8_t pL = 0;
			while (!typeStr.empty() && typeStr[0] == '*') {
				pL++;
				typeStr = typeStr.substr(1);
			}
			argList.push_back(argType(typeStr, getASTNodeTypeFromString(typeStr), pL));
		}
	}

	bool isStructReturn = CalleeFID->isStructReturn;
	AllocaInst* sretAlloc = nullptr;
	if (isStructReturn) {
		// Get the struct type from definitions
		structType* retStruct = structDefinitions[CalleeFID->returnType];
		if (retStruct->structVal == nullptr) {
			return messageSystem::error("Struct return type not fully defined");
		}

		// Allocate space for the returned struct on the caller's stack
		sretAlloc = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(), retStruct->structVal, "sret_alloc");

		// Insert the sret pointer as the FIRST argument in ArgsV
		ArgsV.insert(ArgsV.begin(), sretAlloc);

		// For argList matching: Temporarily add sret to argList for validation
		// (This matches how it's stored in functionID)
		argType sretArg("*" + CalleeFID->returnType, Struct_Type, 1, false, true);
		argList.insert(argList.begin(), sretArg);
	}

	// Validate argument count (now including sret if applicable)
	// For coerced functions, the LLVM arg count is larger than the user arg count
	if (CalleeFID->variableNumArguments == false) {
		size_t expectedIRArgCount = ArgsV.size();
		for (auto& ua : CalleeFID->userArguments)
			if (ua.externCoercionCount > 0)
				expectedIRArgCount += ua.externCoercionCount - 1;
		if (CalleeF->arg_size() != expectedIRArgCount) {  // Use ArgsV.size() which includes sret
			console::indentation = errorDepth;
			if (errorDepth < maxErrorTraceDepth)
				printTokenError(getASTTokenRange(this), "Incorrect number of arguments passed to function (expected " + std::to_string(CalleeF->arg_size()) + ")", __LINE__);
			if (errorDepth < maxErrorTraceDepth)
				CalleeFID->print();
			wasError = true;
			errorDepth++;
			return nullptr;	 // TODO
			return messageSystem::error("Incorrect number of arguments passed to function (expected " + std::to_string(CalleeF->arg_size()) + ")");
		}
	}
	else if (CalleeF->arg_size() > ArgsV.size()) {
		console::indentation = errorDepth;
		if (errorDepth < maxErrorTraceDepth)
			printTokenError(getASTTokenRange(this), "Incorrect number of arguments passed to function", __LINE__);
		if (errorDepth < maxErrorTraceDepth)
			CalleeFID->print();
		wasError = true;
		errorDepth++;
		return nullptr;	 // TODO
		return messageSystem::error("Incorrect number of arguments passed to function (expected " + std::to_string(CalleeF->arg_size()) + ")");
	}

	int formalArgCount = (int)CalleeFID->userArguments.size();
	ArgsV.clear();
	int irArgIdx = isStructReturn ? 1 : 0;		  // tracks position in the LLVM function's param list
	for (int i = 0; i < (int)args.size(); i++) {  // Start from caller's args (sret is already handled)
		int formalArgIdx = i + (isStructReturn ? 1 : 0);
		int coerce = (i < (int)CalleeFID->userArguments.size()) ? (int)CalleeFID->userArguments[i].externCoercionCount : 0;
		bool coerceIsFloat = (i < (int)CalleeFID->userArguments.size()) && CalleeFID->userArguments[i].externCoercionIsFloat;
		// For variadic extra args (beyond declared params), just pass the value through
		if (CalleeFID->variableNumArguments && i >= formalArgCount) {
			ArgsV.push_back(cachedArgVals[i]);
			if (!ArgsV.back()) {
				messageSystem::endBlock();
				return nullptr;
			}
			irArgIdx++;
			continue;
		}
		bool isRef = CalleeFID->arguments[formalArgIdx].isReference;
		Value* argVal = nullptr;
		if (isRef) {
			messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__);

			if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
				return messageSystem::error("Cannot pass value as reference");
			}
			// Passing a value that requires an implicit cast to a ref parameter is not allowed:
			// the cast would produce a temporary, and a reference to a temporary is meaningless.
			{
				Value* actualVal = cachedArgVals[i];
				const argType& fa = CalleeFID->arguments[formalArgIdx];
				bool wasDef = true;
				Type* formalType = getLLVMTypeFromString(fa.typeString, 0, token, wasDef, pass);
				if (formalType && actualVal && !actualVal->getType()->isPointerTy() && !formalType->isPointerTy() && actualVal->getType() != formalType) {
					return messageSystem::error("Cannot pass '" + getStringTypeFromLLVMType(actualVal->getType()) + "' as 'ref " + fa.typeString + "': implicit cast to reference is not allowed");
				}
			}
			args[i]->childNodes[0]->isRef = true;
			argVal = (Value*)(args[i]->*(args[i]->codegen))(pass);
			if (wasError) {
				messageSystem::endBlock();
				return nullptr;
			}

			messageSystem::endBlock();
		}
		else {
			// Reuse the Value already generated in the first pass to avoid double side-effects
			argVal = cachedArgVals[i];
		}
		// Implicit string <-> *char conversions at call sites
		const argType& formal = CalleeFID->arguments[formalArgIdx];
		if (argVal && argVal->getType()->isStructTy() &&
			formal.pointerLevel == 1 &&
			(formal.typeString == "char" || formal.typeString == "int8")) {
			// string -> *char: extract .address (element 0)
			argVal = Builder->CreateExtractValue(argVal, {0}, "str_addr");
		}
		else if (argVal && argVal->getType()->isPointerTy() && !isRef &&
				 formal.pointerLevel == 0 && formal.typeString == "string" &&
				 structDefinitions.count("string") && structDefinitions["string"]->structVal) {
			// *char -> string: build string struct with strlen
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
		// ABI coercion: split struct into doubles (SSE/XMM) or i64s (INTEGER) per x86-64 SysV ABI
		if (coerce > 0 && argVal && argVal->getType()->isStructTy()) {
			AllocaInst* tmp = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(), argVal->getType(), "coerce");
			Builder->CreateStore(argVal, tmp);
			Type* primTy = coerceIsFloat ? (Type*)Type::getDoubleTy(*TheContext) : (Type*)Type::getInt64Ty(*TheContext);
			for (int k = 0; k < coerce; k++) {
				Value* bytePtr = Builder->CreateConstGEP1_64(Type::getInt8Ty(*TheContext), tmp, (uint64_t)(k * 8), "coerce_gep");
				ArgsV.push_back(Builder->CreateLoad(primTy, bytePtr, "coerce_val"));
			}
			irArgIdx += coerce;
			continue;
		}
		// Pack @packed struct to integer when formal param expects an integer (extern ABI)
		if (argVal && !isRef && argVal->getType()->isStructTy()) {
			Type* formalLLVMType = nullptr;
			if (irArgIdx < (int)CalleeF->getFunctionType()->getNumParams())
				formalLLVMType = CalleeF->getFunctionType()->getParamType(irArgIdx);
			if (formalLLVMType && formalLLVMType->isIntegerTy()) {
				AllocaInst* tmp = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(), argVal->getType(), "packed_tmp");
				Builder->CreateStore(argVal, tmp);
				Value* intPtr = Builder->CreateBitCast(tmp, formalLLVMType->getPointerTo());
				argVal = Builder->CreateLoad(formalLLVMType, intPtr, "packed_int");
			}
			// Large struct passed to pointer param: copy to stack and pass pointer (byval ABI)
			else if (formalLLVMType && formalLLVMType->isPointerTy()) {
				uint64_t size = TheModule->getDataLayout().getTypeAllocSize(argVal->getType());
				if (size > 16) {
					AllocaInst* copy = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(), argVal->getType(), "byval_copy");
					Builder->CreateStore(argVal, copy);
					argVal = copy;
				}
			}
		}
		// Implicit numeric coercion: cast arg to formal param type if they differ
		if (argVal && !isRef) {
			Type* formalLLVMType = nullptr;
			if (irArgIdx < (int)CalleeF->getFunctionType()->getNumParams())
				formalLLVMType = CalleeF->getFunctionType()->getParamType(irArgIdx);
			if (formalLLVMType && argVal->getType() != formalLLVMType &&
				!argVal->getType()->isStructTy() && !argVal->getType()->isPointerTy() &&
				!formalLLVMType->isStructTy() && !formalLLVMType->isPointerTy()) {
				// Use the actual source type's signedness (not hardcoded true)
				const std::string& srcTs = (formalArgIdx < (int)argList.size()) ? argList[formalArgIdx].typeString : "";
				bool isSrcSig = srcTs.empty() ? true : (typeSigns.count(srcTs) ? typeSigns[srcTs] : true);
				argVal = castValue(argVal, formalLLVMType, isSrcSig, false, token);
				if (wasError) {
					messageSystem::endBlock();
					return nullptr;
				}
			}
		}
		ArgsV.push_back(argVal);
		if (!ArgsV.back()) {
			messageSystem::endBlock();
			return nullptr;
		}
		irArgIdx++;
	}
	// Append evaluated default argument values for omitted trailing params
	for (int i = (int)args.size(); i < (int)cachedArgVals.size(); i++) {
		int coerce = (i < (int)CalleeFID->userArguments.size()) ? (int)CalleeFID->userArguments[i].externCoercionCount : 0;
		bool coerceIsFloat = (i < (int)CalleeFID->userArguments.size()) && CalleeFID->userArguments[i].externCoercionIsFloat;
		Value* defVal = cachedArgVals[i];
		if (coerce > 0 && defVal && defVal->getType()->isStructTy()) {
			AllocaInst* tmp = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(), defVal->getType(), "coerce_def");
			Builder->CreateStore(defVal, tmp);
			Type* primTy = coerceIsFloat ? (Type*)Type::getDoubleTy(*TheContext) : (Type*)Type::getInt64Ty(*TheContext);
			for (int k = 0; k < coerce; k++) {
				Value* bytePtr = Builder->CreateConstGEP1_64(Type::getInt8Ty(*TheContext), tmp, (uint64_t)(k * 8), "coerce_gep");
				ArgsV.push_back(Builder->CreateLoad(primTy, bytePtr, "coerce_val"));
			}
		}
		else {
			ArgsV.push_back(defVal);
		}
		if (!ArgsV.back()) {
			messageSystem::endBlock();
			return nullptr;
		}
	}

	// If struct return, re-insert sret as first arg (after rebuilding)
	if (isStructReturn) {
		ArgsV.insert(ArgsV.begin(), sretAlloc);
	}

	Value* callResult = nullptr;
	Type* retType = CalleeF->getReturnType();

	// Create the call
	CallInst* callInst = nullptr;
	if (retType->isVoidTy())
		callInst = Builder->CreateCall(CalleeF, ArgsV);
	else {
		callInst = Builder->CreateCall(CalleeF, ArgsV, "calltmp");
		callResult = callInst;
	}
	// Propagate byval attributes from the function declaration to this call instruction
	if (callInst) {
		for (unsigned pi = 0; pi < CalleeF->getFunctionType()->getNumParams(); pi++) {
			Type* bvType = CalleeF->getParamByValType(pi);
			if (bvType)
				callInst->addParamAttr(pi, Attribute::getWithByValType(*TheContext, bvType));
		}
	}

	messageSystem::endBlock();
	// For struct returns, return the loaded struct value (or pointer if lvalue)
	if (isStructReturn) {
		if (!asaType)
			asaType = new ASAType(nullptr);
		asaType->strVal = CalleeFID->returnType;
		if (lvalue) {
			return sretAlloc;  // Return pointer for lvalue contexts (e.g., assignment)
		}
		else {
			structType* retStruct = structDefinitions[CalleeFID->returnType];
			asaType->baseLLVMType = retStruct->structVal;
			return Builder->CreateLoad(retStruct->structVal, sretAlloc, "sret_load");
		}
	}

	// Unpack integer result back to @packed struct if the declared return type is a packed struct
	if (callResult && callResult->getType()->isIntegerTy() && !CalleeFID->returnType.empty()) {
		auto it = structDefinitions.find(CalleeFID->returnType);
		if (it != structDefinitions.end() && it->second->isPacked && it->second->structVal) {
			AllocaInst* tmp = CreateEntryBlockAlloca(Builder->GetInsertBlock()->getParent(), it->second->structVal, "packed_ret");
			Value* intPtr = Builder->CreateBitCast(tmp, callResult->getType()->getPointerTo());
			Builder->CreateStore(callResult, intPtr);
			callResult = Builder->CreateLoad(it->second->structVal, tmp, "unpacked_ret");
		}
	}

	// Record return type string in asaType so callers can do correct type inference.
	if (!CalleeFID->returnType.empty()) {
		if (!asaType)
			asaType = new ASAType(callResult ? callResult->getType() : nullptr);
		asaType->strVal = CalleeFID->returnType;
	}

	// Non-struct: Return the call result directly
	return callResult;
}


// Value*
void* ASTNode::generateIf(int pass)
{
	messageSystem::startBlock(this, "Generating if statement", __func__, __LINE__, __FILE__);

	if (!LexicalBlocks.empty() && token && token->filePath)
		Builder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));
	ASTNode* condExpr = childNodes[0];
	if (condExpr->childNodes.size() == 0) {
		messageSystem::startBlock(condExpr, "Generating condition", __func__, __LINE__, __FILE__);
		return messageSystem::error("Expected condition expression");
	}
	condExpr = condExpr->childNodes[0];

	if (condExpr->codegen == nullptr) {
		messageSystem::startBlock(condExpr, "Generating condition", __func__, __LINE__, __FILE__);
		return messageSystem::error("Node `" + ASTNodeTypeAsString(condExpr->nodeType) + "` does not have a code generator");
	}

	Value* CondV = (Value*)(condExpr->*(condExpr->codegen))(pass);
	if (wasError) {
		messageSystem::endBlock();
		return nullptr;
	}
	if (!CondV) {
		messageSystem::endBlock();
		return nullptr;
	}

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
		return messageSystem::error("Node `" + ASTNodeTypeAsString(scopeBody->nodeType) + "` does not have a code generator");
	}

	Value* ThenV = (Value*)(scopeBody->*(scopeBody->codegen))(pass);
	if (wasError) {
		messageSystem::endBlock();
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
		return messageSystem::error("Node `" + ASTNodeTypeAsString(elseBody->nodeType) + "` does not have a code generator");
	}

	Value* ElseV = (Value*)(elseBody->*(elseBody->codegen))(pass);
	if (wasError) {
		messageSystem::endBlock();
		return nullptr;
	}

	Builder->CreateBr(MergeBB);
	// codegen of 'Else' can change the current block, update ElseBB for the PHI.
	ElseBB = Builder->GetInsertBlock();


	// Emit merge block.
	TheFunction->insert(TheFunction->end(), MergeBB);
	Builder->SetInsertPoint(MergeBB);

	messageSystem::endBlock();
	return nullptr;
}

// Value*
void* ASTNode::generateStruct(int pass)
{
	messageSystem::startBlock(this, "Generating struct definition", __func__, __LINE__, __FILE__);

	std::string structName = token->first;

	// Do not create a struct with the same name
	if (structDefinitions.find(structName) != structDefinitions.end()) {
		if (structDefinitions[structName]->token != token) {
			return messageSystem::error("Struct cannot be redefined");
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
					currentStructName.pop();
					messageSystem::startBlock(fieldNode, "Generating struct member", __func__, __LINE__, __FILE__);
					return messageSystem::error("Member declaration must have type");
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
					messageSystem::endBlock();
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

		// Generate default constructor body if still empty (not replaced by user)
		{
			functionID* ctorFID = nullptr;
			for (auto& fid : functionIDs)
				if (fid->name == structName && fid->userArguments.empty() && fid->isStructReturn) {
					ctorFID = fid;
					break;
				}
			if (ctorFID && ctorFID->fnValue && ctorFID->fnValue->empty()) {
				Function* fn = ctorFID->fnValue;
				BasicBlock* entryBlock = BasicBlock::Create(*TheContext, "entry", fn);
				Builder->SetInsertPoint(entryBlock);
				Value* sretPtr = fn->getArg(0);
				StructType* sTy = (StructType*)structDefinitions[structName]->structVal;
				Value* szVal = ConstantInt::get(Type::getInt64Ty(*TheContext), TheModule->getDataLayout().getTypeAllocSize(sTy));
				Function* memsetFn = Intrinsic::getDeclaration(TheModule.get(), Intrinsic::memset, {sretPtr->getType(), Type::getInt64Ty(*TheContext)});
				Builder->CreateCall(memsetFn, {sretPtr, ConstantInt::get(Type::getInt8Ty(*TheContext), 0), szVal, ConstantInt::get(Type::getInt1Ty(*TheContext), 0)});
				for (auto& [memberName, defaultNode] : memberDefaultNodes) {
					auto idxIt = memberNameIndexes.find(memberName);
					if (idxIt == memberNameIndexes.end())
						continue;
					uint16_t idx = idxIt->second;
					Value* memberPtr = Builder->CreateStructGEP(sTy, sretPtr, idx, memberName + "_init");
					Value* defaultVal = (Value*)(defaultNode->*(defaultNode->codegen))(pass);
					if (wasError) {
						messageSystem::endBlock();
						return nullptr;
					}
					if (!defaultVal)
						continue;
					bool isSigned = typeSigns.count(members[idx].typeString) ? typeSigns[members[idx].typeString] : false;
					defaultVal = castValue(defaultVal, sTy->getElementType(idx), true, isSigned, token);
					if (wasError) {
						messageSystem::endBlock();
						return nullptr;
					}
					Builder->CreateStore(defaultVal, memberPtr);
				}
				Builder->CreateRetVoid();
			}
		}
		messageSystem::endBlock();
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

		// Auto-generate a default constructor if no constructor for this struct exists yet
		auto addDefaultCtorProto = [&](StructType* ty) {
			argumentList emptyUserArgs;
			if (!getExactFunctionFromID(functionIDs, const_cast<std::string&>(structName), emptyUserArgs, token)) {
				std::vector<Type*> ctorArgTypes = {ty->getPointerTo()};
				FunctionType* FT = FunctionType::get(Type::getVoidTy(*TheContext), ctorArgTypes, false);
				Function* fn = Function::Create(FT, Function::ExternalLinkage, structName, TheModule.get());
				fn->addFnAttr(llvm::Attribute::AlwaysInline);
				fn->getArg(0)->setName("sret");
				argumentList llvmArgs = {argType("*" + structName, Struct_Type, 1, false, true)};
				functionIDs.push_back(new functionID(structName, token, structName, structName, llvmArgs, emptyUserArgs, fn, false, false, true));
			}
		};
		addDefaultCtorProto(existingTy);
		messageSystem::endBlock();
		return existingTy;
	}

	StructType* structTy = StructType::create(*TheContext, fieldTypes, "struct." + structName);

	currentStructName.pop();
	auto newStructDef = new structType(structName, token, structTy, members, memberFunctions, memberNameIndexes);
	newStructDef->memberDefaultNodes = memberDefaultNodes;
	structDefinitions[structName] = newStructDef;

	// Detect @packed / @notpacked attributes for extern call ABI override
	for (auto* attr : attributes) {
		if (attr->token && attr->token->first == "packed")
			newStructDef->isPacked = true;
		if (attr->token && attr->token->first == "notpacked")
			newStructDef->isNotPacked = true;
	}

	// Auto-generate a default constructor if no constructor for this struct exists yet
	auto addDefaultCtorProto = [&](StructType* ty) {
		argumentList emptyUserArgs;
		if (!getExactFunctionFromID(functionIDs, const_cast<std::string&>(structName), emptyUserArgs, token)) {
			std::vector<Type*> ctorArgTypes = {ty->getPointerTo()};
			FunctionType* FT = FunctionType::get(Type::getVoidTy(*TheContext), ctorArgTypes, false);
			Function* fn = Function::Create(FT, Function::ExternalLinkage, structName, TheModule.get());
			fn->addFnAttr(llvm::Attribute::AlwaysInline);
			fn->getArg(0)->setName("sret");
			argumentList llvmArgs = {argType("*" + structName, Struct_Type, 1, false, true)};
			functionIDs.push_back(new functionID(structName, token, structName, structName, llvmArgs, emptyUserArgs, fn, false, false, true));
		}
	};
	addDefaultCtorProto(structTy);
	messageSystem::endBlock();
	return structTy;
}

// Value*
void* ASTNode::generateBreak(int pass)
{
	messageSystem::startBlock(this, "Generating break statement", __func__, __LINE__, __FILE__);

	// Check if we have a label
	std::string targetLabel = "";
	if (childNodes.size() > 0 && childNodes[0]->nodeType == Identifier_Node) {
		targetLabel = childNodes[0]->token->first;
	}

	// Make sure we're inside a loop
	if (loopContextStack.empty()) {
		return messageSystem::error("Break statement must be inside a loop");
	}

	// If no label, break from the innermost loop
	if (targetLabel.empty()) {
		BasicBlock* breakBB = loopContextStack.top().breakBB;
		Builder->CreateBr(breakBB);

		// Create a new unreachable block for any code after the break
		Function* TheFunction = Builder->GetInsertBlock()->getParent();
		BasicBlock* afterBreak = BasicBlock::Create(*TheContext, "after_break", TheFunction);
		Builder->SetInsertPoint(afterBreak);

		messageSystem::endBlock();
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
		return messageSystem::error("Break label \"" + targetLabel + "\" not found in enclosing loops");
	}

	Builder->CreateBr(targetBreakBB);

	// Create a new unreachable block for any code after the break
	Function* TheFunction = Builder->GetInsertBlock()->getParent();
	BasicBlock* afterBreak = BasicBlock::Create(*TheContext, "after_break", TheFunction);
	Builder->SetInsertPoint(afterBreak);

	messageSystem::endBlock();
	return nullptr;
}

// Value*
void* ASTNode::generateContinue(int pass)
{
	messageSystem::startBlock(this, "Generating continue statement", __func__, __LINE__, __FILE__);

	// Check if we have a label
	std::string targetLabel = "";
	if (childNodes.size() > 0 && childNodes[0]->nodeType == Identifier_Node) {
		targetLabel = childNodes[0]->token->first;
	}

	// Make sure we're inside a loop
	if (loopContextStack.empty()) {
		return messageSystem::error("Continue statement must be inside a loop");
	}

	// If no label, continue to the innermost loop
	if (targetLabel.empty()) {
		BasicBlock* continueBB = loopContextStack.top().continueBB;
		Builder->CreateBr(continueBB);

		// Create a new unreachable block for any code after the continue
		Function* TheFunction = Builder->GetInsertBlock()->getParent();
		BasicBlock* afterContinue = BasicBlock::Create(*TheContext, "after_continue", TheFunction);
		Builder->SetInsertPoint(afterContinue);

		messageSystem::endBlock();
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
		return messageSystem::error("Continue label \"" + targetLabel + "\" not found in enclosing loops");
	}

	Builder->CreateBr(targetContinueBB);

	// Create a new unreachable block for any code after the continue
	Function* TheFunction = Builder->GetInsertBlock()->getParent();
	BasicBlock* afterContinue = BasicBlock::Create(*TheContext, "after_continue", TheFunction);
	Builder->SetInsertPoint(afterContinue);

	messageSystem::endBlock();
	return nullptr;
}

// Add a generator for Labeled_Loop:
// Value*
void* ASTNode::generateLabeledLoop(int pass)
{
	messageSystem::startBlock(this, "Generating labeled loop", __func__, __LINE__, __FILE__);

	std::string label = token->first;

	// Find the loop
	if (childNodes.size() == 0) {
		return messageSystem::error("Labeled loop is empty");
	}

	ASTNode* loopNode = childNodes[0];

	// Verify it's actually a loop
	if (loopNode->nodeType != For_Statement_Node && loopNode->nodeType != While_Statement_Node) {
		return messageSystem::error("Label can only be applied to for or while loops");
	}

	// Store the label in the loop node and generate it
	loopNode->label = label;
	void* generatedLoop = (loopNode->*(loopNode->codegen))(pass);

	messageSystem::endBlock();
	return generatedLoop;
}

// Now update the generateFor function to use the label:
// Value*
void* ASTNode::generateFor(int pass)
{
	messageSystem::startBlock(this, "Generating for loop", __func__, __LINE__, __FILE__);

	if (!LexicalBlocks.empty() && token && token->filePath)
		Builder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));
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
	ASTNode* rangeEnd = childNodes[1]->childNodes[1];

	// Compute start value.
	if (rangeStart->codegen == nullptr) {
		return messageSystem::error("Node `" + ASTNodeTypeAsString(rangeStart->nodeType) + "` does not have a code generator");
	}
	Value* StartVal = (Value*)(rangeStart->*(rangeStart->codegen))(pass);
	if (wasError) {
		messageSystem::endBlock();
		return nullptr;
	}
	if (!StartVal) {
		messageSystem::endBlock();
		return nullptr;
	}

	// Compute end value in the preheader so we can determine the iterator type.
	if (rangeEnd->codegen == nullptr) {
		return messageSystem::error("Node `" + ASTNodeTypeAsString(rangeEnd->nodeType) + "` does not have a code generator");
	}
	Value* EndVal = (Value*)(rangeEnd->*(rangeEnd->codegen))(pass);
	if (wasError) {
		messageSystem::endBlock();
		return nullptr;
	}
	if (!EndVal) {
		messageSystem::endBlock();
		return nullptr;
	}

	// Determine iterator type: widest integer type among start and end, minimum i32.
	unsigned iterBits = 32;
	if (StartVal->getType()->isIntegerTy())
		iterBits = std::max(iterBits, StartVal->getType()->getIntegerBitWidth());
	if (EndVal->getType()->isIntegerTy())
		iterBits = std::max(iterBits, EndVal->getType()->getIntegerBitWidth());
	Type* iterType = Type::getIntNTy(*TheContext, iterBits);

	// Cast start and end to the iterator type if needed.
	if (StartVal->getType() != iterType) {
		std::string tyStr = getStringTypeFromLLVMType(StartVal->getType());
		StartVal = Builder->CreateIntCast(StartVal, iterType, typeSigns.count(tyStr) ? typeSigns[tyStr] : true, "startcast");
	}
	if (EndVal->getType() != iterType) {
		std::string tyStr = getStringTypeFromLLVMType(EndVal->getType());
		EndVal = Builder->CreateIntCast(EndVal, iterType, typeSigns.count(tyStr) ? typeSigns[tyStr] : true, "endcast");
	}

	// Make the new basic block for the loop header, inserting after current block.
	Function* TheFunction = Builder->GetInsertBlock()->getParent();
	BasicBlock* PreheaderBB = Builder->GetInsertBlock();
	BasicBlock* LoopCondBB = BasicBlock::Create(*TheContext, "loopcond", TheFunction);
	BasicBlock* LoopBB = BasicBlock::Create(*TheContext, "loop", TheFunction);
	BasicBlock* StepBB = BasicBlock::Create(*TheContext, "loopstep", TheFunction);
	BasicBlock* AfterBB = BasicBlock::Create(*TheContext, "afterloop", TheFunction);

	AllocaInst* Alloca = CreateEntryBlockAlloca(TheFunction, iterType, varName);
	// Store the value into the alloca.
	Builder->CreateStore(StartVal, Alloca);

	// Branch to loop condition check
	Builder->CreateBr(LoopCondBB);

	Builder->SetInsertPoint(LoopCondBB);

	Value* CurVar = Builder->CreateLoad(iterType, Alloca, varName.c_str());

	// Compare: exclusive (i < N)
	Value* Cond = Builder->CreateICmpSLT(CurVar, EndVal, "loopcond");

	// Conditional branch
	Builder->CreateCondBr(Cond, LoopBB, AfterBB);

	// Start insertion in LoopBB.
	Builder->SetInsertPoint(LoopBB);

	// Push loop context for break/continue support
	LoopContext ctx;
	ctx.continueBB = StepBB;  // Continue goes to step (increment) before condition check
	ctx.breakBB = AfterBB;	  // Break goes to after the loop
	ctx.label = label;		  // Empty for unlabeled loops
	loopContextStack.push(ctx);

	namedValues[varName] = new valueType(varName, getStringTypeFromLLVMType(iterType), Alloca);

	// Emit the body of the loop
	ASTNode* scopeBody = childNodes[2];

	if (scopeBody->codegen == nullptr) {
		return messageSystem::error("Node `" + ASTNodeTypeAsString(scopeBody->nodeType) + "` does not have a code generator");
	}
	(scopeBody->*(scopeBody->codegen))(pass);
	if (wasError) {
		loopContextStack.pop();	 // Clean up context
		messageSystem::endBlock();
		return nullptr;
	}

	// Pop loop context
	loopContextStack.pop();

	// Fall through from body to step block
	Builder->CreateBr(StepBB);

	// Step block: increment iterator then jump back to condition
	Builder->SetInsertPoint(StepBB);
	Value* StepVal = ConstantInt::get(*TheContext, APInt(iterBits, 1));
	Value* CurVar2 = Builder->CreateLoad(Alloca->getAllocatedType(), Alloca, varName.c_str());
	Value* NextVar = Builder->CreateAdd(CurVar2, StepVal, "nextvar");
	Builder->CreateStore(NextVar, Alloca);
	Builder->CreateBr(LoopCondBB);

	// After loop
	Builder->SetInsertPoint(AfterBB);

	messageSystem::endBlock();
	return nullptr;
}

// Similarly, if you have a while loop generator, update it too:
// Value*
void* ASTNode::generateWhile(int pass)
{
	messageSystem::startBlock(this, "Generating while loop", __func__, __LINE__, __FILE__);

	if (!LexicalBlocks.empty() && token)
		Builder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));

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
		messageSystem::startBlock(condExpr, "Generating condition expression", __func__, __LINE__, __FILE__);
		return messageSystem::error("Expected condition expression");
	}
	condExpr = condExpr->childNodes[0];

	if (condExpr->codegen == nullptr) {
		return messageSystem::error("Node `" + ASTNodeTypeAsString(condExpr->nodeType) + "` does not have a code generator");
	}

	Value* CondV = (Value*)(condExpr->*(condExpr->codegen))(pass);
	if (wasError) {
		messageSystem::endBlock();
		return nullptr;
	}
	if (!CondV) {
		messageSystem::endBlock();
		return nullptr;
	}

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
		return messageSystem::error("Node `" + ASTNodeTypeAsString(scopeBody->nodeType) + "` does not have a code generator");
	}

	(scopeBody->*(scopeBody->codegen))(pass);
	if (wasError) {
		loopContextStack.pop();
		messageSystem::endBlock();
		return nullptr;
	}

	// Pop loop context
	loopContextStack.pop();

	// Jump back to condition
	Builder->CreateBr(LoopCondBB);

	// After loop
	Builder->SetInsertPoint(AfterBB);

	messageSystem::endBlock();
	return nullptr;
}

// Function*
void* ASTNode::generatePrototype(int pass)
{
	messageSystem::startBlock(this, "Generating prototype", __func__, __LINE__, __FILE__);

	argumentList argList = argumentList();
	std::vector<Type*> argTypes = std::vector<Type*>();
	std::vector<int> byvalParamIndices;
	std::vector<Type*> byvalParamTypes;
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
			// For extern @packed structs, return as integer (no sret)
			if (isExtern) {
				auto it = structDefinitions.find(rTypeString);
				if (it != structDefinitions.end() && it->second->isPacked) {
					unsigned bits = TheModule->getDataLayout().getTypeAllocSizeInBits(retType);
					retType = Type::getIntNTy(*TheContext, bits);
				}
				else {
					isStructReturn = true;
				}
			}
			else {
				isStructReturn = true;
			}
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
			return messageSystem::error("Invalid argument type given");
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
				ASTNode* colonNode = a->childNodes[0];
				ASTNode* nameNode = colonNode->childNodes[0];
				ASTNode* typeNode = colonNode->childNodes[1];
				// nameNode must be a plain identifier; anything else (e.g. 'ref name : type'
				// instead of 'name : ref type') means the parameter syntax is malformed.
				if (nameNode->nodeType != Identifier_Node) {
					// Prefer the name node's token, then the colon node's, then fall back to
					// the function name token (this->token) which always has source location.
					messageSystem::startBlock(nameNode, "Generating argument name", __func__, __LINE__, __FILE__);
					return messageSystem::error("Parameter name must be a plain singular identifier");
				}
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
				if (a->childNodes.size() > 1) {
					arg.hasDefault = true;
					arg.defaultNode = a->childNodes[1];
					arg.defaultRawTokens = a->childNodes[1]->defaultRawTokens;
				}
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
				// Extern struct ABI: auto-detect x86-64 SysV passing convention.
				// @packed overrides to force integer packing; @notpacked forces auto-detect.
				if (isExtern && aType && aType->isStructTy() && pointerLevel == 0 && !isReference) {
					auto it = structDefinitions.find(typeStr);
					if (it != structDefinitions.end()) {
						bool forcePackedInt = it->second->isPacked && !it->second->isNotPacked;
						uint64_t size = TheModule->getDataLayout().getTypeAllocSize(aType);
						if (forcePackedInt) {
							// @packed: pack entire struct into a single integer
							unsigned bits = TheModule->getDataLayout().getTypeAllocSizeInBits(aType);
							aType = Type::getIntNTy(*TheContext, bits);
						}
						else if (size > 16) {
							// MEMORY class: pass via pointer (byval)
							byvalParamIndices.push_back((int)argTypes.size());
							byvalParamTypes.push_back(aType);
							aType = PointerType::getUnqual(*TheContext);
						}
						else if (size > 0) {
							// Classify per eightbyte: all float -> SSE (doubles), any int -> INTEGER (i64s)
							StructType* st = cast<StructType>(aType);
							bool allFloat = true;
							for (auto* el : st->elements())
								if (!el->isFloatTy()) {
									allFloat = false;
									break;
								}
							int numChunks = (int)((size + 7) / 8);
							if (allFloat) {
								// SSE class: coerce to doubles so LLVM uses XMM registers
								argList.back().externCoercionCount = (int8_t)numChunks;
								argList.back().externCoercionIsFloat = true;
								userArgList.back().externCoercionCount = (int8_t)numChunks;
								userArgList.back().externCoercionIsFloat = true;
								for (int k = 0; k < numChunks; k++) {
									argTypes.push_back(Type::getDoubleTy(*TheContext));
									argNames.push_back(nameNode->token->first + (k == 0 ? "" : "_" + std::to_string(k)));
								}
								continue;  // skip push below
							}
							else {
								// INTEGER class: coerce to i64s per eightbyte
								argList.back().externCoercionCount = (int8_t)numChunks;
								argList.back().externCoercionIsFloat = false;
								userArgList.back().externCoercionCount = (int8_t)numChunks;
								userArgList.back().externCoercionIsFloat = false;
								for (int k = 0; k < numChunks; k++) {
									argTypes.push_back(Type::getInt64Ty(*TheContext));
									argNames.push_back(nameNode->token->first + (k == 0 ? "" : "_" + std::to_string(k)));
								}
								continue;  // skip push below
							}
						}
					}
				}
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
			messageSystem::startBlock(a->childNodes[0], "Generating argument", __func__, __LINE__, __FILE__);
			return messageSystem::error("Invalid function parameter syntax (expected 'name : type', got something else)");
		}
	}
	bool isAlwaysInline = false;
	for (auto& m : modifiersNode->childNodes) {
		if (m->token->first == "#replaceable")
			replaceableDefinition = true;
	}
	for (auto* attr : attributes) {
		if (attr->token && attr->token->first == "replaceable")
			replaceableDefinition = true;
		if (attr->token && attr->token->first == "inline")
			isAlwaysInline = true;
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
		messageSystem::endBlock();
		return theFunctionID->fnValue;
	}


	FunctionType* FT = FunctionType::get(actualRetType, argTypes, variableNumArguments);

	Function* fn = nullptr;
	// If extern declaration, dont mangle name; use externSymbolName if an alias was given
	if (isExtern) {
		std::string llvmSymbol = externSymbolName.empty() ? fnName : externSymbolName;
		fn = Function::Create(FT, Function::ExternalLinkage, llvmSymbol, TheModule.get());
	}
	else
		fn = Function::Create(FT, Function::ExternalLinkage, mangledName, TheModule.get());
	if (isAlwaysInline)
		fn->addFnAttr(llvm::Attribute::AlwaysInline);

	// Apply byval attributes to large struct params in extern declarations
	for (int bi = 0; bi < (int)byvalParamIndices.size(); bi++)
		fn->addParamAttr(byvalParamIndices[bi], Attribute::getWithByValType(*TheContext, byvalParamTypes[bi]));

	uint16_t Idx = 0;
	for (auto& arg : fn->args()) {
		arg.setName(argNames[Idx++]);

		if (argList[Idx - 1].isConstant) {
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
	functionIDs.back()->declNode = this;
	if (verbosity >= 5) {
		console::printIndent(depth + 2);
		console::WriteLine("-- Added function \"" + fnName + "\" to functionIDs");
	}

	messageSystem::endBlock();
	return fn;
}

// Function*
void* ASTNode::generateFunction(int pass)
{
	messageSystem::startBlock(this, "Generating function", __func__, __LINE__, __FILE__);

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
		return messageSystem::error("There was a failure to create a function");
	}

	if (!theFunction->empty() && replaceableDefinition == false) {
		console::indentation = errorDepth;
		if (errorDepth < maxErrorTraceDepth)
			printTokenError(getASTTokenRange(this), "Function cannot be redefined, requires unique identity");	// TODO
		theFunctionID = getFunctionIDFromFunctionPointer(functionIDs, theFunction);
		console::indentation++;
		if (errorDepth < maxErrorTraceDepth)
			printTokenMarked(tokenRange {theFunctionID->token, theFunctionID->token}, "Previously defined here:");
		console::indentation--;
		wasError = true;
		errorDepth++;
		return nullptr;
	}

	if (!theFunction->empty() && replaceableDefinition) {
		theFunction->deleteBody();
	}

	if (pass <= 1) {
		messageSystem::endBlock();
		return theFunction;
	}

	theFunctionID = getFunctionIDFromFunctionPointer(functionIDs, theFunction);
	if (!theFunctionID) {
		return messageSystem::error("There was a failure to create a function");
	}

	// Create debug info for the function
	unsigned LineNo = token->lineNumber + 1;  // DWARF is 1-indexed; tokenizer is 0-indexed
	unsigned ScopeLine = LineNo;

	DIFile* FnFile = (token->filePath && !token->filePath->empty())
						 ? getDIFile(*token->filePath)
						 : TheFile;

	// Create subroutine type
	SmallVector<Metadata*, 8> EltTys;
	DIType* DblTy = DBuilder->createBasicType("double", 64, dwarf::DW_ATE_float);
	EltTys.push_back(DblTy);  // Add return type and parameters as needed

	DISubroutineType* SubroutineType = DBuilder->createSubroutineType(
		DBuilder->getOrCreateTypeArray(EltTys));

	// Create function debug info
	DISubprogram* SP = DBuilder->createFunction(
		FnFile,							// Scope
		functionName,					// Name
		StringRef(),					// Linkage name
		FnFile,							// File
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


	// Add remaining arguments
	int i = 0;
	for (auto& arg : theFunction->args()) {
		if (i >= theFunctionID->arguments.size()) {
			theFunctionID->print();
			return messageSystem::error("Mismatch in number of arguments vs function signature: " + std::to_string(theFunctionID->arguments.size()));
		}

		// Always create an alloca for the incoming argument so we'll have addressable storage
		AllocaInst* Alloca = CreateEntryBlockAlloca(theFunction, arg.getType(), arg.getName());
		// Store the incoming argument value into the alloca
		Builder->CreateStore(&arg, Alloca);
		// For const parameters, mark the alloca as invariant after the initial store
		if (theFunctionID->arguments[i].isConstant) {
			uint64_t typeSize = TheModule->getDataLayout().getTypeAllocSize(arg.getType());
			Function* invariantStartFn = Intrinsic::getDeclaration(
				TheModule.get(), Intrinsic::invariant_start,
				{PointerType::getUnqual(*TheContext)});
			Builder->CreateCall(invariantStartFn,
				{ConstantInt::get(Type::getInt64Ty(*TheContext), typeSize), Alloca});
		}

		// Build the "actual type" string the rest of your compiler expects (pointer stars + type name)
		std::string baseType = "";
		for (int p = 0; p < theFunctionID->arguments[i].pointerLevel; p++)
			baseType += "*";

		valueType* vt = new valueType(std::string(arg.getName()), baseType + theFunctionID->arguments[i].typeString, Alloca, true, theFunctionID->arguments[i].isReference);

		vt->isConstant = theFunctionID->arguments[i].isConstant;

		namedValues[std::string(arg.getName())] = vt;

		// Emit debug info for this parameter so GDB can show argument values
		if (DBuilder && !LexicalBlocks.empty()) {
			std::string argTypeStr = baseType + theFunctionID->arguments[i].typeString;
			DIType* DebugType = createDIType(arg.getType(), argTypeStr);
			if (DebugType) {
				DILocalVariable* D = DBuilder->createParameterVariable(
					SP,
					std::string(arg.getName()),
					i + 1,	// 1-indexed
					FnFile,
					LineNo,
					DebugType,
					true  // AlwaysPreserve
				);
				DBuilder->insertDeclare(
					Alloca,
					D,
					DBuilder->createExpression(),
					DILocation::get(SP->getContext(), LineNo, 0, SP),
					Builder->GetInsertBlock());
			}
		}

		i++;
	}

	//NamedValues[std::string(Arg.getName())] = &Arg;

	ASTNode* body;
	for (auto& n : childNodes)
		if (n->nodeType == Scope_Body)
			body = n;

	if (body->codegen == nullptr) {
		return messageSystem::error("Node `" + ASTNodeTypeAsString(body->nodeType) + "` does not have a code generator");
	}

	(body->*(body->codegen))(pass);
	if (wasError) {
		messageSystem::endBlock();
		return nullptr;
	}

	// Create default return at end of function, only if the current block
	// doesn't already have a terminator (e.g. an explicit return statement).
	if (!Builder->GetInsertBlock()->getTerminator()) {
		if (theFunction->getReturnType()->isVoidTy())
			Builder->CreateRetVoid();
		else
			Builder->CreateRet(Constant::getNullValue(theFunction->getReturnType()));
	}

	// Pop this function's scope now that its body is fully generated.
	LexicalBlocks.pop_back();

	// Validate the generated code, checking for consistency.
	verifyFunction(*theFunction);

	//// Optimize the function. // This causes issues
	//if (optimizationLevel >= 1)
	//	TheFPM->run(*theFunction, *TheFAM);

	messageSystem::endBlock();
	return theFunction;
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
	messageSystem::startBlock(this, "Generating `#define` directive", __func__, __LINE__, __FILE__);

	if (childNodes.size() < 2)
		return messageSystem::error("#define requires name and value arguments");

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
	else
		return messageSystem::error("#define requires name and value arguments");

	messageSystem::endBlock();

	return nullptr;
}

// #library "name";
// Records a library to pass to the linker as -l<name>. Collected on pass 0 only
// to avoid duplicates across the three codegen passes.
void* ASTNode::generateLibraryDirective(int pass)
{
	if (pass != 1)
		return nullptr;

	messageSystem::startBlock(this, "Generating `#library` directive", __func__, __LINE__, __FILE__);

	if (childNodes.size() < 2 || childNodes[1]->childNodes.empty()) {
		return messageSystem::error("#library argument must be a string library name");
	}
	ASTNode* nameNode = childNodes[1]->childNodes[0];
	if (nameNode->nodeType != String_Node && nameNode->nodeType != String_Constant_Node) {
		return messageSystem::error("#library argument must be a string literal");
	}
	// Strip surrounding quotes from the string token
	std::string raw = nameNode->token->first;
	std::string libName = raw.substr(1, raw.size() - 2);

	messageSystem::endBlock();

	// Add if not already present
	for (auto& l : linkedLibraries)
		if (l == libName)
			return nullptr;
	linkedLibraries.push_back(libName);
	return nullptr;
}

// #library_static "path/to/lib.a";
// Records a static library path to pass verbatim to the linker. Collected on pass 1 only.
void* ASTNode::generateLibraryStaticDirective(int pass)
{
	if (pass != 1)
		return nullptr;

	messageSystem::startBlock(this, "Generating `#library_static` directive", __func__, __LINE__, __FILE__);

	if (childNodes.size() < 2 || childNodes[1]->childNodes.empty()) {
		return messageSystem::error("#library_static requires a string path to a .a file");
	}
	ASTNode* nameNode = childNodes[1]->childNodes[0];
	if (nameNode->nodeType != String_Node && nameNode->nodeType != String_Constant_Node) {
		return messageSystem::error("#library_static argument must be a string literal");
	}
	std::string raw = nameNode->token->first;
	std::string path = raw.substr(1, raw.size() - 2);

	messageSystem::endBlock();

	for (auto& l : linkedStaticLibraries)
		if (l == path)
			return nullptr;
	linkedStaticLibraries.push_back(path);
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

// #typeof(expr) - returns the ASA type name of expr as a string.
// For identifiers, looks up namedValues to avoid emitting any IR.
// For other expressions, generates the expression and reads the LLVM type.
void* ASTNode::generateTypeofDirective(int pass)
{
	if (pass == 0)
		return nullptr;

	messageSystem::startBlock(this, "Generating `#typeof` directive", __func__, __LINE__, __FILE__);

	if (childNodes.size() < 2 || childNodes[1]->childNodes.empty()) {
		messageSystem::error("#typeof requires a type or variable argument");
	}

	ASTNode* argExpr = childNodes[1]->childNodes[0];
	std::string typeStr;

	// Fast path: identifier - look up in namedValues, no IR emitted
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

	messageSystem::endBlock();
	return result;
}

// #sizeof(T) - returns the alloc size in bytes of T as an int64 constant.
// T can be a variable name, a plain type name, or a pointer-modified type (e.g. *int, * *Wall).
void* ASTNode::generateSizeofDirective(int pass)
{
	if (pass == 0)
		return nullptr;

	messageSystem::startBlock(this, "Generating `#sizeof` directive", __func__, __LINE__, __FILE__);

	if (childNodes.size() < 2 || childNodes[1]->childNodes.empty()) {
		messageSystem::error("#sizeof requires a type or variable argument");
	}

	ASTNode* argNode = childNodes[1]->childNodes[0];
	Type* llvmType = nullptr;

	// Walk a chain of Dereference_Operation / Pointer_Node nodes to count pointer
	// indirections, then resolve the base identifier as a type.
	auto resolvePointerTypeNode = [&](ASTNode* n) -> Type* {
		int stars = 0;
		while (n->nodeType == Dereference_Operation || n->nodeType == Pointer_Node) {
			stars++;
			if (n->childNodes.empty())
				return nullptr;
			n = n->childNodes[0];
		}
		if (n->nodeType != Identifier_Node)
			return nullptr;
		bool wasDefined = true;
		std::string typeName = std::string(stars, '*') + n->token->first;
		Type* t = getLLVMTypeFromString(typeName, 0, token, wasDefined, pass);
		return (wasDefined && t) ? t : nullptr;
	};

	if (argNode->nodeType == Identifier_Node) {
		std::string name = argNode->token->first;

		// Try as a variable first, using its stored type string (which encodes pointer depth)
		valueType* val = findNamedValue(parentNode, this, name, token);
		if (val) {
			bool wasDefined = true;
			llvmType = getLLVMTypeFromString(val->type, 0, token, wasDefined, pass);
			if (!wasDefined)
				llvmType = nullptr;
		}

		// Fall back to treating the identifier as a plain type name
		if (!llvmType) {
			bool wasDefined = true;
			llvmType = getLLVMTypeFromString(name, 0, token, wasDefined, pass);
			if (!wasDefined)
				llvmType = nullptr;
		}
	}
	else if (argNode->nodeType == Dereference_Operation || argNode->nodeType == Pointer_Node) {
		llvmType = resolvePointerTypeNode(argNode);
	}

	// Fallback: generate the expression and read the LLVM type
	if (!llvmType) {
		Value* val = (Value*)(argNode->*(argNode->codegen))(pass);
		if (val)
			llvmType = val->getType();
	}

	if (!llvmType) {
		messageSystem::error("#sizeof: cannot determine type of argument");
	}

	const DataLayout& DL = TheModule->getDataLayout();
	uint64_t size = DL.getTypeAllocSize(llvmType);
	Value* sizeVal = ConstantInt::get(Type::getInt64Ty(*TheContext), size);
	if (!asaType)
		asaType = new ASAType(sizeVal->getType());
	else
		asaType->baseLLVMType = sizeVal->getType();

	messageSystem::endBlock();
	return sizeVal;
}

// #compiles(expr) - returns true if expr compiles without error, false otherwise.
// Performs speculative codegen in a temporary BasicBlock, discards the result.
void* ASTNode::generateCompilesDirective(int pass)
{
	if (pass == 0)
		return nullptr;

	messageSystem::startBlock(this, "Generating `#compiles` directive", __func__, __LINE__, __FILE__);

	if (childNodes.size() < 2 || childNodes[1]->childNodes.empty()) {
		messageSystem::error("#compiles requires an expression argument");
	}

	ASTNode* argNode = childNodes[1]->childNodes[0];

	// Save current state
	BasicBlock* savedInsertBlock = Builder->GetInsertBlock();
	BasicBlock::iterator savedInsertPoint = Builder->GetInsertPoint();
	bool savedWasError = wasError;
	uint8_t savedErrorDepth = errorDepth;

	// Create a temporary function and BasicBlock for speculative codegen
	FunctionType* dummyFnType = FunctionType::get(Type::getVoidTy(*TheContext), false);
	Function* dummyFn = Function::Create(dummyFnType, Function::PrivateLinkage, "__compiles_probe__", TheModule.get());
	BasicBlock* tempBB = BasicBlock::Create(*TheContext, "probe", dummyFn);
	Builder->SetInsertPoint(tempBB);

	// Suppress errors and attempt codegen
	wasError = false;
	messageSystem::suppressErrors = true;
	if (argNode->codegen)
		(argNode->*(argNode->codegen))(pass);
	messageSystem::suppressErrors = false;

	bool compiled = !wasError;
	wasError = savedWasError;
	errorDepth = savedErrorDepth;

	// Remove the temporary function entirely
	dummyFn->eraseFromParent();

	// Restore insert point
	if (savedInsertBlock)
		Builder->SetInsertPoint(savedInsertBlock, savedInsertPoint);

	Value* result = ConstantInt::get(Type::getInt1Ty(*TheContext), compiled ? 1 : 0);
	if (!asaType)
		asaType = new ASAType(result->getType());
	else
		asaType->baseLLVMType = result->getType();

	messageSystem::endBlock();
	return result;
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
	bool doTime = (compilerFlags & Flags_Time);

	std::string libFlags = "";
	for (auto& lib : linkedLibraries)
		libFlags += " -l" + lib;
	for (auto& path : linkedStaticLibraries)
		libFlags += " " + path;
	std::string optimizationOption = "";
	if (optimizationLevel >= 0)
		optimizationOption = " -O" + std::to_string(optimizationLevel);

	if (optimizationLevel >= 1) {
		// For optimized builds, use opt + llc + clang to avoid misoptimizations
		// that occur when running LLVM passes on the in-memory module. The text
		// IR round-trip through opt produces correct results.
		std::string optIRPath = irFilePath + ".opt.ll";
		std::string sFilePath = irFilePath + ".s";

		std::string commandOpt = "opt" + optimizationOption + " " + irFilePath + " -S -o " + optIRPath;
		{
			auto t0 = std::chrono::steady_clock::now();
			int result = std::system(commandOpt.c_str());
			if (doTime) {
				double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
				fprintf(stderr, "\n[opt] real\t%.3fs\n", secs);
			}
			if (result != 0)
				exit(1);
		}
		std::string commandLLC = "llc -relocation-model=pic " + optIRPath + " -o " + sFilePath;
		{
			auto t0 = std::chrono::steady_clock::now();
			int result = std::system(commandLLC.c_str());
			if (doTime) {
				double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
				fprintf(stderr, "[llc] real\t%.3fs\n", secs);
			}
			if (result != 0)
				exit(1);
		}
		std::string commandClang = "clang -fPIE -o " + exeFilePath + " " + sFilePath + libFlags + " -Wl,-rpath,\\$ORIGIN " + clangOptions;
		{
			auto t0 = std::chrono::steady_clock::now();
			int result = std::system(commandClang.c_str());
			if (doTime) {
				double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
				fprintf(stderr, "[clang] real\t%.3fs\n", secs);
			}
			if (result != 0)
				exit(1);
		}
	}
	else {
		// Unoptimized: use llc to convert .ll to assembly, then clang to link.
		std::string commandLLC = "llc -relocation-model=pic " + irFilePath + " -o " + irFilePath + ".s";
		{
			auto t0 = std::chrono::steady_clock::now();
			int result = std::system(commandLLC.c_str());
			if (doTime) {
				double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
				fprintf(stderr, "\n[llc] real\t%.3fs\n", secs);
			}
			if (result != 0)
				exit(1);
		}
		std::string commandClang = "clang -fPIE -o " + exeFilePath + " " + irFilePath + ".s -g" + libFlags + " -Wl,-rpath,\\$ORIGIN " + clangOptions;
		{
			auto t0 = std::chrono::steady_clock::now();
			int result = std::system(commandClang.c_str());
			if (doTime) {
				double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
				fprintf(stderr, "[clang] real\t%.3fs\n", secs);
			}
			if (result != 0)
				exit(1);
		}
	}
	return 0;
}


void optimizeFunctions()
{
	if (optimizationLevel < 1)
		return;

	OptimizationLevel level;
	switch (optimizationLevel) {
		case 1:
			level = OptimizationLevel::O1;
			break;
		case 2:
			level = OptimizationLevel::O2;
			break;
		default:
			level = OptimizationLevel::O3;
			break;
	}

	LoopAnalysisManager LAM;
	FunctionAnalysisManager FAM;
	CGSCCAnalysisManager CGAM;
	ModuleAnalysisManager MAM;
	PassBuilder PB;
	PB.registerModuleAnalyses(MAM);
	PB.registerFunctionAnalyses(FAM);
	PB.registerCGSCCAnalyses(CGAM);
	PB.registerLoopAnalyses(LAM);
	PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

	ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(level);
	MPM.run(*TheModule, MAM);
}
