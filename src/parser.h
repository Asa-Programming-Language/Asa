#pragma once

#include "codegen.h"
#include "filemanager.h"
#include "messagehandler.h"
#include "pch.h"
#include "settings.h"
#include "strops.h"
#include "tokenizer.h"


#define MAX_AST_DEPTH 100

#define PRINT_SUBTOKENS(subTokens)                     \
    for (int ST = 0; ST < subTokens.size(); ST++)      \
        printf("%s ", subTokens[ST].tokenStr.c_str()); \
    printf("\n");

using namespace llvm;

void GO_BACK_TO_BEGINNING_OF_TERM(int& i);

extern bool wasError;

enum ASTNodeType {
    // Null type
    Nothing_Node,

    Identifier_Node,
    Integer_Node,
    SInt128_Type,
    SInt64_Type,
    SInt32_Type,
    SInt16_Type,
    SInt8_Type,
    Begin_Unsigned_Integers,
    UInt128_Type,
    UInt64_Type,
    UInt32_Type,
    UInt16_Type,
    UInt8_Type,
    Byte_Type,
    Boolean_Node,
    Double_Type,
    Float_Node,
    Half_Type,
    String_Node,
    String_Constant_Node,
    Character_Constant_Node,
    Undefined_Initializer_Node,
    Default_Initializer_Node,
    Initial_Initializer_Node,
    Type_Node,
    Void_Node,
    Any_Type,

    Module_Scope,

    Redefined_Operator_Expr,
    Operator_Type_Node,
    Expression_Term,
    Expression_Paren_Term,
    Expression_Statement,
    Expression_Plus,
    Expression_Minus,
    Expression_Times,
    Expression_Divide,
    Expression_Modulo,
    Bitwise_And,
    Bitwise_Or,
    Bitwise_Xor,
    Bitwise_Not,
    Bitwise_Shift_Left,
    Bitwise_Shift_Right,
    Pipe_Operation,
    Pipe_Placeholder,
    Comma_Node,
    Colon_Separator_Node,

    Range_Node,

    Argument_List,

    Function_Call,

    If_Statement_Node,

    Else_Statement_Node,

    For_Statement_Node,

    While_Statement_Node,

    Struct_Define_Node,
    Struct_Type,

    Enum_Define_Node,

    Module_Define_Node,

    Operator_Overload_Node,

    Access_Operation,
    Member_Access,
    Attribute_Access,
    Reference_Operation,
    Const_Keyword,
    Exact_Type_Node,
    Address_Of_Operation,
    Dereference_Operation,
    Pointer_Node,

    Return_Node,
    Result_Node,
    Continue_Node,
    Break_Node,
    Goto_Node,

    Throw_Node,
    Throw_Caller_Node,

    Iterator,
    Condition,
    Scope_Body,

    Compare_Equal,
    Compare_Not,
    Compare_Less,
    Compare_Greater,
    Compare_LessEqual,
    Compare_GreaterEqual,

    Logical_And,
    Logical_Or,
    Logical_Not,

    Compiler_Define,
    Compiler_Define_Function,
    Compiler_Define_Cast,
    Compiler_Define_Struct,
    Compiler_Define_Enum,
    Compile_Time_Directive,
    Labeled_Loop,
    Arguments,
    Compiler_Modifiers,

    Attribute_Node,
    Standalone_Attribute_Node,
    Comment_Node,

    Variants_Node,
    Variant_Param_Node,

    Fully_Defined,

    // Nothing below this
    LastASTNodeType
};

extern std::map<TokenType, ASTNodeType> binaryOperatorExType;

