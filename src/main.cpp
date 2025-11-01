#include "main.h"


int main(int argc, char** argv)
{
	// Check if the console supports color, and disable if not
	console::useColor = console::consoleSupportsColor();

	// Handle options
	int c;
	int digit_optind = 0;

	std::string fileName = "";

	executableDirectory = std::filesystem::weakly_canonical(std::filesystem::path(argv[0])).parent_path().string() + "/";

	console::SetColor(console::redFGColor);
	while (1) {
		int this_option_optind = optind ? optind : 1;
		int option_index = 0;
		static struct option long_options[] = {
			{"compile", no_argument, 0, 'c'},
			{"verbose", no_argument, 0, 'v'},
			{"quiet", no_argument, 0, 'q'},
			{"silent", no_argument, 0, 's'},
			{"file", required_argument, 0, 'f'},
			{"output", required_argument, 0, 'o'},
			{"optimize", required_argument, 0, 'O'},
			{"compilerdebug", no_argument, 0, 'd'},
			{"debug", no_argument, 0, 'D'},
			{"runtests", no_argument, 0, 't'},
			{"run", no_argument, 0, 'r'},
			{"warn", required_argument, 0, 'w'},
			{"version", no_argument, 0, 'V'},
			{0, 0, 0, 0}};

		c = getopt_long(argc, argv, "cvqsdDtrVw:f:o:O:0",
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

			case 'v':
				verbosity += 1;
				break;

			case 's':
				verbosity = 0;
				break;

			case 'q':
				verbosity = 1;
				break;

			case 'f':
				fileName = std::string(optarg);
				break;

			case 'o':
				outputFileName = std::filesystem::weakly_canonical(std::filesystem::path(std::string(optarg))).string();
				break;

			case 'O':
				optimizationLevel = std::stoi(optarg);
				break;

			case 't':
				if (verbosity >= 2)
					std::cout << COMPILER_PRINTOUT << std::endl
							  << std::endl;
				compilerFlags |= Flags_RunTests;
				break;

			case 'd':
				compilerFlags |= Flags_CompilerDebug;
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
				break;
			}

			case 'V':
				if (verbosity >= 2)
					std::cout << COMPILER_PRINTOUT << std::endl
							  << std::endl;
				console::ResetColor();
				exit(0);

			case '?':
				console::ResetColor();
				exit(1);

			default:
				break;
		}
	}
	console::ResetColor();

	for (int i = optind; i < argc; i++) {
		if (fileName == "") {
			fileName = std::string(argv[i]);
			break;
		}
	}

#ifdef DEBUG
	if (compilerFlags == Flags_RunTests)
		runTests();
