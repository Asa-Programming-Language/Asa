#include "codegen.h"

#include "compiler_directives.h"

bool isOptimizing()
{
    return optimizationLevel != "0" && !optimizationLevel.empty();
}

typedef Type LLVMType;
typedef Value LLVMValue;

std::unique_ptr<LLVMContext> llvmCompileContext;
std::unique_ptr<Module> llvmCompileModule;
std::unique_ptr<DIBuilder> llvmDebugBuilder;
static DICompileUnit* llvmDebugCompileUnit;
static DIFile* llvmDebugFile;
static std::unordered_map<std::string, DIFile*> llvmDebugFileCache;
static std::vector<DIScope*> LexicalBlocks;
std::unique_ptr<IRBuilder<>> llvmIRBuilder;

std::unordered_map<std::string, llvm::GlobalVariable*> globalStringLiteralConstants;
bool isCallMemberFunction = false;  // TODO: Maybe remove this

bool wasError = false;   // TODO: Remove this
uint8_t errorDepth = 0;  // TODO: Remove this

//bool suppressCodegenErrors = false;
std::unordered_map<std::string, bool> compilerDirectiveFlags;
std::unordered_map<std::string, bool> commandLineCompilerDirectiveFlags;
std::unordered_map<std::string, std::stack<ASTNode*>> compilerStacks;
std::vector<std::string> linkedLibraries;
std::vector<std::string> linkedStaticLibraries;

static ASTNode* resolveASTNodeValue(ASTNode* node, int pass)
{
    if (!node || !node->returnsASTNode || !node->resolveASTNode)
        return node;

    ASTNode* resolvedNode = (node->*(node->resolveASTNode))(pass);
    if (!resolvedNode && wasError)
        return nullptr;
    return resolvedNode ? resolvedNode : node;
}

static ASTNode* getModuleInnerScope(ASTNode* moduleCompilerDefineNode)
{
    if (!moduleCompilerDefineNode || !moduleCompilerDefineNode->isModuleScope || moduleCompilerDefineNode->childNodes.empty())
        return nullptr;

    ASTNode* outerScope = moduleCompilerDefineNode->childNodes[0];
    if (!outerScope || outerScope->nodeType != Scope_Body || outerScope->childNodes.empty())
        return nullptr;

    ASTNode* moduleDef = outerScope->childNodes[0];
    if (!moduleDef || moduleDef->nodeType != Module_Define_Node || moduleDef->childNodes.empty())
        return nullptr;

    ASTNode* innerScope = moduleDef->childNodes[0];
    return innerScope && innerScope->nodeType == Scope_Body ? innerScope : nullptr;
}

static bool getConstantTruthValue(LLVMValue* value, bool& resolved)
{
    resolved = false;
    if (!value)
        return false;

    Constant* constantValue = dyn_cast<Constant>(value);
    if (!constantValue || isa<UndefValue>(constantValue))
        return false;

    resolved = true;
    return !constantValue->isNullValue();
}

static bool evaluateCompilerDirectiveCondition(ASTNode* conditionNode, int pass, bool& resolved)
{
    resolved = false;
    if (!conditionNode || !conditionNode->codegen)
        return false;

    BasicBlock* savedInsertBlock = llvmIRBuilder->GetInsertBlock();
    BasicBlock::iterator savedInsertPoint = llvmIRBuilder->GetInsertPoint();
    bool savedWasError = wasError;
    uint8_t savedErrorDepth = errorDepth;

    FunctionType* dummyFnType = FunctionType::get(LLVMType::getVoidTy(*llvmCompileContext), false);
    Function* dummyFn = Function::Create(dummyFnType, Function::PrivateLinkage, "__if_condition_probe__", llvmCompileModule.get());
    BasicBlock* tempBB = BasicBlock::Create(*llvmCompileContext, "probe", dummyFn);
    llvmIRBuilder->SetInsertPoint(tempBB);

    wasError = false;
    LLVMValue* conditionValue = (LLVMValue*)(conditionNode->*(conditionNode->codegen))(pass);
    bool generatedError = wasError;
    bool conditionResult = getConstantTruthValue(conditionValue, resolved);

    dummyFn->eraseFromParent();

    if (savedInsertBlock)
        llvmIRBuilder->SetInsertPoint(savedInsertBlock, savedInsertPoint);
    else
        llvmIRBuilder->ClearInsertionPoint();

    if (generatedError)
        return false;

    wasError = savedWasError;
    errorDepth = savedErrorDepth;
    return conditionResult;
}

bool hasAttribute(ASTNode* node, std::string attributeName)
{
    for (auto* attr : node->attributes) {
        if (!attr->token)
            continue;
        const std::string& attrName = attr->token->tokenStr;
        if (attrName == attributeName) {
            return true;
        }
    }
    return false;
}

static ASTNode* unwrapSingleExpressionNode(ASTNode* node)
{
    while (node && node->nodeType == Expression_Term && node->childNodes.size() == 1)
        node = node->childNodes[0];
    return node;
}

static void collectCompilerDirectiveArgs(ASTNode* arg, std::vector<ASTNode*>& args)
{
    arg = unwrapSingleExpressionNode(arg);
    if (!arg)
        return;
    if (arg->nodeType == Comma_Node) {
        for (auto* child : arg->childNodes)
            collectCompilerDirectiveArgs(child, args);
        return;
    }
    args.push_back(arg);
}

static std::vector<ASTNode*> getCompilerDirectiveArgs(ASTNode* directiveNode)
{
    std::vector<ASTNode*> args;
    if (!directiveNode || directiveNode->childNodes.size() < 2)
        return args;

    ASTNode* argContainer = directiveNode->childNodes[1];
    if (!argContainer)
        return args;

    if (argContainer->nodeType == Scope_Body || argContainer->nodeType == Arguments) {
        for (auto* child : argContainer->childNodes)
            collectCompilerDirectiveArgs(child, args);
    }
    else {
        collectCompilerDirectiveArgs(argContainer, args);
    }

    return args;
}

static bool isUndefinedInitializer(ASTNode* node)
{
    node = unwrapSingleExpressionNode(node);
    return node && node->nodeType == Undefined_Initializer_Node;
}

static bool isDefaultInitializer(ASTNode* node)
{
    node = unwrapSingleExpressionNode(node);
    return node && node->nodeType == Default_Initializer_Node;
}

static LLVMValue* generateDefaultValueForType(LLVMType* type, const std::string& typeName, int pointerLevel, int pass, ASTNode* node);
static bool getDeclaredTypeFromColonNode(ASTNode* colonNode, LLVMType*& outType, std::string& outTypeName, int& outPointerLevel, bool& outIsConst, int pass);
static LLVMValue* makeStringConstant(const std::string& str);

std::string getAttributeValue(ASTNode* node, std::string attributeName)
{
    for (auto* attr : node->attributes) {
        if (!attr->token)
            continue;
        const std::string& attrName = attr->token->tokenStr;
        std::string msg;
        if (attrName == attributeName) {
            // Extract optional string argument from Scope_Body child
            std::string val = "true";
            for (auto* ac : attr->childNodes)
                if (ac->nodeType == Scope_Body && !ac->childNodes.empty() && ac->childNodes[0]->token) {
                    ASTNode* valueNode = ac->childNodes[0];
                    if (valueNode->nodeType == String_Node || valueNode->nodeType == String_Constant_Node)
                        val = decodeQuotedStringToken(valueNode->token);
                    else
                        val = valueNode->token->tokenStr;
                    break;
                }

            return val;
        }
    }
    return "false";
}

std::string getInheritedAttributeValue(ASTNode* node, std::string attributeName)
{
    while (node) {
        for (auto* attr : node->attributes) {
            if (!attr->token || attr->token->tokenStr != attributeName)
                continue;

            std::string value = "true";
            for (auto* ac : attr->childNodes) {
                if (ac->nodeType == Scope_Body && !ac->childNodes.empty() && ac->childNodes[0]->token) {
                    ASTNode* valueNode = ac->childNodes[0];
                    if (valueNode->nodeType == String_Node || valueNode->nodeType == String_Constant_Node)
                        value = decodeQuotedStringToken(valueNode->token);
                    else
                        value = valueNode->token->tokenStr;
                    break;
                }
            }
            return value;
        }
        node = node->parentNode;
    }
    return "false";
}

// Check @deprecated / @removed attributes on a symbol's declaration.
// Emits a warning for @deprecated and sets wasError+returns false for @removed.
// Returns false if the symbol is @removed (caller should bail out).
bool checkDeprecationAttributes(ASTNode* usageNode, ASTNode*& declarationNode, std::string symName)
{
    if (hasAttribute(declarationNode, "deprecated")) {
        std::string msg = "'" + symName + "' is deprecated";
        std::string detail = getAttributeValue(declarationNode, "deprecated");
        if (detail == "true" || detail == "false")
            detail = "";
        if (!detail.empty())
            msg += ": " + detail;
        messageSystem::addAttribute(declarationNode);
        messageSystem::warning(msg, messageSystem::Deprecated_Attribute_Warning);
    }
    if (hasAttribute(declarationNode, "removed")) {
        std::string msg = "'" + symName + "' has been removed";
        std::string detail = getAttributeValue(declarationNode, "removed");
        if (detail == "true" || detail == "false")
            detail = "";
        if (!detail.empty())
            msg += ": " + detail;
        messageSystem::addAttribute(declarationNode);
        messageSystem::error(msg, messageSystem::Removed_Attribute_Error);
        return false;
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
// Used by member-access codegen
// TODO: Make this probably follow regular lookup paths
std::unordered_map<std::string, ASTNode*> moduleRegistry;

// Variant function storage.
// Maps unmangled base name to its ASTNode template (has non-empty childNodes[4]).
std::unordered_map<std::string, ASTNode*> variantFunctionTemplates;
// Mangled names of already-instantiated variant functions (prevent double instantiation).
std::unordered_set<std::string> instantiatedVariantFunctions;

// Variant struct storage.
// Maps unmangled base name to its ASTNode template (has non-empty childNodes[1] Variants_Node).
std::unordered_map<std::string, ASTNode*> variantStructTemplates;
// Mangled names of already-instantiated variant structs (prevent double instantiation).
std::unordered_set<std::string> instantiatedVariantStructs;

// Returns the allocated element type for either an AllocaInst or GlobalVariable.
inline LLVMType* getValueStoredType(LLVMValue* ptr)
{
    if (auto* A = dyn_cast<AllocaInst>(ptr))
        return A->getAllocatedType();
    if (auto* G = dyn_cast<GlobalVariable>(ptr))
        return G->getValueType();
    return ptr->getType();
}


std::unordered_map<std::string, LLVMType*> unresolvedTypes;
std::stack<ASAType*> lastRetrievedElementType;
std::stack<LLVMValue*> pipeOperationValue;

// Stack to track loop contexts (for break/continue)
struct LoopContext {
    BasicBlock* continueBB;  // Block to jump to for continue
    BasicBlock* breakBB;     // Block to jump to for break
    std::string label;       // Optional label for labeled break/continue
};
std::stack<LoopContext> loopContextStack;

// Stack to track value-block contexts (for result)
struct ResultContext {
    AllocaInst* resultSlot;  // Stack slot that holds the block's result value (set by generateResult)
    BasicBlock* mergeBB;     // Block to branch to after a result statement
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

// Built-in type synonym groups (same group ID = equivalent types)
static std::unordered_map<std::string, int> typeEquivGroup = {
    {"int", 0},
    {"int32", 0},
    {"uint", 1},
    {"uint32", 1},
    {"int8", 2},
    {"char", 2},
    {"uint8", 3},
    {"uchar", 3},  // TODO: Will there be uchar?
};
static int nextTypeEquivGroup = 4;

// User-defined type aliases (e.g., from `i32 :: int32;`)
std::unordered_map<std::string, std::string> typeAliasMap;  // TODO: Keep in scope

std::string resolveTypeAlias(const std::string& name, int depth)
{
    if (depth > 100)
        return name;
    auto it = typeAliasMap.find(name);
    if (it != typeAliasMap.end())
        return resolveTypeAlias(it->second, depth + 1);
    return name;
}

void registerTypeAlias(const std::string& aliasName, const std::string& targetName)
{
    typeAliasMap[aliasName] = targetName;
    std::string resolved = resolveTypeAlias(targetName);
    auto gIt = typeEquivGroup.find(resolved);
    if (gIt != typeEquivGroup.end())
        typeEquivGroup[aliasName] = gIt->second;
    else {
        int g = nextTypeEquivGroup++;
        typeEquivGroup[aliasName] = g;
        typeEquivGroup[resolved] = g;
    }
    auto sIt = typeSigns.find(resolved);
    if (sIt != typeSigns.end())
        typeSigns[aliasName] = sIt->second;
}

bool areTypesEquivalent(const std::string& type1, const std::string& type2)
{
    if (type1 == type2)
        return true;
    std::string r1 = resolveTypeAlias(type1);
    std::string r2 = resolveTypeAlias(type2);
    if (r1 == r2)
        return true;
    auto it1 = typeEquivGroup.find(r1);
    auto it2 = typeEquivGroup.find(r2);
    return it1 != typeEquivGroup.end() &&
           it2 != typeEquivGroup.end() &&
           it1->second == it2->second;
}


struct argType {
    std::string typeString = "";
    ASTNodeType baseASTType;
    uint8_t pointerLevel = 0;
    bool isReference = false;
    bool isConstant = false;
    bool mustBeExactType = false;
    bool hasDefault = false;
    ASTNode* defaultNode = nullptr;           // Expression_Term node (already resolved at definition site)
    std::vector<asaToken*> defaultRawTokens;  // raw tokens for re-parsing at call site

    // TODO: Update this to allow for many ABI formats
    // Extern ABI coercion: how to split this struct arg for the x86-64 SysV ABI
    int8_t externCoercionCount = 0;      // 0=none, N=split into N primitives (doubles or i64s)
    bool externCoercionIsFloat = false;  // true=doubles (SSE/XMM), false=i64s (INTEGER)


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

struct functionID {
    std::string name = "";
    std::string mangledName = "";
    std::string returnType = "";
    asaToken* token = nullptr;
    argumentList arguments = argumentList();
    argumentList userArguments = argumentList();
    bool variableNumArguments = false;
    bool isStructReturn = false;
    // For extern functions returning small structs via register coercion (x86-64 SysV ABI):
    // instead of sret, the return type is coerced to N i64s or doubles.
    int8_t externReturnCoercionCount = 0;      // 0=none (sret or scalar), N=N coerced chunks
    bool externReturnCoercionIsFloat = false;  // true=doubles (SSE), false=i64s (INTEGER)
    // Auto-generated struct default constructors are marked replaceable so that a user-defined
    // `create` constructor with the same identity can override them without error.
    bool isReplaceable = false;
    uint32_t uses = 0;
    bool isMemberFunction = false;
    Function* fnValue = nullptr;
    ASTNode* declNode = nullptr;

    functionID() {}
    functionID(std::string n, ASTNode* node, asaToken* t, std::string mN, std::string r, argumentList llvmArgs, argumentList userArgs, Function* f, bool vA = false, bool mF = false, bool sRet = false)
    {
        name = n;
        declNode = node;
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
    //void print()
    //{
    //    if (returnType != "")
    //        console::write(returnType + " ", console::blueFGColor);
    //    console::write(name, console::greenFGColor);

    //    console::write("(");
    //    for (int i = 0; i < userArguments.size(); i++) {
    //        if (userArguments[i].isReference)
    //            console::write("ref ", console::magentaFGColor);
    //        if (userArguments[i].isConstant)
    //            console::write("const ", console::magentaFGColor);
    //        console::write(userArguments[i].typeString, console::blueFGColor);
    //        if (i < userArguments.size() - 1)
    //            console::write(", ");
    //    }
    //    console::write(")");

    //    console::write("(");
    //    for (int i = 0; i < arguments.size(); i++) {
    //        if (arguments[i].isReference)
    //            console::write("ref ", console::magentaFGColor);
    //        if (arguments[i].isConstant)
    //            console::write("const ", console::magentaFGColor);
    //        console::write(arguments[i].typeString, console::blueFGColor);
    //        if (i < arguments.size() - 1)
    //            console::write(", ");
    //    }
    //    console::write(")");

    //    if (isStructReturn) {
    //        console::write(" (");
    //        console::write("returns struct", console::yellowFGColor);
    //        console::write(")");
    //    }

    //    if (isMemberFunction) {
    //        console::write(" (");
    //        console::write("is a member function", console::yellowFGColor);
    //        console::write(")");
    //    }
    //    console::writeLine();
    //}
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
        if (a.size() > userArguments.size() && !variableNumArguments)
            return 1000 - 1;
        if (a.size() < userArguments.size()) {
            // Allow if all extra params have defaults
            for (size_t i = a.size(); i < userArguments.size(); i++)
                if (!userArguments[i].hasDefault)
                    return 1000 - 1;
        }
        if (verbosity >= 6)
            console::writeLine("Comparing: " + name, console::blueFGColor);
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

            // *any: formal is a void-pointer that accepts any pointer type at the same level
            if (userArguments[i].typeString == "any" && userArguments[i].pointerLevel > 0 &&
                a[i].pointerLevel == userArguments[i].pointerLevel) {
                differences += 10;
                continue;
            }
            if (verbosity >= 6) {
                if (userArguments[i].typeString != a[i].typeString)
                    console::writeLine("[" + std::to_string(i) + "] typeString: " + userArguments[i].typeString + "!=" + a[i].typeString);
                if (userArguments[i].pointerLevel != a[i].pointerLevel)
                    console::writeLine("[" + std::to_string(i) + "] pointerLevel: " + std::to_string(userArguments[i].pointerLevel) + "!=" + std::to_string(a[i].pointerLevel));
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
                if (t1 == Identifier_Node && !areTypesEquivalent(userArguments[i].typeString, a[i].typeString)) {
                    differences = 800;
                    goto returnDifferences;
                }
                differences += 0;
            }
            // Else if they are both integer types (and same pointer level)
            else if (t1 >= Integer_Node && t1 <= Boolean_Node) {
                if (mustBeExactType) {  // If the argument type must be exact, but aren't
                    differences = 500;
                    goto returnDifferences;
                }
                if (t2 >= Integer_Node && t2 <= Boolean_Node)  // If similar type
                    differences += abs(t1 - t2);
                else if (t2 >= Double_Type && t2 <= Half_Type)  // float -> int implicit
                    differences += 50;
                else {
                    differences = 600;
                    goto returnDifferences;
                }
            }
            // Else if they are both float types (and same pointer level)
            else if (t1 >= Double_Type && t1 <= Half_Type) {
                if (mustBeExactType) {  // If the argument type must be exact but aren't
                    differences = 500;
                    goto returnDifferences;
                }
                if (t2 >= Double_Type && t2 <= Half_Type)  // If similar type
                    differences += abs(t1 - t2);
                else if (t2 >= Integer_Node && t2 <= Boolean_Node)  // int -> float implicit
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
            console::writeLine("returning difference of: " + std::to_string(differences));
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

struct AsaStruct {
    std::string name = "";
    argumentList members = argumentList();
    std::unordered_map<std::string, uint16_t> memberNameIndexes;
    std::unordered_map<std::string, ASTNode*> memberDefaultNodes;  // member name -> default value AST node
    std::unordered_map<std::string, ASTNode*> memberNameTypeNodes;
    uint32_t uses = 0;
    std::vector<functionID*> memberFunctions;
    StructType* structVal = nullptr;
    asaToken* token = nullptr;
    ASTNode* sourceNode = nullptr;
    ASAType* asaType = nullptr;
    bool isPacked = false;     // @packed: force integer packing in extern calls
    bool isNotPacked = false;  // @notpacked: force auto-detect ABI even if @packed is set
    AsaStruct() {}
    AsaStruct(std::string& n, asaToken*& tP, StructType*& sT, argumentList a, std::vector<functionID*>& mF, std::unordered_map<std::string, uint16_t>& mI)
    {
        name = n;
        token = tP;
        structVal = sT;
        members = a;
        memberFunctions = mF;
        memberNameIndexes = mI;
    }
    AsaStruct(std::string& n, asaToken*& tP, ASTNode* sN)
    {
        name = n;
        token = tP;
        sourceNode = sN;
    }
};

std::vector<functionID*> functionIDs = std::vector<functionID*>();
std::unordered_map<std::string, AsaStruct*> structDefinitions = std::unordered_map<std::string, AsaStruct*>();
std::stack<std::string> currentStructName = std::stack<std::string>();

DIType* createDIType(LLVMType* llvmType, const std::string& typeString)
{
    if (!llvmType || !llvmDebugBuilder)
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

        return llvmDebugBuilder->createBasicType(typeString, bitWidth, encoding);
    }
    else if (llvmType->isFloatTy()) {
        return llvmDebugBuilder->createBasicType("float", 32, dwarf::DW_ATE_float);
    }
    else if (llvmType->isDoubleTy()) {
        return llvmDebugBuilder->createBasicType("double", 64, dwarf::DW_ATE_float);
    }
    else if (llvmType->isPointerTy()) {
        // For pointers, create a pointer type
        DIType* pointeeTy = llvmDebugBuilder->createBasicType("void", 8, dwarf::DW_ATE_address);
        return llvmDebugBuilder->createPointerType(pointeeTy,
            llvmCompileModule->getDataLayout().getPointerSizeInBits());
    }

    // Default fallback
    return nullptr;
}

AsaStruct* getStructTypeFromLLVMType(LLVMType*& t)
{
    for (const auto& [key, value] : structDefinitions) {
        if ((LLVMType*)(value->structVal) == (LLVMType*)t) {
            return value;
        }
    }
    return nullptr;
}

//void printFunctionPrototypes()
//{
//    console::writeLine("\nFunction Prototypes:", console::greenFGColor);
//    console::indentation++;
//    for (const auto& f : functionIDs) {
//        console::write("> ");
//        f->print();
//    }
//    console::indentation--;
//}

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

void printFunctionDifferences(argumentList* arguments, functionID* other)
{
    console::write(other->name + " :: (");
    for (int i = 0; i < arguments->size(); i++) {
        argType a = (*arguments)[i];
        argType b = other->userArguments[i];
        std::string aStr = std::string(a.pointerLevel, '*') + a.typeString;
        std::string bStr = std::string(b.pointerLevel, '*') + b.typeString;
        if ((a.typeString == b.typeString || areTypesEquivalent(a.typeString, b.typeString)) && a.pointerLevel == b.pointerLevel)
            console::write(aStr, console::greenFGColor);
        else if (hasCastBetween(a.typeString, a.pointerLevel, b.typeString, b.pointerLevel))
            console::write(aStr + " ~= " + bStr, console::yellowFGColor);
        else
            console::write(aStr + " != " + bStr, console::redFGColor);
        if (i < arguments->size() - 1)
            console::write(", ");
    }
    console::write(")\n");
}

functionID* getFunctionFromID(std::vector<functionID*>& fnIDs, std::string& name, argumentList& arguments, bool wereTypesInferred = false, bool isMemberFunction = false, bool throwIfNotFound = false)
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
        goto undefinedFunction;

    // If the best function match requires exact typing (and different types are passed) throw error
    if (requiresExact) {
        messageSystem::addAttribute(&arguments);
        messageSystem::addAttribute(best);
        return (functionID*)messageSystem::error("Function match not found, closest prototype requires exact types. Did you try casting?", messageSystem::Undefined_Function_Exact_Error);
    }
    if (bestScore < 1000)
        return best;

undefinedFunction:
    if (throwIfNotFound) {
        for (auto& f : functionIDs) {
            if (f->name == name)
                messageSystem::addAttribute(f->declNode);
        }
        return (functionID*)messageSystem::error("Undefined function '" + name + "'", messageSystem::Undefined_Function_Error);
    }
    return nullptr;
}
functionID* getExactFunctionFromID(std::vector<functionID*>& fnIDs, std::string& name, argumentList& arguments, bool wereTypesInferred = false, bool throwIfNotFound = false)
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
            console::writeLine("Exact function match found");
    }
exactFnNotFound:
    if (verbosity >= 6)
        console::writeLine("Exact function match not found");
    if (throwIfNotFound) {
        messageSystem::addAttribute(&arguments);
        messageSystem::addAttribute(best);
        return (functionID*)messageSystem::error("Function match not found, closest prototype requires exact types. Did you try casting?", messageSystem::Undefined_Function_Exact_Error);
    }
    return nullptr;
}
functionID* getFunctionFromID(std::vector<functionID*>& fnIDs, std::string& name, std::vector<ASTNode*>& argValues, bool throwIfNotFound = false)
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
        messageSystem::addAttribute(best);
        //return (functionID*)messageSystem::error("Function match not found, closest prototype requires exact types. Did you try casting?", messageSystem::Undefined_Function_Exact_Error);
        return nullptr;  // TODO: This function doesnt have handling for exact requirement
    }
    if (bestScore < 1000)
        return best;
undefinedFunction:
    if (throwIfNotFound) {
        for (auto& f : functionIDs) {
            if (f->name == name)
                messageSystem::addAttribute(f->declNode);
        }
        return (functionID*)messageSystem::error("Undefined function '" + name + "'", messageSystem::Undefined_Function_Error);
    }
    return nullptr;
}
functionID* getFunctionFromID(std::vector<functionID*>& fnIDs, std::string& name, bool throwIfNotFound = false)
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
        messageSystem::addAttribute(best);
        //return (functionID*)messageSystem::error("Function match not found, closest prototype requires exact types. Did you try casting?", messageSystem::Undefined_Function_Exact_Error);
        return nullptr;  // TODO: Does this case ever occur?
    }
    if (bestScore < 1000)
        return best;
undefinedFunction:
    if (throwIfNotFound) {
        for (auto& f : functionIDs) {
            if (f->name == name)
                messageSystem::addAttribute(f->declNode);
        }
        return (functionID*)messageSystem::error("Undefined function '" + name + "'", messageSystem::Undefined_Function_Error);
    }
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
        console::printError("Function could not be resolved from Function*");
    wasError = true;
    errorDepth++;
    return nullptr;
}

LLVMValue* LogErrorV(const char* Str)
{
    console::printError(Str);
    return nullptr;
}

/// CreateEntryBlockAlloca - Create an alloca instruction in the entry block of
/// the function.  This is used for mutable variables etc.
static AllocaInst* CreateEntryBlockAlloca(Function* TheFunction, LLVMType* t, StringRef VarName)
{
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(), TheFunction->getEntryBlock().begin());
    return TmpB.CreateAlloca(t, nullptr, VarName);
}

static LLVMValue* generateDefaultValueForType(LLVMType* type, const std::string& typeName, int pointerLevel, int pass, ASTNode* node)
{
    if (!type)
        return nullptr;

    std::string resolvedTypeName = resolveTypeAlias(typeName);
    if (pointerLevel == 0 && type->isStructTy() && structDefinitions.find(resolvedTypeName) != structDefinitions.end()) {
        AsaStruct* structDef = structDefinitions[resolvedTypeName];
        if (structDef->structVal == nullptr && structDef->sourceNode)
            (structDef->sourceNode->*(structDef->sourceNode->codegen))(pass);
        if (wasError)
            return nullptr;

        argumentList emptyArgs;
        functionID* ctorID = getExactFunctionFromID(functionIDs, resolvedTypeName, emptyArgs);
        if (!ctorID || !ctorID->fnValue)
            return Constant::getNullValue(type);

        Function* fn = llvmIRBuilder->GetInsertBlock()->getParent();
        AllocaInst* defaultPtr = CreateEntryBlockAlloca(fn, type, "default_" + resolvedTypeName);
        llvmIRBuilder->CreateCall(ctorID->fnValue, {defaultPtr});
        ctorID->uses++;
        return llvmIRBuilder->CreateLoad(type, defaultPtr, "default_load");
    }

    return Constant::getNullValue(type);
}

static void instantiateVariantStruct(const std::string&, const std::string&, const std::string&, std::vector<std::pair<std::string, std::string>> = {});