const std::string ASTNodeTypeStrings[] = {
    "Nothing_Node",

    "Identifier_Node",
    "Integer_Node",
    "SInt128_Type",
    "SInt64_Type",
    "SInt32_Type",
    "SInt16_Type",
    "SInt8_Type",
    "Begin_Unsigned_Integers",
    "UInt128_Type",
    "UInt64_Type",
    "UInt32_Type",
    "UInt16_Type",
    "UInt8_Type",
    "Byte_Type",
    "Boolean_Node",
    "Double_Type",
    "Float_Node",
    "Half_Type",
    "String_Node",
    "String_Constant_Node",
    "Character_Constant_Node",
    "Undefined_Initializer_Node",
    "Default_Initializer_Node",
    "Initial_Initializer_Node",
    "Type_Node",
    "Void_Node",
    "Any_Type",

    "Module_Scope",

    "Redefined_Operator_Expr",
    "Operator_Type_Node",
    "Expression_Term",
    "Expression_Paren_Term",
    "Expression_Statement",
    "Expression_Plus",
    "Expression_Minus",
    "Expression_Times",
    "Expression_Divide",
    "Expression_Modulo",
    "Bitwise_And",
    "Bitwise_Or",
    "Bitwise_Xor",
    "Bitwise_Not",
    "Bitwise_Shift_Left",
    "Bitwise_Shift_Right",
    "Pipe_Operation",
    "Pipe_Placeholder",
    "Comma_Node",
    "Colon_Separator_Node",

    "Range_Node",

    "Argument_List",

    "Function_Call",

    "If_Statement_Node",

    "Else_Statement_Node",

    "For_Statement_Node",

    "While_Statement_Node",

    "Struct_Define_Node",
    "Struct_Type",

    "Enum_Define_Node",

    "Module_Define_Node",

    "Operator_Overload_Node",

    "Access_Operation",
    "Member_Access",
    "Attribute_Access",
    "Reference_Operation",
    "Const_Keyword",
    "Exact_Type_Node",
    "Address_Of_Operation",
    "Dereference_Operation",
    "Pointer_Node",

    "Return_Node",
    "Result_Node",
    "Continue_Node",
    "Break_Node",
    "Goto_Node",

    "Throw_Node",
    "Throw_Caller_Node",

    "Iterator",
    "Condition",
    "Scope_Body",

    "Compare_Equal",
    "Compare_Not",
    "Compare_Less",
    "Compare_Greater",
    "Compare_LessEqual",
    "Compare_GreaterEqual",

    "Logical_And",
    "Logical_Or",
    "Logical_Not",

    "Compiler_Define",
    "Compiler_Define_Function",
    "Compiler_Define_Cast",
    "Compiler_Define_Struct",
    "Compiler_Define_Enum",
    "Compile_Time_Directive",
    "Labeled_Loop",
    "Arguments",
    "Compiler_Modifiers",

    "Attribute_Node",
    "Standalone_Attribute_Node",
    "Comment_Node",

    "Variants_Node",
    "Variant_Param_Node",

    "Fully_Defined",
};

struct ASTNode;
struct AsaBaseType;
struct AsaTypeInstance;

struct AsaVariableValue {
    std::string name;
    // TODO: Replace `typeString` usage with `asaTypeInstance->strVal`
    //std::string typeString;
    AsaTypeInstance* asaTypeInstance = nullptr;
    bool isFunctionArgument = false;
    // TODO: Replace the following usages with their `asaTypeInstance` equivalents
    bool isConstant = false;
    bool isReference = false;
    bool isUndefined = false;

    llvm::Value* llvmValue;
    ASTNode* declNode = nullptr;
    ASTNode* initialValueNode = nullptr;  // compile-time constant initializer, for `initial`
    AsaVariableValue(std::string name, AsaTypeInstance* asaTypeInstance, llvm::Value* llvmValue, bool isFunctionArgument = false, bool isReference = false)
        : name(name),
          asaTypeInstance(asaTypeInstance),
          llvmValue(llvmValue),
          isFunctionArgument(isFunctionArgument),
          isReference(isReference) {};
};
// An Asa value for function arguments
struct AsaArgumentVariableValue : AsaVariableValue {
    // Value for if the function arg has a default value, like: `foo :: (x : int = 5){}`
    // The default value stored as an ASTNode:
    ASTNode* defaultValueNode = nullptr;  // Expression_Term node (already resolved at definition site)
    // TODO: Is this the best way to do it?
    std::vector<asaToken*> defaultRawTokens;  // raw tokens for re-parsing at call site

