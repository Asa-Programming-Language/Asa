#include "compiler_directives.h"

#include "codegen.h"

static ASTNode* createSyntheticNode(ASTNodeType nodeType, TokenType tokenType, const std::string& tokenText, ASTNode* sourceNode)
{
    ASTNode* node = new ASTNode();
    node->nodeType = nodeType;
    node->token = new asaToken(tokenText, tokenType);
    node->codegen = &ASTNode::generateConstant;

    if (sourceNode) {
        node->parentNode = sourceNode;
        node->lineNumber = sourceNode->lineNumber;
        if (sourceNode->token) {
            node->token->lineNumber = sourceNode->token->lineNumber;
            node->token->indexInLine = sourceNode->token->indexInLine;
            node->token->lineValue = sourceNode->token->lineValue;
            node->token->filePath = sourceNode->token->filePath;
            node->token->lineIndent = sourceNode->token->lineIndent;
        }
    }

    return node;
}

void pushCompilerDirectiveCall(const std::string& directiveName, ASTNode* sourceNode)
{
    compilerStacks["directive_calls"].push(createSyntheticNode(String_Constant_Node, String, "\"" + directiveName + "\"", sourceNode));
}

void pushCompilerControlFlowResult(bool result, ASTNode* sourceNode)
{
    compilerStacks["control_flow_result"].push(createSyntheticNode(Boolean_Node, result ? True_Literal : False_Literal, result ? "true" : "false", sourceNode));
}

struct DirectiveValue {
    bool resolved = false;
    double number = 0.0;
    bool isString = false;
    std::string stringValue = "";

    bool truthy() const
    {
        return resolved && (isString ? !stringValue.empty() : number != 0.0);
    }
};

struct CustomCompilerDirective {
    std::vector<std::string> parameters;
    ASTNode* body = nullptr;
};

struct CompilerDirectiveInvocation {
    std::unordered_map<std::string, ASTNode*> arguments;
    ASTNode* contextNode = nullptr;
    bool hasReturn = false;
    ASTNode* returnNode = nullptr;
};

static std::unordered_map<std::string, CustomCompilerDirective> customCompilerDirectives;

static ASTNode* resolveDirectiveASTTarget(ASTNode* targetNode, CompilerDirectiveInvocation* invocation = nullptr);

static void setCompilerDirectiveFlagAlias(const std::string& name, bool value)
{
    compilerDirectiveFlags[name] = value;
    compilerDirectiveFlags[ToLower(name)] = value;
    compilerDirectiveFlags[ToUpper(name)] = value;
}

static void seedCompilerDirectiveFlags()
{
    compilerDirectiveFlags.clear();
    for (const auto& flag : commandLineCompilerDirectiveFlags)
        compilerDirectiveFlags[flag.first] = flag.second;

    if (compilerFlags == Flags_CompilerDebug) {
        setCompilerDirectiveFlagAlias("compilerdebug", true);
        setCompilerDirectiveFlagAlias("compiler_debug", true);
    }
    if (compilerFlags == Flags_Debug)
        setCompilerDirectiveFlagAlias("debug", true);
    if (compilerFlags == Flags_PrintAST)
        setCompilerDirectiveFlagAlias("printast", true);
    if (compilerFlags == Flags_Force_Enable_Color)
        setCompilerDirectiveFlagAlias("color", true);
}

static ASTNode* unwrapExpressionNode(ASTNode* node)
{
    while (node && (node->nodeType == Expression_Term || node->nodeType == Expression_Paren_Term) && node->childNodes.size() == 1)
        node = node->childNodes[0];
    return node;
}

static void collectDirectiveArgs(ASTNode* arg, std::vector<ASTNode*>& args)
{
    while (arg && arg->nodeType == Expression_Term && arg->childNodes.size() == 1)
        arg = arg->childNodes[0];
    if (!arg)
        return;
    if (arg->nodeType == Comma_Node) {
        for (auto* child : arg->childNodes)
            collectDirectiveArgs(child, args);
        return;
    }
    args.push_back(arg);
}

