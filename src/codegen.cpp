#include "codegen.h"

#include "compiler_directives.h"

// OPENCODE:
// Returns true if optimization is enabled (optimizationLevel is not "0" or
// empty). Used to gate optimization passes and the opt/llc/clang pipeline.
bool isOptimizing()
{
    return optimizationLevel != "0" && !optimizationLevel.empty();
}


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

struct CallerLocationParams {
    llvm::Argument* filepath = nullptr;
    llvm::Argument* lineNum = nullptr;
    llvm::Argument* lineContent = nullptr;
};
static std::stack<CallerLocationParams> callerLocationParamStack;
std::vector<std::string> linkedStaticLibraries;

AsaVariableValue* findNamedValue(ASTNode* node, ASTNode* childNode, std::string& identifier, asaToken*& token);

static llvm::Value* generateDefaultValueForType(AsaTypeInstance* asaTypeInstance, int pass, ASTNode* node);
static bool getDeclaredTypeFromColonNode(ASTNode* colonNode, llvm::Type*& outType, std::string& outTypeName, int& outPointerLevel, bool& outIsConst, int pass);
static llvm::Value* createStringConstantLiteral(const std::string& str);


// Lookup an AsaBaseType by its name, like: `int`
// If it does not exist, creates a new one
AsaBaseType* getAsaBaseTypeFromName(std::string typeName)
{
    if (asaBaseTypes.count(typeName) > 0)
        return asaBaseTypes[typeName];
    else {
        bool wasDefined = false;
        int pass = 0;
        return CreateNewAsaType(typeName, Struct_Type, getLLVMTypeFromString(typeName, 0, nullptr, wasDefined, pass), false);
    }
}

void CreateBuiltinAsaBaseTypes()
{
    asaBaseTypes = std::unordered_map<std::string, AsaBaseType*>();

    // TODO: When the `getLLVMTypeFromString` function is cleaned up, this workaround shouldnt be necessary
    bool wasDefined = false;
    int pass = 0;


    CreateNewAsaType("void", Void_Node, getLLVMTypeFromString("void", 0, nullptr, wasDefined, pass), false);

    CreateNewAsaType("bool", Boolean_Node, getLLVMTypeFromString("bool", 0, nullptr, wasDefined, pass), false);

    CreateNewAsaType("uint8", UInt8_Type, getLLVMTypeFromString("uint8", 0, nullptr, wasDefined, pass), false);
    CreateNewAsaType("uint16", UInt16_Type, getLLVMTypeFromString("uint16", 0, nullptr, wasDefined, pass), false);
    CreateNewAsaType("uint32", UInt32_Type, getLLVMTypeFromString("uint32", 0, nullptr, wasDefined, pass), false);
    CreateNewAsaType("uint64", UInt64_Type, getLLVMTypeFromString("uint64", 0, nullptr, wasDefined, pass), false);
    CreateNewAsaType("uint128", UInt128_Type, getLLVMTypeFromString("uint128", 0, nullptr, wasDefined, pass), false);

    CreateNewAsaType("int8", SInt8_Type, getLLVMTypeFromString("int8", 0, nullptr, wasDefined, pass), true);
    CreateNewAsaType("int16", SInt16_Type, getLLVMTypeFromString("int16", 0, nullptr, wasDefined, pass), true);
    CreateNewAsaType("int32", SInt32_Type, getLLVMTypeFromString("int32", 0, nullptr, wasDefined, pass), true);
    CreateNewAsaType("int64", SInt64_Type, getLLVMTypeFromString("int64", 0, nullptr, wasDefined, pass), true);
    CreateNewAsaType("int128", SInt128_Type, getLLVMTypeFromString("int128", 0, nullptr, wasDefined, pass), true);
    setSignedTypeUnsignedVersion("int8", "uint8");
    setSignedTypeUnsignedVersion("int16", "uint16");
    setSignedTypeUnsignedVersion("int32", "uint32");
    setSignedTypeUnsignedVersion("int64", "uint64");
    setSignedTypeUnsignedVersion("int128", "uint128");

    CreateNewAsaType("half", Half_Type, getLLVMTypeFromString("half", 0, nullptr, wasDefined, pass), false);
    CreateNewAsaType("float", Float_Node, getLLVMTypeFromString("float", 0, nullptr, wasDefined, pass), false);
    CreateNewAsaType("double", Double_Type, getLLVMTypeFromString("double", 0, nullptr, wasDefined, pass), false);

    registerTypeAlias("int", "int32");
    registerTypeAlias("uint", "uint32");
    registerTypeAlias("byte", "uint8");
    registerTypeAlias("char", "int8");
}

// OPENCODE:
// Resolves a node that has returnsASTNode set: calls its resolveASTNode
// member-function pointer to get the AST node it resolves to at compile time
// (e.g. #stack_last, #parent, #context, identifiers). Returns the resolved
// node, or the original node if it doesn't resolve.
static ASTNode* resolveASTNodeValue(ASTNode* node, int pass)
{
    if (!node || !node->returnsASTNode || !node->resolveASTNode)
        return node;

    ASTNode* resolvedNode = (node->*(node->resolveASTNode))(pass);
    if (!resolvedNode && wasError)
        return nullptr;
    return resolvedNode ? resolvedNode : node;
}

// OPENCODE:
// Navigates the module nesting: Compiler_Define -> Scope_Body ->
// Module_Define_Node -> inner Scope_Body. Returns the inner scope body of a
// named module, or nullptr if the structure doesn't match.
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

// OPENCODE:
// Attempts to extract a boolean truth value from an LLVM constant. Sets
// resolved=true if value is a non-undef Constant, and returns !isNullValue().
// Used by #if condition evaluation.
static bool getConstantTruthValue(llvm::Value* value, bool& resolved)
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

// OPENCODE:
// Evaluates a compile-time #if condition by speculatively codegen'ing it in a
// throwaway __if_condition_probe__ function with suppressErrors on and
// wasError/errorDepth saved/restored. Returns the boolean result and sets
// resolved=true if the condition evaluated to a constant.
static bool evaluateCompilerDirectiveCondition(ASTNode* conditionNode, int pass, bool& resolved)
{
    resolved = false;
    if (!conditionNode || !conditionNode->codegen)
        return false;

    BasicBlock* savedInsertBlock = llvmIRBuilder->GetInsertBlock();
    BasicBlock::iterator savedInsertPoint = llvmIRBuilder->GetInsertPoint();
    bool savedWasError = wasError;
    uint8_t savedErrorDepth = errorDepth;

    FunctionType* dummyFnType = FunctionType::get(llvm::Type::getVoidTy(*llvmCompileContext), false);
    Function* dummyFn = Function::Create(dummyFnType, Function::PrivateLinkage, "__if_condition_probe__", llvmCompileModule.get());
    BasicBlock* tempBB = BasicBlock::Create(*llvmCompileContext, "probe", dummyFn);
    llvmIRBuilder->SetInsertPoint(tempBB);

    wasError = false;
    llvm::Value* conditionValue = (llvm::Value*)(conditionNode->*(conditionNode->codegen))(pass);
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

// OPENCODE:
// Returns true if node has an attribute with the given name (e.g. "inline",
// "external", "deprecated"). Checks the node's attributes list directly.
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

// OPENCODE:
// Peels single-child Expression_Term wrappers from node. Used everywhere to
// get at the real node underneath (e.g. for ?/default detection and argument
// gathering).
static ASTNode* unwrapSingleExpressionNode(ASTNode* node)
{
    while (node && node->nodeType == Expression_Term && node->childNodes.size() == 1)
        node = node->childNodes[0];
    return node;
}

// OPENCODE:
// Recursively flattens a Comma_Node tree into a flat vector of argument
// AST nodes, unwrapping Expression_Term wrappers. Used to extract directive
// arguments.
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

// OPENCODE:
// Extracts the argument list from a Compile_Time_Directive node. The args may
// be in a Scope_Body, Arguments, or a single expression; collects them into
// a flat vector via collectCompilerDirectiveArgs.
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

// OPENCODE:
// Returns true if node (after unwrapping Expression_Term) is an
// Undefined_Initializer_Node — the `?` symbol.
static bool isUndefinedInitializer(ASTNode* node)
{
    node = unwrapSingleExpressionNode(node);
    return node && node->nodeType == Undefined_Initializer_Node;
}

// OPENCODE:
// Returns true if node (after unwrapping Expression_Term) is a
// Default_Initializer_Node — the `default` keyword.
static bool isDefaultInitializer(ASTNode* node)
{
    node = unwrapSingleExpressionNode(node);
    return node && node->nodeType == Default_Initializer_Node;
}


// OPENCODE:
// Returns true if val's typeString starts with '*' (i.e. it's a pointer type
// in ASA's type system).
static bool isPointerValue(AsaVariableValue* val)
{
    return val && !val->asaTypeInstance->strVal.empty() && val->asaTypeInstance->strVal[0] == '*';
}

// OPENCODE:
// Increments variableReads on the declaration node associated with val. Used
// for unused-variable warnings.
static void markVariableRead(AsaVariableValue* val)
{
    if (val && val->declNode)
        val->declNode->variableReads++;
}

// OPENCODE:
// Increments variableWrites on the declaration node associated with val. Used
// for unused-variable warnings (a variable written but never read is flagged).
static void markVariableWrite(AsaVariableValue* val)
{
    if (val && val->declNode)
        val->declNode->variableWrites++;
}

//// OPENCODE:
//// Sets tracksVariableUsage=true on declNode so the unused-variable checker
//// will count reads/writes for this declaration.
//static void trackVariableUsage(ASTNode* declNode)  // TODO: This does not need to be a function
//{
//    if (declNode)
//        declNode->tracksVariableUsage = true;
//}

// OPENCODE:
// Returns the string value of a named attribute on node. If the attribute has
// a Scope_Body child with a string/identifier token, returns that value;
// otherwise defaults to "true". Returns "false" if the attribute is absent.
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

// OPENCODE:
// Like getAttributeValue but walks up parentNode ancestors until the
// attribute is found. Used for inherited properties like @hideast, @external.
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
inline llvm::Type* getTypePtrFromLLVMValue(llvm::Value* ptr)
{
    if (auto* A = dyn_cast<AllocaInst>(ptr))
        return A->getAllocatedType();
    if (auto* G = dyn_cast<GlobalVariable>(ptr))
        return G->getValueType();
    return ptr->getType();
}


std::unordered_map<std::string, llvm::Type*> unresolvedTypes;
std::stack<AsaTypeInstance*> lastRetrievedElementType;
std::stack<llvm::Value*> pipeOperationValue;

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

// TODO: This map is redundant with the new type system
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

    {"byte", false},
    {"bool", false},

    {"double", false},
    {"float", false},
    {"half", false},
};

// Built-in type synonym groups (same group ID = equivalent types)
// TODO: This map is redundant with the new type system
static std::unordered_map<std::string, int> typeEquivGroup = {
    {"int", 0},
    {"int32", 0},
    {"uint", 1},
    {"uint32", 1},
    {"int8", 2},
    {"byte", 2},
    {"char", 2},
    {"uint8", 3},
};
static int nextTypeEquivGroup = 4;

// User-defined type aliases (e.g., from `i32 :: int32;`)
// TODO: This map is redundant in the new type system
std::unordered_map<std::string, std::string> typeAliasMap;  // TODO: Keep in scope

// OPENCODE:
// Resolves a chain of type aliases (e.g. `i32 -> int -> int32`) to the
// ultimate target type name. Depth-limited to 100 to prevent infinite loops.
// TODO: In the new type system, aliases are resolved the same as regular types. This function becomes useless
std::string resolveTypeAlias(const std::string& name, int depth)
{
    if (depth > 100)
        return name;
    auto it = typeAliasMap.find(name);
    if (it != typeAliasMap.end())
        return resolveTypeAlias(it->second, depth + 1);
    return name;
}

// OPENCODE:
// Registers a user-defined type alias (from `Alias :: Target;`) in
// typeAliasMap. Also copies the resolved target's equivalence group and sign
// so type-compatibility checks treat the alias as equivalent to its target.
void registerTypeAlias(const std::string& aliasName, const std::string& targetName)
{
    // TODO: All of this code can be changed in the new type system to a simpler pointer copy
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
    // New alias method:
    // TODO: Add error handling
    if (asaBaseTypes.count(targetName) > 0) {
        if (asaBaseTypes.count(aliasName) == 0) {
            asaBaseTypes[aliasName] = asaBaseTypes[targetName];
        }
    }
}
// Sets the `unsignedVersion` variable in the AsaBaseType with name `s` to the AsaBaseType with name `u`
void setSignedTypeUnsignedVersion(std::string s, std::string u)
{
    if (asaBaseTypes.count(s) == 0)
        messageSystem::error("Failed to find type with name: `" + s + "`");
    if (asaBaseTypes.count(u) == 0)
        messageSystem::error("Failed to find type with name: `" + u + "`");

    asaBaseTypes[s]->unsignedVersion = asaBaseTypes[u];
}

// OPENCODE:
// Checks type equivalence: direct name match, alias-resolved match, or same
// equivalence group (e.g. int == int32, byte == int8). Used throughout
// codegen for type-compatibility checks.
// TODO: This is a basic string equivalence check between types. Remove it in the new type system
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


//// TODO: replace this with a type derived from AsaTypeInstance
//struct argType {
//    std::string typeString = "";
//    ASTNodeType baseASTType;
//    uint8_t pointerLevel = 0;
//    bool isReference = false;
//    bool isConstant = false;
//    bool mustBeExactType = false;
//    bool hasDefault = false;
//    ASTNode* defaultNode = nullptr;           // Expression_Term node (already resolved at definition site)
//    std::vector<asaToken*> defaultRawTokens;  // raw tokens for re-parsing at call site
//
//    // TODO: Update this to allow for many ABI formats
//    // Extern ABI coercion: how to split this struct arg for the x86-64 SysV ABI
//    int8_t externCoercionCount = 0;      // 0=none, N=split into N primitives (doubles or i64s)
//    bool externCoercionIsFloat = false;  // true=doubles (SSE/XMM), false=i64s (INTEGER)
//
//
//    argType(std::string ts, ASTNodeType bT, uint8_t pL = 0, bool r = false, bool ex = false, bool c = false)
//    {
//        typeString = ts;
//        baseASTType = bT;
//        pointerLevel = pL;
//        isReference = r;
//        isConstant = c;
//        mustBeExactType = ex;
//    }
//};

struct AsaFunctionDefinition {
    std::string name = "";
    std::string mangledName = "";
    std::string returnType = "";
    asaToken* token = nullptr;
    argumentList arguments = argumentList();
    argumentList userArguments = argumentList();
    bool variableNumArguments = false;
    bool isStructReturn = false;
    bool returnIsReference = false;
    // For extern functions returning small structs via register coercion (x86-64 SysV ABI):
    // instead of sret, the return type is coerced to N i64s or doubles.
    int8_t externReturnCoercionCount = 0;      // 0=none (sret or scalar), N=N coerced chunks
    bool externReturnCoercionIsFloat = false;  // true=doubles (SSE), false=i64s (INTEGER)
    // Auto-generated struct default constructors are marked replaceable so that a user-defined
    // `create` constructor with the same identity can override them without error.
    bool isReplaceable = false;
    bool isTrackedCaller = false;
    uint32_t uses = 0;
    bool isMemberFunction = false;
    Function* fnValue = nullptr;
    ASTNode* declNode = nullptr;

    AsaFunctionDefinition() {}
    AsaFunctionDefinition(std::string name, ASTNode* declNode, asaToken* token, std::string mangledName, std::string returnType, argumentList arguments, argumentList userArguments, Function* fnValue, bool variableNumArguments = false, bool isMemberFunction = false, bool isStructReturn = false, bool returnIsReference = false)
        : name(name),
          declNode(declNode),
          token(token),
          mangledName(mangledName),
          returnType(returnType),
          arguments(arguments),
          userArguments(userArguments),
          fnValue(fnValue),
          variableNumArguments(variableNumArguments),
          isMemberFunction(isMemberFunction),
          isStructReturn(isStructReturn),
          returnIsReference(returnIsReference) {};
    // OPENCODE:
    // Compares two ASTNodeType enums for equality, with a special case for
    // sign-flipped integer types when types were inferred from LLVM IR (LLVM
    // drops signedness, so int32 == uint32 after inference).
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
    // OPENCODE:
    // Compares an input function name and argument list with this function's prototype. Returns 0
    // for a perfect match, small numbers for near matches (sign differences), and 1000+ for rejects
    // (wrong name, too many args, type mismatch). Lower = better.
    //
    // TODO: Make argument comparision simple iterating over each list and using == operator or similar
    //       The type comparison should handle special cases and fallbacks automatically
    uint16_t compareMatch(std::string n, argumentList a, bool wereTypesInferred = false)
    {
        uint16_t differences = 0;
        // First check if the name and the argument lengths are the same
        if (n != name)
            return 1000;
        // If this function is variadic, it needs to check each argument individually
        // If there are more arguments than this function has and it isnt variadic:
        if (a.size() > userArguments.size() && !variableNumArguments)
            return 1000 - 1;
        // If there are less arguments than this function has, only allow if the rest have default values, like:
        //     `foo :: (x : int, y : int = 0){}`
        //     `foo(2);`
        if (a.size() < userArguments.size()) {
            // Allow if all extra params have defaults
            for (size_t i = a.size(); i < userArguments.size(); i++)
                if (!userArguments[i]->hasDefaultValue)
                    return 1000 - 1;
        }

        // Debug print
        if (verbosity >= 6) {
            console::writeLine("Comparing: " + name, console::blueFGColor);
            console::indentation++;
        }

        // Then compare all arguments piecewise. Only check non-variadic arguments
        int compareCount = variableNumArguments ? (int)userArguments.size() : (int)a.size();
        for (int i = 0; i < compareCount; i++) {
            AsaArgumentVariableValue* v1 = userArguments[i];
            AsaArgumentVariableValue* v2 = a[i];
            AsaTypeInstance* t1 = v1->asaTypeInstance;
            AsaTypeInstance* t2 = v2->asaTypeInstance;
            // TODO: These AST type comparisions may need to be moved
            ASTNodeType astType1 = t1->baseType->astNodeType;
            ASTNodeType astType2 = t2->baseType->astNodeType;
            bool mustBeExactType = t1->hasModifier(Exact_Type_Node);

            // First check if both type instances match *exactly*
            // Handles the case where one or both types are inferred and possibly missing a sign
            if (*t1 == *t2) {
                differences += 0;
                continue;
            }

            // Else, they may differ in base type, or pointer level, or other modifiers

            // If the target type is an opaque pointer:
            // *any: essentially a void-pointer that accepts any pointer type at the same level
            // `any` is not (currently) a valid type on its own; it must be a pointer
            //
            // TODO: It may be better to, in the future, make this something like `*opaque`, but I am not sure
            if (t1->baseType->typeName == "any" && t1->pointerLevel > 0 &&
                t2->pointerLevel == t1->pointerLevel) {
                differences += 10;
                continue;
            }

            // Debug printing:
            if (verbosity >= 6) {
                if (t1->baseType->typeName != t2->baseType->typeName)
                    console::writeLine("[" + std::to_string(i) + "] typeString: " + t1->baseType->typeName + "!=" + t2->baseType->typeName);
                if (t1->pointerLevel != t2->pointerLevel)
                    console::writeLine("[" + std::to_string(i) + "] pointerLevel: " + std::to_string(t1->pointerLevel) + "!=" + std::to_string(t2->pointerLevel));
            }

            // If pointer levels differ, these are likely classified as completely different types
            if (t1->pointerLevel != t2->pointerLevel) {
                // But first check if it is char* <-> string implicit conversion
                //   string -> *char  (extracts .address at call site)
                //   *char  -> string (wraps in struct at call site)
                //
                // TODO: Is this check necessary now that casts exist, and string literals are `string` by default?
                bool isStringToCharPtr =
                    t1->pointerLevel == 1 &&
                    (t1->baseType->typeName == "byte" || t1->baseType->typeName == "int8") &&
                    t2->pointerLevel == 0 && t2->baseType->typeName == "string";
                bool isCharPtrToString =
                    t1->pointerLevel == 0 && t1->baseType->typeName == "string" &&
                    t2->pointerLevel == 1 && (t2->baseType->typeName == "byte" || t2->baseType->typeName == "int8");
                // If one of the two exceptions, only add a small difference
                if (isStringToCharPtr || isCharPtrToString) {
                    differences += 10;
                    continue;
                }

                // Otherwise, they are considered completely different types:
                differences = 1000;
                goto returnDifferences;
            }

            // Check if they are the same base type (ignoring signedness for LLVM types if they were inferred)
            // TODO: Does this ever execute in the new type system?
            if (compareASTNodeTypes(t1->baseType->astNodeType, t2->baseType->astNodeType, wereTypesInferred)) {
                // For struct/unknown types both sides resolve to Struct_Type, so comparing the AST
                // nodes will always be true; require the base types to also match
                // If they dont match:
                if (t1->baseType->astNodeType == Struct_Type && !(t1->baseType == t2->baseType)) {
                    differences = 800;
                    goto returnDifferences;
                }
                // Otherwise, these are the same type:
                differences += 0;
            }
            // Otherwise, the ASTNodeTypes are different. Check if they are just different but in the same class (eg. int8 && int32 or float && double)

            // Else if the target type is an integer or bool
            else if (t1->baseType->astNodeType >= Integer_Node && t1->baseType->astNodeType <= Boolean_Node) {
                // If the argument type is supposed to be exact, then being in the same class doesnt work: return 500
                if (mustBeExactType) {
                    differences = 500;
                    goto returnDifferences;
                }
                // If the other argument is also an integer, set the difference to the difference between the enums
                if (t2->baseType->astNodeType >= Integer_Node && t2->baseType->astNodeType <= Boolean_Node)  // If similar type
                    differences += abs(t1->baseType->astNodeType - t2->baseType->astNodeType);
                // Else if the other argument is a float, then this would require implicit conversion
                else if (t2->baseType->astNodeType >= Double_Type && t2->baseType->astNodeType <= Half_Type)  // float -> int implicit
                    differences += 50;
                // Else, it is another type entirely
                else {
                    differences = 600;
                    goto returnDifferences;
                }
            }
            // Else if the target type is a floating point number
            else if (t1->baseType->astNodeType >= Double_Type && t1->baseType->astNodeType <= Half_Type) {
                // If the argument type is supposed to be exact, then being in the same class doesnt work: return 500
                if (mustBeExactType) {
                    differences = 500;
                    goto returnDifferences;
                }
                // If the other argument is also a floating point number, set the difference to the difference between the enums
                if (t2->baseType->astNodeType >= Double_Type && t2->baseType->astNodeType <= Half_Type)  // If similar type
                    differences += abs(t1->baseType->astNodeType - t2->baseType->astNodeType);
                // Else if the other argument is an integer or boolean, then this would require implicit conversion
                else if (t2->baseType->astNodeType >= Integer_Node && t2->baseType->astNodeType <= Boolean_Node)  // int -> float implicit
                    differences += 50;
                // Else, it is another type entirely
                else {
                    differences = 600;
                    goto returnDifferences;
                }
            }
            // Else, if we get here, the types don't match, and arent able to be implicitly converted
            else {
                differences = 800;
                goto returnDifferences;
            }
        }
    returnDifferences:
        if (verbosity >= 6) {
            console::writeLine("returning difference of: " + std::to_string(differences));
            console::indentation--;
        }
        return differences;
    }
    // OPENCODE:
    // Overload-resolution variant that compares against raw ASTNode argument
    // nodes (pre-type-resolution). Used during candidate filtering before
    // argument values are fully evaluated.
    //
    // TODO: I think this version of `compareMatch` should be deprecated completely with the new type system
    //uint16_t compareMatch(std::string n, std::vector<ASTNode*> a)
    //{
    //    uint16_t differences = 0;
    //    if (n != name)
    //        return 1000;
    //    if (a.size() > userArguments.size())
    //        return 1000 - 1;
    //    if (a.size() < userArguments.size()) {
    //        for (size_t i = a.size(); i < userArguments.size(); i++)
    //            if (!userArguments[i]->hasDefaultValue)
    //                return 1000 - 1;
    //    }
    //    for (int i = 0; i < (int)a.size(); i++) {
    //        ASTNodeType asaType1 = userArguments[i].baseType->astNodeType;
    //        ASTNodeType asaType2 = a[i]->nodeType;
    //        bool mustBeExactType = userArguments[i].mustBeExactType;
    //        // If asaType1 is an integer type, make sure asaType2 is also
    //        // Difference points are given the further the types are

    //        // If they are the same, return no diff
    //        if (compareASTNodeTypes(asaType1, asaType2))
    //            differences += 0;
    //        // Else if they are both integer types
    //        else if (asaType1 >= Integer_Node && asaType1 <= Boolean_Node) {
    //            if (mustBeExactType)  // If the argument type must be exact
    //                return 500;
    //            if (asaType2 >= Integer_Node && asaType2 <= Boolean_Node)  // If similar type
    //                differences += abs(asaType1 - asaType2);
    //            else
    //                differences += Boolean_Node - Integer_Node;
    //            // TODO: also give points if there exists a cast function
    //        }
    //        // Else if they are both float types
    //        else if (asaType1 >= Double_Type && asaType1 <= Half_Type) {
    //            if (mustBeExactType)  // If the argument type must be exact
    //                return 500;
    //            if (asaType2 >= Double_Type && asaType2 <= Half_Type)  // If similar type
    //                differences += abs(asaType1 - asaType2);
    //            else
    //                differences += Half_Type - Double_Type;
    //            // TODO: also give points if there exists a cast function
    //        }
    //    }
    //    return differences;
    //}
    // OPENCODE:
    // Name-only overload check: returns 1000-1 if name matches (so the caller
    // knows a function with this name exists) but doesn't attempt argument
    // matching. Used for error messages.
    // TODO: I think this version of compareMatch should not exist
    uint16_t compareMatch(std::string n)
    {
        uint16_t differences = 0;
        if (n != name)
            return 1000;
        return 1000 - 1;
    }
};

struct AsaStructDefinition : AsaBaseType {
    std::string name = "";  // TODO: This should be using inherited typeName
    uint32_t uses = 0;
    argumentList members = argumentList();
    std::unordered_map<std::string, uint16_t> memberNameIndexes;
    std::unordered_map<std::string, ASTNode*> memberDefaultNodes;  // member name -> default value AST node
    std::unordered_map<std::string, ASTNode*> memberNameTypeNodes;
    std::vector<AsaFunctionDefinition*> memberFunctions;
    llvm::StructType* structVal = nullptr;
    asaToken* token = nullptr;
    ASTNode* sourceNode = nullptr;
    //AsaTypeInstance* asaType = nullptr;
    bool isPacked = false;     // @packed: force integer packing in extern calls
    bool isNotPacked = false;  // @notpacked: force auto-detect ABI even if @packed is set
    AsaStructDefinition() {}
    AsaStructDefinition(std::string& n, asaToken*& tP, StructType*& sT, argumentList a, std::vector<AsaFunctionDefinition*>& mF, std::unordered_map<std::string, uint16_t>& mI)
    {
        name = n;
        token = tP;
        structVal = sT;
        members = a;
        memberFunctions = mF;
        memberNameIndexes = mI;
    }
    AsaStructDefinition(std::string& n, asaToken*& tP, ASTNode* sN)
    {
        name = n;
        token = tP;
        sourceNode = sN;
    }
};

std::vector<AsaFunctionDefinition*> AsaFunctionDefinitions = std::vector<AsaFunctionDefinition*>();
std::unordered_map<std::string, AsaStructDefinition*> structDefinitions = std::unordered_map<std::string, AsaStructDefinition*>();
std::stack<std::string> currentStructName = std::stack<std::string>();

