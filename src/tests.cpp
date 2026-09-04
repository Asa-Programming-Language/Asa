#include "tests.h"

#ifdef DEBUG

enum TestType {
    No_Test,

    AST_Test,
    Error_Test,
};

struct Test {
    std::string name;
    std::string code;
    ASTNode* expectedAST = nullptr;
    TestType testType = No_Test;
    Test(std::string s, ASTNode* n)
    {
        code = s;
        expectedAST = n;
        testType = AST_Test;
    }
    Test(std::string name, std::string s)
    {
        this->name = name;
        code = s;
        testType = Error_Test;
    }
    Test()
    {
    }
};

    #define A new ASTNode

// clang-format off
std::vector<Test> tests = {
    
    // Main function definition
    Test(
        R"(
        main :: (){

        }
    )",
        A(Scope_Body,
            {A(Compiler_Define_Function,
                {
                    A(Identifier_Node,{}),
                    A(Type_Node,{}),
                    A(Arguments,
                        {A(Expression_Term, {})}
                    ),
                    A(Compiler_Modifiers,{A(Scope_Body)}),
                    A(Variants_Node, {}),
                    A(Scope_Body,
                        {
                        }
                    )
                }
            )
            }
        )
    ),

    // Main function with set expression, adding two integers, result should have simplified AST
    Test(
        R"(
        main :: (){
            i = 2+2;
        }
    )",
        A(Scope_Body,
            {A(Compiler_Define_Function,
                {
                    A(Identifier_Node,{}),
                    A(Type_Node,{}),
                    A(Arguments,
                        {A(Expression_Term, {})}
                    ),
                    A(Compiler_Modifiers,{A(Scope_Body)}),
                    A(Variants_Node, {}),
                    A(Scope_Body,
                        {
                            A(Expression_Statement,
                                {
                                    A({}, Identifier_Node),
                                    A(Expression_Term,
                                        {
                                            A(Expression_Plus, {A(Integer_Node, {}, new asaToken("2", Integer), true),A(Integer_Node, {}, new asaToken("2", Integer), true)})
                                        }
                                    )
                                }
                            )
                        }
                    )
                }
            )
            }
        )
    ),

    // Main function, for loop with range
    Test(
        R"(
        main :: (){
            for(0..100){

            }
        }
    )",
        A(Scope_Body, // Global
            {A(Compiler_Define_Function, // Main
                {
                    A(Identifier_Node,{}), // main name
                    A(Type_Node,{}),
                    A(Arguments,
                        {A(Expression_Term, {})}
                    ),
                    A(Compiler_Modifiers,{A(Scope_Body)}),
                    A(Variants_Node, {}),
                    A(Scope_Body, // Contents of main(){
                        {
                            A(For_Statement_Node,
                                {
                                    A(Nothing_Node),
                                    A(Range_Node,
                                        {
                                            A(Integer_Node, {}, new asaToken("0", Integer), true),
                                            A(Integer_Node, {}, new asaToken("100", Integer), true),
                                        }
                                    ),
                                    A(Scope_Body, {}),
                                }
                            )
                        }
                    )
                }
            )
            }
        )
    ),

    // Main function, for loop with range and iterator
    Test(
        R"(
        main :: (){
            for(i in 0..100){

            }
        }
    )",
        A(Scope_Body, // Global
            {A(Compiler_Define_Function, // Main
                {
                    A(Identifier_Node,{}), // main name
                    A(Type_Node,{}),
                    A(Arguments,
                        {A(Expression_Term, {})}
                    ),
                    A(Compiler_Modifiers,{A(Scope_Body)}),
                    A(Variants_Node, {}),
                    A(Scope_Body, // Contents of main(){
                        {
                            A(For_Statement_Node,
                                {
                                    A(Iterator,  // Iterator i
                                        {A(Identifier_Node, {}, new asaToken("i", Identifier), true)}
                                    ),
                                    A(Range_Node, // Range
                                        {
                                            A(Integer_Node, {}, new asaToken("0", Integer), true),
                                            A(Integer_Node, {}, new asaToken("100", Integer), true),
                                        }
                                    ),
                                    A(Scope_Body, {}),
                                }
                            )
                        }
                    )
                }
            )
            }
        )
    ),

    // Macro definition
    Test(
        R"(
        Macro :: {};
    )",
        A(Scope_Body, // Global
            {A(Compiler_Define, // Macro
                {
                    A(Scope_Body, // Contents of {}
                        {
                        }
                    )
                }
            )
            }
        )
    ),

    // Module includes
    Test(
        R"(
        #import Tests.CompilerImportTest;
    )",
        A(Scope_Body, // Global
            {
                A(Nothing_Node,
                    {
                        A(Identifier_Node, {}),
                        A(Scope_Body,
                            {
                                A(Member_Access,
                                    {
                                        A(Identifier_Node),
                                        A(Identifier_Node),
                                    }
                                )
                            }
                        ),
                    }
                ),
                
                A(Compiler_Define,
                    {
                        A(Scope_Body,
                            {
                                A(Module_Define_Node,
                                    {
                                        A(Scope_Body,
                                            {
                                                A(Compiler_Define_Function, // x
                                                    {
                                                        A(Identifier_Node,{}), // x name
                                                        A(Type_Node,
                                                            {A(Identifier_Node)}),
                                                        A(Arguments,
                                                            {A(Expression_Term, {})}
                                                        ),
                                                        A(Compiler_Modifiers,{A(Scope_Body)}),
                                                        A(Variants_Node, {}),
                                                        A(Scope_Body, // Contents of x(){
                                                            {
                                                                A(Return_Node, 
                                                                    {
                                                                        A(Expression_Term,
                                                                            {
                                                                                A(Integer_Node)
                                                                            }
                                                                        )
                                                                    }
                                                                )
                                                            }
                                                        )
                                                    }
                                                )
                                            }
                                        )
                                    }
                                )
                            }
                        )
                    }
                )
            
            }
        )
    ),
};
// clang-format on

