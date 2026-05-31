#include <algorithm>
#include <cstring>
#include <elf.h>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>
#include "elf.h"

namespace tdb::symbols {

namespace {
// everything read out of the file goes through these, since it can be truncated or just garbage

// copies a T out of the file, empty if it doesn't fit
template<typename T>
std::optional<T> read_at(std::span<const char> file, std::uint64_t offset) {
    if (offset > file.size() || file.size() - offset < sizeof(T))
        return std::nullopt;

    T value;
    std::memcpy(&value, file.data() + offset, sizeof(T));
    return value;
}

// NUL terminated string at index in a string table section
std::optional<std::string_view> read_string(std::span<const char> file, const Elf64_Shdr& strtab, std::uint64_t index) {
    if (index >= strtab.sh_size || strtab.sh_offset > file.size())
        return std::nullopt;

    std::uint64_t start = strtab.sh_offset + index;
    std::uint64_t end = std::min<std::uint64_t>(file.size(), strtab.sh_offset + strtab.sh_size);
    if (start >= end)
        return std::nullopt;

    auto nul = std::find(file.begin() + start, file.begin() + end, '\0');
    if (nul == file.begin() + end)
        return std::nullopt;

    return std::string_view(file.data() + start, nul - (file.begin() + start));
}
} // anonymous namespace

auto ElfSymbols::load(const std::string& path) -> std::expected<ElfSymbols, std::string> {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::unexpected(std::format("can't open {}", path));

    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::span<const char> file(data);

    auto header = read_at<Elf64_Ehdr>(file, 0);
    if (!header || std::memcmp(header->e_ident, ELFMAG, SELFMAG) != 0)
        return std::unexpected(std::format("{} isn't an ELF file", path));
    if (header->e_ident[EI_CLASS] != ELFCLASS64 || header->e_ident[EI_DATA] != ELFDATA2LSB)
        return std::unexpected(std::format("{} isn't a 64-bit little endian ELF file", path));

    ElfSymbols result;
    result.pie_ = header->e_type == ET_DYN;
    result.entry_ = header->e_entry;

    std::vector<Elf64_Shdr> sections;
    for (std::uint64_t i = 0; i < header->e_shnum; i++) {
        auto section = read_at<Elf64_Shdr>(file, header->e_shoff + i * sizeof(Elf64_Shdr));
        if (!section)
            return std::unexpected(std::format("{} has truncated section headers", path));
        sections.push_back(*section);
    }

    for (auto const& section : sections) {
        if (section.sh_type != SHT_SYMTAB && section.sh_type != SHT_DYNSYM)
            continue;
        if (section.sh_entsize != sizeof(Elf64_Sym) || section.sh_link >= sections.size())
            continue;

        auto const& strtab = sections[section.sh_link];
        for (std::uint64_t offset = 0; offset + sizeof(Elf64_Sym) <= section.sh_size; offset += sizeof(Elf64_Sym)) {
            auto sym = read_at<Elf64_Sym>(file, section.sh_offset + offset);
            if (!sym)
                break;

            // only things defined in this file that you'd want to break on or look at
            int type = ELF64_ST_TYPE(sym->st_info);
            if ((type != STT_FUNC && type != STT_OBJECT) || sym->st_shndx == SHN_UNDEF)
                continue;

            auto name = read_string(file, strtab, sym->st_name);
            if (!name || name->empty())
                continue;

            result.add(Symbol{std::string(*name), sym->st_value, sym->st_size}, ELF64_ST_BIND(sym->st_info) != STB_LOCAL);
        }
    }

    std::ranges::sort(result.symbols_, {}, &Symbol::value);
    return result;
}

void ElfSymbols::add(Symbol symbol, bool global) {
    // .symtab and .dynsym repeat a lot of the same symbols
    // check to make sure symbol wasn't already added
    auto found = by_name_.find(symbol.name);
    if (found != by_name_.end()) {
        auto& [existing, existing_global] = found->second;
        if (existing.value == symbol.value)
            return;
        if (global && !existing_global)
            found->second = {symbol, global};
    } else {
        by_name_.emplace(symbol.name, std::pair{symbol, global});
    }

    symbols_.push_back(std::move(symbol));
}

auto ElfSymbols::find(std::string_view name) const -> std::optional<Symbol> {
    if (auto found = by_name_.find(name); found != by_name_.end())
        return found->second.first;
    return std::nullopt;
}

auto ElfSymbols::containing(std::uint64_t value) const -> std::optional<Symbol> {
    // look through the symbols starting at the closest address at or before value
    auto it = std::ranges::upper_bound(symbols_, value, {}, &Symbol::value);
    if (it == symbols_.begin())
        return std::nullopt;

    std::uint64_t start = std::prev(it)->value;
    while (it != symbols_.begin() && std::prev(it)->value == start) {
        --it;
        // zero sized symbols only cover their own address
        if (value - it->value < std::max<std::uint64_t>(it->size, 1))
            return *it;
    }

    return std::nullopt;
}

} // tdb::symbols