#endif

	// Load file if provided
	if (fileName != "") {
		int e = loadFile(fileName, initialFileString);
		if (e != 0) {
			console::WriteLine("Invalid file path provided");
			if (verbosity >= 3)
				console::WriteLine("Path \"" + initialFileString + "\" could not be opened", console::yellowFGColor);
			exit(1);
		}
	}
	else {
		console::WriteLine("Invalid file path provided");
		exit(1);
	}
	projectDirectory = std::filesystem::weakly_canonical(std::filesystem::path(fileName)).parent_path().string() + "/";
	baseFileName = std::filesystem::path(fileName).filename();
	std::string fullFileName = projectDirectory + baseFileName;


	// If output name not provided, create
	if (outputFileName == "")
		outputFileName = std::filesystem::weakly_canonical(std::filesystem::path(std::string(projectDirectory + "build/" + SplitString(baseFileName, ".")[0]))).string();


	// Begin tokenizing file
	int e = tokenize(initialFileString, allTokens, fullFileName);
	if (e != 0) {
		console::Write("Invalid tokens met\n");
		exit(1);
	}
	// Now change any allTokens to their subtoken type if applicable
	e = labelSubTokens(allTokens);
	if (e != 0) {
		console::Write("Invalid tokens met\n");
		exit(1);
	}
	e = joinCommentTokens(allTokens);
	if (e != 0) {
		console::Write("Invalid tokens met\n");
		exit(1);
	}
	e = removeCommentTokens(allTokens);
	if (verbosity >= 5) {
		printf("\nTokens:\n");
		for (int i = 0; i < allTokens.size(); i++) {
			if (allTokens[i]->second != EndOfLine) {
				console::Write(std::to_string(i) + "T:" + std::to_string(allTokens[i]->lineNumber) + "L: ", console::yellowFGColor);
				if (allTokens[i]->lineValue != nullptr)
					printf("[%s]\t[%s]\t[%s]\n", allTokens[i]->first.c_str(), tokenAsString(allTokens[i]->second).c_str(), allTokens[i]->lineValue->c_str());
			}
		}
	}

	// Generate AST
	if (verbosity >= 3)
		console::WriteLine("\n\nGenerating AST...", console::greenFGColor);
	rootNode = generateAST(allTokens);
	// Handle importing nodes from other sources
	for (;;) {
		bool noImports = true;
		// File includes
		addFileIncludes(rootNode);
		for (int i = 0; i < importedNodes.size(); i++)
			rootNode->childNodes.insert(rootNode->childNodes.begin(), importedNodes[i]);
		if (importedNodes.size() > 0)
			noImports = false;
		importedNodes = std::vector<ASTNode*>();

		// Module imports
		addModuleImports(rootNode);
		for (int i = 0; i < importedNodes.size(); i++)
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
	// Unify nested nodes
	unifyNodes(rootNode);

	// Print AST
	if (verbosity >= 4) {
		console::Write("\n\nGenerated AST:\n", console::greenFGColor);
		printAST(rootNode);
	}

	// Find any unused leaf nodes, and throw error if there are any
	findUnusedLeafNodes(rootNode);
	if (wasError)
		goto errorsEncountered;

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
		console::WriteLine("\n\nCompiling:", console::greenFGColor);
	// Pass 0: First pass, type/struct definitions
	// Pass 1: Function pass
	// Pass 2: Final pass
	for (int p = 0; p <= 2; p++) {
		generateOutputCode(rootNode, 0, p);
	}
	// Cleanup unused code
	if (!wasError)
		removeUnusedPrototypes();

	//DBuilder->finalize();

	// Print out all of the generated code.
	if (verbosity >= 5) {
		console::WriteLine("\n\nOutput IR Code:", console::greenFGColor);
		TheModule->print(errs(), nullptr);
	}
	if (wasError)
		goto errorsEncountered;

	// Write IR to <projectpath>/build/<basename>.ll
	std::string irFilePath = projectDirectory + "build/" + baseFileName + ".ll";
	std::error_code EC;
	llvm::raw_fd_ostream OS(irFilePath, EC, llvm::sys::fs::OF_None);
	if (EC) {
		// Handle error
		llvm::errs() << "Could not open file: " << EC.message() << "\n";
		exit(1);
	}
	TheModule->print(OS, nullptr);
	OS.close();


	// Verify the module
	if (compilerFlags == Flags_CompilerDebug) {
		console::WriteLine("\nVerifying code:");
		if (llvm::verifyModule(*TheModule, &llvm::errs())) {
			std::cerr << "Module verification failed!\n";
			abort();
		}
		console::WriteLine("Passed", console::greenFGColor);
	}

errorsEncountered:
	// Print out all function prototypes
	if (verbosity >= 4)
		printFunctionPrototypes();

	if (wasError) {
		console::WriteLine("Errors were encountered while compiling.", console::redFGColor);
		exit(1);
	}

	//// Output the object file in project's build directory
	//std::string objectFilePath = outputFileName + ".o";
	//outputObjectFile(objectFilePath);

	// Link the object file into executable
	generateExecutable(irFilePath, outputFileName);
	if (verbosity >= 1)
		console::WriteLine("Wrote executable to " + outputFileName);


	// Cleanup by deleting files only used for codegen.
	if (compilerFlags == Flags_Debug) {
		if (verbosity >= 3) {
			console::WriteLine("Cleaning up files: " + irFilePath);
			console::WriteLine("Cleaning up files: " + irFilePath + ".s");
		}
		std::filesystem::remove(irFilePath);
		std::filesystem::remove(irFilePath + ".s");
	}
}
