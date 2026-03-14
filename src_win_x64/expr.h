#pragma once
#include "types.h"
#include "symtab.h"
#include <vector>
#include <string>

struct ExprResult {
    int64_t value = 0;
    bool resolved = true;
};

class ExprEvaluator {
public:
    ExprEvaluator(const SymbolTable& symtab, const std::string& scope, int64_t current_addr)
        : symtab_(symtab), scope_(scope), current_addr_(current_addr) {}

    // Evaluate from a vector of tokens starting at pos
    ExprResult evaluate(const std::vector<Token>& tokens, size_t& pos);

    // Expression-level diagnostics (division by zero, unmatched parens, etc.)
    const std::vector<std::string>& errors() const { return errors_; }

private:
    ExprResult parseOr(const std::vector<Token>& tokens, size_t& pos);
    ExprResult parseXor(const std::vector<Token>& tokens, size_t& pos);
    ExprResult parseAnd(const std::vector<Token>& tokens, size_t& pos);
    ExprResult parseShift(const std::vector<Token>& tokens, size_t& pos);
    ExprResult parseAddSub(const std::vector<Token>& tokens, size_t& pos);
    ExprResult parseMulDiv(const std::vector<Token>& tokens, size_t& pos);
    ExprResult parseUnary(const std::vector<Token>& tokens, size_t& pos);
    ExprResult parseAtom(const std::vector<Token>& tokens, size_t& pos);

    const SymbolTable& symtab_;
    std::string scope_;
    int64_t current_addr_;
    std::vector<std::string> errors_;
};
