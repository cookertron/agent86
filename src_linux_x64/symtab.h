#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>
#include <cstdint>

struct SymbolInfo {
    int64_t value;
    bool is_equ;    // constant (EQU) vs address label
};

class SymbolTable {
public:
    // Returns "" on success, or a diagnostic string if the symbol was already
    // defined with a different value (caller decides whether to warn/error).
    std::string define(const std::string& name, int64_t value, bool is_equ = false);
    bool resolve(const std::string& name, const std::string& scope, int64_t& out_value) const;
    bool isDefined(const std::string& name, const std::string& scope) const;

    void setScope(const std::string& scope) { current_scope_ = scope; }
    const std::string& getScope() const { return current_scope_; }

    // Qualify a local label with scope
    std::string qualify(const std::string& name, const std::string& scope) const;
    std::string qualify(const std::string& name) const { return qualify(name, current_scope_); }

    // Read-only dump of all symbols
    std::vector<std::pair<std::string, SymbolInfo>> dump() const;

private:
    std::unordered_map<std::string, SymbolInfo> symbols_;
    std::string current_scope_;
};
