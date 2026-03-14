#pragma once
#include "types.h"
#include <vector>
#include <string>

class Lexer {
public:
    std::vector<Token> tokenize(const std::string& line);
    static std::string toUpper(const std::string& s);
    static bool isRegister(const std::string& name);
    static bool isMnemonic(const std::string& name);
    static bool isDirective(const std::string& name);
    static bool isSizeKeyword(const std::string& name);
private:
    Token readNumber(const std::string& line, size_t& pos);
    Token readString(const std::string& line, size_t& pos);
    Token readIdentifier(const std::string& line, size_t& pos);
};
