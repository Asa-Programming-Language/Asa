#include "tokenizer.h"

std::vector<std::pair<std::string, TokenType>> tokens = std::vector<std::pair<std::string, TokenType>>();

bool charInArray(char x, const char* a)
{
	for (int i = 0; a[i] != '\0'; i++) {
		if (a[i] == x)
			return true;
	}
	return false;
}


const std::map<TokenType, const char*> tokenStarts = {
	{Identifier, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_"},
	{Number, "0123456789"},
	{String, "\""},
	{Punctuation, ".,/;()+=-\\|<>?:!@#$%^&*{}[]`~"},
	{Nothing, " \n"},
};

const std::map<TokenType, const char*> tokenEscapes = {
	{Identifier, " .,/;()+=-\\|<>?:!@#$%^&*{}[]\"'\n"},
	{Number, " ,/;()+=-\\|<>?:!@#$%^&*{}[]\"'\n"},
	{String, "\""},
	{Punctuation, " ,/;()+=-\\|<>?:!@#$%^&*{}[]\"'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789.\n"},
};


// If the char is the first item and the following character is the second item, keep adding to token type
const std::map<const char*, const char*> tokenEscapeCancels = {
	{"+-*:^|!&~<>=", "="},
	{"/", "/*"},
	{"*", "/"},
};

// If this and the following character match for the specific token type, immediately end
const std::map<TokenType, const char*> tokenForceEscapes = {
	{Number, ".."},
};

std::map<const std::string, const TokenType> subTokenTypes = {
	{"(", Left_Paren},
	{")", Right_Paren},
	{"[", Left_Bracket},
	{"]", Right_Bracket},
	{"{", Left_Brace},
	{"}", Right_Brace},
	{",", Comma},
	{".", Dot},
	{"+", Plus},
	{"-", Minus},
	{"/", Slash},
	{"*", Star},
	{":", Colon},
	{";", Semi_Colon},
	{"!", Bang},
	{"!=", Bang_Equal},
	{"=", Equal},
	{"==", Equal_Equal},
	{"<", Less},
	{"<=", Less_Equal},
	{">", Greater},
	{">=", Greater_Equal},
};

const std::string tokenAsString(TokenType t)
{
	return tokenTypeStrings[t];
}

TokenType currentToken = Nothing;
std::string tokenContent = "";
int tokenize(std::string& rawFile)
{
	rawFile = "\n" + rawFile + "\n";  // add extra character at end as buffer for lookahead
	for (int i = 1; i < rawFile.size(); i++) {
		char c = rawFile[i];
		char nextChar = rawFile[i + 1];
		char lastChar = rawFile[i - 1];
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


						// Add tokenContent as element to tokens, and clear it
						tokens.push_back(std::make_pair(tokenContent, currentToken));
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

	return 0;
}

int labelSubTokens(std::vector<std::pair<std::string, TokenType>>& tokens)
{
	for (int i = 0; i < tokens.size(); i++) {
		std::string t = tokens[i].first;
		TokenType tt = tokens[i].second;
		if (tt != Punctuation)
			continue;
		// If token has a known subtype, set TokenType to that instead
		if (subTokenTypes.find(t) != subTokenTypes.end()) {
			const TokenType newType = subTokenTypes[t];
			tokens[i] = std::make_pair(t, newType);
		}
	}

	return 0;
}