LLVMType* getLLVMTypeFromString(std::string typeName, int pointerLevelOffset, asaToken*& token, bool& wasDefined, int& pass)
{
    LLVMType* aType;
    uint16_t pointerLevel = 0;
    while (typeName[0] == '*') {
        typeName = typeName.substr(1);
        pointerLevel++;
    }
    typeName = resolveTypeAlias(typeName);
    // Integer types
    if (typeName == "int" || typeName == "int32" || typeName == "uint" || typeName == "uint32")
        aType = LLVMType::getInt32Ty(*llvmCompileContext);
    else if (typeName == "int16" || typeName == "uint16")
        aType = LLVMType::getInt16Ty(*llvmCompileContext);
    else if (typeName == "int8" || typeName == "uint8" || typeName == "uchar" || typeName == "char")
        aType = LLVMType::getInt8Ty(*llvmCompileContext);
    else if (typeName == "int64" || typeName == "uint64")
        aType = LLVMType::getInt64Ty(*llvmCompileContext);
    else if (typeName == "int128" || typeName == "uint128")
        aType = LLVMType::getInt128Ty(*llvmCompileContext);

    // Floats
    else if (typeName == "float")
        aType = LLVMType::getFloatTy(*llvmCompileContext);
    else if (typeName == "half")
        aType = LLVMType::getHalfTy(*llvmCompileContext);
    else if (typeName == "double")
        aType = LLVMType::getDoubleTy(*llvmCompileContext);

    // Bool
    else if (typeName == "bool")
        aType = LLVMType::getInt1Ty(*llvmCompileContext);

    // Any (void pointer base type)
    else if (typeName == "any")
        aType = LLVMType::getInt8Ty(*llvmCompileContext);

    // Otherwise, look in struct definitions
    else if (structDefinitions.find(typeName) != structDefinitions.end()) {
        // If the struct body hasn't been generated yet, generate it
        if (structDefinitions[typeName]->structVal == nullptr) {
            if (currentStructName.size() == 0 || currentStructName.top() != typeName) {
                aType = (LLVMType*)(structDefinitions[typeName]->sourceNode->*(structDefinitions[typeName]->sourceNode->codegen))(pass);
                if (wasError) {
                    return nullptr;
                }
            }
            // If this type is inside of a struct and the type *is* the struct,
            // throw an error (nested structs aren't allowed)
            // TODO: Make struct self-usage better
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
            aType = (LLVMType*)(structDefinitions[typeName]->structVal);
    }

    // If still not found, check for variant struct instantiation (e.g. "array.int")
    else {
        size_t dotPos = typeName.find('.');
        if (dotPos != std::string::npos) {
            std::string baseName = typeName.substr(0, dotPos);
            std::string typeArg = typeName.substr(dotPos + 1);
            auto tmplIt = variantStructTemplates.find(baseName);
            if (tmplIt != variantStructTemplates.end()) {
                if (instantiatedVariantStructs.find(typeName) == instantiatedVariantStructs.end()) {
                    instantiatedVariantStructs.insert(typeName);
                    instantiateVariantStruct(baseName, typeArg, typeName);
                }
                auto sIt = structDefinitions.find(typeName);
                if (sIt != structDefinitions.end() && sIt->second && sIt->second->structVal) {
                    aType = (LLVMType*)sIt->second->structVal;
                    for (int i = 0; i < pointerLevel + pointerLevelOffset; i++)
                        aType = PointerType::get(*llvmCompileContext, 0);
                    //aType = PointerType::get(*llvmCompileContext, 0);
                    return aType;
                }
            }
        }
        unresolvedTypes[typeName] = nullptr;
        wasDefined = false;
        return (unresolvedTypes[typeName]);
    }
    for (int i = 0; i < pointerLevel + pointerLevelOffset; i++)
        aType = PointerType::get(*llvmCompileContext, 0);
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
        baseTypeName = "char";  // char/uint8 are unsigned in ASA; use unsigned default for i8
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
            llvm::StructType* AsaStruct = static_cast<llvm::StructType*>(baseType);
            if (AsaStruct->hasName()) {
                std::string fullName = AsaStruct->getName().str();
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
        console::writeLine("Returning: " + baseTypeName);

    // Prefix with pointer asterisks
    std::string pointerPrefix(pointerLevel, '*');
    return pointerPrefix + baseTypeName;
}

ASTNodeType getASTNodeTypeFromString(const std::string& typeNameIn)
{
    std::string typeName = resolveTypeAlias(typeNameIn);
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
    if (typeName == "any")
        return Any_Type;

    return Identifier_Node;
}

void castToHighestAccuracy(LLVMValue*& L, LLVMValue*& R, ASTNode* node)
{
    messageSystem::startBlock(node, "Generating automatic type cast", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
    defer(messageSystem::endBlock());

    std::string lTyStr = getStringTypeFromLLVMType(L->getType());
    std::string rTyStr = getStringTypeFromLLVMType(R->getType());
    if (node && node->childNodes.size() >= 2) {
        ASTNode* leftNode = node->childNodes[0];
        ASTNode* rightNode = node->childNodes[1];
        if (leftNode->asaType && !leftNode->asaType->strVal.empty()) {
            std::string trackedType = resolveTypeAlias(leftNode->asaType->strVal);
            if (typeSigns.count(trackedType))
                lTyStr = trackedType;
        }
        if (rightNode->asaType && !rightNode->asaType->strVal.empty()) {
            std::string trackedType = resolveTypeAlias(rightNode->asaType->strVal);
            if (typeSigns.count(trackedType))
                rTyStr = trackedType;
        }
    }
    ASTNodeType LType = getASTNodeTypeFromString(lTyStr);
    ASTNodeType RType = getASTNodeTypeFromString(rTyStr);
    bool lIsFloat = false;
    bool rIsFloat = false;
    bool lIsInt = false;
    bool rIsInt = false;
    // If signed, L more accurate
    if (LType < RType && RType < Begin_Unsigned_Integers) {
        R = castValue(R, L->getType(), typeSigns[rTyStr], typeSigns[lTyStr], node);
        if (R == nullptr)
            goto wasCastError;
        return;
    }
    // If signed, R more accurate
    else if (RType < LType && LType < Begin_Unsigned_Integers) {
        L = castValue(L, R->getType(), typeSigns[lTyStr], typeSigns[rTyStr], node);
        if (L == nullptr)
            goto wasCastError;
        return;
    }
    // If unsigned, L more accurate
    else if (LType < RType && RType < Double_Type) {
        R = castValue(R, L->getType(), typeSigns[rTyStr], typeSigns[lTyStr], node);
        if (R == nullptr)
            goto wasCastError;
        return;
    }
    // If unsigned, R more accurate
    else if (RType < LType && LType < Double_Type) {
        L = castValue(L, R->getType(), typeSigns[lTyStr], typeSigns[rTyStr], node);
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
        R = castValue(R, L->getType(), typeSigns[rTyStr], typeSigns[lTyStr], node);
        if (R == nullptr)
            goto wasCastError;
        return;
    }
    if (rIsFloat && lIsInt) {
        L = castValue(L, R->getType(), typeSigns[lTyStr], typeSigns[rTyStr], node);
        if (L == nullptr)
            goto wasCastError;
        return;
    }
    // If floats, L more accurate
    else if (LType < RType && RType < Half_Type) {
        R = castValue(R, L->getType(), typeSigns[rTyStr], typeSigns[lTyStr], node);
        if (R == nullptr)
            goto wasCastError;
        return;
    }
    // If floats, R more accurate
    else if (RType < LType && LType < Half_Type) {
        L = castValue(L, R->getType(), typeSigns[lTyStr], typeSigns[rTyStr], node);
        if (L == nullptr)
            goto wasCastError;
        return;
    }

wasCastError:
    console::indentation = errorDepth;
    if (errorDepth < maxErrorTraceDepth)
        messageSystem::error("Unsupported cast: cannot implicitly convert '" + lTyStr + "' to '" + rTyStr + "' (or vice versa)", messageSystem::Unsupported_Cast_Error);
    wasError = true;
    errorDepth++;
    return;
}

void initializeCodeGenerator()
{
    for (const auto& flag : commandLineCompilerDirectiveFlags)
        compilerDirectiveFlags[flag.first] = flag.second;

    // Open a new context and module.
    llvmCompileContext = std::make_unique<LLVMContext>();
    llvmCompileModule = std::make_unique<Module>("asa_global", *llvmCompileContext);

    llvmCompileModule->addModuleFlag(Module::Warning, "Debug Info Version", DEBUG_METADATA_VERSION);
    // For Darwin/macOS compatibility (optional but recommended)
    llvmCompileModule->addModuleFlag(Module::Warning, "Dwarf Version", 4);

    // Create debug builder
    llvmDebugBuilder = std::make_unique<DIBuilder>(*llvmCompileModule);

    // Create compile unit
    llvmDebugFile = llvmDebugBuilder->createFile(baseFileName, projectDirectory);
    llvmDebugCompileUnit = llvmDebugBuilder->createCompileUnit(
        dwarf::DW_LANG_C,  // Or create your own language constant
        llvmDebugFile,
        COMPILER_PRINTOUT,  // Producer
        isOptimizing(),     // IsOptimized
        "",                 // Flags
        0                   // Runtime Version
    );

    // Create a new builder for the module.
    llvmIRBuilder = std::make_unique<IRBuilder<>>(*llvmCompileContext);
}

void resetCodeGenerator()
{
    // Free heap-allocated codegen objects
    for (auto* fid : functionIDs)
        delete fid;
    functionIDs.clear();

    for (auto& [name, sd] : structDefinitions)
        delete sd;
    structDefinitions.clear();

    // Release LLVM objects (order matters: llvmDebugBuilder before llvmCompileModule, llvmCompileModule before llvmCompileContext)
    llvmDebugBuilder.reset();
    llvmIRBuilder.reset();
    llvmCompileModule.reset();
    llvmCompileContext.reset();
    llvmDebugCompileUnit = nullptr;
    llvmDebugFile = nullptr;
    llvmDebugFileCache.clear();
    LexicalBlocks.clear();

    // Reset all other global state
    globalStringLiteralConstants.clear();
    isCallMemberFunction = false;
    wasError = false;
    errorDepth = 0;
    compilerDirectiveFlags.clear();
    for (const auto& flag : commandLineCompilerDirectiveFlags)
        compilerDirectiveFlags[flag.first] = flag.second;
    compilerStacks.clear();
    linkedLibraries.clear();
    linkedStaticLibraries.clear();
    globalInitFn = nullptr;
    globalInitList.clear();
    moduleRegistry.clear();
    variantFunctionTemplates.clear();
    instantiatedVariantFunctions.clear();
    variantStructTemplates.clear();
    instantiatedVariantStructs.clear();
    unresolvedTypes.clear();
    lastRetrievedElementType = std::stack<ASAType*>();
    pipeOperationValue = std::stack<LLVMValue*>();
    loopContextStack = std::stack<LoopContext>();
    resultContextStack = std::stack<ResultContext>();
    currentStructName = std::stack<std::string>();

    // Reset type alias map and dynamic equivalence groups
    typeAliasMap.clear();
    typeEquivGroup = {
        {"int", 0},
        {"int32", 0},
        {"uint", 1},
        {"uint32", 1},
        {"int8", 2},
        {"char", 2},
        {"uint8", 3},
        {"uchar", 3},
    };
    nextTypeEquivGroup = 4;

    // Reset type sign table to built-in defaults
    typeSigns = {
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
}

static DIFile* getDIFile(const std::string& fullPath)
{
    auto it = llvmDebugFileCache.find(fullPath);
    if (it != llvmDebugFileCache.end())
        return it->second;
    std::string dir = std::filesystem::path(fullPath).parent_path().string();
    std::string name = std::filesystem::path(fullPath).filename().string();
    DIFile* f = llvmDebugBuilder->createFile(name, dir);
    llvmDebugFileCache[fullPath] = f;
    return f;
}

valueType* findNamedValue(ASTNode* node, ASTNode* childNode, std::string& identifier, asaToken*& token)
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

        // Module-scope nodes hold their variables privately unless the module
        // came from #use, in which case its children are visible unqualified.
        if (c->isModuleScope && !c->importedChildren)
            continue;
        if (c->isModuleScope && c->importedChildren) {
            auto moduleValue = c->namedValues.find(identifier);
            if (moduleValue != c->namedValues.end())
                return moduleValue->second;
        }

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
            AsaStruct* structDef = structDefinitions[structName];

            if (structDef->memberNameIndexes.find(identifier) != structDef->memberNameIndexes.end()) {
                messageSystem::error("Variable '" + identifier + "' not found. Did you mean 'this." + identifier + "'?", messageSystem::Undefined_Variable_Error);
                wasError = true;
                return nullptr;

                // TODO: Add the "Tip" message
                //console::indentation = errorDepth;
                //if (errorDepth < maxErrorTraceDepth)
                //    printTokenError(tokenRange {token, token}, "Variable '" + identifier + "' not found. Did you mean 'this." + identifier + "'?");
                //if (errorDepth < maxErrorTraceDepth)
                //    console::write("Tip: ", console::yellowFGColor);
                //if (errorDepth < maxErrorTraceDepth)
                //    console::writeLine("Member functions must use 'this' to access member variables.\n");
                //wasError = true;
                //errorDepth++;
                //return nullptr;
            }
        }
    }

    return nullptr;
}

static bool isCompileTimeDefinitionNode(ASTNode* node);

static ASTNode* findCompilerDefinition(ASTNode* scope, const std::string& identifier)
{
    while (scope) {
        auto it = scope->compilerDefinitions.find(identifier);
        if (it != scope->compilerDefinitions.end())
            return it->second;

        for (ASTNode* child : scope->childNodes) {
            if (!child || !child->isModuleScope || !child->importedChildren)
                continue;

            if (ASTNode* innerScope = getModuleInnerScope(child)) {
                auto importedIt = innerScope->compilerDefinitions.find(identifier);
                if (importedIt != innerScope->compilerDefinitions.end())
                    return importedIt->second;

                for (ASTNode* moduleChild : innerScope->childNodes) {
                    if (moduleChild && moduleChild->token && moduleChild->token->tokenStr == identifier && isCompileTimeDefinitionNode(moduleChild))
                        return moduleChild;
                }
            }
        }

        scope = scope->parentNode;
    }
    return nullptr;
}

static bool isCompileTimeDefinitionNode(ASTNode* node)
{
    if (!node)
        return false;

    return node->nodeType == Compiler_Define ||
           node->nodeType == Compiler_Define_Function ||
           node->nodeType == Compiler_Define_Cast ||
           node->nodeType == Compiler_Define_Struct ||
           node->nodeType == Compiler_Define_Enum;
}

static ASTNode* findCompileTimeDefinitionNode(ASTNode* scope, const std::string& identifier)
{
    ASTNode* compilerDefinition = findCompilerDefinition(scope, identifier);
    if (compilerDefinition)
        return compilerDefinition;

    while (scope) {
        for (ASTNode* child : scope->childNodes) {
            if (child && child->token && child->token->tokenStr == identifier && isCompileTimeDefinitionNode(child))
                return child;
        }
        scope = scope->parentNode;
    }

    return nullptr;
}

ASTNode* ASTNode::resolveCompilerDefinitionASTNode(int pass)
{
    if (!token)
        return nullptr;

    return findCompileTimeDefinitionNode(parentNode, token->tokenStr);
}

// Infer the ASA type string for a compile-time define body node (an Expression_Term).
// Used when a compiler define is accessed via module member access (e.g. Mod.ABC).
static std::string getDefineTypeString(ASTNode* defineBody)
{
    ASTNode* child = defineBody;
    // Unwrap Expression_Term wrapper
    while (child && child->nodeType == Expression_Term && !child->childNodes.empty())
        child = child->childNodes[0];
    if (!child)
        return "int";
    switch (child->nodeType) {
        case Integer_Node:
            return "int";
        case Float_Node:
            return "float";
        case Boolean_Node:
            return "bool";
        case String_Constant_Node:
            return "string";
        default:
            return "int";
    }
}

// Search the module's scope body tree for a compilerDefinitions entry.
static ASTNode* findModuleCompilerDefine(ASTNode* node, const std::string& name)
{
    auto it = node->compilerDefinitions.find(name);
    if (it != node->compilerDefinitions.end())
        return it->second;
    for (auto& child : node->childNodes) {
        if (child->nodeType == Scope_Body || child->nodeType == Module_Define_Node) {
            ASTNode* found = findModuleCompilerDefine(child, name);
            if (found)
                return found;
        }
    }
    return nullptr;
}

std::string getMemberAccessTypeString(ASTNode* node, ASTNode* parentNode, asaToken*& token)
{
    // Base case: if it's just an identifier, look it up normally
    if (node->nodeType == Identifier_Node) {
        // Check if it's a module name (not a variable)
        auto modIt = moduleRegistry.find(node->token->tokenStr);
        if (modIt != moduleRegistry.end())
            return "__module__:" + node->token->tokenStr;

        valueType* val = findNamedValue(parentNode, nullptr, node->token->tokenStr, token);
        if (!val && !wasError) {
            console::indentation = errorDepth;
            if (errorDepth < maxErrorTraceDepth)
                printTokenError(tokenRange {token, token}, "Unknown variable name: " + node->token->tokenStr);
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
                std::string memberName = rightNode->token->tokenStr;
                auto varIt = modIt->second->namedValues.find(memberName);
                if (varIt != modIt->second->namedValues.end())
                    return varIt->second->type;
                // Also check nested sub-modules
                auto subModIt = moduleRegistry.find(modName + "." + memberName);
                if (subModIt != moduleRegistry.end())
                    return "__module__:" + modName + "." + memberName;
                // Also check compiler defines (e.g. `ABC :: 42;`)
                ASTNode* defineNode = findModuleCompilerDefine(modIt->second, memberName);
                if (defineNode)
                    return getDefineTypeString(defineNode);
            }
            console::indentation = errorDepth;
            if (errorDepth < maxErrorTraceDepth)
                printTokenError(tokenRange {token, token}, "Module has no member '" + rightNode->token->tokenStr + "'");
            wasError = true;
            errorDepth++;
            return "";
        }

        // Remove pointer markers to get the struct name
        std::string structName = leftType;
        while (structName[0] == '*') {
            structName = structName.substr(1);
        }
        structName = resolveTypeAlias(structName);

        // Look up the struct definition
        if (structDefinitions.find(structName) == structDefinitions.end()) {
            console::indentation = errorDepth;
            if (errorDepth < maxErrorTraceDepth)
                printTokenError(tokenRange {token, token}, "Type \"" + structName + "\" is not a defined struct");
            wasError = true;
            errorDepth++;
            return "";
        }

        AsaStruct* structDef = structDefinitions[structName];

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
        std::string memberName = rightNode->token->tokenStr;

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
    if (node->token->tokenStr == "this" && !currentStructName.empty()) {
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
    ASTNode* leftNode = exprStmtNode->nodeType == Colon_Separator_Node ? exprStmtNode : exprStmtNode->childNodes[0];

    // A typed declaration with no initializer has only
    // one child (the Colon_Separator_Node).  Allow that through; untyped (inferred)
    // declarations still require a right-hand-side child.
    bool hasRHS = exprStmtNode->nodeType != Colon_Separator_Node && exprStmtNode->childNodes.size() >= 2;
    if (!hasRHS && leftNode->nodeType != Colon_Separator_Node)
        return;
    ASTNode* rhsInitializer = hasRHS ? unwrapSingleExpressionNode(exprStmtNode->childNodes[1]) : nullptr;
    bool rhsIsUndefined = rhsInitializer && rhsInitializer->nodeType == Undefined_Initializer_Node;
    bool rhsIsDefault = rhsInitializer && rhsInitializer->nodeType == Default_Initializer_Node;

    std::string varName;
    std::string typeName;
    int pointerLevel = 0;
    bool isConst = false;
    LLVMType* llvmType = nullptr;

    if (leftNode->nodeType == Colon_Separator_Node) {
        // Typed declaration: x : int = 5  OR  x : int  (no initializer)
        if (leftNode->childNodes.size() < 2)
            return;
        ASTNode* nameNode = leftNode->childNodes[0];
        ASTNode* typeNode = leftNode->childNodes[1];
        varName = nameNode->token->tokenStr;

    getNextPointerLevel:
        if (typeNode->token->tokenStr == "const") {
            isConst = true;
            typeNode = typeNode->childNodes[0];
            goto getNextPointerLevel;
        }
        if (typeNode->token->tokenStr == "ref" || typeNode->token->tokenStr == "exact") {
            typeNode = typeNode->childNodes[0];
            goto getNextPointerLevel;
        }
        if (typeNode->token->tokenStr == "*") {
            pointerLevel++;
            typeNode = typeNode->childNodes[0];
            goto getNextPointerLevel;
        }
        typeName = typeNode->token->tokenStr;

        bool wasDefined = true;
        int pass = 1;
        llvmType = getLLVMTypeFromString(typeName, 0, typeNode->token, wasDefined, pass);
        if (!llvmType || !wasDefined)
            return;
        for (int i = 0; i < pointerLevel; i++)
            llvmType = PointerType::get(*llvmCompileContext, 0);
    }
    else if (leftNode->nodeType == Identifier_Node) {
        // Untyped declaration: infer type by speculatively evaluating rhs
        if (!hasRHS)
            return;
        varName = leftNode->token->tokenStr;
        ASTNode* exprNode = exprStmtNode->childNodes[1];
        if (!exprNode || !exprNode->codegen)
            return;

        {
            ASTNode* voidCheck = exprNode;
            voidCheck = unwrapSingleExpressionNode(voidCheck);
            if (voidCheck->nodeType == Void_Node || voidCheck->nodeType == Undefined_Initializer_Node || voidCheck->nodeType == Default_Initializer_Node) {
                messageSystem::startBlock(exprStmtNode, "Processing declaration", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                messageSystem::error("Cannot infer type from this initializer. Use an explicit type annotation.", messageSystem::Type_Inference_From_Void_Error);
                return;
            }
        }

        // Save builder state
        BasicBlock* savedBB = llvmIRBuilder->GetInsertBlock();
        BasicBlock::iterator savedPt = savedBB ? llvmIRBuilder->GetInsertPoint() : BasicBlock::iterator();
        bool savedError = wasError;

        // Create a temporary function+block to probe the expression's type
        FunctionType* ft = FunctionType::get(LLVMType::getVoidTy(*llvmCompileContext), false);
        Function* probeF = Function::Create(ft, Function::PrivateLinkage, "__type_probe__", llvmCompileModule.get());
        BasicBlock* probeBB = BasicBlock::Create(*llvmCompileContext, "probe", probeF);
        llvmIRBuilder->SetInsertPoint(probeBB);

        wasError = false;
        messageSystem::suppressErrors = true;
        LLVMValue* probeVal = (LLVMValue*)(exprNode->*(exprNode->codegen))(1);
        messageSystem::suppressErrors = false;

        if (!wasError && probeVal)
            llvmType = probeVal->getType();
        wasError = savedError;

        probeF->eraseFromParent();

        if (savedBB)
            llvmIRBuilder->SetInsertPoint(savedBB, savedPt);

        if (!llvmType)
            return;  // Could not infer type; skip (will be caught as undefined if used)

        typeName = getStringTypeFromLLVMType(llvmType);
    }
    else {
        return;
    }

    std::string globalName = varName;
    Constant* initialValue = rhsIsUndefined ? UndefValue::get(llvmType) : Constant::getNullValue(llvmType);
    GlobalVariable* gv = new GlobalVariable(
        *llvmCompileModule, llvmType, isConst,
        GlobalValue::InternalLinkage,
        initialValue,
        globalName);

    std::string actualType = std::string(pointerLevel, '*') + typeName;
    valueType* vt = new valueType(varName, actualType, gv);
    vt->isConstant = isConst;
    vt->isUndefined = rhsIsUndefined;
    vt->declNode = exprStmtNode;  // the expression statement node that owns the attributes

    // For root-level vars, store in the expression stmt's own namedValues so
    // findNamedValue (which checks direct children of root) can find it.
    // For module vars, store in the module node's namedValues (accessible only
    // via Fore.black member access, not by direct lookup because isModuleScope
    // makes findNamedValue skip it).
    ownerNode->namedValues[varName] = vt;
    if (ownerNode != exprStmtNode)
        exprStmtNode->namedValues[varName] = vt;
    exprStmtNode->currentNodeDoneGenerating = true;

    if (rhsIsUndefined)
        return;

    globalInitList.push_back({gv, exprStmtNode});

    if (!globalInitFn) {
        FunctionType* ft = FunctionType::get(LLVMType::getVoidTy(*llvmCompileContext), false);
        globalInitFn = Function::Create(
            ft, Function::InternalLinkage, "__asa_global_init", *llvmCompileModule);
    }
}

// Register a Compiler_Define (module) node and declare all its variable globals.
static void registerImportedModuleAliases(ASTNode* moduleCompilerDefineNode, const std::string& parentName = "")
{
    if (!moduleCompilerDefineNode || !moduleCompilerDefineNode->isModuleScope || !moduleCompilerDefineNode->token)
        return;

    std::string moduleName = moduleCompilerDefineNode->token->tokenStr;
    std::string fullName = parentName.empty() ? moduleName : (parentName + "." + moduleName);
    moduleRegistry[fullName] = moduleCompilerDefineNode;

    ASTNode* innerScope = getModuleInnerScope(moduleCompilerDefineNode);
    if (!innerScope)
        return;

    for (ASTNode* child : innerScope->childNodes) {
        if (child && child->nodeType == Compiler_Define && child->isModuleScope)
            registerImportedModuleAliases(child, fullName);
    }
}

void processModuleForDeclarations(ASTNode* moduleCompilerDefineNode, std::string parentName)
{
    std::string moduleName = moduleCompilerDefineNode->token->tokenStr;
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
        if (child->nodeType == Expression_Statement || child->nodeType == Colon_Separator_Node)
            declareModuleScopeVariable(child, moduleCompilerDefineNode, true);
        // Recurse into nested sub-modules, registering with compound dot-separated names.
        // Guard with isModuleScope so that plain value defines (DEF :: 42) are not
        // incorrectly treated as modules and registered in moduleRegistry.
        else if (child->nodeType == Compiler_Define && child->isModuleScope)
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

    if (moduleCompilerDefineNode->importedChildren) {
        for (ASTNode* child : innerScope->childNodes) {
            if (child && child->nodeType == Compiler_Define && child->isModuleScope)
                registerImportedModuleAliases(child);
        }
    }
}

// Fill in __asa_global_init's body and finalize it. returns false on error/failure
bool finalizeGlobalInit()
{
    if (!globalInitFn || globalInitList.empty())
        return true;


    BasicBlock* BB = BasicBlock::Create(*llvmCompileContext, "entry", globalInitFn);
    // Clear any stale debug location so __asa_global_init instructions don't
    // inherit a scope from the last user-function that was generated.
    llvmIRBuilder->SetCurrentDebugLocation(DebugLoc());
    llvmIRBuilder->SetInsertPoint(BB);

    for (auto& gi : globalInitList) {
        ASTNode* node = gi.exprStmtNode;
        GlobalVariable* gv = gi.gv;

        LLVMType* gvType = gv->getValueType();
        ASTNode* exprTerm = node->nodeType == Colon_Separator_Node ? nullptr : (node->childNodes.size() >= 2 ? node->childNodes[1] : nullptr);
        ASTNode* initializerNode = unwrapSingleExpressionNode(exprTerm);
        if (initializerNode && initializerNode->nodeType == Undefined_Initializer_Node)
            continue;

        LLVMValue* initVal = nullptr;
        if (!exprTerm || (initializerNode && initializerNode->nodeType == Default_Initializer_Node)) {
            ASTNode* declarationNode = node->nodeType == Colon_Separator_Node ? node : node->childNodes[0];
            if (declarationNode->nodeType != Colon_Separator_Node || declarationNode->childNodes.size() < 2)
                return false;

            ASTNode* typeNode = declarationNode->childNodes[1];
            int pointerLevel = 0;
        getNextGlobalDefaultPointerLevel:
            if (typeNode->token->tokenStr == "const" || typeNode->token->tokenStr == "ref" || typeNode->token->tokenStr == "exact") {
                typeNode = typeNode->childNodes[0];
                goto getNextGlobalDefaultPointerLevel;
            }
            if (typeNode->token->tokenStr == "*") {
                pointerLevel++;
                typeNode = typeNode->childNodes[0];
                goto getNextGlobalDefaultPointerLevel;
            }
            initVal = generateDefaultValueForType(gvType, typeNode->token->tokenStr, pointerLevel, 2, node);
        }
        else {
            initVal = (LLVMValue*)(exprTerm->*(exprTerm->codegen))(2);
        }
        if (wasError || !initVal) {
            return false;
        }

        messageSystem::startBlock(node, "Initializing global variable", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

        if (initVal->getType() != gvType) {
            if (gvType->isStructTy() && initVal->getType()->isPointerTy()) {
                // If it's a string struct { ptr, i32 } and we have a *char, build it properly
                StructType* st = cast<StructType>(gvType);
                if (st->getNumElements() == 2 &&
                    st->getElementType(0)->isPointerTy() &&
                    st->getElementType(1)->isIntegerTy(32)) {
                    FunctionCallee strlenFn = llvmCompileModule->getOrInsertFunction("strlen",
                        FunctionType::get(LLVMType::getInt64Ty(*llvmCompileContext),
                            {PointerType::getUnqual(*llvmCompileContext)}, false));
                    LLVMValue* lenVal = llvmIRBuilder->CreateCall(strlenFn, {initVal}, "strlen");
                    LLVMValue* lenTrunc = llvmIRBuilder->CreateTrunc(lenVal, LLVMType::getInt32Ty(*llvmCompileContext), "len");
                    LLVMValue* strStruct = UndefValue::get(gvType);
                    strStruct = llvmIRBuilder->CreateInsertValue(strStruct, initVal, {0});
                    strStruct = llvmIRBuilder->CreateInsertValue(strStruct, lenTrunc, {1});
                    initVal = strStruct;
                }
                else {
                    initVal = llvmIRBuilder->CreateLoad(gvType, initVal, "gv_load");
                }
            }
            else {
                initVal = castValue(initVal, gvType, false, false, node);
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
            llvmIRBuilder->CreateStore(initVal, gv);
        }

        messageSystem::endBlock();
    }

    llvmIRBuilder->CreateRetVoid();
    llvmIRBuilder->ClearInsertionPoint();

    return true;
}


LLVMValue* castValue(LLVMValue* value, llvm::Type* destType, bool isSrcSigned, bool isToSigned, ASTNode* node, bool destTypeIsStruct)
{
    messageSystem::startBlock(node, "Generating type cast", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
    defer(messageSystem::endBlock());

    llvm::Type* srcType = value->getType();

    if (destTypeIsStruct) {
        return value;
    }

    if (srcType == destType)
        return value;

    if (srcType->isIntegerTy() && destType->isIntegerTy())
        return llvmIRBuilder->CreateIntCast(value, destType, isSrcSigned, "cast");

    if (srcType->isIntegerTy() && destType->isFloatingPointTy())
        return isSrcSigned ? llvmIRBuilder->CreateSIToFP(value, destType, "cast")
                           : llvmIRBuilder->CreateUIToFP(value, destType, "cast");

    if (srcType->isFloatingPointTy() && destType->isIntegerTy())
        return isToSigned ? llvmIRBuilder->CreateFPToSI(value, destType, "cast")
                          : llvmIRBuilder->CreateFPToUI(value, destType, "cast");

    if (srcType->isFloatingPointTy() && destType->isFloatingPointTy())
        return llvmIRBuilder->CreateFPCast(value, destType, "cast");

    if (srcType->isPointerTy() && destType->isPointerTy())
        return llvmIRBuilder->CreatePointerCast(value, destType, "cast");

    if (srcType->isPointerTy() && destType->isIntegerTy())
        return llvmIRBuilder->CreatePtrToInt(value, destType, "cast");

    if (srcType->isIntegerTy() && destType->isPointerTy())
        return llvmIRBuilder->CreateIntToPtr(value, destType, "cast");

    // Use bitcast only if size matches and none of the above applies
    if (llvm::CastInst::isBitOrNoopPointerCastable(srcType, destType, llvmCompileModule->getDataLayout()))
        return llvmIRBuilder->CreateBitCast(value, destType, "cast");

    std::string lTyStr = getStringTypeFromLLVMType(srcType);
    std::string rTyStr = getStringTypeFromLLVMType(destType);

    console::indentation = errorDepth;
    if (errorDepth < maxErrorTraceDepth)
        messageSystem::error("Unsupported cast: cannot implicitly convert '" + lTyStr + "' to '" + rTyStr + "' (or vice versa)", messageSystem::Unsupported_Cast_Error);
    wasError = true;
    errorDepth++;
    return nullptr;
}

//bool findVariableDeclaration(ASTNode*& node, ASTNode*& childNode, std::string& identifier)
//{
//  for (auto& c : node->childNodes) {
//      if (node->depth > 0)     // only go past use if in global scope
//          if (c == childNode)  // dont go past the node where it is used
//              break;
//      if (c->nodeType == Expression_Statement && c->childNodes.size() > 0)
//          if (c->childNodes[0]->token->tokenStr == identifier)
//              return true;
//  }
//  if (node->depth > 0)  // search the parent recursively until found or end of global scope is reached
//      return findVariableDeclaration(node->parentNode, node, identifier);
//
//  return false;
//}

// LLVMValue*
void* ASTNode::generateConstant(int pass)
{
    messageSystem::startBlock(this, "Generating constant", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (nodeType == Integer_Node) {
        try {
            int intVal = stoi(token->tokenStr);
            messageSystem::endBlock();
            return ConstantInt::get(*llvmCompileContext, APInt(32, intVal, true));
        }
        catch (...) {
            long intVal = stol(token->tokenStr);
            messageSystem::endBlock();
            return ConstantInt::get(*llvmCompileContext, APInt(64, intVal, true));
        }
    }
    else if (nodeType == Boolean_Node) {
        messageSystem::endBlock();
        return ConstantInt::get(*llvmCompileContext, APInt(1, token->tokenStr == "true" ? 1 : 0, false));
    }
    else if (nodeType == Float_Node) {
        messageSystem::endBlock();
        return ConstantFP::get(*llvmCompileContext, APFloat(stod(token->tokenStr)));
    }
    else if (nodeType == Void_Node) {
        messageSystem::endBlock();
        // Return a null pointer constant (can be cast to any pointer type)
        return ConstantPointerNull::get(PointerType::getUnqual(*llvmCompileContext));
    }
    else if (nodeType == String_Constant_Node) {
        std::string strValue = decodeQuotedStringToken(token);

        GlobalVariable* globalStr = nullptr;
        if (globalStringLiteralConstants.find(strValue) != globalStringLiteralConstants.end())
            globalStr = globalStringLiteralConstants[strValue];
        else {
            // Create constant data array (i8 array)
            Constant* strConst = ConstantDataArray::getString(*llvmCompileContext, strValue, true);

            // Create global variable to hold the string
            globalStr = new GlobalVariable(
                *llvmCompileModule,
                strConst->getType(),
                true,                         // Constant
                GlobalValue::PrivateLinkage,  // Or InternalLinkage for hidden
                strConst,
                "str");
            globalStr->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);  // Allow merging
            globalStr->setAlignment(Align(1));

            globalStringLiteralConstants[strValue] = globalStr;
        }

        // Get pointer to the tokenStr element (i8*)
        Constant* zero = ConstantInt::get(LLVMType::getInt32Ty(*llvmCompileContext), 0);
        std::vector<Constant*> indices = {zero, zero};
        Constant* strPtr = ConstantExpr::getGetElementPtr(
            globalStr->getValueType(),
            globalStr,
            indices);

        // Return a string struct { ptr, length } unless we're inside the string
        // module itself (where raw *char is needed for bootstrapping).
        if (!compilerDirectiveFlags["IN_STRING_MODULE"] &&
            structDefinitions.count("string") && structDefinitions["string"]->structVal) {
            StructType* strTy = cast<StructType>((LLVMType*)structDefinitions["string"]->structVal);
            Constant* lenConst = ConstantInt::get(LLVMType::getInt32Ty(*llvmCompileContext), (uint32_t)strValue.size());
            messageSystem::endBlock();
            return ConstantStruct::get(strTy, {strPtr, lenConst});
        }

        messageSystem::endBlock();
        return strPtr;  // Fallback: returns i8* pointing to the string
    }
    else if (nodeType == Character_Constant_Node) {
        std::string strValue = decodeQuotedStringToken(token);

        if (strValue.size() > 1) {
            return messageSystem::error("Character constant may contain only a single character");
        }

        messageSystem::endBlock();
        return ConstantInt::get(*llvmCompileContext, APInt(8, strValue[0], false));
    }

    return messageSystem::error("Value could not be parsed as constant");
}

// LLVMValue*
void* ASTNode::generateVariableExpression(int pass)
{
    messageSystem::startBlock(this, "Generating variable expression", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Look this variable up in the function.
    valueType* val = findNamedValue(parentNode, this, token->tokenStr, token);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }
    if (!val) {
        LLVMValue* exprVal = ConstantInt::get(LLVMType::getInt32Ty(*llvmCompileContext), 0);
        LLVMType* llvmType = nullptr;
        Function* theFunction = llvmIRBuilder->GetInsertBlock()->getParent();
        uint16_t pointerLevel = 0;
        std::string typeName = "";

        // If variable does not have type, it is a used undefined variable
        if (childNodes.size() == 0) {
            ASTNode* compilerDefinition = findCompilerDefinition(parentNode, token->tokenStr);
            if (compilerDefinition) {
                messageSystem::endBlock();
                auto generatedCompilerDefine = (compilerDefinition->*(compilerDefinition->codegen))(pass);
                if (wasError) {
                    messageSystem::endBlock();
                    return nullptr;
                }
                return generatedCompilerDefine;
            }
            return messageSystem::error("Use of undefined variable", messageSystem::Undefined_Symbol_Error);
        }
        // If variable does have type, it is a declaration
        else {
            ASTNode* typeNode = childNodes[0];
            typeName = resolveTypeAlias(typeNode->token->tokenStr);

            messageSystem::startBlock(typeNode, "Generating type", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

        getNextPointerLevel:
            if (typeNode->token->tokenStr == "*") {
                pointerLevel++;
                typeNode = typeNode->childNodes[0];
                goto getNextPointerLevel;
            }
            bool wasDefined = true;
            llvmType = getLLVMTypeFromString(typeName, 0, typeNode->token, wasDefined, pass);
            for (int i = 0; i < pointerLevel; i++)
                llvmType = PointerType::get(*llvmCompileContext, 0);
            if (typeSigns.find(typeName) != typeSigns.end())  // If builtin type
                exprVal = castValue(exprVal, llvmType, true, typeSigns[typeNode->token->tokenStr], this);
            else if (structDefinitions.find(typeName) != structDefinitions.end())  // If defined struct
                exprVal = castValue(exprVal, llvmType, true, false, this, true);
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
        AllocaInst* targetPtr = CreateEntryBlockAlloca(theFunction, llvmType, token->tokenStr);
        std::string actualType = (pointerLevel > 0 ? std::string(pointerLevel, '*') : "") + typeName;
        namedValues[token->tokenStr] = new valueType(token->tokenStr, actualType, targetPtr);
        namedValues[token->tokenStr]->declNode = this;
        namedValues[token->tokenStr]->isUndefined = false;

        llvmIRBuilder->CreateStore(exprVal, targetPtr);

        messageSystem::endBlock();

        if (isRef || lvalue)
            return targetPtr;
        else
            return llvmIRBuilder->CreateLoad(targetPtr->getAllocatedType(), targetPtr, token->tokenStr + "_load");
    }
    // Check @deprecated / @removed on the variable declaration
    if (val->declNode)
        if (!checkDeprecationAttributes(this, val->declNode, token->tokenStr)) {
            messageSystem::endBlock();
            return nullptr;
        }

    LLVMValue* A = val->val;
    LLVMType* valType = getValueStoredType(A);
    if (!asaType)
        asaType = new ASAType(valType);
    else
        asaType->baseLLVMType = valType;
    // Store the type string so pointer element types can be resolved later for array subscripts
    asaType->strVal = val->type;
    asaType->isRef = val->isReference;

    if (!isRef && !lvalue && val->isUndefined) {
        messageSystem::addAttribute(val->declNode, "declared here");
        messageSystem::error("Variable '" + token->tokenStr + "' was declared undefined, and used before being defined.", messageSystem::Declared_Undefined_Variable_Error);
    }

    // Handle references: need to dereference when used as rvalue
    if (val->isReference && !isRef && !lvalue) {
        messageSystem::startBlock(this, "Generating reference usage", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

        // valType is the pointer type; load the pointer, then deref through it using the base type
        LLVMValue* ptr = llvmIRBuilder->CreateLoad(valType, A, token->tokenStr + "_ref_ptr");
        bool wasDefined = true;
        LLVMType* baseType = getLLVMTypeFromString(val->type, 0, token, wasDefined, pass);
        if (!baseType || !wasDefined) {
            return messageSystem::error("Cannot resolve ref base type for dereference");
        }
        if (!asaType)
            asaType = new ASAType(baseType);
        else
            asaType->baseLLVMType = baseType;
        messageSystem::endBlock();

        messageSystem::endBlock();
        return llvmIRBuilder->CreateLoad(baseType, ptr, token->tokenStr + "_ref_deref");
    }

    messageSystem::endBlock();
    if (isRef || lvalue)
        return A;
    else
        return llvmIRBuilder->CreateLoad(valType, A, token->tokenStr + "_load");
}

void* ASTNode::generateThrow(int pass)
{
    messageSystem::startBlock(this, "Generating throw statement", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // If it has a child node, we will output it's value as a string
    ASTNode* exprNode = nullptr;
    LLVMValue* outVal = nullptr;
    if (childNodes.size() > 0) {
        exprNode = childNodes[0]->childNodes[0];
        if (exprNode->codegen == nullptr) {
            return messageSystem::error("Node `" + ASTNodeTypeAsString(exprNode->nodeType) + "` does not have a code generator");
        }
        outVal = (LLVMValue*)(exprNode->*(exprNode->codegen))(pass);
    }
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }

    // Create and print the prefix message first
    std::string throwPrefix = "Exception:  file: \"" + *(token->filePath) + "\"   line: " + std::to_string(token->lineNumber) + "\n    ";
    LLVMValue* prefixStr = llvmIRBuilder->CreateGlobalString(throwPrefix);

    // Look up print function for the prefix (char* type)
    argumentList prefixArgList;
    prefixArgList.push_back(argType("char", Char_Type, 1));
    std::string printFnName = "print";
    functionID* prefixPrintFnID = getFunctionFromID(functionIDs, printFnName, prefixArgList, true, false);

    if (prefixPrintFnID) {
        std::vector<LLVMValue*> prefixArgs;
        prefixArgs.push_back(prefixStr);
        llvmIRBuilder->CreateCall(prefixPrintFnID->fnValue, prefixArgs);
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
        functionID* printFnID = getFunctionFromID(functionIDs, printFnName, argList, true, false);

        if (printFnID) {
            // Call the print function
            std::vector<LLVMValue*> printArgs;
            printArgs.push_back(outVal);
            llvmIRBuilder->CreateCall(printFnID->fnValue, printArgs);
            printFnID->uses++;
        }
    }

    // Declare exit function if not already declared
    FunctionType* exitFuncType = FunctionType::get(
        LLVMType::getVoidTy(*llvmCompileContext),
        {Type::getInt32Ty(*llvmCompileContext)},
        false);
    FunctionCallee exitFunc = llvmCompileModule->getOrInsertFunction("exit", exitFuncType);

    // Call exit(1) to terminate the program
    llvmIRBuilder->CreateCall(exitFunc, {ConstantInt::get(LLVMType::getInt32Ty(*llvmCompileContext), 1)});

    // Create an unreachable instruction since exit() doesn't return
    llvmIRBuilder->CreateUnreachable();

    messageSystem::endBlock();

    return nullptr;
}

void* ASTNode::generateReturn(int pass)
{
    messageSystem::startBlock(this, "Generating return", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (!LexicalBlocks.empty() && token && token->filePath)
        llvmIRBuilder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));
    ASTNode* exprNode = childNodes[0];

    messageSystem::startBlock(exprNode, "Generating return expression value", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    LLVMValue* RetVal = (LLVMValue*)(exprNode->*(exprNode->codegen))(pass);
    if (wasError) {
        return messageSystem::error("Failed to generate return expression value");
    }
    Function* currentFunc = llvmIRBuilder->GetInsertBlock()->getParent();
    functionID* fnID = getFunctionIDFromFunctionPointer(functionIDs, currentFunc);
    // Check if we're returning a struct
    LLVMType* returnType = llvmIRBuilder->GetInsertBlock()->getParent()->getReturnType();
    if (fnID->isStructReturn) {
        // Get the sret parameter (first parameter)
        LLVMValue* sretPtr = &*currentFunc->arg_begin();

        // Copy the struct value to the sret location
        if (RetVal->getType()->isPointerTy()) {
            // If RetVal is a pointer to struct, memcpy from it
            LLVMValue* structSize = ConstantInt::get(LLVMType::getInt64Ty(*llvmCompileContext),
                llvmCompileModule->getDataLayout().getTypeAllocSize(returnType));

            // Create memcpy call
            Function* memcpyFunc = Intrinsic::getOrInsertDeclaration(llvmCompileModule.get(), Intrinsic::memcpy, {sretPtr->getType(), RetVal->getType(), LLVMType::getInt64Ty(*llvmCompileContext)});
            llvmIRBuilder->CreateCall(memcpyFunc, {sretPtr, RetVal, structSize, ConstantInt::get(LLVMType::getInt1Ty(*llvmCompileContext), 0)});
        }
        else {
            // If RetVal is a struct value, store it
            llvmIRBuilder->CreateStore(RetVal, sretPtr);
        }


        llvmIRBuilder->CreateRetVoid();
    }
    else {
        // Non-struct return, handle normally
        LLVMType* retType = llvmIRBuilder->GetInsertBlock()->getParent()->getReturnType();
        if (!RetVal) {
            if (retType->isVoidTy()) {
                llvmIRBuilder->CreateRetVoid();
            }
            else {
                // main gets implicit return 0; all other typed functions warn
                Function* currentFn = llvmIRBuilder->GetInsertBlock()->getParent();
                if (currentFn->getName() != "main")
                    printTokenWarning(getASTTokenRange(this), "returning void in function that expects a return value");
                llvmIRBuilder->CreateRet(Constant::getNullValue(retType));
            }
        }
        else {
            if (retType->isVoidTy()) {
                return messageSystem::error("Returning a value is a function with no return type.");
            }
            if (RetVal->getType() != retType) {
                Function* currentFn = llvmIRBuilder->GetInsertBlock()->getParent();
                functionID* fnID = getFunctionIDFromFunctionPointer(functionIDs, currentFn);
                bool isSigned = fnID ? typeSigns.count(fnID->returnType) && typeSigns[fnID->returnType] : true;
                RetVal = castValue(RetVal, retType, true, isSigned, this);
                if (wasError)
                    return nullptr;
            }
            llvmIRBuilder->CreateRet(RetVal);
        }
    }

    messageSystem::endBlock();

    messageSystem::endBlock();
    return nullptr;
}

// LLVMValue*
void* ASTNode::generateResult(int pass)
{
    messageSystem::startBlock(this, "Generating result statement", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (resultContextStack.empty()) {
        return messageSystem::error("'result' used outside a value block");
    }

    ASTNode* exprNode = childNodes[0];
    LLVMValue* val = (LLVMValue*)(exprNode->*(exprNode->codegen))(pass);
    if (wasError || !val) {
        messageSystem::endBlock();
        return nullptr;
    }

    ResultContext& ctx = resultContextStack.top();

    // Allocate the result slot in the function entry block the first time we see a result.
    if (!ctx.resultSlot) {
        Function* theFunction = llvmIRBuilder->GetInsertBlock()->getParent();
        IRBuilder<> entryBuilder(&theFunction->getEntryBlock(),
            theFunction->getEntryBlock().begin());
        ctx.resultSlot = entryBuilder.CreateAlloca(val->getType(), nullptr, "result_slot");
    }

    llvmIRBuilder->CreateStore(val, ctx.resultSlot);
    llvmIRBuilder->CreateBr(ctx.mergeBB);

    // Any code after 'result' is unreachable; give LLVM a valid insertion point.
    Function* theFunction = llvmIRBuilder->GetInsertBlock()->getParent();
    BasicBlock* afterResult = BasicBlock::Create(*llvmCompileContext, "after_result", theFunction);
    llvmIRBuilder->SetInsertPoint(afterResult);

    messageSystem::endBlock();
    return nullptr;
}

// LLVMValue*
void* ASTNode::generateExpression(int pass)
{
    messageSystem::startBlock(this, "Generating expression", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (!LexicalBlocks.empty() && token && token->filePath) {
        llvmIRBuilder->SetCurrentDebugLocation(
            DILocation::get(LexicalBlocks.back()->getContext(),
                token->lineNumber + 1,  // Line (DWARF is 1-indexed)
                token->indexInLine,     // Column
                LexicalBlocks.back()));
    }
    if (childNodes.size() == 0)
        return nullptr;
    ASTNode* exprNode = childNodes[0];

    if (lvalue)
        exprNode->lvalue = true;
    LLVMValue* exprVal = (LLVMValue*)(exprNode->*(exprNode->codegen))(pass);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }
    if (exprVal == nullptr) {
        return messageSystem::error("Unable to compile expression");
    }
    // Propagate asaType from child so access operations (c[i]) can resolve element type
    if (!asaType && exprNode->asaType)
        asaType = exprNode->asaType;

    messageSystem::endBlock();
    return exprVal;
}

// LLVMValue*
void* ASTNode::generateIncDecrement(int pass)
{
    messageSystem::startBlock(this, "Generating unary increment/decrement operation", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    ASTNode* operand = childNodes[0];
    LLVMValue* targetPtr = nullptr;
    LLVMType* targetType = nullptr;

    messageSystem::startBlock(operand, "Generating variable", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (operand->nodeType == Identifier_Node) {
        valueType* val = findNamedValue(parentNode, this, operand->token->tokenStr, token);
        if (!val) {
            return messageSystem::error("Unknown variable '" + operand->token->tokenStr + "'");
        }
        if (val->isConstant) {
            return messageSystem::error("Cannot modify const variable '" + operand->token->tokenStr + "'");
        }
        if (val->isUndefined)
            messageSystem::warning("Variable '" + operand->token->tokenStr + "' may be undefined", messageSystem::Undefined_Variable_Warning);
        targetPtr = val->val;
        targetType = getValueStoredType(targetPtr);
        val->isUndefined = false;
    }
    else {
        operand->lvalue = true;
        targetPtr = (LLVMValue*)(operand->*(operand->codegen))(pass);
        if (wasError)
            return nullptr;
        if (!targetPtr || !targetPtr->getType()->isPointerTy()) {
            return messageSystem::error("Operand of ++/-- must be an lvalue");
        }
        targetType = getValueStoredType(targetPtr);
    }

    messageSystem::endBlock();


    LLVMValue* current = llvmIRBuilder->CreateLoad(targetType, targetPtr, "incdec_load");
    LLVMValue* updated = nullptr;
    bool isFloat = targetType->isFloatingPointTy();
    if (token->tokenType == Plus_Plus)
        updated = isFloat ? llvmIRBuilder->CreateFAdd(current, ConstantFP::get(targetType, 1.0), "incr")
                          : llvmIRBuilder->CreateAdd(current, ConstantInt::get(targetType, 1), "incr");
    else if (token->tokenType == Minus_Minus)
        updated = isFloat ? llvmIRBuilder->CreateFSub(current, ConstantFP::get(targetType, 1.0), "decr")
                          : llvmIRBuilder->CreateSub(current, ConstantInt::get(targetType, 1), "decr");
    else
        return messageSystem::error("Unknown operator");
    llvmIRBuilder->CreateStore(updated, targetPtr);


    messageSystem::endBlock();
    return isPostfix ? current : updated;
}

// LLVMValue*
void* ASTNode::generateExpressionStatement(int pass)
{
    messageSystem::startBlock(this, "Generating runtime statement expression", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    ASTNode* leftNode = childNodes[0];
    ASTNode* declarationNode = leftNode->nodeType == Colon_Separator_Node ? leftNode : this;
    ASTNode* exprNode = childNodes[1];
    Function* theFunction = llvmIRBuilder->GetInsertBlock()->getParent();
    ASTNode* initializerNode = unwrapSingleExpressionNode(exprNode);
    bool rhsIsUndefined = initializerNode && initializerNode->nodeType == Undefined_Initializer_Node;
    bool rhsIsDefault = initializerNode && initializerNode->nodeType == Default_Initializer_Node;

    messageSystem::startBlock(exprNode, "Generating expression right side", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Set debug location if available
    if (!LexicalBlocks.empty() && token && token->filePath) {
        llvmIRBuilder->SetCurrentDebugLocation(
            DILocation::get(LexicalBlocks.back()->getContext(),
                token->lineNumber + 1,  // Line (DWARF is 1-indexed)
                0,                      // Column
                LexicalBlocks.back()));
    }

    LLVMValue* exprVal = nullptr;
    if (!rhsIsUndefined && !rhsIsDefault) {
        // Evaluate right side (rvalue)
        exprVal = (LLVMValue*)(exprNode->*(exprNode->codegen))(pass);
        if (wasError) {
            return nullptr;
        }
        if (!exprVal) {
            return messageSystem::error("Set expression requires right argument");
        }
    }

    messageSystem::endBlock();


    messageSystem::startBlock(leftNode, "Generating expression left side", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    LLVMValue* targetPtr = nullptr;
    LLVMType* targetType = nullptr;
    bool targetIsSigned = true;
    valueType* targetValue = nullptr;

    // If the left side is a pointer lvalue
    if (leftNode->nodeType != Identifier_Node && leftNode->nodeType != Colon_Separator_Node) {
        leftNode->lvalue = true;
        // left side is an expression, evaluate to pointer (lvalue address)
        size_t stackDepthBefore = lastRetrievedElementType.size();
        targetPtr = (LLVMValue*)(leftNode->*(leftNode->codegen))(pass);
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
    LLVMType* type = nullptr;
    int pointerLevel = 0;
    bool isConst = false;
    std::string defaultTypeName = "";
    int defaultPointerLevel = 0;
    if (!leftNode->lvalue) {
        if (leftNode->childNodes.size() > 0) {
            typeNode = leftNode->childNodes[1];
        getNextPointerLevel:
            if (typeNode->token->tokenStr == "const") {
                isConst = true;
                typeNode = typeNode->childNodes[0];
                goto getNextPointerLevel;
            }
            if (typeNode->token->tokenStr == "ref" || typeNode->token->tokenStr == "exact") {
                typeNode = typeNode->childNodes[0];
                goto getNextPointerLevel;
            }
            if (typeNode->token->tokenStr == "*") {
                pointerLevel++;
                typeNode = typeNode->childNodes[0];
                goto getNextPointerLevel;
            }
            bool wasDefined = true;
            type = getLLVMTypeFromString(typeNode->token->tokenStr, 0, typeNode->token, wasDefined, pass);
            //if (wasDefined == false)
            //  return nullptr;
            for (int pL = 0; pL < pointerLevel; pL++)
                type = PointerType::get(*llvmCompileContext, 0);
            defaultTypeName = typeNode->token->tokenStr;
            defaultPointerLevel = pointerLevel;
        }
    }
    //else
    //      type = var->getType();

    // x : T = void; zero-initialize x to the declared type, bypassing any cast
    ASTNode* voidCheckNode = unwrapSingleExpressionNode(exprNode);
    bool rhsIsVoid = (voidCheckNode->nodeType == Void_Node);

    if (rhsIsUndefined) {
        if (type == nullptr)
            return messageSystem::error("Cannot infer type from '?'. Use an explicit type annotation.");
    }
    else if (rhsIsDefault) {
        if (type != nullptr) {
            exprVal = generateDefaultValueForType(type, defaultTypeName, defaultPointerLevel, pass, this);
            if (wasError || !exprVal)
                return nullptr;
        }
    }
    else if (rhsIsVoid && type != nullptr) {
        exprVal = Constant::getNullValue(type);
    }
    // Automatically resolve type from expression if not already set
    else if (type == nullptr) {
        type = exprVal->getType();
    }
    // Otherwise, the type is explicit, and builtin types should be cast automatically
    else {
        messageSystem::startBlock(typeNode, "Generating type", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

        std::string typeName = resolveTypeAlias(typeNode->token->tokenStr);
        if (typeSigns.find(typeName) != typeSigns.end())  // If builtin type
            exprVal = castValue(exprVal, type, true, typeSigns[typeName], exprNode);
        else if (structDefinitions.find(typeName) != structDefinitions.end())  // If defined struct
            exprVal = castValue(exprVal, type, true, false, exprNode, true);
        else {
            return messageSystem::error("Unknown type");
        }
        if (wasError || exprVal == nullptr) {
            return messageSystem::error("Unable to automatically cast");
        }

        messageSystem::endBlock();
    }

    // If the left side is a typed identifier, like: `name : int`, then set leftNode equal to just the identifier. This must be a declaration
    bool isDeclaration = false;
    if (leftNode->nodeType == Colon_Separator_Node) {
        if (leftNode->childNodes.size() > 0) {
            leftNode = leftNode->childNodes[0];
            isDeclaration = true;
        }
    }


    // If the left side is an identifier
    bool newConstLocal = false;
    if (leftNode->nodeType == Identifier_Node) {
        // Simple variable: find alloca and use it as targetPtr.
        // If there is an explicit type annotation, this is always a new declaration (shadowing).
        valueType* val = typeNode ? nullptr : findNamedValue(parentNode, this, leftNode->token->tokenStr, token);
        if (wasError)
            return nullptr;
        if (!val) {
            if (!typeNode && findCompilerDefinition(parentNode, leftNode->token->tokenStr)) {
                return messageSystem::error(
                    "Cannot modify compiler constant '" + leftNode->token->tokenStr +
                    "' with '='. Use '::' to redefine it at compile time.");
            }

            // New inferred declaration: `someVar = void;` - void has no type to infer from.
            if (rhsIsVoid && !typeNode)
                return messageSystem::error("Cannot infer type from 'void'. Use an explicit type annotation: 'name : type = void'.", messageSystem::Type_Inference_From_Void_Error);
            if ((rhsIsUndefined || rhsIsDefault) && !typeNode)
                return messageSystem::error("Cannot infer type from this initializer. Use an explicit type annotation.");
            targetPtr = CreateEntryBlockAlloca(theFunction, type, leftNode->token->tokenStr);
            std::string actualType = "*int";
            if (!typeNode) {
                actualType = getStringTypeFromLLVMType(type);
                // Fall back to the declared type string if LLVM type inference fails
                if (actualType.find("unknown") != std::string::npos && exprNode->asaType && !exprNode->asaType->strVal.empty())
                    actualType = exprNode->asaType->strVal;
            }
            else
                actualType = (pointerLevel > 0 ? std::string(pointerLevel, '*') : "") + typeNode->token->tokenStr;
            namedValues[leftNode->token->tokenStr] = new valueType(leftNode->token->tokenStr, actualType, targetPtr);
            namedValues[leftNode->token->tokenStr]->isConstant = isConst;
            namedValues[leftNode->token->tokenStr]->isUndefined = rhsIsUndefined;
            namedValues[leftNode->token->tokenStr]->declNode = declarationNode;
            targetValue = namedValues[leftNode->token->tokenStr];
            newConstLocal = isConst;

            // Add debug info ONLY if we have a valid scope and the stack is not empty
            if (llvmDebugBuilder && !LexicalBlocks.empty() && token && token->filePath) {
                DIScope* Scope = LexicalBlocks.back();
                unsigned LineNo = token->lineNumber + 1;  // DWARF is 1-indexed

                // Create DIType
                DIType* DebugType = createDIType(type, actualType);

                DIFile* VarFile = (token->filePath && !token->filePath->empty())
                                      ? getDIFile(*token->filePath)
                                      : llvmDebugFile;
                if (DebugType) {
                    DILocalVariable* D = llvmDebugBuilder->createAutoVariable(
                        Scope,
                        leftNode->token->tokenStr,
                        VarFile,
                        LineNo,
                        DebugType,
                        true  // AlwaysPreserve
                    );

                    llvmDebugBuilder->insertDeclare(
                        targetPtr,
                        D,
                        llvmDebugBuilder->createExpression(),
                        DILocation::get(Scope->getContext(), LineNo, 0, Scope),
                        llvmIRBuilder->GetInsertBlock());
                }
            }
        }
        else {
            // Check if trying to modify a const variable
            if (val->isConstant) {
                return messageSystem::error("Cannot modify const variable '" + leftNode->token->tokenStr + "'");
            }

            targetPtr = val->val;
            targetValue = val;
            std::string resolvedType = resolveTypeAlias(val->type);
            targetIsSigned = !typeSigns.count(resolvedType) || typeSigns[resolvedType];
            defaultTypeName = val->type;
            defaultPointerLevel = 0;
            while (!defaultTypeName.empty() && defaultTypeName[0] == '*') {
                defaultTypeName = defaultTypeName.substr(1);
                defaultPointerLevel++;
            }

            // If this is a reference, load the pointer before storing through it
            if (val->isReference) {
                targetPtr = llvmIRBuilder->CreateLoad(getValueStoredType(val->val), targetPtr, leftNode->token->tokenStr + "_ref_store_ptr");
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
                    targetType = PointerType::getUnqual(*llvmCompileContext);
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

    if (rhsIsUndefined) {
        messageSystem::endBlock();
        if (targetValue)
            targetValue->isUndefined = true;
        return targetPtr;
    }
    if (rhsIsDefault && !exprVal) {
        if (!targetType)
            return messageSystem::error("Cannot infer type from 'default'. Use an explicit type annotation.");
        exprVal = generateDefaultValueForType(targetType, defaultTypeName, defaultPointerLevel, pass, this);
        if (wasError || !exprVal)
            return nullptr;
    }

    messageSystem::startBlock(this, "Generating compound assignment operation", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Compound assignment: load current value, apply op, then store result
    if (token->tokenType == Plus_Equal || token->tokenType == Minus_Equal ||
        token->tokenType == Times_Equal || token->tokenType == Slash_Equal ||
        token->tokenType == Ampersand_Equal || token->tokenType == Bar_Equal ||
        token->tokenType == Caret_Equal || token->tokenType == Shift_Left_Equal ||
        token->tokenType == Shift_Right_Equal) {
        // Use targetType directly; LLVM may fold GEPs, making instruction introspection unreliable.
        LLVMType* loadType = targetType;
        if (targetValue && targetValue->isUndefined)
            messageSystem::warning("Variable '" + leftNode->token->tokenStr + "' may be undefined", messageSystem::Undefined_Variable_Warning);
        LLVMValue* currentVal = llvmIRBuilder->CreateLoad(loadType, targetPtr, "cmpd_load");

        bool isFloat = loadType->isFloatingPointTy();
        bool isInt = loadType->isIntegerTy();

        bool isBitwise = token->tokenType == Ampersand_Equal || token->tokenType == Bar_Equal ||
                         token->tokenType == Caret_Equal || token->tokenType == Shift_Left_Equal ||
                         token->tokenType == Shift_Right_Equal;
        if (isBitwise && !isInt)
            return messageSystem::error("Bitwise compound assignment requires integer operands");

        if (isFloat || isInt) {
            // Cast RHS to the variable's type so the result stays the same type
            if (exprVal->getType() != loadType) {
                exprVal = castValue(exprVal, loadType, true, false, exprNode);
                if (wasError)
                    return nullptr;
            }
            if (token->tokenType == Plus_Equal)
                exprVal = isFloat ? llvmIRBuilder->CreateFAdd(currentVal, exprVal, "cmpd_add")
                                  : llvmIRBuilder->CreateAdd(currentVal, exprVal, "cmpd_add");
            else if (token->tokenType == Minus_Equal)
                exprVal = isFloat ? llvmIRBuilder->CreateFSub(currentVal, exprVal, "cmpd_sub")
                                  : llvmIRBuilder->CreateSub(currentVal, exprVal, "cmpd_sub");
            else if (token->tokenType == Times_Equal)
                exprVal = isFloat ? llvmIRBuilder->CreateFMul(currentVal, exprVal, "cmpd_mul")
                                  : llvmIRBuilder->CreateMul(currentVal, exprVal, "cmpd_mul");
            else if (token->tokenType == Slash_Equal)
                exprVal = isFloat ? llvmIRBuilder->CreateFDiv(currentVal, exprVal, "cmpd_div")
                                  : llvmIRBuilder->CreateSDiv(currentVal, exprVal, "cmpd_div");
            else if (token->tokenType == Ampersand_Equal)
                exprVal = llvmIRBuilder->CreateAnd(currentVal, exprVal, "cmpd_and");
            else if (token->tokenType == Bar_Equal)
                exprVal = llvmIRBuilder->CreateOr(currentVal, exprVal, "cmpd_or");
            else if (token->tokenType == Caret_Equal)
                exprVal = llvmIRBuilder->CreateXor(currentVal, exprVal, "cmpd_xor");
            else if (token->tokenType == Shift_Left_Equal)
                exprVal = llvmIRBuilder->CreateShl(currentVal, exprVal, "cmpd_shl");
            else if (token->tokenType == Shift_Right_Equal)
                exprVal = targetIsSigned ? llvmIRBuilder->CreateAShr(currentVal, exprVal, "cmpd_ashr")
                                         : llvmIRBuilder->CreateLShr(currentVal, exprVal, "cmpd_lshr");
        }
        else {
            // Non-scalar: delegate to operator overload for custom types
            static const std::unordered_map<TokenType, std::string> compoundOpName = {
                {Plus_Equal, "operator." + tokenAsString(Plus)},
                {Minus_Equal, "operator." + tokenAsString(Minus)},
                {Times_Equal, "operator." + tokenAsString(Star)},
                {Slash_Equal, "operator." + tokenAsString(Slash)},
                {Ampersand_Equal, "operator." + tokenAsString(Ampersand)},
                {Bar_Equal, "operator." + tokenAsString(Bar)},
                {Caret_Equal, "operator." + tokenAsString(Caret)},
                {Shift_Left_Equal, "operator." + tokenAsString(Shift_Left)},
                {Shift_Right_Equal, "operator." + tokenAsString(Shift_Right)},
            };
            std::string operatorName = compoundOpName.at(token->tokenType);
            argumentList argList;
            std::string lTypeStr = getStringTypeFromLLVMType(currentVal->getType());
            argList.push_back(argType(lTypeStr, getASTNodeTypeFromString(lTypeStr), 0));
            std::string rTypeStr = getStringTypeFromLLVMType(exprVal->getType());
            argList.push_back(argType(rTypeStr, getASTNodeTypeFromString(rTypeStr), 0));
            functionID* calleeID = getFunctionFromID(functionIDs, operatorName, argList, true);
            if (!calleeID || !calleeID->fnValue) {
                return messageSystem::error("No operator overload '" + operatorName + "' found for compound assignment");
            }
            calleeID->uses++;
            std::vector<LLVMValue*> ArgsV = {currentVal, exprVal};
            if (calleeID->isStructReturn) {
                auto structIt = structDefinitions.find(calleeID->returnType);
                if (structIt == structDefinitions.end() || !structIt->second->structVal) {
                    return messageSystem::error("Struct return type not found for compound operator overload");
                }
                AllocaInst* sretAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), structIt->second->structVal, "cmpd_sret");
                ArgsV.insert(ArgsV.begin(), sretAlloc);
                llvmIRBuilder->CreateCall(calleeID->fnValue, ArgsV);
                exprVal = llvmIRBuilder->CreateLoad(structIt->second->structVal, sretAlloc, "cmpd_struct");
            }
            else {
                exprVal = llvmIRBuilder->CreateCall(calleeID->fnValue, ArgsV, "cmpd_struct");
            }
        }
    }
    messageSystem::endBlock();


    messageSystem::startBlock(this, "Existing variable assignment", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // If assigning to an existing variable and types differ, cast to the stored type
    if (targetType && exprVal->getType() != targetType) {
        exprVal = castValue(exprVal, targetType, true, false, exprNode);
        if (wasError)
            return nullptr;

        if (exprVal == nullptr)
            return messageSystem::error("Could not implicitly cast expression to variable type.");
    }

    StoreInst* storeInst = llvmIRBuilder->CreateStore(exprVal, targetPtr);
    if (targetValue)
        targetValue->isUndefined = false;

    // For a newly declared const local, tell the optimizer this memory is
    // invariant after initialization so it can treat reads as constants.
    // Only emit invariant.start when in the entry block: non-entry blocks
    // (e.g. after a branch) can cause numbering conflicts in LLVM 21.
    if (newConstLocal && theFunction) {
        BasicBlock* curBB = llvmIRBuilder->GetInsertBlock();
        BasicBlock* entryBB = &theFunction->getEntryBlock();
        if (curBB == entryBB) {
            LLVMType* storedType = exprVal->getType();
            uint64_t typeSize = llvmCompileModule->getDataLayout().getTypeAllocSize(storedType);
            Function* invariantStartFn = Intrinsic::getOrInsertDeclaration(llvmCompileModule.get(), Intrinsic::invariant_start, {PointerType::getUnqual(*llvmCompileContext)});
            llvmIRBuilder->CreateCall(invariantStartFn,
                {ConstantInt::get(LLVMType::getInt64Ty(*llvmCompileContext), typeSize), targetPtr});
        }
    }

    messageSystem::endBlock();


    messageSystem::endBlock();
    return exprVal;
}

// LLVMValue*
void* ASTNode::generateIterator(int pass)
{
    ASTNode* identifierNode = childNodes[0];
    LLVMValue* var = nullptr;
    //LLVMValue* var = (LLVMValue*)(findNamedValue(parentNode, this, token->tokenStr)->val);
    //if (!var) {
    LLVMType* type = llvm::Type::getInt32Ty(*llvmCompileContext);
    var = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), type, identifierNode->token->tokenStr);
    //}
    return var;
}

// LLVMValue*
void* ASTNode::generateUnaryExpression(int pass)
{
    messageSystem::startBlock(this, "Generating unary expression", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (childNodes.size() == 0) {
        return messageSystem::error("Unary expression requires argument");
    }
    if (childNodes[0]->codegen == nullptr) {
        return messageSystem::error("Node `" + ASTNodeTypeAsString(childNodes[0]->nodeType) + "` does not have a code generator");
    }
    LLVMValue* R = (LLVMValue*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
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
                valueType* val = findNamedValue(parentNode, this, child->token->tokenStr, token);
                if (!val && !wasError) {
                    return messageSystem::error("Unknown variable name for address-of");
                }
                messageSystem::endBlock();
                return (LLVMValue*)(val->val);
            }
            // For member access, array subscript, etc.: evaluate as lvalue to get pointer
            child->lvalue = true;
            LLVMValue* ptr = (LLVMValue*)(child->*(child->codegen))(pass);
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
            LLVMValue* ptrVal = (LLVMValue*)(ptrNode->*(ptrNode->codegen))(pass);
            if (wasError) {
                messageSystem::endBlock();
                return nullptr;
            }
            if (!ptrVal) {
                return messageSystem::error("Dereference of null pointer");
            }
            // Determine element type from allocation when available
            LLVMType* elementType = nullptr;
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
                elementType = LLVMType::getInt32Ty(*llvmCompileContext);

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
            return llvmIRBuilder->CreateLoad(elementType, ptrVal, "deref_tmp");
        }

        case Expression_Minus: {
            ASTNode* valueNode = childNodes[0];
            LLVMValue* v = (LLVMValue*)(valueNode->*(valueNode->codegen))(pass);
            if (wasError) {
                messageSystem::endBlock();
                return nullptr;
            }
            if (!v) {
                return messageSystem::error("Cannot take negative of value");
            }
            if (v->getType()->isIntegerTy()) {
                messageSystem::endBlock();
                return llvmIRBuilder->CreateNeg(v, "neg_tmp");
            }
            else if (v->getType()->isFloatingPointTy()) {
                messageSystem::endBlock();
                return llvmIRBuilder->CreateFNeg(v, "fneg_tmp");
            }
            else {
                return messageSystem::error("Cannot take negative of value");
            }
        }

        case Logical_Not: {
            LLVMValue* boolVal = R->getType()->isIntegerTy(1) ? R : llvmIRBuilder->CreateICmpNE(R, Constant::getNullValue(R->getType()), "tobool");
            LLVMValue* result = llvmIRBuilder->CreateNot(boolVal, "not_tmp");
            messageSystem::endBlock();
            return result;
        }

        case Bitwise_Not: {
            if (!R->getType()->isIntegerTy())
                return messageSystem::error("Bitwise complement requires an integer operand");
            LLVMValue* result = llvmIRBuilder->CreateNot(R, "bitnot_tmp");
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

static bool getDeclaredTypeFromColonNode(ASTNode* colonNode, LLVMType*& outType, std::string& outTypeName, int& outPointerLevel, bool& outIsConst, int pass)
{
    if (!colonNode || colonNode->nodeType != Colon_Separator_Node || colonNode->childNodes.size() < 2)
        return false;

    ASTNode* typeNode = colonNode->childNodes[1];
    outPointerLevel = 0;
    outIsConst = false;

getNextPointerLevel:
    if (typeNode->token->tokenStr == "const") {
        outIsConst = true;
        typeNode = typeNode->childNodes[0];
        goto getNextPointerLevel;
    }
    if (typeNode->token->tokenStr == "ref" || typeNode->token->tokenStr == "exact") {
        typeNode = typeNode->childNodes[0];
        goto getNextPointerLevel;
    }
    if (typeNode->token->tokenStr == "*") {
        outPointerLevel++;
        typeNode = typeNode->childNodes[0];
        goto getNextPointerLevel;
    }

    outTypeName = typeNode->token->tokenStr;
    bool wasDefined = true;
    outType = getLLVMTypeFromString(outTypeName, 0, typeNode->token, wasDefined, pass);
    if (!outType || !wasDefined)
        return false;

    for (int pL = 0; pL < outPointerLevel; pL++)
        outType = PointerType::get(*llvmCompileContext, 0);

    return true;
}

static LLVMValue* generateBareDefaultDeclaration(ASTNode* colonNode, int pass)
{
    if (!colonNode || colonNode->childNodes.size() < 2)
        return nullptr;

    ASTNode* nameNode = colonNode->childNodes[0];
    LLVMType* type = nullptr;
    std::string typeName;
    int pointerLevel = 0;
    bool isConst = false;
    if (!getDeclaredTypeFromColonNode(colonNode, type, typeName, pointerLevel, isConst, pass))
        return nullptr;

    Function* theFunction = llvmIRBuilder->GetInsertBlock()->getParent();
    AllocaInst* targetPtr = CreateEntryBlockAlloca(theFunction, type, nameNode->token->tokenStr);
    std::string actualType = (pointerLevel > 0 ? std::string(pointerLevel, '*') : "") + typeName;
    colonNode->namedValues[nameNode->token->tokenStr] = new valueType(nameNode->token->tokenStr, actualType, targetPtr);
    colonNode->namedValues[nameNode->token->tokenStr]->isConstant = isConst;
    colonNode->namedValues[nameNode->token->tokenStr]->isUndefined = false;
    colonNode->namedValues[nameNode->token->tokenStr]->declNode = colonNode;

    LLVMValue* defaultValue = generateDefaultValueForType(type, typeName, pointerLevel, pass, colonNode);
    if (wasError || !defaultValue)
        return nullptr;
    llvmIRBuilder->CreateStore(defaultValue, targetPtr);
    return defaultValue;
}

// LLVMValue*
void* ASTNode::generateBinaryExpression(int pass)
{
    messageSystem::startBlock(this, "Generating binary expression", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (nodeType == Colon_Separator_Node) {
        LLVMValue* defaultValue = generateBareDefaultDeclaration(this, pass);
        messageSystem::endBlock();
        return defaultValue;
    }

    if (!LexicalBlocks.empty() && token && token->filePath)
        llvmIRBuilder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));
    if (childNodes.size() < 2) {
        return messageSystem::error("Binary expression requires left and right arguments");
    }

    if (!childNodes[0]->codegen || !childNodes[1]->codegen) {
        return messageSystem::error("Binary expression operands missing code generators");
    }

    // Check if it is the pipe operator first
    if (nodeType == Pipe_Operation) {
        // add L to stack
        LLVMValue* L = (LLVMValue*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
        if (wasError) {
            messageSystem::endBlock();
            return nullptr;
        }
        pipeOperationValue.push(L);
        // then process R
        LLVMValue* R = (LLVMValue*)(childNodes[1]->*(childNodes[1]->codegen))(pass);
        if (wasError) {
            messageSystem::endBlock();
            return nullptr;
        }
        // pop stack
        pipeOperationValue.pop();

        messageSystem::endBlock();
        return R;
    }

    if (nodeType == Logical_And || nodeType == Logical_Or) {
        auto toBool = [&](LLVMValue* V, const std::string& name) -> LLVMValue* {
            return V->getType()->isIntegerTy(1) ? V : llvmIRBuilder->CreateICmpNE(V, Constant::getNullValue(V->getType()), name);
        };

        LLVMValue* L = (LLVMValue*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
        if (wasError) {
            messageSystem::endBlock();
            return nullptr;
        }
        if (!L) {
            return messageSystem::error("Error generating left side of logical expression");
        }

        LLVMValue* lBool = toBool(L, "tobool_l");
        Function* TheFunction = llvmIRBuilder->GetInsertBlock()->getParent();
        BasicBlock* LhsBB = llvmIRBuilder->GetInsertBlock();
        BasicBlock* RhsBB = BasicBlock::Create(*llvmCompileContext, nodeType == Logical_And ? "land.rhs" : "lor.rhs", TheFunction);
        BasicBlock* MergeBB = BasicBlock::Create(*llvmCompileContext, nodeType == Logical_And ? "land.end" : "lor.end");

        if (nodeType == Logical_And)
            llvmIRBuilder->CreateCondBr(lBool, RhsBB, MergeBB);
        else
            llvmIRBuilder->CreateCondBr(lBool, MergeBB, RhsBB);

        llvmIRBuilder->SetInsertPoint(RhsBB);
        LLVMValue* R = (LLVMValue*)(childNodes[1]->*(childNodes[1]->codegen))(pass);
        if (wasError) {
            messageSystem::endBlock();
            return nullptr;
        }
        if (!R) {
            return messageSystem::error("Error generating right side of logical expression");
        }

        LLVMValue* rBool = toBool(R, "tobool_r");
        llvmIRBuilder->CreateBr(MergeBB);
        BasicBlock* RhsEvalBB = llvmIRBuilder->GetInsertBlock();

        TheFunction->insert(TheFunction->end(), MergeBB);
        llvmIRBuilder->SetInsertPoint(MergeBB);

        PHINode* Phi = llvmIRBuilder->CreatePHI(LLVMType::getInt1Ty(*llvmCompileContext), 2, nodeType == Logical_And ? "and_tmp" : "or_tmp");
        if (nodeType == Logical_And) {
            Phi->addIncoming(ConstantInt::getFalse(*llvmCompileContext), LhsBB);
            Phi->addIncoming(rBool, RhsEvalBB);
        }
        else {
            Phi->addIncoming(ConstantInt::getTrue(*llvmCompileContext), LhsBB);
            Phi->addIncoming(rBool, RhsEvalBB);
        }
        messageSystem::endBlock();
        return Phi;
    }

    LLVMValue* L = (LLVMValue*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
    LLVMValue* R = (LLVMValue*)(childNodes[1]->*(childNodes[1]->codegen))(pass);

    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }
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
        castToHighestAccuracy(L, R, this);
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

LLVMValue* ASTNode::generateIntegerBinaryOp(LLVMValue* L, LLVMValue* R)
{
    messageSystem::startBlock(this, "Generating integer binary operation", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Check for comparison operations first
    auto compIt = intCompareOps.find(nodeType);
    if (compIt != intCompareOps.end()) {
        messageSystem::endBlock();
        return llvmIRBuilder->CreateICmp(compIt->second, L, R, "icmp_tmp");
    }

    // Regular arithmetic/bitwise operations
    auto opIt = integerOps.find(nodeType);
    if (opIt != integerOps.end()) {
        messageSystem::endBlock();
        return llvmIRBuilder->CreateBinOp(opIt->second, L, R, "int_op");
    }

    if (nodeType == Bitwise_Shift_Right) {
        bool isSigned = true;
        if (childNodes[0]->asaType && !childNodes[0]->asaType->strVal.empty())
            isSigned = isSignedType(resolveTypeAlias(childNodes[0]->asaType->strVal));
        messageSystem::endBlock();
        return isSigned ? llvmIRBuilder->CreateAShr(L, R, "ashr_tmp")
                        : llvmIRBuilder->CreateLShr(L, R, "lshr_tmp");
    }

    return (LLVMValue*)messageSystem::error("Unknown integer binary operator");
}

LLVMValue* ASTNode::generateFloatBinaryOp(LLVMValue* L, LLVMValue* R)
{
    messageSystem::startBlock(this, "Generating float binary operation", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Check for comparison operations first
    auto compIt = floatCompareOps.find(nodeType);
    if (compIt != floatCompareOps.end()) {
        messageSystem::endBlock();
        return llvmIRBuilder->CreateFCmp(compIt->second, L, R, "fcmp_tmp");
    }

    // Regular arithmetic operations
    auto opIt = floatOps.find(nodeType);
    if (opIt != floatOps.end()) {
        messageSystem::endBlock();
        return llvmIRBuilder->CreateBinOp(opIt->second, L, R, "float_op");
    }

    return (LLVMValue*)messageSystem::error("Unknown float binary operator");
}

LLVMValue* ASTNode::generatePointerBinaryOp(LLVMValue* L, LLVMValue* R)
{
    messageSystem::startBlock(this, "Generating pointer binary operation", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Pointer comparisons (== and !=, e.g. ptr == void / ptr != void)
    auto compIt = intCompareOps.find(nodeType);
    if (compIt != intCompareOps.end()) {
        messageSystem::endBlock();
        return llvmIRBuilder->CreateICmp(compIt->second, L, R, "ptr_cmp");
    }

    return (LLVMValue*)messageSystem::error("Invalid pointer operation");
}

void* ASTNode::generatePipePlaceholder(int pass)
{
    messageSystem::startBlock(this, "Generating pipe operation placeholder", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (pipeOperationValue.size() > 0) {
        messageSystem::endBlock();
        return pipeOperationValue.top();
    }

    return messageSystem::error("Pipe operation placeholder '$' can only be used after a pipe operation");
}

// Get the type string for one operand of a binary expression.
// Uses AST identifier/member-access info when available, so that types like
// char vs uint8 (both i8 in LLVM) are resolved correctly.
static std::string getOperandTypeString(ASTNode* binaryNode, int childIdx, LLVMValue* llvmVal)
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
            valueType* val = findNamedValue(binaryNode->parentNode, binaryNode, inner->token->tokenStr, binaryNode->token);
            if (val && !val->type.empty())
                return val->type;
        }
    }
    return getStringTypeFromLLVMType(llvmVal->getType());
}

// TODO: Broken operator overload
bool ASTNode::checkForOperatorOverload(LLVMValue* L, LLVMValue* R)
{
    // Operator overloads have the format: operator.<OperatorName>
    //                             like: operator.Add(string, string)
    //                             for:  "h" + "i"
    std::string operatorName = "operator." + tokenAsString(token->tokenType);

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

    functionID* calleeID = getFunctionFromID(functionIDs, operatorName, argList, true);
    if (calleeID != nullptr && calleeID->fnValue != nullptr)
        return true;
    else {
        if (verbosity >= 6) {
            if (calleeID == nullptr)
                console::writeLine("No calleeID");
            else if (calleeID->fnValue == nullptr)
                console::writeLine("No calleeID->fnValue");
        }
        return false;
    }
}

LLVMValue* ASTNode::generateOperatorOverloadCall(LLVMValue* L, LLVMValue* R)
{
    messageSystem::startBlock(this, "Generating operator overload call", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::string operatorName = "operator." + tokenAsString(token->tokenType);

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

    functionID* calleeID = getFunctionFromID(functionIDs, operatorName, argList, true);

    if (!calleeID || !calleeID->fnValue) {
        return (LLVMValue*)messageSystem::error("Expected operator overload for undefined operator `" + tokenAsString(token->tokenType) + "`, but none were not found");
    }

    calleeID->uses++;

    // Implicit numeric coercion: cast each argument to the formal parameter type
    std::vector<LLVMValue*> ArgsV = {L, R};
    for (int i = 0; i < 2; i++) {
        int formalIdx = i + (calleeID->isStructReturn ? 1 : 0);
        if (formalIdx < (int)calleeID->fnValue->getFunctionType()->getNumParams()) {
            LLVMType* formalType = calleeID->fnValue->getFunctionType()->getParamType(formalIdx);
            if (formalType && ArgsV[i]->getType() != formalType &&
                !ArgsV[i]->getType()->isStructTy() && !ArgsV[i]->getType()->isPointerTy() &&
                !formalType->isStructTy() && !formalType->isPointerTy()) {
                ArgsV[i] = castValue(ArgsV[i], formalType, true, false, this);
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
            return (LLVMValue*)messageSystem::error("Struct return type not defined for operator overload");
        }
        AllocaInst* sretAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), structIt->second->structVal, "op_sret");
        ArgsV.insert(ArgsV.begin(), sretAlloc);
        llvmIRBuilder->CreateCall(calleeID->fnValue, ArgsV);

        messageSystem::endBlock();
        return llvmIRBuilder->CreateLoad(structIt->second->structVal, sretAlloc, "op_overload");
    }

    messageSystem::endBlock();
    return llvmIRBuilder->CreateCall(calleeID->fnValue, ArgsV, "op_overload");
}

// LLVMValue*
void* ASTNode::generateAccessOperation(int pass)
{
    messageSystem::startBlock(this, "Generating access operation", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (childNodes.size() == 0) {
        return messageSystem::error("Access operation requires a left and right argument");
    }

    childNodes[0]->lvalue = true;  // Set flag for base to return address if needed
    LLVMValue* L = (LLVMValue*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }
    LLVMValue* R = (LLVMValue*)(childNodes[1]->*(childNodes[1]->codegen))(pass);
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

    // Inherit asaType from child if not already set, or if child has pointer type and current
    // asaType is stale (cross-pass contamination: pass-1 sets baseLLVMType to the element struct
    // type; pass-2 then re-enters and sees a struct, not a pointer, taking the wrong subscript path).
    if (childNodes[0]->asaType) {
        bool childHasPointer = childNodes[0]->asaType->baseLLVMType &&
                               childNodes[0]->asaType->baseLLVMType->isPointerTy();
        bool currentIsStaleStruct = asaType && asaType->baseLLVMType &&
                                    asaType->baseLLVMType->isStructTy();
        if (!asaType || (childHasPointer && currentIsStaleStruct))
            asaType = childNodes[0]->asaType;
    }

    if (!asaType) {
        return messageSystem::error("Was unable to resolve type of base being accessed");
    }

    // Get the element type from baseType (set by member access or previous operations)
    LLVMType* elementType = asaType->baseLLVMType;
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
                        LLVMType* resolved = getLLVMTypeFromString(member.typeString, 0, token, wd, resolvePass);
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
            LLVMType* resolved = getLLVMTypeFromString(elementTypeStr, 0, token, wd, resolvePass);
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
    // If baseType is a struct, check for operator[] overload first, then fall back to implicit pointer member
    else if (asaType->baseLLVMType && asaType->baseLLVMType->isStructTy()) {
        // Check for operator[] overload
        std::string opName = "operator." + tokenAsString(Both_Brackets);
        argumentList opArgList;
        std::string lTypeStr = asaType->strVal;
        opArgList.push_back(argType(lTypeStr, getASTNodeTypeFromString(lTypeStr), 0));
        std::string rTypeStr = getOperandTypeString(this, 1, R);
        uint8_t rPointerLevel = 0;
        std::string rBaseTypeStr = rTypeStr;
        while (!rBaseTypeStr.empty() && rBaseTypeStr[0] == '*') {
            rPointerLevel++;
            rBaseTypeStr = rBaseTypeStr.substr(1);
        }
        opArgList.push_back(argType(rBaseTypeStr, getASTNodeTypeFromString(rBaseTypeStr), rPointerLevel));

        functionID* opCalleeID = getFunctionFromID(functionIDs, opName, opArgList, true);
        if (opCalleeID && opCalleeID->fnValue) {
            opCalleeID->uses++;
            // Pass struct by value or by pointer depending on the formal parameter type
            int firstArgIdx = opCalleeID->isStructReturn ? 1 : 0;
            LLVMValue* lArg = L;
            if (firstArgIdx < (int)opCalleeID->fnValue->getFunctionType()->getNumParams()) {
                LLVMType* formalType0 = opCalleeID->fnValue->getFunctionType()->getParamType(firstArgIdx);
                if (formalType0 && formalType0->isStructTy())
                    lArg = llvmIRBuilder->CreateLoad(asaType->baseLLVMType, L, "struct_load");
            }
            std::vector<LLVMValue*> ArgsV = {lArg, R};
            if (opCalleeID->isStructReturn) {
                auto sIt = structDefinitions.find(opCalleeID->returnType);
                if (sIt == structDefinitions.end() || !sIt->second->structVal) {
                    return messageSystem::error("Struct return type not defined for operator[] overload");
                }
                AllocaInst* sretAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), sIt->second->structVal, "idx_sret");
                ArgsV.insert(ArgsV.begin(), sretAlloc);
                llvmIRBuilder->CreateCall(opCalleeID->fnValue, ArgsV);
                messageSystem::endBlock();
                return llvmIRBuilder->CreateLoad(sIt->second->structVal, sretAlloc, "idx_overload");
            }
            messageSystem::endBlock();
            return llvmIRBuilder->CreateCall(opCalleeID->fnValue, ArgsV, "idx_overload");
        }

        // No overload: try implicit pointer-member access (for string-like types)
        bool foundPointerMember = false;
        auto structIt = structDefinitions.find(asaType->strVal);
        if (structIt != structDefinitions.end() && structIt->second) {
            for (const auto& member : structIt->second->members) {
                if (member.pointerLevel > 0) {
                    bool wd = true;
                    int resolvePass = 2;
                    LLVMType* resolved = getLLVMTypeFromString(member.typeString, 0, token, wd, resolvePass);
                    if (resolved) {
                        elementType = resolved;
                        foundPointerMember = true;
                    }
                    break;
                }
            }
        }
        if (!foundPointerMember) {
            return messageSystem::error("operator[] is not defined for type '" + asaType->strVal + "'");
        }
    }

    // Determine if we need to load the pointer first
    // L is a pointer-to-pointer when it comes from a struct member (alloca of pointer)
    // L is a direct pointer when it's a function parameter
    LLVMValue* actualPtr = L;
    LLVMType* ptrType = PointerType::getUnqual(*llvmCompileContext);

    // Check if L is an alloca instruction or a pointer to a pointer
    // In that case, we need to load the actual pointer value
    if (AllocaInst* allocaInst = dyn_cast<AllocaInst>(L)) {
        actualPtr = llvmIRBuilder->CreateLoad(ptrType, L, "ptr_deref");
        if (isRefToStruct)
            actualPtr = llvmIRBuilder->CreateLoad(ptrType, actualPtr, "ref_struct_deref");
        else if (asaType->isRef)
            actualPtr = llvmIRBuilder->CreateLoad(ptrType, actualPtr, "ref_ptr_deref");
    }
    else if (GlobalVariable* gv = dyn_cast<GlobalVariable>(L)) {
        // Global pointer variables need a load to get the heap pointer
        if (!isRefToStruct)
            actualPtr = llvmIRBuilder->CreateLoad(ptrType, L, "global_ptr_deref");
    }
    else if (L->getType()->isPointerTy()) {
        // Check if this is coming from a struct member access (GEP instruction)
        // by checking if the last operation was a struct GEP
        if (GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(L)) {
            // This is a GEP from struct member access, load the pointer
            actualPtr = llvmIRBuilder->CreateLoad(ptrType, L, "ptr_deref");
        }
        // Otherwise, L is already a direct pointer (e.g., function parameter)
        // so we use it as-is
    }

    // Create GEP instruction
    LLVMValue* gep = llvmIRBuilder->CreateGEP(elementType, actualPtr, R, "arrayidx");

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
    return llvmIRBuilder->CreateLoad(elementType, gep, "accessop_load");
}

// LLVMValue*
void* ASTNode::generateMemberAccess(int pass)
{
    messageSystem::startBlock(this, "Generating member access operation", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (childNodes.size() == 0) {
        return messageSystem::error("Member access operation requires a left and right argument");
    }
    // Module member access: resolve via module registry, not struct GEP.
    // Handles both single-level (Mod.member) and chained (Mod.Sub.member) access.
    {
        std::string leftModType;
        if (childNodes[0]->nodeType == Identifier_Node) {
            auto modIt = moduleRegistry.find(childNodes[0]->token->tokenStr);
            if (modIt != moduleRegistry.end())
                leftModType = "__module__:" + childNodes[0]->token->tokenStr;
        }
        else if (childNodes[0]->nodeType == Member_Access) {
            // Use type resolution to detect chained module access (e.g. Nested.Inner)
            bool savedError = wasError;
            leftModType = getMemberAccessTypeString(childNodes[0], parentNode, token);
            if (wasError && leftModType.empty()) {
                wasError = savedError;  // Reset error if not a module chain
                leftModType = "";
            }
        }
        if (!leftModType.empty() && leftModType.size() > 11 && leftModType.substr(0, 11) == "__module__:") {
            std::string modName = leftModType.substr(11);
            std::string memberName = childNodes[1]->token->tokenStr;
            auto modIt = moduleRegistry.find(modName);
            if (modIt != moduleRegistry.end()) {
                ASTNode* modNode = modIt->second;
                auto varIt = modNode->namedValues.find(memberName);
                if (varIt != modNode->namedValues.end()) {
                    valueType* vt = varIt->second;
                    LLVMValue* gv = vt->val;
                    LLVMType* gvType = getValueStoredType(gv);
                    asaType = new ASAType(gvType);
                    asaType->strVal = vt->type;
                    lastRetrievedElementType.push(asaType);
                    messageSystem::endBlock();
                    if (lvalue)
                        return gv;
                    return llvmIRBuilder->CreateLoad(gvType, gv, memberName + "_load");
                }
                // Search for a compile-time define (e.g. `ABC :: 5;`) in the module's scope bodies.
                ASTNode* defineNode = findModuleCompilerDefine(modNode, memberName);
                if (defineNode) {
                    std::string defTypeStr = getDefineTypeString(defineNode);
                    messageSystem::endBlock();
                    LLVMValue* defVal = (LLVMValue*)(defineNode->*(defineNode->codegen))(pass);
                    if (wasError) {
                        messageSystem::endBlock();
                        return nullptr;
                    }
                    if (defVal) {
                        asaType = new ASAType(defVal->getType());
                        asaType->strVal = defTypeStr;
                        lastRetrievedElementType.push(asaType);
                    }
                    return defVal;
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
    LLVMValue* L = (LLVMValue*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }

    // Evaluate base pointer
    LLVMValue* basePtr = L;
    if (!basePtr || !basePtr->getType()->isPointerTy()) {
        // A member function returning a struct by value gives us a raw struct Value, not a pointer.
        // Store it into a temporary alloca so we have an addressable pointer for GEP/member calls.
        if (basePtr && basePtr->getType()->isStructTy()) {
            StructType* sty = cast<StructType>(basePtr->getType());
            AllocaInst* tmp = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), sty, "struct_tmp");
            llvmIRBuilder->CreateStore(basePtr, tmp);
            basePtr = tmp;
            // Push the LLVM struct type so the else branch below can look up the struct definition.
            lastRetrievedElementType.push(new ASAType(sty, false, false, "", 0));
        }
        else {
            return messageSystem::error("Base must be a pointer for access");
        }
    }

    if (childNodes[0]->nodeType == Identifier_Node) {
        valueType* v = findNamedValue(this, nullptr, childNodes[0]->token->tokenStr, token);

        if (!v && !wasError) {
            return messageSystem::error("Unknown variable name used");
        }

        // Resolve through pointer indirection if needed
        std::string AsaStructName = v->type;
        {
            int ptrDepth = 0;
            while (!AsaStructName.empty() && AsaStructName[0] == '*') {
                AsaStructName = AsaStructName.substr(1);
                ptrDepth++;
            }
            if (ptrDepth > 0 && structDefinitions.count(resolveTypeAlias(AsaStructName))) {
                for (int i = 0; i < ptrDepth; i++)
                    basePtr = llvmIRBuilder->CreateLoad(PointerType::getUnqual(*llvmCompileContext), basePtr, "ptr_deref");
            }
            else {
                AsaStructName = v->type;
            }
            AsaStructName = resolveTypeAlias(AsaStructName);
        }

        if (structDefinitions.find(AsaStructName) == structDefinitions.end()) {
            return messageSystem::error("Type \"" + v->type + "\" has not been defined");
        }
        AsaStruct* structDefinition = structDefinitions[AsaStructName];

        // If the struct body hasn't been generated yet, generate it
        if (structDefinition->structVal == nullptr)
            LLVMValue* argVal = (LLVMValue*)(structDefinition->sourceNode->*(structDefinition->sourceNode->codegen))(pass);
        if (wasError) {
            messageSystem::endBlock();
            return nullptr;
        }

        // Get member name and index
        std::string memberName = childNodes[1]->token->tokenStr;
        // Handle if member variable access/set
        if (childNodes[1]->nodeType == Identifier_Node) {
            if (structDefinition->memberNameIndexes.find(memberName) == structDefinition->memberNameIndexes.end()) {
                for (auto& m : structDefinition->memberNameTypeNodes)
                    messageSystem::addAttribute(m.second);

                return messageSystem::error("Struct definition \"" + AsaStructName + "\" does not contain member \"" + memberName + "\"", messageSystem::Undefined_Member_Error);

                //printTokenError(getASTTokenRange(childNodes[1]), "Struct definition \"" + AsaStructName + "\" does not contain member \"" + memberName + "\"");
            }


            uint16_t memberIndex = structDefinition->memberNameIndexes[memberName];

            bool wasDefined = true;
            LLVMType* elementType = getLLVMTypeFromString(structDefinition->members[memberIndex].typeString, 0, token, wasDefined, pass);
            // Apply the stored pointer level for this member (fixes pointer members like *char -> i8*)
            for (int _p = 0; _p < structDefinition->members[memberIndex].pointerLevel; ++_p) {
                elementType = PointerType::get(*llvmCompileContext, 0);
            }
            //if (wasDefined == false)
            //  return nullptr;
            //Type* elementType = getLLVMTypeFromString(v->type, -1, childNodes[0]->token);
            //Type* elementType = getLLVMTypeFromString(baseType);
            if (!elementType) {
                return messageSystem::error("Invalid element type");
            }

            if (structDefinition->members[memberIndex].isConstant && lvalue) {
                return messageSystem::error("Cannot modify const member '" + memberName + "'");
            }


            //// Create GEP to compute the address
            //LLVMValue* gep = llvmIRBuilder->CreateGEP(elementType, basePtr, index, "arrayidx");

            if (v->isReference)
                basePtr = llvmIRBuilder->CreateLoad(PointerType::getUnqual(*llvmCompileContext), basePtr, "ref_struct_ptr");
            auto gep = llvmIRBuilder->CreateStructGEP(structDefinition->structVal, basePtr, memberIndex, "struct_member");
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
                return llvmIRBuilder->CreateLoad(elementType, gep, "member_load");
        }
        // Handle if member function call
        else if (childNodes[1]->nodeType == Function_Call) {
            memberName = structDefinition->name + "." + memberName;
            childNodes[1]->token->tokenStr = memberName;

            ASTNode* argsNode = childNodes[1]->childNodes[0];
            std::vector<ASTNode*> args = std::vector<ASTNode*>();
            for (auto& a : argsNode->childNodes)
                if (a->childNodes.size() > 0) {
                    args.push_back(a);
                }

            std::vector<LLVMValue*> ArgsV = std::vector<LLVMValue*>();
            argumentList argList = argumentList();
            // 'this' goes in ArgsV (LLVM call) but NOT in argList (lookup uses userArguments which excludes 'this')
            ArgsV.push_back(basePtr);
            if (!ArgsV.back()) {
                messageSystem::endBlock();
                return nullptr;
            }
            // Add rest of argument values
            for (int i = 0; i < args.size(); i++) {
                LLVMValue* argVal = (LLVMValue*)(args[i]->*(args[i]->codegen))(pass);
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
            functionID* CalleeFID = getFunctionFromID(structDefinition->memberFunctions, memberName, argList, true, true);
            if (!CalleeFID) {
                for (auto& f : structDefinition->memberFunctions) {
                    if (f->name != memberName)
                        continue;
                    messageSystem::addAttribute(f->declNode);
                }

                return messageSystem::error("Struct definition does not contain member function \"" + memberName + "\"", messageSystem::Undefined_Member_Error);
            }

            // Call function
            Function* CalleeF = CalleeFID->fnValue;
            CalleeFID->uses++;
            isCallMemberFunction = true;

            // Handle sret for struct-returning member functions
            bool memberIsStructReturn = CalleeFID->isStructReturn;
            AllocaInst* memberSretAlloc = nullptr;
            if (memberIsStructReturn) {
                AsaStruct* retStruct = structDefinitions[CalleeFID->returnType];
                if (!retStruct || retStruct->structVal == nullptr) {
                    return messageSystem::error("Struct return type not fully defined");
                }
                memberSretAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), retStruct->structVal, "member_sret");
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
            ArgsV = std::vector<LLVMValue*>();
            ArgsV.push_back(basePtr);
            if (!ArgsV.back()) {
                messageSystem::endBlock();
                return nullptr;
            }
            // Add rest of argument values
            for (int i = 0; i < args.size(); i++) {
                if (CalleeFID->userArguments[i].isReference) {
                    const argType& fa = CalleeFID->userArguments[i];
                    if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
                        if (!fa.isConstant) {
                            messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                            return messageSystem::error("Cannot pass value as reference");
                        }
                        // const ref: auto-materialize the rvalue into a temporary alloca
                        messageSystem::startBlock(args[i], "Generating const ref argument (auto-materialize)", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                        LLVMValue* tmpVal = (LLVMValue*)(args[i]->*(args[i]->codegen))(pass);
                        if (wasError) {
                            messageSystem::endBlock();
                            return nullptr;
                        }
                        if (!tmpVal) {
                            messageSystem::endBlock();
                            return nullptr;
                        }
                        bool wasDef = true;
                        LLVMType* formalType = getLLVMTypeFromString(fa.typeString, fa.pointerLevel, token, wasDef, pass);
                        if (!formalType) {
                            return messageSystem::error("Cannot determine type for const ref materialization: " + fa.typeString);
                        }
                        AllocaInst* tmpAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), formalType, "constref_tmp");
                        llvmIRBuilder->CreateStore(tmpVal, tmpAlloc);
                        messageSystem::endBlock();
                        ArgsV.push_back(tmpAlloc);
                        if (!ArgsV.back()) {
                            return nullptr;
                        }
                        continue;
                    }
                    // Check that the argument type matches the ref parameter type
                    {
                        ASTNode* identNode = args[i]->childNodes[0];
                        valueType* argVar = findNamedValue(parentNode, this, identNode->token->tokenStr, token);
                        if (argVar) {
                            LLVMType* actualType = getValueStoredType(argVar->val);
                            bool wasDef = true;
                            LLVMType* formalType = getLLVMTypeFromString(fa.typeString, 0, token, wasDef, pass);
                            if (formalType && actualType && !actualType->isPointerTy() && !formalType->isPointerTy() && actualType != formalType) {
                                messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                                return messageSystem::error("Cannot pass '" + getStringTypeFromLLVMType(actualType) +
                                                            "' as 'ref " + fa.typeString + "': implicit cast to reference is not allowed");
                            }
                        }
                    }
                    args[i]->childNodes[0]->isRef = true;
                }
                LLVMValue* argVal = (LLVMValue*)(args[i]->*(args[i]->codegen))(pass);
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

            LLVMValue* callResult = nullptr;
            LLVMType* retType = CalleeF->getReturnType();

            // Create the call
            if (retType->isVoidTy())
                llvmIRBuilder->CreateCall(CalleeF, ArgsV);
            else
                callResult = llvmIRBuilder->CreateCall(CalleeF, ArgsV, "calltmp");

            messageSystem::endBlock();
            if (memberIsStructReturn) {
                AsaStruct* retStruct = structDefinitions[CalleeFID->returnType];
                return llvmIRBuilder->CreateLoad(retStruct->structVal, memberSretAlloc, "member_sret_load");
            }
            return callResult;
        }
    }
    // If left is not pointer, assume another member access or index operator
    else {
        AsaStruct* structDefinition = nullptr;

        if (lastRetrievedElementType.size() > stackDepthBefore) {
            // Left child pushed a type (e.g. chained member access) -- use the most recent one
            ASAType* poppedType = lastRetrievedElementType.top();
            lastRetrievedElementType.pop();
            // Drain any other residuals left by nested sub-expressions (e.g. a.b[0].c leaves
            // both the pointer-member push from .b AND the subscript push from [0] on the stack;
            // we only want the top one, so discard the rest down to the recorded depth).
            while (lastRetrievedElementType.size() > stackDepthBefore)
                lastRetrievedElementType.pop();
            // Prefer strVal name lookup (reliable even with opaque LLVM pointers)
            if (!poppedType->strVal.empty()) {
                std::string typeName = poppedType->strVal;
                int ptrDepth = 0;
                while (!typeName.empty() && typeName[0] == '*') {
                    typeName = typeName.substr(1);
                    ptrDepth++;
                }
                // Load through each pointer level to reach the struct pointer
                for (int i = 0; i < ptrDepth; i++)
                    basePtr = llvmIRBuilder->CreateLoad(PointerType::getUnqual(*llvmCompileContext), basePtr, "elem_ptr_deref");
                auto sdIt = structDefinitions.find(typeName);
                if (sdIt != structDefinitions.end())
                    structDefinition = sdIt->second;
            }
            // Fall back to LLVM type comparison if strVal lookup failed
            if (!structDefinition)
                structDefinition = getStructTypeFromLLVMType(poppedType->baseLLVMType);
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
                basePtr = llvmIRBuilder->CreateLoad(PointerType::getUnqual(*llvmCompileContext), basePtr, "call_ptr_deref");
            auto sdIt = structDefinitions.find(typeName);
            if (sdIt != structDefinitions.end())
                structDefinition = sdIt->second;
        }

        if (structDefinition == nullptr) {
            messageSystem::startBlock(childNodes[0], "Generating struct access", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
            return messageSystem::error("Struct could not be found");
        }

        // Get member name and index
        std::string memberName = childNodes[1]->token->tokenStr;
        // Handle if member variable access/set
        if (childNodes[1]->nodeType == Identifier_Node) {
            if (structDefinition->memberNameIndexes.find(memberName) == structDefinition->memberNameIndexes.end()) {
                for (auto& m : structDefinition->memberNameTypeNodes)
                    messageSystem::addAttribute(m.second);

                return messageSystem::error("Struct definition \"" + structDefinition->name + "\" does not contain member \"" + memberName + "\"");

                //printTokenError(getASTTokenRange(childNodes[1]), "Struct definition \"" + structDefinition->name + "\" does not contain member \"" + memberName + "\"");
            }


            uint16_t memberIndex = structDefinition->memberNameIndexes[memberName];

            bool wasDefined = true;
            LLVMType* elementType = getLLVMTypeFromString(structDefinition->members[memberIndex].typeString, 0, token, wasDefined, pass);
            // Apply the stored pointer level for this member (fixes pointer members like *MaterialMap)
            for (int _p = 0; _p < structDefinition->members[memberIndex].pointerLevel; ++_p) {
                elementType = PointerType::get(*llvmCompileContext, 0);
            }
            //if (wasDefined == false)
            //  return nullptr;
            //Type* elementType = getLLVMTypeFromString(v->type, -1, childNodes[0]->token);
            //Type* elementType = getLLVMTypeFromString(baseType);
            if (!elementType) {
                return messageSystem::error("Invalid element type");
            }

            //// Create GEP to compute the address
            //LLVMValue* gep = llvmIRBuilder->CreateGEP(elementType, basePtr, index, "arrayidx");

            auto gep = llvmIRBuilder->CreateStructGEP(structDefinition->structVal, basePtr, memberIndex, "struct_member");
            std::string memberStrVal = "";
            for (int _p = 0; _p < structDefinition->members[memberIndex].pointerLevel; ++_p)
                memberStrVal += "*";
            memberStrVal += structDefinition->members[memberIndex].typeString;
            asaType = new ASAType(elementType, false, structDefinition->members[memberIndex].isConstant, memberStrVal, (uint8_t)structDefinition->members[memberIndex].pointerLevel);

            lastRetrievedElementType.push(asaType);

            messageSystem::endBlock();

            // If this is an lvalue (for assignment), return the pointer gep
            if (lvalue)
                return gep;
            // If rvalue, return value
            else
                return llvmIRBuilder->CreateLoad(elementType, gep, "member_load");
        }
        // Handle if member function call
        else if (childNodes[1]->nodeType == Function_Call) {
            memberName = structDefinition->name + "." + memberName;
            childNodes[1]->token->tokenStr = memberName;

            ASTNode* argsNode = childNodes[1]->childNodes[0];
            std::vector<ASTNode*> args = std::vector<ASTNode*>();
            for (auto& a : argsNode->childNodes)
                if (a->childNodes.size() > 0) {
                    args.push_back(a);
                }

            std::vector<LLVMValue*> ArgsV = std::vector<LLVMValue*>();
            argumentList argList = argumentList();
            // 'this' goes in ArgsV (LLVM call) but NOT in argList (lookup uses userArguments which excludes 'this')
            ArgsV.push_back(basePtr);
            if (!ArgsV.back()) {
                messageSystem::endBlock();
                return nullptr;
            }
            // Add rest of argument values
            for (int i = 0; i < args.size(); i++) {
                LLVMValue* argVal = (LLVMValue*)(args[i]->*(args[i]->codegen))(pass);
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
            functionID* CalleeFID = getFunctionFromID(structDefinition->memberFunctions, memberName, argList, true, true);
            if (!CalleeFID) {
                messageSystem::startBlock(childNodes[1], "Generating struct function call", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
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
                AsaStruct* retStruct = structDefinitions[CalleeFID->returnType];
                if (!retStruct || retStruct->structVal == nullptr) {
                    return messageSystem::error("Struct return type not fully defined");
                }
                memberSretAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), retStruct->structVal, "member_sret");
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
            ArgsV = std::vector<LLVMValue*>();
            ArgsV.push_back(basePtr);
            if (!ArgsV.back()) {
                messageSystem::endBlock();
                return nullptr;
            }
            // Add rest of argument values
            for (int i = 0; i < args.size(); i++) {
                if (CalleeFID->userArguments[i].isReference) {
                    const argType& fa = CalleeFID->userArguments[i];
                    if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
                        if (!fa.isConstant) {
                            messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                            return messageSystem::error("Cannot pass value as reference");
                        }
                        // const ref: auto-materialize the rvalue into a temporary alloca
                        messageSystem::startBlock(args[i], "Generating const ref argument (auto-materialize)", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                        LLVMValue* tmpVal = (LLVMValue*)(args[i]->*(args[i]->codegen))(pass);
                        if (wasError) {
                            messageSystem::endBlock();
                            return nullptr;
                        }
                        if (!tmpVal) {
                            messageSystem::endBlock();
                            return nullptr;
                        }
                        bool wasDef = true;
                        LLVMType* formalType = getLLVMTypeFromString(fa.typeString, fa.pointerLevel, token, wasDef, pass);
                        if (!formalType) {
                            return messageSystem::error("Cannot determine type for const ref materialization: " + fa.typeString);
                        }
                        AllocaInst* tmpAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), formalType, "constref_tmp");
                        llvmIRBuilder->CreateStore(tmpVal, tmpAlloc);
                        messageSystem::endBlock();
                        ArgsV.push_back(tmpAlloc);
                        if (!ArgsV.back()) {
                            return nullptr;
                        }
                        continue;
                    }
                    // Check that the argument type matches the ref parameter type
                    {
                        ASTNode* identNode = args[i]->childNodes[0];
                        valueType* argVar = findNamedValue(parentNode, this, identNode->token->tokenStr, token);
                        if (argVar) {
                            LLVMType* actualType = getValueStoredType(argVar->val);
                            bool wasDef = true;
                            LLVMType* formalType = getLLVMTypeFromString(fa.typeString, 0, token, wasDef, pass);
                            if (formalType && actualType && !actualType->isPointerTy() && !formalType->isPointerTy() && actualType != formalType) {
                                messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                                return messageSystem::error("Cannot pass '" + getStringTypeFromLLVMType(actualType) +
                                                            "' as 'ref " + fa.typeString + "': implicit cast to reference is not allowed");
                            }
                        }
                    }
                    args[i]->childNodes[0]->isRef = true;
                }
                LLVMValue* argVal = (LLVMValue*)(args[i]->*(args[i]->codegen))(pass);
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

            LLVMValue* callResult = nullptr;
            LLVMType* retType = CalleeF->getReturnType();

            // Create the call
            if (retType->isVoidTy())
                llvmIRBuilder->CreateCall(CalleeF, ArgsV);
            else
                callResult = llvmIRBuilder->CreateCall(CalleeF, ArgsV, "calltmp");


            messageSystem::endBlock();

            if (memberIsStructReturn) {
                AsaStruct* retStruct = structDefinitions[CalleeFID->returnType];
                return llvmIRBuilder->CreateLoad(retStruct->structVal, memberSretAlloc, "member_sret_load");
            }
            return callResult;

            //return llvmIRBuilder->CreateCall(CalleeF, ArgsV, "calltmp");
        }
    }


    messageSystem::endBlock();
    return nullptr;
}


// LLVMValue*
void* ASTNode::generateScopeBody(int pass)
{
    messageSystem::startBlock(this, "Generating scope body", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Only value blocks (parser-marked via isValueBlock) get their own ResultContext.
    // Control-flow bodies (if/for bodies) that contain `result` propagate to the
    // enclosing value block's context on the stack instead.
    bool isValueBlock = this->isValueBlock;

    BasicBlock* mergeBB = nullptr;
    if (isValueBlock) {
        Function* theFunction = llvmIRBuilder->GetInsertBlock()->getParent();
        mergeBB = BasicBlock::Create(*llvmCompileContext, "result_merge", theFunction);
        resultContextStack.push({nullptr, mergeBB});
    }

    for (auto& c : childNodes) {
        if (pass == 1 && (c->nodeType == Expression_Statement || c->nodeType == Colon_Separator_Node)) {
            declareModuleScopeVariable(c, this, false);
            continue;
        }
        if (c->currentNodeDoneGenerating &&
            (c->nodeType == Expression_Statement || c->nodeType == Colon_Separator_Node))
            continue;
        if (c->codegen != nullptr) {
            (void)(LLVMValue*)(c->*(c->codegen))(pass);
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
            messageSystem::startBlock(c, "Generating child node", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
            void* result = messageSystem::error("Node `" + ASTNodeTypeAsString(c->nodeType) + "` does not have a code generator");
            messageSystem::endBlock();
            return result;
        }
    }

    if (isValueBlock) {
        // Branch to merge in case the result was inside a conditional and this path
        // has no terminator (keeps the IR well-formed).
        if (!llvmIRBuilder->GetInsertBlock()->getTerminator())
            llvmIRBuilder->CreateBr(mergeBB);

        ResultContext ctx = resultContextStack.top();
        resultContextStack.pop();

        llvmIRBuilder->SetInsertPoint(mergeBB);

        if (!ctx.resultSlot) {
            return messageSystem::error("Value block has no reachable 'result' statement");
        }

        LLVMValue* resultVal = llvmIRBuilder->CreateLoad(
            ctx.resultSlot->getAllocatedType(), ctx.resultSlot, "result_val");
        messageSystem::endBlock();
        return resultVal;
    }

    messageSystem::endBlock();
    return nullptr;
}

// LLVMValue*
void* ASTNode::generateCast(int pass)
{
    messageSystem::startBlock(this, "Generating cast", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.size() < 2) {
        return messageSystem::error("Cast expression expected: `#cast(var, type)`");
    }
    ASTNode* varNode = unwrapSingleExpressionNode(args[0]);
    ASTNode* typeNode = unwrapSingleExpressionNode(args[1]);
    std::string varName = varNode->token->tokenStr;
    std::string tyVal = typeNode->token->tokenStr;
    asaToken* typeToken = typeNode->token;

    valueType* val = findNamedValue(parentNode, this, varName, token);
    if (!val && !wasError) {
        return messageSystem::error("Unknown variable name used");
    }
    LLVMValue* var = val->val;
    LLVMValue* value = llvmIRBuilder->CreateLoad(getValueStoredType(var), var, varName + "_load");
    bool wasDefined = true;
    LLVMType* toType = getLLVMTypeFromString(tyVal, 0, typeToken, wasDefined, pass);
    // Determine source signedness from the variable's declared type
    std::string srcTypeStr = val->type;
    while (!srcTypeStr.empty() && srcTypeStr[0] == '*')
        srcTypeStr = srcTypeStr.substr(1);
    bool isSrcSigned = typeSigns.count(srcTypeStr) ? typeSigns[srcTypeStr] : true;
    LLVMValue* casted = castValue(value, toType, isSrcSigned, typeSigns[tyVal], this);

    messageSystem::endBlock();

    if (wasError)
        return nullptr;
    return casted;
}

// LLVMValue*
static std::string typeStringFromNode(ASTNode* node)
{
    if (!node)
        return "";
    if (node->nodeType == Identifier_Node)
        return node->token->tokenStr;
    if (node->nodeType == Pointer_Node || node->nodeType == Dereference_Operation) {
        if (!node->childNodes.empty())
            return "*" + typeStringFromNode(node->childNodes[0]);
        return "*";
    }
    return node->token->tokenStr;
}

void* ASTNode::generateBitcast(int pass)
{
    messageSystem::startBlock(this, "Generating bitcast", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.size() < 2) {
        return messageSystem::error("Bitcast expression expected: `#bitcast(var, type)`");
    }
    ASTNode* varNode = unwrapSingleExpressionNode(args[0]);
    ASTNode* typeNode = unwrapSingleExpressionNode(args[1]);
    std::string varName = varNode->token->tokenStr;
    std::string tyVal = typeStringFromNode(typeNode);

    valueType* val = findNamedValue(parentNode, this, varName, token);
    if (!val && !wasError) {
        return messageSystem::error("Unknown variable name used");
    }
    LLVMValue* var = val->val;
    LLVMValue* value = llvmIRBuilder->CreateLoad(getValueStoredType(var), var, varName + "_load");

    bool wasDefined = true;
    LLVMType* toType = getLLVMTypeFromString(tyVal, 0, typeNode->token, wasDefined, pass);
    if (!toType) {
        return messageSystem::error("Unknown type name used in #bitcast");
    }

    // Pointer <-> integer conversions (ptrtoint / inttoptr)
    if (value->getType()->isPointerTy() && toType->isIntegerTy()) {
        messageSystem::endBlock();
        return llvmIRBuilder->CreatePtrToInt(value, toType, "ptrtoint");
    }
    if (value->getType()->isIntegerTy() && toType->isPointerTy()) {
        messageSystem::endBlock();
        return llvmIRBuilder->CreateIntToPtr(value, toType, "inttoptr");
    }

    uint64_t srcBits = value->getType()->getPrimitiveSizeInBits();
    uint64_t dstBits = toType->getPrimitiveSizeInBits();
    if (srcBits == 0 || dstBits == 0 || srcBits != dstBits) {
        return messageSystem::error("Bitcast requires source and destination types to have the same bit width. (" + std::to_string(srcBits) + " != " + std::to_string(dstBits) + ").");
    }

    messageSystem::endBlock();
    return llvmIRBuilder->CreateBitCast(value, toType, "bitcast");
}

// LLVMValue*
void* ASTNode::generateTypeInstance(int pass)
{
    messageSystem::startBlock(this, "Generating struct instance", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (childNodes.size() < 2 || childNodes[1]->childNodes.size() == 0) {
        return messageSystem::error("New expression requires type name, like `#new type;`");
    }

    std::string typeName = childNodes[1]->childNodes[0]->token->tokenStr;
    if (structDefinitions.find(typeName) == structDefinitions.end()) {
        messageSystem::startBlock(childNodes[1]->childNodes[0], "Generating type from name", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
        return messageSystem::error("Unknown type name used");
    }

    AsaStruct* typeVal = structDefinitions[typeName];

    // If the struct body hasn't been generated yet, generate it
    if (typeVal->structVal == nullptr)
        LLVMValue* argVal = (LLVMValue*)(typeVal->sourceNode->*(typeVal->sourceNode->codegen))(pass);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }

    AllocaInst* var = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), typeVal->structVal, "struct_alloc");

    LLVMValue* structSize = ConstantInt::get(LLVMType::getInt64Ty(*llvmCompileContext),
        llvmCompileModule->getDataLayout().getTypeAllocSize(typeVal->structVal));

    // Create memset call to zero the memory
    Function* memsetFunc = Intrinsic::getOrInsertDeclaration(llvmCompileModule.get(), Intrinsic::memset, {var->getType(), LLVMType::getInt64Ty(*llvmCompileContext)});

    llvmIRBuilder->CreateCall(memsetFunc, {
                                              var,                                                            // dest
                                              ConstantInt::get(LLVMType::getInt8Ty(*llvmCompileContext), 0),  // value (zero)
                                              structSize,                                                     // size
                                              ConstantInt::get(LLVMType::getInt1Ty(*llvmCompileContext), 0)   // is_volatile
                                          });

    // Apply non-zero default values for members that declare them
    for (auto& [memberName, defaultNode] : typeVal->memberDefaultNodes) {
        auto idxIt = typeVal->memberNameIndexes.find(memberName);
        if (idxIt == typeVal->memberNameIndexes.end())
            continue;
        uint16_t idx = idxIt->second;
        LLVMValue* memberPtr = llvmIRBuilder->CreateStructGEP(typeVal->structVal, var, idx, memberName + "_init");
        LLVMValue* defaultVal = (LLVMValue*)(defaultNode->*(defaultNode->codegen))(pass);
        if (wasError)
            return nullptr;
        if (!defaultVal)
            continue;
        LLVMType* memberType = typeVal->structVal->getElementType(idx);
        bool isSigned = typeSigns.count(typeVal->members[idx].typeString) ? typeSigns[typeVal->members[idx].typeString] : false;
        defaultVal = castValue(defaultVal, memberType, true, isSigned, this);
        if (wasError) {
            messageSystem::endBlock();
            return nullptr;
        }
        llvmIRBuilder->CreateStore(defaultVal, memberPtr);
    }

    messageSystem::endBlock();
    return var;
}

// LLVMValue*
// Deep-copy an ASTNode for template instantiation.
// Does NOT copy parentNode (caller sets it after the call).
// Resets currentNodeDoneGenerating so the copy generates fresh.
static ASTNode* deepCopyASTNode(ASTNode* src)
{
    if (!src)
        return nullptr;
    ASTNode* copy = new ASTNode();
    copy->nodeType = src->nodeType;
    copy->token = new asaToken(*src->token);
    copy->lineNumber = src->lineNumber;
    copy->depth = src->depth;
    copy->isRef = src->isRef;
    copy->isConst = src->isConst;
    copy->isExtern = src->isExtern;
    copy->lvalue = src->lvalue;
    copy->isPostfix = src->isPostfix;
    copy->label = src->label;
    copy->externSymbolName = src->externSymbolName;
    copy->isModuleScope = src->isModuleScope;
    copy->importedChildren = src->importedChildren;
    copy->enclosingModule = src->enclosingModule;
    copy->currentNodeDoneGenerating = false;
    copy->isValueBlock = src->isValueBlock;
    copy->codegen = src->codegen;
    copy->compareTokens = src->compareTokens;
    copy->defaultRawTokens = src->defaultRawTokens;
    copy->asaType = src->asaType;
    for (auto* child : src->childNodes)
        copy->childNodes.push_back(deepCopyASTNode(child));
    for (auto* leaf : src->leafNodes)
        copy->leafNodes.push_back(deepCopyASTNode(leaf));
    for (auto* attr : src->attributes)
        copy->attributes.push_back(deepCopyASTNode(attr));
    for (auto& [k, v] : src->compilerDefinitions)
        copy->compilerDefinitions[k] = deepCopyASTNode(v);
    return copy;
}

// Walk an AST subtree and replace every token whose text equals paramName with concreteType.
static void substituteTypeParam(ASTNode* node, const std::string& paramName, const std::string& concreteType, ASTNodeType concreteNodeType = Nothing_Node)
{
    if (!node)
        return;
    if (node->token) {
        if (node->token->tokenStr == paramName) {
            node->token->tokenStr = concreteType;
            // For value params (integer, float, bool, string literals), also update the node
            // type and codegen so the substituted node evaluates as a literal, not a variable reference.
            if (concreteNodeType != Nothing_Node && concreteNodeType != Identifier_Node) {
                node->nodeType = concreteNodeType;
                node->codegen = &ASTNode::generateConstant;
            }
        }
        else {
            // Also substitute in compound names like "array.T" -> "array.int"
            std::string& s = node->token->tokenStr;
            std::string dotParam = "." + paramName;
            size_t pos = 0;
            while ((pos = s.find(dotParam, pos)) != std::string::npos) {
                size_t endPos = pos + dotParam.size();
                if (endPos == s.size() || s[endPos] == '.') {
                    s.replace(pos + 1, paramName.size(), concreteType);
                    pos += 1 + concreteType.size();
                }
                else {
                    pos += dotParam.size();
                }
            }
        }
    }
    for (auto* child : node->childNodes)
        substituteTypeParam(child, paramName, concreteType, concreteNodeType);
    for (auto* leaf : node->leafNodes)
        substituteTypeParam(leaf, paramName, concreteType, concreteNodeType);
}

static ASTNode* variantParamFirstValueNode(ASTNode* node)
{
    if (!node)
        return nullptr;
    if (node->token && !node->token->tokenStr.empty()) {
        switch (node->nodeType) {
            case Identifier_Node:
            case Integer_Node:
            case Float_Node:
            case Boolean_Node:
            case String_Constant_Node:
            case Character_Constant_Node:
                return node;
            default:
                break;
        }
    }
    for (auto* leaf : node->leafNodes) {
        auto r = variantParamFirstValueNode(leaf);
        if (r)
            return r;
    }
    for (auto* child : node->childNodes) {
        auto r = variantParamFirstValueNode(child);
        if (r)
            return r;
    }
    return nullptr;
}

// Return the token text of the first significant node in a variant param subtree.
// Handles both type params (Identifier_Node) and value params (Integer, Float, Bool, String literals).
static std::string variantParamFirstIdent(ASTNode* node)
{
    ASTNode* valueNode = variantParamFirstValueNode(node);
    if (!valueNode || !valueNode->token)
        return "";
    return valueNode->token->tokenStr;
}

// Return the ASTNodeType of the first significant node in a variant param subtree.
static ASTNodeType variantParamNodeType(ASTNode* node)
{
    ASTNode* valueNode = variantParamFirstValueNode(node);
    if (!valueNode)
        return Nothing_Node;
    return valueNode->nodeType;
}

// Instantiate a variant struct template.
// baseName     : e.g. "duo"
// concreteArgs : suffix of the mangled name split by '.', e.g. ["int","float"]
//                OR the raw suffix string (split performed internally when called from getLLVMTypeFromString)
// mangledName  : e.g. "duo.int.float"
// substitutions: optional pre-built (paramName, concreteName) pairs; if empty, derived from concreteArgs
static void instantiateVariantStruct(const std::string& baseName, const std::string& concreteSuffix, const std::string& mangledName, std::vector<std::pair<std::string, std::string>> substitutions)
{
    auto tmplIt = variantStructTemplates.find(baseName);
    if (tmplIt == variantStructTemplates.end())
        return;
    ASTNode* tmpl = tmplIt->second;

    // If substitutions were not pre-built, derive them by splitting concreteSuffix on '.'
    // and matching positionally against the template's Variants_Node param names.
    if (substitutions.empty()) {
        ASTNode* defVariantsNode = (tmpl->childNodes.size() > 1) ? tmpl->childNodes[1] : nullptr;
        if (!defVariantsNode || defVariantsNode->childNodes.empty())
            return;

        // Split concreteSuffix by '.' to recover individual concrete types.
        std::vector<std::string> concreteTypes;
        {
            std::string part;
            for (char c : concreteSuffix) {
                if (c == '.') {
                    if (!part.empty()) {
                        concreteTypes.push_back(part);
                        part.clear();
                    }
                }
                else {
                    part += c;
                }
            }
            if (!part.empty())
                concreteTypes.push_back(part);
        }

        int nv = (int)std::min(concreteTypes.size(), defVariantsNode->childNodes.size());
        for (int vi = 0; vi < nv; vi++) {
            std::string paramName = variantParamFirstIdent(defVariantsNode->childNodes[vi]);
            if (!paramName.empty())
                substitutions.push_back({paramName, concreteTypes[vi]});
        }
        if (substitutions.empty())
            return;
    }

    ASTNode* inst = deepCopyASTNode(tmpl);
    inst->parentNode = tmpl->parentNode;
    inst->depth = tmpl->depth;
    assignParentNodes(inst, inst->depth);

    for (auto& [pName, cType] : substitutions)
        substituteTypeParam(inst, pName, cType);

    inst->token->tokenStr = mangledName;

    // Clear variant params so it generates as a concrete struct
    if (inst->childNodes.size() > 1) {
        inst->childNodes[1]->childNodes.clear();
        inst->childNodes[1]->leafNodes.clear();
    }

    BasicBlock* savedBlock = llvmIRBuilder->GetInsertBlock();
    BasicBlock::iterator savedPt = savedBlock ? llvmIRBuilder->GetInsertPoint() : BasicBlock::iterator();
    DebugLoc savedDbgLoc = llvmIRBuilder->getCurrentDebugLocation();

    inst->generateStruct(0);
    if (!wasError)
        inst->generateStruct(1);
    if (!wasError)
        inst->generateStruct(2);

    if (savedBlock)
        llvmIRBuilder->SetInsertPoint(savedBlock, savedPt);
    llvmIRBuilder->SetCurrentDebugLocation(savedDbgLoc);
}

void* ASTNode::generateCallExpression(int pass)
{
    messageSystem::startBlock(this, "Generating function call", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (!LexicalBlocks.empty() && token && token->filePath)
        llvmIRBuilder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));
    ASTNode* argsNode = childNodes[0];
    bool shouldBeMemberFunction = isCallMemberFunction;
    isCallMemberFunction = false;

    std::vector<ASTNode*> args = std::vector<ASTNode*>();
    for (auto& a : argsNode->childNodes)
        if (a->childNodes.size() > 0) {
            args.push_back(a);
        }

    std::vector<LLVMValue*> ArgsV = std::vector<LLVMValue*>();
    std::vector<LLVMValue*> cachedArgVals = std::vector<LLVMValue*>();
    argumentList argList = argumentList();

    // Build argList and ArgsV WITHOUT sret initially
    for (int i = 0; i < args.size(); i++) {
        LLVMValue* argVal = (LLVMValue*)(args[i]->*(args[i]->codegen))(pass);
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
            valueType* val = findNamedValue(parentNode, this, identifierNode->token->tokenStr, token);
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
            if (!compilerDirectiveFlags["IN_STRING_MODULE"] &&
                structDefinitions.count("string") && structDefinitions["string"]->structVal)
                typeStr = "string";
            else
                typeStr = "*char";
        }
        else if (identifierNode->nodeType == Address_Of_Operation) {
            // &expr: type is pointer-to-inner-type
            if (!identifierNode->childNodes.empty()) {
                ASTNode* inner = identifierNode->childNodes[0];
                if (inner->nodeType == Identifier_Node) {
                    // Plain variable: look up its declared type
                    valueType* val = findNamedValue(parentNode, this, inner->token->tokenStr, token);
                    if (val && !val->type.empty())
                        typeStr = "*" + val->type;
                }
                else if (inner->asaType && !inner->asaType->strVal.empty()) {
                    // Complex expression (member access, subscript, etc.): use the asaType set during codegen
                    typeStr = "*" + inner->asaType->strVal;
                }
            }
            if (typeStr.empty())
                typeStr = getStringTypeFromLLVMType(argVal->getType());
        }
        else if (identifierNode->nodeType == Function_Call) {
            // Look up callee return type from functionIDs to avoid *unknown for pointer returns
            std::string calleeName = identifierNode->token->tokenStr;
            for (auto* fid : functionIDs) {
                if (fid->name == calleeName && !fid->returnType.empty()) {
                    typeStr = fid->returnType;
                    break;
                }
            }
            if (typeStr.empty())
                typeStr = getStringTypeFromLLVMType(argVal->getType());
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
        console::writeLine("\nLooking for function: " + token->tokenStr);
        console::writeLine("Arguments passed:");
        for (size_t i = 0; i < argList.size(); i++) {
            console::writeLine("  [" + std::to_string(i) + "] type: " + argList[i].typeString +
                               ", pointerLevel: " + std::to_string(argList[i].pointerLevel));
        }
    }
    std::string resolvedFnName = resolveTypeAlias(token->tokenStr);

    // Handle variant function calls: foo<i32>(...).
    if (childNodes.size() > 1 && childNodes[1] && childNodes[1]->nodeType == Variants_Node && !childNodes[1]->childNodes.empty()) {
        ASTNode* callVariantsNode = childNodes[1];
        auto tmplIt = variantFunctionTemplates.find(resolvedFnName);
        if (tmplIt != variantFunctionTemplates.end()) {
            ASTNode* tmplNode = tmplIt->second;
            ASTNode* defVariantsNode = tmplNode->childNodes[4];

            // Build mangled name and substitution list from paired definition/call params.
            std::string mangledFnName = resolvedFnName;
            struct SubstItem {
                std::string paramName;
                std::string concreteName;
                ASTNodeType nodeType;
            };
            std::vector<SubstItem> substitutions;
            int nv = (int)std::min(callVariantsNode->childNodes.size(), defVariantsNode->childNodes.size());
            for (int vi = 0; vi < nv; vi++) {
                std::string paramName = variantParamFirstIdent(defVariantsNode->childNodes[vi]);
                std::string concreteType = variantParamFirstIdent(callVariantsNode->childNodes[vi]);
                ASTNodeType concreteNT = variantParamNodeType(callVariantsNode->childNodes[vi]);
                // Sanitize the concrete value for use in the mangled name
                // (strip quotes from string literals so the name is a valid LLVM identifier).
                std::string manglePart = concreteType;
                if (concreteNT == String_Constant_Node) {
                    ASTNode* concreteNode = variantParamFirstValueNode(callVariantsNode->childNodes[vi]);
                    if (concreteNode && concreteNode->token)
                        manglePart = decodeQuotedStringToken(concreteNode->token);
                }
                if (!paramName.empty() && !concreteType.empty()) {
                    mangledFnName += "." + manglePart;
                    substitutions.push_back({paramName, concreteType, concreteNT});
                }
            }

            // Instantiate if not already done.
            if (!substitutions.empty() && instantiatedVariantFunctions.find(mangledFnName) == instantiatedVariantFunctions.end()) {
                instantiatedVariantFunctions.insert(mangledFnName);

                ASTNode* inst = deepCopyASTNode(tmplNode);
                inst->parentNode = tmplNode->parentNode;
                inst->depth = tmplNode->depth;
                assignParentNodes(inst, inst->depth);

                for (auto& s : substitutions)
                    substituteTypeParam(inst, s.paramName, s.concreteName, s.nodeType);

                // Set the variant-mangled base name. generatePrototype will further
                // mangle with return and argument types (e.g. identity.int -> identity.int.int.int),
                // but the fnName stored in functionIDs remains mangledFnName for lookup.
                inst->token->tokenStr = mangledFnName;

                // Clear variant params so the instantiation generates as a normal function.
                inst->childNodes[4]->childNodes.clear();
                inst->childNodes[4]->leafNodes.clear();

                // Save the current llvmIRBuilder context before generating the new function.
                // generateFunction changes the insert point; we must restore it afterward.
                BasicBlock* savedBlock = llvmIRBuilder->GetInsertBlock();
                BasicBlock::iterator savedPt = savedBlock ? llvmIRBuilder->GetInsertPoint() : BasicBlock::iterator();
                DebugLoc savedDbgLoc = llvmIRBuilder->getCurrentDebugLocation();

                inst->generateFunction(2);

                // Restore llvmIRBuilder insert point and debug location.
                if (savedBlock)
                    llvmIRBuilder->SetInsertPoint(savedBlock, savedPt);
                llvmIRBuilder->SetCurrentDebugLocation(savedDbgLoc);

                if (wasError) {
                    messageSystem::endBlock();
                    return nullptr;
                }
            }

            resolvedFnName = mangledFnName;
        }
        else {
            // Not a variant function template. Check if it's a variant struct constructor call (e.g. array<int>()).
            auto sTmplIt = variantStructTemplates.find(resolvedFnName);
            if (sTmplIt != variantStructTemplates.end()) {
                ASTNode* callVariantsNode = childNodes[1];
                ASTNode* tmplNode = sTmplIt->second;
                ASTNode* defVariantsNode = tmplNode->childNodes.size() > 1 ? tmplNode->childNodes[1] : nullptr;
                std::string mangledStructName = resolvedFnName;
                std::vector<std::pair<std::string, std::string>> structSubstitutions;
                if (defVariantsNode) {
                    int nv = (int)std::min(callVariantsNode->childNodes.size(), defVariantsNode->childNodes.size());
                    for (int vi = 0; vi < nv; vi++) {
                        std::string paramName = variantParamFirstIdent(defVariantsNode->childNodes[vi]);
                        std::string concreteType = variantParamFirstIdent(callVariantsNode->childNodes[vi]);
                        if (!paramName.empty() && !concreteType.empty()) {
                            mangledStructName += "." + concreteType;
                            structSubstitutions.push_back({paramName, concreteType});
                        }
                    }
                }
                if (instantiatedVariantStructs.find(mangledStructName) == instantiatedVariantStructs.end()) {
                    instantiatedVariantStructs.insert(mangledStructName);
                    size_t dotPos = mangledStructName.find('.');
                    if (dotPos != std::string::npos)
                        instantiateVariantStruct(resolvedFnName, mangledStructName.substr(dotPos + 1), mangledStructName, structSubstitutions);
                }
                resolvedFnName = mangledStructName;
            }
        }
    }

    functionID* CalleeFID = getFunctionFromID(functionIDs, resolvedFnName, argList, true, shouldBeMemberFunction, true);
    if (!CalleeFID) {
        return nullptr;
    }

    // Check @deprecated / @removed on the declaration
    if (CalleeFID->declNode)
        if (!checkDeprecationAttributes(this, CalleeFID->declNode, CalleeFID->name))
            return nullptr;

    Function* CalleeF = CalleeFID->fnValue;
    CalleeFID->uses++;

    // Fill in default argument values for any missing arguments
    {
        size_t numFormalArgs = CalleeFID->userArguments.size();
        for (size_t di = args.size(); di < numFormalArgs; di++) {
            ASTNode* defNode = CalleeFID->userArguments[di].defaultNode;
            const std::vector<asaToken*>& rawToks = CalleeFID->userArguments[di].defaultRawTokens;
            if (!defNode) {
                return messageSystem::error("Missing argument with no default value");
            }
            LLVMValue* defVal = nullptr;
            if (!rawToks.empty()) {
                // Re-parse with call-site file/line info for #filepath/#linenum
                std::vector<asaToken*> cloned;
                for (int ti = 0; ti < (int)rawToks.size(); ti++) {
                    asaToken* c = new asaToken(*rawToks[ti]);
                    if (rawToks[ti]->tokenType == Hash && ti + 1 < (int)rawToks.size()) {
                        const std::string& next = rawToks[ti + 1]->tokenStr;
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
                defVal = (LLVMValue*)(tempNode->*(tempNode->codegen))(pass);
            }
            else {
                defNode->parentNode = parentNode;
                defVal = (LLVMValue*)(defNode->*(defNode->codegen))(pass);
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
        AsaStruct* retStruct = structDefinitions[CalleeFID->returnType];
        if (retStruct->structVal == nullptr) {
            return messageSystem::error("Struct return type not fully defined");
        }

        // Allocate space for the returned struct on the caller's stack
        sretAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), retStruct->structVal, "sret_alloc");

        // Insert the sret pointer as the FIRST argument in ArgsV
        ArgsV.insert(ArgsV.begin(), sretAlloc);

        // For argList matching: Temporarily add sret to argList for validation
        // (This matches how it's stored in functionID)
        argType sretArg("*" + CalleeFID->returnType, Struct_Type, 1, false, true);
        argList.insert(argList.begin(), sretArg);
    }

    // Validate argument count (including sret if applicable)
    // For coerced functions, the LLVM arg count is larger than the user arg count
    if (CalleeFID->variableNumArguments == false) {
        size_t expectedIRArgCount = ArgsV.size();
        for (auto& ua : CalleeFID->userArguments)
            if (ua.externCoercionCount > 0)
                expectedIRArgCount += ua.externCoercionCount - 1;
        if (CalleeF->arg_size() != expectedIRArgCount) {  // Use ArgsV.size() which includes sret
            return messageSystem::error("Incorrect number of arguments passed to function (expected " + std::to_string(CalleeF->arg_size()) + ")", messageSystem::Incorrect_Number_Of_Function_Arguments_Error);

            //printTokenError(getASTTokenRange(this), "Incorrect number of arguments passed to function (expected " + std::to_string(CalleeF->arg_size()) + ")", __LINE__);
        }
    }
    else if (CalleeF->arg_size() > ArgsV.size()) {
        return messageSystem::error("Incorrect number of arguments passed to function (expected " + std::to_string(CalleeF->arg_size()) + ")", messageSystem::Incorrect_Number_Of_Function_Arguments_Error);

        //printTokenError(getASTTokenRange(this), "Incorrect number of arguments passed to function", __LINE__);
    }

    int formalArgCount = (int)CalleeFID->userArguments.size();
    ArgsV.clear();
    int irArgIdx = isStructReturn ? 1 : 0;        // tracks position in the LLVM function's param list
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
        LLVMValue* argVal = nullptr;
        if (isRef) {
            messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
            const argType& fa = CalleeFID->arguments[formalArgIdx];

            if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
                if (!fa.isConstant) {
                    return messageSystem::error("Cannot pass value as reference");
                }
                // const ref: materialize the cached rvalue into a temporary alloca
                LLVMValue* tmpVal = cachedArgVals[i];
                if (!tmpVal) {
                    messageSystem::endBlock();
                    return nullptr;
                }
                bool wasDef = true;
                LLVMType* formalType = getLLVMTypeFromString(fa.typeString, fa.pointerLevel, token, wasDef, pass);
                if (!formalType) {
                    return messageSystem::error("Cannot determine type for const ref materialization: " + fa.typeString);
                }
                AllocaInst* tmpAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), formalType, "constref_tmp");
                llvmIRBuilder->CreateStore(tmpVal, tmpAlloc);
                argVal = tmpAlloc;
                messageSystem::endBlock();
            }
            else {
                // Passing a value that requires an implicit cast to a ref parameter is not allowed:
                // the cast would produce a temporary, and a reference to a temporary is meaningless.
                {
                    LLVMValue* actualVal = cachedArgVals[i];
                    bool wasDef = true;
                    LLVMType* formalType = getLLVMTypeFromString(fa.typeString, 0, token, wasDef, pass);
                    if (formalType && actualVal && !actualVal->getType()->isPointerTy() && !formalType->isPointerTy() && actualVal->getType() != formalType) {
                        return messageSystem::error("Cannot pass '" + getStringTypeFromLLVMType(actualVal->getType()) + "' as 'ref " + fa.typeString + "': implicit cast to reference is not allowed");
                    }
                }
                args[i]->childNodes[0]->isRef = true;
                argVal = (LLVMValue*)(args[i]->*(args[i]->codegen))(pass);
                if (wasError) {
                    messageSystem::endBlock();
                    return nullptr;
                }

                messageSystem::endBlock();
            }
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
            argVal = llvmIRBuilder->CreateExtractValue(argVal, {0}, "str_addr");
        }
        else if (argVal && argVal->getType()->isPointerTy() && !isRef &&
                 formal.pointerLevel == 0 && formal.typeString == "string" &&
                 structDefinitions.count("string") && structDefinitions["string"]->structVal) {
            // *char -> string: build string struct with strlen
            StructType* strTy = cast<StructType>((LLVMType*)structDefinitions["string"]->structVal);
            FunctionCallee strlenFn = llvmCompileModule->getOrInsertFunction("strlen",
                FunctionType::get(LLVMType::getInt64Ty(*llvmCompileContext), {PointerType::getUnqual(*llvmCompileContext)}, false));
            LLVMValue* lenVal = llvmIRBuilder->CreateCall(strlenFn, {argVal}, "strlen");
            LLVMValue* lenTrunc = llvmIRBuilder->CreateTrunc(lenVal, LLVMType::getInt32Ty(*llvmCompileContext), "len");
            LLVMValue* strStruct = UndefValue::get(strTy);
            strStruct = llvmIRBuilder->CreateInsertValue(strStruct, argVal, {0});
            strStruct = llvmIRBuilder->CreateInsertValue(strStruct, lenTrunc, {1});
            argVal = strStruct;
        }
        // ABI coercion: split struct into doubles (SSE/XMM) or i64s (INTEGER) per x86-64 SysV ABI
        if (coerce > 0 && argVal && argVal->getType()->isStructTy()) {
            AllocaInst* tmp = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), argVal->getType(), "coerce");
            llvmIRBuilder->CreateStore(argVal, tmp);
            LLVMType* primTy = coerceIsFloat ? (LLVMType*)Type::getDoubleTy(*llvmCompileContext) : (LLVMType*)Type::getInt64Ty(*llvmCompileContext);
            for (int k = 0; k < coerce; k++) {
                LLVMValue* bytePtr = llvmIRBuilder->CreateConstGEP1_64(LLVMType::getInt8Ty(*llvmCompileContext), tmp, (uint64_t)(k * 8), "coerce_gep");
                ArgsV.push_back(llvmIRBuilder->CreateLoad(primTy, bytePtr, "coerce_val"));
            }
            irArgIdx += coerce;
            continue;
        }
        // Pack @packed struct to integer when formal param expects an integer (extern ABI)
        if (argVal && !isRef && argVal->getType()->isStructTy()) {
            LLVMType* formalLLVMType = nullptr;
            if (irArgIdx < (int)CalleeF->getFunctionType()->getNumParams())
                formalLLVMType = CalleeF->getFunctionType()->getParamType(irArgIdx);
            if (formalLLVMType && formalLLVMType->isIntegerTy()) {
                AllocaInst* tmp = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), argVal->getType(), "packed_tmp");
                llvmIRBuilder->CreateStore(argVal, tmp);
                LLVMValue* intPtr = llvmIRBuilder->CreateBitCast(tmp, PointerType::get(*llvmCompileContext, 0));
                argVal = llvmIRBuilder->CreateLoad(formalLLVMType, intPtr, "packed_int");
            }
            // Large struct passed to pointer param: copy to stack and pass pointer (byval ABI)
            else if (formalLLVMType && formalLLVMType->isPointerTy()) {
                uint64_t size = llvmCompileModule->getDataLayout().getTypeAllocSize(argVal->getType());
                if (size > 16) {
                    AllocaInst* copy = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), argVal->getType(), "byval_copy");
                    llvmIRBuilder->CreateStore(argVal, copy);
                    argVal = copy;
                }
            }
        }
        // Implicit numeric coercion: cast arg to formal param type if they differ
        if (argVal && !isRef) {
            LLVMType* formalLLVMType = nullptr;
            if (irArgIdx < (int)CalleeF->getFunctionType()->getNumParams())
                formalLLVMType = CalleeF->getFunctionType()->getParamType(irArgIdx);
            if (formalLLVMType && argVal->getType() != formalLLVMType &&
                !argVal->getType()->isStructTy() && !argVal->getType()->isPointerTy() &&
                !formalLLVMType->isStructTy() && !formalLLVMType->isPointerTy()) {
                // Use the actual source type's signedness (not hardcoded true)
                const std::string& srcTs = (formalArgIdx < (int)argList.size()) ? argList[formalArgIdx].typeString : "";
                bool isSrcSig = srcTs.empty() ? true : (typeSigns.count(srcTs) ? typeSigns[srcTs] : true);
                argVal = castValue(argVal, formalLLVMType, isSrcSig, false, this);
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
        LLVMValue* defVal = cachedArgVals[i];
        if (coerce > 0 && defVal && defVal->getType()->isStructTy()) {
            AllocaInst* tmp = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), defVal->getType(), "coerce_def");
            llvmIRBuilder->CreateStore(defVal, tmp);
            LLVMType* primTy = coerceIsFloat ? (LLVMType*)Type::getDoubleTy(*llvmCompileContext) : (LLVMType*)Type::getInt64Ty(*llvmCompileContext);
            for (int k = 0; k < coerce; k++) {
                LLVMValue* bytePtr = llvmIRBuilder->CreateConstGEP1_64(LLVMType::getInt8Ty(*llvmCompileContext), tmp, (uint64_t)(k * 8), "coerce_gep");
                ArgsV.push_back(llvmIRBuilder->CreateLoad(primTy, bytePtr, "coerce_val"));
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

    LLVMValue* callResult = nullptr;
    LLVMType* retType = CalleeF->getReturnType();

    // Create the call
    CallInst* callInst = nullptr;
    if (retType->isVoidTy())
        callInst = llvmIRBuilder->CreateCall(CalleeF, ArgsV);
    else {
        callInst = llvmIRBuilder->CreateCall(CalleeF, ArgsV, "calltmp");
        callResult = callInst;
    }
    // Propagate byval attributes from the function declaration to this call instruction
    if (callInst) {
        for (unsigned pi = 0; pi < CalleeF->getFunctionType()->getNumParams(); pi++) {
            LLVMType* bvType = CalleeF->getParamByValType(pi);
            if (bvType)
                callInst->addParamAttr(pi, Attribute::getWithByValType(*llvmCompileContext, bvType));
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
            AsaStruct* retStruct = structDefinitions[CalleeFID->returnType];
            asaType->baseLLVMType = retStruct->structVal;
            return llvmIRBuilder->CreateLoad(retStruct->structVal, sretAlloc, "sret_load");
        }
    }

    // Unpack coerced register return back to the original struct type.
    // Extern functions returning small structs (<=16 bytes) return them as { i64, i64 } or
    // { double, double } per the x86-64 SysV ABI instead of via sret.
    if (CalleeFID->externReturnCoercionCount > 0 && callResult && !CalleeFID->returnType.empty()) {
        auto it = structDefinitions.find(CalleeFID->returnType);
        if (it != structDefinitions.end() && it->second->structVal) {
            // Allocate space for the struct, store the coerced value into it, then load as struct.
            AllocaInst* tmp = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), it->second->structVal, "ret_coerce");
            // Store the coerced aggregate (e.g. { i64, i64 }) directly through the alloca pointer.
            // With opaque pointers this is valid: we store a differently-typed value to the same memory.
            llvmIRBuilder->CreateStore(callResult, tmp);
            if (!asaType)
                asaType = new ASAType(nullptr);
            asaType->strVal = CalleeFID->returnType;
            asaType->baseLLVMType = it->second->structVal;
            if (lvalue)
                return tmp;
            callResult = llvmIRBuilder->CreateLoad(it->second->structVal, tmp, "ret_struct");
        }
    }

    // Unpack integer result back to @packed struct if the declared return type is a packed struct
    if (callResult && callResult->getType()->isIntegerTy() && !CalleeFID->returnType.empty()) {
        auto it = structDefinitions.find(CalleeFID->returnType);
        if (it != structDefinitions.end() && it->second->isPacked && it->second->structVal) {
            AllocaInst* tmp = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), it->second->structVal, "packed_ret");
            LLVMValue* intPtr = llvmIRBuilder->CreateBitCast(tmp, PointerType::get(*llvmCompileContext, 0));
            llvmIRBuilder->CreateStore(callResult, intPtr);
            callResult = llvmIRBuilder->CreateLoad(it->second->structVal, tmp, "unpacked_ret");
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


// LLVMValue*
void* ASTNode::generateIf(int pass)
{
    messageSystem::startBlock(this, "Generating if statement", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (!LexicalBlocks.empty() && token && token->filePath)
        llvmIRBuilder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));
    ASTNode* condExpr = childNodes[0];
    if (condExpr->childNodes.size() == 0) {
        messageSystem::startBlock(condExpr, "Generating condition", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
        return messageSystem::error("Expected condition expression");
    }
    condExpr = condExpr->childNodes[0];

    if (condExpr->codegen == nullptr) {
        messageSystem::startBlock(condExpr, "Generating condition", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
        return messageSystem::error("Node `" + ASTNodeTypeAsString(condExpr->nodeType) + "` does not have a code generator");
    }

    LLVMValue* CondV = (LLVMValue*)(condExpr->*(condExpr->codegen))(pass);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }
    if (!CondV) {
        messageSystem::endBlock();
        return nullptr;
    }

    // Convert condition to a bool by comparing non-equal to zero (same type as CondV).
    CondV = llvmIRBuilder->CreateICmpNE(CondV, Constant::getNullValue(CondV->getType()), "ifcond");

    Function* TheFunction = llvmIRBuilder->GetInsertBlock()->getParent();

    // Create blocks for the then and else cases.  Insert the 'then' block at the
    // end of the function.
    BasicBlock* ThenBB = BasicBlock::Create(*llvmCompileContext, "then", TheFunction);
    BasicBlock* ElseBB = BasicBlock::Create(*llvmCompileContext, "else");
    BasicBlock* MergeBB = BasicBlock::Create(*llvmCompileContext, "ifcont");

    llvmIRBuilder->CreateCondBr(CondV, ThenBB, ElseBB);

    // Emit if body
    llvmIRBuilder->SetInsertPoint(ThenBB);

    ASTNode* scopeBody = childNodes[1];

    if (scopeBody->codegen == nullptr) {
        return messageSystem::error("Node `" + ASTNodeTypeAsString(scopeBody->nodeType) + "` does not have a code generator");
    }

    LLVMValue* ThenV = (LLVMValue*)(scopeBody->*(scopeBody->codegen))(pass);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }

    llvmIRBuilder->CreateBr(MergeBB);
    // Codegen of 'scopeBody' can change the current block, update ThenBB for the PHI.
    ThenBB = llvmIRBuilder->GetInsertBlock();


    // Emit else block.
    TheFunction->insert(TheFunction->end(), ElseBB);
    llvmIRBuilder->SetInsertPoint(ElseBB);

    ASTNode* elseBody = childNodes[2];

    if (elseBody->codegen == nullptr) {
        return messageSystem::error("Node `" + ASTNodeTypeAsString(elseBody->nodeType) + "` does not have a code generator");
    }

    LLVMValue* ElseV = (LLVMValue*)(elseBody->*(elseBody->codegen))(pass);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }

    llvmIRBuilder->CreateBr(MergeBB);
    // codegen of 'Else' can change the current block, update ElseBB for the PHI.
    ElseBB = llvmIRBuilder->GetInsertBlock();


    // Emit merge block.
    TheFunction->insert(TheFunction->end(), MergeBB);
    llvmIRBuilder->SetInsertPoint(MergeBB);

    messageSystem::endBlock();
    return nullptr;
}

// LLVMValue*
void* ASTNode::generateStruct(int pass)
{
    messageSystem::startBlock(this, "Generating struct definition", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::string structName = token->tokenStr;

    // If this struct has variant parameters, store it as a template and skip generation.
    if (childNodes.size() > 1 && childNodes[1] && childNodes[1]->nodeType == Variants_Node && !childNodes[1]->childNodes.empty()) {
        variantStructTemplates[structName] = this;
        messageSystem::endBlock();
        return nullptr;
    }

    // Do not create a struct with the same name
    if (structDefinitions.find(structName) != structDefinitions.end()) {
        if (structDefinitions[structName]->token != token) {
            return messageSystem::error("Struct cannot be redefined");
        }
    }

    currentStructName.push(structName);
    uint8_t generatingType = 0;  // Generate all member variables first (0), then functions (1)
    argumentList members = argumentList();
    std::vector<Type*> fieldTypes = std::vector<Type*>();
    std::vector<std::string> fieldNames = std::vector<std::string>();
    std::vector<functionID*> memberFunctions = std::vector<functionID*>();
    std::unordered_map<std::string, uint16_t> memberNameIndexes = std::unordered_map<std::string, uint16_t>();
    std::unordered_map<std::string, ASTNode*> memberDefaultNodes = std::unordered_map<std::string, ASTNode*>();
    std::unordered_map<std::string, ASTNode*> memberNameTypeNodes = std::unordered_map<std::string, ASTNode*>();
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
                    messageSystem::startBlock(fieldNode, "Generating struct member", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                    return messageSystem::error("Member declaration must have type");
                }


                ASTNode* typeNode = fieldNode->childNodes[1];
                std::string memberType = typeNode->token->tokenStr;

                ASTNode* nameTypeNode = fieldNode;
                fieldNode = fieldNode->childNodes[0];

                std::string memberName = fieldNode->token->tokenStr;
                int pointerLevel = 0;

                memberNameTypeNodes[memberName] = nameTypeNode;

            recurseAddMemberPointer:

                if (typeNode->token->tokenStr == "*") {
                    pointerLevel++;
                    typeNode = typeNode->childNodes[0];
                    goto recurseAddMemberPointer;
                }

                memberType = typeNode->token->tokenStr;

                bool wasDefined = true;
                LLVMType* fieldType = getLLVMTypeFromString(memberType, 0, typeNode->token,
                    wasDefined, pass);
                for (int i = 0; i < pointerLevel; i++)
                    fieldType = PointerType::get(*llvmCompileContext, 0);
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
        structDefinitions[structName]->memberNameTypeNodes = memberNameTypeNodes;
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
                BasicBlock* entryBlock = BasicBlock::Create(*llvmCompileContext, "entry", fn);
                llvmIRBuilder->SetInsertPoint(entryBlock);
                LLVMValue* sretPtr = fn->getArg(0);
                StructType* sTy = (StructType*)structDefinitions[structName]->structVal;
                LLVMValue* szVal = ConstantInt::get(LLVMType::getInt64Ty(*llvmCompileContext), llvmCompileModule->getDataLayout().getTypeAllocSize(sTy));
                Function* memsetFn = Intrinsic::getOrInsertDeclaration(llvmCompileModule.get(), Intrinsic::memset, {sretPtr->getType(), LLVMType::getInt64Ty(*llvmCompileContext)});
                llvmIRBuilder->CreateCall(memsetFn, {sretPtr, ConstantInt::get(LLVMType::getInt8Ty(*llvmCompileContext), 0), szVal, ConstantInt::get(LLVMType::getInt1Ty(*llvmCompileContext), 0)});
                for (auto& [memberName, defaultNode] : memberDefaultNodes) {
                    auto idxIt = memberNameIndexes.find(memberName);
                    if (idxIt == memberNameIndexes.end())
                        continue;
                    uint16_t idx = idxIt->second;
                    LLVMValue* memberPtr = llvmIRBuilder->CreateStructGEP(sTy, sretPtr, idx, memberName + "_init");
                    LLVMValue* defaultVal = (LLVMValue*)(defaultNode->*(defaultNode->codegen))(pass);
                    if (wasError) {
                        messageSystem::endBlock();
                        return nullptr;
                    }
                    if (!defaultVal)
                        continue;
                    bool isSigned = typeSigns.count(members[idx].typeString) ? typeSigns[members[idx].typeString] : false;
                    defaultVal = castValue(defaultVal, sTy->getElementType(idx), true, isSigned, this);
                    if (wasError) {
                        messageSystem::endBlock();
                        return nullptr;
                    }
                    llvmIRBuilder->CreateStore(defaultVal, memberPtr);
                }
                llvmIRBuilder->CreateRetVoid();
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
        structDefinitions[structName]->memberNameTypeNodes = memberNameTypeNodes;

        // Auto-generate a default constructor if no constructor for this struct exists yet
        auto addDefaultCtorProto = [&](StructType* ty) {
            argumentList emptyUserArgs;
            if (!getExactFunctionFromID(functionIDs, const_cast<std::string&>(structName), emptyUserArgs)) {
                std::vector<Type*> ctorArgTypes = {PointerType::get(*llvmCompileContext, 0)};
                FunctionType* FT = FunctionType::get(LLVMType::getVoidTy(*llvmCompileContext), ctorArgTypes, false);
                Function* fn = Function::Create(FT, Function::InternalLinkage, structName, llvmCompileModule.get());
                fn->addFnAttr(llvm::Attribute::AlwaysInline);
                fn->getArg(0)->setName("sret");
                argumentList llvmArgs = {argType("*" + structName, Struct_Type, 1, false, true)};
                functionIDs.push_back(new functionID(structName, this, token, structName, structName, llvmArgs, emptyUserArgs, fn, false, false, true));
                functionIDs.back()->isReplaceable = true;
            }
        };
        addDefaultCtorProto(existingTy);
        messageSystem::endBlock();
        return existingTy;
    }

    StructType* structTy = StructType::create(*llvmCompileContext, fieldTypes, "struct." + structName);

    currentStructName.pop();
    auto newStructDef = new AsaStruct(structName, token, structTy, members, memberFunctions, memberNameIndexes);
    newStructDef->memberDefaultNodes = memberDefaultNodes;
    newStructDef->memberNameTypeNodes = memberNameTypeNodes;
    structDefinitions[structName] = newStructDef;

    // Detect @packed / @notpacked attributes for extern call ABI override
    for (auto* attr : attributes) {
        if (attr->token && attr->token->tokenStr == "packed")
            newStructDef->isPacked = true;
        if (attr->token && attr->token->tokenStr == "notpacked")
            newStructDef->isNotPacked = true;
    }

    // Auto-generate a default constructor if no constructor for this struct exists yet
    auto addDefaultCtorProto = [&](StructType* ty) {
        argumentList emptyUserArgs;
        if (!getExactFunctionFromID(functionIDs, const_cast<std::string&>(structName), emptyUserArgs)) {
            std::vector<Type*> ctorArgTypes = {PointerType::get(*llvmCompileContext, 0)};
            FunctionType* FT = FunctionType::get(LLVMType::getVoidTy(*llvmCompileContext), ctorArgTypes, false);
            Function* fn = Function::Create(FT, Function::InternalLinkage, structName, llvmCompileModule.get());
            fn->addFnAttr(llvm::Attribute::AlwaysInline);
            fn->getArg(0)->setName("sret");
            argumentList llvmArgs = {argType("*" + structName, Struct_Type, 1, false, true)};
            functionIDs.push_back(new functionID(structName, this, token, structName, structName, llvmArgs, emptyUserArgs, fn, false, false, true));
            functionIDs.back()->isReplaceable = true;
        }
    };
    addDefaultCtorProto(structTy);
    messageSystem::endBlock();
    return structTy;
}

// Register all enum variants as compile-time integer constants.
// Variants with an explicit `= value` use that value; others auto-increment from the previous.
// After this runs, `someEnum.Variant` member-access resolves via compilerDefinitions + moduleRegistry.
void* ASTNode::generateEnum(int pass)
{
    messageSystem::startBlock(this, "Generating enum definition", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Only populate once, regardless of which pass triggers the call.
    // Root-level enums are called from generateOutputCode (all passes);
    // function-body enums are called from generateScopeBody during pass 2.
    if (currentNodeDoneGenerating) {
        messageSystem::endBlock();
        return nullptr;
    }
    currentNodeDoneGenerating = true;

    std::string enumName = token->tokenStr;
    moduleRegistry[enumName] = this;

    // Recursive constant-expression evaluator (handles unary minus, binary +/-/*, identifiers).
    std::function<int64_t(ASTNode*, bool&)> evalConst = [&](ASTNode* n, bool& ok) -> int64_t {
        if (!n) {
            ok = false;
            return 0;
        }
        if ((n->nodeType == Expression_Term || n->nodeType == Expression_Paren_Term) &&
            n->childNodes.size() == 1)
            return evalConst(n->childNodes[0], ok);
        if (n->nodeType == Integer_Node || n->nodeType == SInt32_Type || n->nodeType == SInt64_Type) {
            try {
                return std::stoll(n->token->tokenStr);
            }
            catch (...) {
                ok = false;
                return 0;
            }
        }
        if (n->nodeType == Expression_Minus && n->childNodes.size() == 1)
            return -evalConst(n->childNodes[0], ok);
        if (n->nodeType == Expression_Plus && n->childNodes.size() == 2)
            return evalConst(n->childNodes[0], ok) + evalConst(n->childNodes[1], ok);
        if (n->nodeType == Expression_Minus && n->childNodes.size() == 2)
            return evalConst(n->childNodes[0], ok) - evalConst(n->childNodes[1], ok);
        if (n->nodeType == Expression_Times && n->childNodes.size() == 2)
            return evalConst(n->childNodes[0], ok) * evalConst(n->childNodes[1], ok);
        if (n->nodeType == Identifier_Node) {
            auto it = compilerDefinitions.find(n->token->tokenStr);
            if (it != compilerDefinitions.end())
                return evalConst(it->second, ok);
        }
        ok = false;
        return 0;
    };

    // Inferred step state
    int64_t currentValue = 0;
    int64_t step = 1;
    bool doubleMode = false;  // true = multiply by 2 instead of adding step
    bool hasPrev = false;
    bool prevWasExplicit = false;
    bool prevExplicitWasReset = false;  // last explicit followed an auto (was a "jump reset")
    int64_t prevValue = 0;
    int64_t nextAutoValue = 0;  // the value the next auto variant will receive

    // Variants live in the Scope_Body child (childNodes[0]) added during parsing
    auto& variantList = (!childNodes.empty() && childNodes[0]->nodeType == Scope_Body)
                            ? childNodes[0]->childNodes
                            : childNodes;
    for (auto* variant : variantList) {
        std::string variantName = variant->token->tokenStr;
        bool isExplicitVariant = !variant->childNodes.empty();

        if (isExplicitVariant) {
            bool ok = true;
            int64_t val = evalConst(variant->childNodes[0], ok);
            if (ok)
                currentValue = val;

            if (hasPrev && prevWasExplicit && !prevExplicitWasReset) {
                // Two consecutive explicit values where neither is a jump-reset: infer step pattern.
                int64_t diff = currentValue - prevValue;
                // Detect binary doubling only when it conflicts with the current additive step.
                if (prevValue > 0 && currentValue == prevValue * 2 && currentValue != prevValue + step) {
                    doubleMode = true;
                }
                else {
                    doubleMode = false;
                    step = diff;
                }
                prevExplicitWasReset = false;
            }
            else if (hasPrev && !prevWasExplicit) {
                // Explicit value after auto values: reset to additive step=1 and mark as reset.
                step = 1;
                doubleMode = false;
                prevExplicitWasReset = true;
            }
            else {
                // Explicit after an explicit that was itself a reset: don't infer, clear reset flag.
                prevExplicitWasReset = false;
            }
            prevWasExplicit = true;
        }
        else {
            // Auto variant: use the pre-computed nextAutoValue.
            currentValue = nextAutoValue;
            prevWasExplicit = false;
        }

        // Build synthetic Integer_Node for this variant.
        ASTNode* intNode = new ASTNode();
        intNode->nodeType = Integer_Node;
        intNode->token = new asaToken(std::to_string(currentValue), Integer);
        intNode->codegen = &ASTNode::generateConstant;
        compilerDefinitions[variantName] = intNode;

        // Pre-compute the value the NEXT auto variant would receive.
        if (doubleMode)
            nextAutoValue = (currentValue > 0) ? currentValue * 2 : currentValue + 1;
        else
            nextAutoValue = currentValue + step;

        prevValue = currentValue;
        hasPrev = true;
    }

    messageSystem::endBlock();
    return nullptr;
}

// LLVMValue*
void* ASTNode::generateBreak(int pass)
{
    messageSystem::startBlock(this, "Generating break statement", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Check if we have a label. The argument is wrapped in an Expression_Term by
    // getStatementArgument, so the Identifier_Node is one level down.
    std::string targetLabel = "";
    if (childNodes.size() > 0) {
        ASTNode* arg = childNodes[0];
        if (arg->nodeType == Identifier_Node)
            targetLabel = arg->token->tokenStr;
        else if (arg->nodeType == Expression_Term && !arg->childNodes.empty() &&
                 arg->childNodes[0]->nodeType == Identifier_Node)
            targetLabel = arg->childNodes[0]->token->tokenStr;
    }

    // Make sure we're inside a loop
    if (loopContextStack.empty()) {
        return messageSystem::error("Break statement must be inside a loop");
    }

    // If no label, break from the innermost loop
    if (targetLabel.empty()) {
        BasicBlock* breakBB = loopContextStack.top().breakBB;
        llvmIRBuilder->CreateBr(breakBB);

        // Create a new unreachable block for any code after the break
        Function* TheFunction = llvmIRBuilder->GetInsertBlock()->getParent();
        BasicBlock* afterBreak = BasicBlock::Create(*llvmCompileContext, "after_break", TheFunction);
        llvmIRBuilder->SetInsertPoint(afterBreak);

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

    llvmIRBuilder->CreateBr(targetBreakBB);

    // Create a new unreachable block for any code after the break
    Function* TheFunction = llvmIRBuilder->GetInsertBlock()->getParent();
    BasicBlock* afterBreak = BasicBlock::Create(*llvmCompileContext, "after_break", TheFunction);
    llvmIRBuilder->SetInsertPoint(afterBreak);

    messageSystem::endBlock();
    return nullptr;
}

// LLVMValue*
void* ASTNode::generateContinue(int pass)
{
    messageSystem::startBlock(this, "Generating continue statement", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Check if we have a label. The argument is wrapped in an Expression_Term by
    // getStatementArgument, so the Identifier_Node is one level down.
    std::string targetLabel = "";
    if (childNodes.size() > 0) {
        ASTNode* arg = childNodes[0];
        if (arg->nodeType == Identifier_Node)
            targetLabel = arg->token->tokenStr;
        else if (arg->nodeType == Expression_Term && !arg->childNodes.empty() &&
                 arg->childNodes[0]->nodeType == Identifier_Node)
            targetLabel = arg->childNodes[0]->token->tokenStr;
    }

    // Make sure we're inside a loop
    if (loopContextStack.empty()) {
        return messageSystem::error("Continue statement must be inside a loop");
    }

    // If no label, continue to the innermost loop
    if (targetLabel.empty()) {
        BasicBlock* continueBB = loopContextStack.top().continueBB;
        llvmIRBuilder->CreateBr(continueBB);

        // Create a new unreachable block for any code after the continue
        Function* TheFunction = llvmIRBuilder->GetInsertBlock()->getParent();
        BasicBlock* afterContinue = BasicBlock::Create(*llvmCompileContext, "after_continue", TheFunction);
        llvmIRBuilder->SetInsertPoint(afterContinue);

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

    llvmIRBuilder->CreateBr(targetContinueBB);

    // Create a new unreachable block for any code after the continue
    Function* TheFunction = llvmIRBuilder->GetInsertBlock()->getParent();
    BasicBlock* afterContinue = BasicBlock::Create(*llvmCompileContext, "after_continue", TheFunction);
    llvmIRBuilder->SetInsertPoint(afterContinue);

    messageSystem::endBlock();
    return nullptr;
}

// Add a generator for Labeled_Loop:
// LLVMValue*
void* ASTNode::generateLabeledLoop(int pass)
{
    messageSystem::startBlock(this, "Generating labeled loop", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::string label = token->tokenStr;

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
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }

    messageSystem::endBlock();
    return generatedLoop;
}

// Now update the generateFor function to use the label:
// LLVMValue*
void* ASTNode::generateFor(int pass)
{
    messageSystem::startBlock(this, "Generating for loop", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (!LexicalBlocks.empty() && token && token->filePath)
        llvmIRBuilder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));
    std::string varName = "_iterator";
    std::string label = "";  // Optional label for the loop

    // Check if this loop has a label (set by generateLabeledLoop)
    if (!this->label.empty()) {
        label = this->label;
    }

    if (childNodes[0]->nodeType == Iterator) {
        varName = childNodes[0]->childNodes[0]->token->tokenStr;
    }

    ASTNode* rangeStart = childNodes[1]->childNodes[0];
    ASTNode* rangeEnd = childNodes[1]->childNodes[1];

    // Compute start value.
    if (rangeStart->codegen == nullptr) {
        return messageSystem::error("Node `" + ASTNodeTypeAsString(rangeStart->nodeType) + "` does not have a code generator");
    }
    LLVMValue* StartVal = (LLVMValue*)(rangeStart->*(rangeStart->codegen))(pass);
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
    LLVMValue* EndVal = (LLVMValue*)(rangeEnd->*(rangeEnd->codegen))(pass);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }
    if (!EndVal) {
        messageSystem::endBlock();
        return nullptr;
    }

    // Determine iterator type: use explicit annotation if provided, otherwise widen from range.
    LLVMType* iterType = nullptr;
    if (childNodes[0]->nodeType == Iterator && childNodes[0]->childNodes.size() >= 2) {
        // Explicit type annotation: for(i : uint16 in ...)
        ASTNode* typeNode = childNodes[0]->childNodes[1];
        bool wasDefined = false;
        int resolvePass = pass;
        iterType = getLLVMTypeFromString(typeNode->token->tokenStr, 0, typeNode->token, wasDefined, resolvePass);
    }
    if (!iterType) {
        // Auto-determine: widest integer type among start and end, minimum i32.
        unsigned iterBits = 32;
        if (StartVal->getType()->isIntegerTy())
            iterBits = std::max(iterBits, StartVal->getType()->getIntegerBitWidth());
        if (EndVal->getType()->isIntegerTy())
            iterBits = std::max(iterBits, EndVal->getType()->getIntegerBitWidth());
        iterType = LLVMType::getIntNTy(*llvmCompileContext, iterBits);
    }

    // Cast start and end to the iterator type if needed.
    if (StartVal->getType() != iterType) {
        std::string tyStr = getStringTypeFromLLVMType(StartVal->getType());
        StartVal = llvmIRBuilder->CreateIntCast(StartVal, iterType, typeSigns.count(tyStr) ? typeSigns[tyStr] : true, "startcast");
    }
    if (EndVal->getType() != iterType) {
        std::string tyStr = getStringTypeFromLLVMType(EndVal->getType());
        EndVal = llvmIRBuilder->CreateIntCast(EndVal, iterType, typeSigns.count(tyStr) ? typeSigns[tyStr] : true, "endcast");
    }

    // Make the new basic block for the loop header, inserting after current block.
    Function* TheFunction = llvmIRBuilder->GetInsertBlock()->getParent();
    BasicBlock* PreheaderBB = llvmIRBuilder->GetInsertBlock();
    BasicBlock* LoopCondBB = BasicBlock::Create(*llvmCompileContext, "loopcond", TheFunction);
    BasicBlock* LoopBB = BasicBlock::Create(*llvmCompileContext, "loop", TheFunction);
    BasicBlock* StepBB = BasicBlock::Create(*llvmCompileContext, "loopstep", TheFunction);
    BasicBlock* AfterBB = BasicBlock::Create(*llvmCompileContext, "afterloop", TheFunction);

    AllocaInst* Alloca = CreateEntryBlockAlloca(TheFunction, iterType, varName);
    // Store the value into the alloca.
    llvmIRBuilder->CreateStore(StartVal, Alloca);

    // Branch to loop condition check
    llvmIRBuilder->CreateBr(LoopCondBB);

    llvmIRBuilder->SetInsertPoint(LoopCondBB);

    LLVMValue* CurVar = llvmIRBuilder->CreateLoad(iterType, Alloca, varName.c_str());

    // Compare: exclusive (i < N)
    LLVMValue* Cond = llvmIRBuilder->CreateICmpSLT(CurVar, EndVal, "loopcond");

    // Conditional branch
    llvmIRBuilder->CreateCondBr(Cond, LoopBB, AfterBB);

    // Start insertion in LoopBB.
    llvmIRBuilder->SetInsertPoint(LoopBB);

    // Push loop context for break/continue support
    LoopContext ctx;
    ctx.continueBB = StepBB;  // Continue goes to step (increment) before condition check
    ctx.breakBB = AfterBB;    // Break goes to after the loop
    ctx.label = label;        // Empty for unlabeled loops
    loopContextStack.push(ctx);

    namedValues[varName] = new valueType(varName, getStringTypeFromLLVMType(iterType), Alloca);

    // Emit the body of the loop
    ASTNode* scopeBody = childNodes[2];

    if (scopeBody->codegen == nullptr) {
        return messageSystem::error("Node `" + ASTNodeTypeAsString(scopeBody->nodeType) + "` does not have a code generator");
    }
    (scopeBody->*(scopeBody->codegen))(pass);
    if (wasError) {
        loopContextStack.pop();  // Clean up context
        messageSystem::endBlock();
        return nullptr;
    }

    // Pop loop context
    loopContextStack.pop();

    // Fall through from body to step block
    llvmIRBuilder->CreateBr(StepBB);

    // Step block: increment iterator then jump back to condition
    llvmIRBuilder->SetInsertPoint(StepBB);
    LLVMValue* StepVal = ConstantInt::get(*llvmCompileContext, APInt(iterType->getIntegerBitWidth(), 1));
    LLVMValue* CurVar2 = llvmIRBuilder->CreateLoad(Alloca->getAllocatedType(), Alloca, varName.c_str());
    LLVMValue* NextVar = llvmIRBuilder->CreateAdd(CurVar2, StepVal, "nextvar");
    llvmIRBuilder->CreateStore(NextVar, Alloca);
    llvmIRBuilder->CreateBr(LoopCondBB);

    // After loop
    llvmIRBuilder->SetInsertPoint(AfterBB);

    messageSystem::endBlock();
    return nullptr;
}

// Similarly, if you have a while loop generator, update it too:
// LLVMValue*
void* ASTNode::generateWhile(int pass)
{
    messageSystem::startBlock(this, "Generating while loop", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (!LexicalBlocks.empty() && token)
        llvmIRBuilder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));

    std::string label = "";  // Optional label

    // Check if this loop has a label (set by generateCompilerDefine)
    if (!this->label.empty()) {
        label = this->label;
    }

    Function* TheFunction = llvmIRBuilder->GetInsertBlock()->getParent();

    BasicBlock* LoopCondBB = BasicBlock::Create(*llvmCompileContext, "whilecond", TheFunction);
    BasicBlock* LoopBB = BasicBlock::Create(*llvmCompileContext, "whileloop", TheFunction);
    BasicBlock* AfterBB = BasicBlock::Create(*llvmCompileContext, "afterwhile", TheFunction);

    // Branch to condition check
    llvmIRBuilder->CreateBr(LoopCondBB);
    llvmIRBuilder->SetInsertPoint(LoopCondBB);

    // Evaluate condition
    ASTNode* condExpr = childNodes[0];
    if (condExpr->childNodes.size() == 0) {
        messageSystem::startBlock(condExpr, "Generating condition expression", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
        return messageSystem::error("Expected condition expression");
    }
    condExpr = condExpr->childNodes[0];

    if (condExpr->codegen == nullptr) {
        return messageSystem::error("Node `" + ASTNodeTypeAsString(condExpr->nodeType) + "` does not have a code generator");
    }

    LLVMValue* CondV = (LLVMValue*)(condExpr->*(condExpr->codegen))(pass);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }
    if (!CondV) {
        messageSystem::endBlock();
        return nullptr;
    }

    // Convert condition to bool
    CondV = llvmIRBuilder->CreateICmpNE(CondV, ConstantInt::get(*llvmCompileContext, APInt(1, 0)), "whilecond");

    // Conditional branch
    llvmIRBuilder->CreateCondBr(CondV, LoopBB, AfterBB);

    // Loop body
    llvmIRBuilder->SetInsertPoint(LoopBB);

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
    llvmIRBuilder->CreateBr(LoopCondBB);

    // After loop
    llvmIRBuilder->SetInsertPoint(AfterBB);

    messageSystem::endBlock();
    return nullptr;
}

// Function*
void* ASTNode::generatePrototype(int pass)
{
    messageSystem::startBlock(this, "Generating prototype", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    argumentList argList = argumentList();
    std::vector<Type*> argTypes = std::vector<Type*>();
    std::vector<int> byvalParamIndices;
    std::vector<Type*> byvalParamTypes;
    ASTNode* argsNode = childNodes[2];
    ASTNode* modifiersNode = childNodes[3];
    std::vector<std::string> argNames = std::vector<std::string>();
    std::string fnName = token->tokenStr;
    std::string mangledName = token->tokenStr;
    bool isStruct = false;
    bool isSpecial = false;

    if (fnName == "operator") {
        isSpecial = true;
        fnName = fnName + "." + tokenAsString(childNodes[0]->childNodes[0]->token->tokenType);
        mangledName = fnName + "." + tokenAsString(childNodes[0]->childNodes[0]->token->tokenType);
    }
    else if (fnName == "cast" || fnName == "create" || fnName == "destroy") {
        isSpecial = true;
        fnName = fnName + "." + tokenAsString(childNodes[0]->childNodes[0]->token->tokenType);
        mangledName = fnName + "." + tokenAsString(childNodes[0]->childNodes[0]->token->tokenType);
    }
    else if (currentStructName.size() != 0) {
        fnName = currentStructName.top() + "." + fnName;
        mangledName = currentStructName.top() + "." + mangledName;
        isStruct = true;
    }
    //else
    //  fnName = fnName;

    // Get return type
    LLVMType* retType = LLVMType::getVoidTy(*llvmCompileContext);
    std::string rTypeString = "";
    ASTNode* typeNode = childNodes[1];
    bool isStructReturn = false;
    int8_t externReturnCoercionCount = 0;
    bool externReturnCoercionIsFloat = false;
    if (typeNode->childNodes.size() > 0) {
    recurseAddPointer:
        typeNode = typeNode->childNodes[0];
        // TODO: implement return type modifiers (const, ref, exact); for now, skip them
        if (typeNode->token->tokenStr == "const" || typeNode->token->tokenStr == "ref" || typeNode->token->tokenStr == "exact") {
            if (typeNode->childNodes.size() > 0)
                goto recurseAddPointer;
        }
        if (fnName == "main") {
            // main is the C entry point and must not be name-mangled
        }
        else if (typeNode->token->tokenStr == "*")
            mangledName += ".ptr";
        else
            mangledName += "." + typeNode->token->tokenStr;
        rTypeString += typeNode->token->tokenStr;

        if (typeNode->token->tokenStr == "*") {
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
                    unsigned bits = llvmCompileModule->getDataLayout().getTypeAllocSizeInBits(retType);
                    retType = LLVMType::getIntNTy(*llvmCompileContext, bits);
                }
                else {
                    // x86-64 SysV ABI: small structs (<=16 bytes) are returned in registers,
                    // not via sret. Coerce to INTEGER (i64s) or SSE (doubles) chunks.
                    uint64_t size = llvmCompileModule->getDataLayout().getTypeAllocSize(retType);
                    if (size > 16) {
                        // MEMORY class: use sret
                        isStructReturn = true;
                    }
                    else if (size > 0) {
                        StructType* st = cast<StructType>(retType);
                        bool allFloat = true;
                        for (auto* el : st->elements())
                            if (!el->isFloatTy()) {
                                allFloat = false;
                                break;
                            }
                        int numChunks = (int)((size + 7) / 8);
                        // Build coerced return type: { i64, i64 } or { double, double }
                        std::vector<Type*> chunkTypes;
                        for (int k = 0; k < numChunks; k++)
                            chunkTypes.push_back(allFloat ? (LLVMType*)Type::getDoubleTy(*llvmCompileContext) : (LLVMType*)Type::getInt64Ty(*llvmCompileContext));
                        retType = StructType::get(*llvmCompileContext, chunkTypes);
                        // These will be recorded on the functionID after it is constructed (see below)
                        externReturnCoercionCount = (int8_t)numChunks;
                        externReturnCoercionIsFloat = allFloat;
                    }
                }
            }
            else {
                isStructReturn = true;
            }
        }
    }

    // Like C, main always returns i32 even when declared without a return type.
    if (fnName == "main" && retType->isVoidTy())
        retType = LLVMType::getInt32Ty(*llvmCompileContext);

    argumentList userArgList = argList;

    // Handle struct return by modifying function signature
    LLVMType* actualRetType = retType;
    if (isStructReturn) {
        // For struct returns, add sret parameter as first argument
        argTypes.push_back(PointerType::get(*llvmCompileContext, 0));  // sret parameter (pointer to struct)
        argNames.push_back("sret");
        argList.insert(argList.begin(), argType("*" + rTypeString, getASTNodeTypeFromString(rTypeString), 1, false, true));

        // Change actual return type to void
        actualRetType = LLVMType::getVoidTy(*llvmCompileContext);
    }

    // Get function arguments
    // If it is a struct member function, first add a "this" argument like: (this : ref structName, ...)
    if (currentStructName.size() > 0) {
        std::string typeStr = currentStructName.top();
        bool isReference = true;
        int pointerLevel = 0;  // References don't count as pointer level

        LLVMType* aType = nullptr;
        argList.push_back(argType(typeStr, Struct_Type, pointerLevel, isReference, false, false));

        try {
            bool wasDefined = true;
            aType = getLLVMTypeFromString(typeStr, 0, token, wasDefined, pass);
            //if (wasDefined == false)
            //  return nullptr;
            for (int i = 0; i < pointerLevel; i++) {
                aType = PointerType::get(*llvmCompileContext, 0);
            }
            // Add pointer level for reference (LLVM representation)
            if (isReference) {
                aType = PointerType::get(*llvmCompileContext, 0);
            }
        }
        catch (...) {
            return messageSystem::error("Invalid argument type given");
        }

        // Unknown type name
        // TODO: Add handling for custom structs as well

        //else if (typeName == "string")
        //Type::getStringTy(*llvmCompileContext);
        argTypes.push_back(aType);
        argNames.push_back("this");
    }
    bool variableNumArguments = false;
    for (auto& a : argsNode->childNodes) {  // `a` is the expression term containing the entire argument as an expression
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
                    messageSystem::startBlock(nameNode, "Generating argument name", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                    return messageSystem::error("Parameter name must be a plain singular identifier");
                }
                std::string typeStr = "";
                bool isReference = false;
                bool mustBeExactType = false;
                bool isConstant = false;
                int pointerLevel = 0;

            gatherTypeModifiers:
                if (typeNode->token->tokenStr == "ref") {
                    isReference = true;
                    mangledName += ".ref";
                    // Don't modify typeStr or pointerLevel - references are tracked separately
                    typeNode = typeNode->childNodes[0];
                    goto gatherTypeModifiers;
                }
                if (typeNode->token->tokenStr == "exact") {
                    //mangledName += ".exact";
                    //typeStr += ".exact";
                    mustBeExactType = true;
                    typeNode = typeNode->childNodes[0];
                    goto gatherTypeModifiers;
                }
                if (typeNode->token->tokenStr == "const") {
                    isConstant = true;
                    typeNode = typeNode->childNodes[0];
                    goto gatherTypeModifiers;
                }
                if (typeNode->token->tokenStr == "*") {
                    pointerLevel++;
                    mangledName += ".ptr";
                    // pointerLevel tracks pointer depth; typeStr holds only the base type name
                    typeNode = typeNode->childNodes[0];
                    goto gatherTypeModifiers;
                }

                //// Function arguments should only be constant if they are non-local, so a reference
                //if (isConstant && !isReference)
                //  isConstant = false;

                mangledName += "." + typeNode->token->tokenStr;
                typeStr += typeNode->token->tokenStr;


                LLVMType* aType = nullptr;
                argType arg = argType(typeStr, getASTNodeTypeFromString(typeNode->token->tokenStr), pointerLevel, isReference, mustBeExactType, isConstant);
                if (a->childNodes.size() > 1) {
                    arg.hasDefault = true;
                    arg.defaultNode = a->childNodes[1];
                    arg.defaultRawTokens = a->childNodes[1]->defaultRawTokens;
                }
                argList.push_back(arg);
                userArgList.push_back(arg);

                try {
                    bool wasDefined = true;
                    aType = getLLVMTypeFromString(typeNode->token->tokenStr, 0, typeNode->token, wasDefined, pass);

                    for (int i = 0; i < pointerLevel; i++) {
                        aType = PointerType::get(*llvmCompileContext, 0);
                    }

                    // If this is a reference, add an extra pointer level for LLVM representation
                    // (references are implemented as pointers in LLVM)
                    if (isReference) {
                        aType = PointerType::get(*llvmCompileContext, 0);
                    }
                }
                catch (...) {
                    goto invalidArgument;
                }

                // Unknown type name
                // TODO: Add handling for custom structs as well

                //else if (typeName == "string")
                //Type::getStringTy(*llvmCompileContext);
                // Extern struct ABI: auto-detect x86-64 SysV passing convention.
                // @packed overrides to force integer packing; @notpacked forces auto-detect.
                if (isExtern && aType && aType->isStructTy() && pointerLevel == 0 && !isReference) {
                    auto it = structDefinitions.find(typeStr);
                    if (it != structDefinitions.end()) {
                        bool forcePackedInt = it->second->isPacked && !it->second->isNotPacked;
                        uint64_t size = llvmCompileModule->getDataLayout().getTypeAllocSize(aType);
                        if (forcePackedInt) {
                            // @packed: pack entire struct into a single integer
                            unsigned bits = llvmCompileModule->getDataLayout().getTypeAllocSizeInBits(aType);
                            aType = LLVMType::getIntNTy(*llvmCompileContext, bits);
                        }
                        else if (size > 16) {
                            // MEMORY class: pass via pointer (byval)
                            byvalParamIndices.push_back((int)argTypes.size());
                            byvalParamTypes.push_back(aType);
                            aType = PointerType::getUnqual(*llvmCompileContext);
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
                                    argTypes.push_back(LLVMType::getDoubleTy(*llvmCompileContext));
                                    argNames.push_back(nameNode->token->tokenStr + (k == 0 ? "" : "_" + std::to_string(k)));
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
                                    argTypes.push_back(LLVMType::getInt64Ty(*llvmCompileContext));
                                    argNames.push_back(nameNode->token->tokenStr + (k == 0 ? "" : "_" + std::to_string(k)));
                                }
                                continue;  // skip push below
                            }
                        }
                    }
                }
                argTypes.push_back(aType);
                argNames.push_back(nameNode->token->tokenStr);
                //std::cout << "Arg added: '" << a->childNodes[0]->token->tokenStr << "' of type: '" << typeName << "'\n";
            }
            // Handle ellipses ...
            else if (a->childNodes[0]->nodeType == Argument_List) {
                //std::string typeName = a->childNodes[0]->childNodes[0]->token->tokenStr;
                variableNumArguments = true;
            }
            else
                goto invalidArgument;
            continue;
        invalidArgument:
            messageSystem::startBlock(a->childNodes[0], "Generating argument", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
            return messageSystem::error("Invalid function parameter syntax (expected 'name : type', got something else)");
        }
    }
    bool isAlwaysInline = false;
    bool forceExternal = false;
    bool forceInternal = false;
    bool definitionIsReplaceable = getAttributeValue(this, "replaceable") == "true";
    for (auto& m : modifiersNode->childNodes) {
        if (m->token->tokenStr == "#replaceable")
            definitionIsReplaceable = true;
    }
    for (auto* attr : attributes) {
        if (attr->token && attr->token->tokenStr == "inline")
            isAlwaysInline = true;
        if (attr->token && attr->token->tokenStr == "external")
            forceExternal = true;
        if (attr->token && attr->token->tokenStr == "internal")
            forceInternal = true;
    }


    // Don't add another prototype if the exact same one is already defined
    //Function* theFunction = llvmCompileModule->getFunction(token->tokenStr);
    functionID* theFunctionID = getExactFunctionFromID(functionIDs, fnName, userArgList);
    if (theFunctionID) {
        if (verbosity >= 5) {
            console::printIndent(2);
            console::write("-- Pre-existing function definition found for: ");
            console::write(fnName, console::yellowFGColor);
            console::writeLine(" (" + theFunctionID->mangledName + ")", console::yellowFGColor);
            //theFunctionID->print();
        }
        messageSystem::endBlock();
        return theFunctionID->fnValue;
    }


    FunctionType* FT = FunctionType::get(actualRetType, argTypes, variableNumArguments);

    // Determine linkage: extern declarations and @external always use ExternalLinkage.
    // main must be external so the C runtime can find it.
    // Everything else defaults to InternalLinkage so the optimizer can DCE unused symbols,
    // unless @internal/@external explicitly overrides the default.
    auto pickLinkage = [&](bool defaultExternal) -> Function::LinkageTypes {
        if (forceExternal)
            return Function::ExternalLinkage;
        if (forceInternal)
            return Function::InternalLinkage;
        return defaultExternal ? Function::ExternalLinkage : Function::InternalLinkage;
    };

    Function* fn = nullptr;
    // If extern declaration, dont mangle name; use externSymbolName if an alias was given
    if (isExtern) {
        std::string llvmSymbol = externSymbolName.empty() ? fnName : externSymbolName;
        fn = Function::Create(FT, pickLinkage(true), llvmSymbol, llvmCompileModule.get());
    }
    else if (fnName == "main") {
        // main must be ExternalLinkage so the C runtime entry point can find it
        fn = Function::Create(FT, pickLinkage(true), mangledName, llvmCompileModule.get());
    }
    else
        fn = Function::Create(FT, pickLinkage(false), mangledName, llvmCompileModule.get());
    if (isAlwaysInline)
        fn->addFnAttr(llvm::Attribute::AlwaysInline);

    // Apply byval attributes to large struct params in extern declarations
    for (int bi = 0; bi < (int)byvalParamIndices.size(); bi++)
        fn->addParamAttr(byvalParamIndices[bi], Attribute::getWithByValType(*llvmCompileContext, byvalParamTypes[bi]));

    uint16_t Idx = 0;
    for (auto& arg : fn->args()) {
        arg.setName(argNames[Idx++]);

        if (argList[Idx - 1].isConstant) {
            LLVMType* argType = arg.getType();

            // readonly can only be applied to pointer types
            if (argType->isPointerTy()) {
                arg.addAttr(llvm::Attribute::ReadOnly);
                // Optionally also add NoCapture to indicate the pointer isn't stored
                // arg.addAttr(llvm::Attribute::NoCapture);
            }
        }
    }

    functionIDs.push_back(new functionID(fnName, this, token, mangledName, rTypeString, argList, userArgList, fn, variableNumArguments, isStruct, isStructReturn));
    functionIDs.back()->externReturnCoercionCount = externReturnCoercionCount;
    functionIDs.back()->externReturnCoercionIsFloat = externReturnCoercionIsFloat;
    //functionIDs.back()->declNode = this;
    if (verbosity >= 5) {
        console::printIndent(depth + 2);
        console::writeLine("-- Added function \"" + fnName + "\" to functionIDs");
    }

    messageSystem::endBlock();
    return fn;
}

// Function*
void* ASTNode::generateFunction(int pass)
{
    messageSystem::startBlock(this, "Defining function", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // First, check for an existing function from a previous declaration.
    //Function* theFunction = llvmCompileModule->getFunction(token->tokenStr);
    functionID* theFunctionID = nullptr;
    Function* theFunction = nullptr;
    std::string functionName = token->tokenStr;
    bool isStruct = false;

    if (currentStructName.size() > 0) {
        functionName = currentStructName.top() + "." + functionName;
        isStruct = true;
    }

    // If this function has variant parameters, store it as a template and
    // skip normal LLVM generation. It will be instantiated on demand at each call site.
    if (childNodes.size() > 4 && !childNodes[4]->childNodes.empty()) {
        variantFunctionTemplates[token->tokenStr] = this;
        messageSystem::endBlock();
        return nullptr;
    }

    //if (!theFunctionID)
    theFunction = (Function*)this->generatePrototype(pass);
    //else
    //  theFunction = theFunctionID->fnValue;

    // TODO: This should use the out-of-order compilation system
    // If the function wasn't generated, try later
    if (!theFunction) {
        return messageSystem::error("There was a failure to create a function");
    }

    bool definitionIsReplaceable = getAttributeValue(this, "replaceable") == "true";
    for (auto* child : childNodes) {
        if (!child || child->nodeType != Compiler_Modifiers)
            continue;
        for (auto* modifier : child->childNodes) {
            if (modifier && modifier->token && modifier->token->tokenStr == "#replaceable") {
                definitionIsReplaceable = true;
                break;
            }
        }
    }

    if (!theFunction->empty()) {
        theFunctionID = getFunctionIDFromFunctionPointer(functionIDs, theFunction);
        bool existingIsReplaceable = theFunctionID && theFunctionID->isReplaceable;
        if (!definitionIsReplaceable && !existingIsReplaceable) {
            if (theFunctionID)
                messageSystem::addAttribute(theFunctionID->declNode, "previously defined here");
            return messageSystem::error("Function cannot be redefined, requires unique identity", messageSystem::Redefined_Error);
            return nullptr;
        }
        // Either this function is marked @replaceable, or the existing one is auto-generated
        // (isReplaceable). Either way, clear the old body and redefine.
        theFunction->deleteBody();
        // Keep the functionID in the list (its LLVM fn pointer and signature are still correct).
        // Just mark it as no longer replaceable and update its declaration node.
        if (theFunctionID) {
            theFunctionID->isReplaceable = false;
            theFunctionID->declNode = this;
        }
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
                         : llvmDebugFile;

    // Create subroutine type
    SmallVector<Metadata*, 8> EltTys;
    DIType* DblTy = llvmDebugBuilder->createBasicType("double", 64, dwarf::DW_ATE_float);
    EltTys.push_back(DblTy);  // Add return type and parameters as needed

    DISubroutineType* SubroutineType = llvmDebugBuilder->createSubroutineType(
        llvmDebugBuilder->getOrCreateTypeArray(EltTys));

    // Create function debug info
    DISubprogram* SP = llvmDebugBuilder->createFunction(
        FnFile,                         // Scope
        functionName,                   // Name
        StringRef(),                    // Linkage name
        FnFile,                         // File
        LineNo,                         // Line number
        SubroutineType,                 // Type
        ScopeLine,                      // Scope line
        DINode::FlagPrototyped,         // Flags
        DISubprogram::SPFlagDefinition  // SP Flags
    );

    theFunction->setSubprogram(SP);
    LexicalBlocks.push_back(SP);

    // Create entry block and emit debug location
    BasicBlock* fnBlock = BasicBlock::Create(*llvmCompileContext, "entry", theFunction);
    // Clear any stale debug location from a previously generated function before
    // setting the insert point, so no instructions inherit the wrong subprogram.
    llvmIRBuilder->SetCurrentDebugLocation(DebugLoc());
    llvmIRBuilder->SetInsertPoint(fnBlock);

    // Call the global variable init function at the start of main
    if (functionName == "main" && globalInitFn)
        llvmIRBuilder->CreateCall(globalInitFn, {});

    // Set debug location for entry
    llvmIRBuilder->SetCurrentDebugLocation(
        DILocation::get(SP->getContext(), LineNo, 0, SP));


    // Add remaining arguments
    int i = 0;
    for (auto& arg : theFunction->args()) {
        if (i >= theFunctionID->arguments.size()) {
            //theFunctionID->print(); // TODO: Add this information to message attribute if necessary
            return messageSystem::error("Mismatch in number of arguments vs function signature: " + std::to_string(theFunctionID->arguments.size()));
        }

        // Always create an alloca for the incoming argument so we'll have addressable storage
        AllocaInst* Alloca = CreateEntryBlockAlloca(theFunction, arg.getType(), arg.getName());
        // Store the incoming argument value into the alloca
        llvmIRBuilder->CreateStore(&arg, Alloca);
        // For const parameters, mark the alloca as invariant after the initial store
        if (theFunctionID->arguments[i].isConstant) {
            uint64_t typeSize = llvmCompileModule->getDataLayout().getTypeAllocSize(arg.getType());
            Function* invariantStartFn = Intrinsic::getOrInsertDeclaration(llvmCompileModule.get(), Intrinsic::invariant_start, {PointerType::getUnqual(*llvmCompileContext)});
            llvmIRBuilder->CreateCall(invariantStartFn, {ConstantInt::get(LLVMType::getInt64Ty(*llvmCompileContext), typeSize), Alloca});
        }

        // Build the "actual type" string the rest of your compiler expects (pointer stars + type name)
        std::string baseType = "";
        for (int p = 0; p < theFunctionID->arguments[i].pointerLevel; p++)
            baseType += "*";

        valueType* vt = new valueType(std::string(arg.getName()), baseType + theFunctionID->arguments[i].typeString, Alloca, true, theFunctionID->arguments[i].isReference);

        vt->isConstant = theFunctionID->arguments[i].isConstant;

        namedValues[std::string(arg.getName())] = vt;

        // Emit debug info for this parameter so GDB can show argument values
        if (llvmDebugBuilder && !LexicalBlocks.empty()) {
            std::string argTypeStr = baseType + theFunctionID->arguments[i].typeString;
            DIType* DebugType = createDIType(arg.getType(), argTypeStr);
            if (DebugType) {
                DILocalVariable* D = llvmDebugBuilder->createParameterVariable(
                    SP,
                    std::string(arg.getName()),
                    i + 1,  // 1-indexed
                    FnFile,
                    LineNo,
                    DebugType,
                    true  // AlwaysPreserve
                );
                llvmDebugBuilder->insertDeclare(
                    Alloca,
                    D,
                    llvmDebugBuilder->createExpression(),
                    DILocation::get(SP->getContext(), LineNo, 0, SP),
                    llvmIRBuilder->GetInsertBlock());
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
    if (!llvmIRBuilder->GetInsertBlock()->getTerminator()) {
        if (theFunction->getReturnType()->isVoidTy())
            llvmIRBuilder->CreateRetVoid();
        else
            llvmIRBuilder->CreateRet(Constant::getNullValue(theFunction->getReturnType()));
    }

    // Pop this function's scope now that its body is fully generated.
    LexicalBlocks.pop_back();

    // Validate the generated code, checking for consistency.
    verifyFunction(*theFunction);

    //// Optimize the function. // This causes issues
    //if (optimizationLevel >= 1)
    //  TheFPM->run(*theFunction, *TheFAM);

    messageSystem::endBlock();
    return theFunction;
}

// Nothing
void* ASTNode::generateNothing(int pass)
{
    return nullptr;
}

// #setflag NAME VALUE;
// Sets a compiler-time flag in compilerFlags. Processed on every pass so
// that flags are correctly scoped during each codegen phase.
void* ASTNode::generateCompilerFlagDirective(int pass)
{
    messageSystem::startBlock(this, "Generating `#setflag` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (childNodes.size() < 2)
        return messageSystem::error("#setflag requires name and value arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    // childNodes[0] = "setflag" keyword node
    // childNodes[1] = scope body containing NAME and VALUE tokens
    // Walk the body's tree collecting leaf tokens in order
    std::vector<std::string> tokens;
    std::function<void(ASTNode*)> collectTokens = [&](ASTNode* n) {
        if (n->token && !n->token->tokenStr.empty() && n->childNodes.empty())
            tokens.push_back(n->token->tokenStr);
        for (auto& c : n->childNodes)
            collectTokens(c);
    };
    collectTokens(childNodes[1]);

    if (tokens.size() >= 2) {
        std::string value = ToLower(tokens[1]);
        if (value == "true" || value == "1")
            compilerDirectiveFlags[tokens[0]] = true;
        else if (value == "false" || value == "0")
            compilerDirectiveFlags[tokens[0]] = false;
        else
            return messageSystem::error("#setflag value must be a bool", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }
    else
        return messageSystem::error("#setflag requires name and value arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    messageSystem::endBlock();
    pushCompilerDirectiveCall("setflag", this);

    return nullptr;
}

// #getflag(NAME)
// Returns the current compiler-time flag value as a bool. Missing flags read as false.
void* ASTNode::generateCompilerGetFlagDirective(int pass)
{
    if (pass == 0)
        return nullptr;

    messageSystem::startBlock(this, "Generating `#getflag` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty())
        return messageSystem::error("#getflag requires a flag name argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    ASTNode* nameNode = unwrapSingleExpressionNode(args[0]);
    if (!nameNode)
        return messageSystem::error("#getflag requires a flag name argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    if (nameNode->nodeType == Comma_Node && !nameNode->childNodes.empty())
        nameNode = nameNode->childNodes[0];

    while (nameNode && nameNode->nodeType == Expression_Term && nameNode->childNodes.size() == 1)
        nameNode = nameNode->childNodes[0];

    if (!nameNode || !nameNode->token ||
        (nameNode->nodeType != Identifier_Node && nameNode->nodeType != String_Node && nameNode->nodeType != String_Constant_Node))
        return messageSystem::error("#getflag flag name must be an identifier or string literal", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    std::string flagName = nameNode->token->tokenStr;
    if (nameNode->nodeType == String_Node || nameNode->nodeType == String_Constant_Node)
        flagName = decodeQuotedStringToken(nameNode->token);

    auto flag = compilerDirectiveFlags.find(flagName);
    bool flagValue = flag != compilerDirectiveFlags.end() && flag->second;
    LLVMValue* result = ConstantInt::get(LLVMType::getInt1Ty(*llvmCompileContext), flagValue ? 1 : 0);
    if (!asaType)
        asaType = new ASAType(result->getType());
    else
        asaType->baseLLVMType = result->getType();

    messageSystem::endBlock();
    pushCompilerDirectiveCall("getflag", this);
    return result;
}

// #library "name";
// Records a library to pass to the linker as -l<name>. Collected on pass 0 only
// to avoid duplicates across the three codegen passes.
void* ASTNode::generateLibraryDirective(int pass)
{
    if (pass != 1)
        return nullptr;

    messageSystem::startBlock(this, "Generating `#library` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty()) {
        return messageSystem::error("#library argument must be a string library name", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }
    ASTNode* nameNode = unwrapSingleExpressionNode(args[0]);
    if (nameNode->nodeType != String_Node && nameNode->nodeType != String_Constant_Node) {
        return messageSystem::error("#library argument must be a string literal", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }
    std::string libName = decodeQuotedStringToken(nameNode->token);

    messageSystem::endBlock();

    // Add if not already present
    for (auto& l : linkedLibraries)
        if (l == libName)
            return nullptr;
    linkedLibraries.push_back(libName);
    pushCompilerDirectiveCall("library", this);
    return nullptr;
}

// #library_static "path/to/lib.a";
// Records a static library path to pass verbatim to the linker. Collected on pass 1 only.
void* ASTNode::generateLibraryStaticDirective(int pass)
{
    if (pass != 1)
        return nullptr;

    messageSystem::startBlock(this, "Generating `#library_static` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty()) {
        return messageSystem::error("#library_static requires a string path to a .a file", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }
    ASTNode* nameNode = unwrapSingleExpressionNode(args[0]);
    if (nameNode->nodeType != String_Node && nameNode->nodeType != String_Constant_Node) {
        return messageSystem::error("#library_static argument must be a string literal", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }
    std::string path = decodeQuotedStringToken(nameNode->token);

    messageSystem::endBlock();

    for (auto& l : linkedStaticLibraries)
        if (l == path)
            return nullptr;
    linkedStaticLibraries.push_back(path);
    pushCompilerDirectiveCall("library_static", this);
    return nullptr;
}

// #stack_push STACK_NAME AST_NODE;
// Pushes an AST node onto a named stack.
void* ASTNode::generateCompilerStackPushDirective(int pass)
{
    messageSystem::startBlock(this, "Generating `#stack_push` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (childNodes.size() < 2)
        return messageSystem::error("#stack_push requires stack name and AST node arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.size() >= 2) {
        ASTNode* stackNameNode = unwrapSingleExpressionNode(args[0]);
        std::string stackName = stackNameNode->token->tokenStr;
        if (stackNameNode->nodeType == String_Node || stackNameNode->nodeType == String_Constant_Node)
            stackName = decodeQuotedStringToken(stackNameNode->token);
        ASTNode* astNodeToPush = args[1];
        compilerStacks[stackName].push(astNodeToPush);
    }
    else {
        return messageSystem::error("#stack_push requires stack name and AST node arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }

    messageSystem::endBlock();
    pushCompilerDirectiveCall("stack_push", this);

    return nullptr;
}

// #stack_pop STACK_NAME;
// Pops an AST node from a named stack.
void* ASTNode::generateCompilerStackPopDirective(int pass)
{
    messageSystem::startBlock(this, "Generating `#stack_pop` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (childNodes.size() < 2)
        return messageSystem::error("#stack_pop requires stack name argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty())
        return messageSystem::error("#stack_pop requires stack name argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    ASTNode* nameNode = unwrapSingleExpressionNode(args[0]);
    std::string stackName = nameNode->token->tokenStr;
    if (nameNode->nodeType == String_Node || nameNode->nodeType == String_Constant_Node)
        stackName = decodeQuotedStringToken(nameNode->token);
    if (compilerStacks.count(stackName) && !compilerStacks[stackName].empty()) {
        compilerStacks[stackName].pop();
    }

    messageSystem::endBlock();
    pushCompilerDirectiveCall("stack_pop", this);

    return nullptr;
}

// #stack_last STACK_NAME;
// Gets the last AST node from a named stack.
void* ASTNode::generateCompilerStackLastDirective(int pass)
{
    messageSystem::startBlock(this, "Generating `#stack_last` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (childNodes.size() < 2)
        return messageSystem::error("#stack_last requires stack name argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty())
        return messageSystem::error("#stack_last requires stack name argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    ASTNode* nameNode = unwrapSingleExpressionNode(args[0]);
    std::string stackName = nameNode->token->tokenStr;
    if (nameNode->nodeType == String_Node || nameNode->nodeType == String_Constant_Node)
        stackName = decodeQuotedStringToken(nameNode->token);
    if (!compilerStacks.count(stackName) || compilerStacks[stackName].empty())
        return messageSystem::error("#stack_last used with an empty stack", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    ASTNode* stackNode = compilerStacks[stackName].top();
    if (!stackNode || !stackNode->codegen)
        return messageSystem::error("#stack_last found a node without a code generator", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    LLVMValue* value = (LLVMValue*)(stackNode->*(stackNode->codegen))(pass);
    if (stackNode->asaType) {
        if (!asaType)
            asaType = new ASAType(stackNode->asaType->baseLLVMType);
        asaType->baseLLVMType = stackNode->asaType->baseLLVMType;
        asaType->strVal = stackNode->asaType->strVal;
    }

    messageSystem::endBlock();
    pushCompilerDirectiveCall("stack_last", this);
    return value;
}

ASTNode* ASTNode::resolveCompilerStackLastASTNode(int pass)
{
    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty())
        return nullptr;

    ASTNode* nameNode = unwrapSingleExpressionNode(args[0]);
    if (nameNode && nameNode->nodeType == Comma_Node && !nameNode->childNodes.empty())
        nameNode = nameNode->childNodes[0];
    if (!nameNode || !nameNode->token)
        return nullptr;

    std::string stackName = nameNode->token->tokenStr;
    if (nameNode->nodeType == String_Node || nameNode->nodeType == String_Constant_Node)
        stackName = decodeQuotedStringToken(nameNode->token);

    if (!compilerStacks.count(stackName) || compilerStacks[stackName].empty())
        return nullptr;

    return compilerStacks[stackName].top();
}

ASTNode* ASTNode::resolveCompilerContextASTNode(int pass)
{
    messageSystem::error("#context is only available inside a custom compiler directive invocation", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    return nullptr;
}

ASTNode* ASTNode::resolveCompilerParentASTNode(int pass)
{
    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty()) {
        messageSystem::error("#parent requires an AST node argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return nullptr;
    }

    ASTNode* argNode = args[0];
    if (!argNode) {
        messageSystem::error("#parent requires an AST node argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return nullptr;
    }
    argNode = unwrapSingleExpressionNode(argNode);
    if ((argNode->nodeType == Expression_Term || argNode->nodeType == Expression_Paren_Term) && argNode->childNodes.empty()) {
        messageSystem::error("#parent requires an AST node argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return nullptr;
    }

    ASTNode* resolvedNode = resolveASTNodeValue(argNode, pass);
    if (!resolvedNode) {
        if (!wasError)
            messageSystem::error("#parent argument resolved to null", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return nullptr;
    }

    if (!resolvedNode->parentNode) {
        messageSystem::error("#parent argument has no parent AST node", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return nullptr;
    }
    return resolvedNode->parentNode;
}

// #print_ast AST_NODE;
// Prints an AST node during compilation.
void* ASTNode::generateCompilerPrintASTDirective(int pass)
{
    if (pass != 2)
        return nullptr;

    messageSystem::startBlock(this, "Generating `#print_ast` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty())
        return messageSystem::error("#print_ast requires an AST node argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    ASTNode* argNode = args[0];
    if (!argNode)
        return messageSystem::error("#print_ast requires an AST node argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    ASTNode* nodeToPrint = resolveASTNodeValue(argNode, pass);
    if (!nodeToPrint)
        return messageSystem::error("#print_ast argument did not resolve to an AST node", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    printAST(nodeToPrint);

    messageSystem::endBlock();
    pushCompilerDirectiveCall("print_ast", this);
    return nullptr;
}

static void* generateCompilerStringPrintDirective(ASTNode* directiveNode, int pass, bool appendNewline)
{
    if (pass != 2)
        return nullptr;

    const std::string directiveName = appendNewline ? "#printl" : "#print";
    messageSystem::startBlock(directiveNode, "Generating `" + directiveName + "` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(directiveNode);
    if (args.empty())
        return messageSystem::error(directiveName + " requires a string argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    ASTNode* messageNode = unwrapSingleExpressionNode(args[0]);
    if (!messageNode || !messageNode->token ||
        (messageNode->nodeType != String_Node && messageNode->nodeType != String_Constant_Node))
        return messageSystem::error(directiveName + " argument must be a string literal", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    std::string message = decodeQuotedStringToken(messageNode->token);

    if (appendNewline)
        console::writeLine(message);
    else
        console::write(message);

    messageSystem::endBlock();
    pushCompilerDirectiveCall(appendNewline ? "printl" : "print", directiveNode);
    return nullptr;
}

// #print MESSAGE;
// Prints a string during compilation.
void* ASTNode::generateCompilerPrintDirective(int pass)
{
    return generateCompilerStringPrintDirective(this, pass, false);
}

// #printl MESSAGE;
// Prints a string and newline during compilation.
void* ASTNode::generateCompilerPrintLineDirective(int pass)
{
    return generateCompilerStringPrintDirective(this, pass, true);
}

// #set_attribute is handled by the pre-codegen compiler directive pass.
void* ASTNode::generateCompilerSetAttributeDirective(int pass)
{
    return nullptr;
}

// #if CONDITION AST_NODE;
// Compiles AST_NODE only when CONDITION evaluates to a compile-time true value.
void* ASTNode::generateCompilerIfDirective(int pass)
{
    messageSystem::startBlock(this, "Generating `#if` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.size() < 2)
        return messageSystem::error("#if requires condition and AST node arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    ASTNode* conditionNode = args[0];
    ASTNode* includedNode = args[1];

    bool resolved = false;
    bool conditionValue = evaluateCompilerDirectiveCondition(conditionNode, pass, resolved);
    if (wasError)
        return nullptr;
    if (!resolved)
        return messageSystem::error("#if condition must evaluate to a compile-time constant", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    if (!conditionValue) {
        messageSystem::endBlock();
        pushCompilerControlFlowResult(false, this);
        pushCompilerDirectiveCall("if", this);
        return nullptr;
    }

    includedNode = resolveASTNodeValue(includedNode, pass);
    if (!includedNode || !includedNode->codegen)
        return messageSystem::error("#if AST node argument does not have a code generator", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    void* result = (includedNode->*(includedNode->codegen))(pass);
    if (wasError)
        return nullptr;

    messageSystem::endBlock();
    pushCompilerControlFlowResult(true, this);
    pushCompilerDirectiveCall("if", this);
    return result;
}

// #error MESSAGE AST_NODE;
// Generates a custom error with the given message and AST node as context.
void* ASTNode::generateCompilerErrorDirective(int pass)
{
    messageSystem::startBlock(this, "Generating `#error` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.size() < 2) {
        return messageSystem::error("#error requires message and AST node arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }

    // Execute the error
    ASTNode* messageNode = unwrapSingleExpressionNode(args[0]);
    ASTNode* contextNode = args[1];
    if (!messageNode || !messageNode->token ||
        (messageNode->nodeType != String_Node && messageNode->nodeType != String_Constant_Node)) {
        return messageSystem::error("#error message must be a string literal", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }

    std::string message = decodeQuotedStringToken(messageNode->token);

    messageSystem::endBlock();

    // Execute the custom error:
    contextNode = resolveASTNodeValue(contextNode, pass);
    messageSystem::startBlock(contextNode ? contextNode : this, "#error directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
    return messageSystem::error(message, messageSystem::Custom_Directive_Error);
}

// #warning MESSAGE AST_NODE;
// Generates a custom warning with the given message and AST node as context.
void* ASTNode::generateCompilerWarningDirective(int pass)
{
    messageSystem::startBlock(this, "Generating `#warning` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.size() < 2) {
        return messageSystem::error("#warning requires message and AST node arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }

    ASTNode* messageNode = unwrapSingleExpressionNode(args[0]);
    ASTNode* contextNode = args[1];
    if (!messageNode || !messageNode->token ||
        (messageNode->nodeType != String_Node && messageNode->nodeType != String_Constant_Node)) {
        return messageSystem::error("#warning message must be a string literal", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }

    std::string message = decodeQuotedStringToken(messageNode->token);

    messageSystem::endBlock();

    // Execute the custom warning:
    contextNode = resolveASTNodeValue(contextNode, pass);
    messageSystem::startBlock(contextNode ? contextNode : this, "#warning directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
    messageSystem::warning(message, messageSystem::Custom_Directive_Warning);
    pushCompilerDirectiveCall("warning", this);
    return nullptr;
}

// Returns a string value (struct or *char fallback) for a compile-time string constant,
// using the same global cache and struct-building logic as String_Constant_Node.
static LLVMValue* makeStringConstant(const std::string& str)
{
    GlobalVariable* globalStr = nullptr;
    if (globalStringLiteralConstants.count(str)) {
        globalStr = globalStringLiteralConstants[str];
    }
    else {
        Constant* strConst = ConstantDataArray::getString(*llvmCompileContext, str, true);
        globalStr = new GlobalVariable(
            *llvmCompileModule,
            strConst->getType(),
            true,
            GlobalValue::PrivateLinkage,
            strConst,
            "str");
        globalStr->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
        globalStr->setAlignment(Align(1));
        globalStringLiteralConstants[str] = globalStr;
    }

    Constant* zero = ConstantInt::get(LLVMType::getInt32Ty(*llvmCompileContext), 0);
    std::vector<Constant*> indices = {zero, zero};
    Constant* strPtr = ConstantExpr::getGetElementPtr(globalStr->getValueType(), globalStr, indices);

    // Return a string struct when the string type is defined (matches String_Constant_Node behavior)
    if (!compilerDirectiveFlags["IN_STRING_MODULE"] &&
        structDefinitions.count("string") && structDefinitions["string"]->structVal) {
        StructType* strTy = cast<StructType>((LLVMType*)structDefinitions["string"]->structVal);
        Constant* lenConst = ConstantInt::get(LLVMType::getInt32Ty(*llvmCompileContext), (uint32_t)str.size());
        return ConstantStruct::get(strTy, {strPtr, lenConst});
    }
    return strPtr;
}

static ASTNode* findRightmostTokenNode(ASTNode* node)
{
    node = unwrapSingleExpressionNode(node);
    while (node && node->childNodes.size() >= 2)
        node = unwrapSingleExpressionNode(node->childNodes.back());
    while (node && !node->token && node->childNodes.size() == 1)
        node = unwrapSingleExpressionNode(node->childNodes[0]);
    return node;
}

// #nameof(expr) - returns the simple name of an AST node as a string.
void* ASTNode::generateNameofDirective(int pass)
{
    if (pass == 0)
        return nullptr;

    messageSystem::startBlock(this, "Generating `#nameof` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
    defer(messageSystem::endBlock());

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty())
        return messageSystem::error("#nameof requires an expression argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    ASTNode* namedNode = resolveASTNodeValue(args[0], pass);
    namedNode = findRightmostTokenNode(namedNode);
    if (!namedNode || !namedNode->token)
        return messageSystem::error("#nameof argument did not resolve to a named AST node", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    std::string name = namedNode->token->tokenStr;
    if (namedNode->nodeType == String_Node || namedNode->nodeType == String_Constant_Node)
        name = decodeQuotedStringToken(namedNode->token);

    LLVMValue* result = makeStringConstant(name);
    if (!asaType)
        asaType = new ASAType(result->getType());
    else
        asaType->baseLLVMType = result->getType();

    pushCompilerDirectiveCall("nameof", this);
    return result;
}

// #typeof(expr) - returns the ASA type name of expr as a string.
// For identifiers, looks up namedValues to avoid emitting any IR.
// For other expressions, generates the expression and reads the LLVM type.
void* ASTNode::generateTypeofDirective(int pass)
{
    if (pass == 0)
        return nullptr;

    messageSystem::startBlock(this, "Generating `#typeof` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty()) {
        messageSystem::error("#typeof requires a type or variable argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }

    ASTNode* argExpr = unwrapSingleExpressionNode(args[0]);
    std::string typeStr;

    // Fast path: identifier - look up in namedValues, no IR emitted
    if (argExpr->nodeType == Identifier_Node) {
        valueType* val = findNamedValue(parentNode, this, argExpr->token->tokenStr, token);
        if (val)
            typeStr = val->type;
    }

    // Fallback: generate the expression and read the LLVM type
    if (typeStr.empty()) {
        LLVMValue* val = (LLVMValue*)(argExpr->*(argExpr->codegen))(pass);
        if (!val || wasError)
            return nullptr;
        if (argExpr->asaType && argExpr->asaType->baseLLVMType)
            typeStr = getStringTypeFromLLVMType(argExpr->asaType->baseLLVMType);
        else
            typeStr = getStringTypeFromLLVMType(val->getType());
    }

    LLVMValue* result = makeStringConstant(typeStr);
    if (!asaType)
        asaType = new ASAType(result->getType());
    else
        asaType->baseLLVMType = result->getType();

    messageSystem::endBlock();
    pushCompilerDirectiveCall("typeof", this);
    return result;
}

// #sizeof(T) - returns the alloc size in bytes of T as an int64 constant.
// T can be a variable name, a plain type name, or a pointer-modified type (e.g. *int, * *Wall).
void* ASTNode::generateSizeofDirective(int pass)
{
    if (pass == 0)
        return nullptr;

    messageSystem::startBlock(this, "Generating `#sizeof` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty()) {
        messageSystem::error("#sizeof requires a type or variable argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }

    ASTNode* argNode = unwrapSingleExpressionNode(args[0]);
    LLVMType* llvmType = nullptr;

    messageSystem::startBlock(argNode, "Getting argument type", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Walk a chain of Dereference_Operation / Pointer_Node nodes to count pointer
    // indirections, then resolve the base identifier as a type.
    auto resolvePointerTypeNode = [&](ASTNode* n) -> LLVMType* {
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
        std::string typeName = std::string(stars, '*') + n->token->tokenStr;
        LLVMType* t = getLLVMTypeFromString(typeName, 0, token, wasDefined, pass);
        return (wasDefined && t) ? t : nullptr;
    };

    if (argNode->nodeType == Identifier_Node) {
        std::string name = argNode->token->tokenStr;

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
        LLVMValue* val = (LLVMValue*)(argNode->*(argNode->codegen))(pass);
        if (wasError) {
            messageSystem::endBlock();
            return nullptr;
        }
        if (val)
            llvmType = val->getType();
    }

    if (!llvmType) {
        messageSystem::error("#sizeof: cannot determine type of argument");
    }

    const DataLayout& DL = llvmCompileModule->getDataLayout();
    uint64_t size = DL.getTypeAllocSize(llvmType);
    LLVMValue* sizeVal = ConstantInt::get(LLVMType::getInt64Ty(*llvmCompileContext), size);
    if (!asaType)
        asaType = new ASAType(sizeVal->getType());
    else
        asaType->baseLLVMType = sizeVal->getType();

    messageSystem::endBlock();
    messageSystem::endBlock();
    pushCompilerDirectiveCall("sizeof", this);
    return sizeVal;
}

// #compiles(expr) - returns true if expr compiles without error, false otherwise.
// Performs speculative codegen in a temporary BasicBlock, discards the result.
void* ASTNode::generateCompilesDirective(int pass)
{
    if (pass == 0)
        return nullptr;

    messageSystem::startBlock(this, "Generating `#compiles` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty()) {
        messageSystem::error("#compiles requires an expression argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    }

    ASTNode* argNode = args[0];

    // Save current state
    BasicBlock* savedInsertBlock = llvmIRBuilder->GetInsertBlock();
    BasicBlock::iterator savedInsertPoint = llvmIRBuilder->GetInsertPoint();
    bool savedWasError = wasError;
    uint8_t savedErrorDepth = errorDepth;
    bool savedSuppressErrors = messageSystem::suppressErrors;
    messageSystem::MessageBlockNode* savedMessageNode = messageSystem::currentNode;

    // Create a temporary function and BasicBlock for speculative codegen
    FunctionType* dummyFnType = FunctionType::get(LLVMType::getVoidTy(*llvmCompileContext), false);
    Function* dummyFn = Function::Create(dummyFnType, Function::PrivateLinkage, "__compiles_probe__", llvmCompileModule.get());
    BasicBlock* tempBB = BasicBlock::Create(*llvmCompileContext, "probe", dummyFn);
    llvmIRBuilder->SetInsertPoint(tempBB);

    // Suppress errors and attempt codegen
    wasError = false;
    messageSystem::suppressErrors = true;
    if (argNode->codegen)
        (argNode->*(argNode->codegen))(pass);
    else
        wasError = true;
    while (messageSystem::currentNode && messageSystem::currentNode != savedMessageNode)
        messageSystem::endBlock();
    messageSystem::suppressErrors = savedSuppressErrors;

    bool compiled = !wasError;
    wasError = savedWasError;
    errorDepth = savedErrorDepth;

    // Remove the temporary function entirely
    dummyFn->eraseFromParent();

    // Restore insert point
    if (savedInsertBlock)
        llvmIRBuilder->SetInsertPoint(savedInsertBlock, savedInsertPoint);

    LLVMValue* result = ConstantInt::get(LLVMType::getInt1Ty(*llvmCompileContext), compiled ? 1 : 0);
    if (!asaType)
        asaType = new ASAType(result->getType());
    else
        asaType->baseLLVMType = result->getType();

    messageSystem::endBlock();
    pushCompilerDirectiveCall("compiles", this);
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
    llvmCompileModule->setTargetTriple(Triple(TargetTriple));

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

    llvmCompileModule->setDataLayout(TheTargetMachine->createDataLayout());

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

    pass.run(*llvmCompileModule);
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
    std::string optimizationOption = " -O" + optimizationLevel;

    if (isOptimizing()) {
        // For optimized builds, use opt + llc + clang to avoid misoptimizations
        // that occur when running LLVM passes on the in-memory module. The text
        // IR round-trip through opt produces correct results.
        std::string optIRPath = irFilePath + ".opt.ll";
        std::string sFilePath = irFilePath + ".s";

        // opt doesn't support -Og or -Ofast; remap them to nearest equivalents
        std::string optLevel = optimizationLevel;
        if (optLevel == "g")
            optLevel = "1";
        else if (optLevel == "fast")
            optLevel = "3";
        std::string stripDebugFlag = (compilerFlags & Flags_Debug) ? "" : " -strip-debug";
        std::string commandOpt = "opt" + stripDebugFlag + " -O" + optLevel + " " + irFilePath + " -S -o " + optIRPath;
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
    if (!isOptimizing())
        return;

    OptimizationLevel level;
    if (optimizationLevel == "1")
        level = OptimizationLevel::O1;
    else if (optimizationLevel == "2")
        level = OptimizationLevel::O2;
    else if (optimizationLevel == "3")
        level = OptimizationLevel::O3;
    else if (optimizationLevel == "s")
        level = OptimizationLevel::Os;
    else if (optimizationLevel == "z")
        level = OptimizationLevel::Oz;
    else
        level = OptimizationLevel::O2;  // g, fast: approximate with O2

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
    MPM.run(*llvmCompileModule, MAM);
}
