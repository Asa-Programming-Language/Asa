#include "tests.h"

#ifdef DEBUG

enum TestType {
	No_Test,

	AST,
};

struct Test {
	std::string code;
	ASTNode* expectedAST = nullptr;
	TestType testType = No_Test;
	Test(std::string s, ASTNode* n)
	{
		code = s;
		expectedAST = n;
		testType = AST;
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
					A(Compiler_Modifiers,{}),
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
					A(Compiler_Modifiers,{}),
					A(Scope_Body,
						{
							A(Expression_Statement,
								{
									A({}, Identifier_Node),
									A(Expression_Term,
										{A(Integer_Node, {}, tokenPair("4", Integer), true)}
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
					A(Compiler_Modifiers,{}),
					A(Scope_Body, // Contents of main(){
						{
							A(For_Statement_Node,
								{
									A(Range_Node,
										{
											A(Range_Node,
												{
													A(Integer_Node, {}, tokenPair("0", Integer), true),
													A(Integer_Node, {}, tokenPair("100", Integer), true),
												}
											)
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
			for(i : 0..100){

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
					A(Compiler_Modifiers,{}),
					A(Scope_Body, // Contents of main(){
						{
							A(For_Statement_Node,
								{
									A(Iterator,  // Iterator i
										{A(Identifier_Node, {}, tokenPair("i", Identifier), true)}
									),
									A(Range_Node, // Range
										{
											A(Range_Node,
												{
													A(Integer_Node, {}, tokenPair("0", Integer), true),
													A(Integer_Node, {}, tokenPair("100", Integer), true),
												}
											)
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
		#import Tests.Test1;
	)",
		A(Scope_Body, // Global
			{
			A(Nothing_Node,
				{
					A(Identifier_Node, {}),
					A(Scope_Body, 
						{
							A(Module_Scope,
								{
									A(Identifier_Node),
									A(Identifier_Node),
								}
							)
						}
					),
				}
			),
			A(Compiler_Define_Function, // x
				{
					A(Identifier_Node,{}), // x name
					A(Type_Node,{}),
					A(Arguments,
						{A(Expression_Term, {})}
					),
					A(Compiler_Modifiers,{}),
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
	),
};
// clang-format on


void runTests()
{
	printf("Running tests...\n");

	// Set verbosity to 0 temporarily
	int lastVerbosity = verbosity;
	verbosity = 0;

	for (int i = 0; i < tests.size(); i++) {
		try {
			console::Write(PadStringRight("Test " + std::to_string(i + 1), '.', 60));

			Test& t = tests[i];
			std::vector<tokenPair> localTokens = std::vector<tokenPair>();

			// Begin tokenizing file
			int e = tokenize(t.code, localTokens);
			if (e != 0) {
				console::PrintError("Invalid tokens met", __LINE__, __FILE__);
				goto testFailed;
			}
			// Now change any tokens to their subtoken type if applicable
			e = labelSubTokens(localTokens);
			if (e != 0) {
				console::PrintError("Invalid tokens met", __LINE__, __FILE__);
				goto testFailed;
			}
			e = joinCommentTokens(localTokens);
			if (e != 0) {
				console::PrintError("Invalid tokens met", __LINE__, __FILE__);
				goto testFailed;
			}
			e = removeCommentTokens(localTokens);

			// Generate AST
			ASTNode* localRoot = generateAST(localTokens);

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
			// Optimize constant AST nodes
			optimizeASTNode(localRoot);
			// Assign parent nodes
			assignParentNodes(localRoot);
			// Resolve dependencies
			resolveDependencies(localRoot);

			// Do the check this test is for:
			switch (t.testType) {
				case AST: {
					if (*(t.expectedAST) == *(localRoot)) {
					}
					else {
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
		console::WriteLine("ok", console::greenFGColor);
		continue;
	fail:
		console::WriteLine("failed", console::redFGColor);
		console::PrintError("Test [" + std::to_string(i) + "] failed", __LINE__, __FILE__);
		goto testFailed;
	}

	verbosity = lastVerbosity;

	console::WriteLine("All tests passed ✔  \n", console::greenFGColor);
	return;

testFailed:
	verbosity = lastVerbosity;

	console::PrintError("");
	console::WriteLine("A test failed ❌ \n", console::redFGColor);
	//throw;
}
#endif