    // TODO: Update this to allow for many ABI formats
    // Extern ABI coercion: how to split this struct arg for the x86-64 SysV ABI
    int8_t externCoercionCount = 0;      // 0=none, N=split into N primitives (doubles or i64s)
    bool externCoercionIsFloat = false;  // true=doubles (SSE/XMM), false=i64s (INTEGER)

    // Inherit the constructor
    using AsaVariableValue::AsaVariableValue;
};

// Struct defining a data type in Asa
struct AsaBaseType {
    const std::string typeName;
    Type* baseLLVMType = nullptr;
    bool isDefined = false;

    bool isSigned = false;
    // If this is a signed type, then also store a pointer to the unsigned version
    AsaBaseType* unsignedVersion = nullptr;

    bool isStruct = false;

    ASTNodeType astNodeType = Struct_Type;

    AsaBaseType(std::string name, ASTNodeType astNodeType, llvm::Type* baseLLVMType, bool isSigned = false, bool isStruct = false)
        : typeName(name), astNodeType(astNodeType), baseLLVMType(baseLLVMType), isSigned(isSigned), isStruct(isStruct), isDefined(true) {};
    AsaBaseType(std::string name, ASTNodeType astNodeType, bool isSigned = false, bool isStruct = false)
        : typeName(name), astNodeType(astNodeType), isSigned(isSigned), isStruct(isStruct), isDefined(false) {};
    AsaBaseType() {};

    AsaBaseType* getUnsigned();
};
//struct AsaBaseStruct : AsaBaseType {
//    std::vector<AsaTypeInstance*> memberTypes = {};
//    std::vector<std::string> memberNames = {};
//};
// TODO: Is this the best way to do it? two maps with 2 keys?
extern std::unordered_map<std::string, AsaBaseType*> asaBaseTypes;
extern std::unordered_map<llvm::Type*, AsaBaseType*> asaBaseTypesFromLLVM;
AsaBaseType* CreateNewAsaType(std::string name, ASTNodeType astType, llvm::Type* (*baseLLVMTypeFn)(), bool isSigned);
AsaBaseType* CreateNewAsaType(std::string name, ASTNodeType astType, llvm::Type* baseLLVMType, bool isSigned);
AsaBaseType* CreateNewAsaType(std::string name, ASTNodeType astType);
AsaTypeInstance* CreateAsaTypeInstanceFromASTNode(ASTNode*& node);
AsaTypeInstance* CreateAsaTypeInstanceFromString(std::string typeStr);
AsaTypeInstance* CreateVoidAsaTypeInstance();

ASTNode* ExtractTypeModifiersFromType(ASTNode* node, std::vector<ASTNodeType>& modifiers);


// Struct defining an instance of a type in Asa. For example, `x : int;` is borrowing from the `AsaBaseType` `"int"`
struct AsaTypeInstance {
    // A pointer to the base Asa type
    AsaBaseType* baseType = nullptr;
    // The LLVM Type object, after type modifiers are applied
    llvm::Type* llvmType = nullptr;

    bool inferredType = false;
    std::string strVal = "";
    std::vector<ASTNodeType> typeModifiers = std::vector<ASTNodeType>();
    uint8_t pointerLevel = 0;

    // TODO: Make all of the below values use the above vector
    bool isRef = false;
    bool isConst = false;

    //AsaTypeInstance(Type* baseType, bool isRef, bool isConst, std::string strVal, uint8_t pointerLevel)
    //    : baseLLVMType(baseType), isRef(isRef), isConst(isConst), strVal(strVal), pointerLevel(pointerLevel) {};
    //AsaTypeInstance(Type* baseType)
    //    : baseLLVMType(baseType) {};

