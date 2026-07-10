#include "main.h"


int main(int argc, char** argv)
{
    // Check if the console supports color, and disable if not
    console::useColor = console::consoleSupportsColor();

    // Handle options
    int c;
    int digit_optind = 0;

    std::string fileName = "";

    std::string clangOptions = "";

    executableDirectory = std::filesystem::weakly_canonical(std::filesystem::path(argv[0])).parent_path().string() + "/";

    console::setColor(console::redFGColor);
    while (1) {
        int this_option_optind = optind ? optind : 1;
        int option_index = 0;
        static struct option long_options[] = {
            {"compile", no_argument, 0, 'c'},
            {"clangoptions", required_argument, 0, 'C'},
            {"verbose", no_argument, 0, 'v'},
            {"quiet", no_argument, 0, 'q'},
            {"silent", no_argument, 0, 's'},
            {"flags", required_argument, 0, 'f'},
            {"output", required_argument, 0, 'o'},
            {"optimize", required_argument, 0, 'O'},
            {"compilerdebug", no_argument, 0, 'd'},
            {"debug", no_argument, 0, 'D'},
            {"runtests", no_argument, 0, 't'},
            {"run", no_argument, 0, 'r'},
            {"warn", required_argument, 0, 'w'},
            {"version", no_argument, 0, 'V'},
            {"printast", no_argument, 0, 'a'},
            {"time", no_argument, 0, 'T'},
            {0, 0, 0, 0}};

        c = getopt_long(argc, argv, "cCvqsdDtrVaw:f:o:O:T0",
            long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 0:
                break;

            case '0':
                break;

            case 'c':
                break;

            case 'C':
                clangOptions = std::string(optarg);
                break;

            case 'v':
                verbosity += 1;
                break;

            case 's':
                verbosity = 0;
                break;

            case 'q':
                verbosity = 1;
                break;

            case 'f': {
                std::string flagName = std::string(optarg);
                commandLineCompilerDirectiveFlags[ToLower(flagName)] = true;
                commandLineCompilerDirectiveFlags[ToUpper(flagName)] = true;

                if (flagName == "errortest")
                    compilerFlags |= Flags_RunErrorTests;
                else if (flagName == "compact")
                    compact = true;
                else if (flagName == "color")
                    compilerFlags |= Flags_Force_Enable_Color;
                break;
            }

            case 'o':
                outputFileName = std::filesystem::weakly_canonical(std::filesystem::path(std::string(optarg))).string();
                break;

            case 'O': {
                static const std::unordered_set<std::string> validLevels = {"0", "1", "2", "3", "s", "z", "g", "fast"};
                std::string val = std::string(optarg);
                if (!validLevels.count(val)) {
                    fprintf(stderr, "Unknown optimization level: -O%s\n", val.c_str());
                    return 1;
                }
                optimizationLevel = val;
                if (val == "g")
                    compilerFlags |= Flags_Debug;
                break;
            }

            case 't':
                console::resetColors();
                if (verbosity >= 2)
                    std::cout << COMPILER_PRINTOUT << std::endl
                              << std::endl;
                compilerFlags |= Flags_RunTests;
                break;

            case 'd':
                compilerFlags |= Flags_CompilerDebug;
                commandLineCompilerDirectiveFlags["compilerdebug"] = true;
                commandLineCompilerDirectiveFlags["COMPILERDEBUG"] = true;
                commandLineCompilerDirectiveFlags["compiler_debug"] = true;
                commandLineCompilerDirectiveFlags["COMPILER_DEBUG"] = true;
                break;

            case 'D':
                compilerFlags |= Flags_Debug;
                break;

            case 'r':
                compilerFlags |= Flags_Run;
                break;

            case 'w': {
                std::string flagVal = std::string(optarg);
                if (flagVal == "none")
                    warningFlags = W_None;
                else if (flagVal == "all")
                    warningFlags |= W_All;
                else if (flagVal == "conversion")
                    warningFlags |= W_Conversion;
                else if (flagVal == "attributes")
                    warningFlags |= W_Attributes;
                else if (flagVal == "unused")
                    warningFlags |= W_Unused;
                break;
            }

            case 'a':
                compilerFlags |= Flags_PrintAST;
                break;

            case 'T':
                compilerFlags |= Flags_Time;
                break;

            case 'V':
                if (verbosity >= 2)
                    std::cout << COMPILER_PRINTOUT << std::endl
                              << std::endl;
                console::resetColors();
                exit(0);

            case '?':
                console::resetColors();
                exit(1);

            default:
                break;
        }
    }
    console::resetColors();

    for (int i = optind; i < argc; i++) {
        if (fileName == "") {
            fileName = std::string(argv[i]);
            break;
        }
    }

    // Check if the console still supports color. (this second check allows for
    // the -fcolor option to force enable color
    console::useColor = console::consoleSupportsColor();

#ifdef DEBUG
    if (compilerFlags == Flags_RunTests) {
        runTests();
        exit(0);
    }
    if (compilerFlags == Flags_RunErrorTests) {
        runErrorTests();
        exit(0);
    }
#endif

    // Load file if provided
    if (fileName != "") {
        int e = loadFile(fileName, initialFileString);
        //initialFileString = "#import Builtin.String;\n" + initialFileString;
        //initialFileString = "#import Builtin.*;\n" + initialFileString;
        if (e != 0) {
            console::writeLine("Invalid file path provided");
            if (verbosity >= 3)
                console::writeLine("Path \"" + fileName + "\" could not be opened", console::yellowFGColor);
            exit(1);
        }
    }
    else {
        console::writeLine("Invalid file path provided");
        exit(1);
    }
    projectDirectory = std::filesystem::weakly_canonical(std::filesystem::path(fileName)).parent_path().string() + "/";
    baseFileName = std::filesystem::path(fileName).filename();
    std::string fullFileName = projectDirectory + baseFileName;

    if (verbosity >= 5) {
        console::writeLine("Asa compiler directory: " + executableDirectory);
        console::writeLine("Project directory: " + projectDirectory);
    }


    // If output name not provided, create
    if (outputFileName == "")
        outputFileName = std::filesystem::weakly_canonical(std::filesystem::path(std::string(projectDirectory + "build/" + SplitString(baseFileName, ".")[0]))).string();


    // Begin tokenizing file
    int e = tokenize(initialFileString, allTokens, fullFileName, lines, fileNames);
    if (e != 0) {
        console::write("Invalid tokens met\n");
        exit(1);
    }
    // Now change any allTokens to their subtoken type if applicable
    e = labelSubTokens(allTokens);
    if (e != 0) {
        console::write("Invalid tokens met\n");
        exit(1);
    }
    e = joinCommentTokens(allTokens);
    if (e != 0) {
        console::write("Invalid tokens met\n");
        exit(1);
    }
    if (verbosity >= 5) {
        printf("\nTokens:\n");
        for (int i = 0; i < allTokens.size(); i++) {
            if (allTokens[i]->tokenType != EndOfLine) {
                console::write(std::to_string(i) + "T:" + std::to_string(allTokens[i]->lineNumber) + "L: ", console::yellowFGColor);
                if (allTokens[i]->lineValue != nullptr)
                    printf("[%s]\t[%s]\t[%s]\n", allTokens[i]->tokenStr.c_str(), tokenAsString(allTokens[i]->tokenType).c_str(), allTokens[i]->lineValue->c_str());
            }
        }
    }

    // Generate AST
    if (verbosity >= 3)
        console::writeLine("\n\nGenerating AST...", console::greenFGColor);
    rootNode = generateAST(allTokens);
#define A new ASTNode


    // Insert `#import Builtin.Aliases;` at beginning of AST
    rootNode->childNodes.insert(rootNode->childNodes.begin(),
        A(Compile_Time_Directive,
            {
                A(Identifier_Node, {}, new asaToken("#import", Identifier)),
                A(Scope_Body,
                    {A(Member_Access,
                        {
                            A(Identifier_Node, {}, new asaToken("Builtin", Identifier)),
                            A(Identifier_Node, {}, new asaToken("Aliases", Identifier)),
                        },
                        new asaToken(".", Dot))}),
            }));
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
        addFileIncludes(rootNode);
        for (int i = importedNodes.size() - 1; i >= 0; i--)
            rootNode->childNodes.insert(rootNode->childNodes.begin(), importedNodes[i]);
        if (importedNodes.size() > 0)
            noImports = false;
        importedNodes = std::vector<ASTNode*>();

        // Module imports
        addModuleImports(rootNode);
        for (int i = importedNodes.size() - 1; i >= 0; i--)
            rootNode->childNodes.insert(rootNode->childNodes.begin(), importedNodes[i]);
        if (importedNodes.size() > 0)
            noImports = false;
        importedNodes = std::vector<ASTNode*>();

        if (noImports)
            break;
    }
    // Order AST operations
    fixPrecedence(rootNode);
    //// Optimize constant AST nodes
    //optimizeASTNode(rootNode);
    // Assign parent nodes
    assignParentNodes(rootNode);
    // Move qualified compile-time definitions (`Owner.member :: ...`) into their owner scope.
    normalizeQualifiedCompilerDefinitions(rootNode);
    assignParentNodes(rootNode);
    // Unify nested nodes
    unifyNodes(rootNode);
    // Resolve compile-time constant directives (#linenum, #line, etc.)
    resolveCompileTimeDirectives(rootNode);
    // Resolve .@ attribute access expressions to their constant values
    resolveAttributeAccess(rootNode);
    // Check for incompatible attribute combinations
    checkAttributeCompatibility(rootNode);
    // Find any unused leaf nodes, and throw error if there are any
    findUnusedLeafNodes(rootNode);
    // Process AST-affecting compiler directives after parse validation and before AST output/codegen.
    processCompilerDirectives(rootNode);
    // Re-check attributes because directives may have added or replaced them.
    checkAttributeCompatibility(rootNode);
    if (wasError)
        goto errorsEncountered;

    // Print AST (verbose)
    if (verbosity >= 4) {
        console::write("\n\nGenerated AST:\n", console::greenFGColor);
        printAST(rootNode);
    }

    // Print AST and exit if --printast was requested
    if (compilerFlags & Flags_PrintAST) {
        console::useColor = false;
        for (int i = 0; i < rootNode->childNodes.size(); i++) {
            ASTNode* child = rootNode->childNodes[i];
            if (child->token != nullptr &&
                child->token->filePath != nullptr &&
                *child->token->filePath == fullFileName)
                printAST(child);
        }
        exit(0);
    }


    // Force run main function
    if (compilerFlags == Flags_Run) {
        startTreeWalkExecution(rootNode);
        exit(1);
    }

    //// Resolve dependencies
    //resolveDependencies(rootNode);

    // TODO: At this point allow for compile-time execution (#run)
    // via tree-walking or byte-code execution

    // Create build directory
    std::filesystem::create_directory(projectDirectory + "build");

    // Generate the IR LLVM Code:
    initializeCodeGenerator();
    if (verbosity >= 4)
        console::writeLine("\n\nCompiling:", console::greenFGColor);
    // Pass 0: First pass, type/struct definitions
    // Pass 1: Function prototypes and struct contents
    // Pass 2: Variable and function contents
    for (int p = 0; p <= 2; p++) {
        int wasError = generateOutputCode(rootNode, 0, p);
        if (wasError)
            exit(1);
    }
    bool res = finalizeGlobalInit();
    if (!res || wasError)
        goto errorsEncountered;

    warnAboutUnusedVariables(rootNode);
    if (wasError)
        goto errorsEncountered;

    // Finalize debug info before optimization so the optimizer sees a consistent,
    // fully-resolved module. Running optimizeFunctions() on an unfinalized module
    // can cause incorrect loop elimination and other misoptimizations.
    llvmDebugBuilder->finalize();

    std::string irFilePath = projectDirectory + "build/" + baseFileName + ".ll";

    // Note: for optimizationLevel >= 1, optimization is handled by clang when
    // compiling the .ll file, so we skip the in-memory LLVM pass pipeline here.
    // Running it in-memory can produce misoptimizations due to module state
    // that doesn't round-trip cleanly through the text IR format.
    if (!wasError && !isOptimizing())
        optimizeFunctions();
    else if (wasError)
        goto errorsEncountered;

    // Print out all of the generated code.
    if (verbosity >= 5) {
        console::writeLine("\n\nOutput IR Code:", console::greenFGColor);
        llvmCompileModule->print(errs(), nullptr);
    }
    if (wasError)
        goto errorsEncountered;

    // Write IR to <projectpath>/build/<basename>.ll
    {
        std::error_code EC;
        llvm::raw_fd_ostream OS(irFilePath, EC, llvm::sys::fs::OF_None);
        if (EC) {
            llvm::errs() << "Could not open file: " << EC.message() << "\n";
            exit(1);
        }
        llvmCompileModule->print(OS, nullptr);
    }


    // Verify the module
    if (compilerFlags == Flags_CompilerDebug) {
        console::writeLine("\nVerifying code:");
        if (llvm::verifyModule(*llvmCompileModule, &llvm::errs())) {
            std::cerr << "Module verification failed!\n";
            abort();
        }
        console::writeLine("Passed", console::greenFGColor);
    }

errorsEncountered:
    // Print out all function prototypes TODO: Update this
    //if (verbosity >= 4)
    //    printFunctionPrototypes();

    if (wasError) {
        console::writeLine("\nErrors were encountered while compiling.", console::redFGColor);
        exit(1);
    }

    //// Output the object file in project's build directory
    //std::string objectFilePath = outputFileName + ".o";
    //outputObjectFile(objectFilePath);

    // Link the object file into executable
    generateExecutable(irFilePath, outputFileName, clangOptions);
    if (verbosity >= 1)
        console::writeLine("Wrote executable to " + outputFileName);


    // Cleanup by deleting files only used for codegen.
    if (compilerFlags == Flags_Debug) {
        if (verbosity >= 3) {
            console::writeLine("Cleaning up files: " + irFilePath);
            console::writeLine("Cleaning up files: " + irFilePath + ".s");
        }
        std::filesystem::remove(irFilePath);
        std::filesystem::remove(irFilePath + ".s");
        std::filesystem::remove(irFilePath + ".opt.ll");
    }
}
