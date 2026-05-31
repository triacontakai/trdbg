#ifndef SYMBOLS_ELF_H_
#define SYMBOLS_ELF_H_

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tdb::symbols {

struct Symbol {
    std::string name;
    // link-time address, PIE executables need their load offset added to get the runtime one
    std::uint64_t value;
    std::uint64_t size;
};

/**
 * Function and variable symbols from an ELF file's .symtab and .dynsym
 * only 64-bit little endian files for now
 */
class ElfSymbols {
public:
    static std::expected<ElfSymbols, std::string> load(const std::string& path);

    std::optional<Symbol> find(std::string_view name) const;
    // the symbol whose range covers value (a link-time address)
    std::optional<Symbol> containing(std::uint64_t value) const;

    // whether it gets loaded at some base address instead of where it was linked
    bool pie() const { return pie_; }
    std::uint64_t entry() const { return entry_; }

private:
    void add(Symbol symbol, bool global);

    bool pie_ = false;
    std::uint64_t entry_ = 0;
    // sorted by value once loading is done
    std::vector<Symbol> symbols_;
    // names to values, preferring global symbols over local ones with the same name
    std::map<std::string, std::pair<Symbol, bool>, std::less<>> by_name_;
};

} // tdb::symbols

#endif