    AsaTypeInstance(AsaBaseType* baseType, bool isRef, bool isConst, std::string strVal, uint8_t pointerLevel)
        : baseType(baseType), isRef(isRef), isConst(isConst), strVal(strVal), pointerLevel(pointerLevel) {};
    //AsaTypeInstance(AsaBaseType* baseType)
    //    : baseType(baseType) {};
    //AsaTypeInstance(llvm::Type* llvmType){
    //};
    AsaTypeInstance() {};

    // Constructor to copy the values from one AsaTypeInstance to another
    AsaTypeInstance(AsaTypeInstance* other)
        : baseType(other->baseType),
          llvmType(other->llvmType),
          inferredType(other->inferredType),
          strVal(other->strVal),
          typeModifiers(other->typeModifiers),
          pointerLevel(other->pointerLevel),
          isRef(other->isRef),
          isConst(other->isConst) {};

    AsaTypeInstance* dereference(ASTNode* dereferenceNode);
    AsaTypeInstance* getPointerTo(ASTNode* addressOfNode);

    bool hasModifier(ASTNodeType modifierType)
    {
        for (int i = 0; i < this->typeModifiers.size(); i++) {
            ASTNodeType m = this->typeModifiers[i];
            // If this modifier is the one we are checking:
            if (m == modifierType) {
                switch (m) {
                    // `exact` applies to the entire type, regardless of order
                    case Exact_Type_Node:
                        return true;
                    // `const` only applies to everything after it. So for it to
                    // be `true`, it must be at index 0
                    case Const_Keyword:
                        return i == 0;
                    // `ref` applies to the entire type, regardless of order  TODO: Should this be liek this?
                    case Reference_Operation:
                        return true;
                    // `*` pointer applies to everything after it. But it can be
                    // true even if another modifier precedes it
                    case Pointer_Node:
                        return true;

                    default:
                        break;
                }
            }
        }
        return false;
    }

    // Reorders type modifiers to be in the same order as equivalent modifier patterns and respect effects.
    // For example, the type: `* const * int` is not the same thing as `const **int`.
    // But, the type `* ref int` is the same thing as `ref * int`
    // So this function pushes all of the global modifiers to the front, and leaves the ordering of the others the same.
    //
    // TODO: This function does not verify that multiple global modifiers are not applied, like `ref * const ref int`
    //       That makes it so that until a check is implemented, `ref ref * const int` is the same as `ref * const int`
    void normalizeModifiers()
    {
        std::vector<ASTNodeType> normalized = this->typeModifiers;
        int globalModifierCount = 0;

        for (int i = 0; i < this->typeModifiers.size(); i++) {
            ASTNodeType m = this->typeModifiers[i];
            switch (m) {
                // `exact` is global
                case Exact_Type_Node:
                // `ref` is global
                case Reference_Operation: {
                    // Remove the modifer from the normalized vector:
                    normalized.erase(normalized.begin() + i);
                    // Add it back at the beginning
                    normalized.insert(normalized.begin(), m);
                    // Increment global modifier count to know how many to sort
                    globalModifierCount++;
                }
                // All other type modifiers, like `const` and `*` depend on ordering
                default:
                    break;
            }
        }

        // Now sort the beginning of the `normalized` vector, but only the global modifiers
        std::partial_sort(normalized.begin(), normalized.begin() + globalModifierCount, normalized.begin() + globalModifierCount);

        // Now swap old `typeModifiers` with the normalized one
        this->typeModifiers = normalized;
    }

