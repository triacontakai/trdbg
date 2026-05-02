#ifndef PARSE_H_
#define PARSE_H_

#include <charconv>
#include <concepts>
#include <optional>
#include <string_view>
#include <system_error>

namespace tdb {

// decimal, 0x hex, or negative (two's complement)
template<std::unsigned_integral T>
std::optional<T> parse_integer(std::string_view s) {
    bool negative = s.starts_with('-');
    if (negative)
        s.remove_prefix(1);

    int base = 10;
    if (s.starts_with("0x") || s.starts_with("0X")) {
        base = 16;
        s.remove_prefix(2);
    }

    T value;
    auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (err != std::errc() || end != s.data() + s.size())
        return std::nullopt;

    return negative ? static_cast<T>(-value) : value;
}

} // tdb

#endif
