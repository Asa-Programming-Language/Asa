#include "main.h"


int main(int argc, char** argv)
{
	// Check if the console supports color, and disable if not
	console::useColor = console::consoleSupportsColor();

	if (verbosity >= 2)
		std::cout << COMPILER_PRINTOUT << std::endl
				  << std::endl;

#ifdef DEBUG
	runTests();
#endif

	// Handle options
	int c;
	int digit_optind = 0;

	std::string fileName;

	executableDirectory = std::filesystem::weakly_canonical(std::filesystem::path(argv[0])).parent_path().string() + "/";

	console::SetColor(console::redFGColor);
	while (1) {
		int this_option_optind = optind ? optind : 1;
		int option_index = 0;
		static struct option long_options[] = {
			{"compile", no_argument, 0, 'c'},
			{"verbose", no_argument, 0, 'v'},
			{"quiet", no_argument, 0, 'q'},
			{"file", required_argument, 0, 'f'},
			{"output", required_argument, 0, 'o'},
			{"optimize", required_argument, 0, 'O'},
			{0, 0, 0, 0}};

		c = getopt_long(argc, argv, "cvqf:o:O:0",
			long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
			case 0:
				//printf("option %s", long_options[option_index].name);
				//if (optarg)
				//	printf(" with arg %s", optarg);
				//printf("\n");
				break;

			case '0':
				//if (digit_optind != 0 && digit_optind != this_option_optind)
				//	printf("digits occur in two different argv-elements.\n");
				//digit_optind = this_option_optind;
				//printf("option %c\n", c);
				break;

			case 'c':
				//printf("option c\n");
				break;

			case 'v':
				//printf("option v\n");
				verbosity += 1;
				break;

			case 'q':
				//printf("option q\n");
				verbosity += 1;
				break;

			case 'f':
				//printf("option f with value '%s'\n", optarg);
				fileName = std::string(optarg);
				//printf("fileName is %s\n", fileName.c_str());
				break;

			case 'o':
				outputFileName = std::filesystem::weakly_canonical(std::filesystem::path(std::string(optarg))).string();
				break;

			case 'O':
				optimizationLevel = std::stoi(optarg);
				break;

			case '?':
				console::ResetColor();
				exit(1);

			default:
				//printf("?? getopt returned character code 0%o ??\n", c);
				break;
		}
	}
	console::ResetColor();

	// Load file if provided
	if (fileName != "") {
		int e = loadFile(fileName, initialFileString);
		if (e != 0) {
			console::Write("Invalid file path provided\n");
			exit(1);
		}
	}
	else {
		console::Write("Invalid file path provided\n");
		exit(1);
	}
	projectDirectory = std::filesystem::weakly_canonical(std::filesystem::path(fileName)).parent_path().string() + "/";
	baseFileName = std::filesystem::path(fileName).filename();


	// If output name not provided, create
	if (outputFileName == "")
		outputFileName = std::filesystem::weakly_canonical(std::filesystem::path(std::string(projectDirectory + "build/" + SplitString(baseFileName, ".")[0]))).string();


	// Begin tokenizing file
	int e = tokenize(initialFileString, allTokens);
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
			if (allTokens[i].second != EndOfLine) {
				console::Write(std::to_string(i) + "T:" + std::to_string(allTokens[i].lineNumber) + "L: ", console::yellowFGColor);
				if (allTokens[i].lineValue != nullptr)
					printf("[%s]\t[%s]\t[%s]\n", allTokens[i].first.c_str(), tokenAsString(allTokens[i].second).c_str(), allTokens[i].lineValue->c_str());
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
			rootNode->childNodes.push_back(importedNodes[i]);
		if (importedNodes.size() > 0)
			noImports = false;
		importedNodes = std::vector<ASTNode*>();

		// Module imports
		addModuleImports(rootNode);
		for (int i = 0; i < importedNodes.size(); i++)
			rootNode->childNodes.push_back(importedNodes[i]);
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

	//// Resolve dependencies
	//resolveDependencies(rootNode);

	// TODO: At this point allow for compile-time execution (#run)
	// via tree-walking or byte-code execution

	// Create build directory
	std::filesystem::create_directory(projectDirectory + "build");

	// Generate the IR LLVM Code:
	initializeCodeGenerator();
	if (verbosity >= 2)
		console::WriteLine("\n\nCompiling:", console::greenFGColor);
	// First pass
	generateOutputCode(rootNode, 0, 0);
	// Second pass
	generateOutputCode(rootNode, 0, 1);
	// Cleanup unused code
	removeUnusedPrototypes();

	// Print out all of the generated code.
	if (verbosity >= 4) {
		console::WriteLine("\n\nOutput IR Code:", console::greenFGColor);
		TheModule->print(errs(), nullptr);
	}

	//// Verify the module
	//if (llvm::verifyModule(*TheModule, &llvm::errs())) {
	//	std::cerr << "Module verification failed!\n";
	//	abort();
	//}

	// Output the object file in project's build directory
	std::string objectFilePath = outputFileName + ".o";
	outputObjectFile(objectFilePath);

	// Link the object file into executable
	generateExecutable(objectFilePath, outputFileName);
	if (verbosity >= 1)
		console::WriteLine("\n\nWrote executable to " + outputFileName);
}