    // Function to get the mangled string name of the AsaTypeInstance.
    // For example, the type: `* const ref * exact int`
    //     First, the modifiers get sorted: `ref exact * const * int`
    //     Then, it gets converted into a string: `ref.exact.ptr.const.ptr.int`
    std::string getMangledName()
    {
        std::string mangledString = "";

        // First normalize the modifier order
        // TODO: Maybe put this in the constructor so it is only called once
        this->normalizeModifiers();

        // Next, add type modifiers to the mangled string, like: `ref.ptr.const.ptr.`
        for (int i = 0; i < this->typeModifiers.size(); i++) {
            ASTNodeType m = this->typeModifiers[i];
            switch (m) {
                case Exact_Type_Node:
                    mangledString += "exact.";  // TODO: Should this be included in the mangled type name?
                    break;

                case Const_Keyword:
                    mangledString += "const.";
                    break;

                case Reference_Operation:
                    mangledString += "ref.";
                    break;

                case Pointer_Node:
                    mangledString += "ptr.";
                    break;

                default:
                    break;
            }
        }

        // Then add the base type name, like: `ref.ptr.const.ptr.int`
        mangledString += this->baseType->typeName;

        return mangledString;
    }
};

//// Compare if two Asa base types are equivalent by value
//bool operator==(const AsaBaseType& l, const AsaBaseType& r)
//{
//    // TODO: Not sure if just comparing the llvm type is enough to know equivalence
//    if (l.baseLLVMType != r.baseLLVMType)
//        return false;
//    return true;
//}

// Compare two AsaBaseTypes for equivalency. If one or both of them were inferred from an LLVM type,
// then one or both of them may be signed, and in that case signs should be ignored
inline bool baseTypesEqual(AsaBaseType*& l, AsaBaseType*& r, bool inferredType = false)
{
    // AsaBaseTypes are unique, so we can just compare their pointers directly, rather than their values:

    // If not inferred, just check for equivalency normally:
    if (!inferredType) {
        return l == r;
    }
    // Otherwise, compare them without signs:
    else {
        return l->getUnsigned() == r->getUnsigned();
    }
}

// Comparision operator to compare if two type instances have an exactly equivalent value
// Handles the case where one or both are inferred
inline bool operator==(AsaTypeInstance& l, AsaTypeInstance& r)
{
    // TODO: Update this to only compare the components of the struct that matter

    if (!baseTypesEqual(l.baseType, r.baseType, l.inferredType || r.inferredType))
        return false;
    if (l.typeModifiers != r.typeModifiers)
        return false;
    if (l.pointerLevel != r.pointerLevel)
        return false;

    return true;
}

struct ASTNode {
    ASTNode* parentNode = nullptr;
    std::vector<ASTNode*> childNodes = std::vector<ASTNode*>();
    ASTNodeType nodeType = Nothing_Node;
    asaToken* token = new asaToken();
    int lineNumber = 0;
    uint16_t depth = 0;
    bool isRef = false;
    bool isConst = false;
    bool isExtern = false;
    bool lvalue = false;
    // TODO: Many of the following variables should not be node-level
    bool isPostfix = false;
    std::string label = "";
    std::string externSymbolName = "";       // when non-empty, the actual C symbol to link (overrides fnName for LLVM)
    bool isModuleScope = false;              // true for Compiler_Define nodes representing named modules
    bool importedChildren = false;           // true for #import modules whose children are visible unqualified
    std::string enclosingModule = "";        // set on imported nodes to record source module name
    bool currentNodeDoneGenerating = false;  // TODO: Make codegen mark each node that completely finishes generating as done
    bool isValueBlock = false;               // true for Scope_Body nodes that are value-returning { result ...; } expressions
    bool isPoisoned = false;
    bool returnsASTNode = false;
    bool tracksVariableUsage = false;
    int variableReads = 0;
    int variableWrites = 0;
    asaToken* closingToken = nullptr;  // closing delimiter token (e.g. }) stored for token range tracking
    //Type* llvmType;
    //Type* baseType = nullptr;
    AsaTypeInstance* asaType = nullptr;
    // Add leaf nodes here as they are still yet to be used.
    std::vector<ASTNode*> leafNodes = std::vector<ASTNode*>();
    std::vector<ASTNode*> attributes = std::vector<ASTNode*>();  // TODO: Maybe make this an ordered map
    ASTNode* docComment = nullptr;
    std::vector<asaToken*> defaultRawTokens;  // raw tokens for default param expr, for call-site re-parsing