// OPENCODE:
// When a string literal is used in a mutable context (e.g. passed to a ref
// or *char parameter), mallocs a copy and wraps it in a string struct so the
// original constant isn't mutated. No-op inside the string module or when the
// string struct type isn't available.
static llvm::Value* createMutableStringCopy(ASTNode* literalNode, llvm::Value* literalStringValue, int pass)
{
    if (!literalNode || literalNode->nodeType != String_Constant_Node || !literalStringValue ||
        compilerDirectiveFlags["IN_STRING_MODULE"] ||
        !structDefinitions.count("string") || !structDefinitions["string"]->structVal)
        return literalStringValue;

    std::string strValue = decodeQuotedStringToken(literalNode->token);
    uint64_t byteCount = strValue.size() + 1;

    FunctionCallee mallocFn = llvmCompileModule->getOrInsertFunction(
        "malloc",
        FunctionType::get(PointerType::getUnqual(*llvmCompileContext), {llvm::Type::getInt64Ty(*llvmCompileContext)}, false));
    llvm::Value* dst = llvmIRBuilder->CreateCall(
        mallocFn,
        {ConstantInt::get(llvm::Type::getInt64Ty(*llvmCompileContext), byteCount)},
        "str_literal_copy");

    llvm::Value* src = llvmIRBuilder->CreateExtractValue(literalStringValue, {0}, "str_literal_src");
    Function* memcpyFunc = Intrinsic::getOrInsertDeclaration(
        llvmCompileModule.get(),
        Intrinsic::memcpy,
        {PointerType::getUnqual(*llvmCompileContext), PointerType::getUnqual(*llvmCompileContext), llvm::Type::getInt64Ty(*llvmCompileContext)});
    llvmIRBuilder->CreateCall(
        memcpyFunc,
        {dst, src, ConstantInt::get(llvm::Type::getInt64Ty(*llvmCompileContext), byteCount), ConstantInt::get(llvm::Type::getInt1Ty(*llvmCompileContext), 0)});

    StructType* strTy = cast<StructType>((llvm::Type*)structDefinitions["string"]->structVal);
    llvm::Value* mutableString = UndefValue::get(strTy);
    mutableString = llvmIRBuilder->CreateInsertValue(mutableString, dst, {0});
    mutableString = llvmIRBuilder->CreateInsertValue(mutableString, ConstantInt::get(llvm::Type::getInt32Ty(*llvmCompileContext), (uint32_t)strValue.size()), {1});
    return mutableString;
}

// OPENCODE:
// Creates a LLVM debug-info type (DIType) from an LLVM type and ASA type
// string. Handles integers (signed/unsigned via typeString), floats, doubles,
// and pointers. Used by local variable and parameter debug declarations.
DIType* createDIType(llvm::Type* llvmType, const std::string& typeString)
{
    if (!llvmType || !llvmDebugBuilder)
        return nullptr;

    // Handle basic types
    if (llvmType->isIntegerTy()) {
        unsigned bitWidth = llvmType->getIntegerBitWidth();
        unsigned encoding = dwarf::DW_ATE_signed;

        // Check if unsigned
        if (typeString.find("uint") != std::string::npos ||
            typeString == "bool" || typeString == "byte") {
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

// OPENCODE:
// Reverse-looks-up a struct definition by its LLVM StructType pointer. Used
// to recover the AsaStructDefinition* from an llvm::Type* during codegen (e.g. member
// access, struct return).
AsaStructDefinition* getStructTypeFromLLVMType(llvm::Type*& t)
{
    for (const auto& [key, value] : structDefinitions) {
        if ((llvm::Type*)(value->structVal) == (llvm::Type*)t) {
            return value;
        }
    }
    return nullptr;
}

// OPENCODE:
// Checks whether one type can be casted into another. Always true between two
// numeric types.
// For user-defined types, looks for a `cast` function with a
// matching single-parameter signature. Returns false if either side is a
// pointer.
static bool hasCastBetween(const std::string& fromType, uint8_t fromPtrLevel, const std::string& toType, uint8_t toPtrLevel)
{
    // Only consider non-pointer casts
    if (fromPtrLevel != 0 || toPtrLevel != 0)
        return false;
    // Both numeric: #cast always works between any two numeric types
    if (typeSigns.count(fromType) && typeSigns.count(toType))
        return true;
    // User-defined: look for a cast function with matching signature
    // TODO: Change this lookup to use the standard function lookup path
    for (auto& functionDefinition : AsaFunctionDefinitions) {
        if (functionDefinition->name != "cast" || functionDefinition->returnType != toType)
            continue;
        if (functionDefinition->userArguments.size() == 1 &&
            functionDefinition->userArguments[0]->asaTypeInstance->baseType->typeName == fromType &&
            functionDefinition->userArguments[0]->asaTypeInstance->pointerLevel == 0)
            return true;
    }
    return false;
}

// OPENCODE:
// Formats a function call signature string like `name(<*type>, <*type>)` for
// error messages and diagnostics.
//
// TODO: This needs to use a properly generated string from the Asa type instances
std::string formatCallSignature(const std::string& name, const argumentList& args)
{
    std::string s = name + "(";
    for (size_t i = 0; i < args.size(); i++) {
        s += "<" + std::string(args[i]->asaTypeInstance->pointerLevel, '*') + args[i]->asaTypeInstance->baseType->typeName + ">";
        if (i + 1 < args.size())
            s += ", ";
    }
    s += ")";
    return s;
}

// OPENCODE:
// Prints a color-coded diff between the caller's argument list and the
// candidate function's parameter list: green=exact, yellow=castable,
// red=mismatch. Used in overload-resolution error messages.
//
// TODO: make this function also use the proper type strings generated by Asa type instances
void printFunctionDifferences(argumentList* arguments, AsaFunctionDefinition* other)
{
    console::write(other->name + " :: (");
    for (int i = 0; i < arguments->size(); i++) {
        AsaTypeInstance* a = (*arguments)[i]->asaTypeInstance;
        AsaTypeInstance* b = other->userArguments[i]->asaTypeInstance;
        // TODO: NO vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
        std::string aStr = std::string(a->pointerLevel, '*') + a->baseType->typeName;
        std::string bStr = std::string(b->pointerLevel, '*') + b->baseType->typeName;
        // If the types are equivalent, then mark it as green
        if (*a == *b)
            console::write(aStr, console::greenFGColor);
        // If the types are not equivalent, BUT a cast function between them exists, mark it as yellow
        else if (hasCastBetween(a->baseType->typeName, a->pointerLevel, b->baseType->typeName, b->pointerLevel))
            console::write(aStr + " ~= " + bStr, console::yellowFGColor);
        // Otherwise, they are different types without a know cast method. Mark it as red
        else
            console::write(aStr + " != " + bStr, console::redFGColor);

        // Add a comma after this arg unless it is the last one
        if (i < arguments->size() - 1)
            console::write(", ");
    }
    console::write(")\n");
}

// OPENCODE:
// Prints a function candidate's full signature with type annotations for
// overload-resolution error output. Strips the `StructName.` prefix from
// member functions for readability.
void printFunctionCandidate(AsaFunctionDefinition* fn)
{
    if (!fn)
        return;

    std::string displayName = fn->name;
    if (fn->isMemberFunction) {
        size_t dotPos = displayName.rfind('.');
        if (dotPos != std::string::npos)
            displayName = displayName.substr(dotPos + 1);
    }

    console::applyIndent();
    console::write(displayName, console::greenFGColor);
    console::write(" :: ");
    if (fn->returnIsReference)
        console::write("ref ", console::magentaFGColor);
    if (!fn->returnType.empty())
        console::write(fn->returnType, console::blueFGColor);
    console::write("(");
    for (int i = 0; i < (int)fn->userArguments.size(); i++) {
        const AsaArgumentVariableValue& arg = fn->userArguments[i];
        if (arg.isConstant)
            console::write("const ", console::magentaFGColor);
        if (arg.isReference)
            console::write("ref ", console::magentaFGColor);
        if (arg.pointerLevel > 0)
            console::write(std::string(arg.pointerLevel, '*'));
        console::write(arg.typeString, console::blueFGColor);
        if (i < (int)fn->userArguments.size() - 1)
            console::write(", ");
    }
    console::write(")\n");
}

// OPENCODE:
// Overload resolution: finds the best-matching AsaFunctionDefinition from fnIDs by
// scoring each candidate via compareMatch. Picks lowest score; errors if the
// best requires exact types (score 500) or no match under 500. Optionally
// throws with candidate listing if throwIfNotFound.
AsaFunctionDefinition* getFunctionFromID(std::vector<AsaFunctionDefinition*>& fnIDs, std::string& name, argumentList& arguments, bool wereTypesInferred = false, bool isMemberFunction = false, bool throwIfNotFound = false)
{
    AsaFunctionDefinition* best;
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
        return (AsaFunctionDefinition*)messageSystem::error("Function match not found, closest prototype requires exact types. Did you try casting?", messageSystem::Undefined_Function_Exact_Error);
    }
    if (bestScore < 1000)
        return best;

undefinedFunction:
    if (throwIfNotFound) {
        for (auto& f : AsaFunctionDefinitions) {
            if (f->name == name)
                messageSystem::addAttribute(f->declNode);
        }
        return (AsaFunctionDefinition*)messageSystem::error("Undefined function '" + name + "'", messageSystem::Undefined_Function_Error);
    }
    return nullptr;
}
// OPENCODE:
// Strict overload resolution: like getFunctionFromID but only accepts a
// perfect match (score 0). Used for exact-lookup scenarios like variant
// instantiation dedup and operator dispatch.
AsaFunctionDefinition* getExactFunctionFromID(std::vector<AsaFunctionDefinition*>& fnIDs, std::string& name, argumentList& arguments, bool wereTypesInferred = false, bool throwIfNotFound = false)
{
    AsaFunctionDefinition* best;
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
        return (AsaFunctionDefinition*)messageSystem::error("Function match not found, closest prototype requires exact types. Did you try casting?", messageSystem::Undefined_Function_Exact_Error);
    }
    return nullptr;
}
// OPENCODE:
// Overload resolution variant that takes raw ASTNode argument values (pre-
// type-resolution). Uses the ASTNode-based compareMatch overload for scoring.
AsaFunctionDefinition* getFunctionFromID(std::vector<AsaFunctionDefinition*>& fnIDs, std::string& name, std::vector<ASTNode*>& argValues, bool throwIfNotFound = false)
{
    AsaFunctionDefinition* best;
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
        //return (AsaFunctionDefinition*)messageSystem::error("Function match not found, closest prototype requires exact types. Did you try casting?", messageSystem::Undefined_Function_Exact_Error);
        return nullptr;  // TODO: This function doesnt have handling for exact requirement
    }
    if (bestScore < 1000)
        return best;
undefinedFunction:
    if (throwIfNotFound) {
        for (auto& f : AsaFunctionDefinitions) {
            if (f->name == name)
                messageSystem::addAttribute(f->declNode);
        }
        return (AsaFunctionDefinition*)messageSystem::error("Undefined function '" + name + "'", messageSystem::Undefined_Function_Error);
    }
    return nullptr;
}
// OPENCODE:
// Name-only overload resolution: finds any AsaFunctionDefinition with a matching name.
// Used for error reporting and existence checks.
AsaFunctionDefinition* getFunctionFromID(std::vector<AsaFunctionDefinition*>& fnIDs, std::string& name, bool throwIfNotFound = false)
{
    AsaFunctionDefinition* best;
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
        //return (AsaFunctionDefinition*)messageSystem::error("Function match not found, closest prototype requires exact types. Did you try casting?", messageSystem::Undefined_Function_Exact_Error);
        return nullptr;  // TODO: Does this case ever occur?
    }
    if (bestScore < 1000)
        return best;
undefinedFunction:
    if (throwIfNotFound) {
        for (auto& f : AsaFunctionDefinitions) {
            if (f->name == name)
                messageSystem::addAttribute(f->declNode);
        }
        return (AsaFunctionDefinition*)messageSystem::error("Undefined function '" + name + "'", messageSystem::Undefined_Function_Error);
    }
    return nullptr;
}
// OPENCODE:
// Reverse-looks up a AsaFunctionDefinition from its LLVM Function* pointer. Used when
// codegen has only the Function* (e.g. from a call instruction) and needs
// metadata like return type or argument list.
AsaFunctionDefinition* getFunctionIDFromFunctionPointer(std::vector<AsaFunctionDefinition*>& fnIDs, Function*& fnPtr)
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

// OPENCODE:
// Simple error helper: prints a string to stderr and returns nullptr as
// llvm::Value*. Used as a shorthand in codegen paths that can't recover.
llvm::Value* LogErrorV(const char* Str)
{
    console::printError(Str);
    return nullptr;
}

// OPENCODE:
// Create an alloca instruction in the entry block of the function. This is
// used for mutable variables etc. (AllocaInst is placed at entry for mem2reg
// optimization to promote it to registers.)
static AllocaInst* CreateEntryBlockAlloca(Function* TheFunction, llvm::Type* t, StringRef VarName)
{
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(), TheFunction->getEntryBlock().begin());
    return TmpB.CreateAlloca(t, nullptr, VarName);
}

// TODO: This should likely take as input just an `asaTypeInstance`
// OPENCODE:
// Generates a zero-initialized or default-valued LLVM value for a given type.
// For structs: allocas, memsets to 0, then fills in any member default values
// from structDef->memberDefaultNodes. For primitives: returns 0/0.0/null.
// Recursively handles default initializers and undefined initializers.
static llvm::Value* generateDefaultValueForType(AsaTypeInstance* asaTypeInstance, int pass, ASTNode* node)
{
    if (!asaTypeInstance)
        return nullptr;

    //std::string resolvedTypeName = resolveTypeAlias(typeName);

    // If this is a struct and not a pointer:
    if (asaTypeInstance->pointerLevel == 0 &&
        asaTypeInstance->baseType->isStruct &&
        structDefinitions.find(asaTypeInstance->baseType->typeName) != structDefinitions.end()) {

        AsaStructDefinition* structDef = structDefinitions[asaTypeInstance->baseType->typeName];
        // TODO: maybe dont force codegen for missing definitions like this:
        if (structDef->structVal == nullptr && structDef->sourceNode)
            (structDef->sourceNode->*(structDef->sourceNode->codegen))(pass);
        if (wasError)
            return nullptr;

        Function* fn = llvmIRBuilder->GetInsertBlock()->getParent();
        AllocaInst* defaultPtr = CreateEntryBlockAlloca(fn, asaTypeInstance->llvmType, "default_" + asaTypeInstance->baseType->typeName);

        llvm::Value* structSize = ConstantInt::get(llvm::Type::getInt64Ty(*llvmCompileContext), llvmCompileModule->getDataLayout().getTypeAllocSize(asaTypeInstance->llvmType));
        Function* memsetFunc = Intrinsic::getOrInsertDeclaration(llvmCompileModule.get(), Intrinsic::memset, {defaultPtr->getType(), llvm::Type::getInt64Ty(*llvmCompileContext)});
        llvmIRBuilder->CreateCall(memsetFunc, {defaultPtr,
                                                  ConstantInt::get(llvm::Type::getInt8Ty(*llvmCompileContext), 0),
                                                  structSize,
                                                  ConstantInt::get(llvm::Type::getInt1Ty(*llvmCompileContext), 0)});

        StructType* structType = cast<StructType>(asaTypeInstance->llvmType);
        for (auto& [memberName, defaultNode] : structDef->memberDefaultNodes) {
            auto idxIt = structDef->memberNameIndexes.find(memberName);
            if (idxIt == structDef->memberNameIndexes.end())
                continue;

            uint16_t idx = idxIt->second;
            llvm::Type* memberType = structType->getElementType(idx);
            llvm::Value* defaultVal = nullptr;
            ASTNode* unwrappedDefault = unwrapSingleExpressionNode(defaultNode);
            if (unwrappedDefault && unwrappedDefault->nodeType == Undefined_Initializer_Node)
                defaultVal = UndefValue::get(memberType);
            else if (unwrappedDefault && unwrappedDefault->nodeType == Default_Initializer_Node)
                defaultVal = generateDefaultValueForType(memberType, structDef->members[idx].typeString, structDef->members[idx].pointerLevel, pass, node);
            else
                defaultVal = (llvm::Value*)(defaultNode->*(defaultNode->codegen))(pass);

            if (wasError)
                return nullptr;
            if (!defaultVal)
                continue;

            bool isSigned = typeSigns.count(structDef->members[idx].typeString) ? typeSigns[structDef->members[idx].typeString] : false;
            defaultVal = castValue(defaultVal, memberType, true, isSigned, node);
            if (wasError)
                return nullptr;

            llvm::Value* memberPtr = llvmIRBuilder->CreateStructGEP(structType, defaultPtr, idx, memberName + "_default");
            llvmIRBuilder->CreateStore(defaultVal, memberPtr);
        }

        return llvmIRBuilder->CreateLoad(asaTypeInstance->llvmType, defaultPtr, "default_load");
    }

    return Constant::getNullValue(asaTypeInstance->llvmType);
}

static void instantiateVariantStruct(const std::string&, const std::string&, const std::string&, std::vector<std::pair<std::string, std::string>> = {});