// clang-format off
std::vector<Test> errorTests = {

    /////////////
    // Errors: //
    /////////////
    
    Test("Undefined function",
        R"(
        main :: (){
            someUndefinedFunction();
        }
    )"),

    Test("Undefined function with candidates",
        R"(
        foo :: (x : int){

        }
        foo :: (x : float){

        }

        main :: (){
            foo();
        }
    )"),

    Test("Undefined function with exact candidates",
        R"(
        foo :: (x : exact float){

        }

        main :: (){
            foo(4);
        }
    )"),

    Test("Unsupported cast",
        R"(
        s :: struct {}

        main :: (){
            x : s = s();
            x = 4.5;
        }
    )"),

    Test("Undefined variable",
        R"(
        main :: (){
            x = y;
        }
    )"),

    Test("Undefined variable with candidates",
        R"(
        main :: (){
            someVar = 4;
            x = someVa;
        }
    )"),

    Test("Redefined function",
        R"(
        someFunc :: (){
        }

        someFunc :: (){
        }
    )"),

    Test("Redefined function, all same line",
        R"(
        someFunc :: (){} someFunc :: (){}
    )"),

    Test("Redefined variable",
        R"(
        someVar : int = 5;
        someVar : int = 5;
    )"),

    Test("Runtime assignment to compiler constant",
        R"(
        SOME_VAL :: 5;

        main :: (){
            SOME_VAL = 1;
        }
    )"),

    Test("Invalid compiler directive arguments",
        R"(
        #define 2;
    )"),

    Test("Invalid compiler directive arguments",
        R"(
        #library 2;
    )"),

    Test("Variable declaration missing rhs",
        R"(
        someVar : int;
    )"),

    Test("Inferred variable declaration with undefined initializer missing type",
        R"(
        someVar = ?;
    )"),

    Test("Inferred variable declaration with undefined initializer missing type, filler lines",
        R"(
        main :: (){
            x : int = 9;
            someVar = ?;
            x += someVar;
        }
    )"),

    Test("Incompatible attributes used together",
        R"(
        @external:
        @internal:
        main :: (){
            // ...
        }
    )"),

    Test("Incompatible attributes used together, same line",
        R"(
        @external: @internal:
        main :: (){
            // ...
        }
    )"),

    Test("Duplicate attributes",
        R"(
        @public:
        @public:
        main :: (){
            // ...
        }
    )"),

    Test("Undefined variable, lines filled with comments",
        R"(
        // The main function
        main :: (){
            // Set x to y
            x = y;
        } // End of main function
    )"),

    Test("Undefined variable, lines filled with comments with gaps",
        R"(
        // The main function
        main :: (){
            // Set x to y
            // Then here's another comment
            // And another
            x = y;
        }// End  of main function
    )"),

    Test("Undefined variable, very long line to go around",
        R"(
        main :: (){
            x = y;
        } // This is a long comment that should likely be more than the allowed length for the message line to go around
    )"),

    Test("Undefined member (struct)",
        R"(
        bar :: struct{}
        main :: (){
            b : bar = default;
            b.mem = 5;
        }
    )"),

    Test("Undefined member with candidates (struct)",
        R"(
        bar :: struct{
            a : int = default;
            b : char = 'w';
        }
        main :: (){
            b : bar = default;
            b.mem = 5;
        }
    )"),

    Test("Removed function usage",
        R"(
        @removed:
        foo :: (){

        }
        main :: (){
            foo();
        }
    )"),

    Test("Removed function usage with message",
        R"(
        @removed("Use `bar()` instead"):
        foo :: (){

        }
        main :: (){
            foo();
        }
    )"),

    Test("Removed function usage with message, very long distance between",
        R"(
        @removed("Use `bar()` instead"):
        foo :: (){

        }
        















        main :: (){
            foo();
        }
    )"),

    Test("Member variable used without `this` qualifier",
        R"(
        bar :: struct{
            x : int = 0;

            memberFunc :: (){
                x += 1;
            }
        }
    )"),

    Test("Attribute missing ending colon/semicolon",
        R"(
        @someAttr
        foo :: (){

        }
    )"),

    Test("Context info invalid location",
        R"(
        #funcname;
    )"),

    ///////////////
    // Warnings: //
    ///////////////

    Test("Deprecated function usage warning",
        R"(
        @deprecated:
        foo :: (){

        }

        main :: (){
            foo();
        }
    )"),

    Test("Deprecated function usage warning with message",
        R"(
        @deprecated("Use 'bar(); instead"):
        foo :: (){

        }

        main :: (){
            foo();
        }
    )"),

    Test("Multiple warnings in one compilation",
        R"(
        @deprecated("Use 'bar(); instead"):
        foo :: (){

        }

        @deprecated("bar is actually deprecated too. What are we to do?"):
        bar :: (){
            // I am going to put some comments 
            //
            // in here
            x : int = 5;
        }

        main :: (){
            foo();

            bar();
        }
    )"),

    Test("Undefined variable usage warning",
        R"(
        main :: (){
            x : int = ?;
            y : int = x;
            x = 1;
            z : int = x;
        }
    )"),
};
// clang-format on