    std::unordered_map<std::string, ASTNode*> interpreterScopeValues = std::unordered_map<std::string, ASTNode*>();

    std::map<std::string, AsaVariableValue*> namedValues = std::map<std::string, AsaVariableValue*>();
    std::map<std::string, ASTNode*> compilerDefinitions = std::map<std::string, ASTNode*>();

    bool compareTokens = false;
    ASTNode* (ASTNode::*resolveASTNode)(int pass) = nullptr;

    // Codegen functions for each node type:

    void* generateConstant(int pass = 0);
    void* generateVariableExpression(int pass = 0);
    void* generateReturn(int pass = 0);
    void* generateResult(int pass = 0);
    void* generateBreak(int pass = 0);
    void* generateContinue(int pass = 0);
    void* generateExpression(int pass = 0);
    void* generateExpressionStatement(int pass = 0);
    void* generateIterator(int pass = 0);
    void* generateUnaryExpression(int pass = 0);
    void* generateBinaryExpression(int pass = 0);
    void* generatePipePlaceholder(int pass = 0);
    void* generateAccessOperation(int pass = 0);
    void* generateMemberAccess(int pass = 0);
    void* generateScopeBody(int pass = 0);
    void* generateIf(int pass = 0);
    void* generateStruct(int pass);
    void* generateEnum(int pass);
    void* generateLabeledLoop(int pass = 0);
    void* generateFor(int pass = 0);
    void* generateWhile(int pass = 0);
    void* generatePrototype(int pass = 0);
    void* generateFunction(int pass = 0);
    void* generateCast(int pass = 0);
    void* generateBitcast(int pass = 0);
    void* generateThrow(int pass = 0);
    void* generateThrowCaller(int pass = 0);
    void* generateStructInstance(int pass = 0);
    void* generateCallerFilepathDirective(int pass = 0);
    void* generateCallerLineNumDirective(int pass = 0);
    void* generateCallerLineDirective(int pass = 0);
    void* generateCallExpression(int pass = 0);
    void* generateIncDecrement(int pass = 0);
    void* generateNothing(int pass = 0);
    void* generateNoOp(int pass = 0);
    void* generateCompilerFlagDirective(int pass = 0);
    void* generateCompilerGetFlagDirective(int pass = 0);
    void* generateCompilerStackPushDirective(int pass = 0);
    void* generateCompilerStackPopDirective(int pass = 0);
    void* generateCompilerStackLastDirective(int pass = 0);
    void* generateCompilerPrintASTDirective(int pass = 0);
    void* generateCompilerPrintDirective(int pass = 0);
    void* generateCompilerPrintLineDirective(int pass = 0);
    void* generateCompilerIfDirective(int pass = 0);
    void* generateCompilerErrorDirective(int pass = 0);
    void* generateCompilerWarningDirective(int pass = 0);
    void* generateCompilerSetAttributeDirective(int pass = 0);
    void* generateLibraryDirective(int pass = 0);
    void* generateLibraryStaticDirective(int pass = 0);
    void* generateNameofDirective(int pass = 0);
    void* generateTypeofDirective(int pass = 0);
    void* generateSizeofDirective(int pass = 0);
    void* generateCompilesDirective(int pass = 0);
    ASTNode* resolveCompilerStackLastASTNode(int pass = 0);
    ASTNode* resolveCompilerContextASTNode(int pass = 0);
    ASTNode* resolveCompilerDefinitionASTNode(int pass = 0);
    ASTNode* resolveCompilerParentASTNode(int pass = 0);
    ASTNode* resolveCompilerFuncASTNode(int pass = 0);

    void* (ASTNode::*codegen)(int pass) = &ASTNode::generateNothing;

    // Helper functions:

    llvm::Value* generateOperatorOverloadCall(llvm::Value* L, llvm::Value* R);
    bool checkForOperatorOverload(llvm::Value* L, llvm::Value* R);
    llvm::Value* generatePointerBinaryOp(llvm::Value* L, llvm::Value* R);
    llvm::Value* generateFloatBinaryOp(llvm::Value* L, llvm::Value* R);
    llvm::Value* generateIntegerBinaryOp(llvm::Value* L, llvm::Value* R);