// OPENCODE:
// Validates that a variant struct/function call has the correct number of
// type arguments. Emits a contextual error block if the count doesn't match
// the variant definition's Variants_Node child count.
static bool validateVariantArgumentCount(ASTNode* useNode, ASTNode* defVariantsNode, int actualCount, const std::string& name)
{
    int expectedCount = defVariantsNode ? (int)defVariantsNode->childNodes.size() : 0;
    if (actualCount == expectedCount)
        return true;

    messageSystem::startBlock(useNode, "Checking variant arguments", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
    messageSystem::error("Variant '" + name + "' expects " + std::to_string(expectedCount) +
                         " argument" + (expectedCount == 1 ? "" : "s") +
                         ", got " + std::to_string(actualCount));
    messageSystem::endBlock();
    return false;
}

// OPENCODE:
// Maps an ASA type name string to its LLVM type. Handles all primitive types
// (int/uint/float/double/bool/byte/half/any), struct types (lazy-generates
// the struct body if not yet emitted), void, and pointer levels via
// pointerLevelOffset. Sets wasDefined if the type was resolved to a struct.
//
// TODO: Make this only for conversion from AsaBaseType to llvm::Type, rather than direct string to type conversion
llvm::Type* getLLVMTypeFromString(std::string typeName, int pointerLevelOffset, ASTNode* contextNode, bool& wasDefined, int& pass)
{
    llvm::Type* aType;
    uint16_t pointerLevel = 0;
    while (typeName[0] == '*') {
        typeName = typeName.substr(1);
        pointerLevel++;
    }
    typeName = resolveTypeAlias(typeName);
    // Integer types
    if (typeName == "int" || typeName == "int32" || typeName == "uint" || typeName == "uint32")
        aType = llvm::Type::getInt32Ty(*llvmCompileContext);
    else if (typeName == "int16" || typeName == "uint16")
        aType = llvm::Type::getInt16Ty(*llvmCompileContext);
    else if (typeName == "int8" || typeName == "uint8" || typeName == "byte")
        aType = llvm::Type::getInt8Ty(*llvmCompileContext);
    else if (typeName == "int64" || typeName == "uint64")
        aType = llvm::Type::getInt64Ty(*llvmCompileContext);
    else if (typeName == "int128" || typeName == "uint128")
        aType = llvm::Type::getInt128Ty(*llvmCompileContext);

    // Floats
    else if (typeName == "float")
        aType = llvm::Type::getFloatTy(*llvmCompileContext);
    else if (typeName == "half")
        aType = llvm::Type::getHalfTy(*llvmCompileContext);
    else if (typeName == "double")
        aType = llvm::Type::getDoubleTy(*llvmCompileContext);

    // Bool
    else if (typeName == "bool")
        aType = llvm::Type::getInt1Ty(*llvmCompileContext);

    // Any (void pointer base type)
    else if (typeName == "any")
        aType = llvm::Type::getInt8Ty(*llvmCompileContext);

    // Otherwise, look in struct definitions
    else if (structDefinitions.find(typeName) != structDefinitions.end()) {
        // If the struct body hasn't been generated yet, generate it
        if (structDefinitions[typeName]->structVal == nullptr) {
            if (currentStructName.size() == 0 || currentStructName.top() != typeName) {
                aType = (llvm::Type*)(structDefinitions[typeName]->sourceNode->*(structDefinitions[typeName]->sourceNode->codegen))(pass);
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
                messageSystem::startBlock(contextNode, "Resolving type", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                messageSystem::error("Cannot nest struct in self");
                messageSystem::endBlock();
                errorDepth++;
                return nullptr;
            }
        }
        else
            aType = (llvm::Type*)(structDefinitions[typeName]->structVal);
    }

    // If still not found, check for variant struct instantiation (e.g. "array.int")
    else {
        size_t dotPos = typeName.find('.');
        if (dotPos != std::string::npos) {
            std::string baseName = typeName.substr(0, dotPos);
            std::string typeArg = typeName.substr(dotPos + 1);
            auto tmplIt = variantStructTemplates.find(baseName);
            if (tmplIt != variantStructTemplates.end()) {
                ASTNode* defVariantsNode = tmplIt->second->childNodes.size() > 1 ? tmplIt->second->childNodes[1] : nullptr;
                int actualCount = 0;
                if (!typeArg.empty()) {
                    actualCount = 1;
                    for (char c : typeArg)
                        if (c == '.')
                            actualCount++;
                }
                if (!validateVariantArgumentCount(contextNode, defVariantsNode, actualCount, baseName)) {
                    wasDefined = false;
                    return nullptr;
                }
                if (instantiatedVariantStructs.find(typeName) == instantiatedVariantStructs.end()) {
                    instantiatedVariantStructs.insert(typeName);
                    instantiateVariantStruct(baseName, typeArg, typeName);
                }
                auto sIt = structDefinitions.find(typeName);
                if (sIt != structDefinitions.end() && sIt->second && sIt->second->structVal) {
                    aType = (llvm::Type*)sIt->second->structVal;
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


// OPENCODE:
// Reverse-maps an LLVM type back to an ASA type string (e.g. i32 -> "int32",
// struct.X -> "X"). Pointer levels are prefixed as asterisks. Used when type
// info is only available from LLVM IR (e.g. inferred operand types).
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
        baseTypeName = "byte";  // byte/uint8 are unsigned in ASA; use unsigned default for i8
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
            llvm::StructType* AsaStructDefinitionDefinitionDefinition = static_cast<llvm::StructType*>(baseType);
            if (AsaStructDefinitionDefinitionDefinition->hasName()) {
                std::string fullName = AsaStructDefinitionDefinitionDefinition->getName().str();
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

// OPENCODE:
// Maps an ASA type name string to its ASTNodeType enum (e.g. "int32" ->
// SInt32_Type). Returns Identifier_Node for unknown/user-defined types.
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
    if (typeName == "byte")
        return Byte_Type;
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

// OPENCODE:
// Promotes both operands of a binary op to the same (highest-precision) type.
// Compares ASTNodeType ranks, casts the lower-precision side to the higher,
// handling signed/unsigned/float promotions. Prefers asaType tracking over
// LLVM type introspection for sign information.
void castToHighestAccuracy(llvm::Value*& L, llvm::Value*& R, ASTNode* node)
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

// OPENCODE:
// One-time setup: creates the LLVMContext, Module, IRBuilder, and DIBuilder
// with a compile unit for debug info. Seeds compilerDirectiveFlags from
// command-line flags. Called once from main.cpp before codegen passes.
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

// OPENCODE:
// Tears down all codegen state: deletes AsaFunctionDefinitions and structDefinitions,
// resets LLVM unique_ptrs (builder, module, context), clears all global
// maps/stacks. Called from main.cpp after compilation completes or on error.
void resetCodeGenerator()
{
    // Free heap-allocated codegen objects
    for (auto* fid : AsaFunctionDefinitions)
        delete fid;
    AsaFunctionDefinitions.clear();

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
    lastRetrievedElementType = std::stack<AsaTypeInstance*>();
    pipeOperationValue = std::stack<llvm::Value*>();
    loopContextStack = std::stack<LoopContext>();
    resultContextStack = std::stack<ResultContext>();
    currentStructName = std::stack<std::string>();

    // Reset dynamic equivalence groups (typeAliasMap is preserved - aliases are
    // registered during parsing, before initializeCodeGenerator is called).
    typeEquivGroup = {
        {"int", 0},
        {"int32", 0},
        {"uint", 1},
        {"uint32", 1},
        {"int8", 2},
        {"byte", 2},
        {"uint8", 3},
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
        {"byte", false},
        {"bool", false},
        {"double", false},
        {"float", false},
        {"half", false},
    };
}

// OPENCODE:
// Returns or creates a DIFile debug-info node for a source file path, using
// a cache to avoid duplicates. Splits the path into directory + filename.
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

// OPENCODE:
// The core variable/symbol lookup: searches for an identifier in the current
// scope's namedValues, then walks up parent scopes respecting shadowing,
// module-scope privacy, and function-local visibility. At global scope only
// top-level declarations are visible; nested scopes see expression-statement
// declarations. Also suggests `this.member` if inside a struct member function.
AsaVariableValue* findNamedValue(ASTNode* node, ASTNode* childNode, std::string& identifier, asaToken*& token)
{
    // First look in self
    if (node->namedValues.find(identifier) != node->namedValues.end()) {
        return node->namedValues[identifier];
    }

    // Search through child nodes, but with proper scoping rules. Keep the
    // latest matching declaration before the use so shadowing works correctly.
    AsaVariableValue* foundValue = nullptr;
    for (auto& c : node->childNodes) {
        // At nested scopes (depth > 0), only search up to the point where it's used
        if (node->depth > 0 && c == childNode) {
            break;
        }

        // Module-scope nodes hold their variables privately unless the module
        // came from #import, in which case its children are visible unqualified.
        if (c->isModuleScope && !c->importedChildren)
            continue;
        if (c->isModuleScope && c->importedChildren) {
            auto moduleValue = c->namedValues.find(identifier);
            if (moduleValue != c->namedValues.end())
                foundValue = moduleValue->second;
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
                foundValue = c->namedValues[identifier];
            }
        }
        else {
            // At nested scopes, only declaration statement nodes contribute
            // variables to this lexical scope. Control-flow/scope-body nodes keep
            // their own named values private.
            if ((c->nodeType == Expression_Statement || c->nodeType == Colon_Separator_Node) &&
                c->namedValues.find(identifier) != c->namedValues.end()) {
                foundValue = c->namedValues[identifier];
            }
        }
    }
    if (foundValue)
        return foundValue;

    // Search recursively upward, but stop at global scope
    if (node->depth > 0)
        return findNamedValue(node->parentNode, node, identifier, token);

    // Variable not found - check if we're in a member function
    if (!currentStructName.empty()) {
        std::string structName = currentStructName.top();
        if (structDefinitions.find(structName) != structDefinitions.end()) {
            AsaStructDefinition* structDef = structDefinitions[structName];

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

// OPENCODE:
// Searches scope and ancestor scopes for a named compiler definition
// (struct, function, enum, module, cast). Also checks imported module children
// (importedChildren flag) by navigating into their inner scope. Returns the
// ASTNode* or nullptr.
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

// OPENCODE:
// Returns true if node is a compile-time definition (Compiler_Define,
// Compiler_Define_Function, Compiler_Define_Cast, Compiler_Define_Struct, or
// Compiler_Define_Enum).
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

// OPENCODE:
// Like findCompilerDefinition but also scans child nodes of each scope for
// matching token names that are compile-time definitions. Used as a fallback
// when compilerDefinitions map doesn't contain the entry.
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

// OPENCODE:
// resolveASTNode callback: resolves an identifier AST node to its compile-time
// definition (struct, function, enum, etc.) by searching parent scopes.
ASTNode* ASTNode::resolveCompilerDefinitionASTNode(int pass)
{
    if (!token)
        return nullptr;

    return findCompileTimeDefinitionNode(parentNode, token->tokenStr);
}

// OPENCODE:
// Infer the ASA type string for a compile-time define body node (an
// Expression_Term). Used when a compiler define is accessed via module member
// access (e.g. Mod.ABC).
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

// OPENCODE:
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

// OPENCODE:
// Recursively resolves the type string of a member-access expression
// (e.g. `a.b.c` or `arr[i]`). Handles identifiers (variable lookup via
// findNamedValue), Access_Operation (strips one pointer level for array
// indexing), Member_Access (resolves left type, then looks up right member
// in struct definitions or module registry), and dereference operators.
std::string getMemberAccessTypeString(ASTNode* node, ASTNode* parentNode, asaToken*& token)
{
    // Base case: if it's just an identifier, look it up normally
    if (node->nodeType == Identifier_Node) {
        // Check if it's a module name (not a variable)
        auto modIt = moduleRegistry.find(node->token->tokenStr);
        if (modIt != moduleRegistry.end())
            return "__module__:" + node->token->tokenStr;

        AsaVariableValue* val = findNamedValue(parentNode, nullptr, node->token->tokenStr, token);
        if (!val && !wasError) {
            console::indentation = errorDepth;
            if (errorDepth < maxErrorTraceDepth)
                printTokenError(tokenRange {token, token}, "Unknown variable name: " + node->token->tokenStr);
            wasError = true;
            errorDepth++;
            return "";
        }
        return val ? val->asaTypeInstance->strVal : "";
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
                    return varIt->second->asaTypeInstance->strVal;
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

        AsaStructDefinition* structDef = structDefinitions[structName];

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


// OPENCODE:
// Emits a "variable already declared in this scope" error with a contextual
// block pointing to the previous declaration. Returns true (always reports).
//   Module vars: ownerNode == the Compiler_Define module node.
static bool reportSameScopeRedeclaration(ASTNode* declarationNode, ASTNode* previousDeclarationNode, const std::string& varName)
{
    messageSystem::startBlock(declarationNode, "Checking variable declaration", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
    if (previousDeclarationNode)
        messageSystem::addAttribute(previousDeclarationNode, "previously declared here");
    messageSystem::error("Variable '" + varName + "' is already declared in this scope.", messageSystem::Variable_Declaration_Error);
    messageSystem::endBlock();
    return true;
}

// OPENCODE:
// Checks if varName is already declared in ownerNode's namedValues or
// compilerDefinitions. If so, calls reportSameScopeRedeclaration and returns
// true. Used to prevent duplicate variable declarations in the same scope.
static bool checkSameScopeRedeclaration(ASTNode* ownerNode, ASTNode* declarationNode, const std::string& varName)
{
    if (!ownerNode)
        return false;

    auto existing = ownerNode->namedValues.find(varName);
    if (existing != ownerNode->namedValues.end() && existing->second) {
        ASTNode* previousDeclarationNode = existing->second->declNode;
        if (previousDeclarationNode != declarationNode)
            return reportSameScopeRedeclaration(declarationNode, previousDeclarationNode, varName);
    }

    for (ASTNode* child : ownerNode->childNodes) {
        if (child == declarationNode)
            break;
        if (!child || (child->nodeType != Expression_Statement && child->nodeType != Colon_Separator_Node))
            continue;

        auto childValue = child->namedValues.find(varName);
        if (childValue != child->namedValues.end() && childValue->second)
            return reportSameScopeRedeclaration(declarationNode, childValue->second->declNode, varName);
    }

    return false;
}

// OPENCODE:
// Declares a module-scope or global variable as an LLVM GlobalVariable.
// Handles both typed declarations (`x : int = 5`) and inferred declarations
// (`x = 5;` — probes the RHS in a throwaway function to discover its type).
// Registers the AsaVariableValue in ownerNode->namedValues, checks for redeclaration,
// and queues non-trivial initializers into globalInitList for deferred
// initialization in __asa_global_init.
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
    llvm::Type* llvmType = nullptr;

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
        llvmType = getLLVMTypeFromString(typeName, 0, typeNode, wasDefined, pass);
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
            if (voidCheck->nodeType == Undefined_Initializer_Node || voidCheck->nodeType == Default_Initializer_Node) {
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
        FunctionType* ft = FunctionType::get(llvm::Type::getVoidTy(*llvmCompileContext), false);
        Function* probeF = Function::Create(ft, Function::PrivateLinkage, "__type_probe__", llvmCompileModule.get());
        BasicBlock* probeBB = BasicBlock::Create(*llvmCompileContext, "probe", probeF);
        llvmIRBuilder->SetInsertPoint(probeBB);

        wasError = false;
        messageSystem::suppressErrors = true;
        llvm::Value* probeVal = (llvm::Value*)(exprNode->*(exprNode->codegen))(1);
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

    if (checkSameScopeRedeclaration(ownerNode, exprStmtNode, varName))
        return;

    std::string globalName = varName;
    Constant* initialValue = rhsIsUndefined ? UndefValue::get(llvmType) : Constant::getNullValue(llvmType);
    GlobalVariable* gv = new GlobalVariable(
        *llvmCompileModule, llvmType, isConst,
        GlobalValue::InternalLinkage,
        initialValue,
        globalName);

    std::string actualType = std::string(pointerLevel, '*') + typeName;
    AsaVariableValue* vt = new AsaVariableValue(varName, actualType, gv);
    vt->isConstant = isConst;
    vt->isUndefined = rhsIsUndefined;
    vt->declNode = exprStmtNode;            // the expression statement node that owns the attributes
    vt->initialValueNode = rhsInitializer;  // Store the initial value node for global variables to support 'initial' keyword

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
        FunctionType* ft = FunctionType::get(llvm::Type::getVoidTy(*llvmCompileContext), false);
        globalInitFn = Function::Create(
            ft, Function::InternalLinkage, "__asa_global_init", *llvmCompileModule);
    }
}

// OPENCODE:
// Register a Compiler_Define (module) node and declare all its variable
// globals. Recursively registers nested sub-modules with compound
// dot-separated names (e.g. "Parent.Child").
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

// OPENCODE:
// Recursively sets enclosingModule on node and all descendants so member-
// access codegen knows which module a declaration belongs to.
static void setModuleMemberContext(ASTNode* node, const std::string& moduleName)
{
    if (!node)
        return;
    if (node->enclosingModule.empty())
        node->enclosingModule = moduleName;
    for (auto* child : node->childNodes)
        setModuleMemberContext(child, moduleName);
}

// OPENCODE:
// Processes a module Compiler_Define node: navigates to its inner scope,
// declares all module-scope variables as LLVM globals via
// declareModuleScopeVariable, and recurses into nested sub-modules. Sets
// isModuleScope, registers in moduleRegistry, and sets enclosingModule on
// all children. Called during pass 0/1 for each imported or inline module.
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

    if (!moduleCompilerDefineNode->importedChildren) {
        for (ASTNode* child : innerScope->childNodes) {
            if (child && !(child->nodeType == Compiler_Define && child->isModuleScope))
                setModuleMemberContext(child, fullName);
        }
    }

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

        llvm::Type* gvType = gv->getValueType();
        ASTNode* exprTerm = node->nodeType == Colon_Separator_Node ? nullptr : (node->childNodes.size() >= 2 ? node->childNodes[1] : nullptr);
        ASTNode* initializerNode = unwrapSingleExpressionNode(exprTerm);
        if (initializerNode && initializerNode->nodeType == Undefined_Initializer_Node)
            continue;

        llvm::Value* initVal = nullptr;
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
            initVal = (llvm::Value*)(exprTerm->*(exprTerm->codegen))(2);
        }
        if (wasError || !initVal) {
            return false;
        }

        if (!gv->isConstant() && initializerNode &&
            initializerNode->nodeType == String_Constant_Node &&
            structDefinitions.count("string") && structDefinitions["string"]->structVal &&
            gvType == structDefinitions["string"]->structVal) {
            initVal = createMutableStringCopy(initializerNode, initVal, 2);
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
                        FunctionType::get(llvm::Type::getInt64Ty(*llvmCompileContext),
                            {PointerType::getUnqual(*llvmCompileContext)}, false));
                    llvm::Value* lenVal = llvmIRBuilder->CreateCall(strlenFn, {initVal}, "strlen");
                    llvm::Value* lenTrunc = llvmIRBuilder->CreateTrunc(lenVal, llvm::Type::getInt32Ty(*llvmCompileContext), "len");
                    llvm::Value* strStruct = UndefValue::get(gvType);
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

// OPENCODE:
// Recursively walks the AST and emits an "unused variable" warning for every
// declaration node where tracksVariableUsage is set and variableReads == 0.
static void warnAboutUnusedVariablesRecursive(ASTNode* node)
{
    if (!node)
        return;

    if (node->tracksVariableUsage && node->variableReads == 0) {
        std::string name = node->token ? node->token->tokenStr : "variable";
        ASTNode* declarationNode = node;
        if (node->nodeType == Expression_Statement && !node->childNodes.empty())
            declarationNode = node->childNodes[0];
        if (declarationNode->nodeType == Colon_Separator_Node &&
            !declarationNode->childNodes.empty() &&
            declarationNode->childNodes[0]->token)
            name = declarationNode->childNodes[0]->token->tokenStr;

        messageSystem::startBlock(node, "Checking variable usage", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
        messageSystem::warning("Variable '" + name + "' was declared but never used.", messageSystem::Unused_Variable_Warning);
        messageSystem::endBlock();
    }

    for (auto* child : node->childNodes)
        warnAboutUnusedVariablesRecursive(child);
}

// OPENCODE:
// Entry point for unused-variable warnings. Only runs if the Unused or All
// warning flag is set. Delegates to the recursive walker.
void warnAboutUnusedVariables(ASTNode* rootNode)
{
    if (!(warningFlags == W_Unused || warningFlags == W_All))
        return;

    warnAboutUnusedVariablesRecursive(rootNode);
}


// OPENCODE:
// The central type-cast dispatcher: converts value to destType using the
// appropriate LLVM cast instruction (IntCast, SIToFP/UIToFP, FPToSI/FPToUI,
// FPCast, pointer casts, struct handling). Honors signedness and skips
// no-op casts. Errors on incompatible types.
llvm::Value* castValue(llvm::Value* value, llvm::Type* destType, bool isSrcSigned, bool isToSigned, ASTNode* node, bool destTypeIsStruct)
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

// llvm::Value*
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
    else if (nodeType == Undefined_Initializer_Node) {
        messageSystem::endBlock();
        return ConstantPointerNull::get(PointerType::getUnqual(*llvmCompileContext));
    }
    else if (nodeType == Initial_Initializer_Node) {
        messageSystem::endBlock();
        return messageSystem::error("'initial' can only be used in assignments to reset a variable");
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
        Constant* zero = ConstantInt::get(llvm::Type::getInt32Ty(*llvmCompileContext), 0);
        std::vector<Constant*> indices = {zero, zero};
        Constant* strPtr = ConstantExpr::getGetElementPtr(
            globalStr->getValueType(),
            globalStr,
            indices);

        // Return a string struct { ptr, length } unless we're inside the string
        // module itself (where raw *char is needed for bootstrapping).
        if (!compilerDirectiveFlags["IN_STRING_MODULE"] &&
            structDefinitions.count("string") && structDefinitions["string"]->structVal) {
            StructType* strTy = cast<StructType>((llvm::Type*)structDefinitions["string"]->structVal);
            Constant* lenConst = ConstantInt::get(llvm::Type::getInt32Ty(*llvmCompileContext), (uint32_t)strValue.size());
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

// llvm::Value*
void* ASTNode::generateVariableExpression(int pass)
{
    messageSystem::startBlock(this, "Generating variable expression", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Look this variable up in the function
    AsaVariableValue* existingValue = findNamedValue(parentNode, this, token->tokenStr, token);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }
    if (!existingValue) {
        llvm::Value* exprVal = ConstantInt::get(llvm::Type::getInt32Ty(*llvmCompileContext), 0);
        llvm::Type* llvmType = nullptr;
        Function* theFunction = llvmIRBuilder->GetInsertBlock()->getParent();
        uint16_t pointerLevel = 0;
        std::string typeName = "";

        // If variable does not have type, it may be a used undefined variable
        if (childNodes.size() == 0) {
            // Or, it may be a compiler definition, like `FOO :: 5;`
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
        // TODO: Is this code path ever used?
        // If variable does have type, it is a declaration
        else {
            ASTNode* typeNode = childNodes[0];
            typeName = resolveTypeAlias(typeNode->token->tokenStr);  // TODO:

            messageSystem::startBlock(typeNode, "Generating type", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

            // This never runs, even throughout the entire test suite:
            console::writeLine("code path used!");
            exit(1);

            AsaTypeInstance* asaTypeInstance = CreateAsaTypeInstanceFromASTNode(typeNode);
        getNextPointerLevel:
            if (typeNode->token->tokenStr == "*" ||
                typeNode->token->tokenStr == "const" ||
                typeNode->token->tokenStr == "exact") {
                pointerLevel++;
                typeNode = typeNode->childNodes[0];
                goto getNextPointerLevel;
            }
            bool wasDefined = true;
            llvmType = getLLVMTypeFromString(typeName, 0, typeNode, wasDefined, pass);
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
            asaType = new AsaTypeInstance(llvmType);
        else
            asaType->baseLLVMType = llvmType;
        // TODO: Does any of the following ever run either?
        AllocaInst* targetPtr = CreateEntryBlockAlloca(theFunction, llvmType, token->tokenStr);
        //std::string actualType = (pointerLevel > 0 ? std::string(pointerLevel, '*') : "") + typeName;
        namedValues[token->tokenStr] = new AsaVariableValue(token->tokenStr, asaType, targetPtr);
        namedValues[token->tokenStr]->declNode = this;
        namedValues[token->tokenStr]->isUndefined = false;
        this->tracksVariableUsage = true;
        markVariableWrite(namedValues[token->tokenStr]);

        llvmIRBuilder->CreateStore(exprVal, targetPtr);

        messageSystem::endBlock();

        if (isRef || lvalue)
            return targetPtr;
        else
            return llvmIRBuilder->CreateLoad(targetPtr->getAllocatedType(), targetPtr, token->tokenStr + "_load");
    }
    // Check @deprecated / @removed on the variable declaration
    if (existingValue->declNode)
        if (!checkDeprecationAttributes(this, existingValue->declNode, token->tokenStr)) {
            messageSystem::endBlock();
            return nullptr;
        }

    llvm::Value* A = existingValue->llvmValue;
    llvm::Type* valType = getTypePtrFromLLVMValue(A);
    if (!asaType)
        asaType = new AsaTypeInstance(valType);
    else
        asaType->baseLLVMType = valType;
    // Store the type string so pointer element types can be resolved later for array subscripts
    asaType->strVal = existingValue->asaTypeInstance->strVal;
    asaType->isRef = existingValue->isReference;

    if (!isRef && !lvalue && existingValue->isUndefined) {
        messageSystem::addAttribute(existingValue->declNode, "declared here");
        messageSystem::error("Variable '" + token->tokenStr + "' was declared undefined, and used before being defined.", messageSystem::Declared_Undefined_Variable_Error);
    }
    if (!isRef && (!lvalue || isPointerValue(existingValue)))
        markVariableRead(existingValue);

    // Handle references: need to dereference when used as rvalue
    if (existingValue->isReference && !isRef && !lvalue) {
        messageSystem::startBlock(this, "Generating reference usage", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

        // valType is the pointer type; load the pointer, then deref through it using the base type
        llvm::Value* ptr = llvmIRBuilder->CreateLoad(valType, A, token->tokenStr + "_ref_ptr");
        bool wasDefined = true;
        llvm::Type* baseType = getLLVMTypeFromString(existingValue->asaTypeInstance->strVal, 0, this, wasDefined, pass);
        if (!baseType || !wasDefined) {
            return messageSystem::error("Cannot resolve ref base type for dereference");
        }
        if (!asaType)
            asaType = new AsaTypeInstance(baseType);
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

// OPENCODE:
// Codegen for #caller_filepath: returns the filepath string passed by the
// caller via @tracked_caller. Errors if used outside a @tracked_caller
// function. Returns a string struct or raw *char in the string module.
void* ASTNode::generateCallerFilepathDirective(int pass)
{
    if (callerLocationParamStack.empty())
        return messageSystem::error("#caller_filepath used outside of a function with @tracked_caller", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    if (!compilerDirectiveFlags["IN_STRING_MODULE"] &&
        structDefinitions.count("string") && structDefinitions["string"]->structVal) {
        llvm::Argument* rawPtr = callerLocationParamStack.top().filepath;
        StructType* strTy = cast<StructType>((llvm::Type*)structDefinitions["string"]->structVal);
        llvm::Value* undef = UndefValue::get(strTy);
        llvm::Value* addr = llvmIRBuilder->CreateInsertValue(undef, rawPtr, {0});
        FunctionType* strlenFuncType = FunctionType::get(llvm::Type::getInt64Ty(*llvmCompileContext), {PointerType::getUnqual(*llvmCompileContext)}, false);
        FunctionCallee strlenFn = llvmCompileModule->getOrInsertFunction("strlen", strlenFuncType);
        llvm::Value* lenVal = llvmIRBuilder->CreateCall(strlenFn, {rawPtr}, "caller_strlen");
        llvm::Value* lenTrunc = llvmIRBuilder->CreateTrunc(lenVal, Type::getInt32Ty(*llvmCompileContext), "caller_len");
        return llvmIRBuilder->CreateInsertValue(addr, lenTrunc, {1});
    }

    return callerLocationParamStack.top().filepath;
}

// OPENCODE:
// Codegen for #caller_linenum: returns the line number i64 passed by the
// caller via @tracked_caller. Errors if used outside a @tracked_caller function.
void* ASTNode::generateCallerLineNumDirective(int pass)
{
    if (callerLocationParamStack.empty())
        return messageSystem::error("#caller_linenum used outside of a function with @tracked_caller", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    return callerLocationParamStack.top().lineNum;
}

// OPENCODE:
// Codegen for #caller_line: returns the source line content string passed by
// the caller via @tracked_caller. Errors if used outside a @tracked_caller
// function. Returns a string struct or raw *char in the string module.
void* ASTNode::generateCallerLineDirective(int pass)
{
    if (callerLocationParamStack.empty())
        return messageSystem::error("#caller_line used outside of a function with @tracked_caller", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    if (!compilerDirectiveFlags["IN_STRING_MODULE"] &&
        structDefinitions.count("string") && structDefinitions["string"]->structVal) {
        llvm::Argument* rawPtr = callerLocationParamStack.top().lineContent;
        StructType* strTy = cast<StructType>((llvm::Type*)structDefinitions["string"]->structVal);
        llvm::Value* undef = UndefValue::get(strTy);
        llvm::Value* addr = llvmIRBuilder->CreateInsertValue(undef, rawPtr, {0});
        FunctionType* strlenFuncType = FunctionType::get(llvm::Type::getInt64Ty(*llvmCompileContext), {PointerType::getUnqual(*llvmCompileContext)}, false);
        FunctionCallee strlenFn = llvmCompileModule->getOrInsertFunction("strlen", strlenFuncType);
        llvm::Value* lenVal = llvmIRBuilder->CreateCall(strlenFn, {rawPtr}, "caller_strlen");
        llvm::Value* lenTrunc = llvmIRBuilder->CreateTrunc(lenVal, Type::getInt32Ty(*llvmCompileContext), "caller_len");
        return llvmIRBuilder->CreateInsertValue(addr, lenTrunc, {1});
    }

    return callerLocationParamStack.top().lineContent;
}

// OPENCODE:
// Codegen for `throw`: evaluates the optional expression argument, prints
// "Exception: file: ... line: ..." with the expression value via printf,
// then calls exit(1). Used for runtime error reporting.
void* ASTNode::generateThrow(int pass)
{
    messageSystem::startBlock(this, "Generating throw statement", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // If it has a child node, we will output it's value as a string
    ASTNode* exprNode = nullptr;
    llvm::Value* outVal = nullptr;
    if (childNodes.size() > 0) {
        exprNode = childNodes[0]->childNodes[0];
        if (exprNode->codegen == nullptr) {
            return messageSystem::error("Node `" + ASTNodeTypeAsString(exprNode->nodeType) + "` does not have a code generator");
        }
        outVal = (llvm::Value*)(exprNode->*(exprNode->codegen))(pass);
    }
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }

    // Create and print the prefix message first
    std::string throwPrefix = "Exception:  file: \"" + *(token->filePath) + "\"   line: " + std::to_string(token->lineNumber) + "\n    ";
    llvm::Value* prefixStr = llvmIRBuilder->CreateGlobalString(throwPrefix);

    // Look up print function for the prefix (char* type)
    argumentList prefixArgList;
    prefixArgList.push_back(AsaArgumentVariableValue("byte", Byte_Type, 1));
    std::string printFnName = "print";
    AsaFunctionDefinition* prefixPrintFnID = getFunctionFromID(AsaFunctionDefinitions, printFnName, prefixArgList, true, false);

    if (prefixPrintFnID) {
        std::vector<llvm::Value*> prefixArgs;
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
            if (!compilerDirectiveFlags["IN_STRING_MODULE"] &&
                structDefinitions.count("string") && structDefinitions["string"]->structVal)
                typeStr = "string";
            else
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

        argList.push_back(AsaArgumentVariableValue(baseTypeStr, getASTNodeTypeFromString(baseTypeStr), pointerLevel));

        // Look up the print function
        std::string printFnName = "printl";
        AsaFunctionDefinition* printFnID = getFunctionFromID(AsaFunctionDefinitions, printFnName, argList, true, false);

        if (printFnID) {
            // Call the print function
            std::vector<llvm::Value*> printArgs;
            printArgs.push_back(outVal);
            llvmIRBuilder->CreateCall(printFnID->fnValue, printArgs);
            printFnID->uses++;
        }
    }

    // Declare exit function if not already declared
    FunctionType* exitFuncType = FunctionType::get(
        llvm::Type::getVoidTy(*llvmCompileContext),
        {Type::getInt32Ty(*llvmCompileContext)},
        false);
    FunctionCallee exitFunc = llvmCompileModule->getOrInsertFunction("exit", exitFuncType);

    // Call exit(1) to terminate the program
    llvmIRBuilder->CreateCall(exitFunc, {ConstantInt::get(llvm::Type::getInt32Ty(*llvmCompileContext), 1)});

    // Create an unreachable instruction since exit() doesn't return
    llvmIRBuilder->CreateUnreachable();

    messageSystem::endBlock();

    return nullptr;
}

// OPENCODE:
// Codegen for `throw_caller`: prints the caller's filepath, line number, and
// line content via printf, then calls exit(1). Only valid inside @tracked_caller
// functions.
void* ASTNode::generateThrowCaller(int pass)
{
    messageSystem::startBlock(this, "Generating throw_caller statement", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (callerLocationParamStack.empty())
        return messageSystem::error("throw_caller used outside of a @tracked_caller function");

    llvm::Argument* callerFile = callerLocationParamStack.top().filepath;
    llvm::Argument* callerLine = callerLocationParamStack.top().lineNum;
    llvm::Argument* callerLineContent = callerLocationParamStack.top().lineContent;

    FunctionType* printfFuncType = FunctionType::get(
        llvm::Type::getInt32Ty(*llvmCompileContext),
        {PointerType::getUnqual(*llvmCompileContext)},
        true);
    FunctionCallee printfFunc = llvmCompileModule->getOrInsertFunction("printf", printfFuncType);

    llvm::Value* formatStr = llvmIRBuilder->CreateGlobalString("Exception at %s\n%d |  %s\n");
    llvmIRBuilder->CreateCall(printfFunc, {formatStr, callerFile, callerLine, callerLineContent});

    // Evaluate and print the thrown value if present
    if (childNodes.size() > 0) {
        size_t childIndex = 0;
        ASTNode* exprNode = childNodes[childIndex];

        void* rawVal = (exprNode->*(exprNode->codegen))(pass);
        llvm::Value* outVal = (llvm::Value*)rawVal;

        argumentList argList;
        std::string typeStr = "";

        if (exprNode->nodeType == String_Constant_Node) {
            if (!compilerDirectiveFlags["IN_STRING_MODULE"] &&
                structDefinitions.count("string") && structDefinitions["string"]->structVal)
                typeStr = "string";
            else
                typeStr = "*char";
        }
        else {
            typeStr = getStringTypeFromLLVMType(outVal->getType());
        }

        uint8_t pointerLevel = 0;
        std::string baseTypeStr = typeStr;
        while (baseTypeStr.length() > 0 && baseTypeStr[0] == '*') {
            pointerLevel++;
            baseTypeStr = baseTypeStr.substr(1);
        }

        argList.push_back(AsaArgumentVariableValue(baseTypeStr, getASTNodeTypeFromString(baseTypeStr), pointerLevel));

        std::string printFnName = "printl";
        AsaFunctionDefinition* printFnID = getFunctionFromID(AsaFunctionDefinitions, printFnName, argList, true, false);

        if (printFnID) {
            std::vector<llvm::Value*> printArgs;
            printArgs.push_back(outVal);
            llvmIRBuilder->CreateCall(printFnID->fnValue, printArgs);
            printFnID->uses++;
        }
    }

    FunctionType* exitFuncType = FunctionType::get(
        llvm::Type::getVoidTy(*llvmCompileContext),
        {Type::getInt32Ty(*llvmCompileContext)},
        false);
    FunctionCallee exitFunc = llvmCompileModule->getOrInsertFunction("exit", exitFuncType);

    llvmIRBuilder->CreateCall(exitFunc, {ConstantInt::get(llvm::Type::getInt32Ty(*llvmCompileContext), 1)});

    llvmIRBuilder->CreateUnreachable();

    messageSystem::endBlock();

    return nullptr;
}

// OPENCODE:
// Codegen for `return`: evaluates the return expression, casts it to the
// function's return type, stores it into the result slot if inside a value
// block, then creates a branch to the merge block or a direct ret instruction.
// Handles ref returns by forcing lvalue mode.
void* ASTNode::generateReturn(int pass)
{
    messageSystem::startBlock(this, "Generating return", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (!LexicalBlocks.empty() && token && token->filePath)
        llvmIRBuilder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));
    ASTNode* exprNode = childNodes[0];
    Function* currentFunc = llvmIRBuilder->GetInsertBlock()->getParent();
    AsaFunctionDefinition* fnID = getFunctionIDFromFunctionPointer(AsaFunctionDefinitions, currentFunc);
    if (fnID && fnID->returnIsReference)
        exprNode->lvalue = true;

    messageSystem::startBlock(exprNode, "Generating return expression value", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (fnID && fnID->returnIsReference) {
        ASTNode* baseNode = unwrapSingleExpressionNode(exprNode);
        while (baseNode && (baseNode->nodeType == Member_Access || baseNode->nodeType == Access_Operation) && !baseNode->childNodes.empty())
            baseNode = unwrapSingleExpressionNode(baseNode->childNodes[0]);

        if (baseNode && baseNode->nodeType == Identifier_Node) {
            AsaVariableValue* returnedValue = findNamedValue(parentNode, this, baseNode->token->tokenStr, token);
            if (wasError)
                return nullptr;
            if (returnedValue && isa<AllocaInst>(returnedValue->llvmValue) && !returnedValue->isReference) {
                if (returnedValue->declNode)
                    messageSystem::addAttribute(returnedValue->declNode, "declared here");
                return messageSystem::error("Cannot return reference to local value '" + baseNode->token->tokenStr + "'");
            }
        }
    }

    llvm::Value* RetVal = (llvm::Value*)(exprNode->*(exprNode->codegen))(pass);
    if (wasError) {
        return messageSystem::error("Failed to generate return expression value");
    }
    if (fnID && fnID->returnIsReference) {
        ASTNode* baseNode = unwrapSingleExpressionNode(exprNode);
        while (baseNode && (baseNode->nodeType == Member_Access || baseNode->nodeType == Access_Operation) && !baseNode->childNodes.empty())
            baseNode = unwrapSingleExpressionNode(baseNode->childNodes[0]);

        if (baseNode && baseNode->nodeType == Identifier_Node) {
            AsaVariableValue* returnedValue = findNamedValue(parentNode, this, baseNode->token->tokenStr, token);
            if (wasError)
                return nullptr;
            if (returnedValue && returnedValue->isReference &&
                unwrapSingleExpressionNode(exprNode) == baseNode &&
                RetVal && isa<AllocaInst>(RetVal)) {
                RetVal = llvmIRBuilder->CreateLoad(getTypePtrFromLLVMValue(RetVal), RetVal, baseNode->token->tokenStr + "_ref_return_ptr");
            }
        }
    }
    // Check if we're returning a struct
    llvm::Type* returnType = llvmIRBuilder->GetInsertBlock()->getParent()->getReturnType();
    if (fnID->isStructReturn) {
        // Get the sret parameter (first parameter)
        llvm::Value* sretPtr = &*currentFunc->arg_begin();

        // Copy the struct value to the sret location
        if (RetVal->getType()->isPointerTy()) {
            // If RetVal is a pointer to struct, memcpy from it
            llvm::Value* structSize = ConstantInt::get(llvm::Type::getInt64Ty(*llvmCompileContext),
                llvmCompileModule->getDataLayout().getTypeAllocSize(returnType));

            // Create memcpy call
            Function* memcpyFunc = Intrinsic::getOrInsertDeclaration(llvmCompileModule.get(), Intrinsic::memcpy, {sretPtr->getType(), RetVal->getType(), llvm::Type::getInt64Ty(*llvmCompileContext)});
            llvmIRBuilder->CreateCall(memcpyFunc, {sretPtr, RetVal, structSize, ConstantInt::get(llvm::Type::getInt1Ty(*llvmCompileContext), 0)});
        }
        else {
            // If RetVal is a struct value, store it
            llvmIRBuilder->CreateStore(RetVal, sretPtr);
        }


        llvmIRBuilder->CreateRetVoid();
    }
    else {
        // Non-struct return, handle normally
        llvm::Type* retType = llvmIRBuilder->GetInsertBlock()->getParent()->getReturnType();
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
                AsaFunctionDefinition* fnID = getFunctionIDFromFunctionPointer(AsaFunctionDefinitions, currentFn);
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

// llvm::Value*
void* ASTNode::generateResult(int pass)
{
    messageSystem::startBlock(this, "Generating result statement", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (resultContextStack.empty()) {
        return messageSystem::error("'result' used outside a value block");
    }

    ASTNode* exprNode = childNodes[0];
    llvm::Value* val = (llvm::Value*)(exprNode->*(exprNode->codegen))(pass);
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

// llvm::Value*
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
    llvm::Value* exprVal = (llvm::Value*)(exprNode->*(exprNode->codegen))(pass);
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

// llvm::Value*
void* ASTNode::generateIncDecrement(int pass)
{
    messageSystem::startBlock(this, "Generating unary increment/decrement operation", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    ASTNode* operand = childNodes[0];
    llvm::Value* targetPtr = nullptr;
    llvm::Type* targetType = nullptr;

    messageSystem::startBlock(operand, "Generating variable", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (operand->nodeType == Identifier_Node) {
        AsaVariableValue* val = findNamedValue(parentNode, this, operand->token->tokenStr, token);
        if (!val) {
            return messageSystem::error("Unknown variable '" + operand->token->tokenStr + "'");
        }
        if (val->isConstant) {
            return messageSystem::error("Cannot modify const variable '" + operand->token->tokenStr + "'");
        }
        if (val->isUndefined)
            messageSystem::warning("Variable '" + operand->token->tokenStr + "' may be undefined", messageSystem::Undefined_Variable_Warning);
        targetPtr = val->llvmValue;
        targetType = getTypePtrFromLLVMValue(targetPtr);
        val->isUndefined = false;
    }
    else {
        operand->lvalue = true;
        targetPtr = (llvm::Value*)(operand->*(operand->codegen))(pass);
        if (wasError)
            return nullptr;
        if (!targetPtr || !targetPtr->getType()->isPointerTy()) {
            return messageSystem::error("Operand of ++/-- must be an lvalue");
        }
        targetType = getTypePtrFromLLVMValue(targetPtr);
    }

    messageSystem::endBlock();


    llvm::Value* current = llvmIRBuilder->CreateLoad(targetType, targetPtr, "incdec_load");
    llvm::Value* updated = nullptr;
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

// Check if an AST expression is a compile-time constant expression.
// Handles literals, constant binary/unary expressions, and function calls
// where all arguments are themselves constant expressions.
static bool isConstantExpression(ASTNode* node)
{
    if (!node)
        return false;
    switch (node->nodeType) {
        case Integer_Node:
        case Float_Node:
        case Boolean_Node:
        case String_Constant_Node:
        case Character_Constant_Node:
        case Default_Initializer_Node:
        case Undefined_Initializer_Node:
            return true;
        case Function_Call: {
            if (node->childNodes.empty())
                return false;
            ASTNode* argsNode = node->childNodes[0];
            if (argsNode->nodeType != Arguments)
                return false;
            for (auto* arg : argsNode->childNodes) {
                ASTNode* argExpr = arg;
                if (arg->childNodes.size() == 1)
                    argExpr = arg->childNodes[0];
                if (!isConstantExpression(argExpr))
                    return false;
            }
            return true;
        }
        case Address_Of_Operation:
            if (!node->childNodes.empty())
                return isConstantExpression(node->childNodes[0]);
            return false;
        case Expression_Plus:
        case Expression_Minus:
        case Expression_Times:
        case Expression_Divide:
        case Expression_Modulo:
        case Bitwise_And:
        case Bitwise_Or:
        case Bitwise_Xor:
        case Bitwise_Not:
        case Bitwise_Shift_Left:
        case Bitwise_Shift_Right:
        case Logical_And:
        case Logical_Or:
        case Compare_Equal:
        case Compare_Not:
        case Compare_Less:
        case Compare_Greater:
        case Compare_LessEqual:
        case Compare_GreaterEqual:
            for (auto* child : node->childNodes)
                if (!isConstantExpression(child))
                    return false;
            return true;
        default:
            return false;
    }
}

// llvm::Value*
void* ASTNode::generateExpressionStatement(int pass)
{
    messageSystem::startBlock(this, "Generating runtime statement expression", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    ASTNode* leftNode = childNodes[0];
    ASTNode* declarationNode = this;
    //ASTNode* declarationNode = leftNode->nodeType == Colon_Separator_Node ? leftNode : this;
    ASTNode* exprNode = childNodes[1];
    Function* theFunction = llvmIRBuilder->GetInsertBlock()->getParent();
    ASTNode* initializerNode = unwrapSingleExpressionNode(exprNode);
    bool rhsIsUndefined = initializerNode && initializerNode->nodeType == Undefined_Initializer_Node;
    bool rhsIsDefault = initializerNode && initializerNode->nodeType == Default_Initializer_Node;
    bool rhsIsInitial = initializerNode && initializerNode->nodeType == Initial_Initializer_Node;

    // Generate RHS value {{{

    messageSystem::startBlock(exprNode, "Generating expression right side", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Set debug location if available
    if (!LexicalBlocks.empty() && token && token->filePath) {
        llvmIRBuilder->SetCurrentDebugLocation(
            DILocation::get(LexicalBlocks.back()->getContext(),
                token->lineNumber + 1,  // Line (DWARF is 1-indexed)
                0,                      // Column
                LexicalBlocks.back()));
    }

    llvm::Value* exprVal = nullptr;
    if (!rhsIsUndefined && !rhsIsDefault && !rhsIsInitial) {
        // Evaluate right side (rvalue)
        exprVal = (llvm::Value*)(exprNode->*(exprNode->codegen))(pass);
        if (wasError) {
            return nullptr;
        }
        if (!exprVal) {
            return messageSystem::error("Set expression requires right argument");
        }
    }

    messageSystem::endBlock();
    //}}}

    // Generate LHS Operation {{{
    messageSystem::startBlock(leftNode, "Generating expression left side", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    llvm::Value* targetPtr = nullptr;
    llvm::Type* targetType = nullptr;
    bool targetIsSigned = true;
    AsaVariableValue* targetValue = nullptr;

    // If the left side is a pointer lvalue
    if (leftNode->nodeType != Identifier_Node && leftNode->nodeType != Colon_Separator_Node) {
        leftNode->lvalue = true;
        // left side is an expression, evaluate to pointer (lvalue address)
        size_t stackDepthBefore = lastRetrievedElementType.size();
        targetPtr = (llvm::Value*)(leftNode->*(leftNode->codegen))(pass);
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
    llvm::Type* type = nullptr;
    int pointerLevel = 0;
    bool isConst = false;
    int defaultPointerLevel = 0;
    AsaTypeInstance* asaTypeInstance = nullptr;
    if (!leftNode->lvalue) {
        // If the left node has children, it is likely a typed identifier like: `x : int = ...`
        if (leftNode->childNodes.size() > 0) {
            // The type node is the one on the right, containing any type modifiers: `... ref const * int ...`
            typeNode = leftNode->childNodes[1];
            // Create Asa type instance using the type node. Modifies `typeNode` to be only the type name with no modifiers
            asaTypeInstance = CreateAsaTypeInstanceFromASTNode(typeNode);
            type = asaTypeInstance->llvmType;
            //getNextPointerLevel:
            //    if (typeNode->token->tokenStr == "const") {
            //        isConst = true;
            //        typeNode = typeNode->childNodes[0];
            //        goto getNextPointerLevel;
            //    }
            //    if (typeNode->token->tokenStr == "ref" || typeNode->token->tokenStr == "exact") {
            //        typeNode = typeNode->childNodes[0];
            //        goto getNextPointerLevel;
            //    }
            //    if (typeNode->token->tokenStr == "*") {
            //        pointerLevel++;
            //        typeNode = typeNode->childNodes[0];
            //        goto getNextPointerLevel;
            //    }
            //bool wasDefined = true;
            //type = getLLVMTypeFromString(typeNode->token->tokenStr, 0, typeNode, wasDefined, pass);
            //if (wasDefined == false)
            //  return nullptr;
            //for (int pL = 0; pL < pointerLevel; pL++)
            //    type = PointerType::get(*llvmCompileContext, 0);
            defaultPointerLevel = asaTypeInstance->pointerLevel;
        }
    }
    //else
    //      type = var->getType();

    // If the rhs is the ? symbol, like: `x : int = ?;`, then make sure the type is explicit
    if (rhsIsUndefined) {
        if (asaTypeInstance == nullptr)
            return messageSystem::error("Cannot infer type from undefined value '?'. Use an explicit type annotation.");
    }
    // If the rhs is the `default` keyword, then generate the "default" value for the type
    else if (rhsIsDefault) {
        if (asaTypeInstance == nullptr)
            return messageSystem::error("Cannot infer type from default value 'default'. Use an explicit type annotation.");
        else {
            exprVal = generateDefaultValueForType(asaTypeInstance, pass, this);
            if (wasError || !exprVal)
                return nullptr;
        }
    }
    // If the rhs is the `initial` keyword, then wait until the lhs is processed to know what to do
    else if (rhsIsInitial) {
        // type will be resolved from the target variable during LHS processing
    }
    // If the type is not explicitly set, then automatically resolve type from the expression
    else if (type == nullptr) {
        type = exprVal->getType();
    }
    // Otherwise, the type is explicit AND the rhs is a normal value: cast it (if necessary)
    else {
        messageSystem::startBlock(typeNode, "Generating type", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

        // TODO: Use the new type system for this:
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

    // If the initializer expression is a string literal, AND the variable is not a const or a pointer,
    // then this variable is mutable, and the string must be copied from the constant memory first:
    if (!rhsIsUndefined && !rhsIsDefault && exprVal && initializerNode &&
        initializerNode->nodeType == String_Constant_Node && !isConst &&
        structDefinitions.count("string") && structDefinitions["string"]->structVal &&
        ((typeNode && asaTypeInstance->baseType->typeName == "string" && pointerLevel == 0) ||
            (!typeNode && exprVal->getType() == structDefinitions["string"]->structVal))) {
        exprVal = createMutableStringCopy(initializerNode, exprVal, pass);
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
    std::string identifierString = leftNode->token->tokenStr;
    if (leftNode->nodeType == Identifier_Node) {

        // Simple variable: find alloca and use it as targetPtr.
        // If there is an explicit type annotation, this is always a new declaration
        AsaVariableValue* lookupAsaValue = typeNode ? nullptr : findNamedValue(parentNode, this, identifierString, token);
        if (wasError)
            return nullptr;

        // If this variable name doesnt exist in this context
        if (!lookupAsaValue) {
            // First check if it is a compiler definition, like `FOO :: 5;` which is improperly using `=`
            if (!typeNode && findCompilerDefinition(parentNode, identifierString)) {
                return messageSystem::error(
                    "Cannot modify compiler constant '" + identifierString +
                    "' with '='. Use '::' to redefine it at compile time.");
            }
            // TODO: Is this line even reachable?
            // Then check to see if the variable is being redeclared in the same scope
            if (typeNode && checkSameScopeRedeclaration(parentNode, declarationNode, identifierString))
                return nullptr;

            // TODO: Wouldn't the errors earlier always throw instead of this one?
            if ((rhsIsUndefined || rhsIsDefault) && !typeNode)
                return messageSystem::error("Cannot infer type from this initializer. Use an explicit type annotation.");

            // Create the variable allocation
            targetPtr = CreateEntryBlockAlloca(theFunction, type, identifierString);
            //// TODO: Replace `actualType` with the type instances `strVal`
            //std::string actualType = "*int";
            //if (!typeNode) {
            //    actualType = getStringTypeFromLLVMType(type);
            //    // Fall back to the declared type string if LLVM type inference fails
            //    if (actualType.find("unknown") != std::string::npos && exprNode->asaType && !exprNode->asaType->strVal.empty())
            //        actualType = exprNode->asaType->strVal;
            //}
            //else
            //    actualType = (pointerLevel > 0 ? std::string(pointerLevel, '*') : "") + typeNode->token->tokenStr;

            // Add the variable definition to the local `namedValues` map
            namedValues[identifierString] = new AsaVariableValue(identifierString, asaTypeInstance, targetPtr);
            namedValues[identifierString]->isConstant = isConst;
            namedValues[identifierString]->isUndefined = rhsIsUndefined;
            namedValues[identifierString]->declNode = declarationNode;
            namedValues[identifierString]->initialValueNode = initializerNode;
            targetValue = namedValues[identifierString];
            declarationNode->tracksVariableUsage = true;
            newConstLocal = isConst;

            // Add debug info ONLY if we have a valid scope and the stack is not empty
            if (llvmDebugBuilder && !LexicalBlocks.empty() && token && token->filePath) {
                DIScope* Scope = LexicalBlocks.back();
                unsigned LineNo = token->lineNumber + 1;  // DWARF is 1-indexed

                // Create DIType
                DIType* DebugType = createDIType(type, asaTypeInstance->strVal);

                DIFile* VarFile = (token->filePath && !token->filePath->empty())
                                      ? getDIFile(*token->filePath)
                                      : llvmDebugFile;
                if (DebugType) {
                    DILocalVariable* D = llvmDebugBuilder->createAutoVariable(
                        Scope,
                        identifierString,
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
        // Otherwise this variable does exist already, this is a set operation rather than definition
        else {
            // Check if trying to modify a const variable
            if (lookupAsaValue->asaTypeInstance->isConst) {
                return messageSystem::error("Cannot modify const variable '" + identifierString + "'");
            }

            targetPtr = lookupAsaValue->llvmValue;
            targetValue = lookupAsaValue;
            // TODO: `resolvedType` should be replaced by usage of the asaTypeInstance
            std::string resolvedType = resolveTypeAlias(lookupAsaValue->asaTypeInstance->strVal);
            // TODO: The signed value should be stored in the asaTypeInstance->baseType, rather than using `typeSigns`
            targetIsSigned = lookupAsaValue->asaTypeInstance->baseType->isSigned;
            //defaultTypeName = lookupAsaValue->typeString;
            //defaultPointerLevel = 0;
            //while (!defaultTypeName.empty() && defaultTypeName[0] == '*') {
            //    defaultTypeName = defaultTypeName.substr(1);
            //    defaultPointerLevel++;
            //}

            // If the variable is a reference, load the pointer before storing through it
            if (lookupAsaValue->asaTypeInstance->isRef) {
                targetPtr = llvmIRBuilder->CreateLoad(lookupAsaValue->asaTypeInstance->llvmType, targetPtr, identifierString + "_ref_store_ptr");
                //// Resolve element type from the type string rather than the alloca type
                //std::string refTypeStr = lookupAsaValue->asaTypeInstance->strVal;
                //int refPtrLvl = 0;
                //while (!refTypeStr.empty() && refTypeStr[0] == '*') {
                //    refTypeStr = refTypeStr.substr(1);
                //    refPtrLvl++;
                //}
                //bool refWd = true;
                //targetType = getLLVMTypeFromString(refTypeStr, 0, this, refWd, pass);
                targetType = lookupAsaValue->asaTypeInstance->llvmType;
                // TODO: Is this necessary?
                if (targetType && lookupAsaValue->asaTypeInstance->pointerLevel > 0)
                    targetType = PointerType::getUnqual(*llvmCompileContext);
                // TODO: This might be an error case in the new type system, actually:
                if (!targetType)
                    targetType = getTypePtrFromLLVMValue(lookupAsaValue->llvmValue);
            }
            else {
                targetType = getTypePtrFromLLVMValue(targetPtr);
            }
        }
    }
    else if (targetType == nullptr)
        targetType = type;

    messageSystem::endBlock();
    // }}}

    // TODO: Are these checks necessary? They are essentially duplicates fromt the above ones
    // If the rhs is an undefined keyword `?`, just mark it as "isUndefined", and return
    if (rhsIsUndefined) {
        messageSystem::endBlock();
        if (targetValue)
            targetValue->isUndefined = true;
        markVariableWrite(targetValue);
        return targetPtr;
    }
    // If the rhs is a `default` keyword, generate the default value for that specific type
    if (rhsIsDefault && !exprVal) {
        if (!targetType)
            return messageSystem::error("Cannot infer type from 'default'. Use an explicit type annotation.");
        exprVal = generateDefaultValueForType(targetType, asaTypeInstance->baseType->typeName, defaultPointerLevel, pass, this);
        if (wasError || !exprVal)
            return nullptr;
    }
    if (rhsIsInitial && !exprVal) {
        if (isDeclaration)
            return messageSystem::error("Cannot use 'initial' in a declaration");
        if (!targetValue || !targetValue->initialValueNode)
            return messageSystem::error("Variable '" + identifierString + "' has no initial value");
        ASTNode* initNode = targetValue->initialValueNode;
        if (initNode->nodeType == Undefined_Initializer_Node)
            return messageSystem::error("'" + identifierString + "' was declared with '?', which has no defined initial value");
        if (initNode->nodeType == Default_Initializer_Node) {
            if (!targetType)
                return messageSystem::error("Cannot infer type from 'default'. Use an explicit type annotation.");
            exprVal = generateDefaultValueForType(targetType, asaTypeInstance->baseType->typeName, defaultPointerLevel, pass, this);
        }
        else {
            exprVal = (llvm::Value*)(initNode->*(initNode->codegen))(pass);
        }
        if (wasError || !exprVal)
            return nullptr;
        if (!llvm::dyn_cast<llvm::Constant>((llvm::Value*)exprVal) && !isConstantExpression(initNode))
            return messageSystem::error("Initial value of '" + identifierString + "' is not a compile-time constant");
    }

    // Check if it is a compound assignment operation, like +=, and if so, process accordingly
    {
        messageSystem::startBlock(this, "Generating compound assignment operation", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

        // Compound assignment: load current value, apply op, then store result
        if (token->tokenType == Plus_Equal || token->tokenType == Minus_Equal ||
            token->tokenType == Times_Equal || token->tokenType == Slash_Equal ||
            token->tokenType == Ampersand_Equal || token->tokenType == Bar_Equal ||
            token->tokenType == Caret_Equal || token->tokenType == Shift_Left_Equal ||
            token->tokenType == Shift_Right_Equal) {
            // Use targetType directly; LLVM may fold GEPs, making instruction introspection unreliable.
            llvm::Type* loadType = targetType;
            if (targetValue && targetValue->isUndefined)
                messageSystem::warning("Variable '" + identifierString + "' may be undefined", messageSystem::Undefined_Variable_Warning);
            markVariableRead(targetValue);
            llvm::Value* currentVal = llvmIRBuilder->CreateLoad(loadType, targetPtr, "cmpd_load");

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
                    {Plus_Equal, "operator." + tokenTypeAsString(Plus)},
                    {Minus_Equal, "operator." + tokenTypeAsString(Minus)},
                    {Times_Equal, "operator." + tokenTypeAsString(Star)},
                    {Slash_Equal, "operator." + tokenTypeAsString(Slash)},
                    {Ampersand_Equal, "operator." + tokenTypeAsString(Ampersand)},
                    {Bar_Equal, "operator." + tokenTypeAsString(Bar)},
                    {Caret_Equal, "operator." + tokenTypeAsString(Caret)},
                    {Shift_Left_Equal, "operator." + tokenTypeAsString(Shift_Left)},
                    {Shift_Right_Equal, "operator." + tokenTypeAsString(Shift_Right)},
                };
                std::string operatorName = compoundOpName.at(token->tokenType);
                argumentList argList;
                std::string lTypeStr = getStringTypeFromLLVMType(currentVal->getType());
                argList.push_back(AsaArgumentVariableValue(lTypeStr, getASTNodeTypeFromString(lTypeStr), 0));
                std::string rTypeStr = getStringTypeFromLLVMType(exprVal->getType());
                argList.push_back(AsaArgumentVariableValue(rTypeStr, getASTNodeTypeFromString(rTypeStr), 0));
                AsaFunctionDefinition* calleeID = getFunctionFromID(AsaFunctionDefinitions, operatorName, argList, true);
                if (!calleeID || !calleeID->fnValue) {
                    return messageSystem::error("No operator overload '" + operatorName + "' found for compound assignment");
                }
                calleeID->uses++;
                std::vector<llvm::Value*> ArgsV = {currentVal, exprVal};

                // Append tracked_caller hidden parameters if the callee requires them
                if (calleeID->isTrackedCaller && token && token->filePath) {
                    llvm::Value* hiddenFilepath = llvmIRBuilder->CreateGlobalString(*token->filePath);
                    llvm::Value* hiddenLineNum = ConstantInt::get(Type::getInt32Ty(*llvmCompileContext), token->lineNumber);
                    llvm::Value* hiddenLineContent = llvmIRBuilder->CreateGlobalString(
                        token->lineValue ? *token->lineValue : "");
                    ArgsV.push_back(hiddenFilepath);
                    ArgsV.push_back(hiddenLineNum);
                    ArgsV.push_back(hiddenLineContent);
                }

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
    }

    // If regular assignment for an existing variable
    {
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
        if (targetValue) {
            targetValue->isUndefined = false;
            markVariableWrite(targetValue);
        }

        // For a newly declared const local, tell the optimizer this memory is
        // invariant after initialization so it can treat reads as constants.
        // Only emit invariant.start when in the entry block: non-entry blocks
        // (e.g. after a branch) can cause numbering conflicts in LLVM 21.
        if (newConstLocal && theFunction) {
            BasicBlock* curBB = llvmIRBuilder->GetInsertBlock();
            BasicBlock* entryBB = &theFunction->getEntryBlock();
            if (curBB == entryBB) {
                llvm::Type* storedType = exprVal->getType();
                uint64_t typeSize = llvmCompileModule->getDataLayout().getTypeAllocSize(storedType);
                Function* invariantStartFn = Intrinsic::getOrInsertDeclaration(llvmCompileModule.get(), Intrinsic::invariant_start, {PointerType::getUnqual(*llvmCompileContext)});
                llvmIRBuilder->CreateCall(invariantStartFn,
                    {ConstantInt::get(llvm::Type::getInt64Ty(*llvmCompileContext), typeSize), targetPtr});
            }
        }

        messageSystem::endBlock();
    }


    messageSystem::endBlock();
    return exprVal;
}

// llvm::Value*
void* ASTNode::generateIterator(int pass)
{
    ASTNode* identifierNode = childNodes[0];
    llvm::Value* var = nullptr;
    //llvm::Value* var = (llvm::Value*)(findNamedValue(parentNode, this, token->tokenStr)->llvmValue);
    //if (!var) {
    llvm::Type* type = llvm::Type::getInt32Ty(*llvmCompileContext);
    var = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), type, identifierNode->token->tokenStr);
    //}
    return var;
}

// llvm::Value*
void* ASTNode::generateUnaryExpression(int pass)
{
    messageSystem::startBlock(this, "Generating unary expression", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (childNodes.size() == 0) {
        return messageSystem::error("Unary expression requires argument");
    }
    if (childNodes[0]->codegen == nullptr) {
        return messageSystem::error("Node `" + ASTNodeTypeAsString(childNodes[0]->nodeType) + "` does not have a code generator");
    }
    llvm::Value* R = (llvm::Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
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
            // Plain variable: return the alloca directly without touching asaType
            if (child->nodeType == Identifier_Node) {
                AsaVariableValue* val = findNamedValue(parentNode, this, child->token->tokenStr, token);
                if (!val && !wasError) {
                    return messageSystem::error("Unknown variable name for address-of");
                }
                messageSystem::endBlock();
                return (llvm::Value*)(val->llvmValue);
            }
            // For member access, array subscript, etc.: evaluate as lvalue to get pointer
            child->lvalue = true;
            llvm::Value* ptr = (llvm::Value*)(child->*(child->codegen))(pass);
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
            llvm::Value* ptrVal = (llvm::Value*)(ptrNode->*(ptrNode->codegen))(pass);
            if (wasError) {
                messageSystem::endBlock();
                return nullptr;
            }
            if (!ptrVal) {
                return messageSystem::error("Dereference of null pointer");
            }
            // Determine element type from allocation when available
            llvm::Type* elementType = nullptr;
            std::string elementTypeStr;
            if (AllocaInst* allocaVal = dyn_cast<AllocaInst>(ptrVal)) {
                elementType = allocaVal->getAllocatedType();
            }
            else if (ptrNode->asaType && !ptrNode->asaType->strVal.empty() && ptrNode->asaType->strVal[0] == '*') {
                // Resolve the pointee type from the recorded ASA type string
                elementTypeStr = ptrNode->asaType->strVal.substr(1);
                bool wasDefined = true;
                elementType = getLLVMTypeFromString(elementTypeStr, 0, this, wasDefined, pass);
            }
            if (!elementType)
                elementType = llvm::Type::getInt32Ty(*llvmCompileContext);

            // If used as lvalue (*ptr = val), return the pointer so the caller stores through it
            if (lvalue) {
                if (!asaType)
                    asaType = new AsaTypeInstance(elementType);
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
            llvm::Value* v = (llvm::Value*)(valueNode->*(valueNode->codegen))(pass);
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
            llvm::Value* boolVal = R->getType()->isIntegerTy(1) ? R : llvmIRBuilder->CreateICmpNE(R, Constant::getNullValue(R->getType()), "tobool");
            llvm::Value* result = llvmIRBuilder->CreateNot(boolVal, "not_tmp");
            messageSystem::endBlock();
            return result;
        }

        case Bitwise_Not: {
            if (!R->getType()->isIntegerTy())
                return messageSystem::error("Bitwise complement requires an integer operand");
            llvm::Value* result = llvmIRBuilder->CreateNot(R, "bitnot_tmp");
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

// OPENCODE:
// Classifies an LLVM type into Integer, Float, Pointer, or Unknown category.
// Used by binary-op codegen to dispatch to the correct operation handler.
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

// OPENCODE:
// Returns true if typeStr is a signed type per the typeSigns table.
// TODO: This does not need to exist:
bool isSignedType(const std::string& typeStr)
{
    return typeSigns.count(typeStr) && typeSigns[typeStr];
}

// OPENCODE:
// Extracts type info from a Colon_Separator_Node (`name : type`): parses
// const/ref/exact modifiers and pointer indirections, then resolves the LLVM
// type via getLLVMTypeFromString. Outputs type, name, pointer level, and
// const flag. Returns false if the type can't be resolved.
static bool getDeclaredTypeFromColonNode(ASTNode* colonNode, llvm::Type*& outType, std::string& outTypeName, int& outPointerLevel, bool& outIsConst, int pass)
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
    outType = getLLVMTypeFromString(outTypeName, 0, typeNode, wasDefined, pass);
    if (!outType || !wasDefined)
        return false;

    for (int pL = 0; pL < outPointerLevel; pL++)
        outType = PointerType::get(*llvmCompileContext, 0);

    return true;
}

// OPENCODE:
// Generates a bare default declaration for a Colon_Separator_Node without an
// initializer, like `x : int;`: allocas space, creates an AsaVariableValue entry in namedValues, fills
// it with the type's default value (zero/undef/default-constructed), and
// returns the default value.
static llvm::Value* generateBareVariableDeclaration(ASTNode* colonNode, int pass)
{
    if (!colonNode || colonNode->childNodes.size() < 2)
        return nullptr;

    ASTNode* nameNode = colonNode->childNodes[0];
    ASTNode* typeNode = colonNode->childNodes[1];
    //llvm::Type* type = nullptr;
    //std::string typeName;
    //int pointerLevel = 0;
    //bool isConst = false;
    //if (!getDeclaredTypeFromColonNode(colonNode, type, typeName, pointerLevel, isConst, pass))
    //    return nullptr;
    AsaTypeInstance* asaTypeInstance = CreateAsaTypeInstanceFromASTNode(typeNode);

    Function* theFunction = llvmIRBuilder->GetInsertBlock()->getParent();
    AllocaInst* targetPtr = CreateEntryBlockAlloca(theFunction, asaTypeInstance->llvmType, nameNode->token->tokenStr);
    colonNode->namedValues[nameNode->token->tokenStr] = new AsaVariableValue(nameNode->token->tokenStr, asaTypeInstance, targetPtr);
    //colonNode->namedValues[nameNode->token->tokenStr]->isConstant = isConst;
    //colonNode->namedValues[nameNode->token->tokenStr]->isUndefined = false;
    colonNode->namedValues[nameNode->token->tokenStr]->declNode = colonNode;
    colonNode->tracksVariableUsage = true;
    //markVariableWrite(colonNode->namedValues[nameNode->token->tokenStr]);

    // Now get the "default" value for this type. For example, `x : int;` -> default:`0`, `s : string;` -> default:`""`
    llvm::Value* defaultValue = generateDefaultValueForType(asaTypeInstance, pass, colonNode);
    if (wasError || !defaultValue)
        return nullptr;

    // Then store this default value into the variable
    llvmIRBuilder->CreateStore(defaultValue, targetPtr);
    return defaultValue;
}

// llvm::Value*
void* ASTNode::generateBinaryExpression(int pass)
{
    messageSystem::startBlock(this, "Generating binary expression", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (nodeType == Colon_Separator_Node) {
        llvm::Value* defaultValue = generateBareVariableDeclaration(this, pass);
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
        llvm::Value* L = (llvm::Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
        if (wasError) {
            messageSystem::endBlock();
            return nullptr;
        }
        pipeOperationValue.push(L);
        // then process R
        llvm::Value* R = (llvm::Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);
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
        auto toBool = [&](llvm::Value* V, const std::string& name) -> llvm::Value* {
            return V->getType()->isIntegerTy(1) ? V : llvmIRBuilder->CreateICmpNE(V, Constant::getNullValue(V->getType()), name);
        };

        llvm::Value* L = (llvm::Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
        if (wasError) {
            messageSystem::endBlock();
            return nullptr;
        }
        if (!L) {
            return messageSystem::error("Error generating left side of logical expression");
        }

        llvm::Value* lBool = toBool(L, "tobool_l");
        Function* TheFunction = llvmIRBuilder->GetInsertBlock()->getParent();
        BasicBlock* LhsBB = llvmIRBuilder->GetInsertBlock();
        BasicBlock* RhsBB = BasicBlock::Create(*llvmCompileContext, nodeType == Logical_And ? "land.rhs" : "lor.rhs", TheFunction);
        BasicBlock* MergeBB = BasicBlock::Create(*llvmCompileContext, nodeType == Logical_And ? "land.end" : "lor.end");

        if (nodeType == Logical_And)
            llvmIRBuilder->CreateCondBr(lBool, RhsBB, MergeBB);
        else
            llvmIRBuilder->CreateCondBr(lBool, MergeBB, RhsBB);

        llvmIRBuilder->SetInsertPoint(RhsBB);
        llvm::Value* R = (llvm::Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);
        if (wasError) {
            messageSystem::endBlock();
            return nullptr;
        }
        if (!R) {
            return messageSystem::error("Error generating right side of logical expression");
        }

        llvm::Value* rBool = toBool(R, "tobool_r");
        llvmIRBuilder->CreateBr(MergeBB);
        BasicBlock* RhsEvalBB = llvmIRBuilder->GetInsertBlock();

        TheFunction->insert(TheFunction->end(), MergeBB);
        llvmIRBuilder->SetInsertPoint(MergeBB);

        PHINode* Phi = llvmIRBuilder->CreatePHI(llvm::Type::getInt1Ty(*llvmCompileContext), 2, nodeType == Logical_And ? "and_tmp" : "or_tmp");
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

    llvm::Value* L = (llvm::Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
    llvm::Value* R = (llvm::Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);

    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }
    if (!L || !R) {
        return messageSystem::error("Error generating term");
    }

    bool isComparison = intCompareOps.find(nodeType) != intCompareOps.end();
    bool leftIsPointer = L->getType()->isPointerTy();
    bool rightIsPointer = R->getType()->isPointerTy();
    bool leftIsInteger = L->getType()->isIntegerTy();
    bool rightIsInteger = R->getType()->isIntegerTy();
    if (isComparison && ((leftIsPointer && rightIsInteger) || (leftIsInteger && rightIsPointer))) {
        return messageSystem::error("Cannot compare pointer and integer without an explicit cast. Use int(ptr) or compare the pointer with '?'.");
    }

    // Check for operator overloads first (skip for pointer operands)
    bool eitherIsPointer = leftIsPointer || rightIsPointer;
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

// OPENCODE:
// Generates LLVM IR for an integer binary operation: arithmetic (add/sub/mul/
// div/rem), bitwise (and/or/xor), shifts (ashr/lshr), and comparisons (icmp).
// Dispatches via the integerOps/intCompareOps lookup maps. Honors signedness
// for shift-right via asaType tracking.
llvm::Value* ASTNode::generateIntegerBinaryOp(llvm::Value* L, llvm::Value* R)
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

    return (llvm::Value*)messageSystem::error("Unknown integer binary operator");
}

// OPENCODE:
// Generates LLVM IR for a float binary operation: arithmetic (fadd/fsub/fmul/
// fdiv) and comparisons (fcmp). Dispatches via the floatOps/floatCompareOps
// lookup maps.
llvm::Value* ASTNode::generateFloatBinaryOp(llvm::Value* L, llvm::Value* R)
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

    return (llvm::Value*)messageSystem::error("Unknown float binary operator");
}

// OPENCODE:
// Generates LLVM IR for a pointer binary operation: only equality/inequality
// comparisons are valid (ptr == ? / ptr != ?). Dispatches via intCompareOps.
llvm::Value* ASTNode::generatePointerBinaryOp(llvm::Value* L, llvm::Value* R)
{
    messageSystem::startBlock(this, "Generating pointer binary operation", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Pointer comparisons (== and !=, e.g. ptr == ? / ptr != ?)
    auto compIt = intCompareOps.find(nodeType);
    if (compIt != intCompareOps.end()) {
        messageSystem::endBlock();
        return llvmIRBuilder->CreateICmp(compIt->second, L, R, "ptr_cmp");
    }

    return (llvm::Value*)messageSystem::error("Invalid pointer operation");
}

// OPENCODE:
// Codegen for the pipe placeholder `$` in a pipe expression (`x |> f($)`):
// returns the top value from pipeOperationValue stack, which was pushed by
// the pipe operator codegen before evaluating the right-hand call.
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
static std::string getOperandTypeString(ASTNode* binaryNode, int childIdx, llvm::Value* llvmVal)
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
            AsaVariableValue* val = findNamedValue(binaryNode->parentNode, binaryNode, inner->token->tokenStr, binaryNode->token);
            if (val && !val->asaTypeInstance->strVal.empty())
                return val->asaTypeInstance->strVal;
        }
    }
    return getStringTypeFromLLVMType(llvmVal->getType());
}

// TODO: Broken operator overload
bool ASTNode::checkForOperatorOverload(llvm::Value* L, llvm::Value* R)
{
    // Operator overloads have the format: operator.<OperatorName>
    //                             like: operator.Add(string, string)
    //                             for:  "h" + "i"
    std::string operatorName = "operator." + tokenTypeAsString(token->tokenType);

    // Build argumentList from L and R types
    argumentList argList;
    std::string lTypeStr = getOperandTypeString(this, 0, L);
    uint8_t lPointerLevel = 0;
    std::string lBaseTypeStr = lTypeStr;
    while (lBaseTypeStr.length() > 0 && lBaseTypeStr[0] == '*') {
        lPointerLevel++;
        lBaseTypeStr = lBaseTypeStr.substr(1);
    }
    argList.push_back(AsaArgumentVariableValue(lBaseTypeStr, getASTNodeTypeFromString(lBaseTypeStr), lPointerLevel));

    std::string rTypeStr = getOperandTypeString(this, 1, R);
    uint8_t rPointerLevel = 0;
    std::string rBaseTypeStr = rTypeStr;
    while (rBaseTypeStr.length() > 0 && rBaseTypeStr[0] == '*') {
        rPointerLevel++;
        rBaseTypeStr = rBaseTypeStr.substr(1);
    }
    argList.push_back(AsaArgumentVariableValue(rBaseTypeStr, getASTNodeTypeFromString(rBaseTypeStr), rPointerLevel));

    //printFunctionPrototypes();

    AsaFunctionDefinition* calleeID = getFunctionFromID(AsaFunctionDefinitions, operatorName, argList, true);
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

// OPENCODE:
// Resolves and calls a user-defined operator overload function for a binary
// expression. Builds an argumentList from the operand types, looks up
// `operator.<op>` (e.g. `operator.+`), and emits the call. Returns the call's
// return value or nullptr if no overload exists.
llvm::Value* ASTNode::generateOperatorOverloadCall(llvm::Value* L, llvm::Value* R)
{
    messageSystem::startBlock(this, "Generating operator overload call", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    std::string operatorName = "operator." + tokenTypeAsString(token->tokenType);

    // Build argumentList from L and R types
    argumentList argList;
    std::string lTypeStr = getOperandTypeString(this, 0, L);
    uint8_t lPointerLevel = 0;
    std::string lBaseTypeStr = lTypeStr;
    while (lBaseTypeStr.length() > 0 && lBaseTypeStr[0] == '*') {
        lPointerLevel++;
        lBaseTypeStr = lBaseTypeStr.substr(1);
    }
    argList.push_back(AsaArgumentVariableValue(lBaseTypeStr, getASTNodeTypeFromString(lBaseTypeStr), lPointerLevel));

    std::string rTypeStr = getOperandTypeString(this, 1, R);
    uint8_t rPointerLevel = 0;
    std::string rBaseTypeStr = rTypeStr;
    while (rBaseTypeStr.length() > 0 && rBaseTypeStr[0] == '*') {
        rPointerLevel++;
        rBaseTypeStr = rBaseTypeStr.substr(1);
    }
    argList.push_back(AsaArgumentVariableValue(rBaseTypeStr, getASTNodeTypeFromString(rBaseTypeStr), rPointerLevel));

    AsaFunctionDefinition* calleeID = getFunctionFromID(AsaFunctionDefinitions, operatorName, argList, true);

    if (!calleeID || !calleeID->fnValue) {
        return (llvm::Value*)messageSystem::error("Expected operator overload for undefined operator `" + tokenTypeAsString(token->tokenType) + "`, but none were not found");
    }

    calleeID->uses++;

    // Implicit numeric coercion: cast each argument to the formal parameter type
    std::vector<llvm::Value*> ArgsV = {L, R};
    for (int i = 0; i < 2; i++) {
        int formalIdx = i + (calleeID->isStructReturn ? 1 : 0);
        if (formalIdx < (int)calleeID->fnValue->getFunctionType()->getNumParams()) {
            llvm::Type* formalType = calleeID->fnValue->getFunctionType()->getParamType(formalIdx);
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

    // Append tracked_caller hidden parameters if the callee requires them
    if (calleeID->isTrackedCaller && token && token->filePath) {
        llvm::Value* hiddenFilepath = llvmIRBuilder->CreateGlobalString(*token->filePath);
        llvm::Value* hiddenLineNum = ConstantInt::get(Type::getInt32Ty(*llvmCompileContext), token->lineNumber);
        llvm::Value* hiddenLineContent = llvmIRBuilder->CreateGlobalString(
            token->lineValue ? *token->lineValue : "");
        ArgsV.push_back(hiddenFilepath);
        ArgsV.push_back(hiddenLineNum);
        ArgsV.push_back(hiddenLineContent);
    }

    // If the operator returns a struct, we need to pass an sret pointer as the first arg
    if (calleeID->isStructReturn) {
        auto structIt = structDefinitions.find(calleeID->returnType);
        if (structIt == structDefinitions.end() || !structIt->second->structVal) {
            return (llvm::Value*)messageSystem::error("Struct return type not defined for operator overload");
        }
        AllocaInst* sretAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), structIt->second->structVal, "op_sret");
        ArgsV.insert(ArgsV.begin(), sretAlloc);
        llvmIRBuilder->CreateCall(calleeID->fnValue, ArgsV);

        messageSystem::endBlock();
        return llvmIRBuilder->CreateLoad(structIt->second->structVal, sretAlloc, "op_overload");
    }

    llvm::Value* callValue = llvmIRBuilder->CreateCall(calleeID->fnValue, ArgsV, "op_overload");
    if (calleeID->returnIsReference) {
        bool wasDefined = true;
        int resolvePass = 2;
        llvm::Type* refType = getLLVMTypeFromString(calleeID->returnType, 0, this, wasDefined, resolvePass);
        if (!refType || !wasDefined) {
            messageSystem::endBlock();
            return (llvm::Value*)messageSystem::error("Cannot resolve ref return type: " + calleeID->returnType);
        }
        if (!asaType)
            asaType = new AsaTypeInstance(refType);
        else
            asaType->baseLLVMType = refType;
        asaType->strVal = calleeID->returnType;
        asaType->isRef = true;
        messageSystem::endBlock();
        if (lvalue || isRef)
            return callValue;
        return llvmIRBuilder->CreateLoad(refType, callValue, "op_ref_load");
    }

    messageSystem::endBlock();
    return callValue;
}

// llvm::Value*
void* ASTNode::generateAccessOperation(int pass)
{
    messageSystem::startBlock(this, "Generating access operation", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    if (childNodes.size() == 0) {
        return messageSystem::error("Access operation requires a left and right argument");
    }

    childNodes[0]->lvalue = true;  // Set flag for base to return address if needed
    llvm::Value* L = (llvm::Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }
    llvm::Value* R = (llvm::Value*)(childNodes[1]->*(childNodes[1]->codegen))(pass);
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

    if (asaType->isRef && structDefinitions.count(asaType->strVal) && L && L->getType()->isPointerTy()) {
        L = llvmIRBuilder->CreateLoad(PointerType::getUnqual(*llvmCompileContext), L, "ref_struct_ptr");
        if (structDefinitions[asaType->strVal]->structVal)
            asaType->baseLLVMType = structDefinitions[asaType->strVal]->structVal;
    }

    // Get the element type from baseType (set by member access or previous operations)
    llvm::Type* elementType = asaType->baseLLVMType;
    std::string resolvedElementTypeStr;

    // If baseType is a pointer, we need to determine what it points to
    if (asaType->baseLLVMType && asaType->baseLLVMType->isPointerTy()) {
        std::string typeStr = asaType->strVal;
        // Strip exactly one leading '*' to get the element type string for this subscript.
        // getLLVMTypeFromString handles any further '*' prefixes in the element type string.
        std::string elementTypeStr = (!typeStr.empty() && typeStr[0] == '*') ? typeStr.substr(1) : typeStr;
        resolvedElementTypeStr = elementTypeStr;
        if (elementTypeStr == typeStr && !typeStr.empty() && structDefinitions.count(typeStr)) {
            return messageSystem::error("operator[] is not defined for type '" + typeStr + "'");
        }
        else if (!elementTypeStr.empty()) {
            bool wd = true;
            int resolvePass = 2;
            llvm::Type* resolved = getLLVMTypeFromString(elementTypeStr, 0, this, wd, resolvePass);
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
    // If baseType is a struct, check for operator[] overload
    else if (asaType->baseLLVMType && asaType->baseLLVMType->isStructTy()) {
        // Check for operator[] overload
        std::string opName = "operator." + tokenTypeAsString(Both_Brackets);
        argumentList opArgList;
        std::string lTypeStr = asaType->strVal;
        opArgList.push_back(AsaArgumentVariableValue(lTypeStr, getASTNodeTypeFromString(lTypeStr), 0));
        std::string rTypeStr = getOperandTypeString(this, 1, R);
        uint8_t rPointerLevel = 0;
        std::string rBaseTypeStr = rTypeStr;
        while (!rBaseTypeStr.empty() && rBaseTypeStr[0] == '*') {
            rPointerLevel++;
            rBaseTypeStr = rBaseTypeStr.substr(1);
        }
        opArgList.push_back(AsaArgumentVariableValue(rBaseTypeStr, getASTNodeTypeFromString(rBaseTypeStr), rPointerLevel));

        AsaFunctionDefinition* opCalleeID = getFunctionFromID(AsaFunctionDefinitions, opName, opArgList, true);
        if (opCalleeID && opCalleeID->fnValue) {
            opCalleeID->uses++;
            // Pass struct by value or by pointer depending on the formal parameter type
            int firstArgIdx = opCalleeID->isStructReturn ? 1 : 0;
            llvm::Value* lArg = L;
            if (firstArgIdx < (int)opCalleeID->fnValue->getFunctionType()->getNumParams()) {
                llvm::Type* formalType0 = opCalleeID->fnValue->getFunctionType()->getParamType(firstArgIdx);
                if (formalType0 && formalType0->isStructTy())
                    lArg = llvmIRBuilder->CreateLoad(asaType->baseLLVMType, L, "struct_load");
            }
            std::vector<llvm::Value*> ArgsV = {lArg, R};

            // Append tracked_caller hidden parameters if the callee requires them
            if (opCalleeID->isTrackedCaller && token && token->filePath) {
                llvm::Value* hiddenFilepath = llvmIRBuilder->CreateGlobalString(*token->filePath);
                llvm::Value* hiddenLineNum = ConstantInt::get(Type::getInt32Ty(*llvmCompileContext), token->lineNumber);
                llvm::Value* hiddenLineContent = llvmIRBuilder->CreateGlobalString(
                    token->lineValue ? *token->lineValue : "");
                ArgsV.push_back(hiddenFilepath);
                ArgsV.push_back(hiddenLineNum);
                ArgsV.push_back(hiddenLineContent);
            }

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
            llvm::Value* callValue = llvmIRBuilder->CreateCall(opCalleeID->fnValue, ArgsV, "idx_overload");
            if (opCalleeID->returnIsReference) {
                bool wasDefined = true;
                llvm::Type* refType = getLLVMTypeFromString(opCalleeID->returnType, 0, this, wasDefined, pass);
                if (!refType || !wasDefined)
                    return messageSystem::error("Cannot resolve ref return type: " + opCalleeID->returnType);
                if (!asaType)
                    asaType = new AsaTypeInstance(refType);
                else
                    asaType->baseLLVMType = refType;
                asaType->strVal = opCalleeID->returnType;
                asaType->isRef = true;
                lastRetrievedElementType.push(asaType);
                messageSystem::endBlock();
                if (lvalue || isRef)
                    return callValue;
                return llvmIRBuilder->CreateLoad(refType, callValue, "idx_ref_load");
            }
            messageSystem::endBlock();
            return callValue;
        }

        return messageSystem::error("operator[] is not defined for type '" + asaType->strVal + "'");
    }

    // Determine if we need to load the pointer first
    // L is a pointer-to-pointer when it comes from a struct member (alloca of pointer)
    // L is a direct pointer when it's a function parameter
    llvm::Value* actualPtr = L;
    llvm::Type* ptrType = PointerType::getUnqual(*llvmCompileContext);

    // Check if L is an alloca instruction or a pointer to a pointer
    // In that case, we need to load the actual pointer value
    if (AllocaInst* allocaInst = dyn_cast<AllocaInst>(L)) {
        actualPtr = llvmIRBuilder->CreateLoad(ptrType, L, "ptr_deref");
        if (asaType->isRef)
            actualPtr = llvmIRBuilder->CreateLoad(ptrType, actualPtr, "ref_ptr_deref");
    }
    else if (GlobalVariable* gv = dyn_cast<GlobalVariable>(L)) {
        // Global pointer variables need a load to get the heap pointer
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
    llvm::Value* gep = llvmIRBuilder->CreateGEP(elementType, actualPtr, R, "arrayidx");

    // If this is an lvalue (for assignment), push element type so compound assignment can load it
    if (lvalue) {
        asaType = new AsaTypeInstance(elementType);
        asaType->strVal = resolvedElementTypeStr;
        lastRetrievedElementType.push(asaType);

        messageSystem::endBlock();
        return gep;
    }

    // Propagate element type string for rvalue uses (e.g. member access on subscript result)
    if (!asaType)
        asaType = new AsaTypeInstance(elementType);
    else
        asaType->baseLLVMType = elementType;
    asaType->strVal = resolvedElementTypeStr;

    messageSystem::endBlock();

    // If rvalue, load and return the value
    return llvmIRBuilder->CreateLoad(elementType, gep, "accessop_load");
}

// llvm::Value*
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
            ASTNode* rightNode = childNodes[1];
            std::string memberName = rightNode->token->tokenStr;
            auto modIt = moduleRegistry.find(modName);
            if (modIt != moduleRegistry.end()) {
                ASTNode* modNode = modIt->second;
                if (rightNode->nodeType == Function_Call) {
                    std::string originalName = rightNode->token->tokenStr;
                    rightNode->token->tokenStr = modName + "." + originalName;
                    llvm::Value* callValue = (llvm::Value*)(rightNode->*(rightNode->codegen))(pass);
                    rightNode->token->tokenStr = originalName;
                    if (rightNode->asaType) {
                        //asaType = new AsaTypeInstance(rightNode->asaType->baseLLVMType, rightNode->asaType->isRef, rightNode->asaType->isConst, rightNode->asaType->strVal, rightNode->asaType->pointerLevel);
                        asaType = rightNode->asaType;
                        lastRetrievedElementType.push(asaType);
                    }
                    messageSystem::endBlock();
                    return callValue;
                }
                auto varIt = modNode->namedValues.find(memberName);
                if (varIt != modNode->namedValues.end()) {
                    AsaVariableValue* vt = varIt->second;
                    if (!lvalue)
                        markVariableRead(vt);
                    llvm::Value* gv = vt->llvmValue;
                    llvm::Type* gvType = getTypePtrFromLLVMValue(gv);
                    asaType = new AsaTypeInstance(gvType);
                    asaType->strVal = vt->asaTypeInstance->strVal;
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
                    llvm::Value* defVal = (llvm::Value*)(defineNode->*(defineNode->codegen))(pass);
                    if (wasError) {
                        messageSystem::endBlock();
                        return nullptr;
                    }
                    if (defVal) {
                        asaType = new AsaTypeInstance(defVal->getType());
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
    std::string operatorOverloadName = "operator." + tokenTypeAsString(Dot);
    argumentList argList = argumentList();

    size_t stackDepthBefore = lastRetrievedElementType.size();
    llvm::Value* L = (llvm::Value*)(childNodes[0]->*(childNodes[0]->codegen))(pass);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }

    // Evaluate base pointer
    llvm::Value* basePtr = L;
    if (!basePtr || !basePtr->getType()->isPointerTy()) {
        // A member function returning a struct by value gives us a raw struct Value, not a pointer.
        // Store it into a temporary alloca so we have an addressable pointer for GEP/member calls.
        if (basePtr && basePtr->getType()->isStructTy()) {
            StructType* sty = cast<StructType>(basePtr->getType());
            AllocaInst* tmp = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), sty, "struct_tmp");
            llvmIRBuilder->CreateStore(basePtr, tmp);
            basePtr = tmp;
            // Push the LLVM struct type so the else branch below can look up the struct definition.
            lastRetrievedElementType.push(new AsaTypeInstance(sty, false, false, "", 0));
        }
        else {
            return messageSystem::error("Base must be a pointer for access");
        }
    }

    if (childNodes[0]->nodeType == Identifier_Node) {
        AsaVariableValue* v = findNamedValue(this, nullptr, childNodes[0]->token->tokenStr, token);

        if (!v && !wasError) {
            return messageSystem::error("Unknown variable name used");
        }
        markVariableRead(v);

        // Resolve through pointer indirection if needed
        std::string AsaStructDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionName = v->asaTypeInstance->strVal;
        {
            int ptrDepth = 0;
            while (!AsaStructDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionName.empty() && AsaStructDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionName[0] == '*') {
                AsaStructDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionName = AsaStructDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionName.substr(1);
                ptrDepth++;
            }
            if (ptrDepth > 0 && structDefinitions.count(resolveTypeAlias(AsaStructDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionName))) {
                for (int i = 0; i < ptrDepth; i++)
                    basePtr = llvmIRBuilder->CreateLoad(PointerType::getUnqual(*llvmCompileContext), basePtr, "ptr_deref");
            }
            else {
                AsaStructDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionName = v->asaTypeInstance->strVal;
            }
            AsaStructDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionName = resolveTypeAlias(AsaStructDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionDefinitionName);
        }

        if (structDefinitions.find(AsaStructDefinitionName) == structDefinitions.end()) {
            return messageSystem::error("Type \"" + v->asaTypeInstance->strVal + "\" has not been defined");
        }
        AsaStructDefinitionDefinition* structDefinition = structDefinitions[AsaStructDefinitionDefinitionName];

        // If the struct body hasn't been generated yet, generate it
        if (structDefinition->structVal == nullptr)
            llvm::Value* argVal = (llvm::Value*)(structDefinition->sourceNode->*(structDefinition->sourceNode->codegen))(pass);
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

                return messageSystem::error("Struct definition \"" + AsaStructDefinitionName + "\" does not contain member \"" + memberName + "\"", messageSystem::Undefined_Member_Error);

                //printTokenError(getASTTokenRange(childNodes[1]), "Struct definition \"" + AsaStructDefinitionName + "\" does not contain member \"" + memberName + "\"");
            }


            uint16_t memberIndex = structDefinition->memberNameIndexes[memberName];

            bool wasDefined = true;
            llvm::Type* elementType = getLLVMTypeFromString(structDefinition->members[memberIndex].typeString, 0, this, wasDefined, pass);
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
                // Allow indexing through a const pointer member but block
                // direct reassignment of the pointer itself.  When used as a
                // subscript base, the parent node is Access_Operation.
                bool isSubscriptBase = parentNode && parentNode->nodeType == Access_Operation;
                if (!isSubscriptBase) {
                    // Allow const member writes inside create constructors and member
                    // functions of this struct when writing to a local variable (not `this`).
                    bool allowed = false;
                    if (v && v->llvmValue && isa<AllocaInst>(v->llvmValue)) {
                        llvm::Value* currentFn = llvmIRBuilder->GetInsertBlock()->getParent();
                        for (auto* fid : AsaFunctionDefinitions) {
                            if (fid->fnValue == currentFn) {
                                std::string fnName = fid->name;
                                std::string prefix = AsaStructDefinitionDefinitionName + ".";
                                if (fnName == AsaStructDefinitionDefinitionName ||
                                    fnName.substr(0, prefix.size()) == prefix)
                                    allowed = true;
                                break;
                            }
                        }
                    }
                    if (!allowed)
                        return messageSystem::error("Cannot modify const member '" + memberName + "'");
                }
            }


            //// Create GEP to compute the address
            //llvm::Value* gep = llvmIRBuilder->CreateGEP(elementType, basePtr, index, "arrayidx");

            if (v->isReference)
                basePtr = llvmIRBuilder->CreateLoad(PointerType::getUnqual(*llvmCompileContext), basePtr, "ref_struct_ptr");
            auto gep = llvmIRBuilder->CreateStructGEP(structDefinition->structVal, basePtr, memberIndex, "struct_member");
            std::string memberStrVal = "";
            for (int _p = 0; _p < structDefinition->members[memberIndex].pointerLevel; ++_p)
                memberStrVal += "*";
            memberStrVal += structDefinition->members[memberIndex].typeString;
            asaType = new AsaTypeInstance(elementType, false, structDefinition->members[memberIndex].isConstant, memberStrVal, (uint8_t)structDefinition->members[memberIndex].pointerLevel);
            lastRetrievedElementType.push(asaType);

            messageSystem::endBlock();
            if (lvalue)
                return gep;
            // If rvalue, return value
            else {
                return llvmIRBuilder->CreateLoad(elementType, gep, "member_load");
            }
        }
        // Handle if member function call
        else if (childNodes[1]->nodeType == Function_Call) {
            std::string userMemberName = memberName;
            memberName = structDefinition->name + "." + memberName;

            ASTNode* argsNode = childNodes[1]->childNodes[0];
            std::vector<ASTNode*> args = std::vector<ASTNode*>();
            for (auto& a : argsNode->childNodes)
                if (a->childNodes.size() > 0) {
                    args.push_back(a);
                }

            std::vector<llvm::Value*> ArgsV = std::vector<llvm::Value*>();
            argumentList argList = argumentList();
            // 'this' goes in ArgsV (LLVM call) but NOT in argList (lookup uses userArguments which excludes 'this')
            // If the base is a reference (e.g. 'this' in a member function), dereference it
            // to get the actual struct pointer before passing as 'this'.
            if (v && v->isReference)
                basePtr = llvmIRBuilder->CreateLoad(PointerType::getUnqual(*llvmCompileContext), basePtr, "ref_struct_ptr");
            ArgsV.push_back(basePtr);
            if (!ArgsV.back()) {
                messageSystem::endBlock();
                return nullptr;
            }
            // Add rest of argument values
            for (int i = 0; i < args.size(); i++) {
                llvm::Value* argVal = (llvm::Value*)(args[i]->*(args[i]->codegen))(pass);
                if (wasError) {
                    messageSystem::endBlock();
                    return nullptr;
                }
                ArgsV.push_back(argVal);
                argList.push_back(AsaArgumentVariableValue(getStringTypeFromLLVMType(argVal->getType()), getASTNodeTypeFromString(getStringTypeFromLLVMType(argVal->getType())), 0));
                if (!ArgsV.back()) {
                    messageSystem::endBlock();
                    return nullptr;
                }
            }

            // Look up the id in the struct function (against userArguments, which excludes 'this').
            AsaFunctionDefinition* CalleeFID = getFunctionFromID(structDefinition->memberFunctions, memberName, argList, true, true);
            if (!CalleeFID) {
                for (auto& f : structDefinition->memberFunctions) {
                    messageSystem::addAttribute(f);
                }

                return messageSystem::error("Struct definition does not contain member function \"" + userMemberName + "\"", messageSystem::Undefined_Member_Function_Error);
            }

            // Call function
            Function* CalleeF = CalleeFID->fnValue;
            CalleeFID->uses++;
            isCallMemberFunction = true;

            // Handle sret for struct-returning member functions
            bool memberIsStructReturn = CalleeFID->isStructReturn;
            AllocaInst* memberSretAlloc = nullptr;
            if (memberIsStructReturn) {
                AsaStructDefinitionDefinition* retStruct = structDefinitions[CalleeFID->returnType];
                if (!retStruct || retStruct->structVal == nullptr) {
                    return messageSystem::error("Struct return type not fully defined");
                }
                memberSretAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), retStruct->structVal, "member_sret");
                ArgsV.insert(ArgsV.begin(), memberSretAlloc);
            }

            // If argument mismatch error. ArgsV has [sret?] + 'this' + user args = full LLVM arg count.
            {
                size_t expectedArgCount = ArgsV.size();
                if (CalleeFID->isTrackedCaller)
                    expectedArgCount += 3;
                if (CalleeFID->variableNumArguments == false)
                    if (CalleeF->arg_size() != expectedArgCount) {
                        return messageSystem::error("Incorrect number of arguments passed to function");
                    }
                    else if (CalleeF->arg_size() > expectedArgCount) {
                        return messageSystem::error("Incorrect number of arguments passed to function");
                    }
            }

            // Rebuild ArgsV for the actual call
            ArgsV = std::vector<llvm::Value*>();
            ArgsV.push_back(basePtr);
            if (!ArgsV.back()) {
                messageSystem::endBlock();
                return nullptr;
            }
            // Add rest of argument values
            for (int i = 0; i < args.size(); i++) {
                if (CalleeFID->userArguments[i].isReference) {
                    const AsaArgumentVariableValue& fa = CalleeFID->userArguments[i];
                    if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
                        if (!fa.isConstant) {
                            messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                            return messageSystem::error("Cannot pass value as reference");
                        }
                        // const ref: auto-materialize the rvalue into a temporary alloca
                        messageSystem::startBlock(args[i], "Generating const ref argument (auto-materialize)", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                        llvm::Value* tmpVal = (llvm::Value*)(args[i]->*(args[i]->codegen))(pass);
                        if (wasError) {
                            messageSystem::endBlock();
                            return nullptr;
                        }
                        if (!tmpVal) {
                            messageSystem::endBlock();
                            return nullptr;
                        }
                        bool wasDef = true;
                        llvm::Type* formalType = getLLVMTypeFromString(fa.typeString, fa.pointerLevel, args[i], wasDef, pass);
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
                        AsaVariableValue* argVar = findNamedValue(parentNode, this, identNode->token->tokenStr, token);
                        if (argVar) {
                            llvm::Type* actualType = getTypePtrFromLLVMValue(argVar->llvmValue);
                            bool wasDef = true;
                            llvm::Type* formalType = getLLVMTypeFromString(fa.typeString, 0, args[i], wasDef, pass);
                            if (formalType && actualType && !actualType->isPointerTy() && !formalType->isPointerTy() && actualType != formalType) {
                                messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                                return messageSystem::error("Cannot pass '" + getStringTypeFromLLVMType(actualType) +
                                                            "' as 'ref " + fa.typeString + "': implicit cast to reference is not allowed");
                            }
                        }
                    }
                    args[i]->childNodes[0]->isRef = true;
                }
                llvm::Value* argVal = (llvm::Value*)(args[i]->*(args[i]->codegen))(pass);
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
            lastRetrievedElementType.push(new AsaTypeInstance(getLLVMTypeFromString(CalleeFID->returnType, 0, this, wasDefined, pass)));

            isCallMemberFunction = false;
            asaType = lastRetrievedElementType.top();
            if (CalleeFID->returnIsReference)
                asaType->isRef = true;

            // Append tracked_caller hidden parameters if the callee requires them
            if (CalleeFID->isTrackedCaller && token && token->filePath) {
                llvm::Value* hiddenFilepath = llvmIRBuilder->CreateGlobalString(*token->filePath);
                llvm::Value* hiddenLineNum = ConstantInt::get(Type::getInt32Ty(*llvmCompileContext), token->lineNumber);
                llvm::Value* hiddenLineContent = llvmIRBuilder->CreateGlobalString(
                    token->lineValue ? *token->lineValue : "");
                ArgsV.push_back(hiddenFilepath);
                ArgsV.push_back(hiddenLineNum);
                ArgsV.push_back(hiddenLineContent);
            }

            llvm::Value* callResult = nullptr;
            llvm::Type* retType = CalleeF->getReturnType();

            // Create the call
            if (retType->isVoidTy())
                llvmIRBuilder->CreateCall(CalleeF, ArgsV);
            else
                callResult = llvmIRBuilder->CreateCall(CalleeF, ArgsV, "calltmp");

            messageSystem::endBlock();
            if (memberIsStructReturn) {
                AsaStructDefinitionDefinition* retStruct = structDefinitions[CalleeFID->returnType];
                return llvmIRBuilder->CreateLoad(retStruct->structVal, memberSretAlloc, "member_sret_load");
            }
            if (CalleeFID->returnIsReference) {
                if (lvalue || isRef)
                    return callResult;
                bool wasDef = true;
                llvm::Type* refType = getLLVMTypeFromString(CalleeFID->returnType, 0, this, wasDef, pass);
                return llvmIRBuilder->CreateLoad(refType, callResult, "ref_load");
            }
            return callResult;
        }
    }
    // If left is not pointer, assume another member access or index operator
    else {
        AsaStructDefinitionDefinitionDefinition* structDefinition = nullptr;

        if (lastRetrievedElementType.size() > stackDepthBefore) {
            // Left child pushed a type (e.g. chained member access) -- use the most recent one
            AsaTypeInstance* poppedType = lastRetrievedElementType.top();
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
            llvm::Type* elementType = getLLVMTypeFromString(structDefinition->members[memberIndex].typeString, 0, this, wasDefined, pass);
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
            //llvm::Value* gep = llvmIRBuilder->CreateGEP(elementType, basePtr, index, "arrayidx");

            auto gep = llvmIRBuilder->CreateStructGEP(structDefinition->structVal, basePtr, memberIndex, "struct_member");
            std::string memberStrVal = "";
            for (int _p = 0; _p < structDefinition->members[memberIndex].pointerLevel; ++_p)
                memberStrVal += "*";
            memberStrVal += structDefinition->members[memberIndex].typeString;
            asaType = new AsaTypeInstance(elementType, false, structDefinition->members[memberIndex].isConstant, memberStrVal, (uint8_t)structDefinition->members[memberIndex].pointerLevel);

            lastRetrievedElementType.push(asaType);

            messageSystem::endBlock();

            // If this is an lvalue (for assignment), return the pointer gep
            if (lvalue)
                return gep;
            // If rvalue, return value
            else {
                return llvmIRBuilder->CreateLoad(elementType, gep, "member_load");
            }
        }
        // Handle if member function call
        else if (childNodes[1]->nodeType == Function_Call) {
            std::string userMemberName = memberName;
            memberName = structDefinition->name + "." + memberName;

            ASTNode* argsNode = childNodes[1]->childNodes[0];
            std::vector<ASTNode*> args = std::vector<ASTNode*>();
            for (auto& a : argsNode->childNodes)
                if (a->childNodes.size() > 0) {
                    args.push_back(a);
                }

            std::vector<llvm::Value*> ArgsV = std::vector<llvm::Value*>();
            argumentList argList = argumentList();
            // 'this' goes in ArgsV (LLVM call) but NOT in argList (lookup uses userArguments which excludes 'this')
            ArgsV.push_back(basePtr);
            if (!ArgsV.back()) {
                messageSystem::endBlock();
                return nullptr;
            }
            // Add rest of argument values
            for (int i = 0; i < args.size(); i++) {
                llvm::Value* argVal = (llvm::Value*)(args[i]->*(args[i]->codegen))(pass);
                if (wasError) {
                    messageSystem::endBlock();
                    return nullptr;
                }
                ArgsV.push_back(argVal);
                argList.push_back(AsaArgumentVariableValue(getStringTypeFromLLVMType(argVal->getType()), getASTNodeTypeFromString(getStringTypeFromLLVMType(argVal->getType())), 0));
                if (!ArgsV.back()) {
                    messageSystem::endBlock();
                    return nullptr;
                }
            }

            // Look up the id in the struct function (against userArguments, which excludes 'this').
            AsaFunctionDefinition* CalleeFID = getFunctionFromID(structDefinition->memberFunctions, memberName, argList, true, true);
            if (!CalleeFID) {
                messageSystem::startBlock(childNodes[1], "Generating struct function call", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                for (auto& f : structDefinition->memberFunctions) {
                    messageSystem::addAttribute(f);
                }
                return messageSystem::error("Struct definition does not contain member function \"" + userMemberName + "\"", messageSystem::Undefined_Member_Function_Error);
            }

            // Call function
            Function* CalleeF = CalleeFID->fnValue;
            CalleeFID->uses++;
            isCallMemberFunction = true;

            // Handle sret for struct-returning member functions
            bool memberIsStructReturn = CalleeFID->isStructReturn;
            AllocaInst* memberSretAlloc = nullptr;
            if (memberIsStructReturn) {
                AsaStructDefinitionDefinitionDefinition* retStruct = structDefinitions[CalleeFID->returnType];
                if (!retStruct || retStruct->structVal == nullptr) {
                    return messageSystem::error("Struct return type not fully defined");
                }
                memberSretAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), retStruct->structVal, "member_sret");
                ArgsV.insert(ArgsV.begin(), memberSretAlloc);
            }

            // If argument mismatch error. ArgsV has [sret?] + 'this' + user args = full LLVM arg count.
            {
                size_t expectedArgCount = ArgsV.size();
                if (CalleeFID->isTrackedCaller)
                    expectedArgCount += 3;
                if (CalleeFID->variableNumArguments == false)
                    if (CalleeF->arg_size() != expectedArgCount) {
                        return messageSystem::error("Incorrect number of arguments passed to function");
                    }
                    else if (CalleeF->arg_size() > expectedArgCount) {
                        return messageSystem::error("Incorrect number of arguments passed to function");
                    }
            }

            // Rebuild ArgsV for the actual call
            ArgsV = std::vector<llvm::Value*>();
            ArgsV.push_back(basePtr);
            if (!ArgsV.back()) {
                messageSystem::endBlock();
                return nullptr;
            }
            // Add rest of argument values
            for (int i = 0; i < args.size(); i++) {
                if (CalleeFID->userArguments[i].isReference) {
                    const AsaArgumentVariableValue& fa = CalleeFID->userArguments[i];
                    if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
                        if (!fa.isConstant) {
                            messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                            return messageSystem::error("Cannot pass value as reference");
                        }
                        // const ref: auto-materialize the rvalue into a temporary alloca
                        messageSystem::startBlock(args[i], "Generating const ref argument (auto-materialize)", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                        llvm::Value* tmpVal = (llvm::Value*)(args[i]->*(args[i]->codegen))(pass);
                        if (wasError) {
                            messageSystem::endBlock();
                            return nullptr;
                        }
                        if (!tmpVal) {
                            messageSystem::endBlock();
                            return nullptr;
                        }
                        bool wasDef = true;
                        llvm::Type* formalType = getLLVMTypeFromString(fa.typeString, fa.pointerLevel, args[i], wasDef, pass);
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
                        AsaVariableValue* argVar = findNamedValue(parentNode, this, identNode->token->tokenStr, token);
                        if (argVar) {
                            llvm::Type* actualType = getTypePtrFromLLVMValue(argVar->llvmValue);
                            bool wasDef = true;
                            llvm::Type* formalType = getLLVMTypeFromString(fa.typeString, 0, args[i], wasDef, pass);
                            if (formalType && actualType && !actualType->isPointerTy() && !formalType->isPointerTy() && actualType != formalType) {
                                messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                                return messageSystem::error("Cannot pass '" + getStringTypeFromLLVMType(actualType) +
                                                            "' as 'ref " + fa.typeString + "': implicit cast to reference is not allowed");
                            }
                        }
                    }
                    args[i]->childNodes[0]->isRef = true;
                }
                llvm::Value* argVal = (llvm::Value*)(args[i]->*(args[i]->codegen))(pass);
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

            // Append tracked_caller hidden parameters if the callee requires them
            if (CalleeFID->isTrackedCaller && token && token->filePath) {
                llvm::Value* hiddenFilepath = llvmIRBuilder->CreateGlobalString(*token->filePath);
                llvm::Value* hiddenLineNum = ConstantInt::get(Type::getInt32Ty(*llvmCompileContext), token->lineNumber);
                llvm::Value* hiddenLineContent = llvmIRBuilder->CreateGlobalString(
                    token->lineValue ? *token->lineValue : "");
                ArgsV.push_back(hiddenFilepath);
                ArgsV.push_back(hiddenLineNum);
                ArgsV.push_back(hiddenLineContent);
            }

            bool wasDefined = true;
            lastRetrievedElementType.push(new AsaTypeInstance(getLLVMTypeFromString(CalleeFID->returnType, 0, this, wasDefined, pass)));

            isCallMemberFunction = false;
            asaType = lastRetrievedElementType.top();
            if (CalleeFID->returnIsReference)
                asaType->isRef = true;

            llvm::Value* callResult = nullptr;
            llvm::Type* retType = CalleeF->getReturnType();

            // Create the call
            if (retType->isVoidTy())
                llvmIRBuilder->CreateCall(CalleeF, ArgsV);
            else
                callResult = llvmIRBuilder->CreateCall(CalleeF, ArgsV, "calltmp");


            messageSystem::endBlock();

            if (memberIsStructReturn) {
                AsaStructDefinitionDefinitionDefinition* retStruct = structDefinitions[CalleeFID->returnType];
                return llvmIRBuilder->CreateLoad(retStruct->structVal, memberSretAlloc, "member_sret_load");
            }
            if (CalleeFID->returnIsReference) {
                if (lvalue || isRef)
                    return callResult;
                bool wasDef = true;
                llvm::Type* refType = getLLVMTypeFromString(CalleeFID->returnType, 0, this, wasDef, pass);
                return llvmIRBuilder->CreateLoad(refType, callResult, "ref_load");
            }
            return callResult;

            //return llvmIRBuilder->CreateCall(CalleeF, ArgsV, "calltmp");
        }
    }


    messageSystem::endBlock();
    return nullptr;
}


// llvm::Value*
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
            (void)(llvm::Value*)(c->*(c->codegen))(pass);
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

        llvm::Value* resultVal = llvmIRBuilder->CreateLoad(
            ctx.resultSlot->getAllocatedType(), ctx.resultSlot, "result_val");
        messageSystem::endBlock();
        return resultVal;
    }

    messageSystem::endBlock();
    return nullptr;
}

// llvm::Value*
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

    AsaVariableValue* val = findNamedValue(parentNode, this, varName, token);
    if (!val && !wasError) {
        return messageSystem::error("Unknown variable name used");
    }
    markVariableRead(val);
    llvm::Value* var = val->llvmValue;
    llvm::Value* value = llvmIRBuilder->CreateLoad(getTypePtrFromLLVMValue(var), var, varName + "_load");
    bool wasDefined = true;
    llvm::Type* toType = getLLVMTypeFromString(tyVal, 0, typeNode, wasDefined, pass);
    // Determine source signedness from the variable's declared type
    std::string srcTypeStr = val->asaTypeInstance->strVal;
    while (!srcTypeStr.empty() && srcTypeStr[0] == '*')
        srcTypeStr = srcTypeStr.substr(1);
    bool isSrcSigned = typeSigns.count(srcTypeStr) ? typeSigns[srcTypeStr] : true;
    llvm::Value* casted = castValue(value, toType, isSrcSigned, typeSigns[tyVal], this);

    messageSystem::endBlock();

    if (wasError)
        return nullptr;
    return casted;
}

// llvm::Value*
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

// OPENCODE:
// Codegen for `#bitcast(var, type)`: loads the variable, then emits a raw
// bitcast/ptrtoint/inttoptr to reinterpret it as the target type. No safety
// checks — the caller is responsible for validity.
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

    AsaVariableValue* val = findNamedValue(parentNode, this, varName, token);
    if (!val && !wasError) {
        return messageSystem::error("Unknown variable name used");
    }
    markVariableRead(val);
    llvm::Value* var = val->llvmValue;
    llvm::Value* value = llvmIRBuilder->CreateLoad(getTypePtrFromLLVMValue(var), var, varName + "_load");

    bool wasDefined = true;
    llvm::Type* toType = getLLVMTypeFromString(tyVal, 0, typeNode, wasDefined, pass);
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

// llvm::Value*
void* ASTNode::generateStructInstance(int pass)
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

    AsaStructDefinition* typeVal = structDefinitions[typeName];

    // TODO: This should use the dependency generator system:
    // If the struct body hasn't been generated yet, generate it
    if (typeVal->structVal == nullptr)
        llvm::Value* argVal = (llvm::Value*)(typeVal->sourceNode->*(typeVal->sourceNode->codegen))(pass);
    if (wasError) {
        messageSystem::endBlock();
        return nullptr;
    }

    AllocaInst* var = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), typeVal->structVal, "struct_alloc");

    llvm::Value* structSize = ConstantInt::get(llvm::Type::getInt64Ty(*llvmCompileContext),
        llvmCompileModule->getDataLayout().getTypeAllocSize(typeVal->structVal));

    // Create memset call to zero the memory
    Function* memsetFunc = Intrinsic::getOrInsertDeclaration(llvmCompileModule.get(), Intrinsic::memset, {var->getType(), llvm::Type::getInt64Ty(*llvmCompileContext)});

    llvmIRBuilder->CreateCall(memsetFunc, {
                                              var,                                                              // dest
                                              ConstantInt::get(llvm::Type::getInt8Ty(*llvmCompileContext), 0),  // value (zero)
                                              structSize,                                                       // size
                                              ConstantInt::get(llvm::Type::getInt1Ty(*llvmCompileContext), 0)   // is_volatile
                                          });

    // Apply non-zero default values for members that declare them
    for (auto& [memberName, defaultNode] : typeVal->memberDefaultNodes) {
        auto idxIt = typeVal->memberNameIndexes.find(memberName);
        if (idxIt == typeVal->memberNameIndexes.end())
            continue;
        uint16_t idx = idxIt->second;
        llvm::Value* memberPtr = llvmIRBuilder->CreateStructGEP(typeVal->structVal, var, idx, memberName + "_init");
        llvm::Value* defaultVal = nullptr;
        ASTNode* unwrappedDefault = unwrapSingleExpressionNode(defaultNode);
        if (unwrappedDefault && unwrappedDefault->nodeType == Undefined_Initializer_Node)
            defaultVal = UndefValue::get(typeVal->structVal->getElementType(idx));
        else if (unwrappedDefault && unwrappedDefault->nodeType == Default_Initializer_Node)
            defaultVal = generateDefaultValueForType(typeVal->structVal->getElementType(idx), typeVal->members[idx].typeString, typeVal->members[idx].pointerLevel, pass, this);
        else
            defaultVal = (llvm::Value*)(defaultNode->*(defaultNode->codegen))(pass);
        if (wasError)
            return nullptr;
        if (!defaultVal)
            continue;
        llvm::Type* memberType = typeVal->structVal->getElementType(idx);
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

// llvm::Value*
// Deep-copy an ASTNode for template instantiation.
// Does NOT copy parentNode (caller sets it after the call).
// Resets currentNodeDoneGenerating so the copy generates fresh.
static ASTNode* deepCopyASTNodeImpl(ASTNode* src, std::map<ASTNode*, ASTNode*>& visited)
{
    if (!src)
        return nullptr;
    auto it = visited.find(src);
    if (it != visited.end())
        return it->second;
    ASTNode* copy = new ASTNode();
    visited[src] = copy;
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
        copy->childNodes.push_back(deepCopyASTNodeImpl(child, visited));
    for (auto* leaf : src->leafNodes)
        copy->leafNodes.push_back(deepCopyASTNodeImpl(leaf, visited));
    for (auto* attr : src->attributes)
        copy->attributes.push_back(deepCopyASTNodeImpl(attr, visited));
    for (auto& [k, v] : src->compilerDefinitions)
        copy->compilerDefinitions[k] = deepCopyASTNodeImpl(v, visited);
    return copy;
}

// OPENCODE:
// Convenience wrapper around deepCopyASTNodeImpl: creates a fresh visited map
// and deep-copies the entire AST subtree. Used by variant instantiation to
// clone template AST nodes before substituting type parameters.
static ASTNode* deepCopyASTNode(ASTNode* src)
{
    std::map<ASTNode*, ASTNode*> visited;
    return deepCopyASTNodeImpl(src, visited);
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

// OPENCODE:
// Finds the first significant value node (Identifier, Integer, Float, Bool,
// String, Char) in a variant param subtree by searching leafNodes then
// childNodes. Used by variant instantiation to extract the concrete value
// from a variant type argument.
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
    ASTNode* defVariantsNode = (tmpl->childNodes.size() > 1) ? tmpl->childNodes[1] : nullptr;

    // If substitutions were not pre-built, derive them by splitting concreteSuffix on '.'
    // and matching positionally against the template's Variants_Node param names.
    if (substitutions.empty()) {
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

        if (!validateVariantArgumentCount(tmpl, defVariantsNode, (int)concreteTypes.size(), baseName))
            return;

        int nv = (int)concreteTypes.size();
        for (int vi = 0; vi < nv; vi++) {
            std::string paramName = variantParamFirstIdent(defVariantsNode->childNodes[vi]);
            if (!paramName.empty())
                substitutions.push_back({paramName, concreteTypes[vi]});
        }
        if (substitutions.empty())
            return;
    }
    else if (!validateVariantArgumentCount(tmpl, defVariantsNode, (int)substitutions.size(), baseName)) {
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

// OPENCODE:
// The main function-call codegen: evaluates arguments, performs overload
// resolution via getFunctionFromID, handles struct-return (sret) ABI, ref
// parameters, member-function dispatch, default arguments, variant function
// instantiation, extern ABI coercion, and pipe-call sugar. Emits the LLVM
// call instruction and returns the result value.
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

    std::vector<llvm::Value*> ArgsV = std::vector<llvm::Value*>();
    std::vector<llvm::Value*> cachedArgVals = std::vector<llvm::Value*>();
    argumentList argList = argumentList();

    // Build argList and ArgsV WITHOUT sret initially
    for (int i = 0; i < args.size(); i++) {
        if (!args[i]->codegen)
            return messageSystem::error("Node `" + ASTNodeTypeAsString(args[i]->nodeType) + "` does not have a code generator");

        llvm::Value* argVal = (llvm::Value*)(args[i]->*(args[i]->codegen))(pass);
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
            AsaVariableValue* val = findNamedValue(parentNode, this, identifierNode->token->tokenStr, token);
            if (val) {
                typeStr = val->asaTypeInstance->strVal;
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
                    AsaVariableValue* val = findNamedValue(parentNode, this, inner->token->tokenStr, token);
                    if (val && !val->asaTypeInstance->strVal.empty())
                        typeStr = "*" + val->asaTypeInstance->strVal;
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
            // Look up callee return type from AsaFunctionDefinitions to avoid *unknown for pointer returns
            std::string calleeName = identifierNode->token->tokenStr;
            for (auto* fid : AsaFunctionDefinitions) {
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

        argList.push_back(AsaArgumentVariableValue(baseTypeStr, getASTNodeTypeFromString(baseTypeStr), pointerLevel));

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
            if (!validateVariantArgumentCount(callVariantsNode, defVariantsNode, (int)callVariantsNode->childNodes.size(), resolvedFnName)) {
                messageSystem::endBlock();
                return nullptr;
            }
            int nv = (int)callVariantsNode->childNodes.size();
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
                // but the fnName stored in AsaFunctionDefinitions remains mangledFnName for lookup.
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
                    if (!validateVariantArgumentCount(callVariantsNode, defVariantsNode, (int)callVariantsNode->childNodes.size(), resolvedFnName)) {
                        messageSystem::endBlock();
                        return nullptr;
                    }
                    int nv = (int)callVariantsNode->childNodes.size();
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

    // Try alias-resolved name first, then fall back to the original token name
    // (cast functions like `char(x)` use the original name, not the alias target)
    AsaFunctionDefinition* CalleeFID = nullptr;
    if (resolvedFnName != token->tokenStr)
        CalleeFID = getFunctionFromID(AsaFunctionDefinitions, resolvedFnName, argList, true, shouldBeMemberFunction, false);
    if (!CalleeFID)
        CalleeFID = getFunctionFromID(AsaFunctionDefinitions, token->tokenStr, argList, true, shouldBeMemberFunction, true);
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
            llvm::Value* defVal = nullptr;
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
                defVal = (llvm::Value*)(tempNode->*(tempNode->codegen))(pass);
            }
            else {
                defNode->parentNode = parentNode;
                defVal = (llvm::Value*)(defNode->*(defNode->codegen))(pass);
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
            argList.push_back(AsaArgumentVariableValue(typeStr, getASTNodeTypeFromString(typeStr), pL));
        }
    }

    bool isStructReturn = CalleeFID->isStructReturn;
    AllocaInst* sretAlloc = nullptr;
    if (isStructReturn) {
        // Get the struct type from definitions
        AsaStructDefinitionDefinition* retStruct = structDefinitions[CalleeFID->returnType];
        if (retStruct->structVal == nullptr) {
            return messageSystem::error("Struct return type not fully defined");
        }

        // Allocate space for the returned struct on the caller's stack
        sretAlloc = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), retStruct->structVal, "sret_alloc");

        // Insert the sret pointer as the FIRST argument in ArgsV
        ArgsV.insert(ArgsV.begin(), sretAlloc);

        // For argList matching: Temporarily add sret to argList for validation
        // (This matches how it's stored in AsaFunctionDefinition)
        AsaArgumentVariableValue sretArg("*" + CalleeFID->returnType, Struct_Type, 1, false, true);
        argList.insert(argList.begin(), sretArg);
    }

    // Validate argument count (including sret if applicable)
    // For coerced functions, the LLVM arg count is larger than the user arg count
    if (CalleeFID->variableNumArguments == false) {
        size_t expectedIRArgCount = ArgsV.size();
        if (CalleeFID->isTrackedCaller)
            expectedIRArgCount += 3;
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
        llvm::Value* argVal = nullptr;
        if (isRef) {
            messageSystem::startBlock(args[i], "Generating reference argument", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
            const AsaArgumentVariableValue& fa = CalleeFID->arguments[formalArgIdx];

            if (args[i]->childNodes.size() != 1 || args[i]->childNodes[0]->nodeType != Identifier_Node) {
                if (!fa.isConstant) {
                    return messageSystem::error("Cannot pass value as reference");
                }
                // const ref: materialize the cached rvalue into a temporary alloca
                llvm::Value* tmpVal = cachedArgVals[i];
                if (!tmpVal) {
                    messageSystem::endBlock();
                    return nullptr;
                }
                bool wasDef = true;
                llvm::Type* formalType = getLLVMTypeFromString(fa.typeString, fa.pointerLevel, args[i], wasDef, pass);
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
                    llvm::Value* actualVal = cachedArgVals[i];
                    bool wasDef = true;
                    llvm::Type* formalType = getLLVMTypeFromString(fa.typeString, 0, args[i], wasDef, pass);
                    if (formalType && actualVal && !actualVal->getType()->isPointerTy() && !formalType->isPointerTy() && actualVal->getType() != formalType) {
                        return messageSystem::error("Cannot pass '" + getStringTypeFromLLVMType(actualVal->getType()) + "' as 'ref " + fa.typeString + "': implicit cast to reference is not allowed");
                    }
                }
                args[i]->childNodes[0]->isRef = true;
                argVal = (llvm::Value*)(args[i]->*(args[i]->codegen))(pass);
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
        const AsaArgumentVariableValue& formal = CalleeFID->arguments[formalArgIdx];
        if (argVal && argVal->getType()->isStructTy() &&
            formal.pointerLevel == 1 &&
            (formal.typeString == "byte" || formal.typeString == "int8")) {
            // string -> *char: extract .address (element 0)
            argVal = llvmIRBuilder->CreateExtractValue(argVal, {0}, "str_addr");
        }
        else if (argVal && argVal->getType()->isPointerTy() && !isRef &&
                 formal.pointerLevel == 0 && formal.typeString == "string" &&
                 structDefinitions.count("string") && structDefinitions["string"]->structVal) {
            // *char -> string: build string struct with strlen
            StructType* strTy = cast<StructType>((llvm::Type*)structDefinitions["string"]->structVal);
            FunctionCallee strlenFn = llvmCompileModule->getOrInsertFunction("strlen",
                FunctionType::get(llvm::Type::getInt64Ty(*llvmCompileContext), {PointerType::getUnqual(*llvmCompileContext)}, false));
            llvm::Value* lenVal = llvmIRBuilder->CreateCall(strlenFn, {argVal}, "strlen");
            llvm::Value* lenTrunc = llvmIRBuilder->CreateTrunc(lenVal, llvm::Type::getInt32Ty(*llvmCompileContext), "len");
            llvm::Value* strStruct = UndefValue::get(strTy);
            strStruct = llvmIRBuilder->CreateInsertValue(strStruct, argVal, {0});
            strStruct = llvmIRBuilder->CreateInsertValue(strStruct, lenTrunc, {1});
            argVal = strStruct;
        }
        // ABI coercion: split struct into doubles (SSE/XMM) or i64s (INTEGER) per x86-64 SysV ABI
        if (coerce > 0 && argVal && argVal->getType()->isStructTy()) {
            AllocaInst* tmp = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), argVal->getType(), "coerce");
            llvmIRBuilder->CreateStore(argVal, tmp);
            llvm::Type* primTy = coerceIsFloat ? (llvm::Type*)Type::getDoubleTy(*llvmCompileContext) : (llvm::Type*)Type::getInt64Ty(*llvmCompileContext);
            for (int k = 0; k < coerce; k++) {
                llvm::Value* bytePtr = llvmIRBuilder->CreateConstGEP1_64(llvm::Type::getInt8Ty(*llvmCompileContext), tmp, (uint64_t)(k * 8), "coerce_gep");
                ArgsV.push_back(llvmIRBuilder->CreateLoad(primTy, bytePtr, "coerce_val"));
            }
            irArgIdx += coerce;
            continue;
        }
        // Pack @packed struct to integer when formal param expects an integer (extern ABI)
        if (argVal && !isRef && argVal->getType()->isStructTy()) {
            llvm::Type* formalLLVMType = nullptr;
            if (irArgIdx < (int)CalleeF->getFunctionType()->getNumParams())
                formalLLVMType = CalleeF->getFunctionType()->getParamType(irArgIdx);
            if (formalLLVMType && formalLLVMType->isIntegerTy()) {
                AllocaInst* tmp = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), argVal->getType(), "packed_tmp");
                llvmIRBuilder->CreateStore(argVal, tmp);
                llvm::Value* intPtr = llvmIRBuilder->CreateBitCast(tmp, PointerType::get(*llvmCompileContext, 0));
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
            llvm::Type* formalLLVMType = nullptr;
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
        llvm::Value* defVal = cachedArgVals[i];
        if (coerce > 0 && defVal && defVal->getType()->isStructTy()) {
            AllocaInst* tmp = CreateEntryBlockAlloca(llvmIRBuilder->GetInsertBlock()->getParent(), defVal->getType(), "coerce_def");
            llvmIRBuilder->CreateStore(defVal, tmp);
            llvm::Type* primTy = coerceIsFloat ? (llvm::Type*)Type::getDoubleTy(*llvmCompileContext) : (llvm::Type*)Type::getInt64Ty(*llvmCompileContext);
            for (int k = 0; k < coerce; k++) {
                llvm::Value* bytePtr = llvmIRBuilder->CreateConstGEP1_64(llvm::Type::getInt8Ty(*llvmCompileContext), tmp, (uint64_t)(k * 8), "coerce_gep");
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

    // Append tracked_caller hidden parameters if the callee requires them
    if (CalleeFID->isTrackedCaller && token && token->filePath) {
        llvm::Value* hiddenFilepath = llvmIRBuilder->CreateGlobalString(*token->filePath);
        llvm::Value* hiddenLineNum = ConstantInt::get(Type::getInt32Ty(*llvmCompileContext), token->lineNumber);
        llvm::Value* hiddenLineContent = llvmIRBuilder->CreateGlobalString(
            token->lineValue ? *token->lineValue : "");
        ArgsV.push_back(hiddenFilepath);
        ArgsV.push_back(hiddenLineNum);
        ArgsV.push_back(hiddenLineContent);
    }

    llvm::Value* callResult = nullptr;
    llvm::Type* retType = CalleeF->getReturnType();

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
            llvm::Type* bvType = CalleeF->getParamByValType(pi);
            if (bvType)
                callInst->addParamAttr(pi, Attribute::getWithByValType(*llvmCompileContext, bvType));
        }
    }

    messageSystem::endBlock();
    // For struct returns, return the loaded struct value (or pointer if lvalue)
    if (isStructReturn) {
        if (!asaType)
            asaType = new AsaTypeInstance(nullptr);
        asaType->strVal = CalleeFID->returnType;
        if (lvalue) {
            return sretAlloc;  // Return pointer for lvalue contexts (e.g., assignment)
        }
        else {
            AsaStructDefinitionDefinition* retStruct = structDefinitions[CalleeFID->returnType];
            asaType->baseLLVMType = retStruct->structVal;
            return llvmIRBuilder->CreateLoad(retStruct->structVal, sretAlloc, "sret_load");
        }
    }

    if (CalleeFID->returnIsReference) {
        bool wasDefined = true;
        llvm::Type* refType = getLLVMTypeFromString(CalleeFID->returnType, 0, this, wasDefined, pass);
        if (!refType || !wasDefined)
            return messageSystem::error("Cannot resolve ref return type: " + CalleeFID->returnType);
        if (!asaType)
            asaType = new AsaTypeInstance(refType);
        else
            asaType->baseLLVMType = refType;
        asaType->strVal = CalleeFID->returnType;
        asaType->isRef = true;
        if (lvalue || isRef)
            return callResult;
        return llvmIRBuilder->CreateLoad(refType, callResult, "ref_return_load");
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
                asaType = new AsaTypeInstance(nullptr);
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
            llvm::Value* intPtr = llvmIRBuilder->CreateBitCast(tmp, PointerType::get(*llvmCompileContext, 0));
            llvmIRBuilder->CreateStore(callResult, intPtr);
            callResult = llvmIRBuilder->CreateLoad(it->second->structVal, tmp, "unpacked_ret");
        }
    }

    // Record return type string in asaType so callers can do correct type inference.
    if (!CalleeFID->returnType.empty()) {
        if (!asaType)
            asaType = new AsaTypeInstance(callResult ? callResult->getType() : nullptr);
        asaType->strVal = CalleeFID->returnType;
    }

    // Non-struct: Return the call result directly
    return callResult;
}


// llvm::Value*
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

    llvm::Value* CondV = (llvm::Value*)(condExpr->*(condExpr->codegen))(pass);
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

    llvm::Value* ThenV = (llvm::Value*)(scopeBody->*(scopeBody->codegen))(pass);
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

    llvm::Value* ElseV = (llvm::Value*)(elseBody->*(elseBody->codegen))(pass);
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

// TODO: Rewrite struct codegen to not rely on specific passes, but just generate as needed
// llvm::Value*
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
            if (structDefinitions[structName]->sourceNode)
                messageSystem::addAttribute(structDefinitions[structName]->sourceNode, "previously defined here");
            return messageSystem::error("Struct cannot be redefined", messageSystem::Redefined_Error);
        }
    }

    currentStructName.push(structName);
    uint8_t generatingType = 0;  // Generate all member variables first (0), then functions (1)
    argumentList members = argumentList();
    std::vector<Type*> fieldTypes = std::vector<Type*>();
    std::vector<std::string> fieldNames = std::vector<std::string>();
    std::vector<AsaFunctionDefinition*> memberFunctions = std::vector<AsaFunctionDefinition*>();
    std::unordered_map<std::string, uint16_t> memberNameIndexes = std::unordered_map<std::string, uint16_t>();
    std::unordered_map<std::string, ASTNode*> memberDefaultNodes = std::unordered_map<std::string, ASTNode*>();
    std::unordered_map<std::string, ASTNode*> memberNameTypeNodes = std::unordered_map<std::string, ASTNode*>();
    uint16_t i = 0;
    std::vector<ASTNode*> structBodyNodes;
    if (!childNodes.empty() && childNodes[0]) {
        for (ASTNode* bodyChild : childNodes[0]->childNodes) {
            if (bodyChild && bodyChild->nodeType == Scope_Body) {
                for (ASTNode* nestedChild : bodyChild->childNodes)
                    structBodyNodes.push_back(nestedChild);
            }
            else {
                structBodyNodes.push_back(bodyChild);
            }
        }
    }
    for (; generatingType < 2; generatingType++) {
        // Pre-populate memberFunctions from the global AsaFunctionDefinitions list so that
        // member functions can reference each other during codegen.
        if (generatingType == 1 && pass > 1) {
            // Generate prototypes for all member functions first (if not already done
            // in pass 1), so they exist in AsaFunctionDefinitions for cross-referencing during
            // body generation.
            for (auto fieldNode : structBodyNodes) {
                if (fieldNode->nodeType == Compiler_Define_Function) {
                    bool isOperatorOverload = fieldNode->token &&
                                              (fieldNode->token->tokenStr == "operator" || fieldNode->token->tokenStr.rfind("operator.", 0) == 0);
                    if (isOperatorOverload) {
                        if (fieldNode->childNodes.size() > 4 && !fieldNode->childNodes[4]->childNodes.empty()) {
                            (void)(fieldNode->*(fieldNode->codegen))(1);
                            (void)fieldNode->generatePrototype(1);
                        }
                        continue;
                    }
                    bool isCreate = fieldNode->token && fieldNode->token->tokenStr == structName;
                    if (isCreate)
                        continue;
                    std::string qualifiedName = structName + "." + fieldNode->token->tokenStr;
                    bool found = false;
                    for (auto& fid : AsaFunctionDefinitions) {
                        if (fid->name == qualifiedName) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        (fieldNode->*(fieldNode->codegen))(1);
                    }
                }
            }
            memberFunctions.clear();
            for (auto fieldNode : structBodyNodes) {
                if (fieldNode->nodeType == Compiler_Define_Function) {
                    bool isOperatorOverload = fieldNode->token &&
                                              (fieldNode->token->tokenStr == "operator" || fieldNode->token->tokenStr.rfind("operator.", 0) == 0);
                    if (isOperatorOverload)
                        continue;
                    bool isCreate = fieldNode->token && fieldNode->token->tokenStr == structName;
                    if (isCreate)
                        continue;
                    std::string qualifiedName = structName + "." + fieldNode->token->tokenStr;
                    for (auto& fid : AsaFunctionDefinitions) {
                        if (fid->name == qualifiedName) {
                            memberFunctions.push_back(fid);
                            break;
                        }
                    }
                }
            }
            if (structDefinitions.count(structName))
                structDefinitions[structName]->memberFunctions = memberFunctions;
        }
        for (auto fieldNode : structBodyNodes) {

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


                ASTNode* nameTypeNode = fieldNode;
                fieldNode = fieldNode->childNodes[0];

                std::string memberName = fieldNode->token->tokenStr;
                memberNameTypeNodes[memberName] = nameTypeNode;

                llvm::Type* fieldType = nullptr;
                std::string memberType;
                int pointerLevel = 0;
                bool isConst = false;
                if (!getDeclaredTypeFromColonNode(nameTypeNode, fieldType, memberType, pointerLevel, isConst, pass)) {
                    currentStructName.pop();
                    messageSystem::startBlock(nameTypeNode, "Generating struct member", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
                    return messageSystem::error("Unable to resolve struct member type");
                }

                fieldTypes.push_back(fieldType);
                fieldNames.push_back(memberName);
                memberNameIndexes[memberName] = i;

                if (defaultValNode != nullptr)
                    memberDefaultNodes[memberName] = defaultValNode;

                members.push_back(AsaArgumentVariableValue(memberType, getASTNodeTypeFromString(memberType), pointerLevel, false, false, isConst));
                i++;
            }
            // Else it is a member function definition
            else if (fieldNode->nodeType == Compiler_Define_Function &&
                     generatingType == 1 && pass > 1) {
                bool isOperatorOverload = fieldNode->token &&
                                          (fieldNode->token->tokenStr == "operator" || fieldNode->token->tokenStr.rfind("operator.", 0) == 0);
                // Generate function
                Function* memberFunction = (Function*)(fieldNode->*(fieldNode->codegen))(pass);
                if (wasError) {
                    currentStructName.pop();
                    messageSystem::endBlock();
                    return nullptr;
                }
                if (isOperatorOverload)
                    continue;
                // Member function IDs were pre-populated before the loop so that
                // member functions can reference each other during codegen.
            }
        }
        if (generatingType == 0 && pass > 1 && structDefinitions.count(structName)) {
            structDefinitions[structName]->members = members;
            structDefinitions[structName]->memberNameIndexes = memberNameIndexes;
            structDefinitions[structName]->memberDefaultNodes = memberDefaultNodes;
            structDefinitions[structName]->memberNameTypeNodes = memberNameTypeNodes;
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
            AsaFunctionDefinition* ctorFID = nullptr;
            for (auto& fid : AsaFunctionDefinitions)
                if (fid->name == structName && fid->userArguments.empty() && fid->isStructReturn) {
                    ctorFID = fid;
                    break;
                }
            if (ctorFID && ctorFID->fnValue && ctorFID->fnValue->empty()) {
                Function* fn = ctorFID->fnValue;
                BasicBlock* entryBlock = BasicBlock::Create(*llvmCompileContext, "entry", fn);
                llvmIRBuilder->SetInsertPoint(entryBlock);
                llvm::Value* sretPtr = fn->getArg(0);
                StructType* sTy = (StructType*)structDefinitions[structName]->structVal;
                llvm::Value* szVal = ConstantInt::get(llvm::Type::getInt64Ty(*llvmCompileContext), llvmCompileModule->getDataLayout().getTypeAllocSize(sTy));
                Function* memsetFn = Intrinsic::getOrInsertDeclaration(llvmCompileModule.get(), Intrinsic::memset, {sretPtr->getType(), llvm::Type::getInt64Ty(*llvmCompileContext)});
                llvmIRBuilder->CreateCall(memsetFn, {sretPtr, ConstantInt::get(llvm::Type::getInt8Ty(*llvmCompileContext), 0), szVal, ConstantInt::get(llvm::Type::getInt1Ty(*llvmCompileContext), 0)});
                for (auto& [memberName, defaultNode] : memberDefaultNodes) {
                    auto idxIt = memberNameIndexes.find(memberName);
                    if (idxIt == memberNameIndexes.end())
                        continue;
                    uint16_t idx = idxIt->second;
                    llvm::Value* memberPtr = llvmIRBuilder->CreateStructGEP(sTy, sretPtr, idx, memberName + "_init");
                    llvm::Value* defaultVal = nullptr;
                    ASTNode* unwrappedDefault = unwrapSingleExpressionNode(defaultNode);
                    if (unwrappedDefault && unwrappedDefault->nodeType == Undefined_Initializer_Node)
                        defaultVal = UndefValue::get(sTy->getElementType(idx));
                    else if (unwrappedDefault && unwrappedDefault->nodeType == Default_Initializer_Node)
                        defaultVal = generateDefaultValueForType(sTy->getElementType(idx), members[idx].typeString, members[idx].pointerLevel, pass, this);
                    else
                        defaultVal = (llvm::Value*)(defaultNode->*(defaultNode->codegen))(pass);
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
            if (!getExactFunctionFromID(AsaFunctionDefinitions, const_cast<std::string&>(structName), emptyUserArgs)) {
                std::vector<Type*> ctorArgTypes = {PointerType::get(*llvmCompileContext, 0)};
                FunctionType* FT = FunctionType::get(llvm::Type::getVoidTy(*llvmCompileContext), ctorArgTypes, false);
                Function* fn = Function::Create(FT, Function::InternalLinkage, structName, llvmCompileModule.get());
                fn->addFnAttr(llvm::Attribute::AlwaysInline);
                fn->getArg(0)->setName("sret");
                argumentList llvmArgs = {AsaArgumentVariableValue("*" + structName, Struct_Type, 1, false, true)};
                AsaFunctionDefinitions.push_back(new AsaFunctionDefinition(structName, this, token, structName, structName, llvmArgs, emptyUserArgs, fn, false, false, true));
                AsaFunctionDefinitions.back()->isReplaceable = true;
            }
        };
        addDefaultCtorProto(existingTy);
        messageSystem::endBlock();
        return existingTy;
    }

    StructType* structTy = StructType::create(*llvmCompileContext, fieldTypes, "struct." + structName);

    currentStructName.pop();
    auto newStructDef = new AsaStructDefinition(structName, token, structTy, members, memberFunctions, memberNameIndexes);
    newStructDef->sourceNode = this;
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
        if (!getExactFunctionFromID(AsaFunctionDefinitions, const_cast<std::string&>(structName), emptyUserArgs)) {
            std::vector<Type*> ctorArgTypes = {PointerType::get(*llvmCompileContext, 0)};
            FunctionType* FT = FunctionType::get(llvm::Type::getVoidTy(*llvmCompileContext), ctorArgTypes, false);
            Function* fn = Function::Create(FT, Function::InternalLinkage, structName, llvmCompileModule.get());
            fn->addFnAttr(llvm::Attribute::AlwaysInline);
            fn->getArg(0)->setName("sret");
            argumentList llvmArgs = {AsaArgumentVariableValue("*" + structName, Struct_Type, 1, false, true)};
            AsaFunctionDefinitions.push_back(new AsaFunctionDefinition(structName, this, token, structName, structName, llvmArgs, emptyUserArgs, fn, false, false, true));
            AsaFunctionDefinitions.back()->isReplaceable = true;
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

// llvm::Value*
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

// llvm::Value*
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
// llvm::Value*
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
// llvm::Value*
void* ASTNode::generateFor(int pass)
{
    messageSystem::startBlock(this, "Generating for loop", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Debug info:
    if (!LexicalBlocks.empty() && token && token->filePath)
        llvmIRBuilder->SetCurrentDebugLocation(DILocation::get(LexicalBlocks.back()->getContext(), token->lineNumber + 1, 0, LexicalBlocks.back()));

    // Placeholder iterator name, for if one is not provided, like: `for(0..5)`
    std::string varName = "_iterator";
    std::string label = "";  // Optional label for the loop

    // Check if this loop has a label (set by generateLabeledLoop)
    if (!this->label.empty()) {
        label = this->label;
    }

    // If an iterator is provided, use it
    if (childNodes[0]->nodeType == Iterator) {
        varName = childNodes[0]->childNodes[0]->token->tokenStr;
    }

    // TODO: Make this use the `range` struct type instead
    // Get the start and end range values
    ASTNode* rangeStart = childNodes[1]->childNodes[0];
    ASTNode* rangeEnd = childNodes[1]->childNodes[1];

    // Compute start value.
    //if (rangeStart->codegen == nullptr) {
    //    return messageSystem::error("Node `" + ASTNodeTypeAsString(rangeStart->nodeType) + "` does not have a code generator");
    //}
    llvm::Value* StartVal = (llvm::Value*)(rangeStart->*(rangeStart->codegen))(pass);
    if (wasError || !StartVal) {
        messageSystem::endBlock();
        return nullptr;
    }

    // Compute end value in the preheader so we can determine the iterator type.
    llvm::Value* EndVal = (llvm::Value*)(rangeEnd->*(rangeEnd->codegen))(pass);
    if (wasError || !EndVal) {
        messageSystem::endBlock();
        return nullptr;
    }

    // Determine iterator type: use explicit annotation if provided, otherwise widen from range.
    llvm::Type* iterType = nullptr;
    if (childNodes[0]->nodeType == Iterator && childNodes[0]->childNodes.size() >= 2) {
        // Explicit type annotation: for(i : uint16 in ...)
        ASTNode* typeNode = childNodes[0]->childNodes[1];
        bool wasDefined = false;
        int resolvePass = pass;
        iterType = getLLVMTypeFromString(typeNode->token->tokenStr, 0, typeNode, wasDefined, resolvePass);
    }
    if (!iterType) {
        // Auto-determine: widest integer type among start and end, minimum i8.
        unsigned iterBits = 8;
        if (StartVal->getType()->isIntegerTy())
            iterBits = std::max(iterBits, StartVal->getType()->getIntegerBitWidth());
        if (EndVal->getType()->isIntegerTy())
            iterBits = std::max(iterBits, EndVal->getType()->getIntegerBitWidth());
        iterType = llvm::Type::getIntNTy(*llvmCompileContext, iterBits);
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

    llvm::Value* CurVar = llvmIRBuilder->CreateLoad(iterType, Alloca, varName.c_str());

    // Compare: exclusive (i < N)
    llvm::Value* Cond = llvmIRBuilder->CreateICmpSLT(CurVar, EndVal, "loopcond");

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

    // Put the iterator variable into the named values
    namedValues[varName] = new AsaVariableValue(varName, getStringTypeFromLLVMType(iterType), Alloca);

    // Emit the body of the loop
    ASTNode* scopeBody = childNodes[2];

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

    // TODO: Allow for custom step code
    // Step block: increment iterator then jump back to condition
    llvmIRBuilder->SetInsertPoint(StepBB);
    llvm::Value* StepVal = ConstantInt::get(*llvmCompileContext, APInt(iterType->getIntegerBitWidth(), 1));
    llvm::Value* CurVar2 = llvmIRBuilder->CreateLoad(Alloca->getAllocatedType(), Alloca, varName.c_str());
    llvm::Value* NextVar = llvmIRBuilder->CreateAdd(CurVar2, StepVal, "nextvar");
    llvmIRBuilder->CreateStore(NextVar, Alloca);
    llvmIRBuilder->CreateBr(LoopCondBB);

    // After loop
    llvmIRBuilder->SetInsertPoint(AfterBB);

    messageSystem::endBlock();
    return nullptr;
}

// Similarly, if you have a while loop generator, update it too:
// llvm::Value*
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

    llvm::Value* CondV = (llvm::Value*)(condExpr->*(condExpr->codegen))(pass);
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
    // Function modifiers, compiler directives between the prototype and the body like:
    //     ```asa
    //     foo :: int() #example {
    //     }
    //     ```
    //
    // TODO: Modifiers will likely either be unused in the future, or a location for executable metaprogramming code.
    ASTNode* modifiersNode = childNodes[3];
    std::vector<std::string> argNames = std::vector<std::string>();
    // TODO: Make the function name *never* mangled.
    //       The only case where it should be "mangled" is if it never had a proper name to begin with.
    //       For example, `operator(+) :: foo(l : foo, r : foo)`. This would have the name `operator(+)` if
    //       left unchanged, which would be invalid. So it should be modified to be `operator(+).
    std::string fnName = token->tokenStr;
    std::string mangledName = token->tokenStr;
    bool isStructMemberFunction = false;
    // Is true if this is not a regular function, but rather one with a special use, such as an
    // operator overload, cast, constructor, destructor
    bool isSpecial = false;
    bool isOperatorOverload = fnName == "operator" || fnName.rfind("operator.", 0) == 0;
    bool isCreateConstructor = currentStructName.size() > 0 && fnName == currentStructName.top();

    // If this is an operator overload function:
    if (fnName == "operator") {
        isSpecial = true;
        // The first child for `operator(+)` looks like this:
        //     ```
        //     operator:(Operator_Overload_Node){
        //         +:(Operator_Type_Node){}
        //     }
        //     ```
        // So this makes the name be: `operator.Plus`
        fnName = fnName + "." + tokenTypeAsString(childNodes[0]->childNodes[0]->token->tokenType);
        //mangledName = fnName + "." + tokenTypeAsString(childNodes[0]->childNodes[0]->token->tokenType);
        mangledName = fnName;
    }
    // TODO: Does this branch ever execute? wouldnt the above condition always execute instead when this should?
    else if (isOperatorOverload) {
        isSpecial = true;
    }
    // Handle special functions cast, create, and destroy
    else if (fnName == "cast" || fnName == "create" || fnName == "destroy") {
        isSpecial = true;
        // The first child for `cast :: bool(x : int)` looks like this:
        //     ```
        //     cast:(Identifier_Node){}
        //     ```
        // And the function name was already swapped in the parser stage, so instead of `cast`, it is `bool`
        // So this makes the name be: `bool.cast`
        fnName = fnName + "." + tokenTypeAsString(childNodes[0]->token->tokenType);
        //mangledName = fnName + "." + tokenTypeAsString(childNodes[0]->childNodes[0]->token->tokenType);
        mangledName = fnName;
    }
    // If this function is inside of a struct (a member function)
    else if (currentStructName.size() != 0 && fnName != currentStructName.top()) {
        fnName = currentStructName.top() + "." + fnName;
        mangledName = currentStructName.top() + "." + mangledName;
        isStructMemberFunction = true;
    }
    // If this function is inside of a module
    else if (!enclosingModule.empty()) {
        fnName = enclosingModule + "." + fnName;
        mangledName = enclosingModule + "." + mangledName;
    }

    // Get the return type
    AsaTypeInstance* asaReturnTypeInstance = nullptr;
    std::string mangledReturnType = "";
    ASTNode* returnTypeNode = childNodes[1];
    // TODO: Make all of these values use AsaTypeInstance
    llvm::Type* retType = llvm::Type::getVoidTy(*llvmCompileContext);  // By default, it is `void`
    std::string rTypeString = "";
    bool isStructReturn = false;
    bool returnIsReference = false;

    int8_t externReturnCoercionCount = 0;
    bool externReturnCoercionIsFloat = false;


    // If the return type is not specified, make it `void`
    // The return type node should be like this currently: `Type_Node{}`
    if (returnTypeNode->childNodes.size() == 0) {
        asaReturnTypeInstance = CreateVoidAsaTypeInstance();
    }
    // Otherwise, it has a specified return type
    // The return type node should be like this currently: `Type_Node{Identifier_Node{}}`
    else {
        //recurseAddPointer:
        // Set the return type node to its child. This should be the actual type node
        returnTypeNode = returnTypeNode->childNodes[0];

        // Get the Asa type instance from the node, modifying the `returnTypeNode` to be the
        // actual type name, without modifiers.
        asaReturnTypeInstance = CreateAsaTypeInstanceFromASTNode(retrunTypeNode);

        // Get the mangled return type name
        // Example: `foo :: *int(){}` -> `ret.ptr.int`
        mangledReturnType = "ret." + asaReturnTypeInstance->getMangledName();

        //if (returnTypeNode->token->tokenStr == "ref")
        //    returnIsReference = true;
        //if (returnTypeNode->token->tokenStr == "const" || returnTypeNode->token->tokenStr == "ref" || returnTypeNode->token->tokenStr == "exact") {
        //    if (returnTypeNode->childNodes.size() > 0)
        //        goto recurseAddPointer;
        //}
        //if (fnName == "main") {
        //    // main is the C entry point and must not be name-mangled
        //}
        //else if (returnTypeNode->token->tokenStr == "*")
        //    mangledName += ".ptr";
        //else
        //    mangledName += "." + returnTypeNode->token->tokenStr;
        rTypeString += resolveTypeAlias(returnTypeNode->token->tokenStr);

        //if (returnTypeNode->token->tokenStr == "*") {
        //    goto recurseAddPointer;
        //}

        //bool wasDefined = true;
        //retType = getLLVMTypeFromString(rTypeString, 0, returnTypeNode, wasDefined, pass);
        retType = asaReturnTypeInstance->llvmType;

        if (asaReturnTypeInstance->hasModifier(Reference_Operation)) {
            retType = PointerType::get(*llvmCompileContext, 0);
        }
        // If the return type is a struct
        else if (asaReturnTypeInstance->baseType->isStruct) {
            // For extern @packed structs, return as integer (no sret)
            if (isExtern) {
                auto it = structDefinitions.find(rTypeString);
                if (it != structDefinitions.end() && it->second->isPacked) {
                    unsigned bits = llvmCompileModule->getDataLayout().getTypeAllocSizeInBits(retType);
                    retType = llvm::Type::getIntNTy(*llvmCompileContext, bits);
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
                            chunkTypes.push_back(allFloat ? (llvm::Type*)Type::getDoubleTy(*llvmCompileContext) : (llvm::Type*)Type::getInt64Ty(*llvmCompileContext));
                        retType = StructType::get(*llvmCompileContext, chunkTypes);
                        // These will be recorded on the AsaFunctionDefinition after it is constructed (see below)
                        externReturnCoercionCount = (int8_t)numChunks;
                        externReturnCoercionIsFloat = allFloat;
                    }
                }
            }
            // Otherwise, just set isStructReturn to true
            else {
                isStructReturn = true;
            }
        }
    }

    // Like C, main always returns int32 even when declared without a return type.
    // This is checked here, after resolving the return type, so that it will override
    // whatever return type may be specified. Example: `main :: void (){}` -> `main :: int32(){}`
    //
    // TODO: Make it error if main has a return type != int32 or blank. Then move this override up.
    if (fnName == "main" && retType->isVoidTy())
        retType = llvm::Type::getInt32Ty(*llvmCompileContext);

    argumentList userArgList = argList;

    // If this function has a struct return (sret), then the arguments need to have a hidden sret argument.
    // Handle struct return by modifying function signature
    llvm::Type* actualRetType = retType;
    if (isStructReturn) {
        // For struct returns, add sret parameter as first argument
        argTypes.push_back(PointerType::get(*llvmCompileContext, 0));  // sret parameter (pointer to struct)
        argNames.push_back("sret");
        //argList.insert(argList.begin(), AsaArgumentVariableValue("*" + rTypeString, getASTNodeTypeFromString(rTypeString), 1, false, true));

        AsaArgumentVariableValue* argTypeInstance = asaReturnTypeInstance->getPointerTo();
        argList.insert(argList.begin(), argTypeInstance);

        // Change actual return type to void
        actualRetType = llvm::Type::getVoidTy(*llvmCompileContext);
    }

    // Get function arguments
    // If it is a struct member function, first add a "this" argument like: (this : ref structName, ...)
    if (currentStructName.size() > 0 && !isOperatorOverload && !isCreateConstructor) {
        std::string typeStr = currentStructName.top();
        bool isReference = true;
        int pointerLevel = 0;  // References don't count as pointer level

        llvm::Type* aType = nullptr;
        argList.push_back(AsaArgumentVariableValue(typeStr, Struct_Type, pointerLevel, isReference, false, false));

        try {
            bool wasDefined = true;
            aType = getLLVMTypeFromString(typeStr, 0, this, wasDefined, pass);
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
    bool variableNumArguments = false;      // True if this is a variadic function, like `printf(s : string, ...)`
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
                typeStr += resolveTypeAlias(typeNode->token->tokenStr);


                llvm::Type* aType = nullptr;
                AsaArgumentVariableValue arg = AsaArgumentVariableValue(typeStr, getASTNodeTypeFromString(typeNode->token->tokenStr), pointerLevel, isReference, mustBeExactType, isConstant);
                if (a->childNodes.size() > 1) {
                    arg.hasDefault = true;
                    arg.defaultNode = a->childNodes[1];
                    arg.defaultRawTokens = a->childNodes[1]->defaultRawTokens;
                }
                argList.push_back(arg);
                userArgList.push_back(arg);

                try {
                    bool wasDefined = true;
                    aType = getLLVMTypeFromString(typeNode->token->tokenStr, 0, typeNode, wasDefined, pass);

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
                            aType = llvm::Type::getIntNTy(*llvmCompileContext, bits);
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
                                    argTypes.push_back(llvm::Type::getDoubleTy(*llvmCompileContext));
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
                                    argTypes.push_back(llvm::Type::getInt64Ty(*llvmCompileContext));
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

    bool isTrackedCaller = hasAttribute(this, "tracked_caller");
    if (isTrackedCaller) {
        llvm::Type* i8PtrTy = PointerType::getUnqual(*llvmCompileContext);
        argTypes.push_back(i8PtrTy);
        argTypes.push_back(Type::getInt32Ty(*llvmCompileContext));
        argTypes.push_back(i8PtrTy);
        argNames.push_back("__caller_filepath");
        argNames.push_back("__caller_linenum");
        argNames.push_back("__caller_line");
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
    AsaFunctionDefinition* theFunctionID = getExactFunctionFromID(AsaFunctionDefinitions, fnName, userArgList);
    if (theFunctionID) {
        // If the existing AsaFunctionDefinition was created by a different definition node,
        // treat this as a redefinition error. Auto-generated defaults are exempt
        // (isReplaceable) so user-defined functions can legitimately override them.
        if (theFunctionID->declNode && theFunctionID->declNode != this && !theFunctionID->isReplaceable) {
            messageSystem::addAttribute(theFunctionID->declNode, "previously defined here");
            messageSystem::error("Function cannot be redefined, requires unique identity", messageSystem::Redefined_Error);
            messageSystem::endBlock();
            return theFunctionID->fnValue;
        }
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
    int numHidden = isTrackedCaller ? 3 : 0;
    for (auto& arg : fn->args()) {
        if (Idx < (int)argNames.size())
            arg.setName(argNames[Idx]);

        if (Idx < (int)argNames.size() - numHidden && Idx < (int)argList.size()) {
            if (argList[Idx].isConstant) {
                llvm::Type* AsaArgumentVariableValue = arg.getType();

                // readonly can only be applied to pointer types
                if (argType->isPointerTy()) {
                    arg.addAttr(llvm::Attribute::ReadOnly);
                }
            }
        }
        Idx++;
    }

    AsaFunctionDefinitions.push_back(new AsaFunctionDefinition(fnName, this, token, mangledName, rTypeString, argList, userArgList, fn, variableNumArguments, isStructMemberFunction, isStructReturn, returnIsReference));
    AsaFunctionDefinitions.back()->externReturnCoercionCount = externReturnCoercionCount;
    AsaFunctionDefinitions.back()->externReturnCoercionIsFloat = externReturnCoercionIsFloat;
    AsaFunctionDefinitions.back()->isTrackedCaller = isTrackedCaller;
    //AsaFunctionDefinitions.back()->declNode = this;
    if (verbosity >= 5) {
        console::printIndent(depth + 2);
        console::writeLine("-- Added function \"" + fnName + "\" to AsaFunctionDefinitions");
    }

    messageSystem::endBlock();
    return fn;
}

// Function*
void* ASTNode::generateFunction(int pass)
{
    messageSystem::startBlock(this, "Defining function", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Nested function definitions are not allowed
    for (ASTNode* p = this->parentNode; p; p = p->parentNode) {
        if (p->nodeType == Compiler_Define_Function) {
            messageSystem::error("Nested function definitions are not allowed");
            messageSystem::endBlock();
            return nullptr;
        }
    }

    // First, check for an existing function from a previous declaration.
    //Function* theFunction = llvmCompileModule->getFunction(token->tokenStr);
    AsaFunctionDefinition* theFunctionID = nullptr;
    Function* theFunction = nullptr;
    std::string functionName = token->tokenStr;
    bool isStruct = false;
    bool isOperatorOverload = functionName == "operator" || functionName.rfind("operator.", 0) == 0;
    bool isCreateConstructor = currentStructName.size() > 0 && functionName == currentStructName.top();

    if (currentStructName.size() > 0 && !isOperatorOverload && !isCreateConstructor) {
        functionName = currentStructName.top() + "." + functionName;
        isStruct = true;
    }
    else if (!enclosingModule.empty()) {
        functionName = enclosingModule + "." + functionName;
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
        theFunctionID = getFunctionIDFromFunctionPointer(AsaFunctionDefinitions, theFunction);
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
        // Keep the AsaFunctionDefinition in the list (its LLVM fn pointer and signature are still correct).
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

    theFunctionID = getFunctionIDFromFunctionPointer(AsaFunctionDefinitions, theFunction);
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


    // Push tracked_caller hidden params onto the stack so #caller_* directives can access them
    if (theFunctionID->isTrackedCaller) {
        CallerLocationParams params;
        int argIdx = 0;
        for (auto& arg : theFunction->args()) {
            std::string name = arg.getName().str();
            if (name == "__caller_filepath")
                params.filepath = &arg;
            else if (name == "__caller_linenum")
                params.lineNum = &arg;
            else if (name == "__caller_line")
                params.lineContent = &arg;
            argIdx++;
        }
        callerLocationParamStack.push(params);
    }

    // Add remaining arguments
    int i = 0;
    for (auto& arg : theFunction->args()) {
        std::string argName = arg.getName().str();
        if (theFunctionID->isTrackedCaller &&
            (argName == "__caller_filepath" || argName == "__caller_linenum" || argName == "__caller_line")) {
            continue;  // Skip hidden tracked_caller params (no alloca/named value needed)
        }
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
            llvmIRBuilder->CreateCall(invariantStartFn, {ConstantInt::get(llvm::Type::getInt64Ty(*llvmCompileContext), typeSize), Alloca});
        }

        // Build the "actual type" string the rest of your compiler expects (pointer stars + type name)
        std::string baseType = "";
        for (int p = 0; p < theFunctionID->arguments[i].pointerLevel; p++)
            baseType += "*";

        AsaVariableValue* vt = new AsaVariableValue(std::string(arg.getName()), baseType + theFunctionID->arguments[i].typeString, Alloca, true, theFunctionID->arguments[i].isReference);

        vt->isConstant = theFunctionID->arguments[i].isConstant;

        namedValues[std::string(arg.getName())] = vt;

        // Emit debug info for this parameter so GDB can show argument values
        if (llvmDebugBuilder && !LexicalBlocks.empty()) {
            std::string argTypeStr = baseType + theFunctionID->arguments[i]->strVal;
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

    if (theFunctionID->isTrackedCaller && !callerLocationParamStack.empty())
        callerLocationParamStack.pop();

    messageSystem::endBlock();
    return theFunction;
}

// Nothing
void* ASTNode::generateNothing(int pass)
{
    messageSystem::startBlock(this, "Generating node", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
    return messageSystem::error("Node is missing a codegen function");
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

    // Get name of the flag
    ASTNode* nameNode = unwrapSingleExpressionNode(args[0]);
    if (!nameNode)
        return messageSystem::error("#getflag requires a flag name argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    if (nameNode->nodeType == Comma_Node && !nameNode->childNodes.empty())
        nameNode = nameNode->childNodes[0];

    while (nameNode && nameNode->nodeType == Expression_Term && nameNode->childNodes.size() == 1)
        nameNode = nameNode->childNodes[0];

    // Ensure the flag name is properly formed
    if (!nameNode || !nameNode->token ||
        (nameNode->nodeType != Identifier_Node && nameNode->nodeType != String_Node && nameNode->nodeType != String_Constant_Node))
        return messageSystem::error("#getflag flag name must be an identifier or string literal", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    std::string flagName = nameNode->token->tokenStr;
    if (nameNode->nodeType == String_Node || nameNode->nodeType == String_Constant_Node)
        flagName = decodeQuotedStringToken(nameNode->token);

    // Get the value of the flag
    auto flag = compilerDirectiveFlags.find(flagName);
    bool flagValue = flag != compilerDirectiveFlags.end() && flag->second;
    llvm::Value* result = ConstantInt::get(llvm::Type::getInt1Ty(*llvmCompileContext), flagValue ? 1 : 0);

    asaType = new AsaTypeInstance(getAsaBaseTypeFromName("bool"), false, true, "bool", 0);
    //if (!asaType)
    //    asaType = new AsaTypeInstance(result->getType());
    //else
    //    asaType->baseLLVMType = result->getType();

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

    //if (childNodes.size() < 2)
    //    return messageSystem::error("#stack_last requires stack name argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty())
        return messageSystem::error("#stack_last requires stack name argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    // Get the name of the stack
    ASTNode* nameNode = unwrapSingleExpressionNode(args[0]);
    std::string stackName = nameNode->token->tokenStr;
    if (nameNode->nodeType == String_Node || nameNode->nodeType == String_Constant_Node)
        stackName = decodeQuotedStringToken(nameNode->token);
    // Make sure the stack with that name actually exists and is not empty
    if (!compilerStacks.count(stackName) || compilerStacks[stackName].empty())
        return messageSystem::error("#stack_last used with an empty stack", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    // Get the node at the top of the specified stack
    ASTNode* stackNode = compilerStacks[stackName].top();
    if (!stackNode || !stackNode->codegen)
        return messageSystem::error("#stack_last found a node without a code generator", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    // Then perform codegen on that node
    llvm::Value* value = (llvm::Value*)(stackNode->*(stackNode->codegen))(pass);
    if (stackNode->asaType) {
        if (!asaType)
            asaType = stackNode->asaType;
        //asaType->baseLLVMType = stackNode->asaType->baseLLVMType;
        //asaType->strVal = stackNode->asaType->strVal;
    }

    messageSystem::endBlock();
    pushCompilerDirectiveCall("stack_last", this);
    return value;
}

// OPENCODE:
// resolveASTNode callback for #stack_last: returns the top ASTNode on the
// named compiler stack without popping it. Used when #stack_last is used in
// a compile-time context (e.g. #nameof, #typeof, member access).
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

// OPENCODE:
// resolveASTNode callback for #context: returns the context ASTNode passed
// to a custom compiler directive. Errors if used outside a custom directive
// invocation.
ASTNode* ASTNode::resolveCompilerContextASTNode(int pass)
{
    messageSystem::error("#context is only available inside a custom compiler directive invocation", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    return nullptr;
}

// OPENCODE:
// resolveASTNode callback for #parent: returns the parentNode of the given
// AST node argument (or the caller's own parent if no argument). Used in
// custom compiler directives to walk the AST upward.
ASTNode* ASTNode::resolveCompilerParentASTNode(int pass)
{
    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty())
        return this->parentNode;

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

// OPENCODE:
// resolveASTNode callback for #func_ast: walks up parentNodes until it finds
// a Compiler_Define_Function node. Errors if used outside a function definition.
ASTNode* ASTNode::resolveCompilerFuncASTNode(int pass)
{
    ASTNode* scope = this->parentNode;
    while (scope) {
        if (scope->nodeType == Compiler_Define_Function)
            return scope;
        scope = scope->parentNode;
    }
    messageSystem::error("#func_ast is only available inside a function definition", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
    return nullptr;
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

    if (nodeToPrint->nodeType == Identifier_Node && nodeToPrint->token) {
        ASTNode* scope = this->parentNode;
        while (scope) {
            auto it = scope->compilerDefinitions.find(nodeToPrint->token->tokenStr);
            if (it != scope->compilerDefinitions.end()) {
                nodeToPrint = it->second;
                break;
            }
            scope = scope->parentNode;
        }
    }

    printAST(nodeToPrint);

    messageSystem::endBlock();
    pushCompilerDirectiveCall("print_ast", this);
    return nullptr;
}

// OPENCODE:
// Shared implementation for #print and #printl compiler directives: prints
// a string literal to the console during compilation (pass 2 only). The
// appendNewline flag selects between #print and #printl behavior.
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
static llvm::Value* createStringConstantLiteral(const std::string& str)
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

    Constant* zero = ConstantInt::get(llvm::Type::getInt32Ty(*llvmCompileContext), 0);
    std::vector<Constant*> indices = {zero, zero};
    Constant* strPtr = ConstantExpr::getGetElementPtr(globalStr->getValueType(), globalStr, indices);

    // Return a string struct when the string type is defined (matches String_Constant_Node behavior)
    if (!compilerDirectiveFlags["IN_STRING_MODULE"] &&
        structDefinitions.count("string") && structDefinitions["string"]->structVal) {
        StructType* strTy = cast<StructType>((llvm::Type*)structDefinitions["string"]->structVal);
        Constant* lenConst = ConstantInt::get(llvm::Type::getInt32Ty(*llvmCompileContext), (uint32_t)str.size());
        return ConstantStruct::get(strTy, {strPtr, lenConst});
    }
    return strPtr;
}

// OPENCODE:
// Traverses to the rightmost descendant with a token in an AST subtree.
// Used by #nameof to extract the name token from expressions like `a.b.c`.
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

    llvm::Value* result = createStringConstantLiteral(name);
    this->asaType = new AsaTypeInstance(getAsaBaseTypeFromName("string"), false, true, "string", 0);
    //if (!asaType)
    //    asaType = new AsaTypeInstance(result->getType());
    //else
    //    asaType->baseLLVMType = result->getType();

    pushCompilerDirectiveCall("nameof", this);
    return result;
}

// #typeof(expr) - returns the Asa type name of expr as a string.
// For identifiers, looks up namedValues to avoid emitting any IR.
// For other expressions, generates the expression and reads the LLVM type.
//
// TODO: In the future, I may make `#typeof` return an actual type descriptor or similar, and
//       use `#typename` to do what this currently does
void* ASTNode::generateTypeofDirective(int pass)
{
    if (pass == 0)
        return nullptr;

    messageSystem::startBlock(this, "Generating `#typeof` directive", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);
    defer(messageSystem::endBlock());

    std::vector<ASTNode*> args = getCompilerDirectiveArgs(this);
    if (args.empty())
        return messageSystem::error("#typeof requires a type or variable argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);

    // Get the expression that we are getting the type of: `#sizeof(<this>)`
    ASTNode* argExpr = unwrapSingleExpressionNode(args[0]);
    std::string typeStr;

    // If this is an identifier, check if it is a variable name
    if (argExpr->nodeType == Identifier_Node) {
        AsaVariableValue* val = findNamedValue(parentNode, this, argExpr->token->tokenStr, token);
        if (val) {
            typeStr = val->asaTypeInstance->strVal;
        }
        // If it is not a variable name, then check if it is a type name:
        else {
            std::string maybeTypeName = argExpr->token->tokenStr;
            bool wasDefined = true;
            int typePass = pass;
            llvm::Type* type = getLLVMTypeFromString(maybeTypeName, 0, argExpr, wasDefined, typePass);
            if (type && wasDefined)
                typeStr = resolveTypeAlias(maybeTypeName);
        }
    }

    // Fallback: generate the expression and read the LLVM type
    if (typeStr.empty()) {
        llvm::Value* val = (llvm::Value*)(argExpr->*(argExpr->codegen))(pass);
        if (!val || wasError)
            return nullptr;
        // TODO: implement

        //typeStr = argExpr->strVal;
        //if (argExpr->asaType && argExpr->asaType->baseLLVMType)
        //    typeStr = getStringTypeFromLLVMType(argExpr->asaType->baseLLVMType);
        //else
        //    typeStr = getStringTypeFromLLVMType(val->getType());
    }

    llvm::Value* result = createStringConstantLiteral(typeStr);
    //if (!asaType)
    asaType = new AsaTypeInstance(getAsaBaseTypeFromName("string"), false, true, "string", 0);
    ////asaType = new AsaTypeInstance(result->getType());
    //else
    //    asaType->baseLLVMType = result->getType();

    pushCompilerDirectiveCall("typeof", this);
    return result;
}

// #sizeof(T) - returns the alloc size in bytes of T as an int64 constant.
// T can be a variable name, a plain type name, or a pointer-modified type (e.g. *int, * *Wall).
// TODO: Make this get the type from the new type system, rather than resolving it itself
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
    llvm::Type* llvmType = nullptr;
    AsaTypeInstance* asaTypeInstance = nullptr;

    messageSystem::startBlock(argNode, "Getting argument type", __func__, __LINE__, __FILE__, messageSystem::Codegen_Block);

    // Walk a chain of Dereference_Operation / Pointer_Node nodes to count pointer
    // indirections, then resolve the base identifier as a type.
    auto resolvePointerTypeNode = [&](ASTNode* n) -> llvm::Type* {
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
        llvm::Type* t = getLLVMTypeFromString(typeName, 0, n, wasDefined, pass);
        return (wasDefined && t) ? t : nullptr;
    };

    // If the argument is just an identifier:
    if (argNode->nodeType == Identifier_Node) {
        std::string name = argNode->token->tokenStr;

        // Try as a variable first, using its stored type string (which encodes pointer depth)
        AsaVariableValue* val = findNamedValue(parentNode, this, name, token);
        if (val) {
            bool wasDefined = true;
            asaTypeInstance = val->asaTypeInstance;
            //llvmType = getLLVMTypeFromString(val->asaTypeInstance->strVal, 0, argNode, wasDefined, pass);
            //if (!wasDefined)
            //    llvmType = nullptr;
        }

        // Fall back to treating the identifier as a plain type name
        if (!llvmType) {
            bool wasDefined = true;
            llvmType = getLLVMTypeFromString(name, 0, argNode, wasDefined, pass);
            if (!wasDefined)
                llvmType = nullptr;
        }
    }
    // Else, if the argument has modifiers, like `* const int` or `*var`
    else if (argNode->nodeType == Dereference_Operation || argNode->nodeType == Pointer_Node) {
        llvmType = resolvePointerTypeNode(argNode);
    }

    // Fallback: generate the expression and read the LLVM type
    if (!llvmType) {
        llvm::Value* val = (llvm::Value*)(argNode->*(argNode->codegen))(pass);
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
    llvm::Value* sizeVal = ConstantInt::get(llvm::Type::getInt64Ty(*llvmCompileContext), size);
    asaType = new AsaTypeInstance(getAsaBaseTypeFromName("int64"), false, true, "int64", 0);
    //if (!asaType)
    //    asaType = new AsaTypeInstance(sizeVal->getType());
    //else
    //    asaType->baseLLVMType = sizeVal->getType();

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
    bool hasUnimplementedDirective = false;
    std::function<void(ASTNode*)> checkUnimplementedDirectives = [&](ASTNode* n) {
        if (!n || hasUnimplementedDirective)
            return;
        if (n->nodeType == Compile_Time_Directive && !n->codegen && !n->returnsASTNode) {
            hasUnimplementedDirective = true;
            return;
        }
        for (auto* child : n->childNodes)
            checkUnimplementedDirectives(child);
    };
    checkUnimplementedDirectives(argNode);
    if (hasUnimplementedDirective) {
        llvm::Value* result = ConstantInt::get(llvm::Type::getInt1Ty(*llvmCompileContext), 0);
        asaType = new AsaTypeInstance(getAsaBaseTypeFromName("bool"), false, true, "bool", 0);
        //if (!asaType)
        //    asaType = new AsaTypeInstance(result->getType());
        //else
        //    asaType->baseLLVMType = result->getType();
        messageSystem::endBlock();
        pushCompilerDirectiveCall("compiles", this);
        return result;
    }

    // Save current state
    BasicBlock* savedInsertBlock = llvmIRBuilder->GetInsertBlock();
    BasicBlock::iterator savedInsertPoint = llvmIRBuilder->GetInsertPoint();
    bool savedWasError = wasError;
    uint8_t savedErrorDepth = errorDepth;
    bool savedSuppressErrors = messageSystem::suppressErrors;
    messageSystem::MessageBlockNode* savedMessageNode = messageSystem::currentNode;

    // Create a temporary function and BasicBlock for speculative codegen
    FunctionType* dummyFnType = FunctionType::get(llvm::Type::getVoidTy(*llvmCompileContext), false);
    Function* dummyFn = Function::Create(dummyFnType, Function::PrivateLinkage, "__compiles_probe__", llvmCompileModule.get());
    BasicBlock* tempBB = BasicBlock::Create(*llvmCompileContext, "probe", dummyFn);
    llvmIRBuilder->SetInsertPoint(tempBB);

    // Suppress errors and attempt codegen
    wasError = false;
    messageSystem::suppressErrors = true;
    (argNode->*(argNode->codegen))(pass);
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

    llvm::Value* result = ConstantInt::get(llvm::Type::getInt1Ty(*llvmCompileContext), compiled ? 1 : 0);
    asaType = new AsaTypeInstance(getAsaBaseTypeFromName("bool"), false, true, "bool", 0);
    //if (!asaType)
    //    asaType = new AsaTypeInstance(result->getType());
    //else
    //    asaType->baseLLVMType = result->getType();

    messageSystem::endBlock();
    pushCompilerDirectiveCall("compiles", this);
    return result;
}

// OPENCODE:
// Emits the LLVM module to an object (.o) file. Initializes all LLVM targets,
// looks up the host target triple, creates a TargetMachine, sets the module
// data layout, then runs a legacy pass manager with the assembly printer to
// emit the object file. Returns 0 on success, 1 on failure.
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

// OPENCODE:
// Links the compiled IR into an executable. For optimized builds: runs opt
// (LLVM optimizer) on the text IR, then llc (LLVM compiler to assembly),
// then clang to link. For non-optimized builds: invokes clang directly on
// the IR. Passes linked libraries and static library paths. Returns 0 on
// success, 1 on failure.
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


// OPENCODE:
// Runs the new-pass-manager LLVM optimizer on all functions in the module.
// Maps the optimization level string (-O0..-O3, -Os, -Oz) to an
// OptimizationLevel enum, sets up the analysis managers, and runs the module
// pipeline. No-op if optimization is disabled.
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