// TODO: Make all compilation be executed using a singular function path, to prevent repetition and
// multiple steps that must be kept up to date separately

void runTests()
{
    printf("Running AST tests...\n");

    // Set verbosity to 0 temporarily
    int lastVerbosity = verbosity;
    verbosity = 0;

    for (int i = 0; i < tests.size(); i++) {
        try {
            console::printIndent(1);
            console::write(PadStringRight("Test " + std::to_string(i + 1), '.', 60) + " ");

            Test& t = tests[i];
            std::vector<asaToken*> localTokens = std::vector<asaToken*>();
            std::vector<std::string*> localLines;
            std::vector<std::string*> localFileNames;

            // Begin tokenizing file
            std::string fileName = "";
            int e = tokenize(t.code, localTokens, fileName, localLines, localFileNames);
            if (e != 0) {
                console::printError("Invalid tokens met", __LINE__, __FILE__);
                goto testFailed;
            }
            // Now change any tokens to their subtoken type if applicable
            e = labelSubTokens(localTokens);
            if (e != 0) {
                console::printError("Invalid tokens met", __LINE__, __FILE__);
                goto testFailed;
            }
            e = joinCommentTokens(localTokens);
            if (e != 0) {
                console::printError("Invalid tokens met", __LINE__, __FILE__);
                goto testFailed;
            }

            // Make allTokens reflect the current test's tokens so collectSourceLines works
            allTokens = localTokens;

            // Generate AST
            ASTNode* localRoot = generateAST(localTokens);
            if (wasError || !localRoot)
                exit(1);

            // Handle importing nodes from other sources
            for (;;) {
                bool noImports = true;
                // File includes
                addFileIncludes(localRoot);
                for (int i = 0; i < importedNodes.size(); i++)
                    localRoot->childNodes.push_back(importedNodes[i]);
                if (importedNodes.size() > 0)
                    noImports = false;
                importedNodes = std::vector<ASTNode*>();

                // Module imports
                addModuleImports(localRoot);
                for (int i = 0; i < importedNodes.size(); i++)
                    localRoot->childNodes.push_back(importedNodes[i]);
                if (importedNodes.size() > 0)
                    noImports = false;
                importedNodes = std::vector<ASTNode*>();

                if (noImports)
                    break;
            }
            // Order AST operations
            fixPrecedence(localRoot);
            //// Optimize constant AST nodes
            //optimizeASTNode(localRoot);
            // Assign parent nodes
            assignParentNodes(localRoot);
            // Unify nested nodes
            unifyNodes(localRoot);
            //// Resolve dependencies
            //resolveDependencies(localRoot);
            // Resolve compile-time constant directives (#linenum, #line, etc.)
            resolveCompileTimeDirectives(localRoot);
            // Resolve .@ attribute access expressions to their constant values
            resolveAttributeAccess(localRoot);
            // Check for incompatible attribute combinations
            checkAttributeCompatibility(localRoot);

            if (wasError)
                goto fail;

            // Do the check this test is for:
            switch (t.testType) {
                case AST_Test: {
                    if (*(t.expectedAST) == *(localRoot)) {
                    }
                    else {
                        console::writeLine("failed", console::redFGColor);
                        printf("Expected AST:\n");
                        printAST(t.expectedAST);
                        printf("\n\nActual AST:\n");
                        printAST(localRoot);
                        goto fail;
                    }
                    break;
                }
                default:
                    break;
            }
        }
        catch (...) {
            goto fail;
        }
        console::writeLine("ok", console::greenFGColor);
        continue;
    fail:
        console::printError("Test [" + std::to_string(i) + "] failed", __LINE__, __FILE__);
        goto testFailed;
    }

    verbosity = lastVerbosity;

    console::writeLine("All tests passed ✔  \n", console::greenFGColor);
    return;

testFailed:
    verbosity = lastVerbosity;

    console::printError("");
    console::writeLine("A test failed ❌ \n", console::redFGColor);
    //exit(1);
    //throw;
}