    //std::unique_ptr<PrototypeAST> Proto;
    //std::unique_ptr<ExprAST> Body;

    ASTNode() {}
    ASTNode(ASTNodeType nType)
    {
        nodeType = nType;
    }
    ASTNode(ASTNodeType nType, std::vector<ASTNode*> cNodes)
    {
        childNodes = cNodes;
        nodeType = nType;
    }
    ASTNode(std::vector<ASTNode*> cNodes, ASTNodeType nType)
    {
        childNodes = cNodes;
        nodeType = nType;
    }
    ASTNode(ASTNodeType nType, std::vector<ASTNode*> cNodes, asaToken* t, bool cmpTokens = false)
    {
        childNodes = cNodes;
        nodeType = nType;
        token = t;
        compareTokens = cmpTokens;
    }
    ASTNode(std::vector<ASTNode*> cNodes, ASTNodeType nType, asaToken* t, bool cmpTokens = false)
    {
        childNodes = cNodes;
        nodeType = nType;
        token = t;
        compareTokens = cmpTokens;
    }

    bool empty()
    {
        return childNodes.size() == 0;
    }

    bool operator==(ASTNode other)
    {
        // Make sure children are the same as well
        if (childNodes.size() == other.childNodes.size())
            for (int i = 0; i < childNodes.size(); i++) {
                if ((*(childNodes[i]) == *(other.childNodes[i])) == false)
                    return false;
            }
        else
            return false;

        // If child nodes match, then we have to check the current node
        if (compareTokens)
            if (!(*token == *(other.token)))
                return false;
        if (nodeType == other.nodeType /* && token == other.token &&
            lineNumber == other.lineNumber*/
        )
            return true;

        // Otherwise false
        return false;
    }
};


extern ASTNode* rootNode;
extern std::vector<ASTNode*> importedNodes;

int beginParse(const std::vector<asaToken*>& tokens);
ASTNode* generateAST(const std::vector<asaToken*>& tokens, int depth = 0, ASTNode* parentNodePtr = nullptr, bool isScopeBody = false);
void orderASTOperations(ASTNode* startNode);
const std::string ASTNodeTypeAsString(ASTNodeType t);
int printAST(ASTNode* startNode, int depth = 0);
void fixPrecedence(ASTNode*& node);
void unifyNodes(ASTNode*& node);
void normalizeQualifiedCompilerDefinitions(ASTNode*& node);
void optimizeASTNode(ASTNode*& node);
void resolveCompileTimeDirectives(ASTNode*& node, std::string moduleCtx = "", std::string funcCtx = "");
void resolveAttributeAccess(ASTNode*& node);
void checkAttributeCompatibility(ASTNode* node);
bool hasAttribute(ASTNode* node, std::string attributeName);
std::string getAttributeValue(ASTNode* node, std::string attributeName);
std::string getInheritedAttributeValue(ASTNode* node, std::string attributeName);
void addFileIncludes(ASTNode*& node);
void addModuleImports(ASTNode*& node);
void assignParentNodes(ASTNode*& node, int depth = 0);
tokenRange getASTTokenRange(ASTNode* node);
void printSourceLines(asaToken* startToken, asaToken* endToken, std::string underlineColor, std::string underlineMessage);
void printTokenMarked(tokenRange tokRange, std::string msgString = "", int sourceLineNumber = 0, const char* fileName = "");
void printTokenError(tokenRange tokRange, std::string errorString = "", int sourceLineNumber = 0, const char* fileName = "");
void printTokenWarning(tokenRange tokRange, std::string errorString = "", int sourceLineNumber = 0, const char* fileName = "");
void findUnusedLeafNodes(ASTNode*& node);
void printModuleLoaded(std::string& moduleName, std::string& modulePath);
int generateOutputCode(ASTNode*& node, int depth = 0, int pass = 0);
