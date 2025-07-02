#pragma once

#include <string>
#include <utility>
#include <vector>

#include "tokenizer.h"

int beginParse(const std::vector<std::pair<std::string, TokenType>>& tokens);
int generateAST(const std::vector<std::pair<std::string, TokenType>>& tokens);