void runErrorTests()
{
    printf("Running error tests...\n");

    warningFlags |= W_All;
    messageSystem::exitOnError = false;

    for (int i = 0; i < errorTests.size(); i++) {
        try {
            Test& t = errorTests[i];
            std::vector<asaToken*> localTokens = std::vector<asaToken*>();
            std::vector<std::string*> localLines;
            std::vector<std::string*> localFileNames;

            console::writeLine("\n\n======================== [" + t.name + "] ======================\n\n");

            // Begin tokenizing file
            std::string fileName = "/example/fake/directory/main.asa";
            projectDirectory = "/example/fake/directory/";
            int e = tokenize(t.code, localTokens, fileName, localLines, localFileNames);
            if (e != 0) {
                console::printError("Invalid tokens met", __LINE__, __FILE__);
            }
            // Now change any tokens to their subtoken type if applicable
            e = labelSubTokens(localTokens);
            if (e != 0) {
                console::printError("Invalid tokens met", __LINE__, __FILE__);
            }
            e = joinCommentTokens(localTokens);
            if (e != 0) {
                console::printError("Invalid tokens met", __LINE__, __FILE__);
            }

            // Make allTokens reflect the current test's tokens so collectSourceLines works
            allTokens = localTokens;

            if (verbosity >= 5) {
                printf("\nTokens:\n");
                for (int j = 0; j < allTokens.size(); j++) {
                    if (allTokens[j]->tokenType != EndOfLine) {
                        console::write(std::to_string(j) + "T:" + std::to_string(allTokens[j]->lineNumber) + "L: ", console::yellowFGColor);
                        if (allTokens[j]->lineValue != nullptr)
                            printf("[%s]\t[%s]\t[%s]\n", allTokens[j]->tokenStr.c_str(), tokenTypeAsString(allTokens[j]->tokenType).c_str(), allTokens[j]->lineValue->c_str());
                        else
                            printf("[%s]\t[%s]\t[nullptr]\n", allTokens[j]->tokenStr.c_str(), tokenTypeAsString(allTokens[j]->tokenType).c_str());
                    }
                }
            }

            // Generate AST
            ASTNode* localRoot = generateAST(localTokens);
            if (wasError || !localRoot)
                goto wasError;

            // Insert `#import Builtin.Casts;` at beginning of AST
            rootNode->childNodes.insert(rootNode->childNodes.begin(),
                A(Compile_Time_Directive,
                    {
                        A(Identifier_Node, {}, new asaToken("#import", Identifier)),
                        A(Scope_Body,
                            {A(Member_Access,
                                {
                                    A(Identifier_Node, {}, new asaToken("Builtin", Identifier)),
                                    A(Identifier_Node, {}, new asaToken("Casts", Identifier)),
                                },
                                new asaToken(".", Dot))}),
                    }));
            // Insert `#import Builtin.String;` at beginning of AST
            rootNode->childNodes.insert(rootNode->childNodes.begin(),
                A(Compile_Time_Directive,
                    {
                        A(Identifier_Node, {}, new asaToken("#import", Identifier)),
                        A(Scope_Body,
                            {A(Member_Access,
                                {
                                    A(Identifier_Node, {}, new asaToken("Builtin", Identifier)),
                                    A(Identifier_Node, {}, new asaToken("String", Identifier)),
                                },
                                new asaToken(".", Dot))}),
                    }));
            // Handle importing nodes from other sources
            for (;;) {
                bool noImports = true;
                // File includes
                addFileIncludes(localRoot);
                for (int i = 0; i < importedNodes.size(); i++)
                    localRoot->childNodes.push_back(importedNodes[i]);
                if (importedNodes.size() > 0)
                    noImports = false;
                importedNodes = std::vector<ASTNode*>();

                // Module imports
                addModuleImports(localRoot);
                for (int i = 0; i < importedNodes.size(); i++)
                    localRoot->childNodes.push_back(importedNodes[i]);
                if (importedNodes.size() > 0)
                    noImports = false;
                importedNodes = std::vector<ASTNode*>();

                if (noImports)
                    break;
            }
            // Order AST operations
            fixPrecedence(localRoot);
            //// Optimize constant AST nodes
            //optimizeASTNode(localRoot);
            // Assign parent nodes
            assignParentNodes(localRoot);
            // Unify nested nodes
            unifyNodes(localRoot);
            //// Resolve dependencies
            //resolveDependencies(localRoot);
            // Resolve compile-time constant directives (#linenum, #line, etc.)
            resolveCompileTimeDirectives(localRoot);
            // Resolve .@ attribute access expressions to their constant values
            resolveAttributeAccess(localRoot);
            // Check for incompatible attribute combinations
            checkAttributeCompatibility(localRoot);

            if (wasError)
                goto wasError;

            // Find any unused leaf nodes, and throw error if there are any
            findUnusedLeafNodes(localRoot);

            // Generate the IR LLVM Code:
            initializeCodeGenerator();
            if (verbosity >= 4)
                console::writeLine("\n\nCompiling:", console::greenFGColor);
            // Pass 0: First pass, type/struct definitions
            // Pass 1: Function prototypes and struct contents
            // Pass 2: Variable and function contents
            for (int p = 0; p <= 2; p++) {
                int wasError = generateOutputCode(localRoot, 0, p);
                if (wasError)
                    break;
            }

            finalizeGlobalInit();

        wasError:

            // Reset the compiler for the next test
            resetCodeGenerator();
            messageSystem::clearNodes();
        }
        catch (...) {
        }
    }

    messageSystem::exitOnError = true;
}


#endif
