#include "symtab.h"
#include "lexer.h"

std::string SymbolTable::qualify(const std::string& name, const std::string& scope) const {
    if (!name.empty() && name[0] == '.') {
        return scope + name; // e.g., "main" + ".loop" → "main.loop"
    }
    return name;
}

std::string SymbolTable::define(const std::string& name, int64_t value, bool is_equ) {
    std::string key = Lexer::toUpper(name);
    auto it = symbols_.find(key);
    std::string diag;
    if (it != symbols_.end() && it->second.value != value) {
        diag = "symbol '" + name + "' redefined (was " +
               std::to_string(it->second.value) + ", now " +
               std::to_string(value) + ")";
    }
    symbols_[key] = {value, is_equ};
    return diag;
}

bool SymbolTable::resolve(const std::string& name, const std::string& scope, int64_t& out_value) const {
    std::string key = Lexer::toUpper(name);

    // If local label, try qualified first
    if (!key.empty() && key[0] == '.') {
        std::string qualified = Lexer::toUpper(scope) + key;
        auto it = symbols_.find(qualified);
        if (it != symbols_.end()) {
            out_value = it->second.value;
            return true;
        }
    }

    // Try as-is (global or already qualified)
    auto it = symbols_.find(key);
    if (it != symbols_.end()) {
        out_value = it->second.value;
        return true;
    }

    return false;
}

bool SymbolTable::isDefined(const std::string& name, const std::string& scope) const {
    int64_t dummy;
    return resolve(name, scope, dummy);
}

std::vector<std::pair<std::string, SymbolInfo>> SymbolTable::dump() const {
    return std::vector<std::pair<std::string, SymbolInfo>>(symbols_.begin(), symbols_.end());
}
