#include "tokenizer.h"

std::vector<tokenPair> allTokens = std::vector<tokenPair>();

bool charInArray(char x, const char* a)
{
	for (int i = 0; a[i] != '\0'; i++) {
		if (a[i] == x)
			return true;
	}
	return false;
}

//tokenPair NEXT_TOKEN(int& i)
//{
//	if (i + 1 < tokens.size())
//		return tokens[++i];
//	else
//		printf("Error: Out of bounds token\n");
//	throw std::runtime_error("Error: Out of bounds token from NEXT_TOKEN\n" __FILE__);
//}

const std::map<TokenType, const char*> tokenStarts = {
	{Identifier, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_"},
	{Integer, "0123456789"},
	{String, "\""},
	{Punctuation, ".,/;()+=-\\|<>?:!@#$%^&*{}[]`~"},
	{EndOfLine, "\n"},
	{Nothing, " \t"},
};

const std::map<TokenType, const char*> tokenEscapes = {
	{Identifier, " .,/;()+=-\\|<>?:!@#$%^&*{}[]\"'\n\t\r"},
	{Integer, " ,/;()+=-\\|<>?:!@#$%^&*{}[]\"'\n\t\r"},
	{String, "\""},
	{Punctuation, " ,/;()+=-\\|<>?:!@#$%^&*{}[]\"'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789.\n\t\r"},
	{EndOfLine, " ,/;()+=-\\|<>?:!@#$%^&*{}[]\"'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789.\n\t\r"},
};


// If the char is the first item and the following character is the second item, keep adding to token type
const std::map<const char*, const char*> tokenEscapeCancels = {
	{"+-*:^|!&~<>=", "="},
	{"+-", "+-"},
	{"/", "/*"},
	{"*", "/"},
	{":", ":"},
	{".", "."},
};

// If this and the following character match for the specific token type, immediately end
const std::map<TokenType, const char*> tokenForceEscapes = {
	{Integer, ".."},
};

// If tokenType value contains string, change to new tokenType
std::map<TokenType, std::pair<const char*, TokenType>> tokenContainsSwap = {
	{Integer, {".", Float}},
};

std::map<const std::string, const TokenType> subTokenTypes = {
	// Symbols
	{"(", Left_Paren},
	{")", Right_Paren},
	{"[", Left_Bracket},
	{"]", Right_Bracket},
	{"{", Left_Brace},
	{"}", Right_Brace},
	{",", Comma},
	{".", Dot},
	{"..", Dot_Dot},
	{"+", Plus},
	{"-", Minus},
	{"/", Slash},
	{"*", Star},
	{"#", Hash},
	{":", Colon},
	{"::", Colon_Colon},
	{";", Semi_Colon},
	{"!", Bang},
	{"!=", Bang_Equal},
	{"+=", Plus_Equal},
	{"++", Plus_Plus},
	{"-=", Minus_Equal},
	{"--", Minus_Minus},
	{"*=", Times_Equal},
	{"/=", Slash_Equal},
	{"=", Equal},
	{"==", Equal_Equal},
	{"<", Less},
	{"<=", Less_Equal},
	{">", Greater},
	{">=", Greater_Equal},

	// Keywords
	{"if", If_Statement},
	{"for", For_Statement},
	{"while", While_Statement},
	//{"return", },
	//{"break", },
	//{"continue", },
	//{"switch", },
	//{"case", },
	//{"constant", },
	//{"int", },

	// Literals
	{"true", True_Literal},
	{"false", False_Literal},
};

const std::string tokenAsString(TokenType t)
{
	if (t < LastTokenType)
		return tokenTypeStrings[t];
	else {
		printf("Error: Undefined token type `%d`", t);
		return "UNDEFINED TOKEN TYPE";
	}
}

TokenType currentToken = Nothing;
std::string tokenContent = "";
int tokenize(std::string& rawFile)
{
	// First remove carriage returns if they exist
	std::string output = "";
	for (char c : rawFile) {
		if (c != '\r') {
			output += c;
		}
	}
	rawFile = output;

	// Then start making tokens
	int lineNumber = 1;
	rawFile = "\n" + rawFile + "\n";  // add extra character at end as buffer for lookahead
	for (int i = 1; i < rawFile.size(); i++) {
		char c = rawFile[i];
		char nextChar = rawFile[i + 1];
		char lastChar = rawFile[i - 1];
		//if (lastChar == '\n')
		//	lineNumber++;
		// If no token is building, check what the new one should be
		if (currentToken == Nothing) {
			for (auto const& [tokenType, str] : tokenStarts) {
				if (charInArray(c, str)) {
					if (tokenType == Nothing)
						break;
					currentToken = tokenType;
					tokenContent += c;
					break;
				}
			}
		}
		// If current token is something
		else {
			// Check if it should end
			for (auto const& [tokenType, str] : tokenEscapes) {
				if (tokenType == currentToken)
					if (charInArray(c, str)) {
						// Check if 2 characters create a match to cancel ending this token
						for (auto const& [c1, c2] : tokenEscapeCancels) {
							if (charInArray(lastChar, c1) && charInArray(c, c2)) {
								goto cancelEnd;
							}
						}

					endToken:
						if (currentToken == String) {
							tokenContent += c;
						}
						else
							i--;
						if (currentToken == EndOfLine)
							lineNumber++;

						if (tokenContainsSwap.find(currentToken) != tokenContainsSwap.end()) {
							if (tokenContent.find(tokenContainsSwap[currentToken].first[0]) != std::string::npos)
								currentToken = tokenContainsSwap[currentToken].second;
						}


						// Add tokenContent as element to tokens, and clear it
						allTokens.push_back(tokenPair(tokenContent, currentToken, lineNumber));
						tokenContent = "";

						currentToken = Nothing;
						break;
					}
				// Check if 2 characters create a match to force ending this token early
				for (auto const& [tok, c2] : tokenForceEscapes) {
					if (currentToken == tok)
						if (c == c2[0] && nextChar == c2[1]) {
							goto endToken;
						}
				}
			}
		cancelEnd:
			if (currentToken != Nothing)
				tokenContent += c;
		}
	}
	allTokens.push_back(tokenPair("", EndOfFile, lineNumber + 1));

	return 0;
}

int labelSubTokens(std::vector<tokenPair>& tokens)
{
	for (int i = 0; i < tokens.size(); i++) {
		std::string t = tokens[i].first;
		TokenType tt = tokens[i].second;
		//if (tt != Punctuation)
		//	continue;
		// If token has a known subtype, set TokenType to that instead
		if (subTokenTypes.find(t) != subTokenTypes.end()) {
			const TokenType newType = subTokenTypes[t];
			tokens[i].second = newType;
		}
	}

	return 0;
}

int joinCommentTokens(std::vector<tokenPair>& tokens)
{
	int i = 0;
	bool inComment = false;
	bool multiLineComment = false;
	int startIndex = 0;
	std::string newTokenContents = "";
	while (i < tokens.size() - 1) {
		tokenPair t = NEXT_TOKEN(i);
		if (!inComment) {
			if (t.first == "//" || t.first == "/*") {
				inComment = true;
				if (t.first == "/*")
					multiLineComment = true;
				startIndex = i;
				newTokenContents = t.first + " ";
			}
		}
		else if (inComment) {
			newTokenContents += t.first + " ";
			//tokens.erase(tokens.begin() + i);
			// If end of comment, combine all parts into single token and delete others
			if (multiLineComment) {
				if (t.first == "*/")
					goto endComment;
			}
			else if (t.second == EndOfLine)
				goto endComment;

			continue;

		endComment:
			if (t.second == EndOfLine) {  // If newline, redact last character
				//i--;
				newTokenContents = newTokenContents.substr(0, newTokenContents.length() - 4);
			}
			inComment = false;
			multiLineComment = false;
			tokens[startIndex].first = newTokenContents;
			tokens[startIndex].second = Comment;
			tokens.erase(tokens.begin() + startIndex + 1, tokens.begin() + i);
			i = startIndex + 1;
		}
	}
	return 0;
}

int removeCommentTokens(std::vector<tokenPair>& tokens)
{
	int i = 0;
	while (i < tokens.size() - 1) {
		tokenPair t = NEXT_TOKEN(i);
		if (t.second == Comment) {
			tokens.erase(tokens.begin() + i);
			i--;
		}
	}
	return 0;
}
