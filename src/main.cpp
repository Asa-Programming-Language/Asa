#include "main.h"


int main(int argc, char** argv)
{
	// Handle options
	int c;
	int digit_optind = 0;

	std::string fileName;

	while (1) {
		int this_option_optind = optind ? optind : 1;
		int option_index = 0;
		static struct option long_options[] = {
			{"compile", no_argument, 0, 'c'},
			{"verbose", no_argument, 0, 'v'},
			{"quiet", no_argument, 0, 'q'},
			{"file", required_argument, 0, 'f'},
			{0, 0, 0, 0}};

		c = getopt_long(argc, argv, "cvqf:0",
			long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
			case 0:
				printf("option %s", long_options[option_index].name);
				if (optarg)
					printf(" with arg %s", optarg);
				printf("\n");
				break;

			case '0':
				if (digit_optind != 0 && digit_optind != this_option_optind)
					printf("digits occur in two different argv-elements.\n");
				digit_optind = this_option_optind;
				printf("option %c\n", c);
				break;

			case 'c':
				printf("option c\n");
				break;

			case 'v':
				printf("option v\n");
				verbosity++;
				break;

			case 'q':
				printf("option q\n");
				verbosity--;
				break;

			case 'f':
				printf("option f with value '%s'\n", optarg);
				fileName = std::string(optarg);
				printf("fileName is %s\n", fileName.c_str());
				break;

			case '?':
				break;

			default:
				printf("?? getopt returned character code 0%o ??\n", c);
		}
	}

	// Load file if provided
	if (fileName != "") {
		int e = loadFile(fileName);
		if (e != 0) {
			ERROR("Invalid file path provided\n");
			exit(1);
		}
	}
	else {
		ERROR("Invalid file path provided\n");
		exit(1);
	}

	// Begin tokenizing file
	int e = tokenize(initialFileString);
	if (e != 0) {
		ERROR("Invalid allTokens met\n");
		exit(1);
	}
	// Now change any allTokens to their subtoken type if applicable
	e = labelSubTokens(allTokens);
	if (e != 0) {
		ERROR("Invalid allTokens met\n");
		exit(1);
	}
	e = joinCommentTokens(allTokens);
	if (e != 0) {
		ERROR("Invalid allTokens met\n");
		exit(1);
	}
	e = removeCommentTokens(allTokens);
	printf("\nTokens:\n");
	for (int i = 0; i < allTokens.size(); i++) {
		if (allTokens[i].second != EndOfLine) {
			printf("%dT:%d: ", i, allTokens[i].lineNumber);
			//if (allTokens[i].lineValue != nullptr)
			printf("[%s]\t[%s]\t[%s]\n", allTokens[i].first.c_str(), tokenAsString(allTokens[i].second).c_str(), allTokens[i].lineValue->c_str());
		}
	}

	// Generate AST
	rootNode = generateAST(allTokens);
	// Order AST operations
	fixPrecedence(rootNode);
	// Optimize constant AST nodes
	//optimizeASTNode(rootNode);
	//if (e != 0) {
	//	ERROR("Errors creating abstract syntax tree\n");
	//	exit(1);
	//}
	printf("\n\nGenerated AST:\n");
	printAST(rootNode);


	//// Parse the allTokens
	//e = beginParse(allTokens);
	//if (e != 0) {
	//	ERROR("Invalid allTokens met\n");
	//	exit(1);
	//}

	if (optind < argc) {
		printf("non-option ARGV-elements: ");
		while (optind < argc)
			printf("%s ", argv[optind++]);
		printf("\n");
	}
}