static std::vector<ASTNode*> getDirectiveCallArgs(ASTNode* directiveNode)
{
    std::vector<ASTNode*> args;
    if (!directiveNode || directiveNode->childNodes.size() < 2)
        return args;

    ASTNode* argContainer = directiveNode->childNodes[1];
    if (!argContainer)
        return args;

    if (argContainer->nodeType == Scope_Body || argContainer->nodeType == Arguments) {
        for (auto* child : argContainer->childNodes)
            collectDirectiveArgs(child, args);
    }
    else {
        collectDirectiveArgs(argContainer, args);
    }

    return args;
}

static ASTNode* resolveInvocationNode(ASTNode* node, CompilerDirectiveInvocation* invocation)
{
    node = unwrapExpressionNode(node);
    if (!node)
        return nullptr;

    if (node->nodeType == Identifier_Node && node->token && invocation) {
        auto arg = invocation->arguments.find(node->token->tokenStr);
        if (arg != invocation->arguments.end())
            return arg->second;
    }

    if (node->nodeType == Compile_Time_Directive && !node->childNodes.empty() &&
        node->childNodes[0]->token && node->childNodes[0]->token->tokenStr == "context") {
        if (!invocation) {
            messageSystem::error("#context is only available inside a custom compiler directive invocation", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
            return nullptr;
        }
        if (!invocation->contextNode) {
            messageSystem::error("#context resolved to null", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
            return nullptr;
        }
        return invocation->contextNode;
    }

    return node;
}

static void collectIdentifierNames(ASTNode* node, std::vector<std::string>& names)
{
    while (node && node->nodeType == Expression_Term && node->childNodes.size() == 1)
        node = node->childNodes[0];
    if (!node)
        return;

    if (node->nodeType == Identifier_Node && node->token) {
        names.push_back(node->token->tokenStr);
        return;
    }

    for (auto* child : node->childNodes)
        collectIdentifierNames(child, names);
    for (auto* leaf : node->leafNodes)
        collectIdentifierNames(leaf, names);
}

static ASTNode* cloneDirectiveBodyNode(ASTNode* src, ASTNode* parentNode = nullptr)
{
    if (!src)
        return nullptr;

    ASTNode* copy = new ASTNode();
    copy->parentNode = parentNode;
    copy->nodeType = src->nodeType;
    copy->token = src->token ? new asaToken(*src->token) : nullptr;
    copy->lineNumber = src->lineNumber;
    copy->codegen = src->codegen;
    copy->resolveASTNode = src->resolveASTNode;
    copy->returnsASTNode = src->returnsASTNode;
    copy->closingToken = src->closingToken ? new asaToken(*src->closingToken) : nullptr;
    copy->isExtern = src->isExtern;
    copy->isModuleScope = src->isModuleScope;
    copy->importedChildren = src->importedChildren;
    copy->enclosingModule = src->enclosingModule;
    copy->currentNodeDoneGenerating = false;
    copy->isValueBlock = src->isValueBlock;
    copy->isPoisoned = src->isPoisoned;
    copy->isPostfix = src->isPostfix;
    copy->label = src->label;
    copy->externSymbolName = src->externSymbolName;

    if (src->asaType)
        copy->asaType = std::move(src->asaType);
        //copy->asaType = new AsaTypeInstance(src->asaType->baseLLVMType, src->asaType->isRef, src->asaType->isConst, src->asaType->strVal, src->asaType->pointerLevel);

    copy->docComment = cloneDirectiveBodyNode(src->docComment, copy);

    for (auto* attr : src->attributes)
        copy->attributes.push_back(cloneDirectiveBodyNode(attr, copy));
    for (auto* child : src->childNodes)
        copy->childNodes.push_back(cloneDirectiveBodyNode(child, copy));
    for (auto* leaf : src->leafNodes)
        copy->leafNodes.push_back(cloneDirectiveBodyNode(leaf, copy));
    for (const auto& [name, definition] : src->compilerDefinitions)
        copy->compilerDefinitions[name] = cloneDirectiveBodyNode(definition, copy);

    return copy;
}

static bool readCompilerDirectiveFlagName(ASTNode* node, std::string& outName)
{
    node = unwrapExpressionNode(node);
    if (node && node->nodeType == Comma_Node && !node->childNodes.empty())
        node = unwrapExpressionNode(node->childNodes[0]);

    if (!node || !node->token ||
        (node->nodeType != Identifier_Node && node->nodeType != String_Node && node->nodeType != String_Constant_Node))
        return false;

    outName = node->nodeType == Identifier_Node ? node->token->tokenStr : decodeQuotedStringToken(node->token);
    return !outName.empty();
}

static ASTNode* findCompilerDefinitionForDirective(ASTNode* node, const std::string& name)
{
    ASTNode* scope = node;
    while (scope) {
        auto it = scope->compilerDefinitions.find(name);
        if (it != scope->compilerDefinitions.end())
            return it->second;
        scope = scope->parentNode;
    }
    return nullptr;
}

static DirectiveValue evaluateDirectiveValue(ASTNode* node, CompilerDirectiveInvocation* invocation = nullptr)
{
    node = resolveInvocationNode(node, invocation);
    if (!node)
        return {};

    switch (node->nodeType) {
        case Identifier_Node: {
            if (!node->token)
                return {};
            ASTNode* definition = findCompilerDefinitionForDirective(node, node->token->tokenStr);
            if (!definition || definition == node)
                return {};
            return evaluateDirectiveValue(definition, invocation);
        }
        case Boolean_Node:
            return {true, node->token && node->token->tokenStr == "true" ? 1.0 : 0.0};
        case Integer_Node:
            return {true, node->token ? (double)std::stoll(node->token->tokenStr) : 0.0};
        case Float_Node:
            return {true, node->token ? std::stod(node->token->tokenStr) : 0.0};
        case String_Node:
        case String_Constant_Node:
            return {true, 0.0, true, node->token ? decodeQuotedStringToken(node->token) : ""};
        case Logical_Not: {
            if (node->childNodes.empty())
                return {};
            DirectiveValue value = evaluateDirectiveValue(node->childNodes[0], invocation);
            return value.resolved ? DirectiveValue {true, value.truthy() ? 0.0 : 1.0} : DirectiveValue {};
        }
        case Logical_And:
        case Logical_Or: {
            if (node->childNodes.size() < 2)
                return {};
            DirectiveValue left = evaluateDirectiveValue(node->childNodes[0], invocation);
            if (!left.resolved)
                return {};
            if (node->nodeType == Logical_And && !left.truthy())
                return {true, 0.0};
            if (node->nodeType == Logical_Or && left.truthy())
                return {true, 1.0};
            DirectiveValue right = evaluateDirectiveValue(node->childNodes[1], invocation);
            return right.resolved ? DirectiveValue {true, right.truthy() ? 1.0 : 0.0} : DirectiveValue {};
        }
        case Compare_Equal:
        case Compare_Not:
        case Compare_Less:
        case Compare_Greater:
        case Compare_LessEqual:
        case Compare_GreaterEqual: {
            if (node->childNodes.size() < 2)
                return {};
            DirectiveValue left = evaluateDirectiveValue(node->childNodes[0], invocation);
            DirectiveValue right = evaluateDirectiveValue(node->childNodes[1], invocation);
            if (!left.resolved || !right.resolved)
                return {};

            bool result = false;
            if (left.isString || right.isString) {
                if (!left.isString || !right.isString)
                    return {};
                if (node->nodeType == Compare_Equal)
                    result = left.stringValue == right.stringValue;
                else if (node->nodeType == Compare_Not)
                    result = left.stringValue != right.stringValue;
                else
                    return {};
            }
            else if (node->nodeType == Compare_Equal)
                result = left.number == right.number;
            else if (node->nodeType == Compare_Not)
                result = left.number != right.number;
            else if (node->nodeType == Compare_Less)
                result = left.number < right.number;
            else if (node->nodeType == Compare_Greater)
                result = left.number > right.number;
            else if (node->nodeType == Compare_LessEqual)
                result = left.number <= right.number;
            else if (node->nodeType == Compare_GreaterEqual)
                result = left.number >= right.number;
            return {true, result ? 1.0 : 0.0};
        }
        case Compile_Time_Directive: {
            if (node->childNodes.empty() || !node->childNodes[0]->token)
                return {};
            const std::string& directiveName = node->childNodes[0]->token->tokenStr;
            if (directiveName == "stack_last" || directiveName == "stack_pop") {
                std::vector<ASTNode*> args = getDirectiveCallArgs(node);
                if (args.empty())
                    return {};
                std::string stackName;
                if (!readCompilerDirectiveFlagName(args[0], stackName))
                    return {};
                if (!compilerStacks.count(stackName) || compilerStacks[stackName].empty())
                    return {};
                ASTNode* stackNode = compilerStacks[stackName].top();
                if (directiveName == "stack_pop")
                    compilerStacks[stackName].pop();
                return evaluateDirectiveValue(stackNode, invocation);
            }
            if (directiveName == "getflag") {
                std::vector<ASTNode*> args = getDirectiveCallArgs(node);
                if (args.empty())
                    return {};
                std::string flagName;
                if (!readCompilerDirectiveFlagName(args[0], flagName))
                    return {};
                auto flag = compilerDirectiveFlags.find(flagName);
                return {true, flag != compilerDirectiveFlags.end() && flag->second ? 1.0 : 0.0};
            }
            if (directiveName == "nameof") {
                std::vector<ASTNode*> args = getDirectiveCallArgs(node);
                if (args.empty())
                    return {};
                ASTNode* namedNode = resolveDirectiveASTTarget(args[0], invocation);
                if (!namedNode || !namedNode->token)
                    return {};
                if (namedNode->nodeType == String_Node || namedNode->nodeType == String_Constant_Node)
                    return {true, 0.0, true, decodeQuotedStringToken(namedNode->token)};
                return {true, 0.0, true, namedNode->token->tokenStr};
            }
            return {};
        }
        default:
            return {};
    }
}

static ASTNode* resolveDirectiveASTTarget(ASTNode* targetNode, CompilerDirectiveInvocation* invocation)
{
    targetNode = resolveInvocationNode(targetNode, invocation);
    if (!targetNode)
        return nullptr;

    if (targetNode->returnsASTNode && targetNode->resolveASTNode) {
        ASTNode* resolvedNode = (targetNode->*(targetNode->resolveASTNode))(0);
        if (resolvedNode)
            return resolvedNode;
        if (wasError)
            return nullptr;
    }

    return targetNode;
}

static void replaceAttributeReference(ASTNode* node, ASTNode* oldAttr, ASTNode* newAttr)
{
    if (!node || !oldAttr || !newAttr)
        return;

    for (auto*& attr : node->attributes)
        if (attr == oldAttr)
            attr = newAttr;

    for (auto* child : node->childNodes)
        replaceAttributeReference(child, oldAttr, newAttr);
}

static bool applySetAttributeDirective(ASTNode* directiveNode, CompilerDirectiveInvocation* invocation = nullptr)
{
    messageSystem::startBlock(directiveNode, "Processing `#set_attribute` directive", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
    defer(messageSystem::endBlock());

    if (!directiveNode || directiveNode->childNodes.size() < 2 || directiveNode->childNodes[1]->childNodes.empty()) {
        messageSystem::error("#set_attribute requires AST node, attribute name, and value arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    std::vector<ASTNode*> args = getDirectiveCallArgs(directiveNode);

    if (args.size() < 3) {
        messageSystem::error("#set_attribute requires AST node, attribute name, and value arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    ASTNode* targetNode = resolveDirectiveASTTarget(args[0], invocation);
    if (!targetNode || targetNode->nodeType == Identifier_Node) {
        messageSystem::error("#set_attribute target did not resolve to an AST node", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    ASTNode* nameNode = unwrapExpressionNode(args[1]);
    if (!nameNode || !nameNode->token ||
        (nameNode->nodeType != String_Node && nameNode->nodeType != String_Constant_Node && nameNode->nodeType != Identifier_Node)) {
        messageSystem::error("#set_attribute attribute name must be a string literal or identifier", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    std::string attrName = nameNode->nodeType == Identifier_Node ? nameNode->token->tokenStr : decodeQuotedStringToken(nameNode->token);
    if (attrName.empty()) {
        messageSystem::error("#set_attribute attribute name cannot be empty", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    ASTNode* valueNode = unwrapExpressionNode(args[2]);
    bool isLiteralValue = valueNode &&
                          (valueNode->nodeType == Integer_Node ||
                              valueNode->nodeType == Float_Node ||
                              valueNode->nodeType == Boolean_Node ||
                              valueNode->nodeType == String_Node ||
                              valueNode->nodeType == String_Constant_Node);
    if (!valueNode || !valueNode->token || !isLiteralValue) {
        messageSystem::error("#set_attribute value must be a compile-time constant (int, float, bool, or string)", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    ASTNode* attr = new ASTNode();
    attr->nodeType = Attribute_Node;
    attr->token = new asaToken(attrName, Identifier, nameNode->token->lineNumber, nameNode->token->indexInLine, nameNode->token->lineValue, nameNode->token->filePath);
    attr->parentNode = targetNode;

    ASTNode* scopeBody = new ASTNode();
    scopeBody->nodeType = Scope_Body;
    scopeBody->codegen = &ASTNode::generateScopeBody;
    scopeBody->parentNode = attr;

    ASTNode* valueCopy = new ASTNode();
    valueCopy->nodeType = valueNode->nodeType;
    valueCopy->token = valueNode->token ? new asaToken(*valueNode->token) : new asaToken();
    valueCopy->codegen = valueNode->codegen;
    valueCopy->parentNode = scopeBody;
    scopeBody->childNodes.push_back(valueCopy);
    attr->childNodes.push_back(scopeBody);

    bool replaced = false;
    for (auto*& existingAttr : targetNode->attributes) {
        if (existingAttr && existingAttr->token && existingAttr->token->tokenStr == attrName) {
            replaceAttributeReference(targetNode, existingAttr, attr);
            replaced = true;
            break;
        }
    }
    if (!replaced)
        targetNode->attributes.push_back(attr);

    return true;
}

static bool registerCustomDirective(ASTNode* directiveNode)
{
    messageSystem::startBlock(directiveNode, "Processing `#make_directive` directive", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
    defer(messageSystem::endBlock());

    if (!directiveNode || directiveNode->childNodes.size() < 2 || directiveNode->childNodes[1]->childNodes.empty()) {
        messageSystem::error("#make_directive requires name, parameter list, and body arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    std::vector<ASTNode*> args = getDirectiveCallArgs(directiveNode);
    if (args.size() < 3) {
        messageSystem::error("#make_directive requires name, parameter list, and body arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    ASTNode* nameNode = unwrapExpressionNode(args[0]);
    if (!nameNode || !nameNode->token ||
        (nameNode->nodeType != String_Node && nameNode->nodeType != String_Constant_Node && nameNode->nodeType != Identifier_Node)) {
        messageSystem::error("#make_directive name must be a string literal or identifier", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    std::string directiveName = nameNode->nodeType == Identifier_Node ? nameNode->token->tokenStr : decodeQuotedStringToken(nameNode->token);
    if (directiveName.empty()) {
        messageSystem::error("#make_directive name cannot be empty", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    std::vector<std::string> parameters;
    if (args.size() == 3) {
        collectIdentifierNames(args[1], parameters);
    }
    else {
        for (int i = 1; i < (int)args.size() - 1; i++)
            collectIdentifierNames(args[i], parameters);
    }

    ASTNode* bodyNode = args.back();
    if (!bodyNode || bodyNode->nodeType != Scope_Body) {
        messageSystem::error("#make_directive body must be an AST scope body", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    customCompilerDirectives[directiveName] = {parameters, bodyNode};
    return true;
}

static bool applyReturnDirective(ASTNode* directiveNode, CompilerDirectiveInvocation* invocation)
{
    messageSystem::startBlock(directiveNode, "Processing `#return` directive", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
    defer(messageSystem::endBlock());

    if (!invocation) {
        messageSystem::error("#return can only be used inside #make_directive bodies", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    invocation->hasReturn = true;
    invocation->returnNode = nullptr;

    std::vector<ASTNode*> args = getDirectiveCallArgs(directiveNode);
    if (args.empty())
        return true;

    invocation->returnNode = resolveDirectiveASTTarget(args[0], invocation);
    return true;
}

static bool processSetFlagDirective(ASTNode* directiveNode)
{
    messageSystem::startBlock(directiveNode, "Processing `#setflag` directive", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
    defer(messageSystem::endBlock());

    std::vector<std::string> tokens;
    std::function<void(ASTNode*)> collectTokens = [&](ASTNode* node) {
        if (!node)
            return;
        if (node->token && !node->token->tokenStr.empty() && node->childNodes.empty())
            tokens.push_back(node->token->tokenStr);
        for (auto* child : node->childNodes)
            collectTokens(child);
    };
    if (directiveNode->childNodes.size() >= 2)
        collectTokens(directiveNode->childNodes[1]);

    if (tokens.size() < 2) {
        messageSystem::error("#setflag requires name and value arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    std::string value = ToLower(tokens[1]);
    if (value == "true" || value == "1")
        setCompilerDirectiveFlagAlias(tokens[0], true);
    else if (value == "false" || value == "0")
        setCompilerDirectiveFlagAlias(tokens[0], false);
    else {
        messageSystem::error("#setflag value must be a bool", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    return true;
}

static bool readDirectiveStackName(ASTNode* node, std::string& stackName)
{
    node = unwrapExpressionNode(node);
    if (!node || !node->token ||
        (node->nodeType != Identifier_Node && node->nodeType != String_Node && node->nodeType != String_Constant_Node))
        return false;

    stackName = node->nodeType == Identifier_Node ? node->token->tokenStr : decodeQuotedStringToken(node->token);
    return !stackName.empty();
}

static bool processVarDirective(ASTNode* directiveNode, CompilerDirectiveInvocation* invocation)
{
    messageSystem::startBlock(directiveNode, "Processing `#var` directive", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
    defer(messageSystem::endBlock());

    std::vector<ASTNode*> args = getDirectiveCallArgs(directiveNode);
    if (args.size() < 2) {
        messageSystem::error("#var requires name and value arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    ASTNode* nameNode = unwrapExpressionNode(args[0]);
    if (!nameNode || !nameNode->token || nameNode->nodeType != Identifier_Node) {
        messageSystem::error("#var name must be an identifier", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    ASTNode* valueNode = resolveDirectiveASTTarget(args[1], invocation);
    if (!valueNode) {
        messageSystem::error("#var value did not resolve to an AST node", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    if (invocation)
        invocation->arguments[nameNode->token->tokenStr] = valueNode;
    else if (directiveNode->parentNode)
        directiveNode->parentNode->compilerDefinitions[nameNode->token->tokenStr] = valueNode;
    return true;
}

static bool processStackPushDirective(ASTNode* directiveNode, CompilerDirectiveInvocation* invocation)
{
    messageSystem::startBlock(directiveNode, "Processing `#stack_push` directive", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
    defer(messageSystem::endBlock());

    std::vector<ASTNode*> args = getDirectiveCallArgs(directiveNode);
    if (args.size() < 2) {
        messageSystem::error("#stack_push requires stack name and AST node arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    std::string stackName;
    if (!readDirectiveStackName(args[0], stackName)) {
        messageSystem::error("#stack_push stack name must be an identifier or string literal", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    ASTNode* valueNode = resolveDirectiveASTTarget(args[1], invocation);
    if (!valueNode) {
        messageSystem::error("#stack_push value did not resolve to an AST node", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    compilerStacks[stackName].push(valueNode);
    return true;
}

static bool processStackPopDirective(ASTNode* directiveNode)
{
    messageSystem::startBlock(directiveNode, "Processing `#stack_pop` directive", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
    defer(messageSystem::endBlock());

    std::vector<ASTNode*> args = getDirectiveCallArgs(directiveNode);
    if (args.empty()) {
        messageSystem::error("#stack_pop requires stack name argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    std::string stackName;
    if (!readDirectiveStackName(args[0], stackName)) {
        messageSystem::error("#stack_pop stack name must be an identifier or string literal", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    if (compilerStacks.count(stackName) && !compilerStacks[stackName].empty())
        compilerStacks[stackName].pop();
    return true;
}

static bool processCustomMessageDirective(ASTNode* directiveNode, CompilerDirectiveInvocation* invocation, bool isError)
{
    std::string directiveName = isError ? "error" : "warning";
    messageSystem::startBlock(directiveNode, "Processing `#" + directiveName + "` directive", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
    defer(messageSystem::endBlock());

    std::vector<ASTNode*> args = getDirectiveCallArgs(directiveNode);
    if (args.empty()) {
        messageSystem::error("#" + directiveName + " requires a message argument", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    ASTNode* messageNode = unwrapExpressionNode(args[0]);
    if (!messageNode || !messageNode->token ||
        (messageNode->nodeType != String_Node && messageNode->nodeType != String_Constant_Node)) {
        messageSystem::error("#" + directiveName + " message must be a string literal", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    std::string message = decodeQuotedStringToken(messageNode->token);
    ASTNode* contextNode = args.size() >= 2 ? resolveDirectiveASTTarget(args[1], invocation) : directiveNode;
    messageSystem::startBlock(contextNode ? contextNode : directiveNode, "#" + directiveName + " directive", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
    if (isError) {
        messageSystem::error(message, messageSystem::Custom_Directive_Error);
        messageSystem::endBlock();
        return false;
    }
    messageSystem::warning(message, messageSystem::Custom_Directive_Warning);
    messageSystem::endBlock();
    return true;
}

static void makeDirectiveNodeEmpty(ASTNode*& node)
{
    node->nodeType = Nothing_Node;
    node->codegen = &ASTNode::generateNothing;
    node->childNodes.clear();
    node->leafNodes.clear();
}

static void processCompilerDirectiveTree(ASTNode*& node, CompilerDirectiveInvocation* invocation = nullptr);

static bool invokeCustomDirective(ASTNode*& node, const CustomCompilerDirective& directive, const std::string& directiveName)
{
    messageSystem::startBlock(node, "Processing custom compiler directive `#" + directiveName + "`", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
    defer(messageSystem::endBlock());
    ASTNode* callNode = node;

    std::vector<ASTNode*> args = getDirectiveCallArgs(node);

    if (args.size() != directive.parameters.size()) {
        messageSystem::error("#" + directiveName + " expects " + std::to_string(directive.parameters.size()) +
                                 " arguments, got " + std::to_string(args.size()),
            messageSystem::Invalid_Compiler_Directive_Arguments_Error);
        return false;
    }

    CompilerDirectiveInvocation childInvocation;
    childInvocation.contextNode = node;
    for (int i = 0; i < (int)directive.parameters.size(); i++)
        childInvocation.arguments[directive.parameters[i]] = args[i];

    ASTNode* invocationBody = cloneDirectiveBodyNode(directive.body, directive.body->parentNode);
    for (auto*& child : invocationBody->childNodes) {
        processCompilerDirectiveTree(child, &childInvocation);
        if (wasError || childInvocation.hasReturn)
            break;
    }

    if (wasError)
        return false;

    if (childInvocation.returnNode) {
        ASTNode* returnedNode = childInvocation.returnNode;
        returnedNode->parentNode = callNode->parentNode;
        node = returnedNode;
    }
    else {
        makeDirectiveNodeEmpty(node);
    }

    pushCompilerDirectiveCall(directiveName, callNode);
    return true;
}

static void processCompilerDirectiveTree(ASTNode*& node, CompilerDirectiveInvocation* invocation)
{
    if (!node || wasError)
        return;

    if (node->nodeType == Compile_Time_Directive && !node->childNodes.empty() && node->childNodes[0]->token) {
        const std::string& name = node->childNodes[0]->token->tokenStr;

        if (name == "make_directive") {
            if (registerCustomDirective(node)) {
                pushCompilerDirectiveCall(name, node);
                makeDirectiveNodeEmpty(node);
            }
            return;
        }

        if (name == "return") {
            if (applyReturnDirective(node, invocation))
                pushCompilerDirectiveCall(name, node);
            return;
        }

        if (name == "var") {
            if (processVarDirective(node, invocation))
                pushCompilerDirectiveCall(name, node);
            makeDirectiveNodeEmpty(node);
            return;
        }

        auto customDirective = customCompilerDirectives.find(name);
        if (customDirective != customCompilerDirectives.end()) {
            invokeCustomDirective(node, customDirective->second, name);
            return;
        }

        if (name == "setflag") {
            if (processSetFlagDirective(node))
                pushCompilerDirectiveCall(name, node);
            return;
        }

        if (name == "stack_push") {
            if (processStackPushDirective(node, invocation))
                pushCompilerDirectiveCall(name, node);
            return;
        }

        if (name == "stack_pop") {
            if (processStackPopDirective(node))
                pushCompilerDirectiveCall(name, node);
            return;
        }

        if (name == "if") {
            messageSystem::startBlock(node, "Processing `#if` directive", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
            defer(messageSystem::endBlock());

            std::vector<ASTNode*> args = getDirectiveCallArgs(node);
            if (args.size() < 2) {
                messageSystem::error("#if requires condition and AST node arguments", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
                return;
            }

            DirectiveValue condition = evaluateDirectiveValue(args[0], invocation);
            if (!condition.resolved)
                return;

            bool conditionResult = condition.truthy();
            if (conditionResult)
                processCompilerDirectiveTree(args[1], invocation);
            if (wasError)
                return;
            pushCompilerControlFlowResult(conditionResult, node);
            pushCompilerDirectiveCall(name, node);
            return;
        }

        if (name == "set_attribute") {
            if (applySetAttributeDirective(node, invocation))
                pushCompilerDirectiveCall(name, node);
            return;
        }

        if (name == "error" && invocation) {
            processCustomMessageDirective(node, invocation, true);
            return;
        }

        if (name == "warning" && invocation) {
            if (processCustomMessageDirective(node, invocation, false))
                pushCompilerDirectiveCall(name, node);
            return;
        }

        if (invocation && !node->codegen && !node->returnsASTNode) {
            messageSystem::startBlock(node, "Processing compiler directive `#" + name + "`", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
            defer(messageSystem::endBlock());
            messageSystem::error("Unknown compiler directive `#" + name + "`", messageSystem::Invalid_Compiler_Directive_Arguments_Error);
            return;
        }
    }

    for (auto*& child : node->childNodes) {
        processCompilerDirectiveTree(child, invocation);
        if (invocation && invocation->hasReturn)
            return;
    }
}

// Pre-pass: register all #make_directive calls before any other directive runs.
// This ensures custom directives are available regardless of module load order.
static void processMakeDirectivesOnly(ASTNode*& node)
{
    if (!node || wasError)
        return;

    if (node->nodeType == Compile_Time_Directive && !node->childNodes.empty() && node->childNodes[0]->token) {
        const std::string& name = node->childNodes[0]->token->tokenStr;
        if (name == "make_directive") {
            if (registerCustomDirective(node)) {
                pushCompilerDirectiveCall(name, node);
                makeDirectiveNodeEmpty(node);
            }
            return;
        }
    }

    for (auto*& child : node->childNodes)
        processMakeDirectivesOnly(child);
}

void processCompilerDirectives(ASTNode*& node)
{
    messageSystem::startBlock(node, "Processing compiler directives", __func__, __LINE__, __FILE__, messageSystem::Parser_Block);
    seedCompilerDirectiveFlags();
    processMakeDirectivesOnly(node);
    processCompilerDirectiveTree(node);
    messageSystem::endBlock();
}
