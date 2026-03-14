#include "expr.h"
#include "lexer.h"

static bool isExprEnd(const Token& t) {
    return t.type == TokenType::EOL ||
           t.type == TokenType::COMMA ||
           t.type == TokenType::CLOSE_BRACKET ||
           t.type == TokenType::CLOSE_PAREN;
}

ExprResult ExprEvaluator::evaluate(const std::vector<Token>& tokens, size_t& pos) {
    return parseOr(tokens, pos);
}

// Level 1: bitwise OR (lowest precedence)
ExprResult ExprEvaluator::parseOr(const std::vector<Token>& tokens, size_t& pos) {
    ExprResult left = parseXor(tokens, pos);
    while (pos < tokens.size() && !isExprEnd(tokens[pos])) {
        if (tokens[pos].type == TokenType::PIPE) {
            pos++;
            ExprResult right = parseXor(tokens, pos);
            left.value |= right.value;
            if (!right.resolved) left.resolved = false;
        } else {
            break;
        }
    }
    return left;
}

// Level 2: bitwise XOR
ExprResult ExprEvaluator::parseXor(const std::vector<Token>& tokens, size_t& pos) {
    ExprResult left = parseAnd(tokens, pos);
    while (pos < tokens.size() && !isExprEnd(tokens[pos])) {
        if (tokens[pos].type == TokenType::CARET) {
            pos++;
            ExprResult right = parseAnd(tokens, pos);
            left.value ^= right.value;
            if (!right.resolved) left.resolved = false;
        } else {
            break;
        }
    }
    return left;
}

// Level 3: bitwise AND
ExprResult ExprEvaluator::parseAnd(const std::vector<Token>& tokens, size_t& pos) {
    ExprResult left = parseShift(tokens, pos);
    while (pos < tokens.size() && !isExprEnd(tokens[pos])) {
        if (tokens[pos].type == TokenType::AMPERSAND) {
            pos++;
            ExprResult right = parseShift(tokens, pos);
            left.value &= right.value;
            if (!right.resolved) left.resolved = false;
        } else {
            break;
        }
    }
    return left;
}

// Level 4: shifts << >>
ExprResult ExprEvaluator::parseShift(const std::vector<Token>& tokens, size_t& pos) {
    ExprResult left = parseAddSub(tokens, pos);
    while (pos < tokens.size() && !isExprEnd(tokens[pos])) {
        if (tokens[pos].type == TokenType::SHL) {
            pos++;
            ExprResult right = parseAddSub(tokens, pos);
            left.value <<= right.value;
            if (!right.resolved) left.resolved = false;
        } else if (tokens[pos].type == TokenType::SHR) {
            pos++;
            ExprResult right = parseAddSub(tokens, pos);
            left.value >>= right.value;
            if (!right.resolved) left.resolved = false;
        } else {
            break;
        }
    }
    return left;
}

// Level 5: addition, subtraction
ExprResult ExprEvaluator::parseAddSub(const std::vector<Token>& tokens, size_t& pos) {
    ExprResult left = parseMulDiv(tokens, pos);
    while (pos < tokens.size() && !isExprEnd(tokens[pos])) {
        if (tokens[pos].type == TokenType::PLUS) {
            pos++;
            ExprResult right = parseMulDiv(tokens, pos);
            left.value += right.value;
            if (!right.resolved) left.resolved = false;
        } else if (tokens[pos].type == TokenType::MINUS) {
            pos++;
            ExprResult right = parseMulDiv(tokens, pos);
            left.value -= right.value;
            if (!right.resolved) left.resolved = false;
        } else {
            break;
        }
    }
    return left;
}

// Level 6: multiplication, division, modulo
ExprResult ExprEvaluator::parseMulDiv(const std::vector<Token>& tokens, size_t& pos) {
    ExprResult left = parseUnary(tokens, pos);
    while (pos < tokens.size() && !isExprEnd(tokens[pos])) {
        if (tokens[pos].type == TokenType::STAR) {
            pos++;
            ExprResult right = parseUnary(tokens, pos);
            left.value *= right.value;
            if (!right.resolved) left.resolved = false;
        } else if (tokens[pos].type == TokenType::SLASH) {
            pos++;
            ExprResult right = parseUnary(tokens, pos);
            if (right.value != 0) {
                left.value /= right.value;
            } else {
                errors_.push_back("division by zero");
            }
            if (!right.resolved) left.resolved = false;
        } else if (tokens[pos].type == TokenType::PERCENT) {
            pos++;
            ExprResult right = parseUnary(tokens, pos);
            if (right.value != 0) {
                left.value %= right.value;
            } else {
                errors_.push_back("modulo by zero");
            }
            if (!right.resolved) left.resolved = false;
        } else {
            break;
        }
    }
    return left;
}

// Level 7: unary minus, bitwise NOT
ExprResult ExprEvaluator::parseUnary(const std::vector<Token>& tokens, size_t& pos) {
    if (pos < tokens.size() && tokens[pos].type == TokenType::MINUS) {
        pos++;
        ExprResult r = parseUnary(tokens, pos);
        r.value = -r.value;
        return r;
    }
    if (pos < tokens.size() && tokens[pos].type == TokenType::TILDE) {
        pos++;
        ExprResult r = parseUnary(tokens, pos);
        r.value = ~r.value;
        return r;
    }
    return parseAtom(tokens, pos);
}

// Level 8: atoms — numbers, symbols, $, parenthesized expressions
ExprResult ExprEvaluator::parseAtom(const std::vector<Token>& tokens, size_t& pos) {
    if (pos >= tokens.size()) return {0, false};

    const Token& t = tokens[pos];

    // Number literal
    if (t.type == TokenType::NUMBER && t.numval != -1) {
        pos++;
        return {t.numval, true};
    }

    // Character literal
    if (t.type == TokenType::CHAR_LITERAL) {
        pos++;
        return {t.numval, true};
    }

    // Dollar sign = current address
    if (t.type == TokenType::DOLLAR) {
        pos++;
        return {current_addr_, true};
    }

    // Symbol reference (label, EQU, etc.)
    if (t.type == TokenType::NUMBER && t.numval == -1) {
        pos++;
        int64_t val;
        if (symtab_.resolve(t.text, scope_, val)) {
            return {val, true};
        }
        return {0, false}; // unresolved forward reference
    }

    // Parenthesized expression
    if (t.type == TokenType::OPEN_PAREN) {
        pos++; // consume '('
        ExprResult r = evaluate(tokens, pos);
        // consume ')'
        if (pos < tokens.size() && tokens[pos].type == TokenType::CLOSE_PAREN) {
            pos++;
        } else {
            errors_.push_back("unmatched '(' — expected closing ')'");
        }
        return r;
    }

    // Unknown token
    errors_.push_back("unexpected token '" + t.text + "' in expression");
    pos++;
    return {0, false};
}
